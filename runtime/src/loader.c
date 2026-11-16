/* Loads the guest executable (main.elf produced by triage.py), applies relocations,
 * binds imports to the HLE layer, and builds the guest-address -> host-function map. */
#include "xrt_internal.h"
#include <string.h>

Image g_img;
uint64_t G_BASE;
uint64_t g_dyn_off;

typedef struct { uint32_t off; GuestFn fn; } FnEntry;
extern const unsigned xrt_fn_count;
extern const FnEntry xrt_fn_table[];
static GuestFn *g_fnmap;      /* direct map: (addr - G_BASE) / 4 -> fn */

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;

int xrt_load_image(const char *path) {
    size_t len;
    uint8_t *f = read_file(path, &len);
    if (!f) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    uint64_t phoff; memcpy(&phoff, f + 32, 8);
    uint16_t phnum; memcpy(&phnum, f + 56, 2);
    uint64_t end = 0, dyn = 0;
    struct { uint32_t type, flags; uint64_t off, va, pa, fsz, msz, align; } ph[8];
    for (int i = 0; i < phnum && i < 8; i++) {
        memcpy(&ph[i], f + phoff + i * 56, 56);
        if (ph[i].type == 1 && ph[i].va + ph[i].msz > end) end = ph[i].va + ph[i].msz;
        if (ph[i].type == 2) dyn = ph[i].va;
    }
    g_img.size = (end + 0xfff) & ~0xfffULL;
    g_img.text_size = ph[0].fsz;
    g_img.base = plat_alloc(g_img.size);
    G_BASE = (uint64_t)(uintptr_t)g_img.base;
    for (int i = 0; i < phnum; i++)
        if (ph[i].type == 1) memcpy(g_img.base + ph[i].va, f + ph[i].off, ph[i].fsz);
    free(f);
    LOG(1, "[load] %s at %p (%llu MB, text %llu MB)", path, g_img.base,
        (unsigned long long)(g_img.size >> 20), (unsigned long long)(g_img.text_size >> 20));

    /* dynamic section */
    g_dyn_off = dyn;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, symtab = 0, strtab = 0;
    for (uint64_t o = dyn;; o += 16) {
        int64_t tag; uint64_t val;
        memcpy(&tag, g_img.base + o, 8); memcpy(&val, g_img.base + o + 8, 8);
        if (tag == 0) break;
        switch (tag) {
        case 7: rela = val; break;       case 8: relasz = val; break;
        case 23: jmprel = val; break;    case 2: pltrelsz = val; break;
        case 6: symtab = val; break;     case 5: strtab = val; break;
        case 25: g_img.init_array = val; break;
        case 27: g_img.init_array_count = val / 8; break;
        }
    }
    Sym *syms = (Sym *)(g_img.base + symtab);
    const char *strs = (const char *)(g_img.base + strtab);
    unsigned imports = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t tab = pass ? jmprel : rela, sz = pass ? pltrelsz : relasz;
        for (uint64_t i = 0; i < sz / 24; i++) {
            uint64_t off, info; int64_t add;
            memcpy(&off, g_img.base + tab + i * 24, 8);
            memcpy(&info, g_img.base + tab + i * 24 + 8, 8);
            memcpy(&add, g_img.base + tab + i * 24 + 16, 8);
            uint32_t type = (uint32_t)info, si = (uint32_t)(info >> 32);
            uint64_t *where = (uint64_t *)(g_img.base + off), v;
            if (type == 1027) {                                   /* RELATIVE */
                v = G_BASE + (uint64_t)add;
            } else if (type == 1025 || type == 1026 || type == 257) { /* GLOB_DAT, JUMP_SLOT, ABS64 */
                Sym *s = &syms[si];
                if (s->shndx) v = G_BASE + s->value + (uint64_t)add;
                else {
                    uint64_t d = xrt_donor_data(strs + s->name);
                    v = (d ? d : hle_resolve(strs + s->name)) + (uint64_t)add; imports++;
                }
            } else {
                fprintf(stderr, "[load] unsupported relocation type %u\n", type);
                continue;
            }
            memcpy(where, &v, 8);
        }
    }
    /* exported entry points */
    for (uint64_t i = 1; (uint8_t *)&syms[i] < (uint8_t *)strs; i++) {
        const char *n = strs + syms[i].name;
        if (!syms[i].shndx) continue;
        if (!strcmp(n, "nnMain")) g_img.sym_nnMain = G_BASE + syms[i].value;
        if (!strcmp(n, "nninitStartup")) g_img.sym_nninitStartup = G_BASE + syms[i].value;
    }
    LOG(1, "[load] relocations applied, %u import slots bound to HLE, %llu static initializers",
        imports, (unsigned long long)g_img.init_array_count);
    return 1;
}

