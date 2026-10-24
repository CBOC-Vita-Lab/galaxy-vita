#include "input.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>

extern "C" int port_rear_aim_enabled(void);  // platform/src/port/perf.cpp: the option, kept in settings.ini
extern "C" int port_rear_aim_toggle(void);

namespace {
struct Vec { float x, y, z; };
Vec neutral, right, forward;
bool calibrated, motionStarted, touchStarted, rearStarted, pointerValid;
float pointerX, pointerY;
SceTouchPanelInfo panel, rearPanel;
// Controls (env.txt):
//   PETARI_RSTICK=pointer   the right stick moves the pointer (default: it is the camera, the
//                           Wii remote's D-pad: left / right turn, up first person, down back)
//   PETARI_TOUCH_A=0        a touch only moves the pointer (default: a short tap also presses A
//                           where it lands, a finger held still holds A, a drag does neither)
//   GXV_VITA_WIDE=0         (renderer) the picture unstretched between side bars; the pointer follows
// Always: on the game's 2D menus the left stick and the D-pad move the pointer; in tilt-steered
// levels "level" is the grip when the level starts steering, Select sets it again.
bool rstickPointer, touchA = true, wide = true;
// The touch in progress.
bool touching, touchMoved;
float touchTime, touchX0, touchY0;
float tapA; // seconds of A still to give after a tap
// The pointer is on screen while a finger is, for a few seconds after, and while Circle (B:
// shoot a Star Bit at the pointer) is held, when the right stick aims it. With no pointer the
// game's menus ask to "touch the screen" (the "point at the screen" text, platform/src/port/handheld_ui.cpp).
bool menuPointer; // the left stick is moving the pointer (a 2D menu is up)

// Rear touch aiming (an option: pause menu, Triangle; platform/src/port/perf.cpp keeps it in settings.ini). The rear pad works like a
// laptop's track pad: a finger sliding on it moves the pointer by how far it slides, it never jumps the pointer to where the finger is,
// and it never presses a button (Circle shoots a Star Bit, Cross holds a Pull Star, as without it).
// The hands that hold the console rest fingers on the rear pad, and the pad reports up to four contacts in any order; an earlier
// version followed report[0] wherever it was, so the pointer jumped between resting fingers and squiggled across the screen.
// Here a contact only takes the pointer once it has slid kRearArm panel units from where it landed (resting fingers do not slide),
// the pointer then follows that contact by its id alone until it lifts, the movement is smoothed over a few samples and a
// movement below the pad's noise is ignored. Not on the game's 2D menus (the left stick and Cross work there) and never while a
// finger is on the front screen.
float clamp(float x, float lo, float hi) { return fminf(hi, fmaxf(lo, x)); }
float pointerLife;
constexpr float kPointerSeconds = 3.0f;
struct RearContact { int id; float x0, y0; bool used; };
RearContact rearSeen[8];
int rearSeenCount;
int rearId = -1;          // the contact that moves the pointer, -1 none
float rearLastX, rearLastY, rearVelX, rearVelY;
bool rearAccumulate = true;  // PETARI_REAR_ACCUM (default 1): movements below the noise step add up (see rearAim)
constexpr float kRearArm = 40.0f;     // panel units (the rear pad is about 1920 x 890): about 2 % of its width
constexpr float kRearNoise = 1.5f;    // panel units per sample
constexpr float kRearGain = 2.0f / 1100.0f;  // pointer units (-1..1 across the picture) per panel unit: 1100 units = the whole width

void rearAim(bool frontDown) {
    if (!rearStarted) return;
    SceTouchData rear{};
    const bool any = !frontDown && port_rear_aim_enabled() && sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0 && rear.reportNum > 0;
    if (!any) {
        rearId = -1;
        rearSeenCount = 0;
        return;
    }
    const int n = rear.reportNum < 8 ? rear.reportNum : 8;
    // forget contacts that have lifted, note new ones where they landed
    RearContact kept[8];
    int keptCount = 0;
    for (int i = 0; i < n; i++) {
        const SceTouchReport& r = rear.report[i];
        RearContact c{r.id, float(r.x), float(r.y), false};
        for (int k = 0; k < rearSeenCount; k++) {
            if (rearSeen[k].id == r.id) { c = rearSeen[k]; break; }
        }
        kept[keptCount++] = c;
    }
    memcpy(rearSeen, kept, sizeof(RearContact) * static_cast<size_t>(keptCount));
    rearSeenCount = keptCount;
    const SceTouchReport* aim = nullptr;
    for (int i = 0; i < n; i++) {
        if (rear.report[i].id == rearId) aim = &rear.report[i];
    }
    if (!aim) {
        rearId = -1;
        // a contact takes the pointer when it has slid far enough from where it landed
        for (int i = 0; i < n && !aim; i++) {
            const SceTouchReport& r = rear.report[i];
            RearContact& c = rearSeen[i];
            const float dx = r.x - c.x0, dy = r.y - c.y0;
            if (!c.used && dx * dx + dy * dy >= kRearArm * kRearArm) {
                c.used = true;
                aim = &r;
                rearId = r.id;
                rearLastX = r.x;
                rearLastY = r.y;
                rearVelX = rearVelY = 0;
                if (pointerLife <= 0) pointerX = pointerY = 0; // (a fresh pointer starts in the middle)
            }
        }
        if (!aim) return;
    }
    // A movement below the pad's noise is not dropped but kept until it adds up to more (the reference point only moves on
    // with the pointer): a slow, careful slide (under 1.5 units a sample, ~90 units a second at the pad's 60 samples a
    // second) still moves the pointer, where an earlier version threw each small step away and the pointer stood still.
    // (PETARI_REAR_ACCUM=0 in env.txt: the earlier way, for comparison)
    float dx = aim->x - rearLastX, dy = aim->y - rearLastY;
    if (!rearAccumulate) rearLastX = aim->x, rearLastY = aim->y;
    if (fabsf(dx) >= kRearNoise) rearLastX = aim->x;
    else dx = 0;
    if (fabsf(dy) >= kRearNoise) rearLastY = aim->y;
    else dy = 0;
    rearVelX = rearVelX * 0.5f + dx * 0.5f;  // a short running average takes the pad's jitter out
    rearVelY = rearVelY * 0.5f + dy * 0.5f;
    const float panelW = float(rearPanel.maxDispX - rearPanel.minDispX), panelH = float(rearPanel.maxDispY - rearPanel.minDispY);
    const float gainY = kRearGain * (panelW > 0 && panelH > 0 ? (panelW / panelH) * (544.0f / 960.0f) : 1.0f);
    pointerX = clamp(pointerX + rearVelX * kRearGain, -1, 1);
    pointerY = clamp(pointerY + rearVelY * gainY, -1, 1);
    pointerLife = kPointerSeconds;
}

float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
bool normalize(Vec& v) {
    float length = sqrtf(dot(v, v));
    if (!isfinite(length) || length < 0.1f) return false;
    v = {v.x / length, v.y / length, v.z / length};
    return true;
}

void stick(unsigned char x, unsigned char y, float& outX, float& outY) {
    float sx = (x - 127.5f) / 127.5f;
    float sy = (127.5f - y) / 127.5f;
    float radius = sqrtf(sx * sx + sy * sy);
    const float deadzone = 0.14f;
    if (radius <= deadzone) { outX = outY = 0; return; }
    float scale = (fminf(radius, 1.0f) - deadzone) / ((1.0f - deadzone) * radius);
    outX = sx * scale;
    outY = sy * scale;
}

void motion(PortPadState* pad) {
    SceMotionState state{};
    if (!motionStarted || sceMotionGetState(&state) < 0) return;
    Vec gravity{state.acceleration.x, state.acceleration.y, state.acceleration.z};
    if (!normalize(gravity)) return;
    if (!calibrated) {
        neutral = gravity;
        Vec axis = fabsf(gravity.x) < 0.9f ? Vec{1, 0, 0} : Vec{0, 0, 1};
        float parallel = dot(axis, gravity);
        right = {axis.x - parallel * gravity.x, axis.y - parallel * gravity.y,
                 axis.z - parallel * gravity.z};
        if (!normalize(right)) return;
        forward = cross(right, neutral);
        calibrated = true;
    }
    // Calibrate the holding angle, not Galaxy's level-specific neutral pitch.
    // wpad.cpp applies that pitch only when a tilt-controlled level asks for it.
    pad->tilted = 1;
    pad->downX = dot(gravity, right);
    pad->downY = -dot(gravity, neutral);
    pad->downZ = dot(gravity, forward);
}
}

