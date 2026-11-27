// GXV_TEX_COMPRESS (texcomp23, 8 Nov 2026): texture bandwidth. Large uncompressed colour textures become DXT1 (opaque) or DXT5 (with
// alpha) a few pictures after their upload, converted off the game's threads and cached on the memory card.
//
// WHY. Most of Galaxy's world textures are CMPR and already reach the GPU as DXT1 (4 bits a texel). The others are decoded to RGBA8
// (32 bits a texel): GX RGBA8 / RGB5A3 / RGB565 / palette (C4, C8, C14X2) textures, and CMPR textures with transparent texels (dark
// fringes as DXT1). Particles, effects, some foliage and object skins are among them; in Honeyhive the console reports ~260 of them,
// ~10 MiB ("tex-swizzle:" lines). The SGX samples DXT1 / DXT5 natively (UBC1 / UBC3): 1/8 and 1/4 of the bytes of RGBA8 for every
// texel fetched, so a texture-cache miss moves 8x / 4x less memory.
//
// WHAT IS CONVERTED (a texture must pass all of these; the census and the log count every reason):
//   - it took the RGBA8 path of the texture cache (vgx_texcache.cpp: RGBA8 level 0, with or without generated levels), its GX format is
//     not I4 / I8 / IA4 / IA8 (those have the exact LA88 layouts: GXV_LA88, GXV_LA88_MIPS), not an HD replacement, not patched by the
//     host (button pictures), not the authored I4 chain;
//   - both sides powers of two, each >= 8, at least GXV_TEX_COMPRESS_MIN texels (default 4096 = 64x64): small textures cost little
//     bandwidth and are the likeliest to be UI pieces;
//   - first upload of its cache entry (a texture whose bytes change after it was first seen is animated in memory: never);
//   - the back end has drawn it with a perspective projection and never with an orthographic one (layouts, fonts, the HUD, screen
//     effects: 2D) and never as an indirect map (water / heat-haze offsets, whose bits are coordinates, not colours). EFB copies and
//     render targets never come here (they are not in the texture cache);
//   - the encoded level 0 is within the quality floor: RGB PSNR >= GXV_TEX_COMPRESS_PSNR (default 32 dB) and alpha PSNR >= 36 dB.
//     A texture that fails keeps its RGBA8 bytes (and the card cache remembers the verdict).
// HOW (no work on the game thread, and only a copy of level 0 on the front end):
//   1. The front end uploads the texture exactly as before (RGBA8, swizzled by GXV_TEX_SWIZZLE): the first pictures look as they always
//      did and the load takes no longer. If eligible so far, Offer() queues a job: the cache key (the entry's content hash, palette hash,
//      GX format, size, mip flag, encoder version, quality floor) and, unless the card cache already has that key, a copy of level 0.
//   2. A worker thread (lowest priority on cores 1-2, the fourth core when it is proven; port_vita_thread_class / _core3 part 4, as the
//      asynchronous shader compiler) reads the card cache, or encodes (box-filtered levels down to 4x4 when the texture has levels;
//      principal-axis endpoints + two least-squares refinements per block; optimal single-colour blocks), checks the floor, and appends
//      the result to the card cache.
//   3. At the end of a front-end frame (TexCacheEndFrame) the finished results are offered back: if the cache entry still holds the same
//      bytes under the same id, and the back end's usage marks allow it, the texture is uploaded again as DXT1 / DXT5 under the same id
//      (a few a frame). Nothing else changes: same id, same sampler state, same filters.
// CARD CACHE: <GXV_TEX_COMPRESS_CACHE>.idx (40-byte records) + .dat (payloads), default ux0:data/smg/texcomp on the Vita (none on other
//   hosts unless set; "0" turns it off). Append-only; every payload carries a hash checked before use; a torn last record is ignored; it
//   stops growing at GXV_TEX_COMPRESS_CACHE_MB (default 48). Delete both files to rebuild. Version 1 of the encoder is in the key.
// PICTURE: a MINOR CHANGE. DXT is lossy: colours of a 4x4 block lie on one line through RGB space (565 endpoints). Binary alpha (cut-out
//   foliage) is exact, smooth alpha has 8 steps a block. The level chain stops at 4x4 (vitaGL's went to 1x1): the most distant pixels
//   sample a 4x4 level instead of a smaller one. Whatever a texture's verdict, its first pictures are the old ones.
// SWITCHES (vita/vgx-game/entry.cpp sets the default 0):
//   GXV_TEX_COMPRESS=0|1|perf        off (default) / on in every mode / on in Performance mode only
//   GXV_TEX_COMPRESS_IA=1            also I4 / I8 / IA4 / IA8 textures (default 0: they keep their exact LA88 / RGBA8 layouts). As (L, L, L, A)
//                                    they become DXT5 (DXT1 when A is 255 everywhere): 1 byte a texel instead of 2 (LA88) or 4 (mipmapped:
//                                    RGBA8 unless GXV_LA88_MIPS). Indirect maps (mostly I8) stay out by the usage rule.
//   GXV_TEX_COMPRESS_MIN=<texels>    smallest texture converted (default 4096)
//   GXV_TEX_COMPRESS_PSNR=<dB>       quality floor for RGB (default 32; alpha uses this + 4)
//   GXV_TEX_COMPRESS_CACHE=<path>    card cache files' stem (default ux0:data/smg/texcomp on the Vita; 0 = no card cache)
//   GXV_TEX_COMPRESS_CACHE_MB=<MiB>  card cache size limit (default 48)
//   GXV_TEX_COMPRESS_INFLIGHT_MB=<MiB> level-0 copies the worker may hold at once (default 8; beyond it a texture is left for its next
//                                    load: "cap" in the log)
//   GXV_TEX_COMPRESS=ceiling         DIAGNOSTIC, WRONG PICTURE: the same textures, by the same rules, become one 4x4 DXT block of their
//                                    average colours instead (no card cache, no floor): their texture reads all but vanish. The 3D time it
//                                    saves against =0 is the most any conversion of these textures can save (the console A/B's ceiling).
//   GXV_TEXCOMP_CENSUS=1             diagnostic, independent of the switch: the "tex-census:" lines (texture bytes read per picture,
//                                    every GXV_TEXCOMP_CENSUS_S seconds, default 5)
// LOG (basic log, at most every 30 s after a change): "tex-compress: GXV_TEX_COMPRESS=<m>: N textures converted (DXT1 a, DXT5 b),
//   X MiB -> Y MiB (Z MiB saved); card cache h hits (r ms read), e encoded (E ms, max M ms, P Mtexel/s), q under the quality floor,
//   W MiB written; waiting for a 3D use w; left as they were: 2D u, indirect i, stale s, range g; not offered: I/IA .., small .., ...".
#include "vgx_automip.h"
#include "vgx_texcomp.h"
#include "vgx_cmd.h"
#include "vgx_common.h"
#include "vgx_texdecode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" int port_perf_mode_60(void) __attribute__((weak));
extern "C" int port_vita_thread_class(int cls) __attribute__((weak));
extern "C" int port_vita_thread_core3(int part, int planned) __attribute__((weak));

