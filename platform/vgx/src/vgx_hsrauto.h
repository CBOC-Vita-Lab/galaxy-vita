// GXV_HSR_AUTO (geom26): the draw-order planner (vgx_hsrorder.h, "alpha-tested draws behind the opaque ones") as a GENERIC per-stage rule.
// Pure logic (tests/vgx_hsrauto_test.cpp runs this very file); the back end (vgx_backend_gl.cpp, namespace hsrx) feeds it one picture at a time.
//
// Why: on a tile-based deferred renderer an alpha-tested (discard) draw cannot let the hardware settle depth before shading, so every opaque
// fragment drawn AFTER it in the same scene is shaded even where it ends up hidden. The planner moves such draws behind the opaque draws of their
// segment (the decal-aware rule, GXV_HSR_ORDER_TIES=7). The console measured the saving on Dreadnought only (30.2 -> 23.6 ms with the inexact rule 0);
// the tours say the cost of alpha-tested draws is ~0 on 14 stages and 1-2 ms on three more (new/results27 PRICING.tsv), so a global switch would only
// pay the planner's CPU and the geometry proofs' memory where nothing is gained. This is the stage-by-stage decision, made from what the stage
// really draws, with no table to maintain:
//   * PROBE   (first probePics drawn pictures of a stage): the planner runs on a copy (nothing is reordered) and counts the alpha-tested draws that sit
//             before the last opaque draw of their scene ("T before last O", the draws the reorder could help).
//   * ON      mean T-before-last-O >= minTBefore: the reorder is applied. Every onWindow pictures it is checked that it still moves at least
//             minMoved draws a picture and costs at most maxPlanUs microseconds of planner time a picture; if not, OFF.
//   * OFF     the planner does not run at all (zero cost). After recheck pictures it probes again (a stage changes: another section, another camera).
// observeOnly (GXV_HSR_AUTO=2): it stays in PROBE for ever and reports the verdict each window; nothing is applied (the census run).
#pragma once
#include <cstdint>

namespace vgx {
namespace hsrauto {

enum Phase : uint8_t { kProbe = 0, kOn = 1, kOff = 2 };
enum Why : uint8_t { kWhyStart = 0, kWhyEnough, kWhyFewT, kWhyNoMove, kWhyCost, kWhyRecheck };

struct Cfg {
    uint32_t probePics = 240;     // drawn pictures looked at before the first decision
    uint32_t minDraws = 24;       // a picture with fewer draws is a menu / fade / loading picture and does not count
    double minTBefore = 1.0;      // mean alpha-tested draws before the last opaque draw of their scene (a picture) for ON
    uint32_t onWindow = 600;      // pictures between checks while ON
    double minMoved = 0.5;        // mean alpha-tested draws moved a picture below which ON ends
    double maxPlanUs = 400.0;     // mean planner microseconds a picture above which ON ends
    uint32_t recheck = 7200;      // pictures spent OFF before probing again (two minutes at 60)
    bool observeOnly = false;     // GXV_HSR_AUTO=2
};

struct Auto {
    Phase phase = kProbe;
    Why why = kWhyStart;
    uint32_t n = 0;                          // drawn pictures counted in this phase window
    uint64_t sumT = 0, sumMoved = 0, sumUs = 0;
    uint32_t idle = 0;                       // pictures spent OFF
    uint32_t changes = 0;                    // phase changes since Reset
    double lastT = 0.0, lastMoved = 0.0, lastUs = 0.0;  // the means of the window that ended last (for the log line)
    bool verdictOn = false;                  // observeOnly: what the last window would have decided

    void Reset() { *this = Auto{}; }
    bool Planning() const { return phase != kOff; }
    bool Applying() const { return phase == kOn; }

    // One picture was planned. draws: its draw count; tBefore: alpha-tested draws before the last opaque draw of their scene; tMoved: alpha-tested
    // draws the plan moved (or would move); planUs: the planner's microseconds. True when the phase changed or (observeOnly) a verdict was made.
    bool Feed(uint32_t draws, uint32_t tBefore, uint32_t tMoved, uint64_t planUs, const Cfg& c) {
        if (phase == kOff) return false;
        if (draws < c.minDraws) return false;
        ++n; sumT += tBefore; sumMoved += tMoved; sumUs += planUs;
        if (phase == kProbe) {
            if (n < c.probePics) return false;
            lastT = static_cast<double>(sumT) / n; lastMoved = static_cast<double>(sumMoved) / n; lastUs = static_cast<double>(sumUs) / n;
            const bool on = lastT >= c.minTBefore;
            n = 0; sumT = sumMoved = sumUs = 0;
            if (c.observeOnly) { verdictOn = on; return true; }
            phase = on ? kOn : kOff;
            why = on ? kWhyEnough : kWhyFewT;
            idle = 0; ++changes;
            return true;
        }
        // kOn
        if (n < c.onWindow) return false;
        lastT = static_cast<double>(sumT) / n; lastMoved = static_cast<double>(sumMoved) / n; lastUs = static_cast<double>(sumUs) / n;
        n = 0; sumT = sumMoved = sumUs = 0;
        if (lastMoved < c.minMoved) { phase = kOff; why = kWhyNoMove; idle = 0; ++changes; return true; }
        if (lastUs > c.maxPlanUs) { phase = kOff; why = kWhyCost; idle = 0; ++changes; return true; }
        return false;
    }
    // One picture passed while OFF (the planner did not run). True when it is time to probe again.
    bool Skip(const Cfg& c) {
        if (phase != kOff) return false;
        if (++idle < c.recheck) return false;
        phase = kProbe; why = kWhyRecheck; n = 0; sumT = sumMoved = sumUs = 0; idle = 0; ++changes;
        return true;
    }
};

inline const char* PhaseName(Phase p) { return p == kProbe ? "probe" : p == kOn ? "on" : "off"; }
inline const char* WhyName(Why w) {
    switch (w) {
        case kWhyStart: return "stage start";
        case kWhyEnough: return "enough alpha-tested draws before opaque ones";
        case kWhyFewT: return "too few alpha-tested draws before opaque ones to pay";
        case kWhyNoMove: return "the plan moves almost nothing";
        case kWhyCost: return "the planner costs too much";
        case kWhyRecheck: return "recheck";
    }
    return "?";
}

}  // namespace hsrauto
}  // namespace vgx
