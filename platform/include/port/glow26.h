// GXV_GLOW26 (glow26, 12 Nov 2026; default 0 = off): the cheap look-alike for the costly soft effects (glow sheets, haze, bloom, light beams), chosen by cost and not by stage name.
// Pure logic, no GL and no game state, so that tests/glow26_test.cpp can drive it; the back end (vgx_backend_gl.cpp, vgx_draw.cpp) and the game's bloom / light-volume code only read it.
//
// Owner's ruling (12 Nov 10:53): a soft effect may be re-implemented more cheaply if it looks similar to the player and keeps its visual role; it must not vanish.
//
// What a level does (Profile / Apply; every switch is only RAISED, an explicit stage setting that is already higher stays):
//   0  nothing (Quality mode). The default path is bit-identical.
//   1  mild:   compositor (GXV_FXLOWRES_AUTO=1) with the nearest-depth composite (UPS=1) and DEFER=1 (additive layers of a stretch in one run), keys routed from 0.5 screens of fill;
//              GXV_IND_FLAT=1 (blended warps read their texture directly: measured -3.3 ms at Freezeflame, no visible change); GXV_BLOOM_FOLD=1 (folded bloom chain, blur shape kept);
//              GXV_LIGHTVOL=3 (exact culling and grouping of the light beams).
//   2  strong: as 1, but DEFER=2, keys routed from 0.3 screens, quarter size when 80 % of the routed fill is soft and at least 2.5 screens, a run may pay off at 0.2 ms;
//              GXV_BLOOM_FOLD=2 (reduced bloom, energy within about +-10 % for medium and large sources); GXV_LIGHTVOL=7 (far, small beams fade).
// Never touched, by construction of the compositor's own rules (vgx_fxauto.h, backend Gather): Mario, the HUD / 2D pass, opaque geometry, alpha-tested cut-outs, anything that writes depth,
// small crisp sprites (thin rule), draws near the camera (near rule).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "stage_knob.h"  // (q30-26: Quality())
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf)
#include <cstdio>
#include <cstring>

