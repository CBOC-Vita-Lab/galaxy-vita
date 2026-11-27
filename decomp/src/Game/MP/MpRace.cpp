// Galaxy 3.0 multiplayer: Star Race (owner: flow). DESIGN.md section 1 mode 2 and section 4 "Race".
// The host's scenario choice becomes a direct MOVE for everyone. After the scene start barrier each console holds its player when its own
// scene reaches Action and says READY; the host sends GO when every present racer is ready (or 10 s after its own entry) and every console
// starts its countdown on GO (racefix, MpFlowLogic RaceStartGate: the same moment for all, whatever its opening camera did). Each console
// plays the Cosmic Mario countdown (RaceManagerLayout, as GhostPlayer does) and
// counts its own logic turns from the end of the countdown. The star touch is the finish (FINISH + leaderboard submit); the finisher
// waits behind the closed star wipe (GameStageClearSequence) until RESULTS: the host publishes them when everyone present finished or
// raceWaitSec*10 s after the first finisher. Then the results card, then the host's next MOVE; a non-finisher is moved without the star.
// Every console keeps the race board (GO, FINISH list, deadline) so a migrated host can continue it.
// Series (MpSeriesLogic.hpp): the host scores every race (points per place), the winner of the last race starts the next countdown late
// (catch-up head start, a setting, default on) and the results card ends in the host's choice: Race Again, Next Star or Leave Race. GO
// and RESULTS carry the series block, so every console holds it and a new host continues from it. Times: each console counts from its
// own start; the leaderboard gets exactly those frames; the room's standings add the head start.
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpUi.hpp"         // MP3 integration: live standings on the waiting screen
#include "Game/MP/MpSpectate.hpp"  // MP3 spectate: a finisher watches a rival behind the star wipe
#include "port/mp_core_game.h"      // MP3 integration: race state for a new host (RESUME)

#include "Game/Map/RaceManager.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StringUtil.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace MpFlowLogic;

namespace {
    const int64_t kResultsCardMs = 6000;
    const int64_t kHostSilentMs = 60000;
    const int kFallbackCountTurns = 180;

    // race: the series of this room (survives from race to race; reset when the session starts or ends or the mode is not Star Race)
    MpSeriesLogic::Series sSeries;

    struct Race {
        bool active;            // a race scenario is the session's current scene
        uint16_t serial;
        const char* stage;      // ScenarioData::mGalaxyName (lives as long as the scenario data)
        uint8_t galaxy;
        int scenario, selected;
        bool goKnown, goSent;
        RaceStartGate startGate;   // racefix: who is ready (host), and when the wait for the slowest racer began
        int64_t readySentMs = -1;  // racefix (guest): when READY last went to the host
        uint8_t goBytes[16];       // racefix (host): the GO that went out, sent again to a console whose READY comes after it
        int goLen = 0;
        bool controlHeld;
        bool countdownStarted, racing, finished;
        int countStep;
        int64_t actionSinceMs;
        uint32_t frames;        // logic turns since the end of the countdown (scene turns only)
        RaceBoard board;
        bool resultsKnown;
        Results results;
        int64_t resultsMs;
        bool holdShown;
        int64_t holdResultsMs;
        bool hostLeaveIssued;
        PoseDigest digest;
        // race (series / catch-up)
        bool gateBegun;
        MpSeriesLogic::StartGate gate;                 // this console's own start: the head start wait, then its own frames
        uint32_t headFrames[MP_MAX_PLAYERS];           // each slot's head start in this race (from GO), frames at 60/s
        bool hasChoices;                               // the results carried a tail: the card with the host's next step
        uint8_t choice;                                // MpSeriesLogic::Choice the host made (CH_NONE until then)
        uint8_t next;                                  // the scenario Next Star would go to (0 = none)
        bool followUp;                                 // (host) Race Again / Next Star chosen: the next move goes to followScen
        int followScen, followSel;
        bool followIssued;                             // (host, did not finish) the direct move was requested
        int localSlot = -1;                            // cached local slot while reconnecting
    };
    Race sRace;
    RaceManagerLayout* sLayout = nullptr;   // scene-scoped (created at scene init, forgotten at scene end)
    bool sLayoutShown = false;
    MpMsg sOut;

    int64_t now() { return MpGameInternal::nowMs(); }
    int64_t limitMs() { return (int64_t)mp_settings()->raceWaitSec * 10 * 1000; }

    bool isHub(const char* stage) {
        static const char* const kHub[] = {"AstroGalaxy", "AstroDome", "LibraryRoom", "PeachCastleGardenGalaxy", "EpilogueDemoStage",
                                           "FileSelect"};
        if (!stage) return true;
        for (const char* h : kHub) if (MR::isEqualStringCase(stage, h)) return true;
        return false;
    }

