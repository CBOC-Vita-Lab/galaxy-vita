// Galaxy 3.0 multiplayer: game flow for both modes (owner: flow). DESIGN.md sections 1, 4 "Flow", 8 (host migration), 9.
// - Galaxy moves: the host mirrors every GameSequenceProgress::requestGalaxyMove (after the story executor decided the destination)
//   as MOVE{galaxy index, scenario, selected, start id, zone, serial, type}; guests run only the moves the host commands (validated
//   against their own scenario data; galaxies travel as an index, never a string) and take the host's final destination.
// - Scene start barrier (GameSystemSceneController::isReadyToStartScene): LOADED from every present member or 40 s (kBarrierTimeoutMs).
// - Star (Play Together): STAR from the first step of PowerStar::exeStageClearDemo; receivers deliver it natively (the PowerStar is
//   moved to the player), else a synthetic star-get demo, else record-and-leave. Star ledger: first claim wins, repeats are no-ops.
// - Lives never reach 0 in a session; Play Together loses none. Pause = the session menu. Cutscene skip = vote.
// - Host migration: every console keeps the current WHERE, the ledger, the race board; a new host re-sends WHERE (+ race state),
//   guests re-send LOADED / FINISH. Only the host role is checked (mp_is_host / mp_host_slot), never slot 0.
// Star Race specifics live in MpRace.cpp.
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpWorldGuard.hpp"
#include <cstdlib>
#include <cstring>
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpWaitLogic.hpp"   // uxwait: the "Waiting for <name>..." banner
#include "Game/MP/MpBubble.hpp"   // bubble: Play Together "Bubbles" falls
#include "Game/MP/MpWorld.hpp"    // worldsync: onPeerJoined reply-gate reset on rejoin

#include "Game/Demo/DemoDirector.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/MapObj/PowerStar.hpp"
#include "Game/MapObj/PowerStarHolder.hpp"
#include "Game/NPC/EventDirector.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GalaxyMoveArgument.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameDataFunction.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/JMapIdInfo.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/StringUtil.hpp"

#define MP_WEAK __attribute__((weak))

// ---- functions of other owners, with weak defaults here until they exist --------------------------------------------------------
#include "Game/MP/MpSaveGame.hpp"   // MP3 integration: the save agent's real API
#include "Game/MP/MpUi.hpp"         // MP3 integration: warp / dome requests from the session menu
#include "Game/MP/MpTeam.hpp"       // MP3 celebrate: party strip, team star card
#include "Game/MP/MpStarBits.hpp"   // MP3 costar: the team Star Bit count
#include "port/mp_core_game.h"      // MP3 integration: core events, WHERE / scene serial for host migration
#include "port/mp_prof.h"

using namespace MpFlowLogic;

namespace {
    const int64_t kBarrierTimeoutMs = 40000;   // a Vita loading a big galaxy cold (shader warm-up) can take 25 s+; a lost peer leaves the mask at 15 s
    const int64_t kSafePointWaitMs = 3000;
    const int64_t kJoinSnapshotWaitMs = 15000;
    const int64_t kClearHoldTimeoutMs = 60000;
    const int64_t kSkipMajorityMs = 5000;

    struct Pending {
        bool valid;
        bool isMove;
        Where w;
        int64_t sinceMs;
    };
    struct Delivery {
        bool active;
        uint16_t serial;
        int starId;
        bool grand;
        bool synthOk;       // gamefix: the star is in the scenario's PowerStarId mask: the synthetic / record paths may record it
        int fromSlot;
        int stage;          // 0 waiting to deliver natively, 1 native placed, 2 synthetic demo requested
        int64_t stageMs;
        int64_t startMs;
    };

    bool sSession = false;
    uint16_t sEpoch = 0;
    uint16_t sSerial = 0;
    bool sSerialKnown = false;
    Where sCur;                 // where the session is (the host's last WHERE / MOVE / own move): every console keeps it
    bool sCurValid = false;
    Pending sPending;           // a host command not executed yet
    uint16_t sBrowseSerial = 0xFFFF;   // touches: the scene serial whose "<host> is choosing" line was shown
    uint16_t sPickSerial = 0xFFFF;     // touches: the serial whose destination card was shown
    Pending sCommanded;         // the command being executed (gate accepted it; onGalaxyMove applies its destination)
    bool sExecuting = false;
    bool sAllowOwnOnce = false;
    bool sChoiceValid = false;  // star select (Play Together): the host's choice
    uint16_t sChoiceSerial = 0;
    uint8_t sChoiceSelected = 0;
    Barrier sBarrier;
    bool sLoadedSent = false;
    SkipVote sVote;
    StarLedger sLedger;
    Delivery sDeliv;
    bool sLocalClear = false;   // a star demo of our own in this serial
    bool sNoShareClear = false; // glitchfix G3: our own star had no shareable id: nobody was told, so we do not wait for a host move
    uint32_t sStarvDropped = 0;
    FlowClock sFlowClock;          // starfix: counts only time spent pumping messages (the 5 s window measures arrival, not handling)
    ReplyCap sReplyCap;            // starfix: at most one private reply (dropped / answered again) per second per sender
    LeftScene sLeft;               // starfix: the scene this console just LEFT (frozen at the MOVE, cleared at session end)
    StarPending sStarPending;      // starfix: a guest's own STARs the host has not answered with a STARV yet
    ClaimRetryTable sClaimRetry;   // glitchfix S1 / fix2: claims waiting for the star to appear here (4 at once; a full table refuses out loud)
    int sLastStarId = -1;
    uint16_t sLastStarSerial = 0;
    bool sRecordLeave = false;
    int64_t sRecordLeaveMs = 0;
    bool sClearHold = false;
    int64_t sClearHoldMs = 0;
    // uxwait: a guest held behind the closed star screen for the host's next move sees "Waiting for <host>..." after about 2 s
    MpWaitLogic::Tracker sWaitBanner;
    bool sWaitBannerUp = false;
    bool sWasDead = false;
    int sWarpTarget = -1;
    int64_t sWarpDeadlineMs = 0;
    bool sPresent[MP_MAX_PLAYERS] = {};
    bool sSnapshotApplied = false;
    bool sLivesClampLogged = false;
    // integ: after a session ends (left, kicked, everyone gone) the console goes back to the title / file select once the ui's
    // save-on-leave dialogs are done (the progress image of the session must not be played on as single-player)
    bool sReturnPending = false;
    int64_t sReturnSinceMs = 0;
    int64_t sReturnCheckMs = 0;
    bool sReturnAsked = false;   // gamefix: offerSaveOnLeave (file reads, 128 KB of buffers) ran once for this session end: the rest is polled cheaply

    int64_t now() { return MpGameInternal::nowMs(); }
    // uxwait: waiting = this console is held by the host's console right now (no input is blocked by the banner: it is only a line of text)
    void waitBannerStep(bool waiting) {
        const int host = mp_host_slot();
        const bool known = waiting && mp_active() && !mp_is_host() && host >= 0 && host < MP_MAX_PLAYERS && mp_slot_present(host);
        const MpWaitLogic::Out o = sWaitBanner.step(known, known ? host : -1, now());
        if (o.show || sWaitBannerUp) MpUi::waitBanner(o.show ? o.slot : -1);
        sWaitBannerUp = o.show;
    }