namespace vgx::texcomp {

// ================================================================ the encoder
namespace {
constexpr uint32_t kEncoderVersion = 1;

inline int Expand5(int v) { return (v << 3) | (v >> 2); }
inline int Expand6(int v) { return (v << 2) | (v >> 4); }
inline void Unpack565(uint32_t c, int o[3]) { o[0] = Expand5((c >> 11) & 31); o[1] = Expand6((c >> 5) & 63); o[2] = Expand5(c & 31); }
inline uint32_t Pack565(int r5, int g6, int b5) { return static_cast<uint32_t>((r5 << 11) | (g6 << 5) | b5); }
inline int Q5(float v) { const int q = static_cast<int>(v * (31.0f / 255.0f) + 0.5f); return q < 0 ? 0 : q > 31 ? 31 : q; }
inline int Q6(float v) { const int q = static_cast<int>(v * (63.0f / 255.0f) + 0.5f); return q < 0 ? 0 : q > 63 ? 63 : q; }

// The four-colour palette (the same rounding as vgx_backend_gl.cpp DecodeDxt1: (2 a + b + 1) / 3).
inline void Palette4(uint32_t c0, uint32_t c1, int pal[4][3]) {
    Unpack565(c0, pal[0]);
    Unpack565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
    }
}
// Nearest palette entry per texel; returns the summed squared error.
inline uint32_t Assign(const uint8_t* px, uint32_t c0, uint32_t c1, uint8_t idx[16]) {
    int pal[4][3];
    Palette4(c0, c1, pal);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t* p = px + i * 4;
        uint32_t best = ~0u;
        uint8_t bi = 0;
        for (uint8_t e = 0; e < 4; ++e) {
            const int dr = p[0] - pal[e][0], dg = p[1] - pal[e][1], db = p[2] - pal[e][2];
            const uint32_t d = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
            if (d < best) { best = d; bi = e; }
        }
        idx[i] = bi;
        total += best;
    }
    return total;
}

// Single-colour blocks: for each 8-bit value the 5- / 6-bit endpoint pair whose 2/3 mix (palette entry 2) is closest.
struct OneColour { uint8_t e0, e1; };
struct OneColourTables {
    OneColour t5[256], t6[256];
    OneColourTables() {
        for (int v = 0; v < 256; ++v) {
            for (int bits = 5; bits <= 6; ++bits) {
                const int top = (1 << bits) - 1;
                int best = 1 << 30;
                OneColour bc{0, 0};
                for (int a = 0; a <= top; ++a)
                    for (int b = 0; b <= top; ++b) {
                        const int ea = bits == 5 ? Expand5(a) : Expand6(a), eb = bits == 5 ? Expand5(b) : Expand6(b);
                        const int mix = (2 * ea + eb + 1) / 3;
                        const int err = std::abs(mix - v) * 8 + (a == b ? 0 : 1); // (ties: the same endpoint twice)
                        if (err < best) { best = err; bc = OneColour{static_cast<uint8_t>(a), static_cast<uint8_t>(b)}; }
                    }
                (bits == 5 ? t5 : t6)[v] = bc;
            }
        }
    }
};
const OneColourTables& Tables() {
    static const OneColourTables t; // (built once, ~0.2 ms; first use is on the worker thread)
    return t;
}

inline void WriteColour(uint8_t out[8], uint32_t c0, uint32_t c1, const uint8_t idx[16]) {
    uint8_t ix[16];
    std::memcpy(ix, idx, 16);
    if (c0 < c1) { // four-colour mode needs c0 > c1: swap the endpoints, 0 <-> 1, 2 <-> 3
        std::swap(c0, c1);
        for (uint8_t& i : ix) i ^= 1;
    }
    if (c0 == c1) std::memset(ix, 0, 16); // (one colour: every decoder reads index 0 as c0)
    out[0] = static_cast<uint8_t>(c0); out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1); out[3] = static_cast<uint8_t>(c1 >> 8);
    for (int y = 0; y < 4; ++y)
        out[4 + y] = static_cast<uint8_t>(ix[y * 4] | (ix[y * 4 + 1] << 2) | (ix[y * 4 + 2] << 4) | (ix[y * 4 + 3] << 6));
}
} // namespace

