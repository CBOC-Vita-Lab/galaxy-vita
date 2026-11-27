// Galaxy 3.0 multiplayer: messages between players (owner: chat). Pure declarations (no game headers): MpMenuSequence.cpp and the
// host tests include it; decomp/src/Game/MP/MpChat.cpp implements it (the tests stub the menu side).
//
// What players get (DESIGN.md section 3 CHAT, docs/MULTIPLAYER.md):
//  - Quick messages, everywhere: the session menu's Messages page (12 preset phrases, EN/FR/ES; the message carries the phrase's
//    number, so each console shows it in its own language), and in play the quick ring: hold Select, press a direction.
//  - Typed messages (up to 40 characters, the system keyboard) in rooms that are not public, unless the host turned them off.
//  - Shown as the game's own NPC speech balloon (TalkBalloonStretch) above the sender's character for a few seconds, and as a
//    toast line with the sender's head. Mute (Players screen) hides a player's typed messages and balloons on this console.
// Log lines (the harness): "ui: chat sent id=N" / "ui: chat sent id=text chars=N", "ui: chat shown from=S kind=phrase|text".
#pragma once

#include <stdint.h>

#include "port/mp_chat.h"

struct MpMsg;
namespace MpPanel { struct Ring; }

namespace MpChat {
    // ---- the menus (MpMenuSequence.cpp)
    bool sendPhrase(int phrase);       // false: not in a session, a bad id or too soon (rate limit; the caller buzzes)
    bool sendText(const char* utf8);   // what was typed (cleaned first); false: typed messages not allowed here / nothing / too soon
    void noteRoomPublic(bool isPublic); // the menu: the next room is public (Quick Match) / not (code, friend, address, Wi-Fi, hosting); typed text never in a public room
    bool typedAllowed();               // this room takes typed messages (mp_chat_typed_allowed on the session's settings)
    bool muted(int slot);
    void setMuted(int slot, bool on);

    // ---- the session (MpGame.cpp)
    bool handleMessage(const MpMsg& m);   // CHAT (0x3E); true = consumed
    void initScene();                     // scene init (after placement): the balloons of this scene
    void update();                        // every game-scene turn of a session
    void endScene();                      // the scene's layouts go with it
    void sessionTick();                   // every frame of a session, any scene (mute bookkeeping, the test hook's clock)

    // ---- the quick ring (MpUiSystem.cpp draws it; WPadButton.cpp lends it Select and the D-pad)
    bool ringModel(MpPanel::Ring* out);   // false: nothing to draw
    void filterPad(uint32_t* hold, uint32_t* trigger, uint32_t* release, uint32_t* repeat);
}
