#include <algorithm>
#include <unordered_set>
// HLE of the Nintendo SDK (nn::*) functions the game imports.
// Strategy: implement each API natively on the host. Guest-side objects (MutexType, EventType,
// ThreadType...) are opaque guest memory; we key host objects by the guest object's address.
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <filesystem>
#include <vector>
#include "input.h"
#include "gpu.h"
extern "C" {
#include "xrt_internal.h"
}
namespace fs = std::filesystem;
#ifdef _WIN32
#include <malloc.h>
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define P(i) ((void *)(uintptr_t)c->x[i])
#define S(i) ((const char *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define OK() RET(0)                 // nn::Result success
#define RES_FAIL(mod, desc) RET(((desc) << 9) | (mod))
#define HLE(name) static void name(Ctx *c)

static std::string g_romfs_dir = "romfs", g_save_dir = "save";
extern "C" void hle_set_paths(const char *romfs, const char *save) { g_romfs_dir = romfs; g_save_dir = save; }

// ---------------------------------------------------------------- time
static const uint64_t TICK_HZ = 19200000;   // Switch system tick frequency
HLE(os_GetSystemTick) { RET(plat_ticks_ns() * 12 / 625); }            // ns * 19.2e6 / 1e9
HLE(os_GetSystemTickFrequency) { RET(TICK_HZ); }
HLE(os_ConvertToTimeSpan) { int64_t t = (int64_t)c->x[0]; RET((t / 12) * 625 + (t % 12) * 625 / 12); }
HLE(os_ConvertToTick) { int64_t n = (int64_t)c->x[0]; RET((n / 625) * 12 + (n % 625) * 12 / 625); }
// nn::os::SleepThread: precise (the game's wait loops sleep tens of microseconds; a plain host sleep rounds that up to
// a millisecond or more). Stats with XRT_LOG_SLEEP=1.
static std::atomic<uint64_t> g_sleep_n, g_sleep_req_ns, g_sleep_got_ns;
HLE(os_SleepThread) {
    int64_t ns = (int64_t)c->x[0];
    uint64_t t0 = plat_ticks_ns();
    if (ns > 0) plat_sleep_until_ns(t0 + (uint64_t)ns); else std::this_thread::yield();
    static int log = -1; if (log < 0) log = getenv("XRT_LOG_SLEEP") ? 1 : 0;
    if (log) {
        uint64_t n = ++g_sleep_n; g_sleep_req_ns += ns > 0 ? ns : 0; g_sleep_got_ns += plat_ticks_ns() - t0;
        if (n % 20000 == 0) LOG(0, "[os] SleepThread: %llu calls, avg asked %.1f us, slept %.1f us", (unsigned long long)n, g_sleep_req_ns / 1e3 / n, g_sleep_got_ns / 1e3 / n);
    }
}
HLE(os_YieldThread) { std::this_thread::yield(); }