void xrt_build_fn_map(void) {
    g_fnmap = plat_alloc((g_img.text_size / 4 + 1) * sizeof(GuestFn));
    for (unsigned i = 0; i < xrt_fn_count; i++) g_fnmap[xrt_fn_table[i].off / 4] = xrt_fn_table[i].fn;
    LOG(1, "[load] %u recompiled functions mapped", xrt_fn_count);
}

/* ---------------------------------------------------------------- dispatch */
static _Thread_local uint64_t t_last_dispatch[16];
static _Thread_local unsigned t_ld;
_Thread_local Ctx *t_ctx;          /* guest context of the last indirect call on this thread (crash reports) */

/* watchdog registry: every thread that runs guest code, for XRT_WATCHDOG=seconds dumps */
typedef struct { uint64_t *ring; unsigned *ld; Ctx **ctx; } ThreadDbg;
static ThreadDbg g_tdbg[256]; static volatile int g_ntdbg;
static _Thread_local int t_registered;
void xrt_watchdog_dump(void) {
    fprintf(stderr, "[watchdog] %d threads\n", g_ntdbg);
    for (int i = 0; i < g_ntdbg; i++) {
        ThreadDbg *d = &g_tdbg[i];
        unsigned ld = *d->ld;
        fprintf(stderr, "  T%-3d lr=main+0x%llx  last:", i, (unsigned long long)(*d->ctx ? (*d->ctx)->x[30] - G_BASE : 0));
        for (unsigned k = 6; k >= 1; k--) {
            uint64_t t = d->ring[(ld - k) & 15];
            if (!t) continue;
            if (t >= HLE_BASE) fprintf(stderr, " [%s]", hle_name((t - HLE_BASE) >> 2));
            else fprintf(stderr, " %llx", (unsigned long long)(t - G_BASE));
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}
void xrt_dispatch(Ctx *c, uint64_t t) {
    t_last_dispatch[t_ld++ & 15] = t;
    t_ctx = c;
    if (!t_registered) {
        t_registered = 1;
        int i = __atomic_fetch_add(&g_ntdbg, 1, __ATOMIC_SEQ_CST);
        if (i < 256) { g_tdbg[i].ring = t_last_dispatch; g_tdbg[i].ld = &t_ld; g_tdbg[i].ctx = &t_ctx; } else g_ntdbg = 256;
    }
    if (t - G_BASE < g_img.text_size) {
        GuestFn fn = g_fnmap[(t - G_BASE) >> 2];
        if (!fn) {
            FILE *f = fopen("extra_entries.txt", "a");
            if (f) { fprintf(f, "%llx\n", (unsigned long long)(t - G_BASE)); fclose(f); }
            FATAL("indirect jump to main+0x%llx which is not a function start (appended to the game folder's "
                  "extra_entries.txt: rebuild to include it)", (unsigned long long)(t - G_BASE));
        }
        fn(c);
        return;
    }
    if (t >= HLE_BASE) { hle_call(c, (t - HLE_BASE) >> 2); return; }
    FATAL("indirect jump to non-code address 0x%llx", (unsigned long long)t);
}

void xrt_trap(Ctx *c, uint64_t pc, const char *why) {
    FATAL("guest trap '%s' at main+0x%llx (x0=0x%llx x1=0x%llx lr=main+0x%llx)", why,
          (unsigned long long)(pc - G_BASE), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
          (unsigned long long)(c->x[30] - G_BASE));
}

void xrt_unimpl(Ctx *c, uint64_t pc, const char *text) {
    FATAL("unimplemented instruction at main+0x%llx: %s", (unsigned long long)(pc - G_BASE), text);
}

void xrt_backtrace(void) {
    fprintf(stderr, "  recent indirect targets:");
    for (unsigned i = 0; i < 16; i++) {
        uint64_t t = t_last_dispatch[(t_ld + i) & 15];
        if (!t) continue;
        if (t >= HLE_BASE) fprintf(stderr, " [%s]", hle_name((t - HLE_BASE) >> 2));
        else fprintf(stderr, " main+0x%llx", (unsigned long long)(t - G_BASE));
    }
    fputc('\n', stderr);
}

/* ---------------------------------------------------------------- host -> guest calls */
uint64_t xrt_call(Ctx *c, uint64_t fn, int nargs, ...) {
    uint64_t save[31]; V128 v0 = c->v[0];
    memcpy(save, c->x, sizeof save);
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 8; i++) c->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    c->x[30] = 0;
    xrt_dispatch(c, fn);
    uint64_t r = c->x[0];
    memcpy(c->x, save, sizeof save);
    c->v[0] = v0;
    (void)v0;
    return r;
}

Ctx *xrt_new_ctx(uint64_t stack_size) {
    Ctx *c = plat_alloc(sizeof(Ctx));
    uint8_t *stack = plat_alloc(stack_size);
    c->x[31] = ((uint64_t)(uintptr_t)(stack + stack_size)) & ~0xfULL;
    return c;
}

/* ---------------------------------------------------------------- exports + data donor */
uint64_t xrt_find_export(const char *name) {
    /* re-walk dynsym of the loaded image */
    uint64_t symtab = 0, strtab = 0;
    for (uint64_t o = g_dyn_off;; o += 16) {
        int64_t tag; uint64_t val;
        memcpy(&tag, g_img.base + o, 8); memcpy(&val, g_img.base + o + 8, 8);
        if (!tag) break;
        if (tag == 6) symtab = val;
        if (tag == 5) strtab = val;
    }
    Sym *syms = (Sym *)(g_img.base + symtab);
    const char *strs = (const char *)(g_img.base + strtab);
    for (uint64_t i = 1; (uint8_t *)&syms[i] < (uint8_t *)strs; i++)
        if (syms[i].shndx && !strcmp(strs + syms[i].name, name)) return G_BASE + syms[i].value;
    FATAL("export %s not found", name);
}

/* sdk.elf is never executed; it only donates the initial values of data objects the game
 * imports from the SDK (e.g. nn::util float constants). */
static uint8_t *g_donor; static uint64_t g_donor_sym, g_donor_str;
int xrt_load_donor(const char *path) {
    size_t len; uint8_t *f = read_file(path, &len);
    if (!f) return 0;
    g_donor = f + 0x1000;
    uint64_t phoff; memcpy(&phoff, f + 32, 8);
    uint16_t phnum; memcpy(&phnum, f + 56, 2);
    uint64_t dyn = 0;
    for (int i = 0; i < phnum; i++) { uint32_t t; memcpy(&t, f + phoff + i * 56, 4); if (t == 2) memcpy(&dyn, f + phoff + i * 56 + 16, 8); }
    for (uint64_t o = dyn;; o += 16) {
        int64_t tag; uint64_t val; memcpy(&tag, g_donor + o, 8); memcpy(&val, g_donor + o + 8, 8);
        if (!tag) break;
        if (tag == 6) g_donor_sym = val;
        if (tag == 5) g_donor_str = val;
    }
    return 1;
}
uint64_t xrt_donor_data(const char *name) {
    if (!g_donor) return 0;
    Sym *syms = (Sym *)(g_donor + g_donor_sym);
    const char *strs = (const char *)(g_donor + g_donor_str);
    for (uint64_t i = 1; (uint8_t *)&syms[i] < (uint8_t *)strs; i++) {
        if ((syms[i].info & 15) != 1 || !syms[i].shndx || strcmp(strs + syms[i].name, name)) continue;
        uint8_t *copy = plat_alloc(syms[i].size + 16);
        memcpy(copy, g_donor + syms[i].value, syms[i].size);
        LOG(2, "[load] data import %s (%llu bytes) from sdk", name, (unsigned long long)syms[i].size);
        return (uint64_t)(uintptr_t)copy;
    }
    return 0;
}

/* ---------------------------------------------------------------- crash reports */
/* host code address -> "f_xxxxxxx+0xNN" (the recompiled function containing it) */
static void describe_host_pc(uintptr_t pc, char *out, size_t n) {
    uintptr_t best = 0; uint32_t best_off = 0;
    for (unsigned i = 0; i < xrt_fn_count; i++) {
        uintptr_t f = (uintptr_t)xrt_fn_table[i].fn;
        if (f <= pc && f > best) { best = f; best_off = xrt_fn_table[i].off; }
    }
    if (best && pc - best < (1u << 20)) snprintf(out, n, "recompiled f_%07x (main+0x%x) + host 0x%llx", best_off, best_off, (unsigned long long)(pc - best));
    else snprintf(out, n, "runtime/host code at %p", (void *)pc);
}

void xrt_describe_host_pc(uintptr_t pc, char *out, size_t n) { describe_host_pc(pc, out, n); }
void xrt_crash_report(const char *what, uintptr_t host_pc, uintptr_t fault_addr) {
    char where[160];
    describe_host_pc(host_pc, where, sizeof where);
    fprintf(stderr, "\n==== CRASH: %s ====\n  at %s\n  fault address: 0x%llx\n", what, where, (unsigned long long)fault_addr);
    Ctx *c = t_ctx;
    if (c) {
        fprintf(stderr, "  guest registers (last indirect call on this thread):\n");
        for (int i = 0; i < 31; i++) {
            uint64_t v = c->x[i];
            fprintf(stderr, "    x%-2d = 0x%016llx%s", i, (unsigned long long)v, (i % 3 == 2) ? "\n" : "");
        }
        fprintf(stderr, "\n    sp  = 0x%016llx   lr = main+0x%llx\n", (unsigned long long)c->x[31], (unsigned long long)(c->x[30] - G_BASE));
    }
    xrt_backtrace();
    fflush(stderr);
}

/* entry hooks (recomp/hooks.txt) */
typedef void (*XrtHookFn)(Ctx *c);
static struct { uint64_t func; XrtHookFn fn; } g_hooks[32]; static int g_nhooks;
void xrt_register_hook(uint64_t func, void (*fn)(Ctx *c)) { if (g_nhooks < 32) { g_hooks[g_nhooks].func = func; g_hooks[g_nhooks].fn = fn; g_nhooks++; } }
void xrt_hook(Ctx *c, uint64_t func) { for (int i = 0; i < g_nhooks; i++) if (g_hooks[i].func == func) g_hooks[i].fn(c); }
/* hooks that may answer for the function: nonzero = return at once (x0 holds the result) */
typedef int (*XrtHookRetFn)(Ctx *c);
static struct { uint64_t func; XrtHookRetFn fn; } g_rhooks[16]; static int g_nrhooks;
void xrt_register_hook_ret(uint64_t func, int (*fn)(Ctx *c)) { if (g_nrhooks < 16) { g_rhooks[g_nrhooks].func = func; g_rhooks[g_nrhooks].fn = fn; g_nrhooks++; } }
int xrt_hook_ret(Ctx *c, uint64_t func) {
    xrt_hook(c, func);
    for (int i = 0; i < g_nrhooks; i++) if (g_rhooks[i].func == func && g_rhooks[i].fn(c)) return 1;
    return 0;
}
void xrt_trace(Ctx *c, uint64_t func) {
    fprintf(stderr, "[trace] main+0x%llx(x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx) lr=main+0x%llx\n",
            (unsigned long long)func, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
            (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)(c->x[30] - G_BASE));
    /* XRT_TRACE_STR=off: print the C string at x0+off */
    static long str_off = -2;
    if (str_off == -2) { const char *e = getenv("XRT_TRACE_STR"); str_off = e ? strtol(e, 0, 0) : -1; }
    static unsigned long long str_func;
    if (str_off == -1 && 0) {}
    { static int once; if (!once) { once = 1; const char *e = getenv("XRT_TRACE_STR"); if (e && strchr(e, ',')) str_func = strtoull(strchr(e, ',') + 1, 0, 16); } }
    if (str_off >= 0 && c->x[0] > 0x10000 && (!str_func || str_func == func)) fprintf(stderr, "    str: '%.64s'\n", (const char *)(uintptr_t)(c->x[0] + str_off));
    /* XRT_TRACE_DUMP=size: hex+ascii dump of [x0, x0+size) */
    static long dump_sz = -2;
    if (dump_sz == -2) { const char *e = getenv("XRT_TRACE_DUMP"); dump_sz = e ? strtol(e, 0, 0) : -1; }
    if (dump_sz > 0 && c->x[0] > 0x10000) {
        for (long o = 0; o < dump_sz; o += 16) {
            fprintf(stderr, "    +%03lx:", o);
            for (int k = 0; k < 16; k++) fprintf(stderr, " %02x", LD8(c->x[0] + o + k));
            fprintf(stderr, "  ");
            for (int k = 0; k < 16; k++) { uint8_t ch = LD8(c->x[0] + o + k); fputc(ch >= 32 && ch < 127 ? ch : '.', stderr); }
            fputc('\n', stderr);
        }
    }
    /* XRT_TRACE_VEC=off,stride: also dump the std::vector at [x0+off] (begin,end) with the given element size */
    static int vec_off = -2, vec_stride; static unsigned long long vec_func;
    if (vec_off == -2) { const char *e = getenv("XRT_TRACE_VEC"); vec_off = -1; if (e) sscanf(e, "%x,%x,%llx", &vec_off, &vec_stride, &vec_func); }
    if (vec_off >= 0 && c->x[0] > 0x10000 && (!vec_func || vec_func == func)) {
        uint64_t b = LD64(c->x[0] + vec_off), e = LD64(c->x[0] + vec_off + 8);
        fprintf(stderr, "  vector [%llx..%llx) %llu elements\n", (unsigned long long)b, (unsigned long long)e,
                (unsigned long long)(vec_stride ? (e - b) / vec_stride : 0));
        for (uint64_t p = b; vec_stride && p < e && p < b + 64ULL * vec_stride; p += vec_stride) {
            fprintf(stderr, "   ");
            for (int k = 0; k < vec_stride; k += 4) fprintf(stderr, " %08x", LD32(p + k));
            fprintf(stderr, "\n");
        }
    }
}
