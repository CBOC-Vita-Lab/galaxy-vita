// Galaxy 3.0 multiplayer: spectating, the pure rules (owner: spectate). No game headers: shared by the game side (MpSpectate.cpp,
// CameraMpWatch.cpp) and the host tests (tests/mp/spectate_test.cpp).
//
//  - Selector: who a waiting player watches (a bubbled player with nobody near, an out-of-lives player, a Star Race finisher), the L / R
//    order, what happens when the watched player leaves / falls / finishes, and when the player is back in play.
//  - Follow: the follow camera's maths. The view sits behind and above the watched character in ITS gravity frame: the frame (up,
//    heading) turns smoothly, the camera is placed from the smoothed frame, so a gravity flip swings the view round the character
//    (never through it) and nothing overshoots. Wall collision is the game's own (CameraViewInterpolator's binder).
#pragma once

#include <math.h>
#include <stdint.h>

namespace MpSpectate {

constexpr int kSlots = 4;

// ============================================================================================================================
// Selection
// ============================================================================================================================
enum Reason : uint8_t {
    REASON_NONE = 0,    // in play
    REASON_BUBBLE = 1,  // in a bubble (floating towards a teammate)
    REASON_OUT = 2,     // in a bubble with no life left (only a teammate's touch pops it)
    REASON_RACE = 3,    // Star Race: finished, waiting for the others
};

enum Event : uint8_t { EV_NONE = 0, EV_START, EV_SWITCH, EV_OWN, EV_END, EV_LOST };

constexpr int kStartDelayTurns = 90;   // a wait must last 1.5 s before the view starts by itself (a bubble that pops at once never does)

struct SelIn {
    int local = 0;                 // our slot
    uint8_t reason = REASON_NONE;  // what we wait for now (NONE = back in play)
    bool startOk = false;          // the wait is long enough to start by itself (far bubble, out of lives, race finish)
    bool canWatch[kSlots] = {};    // this slot may be watched this turn (present, in this scene, shown, playing)
    bool allowOwn = false;         // the L / R ring has a stop for our own view (a bubble: yes; a race: the standings are too costly)
    int prefer = -1;               // slot to start with when several can be watched (the teammate the bubble floats to)
    int step = 0;                  // -1 L, +1 R, 0 none (one turn's edge)
    int pick = -1;                 // a touch on a party strip head: that slot
    bool eventBlocked = false;     // a game event camera or a demo is running that is not ours: do not start (review2 #7)
};

struct SelOut {
    int target = -1;               // slot watched now, -1 = our own view
    Event ev = EV_NONE;
    bool forced = false;           // EV_SWITCH / EV_LOST because the watched player left, fell or finished
    bool blocked = false;          // a start was due (or asked for) but a game event camera / demo is running: deferred, tried again next turn
};

class Selector {
public:
    int target() const { return mTarget; }
    bool optedOut() const { return mOptOut; }
    void reset() { *this = Selector(); }

    SelOut step(const SelIn& in) {
        SelOut out;
        out.target = mTarget;
        if (in.reason == REASON_NONE) {                      // back in play: the view returns at once
            if (mTarget >= 0) { mTarget = -1; out.ev = EV_END; }
            mOptOut = false;
            mWait = 0;
            out.target = mTarget;
            return out;
        }
        if (mTarget >= 0 && (mTarget >= kSlots || mTarget == in.local || !in.canWatch[mTarget])) {
            // the watched player left, fell, was bubbled or finished: the next one, else our own view
            const int n = cycle(in, mTarget, +1, false);
            out.forced = true;
            if (n >= 0) { mTarget = n; out.ev = EV_SWITCH; }
            else { mTarget = -1; out.ev = EV_LOST; mWait = 0; }
            out.target = mTarget;
            return out;
        }
        const int press = in.step != 0 ? (in.step > 0 ? 1 : -1) : 0;
        if (mTarget >= 0) {
            mWait = 0;
            if (in.pick >= 0 && in.pick < kSlots && in.pick != mTarget && in.pick != in.local && in.canWatch[in.pick]) {
                mTarget = in.pick; out.ev = EV_SWITCH;
            } else if (press != 0) {
                const int n = cycle(in, mTarget, press, in.allowOwn);
                if (n < 0) { mTarget = -1; mOptOut = true; out.ev = EV_OWN; }
                else if (n != mTarget) { mTarget = n; out.ev = EV_SWITCH; }
            }
            out.target = mTarget;
            return out;
        }
        // review2 #7: our "MpWatch" chunk shares the priority slot of most game event cameras: starting it would cut a boss intro or a
        // talk camera short. Nothing starts while one runs; the wait keeps counting, so the start comes at once when the event ends.
        if (in.eventBlocked) {
            const bool auto_ = in.startOk && !mOptOut && anyWatchable(in);
            if (auto_) { if (mWait < kStartDelayTurns) ++mWait; } else { mWait = 0; }
            out.blocked = in.pick >= 0 || press != 0 || (auto_ && mWait >= kStartDelayTurns);
            return out;
        }
        // own view: a press or a touch starts watching at once; otherwise a long enough wait starts by itself
        if (in.pick >= 0 && in.pick < kSlots && in.pick != in.local && in.canWatch[in.pick]) {
            mTarget = in.pick; mOptOut = false; out.ev = EV_START;
        } else if (press != 0) {
            const int n = cycle(in, -1, press, false);
            if (n >= 0) { mTarget = n; mOptOut = false; out.ev = EV_START; }
        } else if (in.startOk && !mOptOut && anyWatchable(in)) {
            if (++mWait >= kStartDelayTurns) {
                const int n = (in.prefer >= 0 && in.prefer < kSlots && in.prefer != in.local && in.canWatch[in.prefer]) ? in.prefer : cycle(in, in.local, +1, false);
                if (n >= 0) { mTarget = n; out.ev = EV_START; mWait = 0; }
            }
        } else {
            mWait = 0;
        }
        out.target = mTarget;
        return out;
    }

