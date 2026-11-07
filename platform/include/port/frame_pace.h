#pragma once
#include <algorithm>
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
    struct Verdict { int code; bool behind, gpuBusy; bool capped = false; };
    LegacySkip(int maxRun, int pipeDepth, int maxPipeRun, const Slip* slip = nullptr, int maxStep = 0)
        : maxRun_(maxRun), pipeDepth_(pipeDepth), maxPipeRun_(maxPipeRun), slip_(slip),
          capRun_(maxStep >= 2 ? (maxStep > 8 ? 7 : maxStep - 1) : 1 << 30) {}
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
        bool behind = debt_ >= kPeriodNs / 2 && run_ < maxRun_;
        bool gpuBusy = pipeDepth_ > 0 && pending >= static_cast<unsigned>(pipeDepth_) && run_ < maxPipeRun_;
        bool capped = false;
        if ((behind || gpuBusy) && run_ >= capRun_) {  // (GXV_SKIP_MAXSTEP: this turn draws; the debt is kept for the turns after it)
            capped = true;
            behind = gpuBusy = false;
        }
        if (behind || gpuBusy) {
            ++run_;
            return {behind ? 2 : 1, behind, gpuBusy};
        }
        run_ = 0;
        if (capped) return {0, false, false, true};  // (not left out by the slip's cadence either)
        if (engaged && (cadence_++ & 1)) return {1, false, false};
        return {0, false, false};
    }

private:
    int maxRun_, pipeDepth_, maxPipeRun_;
    const Slip* slip_;
    int capRun_;  // skipped turns allowed in a row (GXV_SKIP_MAXSTEP - 1; very large when off)
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
    void Reset() { started_ = false; }

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
    }
    unsigned Latency() const { return lat_; }
    bool Aligned() const { return align_; }
    // earliest: the first vblank the picture can be on (counter + 1). valid / stamp: the picture's turn, if it has one.
    // Returns the vblank to show it on (earliest or later; never more than max_ + 1 later).
    uint32_t Target(uint32_t earliest, bool valid, uint16_t stamp) {
        ++stats_.pictures;
        cur_ = false;
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
                    if (haveShown_ && static_cast<int32_t>(t - (lastShown_ + 1)) < 0) t = lastShown_ + 1;  // one picture per vblank
                    if (static_cast<int32_t>(t - earliest) > static_cast<int32_t>(max_ + 1)) t = earliest + max_ + 1;
                    if (static_cast<int32_t>(t - earliest) > 0) target = t;
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
        lastShown_ = shown;
        haveShown_ = true;
        if (!cur_) return;
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
    }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }

private:
    void Adapt() {
        const bool overload = curNatural_ > max_;           // no L in range would have it: show it, learn nothing
        const bool miss = curNatural_ > lat_;
        if (miss) ++stats_.misses;
        if (overload) {
            ++stats_.overload;
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
    Stats stats_;
    bool align_ = false, cur_ = false, havePrev_ = false, haveShown_ = false;
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
    // The decision that was made for an offered turn: drawn, or left out by the frame-skip rule (then the next turn is offered).
    void Result(bool offered, bool drawn) {
        if (drawn) owed_ = false;
        else if (offered) owed_ = true;
    }

private:
    unsigned n_, phase_ = 0;
    bool owed_ = false;
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
struct SteadyCadenceParams {
    double belowFps = kSteady30BelowFps, engageSec = 3.0;   // engage: pictures a second under belowFps for engageSec
    double aboveFps = kSteady30AboveFps, releaseSec = 4.0;  // release: the estimate without the cadence at aboveFps or more for releaseSec
    double gpuShare = 0.85;                    // a GPU busy this share of the picture interval, with a smaller picture still
                                               // possible, is the GPU's limit: no cadence
    double confirmSec = 8.0;                   // a release must hold this long ...
    double blockSec = 30.0, maxBlockSec = 240.0, forgetSec = 300.0;  // ... or the next release is blocked
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
        if (++turns_ >= kWindow) Evaluate(nowUs);
    }

private:
    void ClearWindow() { sumD_ = sumN_ = 0; nD_ = nN_ = turns_ = 0; }
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
        pipeFps_ = stagesKnown_ ? std::min(60.0, 1e6 / static_cast<double>(pipeUs_)) : 0.0;
        est_ = gameFps_ > 0.0 ? (pipeFps_ > 0.0 ? std::min(gameFps_, pipeFps_) : gameFps_) : 0.0;
        if (lastBlockAt_ && nowUs > lastBlockAt_ + static_cast<uint64_t>(p_.forgetSec * 1e6)) { blockLen_ = 0; lastBlockAt_ = 0; }
        // The GPU holds the pictures (busy gpuShare of their interval or more) and the controller can still make its work smaller: its turn.
        const bool gpuLimit = stagesKnown_ && lowerable_ && rate_ > 0.0 && gpuUs_ >= p_.gpuShare * 1e6 / rate_;
        if (!engaged_) {
            const bool bad = rate_ < p_.belowFps && est_ > 0.0 && est_ < p_.aboveFps && !gpuLimit;
            badRun_ = bad ? badRun_ + secs : 0.0;
            // (soon after a release only one such window is needed: the estimate that released it was wrong)
            const bool confirming = confirmUntil_ && nowUs < confirmUntil_;
            if (bad && (badRun_ >= p_.engageSec - 1e-9 || confirming)) {
                engaged_ = true;
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
            const bool good = est_ >= p_.aboveFps && nowUs >= releaseBlockedUntil_;
            goodRun_ = good ? goodRun_ + secs : 0.0;
            if (goodRun_ >= p_.releaseSec - 1e-9) {
                engaged_ = false;
                ++changes_;
                badRun_ = goodRun_ = 0;
                confirmUntil_ = nowUs + static_cast<uint64_t>(p_.confirmSec * 1e6);
            }
        }
        ClearWindow();
    }
    bool on_ = false, engaged_ = false, began_ = false, open_ = false, drew_ = false, lastDrew_ = false;
    bool stagesKnown_ = false, lowerable_ = false;
    Params p_;
    unsigned changes_ = 0;
    uint64_t last_ = 0, start_ = 0, confirmUntil_ = 0, releaseBlockedUntil_ = 0, lastBlockAt_ = 0;
    uint32_t gpuUs_ = 0, pipeUs_ = 0;
    int64_t sumD_ = 0, sumN_ = 0;
    unsigned nD_ = 0, nN_ = 0, turns_ = 0;
    double badRun_ = 0, goodRun_ = 0, blockLen_ = 0;
    double rate_ = 0, est_ = 0, gameFps_ = 0, pipeFps_ = 0;
};
}
