// Rear touch aiming (vita/runtime/input.cpp), on the host with the Vita SDK's headers and stub system calls.
// Build: tools/test_vita_rear_touch.sh
#ifdef NDEBUG
#error "needs assert()"
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>

#include "../vita/runtime/input.h"

// ---- the system, as the input code sees it
static SceCtrlData gCtrl;
static SceTouchData gFront, gRear;
static int gRearAim = 1, gZoomMenu = 0, gMenus = 0, gToggles = 0;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* d, int) { *d = gCtrl; return 1; }
int sceMotionStartSampling(void) { return -1; }
int sceMotionStopSampling(void) { return 0; }
int sceMotionGetState(SceMotionState*) { return -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* p) {
    std::memset(p, 0, sizeof(*p));
    p->minDispX = 0; p->minDispY = 0;
    p->maxDispX = 1919; p->maxDispY = port == SCE_TOUCH_PORT_FRONT ? 1087 : 889;
    return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* d, SceUInt32) { *d = port == SCE_TOUCH_PORT_FRONT ? gFront : gRear; return 1; }
int port_input_tilt_active(void) { return 0; }
int port_pointer_on_menus(void) { return gMenus; }
int port_zoom_menu_active(void) { return gZoomMenu; }
int port_zoom_adjust(int) { return 15; }
int port_rear_aim_enabled(void) { return gRearAim; }
int port_rear_aim_toggle(void) { ++gToggles; gRearAim = !gRearAim; return gRearAim; }
}

struct Contact { int id; float x, y; };
static PortPadState poll(std::vector<Contact> rear, std::vector<Contact> front = {}, unsigned buttons = 0) {
    gCtrl = {};
    gCtrl.buttons = buttons;
    gCtrl.lx = gCtrl.ly = gCtrl.rx = gCtrl.ry = 128;
    gRear = {}; gFront = {};
    gRear.reportNum = static_cast<SceUInt8>(rear.size());
    for (size_t i = 0; i < rear.size(); i++) { gRear.report[i].id = rear[i].id; gRear.report[i].x = rear[i].x; gRear.report[i].y = rear[i].y; }
    gFront.reportNum = static_cast<SceUInt8>(front.size());
    for (size_t i = 0; i < front.size(); i++) { gFront.report[i].id = front[i].id; gFront.report[i].x = front[i].x; gFront.report[i].y = front[i].y; }
    PortPadState pad{};
    port_vita_input_poll(1.0f / 60.0f, &pad);
    return pad;
}

static unsigned rngState = 12345;
static float noise(float amp) { rngState = rngState * 1664525u + 1013904223u; return ((rngState >> 8) / float(1u << 24) - 0.5f) * 2 * amp; }

