// GXV_PALROWS: the vertex shader text for the full (perVtxPos 1), short (2) and no palette (0) keys, with lighting, a texgen and
// fog variants, is byte-identical to what the generator made before the small palette existed (digest of all the texts below,
// made with the generator at the commit before the switch); the small palette (3) differs from the short one only in the array
// size, the clamp and the normal offset. Prints the digest; exits 1 on a mismatch. No GL needed.
#include "vgx_shadergen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
uint64_t Fnv(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::string Gen(const vgx::VsKey& k) {
    vgx::ShaderSource s;
    vgx::GenerateVertexShader(k, s);
    return s.text;
}
std::string Replace(std::string s, const std::string& a, const std::string& b) {
    const size_t p = s.find(a);
    if (p == std::string::npos) { std::fprintf(stderr, "FAIL: \"%s\" not in the text\n", a.c_str()); std::exit(1); }
    return s.replace(p, a.size(), b);
}
} // namespace
int main(int argc, char** argv) {
    uint64_t h = 14695981039346656037ull;
    int texts = 0;
    for (int pvp = 0; pvp <= 2; ++pvp)
        for (int chans = 0; chans <= 2; ++chans)
            for (int tg = 0; tg <= 2; ++tg)
                for (int fog = 0; fog <= 1; ++fog) {
                    vgx::VsKey k;
                    k.perVtxPos = static_cast<uint8_t>(pvp);
                    k.numChans = static_cast<uint8_t>(chans);
                    k.numTexGens = static_cast<uint8_t>(tg);
                    k.fogVtx = static_cast<uint8_t>(fog ? 1 : 0);
                    for (int c = 0; c < chans; ++c) {
                        k.chan[c].enable = 1; k.chan[c].lightMask = 1; k.chan[c].diffuse = 2;
                        k.chan[c + 2].enable = c; k.chan[c + 2].lightMask = 1;
                    }
                    for (int t = 0; t < tg; ++t) { k.tg[t].type = 0; k.tg[t].src = 1; }
                    h = Fnv(h, Gen(k));
                    ++texts;
                    if (pvp == 2 && !std::getenv("PALROWS_TEXT_PRE_SWITCH")) { // (set when built against the generator before the switch)
                        // the small palette is the short one with the array size, the clamp and the normal offset changed
                        vgx::VsKey s3 = k;
                        s3.perVtxPos = 3;
                        const std::string a = Gen(k), b = Gen(s3);
                        std::string r = Replace(a, "uniform vec4 u_pal[60];", "uniform vec4 u_pal[24];");
                        r = Replace(r, "if (pi > 27) pi = 27;", "if (pi > 9) pi = 9;");
                        if (a.find("int ni = 30 + pi;") != std::string::npos) r = Replace(r, "int ni = 30 + pi;", "int ni = 12 + pi;");
                        else if (chans > 0 || tg > 0) { /* (no normal read: nothing to change) */ }
                        if (r != b) { std::fprintf(stderr, "FAIL: small palette text differs from the short one beyond the three changes (chans %d)\n", chans); return 1; }
                    }
                }
    std::printf("%d vertex shader texts, digest %016llx\n", texts, static_cast<unsigned long long>(h));
    if (argc > 1) {
        const unsigned long long want = std::strtoull(argv[1], nullptr, 16);
        if (want != h) { std::printf("FAIL: digest differs from the pre-switch generator (%016llx)\n", want); return 1; }
        std::printf("PASS: byte-identical to the generator before GXV_PALROWS\n");
    }
    return 0;
}