// ---------------------------------------------------------------- mutexes / locks (keyed by guest address)
// Guest sync objects are host objects found by the guest struct's address. The game takes ~75k mutex locks a frame,
// so the lookup is a lock-free insert-only table (milestone M5; it was a global lock + unordered_map) and the
// mutex a small recursive lock on an atomic word: uncontended lock / unlock never enter the kernel; contended ones
// sleep on the word (WaitOnAddress / futex).
static std::mutex g_objs_lock;
template <class T> struct AddrTable {
    static const size_t N = 1 << 18;
    struct Slot { std::atomic<uint64_t> key{0}; std::atomic<T *> val{nullptr}; };
    Slot *s = new Slot[N];
    T *get(uint64_t a) {
        size_t h = (size_t)(((a >> 3) * 0x9E3779B97F4A7C15ull) >> 46);
        for (size_t i = 0; i < N; i++) {
            Slot &sl = s[(h + i) & (N - 1)];
            uint64_t k = sl.key.load(std::memory_order_acquire);
            if (k == 0) {
                uint64_t z = 0;
                if (sl.key.compare_exchange_strong(z, a, std::memory_order_acq_rel)) { T *v = new T(); sl.val.store(v, std::memory_order_release); return v; }
                k = z;
            }
            if (k == a) { T *v; while (!(v = sl.val.load(std::memory_order_acquire))) std::this_thread::yield(); return v; }
        }
        FATAL("sync object table full");
    }
};
#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall WaitOnAddress(volatile void *, void *, size_t, unsigned long);
extern "C" __declspec(dllimport) void __stdcall WakeByAddressSingle(void *);
#pragma comment(lib, "Synchronization.lib")
#define INFINITE_WAIT 0xFFFFFFFFul
#else
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
static uint32_t thread_token() { static std::atomic<uint32_t> next{1}; static thread_local uint32_t t = next++; return t; }
static void addr_wait(std::atomic<uint32_t> *w, uint32_t v) {
#ifdef _WIN32
    WaitOnAddress((volatile void *)w, &v, 4, INFINITE_WAIT);
#else
    syscall(SYS_futex, (uint32_t *)w, FUTEX_WAIT_PRIVATE, v, nullptr, nullptr, 0);
#endif
}
static void addr_wake_one(std::atomic<uint32_t> *w) {
#ifdef _WIN32
    WakeByAddressSingle((void *)w);
#else
    syscall(SYS_futex, (uint32_t *)w, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
}
struct FastMutex {                 // recursive; state 0 free, 1 locked, 2 locked with sleepers
    std::atomic<uint32_t> state{0}, owner{0}; uint32_t depth = 0;
    void lock() {
        uint32_t me = thread_token();
        if (owner.load(std::memory_order_relaxed) == me) { depth++; return; }
        uint32_t z = 0;
        if (!state.compare_exchange_strong(z, 1, std::memory_order_acquire)) {
            for (int i = 0; i < 64; i++) { z = 0; if (state.load(std::memory_order_relaxed) == 0 && state.compare_exchange_weak(z, 1, std::memory_order_acquire)) goto got; }
            while (state.exchange(2, std::memory_order_acquire) != 0) addr_wait(&state, 2);
        }
    got:
        owner.store(me, std::memory_order_relaxed); depth = 1;
    }
    bool try_lock() {
        uint32_t me = thread_token();
        if (owner.load(std::memory_order_relaxed) == me) { depth++; return true; }
        uint32_t z = 0;
        if (!state.compare_exchange_strong(z, 1, std::memory_order_acquire)) return false;
        owner.store(me, std::memory_order_relaxed); depth = 1; return true;
    }
    void unlock() {
        if (owner.load(std::memory_order_relaxed) != thread_token()) return;   // (not ours: ignore, as the old host mutex would have been undefined)
        if (--depth) return;
        owner.store(0, std::memory_order_relaxed);
        if (state.exchange(0, std::memory_order_release) == 2) addr_wake_one(&state);
    }
};
static AddrTable<FastMutex> g_mutexes;
static FastMutex *mtx(uint64_t a) { return g_mutexes.get(a); }
HLE(os_InitializeMutex) { mtx(c->x[0]); }
HLE(os_FinalizeMutex) { }
HLE(os_LockMutex) { mtx(c->x[0])->lock(); }
HLE(os_TryLockMutex) { RET(mtx(c->x[0])->try_lock() ? 1 : 0); }
HLE(os_UnlockMutex) { mtx(c->x[0])->unlock(); }
HLE(os_IsMutexLockedByCurrentThread) { RET(1); }   // used in asserts only

// reader/writer locks: a recursive mutex is a correct (if conservative) implementation
HLE(os_InitRW) { mtx(c->x[0]); }
HLE(os_AcqRW) { mtx(c->x[0])->lock(); }
HLE(os_RelRW) { mtx(c->x[0])->unlock(); }

// condition variables
static AddrTable<std::condition_variable_any> g_cvs;
static std::condition_variable_any *cv(uint64_t a) { return g_cvs.get(a); }
HLE(os_InitializeConditionVariable) { cv(c->x[0]); }
HLE(os_FinalizeConditionVariable) { }
HLE(os_SignalConditionVariable) { cv(c->x[0])->notify_one(); }
HLE(os_BroadcastConditionVariable) { cv(c->x[0])->notify_all(); }
HLE(os_WaitConditionVariable) { auto *m = mtx(c->x[1]); uint32_t d = m->depth; m->depth = 1; std::unique_lock<FastMutex> l(*m, std::adopt_lock); cv(c->x[0])->wait(l); l.release(); m->depth = d; }

// events
struct Event { std::mutex m; std::condition_variable cv; bool signaled = false, autoclear = false; };
static AddrTable<Event> g_events;
static Event *ev(uint64_t a) { return g_events.get(a); }
HLE(os_InitializeEvent) { Event *e = ev(c->x[0]); e->signaled = c->x[1] & 1; e->autoclear = (c->x[2] & 1) != 0; }
HLE(os_FinalizeEvent) { }
static void mw_wake();
HLE(os_SignalEvent) { Event *e = ev(c->x[0]); { std::lock_guard<std::mutex> l(e->m); e->signaled = true; } e->cv.notify_all(); mw_wake(); }
HLE(os_ClearEvent) { Event *e = ev(c->x[0]); std::lock_guard<std::mutex> l(e->m); e->signaled = false; }
// host-side access for other HLE modules (movie:: decoder events)
void xrt_os_signal_event(uint64_t a) { Event *e = ev(a); { std::lock_guard<std::mutex> l(e->m); e->signaled = true; } e->cv.notify_all(); mw_wake(); }
void xrt_os_clear_event(uint64_t a) { Event *e = ev(a); std::lock_guard<std::mutex> l(e->m); e->signaled = false; }
HLE(os_WaitEvent) { Event *e = ev(c->x[0]); std::unique_lock<std::mutex> l(e->m); e->cv.wait(l, [e] { return e->signaled; }); if (e->autoclear) e->signaled = false; }
HLE(os_TryWaitEvent) { Event *e = ev(c->x[0]); std::lock_guard<std::mutex> l(e->m); bool s = e->signaled; if (s && e->autoclear) e->signaled = false; RET(s); }
HLE(os_TimedWaitEvent) {
    Event *e = ev(c->x[0]); std::unique_lock<std::mutex> l(e->m);
    bool s = e->cv.wait_for(l, std::chrono::nanoseconds((int64_t)c->x[1]), [e] { return e->signaled; });
    if (s && e->autoclear) e->signaled = false; RET(s);
}

// ---------------------------------------------------------------- multi-wait (WaitAny over events)
// Holders are linked to a MultiWait; WaitAny returns the first holder whose event is signaled, without clearing it
// (as Horizon's os library: the caller clears / try-waits the event itself). Used by the movie player's decoder
// thread on the movie:: decoder events. Signals wake every multi-waiter, which then rechecks its holders.
struct MwHolder { uint64_t event = 0, mw = 0; };
static std::unordered_map<uint64_t, MwHolder> g_mw_holders;
static std::unordered_map<uint64_t, std::vector<uint64_t>> g_mws;   // multi-wait -> holders, in link order
static std::mutex g_mw_m;
static std::condition_variable g_mw_cv;
static uint64_t g_mw_gen;
static void mw_wake() { { std::lock_guard<std::mutex> l(g_mw_m); g_mw_gen++; } g_mw_cv.notify_all(); }
HLE(os_InitializeMultiWait) { std::lock_guard<std::mutex> l(g_mw_m); g_mws[c->x[0]].clear(); }
HLE(os_FinalizeMultiWait) { std::lock_guard<std::mutex> l(g_mw_m); g_mws.erase(c->x[0]); }
HLE(os_InitializeMultiWaitHolderEvent) { std::lock_guard<std::mutex> l(g_mw_m); g_mw_holders[c->x[0]] = MwHolder{c->x[1], 0}; }
HLE(os_FinalizeMultiWaitHolder) { std::lock_guard<std::mutex> l(g_mw_m); g_mw_holders.erase(c->x[0]); }
HLE(os_LinkMultiWaitHolder) {   // (multi-wait, holder)
    std::lock_guard<std::mutex> l(g_mw_m);
    g_mws[c->x[0]].push_back(c->x[1]);
    g_mw_holders[c->x[1]].mw = c->x[0];
}
HLE(os_UnlinkMultiWaitHolder) { // (holder)
    std::lock_guard<std::mutex> l(g_mw_m);
    MwHolder &h = g_mw_holders[c->x[0]];
    auto it = g_mws.find(h.mw);
    if (it != g_mws.end()) { auto &v = it->second; v.erase(std::remove(v.begin(), v.end(), c->x[0]), v.end()); }
    h.mw = 0;
}
static uint64_t mw_poll(uint64_t mw) {   // g_mw_m held
    auto it = g_mws.find(mw);
    if (it == g_mws.end()) return 0;
    for (uint64_t h : it->second) {
        auto hi = g_mw_holders.find(h);
        if (hi == g_mw_holders.end() || !hi->second.event) continue;
        Event *e = ev(hi->second.event);
        std::lock_guard<std::mutex> l(e->m);
        if (e->signaled) return h;
    }
    return 0;
}
HLE(os_WaitAny) {
    std::unique_lock<std::mutex> l(g_mw_m);
    for (;;) {
        if (uint64_t h = mw_poll(c->x[0])) { RET(h); return; }
        g_mw_cv.wait_for(l, std::chrono::milliseconds(50));   // (the timeout only guards against a missed wake)
    }
}
HLE(os_TryWaitAny) { std::lock_guard<std::mutex> l(g_mw_m); RET(mw_poll(c->x[0])); }
HLE(os_TimedWaitAny) {  // (multi-wait, TimeSpan ns)
    auto end = std::chrono::steady_clock::now() + std::chrono::nanoseconds((int64_t)c->x[1]);
    std::unique_lock<std::mutex> l(g_mw_m);
    for (;;) {
        if (uint64_t h = mw_poll(c->x[0])) { RET(h); return; }
        if (std::chrono::steady_clock::now() >= end) { RET(0); return; }
        g_mw_cv.wait_until(l, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)));
    }
}

// ---------------------------------------------------------------- timer events (frame pacing)
struct Timer { std::mutex m; std::condition_variable cv; bool clear_auto = true, armed = false, signaled = false;
               int64_t next_ns = 0, period_ns = 0; };
