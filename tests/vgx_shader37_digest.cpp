// shader37: a digest of every shader text (and u_tev row count) a vgx_shaderlist.bin's keys make, for the "switches off == before" check of
// tools/test_vgx_shader37.sh; with --changed it also prints how many keys differ from the same keys made with the switches off. No GL.
#include "vgx_shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
namespace vgx { bool LogEnabled() { return false; } }
namespace {
uint64_t Fnv(uint64_t h, const std::string& s) { for (unsigned char c : s) h = (h ^ c) * 1099511628211ull; return h; }
size_t Count(const std::string& s, const char* p) { size_t n = 0, i = 0; while ((i = s.find(p, i)) != std::string::npos) { ++n; i += std::strlen(p); } return n; }
}
int main(int argc, char** argv) {
    FILE* f = argc > 1 ? std::fopen(argv[1], "rb") : nullptr;
    if (!f) { std::fprintf(stderr, "usage: digest <vgx_shaderlist.bin> [--changed]\n"); return 2; }
    struct H { char m[8]; uint32_t vs, fs, ver, res; } h;
    if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.m, "VGXSHL01", 8) || h.vs != sizeof(vgx::VsKey) || h.fs != sizeof(vgx::FsKey)) { std::fprintf(stderr, "bad list\n"); return 2; }
    std::vector<vgx::VsKey> vk;
    std::vector<vgx::FsKey> fk;
    uint32_t t;
    while (std::fread(&t, 4, 1, f) == 1) {
        if (t == 1) { vgx::VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; vk.push_back(k); }
        else if (t == 2) { vgx::FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; fk.push_back(k); }
        else if (t == 3) { uint32_t v[2]; if (std::fread(v, sizeof v, 1, f) != 1) break; }
        else if (t == 4) { uint32_t v[4]; if (std::fread(v, sizeof v, 1, f) != 1) break; }
        else break;
    }
    std::fclose(f);
#ifdef SHADER37_NEW // (not in the generator before the switches: the digest part below compiles against both)
    if (argc > 2 && !std::strcmp(argv[2], "--changed")) {
        const long fsMode = vgx::FsAtFastMode(), vsMode = vgx::VsNoBranchMode();
        size_t fsChanged = 0, fsAt = 0, vsChanged = 0, ifsOld = 0, ifsNew = 0, rows = 0;
        for (const auto& k : fk) {
            vgx::ShaderSource a, b;
            vgx::FsAtFastModeForTest(0); vgx::GenerateFragmentShader(k, a);
            vgx::FsAtFastModeForTest(fsMode); vgx::GenerateFragmentShader(k, b);
            fsChanged += a.text != b.text;
            fsAt += k.alphaTestOn != 0;
            rows += b.nTev > a.nTev;
        }
        for (const auto& k : vk) {
            vgx::ShaderSource a, b;
            vgx::VsNoBranchModeForTest(0); vgx::GenerateVertexShader(k, a);
            vgx::VsNoBranchModeForTest(vsMode); vgx::GenerateVertexShader(k, b);
            vsChanged += a.text != b.text;
            ifsOld += Count(a.text, "if ("); ifsNew += Count(b.text, "if (");
        }
        std::printf("ATFAST=%ld NOBRANCH=%ld: fragment shaders %zu of %zu changed (%zu with an alpha test, %zu with one more u_tev row); vertex shaders %zu of %zu changed, `if` count %zu -> %zu\n",
                    fsMode, vsMode, fsChanged, fk.size(), fsAt, rows, vsChanged, vk.size(), ifsOld, ifsNew);
        return 0;
    }
#endif
    uint64_t hv = 14695981039346656037ull, hf = hv;
    for (const auto& k : vk) { vgx::ShaderSource s; vgx::GenerateVertexShader(k, s); hv = Fnv(hv, s.text); }
    for (const auto& k : fk) { vgx::ShaderSource s; vgx::GenerateFragmentShader(k, s); hf = Fnv(hf, s.text); hf = Fnv(hf, std::to_string(s.nTev)); }
    std::printf("vs %016llx fs %016llx (%zu %zu)\n", static_cast<unsigned long long>(hv), static_cast<unsigned long long>(hf), vk.size(), fk.size());
    return 0;
}
