// Galaxy 3.0 multiplayer: the puppet demo's script and canned poses, pure (owner: demo; no game headers; host tested in
// tests/mp/demo_test.cpp). The game side (MpDemo.cpp) applies what viewAt() says to the mock roster and feeds poses made by makePose()
// to the puppets as STATE messages, exactly as a remote console's packets would arrive (no socket, no session).
//
// The script is a function of the time since the demo began, as a fraction u = t / secs of the demo's length (default 60 s):
//   0.03 .. 0.30   set A (Luigi, Toad, Cosmic Mario) joins on slots 1..3; Luigi goes through the Boo / Fire / Ice forms
//   0.30 .. 0.34   set A leaves
//   0.34 .. 0.97   set B (Mario, Yellow Toad, Cosmic Luigi) joins; Mario goes through Bee, Rainbow, Spring, Flying, Bone
//   0.62           a greeting toast; 0.66 .. 0.76 Yellow Toad in a bubble (toasts: bubbled, popped); 0.70 .. 0.82 the pointer of Cosmic Luigi
//   0.80           one card (the team star card); connection bars: weak (slot 2, 0.45..0.60), lost (slot 3, 0.60..0.70)
//   0.97 .. 1.00   everybody leaves; the demo ends
// Every activity (walk a small circle, jump, spin, crouch; swim when the player is in water) lasts 1/20 of the demo and each puppet
// starts at another one, so every activity is on screen all the time.
#pragma once

#include <math.h>
#include <stdint.h>

#include "Game/MP/MpPoseMath.hpp"
#include "port/mp_proto.h"

