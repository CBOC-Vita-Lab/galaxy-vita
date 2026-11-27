// Galaxy 3.0 multiplayer: Co-Star help from every player's Star Pointer, game side (owner: costar). Rules: MpCoStarLogic.hpp.
//
// The original's second player holds an enemy by pointing at it with A pressed. The enemy's own code asks
// MR::isStarPointerPointing2POnPressButton; when the real second pad says no, that function now asks MpCoStar::holds, which answers
// from the players' held pointers (MpCoop::pointerPoint: POINTER 0x24, which is only sent while the sender's pointer button is held;
// the local player's own pointer counts too, so everybody sees the same enemy freeze). The enemy then runs its own bind state
// (WalkerStateBindStarPointer and the others): the original's hit sound, freeze sound, touch effect and stopped animation.
// Star Bits near another player's held pointer fly to the local player (StarPiece::tryGotJudge asks MpCoStar::sweeps), as the second
// player's pointer collected them for Mario; they are counted like any Star Bit (a bit can then be counted by two players on their own copies: the count is bounded by the sweep gate, and no bit is ever destroyed).
// Never fed to: switches, NPCs, menus, bosses (the hook is in the enemies' hold question only; the actor must be a non-boss enemy).
//
// Desktop test hook: SMG_MP_TEST_COSTAR="<secs>:<dur>[:<x>,<y>,<z>]" makes THIS console's pointer ping (B held) from <secs> after each
// scene start for <dur> seconds at the given world point, or on the nearest enemy to the player when no point is given.
#include "Game/MP/MpCoStar.hpp"

#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/MP/MpCoStarLogic.hpp"
#include "Game/MP/MpCoop.hpp"
#include "Game/MP/MpStarBits.hpp"
#include "Game/MP/MpWorldLogic.hpp"
#include "Game/MapObj/StarPiece.hpp"
#include "Game/Screen/StarPointerTarget.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <math.h>
#include <stdlib.h>

using namespace MpCoStarLogic;

namespace {
    bool sLive = false;
    HoldGate sHold[MP_MAX_PLAYERS];
    SweepGate sSweep[MP_MAX_PLAYERS];
    bool sOn[MP_MAX_PLAYERS];          // this slot's pointer is held, fresh and sane (cached once a turn)
    float sPt[MP_MAX_PLAYERS][3];
    bool sPrevHeld[MP_MAX_PLAYERS];
    uint32_t sHeldSince[MP_MAX_PLAYERS];
    const LiveActor* sHeldActor[MP_MAX_PLAYERS];   // (scene objects: cleared with the scene)
    float sHeldFrom[MP_MAX_PLAYERS][3];
    uint32_t sHoldsStarted[MP_MAX_PLAYERS];
    uint32_t sSweeps[MP_MAX_PLAYERS];
    uint32_t sRefused[MP_MAX_PLAYERS];  // sane-point refusals logged (rude pointers)
    uint32_t sTurn = 0;

    bool together() { return mp_active() && mp_mode() == MP_MODE_TOGETHER; }

    // The sender's own position: a remote puppet's last pose, or the local player.
    bool senderPos(int slot, float out[3]) {
        if (slot == mp_local_slot()) {
            const TVec3f* p = MR::getPlayerPos();
            if (p == nullptr) return false;
            out[0] = p->x; out[1] = p->y; out[2] = p->z;
            return true;
        }
        MpPose pose;
        if (!MpPuppets::lastPose(slot, &pose)) return false;
        for (int i = 0; i < 3; i++) out[i] = pose.pos[i];
        return true;
    }

    // An enemy the original's hold code may be asked about: a hit sensor in the enemy range, not a boss.
    bool holdable(const LiveActor* a) {
        if (a->mSensorKeeper == nullptr || MR::isDead(a)) return false;
        HitSensor* body = a->getSensor("body");
        if (body == nullptr || !MR::isSensorEnemy(body)) return false;
        return !MpWorldLogic::isBossName(a->mName);   // (second line only: bosses do not ask this question, see isStarPointerPointing2POnPressButtonNoMp)
    }

