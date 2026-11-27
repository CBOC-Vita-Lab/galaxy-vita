// ratecut31 (17 Nov 2026, plan-perf31 B3): GXV_POSE_PHASE, the every-n-th-turn pose and animation cuts made on the turn a picture shows. The pure rules and
// the "pose age at draw" census; host model and mutants: tests/pose_phase.cpp (tools/test_pose_phase.sh). The glue: LiveActor.cpp (PbAnimSkips, the FARANIM /
// CHARPOSE / FAMPOSE / MATLOD cadences, the star-bit far pose), ActorTurnLod.cpp (port_starbatch_far_skip), PlantGroup.cpp (GXV_HR_PLANTMOVE), perf.cpp (the
// turn's phase and the census line, port_undrawn_turn_begin), cpulod.h (cpulod::SkipAt).
//
// THE TURN / PICTURE PHASE. A picture is drawn at the START of a drawn turn, from the state the turn before left (GameSystem::frameLoop: the flushes and draw()
// come before update() and calcAnim()). In a drawn + undrawn pair (Performance with 60 Assist / Smooth Motion, Quality's even 30) the state a real picture
// shows is the undrawn turn's; what the drawn turn's calc-anim makes is made again by the undrawn turn before any real picture uses it. 60 Assist's second
// picture (drawn at the undrawn turn's start) is the first one re-projected, plus the pair set (PortHrwSet.cpp tags those models in the first picture) drawn
// fresh from the pose the drawn turn's calc-anim made.
//   So: a pose left out on a SKIP TURN (a drawn turn that follows an undrawn one) is never seen, unless the model is in the pair set; a pose left out on any
//   other turn may be seen.
// The cuts choose their turns by the actor's address (GXV_PB_ANIM's and far-logic level 3's every-2nd alternation, GXV_FARANIM / GXV_CHARPOSE / GXV_FAMPOSE /
// GXV_MATLOD's every-n-th count, GXV_STARBATCH_FAR's parity) or by the group's own tick (GXV_HR_PLANTMOVE). Against the pair, half of the every-2nd actors
// show a pose one turn old in every picture; an every-4th actor (the stage table's cpulod=3 sets FARANIM / CHARPOSE / FAMPOSE / MATLOD to 4) shows 0, 1, 2,
// 3 turns old in turn, a 15 Hz step; and every slip of the pacer (a catch-up turn) or restart of an actor's count (near / far, a flight) moves the phase.
//
// GXV_POSE_PHASE=<0|1|2> (default 0 until the console A/B):
//   0  as before (bit for bit: the mode-0 paths below are the old functions, tested for identity).
//   1  exact for the picture: a cut leaves a pose out ONLY on a skip turn and makes it on every other turn; the every-3rd / every-4th members are capped at
//      every 2nd (aligned, they would still show a pose two turns old in every second picture); a model of 60 Assist's pair set (tagged this turn) is posed
//      every turn; GXV_STARBATCH_FAR's every-second-picture rule at the flush (the picture's own pose) is off. Pose age at draw = 0 by construction, except
//      where the pacer breaks the pair right after a skip turn (two drawn turns in a row): counted ("broken").
//   2  the every-2nd members on the skip turn, the every-3rd / 4th and the flush rule as before (to price the cap in the A/B).
// Unchanged in every mode: which actors a cut takes, its distances, guards and gates, the BCK clock release, movement, sensors, logic and random numbers (the
// cuts here leave out calc-anim or material-list work only; HR_PLANTMOVE's left-out movements are replayed exactly as before, only on other turns). In a
// steady pair the skip turns are every second turn: an every-2nd cut saves what it saved (its work moves out of the drawn turn into the undrawn one). In a
// plain 60 (every turn drawn) there is no skip turn: the cuts save nothing there, which is what exactness costs. The far-logic levels 1 / 2 (movement AND
// pose every second turn, half speed) are logic and stay as they are: their pose is made on the turn of their own movement, so it is never older than
// the actor's state.
// GXV_POSE_PHASE_CENSUS=1 (log only, any mode): one "pose-phase:" line every 600 turns: per cut, its decisions in the turns whose state a picture showed, by
// the age of the pose drawn (0, 1, 2, 3+ turns), the pair-set models left out in a drawn turn (seen in 60 Assist's second picture), skip turns and broken pairs.
#pragma once

#include <cstdint>
#include <cstring>
#include "cliprange.h"
#include "cpucuts.h"

