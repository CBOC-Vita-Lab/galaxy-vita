// Galaxy 3.0 multiplayer: Play Together "Bubbles" falls, the pure part (owner: bubble). No game headers: the host tests build it as
// is (tests/mp/bubble_test.cpp). MpBubble.cpp (game side) feeds it puppet positions and pad input and applies what it decides.
//
//  - Who is in a bubble (Table): every console keeps one entry per slot from the BUBBLE messages of the current scene serial (the
//    host relays them to everyone, so a new host after a migration already has them) and from the STATE flag MP_POSE_BUBBLE (a
//    late joiner sees the others' bubbles from their poses alone).
//  - Everyone caught (allCaught): every player of this scene serial is in a bubble, the BUBBLE message and the pose agree, for
//    kRestartDebounce turns in a row: the host sends RESTART.
//  - Where the bubble floats (hoverPoint / stepFloat): toward the nearest teammate who is present, playing and not in a bubble,
//    kHoverUp above and kHoverSide to the side of him (a few hundred units away), along his gravity "up"; a gentle bob; the
//    bubbled player's stick drifts it a little. A damped spring with a speed cap (smooth, never a jump).
//  - Pops (touchPops / mayPopWithA): a teammate's body touching the bubble pops it; the bubbled player's A pops it when the
//    bubble is near a teammate standing on the ground. A player out of lives cannot pop it himself (only a teammate's touch, which
//    gives him 1 life back): a player at 0 lives must not get infinite tries.
//  - Spin and Star Bit pops (spinReaches / segmentHitsSphere / acceptPop): the popper's console sees the remote bubble where its
//    own screen shows it: its spin within reach, or one of its Star Bit shots flying through it, sends a pop request (a BUMP of
//    kind kBumpPop to the bubbled slot). The bubbled console checks the request (a teammate of this scene, past the grace time,
//    a spin from near enough by its own view) and applies the pop itself: the pop authority stays with the bubbled console.
//  - Arrivals (drop-in, warp to a friend): the player appears in a bubble beside the friend, no life lost; it pops by itself after
//    kArrivalTurns or at a touch. BUBBLE carries MP_BUBBLE_ARRIVAL: no "in a bubble" toast, never counts as caught.
#pragma once

#include <math.h>
#include <stdint.h>

#include "port/mp_proto.h"

