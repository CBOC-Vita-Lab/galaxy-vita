#include "input.h"
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace port::controls;
static SceCtrlData raw;
static SceTouchData front, rear;
static int type = SCE_CTRL_TYPE_PHY, readResult = 1, menus = 0, zoom = 0, tilt = 0;
static int reads = 0, zoomSteps = 0, toggles = 0;
static bool motionOk = true;
static SceMotionState motionState;
extern "C" {
int oracle_init(); void oracle_poll(float, PortPadState*); void oracle_shutdown();
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData* out, int) { assert(port == 0); ++reads; *out = raw; return readResult; }
int sceCtrlPeekBufferPositive2(int port, SceCtrlData* out, int) {
    assert(port == (type == SCE_CTRL_TYPE_PHY ? 0 : 1));
    ++reads; *out = raw;
    if (type == SCE_CTRL_TYPE_PHY) {
        out->buttons &= ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
        if (raw.buttons & SCE_CTRL_LTRIGGER) out->buttons |= SCE_CTRL_L1;
        if (raw.buttons & SCE_CTRL_RTRIGGER) out->buttons |= SCE_CTRL_R1;
    }
    return readResult;
}
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* info) {
    *info = {}; info->port[0] = type == SCE_CTRL_TYPE_PHY ? type : SCE_CTRL_TYPE_VIRT;
    if (type != SCE_CTRL_TYPE_PHY) info->port[1] = type;
    return 0;
}
int sceMotionStartSampling() { return motionOk ? 0 : -1; }
int sceMotionStopSampling() { return 0; }
int sceMotionGetState(SceMotionState* out) { *out = motionState; return motionOk ? 0 : -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* out) {
    *out = {}; out->maxDispX = 1919; out->maxDispY = port == SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData* out, SceUInt32) { *out = port == SCE_TOUCH_PORT_FRONT ? front : rear; return 1; }
