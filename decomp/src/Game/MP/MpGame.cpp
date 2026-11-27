// Galaxy 3.0 multiplayer: the game-side dispatcher (owner: flow). Once per GameScene logic turn it drains mp_poll() and routes the
// messages by type to the modules, ticks the core and updates puppets, world sync and flow. While no game scene updates (the scene
// start barrier, the star select) MpFlow pumps through MpGameInternal::pumpSystem(): flow and ui messages are handled at once,
// puppet and world messages are held (fixed queue) and replayed at the first turn of the next scene; STATE is dropped there
// (latest wins, a fresh one follows).
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpEnteredLogic.hpp"   // optfix
#include "Game/MP/MpDrainLogic.hpp"   // corefix2: a batch stops when its session ended
#include "Game/MP/MpWorldLogic.hpp"   // worldsync: isPeriodicCheck
#include "Game/MP/MpHeldPool.hpp"   // optfix: the held-message byte pool
#include "Game/MP/MpBubble.hpp"   // bubble: Play Together "Bubbles" falls
#include "Game/MP/MpSaveGame.hpp"
#include "Game/MP/MpUi.hpp"   // MP3 integration: TOAST / STAMP go to MpUi::handleMessage
#include "Game/MP/MpTeam.hpp"   // MP3 celebrate
#include "Game/MP/MpStarBits.hpp"   // MP3 costar
#include "Game/MP/MpChat.hpp"   // MP3 chat: CHAT (0x3E), balloons, the quick ring
#include "Game/MP/MpSpectate.hpp"   // MP3 spectate: a waiting player watches a teammate
#include "Game/MP/MpWarm.hpp"   // MP3 puppetprep: first-appearance warm-up
#include "Game/MP/MpHead3D.hpp"   // MP3 heads3d: the file select's 3D heads
#include "Game/MP/MpDemo.hpp"   // MP3 demo: SMG_MP_DEMO (off: one int compare per hook)

#include <string.h>
#include <stdlib.h>
#include "port/mp_prof.h"
#include "port/mp_test_env.h"

extern "C" int64_t port_host_time_ns(void);

namespace {
    uint32_t sTurn = 0;
    volatile bool sSceneExists = false;   // written on the scene init thread / game thread, read on the game thread
    int sSceneState = MP_SCENE_OTHER;
    int sSceneTurnsSincePump = 0;
    bool sModulesInScene = false;   // puppets / world were set up for the current scene
    bool sWasActive = false;
    bool sWasHeld = false;
    MpMode sHeldMode = MP_MODE_NONE;

#if defined(TARGET_PC) && !defined(__vita__)
    int sTestHoldLive = -1;           // SMG_MP_TEST_HOLD_LIVE (desktop harness only)
    uint32_t sSceneTurnsSinceInit = 0;
    bool testHeld() {
        if (sTestHoldLive < 0) { const char* e = getenv("SMG_MP_TEST_HOLD_LIVE"); sTestHoldLive = e ? atoi(e) : 0; }
        return sTestHoldLive > 0 && sSceneTurnsSinceInit < (uint32_t)sTestHoldLive;
    }
#else
    inline bool testHeld() { return false; }
#endif
    MpHeld::Pool sHeld;   // optfix: a byte pool (8 KB), not 24 whole messages (24.7 KB)
    uint32_t sHeldDropped = 0;
    // optfix: after multiplayer was opened once, the per-frame hooks ran for the rest of the boot (a dozen calls). The rules for clearing
    // the entered flag again are MpEnteredLogic.hpp (host tested); never with the desktop autopilot (SMG_MP_AUTO) or the demo.
    MpEnteredLogic::Retire sRetire;
    bool retireEntered() {
        bool demoOrAuto = MpDemo::enabled();
#if defined(TARGET_PC) && !defined(__vita__)
        static const bool sAutopilot = [] { const char* a = MP_TEST_GETENV("SMG_MP_AUTO"); return a && *a; }();   // (the harness autopilot lives on the flag)
        demoOrAuto = demoOrAuto || sAutopilot;
#endif
        MpEnteredLogic::Inputs in;
        in.sessionActive = MpGame::sessionHeld();
        in.uiSettled = MpUi::settledQuiet();
        in.saveSettled = MpSaveGame::settled();
        in.returnPending = MpGameInternal::returnPending();
        in.demoOrAutopilot = demoOrAuto;
        const bool retire = sRetire.turn(in, [] {
            MpUiStatus st;
            mp_ui_status(&st);
            return st.phase == MP_UI_IDLE;   // (a sign-in, a scan, a lobby the menu left running: keep ticking the core)
        });
        if (retire) mp_retire_entered();
        return retire;
    }
    MpMsg sMsg[8];   // MP3 netopt: eight message buffers for mp_poll_many (static: no stack copy of 8 KiB per turn)