void EncodeColorBlock(const uint8_t rgba[64], uint8_t out[8]) {
    // Solid colour: the best mix from the tables (exact for most values).
    bool solid = true;
    for (int i = 1; i < 16 && solid; ++i) solid = rgba[i * 4] == rgba[0] && rgba[i * 4 + 1] == rgba[1] && rgba[i * 4 + 2] == rgba[2];
    if (solid) {
        const OneColourTables& t = Tables();
        const OneColour r = t.t5[rgba[0]], g = t.t6[rgba[1]], b = t.t5[rgba[2]];
        uint32_t c0 = Pack565(r.e0, g.e0, b.e0), c1 = Pack565(r.e1, g.e1, b.e1);
        // (an exact endpoint beats the mix when both are equally close: keep the error check honest)
        uint8_t ia[16];
        const uint32_t eMix = Assign(rgba, c0, c1, ia);
        const uint32_t cq = Pack565(Q5(rgba[0]), Q6(rgba[1]), Q5(rgba[2]));
        uint8_t ib[16];
        const uint32_t eQ = Assign(rgba, cq, cq, ib);
        if (eQ <= eMix) { WriteColour(out, cq, cq, ib); return; }
        WriteColour(out, c0, c1, ia);
        return;
    }
    // Mean and covariance of the 16 colours.
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) mean[k] += rgba[i * 4 + k];
    for (float& m : mean) m *= 1.0f / 16.0f;
    float cov[6] = {0, 0, 0, 0, 0, 0}; // rr rg rb gg gb bb
    for (int i = 0; i < 16; ++i) {
        const float r = rgba[i * 4] - mean[0], g = rgba[i * 4 + 1] - mean[1], b = rgba[i * 4 + 2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    // Principal axis: power iteration from the largest diagonal's row.
    float ax[3];
    {
        const int start = cov[0] >= cov[3] && cov[0] >= cov[5] ? 0 : cov[3] >= cov[5] ? 1 : 2;
        const float row[3][3] = {{cov[0], cov[1], cov[2]}, {cov[1], cov[3], cov[4]}, {cov[2], cov[4], cov[5]}};
        ax[0] = row[start][0]; ax[1] = row[start][1]; ax[2] = row[start][2];
        for (int it = 0; it < 6; ++it) {
            const float n0 = row[0][0] * ax[0] + row[0][1] * ax[1] + row[0][2] * ax[2];
            const float n1 = row[1][0] * ax[0] + row[1][1] * ax[1] + row[1][2] * ax[2];
            const float n2 = row[2][0] * ax[0] + row[2][1] * ax[1] + row[2][2] * ax[2];
            const float m = std::max(std::fabs(n0), std::max(std::fabs(n1), std::fabs(n2)));
            if (m < 1e-6f) break;
            ax[0] = n0 / m; ax[1] = n1 / m; ax[2] = n2 / m;
        }
        if (std::fabs(ax[0]) + std::fabs(ax[1]) + std::fabs(ax[2]) < 1e-6f) { ax[0] = ax[1] = ax[2] = 1.0f; }
    }
    // Extremes along the axis: the endpoints (the texels themselves).
    float lo = 1e30f, hi = -1e30f;
    int iLo = 0, iHi = 0;
    for (int i = 0; i < 16; ++i) {
        const float d = (rgba[i * 4] - mean[0]) * ax[0] + (rgba[i * 4 + 1] - mean[1]) * ax[1] + (rgba[i * 4 + 2] - mean[2]) * ax[2];
        if (d < lo) { lo = d; iLo = i; }
        if (d > hi) { hi = d; iHi = i; }
    }
    const uint8_t* pHi = rgba + iHi * 4;
    const uint8_t* pLo = rgba + iLo * 4;
    uint32_t c0 = Pack565(Q5(pHi[0]), Q6(pHi[1]), Q5(pHi[2]));
    uint32_t c1 = Pack565(Q5(pLo[0]), Q6(pLo[1]), Q5(pLo[2]));
    uint8_t idx[16], tryIdx[16];
    uint32_t err = Assign(rgba, c0, c1, idx);
    // Candidate 2: the bounding box's corners, ordered along the axis.
    {
        int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min<int>(mn[k], rgba[i * 4 + k]); mx[k] = std::max<int>(mx[k], rgba[i * 4 + k]); }
        float a[3], b[3];
        for (int k = 0; k < 3; ++k) { a[k] = ax[k] >= 0 ? mx[k] : mn[k]; b[k] = ax[k] >= 0 ? mn[k] : mx[k]; }
        const uint32_t d0 = Pack565(Q5(a[0]), Q6(a[1]), Q5(a[2])), d1 = Pack565(Q5(b[0]), Q6(b[1]), Q5(b[2]));
        const uint32_t e2 = Assign(rgba, d0, d1, tryIdx);
        if (e2 < err) { err = e2; c0 = d0; c1 = d1; std::memcpy(idx, tryIdx, 16); }
    }
    // Least-squares refinement of the endpoints for the indices found (twice).
    static const float kW0[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
    for (int it = 0; it < 2 && err > 0; ++it) {
        float aa = 0, ab = 0, bb = 0, ax0[3] = {0, 0, 0}, bx0[3] = {0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            const float a = kW0[idx[i]], b = 1.0f - a;
            aa += a * a; ab += a * b; bb += b * b;
            for (int k = 0; k < 3; ++k) { ax0[k] += a * rgba[i * 4 + k]; bx0[k] += b * rgba[i * 4 + k]; }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-4f) break;
        const float inv = 1.0f / det;
        float e0[3], e1[3];
        for (int k = 0; k < 3; ++k) {
            e0[k] = (ax0[k] * bb - bx0[k] * ab) * inv;
            e1[k] = (bx0[k] * aa - ax0[k] * ab) * inv;
        }
        const uint32_t d0 = Pack565(Q5(e0[0]), Q6(e0[1]), Q5(e0[2])), d1 = Pack565(Q5(e1[0]), Q6(e1[1]), Q5(e1[2]));
        if (d0 == c0 && d1 == c1) break;
        const uint32_t e2 = Assign(rgba, d0, d1, tryIdx);
        if (e2 >= err) break;
        err = e2; c0 = d0; c1 = d1;
        std::memcpy(idx, tryIdx, 16);
    }
    WriteColour(out, c0, c1, idx);
}

namespace {
// The alpha palette as vgx_backend_gl.cpp DecodeDxt5 builds it.
inline void AlphaPalette(int a0, int a1, int pal[8]) {
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else { for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
}
inline uint32_t AlphaAssign(const uint8_t* a16, int a0, int a1, uint8_t idx[16]) {
    int pal[8];
    AlphaPalette(a0, a1, pal);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 1 << 30;
        uint8_t bi = 0;
        for (uint8_t e = 0; e < 8; ++e) {
            const int d = std::abs(a16[i] - pal[e]);
            if (d < best) { best = d; bi = e; }
        }
        idx[i] = bi;
        total += static_cast<uint32_t>(best * best);
    }
    return total;
}
} // namespace

void EncodeAlphaBlock(const uint8_t rgba[64], uint8_t out[8]) {
    uint8_t a[16];
    int mn = 255, mx = 0, mnIn = 255, mxIn = 0; // (In: leaving out 0 and 255)
    bool binary = true;
    for (int i = 0; i < 16; ++i) {
        a[i] = rgba[i * 4 + 3];
        mn = std::min<int>(mn, a[i]); mx = std::max<int>(mx, a[i]);
        if (a[i] != 0 && a[i] != 255) { mnIn = std::min<int>(mnIn, a[i]); mxIn = std::max<int>(mxIn, a[i]); binary = false; }
    }
    uint8_t idx[16], t[16];
    int a0, a1;
    if (mn == mx) { a0 = a1 = mn; std::memset(idx, 0, 16); }
    else if (binary) { a0 = 255; a1 = 0; for (int i = 0; i < 16; ++i) idx[i] = a[i] ? 0 : 1; } // (exact: palette entries 0 and 1)
    else {
        a0 = mx; a1 = mn; // 8-value mode
        uint32_t err = AlphaAssign(a, a0, a1, idx);
        // one round of nudging the endpoints by one step (they need not be texels)
        if (err) {
            int b0Best = a0, b1Best = a1;
            for (int d0 = -1; d0 <= 1; ++d0)
                for (int d1 = -1; d1 <= 1; ++d1) {
                    const int b0 = mx + d0, b1 = mn + d1;
                    if ((d0 == 0 && d1 == 0) || b0 > 255 || b1 < 0 || b0 <= b1) continue;
                    const uint32_t e = AlphaAssign(a, b0, b1, t);
                    if (e < err) { err = e; b0Best = b0; b1Best = b1; std::memcpy(idx, t, 16); }
                }
            a0 = b0Best; a1 = b1Best;
        }
        // 6-value mode with explicit 0 and 255 (a0 <= a1): better when the block has 0 or 255 beside a narrow middle range
        if ((mn == 0 || mx == 255) && mnIn <= mxIn) {
            const uint32_t e = AlphaAssign(a, mnIn, mxIn, t);
            if (e < err) { a0 = mnIn; a1 = mxIn; std::memcpy(idx, t, 16); }
        }
    }
    out[0] = static_cast<uint8_t>(a0);
    out[1] = static_cast<uint8_t>(a1);
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= static_cast<uint64_t>(idx[i] & 7) << (i * 3);
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<uint8_t>(bits >> (k * 8));
}

namespace {
inline void GatherBlock(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t bx, uint32_t by, uint8_t px[64]) {
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
            std::memcpy(px + (y * 4 + x) * 4, rgba + (static_cast<size_t>(sy) * w + sx) * 4, 4);
        }
}
} // namespace

void EncodeDxt1(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, dst += 8) {
            GatherBlock(rgba, w, h, bx, by, px);
            EncodeColorBlock(px, dst);
        }
}
void EncodeDxt5(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t px[64];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, dst += 16) {
            GatherBlock(rgba, w, h, bx, by, px);
            EncodeAlphaBlock(px, dst);
            EncodeColorBlock(px, dst + 8);
        }
}

void DecodeDxt(const uint8_t* src, uint32_t w, uint32_t h, bool dxt5, uint8_t* rgba) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = src + (static_cast<size_t>(by) * bw + bx) * (dxt5 ? 16 : 8);
            const uint8_t* c = dxt5 ? b + 8 : b;
            const uint32_t c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
            int pal[4][4];
            Unpack565(c0, pal[0]);
            Unpack565(c1, pal[1]);
            const bool four = dxt5 || c0 > c1;
            for (int k = 0; k < 3; ++k) {
                if (four) {
                    pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
                    pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
                } else {
                    pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
                    pal[3][k] = 0;
                }
            }
            pal[0][3] = pal[1][3] = pal[2][3] = 255;
            pal[3][3] = four ? 255 : 0;
            int apal[8] = {0};
            uint64_t abits = 0;
            if (dxt5) {
                AlphaPalette(b[0], b[1], apal);
                for (int k = 0; k < 6; ++k) abits |= static_cast<uint64_t>(b[2 + k]) << (k * 8);
            }
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    const int e = (c[4 + y] >> (x * 2)) & 3;
                    uint8_t* o = rgba + (static_cast<size_t>(py) * w + px) * 4;
                    o[0] = static_cast<uint8_t>(pal[e][0]); o[1] = static_cast<uint8_t>(pal[e][1]); o[2] = static_cast<uint8_t>(pal[e][2]);
                    o[3] = dxt5 ? static_cast<uint8_t>(apal[(abits >> ((y * 4 + x) * 3)) & 7]) : static_cast<uint8_t>(pal[e][3]);
                }
        }
}

