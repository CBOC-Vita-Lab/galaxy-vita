// MP3 lodgate (13 Nov 2026): the performance features that decide, from the LOCAL camera, the local player's distance or a per-console clock, whether a piece of GAME STATE
// exists, is collidable, moves or is posed for a sensor are held at their state-neutral setting while a multiplayer session is active. Pure rules (no game types, no clocks, no
// globals but SessionLog): host-tested by tests/mpgate.cpp (tools/test_mpgate.sh). The stored settings are never touched: the hold is read at the use, so the feature is back the
// moment the session ends. Far logic (GXV_FARLOGIC, Quality step 3l) has its own rule in farlogic.h (LevelInSession), the first of this kind.
//
// SESSION HELD (D5, 13 Nov night). mp_active() goes false the moment the link is lost, but the session is not over: the core rejoins the same session on its own for up to 30 s
// (MP_UI_RECONNECTING), and the world this console plays in is the shared one it must still be in step with when it is back. Every gate below, the far-logic gate and the other
// per-console state-changing speed gates therefore read port_net_session_lockstep() (SessionHeld), not mp_active(). The states it covers (the arguments, all from the core under its lock):
//   active          LOBBY / PLAYING, hosting or joined (setActive(true) .. requestEnd): the session itself.
//   reconnecting    m_reconnect: the core lost the link and rejoins for up to 30 s (phase JOINING / CONNECTING, error NO_WIFI), the retries in between, until it works or gives up.
//   dyingReconnect  requestEnd() has run for a link loss (active already false) but finishEnd() has not yet started the reconnect: the one pump in between. Without it a gate opens for it.
//   rejoining       a rejoin of the session we left (resume after a sleep) is in flight; that path does not set m_reconnect. Cleared when joined, when the attempt ends, or when the player leaves.
// NOT held: idle (no session), a menu scan / a fresh join or host that has not yet got a world, LEAVING and a deliberate leave / kick / host-closed end (nothing is owed any more), a
// suspended console (asleep, the game is not running), a reconnect that gave up (the window ran out: back to IDLE), the "rejoin offered" menu (the player's choice, not a reconnect).

// Why each one is held (the audit is in the lodgate report; the short form):
//   kDecor   plant groups beyond the far-decor horizon (GXV_PB_DECOR, Quality steps 3f / 3j, GXV_FARFIELD tier 3) are CLIPPED: a clipped group's hit sensor and star-pointer target are
//            off, it moves nothing, and a group made for coins or star bits (PlantGroup::mIsStarPiece / declareCoin) pays them out when touched. Which groups are clipped depends on
//            the local camera and the local player only; a teammate's puppet shaking a group that is far from this console's camera would do nothing here.
//   kPose    the pose cuts (GXV_PB_ANIM and the Quality step 3h, GXV_CHARPOSE, GXV_FAMPOSE, GXV_FARLOGIC level 3's pose rule, the GXV_FARFIELD rate tier) leave an actor's joints
//            one to three turns old; for an actor with a hit sensor that follows a joint that is a different sensor position, decided from the local camera / player distance.
//            Only actors WITH a hit sensor are held (an actor without one is drawn from the old pose and nothing reads it: visual only).
//   kAlod    GXV_ALOD leaves far, off-picture actors' turns out (and replays them later): the draw order of the shared random numbers, the sensors' positions (up to 3 turns old) and
//            the order against other actors differ per console, by the local view cone and the local player's distance.
//   kIdle    GXV_IDLE25 replaces a star bit's / coin's / petit piranha's whole movement by a micro-step when "nothing could happen": the predicate knows the local player and the
//            local star pointer, not another player's pointer (MpCoStar::sweeps in StarPiece::tryGotJudge) or a teammate's bubble shot, so an idle star bit would ignore them.
//   kXskip   the black cover after an accepted skip is held up to GXV_XSKIP_HOLD_MS while THIS console's shader / texture work is not done: the cut (the starter's flight ends, Mario
//            is released, the BGM starts, a demo waiting for the cinema frame goes on) happens on a different turn on each console.
#pragma once

#include <stdint.h>