    bool hostOnlyOk(const MpMsg& m) {
        return !mp_msg_host_only(m.type) || (int)m.fromSlot == mp_host_slot();
    }

    void routeUi(const MpMsg& m) {
        MpUi::handleMessage(m);   // TOAST (0x50), STAMP (0x51): the ui module checks the payload; other ids in 0x50..0x5F are dropped
    }

    void drainEvents() {
        MpCoreEvent ev;
        for (int guard = 0; guard < 64 && mp_core_poll_event(&ev); guard++) MpGameInternal::onCoreEvent(ev);
    }

    void hold(const MpMsg& m) {
        if (m.type == MP_MSG_STATE || m.type == MP_MSG_POINTER) return;   // unreliable, latest wins (MP3 coop: the pointer ping too)
        if (MpWorldLogic::isPeriodicCheck(m.type, m.data, m.len)) return;   // worldsync: the host's 1 s check messages are repeated: never held (they starved the pool)
        if (!sHeld.push(m)) {
            if ((sHeldDropped++ % 16) == 0) mp_log("game held queue full: dropped type=0x%02x (total %u)", m.type, sHeldDropped);
            if (m.type >= 0x40 && m.type <= 0x4F) MpWorldSync::noteHeldDrop(m.type);   // worldsync: counted, and logged with its type
        }
    }

    void route(const MpMsg& m, bool sceneLive) {
        if (m.type < MP_MSG_GAME_FIRST || m.type > MP_MSG_GAME_LAST || m.fromSlot >= MP_MAX_PLAYERS || m.len > MP_MAX_PAYLOAD) return;
        if (!hostOnlyOk(m)) {
            mp_log("game drop type=0x%02x from=%d: host-only, host is %d", m.type, m.fromSlot, mp_host_slot());
            return;
        }
        if (m.type == MP_MSG_CHAT) {   // MP3 chat (in flow's id range, not flow's): shown at once, in any scene
            MpChat::handleMessage(m);
            return;
        }
        if (m.type <= 0x2F) {
            if (sceneLive) MpPuppets::handle(m); else hold(m);
        } else if (m.type <= 0x3F) {
            MpFlow::handle(m);
        } else if (m.type <= 0x4F) {
            if (sceneLive) MpWorldSync::handle(m); else if (!MpWorldSync::holdEarly(m)) hold(m);   // worldsync: object events wait in the world's own queue
        } else {
            routeUi(m);
        }
    }

#if defined(TARGET_PC) && !defined(__vita__)
    // starfix (harness, desktop only): SMG_MP_TEST_STALL_MS=<ms> models a scene load on THIS console: from the moment MpGameInternal::testStallArm() is called (the dome
    // test hook, right after the host's MOVE) for <ms> NOTHING of the multiplayer pump runs: no drain, no MpFlow::update (so no flow clock tick), no puppets, no tick.
    // The core keeps queueing what arrives, as when the game thread is busy loading; the first pump afterwards gives the flow clock one capped step.
    // Armed only by the move under test, never by a serial change. Compiled out of the Vita objects.
    int64_t sStallUntil = 0;
    bool sStallLogged = false;
    bool sStallArmed = false;   // one-shot per process: the dome hook fires again in every later scene, only the first move is the one under test
    int stallMs() {
        static int sMs = -1;
        if (sMs < 0) { const char* e = MP_TEST_GETENV("SMG_MP_TEST_STALL_MS"); sMs = e ? atoi(e) : 0; }
        return sMs;
    }
#endif
}  // namespace