    unsigned presentMask() { return MpGameInternal::presentMask(); }

    // the head start of every slot in this race, as the host sent it in GO
    void noteHeadStarts(const MpSeriesLogic::Block& b) {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            sRace.headFrames[i] = MpSeriesLogic::headStartFrames(b, i);
            sRace.board.headStart[i] = sRace.headFrames[i];
        }
    }
    int headStartSecOf(int slot) { return slot >= 0 && slot < MP_MAX_PLAYERS ? (int)(sRace.headFrames[slot] / MpSeriesLogic::kFps) : 0; }
    int headStartSlot() {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if (sRace.headFrames[i]) return i;
        return -1;
    }

    void releaseControl() {
        if (sRace.controlHeld && MpGameInternal::sceneExists() && MR::isExistMario()) MR::onPlayerControl(true);
        sRace.controlHeld = false;
    }

    void applyResults(const Results& res, const MpMsg& m) {
        if (sRace.resultsKnown) return;   // idempotent
        sRace.resultsKnown = true;
        sRace.results = res;
        sRace.hasChoices = res.hasTail;
        if (res.hasTail) {
            sRace.choice = res.tail.choice;
            sRace.next = res.tail.next;
            if (!mp_is_host()) sSeries.restore(res.tail.series);   // (the host scored the race itself when it published)
        }
        sRace.resultsMs = now();
        for (int i = 0; i < res.count; i++) {
            if (res.e[i].frames != kDnf) sRace.board.noteFinish(res.e[i].slot, res.e[i].frames, now());
        }
        {   // hooks: the card's place numbers in the list's order ("1,1" = a tie for first; 0 = did not finish), for the scenarios
            char places[MP_MAX_PLAYERS * 3 + 1];
            size_t pn = 0;
            places[0] = 0;
            for (int i = 0; i < res.count && i < MP_MAX_PLAYERS; i++)
                pn += (size_t)snprintf(places + pn, sizeof places - pn, i ? ",%d" : "%d", (int)res.e[i].place);
            mp_log("race results serial=%d n=%d first=%d frames=%u places=%s", res.serial, res.count, res.count ? res.e[0].slot : -1,
                   res.count ? res.e[0].frames : 0u, places);
        }
        MpUi::showWaiting(false, 0, 0, 0);
        MpUi::showResults(m);
    }

    // Next Star: the next scenario of this galaxy the star select would offer (the host's own progress: the star just played counts as
    // collected). 0 = none (or the scene is not this race's any more).
    int nextStarScenario() {
        if (!MpRace::isRaceScene() || sRace.selected != sRace.scenario) return 0;   // (a comet's placed scenario differs: no next star)
        const int normal = MR::getCurrentStageNormalScenarioNum();
        if (normal < 1 || normal > 32) return 0;
        unsigned mask = 0;
        for (int k = 1; k <= normal; k++) if (MR::hasPowerStarInCurrentStage(k)) mask |= 1u << (k - 1);
        return MpSeriesLogic::nextScenario(sRace.scenario, normal, mask);
    }

    void publishResults() {
        Results res;
        const unsigned mask = presentMask();
        sRace.board.rank(mask, &res);
        // the series: joins and leaves first, then this race's points and the streak
        sSeries.syncPresence(mask);
        MpSeriesLogic::Placing pl[MP_MAX_PLAYERS];
        for (int i = 0; i < res.count; i++) pl[i] = MpSeriesLogic::Placing{res.e[i].slot, res.e[i].place};
        sSeries.applyRace(pl, res.count, true);
        res.hasTail = true;
        res.tail.series = sSeries.block();
        res.tail.choice = MpSeriesLogic::CH_NONE;
        res.tail.next = (uint8_t)nextStarScenario();
        MpWriter w(sOut.data, sizeof sOut.data);
        writeResults(w, res);
        if (!w.ok()) return;
        sOut.type = MP_MSG_RESULTS;
        sOut.fromSlot = (uint8_t)mp_local_slot();
        sOut.len = (uint16_t)w.n;
        mp_send(MP_MSG_RESULTS, sOut.data, sOut.len, true);
        applyResults(res, sOut);
    }

    void sendGo() {
        // the host decides the head start: the last race's sole winner, when catch-up is on and someone is there to be given it
        const unsigned mask = presentMask();
        sSeries.syncPresence(mask);
        Go g;
        g.serial = sRace.serial;
        g.startAtTurn = MpGame::turn();
        g.hasSeries = true;
        g.series = sSeries.goBlock(true, mask);
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        writeGo(w, g);
        mp_send(MP_MSG_GO, b, (uint16_t)w.n, true);
        memcpy(sRace.goBytes, b, w.n);
        sRace.goLen = (int)w.n;
        sRace.goSent = true;
        sRace.goKnown = true;
        noteHeadStarts(g.series);
    }
    void sendReady() {
        uint8_t b[2];
        MpWriter w(b, sizeof b);
        w.u16(sRace.serial);
        if (!w.ok() || !mp_send(MP_MSG_RACEREADY, b, (uint16_t)w.n, true)) return;
        if (sRace.readySentMs < 0) mp_log("race ready sent serial=%d", sRace.serial);
        sRace.readySentMs = now();
    }
    void sendFinish() {
        uint8_t b[6];
        MpWriter w(b, sizeof b);
        writeFinish(w, Finish{sRace.serial, sRace.frames});
        mp_send(MP_MSG_FINISH, b, (uint16_t)w.n, true);
    }

    void showLayoutRacing() {
        if (!sLayout || sLayoutShown) return;
        sLayout->setTime(sRace.frames);
        sLayout->appear();
        sLayout->hideAllPane();
        MR::showPane(sLayout, "Record");
        sLayoutShown = true;
    }
    // (host) the card's choice: remembered, and told to everyone with the same RESULTS again, its tail carrying the choice
    void hostChoose(uint8_t c) {
        if (c == MpSeriesLogic::CH_NONE || c >= MpSeriesLogic::CH_COUNT || sRace.choice != MpSeriesLogic::CH_NONE) return;
        if (c == MpSeriesLogic::CH_NEXT && sRace.next == 0) c = MpSeriesLogic::CH_AGAIN;
        sRace.choice = c;
        sRace.results.tail.choice = c;
        if (c == MpSeriesLogic::CH_AGAIN) {
            sRace.followUp = true;
            sRace.followScen = sRace.scenario;
            sRace.followSel = sRace.selected;
        } else if (c == MpSeriesLogic::CH_NEXT) {
            sRace.followUp = true;
            sRace.followScen = sRace.followSel = sRace.next;
        }
        MpWriter w(sOut.data, sizeof sOut.data);
        writeResults(w, sRace.results);
        if (w.ok()) {
            sOut.type = MP_MSG_RESULTS;
            sOut.fromSlot = (uint8_t)mp_local_slot();
            sOut.len = (uint16_t)w.n;
            mp_send(MP_MSG_RESULTS, sOut.data, sOut.len, true);
        }
        MpUi::setResultsChoice(c);
        mp_log("race host chose %d next=%d serial=%d", (int)c, sRace.next, sRace.serial);
    }
}  // namespace