namespace {
// What a GPU can return from these blocks, per channel: the endpoints exactly, an interpolated entry within one step of the spec's
// rounding (decoders differ there), only the entries some texel uses. For pix23's ranges, which must hold whatever the decoder.
void BlockRange(const uint8_t* src, uint32_t w, uint32_t h, bool dxt5, uint8_t lo[4], uint8_t hi[4]) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    auto put = [&](int c, int v, bool exact) {
        const int l = exact ? v : std::max(0, v - 1), u = exact ? v : std::min(255, v + 1);
        lo[c] = static_cast<uint8_t>(std::min<int>(lo[c], l));
        hi[c] = static_cast<uint8_t>(std::max<int>(hi[c], u));
    };
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = src + (static_cast<size_t>(by) * bw + bx) * (dxt5 ? 16 : 8);
            const uint8_t* c = dxt5 ? b + 8 : b;
            const uint32_t c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
            int pal[4][3];
            Palette4(c0, c1, pal);
            const bool four = dxt5 || c0 > c1;
            int apal[8] = {0};
            uint64_t abits = 0;
            if (dxt5) {
                AlphaPalette(b[0], b[1], apal);
                for (int k = 0; k < 6; ++k) abits |= static_cast<uint64_t>(b[2 + k]) << (k * 8);
            }
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    if (bx * 4 + x >= w || by * 4 + y >= h) continue;
                    const int e = (c[4 + y] >> (x * 2)) & 3;
                    if (!four && e >= 2) {
                        // (three-colour mode: never written for these textures; its middle entry and transparent black, conservatively)
                        for (int k = 0; k < 3; ++k) { put(k, (pal[0][k] + pal[1][k]) / 2, false); put(k, 0, true); }
                        put(3, 0, true);
                        put(3, 255, true);
                        continue;
                    }
                    for (int k = 0; k < 3; ++k) put(k, pal[e][k], e < 2);
                    if (dxt5) {
                        const int ai = static_cast<int>((abits >> ((y * 4 + x) * 3)) & 7);
                        const bool exact = ai < 2 || (b[0] <= b[1] && ai >= 6);
                        put(3, apal[ai], exact);
                    } else {
                        put(3, 255, true);
                    }
                }
        }
}
} // namespace

Quality Measure(const uint8_t* a, const uint8_t* b, size_t texels) {
    Quality q;
    double se = 0, sa = 0;
    for (size_t i = 0; i < texels; ++i) {
        for (int k = 0; k < 3; ++k) {
            const int d = std::abs(static_cast<int>(a[i * 4 + k]) - static_cast<int>(b[i * 4 + k]));
            se += d * d;
            q.maxErrRgb = std::max<uint32_t>(q.maxErrRgb, static_cast<uint32_t>(d));
        }
        const int d = std::abs(static_cast<int>(a[i * 4 + 3]) - static_cast<int>(b[i * 4 + 3]));
        sa += d * d;
        q.maxErrA = std::max<uint32_t>(q.maxErrA, static_cast<uint32_t>(d));
    }
    const auto psnr = [](double mse) { return mse <= 0.0 ? 99.0 : std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse)); };
    q.psnrRgb = texels ? psnr(se / (3.0 * static_cast<double>(texels))) : 99.0;
    q.psnrA = texels ? psnr(sa / static_cast<double>(texels)) : 99.0;
    return q;
}

bool EncodeTexture(const uint8_t* rgba, uint32_t w, uint32_t h, bool mips, Encoded& out) {
    out = Encoded{};
    if (w < 4 || h < 4 || (w & (w - 1)) || (h & (h - 1))) return false;
    const size_t n = static_cast<size_t>(w) * h;
    bool opaque = true;
    for (size_t i = 0; i < n && opaque; ++i) opaque = rgba[i * 4 + 3] == 255;
    out.format = opaque ? kFmtDxt1 : kFmtDxt5;
    const uint32_t blockBytes = opaque ? 8u : 16u;
    std::vector<uint8_t> cur, next, dec(n * 4);
    const uint8_t* lv = rgba;
    uint32_t lw = w, lh = h;
    uint8_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int level = 0;; ++level) {
        out.levels.emplace_back(static_cast<size_t>((lw + 3) / 4) * ((lh + 3) / 4) * blockBytes);
        if (opaque) EncodeDxt1(lv, lw, lh, out.levels.back().data());
        else EncodeDxt5(lv, lw, lh, out.levels.back().data());
        if (level == 0) {
            DecodeDxt(out.levels.back().data(), lw, lh, !opaque, dec.data());
            out.q = Measure(rgba, dec.data(), static_cast<size_t>(lw) * lh);
        }
        BlockRange(out.levels.back().data(), lw, lh, !opaque, lo, hi);
        if (!mips || lw / 2 < 4 || lh / 2 < 4) break;
        uint32_t nw = 0, nh = 0;
        Downsample(lv, lw, lh, next, nw, nh);
        cur.swap(next);
        lv = cur.data();
        lw = nw;
        lh = nh;
    }
    std::memcpy(out.lo, lo, 4);
    std::memcpy(out.hi, hi, 4);
    return true;
}

// ================================================================ the switch and its settings
int Mode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const char* e = std::getenv("GXV_TEX_COMPRESS");
        if (!e || !*e) return 0;
        if (*e == 'p' || *e == 'P') {
            if (port_perf_mode_60) return port_perf_mode_60() ? 1 : 0;
            // (hosts without the port layer: the same rule as platform/src/port/perf.cpp)
            const char* t = std::getenv("GXV_AUTOQ_TARGET");
            const char* f = std::getenv("GXV_FPS_LIMIT");
            return (t && std::atoi(t) == 60 && !(f && std::atoi(f) == 30)) ? 1 : 0;
        }
        if (*e == 'c' || *e == 'C') return 2; // ("ceiling": the diagnostic below)
        return std::atoi(e) > 0 ? 1 : 0;
    }()));
    return m;
}

