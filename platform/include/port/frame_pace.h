#pragma once
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)

namespace port::pace {
enum class Policy { Adaptive, Fixed30 };
// GXV_STALL_FORGIVE=<ms> (catchup25; default 0 = off, today's behaviour): a turn that starts more than this long after the one before it
// (a stall: a shader compile, a long load step, a GPU hang; the game thread's own turn or its wait for the front end / back end) is
// treated as on time by the pacing rules: their "behind the display's clock" debt is dropped instead of being paid back by turns without a
// picture (the game would run those turns as fast as it can: a visible jump of up to 4 turns, 66 ms, after a stall of 100-250 ms; stalls
// over 250 ms, 15 turns, were already forgiven by the rules' own gap test). A stall then costs the stall and nothing more. Returns the
// threshold in ns, 0 = off; a non-zero value is held to 80 ms .. 10 s so that no ordinary slow frame (17-60 ms) can ever be forgiven.
inline int64_t StallForgiveNs(long ms) {
    if (ms <= 0) return 0;
    if (ms < 80) ms = 80;
    if (ms > 10000) ms = 10000;
    return static_cast<int64_t>(ms) * 1000000;
}
// GXV_DEBT_CAP=<ms> (strk27; default 0 = the cap of today, 4 refreshes = 66.7 ms; 17..67): the most "behind the display's clock" time the skip rules keep. A turn of 50-80 ms (below
// GXV_STALL_FORGIVE's 80 ms) leaves up to 4 refreshes of debt, paid back by up to 3-4 turns without a picture and a jump of that much game time in one picture. Strikers found the
// same shape in its fixed-step simulation (lessons8, MSC_SLIP_STEPS=2: a late frame advances the game by at most two steps; model: a 70 ms hitch every 150 pictures, holds of 3+ refreshes
// 0.6-0.8 % -> 0 %): the cap there is the number of steps made up, here the debt. 33 ms = two refreshes: at most two catch-up turns. Game speed pays (the game clock stays behind by the rest).
inline int64_t DebtCapNs(long ms) {
    if (ms <= 0) return 4 * 16683350;
    if (ms < 17) ms = 17;
    if (ms > 66) return 4 * 16683350;
    return static_cast<int64_t>(ms) * 1000000;
}
// GXV_HRW=5 (warp32 PATCH7): the re-projected 60 shows a picture in EVERY refresh (a first picture, then its warp), each held to its own turn's vblank. That needs
// GXV_PRESENT_SYNC=2, the 2-of-4 turn rhythm (GXV_SKIP_PATTERN=2) and no pacing mode that owns the display's callback: the fixed 30 cap (GXV_FPS_LIMIT=30) presents every
// second refresh by itself and switches the present sync off, so mode 5 was silently off on the console whenever a setting asked for the cap (c106, 13 Nov).
// With GXV_HRW=5 in the environment the pacing is therefore mode 5's own: no cap, no GXV_PACE, pattern 2 (unless another pattern is set), present sync 2.
// GXV_HRW_PACING_AUTO=0 turns that off (the switches are taken as given; mode 5 then says why it is off).
inline bool Hrw5Pacing() {
    const char* h = std::getenv("GXV_HRW");
    const char* a = std::getenv("GXV_HRW_PACING_AUTO");
    return h && std::atoi(h) == 5 && !(a && *a && std::atoi(a) == 0);
}
// Mode 3 is the fixed presentation policy, never the adaptive 20-picture fallback.
inline int ResolveMode(int fpsLimit, int adaptive, bool fixedSupported) {
    if (fpsLimit == 30 && fixedSupported) return 3;
    return adaptive == 1 || adaptive == 2 ? adaptive : 0;
}
// GXV_SLIP (default 0): bounded slip. A game whose pair of turns plus the draw costs a little more than two retraces (CPU-bound)
// either skips pictures to keep its speed (the frame-skip rules, which hold 100 % speed at a picture rate that falls under 30 once
// the pair no longer fits: in tests/pace_cpubound_model.cpp a pair of 34 ms gives about 27 a second, one of 36 ms about 17), or presents every second turn and lets
// the game clock fall behind real time by the little that is missing (a pair of 34 ms: 29.4 pictures at 98 % speed). The original
// console game slowed down rather than skipping, and 30 pictures is the least the owner accepts. The slip is bounded: only while
// the slowdown it needs stays under a cap (GXV_SLIP_MAX percent, default 15), and only for a sustained load.
//
// What it measures: the work of each turn, from its start to the retrace wait (the wait is not a cost), in two classes, the turns
// that draw (D) and those that do not (N). Every 32 turns it takes the means; the pair costs P = D + N. At two retraces of 16.683 ms:
//   game speed at one picture per pair   2R / P            (the slowdown is 1 - that)
//   pictures a second                    60 R / P
//   what the skip rule gets instead      60 (R - N) / (D - N) pictures at full speed (it skips until the debt is paid)
// The slip is engaged after two windows in a row with P over 2R (a pair that fits needs nothing: the skip rule then keeps 30 or
// more), a slowdown inside the cap, and a gain in pictures (over the skip rule's) of at least 1.5 times the speed it costs (a
// stretch the skip rule serves nearly as well is not worth a slower game). It is left after one window that fails the same tests
// with some margin, or whose measured speed (turns against the clock, the waits included) is below the cap's: a wait that is not
// the one the estimate assumes cannot take the game under the cap, and the slip then stays off for about 20 s. While engaged:
//  - a late turn does not skip its picture: the controllers see no debt (the retrace wait of a turn that is late has no wait to
//    do: the game's retrace queue already holds the retrace that passed, MainLoopFramework waitForTick), so the game runs at its
//    own pace;
//  - the cadence is a picture every second turn (the fixed 30 policy has it already; the Up-to-60 rule alternates).
// No catch-up: the slipped time is not paid back. The game's logic counts turns and has no other clock to follow, music and sound
// run on the audio device's clock (platform/src/audio/ai_dsp.cpp aiClock: JAudio mixes a block per interrupt of the device's
// pace; the rhythm sequence follows the stream position itself, AudBgmTempoAdjuster, and the beats the game reads are stamped with
// OSGetTime, AudRhythmHolder) and sounds are triggered by game events, so nothing accumulates but the offset between a cutscene's
// animation and its music (the slowdown times the length of the scene, under the cap); running turns faster than 60 a second to
// pay it back would be a visible fast forward, and the console game never did. The reference is dropped at once (the debt is
// reset every turn while engaged and the frame-skip rule starts from zero when the slip is left); lag_ keeps the total for the log.
class Slip {
public:
    static constexpr int64_t kPeriodUs = 16683;
    static constexpr unsigned kWindow = 32;      // turns per window
    // maxPct: the largest slowdown, percent of real time; 0 (the default) is off, and every member then answers as if the class
    // were not there.
    void Configure(unsigned maxPct) {
        max_ = maxPct > 50 ? 50 : maxPct;
        Reset();
    }
    bool Enabled() const { return max_ != 0; }
    bool Engaged() const { return engaged_; }
    unsigned MaxPercent() const { return max_; }
    unsigned Changes() const { return changes_; }
    uint64_t LagUs() const { return lag_; }
    double PairUs() const { return pairUs_; }                   // the last window's pair cost
    double SpeedPercent() const { return pairUs_ > 0 ? 100.0 * 2 * kPeriodUs / pairUs_ : 100.0; }