extern "C" int port_input_tilt_active(void); // platform/src/input/wpad.cpp
extern "C" int port_pointer_on_menus(void);  // platform/src/gx/vgx_galaxy.cpp
extern "C" int port_zoom_menu_active(void);  // platform/src/port/perf.cpp: the pause menu waits for a choice
extern "C" int port_zoom_adjust(int steps);

// In the pause menu L lowers the camera zoom and R raises it, a step (0.1) per press and, held, one more every 0.1 s after
// 0.4 s. A shoulder button that was already down when the menu opened does nothing until it has been let go.
static void zoomButtons(unsigned buttons, float dt) {
    static bool armed = false;
    static int heldDir = 0;
    static float repeat = 0;
    if (!port_zoom_menu_active()) {
        armed = false;
        heldDir = 0;
        return;
    }
    const int dir = ((buttons & SCE_CTRL_RTRIGGER) ? 1 : 0) - ((buttons & SCE_CTRL_LTRIGGER) ? 1 : 0);
    if (dir == 0) {
        armed = true;
        heldDir = 0;
        return;
    }
    if (!armed) return;
    if (dir != heldDir) {
        heldDir = dir;
        repeat = 0.4f;
        port_zoom_adjust(dir);
    } else if ((repeat -= dt) <= 0) {
        repeat += 0.1f;
        port_zoom_adjust(dir);
    }
}