int port_input_tilt_active() { return tilt; }
int port_pointer_on_menus() { return menus; }
int port_zoom_menu_active() { return zoom; }
int port_zoom_adjust(int n) { zoomSteps += n; return 15; }
int port_rear_aim_enabled() { return 1; }
int port_rear_aim_toggle() { return ++toggles; }
}
static PortPadState poll(float dt = 1.0f / 60) { PortPadState p; port_vita_input_poll(dt, &p); return p; }
static void neutral() { raw = {}; raw.lx = raw.ly = raw.rx = raw.ry = 128; front = rear = {}; }
static void assertSame(const PortPadState& a, const PortPadState& b) {
    assert(a.connected == b.connected && a.buttons == b.buttons && a.tilted == b.tilted && a.pointerValid == b.pointerValid && a.touched == b.touched);
    assert(a.stickX == b.stickX && a.stickY == b.stickY);
    assert(a.accX == b.accX && a.accY == b.accY && a.accZ == b.accZ);
    assert(a.nunAccX == b.nunAccX && a.nunAccY == b.nunAccY && a.nunAccZ == b.nunAccZ);
    assert(a.downX == b.downX && a.downY == b.downY && a.downZ == b.downZ);
    assert(a.pointerX == b.pointerX && a.pointerY == b.pointerY && a.pointerDist == b.pointerDist);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool off = std::strcmp(argv[1], "off") == 0;
    const bool defaults = std::strcmp(argv[1], "default") == 0;
    setenv("GXV_REMAP", off ? "0" : "1", 1);
    neutral(); motionState.acceleration.y = -1;
    if (std::strcmp(argv[1], "escape") == 0) {
        // Jump, Star Bits and Pause moved onto the zoom/rear-aim buttons must still reach the game in
        // the pause menu (confirm, back, unpause); an unbound zoom button still does not.
        auto map = GetControlApi().defaults(ProfileId::Handheld);
        std::string error, conflict;
        assert(GetControlApi().bind(map,"jump",0,"r1",true,NominalCaps(ProfileId::Handheld),conflict) == BindResult::Applied);
        assert(GetControlApi().bind(map,"star_bits",0,"l1",true,NominalCaps(ProfileId::Handheld),conflict) == BindResult::Applied);
        assert(GetControlApi().bind(map,"pause",0,"triangle",true,NominalCaps(ProfileId::Handheld),conflict) == BindResult::Applied);
        assert(SetActiveMapping(map,ProfileId::Handheld,NominalCaps(ProfileId::Handheld),error));
        assert(port_vita_input_init()); zoom = 1; poll();
        raw.buttons=SCE_CTRL_RTRIGGER; assert(poll().buttons & 0x800); neutral(); poll();
        raw.buttons=SCE_CTRL_LTRIGGER; assert(poll().buttons & 0x400); neutral(); poll();
        raw.buttons=SCE_CTRL_TRIANGLE; assert(poll().buttons & 0x0010); neutral(); poll();
        zoom = 0;
        std::puts("PASS: remapped confirm, back and pause pass through the pause menu");
        return 0;
    }
    if (std::strcmp(argv[1], "calibration") == 0) {
        auto map = GetControlApi().defaults(ProfileId::Handheld);
        std::string error, conflict;
        assert(GetControlApi().bind(map,"minus",0,"triangle",true,NominalCaps(ProfileId::Handheld),conflict) == BindResult::Applied);
        assert(SetActiveMapping(map,ProfileId::Handheld,NominalCaps(ProfileId::Handheld),error));
        assert(port_vita_input_init()); tilt=1; poll();
        motionState.acceleration.x=.6f; motionState.acceleration.y=-.8f;
        assert(std::fabs(poll().downX-.6f)<1e-5f);
        raw.buttons=SCE_CTRL_SELECT; assert(std::fabs(poll().downX-.6f)<1e-5f);
        raw.buttons=SCE_CTRL_TRIANGLE; const auto calibrated=poll();
        assert(std::fabs(calibrated.downX)<1e-5f && (calibrated.buttons&0x1000));
        // A hotplug to a source the saved profile cannot serve (no touch/motion) is reported as
        // unavailable, but the game keeps the connected controller's default buttons instead of
        // losing all input. Fixed settings still works.
        type=SCE_CTRL_TYPE_DS4; neutral(); poll(); raw.buttons=SCE_CTRL_CROSS;
        assert((poll().buttons & 0x800) && !port_vita_input_mapping_available());
        PortVitaRawInput ui; assert(port_vita_input_raw_poll(ui) && ui.held==Cross);
        std::puts("PASS: remapped Minus calibrates; old physical Select does not");
        return 0;
    }
    if (off || defaults) {
        assert(port_vita_input_init() && oracle_init());
        if (defaults) { poll(); PortPadState ignored; oracle_poll(1.0f/60, &ignored); }
        unsigned seed = 77;
        const unsigned bits[] = {SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER,
            SCE_CTRL_RTRIGGER, SCE_CTRL_START, SCE_CTRL_SELECT, SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT};
        for (int i = 0; i < 3000; ++i) {
            seed = seed * 1664525 + 1013904223; raw.buttons = 0;
            for (unsigned b = 0; b < 12; ++b) if (seed & (1u << b)) raw.buttons |= bits[b];
            raw.lx = seed >> 24; raw.ly = seed >> 16; raw.rx = seed >> 8; raw.ry = seed;
            raw.timeStamp += 16667; menus = i % 100 < 30; zoom = i % 130 < 40; tilt = i % 90 < 50;
            front = rear = {};
            if (i % 60 < 22) { front.reportNum = 1; front.report[0].x = 600 + i % 10; front.report[0].y = 500; }
            if (i % 90 < 70) { rear.reportNum = 1; rear.report[0].id = 4; rear.report[0].x = 300 + i % 90 * 5; rear.report[0].y = 400; }
            const float dt = i % 7 == 0 ? std::numeric_limits<float>::quiet_NaN() : (i % 5) * .02f;
            const auto actual = poll(dt); PortPadState expected; oracle_poll(dt, &expected); assertSame(actual, expected);
        }
        // Settings raw acquisition must still normalize a PSTV source with remap off.
        type = SCE_CTRL_TYPE_DS4; raw.buttons = SCE_CTRL_L1 | SCE_CTRL_R2;
        PortVitaRawInput ui; assert(port_vita_input_raw_init() && port_vita_input_raw_poll(ui));
        assert(ui.source == 1 && ui.held == (L1 | R2) && !ui.caps.motion && !ui.caps.frontTouch);
        type = SCE_CTRL_TYPE_PHY;
    } else {
        auto map = GetControlApi().defaults(ProfileId::PlayStation);
        std::string error, conflict;
        assert(GetControlApi().bind(map, "star_bits", 0, "square", true, NominalCaps(ProfileId::PlayStation), conflict) == BindResult::Applied);
        assert(GetControlApi().bind(map, "minus", 0, "l3", false, NominalCaps(ProfileId::PlayStation), conflict) == BindResult::Applied);
        assert(SetActiveMapping(map, ProfileId::PlayStation, NominalCaps(ProfileId::PlayStation), error));
        type = SCE_CTRL_TYPE_DS4;
        assert(port_vita_input_init()); poll(); // source entry observes neutral
        raw.buttons = SCE_CTRL_SQUARE; raw.rx = 255;
        auto p = poll(); assert((p.buttons & 0x400) && p.pointerValid && !(p.buttons & 2));
        raw.buttons = SCE_CTRL_L1; p = poll(); assert(p.buttons & 0x2000); // not L2
        raw.buttons = SCE_CTRL_L2; p = poll(); assert(!(p.buttons & 0x2000));
        neutral(); tilt = 1; raw.lx = 255; raw.ly = 0;
        p = poll(); assert(p.tilted && p.downX > 0 && p.downZ > 0 && p.downY < 0);
        assert(std::fabs(p.downX*p.downX + p.downY*p.downY + p.downZ*p.downZ - 1) < 1e-5f);
        tilt = 0; assert(!poll().tilted);
    }
    // Raw UI polling cannot consume a pending runtime reset.
    neutral(); menus = zoom = tilt = 0; poll();
    raw.buttons = SCE_CTRL_CROSS | SCE_CTRL_START; raw.lx = 255; raw.rx = 255;
    front.reportNum = 1; front.report[0].x = 900; front.report[0].y = 500;
    poll(); port_vita_input_settings_rearm();
    PortVitaRawInput ui; port_vita_input_raw_poll(ui);
    auto p = poll(); assert(p.connected && !p.buttons && !p.stickX && !p.pointerValid && !p.touched && !p.accX);
    for (int i = 0; i < 20; ++i) { p = poll(); assert(!p.buttons && !p.stickX && !p.touched); }
    neutral(); poll(); raw.buttons = SCE_CTRL_CROSS; assert(poll().buttons & 0x800);
    // Fixed pause commands are raw and gated before any remapping side path.
    neutral(); zoom = 1; poll();
    raw.buttons = SCE_CTRL_TRIANGLE | (type == SCE_CTRL_TYPE_PHY ? SCE_CTRL_RTRIGGER : SCE_CTRL_R1);
    poll(); port_vita_input_settings_rearm();
    const int keptZoom = zoomSteps, keptToggles = toggles;
    for (int i = 0; i < 80; ++i) poll();
    assert(zoomSteps == keptZoom && toggles == keptToggles);
    neutral(); poll(); raw.buttons = SCE_CTRL_TRIANGLE; poll(); assert(toggles == keptToggles+1);
    zoom = 0;
    // Read-once means a later environment edit cannot switch the gameplay path.
    setenv("GXV_REMAP", off ? "1" : "0", 1);
    assert(RemapEnabled() == !off);
    // Errors, not stale held data; reconnect must rearm until release.
    readResult = -1; assert(!poll().connected); readResult = 1; assert(!poll().buttons);
    neutral(); poll(); raw.buttons = SCE_CTRL_CROSS; assert(poll().buttons & 0x800);
    std::puts("PASS: actual Vita input oracle, normalization, remap, tilt and rearm");
}
