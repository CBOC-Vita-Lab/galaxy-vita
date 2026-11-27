// relfix24: platform/include/port/tilt_autostick.h on synthetic sensor traces: a dead sensor, a still console with a working (noisy)
// sensor, a moving one, the stage and stick conditions, the level-only rule B and its release, the off switch.
#include "port/tilt_autostick.h"
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>

using namespace port::tilt;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned rng = 12345;
static float noise() { rng = rng * 1664525u + 1013904223u; return ((rng >> 16) & 7) * 0.00006f; }  // a few LSBs of ADC noise

struct Trace {
    const char* name;
    // fills the sample for frame i
    void (*fill)(int i, Sample& s);
};
static void deadZero(int, Sample& s) { s.rawValid = true; for (float& v : s.raw) v = 0; }
static void deadConst(int, Sample& s) { s.rawValid = true; const float c[6] = {0.01f, -0.98f, 0.12f, 0, 0, 0}; std::memcpy(s.raw, c, sizeof c); }
static void stillNoisy(int, Sample& s) { s.rawValid = true; const float c[6] = {0.01f, -0.98f, 0.12f, 0, 0, 0}; for (int k = 0; k < 6; k++) s.raw[k] = c[k] + noise(); }
static void moving(int i, Sample& s) { s.rawValid = true; for (int k = 0; k < 6; k++) s.raw[k] = 0.01f * (float)((i * (k + 3)) % 97); }

// runs `seconds` of 60 Hz frames; returns the time (s) at which the stick took over, or -1
static float run(AutoStick& a, void (*fill)(int, Sample&), float seconds, bool tiltLevel, float stick, float lean, bool sensorSource = true) {
    const float dt = 1.0f / 60.0f;
    const int n = (int)(seconds * 60);
    for (int i = 0; i < n; i++) {
        Sample s; fill(i, s); s.stickX = stick; s.stickY = 0; s.leanDeg = lean;
        if (a.Step(dt, tiltLevel, sensorSource, s)) return (float)i * dt;
    }
    return -1;
}