static std::unordered_map<uint64_t, Timer *> g_timers;
static Timer *tm_(uint64_t a) { std::lock_guard<std::mutex> l(g_objs_lock); auto &t = g_timers[a]; if (!t) t = new Timer(); return t; }
HLE(os_InitializeTimerEvent) { Timer *t = tm_(c->x[0]); t->clear_auto = (c->x[1] & 1) != 0; t->armed = false; }
HLE(os_FinalizeTimerEvent) { }
HLE(os_StartPeriodicTimerEvent) {   // (timer, TimeSpan first, TimeSpan interval)
    Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m);
    t->armed = true; t->period_ns = (int64_t)c->x[2]; t->next_ns = (int64_t)plat_ticks_ns() + (int64_t)c->x[1];
    { static int n; if (n++ < 8) LOG(1, "[os] periodic timer %llx: first %.3f ms, period %.3f ms (lr %llx)", (unsigned long long)c->x[0], (int64_t)c->x[1] / 1e6, (int64_t)c->x[2] / 1e6, (unsigned long long)c->x[30]); }
}
HLE(os_StartOneShotTimerEvent) {
    Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m);
    t->armed = true; t->period_ns = 0; t->next_ns = (int64_t)plat_ticks_ns() + (int64_t)c->x[1];
}
HLE(os_StopTimerEvent) { Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m); t->armed = false; }
static bool timer_poll(Timer *t) {      // caller holds t->m
    if (t->armed && (int64_t)plat_ticks_ns() >= t->next_ns) {
        t->signaled = true;
        if (t->period_ns > 0) { int64_t now = (int64_t)plat_ticks_ns(); while (t->next_ns <= now) t->next_ns += t->period_ns; }
        else t->armed = false;
    }
    return t->signaled;
}
HLE(os_WaitTimerEvent) {
    Timer *t = tm_(c->x[0]);
    uint64_t t0 = plat_ticks_ns();
    struct Done { uint64_t t0; ~Done() { gpu_prof_add(PROF_TIMER, plat_ticks_ns() - t0); } } done{t0};
    for (;;) {
        int64_t wait;
        {
            std::lock_guard<std::mutex> l(t->m);
            if (timer_poll(t)) { if (t->clear_auto) t->signaled = false; return; }
            wait = t->armed ? t->next_ns - (int64_t)plat_ticks_ns() : 1000000;
        }
        plat_sleep_until_ns(plat_ticks_ns() + (uint64_t)(wait > 0 ? wait : 0));
    }
}
HLE(os_TryWaitTimerEvent) { Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m); bool s = timer_poll(t); if (s && t->clear_auto) t->signaled = false; RET(s); }
HLE(os_SignalTimerEvent) { Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m); t->signaled = true; }
HLE(os_ClearTimerEvent) { Timer *t = tm_(c->x[0]); std::lock_guard<std::mutex> l(t->m); t->signaled = false; }

// ---------------------------------------------------------------- threads
struct HostThread { uint64_t guest_type, entry, arg, stack_size; void *t = nullptr; const char *name = nullptr; };
static std::unordered_map<uint64_t, HostThread *> g_threads;
static thread_local uint64_t t_current_type;
static uint8_t g_main_thread_type[0x200];
static const char *g_main_name = "MainThread";

// nn::os::ThreadType is read directly by game code (e.g. Wwise checks the state byte after
// StartThread), so we keep its public fields in sync with the SDK layout:
//   0x40 u8 state (0 none, 1 initialized, 3 started, 4 terminated)   0x44 s16 base_priority
//   0x46 u16 version   0x50 stack   0x58 stack_size   0x60 argument   0x68 function
//   0x80 char name_buffer[0x20]   0xA0 const char *name_pointer
enum { TS_INITIALIZED = 1, TS_STARTED = 3, TS_TERMINATED = 4 };
static void tt_set(uint64_t t, int off, uint64_t v, int size) { std::memcpy((uint8_t *)(uintptr_t)t + off, &v, size); }

HLE(os_CreateThread) {   // (ThreadType*, entry, arg, stack, stackSize, priority[, core])
    auto *h = new HostThread{c->x[0], c->x[1], c->x[2], c->x[4] ? c->x[4] : 0x100000};
    uint64_t t = c->x[0];
    tt_set(t, 0x40, TS_INITIALIZED, 1);
    tt_set(t, 0x44, c->x[5], 2);
    tt_set(t, 0x46, 1, 2);
    tt_set(t, 0x48, c->x[3], 8); tt_set(t, 0x50, c->x[3], 8);
    tt_set(t, 0x58, c->x[4], 8); tt_set(t, 0x60, c->x[2], 8); tt_set(t, 0x68, c->x[1], 8);
    std::snprintf((char *)(uintptr_t)(t + 0x80), 0x20, "Thread_0x%llx", (unsigned long long)t);
    tt_set(t, 0xA0, t + 0x80, 8);
    h->name = (const char *)(uintptr_t)(t + 0x80);
    std::lock_guard<std::mutex> l(g_objs_lock);
    g_threads[t] = h;
    LOG(2, "[os] CreateThread %llx entry=main+0x%llx", (unsigned long long)t, (unsigned long long)(c->x[1] - G_BASE));
    OK();
}
HLE(os_StartThread) {
    HostThread *h;
    { std::lock_guard<std::mutex> l(g_objs_lock); h = g_threads[c->x[0]]; }
    if (!h) FATAL("StartThread on unknown thread");
    tt_set(h->guest_type, 0x40, TS_STARTED, 1);
    h->t = plat_thread_create([](void *p) {
        HostThread *h = (HostThread *)p;
        t_current_type = h->guest_type;
        Ctx *tc = xrt_new_ctx(h->stack_size < 0x40000 ? 0x40000 : h->stack_size);
        tc->x[0] = h->arg;
        hle_prof_thread_start(h->name, h->entry);
        { char lb[64]; snprintf(lb, sizeof lb, "%s@%llx", h->name ? h->name : "", (unsigned long long)(h->entry - G_BASE)); xrt_sampler_register_thread(lb); }
        LOG(1, "[os] thread '%s' started (entry main+0x%llx)", h->name ? h->name : "?", (unsigned long long)(h->entry - G_BASE));
        xrt_dispatch(tc, h->entry);
        tt_set(h->guest_type, 0x40, TS_TERMINATED, 1);
        LOG(1, "[os] thread '%s' exited", h->name ? h->name : "?");
    }, h, HOST_STACK_THREAD);
}
HLE(os_WaitThread) {
    HostThread *h;
    { std::lock_guard<std::mutex> l(g_objs_lock); h = g_threads[c->x[0]]; }
    if (h && h->t) { plat_thread_join(h->t); h->t = nullptr; }
}
HLE(os_DestroyThread) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    tt_set(c->x[0], 0x40, 0, 1);
    g_threads.erase(c->x[0]);
}
static void init_main_thread_type() {
    static bool done;
    if (done) return;
    done = true;
    uint64_t t = (uint64_t)(uintptr_t)g_main_thread_type;
    tt_set(t, 0x40, TS_STARTED, 1); tt_set(t, 0x46, 1, 2);
    std::snprintf((char *)g_main_thread_type + 0x80, 0x20, "MainThread");
    tt_set(t, 0xA0, t + 0x80, 8);
}
HLE(os_GetCurrentThread) { init_main_thread_type(); RET(t_current_type ? t_current_type : (uint64_t)(uintptr_t)g_main_thread_type); }
HLE(os_SetThreadName) {      // copies into the ThreadType's name buffer
    const char *n = S(1) ? S(1) : "";
    std::snprintf((char *)(uintptr_t)(c->x[0] + 0x80), 0x20, "%s", n);
    tt_set(c->x[0], 0xA0, c->x[0] + 0x80, 8);
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto it = g_threads.find(c->x[0]);
    if (it != g_threads.end()) it->second->name = (const char *)(uintptr_t)(c->x[0] + 0x80);
}
HLE(os_SetThreadNamePointer) {   // keeps the caller's pointer
    tt_set(c->x[0], 0xA0, c->x[1], 8);
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto it = g_threads.find(c->x[0]);
    if (it != g_threads.end()) it->second->name = S(1);
}
HLE(os_GetThreadNamePointer) { uint64_t p; std::memcpy(&p, (uint8_t *)(uintptr_t)c->x[0] + 0xA0, 8); RET(p ? p : (uint64_t)(uintptr_t)g_main_name); }
HLE(os_GetCurrentCoreNumber) { RET(0); }
HLE(nop) { }
HLE(ok0) { RET(0); }
HLE(ret0) { RET(0); }
HLE(ret1) { RET(1); }