namespace port {
namespace glow26 {

// q30-26: kQ = the Quality (30) mode's level: what a level 1 / 2 becomes there. Only the MEASURED-INVISIBLE members: GXV_IND_FLAT=1 (Freezeflame 27.0 -> 23.7 ms GPU, no visible change) and the exact
// light-volume cull and grouping (GXV_LIGHTVOL=3). No compositor (reduced-size layers), no bloom fold (halo grain differs, visual check owed), no light-beam fade.
constexpr int kQ = 3;
inline int ClampLevel(long v) { return v >= 1 ? (stageknob::Quality() ? kQ : (v >= 2 ? 2 : 1)) : 0; }

// ---------------------------------------------------------------- the switches a level sets
struct Profile {
    int autoMode = 0;        // GXV_FXLOWRES_AUTO (0 off, 1 route, 2 census only)
    int ups = 0;             // GXV_FXLOWRES_UPS: composite with nearest depth
    int defer = 0;           // GXV_FXLOWRES_DEFER
    long autoMilli = 750;    // GXV_FXLOWRES_AUTO_MILLI: thousandths of a screen of fill a key needs
    float quarterScreens = 4.0f, softFrac = 0.9f, minNetMs = 0.3f, joinMs = 0.25f; // fxauto::Cfg
    int indFlat = 0;         // GXV_IND_FLAT
    int bloomFold = 0;       // GXV_BLOOM_FOLD
    int lightvol = 0;        // GXV_LIGHTVOL (bit mask)
};
inline Profile Apply(int level, Profile p) {
    if (level <= 0) return p;
    if (level >= kQ) {   // (q30-26: Quality: the measured-invisible members only; nothing is routed to the reduced-size target)
        p.indFlat = std::max(p.indFlat, 1);
        p.lightvol |= 3;
        return p;
    }
    if (p.autoMode == 0) p.autoMode = 1; // (a census (2) is left as it is: the census run must not change the picture)
    p.ups = std::max(p.ups, 1);
    p.indFlat = std::max(p.indFlat, 1);
    p.lightvol |= 3;
    if (level == 1) {
        p.defer = std::max(p.defer, 1);
        p.autoMilli = std::min(p.autoMilli, 500L);
        p.bloomFold = std::max(p.bloomFold, 1);
    } else {
        p.defer = std::max(p.defer, 2);
        p.autoMilli = std::min(p.autoMilli, 300L);
        p.quarterScreens = std::min(p.quarterScreens, 2.5f);
        p.softFrac = std::min(p.softFrac, 0.8f);
        p.minNetMs = std::min(p.minNetMs, 0.2f);
        p.bloomFold = std::max(p.bloomFold, 2);
        p.lightvol |= 4;
    }
    return p;
}
// The single-switch floors, for the sites that read one switch (vgx_draw.cpp IndFlatMode, the bloom and light-volume readers).
inline int IndFlatFloor(int level, int v) { return level >= 1 ? std::max(v, 1) : v; }
inline int BloomFoldFloor(int level, int v) { return level >= kQ ? v : (level >= 2 ? std::max(v, 2) : (level == 1 ? std::max(v, 1) : v)); }
// compfix26: what the raised switches actually did, counted where they act and printed by the back end's "fxlowres: ... companions" line (a mode on a census line proves a setting, a count proves an effect).
struct Counters {
    std::atomic<uint32_t> indFlatStages{0};   // indirect stages left direct by GXV_IND_FLAT (counted per fragment-key build, so per distinct shader, not per draw)
    std::atomic<uint32_t> bloomFolded{0};     // bloom pictures drawn by the folded / reduced chain (GXV_BLOOM_FOLD 1 or 2)
    std::atomic<uint32_t> lightvolPics{0}, lightvolVolumes{0}, lightvolCulled{0}, lightvolCut{0}, lightvolFaded{0}; // GXV_LIGHTVOL: pictures with volumes, volumes seen, culled, cut to the clear pass, faded (APPLIED, not planned)
};
inline Counters g_count;
inline int LightVolFloor(int level, int v) { return level >= kQ ? (v | 3) : (level >= 2 ? (v | 7) : (level == 1 ? (v | 3) : v)); }

// ---------------------------------------------------------------- which layers are the expensive soft ones
// One layer = one blended draw (or the sum of the draws of one shader key) as the back end measures it.
struct LayerObs {
    bool blend = false, additive = false;
    bool depthWrite = false, alphaTest = false, hud = false, mario = false, opaque = false;
    bool near_ = false;           // nearer than the near limit
    float fillScreens = 0.0f;     // screens of fill, overlap counted
    float coverScreens = 0.0f;    // the screens its union rectangle covers (0 when not known)
    float widthPx = 1000.0f;      // mean projected width in pixels (small crisp sprites are narrow)
    float soft = 0.0f;            // 0 crisp .. 1 smooth: share of the texture's variance that survives block averaging (see SoftScore)
};
struct ClassCfg {
    float enterScreens = 0.5f;    // fill needed to become a soft layer
    float exitFactor = 0.6f;      // leave under enterScreens * exitFactor
    float minSoft = 0.8f;         // smoothness needed (SoftScore)
    float minWidthPx = 24.0f;     // narrower draws are crisp sprites (star bits, coins, sparkles)
    float overdraw = 2.0f;        // fill / cover at which a layer counts as stacked even when small
    uint32_t enterN = 3, exitN = 8; // consecutive pictures to enter / leave (hysteresis)
};
// 1 = smooth: 1 - (energy left after averaging 4 x 4 blocks) / variance, clamped. The back end's IF_SOFT uses 0.9 on the same measure.
inline float SoftScore(float residualOverVariance) { return std::clamp(1.0f - residualOverVariance, 0.0f, 1.0f); }
// Never-rules first; then cost: fill (or stacking) big enough and the texture smooth.
inline bool NeverTouch(const LayerObs& o) { return !o.blend || o.depthWrite || o.alphaTest || o.hud || o.mario || o.opaque; }
inline bool Qualifies(const LayerObs& o, const ClassCfg& c, bool wasSoft) {
    if (NeverTouch(o) || o.near_) return false;
    if (o.widthPx < c.minWidthPx) return false;
    if (o.soft < c.minSoft) return false;
    const float need = wasSoft ? c.enterScreens * c.exitFactor : c.enterScreens;
    const bool stacked = o.coverScreens > 0.0f && o.fillScreens / o.coverScreens >= c.overdraw && o.fillScreens >= need * 0.5f;
    return o.fillScreens >= need || stacked;
}
// Per-key state with hysteresis: the classification flips only after enterN / exitN pictures in a row, so a layer whose fill hovers around the limit does not flicker.
struct KeyState {
    bool soft = false;
    uint32_t run = 0;   // consecutive pictures that disagree with `soft`
    uint32_t flips = 0;
};
inline bool Step(KeyState& s, bool qualifies, const ClassCfg& c) {
    if (qualifies == s.soft) { s.run = 0; return s.soft; }
    if (++s.run >= (s.soft ? c.exitN : c.enterN)) { s.soft = qualifies; s.run = 0; ++s.flips; }
    return s.soft;
}

// ---------------------------------------------------------------- intensity compensation
// Additive light is linear, so replacing N equal stacked layers by one drawn with N times the intensity keeps the sum; the framebuffer clamps at 1, so the sum is kept only
// while the pixel stays under 1. The functions answer: the scale, and the share of the light that survives the clamp (1 = all).
inline float CollapseScale(int layers) { return static_cast<float>(std::max(layers, 1)); }
inline float StrideScale(int keepEvery) { return static_cast<float>(std::max(keepEvery, 1)); }
// Light kept when `layerValue` (0..1 per layer, the texture's value times the colour) is stacked `layers` times and drawn as 1 layer at `scale` times: before = min(1, dst + layers * v) - dst
// against after = min(1, dst + scale * v) - dst, for a destination `dst` (0..1). Both are clamped, so the ratio is 1 unless the original clamps and the new one does not (or the reverse).
inline float KeptLight(float dst, float layerValue, int layers, float scale) {
    const float before = std::min(1.0f, dst + layerValue * static_cast<float>(layers)) - dst;
    const float after = std::min(1.0f, dst + layerValue * scale) - dst;
    return before > 1e-6f ? after / before : 1.0f;
}
// Alpha-8 quantisation: a layer drawn at 1/N intensity loses its low bits; the sum of N quantised layers differs from the exact sum by up to N/2 levels. The collapsed layer
// (scale N) is quantised once. True when the collapsed form is the more accurate one (always for N >= 2); the error in 1/255 levels of the original form is N/2.
inline float QuantErrLevels(int layers) { return 0.5f * static_cast<float>(std::max(layers, 1)); }

// ---------------------------------------------------------------- tight rectangles
struct Rect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
inline float Area(const Rect& r) { return std::max(0.0f, r.x1 - r.x0) * std::max(0.0f, r.y1 - r.y0); }
inline Rect Intersect(const Rect& a, const Rect& b) {
    Rect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}
// The sub-rectangle (0..1 of the texture) outside which every texel's light (max channel * alpha) is under `thresh` (e.g. 1/255): the part of a soft texture that matters.
inline Rect OpaqueExtent(const float* light, int w, int h, float thresh) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (light[y * w + x] >= thresh) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 < 0) return Rect{0, 0, 0, 0};
    // one texel of margin each side: bilinear sampling reads a neighbour
    return Rect{std::max(0, x0 - 1) / static_cast<float>(w), std::max(0, y0 - 1) / static_cast<float>(h), std::min(w, x1 + 2) / static_cast<float>(w), std::min(h, y1 + 2) / static_cast<float>(h)};
}
// An axis-aligned textured quad covering `quad` (NDC or pixels) with texture 0..1 across it: the quad that covers only the texture sub-rectangle `ext`, cut to `screen`.
// The result's uv0 / uv1 are the texture coordinates of its corners.
struct TightQuad { Rect quad; float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };
inline TightQuad Tighten(const Rect& quad, const Rect& ext, const Rect& screen) {
    TightQuad t;
    const float qw = quad.x1 - quad.x0, qh = quad.y1 - quad.y0;
    Rect sub{quad.x0 + ext.x0 * qw, quad.y0 + ext.y0 * qh, quad.x0 + ext.x1 * qw, quad.y0 + ext.y1 * qh};
    const Rect cut = Intersect(sub, screen);
    t.quad = cut;
    if (qw > 0.0f && qh > 0.0f) {
        t.u0 = (cut.x0 - quad.x0) / qw; t.u1 = (cut.x1 - quad.x0) / qw;
        t.v0 = (cut.y0 - quad.y0) / qh; t.v1 = (cut.y1 - quad.y0) / qh;
    }
    return t;
}
// Fill saved (screens) by drawing `tight` instead of `quad`, with `layers` stacked.
inline float FillSavedScreens(const Rect& quad, const Rect& tight, float screenArea, int layers) {
    return std::max(0.0f, Area(quad) - Area(tight)) / std::max(screenArea, 1e-9f) * static_cast<float>(std::max(layers, 1));
}
// Pixels of fill a picture saves by drawing a layer set of `fillScreens` at 1/div of the width and height (the composite costs one pass over `rectScreens`).
inline float PixelsSaved(float fillScreens, int div, float rectScreens, float screenPx = 960.0f * 544.0f) {
    const float d2 = static_cast<float>(div * div);
    return (fillScreens * (1.0f - 1.0f / d2) - rectScreens) * screenPx;
}