    // The ring: watchable slots in ascending order, then (allowOwn) our own view. From `cur` (-1 = own / none) one stop `dir` on.
    // Returns the slot, or -1 for our own view / nothing to watch.
    static int cycle(const SelIn& in, int cur, int dir, bool ownStop) {
        int ring[kSlots + 1];
        int n = 0;
        for (int s = 0; s < kSlots; s++)
            if (s != in.local && in.canWatch[s]) ring[n++] = s;
        if (ownStop) ring[n++] = -1;
        if (n == 0) return -1;
        // position of `cur` in the ring (or where it would sit): cur == -1 is the own stop when the ring has it, else "before the first"
        int at = -2;
        for (int i = 0; i < n; i++) if (ring[i] == cur) at = i;
        if (at == -2) {
            if (cur < 0) {                                   // own view with no own stop: R = first, L = last
                return dir > 0 ? ring[0] : ring[n - 1];
            }
            // a slot that cannot be watched any more: the next higher one (or the first), L: the next lower one (or the last)
            if (dir > 0) { for (int i = 0; i < n; i++) if (ring[i] > cur || ring[i] < 0) return ring[i]; return ring[0]; }
            for (int i = n - 1; i >= 0; i--) if (ring[i] >= 0 && ring[i] < cur) return ring[i];
            return ring[n - 1];
        }
        return ring[(at + (dir > 0 ? 1 : n - 1)) % n];
    }

