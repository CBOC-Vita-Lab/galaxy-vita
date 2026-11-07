// Does one press of Square (or R) give exactly one spin?  The production chain is run end to end on the host: the Vita poll
// (vita/runtime/input.cpp) -> port_input_set -> KPADRead (platform/src/input/wpad.cpp) -> the decompilation's own
// WPadAcceleration (the acceleration history) and WPadHVSwing (the swing detector), both compiled unchanged, -> Mario's
// pad-swing rule (MarioActor::updateControllerSwing, transcribed below; tools/test_spin_detect.sh checks the transcription
// against the decompilation) -> the spin interval (_946, set when a spin starts: mSpinIntervalTime 45 + 34 updates).
//
// One KPADRead per game update (WPadHolder::update), so every length below is in game updates, whatever the picture rate.
// Arguments: remap (0/1), shake latch (0/1), pulse shaping (0/1, GXV_SHAKE_PULSE).
#include "input.h"
#include "Game/System/WPad.hpp"
#include "Game/System/WPadAcceleration.hpp"
#include "Game/System/WPadHVSwing.hpp"
#include "revolution/kpad.h"
#include <psp2/ctrl.h>
#include <psp2/motion.h>
#include <psp2/touch.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ---- the SDK calls the Vita input layer makes (no controller, no motion, no touch) ----
static SceCtrlData raw;
static int type = SCE_CTRL_TYPE_PHY;
extern "C" {
int sceCtrlSetSamplingMode(SceCtrlPadInputMode) { return 0; }
int sceCtrlPeekBufferPositive(int, SceCtrlData* out, int) { *out = raw; return 1; }
int sceCtrlPeekBufferPositive2(int, SceCtrlData* out, int) {
    *out = raw;
    if (type == SCE_CTRL_TYPE_PHY) {
        out->buttons &= ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
        if (raw.buttons & SCE_CTRL_LTRIGGER) out->buttons |= SCE_CTRL_L1;
        if (raw.buttons & SCE_CTRL_RTRIGGER) out->buttons |= SCE_CTRL_R1;
    }
    return 1;
}
int sceCtrlGetControllerPortInfo(SceCtrlPortInfo* out) {
    *out = {}; out->port[0] = type == SCE_CTRL_TYPE_PHY ? type : SCE_CTRL_TYPE_VIRT;
    if (type != SCE_CTRL_TYPE_PHY) out->port[1] = type;
    return 0;
}
int sceMotionStartSampling() { return -1; }
int sceMotionStopSampling() { return 0; }
int sceMotionGetState(SceMotionState*) { return -1; }
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo* out) {
    *out = {}; out->maxDispX = 1919; out->maxDispY = port == SCE_TOUCH_PORT_FRONT ? 1087 : 889; return 0;
}
int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
int sceTouchPeek(SceUInt32, SceTouchData* out, SceUInt32) { *out = SceTouchData{}; return 1; }
int port_pointer_on_menus() { return 0; }
int port_zoom_menu_active() { return 0; }
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
unsigned port_mtx_profile_begin(const void*) { return 0; }  // (mtx.c's profile hooks)
void port_mtx_profile_end(unsigned, unsigned) {}
}

// ---- the stand-in WPad: the game's own detector sources run against it ----
KPADStatus* WPad::getKPadStatus(u32 i) const { return static_cast<s32>(i) < mValid ? const_cast<KPADStatus*>(&mStatus[i]) : nullptr; }
s32 WPad::getValidStatusCount() const { return mValid; }
s32 WPad::getEnableAccelPastCount(u32 dev) const { return (dev == WPAD_DEV_CORE ? mCorePadAccel : mSubPadAccel)->_628; }
bool WPad::getAcceleration(TVec3f* o, u32 dev) const { return (dev == WPAD_DEV_CORE ? mCorePadAccel : mSubPadAccel)->getAcceleration(o); }
bool WPad::getPastAcceleration(TVec3f* o, s32 i, u32 dev) const { return (dev == WPAD_DEV_CORE ? mCorePadAccel : mSubPadAccel)->getPastAcceleration(o, i); }
bool MR::isDeviceFreeStyle(const KPADStatus* s) { return s && s->wpad_err == WPAD_ERR_NONE && s->dev_type == WPAD_DEV_FREESTYLE; }