int main() {
    assert(port_vita_input_init());

    // 1. Two fingers resting on the back (holding the console), reported in changing order, with the pad's noise:
    //    the pointer never appears and never moves.
    for (int t = 0; t < 600; t++) {
        Contact a{3, 300 + noise(6), 450 + noise(6)}, b{7, 1650 + noise(6), 500 + noise(6)};
        PortPadState pad = t % 2 ? poll({a, b}) : poll({b, a});
        assert(!pad.pointerValid);
    }

    // 2. A third finger slides to the right: it takes the pointer, which moves right smoothly; the resting fingers
    //    (still reported, in changing order) do not pull it about.
    float lastX = -2, lastY = 0;
    int moved = 0;
    for (int t = 0; t < 120; t++) {
        Contact a{3, 300 + noise(6), 450 + noise(6)}, b{7, 1650 + noise(6), 500 + noise(6)};
        Contact c{9, 900 + t * 4.0f + noise(1.0f), 400 + noise(1.0f)};
        PortPadState pad = t % 3 == 0 ? poll({a, c, b}) : t % 3 == 1 ? poll({c, b, a}) : poll({b, a, c});
        if (!pad.pointerValid) { assert(t < 12); continue; }
        assert(pad.pointerX >= lastX - 1e-6f);                   // never backwards
        if (lastX > -2) assert(pad.pointerX - lastX < 0.03f);    // never a jump
        if (lastX > -2) assert(std::fabs(pad.pointerY - lastY) < 0.01f);
        if (lastX > -2 && pad.pointerX > lastX) ++moved;
        lastX = pad.pointerX; lastY = pad.pointerY;
        assert((pad.buttons & 0x0C00) == 0);  // no A, no B from the rear
    }
    assert(moved > 90 && lastX > 0.4f && lastX < 0.95f);

    // 2b. A slow, careful slide (0.6 panel units a poll, under the pad's noise step) still moves the pointer, steadily.
    {
        for (int t = 0; t < 200; t++) poll({{3, 300, 450}});  // (the pointer of step 2 goes away: the slide starts in the middle)
        float x0 = -2, xs = 0;
        for (int t = 0; t < 400; t++) {
            PortPadState pad = poll({{3, 300, 450}, {9, 900 + t * 0.6f + (t < 80 ? t * 0.6f : 48.0f), 400}});
            if (!pad.pointerValid) continue;
            if (x0 < -1) x0 = pad.pointerX;
            assert(pad.pointerX >= xs - 1e-6f || xs == 0);
            xs = pad.pointerX;
        }
        // (after arming, 320 polls x 0.6 units = 192 units: 0.35 of the pointer's range at 2 / 1100 a unit)
        assert(xs - x0 > 0.25f);
        poll({});
        lastX = poll({}).pointerX;
        lastY = poll({}).pointerY;
    }

    // 3. The sliding finger lifts and lands again elsewhere: the pointer stays where it was (no jump to the finger).
    const float keptX = lastX, keptY = lastY;
    for (int t = 0; t < 10; t++) {
        PortPadState pad = poll({{3, 300, 450}, {11, 200, 800}});
        assert(std::fabs(pad.pointerX - keptX) < 1e-6f && std::fabs(pad.pointerY - keptY) < 1e-6f);
    }

    // 4. A finger on the front screen wins: the pointer goes where it touches, the rear slide is ignored.
    for (int t = 0; t < 10; t++) {
        PortPadState pad = poll({{3, 300, 450}, {12, 900 + t * 30.0f, 400}}, {{1, 960, 544}});
        assert(std::fabs(pad.pointerX) < 0.01f && std::fabs(pad.pointerY) < 0.01f);
    }
    poll({});

    // 5. Option off: a sliding rear finger does nothing.
    gRearAim = 0;
    const float offX = poll({}).pointerX;
    for (int t = 0; t < 200; t++) {
        PortPadState pad = poll({{14, 200 + t * 6.0f, 400}});
        assert(pad.pointerX == offX);
    }

    // 6. Triangle in the pause menu toggles it once per press, and the game does not see Triangle there.
    gZoomMenu = 1;
    poll({});
    PortPadState pad = poll({}, {}, SCE_CTRL_TRIANGLE);
    assert(gToggles == 1 && gRearAim == 1 && (pad.buttons & 0x4000) == 0);
    for (int t = 0; t < 30; t++) poll({}, {}, SCE_CTRL_TRIANGLE);
    assert(gToggles == 1);
    poll({});
    poll({}, {}, SCE_CTRL_TRIANGLE);
    assert(gToggles == 2 && gRearAim == 0);
    gZoomMenu = 0;
    pad = poll({}, {}, SCE_CTRL_TRIANGLE);
    assert(gToggles == 2 && (pad.buttons & 0x4000));  // in play Triangle is C again

    // 7. On a 2D menu the rear pad does not move the pointer.
    gRearAim = 1;
    gMenus = 1;
    poll({});
    pad = poll({});
    const float menuX = pad.pointerX;
    for (int t = 0; t < 100; t++) pad = poll({{15, 200 + t * 8.0f, 400}});
    assert(std::fabs(pad.pointerX - menuX) < 1e-6f);

    std::puts("PASS: rear touch aiming (resting fingers ignored, tracked contact, no jumps, front wins, option and toggle)");
    return 0;
}
