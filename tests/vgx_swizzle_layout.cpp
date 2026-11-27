// gxm23: host test of platform/vgx/vita/vgx_swizzle_layout.h (GXV_TEX_SWIZZLE's memory plan).
//   g++ -std=c++17 -O1 -Iplatform/vgx/vita tests/vgx_swizzle_layout.cpp -o /tmp/swz && /tmp/swz
// Checks, for every power-of-two size 8..1024 x 8..1024, bpp 2 and 4, with and without generated levels:
//   1. the linear plan is vitaGL's: gpu_alloc_mipmaps(-1)'s level count and jumps (offsets), gpu_alloc_texture's row stride
//      (transcribed from vgl/work/src21/utils/gpu_utils.c);
//   2. the swizzled plan is the layout Vita3K reads for SCE_GXM_TEXTURE_SWIZZLED (renderer/src/texture/cache.cpp: align 1x1 for a
//      non-block format, mip_size = layout_w * layout_h * bpp, layout halves per level);
//   3. SwizzledIndex is a bijection on every level and the exact inverse of Vita3K's swizzled_texture_to_linear_texture
//      (renderer/src/texture/format.cpp, transcribed).
#include "vgx_swizzle_layout.h"

#include <cstdio>
#include <vector>

using namespace vgx::swz;