// ---------------------------------------------------------------- the similarity check (GXV_GLOW26_VERIFY)
// RGBA8 pictures, rows top to bottom or bottom to top alike (both pictures must use the same).
struct Diff {
    uint64_t n = 0;          // pixels compared
    double sumAbs = 0;       // sum over pixels of the mean absolute channel difference (RGB), 0..255
    uint32_t maxAbs = 0;     // largest channel difference
    double sumSq = 0;        // for PSNR
    double lightA = 0, lightB = 0; // sum of R+G+B in each picture
};
inline void Accum(Diff& d, const uint8_t* a, const uint8_t* b, size_t pixels, const Rect* roi = nullptr, int w = 0, int h = 0) {
    for (size_t i = 0; i < pixels; ++i) {
        if (roi && w > 0) {
            const float x = (static_cast<float>(i % static_cast<size_t>(w)) + 0.5f) / static_cast<float>(w), y = (static_cast<float>(i / static_cast<size_t>(w)) + 0.5f) / static_cast<float>(std::max(h, 1));
            if (x < roi->x0 || x >= roi->x1 || y < roi->y0 || y >= roi->y1) continue;
        }
        int s = 0;
        double sq = 0;
        for (int c = 0; c < 3; ++c) {
            const int da = a[i * 4 + c], db = b[i * 4 + c], e = std::abs(da - db);
            s += e;
            sq += static_cast<double>(e) * e;
            d.maxAbs = std::max<uint32_t>(d.maxAbs, static_cast<uint32_t>(e));
            d.lightA += da;
            d.lightB += db;
        }
        d.sumAbs += s / 3.0;
        d.sumSq += sq / 3.0;
        ++d.n;
    }
}
inline double Mean(const Diff& d) { return d.n ? d.sumAbs / static_cast<double>(d.n) : 0.0; }
inline double Psnr(const Diff& d) { const double mse = d.n ? d.sumSq / static_cast<double>(d.n) : 0.0; return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0; }
// Light ratio B / A of the whole compared area. 1 when A is black.
inline double LightRatio(const Diff& d) { return d.lightA > 0 ? d.lightB / d.lightA : 1.0; }
// The glow's own light: the picture's light inside the region of interest minus the same region's light without the glow (the baseline, taken from a picture in which the layers were
// left out, or estimated by the caller); the layer ratio is cheap-glow / original-glow. Returns 1 when the original glow is nil.
inline double LayerRatio(double lightOrig, double lightCheap, double lightBase) {
    const double g = lightOrig - lightBase;
    return std::fabs(g) > 1e-9 ? (lightCheap - lightBase) / g : 1.0;
}
// What a look-alike must meet: total light within 10 %, small mean error (the noise floor of two pictures of the same method is subtracted by the caller if known).
struct Verdict { bool lightOk, meanOk; };
inline Verdict Judge(double lightRatio, double meanErr, double noiseMean, double maxLightDev = 0.10, double maxMeanOver = 1.5) {
    return Verdict{std::fabs(lightRatio - 1.0) <= maxLightDev, meanErr <= noiseMean + maxMeanOver};
}

