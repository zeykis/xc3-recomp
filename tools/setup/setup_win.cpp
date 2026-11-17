// xc3-setup: installs the recompiled Xenoblade Chronicles 3 from the player's own game files.
//
// Input: the base game (.xci / .nsp / .nca), the update and the DLC (.nsp / .nca, or an emulator's installed
// contents, found automatically), and the console keys (prod.keys; title keys from the .nsp tickets or title.keys).
// It identifies every file by its NCA header (nx.cpp), checks that the update's code is the version this build
// was recompiled for, extracts ExeFS / RomFS / DLC with hactool (shipped next to it), converts the executables
// to the ELF files the runtime loads, copies the program, the patches and (optionally) the emulator's saves, and
// makes shortcuts. Nothing is downloaded; the files never leave the PC.
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
#include <atomic>
#include <cstdarg>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "nx.h"
#if __has_include("build_info.h")
#include "build_info.h"
#endif
#ifndef XC3_MAIN_BUILD_ID   // (built before the game code was generated: the version the patches and hooks are for, 2.2.1)
#define XC3_MAIN_BUILD_ID "82D187FE9EF9BE921B8E1B1CDDF7BD287CDB22C1"
#endif
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
static const uint64_t TITLE_BASE = 0x010074F013262000ULL;   // Xenoblade Chronicles 3 (DLC: 0x...3001 - 0x...300F)

// ---------------------------------------------------------------- strings
static std::string u8(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string fmt(const char *f, ...) {
    char b[2048]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b;
}
static std::string gb(uint64_t n) { return fmt("%.1f GB", n / 1073741824.0); }
static std::string lower_ext(const fs::path &p) { std::string e = u8(p.extension().wstring()); for (char &c : e) c = (char)tolower(c); return e; }

// ---------------------------------------------------------------- UI state
enum {
    ID_LIST = 100, ID_ADDFILES, ID_ADDFOLDER, ID_REMOVE, ID_DETECT, ID_KEYS, ID_KEYSB, ID_DEST, ID_DESTB,
    ID_SAVES, ID_SHORTCUT, ID_SKIPDATA, ID_SCAN, ID_INSTALL, ID_PROGRESS, ID_STATUS, ID_LOG,
    ID_BASE, ID_BASEB, ID_UPD, ID_UPDB, ID_CLEARBASE, ID_CLEARUPD
};
static const UINT WM_LOGLINE = WM_APP + 1, WM_PROGRESS = WM_APP + 2, WM_DONE = WM_APP + 3;
static HWND g_base, g_upd, g_base_st, g_upd_st;   // base game / update slots (+ what was recognized in them)
static HWND g_wnd, g_list, g_keys, g_dest, g_saves, g_shortcut, g_skip, g_scan, g_install, g_prog, g_status, g_log;
static HFONT g_font;
static std::vector<std::wstring> g_sources;           // DLC slot: files (one or more packs each)
static std::vector<std::wstring> g_source_labels;     // what each DLC file was recognized as
static std::wstring g_save_src;                       // emulator save folder for XC3, if found
static std::mutex g_logm; static std::vector<std::string> g_pending;
static std::atomic<bool> g_busy{false};

static void log(const std::string &s) {
    { std::lock_guard<std::mutex> l(g_logm); g_pending.push_back(s); }
    PostMessageW(g_wnd, WM_LOGLINE, 0, 0);
}
static void status(const std::string &s, int permille = -1) {
    std::string *p = new std::string(s);
    PostMessageW(g_wnd, WM_PROGRESS, (WPARAM)permille, (LPARAM)p);
}
static std::wstring get_text(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n, 0); GetWindowTextW(h, &s[0], n + 1); return s; }
static void refresh_list() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_sources.size(); i++) {
        std::wstring t = (i < g_source_labels.size() && !g_source_labels[i].empty() ? g_source_labels[i] + L"   " : L"") + g_sources[i];
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
}
static void add_source(const std::wstring &s, const std::wstring &label = L"") {
    for (auto &x : g_sources) if (_wcsicmp(x.c_str(), s.c_str()) == 0) return;
    g_sources.push_back(s); g_source_labels.push_back(label);
}

namespace nx { struct Keys; }
static int place_folder(const nx::Keys &K, bool have_keys, const fs::path &dir, bool fill_only_empty);

