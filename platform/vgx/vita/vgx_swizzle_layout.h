// gxm23: the memory plan of a texture uploaded SWIZZLED (GXV_TEX_SWIZZLE; vgx_host_galaxy_vita.cpp vgxvita_uploadSwizzled).
// Header-only and free of sceGxm so that tests/vgx_swizzle_layout.cpp can check it on the host.
//
// The texels are first laid out exactly as vitaGL lays out a linear texture and its generated levels (utils/gpu_utils.c:
// gpu_alloc_texture's rows of VGL_ALIGN(w, 8) texels, gpu_alloc_mipmaps' level offsets MAX(w, 8) * h * bpp and its chain of
// log2(min(w, h)) levels, made by sceGxmTransferDownscale from each level to the next with those strides), so the levels the GPU makes
// are the ones vitaGL would make. Each level is then copied by the transfer unit, bits unchanged (RAW16 / RAW32), into a swizzled
// chain: level l is (w >> l) x (h >> l) texels, packed one after the other with no padding (the layout sceGxm samples for
// SCE_GXM_TEXTURE_SWIZZLED with a mip count: Vita3K renderer/src/texture/cache.cpp, mip_size = layout w * h * bpp for swizzled types).
#pragma once
#include <cstddef>
#include <cstdint>

namespace vgx {
namespace swz {

constexpr unsigned kMaxLevels = 11; // 1024 -> 1

inline bool IsPow2(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }
inline uint32_t Log2(uint32_t v) { uint32_t r = 0; while (v > 1u) { v >>= 1; ++r; } return r; }
inline uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

struct Plan {
    unsigned levels = 0;                 // 0: not eligible (the caller keeps vitaGL's linear upload)
    uint32_t w[kMaxLevels] = {}, h[kMaxLevels] = {};
    uint32_t linOff[kMaxLevels] = {};    // byte offset of level l in the linear chain (vitaGL's)
    uint32_t linStride[kMaxLevels] = {}; // its row stride in bytes: VGL_ALIGN(w, 8) * bpp
    uint32_t swzOff[kMaxLevels] = {};    // byte offset of level l in the swizzled chain
    uint32_t linBytes = 0, swzBytes = 0;
};

// width x height (powers of two, 8..1024 each), bpp 1 (single-level TF_I8 CPU swizzle), 2, or 4, genMips: the chain vitaGL's glGenerateMipmap makes.
inline Plan MakePlan(uint32_t width, uint32_t height, uint32_t bpp, bool genMips) {
    Plan p;
    if (!IsPow2(width) || !IsPow2(height) || width < 8 || height < 8 || width > 1024 || height > 1024 || (bpp != 1 && bpp != 2 && bpp != 4)) return p;
    if (bpp == 1 && genMips) return p; // TF_I8 is single-level only (no U8 transfer downscale)
    // gpu_alloc_mipmaps(-1): `while (w > 1 && h > 1) { jumps[mips] = MAX(w, 8) * h * bpp; w /= 2; h /= 2; mips++; }` -> mip_count = mips
    const unsigned n = genMips ? Log2(width < height ? width : height) : 1u;
    if (n == 0 || n > kMaxLevels) return p;
    uint32_t lo = 0, so = 0;
    for (unsigned l = 0; l < n; ++l) {
        const uint32_t w = width >> l, h = height >> l;
        p.w[l] = w;
        p.h[l] = h;
        p.linOff[l] = lo;
        p.linStride[l] = Align8(w) * bpp;
        p.swzOff[l] = so;
        lo += (w > 8u ? w : 8u) * h * bpp;
        so += w * h * bpp;
    }
    p.levels = n;
    p.linBytes = lo;
    p.swzBytes = so;
    return p;
}

inline uint32_t Part1By1(uint32_t v) {
    v &= 0x0000FFFFu;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

// Texel index (times bpp: the byte offset) of texel (x, y) inside a swizzled level of w x h texels (powers of two): Morton order over the min(w, h) square, the
// squares of a non-square level following each other along its longer side (Vita3K renderer/src/texture/format.cpp,
// swizzled_texture_to_linear_texture, the inverse).
inline uint32_t SwizzledIndex(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t m = w < h ? w : h;
    const uint32_t k = Log2(m);
    const uint32_t code = Part1By1(y & (m - 1u)) | (Part1By1(x & (m - 1u)) << 1); // (y on the even bits, x on the odd ones)
    const uint32_t upper = (w >= h ? x : y) >> k;
    return code | (upper << (2u * k));
}

// CPU Morton swizzle for 1-byte U8_RRRR (TF_I8, single level, powers of two 8..1024).
// libSceGxm has no 8-bit transfer-unit copy format (RAW16 minimum), and SGX543 requires
// SCE_GXM_TEXTURE_SWIZZLED for MIRROR wrap (e.g. BlackHole.bdl IntTex_QuarterHalo 64x64 I8).
// Processes 2x2 texel blocks (4 contiguous Morton bytes: (x,y), (x,y+1), (x+1,y), (x+1,y+1))
// into cached RAM (`dst`) before sequential VRAM copy (~4x faster than per-texel stores).
inline void SwizzleU8(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h) {
    if (!IsPow2(w) || !IsPow2(h) || w < 2u || h < 2u || w > 1024u || h > 1024u) return;
    const uint32_t m = w < h ? w : h;
    const uint32_t k = Log2(m);
    const uint32_t mask = m - 1u;
    const uint32_t shift = 2u * k;
    const uint32_t hw = w >> 1;
    uint32_t xLut[512];
    for (uint32_t xi = 0; xi < hw; ++xi) {
        const uint32_t x = xi << 1;
        xLut[xi] = (Part1By1(x & mask) << 1) | (w >= h ? ((x >> k) << shift) : 0u);
    }
    for (uint32_t y = 0; y < h; y += 2u) {
        const uint32_t yBits = Part1By1(y & mask) | (w < h ? ((y >> k) << shift) : 0u);
        const uint8_t* r0 = src + static_cast<size_t>(y) * w;
        const uint8_t* r1 = r0 + w;
        for (uint32_t xi = 0; xi < hw; ++xi) {
            const uint32_t x = xi << 1;
            uint8_t* o = dst + (yBits | xLut[xi]);
            o[0] = r0[x];
            o[1] = r1[x];
            o[2] = r0[x + 1u];
            o[3] = r1[x + 1u];
        }
    }
}

} // namespace swz
} // namespace vgx
