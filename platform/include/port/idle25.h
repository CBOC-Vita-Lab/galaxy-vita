// GXV_IDLE25 (idle25, 12 Nov 2026): exact idle skipping of actors whose movement() provably does nothing observable. The pure parts, shared by the
// game (decomp/src/Game/LiveActor/ActorTurnLod.cpp, the IDLE25 block) and the host test (tests/idle25.cpp, tools/test_idle25.sh). No game types.
//
// THE IDEA. LiveActor::movement() of a star bit that floats, a coin that waits or a petit piranha that sleeps costs 8-30 us and changes next to
// nothing: a step counter, an angle by +15, a countdown. For such an actor in such a nerve state the whole movement is a known function of a small
// STATE VECTOR (the "micro-step" below), PROVIDED nothing else could happen that turn. That proviso is the IDLE PREDICATE: every part of
// movement() (anim clock, gravity, sensor hits, nerve, control, binder, effects, light, sensor positions, sound, shadow request) is either proved
// a no-op for this state (cheap checks of the actual fields: if one fails the actor is simply not idle this turn and runs the real movement) or
// replaced by its exact one-step effect. The predicate is evaluated EVERY turn from the live fields, so there is no owed time, no margin for
// the player's speed and no wake-up lag: an actor leaves idle on the very turn its condition is met, and then runs the real movement of that turn.
// Nothing needs a catch-up because no step is ever left out; "catch-up" exists only as the n-step closed form of the micro-step (CatchUp), used by
// the host test as the oracle (n micro-steps = CatchUp(n), bit for bit) and by any caller that wants to skip n turns it can prove idle.
//
// FLOAT BIT-EXACTNESS. The only float accumulator is the star bit's Y angle: `rot += 15.0f; rot = repeat(rot, 0, 360)` with the game's own
// repeat (min + (f32)fmod(max + (value - min), max)): RepeatDegree below is that formula; the caller in the game uses MR::repeatDegree itself.
// CatchUp(n) repeats the addition n times (not a multiplication), so an angle that is not a multiple of 15 stays bit-identical too.
#pragma once

#include <math.h>
#include <stdint.h>