    // The start of a turn (the controllers ask for a decision right after). allowed false: a load, a scene change; a gap in the
    // turns (a pause, a long load): nothing to learn from, nothing to slip about.
    void Begin(uint64_t nowUs, bool allowed) {
        if (!max_) return;
        const bool gap = began_ && (nowUs < last_ || nowUs - last_ > 250000);
        if (!allowed || gap) Reset();
        if (engaged_) ++epTurns_;
        began_ = true;
        last_ = start_ = nowUs;
        open_ = allowed && !gap;
        if (open_ && turns_ == 0) first_ = nowUs;
    }
    // The end of the turn's work, before the retrace wait. drew: the turn drew a picture.
    void End(uint64_t nowUs, bool drew) {
        if (!max_ || !open_) return;
        open_ = false;
        int64_t w = nowUs >= start_ ? static_cast<int64_t>(nowUs - start_) : 0;
        if (w > 4 * kPeriodUs) w = 4 * kPeriodUs;
        if (drew) { sumD_ += w; ++nD_; } else { sumN_ += w; ++nN_; }
        if (++turns_ >= kWindow) Evaluate(nowUs);
    }

private:
    void Reset() {
        if (engaged_) ++changes_;  // (left: a load, a pause, a scene change)
        engaged_ = false;
        good_ = 0;
        ClearWindow();
        open_ = false;
    }
    void ClearWindow() { sumD_ = sumN_ = 0; nD_ = nN_ = turns_ = 0; }
    void Evaluate(uint64_t nowUs) {
        // (a window needs both classes: before the slip is engaged the Up-to-60 rule has few turns of the second one)
        if (nD_ >= 4 && nN_ >= 4) {
            const double r = static_cast<double>(kPeriodUs);
            const double d = static_cast<double>(sumD_) / nD_, n = static_cast<double>(sumN_) / nN_, pair = d + n;
            pairUs_ = pair;
            const double slowdown = pair > 2 * r ? 1.0 - 2 * r / pair : 0.0;
            const double slipPictures = 60.0 * r / pair;
            // The skip rule's picture rate at full speed (a turn that skips costs n < r; if it does not, nothing is held).
            const double rulePictures = (d > n && n < r) ? 60.0 * (r - n) / (d - n) : 0.0;
            const double gain = 1.0 + 1.5 * slowdown;
            const bool enter = holdoff_ == 0 && pair > 2 * r * 1.005 && slowdown * 100.0 <= max_ && slipPictures >= rulePictures * gain;
            const bool stay = pair > 2 * r * 0.985 && slowdown * 100.0 <= max_ + 2 && slipPictures >= rulePictures * (1.0 + slowdown);
            // The speed the clock shows over this window's turns (31 whole turns, from the first start to the last).
            const double measured = start_ > first_ ? 31.0 * r / static_cast<double>(start_ - first_) : 1.0;
            if (!engaged_) {
                if (enter) {
                    if (++good_ >= 2) {
                        engaged_ = true;
                        ++changes_;
                        epStart_ = nowUs;
                        epTurns_ = 0;
                    }
                } else {
                    good_ = 0;
                }
            } else if (!stay || measured * 100.0 < 100.0 - max_ - 2.0) {
                if (stay) holdoff_ = 40;  // (the clock disagrees with the estimate: leave it alone for a while)
                engaged_ = false;
                good_ = 0;
                ++changes_;
                const int64_t behind = static_cast<int64_t>(nowUs - epStart_) - static_cast<int64_t>(epTurns_) * kPeriodUs;
                if (behind > 0) lag_ += static_cast<uint64_t>(behind);
            }
        }
        if (holdoff_) --holdoff_;
        ClearWindow();
    }
    unsigned max_ = 0, good_ = 0, changes_ = 0, holdoff_ = 0;
    bool engaged_ = false, began_ = false, open_ = false;
    uint64_t last_ = 0, start_ = 0, first_ = 0, epStart_ = 0, epTurns_ = 0, lag_ = 0;
    int64_t sumD_ = 0, sumN_ = 0;
    unsigned nD_ = 0, nN_ = 0, turns_ = 0;
    double pairUs_ = 0;
};
// The one the game's turn loop uses (port/perf.cpp starts and ends the turns; the fixed 30 controller reads it).
inline Slip& GlobalSlip() {
    static Slip slip;
    return slip;
}

// The frame-skip rule of port/perf.cpp port_frameskip_turn for the handhelds' "Up to 60", as a class so that the host model can run
// the same code. A turn that starts more than half a refresh behind the display's clock skips its picture (at most maxRun in a row;
// code 2: no retrace wait, it is late); a back end that holds pipeDepth pictures it has not finished makes the turn skip too (at
// most maxPipeRun in a row; code 1: not late, it takes its retrace). With the slip engaged (Slip above) the debt is not kept, a late
// turn does not skip, and the pictures come every second turn (code 1 for the other).
//
// GXV_SKIP_MAXSTEP=N (maxStep; 0 = off, the default; 2-8): a picture at least every N turns, whatever the reason to skip. The two
// rules above count one run together but each against its own limit (maxRun 5 for a late turn, maxPipeRun 6 for a busy back end),
// so a turn that ran long (the draw-done wait for a front end at ~28 ms a picture, lit Observatory at zoom 1.0) followed by a full
// back-end queue skips 3-6 turns in a row: that picture stays on screen 4-7 vblanks (67-117 ms). r49 (lit hub, steady play): the
// "present:" line shows 8-12 pictures every 5 s that advanced 4 or more turns, and the basic log's "longest" is 77-93 ms in every
// window, while no back-end frame took over 42 ms ("stutter-5s: in play ... longest"): the long gaps are the skip runs, not work.
// With the cap the turn that would make the run N long draws instead; if the pipeline is still full, the game thread waits for it
// (the draw-done wait, the frame queue), which costs game speed for that moment rather than a picture held for a long time.
// tests/skip_maxstep_model.cpp: on a model of that pipeline, 3 removes every gap of 4+ turns at no loss of pictures a second.
// capped: the turn would have skipped and draws because of the cap (counted for the log).
class LegacySkip {
public:
    static constexpr int64_t kPeriodNs = 16683350;
    // forgiven: the turn started more than the stall threshold (GXV_STALL_FORGIVE, StallForgiveNs) after the one before: its debt was dropped
    // (gapNs: that gap, debtNs: the debt it would have had, for the log).
    struct Verdict { int code; bool behind, gpuBusy; bool capped = false; bool forgiven = false; int64_t gapNs = 0, debtNs = 0; };
    LegacySkip(int maxRun, int pipeDepth, int maxPipeRun, const Slip* slip = nullptr, int maxStep = 0, int64_t forgiveNs = 0)
        : maxRun_(maxRun), pipeDepth_(pipeDepth), maxPipeRun_(maxPipeRun), slip_(slip),
          capRun_(maxStep >= 2 ? (maxStep > 8 ? 7 : maxStep - 1) : 1 << 30), forgive_(forgiveNs) {}
    uint64_t Forgiven() const { return forgivenCount_; }
    void SetDebtCap(int64_t ns) { debtCap_ = ns > 0 ? ns : 4 * kPeriodNs; }   // (GXV_DEBT_CAP)
    // GXV_SPEED_GUARD (speed_guard.h): the turn that Turn just answered with 0 (a picture) is left out by the guard instead (an extra turn without a picture). Counts as one more
    // turn of the run of skipped turns (the step cap then makes the NEXT turn draw: a three-turn picture, not a four-turn one) and gives back the slip cadence's step. Never
    // called with the guard off.
    void ExtraTaken() {
        run_ = runBefore_ + 1;
        if (cadenceStep_) --cadence_;
    }
    // (speed31) the guard owns the repayment of lost time (SpeedGuard::Owns()): no debt is kept. Set before every Turn; never called with the guard off.
    void SetOwns(bool on) { owns_ = on; }
    Verdict Turn(int64_t nowNs, bool allowed, unsigned pending) {
        const int64_t dt = last_ ? nowNs - last_ : kPeriodNs;
        last_ = nowNs;
        runBefore_ = run_;
        cadenceStep_ = false;
        if (dt > 15 * kPeriodNs || !allowed) {  // a load, a pause, a scene change: nothing to catch up with
            debt_ = 0;
            run_ = 0;
            return {0, false, false};
        }
        bool forgiven = false;
        int64_t wouldOwe = 0;
        if (forgive_ && dt > forgive_) {  // (GXV_STALL_FORGIVE: a stall costs the stall; the time is not paid back by turns without a picture)
            forgiven = true;
            wouldOwe = std::min<int64_t>(std::max<int64_t>(debt_ + dt - kPeriodNs, 0), debtCap_);
            debt_ = 0;
            ++forgivenCount_;
        } else {
            debt_ += dt - kPeriodNs;
            if (debt_ < 0) debt_ = 0;
            if (debt_ > debtCap_) debt_ = debtCap_;
        }
        const bool engaged = slip_ && slip_->Engaged();
        if (engaged) {
            if (!wasEngaged_) cadence_ = 0;
            debt_ = 0;
        }
        wasEngaged_ = engaged;
        if (owns_) debt_ = 0;  // (speed31, GXV_SPEED_GUARD_OWN: the speed guard repays the lost time while it is active; the rule's own catch-up would pay it twice)
        bool behind = debt_ >= kPeriodNs / 2 && run_ < maxRun_;
        bool gpuBusy = pipeDepth_ > 0 && pending >= static_cast<unsigned>(pipeDepth_) && run_ < maxPipeRun_;
        bool capped = false;
        if ((behind || gpuBusy) && run_ >= capRun_) {  // (GXV_SKIP_MAXSTEP: this turn draws; the debt is kept for the turns after it)
            capped = true;
            behind = gpuBusy = false;
        }
        Verdict v{0, false, false};
        v.forgiven = forgiven;
        v.gapNs = forgiven ? dt : 0;
        v.debtNs = wouldOwe;
        if (behind || gpuBusy) {
            ++run_;
            v.code = behind ? 2 : 1;
            v.behind = behind;
            v.gpuBusy = gpuBusy;
            return v;
        }
        run_ = 0;
        if (capped) { v.capped = true; return v; }  // (not left out by the slip's cadence either)
        if (engaged) {
            cadenceStep_ = true;
            if (cadence_++ & 1) v.code = 1;
        }
        return v;
    }

private:
    int runBefore_ = 0;
    bool cadenceStep_ = false, owns_ = false;
    int maxRun_, pipeDepth_, maxPipeRun_;
    const Slip* slip_;
    int capRun_;  // skipped turns allowed in a row (GXV_SKIP_MAXSTEP - 1; very large when off)
    int64_t forgive_ = 0;  // GXV_STALL_FORGIVE in ns, 0 = off
    int64_t debtCap_ = 4 * kPeriodNs;  // GXV_DEBT_CAP
    uint64_t forgivenCount_ = 0;
    int64_t last_ = 0, debt_ = 0;
    int run_ = 0;
    unsigned cadence_ = 0;
    bool wasEngaged_ = false;
};

class Controller {
public:
    explicit Controller(bool verify = false, Policy policy = Policy::Adaptive) : verify_(verify), policy_(policy) {}
    void Reset() {
        const Slip* slip = slip_;
        const int64_t forgive = forgive_, debtCap = debtCapUs_;
        const bool pass = passOffer_;
        const unsigned evenQ = evenQ_;
        *this = Controller(verify_, policy_);
        slip_ = slip;
        forgive_ = forgive;
        debtCapUs_ = debtCap;
        passOffer_ = pass;
        evenQ_ = evenQ;
    }
    // The slip (above): while it is engaged the fixed 30 policy keeps every second turn's picture and does not skip a late one.
    void UseSlip(const Slip* slip) { slip_ = slip; }
    // GXV_PACE_PASSOFFER (smooth25; default 0 = the phase runs on whatever the turns do): the fixed 30's phase counts turns (a picture on every second turn), so a due
    // turn the rules leave out (a late turn, or a back end that holds two pictures) loses its picture's whole slot: the next one is two turns later and the picture
    // before it is held 4 vblanks (the "pace:" lines' 4+ column: 31 of 103 pictures in c54's last Honeyhive window). With it the offer passes on, as SteadyCadence's
    // does (frame_pace.h, "a 3-vblank hold"): the turn after the left-out one is due again, and a picture is never offered on two consecutive turns.
    void SetPassOffer(bool on) { passOffer_ = on; }
    // GXV_EVEN26_Q (pace26; Quality / Locked 30; busyAt 0 = off, today's rules): an even 2-vblank cadence. FixedTurn leaves a DUE turn out only when the back end holds
    // busyAt (default 4) pictures or more, not at 2 (a three-stage pipeline at 30 a second holds 2 as a matter of course: every such skip was a picture 4 vblanks
    // after the one before it, 31 of 103 pictures in c54's last Honeyhive window), and the late catch-up (code 2, a turn without its retrace wait) lands on the turn
    // that was to be left out anyway, never on the due one: a game thread that falls behind pays it back in its idle turns, the picture rhythm stays 2-2-2. The back end's gate
    // stalls the game thread for the moment instead (game speed for a moment, not a longer hold). A due turn that is left out passes its offer to the next turn (a 3-vblank hold, not 4).
    void SetEvenQ(unsigned busyAt) { evenQ_ = busyAt; }
    // GXV_STALL_FORGIVE (StallForgiveNs, in ns; 0 = off): a gap between two turns' starts above it drops the debt (no catch-up turns).
    void SetForgive(int64_t ns) { forgive_ = ns > 0 ? ns : 0; }
    // GXV_DEBT_CAP (DebtCapNs, in ns; 0 = 4 refreshes): the most debt kept.
    void SetDebtCap(int64_t ns) { debtCapUs_ = ns > 0 ? ns / 1000 : 66732; }
    uint64_t Forgiven() const { return forgivenCount_; }
    bool PresentAllowed() const { return presentAllowed_; }
    // GXV_SPEED_GUARD (speed_guard.h; the fixed 30 only): SetExtra(true) before a Turn lets that turn, when it is the DUE one and would draw, be left out instead (code 2: no retrace
    // wait) with the due turn staying the next one: a three-turn picture. ExtraTaken() says the last Turn did. Never called with the guard off.
    void SetExtra(bool on) { extra_ = on; }
    bool ExtraTaken() const { return extraTaken_; }
    // (speed31, GXV_SPEED_GUARD_OWN) the guard repays the lost time while it is active (SpeedGuard::Owns()): the late catch-up is off, as with the slip engaged. Set before every Turn.
    void SetOwns(bool on) { owns_ = on; }

