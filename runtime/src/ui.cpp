// TAB settings overlay (milestone M5): window size / mode, internal resolution, frame rate caps, present mode, FSR,
// volume. Everything but the internal resolution applies at once; Save writes xc3.ini (settings.cpp).
#include "ui.h"
#include "settings.h"
#include "input.h"
#include "imgui.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#endif
extern "C" {
#include "xrt_internal.h"
}

namespace {
std::atomic<bool> g_open{false};
struct Ev { int type; int a, b; float f; };   // 0 mouse pos, 1 button, 2 wheel, 3 key, 4 char, 5 focus lost
std::mutex g_evm;
std::vector<Ev> g_ev;
bool g_init;
float g_boot_scale;
uint64_t g_last_ns;
double g_saved_at = -100;
bool g_save_ok;

void push(int type, int a, int b = 0, float f = 0) { std::lock_guard<std::mutex> l(g_evm); if (g_ev.size() < 4096) g_ev.push_back({type, a, b, f}); }

#ifdef _WIN32
ImGuiKey vk_to_key(WPARAM vk) {
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home; case VK_END: return ImGuiKey_End;
    case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace; case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return ImGuiKey_LeftCtrl;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return ImGuiKey_LeftShift;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return ImGuiKey_LeftAlt;
    case 'A': return ImGuiKey_A; case 'C': return ImGuiKey_C; case 'V': return ImGuiKey_V; case 'X': return ImGuiKey_X;
    case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}
#endif
}  // namespace

bool ui_visible() { return g_open.load(std::memory_order_relaxed); }

bool ui_on_msg(void *hwnd, unsigned m, uintptr_t w, intptr_t l) {
#ifdef _WIN32
    (void)hwnd;
    if (m == WM_KEYDOWN && w == VK_TAB) {
        if (!(l & (1 << 30))) {   // (not auto-repeat)
            bool o = !g_open.load();
            g_open = o;
            input_release_all();   // no key stays held in the game while the menu is up
            if (o) push(5, 0);
        }
        return true;
    }
    if (!g_open.load(std::memory_order_relaxed)) return false;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_ESCAPE) { g_open = false; input_release_all(); return true; }
        push(3, (int)vk_to_key(w), 1); return true;
    case WM_KEYUP: case WM_SYSKEYUP: push(3, (int)vk_to_key(w), 0); return true;
    case WM_CHAR: push(4, (int)w); return true;
    case WM_MOUSEMOVE: push(0, GET_X_LPARAM(l), GET_Y_LPARAM(l)); return true;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: push(1, 0, m == WM_LBUTTONDOWN); return true;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: push(1, 1, m == WM_RBUTTONDOWN); return true;
    case WM_MOUSEWHEEL: push(2, 0, 0, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA); return true;
    }
#else
    (void)hwnd; (void)m; (void)w; (void)l;
#endif
    return false;
}

void ui_init(int win_h) {
    if (g_init) return;
    g_init = true;
    g_boot_scale = settings().res_scale;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;    // (no imgui.ini: our settings live in xc3.ini)
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float px = std::round(std::max(16.0f, win_h / 1080.0f * 20.0f));
    ImFont *f = nullptr;
#ifdef _WIN32
    char path[MAX_PATH]; UINT n = GetWindowsDirectoryA(path, MAX_PATH);
    if (n && n < MAX_PATH - 32) { strcat(path, "\\Fonts\\segoeui.ttf"); if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) f = io.Fonts->AddFontFromFileTTF(path, px); }
#endif
    if (!f) { ImFontConfig c; c.SizePixels = std::round(px * 0.8f); io.Fonts->AddFontDefault(&c); }
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6; s.FrameRounding = 4; s.GrabRounding = 4; s.WindowPadding = ImVec2(14, 12); s.ItemSpacing = ImVec2(10, 8);
    s.Colors[ImGuiCol_WindowBg].w = 0.94f;
    s.ScaleAllSizes(px / 18.0f);
    LOG(1, "[ui] overlay ready (TAB), font %.0f px", px);
}

static const char *pm_names[] = {"VSync (FIFO)", "Fast VSync (mailbox)", "Off (may tear)"};
static const char *up_names[] = {"Bilinear", "AMD FSR 1"};
static const char *wm_names[] = {"Windowed", "Borderless fullscreen"};

