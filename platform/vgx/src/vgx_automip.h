// GXV_AUTOMIP (automip25): mip levels for textures the game declares WITHOUT any (one level, or a mip chain cut to level 0 by its max LOD),
// made at the texture's first upload and taken back if the game is seen to draw the texture in 2D or as an indirect map. Off by default. The whole design, the rules and the reasons are in
// vgx_texcache.cpp (the section "GXV_AUTOMIP"); this header holds the switches (read once, by any thread) and the few things the back
// end and the texcomp usage marks share with the front end.
//   GXV_AUTOMIP=0        off (default): nothing is read, written or logged; the picture is bit-identical to a build without this file
//   GXV_AUTOMIP=1        on: qualifying textures get a generated chain at their first upload, and are sampled with a mip filter
//   GXV_AUTOMIP=2        census only: one "automip-census" line every GXV_AUTOMIP_SEC (10) seconds, nothing changes
//   GXV_AUTOMIP_FILTER=1|2   1 (default) bilinear + nearest level (LINEAR_MIPMAP_NEAREST: one fetch, as without mips); 2 trilinear (two fetches)
//   GXV_AUTOMIP_BIAS=<n>     LOD bias in eighths of a level added to the texture's bias (default -4 = half a level sharper: level 0 stays
//                            exact until a texel covers 1.41 pixels, so near surfaces are as before and only the minified ones soften)
//   GXV_AUTOMIP_MIN=<n>      the larger side must be at least n texels (default 64); the smaller side must be at least 16
//   GXV_AUTOMIP_KB=<n>       the most extra texture memory the generated levels may hold at any time, in KiB (default 6144 = 6 MiB)
//   GXV_AUTOMIP_ALPHA=1      textures with transparent texels too, with their own chain whose alpha keeps the cut-out's coverage (default 0: left out;
//                            2 is a test: the plain box average of the alpha)
//   GXV_AUTOMIP_VERIFY=<n>   pictures a texture given a chain is watched: drawn in 2D or read as an indirect map, it is taken back (default 600)
//   GXV_AUTOMIP_DXT_TEXELS=<n>  the largest CMPR texture (level 0 texels) given a chain, whose levels are encoded on the CPU at the upload
//                            (default 32768 = 256x128); RGBA8-path textures cost no CPU (the back end makes the levels)
//   GXV_AUTOMIP_DXT_PLAY=1   let a CMPR texture be given its chain in a live picture too (default 0: only at loading screens and fades)
#pragma once
#include "vgx_common.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace vgx {
namespace automip {

struct Cfg {
    int mode = 0;             // 0 off, 1 on, 2 census only
    int filter = 1;           // 1 LINEAR_MIPMAP_NEAREST, 2 LINEAR_MIPMAP_LINEAR
    int bias = -4;            // eighths of a level
    uint32_t minSide = 64;    // the larger side
    uint32_t minSmall = 16;   // the smaller side
    uint64_t budget = 6144ull << 10; // live extra bytes
    bool alpha = false;
    int alphaMode = 0;        // GXV_AUTOMIP_ALPHA: 1 coverage-preserving, 2 plain box average (a test: shows what the rule is for)
    uint32_t verify = 600;
    uint32_t dxtTexels = 32768;
    bool dxtPlay = false;
    uint32_t sec = 10;
};
inline const Cfg& Config() {
    static const Cfg c = [] {
        Cfg k;
        const long m = EnvInt("GXV_AUTOMIP", 0);
        k.mode = m == 1 ? 1 : m == 2 ? 2 : 0;
        if (k.mode == 0) return k; // (nothing else is read)
        k.filter = EnvInt("GXV_AUTOMIP_FILTER", 1) == 2 ? 2 : 1;
        long b = EnvInt("GXV_AUTOMIP_BIAS", -4);
        k.bias = static_cast<int>(b < -16 ? -16 : b > 8 ? 8 : b);
        long v = EnvInt("GXV_AUTOMIP_MIN", 64);
        k.minSide = static_cast<uint32_t>(v < 8 ? 8 : v > 4096 ? 4096 : v);
        v = EnvInt("GXV_AUTOMIP_KB", 6144);
        k.budget = static_cast<uint64_t>(v < 0 ? 0 : v) << 10;
        k.alphaMode = static_cast<int>(EnvInt("GXV_AUTOMIP_ALPHA", 0));
        k.alpha = k.alphaMode == 1 || k.alphaMode == 2;
        v = EnvInt("GXV_AUTOMIP_VERIFY", 600);
        k.verify = static_cast<uint32_t>(v < 1 ? 1 : v);
        k.dxtPlay = EnvInt("GXV_AUTOMIP_DXT_PLAY", 0) != 0;
        v = EnvInt("GXV_AUTOMIP_DXT_TEXELS", 32768);
        k.dxtTexels = static_cast<uint32_t>(v < 0 ? 0 : v);
        v = EnvInt("GXV_AUTOMIP_SEC", 10);
        k.sec = static_cast<uint32_t>(v < 1 ? 1 : v);
        return k;
    }();
    return c;
}
inline int Mode() { return Config().mode; }


// ---- pure functions of the front end (here so that a test can call them without the cache)
// Is every texel of this RGBA8 level opaque?
inline bool Opaque(const std::vector<uint8_t>& lv) {
    for (size_t i = 3; i < lv.size(); i += 4)
        if (lv[i] != 255) return false;
    return true;
}
// Alpha that keeps a cut-out's coverage (Castano, "Computing alpha mipmaps"): the share of texels at or over the alpha test's reference
// (assumed 0.5: 128; the game's own reference is not known here) is measured at level 0, and each level's alpha is scaled so that the same
// share of ITS texels reaches 128. Colours are the plain box average. A level whose texels are all alike cannot be corrected (all or none
// pass): the scale then only moves them to 128 or leaves them.
inline void Coverage(std::vector<std::vector<uint8_t>>& lv) {
    auto share = [](const std::vector<uint8_t>& l) {
        size_t n = 0;
        for (size_t i = 3; i < l.size(); i += 4) n += l[i] >= 128;
        return static_cast<double>(n) / static_cast<double>(l.size() / 4);
    };
    const double want = share(lv[0]);
    for (size_t k = 1; k < lv.size(); ++k) {
        std::vector<uint8_t>& l = lv[k];
        const size_t n = l.size() / 4;
        if (want <= 0.0 || want >= 1.0 || n == 0) continue;
        uint32_t hist[256] = {}; // (the alpha value t whose share of texels at or over it is nearest the wanted one)
        for (size_t i = 3; i < l.size(); i += 4) ++hist[l[i]];
        size_t acc = 0;
        int t = 0;
        double bestErr = 1e30;
        for (int a = 255; a >= 1; --a) {
            acc += hist[a];
            const double err = std::fabs(static_cast<double>(acc) / static_cast<double>(n) - want);
            if (err < bestErr) { bestErr = err; t = a; }
        }
        if (t <= 0) continue;
        const double s = 128.0 / static_cast<double>(t);
        for (size_t i = 3; i < l.size(); i += 4) l[i] = static_cast<uint8_t>(s * l[i] + 0.5 > 255.0 ? 255.0 : s * l[i] + 0.5);
    }
}

// ---- the back end's side (vgx_backend_gl.cpp; render thread; weak there). Both only while the census is on (bit 2 of the texcomp hooks).
// NoteDraw: a texture drawn: the display pixels the draw covers, its use marks (texcomp::kUse3D / kUse2D / kUseInd), whether it is a render target.
void NoteDraw(uint32_t texId, float px, uint8_t use, bool renderTarget);
void Picture(); // end of a picture: the census line when its window is over

} // namespace automip
} // namespace vgx