    // One call per simulation turn; 1 keeps its retrace, 2 catches up without one.
    int Turn(uint64_t now, bool allowed, bool quiet, unsigned pending, uint64_t backendUs, uint64_t wholeUs = 0) {
        if (policy_ == Policy::Fixed30) return FixedTurn(now, allowed, pending);
        const uint64_t workUs = wholeUs > backendUs ? wholeUs : backendUs;
        const bool gap = started_ && (now < last_ || now - last_ > 250000);
        if (!started_ || gap || !allowed) {
            debt_ = 0;
            catchup_ = 0;
            phase_ = oracleRemaining_ = 0;
            windowAt_ = now;
            samples_ = bad_ = 0;
            roomKnown_ = true;
            if (gap || !allowed) {
                overUs_ = roomUs_ = 0;
                want_ = divisor_;
            }
        } else {
            const int64_t delta = static_cast<int64_t>(now - last_) - 16683;
            if (forgive_ && static_cast<int64_t>(now - last_) * 1000 > forgive_) { debt_ = 0; ++forgivenCount_; }
            else {
                debt_ += delta;
                if (debt_ < 0) debt_ = 0;
                if (debt_ > debtCapUs_) debt_ = debtCapUs_;
            }
        }
        if (!started_) changedAt_ = now;
        started_ = true;
        last_ = now;

        // Render-thread CPU alone cannot establish GPU headroom, especially while paced at 20.
        const bool recoveryKnown = wholeUs != 0 && workUs < 28000;
        // Do not carry completed good windows across bad evidence in the current partial window.
        if (!recoveryKnown || pending >= 2 || debt_ >= 8341) {
            roomUs_ = 0;
            if (want_ == 2) want_ = divisor_;
        }
        if (quiet && want_ != divisor_ && (want_ != 2 || recoveryKnown) && now >= changedAt_ && now - changedAt_ >= 5000000) {
            divisor_ = want_;
            changedAt_ = now;
            phase_ = oracleRemaining_ = 0;
            overUs_ = roomUs_ = 0;
            ++changes_;
        }
        const bool late = debt_ >= 8341 && catchup_ < 3;
        catchup_ = late ? catchup_ + 1 : 0;
        const bool due = phase_ == 0;
        phase_ = (phase_ + 1) % divisor_;
        const int decision = !allowed ? 0 : late ? 2 : (!due || pending >= 2) ? 1 : 0;

        if (verify_) {
            // Independent countdown oracle for picture selection, not game logic.
            const bool oracleDue = oracleRemaining_ == 0;
            oracleRemaining_ = oracleDue ? divisor_ - 1 : oracleRemaining_ - 1;
            const int expected = !allowed ? 0 : late ? 2 : (!oracleDue || pending >= 2) ? 1 : 0;
            if (decision != expected) ++mismatches_;
        }

        if (quiet || !allowed || gap) {
            windowAt_ = now;
            samples_ = bad_ = 0;
            roomKnown_ = true;
        } else {
            ++samples_;
            roomKnown_ = roomKnown_ && recoveryKnown;
            // A single missed deadline is not evidence for a slower mode.
            if (late || pending >= 2 || workUs > 34500) ++bad_;
            if (now >= windowAt_ && now - windowAt_ >= 1000000) {
                const uint64_t span = now - windowAt_;
                if (samples_ && bad_ * 5 >= samples_) {
                    overUs_ += span;
                    roomUs_ = 0;
                    want_ = divisor_;
                } else if (bad_ == 0 && roomKnown_) {
                    roomUs_ += span;
                    overUs_ = 0;
                    want_ = divisor_;
                } else {
                    overUs_ = roomUs_ = 0;
                    want_ = divisor_;
                }
                if (overUs_ >= 3000000) want_ = 3;
                if (roomUs_ >= 6000000) want_ = 2;
                windowAt_ = now;
                samples_ = bad_ = 0;
                roomKnown_ = true;
            }
        }
        return decision;
    }