    unsigned presentMask() {
        unsigned m = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) if (mp_slot_present(i)) m |= 1u << i;
        int self = mp_local_slot();
        if (self >= 0 && self < MP_MAX_PLAYERS) m |= 1u << self;
        return m;
    }

    const char* roleName() { return mp_is_host() ? "host" : "guest"; }

    void setCoreSerial(uint16_t s) { mp_flow_set_scene_serial(s); }
    // integ: every console keeps the core's copy of "where the session is" current (a new host re-sends it after a migration)
    void noteCur() {
        if (!sCurValid) return;
        uint8_t b[kWhereBytes];
        MpWriter w(b, sizeof b);
        writeWhere(w, sCur, false);
        if (w.ok()) mp_core_set_where(b, (uint16_t)w.n);
    }

    // ---- scenario data: galaxy index <-> name -----------------------------------------------------------------------------------
    int galaxyCount() {
        ScenarioDataParser* p = ScenarioDataFunction::getScenarioDataParser();
        return p ? (int)p->mScenarioData.size() : 0;
    }
    const ScenarioData* galaxyAt(int idx) {
        ScenarioDataParser* p = ScenarioDataFunction::getScenarioDataParser();
        if (!p || idx < 0 || idx >= (int)p->mScenarioData.size()) return nullptr;
        return p->getScenarioData((s32)idx);
    }
    int galaxyIndexOf(const char* name) {
        if (!name || !*name) return -1;
        const int n = galaxyCount();
        for (int i = 0; i < n && i < 255; i++) {
            const ScenarioData* d = galaxyAt(i);
            if (d && d->mGalaxyName && MR::isEqualStringCase(d->mGalaxyName, name)) return i;
        }
        return -1;
    }
    // fix2: the highest scenario number of a galaxy = its ScenarioData.bcsv rows (ScenarioNo runs 1..rows in every
    // StageData/*Scenario.arc, tests/mp/scenario_data_check.py). ScenarioData::getScenarioNum() counts only the visible ones
    // (no IsHidden): hidden and green stars (BattleShipGalaxy 6 and 7, ...) would be refused as a direct move.
    int scenarioMax(const ScenarioData* d) { return d && d->mScenarioData ? d->mScenarioData->getNumEntries() : 0; }
    bool rangesOk(const Where& w, bool isMove) {
        const ScenarioData* d = galaxyAt(w.galaxy);
        if (!d) return false;
        return checkWhereRanges(w, galaxyCount(), scenarioMax(d), (int)d->getZoneNum(), isMove);
    }
    bool whereFromArg(const GalaxyMoveArgument& a, Where* w) {
        const int idx = galaxyIndexOf(a.mStageName);
        if (idx < 0) return false;
        w->galaxy = (uint8_t)idx;
        w->scenario = (a.mScenarioNo >= 1 && a.mScenarioNo <= 255) ? (uint8_t)a.mScenarioNo : 0;
        w->selected = (a._C >= 1 && a._C <= 255) ? (uint8_t)a._C : 0;
        w->startId = (a.mIDInfo._0 >= 0 && a.mIDInfo._0 <= 32767) ? (int16_t)a.mIDInfo._0 : (int16_t)-1;
        w->zone = (a.mIDInfo.mZoneID >= 0 && a.mIDInfo.mZoneID < 255) ? (uint8_t)a.mIDInfo.mZoneID : (uint8_t)kUnset8;
        w->moveType = (uint8_t)a.mMoveType;
        return true;
    }
    // The (galaxy, scenario) of the scene this console is in, for the star ledger.
    void currentKey(uint8_t* g, uint8_t* s) {
        *g = sCur.galaxy;
        *s = sCur.scenario;
        if (MpGameInternal::sceneExists()) {
            int idx = galaxyIndexOf(MR::getCurrentStageName());
            s32 sc = MR::getCurrentScenarioNo();
            if (idx >= 0) *g = (uint8_t)idx;
            if (sc >= 1 && sc <= 255) *s = (uint8_t)sc;
        }
    }

    // gamefix: is starId a real star of the scenario this console is in? mask: the scenario's PowerStarId bit (a synthetic star-get
    // may run only then); holder: the scene has that PowerStar object (the native path). Anything else is a forged / stale STAR.
    bool starIsReal(int starId, bool* synthOk) {
        *synthOk = false;
        if (starId < 1 || starId > kMaxStarId) return false;
        uint8_t g, sc;
        currentKey(&g, &sc);
        const ScenarioData* d = galaxyAt(g);
        u32 mask = 0;
        if (d && sc >= 1 && d->getValueU32("PowerStarId", (s32)sc, &mask) && starInMask(mask, (uint8_t)starId)) *synthOk = true;
        bool holderHas = false;
        if (MpGameInternal::sceneExists() && MR::isExistSceneObj(SceneObj_EventDirector)) {
            PowerStarHolder* h = EventFunction::getPowerStarHolder();
            holderHas = h && h->findPowerStarRequestInfo(starId) != nullptr;
        }
        return *synthOk || holderHas;
    }

    // glitchfix S1: does this console's scene hold the PowerStar, and has it appeared in the scene (the console's own simulation)?
    void holderState(int starId, bool* has, bool* appeared) {
        *has = *appeared = false;
        if (!MpGameInternal::sceneExists() || !MR::isExistSceneObj(SceneObj_EventDirector)) return;
        PowerStarHolder* h = EventFunction::getPowerStarHolder();
        PowerStarRequestInfo* info = h ? h->findPowerStarRequestInfo(starId) : nullptr;
        if (!info) return;
        *has = true;
        *appeared = info->mIsAppear;
    }
    void creditStar(const Star& s, int fromSlot, bool synthOk);
    void refuseClaim(const Star& s, int fromSlot, const char* why, uint8_t verdict = SV_REFUSED);
    void sendStarv(const Star& s, int bySlot, uint16_t galaxyHash, uint8_t scenario, uint8_t verdict);
    void sendStarvMsg(const Starv& v);
    void judgeLate(const Star& s, int fromSlot);
    void lateEffects(const Star& s, int fromSlot, LateResult r);
    bool applySnapshotNow();
    void sendDropReply(const Star& s, int fromSlot);
    int galaxyIndexFromHash(uint16_t h);
    void flushClaimRetriesToLate();
    void liveStarKey(uint16_t* hash, uint8_t* scen);
    bool claimerInSameScene(int slot);

    // ---- sending ----------------------------------------------------------------------------------------------------------------
    void sendWhere(int toSlot) {
        if (!sCurValid || !mp_is_host()) return;
        uint8_t b[kWhereBytes];
        MpWriter w(b, sizeof b);
        Where cur = sCur;
        cur.serial = sSerial;
        if (cur.scenario == 0) {   // the host is in its star select: the choice (WHERE / race MOVE) brings the joiner along
            mp_log("flow where to=%d deferred (star select)", toSlot);
            return;
        }
        writeWhere(w, cur, false);
        bool ok = toSlot < 0 ? mp_send(MP_MSG_WHERE, b, (uint16_t)w.n, true) : mp_send_to(toSlot, MP_MSG_WHERE, b, (uint16_t)w.n, true);
        const ScenarioData* d = galaxyAt(cur.galaxy);
        mp_log("flow where to=%d galaxy=%s scen=%d sel=%d serial=%d ok=%d", toSlot, d ? d->mGalaxyName : "?", cur.scenario, cur.selected,
               cur.serial, ok ? 1 : 0);
    }
    void sendMove(const Where& v) {
        uint8_t b[kMoveBytes];
        MpWriter w(b, sizeof b);
        writeWhere(w, v, true);
        mp_send(MP_MSG_MOVE, b, (uint16_t)w.n, true);
    }
    void sendSerialMsg(uint8_t type, uint16_t serial) {
        uint8_t b[2];
        MpWriter w(b, sizeof b);
        w.u16(serial);
        mp_send(type, b, (uint16_t)w.n, true);
    }
    // touches: "<host> is choosing a galaxy." (was "The host chooses the galaxy."). gamefix2: every refusal site goes through
    // refusalEpisode(): a held button / a per-frame retry is ONE episode (one log line, one toast), see MpFlow::RefusalGate.
    MpFlow::RefusalGate sRefusals;
    enum { kRefuseMove = 0, kRefuseGalaxyStart = 1 };
    bool refusalEpisode(int kind, uint32_t* repeats) { return sRefusals.call(kind, now(), repeats); }
    void refuseToast() {
        if (mp_active() && !mp_is_host()) MpUi::toast(MP_T_HOST_BROWSING, mp_host_slot(), 0);
        else MpUi::toast(MP_TXT_HOST_CHOOSES, -1, 0);
    }

    // ---- session lifecycle --------------------------------------------------------------------------------------------------------
    void resetAll() {
        sEpoch = mp_host_epoch();
        sSerial = 0;
        sSerialKnown = false;
        sCurValid = false;
        sPending.valid = false;
        sCommanded.valid = false;
        sExecuting = false;
        sAllowOwnOnce = false;
        sChoiceValid = false;
        sBarrier = Barrier();
        sLoadedSent = false;
        sVote.clear();
        sLedger.clear();
        sLeft.clear();
        sStarPending.clear();
        sClaimRetry = ClaimRetryTable();
        sDeliv.active = false;
        sLocalClear = false;
        sNoShareClear = false;
        sLastStarId = -1;
        sRecordLeave = false;
        sClearHold = false;
        waitBannerStep(false);
        sWasDead = false;
        sWarpTarget = -1;
        for (bool& p : sPresent) p = false;
        sSnapshotApplied = false;
        sLivesClampLogged = false;
        MpRace::sessionReset();
        MpRace::seriesReset();   // race: a room's series ends with the room
    }
    // 13 Nov (A4): a console whose link dropped and that rejoins on its own (the core's silent reconnect, up to 75 s) used to lose all of its
    // flow state here (the session "ended" for the flow): the host's WHERE then looked like a new scene, and the returning player sat on the
    // scene-entry barrier and replayed the galaxy's intro. While the core reconnects the flow now HOLDS its state (serial, barrier, holds):
    // a rejoin to the same slot goes on as if nothing happened (the host's WHERE for the scene we are in only refreshes sCur); a different
    // slot, or the reconnect giving up, ends it the old way.
    void epochCheck();
    bool sHeld = false;
    int sHeldSlot = -1, sLastSlot = -1;
    bool sLastIsHost = false;
    bool coreReconnecting() {
        MpUiStatus st;
        memset(&st, 0, sizeof st);
        mp_ui_status(&st);
        return st.phase == MP_UI_RECONNECTING;
    }
    bool ensureSession() {
        const bool active = mp_active();
        if (active) {
            sLastSlot = mp_local_slot();
            sLastIsHost = mp_is_host();
        }
        if (sHeld) {
            if (active) {
                sHeld = false;
                if (mp_local_slot() == sHeldSlot && sSession) {
                    mp_log("flow session resumed after the reconnect role=%s slot=%d serial=%d known=%d", roleName(), mp_local_slot(), sSerial, sSerialKnown ? 1 : 0);
                    const uint16_t prevEpoch = sEpoch;
                    epochCheck();   // (a host change during the outage: as the core's event would have done)
                    const bool epochChanged = (sEpoch != prevEpoch);
                    MpRace::onReconnected();
                    // Flush any local STAR collected during the reconnect outage (skip if epochCheck() already flushed sStarPending).
                    if (!epochChanged) {
                        if (mp_is_host()) {
                            Starv answers[StarPending::kEntries];
                            const int answered = becameHostAnswers(sStarPending, mp_local_slot(), mp_host_epoch(), answers, StarPending::kEntries);
                            for (int i = 0; i < answered; i++) {
                                uint8_t b[16];
                                MpWriter w(b, sizeof b);
                                Star st{answers[i].serial, answers[i].starId, answers[i].grand, answers[i].by, MpGame::turn(), answers[i].galaxyHash, answers[i].scenario};
                                writeStar(w, st);
                                mp_send(MP_MSG_STAR, b, (uint16_t)w.n, true);
                                sendStarvMsg(answers[i]);
                                mp_log("flow star id=%d serial=%d flushed after reconnect (host)", answers[i].starId, answers[i].serial);
                            }
                        } else {
                            const int64_t t = now();
                            for (StarPending::Entry& e : sStarPending.e) {
                                if (!e.valid) continue;
                                uint8_t b[16];
                                MpWriter w(b, sizeof b);
                                writeStar(w, e.s);
                                mp_send(MP_MSG_STAR, b, (uint16_t)w.n, true);
                                e.sinceMs = e.sentMs = t;
                                mp_log("flow star id=%d serial=%d flushed after reconnect", e.s.starId, e.s.serial);
                            }
                        }
                    }
                    return true;
                }
                sSession = false;   // another slot: a new session for the flow (the edge below starts it)
            } else if (coreReconnecting()) {
                return false;
            } else {
                sHeld = false;   // the reconnect gave up: the session ends below
            }
        }
        if (!active && sSession && !sHeld && sLastSlot >= 0 && coreReconnecting()) {
            sHeld = true;
            sHeldSlot = sLastSlot;
            mp_log("flow session held while the core reconnects slot=%d serial=%d", sHeldSlot, sSerial);
            return false;
        }
        if (active != sSession) {
            sSession = active;
            resetAll();
            if (active) {
                // the slots already here when we start are not "new" (the host sends WHERE on a join edge only)
                for (int i = 0; i < MP_MAX_PLAYERS; i++) sPresent[i] = mp_slot_present(i);
                mp_log("flow session start role=%s slot=%d mode=%d", roleName(), mp_local_slot(), (int)mp_mode());
                // joinfix: a guest already here at the start has no join edge below: it still gets the host's progress
                bool guestHere = false;
                for (int i = 0; i < MP_MAX_PLAYERS; i++) guestHere = guestHere || (sPresent[i] && i != mp_local_slot());
                if (offerSnapshotAtSessionStart(mp_is_host(), MpGameInternal::sceneExists(), MR::isStageFileSelect(), guestHere)) MpSaveGame::offerSnapshot();
            } else {
                MpUi::showWaiting(false, 0, 0, 0);
                mp_log("flow session end");
                sReturnPending = true;
                sReturnSinceMs = now();
                sReturnCheckMs = 0;
                sReturnAsked = false;
            }
        }
        return active;
    }

    void becameHost() {
        mp_log("flow host now slot=%d epoch=%d serial=%d", mp_local_slot(), mp_host_epoch(), sSerial);
        MpUi::toast(MP_TXT_NEW_HOST, mp_local_slot(), 0);
        if (sSerialKnown) sendWhere(-1);
        // starfix: our own claims the old host never answered: our own play credited them, so as the host we answer them ourselves
        Starv answers[StarPending::kEntries];
        const int answered = becameHostAnswers(sStarPending, mp_local_slot(), mp_host_epoch(), answers, StarPending::kEntries);
        for (int i = 0; i < answered; i++) sendStarvMsg(answers[i]);
        // (the STARVs above may be dropped by a console that has not learned our epoch yet: an image of our progress brings everyone to the same state)
        if (answered && MpGameInternal::sceneExists() && !MR::isStageFileSelect() && !MpSaveGame::offerSnapshot()) mp_log("flow became host: the snapshot could not be offered");
        MpRace::onBecameHost();
        // the barrier and the holds continue on their own; LOADED of this serial is ours already
    }
    void hostChanged() {
        mp_log("flow host changed to=%d epoch=%d", mp_host_slot(), mp_host_epoch());
        MpUi::toast(MP_TXT_NEW_HOST, mp_host_slot(), 0);
        if (sLoadedSent && sSerialKnown) sendSerialMsg(MP_MSG_LOADED, sSerial);   // idempotent on every console
        // starfix (D): our own STARs the old host never answered go to the new host (it judges them with ITS OWN LeftScene, or live)
        for (StarPending::Entry& e : sStarPending.e) {
            if (!e.valid) continue;
            uint8_t b[16];
            MpWriter w(b, sizeof b);
            writeStar(w, e.s);
            mp_send(MP_MSG_STAR, b, (uint16_t)w.n, true);
            e.sinceMs = e.sentMs = now();
            mp_log("flow star id=%d serial=%d sent again to the new host", e.s.starId, e.s.serial);
        }
        MpRace::onHostChanged();
    }
    void epochCheck() {
        const uint16_t e = mp_host_epoch();
        if (e == sEpoch) return;
        sEpoch = e;
        if (mp_is_host()) becameHost(); else hostChanged();
    }
    void presenceCheck() {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            const bool p = mp_slot_present(i);
            if (p && !sPresent[i] && i != mp_local_slot()) {
                mp_log("flow join slot=%d", i);
                if (mp_is_host()) {
                    // integ: the joiner gets the host's progress first (the core sends the current image to every guest)
                    if (MpGameInternal::sceneExists() && !MR::isStageFileSelect()) MpSaveGame::offerSnapshot();
                    sendWhere(i);
                }
                // a drop-in joiner arms its barrier for the scene everyone is already playing: tell it we are loaded (our
                // LOADED went out before it was here), or it waits out the whole barrier timeout alone
                if (sLoadedSent && sSerialKnown && !sBarrier.armed) sendSerialMsg(MP_MSG_LOADED, sSerial);   // (idempotent everywhere)
            }
            if (!p && sPresent[i]) sLeft.slotGone(i);   // starfix: the frozen in-scene bit goes with the player, a newcomer in that slot does not inherit it
            sPresent[i] = p;
        }
    }

    // ---- star delivery (Play Together) ---------------------------------------------------------------------------------------------
    bool deliverNative(int starId) {
        if (!MR::isExistSceneObj(SceneObj_EventDirector)) return false;
        PowerStarHolder* holder = EventFunction::getPowerStarHolder();
        if (!holder) return false;
        PowerStarRequestInfo* info = holder->findPowerStarRequestInfo(starId);
        if (!info || !info->mStar) return false;
        PowerStar* star = info->mStar;
        const TVec3f pos = *MR::getPlayerCenterPos();
        info->mIsAppear = true;
        star->mPosition.set(pos);
        star->mAppearPosition.set(pos);
        star->mInitPosition.set(pos);
        star->mpMakeCollectable();   // a demo-cast (boss) star or one waiting for its appear demo could not be touched otherwise
        return true;
    }
    void deliverSynthetic(const Delivery& d) {
        if (!d.synthOk) { mp_log("flow star synthetic refused id=%d (not a star of this scenario)", d.starId); return; }   // gamefix
        MR::sendStageResultSequenceParam(d.starId);
        if (d.grand) MR::requestGrandStarGetDemo(); else MR::requestPowerStarGetDemo();
        MR::offPlayerControl();
        MR::startBckPlayer(d.grand ? "GrandStarGet" : "PowerStarGet", (s32)0);
    }
    void startRecordLeave(int starId) {
        if (starId > 0) MR::sendStageResultSequenceParam(starId);
        MR::closeWipeCircle(80);
        sRecordLeave = true;
        sRecordLeaveMs = now();
        mp_log("star delivered id=%d path=record", starId);
    }
    void deliveryStep(int64_t t) {
        if (!sDeliv.active) return;
        if (sDeliv.serial != sSerial) {
            mp_log("flow star delivery dropped id=%d (serial %d, now %d)", sDeliv.starId, sDeliv.serial, sSerial);
            sDeliv.active = false;
            return;
        }
        if (!MpGameInternal::sceneExists()) return;   // reloading: deliver in the next scene of this serial
        const int st = MpGameInternal::sceneState();
        if (st == MP_SCENE_STAR_GET || st == MP_SCENE_GRAND_STAR_GET) {
            if (sDeliv.stage == 2) mp_log("star delivered id=%d path=synthetic", sDeliv.starId);
            else mp_log("star delivered id=%d path=native", sDeliv.starId);   // (the star demo began without our onStarGet)
            sDeliv.active = false;
            return;
        }
        if (MpBubble::localBubbled()) MpBubble::popInPlace("star");   // bubble: the star comes to a player out of his bubble
        const bool alive = MR::isExistMario() && !MR::isPlayerDead() && !MpBubble::localBubbled();
        const bool action = st == MP_SCENE_ACTION && alive;
        const bool bound = alive && bindHoldsDelivery(MR::isPlayerInBind(), t - sDeliv.startMs);   // glitchfix G4
        switch (sDeliv.stage) {
        case 0:
            if (bound) {
                // glitchfix G4: bound to a launch star / pull star: wait for the bind to end (bounded), never strike the pose mid-flight
            } else if (action && MR::canStartDemo() && !MR::isDemoActive()) {
                if (deliverNative(sDeliv.starId)) {
                    mp_log("flow star native placed id=%d", sDeliv.starId);
                    sDeliv.stage = 1;
                } else {
                    deliverSynthetic(sDeliv);
                    sDeliv.stage = 2;
                }
                sDeliv.stageMs = t;
            } else if (t - sDeliv.stageMs > 3000 && action) {   // (not while bound: the first branch)
                deliverSynthetic(sDeliv);
                sDeliv.stage = 2;
                sDeliv.stageMs = t;
            } else if (t - sDeliv.startMs > 15000) {
                sDeliv.active = false;
                startRecordLeave(sDeliv.synthOk ? sDeliv.starId : 0);
            }
            break;
        case 1:
            if (t - sDeliv.stageMs > 1500 && !bound) {
                if (action) {
                    deliverSynthetic(sDeliv);
                    sDeliv.stage = 2;
                    sDeliv.stageMs = t;
                } else if (t - sDeliv.startMs > 15000) {
                    sDeliv.active = false;
                    startRecordLeave(sDeliv.synthOk ? sDeliv.starId : 0);
                }
            }
            break;
        default:
            if (t - sDeliv.stageMs > 2500) {
                sDeliv.active = false;
                startRecordLeave(sDeliv.synthOk ? sDeliv.starId : 0);
            }
            break;
        }
    }
    void recordLeaveStep(int64_t t) {
        if (!sRecordLeave || !MpGameInternal::sceneExists() || MR::isWipeActive()) {
            if (!sClearHold) waitBannerStep(false);   // uxwait (a clear hold of its own keeps the banner)
            return;
        }
        const bool host = mp_is_host();
        if (!host && !sPending.valid && t - sRecordLeaveMs < kClearHoldTimeoutMs) {   // wait behind the wipe for the host
            waitBannerStep(true);   // uxwait
            return;
        }
        waitBannerStep(false);
        if (!host && !sPending.valid) {
            mp_log("flow record leave: no host move, going alone");
            sAllowOwnOnce = true;
        }
        sRecordLeave = false;
        MR::requestChangeStageAfterStageClear();
    }

    // ---- guests: run the host's commands ---------------------------------------------------------------------------------------
    bool safePoint() {
        if (MR::isDemoActive() || MR::isPlayerInBind()) return false;
        return MR::isOnGroundPlayer() || MR::isPlayerSwimming();
    }
    void runMove(int type, const Where& w) {
        const ScenarioData* d = galaxyAt(w.galaxy);
        const char* name = d ? d->mGalaxyName : nullptr;
        JMapIdInfo id(w.startId < 0 ? 0 : w.startId, w.zone == kUnset8 ? 0 : w.zone);
        sExecuting = true;
        if (type == MOVE_DIRECT && name) {
            GalaxyMoveArgument arg(MOVE_DIRECT, name, w.scenario ? w.scenario : 1, w.startId >= 0 ? &id : nullptr);
            arg._C = w.selected ? w.selected : -1;
            GameSequenceFunction::requestGalaxyMove(arg);
        } else if (type == MOVE_STAR_SELECT && name) {
            GameSequenceFunction::requestGalaxyMove(GalaxyMoveArgument(MOVE_STAR_SELECT, name, w.scenario ? w.scenario : -1, nullptr));
        } else {
            // back to the dome / after loading: the story executor decides, onGalaxyMove applies the host's destination
            GameSequenceFunction::requestGalaxyMove(GalaxyMoveArgument(type == MOVE_AFTER_LOAD ? MOVE_AFTER_LOAD : MOVE_BACK_TO_DOME,
                                                                       nullptr, 1, nullptr));
        }
        sExecuting = false;
        if (sPending.valid && !sCommanded.valid) {
            // (the gate did not run: should not happen) drop it so we do not loop
            mp_log("flow move not taken type=%d serial=%d", type, w.serial);
            sPending.valid = false;
        }
    }
    void executePending(int64_t t) {
        if (!sPending.valid || !MpGameInternal::sceneExists() || sRecordLeave) return;
#if defined(TARGET_PC) && !defined(__vita__)
        {   // MP3 worldsync (harness test support, desktop only): SMG_MP_TEST_MOVE_DELAY_MS=<ms> keeps a guest in its scene that long after the host's MOVE arrived
            static int64_t sTestDelayMs = -1;
            if (sTestDelayMs < 0) { const char* e = getenv("SMG_MP_TEST_MOVE_DELAY_MS"); sTestDelayMs = e ? (int64_t)atoll(e) : 0; }
            if (sTestDelayMs > 0 && t - sPending.sinceMs < sTestDelayMs) return;
        }
#endif
        const int st = MpGameInternal::sceneState();
        if (st == MP_SCENE_STAR_GET || st == MP_SCENE_GRAND_STAR_GET) return;   // holdAfterClear releases it
        if (st != MP_SCENE_ACTION && st != MP_SCENE_OPENING && st != MP_SCENE_STARTER) return;
        if (!MR::isExistMario() || MR::isPlayerDead()) return;                  // the reload comes first
        const Where w = sPending.w;
        const bool fileSelect = MR::isStageFileSelect();
        if (fileSelect || !sSnapshotApplied) {
            const bool waiting = mp_snapshot_ready() > 0;
            if (applySnapshotNow()) MpSaveGame::writeRecoveryCheckpoint();   // MP3 integration
            if (!mp_active()) return;   // a refused snapshot left the session this turn: run no pending MOVE
            if ((waiting && mp_snapshot_ready() == 0) || MpSaveGame::snapshotAppliedThisSession()) sSnapshotApplied = true;
            if (fileSelect && !sSnapshotApplied && t - sPending.sinceMs < kJoinSnapshotWaitMs) return;   // "sync valid" gate
        }
        if (!safePoint() && t - sPending.sinceMs < kSafePointWaitMs) return;
        int type = sPending.isMove ? w.moveType : (int)MOVE_DIRECT;
        if (fileSelect) {
            type = MOVE_AFTER_LOAD;
        } else if (type == MOVE_AFTER_CLEAR) {
            if (mp_mode() == MP_MODE_TOGETHER && !MpRace::isRaceScene()) {
                if (sDeliv.active) return;   // the delivery ends in a star demo or a record-and-leave
                if (sLastStarId > 0 && sLastStarSerial == sSerial) {
                    startRecordLeave(sLastStarId);
                    return;
                }
            }
            type = MOVE_BACK_TO_DOME;   // a race non-finisher (or no star known) goes without the star
        }
        mp_log("flow run command type=%d serial=%d", type, w.serial);
        // MP3 touches: the destination card, before the load (a join at the file select is no pick; the star select's own choice shows it)
        if (!fileSelect && type == MOVE_DIRECT && w.scenario >= 1 && sPickSerial != w.serial) {
            sPickSerial = w.serial;
            MpUi::announcePick(mp_host_slot(), galaxyAt(w.galaxy) ? galaxyAt(w.galaxy)->mGalaxyName : nullptr, w.selected >= 1 ? w.selected : w.scenario);
        }
        runMove(type, w);
    }

    // ---- deaths and warps ---------------------------------------------------------------------------------------------------------
    void deathStep() {
        if (!MpGameInternal::sceneExists() || !MR::isExistMario()) return;
        const bool dead = MR::isPlayerDead();
        if (dead && !sWasDead) {
            mp_log("flow dead slot=%d serial=%d", mp_local_slot(), sSerial);
            if (sSerialKnown) sendSerialMsg(MP_MSG_DEAD, sSerial);
        }
        sWasDead = dead;
    }
    void warpStep(int64_t t) {
        if (sWarpTarget < 0) return;
        const int target = sWarpTarget;
        if (t > sWarpDeadlineMs || !mp_slot_present(target)) {
            mp_log("flow warp failed to=%d (timeout)", target);
            MpUi::toast(MP_TXT_WARP_FAILED, target, 0);
            sWarpTarget = -1;
            return;
        }
        if (!MpGameInternal::sceneExists() || MpGameInternal::sceneState() != MP_SCENE_ACTION || !MR::isExistMario() ||
            MR::isPlayerDead() || MR::isDemoActive() || MR::isPlayerInBind() || MpBubble::localBubbled()) {
            return;   // after the retry start
        }
        MpPose them, me;
        if (!MpPuppets::lastPose(target, &them) || !MpPuppets::lastPose(mp_local_slot(), &me)) return;
        // MP3 dropin: zone 0xFF = unknown (MpLocalCapture always sends it: positions are global in a scene); only two known,
        // different zones refuse (before: every warp failed with zone=255/255)
        const bool zoneMismatch = them.zone != 0xFF && me.zone != 0xFF && them.zone != me.zone;
        if (them.sceneSerial != sSerial || zoneMismatch || (them.flags & (MP_POSE_DEAD | MP_POSE_IN_DEMO | MP_POSE_BUBBLE))) {
            mp_log("flow warp failed to=%d serial=%d/%d zone=%d/%d", target, them.sceneSerial, sSerial, them.zone, me.zone);
            MpUi::toast(MP_TXT_WARP_FAILED, target, 0);
            sWarpTarget = -1;
            return;
        }
        for (int i = 0; i < 3; i++) if (!mp_pose_float_ok(them.pos[i])) { sWarpTarget = -1; return; }
        // MP3 bubble2: the warp arrives in a bubble beside the friend (pops by itself in ~2 s); else the plain teleport
        if (!MpBubble::arriveInBubble("warp", target)) MR::setPlayerPos(TVec3f(them.pos[0], them.pos[1], them.pos[2]));
        uint8_t b[1] = {(uint8_t)target};
        mp_send(MP_MSG_WARPREQ, b, 1, true);
        mp_log("flow warp to=%d pos=%.0f,%.0f,%.0f", target, them.pos[0], them.pos[1], them.pos[2]);
        mp_log("warp slot=%d target=%d", mp_local_slot(), target);   // hooks: the scenarios' line
        MpTeam::noteHelped();   // MP3 celebrate: a friend warped to counts for the star card
        sWarpTarget = -1;
    }

    // ---- message handlers ------------------------------------------------------------------------------------------------------------
    void takeCommand(const Where& w, bool isMove) {
        sCur = w;
        sCurValid = true;
        noteCur();
        if (mp_is_host()) return;   // (a host only sees its own commands through migration leftovers)
        if (sSerialKnown && !serialNewer(w.serial, sSerial)) return;           // done already
        if (sPending.valid && !serialNewer(w.serial, sPending.w.serial)) {
            if (!isMove && w.serial == sPending.w.serial && w.selected) {      // the star select choice of a pending MOVE
                sChoiceValid = true; sChoiceSerial = w.serial; sChoiceSelected = w.selected;
            }
            return;
        }
        sPending.valid = true;
        sPending.isMove = isMove;
        sPending.w = w;
        sPending.sinceMs = now();
        const ScenarioData* d = galaxyAt(w.galaxy);
        mp_log("flow command %s type=%d galaxy=%s scen=%d sel=%d serial=%d", isMove ? "move" : "where", isMove ? w.moveType : -1,
               d ? d->mGalaxyName : "?", w.scenario, w.selected, w.serial);
    }
    void handleWhere(const MpMsg& m, bool isMove) {
        Where w;
        if (!readWhere(m.data, m.len, &w, isMove) || !rangesOk(w, isMove)) {
            mp_log("flow drop %s from=%d len=%d (invalid)", isMove ? "MOVE" : "WHERE", m.fromSlot, m.len);
            return;
        }
        if (!isMove && sSerialKnown && w.serial == sSerial) {
            sCur = w;
            sCurValid = true;
        noteCur();
            if (w.selected) {   // the host's star select choice for the scene we are in
                sChoiceValid = true; sChoiceSerial = w.serial; sChoiceSelected = w.selected;
            }
            return;
        }
        takeCommand(w, isMove);
    }
    void handleStar(const MpMsg& m) {
        Star s;
        if (!readStar(m.data, m.len, &s)) { mp_log("flow drop STAR from=%d (invalid)", m.fromSlot); return; }
        if (mp_mode() != MP_MODE_TOGETHER) return;
        // starfix: a star is judged ONCE, by the host, and every console credits on the host's STARV. A guest does not judge (no retry, no
        // clock of its own): the claim it sees here is the host's relay of another console's STAR.
        if (!mp_is_host()) { mp_log("flow STAR id=%d by=%d serial=%d seen (credit waits for the host's verdict)", s.starId, m.fromSlot, s.serial); return; }
        static MpWorldGuard::SenderLimits sStarLimits;   // MP3 security: a guest's STAR is judged on the leader's console: its rate
        StarClaimIn in;
        in.s = s; in.from = m.fromSlot; in.sceneKnown = sSerialKnown; in.spFile = MpSaveGame::playingOnSinglePlayerFile();
        in.slotPresent = m.fromSlot >= 0 && m.fromSlot < MP_MAX_PLAYERS && mp_slot_present(m.fromSlot);
        in.flowNow = sFlowClock.t; in.hostSerial = sSerial; in.claimGalaxyIdx = galaxyIndexFromHash(s.galaxyHash);
        bool synthOk = false, has = false, appeared = false;
        if (sSerialKnown && s.serial == sSerial) {   // the live probes (only a live claim reads the scene)
            in.liveReal = starIsReal(s.starId, &synthOk);
            in.liveSynthOk = synthOk;
            if (in.liveReal) holderState(s.starId, &has, &appeared);
            in.liveHas = has; in.liveAppeared = appeared;
            in.claimerSameScene = claimerInSameScene(m.fromSlot);
            in.fromLocal = m.fromSlot == mp_local_slot();
        }
        const StarDecision d = hostDecideStar(in, sLedger, sLeft, [&] {
            return m.fromSlot == mp_host_slot() || sStarLimits.allow(m.fromSlot, MpWorldGuard::K_STAR, (uint32_t)(now() * 3u / 50u));
        });
        switch (d.act) {
        case StarAct::DropReply:
            mp_log("flow refused STAR id=%d by=%d: %s", s.starId, m.fromSlot, sSerialKnown ? "rate" : "no scene known");
            sendDropReply(s, m.fromSlot);
            return;
        case StarAct::AnswerAgain:   // a repeat of a claim already credited (a resend after a host change or a lost STARV): "credited" again, never judged afresh
            mp_log("flow star dup id=%d by=%d serial=%d (answered again)", s.starId, m.fromSlot, s.serial);
            if (m.fromSlot >= 0 && m.fromSlot < MP_MAX_PLAYERS && m.fromSlot != mp_local_slot() && sReplyCap.allow(m.fromSlot, now())) sendStarv(s, m.fromSlot, s.galaxyHash, s.scenario, SV_CREDITED);
            return;
        case StarAct::CreditLate: case StarAct::RefuseLate:   // a scene the host has moved on from: its own memory of it decides
            lateEffects(s, m.fromSlot, d.late);
            return;
        case StarAct::RefuseNoStar:   // gamefix: not a star of this galaxy / scenario: never run the star sequence
            mp_log("flow drop STAR id=%d by=%d (no such star here)", s.starId, m.fromSlot);
            refuseClaim(s, m.fromSlot, "no such star in this scenario", SV_NO_STAR);   // fix2: never silent (starfix: on a co-op file too)
            return;
        case StarAct::RefuseVerdict:
            refuseClaim(s, m.fromSlot, "not a star this console can credit (single-player file)", SV_NOT_THERE);
            return;
        case StarAct::Retry:   // glitchfix S1: object sync lag may bring the star here a moment later
            if (!sClaimRetry.add(s, m.fromSlot, synthOk, now() + kClaimRetryMs)) {
                mp_log("flow claim retry table full (%d): STAR id=%d by=%d", kClaimRetryMax, s.starId, m.fromSlot);
                refuseClaim(s, m.fromSlot, "too many claims waiting", SV_REFUSED);
                return;
            }
            mp_log("flow STAR id=%d by=%d waits for the star to appear here (single-player file)", s.starId, m.fromSlot);
            return;
        case StarAct::CreditLive: break;
        }
        if (in.spFile && m.fromSlot != mp_local_slot() && !(has && appeared))
            mp_log("flow STAR id=%d by=%d credited without appearing here (scenario star, same scene, console-only kind: has=%d grand=%d)", s.starId, m.fromSlot, has ? 1 : 0, s.grand);
        creditStar(s, m.fromSlot, synthOk);
        uint16_t gh; uint8_t sc;
        liveStarKey(&gh, &sc);
        sendStarv(s, m.fromSlot, gh, sc, SV_CREDITED);
    }
    // starfix: the key of the scene this console is in, as a STAR / STARV carries it (a name hash: the same on every console)
    void liveStarKey(uint16_t* hash, uint8_t* scen) {
        uint8_t g, sc;
        currentKey(&g, &sc);
        const ScenarioData* d = galaxyAt(g);
        *hash = d && d->mGalaxyName ? mp_name_hash15(d->mGalaxyName) : (uint16_t)0;
        *scen = sc >= 1 ? sc : (uint8_t)1;
    }
    // starfix (E): hash -> galaxy index. The same function object keys already use; names are unique (tests/mp/scenario_data_check.py), an unknown hash maps to nothing.
    int galaxyIndexFromHash(uint16_t h) {
        const int n = galaxyCount();
        for (int i = 0; i < n && i < 255; i++) {
            const ScenarioData* d = galaxyAt(i);
            if (d && d->mGalaxyName && mp_name_hash15(d->mGalaxyName) == h) return i;
        }
        return -1;
    }
    uint32_t scenarioStarMask(int galaxyIdx, int scenario) {
        const ScenarioData* d = galaxyAt(galaxyIdx);
        u32 mask = 0;
        return d && scenario >= 1 && d->getValueU32("PowerStarId", (s32)scenario, &mask) ? (uint32_t)mask : 0u;
    }
    // starfix (a): the host's verdict, to every console
    void sendStarvMsg(const Starv& v) {
        if (!mp_is_host()) return;
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        writeStarv(w, v);
        mp_send(MP_MSG_STARV, b, (uint16_t)w.n, true);
        mp_log("flow star verdict id=%d by=%d serial=%d verdict=%d", v.starId, v.by, v.serial, v.verdict);   // hooks: the scenarios' line
    }
    void sendStarv(const Star& s, int bySlot, uint16_t galaxyHash, uint8_t scenario, uint8_t verdict) {
        sendStarvMsg(Starv{s.serial, mp_host_epoch(), s.starId, (uint8_t)bySlot, s.grand, galaxyHash, scenario, verdict});
    }
    // starfix: a STAR dropped without judgement (rate cap, no scene known) is answered to its sender only, so it does not wait out the 60 s; at most 1 / s / sender
    void sendDropReply(const Star& s, int fromSlot) {
        if (!mp_is_host() || fromSlot < 0 || fromSlot >= MP_MAX_PLAYERS || fromSlot == mp_local_slot()) return;
        if (!sReplyCap.allow(fromSlot, now())) return;
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        writeStarv(w, Starv{s.serial, mp_host_epoch(), s.starId, (uint8_t)fromSlot, s.grand, s.galaxyHash, s.scenario, SV_DROPPED});
        mp_send_to(fromSlot, MP_MSG_STARV, b, (uint16_t)w.n, true);
    }
    // starfix (B.1, d): what the scene being left held, taken at the MOVE BEFORE the teardown (both sites of onGalaxyMove). Nothing is read later.
    void captureLeftScene() {
        sLeft.clear();
        if (!sSerialKnown || !MpGameInternal::sceneExists() || MR::isStageFileSelect()) return;
        const int g = galaxyIndexOf(MR::getCurrentStageName());
        const s32 sc = MR::getCurrentScenarioNo();
        if (g < 0 || sc < 1 || sc > 255) return;
        const ScenarioData* d = galaxyAt(g);
        if (!d || !d->mGalaxyName) return;
        LeftScene L;
        L.valid = true;
        L.galaxy = (uint8_t)g; L.scenario = (uint8_t)sc; L.galaxyHash = mp_name_hash15(d->mGalaxyName);
        L.serial = sSerial; L.leftAtMs = sFlowClock.tick(now(), kFlowStepCapMs);
        L.scenMask = (uint8_t)(scenarioStarMask(g, sc) & 0xFF);
        for (int id = 1; id <= kMaxStarId; id++) {
            bool has = false, appeared = false;
            holderState(id, &has, &appeared);
            if (has) L.hasMask |= (uint8_t)(1u << (id - 1));
            if (appeared) L.appearedMask |= (uint8_t)(1u << (id - 1));
        }
        // (grandMask stays 0: every late grand star is refused, so nothing reads it)
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            MpPose p;
            if (i == mp_local_slot() || (mp_slot_present(i) && MpPuppets::lastPose(i, &p) && p.sceneSerial == sSerial)) L.inSerialMask |= (uint8_t)(1u << i);
        }
        sLeft = L;
        mp_log("flow left scene serial=%d galaxy=%s scen=%d has=%02x appeared=%02x scen_mask=%02x in_scene=%x", L.serial, d->mGalaxyName, sc, L.hasMask, L.appearedMask, L.scenMask, L.inSerialMask);
    }
    // starfix (B): a claim of a scene the host has already left, judged from sLeft only (never the live scene, never a live pose)
    void judgeLate(const Star& s, int fromSlot) {
        lateEffects(s, fromSlot, lateClaimJudge(sLeft, s, fromSlot, fromSlot >= 0 && fromSlot < MP_MAX_PLAYERS && mp_slot_present(fromSlot), sFlowClock.t, MpSaveGame::playingOnSinglePlayerFile()));
    }
    // the effects of a late judgement (the decision is lateClaimJudge, called by judgeLate and by hostDecideStar)
    void lateEffects(const Star& s, int fromSlot, LateResult r) {
        if (r != LateResult::Credit) {
            static const char* const why[] = {"credit", "no scene remembered", "not the scene the host just left", "after the 5 s window", "claimer not present", "galaxy or scenario differ from the scene left",
                                              "a grand star cannot be saved late", "no such star in the scenario", "claimer was not in that scene", "the star had not appeared there"};
            mp_log("flow late STAR id=%d by=%d serial=%d/%d refused: %s", s.starId, fromSlot, s.serial, sSerial, why[(int)r]);
            refuseClaim(s, fromSlot, why[(int)r], lateVerdictCode(r));
            return;
        }
        const ScenarioData* d = galaxyAt(sLeft.galaxy);
        if (!d || !d->mGalaxyName) { refuseClaim(s, fromSlot, "no scenario data", SV_REFUSED); return; }
        if (!sLedger.claim(sLeft.galaxy, sLeft.scenario, s.starId, fromSlot, s.serial)) {
            mp_log("flow star dup id=%d by=%d (late)", s.starId, fromSlot);
        } else {
            mp_log("flow star late id=%d by=%d serial=%d/%d", s.starId, fromSlot, s.serial, sSerial);
            GameDataFunction::setGameFlagPowerStarSuccess(d->mGalaxyName, s.starId, true);   // reads: writes the save holder by name, needs no scene
            MpUi::toast(MP_TXT_STAR_GOT, fromSlot, s.starId);
            MpTeam::onStar(fromSlot, s.serial, s.starId);
            if (!MpSaveGame::offerSnapshot()) mp_log("flow star late id=%d: the snapshot could not be offered", s.starId);   // the star is in the host's game data now: the next image holds it (what stops the overwrite)
        }
        sendStarv(s, fromSlot, sLeft.galaxyHash, sLeft.scenario, SV_CREDITED);
    }
    // starfix (review 4): a snapshot may predate this session's credits (the core keeps the last complete image ready while a newer one arrives), so after
    // one is applied the flags of the session's credited non-grand stars are set again (idempotent)
    bool applySnapshotNow() {
        const bool applied = MpSaveGame::applySnapshotIfReady();
        if (applied && sLedger.n > 0) {
            const int n = replayLedgerFlags(sLedger, [](uint8_t g, uint8_t sc) { return scenarioStarMask(g, sc); },
                                            [](uint8_t g, uint8_t, uint8_t id) {
                                                const ScenarioData* d = galaxyAt(g);
                                                if (d && d->mGalaxyName) GameDataFunction::setGameFlagPowerStarSuccess(d->mGalaxyName, id, true);
                                            });
            if (n) mp_log("flow star credits set again after a snapshot n=%d", n);
        }
        return applied;
    }
    // starfix (review 3): claims waiting in the 3 s retry table when the host moves on are judged NOW, against the scene just captured (update() does not run
    // until the next scene does: a load and up to 40 s of barrier later)
    void flushClaimRetriesToLate() {
        for (int i = 0; i < kClaimRetryMax; i++) {
            ClaimRetryEntry& e = sClaimRetry.slot[i];
            if (!e.valid) continue;
            const ClaimRetryEntry r = e;
            e.valid = false;
            if (mp_is_host()) judgeLate(r.s, r.from);
        }
    }
    // starfix (D): the host's verdict on EVERY console, the claimer's included
    void handleStarv(const MpMsg& m) {
        Starv v;
        if (!readStarv(m.data, m.len, &v)) { mp_log("flow drop STARV from=%d (invalid)", m.fromSlot); return; }
        if (!starvAccepted(m.fromSlot, mp_host_slot(), v.epoch, mp_host_epoch())) {
            if (++sStarvDropped <= 8 || (sStarvDropped & 63) == 0) mp_log("flow drop STARV from=%d epoch=%d (host slot=%d epoch=%d) n=%u", m.fromSlot, v.epoch, mp_host_slot(), mp_host_epoch(), sStarvDropped);
            return;
        }
        const int gi = galaxyIndexFromHash(v.galaxyHash);
        StarvIn in;
        in.v = v; in.fromSlot = m.fromSlot; in.hostSlot = mp_host_slot(); in.hostEpoch = mp_host_epoch();
        in.isHost = mp_is_host(); in.together = mp_mode() == MP_MODE_TOGETHER; in.localSlot = mp_local_slot();
        in.galaxyIdx = gi; in.scenMask = gi >= 0 ? scenarioStarMask(gi, v.scenario) : 0u;
        uint8_t g, sc;
        currentKey(&g, &sc);
        in.live = sSerialKnown && v.serial == sSerial && MpGameInternal::sceneExists() && gi >= 0 && (int)g == gi && sc == v.scenario;
        if (in.live) { bool so = false; in.liveHolderReal = starIsReal(v.starId, &so); }
        const bool synthOk = gi >= 0 && starInMask(in.scenMask, v.starId);
        Star s{v.serial, v.starId, v.grand, v.by, 0, v.galaxyHash, v.scenario};
        const StarvAct act = decideStarv(in);
        if (act == StarvAct::Ignore || act == StarvAct::Drop) return;
        if ((int)v.by == mp_local_slot()) sStarPending.answer(v.serial, v.starId);
        switch (act) {
        case StarvAct::RefusedOther:
            mp_log("flow star final id=%d by=%d serial=%d verdict=refused(%d)", v.starId, v.by, v.serial, v.verdict);
            return;
        case StarvAct::RefusedOwn: case StarvAct::RefusedOwnToast:
            mp_log("flow star final id=%d by=%d serial=%d verdict=refused(%d)", v.starId, v.by, v.serial, v.verdict);
            if (gi >= 0) sLedger.erase((uint8_t)gi, v.scenario, v.starId, v.serial);   // only the ledger entry: the flag our own play set stays
            if (act == StarvAct::RefusedOwnToast) MpUi::toast(MP_T_STAR_NOT_SAVED, mp_local_slot(), v.starId);   // (the host's other refusals reach us as its TOAST)
            return;
        case StarvAct::CreditedOwn:   // (d) the claimer re-sets the flag too: a snapshot may have overwritten its RAM credit
            if (synthOk && !v.grand) {
                GameDataFunction::setGameFlagPowerStarSuccess(galaxyAt(gi)->mGalaxyName, v.starId, true);
                mp_log("flow star final id=%d by=%d serial=%d verdict=credited (own, flag set again)", v.starId, v.by, v.serial);
            } else mp_log("flow star final id=%d by=%d serial=%d verdict=credited (own)", v.starId, v.by, v.serial);
            return;
        case StarvAct::Veto:   // the guest's veto: the STATIC scenario mask (or, in the very scene, the scene's own holder). Never claimerInSameScene, never holder state of the claimer.
            refuseClaim(s, v.by, "not a star of that scenario");
            mp_log("flow star final id=%d by=%d serial=%d verdict=vetoed", v.starId, v.by, v.serial);
            return;
        case StarvAct::CreditLive:
            mp_log("flow star final id=%d by=%d serial=%d verdict=credited", v.starId, v.by, v.serial);
            creditStar(s, v.by, synthOk);
            return;
        case StarvAct::NotRecorded:
            mp_log("flow star final id=%d by=%d serial=%d verdict=credited (recorded: not in that scene)", v.starId, v.by, v.serial);
            mp_log("flow star id=%d by=%d not recorded (%s)", v.starId, v.by, gi < 0 ? "unknown galaxy" : "grand star outside its scene");
            return;
        case StarvAct::RecordLate:
            mp_log("flow star final id=%d by=%d serial=%d verdict=credited (recorded: not in that scene)", v.starId, v.by, v.serial);
            if (!sLedger.claim((uint8_t)gi, v.scenario, v.starId, v.by, v.serial)) { mp_log("flow star dup id=%d by=%d (recorded)", v.starId, v.by); return; }
            GameDataFunction::setGameFlagPowerStarSuccess(galaxyAt(gi)->mGalaxyName, v.starId, true);
            MpUi::toast(MP_TXT_STAR_GOT, v.by, v.starId);
            MpTeam::onStar(v.by, v.serial, v.starId);
            return;
        default: return;
        }
    }
    // starfix: a guest's own STARs the host never answered: a new host answers them itself, 60 s without a verdict ends the wait out loud
    void starPendingStep(int64_t t) {
        Star s;
        while (expireStarClaim(sStarPending, sLedger, t, kClearHoldTimeoutMs, [](uint16_t h) { return galaxyIndexFromHash(h); }, &s)) {   // (the ledger entry goes too: a replay must not set a flag the host never confirmed)
            mp_log("flow star id=%d serial=%d: no verdict from the host after %d s", s.starId, s.serial, (int)(kClearHoldTimeoutMs / 1000));
            MpUi::toast(MP_T_STAR_NOT_SAVED, mp_local_slot(), s.starId);
        }
        // an unanswered claim goes to the host again every 10 s (a dropped STARV, or one sent before this console knew the new epoch, cannot lose it)
        while (!mp_is_host() && sStarPending.takeResend(t, kStarResendMs, &s)) {
            uint8_t b[16];
            MpWriter w(b, sizeof b);
            writeStar(w, s);
            mp_send(MP_MSG_STAR, b, (uint16_t)w.n, true);
            mp_log("flow star id=%d serial=%d sent again (no verdict yet)", s.starId, s.serial);
        }
    }
    // fix2: is the claimer's console in the scene serial this console is in? (its last pose says; none known = no)
    bool claimerInSameScene(int slot) {
        MpPose p;
        return sSerialKnown && MpPuppets::lastPose(slot, &p) && p.sceneSerial == sSerial;
    }
    // fix2: a claim finally refused is never silent: log it, tell the players with a toast (the leader sends it to every console)
    void refuseClaim(const Star& s, int fromSlot, const char* why, uint8_t verdict) {
        mp_log("flow refused STAR id=%d by=%d: %s", s.starId, fromSlot, why);
        MpUi::toast(MP_T_STAR_NOT_SAVED, fromSlot, s.starId);
        if (!mp_is_host()) return;
        sendStarv(s, fromSlot, s.galaxyHash, s.scenario, verdict);   // starfix (a): a refusal is a verdict too
        uint8_t b[7];
        MpWriter w(b, sizeof b);
        w.u16(MP_T_STAR_NOT_SAVED); w.u8((uint8_t)fromSlot); w.u32(s.starId);
        mp_send(MP_MSG_TOAST, b, (uint16_t)w.n, true);
    }
    void claimRetryStep(int64_t t) {
        for (int i = 0; i < kClaimRetryMax; i++) {
            ClaimRetryEntry& e = sClaimRetry.slot[i];
            if (!e.valid) continue;
            const ClaimRetryEntry r = e;
            if (!mp_is_host() || !sSerialKnown || !MpSaveGame::playingOnSinglePlayerFile()) { e.valid = false; continue; }
            if (r.s.serial != sSerial) { e.valid = false; judgeLate(r.s, r.from); continue; }   // starfix: the host moved on while the claim waited: the scene it left decides
            bool has = false, appeared = false;
            holderState(r.s.starId, &has, &appeared);
            const ClaimVerdict cv = claimVerdict(true, r.from == mp_local_slot(), has, appeared, r.synthOk, r.s.grand != 0, claimerInSameScene(r.from));
            if (cv == ClaimVerdict::Accept) {
                e.valid = false;
                creditStar(r.s, r.from, r.synthOk);
                uint16_t gh; uint8_t sc;
                liveStarKey(&gh, &sc);
                sendStarv(r.s, r.from, gh, sc, SV_CREDITED);   // starfix (a): after the retry too
                continue;
            }
            if (t >= r.untilMs) {
                e.valid = false;
                refuseClaim(r.s, r.from, "not appeared on this console (single-player file, waited)", SV_NOT_THERE);
            }
        }
    }
    // glitchfix G2: the ledger claim and the delivery (or, for a second star while we are in our own demo, its direct record)
    void creditStar(const Star& s, int fromSlot, bool synthOk) {
        uint8_t g, sc;
        currentKey(&g, &sc);
        if (!sLedger.claim(g, sc, s.starId, fromSlot, s.serial, s.grand != 0)) { mp_log("flow star dup id=%d by=%d", s.starId, fromSlot); return; }
        mp_log("flow star got id=%d grand=%d by=%d", s.starId, s.grand, fromSlot);
        sLastStarId = s.starId;
        sLastStarSerial = s.serial;
        MpUi::toast(MP_TXT_STAR_GOT, fromSlot, s.starId);
        MpTeam::onStar(fromSlot, s.serial, s.starId);   // MP3 celebrate: flash the head, send our tally
        const LateStar plan = lateStarPlan(sLocalClear, sDeliv.active, s.grand != 0, synthOk);
        if (plan == LateStar::Record) {
            // already in a star demo of our own: this console's result sequence holds one star, so the extra one is recorded directly
            GameDataFunction::setGameFlagPowerStarSuccess(MR::getCurrentStageName(), s.starId, true);
            mp_log("flow star id=%d by=%d recorded without a demo (a star demo is already running here)", s.starId, fromSlot);
            return;
        }
        if (plan == LateStar::NoticeOnly) {
            mp_log("flow star id=%d grand=%d by=%d credited in the ledger only (a star demo is already running; no demo for it here)", s.starId, s.grand, fromSlot);
            return;
        }
        sDeliv.active = true;
        sDeliv.serial = s.serial;
        sDeliv.starId = s.starId;
        sDeliv.grand = s.grand != 0;
        sDeliv.synthOk = synthOk;
        sDeliv.fromSlot = fromSlot;
        sDeliv.stage = 0;
        sDeliv.startMs = sDeliv.stageMs = now();
    }
    void handleSkipVote(const MpMsg& m) {
        SkipVoteMsg v;
        if (!readSkipVote(m.data, m.len, &v)) return;
        if (!sSerialKnown || v.serial != sSerial) return;
        if (sVote.vote(m.fromSlot, v.serial, v.hash, now())) {
            const unsigned mask = presentMask();
            mp_log("flow skip vote %d/%d", sVote.votes(v.serial, v.hash, mask), popcount4(mask));
            MpUi::toast(MP_TXT_SKIP_VOTE, m.fromSlot, (uint32_t)sVote.votes(v.serial, v.hash, mask) | ((uint32_t)popcount4(mask) << 8));
        }
    }

    // ---- skip vote: what is on screen ---------------------------------------------------------------------------------------------
    enum { kSkipMovie = 1, kSkipOpening = 2, kSkipStarter = 3, kSkipDemo = 4, kSkipTalk = 5 };   // PORT_SKIP_* (port/compat.h)
    uint32_t contextHash(int kind) {
        if (kind == kSkipDemo) {
            const char* name = DemoFunction::getDemoDirector()->getCurrentDemoName();
            return hashName(name ? name : "?", (uint32_t)kind);
        }
        return hashName(MR::getCurrentStageName(), (uint32_t)kind * 0x10001u + (uint32_t)MR::getCurrentScenarioNo());
    }
    unsigned eligibleMask(int kind) {
        unsigned mask = presentMask();
        if (kind != kSkipDemo) return mask;   // movies and the galaxy intro play on everyone
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!(mask & (1u << i)) || i == mp_local_slot()) continue;
            MpPose p;
            if (MpPuppets::lastPose(i, &p) && (p.sceneSerial != sSerial || !(p.flags & MP_POSE_IN_DEMO))) mask &= ~(1u << i);
        }
        return mask;
    }
}  // namespace

