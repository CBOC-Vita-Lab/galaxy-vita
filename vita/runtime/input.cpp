#include "input.h"
#include "port/pointer_aim.h"
#include "port/tilt_gain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>

extern "C" int port_rear_aim_enabled(void);  // platform/src/port/perf.cpp: the option, kept in settings.ini
extern "C" int port_rear_aim_toggle(void);
extern "C" int port_stick_pointer_enabled(void);  // the same file: the right stick moves the pointer, not the camera (pause menu: Square)
extern "C" int port_stick_pointer_toggle(void);
extern "C" int port_star_bit_reach_next(void);  // the same file: Star Bit reach (pause menu: Select steps it)
extern "C" int port_skip_context_kind(void) __attribute__((weak));  // platform/src/port/cutscene_skip.cpp: 4 cutscene, 5 dialogue, 0 none

namespace {
struct Vec { float x, y, z; };
Vec neutral, right, forward;
bool calibrated, motionStarted, touchStarted, rearStarted, pointerValid;
float pointerX, pointerY;
SceTouchPanelInfo panel, rearPanel;
// Controls (env.txt):
//   PETARI_RSTICK=pointer   the right stick moves the pointer, whatever the pause menu's setting says (forkB, from Producdevity's
//                           fork: Square in the pause menu switches the stick between Camera and Pointer, saved in settings.ini as
//                           stick_pointer; its default: it is the camera, the Wii remote's D-pad: left / right turn, up first
//                           person, down back; the D-pad stays the camera either way)
//                           With an external pad (PS TV: DS3 / DS4) on the PlayStation profile the right stick is the pointer
//                           by default (R2 shoots, the D-pad is the camera, see platform/src/input/control_bindings.cpp);
//                           PETARI_RSTICK=camera keeps the stick the camera there too, and over the pause menu's setting.
//   GXV_MOUSE_AIM=1         (Settings > Controls > Mouse-style aiming) the stick moves the pointer like a mouse: speed on a curve
//                           with acceleration while held (platform/include/port/pointer_aim.h); default off = the 1.0 linear speed
//   GXV_TILT_GAIN=50..200   (Settings > Controls > Tilt sensitivity) percent of the lean that steers Star Ball and ray surfing, in steps
//                           of 25; 100 (default) is the game's own steering, untouched (platform/include/port/tilt_gain.h)
//   GXV_TILT_STICK=1        (Settings > Controls > Tilt with) the left stick tilts those levels even where the motion sensor exists
//   PETARI_TOUCH_A=0        a touch only moves the pointer (default: a short tap also presses A
//                           where it lands, a finger held still holds A, a drag does neither)
//   GXV_VITA_WIDE=0         (renderer) the picture unstretched between side bars; the pointer follows
// Always: on the game's 2D menus the left stick and the D-pad move the pointer; in tilt-steered
// levels "level" is the grip when the level starts steering, Select sets it again.
bool rstickPointer, touchA = true, wide = true;
bool rstickCamera;       // PETARI_RSTICK=camera: the stick stays the camera on a pad too
bool mouseAim;           // GXV_MOUSE_AIM
float mouseHeld;         // port::aim::State::held
// The touch in progress.
bool touching, touchMoved;
float touchTime, touchX0, touchY0;
float tapA; // seconds of A still to give after a tap
// GXV_TAP_CLEAN=1 (read once, default off): during a dialogue or a cutscene a touch is only ever a press of A, never a held
// A.  The game finishes a page of text when A is held for 11 updates (TalkStateEvent::talk) and the port skips the whole
// dialogue when A is held for a second (platform/src/port/cutscene_skip.cpp), and a finger resting on the screen held A for
// as long as it stayed.  With the switch a finger held still gives one 0.1 s press at the 0.2 s mark (the same edge a
// longer touch gave), and a new touch ends a press still running so two taps are two edges.  Pull Stars and the rest of
// play (no dialogue, no cutscene) keep the held A.
bool holdPulsed;
bool tapCleanOn() {
    static const bool on = [] { const char* e = getenv("GXV_TAP_CLEAN"); return e && strcmp(e, "1") == 0; }();
    return on;
}
bool talkOrDemoOnScreen() {
    if (!port_skip_context_kind) return false;
    const int kind = port_skip_context_kind();
    return kind == 4 /* PORT_SKIP_DEMO */ || kind == 5 /* PORT_SKIP_TALK */;
}
// The pointer is on screen while a finger is, for a few seconds after, and while Circle (B:
// shoot a Star Bit at the pointer) is held, when the right stick aims it. With no pointer the
// game's menus ask to "touch the screen" (the "point at the screen" text, platform/src/port/handheld_ui.cpp).
bool menuPointer; // the left stick is moving the pointer (a 2D menu is up)
std::atomic<bool> pendingReset{false}, mappingAvailable{true};
bool tiltWasActive;
bool zoomArmed, rearToggleArmed, stickToggleArmed, reachToggleArmed;
int zoomHeldDir;
float zoomRepeat;
unsigned blockedButtons;
bool blockLeftStick, blockRightStick, blockFront, blockRear;
int gameSource = -1;
unsigned gameType;
uint64_t rawPrevious;
int rawSource = -1;
unsigned rawType;

void resetTransient();

bool settingsEnabled() {
    static const bool on = [] { const char* e = getenv("GXV_SETTINGS"); return e && strcmp(e, "1") == 0; }();
    return on;
}

uint64_t normalizeButtons(unsigned b, bool positive2) {
    using namespace port::controls;
    uint64_t out = 0;
    const struct { unsigned raw; uint64_t bit; } pairs[] = {
        {SCE_CTRL_CROSS, Cross}, {SCE_CTRL_CIRCLE, Circle}, {SCE_CTRL_SQUARE, Square}, {SCE_CTRL_TRIANGLE, Triangle},
        {positive2 ? SCE_CTRL_L1 : SCE_CTRL_LTRIGGER, L1}, {positive2 ? SCE_CTRL_R1 : SCE_CTRL_RTRIGGER, R1},
        {SCE_CTRL_L3, L3}, {SCE_CTRL_R3, R3}, {SCE_CTRL_START, Start}, {SCE_CTRL_SELECT, Select},
        {SCE_CTRL_UP, Up}, {SCE_CTRL_DOWN, Down}, {SCE_CTRL_LEFT, Left}, {SCE_CTRL_RIGHT, Right}
    };
    for (auto p : pairs) if (b & p.raw) out |= p.bit;
    if (positive2 && (b & SCE_CTRL_L2)) out |= L2;
    if (positive2 && (b & SCE_CTRL_R2)) out |= R2;
    return out;
}

bool selectRaw(SceCtrlData& data, PortVitaRawInput& out) {
    using namespace port::controls;
    SceCtrlPortInfo info{};
    if (sceCtrlGetControllerPortInfo(&info) < 0) return false;
    int source = -1;
    // PSTV port 0 is a virtual alias of the first paired controller. Select exactly
    // one real port; never merge that alias or two controllers' buttons.
    if (info.port[0] == SCE_CTRL_TYPE_PHY) source = 0;
    else for (int i = 1; i < 5; ++i)
        if (info.port[i] == SCE_CTRL_TYPE_DS3 || info.port[i] == SCE_CTRL_TYPE_DS4) { source = i; break; }
    if (source < 0 || sceCtrlPeekBufferPositive2(source, &data, 1) <= 0) return false;
    out.source = source; out.controllerType = info.port[source]; out.timestamp = data.timeStamp;
    const bool physical = info.port[source] == SCE_CTRL_TYPE_PHY;
    out.caps = NominalCaps(physical ? ProfileId::Handheld : ProfileId::PlayStation);
    out.caps.frontTouch = physical && touchStarted;
    out.caps.rearTouch = physical && rearStarted;
    out.caps.motion = physical && motionStarted;
    out.caps.tiltFallback = !out.caps.motion;
    out.held = normalizeButtons(data.buttons, true) & out.caps.availableButtons;
    return true;
}

// Rear touch aiming (an option: pause menu, Triangle; platform/src/port/perf.cpp keeps it in settings.ini). The rear pad works like a
// laptop's track pad: a finger sliding on it moves the pointer by how far it slides, it never jumps the pointer to where the finger is,
// and it never presses a button (Circle shoots a Star Bit, Cross holds a Pull Star, as without it).
// The hands that hold the console rest fingers on the rear pad, and the pad reports up to four contacts in any order; an earlier
// version followed report[0] wherever it was, so the pointer jumped between resting fingers and squiggled across the screen.
// Here a contact only takes the pointer once it has slid kRearArm panel units from where it landed (resting fingers do not slide),
// the pointer then follows that contact by its id alone until it lifts, the movement is smoothed over a few samples and a
// movement below the pad's noise is ignored. A slide that took the pointer within kRearArmQuick of landing moves it from where
// it landed, so the start of a deliberate slide is not lost. Not on the game's 2D menus (the left stick and Cross work there)
// and never while a finger is on the front screen. (The quick slide and the reset below: fixes from Producdevity's fork, 82e2219.)
float clamp(float x, float lo, float hi) { return fminf(hi, fmaxf(lo, x)); }
float pointerLife;
constexpr float kPointerSeconds = 3.0f;
struct RearContact { int id; float x0, y0, age; bool used; };
RearContact rearSeen[8];
int rearSeenCount;
int rearId = -1;          // the contact that moves the pointer, -1 none
float rearLastX, rearLastY, rearVelX, rearVelY;
bool rearAccumulate = true;  // PETARI_REAR_ACCUM (default 1): movements below the noise step add up (see rearAim)
constexpr float kRearArm = 40.0f;     // panel units (the rear pad is about 1920 x 890): about 2 % of its width
constexpr float kRearArmQuick = 0.25f;  // seconds; slower than kRearArm in this long is a resting finger creeping
constexpr float kRearNoise = 1.5f;    // panel units per sample
constexpr float kRearGain = 2.0f / 1100.0f;  // pointer units (-1..1 across the picture) per panel unit: 1100 units = the whole width

void resetTransient() {
    calibrated = pointerValid = touching = touchMoved = menuPointer = tiltWasActive = false;
    neutral = right = forward = {};
    pointerX = pointerY = pointerLife = tapA = touchTime = touchX0 = touchY0 = 0;
    mouseHeld = 0;
    holdPulsed = false;
    rearId = -1; rearSeenCount = 0;
    memset(rearSeen, 0, sizeof rearSeen);
    rearLastX = rearLastY = rearVelX = rearVelY = 0;
    zoomArmed = rearToggleArmed = stickToggleArmed = reachToggleArmed = false;
    zoomHeldDir = 0; zoomRepeat = 0;
    blockedButtons = ~0u;
    blockLeftStick = blockRightStick = blockFront = blockRear = true;
}

void rearAim(bool frontDown, float dt) {
    if (!rearStarted) return;
    SceTouchData rear{};
    const bool enabled = !frontDown && port_rear_aim_enabled();
    const bool contact = (enabled || blockRear) && sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0 && rear.reportNum > 0;
    const int n = contact ? (rear.reportNum < 8 ? rear.reportNum : 8) : 0;
    if (blockRear) {
        // After an input reset (the Settings menu has closed) the contacts already on the pad never take the pointer; one put
        // down afterwards can. Waiting for an empty pad instead kept rear aiming off while a hand holding the console rested
        // a finger on it.
        blockRear = false;
        rearId = -1;
        rearSeenCount = 0;
        for (int i = 0; i < n; i++) rearSeen[rearSeenCount++] = {rear.report[i].id, float(rear.report[i].x), float(rear.report[i].y), 0, true};
        return;
    }
    if (!enabled || !contact) {
        rearId = -1;
        rearSeenCount = 0;
        return;
    }
    // forget contacts that have lifted, note new ones where they landed
    RearContact kept[8];
    int keptCount = 0;
    for (int i = 0; i < n; i++) {
        const SceTouchReport& r = rear.report[i];
        RearContact c{r.id, float(r.x), float(r.y), 0, false};
        for (int k = 0; k < rearSeenCount; k++) {
            if (rearSeen[k].id == r.id) { c = rearSeen[k]; c.age += dt; break; }
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
                const bool quick = c.age <= kRearArmQuick;
                rearLastX = quick ? c.x0 : r.x;
                rearLastY = quick ? c.y0 : r.y;
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
    port::tilt::Scale(pad->downX, pad->downY, pad->downZ, port::tilt::Percent());
}
}

extern "C" int port_input_tilt_active(void); // platform/src/input/wpad.cpp
extern "C" int port_pointer_on_menus(void);  // platform/src/gx/vgx_galaxy.cpp
extern "C" int port_zoom_menu_active(void);  // platform/src/port/perf.cpp: the pause menu waits for a choice
extern "C" int port_zoom_adjust(int steps);

// In the pause menu L lowers the camera zoom and R raises it, a step (0.1) per press and, held, one more every 0.1 s after
// 0.4 s. A shoulder button that was already down when the menu opened does nothing until it has been let go.
static void zoomButtons(unsigned buttons, float dt) {
    bool& armed = zoomArmed;
    int& heldDir = zoomHeldDir;
    float& repeat = zoomRepeat;
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

// In the pause menu Triangle turns rear touch aiming on or off, Square switches the right stick between the camera and the
// pointer and Select steps the Star Bit reach (a press; held, nothing more happens). The game does not get Select there (the
// Wii's Minus would close the menu).
static void toggleButton(unsigned buttons, unsigned button, bool& armed, int (*toggle)(void)) {
    if (!port_zoom_menu_active()) {
        armed = false;
        return;
    }
    const bool down = (buttons & button) != 0;
    if (!down) {
        armed = true;
        return;
    }
    if (armed) {
        armed = false;
        toggle();
    }
}

extern "C" int port_vita_input_init(void) {
    calibrated = false;
    pointerValid = false;
    pointerX = pointerY = 0;
    pointerLife = 0;
    if (const char* e = getenv("PETARI_RSTICK")) { rstickPointer = strcmp(e, "pointer") == 0; rstickCamera = strcmp(e, "camera") == 0; }
    if (const char* e = getenv("GXV_MOUSE_AIM")) mouseAim = atoi(e) != 0;  // read once; the Settings menu sets it (settings_launch.cpp), default off
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
    (void)settingsEnabled();
    (void)port::controls::RemapEnabled();
    if (port::controls::RemapEnabled()) {
        (void)port::controls::ActiveMapping();
        PortVitaRawInput launch;
        SceCtrlData sample{};
        selectRaw(sample, launch);
        port::controls::FreezeActiveCapabilities(launch.caps);
    }
    return 1;
}

extern "C" int port_vita_input_raw_init(void) {
    (void)settingsEnabled();
    (void)port::controls::RemapEnabled();
    return sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG) >= 0;
}

bool port_vita_input_raw_poll(PortVitaRawInput& out) {
    out = {};
    SceCtrlData data{};
    if (!selectRaw(data, out)) {
        out.released = rawPrevious;
        rawPrevious = 0; rawSource = -1; rawType = 0;
        return false;
    }
    if (out.source != rawSource || out.controllerType != rawType) rawPrevious = out.held;
    out.pressed = out.held & ~rawPrevious; out.released = rawPrevious & ~out.held;
    rawPrevious = out.held; rawSource = out.source; rawType = out.controllerType;
    stick(data.lx, data.ly, out.leftX, out.leftY);
    stick(data.rx, data.ry, out.rightX, out.rightY);
    return true;
}

bool port_vita_input_mapping_available(void) { return mappingAvailable.load(std::memory_order_acquire); }
extern "C" void port_vita_input_settings_rearm(void) { pendingReset.store(true, std::memory_order_release); }

extern "C" void port_vita_input_recenter(void) { calibrated = false; }

// True when the controller in use is an external pad (a DS3 or DS4 on a PS TV) and no Vita handheld controls are present.
// Valid before port_vita_input_init: the Settings start-up uses it to choose the profile.
extern "C" int port_vita_input_external_pad(void) {
    SceCtrlPortInfo info{};
    if (sceCtrlGetControllerPortInfo(&info) < 0 || info.port[0] == SCE_CTRL_TYPE_PHY) return 0;
    for (int i = 1; i < 5; ++i)
        if (info.port[i] == SCE_CTRL_TYPE_DS3 || info.port[i] == SCE_CTRL_TYPE_DS4) return 1;
    return 0;
}

extern "C" void port_vita_input_poll(float elapsedSeconds, PortPadState* pad) {
    // The UI cannot consume this flag. Reset before even the first resumed SDK read.
    if (pendingReset.exchange(false, std::memory_order_acq_rel)) resetTransient();
    *pad = {};
    SceCtrlData control{};
    PortVitaRawInput sampled;
    const bool remap = port::controls::RemapEnabled();
    const bool ok = remap ? selectRaw(control, sampled) : sceCtrlPeekBufferPositive(0, &control, 1) > 0;
    if (!ok) { resetTransient(); gameSource = -1; gameType = 0; mappingAvailable.store(false); return; }
    if (remap && (sampled.source != gameSource || sampled.controllerType != gameType)) resetTransient();
    gameSource = remap ? sampled.source : 0; gameType = remap ? sampled.controllerType : SCE_CTRL_TYPE_PHY;
    blockedButtons &= control.buttons;
    control.buttons &= ~blockedButtons;
    float gateX, gateY;
    stick(control.lx, control.ly, gateX, gateY);
    if (blockLeftStick && gateX == 0 && gateY == 0) blockLeftStick = false;
    if (blockLeftStick) control.lx = control.ly = 128;
    stick(control.rx, control.ry, gateX, gateY);
    if (blockRightStick && gateX == 0 && gateY == 0) blockRightStick = false;
    if (blockRightStick) control.rx = control.ry = 128;
    unsigned commandButtons = control.buttons;
    bool mappedSpin = false;
    bool padPointer = false;  // an external pad on the PlayStation profile: the right stick is the pointer
    if (remap) {
        using namespace port::controls;
        const auto& active = ActiveMapping();
        uint64_t normalized = normalizeButtons(control.buttons, true) & sampled.caps.availableButtons;
        commandButtons = 0;
        if (normalized & L1) commandButtons |= SCE_CTRL_LTRIGGER;
        if (normalized & R1) commandButtons |= SCE_CTRL_RTRIGGER;
        if (normalized & Triangle) commandButtons |= SCE_CTRL_TRIANGLE;
        if (normalized & Square) commandButtons |= SCE_CTRL_SQUARE;
        if (normalized & Select) commandButtons |= SCE_CTRL_SELECT;
        uint64_t required = 0;
        for (auto mask : active.masks) required |= mask;
        const auto& promptCaps = ActiveCapabilities();
        const bool available = (required & ~sampled.caps.availableButtons) == 0 &&
            (!promptCaps.frontTouch || sampled.caps.frontTouch) &&
            (!promptCaps.rearTouch || sampled.caps.rearTouch) &&
            (!promptCaps.motion || sampled.caps.motion);
        mappingAvailable.store(available, std::memory_order_release);
        // A saved profile that needs buttons this controller lacks (a PlayStation profile with L2 on a
        // handheld Vita, say) falls back to the default buttons of the connected controller, so the
        // game never loses all input.
        const CompiledMapping* mapping = &active;
        if (!available) {
            static CompiledMapping fallback[2];
            static int fallbackState[2] = {0, 0};  // 0 not built, 1 usable, -1 unusable
            const ProfileId kind = sampled.controllerType == SCE_CTRL_TYPE_PHY ? ProfileId::Handheld : ProfileId::PlayStation;
            int& state = fallbackState[static_cast<unsigned>(kind)];
            if (!state) {
                std::string error;
                state = Compile(GetControlApi().defaults(kind), kind, sampled.caps, fallback[static_cast<unsigned>(kind)], error) ? 1 : -1;
            }
            if (state < 0) { pad->connected = 1; return; }
            mapping = &fallback[static_cast<unsigned>(kind)];
        }
        // In the pause menu L1/R1/Triangle/Select drive zoom, rear aiming and Star Bit reach, so the game does not get them,
        // except a button the player bound to confirm, back, pause or menu movement: those always pass,
        // so a custom layout can never trap the player in the menu.
        if (port_zoom_menu_active()) {
            uint64_t menuKeys = 0;
            for (Action a : {Action::Jump, Action::StarBits, Action::Pause, Action::CameraUp, Action::CameraDown,
                             Action::CameraLeft, Action::CameraRight})
                menuKeys |= mapping->masks[static_cast<size_t>(a)];
            normalized &= ~((L1 | R1 | Triangle | Select) & ~menuKeys);
        }
        padPointer = port::controls::PadDefaultsEnabled() && !rstickCamera && sampled.controllerType != SCE_CTRL_TYPE_PHY &&
            mapping->profile == ProfileId::PlayStation;
        const auto translated = Translate(*mapping, normalized);
        mappedSpin = translated.spin;
        control.buttons = 0;
        const struct { uint32_t wii; unsigned vita; } legacy[] = {
            {0x800, SCE_CTRL_CROSS}, {0x400, SCE_CTRL_CIRCLE}, {0x2000, SCE_CTRL_LTRIGGER},
            {0x4000, SCE_CTRL_TRIANGLE}, {0x10, SCE_CTRL_START}, {0x1000, SCE_CTRL_SELECT},
            {1, SCE_CTRL_LEFT}, {2, SCE_CTRL_RIGHT}, {4, SCE_CTRL_DOWN}, {8, SCE_CTRL_UP}
        };
        for (auto p : legacy) if (translated.buttons & p.wii) control.buttons |= p.vita;
    } else mappingAvailable.store(true, std::memory_order_release);
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
        if (!remap && zoomMenu && (button.vita == SCE_CTRL_LTRIGGER || button.vita == SCE_CTRL_TRIANGLE || button.vita == SCE_CTRL_SELECT)) continue;
        if (control.buttons & button.vita) pad->buttons |= button.wii;
    }
    stick(control.lx, control.ly, pad->stickX, pad->stickY);
    float rx, ry;
    stick(control.rx, control.ry, rx, ry);
    if (!isfinite(elapsedSeconds)) elapsedSeconds = 0;
    float dt = clamp(elapsedSeconds, 0, 0.05f);
    zoomButtons(commandButtons, dt);
    toggleButton(commandButtons, SCE_CTRL_TRIANGLE, rearToggleArmed, port_rear_aim_toggle);
    toggleButton(commandButtons, SCE_CTRL_SQUARE, stickToggleArmed, port_stick_pointer_toggle);
    toggleButton(commandButtons, SCE_CTRL_SELECT, reachToggleArmed, port_star_bit_reach_next);
    const bool aiming = (control.buttons & SCE_CTRL_CIRCLE) != 0;
    const bool stickPointerSetting = !rstickCamera && port_stick_pointer_enabled();  // (the pause menu's Right Stick: Pointer)
    if (rstickPointer || padPointer || stickPointerSetting || aiming) {
        if (aiming && pointerLife <= 0) pointerX = pointerY = 0; // (a fresh aim starts from the middle)
        if (mouseAim) {
            // Mouse-style: speed on a curve with acceleration while held (the raw stick: the dead zone is in aim::Step).
            const float aspect = (wide ? 960.0f : 544.0f * 4.0f / 3.0f) / 544.0f;
            port::aim::State st{pointerX, pointerY, mouseHeld};
            const bool moved = port::aim::Step(st, (control.rx - 127.5f) / 127.5f, (127.5f - control.ry) / 127.5f, dt, aspect);
            pointerX = st.x; pointerY = st.y; mouseHeld = st.held;
            if (moved) pointerLife = kPointerSeconds;
        } else if (rx != 0 || ry != 0) {
            pointerX = clamp(pointerX + rx * dt * 2.0f, -1, 1);
            pointerY = clamp(pointerY - ry * dt * 2.0f, -1, 1);
            pointerLife = kPointerSeconds;
        }
        if (aiming) pointerLife = kPointerSeconds;
    } else {
        mouseHeld = 0;
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
    const bool contact = touchStarted && (!remap || sampled.caps.frontTouch) && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum;
    if (blockFront && !contact) blockFront = false;
    const bool down = contact && !blockFront;
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
            holdPulsed = false;
            if (tapCleanOn()) tapA = 0;  // (a press of the tap before is over: this touch makes its own edge)
        }
        touchTime += dt;
        if (fabsf(pointerX - touchX0) > 0.08f || fabsf(pointerY - touchY0) > 0.08f) touchMoved = true;
        if (touchA && !touchMoved && touchTime >= 0.20f) {
            if (tapCleanOn() && talkOrDemoOnScreen()) {
                if (!holdPulsed) { holdPulsed = true; tapA = 0.10f; }  // one press, not a hold
            } else {
                pad->buttons |= 0x0800; // a finger held still: A held (pull stars)
            }
        }
    } else if (touching) {
        touching = false;
        if (touchA && !touchMoved && touchTime < 0.20f) tapA = 0.10f; // a tap: A where it landed
    }
    // the rear pad (if chosen): in play only, never with a finger on the front screen
    rearAim(contact || menuPointer || (remap && !sampled.caps.rearTouch), dt);
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
        const bool tiltActive = port_input_tilt_active() != 0;
        if (tiltActive && (!tiltWasActive || (control.buttons & SCE_CTRL_SELECT))) calibrated = false;
        tiltWasActive = tiltActive;
    }
    if (remap ? mappedSpin : (control.buttons & (zoomMenu ? SCE_CTRL_SQUARE : (SCE_CTRL_RTRIGGER | SCE_CTRL_SQUARE)))) {
        // A held shoulder produces shake samples; the game owns spin cooldown.
        float shake = (control.timeStamp / 16000) & 1 ? 2.5f : -2.5f;
        pad->accX = pad->accY = shake;
    }
    // Tilt from the motion sensor, or (no sensor, or "Tilt with: Left stick") from the left stick.
    if (!remap || (sampled.caps.motion && !port::tilt::UseStick())) motion(pad);
    else if ((sampled.caps.tiltFallback || sampled.caps.motion) && port_input_tilt_active()) {
        // Same gravity interface as motion; WPAD retains the level's neutral pitch.
        const float x = pad->stickX * 0.65f, z = pad->stickY * 0.65f;
        pad->tilted = 1; pad->downX = x; pad->downZ = z;
        pad->downY = -sqrtf(fmaxf(0, 1.0f - x*x - z*z));
        port::tilt::Scale(pad->downX, pad->downY, pad->downZ, port::tilt::Percent());
        pad->stickX = pad->stickY = 0;
    }
}

extern "C" void port_vita_input_shutdown(void) {
    if (motionStarted) sceMotionStopSampling();
    if (touchStarted) sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    if (rearStarted) sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
    motionStarted = touchStarted = rearStarted = false;
}