namespace {
struct Settings {
    uint32_t minTexels = 4096;
    double psnr = 32.0;
    std::string cacheStem;
    uint64_t cacheMax = 48ull << 20;
    uint64_t inflightMax = 8ull << 20;
    const char* modeText = "0";
    bool ia = false;
    Settings() {
        ia = EnvInt("GXV_TEX_COMPRESS_IA", 0) != 0;
        minTexels = static_cast<uint32_t>(std::max(16L, EnvInt("GXV_TEX_COMPRESS_MIN", 4096)));
        if (const char* p = std::getenv("GXV_TEX_COMPRESS_PSNR"); p && *p) psnr = std::atof(p);
        if (const char* c = std::getenv("GXV_TEX_COMPRESS_CACHE"); c && *c) cacheStem = std::strcmp(c, "0") == 0 ? std::string() : std::string(c);
        else {
#if defined(__vita__) || defined(__VGX_VITA__)
            cacheStem = "ux0:data/smg/texcomp";
#endif
        }
        cacheMax = static_cast<uint64_t>(std::max(1L, EnvInt("GXV_TEX_COMPRESS_CACHE_MB", 48))) << 20;
        inflightMax = static_cast<uint64_t>(std::max(1L, EnvInt("GXV_TEX_COMPRESS_INFLIGHT_MB", 8))) << 20;
        if (const char* e = std::getenv("GXV_TEX_COMPRESS"); e && *e) modeText = e;
    }
};
const Settings& Cfg() {
    static const Settings s; // (front end's first offer; read-only afterwards)
    return s;
}

// ================================================================ the card cache
#pragma pack(push, 1)
struct IdxRec {
    uint64_t key;
    uint32_t off, size;     // into the .dat; size 0 for a "below the floor" verdict
    uint16_t w, h;
    uint8_t format, levels; // kFmtDxt1 / kFmtDxt5 / 0xFF (below the floor)
    uint8_t version, pad;
    uint32_t hash;          // low 32 bits of XXH3 of the payload
    uint8_t lo[4], hi[4];   // decoded range (pix23)
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(IdxRec) == 40, "card cache record");
constexpr uint8_t kRejected = 0xFF;

std::mutex& g_idxMtx = *new std::mutex; // the index (front end looks up, the worker adds; never destroyed)
std::unordered_map<uint64_t, IdxRec> g_index;
bool g_indexLoaded = false;
FILE* g_idxW = nullptr;  // (worker) append handles
FILE* g_datW = nullptr;
FILE* g_datR = nullptr;
uint64_t g_datSize = 0;
bool g_cacheBroken = false;

bool IndexLookup(uint64_t key, IdxRec& r) {
    std::lock_guard<std::mutex> l(g_idxMtx);
    auto it = g_index.find(key);
    if (it == g_index.end()) return false;
    r = it->second;
    return true;
}
// (worker) the index from the card, once
void CacheOpen() {
    if (g_indexLoaded) return;
    g_indexLoaded = true;
    const std::string& stem = Cfg().cacheStem;
    if (stem.empty()) return;
    const std::string idx = stem + ".idx", dat = stem + ".dat";
    if (FILE* f = std::fopen(dat.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        g_datSize = static_cast<uint64_t>(std::ftell(f));
        std::fclose(f);
    }
    std::vector<IdxRec> recs;
    if (FILE* f = std::fopen(idx.c_str(), "rb")) {
        IdxRec r;
        while (std::fread(&r, sizeof r, 1, f) == 1) recs.push_back(r); // (a torn last record is left out)
        std::fclose(f);
    }
    {
        std::lock_guard<std::mutex> l(g_idxMtx);
        for (const IdxRec& r : recs) {
            if (r.version != kEncoderVersion) continue;
            if (r.format != kRejected && (static_cast<uint64_t>(r.off) + r.size > g_datSize || !r.size)) continue;
            g_index[r.key] = r;
        }
    }
    g_idxW = std::fopen(idx.c_str(), "ab");
    g_datW = std::fopen(dat.c_str(), "ab");
    g_datR = std::fopen(dat.c_str(), "rb");
    if (!g_idxW || !g_datW) g_cacheBroken = true;
}
} // namespace

// ================================================================ jobs, results, the worker
namespace {
struct Job {
    uint32_t texId = 0;
    EntryRef ref;
    uint64_t contentHash = 0, key = 0;
    uint16_t w = 0, h = 0;
    bool mips = false;
    std::vector<uint8_t> rgba; // empty: a card-cache hit at the time of the offer
};
// (never destroyed: the worker waits on them for the life of the process, and destroying a condition variable that has a waiter at exit
// blocks on some hosts)
std::mutex& g_mtx = *new std::mutex;
std::condition_variable& g_cv = *new std::condition_variable;
std::condition_variable& g_cvIdle = *new std::condition_variable;
std::deque<Job> g_jobs;
std::vector<Result> g_done;
bool g_busy = false, g_started = false;
std::atomic<uint64_t> g_inflight{0};
// counters (worker: atomics; front end: plain, read by the log line on the same thread)
std::atomic<uint64_t> g_hits{0}, g_encoded{0}, g_rejected{0}, g_readUs{0}, g_encodeUs{0}, g_encodeMaxUs{0}, g_writeUs{0}, g_written{0},
    g_encodedTexels{0}, g_readFails{0};
Counters g_fe;            // (front end)
std::vector<Result> g_wait; // (front end) results waiting for their texture's first 3D draw
size_t g_waitNext = 0;
bool g_logDirty = false;

void WorkerPlace() {
    // background work: the lowest priority on cores 1-2, then the fourth core when it is proven and allowed (part 4, as the shader worker)
    if (port_vita_thread_class) port_vita_thread_class(4);
    if (port_vita_thread_core3) (void)port_vita_thread_core3(4, 0x60000);
}

bool ReadPayload(const IdxRec& r, Result& out) {
    if (!g_datR || r.format == kRejected) return false;
    std::vector<uint8_t> all(r.size);
    if (std::fseek(g_datR, static_cast<long>(r.off), SEEK_SET) != 0 || std::fread(all.data(), 1, all.size(), g_datR) != all.size()) return false;
    if (static_cast<uint32_t>(XXH3_64bits(all.data(), all.size())) != r.hash) return false;
    const uint32_t bb = r.format == kFmtDxt1 ? 8u : 16u;
    uint32_t lw = r.w, lh = r.h;
    size_t off = 0;
    out.levels.clear();
    for (uint32_t l = 0; l < r.levels; ++l) {
        const size_t n = static_cast<size_t>((lw + 3) / 4) * ((lh + 3) / 4) * bb;
        if (off + n > all.size()) return false;
        out.levels.emplace_back(all.begin() + static_cast<long>(off), all.begin() + static_cast<long>(off + n));
        off += n;
        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }
    if (off != all.size()) return false;
    out.format = r.format;
    std::memcpy(out.lo, r.lo, 4);
    std::memcpy(out.hi, r.hi, 4);
    return true;
}
void Append(uint64_t key, const Encoded* enc, uint16_t w, uint16_t h) {
    if (!g_idxW || !g_datW || g_cacheBroken) return;
    IdxRec r{};
    r.key = key;
    r.w = w;
    r.h = h;
    r.version = kEncoderVersion;
    std::vector<uint8_t> all;
    if (enc) {
        for (const auto& lv : enc->levels) all.insert(all.end(), lv.begin(), lv.end());
        if (g_datSize + all.size() > Cfg().cacheMax) return; // (full: no more entries)
        r.off = static_cast<uint32_t>(g_datSize);
        r.size = static_cast<uint32_t>(all.size());
        r.format = enc->format;
        r.levels = static_cast<uint8_t>(enc->levels.size());
        r.hash = static_cast<uint32_t>(XXH3_64bits(all.data(), all.size()));
        std::memcpy(r.lo, enc->lo, 4);
        std::memcpy(r.hi, enc->hi, 4);
        if (std::fwrite(all.data(), 1, all.size(), g_datW) != all.size() || std::fflush(g_datW) != 0) { g_cacheBroken = true; return; }
        g_datSize += all.size();
        g_written.fetch_add(all.size(), std::memory_order_relaxed);
    } else {
        r.format = kRejected;
    }
    // (the payload is on the card before its record: a record never points at bytes that are not there)
    if (std::fwrite(&r, sizeof r, 1, g_idxW) != 1 || std::fflush(g_idxW) != 0) { g_cacheBroken = true; return; }
    std::lock_guard<std::mutex> l(g_idxMtx);
    g_index[key] = r;
}

inline uint64_t NowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Run(Job& j) {
    Result r;
    r.texId = j.texId;
    r.ref = j.ref;
    r.contentHash = j.contentHash;
    r.w = j.w;
    r.h = j.h;
    if (Mode() == 2) {
        // (GXV_TEX_COMPRESS=ceiling) one 4x4 level of box averages: bandwidth gone, picture wrong
        if (j.rgba.empty()) return;
        std::vector<uint8_t> cur(j.rgba), next;
        uint32_t lw = j.w, lh = j.h;
        while (lw > 4 || lh > 4) {
            uint32_t nw = 0, nh = 0;
            Downsample(cur.data(), lw, lh, next, nw, nh);
            cur.swap(next);
            lw = nw;
            lh = nh;
        }
        Encoded enc;
        if (!EncodeTexture(cur.data(), lw, lh, false, enc)) return;
        r.w = static_cast<uint16_t>(lw);
        r.h = static_cast<uint16_t>(lh);
        r.format = enc.format;
        r.levels = std::move(enc.levels);
        std::memcpy(r.lo, enc.lo, 4);
        std::memcpy(r.hi, enc.hi, 4);
        g_encoded.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> l(g_mtx);
        g_done.push_back(std::move(r));
        return;
    }
    IdxRec rec;
    if (IndexLookup(j.key, rec)) {
        if (rec.format == kRejected) { g_rejected.fetch_add(1, std::memory_order_relaxed); return; }
        const uint64_t t0 = NowUs();
        const bool ok = ReadPayload(rec, r);
        g_readUs.fetch_add(NowUs() - t0, std::memory_order_relaxed);
        if (ok) {
            r.fromCache = true;
            g_hits.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> l(g_mtx);
            g_done.push_back(std::move(r));
            return;
        }
        g_readFails.fetch_add(1, std::memory_order_relaxed);
        {
            // (a bad payload: forgotten, so that the next offer of the texture copies its texels and encodes it again; the new record wins)
            std::lock_guard<std::mutex> l(g_idxMtx);
            g_index.erase(j.key);
        }
        if (j.rgba.empty()) return; // (no texels to encode from: the texture stays as it is this time)
    }
    if (j.rgba.empty()) return;
    const uint64_t t0 = NowUs();
    Encoded enc;
    const bool ok = EncodeTexture(j.rgba.data(), j.w, j.h, j.mips, enc);
    const uint64_t us = NowUs() - t0;
    g_encodeUs.fetch_add(us, std::memory_order_relaxed);
    g_encodedTexels.fetch_add(static_cast<uint64_t>(j.w) * j.h, std::memory_order_relaxed);
    uint64_t mx = g_encodeMaxUs.load(std::memory_order_relaxed);
    while (us > mx && !g_encodeMaxUs.compare_exchange_weak(mx, us, std::memory_order_relaxed)) {}
    if (!ok) return;
    const double floorRgb = Cfg().psnr, floorA = Cfg().psnr + 4.0;
    const bool good = enc.q.psnrRgb >= floorRgb && (enc.format == kFmtDxt1 || enc.q.psnrA >= floorA);
    g_encoded.fetch_add(1, std::memory_order_relaxed);
    const uint64_t tw = NowUs();
    Append(j.key, good ? &enc : nullptr, j.w, j.h);
    g_writeUs.fetch_add(NowUs() - tw, std::memory_order_relaxed);
    if (!good) { g_rejected.fetch_add(1, std::memory_order_relaxed); return; }
    r.format = enc.format;
    r.levels = std::move(enc.levels);
    std::memcpy(r.lo, enc.lo, 4);
    std::memcpy(r.hi, enc.hi, 4);
    std::lock_guard<std::mutex> l(g_mtx);
    g_done.push_back(std::move(r));
}

void* WorkerMain(void*) {
    WorkerPlace();
    if (Mode() != 2) CacheOpen(); // (the ceiling diagnostic never touches the card)
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_mtx);
            g_busy = false;
            g_cvIdle.notify_all();
            g_cv.wait(l, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
            g_busy = true;
        }
        const uint64_t held = j.rgba.size();
        Run(j);
        g_inflight.fetch_sub(held, std::memory_order_relaxed);
    }
    return nullptr;
}
bool StartWorker() {
    if (g_started) return true;
    g_started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128u << 10);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, &WorkerMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (port_log_summary) port_log_summary("tex-compress: the worker thread could not be started (%d): GXV_TEX_COMPRESS does nothing", rc);
        return false;
    }
    pthread_detach(th);
    return true;
}
bool g_workerOk = false;

