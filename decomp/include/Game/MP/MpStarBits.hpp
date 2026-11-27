// Galaxy 3.0 multiplayer: the team Star Bit count, game side (owner: costar; MpStarBits.cpp, rules and wire in MpStarBitsLogic.hpp).
// Play Together only. Every entry point is a no-op without a session (and in Star Race), so single player counts Star Bits as before.
#pragma once

#include "Game/MP/MpGame.hpp"

namespace MpStarBits {
    void update();                      // MpFlow::update, once a turn: send DELTA / TOTAL, bring the game's count to the team's
    bool handle(const MpMsg& m);        // TALLY (0x3D): true = it was a team Star Bit message (DELTA / TOTAL), false = an old-form tally
    // MR::addStarPiece(num), before the game adds: tells the team. True = skip the game's own add (a swept bit arrives, see below).
    bool onAdd(int num);
    // ScenePlayingResult::incStarPiece, at each 50: false = no 1-Up (lives off: "Infinite lives"; or a catch-up jump)
    bool oneUpAllowed();
    bool teamActive();                  // Play Together with a scene: the count below is the player's own part of the team's
    int teamCount();                    // the shared count the Lumas and the HUD use (the game's own, held equal on every console); -1 without a team count
    unsigned ownCollected();            // Star Bits this console's player picked up this scene (the tally card's number)
    void sceneEnd();
}
