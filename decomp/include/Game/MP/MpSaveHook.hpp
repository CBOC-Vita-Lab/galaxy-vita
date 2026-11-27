// Galaxy 3.0 multiplayer: the save sequence's two questions (owner: save). The ONLY expressions the game's save nerves ask
// (decomp/src/Game/System/SaveDataHandleSequence.cpp); the host harness (tests/mp/savelife_test.cpp) includes this same header
// and drives the real MpSaveGame.cpp through it, so the test cannot drift from production again.
//
// No pre-check (review finding 2): the net thread can end a session between one frame's MpSaveGame::tick and the next frame's
// save nerve (the save sequence updates before the scene). A pre-check on mp_active() then skips the gate exactly when it must
// latch the end. savesSuppressed() syncs itself and costs a few bool tests without a session (single-player: false unless a
// root switch left the image unsafe, MpSaveGame.cpp).
#pragma once
#include "Game/MP/MpSaveGame.hpp"

namespace MpSaveHook {
    // True: the game's save must not run (it takes its own "continue without saving" path).
    inline bool suppressed() { return MpSaveGame::savesSuppressed(); }
    // True: the original host's save prompt confirms itself (asked only after suppressed() said false).
    inline bool autoconfirm() { return MpSaveGame::savesAutoconfirm(); }
}  // namespace MpSaveHook
