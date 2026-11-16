// XC3 Launcher: starts the recompiled Xenoblade Chronicles 3 normally or randomized. Each has its own save folder
// (save\ and save_randomized\) and the randomized one turns on the randomizer's files (mods\Randomizer), through
// the profile mechanism of xc3.exe (--profile <name>, profiles\<name>.ini; runtime/src/settings.cpp).
// The randomizer itself (SonahSofern's Xenoblade-Series-Randomizer, github.com/SonahSofern/Xenoblade-Series-Randomizer)
// writes its files into mods\Randomizer when its output folder is set to that folder. The launcher downloads its
// release the first time (pinned version + SHA-256, into Randomizer\ next to the launcher), points its output folder
// there and starts it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>
#include <filesystem>
#include <memory>
#include <string>
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "bcrypt.lib")
namespace fs = std::filesystem;
// the randomizer release the launcher downloads (108 MB zip: XCRandomizer_<ver>.exe + its XC2 extras)
static const wchar_t *k_rz_url = L"https://github.com/SonahSofern/Xenoblade-Series-Randomizer/releases/download/1.4.2/XCRandomizer_1.4.2.zip";
static const wchar_t *k_rz_exe = L"XCRandomizer_1.4.2.exe";
static const char *k_rz_sha = "41c703caf989068c8d435fdb7a10a309027a0e4ecaa270eb54b5565f4979c8d3";
enum { WM_RZ_DONE = WM_APP + 1 };
enum { ID_PLAY = 100, ID_PLAYR, ID_RANDO, ID_COPY, ID_OPENMODS, ID_CLOSE, ID_STATUS_R, ID_STATUS_S };
static HWND g_wnd, g_rstat, g_sstat, g_close;
static HFONT g_font, g_big;
static fs::path g_dir, g_game, g_exe, g_ini;
static std::wstring g_rando_exe;
static const wchar_t *k_rmod = L"Randomizer";

static int S(int v) { static UINT dpi = 0; if (!dpi) { HDC dc = GetDC(nullptr); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); } return MulDiv(v, dpi, 96); }
static std::wstring ini_get(const wchar_t *key) { wchar_t b[2048] = L""; GetPrivateProfileStringW(L"launcher", key, L"", b, 2048, g_ini.c_str()); return b; }
static void ini_set(const wchar_t *key, const std::wstring &v) { WritePrivateProfileStringW(L"launcher", key, v.c_str(), g_ini.c_str()); }