namespace MpRace {
    void sessionReset() {
        releaseControl();
        sRace = Race{};
        MpUi::showHeadStart(0, 0);
    }
    void seriesReset() {
        sSeries.reset();
        MpUi::closeResults();
        MpUi::forgetSeriesNames();
    }

    bool isHubStage(const char* stage) { return isHub(stage); }

    bool isRaceScene() {
        return MpGame::sessionHeld() && MpGame::heldMode() == MP_MODE_RACE && sRace.active && sRace.serial == MpGameInternal::serial() &&
               MpGameInternal::sceneExists() && MR::isEqualStringCase(MR::getCurrentStageName(), sRace.stage);
    }
    bool finishedLocal() { return sRace.finished; }
    int ownRaceTurns() { return isRaceScene() && sRace.gate.racing ? (int)sRace.gate.ownFrames() : 0; }
    bool resultsKnown() { return sRace.resultsKnown; }
    bool slotFinished(int slot) { return sRace.active && slot >= 0 && slot < MP_MAX_PLAYERS && sRace.board.frames[slot] != 0; }

    void onMoveExecuted(uint16_t serial, int moveType, const char* stageName, uint8_t galaxyIndex, int scenarioNo, int selectedNo) {
        if (mp_mode() != MP_MODE_RACE) return;
        if (!(sRace.active && sRace.serial == serial)) {   // (a move to a new scene: the results card and the head start line go)
            MpUi::closeResults();
            MpUi::showHeadStart(0, 0);
        }
        if (moveType != MOVE_DIRECT || isHub(stageName) || scenarioNo < 1) {
            if (sRace.active) mp_log("race over serial=%d (moved to %s)", sRace.serial, stageName ? stageName : "-");
            releaseControl();
            sRace.active = false;
            return;
        }
        if (sRace.active && sRace.serial == serial) return;   // idempotent (host: choice + move)
        sessionReset();
        sRace.active = true;
        sRace.serial = serial;
        sRace.stage = stageName;
        sRace.galaxy = galaxyIndex;
        sRace.scenario = scenarioNo;
        sRace.selected = selectedNo;
        sRace.localSlot = mp_local_slot();
        sRace.board.reset(serial);
        sRace.startGate.reset(serial);
        mp_core_set_race_state(false, 0);
        mp_log("race armed serial=%d galaxy=%s scen=%d sel=%d", serial, stageName, scenarioNo, selectedNo);
    }

