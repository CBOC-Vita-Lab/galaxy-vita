// Tilt sensitivity (platform/include/port/tilt_gain.h): the 1 to 10 scale and its percentages, 100% leaves the vector alone bit for bit, the lean grows and shrinks with the
// percentage and never reverses, the result stays unit length, nothing becomes NaN at the extremes, the stick path of
// vita/runtime/input.cpp gives the same answers, and the switch parses as the menu writes it.
//   tilt_gain            the arithmetic
//   tilt_gain --env P    GXV_TILT_GAIN=P as the process started (Percent() is read once); P is the expected percentage
#include "port/tilt_gain.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace port::tilt;
struct V { float x, y, z; };
static V Lean(float degrees, float heading) {   // heading in degrees around the Y axis: 0 leans toward +X
    const float a = degrees * 3.14159265f / 180.0f, h = heading * 3.14159265f / 180.0f;
    return {std::sin(a) * std::cos(h), -std::cos(a), std::sin(a) * std::sin(h)};
}
static V Stick(float sx, float sy) {   // the stick path of input.cpp
    const float x = sx * 0.65f, z = sy * 0.65f;
    return {x, -std::sqrt(std::fmax(0.0f, 1.0f - x * x - z * z)), z};
}
static float LeanOf(const V& v) { return std::atan2(std::sqrt(v.x * v.x + v.z * v.z), -v.y) * 180.0f / 3.14159265f; }
static float Len(const V& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static V Scaled(V v, int percent) { Scale(v.x, v.y, v.z, percent); return v; }

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--env")) {
        assert(Percent() == std::atoi(argv[2]));
        std::puts("tilt_gain: switch as started passed");
        return 0;
    }
    // The menu's values and the switch's parsing.
    for (int l = -2; l <= 14; ++l) assert(ValidLevel(l) == (l >= 1 && l <= 10));
    // The scale: 1 is half, 5 the game's own (100 %), 10 double, rising at every step, and an invalid level reads as the game's own.
    const int want[10] = {50, 63, 75, 88, 100, 120, 140, 160, 180, 200};
    for (int l = 1; l <= 10; ++l) { assert(PercentOfLevel(l) == want[l - 1]); assert(LevelOfPercent(want[l - 1]) == l); if (l > 1) assert(PercentOfLevel(l) > PercentOfLevel(l - 1)); }
    assert(PercentOfLevel(0) == 100 && PercentOfLevel(11) == 100 && PercentOfLevel(5) == kDefaultPercent && kDefaultLevel == 5);
    // The 1.2 beta's percentages (50 to 200 in steps of 25) map to the nearest level; anything else too, ties to the lower level.
    const int beta[7][2] = {{50, 1}, {75, 3}, {100, 5}, {125, 6}, {150, 7}, {175, 9}, {200, 10}};
    for (const auto& b : beta) assert(LevelOfPercent(b[0]) == b[1]);
    assert(LevelOfPercent(0) == 1 && LevelOfPercent(999) == 10 && LevelOfPercent(150) == 7 && LevelOfPercent(94) == 4 && LevelOfPercent(95) == 5);
    assert(ParsePercent(nullptr) == 100 && ParsePercent("") == 100 && ParsePercent("abc") == 100 && ParsePercent("150x") == 100);
    assert(ParsePercent("100") == 100 && ParsePercent("50") == 50 && ParsePercent("200") == 200 && ParsePercent("75") == 75 && ParsePercent("63") == 63 && ParsePercent("130") == 130);
    assert(ParsePercent("10") == 50 && ParsePercent("-5") == 50 && ParsePercent("900") == 200 && ParsePercent("0100") == 100);

    // 100% is today's behaviour bit for bit: any vector, even one that is not unit length or not finite, is untouched.
    const V odd[] = {{0, -1, 0}, {0.3f, -0.9f, 0.1f}, {0.6f, 0.1f, -0.7f}, {5, 6, 7}, {0, 0, 0},
                     {std::numeric_limits<float>::quiet_NaN(), -1, 0}, {std::numeric_limits<float>::infinity(), -1, 0}};
    for (const auto& v : odd) {
        V w = v; Scale(w.x, w.y, w.z, 100);
        assert(std::memcmp(&v, &w, sizeof v) == 0);
    }
    // Level: nothing to scale, at every percentage.
    for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) { const V l = Scaled({0, -1, 0}, p); assert(l.x == 0 && l.y == -1 && l.z == 0); }
    // Not finite input is left alone at every percentage (never made worse).
    for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) {
        V n{std::numeric_limits<float>::quiet_NaN(), -1, 0}; Scale(n.x, n.y, n.z, p); assert(std::isnan(n.x) && n.y == -1 && n.z == 0);
        V i{0.1f, -std::numeric_limits<float>::infinity(), 0}; Scale(i.x, i.y, i.z, p); assert(i.x == 0.1f && std::isinf(i.y));
    }

    // Angle-correct: a lean of A degrees becomes A * percent / 100 (until the 85 degree stop), in the same direction, still unit length.
    for (float heading = 0; heading < 360; heading += 37) for (float degrees = 0.5f; degrees <= 120; degrees += 4.5f) {
        const V base = Lean(degrees, heading);
        float last = -1;
        for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) {
            const V w = Scaled(base, p);
            assert(std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.z));
            assert(std::fabs(Len(w) - 1.0f) < 2e-6f);
            const float want = std::fmin(degrees * p / 100.0f, std::fmax(degrees, 85.0f));
            assert(std::fabs(LeanOf(w) - (p == 100 ? degrees : want)) < 0.02f);
            // The direction of the lean is kept (the heading around the Y axis).
            if (degrees < 85) {
                const float h = std::atan2(w.z, w.x), h0 = std::atan2(base.z, base.x);
                assert(std::fabs(std::remainder(h - h0, 6.2831853f)) < 1e-3f);
            }
            // Monotonic: a higher percentage never leans less.
            assert(LeanOf(w) >= last - 1e-3f);
            last = LeanOf(w);
        }
    }
    // The stops: 200% of 60 degrees is 85, not 120; a lean already past 85 is not pulled back by 150%, and 50% still halves it.
    assert(std::fabs(LeanOf(Scaled(Lean(60, 10), 200)) - 85.0f) < 0.02f);
    assert(std::fabs(LeanOf(Scaled(Lean(100, 10), 150)) - 100.0f) < 0.02f && std::fabs(LeanOf(Scaled(Lean(100, 10), 50)) - 50.0f) < 0.02f);
    assert(std::fabs(LeanOf(Scaled(Lean(20, 0), 200)) - 40.0f) < 0.02f && std::fabs(LeanOf(Scaled(Lean(20, 0), 50)) - 10.0f) < 0.02f);
    // Tiny leans (the noise around level) and a lean of exactly 180 degrees stay finite.
    for (float tiny : {1e-9f, 1e-7f, 1e-5f, 1e-3f}) for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) {
        const V w = Scaled({tiny, -1, tiny}, p); assert(std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.z) && std::fabs(Len(w) - 1.0f) < 1e-5f);
    }
    for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) { const V w = Scaled({1e-3f, 1, 0}, p); assert(std::isfinite(w.x) && std::isfinite(w.y) && std::fabs(Len(w) - 1.0f) < 1e-5f); }

    // The stick path: the full stick is a 40.5 degree lean (0.65 of the unit circle); sensitivity scales that the same way, straight
    // left stays straight left, the centre stays level, and the corner of the stick square (|x|,|y| = 1) stays finite and unit.
    for (int lv = 1; lv <= kLevels; ++lv) for (int p = PercentOfLevel(lv), once = 1; once; once = 0) {
        const V full = Scaled(Stick(1, 0), p);
        const float want = std::fmin(std::asin(0.65f) * 180.0f / 3.14159265f * p / 100.0f, 85.0f);
        assert(std::fabs(LeanOf(full) - want) < 0.02f && std::fabs(full.z) < 1e-6f && full.x > 0);
        const V up = Scaled(Stick(0, -1), p); assert(up.z < 0 && std::fabs(up.x) < 1e-6f && std::fabs(LeanOf(up) - want) < 0.02f);
        const V centre = Scaled(Stick(0, 0), p); assert(centre.x == 0 && centre.y == -1 && centre.z == 0);
        for (float sx : {-1.0f, 1.0f}) for (float sy : {-1.0f, 1.0f}) {
            const V c = Scaled(Stick(sx, sy), p); assert(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::fabs(Len(c) - 1.0f) < 2e-6f);
        }
        float last = -1;
        for (float m = 0; m <= 1.0f; m += 0.05f) { const float lean = LeanOf(Scaled(Stick(m, 0), p)); assert(lean >= last - 1e-3f); last = lean; }
    }
    // 100% on the stick path is exactly the vector the stick path always made.
    for (float sx = -1; sx <= 1; sx += 0.1f) for (float sy = -1; sy <= 1; sy += 0.1f) {
        const V v = Stick(sx, sy), w = Scaled(v, 100); assert(std::memcmp(&v, &w, sizeof v) == 0);
    }
    std::puts("tilt_gain: percentages, bit-exact 100%, angle scaling, stops, unit length, no NaN and the stick path passed");
}