namespace MpGameInternal {
#if defined(TARGET_PC) && !defined(__vita__)
    void testStallArm() {
        const int ms = stallMs();
        if (ms <= 0 || sStallArmed) return;
        sStallArmed = true;
        sStallUntil = nowMs() + ms;
        sStallLogged = true;
        mp_log("game test stall: no messages read for %d ms (nothing of the multiplayer pump runs)", ms);
    }
    bool testStalled() {
        if (!sStallUntil) return false;
        if (nowMs() < sStallUntil) return true;
        sStallUntil = 0;
        if (sStallLogged) { sStallLogged = false; mp_log("game test stall: reading messages again"); }
        return false;
    }
#else
    void testStallArm() {}
    bool testStalled() { return false; }
#endif
}  // namespace MpGameInternal

namespace {
    void drain(bool sceneLive) {
        if (sceneLive && sHeld.count() > 0) {
            sHeld.rewind();
            while (sHeld.next(&sMsg[0]) && mp_active()) route(sMsg[0], true);   // (sMsg[0] is the staging buffer; the poll loop below reuses it) // corefix2: not into a session that ended meanwhile
            sHeld.clear();
        }
        // MP3 netopt: up to 8 messages per core lock (mp_poll_many), the same 512-message guard per turn
        for (int guard = 0; guard < 512;) {
            const uint32_t n = mp_poll_many(sMsg, 8);
            if (n == 0) break;
            // corefix2: they were all popped before the first was routed: stop when a handler ended the session, hold the scene-bound rest
            // when it moved the scene
            const uint32_t routed = MpDrainLogic::routeBatch(n, sceneLive, [](uint32_t i, bool live) { route(sMsg[i], live); }, [] { return mp_active(); },
                                     [] { return mp_scene_serial(); });
            if (routed < n) break;   // (the session is over: whatever the core still holds is its own to drop)
            guard += (int)n;
        }
    }
}  // namespace

namespace MpGameInternal {
    void pumpSystem() {
        MP_PROF_SCOPE(MP_PROF_TOTAL);
        MpSaveGame::tick();   // MP3 integration: recovery checkpoints / session start-end (one bool read without a session)
        const bool active = mp_active();
        if (!active && !MpGame::sessionHeld()) return;
        if (MpGameInternal::testStalled()) return;   // starfix: a modelled load pumps nothing
        if (sSceneTurnsSincePump > 0) {   // a game scene already pumped this frame
            sSceneTurnsSincePump = 0;
            return;
        }
        if (active) {
            sTurn++;
            drain(false);
        }
        mp_tick_game(sTurn);
        drainEvents();
    }
    bool sceneExists() { return sSceneExists; }
    int sceneState() { return sSceneState; }
    int64_t nowMs() { return port_host_time_ns() / 1000000; }
}  // namespace MpGameInternal

namespace MpFlowHooks {
    void noteGameSceneState(int state) {
        sSceneState = state;
        if (mp_active()) MpTeam::noteSceneState(state);   // MP3 celebrate: the star-get demo cheers
    }
}

namespace MpGame {
    bool sessionHeld() {
        return mp_active() || port_net_session_lockstep() != 0;
    }

    MpMode heldMode() {
        if (mp_active()) {
            sHeldMode = mp_mode();
            return sHeldMode;
        }
        if (port_net_session_lockstep() != 0) return sHeldMode;
        sHeldMode = MP_MODE_NONE;
        return MP_MODE_NONE;
    }

