// Player settings (milestone M5): xc3.ini next to where the game is started (the game folder), editable in game
// with the TAB overlay (ui.cpp) or by hand. XRT_* environment variables override the file (test runs).
#pragma once
#include <atomic>
#include <map>
#include <string>

enum { WIN_WINDOWED = 0, WIN_BORDERLESS = 1 };
enum { PM_FIFO = 0, PM_MAILBOX = 1, PM_IMMEDIATE = 2 };     // vsync / fast vsync (no tearing) / off (tearing)
enum { UP_BILINEAR = 0, UP_FSR = 1 };

struct XrtSettings {
    int window_mode = WIN_WINDOWED;
    int window_w = 1600, window_h = 900;     // windowed size (client area)
    float res_scale = 2.0f;                  // internal 3D resolution: x 960x544 (2 = 1080p-class, 2.66 = 1440p, 4 = 4K)
    int fps_cap = 120;                       // 0 = uncapped
    int menu_fps = 60;                       // menus / screens without the 3D scene (they step per frame)
    int present_mode = PM_MAILBOX;
    int upscaler = UP_FSR;                   // game image -> window
    float fsr_sharpness = 0.25f;             // RCAS, stops: 0 = sharpest, 2 = softest
    int keep_aspect = 1;                     // letterbox (1) or stretch (0)
    int volume = 100;                        // 0..100
    int show_fps = 0;                        // small frame-rate counter in the corner
    int frame_gen = 0;
    std::map<std::string, int> romfs_mods;   // RomFS mods (game/mods) turned off / on by name (absent = on) - xc3.ini
    // Profiles (the launcher: "Normal", "Randomized", ...): xc3.exe --profile <name> reads profiles\<name>.ini after
    // xc3.ini (any key; save_dir = the profile's own save folder; romfs_mod.<name> = 1 for the mods only it uses).
    std::string profile, save_dir = "save";
    std::map<std::string, int> profile_mods;  // romfs_mod.* of the active profile's file
};

XrtSettings &settings();                     // loaded on first use
void settings_set_profile(const char *name);  // (main.cpp, before the first settings() call)
// RomFS mods (hle_nn.cpp): folders in the game folder's mods\ that replace / add game files
int romfs_mod_count();
std::string romfs_mod_name(int i);
size_t romfs_mod_files(int i);
bool romfs_mod_on(int i);
void romfs_mod_set(int i, bool on);
bool settings_save();                        // writes xc3.ini; false if it couldn't
const char *settings_path();
// bumped when a change needs the presenter / window to act (swapchain mode, window mode or size)
extern std::atomic<int> g_settings_gen;
