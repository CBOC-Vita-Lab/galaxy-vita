// detail25 (11 Nov): adaptive detail ("the governor") and the cuts it drives. The pure parts, header only so a host test can link them
// (tests/detail.cpp, tools/test_detail.sh). Everything defaults to 0 = today's behaviour exactly.
//
//   GXV_DETAIL_GOV=1       the governor: one detail level (0..5) moved by the game thread's drawn-turn time and the front end's picture time.
//                          Needs GXV_AUTOQ_TARGET=60 (the two measurements come from the autoq's totals). Never moves in a flight / demo /
//                          loading / wipe; at most one step a second; every change logged ("detail: level 2 -> 3: ...").
//   GXV_DETAIL_LEVEL=<n>   a FIXED level n (0..5) with no controller (A/B and screenshots). 1 = today.
//   GXV_DETAIL_CYCLE=<s>   stress: walk the levels 0 1 2 3 4 5 4 3 2 1 0 ... every <s> seconds regardless of load.
//   GXV_DETAIL_BUDGET=0|1  (default 1) the level also scales the existing performance budget's camera distances (decor, shadow, fur, anim, ptcl).
//   GXV_DETAIL_PLANT=0|1|2 plant / flower group thinning (1 on, 2 dry: counted per level, nothing removed).
//   GXV_DETAIL_FUR=0|1|2   fur layers by distance (1 on, 2 dry).
//   GXV_DETAIL_HI / _LO    ms: over (tighten) / under (relax) thresholds, default 15.5 / 13.0.  _DWELL (s, 1.0)  _RELAX (s, 4.0)
//   GXV_DETAIL_FALLBACK=<s> an on-screen plant waits this long for an unseen moment before its change is made anyway (default 2.0 for thinning,
//                          2.5 x that for restoring).  GXV_DETAIL_HYST=<%> (8).  GXV_DETAIL_FUR_GAIN=<exp> (0.5), GXV_DETAIL_FUR_FAR=1|2 (2).
// A change is never abrupt: a plant member changes when it is outside the view or after the fallback, a few per turn; fur layers fade over
// ~0.5 s; the budget's distances slide a few percent a second.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "port/hot_knob.h"
#include "port/stage_knob.h"  // (stagepreset25: the stage-settable ones below)