namespace port {
namespace mpgate {

inline bool SessionHeld(bool active, bool reconnecting, bool dyingReconnect, bool rejoining) { return active || reconnecting || dyingReconnect || rejoining; }

enum Feature : uint8_t { kDecor = 0, kPose, kAlod, kIdle, kXskip, kAnimLod, kPlantMove, kStarBatch, kFeatureN };

inline const char* Name(int f) {
    static const char* const n[kFeatureN] = {"far decor clip", "far pose cut", "actor update LOD", "idle skip", "skip cover hold", "far animation deferral", "far plant rest", "far star bit pose cut"};
    return f >= 0 && f < kFeatureN ? n[f] : "?";
}
inline const char* Why(int f) {
    static const char* const w[kFeatureN] = {
        "plant groups beyond the horizon would lose their hit sensor and coin / star bit payout by the local camera's distance",
        "a far actor with a hit sensor would read a pose one to three turns old by the local camera's distance",
        "far off-picture actors would skip turns by the local view and the local player's distance",
        "an idle star bit / coin would not see another player's pointer or a teammate's shot",
        "the cut after a skip would wait for this console's own shader work",
        "a far actor with a hit sensor would have its joints made once a picture, by this console's camera, player distance and picture pacing",
        "a far plant group would rest every second member's movement by the local player's distance, and a remote push would pay the owed step a frame out of phase",
        "a far star bit would leave its pose out by the local camera's distance"};
    return f >= 0 && f < kFeatureN ? w[f] : "?";
}
inline const char* Neutral(int f) {
    static const char* const s[kFeatureN] = {"no horizon: nothing clipped by distance", "poses every turn for actors with a hit sensor", "every actor moves every turn (owed steps paid)",
                                             "off: every movement runs in full", "no hold: the cut is made at once",
                                             "an actor with a hit sensor is made every turn", "every member moves every turn", "every star bit is posed every turn"};
    return f >= 0 && f < kFeatureN ? s[f] : "?";
}

// ---- the rules (one bool in, the setting out) ---------------------------------------------------------------------------------------------------------
// 0 = no horizon (the far-decor distance in game units, 0 = off)
inline float DecorHorizon(float horizon, bool mpActive) { return mpActive ? 0.0f : horizon; }
// May this actor's pose be left out (a pose cut asked to)? An actor with a hit sensor is posed every turn in a session.
inline bool PoseCutAllowed(bool mpActive, int sensorCount) { return !(mpActive && sensorCount > 0); }
// The turn gate of GXV_ALOD: open = the actors may be left out this turn
inline bool AlodGateOpen(bool open, bool mpActive) { return open && !mpActive; }
// GXV_IDLE25's mode (0 off, 1 on, 2 verify)
inline int IdleMode(int mode, bool mpActive) { return mpActive ? 0 : mode; }
// May a far actor's joints be deferred to once a picture (AnimLodDefers: the far rule, the plant / undrawn rules)? An actor with a hit sensor never is in a held session.
inline bool AnimDeferAllowed(bool held, int sensorCount) { return !(held && sensorCount > 0); }
// Cutscene skip (cutscene_skip.cpp): a full hold in a session is this player's VOTE and the skip waits for the others. While the session is held but not active (a reconnect: no votes
// arrive, and MpFlow::skipVote would answer true at once) the hold must wait, not skip locally: this console would come back past a cutscene the others are still in. Dialogue stays local.
inline bool SkipHoldWaits(bool held, bool active, bool isTalk) { return held && !active && !isTalk; }
// May a far plant group rest every second member's movement (GXV_HR_PLANTMOVE)? / may a far star bit's pose be left out (GXV_STARBATCH_FAR)? Never in a held session.
inline bool PlantRestAllowed(bool held) { return !held; }
inline bool StarBatchFarAllowed(bool held) { return !held; }
// May the black cover be held after a skip?
inline bool CoverHoldAllowed(bool mpActive) { return !mpActive; }
// Star Bit Reach (settingsnative, the player's pause/Settings choice 1..5): the wider reach would make a player's pointer collect Star Bits another player's does not, so in a Star Race
// (MP_MODE_RACE) the level is the game's own (1) AT THE USE (StarPointerUtil.cpp); the stored choice is never touched and is back the moment the race ends.
inline int StarBitReachLevel(int stored, bool starRace) { return starRace ? 1 : stored; }

// One line per feature per session. Due(f, mp): true the first time a feature is held in a session. Reset() at a session's end (or once per turn when none is active).
struct SessionLog {
    bool said[kFeatureN] = {};
    bool Due(int f, bool mpActive) {
        if (f < 0 || f >= kFeatureN) return false;
        if (!mpActive) { said[f] = false; return false; }
        if (said[f]) return false;
        said[f] = true;
        return true;
    }
    void Reset() { for (bool& b : said) b = false; }
};

}  // namespace mpgate
}  // namespace port

#ifdef __cplusplus
extern "C" {
#endif
// perf.cpp: one log line per feature per session ("mpgate: ..."), called from the gate sites only on their multiplayer branch; port_mpgate_idle is called once a turn
// and re-arms the lines when no session is active.
void port_mpgate_note(int feature);
void port_mpgate_idle(void);
#ifdef __cplusplus
}
#endif