    unsigned Divisor() const { return divisor_; }
    unsigned Changes() const { return changes_; }
    uint64_t Mismatches() const { return mismatches_; }

private:
    int FixedTurn(uint64_t now, bool allowed, unsigned pending) {
        extraTaken_ = false;
        const bool gap = started_ && (now < last_ || now - last_ > 250000);
        if (!started_ || gap) {
            phase_ = 0;
            forcePresent_ = true;
        }
        if (!started_ || gap || !allowed) {
            debt_ = 0;
            catchup_ = 0;
        } else {
            if (forgive_ && static_cast<int64_t>(now - last_) * 1000 > forgive_) { debt_ = 0; catchup_ = 0; ++forgivenCount_; }
            else {
                debt_ += static_cast<int64_t>(now - last_) - 16683;
                if (debt_ < 0) debt_ = 0;
                if (debt_ > debtCapUs_) debt_ = debtCapUs_;
            }
        }
        started_ = true;
        last_ = now;
        const bool absorb = (slip_ && slip_->Engaged()) || owns_;
        if (absorb) debt_ = 0;  // (the slipped time is not caught up: Slip; speed31: nor while the speed guard repays it)
        const bool late = debt_ >= 8341 && catchup_ < 3;
        const bool due = phase_ == 0;
        phase_ = (phase_ + 1) % 2;
        int decision;
        if (evenQ_ && allowed) {  // (GXV_EVEN26_Q: a due turn draws; the catch-up and the retrace wait go to the turn that is left out anyway)
            decision = due ? (pending >= evenQ_ ? 1 : 0) : (late ? 2 : 1);
            catchup_ = (late && decision == 2) ? catchup_ + 1 : (late ? catchup_ : 0);
            if (due && decision != 0) phase_ = 0;
        } else {
            catchup_ = late ? catchup_ + 1 : 0;
            decision = !allowed ? 0 : late ? 2 : (!due || pending >= 2) ? 1 : 0;
        }
        if (extra_ && allowed && due && decision == 0) {  // (GXV_SPEED_GUARD: one more turn without a picture; the next turn is the due one again)
            decision = 2;
            phase_ = 0;
            extraTaken_ = true;
        }
        extra_ = false;
        if (passOffer_ && due && decision != 0) phase_ = 0;  // (smooth25: the left-out turn's picture is offered to the next turn, not to the one after it)
        if (decision == 0) {
            // Count emitted pictures, not nominal slots: backlog may skip every due turn.
            presentAllowed_ = forcePresent_ || allowed || due || previousHidden_;
            previousHidden_ = !presentAllowed_;
            forcePresent_ = false;
        }
        return decision;
    }
    uint64_t last_ = 0, changedAt_ = 0, windowAt_ = 0, overUs_ = 0, roomUs_ = 0, mismatches_ = 0;
    int64_t debt_ = 0, forgive_ = 0, debtCapUs_ = 66732;
    uint64_t forgivenCount_ = 0;
    unsigned divisor_ = 2, want_ = 2, phase_ = 0, oracleRemaining_ = 0, evenQ_ = 0;
    unsigned catchup_ = 0, samples_ = 0, bad_ = 0, changes_ = 0;
    bool started_ = false, verify_ = false, roomKnown_ = true, passOffer_ = false, extra_ = false, extraTaken_ = false, owns_ = false;
    Policy policy_ = Policy::Adaptive;
    const Slip* slip_ = nullptr;
    bool presentAllowed_ = true, previousHidden_ = false, forcePresent_ = true;
};

class PresentationSlots {
public:
    void Reset() { started_ = false; }
    // GXV_EVEN26_QSLACK (pace26; default 0 = as before): the first picture of a rhythm (and every restart of it) is put `slack` vblanks after the vblank it could have taken. The
    // slots run on from there every `divisor` vblanks, so every picture then has `slack` vblanks of room for a GPU or back-end time that runs long, instead of the slot being
    // missed (a 3-vblank hold and the rhythm moved later for good). The price is `slack` vblanks of latency, once.
    void SetSlack(unsigned slack) { slack_ = slack > 3 ? 3 : slack; }
    // earliest is the next scanout vblank (GetVcount + 1), after GPU completion.
    uint32_t Next(uint32_t earliest, unsigned divisor) {
        if (!started_ || divisor != divisor_) {
            started_ = true;
            divisor_ = divisor;
            target_ = earliest + slack_;
            nominal_ = target_;
            return target_;
        }
        target_ += divisor_;
        nominal_ = target_;   // (q30cush31b: the slot the rhythm asked for, before a late picture pulls it forward to `earliest`)
        if (static_cast<int32_t>(earliest - target_) > 0) target_ = earliest;
        return target_;
    }
    // q30cush31b: the slot of the LAST Next() call before the late-picture clamp. A picture ready after it has Next() > Nominal(): the margin must be measured against Nominal(), not
    // against the clamped slot (which is always the next vblank, so the margin against it is never negative and a late picture looks on time).
    uint32_t Nominal() const { return nominal_; }
    void Shown(uint32_t actual) { target_ = actual; }

private:
    uint32_t target_ = 0, nominal_ = 0;
    unsigned divisor_ = 2, slack_ = 0;
    bool started_ = false;
};

// The wait for a presentation slot, off the display hardware. Display: uint32_t Count() (the vblank counter) and int WaitVblank()
// (blocks until the next vblank starts; negative: the wait failed, stop). Returns the vblank the picture will be on.
//
// The older loop (countWaits false) trusts the counter after every wait. If the counter is read before it was updated for the vblank
// that just woke the thread, the loop thinks that vblank has not come yet and waits for one more: a 30 FPS picture is held for three
// vblanks instead of two, once. countWaits true counts a wait that returned as a vblank that passed, so the picture is never later
// than the slot (the counter is still believed when it is ahead, e.g. after a thread slept through several vblanks).
struct SlotWaitResult {
    uint32_t shown = 0;     // the vblank the picture will be shown on
    unsigned waits = 0;     // vblank waits made (0: the slot had already come)
    bool failed = false;    // WaitVblank reported an error
};
template <class Display> SlotWaitResult WaitForSlot(Display& d, uint32_t due, bool countWaits, unsigned maxWaits = 8) {
    SlotWaitResult r;
    r.shown = static_cast<uint32_t>(d.Count()) + 1;
    while (static_cast<int32_t>(due - r.shown) > 0 && r.waits < maxWaits) {
        if (d.WaitVblank() < 0) {
            r.failed = true;
            break;
        }
        ++r.waits;
        uint32_t seen = static_cast<uint32_t>(d.Count()) + 1;
        if (countWaits && static_cast<int32_t>(seen - (r.shown + 1)) < 0) seen = r.shown + 1;
        r.shown = seen;
    }
    return r;
}
// ---- GXV_PRESENT_SYNC: pictures shown on the vblank that matches their game time ---------------------------------------------
// At "Game speed Full" the game runs 60 turns a second on the display's own vblank clock (vgx_host_galaxy_vita.cpp VblankMain) and
// draws only the turns the pipeline has room for (about 45 a second at the Observatory). Without this, a picture goes to the display
// at the first vblank after the GPU finished it (vitaGL's display queue, no vsync wait). How long that takes from the start of its
// turn varies from picture to picture (the front end's 16-30 ms, the back end, the GPU), so the time on screen between two pictures
// is not the game time between them: a picture that came 2 turns after the last can be held for 1 vblank, and the next one, 1 turn
// later, for 2. Mario then moves at double and then half his speed for a picture each: the "obvious frame skipping" (judder), on top
// of the uneven 1/2-vblank rhythm of 45 pictures on a 60 Hz display, which alone keeps every position right.
//
// Each drawn turn carries its vblank number (TurnClock below, sent with the picture through the GX stream as a BP 0xFF marker,
// vgx_state.h kPresentStampMarker), and the display callback holds the picture until vblank stamp + L, L the pipeline's latency in
// vblanks (PresentSync below). A picture that is in time is then shown exactly L vblanks after its turn began: the screen's time
// and the game's time advance together and Mario's displayed speed is constant. L adapts: up by one when the pictures keep missing
// it (GXV_PRESENT_SYNC_RAISE, default 4 of the last 32), down by one after ~4 s in which (almost) every picture would also have met L - 1, with a hold-off after a
// raise so that it does not flip; a picture over the largest L (a pause menu at 10 fps, a shader stall) is shown when ready and does
// not raise it. The cost: the latency of the slower pictures for all of them (about half to one vblank more on average than "as
// soon as ready"); the game's speed (60 turns a second) and which turns are drawn are not changed.

// The game thread's clock, one call per turn: the vblank the turn belongs to. It advances one per turn and follows the display
// counter: never ahead of it (a turn that started late after a pause, a load, a slow turn: the game lost that time) and never more
// than kMaxBehind behind it (the frame-skip rule's debt is capped at 4 turns: the turns it does not catch up are lost time too).
class TurnClock {
public:
    static constexpr int32_t kMaxBehind = 4;
    uint32_t Turn(uint32_t vcount) {
        if (!started_) {
            started_ = true;
            logical_ = vcount;
            return logical_;
        }
        ++logical_;
        const int32_t ahead = static_cast<int32_t>(logical_ - vcount);
        if (ahead > 0) logical_ = vcount;
        else if (ahead < -kMaxBehind) logical_ = vcount - kMaxBehind;
        return logical_;
    }
    void Reset() { started_ = false; }  // (the next turn takes the display's counter as it is: GXV_STALL_FORGIVE after a stall)

private:
    uint32_t logical_ = 0;
    bool started_ = false;
};

// The display side, one Target + Shown per picture (on the display queue's callback). Vblank numbers are the display's counter
// (sceDisplayGetVcount); a stamp is its low 16 bits.
class PresentSync {
public:
    static constexpr unsigned kSane = 60;   // a stamp more than a second old is not this picture's (a reset, a settings hold)
    struct Stats {
        uint64_t pictures = 0, stamped = 0, waits = 0, misses = 0, overload = 0, replaced = 0, raises = 0, lowers = 0;
        uint64_t hold[5] = {}, step[5] = {}, err[4] = {};  // vblanks held, turns advanced (index 4: 4 or more); |hold - step| 0..3+
        uint64_t natural[8] = {};                          // ready-to-show minus stamp (vblanks), 0..7+
        uint64_t errSum = 0, pairs = 0;                    // sum of |hold - step| over consecutive stamped pictures
        int64_t latSum = 0, latSq = 0;                     // shown - stamp: its mean and spread (the timing judder)
        // GXV_LATENCY24 (Latency24 below; counted always, they only read): entries that came straight after the previous callback's return (the
        // GPU had finished the picture earlier: the readiness is only known to be no later), late pictures that the largest latency would have
        // shown on time (the ones a lower latency adds), lowerings that failed (undone on probation), the lowest and highest latency held.
        uint64_t censored = 0, lateTighter = 0, probeFails = 0, calmReleases = 0;
        unsigned latMin = 99, latMax = 0;
    };
    // GXV_LATENCY24: the latency rule that sees the display queue's backlog. Without it (on false) every decision below is the rule of
    // GXV_PRESENT_SYNC (it came down only after 180 pictures with at most one that would have missed L - 1 - and counted a picture that waited
    // behind the previous callback's hold as one that would have missed, so a latency above what the pictures need could never come down while
    // the pictures were held: "ready after" read L for every picture). With it:
    //  - an entry that came within censorUs of the previous callback's return is censored (NoteBacklog): the GPU had finished the picture
    //    earlier and the entry only shows when the previous callback let go. It is evidence all the same: the picture was ready no later than
    //    the previous picture's slot, so it counts as one that would have met L - 1 when that slot is at most L - 1 vblanks after its own
    //    turn (not when the previous callback was not held, or when the chain of slots lies above L);
    //  - every `window` pictures (four times as many when most of them were on consecutive turns), L comes down by one when the evidence covers at least minKnown pictures and at most tolPerMille per mille of
    //    them would have missed L - 1 (the old rule: 180 pictures, one over); never without evidence: no probing;
    //  - a lowering is on probation for `probation` pictures: `probeMisses` late ones raise L back at once, and the next lowering is held
    //    off for probeBase pictures, doubled by every failed one up to probeMax. After an ordinary raise it is held off for `holdoff` pictures;
    //  - the latency that is on screen is the chain of slots, one picture a vblank (Target), not L: with pictures on consecutive vblanks a
    //    lower L changes nothing until a turn is left out; with catchUp the picture whose slot the one before holds replaces it (that one is
    //    never shown: one lost picture for a vblank of latency, at most one in dropGap);
    //  - L does not go under lowFloor.
    struct Latency24 {
        bool on = false;
        unsigned window = 90, tolPerMille = 5, minKnown = 45, probation = 48, probeMisses = 2, lowFloor = 2, holdoff = 150;
        unsigned probeBase = 300, probeMax = 4800;
        uint32_t censorUs = 1200;
        // pace33 PATCH3 (console c118: "lowered 5, raised 5, lowerings failed 0" a run: the raise came after the 48 pictures of probation, so nothing backed off and L went down and up every
        // ~40 s, four late pictures and one given up each time): a raise within regretPics pictures of a lowering is a failed lowering too (the next one waits Backoff(): 300, 600 .. 4800 pictures).
        // 0 = off = as before.
        unsigned regretPics = 0;
        // pace33 PATCH10 (console c140 defaults: Dreadnought sat at L 5 = 103 ms pad to flip for 80 s with 0 late pictures and every entry waiting behind the previous callback, because two
        // lowerings had failed in the stage's opening and the hold-off was 4800 pictures; Honeyhive 30 s at 102 ms the same way): a hold-off after a failed lowering ends early after a
        // CLEAN run: calmBase << fails pictures in a row (GXV_LATENCY24_CALM 450: 900, 1800, at most calmMax 2400) with no late picture and each one evidence that L - 1 would have been met. One late or
        // unproven picture starts the run again. The lowering itself still needs its window of evidence. 0 = off = the hold-off runs its whole time.
        unsigned calmBase = 0, calmMax = 2400;
        // thr24 (GXV_LATENCY24_FAST, default 0 = as before; console night58: Dreadnought / Honeyhive / Gusty Garden spent 600-1200 pictures of a minute at L 4-5 with no late picture at all, the hold-off after a failed
        // lowering running on: probeMax is 14400 with the drain's regret rule). All three are 0 = off. holdCap: no hold-off after a failed lowering is longer than this many pictures (1200 = 20 s at 60);
        // regretMul: the multiplier of a regret's hold-off in place of 4 (2); cleanPics: the hold-off ends after this many clean pictures in a row whatever the number of failures (180 = 3 s at 60;
        // the lowering itself still needs its window of evidence, and a lowering that fails again is undone within probation as before).
        unsigned holdCap = 0, regretMul = 0, cleanPics = 0;
        bool catchUp = false;       // a picture whose slot is taken by the one before (pictures on consecutive vblanks, L lowered) replaces it
        unsigned dropGap = 24;      // ... at most once in this many pictures
    };
    // align false: measure only (every picture is shown when ready, as without the switch). minLat..maxLat: L's range.
    // raise: late pictures among the last 32 that raise the latency by one (the tolerance: more is smoother, fewer is quicker).
    void Configure(bool align, unsigned minLat, unsigned maxLat, unsigned initial, unsigned raise = 4) {
        align_ = align;
        raise_ = raise < 1 ? 1 : (raise > 32 ? 32 : raise);
        min_ = minLat < 1 ? 1 : (minLat > 8 ? 8 : minLat);
        max_ = maxLat < min_ ? min_ : (maxLat > 8 ? 8 : maxLat);
        lat_ = initial < min_ ? min_ : (initial > max_ ? max_ : initial);
        Reset();
    }
    void Reset() {
        havePrev_ = false;
        haveShown_ = false;
        missBits_ = 0;
        run_ = runOver_ = 0;
        holdoff_ = 0;
        evHave_ = false;
        censored_ = prevHeld_ = false;
        known_ = knownOver_ = consec_ = probation_ = probeMiss_ = 0;
        onTime_ = 0;
    }
    void ConfigureLatency24(const Latency24& l) {
        l24_ = l;
        if (l24_.window < 8) l24_.window = 8;
        if (l24_.probeMisses < 1) l24_.probeMisses = 1;
        if (l24_.probeBase < 1) l24_.probeBase = 1;
        if (l24_.probeMax < l24_.probeBase) l24_.probeMax = l24_.probeBase;
        fails_ = 0;
        sinceRaise_ = 0;
    }
    bool Latency24On() const { return l24_.on; }
    // pace33 (GXV_DRAIN33, port/drain33.h): the catch-up's "at most once in dropGap pictures" is replaced by the caller's word, given per picture before Target (AllowDrop). Behind() is the
    // catch-up's own condition for the picture about to be given to Target: its slot by L is earlier than the chain's next vblank, and it came in time to take the previous picture's.
    // Dropped() after Target: this picture took the previous one's vblank (that one is never seen).
    void SetExternalDrop(bool on) { extDrop_ = on; }
    // Between Target and Shown: this picture reached the callback less than the caller's margin before the vblank it had to beat. It is no evidence that a lower latency would hold
    // (it counts as one that would have missed L - 1): the latency does not come down to a level the pictures meet by a hair, where every late one is a repeated picture.
    void NoteTight(bool tight) { tight_ = tight; }
    void AllowDrop(bool allow) { extAllow_ = allow; }
    bool Behind(uint32_t earliest, bool valid, uint16_t stamp) const {
        if (!valid || !align_ || !haveShown_ || !l24_.on || !l24_.catchUp) return false;
        const uint32_t natural = static_cast<uint16_t>(static_cast<uint16_t>(earliest) - stamp);
        if (natural > kSane) return false;
        const uint32_t t = earliest - natural + lat_;
        return static_cast<int32_t>(t - (lastShown_ + 1)) < 0 && static_cast<int32_t>(lastShown_ - earliest) >= 0;
    }
    bool Dropped() const { return dropped_; }
    // Between Target and Shown: this entry came straight after the previous callback returned (see Latency24).
    void NoteBacklog(bool censored) { censored_ = censored; }
    unsigned Latency() const { return lat_; }
    // pace25 (GXV_PENDING_HOLD25): the pictures the skip rule may count as pending on top of its own limit. A picture stamped at turn t is shown at t + L, and the three display
    // buffers / two-entry display queue keep it with the back end (queued, not yet through the swap) for about L - 2 vblanks (the "present:" line's "ready after" 3-4 at L 5, the
    // render thread's "waits sema" + "display queue"): at one picture per vblank L - 2 pictures are pending BY DESIGN. LegacySkip's limit is 2, so from L 4 up every turn found the
    // back end busy and left its picture out, one in five, for good: 48.0 fps with the GPU at 15 ms, late 0 (night60 Beach Bowl 03:50:32-44, Good Egg 03:56:40-57:20). While the last 32 stamped pictures (overloaded ones too) were all on time at L, the wait is the latency's, not work: L - 3 more are allowed (the limit becomes L - 1). A late picture takes the allowance away at once: a back end that
    // really is behind (GPU bound) is held to the old limit as before. 0 when not aligned or L <= 3.
    // pace26 (GXV_PENDING_HOLD25=2, guarded): a GPU that needs 16.7-20 ms a picture is on time at L until the allowance lets one more picture in, then one is late (about one a second, 18 ms model).
    // A late picture that came while the allowance was on is a strike: the allowance then stays off for guardNeed_ pictures (counted whether or not they are late: 1800 = 30 s, 60, 120, 240 s cap) and then needs 32 on time (pace27: only a marginal late, 1-2 vblanks, within 8 pictures of the allowance being on is a strike), and the
    // strikes are forgiven after 3600 pictures with the allowance on and none late. HoldAllowance(true): the guarded rule. GuardEngaged(): strikes so far (the "pace: pending hold" line).
    unsigned HoldAllowance(bool guarded = false) const { return align_ && lat_ > 3 && onTime_ >= 32u && (!guarded || sinceStrike_ >= guardNeed_) ? lat_ - 3 : 0; }
    unsigned GuardEngaged() const { return guardCount_; }
    bool Aligned() const { return align_; }
    // earliest: the first vblank the picture can be on (counter + 1). valid / stamp: the picture's turn, if it has one.
    // Returns the vblank to show it on (earliest or later; never more than max_ + 1 later).
    uint32_t Target(uint32_t earliest, bool valid, uint16_t stamp) {
        ++stats_.pictures;
        cur_ = false;
        curHeld_ = false;
        dropped_ = false;
        uint32_t target = earliest;
        if (valid) {
            const uint32_t natural = static_cast<uint16_t>(static_cast<uint16_t>(earliest) - stamp);
            if (natural <= kSane) {
                cur_ = true;
                curStamp_ = earliest - natural;
                curNatural_ = natural;
                ++stats_.stamped;
                ++stats_.natural[natural < 7 ? natural : 7];
                if (align_) {
                    uint32_t t = curStamp_ + lat_;
                    if (sinceDrop_ < 1000000u) ++sinceDrop_;
                    if (haveShown_ && static_cast<int32_t>(t - (lastShown_ + 1)) < 0) {
                        // one picture per vblank (GXV_LATENCY24 catchUp: not when L is lower than the chain of slots and the previous picture has not
                        // been on the screen yet: this one takes its vblank, and the previous one is never shown)
                        if (l24_.on && l24_.catchUp && (extDrop_ ? extAllow_ : sinceDrop_ >= l24_.dropGap) && static_cast<int32_t>(lastShown_ - earliest) >= 0) {
                            t = lastShown_;
                            sinceDrop_ = 0;
                            dropped_ = true;
                        } else {
                            t = lastShown_ + 1;
                        }
                    }
                    if (static_cast<int32_t>(t - earliest) > static_cast<int32_t>(max_ + 1)) t = earliest + max_ + 1;
                    if (static_cast<int32_t>(t - earliest) > 0) {
                        target = t;
                        curHeld_ = true;
                    }
                }
            }
        }
        if (!cur_) havePrev_ = false;  // (the next stamped picture starts a new pair)
        return target;
    }
    // The vblank the picture is on (after any wait); waited: a wait was made.
    void Shown(uint32_t shown, bool waited) {
        if (waited) ++stats_.waits;
        if (haveShown_ && shown == lastShown_) ++stats_.replaced;  // (two pictures in one vblank: the first was never seen)
        evPrevShown_ = lastShown_;
        evHave_ = haveShown_;
        evPrevStamp_ = prevStamp_;
        evHavePrev_ = havePrev_;
        lastShown_ = shown;
        haveShown_ = true;
        if (!cur_) {
            censored_ = prevHeld_ = false;
            return;
        }
        {   // (pace27) guarded allowance: armed = on time 32 pictures, L > 3 and the back-off (counted in pictures, late or not) is over
            ++sinceStrike_;
            const bool armed = lat_ > 3 && onTime_ >= 32u && sinceStrike_ >= guardNeed_;
            if (armed) armedRecent_ = 8u; else if (armedRecent_) --armedRecent_;
            const bool late = curNatural_ > lat_;
            if (late && armedRecent_ && curNatural_ <= lat_ + 2u) {   // (a strike: marginally late within 8 pictures of the allowance being on; a gross lateness is a load or a scene, not the allowance)
                if (strikes_ < 8u) ++strikes_;
                ++guardCount_;
                sinceStrike_ = 0;
                guardNeed_ = 1800u << (strikes_ > 4u ? 3u : strikes_ - 1u);   // 30 s, 60, 120, capped 240 s at 60 pictures a second
                armedRun_ = 0;
            } else if (late) armedRun_ = 0;
            else if (armed && ++armedRun_ >= 3600u && strikes_) { strikes_ = 0; guardNeed_ = 32u; }
        }
        onTime_ = curNatural_ <= lat_ ? (onTime_ < 1000000u ? onTime_ + 1u : onTime_) : 0u;   // (pace25: before Adapt, which may move L)
        const int32_t lat = static_cast<int32_t>(shown - curStamp_);
        stats_.latSum += lat;
        stats_.latSq += static_cast<int64_t>(lat) * lat;
        if (havePrev_) {
            const int32_t hold = static_cast<int32_t>(shown - prevShown_), step = static_cast<int32_t>(curStamp_ - prevStamp_);
            if (hold >= 0 && step > 0 && hold < 64 && step < 64) {
                ++stats_.hold[hold < 4 ? hold : 4];
                ++stats_.step[step < 4 ? step : 4];
                const uint32_t e = static_cast<uint32_t>(hold > step ? hold - step : step - hold);
                ++stats_.err[e < 3 ? e : 3];
                stats_.errSum += e;
                ++stats_.pairs;
            }
        }
        prevShown_ = shown;
        prevStamp_ = curStamp_;
        havePrev_ = true;
        Adapt();
        prevHeld_ = curHeld_;
        if (lat_ < stats_.latMin) stats_.latMin = lat_;
        if (lat_ > stats_.latMax) stats_.latMax = lat_;
        censored_ = false;
    }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }

private:
    void Adapt() {
        const bool overload = curNatural_ > max_;           // no L in range would have it: show it, learn nothing
        const bool miss = curNatural_ > lat_;
        if (miss) {
            ++stats_.misses;
            if (curNatural_ <= max_) ++stats_.lateTighter;
        }
        if (censored_) ++stats_.censored;
        if (overload) {
            ++stats_.overload;
            return;
        }
        if (l24_.on && align_) {
            Adapt24(miss);
            return;
        }
        missBits_ = (missBits_ << 1) | (miss ? 1u : 0u);
        if (holdoff_) --holdoff_;
        if (!align_) return;
        unsigned recent = 0;
        for (uint32_t b = missBits_; b; b &= b - 1) ++recent;
        if (recent >= raise_ && lat_ < max_) {
            ++lat_;
            ++stats_.raises;
            missBits_ = 0;
            run_ = runOver_ = 0;
            holdoff_ = 300;  // (about 6 s at 45 pictures a second before it may come down again)
            return;
        }
        ++run_;
        if (curNatural_ + 1 > lat_) ++runOver_;  // would have missed L - 1
        if (run_ >= 180) {
            if (runOver_ <= 1 && lat_ > min_ && holdoff_ == 0) {
                --lat_;
                ++stats_.lowers;
                missBits_ = 0;
            }
            run_ = runOver_ = 0;
        }
    }
    void Adapt24(bool miss) {
        missBits_ = (missBits_ << 1) | (miss ? 1u : 0u);
        if (holdoff_) --holdoff_;
        if (sinceRaise_ < 100000u) ++sinceRaise_;
        if (sinceRaise_ == 2400u + regretHold_ && fails_ > 0) --fails_;  // (a long quiet at one latency forgives one failed probe; PATCH3: counted after a regret's own wait)
        bool clean = false;   // (PATCH10) this picture is evidence that L - 1 would have been met
        if (!censored_) {
            ++known_;
            if (curNatural_ >= lat_ || tight_) ++knownOver_;      // (would have missed L - 1; pace33: or met it by less than the margin)
            else clean = true;
        } else if (prevHeld_ && evHave_ && static_cast<int32_t>(evPrevShown_ - curStamp_) <= static_cast<int32_t>(lat_) - 1) {
            clean = true;
            ++known_;  // (the entry came straight after a callback that was held to its slot and returned a vblank before it: the picture was
                       //  ready no later than that slot, which is at most L - 1 vblanks after its own turn: it would have met L - 1 as well.
                       //  No evidence when the previous callback was not held, or when its slot lies later (the chain of slots is above L))
        }
        calm_ = clean && !miss ? calm_ + 1 : 0;
        if ((l24_.calmBase || l24_.cleanPics) && holdoff_ > l24_.holdoff && fails_ > 0) {   // (only the long waits after a failed lowering; an ordinary raise's hold-off runs out by itself)
            const uint64_t need64 = static_cast<uint64_t>(l24_.calmBase) << (fails_ < 8 ? fails_ : 8);
            const unsigned need0 = need64 > l24_.calmMax ? l24_.calmMax : static_cast<unsigned>(need64);
            const unsigned need = l24_.cleanPics ? l24_.cleanPics : need0;   // (thr24)
            if (calm_ >= need) { holdoff_ = 0; calm_ = 0; ++stats_.calmReleases; }
        }
        bool raise = false;
        if (probation_) {
            --probation_;
            if (miss) ++probeMiss_;
            if (probeMiss_ >= l24_.probeMisses && lat_ < max_) {  // a probe that fails is undone at once
                raise = true;
                ++stats_.probeFails;
                if (fails_ < 16) ++fails_;
                holdoff_ = Backoff();
                if (l24_.holdCap && holdoff_ > l24_.holdCap) holdoff_ = l24_.holdCap;   // (thr24)
            }
        }
        if (!raise) {
            unsigned recent = 0;
            for (uint32_t b = missBits_; b; b &= b - 1) ++recent;
            if (recent >= raise_ && lat_ < max_) {
                raise = true;
                holdoff_ = l24_.holdoff;
                if (l24_.regretPics && sinceLower_ < l24_.regretPics) {   // (PATCH3: the lowering did not hold)
                    ++stats_.probeFails;
                    if (fails_ < 16) ++fails_;
                    const unsigned b = Backoff();   // (four times a failed probe's wait: this one cost late pictures on the screen, a probe's two)
                    const unsigned mul = l24_.regretMul ? l24_.regretMul : 4u;   // (thr24)
                    holdoff_ = b > l24_.probeMax / mul ? l24_.probeMax : b * mul;
                    if (l24_.holdCap && holdoff_ > l24_.holdCap) holdoff_ = l24_.holdCap;
                    regretHold_ = holdoff_;
                }
            }
        }
        if (sinceLower_ < 1000000u) ++sinceLower_;
        if (raise) {
            ++lat_;
            ++stats_.raises;
            missBits_ = 0;
            run_ = known_ = knownOver_ = consec_ = 0;
            probation_ = probeMiss_ = 0;
            sinceRaise_ = 0;
            return;
        }
        if (evHavePrev_ && curStamp_ - evPrevStamp_ == 1u) ++consec_;
        ++run_;
        // (pictures on consecutive turns chain their slots: one picture that misses its slot pushes every later one by a vblank until a turn
        //  is left out or a picture is dropped. There the evidence must cover four times as many pictures)
        if (run_ < (consec_ * 2u > run_ ? l24_.window * 4u : l24_.window)) return;
        const unsigned floor = min_ > l24_.lowFloor ? min_ : l24_.lowFloor;
        bool lower = false;
        if (lat_ > floor && holdoff_ == 0 && probation_ == 0 && known_ >= l24_.minKnown) lower = knownOver_ * 1000u <= known_ * l24_.tolPerMille;
        run_ = known_ = knownOver_ = consec_ = 0;
        if (!lower) return;
        --lat_;
        ++stats_.lowers;
        missBits_ = 0;
        probation_ = l24_.probation;
        probeMiss_ = 0;
        sinceRaise_ = 0;
        sinceLower_ = 0;
    }
    unsigned Backoff() const {
        uint64_t b = static_cast<uint64_t>(l24_.probeBase) << (fails_ < 12 ? fails_ : 12);
        return b > l24_.probeMax ? l24_.probeMax : static_cast<unsigned>(b);
    }
    Stats stats_;
    Latency24 l24_;
    bool align_ = false, cur_ = false, havePrev_ = false, haveShown_ = false, censored_ = false, curHeld_ = false, prevHeld_ = false;
    unsigned consec_ = 0, known_ = 0, knownOver_ = 0, probation_ = 0, probeMiss_ = 0, fails_ = 0, sinceRaise_ = 0, sinceDrop_ = 0;
    bool extDrop_ = false, extAllow_ = false, dropped_ = false, tight_ = false;   // (pace33)
    unsigned regretHold_ = 0;
    unsigned guardNeed_ = 32, strikes_ = 0, guardCount_ = 0, sinceStrike_ = 1000000u, armedRecent_ = 0, armedRun_ = 0;                        // (pace26) clean pictures the guarded allowance needs; strikes; guard engaged count
    unsigned onTime_ = 0;                                                         // (pace25) stamped pictures in a row that were ready by their slot at the L of the time
    unsigned calm_ = 0;                                                           // (PATCH10) clean pictures in a row
    unsigned sinceLower_ = 1000000u;                                              // (PATCH3) stamped pictures since L last came down
    uint32_t evPrevShown_ = 0, evPrevStamp_ = 0;
    bool evHave_ = false, evHavePrev_ = false;
    unsigned min_ = 1, max_ = 5, lat_ = 3, run_ = 0, runOver_ = 0, holdoff_ = 0, raise_ = 4;
    uint32_t missBits_ = 0;
    uint32_t curStamp_ = 0, curNatural_ = 0, prevStamp_ = 0, prevShown_ = 0, lastShown_ = 0;
};

