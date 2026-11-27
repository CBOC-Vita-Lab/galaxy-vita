// Galaxy 3.0 multiplayer: Co-Star help from every player's Star Pointer (owner: costar; MpCoStar.cpp, rules in MpCoStarLogic.hpp).
// Play Together only; every entry point is a no-op without a session.
#pragma once

#include "Game/MP/MpGame.hpp"

class LiveActor;
class StarPiece;

namespace MpCoStar {
    void initScene();               // MpCoop::initScene
    void update();                  // MpCoop::update, once a turn
    void endScene();                // MpCoop::endScene
    // MR::isStarPointerPointing2POnPressButton found nobody pointing (the real second pad): do the players' pointers hold this actor?
    // True only for an enemy (not a boss) that a held pointer is on, within the rules (one target a pointer, 4 s, cool-down, budget).
    bool holds(const LiveActor* actor);
    // StarPiece::tryGotJudge: does another player's pointer sweep this Star Bit (it then flies to the local player, not counted again)?
    bool sweeps(StarPiece* piece);
    // Desktop test hook (SMG_MP_TEST_POINTER_ENEMY, MpCoop.cpp): the position of the enemy nearest to the player among those that asked the
    // hold question in the last turns. False = none.
    bool testNearestEnemy(float out[3]);
    bool testNearestStarBit(float out[3]);   // SMG_MP_TEST_POINTER_STARBIT: the nearest Star Bit that asked to be got
}
