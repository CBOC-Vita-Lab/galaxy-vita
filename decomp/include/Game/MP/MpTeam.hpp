// Galaxy 3.0 multiplayer: the party strip's data and the team star celebration (owner: celebrate; MpTeam.cpp, logic in MpPartyLogic.hpp).
// Hooks call these after their own mp_active() test (single player never reaches them).
#pragma once

#include "Game/MP/MpGame.hpp"

struct MpPoseF;
namespace MpPanel { struct Strip; }

namespace MpTeam {
    // ---- the party strip (Play Together and Star Race)
    void noteHud(bool shown);                  // PlayerLeft::control, once a turn while the life counter lives: false in its Hide nerve
    bool stripModel(MpPanel::Strip* out);
    // ingame: the local character has its own head on the life counter (not the game's Mario / Luigi picture)
    bool counterOwnHead();
    // ingame: PlayerLeft, once a turn: its hidden picture pane's place (screen units) and scale (its count-up bounce)
    void noteCounterPane(float x, float y, float scale);      // MpUiSystem, once a frame: false = nothing to draw (no session, one player, HUD hidden)
    // ---- the team star (Play Together)
    void onStar(int bySlot, uint16_t serial, int starId);   // a STAR was accepted (own or received): flash, tally, near test
    void handle(const MpMsg& m);               // TALLY (0x3D)
    void update();                             // MpFlow::update, once a turn
    void noteSceneState(int sceneState);       // MpFlowHooks::noteGameSceneState: the star-get demo cheers
    void onClearHold();                        // MpFlow::holdAfterClear: the demo is over, show the card (once per star, refreshed)
    void noteHelped();                         // a bubble popped / a friend warped to (also the bubble module's hook)
    bool celebrate(MpPoseF* pose);             // MpPuppetBase::step: during the star-get demo a puppet plays the star-get animation
    void sceneEnd();
}
