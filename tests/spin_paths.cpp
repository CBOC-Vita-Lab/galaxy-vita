// Square (and R) must reach the game as a shake in every situation the player meets: production vita/runtime/input.cpp and
// platform/src/input/wpad.cpp (what KPADRead hands the game), SDK calls faked.  The game's own swing test
// (WPadHVSwing::updateSwing: distance between the current acceleration and the one 20 reads ago, at least 1) runs on the
// result.  Arguments: remap (0/1), the latch switch (0/1), and optionally GXV_SHAKE_PULSE (0/1).
#include "input.h"
#include "revolution/kpad.h"
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static SceCtrlData raw;
static SceTouchData front;
static KPADStatus kpad;
static int type = SCE_CTRL_TYPE_PHY, menus, zoom, tilt;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* out, int) { *out=raw; return 1; }
int sceCtrlPeekBufferPositive2(int, SceCtrlData* out, int) {
    *out=raw;
    if (type == SCE_CTRL_TYPE_PHY) {
        out->buttons &= ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
        if (raw.buttons & SCE_CTRL_LTRIGGER) out->buttons |= SCE_CTRL_L1;
        if (raw.buttons & SCE_CTRL_RTRIGGER) out->buttons |= SCE_CTRL_R1;
    }
    return 1;
}
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* out) {
    *out={}; out->port[0]=type == SCE_CTRL_TYPE_PHY ? type : SCE_CTRL_TYPE_VIRT;
    if (type != SCE_CTRL_TYPE_PHY) out->port[1]=type;
    return 0;
}
int sceMotionStartSampling() { return -1; }
int sceMotionStopSampling() { return 0; }
int sceMotionGetState(SceMotionState*) { return -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* out) {
    *out={}; out->maxDispX=1919; out->maxDispY=port==SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* out, SceUInt32) { *out=port==SCE_TOUCH_PORT_FRONT ? front : SceTouchData{}; return 1; }
int port_pointer_on_menus() { return menus; }
int port_zoom_menu_active() { return zoom; }
int port_zoom_adjust(int) { return 15; }
int port_rear_aim_enabled() { return 0; }
int port_rear_aim_toggle() { return 0; }
int port_star_bit_reach_next() { return 1; }
int port_stick_pointer_enabled() { return 0; }
int port_stick_pointer_toggle() { return 0; }
int64_t port_host_time_ns() { return 1; }
int port_skip_inject_a() { return -1; }
int port_handheld() { return 1; }
unsigned long long port_gx_recording_frame() { return 1; }
void port_log(const char*, ...) {}
}
static int ticks;
// One 60 Hz game update: poll the controller `polls` times (the poll thread runs faster than the game), then the game reads.
static void step(int polls = 4) {
    PortPadState pad;
    for (int i = 0; i < polls; ++i) {
        raw.timeStamp += 16667 / polls; ++ticks;
        port_vita_input_poll(1.0f / 60 / polls, &pad);
        port_input_set(0, &pad);
    }
    assert(KPADRead(0, &kpad, 1) == 1);
}
// The game's detector over a history of reads.
struct Swing {
    std::vector<std::pair<float, float>> hist;  // (x, z) of acceleration the game derives: x = -acc.x, z = -acc.y
    bool swing = false;
    int edges = 0;
    void read() {
        hist.push_back({-kpad.acc.x, -kpad.acc.y});
        bool now = false;
        if (hist.size() > 20) {
            const auto& a = hist.back(); const auto& b = hist[hist.size() - 21];
            now = std::hypot(a.first - b.first, a.second - b.second) >= 1.0f;
        }
        edges += now && !swing;
        swing = now;
    }
};
static void rest(Swing& g, int n) { raw.buttons = 0; for (int i = 0; i < n; ++i) { step(); g.read(); } }
static int pressSquare(Swing& g, unsigned button, int frames, int polls = 4) {
    const int before = g.edges;
    raw.buttons = button;
    for (int i = 0; i < frames; ++i) { step(polls); g.read(); }
    raw.buttons = 0;
    return g.edges - before;
}

