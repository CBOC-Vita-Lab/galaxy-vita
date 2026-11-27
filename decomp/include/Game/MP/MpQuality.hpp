// Galaxy 3.0 multiplayer: the automatic puppet quality governor, game side (owner: perf3). The rules are in MpQualityLogic.hpp;
// MpQuality.cpp feeds them the picture intervals and answers the puppets' level-of-detail questions. Game thread only.
// Nothing here runs without a session (MpPuppets::update returns before MpQuality::turn when multiplayer is off).
#pragma once

#include "Game/MP/MpQualityLogic.hpp"

namespace MpQuality {
    void turn();        // once a logic turn, from MpPuppets::update (session and scene live)
    void endScene();    // MpPuppets::endScene: the window and the per-slot states start again with the next scene
    int level();        // 0..LEVEL_MAX (0 without a session)
    const LevelParams& params();   // the current level's distances and rules
    bool shadows();     // remote puppets may cast shadows
    // Joints on every other picture for this remote slot at this squared camera distance (levels 2 and 3; the far rule is the
    // caller's own). Keeps the slot's "distant" hysteresis; a slot outside 0..3 is never half rate.
    bool halfRate(int slot, float d2);
}  // namespace MpQuality
