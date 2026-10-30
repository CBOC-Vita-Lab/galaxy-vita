// Touch presses of A during dialogue: production vita/runtime/input.cpp and platform/src/input/wpad.cpp (what the game's KPAD
// reads), SDK calls faked.  GXV_TAP_CLEAN=1 makes a finger resting on the screen a single press of A while a dialogue or a
// cutscene is on screen; without it the finger holds A as long as it stays (the game finishes a page after 11 updates of
// held A, and the port skips the whole dialogue after one second).
#include "input.h"
#include "revolution/kpad.h"
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static SceCtrlData raw;
static SceTouchData front;
static KPADStatus kpad;
static int skipKind;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* out, int) { *out=raw; return 1; }
int sceCtrlPeekBufferPositive2(int, SceCtrlData* out, int) { *out=raw; return 1; }
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* out) { *out={}; out->port[0]=SCE_CTRL_TYPE_PHY; return 0; }
int sceMotionStartSampling() { return -1; }
int sceMotionStopSampling() { return 0; }
int sceMotionGetState(SceMotionState*) { return -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* out) {
    *out={}; out->maxDispX=1919; out->maxDispY=port==SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* out, SceUInt32) { *out=port==SCE_TOUCH_PORT_FRONT ? front : SceTouchData{}; return 1; }
int port_pointer_on_menus() { return 0; }
int port_zoom_menu_active() { return 0; }
int port_zoom_adjust(int) { return 15; }
int port_rear_aim_enabled() { return 0; }
int port_rear_aim_toggle() { return 0; }
int64_t port_host_time_ns() { return 1; }
int port_skip_inject_a() { return -1; }
int port_handheld() { return 1; }
unsigned long long port_gx_recording_frame() { return 1; }
void port_log(const char*, ...) {}
int port_skip_context_kind() { return skipKind; }
}
// One 60 Hz step; returns whether the game read A as held.
static bool step() {
    PortPadState pad;
    port_vita_input_poll(1.0f / 60, &pad);
    port_input_set(0, &pad);
    assert(KPADRead(0, &kpad, 1) == 1);
    return (kpad.hold & 0x800) != 0;
}
struct Run { int edges, held, longest; };
static void note(Run& r, bool a, bool& was, int& streak) {
    r.edges += a && !was; r.held += a; streak = a ? streak + 1 : 0; if (streak > r.longest) r.longest = streak; was = a;
}
// A finger on the screen for `frames` updates, then `after` updates without it.
static Run touch(int frames, int after) {
    Run r{}; bool was = false; int streak = 0;
    front.reportNum = 1; front.report[0].x = 960; front.report[0].y = 500;
    for (int i = 0; i < frames; ++i) note(r, step(), was, streak);
    front = {};
    for (int i = 0; i < after; ++i) note(r, step(), was, streak);
    return r;
}
static void settle() { front = {}; raw.buttons = 0; for (int i = 0; i < 30; ++i) step(); }

int main(int argc, char** argv) {
    assert(argc == 2);
    const bool clean = std::strcmp(argv[1], "on") == 0;
    setenv("GXV_REMAP", "1", 1);
    setenv("GXV_TAP_CLEAN", clean ? "1" : "0", 1);
    raw.lx = raw.ly = raw.rx = raw.ry = 128;
    assert(port_vita_input_init()); settle();

    // A finger resting for 1.5 s on a dialogue.
    skipKind = 5;
    Run rest = touch(90, 30);
    if (clean) assert(rest.edges == 1 && rest.longest <= 7 && rest.held <= 7);          // one press of 0.1 s
    else assert(rest.edges == 1 && rest.longest >= 70);                                  // as before: a held A
    settle();
    // The same in a cutscene (kind 4).
    skipKind = 4;
    rest = touch(90, 30);
    if (clean) assert(rest.edges == 1 && rest.held <= 7); else assert(rest.longest >= 70);
    settle();

    // A short tap is unchanged: one press of about 0.1 s after the finger lifts.
    skipKind = 5;
    Run tap = touch(5, 20);
    assert(tap.edges == 1 && tap.longest >= 5 && tap.longest <= 8);
    settle();

    // Two quick taps: with the switch they are two presses; without it the second tap joins the first press.
    {
        Run two{}; bool was = false; int streak = 0;
        for (int t = 0; t < 2; ++t) {
            front.reportNum = 1; front.report[0].x = 960; front.report[0].y = 500;
            for (int i = 0; i < 3; ++i) note(two, step(), was, streak);
            front = {};
            for (int i = 0; i < 2; ++i) note(two, step(), was, streak);
        }
        for (int i = 0; i < 20; ++i) note(two, step(), was, streak);
        if (clean) assert(two.edges == 2); else assert(two.edges == 1);
    }
    settle();

    // Outside a dialogue or cutscene the finger still holds A (Pull Stars), with or without the switch.
    skipKind = 0;
    rest = touch(90, 30);
    assert(rest.edges == 1 && rest.longest >= 70);
    settle();
    skipKind = 1;  // a movie: untouched
    rest = touch(90, 30);
    assert(rest.longest >= 70);
    settle();

    // Cross is the player's own press and is never changed.
    skipKind = 5;
    {
        Run r{}; bool was = false; int streak = 0;
        raw.buttons = SCE_CTRL_CROSS;
        for (int i = 0; i < 40; ++i) note(r, step(), was, streak);
        assert(r.edges == 1 && r.longest == 40);
    }
    port_vita_input_shutdown();
    std::printf("PASS: touch A in dialogue GXV_TAP_CLEAN=%s\n", argv[1]);
}