namespace {
    // integ: the session menu's "Return to the Observatory" once confirmed (and the desktop test hook below calls the same function)
    void domeRequestStep() {
        if (!mp_is_host()) {
            mp_log("flow dome request ignored (guest)");
        } else if (!MpGameInternal::sceneExists() || MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isStageFileSelect()) {
            mp_log("flow dome request ignored (not in play)");
        } else {
            // the host's own move back to the Observatory: onGalaxyMove mirrors it as MOVE type 3 for everyone
            mp_log("flow dome request serial=%d", sSerial);
            mp_log("observatory slot=%d", mp_local_slot());   // hooks: the scenarios' line
            MR::requestChangeStageGoBackAstroDome();
        }
    }

    // integ: the session menu's requests (ui), polled once per turn
    void menuRequestStep() {
        int slot = -1;
        if (MpUi::takeWarpRequest(&slot)) MpFlowHooks::requestWarpTo(slot);
        if (MpUi::takeDomeRequest()) domeRequestStep();
    }

    // integ: back to the title / file select after a session ended, once the ui's leave dialogs (save the progress?) are done
    void returnToTitleStep() {
        if (!sReturnPending) return;
        if (mp_active()) { sReturnPending = false; return; }   // (a new session started meanwhile)
        if (!MpGameInternal::sceneExists() || MR::isStageFileSelect()) {
            if (MR::isStageFileSelect()) sReturnPending = false;   // already there
            return;
        }
        const int64_t t = now();
        if (MpUi::isMenuOpen() || mp_dialog_active() || MpGameInternal::sceneState() != MP_SCENE_ACTION) return;
        if (t - sReturnSinceMs < 600000) {
            if (t - sReturnCheckMs < 1000) return;   // (once a second)
            sReturnCheckMs = t;
            if (!sReturnAsked) {   // gamefix: the file-reading offer is asked once; then only the cheap "still undecided" flag
                sReturnAsked = true;
                MpSaveGame::LeaveOffer offer;
                if (MpSaveGame::offerSaveOnLeave(&offer) && offer.offer) return;   // the decision is not made yet
            } else if (MpSaveGame::leaveDecisionPending()) {
                return;
            }
        }
        sReturnPending = false;
        mp_log("flow back to title after the session");
        MR::requestChangeSceneTitle();
    }
}  // namespace