int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    const bool remap = std::atoi(argv[1]) != 0, latch = std::atoi(argv[2]) != 0, pulse = argc == 4 && std::atoi(argv[3]) != 0;
    setenv("GXV_SHAKE_PULSE", pulse ? "1" : "0", 1);
    setenv("GXV_REMAP", remap ? "1" : "0", 1);
    setenv("GXV_SHAKE_LATCH", latch ? "1" : "0", 1);
    raw.lx = raw.ly = raw.rx = raw.ry = 128;
    assert(port_vita_input_init());
    Swing g;
    rest(g, 40);

    for (unsigned button : {unsigned(SCE_CTRL_SQUARE), unsigned(SCE_CTRL_RTRIGGER)}) {
        const char* name = button == SCE_CTRL_SQUARE ? "Square" : "R";
        // plain play, a press of 5 and of 20 updates
        assert(pressSquare(g, button, 5) >= 1); rest(g, 40);
        assert(pressSquare(g, button, 20) >= 1); rest(g, 40);
        // on a 2D menu (pointer on menus)
        menus = 1; assert(pressSquare(g, button, 6) >= 1); menus = 0; rest(g, 40);
        // finger on the screen
        front.reportNum = 1; front.report[0].x = 960; front.report[0].y = 500;
        assert(pressSquare(g, button, 6) >= 1); front = {}; rest(g, 40);
        // tilt-steered level
        tilt = 1; assert(pressSquare(g, button, 6) >= 1); tilt = 0; rest(g, 40);
        // the pause menu / dome zoom owns L and R: Square always spins, R does not (by design)
        zoom = 1;
        const int inMenu = pressSquare(g, button, 6);
        if (button == SCE_CTRL_SQUARE) assert(inMenu >= 1); else assert(inMenu == 0);
        zoom = 0; rest(g, 40);
        std::printf("  %s: spin in play, menus, touch, tilt, zoom menu ok\n", name);
    }

    // Right after the settings screen: a press that comes after the return spins; one held across it waits for the release.
    raw.buttons = 0; step(); port_input_settings_rearm(); port_vita_input_settings_rearm(); step(); g.read();
    assert(pressSquare(g, SCE_CTRL_SQUARE, 6) >= 1); rest(g, 40);
    raw.buttons = SCE_CTRL_SQUARE; step(); port_input_settings_rearm(); port_vita_input_settings_rearm();
    step(); g.read();
    assert(kpad.acc.x == 0.0f && kpad.acc.y == -1.0f);   // held across: not delivered
    rest(g, 40);
    assert(pressSquare(g, SCE_CTRL_SQUARE, 6) >= 1); rest(g, 40);

    // A press shorter than one game update (the game's turn was long): lost without the latch, delivered with it.
    {
        PortPadState pad;
        raw.buttons = SCE_CTRL_SQUARE; raw.timeStamp += 16667;
        port_vita_input_poll(0.004f, &pad); port_input_set(0, &pad);
        raw.buttons = 0; raw.timeStamp += 1000;
        port_vita_input_poll(0.004f, &pad); port_input_set(0, &pad);
        assert(KPADRead(0, &kpad, 1) == 1);
        const bool seen = std::fabs(kpad.acc.x) > 1.0f;
        assert(seen == (latch || pulse));
        // once only: the latch reports the shake on one read; the pulse shaping goes on for its 20 reads
        assert(KPADRead(0, &kpad, 1) == 1 && (pulse ? std::fabs(kpad.acc.x) > 1.0f : kpad.acc.x == 0.0f));
    }
    port_vita_input_shutdown();
    std::printf("PASS: Square and R spin paths remap=%d latch=%d pulse=%d\n", remap, latch, pulse);
}
