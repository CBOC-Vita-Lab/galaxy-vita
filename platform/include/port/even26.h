#pragma once
// GXV_EVEN26 (pace26): even frame pacing. Two pure pieces (no clock, no globals, no platform includes: tests/even26_model.cpp runs this code):
//
//  Even26   WHICH turns draw a picture when the pipeline cannot take every turn (Performance mode, 30-59 pictures a second).
//  EvenMeter  the owner-facing smoothness score, the "even:" line: how many pictures came on the ideal cadence, judder, latency, missed moments.
//
// WHY (console logs c79, jobs 1786-1788, tools/analyze_logs.py): with GXV_PRESENT_SYNC=2 a picture is shown L vblanks after its turn began, so the
// hold on screen IS the number of turns it advanced ("held-vs-step off by 0" for 97 % of pictures at 40-57 a second). What is left is WHICH turns draw:
// port::pace::LegacySkip leaves a turn out when the back end holds pending >= 2 pictures (code 1). `pending` is delayed and quantised, so the rule
// is a bang-bang loop: 3-4 pictures on consecutive turns (they queue behind one another: ready after 5-7 vblanks, L raised to 5), then two turns out
// (a 3-vblank hold), 1-1-1-3 (pacer25's finding, the 3+ columns of the logs: 5-6 % of pictures over the tour, 17-24 % in single windows).
// Where the GPU alone sets the pace (Dreadnought, 31 ms a picture) the rule even picks 1, 2 and 3 turns at random while the GPU delivers a picture
// every 2 vblanks: hold 2, game step 1/2/3: judder 0.55 vblanks a picture and a latency of 10 vblanks (every picture "ready after 7+": the queue is full).
//
// WHAT: a picture every `period` turns (a real number, Bresenham: holds are floor and ceil of it, the long ones as far apart as they can be), the
// period found by a slow closed loop on the legacy rule's own back-end-busy verdict instead of a cost model or a ladder (pacer25's ladder, +2 steps per
// two guard hits, never came down in time: -9.5 fps at Melty Molten):
//  - each turn the credit grows by 1; a turn draws when the credit reaches the period (credit -= period);
//  - the legacy rule keeps its say where it knows better: the game thread behind the display's clock (code 2) leaves the turn out, the due picture is
//    carried to the next turn (a pass-on, never two long holds in a row from it);
//  - the legacy rule's busy verdict (pending >= its pipeDepth) on a DUE turn is feedback, not a veto: the period grows by gainUp (2-8 %, more when
//    events cluster), the picture is still drawn unless pending is at the guard (3), the back end's own gate then stalls the game thread for the
//    moment (game speed, not a longer hold);
//  - a window of `window` turns without such a verdict shortens the period by gainDown (0.4 %), never under 1.
//  - near 2 the rhythm is quantised: a period of snap30 (default 1.85) or more is exactly 2.0 (an even 30: no stray hold of 1 every few pictures);
//    near 1 (under 1.03) every turn draws. The period itself keeps tracking, so the snap leaves when the pipeline has room.
// Game outcomes never depend on the draw decision (as the legacy skip), the logic keeps its 60 turns a second.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)

