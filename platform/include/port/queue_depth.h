// How many frames the game thread may have queued for the render thread: open (the in-flight depth + 1) while the back end keeps
// up, cut to one frame while it is the bottleneck. Pure, so the decision is tested on a PC (tests/queue_depth_test.cpp).
//
// The sample is the time one frame took on the render thread, in ns, capped at 60 ms so a shader-compile stall does not decide.
// Over 24 samples: a mean of 26 ms or more cuts the queue to one frame, a mean of 20 ms or less opens it again.
//
// The trap this is built to avoid (GXV_FIXED_DEPTH): when the display is paced to 30 pictures a second, the render thread's frame
// includes its wait for the display queue, so a back end that is far faster than 33 ms still measures about 33 ms. The mean is then
// over 26 ms for ever and never gets under 20 ms: the queue stays at one frame, the game thread and the render thread stop
// overlapping, and the frame time becomes the sum of their two costs instead of the larger one. With `samplesAreWaitFree` the caller
// gives the thread's own CPU time instead of its wall time, which the display wait does not touch.
#pragma once

#include <cstdint>

namespace port::depth {

class Adapter {
public:
    static constexpr int64_t kCapNs = 60000000, kTightNs = 26000000, kOpenNs = 20000000;
    static constexpr unsigned kWindow = 24;

    void Configure(unsigned openLimit) { open_ = openLimit; }

    // Feed one frame's time. Returns the new queue limit when it changes, 0 when it does not.
    unsigned Feed(int64_t ns) {
        sum_ += ns < kCapNs ? ns : kCapNs;
        if (++n_ < kWindow) return 0;
        last_ = sum_ / n_;
        sum_ = 0;
        n_ = 0;
        if (!tight_ && last_ >= kTightNs) {
            tight_ = true;
            return 1;
        }
        if (tight_ && last_ <= kOpenNs) {
            tight_ = false;
            return open_;
        }
        return 0;
    }
    bool Tight() const { return tight_; }
    int64_t LastMean() const { return last_; }

private:
    int64_t sum_ = 0, last_ = 0;
    unsigned n_ = 0, open_ = 3;
    bool tight_ = false;
};

}  // namespace port::depth
