// Depth of field switch (Settings > Display > Depth of field, GXV_DOF; default on).
//
// Off skips DepthOfFieldBlur::draw entirely: its captures, blur passes and final composite are not issued, so the scene shows the
// picture without the blur. Nothing the blur does is read back by other code (the same early return is what isSomething() already
// gives when the effect is switched off by the game), and its state changes are all restored before it returns, so skipping it
// leaves the GX state as it was. On is bit-for-bit the draw that was there.
#pragma once

#include <cstdlib>

namespace port {
namespace dof {

// The switch's value: unset or empty is on; "0" (or any number equal to 0) is off; anything else is on.
inline bool ParseSwitch(const char* v) {
    if (!v || !*v) return true;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 0);
    return end == v || *end ? true : parsed != 0;
}

// GXV_DOF, read once. The Settings menu sets it at start-up (platform/src/settings/settings_launch.cpp); env.txt wins.
inline bool Enabled() {
    static const bool on = ParseSwitch(std::getenv("GXV_DOF"));
    return on;
}

}  // namespace dof
}  // namespace port
