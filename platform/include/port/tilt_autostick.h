// GXV_TILT_AUTOSTICK (relfix24, 10 Nov; default 1, 0 = off): the left stick steers the tilt levels when the motion sensor is present
// but dead. The Vita reports a dead gyro / accelerometer as present, so the source stayed "Sensor" and the player had to find
// LiveArea > Settings > Controls > "Tilt with: Left stick" (report G06). Two rules, both only inside a tilt level (port_input_tilt_active)
// and only while the setting is "Sensor"; neither ever writes the saved setting.
//
//  A. Frozen raw readings. The raw sensor record (sceMotionGetSensorState: accelerometer x y z, gyro x y z) is compared with the one
//     before. A working sensor never repeats the whole record for long: its ADC noise flips at least one of the six channels within a
//     few samples, also on a console lying on a table (a MEMS accelerometer has a noise floor of several least-significant bits; the
//     SceMotionState values are NOT used, because the SDK documents that their deadband filters small movements out and so can be
//     perfectly constant on a still console). Time counts only while the player moves the left stick in a tilt level, and any change of
//     any channel sets it back to zero. kFrozenSeconds (default 4) of that and the stick steers for the rest of the session.
//  B. No response to a full stick. The player holds the stick (nearly) fully for kPushSeconds (1.5) while the lean the sensor reports
//     stays under kLeanDeg (3 degrees). The stick steers for the rest of that tilt level only, and the lean is still watched: when it
//     passes kBackDeg (8) the sensor answers after all and the sensor takes over again. (A player holding the console still and the
//     stick full for 1.5 s in a tilt level is rare, and only loses the sensor for the level.)
//
// Plain state and a Step() on numbers, no SDK types: host-tested (tests/tilt_autostick.cpp).
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace port {
namespace tilt {

constexpr float kFrozenSeconds = 4.0f;  // rule A (GXV_TILT_AUTOSTICK_S overrides, 1..30)
constexpr float kPushSeconds = 1.5f;    // rule B
constexpr float kStickMoved = 0.4f;     // a stick counts as moved for rule A above this deflection
constexpr float kStickFull = 0.9f;      // ... and as full for rule B
constexpr float kLeanDeg = 3.0f;        // rule B: no response below this lean
constexpr float kBackDeg = 8.0f;        // rule B: the sensor answers above this lean

// GXV_TILT_AUTOSTICK, read once: unset or anything but 0 is on.
inline bool AutoStickEnabled() {
    static const bool on = [] { const char* e = std::getenv("GXV_TILT_AUTOSTICK"); return !e || std::atoi(e) != 0; }();
    return on;
}
inline float ParseFrozenSeconds(const char* v) {
    if (!v || !*v) return kFrozenSeconds;
    char* end = nullptr;
    const long s = std::strtol(v, &end, 10);
    if (end == v || *end || s < 1 || s > 30) return kFrozenSeconds;
    return static_cast<float>(s);
}

struct Sample {
    bool rawValid = false;   // the raw record could be read this frame
    float raw[6] = {};       // accelerometer x y z, gyro x y z
    float stickX = 0, stickY = 0;  // left stick, -1..1, dead zone applied
    float leanDeg = 0;       // how far the sensor says the console is from the calibrated grip (degrees)
};

class AutoStick {
public:
    enum Reason { None = 0, Frozen = 1, NoResponse = 2 };

    // One call per input frame. `tiltLevel`: the game is steering by tilt now. `sensorSource`: the setting is "Sensor" and the sensor is
    // reported present (otherwise the stick is used anyway and there is nothing to decide). Returns true while the stick should steer.
    // `event` gets 1 on the frame rule A latches, 2 on the frame rule B latches, 3 when B lets go, else 0 (for the one-line log).
    bool Step(float dt, bool tiltLevel, bool sensorSource, const Sample& s, int* event = nullptr) {
        if (event) *event = 0;
        if (!sensorSource || !tiltLevel) {
            pushTime_ = 0;
            if (!tiltLevel) {
                levelLatch_ = false;
                haveLast_ = false;  // the first record of the next level is a baseline, not a change
            }
            return sessionLatch_ || (sensorSource && levelLatch_);
        }
        if (sessionLatch_) return true;
        const float stick = std::sqrt(s.stickX * s.stickX + s.stickY * s.stickY);

        // Rule A: the raw record did / did not change.
        if (s.rawValid) {
            if (haveLast_ && std::memcmp(last_, s.raw, sizeof last_) != 0) frozen_ = 0;
            std::memcpy(last_, s.raw, sizeof last_);
            haveLast_ = true;
            rawSeen_ = true;
            if (stick >= kStickMoved && !(levelLatch_)) frozen_ += dt;
        }
        if (rawSeen_ && frozen_ >= limit_) {
            sessionLatch_ = true;
            reason_ = Frozen;
            if (event) *event = 1;
            return true;
        }

        // Rule B: a full stick and a lean that does not move.
        if (levelLatch_) {
            if (s.leanDeg > kBackDeg) {  // the sensor answers: it is alive, hand the level back
                levelLatch_ = false;
                pushTime_ = 0;
                if (event) *event = 3;
                return false;
            }
            return true;
        }
        if (stick >= kStickFull && s.leanDeg < kLeanDeg) {
            pushTime_ += dt;
            if (pushTime_ >= kPushSeconds) {
                levelLatch_ = true;
                reason_ = NoResponse;
                if (event) *event = 2;
                return true;
            }
        } else {
            pushTime_ = 0;
        }
        return false;
    }

    bool SessionLatched() const { return sessionLatch_; }
    bool LevelLatched() const { return levelLatch_; }
    Reason LastReason() const { return reason_; }
    void SetFrozenSeconds(float s) { limit_ = s; }
    void Reset() { *this = AutoStick(); }

private:
    float last_[6] = {};
    bool haveLast_ = false, rawSeen_ = false;
    float frozen_ = 0, pushTime_ = 0, limit_ = kFrozenSeconds;
    bool sessionLatch_ = false, levelLatch_ = false;
    Reason reason_ = None;
};

}  // namespace tilt
}  // namespace port