namespace port {
namespace idle25 {

// Families (the ids of MR::portMarkActorLod / actorlod23, as farlogic25).
enum Fam : uint8_t { kFamNone = 0, kFamStarPiece = 1, kFamCoin = 2, kFamPetit = 5, kFamN = 8 };

// Why an actor ran its real movement (census: "awake because"). kIdle is the skipped case.
enum Why : uint8_t {
    kIdle = 0,
    kWhyOff,      // actor not in a family / dead / still owed steps (ALOD)
    kWhyNerve,    // not in an idle nerve, or a nerve change pending, or the first step of a nerve
    kWhyHit,      // a sensor has a hit this turn
    kWhyAnim,     // an animation player is not at its fixpoint
    kWhyFx,       // effect keeper with a sync-bck / attribute effect
    kWhySound,    // a level sound or an animated sound is alive
    kWhyGeom,     // sensor position differs from what an update would write, binder not off, velocity not zero, gravity on, light / camera controller
    kWhyEvent,    // the turn of the star bit's periodic "gettable" effect
    kWhyPointer,  // the star pointer can point at it this turn
    kWhyNear,     // the player is within the nerve's own wake distance
    kWhyFlag,     // a class flag outside the idle shape (sand check, group clipping, ...)
    kWhyRail,     // star bit on its rail (RailMove: the group moves it; ACTIVE by design)
    kWhyN
};
inline const char* WhyName(int w) {
    static const char* const n[kWhyN] = {"idle", "off", "nerve", "hit", "anim", "effects", "sound", "geometry", "event", "pointer", "near", "flag", "rail"};
    return w >= 0 && w < kWhyN ? n[w] : "?";
}

// ---- the game's repeat(): min + (f32)fmod(max + (value - min), max), with min 0 and max 360 (MathUtil.hpp) ----
inline float RepeatDegree(float v) { return 0.0f + static_cast<float>(fmod(360.0f + (v - 0.0f), 360.0f)); }

// ---- star bit, nerve Floating ----------------------------------------------------------------------------------------------------------------
// movement(): Spine::update -> exeFloating (not the first step): rot.y += 15, repeat; tryGotJudge: if the player is farther than 15000 (or flag _1
// is off) _C8 = -1, else _C8 = (_C8 + 91) % 90 and at 20 the "GetAble" effect is emitted, and while mGettableDelayCounter < 0 the star pointer is
// asked; mStep++; then control(): mGettableDelayCounter is counted down to -1. updateBinder: pos += vel (binder off), Binder::clear().
constexpr float kSbTurn = 15.0f;
constexpr float kSbRange2 = 225000000.0f;  // 15000^2: tryGotJudge's reach
constexpr int32_t kSbEventC8 = 20;         // emitGettableEffect at this value of _C8

struct SbState {
    float rotY;
    int32_t step;   // Spine::mStep
    int32_t c8;     // StarPiece::_C8
    int32_t delay;  // StarPiece::mGettableDelayCounter
    bool operator==(const SbState& o) const { return rotY == o.rotY && step == o.step && c8 == o.c8 && delay == o.delay; }
};
// `active`: player within 15000 and flag _1 (the inputs of tryGotJudge's first test).
inline int32_t SbNextC8(int32_t c8, bool active) { return active ? (c8 + 91) % 90 : -1; }
inline bool SbEventNext(const SbState& s, bool active) { return active && SbNextC8(s.c8, active) == kSbEventC8; }
inline bool SbPointerAsked(const SbState& s, bool active) { return active && s.delay < 0; }
// One step of the idle micro-step. Only valid when the turn is idle (no event, pointer not pointing).
inline void SbAdvance(SbState& s, bool active) {
    s.rotY += kSbTurn;
    s.rotY = RepeatDegree(s.rotY);
    s.c8 = SbNextC8(s.c8, active);
    s.step += 1;
    if (s.delay >= 0) {
        s.delay -= 1;
    }
}
// n steps at a constant `active` (repeated addition: bit-exact with n calls of SbAdvance).
inline void SbCatchUp(SbState& s, int n, bool active) {
    for (int i = 0; i < n; ++i) {
        SbAdvance(s, active);
    }
}

// ---- coin, nerves Fix / FixHide / NonActive, and anything whose only state is the step counter ----------------------------------------------
struct StepState {
    int32_t step;
    bool operator==(const StepState& o) const { return step == o.step; }
};
inline void StepAdvance(StepState& s) { s.step += 1; }
inline void StepCatchUp(StepState& s, int n) { s.step += n; }

// ---- petit piranha, nerve NonActive (+ its AnimScaleController in Stop) ------------------------------------------------------------------------
// exeNonActive wakes when the player is within 5000 (isNearPlayerAnyTime: squared distance < d*d, no hidden test). Both steppers are counters.
constexpr float kPetWake = 5000.0f;
inline bool PetNear(float dist2) { return dist2 < kPetWake * kPetWake; }
struct PetState {
    int32_t step;       // Spine::mStep
    int32_t scaleStep;  // AnimScaleController's own Spine::mStep
    bool operator==(const PetState& o) const { return step == o.step && scaleStep == o.scaleStep; }
};
inline void PetAdvance(PetState& s) {
    s.step += 1;
    s.scaleStep += 1;
}

// ---- the anim-quiet rule on a J3DFrameCtrl (what ModelManager::update() would do to it) ---------------------------------------------------------
// J3DFrameCtrl::update: mState = 0; mFrame += mRate; then the attribute's range rules. It is a no-op exactly when the rate is 0, the state is already 0
// and the frame is inside [start, end - 1): then none of NONE / RESET / LOOP / REVERSE / LOOP_REVERSE touches anything (REVERSE-LOOP compares with end - 1).
inline bool FrameCtrlQuiet(float frame, float rate, uint8_t state, int start, int end) {
    return rate == 0.0f && state == 0 && frame >= static_cast<float>(start) && frame < static_cast<float>(end) - 1.0f;
}

// ---- verify (GXV_IDLE25=2) under GXV_ALOD (verifyfix31) -------------------------------------------------------------------------------------
// The compare in verify mode is "predicted idle micro-step" against "the real movement of this turn". With GXV_ALOD half-speed the closure handed to
// IdMovement as "real" can be a REST turn: it leaves the movement out and only books a step owed (replayed later), so the actor did not move and the
// compare (coin nerve step 1 -> 2 predicted, 1 real) fails on about every second turn. IDLE25 itself is exact against the ORIGINAL (un-skipped) game
// (mode 1 replaces the whole movement, ALOD included, by the micro-step on every idle turn, and an actor that owes steps is never idle), so the
// reference of the compare must be the original movement: under verify a rest turn is not taken (the actor makes its movement on this turn, owing
// nothing). `skip` is ALOD's decision, `verifying` is IDLE25=2 and the actor is an idle-family one.
inline bool AlodSkipUnderVerify(bool skip, bool verifying) {
#ifdef PORT_IDLE25_MUTANT_VERIFY_SKIP
    (void)verifying;
    return skip;  // (mutant: the old behaviour, the compare reference is a left-out turn)
#else
    return skip && !verifying;
#endif
}

// ---- census ------------------------------------------------------------------------------------------------------------------------------
struct FamCensus {
    uint64_t decisions = 0, idle = 0, wakes = 0, verifyChecks = 0, verifyBad = 0;
    uint64_t why[kWhyN] = {};
    uint64_t idleNs = 0, idleSamples = 0, realNs = 0, realSamples = 0;  // sampled (one in 16): cost of an idle turn (predicate + step) and of a real one
    uint64_t lastBadField = 0;
};

}  // namespace idle25
}  // namespace port
