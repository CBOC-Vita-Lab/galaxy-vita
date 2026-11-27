// Galaxy 3.0 multiplayer: coop extras (owner: coop). Greeting emote, pointer ping, friend super jump. See MpCoopLogic.hpp for the
// rules (pure, host tested); MpCoop.cpp is the game side. Called from the puppet module (MpPuppets.cpp, MpRemotePlayer.cpp,
// MpNameTag.cpp) and the session menu (MpUi::sendStamp). Every entry point is a no-op without a session.
#pragma once

#include "Game/MP/MpGame.hpp"

namespace MpCoop {
    void initScene();                 // MpPuppets::initScene, after the puppets and name tags exist
    void update();                    // MpPuppets::update, once per logic turn
    void endScene();                  // MpPuppets::endScene: forget every scene object
    bool warm(bool on);               // MpWarm: one pointer mark appears off screen (on) and goes (off); false: none to warm
    void handle(const MpMsg& m);      // EMOTE (0x22), POINTER (0x24)

    // A greeting from this console: stamp 0..3 (the toast's text). fromMenu = the session menu's stamp list (the gesture still
    // needs an idle player, the rest is sent anyway); else the button, which does nothing unless the player stands idle.
    bool requestEmote(int stamp, bool fromMenu);

    // MP3 costar: the world point under this slot's Star Pointer while its button is held (a fresh POINTER; the local slot: its own last
    // ping). False = not held. The point is untrusted for a remote slot: MpCoStar clamps and rate-limits what it does with it.
    bool pointerPoint(int slot, float out[3]);

    bool superJumpArmed();            // the puppets' head spring sensors are live (A held in the air, interaction on)
    void noteTrample(int slot, bool super);   // the local player stomped this slot's puppet (super: on its head spring)
    bool nameTagIcon(int slot);       // MpNameTags: show the greeting hand on this slot's tag this turn
}