namespace MpBubbleLogic {

constexpr float kRadius = 150.0f;          // BigBubble's sphere at volume 1 (BigBubble::sBaseRadiusSphere)
constexpr float kRiderDrop = 80.0f;        // the player's feet sit this far below the bubble's centre (his middle in the middle)
constexpr float kBodyUp = 90.0f;           // a teammate's body centre above his feet (the touch test)
constexpr float kTouchSlack = 60.0f;       // a teammate's body "radius" for the touch test
constexpr float kHoverUp = 230.0f;         // where the bubble waits: above the teammate's feet...
constexpr float kHoverSide = 230.0f;       // ...and this far to his side (about 330 units away)
constexpr float kNearForA = 650.0f;        // the A pop: the bubble's centre within this of a grounded teammate's feet
constexpr float kDriftMax = 220.0f;        // the stick drifts the bubble up to this far from its hover point
constexpr float kSpring = 0.018f;          // the float: acceleration per unit of distance per turn^2
constexpr float kDamping = 0.24f;          // velocity damping per turn (about critical for kSpring)
constexpr float kMaxSpeed = 28.0f;         // units per turn (a long way back from an abyss takes a few seconds)
constexpr float kBobAmp = 14.0f;           // the gentle bob
constexpr float kBobTurns = 150.0f;        // its period
constexpr int kRestartDebounce = 60;       // turns everyone must stay caught before the host restarts (1 s: the last bubble shows)
constexpr int kAloneTurns = 240;           // a bubble with nobody to float to restarts at the checkpoint after 4 s (never stranded)
constexpr int kPopGraceTurns = 90;         // a new bubble cannot be popped for 1.5 s (it grows and floats off first)
constexpr uint16_t kInvincibleTurns = 120; // the blink after a pop (2 s)
constexpr float kSpinReach = 140.0f;       // a teammate's spin pops the bubble when his body centre is this close to its surface
constexpr float kShotReach = 3000.0f;      // a Star Bit shot pops from this far: StarPiece::exeThrow ends a flight 1500 from the camera (+ the camera's own distance behind the player)
constexpr float kPopCheckSlack = 300.0f;   // the bubbled console's own check of a spin pop: the two views differ by the lag
constexpr int kArrivalTurns = 120;         // an arrival bubble (drop-in, warp to a friend) pops by itself after 2 s
constexpr int kArrivalGraceTurns = 20;     // ...and a touch pops it from this age on
constexpr int kPopRequestCooldown = 30;    // the popper's console asks again for the same bubble at most twice a second
constexpr uint8_t kBumpPop = 4;            // BUMP kind of a pop request (MpPuppetsInternal.hpp MP_BUMP_POP)
enum PopCause : uint8_t { kPopSpin = 1, kPopShot = 2 };

struct V3 {
    float x, y, z;
};
static inline V3 v3(float x, float y, float z) { return V3{x, y, z}; }
static inline V3 add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline V3 sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 mul(V3 a, float s) { return V3{a.x * s, a.y * s, a.z * s}; }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float len(V3 a) { return sqrtf(dot(a, a)); }
static inline bool finite3(V3 a) { return mp_pose_float_ok(a.x) && mp_pose_float_ok(a.y) && mp_pose_float_ok(a.z); }
static inline V3 normOr(V3 a, V3 fallback) {
    const float l = len(a);
    return l > 1.0e-4f && l == l ? mul(a, 1.0f / l) : fallback;
}
// a perpendicular to `up` from `dir` (dir with its up part removed), or `fallback`
static inline V3 flatten(V3 dir, V3 up, V3 fallback) { return normOr(sub(dir, mul(up, dot(dir, up))), fallback); }

// The up axis (gravity "up") of a pose's base rotation: the quaternion's Y axis.
static inline V3 upOfQuat(const int16_t q[4]) {
    const float x = q[0] / 32767.0f, y = q[1] / 32767.0f, z = q[2] / 32767.0f, w = q[3] / 32767.0f;
    const V3 up = v3(2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x));
    return normOr(up, v3(0.0f, 1.0f, 0.0f));
}

// ---- the bubble table -----------------------------------------------------------------------------------------------------------
struct Entry {
    bool on;          // the latest BUBBLE message of this serial says "in a bubble"
    bool out;         // ...and out of lives
    bool arrival;     // ...an arrival bubble (drop-in, warp): pops by itself, never "caught"
    uint8_t lives;
};
struct Table {
    uint16_t serial = 0;
    bool serialKnown = false;
    Entry e[MP_MAX_PLAYERS] = {};

    void reset(uint16_t s) {
        serial = s;
        serialKnown = true;
        for (Entry& x : e) x = Entry{};
    }
    // a BUBBLE message (any slot, own included); false when it is for another scene serial (dropped)
    bool note(int slot, const MpBubbleMsg& m) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS) return false;
        if (!serialKnown || m.serial != serial) return false;
        e[slot].on = m.on != 0;
        e[slot].out = (m.flags & MP_BUBBLE_OUT_OF_LIVES) != 0;
        e[slot].arrival = (m.flags & MP_BUBBLE_ARRIVAL) != 0;
        e[slot].lives = m.lives;
        return true;
    }
    void leave(int slot) {
        if (slot >= 0 && slot < MP_MAX_PLAYERS) e[slot] = Entry{};
    }
};