// ---------------------------------------------------------------- emulator detection
static std::wstring known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static void detect_emulators(bool quiet) {
    fs::path appdata = known(FOLDERID_RoamingAppData);
    int found = 0;
    std::error_code ec;
    for (const wchar_t *emu : {L"eden", L"citron", L"sudachi", L"suyu", L"torzu", L"yuzu"}) {
        fs::path root = appdata / emu;
        if (!fs::exists(root / L"keys" / L"prod.keys", ec)) continue;
        if (get_text(g_keys).empty()) SetWindowTextW(g_keys, (root / L"keys" / L"prod.keys").c_str());
        fs::path reg = root / L"nand" / L"user" / L"Contents" / L"registered";
        if (fs::is_directory(reg, ec)) {
            nx::Keys K; K.load_prod(u8((root / L"keys" / L"prod.keys").wstring()));
            if (place_folder(K, true, reg, true)) found++;
        }
        fs::path saves = root / L"nand" / L"user" / L"save" / L"0000000000000000";
        if (g_save_src.empty() && fs::is_directory(saves, ec)) {
            fs::file_time_type best{};
            for (auto &u : fs::directory_iterator(saves, ec)) {
                fs::path d = u.path() / L"010074F013262000";
                if (fs::is_directory(d, ec)) { auto t = fs::last_write_time(d, ec); if (g_save_src.empty() || t > best) { best = t; g_save_src = d.wstring(); } }
            }
        }
        if (!quiet) log(fmt("Found %s (installed contents, keys%s)", u8(emu).c_str(), g_save_src.empty() ? "" : ", saves"));
    }
    if (get_text(g_keys).empty()) {
        fs::path ryu = appdata / L"Ryujinx" / L"system" / L"prod.keys";
        if (fs::exists(ryu, ec)) SetWindowTextW(g_keys, ryu.c_str());
    }
    if (!quiet && !found) log("No emulator with Xenoblade Chronicles 3 installed found: choose your files above.");
    refresh_list();
    EnableWindow(g_saves, !g_save_src.empty());
    if (!g_save_src.empty()) {
        SendMessageW(g_saves, BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextW(g_saves, (L"Copy my saves from " + g_save_src).c_str());
    }
}

// ---------------------------------------------------------------- scanning
struct Found {
    nx::NcaInfo base, update, control; bool has_base = false, has_update = false;
    std::map<int, nx::NcaInfo> dlc;
    std::vector<nx::NcaInfo> updates;   // every update program seen (the right version is picked by its code)
};
static bool better(const nx::NcaInfo &a, const nx::NcaInfo &b) {   // a loose .nca file beats one inside a container
    return (a.blob.off == 0) && (b.blob.off != 0);
}
static void consider(Found &F, const nx::NcaInfo &n) {
    if ((n.title & ~0xFFFULL) == TITLE_BASE && (n.title & 0xFFF) == 0) {
        if (n.type == nx::CT_PROGRAM && !n.patch) { if (!F.has_base || better(n, F.base)) F.base = n; F.has_base = true; }
        else if (n.type == nx::CT_PROGRAM && n.patch) F.updates.push_back(n);
    } else if ((n.title & ~0xFFFULL) == TITLE_BASE + 0x1000 && (n.title & 0xFFF) >= 1 && (n.type == nx::CT_PUBLIC_DATA || n.type == nx::CT_DATA)) {
        int i = (int)(n.title & 0xFFF);
        if (!F.dlc.count(i) || better(n, F.dlc[i])) F.dlc[i] = n;
    }
}
static void scan_file(nx::Keys &K, Found &F, const fs::path &p, int &n_files) {
    std::error_code ec;
    std::string ext = lower_ext(p), ps = u8(p.wstring());
    uint64_t sz = fs::file_size(p, ec);
    if (ec) return;
    nx::Blob b{ps, 0, sz};
    if (ext == ".nca") {
        nx::NcaInfo n;
        if (nx::nca_identify(K, b, n)) { n.label = ps; consider(F, n); n_files++; }
    } else if (ext == ".nsp" || ext == ".xci") {
        std::vector<nx::Entry> es;
        if (!(ext == ".xci" ? nx::parse_xci_secure(b, es) : nx::parse_pfs0(b, es))) { log("  can't read " + ps + " (not a valid " + ext + ")"); return; }
        for (auto &e : es)
            if (e.name.size() > 4 && e.name.substr(e.name.size() - 4) == ".tik" && e.size < 0x1000) {
                std::vector<uint8_t> t((size_t)e.size); nx::Blob tb{ps, e.off, e.size};
                std::string rid, key;
                bool pers = false;
                if (tb.read(0, t.data(), t.size())) {
                    if (nx::parse_ticket(t, rid, key, &pers)) K.titlekeys[rid] = key;
                    else if (pers) K.personalized.insert(rid);
                }
            }
        for (auto &e : es)
            if (e.name.size() > 4 && e.name.substr(e.name.size() - 4) == ".nca") {
                nx::NcaInfo n; nx::Blob eb{ps, e.off, e.size};
                if (nx::nca_identify(K, eb, n)) { n.label = ps + " > " + e.name; consider(F, n); }
            }
        n_files++;
    }
}
// what a single file holds (base / update / which DLC packs); needs prod.keys (header key) only
struct Kind { bool base = false, update = false; std::vector<int> dlc; bool ok = false; };
static bool load_keys_quiet(nx::Keys &K) {
    std::string keys = u8(get_text(g_keys));
    return !keys.empty() && K.load_prod(keys);
}
static Kind classify(const nx::Keys &K0, const fs::path &p) {
    nx::Keys K = K0; Found F; int n = 0; Kind k;
    scan_file(K, F, p, n);
    k.base = F.has_base; k.update = !F.updates.empty();
    for (auto &d : F.dlc) k.dlc.push_back(d.first);
    k.ok = k.base || k.update || !k.dlc.empty();
    return k;
}
static std::wstring dlc_label(const std::vector<int> &v) {
    std::wstring s = v.size() > 1 ? L"[DLC " : L"[DLC ";
    for (size_t i = 0; i < v.size(); i++) s += (i ? L"+" : L"") + std::to_wstring(v[i]);
    return s + L"]";
}
static void set_slot(HWND edit, HWND st, const std::wstring &path, const std::wstring &what) {
    SetWindowTextW(edit, path.c_str()); SetWindowTextW(st, what.c_str());
}
// sorts a file into the right slot by what it contains: base game, update, DLC (a file may hold several)
static bool place_file(const nx::Keys &K, bool have_keys, const fs::path &p, bool fill_only_empty) {
    std::string e = lower_ext(p);
    if (e != ".nca" && e != ".nsp" && e != ".xci") return false;
    if (!have_keys) {   // without keys: by extension (an .xci is the base game; the rest goes to DLC until checked)
        if (e == ".xci") { set_slot(g_base, g_base_st, p.wstring(), L"(not checked: select prod.keys)"); return true; }
        add_source(p.wstring(), L"[?]"); return true;
    }
    Kind k = classify(K, p);
    if (!k.ok) return false;
    if (k.base && (!fill_only_empty || get_text(g_base).empty())) set_slot(g_base, g_base_st, p.wstring(), k.update ? L"base game + update" : L"base game");
    if (k.update && !k.base && (!fill_only_empty || get_text(g_upd).empty())) set_slot(g_upd, g_upd_st, p.wstring(), L"update");
    if (!k.dlc.empty()) add_source(p.wstring(), dlc_label(k.dlc));
    return true;
}
static int place_folder(const nx::Keys &K, bool have_keys, const fs::path &dir, bool fill_only_empty) {
    int n = 0; std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && place_file(K, have_keys, it->path(), fill_only_empty)) n++;
    }
    return n;
}
static void add_paths(const std::vector<std::wstring> &paths, bool fill_only_empty = false) {
    nx::Keys K; bool hk = load_keys_quiet(K);
    if (hk) { fs::path kd = fs::path(get_text(g_keys)).parent_path(); for (const wchar_t *n : {L"title.keys", L"title.keys_autogenerated"}) K.load_titlekeys(u8((kd / n).wstring())); }
    int n = 0; std::error_code ec;
    for (auto &p : paths) {
        if (fs::is_directory(p, ec)) n += place_folder(K, hk, p, fill_only_empty);
        else if (place_file(K, hk, p, fill_only_empty)) n++;
        else log("Not a Xenoblade Chronicles 3 file (or unreadable): " + u8(p));
    }
    if (!hk && n) log("Select prod.keys first so the files can be recognized (base game / update / DLC).");
    refresh_list();
}