static void settings_window(int src_w, int src_h) {
    XrtSettings &s = settings();
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(io.FontGlobalScale * 300, 0), ImVec2(io.DisplaySize.x * 0.95f, io.DisplaySize.y * 0.95f));
    ImGui::Begin("Settings  (TAB / Esc to close)", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    const float lw = ImGui::GetFontSize() * 16;
    bool win_changed = false;

    ImGui::SeparatorText("Display");
    ImGui::SetNextItemWidth(lw);
    if (ImGui::Combo("Window mode", &s.window_mode, wm_names, 2)) win_changed = true;
    {
        static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
        char cur[32]; snprintf(cur, sizeof cur, "%d x %d", s.window_w, s.window_h);
        ImGui::BeginDisabled(s.window_mode == WIN_BORDERLESS);
        ImGui::SetNextItemWidth(lw);
        if (ImGui::BeginCombo("Window size", cur)) {
            for (auto &z : sizes) {
                char b[32]; snprintf(b, sizeof b, "%d x %d", z[0], z[1]);
                if (ImGui::Selectable(b, s.window_w == z[0] && s.window_h == z[1])) { s.window_w = z[0]; s.window_h = z[1]; win_changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(lw);
    if (ImGui::Combo("VSync", &s.present_mode, pm_names, 3)) g_settings_gen++;
    bool ka = s.keep_aspect != 0;
    if (ImGui::Checkbox("Keep 16:9 (black bars)", &ka)) s.keep_aspect = ka;

    ImGui::SeparatorText("Rendering");
    ImGui::SetNextItemWidth(lw);
    ImGui::SliderFloat("Resolution scale", &s.res_scale, 1.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    {
        auto r16 = [](double v) { return ((int)(v + 8) / 16) * 16; };
        ImGui::TextDisabled("3D scene %d x %d  (Switch: 960 x 544)", r16(960 * s.res_scale), r16(544 * s.res_scale));
        if (std::fabs(s.res_scale - g_boot_scale) > 0.001f) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Restart the game to apply (Save first)");
    }
    ImGui::SetNextItemWidth(lw);
    ImGui::Combo("Upscaler", &s.upscaler, up_names, 2);
    ImGui::BeginDisabled(s.upscaler != UP_FSR);
    ImGui::SetNextItemWidth(lw);
    float sharp = 2.0f - s.fsr_sharpness;   // (shown as "more = sharper")
    if (ImGui::SliderFloat("FSR sharpness", &sharp, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) s.fsr_sharpness = 2.0f - sharp;
    ImGui::EndDisabled();
    if (src_w > 0) ImGui::TextDisabled("Game image %d x %d -> window %d x %d%s", src_w, src_h, (int)io.DisplaySize.x, (int)io.DisplaySize.y,
                                       s.upscaler == UP_FSR && (io.DisplaySize.x <= src_w && io.DisplaySize.y <= src_h) ? "  (no upscale: FSR idle)" : "");

    if (int nm = romfs_mod_count()) {
        ImGui::SeparatorText("Mods (mods folder)");
        for (int i = 0; i < nm; i++) {
            bool on = romfs_mod_on(i);
            std::string label = romfs_mod_name(i) + "##rm" + std::to_string(i);
            if (ImGui::Checkbox(label.c_str(), &on)) romfs_mod_set(i, on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%zu files. Files the game already loaded change at the next area load or restart.", romfs_mod_files(i));
        }
    } else {
        ImGui::SeparatorText("Mods");
        ImGui::TextDisabled("Put mods (for emulators) in the game folder's mods\\ folder");
    }
    if (xrt_mod_count) {
        ImGui::SeparatorText("Graphics mods");
        for (unsigned i = 0; i < xrt_mod_count && i < 64; i++) {
            bool on = xrt_mod_flags[i] != 0;
            if (ImGui::Checkbox(xrt_mod_names[i], &on)) xrt_mod_flags[i] = on;
            if (xrt_mod_descs[i] && xrt_mod_descs[i][0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", xrt_mod_descs[i]);
        }
        ImGui::TextDisabled("Some take effect on the next area / cutscene load, or after a restart");
    }

    ImGui::SeparatorText("Frame rate");
    {
        static const int caps[] = {30, 60, 90, 120, 144, 165, 240, 0};
        auto pick = [&](const char *label, int &v) {
            char cur[24]; if (v) snprintf(cur, sizeof cur, "%d fps", v); else snprintf(cur, sizeof cur, "Unlimited");
            ImGui::SetNextItemWidth(lw);
            if (ImGui::BeginCombo(label, cur)) {
                for (int c : caps) { char b[24]; if (c) snprintf(b, sizeof b, "%d fps", c); else snprintf(b, sizeof b, "Unlimited");
                                     if (ImGui::Selectable(b, v == c)) v = c; }
                ImGui::EndCombo();
            }
        };
        bool fg = s.frame_gen != 0;
        if (ImGui::Checkbox("Frame generation (x2)", &fg)) s.frame_gen = fg;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A generated image between game frames (optical flow, like Lossless Scaling).\nThe game runs at half the display refresh rate, twice that is shown.\nAbout half a frame more latency; some warping around fast motion and the HUD.");
        int hz = gpu_display_hz();
        if (s.frame_gen) ImGui::TextDisabled(hz > 0 ? "Display %d Hz: game at %d, %d shown" : "Display refresh unknown", hz, hz / 2, hz);
        pick("Frame cap", s.fps_cap);
        pick("Menu frame cap", s.menu_fps);
        ImGui::TextDisabled("Menus animate per frame: above 60 they run fast");
        bool sf = s.show_fps != 0;
        if (ImGui::Checkbox("Show frame rate", &sf)) s.show_fps = sf;
    }

    ImGui::SeparatorText("Audio");
    ImGui::SetNextItemWidth(lw);
    ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);

    ImGui::Spacing(); ImGui::Separator();
    if (ImGui::Button("Save")) { g_save_ok = settings_save(); g_saved_at = ImGui::GetTime(); }
    ImGui::SameLine();
    if (ImGui::Button("Close")) { g_open = false; input_release_all(); }
    ImGui::SameLine();
    if (ImGui::GetTime() - g_saved_at < 3) ImGui::TextColored(g_save_ok ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1), g_save_ok ? "Saved to %s" : "Could not write %s", settings_path());
    else ImGui::TextDisabled("Keys: Esc/P = +, Shift = -");
    ImGui::End();
    if (win_changed) gpu_window_apply();
}

ImDrawData *ui_frame(int w, int h, double fps, int src_w, int src_h) {
    if (!g_init) return nullptr;
    bool open = g_open.load(std::memory_order_relaxed);
    const XrtSettings &s = settings();
    ImGuiIO &io = ImGui::GetIO();
    std::vector<Ev> ev;
    { std::lock_guard<std::mutex> l(g_evm); ev.swap(g_ev); }
    for (const Ev &e : ev) {
        switch (e.type) {
        case 0: io.AddMousePosEvent((float)e.a, (float)e.b); break;
        case 1: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case 2: io.AddMouseWheelEvent(0, e.f); break;
        case 3: if (e.a != ImGuiKey_None) {
                    io.AddKeyEvent((ImGuiKey)e.a, e.b != 0);
                    if (e.a == ImGuiKey_LeftCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
                    if (e.a == ImGuiKey_LeftShift) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
                    if (e.a == ImGuiKey_LeftAlt) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
                } break;
        case 4: if (e.a > 0 && e.a < 0x10000) io.AddInputCharacter((unsigned)e.a); break;
        case 5: io.ClearInputKeys(); break;
        }
    }
    if (!open && !s.show_fps) { g_last_ns = 0; return nullptr; }
    uint64_t now = plat_ticks_ns();
    io.DeltaTime = g_last_ns && now > g_last_ns ? (float)((now - g_last_ns) / 1e9) : 1.0f / 60;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    g_last_ns = now;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.MouseDrawCursor = false;
    ImGui::NewFrame();
    if (s.show_fps) {
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        if (gpu_fg_active()) ImGui::Text("%.0f fps (%.0f shown)", fps, fps * 2);
        else ImGui::Text("%.0f fps", fps);
        ImGui::End();
    }
    if (open) settings_window(src_w, src_h);
    ImGui::Render();
    return ImGui::GetDrawData();
}
