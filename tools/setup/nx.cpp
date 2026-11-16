#include "nx.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define FSEEK64 _fseeki64
#else
#define FSEEK64 fseeko
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace nx {

FILE *xfopen(const std::string &utf8, const char *mode) {   // (paths are UTF-8 everywhere in the installer)
#ifdef _WIN32
    wchar_t w[1024], m[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w, 1024)) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, m, 8);
    return _wfopen(w, m);
#else
    return fopen(utf8.c_str(), mode);
#endif
}
static bool read_lines(const std::string &path, std::vector<std::string> &out) {
    FILE *f = xfopen(path, "rb");
    if (!f) return false;
    std::string cur; int c;
    while ((c = fgetc(f)) != EOF) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += (char)c; }
    if (!cur.empty()) out.push_back(cur);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- small helpers
std::string hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789ABCDEF";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
static int hv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool unhex(const std::string &s, uint8_t *out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) { int a = hv(s[2 * i]), b = hv(s[2 * i + 1]); if (a < 0 || b < 0) return false; out[i] = (uint8_t)(a << 4 | b); }
    return true;
}
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static std::string upper(std::string s) { for (char &c : s) if (c >= 'a' && c <= 'z') c -= 32; return s; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

// ---------------------------------------------------------------- keys
bool Keys::load_prod(const std::string &path) {
    std::vector<std::string> lines;
    if (!read_lines(path, lines)) return false;
    for (const std::string &line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!k.empty() && !v.empty()) named[k] = v;
    }
    uint8_t hk[32];
    return header_key(hk);
}
int Keys::load_titlekeys(const std::string &path) {
    std::vector<std::string> lines;
    if (!read_lines(path, lines)) return 0;
    int n = 0;
    for (const std::string &line : lines) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = upper(trim(line.substr(0, eq))), v = upper(trim(line.substr(eq + 1)));
        uint8_t t[16];
        if (k.size() == 32 && unhex(v, t, 16)) { titlekeys[k] = v; n++; }
    }
    return n;
}
bool Keys::header_key(uint8_t out[32]) const {
    auto it = named.find("header_key");
    return it != named.end() && unhex(it->second, out, 32);
}

// ---------------------------------------------------------------- AES-128 (only for NCA headers: a few KB)
namespace {
uint8_t SB[256], ISB[256];
uint8_t xt(uint8_t x) { return (uint8_t)(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
uint8_t mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
void init_tables() {
    static bool done; if (done) return; done = true;
    // S-box from the multiplicative inverse + affine map
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^ (uint8_t)(q << 3 | q >> 5) ^ (uint8_t)(q << 4 | q >> 4);
        SB[p] = x ^ 0x63;
    } while (p != 1);
    SB[0] = 0x63;
    for (int i = 0; i < 256; i++) ISB[SB[i]] = (uint8_t)i;
}
struct Aes {
    uint8_t rk[176];
    explicit Aes(const uint8_t *key) {
        init_tables();
        memcpy(rk, key, 16);
        uint8_t rc = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
            if (i % 16 == 0) { uint8_t u = t[0]; t[0] = SB[t[1]] ^ rc; t[1] = SB[t[2]]; t[2] = SB[t[3]]; t[3] = SB[u]; rc = xt(rc); }
            for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
        }
    }
    void enc(uint8_t s[16]) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[i];
        for (int r = 1; r <= 10; r++) {
            uint8_t t[16];
            for (int i = 0; i < 16; i++) t[i] = SB[s[(i + 4 * (i % 4)) % 16]];   // SubBytes + ShiftRows
            if (r < 10)
                for (int c = 0; c < 4; c++) {
                    uint8_t *a = t + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3; a[1] = a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3;
                    a[2] = a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3; a[3] = xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3);
                }
            for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
        }
    }
    void dec(uint8_t s[16]) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
        for (int r = 9; r >= 0; r--) {
            uint8_t t[16];
            for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = ISB[s[i]];   // InvShiftRows + InvSubBytes
            for (int i = 0; i < 16; i++) t[i] ^= rk[16 * r + i];
            if (r > 0)
                for (int c = 0; c < 4; c++) {
                    uint8_t *a = t + 4 * c, a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                    a[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                    a[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                    a[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
                }
            memcpy(s, t, 16);
        }
    }
};
// AES-128-XTS decryption, Nintendo variant (sector number big-endian in the tweak)
void xts_decrypt(const uint8_t key[32], uint8_t *data, size_t len, uint64_t sector, size_t sector_size) {
    Aes k1(key), k2(key + 16);
    for (size_t off = 0; off < len; off += sector_size, sector++) {
        uint8_t tw[16] = {};
        for (int i = 0; i < 8; i++) tw[15 - i] = (uint8_t)(sector >> (8 * i));
        k2.enc(tw);
        for (size_t b = 0; b < sector_size && off + b < len; b += 16) {
            uint8_t *blk = data + off + b;
            for (int i = 0; i < 16; i++) blk[i] ^= tw[i];
            k1.dec(blk);
            for (int i = 0; i < 16; i++) blk[i] ^= tw[i];
            uint8_t carry = 0;
            for (int i = 0; i < 16; i++) { uint8_t nc = tw[i] >> 7; tw[i] = (uint8_t)(tw[i] << 1 | carry); carry = nc; }
            if (carry) tw[0] ^= 0x87;
        }
    }
}
}  // namespace