namespace MpGameInternal {
    uint16_t serial() { return sSerial; }
    void afterSessionStep() {
        if (sHeld) ensureSession();
        if (sReturnPending) returnToTitleStep();
    }
    bool returnPending() { return sReturnPending; }   // optfix: the back-to-title step still waits
    // integ: session events from the core (MpGame.cpp drains mp_core_poll_event)
    void onCoreEvent(const MpCoreEvent& ev) {
        switch (ev.kind) {
        case MP_EV_BECAME_HOST:
        case MP_EV_HOST_CHANGED:
            mp_log("flow core event %s slot=%d old=%d", ev.kind == MP_EV_BECAME_HOST ? "became_host" : "host_changed", ev.slot, ev.other);
            if (ensureSession()) epochCheck();   // WHERE (+ race state) re-sent / LOADED re-sent; world re-sends SWITCHMAP itself
            break;
        case MP_EV_RESUMED:
            // a guest answered our HOST_CHANGE: bring it along if it is not in our scene
            mp_log("flow core event resumed slot=%d serial=%d (ours %d) finished=%d", ev.slot, ev.serial, sSerial, ev.finished);
            if (ensureSession() && mp_is_host() && sSerialKnown && ev.serial != sSerial) sendWhere(ev.slot);
            break;
        case MP_EV_JOINED:
        case MP_EV_LEFT:
            mp_log("flow core event %s slot=%d", ev.kind == MP_EV_JOINED ? "joined" : "left", ev.slot);   // (toasts: ui)
            // review: a quick rejoin (reconnect after a Wi-Fi drop / sleep) takes its slot back while the slot never stopped
            // being present here, so presenceCheck sees no join edge: reset remote state and resend WHERE / race state / world gate.
            if (ev.kind == MP_EV_JOINED && ev.other == 1 && ev.slot != mp_local_slot() && ensureSession()) {
                MpPuppets::onPeerJoined(ev.slot);
                MpBubble::onPeerJoined(ev.slot);
                if (mp_is_host()) {
                    MpWorldSync::onPeerJoined(ev.slot);
                    MpRace::onPeerJoined(ev.slot);
                    if (sSerialKnown) sendWhere(ev.slot);
                }
            }
            break;
        case MP_EV_STARTED:
            mp_log("flow core event started host=%d", ev.slot);
            break;
        default:
            break;
        }
    }
    bool hasPendingCommand() { return sPending.valid; }
    void allowOwnMoveOnce() { sAllowOwnOnce = true; }
    unsigned presentMask() { return ::presentMask(); }
}

