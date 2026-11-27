#pragma once
// GXV_LATE_TURN (pace32, 13 Nov 2026): "phase sync" for the even 30. Pure logic (no clock, no globals): tests/pace32_test.cpp runs this code.
//
// WHY (the owner, 13 Nov 21:30: heavy stages run a "Locked even 30, lowest delay"). At an even 30 the game runs two turns a picture: A draws the state the turn before it made, then reads the
// pad and updates; B (no picture) reads the pad, updates, and waits for its retrace; the next A draws what B made. So the pad sample a picture shows is B's, and between that read and the draw
// lies B's update AND B's idle wait for the retrace: the game thread finished early and then slept with a pad sample that grows stale. Oculus' Phase Sync (developers.meta.com, "Bringing Phase
// Sync to mobile VR": finish just before the deadline, not as early as possible) moves that sleep to the front: B waits FIRST, then reads the pad, and ends just before its retrace.
//
// WHAT. Per eligible turn (an undrawn turn that keeps its retrace, in a two-turn rhythm) the glue reports the turn's SLACK = the idle time it had before its retrace + the delay it was given
// (0 for a turn that ended after its retrace). The delay of the next eligible turn is the smallest slack of the last `window` turns less `marginUs`, at most `maxUs`; it rises by `stepUpUs` a
// turn and falls at once. A turn that was delayed and then missed its retrace costs a whole refresh of game time, so it is treated as a fault: the delay is zero for `backoffTurns` eligible
// turns, the margin grows by half (to at most 3 times) and shrinks back after `healTurns` clean ones.
// The delay sits INSIDE the turn (after the pacing decision, before the update), so the turn starts are where they were: no pacer, no speed guard and no present stamp sees a change.
#include <algorithm>
#include <cstdint>
#include <stdio.h>

namespace port::lateturn {

struct Config {
    bool on = false;
    uint32_t marginUs = 2500;      // GXV_LATE_TURN_MARGIN_US: the turn is planned to end this long before its retrace
    uint32_t maxUs = 12000;        // GXV_LATE_TURN_MAX_US
    uint32_t stepUpUs = 250;       // the delay rises this much a turn
    unsigned window = 90;          // eligible turns the smallest slack is taken over (90 = 3 s at an even 30: a heavier turn every second or two stays in it)
    unsigned backoffTurns = 90;    // after a delayed turn missed its retrace: no delay for this many eligible turns (3 s)
    unsigned healTurns = 900;      // clean eligible turns that take one widening of the margin back (30 s)
};

struct Stats {
    uint32_t turns = 0, delayed = 0, lates = 0, backoffs = 0, resets = 0, tight = 0;
    uint64_t delayUs = 0, idleLeftUs = 0;
    uint32_t delayMaxUs = 0;
};

class Planner {
public:
    static constexpr unsigned kRing = 128;
    void Configure(const Config& c) {
        c_ = c;
        if (c_.window < 4) c_.window = 4;
        if (c_.window > kRing) c_.window = kRing;
        if (c_.maxUs > 15000) c_.maxUs = 15000;
        Reset();
        stats_ = Stats();
        widen_ = 2;
    }
    bool Enabled() const { return c_.on; }
    const Config& Cfg() const { return c_; }
    // A load, a covered screen, a change of rhythm: the measurements start again.
    void Reset() { n_ = h_ = 0; delay_ = 0; hold_ = 0; clean_ = 0; ++stats_.resets; }
    // The delay (us) for the eligible turn that is about to update. 0 = none.
    uint32_t Plan() {
        if (!c_.on) return 0;
        if (hold_ > 0) { --hold_; delay_ = 0; return 0; }
        if (n_ < c_.window) return 0;
        uint32_t lo = ring_[0];
        for (unsigned i = 1; i < n_; ++i) lo = std::min(lo, ring_[i]);
        const uint32_t margin = c_.marginUs * widen_ / 2;
        uint32_t target = lo > margin ? lo - margin : 0;
        if (target > c_.maxUs) target = c_.maxUs;
        if (target < delay_) delay_ = target;
        else delay_ = std::min(target, delay_ + c_.stepUpUs);
        if (delay_ < 300) return 0;   // (a sleep under 0.3 ms is not worth its own jitter)
        return delay_;
    }
    // The eligible turn is over. idleUs: from the end of its work to its retrace's wake. sleptUs: what it really slept before its update (0 = not delayed). late: it missed its retrace
    // (the next turn began more than a refresh and a half after this one).
    void Result(uint32_t idleUs, uint32_t sleptUs, bool late) {
        if (!c_.on) return;
        ++stats_.turns;
        uint32_t slack = late ? 0u : idleUs + sleptUs;
        // a delayed turn with no idle left ran into (or over) its retrace: how far over is not known, so the slack is taken as the sleep less the margin (the next delay is two margins shorter)
        if (!late && sleptUs && idleUs < 300) { slack = sleptUs > MarginNowUs() ? sleptUs - MarginNowUs() : 0u; ++stats_.tight; }
        ring_[h_] = slack;
        h_ = (h_ + 1) % c_.window;
        if (n_ < c_.window) ++n_;
        if (sleptUs) {
            ++stats_.delayed;
            stats_.delayUs += sleptUs;
            stats_.delayMaxUs = std::max(stats_.delayMaxUs, sleptUs);
            if (!late) stats_.idleLeftUs += idleUs;
        }
        if (late) {
            ++stats_.lates;
            if (sleptUs) {   // our fault (or not: it is treated as ours)
                ++stats_.backoffs;
                hold_ = c_.backoffTurns;
                delay_ = 0;
                if (widen_ < 6) ++widen_;
                clean_ = 0;
            }
        } else if (++clean_ >= c_.healTurns) {
            clean_ = 0;
            if (widen_ > 2) --widen_;
        }
    }
    uint32_t DelayNow() const { return delay_; }
    uint32_t MarginNowUs() const { return c_.marginUs * widen_ / 2; }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }

private:
    Config c_;
    uint32_t ring_[kRing] = {};
    unsigned n_ = 0, h_ = 0, hold_ = 0, clean_ = 0, widen_ = 2;
    uint32_t delay_ = 0;
    Stats stats_;
};

// "late-turn: ..." (every 10 s while it runs): the proof that it acted and by how much.
inline int FormatLine(char* out, unsigned cap, const Planner& p, double seconds) {
    const Stats& s = p.GetStats();
    return snprintf(out, cap, "late-turn: GXV_LATE_TURN=1: %.1f s, %u undrawn turns of the even 30, %u started late on purpose (mean %.1f ms, longest %.1f ms: the pad read of the next picture is that much "
                              "fresher), idle left before the retrace %.1f ms mean (none left %u times), turns that missed their retrace %u (%u after a delay: backed off), delay now %.1f ms, margin %.1f ms",
                    seconds, s.turns, s.delayed, s.delayed ? s.delayUs / 1000.0 / s.delayed : 0.0, s.delayMaxUs / 1000.0, s.delayed > s.backoffs ? s.idleLeftUs / 1000.0 / (s.delayed - s.backoffs) : 0.0, s.tight, s.lates,
                    s.backoffs, p.DelayNow() / 1000.0, p.MarginNowUs() / 1000.0);
}

}  // namespace port::lateturn
