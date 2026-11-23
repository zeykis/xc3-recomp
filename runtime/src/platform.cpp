// Host platform layer (memory, time). Portable: Windows + Linux.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <atomic>
static std::atomic<uint64_t> g_plat_alloc_bytes{0}, g_plat_alloc_n{0};
extern "C" uint64_t plat_alloc_total(uint64_t *count) { if (count) *count = g_plat_alloc_n.load(); return g_plat_alloc_bytes.load(); }
extern "C" void *plat_alloc(uint64_t size) {
    size = (size + 0xfff) & ~0xfffULL;
    g_plat_alloc_bytes += size; g_plat_alloc_n++;
#ifdef _WIN32
    // MEM_WRITE_WATCH: the GPU backend asks which pages the CPU wrote (plat_write_watch) to keep VRAM mirrors current
    void *p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
#else
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (!p) { std::fprintf(stderr, "out of memory allocating %llu bytes\n", (unsigned long long)size); std::fflush(stderr); std::abort(); }
    return p;
}

extern "C" uint64_t plat_ticks_ns(void) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint64_t)duration_cast<nanoseconds>(steady_clock::now() - t0).count();
}

// Precise sleep (frame limiter): a high-resolution waitable timer on Windows (plain Sleep rounds to the 15.6 ms
// scheduler tick), then the last ~0.5 ms yielding.
#include <thread>
#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
#include <timeapi.h>
static struct TimerRes { TimerRes() { timeBeginPeriod(1); } } g_timer_res;   // 1 ms scheduler tick for timed waits (cv / events)
#endif
extern "C" void plat_sleep_until_ns(uint64_t deadline) {
    for (;;) {
        uint64_t now = plat_ticks_ns();
        if (now >= deadline) return;
        uint64_t left = deadline - now;
        if (left <= 500000) { std::this_thread::yield(); continue; }
#ifdef _WIN32
        static thread_local HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (t) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 400000) / 100);   // relative, 100 ns units
            if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(t, INFINITE); continue; }
        }
        Sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::nanoseconds(left - 400000));
#endif
    }
}

// Threads with large host stacks: recompiled code nests host C frames for every guest call,
// so host stacks must be much larger than the guest's.
#ifdef _WIN32
#include <process.h>
struct ThreadStart { void (*fn)(void *); void *arg; };
static unsigned __stdcall thread_tramp(void *p) {
    ThreadStart s = *(ThreadStart *)p; delete (ThreadStart *)p;
    ULONG g = 128 * 1024; SetThreadStackGuarantee(&g);   // (a stack overflow can still run the crash report)
    s.fn(s.arg); return 0;
}
extern "C" void *plat_thread_create(void (*fn)(void *), void *arg, uint64_t stack) {
    return (void *)_beginthreadex(nullptr, (unsigned)stack, thread_tramp, new ThreadStart{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
}
extern "C" void plat_thread_join(void *t) { WaitForSingleObject((HANDLE)t, INFINITE); }
#else
#include <pthread.h>
struct ThreadStart { void (*fn)(void *); void *arg; };
static void *thread_tramp(void *p) { ThreadStart s = *(ThreadStart *)p; delete (ThreadStart *)p; s.fn(s.arg); return nullptr; }
extern "C" void *plat_thread_create(void (*fn)(void *), void *arg, uint64_t stack) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, stack);
    pthread_t *t = new pthread_t;
    if (pthread_create(t, &a, thread_tramp, new ThreadStart{fn, arg})) { delete t; return nullptr; }
    return t;
}
extern "C" void plat_thread_join(void *t) { pthread_join(*(pthread_t *)t, nullptr); }
#endif

