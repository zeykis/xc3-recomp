// xc3.ini: "key = value" lines, '#' or ';' comments. Unknown keys are ignored; missing keys keep their defaults.
#include "settings.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <filesystem>
extern "C" {
#include "xrt_internal.h"
}

std::atomic<int> g_settings_gen{0};
uint8_t xrt_mod_flags[64];   // (switchable mods: see xrt.h; written here and by the TAB menu)

static int mod_index(const char *name) {
    for (unsigned i = 0; i < xrt_mod_count && i < 64; i++) if (!strcmp(xrt_mod_names[i], name)) return (int)i;
    return -1;
}
static XrtSettings g_s;
static std::once_flag g_once;
static std::string g_profile_arg;
static std::map<std::string, int> g_profile_only;   // mods some profile turns on (off everywhere else unless set)
void settings_set_profile(const char *name) { g_profile_arg = name ? name : ""; }
static std::string profile_path(const std::string &p) { return "profiles/" + p + ".ini"; }

const char *settings_path() { return "xc3.ini"; }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_kv(XrtSettings &s, const char *k, const char *v) {
    if (!strcmp(k, "window_mode")) s.window_mode = !strcmp(v, "borderless") ? WIN_BORDERLESS : !strcmp(v, "windowed") ? WIN_WINDOWED : clampi(atoi(v), 0, 1);
    else if (!strcmp(k, "window_width")) s.window_w = clampi(atoi(v), 640, 16384);
    else if (!strcmp(k, "window_height")) s.window_h = clampi(atoi(v), 360, 16384);
    else if (!strcmp(k, "resolution_scale")) s.res_scale = clampf((float)atof(v), 0.5f, 4.0f);
    else if (!strcmp(k, "fps_cap")) s.fps_cap = clampi(atoi(v), 0, 1000);
    else if (!strcmp(k, "menu_fps")) s.menu_fps = clampi(atoi(v), 0, 1000);
    else if (!strcmp(k, "present_mode")) s.present_mode = !strcmp(v, "fifo") || !strcmp(v, "vsync") ? PM_FIFO : !strcmp(v, "immediate") ? PM_IMMEDIATE : PM_MAILBOX;
    else if (!strcmp(k, "upscaler")) s.upscaler = !strcmp(v, "bilinear") ? UP_BILINEAR : UP_FSR;
    else if (!strcmp(k, "fsr_sharpness")) s.fsr_sharpness = clampf((float)atof(v), 0.0f, 2.0f);
    else if (!strcmp(k, "keep_aspect")) s.keep_aspect = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "volume")) s.volume = clampi(atoi(v), 0, 100);
    else if (!strcmp(k, "show_fps")) s.show_fps = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "frame_gen")) s.frame_gen = atoi(v) ? 1 : 0;
    else if (!strncmp(k, "romfs_mod.", 10)) s.romfs_mods[k + 10] = atoi(v) ? 1 : 0;
    else if (!strcmp(k, "save_dir")) { std::string d = v; if (!d.empty() && d.find("..") == std::string::npos) s.save_dir = d; }
    else if (!strncmp(k, "mod.", 4)) {
        int i = mod_index(k + 4);
        if (i >= 0) xrt_mod_flags[i] = atoi(v) ? 1 : 0;
        else LOG(1, "[settings] unknown mod '%s' (not in this build's game/patches/mods)", k + 4);
    }
}