// ---- GXV_SKIP_PATTERN: which turns are drawn, in a fixed rhythm --------------------------------------------------------------
// N of every 4 turns are offered a picture (3: 45 a second, holds 1-1-2; 2: 30, holds 2-2); the frame-skip rule can still leave out
// an offered turn (late, or the back end busy), and the next turn is then offered instead so that no picture is lost to the rhythm.
// With GXV_PRESENT_SYNC the holds on screen are the turns between pictures, so a fixed rhythm is an even motion when the scene
// cannot hold 60; the price is pictures in a scene that could show more. 0 / 4: off (every turn offered).
class SkipPattern {
public:
    explicit SkipPattern(int perFour = 0) : n_(perFour < 1 || perFour > 3 ? 4 : perFour) {}
    bool Enabled() const { return n_ < 4; }
    // Whether this turn is offered a picture; call once per turn, with reset on a load / scene change.
    bool Offer(bool reset) {
        if (reset) { phase_ = 0; owed_ = false; }
        const unsigned p = phase_;
        phase_ = (phase_ + 1) & 3u;
        const bool due = ((p + 1) * n_) / 4 != (p * n_) / 4;
        return due || owed_;
    }
    // (gov5) the rhythm is not in force (60 Assist stands down: plain 60, every turn may draw): keeps it parked on a turn that is NOT offered, so that when it takes over again the turn after
    // a first picture (the drawn turn that engaged it) is the pair's undrawn turn, and the next one is offered. Call once per turn instead of Offer.
    void Park() { phase_ = 0; owed_ = false; }
    // The decision that was made for an offered turn: drawn, or left out by the frame-skip rule (then the next turn is offered).
    void Result(bool offered, bool drawn) {
        if (drawn) owed_ = false;
        else if (offered) owed_ = true;
    }

private:
    unsigned n_, phase_ = 0;
    bool owed_ = false;
};
// ---- GXV_PACE24: what the display really showed (pace24) -------------------------------------------------------------------
// The census "late" of the autoq floor line is taken where the render thread's swap returns, and with the display callback holding pictures
// to their slots (GXV_PRESENT_SYNC=2) that is not the screen's rhythm: 2705 "none of these" pictures in the c27 play windows, 55 % of them after
// a turn the skip rule left out, the rest one-turn spans whose swap came late because the display queue was full. The screen's own rhythm is the
// number of vblanks each picture stays up (the display callback knows the vblank it put it on). HoldMeter counts those holds and the
// "breaks": a picture held for a different number of vblanks than the one before it (1-2-1-2 or 1-1-2-1-1-2 are all breaks; 2-2-2-2 and 1-1-1-1 are none).
// One writer (the display callback's thread), any reader (the game thread's cadence); relaxed atomics, no lock.
class HoldMeter {
public:
    struct Snap { uint32_t pictures = 0, breaks = 0, hold[5] = {}; };
    // `vblank`: the display counter value the picture was put on. A gap over 8 vblanks (a stall, a pause) is no hold and no break.
    void Shown(uint32_t vblank) {
        if (have_) {
            const int32_t d = static_cast<int32_t>(vblank - last_);
            if (d >= 0 && d <= 8) {
                const uint32_t h = d < 1 ? 1u : static_cast<uint32_t>(d);
                pictures_.fetch_add(1, std::memory_order_relaxed);
                hold_[h < 4 ? h : 4].fetch_add(1, std::memory_order_relaxed);
                if (haveHold_ && h != prevHold_) breaks_.fetch_add(1, std::memory_order_relaxed);
                prevHold_ = h;
                haveHold_ = true;
            } else {
                haveHold_ = false;
            }
        }
        last_ = vblank;
        have_ = true;
    }
    Snap Read() const {
        Snap s;
        s.pictures = pictures_.load(std::memory_order_relaxed);
        s.breaks = breaks_.load(std::memory_order_relaxed);
        for (int i = 0; i < 5; ++i) s.hold[i] = hold_[i].load(std::memory_order_relaxed);
        return s;
    }
    void Restart() { have_ = haveHold_ = false; }  // (writer side: a load or settings hold; the next picture starts a new pair)
private:
    std::atomic<uint32_t> pictures_{0}, breaks_{0}, hold_[5] = {};
    uint32_t last_ = 0, prevHold_ = 0;
    bool have_ = false, haveHold_ = false;
};
inline HoldMeter& GlobalHoldMeter() {
    static HoldMeter m;
    return m;
}
// The "smooth:" line's window (every GXV_SMOOTH_LOG_S seconds, default 10): pictures shown, how many were held longer than the rhythm in force
// expected (more than 1 vblank on the up-to-60 rhythm, more than 2 on the steady 30 and the fixed 30 modes), the longest hold in ms, the holds
// 16 / 33 / 50 / 67+ ms, the breaks, and the pacer's state changes. Display callback thread only.
class SmoothCensus {
public:
    struct Window {
        double seconds = 0, maxMs = 0;
        uint32_t pictures = 0, late = 0, breaks = 0, hold[5] = {}, steadyPictures = 0, changes = 0;
    };
    // hold: vblanks since the previous picture (0: unknown, not counted); expect: the rhythm's own hold (1 or 2); steady: that rhythm is the cadence.
    void Picture(uint32_t hold, unsigned expect, bool steady) {
        if (hold == 0 || hold > 8) { havePrev_ = false; return; }  // (a gap: a stall or a load is the stutter census's business)
        ++w_.pictures;
        if (steady) ++w_.steadyPictures;
        ++w_.hold[hold < 4 ? hold : 4];
        if (hold > expect) ++w_.late;
        const double ms = hold * 16.6833;
        if (ms > w_.maxMs) w_.maxMs = ms;
        if (havePrev_ && hold != prev_) ++w_.breaks;
        prev_ = hold;
        havePrev_ = true;
    }
    bool Due(uint64_t nowUs, uint64_t periodUs) {
        if (!start_) start_ = nowUs;
        return nowUs - start_ >= periodUs;
    }
    // The window so far, and a new one; changesTotal: the pacer's own state changes since start (the difference is the window's).
    Window Take(uint64_t nowUs, uint32_t changesTotal) {
        Window r = w_;
        r.seconds = start_ && nowUs > start_ ? static_cast<double>(nowUs - start_) / 1e6 : 0.0;
        r.changes = changesTotal - lastChanges_;
        lastChanges_ = changesTotal;
        w_ = Window();
        start_ = nowUs;
        return r;
    }
    static int Format(char* out, size_t n, const Window& w, bool steadyNow, int rung) {
        const double fps = w.seconds > 0 ? w.pictures / w.seconds : 0.0;
        return snprintf(out, n,
            "smooth: %.1f s, %u pictures (%.1f a second), late %u, longest hold %.0f ms, holds 16/33/50/67+ ms %u/%u/%u/%u, breaks %u (%.1f a second), "
            "pacer %s (%.0f %% of pictures on it), state changes %u, rung %d",
            w.seconds, w.pictures, fps, w.late, w.maxMs, w.hold[1], w.hold[2], w.hold[3], w.hold[4], w.breaks, w.seconds > 0 ? w.breaks / w.seconds : 0.0,
            steadyNow ? "steady 30" : "up to 60", w.pictures ? 100.0 * w.steadyPictures / w.pictures : 0.0, w.changes, rung);
    }
private:
    Window w_;
    uint64_t start_ = 0;
    uint32_t prev_ = 0, lastChanges_ = 0;
    bool havePrev_ = false;
};
// ---- smooth25: the release gate's present-cadence metric (the "pace:" line) ------------------------------------------------------------------------------
// One census for every mode (the 1-vblank target of Performance mode's 60 and the 2-vblank target of the locked / steady 30), per window (5 s) and for the run:
//   pictures      shown in the window (a hold is the vblanks since the picture before; a gap over 8 vblanks or an unknown one is not counted: a load, a pause)
//   holds         0/1/2/3/4+ vblanks (as before) and the longest hold (vblanks and ms)
//   late          holds longer than the target (at least one vblank late) and late2: longer than target + 1 (at least two): the numbers the release gate reads
//   in play       the same counts for pictures whose own turn AND the one before were in play (no loading, no closed screen, no menu): the gate is
//                 "in play: late 0" for a window of steady play; windows with a load in them show their late pictures in the first set only
//   p99           the hold (vblanks) that 99 % of the in-play pictures of the run were at or under (a running histogram, 1..8+): 2 at a 30 target when at most
//                 one picture in a hundred was late, 3 when more were
// Pure; the display callback feeds it (vgx_host_galaxy_vita.cpp SmoothShown) and the host test (tests/pace_metric_model.cpp) runs traces shaped like the logs'.
class PaceMetric {
public:
    struct Window {
        uint32_t pictures = 0, hold[5] = {}, longest = 0, late = 0, late2 = 0;
        uint32_t playPictures = 0, playHold[5] = {}, playLongest = 0, playLate = 0, playLate2 = 0;
        unsigned target = 0;
    };
    // hold: vblanks since the previous picture (0: unknown); target: the rhythm's own hold (1 or 2); inPlay: this picture and the previous one were in play.
    void Picture(uint32_t hold, unsigned target, bool inPlay) {
        if (hold == 0 || hold > 8) return;
        w_.target = target;
        ++w_.pictures;
        ++w_.hold[hold < 4 ? hold : 4];
        if (hold > w_.longest) w_.longest = hold;
        if (hold > target) ++w_.late;
        if (hold > target + 1) ++w_.late2;
        if (!inPlay) return;
        ++w_.playPictures;
        ++w_.playHold[hold < 4 ? hold : 4];
        if (hold > w_.playLongest) w_.playLongest = hold;
        if (hold > target) ++w_.playLate;
        if (hold > target + 1) ++w_.playLate2;
        ++runHist_[hold];  // (hold 1..8)
        ++runPlay_;
        if (hold > runLongest_) runLongest_ = hold;
    }
    Window Take() {
        Window r = w_;
        w_ = Window();
        return r;
    }
    // The vblanks 99 % of the in-play pictures so far were held at or under (0 before any).
    unsigned RunP99() const {
        if (!runPlay_) return 0;
        const uint64_t need = (runPlay_ * 99 + 99) / 100;  // ceil(0.99 n)
        uint64_t seen = 0;
        for (unsigned h = 1; h <= 8; ++h) {
            seen += runHist_[h];
            if (seen >= need) return h;
        }
        return 8;
    }
    uint64_t RunPlayPictures() const { return runPlay_; }
    unsigned RunLongest() const { return runLongest_; }
    // " longest 2 vblanks (33 ms), late 0 (>target) / 0 (>target+1); in play: 150 pictures, holds 0/0/150/0/0, longest 2, late 0 / 0; run p99 2 vblanks (in play, 4200 pictures, longest 4)"
    int Format(char* out, size_t n, const Window& w) const {
        return snprintf(out, n,
            "longest %u vblanks (%.0f ms), late %u (over target %u) / %u (over target+1); in play: %u pictures, holds 0/1/2/3/4+ %u/%u/%u/%u/%u, longest %u, late %u / %u; "
            "run p99 %u vblanks (in play, %llu pictures, longest %u)",
            w.longest, w.longest * 16.6833, w.late, w.target, w.late2, w.playPictures, w.playHold[0], w.playHold[1], w.playHold[2], w.playHold[3], w.playHold[4],
            w.playLongest, w.playLate, w.playLate2, RunP99(), static_cast<unsigned long long>(runPlay_), runLongest_);
    }

private:
    Window w_;
    uint64_t runHist_[9] = {}, runPlay_ = 0;
    unsigned runLongest_ = 0;
};
// ---- GXV_STEADY30: Performance mode's steady 30 when 60 is out of reach -------------------------------------------------------
// Performance mode (GXV_AUTOQ_TARGET=60) runs the "Up to 60" skip rule: every turn the pipeline has room for is drawn, and
// GXV_PRESENT_SYNC=2 shows each picture on the vblank of its turn, so Mario's displayed speed is right whatever the rate. Between
// 30 and about 40 pictures a second that still means a mix of pictures held for 1, 2 and 3 vblanks (16.7, 33 and 50 ms) on the
// Vita's fixed 60 Hz screen: the uneven rhythm reads as stutter, and a steady 30 (every picture 2 vblanks) looks smoother for the
// few pictures it gives up. 40 a second is no steady option on a 60 Hz screen (it is 1-2-1-2 vblanks; GXV_SKIP_PATTERN=3 is the
// regular 45, 1-1-2). The GPU's share is the controller's to answer (a smaller picture, cheaper steps); the CPU's (the game turn,
// the front end, the renderer) is not: no picture size makes them faster. So when the pictures stay under belowFps (38) for
// engageSec (3 s) and the GPU is not what holds them (or nothing is left to make its work smaller), every second turn is offered
// a picture (the GXV_SKIP_PATTERN=2 rhythm, as a state): a turn not offered keeps its retrace (code 1). It is left when the
// estimate of the pictures without it, min(60, game, pipeline), is at least aboveFps (45) for releaseSec (4 s):
//   game      the skip rule's picture rate at full speed from the turns' own work (start to retrace wait; Slip above): turns that
//             draw cost d, the others n, so 60 (R - n) / (d - n) pictures a second keep the game's 60 turns (60 when d <= R);
//   pipeline  1 s / the slowest stage that works a picture each on its own thread or unit (front end CPU, renderer CPU, GPU), from
//             the automatic quality controller's last window (vgx_autoq.cpp port_autoq_stages).
// A release whose pictures fall under belowFps again within confirmSec (the estimate missed something) engages at once, and the next
// release is blocked for blockSec, doubling to maxBlockSec (forgotten after forgetSec without a block): no flapping between the two
// rhythms. A load or scene change (allowed false, or a gap) clears the measurements but keeps the state (a drop-in into the same
// heavy area does not flip it); quiet moments (the pause menu, a closed fade) are not measured and change nothing.
// (the release threshold, also read by the automatic quality controller: vgx_autoq.h Config::cadenceReachFps, where a scene that cannot
// estimate this many pictures a second at any picture size may climb back on the steady 30, GXV_AUTOQ_CADENCE_UP)
inline constexpr double kSteady30BelowFps = 38.0, kSteady30AboveFps = 45.0;
// GXV_PACE23: between 38 and 50 pictures a second is the uneven rhythm (holds of 1, 2 and 3 vblanks) the cadence exists to remove, so it
// engages below 50 and releases only when 57 or more are estimated (a scene that can really hold ~60); a shorter engage (2.5 s) and release (3 s).
inline constexpr double kPace23Steady30BelowFps = 50.0, kPace23Steady30AboveFps = 57.0;
struct SteadyCadenceParams {
    double belowFps = kSteady30BelowFps, engageSec = 3.0;   // engage: pictures a second under belowFps for engageSec
    double aboveFps = kSteady30AboveFps, releaseSec = 4.0;  // release: the estimate without the cadence at aboveFps or more for releaseSec
    double gpuShare = 0.85;                    // a GPU busy this share of the picture interval, with a smaller picture still
                                               // possible, is the GPU's limit: no cadence
    double confirmSec = 8.0;                   // a release must hold this long ...
    double blockSec = 30.0, maxBlockSec = 240.0, forgetSec = 300.0;  // ... or the next release is blocked
    // GXV_SEAMLESS (seamless23; off by default, and then every decision is the one made without these): no burst of uneven pictures at a switch.
    //   cutRelease  a release that is due waits for a camera cut (NoteCut) or a quiet moment, where the change of rhythm is not seen, at most
    //               cutWaitSec (the estimate must stay good meanwhile; a bad window cancels it);
    //   quickTurns  within confirmSec after a release, a short window of this many turns whose pictures fall under belowFps engages again at
    //               once (not after a whole second of 1-2-3 vblank holds), and blocks the next release as the confirm rule does. 0: off.
    bool cutRelease = false;
    double cutWaitSec = 15.0;
    unsigned quickTurns = 0;
    // GXV_PACE24 (pace24; off by default, and then every decision is the one made without these):
    //   p24        the calmer, quicker rule below; a release now clears the window it was made in (without it, a release at a camera cut or a quiet
    //              moment was undone by the quick-engage check 7-13 ms later, from the stale window's 30 pictures a second: c27 760/768, 8 of 8 cut
    //              releases, every one doubling the release block to 240 s);
    //   window     turns a decision window has (60 = one second, today's; 30 with p24);
    //   breakFps   the display's breaks a second (HoldMeter) that make a rhythm irregular: engaging needs that many (a regular 45 or 54 is none of its business) when
    //              the meter has pictures; 0: the rate alone decides;
    //   dwellSec   after an engage no release for this long (no flapping on a marginal scene).
    bool p24 = false;
    unsigned window = 60;
    double breakFps = 0.0;
    double dwellSec = 0.0;
};
class SteadyCadence {
public:
    static constexpr int64_t kPeriodUs = 16683;
    static constexpr unsigned kWindow = 60;  // turns a window (about a second)
    using Params = SteadyCadenceParams;
    void Configure(bool on) { Configure(on, Params()); }
    void Configure(bool on, const Params& p) {
        *this = SteadyCadence();
        on_ = on;
        p_ = p;
    }
    bool Enabled() const { return on_; }
    bool Engaged() const { return engaged_; }
    unsigned Changes() const { return changes_; }
    // Diagnostics of the last full window (and the reason for the last change): pictures a second, the estimate without the cadence and
    // its two parts (0: not known), the GPU's busy time a picture (us, 0: not known), whether a smaller picture was still possible.
    double RateFps() const { return rate_; }
    double EstimateFps() const { return est_; }
    double GameFps() const { return gameFps_; }
    double PipeFps() const { return pipeFps_; }
    uint32_t GpuUs() const { return gpuUs_; }
    bool Lowerable() const { return lowerable_; }
    double BlockSec() const { return blockLen_; }
    // GXV_PACE24: the display's holds (HoldMeter, may be null) and the breaks a second the last window saw (-1: no meter / too few pictures).
    void UseMeter(const HoldMeter* m) { meter_ = m; if (m) snap_ = quickSnap_ = m->Read(); }
    double BreaksPerSec() const { return breaksPerSec_; }
    // GXV_SEAMLESS: the game's camera cut (a jump of the stage camera) at this time; how the last switches went.
    void NoteCut(uint64_t nowUs) { cutAt_ = nowUs; }
    unsigned ReleasesAtCut() const { return releasesAtCut_; }
    unsigned ReleasesTimedOut() const { return releasesTimedOut_; }
    unsigned QuickEngages() const { return quickEngages_; }
    bool LastAtCut() const { return lastAtCut_; }
    // GXV_SMOOTH60 (policy26): the per-stage controller owns the rhythm. mode 1: the even 30 is on and nothing releases it; 0: it is off and nothing engages it
    // (the pictures are offered every turn the skip rule has room for); -1 (the default): the rules above decide, as before. The window's measurements go on
    // (RateFps and the rest stay current) but no longer change the state.
    void Force(int mode, uint64_t nowUs) {
        forced_ = mode < 0 ? -1 : (mode > 0 ? 1 : 0);
        if (!on_ || forced_ < 0) return;
        const bool want = forced_ == 1;
        if (want == engaged_) return;
        engaged_ = want;
        if (want) NoteEngaged(nowUs);
        ++changes_;
        badRun_ = goodRun_ = 0;
        confirmUntil_ = 0;
        releaseReady_ = false;
        readySince_ = 0;
        lastDrew_ = false;
        ClearWindow();
    }
    int Forced() const { return forced_; }
    // The stress mode (GXV_SEAMLESS_STRESS): the other rhythm now, as a release or an engage would make it.
    void ForceToggle(uint64_t nowUs) {
        if (!on_) return;
        if (engaged_) Release(nowUs, false);
        else { engaged_ = true; NoteEngaged(nowUs); ++changes_; badRun_ = goodRun_ = 0; confirmUntil_ = 0; releaseReady_ = false; readySince_ = 0; }
        lastAtCut_ = false;
        ClearWindow();
    }