// ---------------------------------------------------------------- files / containers
bool Blob::read(uint64_t at, void *dst, size_t n) const {
    FILE *f = xfopen(path, "rb");
    if (!f) return false;
    bool ok = FSEEK64(f, (int64_t)(off + at), SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
    fclose(f);
    return ok;
}

static bool parse_fs(const Blob &b, uint64_t base, size_t entsz, const char magic[4], std::vector<Entry> &out) {
    uint8_t h[16];
    if (!b.read(base, h, 16) || memcmp(h, magic, 4)) return false;
    uint32_t n = rd32(h + 4), st = rd32(h + 8);
    if (n == 0 || n > 4096 || st > (1u << 20)) return false;
    std::vector<uint8_t> tab(entsz * n + st);
    if (!b.read(base + 16, tab.data(), tab.size())) return false;
    uint64_t data = base + 16 + entsz * n + st;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = tab.data() + entsz * i;
        uint32_t no = rd32(e + 16);
        if (no >= st) return false;
        const char *nm = (const char *)tab.data() + entsz * n + no;
        out.push_back({std::string(nm, strnlen(nm, st - no)), b.off + data + rd64(e), rd64(e + 8)});
    }
    return true;
}
bool parse_pfs0(const Blob &b, std::vector<Entry> &out) { return parse_fs(b, 0, 0x18, "PFS0", out); }
bool parse_xci_secure(const Blob &b, std::vector<Entry> &out) {
    uint8_t h[0x200];
    uint64_t extra = 0;
    if (!b.read(0x100, h, 0x100)) return false;
    if (memcmp(h, "HEAD", 4)) { if (!b.read(0x1100, h, 0x100) || memcmp(h, "HEAD", 4)) return false; extra = 0x1000; }
    uint64_t root = rd64(h + 0x30) + extra;
    std::vector<Entry> parts;
    Blob rb = b;
    if (!parse_fs(rb, root, 0x40, "HFS0", parts)) return false;
    for (const Entry &p : parts)
        if (p.name == "secure") {
            Blob sb = b; sb.off = 0;
            return parse_fs(sb, p.off, 0x40, "HFS0", out);
        }
    return false;
}

bool nca_identify(const Keys &k, const Blob &b, NcaInfo &out) {
    uint8_t key[32], h[0xC00];
    if (!k.header_key(key) || !b.read(0, h, sizeof h)) return false;
    xts_decrypt(key, h, sizeof h, 0, 0x200);   // (NCA3: header + the four section headers, sectors 0..5)
    if (memcmp(h + 0x200, "NCA3", 4) && memcmp(h + 0x200, "NCA2", 4)) return false;
    out.blob = b;
    out.type = h[0x205];
    out.size = rd64(h + 0x208);
    out.title = rd64(h + 0x210);
    bool rid = false;
    for (int i = 0; i < 16; i++) if (h[0x230 + i]) rid = true;
    out.rights_id = rid ? hex(h + 0x230, 16) : std::string();
    out.patch = false;   // an update's program: a section patching the base RomFS (BKTR)
    for (int i = 0; i < 4; i++) if (!memcmp(h + 0x400 + 0x200 * i + 0x110, "BKTR", 4) || !memcmp(h + 0x400 + 0x200 * i + 0x130, "BKTR", 4)) out.patch = true;
    return true;
}

bool parse_ticket(const std::vector<uint8_t> &t, std::string &rights_id, std::string &key) {
    if (t.size() < 4) return false;
    uint32_t sig = rd32(t.data());
    size_t hdr;
    switch (sig) {
    case 0x10000: case 0x10003: hdr = 4 + 0x200 + 0x3C; break;
    case 0x10001: case 0x10004: hdr = 4 + 0x100 + 0x3C; break;
    case 0x10002: case 0x10005: hdr = 4 + 0x3C + 0x40; break;
    default: return false;
    }
    if (t.size() < hdr + 0x170) return false;
    if (t[hdr + 0x141] != 0) return false;   // personalized: needs the console's keys
    key = hex(t.data() + hdr + 0x40, 16);
    rights_id = hex(t.data() + hdr + 0x160, 16);
    return true;
}

