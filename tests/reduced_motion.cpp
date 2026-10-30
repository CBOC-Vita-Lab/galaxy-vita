// Reduced motion (platform/include/port/reduced_motion.h): shake scale, flash cap, off = identity, the GXV_REDUCED_MOTION switch, and
// the projection arithmetic the camera context uses (the real TProj3f from the decomp's JGeometry).
//   reduced_motion            the arithmetic checks
//   reduced_motion --env 0|1  the switch as the process was started (Enabled() is read once)
#include "port/reduced_motion.h"
#include <JSystem/JGeometry.hpp>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace port::motion;

static uint32_t Bits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

// CameraShakePatternSingly::update, copied: the offset of one shake at frame `frame` of kFrames.
static float SinglyOffset(float intensity, int frame) {
    const int kFrames = 25;
    int left = kFrames - frame;
    if (left < 0) left = 0;
    float amount = intensity * std::sin(3.14159265f * 4.0f * left / kFrames);
    return amount * std::sin(3.14159265f * 0.5f * left / kFrames);
}

// CameraContext::updateProjectionMtx for a given shake offset.
static TProj3f Projection(float x, float y) {
    TProj3f p;
    p.makePerspective(45.0f, 16.0f / 9.0f, 100.0f, 800000.0f);
    TProj3f t;
    t.makeTrans(x, y);
    p.concat(t, p);
    return p;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--env") == 0) {
        assert(Enabled() == (argv[2][0] == '1'));  // the process was started with the switch in the environment
        assert(ShakeScale(8.0f) == (Enabled() ? 2.0f : 8.0f));
        assert(CapFlashAlpha(1.0f, 6, 255, 255, 255) == (Enabled() ? kFlashAlphaCap : 1.0f));
        return 0;
    }

    // The switch: unset, empty, 0 or not a number is off; any other number is on.
    for (const char* off : {static_cast<const char*>(nullptr), "", "0", "0x0", "00", "-0", " ", "-", "abc", "1suffix", "yes", "on"}) assert(!ParseSwitch(off));
    for (const char* on : {"1", "0x1", "7", "-1", "01", "+1", "100"}) assert(ParseSwitch(on));
    assert(!Enabled() || std::getenv("GXV_REDUCED_MOTION"));  // default off

    // The numbers the Settings text promises.
    assert(kShakeScale == 0.25f && kFlashAlphaCap == 0.40f);

    // Off is identity, bit for bit, for every value including the odd ones.
    for (float v : {0.0f, -0.0f, 1.0f, -1.0f, 0.08f, 9.0f, 123.456f, -0.5625f, 1e-38f, 3.4e38f, INFINITY, -INFINITY, NAN})
        assert(Bits(ShakeScale(v, false)) == Bits(v));
    for (float a : {0.0f, 0.1f, 0.4f, 0.5f, 1.0f, NAN})
        for (int frames : {0, 1, 12, 13, 30, 60})
            for (int c : {0, 100, 160, 255}) assert(Bits(CapFlashAlpha(a, frames, c, c, c, false)) == Bits(a));

    // On: the shake is a quarter. Exact for any float (a power of two), so zero stays zero and the sign is kept.
    for (float v : {0.0f, 1.0f, -1.0f, 0.08f, 9.0f, -4.5f, 123.456f}) {
        assert(ShakeScale(v, true) == v * 0.25f);
        assert(std::signbit(ShakeScale(v, true)) == std::signbit(v));
    }
    assert(ShakeScale(8.0f, true) == 2.0f && ShakeScale(0.0f, true) == 0.0f);

    // The shake keeps its timing: scaled and unscaled offsets are zero on the same frames, change sign on the same frames, and peak
    // on the same frame; only the size changes. (The shake task lengths and delays are not read by the scale at all.)
    for (float intensity : {0.08f, 0.2f, 0.5f, 1.0f, 3.0f, 6.0f, 9.0f}) {
        int peak = 0, peakScaled = 0;
        float best = 0, bestScaled = 0;
        for (int frame = 0; frame <= 26; ++frame) {
            const float full = SinglyOffset(intensity, frame), calm = ShakeScale(full, true);
            assert((full == 0.0f) == (calm == 0.0f) && std::signbit(full) == std::signbit(calm));
            assert(std::fabs(calm) <= std::fabs(full) * 0.25f * 1.0000001f);
            if (std::fabs(full) > best) { best = std::fabs(full); peak = frame; }
            if (std::fabs(calm) > bestScaled) { bestScaled = std::fabs(calm); peakScaled = frame; }
        }
        assert(peak == peakScaled && bestScaled <= best * 0.25f * 1.0000001f && best > 0.0f);
    }

    // The flash: a short bright fade is held to 40 %; a long one, a dark one and a lower opacity are not.
    assert(Luma(255, 255, 255) == 255 && Luma(0, 0, 0) == 0 && Luma(160, 160, 160) == 160);
    assert(IsShortBrightFlash(1, 255, 255, 255) && IsShortBrightFlash(12, 255, 255, 255) && !IsShortBrightFlash(13, 255, 255, 255));
    assert(IsShortBrightFlash(12, 160, 160, 160) && !IsShortBrightFlash(12, 159, 159, 159));
    assert(!IsShortBrightFlash(12, 0, 0, 0) && !IsShortBrightFlash(12, 255, 0, 0) /* luma 76 */ && IsShortBrightFlash(12, 255, 255, 0) /* 226 */);
    assert(CapFlashAlpha(1.0f, 6, 255, 255, 255, true) == 0.40f);
    assert(CapFlashAlpha(0.41f, 12, 255, 255, 255, true) == 0.40f);
    assert(CapFlashAlpha(0.40f, 12, 255, 255, 255, true) == 0.40f && CapFlashAlpha(0.25f, 12, 255, 255, 255, true) == 0.25f);
    assert(CapFlashAlpha(0.0f, 0, 255, 255, 255, true) == 0.0f);
    // Fades the player needs stay exactly as they are: the game's own are 30 frames or more (30 scene fade, 60 TicoFat / Bowser, 80-180).
    for (int frames : {13, 30, 45, 60, 80, 90, 180}) for (float a : {0.0f, 0.5f, 1.0f}) assert(CapFlashAlpha(a, frames, 255, 255, 255, true) == a);
    for (float a : {0.5f, 1.0f}) for (int frames : {1, 6, 12}) assert(CapFlashAlpha(a, frames, 0, 0, 0, true) == a);  // to black
    // Never raises opacity, and keeps the order of opacities (a rising fade still rises).
    float last = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float a = i / 100.0f, c = CapFlashAlpha(a, 6, 255, 255, 255, true);
        assert(c <= a && c >= last);
        last = c;
    }

    // The projection: the camera context folds the shake into the projection as a translation, so scaling the offset scales
    // the shake on screen, linearly, and nothing else in the matrix changes.
    const TProj3f rest = Projection(0.0f, 0.0f);
    for (auto [x, y] : {std::pair<float, float>{0.0f, 0.0f}, {0.0f, 0.5f}, {-0.3f, 0.07f}, {0.56f, -0.56f}}) {
        const TProj3f full = Projection(x, y), calm = Projection(ShakeScale(x, true), ShakeScale(y, true)), same = Projection(ShakeScale(x, false), ShakeScale(y, false));
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const bool shifted = i < 2 && j == 2;  // the two entries that carry the shake
                assert(Bits(same.mMtx[i][j]) == Bits(full.mMtx[i][j]));                // option off: the very same matrix
                if (!shifted) {
                    assert(Bits(calm.mMtx[i][j]) == Bits(full.mMtx[i][j]));            // lens, depth range, aspect: untouched
                } else {
                    const float shakeFull = full.mMtx[i][j] - rest.mMtx[i][j], shakeCalm = calm.mMtx[i][j] - rest.mMtx[i][j];
                    assert(std::fabs(shakeCalm - shakeFull * 0.25f) <= 1e-6f);        // the screen shift is a quarter
                }
            }
    }
    std::puts("reduced_motion: switch, scale, cap, identity and projection arithmetic passed");
    return 0;
}
