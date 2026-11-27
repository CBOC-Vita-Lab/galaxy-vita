// settingsnative: the live setters of the native Settings menu that sit on a header's value (Less Motion, Tilt, Tilt Strength), and the
// "Settings can open here" answer for the pause panel's hint. The others sit next to their readers: perf.cpp (Camera Zoom, Right Stick, Rear
// Touch, Tap to Jump, Invert Camera, Star Bit Reach) and vita/runtime/input.cpp (Pointer).
//
// Every get returns the row's VALUE INDEX (port/settings_native.h NativeLiveOps) and every set applies it at once with a relaxed store: the
// readers (CameraContext, CenterScreenBlur, WipeFade; the input thread) pick it up on their next read. The player's file is the menu's business.
#include "port/reduced_motion.h"
#include "port/settings_native.h"
#include "port/tilt_gain.h"

#include <stdint.h>

extern "C" void port_handheld_pictures_changed(void) __attribute__((weak));   // platform/src/gx/vgx_galaxy.cpp: the button pictures are made again

extern "C" {
// Less Motion: 0 off, 1 on.
int port_less_motion_enabled(void) { return port::motion::Enabled() ? 1 : 0; }
int port_less_motion_set(int on) {
    port::motion::SetLive(on != 0);
    return on != 0;
}
// Tilt: 0 the console, 1 the left stick. Reaches the input thread's next poll and the on-screen prompts: the hold-the-controller pictures
// (handheld_ui.cpp) are made again, as for a button swap.
int port_tilt_stick_enabled(void) { return port::tilt::UseStick() ? 1 : 0; }
int port_tilt_stick_set(int stick) {
    const bool changed = port::tilt::UseStick() != (stick != 0);
    port::tilt::SetUseStick(stick != 0);
    if (changed && port_handheld_pictures_changed) port_handheld_pictures_changed();
    return stick != 0;
}
// Tilt Strength: the level 1..10 (5 the game's own), as the row's index + 1.
int port_tilt_level(void) { return port::tilt::LevelOfPercent(port::tilt::Percent()); }
int port_tilt_level_set(int level) {
    if (!port::tilt::ValidLevel(level)) return port_tilt_level();
    port::tilt::SetPercent(port::tilt::PercentOfLevel(level));
    return level;
}
// "Change Buttons" capture: the Vita's input poll (vita/runtime/input.cpp) has the strong one. Anywhere else the answer is all ones, "unsupported":
// the menu goes straight to its button list.
uint64_t __attribute__((weak)) port_vita_input_held_buttons(void) { return ~0ull; }
// The pause panel's "Settings" hint and Triangle: the platform has a native Settings (NativeHost::available).
int port_settings_native_available(void) {
    const port::settings::NativeHost* host = port::settings::NativeHostGet();
    return host && host->available ? 1 : 0;
}
}