    void sceneInit() {
        // (scene init thread) the countdown and timer layout of the Cosmic Mario races, in a race scene only
        sLayout = nullptr;
        sLayoutShown = false;
        if (!MpGame::sessionHeld() || MpGame::heldMode() != MP_MODE_RACE || !sRace.active || sRace.serial != MpGameInternal::serial() ||
            !MR::isEqualStringCase(MR::getCurrentStageName(), sRace.stage)) {
            return;
        }
        sLayout = new RaceManagerLayout("MpRaceLayout");
        sLayout->init(JMapInfoIter());
        sLayout->kill();
    }

    void sceneEnd() {
        sLayout = nullptr;
        sLayoutShown = false;
        sRace.controlHeld = false;   // the player object dies with the scene
    }

    void onBarrierReleased(uint16_t serial) {
        if (sRace.active && serial == sRace.serial) mp_log("race barrier released serial=%d", serial);
    }

    void update() {
        if (mp_active() && mp_mode() != MP_MODE_RACE && (sSeries.in || sSeries.streakLen)) sSeries.reset();   // the mode changed: no series
        if (!MpGame::sessionHeld() || MpGame::heldMode() != MP_MODE_RACE || !sRace.active) return;
        const bool active = mp_active();
        if (active) sRace.localSlot = mp_local_slot();
        const int localSlot = active ? mp_local_slot() : sRace.localSlot;
        const int64_t t = now();
        if (active && mp_is_host() && !sRace.resultsKnown && sRace.board.shouldPublish(MpGameInternal::presentMask(), limitMs(), t)) {
            publishResults();
        }
        // the host's press on the results card: Race Again / Next Star / Leave Race (one press; guests are told by RESULTS)
        if (active && sRace.resultsKnown && sRace.hasChoices && mp_is_host() && sRace.choice == MpSeriesLogic::CH_NONE) {
            int c;
            if (MpUi::takeResultsChoice(&c)) hostChoose((uint8_t)c);
        }
#if defined(TARGET_PC) && !defined(__vita__)
        {   // desktop test hook: SMG_MP_TEST_CHOICE="<c>[,<c>...][:<secs>]": the host presses choice c (1 Race Again, 2 Next Star, 3 Leave Race) for
            // the 1st, 2nd, ... race, <secs> (default 6) after the card came up. Nothing without the env.
            static int sInit = -1, sList[8], sCount = 0, sDone = 0;
            static int64_t sDelayMs = 6000;
            if (sInit < 0) {
                const char* e = getenv("SMG_MP_TEST_CHOICE");
                sInit = (e && *e) ? 1 : 0;
                for (const char* q = e; sInit && q && *q && *q != ':' && sCount < 8; ++q)
                    if (*q >= '1' && *q <= '3') sList[sCount++] = *q - '0';
                const char* c = e ? strchr(e, ':') : nullptr;
                if (c) sDelayMs = (int64_t)(atof(c + 1) * 1000.0);
            }
            static uint16_t sLastSerial = 0xFFFF;
            if (active && sInit == 1 && mp_is_host() && sRace.resultsKnown && sRace.hasChoices && sRace.choice == MpSeriesLogic::CH_NONE &&
                t - sRace.resultsMs >= sDelayMs && sRace.serial != sLastSerial) {
                sLastSerial = sRace.serial;
                const int c = sDone < sCount ? sList[sDone] : MpSeriesLogic::CH_LEAVE;
                sDone++;
                mp_log("race test choice %d", c);
                hostChoose((uint8_t)c);
            }
        }
#endif
        // a new host that inherits a decided card (the old host chose, then left): it carries the choice out
        if (active && sRace.resultsKnown && sRace.hasChoices && mp_is_host() && !sRace.followUp &&
            (sRace.choice == MpSeriesLogic::CH_AGAIN || (sRace.choice == MpSeriesLogic::CH_NEXT && sRace.next > 0))) {
            sRace.followUp = true;
            sRace.followScen = sRace.choice == MpSeriesLogic::CH_AGAIN ? sRace.scenario : sRace.next;
            sRace.followSel = sRace.choice == MpSeriesLogic::CH_AGAIN ? sRace.selected : sRace.next;
        }
        if (!isRaceScene()) return;
        const int st = MpGameInternal::sceneState();
        const bool alive = MR::isExistMario() && !MR::isPlayerDead();
        if (!sRace.countdownStarted && !sRace.finished) {
            // racefix: nobody counts down before GO. The host sends it when every present racer is in Action (or kRaceGoWaitMs after its own
            // entry), whatever its own scene does meanwhile (a pause or a miss must not hold the others); a guest reports READY until it
            // hears GO and starts on GO (at once when GO came before its own opening was over)
            if (active && mp_is_host() && st == MP_SCENE_ACTION && alive) sRace.startGate.hostEntered(localSlot, t);
            if (active && mp_is_host() && !sRace.goSent) {
                sRace.startGate.prune(presentMask(), localSlot);
                if (sRace.startGate.shouldSendGo(presentMask(), t)) {
                    mp_log("race go sent serial=%d ready=%x present=%x waited=%d ms", sRace.serial, sRace.startGate.ready, presentMask(),
                           (int)(t - sRace.startGate.waitSinceMs));
                    sendGo();
                }
            }
            if (st != MP_SCENE_ACTION || !alive) return;
            if (!sRace.controlHeld) {
                MR::offPlayerControl();
                sRace.controlHeld = true;
                sRace.actionSinceMs = t;
            }
            if (active && !mp_is_host() && RaceStartGate::shouldSendReady(true, sRace.goKnown, sRace.readySentMs, t)) sendReady();
            if (!RaceStartGate::consoleStarts(true, sRace.goKnown, sRace.actionSinceMs, t)) return;
            sRace.countdownStarted = true;
            sRace.countStep = 0;
            if (sLayout) {
                sLayout->setTime(0);
                sLayout->appear();
                sLayout->playCountAndGo();
                MR::showPane(sLayout, "Record");
                sLayout->hideBestRecordPane();
                sLayoutShown = true;
            }
            mp_log("race countdown serial=%d layout=%d", sRace.serial, sLayout ? 1 : 0);
        }
        if (RaceStartGate::needsRehold(sRace.countdownStarted, sRace.racing, sRace.finished, sRace.controlHeld, st == MP_SCENE_ACTION, alive)) {
            MR::offPlayerControl();   // racefix: a miss during the countdown / head start wait: the new scene's player is held again
            sRace.controlHeld = true;
        }
        if (sRace.countdownStarted && !sRace.racing && !sRace.finished) {
            bool done = sRace.gateBegun;   // (the countdown is over; the head start wait goes on below)
            if (!done) {
                if (sRace.countStep % 60 == 0) MR::startSystemSE("SE_SY_RACE_COUNT_DOWN", -1, -1);
                sRace.countStep++;
                if (sLayout) {
                    done = !sLayout->isPlayCountAnim();
                } else {
                    done = sRace.countStep >= kFallbackCountTurns;
                    MpUi::showWaiting(!done, 0, 0, (kFallbackCountTurns - sRace.countStep + 59) / 60);
                }
            }
            // the head start line stays under the last winner's name while the countdown plays and the winner waits
            if (!done) {
                const int hs = headStartSlot();
                if (hs >= 0) MpUi::showHeadStart(hs, headStartSecOf(hs));
                return;
            }
            // the countdown is over for the room. This console starts at once, or (the last winner) after its head start
            if (!sRace.gateBegun) {
                sRace.gateBegun = true;
                sRace.gate.begin(localSlot >= 0 && localSlot < MP_MAX_PLAYERS ? sRace.headFrames[localSlot] : 0u);
                sRace.digest.reset();
                if (sRace.gate.headFrames > 0) mp_log("race head start %u turns slot=%d", sRace.gate.headFrames, localSlot);
            }
            if (!sRace.gate.racing) {
                if (sLayout) sLayout->setTime(0);
                const int hs = headStartSlot();
                if (hs >= 0) MpUi::showHeadStart(hs, headStartSecOf(hs));
                sRace.gate.tick();   // (waiting: one turn of the head start)
                if (!sRace.gate.racing) return;
                // released this turn: its own frame 1 is the next block's
                sRace.gate.frames = 0;
            }
            sRace.racing = true;
            sRace.frames = 0;
            releaseControl();
            MR::startSystemSE("SE_SY_RACE_START", -1, -1);
            mp_log("race go serial=%d", sRace.serial);
        }
        if (sRace.racing && !sRace.finished) {
            sRace.gate.tick();
            sRace.frames = sRace.gate.ownFrames();   // (the player's own turns from its own start: the leaderboard's time)
            {   // the others see the head start line until the winner has been released
                const int hs = headStartSlot();
                if (hs >= 0 && sRace.frames <= sRace.headFrames[hs] && hs != localSlot) MpUi::showHeadStart(hs, headStartSecOf(hs));
                else MpUi::showHeadStart(0, 0);
            }
            if (!sLayoutShown) showLayoutRacing();   // a new scene after a miss: the timer goes on
            if (sLayout) sLayout->setTime(sRace.frames);
            if (MR::isExistMario()) {
                const TVec3f* p = MR::getPlayerPos();
                sRace.digest.add(p->x, p->y, p->z, sRace.frames);
            }
        }
        // a host that did not finish moves everyone on: with the card's choices once it has chosen (Race Again / Next Star: the next
        // race at once; Leave Race: the dome), else (an old-style card) once the results card has been up
        if (active && mp_is_host() && sRace.resultsKnown && !sRace.finished && !sRace.hostLeaveIssued && st == MP_SCENE_ACTION && alive &&
            (sRace.hasChoices ? sRace.choice != MpSeriesLogic::CH_NONE : t - sRace.resultsMs >= kResultsCardMs)) {
            sRace.hostLeaveIssued = true;
            releaseControl();
            if (sRace.followUp && MpFlowRace::hostDirectMove(sRace.stage, sRace.followScen, sRace.followSel)) {
                mp_log("race host starts the next race serial=%d scen=%d", sRace.serial, sRace.followScen);
            } else {
                mp_log("race host leaves without the star serial=%d", sRace.serial);
                MR::requestChangeStageGoBackAstroDome();
            }
        }
    }

