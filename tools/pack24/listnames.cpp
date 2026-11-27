// listnames (pack24: canon24b's tool + "--used" / "--marked-only"; pack23 tools/listnames.cpp + GXV_TEVIN_CANON=2 keys as the front end makes them): the vitaGL cache file names (<v|f>/<HH>/<HASH>.gxp, as custom_shaders.c spells them) of every shader text of a warm-up list
// (vgx_shaderlist.bin), made by the shader generator of the tree it is built against, under the environment it runs in (the game's
// entry.cpp defaults: refill_pack.py passes them). Text = "#version 100\n" + generator text, as vgx_backend_gl.cpp Compile() hands it to
// glShaderSource (GXV_FS_HALF=2's marker line is not modelled: that mode is an A/B switch only). The register-folding fragment
// variants (FsKey::regFromVtx = 3) are keys of the list in their own right.
//   listnames <vgx_shaderlist.bin> [--fog] [--used] [--marked-only]
//        one name per line, sorted, unique; "--fog": only texts with the fog varying; "--used": only keys that a program (type 3) or a
//        variant (type 4) record of the list refers to (the keys the warm-up really links; the rest are keys the list knows but never pairs);
//        "--marked-only": only the fragment texts of marked keys (TEVIN_CANON).
//   The name set of the whole list goes to stdout, a one-line count summary to stderr.
//   "--closure" (seedgate31 A2): instead of the names, one line per key of the list: "<v|f> <key hash, 16 hex digits> <cache file name>". The key hash is what a stage
//        record holds (vgx::HashKey of the key struct, for a fragment key as the game warms it, after CanonListFsKey). refill_pack.py's closure gate looks every record's
//        two hashes up here: a hash not in the output is a record without a key recipe, a name not in the merged folder is a record without its compiled file.
// Exit 2: not a list of this generator (key sizes / generator version differ).
#define XXH_INLINE_ALL
#include "vgx_shadergen.h"
#include "xxhash.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace vgx;
extern "C" void port_log_summary(const char*, ...) {}
struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; };
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: listnames <vgx_shaderlist.bin> [--fog]\n"); return 1; }
    bool fogOnly = false, usedOnly = false, markedOnly = false, closure = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fog")) fogOnly = true;
        else if (!std::strcmp(argv[i], "--used")) usedOnly = true;
        else if (!std::strcmp(argv[i], "--marked-only")) markedOnly = true;
        else if (!std::strcmp(argv[i], "--closure")) closure = true;
        else { std::fprintf(stderr, "listnames: unknown option %s\n", argv[i]); return 1; }
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    Hdr h;
    if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "VGXSHL01", 8) != 0 || h.vs != sizeof(VsKey) || h.fs != sizeof(FsKey) ||
        h.gen != kShaderGenVersion) {
        std::fprintf(stderr, "listnames: %s is not a list of this generator (sizes %u/%u gen %u; here %zu/%zu gen %u)\n", argv[1], h.vs, h.fs, h.gen,
                     sizeof(VsKey), sizeof(FsKey), kShaderGenVersion);
        return 2;
    }
    std::vector<VsKey> V; std::vector<FsKey> F; uint32_t type;
    std::set<uint32_t> usedV, usedF;
    while (std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; V.push_back(k); }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; F.push_back(k); }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, 8, 1, f) != 1) break; usedV.insert(v[0]); usedF.insert(v[1]); }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, 16, 1, f) != 1) break; usedV.insert(v[0]); usedF.insert(v[1]); }
        else break;
    }
    std::fclose(f);
    std::set<std::string> out;
    const std::string fog = FogVaryingName();
    std::set<std::string> closureLines;
    unsigned long long curHash = 0;
    auto emit = [&](char kind, const std::string& t) {
        if (fogOnly && t.find(fog) == std::string::npos) return;
        const std::string s = "#version 100\n" + t;
        const unsigned long long hh = XXH3_64bits(s.data(), s.size());
        char b[64]; std::snprintf(b, sizeof b, "%c/%02X/%llX.gxp", kind, static_cast<unsigned>(hh >> 56), hh); out.insert(b);
        if (closure) { char c[96]; std::snprintf(c, sizeof c, "%c %016llx %s", kind, curHash, b); closureLines.insert(c); }
    };
    if (!markedOnly)
        for (size_t i = 0; i < V.size(); ++i) { if (usedOnly && !usedV.count(static_cast<uint32_t>(i))) continue; ShaderSource s; GenerateVertexShader(V[i], s); curHash = HashKey(&V[i], sizeof(VsKey)); emit('v', s.text); }
    // (canon24b) GXV_TEVIN_CANON=2: a marked list key the front end would now make with its preWrap mask (BuildFsKey), as the game draws it; a
    // list recorded under =0 / =1 has none. With "--marked-only" only the texts of marked keys.
    for (size_t i = 0; i < F.size(); ++i) {
        FsKey k = F[i];
        if (usedOnly && !usedF.count(static_cast<uint32_t>(i))) continue;
        if (markedOnly && !(k.inWrap & 1)) continue;
        CanonListFsKey(k); // (as vgx_draw.cpp EnsureShaderList: the key the game warms and draws under this GXV_TEVIN_CANON)
        ShaderSource s; GenerateFragmentShader(k, s); curHash = HashKey(&k, sizeof(FsKey)); emit('f', s.text);
    }
    if (closure) for (auto& x : closureLines) std::puts(x.c_str());
    else for (auto& x : out) std::puts(x.c_str());
    std::fprintf(stderr, "listnames: %zu vertex keys, %zu fragment keys -> %zu names (fog varying %s)\n", V.size(), F.size(), out.size(), fog.c_str());
}
