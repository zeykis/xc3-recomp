/* Internal runtime declarations (not used by generated code). */
#ifndef XRT_INTERNAL_H
#define XRT_INTERNAL_H
#include "xrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define HLE_BASE 0xFFFF800000000000ULL   /* imports resolve to HLE_BASE + index*4 (never valid host memory) */

typedef void (*HleFn)(Ctx *c);
typedef struct { const char *name; HleFn fn; } HleImpl;

/* image */
typedef struct {
    uint8_t *base;
    uint64_t size, text_size;
    uint64_t init_array, init_array_count;
    uint64_t sym_nnMain, sym_nninitStartup;
} Image;
extern Image g_img;
int  xrt_load_image(const char *path);
void xrt_build_fn_map(void);

/* HLE */
uint64_t hle_resolve(const char *name);
void     hle_call(Ctx *c, uint64_t idx);
const char *hle_name(uint64_t idx);
void     hle_register_all(void);
void     hle_print_stats(void);

/* calling guest code from host (callbacks, thread entry, qsort comparators...) */
uint64_t xrt_call(Ctx *c, uint64_t fn, int nargs, ...);
Ctx     *xrt_new_ctx(uint64_t stack_size);

/* logging */
extern int g_log_level;
extern volatile uint64_t g_nvn_frames;
void hle_prof_thread_start(const char *name, uint64_t entry);   /* XRT_PROF_HLE=3 thread names (hle.c) */
void xrt_sampler_register_thread(const char *label);             /* XRT_SAMPLE sampling profiler (sampler.cpp) */
void xrt_register_hook(uint64_t func, void (*fn)(Ctx *c));       /* recomp/hooks.txt handlers (loader.c) */
void xrt_apply_render_scale(void);                               /* XRT_RES_SCALE (hle_nvn.cpp), at boot */
#define LOG(lvl, ...) do { if (g_log_level >= (lvl)) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define FATAL(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); fputc('\n', stderr); xrt_backtrace(); exit(1); } while (0)
void xrt_backtrace(void);
void plat_reinstall_crash_filter(void);
void xrt_register_hook_ret(uint64_t func, int (*fn)(Ctx *c));
void xrt_mods_boot(void);
uint64_t plat_alloc_total(uint64_t *count);

/* platform */
void *plat_alloc(uint64_t size);   /* zeroed, RW */
uint64_t plat_ticks_ns(void);
void plat_sleep_until_ns(uint64_t deadline);   /* precise sleep (frame limiter) */
void *plat_thread_create(void (*fn)(void *), void *arg, uint64_t stack);
void plat_thread_join(void *t);
void plat_install_crash_handler(void);
void xrt_crash_report(const char *what, uintptr_t host_pc, uintptr_t fault_addr);
void *plat_reserve(uint64_t size);
int plat_commit(void *addr, uint64_t size);
int plat_decommit(void *addr, uint64_t size);
int plat_write_watch(void *base, uint64_t size, void **pages, uint64_t *count, uint64_t *page_size);
uint64_t xrt_find_export(const char *name);
int xrt_load_donor(const char *path);
uint64_t xrt_donor_data(const char *name);
#define HOST_STACK_MAIN (512ULL << 20)
#define HOST_STACK_THREAD (128ULL << 20)

/* guest varargs (AAPCS64) for printf-family */
typedef struct {
    Ctx *c; int next_gr, next_vr; uint64_t stack;     /* direct variadic call */
    uint64_t va;                                      /* or: pointer to a guest va_list */
} GuestArgs;
uint64_t ga_int(GuestArgs *a);
double   ga_double(GuestArgs *a);
int      guest_vsnprintf(char *out, size_t n, const char *fmt, GuestArgs *a);

#endif
