// popin31 (17 Nov): what a cut site of the port asks before it hides, thins or swaps a thing (platform/include/port/popin.h, B1 / B2). Game units only:
// the state lives in decomp/src/Game/LiveActor/LodCtrl.cpp (port_popin_turn writes it once a game turn); the pure rules are in popin.h and host-tested.
//
// The pattern at a site (the cut has decided it wants the thing; `was` = the cut holds it now):
//     if (want && port::popin::GuardOn(port::popin::kFlShadow)) want = port::popin::CutAllowed(port::popin::kShadow, was, radius, dist, x, y, z);
// GuardOn(kind) is one load (that kind's floor and the release off: false, and nothing else runs). The radius is computed only inside that branch.
// popin31b: the floor is per kind (GXV_PB_MINPX_<KIND>, popin.h); CutAllowed takes the census kind and floors it by FloorKindOf(kind). CutVerdict says
// why a cut was refused (the census times the work of the things the FLOOR kept: port_pb_floor_cost).
#pragma once

#include <cstdint>
#include "port/popin.h"

class LiveActor;

extern "C" {
extern port::popin::FloorTurn port_pb_floor;   // the floor's constants and the release flag of this turn (LodCtrl.cpp)
extern uint32_t port_popin_turn_no;            // game turns since boot (port_popin_turn), for the size caches' age
void port_pb_cut_kept(int kind, int byRelease);   // a site kept a thing (the census; the release's count of holds)
int port_pb_in_view(float x, float y, float z, float radius);   // the game's clipping frustum (its own field of view: never narrower than the picture)
float port_pb_model_extent(const LiveActor* pActor);   // the model's extent from its origin (pose and scale in it); 0: no model; 1e5: unknown
// (popin31b, LodCtrl.cpp; the census only) the timed work of one thing the floor of kind fk kept (ns), and a kept shadow's projection cast (hit: the memo answered)
void port_pb_floor_cost(int fk, uint32_t ns);
void port_pb_floor_shadow_memo(int hit);
}

namespace port {
namespace popin {

// Either B1's release or B2's floor (of any kind) is in force this turn
inline bool GuardOn() { return port_pb_floor.guard != 0; }
// popin31b: the release or the floor of kind fk (FloorKind) is in force this turn (one integer load: with both off this is all a site pays)
inline bool GuardOn(int fk) { return port_pb_floor.guardK[fk] != 0; }

// Why a cut may not act now: 0 it may; 1 the floor kept the thing (it is at or above its kind's floor); 2 the release after a flight held it
enum { kCutOk = 0, kCutFloor = 1, kCutRelease = 2 };
// (radius: the thing's size radius; dist: its distance from the eye, or its depth along the view, which is never more; viewRadius: the sphere at x, y, z the
// release asks the frustum about, < 0: the same radius)
inline int CutVerdict(int kind, bool wasCut, float radius, float dist, float x, float y, float z, float viewRadius = -1.0f) {
    if (!FloorMayCutK(port_pb_floor, FloorKindOf(kind), radius, dist, wasCut)) {
        if (LogOn()) port_pb_cut_kept(kind, 0);
        return kCutFloor;
    }
    if (port_pb_floor.releasing && !wasCut && port_pb_in_view(x, y, z, viewRadius >= 0.0f ? viewRadius : radius)) {
        port_pb_cut_kept(kind, 1);
        return kCutRelease;
    }
    return kCutOk;
}
// The same from a squared distance (no root)
inline int CutVerdict2(int kind, bool wasCut, float radius, float dist2, float x, float y, float z) {
    if (!FloorMayCut2K(port_pb_floor, FloorKindOf(kind), radius, dist2, wasCut)) {
        if (LogOn()) port_pb_cut_kept(kind, 0);
        return kCutFloor;
    }
    if (port_pb_floor.releasing && !wasCut && port_pb_in_view(x, y, z, radius)) {
        port_pb_cut_kept(kind, 1);
        return kCutRelease;
    }
    return kCutOk;
}
// May the cut act on this thing now?
inline bool CutAllowed(int kind, bool wasCut, float radius, float dist, float x, float y, float z, float viewRadius = -1.0f) {
    return CutVerdict(kind, wasCut, radius, dist, x, y, z, viewRadius) == kCutOk;
}
inline bool CutAllowed2(int kind, bool wasCut, float radius, float dist2, float x, float y, float z) {
    return CutVerdict2(kind, wasCut, radius, dist2, x, y, z) == kCutOk;
}
// popin31b: is this thing the floor kept one to time for the census's price line? One in 8 of them, census on only (a counter per site)
inline bool CostSample(uint32_t& counter) { return CensusOn() && ((++counter) & 7u) == 1u; }

// A size cache: the model's extent, measured again when it is older than 64 turns (an actor that grows or changes its pose a lot is re-measured).
// ext < 0: never measured. Cost: one compare a call; the measure (a walk over the model's joints) about once a second per thing a cut is acting on.
inline float CachedExtent(float& ext, uint16_t& at, const LiveActor* pActor) {
    const uint16_t now = static_cast<uint16_t>(port_popin_turn_no);
    if (ext < 0.0f || static_cast<uint16_t>(now - at) > 64) {
        ext = port_pb_model_extent(pActor);
        at = now;
    }
    return ext;
}

}  // namespace popin
}  // namespace port
