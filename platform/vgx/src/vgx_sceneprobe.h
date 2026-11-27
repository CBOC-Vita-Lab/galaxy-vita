// GXV_SCENE_PROBE=<n> (default 0, test-only, never shipped on): n empty scenes at the start of every picture, to measure what one scene costs the GPU.
//
// Why (perfswitches/FABLE-IDEAS.md, idea 5): every scene-removing idea (tail fold, dead bloom bracket, direct display, water capture) is valued at "about 1 ms a scene",
// from a fit (vgx_ta27.h) and two A/Bs. This switch adds n scenes a picture whose content is nothing but a clear of a private 64x64 target, so the scene count moves and
// nothing else does; the difference of the "gpu-scenes:" busy / TA / 3D (and of fps) between n = 0, 2 and 4, divided by n, is the marginal cost of a scene as the console
// really pays it. It also tells whether that cost is GPU time or the watcher threads' latency (they share a core with the render thread): a cost that shows in
// "busy" is the GPU's; one that shows only in the render thread's ms is the CPU's.
//
// This header is the pure part: the clamp of n, the five-second window and its log line, and the arithmetic of the marginal cost. The back end's part (which makes the scenes)
// is sceneprobe in vgx_backend_gl.cpp. Host test: tests/vgx_sceneprobe.cpp (tools/test_vgx_sceneprobe.sh).
#pragma once
#include <cstdint>
#include <cstdio>

namespace vgx {
namespace sceneprobe {

// The most scenes a picture may be given: vitaGL's scene watcher follows 48 scenes (gpu_timers.py RT_SLOTS - 16) and a Beach Bowl picture has 17 of its own, so beyond
// 16 the timers would lose scenes and the line would measure the watcher, not the GPU.
constexpr unsigned kMaxScenes = 16;
constexpr int kTarget = 64; // the private target's side, pixels

// The value of the key: anything below 1 (or not a number the caller could read) is off; above kMaxScenes is kMaxScenes.
inline unsigned Clamp(long v) { return v < 1 ? 0u : (v > static_cast<long>(kMaxScenes) ? kMaxScenes : static_cast<unsigned>(v)); }

// A five-second window of what the probe did: pictures seen, probe scenes made, probe scenes it could not make (no target object).
struct Window {
    uint32_t pictures = 0, made = 0, refused = 0;
    void Picture(unsigned madeNow, unsigned refusedNow) { ++pictures; made += madeNow; refused += refusedNow; }
    void Reset() { *this = Window{}; }
    // "scene-probe: GXV_SCENE_PROBE=4: 300 pictures in 5.0 s, 4.00 empty 64x64 scenes a picture (1200 made, 0 refused); read gpu-scenes busy / TA / 3D at n = 0, 2, 4"
    int Format(char* out, size_t cap, unsigned n, double secs) const {
        const double per = pictures ? static_cast<double>(made) / pictures : 0.0;
        return std::snprintf(out, cap,
                             "scene-probe: GXV_SCENE_PROBE=%u: %u pictures in %.1f s, %.2f empty %dx%d scenes a picture (%u made, %u refused)%s; "
                             "marginal scene = (gpu-scenes busy at n - busy at 0) / n",
                             n, static_cast<unsigned>(pictures), secs, per, kTarget, kTarget, static_cast<unsigned>(made), static_cast<unsigned>(refused),
                             refused ? ": REFUSED scenes make the run invalid" : "");
    }
};

// The marginal cost of one scene, ms: the picture time (or the busy / TA / 3D ms a picture) at n scenes against the control at 0. Not defined for n = 0.
inline bool MarginalMs(double msAtN, double msAt0, unsigned n, double* out) {
    if (n == 0 || !out) return false;
    *out = (msAtN - msAt0) / static_cast<double>(n);
    return true;
}

// The slope of a line through (n, ms) points by least squares (n = 0, 2, 4 in the session): ms a scene. False with fewer than two distinct n.
inline bool SlopeMs(const unsigned* n, const double* ms, unsigned count, double* out) {
    if (!out || count < 2) return false;
    double sn = 0, sm = 0;
    for (unsigned i = 0; i < count; ++i) { sn += n[i]; sm += ms[i]; }
    const double mn = sn / count, mm = sm / count;
    double num = 0, den = 0;
    for (unsigned i = 0; i < count; ++i) { num += (n[i] - mn) * (ms[i] - mm); den += (n[i] - mn) * (n[i] - mn); }
    if (den <= 0.0) return false;
    *out = num / den;
    return true;
}

} // namespace sceneprobe
} // namespace vgx
