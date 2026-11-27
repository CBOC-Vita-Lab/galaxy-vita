// popin24 (10 Nov): pop-in audit. The pure parts, header only so a host test can link them (tests/popin.cpp, tools/test_popin.sh).
//
//   GXV_POPIN_LOG=1     the census: a "popin:" basic-log line about every 10 s with what the PORT's own speed switches (and, for
//                       comparison, the game's own clipping) made appear or disappear, how many of them inside the picture and the
//                       largest projected size; "popin-top:" lines name the largest. Default 0 (nothing is computed).
//   GXV_POPIN_HYST=<%>  default 0. Hysteresis of the port's switches (GXV_PB_LOD, _DECOR, _SHADOW, _FUR; the game's own LodCtrl
//                       switch too): a thing that went far comes back only <%> of its distance nearer than it left, so a camera or
//                       player moving to and fro across a threshold does not flicker it. The particle thinning (GXV_PB_PTCL) ramps
//                       over 4 x <%> of its distance instead of switching. Suggested: 8.
//   GXV_POPIN_ZOOM=0|1|2 default 0. The handheld zoom (1.0 .. 2.0) narrows the field of view, so a thing at a given distance is drawn
//                       up to twice as large as the distances were tuned for. 1: the port's own distances (the PB_* ones) are multiplied
//                       by the zoom ratio (tan(game fovy / 2) / tan(picture fovy / 2), 1 when not zoomed); 2: also the game's own
//                       LodCtrl distances (a Middle / Low model takes over at a distance seen at the same size as at zoom 1.0).
//   GXV_POPIN_CLIPPAD=<%> default 0. The side planes of the game's clipping frustum (and of the picture frustum the zoom cull uses) are
//                       moved out by <%> of the view (the tangents of the half angles times 1 + <%>/100), so an actor whose clipping
//                       sphere is a little smaller than its model is running and drawn before its edge shows. Costs the actors in the
//                       extra ring (their update and draw). Suggested: 10.
//
// popin25 (11 Nov): fly-over pop-ins (the opening camera tour, the flights between planets).
//   GXV_POPIN_CENSUS=1  the census by CAUSE (the game's clipping by frustum edge or by a far level, a LOD swap, each budget rule, a draw the
//                       renderer skipped), per 5 s window and per flight (demo / opening camera / far fast camera), with the largest pops'
//                       names, screen position and height in pixels. Implies GXV_POPIN_LOG's hooks; prints "popin25:" lines instead of "popin:".
//   GXV_PB_DEMO=0|1|2  default 0. During a flight (a demo, the opening camera, the starter fly-in, the power-star demo, or a fast camera far
//                       from the player): 1 = the performance budget's distances are widened by GXV_PB_DEMO_WIDEN (default 1.5; PB_LOD's
//                       factor moves that much toward 1) and LodCtrl measures from the nearer of the player and the camera; 2 = the budget
//                       is off for the flight (and the same camera-centred LOD distance).
//   GXV_POPIN_LEAD=<turns> default 0. A fast camera appears things earlier: every budget distance (decor, shadow, fur, particle, anim) and the
//                       game's own clipping spheres grow by the camera's speed (units per turn, peak-held) times <turns>, at most a quarter of
//                       the distance (clipping spheres: GXV_POPIN_LEAD_MAX units, default 600), and a TURNING camera pads the clipping frustum's
//                       sides by its turn rate (radians a turn) times <turns> / tan(half fov), at most GXV_POPIN_LEAD_PAD (default 0.25).
//                       Suggested: 6.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/hot_knob.h"

namespace port {
namespace popin {

// ---- switches (read once, PORT_HOT_KNOB) ---------------------------------------------------------------------------------------
inline int LogOn() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_POPIN_LOG");
        const char* c = std::getenv("GXV_POPIN_CENSUS");  // (popin25: the census by cause also needs the hooks)
        return (e && *e && std::strtol(e, nullptr, 0) != 0) || (c && *c && std::strtol(c, nullptr, 0) != 0) ? 1 : 0;
    }());
}
// popin25: the census by cause (1) instead of the popin24 10 s line
inline int CensusOn() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_POPIN_CENSUS");
        return e && *e && std::strtol(e, nullptr, 0) != 0 ? 1 : 0;
    }());
}
// popin25: GXV_PB_DEMO 0 / 1 / 2 (see the top of this file)
inline int DemoMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_PB_DEMO");
        const long v = e && *e ? std::strtol(e, nullptr, 0) : 0;
        return static_cast<int>(v < 0 ? 0 : (v > 2 ? 2 : v));
    }());
}
// popin25: how much a flight widens the budget's distances (1 .. 4)
inline float DemoWiden() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_PB_DEMO_WIDEN");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 1.5f;
        return v < 1.0f ? 1.0f : (v > 4.0f ? 4.0f : v);
    }());
}
// popin25: GXV_POPIN_LEAD, the pre-appear lead in game turns (0 = off, at most 120)
inline float LeadTurns() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_POPIN_LEAD");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 0.0f;
        return v > 0.0f ? (v < 120.0f ? v : 120.0f) : 0.0f;
    }());
}
// popin25: the most a clipping sphere grows by the lead, in units
inline float LeadClipMax() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_POPIN_LEAD_MAX");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 600.0f;
        return v < 0.0f ? 0.0f : (v > 3000.0f ? 3000.0f : v);
    }());
}
// popin25: a camera is a "far fast camera" flight when it moves at least this many units a turn (default 60; Mario runs 15-25) ...
inline float FlightSpeedMin() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_PB_DEMO_SPEED");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 60.0f;
        return v > 1.0f ? v : 1.0f;
    }());
}
// ... and is at least this far from the player (default 2500)
inline float FlightFarCam() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_PB_DEMO_FARCAM");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 2500.0f;
        return v > 100.0f ? v : 100.0f;
    }());
}
// percent, 0 = off; at most 30
inline float HystPct() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_POPIN_HYST");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 0.0f;
        return v > 0.0f ? (v < 30.0f ? v : 30.0f) : 0.0f;
    }());
}
inline int ZoomMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_POPIN_ZOOM");
        const long v = e && *e ? std::strtol(e, nullptr, 0) : 0;
        return static_cast<int>(v < 0 ? 0 : (v > 2 ? 2 : v));
    }());
}
// the factor the clipping frustum's half-angle tangents are multiplied by (1 = off); at most 1.5
inline float ClipPadFactor() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_POPIN_CLIPPAD");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 0.0f;
        return 1.0f + (v > 0.0f ? (v < 50.0f ? v : 50.0f) : 0.0f) * 0.01f;
    }());
}

// ---- pure helpers -------------------------------------------------------------------------------------------------------------
// The distance of the band a thing that has gone far must come back through: limit * pct / 100.
inline float Band(float limit, float pct) { return limit * pct * 0.01f; }

// Far / near with hysteresis: far when d > limit; a thing already far stays far until d <= limit - band. band 0: plain d > limit.
inline bool FarHyst(bool wasFar, float d, float limit, float band) { return d > (wasFar ? limit - band : limit); }