static bool parse_file(const std::string &path, XrtSettings &s) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '[' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *v = eq + 1;
        for (char *e = eq - 1; e >= p && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t'); e--) *e = 0;
        set_kv(s, p, v);
    }
    fclose(f);
    return true;
}
static void load() {
    if (parse_file(settings_path(), g_s)) LOG(1, "[settings] loaded %s", settings_path());
    else LOG(1, "[settings] no %s: defaults (written on first save from the TAB menu)", settings_path());
    {   // mods only a profile uses (profiles\*.ini, romfs_mod.<name> = 1)
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator("profiles", ec)) {
            if (e.path().extension() != ".ini") continue;
            XrtSettings t; parse_file(e.path().string(), t);
            for (auto &kv : t.romfs_mods) if (kv.second) g_profile_only[kv.first] = 1;
        }
    }
    if (!g_profile_arg.empty() && g_profile_arg != "normal") {
        g_s.profile = g_profile_arg;
        g_s.save_dir = "save_" + g_profile_arg;
        XrtSettings t = g_s; t.romfs_mods.clear();
        bool ok = parse_file(profile_path(g_profile_arg), t);
        std::map<std::string, int> base = g_s.romfs_mods;
        g_s = t; g_s.romfs_mods = base; g_s.profile_mods = t.romfs_mods;
        LOG(1, "[settings] profile %s (%s): saves in %s", g_profile_arg.c_str(), ok ? profile_path(g_profile_arg).c_str() : "no file: defaults", g_s.save_dir.c_str());
    }
    // test runs: XRT_* variables win over the file
    if (const char *e = getenv("XRT_FPS_CAP")) set_kv(g_s, "fps_cap", e);
    if (const char *e = getenv("XRT_MENU_FPS")) set_kv(g_s, "menu_fps", e);
    if (const char *e = getenv("XRT_RES_SCALE")) set_kv(g_s, "resolution_scale", e);
    if (const char *e = getenv("XRT_PRESENT_MODE")) set_kv(g_s, "present_mode", e);
    if (const char *e = getenv("XRT_UPSCALER")) set_kv(g_s, "upscaler", e);
    if (const char *e = getenv("XRT_FSR_SHARPNESS")) set_kv(g_s, "fsr_sharpness", e);
    if (const char *e = getenv("XRT_VOLUME")) set_kv(g_s, "volume", e);
    if (const char *e = getenv("XRT_FRAME_GEN")) set_kv(g_s, "frame_gen", e);
    if (const char *e = getenv("XRT_MODS")) {   // "all", "none", or names separated by commas
        bool all = !strcmp(e, "all");
        for (unsigned i = 0; i < xrt_mod_count && i < 64; i++) xrt_mod_flags[i] = all;
        if (!all && strcmp(e, "none")) {
            std::string list = e; size_t p = 0;
            while (p <= list.size()) {
                size_t q = list.find(',', p); if (q == std::string::npos) q = list.size();
                std::string n = list.substr(p, q - p); int i = mod_index(n.c_str());
                if (i >= 0) xrt_mod_flags[i] = 1; else if (!n.empty()) LOG(1, "[settings] XRT_MODS: unknown mod '%s'", n.c_str());
                p = q + 1;
            }
        }
    }
    if (const char *e = getenv("XRT_WINDOW")) {   // WxH, or "borderless"
        int w, h;
        if (sscanf(e, "%dx%d", &w, &h) == 2) { g_s.window_w = clampi(w, 640, 16384); g_s.window_h = clampi(h, 360, 16384); g_s.window_mode = WIN_WINDOWED; }
        else set_kv(g_s, "window_mode", e);
    }
    LOG(1, "[settings] window %s %dx%d, scale %.2f, fps cap %d (menus %d), present %d, upscaler %s (sharpness %.2f), volume %d",
        g_s.window_mode == WIN_BORDERLESS ? "borderless" : "windowed", g_s.window_w, g_s.window_h, g_s.res_scale, g_s.fps_cap, g_s.menu_fps,
        g_s.present_mode, g_s.upscaler == UP_FSR ? "FSR 1" : "bilinear", g_s.fsr_sharpness, g_s.volume);
    for (unsigned i = 0; i < xrt_mod_count && i < 64; i++) LOG(1, "[settings] mod %u %-28s %s", i, xrt_mod_names[i], xrt_mod_flags[i] ? "on" : "off");
}

bool settings_romfs_mod_on(const char *name) {
    const XrtSettings &s = settings();
    auto p = s.profile_mods.find(name);
    if (p != s.profile_mods.end()) return p->second != 0;          // the active profile says
    if (g_profile_only.count(name)) return false;                  // another profile's mod (e.g. the randomizer)
    auto it = s.romfs_mods.find(name);
    return it == s.romfs_mods.end() || it->second != 0;
}
XrtSettings &settings() { std::call_once(g_once, load); return g_s; }