    bool onStarGet(int starId, bool grand) {
        (void)grand;
        if (!isRaceScene()) return false;
        if (!sRace.racing || sRace.finished) return true;   // never a STAR in a race
        sRace.finished = true;
        uint32_t frames = sRace.frames ? sRace.frames : 1;
#if defined(TARGET_PC) && !defined(__vita__)
        {   // desktop test hook: SMG_MP_TEST_RACE_FRAMES=<n>: this console's FINISH carries exactly n frames (two consoles with the same n tie).
            // The finish itself is the real one (the star's collection); only the time is fixed. Nothing without the env.
            static int sFixed = -2;
            if (sFixed == -2) {
                const char* e = getenv("SMG_MP_TEST_RACE_FRAMES");
                sFixed = (e && atoi(e) > 0 && atoi(e) < 600000) ? atoi(e) : -1;
                if (sFixed > 0) mp_log("race test frames %d (SMG_MP_TEST_RACE_FRAMES)", sFixed);
            }
            if (sFixed > 0) frames = (uint32_t)sFixed;
        }
#endif
        const bool active = mp_active();
        const int localSlot = active ? mp_local_slot() : sRace.localSlot;
        sRace.frames = frames;
        if (localSlot >= 0 && localSlot < MP_MAX_PLAYERS) {
            sRace.board.noteFinish(localSlot, frames, now());
            MpUi::setStanding(localSlot, sRace.board.roomFrames(localSlot));
        }
        mp_core_set_race_state(true, frames);
        if (active) sendFinish();
        mp_log("race finish slot=%d frames=%u star=%d%s", localSlot, frames, starId, active ? "" : " (reconnecting)");
        if (sLayout) sLayout->playGoal();
        uint8_t digest[16];
        sRace.digest.out(digest);
        const int scen = sRace.selected > 0 ? sRace.selected : sRace.scenario;
        // integ: the leaderboard's galaxy index is the ui's table (MpMsgText::galaxyIndexOf, the Observatory order the board
        // menu asks with); WHERE / MOVE keep the scenario-data index (a wire detail both consoles resolve from their own data)
        const int boardGalaxy = MpMsgText::galaxyIndexOf(sRace.stage);
        if (boardGalaxy >= 0 && boardGalaxy <= 63) mp_race_submit((uint8_t)boardGalaxy, (uint8_t)scen, frames, digest);
        else mp_log("race time not submitted: %s is not a board galaxy", sRace.stage ? sRace.stage : "-");
        return true;
    }