// gamefix: a pose older than this is not "around" (the puppets hide a player after the same 180 turns without a STATE). The pose's
// own turn counter (the sender's) is what changes while he sends: this console notes, every turn, the local turn it last changed.
constexpr uint32_t kPoseStaleTurns = 180;
struct PoseFreshness {
    uint32_t last[MP_MAX_PLAYERS] = {};   // the sender's turn counter in the latest pose
    uint32_t seen[MP_MAX_PLAYERS] = {};   // our turn when it last changed
    bool known[MP_MAX_PLAYERS] = {};
    void note(int s, uint32_t poseTurn, uint32_t now) {
        if (s < 0 || s >= MP_MAX_PLAYERS) return;
        if (!known[s] || last[s] != poseTurn) { known[s] = true; last[s] = poseTurn; seen[s] = now; }
    }
    bool fresh(int s, uint32_t now) const { return s >= 0 && s < MP_MAX_PLAYERS && known[s] && now - seen[s] <= kPoseStaleTurns; }
    void reset() { *this = PoseFreshness(); }
};

// What the host knows of one slot for the restart decision.
struct SlotView {
    bool present;     // in the session
    bool inScene;     // its latest pose is of this scene serial (or it is the local player in a live scene)
    bool poseBubble;  // its latest pose has MP_POSE_BUBBLE (the local player: own state)
    bool loading = false;  // glitchfix G8: present and still loading (no pose of this serial yet, but not stalled): the restart waits for him
};
// glitchfix G8: how long the host waits for a loading player before it ignores him (a scene load on the console takes well under this).
constexpr uint32_t kLoadingWaitTurns = 2400;   // 40 s at 60 turns/s
struct LoadingWait {
    uint32_t since[MP_MAX_PLAYERS] = {};   // turn the slot started to load (0 = not loading)
    // rawLoading: the slot is present, not in this scene and not stalled. true = still worth waiting for.
    bool step(int s, bool rawLoading, uint32_t now) {
        if (s < 0 || s >= MP_MAX_PLAYERS) return false;
        if (!rawLoading) { since[s] = 0; return false; }
        if (since[s] == 0) since[s] = now ? now : 1;
        return now - since[s] < kLoadingWaitTurns;
    }
    void reset() { *this = LoadingWait(); }
};
// Everyone of this scene serial is in a bubble (the message and the pose agree): at least one player, nobody of the scene free.
// An arrival bubble is not "caught" (it pops by itself in a moment).
static inline bool everyoneCaught(const Table& t, const SlotView v[MP_MAX_PLAYERS]) {
    int caught = 0;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (v[s].present && v[s].loading && !v[s].inScene) return false;   // G8: a loading player is not "caught"
        if (!v[s].present || !v[s].inScene) continue;
        if (!(t.e[s].on && !t.e[s].arrival && v[s].poseBubble)) return false;
        ++caught;
    }
    return caught > 0;
}
// The host's debounce: true once, on the turn the condition has held for kRestartDebounce turns (then it re-arms only after a
// reset: one RESTART per caught moment).
struct RestartGate {
    int held = 0;
    bool fired = false;
    void reset() { held = 0; fired = false; }
    bool step(bool caught) {
        if (!caught) { held = 0; fired = false; return false; }
        if (fired) return false;
        if (++held >= kRestartDebounce) { fired = true; return true; }
        return false;
    }
};