// ---- usage marks (back end writes, front end reads; ids hashed into 64Ki slots: a collision only ever keeps a texture uncompressed)
constexpr uint32_t kUseSlots = 1u << 16;
std::atomic<uint8_t> g_use[kUseSlots];

uint64_t CacheKey(const OfferIn& in) {
    struct K {
        uint64_t content, tlut;
        uint32_t fmt, dims, flags, version;
        double psnr;
    } k{in.contentHash, in.ref.tlutHash, in.gxFmt, static_cast<uint32_t>(in.w) | (static_cast<uint32_t>(in.h) << 16), in.mips ? 1u : 0u,
        kEncoderVersion, Cfg().psnr};
    return XXH3_64bits(&k, sizeof k);
}
} // namespace

uint8_t Usage(uint32_t texId) { return g_use[texId & (kUseSlots - 1)].load(std::memory_order_relaxed); }
void UsageReset(uint32_t texId) { g_use[texId & (kUseSlots - 1)].store(0, std::memory_order_relaxed); }
void NoteUse(uint32_t texId, uint8_t use) {
    std::atomic<uint8_t>& s = g_use[texId & (kUseSlots - 1)];
    if ((s.load(std::memory_order_relaxed) & use) != use) s.fetch_or(use, std::memory_order_relaxed);
}

Class Classify(const OfferIn& in) {
    if (in.gxFmt <= 3 && !Cfg().ia) return kClsIA;
    if (in.patched) return kClsPatched;
    if (in.ownChain) return kClsChain;
    if ((in.w & (in.w - 1)) || (in.h & (in.h - 1))) return kClsNpot;
    if (in.w < 8 || in.h < 8 || static_cast<uint32_t>(in.w) * in.h < Cfg().minTexels) return kClsSmall;
    if (in.reuploaded) return kClsDynamic;
    return kClsQueued;
}

Class Offer(const OfferIn& in) {
    ++g_fe.offered;
    const Settings& cfg = Cfg();
    const Class c = Classify(in);
    if (c != kClsQueued) { ++g_fe.cls[c]; return c; }
    if (!g_workerOk) {
        g_workerOk = StartWorker();
        if (!g_workerOk) { ++g_fe.cls[kClsCap]; return kClsCap; }
    }
    UsageReset(in.texId);
    Job j;
    j.texId = in.texId;
    j.ref = in.ref;
    j.contentHash = in.contentHash;
    j.key = CacheKey(in);
    j.w = in.w;
    j.h = in.h;
    j.mips = in.mips;
    IdxRec rec;
    const bool known = Mode() != 2 && IndexLookup(j.key, rec);
    if (known && rec.format == kRejected) { ++g_fe.cls[kClsRejected]; return kClsRejected; }
    if (!known) {
        const uint64_t bytes = static_cast<uint64_t>(in.w) * in.h * 4;
        if (g_inflight.load(std::memory_order_relaxed) + bytes > cfg.inflightMax || (!in.rgba && !in.la88 && !in.i8)) { ++g_fe.cls[kClsCap]; g_logDirty = true; return kClsCap; }
        g_inflight.fetch_add(bytes, std::memory_order_relaxed);
        if (in.rgba) j.rgba.assign(in.rgba, in.rgba + bytes);
        else if (in.i8) { j.rgba.resize(bytes); ExpandI8ToRGBA8(in.i8, static_cast<size_t>(in.w) * in.h, j.rgba.data()); } // (I4/I8: (I, I, I, I))
        else { j.rgba.resize(bytes); ExpandLA88ToRGBA8(in.la88, static_cast<size_t>(in.w) * in.h, j.rgba.data()); } // (I/IA: (L, L, L, A))
    }
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_jobs.push_back(std::move(j));
    }
    g_cv.notify_one();
    ++g_fe.cls[kClsQueued];
    ++g_fe.queued;
    g_logDirty = true;
    return kClsQueued;
}