// 1 at d <= start, `lo` at d >= start + span, linear between (span <= 0: a step at start).
inline float Ramp(float d, float start, float span, float lo) {
    if (d <= start) return 1.0f;
    if (!(span > 0.0f) || d >= start + span) return lo;
    return 1.0f + (lo - 1.0f) * ((d - start) / span);
}

// The zoom ratio: the game's tangent of the half field of view over the picture's (>= 1; 1 not zoomed), clamped to [1, 2.5].
inline float ZoomRatio(float fovyGameDeg, float fovyPictureDeg) {
    const float k = 0.00872664626f;  // pi / 360
    const float a = std::tan(fovyGameDeg * k), b = std::tan(fovyPictureDeg * k);
    if (!(a > 0.0f) || !(b > 0.0f)) return 1.0f;
    const float r = a / b;
    return r < 1.0f ? 1.0f : (r > 2.5f ? 2.5f : r);
}

// LodCtrl's distance chain (the part after the view group's forcing): 0 High, 1 Middle, 2 Low, -1 nothing changes (a LodCtrl with a
// Middle but no Low model past the Low distance). `shown` is the level now shown; with hystPct > 0 a model already at a farther level
// than a distance's own returns popin-pct nearer than the distance. hystPct 0: the game's chain exactly.
inline int LodLevel(float distance, float middle, float low, bool hasMiddle, bool hasLow, int shown, float hystPct) {
    float midCmp = middle, lowCmp = low;
    if (hystPct > 0.0f) {
        if (shown >= 1) midCmp -= Band(middle, hystPct);
        if (shown >= 2) lowCmp -= Band(low, hystPct);
    }
    if (distance < midCmp) return 0;
    if (!hasMiddle && distance < lowCmp) return 0;
    if (hasMiddle && distance < lowCmp) return 1;
    if (hasLow) return 2;
    return -1;
}

// A sphere's projected diameter as a fraction of the picture's height (camera depth `depth`, vertical field of view `fovyDeg`).
inline float SizeFrac(float radius, float depth, float fovyDeg) {
    const float t = std::tan(fovyDeg * 0.00872664626f);
    if (!(depth > 1.0f) || !(t > 0.0f)) return 0.0f;
    return radius / (depth * t);
}

// ---- the census ---------------------------------------------------------------------------------------------------------------
enum Kind : int {
    kOrigClip = 0,  // the game's own clipping (a far-clip level or the frustum) started / ended: the original game does this too
    kLodOrig,       // a LodCtrl model swap at a distance the game itself has (the unscaled decision agrees)
    kLodPort,       // a LodCtrl swap the port's earlier distances (GXV_PB_LOD) made (the unscaled decision would still show the other model)
    kDecor,         // a flower / bush group clipped / unclipped by GXV_PB_DECOR
    kShadow,        // a shadow far-clipped / returned by GXV_PB_SHADOW
    kFur,           // a fur's layers thinned / restored by GXV_PB_FUR
    kZoomCull,      // a model not drawn / drawn again by GXV_ZOOM_DRAWCULL (never inside the picture by construction)
    kOrigFar,       // (popin25) the game's own clipping by a FAR LEVEL (5000 .. 60000 from the camera plane), the frustum's sides would show it
    kFarSpeck,      // (popin31) a model stamped not drawn / drawn again by the far-speck cut (GXV_FARDRAW, its far-field tier; GXV_FARDRAW_KINDS 31 takes enemies too)
    kPieceThin,     // (popin31) a far star bit stamped not drawn / drawn again by GXV_PIECETHIN
    kPlantReveal,   // (fix31) a member of a far plant group the budget gave back that C185's split reveal (GXV_HONEY_PLANT_REVEAL) holds back one picture
    kKinds
};
inline const char* KindName(int k) {
    static const char* const n[kKinds] = {"orig-clip", "lod-orig", "lod-port", "decor", "shadow", "fur", "zoomcull", "orig-far", "far-speck", "piece-thin", "plant-reveal"};
    return k >= 0 && k < kKinds ? n[k] : "?";
}
// (popin31) the port's own kinds: the ones the invisibility floor (GXV_PB_MINPX) and the release after a flight act on
inline bool PortKind(int k) { return k == kLodPort || k == kDecor || k == kShadow || k == kFur || k == kZoomCull || k == kFarSpeck || k == kPieceThin || k == kPlantReveal; }

// popin31b: the kinds of the invisibility floor (GXV_PB_MINPX_<KIND>; the knobs are below with GXV_PB_MINPX)
#ifndef POPIN_MUTANT
#define POPIN_MUTANT 0   // (host tests only: tools/test_popin.sh builds the mutants with -DPOPIN_MUTANT=n; 0 in every game build)
#endif
enum FloorKind : int { kFlShadow = 0, kFlDecor, kFlFur, kFlLod, kFlSpeck, kFlStarBit, kFlParticle, kFloorKinds };
inline const char* FloorKindName(int fk) {
    static const char* const n[kFloorKinds] = {"shadow", "decor", "fur", "lod", "speck", "starbit", "particle"};
    return fk >= 0 && fk < kFloorKinds ? n[fk] : "?";
}
inline const char* FloorKindEnv(int fk) {
    static const char* const n[kFloorKinds] = {"GXV_PB_MINPX_SHADOW", "GXV_PB_MINPX_DECOR", "GXV_PB_MINPX_FUR", "GXV_PB_MINPX_LOD", "GXV_PB_MINPX_SPECK",
                                               "GXV_PB_MINPX_STARBIT", "GXV_PB_MINPX_PARTICLE"};
    return fk >= 0 && fk < kFloorKinds ? n[fk] : "";
}
// The floor kind of a census kind (the cut that made the event); -1: no floor acts on it (the game's own kinds, the zoom cull)
inline int FloorKindOf(int kind) {
    switch (kind) {
    case kShadow: return kFlShadow;
    case kDecor: return kFlDecor;
    case kPlantReveal: return POPIN_MUTANT == 2 ? kFlShadow : kFlDecor;   // (the reveal acts on the plant groups the decor cut gave back)
    case kFur: return kFlFur;
    case kLodPort: return kFlLod;
    case kFarSpeck: return kFlSpeck;
    case kPieceThin: return kFlStarBit;
    default: return -1;
    }
}
struct Event {
    int kind = 0;
    bool appear = false;   // true: hidden -> visible / thin -> full / far model -> near model; false: the reverse
    bool inFrame = false;  // the centre is inside the picture (|ndc| <= 1, in front)
    bool core = false;     // and well inside it (|ndc| < 0.8)
    float ndcX = 0, ndcY = 0, depth = 0, size = 0;  // size: projected diameter / picture height (the radius is the clipping sphere's or the model's)
    char name[28] = {0};
    uint8_t flight = 0;    // (popin25) the flight kind at the moment (Flight; 0 none)
    uint8_t gov = 0;       // (popin31) 1: within kGovWindow turns after a governor level change (the change may be what made it pop)
    float unit = -1.0f;    // (popin31) the size of ONE element as the floor measures it (a flower of a group; else = size); < 0: = size
};
// (popin31) turns after a governor level change in which a port-made pop is attributed to it
constexpr int kGovWindow = 30;

