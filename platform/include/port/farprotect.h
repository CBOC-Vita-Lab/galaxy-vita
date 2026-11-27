// far49 (22 Nov 2026): GXV_FAR_PROTECT (default 1; 0 = the old behaviour). Pure rule (no game types), host-tested by tests/farprotect.cpp (tools/test_farprotect.sh).
//
// WHY. Owner, Freezeflame in Performance mode: "the penguin did stop animating a little bit. I was trying to chase him but it was almost impossible and the frozen
// animation was noticeable." The chased penguin is a PenguinSkater (NPC list; hit sensor, rail, stage switch, camera). Far logic (GXV_FARLOGIC) and its mirror gate
// cannot take it (Classify: sensor / rail / switch / camera = never), GXV_FAMPOSE reaches only marked families, GXV_CHARPOSE only the Enemy / Item lists, GXV_FARANIM
// skips a sensor actor (GXV_FARANIM_SENS=0), GXV_FARDRAW has had the objective guard since prog32b. What DID reach it: the NPC list's every-2nd-turn pose
// (GXV_PB_ANIM=900 in the stage preset), the deferred pose of GXV_ANIM_LOD (3000), and the NPC LodCtrl's swaps (GXV_PB_LOD=0.45): all of them judge by distance
// from the camera AND the player only, never by what the player is doing with the actor. A chased actor runs away: it is exactly the one that sits at those
// distances. The cut is invisible for scenery and visible for the thing the eyes are on.
//
// WHAT. An actor the player deals with is never left out of a pose by any port cut (the existing LiveActor::mPortNoCut path: posed on every turn, no deferral) and is
// never classed by far logic. "Deals with" is decided once, from the actor's own parts (it never changes), and costs nothing afterwards:
//   - an objective by name (cpucuts::ObjectiveName: Penguin*, Kinopio*, Rabbit*, Luma*, Boss*, Koopa*, Switch*, ...: what GXV_FARDRAW_OBJECTIVE already protects), or
//   - a sensor actor of no marked family that the engine marks as triggered or aimed at or followed: a stage switch, a camera controller, a star-pointer target or a
//     rail (patrols, chased and racing actors, cues), or
//   - an NPC (the NPC calc-anim list) with a hit sensor: the talking, racing and chased characters.
// Star bits, coins, the chasers, bees and the rolling rock (marked families) keep their own rules (GXV_PROTECT_SPRITES, the family cuts); plain decoration has no sensor.
#pragma once
#include <cstdlib>
#include "cpucuts.h"

namespace port {
namespace farprotect {

struct Facts {
    const char* name = nullptr;
    bool sensors = false;     // a sensor keeper with at least one sensor
    bool stageSwitch = false; // mStageSwitchCtrl
    bool camCtrl = false;     // mCameraCtrl
    bool pointer = false;     // mStarPointerTarget
    bool rail = false;        // mRailRider
    bool npcList = false;     // the NPC calc-anim list (MR::CalcAnimType 6)
    unsigned char family = 0; // mFlFam (0: none)
};
inline bool Protected(const Facts& f) {
    if (cpucuts::ObjectiveName(f.name)) {
        return true;
    }
    if (!f.sensors || f.family != 0) {
        return false;
    }
    return f.stageSwitch || f.camCtrl || f.pointer || f.rail || f.npcList;
}
inline bool ParseOn(const char* e) { return !(e != nullptr && e[0] == '0' && e[1] == 0); }
inline bool On() {
    static const bool v = ParseOn(std::getenv("GXV_FAR_PROTECT"));
    return v;
}

}  // namespace farprotect
}  // namespace port
