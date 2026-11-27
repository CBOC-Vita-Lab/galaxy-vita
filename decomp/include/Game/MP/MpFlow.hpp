// Galaxy 3.0 multiplayer: flow-module entry points beyond the contract header (MpGame.hpp). Owner: flow (MpGame.cpp, MpFlow.cpp,
// MpRace.cpp). The game hooks call these after their own `mp_active()` test, so single-player never reaches them.
#pragma once

#include "Game/MP/MpGame.hpp"
#include "port/mp_core_game.h"

class GalaxyMoveArgument;

// Toast text ids of the flow module (ui agent's message table; MpUi::toast(id, slot, arg)).
enum MpFlowText : uint16_t {
    MP_TXT_HOST_CHOOSES = 0x0301,       // "The host chooses the galaxy."
    MP_TXT_STAR_GOT = 0x0302,           // "<slot> got the Power Star!"           arg = star id
    MP_TXT_PLAYER_DOWN = 0x0303,        // "<slot> fell."
    MP_TXT_WARPED_TO_YOU = 0x0304,      // "<slot> warped to you."
    MP_TXT_WARP_FAILED = 0x0305,        // "Can't warp to <slot> right now."
    MP_TXT_NO_SETTINGS = 0x0306,        // "Settings can't be opened during a session."
    MP_TXT_SKIP_VOTE = 0x0307,          // "<slot> wants to skip (n/m)."          arg = votes | total << 8
    MP_TXT_RACE_FINISHED = 0x0308,      // "<slot> finished! <time>"              arg = frames
    MP_TXT_STAMP = 0x0309,              // chat stamp from <slot>                 arg = stamp id
    MP_TXT_NEW_HOST = 0x030A,           // "<slot> is now the host."
    MP_TXT_HOST_RACES_ONLY = 0x030B,    // "Only the host races Cosmic Mario."
};

// GameScene nerve, as the GameScene::update hook reports it (MpFlowHooks::noteGameSceneState).
enum MpSceneState : int {
    MP_SCENE_OTHER = 0, MP_SCENE_OPENING, MP_SCENE_STARTER, MP_SCENE_ACTION, MP_SCENE_PAUSE, MP_SCENE_STAR_GET, MP_SCENE_GRAND_STAR_GET,
    MP_SCENE_GAME_OVER, MP_SCENE_COMET_RETRY, MP_SCENE_SAVE_AFTER_GAME_OVER, MP_SCENE_MOVIE, MP_SCENE_GALAXY_MAP, MP_SCENE_STAFF_ROLL,
    MP_SCENE_TIME_UP
};

namespace MpFlowHooks {
    void noteGameSceneState(int state);                    // GameScene::update, before MpGame::onGameSceneUpdate
    bool fileSelectMayAsk();   // touches: the file select's "go on to the game" request: a guest waiting for the host's move asks twice a second, not every frame
    bool refuseGalaxyMove(const GalaxyMoveArgument& arg);  // top of GameSequenceProgress::requestGalaxyMove: true = drop it
    // ScenarioSelectLayout::exeWaitScenarioSelect (guests in Play Together): 0 = normal input, 1 = input blocked this turn,
    // 2 = select *outSelected now, 3 = cancel the star select now.
    int scenarioSelectGuest(int* outSelected);
    void onScenarioChosen(int placedNo, int selectedNo);   // ScenarioSelectScene::trySetCurrentScenarioNo after setCurrentScenarioNo
    bool refuseGuestGalaxyStart();                         // domes / stage cubes: true = refused (a toast was shown)
    bool keepLives();                                      // MR::decPlayerLeft: true = no life is lost (Play Together)
    void requestWarpTo(int slot);                          // session menu "Warp to <name>" (ui agent)
    bool guestMayStartGhostRace();                         // GhostPlayer (Cosmic Mario) in Play Together: host only
}

// Internal (MpGame.cpp <-> MpFlow.cpp <-> MpRace.cpp).
namespace MpGameInternal {
    void pumpSystem();          // drain messages + tick when no game scene updates (barrier, star select)
    bool sceneExists();
    int sceneState();           // MpSceneState
    uint16_t serial();          // the flow's scene serial (host: own counter; guest: the last executed MOVE/WHERE)
    int64_t nowMs();
    void testStallArm();        // starfix (desktop harness): SMG_MP_TEST_STALL_MS: from now on this console's multiplayer pump is off for that long (a modelled load)
    bool testStalled();         // true while it is; always false on the Vita
    bool hasPendingCommand();   // a host MOVE/WHERE waits to be run on this console
    void allowOwnMoveOnce();    // the host is silent: let this console's next own move through (holds that timed out)
    unsigned presentMask();
    void onCoreEvent(const MpCoreEvent& ev);
    bool returnPending();                       // optfix: the back-to-title step after a session is still waiting
    void afterSessionStep();                   // integ: each game turn without a session (back to the title after one; cheap)   // integ: MpGame.cpp drains mp_core_poll_event into this
}

namespace MpRace {
    void onMoveExecuted(uint16_t serial, int moveType, const char* stageName, uint8_t galaxyIndex, int scenarioNo, int selectedNo);
    void sceneInit();           // GameScene init thread: creates the race layout in a race scene
    void sceneEnd();
    void update();              // each GameScene turn (all nerves)
    void onBarrierReleased(uint16_t serial);
    bool onStarGet(int starId, bool grand);   // true = taken as a race finish (no STAR)
    bool holdAfterClear();
    void handle(const MpMsg& m);              // GO, FINISH, RESULTS, HOLD
    void onBecameHost();                      // host migration: re-send race state
    void onHostChanged();                     // guests: re-send FINISH to the new host
    void onPeerJoined(int slot);              // rejoin: host unicasts RESULTS or GO to the returning slot
    void onReconnected();                     // rejoin: guest re-sends FINISH / RACEREADY after a silent reconnect
    bool isHubStage(const char* stage);
    bool isRaceScene();
    bool finishedLocal();
    int ownRaceTurns();                        // this console's own race turns since its own start (0 while counting down / waiting out a head start)
    bool resultsKnown();
    bool slotFinished(int slot);               // MP3 spectate: this slot crossed the finish line (a finisher is not watched)
    void sessionReset();                       // one race's state (a new race arms again); the series stays
    void seriesReset();                        // race: a session starts or ends: the series (points, streak) and its names go
}
namespace MpFlowRace {
    // race: the host, not behind the star wipe (it did not finish), moves everyone to the next race at once (a direct move).
    bool hostDirectMove(const char* stage, int scenario, int selected);
    // race: a guest behind the star wipe runs the host's direct MOVE (the next race) itself; false = none waits
    bool runPendingDirect();
}
