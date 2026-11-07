#pragma once
#include <cmath>

// Mouse-style Star Bit pointer aiming (Settings > Controls > Mouse-style aiming, GXV_MOUSE_AIM=1; default off).
//
// The 1.0 behaviour (vita/runtime/input.cpp, option off) moves the pointer at a speed that is the stick's deflection times a
// fixed 2 pointer units per second. This is the other way, like a mouse with pointer acceleration: the stick still sets a
// speed, but the speed follows a curve (a small push is a slow, exact move; a full push is a fast sweep) and grows while the
// stick is kept pushed. Only the pure arithmetic lives here so a host test can check it; input.cpp feeds it the raw stick.
//
// Units: the pointer is -1..1 across the picture on both axes. A pointer unit is 480 px across but 272 px down on the 960 x 544
// screen, so the vertical speed is scaled by the picture's width over its height ("aspect") to move the same number of pixels
// per second in every direction.
namespace port::aim {
constexpr float kDeadzone = 0.14f;     // radial, the same as the stick used everywhere else (vita/runtime/input.cpp stick())
constexpr float kSlow = 0.35f;         // pointer units per second per unit of deflection (the linear part)
constexpr float kFast = 1.90f;         // ... and the cubic part: a full push is kSlow + kFast = 2.25 units a second (1.0: 2.0)
constexpr float kRamp = 0.60f;         // extra gain after a long push: 1 .. 1 + kRamp
constexpr float kRampSeconds = 0.60f;  // ... reached after this long
constexpr float kMaxStep = 0.05f;      // longest read interval counted (the same cap as the 1.0 path)

struct State {
    float x = 0, y = 0;   // the pointer, -1..1
    float held = 0;       // seconds the stick has been pushed past the dead zone without a break
};

// The stick's deflection after the radial dead zone, 0..1 (0 inside the zone, rescaled so it starts from 0 at the edge) and
// the unit direction it points. sx, sy: -1..1, +y up (the Vita's y axis is flipped by the caller).
inline float Deflection(float sx, float sy, float& dirX, float& dirY) {
    dirX = dirY = 0;
    const float radius = std::sqrt(sx * sx + sy * sy);
    if (!std::isfinite(radius) || radius <= kDeadzone) return 0;
    dirX = sx / radius;
    dirY = sy / radius;
    const float m = (std::fmin(radius, 1.0f) - kDeadzone) / (1.0f - kDeadzone);
    return m;
}

// Pointer units per second at deflection m (0..1), without the hold ramp.
inline float Speed(float m) { return kSlow * m + kFast * m * m * m; }

// The integral of the hold gain 1 + kRamp * min(t / kRampSeconds, 1) over [t0, t0 + dt] (t0 >= 0, dt >= 0): exact, so the
// distance moved in a time does not depend on how that time was cut into reads.
inline float GainIntegral(float t0, float dt) {
    const float t1 = t0 + dt;
    const float rampEnd = kRampSeconds;
    auto F = [&](float t) {  // antiderivative
        if (t <= rampEnd) return t + kRamp * t * t / (2 * rampEnd);
        return t + kRamp * (rampEnd / 2 + (t - rampEnd));
    };
    return F(t1) - F(t0);
}

// One read: moves the pointer by the stick for dt seconds. Returns true if the stick was past the dead zone (the pointer
// is "in use": the caller keeps it on screen). A stick held at one place gives the same position at 30 and 60 reads a second
// (the stick is taken as constant over a read, the usual zero-order hold; the ramp and the speed are integrated exactly).
inline bool Step(State& s, float sx, float sy, float dt, float aspect) {
    if (!std::isfinite(dt) || dt < 0) dt = 0;
    if (dt > kMaxStep) dt = kMaxStep;
    if (!(aspect > 0) || !std::isfinite(aspect)) aspect = 1;
    float dx, dy;
    const float m = Deflection(sx, sy, dx, dy);
    if (m <= 0) { s.held = 0; return false; }
    const float distance = Speed(m) * GainIntegral(s.held, dt);
    s.held += dt;
    s.x += dx * distance;
    s.y -= dy * distance * aspect;  // the screen's y grows downwards
    s.x = std::fmin(1.0f, std::fmax(-1.0f, s.x));
    s.y = std::fmin(1.0f, std::fmax(-1.0f, s.y));
    return true;
}
}  // namespace port::aim
