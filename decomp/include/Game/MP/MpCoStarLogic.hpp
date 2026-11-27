// Galaxy 3.0 multiplayer: Co-Star help from the other players' Star Pointers, pure rules (owner: costar). No game headers, no globals:
// the game side is MpCoStar.cpp, the host tests are tests/mp/costar_test.cpp.
//
// Galaxy's own second player (the Wii remote on channel 1) holds an enemy by pointing at it with A pressed: the enemy's own code asks
// MR::isStarPointerPointing2POnPressButton and runs its "bind" state (the freeze sound, the touch effect, the stopped animation).
// Here the other players' pointers (POINTER, 0x24: the world point under the sender's pointer, sent only while its button has been held
// for 8 turns) answer that question on every console, the sender's own included, so everybody sees the same enemy freeze.
//
// A pointer is untrusted input. It holds only:
//   - an enemy that is not a boss (the original's hold code asks for it; a switch, an NPC or a menu never gets the question answered),
//   - within reach of the point (the point is the map hit behind the target: the reach is a bit wider than the target's radius),
//   - while the point is sane (finite, inside the world box, near the sender's own puppet),
//   - one target per pointer at a time, at most kMaxHoldTurns per hold, then a cool-down, and a budget that refills slowly
//     (so a pointer that toggles on and off cannot keep an enemy frozen for ever).
// Star Bits near the point (the original's second player collects them for Mario) fly to the local player on every console, at most a
// few a second per pointer; they are not counted again (the sender counted its own copy; MpStarBits).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MpCoStarLogic {

static constexpr int kMaxHoldTurns = 240;      // 4 s of holding one target
static constexpr int kCooldownTurns = 150;     // 2.5 s without any hold after that
static constexpr int kBudgetMax = 480;         // 8 s of holding in the bucket
static constexpr int kBudgetMinToStart = 30;   // 0.5 s needed to start a hold
static constexpr int kStaleTurns = 3;          // the holder stopped asking (died, was clipped): the claim is let go
static constexpr float kMaxCoord = 500000.0f;  // every coordinate of a pointer point
static constexpr float kMaxFromSender = 5000.0f;   // the point must be this close to the sender's own puppet
static constexpr float kReachScale = 2.2f;
static constexpr float kReachAdd = 60.0f;
static constexpr float kSweepReach = 200.0f;   // Star Bits this close to the point are swept
static constexpr int kSweepBurst = 3;
static constexpr int kSweepTurnsPerToken = 15; // 4 a second

static inline bool finite(float f) { return f == f && f > -1.0e30f && f < 1.0e30f; }

// A pointer point is used only when it is sane. ref = the sender's puppet position (null: unknown, so unusable).
static inline bool pointUsable(const float p[3], const float* ref) {
    if (ref == nullptr) return false;
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (!finite(p[i]) || !finite(ref[i]) || p[i] > kMaxCoord || p[i] < -kMaxCoord) return false;
        const float d = p[i] - ref[i];
        d2 += d * d;
    }
    return d2 <= kMaxFromSender * kMaxFromSender;
}
// The reach around a target of this radius (and a bad radius gets the smallest one).
static inline float holdReach(float radius) {
    if (!finite(radius) || radius < 0.0f) radius = 0.0f;
    if (radius > 2000.0f) radius = 2000.0f;
    return radius * kReachScale + kReachAdd;
}
static inline bool inReach(const float p[3], const float target[3], float radius) {
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (!finite(p[i]) || !finite(target[i])) return false;
        const float d = p[i] - target[i];
        d2 += d * d;
    }
    const float r = holdReach(radius);
    return d2 <= r * r;
}

static inline bool turnAfter(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }   // a is later than b

// One per pointer (slot). Feed it every question an actor asks; it answers for that actor.
struct HoldGate {
    uint32_t actor;        // the held target's id (0 = none)
    uint32_t startTurn, lastTurn, chargedTurn;
    uint32_t coolUntil;
    uint32_t budget;
    uint32_t budgetTurn;
    void reset(uint32_t turn) { actor = 0; startTurn = lastTurn = turn; chargedTurn = turn - 1; coolUntil = turn; budget = kBudgetMax; budgetTurn = turn; }
    void refill(uint32_t turn) {
        const uint32_t dt = turn - budgetTurn;
        if (dt == 0) return;
        if (dt > 0x7FFFFFFFu) { budgetTurn = turn; return; }   // the turn counter went backwards: no refill, no harm
        const uint32_t add = dt / 2;                           // +1 per 2 turns (an odd turn stays for the next call)
        budgetTurn += add * 2;
        const uint64_t v = (uint64_t)budget + add;
        budget = v > (uint64_t)kBudgetMax ? (uint32_t)kBudgetMax : (uint32_t)v;
        if (budget == (uint32_t)kBudgetMax) budgetTurn = turn;
    }
    void release() { actor = 0; }
    // pointerOn: a fresh pointer of this slot is on (its button held); near: this actor is within reach of the point.
    bool query(uint32_t turn, uint32_t id, bool pointerOn, bool near) {
        if (id == 0) return false;
        refill(turn);
        if (!pointerOn) { release(); return false; }
        if (actor != 0 && turnAfter(turn, lastTurn + kStaleTurns)) release();   // the holder stopped asking
        if (turnAfter(coolUntil, turn)) return false;                           // cooling down
        if (actor == id) {
            if (!near) { release(); return false; }
            if (turn - startTurn >= (uint32_t)kMaxHoldTurns || budget == 0) {
                release();
                coolUntil = turn + kCooldownTurns;
                return false;
            }
            lastTurn = turn;
            charge(turn);
            return true;
        }
        if (actor != 0 || !near || budget < (uint32_t)kBudgetMinToStart) return false;
        actor = id;
        startTurn = lastTurn = turn;
        charge(turn);
        return true;
    }
    void charge(uint32_t turn) {
        if (chargedTurn == turn) return;   // (many questions in one turn cost one turn)
        chargedTurn = turn;
        if (budget > 0) budget--;
    }
    bool holding(uint32_t turn) const { return actor != 0 && !turnAfter(turn, lastTurn + kStaleTurns); }
};

// Star Bits a pointer may sweep: a small bucket.
struct SweepGate {
    uint32_t tokens;
    uint32_t turn;
    void reset(uint32_t t) { tokens = kSweepBurst; turn = t; }
    bool take(uint32_t now) {
        const uint32_t dt = now - turn;
        if (dt <= 0x7FFFFFFFu && dt >= (uint32_t)kSweepTurnsPerToken) {
            const uint32_t add = dt / (uint32_t)kSweepTurnsPerToken;
            tokens = tokens + add > (uint32_t)kSweepBurst ? (uint32_t)kSweepBurst : tokens + add;
            turn += add * (uint32_t)kSweepTurnsPerToken;
        } else if (dt > 0x7FFFFFFFu) {
            turn = now;   // went backwards
        }
        if (tokens == 0) return false;
        tokens--;
        return true;
    }
};

}  // namespace MpCoStarLogic
