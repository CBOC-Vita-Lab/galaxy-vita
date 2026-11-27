// Galaxy 3.0 multiplayer: puppet formation, pure logic (owner: formation; no game headers; host tests: tests/mp/formation_test.cpp).
// The game side is MpFormation.cpp (called from MpPuppets::update).
//
// Every console starts its players on the same start point and plays the same demos, so during arrivals, star-get demos, warps and
// the first seconds of a scene the remote puppets' true positions coincide with the local player and they are drawn inside him.
// This gives them some personal space, VISUALLY (the drawn pose, its shadow and name tag; never the real player, never collision):
//
//  - SPACE: a puppet closer than kEngage to the local player while either of them is in a demo / cutscene, or in the spawn window
//    (the first kSpawnTurns playable turns of a scene), is pushed out to a formation spot kSpacing from the local player along the
//    local gravity's tangent plane: rank 0 (the lowest other slot) to the camera's right, rank 1 left, rank 2 behind (away from the
//    camera). The push is only what is missing ("personal space": a puppet already that far along its direction is left alone) and
//    falls off between kEngage and kRelease, so a pair walking apart releases without a slide. After the demo / window the space is
//    kept while they stay closer than kRelease, at most kGraceTurns. On the ground a spot needs ground (the game's ground check, done
//    by the caller): the next candidate (other side, behind, in front) is taken, none = no push.
//  - ARC (Play Together, the star-get demo): every puppet within kGather stands on a small arc behind the local player (the star
//    receiver: every console plays its own star-get demo) and turns to the camera: a team photo. A puppet farther than kAppear
//    from its place appears there (the drop-in grow / fade-in) instead of flying across the level.
//  - The directions come from a frame latched from the camera when the first puppet engages and then only carried along the
//    changing gravity (no swinging when the camera turns); it is latched again only when the camera has turned 70..110 degrees
//    away (a puppet on the old side would be in front of or behind the player on screen).
//  - Nothing pops: the offset follows its target through a critically damped spring (~kSettle s), the camera-facing turn too.
#pragma once

#include <cmath>
#include <cstdint>