// ---- floating ---------------------------------------------------------------------------------------------------------------
struct Teammate {
    bool valid;       // present, in this scene, playing (not dead, not in a demo), not in a bubble
    V3 feet;          // drawn position (the puppet's)
    V3 up;            // his gravity up
    bool onGround;
};
// The nearest valid teammate to `from` (-1 = none).
static inline int nearest(const Teammate t[MP_MAX_PLAYERS], V3 from) {
    int best = -1;
    float bestD = 0.0f;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (!t[s].valid) continue;
        const float d = len(sub(t[s].feet, from));
        if (best < 0 || d < bestD) best = s, bestD = d;
    }
    return best;
}
// Where the bubble waits near teammate `m`: above him and to the side the bubble comes from (camera side when right above him).
static inline V3 hoverPoint(const Teammate& m, V3 bubble, V3 cameraSide) {
    const V3 side = flatten(sub(bubble, m.feet), m.up, flatten(cameraSide, m.up, v3(1.0f, 0.0f, 0.0f)));
    return add(add(m.feet, mul(m.up, kHoverUp)), mul(side, kHoverSide));
}
// One turn of the float toward `target` (+ bob along `up` at turn `t`). Returns the new position; *vel is updated.
static inline V3 stepFloat(V3 pos, V3* vel, V3 target, V3 up, uint32_t t) {
    const float bob = kBobAmp * sinf(6.2831853f * (float)(t % (uint32_t)kBobTurns) / kBobTurns);
    const V3 goal = add(target, mul(up, bob));
    V3 v = add(mul(*vel, 1.0f - kDamping), mul(sub(goal, pos), kSpring));
    const float sp = len(v);
    if (sp > kMaxSpeed) v = mul(v, kMaxSpeed / sp);
    if (!finite3(v)) v = v3(0.0f, 0.0f, 0.0f);
    *vel = v;
    const V3 p = add(pos, v);
    return finite3(p) ? p : pos;
}
// The stick's drift offset (smoothed by the caller): stick x along the camera's side, stick y "into the screen", both flattened
// on the plane of `up`, at most kDriftMax.
static inline V3 driftOffset(float stickX, float stickY, V3 camSide, V3 camFront, V3 up) {
    const float sx = stickX < -1.0f ? -1.0f : (stickX > 1.0f ? 1.0f : stickX);
    const float sy = stickY < -1.0f ? -1.0f : (stickY > 1.0f ? 1.0f : stickY);
    const V3 s = flatten(camSide, up, v3(1.0f, 0.0f, 0.0f));
    const V3 f = flatten(camFront, up, v3(0.0f, 0.0f, 1.0f));
    V3 d = add(mul(s, sx * kDriftMax), mul(f, sy * kDriftMax));
    const float l = len(d);
    if (l > kDriftMax) d = mul(d, kDriftMax / l);
    return d;
}

// ---- pops -------------------------------------------------------------------------------------------------------------------
// The first teammate (lowest slot) whose body touches the bubble, -1 none (t[s].valid: a playing teammate, not in a bubble).
static inline int touchPops(V3 centre, float radius, const Teammate t[MP_MAX_PLAYERS]) {
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (!t[s].valid) continue;
        const V3 body = add(t[s].feet, mul(t[s].up, kBodyUp));
        if (len(sub(body, centre)) < radius + kTouchSlack) return s;
    }
    return -1;
}
// The A pop: not out of lives, past the grace time, and a grounded teammate near; returns his slot (-1: no pop).
static inline int mayPopWithA(bool aPressed, bool outOfLives, int ageTurns, V3 centre, const Teammate t[MP_MAX_PLAYERS]) {
    if (!aPressed || outOfLives || ageTurns < kPopGraceTurns) return -1;
    int best = -1;
    float bestD = 0.0f;
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        if (!t[s].valid || !t[s].onGround) continue;
        const float d = len(sub(t[s].feet, centre));
        if (d < kNearForA && (best < 0 || d < bestD)) best = s, bestD = d;
    }
    return best;
}
// ---- spin and Star Bit pops (the popper's console asks, the bubbled console decides) ---------------------------------------------
// The popper's spin reaches the bubble: his body centre within kSpinReach of its surface.
static inline bool spinReaches(V3 body, V3 centre, float radius) {
    const float d = len(sub(body, centre));
    return d == d && d < radius + kSpinReach;
}
// A Star Bit's flight this turn (segment a -> b) passes through the sphere (c, r).
static inline bool segmentHitsSphere(V3 a, V3 b, V3 c, float r) {
    if (!finite3(a) || !finite3(b) || !finite3(c) || !(r > 0.0f)) return false;
    const V3 ab = sub(b, a);
    const float l2 = dot(ab, ab);
    float t = l2 > 1.0e-6f ? dot(sub(c, a), ab) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const V3 q = add(a, mul(ab, t));
    return len(sub(q, c)) < r;
}
// The pop request on the wire: a BUMP (target u8, kind u8 = kBumpPop, then the BUMP's three s8: cause, 0, 0). 5 bytes.
static inline bool writePopRequest(MpWriter& w, uint8_t target, uint8_t cause) {
    w.u8(target); w.u8(kBumpPop); w.u8(cause); w.u8(0); w.u8(0);
    return w.ok();
}
// True = the BUMP is a pop request (kind kBumpPop), whatever its validity; *ok = well formed (target < MP_MAX_PLAYERS, a known cause,
// zero padding, exactly 5 bytes).
static inline bool readPopRequest(const uint8_t* d, size_t n, bool* ok, uint8_t* target, uint8_t* cause) {
    *ok = false;
    if (n < 2 || d[1] != kBumpPop) return false;
    MpReader r(d, n);
    *target = r.u8();
    r.u8();
    *cause = r.u8();
    const uint8_t p0 = r.u8(), p1 = r.u8();
    *ok = r.done() && *target < MP_MAX_PLAYERS && (*cause == kPopSpin || *cause == kPopShot) && p0 == 0 && p1 == 0;
    return true;
}
// The bubbled console's check of a pop request: past the grace time, from a teammate of this scene who is playing (not in a
// bubble); a spin or a shot also from near enough by our own view of him (dist: his body centre to our bubble's centre, < 0 unknown).
static inline bool acceptPop(uint8_t cause, int ageTurns, int graceTurns, bool requesterPlaying, float dist, float radius) {
    if (ageTurns < graceTurns || !requesterPlaying) return false;
    if (cause == kPopShot) return dist >= 0.0f && dist < radius + kShotReach + kPopCheckSlack;   // gamefix: a shot has a reach too
    if (cause == kPopSpin) return dist >= 0.0f && dist < radius + kSpinReach + kPopCheckSlack;
    return false;
}
// An arrival bubble's own pop: after kArrivalTurns.
static inline bool arrivalDone(int ageTurns) { return ageTurns >= kArrivalTurns; }

