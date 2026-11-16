/* xrt.h - XC3 recomp runtime: guest CPU context + helpers used by generated code.
 *
 * Model: guest addresses ARE host addresses (the game image is mapped into host memory and
 * relocated to wherever it lands; G_BASE is that load address). Loads/stores are plain
 * memory accesses. Requires clang or gcc (uses __int128 and __builtin_*); on Windows use clang-cl.
 */
#ifndef XRT_H
#define XRT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union V128 {
    uint8_t b[16]; uint16_t h[8]; uint32_t s[4]; uint64_t d[2];
    int8_t sb[16]; int16_t sh[8]; int32_t ss[4]; int64_t sd[2];
    float f[4]; double fd[2];
} V128;

typedef struct Ctx {
    uint64_t x[32];          /* x[31] is SP. XZR never touches the array. */
    V128 v[32];
    uint8_t n, z, c, vf;     /* NZCV */
    uint64_t excl_addr;      /* exclusive monitor (LDXR/STXR) */
    uint64_t excl_val;
    uint64_t tpidr_el0, tpidrro_el0;
    void *thread;            /* host thread object (runtime) */
} Ctx;

typedef void (*GuestFn)(Ctx *);

extern uint64_t G_BASE;                         /* load address of main */
void xrt_dispatch(Ctx *c, uint64_t target);     /* indirect call/jump (BLR/BR/unknown) */
void xrt_trap(Ctx *c, uint64_t pc, const char *why);
void xrt_hvc(Ctx *c, uint64_t imm);   /* hvc #imm in a patch: a runtime call (hle_nvn.cpp) */
void xrt_unimpl(Ctx *c, uint64_t pc, const char *text);
void xrt_hook(Ctx *c, uint64_t func);    /* entry hook for functions in recomp/hooks.txt (handlers: xrt_register_hook) */
int xrt_hook_ret(Ctx *c, uint64_t func); /* the same, and nonzero when a handler answered for the function (xrt_register_hook_ret) */
void xrt_trace(Ctx *c, uint64_t func);   /* entry trace for functions listed in analysis/trace_funcs.txt */
/* switchable mods (recompile.py, .pchtxt files in <game>/patches/mods): generated code tests xrt_mod_flags[k] at each patched
   instruction; names / descriptions are generated into functable.c; the runtime sets the flags (xc3.ini, TAB menu) */
extern uint8_t xrt_mod_flags[64];
extern const unsigned xrt_mod_count;
extern const char *const xrt_mod_names[];
extern const char *const xrt_mod_descs[];

