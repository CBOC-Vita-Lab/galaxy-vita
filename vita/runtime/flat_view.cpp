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
}
// (weak: the host tests link this file alone)
extern "C" uint64_t port_gx_recording_frame(void) __attribute__((weak));
extern "C" int port_zoom_menu_active(void) __attribute__((weak));
extern "C" void port_vr_wipe(int, float closed, unsigned int) {
    const uint64_t frame = port_gx_recording_frame ? port_gx_recording_frame() : 0;
    // (two in one frame, a fade over the cinema frame's black: the more closed one counts)
    if (frame == wipeFrame.load(std::memory_order_relaxed) && closed < wipeClosed.load(std::memory_order_relaxed)) return;
    wipeClosed.store(closed, std::memory_order_relaxed);
    wipeFrame.store(frame, std::memory_order_relaxed);
}
// 1 while the pause menu waits for a choice, or a fade / wipe drawn in the last 3 recorded frames has the screen 90 %
// closed or more. Any thread.
extern "C" int port_screen_quiet(void) {
    if (port_zoom_menu_active && port_zoom_menu_active()) return 1;
    if (!port_gx_recording_frame) return 0;
    const uint64_t now = port_gx_recording_frame(), at = wipeFrame.load(std::memory_order_relaxed);
    return at != 0 && now - at <= 3 && wipeClosed.load(std::memory_order_relaxed) >= 0.9f ? 1 : 0;
}
extern "C" void port_vr_pause_menu(int) {}
