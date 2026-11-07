// Random TEV keys for the half precision tests (GXV_FS_HALF). Deterministic: the same seed gives the same keys, on any machine.
// Uses only fields of FsKey that the generator has always had, so that vgx_fshalf_test.cpp can be compiled against the generator as it was
// before GXV_FS_HALF (to pin the digest of its output) and after.
#pragma once
#include "vgx_shadergen.h"
#include <cstdint>
#include <string>

namespace fshalf_keys {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t Next() { // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    uint32_t U(uint32_t n) { return static_cast<uint32_t>((Next() >> 33) % n); } // 0..n-1
    bool P(unsigned percent) { return U(100) < percent; }
    double F() { return static_cast<double>(Next() >> 11) / 9007199254740992.0; } // [0,1)
};

// A key of the shape real games make (J3D materials): mostly one to four stages, mostly clamped, scale 1, bias 0.
// allShapes: also indirect stages, fog, compare-mode stages, logic-op output and out-of-range registers (keys the half analysis rejects).
inline vgx::FsKey RandomKey(Rng& r, bool allShapes) {
    vgx::FsKey k;
    const unsigned w = r.U(100);
    k.numStages = static_cast<uint8_t>(w < 35 ? 1 : w < 65 ? 2 : w < 80 ? 3 : w < 90 ? 4 : 5 + r.U(4));
    k.numTexGens = static_cast<uint8_t>(r.U(4));
    k.texProjMask = static_cast<uint8_t>(r.P(10) ? r.U(8) : 0);
    if (allShapes && r.P(12)) k.numIndStages = static_cast<uint8_t>(1 + r.U(2));
    for (int s = 0; s < k.numStages; ++s) {
        vgx::FsKey::Stage& st = k.stage[s];
        uint32_t c = 0, a = 0;
        for (int i = 0; i < 4; ++i) {
            c |= (r.U(16) & 15u) << (i * 4);
            a |= (r.U(8) & 7u) << (4 + i * 3);
        }
        auto Common = [&](uint32_t& v, bool compareOk) {
            const unsigned b = r.U(100);
            uint32_t bias = b < 85 ? 0 : b < 92 ? 1 : 2;
            if (allShapes && compareOk && r.P(4)) bias = 3;
            const uint32_t op = r.P(10) ? 1u : 0u, clampOn = r.P(90) ? 1u : 0u;
            const unsigned sc = r.U(100);
            const uint32_t scale = sc < 80 ? 0 : sc < 86 ? 1 : sc < 94 ? 2 : 3;
            v |= (bias << 16) | (op << 18) | (clampOn << 19) | (scale << 20) | ((r.P(70) ? 0u : r.U(4)) << 22);
        };
        Common(c, true);
        Common(a, true);
        st.color = c & 0x00FFFFFFu;
        st.alpha = a & 0x00FFFFF0u;
        st.texEnable = r.P(70) ? 1 : 0;
        st.texMap = static_cast<uint8_t>(r.U(4));
        st.texCoord = static_cast<uint8_t>(k.numTexGens ? r.U(k.numTexGens) : 0);
        const unsigned rc = r.U(100);
        st.rasChan = static_cast<uint8_t>(rc < 60 ? 0 : rc < 70 ? 1 : 7);
        st.kcsel = static_cast<uint8_t>(r.P(50) ? r.U(8) : 12 + r.U(20));
        st.kasel = static_cast<uint8_t>(r.P(50) ? r.U(8) : 16 + r.U(16));
        auto Swap = [&]() -> uint8_t {
            if (!r.P(20)) return 0xE4;
            uint8_t v = 0;
            for (int i = 0; i < 4; ++i) v = static_cast<uint8_t>(v | (r.U(4) << (i * 2)));
            return v;
        };
        st.rasSwap = Swap();
        st.texSwap = Swap();
        if (allShapes && k.numIndStages && r.P(50)) st.ind = 1u + r.U(0x1FFFFE);
        if (r.P(3)) st.texOff = 1;
    }
    for (int i = 0; i < k.numIndStages; ++i) { k.ind[i].texMap = static_cast<uint8_t>(r.U(4)); k.ind[i].texCoord = static_cast<uint8_t>(k.numTexGens ? r.U(k.numTexGens) : 0); }
    if (r.P(20)) {
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(r.U(8) | (r.U(8) << 3) | (r.U(4) << 6));
    }
    k.dstAlpha = r.P(5) ? 1 : 0;
    k.regFromVtx = r.P(5) ? static_cast<uint8_t>(1 + r.U(3)) : 0;
    if (allShapes) {
        if (r.P(10)) k.fog = static_cast<uint8_t>((2 + r.U(6)) | (r.P(30) ? 8 : 0) | (r.P(25) ? vgx::kFogVtxBit : 0));
        if (r.P(2)) k.logicSrc = static_cast<uint8_t>(1 + r.U(2));
        if (r.P(3)) k.inWrap = 1;
    }
    return k;
}

inline uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
// A digest of the generated source of 3000 random keys of every shape (with the default environment of the generator).
inline uint64_t SourceDigest(unsigned* count) {
    Rng r(0xF5A1F);
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < 3000; ++i) {
        const vgx::FsKey k = RandomKey(r, true);
        vgx::ShaderSource src;
        vgx::GenerateFragmentShader(k, src);
        h = Mix(h, vgx::HashKey(src.text.data(), src.text.size()));
    }
    if (count) *count = 3000;
    return h;
}

} // namespace fshalf_keys