// TLS slots
static std::atomic<uint32_t> g_next_tls{0};
static thread_local uint64_t t_tls[64];
HLE(os_AllocateTlsSlot) { uint32_t s = g_next_tls++; *(uint32_t *)P(0) = s; OK(); }
HLE(os_GetTlsValue) { RET(t_tls[(uint32_t)c->x[0] & 63]); }
HLE(os_SetTlsValue) { t_tls[(uint32_t)c->x[0] & 63] = c->x[1]; }

// ---------------------------------------------------------------- memory
HLE(os_IsVirtualAddressMemoryEnabled) { RET(1); }
// Switch apps get a fixed physical memory budget; the game probes it by committing pages
// until AllocateMemoryPages fails, so we must model that limit.
static const uint64_t PHYS_LIMIT = 3285ULL << 20;
static std::atomic<uint64_t> g_committed{0};
static std::mutex g_regions_mu; static std::vector<std::pair<uint64_t, uint64_t>> g_regions;   // (findctr.cpp)
extern "C" int hle_guest_regions(uint64_t *base, uint64_t *size, int max) {
    std::lock_guard<std::mutex> lk(g_regions_mu);
    int n = 0;
    for (auto &r : g_regions) { if (n >= max) break; base[n] = r.first; size[n] = r.second; n++; }
    return n;
}
HLE(os_AllocateAddressRegion) {       // (u64* out, size)
    void *p = plat_reserve(c->x[1]);
    if (p) { std::lock_guard<std::mutex> lk(g_regions_mu); g_regions.push_back({(uint64_t)(uintptr_t)p, c->x[1]}); }
    *(uint64_t *)P(0) = (uint64_t)(uintptr_t)p;
    LOG(1, "[os] AllocateAddressRegion %llu MB -> %p", (unsigned long long)(c->x[1] >> 20), p);
    if (!p) { RES_FAIL(3, 8); return; }
    OK();
}
HLE(os_AllocateMemoryPages) {         // (addr, size)
    if (g_committed + c->x[1] > PHYS_LIMIT) { RES_FAIL(3, 8); return; }      // ResultOutOfMemory
    if (!plat_commit(P(0), c->x[1])) { RES_FAIL(3, 8); return; }
    g_committed += c->x[1];
    OK();
}
extern "C" int pad_hold_dt_hook(Ctx *c);
uint64_t hle_guest_committed() { return g_committed.load(); }
HLE(os_FreeMemoryPages) { plat_decommit(P(0), c->x[1]); g_committed -= c->x[1]; OK(); }
HLE(os_FreeAddressRegion) { OK(); }
HLE(os_QueryMemoryInfo) {
    uint64_t *mi = (uint64_t *)P(0);
    std::memset(mi, 0, 0x38);
    mi[0] = 3285ULL << 20;   // totalAvailableMemorySize (retail app limit, approx.)
    mi[1] = 512ULL << 20;
}

// nn::mem::StandardAllocator -> host heap (object contents unused)
HLE(mem_SA_ctor) { }
HLE(mem_SA_Initialize) { }
HLE(mem_SA_Finalize) { }
HLE(mem_SA_Allocate) {   // (this, size, align)
    uint64_t al = c->x[2] < 16 ? 16 : c->x[2];
#ifdef _WIN32
    RET(_aligned_malloc(c->x[1], al));
#else
    void *p = nullptr; if (posix_memalign(&p, al, c->x[1] ? c->x[1] : 1)) p = nullptr; RET(p);
#endif
}
HLE(mem_SA_Free) {
#ifdef _WIN32
    _aligned_free(P(1));
#else
    free(P(1));
#endif
}

// operator new(size, nothrow): forward to the game's own malloc
extern "C" uint64_t xrt_find_export(const char *name);
HLE(op_new_nothrow) { static uint64_t m = xrt_find_export("malloc"); RET(xrt_call(c, m, 1, c->x[0])); }

// ---------------------------------------------------------------- applet / settings
HLE(oe_GetOperationMode) { RET(1); }          // 1 = Console (docked)
HLE(oe_GetPerformanceMode) { RET(1); }        // 1 = Boost
HLE(oe_GetCurrentFocusState) { RET(1); }      // InFocus
// System language (nn::settings::LanguageCode). Override with XRT_LANG=fr / de / ja / en-US ...
static const char *sys_lang() { const char *e = getenv("XRT_LANG"); return e && *e && strlen(e) < 8 ? e : "en-GB"; }
HLE(oe_GetDesiredLanguage) { uint64_t v = 0; std::memcpy(&v, sys_lang(), strlen(sys_lang())); RET(v); }
HLE(oe_TryPopNotificationMessage) { RET(0); }
HLE(settings_LangCount) { RET(1); }
HLE(settings_GetLangCodes) { uint64_t v = 0; std::memcpy(&v, sys_lang(), strlen(sys_lang())); if (c->x[1] > 0) *(uint64_t *)P(0) = v; RET(1); }
HLE(os_GetHostArgc) { RET(0); }
HLE(os_GetHostArgv) { RET(0); }

// ---------------------------------------------------------------- filesystem
struct HFile { FILE *f; std::mutex m; };   // nn::fs reads are positional + thread-safe; FILE* seek+read is not
static std::map<std::string, std::string> g_mounts;
static std::string host_path(const char *p) {
    std::string s(p);
    auto colon = s.find(':');
    if (colon == std::string::npos) return s;
    std::string mount = s.substr(0, colon), rest = s.substr(colon + 1);
    auto it = g_mounts.find(mount);
    std::string base = it != g_mounts.end() ? it->second : mount;
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    return base + "/" + rest;
}
// ---------------------------------------------------------------- RomFS mods (M5)
// Mods made for emulators: each folder in the game folder's mods\ (mods\<name>\romfs\..., or the files directly in
// mods\<name>\) replaces or adds game files. A file the game opens from its RomFS is taken from the first enabled
// mod that has it (mods in name order), else from the game. Turned on / off in the TAB menu (romfs_mod.<name> in
// xc3.ini); files the game already loaded change on its next load (area change, restart).
// exefs\*.pchtxt code patches in those folders become switchable mods at the next build (recomp/recompile.py).
struct RomfsMod { std::string name, root; size_t files = 0; bool on = true; };
static std::mutex g_mods_m;
static std::vector<RomfsMod> g_rmods;
static std::unordered_map<std::string, std::string> g_mod_files;   // lowercase path in the RomFS -> host file
static std::unordered_set<std::string> g_mod_dirs;
static std::string mods_dir() { std::string r = g_romfs_dir; size_t p = r.find_last_of("/\\"); return (p == std::string::npos ? std::string(".") : r.substr(0, p)) + "/mods"; }
static std::string lower_path(std::string s) {
    for (char &ch : s) { if (ch == '\\') ch = '/'; else if (ch >= 'A' && ch <= 'Z') ch += 32; }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}
