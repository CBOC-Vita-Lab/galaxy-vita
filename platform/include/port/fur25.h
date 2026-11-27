// reflfur25 (12 Nov): fur shells by distance and projected size, with their alpha merged, and the census. Header only (the pure parts are tested by
// tests/fur25.cpp, tools/test_fur25.sh). Everything defaults to 0 = the game's / the existing GXV_PB_FUR + GXV_DETAIL_FUR behaviour, bit-identical.
//
// How the game draws fur (decomp Util/FurCtrl.cpp, FurDrawer.cpp, FurMulti.cpp): a fur material of a model (a planet's, the queen bee's ...) is FurMulti
// -> one FurCtrl per fur material: n (= the .fur.txt's layer count, 6 by default) SHELLS, the same mesh n times, each with its vertices pushed out along
// the normal by L_i = length * ((i + 1) / n) ^ curve (static: made once by calcLayerForm), drawn root to tip as alpha blended full-shape draws (SRCALPHA /
// INVSRCALPHA, alpha test 32). A shell's look is three constants of its TEV: K3_R (a grey brightness added), K3_G (the STRAND alpha: out alpha = strandMap.a
// * K3_G + K3_B) and K3_B (the SKIN alpha everywhere). Every shell is a draw on the game thread (setup + record), the front end (a shape) and a blended pass
// over its pixels on the GPU. The existing levers: GXV_PB_FUR (every second shell beyond a camera distance, tip always kept, no alpha change, a pop at the
// distance with GXV_POPIN_HYST's hysteresis) and GXV_DETAIL_FUR (the same in 2 states with a fade and a pow(n / kept, 0.5) alpha gain).
//
//   GXV_FUR25=0|1|2   (stage-settable, key "fur25")
//     the shells an object draws follow the SCREEN SIZE of its fur: thickness_px = tipLength * scale * px-per-unit at the object's nearest point (a bound,
//     the shells are never farther apart than that: it is the silhouette direction). Adjacent shells closer than GAP px add nothing:
//       supported shells s = thickness_px / GAP + 1,  kept shells k = floor(s) clamped to [2, n]:  k < n  => the n shells form k groups of consecutive
//       shells, each drawn as ITS TOP shell, with the group's alpha MERGED so the optical density is kept exactly (below).
//     full (k = n) inside FULL units of the camera, within NEAR units of the player, in a flight / demo (port_pb_demo_off), for light type 0 (the player's).
//     k moves by at most ONE shell per STEP pictures with a hysteresis of HYST shells (k stays while s is in [k - HYST, k + 1 + HYST)); an object that was
//     not drawn for a while is set to its target at once (it was unseen).
//     level 1 (conservative): GAP 0.75 px, FULL 1500 units, NEAR 700, STEP 12 pictures.  level 2 (stronger): GAP 1.25 px, FULL 1000, NEAR 500, STEP 6.
//   Alpha compensation (exact optical density): a ray through shells m of a group passes the strand with transmittance prod(1 - A_m), A_m = K3_G_m / 255
//     + K3_B_m / 255 (strand pixel) and prod(1 - B_m) (skin pixel); the merged shell has A' = 1 - prod(1 - A_m), B' = 1 - prod(1 - B_m),
//     K3_B' = 255 B', K3_G' = 255 (A' - B'), and K3_R' = the mean of the group's K3_R weighted by each shell's visible contribution
//     (A_m * prod over the shells above it (1 - A)), so the sum of optical depths -ln(1 - A) is kept (equal up to the 255 clamp) and the colour to first order.
//   GXV_FURCENSUS=1  "furcensus:" every 5 s: objects and shells per picture, by the nearest-point distance band and by the projected thickness band.
//   Env-only fine tuning: GXV_FUR25_GAP (px), GXV_FUR25_FULL, GXV_FUR25_NEAR (units), GXV_FUR25_STEP (pictures), GXV_FUR25_HYST (shells), GXV_FUR25_LINES (544).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/hot_knob.h"
#include "port/stage_knob.h"