static bool scan(nx::Keys &K, Found &F, std::string &err) {
    K = nx::Keys(); F = Found();
    std::string keys = u8(get_text(g_keys));
    if (keys.empty() || !K.load_prod(keys)) { err = "prod.keys not found or without header_key: dump the keys of your own console (Lockpick_RCM) and select prod.keys."; return false; }
    fs::path kd = fs::path(wide(keys)).parent_path();
    int tk = 0;
    for (const wchar_t *n : {L"title.keys", L"title.keys_autogenerated"}) tk += K.load_titlekeys(u8((kd / n).wstring()));
    log(fmt("Keys: %s%s", keys.c_str(), tk ? fmt(" (+%d title keys)", tk).c_str() : ""));
    int n_files = 0;
    std::error_code ec;
    std::vector<std::wstring> srcs = g_sources;
    for (HWND h : {g_upd, g_base}) { std::wstring t = get_text(h); if (!t.empty()) srcs.insert(srcs.begin(), t); }
    for (auto &src : srcs) {
        fs::path p(src);
        if (fs::is_directory(p, ec)) {
            for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
                if (it->is_regular_file(ec)) { std::string e = lower_ext(it->path()); if (e == ".nca" || e == ".nsp" || e == ".xci") scan_file(K, F, it->path(), n_files); }
            }
        } else scan_file(K, F, p, n_files);
    }
    // the update: the newest one we can decrypt (the version check happens on its code, at install)
    for (auto &u : F.updates) if (!F.has_update || u.size > F.update.size) { F.update = u; F.has_update = true; }
    auto key_ok = [&](const nx::NcaInfo &n) { return n.rights_id.empty() || K.titlekeys.count(n.rights_id); };
    log(F.has_base ? "Base game: " + F.base.label + (key_ok(F.base) ? "" : "  [no title key!]") : "Base game: NOT FOUND");
    log(F.has_update ? "Update: " + F.update.label + (key_ok(F.update) ? "" : "  [no title key!]") : "Update: not found (the base game alone only works if this build was made for it)");
    for (auto &d : F.dlc) log(fmt("DLC %d: ", d.first) + d.second.label + (key_ok(d.second) ? "" : "  [no title key: skipped]"));
    if (F.dlc.empty()) log("DLC: none found (optional)");
    if (!F.has_base && !(SendMessageW(g_skip, BM_GETCHECK, 0, 0) == BST_CHECKED)) { err = "The base game was not found. Put your Xenoblade Chronicles 3 .xci / .nsp in \"Base game\" (or tick \"keep the installed game data\" to only add DLC to an existing install)."; return false; }
    if ((F.has_base && !key_ok(F.base)) || (F.has_update && !key_ok(F.update))) { err = "A title key is missing: add the game's .nsp (it carries its ticket) or put title.keys next to prod.keys."; return false; }
    return true;
}

