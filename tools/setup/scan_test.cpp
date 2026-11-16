// Linux test of the installer core: nx_test KEYS (scan PATH... | nso NSO ELF | aes)
#include "nx.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;
namespace nx { void aes_selftest(); }
int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "nso")) { std::string e, id; nx::nso_build_id(argv[2], id); printf("build id %s\n", id.c_str()); bool ok = nx::nso_to_elf(argv[2], argv[3], e); printf("%s %s\n", ok ? "ok" : "FAIL", e.c_str()); return !ok; }
    nx::Keys k; if (!k.load_prod(argv[1])) { printf("no header_key\n"); return 1; }
    for (int i = 3; i < argc; i++) {
        std::vector<std::string> files;
        if (fs::is_directory(argv[i])) { for (auto &e : fs::recursive_directory_iterator(argv[i])) if (e.is_regular_file()) files.push_back(e.path().string()); }
        else files.push_back(argv[i]);
        for (auto &p : files) {
            nx::Blob b{p, 0, (uint64_t)fs::file_size(p)};
            std::vector<nx::Entry> ents;
            bool xci = p.size() > 4 && p.substr(p.size() - 4) == ".xci", nsp = p.size() > 4 && p.substr(p.size() - 4) == ".nsp";
            if (xci || nsp) {
                bool ok = xci ? nx::parse_xci_secure(b, ents) : nx::parse_pfs0(b, ents);
                printf("%s: %s, %zu entries\n", p.c_str(), ok ? "ok" : "PARSE FAIL", ents.size());
                for (auto &e : ents) {
                    nx::NcaInfo in; nx::Blob eb{p, e.off, e.size};
                    if (e.name.size() > 4 && e.name.substr(e.name.size() - 4) == ".nca" && nx::nca_identify(k, eb, in))
                        printf("   %-44s %016llX type %d size %llu %s\n", e.name.c_str(), (unsigned long long)in.title, in.type, (unsigned long long)in.size, in.rights_id.empty() ? "" : "rights");
                    else printf("   %s (%llu)\n", e.name.c_str(), (unsigned long long)e.size);
                }
            } else {
                nx::NcaInfo in;
                if (nx::nca_identify(k, b, in)) printf("%s: %016llX type %d size %llu/%llu %s\n", p.c_str(), (unsigned long long)in.title, in.type, (unsigned long long)in.size, (unsigned long long)b.size, in.rights_id.empty() ? "" : "rights"), in.patch ? printf("   ^ patch (update)\n") : 0;
            }
        }
    }
}