// A pop is "visible" from this projected size (2 % of the picture's height: ~11 px at 544 rows).
constexpr float kVisibleSize = 0.02f;

struct Census {
    uint32_t appIn[kKinds] = {}, appCore[kKinds] = {}, appVis[kKinds] = {}, appOut[kKinds] = {};
    uint32_t hideIn[kKinds] = {}, hideVis[kKinds] = {}, hideOut[kKinds] = {};
    Event top[3];  // the largest in-frame appearances of the window
    int nTop = 0;
    uint32_t turns = 0;
    // popin31: per kind the largest in-frame appearance (fraction of the height), the in-frame appearances whose ONE element is at or above the
    // floor (floorFrac: GXV_PB_MINPX / picture rows; 0 = the floor is off, not counted), and the port-made ones within kGovWindow turns of a
    // governor level change
    float maxSize[kKinds] = {};
    uint32_t appFloor[kKinds] = {};
    uint32_t appGov = 0;
    float floorFrac = 0.0f;
    // popin31b: the floor of each kind's own cut (GXV_PB_MINPX_<KIND> / rows); < 0: floorFrac (the game's own kinds, and every kind as in popin31)
    float floorFracK[kKinds] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    static_assert(kKinds == 11, "popin31b: floorFracK's initialiser has one -1 per kind");
    float FloorFracOf(int k) const { return POPIN_MUTANT != 5 && floorFracK[k] >= 0.0f ? floorFracK[k] : floorFrac; }   // (mutant 5: per kind ignored)

    void Add(const Event& e) {
        const int k = e.kind >= 0 && e.kind < kKinds ? e.kind : 0;
        if (e.appear) {
            if (!e.inFrame) { ++appOut[k]; return; }
            ++appIn[k];
            if (e.core) ++appCore[k];
            if (e.size >= kVisibleSize) ++appVis[k];
            if (e.size > maxSize[k]) maxSize[k] = e.size;
            const float ff = FloorFracOf(k);
            if (ff > 0.0f && (e.unit >= 0.0f ? e.unit : e.size) >= ff) ++appFloor[k];
            if (e.gov && PortKind(k)) ++appGov;
            // keep the three largest
            if (nTop < 3) {
                top[nTop++] = e;
            } else {
                int small = 0;
                for (int i = 1; i < 3; ++i)
                    if (top[i].size < top[small].size) small = i;
                if (e.size > top[small].size) top[small] = e;
            }
        } else {
            if (!e.inFrame) { ++hideOut[k]; return; }
            ++hideIn[k];
            if (e.size >= kVisibleSize) ++hideVis[k];
        }
    }
    void Reset() {
        const Census keep = *this;
        *this = Census();
        floorFrac = keep.floorFrac;
        for (int k = 0; k < kKinds; ++k) floorFracK[k] = keep.floorFracK[k];
    }
    // popin31b: set the per-kind floors from the floor kinds' px (px[fk] <= 0: that kind's cut has no floor: nothing of it is counted at or above one)
    void SetKindFloors(const float (&px)[kFloorKinds], float rows) {
        for (int k = 0; k < kKinds; ++k) {
            const int fk = FloorKindOf(k);
            floorFracK[k] = fk >= 0 ? (px[fk] > 0.0f ? px[fk] / rows : 0.0f) : -1.0f;
        }
    }
    uint32_t Sum(const uint32_t (&a)[kKinds]) const { uint32_t s = 0; for (int i = 0; i < kKinds; ++i) s += a[i]; return s; }
    // the port's own kinds (everything but the game's: orig-clip, orig-far and lod-orig)
    uint32_t SumPort(const uint32_t (&a)[kKinds]) const {
        return a[kLodPort] + a[kDecor] + a[kShadow] + a[kFur] + a[kZoomCull] + a[kFarSpeck] + a[kPieceThin] + a[kPlantReveal];
    }
    // the game's own kinds (popin25: orig-far too)
    uint32_t SumGame(const uint32_t (&a)[kKinds]) const { return a[kOrigClip] + a[kOrigFar] + a[kLodOrig]; }

    // "popin: 10 s (601 turns): in-frame appearances 14 (>=2% of the picture: 3; port-made 5 / 1), ..." into buf.
    int Format(char* buf, size_t n, double seconds) const {
        int w = snprintf(buf, n, "popin: %.0f s (%u turns): appearances in the picture %u (visible, >= 2%% of its height: %u; centre well inside: %u)"
                                      "; PORT-made %u (visible %u), game's own %u (visible %u); by kind in/visible/outside:",
                              seconds, turns, Sum(appIn), Sum(appVis), Sum(appCore), SumPort(appIn), SumPort(appVis),
                              SumGame(appIn), SumGame(appVis));
        for (int k = 0; k < kKinds && w > 0 && static_cast<size_t>(w) < n; ++k)
            w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %u/%u/%u (hid %u/%u)", KindName(k), appIn[k], appVis[k], appOut[k], hideIn[k], hideVis[k]);
        return w;
    }
};

// ================================================================ popin25: fly-over pop-ins =============================================
// The kinds of flight (what the game or the camera is doing while the budget's distances are measured).
enum Flight : int {
    kFlightNone = 0,
    kFlightOpening,   // the stage's opening camera tour (GameSceneScenarioOpeningCamera: the player hidden and not moved, the camera flies)
    kFlightStarter,   // the scenario starter's fly-in (the galaxy name, the player descends)
    kFlightDemo,      // a demo (DemoDirector active: the power star's appearance, a Launch Star's appear camera, a boss intro ...)
    kFlightStarGet,   // the power star / grand star get demo
    kFlightFar,       // a fast camera far from the player (a Launch Star flight: the event camera)
    kFlights
};
inline const char* FlightName(int f) {
    static const char* const n[kFlights] = {"none", "opening-camera", "scenario-starter", "demo", "star-get-demo", "far-fast-camera"};
    return f >= 0 && f < kFlights ? n[f] : "?";
}

// What kind of flight: the game's own demo state first (demoKind, a Flight value or 0), else a camera that is fast AND far from the player.
inline int ClassifyFlight(int demoKind, float camSpeed, float camPlayerDist, float speedMin, float farCam) {
    if (demoKind > 0 && demoKind < kFlightFar) return demoKind;
    if (camSpeed >= speedMin && camPlayerDist >= farCam) return kFlightFar;
    return kFlightNone;
}

