// Debug tool (XRT_FIND_REPEAT=1): finds the game's "frames a direction has been held" counter (menu cursor repeat)
// and the recompiled code that writes it. Hold a direction (d-pad or left stick) in a menu for a second or more, a few
// times. Per hold:
//   frame 20 of the hold: the first hold scans all guest memory for values that could be the counter (an int of
//            14..26, or a float that is a multiple of 1/30, 1/60 or 1/120 there); later holds re-read the survivors
//   frame 40: survivors went up by exactly 20 frames (int +20, float +20 steps)
//   3 frames after release: survivors went back to (nearly) 0
// When 64 or fewer are left, their pages get guard pages: the recompiled function that writes one is logged
// ("[findctr] write ... by recompiled f_xxxxxxx"). Everything goes to the log as [findctr] lines.
extern "C" {
#include "xrt_internal.h"
}
#include "input.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <vector>
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" int hle_guest_regions(uint64_t *base, uint64_t *size, int max);
extern "C" void xrt_describe_host_pc(uintptr_t pc, char *out, size_t n);

namespace {
struct Cand { uint64_t addr; uint32_t v20; uint8_t kind; };   // kind 0 = int, 1/2/3 = float in 1/30, 1/60, 1/120 steps
const float STEP[4] = {1.0f, 1.0f / 30, 1.0f / 60, 1.0f / 120};
const uint64_t DIR_MASK = 0xFF000;   // d-pad 12..15, left stick 16..19

std::vector<Cand> g_cand;
bool g_have = false;                  // a candidate set exists (after the first complete hold)
int g_holds = 0;

// guard pages
std::mutex g_gmu;
std::set<uint64_t> g_gpages;          // armed pages
std::set<uint64_t> g_gaddr;           // candidate addresses watched
std::set<uintptr_t> g_seen_pc;
std::atomic<bool> g_guard_on{false};
thread_local uint64_t t_rearm;

bool readable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
int float_kind(float f, int lo, int hi) {
    if (!(f > 0) || !std::isfinite(f)) return 0;
    for (int k = 1; k <= 3; k++) {
        float n = f / STEP[k];
        if (n >= lo - 0.01f && n <= hi + 0.01f && std::fabs(n - std::round(n)) < 0.01f) return k;
    }
    return 0;
}
template <class F> void each_committed(uint64_t base, uint64_t size, F fn) {
    uint64_t a = base, end = base + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t rb = (uint64_t)(uintptr_t)mi.BaseAddress, re = rb + mi.RegionSize;
        if (re > end) re = end;
        if (mi.State == MEM_COMMIT && readable(mi.Protect)) fn(a, re);
        a = re;
    }
}
void full_scan() {
    g_cand.clear();
    uint64_t rb[64], rs[64];
    int n = hle_guest_regions(rb, rs, 62);
    rb[n] = (uint64_t)(uintptr_t)g_img.base + 0x1a92000; rs[n] = 0x8d9000; n++;   // the game's .data / .bss
    uint64_t bytes = 0;
    for (int r = 0; r < n; r++) {
        each_committed(rb[r], rs[r], [&](uint64_t a, uint64_t e) {
            bytes += e - a;
            const uint32_t *p = (const uint32_t *)(uintptr_t)a, *pe = (const uint32_t *)(uintptr_t)e;
            for (; p < pe; p++) {
                uint32_t v = *p;
                if (v >= 14 && v <= 26) { g_cand.push_back({(uint64_t)(uintptr_t)p, v, 0}); continue; }
                if ((v >> 23) - 0x7a < 6) {   // floats 2^-5 .. 2^1 only (exponent 0x7a..0x7f)
                    float f; std::memcpy(&f, &v, 4);
                    if (int k = float_kind(f, 14, 26)) g_cand.push_back({(uint64_t)(uintptr_t)p, v, (uint8_t)k});
                }
                if (g_cand.size() > 20000000) { LOG(0, "[findctr] too many candidates - scan cut short"); return; }
            }
        });
    }
    LOG(0, "[findctr] scanned %.0f MB in %d regions: %zu candidates", bytes / 1048576.0, n, g_cand.size());
}
// (one VirtualQuery per memory region, not per candidate: the candidates are in address order)
struct RangeCache {
    uint64_t lo = 0, hi = 0; bool ok = false;
    bool check(uint64_t a) {
        if (a >= lo && a < hi) return ok;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) { lo = a & ~0xfffull; hi = lo + 4096; ok = false; return false; }
        lo = (uint64_t)(uintptr_t)mi.BaseAddress; hi = lo + mi.RegionSize;
        ok = mi.State == MEM_COMMIT && (readable(mi.Protect) || (mi.Protect & PAGE_GUARD));
        return ok;
    }
};
thread_local RangeCache t_rc;
bool still_readable(uint64_t a) { return t_rc.check(a); }
uint32_t rd(uint64_t a) { return *(volatile const uint32_t *)(uintptr_t)a; }
float as_f(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }

void list(const char *what) {
    LOG(0, "[findctr] %s: %zu candidates", what, g_cand.size());
    if (g_cand.size() <= 32)
        for (auto &c : g_cand) {
            uint32_t v = rd(c.addr);
            if (c.kind) LOG(0, "[findctr]   %p float (1/%d steps) now %.4f", (void *)(uintptr_t)c.addr, (int)std::lround(1 / STEP[c.kind]), as_f(v));
            else LOG(0, "[findctr]   %p int now %u", (void *)(uintptr_t)c.addr, v);
        }
}
void filter(bool (*keep)(const Cand &, uint32_t)) {
    t_rc = RangeCache(); size_t o = 0;
    for (auto &c : g_cand) {
        if (!still_readable(c.addr)) continue;
        uint32_t v = rd(c.addr);
        if (keep(c, v)) g_cand[o++] = c;
    }
    g_cand.resize(o);
}
void arm_guards() {
    std::lock_guard<std::mutex> lk(g_gmu);
    g_gaddr.clear();
    for (auto &c : g_cand) g_gaddr.insert(c.addr & ~3ull);
    for (auto &c : g_cand) {
        uint64_t pg = c.addr & ~0xfffull;
        if (g_gpages.count(pg)) continue;
        DWORD old;
        if (VirtualProtect((void *)(uintptr_t)pg, 4096, PAGE_READWRITE | PAGE_GUARD, &old)) g_gpages.insert(pg);
    }
    g_guard_on = true;
    LOG(0, "[findctr] watching writes to %zu addresses (%zu pages)", g_gaddr.size(), g_gpages.size());
}
LONG CALLBACK guard_veh(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == STATUS_SINGLE_STEP && t_rearm) {
        DWORD old; VirtualProtect((void *)(uintptr_t)t_rearm, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
        t_rearm = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != STATUS_GUARD_PAGE_VIOLATION || !g_guard_on.load()) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    uint64_t pg = fa & ~0xfffull;
    bool write = e->ExceptionRecord->ExceptionInformation[0] == 1;
    {
        std::lock_guard<std::mutex> lk(g_gmu);
        if (!g_gpages.count(pg)) return EXCEPTION_CONTINUE_SEARCH;
        if (write && g_gaddr.count(fa & ~3ull)) {
            uintptr_t pc = (uintptr_t)e->ContextRecord->Rip;
            if (g_seen_pc.size() < 200 && g_seen_pc.insert(pc).second) {
                char where[200]; xrt_describe_host_pc(pc, where, sizeof where);
                LOG(0, "[findctr] write to %p (now %u / %.4f) by %s", (void *)(uintptr_t)fa, rd(fa & ~3ull), as_f(rd(fa & ~3ull)), where);
            }
        }
    }
    t_rearm = pg;                                // re-arm after this instruction
    e->ContextRecord->EFlags |= 0x100;           // (trap flag: single step)
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

extern "C" void findctr_frame(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XRT_FIND_REPEAT"); on = e && atoi(e) ? 1 : 0;
        if (on) { AddVectoredExceptionHandler(1, guard_veh); LOG(0, "[findctr] on: hold a direction in a menu for a second or more, a few times"); }
    }
    if (!on) return;
    static int hf = 0, rel = -1; static uint64_t dir = 0; static bool scanned = false, ok40 = false;
    uint64_t d = input_buttons() & DIR_MASK;
    { int32_t st[4]; input_sticks(st);   // (the left stick: bits 16..19 like the Switch's stick directions)
      if (st[0] < -0x4000) d |= 1 << 16; if (st[1] > 0x4000) d |= 1 << 17; if (st[0] > 0x4000) d |= 1 << 18; if (st[1] < -0x4000) d |= 1 << 19; }
    if (d && d == dir) hf++;
    else if (d) { dir = d; hf = 1; scanned = ok40 = false; rel = -1; }
    else if (dir) {   // released
        if (ok40) rel = 0;
        dir = 0; hf = 0;
    }
    if (d && hf == 20) {
        if (!g_have) full_scan();
        else { t_rc = RangeCache(); for (auto &c : g_cand) if (still_readable(c.addr)) c.v20 = rd(c.addr); }
        if (g_have) filter([](const Cand &c, uint32_t v) { return c.kind ? float_kind(as_f(v), 14, 26) == c.kind : (v >= 14 && v <= 26); });
        t_rc = RangeCache(); for (auto &c : g_cand) if (still_readable(c.addr)) c.v20 = rd(c.addr);
        scanned = true;
        list("hold frame 20");
    }
    if (d && hf == 40 && scanned) {
        filter([](const Cand &c, uint32_t v) {
            if (!c.kind) return v == c.v20 + 20;
            return std::fabs(as_f(v) - as_f(c.v20) - 20 * STEP[c.kind]) < 0.003f;
        });
        ok40 = true;
        list("hold frame 40 (+20 frames)");
    }
    if (rel >= 0 && ++rel == 4) {
        filter([](const Cand &c, uint32_t v) { return c.kind ? as_f(v) < 0.06f : v <= 3; });
        rel = -1; ok40 = false; g_have = true; g_holds++;
        char w[64]; std::snprintf(w, sizeof w, "after release (hold %d)", g_holds);
        list(w);
        if (g_cand.empty()) { g_have = false; LOG(0, "[findctr] nothing left - starting over with the next hold"); }
        else if (g_cand.size() <= 64) arm_guards();
    }
    if (!d && !g_have && scanned && !ok40) { scanned = false; g_cand.clear(); }
}
#else
extern "C" void findctr_frame(void) {}
#endif