    void onGameSceneInitAfterPlacement() {
        // (scene init thread: no messages here; only scene-scoped objects)
        sSceneExists = true;
        sSceneState = MP_SCENE_OTHER;
#if defined(TARGET_PC) && !defined(__vita__)
        sSceneTurnsSinceInit = 0;
#endif
        if (mp_active()) sHeldMode = mp_mode();
        sModulesInScene = sessionHeld();
        MpUi::sceneBegin(sModulesInScene);   // memfix: a session scene makes its UI layouts now (scene heap); the others none
        if (MpDemo::enabled()) MpDemo::sceneInit();   // MP3 demo: canned remote players (SMG_MP_DEMO=1); nothing without the switch
        if (!sModulesInScene) return;
        MpPuppets::logSceneHeap("init");   // memfix: the scene's own init is done (before every multiplayer object)
        MpPuppets::initScene();
        MpWorldSync::placementDone();
        MpRace::sceneInit();
        MpBubble::sceneInit();
        MpSpectate::sceneInit();   // MP3 spectate: the watch camera's target
        MpChat::initScene();   // MP3 chat: the speech balloons of this scene
        MpWarm::sceneInit();   // MP3 puppetprep: after every module made its scene objects: the first pictures warm them
        MpPuppets::logSceneHeap("mp");   // memfix: after multiplayer's objects (puppets, spares, tags, balloons, world)
    }

    void onGameScenePreUpdate() {
        if (!mp_active() || !sWasActive || !sSceneExists || MpGameInternal::testStalled()) return;
        MP_PROF_SCOPE(MP_PROF_TOTAL);
        for (int guard = 0; guard < 64;) {
            const uint32_t n = mp_poll_states(sMsg, 8);
            if (n == 0) break;
            for (uint32_t i = 0; i < n && mp_active(); i++) {
                if (sMsg[i].fromSlot < MP_MAX_PLAYERS && sMsg[i].len <= MP_MAX_PAYLOAD) {
                    MpPuppets::handle(sMsg[i]);
                }
            }
            guard += (int)n;
        }
    }

    void onGameScenePostCalcAnim() {
        if (MpGame::sessionHeld() && sSceneExists) {
            MpBubble::postCalcAnim();
        }
        if (!mp_active() || !sWasActive || !sSceneExists || MpGameInternal::testStalled()) return;
        MpPuppets::sendLocalPose();
    }