// The camera's speed in units per game turn, peak-held (a fast camera stays "fast" for a few turns after it slows). A jump of more than
// cutDist between two turns is a camera CUT: it is not a speed, the old value just decays.
struct CamTrack {
    float last[3] = {0, 0, 0};
    float speed = 0.0f;
    bool have = false;
    bool Update(const float pos[3], float cutDist, float decay = 0.95f) {
        bool cut = false;
        if (have) {
            const float dx = pos[0] - last[0], dy = pos[1] - last[1], dz = pos[2] - last[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > cutDist) {
                cut = true;
                speed *= decay;
            } else {
                const float held = speed * decay;
                speed = d > held ? d : held;
            }
        }
        last[0] = pos[0]; last[1] = pos[1]; last[2] = pos[2];
        have = true;
        return cut;
    }
    // The direction the camera looks (a unit vector), once a turn: the turn rate in radians per game turn, peak-held like the speed; a turn of
    // more than cutRad between two turns is a cut.
    float dir[3] = {0, 0, 1};
    float turn = 0.0f;
    bool haveDir = false;
    bool UpdateDir(const float d[3], float cutRad = 0.61f, float decay = 0.95f) {
        bool cut = false;
        if (haveDir) {
            const float n = std::sqrt((d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]));
            float c = n > 0.0f ? (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / n : 1.0f;
            c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
            const float a = std::acos(c);
            if (a > cutRad) {
                cut = true;
                turn *= decay;
            } else {
                const float held = turn * decay;
                turn = a > held ? a : held;
            }
        }
        dir[0] = d[0]; dir[1] = d[1]; dir[2] = d[2];
        haveDir = true;
        return cut;
    }
    void Reset() { have = false; speed = 0.0f; haveDir = false; turn = 0.0f; }
};
// GXV_POPIN_LEAD for a TURNING camera: the clipping frustum's side tangents grow by (turn rate in radians a turn x lead turns) / tan(half fov), at
// most `cap` (0.25 = 25 %): what the sides sweep in `lead` turns, so an object the view is turning toward is already running when it enters.
inline float LeadFramePad(float turnRate, float leadTurns, float fovyDeg, float cap) {
    if (!(leadTurns > 0.0f) || !(turnRate > 0.0f)) return 0.0f;
    const float t = std::tan(fovyDeg * 0.00872664626f);
    if (!(t > 0.0f)) return 0.0f;
    const float p = turnRate * leadTurns / t;
    return p < cap ? p : cap;
}
// the cap of that pad (GXV_POPIN_LEAD_PAD, default 0.25, at most 1)
inline float LeadPadMax() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_POPIN_LEAD_PAD");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 0.25f;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }());
}

// GXV_PB_DEMO: the budget is off for this turn (mode 2 in a flight)
inline bool BudgetOffInFlight(int mode, int flight) { return mode >= 2 && flight != kFlightNone; }
// GXV_PB_DEMO: the factor the budget's distances are widened by (mode 1 in a flight: widen; else 1)
inline float FlightWiden(int mode, int flight, float widen) { return mode == 1 && flight != kFlightNone && widen > 1.0f ? widen : 1.0f; }
// GXV_PB_DEMO: the distance LodCtrl compares: from the player (the game's), in a flight (mode >= 1) from the nearer of the player and the camera
inline float FlightLodDistance(int mode, int flight, float distPlayer, float distCamera) {
    return mode >= 1 && flight != kFlightNone && distCamera < distPlayer ? distCamera : distPlayer;
}
// PB_LOD's factor in a flight: the earlier swap is taken back by `widen` toward 1 (0.6 * 1.5 = 0.9); `off`: 1
inline float FlightLodScale(float pbScale, float widen, bool off) {
    if (off) return 1.0f;
    const float v = pbScale * widen;
    return v < 1.0f ? v : 1.0f;
}
// GXV_POPIN_LEAD: a thing that a fast camera will reach in `lead` turns appears that much earlier. limit: the distance it is judged against.
// At most a quarter of the distance; 0 when the lead is off or the camera is slow.
inline float LeadMargin(float camSpeed, float leadTurns, float limit) {
    if (!(leadTurns > 0.0f) || !(camSpeed > 0.0f) || !(limit > 0.0f)) return 0.0f;
    const float m = camSpeed * leadTurns, cap = limit * 0.25f;
    return m < cap ? m : cap;
}
// the same for a clipping sphere's radius: at most clipMax units
inline float LeadClipMargin(float camSpeed, float leadTurns, float clipMax) {
    if (!(leadTurns > 0.0f) || !(camSpeed > 0.0f)) return 0.0f;
    const float m = camSpeed * leadTurns;
    return m < clipMax ? m : clipMax;
}