// In the pause menu Triangle turns rear touch aiming on or off (a press; held, nothing more happens).
static void rearToggleButton(unsigned buttons) {
    static bool armed = false;
    if (!port_zoom_menu_active()) {
        armed = false;
        return;
    }
    const bool down = (buttons & SCE_CTRL_TRIANGLE) != 0;
    if (!down) {
        armed = true;
        return;
    }
    if (armed) {
        armed = false;
        port_rear_aim_toggle();
    }
}

extern "C" int port_vita_input_init(void) {
    calibrated = false;
    pointerValid = false;
    pointerX = pointerY = 0;
    pointerLife = 0;
    if (const char* e = getenv("PETARI_RSTICK")) rstickPointer = strcmp(e, "pointer") == 0;
    if (const char* e = getenv("PETARI_TOUCH_A")) touchA = atoi(e) != 0;
    if (const char* e = getenv("GXV_VITA_WIDE")) wide = atoi(e) != 0;
    if (const char* e = getenv("PETARI_REAR_ACCUM")) rearAccumulate = atoi(e) != 0;
    if (sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG) < 0) return 0;
    motionStarted = sceMotionStartSampling() >= 0;
    touchStarted = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) >= 0 &&
        panel.maxDispX > panel.minDispX && panel.maxDispY > panel.minDispY &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0;
    rearStarted = sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rearPanel) >= 0 &&
        rearPanel.maxDispX > rearPanel.minDispX && rearPanel.maxDispY > rearPanel.minDispY &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START) >= 0;
    rearId = -1;
    rearSeenCount = 0;
    return 1;
}

extern "C" void port_vita_input_recenter(void) { calibrated = false; }

