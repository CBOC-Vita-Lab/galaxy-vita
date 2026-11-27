// Galaxy 3.0 multiplayer: the team Star Bit count, game side (owner: costar). Logic and wire: MpStarBitsLogic.hpp (DELTA / TOTAL in TALLY).
//
// The count is the game's own (MR::getStarPieceNum: HUD counter, Hungry Lumas, 1-Up at each 50, reflected into the file at the end of
// the stage), kept equal on every console:
//   - a local pick-up / spend goes through the game's MR::addStarPiece as always; MpStarBits::onAdd (a hook at its top) only tells the team,
//   - the host adds the guests' DELTAs to its total and broadcasts TOTAL; each turn this module brings the game's count to the team's
//     number with the game's own MR::addStarPiece (in steps that stop at each multiple of 50, so the 1-Up rule sees each crossing),
//   - the 1-Up at each 50 goes to everyone when the host's falls rule is Bubbles (lives count); with Infinite lives it is ignored.
// Desktop test hook (compiled out of the Vita build): SMG_MP_TEST_STARBITS="<secs>:<n>[:<slot>]" makes this console's player pick up n Star Bits
// (the game's own MR::gotByPlayer, one every 20 turns) starting <secs> after it got control in a scene (only on <slot> when given).
// Saving is the game's: the host's count is the file's count the usual way, a guest never saves (mp_save_suppressed); no write path here.
#include "Game/MP/MpStarBits.hpp"

#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpStarBitsLogic.hpp"
#include "Game/MapObj/StarPieceDirector.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <stdlib.h>
#include <string.h>

using namespace MpStarBitsLogic;

namespace {
    Host sHost;
    Guest sGuest;
    bool sInit = false;
    bool sHostEver = false;         // sHost holds a state of this session (its serial / epoch / acks / rev outlive an invalid scene)
    uint16_t sHostRev = 0;          // the last revision this host sent: the next scene's first TOTAL continues from it
    uint32_t sDropLogs = 0;         // RATE_LIMITED / other-scene deltas logged (a flood is not logged line by line)
    bool sWaiting = false;          // the serial changed while the OLD scene still exists: no count is read, sent or shown until the new scene is up
    uint32_t sWaitTurn = 0;
    bool sIsHost = false;
    uint16_t sSerial = 0;
    uint32_t sTurn = 0;
    unsigned sOwn = 0;
    bool sApplying = false;
    bool sSilent = false;           // a catch-up jump: no 1-Up for it
    unsigned sPresent = 0;
    int sLogBudget = 0;
    uint32_t sDeltasSent = 0, sTotalsSent = 0, sTotalsGot = 0, sDeltasGot = 0;
    int sSeenCount = -1, sLoggedCount = -1;   // the game's count last turn / last logged as settled
    uint32_t sSeenTurn = 0;

    bool together() { return mp_active() && mp_mode() == MP_MODE_TOGETHER; }
    int capNow() { return MR::isStageAstroLocation() ? kCapStock : kCapGalaxy; }
    void logf2(const char* what, int a, int b) {
        if (sLogBudget <= 0) return;
        sLogBudget--;
        mp_log("starbits %s %d %d", what, a, b);
    }

    void initFor(uint16_t serial, bool sceneReady) {
        sInit = true;
        sSerial = serial;
        sIsHost = mp_is_host();
        sOwn = 0;
        sSeenCount = sLoggedCount = -1;
        sSilent = false;
        sLogBudget = 60;
        sDeltasSent = sTotalsSent = sTotalsGot = sDeltasGot = 0;
        sPresent = MpGameInternal::presentMask();
        if (sIsHost) {
            // the host starts from the game's own count, which needs the scene (in the Observatory it is the file's stock)
            if (!sceneReady) {
                sHost.valid = false;
                return;
            }
            if (sHostEver && sHost.serial == serial && sHost.epoch == (uint8_t)mp_host_epoch()) {
                sHost.reinit(capNow(), MR::getStarPieceNum(), sTurn);   // same scene again: acks stay, the revision goes on
            } else {
                sHost.reset(serial, (uint8_t)mp_host_epoch(), capNow(), MR::getStarPieceNum(), sTurn, (uint16_t)(sHostRev + 1));
            }
            sHostEver = true;
            sGuest.valid = false;
            mp_log("starbits init serial=%u role=host cap=%d start=%d", (unsigned)serial, capNow(), MR::getStarPieceNum());
        } else {
            sGuest.reset(serial, sceneReady ? capNow() : kCapGalaxy, sTurn);
            sHost.valid = false;
            mp_log("starbits init serial=%u role=guest cap=%d", (unsigned)serial, sGuest.cap);
        }
    }

