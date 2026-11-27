// far30 (13 Nov 2026) GXV_FARFIELD: the far-field system for Performance, the pure parts (no game types, no clocks, no threads; host-tested by tests/farfield.cpp).
//
// WHAT IT IS. The owner's direction: the picture stays 960x544; what is far from the camera and small on the screen is given up first and as deeply as needed, what is
// close keeps everything; nothing pops (every change is a fade, a sweep or a rate change); the quality comes back fast when the load allows. This header holds the rules;
// the cuts themselves are the existing ones (platform/include/port/cpulod.h members, perf.cpp's PbGovern, ClippingActorHolder.cpp's far-draw loop with GXV_THINFADE), driven
// by the existing governors (fegov.h; recover30's give-back rules). Nothing here is a second governor.
//
// 1. THE ORDERING RULE (Protected / MayCut / FloorHorizon). A cut never reaches an object inside 25 m of the camera or of the player (the existing cuts all test BOTH: an
//    object must be far from both) while anything farther is still uncut; an object whose projected radius is over 64 px counts as near whatever its distance; Mario, the HUD,
//    collectibles the player may head for, anything that can hit the player, platforms and the ground the player stands on are never cut. The only thing allowed inside 25 m
//    is the LAST-RESORT level (GXV_FARFIELD_LAST=1, governor level 4): the old reaches of the governors' levels 2 and 3 (9 .. 18 m). Hysteresis: a cut object comes back
//    only when it is 6 % nearer than it was cut at (or its projection falls under 56 px), so an object at an edge never flickers.
// 2. THE TIERS (Plan): four continuous horizons, deeper = nearer, driven by one depth number 0 .. 4 that the governors' level (and the walk between two levels) gives:
//      tier 1 RATE   update rate beyond the horizon (far draws held, far poses every 2nd / 3rd / 4th turn with the phase spread, far particles emitted less, far shadow updates)
//      tier 2 LOD    the game's own lower models earlier (GXV_PB_LOD factor), far material lists less often
//      tier 3 DENSITY far plant groups and decoration thinned (GXV_PB_DECOR distance, through the existing fade)
//      tier 4 DRAW   far small objects faded out entirely over a band (a true shorter draw distance for decoration and small props)
//    The horizon of a tier moves through its knots in depth; a deeper tier starts to bite only once the lighter ones have reached their floors.
// 3. THE SWEEP (SweepStep): a horizon moves toward its target at a bounded speed that grows with the distance (a fraction of the horizon a second, at least a floor in metres
//    a second), so the edge of a tier never moves visibly: at 25 m 10 m/s, at 100 m 40 m/s (projected: the same angular rate).
// 4. THE FADE BAND (FadeWeight / HideState): between the horizon and horizon + band an object's weight falls 1 -> 0 (smooth), and it is hidden for good only below 2 %, shown
//    again above 6 %: an object at the edge neither pops nor flickers as the camera moves.
// 5. THE PHASE SPREAD (PoseNow): an actor far enough poses on turns where (turn + phase(id)) % k == 0, phase = id % 12: for k in 2, 3, 4 every actor poses exactly once in k
//    turns and, for ids that are spread over the 12 residues, the same number of actors poses on every turn (no judder in step).
//
// GXV_FARFIELD=0 (default): nothing here is read by the game. GXV_FARFIELD=1: Performance only; the governors' rows 1..3 of the cpulod table are floored at 25 m, the new tiers
// are read by the cuts, GXV_FARFIELD_START gives a stage's opening level, GXV_FARFIELD_LAST=1 allows level 4.
#pragma once
#include <cmath>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace port {
namespace farfield {

constexpr float kFloor = 2500.0f;       // 25 m: no cut reaches inside it except the last resort
constexpr float kInnerFloor = 900.0f;   // the old level 3 reach (PB_ANIM 900): the last resort never goes under it
constexpr float kBigPx = 64.0f;         // a projected radius over this is near
constexpr float kBigPxBack = 56.0f;     // ... and comes back to far only under this (hysteresis)
constexpr float kHyst = 0.06f;
constexpr int kDepthMax = 4;

// ---- 1. the ordering rule ----------------------------------------------------------------------------------------------------------------
enum Flag : uint8_t {
    kMario = 1,        // the player and what he carries
    kHud = 2,          // 2D, layouts, the pointer
    kCollectible = 4,  // coins, star bits, 1-ups, power stars: anything he may head for
    kThreat = 8,       // anything that can hit him: enemies, bosses, hazards, projectiles
    kGround = 16,      // collision he stands on or can stand on: planets, platforms, moving floors
    kPlatform = 32,
};
struct Subject {
    float camD = 0, plD = 0;   // distances of the centre from the camera / the player (game units)
    float radius = 0;          // bounding radius (game units)
    float radiusPx = 0;        // projected radius in pixels of a 544-line picture (distcensus.h RadiusPx)
    uint8_t flags = 0;         // Flag bits
};
inline bool Protected(const Subject& s) { return s.flags != 0; }
// the lowest distance a cut may be asked for
inline float FloorHorizon(float h, bool lastResort) {
    const float f = lastResort ? kInnerFloor : kFloor;
    return h < f ? f : h;
}
// May a cut with this horizon take the object? `was`: it is cut now (hysteresis). Both distances (camera AND player), less the radius, must be beyond the horizon.
inline bool MayCut(const Subject& s, float horizon, bool was, bool lastResort) {
    if (Protected(s)) return false;
    if (s.radiusPx > (was ? kBigPx : kBigPxBack)) return false;
    const float h = FloorHorizon(horizon, lastResort) * (was ? 1.0f : 1.0f + kHyst);
    const float c = s.camD - s.radius, p = s.plD - s.radius;
    return c > h && p > h;
}

// ---- 2. the tiers -----------------------------------------------------------------------------------------------------------------------
struct Knot { float depth, v; };
// piecewise linear through knots (depth ascending); clamped at both ends
template <int N>
inline float Interp(const Knot (&k)[N], float depth) {
    if (depth <= k[0].depth) return k[0].v;
    for (int i = 1; i < N; ++i)
        if (depth <= k[i].depth) return k[i - 1].v + (k[i].v - k[i - 1].v) * (depth - k[i - 1].depth) / (k[i].depth - k[i - 1].depth);
    return k[N - 1].v;
}
// horizons in game units: 30000 = off. Knots at the governors' levels (depth 0..3) and the last resort (depth 4, the only one inside the floor).
constexpr float kOff = 30000.0f;
constexpr Knot kRate[] = {{0, kOff}, {1, 4800}, {2, 3800}, {3, 2500}, {4, 1800}};      // tier 1: poses / held draws / particles beyond
constexpr Knot kDecor[] = {{0, kOff}, {1, kOff}, {2, 6000}, {3, 4500}, {4, 3500}};     // tier 3: plant groups and decoration thinned beyond (GXV_PB_DECOR; the budget's own 5000 is level 0)
constexpr Knot kDraw[] = {{0, kOff}, {1, kOff}, {2, kOff}, {3, 9000}, {4, 5000}};      // tier 4: small decoration and props faded out beyond (horizon, plus the band)
constexpr Knot kLodScale[] = {{0, 1.0f}, {1, 0.75f}, {2, 0.6f}, {3, 0.5f}, {4, 0.4f}};  // tier 2: the factor on the game's LOD distances (never nearer than the floor)
constexpr Knot kShadow[] = {{0, kOff}, {1, 2800}, {2, 2600}, {3, 2500}, {4, 1800}};    // far shadow clip (the budget's own 2800 is level 0); the old table went to 15 m
constexpr float kFadeBand = 2000.0f;                                                       // 20 m (far30 rev 2: wider)
// the pose rate the depth asks for: every n-th turn
inline int PoseN(float depth) { return depth < 1.0f ? 1 : depth < 2.0f ? 2 : depth < 3.0f ? 3 : 4; }
// the particle emission factor beyond the rate horizon
constexpr Knot kPtcl[] = {{0, 1.0f}, {1, 0.7f}, {2, 0.5f}, {3, 0.4f}, {4, 0.3f}};

struct Horizons {
    float rate, decor, draw, lod, shadow, ptcl, fadeBand;
    int poseN;
};
// The horizons at a depth. `lastResort` false: nothing is nearer than the floor (depth is clamped at 3); true: depth up to 4.
inline Horizons Plan(float depth, bool lastResort) {
    const float dmax = lastResort ? static_cast<float>(kDepthMax) : static_cast<float>(kDepthMax - 1);
    if (depth < 0.0f) depth = 0.0f;
    if (depth > dmax) depth = dmax;
    Horizons h;
    h.rate = FloorHorizon(Interp(kRate, depth), lastResort);
    h.decor = FloorHorizon(Interp(kDecor, depth), lastResort);
    h.draw = FloorHorizon(Interp(kDraw, depth), lastResort);
    h.lod = Interp(kLodScale, depth);
    h.shadow = FloorHorizon(Interp(kShadow, depth), lastResort);
    h.ptcl = Interp(kPtcl, depth);
    h.fadeBand = kFadeBand;
    h.poseN = PoseN(depth);
    return h;
}
// The depth the governors' state gives: `from` / `to` are levels (0..4), `frac` the walk's progress (0..1, lockfade.h Fader::Frac). A walk is a straight line in depth.
inline float DepthOf(int from, int to, float frac, bool walking) {
    if (!walking) return static_cast<float>(to < 0 ? 0 : (to > kDepthMax ? kDepthMax : to));
    return static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * (frac < 0 ? 0 : (frac > 1 ? 1 : frac));
}

// ---- 3. the sweep ----------------------------------------------------------------------------------------------------------------------
// One step of a horizon toward its target: at most max(floorMps * 100, frac * cur) game units a second (frac 0.4: 40 % of the distance a second), both ways.
inline float SweepStep(float cur, float target, float dtSec, float fracPerSec = 0.4f, float floorMps = 10.0f) {
    const float ref = cur < target ? cur : target;   // the nearer of the two sets the pace (the visible part)
    float lim = fracPerSec * ref;
    if (lim < floorMps * 100.0f) lim = floorMps * 100.0f;
    const float step = lim * dtSec;
    if (cur < target) return cur + step > target ? target : cur + step;
    return cur - step < target ? target : cur - step;
}
// Outward (a give-back) may be quicker than inward: the owner wants quality back fast. The multiplier applies to both the fraction and the floor.
inline float SweepStepDir(float cur, float target, float dtSec, float outMul = 2.0f) {
    return SweepStep(cur, target, dtSec * (target > cur ? outMul : 1.0f));
}

// ---- 4. the fade band -------------------------------------------------------------------------------------------------------------------
inline float FadeWeight(float d, float horizon, float band) {
    if (d <= horizon) return 1.0f;
    if (d >= horizon + band) return 0.0f;
    const float x = (d - horizon) / band;
    return 1.0f - x * x * (3.0f - 2.0f * x);
}
constexpr float kHideBelow = 0.02f, kShowAbove = 0.06f;
// hidden-for-good state of an object at the edge: hides below 2 %, shows again above 6 % (so it cannot flicker while its weight hovers)
struct HideState {
    bool hidden = false;
    bool Update(float w) {
        if (hidden) { if (w > kShowAbove) hidden = false; }
        else if (w < kHideBelow) hidden = true;
        return hidden;
    }
};

// ---- 5. the phase spread --------------------------------------------------------------------------------------------------------------
constexpr int kPhases = 12;   // divisible by 2, 3 and 4
inline int Phase(uint32_t id) { return static_cast<int>(id % kPhases); }
inline bool PoseNow(uint32_t turn, uint32_t id, int k) {
    if (k <= 1) return true;
    return (turn + static_cast<uint32_t>(Phase(id))) % static_cast<uint32_t>(k) == 0;
}
// the longest an actor goes without a pose at rate k (turns)
inline int MaxStale(int k) { return k <= 1 ? 0 : k - 1; }

// ---- far3: the Quality ladder's far-field steps 3h..3l (platform/include/port/q30lock.h). One carrier each, the same ones the Performance tiers use, swept in by a weight w 0..1 (the ladder's ramp,
// 4 s). w = 0 is OFF (0 = no horizon). The horizon moves from far (kOff) to its target geometrically, so its speed is a fixed share of its own distance (ln(kOff / target) / T a second): the
// far end goes first, slowly where objects are dense and quickly where there are none, and no object is ever cut at once (the draw step fades through THINFADE as well).
namespace qfar {
constexpr float kQRate = 6000.0f;    // 3h: calc-anim of far models, characters and marked families every 2nd turn beyond 60 m (the budget's PB_ANIM rule)
constexpr float kQDecor = 4500.0f;   // 3j: flower / bush / plant groups not drawn beyond 45 m (PB_DECOR; 3f's 70 m is the step before)
constexpr float kQDraw = 9000.0f;    // 3k: tiny decoration (under GXV_FARFIELD_PX tall) faded out beyond 90 m (the draw tier; edge spread over kFadeBand): removal is the last resort
constexpr float kQLogic = 10000.0f;  // 3l: ambient actors at half pace beyond 100 m (farlogic, only decoration and honey bees)
constexpr float kQLodScale = 0.6f;   // 3i: the game's LOD distances x0.6 (the budget's own, 3g's 0.8 is the step before)
inline float Sweep(float target, float w) {
    if (!(w > 0.0f)) return 0.0f;
    const float t = target < kFloor ? kFloor : target;
    if (w >= 1.0f) return t;
    return t * std::pow(kOff / t, 1.0f - w);
}
inline float LodScale(float w) { return !(w > 0.0f) ? 1.0f : (w >= 1.0f ? kQLodScale : 1.0f + (kQLodScale - 1.0f) * w); }
// the largest relative speed of a horizon (share of its distance a second) over a sweep of `seconds`
inline float MaxRelSpeed(float target, float seconds) { return std::log(kOff / (target < kFloor ? kFloor : target)) / seconds; }
}  // namespace qfar

// ---- the census line (every 5 s, perf.cpp) -----------------------------------------------------------------------------------------------
// far: <stage>: depth D (level L), horizon decor X m, anim Y m, lod Z, draw W m (+band B m); held draws N, faded objects N, re-posed 1-in-k ...; moved in K times, out K times
struct Census {
    float depth = 0;
    int level = 0, poseN = 1;
    uint32_t heldDraws = 0, fadedObjects = 0, movedIn = 0, movedOut = 0;
    float decor = 0, anim = 0, lod = 1, draw = 0, band = 0;
    uint32_t poseLeftOut = 0;
    uint32_t pops = 0;      // objects that changed between drawn and not drawn in the window
    float popPx = 0;        // the tallest such object, pixels of 544
    float secs = 5.0f;
};
inline int Format(char* buf, size_t n, const char* stage, const Census& c) {
    auto m = [](float u) { return u >= kOff ? -1.0 : static_cast<double>(u) / 100.0; };
    char d[4][24];
    const float v[4] = {c.decor, c.anim, 0, c.draw};
    for (int i = 0; i < 4; ++i) {
        if (i == 2) { snprintf(d[i], sizeof d[i], "x%.2f", static_cast<double>(c.lod)); continue; }
        if (m(v[i]) < 0) snprintf(d[i], sizeof d[i], "off");
        else snprintf(d[i], sizeof d[i], "%.0f", m(v[i]));
    }
    return snprintf(buf, n, "far: %s: depth %.2f (level %d), horizon decor %s m, anim %s m, lod %s, draw %s m (band %.0f m); held draws %u, faded objects %u, re-posed 1-in-%d (%u left out); moved in %u times, out %u times; pop events %.1f a second, largest %.0f px",
                    stage, static_cast<double>(c.depth), c.level, d[0], d[1], d[2], d[3], static_cast<double>(c.band) / 100.0, static_cast<unsigned>(c.heldDraws),
                    static_cast<unsigned>(c.fadedObjects), c.poseN, static_cast<unsigned>(c.poseLeftOut), static_cast<unsigned>(c.movedIn), static_cast<unsigned>(c.movedOut),
                    static_cast<double>(c.secs > 0.0f ? static_cast<float>(c.pops) / c.secs : 0.0f), static_cast<double>(c.popPx));
}

}  // namespace farfield
}  // namespace port