static void mods_index_locked() {   // (g_mods_m)
    g_mod_files.clear(); g_mod_dirs.clear();
    for (RomfsMod &m : g_rmods) {
        m.files = 0;
        if (!m.on) continue;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(m.root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::string rel = lower_path(fs::relative(it->path(), m.root, ec).generic_string());
            if (rel.empty()) continue;
            if (rel == "skyline" || rel.compare(0, 8, "skyline/") == 0 || rel == "exefs" || rel.compare(0, 6, "exefs/") == 0) {   // (Switch plugins: built in here)
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) { g_mod_dirs.insert(rel); continue; }
            if (!it->is_regular_file(ec)) continue;
            if (g_mod_files.emplace(rel, it->path().string()).second) m.files++;
            else LOG(1, "[mods] %s: %s is also in an earlier mod - that one is used", m.name.c_str(), rel.c_str());
        }
    }
}
extern bool settings_romfs_mod_on(const char *name);   // settings.cpp (default on)
static void mods_init() {
    static bool done; if (done) return; done = true;
    std::lock_guard<std::mutex> l(g_mods_m);
    std::error_code ec;
    std::string dir = mods_dir();
    if (!fs::is_directory(dir, ec)) { fs::create_directories(dir, ec); return; }   // (an empty one shows players where mods go)
    std::vector<fs::path> names;
    for (auto &e : fs::directory_iterator(dir, ec)) if (e.is_directory(ec)) names.push_back(e.path());
    std::sort(names.begin(), names.end());
    for (auto &p : names) {
        RomfsMod m; m.name = p.filename().string();
        // layouts: <mod>\romfs\..., <mod>\files..., or a Switch SD card / Atmosphere one (the randomizer's output folder):
        // <mod>\[atmosphere\]contents\010074F013262000\romfs\...
        fs::path root = p;
        for (fs::path q : {p / "romfs", p / "contents" / "010074F013262000" / "romfs", p / "atmosphere" / "contents" / "010074F013262000" / "romfs"})
            if (fs::is_directory(q, ec)) { root = q; break; }
        m.root = root.string();
        bool only_exefs = root == p && !fs::is_directory(p / "romfs", ec) && fs::is_directory(p / "exefs", ec);
        if (only_exefs) { LOG(1, "[mods] %s: code patch only (exefs) - built in at the next build", m.name.c_str()); continue; }
        m.on = settings_romfs_mod_on(m.name.c_str());
        g_rmods.push_back(m);
    }
    mods_index_locked();
    for (auto &m : g_rmods) LOG(1, "[mods] %s: %s, %zu files", m.name.c_str(), m.on ? "on" : "off", m.files);
}
// The game reads almost everything from its archive (bf3.arh / bf3.ard); it looks for loose RomFS files (DLC packs
// first, then the base game) only for files the archive doesn't list. A file a mod has is hidden from the archive
// lookup (ml::DevFileArchiver::getFileInfo, main+0x1257798 in 2.2.0: the address xc3-file-loader by roccodev hooks),
// so the game opens it loose - from the mod (resolve_path).
static std::atomic<uint64_t> g_mod_hidden{0};
static int archive_lookup_hook(Ctx *c) {   // (this, media, const char *name, FileInfoResult &)
    const char *name = (const char *)(uintptr_t)c->x[2];
    if (!name) return 0;
    static int logn = getenv("XRT_LOG_ARCHIVE") ? 2000 : 0;   // (which names the game asks the archive for)
    if (logn > 0) { logn--; LOG(0, "[archive] %s", name); }
    std::lock_guard<std::mutex> l(g_mods_m);
    if (g_mod_files.empty() || !g_mod_files.count(lower_path(name))) return 0;
    if (g_mod_hidden++ < 100) LOG(1, "[mods] %s: from the mods folder", name);
    c->x[0] = 0;   // "not in the archive"
    return 1;
}
extern "C" void xrt_mods_boot(void) {
    mods_init();
    xrt_register_hook_ret(0x1257798, archive_lookup_hook);
    xrt_register_hook_ret(0xe6b3e0, pad_hold_dt_hook);   // (hle_nvn.cpp: menu cursor repeat)
}
static std::string aoc_dir();
static bool mod_mount(const std::string &mount) {   // the game's RomFS or a DLC pack's
    auto it = g_mounts.find(mount);
    if (it == g_mounts.end()) return false;
    static const std::string ad = aoc_dir();
    return it->second == g_romfs_dir || it->second.compare(0, ad.size(), ad) == 0;
}
// the host file for a guest path: a mod's file when one replaces it
static std::string resolve_path(const char *guest, bool *from_mod = nullptr) {
    std::string s(guest);
    if (from_mod) *from_mod = false;
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        if (mod_mount(s.substr(0, colon))) {
            mods_init();
            std::lock_guard<std::mutex> l(g_mods_m);
            if (!g_mod_files.empty()) {
                auto f = g_mod_files.find(lower_path(s.substr(colon + 1)));
                if (f != g_mod_files.end()) { if (from_mod) *from_mod = true; return f->second; }
            }
        }
    }
    return host_path(guest);
}
static bool mod_dir_exists(const char *guest) {
    std::string s(guest); size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    if (!mod_mount(s.substr(0, colon))) return false;
    std::lock_guard<std::mutex> l(g_mods_m);
    return g_mod_dirs.count(lower_path(s.substr(colon + 1))) != 0;
}
// TAB menu
int romfs_mod_count() { mods_init(); std::lock_guard<std::mutex> l(g_mods_m); return (int)g_rmods.size(); }
std::string romfs_mod_name(int i) { std::lock_guard<std::mutex> l(g_mods_m); return i >= 0 && i < (int)g_rmods.size() ? g_rmods[i].name : std::string(); }
size_t romfs_mod_files(int i) { std::lock_guard<std::mutex> l(g_mods_m); return i >= 0 && i < (int)g_rmods.size() ? g_rmods[i].files : 0; }
bool romfs_mod_on(int i) { std::lock_guard<std::mutex> l(g_mods_m); return i >= 0 && i < (int)g_rmods.size() && g_rmods[i].on; }
static void missing_forget();
void romfs_mod_set(int i, bool on) {
    { std::lock_guard<std::mutex> l(g_mods_m); if (i < 0 || i >= (int)g_rmods.size() || g_rmods[i].on == on) return; g_rmods[i].on = on; mods_index_locked(); }
    missing_forget();
    LOG(1, "[mods] %s %s", romfs_mod_name(i).c_str(), on ? "on" : "off");
}
std::string xrt_host_path(const char *guest_path) { return resolve_path(guest_path); }
HLE(fs_QueryMountRomCacheSize) { *(uint64_t *)P(0) = 0x1000; OK(); }
HLE(fs_MountRom) { g_mounts[S(0)] = g_romfs_dir; LOG(1, "[fs] mount %s: -> %s", S(0), g_romfs_dir.c_str()); OK(); }
HLE(fs_MountSave) { fs::create_directories(g_save_dir); g_mounts[S(0)] = g_save_dir; OK(); }
HLE(fs_Unmount) { g_mounts.erase(S(0)); }
HLE(fs_SetAllocator) { }
// Game data that isn't there (the game probes every DLC pack for files it overrides, again and again): remembered, so
// a probe costs a lookup instead of a failed open (milliseconds on Windows with a virus scanner) and a log line.
static std::mutex g_missing_m; static std::unordered_map<std::string, int> g_missing;
static void missing_forget() { std::lock_guard<std::mutex> l(g_missing_m); g_missing.clear(); }   // (a mod was turned on)
HLE(fs_OpenFile) {      // (FileHandle* out, path, mode)
    bool modded = false;
    std::string hp = resolve_path(S(1), &modded);
    if (modded) { static int n; if (n++ < 200) LOG(1, "[mods] %s -> %s", S(1), hp.c_str()); }
    int mode = (int)c->x[2];
    const bool ro = !(mode & 2) && hp.compare(0, g_save_dir.size(), g_save_dir) != 0;   // (save files can appear later)
    if (ro) {
        std::lock_guard<std::mutex> l(g_missing_m);
        auto it = g_missing.find(hp);
        if (it != g_missing.end()) { it->second++; gpu_note_io(0, 0, true); RES_FAIL(2, 1); return; }
    }
    uint64_t t0 = plat_ticks_ns();
    FILE *f = std::fopen(hp.c_str(), (mode & 2) ? "r+b" : "rb");
    gpu_note_io(plat_ticks_ns() - t0, 0, f == nullptr);
    LOG(2, "[fs] open %s -> %s", S(1), f ? "ok" : "NOT FOUND");
    static int fslog = getenv("XRT_FSLOG") ? 1 : 0;
    if (fslog && f) LOG(0, "[fs] opened %s as %p", S(1), (void *)f);
    if (!f) {
        if (ro) { std::lock_guard<std::mutex> l(g_missing_m); g_missing[hp] = 1; }
        LOG(1, "[fs] missing file %s (%s)", S(1), hp.c_str()); RES_FAIL(2, 1); return;   // ResultPathNotFound
    }
    { HFile *h = new HFile(); h->f = f; *(uint64_t *)P(0) = (uint64_t)(uintptr_t)h; }
    OK();
}
HLE(fs_CloseFile) { HFile *h = (HFile *)P(0); if (h) { std::fclose(h->f); delete h; } }
HLE(fs_GetFileSize) { HFile *h = (HFile *)P(1); std::lock_guard<std::mutex> l(h->m); int64_t cur = ftell64(h->f); fseek64(h->f, 0, SEEK_END); *(int64_t *)P(0) = ftell64(h->f); fseek64(h->f, cur, SEEK_SET); OK(); }
HLE(fs_ReadFile4) {   // (handle, offset, buf, size): must read exactly size bytes
    HFile *h = (HFile *)P(0); std::lock_guard<std::mutex> l(h->m);
    static int fslog = getenv("XRT_FSLOG") ? 1 : 0;
    if (fslog) LOG(0, "[fs] read %p off=0x%llx size=0x%llx", (void *)h, (unsigned long long)c->x[1], (unsigned long long)c->x[3]);
    uint64_t t0 = plat_ticks_ns();
    fseek64(h->f, (int64_t)c->x[1], SEEK_SET);
    size_t n = std::fread(P(2), 1, c->x[3], h->f);
    gpu_note_io(plat_ticks_ns() - t0, n, false);
    if (n != c->x[3]) { LOG(1, "[fs] short read: wanted %llu at %llu, got %llu", (unsigned long long)c->x[3], (unsigned long long)c->x[1], (unsigned long long)n); RES_FAIL(2, 3001); return; }
    OK();
}
HLE(fs_ReadFile5) {
    HFile *h = (HFile *)P(1); std::lock_guard<std::mutex> l(h->m);
    static int fslog = getenv("XRT_FSLOG") ? 1 : 0;
    if (fslog) LOG(0, "[fs] read5 %p off=0x%llx size=0x%llx", (void *)h, (unsigned long long)c->x[2], (unsigned long long)c->x[4]);
    uint64_t t0 = plat_ticks_ns();
    fseek64(h->f, (int64_t)c->x[2], SEEK_SET); size_t n = std::fread(P(3), 1, c->x[4], h->f);
    gpu_note_io(plat_ticks_ns() - t0, n, false);
    *(uint64_t *)P(0) = n; OK();
}
HLE(fs_WriteFile) { HFile *h = (HFile *)P(0); std::lock_guard<std::mutex> l(h->m); fseek64(h->f, (int64_t)c->x[1], SEEK_SET); std::fwrite(P(2), 1, c->x[3], h->f); OK(); }
HLE(fs_FlushFile) { HFile *h = (HFile *)P(0); std::lock_guard<std::mutex> l(h->m); std::fflush(h->f); OK(); }
HLE(fs_CreateFile) { std::string hp = host_path(S(0)); FILE *f = std::fopen(hp.c_str(), "wb"); if (f) std::fclose(f); OK(); }
HLE(fs_DeleteFile) { std::remove(host_path(S(0)).c_str()); OK(); }
HLE(fs_CreateDirectory) { fs::create_directories(host_path(S(0))); OK(); }
HLE(fs_GetEntryType) {  // (DirectoryEntryType* out, path): 0 = dir, 1 = file
    std::string hp = resolve_path(S(1));
    std::error_code ec;
    if (!fs::exists(hp, ec)) { if (mod_dir_exists(S(1))) { *(uint32_t *)P(0) = 0; OK(); return; } RES_FAIL(2, 1); return; }
    *(uint32_t *)P(0) = fs::is_directory(hp, ec) ? 0 : 1;
    OK();
}
HLE(fs_Commit) { OK(); }