    // The state of this scene: made when the scene's serial changes (a host needs the scene to read the game's count), moved over when
    // this console becomes the host.
    void ensure(bool sceneReady) {
        const uint16_t serial = mp_scene_serial();
        const bool wasInit = sInit;
        const bool wasWaiting = sWaiting;
        const GateAct act = gateStep(sInit, sWaiting, sSerial, sWaitTurn, serial, sTurn);
        if (!sInit) {
            if (wasInit && !wasWaiting) { sHost.valid = false; sGuest.valid = false; }   // the serial just changed under the old scene
            return;
        }
        if (act == GATE_START) {
            initFor(serial, sceneReady);
        } else if (sIsHost != mp_is_host()) {
            if (mp_is_host() && sGuest.valid) {   // this console became the host: what it knew, plus its own unacknowledged pick-ups
                sGuest.becomeHost(&sHost, mp_local_slot(), (uint8_t)mp_host_epoch(), sTurn);
                sIsHost = true;
                sGuest.valid = false;
                mp_log("starbits migrated: now host epoch=%u total=%d acks=%u,%u,%u,%u", (unsigned)sHost.epoch, sHost.total, (unsigned)sHost.acks[0],
                       (unsigned)sHost.acks[1], (unsigned)sHost.acks[2], (unsigned)sHost.acks[3]);
            } else {
                initFor(serial, sceneReady);
            }
        } else if (sIsHost && !sHost.valid && sceneReady) {
            initFor(serial, true);
        }
    }

    void sendTotal(const Total& t) {
        uint8_t b[24];
        MpWriter w(b, sizeof b);
        writeTotal(w, t);
        if (w.ok() && mp_send(MP_MSG_TALLY, b, (uint16_t)w.n, true)) {
            if (++sTotalsSent <= 3 || (sTotalsSent % 64) == 0) logf2("total sent rev/total", (int)t.rev, (int)t.total);
        }
    }
    void sendDelta(const Delta& d) {
        uint8_t b[16];
        MpWriter w(b, sizeof b);
        writeDelta(w, d);
        if (w.ok() && mp_send(MP_MSG_TALLY, b, (uint16_t)w.n, true)) {
            if (++sDeltasSent <= 4 || (sDeltasSent % 64) == 0) logf2("delta sent seq/delta", (int)d.seq, (int)d.delta);
        }
    }

    // Bring the game's count to the team's number: one step a turn.
    void reconcile() {
        int want;
        if (sIsHost) {
            if (!sHost.valid) return;
            want = sHost.total;
        } else {
            if (!sGuest.known) return;
            want = sGuest.view();
        }
        const int cur = MR::getStarPieceNum();
        const int step = nextStep(cur, want);
        if (step == 0) { sSilent = false; return; }
        const int gap = want > cur ? want - cur : cur - want;
        sSilent = gap > 20;   // a joiner's catch-up or a big correction is not a 1-Up for the 50s it passes
        if (sApplying) return;
        sApplying = true;
        logf2("apply cur/step", cur, step);
        MR::addStarPiece(step);
        sApplying = false;
    }

#if defined(TARGET_PC) && !defined(__vita__)
    int sTestSecs = -2, sTestN = 0, sTestSlot = -1;
    uint16_t sTestSerial = 0;
    bool sTestSerialSet = false;
    uint32_t sTestStart = 0;
    bool sTestStarted = false;
    int sTestDone = 0;
    void testStep() {
        if (sTestSecs == -2) {
            const char* e = getenv("SMG_MP_TEST_STARBITS");
            sTestSecs = (e && *e) ? atoi(e) : -1;
            if (sTestSecs >= 0) {
                const char* c = strchr(e, ':');
                sTestN = c ? atoi(c + 1) : 0;
                const char* c2 = c ? strchr(c + 1, ':') : nullptr;
                sTestSlot = c2 ? atoi(c2 + 1) : -1;
                mp_log("starbits test: %d pick-ups %d s after control (slot %d)", sTestN, sTestSecs, sTestSlot);
            }
        }
        if (sTestSecs < 0 || (sTestSlot >= 0 && sTestSlot != mp_local_slot())) return;
        if (!sTestSerialSet || sTestSerial != sSerial) {
            sTestSerialSet = true; sTestSerial = sSerial; sTestStarted = false; sTestDone = 0;
        }
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isDemoActive()) return;
        if (!sTestStarted) { sTestStarted = true; sTestStart = sTurn; }
        if (sTurn - sTestStart < (uint32_t)sTestSecs * 60u || sTestDone >= sTestN || (sTurn % 20) != 0) return;
        sTestDone++;
        MR::gotByPlayer();
        if (sTestDone == sTestN) mp_log("starbits test: %d picked up, count now %d", sTestN, MR::getStarPieceNum());
    }
#endif
}  // namespace

namespace MpStarBits {
    bool teamActive() { return sInit && together(); }
    unsigned ownCollected() { return sOwn; }
    int teamCount() { return teamActive() && (sIsHost ? sHost.valid : sGuest.known) ? MR::getStarPieceNum() : -1; }

