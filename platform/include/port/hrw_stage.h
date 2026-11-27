#pragma once
// GXV_HRW_STAGE (pace33 PATCH6, 14 Nov 2026): the per-stage choice between the re-projected 60 and a locked even 30. Pure logic: tests/pace33_test.cpp runs this code.
//
// The owner picks per stage after his eye tests (Beach Bowl, 14 Nov: "some flashing, occasional screen tear, what looks like jitter" with the re-projected 60 while the camera moves). A stage
// row's key `reproj` (the switch GXV_HRW_STAGE; the environment's value is the default for stages whose row says nothing):
//   0  no word: as the mode (GXV_HRW) says
//   1  a locked even 30: no pair is opened in this stage (no snapshot, no second picture); the turn rhythm, present sync, the drain and the late turn stay as they are, so the
//      stage shows one picture every second refresh with the same low delay
//   2  the re-projected 60 (pairs), as GXV_HRW=5 does anyway: for a table that names every stage
//   3  as 1, and the row says so for the log: "a locked even 30 with the Quality picture". The pacing is 1's; the picture is whatever the SAME row sets beside it (the stage-settable
//      members of the Quality picture: pb=0, bloom_fold=0, ptcl_thin=0, cpulod=1 ...: tools/presets/stage-presets-1114-reproj-q.txt). Not per stage yet: edge smoothing at the
//      screen's own size (GXV_EDGE_AA_NATIVE, read once), the Quality umbrella's rewritten distances, the lock ladder.
// The choice in force changes ONLY while the screen is covered (a stage load, a closed wipe, a pause) or at a new scene: a value that changes in the open (a mid-stage preset step, an
// A/B flip) waits for the next cover. One log line per change, one per deferral.
#include <atomic>
#include <cstdint>