// ---- the census by cause --------------------------------------------------------------------------------------------------------
// One counter set per flight run (and one per 5 s window). Pops are appearances ON the picture (the centre projects inside it).
struct FlightCensus {
    Census c;                    // (the appearance / hide counters and the three largest of the span)
    uint32_t renderSkips = 0;    // draws the renderer left out for a program that was not ready (a skipped draw IS a pop), whole picture
    uint32_t noModel = 0;        // clip events of actors without a model (sound sources, area objects: invisible, not counted as pops)
    uint32_t turnsFlight = 0;    // turns of the span that were in a flight
    uint32_t turnsAll = 0;
    int lastFlight = 0;
    float speedMax = 0.0f, camPlayerMax = 0.0f;
    // popin31 (B0): what the census did not count before
    uint32_t turnsRelease = 0;                 // turns of the span in the release after a flight (GXV_PB_DEMO=2)
    uint32_t govChanges = 0;                   // governor level changes (front end, game thread, far-draw) in the span
    uint32_t govFe = 0, govGame = 0, govCpu = 0, govMax = 0;  // the levels last seen, and the largest of any of them in the span
    uint32_t ptclScaled = 0;                   // GXV_PB_PTCL: emitters whose emission was scaled down (creates)
    uint32_t ptclThinDraws = 0, ptclLeftOut = 0;  // GXV_PTCL_THIN: emitter draws thinned, particles left out
    uint32_t floorKept[kKinds] = {};           // GXV_PB_MINPX: object-turns a cut wanted but the floor kept (it was at or above the floor)
    uint32_t releaseHeld[kKinds] = {};         // GXV_PB_DEMO=2: object-turns a cut wanted to START on a thing in the picture after a flight and the release held
    // popin31b: the price of what the floor gives back. costN / costNs: of the kept things, the ones whose work was timed (a sample) and that work's
    // nanoseconds (shadow: its update, the ground probe and gravity, the memo's answer included; decor: the group's control, its members' movement;
    // fur: the shells the cut would have left out, as their share of the timed shell loop); ptclFloorKept: emitters the particle floor kept from
    // thinning (creates and draws); shadowMemoHit / shadowMemoLook: of the kept shadows' projection casts, the ones the memo (GXV_SHADOWMEMO) answered
    uint32_t costN[kFloorKinds] = {};
    uint64_t costNs[kFloorKinds] = {};
    uint32_t ptclFloorKept = 0;
    uint32_t shadowMemoHit = 0, shadowMemoLook = 0;
    void Reset() {
        const Census keep = c;
        *this = FlightCensus();
        c.floorFrac = keep.floorFrac;
        for (int k = 0; k < kKinds; ++k) c.floorFracK[k] = keep.floorFracK[k];
    }
    // popin31b: the object-turns the floor of kind fk kept (from the census kinds it covers; particles: their own count)
    uint32_t KeptOf(int fk) const {
        if (fk == kFlParticle) return ptclFloorKept;
        uint32_t s = 0;
        for (int k = 0; k < kKinds; ++k)
            if (FloorKindOf(k) == fk) s += floorKept[k];
        return s;
    }
    // "popin31b: 5 s [play, 300 turns]: the floor by kind (px: kept object-turns a turn, CPU each from the timed sample, CPU given back a turn): shadow 3.0:
    //  41.2 kept, 0.90 us each (120 timed; ground probe answered by the memo 97 %) = 37.1 us; decor 3.0: ...; fur off; lod 3.0: 3.0 kept (not timed); ...;
    //  CPU given back by the timed kinds 88.0 us a turn"
    int Format31b(char* buf, size_t n, double seconds, const char* label, const float (&px)[kFloorKinds]) const {
        const double turns = turnsAll > 0 ? static_cast<double>(turnsAll) : 1.0;
        int w = snprintf(buf, n, "popin31b: %.0f s [%s, %u turns]: the floor by kind (px: kept object-turns a turn, CPU each from the timed sample, CPU given back a turn):",
                         seconds, label, turnsAll);
        double total = 0.0;
        bool anyUntimed = false;
        for (int fk = 0; fk < kFloorKinds && w > 0 && static_cast<size_t>(w) < n; ++fk) {
            if (!(px[fk] > 0.0f)) {
                w += snprintf(buf + w, n - static_cast<size_t>(w), "%s %s off", fk ? ";" : "", FloorKindName(fk));
                continue;
            }
            const double kept = KeptOf(fk) / turns;
            const bool timedKind = fk == kFlShadow || fk == kFlDecor || fk == kFlFur;
            if (timedKind && costN[fk] > 0) {
                const double each = static_cast<double>(costNs[fk]) / costN[fk] / 1000.0;
                const double back = (POPIN_MUTANT == 6 ? 1.0 : kept) * each;   // (mutant 6: the count of kept things left out of the price)
                total += back;
                w += snprintf(buf + w, n - static_cast<size_t>(w), "%s %s %.1f: %.1f kept, %.2f us each (%u timed", fk ? ";" : "", FloorKindName(fk),
                              static_cast<double>(px[fk]), kept, each, costN[fk]);
                if (fk == kFlShadow && shadowMemoLook > 0 && w > 0 && static_cast<size_t>(w) < n)
                    w += snprintf(buf + w, n - static_cast<size_t>(w), "; ground probe answered by the memo %.0f %%", 100.0 * shadowMemoHit / shadowMemoLook);
                if (w > 0 && static_cast<size_t>(w) < n) w += snprintf(buf + w, n - static_cast<size_t>(w), ") = %.1f us", back);
            } else {
                anyUntimed = anyUntimed || kept > 0.0;
                w += snprintf(buf + w, n - static_cast<size_t>(w), "%s %s %.1f: %.1f kept (%s)", fk ? ";" : "", FloorKindName(fk), static_cast<double>(px[fk]), kept,
                              timedKind ? "none timed" : "not timed: its cost is on the GPU / in the draw");
            }
        }
        if (w > 0 && static_cast<size_t>(w) < n)
            w += snprintf(buf + w, n - static_cast<size_t>(w), "; CPU given back by the timed kinds %.1f us a turn%s", total,
                          anyUntimed ? " (the untimed kinds are priced by their one-kind console arms)" : "");
        return w;
    }
    // "popin31: 5 s [play]: governors fe 2 game 1 far-draw 2 (max 3, 1 change; port-made pops within 30 turns of a change 4); largest pop in px by kind: ...;
    // port-made at or above the floor (3 px, one element): 0 ...; kept by the floor a turn: ...; release: ...; particles: ..."
    // (popin31b: kindPx, when given and not every kind at minPx, names each kind's floor; the line is "on" when any kind is)
    int Format31(char* buf, size_t n, double seconds, const char* label, float minPx, float rows, const float* kindPx = nullptr) const {
        const double turns = turnsAll > 0 ? static_cast<double>(turnsAll) : 1.0;
        bool byKind = false, anyOn = minPx > 0.0f;
        if (kindPx != nullptr) {
            for (int fk = 0; fk < kFloorKinds; ++fk) {
                byKind = byKind || kindPx[fk] != minPx;
                anyOn = anyOn || kindPx[fk] > 0.0f;
            }
        }
        int w = snprintf(buf, n, "popin31: %.0f s [%s, %u turns, %u in a flight, %u in the release after one]: governors fe %u game %u far-draw %u (max %u, %u changes; "
                                 "port-made pops within %d turns of a change %u); largest pop in px of %.0f by kind:",
                         seconds, label, turnsAll, turnsFlight, turnsRelease, govFe, govGame, govCpu, govMax, govChanges, kGovWindow, c.appGov,
                         static_cast<double>(rows));
        for (int k = 0; k < kKinds && w > 0 && static_cast<size_t>(w) < n; ++k)
            if (c.maxSize[k] > 0.0f) w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %.0f", KindName(k), static_cast<double>(c.maxSize[k] * rows));
        if (w > 0 && static_cast<size_t>(w) < n) {
            if (anyOn) {
                uint32_t port = 0;
                for (int k = 0; k < kKinds; ++k) if (PortKind(k)) port += c.appFloor[k];
                if (byKind) {
                    w += snprintf(buf + w, n - static_cast<size_t>(w), "; GXV_PB_MINPX by kind");
                    for (int fk = 0; fk < kFloorKinds && w > 0 && static_cast<size_t>(w) < n; ++fk)
                        w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %.1f", FloorKindName(fk), static_cast<double>(kindPx[fk]));
                } else {
                    w += snprintf(buf + w, n - static_cast<size_t>(w), "; GXV_PB_MINPX %.1f", static_cast<double>(minPx));
                }
                if (w > 0 && static_cast<size_t>(w) < n)
                    w += snprintf(buf + w, n - static_cast<size_t>(w), ": port-made pops at or above it (one element) %u, game's own %u; kept by the floor a turn:",
                                  port, c.appFloor[kOrigClip] + c.appFloor[kOrigFar] + c.appFloor[kLodOrig]);
                for (int k = 0; k < kKinds && w > 0 && static_cast<size_t>(w) < n; ++k)
                    if (floorKept[k]) w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %.1f", KindName(k), floorKept[k] / turns);
            } else {
                w += snprintf(buf + w, n - static_cast<size_t>(w), "; GXV_PB_MINPX off");
            }
        }
        if (w > 0 && static_cast<size_t>(w) < n) {
            uint32_t held = 0;
            for (int k = 0; k < kKinds; ++k) held += releaseHeld[k];
            w += snprintf(buf + w, n - static_cast<size_t>(w), "; release holds %u (object-turns)", held);
            for (int k = 0; k < kKinds && w > 0 && static_cast<size_t>(w) < n; ++k)
                if (releaseHeld[k]) w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %u", KindName(k), releaseHeld[k]);
        }
        if (w > 0 && static_cast<size_t>(w) < n)
            w += snprintf(buf + w, n - static_cast<size_t>(w), "; particles: GXV_PB_PTCL emitters scaled %.1f a turn, GXV_PTCL_THIN emitter draws thinned %.1f a turn, "
                                                            "particles left out %.1f a turn",
                          ptclScaled / turns, ptclThinDraws / turns, ptclLeftOut / turns);
        return w;
    }
    // "popin25: 5 s [opening-camera 100 % of 301 turns, camera up to 83 u/turn, 5200 from the player]: pops on screen 12 (centre 5, >= 2 % of the
    // height 9) by cause: orig-clip 3 orig-far 4 lod-orig 0 lod-port 1 decor 2 shadow 1 fur 1 zoomcull 0 render-skip 0 (hid ...)"
    int Format(char* buf, size_t n, double seconds, const char* label) const {
        int w = snprintf(buf, n, "popin25: %.0f s [%s, %u of %u turns in a flight, camera up to %.0f u/turn, up to %.0f from the player]: pops on screen %u "
                                 "(centre well inside %u, >= 2%% of the height %u; PORT-made %u, game's own %u) by cause (on screen/visible):",
                         seconds, label, turnsFlight, turnsAll, static_cast<double>(speedMax), static_cast<double>(camPlayerMax), c.Sum(c.appIn),
                         c.Sum(c.appCore), c.Sum(c.appVis), c.SumPort(c.appIn), c.SumGame(c.appIn));
        for (int k = 0; k < kKinds && w > 0 && static_cast<size_t>(w) < n; ++k)
            w += snprintf(buf + w, n - static_cast<size_t>(w), " %s %u/%u", KindName(k), c.appIn[k], c.appVis[k]);
        if (w > 0 && static_cast<size_t>(w) < n)
            w += snprintf(buf + w, n - static_cast<size_t>(w), " render-skip %u; hidden on screen %u (>= 2%%: %u); off screen %u; no-model clip events ignored %u",
                          renderSkips, c.Sum(c.hideIn), c.Sum(c.hideVis), c.Sum(c.appOut), noModel);
        return w;
    }
};