    void onGameSceneUpdate() {
#if defined(TARGET_PC) && !defined(__vita__)
        ++sSceneTurnsSinceInit;
#endif
        MpUi::sceneRunning();   // integ (B1): the scene's init is over (a flag store): the main thread may now make layouts
        if (MpDemo::enabled()) MpDemo::update();   // MP3 demo (SMG_MP_DEMO=1): one turn of the script
        if (!mp_entered()) return;   // MP3 netpack: multiplayer never entered in this boot: nothing below has anything to do (one bool)
#if defined(SMG_MP_PROF) && SMG_MP_PROF
        struct TurnEnd { ~TurnEnd() { MP_PROF_TURN(); } } turnEnd;   // (perf counter: one line every SMG_MP_PROF turns)
#endif
        MP_PROF_SCOPE(MP_PROF_TOTAL);
        MpSaveGame::tick();   // MP3 integration
        if (!mp_active()) {
            const bool held = sessionHeld();
            if (sWasActive) {   // the session dropped (ended or reconnecting): let modules react to the falling edge
                sWasActive = false;
                sHeld.clear();
                MpPuppets::update();   // review: the puppets of this scene go (they are no longer stepped)
                MpWorldSync::update(); // worldsync: latch s_wasActive = false so reconnect triggers immediate repair
                MpFlow::update();
                MpBubble::update();   // (holds local bubble while reconnecting; releases if session ended)
                MpSpectate::update();   // MP3 spectate: the view returns to the player's own
                MpChat::sessionTick();   // MP3 chat: the ring lets Select go
                if (!held) {             // immediate non-reconnect session end: finish cleanup in the same frame
                    sWasHeld = false;
                    sHeldMode = MP_MODE_NONE;
                    MpStarBits::update();
                }
            } else if (held) {
                MpRace::update();     // keep race countdown / clock ticking while reconnecting
                MpBubble::update();   // keep a locally bubbled player safely held while reconnecting
            } else if (sWasHeld) {    // reconnect gave up: release anything kept alive across the reconnect window
                sWasHeld = false;
                sHeldMode = MP_MODE_NONE;
                MpPuppets::update();  // clear held roster snapshot immediately
                MpBubble::update();
                MpStarBits::update();
                MpChat::sessionTick();
            }
            if (held) sWasHeld = true;
            // MP3 integration: the session layer's idle tick (menus' lobby / scans, the desktop autopilot SMG_MP_AUTO); a few
            // loads and a null test when multiplayer was never used. Then the flow's "back to the title" after a session.
            sSceneTurnsSincePump++;
            mp_tick_game(sTurn);
            drainEvents();
            MpGameInternal::afterSessionStep();
            if (retireEntered()) mp_log("game: multiplayer idle: per-frame hooks off until the menu opens again");
            return;
        }
        sWasActive = true;
        sWasHeld = true;
        sHeldMode = mp_mode();
        if (MpGameInternal::testStalled()) return;   // starfix: a modelled load runs no multiplayer turn at all (desktop test hook)
        sTurn++;
        sSceneTurnsSincePump++;
        drain(true);
        mp_tick_game(sTurn);
        drainEvents();
        MpPuppets::update();
        MpWorldSync::update();
        MpFlow::update();
        MpBubble::update();
        MpSpectate::update();   // MP3 spectate (after the bubble and the flow: it reads their state)
        MpChat::update();   // MP3 chat: the quick ring, the balloons (after the puppets moved)
    }

    void onGameSceneEnd() {
        if (MpDemo::enabled()) MpDemo::sceneEnd();   // MP3 demo
        MpHead3D::sceneEnd();   // MP3 heads3d: the character select's 3D heads go with the file select's scene
        MpUi::sceneEnd();     // memfix: the UI layouts of this scene (their memory goes with the scene heap)
        MpRace::sceneEnd();   // drops scene pointers even when the session ended meanwhile
        MpTeam::sceneEnd();
        MpStarBits::sceneEnd();   // MP3 costar
        MpBubble::sceneEnd();
        MpSpectate::sceneEnd();   // MP3 spectate
        MpWarm::sceneEnd();
        MpChat::endScene();   // MP3 chat: the balloons go with the scene
        if (sModulesInScene) {
            MpPuppets::endScene();
            MpWorldSync::endScene();
        }
        sModulesInScene = false;
        sSceneExists = false;
        sSceneState = MP_SCENE_OTHER;
    }

#if defined(TARGET_PC) && !defined(__vita__)
    // MP3 worldsync (harness test support, desktop only): SMG_MP_TEST_HOLD_LIVE=<turns> keeps this console "not live" for the first <turns> scene turns after
    // each scene start (a console still in its opening demo while the other already plays). Off unless set.
    bool isGameSceneLive() { return sSceneExists && sSceneState == MP_SCENE_ACTION && !testHeld(); }
#else
    bool isGameSceneLive() { return sSceneExists && sSceneState == MP_SCENE_ACTION; }
#endif

    uint32_t turn() { return sTurn; }
    void dropHeldWorld() { if (sHeld.count() > 0) { const int n = sHeld.dropRange(0x40, 0x4F); if (n > 0) mp_log("game held world messages discarded: %d (scene restart)", n); } }
}  // namespace MpGame
