// A digest of the generated source of a fixed set of keys that uses only what the generator has always had
// (no vertex-fog field, no switches), so the same header compiles against the generator before GXV_FOGVTX /
// GXV_NODISCARD and after: the two numbers must be equal with both switches off. (The values pinned in
// vgx_fogvtx_gl.cpp were made by compiling this header with a small main against the generator of the commit before the switches.)
#pragma once
#include "vgx_shadergen.h"
#include <cstdint>
#include <string>

namespace fogvtx_digest {
inline uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

inline uint64_t FragmentDigest(unsigned* count) {
    uint64_t h = 14695981039346656037ull;
    unsigned n = 0;
    static const uint8_t kTests[][2] = {{0, 0}, {1, 0}, {6 | (3 << 3), 0}, {4 | (4 << 3), 1}, {0 | (0 << 3), 3}, {3 | (1 << 3), 1}, {6 | (7 << 3), 2}, {7 | (7 << 3), 0}};
    for (int stages = 1; stages <= 3; ++stages)
        for (int fog = 0; fog < 16; ++fog)
            for (const auto& t : kTests)
                for (int tex = 0; tex < 2; ++tex) {
                    vgx::FsKey k;
                    k.numStages = static_cast<uint8_t>(stages);
                    k.fog = static_cast<uint8_t>(fog);
                    k.alphaTestOn = t[1] != 0 || t[0] != 0;
                    k.alphaTest = static_cast<uint8_t>(t[0] | (t[1] << 6));
                    k.dstAlpha = static_cast<uint8_t>(stages & 1);
                    k.numTexGens = static_cast<uint8_t>(tex);
                    for (int s = 0; s < stages; ++s) {
                        k.stage[s].color = 0x08af0u + static_cast<uint32_t>(s) * 0x111;
                        k.stage[s].alpha = 0x1ff0u & (0x0a90u + static_cast<uint32_t>(s) * 0x70);
                        k.stage[s].texEnable = static_cast<uint8_t>(tex);
                        k.stage[s].rasChan = 0;
                    }
                    vgx::ShaderSource src;
                    vgx::GenerateFragmentShader(k, src);
                    h = Mix(h, vgx::HashKey(src.text.data(), src.text.size()));
                    ++n;
                }
    if (count) *count = n;
    return h;
}

inline uint64_t VertexDigest(unsigned* count) {
    uint64_t h = 14695981039346656037ull;
    unsigned n = 0;
    for (int chans = 0; chans <= 2; ++chans)
        for (int pvp = 0; pvp <= 2; ++pvp)
            for (int tgs = 0; tgs <= 3; ++tgs)
                for (int lit = 0; lit < 2; ++lit) {
                    vgx::VsKey k;
                    k.numChans = static_cast<uint8_t>(chans);
                    k.perVtxPos = static_cast<uint8_t>(pvp);
                    k.numTexGens = static_cast<uint8_t>(tgs);
                    for (int c = 0; c < 4; ++c) {
                        k.chan[c].enable = static_cast<uint8_t>(lit);
                        k.chan[c].lightMask = lit ? 3 : 0;
                        k.chan[c].diffuse = static_cast<uint8_t>(1 + (c & 1));
                        k.chan[c].matSrcVtx = static_cast<uint8_t>(c & 1);
                    }
                    for (int t = 0; t < tgs; ++t) { k.tg[t].src = static_cast<uint8_t>(5 + t); k.tg[t].proj = static_cast<uint8_t>(t & 1); }
                    vgx::ShaderSource src;
                    vgx::GenerateVertexShader(k, src);
                    h = Mix(h, vgx::HashKey(src.text.data(), src.text.size()));
                    ++n;
                }
    if (count) *count = n;
    return h;
}
} // namespace fogvtx_digest