// ---------------------------------------------------------------- install helpers
static std::wstring g_hactool, g_keys_path;
static std::string quote(const std::string &s) { return "\"" + s + "\""; }
static uint64_t free_space(const fs::path &p) {
    ULARGE_INTEGER fb{}; fs::path q = p; std::error_code ec;
    while (!q.empty() && !fs::exists(q, ec)) q = q.parent_path();
    GetDiskFreeSpaceExW(q.c_str(), &fb, nullptr, nullptr);
    return fb.QuadPart;
}
// runs hactool; while it works, progress = disk space used vs. the expected output size
static bool hactool(const std::string &args, const fs::path &logfile, const std::string &what, uint64_t expect, const fs::path &disk) {
    std::wstring cmd = L"\"" + g_hactool + L"\" -k \"" + g_keys_path + L"\" --disablekeywarns --suppresskeys " + wide(args);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lf = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = lf; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(lf); log("  can't start hactool.exe"); return false;
    }
    uint64_t f0 = free_space(disk), t0 = GetTickCount64();
    while (WaitForSingleObject(pi.hProcess, 1000) == WAIT_TIMEOUT) {
        uint64_t used = f0 > free_space(disk) ? f0 - free_space(disk) : 0;
        int pm = expect ? (int)std::min<uint64_t>(999, used * 1000 / expect) : -1;
        status(fmt("%s - %s written, %llu s", what.c_str(), gb(used).c_str(), (unsigned long long)((GetTickCount64() - t0) / 1000)), pm);
    }
    DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(lf);
    return true;   // (hactool's exit code is not meaningful: the caller checks the output)
}
// an NCA inside an .nsp / .xci is copied out first (hactool reads whole files)
static bool materialize(const nx::NcaInfo &n, const fs::path &tmp, const std::string &name, std::string &out) {
    std::error_code ec;
    if (n.blob.off == 0 && fs::file_size(wide(n.blob.path), ec) == n.blob.size) { out = n.blob.path; return true; }
    fs::path dst = tmp / wide(name);
    if (fs::exists(dst, ec) && fs::file_size(dst, ec) == n.blob.size) { out = u8(dst.wstring()); return true; }
    FILE *in = nx::xfopen(n.blob.path, "rb"), *o = nx::xfopen(u8(dst.wstring()), "wb");
    if (!in || !o) { if (in) fclose(in); if (o) fclose(o); return false; }
    _fseeki64(in, (int64_t)n.blob.off, SEEK_SET);
    std::vector<char> buf(8 << 20); uint64_t left = n.blob.size, done = 0;
    while (left) {
        size_t k = (size_t)std::min<uint64_t>(left, buf.size());
        if (fread(buf.data(), 1, k, in) != k || fwrite(buf.data(), 1, k, o) != k) { fclose(in); fclose(o); return false; }
        left -= k; done += k;
        status(fmt("Copying %s out of its package - %s / %s", name.c_str(), gb(done).c_str(), gb(n.blob.size).c_str()), (int)(done * 1000 / n.blob.size));
    }
    fclose(in); fclose(o);
    out = u8(dst.wstring());
    return true;
}
static std::string tk_arg(const nx::Keys &K, const nx::NcaInfo &n) {
    if (n.rights_id.empty()) return "";
    auto it = K.titlekeys.find(n.rights_id);
    return it == K.titlekeys.end() ? "" : " --titlekey=" + it->second;
}
// an NCA that needs a title key Setup doesn't have: why, and what to do
static std::string missing_titlekey(const nx::Keys &K, const nx::NcaInfo &n, const char *what) {
    {   // prod.keys too old for this file: it needs the master key revision it was made with
        char nm[64]; snprintf(nm, sizeof nm, n.rights_id.empty() ? "key_area_key_application_%02x" : "titlekek_%02x", n.keygen);
        if (!K.named.count(nm))
            return std::string("prod.keys has no ") + nm + ": the " + what + " was made with a newer firmware's keys (master key " +
                   fmt("%02x", n.keygen) + "). Dump prod.keys again with Lockpick_RCM from your console on a firmware at least as new "
                   "as the one the " + what + " needs (a console that has the update installed and has run it has them).";
    }
    if (n.rights_id.empty() || K.titlekeys.count(n.rights_id)) return "";
    std::string e = std::string("The ") + what + "'s title key is missing (rights id " + n.rights_id + "). ";
    if (K.personalized.count(n.rights_id))
        e += "Its .nsp has a personalized ticket (dumped with console-specific data), which only your console can decrypt. "
             "Dump it again with nxdumptool with \"remove console specific data\" on, or install it in Eden first "
             "(Setup then reads Eden's title.keys).";
    else
        e += "The .nsp has no ticket for it and title.keys (next to prod.keys) doesn't list it: dump it with its ticket, "
             "or install it in Eden first (Setup then reads Eden's title.keys).";
    return e;
}
static uint64_t dir_files(const fs::path &p) {
    uint64_t n = 0; std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) { if (ec) break; if (it->is_regular_file(ec)) n++; }
    return n;
}
static void copy_tree(const fs::path &from, const fs::path &to, bool overwrite) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | (overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::skip_existing), ec);
}
static bool make_link(const fs::path &lnk, const fs::path &target, const fs::path &workdir, const std::wstring &args, const std::wstring &desc) {
    IShellLinkW *sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl))) return false;
    sl->SetPath(target.c_str()); sl->SetWorkingDirectory(workdir.c_str()); sl->SetArguments(args.c_str()); sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile *pf = nullptr; bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