// ---------------------------------------------------------------- census
struct Census {
    uint32_t pictures = 0;            // pictures observed
    double compLayers = 0;            // sum over pictures of the compositor's routed runs
    double compKeys = 0;              // keys routed (the stage's, not a sum)
    int div = 2;                      // reduced-size divisor
    double fillMovedScreens = 0;      // sum over pictures of the fill moved to the reduced-size target (screens)
    double netMs = 0;                 // sum over pictures of the model's saving minus its cost (an estimate)
    double rectScreens = 0;           // sum over pictures of the composite's rectangles (screens)
    int indFlat = 0, bloomFold = 0, lightvol = 0; // the other methods' modes (they change a shader key or a draw list, not countable per picture here)
    double lightRatio = 1.0;          // by construction 1.0; GXV_GLOW26_VERIFY measures the real one
};
inline int Format(char* out, size_t cap, const char* stage, int level, const Census& c) {
    const double n = c.pictures ? static_cast<double>(c.pictures) : 1.0;
    const double saved = PixelsSaved(static_cast<float>(c.fillMovedScreens / n), c.div, static_cast<float>(c.rectScreens / n));
    return snprintf(out, cap,
                         "glow26: stage %s: level %d: layers treated by method a picture: compositor %.2f runs (%.0f keys routed, 1/%d size), indirect warps read direct (GXV_IND_FLAT=%d), "
                         "bloom folded (GXV_BLOOM_FOLD=%d), light beams (GXV_LIGHTVOL=%d); compositor fill moved %.2f screens, fill saved %.0f pixels a picture (%.2f screens of 960x544), "
                         "estimated net %+.2f ms; light ratio %.3f (by construction; GXV_GLOW26_VERIFY measures it)",
                         stage, level, c.compLayers / n, c.compKeys, c.div, c.indFlat, c.bloomFold, c.lightvol, c.fillMovedScreens / n, saved, saved / (960.0 * 544.0), c.netMs / n, c.lightRatio);
}