    uint32_t idOf(const LiveActor* a) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(a);
        const uint32_t h = (uint32_t)(v ^ (v >> 32)) | 1u;
        return h;
    }

    // ---- desktop test hook: the nearest enemy that asks the hold question (compiled out of the Vita build) ----------------------------------
#if defined(TARGET_PC) && !defined(__vita__)
    int sTestOn = -1;
    float sCandPos[3];
    float sCandDist = 0.0f;
    uint32_t sCandTurn = 0;
    bool sCandValid = false;
    float sBitPos[3];
    float sBitDist = 0.0f;
    uint32_t sBitTurn = 0;
    bool sBitValid = false;
    void noteStarBit(const TVec3f& at) {   // (the nearest Star Bit that asks to be got; SMG_MP_TEST_POINTER_STARBIT in MpCoop.cpp aims at it)
        const TVec3f* me = MR::getPlayerPos();
        if (me == nullptr) return;
        const float dx = at.x - me->x, dy = at.y - me->y, dz = at.z - me->z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (!sBitValid || sTurn - sBitTurn > 20 || d < sBitDist) {
            sBitValid = true;
            sBitDist = d;
            sBitPos[0] = at.x; sBitPos[1] = at.y; sBitPos[2] = at.z;
        }
        sBitTurn = sTurn;
    }
    void noteCandidate(const LiveActor* a, const TVec3f& tp) {
        if (sTestOn < 0) {
            const char* e = getenv("SMG_MP_TEST_POINTER_ENEMY");
            sTestOn = (e && *e) ? 1 : 0;
        }
        if (!sTestOn) return;
        const TVec3f* me = MR::getPlayerPos();
        if (me == nullptr || !holdable(a)) return;
        const float dx = tp.x - me->x, dy = tp.y - me->y, dz = tp.z - me->z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (!sCandValid || sTurn - sCandTurn > 20 || d < sCandDist) {
            sCandValid = true;
            sCandDist = d;
            sCandPos[0] = tp.x; sCandPos[1] = tp.y; sCandPos[2] = tp.z;
        }
        sCandTurn = sTurn;
    }
#endif
}  // namespace

