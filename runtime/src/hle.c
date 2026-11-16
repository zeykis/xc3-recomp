/* HLE layer: import binding + implementations of C library functions the game imports.
 * Every import gets an index; unimplemented imports log once and return 0, so we can see
 * exactly which SDK functions the game needs next (run with XRT_STRICT=1 to stop instead). */
#include "xrt_internal.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#define MAX_IMPORTS 4096
static struct { const char *name; HleFn fn; uint64_t calls; } g_imp[MAX_IMPORTS];
static unsigned g_nimp;
int g_log_level = 1;
volatile uint64_t g_nvn_frames;      /* presented frames (hle_nvn.cpp), for frame-ranged traces */
static int g_strict = -1;

extern const HleImpl hle_libc[], hle_nn[], hle_cxx[], hle_fiber[], hle_audio[], hle_nvn[], hle_movie[], hle_image[];

static HleFn find_impl(const char *name) {
    const HleImpl *tabs[] = { hle_libc, hle_nn, hle_cxx, hle_fiber, hle_audio, hle_nvn, hle_movie, hle_image };
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (const HleImpl *p = tabs[t]; p->name; p++)
            if (!strcmp(p->name, name)) return p->fn;
    return NULL;
}

static volatile int g_resolve_lock;
uint64_t hle_resolve(const char *name) {        /* thread-safe: NVN/vtable entries are added at runtime */
    while (__atomic_exchange_n(&g_resolve_lock, 1, __ATOMIC_ACQUIRE)) { }
    uint64_t r = 0;
    for (unsigned i = 0; i < g_nimp; i++)
        if (!strcmp(g_imp[i].name, name)) { r = HLE_BASE + i * 4; break; }
    if (!r) {
        if (g_nimp >= MAX_IMPORTS) FATAL("too many imports");
        g_imp[g_nimp].name = name;
        g_imp[g_nimp].fn = find_impl(name);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        r = HLE_BASE + g_nimp * 4;
        __atomic_store_n(&g_nimp, g_nimp + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_resolve_lock, 0, __ATOMIC_RELEASE);
    return r;
}

const char *hle_name(uint64_t idx) { return idx < g_nimp ? g_imp[idx].name : "?"; }

/* XRT_PROF_HLE=1: time spent in each SDK function on the game's render thread (the one that presents), reported
   with the frame profile: what the frame waits on (sleeps, events, vsync) and what it spins on (many cheap calls). */
static int g_prof_hle = -1;
static _Thread_local int t_render_thread;
static int g_prof_all;
#define HLE_PROF_MAX 65536   /* (entries are also registered later: movie / Opus vtable slots) */
static uint64_t *g_hle_ns, *g_hle_n;
void xrt_mark_render_thread(void) { t_render_thread = 1; }
/* XRT_PROF_HLE=3: per guest thread, how much of the frame it runs (wall time minus time blocked in SDK waits:
   events, timers, condition variables, mutexes, sleeps, fiber switches, nvnSyncWait) - which thread is the frame's
   critical path. */
#define PT_MAX 256
static struct { char name[40]; uint64_t wait_ns, start_ns, in_wait; } g_pt[PT_MAX];   /* in_wait: start of the wait in progress */
static int g_pt_n;
static _Thread_local int t_pt = -1;
static _Thread_local const char *t_pt_name; static _Thread_local uint64_t t_pt_entry;
static uint8_t *g_is_wait;   /* per SDK function: 0 not yet classified, 1 runs, 2 blocks */
void hle_prof_thread_start(const char *name, uint64_t entry) { t_pt_name = name; t_pt_entry = entry; }
static int pt_slot(void) {
    if (t_pt >= 0) return t_pt;
    int i = __atomic_fetch_add(&g_pt_n, 1, __ATOMIC_RELAXED);
    if (i >= PT_MAX) { t_pt = PT_MAX; return t_pt; }
    if (t_pt_entry) snprintf(g_pt[i].name, sizeof g_pt[i].name, "%.20s@%llx", t_pt_name && *t_pt_name ? t_pt_name : "", (unsigned long long)(t_pt_entry - (uint64_t)(uintptr_t)g_img.base));
    else snprintf(g_pt[i].name, sizeof g_pt[i].name, "main/other");
    g_pt[i].start_ns = plat_ticks_ns();
    return t_pt = i;
}
static int is_wait(uint64_t idx) {
    if (!g_is_wait[idx]) {
        const char *n = g_imp[idx].name;
        g_is_wait[idx] = (strstr(n, "Wait") || strstr(n, "Sleep") || strstr(n, "LockMutex") || strstr(n, "SwitchToFiber") ||
                          strstr(n, "ReceiveMessageQueue") || strstr(n, "SendMessageQueue") || strstr(n, "AcquireSemaphore") ||
                          strstr(n, "WaitAny") || strstr(n, "Join")) && !strstr(n, "TryWait") ? 2 : 1;
    }
    return g_is_wait[idx] == 2;
}
static void pt_report(double frames) {
    static uint64_t last; uint64_t now = plat_ticks_ns();
    if (!last) { last = now; return; }
    double wall = (double)(now - last); last = now;
    int n = g_pt_n < PT_MAX ? g_pt_n : PT_MAX;
    double busy[PT_MAX];
    for (int i = 0; i < n; i++) {
        double w = wall; if (g_pt[i].start_ns > now - (uint64_t)wall) w = (double)(now - g_pt[i].start_ns);
        double wt = (double)__atomic_exchange_n(&g_pt[i].wait_ns, 0, __ATOMIC_RELAXED);
        uint64_t iw = __atomic_load_n(&g_pt[i].in_wait, __ATOMIC_RELAXED);   /* blocked right now: count up to now */
        if (iw && __atomic_compare_exchange_n(&g_pt[i].in_wait, &iw, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) wt += (double)(now - iw);
        busy[i] = w - wt;
    }
    LOG(1, "[prof] busiest guest threads (ms/frame running, frame = %.2f ms):", wall / 1e6 / frames);
    for (int k = 0; k < 10; k++) {
        int bi = -1; for (int i = 0; i < n; i++) if (busy[i] > 0 && (bi < 0 || busy[i] > busy[bi])) bi = i;
        if (bi < 0) break;
        LOG(1, "[prof]   %7.2f  %s", busy[bi] / 1e6 / frames, g_pt[bi].name);
        busy[bi] = 0;
    }
}
static void lr_report(double frames);
void hle_prof_report(double frames) {
    if (g_prof_hle != 1 || !g_hle_ns || frames <= 0) return;
    if (g_prof_all == 3) { pt_report(frames); return; }
    uint64_t tot = 0;
    uint64_t n = g_nimp < HLE_PROF_MAX ? g_nimp : HLE_PROF_MAX;
    for (uint64_t i = 0; i < n; i++) tot += g_hle_ns[i];
    LOG(1, "[prof] %s: %.2f ms/frame in SDK functions; top:", g_prof_all ? "all threads" : "render thread", tot / 1e6 / frames);
    for (int k = 0; k < (g_prof_all ? 20 : 12); k++) {
        uint64_t best = 0, bi = 0;
        for (uint64_t i = 0; i < n; i++) if (g_hle_ns[i] > best) { best = g_hle_ns[i]; bi = i; }
        if (!best) break;
        LOG(1, "[prof]   %7.3f ms  %8.1f calls  %s", best / 1e6 / frames, g_hle_n[bi] / frames, g_imp[bi].name);
        g_hle_ns[bi] = 0;
    }
    lr_report(frames);
    memset(g_hle_ns, 0, HLE_PROF_MAX * sizeof *g_hle_ns); memset(g_hle_n, 0, HLE_PROF_MAX * sizeof *g_hle_n);
}
/* XRT_PROF_LR=substring: which guest code calls the SDK function(s) whose name contains it (return addresses,
   reported with the profile) - e.g. GetSystemTick to find a busy-wait loop. */
static const char *g_lr_pat; static int g_lr_init;
#define LR_SLOTS 64
static uint64_t g_lr_addr[LR_SLOTS], g_lr_cnt[LR_SLOTS];
static void lr_note(Ctx *c, uint64_t idx) {
    if (!g_lr_init) { g_lr_init = 1; g_lr_pat = getenv("XRT_PROF_LR"); }
    if (!g_lr_pat || !strstr(g_imp[idx].name, g_lr_pat)) return;
    uint64_t lr = c->x[30];
    /* XRT_PROF_LR_UP=<sp offset>: also attribute to the caller one level up, whose return address the calling
       function saved at [sp + offset] (read from the guest stack) */
    { static long up = -2; if (up == -2) { const char *e = getenv("XRT_PROF_LR_UP"); up = e ? strtol(e, 0, 0) : -1; }
      if (up >= 0 && c->x[31]) lr = *(uint64_t *)(uintptr_t)(c->x[31] + (uint64_t)up); }
    for (int i = 0; i < LR_SLOTS; i++) {
        if (g_lr_addr[i] == lr) { __atomic_fetch_add(&g_lr_cnt[i], 1, __ATOMIC_RELAXED); return; }
        if (!g_lr_addr[i]) { uint64_t z = 0; if (__atomic_compare_exchange_n(&g_lr_addr[i], &z, lr, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { __atomic_fetch_add(&g_lr_cnt[i], 1, __ATOMIC_RELAXED); return; } if (g_lr_addr[i] == lr) { __atomic_fetch_add(&g_lr_cnt[i], 1, __ATOMIC_RELAXED); return; } }
    }
}
static void lr_report(double frames) {
    if (!g_lr_pat) return;
    LOG(1, "[prof] callers of *%s* per frame:", g_lr_pat);
    for (int k = 0; k < 8; k++) {
        int bi = -1; for (int i = 0; i < LR_SLOTS; i++) if (g_lr_cnt[i] && (bi < 0 || g_lr_cnt[i] > g_lr_cnt[bi])) bi = i;
        if (bi < 0) break;
        LOG(1, "[prof]   %10.1f  lr main+%#llx", g_lr_cnt[bi] / frames, (unsigned long long)(g_lr_addr[bi] - (uint64_t)(uintptr_t)g_img.base));
        g_lr_cnt[bi] = 0;
    }
    for (int i = 0; i < LR_SLOTS; i++) g_lr_cnt[i] = 0;
}
static void hle_call_impl(Ctx *c, uint64_t idx);
void hle_call(Ctx *c, uint64_t idx) {
    if (g_prof_hle < 0) {
        const char *e = getenv("XRT_PROF_HLE");
        g_prof_hle = e ? 1 : 0;
        g_prof_all = e ? (atoi(e) == 2 ? 2 : atoi(e) == 3 ? 3 : 0) : 0;   /* 2: every thread (summed), not only the render thread; 3: per thread */
        if (g_prof_hle) { g_hle_ns = (uint64_t *)calloc(HLE_PROF_MAX, 8); g_hle_n = (uint64_t *)calloc(HLE_PROF_MAX, 8); g_is_wait = (uint8_t *)calloc(HLE_PROF_MAX, 1); }
    }
    if (g_prof_all == 3 && idx < g_nimp && idx < HLE_PROF_MAX) {
        int s = pt_slot();
        if (s >= PT_MAX || !is_wait(idx)) { hle_call_impl(c, idx); return; }
        __atomic_store_n(&g_pt[s].in_wait, plat_ticks_ns(), __ATOMIC_RELAXED);
        hle_call_impl(c, idx);
        uint64_t t0 = __atomic_exchange_n(&g_pt[s].in_wait, 0, __ATOMIC_RELAXED);   /* (moved forward by a report meanwhile) */
        __atomic_fetch_add(&g_pt[s].wait_ns, plat_ticks_ns() - t0, __ATOMIC_RELAXED);
        return;
    }
    if (g_prof_hle == 1 && idx < g_nimp) lr_note(c, idx);
    if (g_prof_hle != 1 || !(t_render_thread || g_prof_all) || idx >= g_nimp || idx >= HLE_PROF_MAX) { hle_call_impl(c, idx); return; }
    uint64_t t0 = plat_ticks_ns();
    hle_call_impl(c, idx);
    __atomic_fetch_add(&g_hle_ns[idx], plat_ticks_ns() - t0, __ATOMIC_RELAXED); __atomic_fetch_add(&g_hle_n[idx], 1, __ATOMIC_RELAXED);
}
static void hle_call_impl(Ctx *c, uint64_t idx) {
    if (idx >= g_nimp) FATAL("bad HLE index %llu", (unsigned long long)idx);
    if (g_strict < 0) g_strict = getenv("XRT_STRICT") != NULL;
    /* call counts only for the first call and for unimplemented imports: a counter written on every call (mutexes:
       ~10 million calls a second from several threads) keeps the shared table's cache lines bouncing between cores */
    int first_call = g_imp[idx].calls == 0;
    if (first_call || !g_imp[idx].fn) g_imp[idx].calls++;
    { static int first = -1; if (first < 0) first = getenv("XRT_HLEFIRST") != NULL;
      if (first && first_call) LOG(0, "[hlefirst] %s", g_imp[idx].name); }
    { static long ntrace = -2, tfrom = -1, tto = -1; static long nseen;
      if (ntrace == -2) { const char *e = getenv("XRT_NVNTRACE"); ntrace = e ? atol(e) : -1;
                          const char *f = getenv("XRT_NVNTRACE_FRAMES"); if (f) { sscanf(f, "%ld,%ld", &tfrom, &tto); tto += tfrom; ntrace = 1L << 40; } }
      if (ntrace > 0 && !strncmp(g_imp[idx].name, "nvn:", 4) && nseen < ntrace &&
          (tfrom < 0 || ((long)g_nvn_frames >= tfrom && (long)g_nvn_frames < tto))) { nseen++;
        LOG(0, "[nvntrace] %s(%llx, %llx, %llx, %llx, %llx, %llx, %llx, %llx | s0=%g s1=%g s2=%g s3=%g)", g_imp[idx].name + 4, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
            (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7], c->v[0].f[0], c->v[1].f[0], c->v[2].f[0], c->v[3].f[0]); } }
    if (g_imp[idx].fn) {
        LOG(3, "[hle] %s", g_imp[idx].name);
        g_imp[idx].fn(c);
        return;
    }
    /* play reports (telemetry) are intentionally dropped */
    if (!strncmp(g_imp[idx].name, "_ZN2nn5prepo", 12)) { c->x[0] = 0; return; }
    if (first_call) LOG(1, "[hle] UNIMPLEMENTED %s (x0=0x%llx x1=0x%llx) -> returning 0", g_imp[idx].name,
                                   (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
    if (g_strict) FATAL("strict mode: unimplemented import %s", g_imp[idx].name);
    c->x[0] = 0;
}

void hle_print_stats(void) {
    unsigned impl = 0, used = 0;
    for (unsigned i = 0; i < g_nimp; i++) { impl += g_imp[i].fn != NULL; used += g_imp[i].calls > 0; }
    fprintf(stderr, "[hle] imports: %u bound, %u implemented, %u called\n", g_nimp, impl, used);
    for (unsigned i = 0; i < g_nimp; i++)
        if (g_imp[i].calls && !g_imp[i].fn) fprintf(stderr, "  needed: %s (%llu calls)\n", g_imp[i].name, (unsigned long long)g_imp[i].calls);
}

/* ---------------------------------------------------------------- guest varargs */
uint64_t ga_int(GuestArgs *a) {
    if (a->va) {                                   /* guest va_list: {stack, gr_top, vr_top, gr_offs, vr_offs} */
        uint64_t *vl = (uint64_t *)(uintptr_t)a->va;
        int32_t *offs = (int32_t *)(vl + 3);
        if (offs[0] < 0) { uint64_t v = LD64(vl[1] + offs[0]); offs[0] += 8; return v; }
        uint64_t v = LD64(vl[0]); vl[0] += 8; return v;
    }
    if (a->next_gr < 8) return a->c->x[a->next_gr++];
    uint64_t v = LD64(a->stack); a->stack += 8; return v;
}

double ga_double(GuestArgs *a) {
    if (a->va) {
        uint64_t *vl = (uint64_t *)(uintptr_t)a->va;
        int32_t *offs = (int32_t *)(vl + 3);
        if (offs[1] < 0) { double d = U2D(LD64(vl[2] + offs[1])); offs[1] += 16; return d; }
        double d = U2D(LD64(vl[0])); vl[0] += 8; return d;
    }
    if (a->next_vr < 8) return a->c->v[a->next_vr++].fd[0];
    double d = U2D(LD64(a->stack)); a->stack += 8; return d;
}

/* printf with arguments pulled from the guest calling convention */
int guest_vsnprintf(char *out, size_t n, const char *fmt, GuestArgs *a) {
    size_t o = 0;
    char spec[32], tmp[512];
    #define PUT(s) do { const char *_s = (s); while (*_s) { if (o + 1 < n) out[o] = *_s; o++; _s++; } } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { if (o + 1 < n) out[o] = *p; o++; continue; }
        const char *st = p++;
        if (*p == '%') { if (o + 1 < n) out[o] = '%'; o++; continue; }
        while (*p && strchr("-+ #0", *p)) p++;
        int star_w = 0, star_p = 0; long wv = 0, pv = 0;
        if (*p == '*') { star_w = 1; wv = (long)(int)ga_int(a); p++; } else while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { p++; if (*p == '*') { star_p = 1; pv = (long)(int)ga_int(a); p++; } else while (isdigit((unsigned char)*p)) p++; }
        int lng = 0;
        while (*p && strchr("hlLqjzt", *p)) { if (*p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'q') lng = 1; p++; }
        size_t sl = (size_t)(p - st + 1);
        if (sl >= sizeof spec - 4) sl = sizeof spec - 4;
        /* rebuild a host format without length modifiers, then add our own */
        char base[32]; size_t bl = 0;
        for (const char *q = st; q < p; q++) if (!strchr("hlLqjzt", *q)) base[bl++] = *q;
        base[bl] = 0;
        char conv = *p;
        if (!conv) break;
        if (strchr("diuxXoc", conv)) {
            uint64_t v = ga_int(a);
            snprintf(spec, sizeof spec, "%sll%c", base, conv == 'c' ? 'u' : conv);
            if (conv == 'c') { tmp[0] = (char)v; tmp[1] = 0; }
            else if (star_w && star_p) snprintf(tmp, sizeof tmp, spec, (int)wv, (int)pv, (conv == 'd' || conv == 'i') ? (long long)(lng ? (int64_t)v : (int32_t)v) : (long long)(lng ? v : (uint32_t)v));
            else if (star_w || star_p) snprintf(tmp, sizeof tmp, spec, (int)(star_w ? wv : pv), (conv == 'd' || conv == 'i') ? (long long)(lng ? (int64_t)v : (int32_t)v) : (long long)(lng ? v : (uint32_t)v));
            else snprintf(tmp, sizeof tmp, spec, (conv == 'd' || conv == 'i') ? (long long)(lng ? (int64_t)v : (int32_t)v) : (long long)(lng ? v : (uint32_t)v));
        } else if (strchr("fFeEgGaA", conv)) {
            double d = ga_double(a);
            snprintf(spec, sizeof spec, "%s%c", base, conv);
            if (star_w && star_p) snprintf(tmp, sizeof tmp, spec, (int)wv, (int)pv, d);
            else if (star_w || star_p) snprintf(tmp, sizeof tmp, spec, (int)(star_w ? wv : pv), d);
            else snprintf(tmp, sizeof tmp, spec, d);
        } else if (conv == 's') {
            uint64_t v = ga_int(a);
            snprintf(spec, sizeof spec, "%ss", base);
            const char *s = v ? (const char *)(uintptr_t)v : "(null)";
            if (star_w && star_p) snprintf(tmp, sizeof tmp, spec, (int)wv, (int)pv, s);
            else if (star_w || star_p) snprintf(tmp, sizeof tmp, spec, (int)(star_w ? wv : pv), s);
            else snprintf(tmp, sizeof tmp, spec, s);
        } else if (conv == 'p') {
            snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)ga_int(a));
        } else { tmp[0] = 0; }
        PUT(tmp);
    }
    if (n) out[o < n ? o : n - 1] = 0;
    { /* XRT_WATCH=substr: report who formats a matching string (debug aid) */
        static const char *w = (const char *)1;
        if (w == (const char *)1) w = getenv("XRT_WATCH");
        if (w && n && strstr(out, w))
            fprintf(stderr, "[watch] '%s' fmt='%s' lr=main+0x%llx\n", out, fmt, (unsigned long long)(a->c->x[30] - G_BASE));
    }
    return (int)o;
    #undef PUT
}

/* ---------------------------------------------------------------- libc */
#define P(i) ((void *)(uintptr_t)c->x[i])
#define S(i) ((const char *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(val) do { float _f = (val); c->v[0].d[0] = 0; c->v[0].d[1] = 0; c->v[0].f[0] = _f; } while (0)
#define RETD(val) do { double _f = (val); c->v[0].d[1] = 0; c->v[0].fd[0] = _f; } while (0)
#define F(i) (c->v[i].f[0])
#define D(i) (c->v[i].fd[0])

static void h_memcpy(Ctx *c)  { RET(memcpy(P(0), P(1), c->x[2])); }
static void h_memmove(Ctx *c) { RET(memmove(P(0), P(1), c->x[2])); }
static void h_memset(Ctx *c)  { RET(memset(P(0), (int)c->x[1], c->x[2])); }
static void h_memcmp(Ctx *c)  { RET((int64_t)memcmp(P(0), P(1), c->x[2])); }
static void h_strlen(Ctx *c)  { RET(strlen(S(0))); }
static void h_wcslen(Ctx *c)  { const uint32_t *p = P(0); uint64_t n = 0; while (p[n]) n++; RET(n); }  /* wchar_t is 32-bit on Switch */
static void watch_str(Ctx *c, const char *what, const char *s) {
    static const char *w = (const char *)1;
    if (w == (const char *)1) w = getenv("XRT_WATCH");
    if (w && strstr(s, w)) fprintf(stderr, "[watch] %s '%s' lr=main+0x%llx\n", what, s, (unsigned long long)(c->x[30] - G_BASE));
}
static void h_strcpy(Ctx *c)  { watch_str(c, "strcpy src", S(1)); RET(strcpy(P(0), S(1))); }
static void h_strncpy(Ctx *c) { RET(strncpy(P(0), S(1), c->x[2])); }
static void h_strcat(Ctx *c)  { RET(strcat(P(0), S(1))); watch_str(c, "strcat", S(0)); }
static void h_strncat(Ctx *c) { RET(strncat(P(0), S(1), c->x[2])); }
static void h_strcmp(Ctx *c)  { RET((int64_t)strcmp(S(0), S(1))); }
static void h_strncmp(Ctx *c) { RET((int64_t)strncmp(S(0), S(1), c->x[2])); }
static void h_strcasecmp(Ctx *c)  { RET((int64_t)strcasecmp(S(0), S(1))); }
static void h_strncasecmp(Ctx *c) { RET((int64_t)strncasecmp(S(0), S(1), c->x[2])); }
static void h_strchr(Ctx *c)  { RET(strchr(S(0), (int)c->x[1])); }
static void h_strrchr(Ctx *c) { RET(strrchr(S(0), (int)c->x[1])); }
static void h_strstr(Ctx *c)  { RET(strstr(S(0), S(1))); }
static void h_atoi(Ctx *c)    { RET((int64_t)atoi(S(0))); }
static void h_atof(Ctx *c)    { RETD(atof(S(0))); }
static void h_strtol(Ctx *c)  { char *e; long v = strtol(S(0), &e, (int)c->x[2]); if (c->x[1]) ST64(c->x[1], (uint64_t)(uintptr_t)e); RET(v); }
static void h_strtod(Ctx *c)  { char *e; double v = strtod(S(0), &e); if (c->x[1]) ST64(c->x[1], (uint64_t)(uintptr_t)e); RETD(v); }
static void h_abort(Ctx *c)   { FATAL("guest called abort()"); }
static _Thread_local int t_errno;
static void h_errno(Ctx *c)   { RET(&t_errno); }
static void h_rand(Ctx *c)    { RET((uint64_t)(rand() & 0x7fffffff)); }
static void h_time(Ctx *c)    { uint64_t t = (uint64_t)time(NULL); if (c->x[0]) ST64(c->x[0], t); RET(t); }

static void h_snprintf(Ctx *c)  { GuestArgs a = { c, 3, 0, c->x[31], 0 }; RET((int64_t)guest_vsnprintf(P(0), c->x[1], S(2), &a)); }
static void h_sprintf(Ctx *c)   { GuestArgs a = { c, 2, 0, c->x[31], 0 }; RET((int64_t)guest_vsnprintf(P(0), (size_t)-1 >> 1, S(1), &a)); }
static void h_vsnprintf(Ctx *c) { GuestArgs a = { c, 0, 0, 0, c->x[3] }; RET((int64_t)guest_vsnprintf(P(0), c->x[1], S(2), &a)); }
static void h_vsprintf(Ctx *c)  { GuestArgs a = { c, 0, 0, 0, c->x[2] }; RET((int64_t)guest_vsnprintf(P(0), (size_t)-1 >> 1, S(1), &a)); }

/* qsort with a guest comparator: run a native sort, calling back into recompiled code */
static Ctx *q_ctx; static uint64_t q_fn;
static int q_cmp(const void *a, const void *b) { return (int)(int32_t)xrt_call(q_ctx, q_fn, 2, (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b); }
static void h_qsort(Ctx *c) { Ctx *pc = q_ctx; uint64_t pf = q_fn; q_ctx = c; q_fn = c->x[3]; qsort(P(0), c->x[1], c->x[2], q_cmp); q_ctx = pc; q_fn = pf; }

/* math: float args/results in v registers */
static void h_sinf(Ctx *c)  { RETF(sinf(F(0))); }      static void h_cosf(Ctx *c)  { RETF(cosf(F(0))); }
static void h_tanf(Ctx *c)  { RETF(tanf(F(0))); }      static void h_asinf(Ctx *c) { RETF(asinf(F(0))); }
static void h_acosf(Ctx *c) { RETF(acosf(F(0))); }     static void h_atanf(Ctx *c) { RETF(atanf(F(0))); }
static void h_atan2f(Ctx *c){ RETF(atan2f(F(0), F(1))); }
static void h_sqrtf(Ctx *c) { RETF(sqrtf(F(0))); }     static void h_powf(Ctx *c)  { RETF(powf(F(0), F(1))); }
static void h_expf(Ctx *c)  { RETF(expf(F(0))); }      static void h_exp2f(Ctx *c) { RETF(exp2f(F(0))); }
static void h_logf(Ctx *c)  { RETF(logf(F(0))); }      static void h_log2f(Ctx *c) { RETF(log2f(F(0))); }
static void h_log10f(Ctx *c){ RETF(log10f(F(0))); }    static void h_fmodf(Ctx *c) { RETF(fmodf(F(0), F(1))); }
static void h_cbrtf(Ctx *c) { RETF(cbrtf(F(0))); }
static void h_modff(Ctx *c) { float ip; float r = modff(F(0), &ip); memcpy(P(0), &ip, 4); RETF(r); }
static void h_sin(Ctx *c)   { RETD(sin(D(0))); }       static void h_cos(Ctx *c)   { RETD(cos(D(0))); }
static void h_sqrt(Ctx *c)  { RETD(sqrt(D(0))); }      static void h_pow(Ctx *c)   { RETD(pow(D(0), D(1))); }
static void h_exp(Ctx *c)   { RETD(exp(D(0))); }       static void h_exp2(Ctx *c)  { RETD(exp2(D(0))); }
static void h_log(Ctx *c)   { RETD(log(D(0))); }       static void h_log2(Ctx *c)  { RETD(log2(D(0))); }
static void h_log10(Ctx *c) { RETD(log10(D(0))); }     static void h_fmod(Ctx *c)  { RETD(fmod(D(0), D(1))); }
static void h_modf(Ctx *c)  { double ip; double r = modf(D(0), &ip); memcpy(P(0), &ip, 8); RETD(r); }

/* C++ runtime bits */
static void h_guard_acquire(Ctx *c) { uint8_t *g = P(0); RET(*g == 0); }  /* single-threaded init path; TODO: real locking */
static void h_guard_release(Ctx *c) { uint8_t *g = P(0); *g = 1; }
static void h_pure_virtual(Ctx *c)  { FATAL("pure virtual call"); }
static void h_nop(Ctx *c) { (void)c; }

const HleImpl hle_libc[] = {
    {"memcpy", h_memcpy}, {"memmove", h_memmove}, {"memset", h_memset}, {"memcmp", h_memcmp},
    {"strlen", h_strlen}, {"wcslen", h_wcslen}, {"strcpy", h_strcpy}, {"strncpy", h_strncpy}, {"strcat", h_strcat},
    {"strncat", h_strncat}, {"strcmp", h_strcmp}, {"strncmp", h_strncmp}, {"strcasecmp", h_strcasecmp},
    {"strncasecmp", h_strncasecmp}, {"strchr", h_strchr}, {"strrchr", h_strrchr}, {"strstr", h_strstr},
    {"atoi", h_atoi}, {"atof", h_atof}, {"strtol", h_strtol}, {"strtod", h_strtod}, {"abort", h_abort},
    {"__errno_location", h_errno}, {"rand", h_rand}, {"time", h_time},
    {"snprintf", h_snprintf}, {"sprintf", h_sprintf}, {"vsnprintf", h_vsnprintf}, {"vsprintf", h_vsprintf},
    {"qsort", h_qsort},
    {"sinf", h_sinf}, {"cosf", h_cosf}, {"tanf", h_tanf}, {"asinf", h_asinf}, {"acosf", h_acosf}, {"atanf", h_atanf},
    {"atan2f", h_atan2f}, {"sqrtf", h_sqrtf}, {"powf", h_powf}, {"expf", h_expf}, {"exp2f", h_exp2f}, {"logf", h_logf},
    {"log2f", h_log2f}, {"log10f", h_log10f}, {"fmodf", h_fmodf}, {"cbrtf", h_cbrtf}, {"modff", h_modff},
    {"sin", h_sin}, {"cos", h_cos}, {"sqrt", h_sqrt}, {"pow", h_pow}, {"exp", h_exp}, {"exp2", h_exp2}, {"log", h_log},
    {"log2", h_log2}, {"log10", h_log10}, {"fmod", h_fmod}, {"modf", h_modf},
    {"__cxa_guard_acquire", h_guard_acquire}, {"__cxa_guard_release", h_guard_release},
    {"__cxa_pure_virtual", h_pure_virtual}, {"__nnmusl_init_dso", h_nop},
    {0, 0}
};