void NoteApplied(uint32_t oldBytes, uint32_t newBytes) {
    ++g_fe.applied;
    g_fe.bytesBefore += oldBytes;
    g_fe.bytesAfter += newBytes;
}

namespace {
void LogMaybe() {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(25);
    if (!g_logDirty || !port_log_summary) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(30)) return;
    last = now;
    g_logDirty = false;
    const Counters c = GetCounters();
    const double encMs = c.encodeUs / 1000.0;
    const double mtps = c.encodeUs ? static_cast<double>(g_encodedTexels.load()) / static_cast<double>(c.encodeUs) : 0.0;
    port_log_summary("tex-compress: GXV_TEX_COMPRESS=%s: %llu textures converted (%llu from the card cache), %.1f MiB -> %.1f MiB (%.1f MiB saved); "
                     "card cache %llu hits (%.0f ms read), %llu encoded (%.0f ms, max %.1f ms, %.2f Mtexel/s), %llu under the quality floor, %.1f MiB "
                     "written (%.0f ms)%s; waiting for a 3D use %llu; left as they were: 2D %llu, indirect %llu, stale %llu, range %llu; not offered: "
                     "I/IA %llu, small %llu, npot %llu, patched/HD %llu, own levels %llu, changing %llu, over the in-flight cap %llu, known below the floor %llu",
                     Cfg().modeText, static_cast<unsigned long long>(c.applied), static_cast<unsigned long long>(c.appliedFromCache),
                     c.bytesBefore / 1048576.0, c.bytesAfter / 1048576.0, (c.bytesBefore - std::min(c.bytesBefore, c.bytesAfter)) / 1048576.0,
                     static_cast<unsigned long long>(c.hits), c.readUs / 1000.0, static_cast<unsigned long long>(c.encoded), encMs,
                     g_encodeMaxUs.load() / 1000.0, mtps, static_cast<unsigned long long>(c.rejected), g_written.load() / 1048576.0, c.writeUs / 1000.0,
                     g_cacheBroken ? " (card cache NOT writable)" : (Cfg().cacheStem.empty() ? " (no card cache)" : ""),
                     static_cast<unsigned long long>(c.waiting), static_cast<unsigned long long>(c.drop2D), static_cast<unsigned long long>(c.dropInd),
                     static_cast<unsigned long long>(c.stale), static_cast<unsigned long long>(c.dropRange), static_cast<unsigned long long>(c.cls[kClsIA]),
                     static_cast<unsigned long long>(c.cls[kClsSmall]), static_cast<unsigned long long>(c.cls[kClsNpot]),
                     static_cast<unsigned long long>(c.cls[kClsPatched]), static_cast<unsigned long long>(c.cls[kClsChain]),
                     static_cast<unsigned long long>(c.cls[kClsDynamic]), static_cast<unsigned long long>(c.cls[kClsCap]),
                     static_cast<unsigned long long>(c.cls[kClsRejected]));
}
} // namespace

void FrameEnd(Verdict (*fn)(Result& r, void* ctx), void* ctx) {
    {
        std::lock_guard<std::mutex> l(g_mtx);
        for (Result& r : g_done) g_wait.push_back(std::move(r));
        if (!g_done.empty()) g_logDirty = true;
        g_done.clear();
    }
    // A bounded look a frame: at most 64 waiting results checked and 4 applied (each apply is one compressed upload: a memcpy of 1/8 - 1/4
    // of the RGBA8 bytes and one transfer wait on the back end; ~250 textures after a stage load are through in about a second).
    constexpr size_t kChecks = 64, kApplies = 4;
    size_t checked = 0, applied = 0;
    if (g_waitNext >= g_wait.size()) g_waitNext = 0;
    while (!g_wait.empty() && checked < std::min(kChecks, g_wait.size()) && applied < kApplies) {
        if (g_waitNext >= g_wait.size()) g_waitNext = 0;
        Result& r = g_wait[g_waitNext];
        ++checked;
        const Verdict v = fn(r, ctx);
        if (v == kKeep) { ++g_waitNext; continue; }
        g_logDirty = true;
        switch (v) {
        case kApplied: ++applied; if (r.fromCache) ++g_fe.appliedFromCache; break;
        case kStale: ++g_fe.stale; break;
        case kDrop2D: ++g_fe.drop2D; break;
        case kDropInd: ++g_fe.dropInd; break;
        case kDropRange: ++g_fe.dropRange; break;
        default: break;
        }
        if (g_waitNext + 1 != g_wait.size()) g_wait[g_waitNext] = std::move(g_wait.back());
        g_wait.pop_back();
    }
    // (a result whose texture is never drawn in 3D waits; the oldest go beyond 4096)
    if (g_wait.size() > 4096) g_wait.erase(g_wait.begin(), g_wait.begin() + static_cast<long>(g_wait.size() - 4096));
    LogMaybe();
}

Counters GetCounters() {
    Counters c = g_fe;
    c.hits = g_hits.load();
    c.encoded = g_encoded.load();
    c.rejected = g_rejected.load();
    c.readUs = g_readUs.load();
    c.encodeUs = g_encodeUs.load();
    c.writeUs = g_writeUs.load();
    c.waiting = g_wait.size();
    c.inflightBytes = g_inflight.load();
    return c;
}
void TestDrain() {
    std::unique_lock<std::mutex> l(g_mtx);
    g_cvIdle.wait(l, [] { return g_jobs.empty() && !g_busy; });
}
void TestReset() {
    TestDrain();
    std::lock_guard<std::mutex> l(g_mtx);
    g_fe = Counters{};
    g_wait.clear();
    g_done.clear();
    for (auto& u : g_use) u.store(0, std::memory_order_relaxed);
    g_hits = g_encoded = g_rejected = g_readUs = g_encodeUs = g_encodeMaxUs = g_writeUs = g_written = g_encodedTexels = g_readFails = 0;
}

// ================================================================ the census (GXV_TEXCOMP_CENSUS=1; back end, render thread)
namespace {
struct Info {
    uint32_t gxFmt = 0xFF;
    uint16_t w = 0, h = 0;
    uint8_t upFormat = 0xFF, cls = 0;
    bool mips = false;
    uint32_t bytes = 0;
};
std::mutex g_infoMtx;
std::unordered_map<uint32_t, Info> g_info; // (front end writes at uploads, the back end reads at the report)
struct Acc {
    double px = 0, est = 0; // pixels covered (overlap counted), estimated bytes read
    uint32_t draws = 0;
    uint8_t use = 0;
    bool rt = false, bounded = false;
    uint16_t rtW = 0, rtH = 0;
};
std::unordered_map<uint32_t, Acc> g_acc; // (back end) since the last report
uint64_t g_censusPics = 0;
double g_censusLastMs = 0;
inline double NowMs() { return static_cast<double>(NowUs()) / 1000.0; }
} // namespace

bool CensusOn() {
    const bool on = VGX_KNOB(EnvInt("GXV_TEXCOMP_CENSUS", 0) != 0 ? 1 : 0) != 0;
    return on;
}
int BackendHooks() { return (Mode() ? 1 : 0) | (CensusOn() ? 2 : 0) | (automip::Mode() ? 1 : 0) | (automip::Mode() == 2 ? 4 : 0); } // (GXV_AUTOMIP: the usage marks, and its census)

