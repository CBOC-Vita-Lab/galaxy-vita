#include "input.h"
#include "port/pointer_aim.h"
#include "port/rear_aim.h"
#include "port/tilt_autostick.h"
#include "port/tilt_gain.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>

extern "C" int port_rear_aim_enabled(void);  // platform/src/port/perf.cpp: the option, kept in settings.ini
extern "C" int port_stick_pointer_enabled(void);  // the same file: the right stick moves the pointer, not the camera (Settings > Camera)
extern "C" int port_tap_jump_enabled(void);  // the same file (settingsnative): a tap on the screen also presses A (Settings > Controls > Tap to Jump)
extern "C" int port_settings_hint_visible(void);  // the same file (settingsnative): the pause panel's "Triangle: Settings" is showing
extern "C" void port_handheld_pictures_changed(void) __attribute__((weak));  // platform/src/gx/vgx_galaxy.cpp: the button pictures are made again
extern "C" void port_settings_open_request(void);  // the same file: Triangle in the pause menu asks for the native Settings
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));  // the basic log (os_alarm.cpp)
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
//   GXV_TILT_AUTOSTICK=0    (default 1) off: no automatic switch to the left stick for a dead motion sensor in the tilt levels
//                           (platform/include/port/tilt_autostick.h; GXV_TILT_AUTOSTICK_S = seconds of frozen readings, default 4)
//   PETARI_TOUCH_A=0        a touch only moves the pointer (default: a short tap also presses A
//                           where it lands, a finger held still holds A, a drag does neither)
//   GXV_VITA_WIDE=0         (renderer) the picture unstretched between side bars; the pointer follows
// Always: on the game's 2D menus the left stick and the D-pad move the pointer; in tilt-steered
// levels "level" is the grip when the level starts steering, Select sets it again.
bool rstickPointer, touchA = true, wide = true;
bool rstickCamera;       // PETARI_RSTICK=camera: the stick stays the camera on a pad too
std::atomic<bool> mouseAim{false};   // GXV_MOUSE_AIM (settingsnative: Settings > Controls > Pointer sets it while the game runs: port_pointer_quick_set)
bool mouseAimSeen;       // (the poll thread's last look at it: a flip zeroes mouseHeld there, not on the setter's thread)
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
std::atomic<uint64_t> heldNormalized{0};   // settingsnative: the controller's buttons in the port's stable bits (port/control_bindings.h Button), as the poll saw them
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

// GXV_PAD_LEAN (thr24, default 0 = every poll as before; 1 = all three, else a mask): the poll thread (6 ms) and the game thread's resample ran the system's port query, the rear touch pad and the
// motion sensor on every poll (console night58 cores-5s: SMGV00001 0.7-1.3 ms a picture at priority 0x50, holding g_padMutex all the while). Bit 1: the port query is answered from the last one for up to
// kPortCachePolls polls (about half a second; a failed read of the chosen port asks again at once, so a pad that went away is seen as before); bit 2: the rear pad is not read while rear aiming is off
// (only the pointer census's "rear pad reports" count stops; an input reset still reads it once, as it blocks the contacts on the pad); bit 4: the motion sensor is not read while no tilt level is
// steering (port_input_tilt_active: the game has used the tilt in the last 0.5 s; the first poll after it starts reads it, and the calibration restarts there as before).
int padLean() {
    static const int mask = [] { const char* e = getenv("GXV_PAD_LEAN"); const int v = e && *e ? atoi(e) : 0; return v == 1 ? 7 : (v < 0 ? 0 : v & 7); }();
    return mask;
}
constexpr unsigned kPortCachePolls = 80;
struct PortCache { SceCtrlPortInfo info; unsigned age = kPortCachePolls; bool ok = false; } portCache;
unsigned leanPortQueries, leanPortHits, leanRearSkips, leanMotionSkips, leanPolls;