int main() {
    // 1. dead sensor (all zero, or a constant record), stick moved in a tilt level: A latches at about 4 s, for the session
    for (auto fill : {deadZero, deadConst}) {
        AutoStick a; int ev = 0;
        const float t = run(a, fill, 10, true, 0.7f, 0.0f);
        CHECK(t > 3.9f && t < 4.2f);
        CHECK(a.SessionLatched() && a.LastReason() == AutoStick::Frozen);
        // it stays: out of the level, in the next level, with the stick at rest
        Sample s; fill(0, s);
        CHECK(a.Step(0.016f, false, true, s, &ev));
        CHECK(a.Step(0.016f, true, true, s, &ev));
        s.stickX = 0; CHECK(a.Step(0.016f, true, true, s, &ev));
    }
    // 2. a still console with a working sensor: its noise changes the record every few frames; an hour of a moved stick never latches
    {
        AutoStick a; CHECK(run(a, stillNoisy, 3600, true, 0.8f, 0.0f) < 0 || false);
        CHECK(!a.SessionLatched());
    }
    // 3. a moving console: never
    { AutoStick a; CHECK(run(a, moving, 600, true, 1.0f, 20.0f) < 0); CHECK(!a.SessionLatched()); }
    // 4. a record that changes only once every 3.9 s (slower than any real sensor) still does not latch; once every 4.1 s does
    {
        for (float gap : {3.9f, 4.1f}) {
            AutoStick a; const float dt = 1.0f / 60.0f; bool latched = false; float acc = 0; float v = 0;
            for (int i = 0; i < 60 * 60 && !latched; i++) {
                acc += dt; if (acc >= gap) { acc = 0; v += 0.001f; }
                Sample s; s.rawValid = true; for (float& x : s.raw) x = 0; s.raw[0] = v; s.stickX = 0.7f; s.leanDeg = 0;
                latched = a.Step(dt, true, true, s);
            }
            CHECK(latched == (gap > 4.0f));
        }
    }
    // 5. conditions: no latch without the tilt level, without the stick moved, with an unreadable record, or when the stick is the source anyway
    { AutoStick a; CHECK(run(a, deadZero, 30, false, 0.9f, 0) < 0); CHECK(!a.SessionLatched()); }
    { AutoStick a; CHECK(run(a, deadZero, 30, true, 0.0f, 0) < 0); CHECK(!a.SessionLatched()); }
    { AutoStick a; CHECK(run(a, deadZero, 30, true, 0.3f, 0) < 0); }  // below kStickMoved
    { AutoStick a; auto bad = [](int, Sample& s) { s.rawValid = false; }; CHECK(run(a, bad, 30, true, 0.7f, 0) < 0); }
    { AutoStick a; CHECK(run(a, deadZero, 30, true, 0.9f, 0, false) < 0); CHECK(!a.SessionLatched()); }
    // 6. only the time with the stick moved counts: 3 s moved, then a long rest, then 3 s moved: total 6 s of moved stick, so it latches in the second part
    {
        AutoStick a; CHECK(run(a, deadZero, 3, true, 0.7f, 0) < 0); CHECK(run(a, deadZero, 60, true, 0.0f, 0) < 0);
        CHECK(run(a, deadZero, 3, true, 0.7f, 0) > 0);
    }
    // 6b. but any change in between starts again
    {
        AutoStick a; CHECK(run(a, deadZero, 3, true, 0.7f, 0) < 0);
        Sample s; s.rawValid = true; s.raw[0] = 1; s.stickX = 0.7f; CHECK(!a.Step(0.016f, true, true, s));
        CHECK(run(a, deadZero, 3.5f, true, 0.7f, 0) < 0);
    }
    // 7. rule B: stick full, no lean for 1.5 s while the raw record keeps changing (a working-looking sensor): level only, and release
    {
        AutoStick a; int ev = 0;
        const float t = run(a, stillNoisy, 5, true, 1.0f, 0.5f);
        CHECK(t > 1.45f && t < 1.6f);
        CHECK(a.LevelLatched() && !a.SessionLatched() && a.LastReason() == AutoStick::NoResponse);
        Sample s; stillNoisy(0, s); s.stickX = 1; s.leanDeg = 5;  // between the two thresholds: stays
        CHECK(a.Step(0.016f, true, true, s, &ev));
        s.leanDeg = 12;  // the sensor answers: the sensor takes over again
        CHECK(!a.Step(0.016f, true, true, s, &ev) && ev == 3 && !a.LevelLatched());
        // out of the level it would have ended too
        AutoStick b; run(b, stillNoisy, 3, true, 1.0f, 0.5f); CHECK(b.LevelLatched());
        Sample s2; stillNoisy(0, s2); CHECK(!b.Step(0.016f, false, true, s2)); CHECK(!b.LevelLatched());
    }
    // 7b. rule B does not fire when the player leans the console, with the stick partly pushed, or for less than 1.5 s
    { AutoStick a; CHECK(run(a, stillNoisy, 10, true, 1.0f, 6.0f) < 0); }
    { AutoStick a; CHECK(run(a, stillNoisy, 10, true, 0.8f, 0.5f) < 0); }
    { AutoStick a; CHECK(run(a, stillNoisy, 1.4f, true, 1.0f, 0.5f) < 0); CHECK(run(a, stillNoisy, 1.0f, true, 0.0f, 0.5f) < 0); CHECK(run(a, stillNoisy, 1.4f, true, 1.0f, 0.5f) < 0); }
    // 8. event values on the frames that latch
    { AutoStick a; int ev = 0; Sample s; deadZero(0, s); s.stickX = 0.7f; bool on = false; int seen = 0;
      for (int i = 0; i < 600 && !on; i++) { on = a.Step(1.0f / 60, true, true, s, &ev); if (ev) seen = ev; }
      CHECK(on && seen == 1); }
    // 9. the seconds switch and the env parse
    CHECK(ParseFrozenSeconds(nullptr) == kFrozenSeconds && ParseFrozenSeconds("") == kFrozenSeconds && ParseFrozenSeconds("6") == 6.0f);
    CHECK(ParseFrozenSeconds("0") == kFrozenSeconds && ParseFrozenSeconds("99") == kFrozenSeconds && ParseFrozenSeconds("x") == kFrozenSeconds);
    { AutoStick a; a.SetFrozenSeconds(8); const float t = run(a, deadZero, 20, true, 0.7f, 0); CHECK(t > 7.9f && t < 8.2f); }
    // 10. the switch: unset = on, 0 = off (read once per process: the shell script runs this binary with each value)
    if (std::getenv("EXPECT_AUTOSTICK")) CHECK(AutoStickEnabled() == (std::atoi(std::getenv("EXPECT_AUTOSTICK")) != 0));
    std::printf(fails ? "FAILED %d\n" : "PASS tilt_autostick\n", fails);
    return fails ? 1 : 0;
}
