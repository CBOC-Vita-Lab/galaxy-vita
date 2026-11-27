// Galaxy 3.0 multiplayer: how a character select head moves (owner: heads3d). Pure code (no game, no SDK): host tested in
// tests/mp/head3d_motion_test.cpp. The numbers are the file select's own (decomp/src/Game/Map/FileSelectItem.cpp):
//   spin       FileSelectItem::updateRotate: never slower than 0.5 degrees a frame (the sign of the last flick), a flick of the
//              star pointer adds 0.03 degrees a frame per screen pixel (at most 25), slowing by 0.98 a frame
//   turn       turnToFront(40): the angle (in -180..180) times 1 - (step / 40)^2 each frame, the spin held at 0
//   grow       FileSelectItemSub::ScaleController: to 1.2 when pointed, back to 1.0 when left, s += (step / 30) (target - s) over
//              30 frames
//   blink      FileSelectItemSub::BlinkController: open 180..300 frames, shut 10; spun hard (more than 6 / 10 degrees a frame for
//              180 counts) the eyes close until the head has turned slowly (2 degrees a frame) for 60 frames, then open
#pragma once

#include <math.h>

namespace MpHead3DMotion {

constexpr float kMinSpin = 0.5f, kSpinDecay = 0.98f, kFlickPerPixel = 0.03f, kMaxSpin = 25.0f;
constexpr int kTurnFrames = 40, kGrowFrames = 30;
constexpr float kBig = 1.2f, kSmall = 1.0f;

struct Motion {
    float rotY = 0.0f;   // degrees, 0 = facing the camera
    float spin = 0.0f;   // degrees a frame
    int turnDur = 0, turnStep = 0;
    float grow = kSmall;
    int growStep = kGrowFrames + 1;
    bool big = false;
    bool front = false;   // character select: at rest the head faces the camera (a flick spins it, it eases back); the file select's never rests
};

inline float wrap(float v, float lo, float range) {
    if (!(v == v) || range <= 0.0f) return lo;   // (NaN)
    v = fmodf(v - lo, range);
    if (v < 0.0f) v += range;
    return lo + v;
}

// The screen opens: facing the front, at rest; already grown if it opens on the cursor
inline void reset(Motion& m, bool selected) {
    const bool front = m.front;
    m = Motion();
    m.front = front;
    m.big = selected;
    m.grow = selected ? kBig : kSmall;
}

// Pointed (grow and turn to the front) or left (shrink back)
inline void point(Motion& m, bool selected) {
    if (selected == m.big) return;
    m.big = selected;
    m.growStep = 0;
    if (selected) {
        m.turnDur = kTurnFrames;
        m.turnStep = 0;
    }
}

// The star pointer moved across the head by this many screen pixels (to the right > 0)
inline void flick(Motion& m, float pixels) {
    if (!(pixels == pixels)) return;
    m.spin += kFlickPerPixel * pixels;
    m.spin = m.spin < -kMaxSpin ? -kMaxSpin : (m.spin > kMaxSpin ? kMaxSpin : m.spin);
}

// One frame. True while turning to the front (the eyes are kept open then).
inline bool step(Motion& m) {
    if (m.growStep <= kGrowFrames) {
        const float v = static_cast<float>(m.growStep) / static_cast<float>(kGrowFrames);
        m.grow += v * ((m.big ? kBig : kSmall) - m.grow);
        m.growStep++;
    } else {
        m.grow = m.big ? kBig : kSmall;
    }
    if (m.turnDur > 0) {
        m.spin = 0.0f;
        m.turnStep++;
        float rate = static_cast<float>(m.turnStep) / static_cast<float>(m.turnDur);
        rate *= rate;
        rate = rate > 1.0f ? 1.0f : (rate < 0.0f ? 0.0f : rate);
        if (m.turnStep >= m.turnDur) m.turnDur = m.turnStep = 0;
        const float a = wrap(m.rotY, -180.0f, 360.0f);
        m.rotY = a - a * rate;
        return true;
    }
    m.spin *= kSpinDecay;
    if (m.front) {
        if (m.spin > -kMinSpin && m.spin < kMinSpin) {   // slow: ease to the front by the shorter way
            m.spin = 0.0f;
            const float a = wrap(m.rotY, -180.0f, 360.0f);
            m.rotY = (a > -0.2f && a < 0.2f) ? 0.0f : a * 0.94f;
        } else {
            m.rotY = wrap(m.rotY + m.spin, 0.0f, 360.0f);
        }
        return false;
    }
    if (m.spin >= 0.0f && m.spin < kMinSpin) m.spin = kMinSpin;
    if (m.spin < 0.0f && m.spin > -kMinSpin) m.spin = -kMinSpin;
    m.rotY = wrap(m.rotY + m.spin, 0.0f, 360.0f);
    return false;
}

// ---- the eyes
enum BlinkState { BLINK_OPEN = 0, BLINK_SHUT, BLINK_SLEEP, BLINK_WAKE };
enum BlinkAct { ACT_NONE = 0, ACT_NORMAL, ACT_BLINK, ACT_CLOSE, ACT_OPEN };   // the model's actions: "normal", "blink", "close", "open"
struct Blink {
    BlinkState state = BLINK_OPEN;
    int step = 0, limit = 240, count = 0;
};

// Enter a state; `limit` is the next open time (the caller's random 180..300). The action to start.
inline BlinkAct blinkEnter(Blink& b, BlinkState s, int limit) {
    b.state = s;
    b.step = 0;
    switch (s) {
    case BLINK_OPEN: b.limit = limit; b.count = 0; return ACT_NONE;
    case BLINK_SHUT: return ACT_BLINK;
    case BLINK_SLEEP: b.count = 0; return ACT_CLOSE;
    case BLINK_WAKE: return ACT_OPEN;
    }
    return ACT_NONE;
}

// One frame of the eyes. spin: this frame's; animDone: the eye animation has stopped; turning: Motion::step's result; limit: a
// fresh random open time (used only when the eyes open again). The action to start this frame.
inline BlinkAct blinkStep(Blink& b, float spin, bool animDone, bool turning, int limit) {
    if (turning) {   // (turnToFront: BlinkController::open, every frame of the turn)
        if (b.state == BLINK_OPEN) return ACT_NONE;
        blinkEnter(b, BLINK_OPEN, limit);
        return ACT_NORMAL;
    }
    const float v = spin < 0.0f ? -spin : spin;
    b.step++;
    switch (b.state) {
    case BLINK_OPEN:
        b.count = v > 10.0f ? b.count + 2 : (v > 6.0f ? b.count + 1 : 0);
        if (b.count > 180) return blinkEnter(b, BLINK_SLEEP, limit);
        if (b.step >= b.limit) return blinkEnter(b, BLINK_SHUT, limit);
        return ACT_NONE;
    case BLINK_SHUT:
        if (b.step >= 10 || animDone) {
            blinkEnter(b, BLINK_OPEN, limit);
            return ACT_NORMAL;
        }
        return ACT_NONE;
    case BLINK_SLEEP:
        b.count = v > 2.0f ? 0 : b.count + 1;
        if (b.count > 60) return blinkEnter(b, BLINK_WAKE, limit);
        return ACT_NONE;
    case BLINK_WAKE:
        if (animDone || b.step > 60) {
            blinkEnter(b, BLINK_OPEN, limit);
            return ACT_NORMAL;
        }
        return ACT_NONE;
    }
    return ACT_NONE;
}

// ---- the size: a head `size` square units tall (the layout's head box) holds the model's head (`height` model units) at
// `fill` of it, `worldPerSquare` world units a square unit at the heads' depth, grown and popped
inline float modelScale(float size, float fill, float worldPerSquare, float height, float grow, float pop) {
    if (!(height > 1e-3f) || !(size > 0.0f) || !(worldPerSquare > 0.0f)) return 0.0f;
    return size * fill * worldPerSquare / height * grow * pop;
}

// The pop of a head that appears (past its size, back), the plain shrink of one that goes; t in 0..1
inline float pop(float t, bool appearing) {
    if (!(t > 0.0f)) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (!appearing) return t * t * (3.0f - 2.0f * t);
    const float c = 1.70158f, u = t - 1.0f;
    return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
}

}  // namespace MpHead3DMotion