// ---------------------------------------------------------------- display / GPU entry points
static uint8_t g_fake_display[64], g_fake_layer[64], g_fake_window[64];
HLE(vi_OpenDefaultDisplay) { *(uint64_t *)P(0) = (uint64_t)(uintptr_t)g_fake_display; OK(); }
HLE(vi_CreateLayer) { *(uint64_t *)P(0) = (uint64_t)(uintptr_t)g_fake_layer; OK(); }
HLE(vi_GetNativeWindow) { *(uint64_t *)P(0) = (uint64_t)(uintptr_t)g_fake_window; OK(); }

// NVN: every entry point is resolved by name at runtime. They all land in the HLE table as
// "nvn:<name>"; the NVN->Vulkan backend (milestone M3) fills in implementations.
static std::mutex g_nvn_lock;
static uint64_t nvn_proc(const char *name) {
    std::lock_guard<std::mutex> l(g_nvn_lock);
    std::string key = std::string("nvn:") + name;
    return hle_resolve(strdup(key.c_str()));
}
HLE(nvnBootstrapLoader) { RET(nvn_proc(S(0))); }

extern "C" void hle_nvn_get_proc(Ctx *c) { RET(c->x[1] ? nvn_proc(S(1)) : 0); }


// ---------------------------------------------------------------- input (virtual Pro Controller, no buttons yet: M4 maps SDL input here)
HLE(hid_style_set) { RET(c->x[0] && *(uint32_t *)P(0) == 0 ? 1 : 0); }     // NpadId 0 = Pro Controller (FullKey)
HLE(hid_fullkey_state) {   // NpadFullKeyState: i64 sampling; u64 buttons; i32 lx,ly,rx,ry; u32 attributes
    static std::atomic<int64_t> sampling{0};
    uint8_t *s = (uint8_t *)P(0);
    std::memset(s, 0, 0x28);
    int64_t n = ++sampling; std::memcpy(s, &n, 8);
    uint32_t attr = (*(uint32_t *)P(1) == 0) ? 1 : 0;      // IsConnected
    std::memcpy(s + 0x20, &attr, 4);
    if (attr) {
        uint64_t b = input_buttons(); std::memcpy(s + 8, &b, 8);
        int32_t st[4]; input_sticks(st); std::memcpy(s + 0x10, st, 16);
    }
}
HLE(hid_other_state) { std::memset(P(0), 0, 0x28); }
HLE(hid_touch_states) { RET(0); }
HLE(hid_ctrl_support_default) { std::memset(P(0), 0, 0x8); }
HLE(hid_show_ctrl_support) {   // ControllerSupportResultInfo { i32 playerCount; u32 selectedNpadId }
    int32_t *r = (int32_t *)P(0); r[0] = 1; r[1] = 0; RET(0);
}

