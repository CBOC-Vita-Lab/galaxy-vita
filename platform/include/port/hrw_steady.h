// warpsteady31 (17 Nov 2026): three steadiness rules for the 60 Assist second picture, each behind its own switch, all default 0 until the console A/B. Pure logic (no GL, no clock of
// its own: the caller gives the time), so tests/hrw_steady_test.cpp checks them on the host. The reasoning is in reports/warpsteady31.md: a steady error is far less visible than
// one that flips at 30 Hz or less, and a steady 30 for half a second is more honest than 30/60 alternating every few pictures.
//
//   GXV_HRW_PLANE_RAMP=<pairs>   (0 = off, the plane is on or off per pair as before; 4-6 is the proposal) the ground plane's weight goes up by 1/pairs on each pair that has a plane and
//                                 down by 1/pairs on each pair that has none (for the ground-information reasons only), so the near ground never flips between "right" and "doubled".
//   GXV_HRW_HOLD30_MS=<ms>       (0 = off; ~500 is the proposal) after a pair gave up for a motion reason (edge strip, ground parallax, degenerate source) every pair for that long
//                                 shows the first picture again (30) instead of being tried: the rhythm is 30 for the window, then 60 again, never 30/60/30/60.
//   GXV_HRW_EDGE_PX=<px>         (default -1 = GXV_HRW_EDGE_MAX_PX, which defaults to 48: console-tested since c121) the edge strip's limit; the comment that said 24 was stale (EdgeLimit).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "port/hrw36.h"

namespace port {
namespace hrwsteady {

// ---- the plane's weight -------------------------------------------------------------------------------------------------------------------------------------------------------------
// The plane scaled to weight w in 0..1: every pixel is given the inverse depth w * k * fade (k clamped and faded as EvalK does). The plane at distance d / w is the same plane with
// the camera w times as high above it, so: K and kMax times w, the ground point g divided by w (the fade measures a hit's distance from g, and a hit is 1 / w times as far), the fade's
// slope times w. EvalK(Scaled(p, w)) == w * EvalK(p) exactly (up to rounding); w >= 1 returns p, w <= 0 returns a plane that is off.
inline port::hrw36::Plane Scaled(const port::hrw36::Plane& p, double w) {
    if (!p.on || !(w > 0.0)) { port::hrw36::Plane o = p; o.on = false; return o; }
    if (w >= 1.0) return p;
    port::hrw36::Plane o = p;
    for (int i = 0; i < 3; ++i) o.K[i] = static_cast<float>(p.K[i] * w);
    o.kMax = static_cast<float>(p.kMax * w);
    for (int i = 0; i < 3; ++i) o.g[i] = static_cast<float>(p.g[i] / w);
    o.fadeB = static_cast<float>(p.fadeB * w);
    return o;
}

// Why a pair had no plane, for the ramp: the ground under him was not known this pair (the plane can go on from the last ground it saw) or the pair cannot use a plane at all
// (HUD rectangles, margins, program: nothing to ramp, the weight goes to 0 at once).
struct PlaneRamp {
    int steps = 0;                // GXV_HRW_PLANE_RAMP (0 = off)
    int level = 0;                // 0 .. steps
    bool have = false;            // a ground is remembered
    int age = 0;                  // pairs since it was seen
    float g[3] = {}, n[3] = {};   // the last good ground point and normal (B's view space of the pair that saw them)
    void Configure(long s) { steps = static_cast<int>(std::clamp<long>(s, 0, 60)); level = 0; have = false; age = 0; }
    // A pair that has a plane: remember its ground; the weight to draw it with.
    double Good(const float gnd[3], const float nrm[3]) {
        for (int i = 0; i < 3; ++i) { g[i] = gnd[i]; n[i] = nrm[i]; }
        have = true; age = 0;
        if (steps <= 0) return 1.0;
        level = std::min(level + 1, steps);
        return static_cast<double>(level) / steps;
    }
    // A pair whose ground information is missing: may the ramp go on with the last ground (it still has weight to lose)?
    bool CanDecay() const { return steps > 0 && have && level > 0; }  // (the weight is the age's limit: from `steps` it falls to 0 in at most `steps` pairs)
    // Take the next step down (call only after CanDecay() and a plane made from g, n); the weight to draw with.
    double Decay() { ++age; level = std::max(level - 1, 0); return static_cast<double>(level) / steps; }
    // A pair that cannot use a plane at all.
    void Reset() { level = 0; have = false; age = 0; }
};

// ---- the 30 hold -------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Hold30 {
    double holdMs = 0.0;      // GXV_HRW_HOLD30_MS (0 = off)
    double until = -1.0;      // the time (ms) until which every pair is given up
    uint64_t armed = 0, held = 0;
    void Configure(long ms) { holdMs = static_cast<double>(std::clamp<long>(ms, 0, 5000)); until = -1.0; armed = held = 0; }
    // Does this give-up reason start a hold? Only the camera-motion ones: a missing snapshot, an id mismatch or a program that is not built are not about motion.
    bool Active(double nowMs) { if (holdMs > 0.0 && nowMs < until) { ++held; return true; } return false; }
    void Gave(bool motionReason, double nowMs) { if (holdMs > 0.0 && motionReason) { until = nowMs + holdMs; ++armed; } }
};

// ---- the edge limit ----------------------------------------------------------------------------------------------------------------------------------------------------------------
// The limit in pixels: GXV_HRW_EDGE_PX when given (>= 0), else GXV_HRW_EDGE_MAX_PX (default 48; 0 = never). The comment above the code said 24, the value in the code was
// 48 since c121 (warp32 PATCH13) and the console runs (c172b-c181, the 1115 stress route) were taken with 48, so the code is right and the comment was stale.
inline double EdgeLimit(long edgePx, long edgeMaxPx) {
    const long v = edgePx >= 0 ? edgePx : edgeMaxPx;
    return static_cast<double>(std::clamp<long>(v, 0, 100000));
}
// The pair's decision with the hysteresis (warping resumes below 75 % of the limit). Returns true when the pair is given up.
inline bool EdgeGiveUp(double limit, double lastEdge, bool& off) {
    if (!(limit > 0.0)) return false;
    if (lastEdge > limit) off = true;
    else if (lastEdge < limit * 0.75) off = false;
    return off;
}

}  // namespace hrwsteady
}  // namespace port