    static bool anyWatchable(const SelIn& in) {
        for (int s = 0; s < kSlots; s++) if (s != in.local && in.canWatch[s]) return true;
        return false;
    }

private:
    int mTarget = -1;
    bool mOptOut = false;
    int mWait = 0;
};

// ============================================================================================================================
// The follow camera
// ============================================================================================================================
struct V3 { float x, y, z; };
static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 mul(V3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(V3 a) { return sqrtf(dot(a, a)); }
static inline bool finite3(V3 a) { return a.x == a.x && a.y == a.y && a.z == a.z && fabsf(a.x) < 1e8f && fabsf(a.y) < 1e8f && fabsf(a.z) < 1e8f; }
static inline V3 normOr(V3 a, V3 fallback) {
    const float l = len(a);
    return l > 1e-6f ? mul(a, 1.0f / l) : fallback;
}
// a rotated about the unit axis by angle (Rodrigues)
static inline V3 rotate(V3 a, V3 axis, float angle) {
    const float c = cosf(angle), s = sinf(angle);
    return add(add(mul(a, c), mul(cross(axis, a), s)), mul(axis, dot(axis, a) * (1.0f - c)));
}
// any unit vector perpendicular to a
static inline V3 perpendicular(V3 a) {
    const V3 t = fabsf(a.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
    return normOr(cross(a, t), v3(1, 0, 0));
}

struct FollowParams {
    float dist = 900.0f;        // camera distance from the watched point
    float pitch = 0.34f;        // radians above the horizon of the character's frame
    float height = 120.0f;      // watched point above the feet (along the frame's up)
    float kUp = 0.10f;          // part of the remaining turn of the up vector taken each turn
    float minUpStep = 0.012f;   // ...but at least this many radians (a flip finishes)
    float maxUpStep = 0.05f;    // ...and at most this many (a flip takes about a second, never a whip)
    float kYaw = 0.045f;        // heading follows the character's facing (only while it moves), part per turn
    float kPos = 0.16f;         // watched point smoothing (slow end)
    float maxLag = 1400.0f;     // the watched point never trails the character by more than this
    float runSpeed = 12.0f;     // units per turn that count as "moving" for the heading (a run)
};

struct FollowIn {
    V3 feet;     // character position
    V3 up;       // its gravity up
    V3 front;    // its facing
    V3 vel;      // units per turn
};

struct FollowState {
    bool init = false;
    V3 up = {0, 1, 0};        // smoothed up
    V3 head = {0, 0, -1};     // unit, perpendicular to up: from the character towards the camera, horizontally
    V3 focus = {0, 0, 0};     // smoothed watched point
};

struct FollowOut {
    V3 pos, watch, up;
};

static inline V3 idealFocus(const FollowIn& in, const FollowParams& p, V3 upS) { return add(in.feet, mul(upS, p.height)); }

// Turns `a` towards `b` by the larger of (fraction k of the remaining angle) and minStep, never past b; the same turn is applied
// to the companion vector (parallel transport of the heading). An exactly opposite b turns about `hint`.
static inline V3 turnTowards(V3 a, V3 b, float k, float minStep, float maxStep, V3 hint, V3* companion) {
    float c = dot(a, b);
    c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
    const float angle = acosf(c);
    if (angle < 1e-5f) return b;
    V3 axis = cross(a, b);
    if (len(axis) < 1e-5f) axis = cross(a, hint);
    axis = normOr(axis, perpendicular(a));
    float step = angle * k;
    if (step < minStep) step = minStep;
    if (step > maxStep) step = maxStep;
    if (step >= angle) {
        if (companion) *companion = rotate(*companion, axis, angle);
        return b;
    }
    if (companion) *companion = rotate(*companion, axis, step);
    return normOr(rotate(a, axis, step), b);
}

static inline void followReset(FollowState* s, const FollowIn& in, const FollowParams& p) {
    s->up = normOr(in.up, v3(0, 1, 0));
    // behind the character: opposite its facing, flattened into the frame's plane
    V3 back = mul(in.front, -1.0f);
    back = sub(back, mul(s->up, dot(back, s->up)));
    s->head = normOr(back, perpendicular(s->up));
    s->focus = idealFocus(in, p, s->up);
    s->init = true;
}

static inline FollowOut followPose(const FollowState& s, const FollowParams& p) {
    FollowOut o;
    o.up = s.up;
    o.watch = s.focus;
    o.pos = add(s.focus, add(mul(s.head, p.dist * cosf(p.pitch)), mul(s.up, p.dist * sinf(p.pitch))));
    return o;
}

// One logic turn. Bad input (NaN, huge) leaves the state as it was.
static inline FollowOut followStep(FollowState* s, const FollowIn& in, const FollowParams& p) {
    if (!finite3(in.feet) || !finite3(in.up) || !finite3(in.front) || !finite3(in.vel)) {
        return followPose(*s, p);
    }
    if (!s->init) followReset(s, in, p);
    // 1. the frame: the up vector turns towards the character's (a flip swings round), the heading is carried with it
    const V3 upWant = normOr(in.up, s->up);
    s->up = turnTowards(s->up, upWant, p.kUp, p.minUpStep, p.maxUpStep, s->head, &s->head);
    s->head = normOr(sub(s->head, mul(s->up, dot(s->head, s->up))), perpendicular(s->up));
    // 2. the heading drifts behind the character while it runs (never while it stands: the view stays where the viewer left it)
    V3 vh = sub(in.vel, mul(s->up, dot(in.vel, s->up)));
    const float speed = len(vh);
    const float move = speed / p.runSpeed > 1.0f ? 1.0f : speed / p.runSpeed;
    if (move > 0.0f) {
        V3 back = mul(normOr(sub(in.front, mul(s->up, dot(in.front, s->up))), mul(s->head, -1.0f)), -1.0f);
        // the shorter way round the up axis
        const float sinA = dot(cross(s->head, back), s->up);
        const float cosA = dot(s->head, back);
        const float a = atan2f(sinA, cosA);
        s->head = normOr(rotate(s->head, s->up, a * p.kYaw * move), s->head);
    }
    // 3. the watched point: smoothed, faster the faster the character moves (a launch star), never too far behind
    const V3 want = idealFocus(in, p, s->up);
    float k = p.kPos + speed / 200.0f;
    if (k > 0.7f) k = 0.7f;
    s->focus = add(s->focus, mul(sub(want, s->focus), k));
    V3 lag = sub(want, s->focus);
    const float l = len(lag);
    if (l > p.maxLag) s->focus = sub(want, mul(lag, p.maxLag / l));
    return followPose(*s, p);
}

}  // namespace MpSpectate
