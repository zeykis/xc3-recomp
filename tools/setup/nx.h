// Switch container formats the installer needs, without external libraries: keys files, AES-128 (XTS for NCA
// headers), PFS0 (NSP / ExeFS), HFS0 (XCI), NCA header identification, tickets, NSO -> ELF (LZ4).
// Content (section) decryption and update patching are left to hactool.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nx {

struct Keys {
    std::map<std::string, std::string> named;          // prod.keys: name -> hex
    std::map<std::string, std::string> titlekeys;      // rights id (32 hex, upper) -> encrypted title key (32 hex)
    std::map<std::string, std::vector<std::string>> titlekey_alts;   // every different key seen per rights id (title.keys, tickets)
    void add_titlekey(const std::string &rid, const std::string &key);  // (sets titlekeys too)
    std::set<std::string> personalized;                // rights ids whose only ticket is personalized (console-bound)
    bool load_prod(const std::string &path);           // false: missing / no header_key
    int load_titlekeys(const std::string &path);       // "rights_id = key" lines; returns how many were read
    bool header_key(uint8_t out[32]) const;
};

// A file, or a byte range inside a container file (NSP / XCI)
struct Blob {
    std::string path; uint64_t off = 0, size = 0;
    bool read(uint64_t at, void *dst, size_t n) const;
};

struct Entry { std::string name; uint64_t off, size; };   // offsets absolute in the container file
bool parse_pfs0(const Blob &b, std::vector<Entry> &out);  // PFS0 (NSP, ExeFS)
bool parse_xci_secure(const Blob &b, std::vector<Entry> &out);   // XCI: the files of the "secure" HFS0 partition

enum ContentType { CT_PROGRAM = 0, CT_META = 1, CT_CONTROL = 2, CT_MANUAL = 3, CT_DATA = 4, CT_PUBLIC_DATA = 5 };
struct NcaInfo {
    Blob blob; std::string label;        // where it is (for the log)
    uint64_t title = 0; int type = -1; uint64_t size = 0;
    std::string rights_id;               // 32 hex upper, empty = none (cartridge / no ticket needed)
    bool patch = false;                  // update program (its RomFS section patches the base game's)
    int keygen = 0;                      // master key revision its keys need (titlekek_XX / key_area_key_application_XX)
};
bool nca_identify(const Keys &k, const Blob &b, NcaInfo &out);   // false: not an NCA / no key

// ticket (.tik): rights id + common title key; false for personalized tickets (console-bound)
bool parse_ticket(const std::vector<uint8_t> &t, std::string &rights_id, std::string &key, bool *personalized = nullptr);

// NSO (exefs/main etc.) -> the ELF wrapper the runtime loads (same layout as REA triage.py nso_to_elf)
bool nso_build_id(const std::string &nso_path, std::string &hex64);
bool nso_to_elf(const std::string &nso_path, const std::string &elf_path, std::string &err);

std::string hex(const uint8_t *p, size_t n);
FILE *xfopen(const std::string &utf8_path, const char *mode);
bool unhex(const std::string &s, uint8_t *out, size_t n);

}  // namespace nx
