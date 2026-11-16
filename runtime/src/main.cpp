// xc3 - host executable: loads the game image, runs the Switch app boot sequence on recompiled code.
//   xc3 [--game DIR] [--log N]
// DIR must contain main.elf (from triage.py), optionally sdk.elf (data donor) and romfs/ (extracted RomFS).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#endif
#include "settings.h"
extern "C" {
#include "xrt_internal.h"
void hle_set_paths(const char *romfs, const char *save);
}

static std::string g_game = "game";
extern "C" void xrt_watchdog_dump(void);
static int g_watchdog;
static void watchdog(void *) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(g_watchdog));
        xrt_watchdog_dump();
    }
}

static void guest_main(void *) {
    Ctx *c = xrt_new_ctx(8ULL << 20);
    xrt_sampler_register_thread("main");
    xrt_apply_render_scale();
    xrt_mods_boot();   // (RomFS mods: hle_nn.cpp)
    if (g_img.sym_nninitStartup) {
        LOG(1, "[boot] nninitStartup (main+0x%llx)", (unsigned long long)(g_img.sym_nninitStartup - G_BASE));
        xrt_call(c, g_img.sym_nninitStartup, 0);
    }
    uint64_t *ia = (uint64_t *)(g_img.base + g_img.init_array);
    LOG(1, "[boot] running %llu static initializers", (unsigned long long)g_img.init_array_count);
    for (uint64_t i = 0; i < g_img.init_array_count; i++) {
        LOG(2, "[boot]   init[%llu] main+0x%llx", (unsigned long long)i, (unsigned long long)(ia[i] - G_BASE));
        xrt_call(c, ia[i], 0);
    }
    LOG(1, "[boot] nnMain (main+0x%llx)", (unsigned long long)(g_img.sym_nnMain - G_BASE));
    xrt_call(c, g_img.sym_nnMain, 0);
    LOG(1, "[boot] nnMain returned");
}


// ---------------------------------------------------------------- self tests (run recompiled game code in isolation)
// --zstd-test FILE OFFSET : decompress one xbc1/zstd archive entry with the game's own zstd code
//   (ZSTD_createDStream = main+0xf799e0, ZSTD_decompressStream = main+0xf79b60) and write it to zstd_out.bin
static std::string g_zstd_file; static uint64_t g_zstd_off;
static void zstd_test(void *) {
    Ctx *c = xrt_new_ctx(8ULL << 20);
    if (g_img.sym_nninitStartup) xrt_call(c, g_img.sym_nninitStartup, 0);
    FILE *f = fopen(g_zstd_file.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", g_zstd_file.c_str()); return; }
#ifdef _WIN32
    _fseeki64(f, (long long)g_zstd_off, SEEK_SET);
#else
    fseeko(f, (off_t)g_zstd_off, SEEK_SET);
#endif
    uint8_t hdr[0x30]; fread(hdr, 1, 0x30, f);
    uint32_t usize, csize; memcpy(&usize, hdr + 8, 4); memcpy(&csize, hdr + 12, 4);
    uint8_t *src = (uint8_t *)plat_alloc(csize), *dst = (uint8_t *)plat_alloc(usize + 64);
    fread(src, 1, csize, f); fclose(f);
    fprintf(stderr, "[zstd-test] '%s' type %d: %u -> %u bytes\n", (char *)hdr + 0x14, hdr[4], csize, usize);
    uint64_t custom = (uint64_t)(uintptr_t)plat_alloc(64);   /* ZSTD_customMem {0,0,0} = default malloc/free */
    uint64_t zds = xrt_call(c, G_BASE + 0xf799e0, 1, custom);
    uint64_t *out = (uint64_t *)plat_alloc(64), *in = (uint64_t *)plat_alloc(64);
    out[0] = (uint64_t)(uintptr_t)dst; out[1] = usize; out[2] = 0;
    in[0] = (uint64_t)(uintptr_t)src; in[1] = csize; in[2] = 0;
    for (int it = 0; it < 100000 && (in[2] < in[1] || out[2] < out[1]); it++) {
        int64_t r = (int64_t)xrt_call(c, G_BASE + 0xf79b60, 3, zds, (uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)in);
        if (r < 0 && r > -200) { fprintf(stderr, "[zstd-test] error %lld at in %llu/%llu out %llu\n", (long long)r, (unsigned long long)in[2], (unsigned long long)in[1], (unsigned long long)out[2]); break; }
        if (r == 0) break;
    }
    fprintf(stderr, "[zstd-test] produced %llu bytes\n", (unsigned long long)out[2]);
    FILE *o = fopen("zstd_out.bin", "wb"); fwrite(dst, 1, out[2], o); fclose(o);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) g_game = argv[++i];
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) settings_set_profile(argv[++i]);   // (the launcher: own save folder + mods)
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) g_log_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zstd-test") && i + 2 < argc) { g_zstd_file = argv[++i]; g_zstd_off = strtoull(argv[++i], nullptr, 0); }
        else { std::fprintf(stderr, "usage: %s [--game DIR] [--log N]\n", argv[0]); return 1; }
    }
#ifdef _WIN32
    {   // installed copy (tools/setup): xc3.exe next to game\ - started without --game, run from there
        bool has_game = false;
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--game")) has_game = true;
        wchar_t exe[MAX_PATH];
        if (!has_game && GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            std::wstring dir(exe); dir = dir.substr(0, dir.find_last_of(L"\\/"));
            if (GetFileAttributesW((dir + L"\\game\\main.elf").c_str()) != INVALID_FILE_ATTRIBUTES) { SetCurrentDirectoryW((dir + L"\\game").c_str()); g_game = "."; }
        }
        // started from a shortcut / Explorer (a console of its own): the log goes to play-log.txt, no console window
        DWORD pl[2];
        if (GetConsoleProcessList(pl, 2) == 1 && _isatty(_fileno(stderr))) {
            freopen("play-log.txt", "w", stderr); freopen("play-log.txt", "a", stdout);
            FreeConsole();
        }
    }
#endif
    // the log is written as it goes (unbuffered): a crash that skips every handler still leaves it complete
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (const char *p = getenv("XRT_PROFILE")) settings_set_profile(p);   // (test runs: like --profile)
    plat_install_crash_handler();
    if (const char *w = getenv("XRT_WATCHDOG")) { g_watchdog = atoi(w); if (g_watchdog > 0) plat_thread_create(watchdog, nullptr, 1 << 20); }
    std::string romfs = g_game + "/romfs", save = g_game + "/" + settings().save_dir;   // (a profile has its own save folder)
    hle_set_paths(romfs.c_str(), save.c_str());
    if (!xrt_load_donor((g_game + "/sdk.elf").c_str())) LOG(1, "[load] no sdk.elf - SDK data imports will be missing");
    if (!xrt_load_image((g_game + "/main.elf").c_str())) return 1;
    xrt_build_fn_map();
    void *t = plat_thread_create(g_zstd_file.empty() ? guest_main : zstd_test, nullptr, HOST_STACK_MAIN);
    plat_thread_join(t);
    hle_print_stats();
    return 0;
}