// ==== test builds: SMG_MP_TEST_STAR="<secs>[:<slot>]" =====================================================================================
// <secs> after each scene start (game clock), that console's current scenario star is brought to its player the native way
// (deliverNative: the star appears on the player), so the real collection path runs headless: PowerStar::exeStageClearDemo ->
// onStarGet -> STAR (Play Together) / FINISH (Star Race) -> the others' delivery. Desktop only; nothing without the env.
namespace {
    void testStarStep(int64_t t) {
#if defined(TARGET_PC) && !defined(__vita__)
        {   // SMG_MP_TEST_STAR_AFTER_MOVE_MS=<ms>: this console collects its scenario star <ms> after it RECEIVED the host's move back to the Observatory (a pending MOVE_BACK_TO_DOME),
            // while it is still in the old scene (SMG_MP_TEST_MOVE_DELAY_MS keeps it there): the STAR carries the old serial and reaches a host that has already moved, in an order no
            // scene timing can change (two fixed delays raced: both fired in the same turn). Once per process.
            static int64_t sAfterMs = -2;
            static bool sAfterDone = false;
            if (sAfterMs == -2) {
                const char* e = getenv("SMG_MP_TEST_STAR_AFTER_MOVE_MS");
                sAfterMs = (e && *e) ? (int64_t)atoll(e) : -1;
                if (sAfterMs >= 0) mp_log("flow test star: %ld ms after the host's move back to the Observatory arrives", (long)sAfterMs);
            }
            if (sAfterMs >= 0 && !sAfterDone && sPending.valid && sPending.isMove && sPending.w.moveType == MOVE_BACK_TO_DOME && t - sPending.sinceMs >= sAfterMs &&
                MpGameInternal::sceneExists() && !MR::isStageFileSelect() && MpGameInternal::sceneState() == MP_SCENE_ACTION && MR::isExistMario() && !MR::isPlayerDead() &&
                safePoint() && MR::canStartDemo()) {
                sAfterDone = true;
                const int star = (int)MR::getCurrentScenarioNo();
                const bool ok = deliverNative(star);
                mp_log("flow test star: scenario star %d to the player serial=%d ok=%d (after the move)", star, (int)mp_scene_serial(), ok ? 1 : 0);
            }
        }
        static int sOn = -1;
        static int64_t sDelayMs = 0;
        static int sSlot = -1;
        if (sOn < 0) {
            const char* e = getenv("SMG_MP_TEST_STAR");
            sOn = (e && *e) ? 1 : 0;
            if (sOn) {
                sDelayMs = (int64_t)(atof(e) * 1000.0);
                const char* c = strchr(e, ':');
                sSlot = c ? atoi(c + 1) : -1;
                mp_log("flow test star: %ld ms after each scene start, slot %d", (long)sDelayMs, sSlot);
            }
        }
        if (!sOn) return;
        static int sSceneSerial = -1;
        static int64_t sSceneStartMs = 0;
        static bool sDone = false;
        if (!MpGameInternal::sceneExists() || MR::isStageFileSelect()) {
            sSceneSerial = -1;
            return;
        }
        const int serial = mp_scene_serial();
        if (serial != sSceneSerial) {
            sSceneSerial = serial;
            sSceneStartMs = t;
            sDone = false;
        }
        if (sDone || t - sSceneStartMs < sDelayMs) return;
        // in a Star Race the delay also counts from this console's own start (not from the scene start: a console that waits for the others to
        // load, or out a head start, would otherwise "finish" in the first turns of its race: 0:00.03 on the card)
        if (MpRace::isRaceScene() && MpRace::ownRaceTurns() < (int)(sDelayMs * 60 / 1000)) return;
        // like a real delivery: only in normal play, standing, no demo (the galaxy's fly-in / title card would carry it away)
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || !MR::isExistMario() || MR::isPlayerDead() || !safePoint() ||
            !MR::canStartDemo())
            return;
        sDone = true;
        if (sSlot >= 0 && mp_local_slot() != sSlot) return;
        const int star = (int)MR::getCurrentScenarioNo();
        const bool ok = deliverNative(star);
        mp_log("flow test star: scenario star %d to the player serial=%d ok=%d", star, serial, ok ? 1 : 0);
#else
        (void)t;
#endif
    }
}  // namespace

