#include <random>
// HLE of libc++ (std::__1) functions the game imports from the SDK.
// Guest objects use libc++'s AArch64 layouts; memory for strings is allocated with the game's
// own operator new/delete (exported by main) so ownership stays consistent.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <chrono>
extern "C" {
#include "xrt_internal.h"
}
#define P(i) ((void *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)

static uint64_t guest_new(Ctx *c, uint64_t n) { static uint64_t f = xrt_find_export("_Znwm"); return xrt_call(c, f, 1, n); }
static void guest_delete(Ctx *c, uint64_t p) { static uint64_t f = xrt_find_export("_ZdlPv"); xrt_call(c, f, 1, p); }

// ---------------------------------------------------------------- std::string (libc++ default layout, little endian)
struct GStr { uint64_t w[3]; };   // long: {cap|1, size, data}; short: byte0 = size<<1, data at +1 (22 chars max)
static bool s_long(const GStr *s) { return s->w[0] & 1; }
static uint64_t s_size(const GStr *s) { return s_long(s) ? s->w[1] : ((const uint8_t *)s)[0] >> 1; }
static char *s_data(GStr *s) { return s_long(s) ? (char *)(uintptr_t)s->w[2] : (char *)s + 1; }
static uint64_t s_cap(const GStr *s) { return s_long(s) ? (s->w[0] & ~1ULL) - 1 : 22; }
static void s_set_size(GStr *s, uint64_t n) {
    if (s_long(s)) s->w[1] = n; else ((uint8_t *)s)[0] = (uint8_t)(n << 1);
    s_data(s)[n] = 0;
}
static void s_reserve(Ctx *c, GStr *s, uint64_t n) {          // ensure capacity >= n
    if (n <= s_cap(s)) return;
    uint64_t alloc = ((n + 1) + 15) & ~15ULL;                   // libc++ __recommend
    char *nd = (char *)(uintptr_t)guest_new(c, alloc);
    uint64_t sz = s_size(s);
    std::memcpy(nd, s_data(s), sz + 1);
    if (s_long(s)) guest_delete(c, s->w[2]);
    s->w[0] = alloc | 1; s->w[1] = sz; s->w[2] = (uint64_t)(uintptr_t)nd;
}
static void s_assign(Ctx *c, GStr *s, const char *src, uint64_t n) {
    if (n > s_cap(s)) {                                        // allocate fresh (src may alias old buffer only if n <= cap)
        uint64_t alloc = ((n + 1) + 15) & ~15ULL;
        char *nd = (char *)(uintptr_t)guest_new(c, alloc);
        std::memcpy(nd, src, n);
        if (s_long(s)) guest_delete(c, s->w[2]);
        s->w[0] = alloc | 1; s->w[2] = (uint64_t)(uintptr_t)nd;
        s->w[1] = n; nd[n] = 0;
        return;
    }
    std::memmove(s_data(s), src, n);
    s_set_size(s, n);
}
HLE(str_copy_ctor) {    // basic_string(const basic_string&)
    GStr *d = (GStr *)P(0), *s = (GStr *)P(1);
    std::memset(d, 0, sizeof *d);
    s_assign(c, d, s_data(s), s_size(s));
}
HLE(str_assign_str) { GStr *d = (GStr *)P(0), *s = (GStr *)P(1); if (d != s) s_assign(c, d, s_data(s), s_size(s)); RET(d); }
HLE(str_assign_n) { GStr *d = (GStr *)P(0); s_assign(c, d, (const char *)P(1), c->x[2]); RET(d); }
HLE(str_assign_cstr) { GStr *d = (GStr *)P(0); const char *s = (const char *)P(1); s_assign(c, d, s, std::strlen(s)); RET(d); }
HLE(str_append_n) {
    GStr *d = (GStr *)P(0); const char *src = (const char *)P(1); uint64_t n = c->x[2], sz = s_size(d);
    if (n) {
        // src may point into d: copy first if it will move
        char tmp[256]; char *heap = nullptr; const char *from = src;
        if (sz + n > s_cap(d) && src >= s_data(d) && src < s_data(d) + sz + 1) {
            if (n <= sizeof tmp) { std::memcpy(tmp, src, n); from = tmp; } else { heap = new char[n]; std::memcpy(heap, src, n); from = heap; }
        }
        s_reserve(c, d, sz + n);
        std::memmove(s_data(d) + sz, from, n);
        s_set_size(d, sz + n);
        delete[] heap;
    }
    RET(d);
}
HLE(str_push_back) { GStr *d = (GStr *)P(0); uint64_t sz = s_size(d); s_reserve(c, d, sz + 1); s_data(d)[sz] = (char)c->x[1]; s_set_size(d, sz + 1); }
HLE(str_compare) {      // compare(pos1, n1, const char* s, n2) const
    GStr *d = (GStr *)P(0); uint64_t pos = c->x[1], n1 = c->x[2], n2 = c->x[4]; const char *s = (const char *)P(3);
    uint64_t sz = s_size(d);
    if (pos > sz) FATAL("std::string::compare out_of_range");
    uint64_t rlen = std::min(n1, sz - pos);
    int r = std::memcmp(s_data(d) + pos, s, std::min(rlen, n2));
    if (r == 0) r = rlen < n2 ? -1 : rlen > n2 ? 1 : 0;
    RET((int64_t)r);
}
HLE(throw_length_error) { FATAL("libc++ length_error thrown (no exception support)"); }
HLE(throw_out_of_range) { FATAL("libc++ out_of_range thrown (no exception support)"); }