/* ---- memory ---- */
#define XI static inline __attribute__((always_inline))
XI uint8_t  LD8 (uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
XI uint16_t LD16(uint64_t a) { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
XI uint32_t LD32(uint64_t a) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
XI uint64_t LD64(uint64_t a) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
XI V128     LD128(uint64_t a) { V128 v; memcpy(&v, (void *)(uintptr_t)a, 16); return v; }
XI void ST8 (uint64_t a, uint8_t v)  { *(volatile uint8_t *)(uintptr_t)a = v; }
XI void ST16(uint64_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
XI void ST32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
XI void ST64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }
XI void ST128(uint64_t a, V128 v)    { memcpy((void *)(uintptr_t)a, &v, 16); }

/* ---- flags ---- */
XI void FL_ADD64(Ctx *c, uint64_t a, uint64_t b) { uint64_t r = a + b; c->n = r >> 63; c->z = r == 0; c->c = r < a; c->vf = ((~(a ^ b)) & (a ^ r)) >> 63; }
XI void FL_SUB64(Ctx *c, uint64_t a, uint64_t b) { uint64_t r = a - b; c->n = r >> 63; c->z = r == 0; c->c = a >= b; c->vf = ((a ^ b) & (a ^ r)) >> 63; }
XI void FL_ADD32(Ctx *c, uint32_t a, uint32_t b) { uint32_t r = a + b; c->n = r >> 31; c->z = r == 0; c->c = r < a; c->vf = ((~(a ^ b)) & (a ^ r)) >> 31; }
XI void FL_SUB32(Ctx *c, uint32_t a, uint32_t b) { uint32_t r = a - b; c->n = r >> 31; c->z = r == 0; c->c = a >= b; c->vf = ((a ^ b) & (a ^ r)) >> 31; }
XI void FL_LOG64(Ctx *c, uint64_t r) { c->n = r >> 63; c->z = r == 0; c->c = 0; c->vf = 0; }
XI void FL_LOG32(Ctx *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; c->c = 0; c->vf = 0; }
XI void FL_SET(Ctx *c, unsigned nzcv) { c->n = (nzcv >> 3) & 1; c->z = (nzcv >> 2) & 1; c->c = (nzcv >> 1) & 1; c->vf = nzcv & 1; }
XI void FL_FCMP(Ctx *c, double a, double b) {
    if (a != a || b != b) FL_SET(c, 0x3);           /* unordered: C,V */
    else if (a == b) FL_SET(c, 0x6);                /* Z,C */
    else if (a < b) FL_SET(c, 0x8);                 /* N */
    else FL_SET(c, 0x2);                            /* C */
}
#define C_EQ (c->z)
#define C_NE (!c->z)
#define C_HS (c->c)
#define C_LO (!c->c)
#define C_MI (c->n)
#define C_PL (!c->n)
#define C_VS (c->vf)
#define C_VC (!c->vf)
#define C_HI (c->c && !c->z)
#define C_LS (!c->c || c->z)
#define C_GE (c->n == c->vf)
#define C_LT (c->n != c->vf)
#define C_GT (!c->z && c->n == c->vf)
#define C_LE (c->z || c->n != c->vf)
#define C_AL (1)

/* ---- integer helpers ---- */
XI uint64_t ROR64(uint64_t v, unsigned s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
XI uint32_t ROR32(uint32_t v, unsigned s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
XI uint64_t CLZ64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
XI uint32_t CLZ32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
XI uint64_t RBIT64(uint64_t v) { uint64_t r = 0; for (int i = 0; i < 64; i++) r |= ((v >> i) & 1) << (63 - i); return r; }
XI uint32_t RBIT32(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) r |= ((v >> i) & 1) << (31 - i); return r; }
XI uint64_t UMULH(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
XI int64_t  SMULH(int64_t a, int64_t b) { return (int64_t)(((__int128)a * b) >> 64); }
XI uint64_t UDIV64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
XI uint32_t UDIV32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
XI int64_t  SDIV64(int64_t a, int64_t b) { return b == 0 ? 0 : (b == -1 && a == INT64_MIN) ? INT64_MIN : a / b; }
XI int32_t  SDIV32(int32_t a, int32_t b) { return b == 0 ? 0 : (b == -1 && a == INT32_MIN) ? INT32_MIN : a / b; }
XI uint64_t BFM_MASK(unsigned w) { return w >= 64 ? ~0ULL : ((1ULL << w) - 1); }
XI int64_t  SEXT(uint64_t v, unsigned bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

/* ---- FP helpers (AArch64 semantics: saturating conversions, NaN -> 0) ---- */
#define FCVT_SAT(NAME, T, MIN, MAX, ROUND) \
    XI T NAME(double v) { v = ROUND(v); if (v != v) return 0; if (v <= (double)(MIN)) return MIN; if (v >= (double)(MAX)) return MAX; return (T)v; }
#define RZ(x) (x)
FCVT_SAT(FCVT_S32, int32_t, INT32_MIN, INT32_MAX, trunc)
FCVT_SAT(FCVT_S64, int64_t, INT64_MIN, INT64_MAX, trunc)
FCVT_SAT(FCVT_U32, uint32_t, 0, UINT32_MAX, trunc)
FCVT_SAT(FCVT_U64, uint64_t, 0, UINT64_MAX, trunc)
FCVT_SAT(FCVTM_S32, int32_t, INT32_MIN, INT32_MAX, floor)
FCVT_SAT(FCVTM_U32, uint32_t, 0, UINT32_MAX, floor)
FCVT_SAT(FCVTP_S32, int32_t, INT32_MIN, INT32_MAX, ceil)
FCVT_SAT(FCVTP_U32, uint32_t, 0, UINT32_MAX, ceil)
FCVT_SAT(FCVTP_U64, uint64_t, 0, UINT64_MAX, ceil)
FCVT_SAT(FCVTA_S32, int32_t, INT32_MIN, INT32_MAX, round)
XI float  FMAXNM_F(float a, float b)  { return fmaxf(a, b); }
XI float  FMINNM_F(float a, float b)  { return fminf(a, b); }
XI double FMAXNM_D(double a, double b) { return fmax(a, b); }
XI double FMINNM_D(double a, double b) { return fmin(a, b); }
XI float  FMAX_F(float a, float b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; }
XI float  FMIN_F(float a, float b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; }
XI double FMAX_D(double a, double b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; }
XI double FMIN_D(double a, double b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; }
XI float  FRECPE_F(float a) { return 1.0f / a; }
XI float  FRSQRTE_F(float a) { return 1.0f / sqrtf(a); }
XI float  FRECPS_F(float a, float b) { return fmaf(-a, b, 2.0f); }
XI float  FRSQRTS_F(float a, float b) { return fmaf(-a, b, 3.0f) * 0.5f; }
XI float  U2F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
XI double U2D(uint64_t v) { double f; memcpy(&f, &v, 8); return f; }
XI uint32_t F2U(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
XI uint64_t D2U(double f) { uint64_t v; memcpy(&v, &f, 8); return v; }
XI int16_t SAT_S16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* Scalar FP register writes clear the rest of the 128-bit register. */
#define SET_B(i, val) do { uint8_t  _t = (val); c->v[i].d[0] = 0; c->v[i].d[1] = 0; c->v[i].b[0] = _t; } while (0)
#define SET_H(i, val) do { uint16_t _t = (val); c->v[i].d[0] = 0; c->v[i].d[1] = 0; c->v[i].h[0] = _t; } while (0)
#define SET_S(i, val) do { float    _t = (val); c->v[i].d[0] = 0; c->v[i].d[1] = 0; c->v[i].f[0] = _t; } while (0)
#define SET_SU(i, val) do { uint32_t _t = (val); c->v[i].d[0] = 0; c->v[i].d[1] = 0; c->v[i].s[0] = _t; } while (0)
#define SET_D(i, val) do { double   _t = (val); c->v[i].d[1] = 0; c->v[i].fd[0] = _t; } while (0)
#define SET_DU(i, val) do { uint64_t _t = (val); c->v[i].d[1] = 0; c->v[i].d[0] = _t; } while (0)

/* ---- atomics / exclusives ---- */
#define FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define LDA(T, a) __atomic_load_n((T *)(uintptr_t)(a), __ATOMIC_ACQUIRE)
#define STL(T, a, v) __atomic_store_n((T *)(uintptr_t)(a), (T)(v), __ATOMIC_RELEASE)
/* STXR: succeeds (returns 0) if the monitor still holds this address and memory still has the
 * value we loaded. Implemented as a CAS - correct for all lock-free patterns compilers emit. */
#define STX(T, a, v) ({ uint64_t _xa_ = (a); T _xe_ = (T)c->excl_val; int _xok_ = c->excl_addr == _xa_ && \
    __atomic_compare_exchange_n((T *)(uintptr_t)_xa_, &_xe_, (T)(v), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
    c->excl_addr = 0; _xok_ ? 0u : 1u; })
#define LDX(T, a) ({ uint64_t _xa_ = (a); T _xv_ = __atomic_load_n((T *)(uintptr_t)_xa_, __ATOMIC_SEQ_CST); c->excl_addr = _xa_; c->excl_val = _xv_; (uint64_t)_xv_; })

#ifdef __cplusplus
}
#endif
#endif
