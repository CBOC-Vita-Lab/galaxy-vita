#pragma once
// GXV_DRAIN33 (pace33, 14 Nov 2026): the standing display queue of a fixed cadence is drained, one picture at a time. Pure logic (no clock, no globals): tests/pace33_test.cpp runs this code.
//
// WHY (console c111, GXV_HRW=5: 59.9 pictures a second, even 100/100, and pad to flip 99 ms at Honeyhive, 115 ms at Beach Bowl). Mode 5 makes exactly 60 pictures for 60 refreshes, so the
// display callback (frame_pace.h PresentSync) gives every picture the vblank after the one before it: a chain of slots. A hitch (the stage's first seconds, a shader, a heavy moment) makes one
// picture late, every later picture takes the next slot, and nothing ever gives the lost vblank back: the pictures are finished one, two, three refreshes before they are shown and wait in the
// display queue (2), the back end's queue (1) and the callback. The logs show the chain at 4, 5, 6, 8 and 9 vblanks turn to flip in one stage, changing only at hitches. An even 30 does the same
// (the c111 controls: 5 and 6 vblanks).
//
// WHAT. PresentSync already knows the two things needed: the latency L the pictures need (GXV_LATENCY24: it comes down on evidence) and a catch-up (a picture whose slot the one before it
// still holds takes that vblank; the one before is never seen). This class only decides WHEN one picture may be given up:
//   - behind: this picture's own slot (its turn + L) is earlier than the next free vblank of the chain, and it arrived in time to take the previous picture's vblank;
//   - confirmed: behind for `confirmPics` pictures in a row (0.5 s at 60): one late picture is not a standing queue yet;
//   - not twice in a short time: `gapUs` since the last one (1 s) in play; behind a cover (a closed wipe, a pause, a load) only `gapQuietPics` pictures;
//   - at a still moment when there is one: the camera did not move between the last two turns (the game says so; unknown counts as moving). After `stillWaitUs` (3 s) of waiting for one it is
//     taken anyway: three seconds of 100 ms controls are worse than one 1/60 s step in a moving picture.
// What the player sees of one drain: one picture is left out, the next one comes a refresh early, once: the game's time on the screen steps two turns in one refresh (17 ms). From then on every
// picture is one refresh (16.7 ms) closer to the pad. It repeats until no picture is behind: the chain then sits at L, and L at what the pipeline needs.
#include <cstdint>
#include <stdio.h>

namespace port::drain33 {

struct Config {
    bool on = false;
    unsigned confirmPics = 30;       // GXV_DRAIN33_CONFIRM
    uint32_t gapUs = 1000000;        // GXV_DRAIN33_GAP_MS
    unsigned gapQuietPics = 2;
    uint32_t stillWaitUs = 3000000;  // GXV_DRAIN33_STILL_MS (0 = never wait for a still moment)
};

struct Stats {
    uint32_t pictures = 0, behind = 0;
    uint32_t drops = 0, dropsStill = 0, dropsQuiet = 0, dropsWaited = 0;   // given up: in all; at a still moment; behind a cover; after the wait for a still moment ran out
    uint32_t heldConfirm = 0, heldGap = 0, heldMoving = 0;                 // behind, not given up: not confirmed yet; too soon after the last; the camera moves
};

class Drain {
public:
    void Configure(const Config& c) { c_ = c; Reset(); stats_ = Stats(); total_ = 0; }
    bool Enabled() const { return c_.on; }
    const Config& Cfg() const { return c_; }
    void Reset() { run_ = 0; sinceDrop_ = 1000000u; lastDropUs_ = 0; haveDrop_ = false; waitSince_ = 0; }
    // One call per stamped picture, before PresentSync::Target. behind: see above. still: 1 the camera stands still, 0 it moves, -1 not known. quiet: the screen is covered.
    // True: this picture may take the previous picture's vblank (call Dropped() if PresentSync did it).
    bool Allow(uint64_t nowUs, bool behind, int still, bool quiet) {
        if (!c_.on) return false;
        ++stats_.pictures;
        if (sinceDrop_ < 1000000u) ++sinceDrop_;
        if (!behind) { run_ = 0; waitSince_ = 0; return false; }
        ++stats_.behind;
        ++run_;
        why_ = 0;
        if (quiet) {
            if (sinceDrop_ < c_.gapQuietPics) { ++stats_.heldGap; return false; }
            why_ = 2;
            return true;
        }
        if (run_ < c_.confirmPics) { ++stats_.heldConfirm; return false; }
        if (haveDrop_ && nowUs - lastDropUs_ < c_.gapUs) { ++stats_.heldGap; return false; }
        if (!waitSince_) waitSince_ = nowUs ? nowUs : 1;
        if (still == 1 || c_.stillWaitUs == 0) { why_ = 1; return true; }
        if (nowUs - waitSince_ >= c_.stillWaitUs) { why_ = 3; return true; }
        ++stats_.heldMoving;
        return false;
    }
    void Dropped(uint64_t nowUs) {
        ++stats_.drops; ++total_;
        if (why_ == 1) ++stats_.dropsStill; else if (why_ == 2) ++stats_.dropsQuiet; else if (why_ == 3) ++stats_.dropsWaited;
        lastDropUs_ = nowUs; haveDrop_ = true; sinceDrop_ = 0; run_ = 0; waitSince_ = 0;
    }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }
    uint32_t Total() const { return total_; }

private:
    Config c_;
    unsigned run_ = 0, sinceDrop_ = 1000000u;
    uint64_t lastDropUs_ = 0, waitSince_ = 0;
    bool haveDrop_ = false;
    int why_ = 0;
    Stats stats_;
    uint32_t total_ = 0;
};

// "drain: ..." (every 10 s while it is on): the proof that it acted, and what is left.
// (PATCH2: from a copy of the figures, so that the thread that counts (the display queue's callback: a small stack) is not the one that formats and logs)
inline int FormatLine(char* out, unsigned cap, const Stats& s, const Config& c, uint32_t total, double seconds, double turnToShownVblanks, unsigned latency) {
    return snprintf(out, cap, "drain: GXV_DRAIN33=1: %.1f s, %u pictures, %u behind their slot (finished a refresh or more before they could be shown: the standing queue); pictures given up to "
                              "shorten it %u (at a still moment %u, behind a cover %u, after waiting %.1f s for a still moment %u), %u since the start; behind but kept: not yet %u in a row %u, "
                              "less than %.1f s after the last %u, the camera moving %u; shown %.2f vblanks after the turn's stamp now, L %u",
                    seconds, s.pictures, s.behind, s.drops, s.dropsStill, s.dropsQuiet, c.stillWaitUs / 1e6, s.dropsWaited, total, c.confirmPics, s.heldConfirm, c.gapUs / 1e6,
                    s.heldGap, s.heldMoving, turnToShownVblanks, latency);
}

}  // namespace port::drain33