// ---------------------------------------------------------------- algorithms
HLE(sort_u64) { std::sort((uint64_t *)P(0), (uint64_t *)P(1)); }
HLE(sort_f32) { std::sort((float *)P(0), (float *)P(1)); }
HLE(sort_u16) { std::sort((uint16_t *)P(0), (uint16_t *)P(1)); }
HLE(sort_u8) { std::sort((uint8_t *)P(0), (uint8_t *)P(1)); }
// std::random_device (libc++: the object holds one int, a file descriptor on other platforms): the host's
HLE(random_device_ctor) { *(int32_t *)P(0) = 0; }
HLE(random_device_dtor) { }
HLE(random_device_call) { static std::random_device rd; static std::mutex m; std::lock_guard<std::mutex> l(m); RET((uint32_t)rd()); }
HLE(next_prime) {
    uint64_t n = c->x[0];
    if (n == 0) { RET(0); return; }
    if (n <= 2) { RET(2); return; }
    for (uint64_t k = n | 1;; k += 2) {
        bool p = true;
        for (uint64_t d = 3; d * d <= k; d += 2) if (k % d == 0) { p = false; break; }
        if (p) { RET(k); return; }
    }
}

// ---------------------------------------------------------------- shared_ptr control block
struct SWC { uint64_t vtbl; std::atomic<int64_t> owners, weak; };
static void call_vslot(Ctx *c, SWC *s, int slot) { uint64_t fn = LD64(s->vtbl + slot * 8); xrt_call(c, fn, 1, (uint64_t)(uintptr_t)s); }
HLE(swc_release_weak) {
    SWC *s = (SWC *)P(0);
    if (s->weak.load(std::memory_order_acquire) == 0) call_vslot(c, s, 4);           // __on_zero_shared_weak
    else if (s->weak.fetch_sub(1, std::memory_order_acq_rel) - 1 == -1) call_vslot(c, s, 4);
}
HLE(swc_lock) {
    SWC *s = (SWC *)P(0);
    int64_t o = s->owners.load();
    while (o != -1) if (s->owners.compare_exchange_weak(o, o + 1)) { RET(s); return; }
    RET(0);
}
HLE(swc_dtor) { }
HLE(swc_get_deleter) { RET(0); }

// ---------------------------------------------------------------- std::mutex / condition_variable
static std::mutex g_lk;
static std::unordered_map<uint64_t, std::mutex *> g_m;
static std::unordered_map<uint64_t, std::condition_variable *> g_cv;
static std::mutex *gm(uint64_t a) { std::lock_guard<std::mutex> l(g_lk); auto &m = g_m[a]; if (!m) m = new std::mutex(); return m; }
static std::condition_variable *gcv(uint64_t a) { std::lock_guard<std::mutex> l(g_lk); auto &v = g_cv[a]; if (!v) v = new std::condition_variable(); return v; }
HLE(mtx_lock) { gm(c->x[0])->lock(); }
HLE(mtx_unlock) { gm(c->x[0])->unlock(); }
HLE(mtx_dtor) { }
HLE(cv_notify_one) { gcv(c->x[0])->notify_one(); }
HLE(cv_notify_all) { gcv(c->x[0])->notify_all(); }
HLE(cv_dtor) { }
HLE(cv_wait) {     // wait(unique_lock<mutex>&): unique_lock = {mutex*, bool owns}
    std::mutex *m = gm(LD64(c->x[1]));
    std::unique_lock<std::mutex> l(*m, std::adopt_lock);
    gcv(c->x[0])->wait(l);
    l.release();
}
HLE(steady_now) { RET(plat_ticks_ns()); }
HLE(system_now) { RET(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

#define S "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
extern "C" const HleImpl hle_cxx[] = {
    {S "C1ERKS5_", str_copy_ctor}, {S "aSERKS5_", str_assign_str}, {S "6assignEPKcm", str_assign_n},
    {S "6assignEPKc", str_assign_cstr}, {S "6appendEPKcm", str_append_n}, {S "9push_backEc", str_push_back},
    {"_ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm", str_compare},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", throw_length_error},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", throw_out_of_range},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", throw_length_error},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", throw_out_of_range},
    {"_ZNSt3__16__sortIRNS_6__lessImmEEPmEEvT0_S5_T_", sort_u64},
    {"_ZNSt3__16__sortIRNS_6__lessIffEEPfEEvT0_S5_T_", sort_f32},
    {"_ZNSt3__16__sortIRNS_6__lessIttEEPtEEvT0_S5_T_", sort_u16},
    {"_ZNSt3__16__sortIRNS_6__lessIhhEEPhEEvT0_S5_T_", sort_u8},
    {"_ZNSt3__113random_deviceC1ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", random_device_ctor},
    {"_ZNSt3__113random_deviceD1Ev", random_device_dtor}, {"_ZNSt3__113random_deviceclEv", random_device_call},
    {"_ZNSt3__112__next_primeEm", next_prime},
    {"_ZNSt3__119__shared_weak_count14__release_weakEv", swc_release_weak},
    {"_ZNSt3__119__shared_weak_count4lockEv", swc_lock},
    {"_ZNSt3__119__shared_weak_countD2Ev", swc_dtor},
    {"_ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info", swc_get_deleter},
    {"_ZNSt3__15mutex4lockEv", mtx_lock}, {"_ZNSt3__15mutex6unlockEv", mtx_unlock}, {"_ZNSt3__15mutexD1Ev", mtx_dtor},
    {"_ZNSt3__118condition_variable10notify_oneEv", cv_notify_one}, {"_ZNSt3__118condition_variable10notify_allEv", cv_notify_all},
    {"_ZNSt3__118condition_variableD1Ev", cv_dtor}, {"_ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE", cv_wait},
    {"_ZNSt3__16chrono12steady_clock3nowEv", steady_now}, {"_ZNSt3__16chrono12system_clock3nowEv", system_now},
    {nullptr, nullptr}
};
