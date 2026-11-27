#pragma once
// GXV_FIF (strk27, 13 Nov 2026): frames in flight, the display-end hand-off gate. Pure logic (no clock, no globals): tests/fif_test.cpp runs this code.
//
// Where it comes from. Strikers (np_fps.cpp FramesInFlightGate, MSC_LAT_DEPTH, 1.1; console 23 Oct: "a button reached the screen 150-175 ms after it was read at 30 fps.
// Every stage after the game thread (frame send, decode, back end, GPU, display) is faster than a frame, but the display shows a picture only every second refresh, so
// each stage's queue filled up and every frame waited about a frame at every hand-off"). The game thread starts the next turn (and so reads the pad) only when at most
// `depth` of its pictures are still on their way to the display, counted from the hand-over to the display's flip, not to the end of the back end.
//
// Galaxy's own gate (vgx_galaxy.cpp FrameSink, GXV_INFLIGHT / GXV_LATENCY24_DEPTH) counts only the jobs the back end has not finished executing. A picture the back end has
// already submitted (it waits for the GPU, then for its slot in the display queue) is not counted, so with the GPU or the 30 fps slots as the limit (Quality, Dreadnought: GPU 32.5 ms
// against a 33.4 ms slot) the game thread runs ahead until every queue is full: "even:" showed 10.8-13.7 vblanks (181-229 ms) turn to picture there, 5.1-5.4 at Beach Bowl / Honeyhive.
//
// Safety, all from Strikers' console experience: the wait is at most kMaxWaitUs (three refreshes; Strikers' one) per probe; a turn that came more than 2.5 refreshes after the one before it (a heavy moment) stands the
// gate aside for 150 probes; 30 probes in a row that still found too many in flight switch it off (a pipeline that cannot go faster) and it tries again after 900, 1800, 3600, 7200 probes.
#include <cstdint>

namespace port::fif {

class Gate {
public:
    static constexpr int64_t kRefreshUs = 16683;
    static constexpr int64_t kMaxWaitUs = 3 * kRefreshUs;   // the longest wait of one probe (Strikers: one refresh; here the display's queue is one picture per 2 refreshes)
    struct Stats { uint32_t probes = 0, waits = 0, stillOver = 0, aside = 0, strikes = 0, retries = 0; uint32_t overMax = 0; uint64_t overSum = 0; };
    void Configure(unsigned depth, bool retry = true) { depth_ = depth > 8 ? 8 : depth; retry_ = retry; *this = Gate(depth_, retry_); }
    bool Enabled() const { return depth_ != 0; }
    unsigned Depth() const { return depth_; }
    bool Off() const { return capHits_ >= kStrikeAt; }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }
    // Called once per picture handed over (sent: pictures handed over so far including this one; shown: pictures that have reached the display or were retired).
    // True: wait for the display (at most one refresh), then call Waited().
    bool Want(uint64_t sent, uint64_t shown, int64_t nowUs) {
        if (!depth_) return false;
        ++calls_;
        if (capHits_ >= kStrikeAt && retry_ && calls_ >= retryAt_) { capHits_ = 0; haveCheck_ = false; ++stats_.retries; }
        if (shown == 0 || capHits_ >= kStrikeAt) return false;
        const bool late = haveCheck_ && nowUs - lastUs_ - ownWaitUs_ > kRefreshUs * 5 / 2;   // (the gate's own wait is not lateness)
        ownWaitUs_ = 0;
        lastUs_ = nowUs;
        haveCheck_ = true;
        if (late) { if (!paused_) ++stats_.aside; paused_ = 150; }
        if (paused_) { --paused_; return false; }
        ++stats_.probes;
        const uint64_t over = sent > shown ? sent - shown : 0;
        stats_.overSum += over;
        if (over > stats_.overMax) stats_.overMax = static_cast<uint32_t>(over);
        return over > depth_;
    }
    // After the wait. stillOver: the display had not caught up by the end of it. waitedUs: how long it waited (not counted as a late turn at the next probe).
    void Waited(bool stillOver, int64_t waitedUs = 0) {
        ownWaitUs_ = waitedUs;
        ++stats_.waits;
        if (stillOver) { ++stats_.stillOver; ++capHits_; } else capHits_ = 0;
        if (capHits_ == kStrikeAt) {
            retryAt_ = calls_ + (900u << (strikes_ < 3 ? strikes_ : 3));
            if (strikes_ < 8) ++strikes_;
            ++stats_.strikes;
        }
    }
    uint32_t Paused() const { return paused_; }

private:
    static constexpr unsigned kStrikeAt = 30;
    Gate(unsigned d, bool r) : depth_(d), retry_(r) {}
public:
    Gate() = default;
private:
    unsigned depth_ = 0;
    bool retry_ = true, haveCheck_ = false;
    unsigned capHits_ = 0, paused_ = 0, strikes_ = 0;
    uint32_t calls_ = 0, retryAt_ = 0;
    int64_t lastUs_ = 0, ownWaitUs_ = 0;
    Stats stats_;
};

}  // namespace port::fif
