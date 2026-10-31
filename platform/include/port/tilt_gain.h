// Tilt sensitivity and tilt source (Settings > Controls > Tilt sensitivity, Tilt with; GXV_TILT_GAIN, GXV_TILT_STICK).
//
// The levels that steer by tilt (Star Ball, ray surfing) read a "which way is down" vector (port_input_use_tilt; PortPadState
// downX/downY/downZ): unit length, straight down the Y axis when the console is held at the calibrated grip, tipping toward
// +X/+Z as it leans. Sensitivity scales the angle of that lean, keeping its direction and the unit length: at 200% a 20 degree
// lean steers like 40 degrees, at 50% like 10. 100% returns before touching a value, so it is today's behaviour bit for bit.
// Beyond the console's own lean the scaled angle stops at 85 degrees (a ball or a ray never needs more, and a vector lying flat
// would make the level's neutral-pitch turn in wpad.cpp meaningless); a lean already past that is never reduced by the clamp, so a
// higher percentage never steers less than a lower one.
#pragma once

#include <cmath>
#include <cstdlib>

namespace port {
namespace tilt {

constexpr int kMinPercent = 50, kMaxPercent = 200, kStepPercent = 25, kDefaultPercent = 100;
constexpr float kMaxAngle = 85.0f * 3.14159265358979f / 180.0f;

// A percentage the menu offers: 50, 75, 100, ... 200.
inline bool ValidPercent(int percent) {
    return percent >= kMinPercent && percent <= kMaxPercent && (percent - kMinPercent) % kStepPercent == 0;
}

// The switch's value: a whole number, held to the range and rounded down to a step; unset, empty or anything else is 100.
inline int ParsePercent(const char* v) {
    if (!v || !*v) return kDefaultPercent;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == v || *end) return kDefaultPercent;
    if (parsed < kMinPercent) return kMinPercent;
    if (parsed > kMaxPercent) return kMaxPercent;
    return kMinPercent + static_cast<int>((parsed - kMinPercent) / kStepPercent) * kStepPercent;
}

// Scale the lean of the down vector (x, y, z) by `percent`. y is -cos(lean), so a level console is (0, -1, 0). A vector that is
// not finite, or has no lean to scale, is left alone.
inline void Scale(float& x, float& y, float& z, int percent) {
    if (percent == kDefaultPercent) return;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    const float flat = std::sqrt(x * x + z * z);
    if (flat < 1e-6f) return;
    const float lean = std::atan2(flat, -y);                       // 0 level .. pi upside down
    const float limit = lean > kMaxAngle ? lean : kMaxAngle;
    float scaled = lean * (static_cast<float>(percent) / 100.0f);
    if (scaled > limit) scaled = limit;
    const float side = std::sin(scaled) / flat;
    x *= side; z *= side;
    y = -std::cos(scaled);
}

// GXV_TILT_GAIN, read once. The Settings menu sets it at start-up (platform/src/settings/settings_launch.cpp); env.txt wins.
inline int Percent() {
    static const int percent = ParsePercent(std::getenv("GXV_TILT_GAIN"));
    return percent;
}

// GXV_TILT_STICK=1, read once: tilt comes from the left stick even where the console has a motion sensor.
inline bool UseStick() {
    static const bool on = [] { const char* e = std::getenv("GXV_TILT_STICK"); return e && std::atoi(e) != 0; }();
    return on;
}

}  // namespace tilt
}  // namespace port
