// Galaxy 3.0 multiplayer: weak defaults for the game-side module entry points (decomp/include/Game/MP/MpGame.hpp), so the hooks
// link whichever modules are present. Each module's real implementation replaces these.
#include "Game/MP/MpGame.hpp"

#define MP_WEAK __attribute__((weak))

namespace MpGame {
    MP_WEAK void onGameSceneInitAfterPlacement() {}
    MP_WEAK void onGameScenePreUpdate() {}
    MP_WEAK void onGameSceneUpdate() {}
    MP_WEAK void onGameScenePostCalcAnim() {}
    MP_WEAK void onGameSceneEnd() {}
    MP_WEAK bool isGameSceneLive() { return false; }
    MP_WEAK bool sessionHeld() { return false; }
    MP_WEAK MpMode heldMode() { return MP_MODE_NONE; }
    MP_WEAK uint32_t turn() { return 0; }
}
namespace MpPuppets {
    MP_WEAK void initScene() {}
    MP_WEAK void update() {}
    MP_WEAK void sendLocalPose() {}
    MP_WEAK void handle(const MpMsg&) {}
    MP_WEAK void onPeerJoined(int) {}
    MP_WEAK void endScene() {}
    MP_WEAK void addArchives(void*) {}
    MP_WEAK bool lastPose(int, MpPose*) { return false; }
    MP_WEAK void logSceneHeap(const char*) {}
    MP_WEAK int nameObjSlotsLeft() { return -1; }
}
namespace MpWorldSync {
    MP_WEAK void registerPlacement(const JMapInfoIter&, NameObj*, int, int) {}
    MP_WEAK void placementDone() {}
    MP_WEAK int sceneObjBegin() { return -1; }
    MP_WEAK void sceneObjEnd(int) {}
    MP_WEAK void classifySwitch(const SwitchIdInfo*, const char*, const JMapInfoIter&) {}
    MP_WEAK void onSwitchEdge(const SwitchIdInfo&, bool) {}
    MP_WEAK void onActorDead(LiveActor*) {}
    MP_WEAK void onPurpleCoin(LiveActor*) {}
    MP_WEAK void onChip(LiveActor*) {}
    MP_WEAK void update() {}
    MP_WEAK void handle(const MpMsg&) {}
    MP_WEAK void endScene() {}
    MP_WEAK bool holdEarly(const MpMsg&) { return false; }
    MP_WEAK void noteHeldDrop(uint8_t) {}
}
namespace MpFlow {
    MP_WEAK void update() {}
    MP_WEAK void handle(const MpMsg&) {}
    MP_WEAK bool holdSceneStart(bool) { return false; }
    MP_WEAK void onGalaxyMove(void*) {}
    MP_WEAK bool guestMayStartGalaxy() { return true; }
    MP_WEAK void onStarGet(int, bool) {}
    MP_WEAK bool holdAfterClear() { return false; }
    MP_WEAK bool blockGameOver() { return false; }
    MP_WEAK bool replacePause() { return false; }
    MP_WEAK bool skipVote(int) { return true; }
}
namespace MpUi {
    MP_WEAK void createSystem() {}
    MP_WEAK void updateSystem() {}
    MP_WEAK void drawSystem() {}
    MP_WEAK void openMultiplayerMenu(int) {}
    MP_WEAK void openSessionMenu() {}
    MP_WEAK bool isMenuOpen() { return false; }
    MP_WEAK bool settledQuiet() { return true; }
    MP_WEAK const wchar_t* findMessage(const char*) { return nullptr; }
    MP_WEAK void toast(uint16_t, int, uint32_t) {}
    MP_WEAK void showWaiting(bool, int, int, int) {}
    MP_WEAK void showResults(const MpMsg&) {}
}