// Address-space reservation + commit (Switch virtual memory API maps onto this).
extern "C" void *plat_reserve(uint64_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
#else
    void *p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
extern "C" int plat_commit(void *addr, uint64_t size) {
#ifdef _WIN32
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}
extern "C" int plat_decommit(void *addr, uint64_t size) {
#ifdef _WIN32
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
#else
    return madvise(addr, size, MADV_DONTNEED) == 0 && mprotect(addr, size, PROT_NONE) == 0;
#endif
}

// Crash handler: print where in the recompiled game the crash happened instead of exiting silently.
extern "C" void xrt_crash_report(const char *what, uintptr_t host_pc, uintptr_t fault_addr);
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation" :
                       code == EXCEPTION_STACK_OVERFLOW ? "stack overflow" :
                       code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction" :
                       code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "divide by zero" : "exception";
    uintptr_t fault = code == EXCEPTION_ACCESS_VIOLATION ? (uintptr_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    char buf[64]; std::snprintf(buf, sizeof buf, "%s (0x%08lx)", what, (unsigned long)code);
    xrt_crash_report(buf, (uintptr_t)e->ContextRecord->Rip, fault);
    return EXCEPTION_EXECUTE_HANDLER;
}
// The crash report must come out whatever ends the process: a C++ exception nobody catches (std::terminate), abort()
// (out of memory), a CRT invalid-parameter check, a pure virtual call - none of these reach the exception filter - and
// a driver DLL replacing our unhandled-exception filter (a vectored handler still sees the fault first). A small
// minidump goes next to the log (crash.dmp).
#include <dbghelp.h>
#include <intrin.h>
#ifdef _MSC_VER
#define RETURN_ADDRESS() _ReturnAddress()
#else
#define RETURN_ADDRESS() __builtin_return_address(0)
#endif
#include <csignal>
#include <exception>
#pragma comment(lib, "dbghelp.lib")
static void write_minidump(EXCEPTION_POINTERS *e) {
    static volatile LONG once; if (InterlockedExchange(&once, 1)) return;
    HANDLE f = CreateFileW(L"crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mi = {GetCurrentThreadId(), e, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      e ? &mi : nullptr, nullptr, nullptr);
    CloseHandle(f);
    std::fprintf(stderr, "  minidump: crash.dmp\n"); std::fflush(stderr);
}
static void fatal_report(const char *what) {
    static volatile LONG once; if (InterlockedExchange(&once, 1)) { std::fflush(stderr); return; }
    xrt_crash_report(what, (uintptr_t)RETURN_ADDRESS(), 0);
    write_minidump(nullptr);
    std::fflush(stderr); std::fflush(stdout);
}
static void on_terminate() {
    const char *why = "std::terminate (uncaught C++ exception)";
    char buf[256];
    if (std::exception_ptr ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception &x) { std::snprintf(buf, sizeof buf, "uncaught C++ exception: %s", x.what()); why = buf; }
        catch (...) {}
    }
    fatal_report(why);
    TerminateProcess(GetCurrentProcess(), 3);
}
static void on_abort(int) { fatal_report("abort()"); TerminateProcess(GetCurrentProcess(), 3); }
static void on_invalid_param(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) { fatal_report("CRT invalid parameter"); TerminateProcess(GetCurrentProcess(), 3); }
static void on_purecall() { fatal_report("pure virtual call"); TerminateProcess(GetCurrentProcess(), 3); }
static LONG CALLBACK fatal_veh(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374 /* heap corruption */ || code == 0xC0000409 /* stack buffer overrun */) {
        crash_filter(e);   // (these never reach the filter reliably)
        write_minidump(e);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI crash_filter_dump(EXCEPTION_POINTERS *e) { LONG r = crash_filter(e); write_minidump(e); std::fflush(stderr); return r; }
// Reads (and writes) of the guest's null page, like Eden / yuzu do: the Switch never maps the first pages, and an
// emulator answers an unmapped read with 0 and ignores an unmapped write - so game data that makes the game look at a
// missing table (randomizer seeds, mods: a table of a DLC that isn't installed ...) only crashes here. When recompiled
// code touches an address below 64 KB, the base (or index) register of the instruction is pointed at a zeroed block
// for that one instruction (single step), then put back if the instruction didn't load into it: the instruction reads
// zeros / its write is discarded, and the game carries on as it does in the emulators. XRT_NULL_READS=0: crash instead.
extern "C" void xrt_describe_host_pc(uintptr_t pc, char *out, size_t n);
static uint8_t *g_zero_block;                 // 128 KB: covers base 0..64 KB + displacement
static uintptr_t g_code_lo, g_code_hi;        // xc3.exe's code
static thread_local int t_nr_reg = -1; static thread_local DWORD64 t_nr_orig, t_nr_moved; static thread_local bool t_nr_write;
static std::atomic<int> g_nr_logged{0};
static LONG CALLBACK null_read_veh(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    DWORD64 *regs = &c->Rax;   // (Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8 .. R15: register number order)
    if (code == EXCEPTION_SINGLE_STEP && t_nr_reg >= 0) {
        if (regs[t_nr_reg] == t_nr_moved) regs[t_nr_reg] = t_nr_orig;   // (not the instruction's destination: put back)
        if (t_nr_write) std::memset(g_zero_block, 0, 1 << 17);
        t_nr_reg = -1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || !g_zero_block || e->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t fault = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1], rip = (uintptr_t)c->Rip;
    if (fault >= 0x10000 || rip < g_code_lo || rip >= g_code_hi) return EXCEPTION_CONTINUE_SEARCH;
    // decode up to the ModRM byte: legacy prefixes, REX, opcode (1 byte, 0F xx, 0F 38 xx, 0F 3A xx)
    const uint8_t *p = (const uint8_t *)rip; uint8_t rex = 0; int n = 0;
    while (n < 15) {
        uint8_t b = p[n];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 || b == 0xF0 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) { n++; continue; }
        break;
    }
    if ((p[n] & 0xF0) == 0x40) rex = p[n++];
    uint8_t op = p[n++];
    if (op == 0x0F) { op = p[n++]; if (op == 0x38 || op == 0x3A) n++; }
    else if (op >= 0xA0 && op <= 0xAF) return EXCEPTION_CONTINUE_SEARCH;   // (moffs / string forms: no ModRM)
    uint8_t modrm = p[n++];
    int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return EXCEPTION_CONTINUE_SEARCH;
    int base = -1, index = -1;
    if (rm == 4) {
        uint8_t sib = p[n++];
        base = (sib & 7) | ((rex & 1) << 3);
        index = ((sib >> 3) & 7) | ((rex & 2) << 2);
        if (index == 4) index = -1;                                    // (no index)
        if ((sib & 7) == 5 && mod == 0) base = -1;                     // (disp32, no base)
    } else if (rm == 5 && mod == 0) return EXCEPTION_CONTINUE_SEARCH;  // (RIP-relative)
    else base = rm | ((rex & 1) << 3);
    int reg = -1;
    if (base >= 0 && base != 4 && regs[base] < 0x10000) reg = base;
    else if (index >= 0 && regs[index] < 0x10000) reg = index;
    if (reg < 0) return EXCEPTION_CONTINUE_SEARCH;
    static int on = -1;
    if (on < 0) { const char *v = getenv("XRT_NULL_READS"); on = v ? atoi(v) : 1; }
    if (!on) return EXCEPTION_CONTINUE_SEARCH;
    t_nr_reg = reg; t_nr_orig = regs[reg]; t_nr_moved = (DWORD64)(uintptr_t)g_zero_block + regs[reg]; t_nr_write = e->ExceptionRecord->ExceptionInformation[0] == 1;
    regs[reg] = t_nr_moved;
    c->EFlags |= 0x100;   // (trap flag: back here after the instruction)
    int k = g_nr_logged.fetch_add(1);
    if (k < 8 || (k & (k - 1)) == 0) {
        char where[200]; xrt_describe_host_pc(rip, where, sizeof where);
        std::fprintf(stderr, "[mem] guest %s of address 0x%llx (null page) at %s: %s, like the emulators (%d so far; XRT_NULL_READS=0 to crash)\n",
                     t_nr_write ? "write" : "read", (unsigned long long)fault, where, t_nr_write ? "ignored" : "reads 0", k + 1);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
extern "C" void plat_install_crash_handler(void) {
    SetUnhandledExceptionFilter(crash_filter_dump);
    AddVectoredExceptionHandler(1, fatal_veh);
    {
        g_zero_block = (uint8_t *)VirtualAlloc(nullptr, 1 << 17, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        HMODULE m = GetModuleHandleW(nullptr);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
        g_code_lo = (uintptr_t)m; g_code_hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, null_read_veh);   // (first: before the crash handlers see it)
    }
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_param);
    _set_purecall_handler(on_purecall);
    ULONG g = 128 * 1024; SetThreadStackGuarantee(&g);
}
// (a graphics driver can install its own filter during device creation: ours goes back on top)
extern "C" void plat_reinstall_crash_filter(void) { SetUnhandledExceptionFilter(crash_filter_dump); }
#else
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void crash_sig(int sig, siginfo_t *si, void *uc) {
    uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
    xrt_crash_report(sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : sig == SIGFPE ? "arithmetic error" : "illegal instruction",
                     pc, (uintptr_t)si->si_addr);
    _exit(128 + sig);
}
extern "C" void plat_reinstall_crash_filter(void) {}
extern "C" void plat_install_crash_handler(void) {
    static char altstack[1 << 16];
    stack_t ss = {}; ss.ss_sp = altstack; ss.ss_size = sizeof altstack; sigaltstack(&ss, nullptr);
    struct sigaction sa = {}; sa.sa_sigaction = crash_sig; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
    for (int s : sigs) sigaction(s, &sa, nullptr);
}
#endif

// Pages written by the CPU since the last call (and reset), for memory reserved by plat_alloc / plat_reserve.
// pages: up to *count page addresses; returns 0 on success (then *count = pages reported, *page_size = granularity).
// Not available (returns -1) where the OS has no write tracking: callers fall back to sharing host memory.
extern "C" int plat_write_watch(void *base, uint64_t size, void **pages, uint64_t *count, uint64_t *page_size) {
#ifdef _WIN32
    ULONG_PTR n = (ULONG_PTR)*count; DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, (SIZE_T)size, pages, &n, &gran) != 0) return -1;
    *count = n; *page_size = gran;
    return 0;
#else
    (void)base; (void)size; (void)pages; (void)count; (void)page_size;
    return -1;
#endif
}
