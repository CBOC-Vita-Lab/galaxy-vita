// GXV_PAD_LEAN (thr24): the real vita/runtime/input.cpp against stub system calls. The same scripted input is polled with the switch off (run 0) and on
// (run 7); every PortPadState must be identical (outside a tilt level the tilt fields are not filled with the switch on: the game never reads them there),
// the system calls must drop (port query once per kPortCachePolls polls, no rear pad read while rear aiming is off, no motion read outside a tilt level),
// and another controller that appears is seen within one cache lifetime (80 polls).
// Usage: pad_lean_test <remap 0|1>; the runner (tools/test_pad_lean.sh) sets GXV_PAD_LEAN in the environment.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>

#include "../vita/runtime/input.h"

static SceCtrlData gCtrl;
static SceTouchData gFront, gRear;
static int gRearAim = 0, gTilt = 0, gPortType0 = SCE_CTRL_TYPE_PHY, gPeekFail = 0;
static unsigned cPort, cRear, cMotion, cFront, cPeek;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* d, int) { *d = gCtrl; return 1; }
int sceCtrlPeekBufferPositive2(int, SceCtrlData* d, int) { ++cPeek; if (gPeekFail) return 0; *d = gCtrl; return 1; }
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* info) { ++cPort; *info = {}; info->port[0] = gPortType0; if (gPortType0 != SCE_CTRL_TYPE_PHY) info->port[1] = SCE_CTRL_TYPE_DS4; return 0; }
int sceMotionStartSampling(void) { return 0; }
int sceMotionStopSampling(void) { return 0; }
int sceMotionGetState(SceMotionState* s) { ++cMotion; memset(s, 0, sizeof(*s)); s->acceleration.x = 0.1f; s->acceleration.y = -0.9f; s->acceleration.z = 0.2f; return 0; }
int sceMotionGetSensorState(SceMotionSensorState* s, int) { memset(s, 0, sizeof(*s)); s->accelerometer.y = -1.0f; return 0; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* p) {
    memset(p, 0, sizeof(*p)); p->maxDispX = 1919; p->maxDispY = port == SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* d, SceUInt32) { if (port == SCE_TOUCH_PORT_FRONT) { ++cFront; *d = gFront; } else { ++cRear; *d = gRear; } return 1; }
int port_input_tilt_active(void) { return gTilt; }
int port_pointer_on_menus(void) { return 0; }
int port_zoom_menu_active(void) { return 0; }
int port_zoom_adjust(int) { return 15; }
int port_rear_aim_enabled(void) { return gRearAim; }
int port_rear_aim_toggle(void) { return gRearAim; }
int port_stick_pointer_enabled(void) { return 0; }
int port_stick_pointer_toggle(void) { return 0; }
int port_star_bit_reach_next(void) { return 1; }
int port_tap_jump_enabled(void) { return 1; }
int port_settings_hint_visible(void) { return 0; }
void port_settings_open_request(void) {}
int port_skip_context_kind(void) { return 0; }
}

static uint32_t rng = 99;
static uint32_t Rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main() {
    assert(port_vita_input_init());
    const unsigned initPort = cPort;
    unsigned long long digest = 1469598103934665603ull, digestTiltBlind = 1469598103934665603ull;
    unsigned maxPortWithin = 0;
    for (int t = 0; t < 4000; ++t) {
        gCtrl = {};
        gCtrl.lx = 128 + (Rnd() % 60) - 30; gCtrl.ly = 128; gCtrl.rx = gCtrl.ry = 128;
        gCtrl.buttons = (t / 50) % 2 ? SCE_CTRL_CROSS : 0;
        gFront = {}; gRear = {};
        if ((t / 100) % 3 == 1) { gFront.reportNum = 1; gFront.report[0].x = 400 + t % 700; gFront.report[0].y = 500; }
        if ((t / 70) % 2 == 1) { gRear.reportNum = 2; gRear.report[0] = {}; gRear.report[0].id = 3; gRear.report[0].x = 300; gRear.report[0].y = 450; gRear.report[1].id = 5; gRear.report[1].x = 1700; gRear.report[1].y = 500; }
        gTilt = (t / 400) % 2;                // tilt levels in the second 400 of every 800 polls
        gRearAim = (t >= 3000) ? 1 : 0;       // rear aiming off, and on for the last 1000 polls
        PortPadState pad{};
        port_vita_input_poll(1.0f / 166.0f, &pad);
        // the whole state is digested; the second digest leaves the tilt fields out (they are only read in a tilt level)
        PortPadState blind = pad;
        if (!gTilt) { blind.tilted = 0; blind.downX = blind.downY = blind.downZ = 0; }
        for (const PortPadState* p : {&pad, &blind}) {
            unsigned long long& d = p == &pad ? digest : digestTiltBlind;
            const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
            for (size_t i = 0; i < sizeof(*p); ++i) d = (d ^ b[i]) * 1099511628211ull;
        }
        (void)maxPortWithin;
    }
    // another controller appears: the port query is asked again within one cache lifetime
    gPortType0 = SCE_CTRL_TYPE_DS4; gPeekFail = 0;
    const unsigned before = cPort;
    PortPadState pad{};
    for (int i = 0; i < 100; ++i) port_vita_input_poll(0.006f, &pad);   // (one cache lifetime and some)
    const unsigned lateSeen = cPort - before;
    printf("digest %016llx blind %016llx port-queries %u rear-reads %u motion-reads %u front-reads %u after-switch-queries %u\n", digest, digestTiltBlind, initPort ? cPort - 1 : cPort, cRear, cMotion, cFront, lateSeen);
    return 0;
}