// ==== test builds: the session menu's host actions ================================================================================
// SMG_MP_TEST_DOME=<secs>: <secs> after each scene start (game clock) the host does what "Return to the Observatory" does once confirmed
//   (the flow's own consumer of the menu's request, domeRequestStep).
// SMG_MP_TEST_KICK="<secs>[:<slot>]": <secs> after each scene start the host removes <slot> (default 1) with the call the Remove button's
//   confirmation makes (mp_ui_kick), once per run.
// "Go to a Friend" is SMG_MP_TEST_WARP in MpBubble.cpp (MpFlowHooks::requestWarpTo, the call the menu's request ends in).
// Desktop only; nothing (one static read) without the env.
namespace {
    void testMenuHostStep(int64_t t) {
#if defined(TARGET_PC) && !defined(__vita__)
        static int sOn = -1;
        static int64_t sDomeMs = -1, sKickMs = -1;
        static int sKickSlot = 1;
        if (sOn < 0) {
            const char* d = getenv("SMG_MP_TEST_DOME");
            const char* k = getenv("SMG_MP_TEST_KICK");
            if (d && *d) sDomeMs = (int64_t)(atof(d) * 1000.0);
            if (k && *k) {
                sKickMs = (int64_t)(atof(k) * 1000.0);
                const char* c = strchr(k, ':');
                if (c) sKickSlot = atoi(c + 1);
            }
            sOn = (sDomeMs >= 0 || sKickMs >= 0) ? 1 : 0;
            if (sOn) mp_log("flow test menu: dome %ld ms, kick %ld ms slot %d", (long)sDomeMs, (long)sKickMs, sKickSlot);
        }
        if (!sOn) return;
        static int sSceneSerial = -1;
        static int64_t sSceneStartMs = 0;
        static bool sDomeDone = false, sKickDone = false;
        if (!mp_active() || !MpGameInternal::sceneExists() || MR::isStageFileSelect()) {
            sSceneSerial = -1;
            return;
        }
        const int serial = mp_scene_serial();
        if (serial != sSceneSerial) {
            sSceneSerial = serial;
            sSceneStartMs = t;
            sDomeDone = false;
        }
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || !mp_is_host()) return;
        if (sKickMs >= 0 && !sKickDone && t - sSceneStartMs >= sKickMs) {
            sKickDone = true;
            mp_log("kick test: slot %d", sKickSlot);
            mp_ui_kick(sKickSlot);
        }
        if (sDomeMs >= 0 && !sDomeDone && t - sSceneStartMs >= sDomeMs) {
            sDomeDone = true;
            mp_log("dome test: serial=%d", serial);
            domeRequestStep();
            MpGameInternal::testStallArm();   // starfix: the host's load after this move (SMG_MP_TEST_STALL_MS)
        }
#else
        (void)t;
#endif
    }
}  // namespace

