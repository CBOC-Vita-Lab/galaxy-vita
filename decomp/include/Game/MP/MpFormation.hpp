// Galaxy 3.0 multiplayer: puppet formation, game side (owner: formation; MpFormation.cpp, logic in MpFormationLogic.hpp).
// Personal space for remote puppets drawn inside the local player (arrivals, star-get demos, warps, the first seconds of a scene)
// and the star-get team photo. Visual only: it moves the puppet's drawn pose (MpPuppetBase::setDrawPose), never a player or a
// sensor. Called by MpPuppets after its own mp_active() test; nothing runs in single player.
#pragma once

namespace MpFormation {
    void initScene();   // MpPuppets::initScene, once the puppets exist
    void endScene();    // MpPuppets::endScene
    void update();      // MpPuppets::update, after the puppet lifecycle, before the name tags (they follow the drawn pose)
    unsigned placedMask();   // MP3 lod2: bit = slot: that puppet stands in a formation / the team photo now (counts as the nearest)
    unsigned localSpaceMask();   // wave8: bit = slot: that puppet is visually offset from the local player (MODE_SPACE or MODE_ARC)
}
