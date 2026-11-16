// Sampling profiler for the recompiled game code (milestone M5). XRT_SAMPLE=1 (or =<interval in microseconds>):
// a host thread interrupts every registered guest thread about once per millisecond, reads where it is running and
// maps that host address to the recompiled function containing it (f_<guest offset>, i.e. main+offset). Every 5 s it
// logs, per kind of thread, how much time goes to game code vs runtime/SDK/driver code and the hottest game functions.
// Windows only (SuspendThread / GetThreadContext); elsewhere the calls do nothing.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
extern "C" {
#include "xrt_internal.h"
}

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <thread>
#pragma comment(lib, "dbghelp.lib")

struct FnEntryS { uint32_t off; void *fn; };   // same layout as loader.c's FnEntry
extern "C" const unsigned xrt_fn_count;
extern "C" const FnEntryS xrt_fn_table[];

namespace {
struct SThread { HANDLE h; std::string label; uint64_t total = 0, host = 0, cycles = 0; std::unordered_map<uint32_t, uint32_t> fn; std::unordered_map<uint64_t, uint32_t> hostpc; };
std::mutex g_m;
std::vector<SThread *> g_threads;
std::vector<std::pair<uintptr_t, uint32_t>> g_sorted;   // host fn address -> guest offset
int g_on = -1, g_interval_us = 1000;

int on() {
    if (g_on < 0) { const char *e = getenv("XRT_SAMPLE"); g_on = e ? 1 : 0; if (e && atoi(e) > 1) g_interval_us = atoi(e); }
    return g_on;
}
// recompiled function containing pc, or -1 (runtime / SDK HLE / driver / OS code)
int64_t lookup(uintptr_t pc) {
    if (g_sorted.empty()) return -1;
    auto it = std::upper_bound(g_sorted.begin(), g_sorted.end(), std::make_pair(pc, UINT32_MAX));
    if (it == g_sorted.begin()) return -1;
    --it;
    if (it + 1 == g_sorted.end() || pc - it->first > (1u << 16)) return -1;   // past the last function: runtime code
    return it->second;
}
// host address -> "module!function" (dbghelp; the runtime's own functions need xc3.pdb next to the exe)
std::string host_name(uint64_t pc) {
    static bool init; static std::unordered_map<uint64_t, std::string> cache;
    if (!init) { init = true; SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), nullptr, TRUE); }
    auto it = cache.find(pc); if (it != cache.end()) return it->second;
    char mod[MAX_PATH] = "?"; HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &hm) && hm) {
        char full[MAX_PATH]; if (GetModuleFileNameA(hm, full, MAX_PATH)) { const char *b = strrchr(full, '\\'); snprintf(mod, sizeof mod, "%s", b ? b + 1 : full); }
    }
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; DWORD64 disp = 0;
    std::string r = mod;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, si)) { r += "!"; r += si->Name; }
    return cache[pc] = r;
}
void report(double secs) {
    // group threads by label (the game's job workers share one entry point)
    struct G { uint64_t total = 0, host = 0; int n = 0; std::unordered_map<uint32_t, uint64_t> fn; std::unordered_map<std::string, uint64_t> host_fn; };
    std::vector<std::pair<std::string, G>> groups;
    std::unordered_map<uint32_t, uint64_t> all;
    uint64_t all_total = 0;
    for (SThread *t : g_threads) {
        if (!t->total) continue;
        auto g = std::find_if(groups.begin(), groups.end(), [&](auto &p) { return p.first == t->label; });
        if (g == groups.end()) { groups.push_back({t->label, G{}}); g = groups.end() - 1; }
        g->second.total += t->total; g->second.host += t->host; g->second.n++;
        for (auto &kv : t->fn) { g->second.fn[kv.first] += kv.second; all[kv.first] += kv.second; }
        for (auto &kv : t->hostpc) g->second.host_fn[host_name(kv.first)] += kv.second;
        t->hostpc.clear();
        all_total += t->total - t->host;
        t->total = t->host = 0; t->fn.clear();
    }
    double per_ms = g_interval_us / 1000.0;   // ms of thread time one sample stands for
    std::sort(groups.begin(), groups.end(), [](auto &a, auto &b) { return a.second.total > b.second.total; });
    LOG(1, "[sample] %.1f s: CPU time per thread kind, ms per second (game = recompiled game code, host = runtime / SDK / driver / OS):", secs);
    for (auto &g : groups) {
        LOG(1, "[sample]   %-28s x%d  game %6.1f  host %6.1f", g.first.c_str(), g.second.n,
            (g.second.total - g.second.host) * per_ms / secs, g.second.host * per_ms / secs);
    }
    for (int gi = 0; gi < (int)groups.size() && gi < 4; gi++) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto &kv : groups[gi].second.fn) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        LOG(1, "[sample]   hottest game functions on %s:", groups[gi].first.c_str());
        for (int k = 0; k < (int)v.size() && k < 12; k++)
            LOG(1, "[sample]     %6.1f ms/s  main+0x%x", v[k].first * per_ms / secs, v[k].second);
        std::vector<std::pair<uint64_t, std::string>> h;
        for (auto &kv : groups[gi].second.host_fn) h.push_back({kv.second, kv.first});
        std::sort(h.rbegin(), h.rend());
        LOG(1, "[sample]   hottest host functions on %s:", groups[gi].first.c_str());
        for (int k = 0; k < (int)h.size() && k < 30; k++)
            LOG(1, "[sample]     %6.1f ms/s  %s", h[k].first * per_ms / secs, h[k].second.c_str());
    }
    (void)all; (void)all_total;
}
void sampler_main() {
    {
        std::lock_guard<std::mutex> l(g_m);
        g_sorted.reserve(xrt_fn_count);
        for (unsigned i = 0; i < xrt_fn_count; i++) g_sorted.push_back({(uintptr_t)xrt_fn_table[i].fn, xrt_fn_table[i].off});
        std::sort(g_sorted.begin(), g_sorted.end());
    }
    LOG(1, "[sample] sampling profiler on: every %d us, %u functions", g_interval_us, xrt_fn_count);
    uint64_t t_rep = plat_ticks_ns();
    for (;;) {
        plat_sleep_until_ns(plat_ticks_ns() + (uint64_t)g_interval_us * 1000);
        std::lock_guard<std::mutex> l(g_m);
        for (SThread *t : g_threads) {
            // a thread that used (almost) no CPU since the last sample was blocked: not counted
            ULONG64 cyc = 0; QueryThreadCycleTime(t->h, &cyc);
            bool idle = cyc - t->cycles < 200000; t->cycles = cyc;
            if (idle) continue;
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(t->h, &ctx);
            ResumeThread(t->h);
            if (!ok) continue;
            int64_t f = lookup((uintptr_t)ctx.Rip);
            t->total++;
            if (f < 0) { t->host++; t->hostpc[ctx.Rip]++; } else t->fn[(uint32_t)f]++;
        }
        uint64_t now = plat_ticks_ns();
        if (now - t_rep >= 5000000000ULL) { report((now - t_rep) / 1e9); t_rep = now; }
    }
}
}  // namespace

extern "C" void xrt_sampler_register_thread(const char *label) {
    if (!on()) return;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!h) return;
    std::lock_guard<std::mutex> l(g_m);
    SThread *t = new SThread; t->h = h; t->label = label ? label : "?";
    g_threads.push_back(t);
    static bool started;
    if (!started) { started = true; std::thread(sampler_main).detach(); }
}
#else
extern "C" void xrt_sampler_register_thread(const char *) {}
#endif