namespace MpDemoLogic {

enum Act : uint8_t { ACT_WAIT = 0, ACT_WALK, ACT_JUMP, ACT_SPIN, ACT_CROUCH, ACT_SWIM, ACT_COUNT };
enum { kSlots = 3 };   // remote players shown at once (slots 1..3)

// The characters of the two sets (MpLook values), by remote slot 1..3.
static const uint8_t kSetA[kSlots] = {MP_LOOK_LUIGI, MP_LOOK_TOAD, MP_LOOK_COSMIC};
static const uint8_t kSetB[kSlots] = {MP_LOOK_MARIO, MP_LOOK_YELLOW_TOAD, MP_LOOK_COSMIC_LUIGI};

static const float kJoinA[kSlots] = {0.03f, 0.045f, 0.06f};
static const float kLeaveA[kSlots] = {0.30f, 0.31f, 0.32f};
static const float kJoinB[kSlots] = {0.34f, 0.35f, 0.36f};
static const float kLeaveB[kSlots] = {0.97f, 0.975f, 0.98f};
static const float kEnd = 1.0f;

struct View {
    bool present[kSlots];
    uint8_t look[kSlots];
    uint8_t form[kSlots];      // MpForm
    uint8_t act[kSlots];       // Act
    float actAge[kSlots];      // seconds since the activity began
    bool bubble;               // slot 2 floats in a bubble
    bool pointer;              // slot 3's pointer is held
    int ping[kSlots];          // ms
    bool quiet[kSlots];        // lost
    bool done;
};

// An activity cycle per puppet: the same five, rotated by the slot so the three do not move alike. In water everybody swims.
static inline uint8_t cycleAct(int slot, int index, bool inWater) {
    static const uint8_t cycle[5] = {ACT_WALK, ACT_JUMP, ACT_SPIN, ACT_CROUCH, ACT_WALK};
    if (inWater) return ACT_SWIM;
    return cycle[(index + slot * 2) % 5];
}

static inline uint8_t formAt(int set, int slot, float u) {
    if (slot != 0) return MP_FORM_NORMAL;   // (only the first puppet shows forms; the other characters of a set are Toad / Cosmic: no forms)
    if (set == 0) {
        if (u >= 0.10f && u < 0.14f) return MP_FORM_BOO;
        if (u >= 0.14f && u < 0.18f) return MP_FORM_FIRE;
        if (u >= 0.18f && u < 0.22f) return MP_FORM_ICE;
        return MP_FORM_NORMAL;
    }
    if (u >= 0.40f && u < 0.45f) return MP_FORM_BEE;
    if (u >= 0.45f && u < 0.50f) return MP_FORM_RAINBOW;
    if (u >= 0.50f && u < 0.55f) return MP_FORM_SPRING;
    if (u >= 0.55f && u < 0.60f) return MP_FORM_FLYING;
    if (u >= 0.60f && u < 0.65f) return MP_FORM_BONE;
    if (u >= 0.65f && u < 0.70f) return MP_FORM_BOO;   // (Mario as a Boo too: the see-through list on the other model)
    return MP_FORM_NORMAL;
}

static inline void viewAt(float u, float secs, bool inWater, View* v) {
    for (int i = 0; i < kSlots; i++) {
        v->present[i] = false;
        v->look[i] = kSetA[i];
        v->form[i] = MP_FORM_NORMAL;
        v->act[i] = ACT_WAIT;
        v->actAge[i] = 0.0f;
        v->ping[i] = 40 + 12 * i;
        v->quiet[i] = false;
    }
    v->bubble = u >= 0.66f && u < 0.76f;
    v->pointer = u >= 0.70f && u < 0.82f;
    v->done = u >= kEnd;
    const float actLen = 0.05f;   // of the demo
    for (int i = 0; i < kSlots; i++) {
        int set = -1;
        float since = 0.0f;
        if (u >= kJoinA[i] && u < kLeaveA[i]) { set = 0; since = u - kJoinA[i]; }
        else if (u >= kJoinB[i] && u < kLeaveB[i]) { set = 1; since = u - kJoinB[i]; }
        if (set < 0) continue;
        v->present[i] = true;
        v->look[i] = set == 0 ? kSetA[i] : kSetB[i];
        v->form[i] = formAt(set, i, u);
        const int idx = (int)(since / actLen);
        v->act[i] = cycleAct(i, idx, inWater);
        v->actAge[i] = MpPoseMath::clampf((since - (float)idx * actLen) * secs, 0.0f, actLen * secs);   // (float rounding at the seams)
    }
    // the connection bars: slot 2 weak, then slot 3 lost
    if (u >= 0.45f && u < 0.60f) v->ping[1] = 320;
    if (u >= 0.60f && u < 0.70f) v->quiet[2] = true;
}

// ---- animations ----------------------------------------------------------------------------------------------------------------
// Mario's own bck names (MarioAnime, as the motion table lists them: the archive's file names, lower case); the pose carries them as the simple-bck hash (MP_POSE_SIMPLE_BCK), which every character's
// puppet resolves with the same table (Luigi / Cosmic Luigi through their swap table, Toad through MpToadMap).
static inline const char* actBck(uint8_t act) {
    switch (act) {
    case ACT_WALK: return "walk";
    case ACT_JUMP: return "jump";
    case ACT_SPIN: return "spinground";
    case ACT_CROUCH: return "squatwait";
    case ACT_SWIM: return "swimflutter";
    default: return "wait";
    }
}

// ---- canned poses --------------------------------------------------------------------------------------------------------------
struct Ref {            // the local player's frame
    float pos[3], up[3], front[3];
};

static inline void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static inline void norm3(float a[3]) {
    const float n = MpPoseMath::len3(a);
    if (n > 1.0e-6f) { a[0] /= n; a[1] /= n; a[2] /= n; }
}

static const float kRadius = 380.0f;       // the circle around the player
static const float kWalkSpeed = 3.2f;      // units per turn along the circle (a walk for Toad's map: 1.2 <= h < 9)
static const float kJumpHeight = 260.0f;
static const float kJumpSeconds = 0.9f;

// A puppet at orbit angle `theta` (radians, around the player), doing `act` for `age` seconds. `turn`: the sender turn.
static inline void makePose(const Ref& ref, float theta, uint8_t act, float age, uint8_t form, uint32_t turn, MpPoseF* p) {
    float up[3] = {ref.up[0], ref.up[1], ref.up[2]};
    norm3(up);
    // the tangent plane: e1 = the player's front without its up part, e2 = up x e1
    const float fu = MpPoseMath::dot3(ref.front, up);
    float e1[3] = {ref.front[0] - up[0] * fu, ref.front[1] - up[1] * fu, ref.front[2] - up[2] * fu};
    if (MpPoseMath::len3(e1) < 1.0e-4f) { e1[0] = 1.0f; e1[1] = 0.0f; e1[2] = 0.0f; }
    norm3(e1);
    float e2[3];
    cross3(up, e1, e2);
    norm3(e2);
    const float c = cosf(theta), s = sinf(theta);
    float radial[3], tang[3];
    for (int i = 0; i < 3; i++) {
        radial[i] = c * e1[i] + s * e2[i];
        tang[i] = -s * e1[i] + c * e2[i];
    }
    // facing: walking along the circle; the others stand and look at the player
    float face[3];
    const bool walking = act == ACT_WALK || act == ACT_SWIM;
    for (int i = 0; i < 3; i++) face[i] = walking ? tang[i] : -radial[i];
    float right[3];
    cross3(up, face, right);
    norm3(right);
    float fwd[3];
    cross3(right, up, fwd);
    norm3(fwd);

    float lift = 0.0f, vup = 0.0f;
    if (act == ACT_JUMP) {
        const float ph = fmodf(age, kJumpSeconds) / kJumpSeconds;   // 0..1
        lift = kJumpHeight * 4.0f * ph * (1.0f - ph);
        vup = kJumpHeight * 4.0f * (1.0f - 2.0f * ph) / (kJumpSeconds * 60.0f);   // per turn
    } else if (act == ACT_SWIM) {
        lift = 8.0f * sinf(age * 3.0f);
    }

    memset(p, 0, sizeof *p);
    p->turn = turn;
    p->sceneSerial = 1;   // (mp_scene_serial() of the mock roster)
    p->zone = 0xFF;
    p->form = form;
    for (int i = 0; i < 3; i++) p->pos[i] = ref.pos[i] + radial[i] * kRadius + up[i] * lift;
    float m[3][4];
    memset(m, 0, sizeof m);
    for (int r = 0; r < 3; r++) { m[r][0] = right[r]; m[r][1] = up[r]; m[r][2] = fwd[r]; }
    MpPoseMath::quatFromMtx(m, p->quat);
    p->scale = 1.0f;
    const float speed = act == ACT_WALK ? kWalkSpeed : (act == ACT_SWIM ? 2.0f : 0.0f);
    for (int i = 0; i < 3; i++) p->vel[i] = tang[i] * speed + up[i] * vup;
    p->anim = MpPoseMath::animHash(actBck(act));
    float frame = age * 60.0f;
    float wrap = 24.0f;                          // (the looped clips; a one-shot is held on its last frame by the puppet)
    if (act == ACT_JUMP) { frame = fmodf(age, kJumpSeconds) * 60.0f; wrap = 1000.0f; }
    p->frame = fmodf(frame, wrap);
    p->rate = 1.0f;
    p->weights[0] = 1.0f;
    uint16_t f = MP_POSE_VISIBLE | MP_POSE_SIMPLE_BCK;
    if (act == ACT_SWIM) f |= MP_POSE_IN_WATER;
    else if (act != ACT_JUMP) f |= MP_POSE_ON_GROUND;
    p->flags = f;
}

// The angle step of a walking puppet per turn (it walks the circle at kWalkSpeed).
static inline float walkOmega() { return kWalkSpeed / kRadius; }

// A pointer ping's world point: on the ground near the player, a little in front, moving on a small circle.
static inline void pointerPoint(const Ref& ref, float t, float out[3]) {
    float up[3] = {ref.up[0], ref.up[1], ref.up[2]};
    norm3(up);
    const float fu = MpPoseMath::dot3(ref.front, up);
    float e1[3] = {ref.front[0] - up[0] * fu, ref.front[1] - up[1] * fu, ref.front[2] - up[2] * fu};
    if (MpPoseMath::len3(e1) < 1.0e-4f) { e1[0] = 1.0f; e1[1] = e1[2] = 0.0f; }
    norm3(e1);
    float e2[3];
    cross3(up, e1, e2);
    norm3(e2);
    const float a = t * 1.3f;
    for (int i = 0; i < 3; i++) out[i] = ref.pos[i] + e1[i] * (250.0f + 90.0f * cosf(a)) + e2[i] * (150.0f * sinf(a)) + up[i] * 40.0f;
}

}  // namespace MpDemoLogic