// ---------------------------------------------------------------- install
static bool install(std::string &err) {
    nx::Keys K; Found F;
    if (!scan(K, F, err)) return false;
    fs::path exe_dir; { wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH); exe_dir = fs::path(b).parent_path(); }
    fs::path payload = fs::exists(exe_dir / L"files" / L"xc3.exe") ? exe_dir / L"files" : exe_dir;
    g_hactool = (payload / L"hactool.exe").wstring();
    std::error_code ec;
    if (!fs::exists(g_hactool, ec)) { g_hactool = (exe_dir / L"hactool.exe").wstring(); }
    if (!fs::exists(g_hactool, ec)) { g_hactool = (exe_dir.parent_path() / L"tools" / L"hactool.exe").wstring(); }
    if (!fs::exists(g_hactool, ec)) { err = "hactool.exe is missing next to the installer."; return false; }
    // Built from the source (build-cmake\, no xc3.exe yet): only the game files are installed, into <folder>\game;
    // build-windows.bat then recompiles game\main.elf into xc3.exe.
    const bool data_only = !fs::exists(payload / L"xc3.exe", ec);
    if (data_only) log("No xc3.exe next to Setup: installing the game files only (for building from the source).");
    g_keys_path = get_text(g_keys);
    fs::path dest = get_text(g_dest);
    if (dest.empty()) { err = "Choose an install folder."; return false; }
    fs::path game = dest / L"game", tmp = dest / L"_setup_tmp";
    const std::string want = XC3_MAIN_BUILD_ID;
    const bool skip_data = SendMessageW(g_skip, BM_GETCHECK, 0, 0) == BST_CHECKED;
    fs::create_directories(game, ec);
    if (ec) { err = "Can't create " + u8(game.wstring()) + " (" + ec.message() + ")"; return false; }
    fs::create_directories(tmp, ec);

    if (skip_data && F.has_update && F.has_base) log("Note: \"keep the installed game data\" is ticked: the base game and update are not re-extracted.");
    if (!skip_data) {
        // space: RomFS ~ the base program, DLC, and copies of packaged NCAs
        uint64_t need = F.base.size + (F.has_update ? F.update.size : 0);
        for (auto &d : F.dlc) need += d.second.size;
        if (F.base.blob.off) need += F.base.size;
        if (F.has_update && F.update.blob.off) need += F.update.size;
        uint64_t have = free_space(dest);
        log(fmt("Space needed: about %s, free: %s", gb(need).c_str(), gb(have).c_str()));
        if (have < need) { err = "Not enough free space on that drive (need about " + gb(need) + ")."; return false; }

        std::string base_nca, upd_nca;
        status("Preparing the base game...", 0);
        if (!materialize(F.base, tmp, "base_program.nca", base_nca)) { err = "Can't read the base game file."; return false; }
        // ExeFS: the update's code must be the version this build was recompiled for
        fs::path exefs_dir;
        if (F.has_update) {
            if (!materialize(F.update, tmp, "update_program.nca", upd_nca)) { err = "Can't read the update file."; return false; }
            fs::path ex = tmp / L"exefs_update"; fs::remove_all(ex, ec);
            err = missing_titlekey(K, F.update, "update"); if (!err.empty()) return false;
            hactool(tk_arg(K, F.update) + " --exefsdir=" + quote(u8(ex.wstring())) + " " + quote(upd_nca), tmp / L"hactool_exefs_u.txt", "Reading the update's code", 0, dest);
            std::string id;
            if (!nx::nso_build_id(u8((ex / L"main").wstring()), id)) { err = "Could not decrypt the update (see _setup_tmp\\hactool_exefs_u.txt): check prod.keys / title keys."; return false; }
            log("Update code build id: " + id.substr(0, 16));
            if (want.empty() || id == want) exefs_dir = ex;
            else log("  this build of xc3.exe needs build id " + want.substr(0, 16) + " (game version 2.2.1)");
        }
        if (exefs_dir.empty()) {
            fs::path ex = tmp / L"exefs_base"; fs::remove_all(ex, ec);
            hactool(tk_arg(K, F.base) + " --exefsdir=" + quote(u8(ex.wstring())) + " " + quote(base_nca), tmp / L"hactool_exefs_b.txt", "Reading the base game's code", 0, dest);
            std::string id;
            if (!nx::nso_build_id(u8((ex / L"main").wstring()), id)) { err = "Could not decrypt the base game (see _setup_tmp\\hactool_exefs_b.txt): check prod.keys."; return false; }
            if (!want.empty() && id != want) { err = "Your game files are not the version this build was made for (it needs update 2.2.1, build id " + want.substr(0, 16) + "). Add the 2.2.1 update."; return false; }
            exefs_dir = ex; upd_nca.clear();
        }
        fs::remove_all(game / L"exefs", ec);
        copy_tree(exefs_dir, game / L"exefs", true);

        // RomFS (the update patches the base game's)
        fs::remove_all(game / L"romfs", ec);
        std::string romfs = quote(u8((game / L"romfs").wstring()));
        if (!upd_nca.empty()) {
            if (F.base.rights_id.empty()) {
                hactool(tk_arg(K, F.update) + " --basenca=" + quote(base_nca) + " --romfsdir=" + romfs + " " + quote(upd_nca), tmp / L"hactool_romfs.txt", "Extracting the game data (base + update)", F.base.size, dest);
            } else {   // the base needs its own title key: its RomFS goes through a file first
                fs::path br = tmp / L"base.romfs";
                hactool(tk_arg(K, F.base) + " --romfs=" + quote(u8(br.wstring())) + " " + quote(base_nca), tmp / L"hactool_baseromfs.txt", "Decrypting the base game data", F.base.size, dest);
                hactool(tk_arg(K, F.update) + " --baseromfs=" + quote(u8(br.wstring())) + " --romfsdir=" + romfs + " " + quote(upd_nca), tmp / L"hactool_romfs.txt", "Extracting the game data (base + update)", F.base.size, dest);
                fs::remove(br, ec);
            }
        } else {
            hactool(tk_arg(K, F.base) + " --romfsdir=" + romfs + " " + quote(base_nca), tmp / L"hactool_romfs.txt", "Extracting the game data", F.base.size, dest);
        }
        uint64_t nf = dir_files(game / L"romfs");
        log(fmt("Game data: %llu files", (unsigned long long)nf));
        if (nf < 1000) { err = "Extracting the game data failed (see _setup_tmp\\hactool_romfs.txt)."; return false; }
        if (F.base.blob.off) fs::remove(tmp / L"base_program.nca", ec);
    } else {
        std::string id;
        if (!nx::nso_build_id(u8((game / L"exefs" / L"main").wstring()), id) || (!want.empty() && id != want)) { err = "The game data in that folder is not the version this build needs: untick \"keep the installed game data\"."; return false; }
        log("Keeping the installed game data.");
    }
    {
        // DLC (also onto an existing install)
        uint64_t need = 0;
        for (auto &d : F.dlc) need += d.second.size * (d.second.blob.off ? 2 : 1);
        if (skip_data && need > free_space(dest)) { err = "Not enough free space for the DLC (need about " + gb(need) + ")."; return false; }
        for (auto &d : F.dlc) {
            if (!d.second.rights_id.empty() && !K.titlekeys.count(d.second.rights_id)) { log(fmt("DLC %d skipped: no title key", d.first)); continue; }
            std::string nca;
            if (!materialize(d.second, tmp, fmt("dlc%d.nca", d.first), nca)) { log(fmt("DLC %d: can't read the file", d.first)); continue; }
            fs::path out = game / L"aoc" / std::to_wstring(d.first) / L"romfs"; fs::remove_all(out, ec);
            hactool(tk_arg(K, d.second) + " --romfsdir=" + quote(u8(out.wstring())) + " " + quote(nca), tmp / wide(fmt("hactool_dlc%d.txt", d.first)), fmt("Extracting DLC %d", d.first), d.second.size, dest);
            log(fmt("DLC %d: %llu files", d.first, (unsigned long long)dir_files(out)));
            if (d.second.blob.off) fs::remove(nca, ec);
        }
        // executables -> ELF
        status("Converting the game's code...", 990);
        for (const char *m : {"main", "sdk", "subsdk0"}) {
            fs::path nso = game / L"exefs" / wide(m);
            if (!fs::exists(nso, ec)) continue;
            std::string e;
            if (!nx::nso_to_elf(u8(nso.wstring()), u8((game / wide(std::string(m) + ".elf")).wstring()), e)) { err = e; return false; }
        }
    }
    {   // what is installed now
        std::string have;
        for (int i = 1; i <= 15; i++) if (fs::is_directory(game / L"aoc" / std::to_wstring(i) / L"romfs", ec)) have += fmt(" %d", i);
        log(have.empty() ? "DLC installed: none" : "DLC installed:" + have);
    }

    if (data_only) {
        fs::remove_all(tmp, ec);
        log("Game files installed to " + u8(game.wstring()) + ". Next: build-windows.bat (in that folder if it is the source folder).");
        return true;
    }
    // program, patches, shader cache
    status("Copying the program...", 995);
    for (auto &e : fs::directory_iterator(payload, ec)) {
        std::string x = lower_ext(e.path());
        std::wstring fn = e.path().filename().wstring();
        if (e.is_regular_file() && (x == ".dll" || _wcsicmp(fn.c_str(), L"xc3.exe") == 0 || _wcsicmp(fn.c_str(), L"xc3.pdb") == 0))
            fs::copy_file(e.path(), dest / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        if (e.is_regular_file() && (_wcsicmp(fn.c_str(), L"xc3-launcher.exe") == 0 || _wcsicmp(fn.c_str(), L"XC3 Launcher.exe") == 0))
            fs::copy_file(e.path(), dest / L"XC3 Launcher.exe", fs::copy_options::overwrite_existing, ec);
    }
    if (fs::is_directory(payload / L"patches", ec)) { fs::remove_all(game / L"patches", ec); copy_tree(payload / L"patches", game / L"patches", true); }
    if (fs::exists(payload / L"pipeline_cache.bin", ec) && !fs::exists(game / L"pipeline_cache.bin", ec)) fs::copy_file(payload / L"pipeline_cache.bin", game / L"pipeline_cache.bin", ec);
    if (!fs::exists(dest / L"xc3.exe", ec)) { err = "Copying xc3.exe failed."; return false; }

    // saves
    if (SendMessageW(g_saves, BM_GETCHECK, 0, 0) == BST_CHECKED && !g_save_src.empty()) {
        fs::path sv = game / L"save";
        bool had = fs::is_directory(sv, ec) && !fs::is_empty(sv, ec);
        if (had) {
            SYSTEMTIME st; GetLocalTime(&st);
            fs::path bk = game / wide(fmt("save_backup_%04d%02d%02d_%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute));
            copy_tree(sv, bk, false);
            log("Previous saves backed up to " + u8(bk.wstring()));
        }
        fs::create_directories(sv, ec);
        int n = 0;
        for (auto &e : fs::directory_iterator(g_save_src, ec)) {
            std::string x = lower_ext(e.path());
            if (e.is_regular_file() && (x == ".sav" || x == ".tmb")) { fs::copy_file(e.path(), sv / e.path().filename(), fs::copy_options::overwrite_existing, ec); n++; }
        }
        log(fmt("Saves: %d files copied", n));
    }
    // launcher + shortcuts
    if (FILE *f = nx::xfopen(u8((dest / L"Play XC3.bat").wstring()), "wb")) {
        fprintf(f, "@echo off\r\ncd /d \"%%~dp0game\"\r\nstart \"\" \"..\\xc3.exe\" --game .\r\n");
        fclose(f);
    }
    if (SendMessageW(g_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        bool lnch = fs::exists(dest / L"XC3 Launcher.exe", ec);   // (the launcher: Play / Play Randomized)
        fs::path tgt = lnch ? dest / L"XC3 Launcher.exe" : dest / L"xc3.exe"; fs::path wd = lnch ? dest : game; const wchar_t *args = lnch ? L"" : L"--game .";
        bool a = make_link(fs::path(known(FOLDERID_Desktop)) / L"Xenoblade Chronicles 3 (recomp).lnk", tgt, wd, args, L"Xenoblade Chronicles 3 (recompiled)");
        bool b = make_link(fs::path(known(FOLDERID_Programs)) / L"Xenoblade Chronicles 3 (recomp).lnk", tgt, wd, args, L"Xenoblade Chronicles 3 (recompiled)");
        log(a || b ? "Shortcuts: desktop and Start menu" : "Couldn't make the shortcuts");
    }
    fs::remove_all(tmp, ec);
    log("Installed to " + u8(dest.wstring()));
    return true;
}

// ---------------------------------------------------------------- dialogs
static std::vector<std::wstring> pick(bool folder, bool multi, const wchar_t *title, const COMDLG_FILTERSPEC *flt = nullptr, UINT nflt = 0) {
    std::vector<std::wstring> out;
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD o = 0; d->GetOptions(&o);
    d->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0) | (multi ? FOS_ALLOWMULTISELECT : 0));
    d->SetTitle(title);
    if (flt) d->SetFileTypes(nflt, flt);
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItemArray *arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr))) {
            DWORD n = 0; arr->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem *it = nullptr; PWSTR p = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out.push_back(p); CoTaskMemFree(p); }
                if (it) it->Release();
            }
            arr->Release();
        }
    }
    d->Release();
    return out;
}
static void set_busy(bool b) {
    g_busy = b;
    for (int id : {ID_ADDFILES, ID_ADDFOLDER, ID_REMOVE, ID_DETECT, ID_KEYSB, ID_DESTB, ID_SCAN, ID_INSTALL, ID_BASEB, ID_UPDB, ID_BASE, ID_UPD}) EnableWindow(GetDlgItem(g_wnd, id), !b);
}
static void check_existing() {
    std::error_code ec; std::string id;
    fs::path g = fs::path(get_text(g_dest)) / L"game";
    bool ok = nx::nso_build_id(u8((g / L"exefs" / L"main").wstring()), id) && (std::string(XC3_MAIN_BUILD_ID).empty() || id == XC3_MAIN_BUILD_ID) && fs::is_directory(g / L"romfs", ec);
    EnableWindow(g_skip, ok);
    SendMessageW(g_skip, BM_SETCHECK, ok ? BST_CHECKED : BST_UNCHECKED, 0);
}

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
static int S(int v) { static UINT dpi = 0; if (!dpi) { HDC dc = GetDC(nullptr); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); } return MulDiv(v, dpi, 96); }