namespace port {
namespace posephase {

#ifdef PORT_POSE_PHASE_MUTANT
extern int gMutant;  // (tests/pose_phase.cpp only: 1 skip on every drawn turn, 2 skip on the undrawn turn, 3 no cap, 5 the census books the wrong turn)
#define PORT_PP_MUT(k) (::port::posephase::gMutant == (k))
#else
#define PORT_PP_MUT(k) false
#endif

inline int ParseMode(const char* e) {
    if (e == nullptr || *e == 0) return 0;
    const int v = *e - '0';
    return v == 1 || v == 2 ? v : 0;
}

// The turn's phase, at every turn's start (drawn: this turn draws a picture first). Skip(): this turn's calc-anim is made again before any picture.
struct Phase {
    bool prevDrawn = true;  // (the first turn after a start is never a skip turn)
    bool skip = false;
    bool lastSkip = false;  // the turn before was a skip turn
    bool Begin(bool drawn) {
        lastSkip = skip;
        skip = drawn && !prevDrawn;
        if (PORT_PP_MUT(1)) skip = drawn;
        if (PORT_PP_MUT(2)) skip = !drawn && prevDrawn;
        prevDrawn = drawn;
        return skip;
    }
    // The pacer broke the pair: this turn draws, and the turn before (a skip turn) left poses out that this picture shows.
    bool Broken(bool drawn) const { return drawn && lastSkip; }
};

// The count a cut runs at: mode 1 caps every-3rd / every-4th at every 2nd.
inline int EffN(int mode, int n) {
    if (PORT_PP_MUT(3)) return n;
    return mode == 1 && n > 2 ? 2 : n;
}
inline bool Aligned(int mode, int n) { return mode != 0 && EffN(mode, n) == 2; }

// The every-n-th cadence of GXV_FARANIM / GXV_FAMPOSE / GXV_MATLOD (cliprange::FarAnimSkip's counter: 0xFF fresh, else poses left out in a row).
// true: leave this turn's pose out. Mode 0, or a count the mode leaves as it is: FarAnimSkip itself.
inline bool Cadence(uint8_t& skips, int n, uint32_t seed, int mode, bool skipTurn) {
    if (!Aligned(mode, n)) return cliprange::FarAnimSkip(skips, n, seed);
    skips = skipTurn ? 1 : 0;
    return skipTurn;
}
// GXV_CHARPOSE's cadence (cpucuts::CharPoseSkip: the same count with its displacement gate, which keeps its place in front of the count).
inline bool CharCadence(uint8_t& skips, int n, uint32_t seed, float moved2, float dist, float tanHalf, float maxLines, int mode, bool skipTurn) {
    if (!Aligned(mode, n)) return cpucuts::CharPoseSkip(skips, n, seed, moved2, dist, tanHalf, maxLines);
    const float unitsPerLine = (dist > 1.0f && tanHalf > 0.0f) ? 2.0f * dist * tanHalf / 456.0f : 0.0f;
    const float lim = maxLines * unitsPerLine;
    if (!(unitsPerLine > 0.0f) || moved2 > lim * lim) {
        skips = 0;  // posed now (moved): as CharPoseSkip
        return false;
    }
    skips = skipTurn ? 1 : 0;
    return skipTurn;
}
// The budget's every-2nd alternation (LiveActor.cpp PbAnimSkips; half: 0 not far, 1 the next is left out, 2 the next is made; the first far call poses,
// half the actors starting with a skip by addrBit). true: leave this turn's pose out.
inline bool Alternate(uint8_t& half, uint32_t addrBit, int mode, bool skipTurn) {
    if (mode != 0) {
        half = skipTurn ? 2 : 1;
        return skipTurn;
    }
    switch (half) {
    case 1:
        half = 2;
        return true;
    case 2:
        half = 1;
        return false;
    default:
        half = (addrBit & 1u) ? 1 : 2;
        return false;
    }
}
// GXV_STARBATCH_FAR in a turn (every second pose by the turn's parity, spread by address). true: leave it out.
inline bool StarTurnSkip(unsigned parity, uint32_t addr, int mode, bool skipTurn) {
    if (mode != 0) return skipTurn;
    return ((parity + addr) & 1u) != 0u;
}
// GXV_STARBATCH_FAR at the flush (the picture's own pose, every second picture): mode 1 never leaves it out.
inline bool StarFlushAllowed(int mode) { return mode != 1; }
// GXV_HR_PLANTMOVE: may member i of a resting group leave its movement out this turn? (mode 0: the group's half by its tick, as before)
inline bool PlantRestTurn(int i, uint8_t tick, int mode, bool skipTurn) {
    if (mode != 0) return skipTurn;
    return ((i ^ tick) & 1) != 0;
}

// ---- the census ---------------------------------------------------------------------------------------------------------------------------------------------
// kinds: 0 the budget's every-2nd (GXV_PB_ANIM / far-logic level 3), 1 GXV_FARANIM, 2 GXV_CHARPOSE, 3 GXV_FAMPOSE, 4 GXV_MATLOD (material lists),
// 5 GXV_STARBATCH_FAR (turn and flush), 6 GXV_HR_PLANTMOVE (a member's movement owed: its POSE is exact by the latch rule, PlantGroup.cpp; what is late is
// its light and sound). age: turns since the pose (or step) the model is drawn with, at the decision; 0 = made now.
constexpr int kKinds = 7;
constexpr int kAges = 4;  // 0, 1, 2, 3+
inline const char* KindName(int k) {
    static const char* const n[kKinds] = {"pb-anim", "faranim", "charpose", "fampose", "matlod", "starfar", "plantmove"};
    return k >= 0 && k < kKinds ? n[k] : "?";
}
struct Census {
    uint32_t cur[kKinds][kAges] = {};   // this turn's decisions
    uint64_t seen[kKinds][kAges] = {};  // decisions of the turns whose state a picture showed (the turn before a drawn turn)
    uint64_t all[kKinds][kAges] = {};   // every decision
    uint64_t flushSeen[kAges] = {};     // GXV_STARBATCH_FAR's flush rule: a pose left out in the picture's own flush (always seen; age >= 1)
    uint64_t pairB[kAges] = {};         // a pair-set model left out in a drawn turn: 60 Assist's second picture draws it from that pose
    uint32_t turns = 0, pictures = 0, skipTurns = 0, broken = 0;
    bool prevDrawn = true;
    static int Age(int a) { return a < 0 ? 0 : (a >= kAges ? kAges - 1 : a); }
    // a decision of this turn's calc-anim: its kind, the age of the pose the model now has (0: made), tagged: in 60 Assist's pair set, drawn: this turn draws
    void Note(int kind, int age, bool tagged, bool drawn) {
        if (kind < 0 || kind >= kKinds) return;
        ++cur[kind][Age(age)];
        if (tagged && drawn && age > 0) ++pairB[Age(age)];
    }
    void NoteFlush(int age) { ++flushSeen[Age(age < 1 ? 1 : age)]; }
    // At every turn's start: the decisions of the turn before are seen if this turn draws.
    void TurnBegin(bool drawn, bool skipTurn, bool broke) {
        ++turns;
        if (skipTurn) ++skipTurns;
        if (broke) ++broken;
        const bool seenNow = PORT_PP_MUT(5) ? prevDrawn : drawn;
        if (drawn) ++pictures;
        for (int k = 0; k < kKinds; ++k)
            for (int a = 0; a < kAges; ++a) {
                if (seenNow) seen[k][a] += cur[k][a];
                all[k][a] += cur[k][a];
                cur[k][a] = 0;
            }
        prevDrawn = drawn;
    }
    // The oldest pose any picture showed (0..3; 3 = 3 or more), the pair set's second picture and the flush rule included.
    int MaxAgeSeen() const {
        int m = 0;
        for (int k = 0; k < kKinds; ++k)
            for (int a = 1; a < kAges; ++a)
                if (k != 6 && seen[k][a] != 0 && a > m) m = a;  // (plantmove: a step owed, not a pose: reported apart)
        for (int a = 1; a < kAges; ++a) {
            if (pairB[a] != 0 && a > m) m = a;
            if (flushSeen[a] != 0 && a > m) m = a;
        }
        return m;
    }
    uint64_t SeenOld(int k) const {
        uint64_t s = 0;
        for (int a = 1; a < kAges; ++a) s += seen[k][a];
        return s;
    }
    uint64_t SeenAll(int k) const {
        uint64_t s = 0;
        for (int a = 0; a < kAges; ++a) s += seen[k][a];
        return s;
    }
    void ClearTotals() {
        std::memset(seen, 0, sizeof seen);
        std::memset(all, 0, sizeof all);
        std::memset(flushSeen, 0, sizeof flushSeen);
        std::memset(pairB, 0, sizeof pairB);
        turns = pictures = skipTurns = broken = 0;
    }
};

}  // namespace posephase
}  // namespace port