// ==== contract entry points (MpGame.hpp) ============================================================================================
namespace MpFlow {
    void update() {
        if (!ensureSession()) {
            returnToTitleStep();
            return;
        }
        if (MpGameInternal::testStalled()) return;   // starfix: a modelled load ticks no flow clock (desktop test hook; false on the Vita)
        MP_PROF_SCOPE(MP_PROF_FLOW);
        const int64_t t = now();
        sFlowClock.tick(t, kFlowStepCapMs);
        epochCheck();
        presenceCheck();
        executePending(t);
        deliveryStep(t);
        claimRetryStep(t);
        starPendingStep(t);
        recordLeaveStep(t);
        if (!sClearHold && !sRecordLeave) waitBannerStep(false);   // uxwait
        deathStep();
        menuRequestStep();
        warpStep(t);
        testStarStep(t);
        testMenuHostStep(t);
        MpTeam::update();   // MP3 celebrate: the scene's counters, the card's end
        MpStarBits::update();   // MP3 costar: the team Star Bit count
        MpRace::update();
        if (!MpGameInternal::sceneExists()) sClearHold = false;
    }

    void handle(const MpMsg& m) {
        if (!ensureSession()) return;
        sFlowClock.tick(now(), kFlowStepCapMs);   // before the first message of a pump is handled: a load gap counts as one step
        MP_PROF_SCOPE(MP_PROF_FLOW);
        switch (m.type) {
        case MP_MSG_WHERE: handleWhere(m, false); break;
        case MP_MSG_MOVE: handleWhere(m, true); break;
        case MP_MSG_LOADED: {
            uint16_t s;
            if (readSerialOnly(m.data, m.len, &s)) sBarrier.noteLoaded(m.fromSlot, s);
            break;
        }
        case MP_MSG_STAR: handleStar(m); break;
        case MP_MSG_STARV: handleStarv(m); break;
        case MP_MSG_TALLY: if (!MpStarBits::handle(m)) MpTeam::handle(m); break;   // MP3 celebrate (costar: the team Star Bit DELTA / TOTAL have other lengths)
        case MP_MSG_SKIPVOTE: handleSkipVote(m); break;
        case MP_MSG_DEAD: {
            uint16_t s;
            if (readSerialOnly(m.data, m.len, &s) && sSerialKnown && s == sSerial) {
                mp_log("flow dead slot=%d serial=%d", m.fromSlot, s);
                MpUi::toast(MP_TXT_PLAYER_DOWN, m.fromSlot, 0);
            }
            break;
        }
        case MP_MSG_WARPREQ: {
            uint8_t target;
            if (readWarpReq(m.data, m.len, &target) && (int)target == mp_local_slot()) MpUi::toast(MP_TXT_WARPED_TO_YOU, m.fromSlot, 0);
            break;
        }
        case MP_MSG_GO: case MP_MSG_RACEREADY: case MP_MSG_FINISH: case MP_MSG_RESULTS: case MP_MSG_HOLD:
            MpRace::handle(m);
            break;
        case MP_MSG_BUBBLE: case MP_MSG_RESTART:
            MpBubble::handle(m);   // bubble: Play Together "Bubbles" falls
            break;
        default:
            break;
        }
    }

    bool holdSceneStart(bool announceLoaded) {
        if (!ensureSession()) {
            if (!sHeld) return false;
            MpGameInternal::pumpSystem();
            if (!ensureSession()) return sHeld && sBarrier.armed;
        }
        MpGameInternal::pumpSystem();
        if (MpGameInternal::testStalled()) return sBarrier.armed;   // starfix: a modelled load ticks no flow clock; the hold goes on
        sFlowClock.tick(now(), kFlowStepCapMs);
        epochCheck();
        if (!sBarrier.armed) return false;
        waitBannerStep(false);   // uxwait: the waiting card (below) explains this wait
        const int64_t t = now();
        if (sPending.valid && serialNewer(sPending.w.serial, sBarrier.serial)) {
            mp_log("flow barrier release serial=%d waited=%dms players=%d (superseded)", sBarrier.serial, sBarrier.waitedMs(t), 0);
            sBarrier.disarm();
            MpUi::showWaiting(false, 0, 0, 0);
            return false;
        }
        // (merge fix 2: while the renderer's warm-up hold keeps this console's screen closed the session is pumped but LOADED is not sent:
        // everyone starts together, when this console can open its screen. The caller caps that at 5 s: MpStartHold.hpp)
        if (!announceLoaded) return true;
        if (!sLoadedSent) {
            sLoadedSent = true;
            sBarrier.noteLoaded(mp_local_slot(), sBarrier.serial);
            sendSerialMsg(MP_MSG_LOADED, sBarrier.serial);
        }
        const unsigned mask = presentMask();
        if (sBarrier.shouldRelease(mask, t, kBarrierTimeoutMs)) {
            mp_log("flow barrier release serial=%d waited=%dms players=%d", sBarrier.serial, sBarrier.waitedMs(t),
                   sBarrier.loadedCount(mask));
            const uint16_t s = sBarrier.serial;
            sBarrier.disarm();
            MpUi::showWaiting(false, 0, 0, 0);
            MpRace::onBarrierReleased(s);
            return false;
        }
        const int64_t left = kBarrierTimeoutMs - sBarrier.waitedMs(t);
        MpUi::showWaiting(true, sBarrier.loadedCount(mask), popcount4(mask), (int)((left + 999) / 1000));
        return true;
    }

    void onGalaxyMove(void* galaxyMoveArgument) {
        if (!ensureSession() || !galaxyMoveArgument) return;
        GalaxyMoveArgument& a = *static_cast< GalaxyMoveArgument* >(galaxyMoveArgument);
        const int64_t t = now();
        if (sCommanded.valid) {
            // a host command: take the host's final destination (the story executor already queued its demos)
            const Where w = sCommanded.w;
            sCommanded.valid = false;
            const ScenarioData* d = galaxyAt(w.galaxy);
            if (d) {
                if (!a.isEqualStage(d->mGalaxyName) || (w.scenario && a.mScenarioNo != w.scenario)) {
                    mp_log("flow move override %s:%d -> %s:%d", a.mStageName ? a.mStageName : "-", a.mScenarioNo, d->mGalaxyName, w.scenario);
                }
                a.mStageName = d->mGalaxyName;
                a.mScenarioNo = w.scenario ? w.scenario : -1;
                a._C = w.selected ? w.selected : -1;
                if (w.startId >= 0) a.mIDInfo = JMapIdInfo(w.startId, w.zone == kUnset8 ? 0 : w.zone);
            }
            captureLeftScene();   // starfix: the scene being left, BEFORE the serial changes and the scene goes
            sSerial = w.serial;
            sSerialKnown = true;
            sCur = w;
            sCurValid = true;
        noteCur();
            setCoreSerial(sSerial);
            flushClaimRetriesToLate();   // starfix: claims still waiting for the star to appear are judged against the scene just left
            sBarrier.arm(sSerial, t);
            sLoadedSent = false;
            sLocalClear = false;
            sNoShareClear = false;
            sClearHold = false;
            waitBannerStep(false);
            if (sChoiceValid && sChoiceSerial != sSerial) sChoiceValid = false;
            mp_log("flow move type=%d galaxy=%s scen=%d serial=%d role=%s", (int)a.mMoveType, a.mStageName ? a.mStageName : "-",
                   (int)a.mScenarioNo, sSerial, roleName());
            MpRace::onMoveExecuted(sSerial, w.moveType, a.mStageName, w.galaxy, a.mScenarioNo, a._C);
            if (mp_is_host()) sendWhere(-1);   // a migrated host ran a leftover command: bring everyone along
            return;
        }
        const int type = (int)a.mMoveType;
        if (!mp_is_host()) {
            mp_log("flow own move type=%d galaxy=%s serial=%d role=guest", type, a.mStageName ? a.mStageName : "-", sSerial);
            return;
        }
        if (type == 5 || type == 7) {   // a reload after a miss is local; the title ends the session elsewhere
            mp_log("flow local move type=%d galaxy=%s serial=%d role=host", type, a.mStageName ? a.mStageName : "-", sSerial);
            return;
        }
        Where w;
        if (!whereFromArg(a, &w)) {
            mp_log("flow move unmirrored type=%d galaxy=%s (no scenario data)", type, a.mStageName ? a.mStageName : "-");
            return;
        }
        w.moveType = type == 1 ? (uint8_t)MOVE_DIRECT : (uint8_t)type;
        if (!moveTypeValid(w.moveType)) w.moveType = MOVE_DIRECT;
        // integ: the next serial follows the core's (mp_scene_serial: the last one any host announced; after a migration it can be
        // ahead of the last move this console executed), so a new host never reuses a serial the others have seen
        captureLeftScene();   // starfix: the scene the host leaves, BEFORE the serial moves on
        uint16_t base = sSerialKnown ? sSerial : (uint16_t)0;
        const uint16_t coreSerial = mp_scene_serial();
        if (serialNewer(coreSerial, base)) base = coreSerial;
        sSerial = (uint16_t)(base + 1);
        if (sSerial == 0) sSerial = 1;
        sSerialKnown = true;
        w.serial = sSerial;
        sCur = w;
        sCurValid = true;
        noteCur();
        setCoreSerial(sSerial);
        flushClaimRetriesToLate();   // starfix: claims still waiting for the star to appear are judged against the scene just left
        if (type == MOVE_AFTER_LOAD) {
            // integ: the host's file was just loaded (file select -> game): share its progress before everyone moves
            MpSaveGame::offerSnapshot();
        }
        if (type == MOVE_AFTER_CLEAR && GameSequenceFunction::hasStageResultSequence()) {
            MpSaveGame::offerSnapshot();   // the star is in the host's game data now: share the progress first
            MpSaveGame::writeRecoveryCheckpoint();   // MP3 integration: recovery save after every star
        }
        const bool raceStarSelect = mp_mode() == MP_MODE_RACE && w.moveType == MOVE_STAR_SELECT;
        if (!raceStarSelect) sendMove(w);   // Star Race: the scenario choice becomes a direct MOVE (onScenarioChosen)
        sBarrier.arm(sSerial, t);
        sLoadedSent = false;
        sLocalClear = false;
        sNoShareClear = false;
        sClearHold = false;
        waitBannerStep(false);
        sChoiceValid = false;
        mp_log("flow move type=%d galaxy=%s scen=%d serial=%d role=host", type, a.mStageName, (int)a.mScenarioNo, sSerial);
        if (!raceStarSelect) MpRace::onMoveExecuted(sSerial, w.moveType, a.mStageName, w.galaxy, a.mScenarioNo, a._C);
    }

    bool guestMayStartGalaxy() { return !MpGame::sessionHeld() || (mp_active() ? mp_is_host() : sLastIsHost); }

    void onStarGet(int starId, bool grand) {
        const bool active = ensureSession();
        if (!active && !sHeld) return;
        if (sDeliv.active && starId == sDeliv.starId) {
            mp_log("star delivered id=%d path=native", starId);
            sDeliv.active = false;
            sLocalClear = true;
            return;
        }
        const int localSlot = active ? mp_local_slot() : sHeldSlot;
        const bool isHost = active ? mp_is_host() : sLastIsHost;
        mp_log("flow star got id=%d grand=%d by=%d%s", starId, grand ? 1 : 0, localSlot, active ? "" : " (reconnecting)");
        if (MpRace::onStarGet(starId, grand)) return;
        sLocalClear = true;
        const OwnStar rule = ownStarRule(MpGame::heldMode() == MP_MODE_TOGETHER, starId, sSerialKnown);
        if (rule == OwnStar::Skip) return;
        if (rule == OwnStar::NoId) {
            // glitchfix G3: a star without a shareable id (the stage data has PowerStar placements with Obj_arg0 -1): nothing to send. A
            // guest does not wait for a host move that cannot come (it would sit out the whole hold timeout); it leaves on its own move.
            sNoShareClear = true;
            mp_log("flow star id=%d has no shareable id (1..%d): not sent; no clear-hold wait", starId, (int)kMaxStarId);
            return;
        }
        uint8_t g, sc;
        currentKey(&g, &sc);
        if (!sLedger.claim(g, sc, (uint8_t)starId, localSlot, sSerial, grand)) {
            mp_log("flow star dup id=%d by=%d", starId, localSlot);
            return;
        }
        sLastStarId = starId;
        sLastStarSerial = sSerial;
        MpTeam::onStar(localSlot, sSerial, starId);   // MP3 celebrate: credit our tally once (MpTeam defers tally send if reconnecting)
        Star st{sSerial, (uint8_t)starId, (uint8_t)(grand ? 1 : 0), (uint8_t)localSlot, MpGame::turn()};
        liveStarKey(&st.galaxyHash, &st.scenario);
        if (active) {
            uint8_t b[16];
            MpWriter w(b, sizeof b);
            writeStar(w, st);
            mp_send(MP_MSG_STAR, b, (uint16_t)w.n, true);
            if (isHost) sendStarv(st, localSlot, st.galaxyHash, st.scenario, SV_CREDITED);   // starfix (a): the host's own star is a verdict too
            else if (!trackOwnClaim(sStarPending, sLedger, st, now(), [](uint16_t h) { return galaxyIndexFromHash(h); }))   // a guest waits for the host's STARV
                mp_log("flow star id=%d: %d claims already wait for the host's verdict: this one is sent and answered, its ledger entry is taken back (no 60 s timeout to keep)", starId, (int)StarPending::kEntries);
        } else {
            // Reconnecting: record pending claim so ensureSession() flushes it immediately on reconnect.
            if (!trackOwnClaim(sStarPending, sLedger, st, now(), [](uint16_t h) { return galaxyIndexFromHash(h); }))
                mp_log("flow star id=%d: pending claim table full during reconnect", starId);
        }
    }

