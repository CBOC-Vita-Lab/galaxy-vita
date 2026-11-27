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

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace port {
namespace tilt {

constexpr int kMinPercent = 50, kMaxPercent = 200, kDefaultPercent = 100;
constexpr float kMaxAngle = 85.0f * 3.14159265358979f / 180.0f;

// The menu's scale: 1 (half the steering) to 10 (double), 5 the game's own feel (100 %).
constexpr int kLevels = 10, kDefaultLevel = 5;
inline bool ValidLevel(int level) { return level >= 1 && level <= kLevels; }
// The percentage of each level: 50, 63, 75, 88, 100, 120, 140, 160, 180, 200.
inline int PercentOfLevel(int level) {
    static constexpr int percent[kLevels] = {50, 63, 75, 88, 100, 120, 140, 160, 180, 200};
    return ValidLevel(level) ? percent[level - 1] : kDefaultPercent;
}
// The level nearest to a percentage (the 1.2 beta stored 50 to 200 in steps of 25 as motion_gain); a tie goes to the lower level.
inline int LevelOfPercent(int percent) {
    int best = kDefaultLevel, distance = 1 << 20;
    for (int level = 1; level <= kLevels; ++level) {
        const int d = std::abs(PercentOfLevel(level) - percent);
        if (d < distance) { distance = d; best = level; }
    }
    return best;
}

// The switch's value (a percentage, for env.txt): a whole number held to 50..200; unset, empty or anything else is 100.
inline int ParsePercent(const char* v) {
    if (!v || !*v) return kDefaultPercent;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == v || *end) return kDefaultPercent;
    if (parsed < kMinPercent) return kMinPercent;
    if (parsed > kMaxPercent) return kMaxPercent;
    return static_cast<int>(parsed);
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

// settingsnative: the two values are atomic cells the native Settings menu can change while the game runs (SetPercent / SetUseStick, relaxed
// stores). -1 = not read yet: the first read takes the start-up switch (a racing SetPercent wins over it).
inline std::atomic<int>& PercentCell() { static std::atomic<int> c{-1}; return c; }
inline std::atomic<int>& UseStickCell() { static std::atomic<int> c{-1}; return c; }
inline void SetPercent(int percent) {
    PercentCell().store(percent < kMinPercent ? kMinPercent : percent > kMaxPercent ? kMaxPercent : percent, std::memory_order_relaxed);
}
inline void SetUseStick(bool on) { UseStickCell().store(on ? 1 : 0, std::memory_order_relaxed); }

// GXV_TILT_GAIN, read once (until SetPercent). The Settings menu sets it at start-up (platform/src/settings/settings_launch.cpp); env.txt wins.
inline int Percent() {
    int v = PercentCell().load(std::memory_order_relaxed);
    if (v >= 0) return v;
    int expected = -1;
    PercentCell().compare_exchange_strong(expected, ParsePercent(std::getenv("GXV_TILT_GAIN")), std::memory_order_relaxed);
    return PercentCell().load(std::memory_order_relaxed);
}

// GXV_TILT_STICK=1, read once (until SetUseStick): tilt comes from the left stick even where the console has a motion sensor.
inline bool UseStick() {
    int v = UseStickCell().load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("GXV_TILT_STICK");
        int expected = -1;
        UseStickCell().compare_exchange_strong(expected, e && std::atoi(e) != 0 ? 1 : 0, std::memory_order_relaxed);
        v = UseStickCell().load(std::memory_order_relaxed);
    }
    return v != 0;
}

}  // namespace tilt
}  // namespace port
