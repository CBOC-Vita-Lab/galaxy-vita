// Galaxy 3.0 multiplayer: reads the local player each send turn into an MpPoseF (owner: puppet). Read-only: never changes Mario.
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/MP/MpAway.hpp"   // MP3 touches: the away bit
#include "Game/MP/MpBubble.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/System/ResourceInfo.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <revolution/mtx.h>
#include <cstdlib>
#include <cstring>

extern "C" int64_t port_host_time_ns(void);

namespace {
    // per-track bck resource -> name hash, looked up again only when the resource pointer changes
    const void* sTrackRes[4];
    uint32_t sTrackHash[4];
    bool sLoggedFirst;

    uint8_t formOf(const MarioActor* a) {
        if (a->mCurrModel == 1) {
            return MP_FORM_BONE;
        }
        switch (a->mPlayerMode) {
        case 1: return MP_FORM_RAINBOW;
        case 2: return MP_FORM_FIRE;
        case 3: return MP_FORM_ICE;
        case 4: return MP_FORM_BEE;
        case 5: return MP_FORM_SPRING;
        case 6: return MP_FORM_BOO;
        case 7: return MP_FORM_FLYING;
        default: return MP_FORM_NORMAL;
        }
    }

#if defined(TARGET_PC) && !defined(__vita__)
    // Desktop test hook SMG_MP_TEST_FORM="<secs>:<form>[,<secs>:<form>...]" (secs since the scene's first capture, ascending): the
    // pose we SEND carries that form from then on (the local player is not changed: this console's scene has none of the form's
    // models, as a watcher's would not). Forms 0..8 (MpForm; the encoder clamps anything else to normal).
    struct TestForm {
        double at;
        int form;
    };
    TestForm sTestForm[16];
    int sTestFormCount = -1;
    int64_t sTestFormStartMs;
    int testForm(int actual) {
        if (sTestFormCount < 0) {
            sTestFormCount = 0;
            const char* e = getenv("SMG_MP_TEST_FORM");
            for (const char* p = e; p != nullptr && *p && sTestFormCount < 16;) {
                TestForm t = {atof(p), 0};
                const char* c = strchr(p, ':');
                if (c == nullptr) break;
                t.form = atoi(c + 1);
                sTestForm[sTestFormCount++] = t;
                mp_log("puppet test form: from %.1f s after the first capture of a scene: form %d", t.at, t.form);
                p = strchr(c, ',');
                if (p != nullptr) p++;
            }
        }
        if (sTestFormCount == 0) return actual;
        if (sTestFormStartMs == 0) sTestFormStartMs = port_host_time_ns() / 1000000;
        const double now = (double)(port_host_time_ns() / 1000000 - sTestFormStartMs) / 1000.0;
        int f = actual;
        for (int i = 0; i < sTestFormCount; i++) {
            if (now >= sTestForm[i].at) f = sTestForm[i].form;
        }
        return f;
    }
#else
    inline int testForm(int actual) { return actual; }
#endif

    uint32_t groupHash(const XanimePlayer* p, bool* pSimple) {
        *pSimple = false;
        if (p == nullptr || p->mCurrentAnimation == nullptr) {
            return 0;
        }
        if (p->isAnimationRunSimple()) {
            *pSimple = true;
            return MpPoseMath::animHash(p->getCurrentBckName());
        }
        return p->mCurrentAnimation->mHash;
    }
}  // namespace

namespace MpLocalCapture {
    void resetScene() {
        for (int i = 0; i < 4; i++) {
            sTrackRes[i] = nullptr;
            sTrackHash[i] = 0;
        }
        sLoggedFirst = false;
#if defined(TARGET_PC) && !defined(__vita__)
        sTestFormStartMs = 0;
#endif
    }