// Lives after a fall in Bubbles mode: one is lost (the real decrement); a player who had none is out of lives (keeps 0).
static inline bool fallIsOutOfLives(int livesBefore) { return livesBefore <= 0; }
// Lives after a pop: an out-of-lives player rejoins with 1.
static inline int livesAfterPop(int lives, bool outOfLives) { return outOfLives && lives < 1 ? 1 : lives; }

// The camera must catch up with the player (MR::resetCameraMan: a cut beside him) when it is farther than kCameraLost from him, which is
// never so in play (the follow camera keeps within a few hundred to ~2000 units) but is so after a bubble carried him across the stage or
// a pop beside a teammate far from where the fall happened. At most once per kCameraResetGap turns (a cut every turn would hold the view).
constexpr float kCameraLost = 2500.0f;
constexpr int kCameraResetGap = 20;
static inline bool cameraNeedsCut(float camToPlayer, uint32_t turnsSinceCut) { return camToPlayer > kCameraLost && turnsSinceCut >= (uint32_t)kCameraResetGap; }
// A death that the game queued before the pop (the DeathArea kills the player every turn while he floats over it: the GameOver nerve
// was set in the turn of the pop) fires on the next turn at the new place: no new fall. kStaleKillTurns after a pop such a first step
// is the old death again.
constexpr uint32_t kStaleKillTurns = 4;
static inline bool staleKillAfterPop(bool popped, uint32_t turnsSincePop) { return popped && turnsSincePop <= kStaleKillTurns; }

// Where the popped player lands: beside the teammate (on the bubble's side), a little above his feet.
static inline V3 landingPoint(const Teammate& m, V3 bubble, V3 cameraSide) {
    const V3 side = flatten(sub(bubble, m.feet), m.up, flatten(cameraSide, m.up, v3(1.0f, 0.0f, 0.0f)));
    return add(add(m.feet, mul(side, 110.0f)), mul(m.up, 40.0f));
}

}  // namespace MpBubbleLogic