extern "C" void port_vita_input_poll(float elapsedSeconds, PortPadState* pad) {
    *pad = {};
    SceCtrlData control{};
    if (sceCtrlPeekBufferPositive(0, &control, 1) <= 0) return;
    pad->connected = 1;
    const struct { unsigned vita, wii; } buttons[] = {
        // Cross = A (jump), Circle = B (star bits), L = Z (crouch), Triangle = C (camera behind
        // Mario), Start = +, Select = -. Square and R shake the remote (spin): below.
        {SCE_CTRL_CROSS, 0x0800}, {SCE_CTRL_CIRCLE, 0x0400},
        {SCE_CTRL_LTRIGGER, 0x2000},
        {SCE_CTRL_TRIANGLE, 0x4000}, {SCE_CTRL_START, 0x0010},
        {SCE_CTRL_SELECT, 0x1000}, {SCE_CTRL_LEFT, 0x0001},
        {SCE_CTRL_RIGHT, 0x0002}, {SCE_CTRL_DOWN, 0x0004}, {SCE_CTRL_UP, 0x0008}
    };
    // (In the pause menu the shoulder buttons set the camera zoom: the game does not get them.)
    const bool zoomMenu = port_zoom_menu_active() != 0;
    for (auto button : buttons) {
        if (zoomMenu && (button.vita == SCE_CTRL_LTRIGGER || button.vita == SCE_CTRL_TRIANGLE)) continue;
        if (control.buttons & button.vita) pad->buttons |= button.wii;
    }
    stick(control.lx, control.ly, pad->stickX, pad->stickY);
    float rx, ry;
    stick(control.rx, control.ry, rx, ry);
    if (!isfinite(elapsedSeconds)) elapsedSeconds = 0;
    float dt = clamp(elapsedSeconds, 0, 0.05f);
    zoomButtons(control.buttons, dt);
    rearToggleButton(control.buttons);
    const bool aiming = (control.buttons & SCE_CTRL_CIRCLE) != 0;
    if (rstickPointer || aiming) {
        if (aiming && pointerLife <= 0) pointerX = pointerY = 0; // (a fresh aim starts from the middle)
        if (rx != 0 || ry != 0) {
            pointerX = clamp(pointerX + rx * dt * 2.0f, -1, 1);
            pointerY = clamp(pointerY - ry * dt * 2.0f, -1, 1);
            pointerLife = kPointerSeconds;
        }
        if (aiming) pointerLife = kPointerSeconds;
    } else {
        if (rx < -0.5f) pad->buttons |= 0x0001;
        if (rx > 0.5f) pad->buttons |= 0x0002;
        if (ry < -0.6f) pad->buttons |= 0x0004;
        if (ry > 0.6f) pad->buttons |= 0x0008;
    }
    // On the game's 2D menus (file select, pause, yes/no, the galaxy domes) the left stick and
    // the D-pad move the pointer too and it stays on screen: everything can be chosen with
    // the stick and Cross.
    if (port_pointer_on_menus()) {
        float mx = pad->stickX, my = pad->stickY;
        if (control.buttons & SCE_CTRL_LEFT) mx -= 0.7f;
        if (control.buttons & SCE_CTRL_RIGHT) mx += 0.7f;
        if (control.buttons & SCE_CTRL_UP) my += 0.7f;
        if (control.buttons & SCE_CTRL_DOWN) my -= 0.7f;
        if (pointerLife <= 0 && !menuPointer) pointerX = pointerY = 0; // (first shown in the middle)
        pointerX = clamp(pointerX + mx * dt * 1.5f, -1, 1);
        pointerY = clamp(pointerY - my * dt * 1.5f, -1, 1);
        pointerLife = kPointerSeconds;
        menuPointer = true;
    } else {
        menuPointer = false;
    }
    SceTouchData touch{};
    const bool down = touchStarted && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum;
    pad->touched = down ? 1 : 0;
    if (down) {
        // The pointer's -1..1 spans the game's picture: the middle 4:3 part of the screen unless
        // the picture is stretched.
        const float u = (touch.report[0].x - panel.minDispX) / float(panel.maxDispX - panel.minDispX); // 0..1 across the screen
        const float v = (touch.report[0].y - panel.minDispY) / float(panel.maxDispY - panel.minDispY);
        const float pictureWidth = wide ? 1.0f : (544.0f * 4.0f / 3.0f) / 960.0f;
        pointerLife = kPointerSeconds;
        pointerX = clamp((u - 0.5f) / (0.5f * pictureWidth), -1, 1);
        pointerY = clamp(2.0f * v - 1.0f, -1, 1);
        if (!touching) {
            touching = true;
            touchMoved = false;
            touchTime = 0;
            touchX0 = pointerX;
            touchY0 = pointerY;
        }
        touchTime += dt;
        if (fabsf(pointerX - touchX0) > 0.08f || fabsf(pointerY - touchY0) > 0.08f) touchMoved = true;
        if (touchA && !touchMoved && touchTime >= 0.20f) pad->buttons |= 0x0800; // a finger held still: A held (pull stars)
    } else if (touching) {
        touching = false;
        if (touchA && !touchMoved && touchTime < 0.20f) tapA = 0.10f; // a tap: A where it landed
    }
    // the rear pad (if chosen): in play only, never with a finger on the front screen
    rearAim(down || menuPointer);
    if (tapA > 0) {
        pad->buttons |= 0x0800;
        tapA -= dt;
    }
    if (pointerLife > 0) pointerLife -= dt;
    pointerValid = pointerLife > 0;
    pad->pointerValid = pointerValid;
    pad->pointerX = pointerX;
    pad->pointerY = pointerY;
    pad->pointerDist = 1.0f;
    // Tilt-steered levels: "level" is how the console is held when the game starts to steer
    // by tilt, and again whenever Select is pressed there (it has no other use in those levels).
    {
        static bool tiltWasActive = false;
        const bool tiltActive = port_input_tilt_active() != 0;
        if (tiltActive && (!tiltWasActive || (control.buttons & SCE_CTRL_SELECT))) calibrated = false;
        tiltWasActive = tiltActive;
    }
    if (control.buttons & (zoomMenu ? SCE_CTRL_SQUARE : (SCE_CTRL_RTRIGGER | SCE_CTRL_SQUARE))) {
        // A held shoulder produces shake samples; the game owns spin cooldown.
        float shake = (control.timeStamp / 16000) & 1 ? 2.5f : -2.5f;
        pad->accX = pad->accY = shake;
    }
    motion(pad);
}

extern "C" void port_vita_input_shutdown(void) {
    if (motionStarted) sceMotionStopSampling();
    if (touchStarted) sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    if (rearStarted) sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
    motionStarted = touchStarted = rearStarted = false;
}
