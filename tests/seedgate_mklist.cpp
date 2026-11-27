// seedgate31 A2: writes a small but real shader key list (VGXSHL01, this tree's key structs and generator version) for the refill_pack.py pipeline test
// (tools/test_seedgate.sh): keys that differ in what the generator cares about, programs that pair them. The list is data; nothing here is a game asset.
//   seedgate_mklist <out vgx_shaderlist.bin> [vertex keys, default 12] [fragment keys, default 30]
#include "vgx_shadergen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace vgx;
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const int nv = argc > 2 ? std::atoi(argv[2]) : 12, nf = argc > 3 ? std::atoi(argv[3]) : 30;
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) { std::perror(argv[1]); return 1; }
    struct { char magic[8]; uint32_t vs, fs, gen, res; } h{};
    std::memcpy(h.magic, "VGXSHL01", 8);
    h.vs = sizeof(VsKey); h.fs = sizeof(FsKey); h.gen = kShaderGenVersion;
    std::fwrite(&h, sizeof h, 1, f);
    auto put = [&](uint32_t type, const void* p, size_t n) { std::fwrite(&type, 4, 1, f); std::fwrite(p, n, 1, f); };
    for (int i = 0; i < nv; ++i) {
        VsKey k;
        k.numTexGens = static_cast<uint8_t>(i % 4);
        k.numChans = static_cast<uint8_t>((i / 4) % 3);
        k.chan[0].enable = k.numChans ? 1 : 0;
        k.perVtxPos = static_cast<uint8_t>(i % 2);
        put(1, &k, sizeof k);
    }
    for (int i = 0; i < nf; ++i) {
        FsKey k;
        k.numStages = static_cast<uint8_t>(1 + i % 4);
        k.numTexGens = static_cast<uint8_t>(i % 3);
        k.alphaTestOn = static_cast<uint8_t>((i / 12) % 2);
        for (int s = 0; s < k.numStages; ++s) { k.stage[s].texEnable = static_cast<uint8_t>(s < k.numTexGens); k.stage[s].rasChan = 0; k.stage[s].color = 0x08AF0u + static_cast<uint32_t>(i); }
        put(2, &k, sizeof k);
    }
    for (int i = 0; i < nf; ++i) { uint32_t pr[2] = {static_cast<uint32_t>(i % nv), static_cast<uint32_t>(i)}; put(3, pr, 8); }
    std::fclose(f);
    return 0;
}
