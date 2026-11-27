// texup27 (GXV_TEXSOFT_FAST): the softness test of a texture (compositor25 round 2 (a), vgx_backend_gl.cpp fxl::NoteTex) in one pass over the texels, no scratch
// vector, integer sums for the 8-bit formats. Header only (vgx_cmd.h and the standard library), so the back end and the host test (tests/texsoft_model.cpp, which holds the
// old two-stage code as the reference) share it.
//
// WHY. Every console run that armed the softness census (GXV_FXLOWRES_AUTO or GXV_GLOW26 above 0; the 22-stage tour env, ladder rungs R3/R4) paid 3.1-4.5 ms per uploaded
// texture on the render thread, flat in the texture's bytes (a 256 x 256 cap bounds the work), against 0.35-0.45 ms in every run that did not arm it: the old code decoded
// the whole level 0 into a vector of floats (DXT: per texel branches, a palette per block) and then walked it three times in double precision. This pass reads each 4 x 4
// block once. The verdict is the old one: soft = flat (variance <= 1e-4 per texel) or the energy left after averaging each 4 x 4 block <= 10 % of the variance.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "vgx_cmd.h"

namespace vgx::texsoft {

// The softness verdict from the sums: n texels; S, Q the sums of v and v^2 over all texels; R the sum over 4 x 4 blocks of (sum of v^2 - (sum of v)^2 / 16).
inline bool Verdict(double n, double S, double Q, double R) {
    const double var = Q - S * S / n;
    if (var <= 1.0e-4 * n) return true; // (flat)
    return R <= 0.10 * var;
}

struct Sums {
    double S = 0, Q = 0, R = 0;
    void Block(const float* v) {
        double s = 0, q = 0;
        for (int k = 0; k < 16; ++k) { const double d = v[k]; s += d; q += d * d; }
        S += s; Q += q; R += q - s * s * (1.0 / 16.0);
    }
};

// true: handled, *soft is the verdict. false: not handled (the caller keeps the old code): size not 8..256 or not a multiple of 4, a level too short, an unknown format.
inline bool Soft(const TexUpload& u, bool* soft) {
    const int w = u.width, h = u.height;
    if (w < 8 || h < 8 || w > 256 || h > 256 || (w & 3) || (h & 3) || u.levels.empty()) return false;
    const std::vector<uint8_t>& Lv = u.levels[0];
    const uint8_t* L = Lv.data();
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    const int bw = w / 4, bh = h / 4;
    float v[16];
    switch (u.format) {
    case TF_RGBA8: case TF_LA88: case TF_I8: {
        const size_t bpp = u.format == TF_RGBA8 ? 4u : u.format == TF_LA88 ? 2u : 1u;
        if (Lv.size() < n * bpp) return false;
        uint64_t S = 0, Q = 0, R16 = 0;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                uint32_t s = 0;
                uint64_t q = 0;
                for (int y = 0; y < 4; ++y) {
                    const size_t row = (static_cast<size_t>(by) * 4 + y) * w + static_cast<size_t>(bx) * 4;
                    for (int x = 0; x < 4; ++x) {
                        uint32_t p;
                        if (bpp == 4u) {
                            const uint8_t* t = L + (row + x) * 4;
                            p = std::max<uint32_t>(t[0], std::max<uint32_t>(t[1], t[2])) * t[3];
                        } else if (bpp == 2u) {
                            const uint8_t* t = L + (row + x) * 2;
                            p = static_cast<uint32_t>(t[0]) * t[1];
                        } else {
                            const uint32_t i = L[row + x];
                            p = i * i;
                        }
                        s += p;
                        q += static_cast<uint64_t>(p) * p;
                    }
                }
                S += s; Q += q;
                R16 += q * 16u - static_cast<uint64_t>(s) * s;
            }
        const double sc = 1.0 / 65025.0;
        *soft = Verdict(static_cast<double>(n), static_cast<double>(S) * sc, static_cast<double>(Q) * sc * sc, static_cast<double>(R16) * (1.0 / 16.0) * sc * sc);
        return true;
    }
    case TF_RGB565: case TF_RGBA5551: case TF_RGBA4444: {
        if (Lv.size() < n * 2) return false;
        Sums a;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                for (int y = 0; y < 4; ++y) {
                    const size_t row = (static_cast<size_t>(by) * 4 + y) * w + static_cast<size_t>(bx) * 4;
                    for (int x = 0; x < 4; ++x) {
                        const uint8_t* t = L + (row + x) * 2;
                        const uint32_t p = t[0] | (static_cast<uint32_t>(t[1]) << 8);
                        float r, g, b, al = 1.0f;
                        if (u.format == TF_RGB565) { r = ((p >> 11) & 31) / 31.0f; g = ((p >> 5) & 63) / 63.0f; b = (p & 31) / 31.0f; }
                        else if (u.format == TF_RGBA5551) { r = ((p >> 11) & 31) / 31.0f; g = ((p >> 6) & 31) / 31.0f; b = ((p >> 1) & 31) / 31.0f; al = static_cast<float>(p & 1); }
                        else { r = ((p >> 12) & 15) / 15.0f; g = ((p >> 8) & 15) / 15.0f; b = ((p >> 4) & 15) / 15.0f; al = (p & 15) / 15.0f; }
                        v[y * 4 + x] = std::max(r, std::max(g, b)) * al;
                    }
                }
                a.Block(v);
            }
        *soft = Verdict(static_cast<double>(n), a.S, a.Q, a.R);
        return true;
    }
    case TF_DXT1: case TF_DXT5: {
        const bool d5 = u.format == TF_DXT5;
        const int bs = d5 ? 16 : 8;
        if (Lv.size() < static_cast<size_t>(bw) * bh * bs) return false;
        Sums a;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* b = L + (static_cast<size_t>(by) * bw + bx) * bs;
                float al[8] = {1, 1, 1, 1, 1, 1, 1, 1};
                uint8_t aidx[16] = {};
                if (d5) {
                    const float a0 = b[0], a1 = b[1];
                    if (a0 > a1) for (int k = 0; k < 6; ++k) al[2 + k] = ((6 - k) * a0 + (1 + k) * a1) / 7.0f;
                    else { for (int k = 0; k < 4; ++k) al[2 + k] = ((4 - k) * a0 + (1 + k) * a1) / 5.0f; al[6] = 0.0f; al[7] = 255.0f; }
                    al[0] = a0; al[1] = a1;
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
                    for (int k = 0; k < 16; ++k) aidx[k] = static_cast<uint8_t>((bits >> (3 * k)) & 7);
                    for (float& x : al) x *= 1.0f / 255.0f;
                    b += 8;
                }
                const uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
                auto col = [](uint32_t c, float o[3]) { o[0] = ((c >> 11) & 31) / 31.0f; o[1] = ((c >> 5) & 63) / 63.0f; o[2] = (c & 31) / 31.0f; };
                float pal[4][3];
                col(c0, pal[0]); col(c1, pal[1]);
                const bool four = d5 || c0 > c1;
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3.0f : (pal[0][k] + pal[1][k]) * 0.5f;
                    pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3.0f : 0.0f;
                }
                float pm[4];
                for (int k = 0; k < 4; ++k) pm[k] = std::max(pal[k][0], std::max(pal[k][1], pal[k][2]));
                if (!d5) {
                    // DXT1: a block's 16 values are one of four numbers (the palette's largest channel; the fourth is 0 in the three-colour mode), so its sums come from how many
                    // texels use each code: the 2-bit fields of the four index bytes, counted with a popcount per code.
                    const uint32_t word = b[4] | (static_cast<uint32_t>(b[5]) << 8) | (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
                    double s = 0, q = 0;
                    for (uint32_t k = 0; k < 4; ++k) {
                        const uint32_t x = word ^ (k * 0x55555555u);
                        const uint32_t hit = ~(x | (x >> 1)) & 0x55555555u;
                        const double cnt = static_cast<double>(__builtin_popcount(hit));
                        const double val = (!four && k == 3) ? 0.0 : static_cast<double>(pm[k]);
                        s += cnt * val;
                        q += cnt * val * val;
                    }
                    a.S += s; a.Q += q; a.R += q - s * s * (1.0 / 16.0);
                    continue;
                }
                for (int py = 0; py < 4; ++py)
                    for (int px = 0; px < 4; ++px) {
                        const int code = (b[4 + py] >> (2 * px)) & 3;
                        const float alpha = d5 ? al[aidx[py * 4 + px]] : ((!four && code == 3) ? 0.0f : 1.0f);
                        v[py * 4 + px] = pm[code] * alpha;
                    }
                a.Block(v);
            }
        *soft = Verdict(static_cast<double>(n), a.S, a.Q, a.R);
        return true;
    }
    default: return false;
    }
}

} // namespace vgx::texsoft