namespace port {
namespace detail {

constexpr int kLevels = 6;  // 0 (relaxed: wider than today) .. 5 (tightest); 1 = today's performance budget
enum Rule { kDecor = 0, kShadow, kFur, kAnim, kPtcl, kRules };

struct Row {
    float mult[kRules];       // camera-distance multipliers of the existing budget rules
    float plantHalf, plantQuarter;  // camera distance beyond which every 2nd / 4th plant member is drawn (0 = never thinned)
    float furMid, furFar;     // camera distance beyond which fur draws every 2nd layer / few layers (furMid 0 = the PB_FUR distance times mult[kFur]; furFar 0 = none)
};
inline const Row& LevelRow(int l) {
    static const Row rows[kLevels] = {
        {{1.6f, 1.3f, 1.3f, 1.3f, 1.3f}, 4800.0f, 6200.0f, 0.0f, 0.0f},
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 0.0f},
        {{1.0f, 0.9f, 0.9f, 0.9f, 0.9f}, 4200.0f, 5000.0f, 2300.0f, 4200.0f},
        {{0.9f, 0.85f, 0.8f, 0.8f, 0.8f}, 3600.0f, 4400.0f, 2000.0f, 3500.0f},
        {{0.8f, 0.8f, 0.7f, 0.7f, 0.7f}, 3200.0f, 3900.0f, 1800.0f, 3000.0f},
        {{0.7f, 0.75f, 0.6f, 0.6f, 0.6f}, 2900.0f, 3400.0f, 1500.0f, 2500.0f},
    };
    return rows[l < 0 ? 0 : (l >= kLevels ? kLevels - 1 : l)];
}
// A rule's distance at a multiplier: wider freely, tighter never below min(base, floor) (the player guards of each rule stay as they are).
inline float Floor(int rule) {
    static const float f[kRules] = {3000.0f, 2100.0f, 1500.0f, 1200.0f, 1500.0f};
    return f[rule >= 0 && rule < kRules ? rule : 0];
}
inline float Scaled(float base, float mult, int rule) {
    if (mult >= 1.0f) return base * mult;
    const float v = base * mult, fl = Floor(rule), lo = base < fl ? base : fl;
    return v > lo ? v : lo;
}
constexpr float kSlewStep = 0.0006f;  // per game turn: a 0.1 step of a multiplier takes ~2.8 s
// The slide of a multiplier toward its target, at most `step` a turn.
inline float Slew(float cur, float tgt, float step) {
    return cur < tgt ? (tgt - cur < step ? tgt : cur + step) : (cur - tgt < step ? tgt : cur - step);
}

// ---- knobs ---------------------------------------------------------------------------------------------------------------------------
inline float EnvF(const char* n, float d) { float ov; if (port::stageknob::Lookup(n, &ov)) return ov; const char* e = std::getenv(n); return e && *e ? static_cast<float>(std::atof(e)) : d; }
inline int EnvI(const char* n, int d) { float ov; if (port::stageknob::Lookup(n, &ov)) return static_cast<int>(ov); const char* e = std::getenv(n); return e && *e ? static_cast<int>(std::strtol(e, nullptr, 0)) : d; }
inline int GovOn() { return PORT_STAGE_KNOB(int, EnvI("GXV_DETAIL_GOV", 0) != 0 ? 1 : 0); }
inline int FixedLevel() { return PORT_STAGE_KNOB(int, [] { const int v = EnvI("GXV_DETAIL_LEVEL", -1); return v < 0 ? -1 : (v >= kLevels ? kLevels - 1 : v); }()); }
inline float CycleSec() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_CYCLE", 0.0f); return v > 0.5f ? v : 0.0f; }()); }
inline int BudgetOn() { return PORT_HOT_KNOB(int, EnvI("GXV_DETAIL_BUDGET", 1) != 0 ? 1 : 0); }
inline int PlantMode() { return PORT_STAGE_KNOB(int, [] { const int v = EnvI("GXV_DETAIL_PLANT", 0); return v < 0 ? 0 : (v > 2 ? 2 : v); }()); }
inline int FurMode() { return PORT_STAGE_KNOB(int, [] { const int v = EnvI("GXV_DETAIL_FUR", 0); return v < 0 ? 0 : (v > 2 ? 2 : v); }()); }
inline float Hi() { return PORT_HOT_KNOB(float, EnvF("GXV_DETAIL_HI", 15.5f)); }
inline float Lo() { return PORT_HOT_KNOB(float, EnvF("GXV_DETAIL_LO", 13.0f)); }
inline float DwellMs() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_DWELL", 1.0f); return (v < 1.0f ? 1.0f : v) * 1000.0f; }()); }
inline float RelaxMs() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_RELAX", 4.0f); return (v < 2.0f ? 2.0f : v) * 1000.0f; }()); }
inline int StartLevel() { return PORT_STAGE_KNOB(int, [] { const int v = EnvI("GXV_DETAIL_START", 1); return v < 0 ? 0 : (v >= kLevels ? kLevels - 1 : v); }()); }
inline float Undrawn() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_UNDRAWN", 0.5f); return v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v); }()); }
inline float HystPct() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_HYST", 8.0f); return v < 0.0f ? 0.0f : (v > 30.0f ? 30.0f : v); }()); }
inline int FallbackPics() { return PORT_HOT_KNOB(int, [] { const float v = EnvF("GXV_DETAIL_FALLBACK", 2.0f); return static_cast<int>((v < 0.2f ? 0.2f : (v > 4.0f ? 4.0f : v)) * 60.0f); }()); }
inline float FurGainExp() { return PORT_HOT_KNOB(float, [] { const float v = EnvF("GXV_DETAIL_FUR_GAIN", 0.5f); return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }()); }
inline int FurFarLayers() { return PORT_HOT_KNOB(int, EnvI("GXV_DETAIL_FUR_FAR", 2) <= 1 ? 1 : 2); }
inline bool Active() { return GovOn() || FixedLevel() >= 0 || CycleSec() > 0.0f || PlantMode() != 0 || FurMode() != 0; }

// ---- plant thinning / fur layers -----------------------------------------------------------------------------------------------------
// States: 0 full, 1 half (every 2nd member / layer), 2 quarter (every 4th / few layers). With hysteresis: a state returns only <hyst>% of its
// distance nearer than it left. Idempotent for one distance (applying it twice with the same d changes nothing).
inline int ThinNext(int state, float d, float half, float quarter, float hystPct) {
    if (!(half > 0.0f)) return 0;
    const float h = 1.0f - hystPct * 0.01f;
    int s = state < 0 ? 0 : (state > 2 ? 2 : state);
    if (s == 2 && !(quarter > 0.0f)) s = 1;
    if (s == 2 && d <= quarter * h) s = 1;
    if (s == 1 && d <= half * h) s = 0;
    if (s < 2 && quarter > 0.0f && d > quarter) s = 2;
    else if (s < 1 && d > half) s = 1;
    return s;
}
// Stable choice by member index: member 0 (the group's centre, placed first) is always kept.
inline bool KeepMember(int idx, int state) { return state <= 0 || ((idx & (state == 1 ? 1 : 3)) == 0); }
inline int ThinCountKept(int n, int state) { int k = 0; for (int i = 0; i < n; ++i) k += KeepMember(i, state) ? 1 : 0; return k; }

// Fur: does the state draw layer i of n (the tip layer is always drawn; the silhouette is the tip's)?
inline bool FurDrawLayer(int state, int i, int n, int farLayers) {
    if (state <= 0 || n <= 2) return true;
    if (i == n - 1) return true;
    if (state == 1) return (i & 1) == 0;
    return farLayers >= 2 && i == (n - 1) / 2;
}
inline int FurKept(int state, int n, int farLayers) { int k = 0; for (int i = 0; i < n; ++i) k += FurDrawLayer(state, i, n, farLayers) ? 1 : 0; return k; }
// The alpha weight of a kept layer: the missing layers' share, pow(n / kept, exp) up to 1.6 (what is left is a little denser, so the pelt's
// thickness looks the same).
inline float FurGain(int state, int n, int farLayers, float expo) {
    const int k = FurKept(state, n, farLayers);
    if (k <= 0 || k >= n) return 1.0f;
    const float g = std::pow(static_cast<float>(n) / static_cast<float>(k), expo);
    return g > 1.6f ? 1.6f : g;
}
// The weight of layer i while fading from state `from` to `to` (t 0..1): 0 = not drawn.
inline float FurWeight(int from, int to, float t, int i, int n, int farLayers, float expo) {
    const float a = FurDrawLayer(from, i, n, farLayers) ? FurGain(from, n, farLayers, expo) : 0.0f;
    const float b = FurDrawLayer(to, i, n, farLayers) ? FurGain(to, n, farLayers, expo) : 0.0f;
    return a + (b - a) * t;
}

// May an object whose applied state is `cur` take its target `tgt` now? Unseen (outside the view): at once. A camera cut: with a bigger per-turn
// budget. On screen: only after it waited `waited` pictures (thinning: fallback, restoring: 2.5 x), a few per turn (budget). Changes `budget`.
inline bool MayApply(int cur, int tgt, bool unseen, bool cut, int waited, int fallbackPics, int& budget) {
    if (cur == tgt) return false;
    if (unseen) return true;
    const int fb = tgt < cur ? fallbackPics * 5 / 2 : fallbackPics;
    if ((cut || waited >= fb) && budget > 0) { --budget; return true; }
    return false;
}

// ---- the governor --------------------------------------------------------------------------------------------------------------------
// Inputs once a window (about half a second): the game thread's drawn-turn time and the front end's picture time, ms. `freeze`: a flight, a
// demo, loading, a wipe: nothing changes and nothing accumulates. Moves ONE level at most, at most once a dwell; tightens after two windows over
// `hi`; relaxes after `relaxMs` under `lo` (the step 1 -> 0, wider than today, after 2.5 x that), never within a ban after a relax that had to be
// taken back (the ban doubles: 30 s .. 300 s).
struct Governor {
    int level = 1;
    double lastChange = -1e12, underSince = -1.0, banUntil = -1e12, banLen = 30000.0, lastRelax = -1e12;
    int overStreak = 0;
    float hi = 15.5f, lo = 13.0f;
    double dwell = 1000.0, relaxMs = 4000.0;
    int maxLevel = kLevels - 1;
    // returns the new level (== level when nothing changed); `why` 0 none, 1 tightened, 2 relaxed
    int Update(double nowMs, double gameMs, double feMs, bool freeze, int* why = nullptr) {
        if (why) *why = 0;
        if (freeze) { underSince = -1.0; overStreak = 0; return level; }
        const double load = gameMs > feMs ? gameMs : feMs;
        const bool over = load > hi, under = load < lo;
        overStreak = over ? overStreak + 1 : 0;
        if (!under) underSince = -1.0;
        else if (underSince < 0.0) underSince = nowMs;
        const bool dwelled = nowMs - lastChange >= dwell;
        if (over && overStreak >= 2 && level < maxLevel && dwelled) {
            if (nowMs - lastRelax < 8000.0) {  // a relax that did not hold: ban the next ones for longer
                banUntil = nowMs + banLen;
                banLen = banLen * 2.0 > 300000.0 ? 300000.0 : banLen * 2.0;
            }
            ++level; lastChange = nowMs; underSince = -1.0; overStreak = 0;
            if (why) *why = 1;
        } else if (under && level > 0 && dwelled && nowMs >= banUntil && underSince >= 0.0 &&
                   nowMs - underSince >= (level == 1 ? relaxMs * 2.5 : relaxMs)) {
            --level; lastChange = nowMs; lastRelax = nowMs; underSince = nowMs;
            if (why) *why = 2;
        }
        return level;
    }
};

// The drawn-turn estimate from the game thread's mean CPU a turn: turns without a picture cost about `undrawn` of a drawn one.
inline double DrawnTurnMs(double meanTurnMs, double drawnFrac, double undrawn) {
    const double f = drawnFrac < 0.0 ? 0.0 : (drawnFrac > 1.0 ? 1.0 : drawnFrac);
    const double den = f + undrawn * (1.0 - f);
    return den > 0.05 ? meanTurnMs / den : meanTurnMs;
}
// GXV_DETAIL_CYCLE: the level at `nowMs` for a period: 0 1 2 3 4 5 4 3 2 1 0 ...
inline int CycleLevel(double nowMs, double periodMs) {
    const long k = static_cast<long>(nowMs / periodMs) % (2 * (kLevels - 1));
    return static_cast<int>(k < kLevels ? k : 2 * (kLevels - 1) - k);
}

}  // namespace detail
}  // namespace port