void CensusInfo(uint32_t texId, uint32_t gxFmt, uint16_t w, uint16_t h, uint8_t upFormat, uint32_t bytes, bool mips, uint8_t cls) {
    std::lock_guard<std::mutex> l(g_infoMtx);
    Info& i = g_info[texId];
    i.gxFmt = gxFmt; i.w = w; i.h = h; i.upFormat = upFormat; i.bytes = bytes; i.mips = mips;
    if (cls) i.cls = cls; // (a compressed re-upload keeps the class the first upload had)
}

void CensusDraw(uint32_t texId, float px, bool bounded, uint8_t use, bool renderTarget, uint16_t rtW, uint16_t rtH) {
    if (!(px > 0.0f)) return;
    Acc& a = g_acc[texId];
    a.px += px;
    ++a.draws;
    a.use |= use;
    a.rt = a.rt || renderTarget;
    a.bounded = a.bounded || bounded;
    if (renderTarget) { a.rtW = rtW; a.rtH = rtH; }
}

void CensusPicture() {
    ++g_censusPics;
    const double now = NowMs();
    if (g_censusLastMs == 0) g_censusLastMs = now;
    static const double every = 1000.0 * static_cast<double>(std::max(0L, EnvInt("GXV_TEXCOMP_CENSUS_S", 5)));
    if (now - g_censusLastMs < every || !port_log_summary || !g_censusPics) return;
    g_censusLastMs = now;
    // Bytes read, estimated: a draw reads about one texel a covered pixel (a minified texture at its mip level) unless the texture is
    // smaller than what it covers (magnified: then the whole texture, once); bytes a texel by the format the back end holds.
    enum { kDxt, kRgbaOk, kRgba2D, kRgbaInd, kRgbaSmall, kRgbaOther, kIA, kRt, kUnknown, kGroups };
    static const char* const kGroupName[kGroups] = {"DXT", "RGBA/16-bit convertible", "RGBA 2D", "RGBA indirect", "RGBA small/npot",
                                                   "RGBA other", "I/IA (LA88, RGBA8)", "render targets", "unknown"};
    double g[kGroups] = {}, total = 0, save = 0, saveIa = 0;
    struct Top { uint32_t id; double est, px; Info info; Acc acc; };
    std::vector<Top> top;
    {
        std::lock_guard<std::mutex> l(g_infoMtx);
        for (const auto& kv : g_acc) {
            const Acc& a = kv.second;
            Info in;
            auto it = g_info.find(kv.first);
            if (it != g_info.end()) in = it->second;
            double bpt = 4.0, texels = 0;
            int grp = kUnknown;
            if (a.rt) { grp = kRt; bpt = 4.0; texels = static_cast<double>(a.rtW) * a.rtH; }
            else if (it != g_info.end() && in.upFormat != 0xFF) {
                bpt = in.upFormat == TF_DXT1 ? 0.5 : (in.upFormat == TF_DXT5 || in.upFormat == TF_I8) ? 1.0 : (in.upFormat == TF_RGBA8 ? 4.0 : 2.0);
                texels = static_cast<double>(in.w) * in.h * (in.mips ? 4.0 / 3.0 : 1.0);
                if (in.upFormat == TF_DXT1 || in.upFormat == TF_DXT5) grp = kDxt;
                else if (in.gxFmt <= 3) grp = kIA;
                else if (a.use & kUse2D) grp = kRgba2D;
                else if (a.use & kUseInd) grp = kRgbaInd;
                else if (in.cls == kClsSmall || in.cls == kClsNpot) grp = kRgbaSmall;
                else if (in.cls == kClsQueued) grp = kRgbaOk;
                else grp = kRgbaOther;
            }
            const double perPic = 1.0 / static_cast<double>(g_censusPics);
            const double readTexels = texels > 0 ? std::min(a.px, texels * a.draws) : a.px;
            const double est = readTexels * bpt * perPic;
            g[grp] += est;
            total += est;
            if (grp == kRgbaOk) save += est * (1.0 - (in.upFormat == TF_RGBA8 ? 0.25 : in.upFormat == TF_I8 ? 1.0 : 0.5)); // (DXT5 at worst: 1 byte a texel)
            // (I/IA with GXV_TEX_COMPRESS_IA: 1 byte a texel too, when large enough and drawn in 3D only)
            if (grp == kIA && !(a.use & (kUse2D | kUseInd)) && static_cast<uint32_t>(in.w) * in.h >= Cfg().minTexels && !(in.w & (in.w - 1)) && !(in.h & (in.h - 1)))
                saveIa += est * (1.0 - 1.0 / bpt);
            top.push_back(Top{kv.first, est, a.px * perPic, in, a});
        }
    }
    char line[1024];
    int n = std::snprintf(line, sizeof line, "tex-census: %llu pictures, est. texture reads %.2f MB a picture:", static_cast<unsigned long long>(g_censusPics),
                          total / 1e6);
    for (int k = 0; k < kGroups && n > 0 && n < static_cast<int>(sizeof line); ++k)
        if (g[k] > 0) n += std::snprintf(line + n, sizeof line - n, " %s %.2f MB (%.0f%%)", kGroupName[k], g[k] / 1e6, total > 0 ? 100.0 * g[k] / total : 0.0);
    if (n > 0 && n < static_cast<int>(sizeof line))
        std::snprintf(line + n, sizeof line - n, "; GXV_TEX_COMPRESS would save at least %.2f MB a picture (%.0f%%), with GXV_TEX_COMPRESS_IA=1 %.2f MB more (%.0f%%)",
                      save / 1e6, total > 0 ? 100.0 * save / total : 0.0, saveIa / 1e6, total > 0 ? 100.0 * saveIa / total : 0.0);
    port_log_summary("%s", line);
    std::sort(top.begin(), top.end(), [](const Top& a, const Top& b) { return a.est > b.est; });
    static const char* const kGx[16] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7", "C4", "C8", "C14X2", "?B", "?C", "?D", "CMPR", "?F"};
    static const char* const kUp[9] = {"RGBA8", "DXT1", "LA88", "DXT5", "RGB565", "RGBA5551", "RGBA4444", "I8", "?"};
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
        const Top& t = top[i];
        if (t.acc.rt)
            port_log_summary("tex-census top %" VGX_PRIZU ": id %u render target %ux%u: %.0f px a picture (%u draws), est. %.2f MB (%.0f%%)", i + 1, t.id,
                             t.acc.rtW, t.acc.rtH, t.px, t.acc.draws, t.est / 1e6, total > 0 ? 100.0 * t.est / total : 0.0);
        else
            port_log_summary("tex-census top %" VGX_PRIZU ": id %u %s %ux%u%s as %s, class %u, use %s%s%s: %.0f px a picture (%u draws%s), est. %.2f MB (%.0f%%)", i + 1, t.id,
                             t.info.gxFmt < 16 ? kGx[t.info.gxFmt] : "?", t.info.w, t.info.h, t.info.mips ? " mips" : "", kUp[std::min<uint32_t>(t.info.upFormat, 8)],
                             t.info.cls, (t.acc.use & kUse3D) ? "3D" : "", (t.acc.use & kUse2D) ? " 2D" : "", (t.acc.use & kUseInd) ? " ind" : "", t.px,
                             t.acc.draws, t.acc.bounded ? ", some bounded by rectangle" : "", t.est / 1e6, total > 0 ? 100.0 * t.est / total : 0.0);
    }
    g_acc.clear();
    g_censusPics = 0;
}

} // namespace vgx::texcomp