    bool holdAfterClear() {
        if (!sRace.finished) return false;
        const int64_t t = now();
        if (!sRace.resultsKnown) {
            if (!mp_is_host() && MpGameInternal::hasPendingCommand()) {
                MpUi::showWaiting(false, 0, 0, 0);
                MpSpectate::raceHoldTick(true);
                if (!MpSpectate::readyToRelease()) return true;
                return MpFlowRace::runPendingDirect();
            }
            const unsigned mask = MpGameInternal::presentMask();
            MpUi::showWaiting(true, sRace.board.finishedCount(mask), popcount4(mask), sRace.board.secondsLeft(limitMs(), t));
            MpSpectate::raceHoldTick(false);   // MP3 spectate: may open the wipe on a rival (the waiting panel then stays hidden)
            return true;
        }
        MpSpectate::raceHoldTick(true);   // MP3 spectate: the results are in: a view that is open closes again
        if (sLayout && sRace.hasChoices) sLayout->kill();   // the game's own "TIME" pane must not show through the card
        if (!sRace.holdShown) {
            sRace.holdShown = true;
            sRace.holdResultsMs = t;
        }
        if (sRace.hasChoices) {
            // the card with the host's choices: nobody moves on before the host has chosen. The host's own card chooses by itself after 90 s
            // without a press (MpToast.cpp, uxfix #9), and a guest whose card still has no choice after 120 s leaves on its own (B6, MpToast.cpp:
            // MpSeriesLogic::kCardIdleGuestMs), so an idle host never holds anyone for good. A guest that is still loading or spectating is not
            // waited for beyond the scene start barrier of the next race; a host that is lost migrates.
            // the card sits on the game's own star-get backdrop (the wipe opens again on the frozen scene) until the next move is decided
            MpSpectate::raceBackdrop(mp_is_host() ? sRace.choice == MpSeriesLogic::CH_NONE : !MpGameInternal::hasPendingCommand());
            if (!MpSpectate::readyToRelease()) return true;               // (the star wipe is closing again)
            if (mp_is_host()) {
                if (sRace.choice == MpSeriesLogic::CH_NONE) return true;
                if (!sRace.followUp) return false;                        // Leave Race: the usual clear move (the galaxy map)
                // Race Again / Next Star: the next race is a direct move of this galaxy, made from here (not the star-clear's story move, which
                // queues the dome's demos). The scene goes; the sequence holds until it does.
                if (!sRace.followIssued) {
                    sRace.followIssued = true;
                    mp_log("race host starts the next race from the star wipe serial=%d scen=%d", sRace.serial, sRace.followScen);
                    if (!MpFlowRace::hostDirectMove(sRace.stage, sRace.followScen, sRace.followSel)) return false;
                }
                return true;
            }
            if (MpGameInternal::hasPendingCommand()) return MpFlowRace::runPendingDirect();   // the host's MOVE: a direct one is run from here, any other by our clear move
            return true;
        }
        if (t - sRace.holdResultsMs < kResultsCardMs) return true;   // the results card
        if (!MpSpectate::readyToRelease()) return true;               // MP3 spectate: the star wipe is closing again
        if (mp_is_host()) return false;                               // the host's own clear move becomes everyone's MOVE
        if (MpGameInternal::hasPendingCommand()) return false;        // the host's next MOVE: our clear move takes it
        if (t - sRace.holdResultsMs > kHostSilentMs) {
            mp_log("race hold timeout serial=%d: going alone", sRace.serial);
            MpGameInternal::allowOwnMoveOnce();
            return false;
        }
        return true;
    }