// ---------------------------------------------------------------- A/B schedule (GXV_GLOW26_VERIFY)
// Pictures are taken in blocks of `block`: block 0 ORIGINAL (no compositor), block 1 CHEAP, block 2 ORIGINAL, ... so that every CHEAP block has an ORIGINAL block before and after:
// the difference between those two is the noise floor of the scene (animation), the difference to CHEAP the method's error.
enum Phase { P_ORIG = 0, P_CHEAP = 1, P_OFF = 2 };
// Blocks cycle in four: ORIGINAL, CHEAP, ORIGINAL, then CHEAP (mode 1) or the glow layers LEFT OUT (mode 2: the baseline for the layer's own brightness).
inline Phase PhaseOf(uint64_t picture, uint32_t block, int mode) {
    const unsigned b = static_cast<unsigned>((picture / std::max<uint32_t>(block, 1)) & 3u);
    return b == 1 ? P_CHEAP : (b == 3 ? (mode >= 2 ? P_OFF : P_CHEAP) : P_ORIG);
}
inline bool IsCheapBlock(uint64_t picture, uint32_t block) { return PhaseOf(picture, block, 1) == P_CHEAP; }
inline bool LastOfBlock(uint64_t picture, uint32_t block) { return picture % std::max<uint32_t>(block, 1) == std::max<uint32_t>(block, 1) - 1; }
inline bool BlockStart(uint64_t picture, uint32_t block) { return picture % std::max<uint32_t>(block, 1) == 0; }

} // namespace glow26
} // namespace port
