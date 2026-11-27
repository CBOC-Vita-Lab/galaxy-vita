#include <atomic>
#include <cstdint>
#include "port/compat.h"

namespace {
std::atomic<const void*> transitHost{nullptr};
}

// No headset view exists on the handheld. Returning unavailable leaves the
// original game's camera, movement, cursor hit tests and screen effects active.
extern "C" int port_vr_cull_view(float*, float*, float*) { return 0; }
extern "C" int port_vr_move_axes(float*, float*, float*) { return 0; }
extern "C" int port_vr_pointer_in_world(void) { return 0; }
extern "C" int port_vr_pointer_ray(float*, float*) { return 0; }
extern "C" int port_vr_diorama(void) { return 0; }

extern "C" void port_vr_transit_begin(const void* host) { transitHost.store(host); }
extern "C" void port_vr_transit_end(const void* host) {
    const void* expected = host;
    transitHost.compare_exchange_strong(expected, nullptr);
}
extern "C" const void* port_vr_transit_host(void) { return transitHost.load(); }

// These notifications only drive the upstream headset overlay. The original
// game owns the handheld's cursor, wipes and pause menu, not a second VR layer.
extern "C" void port_vr_pointer_reach(float) {}
extern "C" void port_vr_pointer_depth(float) {}
extern "C" void port_vr_pointer_touched(unsigned long long) {}
// The game's fades and wipes report how far they have closed the screen (WipeFade, WipeRing, CinemaFrame). The handheld
// keeps the latest for port_screen_quiet: the renderer's automatic quality takes its rare steps up while the screen is
// closed or the pause menu is up, where a frozen picture is not seen (platform/vgx/src/vgx_autoq.h, calm mode).
namespace {
std::atomic<float> wipeClosed{0.0f};
std::atomic<uint64_t> wipeFrame{0};
std::atomic<uint64_t> wipeMoveFrame{0}; // (hitch33) the last recorded frame in which the wipe's closed fraction differed from the frame before
}
// (weak: the host tests link this file alone)
extern "C" uint64_t port_gx_recording_frame(void) __attribute__((weak));
extern "C" int port_zoom_menu_active(void) __attribute__((weak));
extern "C" void port_vr_wipe(int, float closed, unsigned int) {
    const uint64_t frame = port_gx_recording_frame ? port_gx_recording_frame() : 0;
    // (two in one frame, a fade over the cinema frame's black: the more closed one counts)
    if (frame == wipeFrame.load(std::memory_order_relaxed) && closed < wipeClosed.load(std::memory_order_relaxed)) return;
    // (hitch33, GXV_WIPE_GUARD) is the wipe MOVING? The frame's final value against the last reporting frame's final value: a wipe or fade that is closing or opening is
    // watched by the player even when it is 90 % closed, and work done in such a frame makes the wipe itself judder. Game thread only writes.
    static float prevFinal = 0.0f;
    static uint64_t savedMove = 0;
    const uint64_t last = wipeFrame.load(std::memory_order_relaxed);
    if (frame != last) {
        // (no report for a while: the screen was open. hitch33 6a: unless the wipe comes back fully closed: a scene that starts behind its closed wipe draws no wipe for a
        // few frames, and the first report after that gap is a black screen, not a movement; console job 2650 counted the stage's first picture, made behind the held wipe, as
        // a moving-wipe frame for it)
        prevFinal = (last != 0 && frame - last <= 3) ? wipeClosed.load(std::memory_order_relaxed) : (closed >= 0.999f ? closed : 0.0f);
        savedMove = wipeMoveFrame.load(std::memory_order_relaxed);
    }
    wipeClosed.store(closed, std::memory_order_relaxed);
    wipeFrame.store(frame, std::memory_order_relaxed);
    const float d = closed - prevFinal;
    wipeMoveFrame.store((d > 0.002f || d < -0.002f) ? frame : savedMove, std::memory_order_relaxed);
}
// 1 while a wipe or fade changed its closed fraction within the last 2 recorded frames (closing or opening); 0 when it stands still (fully closed, held) or none is drawn.
extern "C" int port_screen_wipe_moving(void) {
    if (!port_gx_recording_frame) return 0;
    const uint64_t now = port_gx_recording_frame(), at = wipeMoveFrame.load(std::memory_order_relaxed);
    return at != 0 && now - at <= 2 ? 1 : 0;
}
// 1 while the pause menu waits for a choice, 2 while a fade / wipe drawn in the last 3 recorded frames has the screen 90 %
// closed or more (the automatic quality's GXV_AUTOQ_WIPE tells the two apart: after a wipe comes another area, after the
// pause menu the same one; every other caller only tests for non-zero). Any thread.
extern "C" int port_screen_quiet(void) {
    if (port_zoom_menu_active && port_zoom_menu_active()) return 1;
    if (!port_gx_recording_frame) return 0;
    const uint64_t now = port_gx_recording_frame(), at = wipeFrame.load(std::memory_order_relaxed);
    return at != 0 && now - at <= 3 && wipeClosed.load(std::memory_order_relaxed) >= 0.9f ? 2 : 0;
}
extern "C" void port_vr_pause_menu(int) {}