static std::wstring when(const fs::path &p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return L"";
    fs::file_time_type newest{};
    bool any = false;
    if (fs::is_directory(p, ec)) {
        for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) { auto t = it->last_write_time(ec); if (!any || t > newest) { newest = t; any = true; } }
        }
    } else { newest = fs::last_write_time(p, ec); any = true; }
    if (!any) return L"";
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(newest - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    time_t tt = std::chrono::system_clock::to_time_t(sys);
    tm lt; localtime_s(&lt, &tt);
    wchar_t b[64]; wcsftime(b, 64, L"%Y-%m-%d %H:%M", &lt);
    return b;
}
static size_t count_files(const fs::path &p) {
    size_t n = 0; std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) { if (ec) break; if (it->is_regular_file(ec)) n++; }
    return n;
}
static fs::path rando_dir() { return g_game / L"mods" / k_rmod; }
static void refresh() {
    std::error_code ec;
    size_t n = fs::is_directory(rando_dir(), ec) ? count_files(rando_dir()) : 0;
    std::wstring r = n ? L"Randomized files: " + std::to_wstring(n) + L", made " + when(rando_dir())
                       : L"No randomized files yet: Open Randomizer (downloaded the first time), make a seed, then Play Randomized.";
    SetWindowTextW(g_rstat, r.c_str());
    std::wstring a = when(g_game / L"save"), b = when(g_game / L"save_randomized");
    std::wstring s = L"Saves - normal: " + (a.empty() ? std::wstring(L"none yet") : L"last played " + a) +
                     L"     randomized: " + (b.empty() ? std::wstring(L"none yet (a new game)") : L"last played " + b);
    SetWindowTextW(g_sstat, s.c_str());
    EnableWindow(GetDlgItem(g_wnd, ID_PLAYR), n > 0);
}
static void ensure_profile() {   // profiles\randomized.ini: its save folder and its mod
    std::error_code ec;
    fs::create_directories(g_game / L"profiles", ec);
    fs::create_directories(rando_dir(), ec);
    fs::path pf = g_game / L"profiles" / L"randomized.ini";
    if (!fs::exists(pf, ec))
        if (FILE *f = _wfopen(pf.c_str(), L"w")) {
            fprintf(f, "# profile randomized (XC3 Launcher, xc3.exe --profile randomized): read after xc3.ini\n"
                       "save_dir = save_randomized\nromfs_mod.Randomizer = 1\n");
            fclose(f);
        }
}
static void launch(const wchar_t *profile) {
    ensure_profile();
    std::wstring cmd = L"\"" + g_exe.wstring() + L"\" --game . --profile " + profile;
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    std::wstring cl = cmd;
    if (!CreateProcessW(g_exe.c_str(), &cl[0], nullptr, nullptr, FALSE, 0, nullptr, g_game.c_str(), &si, &pi)) {
        MessageBoxW(g_wnd, (L"Could not start " + g_exe.wstring()).c_str(), L"XC3 Launcher", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ini_set(L"close_on_start", SendMessageW(g_close, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    if (SendMessageW(g_close, BM_GETCHECK, 0, 0) == BST_CHECKED) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}
// ---------------------------------------------------------------- randomizer download
static bool g_rz_busy;
static std::string sha256_file(const fs::path &p) {
    std::string out; BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (FILE *f = _wfopen(p.c_str(), L"rb")) {
            std::vector<unsigned char> b(1 << 20); size_t n;
            while ((n = fread(b.data(), 1, b.size(), f)) > 0) BCryptHashData(h, b.data(), (ULONG)n, 0);
            fclose(f);
            unsigned char d[32];
            if (BCryptFinishHash(h, d, 32, 0) == 0) { char x[65]; for (int i = 0; i < 32; i++) sprintf_s(x + 2 * i, 3, "%02x", d[i]); out = x; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
static bool run_wait(std::wstring cmd, const fs::path &cwd) {
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}
static fs::path rz_home() { return g_dir / L"Randomizer"; }
// worker thread: download, check, unpack; posts WM_RZ_DONE (wparam 1 = ok) with an error text in lparam
static void rz_download() {
    std::error_code ec;
    fs::path dir = rz_home(), zip = dir / L"randomizer.zip";
    fs::create_directories(dir, ec);
    std::wstring *err = nullptr;
    if (URLDownloadToFileW(nullptr, k_rz_url, zip.c_str(), 0, nullptr) != S_OK) err = new std::wstring(L"The download failed (no internet, or GitHub unreachable).");
    else if (sha256_file(zip) != k_rz_sha) err = new std::wstring(L"The downloaded file is not the expected one (checksum mismatch).");
    else if (!run_wait(L"tar.exe -xf \"" + zip.wstring() + L"\"", dir) || !fs::exists(dir / k_rz_exe, ec)) err = new std::wstring(L"Unpacking failed (Windows 10 1803 or later has tar.exe).");
    fs::remove(zip, ec);
    PostMessageW(g_wnd, WM_RZ_DONE, err ? 0 : 1, (LPARAM)err);
}
// the randomizer remembers its settings in XC3\SaveData\Last Save.txt (JSON): its output folder -> ours
static bool rz_point_output(const fs::path &exe) {
    std::error_code ec;
    fs::path ls = exe.parent_path() / L"XC3" / L"SaveData" / L"Last Save.txt";
    if (!fs::exists(ls, ec)) return false;
    std::string t;
    if (FILE *f = _wfopen(ls.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
    size_t k = t.find("\"Output Bdats\"");
    if (k == std::string::npos) return false;
    size_t q1 = t.find('"', t.find(':', k) + 1), q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    std::wstring w = rando_dir().wstring(); for (auto &c : w) if (c == L'\\') c = L'/';
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n - 1 : 0, 0); if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u[0], n, nullptr, nullptr);
    if (t.compare(q1 + 1, q2 - q1 - 1, u) == 0) return true;
    t.replace(q1 + 1, q2 - q1 - 1, u);
    if (FILE *f = _wfopen(ls.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); return true; }
    return false;
}
static void copy_path();
static void start_randomizer(const fs::path &rx) {
    bool pointed = rz_point_output(rx);
    copy_path();
    if (!pointed)
        MessageBoxW(g_wnd, (L"The randomizer opens next. Choose Xenoblade Chronicles 3 and set its output folder to:\n\n" + rando_dir().wstring() +
                            L"\n\n(already copied: paste it with Ctrl+V). It remembers it: next time the launcher keeps it set.").c_str(), L"XC3 Launcher", MB_ICONINFORMATION);
    ShellExecuteW(g_wnd, L"open", rx.c_str(), nullptr, rx.parent_path().c_str(), SW_SHOWNORMAL);
}
static std::wstring pick_file(const wchar_t *title) {
    std::wstring out; IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    COMDLG_FILTERSPEC f[] = {{L"Programs", L"*.exe;*.py"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, f); d->SetTitle(title);
    if (SUCCEEDED(d->Show(g_wnd))) { IShellItem *it = nullptr; PWSTR p = nullptr; if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); } if (it) it->Release(); }
    d->Release();
    return out;
}
static void run_randomizer() {
    ensure_profile();
    std::error_code ec;
    if (g_rz_busy) return;
    if (g_rando_exe.empty() || !fs::exists(g_rando_exe, ec)) {
        fs::path own = rz_home() / k_rz_exe;
        if (fs::exists(own, ec)) { g_rando_exe = own.wstring(); ini_set(L"randomizer", g_rando_exe); }
    }
    if (g_rando_exe.empty() || !fs::exists(g_rando_exe, ec)) {
        int a = MessageBoxW(g_wnd, L"Download SonahSofern's Xenoblade-Series-Randomizer 1.4.2 from GitHub (108 MB, once)?\n\n"
                                   L"Yes: download it next to the launcher (Randomizer folder).\nNo: locate a copy you already have.",
                            L"XC3 Launcher", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDNO) {
            g_rando_exe = pick_file(L"Xenoblade Randomizer");
            if (g_rando_exe.empty()) return;
            ini_set(L"randomizer", g_rando_exe);
        } else {
            g_rz_busy = true;
            EnableWindow(GetDlgItem(g_wnd, ID_RANDO), FALSE);
            SetWindowTextW(g_rstat, L"Downloading the randomizer (108 MB)...");
            std::thread(rz_download).detach();
            return;   // (WM_RZ_DONE starts it)
        }
    }
    start_randomizer(fs::path(g_rando_exe));
}
static void copy_path() {
    ensure_profile();
    std::wstring p = rando_dir().wstring();
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (p.size() + 1) * sizeof(wchar_t));
    if (h) { memcpy(GlobalLock(h), p.c_str(), (p.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_PLAY: launch(L"normal"); return 0;
        case ID_PLAYR: launch(L"randomized"); return 0;
        case ID_RANDO: run_randomizer(); return 0;
        case ID_COPY: copy_path(); SetWindowTextW(g_rstat, (L"Copied: " + rando_dir().wstring()).c_str()); return 0;
        case ID_OPENMODS: ensure_profile(); ShellExecuteW(g_wnd, L"open", rando_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        }
        break;
    case WM_RZ_DONE: {
        g_rz_busy = false;
        EnableWindow(GetDlgItem(g_wnd, ID_RANDO), TRUE);
        std::unique_ptr<std::wstring> err((std::wstring *)l);
        refresh();
        if (!w) { MessageBoxW(g_wnd, (L"Could not get the randomizer: " + *err + L"\n\nYou can download it yourself from\n" + k_rz_url + L"\nand locate it with Open Randomizer > No.").c_str(), L"XC3 Launcher", MB_ICONERROR); return 0; }
        g_rando_exe = (rz_home() / k_rz_exe).wstring(); ini_set(L"randomizer", g_rando_exe);
        start_randomizer(fs::path(g_rando_exe));
        return 0;
    }
    case WM_ACTIVATE: if (LOWORD(w) != WA_INACTIVE && g_rstat && !g_rz_busy) refresh(); break;   // (back from the randomizer: new files)
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)w, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    { wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH); g_dir = fs::path(b).parent_path(); }
    g_ini = g_dir / L"launcher.ini";
    g_exe = g_dir / L"xc3.exe";
    std::error_code ec;
    std::wstring gd = ini_get(L"game_dir");   // (a playable copy that uses the repo's game folder says where it is)
    for (fs::path c : {fs::path(gd), g_dir / L"game", g_dir.parent_path() / L"game"})
        if (!c.empty() && fs::exists(c / L"main.elf", ec)) { g_game = c; break; }
    if (g_game.empty() || !fs::exists(g_exe, ec)) {
        MessageBoxW(nullptr, L"xc3.exe or the game folder (game\\main.elf) was not found next to the launcher.\n"
                             L"Install the game with XC3 Setup, or set game_dir in launcher.ini.", L"XC3 Launcher", MB_ICONERROR);
        return 1;
    }
    g_rando_exe = ini_get(L"randomizer");
    NONCLIENTMETRICSW ncm{sizeof ncm}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(14); g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(20); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; g_big = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSW wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"xc3launcher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    const int W = S(560), x = S(16);
    RECT r{0, 0, W + 2 * x, S(392)}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Xenoblade Chronicles 3 (recomp)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    int y = S(14);
    mk(L"STATIC", L"Xenoblade Chronicles 3", SS_LEFT, x, y, W, S(30), -1, g_big); y += S(38);
    int bw = (W - S(12)) / 2;
    mk(L"BUTTON", L"Play", BS_DEFPUSHBUTTON, x, y, bw, S(56), ID_PLAY, g_big);
    mk(L"BUTTON", L"Play Randomized", BS_PUSHBUTTON, x + bw + S(12), y, bw, S(56), ID_PLAYR, g_big); y += S(64);
    g_sstat = mk(L"STATIC", L"", SS_LEFT, x, y, W, S(20), ID_STATUS_S); y += S(22);
    mk(L"STATIC", L"Each has its own saves: normal in game\\save, randomized in game\\save_randomized.", SS_LEFT, x, y, W, S(20), -1); y += S(34);
    mk(L"STATIC", L"Randomizer", SS_LEFT, x, y, W, S(22), -1, g_big); y += S(28);
    g_rstat = mk(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, x, y, W, S(20), ID_STATUS_R); y += S(24);
    mk(L"STATIC", (L"Its output folder: " + rando_dir().wstring()).c_str(), SS_LEFT | SS_PATHELLIPSIS, x, y, W, S(20), -1); y += S(28);
    int b3 = (W - S(24)) / 3;
    mk(L"BUTTON", L"Open Randomizer...", BS_PUSHBUTTON, x, y, b3, S(30), ID_RANDO);
    mk(L"BUTTON", L"Copy output folder", BS_PUSHBUTTON, x + b3 + S(12), y, b3, S(30), ID_COPY);
    mk(L"BUTTON", L"Show the files", BS_PUSHBUTTON, x + 2 * (b3 + S(12)), y, b3, S(30), ID_OPENMODS); y += S(40);
    mk(L"STATIC", L"New seed: run the randomizer again (same output folder), then Play Randomized.", SS_LEFT, x, y, W, S(20), -1); y += S(30);
    g_close = mk(L"BUTTON", L"Close this window when the game starts", BS_AUTOCHECKBOX, x, y, W, S(22), ID_CLOSE); y += S(26);
    mk(L"STATIC", L"In game: TAB = settings, Alt+Enter = borderless fullscreen.", SS_LEFT, x, y, W, S(20), -1);
    std::wstring cs = ini_get(L"close_on_start");
    SendMessageW(g_close, BM_SETCHECK, cs == L"0" ? BST_UNCHECKED : BST_CHECKED, 0);
    refresh();
    ShowWindow(g_wnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    CoUninitialize();
    return 0;
}