bool selectRaw(SceCtrlData& data, PortVitaRawInput& out, bool poll = false) {
    using namespace port::controls;
    SceCtrlPortInfo info{};
    if (poll && (padLean() & 1) && portCache.ok && portCache.age < kPortCachePolls) {
        info = portCache.info;
        ++portCache.age;
        ++leanPortHits;
    } else {
        if (sceCtrlGetControllerPortInfo(&info) < 0) { portCache.ok = false; return false; }
        ++leanPortQueries;
        if (poll) { portCache.info = info; portCache.ok = true; portCache.age = 0; }
    }
    int source = -1;
    // PSTV port 0 is a virtual alias of the first paired controller. Select exactly
    // one real port; never merge that alias or two controllers' buttons.
    if (info.port[0] == SCE_CTRL_TYPE_PHY) source = 0;
    else for (int i = 1; i < 5; ++i)
        if (info.port[i] == SCE_CTRL_TYPE_DS3 || info.port[i] == SCE_CTRL_TYPE_DS4) { source = i; break; }
    if (source < 0 || sceCtrlPeekBufferPositive2(source, &data, 1) <= 0) {
        if (poll && portCache.age > 0) portCache.age = kPortCachePolls;   // (thr24: the cached answer may be stale: the next poll asks the system)
        return false;
    }
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
// The rules are in platform/include/port/rear_aim.h (crash33, 14 Nov: the 2 % arm distance from the landing place let every resting
// finger take the pointer in time): contacts that land in the outer grip bands are ignored, one finger at a time, a contact takes
// the pointer only by one deliberate slide (48 units within 0.35 s), lets it go after 1.5 s at rest, and the pointer then follows
// that contact by its id alone, smoothed, movements below the pad's noise adding up. Not on the game's 2D menus (the left stick
// and Cross work there) and never while a finger is on the front screen. GXV_REAR_STRICT=0: the rules before.
float clamp(float x, float lo, float hi) { return fminf(hi, fmaxf(lo, x)); }
float pointerLife;
constexpr float kPointerSeconds = 3.0f;
port::rear::Aim rearAimState;   // the rules: platform/include/port/rear_aim.h (host test tests/rear_aim.cpp)
bool rearStrict = true;         // GXV_REAR_STRICT (default 1; 0 = the rules before 14 Nov)
unsigned rearLogSamples;
float rearLogMoved;
constexpr float kRearGain = 2.0f / 1100.0f;  // pointer units (-1..1 across the picture) per panel unit: 1100 units = the whole width

// The pointer census (crash33b, 14 Nov, always on; one line every 10 s in which something happened): WHICH source moved the pointer and by
// how much, and what the two touch panels reported, used or not. The owner's pointer "was moving everywhere" with, in his words, no
// finger on the back: the next run says which of the four sources below it was. The pointer has no others: the motion sensor never
// moves it (it steers the tilt levels only), and a script (ux0:data/smg/input.txt) writes the pad after this code.
enum { kSrcRear, kSrcStick, kSrcMenu, kSrcFront, kSrcN };
struct PointerCensus {
    float t, moved[kSrcN];
    unsigned samples[kSrcN], polls, shown;
    unsigned rearSamples, rearMax, rearLandings, rearUsed, frontLandings, circle;
    float rx0, rx1, ry0, ry1, fx0, fx1, fy0, fy1;
    int rearIds[8], rearIdCount;
    bool frontWas, first;
};
PointerCensus pc;
void pcMove(int src, float x0, float y0) {
    const float d = fabsf(pointerX - x0) + fabsf(pointerY - y0);
    if (d > 0) { pc.moved[src] += d; ++pc.samples[src]; }
}
void pcRear(const SceTouchData& rear, int n, bool used) {
    if (n <= 0) { pc.rearIdCount = 0; return; }
    if (!pc.rearSamples) { pc.rx0 = pc.rx1 = float(rear.report[0].x); pc.ry0 = pc.ry1 = float(rear.report[0].y); }
    ++pc.rearSamples;
    if (used) ++pc.rearUsed;
    if (static_cast<unsigned>(n) > pc.rearMax) pc.rearMax = static_cast<unsigned>(n);
    int ids[8];
    for (int i = 0; i < n; i++) {
        ids[i] = rear.report[i].id;
        bool known = false;
        for (int k = 0; k < pc.rearIdCount; k++) known = known || pc.rearIds[k] == ids[i];
        if (!known) ++pc.rearLandings;
        pc.rx0 = fminf(pc.rx0, float(rear.report[i].x)); pc.rx1 = fmaxf(pc.rx1, float(rear.report[i].x));
        pc.ry0 = fminf(pc.ry0, float(rear.report[i].y)); pc.ry1 = fmaxf(pc.ry1, float(rear.report[i].y));
    }
    memcpy(pc.rearIds, ids, sizeof(int) * static_cast<size_t>(n));
    pc.rearIdCount = n;
}
void pcFront(bool down, float x, float y) {
    if (down) {
        if (!pc.samples[kSrcFront] && !pc.frontLandings) { pc.fx0 = pc.fx1 = x; pc.fy0 = pc.fy1 = y; }
        if (!pc.frontWas) ++pc.frontLandings;
        pc.fx0 = fminf(pc.fx0, x); pc.fx1 = fmaxf(pc.fx1, x);
        pc.fy0 = fminf(pc.fy0, y); pc.fy1 = fmaxf(pc.fy1, y);
    }
    pc.frontWas = down;
}
void pcTick(float dt) {
    ++pc.polls;
    if (pointerLife > 0) ++pc.shown;
    pc.t += dt;
    if (pc.t < 10.0f) return;
    const bool any = pc.moved[0] > 0 || pc.moved[1] > 0 || pc.moved[2] > 0 || pc.moved[3] > 0 || pc.rearSamples || pc.frontLandings || pc.samples[kSrcFront];
    if ((any || !pc.first) && port_log_summary) {
        // (moved: in percent of the picture's width, the pointer's -1..1 being 100 %)
        port_log_summary("pointer: %u polls, on screen %u; moved by: rear pad %d %% (%u polls), right stick %d %% (%u; Circle held %u), menu stick/D-pad %d %% (%u), "
                         "front touch %d %% (%u polls, %u touches, x %d..%d y %d..%d); rear pad reports (aiming %s): %u polls with a contact (%u of them given to the aim), "
                         "most at once %u, %u landings, x %d..%d y %d..%d",
                         pc.polls, pc.shown, static_cast<int>(pc.moved[kSrcRear] * 50.0f), pc.samples[kSrcRear], static_cast<int>(pc.moved[kSrcStick] * 50.0f), pc.samples[kSrcStick],
                         pc.circle, static_cast<int>(pc.moved[kSrcMenu] * 50.0f), pc.samples[kSrcMenu], static_cast<int>(pc.moved[kSrcFront] * 50.0f), pc.samples[kSrcFront],
                         pc.frontLandings, static_cast<int>(pc.fx0), static_cast<int>(pc.fx1), static_cast<int>(pc.fy0), static_cast<int>(pc.fy1),
                         port_rear_aim_enabled() ? "on" : "off", pc.rearSamples, pc.rearUsed, pc.rearMax, pc.rearLandings, static_cast<int>(pc.rx0), static_cast<int>(pc.rx1),
                         static_cast<int>(pc.ry0), static_cast<int>(pc.ry1));
    }
    const bool frontWas = pc.frontWas;
    int ids[8];
    const int idCount = pc.rearIdCount;
    memcpy(ids, pc.rearIds, sizeof ids);
    pc = PointerCensus{};
    pc.first = true;
    pc.frontWas = frontWas;
    pc.rearIdCount = idCount;
    memcpy(pc.rearIds, ids, sizeof ids);
}

void resetTransient() {
    calibrated = pointerValid = touching = touchMoved = menuPointer = tiltWasActive = false;
    neutral = right = forward = {};
    pointerX = pointerY = pointerLife = tapA = touchTime = touchX0 = touchY0 = 0;
    mouseHeld = 0;
    holdPulsed = false;
    rearAimState.Reset();
    zoomArmed = rearToggleArmed = stickToggleArmed = reachToggleArmed = false;
    zoomHeldDir = 0; zoomRepeat = 0;
    blockedButtons = ~0u;
    blockLeftStick = blockRightStick = blockFront = blockRear = true;
}

void rearAim(bool frontDown, float dt) {
    if (!rearStarted) return;
    SceTouchData rear{};
    const bool enabled = !frontDown && port_rear_aim_enabled();
    if (!enabled && !blockRear && (padLean() & 2)) {   // (thr24 GXV_PAD_LEAN bit 2: the same as a poll that saw no contact; no read of the pad)
        ++leanRearSkips;
        rearAimState.Reset();
        return;
    }
    // (crash33b: the pad is read whether aiming is on or not, for the pointer census: a pad that reports contacts nobody makes shows there)
    const bool contact = sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0 && rear.reportNum > 0;
    const int n = contact ? (rear.reportNum < 8 ? rear.reportNum : 8) : 0;
    pcRear(rear, n, enabled && !blockRear);
    port::rear::Contact c[8];
    for (int i = 0; i < n; i++) c[i] = {rear.report[i].id, float(rear.report[i].x), float(rear.report[i].y)};
    if (blockRear) {
        // After an input reset (the Settings menu has closed) the contacts already on the pad never take the pointer; one put
        // down afterwards can. Waiting for an empty pad instead kept rear aiming off while a hand holding the console rested
        // a finger on it.
        blockRear = false;
        rearAimState.Block(c, n);
        return;
    }
    if (!enabled || !contact) {
        rearAimState.Reset();
        return;
    }
    // The pad's coordinate range (the grip bands are fractions of it); the display range where the other is not given.
    port::rear::Rect rect{float(rearPanel.minAaX), float(rearPanel.minAaY), float(rearPanel.maxAaX), float(rearPanel.maxAaY)};
    if (!(rect.x1 > rect.x0) || !(rect.y1 > rect.y0)) rect = {float(rearPanel.minDispX), float(rearPanel.minDispY), float(rearPanel.maxDispX), float(rearPanel.maxDispY)};
    float dx = 0, dy = 0;
    bool show = false, fresh = false;
    const bool aimed = rearAimState.Step(c, n, dt, rect, &dx, &dy, &show, &fresh);
    // One line every 600 samples with a contact (10 s of a hand on the pad): what the rules did.
    if (++rearLogSamples >= 600) {
        const port::rear::Stats& st = rearAimState.stats();
        if (port_log_summary)
            port_log_summary("rear aim: %u samples with a contact (GXV_REAR_STRICT=%d): %u slides took the pointer, %u contacts landed in the grip bands (ignored), "
                             "%u samples with 2+ moving fingers (ignored), %u creeps set back, %u let go at rest, %u jumps not applied; pointer moved %d %% of the picture's width",
                             st.samples, rearStrict ? 1 : 0, st.arms, st.gripLandings, st.multiSamples, st.creeps, st.idleDrops, st.jumps, static_cast<int>(rearLogMoved * 50.0f));
        rearAimState.ClearStats();
        rearLogSamples = 0;
        rearLogMoved = 0;
    }
    if (!aimed) return;
    const float cx0 = pointerX, cy0 = pointerY;
    if (fresh && pointerLife <= 0) pointerX = pointerY = 0; // (a fresh pointer starts in the middle)
    const float panelW = float(rearPanel.maxDispX - rearPanel.minDispX), panelH = float(rearPanel.maxDispY - rearPanel.minDispY);
    const float gainY = kRearGain * (panelW > 0 && panelH > 0 ? (panelW / panelH) * (544.0f / 960.0f) : 1.0f);
    pointerX = clamp(pointerX + dx * kRearGain, -1, 1);
    pointerY = clamp(pointerY + dy * gainY, -1, 1);
    rearLogMoved += fabsf(dx * kRearGain);
    pcMove(kSrcRear, cx0, cy0);
    if (show) pointerLife = kPointerSeconds;
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

// The holding angle: the gravity direction at the moment a tilt level starts steering (or Select is pressed). false: no usable reading.
bool ensureCalibrated(const Vec& gravity) {
    if (calibrated) return true;
    neutral = gravity;
    Vec axis = fabsf(gravity.x) < 0.9f ? Vec{1, 0, 0} : Vec{0, 0, 1};
    float parallel = dot(axis, gravity);
    right = {axis.x - parallel * gravity.x, axis.y - parallel * gravity.y,
             axis.z - parallel * gravity.z};
    if (!normalize(right)) return false;
    forward = cross(right, neutral);
    calibrated = true;
    return true;
}

// GXV_TILT_AUTOSTICK: the raw sensor record (not SceMotionState: its deadband can hold the values constant on a still console) and how far
// the console leans from the grip, in degrees. leanOk false: no reading.
bool readRawAndLean(port::tilt::Sample& out, bool& leanOk) {
    leanOk = false;
    SceMotionSensorState raw{};
    out.rawValid = sceMotionGetSensorState(&raw, 1) >= 0;
    if (out.rawValid) {
        out.raw[0] = raw.accelerometer.x; out.raw[1] = raw.accelerometer.y; out.raw[2] = raw.accelerometer.z;
        out.raw[3] = raw.gyro.x; out.raw[4] = raw.gyro.y; out.raw[5] = raw.gyro.z;
    }
    SceMotionState state{};
    if (sceMotionGetState(&state) < 0) return out.rawValid;
    Vec gravity{state.acceleration.x, state.acceleration.y, state.acceleration.z};
    if (!normalize(gravity) || !ensureCalibrated(gravity)) return out.rawValid;
    out.leanDeg = acosf(clamp(dot(gravity, neutral), -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    leanOk = true;
    return out.rawValid;
}

port::tilt::AutoStick autoStick;
float autoStickLimitSeconds = port::tilt::kFrozenSeconds;

void motion(PortPadState* pad) {
    SceMotionState state{};
    if (!motionStarted || sceMotionGetState(&state) < 0) return;
    Vec gravity{state.acceleration.x, state.acceleration.y, state.acceleration.z};
    if (!normalize(gravity)) return;
    if (!ensureCalibrated(gravity)) return;
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
extern "C" int port_zoom_menu_session_active(void) __attribute__((weak));  // mppause: the session menu's zoom row (Triangle and Select stay the menu's)
static inline bool zoomSessionMenu() { return port_zoom_menu_session_active && port_zoom_menu_session_active(); }

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

// In the pause menu Triangle asks for the native Settings (a press; held, nothing more happens), when the panel's hint says it can open
// (settingsnative: the rear touch, right stick and Star Bit reach hotkeys moved into Settings). The game does not get Select there (the Wii's
// Minus would close the menu), nor L, R and Triangle (unless the player bound them to a menu key: below).
static int openSettingsRequest(void) {
    if (port_settings_hint_visible()) port_settings_open_request();
    return 0;
}
static void toggleButton(unsigned buttons, unsigned button, bool& armed, int (*toggle)(void)) {
    if (!port_zoom_menu_active() || zoomSessionMenu()) {   // (mppause: in the session menu Triangle is Players, not Settings)
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
    if (const char* e = getenv("GXV_MOUSE_AIM")) mouseAim.store(atoi(e) != 0, std::memory_order_relaxed);  // read once; the Settings menu sets it (settings_launch.cpp), default off
    if (const char* e = getenv("PETARI_TOUCH_A")) touchA = atoi(e) != 0;
    if (const char* e = getenv("GXV_VITA_WIDE")) wide = atoi(e) != 0;
    {   // rear touch aiming's rules (port/rear_aim.h): GXV_REAR_STRICT (default 1), GXV_REAR_GRIP_PCT (default 25: each side band, % of the pad's width)
        if (const char* e = getenv("GXV_REAR_STRICT")) rearStrict = !*e || atoi(e) != 0;
        const char* g = getenv("GXV_REAR_GRIP_PCT");
        port::rear::Params p = rearStrict ? port::rear::StrictParams(g && *g ? float(atof(g)) : 25.0f) : port::rear::LegacyParams();
        if (const char* e = getenv("PETARI_REAR_ACCUM")) p.accumulate = atoi(e) != 0;
        rearAimState.Configure(p);
    }
    if (sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG) < 0) return 0;
    autoStickLimitSeconds = port::tilt::ParseFrozenSeconds(getenv("GXV_TILT_AUTOSTICK_S"));
    autoStick.Reset();
    autoStick.SetFrozenSeconds(autoStickLimitSeconds);
    motionStarted = sceMotionStartSampling() >= 0;
    touchStarted = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) >= 0 &&
        panel.maxDispX > panel.minDispX && panel.maxDispY > panel.minDispY &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0;
    rearStarted = sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rearPanel) >= 0 &&
        rearPanel.maxDispX > rearPanel.minDispX && rearPanel.maxDispY > rearPanel.minDispY &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START) >= 0;
    rearAimState.Reset();
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

// bugs26 (12 Nov): GXV_PAD_RAWFALLBACK (default 1). The Settings menu reads the pad through selectRaw; when that fails (the port query errors, or
// the extended read of port 0 returns no sample) the menu showed "Controller disconnected" and answered nothing (KNOWN-ISSUES, "not understood yet").
// On a handheld (port 0 is the physical pad, or the query itself failed) the plain read of port 0 is tried before giving up, and the first fallback
// is logged ("pad: ..."). A PS TV with no controller still reads as disconnected. 0 = the old behaviour.
static bool rawFallback(SceCtrlData& data, PortVitaRawInput& out) {
    static const bool on = [] { const char* e = getenv("GXV_PAD_RAWFALLBACK"); return !e || !*e || atoi(e) != 0; }();
    if (!on) return false;
    using namespace port::controls;
    SceCtrlPortInfo info{};
    const int q = sceCtrlGetControllerPortInfo(&info);
    if (q >= 0 && info.port[0] != SCE_CTRL_TYPE_PHY) return false;
    if (sceCtrlPeekBufferPositive(0, &data, 1) <= 0) return false;
    out.source = 0; out.controllerType = SCE_CTRL_TYPE_PHY; out.timestamp = data.timeStamp;
    out.caps = NominalCaps(ProfileId::Handheld);
    out.caps.frontTouch = touchStarted;
    out.caps.rearTouch = rearStarted;
    out.caps.motion = motionStarted;
    out.caps.tiltFallback = !out.caps.motion;
    out.held = normalizeButtons(data.buttons, false) & out.caps.availableButtons;
    static bool logged = false;
    if (!logged && port_log_summary) { logged = true; port_log_summary("pad: the raw pad read (port query %d) failed; the plain read of port 0 is used for the menu (GXV_PAD_RAWFALLBACK=0 turns this off)", q); }
    return true;
}

bool port_vita_input_raw_poll(PortVitaRawInput& out) {
    out = {};
    SceCtrlData data{};
    if (!selectRaw(data, out) && !rawFallback(data, out)) {
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

// settingsnative, "Change Buttons" capture: the buttons held on the controller in use at the last poll, as the port's stable bits (Cross = 1 ... Right =
// 1 << 15, port/control_bindings.h), before any mapping. The menu reads it each frame (never the SDK: the game's poll owns that).
extern "C" uint64_t port_vita_input_held_buttons(void) { return heldNormalized.load(std::memory_order_relaxed); }
// settingsnative: Settings > Controls > Pointer. 0 = Steady (the stick moves the pointer at a fixed speed), 1 = Quick (mouse-style aiming).
extern "C" int port_pointer_quick_enabled(void) { return mouseAim.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" int port_pointer_quick_set(int on) { mouseAim.store(on != 0, std::memory_order_relaxed); return on != 0; }

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
    // settingsnative: a button swap the Settings staged (platform/src/input/control_bindings.cpp) takes effect here, between two polls, and the
    // held state starts fresh so no button carries over from the old mapping.
    if (port::controls::CommitStagedMapping()) {
        resetTransient();
        if (port_handheld_pictures_changed) port_handheld_pictures_changed();   // the buttons drawn on signs and prompts follow the new mapping
    }
    *pad = {};
    SceCtrlData control{};
    PortVitaRawInput sampled;
    const bool remap = port::controls::RemapEnabled();
    bool ok = remap ? selectRaw(control, sampled, true) : sceCtrlPeekBufferPositive(0, &control, 1) > 0;
    if (!ok && remap && portCache.age == kPortCachePolls && (padLean() & 1)) ok = selectRaw(control, sampled, true);   // (thr24: a cached port failed: once more with a fresh query)
    if (!ok) { heldNormalized.store(0, std::memory_order_relaxed); resetTransient(); gameSource = -1; gameType = 0; mappingAvailable.store(false); return; }
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
        heldNormalized.store(normalized, std::memory_order_relaxed);   // settingsnative: the capture hook (port_vita_input_held_buttons)
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
            normalized &= ~((zoomSessionMenu() ? (L1 | R1) : (L1 | R1 | Triangle | Select)) & ~menuKeys);
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
        if (!remap && zoomMenu && (button.vita == SCE_CTRL_LTRIGGER || (!zoomSessionMenu() && (button.vita == SCE_CTRL_TRIANGLE || button.vita == SCE_CTRL_SELECT)))) continue;
        if (control.buttons & button.vita) pad->buttons |= button.wii;
    }
    stick(control.lx, control.ly, pad->stickX, pad->stickY);
    float rx, ry;
    stick(control.rx, control.ry, rx, ry);
    if (!isfinite(elapsedSeconds)) elapsedSeconds = 0;
    float dt = clamp(elapsedSeconds, 0, 0.05f);
    zoomButtons(commandButtons, dt);
    toggleButton(commandButtons, SCE_CTRL_TRIANGLE, rearToggleArmed, openSettingsRequest);   // settingsnative: was the rear touch hotkey (Square: stick, Select: reach, are gone)
    const bool aiming = (control.buttons & SCE_CTRL_CIRCLE) != 0;
    const bool mouseAimNow = mouseAim.load(std::memory_order_relaxed);
    if (mouseAimNow != mouseAimSeen) { mouseAimSeen = mouseAimNow; mouseHeld = 0; }   // settingsnative: a flip starts the quick pointer from rest
    const bool stickPointerSetting = !rstickCamera && port_stick_pointer_enabled();  // (the pause menu's Right Stick: Pointer)
    if (aiming) ++pc.circle;
    if (rstickPointer || padPointer || stickPointerSetting || aiming) {
        const float cx0 = pointerX, cy0 = pointerY;
        if (aiming && pointerLife <= 0) pointerX = pointerY = 0; // (a fresh aim starts from the middle)
        if (mouseAimNow) {
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
        pcMove(kSrcStick, cx0, cy0);
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
        const float cx0 = pointerX, cy0 = pointerY;
        if (pointerLife <= 0 && !menuPointer) pointerX = pointerY = 0; // (first shown in the middle)
        pointerX = clamp(pointerX + mx * dt * 1.5f, -1, 1);
        pointerY = clamp(pointerY - my * dt * 1.5f, -1, 1);
        pcMove(kSrcMenu, cx0, cy0);
        pointerLife = kPointerSeconds;
        menuPointer = true;
    } else {
        menuPointer = false;
    }
    SceTouchData touch{};
    const bool contact = touchStarted && (!remap || sampled.caps.frontTouch) && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum;
    if (blockFront && !contact) blockFront = false;
    const bool down = contact && !blockFront;
    const bool tapJump = touchA && port_tap_jump_enabled();   // settingsnative: Settings > Controls > Tap to Jump (PETARI_TOUCH_A=0 still wins)
    pad->touched = down ? 1 : 0;
    if (down) {
        // The pointer's -1..1 spans the game's picture: the middle 4:3 part of the screen unless
        // the picture is stretched.
        const float u = (touch.report[0].x - panel.minDispX) / float(panel.maxDispX - panel.minDispX); // 0..1 across the screen
        const float v = (touch.report[0].y - panel.minDispY) / float(panel.maxDispY - panel.minDispY);
        const float pictureWidth = wide ? 1.0f : (544.0f * 4.0f / 3.0f) / 960.0f;
        pointerLife = kPointerSeconds;
        const float cx0 = pointerX, cy0 = pointerY;
        pointerX = clamp((u - 0.5f) / (0.5f * pictureWidth), -1, 1);
        pointerY = clamp(2.0f * v - 1.0f, -1, 1);
        pcMove(kSrcFront, cx0, cy0);
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
        if (tapJump && !touchMoved && touchTime >= 0.20f) {
            if (tapCleanOn() && talkOrDemoOnScreen()) {
                if (!holdPulsed) { holdPulsed = true; tapA = 0.10f; }  // one press, not a hold
            } else {
                pad->buttons |= 0x0800; // a finger held still: A held (pull stars)
            }
        }
    } else if (touching) {
        touching = false;
        if (tapJump && !touchMoved && touchTime < 0.20f) tapA = 0.10f; // a tap: A where it landed
    }
    pcFront(contact, contact ? float(touch.report[0].x) : 0.0f, contact ? float(touch.report[0].y) : 0.0f);
    // the rear pad (if chosen): in play only, never with a finger on the front screen
    rearAim(contact || menuPointer || (remap && !sampled.caps.rearTouch), dt);
    if (tapA > 0) {
        pad->buttons |= 0x0800;
        tapA -= dt;
    }
    pcTick(dt);
    if (pointerLife > 0) pointerLife -= dt;
    pointerValid = pointerLife > 0;
    pad->pointerValid = pointerValid;
    if (padLean() && ++leanPolls >= 1800 && port_log_summary) {   // (thr24: about every 10-20 s of polling: what GXV_PAD_LEAN saved)
        port_log_summary("pad-lean: GXV_PAD_LEAN=%d: %u polls: port queries %u (answered from the cache %u), rear pad reads skipped %u, motion sensor reads skipped %u",
                         padLean(), leanPolls, leanPortQueries, leanPortHits, leanRearSkips, leanMotionSkips);
        leanPolls = leanPortQueries = leanPortHits = leanRearSkips = leanMotionSkips = 0;
    }
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
    // GXV_TILT_AUTOSTICK: with the setting on "Sensor", a sensor that is present but dead hands the tilt levels to the left stick (rules in
    // platform/include/port/tilt_autostick.h). The saved setting is never changed; the one-line log says when and why.
    bool autoStickNow = false;
    const bool sensorSource = remap && sampled.caps.motion && !port::tilt::UseStick();
    if (sensorSource && port::tilt::AutoStickEnabled()) {
        const bool tiltLevel = port_input_tilt_active() != 0;
        port::tilt::Sample sample;
        bool leanOk = false;
        if (tiltLevel) readRawAndLean(sample, leanOk);
        sample.stickX = pad->stickX; sample.stickY = pad->stickY;
        int event = 0;
        autoStickNow = autoStick.Step(dt, tiltLevel, true, sample, &event);
        if (event && port_log_summary) {
            if (event == 1) port_log_summary("tilt: the motion sensor's raw readings did not change for %.0f s while the left stick was moved in a tilt level: the left stick steers from now on (setting unchanged; GXV_TILT_AUTOSTICK=0 turns this off)", (double)autoStickLimitSeconds);
            else if (event == 2) port_log_summary("tilt: left stick held full for %.1f s with no response from the motion sensor: the left stick steers for this level (setting unchanged)", (double)port::tilt::kPushSeconds);
            else port_log_summary("tilt: the motion sensor answered again: the sensor steers");
        }
    }
    if (!remap || (sensorSource && !autoStickNow)) {
        if ((padLean() & 4) && !port_input_tilt_active()) ++leanMotionSkips;   // (thr24 GXV_PAD_LEAN bit 4: nothing reads the tilt outside a tilt level, see padLean)
        else motion(pad);
    }
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