namespace port {
namespace fur25 {

inline float EnvF(const char* n, float d) {
    float ov;
    if (port::stageknob::Lookup(n, &ov)) return ov;
    const char* e = std::getenv(n);
    return e && *e ? static_cast<float>(std::atof(e)) : d;
}
inline int Level() { return PORT_STAGE_KNOB(int, [] { const int v = static_cast<int>(EnvF("GXV_FUR25", 0.0f)); return v < 0 ? 0 : (v > 2 ? 2 : v); }()); }
inline int CensusOn() { return PORT_HOT_KNOB(int, std::getenv("GXV_FURCENSUS") && std::atoi(std::getenv("GXV_FURCENSUS")) != 0 ? 1 : 0); }
inline float Lines() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_FUR25_LINES", 544.0f); return v < 64.0f ? 64.0f : v; }()); }

struct Params {
    float gapPx;    // adjacent kept shells closer than this on the screen add nothing
    float fullDist; // nearest-point camera distance inside which every shell is drawn
    float nearDist; // nearest-point distance to the player inside which every shell is drawn
    int stepPics;   // pictures between two one-shell changes of one object
    float hyst;     // shells
};
inline Params ParamsFor(int level) {
    Params p;
    if (level >= 2) { p.gapPx = 1.25f; p.fullDist = 1000.0f; p.nearDist = 500.0f; p.stepPics = 6; }
    else { p.gapPx = 0.75f; p.fullDist = 1500.0f; p.nearDist = 700.0f; p.stepPics = 12; }
    p.hyst = 0.35f;
    return p;
}
inline Params CurrentParams(int level) {
    Params p = ParamsFor(level);
    float v;
    if ((v = EnvF("GXV_FUR25_GAP", 0.0f)) > 0.05f) p.gapPx = v;
    if ((v = EnvF("GXV_FUR25_FULL", 0.0f)) > 0.0f) p.fullDist = v;
    if ((v = EnvF("GXV_FUR25_NEAR", 0.0f)) > 0.0f) p.nearDist = v;
    if ((v = EnvF("GXV_FUR25_STEP", 0.0f)) >= 1.0f) p.stepPics = static_cast<int>(v);
    if ((v = EnvF("GXV_FUR25_HYST", -1.0f)) >= 0.0f) p.hyst = v > 2.0f ? 2.0f : v;
    return p;
}

// ---- the projection ------------------------------------------------------------------------------------------------------------------
// Screen pixels (of `lines` picture lines) one world unit covers at camera distance `dist`, a vertical field of view of `fovyDeg` degrees.
inline float PixelsPerUnit(float lines, float fovyDeg, float dist) {
    const float t = std::tan(fovyDeg * 0.5f * 3.14159265358979f / 180.0f);
    const float d = dist > 1.0f ? dist : 1.0f;
    return lines / (2.0f * (t > 1e-4f ? t : 1e-4f) * d);
}
// The nearest point of an object of bound `radius` at centre distance `centre` (never under 1).
inline float NearestDist(float centre, float radius) { const float d = centre - radius; return d > 1.0f ? d : 1.0f; }
// ---- fur31 (17 Nov): GXV_PB_FUR for every fur, measured to the fur's SURFACE, latched against the governor ------------------------------------------------
// The Queen Bee's coat changed its shell count again and again while Mario climbed her: the rule measured from the actor's origin, her coat is over 1000 units
// across, and the limit also moves with the governor in 500-unit steps. Now for all fur: (1) the distance is the nearest point of the fur's bound (the origin
// distance against limit + radius * scale: the same test); (2) the way back to the full coat is a band wider than a governor step (kPbMinBand); (3) a coat that
// was drawn in a recent picture keeps the base distance it was last decided with while a new base (a governor step, a level change) would flip its shell count:
// it takes the new base when it would not flip it, or once it was not drawn for a few pictures (off screen), or while it is under the pop-in floor (GXV_PB_MINPX).
constexpr float kPbMinBand = 600.0f;  // units; the governor's steps (cpulod.h: 2500 / 2000 / 1500) are 500 apart at most
inline float PbBand(float limit, float pct) { const float b = limit * pct * 0.01f; return b > kPbMinBand ? b : kPbMinBand; }
inline float SurfaceReach(float reach, float radius, float sx, float sy, float sz) {
    if (!(radius > 0.0f)) return reach;
    const float scale = std::fmax(std::fabs(sx), std::fmax(std::fabs(sy), std::fmax(std::fabs(sz), 0.0f)));
    return reach + radius * scale;
}
// Thinned at the origin distance `centreD`? A thinned coat comes back to full only inside limit - band (never under 0), both measured to the surface.
inline bool PbThinAt(bool wasThin, float centreD, float limit, float pct, float radius, float sx, float sy, float sz) {
    float reach = limit;
    if (wasThin) { reach -= PbBand(limit, pct); if (reach < 0.0f) reach = 0.0f; }
    reach = SurfaceReach(reach, radius, sx, sy, sz);
    return centreD * centreD > reach * reach;
}
// GXV_PB_FUR_SURFACE=0: the old rule (origin distance, the global hysteresis, no latch). Default 1 (a bug fix).
inline int PbSurfaceOn() { return PORT_HOT_KNOB(int, std::getenv("GXV_PB_FUR_SURFACE") && std::atoi(std::getenv("GXV_PB_FUR_SURFACE")) == 0 ? 0 : 1); }
struct PbLatch { float base = 0.0f; uint32_t pic = 0; };
// thinFor(base) -> whether the coat is thinned for that base distance. Returns the decision to draw by and keeps the base it was made with in `st`.
template <class F>
inline bool PbThin(PbLatch& st, uint32_t pic, float baseNew, bool freeToChange, F thinFor, uint32_t unseenPics = 3) {
    if (!(st.base > 0.0f) || pic - st.pic > unseenPics) st.base = baseNew;
    st.pic = pic;
    const bool o = thinFor(st.base);
    if (st.base == baseNew) return o;
    const bool n = thinFor(baseNew);
    if (o == n || freeToChange) { st.base = baseNew; return n; }
    return o;  // a visible change because the governor moved: not now
}
// The supported shells: how many shells fit the fur's thickness (px) with `gapPx` between neighbours.
inline float Supported(float thicknessPx, float gapPx) { return thicknessPx / gapPx + 1.0f; }

// ---- how many shells ------------------------------------------------------------------------------------------------------------------
inline int RawKeep(int n, float s) {
    if (n <= 2) return n;
    const float k = std::floor(s);
    return k < 2.0f ? 2 : (k > static_cast<float>(n) ? n : static_cast<int>(k));
}
// The fully-drawn rules: 0 none; 1 SOFT (inside FULL units of the camera: n shells, reached by the one-shell steps); 2 HARD (within NEAR units of the
// player: n shells at once, never thinner than the game draws it).
inline int FullRule(float nearestCam, float nearestPlayer, const Params& p) { return nearestPlayer < p.nearDist ? 2 : (nearestCam < p.fullDist ? 1 : 0); }

struct State {
    uint8_t keep = 0;      // applied kept shells (0: never drawn)
    uint16_t wait = 0;     // pictures since the last change
    uint32_t lastPic = 0;  // the picture it was last drawn in
};
// One object, one picture. `pic` is the picture counter, `full` FullRule, `s` the supported shells. Returns the kept shells to draw.
// - first draw, or not drawn for more than `unseenPics` pictures: the target at once (it was not on screen); a HARD full rule: n at once
// - otherwise one shell at most per stepPics pictures, with the hysteresis: k stays while s is in [k - hyst, k + 1 + hyst)
inline int Step(State& st, uint32_t pic, int n, int full, float s, const Params& p, uint32_t unseenPics = 3) {
    if (n <= 2) { st.keep = static_cast<uint8_t>(n); st.lastPic = pic; return n; }
    const int tgt = full ? n : RawKeep(n, s);
    const bool unseen = st.keep == 0 || pic - st.lastPic > unseenPics;
    st.lastPic = pic;
    if (unseen || full >= 2) { st.keep = static_cast<uint8_t>(unseen ? tgt : n); st.wait = 0; return st.keep; }
    if (st.wait < 60000) ++st.wait;
    int k = st.keep;
    if (k > n) k = n;
    int want = k;
    if (full) want = n;
    else if (s < static_cast<float>(k) - p.hyst) want = k - 1;
    else if (s >= static_cast<float>(k) + 1.0f + p.hyst) want = k + 1;
    if (want < 2) want = 2;
    if (want > n) want = n;
    if (want != k && st.wait >= p.stepPics) { k += want > k ? 1 : -1; st.wait = 0; }
    st.keep = static_cast<uint8_t>(k);
    return k;
}

// ---- the shells kept ------------------------------------------------------------------------------------------------------------------
// The sets are NESTED: going from k to k - 1 shells drops exactly ONE shell (it joins the group of the next kept shell above it). The keep order is a
// bisection from both ends: the tip (n - 1), the root (0), the middle, then the middles of the halves ... breadth first, so the groups are evenly sized at
// every k (for 8 shells: 7 0 3 5 1 2 4 6 -> k = 4 keeps 7 0 3 5). The tip is never dropped.
constexpr int kMaxShells = 32;
inline void KeepOrder(int n, uint8_t* order) {  // order[r] = the r-th shell to keep
    if (n > kMaxShells) n = kMaxShells;
    int cnt = 0;
    order[cnt++] = static_cast<uint8_t>(n - 1);
    if (n > 1) order[cnt++] = 0;
    int qlo[2 * kMaxShells], qhi[2 * kMaxShells], qh = 0, qt = 0;  // intervals (lo, hi) of shell indices, both ends already chosen
    if (n > 2) { qlo[qt] = 0; qhi[qt] = n - 1; ++qt; }
    while (qh < qt) {
        const int lo = qlo[qh], hi = qhi[qh]; ++qh;
        if (hi - lo < 2) continue;
        const int mid = (lo + hi + 1) / 2;  // the upper middle: nearer the tip
        order[cnt++] = static_cast<uint8_t>(mid);
        if (qt + 2 <= 2 * kMaxShells) { qlo[qt] = lo; qhi[qt] = mid; ++qt; qlo[qt] = mid; qhi[qt] = hi; ++qt; }
    }
}
inline void KeepFlags(int n, int k, uint8_t* keep) {
    if (n > kMaxShells) n = kMaxShells;
    for (int i = 0; i < n; ++i) keep[i] = 1;
    if (k >= n || n < 2) return;
    uint8_t order[kMaxShells];
    KeepOrder(n, order);
    for (int i = 0; i < n; ++i) keep[i] = 0;
    for (int r = 0; r < k && r < n; ++r) keep[order[r]] = 1;
}
// The first shell of the group a kept shell `hi` heads: the shell after the previous kept one.
inline int GroupLo(const uint8_t* keep, int hi) { int lo = hi; while (lo > 0 && !keep[lo - 1]) --lo; return lo; }

// ---- the alpha merge -------------------------------------------------------------------------------------------------------------------
struct Konst { float r, g, b; };  // K3_R (brightness), K3_G (strand alpha), K3_B (skin alpha), 0..255
inline float Clamp255(float v) { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); }
inline float Opacity(float v255) { const float a = v255 / 255.0f; return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
// The merged constants of shells lo..hi (top = hi), root to tip.
inline Konst Merge(const Konst* layer, int lo, int hi) {
    if (lo >= hi) return layer[hi];
    float tS = 1.0f, tK = 1.0f;  // transmittance of a strand / a skin pixel through the whole group
    for (int m = lo; m <= hi; ++m) {
        tS *= 1.0f - Opacity(layer[m].g + layer[m].b);
        tK *= 1.0f - Opacity(layer[m].b);
    }
    const float aS = 1.0f - tS, aK = 1.0f - tK;
    Konst out;
    out.b = Clamp255(255.0f * aK);
    out.g = Clamp255(255.0f * (aS - aK));
    // brightness: each shell's visible contribution = its strand opacity times the transmittance of the shells above it
    float wsum = 0.0f, rsum = 0.0f, above = 1.0f;
    for (int m = hi; m >= lo; --m) {
        const float a = Opacity(layer[m].g + layer[m].b);
        const float w = a * above;
        wsum += w;
        rsum += w * layer[m].r;
        above *= 1.0f - a;
    }
    out.r = wsum > 1e-6f ? Clamp255(rsum / wsum) : layer[hi].r;
    return out;
}
// The optical depth (sum of -ln(1 - A)) of a shell set's strand opacity / skin opacity: what the merge keeps.
inline float Depth(float opacity) { const float o = opacity > 0.9999f ? 0.9999f : opacity; return -std::log(1.0f - o); }

// ---- census ----------------------------------------------------------------------------------------------------------------------------
constexpr int kBands = 5;
inline int DistBand(float nearest) { return nearest < 1500.0f ? 0 : (nearest < 2500.0f ? 1 : (nearest < 4000.0f ? 2 : (nearest < 7000.0f ? 3 : 4))); }
inline int PxBand(float px) { return px < 1.0f ? 0 : (px < 2.0f ? 1 : (px < 4.0f ? 2 : (px < 8.0f ? 3 : 4))); }
struct Census {
    uint32_t pictures = 0, objects = 0, possible = 0, drawn = 0;
    uint32_t distObj[kBands] = {0, 0, 0, 0, 0}, distLayers[kBands] = {0, 0, 0, 0, 0};
    uint32_t pxObj[kBands] = {0, 0, 0, 0, 0}, pxLayers[kBands] = {0, 0, 0, 0, 0};
    uint32_t savedBy25 = 0;
    void Add(float nearest, float thicknessPx, int n, int drawnLayers, int saved25) {
        ++objects; possible += static_cast<uint32_t>(n); drawn += static_cast<uint32_t>(drawnLayers); savedBy25 += static_cast<uint32_t>(saved25);
        const int d = DistBand(nearest), q = PxBand(thicknessPx);
        ++distObj[d]; distLayers[d] += static_cast<uint32_t>(drawnLayers);
        ++pxObj[q]; pxLayers[q] += static_cast<uint32_t>(drawnLayers);
    }
};
inline int Format(char* buf, size_t cap, const Census& c, int level, double secs) {
    const double pic = c.pictures ? static_cast<double>(c.pictures) : 1.0;
    return snprintf(buf, cap,
                         "furcensus: %.1f s, %u pictures, fur25 level %d: a picture: %.2f fur objects, %.2f shells drawn of %.2f, saved by fur25 %.2f; "
                         "by nearest distance <1500 %.2f/%.2f, <2500 %.2f/%.2f, <4000 %.2f/%.2f, <7000 %.2f/%.2f, farther %.2f/%.2f (objects/shells); "
                         "by fur thickness on screen <1 px %.2f/%.2f, <2 %.2f/%.2f, <4 %.2f/%.2f, <8 %.2f/%.2f, more %.2f/%.2f",
                         secs, static_cast<unsigned>(c.pictures), level, c.objects / pic, c.drawn / pic, c.possible / pic, c.savedBy25 / pic,
                         c.distObj[0] / pic, c.distLayers[0] / pic, c.distObj[1] / pic, c.distLayers[1] / pic, c.distObj[2] / pic, c.distLayers[2] / pic,
                         c.distObj[3] / pic, c.distLayers[3] / pic, c.distObj[4] / pic, c.distLayers[4] / pic,
                         c.pxObj[0] / pic, c.pxLayers[0] / pic, c.pxObj[1] / pic, c.pxLayers[1] / pic, c.pxObj[2] / pic, c.pxLayers[2] / pic,
                         c.pxObj[3] / pic, c.pxLayers[3] / pic, c.pxObj[4] / pic, c.pxLayers[4] / pic);
}

}  // namespace fur25
}  // namespace port
