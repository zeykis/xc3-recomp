// nn::os fibers -> host fibers (Win32 Fibers / POSIX ucontext).
// Each guest fiber owns: a guest Ctx (its own registers + the guest stack the game gave us) and a
// large host stack (recompiled code nests host frames per guest call).
#include <mutex>
#include <unordered_map>
extern "C" {
#include "xrt_internal.h"
}
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

#define P(i) ((void *)(uintptr_t)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define HLE(name) static void name(Ctx *c)

static const uint64_t FIBER_HOST_STACK = 64ULL << 20;

struct HFiber {
    uint64_t guest;           // guest FiberType* (0 for a thread's original context)
    uint64_t func, arg;
    Ctx *ctx;
#ifdef _WIN32
    void *fiber;
#else
    ucontext_t uc;
#endif
};

static std::mutex g_fl;
static std::unordered_map<uint64_t, HFiber *> g_fibers;
static thread_local HFiber *t_thread_fiber;   // the host thread's own context
static thread_local HFiber *t_cur;            // currently running fiber on this thread (or t_thread_fiber)

static HFiber *thread_fiber() {
    if (!t_thread_fiber) {
        t_thread_fiber = new HFiber{};
#ifdef _WIN32
        t_thread_fiber->fiber = ConvertThreadToFiber(nullptr);
        if (!t_thread_fiber->fiber) t_thread_fiber->fiber = GetCurrentFiber();
#endif
        t_cur = t_thread_fiber;
    }
    return t_thread_fiber;
}

static void switch_to(HFiber *to) {
    HFiber *from = t_cur ? t_cur : thread_fiber();
    if (from == to) return;
    t_cur = to;
#ifdef _WIN32
    SwitchToFiber(to->fiber);
#else
    swapcontext(&from->uc, &to->uc);
#endif
}

static void fiber_main(HFiber *h) {
    for (;;) {
        // FiberType* (*func)(void*): the return value is the fiber to switch to next
        uint64_t next = xrt_call(h->ctx, h->func, 1, h->arg);
        HFiber *n;
        if (next) {
            std::lock_guard<std::mutex> l(g_fl);
            auto it = g_fibers.find(next);
            if (it == g_fibers.end()) FATAL("fiber returned unknown fiber 0x%llx", (unsigned long long)next);
            n = it->second;
        } else {
            n = thread_fiber();
        }
        switch_to(n);
        // if someone switches back to a finished fiber, run its function again (matches restart semantics)
    }
}

#ifdef _WIN32
static void CALLBACK fiber_tramp(void *p) { fiber_main((HFiber *)p); }
#else
static void fiber_tramp(unsigned lo, unsigned hi) { fiber_main((HFiber *)(uintptr_t)(((uint64_t)hi << 32) | lo)); }
#endif

uint64_t g_fibers_made, g_fibers_ended;   // (memory log)
HLE(os_InitializeFiber) {   // (FiberType*, FiberType*(*)(void*), void* arg, void* stack, size_t stackSize, int flags)
    thread_fiber();
    HFiber *h = new HFiber{};
    h->guest = c->x[0]; h->func = c->x[1]; h->arg = c->x[2];
    h->ctx = (Ctx *)plat_alloc(sizeof(Ctx));
    h->ctx->x[31] = (c->x[3] + c->x[4]) & ~0xfULL;            // guest stack provided by the game
#ifdef _WIN32
    h->fiber = CreateFiberEx(0, FIBER_HOST_STACK, FIBER_FLAG_FLOAT_SWITCH, fiber_tramp, h);
#else
    getcontext(&h->uc);
    h->uc.uc_stack.ss_sp = plat_alloc(FIBER_HOST_STACK);
    h->uc.uc_stack.ss_size = FIBER_HOST_STACK;
    h->uc.uc_link = nullptr;
    uint64_t p = (uint64_t)(uintptr_t)h;
    makecontext(&h->uc, (void (*)())fiber_tramp, 2, (unsigned)(p & 0xffffffff), (unsigned)(p >> 32));
#endif
    std::lock_guard<std::mutex> l(g_fl);
    g_fibers[h->guest] = h; g_fibers_made++;
    LOG(2, "[os] InitializeFiber %llx func=main+0x%llx", (unsigned long long)h->guest, (unsigned long long)(h->func - G_BASE));
}
HLE(os_FinalizeFiber) { std::lock_guard<std::mutex> l(g_fl); g_fibers.erase(c->x[0]); g_fibers_ended++; }
HLE(os_SwitchToFiber) {
    HFiber *to;
    { std::lock_guard<std::mutex> l(g_fl); auto it = g_fibers.find(c->x[0]); to = it == g_fibers.end() ? nullptr : it->second; }
    if (!to && c->x[0]) FATAL("SwitchToFiber to unknown fiber 0x%llx", (unsigned long long)c->x[0]);
    if (!to) to = thread_fiber();      // nullptr = back to the thread's own (non-fiber) context
    thread_fiber();
    switch_to(to);
}
HLE(os_GetCurrentFiber) { RET(t_cur && t_cur != t_thread_fiber ? t_cur->guest : 0); }

extern "C" const HleImpl hle_fiber[] = {
    {"_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", os_InitializeFiber},
    {"_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", os_FinalizeFiber},
    {"_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", os_SwitchToFiber},
    {"_ZN2nn2os15GetCurrentFiberEv", os_GetCurrentFiber},
    {nullptr, nullptr}
};