namespace MpCoStar {
    void initScene() {
        sLive = MpGame::sessionHeld();
        const uint32_t t = MpGame::turn();
        sTurn = t;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            sHold[s].reset(t);
            sSweep[s].reset(t);
            sOn[s] = false;
            sPrevHeld[s] = false;
            sHeldActor[s] = nullptr;
            sHoldsStarted[s] = sSweeps[s] = sRefused[s] = 0;
        }
    }

    void endScene() {
        sLive = false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            sOn[s] = false;
            sPrevHeld[s] = false;
            sHeldActor[s] = nullptr;
        }
    }

    void update() {
        if (!sLive || !together()) {
            return;
        }
        sTurn = MpGame::turn();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            float p[3], ref[3];
            bool on = MpCoop::pointerPoint(s, p);
            if (on) {
                if (!senderPos(s, ref) || !pointUsable(p, ref)) {
                    if (sRefused[s]++ < 3) {
                        mp_log("costar pointer refused slot=%d (out of range or bad) p=%.0f,%.0f,%.0f", s, p[0], p[1], p[2]);
                    }
                    on = false;
                }
            }
            sOn[s] = on;
            if (on) {
                for (int i = 0; i < 3; i++) sPt[s][i] = p[i];
            } else {
                sHold[s].release();
            }
            const bool held = sHold[s].holding(sTurn);
            if (sPrevHeld[s] && !held) {
                float moved = -1.0f;   // how far the held enemy went while it was held (a frozen one: ~0)
                if (sHeldActor[s] != nullptr) {
                    const float dx = sHeldActor[s]->mPosition.x - sHeldFrom[s][0], dy = sHeldActor[s]->mPosition.y - sHeldFrom[s][1],
                                dz = sHeldActor[s]->mPosition.z - sHeldFrom[s][2];
                    moved = sqrtf(dx * dx + dy * dy + dz * dz);
                }
                mp_log("costar hold end slot=%d turns=%u moved=%.1f", s, (unsigned)(sTurn - sHeldSince[s]), moved);
                sHeldActor[s] = nullptr;
            }
            sPrevHeld[s] = held;
        }
    }

    bool holds(const LiveActor* a) {
        if (!sLive || a == nullptr || !together()) {
            return false;
        }
        const StarPointerTarget* tg = a->mStarPointerTarget;
        if (tg == nullptr) {
            return false;
        }
        bool any = false, checked = false, ok = false;
        TVec3f tp;
#if defined(TARGET_PC) && !defined(__vita__)
        if (sTestOn != 0) {
            a->mStarPointerTarget->calcPosition(&tp);
            noteCandidate(a, tp);
        }
#endif
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (!sOn[s] && !sHold[s].holding(sTurn)) {
                continue;
            }
            if (!checked) {   // (an enemy that is asked while no pointer is held costs one flag test)
                checked = true;
                ok = holdable(a);
                if (ok) tg->calcPosition(&tp);
            }
            if (!ok) {
                return false;
            }
            const float tpos[3] = {tp.x, tp.y, tp.z};
            const bool near = sOn[s] && inReach(sPt[s], tpos, tg->mRadius3d);
            if (sHold[s].query(sTurn, idOf(a), sOn[s], near)) {
                if (!sPrevHeld[s]) {
                    sPrevHeld[s] = true;
                    sHeldSince[s] = sTurn;
                    sHeldActor[s] = a;
                    sHeldFrom[s][0] = a->mPosition.x; sHeldFrom[s][1] = a->mPosition.y; sHeldFrom[s][2] = a->mPosition.z;
                    if (sHoldsStarted[s]++ < 8) {
                        // (the placement names are Shift-JIS: the log shows their 15-bit hash)
                        mp_log("costar hold start slot=%d actor=%04x at=%.0f,%.0f,%.0f", s, a->mName ? mp_name_hash15(a->mName) : 0,
                               a->mPosition.x, a->mPosition.y, a->mPosition.z);
                    }
                }
                any = true;
            }
        }
        return any;
    }

    bool testNearestEnemy(float out[3]) {
#if defined(TARGET_PC) && !defined(__vita__)
        if (!sCandValid || sTurn - sCandTurn > 30) return false;
        for (int i = 0; i < 3; i++) out[i] = sCandPos[i];
        return true;
#else
        (void)out;
        return false;
#endif
    }

    bool testNearestStarBit(float out[3]) {
#if defined(TARGET_PC) && !defined(__vita__)
        if (!sBitValid || sTurn - sBitTurn > 30) return false;
        for (int i = 0; i < 3; i++) out[i] = sBitPos[i];
        return true;
#else
        (void)out;
        return false;
#endif
    }

    bool sweeps(StarPiece* piece) {
        if (!sLive || piece == nullptr || !together()) {
            return false;
        }
#if defined(TARGET_PC) && !defined(__vita__)
        if (sTestOn != 0) noteStarBit(piece->mPosition);
#endif
        const int me = mp_local_slot();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == me || !sOn[s]) {
                continue;
            }
            const float dx = sPt[s][0] - piece->mPosition.x, dy = sPt[s][1] - piece->mPosition.y, dz = sPt[s][2] - piece->mPosition.z;
            if (dx * dx + dy * dy + dz * dz > kSweepReach * kSweepReach) {
                continue;
            }
            if (!sSweep[s].take(sTurn)) {
                continue;
            }
            piece->mMpRemoteSweep = true;
            if (sSweeps[s]++ < 6) {
                mp_log("costar sweep slot=%d starbit", s);
            }
            return true;
        }
        return false;
    }
}  // namespace MpCoStar
