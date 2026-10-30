// Reduced motion in play (Settings > Display > Reduced motion, GXV_REDUCED_MOTION=1; default off).
//
// The setting calms the picture, never the game: physics, timing, collision, camera pose and game logic run exactly as with the
// option off. Three things are scaled:
//
//  * Camera shake (CameraShaker -> CameraContext). The shake is a screen-space offset that the camera context folds into the
//    projection. With the option on the context folds in the offset times kShakeScale (25 %). The shake timers, their lengths and
//    their patterns are untouched. There is one projection for everything, so the picture, calcScreenPosition, the Star Pointer
//    hit-test, the HUD marks and the lens flare all agree: the pointer hits what is on screen.
//  * Radial zoom blur (CenterScreenBlur: explosions, the first-person camera): drawn at kBlurScale (50 %) of its offset and opacity;
//    its length and fades are untouched.
//  * Short full-screen flashes (WipeFade): a bright fade that lasts kShortFlashFrames or fewer is drawn at no more than
//    kFlashAlphaCap (40 %) opacity. Scene transitions and wipes to black are longer or dark and are never touched; every fade
//    the game starts today is 30 frames or more, so this is a guard for short ones and changes nothing that exists now.
//
// Off means identity: ShakeScale(v, false) returns v and CapFlashAlpha(a, ..., false) returns a, bit for bit.
#pragma once

#include <cstdlib>

namespace port {
namespace motion {

constexpr float kShakeScale = 0.25f;     // camera shake amplitude with the option on
constexpr float kBlurScale = 0.5f;       // CenterScreenBlur (radial zoom blur) strength with the option on
constexpr float kFlashAlphaCap = 0.40f;  // highest opacity (0..1) of a short bright full-screen flash with the option on
constexpr int kShortFlashFrames = 12;    // a fade of this many 60 Hz game frames or fewer counts as a flash (0.2 s)
constexpr int kBrightLuma = 160;         // fill colours at least this bright (0..255, Rec.601 weights) count as bright

// The switch's value: unset, empty, 0 or not a number is off; any other number is on.
inline bool ParseSwitch(const char* v) {
    if (!v || !*v) return false;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 0);
    return end == v || *end ? false : parsed != 0;
}

// GXV_REDUCED_MOTION, read once. The Settings menu sets it at start-up (platform/src/settings/settings_launch.cpp); env.txt wins.
inline bool Enabled() {
    static const bool on = ParseSwitch(std::getenv("GXV_REDUCED_MOTION"));
    return on;
}

// A shake offset component, scaled for the view.
inline float ShakeScale(float v, bool on) { return on ? v * kShakeScale : v; }
inline float ShakeScale(float v) { return ShakeScale(v, Enabled()); }

// A CenterScreenBlur strength (its zoom offset in pixels, or its opacity 0..255), scaled for the view.
inline float BlurScale(float v, bool on) { return on ? v * kBlurScale : v; }
inline float BlurScale(float v) { return BlurScale(v, Enabled()); }

inline int Luma(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

// True for a fade that is a flash: bright fill and short. `frames` is the fade length in game frames.
inline bool IsShortBrightFlash(int frames, int r, int g, int b) {
    return frames <= kShortFlashFrames && Luma(r, g, b) >= kBrightLuma;
}

// The opacity (0..1) a fade is drawn with. Unchanged unless the option is on and the fade is a short bright flash.
inline float CapFlashAlpha(float alpha, int frames, int r, int g, int b, bool on) {
    return on && IsShortBrightFlash(frames, r, g, b) && alpha > kFlashAlphaCap ? kFlashAlphaCap : alpha;
}
inline float CapFlashAlpha(float alpha, int frames, int r, int g, int b) { return CapFlashAlpha(alpha, frames, r, g, b, Enabled()); }

}  // namespace motion
}  // namespace port
