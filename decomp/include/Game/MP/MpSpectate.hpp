// Galaxy 3.0 multiplayer: spectating (owner: spectate). A player who has nothing to do (a bubble no teammate is near, no life left, a
// Star Race finish) watches a teammate: the camera follows that puppet (event camera "MpWatch", CameraMpWatch), L / R (and a touch on a
// party strip head) switch, a label says whose view it is, and the moment the player is back in play the camera returns to Mario with
// the game's own blend. Rules and maths: MpSpectateLogic.hpp. Every entry point is a no-op without a session.
#pragma once

#include "port/mp_bridge.h"


namespace MpSpectate {
    void sceneInit();                // MpGame::onGameSceneInitAfterPlacement (scene init thread): the camera target object
    void update();                   // MpGame::onGameSceneUpdate after the flow and the bubble, every game turn
    void sceneEnd();                 // scene teardown

    bool watching();                 // the view follows a teammate now (the event camera is up or starting)
    int watchedSlot();               // -1 = our own view
    bool selectSlot(int slot);       // a touch on the party strip's head of `slot` (true = it will be watched)
    bool bubbleCamAvailable();       // wave8: true when "MpWatch" is ready to track the local player's bubble
    bool onLocalBubbleLeave();       // wave8: end the local bubble-follow camera smoothly on pop (returns true if it was active)
    // Planet LOD (LodCtrl::calcDistanceToCamera) while watching: the distance of a model at (x, y, z) to the watched character, so the
    // planets around him show their near models whatever the (held) local Mario is doing far away. False = not watching.
    bool lodDistance(float x, float y, float z, float* out);

    // MpRace::holdAfterClear, every turn of the hold behind the closed star wipe while the results are not in (`results` = they are):
    // true = the wipe is open on a teammate (the waiting panel stays hidden). The hold may release only when readyToRelease().
    bool raceHoldTick(bool results);
    // race: the results card is up behind the star-clear's closed wipe: want = true opens the wipe again on the frozen star-get scene (the
    // card sits on the game's own backdrop, not on black); false closes it again before the scene moves on (readyToRelease() waits for it).
    void raceBackdrop(bool want);
    bool readyToRelease();           // the wipe is closed again (or was never opened)
}