// ---------------------------------------------------------------- account (one local user)
static const uint64_t USER_UID[2] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL};
HLE(acc_list_users) { *(int32_t *)P(0) = 1; if ((int)c->x[2] > 0) std::memcpy(P(1), USER_UID, 16); RET(0); }
HLE(acc_try_open_preselected) { std::memset(P(0), 0, 0x18); *(uint64_t *)P(0) = 1; RET(1); }
HLE(acc_get_user_id) { std::memcpy(P(0), USER_UID, 16); RET(0); }
HLE(fs_is_save_existing) { RET(1); }
// DLC (add-on content): pack n is the folder <game>/aoc/<n>/romfs (extract_dlc.bat), mounted like the RomFS.
static std::string aoc_dir() { std::string r = g_romfs_dir; size_t p = r.find_last_of("/\\"); return (p == std::string::npos ? std::string(".") : r.substr(0, p)) + "/aoc"; }
static std::vector<int> aoc_installed() {
    static std::vector<int> v; static bool scanned;
    if (scanned) return v;
    scanned = true;
    for (int i = 1; i <= 64; i++) if (fs::is_directory(aoc_dir() + "/" + std::to_string(i) + "/romfs")) v.push_back(i);
    std::string l; for (int i : v) l += " " + std::to_string(i);
    LOG(1, "[aoc] %zu DLC pack(s) in %s:%s", v.size(), aoc_dir().c_str(), l.c_str());
    return v;
}
HLE(aoc_list) {   // (int* out, int offset, int count) -> number written
    std::vector<int> v = aoc_installed();
    int off = (int)c->x[1], n = (int)c->x[2], w = 0;
    for (int i = off; i >= 0 && i < (int)v.size() && w < n; i++) ((int32_t *)P(0))[w++] = v[i];
    RET(w);
}
HLE(fs_QueryMountAocCacheSize) { *(uint64_t *)P(0) = 0x1000; OK(); }
HLE(fs_MountAddOnContent) {   // (name, int index, void* cache, size_t)
    std::string d = aoc_dir() + "/" + std::to_string((int)c->x[1]) + "/romfs";
    if (!fs::is_directory(d)) { LOG(1, "[aoc] mount %s: DLC %d not installed", S(0), (int)c->x[1]); RET(0x7d402); return; }   // fs: target not found
    g_mounts[S(0)] = d; LOG(1, "[aoc] mount %s: -> %s", S(0), d.c_str()); OK();
}
HLE(oe_display_version) { std::memset(P(0), 0, 16); std::strcpy((char *)P(0), "2.2.0"); }   // DisplayVersion: char[16]
HLE(fs_save_size_max) { *(int64_t *)P(0) = 0x40000000; *(int64_t *)P(1) = 0x40000000; OK(); }
HLE(oe_pseudo_device_id) { std::memset(P(0), 0x5a, 16); }

// nn::util::TSNPrintf(char*, size_t, const char*, ...) - variadic args follow in x3.. / v0..
extern "C" int guest_vsnprintf(char *out, size_t n, const char *fmt, GuestArgs *a);
HLE(util_tsnprintf) { GuestArgs a = { c, 3, 0, c->x[31], 0 }; RET((int64_t)guest_vsnprintf((char *)P(0), c->x[1], S(2), &a)); }


// nn::settings::operator==(const Language&, const LanguageCode&)
HLE(settings_lang_eq) {
    static const char *codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                  "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    int lang = *(int32_t *)P(0);
    char code[9] = {0}; std::memcpy(code, P(1), 8);
    RET(lang >= 0 && lang < (int)(sizeof codes / sizeof *codes) && !std::strcmp(codes[lang], code));
}

