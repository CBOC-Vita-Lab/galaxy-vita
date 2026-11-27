// GXV_ALOD (actorlod23, 8 Nov 2026): the "update LOD" of actors in Performance mode. Pure rules, shared by the game
// (decomp/src/Game/LiveActor/ActorTurnLod.cpp) and the host test (tests/actor_lod.cpp, tools/test_actor_lod.sh).
//
// An actor of a marked family (MR::portMarkActorLod: a class that runs LiveActor::movement and LiveActor::calcAnim unchanged)
// that is far from the player and outside the picture (or, with GXV_ALOD_TINY, a few pixels small on it) and that nothing
// can touch this turn moves on every second (or fourth) turn only. A left-out turn is not lost: its step is OWED and replayed,
// in order, right before the actor's next movement ("catch-up"), so the actor's own clock (animation frames, nerve steps,
// velocity, timers) makes the same steps in the same order as the original. Between two of its movements the actor simply
// stands one (or three) steps behind; nothing that could notice it is allowed to look (the gates below), and the moment
// anything could, the debt is paid before the actor's movement of that turn.
//
// The latch rule (as gamepar's GXV_HR_PLANTMOVE replay): XanimePlayer::updateAfterMovement advances the BCK frame only when
// the last calcAnm cleared _88; the turn that was left out had a calcAnim that would have cleared it. So every replayed step is
// followed by the clear its turn's calcAnim would have made (when the actor computes its animation at all).
#pragma once
#include <stdint.h>

namespace port {
namespace alod {

constexpr int kMaxRate = 4;  // half (2) or quarter (4) rate

// Is this turn left out? `turn` counts logic turns, `phase` spreads the actors over the turns (from the address), `owed` is
// the number of steps the actor already owes. A turn is left out unless it is the actor's turn of the cycle or the debt is
// already rate - 1 steps (so no actor ever stands more than rate - 1 steps behind).
inline bool SkipThisTurn(uint32_t turn, uint32_t phase, int rate, int owed) {
    if (rate <= 1 || owed < 0 || owed >= rate - 1) {
        return false;
    }
    return ((turn + phase) % static_cast< uint32_t >(rate)) != 0;
}

// The phase of an actor: from its address (heap blocks are 16-byte aligned), so neighbours fall on different turns.
inline uint32_t PhaseOf(const void* p) {
    const uintptr_t a = reinterpret_cast< uintptr_t >(p);
    return static_cast< uint32_t >((a >> 4) ^ (a >> 9));
}

// The distance the player must be beyond: the base distance, the actor's sensor reach (its farthest sensor's offset +
// radius, plus the model's radius), and what the player can cover while the actor stands behind (rate turns at the player's
// current speed, doubled).
inline float PlayerGate(float baseDist, float reach, float playerSpeed, int rate) {
    const float s = playerSpeed > 0.0f ? playerSpeed : 0.0f;
    return baseDist + reach + 2.0f * s * static_cast< float >(rate);
}

// The rate for an actor that may be left out: quarter when it is off-screen and farther than far4 from both the camera and
// the player, else half. maxRate caps it (GXV_ALOD_RATE).
inline int ChooseRate(bool offScreen, float camD2, float plyD2, float far4, int maxRate) {
    int r = 2;
    if (offScreen && far4 > 0.0f && camD2 > far4 * far4 && plyD2 > far4 * far4) {
        r = 4;
    }
    if (maxRate < r) {
        r = maxRate < 2 ? 1 : 2;
    }
    return r;
}

// The radius in pixels of a sphere of `radius` at `dist` from the eye, for a picture `screenH` pixels high with vertical
// field of view fovyDeg (tangent given): r * (H / 2) / (tan(fovy / 2) * d).
inline float PixelRadius(float radius, float dist, float tanHalfFovy, float screenH) {
    if (!(dist > 1.0f) || !(tanHalfFovy > 0.0f)) {
        return 1e9f;
    }
    return radius * (screenH * 0.5f) / (tanHalfFovy * dist);
}

// Catch-up: the owed steps, oldest first. A::liteStep() is the left-out turn's movement (LiveActor::portMovementLite in the game);
// A::latchAsCalcAnim() is what that turn's calcAnim did to the animation latch (clears XanimePlayer::_88 unless the actor does not
// compute its animation). Returns the steps paid.
template < class A > inline int PayOwed(A& a) {
    int n = 0;
    while (a.owed() > 0) {
        a.decOwed();
        a.liteStep();
        a.latchAsCalcAnim();
        ++n;
    }
    return n;
}

}  // namespace alod
}  // namespace port

// ------------------------------------------------------------------------------------------------- game-side entry points
// (decomp/src/Game/LiveActor/ActorTurnLod.cpp; C linkage so platform code and tests can name them)
#ifdef __cplusplus
extern "C" {
#endif
// 0 off, 1 on, 2 census only (decide, never leave out; count and time what mode 1 would leave out). "perf" = Performance mode only.
int port_alod_mode(void);
// The logic-turn counter (bumped at the top of SceneFunction::executeMovementList, every turn the scene runs).
extern uint32_t gPortAlodTurn;
void port_alod_turn_begin(void);
// The camera of this turn is final (after the camera and clipping lists): the view pyramid is taken from it.
void port_alod_camera_ready(void);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
class LiveActor;
namespace MR {
// GXV_ALOD: called at the end of a marked family's init (family 1..7: ActorTurnLod.cpp). Does nothing with GXV_ALOD off.
void portMarkActorLod(LiveActor* pActor, int family);
}  // namespace MR
// GXV_ALOD: takes back an actor's "not drawn in the next picture" stamp (GXV_ALOD_DRAWCULL).
void PortAlodUncull(LiveActor* pActor);
#endif
// NameObj::mFlag bit for "this NameObj is a LiveActor of a family that may be left out" (NameObj.cpp tests it).
#define PORT_NAMEOBJ_FLAG_ALOD 0x0080u
