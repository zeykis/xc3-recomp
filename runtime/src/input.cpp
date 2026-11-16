// Host input -> virtual Pro Controller (NpadFullKey) state.
// Sources: keyboard in the game window (gpu_vk.cpp forwards key events), an XInput gamepad
// (Xbox-style, Windows only, loaded at runtime) and XRT_AUTOPRESS scripts. All sources are combined.
//
// Keyboard (Switch button = key):
//   A = Enter or Z        B = Backspace or X     X = C        Y = V
//   L = Q                 R = E                  ZL = 1       ZR = 3
//   + = Tab               - = Shift              left stick click = F    right stick click = H
//   D-pad = arrow keys    left stick = WASD      right stick = IJKL
// Gamepad: buttons by position like a Switch Pro Controller (Xbox B = Switch A, A = B, Y = X, X = Y),
//   LB/RB = L/R, LT/RT = ZL/ZR, Start = +, Back = -, stick clicks, D-pad, both sticks.
// XRT_AUTOPRESS="A@40,A@45,X@80:4": press a button at the given second after start, for 0.2 s or the
// given hold time.
#include "input.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
extern "C" {
#include "xrt_internal.h"
}

enum : uint64_t {
    B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LS = 1 << 4, B_RS = 1 << 5, B_L = 1 << 6, B_R = 1 << 7,
    B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12, B_UP = 1 << 13, B_RIGHT = 1 << 14, B_DOWN = 1 << 15,
};

static std::atomic<uint64_t> g_keys{0};          // buttons held via keyboard
static std::atomic<int> g_stick_bits{0};         // WASD/IJKL bits

struct Press { uint64_t mask; uint64_t at_ns; uint64_t dur_ns; };
static std::vector<Press> g_script;
static std::once_flag g_once;

static uint64_t button_by_name(const std::string &n) {
    static const char *names[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    for (int i = 0; i < 16; i++) if (n == names[i]) return 1ULL << i;
    return 0;
}
static void parse_script() {
    const char *e = getenv("XRT_AUTOPRESS");
    if (!e) return;
    std::string s = e;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        std::string item = s.substr(p, q - p);
        size_t at = item.find('@');
        if (at != std::string::npos) {   // BUTTON@seconds[:hold seconds]
            uint64_t m = button_by_name(item.substr(0, at));
            std::string when = item.substr(at + 1);
            double t = atof(when.c_str()), hold = 0.2;
            size_t colon = when.find(':'); if (colon != std::string::npos) hold = atof(when.substr(colon + 1).c_str());
            if (m) g_script.push_back({m, (uint64_t)(t * 1e9), (uint64_t)(hold * 1e9)});
        }
        p = q + 1;
    }
    LOG(1, "[input] %zu scripted presses", g_script.size());
}

// ---------------------------------------------------------------- XInput gamepad (Windows)
#ifdef _WIN32
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XState *);
static XInputGetStateFn g_xget;
static std::once_flag g_xonce;
static void xinput_load() {
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char *d : dlls) {
        if (HMODULE h = LoadLibraryA(d)) { g_xget = (XInputGetStateFn)(void *)GetProcAddress(h, "XInputGetState"); if (g_xget) { LOG(1, "[input] gamepad support via %s", d); return; } }
    }
}
// polled at most every 4 ms; first connected pad wins
static bool xinput_poll(XPad &out) {
    std::call_once(g_xonce, xinput_load);
    if (!g_xget) return false;
    static uint64_t last_ns; static XPad cached; static bool have; static int slot = -1; static uint64_t retry_ns;
    uint64_t now = plat_ticks_ns();
    if (now - last_ns < 4000000ULL) { out = cached; return have; }
    last_ns = now;
    have = false;
    if (slot < 0 && now < retry_ns) return false;          // nothing plugged in: rescan once a second
    for (int i = slot < 0 ? 0 : slot; i < 4; i++) {
        XState st;
        if (g_xget((DWORD)i, &st) == 0) {
            if (slot != i) LOG(1, "[input] gamepad %d connected", i);
            slot = i; cached = st.pad; have = true; break;
        }
        if (slot >= 0) { LOG(1, "[input] gamepad %d disconnected", slot); slot = -1; break; }
    }
    if (!have) retry_ns = now + 1000000000ULL;
    out = cached; return have;
}
static int32_t stick_axis(SHORT v) {
    const int dead = 7849;                       // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    int x = v;
    if (x > -dead && x < dead) return 0;
    x = x < 0 ? (x + dead) * 32767 / (32768 - dead) : (x - dead) * 32767 / (32767 - dead);
    return x < -32767 ? -32767 : x > 32767 ? 32767 : x;
}
static uint64_t pad_buttons(const XPad &p) {
    uint64_t b = 0; WORD w = p.buttons;
    if (w & 0x1000) b |= B_B;      // Xbox A (bottom)  -> Switch B (bottom)
    if (w & 0x2000) b |= B_A;      // Xbox B (right)   -> Switch A (right)
    if (w & 0x4000) b |= B_Y;      // Xbox X (left)    -> Switch Y (left)
    if (w & 0x8000) b |= B_X;      // Xbox Y (top)     -> Switch X (top)
    if (w & 0x0100) b |= B_L;  if (w & 0x0200) b |= B_R;
    if (p.lt > 64) b |= B_ZL;  if (p.rt > 64) b |= B_ZR;
    if (w & 0x0010) b |= B_PLUS;  if (w & 0x0020) b |= B_MINUS;
    if (w & 0x0040) b |= B_LS;    if (w & 0x0080) b |= B_RS;
    if (w & 0x0001) b |= B_UP;    if (w & 0x0002) b |= B_DOWN;
    if (w & 0x0004) b |= B_LEFT;  if (w & 0x0008) b |= B_RIGHT;
    return b;
}
#endif

