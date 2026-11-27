// ta24: Honeyhive's vertex keys (stage list) -> generated vertex shader text + op census per key class.
#include "vgx_shadergen.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
using namespace vgx;
struct Hdr { char magic[8]; uint32_t vs, fs, gen, res; };
static int count(const std::string& s, const char* t) { int n = 0; for (size_t p = 0; (p = s.find(t, p)) != std::string::npos; p += strlen(t)) ++n; return n; }
int main(int argc, char** argv) {
    FILE* f = std::fopen(argv[1], "rb");
    Hdr h; std::fread(&h, sizeof h, 1, f);
    std::unordered_map<uint64_t, VsKey> V; uint32_t type;
    while (std::fread(&type, 4, 1, f) == 1) {
        if (type == 1) { VsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; V[HashKey(&k, sizeof k)] = k; }
        else if (type == 2) { FsKey k; if (std::fread(&k, sizeof k, 1, f) != 1) break; }
        else if (type == 3) { uint32_t v[2]; if (std::fread(v, 8, 1, f) != 1) break; }
        else if (type == 4) { uint32_t v[4]; if (std::fread(v, 16, 1, f) != 1) break; }
        else break;
    }
    std::fclose(f);
    FILE* s = std::fopen(argv[2], "rb");
    char mg[8]; std::fread(mg, 8, 1, s);
    struct R { uint32_t stage, lb; uint64_t vs, fs; } r;
    const uint32_t want = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16));
    std::set<uint64_t> vss;
    while (std::fread(&r, sizeof r, 1, s) == 1) if (r.stage == want) vss.insert(r.vs);
    const bool dump = argc > 4;
    int idx = 0;
    std::printf("key  perVtxPos nTG nCh lit  dp  sqrt len div minmax lines  cfg\n");
    long totDot = 0, totSqrt = 0, totDiv = 0;
    for (uint64_t hv : vss) {
        auto it = V.find(hv); if (it == V.end()) continue;
        const VsKey& k = it->second;
        ShaderSource src; GenerateVertexShader(k, src);
        const std::string& t = src.text;
        int dp = count(t, "dot("), sq = count(t, "sqrt("), ln = count(t, "length("), dv = count(t, " / ") , mm = count(t, "max(") + count(t, "min(") + count(t, "clamp(");
        int lines = count(t, ";\n");
        std::string cfg; int lit = 0;
        for (int i = 0; i < 4; ++i) { if ((i & 1) >= k.numChans) continue; const auto& c = k.chan[i];
            char b[64]; std::snprintf(b, sizeof b, "%s%d[e%d m%02x d%d a%d] ", i < 2 ? "C" : "A", i & 1, c.enable, c.lightMask, c.diffuse, c.attn); cfg += b; if (c.enable && c.lightMask) ++lit; }
        std::printf("%03d  %d %d %d %d  %3d %2d %2d %2d %2d %3d  %s\n", idx, k.perVtxPos, k.numTexGens, k.numChans, lit, dp, sq, ln, dv, mm, lines, cfg.c_str());
        totDot += dp; totSqrt += sq; totDiv += dv;
        if (dump && idx == std::atoi(argv[4])) std::printf("----\n%s----\n", t.c_str());
        ++idx;
    }
    std::printf("keys %d, sum dot %ld sqrt %ld div %ld\n", idx, totDot, totSqrt, totDiv);
}
