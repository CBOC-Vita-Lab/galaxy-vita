// Recovery after the console slept or the player went to the Home menu and came back.
//
// Two small pieces shared by the disc reader and the audio output:
//  - port_resume_generation(): a counter the Vita runtime bumps when the system reports a resume (vita/runtime/audio.cpp);
//    weak, so a build without it (desktop, headset, host tests) reads 0 and never sees a resume.
//  - port::ReopenGate: how soon a failed (re)open may be tried again, so a device that is not ready right after a resume
//    is retried instead of being given up for good.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
unsigned port_resume_generation(void) __attribute__((weak));
#ifdef __cplusplus
}

namespace port {

inline unsigned ResumeGeneration() { return port_resume_generation ? port_resume_generation() : 0u; }

// Retry schedule after failed opens: now, then 250 ms, 500 ms, 1 s, then every 2 s.  Time is in nanoseconds on any
// monotonic clock the caller picks.
class ReopenGate {
public:
    bool Due(int64_t nowNs) const { return nowNs >= next_; }
    void Failed(int64_t nowNs) {
        if (failures_ < 1000) ++failures_;
        int64_t delay = 250000000ll;
        for (unsigned i = 1; i < failures_ && delay < 2000000000ll; ++i) delay *= 2;
        if (delay > 2000000000ll) delay = 2000000000ll;
        next_ = nowNs + delay;
    }
    void Succeeded() { failures_ = 0; next_ = 0; }
    unsigned Failures() const { return failures_; }

private:
    int64_t next_ = 0;
    unsigned failures_ = 0;
};

}  // namespace port
#endif
