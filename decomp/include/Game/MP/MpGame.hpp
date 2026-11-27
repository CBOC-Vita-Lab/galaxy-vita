// Galaxy 3.0 multiplayer: game-side module contract (decomp/src/Game/MP). See DESIGN.md.
// The hooks in game files call only the MpGame:: entry points below (each starts with `if (!mp_active()) return;` or an equivalent
// cheap test, so single-player is untouched). MpGame.cpp (owner: flow) routes received messages to the modules by type.
#pragma once

#include "port/mp_bridge.h"

class LiveActor;
class JMapInfoIter;
class NameObj;
struct SwitchIdInfo;

namespace MpGame {
    // ---- scene lifecycle (hooks in GameScene) -----------------------------------------------------------------------------
    void onGameSceneInitAfterPlacement();      // GameScene::init after SceneFunction::startActorPlacement(): creates puppets etc.
    void onGameScenePreUpdate();                // wave8: GameScene::update before updateNerve(): drain fresh STATEs before puppets sample
    void onGameSceneUpdate();                   // GameScene::update, once per logic turn (all nerves)
    void onGameScenePostCalcAnim();             // wave8: GameScene::calcAnim after executeCalcAnimList(): coherent local pose capture & send
    void onGameSceneEnd();                      // scene teardown (GameScene destructor / before the scene heap is freed)
    bool isGameSceneLive();                     // a game scene exists and is in its Action state
    bool sessionHeld();                         // active session OR core reconnecting (scene init & protective gameplay guards stay armed)
    MpMode heldMode();                          // active mp_mode(), or the mode of the session being reconnected (MP_MODE_NONE outside a session)
    void dropHeldWorld();                       // hostreload: a restart while loading: the held world messages (0x40..0x4F) are discarded
    uint32_t turn();                            // logic-turn counter since boot
}

namespace MpPuppets {   // owner: puppet (MpRemotePlayer.cpp, MpPuppetDrawer.cpp, MpLocalCapture.cpp, MpNameTag.cpp)
    void initScene();
    void update();
    void sendLocalPose();                       // wave8: called after GameScene::calcAnim so mPosition/baseMtx/Xanime are coherent
    void handle(const MpMsg& m);                // STATE, LOOK, EMOTE, BUMP
    void onPeerJoined(int slot);                // rejoin: reset that peer's pose buffer so recovery is immediate
    void endScene();
    void addArchives(void* nameObjArchiveListCollector);   // SceneDataInitializer hook: other-character archives
    bool lastPose(int slot, MpPose* out);                  // last pose of a slot (local: last sent); false = none
    int nameObjSlotsLeft();                                // fix2: free places left in the scene's NameObjHolder (fixed 0x1300); -1 unknown
    void logSceneHeap(const char* tag);                    // memfix: "mem scene <tag> serial napa gddr cache" (free bytes of the scene heaps)
}

namespace MpWorldSync { // owner: world (MpWorld.cpp)
    void registerPlacement(const JMapInfoIter& iter, NameObj* created, int groupCountBefore, int groupCountAfter);
    void placementDone();
    int sceneObjBegin();                        // hook around a lazy creation inside a placement (SceneObjHolder::create, MapPartsRailGuideHolder::createRailGuide,
                                                // ElectricRailHolder::createModel): the actors made there (CoinHolder's coins, rail guide points) belong to no row
    void sceneObjEnd(int token);
    void classifySwitch(const SwitchIdInfo* info, const char* switchName, const JMapInfoIter& iter);
    void onSwitchEdge(const SwitchIdInfo& info, bool on);
    void onActorDead(LiveActor* actor);
    void onPurpleCoin(LiveActor* coin);
    void onChip(LiveActor* chip);
    void update();
    void handle(const MpMsg& m);                // SWITCH, SWITCHMAP, KILL, PURPLE, CHIP, REGCRC
    void endScene();
    bool holdEarly(const MpMsg& m);              // worldsync: KILL/PURPLE/CHIP while the scene is not live -> the world early queue (true = taken)
    void noteHeldDrop(uint8_t msgType);          // worldsync: the held pool refused a world message (counted in the scene's drop line)
}

namespace MpFlow {      // owner: flow (MpFlow.cpp, MpRace.cpp)
    void update();
    void handle(const MpMsg& m);                // WHERE, MOVE, LOADED, GO, STAR, FINISH, RESULTS, SKIPVOTE, DEAD, WARPREQ, HOLD
    bool holdSceneStart(bool announceLoaded = true);   // GameSystemSceneController::isReadyToStartScene: true = keep waiting. Always pumps the
                                                // session; announceLoaded false (the renderer's warm-up hold is still in force): no LOADED, no barrier release, returns true
    void onGalaxyMove(void* galaxyMoveArgument);// GameSequenceProgress::requestGalaxyMove (host mirrors it; guests block their own)
    bool guestMayStartGalaxy();                 // domes / launch stars / map: false for guests in Together
    void onStarGet(int starId, bool grand);     // PowerStar::exeStageClearDemo first step
    bool holdAfterClear();                      // GameStageClearSequence before requestChangeStageAfterStageClear
    bool blockGameOver();                       // lives clamp
    bool replacePause();                        // GameScenePauseControl::tryStartPauseMenu: true = open the session menu instead
    bool skipVote(int contextKind);             // port_skip_context: true = everyone agreed, skip now
}

namespace MpUi {        // owner: ui (MpMenuSequence.cpp, MpPanelDraw.cpp, MpMessages.cpp)
    void createSystem();                        // GameSequenceDirector init
    void updateSystem();                        // GameSequenceDirector::update
    void drawSystem();                          // GameSequenceDirector::draw
    void openMultiplayerMenu(int selectedFile); // FileSelector::callbackManual (the 2P button)
    void openSessionMenu();                     // pause replacement during a session
    bool isMenuOpen();                          // hold game input while true
    const wchar_t* findMessage(const char* id); // MessageData::getMessageDirect fallback for "MP_*" ids (null = not ours)
    void toast(uint16_t textId, int slot, uint32_t arg);
    void showWaiting(bool on, int done, int total, int secondsLeft);   // race / barrier waiting screens
    void showResults(const MpMsg& results);
}