namespace MpFormation {

struct V3 {
    float x, y, z;
};

inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
inline V3 add(const V3& a, const V3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 sub(const V3& a, const V3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 mul(const V3& a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float len(const V3& a) { return std::sqrt(dot(a, a)); }
inline bool finite3(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline bool normalize(V3* a) {
    const float l = len(*a);
    if (!(l > 1e-4f) || !std::isfinite(l)) return false;
    *a = mul(*a, 1.0f / l);
    return true;
}
inline V3 flatten(const V3& a, const V3& up) { return sub(a, mul(up, dot(a, up))); }   // onto the plane normal to up

// ---- tuning (units: the game's; turns: 60 per second)
static constexpr float kEngage = 130.0f;     // closer than this (true positions) and in a demo / the spawn window: space
static constexpr float kRelease = 240.0f;    // space ends beyond this; the push fades out from kEngage to here
static constexpr float kSpacing = 190.0f;    // formation spot: this far from the local player
static constexpr float kArcRadius = 180.0f;  // the team photo's arc behind the local player
static constexpr float kArcMiddle = 1.6f;    // (the third of three stands in the middle, this much further back)
static constexpr float kGather = 3000.0f;    // star-get: puppets closer than this come to the photo
static constexpr float kAppear = 700.0f;     // a visual move longer than this is not glided: the puppet appears at its place
static constexpr int kSpawnTurns = 180;      // 3 s after the scene's first playable turn
static constexpr int kGraceTurns = 300;      // after the demo / spawn window: space kept while still close, at most 5 s
static constexpr float kSettle = 0.3f;       // s: blend in / out (critically damped: ~95 % of the way)
static constexpr float kDt = 1.0f / 60.0f;
static constexpr float kRelatch = 0.34f;     // |cos| between the latched and the camera's right below which it is latched again
static constexpr float kSideMin = 25.0f;      // a lone puppet that truly stands this far to one side of the player keeps that side (the way
                                             // out of the formation then never crosses the player)
static constexpr float kHoldMove = 40.0f;     // after the grace a puppet still truly inside the player stays spaced until it has moved this far
static constexpr float kMaxGroundStep = 200.0f;   // a ground correction larger than this is not used (another floor)
static constexpr int kLagTurns = 24;         // a puppet is drawn ~100 ms (+ jitter) behind its player: distances are measured to
                                             // the nearest of the local player's last kLagTurns positions (a fast flight on the same
                                             // path is "together" even though the puppet trails by a few hundred units)

enum Mode : uint8_t { MODE_NONE = 0, MODE_SPACE = 1, MODE_ARC = 2, MODE_GROUP = 3 };
enum Why : uint8_t { WHY_NONE = 0, WHY_DEMO, WHY_SPAWN, WHY_STAR, WHY_APART, WHY_GRACE, WHY_HIDDEN, WHY_STAR_END };
enum Event : uint8_t { EV_NONE = 0, EV_ON, EV_OFF };

inline const char* whyName(uint8_t w) {
    switch (w) {
    case WHY_DEMO: return "demo";
    case WHY_SPAWN: return "spawn";
    case WHY_STAR: return "star";
    case WHY_APART: return "apart";
    case WHY_GRACE: return "grace";
    case WHY_HIDDEN: return "hidden";
    case WHY_STAR_END: return "starend";
    default: return "-";
    }
}

inline float smooth01(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// 1 up to kEngage, 0 from kRelease, smooth in between
inline float falloff(float dist) { return 1.0f - smooth01((dist - kEngage) / (kRelease - kEngage)); }

// ---- frames
struct Frame {
    V3 up, right, back;   // orthonormal: back = up x right points away from the camera
};

// The camera's frame on the tangent plane of the local gravity. gravity zero (space, a flight between planets): the camera's up.
inline bool makeFrame(const V3& gravity, const V3& camRight, const V3& camFwd, const V3& camUp, Frame* out) {
    V3 up = mul(gravity, -1.0f);
    if (!finite3(up) || !normalize(&up)) {
        up = camUp;
        if (!finite3(up) || !normalize(&up)) return false;
    }
    V3 r = flatten(camRight, up);
    if (!finite3(r) || !normalize(&r)) {
        V3 b = flatten(camFwd, up);   // (the camera rolled onto its side: its forward still says where "away" is)
        if (!finite3(b) || !normalize(&b)) return false;
        r = cross(b, up);
        if (!normalize(&r)) return false;
    }
    out->up = up;
    out->right = r;
    out->back = cross(up, r);
    return true;
}

// The latched frame: taken from the camera at the first engagement, then its right is carried along the changing gravity.
struct Latch {
    bool on;
    V3 right;
    void reset() { on = false; right = v3(1.0f, 0.0f, 0.0f); }
    // cam: this turn's camera frame. Returns the frame to use; *relatched = latched again this turn (it was on: the camera turned).
    Frame update(const Frame& cam, bool* relatched, bool arc = false) {
        *relatched = false;
        V3 r = on ? flatten(right, cam.up) : cam.right;
        // (the photo's places are BEHIND the player: a camera that turned all the way round (the star-get camera looks at the player
        // from the front) would put them in front of him, so there the sign counts too)
        if (!on || !normalize(&r) || std::fabs(dot(r, cam.right)) < kRelatch || (arc && dot(r, cam.right) < kRelatch)) {
            *relatched = on;
            r = cam.right;
        }
        on = true;
        right = r;
        Frame f;
        f.up = cam.up;
        f.right = r;
        f.back = cross(cam.up, r);
        return f;
    }
};

// ---- places
// SPACE: direction of candidate i (0..3) for rank k: rank 0 right, left, behind, in front; rank 1 left, right, behind, in front;
// rank 2 behind, right, left, in front.
inline V3 spaceDir(const Frame& f, int rank, int i) {
    static const int8_t order[3][4] = {{0, 1, 2, 3}, {1, 0, 2, 3}, {2, 0, 1, 3}};   // 0 right, 1 left, 2 back, 3 front
    const int r = rank < 0 ? 0 : rank > 2 ? 2 : rank;
    const int k = order[r][i < 0 ? 0 : i > 3 ? 3 : i];
    switch (k) {
    case 0: return f.right;
    case 1: return mul(f.right, -1.0f);
    case 2: return f.back;
    default: return mul(f.back, -1.0f);
    }
}

// ARC: offset from the photo's anchor of candidate i (0 its place, 1 mirrored, 2 at half the radius) for rank k of n puppets.
// Angles from straight behind, positive to the camera's right: 1 puppet: 40; 2: 45, -45; 3: 65, -65, 0 (further back).
inline V3 arcOffset(const Frame& f, int rank, int n, int i) {
    if (n < 1) n = 1;
    if (n > 3) n = 3;
    if (rank < 0) rank = 0;
    if (rank >= n) rank = n - 1;
    static const float deg[3][3] = {{40.0f, 0.0f, 0.0f}, {45.0f, -45.0f, 0.0f}, {65.0f, -65.0f, 0.0f}};
    float a = deg[n - 1][rank] * 0.017453293f;
    float radius = kArcRadius * ((n == 3 && rank == 2) ? kArcMiddle : 1.0f);
    if (i == 1) a = -a;
    if (i == 2) radius *= 0.5f;
    return add(mul(f.back, radius * std::cos(a)), mul(f.right, radius * std::sin(a)));
}

// ---- springs (critically damped, exact step: stable for any dt)
inline void springStep(float* x, float* v, float target, float dt, float settle) {
    const float w = 4.74f / settle;   // (1 + wt) e^-wt = 5 % at wt = 4.74
    const float e = std::exp(-w * dt);
    const float d = *x - target;
    const float t = (*v + w * d) * dt;
    *v = (*v - w * t) * e;
    *x = target + (d + t) * e;
}

struct Spring3 {
    V3 x, v;
    void reset() { x = v = v3(0.0f, 0.0f, 0.0f); }
    void snap(const V3& to) { x = to; v = v3(0.0f, 0.0f, 0.0f); }
    void step(const V3& target, float dt = kDt, float settle = kSettle) {
        springStep(&x.x, &v.x, target.x, dt, settle);
        springStep(&x.y, &v.y, target.y, dt, settle);
        springStep(&x.z, &v.z, target.z, dt, settle);
    }
};

// ---- per puppet
struct SlotIn {
    bool eligible;   // bound to a present player, drawn (not hidden), not in a bubble, not dead
    V3 pos;          // true position (its STATE)
    bool onGround;   // its STATE says so
    bool inDemo;     // its STATE says it is in a demo
};

struct TurnIn {
    bool active;     // a session, a live scene, the local player exists
    V3 anchor;       // the local player
    Frame cam;       // the camera's frame on the local gravity's tangent plane (makeFrame)
    bool cutscene;   // local demo, scenario opening / starter, star-get, bound (launch star, pipe, ...)
    bool spawn;      // the spawn window (the first kSpawnTurns playable turns of the scene)
    bool star;       // the star-get demo in Play Together: the team photo
};

struct Slot {
    uint8_t mode;    // Mode
    uint8_t why;     // Why it is on (or why it went off, for the log)
    uint8_t event;   // Event of the last decide()
    uint8_t ground;  // candidates with ground (bit i), from the caller's ground checks (0xF until checked: see groundKnown)
    bool groundKnown;  // the probe ran since the space / group / photo began (0xF alone cannot tell: it is also the flat-floor result)
    uint16_t grace;
    bool hold;       // the grace is over and it still stands inside (the local player / its group): kept spaced, from holdPos
    V3 holdPos;
    uint8_t prev;    // the mode before this turn's decisions (events)
    uint8_t grank;   // GROUP: its place among the group's members (0..2)
    int8_t groot;    // GROUP: the slot whose true place the group stands at (drawn there)
    V3 gpos;         // that place
    int8_t side;     // +1 / -1: a lone puppet's side (right / left) latched when the space began, 0 = by rank
    Spring3 ofs;
    float face, faceV;
    V3 lastPos;      // last true position (appear test, jumps)
    bool lastValid;
    void reset() {
        mode = MODE_NONE; why = WHY_NONE; event = EV_NONE; ground = 0xF; groundKnown = false; grace = 0; side = 0;
        hold = false; holdPos = v3(0.0f, 0.0f, 0.0f); prev = MODE_NONE; grank = 0; groot = -1; gpos = v3(0.0f, 0.0f, 0.0f);
        ofs.reset(); face = faceV = 0.0f; lastPos = v3(0.0f, 0.0f, 0.0f); lastValid = false;
    }
};

struct SlotOut {
    V3 offset;     // add to the true position for the drawn one
    float face;    // 0..1: turn this much toward the camera
    bool appear;   // moved a long way in one step: play the arrival grow / fade-in
};

struct Formation {
    enum { SLOTS = 4 };
    Slot s[SLOTS];
    Latch latch;
    Frame frame;           // this turn's (latched) frame, valid after begin()
    bool frameOk;
    bool relatched;
    bool arcOn;            // the photo's anchor is set
    V3 arcAnchor;          // the local player where the star-get demo began
    uint32_t relatches;    // (stats for the log)
    V3 hist[kLagTurns];    // the local player's last positions (hist[histHead] newest)
    int histN, histHead;
    int nRemote;           // the other players present (the caller sets it before decide(); 0 = unknown: sides by rank only)

    void reset() {
        nRemote = 0;
        for (int i = 0; i < SLOTS; i++) s[i].reset();
        latch.reset();
        frameOk = relatched = arcOn = false;
        arcAnchor = v3(0.0f, 0.0f, 0.0f);
        relatches = 0;
        histN = histHead = 0;
    }

    // The nearest of the local player's recent positions to pos (the puppet's lagged true position): *at, its distance.
    float matched(const TurnIn& t, const V3& pos, V3* at) const {
        V3 best = t.anchor;
        float bd = len(sub(pos, t.anchor));
        for (int k = 0; k < histN; k++) {
            const V3& h = hist[(histHead - k + kLagTurns) % kLagTurns];
            const float d = len(sub(pos, h));
            if (d < bd) { bd = d; best = h; }
        }
        if (at != nullptr) *at = best;
        return bd;
    }

    bool anyOn() const {
        for (int i = 0; i < SLOTS; i++) {
            if (s[i].mode != MODE_NONE || len(s[i].ofs.x) > 0.5f || s[i].face > 0.002f) return true;
        }
        return false;
    }

    // Once a turn before the slots. Leaves the latch alone while nothing is on (the next engagement latches the camera then).
    void begin(const TurnIn& t, bool camOk) {
        relatched = false;
        frameOk = false;
        if (!t.active || !camOk) {
            histN = 0;
            return;
        }
        if (histN > 0 && len(sub(t.anchor, hist[histHead])) > kAppear) histN = 0;   // (a warp: the old path is not ours any more)
        histHead = (histHead + 1) % kLagTurns;
        hist[histHead] = t.anchor;
        if (histN < kLagTurns) histN++;
        if (!t.star) arcOn = false;
        else if (!arcOn) { arcOn = true; arcAnchor = t.anchor; }
        frame = latch.update(t.cam, &relatched, t.star);
        frameOk = true;
        if (relatched) relatches++;
    }
    // Once a turn after the slots.
    void end() {
        if (!anyOn()) latch.reset();
    }

    // the photo's anchor follows the local player sideways, never up (the star-get jump)
    V3 photoAnchor(const TurnIn& t) const {
        if (!arcOn || !frameOk) return t.anchor;
        return sub(t.anchor, mul(frame.up, dot(sub(t.anchor, arcAnchor), frame.up)));
    }

    // Mode for this turn. Sets s.event (on / off) and s.why.
    uint8_t decide(int slot, const TurnIn& t, const SlotIn& in) {
        Slot& q = s[slot];
        q.event = EV_NONE;
        q.prev = q.mode;
        const uint8_t was = q.mode == MODE_GROUP ? (uint8_t)MODE_NONE : q.mode;   // (the group is decided after, by decideGroups)
        uint8_t mode = was;
        uint8_t why = q.why;
        if (!t.active || !frameOk || !in.eligible || !finite3(in.pos)) {
            mode = MODE_NONE;
            why = WHY_HIDDEN;
        } else {
            const float dist = matched(t, in.pos, nullptr);
            const bool reason = t.cutscene || t.spawn || in.inDemo;
            // (a puppet farther away whose own console runs a demo too is in the star-get as well: all players are pulled into it)
            if (t.star && (was == MODE_ARC || dist < kGather || in.inDemo)) {
                mode = MODE_ARC;
                why = WHY_STAR;
            } else if (was == MODE_ARC) {
                mode = MODE_NONE;   // (the demo is over: back to its own place; space again below if still close)
                why = WHY_STAR_END;
            }
            if (mode != MODE_ARC) {
                if (mode == MODE_SPACE) {
                    if (dist > kRelease) {
                        mode = MODE_NONE;
                        why = WHY_APART;
                    } else if (reason) {
                        q.grace = kGraceTurns;
                        q.hold = false;
                    } else if (q.grace == 0) {
                        // still truly inside the player after the grace: do not slide it back into him; keep the space until it moves
                        if (!q.hold) { q.hold = true; q.holdPos = in.pos; }
                        if (len(sub(in.pos, q.holdPos)) > kHoldMove) {
                            mode = MODE_NONE;
                            why = WHY_GRACE;
                        }
                    } else {
                        q.grace--;
                    }
                } else if (dist < kEngage && reason) {
                    mode = MODE_SPACE;
                    why = t.spawn && !t.cutscene && !in.inDemo ? WHY_SPAWN : WHY_DEMO;
                    q.grace = kGraceTurns;
                    q.hold = false;
                    q.ground = 0xF; q.groundKnown = false;
                    q.side = 0;
                    if (nRemote == 1) {
                        V3 at;
                        matched(t, in.pos, &at);
                        const float lat = dot(flatten(sub(in.pos, at), frame.up), frame.right);
                        if (lat > kSideMin) q.side = 1;
                        else if (lat < -kSideMin) q.side = -1;
                    }
                }
            }
        }
        if (mode != was) {
            q.event = mode == MODE_NONE ? EV_OFF : EV_ON;
            if (mode != MODE_NONE && was != MODE_NONE) q.event = EV_ON;   // (arc -> space: a new reason)
        }
        if (mode == MODE_ARC && was != MODE_ARC) { q.ground = 0xF; q.groundKnown = false; }
        q.mode = mode;
        q.why = why;
        return mode;
    }

    // Puppets that overlap EACH OTHER away from the local player (the others' arrival all on the script's path, one console a bit
    // behind): after decide() of every slot, in slot order. The lowest slot of a cluster (puppets closer than kEngage, kRelease once on)
    // stays at its true place, the others take places around it by their number in the cluster (the same rank maths as around the
    // local player: right, left, behind; in the air the same fixed offsets, a small V). Same reasons, grace and hold as the local space.
    void decideGroups(const TurnIn& t, const SlotIn* in) {
        for (int i = 0; i < SLOTS; i++) {
            Slot& q = s[i];
            const bool wasGroup = q.prev == MODE_GROUP;
            if (q.mode != MODE_NONE) continue;   // (the local space / photo decided)
            if (!t.active || !frameOk || !in[i].eligible || !finite3(in[i].pos)) {
                if (wasGroup) { q.event = EV_OFF; q.why = WHY_HIDDEN; }
                continue;
            }
            const bool reasonMine = t.cutscene || t.spawn || in[i].inDemo;
            int found = -1;
            for (int j = 0; j < i && found < 0; j++) {
                if (!in[j].eligible || !finite3(in[j].pos) || (s[j].mode != MODE_NONE && s[j].mode != MODE_GROUP)) continue;
                const float lim = (wasGroup && (q.groot == j || q.groot == s[j].groot)) ? kRelease : kEngage;
                if (len(sub(in[i].pos, in[j].pos)) < lim) found = j;
            }
            if (found < 0) {
                if (wasGroup) { q.event = EV_OFF; q.why = WHY_APART; }
                continue;
            }
            int r = found;
            if (s[found].mode == MODE_GROUP && s[found].groot >= 0 && s[found].groot < SLOTS &&
                len(sub(in[i].pos, in[s[found].groot].pos)) < (wasGroup ? kRelease : kEngage)) {
                r = s[found].groot;
            }
            const bool reason = reasonMine || in[r].inDemo || in[found].inDemo;
            bool on = false;
            if (reason) {
                on = true;
                q.grace = kGraceTurns;
                q.hold = false;
            } else if (wasGroup) {
                if (q.grace > 0) {
                    q.grace--;
                    on = true;
                } else {
                    if (!q.hold) { q.hold = true; q.holdPos = in[i].pos; }
                    if (len(sub(in[i].pos, q.holdPos)) <= kHoldMove) on = true;
                    else q.why = WHY_GRACE;
                }
            }
            if (!on) {
                if (wasGroup) q.event = EV_OFF;
                continue;
            }
            int rank = 0;
            for (int k = 0; k < i; k++) {
                if (s[k].mode == MODE_GROUP && s[k].groot == r) rank++;
            }
            q.mode = MODE_GROUP;
            q.groot = (int8_t)r;
            q.gpos = in[r].pos;
            q.grank = (uint8_t)(rank > 2 ? 2 : rank);
            if (!wasGroup) {
                q.event = EV_ON;
                q.why = t.spawn && !t.cutscene && !in[i].inDemo ? WHY_SPAWN : WHY_DEMO;
                q.ground = 0xF; q.groundKnown = false;
            }
        }
    }

    // rank 0 / 1 swap for a lone puppet that stands on the other side already
    int placeRank(int slot, int rank) const {
        const int sd = s[slot].side;
        if (nRemote == 1 && rank == 0 && sd != 0) return sd > 0 ? 0 : 1;
        return rank;
    }

    // Does the slot's place need ground (the caller checks the candidates)?
    bool needsGround(int slot, const SlotIn& in) const {
        const Slot& q = s[slot];
        return ((q.mode == MODE_SPACE || q.mode == MODE_GROUP) && in.onGround) || q.mode == MODE_ARC;
    }

    // The caller's ground probe (rays + area queries): at the start of a space / group / photo, then every 8th turn (review2 #3:
    // 0xF is also the normal "all four have ground" result, so it cannot mean "not probed yet").
    bool wantsGroundProbe(int slot, uint32_t turn) const {
        return !s[slot].groundKnown || ((turn + (uint32_t)slot) & 7u) == 0;
    }
    void setGround(int slot, uint8_t m) {
        s[slot].ground = m;
        s[slot].groundKnown = true;
    }

    // Candidate i's place (world) for the caller's ground check.
    V3 candidate(int slot, const TurnIn& t, int rank, int n, int i) const {
        if (s[slot].mode == MODE_ARC) return add(photoAnchor(t), arcOffset(frame, rank, n, i));
        if (s[slot].mode == MODE_GROUP) return add(s[slot].gpos, mul(spaceDir(frame, s[slot].grank, i), kSpacing));
        return add(t.anchor, mul(spaceDir(frame, placeRank(slot, rank), i), kSpacing));
    }
    static int candidates(uint8_t mode) { return mode == MODE_ARC ? 3 : 4; }

    // The offset the slot wants this turn (before the caller's ground correction and the spring).
    V3 target(int slot, const TurnIn& t, const SlotIn& in, int rank, int n) const {
        const Slot& q = s[slot];
        if (q.mode == MODE_NONE || !frameOk) return v3(0.0f, 0.0f, 0.0f);
        const bool ground = needsGround(slot, in);
        int pick = 0;
        if (ground) {
            pick = -1;
            for (int i = 0; i < candidates(q.mode); i++) {
                if (q.ground & (1u << i)) { pick = i; break; }
            }
        }
        if (q.mode == MODE_ARC) {
            if (pick < 0) pick = 0;   // no ground anywhere: its place, in the air
            return sub(add(photoAnchor(t), arcOffset(frame, rank, n, pick)), in.pos);
        }
        if (pick < 0) return v3(0.0f, 0.0f, 0.0f);   // no ground beside the player (a narrow path): no push
        const V3 dir = spaceDir(frame, q.mode == MODE_GROUP ? (int)q.grank : placeRank(slot, rank), pick);
        V3 at;
        if (q.mode == MODE_GROUP) at = q.gpos;
        else matched(t, in.pos, &at);
        const V3 d = sub(in.pos, at);
        const float along = dot(flatten(d, frame.up), dir);
        float push = kSpacing - along;
        if (push < 0.0f) push = 0.0f;
        return mul(dir, push * falloff(len(d)));
    }

    // Moves the drawn offset toward target (spring), or there at once when the drawn position would travel more than kAppear.
    SlotOut finish(int slot, const SlotIn& in, const V3& target, float faceTarget) {
        Slot& q = s[slot];
        SlotOut o;
        o.appear = false;
        if (!in.eligible || !finite3(in.pos) || !finite3(target)) {
            q.ofs.reset();
            q.face = q.faceV = 0.0f;
            q.lastValid = false;
            o.offset = q.ofs.x;
            o.face = 0.0f;
            return o;
        }
        const V3 lastDrawn = add(q.lastPos, q.ofs.x);
        if (q.lastValid && q.mode != MODE_NONE && len(sub(in.pos, q.lastPos)) > kAppear) {
            q.ofs.x = sub(lastDrawn, in.pos);   // its true position jumped (its own respawn): the drawn one stays put
        }
        const V3 want = add(in.pos, target);
        const bool far = len(sub(target, q.ofs.x)) > kAppear && (!q.lastValid || len(sub(want, lastDrawn)) > kAppear);
        if (far) {
            q.ofs.snap(target);
            o.appear = true;
        } else {
            q.ofs.step(target);
        }
        springStep(&q.face, &q.faceV, faceTarget, kDt, kSettle);
        if (q.face < 0.0f) q.face = 0.0f;
        if (q.face > 1.0f) q.face = 1.0f;
        if (q.mode == MODE_NONE && len(q.ofs.x) < 0.05f && len(q.ofs.v) < 0.05f) q.ofs.reset();   // (settled exactly at home)
        q.lastPos = in.pos;
        q.lastValid = true;
        o.offset = q.ofs.x;
        o.face = q.face;
        return o;
    }
};

// ---- ranks: the remote players by slot (stable while the roster is)
inline int rankOf(int slot, int localSlot, unsigned presentMask) {
    if (slot == localSlot || !(presentMask & (1u << slot))) return -1;
    int r = 0;
    for (int i = 0; i < slot; i++) {
        if (i != localSlot && (presentMask & (1u << i))) r++;
    }
    return r > 2 ? 2 : r;
}
inline int remoteCount(int localSlot, unsigned presentMask) {
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (i != localSlot && (presentMask & (1u << i))) n++;
    }
    return n;
}

// ---- rotations (row-major 3x3, columns = the model's axes in the world, as the puppet's mRotMtx)
inline void matMul(const float a[3][3], const float b[3][3], float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i][j] = r[i][j];
    }
}
// rotation by angle about the unit axis (Rodrigues)
inline void axisAngle(const V3& k, float angle, float m[3][3]) {
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    m[0][0] = t * k.x * k.x + c;       m[0][1] = t * k.x * k.y - s * k.z; m[0][2] = t * k.x * k.z + s * k.y;
    m[1][0] = t * k.x * k.y + s * k.z; m[1][1] = t * k.y * k.y + c;       m[1][2] = t * k.y * k.z - s * k.x;
    m[2][0] = t * k.x * k.z - s * k.y; m[2][1] = t * k.y * k.z + s * k.x; m[2][2] = t * k.z * k.z + c;
}
// the shortest rotation taking unit a onto unit b (false: no rotation needed or undefined)
inline bool rotBetween(const V3& a, const V3& b, float m[3][3]) {
    V3 k = cross(a, b);
    const float s = len(k);
    const float c = dot(a, b);
    if (!(s > 1e-5f) || !std::isfinite(s)) return false;   // parallel (or opposite: not a tilt we make)
    if (c < -0.5f) return false;                             // more than 120 degrees: another surface, leave it
    k = mul(k, 1.0f / s);
    axisAngle(k, std::atan2(s, c), m);
    return true;
}
inline V3 column(const float m[3][3], int j) { return v3(m[0][j], m[1][j], m[2][j]); }

// The drawn rotation: rot tilted from the gravity up at its true place (upTrue) to the one at its drawn place (upDrawn), then
// turned about its (new) up by `face` of the way so that its front (column 2) looks at the camera (toCam: from it toward the camera).
inline void drawnRotation(const float rot[3][3], const V3& upTrue, const V3& upDrawn, const V3& toCam, float face, float out[3][3]) {
    float r[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[i][j] = rot[i][j];
    }
    float m[3][3];
    if (rotBetween(upTrue, upDrawn, m)) matMul(m, r, r);
    if (face > 0.001f) {
        V3 up = column(r, 1);
        if (normalize(&up)) {
            V3 front = flatten(column(r, 2), up);
            V3 want = flatten(toCam, up);
            if (normalize(&front) && normalize(&want)) {
                const float ang = std::atan2(dot(cross(front, want), up), dot(front, want));
                axisAngle(up, ang * (face > 1.0f ? 1.0f : face), m);
                matMul(m, r, r);
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out[i][j] = r[i][j];
    }
}

}  // namespace MpFormation