// ---------------------------------------------------------------- registry
#define E(mangled, fn) {mangled, fn}
extern "C" const HleImpl hle_nn[] = {
    E("_ZN2nn2os13GetSystemTickEv", os_GetSystemTick), E("_ZN2nn2os22GetSystemTickFrequencyEv", os_GetSystemTickFrequency),
    E("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", os_ConvertToTimeSpan), E("_ZN2nn2os13ConvertToTickENS_8TimeSpanE", os_ConvertToTick),
    E("_ZN2nn2os11SleepThreadENS_8TimeSpanE", os_SleepThread), E("_ZN2nn2os11YieldThreadEv", os_YieldThread),
    E("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", os_InitializeMutex), E("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", os_FinalizeMutex),
    E("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", os_LockMutex), E("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE", os_TryLockMutex),
    E("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", os_UnlockMutex),
    E("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE", os_IsMutexLockedByCurrentThread),
    E("_ZN2nn2os19InitializeBusyMutexEPNS0_13BusyMutexTypeE", os_InitializeMutex), E("_ZN2nn2os17FinalizeBusyMutexEPNS0_13BusyMutexTypeE", os_FinalizeMutex),
    E("_ZN2nn2os13LockBusyMutexEPNS0_13BusyMutexTypeE", os_LockMutex), E("_ZN2nn2os16TryLockBusyMutexEPNS0_13BusyMutexTypeE", os_TryLockMutex),
    E("_ZN2nn2os15UnlockBusyMutexEPNS0_13BusyMutexTypeE", os_UnlockMutex),
    E("_ZN2nn2os26InitializeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", os_InitRW), E("_ZN2nn2os24FinalizeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", nop),
    E("_ZN2nn2os15AcquireReadLockEPNS0_20ReaderWriterLockTypeE", os_AcqRW), E("_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE", os_RelRW),
    E("_ZN2nn2os16AcquireWriteLockEPNS0_20ReaderWriterLockTypeE", os_AcqRW), E("_ZN2nn2os16ReleaseWriteLockEPNS0_20ReaderWriterLockTypeE", os_RelRW),
    E("_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE", os_InitializeConditionVariable),
    E("_ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE", os_FinalizeConditionVariable),
    E("_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE", os_SignalConditionVariable),
    E("_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE", os_BroadcastConditionVariable),
    E("_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE", os_WaitConditionVariable),
    E("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE", os_InitializeEvent), E("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE", os_FinalizeEvent),
    E("_ZN2nn2os11SignalEventEPNS0_9EventTypeE", os_SignalEvent), E("_ZN2nn2os10ClearEventEPNS0_9EventTypeE", os_ClearEvent),
    E("_ZN2nn2os9WaitEventEPNS0_9EventTypeE", os_WaitEvent), E("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE", os_TryWaitEvent),
    E("_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE", os_TimedWaitEvent),
    E("_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE", os_InitializeMultiWait), E("_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE", os_FinalizeMultiWait),
    E("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE", os_InitializeMultiWaitHolderEvent),
    E("_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE", os_FinalizeMultiWaitHolder),
    E("_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE", os_LinkMultiWaitHolder),
    E("_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE", os_UnlinkMultiWaitHolder),
    E("_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE", os_WaitAny), E("_ZN2nn2os10TryWaitAnyEPNS0_13MultiWaitTypeE", os_TryWaitAny),
    E("_ZN2nn2os12TimedWaitAnyEPNS0_13MultiWaitTypeENS_8TimeSpanE", os_TimedWaitAny),
    E("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi", os_CreateThread),
    E("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii", os_CreateThread),
    E("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE", os_StartThread), E("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE", os_WaitThread),
    E("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE", os_DestroyThread), E("_ZN2nn2os16GetCurrentThreadEv", os_GetCurrentThread),
    E("_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc", os_SetThreadName), E("_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc", os_SetThreadNamePointer),
    E("_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE", os_GetThreadNamePointer),
    E("_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi", nop), E("_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim", nop),
    E("_ZN2nn2os20GetCurrentCoreNumberEv", os_GetCurrentCoreNumber),
    E("_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE", os_AllocateTlsSlot), E("_ZN2nn2os11FreeTlsSlotENS0_7TlsSlotE", nop),
    E("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", os_GetTlsValue), E("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", os_SetTlsValue),
    E("_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", os_IsVirtualAddressMemoryEnabled),
    E("_ZN2nn2os21AllocateAddressRegionEPmm", os_AllocateAddressRegion), E("_ZN2nn2os19AllocateMemoryPagesEmm", os_AllocateMemoryPages),
    E("_ZN2nn2os15FreeMemoryPagesEmm", os_FreeMemoryPages), E("_ZN2nn2os17FreeAddressRegionEm", os_FreeAddressRegion),
    E("_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE", os_QueryMemoryInfo),
    E("_ZN2nn2os32SetMemoryAllocatorForThreadLocalEPFPvmmEPFvS1_mE", nop), E("_ZN2nn2os21SetDyingMessageRegionEmm", nop),
    E("_ZN2nn2os14FlushDataCacheEPKvm", nop), E("_ZN2nn2os11GetHostArgcEv", os_GetHostArgc), E("_ZN2nn2os11GetHostArgvEv", os_GetHostArgv),
    E("_ZN2nn3mem17StandardAllocatorC1Ev", mem_SA_ctor), E("_ZN2nn3mem17StandardAllocator10InitializeEPvm", mem_SA_Initialize),
    E("_ZN2nn3mem17StandardAllocator8FinalizeEv", mem_SA_Finalize), E("_ZN2nn3mem17StandardAllocator8AllocateEmm", mem_SA_Allocate),
    E("_ZN2nn3mem17StandardAllocator4FreeEPv", mem_SA_Free),
    E("_ZnwmRKSt9nothrow_t", op_new_nothrow),
    E("_ZN2nn2oe10InitializeEv", nop), E("_ZN2nn2oe16GetOperationModeEv", oe_GetOperationMode),
    E("_ZN2nn2oe18GetPerformanceModeEv", oe_GetPerformanceMode), E("_ZN2nn2oe20GetCurrentFocusStateEv", oe_GetCurrentFocusState),
    E("_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage), E("_ZN2nn2oe25TryPopNotificationMessageEPj", oe_TryPopNotificationMessage),
    E("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", nop), E("_ZN2nn2oe28SetResumeNotificationEnabledEb", nop),
    E("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", nop), E("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", nop),
    E("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", nop), E("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", nop),
    E("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", nop),
    E("_ZN2nn8settings29GetAvailableLanguageCodeCountEv", settings_LangCount),
    E("_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi", settings_GetLangCodes),
    E("_ZN2nn2fs22QueryMountRomCacheSizeEPm", fs_QueryMountRomCacheSize), E("_ZN2nn2fs8MountRomEPKcPvm", fs_MountRom),
    E("_ZN2nn2fs7UnmountEPKc", fs_Unmount), E("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", fs_SetAllocator),
    E("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", fs_OpenFile), E("_ZN2nn2fs9CloseFileENS0_10FileHandleE", fs_CloseFile),
    E("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", fs_GetFileSize), E("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", fs_ReadFile4),
    E("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", fs_ReadFile5),
    E("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", fs_WriteFile), E("_ZN2nn2fs9FlushFileENS0_10FileHandleE", fs_FlushFile),
    E("_ZN2nn2fs10CreateFileEPKcl", fs_CreateFile), E("_ZN2nn2fs10DeleteFileEPKc", fs_DeleteFile),
    E("_ZN2nn2fs15CreateDirectoryEPKc", fs_CreateDirectory), E("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", fs_GetEntryType),
    E("_ZN2nn2fs6CommitEPKc", fs_Commit), E("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", fs_MountSave),
    E("_ZN2nn2vi10InitializeEv", nop), E("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay),
    E("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer), E("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow),
    E("nvnBootstrapLoader", nvnBootstrapLoader), E("nvn:nvnDeviceGetProcAddress", hle_nvn_get_proc),
    E("_ZN2nn4util11ReferSymbolEPKv", nop), E("_ZN2nn4time10InitializeEv", ret0),
    E("_ZN2nn2os20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE", os_InitializeTimerEvent),
    E("_ZN2nn2os18FinalizeTimerEventEPNS0_14TimerEventTypeE", os_FinalizeTimerEvent),
    E("_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_", os_StartPeriodicTimerEvent),
    E("_ZN2nn2os22StartOneShotTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanE", os_StartOneShotTimerEvent),
    E("_ZN2nn2os14StopTimerEventEPNS0_14TimerEventTypeE", os_StopTimerEvent),
    E("_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE", os_WaitTimerEvent),
    E("_ZN2nn2os17TryWaitTimerEventEPNS0_14TimerEventTypeE", os_TryWaitTimerEvent),
    E("_ZN2nn2os16SignalTimerEventEPNS0_14TimerEventTypeE", os_SignalTimerEvent),
    E("_ZN2nn2os15ClearTimerEventEPNS0_14TimerEventTypeE", os_ClearTimerEvent),
    E("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set), E("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_fullkey_state),
    E("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_other_state), E("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_other_state),
    E("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_touch_states),
    E("_ZN2nn3hid20ControllerSupportArg10SetDefaultEv", hid_ctrl_support_default),
    E("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_show_ctrl_support),
    E("_ZN2nn3hid14InitializeNpadEv", nop), E("_ZN2nn3hid15InitializeMouseEv", nop), E("_ZN2nn3hid18InitializeKeyboardEv", nop),
    E("_ZN2nn3hid21InitializeTouchScreenEv", nop), E("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", nop),
    E("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", nop), E("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", nop),
    E("_ZN2nn7account10InitializeEv", nop), E("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", acc_list_users),
    E("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", acc_try_open_preselected),
    E("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", acc_get_user_id), E("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", nop),
    E("_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE", fs_is_save_existing), E("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", ret0),
    E("_ZN2nn3aoc16ListAddOnContentEPiii", aoc_list),
    E("_ZN2nn2fs17MountAddOnContentEPKciPvm", fs_MountAddOnContent), E("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", fs_QueryMountAocCacheSize),
    E("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", oe_display_version), E("_ZN2nn2oe17FinishStartupLogoEv", nop),
    E("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", ok0), E("_ZN2nn2fs18GetSaveDataSizeMaxEPlS1_", fs_save_size_max),
    E("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", ok0), E("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", oe_pseudo_device_id),
    E("_ZN2nn4util9TSNPrintfEPcmPKcz", util_tsnprintf),
    E("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", settings_lang_eq), E("_ZN2nn5prepo10InitializeEv", nop),
    E("_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", nop), E("_ZN2nn2oe34DisableAlbumImageTakenNotificationEv", nop),
    E("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nop),
    E("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nop), E("_ZN2nv18InitializeGraphicsEPvm", nop),
    {nullptr, nullptr}
};