    void handle(const MpMsg& m) {
        if (mp_mode() != MP_MODE_RACE) return;
        switch (m.type) {
        case MP_MSG_GO: {
            Go g;
            if (!readGo(m.data, m.len, &g)) return;
            if (RaceStartGate::acceptGo(sRace.active, sRace.serial, g.serial, sRace.goKnown)) {
                sRace.goKnown = true;
                if (g.hasSeries) {
                    sSeries.restore(g.series);   // (the host's state: a guest only keeps and shows it)
                    noteHeadStarts(g.series);
                }
                mp_log("race go received serial=%d head=%d", g.serial, headStartSlot());
            }
            break;
        }
        case MP_MSG_RACEREADY: {
            uint16_t s;
            if (!readSerialOnly(m.data, m.len, &s) || !RaceStartGate::acceptReady(mp_is_host(), sRace.active, sRace.serial, s, m.fromSlot)) return;
            if (!(sRace.startGate.ready & (1u << m.fromSlot))) mp_log("race ready from slot=%d serial=%d", m.fromSlot, s);
            sRace.startGate.noteReady(m.fromSlot, s);
            if (RaceStartGate::answerLateReady(mp_is_host(), sRace.goSent, sRace.resultsKnown, sRace.goLen, m.fromSlot, mp_local_slot())) {
                mp_send_to(m.fromSlot, MP_MSG_GO, sRace.goBytes, (uint16_t)sRace.goLen, true);   // READY after GO: it starts at once on this
                mp_log("race go sent again to slot=%d serial=%d", m.fromSlot, s);
            }
            break;
        }
        case MP_MSG_FINISH: {
            Finish f;
            if (!readFinish(m.data, m.len, &f)) { mp_log("race drop FINISH from=%d (invalid)", m.fromSlot); return; }
            if (!sRace.active || f.serial != sRace.serial) return;
            if (sRace.board.noteFinish(m.fromSlot, f.frames, now())) {
                mp_log("race finish slot=%d frames=%u", m.fromSlot, f.frames);
                MpUi::setStanding(m.fromSlot, sRace.board.roomFrames(m.fromSlot));   // (the room's clock: own frames + head start)
                MpUi::toast(MP_TXT_RACE_FINISHED, m.fromSlot, sRace.board.roomFrames(m.fromSlot));
            }
            break;
        }
        case MP_MSG_RESULTS: {
            Results res;
            if (!readResults(m.data, m.len, &res)) { mp_log("race drop RESULTS from=%d (invalid)", m.fromSlot); return; }
            if (!sRace.active || res.serial != sRace.serial) return;
            if (sRace.resultsKnown) {
                // the host's choice (the same RESULTS again, its tail carrying it). Only the host's (the core drops a guest's), and only forward.
                if (res.hasTail && sRace.hasChoices && m.fromSlot == mp_host_slot() && sRace.choice == MpSeriesLogic::CH_NONE &&
                    res.tail.choice != MpSeriesLogic::CH_NONE && res.tail.next == sRace.next) {
                    sRace.choice = res.tail.choice;
                    sRace.results.tail.choice = res.tail.choice;
                    MpUi::setResultsChoice(res.tail.choice);
                    mp_log("race host choice %d serial=%d", (int)res.tail.choice, res.serial);
                }
                return;
            }
            applyResults(res, m);
            break;
        }
        case MP_MSG_HOLD: {
            Hold h;
            if (!readHold(m.data, m.len, &h)) return;
            if (h.reason == HOLD_RACE_WAIT && sRace.active && limitMs() > 0) {
                // the host's deadline: first finish = now - (limit - left)
                sRace.board.firstFinishMs = now() - (limitMs() - (int64_t)h.turnsLeft * 1000 / 60);
                mp_log("race hold left=%u turns", h.turnsLeft);
            }
            break;
        }
        default:
            break;
        }
    }

