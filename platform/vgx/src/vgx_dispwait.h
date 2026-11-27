// How much of the display scene's GPU time is a wait for its buffer, as a pure estimator (strikersrev25; the "gpu-scenes:" log line, GXV_GPU_SCENES).
//
// The back end ends the display scene with vitaGL's back buffer's sync object as its FRAGMENT sync object (vitaGL gxm.c scene_reset:
// sceGxmBeginScene(..., NULL, gxm_sync_objects[gxm_back_buffer_index], ...)): the GPU does not start that scene's pixel work before the display has
// let go of the buffer. The scene timers (vitaGL's rt_gpu.c, the back end's gpuscenes::Take) count a scene from the later of its submission and the
// previous scene's completion to its own completion, so that wait is inside the display scene's span, its 3D and the picture's "busy" and "pixel
// (3D)" totals. It is there when the producer is ahead of the display (near 60 pictures a second, the CPU done early); it is not when the GPU is
// the limit (the buffer was free long before the GPU got to the scene).
//
// Not measurable directly (the GPU gives no timestamp for "the wait ended"), so it is estimated the way GXV_PACE23's HonestPicture does: the display
// scene costs about the same every picture (a stretch and a smoothing of a fixed size), so what it takes over the smallest span of its recent
// pictures, plus 1/8 and 0.3 ms, is the wait. A LOWER bound: when every picture waits, the minimum has a wait in it too (the ceiling stops that from
// hiding a long one: a display scene is never counted as costing more than the ceiling, 8 ms by default).
// Host test: tests/vgx_dispwait.cpp (tools/test_vgx_dispwait.sh).
#pragma once
#include <algorithm>
#include <cstdint>

namespace vgx {
namespace dispwait {

class Estimator {
public:
    static constexpr unsigned kRing = 96;    // pictures remembered
    static constexpr unsigned kMinPictures = 8; // none is judged before this many
    static constexpr uint32_t kCeilingUs = 8000;

    // A change of size or step: the display scene's cost may differ now, its minimum is taken afresh.
    void Reset() { head_ = n_ = 0; }
    // spanUs: the picture's display scene(s), submission (or the previous scene's end) to completion. Returns the part of it taken to be a wait.
    uint32_t Add(uint32_t spanUs, uint32_t ceilingUs = kCeilingUs) {
        if (spanUs == 0) return 0;
        ring_[head_] = spanUs;
        head_ = (head_ + 1) % kRing;
        if (n_ < kRing) ++n_;
        if (n_ < kMinPictures) return 0;
        uint32_t low = ~0u;
        for (unsigned i = 0; i < n_; ++i) low = std::min(low, ring_[i]);
        const uint32_t cap = std::min(low + low / 8u + 300u, ceilingUs);
        return spanUs > cap ? spanUs - cap : 0u;
    }
    unsigned Pictures() const { return n_; }

private:
    uint32_t ring_[kRing] = {};
    unsigned head_ = 0, n_ = 0;
};

} // namespace dispwait
} // namespace vgx