namespace port::pace {

struct Even26Config {
    int mode = 0;                // 0 off (every call answers the legacy rule), 1 acts, 2 census (decides and counts, the legacy rule acts)
    double gainUp = 0.005;       // period *= 1 + gainUp * (1 + min(3, events in the last window)) on a busy verdict at a due turn
    double gainDown = 0.006;     // period *= 1 - gainDown after a window without one
    unsigned window = 30;        // turns a window (0.5 s)
    unsigned eventAt = 2;        // pending at a due turn that counts as a busy verdict (the legacy rule's pipeDepth)
    double early = 0.9;          // a picture may come this much (turns of credit) before it is due, when the back end holds earlyPending pictures or fewer (it would starve)
    unsigned earlyPending = 1;
    unsigned guard = 3;          // pending at which a due picture is left out
    double snap30 = 1.85;        // periods from here to 2 are exactly 2 (0: off)
    double snap60 = 1.15;        // periods under this are exactly 1 (every turn: a stage within 15 % of a refresh keeps its strict 60; 1.05 lost 0.6 fps at 60 in the model)
    double maxPeriod = 3.0;      // longest period (turns); the legacy rule's own step cap still applies
    double hintLo = 0.85, hintHi = 1.2;  // with the slowest stage's cost known (hint, in turns): the period stays between hintLo x hint and hintHi x hint (and starts at hint)
    bool fit60 = false;          // deadfix27 (GXV_EVEN26_FIT60): a stage whose slowest stage costs at most one turn (hint 1.0) keeps period 1.0: the busy verdicts of a healthy 60 (pending 2 at a due turn is routine with two pictures in flight) no longer raise it
    unsigned maxSkipRun = 3;     // never more than this many turns in a row without a picture (a 4-vblank hold); under a period of 2 the hold is at most 2 turns
};

class Even26 {
public:
    struct Turn {
        bool allowed;         // false: a load or scene change (the turn draws, nothing is measured)
        bool quiet;           // a pause menu or closed fade: the legacy rule answers
        int legacy;           // what port::pace::LegacySkip decided: 0 draw, 1 leave out and wait for the retrace, 2 leave out late
        bool legacyBehind;    // ... because the game thread is behind the display's clock (code 2)
        bool legacyBusy;      // ... because the back end holds pending >= its pipeDepth (code 1, the verdict used as feedback)
        bool capped;          // the legacy step cap made it draw (its run of skips was long enough)
        unsigned pending;     // pictures queued to the back end that it has not finished
        uint32_t pipeUs = 0;  // the slowest stage's time per picture (front end / renderer / GPU, port_autoq_stages); 0 unknown
    };
    struct Stats {
        uint64_t turns = 0, draws = 0, legacyDraws = 0, differ = 0, events = 0, guardHits = 0, behindSkips = 0, runForced = 0;
        uint64_t step[5] = {};  // the rhythm's own steps in turns (index 4: 4 or more)
    };
    void Configure(const Even26Config& c) {
        cfg_ = c;
        period_ = 1.0;
        quietWindows_ = 0;
        eventsInWindow_ = 0;
        stats_ = Stats();
        Reset();
    }
    bool Enabled() const { return cfg_.mode != 0; }
    bool Acting() const { return cfg_.mode == 1; }
    double Period() const { return period_; }
    // The period to start from (a stage's last known one, the tests'): 1 .. maxPeriod.
    void SetPeriod(double p) { period_ = std::min(cfg_.maxPeriod, std::max(1.0, p)); }
    double EffectivePeriod() const { return Quantise(period_); }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }

    // 0 draw, 1 / 2 as the legacy codes.
    int Decide(const Turn& t) {
        if (!cfg_.mode) return t.legacy;
        if (!t.allowed || !started_) {
            started_ = true;
            Reset();
            return t.legacy;
        }
        if (t.quiet) return t.legacy;
        ++stats_.turns;
        if (t.legacy == 0) ++stats_.legacyDraws;
        credit_ += 1.0;
        if (t.pipeUs) {
            const double hint = std::min(cfg_.maxPeriod, std::max(1.0, t.pipeUs / 16683.0));
            if (!haveHint_) { haveHint_ = true; if (period_ < hint * cfg_.hintLo) period_ = hint * cfg_.hintLo; }
            hint_ = hint;
        }
        if (hint_ > 0.0) period_ = std::min(std::max(period_, std::max(1.0, hint_ * cfg_.hintLo)), std::max(1.0, hint_ * cfg_.hintHi));
        if (cfg_.fit60 && hint_ > 0.0 && hint_ <= 1.0 + 1e-9) period_ = 1.0;   // (deadfix27: console tour P, Floater: the period crept to the hintHi cap 1.2 = 50 pictures a second on a stage that ran 59.8 without the pacer; its GPU is 14.4 ms)
        const double p = Quantise(period_);
        bool due = credit_ + 1e-9 >= p;
        if (!due && cfg_.early > 0.0 && t.pending <= cfg_.earlyPending && credit_ + cfg_.early + 1e-9 >= p && !t.legacyBehind) due = true;
        int code = 0;
        if (t.legacyBehind) {
            // (the game thread cannot keep 60 turns a second: the legacy rule's debt leaves this turn out; the picture, if due, goes to the next turn)
            code = 2;
            if (due) ++stats_.behindSkips;
            due = false;
        } else if (!due) {
            code = 1;
        } else {
            // a due turn
            if (t.pending >= cfg_.eventAt) {
                ++stats_.events;
                ++eventsInWindow_;
                const double k = 1.0 + std::min<double>(3.0, eventsInWindow_ - 1.0);
                period_ = std::min(cfg_.maxPeriod, period_ * (1.0 + cfg_.gainUp * k));
            }
            if (t.pending >= cfg_.guard) {
                code = 1;
                ++stats_.guardHits;
                due = false;
            }
        }
        // (the hold never exceeds ceil(period) turns, at least 2, at most maxSkipRun + 1: a longer one is a missed moment whatever the rules above said)
        if (code != 0) {
            const unsigned ceilP = static_cast<unsigned>(std::ceil(p - 1e-9));
            const unsigned maxHold = std::min(cfg_.maxSkipRun + 1, std::max(2u, ceilP));
            if (skipRun_ + 2 > maxHold) {
                code = 0;
                ++stats_.runForced;
            }
        }
        if (code == 0) {
            credit_ = std::max(-p, credit_ - p);
            if (credit_ > 1.5 * p) credit_ = 1.5 * p;  // (a long carry is not paid back with pictures on consecutive turns)
            ++stats_.draws;
            const unsigned s = skipRun_ + 1;
            ++stats_.step[s < 4 ? s : 4];
            skipRun_ = 0;
        } else {
            ++skipRun_;
            if (credit_ > 2.0 * p) credit_ = 2.0 * p;
        }
        if ((code == 0) != (t.legacy == 0)) ++stats_.differ;
        if (++windowTurns_ >= cfg_.window) {
            if (eventsInWindow_ == 0) {
                ++quietWindows_;
                period_ = std::max(1.0, period_ * (1.0 - cfg_.gainDown));
            } else {
                quietWindows_ = 0;
            }
            eventsInWindow_ = 0;
            windowTurns_ = 0;
        }
        return cfg_.mode == 1 ? code : t.legacy;
    }
    static int Format(char* out, size_t n, const Even26& e) {
        const Stats& s = e.stats_;
        return snprintf(out, n,
            "even26: %s, picture every %.3f turns (%.1f a second; drawn every %.2f), turns %u, pictures %u (legacy rule %u), differ %u, busy verdicts %u, guard %u, game-behind skips %u, "
            "run-forced %u, steps 1/2/3/4+ %u/%u/%u/%u; cost hint %.2f turns, fit-60 hold %s",
            e.cfg_.mode == 2 ? "census (decides, the legacy rule acts)" : "acting", e.period_, 60.0 / e.period_, e.EffectivePeriod(), static_cast<unsigned>(s.turns),
            static_cast<unsigned>(s.draws), static_cast<unsigned>(s.legacyDraws), static_cast<unsigned>(s.differ), static_cast<unsigned>(s.events),
            static_cast<unsigned>(s.guardHits), static_cast<unsigned>(s.behindSkips), static_cast<unsigned>(s.runForced), static_cast<unsigned>(s.step[1]),
            static_cast<unsigned>(s.step[2]), static_cast<unsigned>(s.step[3]), static_cast<unsigned>(s.step[4]), e.hint_, e.cfg_.fit60 ? "on" : "off");
    }

private:
    double Quantise(double p) const {
        if (p < cfg_.snap60) return 1.0;
        if (cfg_.snap30 > 0.0 && p >= cfg_.snap30 && p <= 2.0) return 2.0;
        return p;
    }
    void Reset() {
        credit_ = Quantise(period_);  // (the next turn draws)
        credit_ -= 1.0;
        skipRun_ = 0;
        windowTurns_ = 0;
        eventsInWindow_ = 0;
    }
    Even26Config cfg_;
    Stats stats_;
    double period_ = 1.0, credit_ = 0.0;
    unsigned skipRun_ = 0, windowTurns_ = 0, quietWindows_ = 0, eventsInWindow_ = 0;
    double hint_ = 0.0;
    bool started_ = false, haveHint_ = false;
};