    void onBecameHost() {
        if (mp_mode() != MP_MODE_RACE || !sRace.active) return;
        if (sRace.resultsKnown) {
            MpWriter w(sOut.data, sizeof sOut.data);
            writeResults(w, sRace.results);
            if (w.ok()) mp_send(MP_MSG_RESULTS, sOut.data, (uint16_t)w.n, true);
            return;
        }
        sRace.startGate.hostChanged(now());
        if (RaceStartGate::resendGoAsNewHost(sRace.goKnown, sRace.countdownStarted)) sendGo();
        const int left = sRace.board.secondsLeft(limitMs(), now());
        if (left >= 0) {
            uint8_t b[5];
            MpWriter w(b, sizeof b);
            writeHold(w, Hold{HOLD_RACE_WAIT, (uint32_t)left * 60u});
            mp_send(MP_MSG_HOLD, b, (uint16_t)w.n, true);
        }
        mp_log("race state resent serial=%d go=%d finished=%d", sRace.serial, sRace.goKnown ? 1 : 0,
               sRace.board.finishedCount(MpGameInternal::presentMask()));
    }

    void onHostChanged() {
        if (mp_mode() == MP_MODE_RACE && sRace.active) {   // racefix: READY goes to the new host again; its wait for the slowest racer begins again
            sRace.readySentMs = -1;
            sRace.startGate.hostChanged(now());
            if (!mp_is_host() && sRace.controlHeld && !sRace.resultsKnown) sendReady();   // a guest holding in Action tells the new host at once, GO or not
        }
        if (mp_mode() == MP_MODE_RACE && sRace.active && sRace.finished) sendFinish();   // idempotent on every console
    }

    void onPeerJoined(int slot) {
        if (!mp_active() || mp_mode() != MP_MODE_RACE || !mp_is_host() || !sRace.active) return;
        if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == mp_local_slot()) return;
        if (sRace.resultsKnown) {
            MpWriter w(sOut.data, sizeof sOut.data);
            writeResults(w, sRace.results);
            if (w.ok()) {
                mp_send_to(slot, MP_MSG_RESULTS, sOut.data, (uint16_t)w.n, true);
                mp_log("race results resent to rejoined slot=%d serial=%d", slot, sRace.serial);
            }
        } else if (sRace.goSent && sRace.goLen > 0) {
            mp_send_to(slot, MP_MSG_GO, sRace.goBytes, (uint16_t)sRace.goLen, true);
            mp_log("race go resent to rejoined slot=%d serial=%d", slot, sRace.serial);
        }
    }

    void onReconnected() {
        if (!mp_active() || mp_mode() != MP_MODE_RACE || !sRace.active) return;
        sRace.localSlot = mp_local_slot();
        if (sRace.finished) {
            mp_core_set_race_state(true, sRace.frames);
        }
        if (mp_is_host()) return;
        if (sRace.finished && !sRace.resultsKnown) {
            sendFinish();
            mp_log("race finish resent after reconnect serial=%d frames=%u", sRace.serial, sRace.frames);
        }
        if (sRace.controlHeld && !sRace.goKnown && !sRace.resultsKnown) {
            sRace.readySentMs = -1;
            sendReady();
        }
    }
}  // namespace MpRace