namespace {
constexpr int kSpinInterval = 45 + 34;  // MarioActor::_946 after a spin: mSpinIntervalTime (MarioConst) + 0x22

struct Game {
    WPad pad;
    WPadAcceleration* acc;
    WPadHVSwing* swing;
    // MarioActor's pad-swing state
    int f1c = 0x3C;      // _F1C (MarioActor::init)
    bool f20 = false;    // _F20
    bool f00 = false;    // _F00: a swing request this update
    int interval = 0;    // _946
    int spins = 0;
    int requests = 0;
    long long usec = 0;  // the Vita controller clock
    int pollsPerUpdate = 4;
    long long updateUsec = 16667;

    Game(long long startUsec) : usec(startUsec) {
        acc = new WPadAcceleration(&pad, WPAD_DEV_CORE);
        swing = new WPadHVSwing(&pad, WPAD_DEV_CORE);
        pad.mCorePadAccel = acc;
        pad.mSubPadAccel = acc;
    }
    ~Game() { delete acc; delete swing; }

    // One game update: the poll thread runs between updates, then the game reads the pad (WPadHolder::update, WPad::update),
    // then Mario's pad rule and the spin start.
    void update(unsigned extraButtons = 0, bool swimming = false) {
        PortPadState ps;
        const unsigned held = raw.buttons;
        raw.buttons |= extraButtons;  // (Cross for an A press, Circle for B, down for this update only)
        for (int i = 0; i < pollsPerUpdate; ++i) {
            usec += updateUsec / pollsPerUpdate;
            raw.timeStamp = static_cast<uint64_t>(usec);
            port_vita_input_poll(1.0f / 60 / pollsPerUpdate, &ps);
            port_input_set(0, &ps);
        }
        raw.buttons = held;
        pad.mValid = KPADRead(0, pad.mStatus, 120);
        acc->update();
        swing->update();
        // MarioActor::updateControllerSwing (decomp/src/Game/Player/MarioActorPad.cpp), checkButtonType(3/4) = A or B trigger
        if (pad.mValid > 0 && (pad.mStatus[0].trig & 0x0C00)) f1c = 10;
        const bool prev = f20;
        f00 = false;
        const bool isCorePadSwing = !swimming ? swing->mIsSwing : swing->mIsTriggerSwing;
        f20 = isCorePadSwing;
        if (f1c != 0) {
            --f1c;
        } else if (isCorePadSwing && !prev) {
            f00 = true;
        }
        if (isCorePadSwing && interval > 10 && f1c < 8) f1c = 8;
        // the spin starts when requested and the interval has run out (MarioActor::updateSwingAction: _946 gates it)
        if (interval) --interval;
        if (f00) {
            ++requests;
            if (!interval) { ++spins; interval = kSpinInterval; }
        }
    }
    // Controller polls only (the poll thread runs, the game does not read: a long turn).
    void poll(unsigned buttons) {
        PortPadState ps;
        raw.buttons = buttons;
        usec += updateUsec / pollsPerUpdate;
        raw.timeStamp = static_cast<uint64_t>(usec);
        port_vita_input_poll(1.0f / 60 / pollsPerUpdate, &ps);
        port_input_set(0, &ps);
        raw.buttons = 0;
    }
    void rest(int n) { raw.buttons = 0; for (int i = 0; i < n; ++i) update(); }
    // Square held for `n` updates, then released; returns the spins it gave (counting on until `settle` updates after).
    int press(unsigned button, int n, int settle = 150, bool swimming = false) {
        const int before = spins;
        raw.buttons = button;
        for (int i = 0; i < n; ++i) update(0, swimming);
        raw.buttons = 0;
        for (int i = 0; i < settle; ++i) update(0, swimming);
        return spins - before;
    }
};
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 4);
    const bool remap = std::atoi(argv[1]) != 0, latch = std::atoi(argv[2]) != 0, pulse = std::atoi(argv[3]) != 0;
    setenv("GXV_REMAP", remap ? "1" : "0", 1);
    setenv("GXV_SHAKE_LATCH", latch ? "1" : "0", 1);
    setenv("GXV_SHAKE_PULSE", pulse ? "1" : "0", 1);
    raw.lx = raw.ly = raw.rx = raw.ry = 128;
    assert(port_vita_input_init());
    const unsigned square = SCE_CTRL_SQUARE, rbutton = SCE_CTRL_RTRIGGER;
    int failures = 0;
    auto check = [&](bool ok, const char* what, long long a, long long b, long long c) {
        if (!ok) { ++failures; if (failures <= 40) std::printf("  FAIL %s (%lld, %lld, %lld)\n", what, a, b, c); }
    };
    long cases = 0;
    // Checks that only the pulse shaping meets (the echo of an unshaped swing gives a late second spin, and a spin 20 updates late
    // after A or B): counted as notes without it.
    int notes = 0;
    auto shaped = [&](bool ok, const char* what, long long a, long long b, long long c) {
        if (pulse) check(ok, what, a, b, c);
        else if (!ok) ++notes;
    };

    // A. One tap of every length 1..10 updates, 60 updates a second, every phase of the controller clock against the 16 ms
    // shake sign (steps of 1 ms over its 32 ms period), Square and R: exactly one spin each.
    for (unsigned button : {square, rbutton}) {
        for (int phase = 0; phase < 32; ++phase) {
            for (int len = 1; len <= 10; ++len) {
                Game g(1000000 + phase * 1000);
                g.rest(60);
                const int spins = g.press(button, len);
                ++cases;
                check(spins == 1, "60/s tap: spins", phase, len, spins);
            }
        }
    }
    // B. The same at 30 updates a second (a locked-30 run: the game's updates are 33 ms apart) for taps of 1..6 updates.
    for (int phase = 0; phase < 32; ++phase) {
        for (int len = 1; len <= 6; ++len) {
            Game g(1000000 + phase * 1000);
            g.updateUsec = 33333;
            g.rest(60);
            const int spins = g.press(square, len);
            ++cases;
            check(spins == 1, "30/s tap: spins", phase, len, spins);
        }
    }
    // C. A tap that comes right after A or B (the game's own cooldown of 10 updates after either): the spin is not lost; it
    // comes once the cooldown has run out.  With the pulse shaping it comes at the first update the game takes it (10 updates
    // after the trigger); without, from the echo of the swing 20 updates after the press.
    for (unsigned other : {unsigned(SCE_CTRL_CROSS), unsigned(SCE_CTRL_CIRCLE)}) {
        for (int gap = 0; gap <= 14; ++gap) {
            for (int len = 1; len <= 10; ++len) {
                Game g(1000000 + 7000 * gap);
                g.rest(60);
                g.update(other);             // A (or B) pressed for one update
                for (int i = 0; i < gap; ++i) g.update();
                raw.buttons = square;
                int at = -1;
                for (int i = 0; i < 150; ++i) {
                    if (i == len) raw.buttons = 0;
                    const int before = g.spins;
                    g.update();
                    if (at < 0 && g.spins > before) at = i;
                }
                raw.buttons = 0;
                ++cases;
                check(g.spins == 1, "after A/B: spins", gap, len, g.spins);
                const int soonest = gap <= 9 ? 9 - gap : 0;
                shaped(at == soonest, "after A/B: update of the spin", gap, len, at);
                check(at == soonest || at == 20, "after A/B: update of the spin (at the soonest, or from the echo)", gap, len, at);
            }
        }
    }
    // C2. A press that ends before the next game update (a long turn: a stage load, a first draw), at every poll of the update.
    for (int down = 1; down <= 3; ++down) {
        for (int at = 0; at <= 3; ++at) {
            for (int quiet : {0, 12}) {  // quiet: polls with nothing pressed first, as long turns have
                Game g(1000000 + 3000 * at);
                g.rest(60);
                const int before = g.spins;
                for (int i = 0; i < quiet + at; ++i) g.poll(0);
                for (int i = 0; i < down; ++i) g.poll(square);
                for (int i = 0; i < 4 + quiet; ++i) g.poll(0);
                for (int i = 0; i < 150; ++i) g.update();
                ++cases;
                const int spins = g.spins - before;
                if (latch || pulse) check(spins == 1, "press between two reads: spins", down, at, spins);
                else if (spins != 1) ++notes;
            }
        }
    }
    // C3. A second tap after d updates: one more spin when the spin interval (79 updates) was over, none when not.
    for (int d = 1; d <= 140; ++d) {
        Game g(1000000 + 997 * d);
        g.rest(60);
        int spins = g.press(square, 3, d - 3 > 0 ? d - 3 : 0);
        spins += g.press(square, 3, 150);
        ++cases;
        if (d >= 85) check(spins == 2, "two taps: second spin after the interval", d, spins, 0);
        else if (d <= 55) check(spins == 1, "two taps: second tap inside the interval", d, spins, 0);
        else if (d <= 70) shaped(spins == 1, "two taps: second tap inside the interval", d, spins, 0);
    }
    // D. Under water the game takes the swing's trigger, not its level.
    for (int len = 1; len <= 10; ++len) {
        Game g(1000000);
        g.rest(60);
        const int spins = g.press(square, len, 150, true);
        ++cases;
        check(spins == 1, "swimming tap: spins", len, 0, spins);
    }
    // E. A press held for a long time: the first spin at once, and no more than one more per spin interval.
    for (int len : {20, 40, 79, 120, 200}) {
        Game g(1000000);
        g.rest(60);
        const int spins = g.press(square, len, 150);
        ++cases;
        check(spins >= 1 && spins <= 1 + len / kSpinInterval, "long press: spins", len, spins, 0);
    }
    // F. Taps one after another, each one's spin interval over before the next: one spin each.
    {
        Game g(1000000);
        g.rest(60);
        int total = 0;
        for (int n = 0; n < 12; ++n) { total += g.press(square, 1 + n % 5, kSpinInterval + 5 + n); }
        ++cases;
        check(total == 12, "tap series: spins", total, 12, 0);
    }
    // G. The press reaches the game on the very update after it began, a touch late at most (the swing is seen by then).
    for (int len = 1; len <= 10; ++len) {
        Game g(1000000);
        g.rest(60);
        raw.buttons = square;
        int first = -1;
        for (int i = 0; i < 12; ++i) { g.update(); if (first < 0 && g.f00) first = i; }
        raw.buttons = 0;
        ++cases;
        check(first >= 0 && first <= 1, "first update of the request", len, first, 0);
        (void)len;
    }
    if (std::getenv("SPIN_DETECT_TRACE")) {
        // how many updates after the press the spin starts, for a tap of 1 update `gap` updates after A
        for (int gap = 0; gap <= 14; gap += 2) {
            Game g(1000000);
            g.rest(60);
            g.update(SCE_CTRL_CROSS);
            for (int i = 0; i < gap; ++i) g.update();
            raw.buttons = square;
            int at = -1;
            for (int i = 0; i < 60; ++i) {
                if (i == 1) raw.buttons = 0;
                const int before = g.spins;
                g.update();
                if (at < 0 && g.spins > before) at = i;
            }
            std::printf("A, %2d updates, then a 1-update tap: spin starts %d updates after the press\n", gap, at);
        }
        for (int len : {20, 79, 120, 200, 400}) {
            Game g(1000000);
            g.rest(60);
            std::printf("held %d updates: %d spins\n", len, g.press(square, len, 150));
        }
        for (int len : {1, 2, 5, 10, 25}) {
            Game g(1000000);
            g.rest(60);
            raw.buttons = square;
            std::printf("len %d: ", len);
            for (int i = 0; i < 100; ++i) {
                if (i == len) raw.buttons = 0;
                g.update();
                std::printf("%c", g.f00 ? 'R' : (g.swing->mIsSwing ? 's' : '.'));
            }
            std::printf("\n");
        }
    }
    port_vita_input_shutdown();
    std::printf("%s: spin per press remap=%d latch=%d pulse=%d: %ld cases, %d failures%s\n", failures ? "FAIL" : "PASS", remap, latch, pulse, cases, failures,
                notes ? (std::string(", ") + std::to_string(notes) + " the unshaped swing does not meet (a spin from the echo of the swing, late or twice)").c_str() : "");
    return failures ? 1 : 0;
}