// The height of a pop in pixels at `rows` picture rows
inline float PixelHeight(float sizeFrac, float rows) { return sizeFrac * rows; }

// ================================================================ popin31 (17 Nov): flights exact, the invisibility floor ===============
//   GXV_PB_MINPX=<px>   default 0 (off). B2, the invisibility floor: a cut of the port's (GXV_PB_DECOR, GXV_PB_SHADOW, GXV_PB_FUR, GXV_PB_LOD's earlier swap,
//                       the far-speck cut GXV_FARDRAW with its far-field tier, GXV_PIECETHIN, and the particle thinning GXV_PB_PTCL / GXV_PTCL_THIN) may hide,
//                       thin or swap a thing only while it projects under <px> pixels on the real picture (the zoomed field of view, 544 rows); at or above
//                       that the game's own behaviour applies whatever the distance says. Measured conservatively: the largest extent a sphere of the
//                       thing's radius can have anywhere in the picture (the corner's stretch included), so "under <px>" is a bound, not an estimate.
//   GXV_PB_MINPX_BAND=<%> default 25. A thing that is shown becomes cuttable only under <px> * (1 - band); a thing already cut stays cut up to <px>.
//   GXV_PB_MINPX_PTCL=<units> default 200. The particle thinning has no per-particle size at the cut: an emitter is thinned only where a particle of
//                       this radius would be under the floor (so in practice not at all at a few pixels).
//   GXV_PB_DEMO=2 now also releases the budget per object after a flight (B1): a cut that would START on a thing inside the picture waits until the
//   thing leaves the picture or the camera cuts; the particle thinning comes back over GXV_PB_DEMO_RAMP turns (default 60).
//   GXV_PB_DEMO_QUIET=<turns> default 8: the release ends after this many turns in a row in which no cut was held.

// ---- switches ----
// (popin31b: the parse of one floor value, shared by GXV_PB_MINPX and the per-kind knobs: 0 / unreadable / negative = off; else 0.5 .. 64)
inline float MinPxParse(const char* e) {
    const float v = e && *e ? static_cast<float>(std::atof(e)) : 0.0f;
    return v > 0.0f ? (v < 0.5f ? 0.5f : (v > 64.0f ? 64.0f : v)) : 0.0f;
}
inline float MinPx() {
    return PORT_HOT_KNOB(float, MinPxParse(std::getenv("GXV_PB_MINPX")));
}
inline float MinPxBand() {  // a fraction, 0 .. 0.5
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_PB_MINPX_BAND");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 25.0f;
        return (v > 0.0f ? (v < 50.0f ? v : 50.0f) : 0.0f) * 0.01f;
    }());
}
inline float MinPxPtclRadius() {
    return PORT_HOT_KNOB(float, [] {
        const char* e = std::getenv("GXV_PB_MINPX_PTCL");
        const float v = e && *e ? static_cast<float>(std::atof(e)) : 200.0f;
        return v < 1.0f ? 1.0f : (v > 100000.0f ? 100000.0f : v);
    }());
}
inline int ReleaseRampTurns() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_PB_DEMO_RAMP");
        const long v = e && *e ? std::strtol(e, nullptr, 0) : 60;
        return static_cast<int>(v < 1 ? 1 : (v > 600 ? 600 : v));
    }());
}
inline int ReleaseQuietTurns() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = std::getenv("GXV_PB_DEMO_QUIET");
        const long v = e && *e ? std::strtol(e, nullptr, 0) : 8;
        return static_cast<int>(v < 1 ? 1 : (v > 600 ? 600 : v));
    }());
}

// ---- popin31b (17 Nov): the floor BY KIND ----
//   GXV_PB_MINPX_SHADOW, _DECOR (flower / bush groups), _FUR, _LOD (PB_LOD's earlier swap), _SPECK (the far-speck cut and its far-field tier), _STARBIT
//   (GXV_PIECETHIN), _PARTICLE (GXV_PB_PTCL and GXV_PTCL_THIN) = <px>: the floor of that kind of cut alone. Unset: GXV_PB_MINPX's value, so the single
//   knob of popin31 is still the default of every kind; 0: that kind has no floor (its cut acts as with GXV_PB_MINPX unset). The band
//   (GXV_PB_MINPX_BAND) and the particle's size (GXV_PB_MINPX_PTCL, in units: not a floor) are shared. So the cheapest, most visible kinds can be
//   switched on first: GXV_PB_MINPX_SHADOW=3 alone floors the shadows and nothing else.
// The census prints a "popin31b:" line per window with, per kind, its floor, the object-turns it kept, and (shadow, decor, fur) the CPU of that
// work measured on a sample of the kept things (the price of what the floor gives back; GPU time is priced by the one-kind console arms).
// One kind's floor from its own knob's text (null / empty: unset) and GXV_PB_MINPX's value: unset inherits the base, a set value (0 included) is its own
inline float MinPxResolve(const char* kindValue, float base) {
    if (POPIN_MUTANT == 1) return kindValue && *kindValue ? MinPxParse(kindValue) : 0.0f;   // (mutant: an unset kind does not inherit)
    return kindValue && *kindValue ? MinPxParse(kindValue) : base;
}
inline float MinPxKind(int fk) {
    switch (fk) {
    case kFlShadow: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_SHADOW"), MinPx()));
    case kFlDecor: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_DECOR"), MinPx()));
    case kFlFur: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_FUR"), MinPx()));
    case kFlLod: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_LOD"), MinPx()));
    case kFlSpeck: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_SPECK"), MinPx()));
    case kFlStarBit: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_STARBIT"), MinPx()));
    case kFlParticle: return PORT_HOT_KNOB(float, MinPxResolve(std::getenv("GXV_PB_MINPX_PARTICLE"), MinPx()));
    default: return 0.0f;
    }
}
// Is any kind's floor on (the base knob or a kind's own)?
inline bool AnyFloorOn() {
    for (int k = 0; k < kFloorKinds; ++k)
        if (MinPxKind(k) > 0.0f) return true;
    return MinPx() > 0.0f;
}