    static bool holdAfterClearStep();
    bool holdAfterClear() {
        const bool hold = holdAfterClearStep();
        // fix2: behind the closed star wipe the scene allows no pause: Start opens the session menu here (leave, stamps, a
        // vote), so a player held there (race results, the host's next move) is never stuck
        if (hold && port_input_take_pause_request() && !MpUi::isMenuOpen()) {
            mp_log("flow hold: session menu (Start)");
            MpUi::openSessionMenu();
        }
        return hold;
    }
    static bool holdAfterClearStep() {
        if (!ensureSession()) {
            waitBannerStep(false);
            if (!sHeld) return false;
            MpGameInternal::pumpSystem();
            if (!ensureSession()) return sHeld;
        }
        if (MpRace::isRaceScene()) return MpRace::holdAfterClear();
        MpTeam::onClearHold();   // MP3 celebrate: the star demo is over: the team star card (host too: it never waits)
        if (mp_is_host()) { waitBannerStep(false); sClearHold = false; return false; }   // (uxwait: this console became the host mid-hold)
        const int64_t t = now();
        if (!sClearHold) {
            sClearHold = true;
            sClearHoldMs = t;
            mp_log("flow clear hold serial=%d", sSerial);
        }
        const ClearHold plan = clearHoldPlan(sPending.valid, sNoShareClear, t - sClearHoldMs, kClearHoldTimeoutMs);
        waitBannerStep(plan == ClearHold::Hold);   // uxwait: held for the host's next move: after about 2 s the banner says who we wait for
        if (plan == ClearHold::ReleaseMove) {
            mp_log("flow clear hold release serial=%d next=%d", sSerial, sPending.w.serial);
            sClearHold = false;
            return false;
        }
        if (plan == ClearHold::ReleaseNoShare) {   // glitchfix G3
            mp_log("flow clear hold released at once serial=%d: our star had no shareable id, no host move to wait for", sSerial);
            sAllowOwnOnce = true;
            sClearHold = false;
            return false;
        }
        if (plan == ClearHold::Timeout) {
            mp_log("flow clear hold timeout serial=%d: going alone", sSerial);
            sAllowOwnOnce = true;
            sClearHold = false;
            return false;
        }
        return true;
    }

    bool blockGameOver() { return MpGame::sessionHeld(); }

    bool replacePause() { return mp_active() || MpUi::reconnecting(); }   // fix3: also while the core reconnects (session menu)

    bool skipVote(int contextKind) {
        if (!ensureSession()) return true;
        const bool query = contextKind < 0;
        const int kind = query ? -contextKind : contextKind;
        if (kind == 0 || kind == kSkipTalk) return !query;   // dialogue is local: no vote
        if (!sSerialKnown) return !query;
        const uint32_t hash = contextHash(kind);
        const unsigned mask = eligibleMask(kind);
        const int64_t t = now();
        if (!query && sVote.vote(mp_local_slot(), sSerial, hash, t)) {
            uint8_t b[6];
            MpWriter w(b, sizeof b);
            writeSkipVote(w, SkipVoteMsg{sSerial, hash});
            mp_send(MP_MSG_SKIPVOTE, b, (uint16_t)w.n, true);
            mp_log("flow skip vote %d/%d", sVote.votes(sSerial, hash, mask), popcount4(mask));
        }
        if (sVote.decide(sSerial, hash, mask, t, kSkipMajorityMs)) {
            mp_log("flow skip decided kind=%d votes=%d/%d", kind, sVote.votes(sSerial, hash, mask), popcount4(mask));
            return true;
        }
        return false;
    }
}  // namespace MpFlow

// ==== hooks beyond the contract (MpFlow.hpp) =========================================================================================
namespace MpFlowHooks {
    bool fileSelectMayAsk() {
        if (!mp_active() || mp_is_host() || mp_mode() != MP_MODE_TOGETHER) return true;
        if (!ensureSession() || sPending.valid || sAllowOwnOnce || sExecuting) return true;   // the host has moved (or we may go): ask at once
        static int64_t sLastAsk = -100000;
        const int64_t t = now();
        if (t - sLastAsk < 500) return false;
        sLastAsk = t;
        return true;
    }
    bool refuseGalaxyMove(const GalaxyMoveArgument& arg) {
        const bool active = ensureSession();
        const int type = (int)arg.mMoveType;
        if (!active) {
            if (sHeld && type != 5 && type != 7) {
                mp_log("flow move held during reconnect type=%d galaxy=%s", type, arg.mStageName ? arg.mStageName : "-");
                return true;
            }
            return false;
        }
        if (sExecuting && sPending.valid) {   // our own run of a host command (or a migrated host's leftover)
            sCommanded = sPending;
            sPending.valid = false;
            if (applySnapshotNow()) MpSaveGame::writeRecoveryCheckpoint();   // MP3 integration
            return false;
        }
        if (mp_is_host()) return false;
        if (type == 5 || type == 7) return false;   // reload after a miss (same serial) / title
        if (sPending.valid && (type == MOVE_BACK_TO_DOME || type == MOVE_AFTER_CLEAR || type == sPending.w.moveType)) {
            sCommanded = sPending;   // e.g. our own star-clear move or star-select cancel, released by the host's MOVE
            sPending.valid = false;
            if (applySnapshotNow()) MpSaveGame::writeRecoveryCheckpoint();   // MP3 integration
            mp_log("flow own move type=%d taken as command serial=%d", type, sCommanded.w.serial);
            return false;
        }
        if (sAllowOwnOnce) {
            sAllowOwnOnce = false;
            mp_log("flow own move type=%d allowed (host silent)", type);
            return false;
        }
        uint32_t repeats = 0;
        if (refusalEpisode(kRefuseMove, &repeats)) {   // the file select / a held button asks every frame: one refusal per episode
            mp_log("flow move refused type=%d galaxy=%s role=guest (x%u in the episode before)", type, arg.mStageName ? arg.mStageName : "-", repeats);
            refuseToast();
        }
        return true;
    }

    int scenarioSelectGuest(int* outSelected) {
        MpGameInternal::pumpSystem();   // no game scene updates during the star select
        sFlowClock.tick(now(), kFlowStepCapMs);
        if (!ensureSession()) return 0;
        epochCheck();
        if (mp_is_host() || mp_mode() != MP_MODE_TOGETHER) return 0;
        if (sPending.valid && sSerialKnown && serialNewer(sPending.w.serial, sSerial)) {
            mp_log("flow star select cancel (host moved on, serial=%d)", sPending.w.serial);
            return 3;
        }
        if (sBrowseSerial != sSerial && sSerialKnown && !sChoiceValid) {   // MP3 touches: told once that the host is picking the star
            sBrowseSerial = sSerial;
            MpUi::toast(MP_T_HOST_BROWSING_STAR, mp_host_slot(), 0);
        }
        if (sChoiceValid && sSerialKnown && sChoiceSerial == sSerial) {
            const ScenarioData* d = galaxyAt(sCur.galaxy);
            if (d && sChoiceSelected >= 1 && sChoiceSelected <= scenarioMax(d)) {
                *outSelected = sChoiceSelected;
                sChoiceValid = false;
                if (sPickSerial != sSerial) {   // MP3 touches: the host's star choice, as a card
                    sPickSerial = sSerial;
                    MpUi::announcePick(mp_host_slot(), d->mGalaxyName, sChoiceSelected);
                }
                mp_log("flow star select apply sel=%d serial=%d", *outSelected, sSerial);
                return 2;
            }
            sChoiceValid = false;
        }
        return 1;
    }

    void onScenarioChosen(int placedNo, int selectedNo) {
        if (!ensureSession()) return;
        if (!mp_is_host()) {
            mp_log("flow star select chosen placed=%d sel=%d role=guest", placedNo, selectedNo);
            return;
        }
        const int idx = galaxyIndexOf(MR::getCurrentStageName());
        if (idx < 0 || placedNo < 1 || placedNo > 255 || !sSerialKnown) return;
        Where w;
        w.galaxy = (uint8_t)idx;
        w.scenario = (uint8_t)placedNo;
        w.selected = (selectedNo >= 1 && selectedNo <= 255) ? (uint8_t)selectedNo : 0;
        w.startId = -1;
        w.zone = kUnset8;
        w.serial = sSerial;
        w.moveType = MOVE_DIRECT;
        sCur = w;
        sCurValid = true;
        noteCur();
        mp_log("flow star select chosen placed=%d sel=%d serial=%d role=host", placedNo, selectedNo, sSerial);
        if (mp_mode() == MP_MODE_RACE) {
            sendMove(w);   // Star Race: everyone goes straight to the scenario
            MpRace::onMoveExecuted(sSerial, MOVE_DIRECT, MR::getCurrentStageName(), w.galaxy, placedNo, selectedNo);
        } else {
            sendWhere(-1);
        }
    }

    bool refuseGuestGalaxyStart() {
        if (MpFlow::guestMayStartGalaxy()) return false;
        uint32_t repeats = 0;
        if (refusalEpisode(kRefuseGalaxyStart, &repeats)) {
            mp_log("flow guest galaxy start refused (x%u in the episode before)", repeats);
            refuseToast();
        }
        return true;
    }

    bool keepLives() {
        if (MpBubble::bypassKeepLives()) return false;   // bubble: a fall into a bubble costs a life (the real decrement)
        const bool keep = MpGame::heldMode() == MP_MODE_TOGETHER;
        if (keep && !sLivesClampLogged) {
            sLivesClampLogged = true;
            mp_log("flow lives clamp");
        }
        return keep;
    }

    void requestWarpTo(int slot) {
        if (!mp_active() || slot < 0 || slot >= MP_MAX_PLAYERS || slot == mp_local_slot() || !mp_slot_present(slot)) return;
        if (MpBubble::localBubbled()) {   // bubble: the bubble floats to the friends already
            mp_log("flow warp request to=%d ignored (in a bubble)", slot);
            return;
        }
        sWarpTarget = slot;
        const bool dead = MR::isExistMario() && MR::isPlayerDead();
        sWarpDeadlineMs = now() + (dead ? 30000 : 10000);
        mp_log("flow warp request to=%d", slot);
    }

    bool guestMayStartGhostRace() {
        if (MpGame::heldMode() != MP_MODE_TOGETHER || (mp_active() ? mp_is_host() : sLastIsHost)) return true;
        refuseToast();
        return false;
    }
}  // namespace MpFlowHooks

namespace MpFlowRace {
    // race: the host that did not finish (it is in the scene, the results card up) starts the next race for everyone: a direct move of
    // this galaxy; onGalaxyMove sends the MOVE. False = no session, not the host, or no such galaxy.
    bool hostDirectMove(const char* stage, int scenario, int selected) {
        if (!mp_active() || !mp_is_host() || !stage || scenario < 1) return false;
        const int idx = galaxyIndexOf(stage);
        if (idx < 0) return false;
        GalaxyMoveArgument arg(MOVE_DIRECT, stage, scenario, nullptr);
        arg._C = selected >= 1 ? selected : -1;
        GameSequenceFunction::requestGalaxyMove(arg);
        return true;
    }

    // race: a guest behind the star wipe (it finished) holds the host's direct MOVE (or a WHERE pointing to a non-hub race stage) for the
    // next race: it runs it from here, as a direct move (the star-clear move the sequence would make next is a story move: it queues the
    // dome's demos, which crash in a race scene). False = no direct race move is waiting (the usual clear move takes whatever is).
    bool runPendingDirect() {
        if (!mp_active() || mp_is_host() || sExecuting || !sPending.valid) return false;
        const ScenarioData* d = galaxyAt(sPending.w.galaxy);
        const bool directMove = sPending.isMove && sPending.w.moveType == MOVE_DIRECT;
        const bool raceWhere = !sPending.isMove && sPending.w.scenario >= 1 && d && !MpRace::isHubStage(d->mGalaxyName);
        if (!directMove && !raceWhere) return false;
        if (!sSnapshotApplied && applySnapshotNow()) sSnapshotApplied = true;
        const Where w = sPending.w;
        mp_log("flow run command (behind the star wipe) type=0 serial=%d%s", w.serial, raceWhere ? " (where)" : "");
        if (sPickSerial != w.serial) {
            sPickSerial = w.serial;
            MpUi::announcePick(mp_host_slot(), d ? d->mGalaxyName : nullptr, w.selected >= 1 ? w.selected : w.scenario);
        }
        runMove(MOVE_DIRECT, w);
        return true;
    }
}  // namespace MpFlowRace
