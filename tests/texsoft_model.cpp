// texup27 host test: vgx_texsoft.h's one-pass softness verdict against the old two-stage code (copied verbatim from vgx_backend_gl.cpp fxl::TexScalar / FieldIsSoft).
// bash tools/test_texsoft.sh
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <algorithm>
#include "../platform/vgx/src/vgx_texsoft.h"
using namespace vgx;
namespace legacy {
bool TexScalar(const TexUpload& u, std::vector<float>& f) {
    const int w = u.width, h = u.height;
    if (w < 8 || h < 8 || w > 256 || h > 256 || u.levels.empty()) return false;
    const std::vector<uint8_t>& L = u.levels[0];
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    f.assign(n, 0.0f);
    auto mx = [](float r, float g, float b) { return std::max(r, std::max(g, b)); };
    switch (u.format) {
    case TF_RGBA8:
        if (L.size() < n * 4) return false;
        for (size_t i = 0; i < n; ++i) f[i] = mx(L[i * 4], L[i * 4 + 1], L[i * 4 + 2]) * L[i * 4 + 3] * (1.0f / 65025.0f);
        return true;
    case TF_LA88:
        if (L.size() < n * 2) return false;
        for (size_t i = 0; i < n; ++i) f[i] = L[i * 2] * L[i * 2 + 1] * (1.0f / 65025.0f);
        return true;
    case TF_I8:
        if (L.size() < n) return false;
        for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(L[i]) * L[i] * (1.0f / 65025.0f);
        return true;
    case TF_RGB565: case TF_RGBA5551: case TF_RGBA4444: {
        if (L.size() < n * 2) return false;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = L[i * 2] | (static_cast<uint32_t>(L[i * 2 + 1]) << 8);
            float r, g, b, a = 1.0f;
            if (u.format == TF_RGB565) { r = ((v >> 11) & 31) / 31.0f; g = ((v >> 5) & 63) / 63.0f; b = (v & 31) / 31.0f; }
            else if (u.format == TF_RGBA5551) { r = ((v >> 11) & 31) / 31.0f; g = ((v >> 6) & 31) / 31.0f; b = ((v >> 1) & 31) / 31.0f; a = static_cast<float>(v & 1); }
            else { r = ((v >> 12) & 15) / 15.0f; g = ((v >> 8) & 15) / 15.0f; b = ((v >> 4) & 15) / 15.0f; a = (v & 15) / 15.0f; }
            f[i] = mx(r, g, b) * a;
        }
        return true;
    }
    case TF_DXT1: case TF_DXT5: {
        const bool d5 = u.format == TF_DXT5;
        const int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = d5 ? 16 : 8;
        if (L.size() < static_cast<size_t>(bw) * bh * bs) return false;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const uint8_t* b = L.data() + (static_cast<size_t>(by) * bw + bx) * bs;
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
                    for (float& v : al) v *= 1.0f / 255.0f;
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
                for (int py = 0; py < 4; ++py)
                    for (int px = 0; px < 4; ++px) {
                        const int x = bx * 4 + px, y = by * 4 + py;
                        if (x >= w || y >= h) continue;
                        const int code = (b[4 + py] >> (2 * px)) & 3;
                        const float a = d5 ? al[aidx[py * 4 + px]] : ((!four && code == 3) ? 0.0f : 1.0f);
                        f[static_cast<size_t>(y) * w + x] = mx(pal[code][0], pal[code][1], pal[code][2]) * a;
                    }
            }
        return true;
    }
    default: return false;
    }
}
bool FieldIsSoft(const std::vector<float>& f, int w, int h) {
    double sum = 0.0;
    for (float v : f) sum += v;
    const double n = static_cast<double>(f.size()), mean = sum / n;
    double var = 0.0;
    for (float v : f) var += (v - mean) * (v - mean);
    if (var <= 1.0e-4 * n) return true; // (flat)
    double resid = 0.0;
    for (int by = 0; by + 4 <= h; by += 4)
        for (int bx = 0; bx + 4 <= w; bx += 4) {
            double m = 0.0;
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) m += f[static_cast<size_t>(by + y) * w + bx + x];
            m *= 1.0 / 16.0;
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) { const double e = f[static_cast<size_t>(by + y) * w + bx + x] - m; resid += e * e; }
        }
    return resid <= 0.10 * var;
}
}
static int g_fail = 0, g_cases = 0, g_soft = 0;
static bool Old(const TexUpload& u, bool* soft) {
    std::vector<float> f;
    if (!legacy::TexScalar(u, f)) return false;
    *soft = legacy::FieldIsSoft(f, u.width, u.height);
    return true;
}
static size_t Bytes(uint8_t fmt, int w, int h) {
    switch (fmt) { case TF_RGBA8: return size_t(w) * h * 4; case TF_I8: return size_t(w) * h; case TF_DXT1: return size_t(w / 4) * (h / 4) * 8; case TF_DXT5: return size_t(w / 4) * (h / 4) * 16; default: return size_t(w) * h * 2; }
}
int main() {
    std::mt19937 rng(12345);
    const uint8_t fmts[] = {TF_RGBA8, TF_DXT1, TF_LA88, TF_DXT5, TF_RGB565, TF_RGBA5551, TF_RGBA4444, TF_I8};
    const int sizes[] = {8, 16, 32, 64, 128, 256};
    for (int iter = 0; iter < 4000; ++iter) {
        TexUpload u;
        u.format = fmts[rng() % 8];
        u.width = sizes[rng() % 6];
        u.height = sizes[rng() % 6];
        u.levels.resize(1);
        u.levels[0].resize(Bytes(u.format, u.width, u.height));
        const int kind = rng() % 5; // 0 noise, 1 flat, 2 smooth gradient, 3 gradient plus small noise, 4 sparse
        for (size_t i = 0; i < u.levels[0].size(); ++i) {
            const int x = int(i % 251);
            uint8_t b = 0;
            switch (kind) {
            case 0: b = uint8_t(rng()); break;
            case 1: b = 77; break;
            case 2: b = uint8_t(i / 16); break;
            case 3: b = uint8_t(i / 24 + (rng() % 5)); break;
            default: b = (rng() % 23 == 0) ? uint8_t(rng()) : 0; break;
            }
            u.levels[0][i] = b ^ uint8_t(x & (kind == 1 ? 0 : 0));
        }
        bool so = false, sn = false;
        const bool ho = Old(u, &so), hn = texsoft::Soft(u, &sn);
        ++g_cases;
        if (!ho || !hn) { ++g_fail; std::printf("FAIL handled old %d new %d fmt %d %dx%d\n", ho, hn, u.format, u.width, u.height); continue; }
        g_soft += so;
        if (so != sn) { ++g_fail; std::printf("FAIL verdict old %d new %d fmt %d %dx%d kind %d\n", so, sn, u.format, u.width, u.height, kind); }
    }
    // not handled: non multiple of 4, too short, out of range
    TexUpload u; u.format = TF_RGBA8; u.width = 10; u.height = 8; u.levels.assign(1, std::vector<uint8_t>(320, 1));
    bool s = false;
    if (texsoft::Soft(u, &s)) { ++g_fail; std::printf("FAIL 10x8 must fall back\n"); }
    u.width = 16; u.height = 16; u.levels[0].resize(100);
    if (texsoft::Soft(u, &s)) { ++g_fail; std::printf("FAIL short level must not be handled\n"); }
    u.width = 512; u.height = 512; u.levels[0].resize(512 * 512 * 4);
    if (texsoft::Soft(u, &s)) { ++g_fail; std::printf("FAIL 512 must not be handled\n"); }
    std::printf("texsoft: %d cases, %d soft, %d failures\n", g_cases, g_soft, g_fail);
    return g_fail ? 1 : 0;
}