    // The controller's stage hint (port_autoq_stages), any time before Begin; known false: none (no target 60, no GPU timers).
    void Stages(bool known, uint32_t gpuUs, uint32_t pipeUs, bool lowerable) {
        stagesKnown_ = known && gpuUs && pipeUs;
        gpuUs_ = stagesKnown_ ? gpuUs : 0;
        pipeUs_ = stagesKnown_ ? pipeUs : 0;
        lowerable_ = stagesKnown_ && lowerable;
    }
    // The start of a turn. allowed false: a load or scene change; quiet: a pause menu or a closed fade. Returns whether the turn is
    // offered a picture (always, unless engaged).
    bool Begin(uint64_t nowUs, bool allowed, bool quiet) {
        if (!on_) return true;
        const bool gap = began_ && (nowUs < last_ || nowUs - last_ > 250000);
        last_ = start_ = nowUs;
        began_ = true;
        open_ = false;
        if (!allowed || gap) {
            ClearWindow();
            badRun_ = goodRun_ = 0;
            lastDrew_ = false;
            return true;
        }
        open_ = !quiet;
        // (cutRelease: a release that was due is made at this turn, a camera cut's or a quiet moment's, while the estimate still holds)
        if (engaged_ && releaseReady_ && (quiet || (cutAt_ != 0 && nowUs >= cutAt_ && nowUs - cutAt_ <= 100000))) {
            Release(nowUs, true);
            ++releasesAtCut_;
        }
        // (the rhythm follows the last picture, not a fixed phase: a turn is offered when the one before it did not draw, so no two
        // pictures come on consecutive turns; an offered turn the skip rule leaves out (late) passes the offer on: a 3-vblank hold)
        return !engaged_ || !lastDrew_;
    }
    // What the skip rule made of the turn: offered (or not, under the cadence), and drawn (a picture is made).
    void Decision(bool offered, bool drawn) {
        if (!on_) return;
        (void)offered;
        drew_ = lastDrew_ = drawn;
    }
    // The end of the turn's work (before its retrace wait).
    void End(uint64_t nowUs) {
        if (!on_ || !open_) return;
        open_ = false;
        int64_t w = nowUs >= start_ ? static_cast<int64_t>(nowUs - start_) : 0;
        if (w > 4 * kPeriodUs) w = 4 * kPeriodUs;
        if (drew_) { sumD_ += w; ++nD_; } else { sumN_ += w; ++nN_; }
        ++turns_;
        // (quickTurns: soon after a release, a short window of uneven pictures engages again at once)
        if (p_.quickTurns && !engaged_ && confirmUntil_ && nowUs < confirmUntil_ && turns_ >= quickT0_ + p_.quickTurns) {
            // (each run of quickTurns turns of the window on its own: uneven pictures in the middle of a window count as early as at its start)
            const double rate = 60.0 * (nD_ - quickD0_) / (turns_ - quickT0_);
            quickT0_ = turns_;
            quickD0_ = nD_;
            const bool gpuLimit = stagesKnown_ && lowerable_ && rate > 0.0 && gpuUs_ >= p_.gpuShare * 1e6 / rate;
            bool irregular = true;
            if (p_.p24 && meter_ && p_.breakFps > 0.0) {  // (GXV_PACE24: uneven pictures on the screen, not only a low rate)
                const HoldMeter::Snap s = meter_->Read();
                const uint32_t mp = s.pictures - quickSnap_.pictures, mb = s.breaks - quickSnap_.breaks;
                quickSnap_ = s;
                if (mp >= 6) irregular = static_cast<double>(mb) * 60.0 / static_cast<double>(p_.quickTurns) >= p_.breakFps;
            }
            if (rate < p_.belowFps && !gpuLimit && irregular) {
                engaged_ = true;
                NoteEngaged(nowUs);
                ++changes_;
                ++quickEngages_;
                rate_ = rate;
                badRun_ = goodRun_ = 0;
                blockLen_ = blockLen_ > 0 ? std::min(blockLen_ * 2.0, p_.maxBlockSec) : p_.blockSec;
                releaseBlockedUntil_ = nowUs + static_cast<uint64_t>(blockLen_ * 1e6);
                lastBlockAt_ = nowUs;
                confirmUntil_ = 0;
                ClearWindow();
                return;
            }
        }
        if (turns_ >= (p_.p24 ? std::min<unsigned>(std::max<unsigned>(p_.window, 20u), kWindow) : kWindow)) Evaluate(nowUs);
    }

private:
    void ClearWindow() { sumD_ = sumN_ = 0; nD_ = nN_ = turns_ = 0; quickT0_ = quickD0_ = 0; if (meter_) snap_ = quickSnap_ = meter_->Read(); }
    void Release(uint64_t nowUs, bool atCut) {
        engaged_ = false;
        ++changes_;
        badRun_ = goodRun_ = 0;
        confirmUntil_ = nowUs + static_cast<uint64_t>(p_.confirmSec * 1e6);
        releaseReady_ = false;
        readySince_ = 0;
        lastAtCut_ = atCut;
        if (p_.p24) ClearWindow();  // (the window that decided the release is not the first one of the new rhythm)
    }
    void NoteEngaged(uint64_t nowUs) { engagedAt_ = nowUs; }
    void Evaluate(uint64_t nowUs) {
        const double r = static_cast<double>(kPeriodUs);
        const double secs = static_cast<double>(turns_) / 60.0;  // (game time: quiet turns are not in the window)
        // (pictures a second by the turns: the game's 60 a second, the waits included)
        rate_ = 60.0 * nD_ / turns_;
        gameFps_ = 0.0;
        if (nD_ >= 4) {
            const double d = static_cast<double>(sumD_) / nD_, n = nN_ >= 4 ? static_cast<double>(sumN_) / nN_ : 0.0;
            if (nN_ < 4 || d <= r) gameFps_ = d <= r ? 60.0 : 60.0 * r / d;  // (every turn draws: as many as fit)
            else if (n >= r) gameFps_ = 0.0;                                 // (the game alone misses its turns)
            else gameFps_ = d > n ? std::min(60.0, 60.0 * (r - n) / (d - n)) : 60.0;
        }
        breaksPerSec_ = -1.0;
        if (p_.p24 && meter_) {  // (GXV_PACE24: how uneven the screen's holds were over this window)
            const HoldMeter::Snap s = meter_->Read();
            const uint32_t mp = s.pictures - snap_.pictures, mb = s.breaks - snap_.breaks;
            snap_ = s;
            if (mp >= 8 && secs > 0.0) breaksPerSec_ = static_cast<double>(mb) / secs;
        }
        pipeFps_ = stagesKnown_ ? std::min(60.0, 1e6 / static_cast<double>(pipeUs_)) : 0.0;
        est_ = gameFps_ > 0.0 ? (pipeFps_ > 0.0 ? std::min(gameFps_, pipeFps_) : gameFps_) : 0.0;
        if (lastBlockAt_ && nowUs > lastBlockAt_ + static_cast<uint64_t>(p_.forgetSec * 1e6)) { blockLen_ = 0; lastBlockAt_ = 0; }
        // The GPU holds the pictures (busy gpuShare of their interval or more) and the controller can still make its work smaller: its turn.
        const bool gpuLimit = stagesKnown_ && lowerable_ && rate_ > 0.0 && gpuUs_ >= p_.gpuShare * 1e6 / rate_;
        if (forced_ >= 0) {
            // (GXV_SMOOTH60: the per-stage controller decides; this window only keeps the diagnostics current)
        } else if (!engaged_) {
            // (GXV_PACE24: a low rate on a regular rhythm is not the uneven mix the cadence exists to remove; no meter reading: the rate decides)
            const bool irregular = !(p_.p24 && p_.breakFps > 0.0 && breaksPerSec_ >= 0.0) || breaksPerSec_ >= p_.breakFps;
            // (GXV_PACE24: the pictures the pipeline really delivered with every turn offered ARE the estimate: the stage means of the hint miss the coupling
            // between the stages (the model's 48 a second reads 57+ from them, the console's "without it 60.0" before a release that was undone); the
            // persistence (engageSec) is what keeps a passing dip from engaging)
            const bool bad = rate_ < p_.belowFps && (p_.p24 || (est_ > 0.0 && est_ < p_.aboveFps)) && !gpuLimit && irregular;
            badRun_ = bad ? badRun_ + secs : 0.0;
            // (soon after a release only one such window is needed: the estimate that released it was wrong)
            const bool confirming = confirmUntil_ && nowUs < confirmUntil_;
            if (bad && (badRun_ >= p_.engageSec - 1e-9 || confirming)) {
                engaged_ = true;
                NoteEngaged(nowUs);
                ++changes_;
                badRun_ = goodRun_ = 0;
                if (confirming) {
                    blockLen_ = blockLen_ > 0 ? std::min(blockLen_ * 2.0, p_.maxBlockSec) : p_.blockSec;
                    releaseBlockedUntil_ = nowUs + static_cast<uint64_t>(blockLen_ * 1e6);
                    lastBlockAt_ = nowUs;
                }
                confirmUntil_ = 0;
            }
            if (confirmUntil_ && nowUs >= confirmUntil_) confirmUntil_ = 0;
        } else {
            const bool good = est_ >= p_.aboveFps && nowUs >= releaseBlockedUntil_ &&
                              (!p_.p24 || nowUs >= engagedAt_ + static_cast<uint64_t>(p_.dwellSec * 1e6));  // (GXV_PACE24: a minimum dwell)
            goodRun_ = good ? goodRun_ + secs : 0.0;
            if (!good) { releaseReady_ = false; readySince_ = 0; }
            if (goodRun_ >= p_.releaseSec - 1e-9) {
                if (!p_.cutRelease) {
                    Release(nowUs, false);
                } else {
                    // (cutRelease: due; made at the next cut or quiet moment (Begin), or now when none came for cutWaitSec)
                    if (!releaseReady_) { releaseReady_ = true; readySince_ = nowUs; }
                    if (static_cast<double>(nowUs - readySince_) >= p_.cutWaitSec * 1e6) {
                        Release(nowUs, false);
                        ++releasesTimedOut_;
                    }
                }
            }
        }
        ClearWindow();
    }
    bool on_ = false, engaged_ = false, began_ = false, open_ = false, drew_ = false, lastDrew_ = false;
    int forced_ = -1;  // (Force)
    bool releaseReady_ = false, lastAtCut_ = false;
    uint64_t cutAt_ = 0, readySince_ = 0;
    unsigned releasesAtCut_ = 0, releasesTimedOut_ = 0, quickEngages_ = 0, quickT0_ = 0, quickD0_ = 0;
    bool stagesKnown_ = false, lowerable_ = false;
    Params p_;
    unsigned changes_ = 0;
    uint64_t last_ = 0, start_ = 0, confirmUntil_ = 0, releaseBlockedUntil_ = 0, lastBlockAt_ = 0;
    uint32_t gpuUs_ = 0, pipeUs_ = 0;
    int64_t sumD_ = 0, sumN_ = 0;
    unsigned nD_ = 0, nN_ = 0, turns_ = 0;
    double badRun_ = 0, goodRun_ = 0, blockLen_ = 0;
    double rate_ = 0, est_ = 0, gameFps_ = 0, pipeFps_ = 0, breaksPerSec_ = -1.0;
    const HoldMeter* meter_ = nullptr;
    HoldMeter::Snap snap_, quickSnap_;
    uint64_t engagedAt_ = 0;
};
}