// ---- B2: the floor (pure) ----
constexpr float kPictureRows = 544.0f;             // the real picture (the Vita's screen; a smaller render target only has fewer pixels)
constexpr float kPictureAspect = 960.0f / 544.0f;   // its width / height (a 4:3 picture is narrower: its corners stretch less)
// How a sphere's size on the picture is bounded. A sphere of radius r whose centre is d from the eye has the angular radius a = asin(r / d). Seen at an angle t
// off the view's axis its outline on the picture plane spans tan(t + a) - tan(t - a) = sin 2a / (cos(t + a) cos(t - a)) along the line from the centre (its
// largest extent). A sphere that touches the picture at all has t <= c + a, c the angle of the picture's corner (tan c = tanHalf * sqrt(1 + aspect^2)), so its
// extent is at most 2 (r / d) / (cos c * cos(c + 2a)) in tangent units, times rows / (2 tanHalfPicture) pixels. The corner is taken at the GAME's (unzoomed)
// field of view, never narrower than the picture's: the bound is then never below the zoomed picture's own and only grows with the zoom (monotone).
inline float CornerCos(float tanHalfGame, float aspect) { return 1.0f / std::sqrt(1.0f + tanHalfGame * tanHalfGame * (1.0f + aspect * aspect)); }
// cos(c + 2 asin(s)), never below cos 89.5 degrees (a sphere that close to the side of the view is a huge one: the floor never lets a cut take it anyway)
inline float CornerCosWide(float cosC, float s) {
    const float c = std::acos(cosC > 1.0f ? 1.0f : cosC);
    const float a = c + 2.0f * std::asin(s < 1.0f ? (s > 0.0f ? s : 0.0f) : 1.0f);
    return a < 1.5620696f ? std::cos(a) : 0.0087265f;
}
// An upper bound of the projected size (the largest extent, in pixels) of a sphere of `radius` whose centre is `dist` from the eye, anywhere it touches the
// picture. dist <= radius: the eye is inside it: huge.
inline float ProjPxBound(float radius, float dist, float rows, float tanHalfPicture, float tanHalfGame, float aspect) {
    if (!(dist > radius) || !(tanHalfPicture > 0.0f)) return 1.0e9f;
    const float s = radius / dist;
    const float cosC = CornerCos(tanHalfGame > tanHalfPicture ? tanHalfGame : tanHalfPicture, aspect);
    return s * rows / (tanHalfPicture * cosC * CornerCosWide(cosC, s));
}
// The per-picture constant: a thing of radius r is under minPx (by the bound) whenever its distance exceeds r * k. 0: the floor is off. The bound is
// s * rows / (tanHalfPicture cos c cos(c + 2 asin s)) for s = r / d, increasing in s; it equals minPx at s* = g(s*) with g(s) = minPx tanHalfPicture
// cos c cos(c + 2 asin s) / rows, a decreasing g. So s0 = g(0) >= s* and s1 = g(s0) <= g(s*) = s*: k = 1 / s1 never lets a thing at or above minPx through.
inline float FloorK(float minPx, float rows, float tanHalfPicture, float tanHalfGame, float aspect) {
    if (!(minPx > 0.0f) || !(tanHalfPicture > 0.0f) || !(rows > 0.0f)) return 0.0f;
    const float cosC = CornerCos(tanHalfGame > tanHalfPicture ? tanHalfGame : tanHalfPicture, aspect);
    const float g0 = minPx * tanHalfPicture * cosC / rows;   // (times cos(c + 2 asin s))
    const float s0 = g0 * cosC;
    const float s1 = g0 * CornerCosWide(cosC, s0);
    return s1 > 0.0f ? 1.0f / s1 : 0.0f;
}
// What the cut sites read, once a game turn (LodCtrl.cpp port_popin_turn writes it; port_pb_floor). k == 0: no floor. kNew: for a thing that is shown
// (it must be under minPx * (1 - band)), kHeld: for a thing already cut (it stays cut up to minPx). cam: the camera's position this turn.
struct FloorTurn {
    float kHeld = 0.0f, kNew = 0.0f;   // GXV_PB_MINPX's own (the base knob; popin31b: the sites read their kind's below)
    float cam[3] = {0.0f, 0.0f, 0.0f};
    int releasing = 0;   // B1: the release after a flight is in progress
    int guard = 0;       // any floor (the base's or a kind's) or releasing (port/popin_site.h GuardOn())
    // popin31b: per kind (FloorKind). guardK[k]: that kind's floor or the release: the one word a site of that kind loads when its switches are off
    int guardK[kFloorKinds] = {};
    float kHeldK[kFloorKinds] = {}, kNewK[kFloorKinds] = {};
};
// The shown thing's constant from the held one: it must be under minPx * (1 - band)
inline float FloorKNew(float kHeld, float band) {
    return kHeld > 0.0f ? kHeld / (1.0f - (band < 0.0f ? 0.0f : (band > 0.5f ? 0.5f : band))) : 0.0f;
}
inline void FloorSet(FloorTurn& f, float minPx, float band, float rows, float tanHalfPicture, float tanHalfGame, float aspect) {
    f.kHeld = FloorK(minPx, rows, tanHalfPicture, tanHalfGame, aspect);
    f.kNew = FloorKNew(f.kHeld, band);
}
// popin31b: every kind's constants from its px (px[k] <= 0: that kind has no floor). Kinds with the same px share one FloorK (its acos / asin are
// the costly part: at most one per distinct value, only when a field of view changed).
inline void FloorSetKinds(FloorTurn& f, const float (&px)[kFloorKinds], float band, float rows, float tanHalfPicture, float tanHalfGame, float aspect) {
    for (int i = 0; i < kFloorKinds; ++i) {
        int j = 0;
        while (j < i && px[j] != px[i]) ++j;
        if (POPIN_MUTANT == 4 && i > 0) j = 0;   // (mutant: every kind takes the first kind's constants)
        if (j < i) {
            f.kHeldK[i] = f.kHeldK[j];
            f.kNewK[i] = f.kNewK[j];
            continue;
        }
        f.kHeldK[i] = FloorK(px[i], rows, tanHalfPicture, tanHalfGame, aspect);
        f.kNewK[i] = FloorKNew(f.kHeldK[i], band);
    }
}
inline void FloorClearKinds(FloorTurn& f) {
    for (int i = 0; i < kFloorKinds; ++i) f.kHeldK[i] = f.kNewK[i] = 0.0f;
}
// The guard words from the constants and the release flag (once a turn, after both are set)
inline void FloorGuards(FloorTurn& f) {
    bool any = f.kHeld > 0.0f || f.releasing != 0;
    for (int i = 0; i < kFloorKinds; ++i) {
        f.guardK[i] = (f.kHeldK[i] > 0.0f || (POPIN_MUTANT != 3 && f.releasing != 0)) ? 1 : 0;   // (mutant 3: the release forgotten)
        any = any || f.guardK[i] != 0;
    }
    f.guard = any ? 1 : 0;
}
// May a cut hide (thin, swap) a thing of `radius` whose centre is `dist` from the eye? (dist may be a depth along the view: it is never more than the
// distance, so the answer is only more careful.) wasCut: the cut holds it now. No floor: always.
inline bool FloorMayCut(const FloorTurn& f, float radius, float dist, bool wasCut) {
    if (!(f.kHeld > 0.0f)) return true;
    return dist > radius * (wasCut ? f.kHeld : f.kNew);
}
// The same from a squared distance (no root)
inline bool FloorMayCut2(const FloorTurn& f, float radius, float dist2, bool wasCut) {
    if (!(f.kHeld > 0.0f)) return true;
    const float lim = radius * (wasCut ? f.kHeld : f.kNew);
    return dist2 > lim * lim;
}
// The squared distance inside which a thing of `radius` is never cut (0: no floor; for the particle thinning)
inline float FloorDist2(const FloorTurn& f, float radius) {
    const float lim = radius * f.kNew;
    return f.kHeld > 0.0f ? lim * lim : 0.0f;
}
// popin31b: the same three by kind (fk < 0 or out of range: no floor)
inline bool FloorMayCutK(const FloorTurn& f, int fk, float radius, float dist, bool wasCut) {
    if (fk < 0 || fk >= kFloorKinds || !(f.kHeldK[fk] > 0.0f)) return true;
    if (POPIN_MUTANT == 7) wasCut = !wasCut;   // (mutant: the band the wrong way round)
    return dist > radius * (wasCut ? f.kHeldK[fk] : f.kNewK[fk]);
}
inline bool FloorMayCut2K(const FloorTurn& f, int fk, float radius, float dist2, bool wasCut) {
    if (fk < 0 || fk >= kFloorKinds || !(f.kHeldK[fk] > 0.0f)) return true;
    if (POPIN_MUTANT == 7) wasCut = !wasCut;
    const float lim = radius * (wasCut ? f.kHeldK[fk] : f.kNewK[fk]);
    return dist2 > lim * lim;
}
inline float FloorDist2K(const FloorTurn& f, int fk, float radius) {
    if (fk < 0 || fk >= kFloorKinds || !(f.kHeldK[fk] > 0.0f)) return 0.0f;
    const float lim = radius * f.kNewK[fk];
    return lim * lim;
}
// The size radius of a thing for the floor: the larger of the clipping sphere's radius and the model's extent (measured from the actor's origin, its
// scale and pose included) with a margin for its animation, plus how far the clipping sphere's centre sits from the origin. The model's extent is
// optional (<= 0: unknown). Never below the clipping radius (the game itself never clips a thing whose sphere is in view).
constexpr float kModelMargin = 1.25f;
inline float SizeRadius(float clipRadius, float modelExtent, float centreOffset) {
    const float m = modelExtent > 0.0f ? modelExtent * kModelMargin + centreOffset : 0.0f;
    return m > clipRadius ? m : clipRadius;
}
// A flower / bush group (PlantGroup): one MEMBER is what can pop, at the group's nearest member. The group's sphere (radius R = 160 + h * scale, h the
// half of the largest side of its members' box in world units, PlantGroup::calcBoundingSphereRadius) holds the members' centres within sqrt(3) * h of its
// centre (the box's corner; the group's position is the box's centre); a member is taken as a sphere of the game's own member spacing (160, sSize) times
// the group's scale (at least 1), with the model margin. Returns the member radius; *nearest = the distance of the nearest member's centre (>= 0).
constexpr float kPlantMemberSize = 160.0f;
inline float PlantMember(float groupDist, float groupRadius, float scale, float* nearest) {
    const float s = scale > 1.0f ? scale : 1.0f;
    const float sc = scale > 0.25f ? scale : 0.25f;
    const float h = (groupRadius > kPlantMemberSize ? groupRadius - kPlantMemberSize : 0.0f) / sc;
    const float d = groupDist - 1.7320508f * h;
    *nearest = d > 0.0f ? d : 0.0f;
    return kPlantMemberSize * s * kModelMargin;
}