bool settings_save() {
    XrtSettings &s = settings();
    FILE *f = fopen(settings_path(), "w");
    if (!f) { LOG(0, "[settings] can't write %s", settings_path()); return false; }
    static const char *pm[] = {"fifo", "mailbox", "immediate"};
    fprintf(f,
            "# Xenoblade Chronicles 3 recomp settings (TAB in game to change them; this file is rewritten on Save)\n"
            "# window_mode: windowed | borderless (borderless = fullscreen on the window's monitor)\n"
            "window_mode = %s\nwindow_width = %d\nwindow_height = %d\n"
            "# internal 3D resolution, x 960x544: 1 = Switch, 2 = 1080p-class, 2.66 = 1440p, 4 = 4K-class (applies on restart)\n"
            "resolution_scale = %.2f\n"
            "# frame cap (0 = none); menus keep their own cap because their animations run per frame\n"
            "fps_cap = %d\nmenu_fps = %d\n"
            "# present_mode: fifo (vsync) | mailbox (vsync, no wait) | immediate (no vsync, may tear)\n"
            "present_mode = %s\n"
            "# upscaler: fsr | bilinear;  fsr_sharpness: 0 = sharpest .. 2 = softest\n"
            "upscaler = %s\nfsr_sharpness = %.2f\nkeep_aspect = %d\n"
            "volume = %d\nshow_fps = %d\n"
            "# frame generation: a generated image between game frames (x2 shown; the game runs at half the refresh rate)\n"
            "frame_gen = %d\n",
            s.window_mode == WIN_BORDERLESS ? "borderless" : "windowed", s.window_w, s.window_h, s.res_scale, s.fps_cap, s.menu_fps,
            pm[clampi(s.present_mode, 0, 2)], s.upscaler == UP_FSR ? "fsr" : "bilinear", s.fsr_sharpness, s.keep_aspect, s.volume, s.show_fps, s.frame_gen);
    {   // RomFS mods (game/mods): a profile's own mods go to its file (below), the rest here
        int n = romfs_mod_count();
        if (n) fprintf(f, "# RomFS mods (folders in the game folder's mods\\): 1 = on\n");
        for (int i = 0; i < n; i++) {
            std::string nm = romfs_mod_name(i);
            bool prof = g_profile_only.count(nm) || s.profile_mods.count(nm);
            if (prof && !s.profile.empty()) { if (s.romfs_mods.count(nm)) fprintf(f, "romfs_mod.%s = %d\n", nm.c_str(), s.romfs_mods[nm]); continue; }
            if (prof && !romfs_mod_on(i) && !s.romfs_mods.count(nm)) continue;   // (another profile's mod: stays off by default)
            fprintf(f, "romfs_mod.%s = %d\n", nm.c_str(), romfs_mod_on(i) ? 1 : 0);
        }
    }
    if (xrt_mod_count) fprintf(f, "# graphics mods (game/patches/mods, built into the exe): 1 = on\n");
    for (unsigned i = 0; i < xrt_mod_count && i < 64; i++) fprintf(f, "mod.%s = %d\n", xrt_mod_names[i], xrt_mod_flags[i]);
    fclose(f);
    LOG(1, "[settings] saved %s", settings_path());
    if (!s.profile.empty()) {   // the profile's file: its save folder and its mods
        std::error_code ec; std::filesystem::create_directories("profiles", ec);
        if (FILE *pf = fopen(profile_path(s.profile).c_str(), "w")) {
            fprintf(pf, "# profile %s (xc3.exe --profile %s, the launcher): read after xc3.ini\nsave_dir = %s\n", s.profile.c_str(), s.profile.c_str(), s.save_dir.c_str());
            int n = romfs_mod_count();
            for (int i = 0; i < n; i++) {
                std::string nm = romfs_mod_name(i);
                if (g_profile_only.count(nm) || s.profile_mods.count(nm)) fprintf(pf, "romfs_mod.%s = %d\n", nm.c_str(), romfs_mod_on(i) ? 1 : 0);
            }
            fclose(pf);
        }
    }
    return true;
}