namespace port::hrwstage {

class Choice {
public:
    // want: the knob's value now. covered: the screen is covered, or this is the first drawn turn of a new scene. True: the choice in force changed at this call.
    bool Update(int want, bool covered) {
        if (want < 0 || want > 3) want = 0;
        deferredNow_ = false;
        if (want == cur_) { pending_ = -1; return false; }
        if (!covered) {
            if (pending_ != want) { pending_ = want; ++deferrals_; deferredNow_ = true; }
            return false;
        }
        cur_ = want;
        pending_ = -1;
        ++changes_;
        return true;
    }
    int Current() const { return cur_; }
    bool PairsOff() const { return cur_ == 1 || cur_ == 3; }
    bool DeferredNow() const { return deferredNow_; }   // this call found a new value in the open (true once per value)
    int Pending() const { return pending_; }
    uint32_t Changes() const { return changes_; }
    uint32_t Deferrals() const { return deferrals_; }

private:
    int cur_ = 0, pending_ = -1;
    bool deferredNow_ = false;
    uint32_t changes_ = 0, deferrals_ = 0;
};

// ---- GXV_HRW_STAGE_AUTO (pace33 PATCH9, default 1): the automatic fallback for a stage that cannot hold even pictures at the re-projected 60 --------------------------------
// Console 2892a (the Comet Observatory with the defaults): 48 shown, "even:" 53-62 of 100 for minutes; nobody had written a row for it. The rule, for any stage whose row says
// nothing (reproj 0; a row's 1, 2 or 3 is the owner's word and is left alone):
//   - the display thread publishes each "even:" window's score (10 s of play; PublishWindow);
//   - a window counts only when it began after the stage (or the choice) did, and only while the fresh player is already off (GXV_HRW_FRESH_AUTO gives that up first);
//   - GXV_HRW_STAGE_AUTO_WINDOWS (2) windows in a row under GXV_HRW_STAGE_AUTO_SCORE (75): the stage falls back to the locked even 30 (the choice 1 above), taken by Choice at
//     the next covered moment (the caller adds a camera cut, and a still moment after GXV_HRW_STAGE_AUTO_OPEN_S);
//   - the stage is remembered for the session. A stage load starts the re-projected 60 again; in a remembered stage ONE bad window is enough to fall back again.
// With retries disabled the fallback lasts for the stage. One log line per decision.
//
// hrw41 (16 Nov 2026), GXV_HRW_STAGE_AUTO_RETRY=<windows> (default 0 = never, every answer as before): the way back. The owner, 16 Nov: "when the camera pans quick,
// you do see double"; his own log of that night has the Comet Observatory at the locked even 30 from 23:31:38 until he left it 100 s later, after two bad windows
// right after the load: in a locked 30 every picture is held for two refreshes, so everything on the screen is seen double on a pan the eye follows. With a value,
// after that many "even:" windows (10 s each) in the fallback, each of them with a score of GXV_HRW_STAGE_AUTO_SCORE or more (the locked 30 itself holds) and with
// room for a pair on the game thread (the caller's word: its drawn + undrawn turn under GXV_HRW_STAGE_AUTO_RETRY_MS10), the re-projected 60 is tried again in the
// same stage. A stage that fell back needs ONE bad window to fall back again (as a remembered stage does), and each fallback of a visit doubles the wait (x2, x4,
// then x8 for good), so a stage that cannot hold it spends ever less of its time trying: with 3 that is 30 s, 60 s, 120 s, then 240 s at the locked 30 between tries.
struct AutoConfig {
    bool on = true;
    int score = 75;
    int windows = 2, windowsAgain = 1;
    int retryWindows = 0;   // (hrw41) 0: no way back inside a stage
};
inline std::atomic<uint32_t>& EvenWindow() { static std::atomic<uint32_t> v{0}; return v; }   // sequence << 8 | score (0: no window yet)
inline void PublishWindow(double score) {   // display thread, once per "even:" window
    std::atomic<uint32_t>& v = EvenWindow();
    uint32_t seq = ((v.load(std::memory_order_relaxed) >> 8) + 1) & 0xFFFFFFu;
    if (!seq) seq = 1;
    const int sc = score < 0 ? 0 : score > 100 ? 100 : static_cast<int>(score + 0.5);
    v.store((seq << 8) | static_cast<uint32_t>(sc), std::memory_order_relaxed);
}
class Auto {
public:
    struct Out { bool fell = false, newStage = false, remembered = false, retried = false; int score = 0, bad = 0, need = 0, calm = 0, wait = 0, tries = 0; };
    void Configure(const AutoConfig& c) { c_ = c; }
    // Once per drawn turn (game thread). key: the stage. eligible: mode 5 in Performance and the stage's row says nothing. window: EvenWindow()'s value. freshOff: no fresh player now.
    // room (hrw41, only read while in the fallback): the game thread has room for a pair now.
    Out Update(uint32_t key, bool eligible, uint32_t window, bool freshOff, bool room = true) {
        Out o;
        const uint32_t seq = window >> 8;
        o.score = static_cast<int>(window & 0xFFu);
        if (key != key_ || !started_) {
            started_ = true;
            key_ = key;
            fallback_ = false;
            bad_ = 0;
            skipFrom_ = seq;
            lastSeq_ = seq;
            remembered_ = Known(key);
            calm_ = 0;
            falls_ = 0;
            o.newStage = true;
        }
        o.remembered = remembered_;
        o.need = remembered_ ? c_.windowsAgain : c_.windows;
        o.tries = falls_;
        if (!c_.on || !eligible) { fallback_ = false; bad_ = 0; calm_ = 0; lastSeq_ = seq; skipFrom_ = seq; return o; }
        if (fallback_) {   // (hrw41) the way back: calm windows of the locked 30 with room for a pair
            o.bad = bad_;
            o.wait = RetryWait();
            if (c_.retryWindows <= 0) return o;
            if (seq == lastSeq_) { if (!room) windowRoom_ = false; o.calm = calm_; return o; }
            const bool complete = windowRoom_ && room && seq == (lastSeq_ == 0xFFFFFFu ? 1u : lastSeq_ + 1);
            windowRoom_ = true;
            lastSeq_ = seq;
            if (((seq - skipFrom_) & 0xFFFFFFu) < 2) { o.calm = calm_; return o; }   // (the window in which the choice changed holds pictures of both)
            calm_ = (o.score >= c_.score && complete) ? calm_ + 1 : 0;
            o.calm = calm_;
            if (calm_ >= o.wait) {
                fallback_ = false;
                bad_ = 0;
                calm_ = 0;
                skipFrom_ = seq;   // (as a new stage: the next window holds pictures of the locked 30)
                o.retried = true;
            }
            return o;
        }
        if (seq == lastSeq_) { o.bad = bad_; return o; }
        lastSeq_ = seq;
        if (((seq - skipFrom_) & 0xFFFFFFu) < 2) return o;   // (the first window after the stage began holds pictures from before it)
        if (!freshOff) { bad_ = 0; return o; }
        bad_ = o.score < c_.score ? bad_ + 1 : 0;
        o.bad = bad_;
        if (bad_ >= o.need) {
            fallback_ = true;
            windowRoom_ = true;
            Remember(key);
            if (c_.retryWindows > 0) remembered_ = true;   // (hrw41: after a way back one bad window is enough, as in a stage known from before)
            calm_ = 0;
            skipFrom_ = seq;
            if (falls_ < 1000) ++falls_;
            o.tries = falls_;
            o.wait = RetryWait();
            o.fell = true;
        }
        return o;
    }
    int RetryWait() const { return c_.retryWindows <= 0 ? 0 : c_.retryWindows << (falls_ <= 1 ? 0 : falls_ >= 4 ? 3 : falls_ - 1); }   // (windows at the locked 30 before the next try)
    bool Fallback() const { return fallback_; }
    bool Known(uint32_t key) const { for (unsigned i = 0; i < n_; ++i) if (keys_[i] == key) return true; return false; }
    unsigned Remembered() const { return n_; }

private:
    void Remember(uint32_t key) { if (!Known(key)) { if (n_ < 32) keys_[n_++] = key; else keys_[next_++ & 31] = key; } }
    AutoConfig c_;
    uint32_t key_ = 0, skipFrom_ = 0, lastSeq_ = 0, keys_[32] = {};
    unsigned n_ = 0, next_ = 0;
    int bad_ = 0, calm_ = 0, falls_ = 0;
    bool started_ = false, fallback_ = false, remembered_ = false, windowRoom_ = true;
};

inline const char* Name(int v) {
    return v == 1 ? "a locked even 30 (no pairs), the Performance picture" : v == 2 ? "the re-projected 60 (pairs)" : v == 3 ? "a locked even 30 (no pairs), the row's Quality picture keys" : "as the mode says";
}

}  // namespace port::hrwstage