// ---- B1: the release after a flight (pure) ----
// A cut that would START on a thing that is in the picture while the release runs waits (the thing stays as the flight showed it). A thing already cut,
// or outside the picture, follows the rule. releasing false: the rule as it is (mode 0 / 1, or no flight has just ended).
inline bool ReleaseHolds(bool releasing, bool wasCut, bool inView) { return releasing && !wasCut && inView; }
// The release's state, once a game turn after the flight state (LodCtrl.cpp port_popin_turn). off: the budget is off this turn (GXV_PB_DEMO=2 in a flight).
struct Release {
    bool on = false;
    uint16_t quiet = 0;      // turns in a row in which no cut was held
    uint32_t turns = 0xFFFFFFu;  // turns since the flight ended (the particle ramp's clock; never flown: "long ago"); counts on after the release ends
    bool cutSeen = false;    // a camera cut since the flight ended (the ramp ends with it)
    bool prevOff = false;
    // cut: a camera cut this turn; heldLast: what the sites held since the last call
    void Turn(bool off, bool cut, uint32_t heldLast, int quietNeed) {
        const bool wasOff = prevOff;
        prevOff = off;
        if (off) { on = false; quiet = 0; turns = 0; cutSeen = false; return; }
        if (wasOff) { on = true; quiet = 0; turns = 0; cutSeen = false; return; }
        if (turns < 0xFFFFFFu) ++turns;
        if (cut) cutSeen = true;
        if (!on) return;
        if (cut) { on = false; return; }
        quiet = heldLast == 0 ? static_cast<uint16_t>(quiet < 0xFFFF ? quiet + 1 : quiet) : 0;
        if (quiet >= quietNeed) on = false;
    }
    // The weight of the particle thinning (0 none .. 1 full) in the turns after a flight: a ramp over rampTurns, at once after a camera cut. Never
    // flown (or long after): 1.
    float PtclWeight(int rampTurns) const {
        if (prevOff) return 0.0f;
        if (cutSeen || rampTurns <= 0 || turns >= static_cast<uint32_t>(rampTurns)) return 1.0f;
        return static_cast<float>(turns) / static_cast<float>(rampTurns);
    }
};
// The thinning factor (kept fraction / emission factor, 1 = none) with the release's weight
inline float RampedFactor(float f, float weight) { return weight >= 1.0f ? f : 1.0f + (f - 1.0f) * (weight > 0.0f ? weight : 0.0f); }

}  // namespace popin
}  // namespace port
