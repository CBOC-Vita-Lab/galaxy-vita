// q30cush31 (18 Nov 2026): GXV_Q30_CUSHION and the "q30sep:" census. Pure rules, no Vita calls; host model and mutants: tests/q30cushion.cpp (tools/test_q30cushion.sh).
// The glue: platform/vgx/vita/vgx_host_galaxy_vita.cpp PaceDisplay (the fixed 30's display callback), platform/src/port/perf.cpp (port_pose_phase_window_take).
//
// WHAT THE DISPLAY CALLBACK DOES (CONFIRMED by reading it). The callback runs on the back-end thread after the picture's GPU fence. Its slot (`due`, a vblank number) is the
// last slot + 2 (PresentationSlots::Next); a picture that is ready AFTER its slot's vblank is shown at the first vblank after it (3 vblanks = a 50 ms hold) and the rhythm
// is re-anchored there. The wait returns at the start of the vblank before the slot, so the picture has to be READY BEFORE ITS SLOT'S VBLANK STARTS. The distance from the
// moment it is ready to that vblank is the picture's MARGIN (this file: marginUs). A spike of the pair's work (game thread, back end, GPU) that is longer than the margin is
// a 50 ms hold; a shorter one is invisible. GXV_Q30_DRAIN only takes a backlog of 2+ whole vblanks away (it never leaves less than one whole vblank of room), so it is not
// what makes the margin small: the margin is whatever the pair's own latency leaves modulo the vblank, 0..16.7 ms when nothing queues up.
//
// THE CUSHION (GXV_Q30_CUSHION=1, default 0). When the margin is seen to be thin (`near` pictures: margin under guardUs, or late), the slot is moved ONE vblank later
// (level 1): the margin grows by 16.7 ms and a 2-4 ms (even a 10 ms) spike is absorbed. The price is stated, not hidden: one 3-vblank hold at the moment it engages (the
// same hold the first miss would have cost, taken on purpose; behind a cover it costs nothing visible) and ONE vblank (16.7 ms) more turn-to-picture time while it is on. It
// is not a second queued picture and it never goes beyond level 1. It lets go (a 1-vblank hold, or free behind a cover) only after `calmPics` pictures whose margin would
// still have been over guardUs without it. The drain's threshold is raised by the level so the two never undo each other. Census (CushionCensus): engagements, releases, the
// pictures the cushion saved (margin with it >= 0 but without it < 0: the holds avoided), pictures late in spite of it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace port {
namespace q30cush {

constexpr int64_t kVblankUs = 16683;  // 59.94 Hz: 16683.35 us (the margin's error over a few pictures is far under a millisecond)

#ifdef PORT_Q30CUSH_MUTANT
extern int gMutant;  // (tests/q30cushion.cpp only)
#define PORT_QC_MUT(k) (::port::q30cush::gMutant == (k))
#else
#define PORT_QC_MUT(k) false
#endif

struct Config {
    int64_t guardUs = 6000;     // a margin under this is "near" (a 2-4 ms spike plus the clock's own wobble fits under 6)
    unsigned window = 64;       // pictures looked at for the engage rule (2 s)
    unsigned engageNear = 3;    // near pictures among the window's that engage the cushion (in play)
    unsigned coverMin = 8;      // behind a cover: this many pictures of the new rhythm are measured, a thin phase then engages at once (free: nothing is watched)
    unsigned calmPics = 1800;   // 60 s of pictures with the margin to spare before it lets go (in play); behind a cover it lets go as soon as the margin allows
    unsigned maxLevel = 1;      // never more than one vblank
    unsigned verifyPics = 16;   // after an engagement in play: this many pictures are measured; the margin must have grown by half a vblank, else it was no use and goes back
};

// One picture's margin is judged here. Picture() returns the vblanks to add to this picture's slot (+1 engage, -1 release, else 0).
class Cushion {
public:
    explicit Cushion(const Config& c = Config()) : c_(c) {}
    void Reset() { level_ = 0; nearRing_ = 0; ringN_ = 0; coverN_ = 0; calm_ = 0; coverMinMargin_ = INT64_MAX; sinceChange_ = 0; verify_ = 0; gaveUp_ = false; ++epochs; }
    bool GaveUp() const { return gaveUp_; }
    unsigned Level() const { return level_; }
    // The drain's room threshold: it may take a vblank only while the ring's lowest `early` is at least this (2 without the cushion).
    unsigned DrainFloor() const { return PORT_QC_MUT(4) ? 2u : 2u + level_; }
    // marginUs: the margin the picture has with the slot as it is NOW (the level's vblank included); play: not a cover / load.
    int Picture(int64_t marginUs, bool play) {
        ++pictures;
        const bool nearNow = marginUs < c_.guardUs;
        if (nearNow) ++nearPics;
        if (marginUs < 0) ++latePics;
        if (level_ > 0 && marginUs >= 0 && marginUs - static_cast<int64_t>(level_) * kVblankUs < 0) ++avoided;   // would have been a 3-vblank hold without it
        if (level_ > 0 && marginUs < 0) ++lateDespite;
        ++sinceChange_;
        // the window of "near" flags and margins
        nearRing_ = (nearRing_ << 1) | (nearNow ? 1ull : 0ull);
        margins_[pos_++ & 63u] = marginUs;
        if (ringN_ < 64) ++ringN_;
        if (verify_ > 0) {   // measuring what the cushion bought (in play)
            if (marginUs < verifyMin_) verifyMin_ = marginUs;
            if (--verify_ == 0 && verifyMin_ < beforeMin_ + kVblankUs / 2 + (PORT_QC_MUT(5) ? -1000000 : 0)) {
                ++gaveUpN; gaveUp_ = true; return Release(false);   // no gain: the chain is coupled (the next picture waits for this slot), a later slot buys nothing
            }
        }
        if (!play) {
            // behind a cover: measure the rhythm's phase over coverMin pictures, then decide once (free of charge: the hold is not watched)
            if (coverN_ < 0xFFFFFFFFu) ++coverN_;
            if (marginUs < coverMinMargin_) coverMinMargin_ = marginUs;
            if (coverN_ == c_.coverMin) {
                if (level_ < c_.maxLevel && coverMinMargin_ < c_.guardUs && !gaveUp_) return Engage(true);
                if (level_ > 0 && coverMinMargin_ - static_cast<int64_t>(level_) * kVblankUs >= c_.guardUs) return Release(true);
            }
            calm_ = 0;
            return 0;
        }
        coverN_ = 0; coverMinMargin_ = INT64_MAX;
        // in play
        if (level_ < c_.maxLevel && ringN_ >= 8 && !gaveUp_) {
            const unsigned w = c_.window < 64 ? c_.window : 64;
            const uint64_t mask = w >= 64 ? ~0ull : ((1ull << w) - 1ull);
            if (static_cast<unsigned>(__builtin_popcountll(nearRing_ & mask)) >= c_.engageNear && !PORT_QC_MUT(3)) return Engage(false);
        }
        if (level_ > 0) {
            // calm: the margin without the cushion would still be over the guard
            if (marginUs - static_cast<int64_t>(level_) * kVblankUs >= c_.guardUs) { if (++calm_ >= c_.calmPics) return Release(false); }
            else calm_ = 0;
        }
        return 0;
    }
    // census
    uint64_t pictures = 0, nearPics = 0, latePics = 0, avoided = 0, lateDespite = 0, engages = 0, releases = 0, engagesCover = 0, epochs = 0, gaveUpN = 0;
private:
    int Engage(bool cover) {
        if (!cover) {   // the lowest margin of the window before it (the verify step compares with it)
            beforeMin_ = INT64_MAX;
            for (unsigned i = 0; i < ringN_; ++i) if (margins_[(pos_ - 1u - i) & 63u] < beforeMin_) beforeMin_ = margins_[(pos_ - 1u - i) & 63u];
            verify_ = c_.verifyPics; verifyMin_ = INT64_MAX;
        }
        ++level_; ++engages; if (cover) ++engagesCover;
        nearRing_ = 0; ringN_ = 0; calm_ = 0; sinceChange_ = 0;
        return PORT_QC_MUT(1) ? 0 : 1;
    }
    int Release(bool cover) {
        (void)cover; verify_ = 0;
        --level_; ++releases; nearRing_ = 0; ringN_ = 0; calm_ = 0; sinceChange_ = 0;
        return PORT_QC_MUT(2) ? 0 : -1;
    }
    Config c_;
    unsigned level_ = 0;
    uint64_t nearRing_ = 0;
    unsigned ringN_ = 0, coverN_ = 0, calm_ = 0, sinceChange_ = 0, verify_ = 0, pos_ = 0;
    bool gaveUp_ = false;
    int64_t margins_[64] = {}, beforeMin_ = 0, verifyMin_ = INT64_MAX;
    int64_t coverMinMargin_ = INT64_MAX;
};

// The margin: the time from the moment the picture is ready (nowUs) to the start of its slot's vblank. anchorIdx / anchorUs: a vblank number and the time its wait returned.
inline int64_t MarginUs(uint32_t due, uint32_t anchorIdx, uint64_t anchorUs, uint64_t nowUs) {
    const int32_t d = static_cast<int32_t>(due - anchorIdx);
    return static_cast<int64_t>(anchorUs) + static_cast<int64_t>(d) * kVblankUs - static_cast<int64_t>(nowUs);
}

// q30cush31b: the margin of a picture against the slot the rhythm ASKED for. PresentationSlots::Next() pulls a late picture's slot forward to the next vblank (`earliest`), so the slot
// it returns (`due`) is never behind the picture: measured against `due`, a late picture has a margin of 0..16.7 ms and looks on time (console 18 Nov, QS2: 12 long holds, 0 late pictures,
// 5 near ones in 86 windows, so the engage rule never saw three). clampAdv = due - PresentationSlots::Nominal(): the vblanks Next() moved it. Mutant 6: ignore it (the old measure).
inline int64_t MarginUsNominal(uint32_t due, uint32_t clampAdv, uint32_t anchorIdx, uint64_t anchorUs, uint64_t nowUs) {
    return MarginUs(due - (PORT_QC_MUT(6) ? 0u : clampAdv), anchorIdx, anchorUs, nowUs);
}

// ---- the "q30sep:" window: the three candidate causes of Quality's odd stutter, apart, one line per window --------------------------------------------------------------
//   (a) LATE pictures: the picture was ready after its slot (margin < 0): a 3+ vblank hold that is the pipeline's. `late`, with `longHold` = holds of 3+ vblanks in all.
//   (b) POSE rhythm: read from GXV_POSE_PHASE_CENSUS's counters (port_pose_phase_window_take): decisions seen at pose age >= 1, and the skip turns.
//   (c) DRAIN / no cushion: `drainHold` (1-vblank holds: a drain's own), `drainLate` (late within 30 pictures after a drain: the drain's room was too thin), `near` (margin under
//       the guard: the pictures one spike from a hold) and the margin's low tail. GXV_Q30_CUSHION's `avoided` and level close the line.
struct SepWindow {
    uint32_t pictures = 0, hold[5] = {};     // holds 0/1/2/3/4+ vblanks
    uint32_t late = 0, nearPics = 0, drains = 0, drainHold = 0, drainLate = 0, lateHoldOther = 0;
    int64_t marginMin = INT64_MAX;
    uint32_t marginHist[12] = {};            // margin in ms: <0, 0-2, 2-4, 4-6, 6-8, 8-12, 12-16, 16-20, 20-25, 25-30, 30-40, 40+
    unsigned sinceDrain = 1000;
    void Drained() { ++drains; sinceDrain = 0; }
    static unsigned Bin(int64_t us) {
        if (us < 0) return 0;
        const int64_t ms = us / 1000;
        static const int64_t edge[11] = {2, 4, 6, 8, 12, 16, 20, 25, 30, 40, 1 << 30};
        for (unsigned i = 0; i < 11; ++i) if (ms < edge[i]) return i + 1;
        return 11;
    }
    // One picture: hold (vblanks since the last picture, 0 for the first), its margin and the guard
    void Picture(unsigned holdV, int64_t marginUs, int64_t guardUs) {
        ++pictures; ++hold[holdV < 4 ? holdV : 4];
        if (marginUs < marginMin) marginMin = marginUs;
        ++marginHist[Bin(marginUs)];
        const bool isLate = marginUs < 0;
        if (isLate) { ++late; if (sinceDrain < 30) ++drainLate; }
        else if (marginUs < guardUs) ++nearPics;
        if (holdV == 1 && drainPending_) ++drainHold;
        if (holdV >= 3 && !isLate) ++lateHoldOther;   // a long hold with the picture on time: the pacer left a turn out / nothing was delivered (not the display's margin)
        drainPending_ = false;
        if (sinceDrain < 1000) ++sinceDrain;
    }
    void DrainNext() { drainPending_ = true; Drained(); }
    int Format(char* out, unsigned cap, double seconds, unsigned cushionLevel, const char* cushionText) const {
        char mh[160];
        int o = 0;
        static const char* const lab[12] = {"<0", "0-2", "2-4", "4-6", "6-8", "8-12", "12-16", "16-20", "20-25", "25-30", "30-40", "40+"};
        for (unsigned i = 0; i < 12 && o < static_cast<int>(sizeof mh) - 16; ++i) if (marginHist[i]) o += std::snprintf(mh + o, sizeof mh - o, "%s%s:%u", o ? " " : "", lab[i], marginHist[i]);
        if (!o) mh[0] = 0;
        return std::snprintf(out, cap,
            "q30sep: %.1f s, %u pictures; (a) late pictures %u (a long hold %u of them with the picture on time %u); (c) drain: %u drains, %u drain holds (1 vblank), %u late within 1 s of a drain, "
            "near (margin < guard) %u, margin min %.1f ms, margin ms {%s}; cushion level %u%s; holds 0/1/2/3/4+ %u/%u/%u/%u/%u",
            seconds, pictures, late, hold[3] + hold[4], lateHoldOther, drains, drainHold, drainLate, nearPics, marginMin == INT64_MAX ? 0.0 : static_cast<double>(marginMin) / 1000.0, mh, cushionLevel,
            cushionText ? cushionText : "", hold[0], hold[1], hold[2], hold[3], hold[4]);
    }
    void Clear() { const unsigned sd = sinceDrain; const bool dp = drainPending_; *this = SepWindow(); sinceDrain = sd; drainPending_ = dp; }
private:
    bool drainPending_ = false;
};

}  // namespace q30cush
}  // namespace port
