#pragma once
#include <cstdint>

namespace port::pace {
enum class Policy { Adaptive, Fixed30 };
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
class LegacySkip {
public:
    static constexpr int64_t kPeriodNs = 16683350;
    struct Verdict { int code; bool behind, gpuBusy; };
    LegacySkip(int maxRun, int pipeDepth, int maxPipeRun, const Slip* slip = nullptr)
        : maxRun_(maxRun), pipeDepth_(pipeDepth), maxPipeRun_(maxPipeRun), slip_(slip) {}
    Verdict Turn(int64_t nowNs, bool allowed, unsigned pending) {
        const int64_t dt = last_ ? nowNs - last_ : kPeriodNs;
        last_ = nowNs;
        if (dt > 15 * kPeriodNs || !allowed) {  // a load, a pause, a scene change: nothing to catch up with
            debt_ = 0;
            run_ = 0;
            return {0, false, false};
        }
        debt_ += dt - kPeriodNs;
        if (debt_ < 0) debt_ = 0;
        if (debt_ > 4 * kPeriodNs) debt_ = 4 * kPeriodNs;
        const bool engaged = slip_ && slip_->Engaged();
        if (engaged) {
            if (!wasEngaged_) cadence_ = 0;
            debt_ = 0;
        }
        wasEngaged_ = engaged;
        const bool behind = debt_ >= kPeriodNs / 2 && run_ < maxRun_;
        const bool gpuBusy = pipeDepth_ > 0 && pending >= static_cast<unsigned>(pipeDepth_) && run_ < maxPipeRun_;
        if (behind || gpuBusy) {
            ++run_;
            return {behind ? 2 : 1, behind, gpuBusy};
        }
        run_ = 0;
        if (engaged && (cadence_++ & 1)) return {1, false, false};
        return {0, false, false};
    }

private:
    int maxRun_, pipeDepth_, maxPipeRun_;
    const Slip* slip_;
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
        *this = Controller(verify_, policy_);
        slip_ = slip;
    }
    // The slip (above): while it is engaged the fixed 30 policy keeps every second turn's picture and does not skip a late one.
    void UseSlip(const Slip* slip) { slip_ = slip; }
    bool PresentAllowed() const { return presentAllowed_; }

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
            debt_ += delta;
            if (debt_ < 0) debt_ = 0;
            if (debt_ > 66732) debt_ = 66732;
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
        const bool gap = started_ && (now < last_ || now - last_ > 250000);
        if (!started_ || gap) {
            phase_ = 0;
            forcePresent_ = true;
        }
        if (!started_ || gap || !allowed) {
            debt_ = 0;
            catchup_ = 0;
        } else {
            debt_ += static_cast<int64_t>(now - last_) - 16683;
            if (debt_ < 0) debt_ = 0;
            if (debt_ > 66732) debt_ = 66732;
        }
        started_ = true;
        last_ = now;
        const bool absorb = slip_ && slip_->Engaged();
        if (absorb) debt_ = 0;  // (the slipped time is not caught up: Slip)
        const bool late = debt_ >= 8341 && catchup_ < 3;
        catchup_ = late ? catchup_ + 1 : 0;
        const bool due = phase_ == 0;
        phase_ = (phase_ + 1) % 2;
        const int decision = !allowed ? 0 : late ? 2 : (!due || pending >= 2) ? 1 : 0;
        if (decision == 0) {
            // Count emitted pictures, not nominal slots: backlog may skip every due turn.
            presentAllowed_ = forcePresent_ || allowed || due || previousHidden_;
            previousHidden_ = !presentAllowed_;
            forcePresent_ = false;
        }
        return decision;
    }
    uint64_t last_ = 0, changedAt_ = 0, windowAt_ = 0, overUs_ = 0, roomUs_ = 0, mismatches_ = 0;
    int64_t debt_ = 0;
    unsigned divisor_ = 2, want_ = 2, phase_ = 0, oracleRemaining_ = 0;
    unsigned catchup_ = 0, samples_ = 0, bad_ = 0, changes_ = 0;
    bool started_ = false, verify_ = false, roomKnown_ = true;
    Policy policy_ = Policy::Adaptive;
    const Slip* slip_ = nullptr;
    bool presentAllowed_ = true, previousHidden_ = false, forcePresent_ = true;
};

class PresentationSlots {
public:
    void Reset() { started_ = false; }
    // earliest is the next scanout vblank (GetVcount + 1), after GPU completion.
    uint32_t Next(uint32_t earliest, unsigned divisor) {
        if (!started_ || divisor != divisor_) {
            started_ = true;
            divisor_ = divisor;
            target_ = earliest;
            return target_;
        }
        target_ += divisor_;
        if (static_cast<int32_t>(earliest - target_) > 0) target_ = earliest;
        return target_;
    }
    void Shown(uint32_t actual) { target_ = actual; }

private:
    uint32_t target_ = 0;
    unsigned divisor_ = 2;
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
}