namespace {
int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { if (++g_fail <= 20) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

// vitaGL gpu_alloc_mipmaps(-1, tex) for a w x h texture of bpp bytes: the level count it sets and its jumps.
unsigned VitaglChain(uint32_t w, uint32_t h, uint32_t bpp, uint32_t* jumps) {
    int mips = 0;
    while ((w > 1) && (h > 1)) {
        jumps[mips] = (w > 8 ? w : 8) * h * bpp;
        w /= 2;
        h /= 2;
        mips++;
    }
    jumps[mips] = (w > 8 ? w : 8) * h * bpp;
    return static_cast<unsigned>(mips); // level = mips -> tex->mip_count = level
}

// Vita3K: compact_one_by_one / decode_morton2_{x,y}
uint32_t Compact1By1(uint32_t x) {
    x &= 0x55555555;
    x = (x ^ (x >> 1)) & 0x33333333;
    x = (x ^ (x >> 2)) & 0x0f0f0f0f;
    x = (x ^ (x >> 4)) & 0x00ff00ff;
    x = (x ^ (x >> 8)) & 0x0000ffff;
    return x;
}
void V3kDecode(uint32_t i, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
    const uint32_t min = width < height ? width : height;
    uint32_t k = 0;
    while ((1u << (k + 1)) <= min) ++k; // std::bit_width(min) - 1
    x = Compact1By1(i >> 1) & (min - 1);
    y = Compact1By1(i >> 0) & (min - 1);
    const uint32_t upper = (i >> (2 * k)) << k;
    if (width >= height) x |= upper;
    else y |= upper;
}
} // namespace

int main() {
    unsigned plans = 0, texels = 0;
    for (uint32_t w = 8; w <= 1024; w *= 2)
        for (uint32_t h = 8; h <= 1024; h *= 2)
            for (uint32_t bpp : {2u, 4u})
                for (bool gen : {false, true}) {
                    const Plan p = MakePlan(w, h, bpp, gen);
                    CHECK(p.levels > 0, "%ux%u not eligible", w, h);
                    if (!p.levels) continue;
                    ++plans;
                    // 1. vitaGL's linear chain
                    uint32_t jumps[16] = {};
                    const unsigned vl = VitaglChain(w, h, bpp, jumps);
                    CHECK(p.levels == (gen ? vl : 1u), "%ux%u bpp %u: %u levels, vitaGL %u", w, h, bpp, p.levels, vl);
                    uint32_t off = 0;
                    for (unsigned l = 0; l < p.levels; ++l) {
                        CHECK(p.linOff[l] == off, "%ux%u level %u linear offset %u, vitaGL %u", w, h, l, p.linOff[l], off);
                        const uint32_t lw = w >> l;
                        CHECK(p.linStride[l] == ((lw + 7u) & ~7u) * bpp, "stride");
                        off += jumps[l];
                    }
                    CHECK(p.linBytes == off, "linear size %u, vitaGL %u", p.linBytes, off);
                    // 2. Vita3K's swizzled chain
                    uint32_t lw = w, lh = h, so = 0;
                    for (unsigned l = 0; l < p.levels; ++l) {
                        CHECK(p.w[l] == lw && p.h[l] == lh, "level size");
                        CHECK(p.swzOff[l] == so, "%ux%u level %u swizzled offset %u, Vita3K %u", w, h, l, p.swzOff[l], so);
                        so += lw * lh * bpp;
                        lw /= 2;
                        lh /= 2;
                    }
                    CHECK(p.swzBytes == so && p.swzBytes <= p.linBytes, "swizzled size");
                }
    // 3. the texel order
    for (uint32_t w = 1; w <= 256; w *= 2)
        for (uint32_t h = 1; h <= 256; h *= 2) {
            std::vector<uint8_t> seen(static_cast<size_t>(w) * h, 0);
            for (uint32_t i = 0; i < w * h; ++i) {
                uint32_t x, y;
                V3kDecode(i, w, h, x, y);
                CHECK(x < w && y < h, "decode out of range");
                if (x >= w || y >= h) continue;
                CHECK(SwizzledIndex(x, y, w, h) == i, "%ux%u: texel (%u,%u) at %u, Vita3K %u", w, h, x, y, SwizzledIndex(x, y, w, h), i);
                ++seen[static_cast<size_t>(y) * w + x];
                ++texels;
            }
            for (uint8_t s : seen) CHECK(s == 1, "%ux%u: not a bijection", w, h);
        }
    // 3b. bpp == 1 (TF_I8 single-level CPU Morton swizzle): MakePlan accepts genMips=false, rejects genMips=true,
    //     and SwizzleU8 matches SwizzledIndex / V3kDecode bit-for-bit across all square and rectangular POT sizes.
    for (uint32_t w = 8; w <= 1024; w *= 2)
        for (uint32_t h = 8; h <= 1024; h *= 2) {
            const Plan p1 = MakePlan(w, h, 1u, false);
            CHECK(p1.levels == 1 && p1.swzBytes == w * h && p1.linBytes == w * h && p1.linStride[0] == w, "bpp1 plan %ux%u", w, h);
            CHECK(MakePlan(w, h, 1u, true).levels == 0, "bpp1 genMips must be rejected %ux%u", w, h);
            if ((w <= 256 && h <= 256) || (w == 1024 && h == 8) || (w == 8 && h == 1024) || (w == 512 && h == 512) || (w == 1024 && h == 1024)) {
                std::vector<uint8_t> src(static_cast<size_t>(w) * h), dst(static_cast<size_t>(w) * h, 0);
                for (uint32_t i = 0; i < w * h; ++i) src[i] = static_cast<uint8_t>((i * 131u + 17u) ^ (i >> 8));
                SwizzleU8(src.data(), dst.data(), w, h);
                for (uint32_t i = 0; i < w * h; ++i) {
                    uint32_t x, y;
                    V3kDecode(i, w, h, x, y);
                    CHECK(dst[i] == src[static_cast<size_t>(y) * w + x], "SwizzleU8 %ux%u at %u (%u,%u)", w, h, i, x, y);
                }
            }
        }
    // not eligible: non power of two, too small, too big, bad bpp
    CHECK(!MakePlan(96, 64, 4, false).levels && !MakePlan(4, 64, 4, true).levels && !MakePlan(2048, 64, 4, false).levels
              && !MakePlan(64, 64, 3, false).levels, "eligibility");
    std::printf("vgx_swizzle_layout: %u plans, %u texels checked, %d failures: %s\n", plans, texels, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