// ---------------------------------------------------------------- NSO
static bool lz4_block(const uint8_t *src, size_t sn, uint8_t *dst, size_t dn) {
    size_t i = 0, o = 0;
    while (i < sn) {
        uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t c; do { if (i >= sn) return false; c = src[i++]; lit += c; } while (c == 255); }
        if (i + lit > sn || o + lit > dn) return false;
        memcpy(dst + o, src + i, lit); i += lit; o += lit;
        if (i >= sn) break;   // last sequence: literals only
        if (i + 2 > sn) return false;
        size_t off = src[i] | src[i + 1] << 8; i += 2;
        size_t ml = (tok & 15) + 4;
        if ((tok & 15) == 15) { uint8_t c; do { if (i >= sn) return false; c = src[i++]; ml += c; } while (c == 255); }
        if (off == 0 || off > o || o + ml > dn) return false;
        for (size_t k = 0; k < ml; k++, o++) dst[o] = dst[o - off];
    }
    return o == dn;
}
static bool read_all(const std::string &p, std::vector<uint8_t> &d) {
    FILE *f = xfopen(p, "rb");
    if (!f) return false;
    FSEEK64(f, 0, SEEK_END);
#ifdef _WIN32
    int64_t n = _ftelli64(f);
#else
    int64_t n = ftello(f);
#endif
    FSEEK64(f, 0, SEEK_SET);
    d.resize((size_t)n);
    bool ok = fread(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}
bool nso_build_id(const std::string &p, std::string &h64) {
    uint8_t h[0x60];
    Blob b{p, 0, 0};
    if (!b.read(0, h, sizeof h) || memcmp(h, "NSO0", 4)) return false;
    h64 = hex(h + 0x40, 0x20);
    return true;
}
bool nso_to_elf(const std::string &nso, const std::string &elf, std::string &err) {
    std::vector<uint8_t> d;
    if (!read_all(nso, d) || d.size() < 0x100 || memcmp(d.data(), "NSO0", 4)) { err = "not an NSO: " + nso; return false; }
    uint32_t flags = rd32(&d[0xC]), bss = rd32(&d[0x3C]);
    struct Seg { uint32_t fo, mo, sz, csz; bool lz; } s[3];
    size_t img = 0;
    for (int i = 0; i < 3; i++) {
        s[i] = {rd32(&d[0x10 + i * 0x10]), rd32(&d[0x14 + i * 0x10]), rd32(&d[0x18 + i * 0x10]), rd32(&d[0x60 + i * 4]), (flags >> i & 1) != 0};
        img = std::max<size_t>(img, (size_t)s[i].mo + s[i].sz);
    }
    std::vector<uint8_t> im(img);
    for (int i = 0; i < 3; i++) {
        size_t cn = s[i].lz ? s[i].csz : s[i].sz;
        if ((size_t)s[i].fo + cn > d.size()) { err = "truncated NSO"; return false; }
        if (s[i].lz) { if (!lz4_block(&d[s[i].fo], cn, &im[s[i].mo], s[i].sz)) { err = "LZ4 error in " + nso; return false; } }
        else memcpy(&im[s[i].mo], &d[s[i].fo], s[i].sz);
    }
    const uint64_t PAGE = 0x1000;
    struct Ph { uint32_t type, flags; uint64_t off, va, pa, fs, ms, al; };
    std::vector<Ph> ph;
    const uint32_t fl[3] = {5, 4, 6};
    for (int i = 0; i < 3; i++) ph.push_back({1, fl[i], PAGE + s[i].mo, s[i].mo, s[i].mo, s[i].sz, (uint64_t)s[i].sz + (i == 2 ? bss : 0), PAGE});
    uint32_t mod0 = rd32(&im[4]);
    if ((size_t)mod0 + 8 <= im.size() && !memcmp(&im[mod0], "MOD0", 4)) {
        int32_t rel; memcpy(&rel, &im[mod0 + 4], 4);
        uint64_t doff = (uint64_t)((int64_t)mod0 + rel), n = 0;
        while (doff + n * 16 + 16 <= im.size() && rd64(&im[doff + n * 16]) != 0) n++;
        ph.push_back({2, 6, PAGE + doff, doff, doff, (n + 1) * 16, (n + 1) * 16, 8});
    }
    std::vector<uint8_t> out(PAGE + im.size());
    uint8_t *e = out.data();
    memcpy(e, "\x7f" "ELF", 4); e[4] = 2; e[5] = 1; e[6] = 1;
    auto w16 = [&](size_t o, uint16_t v) { memcpy(e + o, &v, 2); };
    auto w32 = [&](size_t o, uint32_t v) { memcpy(e + o, &v, 4); };
    auto w64 = [&](size_t o, uint64_t v) { memcpy(e + o, &v, 8); };
    w16(16, 3); w16(18, 183); w32(20, 1); w64(24, s[0].mo); w64(32, 64); w64(40, 0); w32(48, 0);
    w16(52, 64); w16(54, 56); w16(56, (uint16_t)ph.size()); w16(58, 64); w16(60, 0); w16(62, 0);
    for (size_t i = 0; i < ph.size(); i++) {
        size_t o = 64 + 56 * i;
        w32(o, ph[i].type); w32(o + 4, ph[i].flags); w64(o + 8, ph[i].off); w64(o + 16, ph[i].va);
        w64(o + 24, ph[i].pa); w64(o + 32, ph[i].fs); w64(o + 40, ph[i].ms); w64(o + 48, ph[i].al);
    }
    memcpy(out.data() + PAGE, im.data(), im.size());
    FILE *f = xfopen(elf, "wb");
    if (!f) { err = "can't write " + elf; return false; }
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!ok) err = "write failed: " + elf;
    return ok;
}

}  // namespace nx