uint64_t input_buttons() {
    std::call_once(g_once, parse_script);
    uint64_t b = g_keys.load();
    uint64_t now = plat_ticks_ns();
    for (const Press &p : g_script)
        if (now >= p.at_ns && now < p.at_ns + p.dur_ns) b |= p.mask;
#ifdef _WIN32
    XPad pad; if (xinput_poll(pad)) b |= pad_buttons(pad);
#endif
    return b;
}
void input_sticks(int32_t out[4]) {
    int s = g_stick_bits.load();
    const int32_t M = 0x7fff;
    out[0] = (s & 8 ? M : 0) - (s & 2 ? M : 0);     // D right, A left
    out[1] = (s & 1 ? M : 0) - (s & 4 ? M : 0);     // W up,  S down
    out[2] = (s & 128 ? M : 0) - (s & 32 ? M : 0);  // L right, J left
    out[3] = (s & 16 ? M : 0) - (s & 64 ? M : 0);   // I up,  K down
#ifdef _WIN32
    XPad pad;
    if (xinput_poll(pad)) {   // keyboard wins on an axis it is pushing; otherwise the pad's analog value
        int32_t v[4] = {stick_axis(pad.lx), stick_axis(pad.ly), stick_axis(pad.rx), stick_axis(pad.ry)};
        for (int i = 0; i < 4; i++) if (out[i] == 0) out[i] = v[i];
    }
#endif
}

// key: Windows virtual-key code
void input_key(int vk, bool down) {
    uint64_t m = 0; int sb = 0;
    switch (vk) {
    case 0x0D: case 'Z': m = B_A; break;             // Enter / Z -> A
    case 0x08: case 'X': m = B_B; break;             // Backspace / X -> B
    case 'C': m = B_X; break;  case 'V': m = B_Y; break;
    case 'Q': m = B_L; break;  case 'E': m = B_R; break;
    case '1': m = B_ZL; break; case '3': m = B_ZR; break;
    case 0x1B: case 'P': m = B_PLUS; break;          // Escape / P -> +  (Tab opens the settings overlay)
    case 0x10: case 0xA0: case 0xA1: m = B_MINUS; break;   // Shift -> -
    case 'F': m = B_LS; break; case 'H': m = B_RS; break;  // stick clicks
    case 0x25: m = B_LEFT; break; case 0x26: m = B_UP; break;
    case 0x27: m = B_RIGHT; break; case 0x28: m = B_DOWN; break;
    case 'W': sb = 1; break; case 'A': sb = 2; break; case 'S': sb = 4; break; case 'D': sb = 8; break;
    case 'I': sb = 16; break; case 'J': sb = 32; break; case 'K': sb = 64; break; case 'L': sb = 128; break;
    }
    if (m) { if (down) g_keys |= m; else g_keys &= ~m; }
    if (sb) { if (down) g_stick_bits |= sb; else g_stick_bits &= ~sb; }
}
// the window lost focus: release everything so no key stays stuck
void input_release_all() { g_keys = 0; g_stick_bits = 0; }
