// The GXV_PRESENT_SYNC class PresentSync exactly as it was at f6c0c23 (platform/include/port/frame_pace.h), renamed PresentSyncRef: the reference
// tests/latency24_model.cpp compares the new class (Latency24 off) against, decision for decision. Do not edit.
#pragma once
#include <cstdint>

namespace port::pace {
class PresentSyncRef {
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

}  // namespace port::pace
