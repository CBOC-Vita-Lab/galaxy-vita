// Galaxy 3.0 multiplayer: shared world state (Play Together). Extra hook entry points of the world module (owner: world) beyond
// the MpWorldSync contract in MpGame.hpp. Implemented in decomp/src/Game/MP/MpWorld.cpp.
#pragma once

#include "Game/MP/MpGame.hpp"

namespace MpWorldSync {
    void beginPlacement();      // StageDataHolder::initPlacement, first line: opens the scene's placement registry
    void dropCarry(const char* why);   // a group restart: kills / coins / chips reset on every console (MpBubble startRestart, RESTART message)
    void onRestartMessage();           // MpBubble: a RESTART for this serial arrived (a console still loading clears its queued objects and re-requests the map)
    void onPeerJoined(int slot);       // rejoin: host clears ReplyGate for the returning slot so its repair REGCRC is answered immediately
    bool applyingRemote();      // true while a remote event is being applied (hooks must not report it back)
}