static void layout_controls() {
    int W = S(760), x = S(14), y = S(12), bw = S(110), bh = S(28), lw = S(150);
    int ew = W - lw - bw - S(10) - S(4);
    mk(L"STATIC", L"Xenoblade Chronicles 3 - recompiled PC version: installer.  Files can also be dropped on this window.", SS_LEFT, x, y, W, S(22), -1); y += S(30);
    mk(L"STATIC", L"Keys (prod.keys):", SS_LEFT, x, y + S(4), lw, S(20), -1);
    g_keys = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, x + lw, y, ew, S(24), ID_KEYS, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, x + W - bw, y - S(2), bw, bh, ID_KEYSB); y += S(36);
    // base game
    mk(L"STATIC", L"Base game:", SS_LEFT, x, y + S(4), lw, S(20), -1);
    g_base = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, x + lw, y, ew, S(24), ID_BASE, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, x + W - bw, y - S(2), bw, bh, ID_BASEB); y += S(26);
    g_base_st = mk(L"STATIC", L".xci or .nsp (or an installed .nca)", SS_LEFT, x + lw, y, ew, S(18), -1); y += S(24);
    // update
    mk(L"STATIC", L"Update (2.2.1):", SS_LEFT, x, y + S(4), lw, S(20), -1);
    g_upd = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, x + lw, y, ew, S(24), ID_UPD, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, x + W - bw, y - S(2), bw, bh, ID_UPDB); y += S(26);
    g_upd_st = mk(L"STATIC", L".nsp or .nca - needed: the PC version is made from 2.2.1", SS_LEFT, x + lw, y, ew, S(18), -1); y += S(24);
    // DLC
    mk(L"STATIC", L"DLC:", SS_LEFT, x, y + S(4), lw, S(20), -1);
    g_list = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, x + lw, y, ew, S(92), ID_LIST, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Add DLC...", BS_PUSHBUTTON, x + W - bw, y - S(2), bw, bh, ID_ADDFILES);
    mk(L"BUTTON", L"Remove", BS_PUSHBUTTON, x + W - bw, y + S(30), bw, bh, ID_REMOVE);
    mk(L"BUTTON", L"From folder...", BS_PUSHBUTTON, x + W - bw, y + S(62), bw, bh, ID_ADDFOLDER);
    mk(L"STATIC", L"(.nsp / .nca,\nany number)", SS_LEFT, x, y + S(28), lw - S(6), S(40), -1);
    y += S(100);
    mk(L"STATIC", L"Install to:", SS_LEFT, x, y + S(4), lw, S(20), -1);
    g_dest = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, x + lw, y, ew, S(24), ID_DEST, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, x + W - bw, y - S(2), bw, bh, ID_DESTB); y += S(34);
    g_saves = mk(L"BUTTON", L"Copy my saves from the emulator (none found)", BS_AUTOCHECKBOX, x, y, W, S(22), ID_SAVES); y += S(24);
    g_shortcut = mk(L"BUTTON", L"Desktop and Start menu shortcuts", BS_AUTOCHECKBOX, x, y, W / 2 - S(20), S(22), ID_SHORTCUT);
    g_skip = mk(L"BUTTON", L"Keep the installed base game + update", BS_AUTOCHECKBOX, x + W / 2 - S(20), y, W / 2 + S(20), S(22), ID_SKIPDATA); y += S(30);
    g_scan = mk(L"BUTTON", L"Check files", BS_PUSHBUTTON, x, y, bw, S(32), ID_SCAN);
    g_install = mk(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, x + bw + S(10), y, bw, S(32), ID_INSTALL);
    mk(L"BUTTON", L"Find emulators", BS_PUSHBUTTON, x + 2 * (bw + S(10)), y, bw + S(20), S(32), ID_DETECT);
    y += S(40);
    g_prog = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, x, y, W, S(18), ID_PROGRESS); SendMessageW(g_prog, PBM_SETRANGE, 0, MAKELPARAM(0, 1000)); y += S(22);
    g_status = mk(L"STATIC", L"Choose your files, then Install.", SS_LEFT | SS_ENDELLIPSIS, x, y, W, S(20), ID_STATUS); y += S(24);
    g_log = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, x, y, W, S(170), ID_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(g_shortcut, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(g_saves, FALSE); EnableWindow(g_skip, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_ADDFILES: {   // (any file is sorted by content: a base game / update picked here lands in its own slot)
            COMDLG_FILTERSPEC f[] = {{L"DLC (.nsp / .nca)", L"*.nsp;*.nca;*.xci"}, {L"All files", L"*.*"}};
            add_paths(pick(false, true, L"DLC files", f, 2)); return 0;
        }
        case ID_BASEB: case ID_UPDB: {
            bool base = LOWORD(w) == ID_BASEB;
            COMDLG_FILTERSPEC f[] = {{L"Switch game files", base ? L"*.xci;*.nsp;*.nca" : L"*.nsp;*.nca"}, {L"All files", L"*.*"}};
            auto r = pick(false, false, base ? L"Base game (.xci / .nsp)" : L"Update 2.2.1 (.nsp / .nca)", f, 2);
            if (r.empty()) return 0;
            nx::Keys K;
            if (!load_keys_quiet(K)) { set_slot(base ? g_base : g_upd, base ? g_base_st : g_upd_st, r[0], L"(not checked: select prod.keys)"); return 0; }
            Kind k = classify(K, r[0]);
            if (base ? k.base : k.update) set_slot(base ? g_base : g_upd, base ? g_base_st : g_upd_st, r[0], base ? (k.update ? L"base game + update" : L"base game") : L"update");
            else if (k.ok) { log("That file isn't the " + std::string(base ? "base game" : "update") + ": put where it belongs."); add_paths(r); }
            else log("Not a Xenoblade Chronicles 3 file (or the keys can't read it): " + u8(r[0]));
            if (!k.dlc.empty() && (base ? k.base : k.update)) { add_source(r[0], dlc_label(k.dlc)); refresh_list(); }
            return 0;
        }
        case ID_ADDFOLDER: add_paths(pick(true, false, L"Folder with the game files (base game, update, DLC)")); return 0;
        case ID_REMOVE: {
            int i = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)g_sources.size()) { g_sources.erase(g_sources.begin() + i); g_source_labels.erase(g_source_labels.begin() + i); refresh_list(); }
            return 0;
        }
        case ID_DETECT: detect_emulators(false); return 0;
        case ID_KEYSB: {
            COMDLG_FILTERSPEC f[] = {{L"Keys", L"*.keys"}, {L"All files", L"*.*"}};
            auto r = pick(false, false, L"prod.keys", f, 2); if (!r.empty()) SetWindowTextW(g_keys, r[0].c_str()); return 0;
        }
        case ID_DESTB: { auto r = pick(true, false, L"Install folder"); if (!r.empty()) { SetWindowTextW(g_dest, r[0].c_str()); check_existing(); } return 0; }
        case ID_DEST: if (HIWORD(w) == EN_KILLFOCUS) check_existing(); return 0;
        case ID_SCAN: case ID_INSTALL: {
            if (g_busy) return 0;
            set_busy(true);
            bool inst = LOWORD(w) == ID_INSTALL;
            SetWindowTextW(g_log, L"");
            std::thread([inst] {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                std::string err; bool ok;
                if (inst) ok = install(err);
                else { nx::Keys K; Found F; ok = scan(K, F, err); }
                if (!ok) log("ERROR: " + err);
                PostMessageW(g_wnd, WM_DONE, inst ? (ok ? 2 : 1) : (ok ? 4 : 3), 0);
                CoUninitialize();
            }).detach();
            return 0;
        }
        }
        break;
    case WM_DROPFILES: {
        HDROP d = (HDROP)w; std::vector<std::wstring> v;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) { wchar_t b[MAX_PATH * 4]; if (DragQueryFileW(d, i, b, MAX_PATH * 4)) v.push_back(b); }
        DragFinish(d);
        if (!g_busy) add_paths(v);
        return 0;
    }
    case WM_LOGLINE: {
        std::vector<std::string> v; { std::lock_guard<std::mutex> lk(g_logm); v.swap(g_pending); }
        for (auto &s : v) {
            std::wstring t = wide(s) + L"\r\n";
            int n = GetWindowTextLengthW(g_log);
            SendMessageW(g_log, EM_SETSEL, n, n); SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)t.c_str());
        }
        return 0;
    }
    case WM_PROGRESS: {
        std::string *s = (std::string *)l;
        SetWindowTextW(g_status, wide(*s).c_str()); delete s;
        if ((int)w >= 0) SendMessageW(g_prog, PBM_SETPOS, (WPARAM)w, 0);
        return 0;
    }
    case WM_DONE:
        set_busy(false);
        SendMessageW(g_prog, PBM_SETPOS, w == 2 ? 1000 : 0, 0);
        SetWindowTextW(g_status, w == 2 ? L"Done: start the game from the shortcut (TAB in game: settings)." : w == 4 ? L"Files look good: press Install." : L"Something is missing: see the messages below.");
        if (w == 2) { MessageBoxW(h, L"Xenoblade Chronicles 3 is installed.\n\nStart it from the desktop shortcut or \"Play XC3.bat\" in the install folder.\nPress TAB in game for the settings.", L"Done", MB_ICONINFORMATION); check_existing(); }
        return 0;
    case WM_CLOSE:
        if (g_busy && MessageBoxW(h, L"The installation is still running. Quit anyway?", L"Installer", MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_CTLCOLORSTATIC: if ((HWND)l != g_log) { SetBkMode((HDC)w, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); } break;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm{sizeof ncm}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(14);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSW wc{}; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"xc3setup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, S(788), S(668)}; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Xenoblade Chronicles 3 (recomp) - Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    layout_controls();
    DragAcceptFiles(g_wnd, TRUE);
    {   // started from a source folder's build-cmake\: install the game files into that source folder
        wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH); fs::path src = fs::path(b).parent_path().parent_path(); std::error_code ec;
        bool from_src = fs::exists(src / L"recomp" / L"recompile.py", ec);
        SetWindowTextW(g_dest, from_src ? src.c_str() : (fs::path(known(FOLDERID_Profile)) / L"Games" / L"Xenoblade Chronicles 3 (recomp)").c_str());
    }
    detect_emulators(true);
    check_existing();
    if (!get_text(g_upd).empty() || !g_sources.empty()) log("Filled in from your emulator. Add the base game (.xci / .nsp) if it isn't installed there, then press Check files.");
    else log("Choose your base game, update and DLC (or drop the files here), then press Check files.");
    ShowWindow(g_wnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CoUninitialize();
    return 0;
}