    bool capture(MpPoseF* p, uint32_t turn) {
        MarioActor* a = MpPuppetsInt::localActor();
        if (a == nullptr || a->mMarioAnim == nullptr || a->mMarioAnim->mXanimePlayer == nullptr) {
            return false;
        }
        J3DModelX* model = a->getJ3DModel();
        if (model == nullptr) {
            return false;
        }
        memset(p, 0, sizeof *p);
        p->turn = turn;
        p->sceneSerial = mp_scene_serial();
        p->zone = 0xFF;   // positions are global in a scene (all zones are placed in one world); receivers ignore it
        p->form = mp_form_clamp((uint32_t)testForm(formOf(a)));   // (the test hook is the identity without SMG_MP_TEST_FORM)

        XanimePlayer* lower = a->mMarioAnim->mXanimePlayer;
        XanimeCore* core = lower->getCore();
        XjointTransform* joint0 = core != nullptr ? core->getJointTransform(0) : nullptr;

        MtxPtr base = model->getBaseTRMtx();
        Mtx poseMtx;
        const f32 (*rotSrc)[4] = reinterpret_cast< const f32(*)[4] >(&base[0][0]);
        f32 posX = base[0][3];
        f32 posY = base[1][3];
        f32 posZ = base[2][3];
        if (joint0 != nullptr && joint0->_64 != nullptr) {
            // MarioActor::calcAndSetBaseMtx stores flight/correction rotation (_E3C) on joint0->_64.
            // XanimeCore::calcScaleBlendMaya right-multiplies joint 0's animated rotation R_anim0 by joint0->_64
            // (discarding its translation), then left-multiplies by base: R_real = R_base * R_anim0 * R_64.
            // On the puppet joint0->_64 is nullptr, so it computes R_puppet = R_cap * R_anim0.
            // Conjugate by R_anim0 (R_cap = R_base * R_anim0 * R_64 * R_anim0^-1) so R_puppet == R_real even when R_anim0 != I.
            Mtx rot64;
            PSMTXCopy(joint0->_64, rot64);
            rot64[0][3] = 0.0f;
            rot64[1][3] = 0.0f;
            rot64[2][3] = 0.0f;
            if (core->mJointList != nullptr) {
                Quaternion q0 = core->mJointList[0]._28.mRotation;
                const f32 n2 = q0.x * q0.x + q0.y * q0.y + q0.z * q0.z + q0.w * q0.w;
                if (n2 > 1e-6f) {
                    const f32 invN = 1.0f / sqrtf(n2);
                    q0.x *= invN;
                    q0.y *= invN;
                    q0.z *= invN;
                    q0.w *= invN;
                    Quaternion qInv = {-q0.x, -q0.y, -q0.z, q0.w};
                    Mtx anim0Mtx, anim0Inv, tmp1, tmp2;
                    PSMTXQuat(anim0Mtx, &q0);
                    PSMTXQuat(anim0Inv, &qInv);
                    PSMTXConcat(base, anim0Mtx, tmp1);
                    PSMTXConcat(tmp1, rot64, tmp2);
                    PSMTXConcat(tmp2, anim0Inv, poseMtx);
                } else {
                    PSMTXConcat(base, rot64, poseMtx);
                }
                // wave8: XanimeCore applies joint 0's local translation in R_base space (T_base + scale * R_base * rTrans),
                // whereas the puppet applies it in R_cap space (T_cap + scale * R_cap * rTrans). Compensate T_cap so joint 0
                // lands at the exact same world position during face-first fly-ins and tilted binds.
                const TVec3f& rt = core->mJointList[0]._28._C;
                const f32 rt2 = rt.x * rt.x + rt.y * rt.y + rt.z * rt.z;
                if (rt2 > 1e-8f && rt2 < 1e6f) {
                    const f32 sx = model->mBaseScale.x;
                    const f32 sy = model->mBaseScale.y;
                    const f32 sz = model->mBaseScale.z;
                    const f32 tx = rt.x * sx, ty = rt.y * sy, tz = rt.z * sz;
                    const f32 dx = (base[0][0] - poseMtx[0][0]) * tx + (base[0][1] - poseMtx[0][1]) * ty + (base[0][2] - poseMtx[0][2]) * tz;
                    const f32 dy = (base[1][0] - poseMtx[1][0]) * tx + (base[1][1] - poseMtx[1][1]) * ty + (base[1][2] - poseMtx[1][2]) * tz;
                    const f32 dz = (base[2][0] - poseMtx[2][0]) * tx + (base[2][1] - poseMtx[2][1]) * ty + (base[2][2] - poseMtx[2][2]) * tz;
                    if (dx == dx && dy == dy && dz == dz && (dx * dx + dy * dy + dz * dz) < 250.0f * 250.0f) {
                        posX += dx;
                        posY += dy;
                        posZ += dz;
                    }
                }
            } else {
                PSMTXConcat(base, rot64, poseMtx);
            }
            rotSrc = reinterpret_cast< const f32(*)[4] >(&poseMtx[0][0]);
        }
        MpPoseMath::quatFromMtx(rotSrc, p->quat);
        p->pos[0] = posX;
        p->pos[1] = posY;
        p->pos[2] = posZ;
        const TVec3f* v = MR::getPlayerVelocity();
        if (v != nullptr) {
            p->vel[0] = v->x;
            p->vel[1] = v->y;
            p->vel[2] = v->z;
        }
        p->scale = model->mBaseScale.x;

        bool simple = false;
        p->anim = groupHash(lower, &simple);
        p->frame = lower->getFrameCtrl() != nullptr ? lower->tellAnimationFrame() : 0.0f;
        p->rate = lower->getFrameCtrl() != nullptr ? lower->getRate() : 0.0f;
        for (int i = 0; i < 4; i++) {
            p->weights[i] = lower->mWeights[i];
        }

        // the bck on each track (track-swapped bcks: slope idle, swim variants, ...)
        ResTable* motions = nullptr;
        if (lower->mResourceTable != nullptr && lower->mResourceTable->mResourceHolder != nullptr) {
            motions = lower->mResourceTable->mResourceHolder->mMotionResTable;
        }
        if (core != nullptr && motions != nullptr && !simple) {
            p->hasTracks = true;
            for (int i = 0; i < 4; i++) {
                const void* res = i < core->mTrackCount ? core->mTrackList[i]._0 : nullptr;
                if (res != sTrackRes[i]) {
                    sTrackRes[i] = res;
                    const char* name = res != nullptr ? motions->getResName(res) : nullptr;
                    sTrackHash[i] = name != nullptr ? MpPoseMath::animHash(name) : 0;
                }
                p->tracks[i] = sTrackHash[i];
            }
        }

        uint16_t flags = 0;
        if (a->mMarioAnim->_6C && a->mMarioAnim->mXanimePlayerUpper != nullptr) {
            XanimePlayer* upper = a->mMarioAnim->mXanimePlayerUpper;
            bool upperSimple = false;
            uint32_t h = groupHash(upper, &upperSimple);
            if (h != 0 && !upperSimple) {
                p->upperAnim = h;
                p->upperFrame = upper->getFrameCtrl() != nullptr ? upper->tellAnimationFrame() : 0.0f;
                flags |= MP_POSE_UPPER;
            }
        }
        if (simple) {
            flags |= MP_POSE_SIMPLE_BCK;
        }
        // (puppetprep) Boo Mario's body is hidden by the game (his own Boo model replaces it): he stays visible to the others
        bool hidden = mp_pose_hidden(a->_481, a->_482, a->_483, MR::isPlayerHidden(), a->mFlag.mIsHiddenModel, p->form);
        if (!hidden) {
            flags |= MP_POSE_VISIBLE;
        }
        if (a->getMovementStates()._1) {
            flags |= MP_POSE_ON_GROUND;
        }
        if (a->IsMarioSwimming()) {
            flags |= MP_POSE_IN_WATER;
        }
        if (MR::isPlayerDead()) {
            flags |= MP_POSE_DEAD;
        }
        if (MR::isDemoActive()) {
            flags |= MP_POSE_IN_DEMO;
        }
        if (MR::isPlayerInBind()) {
            flags |= MP_POSE_IN_BIND;
        }
        if (MpBubble::localBubbled()) {
            flags |= MP_POSE_BUBBLE;   // bubble: everyone draws our bubble from this flag
        }
        if (MpAway::local()) {
            flags |= MP_POSE_AWAY;   // MP3 touches: the others dim our head and show zzz on our tag
        }
        if (*reinterpret_cast< const u32* >(&model->mFlags) & 1) {
            flags |= MP_POSE_MIRROR;
        }
        if (MpPuppetsInt::localSquashTurns() != 0) {
            flags |= MP_POSE_SQUASHED;
        }
        if (p->anim != 0) {
            const char* n = lower->getCurrentAnimationName();
            // shell carried / jet shell (GhostPlayer's markers): "水泳ジェット", "カメ持ち"
            if (n != nullptr && (strstr(n, "\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67") != nullptr ||
                                 strstr(n, "\x83\x4a\x83\x81\x8e\x9d\x82\xbf") != nullptr)) {
                flags |= MP_POSE_SHELL;
            }
        }
        p->flags = flags;

        if (!sLoggedFirst) {
            sLoggedFirst = true;
            mp_log("puppet capture first serial=%u anim=%08x simple=%d form=%u", (unsigned)p->sceneSerial, (unsigned)p->anim,
                   simple ? 1 : 0, (unsigned)p->form);
        }
        return true;
    }
}  // namespace MpLocalCapture
