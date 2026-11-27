// Galaxy 3.0 multiplayer: coop extras, pure logic (owner: coop). No game headers: the host tests (tests/mp/coop_test.cpp) build it
// as is. The game side is MpCoop.cpp.
//
//  - Greeting emote (Super Mario Bros. Wonder's greeting, with what Galaxy ships): one button (the D-pad down: the game uses it
//    for nothing in play) or a stamp from the session menu. Rate limited (1 per second sent; receivers drop faster ones). The
//    gesture (Mario's "ghost win" pose, Toad's "Joy") plays only while the player stands idle on the ground, so it never
//    interrupts a move, and stops at the first stick / jump / crouch input.
//  - Pointer ping: holding the Star Pointer button (B) shows the point under your pointer to the others as the game's own 2P
//    pointer (DPDPointerSub) in your slot colour. <= 15 Hz, unreliable; show-only.
//  - Friend super jump: landing on a friend's head while holding A (jump) gives the game's own spring head bounce (the
//    ATYPE_PLAYER_AUTO_JUMP path of the Spring Topman / Jump Guarder heads) instead of the plain stomp.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "port/mp_proto.h"

namespace MpCoopLogic {

// ---- greeting emote --------------------------------------------------------------------------------------------------------
static constexpr int64_t kEmoteMinMs = 1000;        // sender: 1 per second
static constexpr int64_t kEmoteRecvMinMs = 800;     // receiver: a hostile or buggy peer cannot spam faster (jitter allowance)
static constexpr int kEmoteFlashTurns = 96;         // the name tag's hand icon flashes for 1.6 s
static constexpr int kEmoteGestureTurns = 150;      // the gesture is stopped after 2.5 s at the latest

struct RateGate {
    int64_t last;
    bool used;
    void reset() { last = 0; used = false; }
    // true = allowed (and recorded). A clock that went backwards re-arms the gate.
    bool allow(int64_t nowMs, int64_t minMs) {
        if (used && nowMs >= last && nowMs - last < minMs) return false;
        last = nowMs;
        used = true;
        return true;
    }
};

// What the local player is doing, as the game side reads it.
struct IdleIn {
    bool onGround;       // MarioActor movement state: standing on something
    bool jumping;
    bool defaultAnim;    // the animation is the default one (stand / walk blend), or the special idle (MarioWait)
    bool stickOn;        // the move stick is pushed
    float walkSpeed;     // Mario::mWalkSpeed
    bool plainStatus;    // Mario's status: none or the idle wait (not swimming, hanging, talking, first person, ...)
    bool busy;           // dead, in a demo, damaged, squatting, carrying, bound, skating, in water
    bool formOk;         // a form with Mario's own body animations (not Boo, Spring, Flying, bone)
};
static inline bool idleForGesture(const IdleIn& in) {
    return in.onGround && !in.jumping && in.defaultAnim && !in.stickOn && in.walkSpeed < 0.05f && in.walkSpeed > -0.05f &&
           in.plainStatus && !in.busy && in.formOk;
}

// The gesture stops at the first input that means "move" (like MarioWait's special idle), or when the player leaves the ground.
static inline bool gestureShouldStop(bool stickOn, bool jumpPressed, bool crouchHeld, bool onGround, int turnsPlayed) {
    return stickOn || jumpPressed || crouchHeld || !onGround || turnsPlayed >= kEmoteGestureTurns;
}

// The name tag's icon: on 8 turns, off 4, for kEmoteFlashTurns after the emote (-1 = none yet).
static inline bool flashOn(int turnsSince) {
    if (turnsSince < 0 || turnsSince >= kEmoteFlashTurns) return false;
    return (turnsSince % 12) < 8;
}

// The character's own voice clip (the player's voice SEs; Toad: the Kinopio NPC's "hey"). port12 >= 0: write that value to the
// sound's track port 12 after it starts (AudSystem::switchToLuigi's port: 20 = Luigi's voice, 0 = Mario's), so a Luigi puppet
// sounds like Luigi on a Mario console and the other way round. -1: leave it.
struct Voice {
    const char* se;
    int port12;
};
static inline Voice voiceFor(int character) {
    switch (character) {
    case MP_LOOK_LUIGI:
    case MP_LOOK_COSMIC_LUIGI: return Voice{"SE_PV_JUMP_JOY", 20};
    case MP_LOOK_TOAD:
    case MP_LOOK_YELLOW_TOAD: return Voice{"SE_SV_KINOPIO_TALK_HEY", -1};
    default: return Voice{"SE_PV_JUMP_JOY", 0};
    }
}

// ---- pointer ping ----------------------------------------------------------------------------------------------------------
static constexpr int64_t kPointerPeriodNs = 66666666;   // 1/15 s: sends on a fixed 15 Hz schedule (every 4th turn at 60/s)
static constexpr int kPointerHoldTurns = 8;           // B must be held this long first (a quick Star Bit shot is no ping)
static constexpr int64_t kPointerStaleMs = 400;       // a receiver hides a pointer it has heard nothing of for this long

// Sender: when to send what. Feed it every turn; it says "send on", "send off" (once, at release) or nothing.
enum PointerSend { POINTER_NONE = 0, POINTER_ON = 1, POINTER_OFF = 2 };
struct PointerSender {
    int held;            // turns B has been held with a valid pointer
    bool sending;        // a ping is out (the others see it)
    int64_t nextNs;      // the next send is due (a fixed-rate schedule: never more than 15 a second on average, no bursts)
    void reset() { held = 0; sending = false; nextNs = 0; }
    PointerSend step(bool holding, int64_t nowNs) {
        if (!holding) {
            held = 0;
            if (sending) {
                sending = false;
                return POINTER_OFF;
            }
            return POINTER_NONE;
        }
        if (held < kPointerHoldTurns) held++;
        if (held < kPointerHoldTurns) return POINTER_NONE;
        // (a clock that jumped back more than a period re-arms the schedule)
        if (sending && nowNs < nextNs && nextNs - nowNs <= kPointerPeriodNs) return POINTER_NONE;
        // on time: the next one a period after this one's due time (a late turn does not shift the schedule); a long gap
        // (or the first one) starts it again from now
        const bool onSchedule = sending && nowNs >= nextNs && nowNs - nextNs < kPointerPeriodNs;
        nextNs = (onSchedule ? nextNs : nowNs) + kPointerPeriodNs;
        sending = true;
        return POINTER_ON;
    }
};

// Receiver: one per remote slot. Smoothed towards the latest point (15 Hz in, 60 Hz out).
struct PointerView {
    bool on;
    bool shown;          // a position has been shown since it came on (the first one is not smoothed)
    float target[3];
    float pos[3];
    int64_t lastMs;
    void reset() { on = false; shown = false; lastMs = 0; for (int i = 0; i < 3; i++) target[i] = pos[i] = 0.0f; }
    void receive(const MpPointerMsg& m, int64_t nowMs) {
        lastMs = nowMs;
        if (!m.on) { on = false; shown = false; return; }
        if (!on) shown = false;
        on = true;
        for (int i = 0; i < 3; i++) target[i] = m.pos[i];
    }
    // once per turn: true = draw at pos
    bool step(int64_t nowMs) {
        if (on && (nowMs < lastMs || nowMs - lastMs > kPointerStaleMs)) { on = false; shown = false; }
        if (!on) return false;
        for (int i = 0; i < 3; i++) pos[i] = shown ? pos[i] + (target[i] - pos[i]) * 0.4f : target[i];
        shown = true;
        return true;
    }
};

// ---- super jump --------------------------------------------------------------------------------------------------------------
// The head's jump sensor is live only while the local player could land on it with the jump button held.
static inline bool superJumpArmed(bool interactionOn, bool jumpHeld, bool localOnGround, bool localDead) {
    return interactionOn && jumpHeld && !localOnGround && !localDead;
}

// ---- arrival: no pushing (mppause) -------------------------------------------------------------------------------------------
// Both players land on the same spawn point and the body push of two puppets/players would shove the other off the ledge. The bodies do not push
// while either one is in a demo (the fly-in, the landing), for kPushGraceTurns after the last demo turn and for the first kPushSceneTurns of a
// scene (the fly-in may not count as a demo on every stage). Bumps by stomps and spins are not part of this: they stay as designed.
constexpr uint32_t kPushGraceTurns = 180, kPushSceneTurns = 240;   // 3 s and 4 s at 60 turns a second
static inline bool pushHeld(uint32_t turn, uint32_t sceneStartTurn, uint32_t lastDemoTurn, bool lastDemoSeen, bool localDemo, bool remoteDemo) {
    if (localDemo || remoteDemo) return true;
    if (lastDemoSeen && turn - lastDemoTurn < kPushGraceTurns) return true;
    return turn - sceneStartTurn < kPushSceneTurns;
}

}  // namespace MpCoopLogic