// ---- the smoothness score ("even:" line) -----------------------------------------------------------------------------------------------
// One call per picture shown (display thread). hold: vblanks since the previous picture. step: game turns the picture advanced (0 = not known). ready:
// vblanks from its turn to the moment it was ready (-1 = not known). latency: vblanks from its turn to the vblank it was shown on (-1 = not known).
//
// Definitions (the same in the console line and in tests/even26_model.cpp):
//   rhythm        mean hold of the window m. The ideal holds are lo = floor(m) and hi = ceil(m) (a window at 1.00 has only 1; at 2.00 only 2).
//   ideal cadence a picture whose hold is lo or hi, whose game step equals its hold (step unknown: not tested), and that is not part of a clump (two
//                 minority holds in a row: with a Bresenham rhythm the long holds are never adjacent while m - lo < 0.5, the short ones while it is above).
//   judder        mean |hold - step| in vblanks a picture (what the "present:" line calls judder; 0 for an aligned picture on time).
//   missed moment a picture held 3 vblanks or more, or one that advanced 3 turns or more (counted once).
//   latency       mean vblanks from the picture's turn to its being shown (x 16.68 ms); pad sample to turn start is separate ("latency:" line).
//   score         100 * ideal share - 25 * judder - 5 * missed moments a second, clamped to 0..100. 100 is a rhythm with no stray hold, no step/hold mismatch and no miss.
class EvenMeter {
public:
    struct Window {
        uint32_t pictures = 0, missed = 0, missedHold = 0, missedStep = 0, stepKnown = 0, latKnown = 0, readyKnown = 0, readyOver = 0;
        uint32_t joint[5][5] = {};    // [hold 0..4+][step 0..4+] (step 0: not known)
        uint32_t pair[5][5] = {};     // [previous hold][hold]
        uint64_t holdSum = 0, offSum = 0, latSum = 0;
        uint32_t latMax = 0;
        double secs = 0;
    };
    struct Score {
        uint32_t pictures = 0, ideal = 0, missed = 0, missedHold = 0, missedStep = 0;
        double fps = 0, mean = 0, idealPct = 100, judder = 0, latVblanks = 0, latMs = 0, missedPerSec = 0, score = 100;
        double hold[4] = {};  // share of holds 1 / 2 / 3 / 4+
        unsigned lo = 1, hi = 1;
        bool latKnown = false, stepKnown = false;
    };
    static constexpr double kVblankMs = 16.68335;
    // hold 0 (two pictures in one vblank: the first was never seen) and the first picture of a window are not counted.
    void Picture(uint32_t hold, uint32_t step, int ready, int latency) {
        if (!havePrev_) { havePrev_ = true; prevHold_ = 0; return; }
        if (hold == 0) return;
        Window& w = w_;
        ++w.pictures;
        const unsigned h = hold < 4 ? hold : 4, s = step < 4 ? step : 4;
        ++w.joint[h][s];
        if (prevHold_) ++w.pair[prevHold_ < 4 ? prevHold_ : 4][h];
        prevHold_ = hold;
        w.holdSum += hold;
        if (step) {
            ++w.stepKnown;
            w.offSum += hold > step ? hold - step : step - hold;
        }
        const bool m = hold >= 3 || step >= 3;
        if (m) ++w.missed;
        if (hold >= 3) ++w.missedHold;
        if (step >= 3) ++w.missedStep;
        if (latency >= 0) { ++w.latKnown; w.latSum += static_cast<uint32_t>(latency); if (static_cast<uint32_t>(latency) > w.latMax) w.latMax = static_cast<uint32_t>(latency); }
        if (ready >= 0) ++w.readyKnown;
    }
    // A scene change or load: the next picture has no previous one.
    void Break() { havePrev_ = false; prevHold_ = 0; }
    const Window& Current() const { return w_; }
    Window Take() { Window w = w_; w_ = Window(); return w; }
    static Score Compute(const Window& w, double secs) {
        Score r;
        r.pictures = w.pictures;
        if (!w.pictures || secs <= 0.0) return r;
        const double n = static_cast<double>(w.pictures);
        r.fps = n / secs;
        r.mean = static_cast<double>(w.holdSum) / n;
        r.lo = static_cast<unsigned>(std::floor(r.mean + 1e-9));
        r.hi = static_cast<unsigned>(std::ceil(r.mean - 1e-9));
        if (r.lo < 1) r.lo = 1;
        if (r.hi < r.lo) r.hi = r.lo;
        uint32_t good = 0;
        for (unsigned h = r.lo; h <= r.hi && h <= 3; ++h) {
            // (hold == step, or the step unknown)
            good += w.joint[h][h] + w.joint[h][0];
        }
        // clumps: two minority holds in a row (the minority is the hold that is the less frequent of lo / hi; none when lo == hi)
        uint32_t clump = 0;
        if (r.hi > r.lo && r.hi <= 3) {
            const unsigned minority = (r.mean - r.lo) < 0.5 ? r.hi : r.lo;
            clump = 2 * w.pair[minority][minority];  // (a clump of k long holds has k - 1 pairs and k damaged pictures: two for a pair)
            // (a clump costs the pictures that make it, not more than the ideal ones there are)
            if (clump > good) clump = good;
        }
        r.ideal = good - clump;
        r.idealPct = 100.0 * r.ideal / n;
        uint32_t hs[5] = {};
        for (unsigned h = 1; h < 5; ++h) { uint32_t t = 0; for (unsigned s = 0; s < 5; ++s) t += w.joint[h][s]; hs[h] = t; }
        for (unsigned h = 1; h <= 4; ++h) r.hold[h - 1] = hs[h] / n;
        r.stepKnown = w.stepKnown != 0;
        r.judder = w.stepKnown ? static_cast<double>(w.offSum) / w.stepKnown : 0.0;
        r.latKnown = w.latKnown != 0;
        r.latVblanks = w.latKnown ? static_cast<double>(w.latSum) / w.latKnown : 0.0;
        r.latMs = r.latVblanks * kVblankMs;
        r.missed = w.missed; r.missedHold = w.missedHold; r.missedStep = w.missedStep;
        r.missedPerSec = w.missed / secs;
        double sc = r.idealPct - 25.0 * r.judder - 5.0 * r.missedPerSec;
        r.score = sc < 0.0 ? 0.0 : sc > 100.0 ? 100.0 : sc;
        return r;
    }
    // one line, every 10 s: "even: ..."
    static int Format(char* out, size_t n, const Score& s, double secs, const char* pacer, int L) {
        char lt[16] = "";
        if (L >= 0) snprintf(lt, sizeof lt, ", L%d", L);
        return snprintf(out, n,
            "even: %.1f s, %u pictures (%.1f a second), score %.0f/100; on the ideal cadence %.0f %% (rhythm %.2f vblanks a picture, ideal holds %u..%u; holds 1/2/3/4+ %.0f/%.0f/%.0f/%.0f %%); "
            "judder %.3f vblanks a picture (%.1f ms)%s; latency %s%.1f vblanks (%.0f ms) turn to picture%s; missed moments %u (%.2f a second: held 3+ vblanks %u, game jump of 3+ turns %u); %s",
            secs, s.pictures, s.fps, s.score, s.idealPct, s.mean, s.lo, s.hi, 100 * s.hold[0], 100 * s.hold[1], 100 * s.hold[2], 100 * s.hold[3], s.judder, s.judder * kVblankMs,
            s.stepKnown ? "" : " (turns not known)", s.latKnown ? "" : "(not known) ", s.latVblanks, s.latMs, lt, s.missed, s.missedPerSec, s.missedHold,
            s.missedStep, pacer ? pacer : "");
    }

private:
    Window w_;
    bool havePrev_ = false;
    uint32_t prevHold_ = 0;
};

}  // namespace port::pace