    void update() {
        if (!together()) {
            if (MpGame::sessionHeld() && MpGame::heldMode() == MP_MODE_TOGETHER) return;   // preserve sInit / sGuest across silent reconnect
            sInit = false;
            sWaiting = false;
            sHostEver = false;
            sHostRev = 0;
            return;
        }
        sTurn++;
        const bool scene = MpGameInternal::sceneExists();
        if (!scene && !sInit) return;
        ensure(scene);
        if (!sInit) return;
#if defined(TARGET_PC) && !defined(__vita__)
        if (scene) testStep();
#endif
        if (sIsHost && scene) {
            sHost.cap = capNow();
            const unsigned pm = MpGameInternal::presentMask();
            if (pm != sPresent) {
                if (pm & ~sPresent) sHost.noteJoin();   // someone joined: they get the number at once
                sPresent = pm;
            }
            Total t;
            if (sHost.poll(sTurn, &t)) sendTotal(t);
            sHostRev = sHost.rev;
        } else if (!sIsHost) {
            if (scene) sGuest.cap = capNow();
            Delta d;
            if (sGuest.poll(sTurn, &d)) sendDelta(d);
        }
        if (scene && MpGameInternal::sceneState() != MP_SCENE_OTHER && MpGameInternal::sceneState() != MP_SCENE_PAUSE) reconcile();
        if (scene) {   // one line when the count has stayed put for 1.5 s (the tests compare the consoles' settled numbers)
            const int cur = MR::getStarPieceNum();
            if (cur != sSeenCount) { sSeenCount = cur; sSeenTurn = sTurn; }
            else if (cur != sLoggedCount && sTurn - sSeenTurn >= 90 && (sIsHost ? sHost.valid : sGuest.known)) {
                sLoggedCount = cur;
                mp_log("starbits settled count=%d team=%d own=%u role=%s", cur, sIsHost ? sHost.total : sGuest.view(), sOwn, sIsHost ? "host" : "guest");
            }
        }
    }

    bool handle(const MpMsg& m) {
        const uint8_t kind = classify(m.data, m.len);
        if (kind == 0) return false;   // the old tally
        if (!together() || m.fromSlot >= MP_MAX_PLAYERS || m.fromSlot == mp_local_slot()) return true;
        if (kind == 0xFF) {
            static uint32_t sBad = 0;
            if (sBad++ < 8) mp_log("starbits drop malformed TALLY from=%d len=%u", (int)m.fromSlot, (unsigned)m.len);
            return true;
        }
        if (!sInit) ensure(MpGameInternal::sceneExists());
        if (kind == kKindDelta) {
            Delta d;
            if (!mp_is_host() || !sIsHost || !readDelta(m.data, m.len, &d) || !mp_slot_present(m.fromSlot)) return true;   // (relayed to a guest: not for it)
            const Host::Result r = sHost.applyDelta(m.fromSlot, d, sTurn);
            const bool drop = r == Host::RATE_LIMITED || r == Host::REJECTED_SCENE;
            if (drop) sDropLogs++;
            if (++sDeltasGot <= 4 || (drop && (sDropLogs <= 8 || (sDropLogs & 255) == 0))) {
                static const char* const kName[] = {"applied", "duplicate", "other-scene", "bad-slot", "RATE-LIMITED"};
                mp_log("starbits delta got from=%d seq=%u d=%d -> %s total=%d", (int)m.fromSlot, (unsigned)d.seq, (int)d.delta, kName[r], sHost.total);
            }
        } else {
            Total t;
            if (mp_is_host() || sIsHost || m.fromSlot != mp_host_slot() || !readTotal(m.data, m.len, &t)) return true;
            const bool was = sGuest.known;
            if (sGuest.onTotal(t, mp_local_slot(), sTurn)) {
                if (!was || ++sTotalsGot <= 2) mp_log("starbits total got rev=%u total=%d epoch=%u first=%d", (unsigned)t.rev, (int)t.total, (unsigned)t.epoch, was ? 0 : 1);
            }
        }
        return true;
    }

    bool onAdd(int num) {
        if (sApplying || num == 0 || !together() || !MpGameInternal::sceneExists()) return false;
        ensure(true);
        if (!sInit) return false;
        const int cur = MR::getStarPieceNum();
        const int cap = capNow();
        int eff = cur + num;
        eff = eff < 0 ? 0 : eff > cap ? cap : eff;
        eff -= cur;
        if (eff == 0) return false;
        if (eff > 0) sOwn += (unsigned)eff;
        if (sIsHost) sHost.applyLocal(eff, mp_local_slot());
        else sGuest.localAdd(eff);
        return false;
    }

    bool oneUpAllowed() {
        if (!together()) return true;
        if (mp_settings()->downed != MP_DOWNED_BUBBLES) return false;   // Infinite lives: nobody has lives to give
        return !sSilent;
    }

    void sceneEnd() {
        sInit = false;
        sWaiting = false;
        sApplying = sSilent = false;
        sOwn = 0;
    }
}  // namespace MpStarBits
