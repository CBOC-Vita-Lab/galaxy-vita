#include "Game/LiveActor/LiveActor.hpp"
#include "Game/AudioLib/AudAnmSoundObject.hpp"
#include "Game/LiveActor/ActorAnimKeeper.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/ActorPadAndCameraCtrl.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/Binder.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/LiveActor/EffectKeeper.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/HitSensorKeeper.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/LiveActor/RailRider.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/Spine.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/Screen/StarPointerTarget.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>

#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#if defined(__vita__) && PORT_LOGGING
extern "C" u32 port_workload_anim_family_begin(unsigned family);
extern "C" void port_workload_anim_family_end(u32 token);
#endif
#if PORT_LOGGING
// GXV_ANIM_PROF (platform/src/port/perf.cpp): the row of the object being timed is named by its model (ASCII resource name).
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_actor_name(const char* name);
extern "C" void port_anim_prof_count(int what);
static inline void PortAnimProfName(const LiveActor* pActor) {
    if (port_anim_prof_on() && pActor->mModelManager != nullptr) {
        port_anim_prof_actor_name(MR::getModelResName(pActor));
    }
}
#define PORT_ANIM_PROF_NAME(actor) PortAnimProfName(actor)
#define PORT_ANIM_PROF_COUNT(what) do { if (port_anim_prof_on()) port_anim_prof_count(what); } while (0)
#else
#define PORT_ANIM_PROF_NAME(actor) ((void)0)
#define PORT_ANIM_PROF_COUNT(what) ((void)0)
#endif
#endif

void LiveActor_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)-1.0f;
}

LiveActor::LiveActor(const char* pName)
    : NameObj(pName), mPosition(0.0f, 0.0f, 0.0f), mRotation(0.0f, 0.0f, 0.0f), mScale(1.0f, 1.0f, 1.0f), mVelocity(0.0f, 0.0f, 0.0f),
      mGravity(0.0f, -1.0f, 0.0f), mModelManager(), mAnimKeeper(), mSpine(), mSensorKeeper(), mBinder(), mRailRider(), mEffectKeeper(),
      mSoundObject(), mShadowControllerList(), mCollisionParts(), mStageSwitchCtrl(), mStarPointerTarget(), mActorLightCtrl(), mCameraCtrl() {
    MR::getAllLiveActorGroup()->registerActor(this);
    MR::getClippingDirector()->registerActor(this);
}

void LiveActor::init(const JMapInfoIter& rIter) {
}

void LiveActor::appear() {
    makeActorAppeared();
}

void LiveActor::makeActorAppeared() {
    if (mSensorKeeper != nullptr) {
        mSensorKeeper->validateBySystem();
    }

    if (MR::isClipped(this)) {
        endClipped();
    }

    mFlag.mIsDead = false;

    if (mCollisionParts != nullptr) {
        MR::validateCollisionParts(this);
    }

    MR::resetPosition(this);

    if (mActorLightCtrl != nullptr) {
        mActorLightCtrl->reset();
    }

    MR::tryUpdateHitSensorsAll(this);
    MR::addToClippingTarget(this);
    MR::connectToSceneTemporarily(this);

    if (!MR::isNoEntryDrawBuffer(this)) {
        MR::connectToDrawTemporarily(this);
    }
}

void LiveActor::kill() {
    makeActorDead();
}

void LiveActor::makeActorDead() {
    mVelocity.zero();

    MR::clearHitSensors(this);

    if (mSensorKeeper != nullptr) {
        mSensorKeeper->invalidateBySystem();
    }

    if (getBinder() != nullptr) {
        mBinder->clear();
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->clear();
    }

    if (mCollisionParts != nullptr) {
        MR::invalidateCollisionParts(this);
    }

    mFlag.mIsDead = true;

    MR::removeFromClippingTarget(this);
    MR::disconnectToSceneTemporarily(this);
    MR::disconnectToDrawTemporarily(this);
}

void LiveActor::movement() {
#ifdef TARGET_PC
    PORT_ANIM_PROF_NAME(this);
#endif
    if (mModelManager != nullptr && !mFlag.mIsStoppedAnim) {
        mModelManager->update();

        if (mAnimKeeper != nullptr) {
            mAnimKeeper->update();
        }
    }

    if (MR::isCalcGravity(this)) {
        MR::calcGravity(this);
    }

    if (mSensorKeeper != nullptr) {
        mSensorKeeper->doObjCol();
    }

    if (mFlag.mIsDead) {
        return;
    }

    if (mSpine != nullptr) {
        mSpine->update();
    }

    if (mFlag.mIsDead) {
        return;
    }

    control();

    if (mFlag.mIsDead) {
        return;
    }

    updateBinder();

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->update();
    }

    if (mCameraCtrl != nullptr) {
        mCameraCtrl->update();
    }

    if (mActorLightCtrl != nullptr) {
        MR::updateLightCtrl(this);
    }

    MR::tryUpdateHitSensorsAll(this);
    MR::actorSoundMovement(this);
    MR::requestCalcActorShadow(this);
}

#ifdef TARGET_PC
// GXV_ANIM_LOD (default 1; GXV_ANIM_LOD_DIST, default 3000 units): on a turn that draws no picture, a map object, NPC, decoration
// or environment model with no collision of its own and farther than the distance from both the player and the camera computes its
// joints and view only once per picture, just before it (port_anim_flush_stale): the picture is exact; what reads its joints between
// two pictures (an effect bound to a joint, a sensor, a part following a joint) sees them as of the last picture. Collision objects, enemies, items,
// the player and everything near keep every turn. The list is dropped with the scene (GameSystemSceneController).
extern "C" int port_turn_skipped_raw(void);
extern "C" int port_anim_lod_category(void);
extern "C" float port_anim_lod_dist2(void);
extern "C" float port_anim_lod_pdist2(void);
extern "C" int port_anim_lod_offscreen(void);
extern "C" int port_anim_lod_free_on(void);  // (GXV_ANIM_LOD_FREE: platform/src/port/perf.cpp)
extern "C" int port_view_pending(void);
#include <vector>
#include "port/host_vector.h"  // (PortHostVector: lists that outlive a scene keep host memory)
#include <algorithm>
#include <cstdlib>
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ObjUtil.hpp"
LiveActor* gPortCalcAnimOwner = nullptr;
// The actors of the current scene (AllLiveActorGroup), sorted, taken at the start of a drawn turn: a listed pointer is used only if
// it is still one of them. Models can be freed inside a scene (the file select's), so a pointer kept from an earlier turn may be stale.
static PortHostVector< LiveActor* > sLiveSnapshot;  // (host heap: it outlives the scene; port/host_vector.h)
PORT_HEAP_GUARD(sLiveSnapshot);
// GXV_LIVE_SNAP (draw7, 5 Nov; default 1, platform/src/port/perf.cpp port_live_snap_on): the snapshot is taken twice per drawn turn
// (the far models' joints, then the materials) and the group hardly ever changes between pictures. The group's array as it was at
// the last sort is kept: when every entry (and their number) is the same, the sorted copy already is what the sort would make, and
// it stays. Exact (the same vector; a compare of the array instead of the copy and the sort). 0: sorted every time, as before.
extern "C" int port_live_snap_on(void);
static PortHostVector< LiveActor* > sLiveRaw;
PORT_HEAP_GUARD(sLiveRaw);
static bool sLiveRawValid = false;
void port_live_actor_snapshot() {
    if (!MR::isExistSceneObj(SceneObj_AllLiveActorGroup)) {
        sLiveSnapshot.clear();
        sLiveRawValid = false;
        return;
    }
    AllLiveActorGroup* group = MR::getAllLiveActorGroup();
    const s32 n = group->getObjNum();
    if (port_live_snap_on()) {
        bool same = sLiveRawValid && static_cast< s32 >(sLiveRaw.size()) == n;
        for (s32 i = 0; same && i < n; ++i) {
            same = sLiveRaw[i] == group->getActor(i);
        }
        if (same) {
            return;  // (sLiveSnapshot is the sorted copy of exactly these entries)
        }
        sLiveRaw.resize(n > 0 ? static_cast< size_t >(n) : 0);
        for (s32 i = 0; i < n; ++i) {
            sLiveRaw[i] = group->getActor(i);
        }
        sLiveRawValid = true;
    } else {
        sLiveRawValid = false;
    }
    sLiveSnapshot.clear();
    sLiveSnapshot.reserve(n);
    for (s32 i = 0; i < n; ++i) {
        sLiveSnapshot.push_back(group->getActor(i));
    }
    std::sort(sLiveSnapshot.begin(), sLiveSnapshot.end());
}
bool port_live_actor_valid(const LiveActor* p) {
    return std::binary_search(sLiveSnapshot.begin(), sLiveSnapshot.end(), const_cast< LiveActor* >(p));
}
extern "C" int port_on_frame_thread(void);
namespace {
PortHostVector< LiveActor* > sStaleAnim;  // (host heap: port/host_vector.h)
PORT_HEAP_GUARD(sStaleAnim);
float MaxAbsScale(const TVec3f& s) {
    const float x = s.x < 0.0f ? -s.x : s.x, y = s.y < 0.0f ? -s.y : s.y, z = s.z < 0.0f ? -s.z : s.z;
    return x > y ? (x > z ? x : z) : (y > z ? y : z);
}
bool AnimLodDefers(const LiveActor* pActor) {
    if (!port_on_frame_thread()) {
        return false;  // (an actor built on the loading thread is computed at once: perf.cpp, port_on_frame_thread)
    }
    // (any turn, drawn or not: the turn that draws next computes the model just before its picture, see port_anim_flush_stale)
    if (pActor->mCollisionParts != nullptr || !port_anim_lod_category()) {
        return false;
    }
    // GXV_ANIM_LOD_FREE (cpuhive, 6 Nov; default 1): a model whose class says nothing but the picture reads its joints (PlantMember:
    // the flowers and bushes of the flower groups) is computed once per picture at any distance. Exact: the picture is the flush's,
    // from the same state; nothing between two pictures reads what is left out (see PlantGroup.cpp).
    if (pActor->mLogicFreeJoints != 0 && port_anim_lod_free_on()) {
        return true;
    }
    const float d2 = port_anim_lod_dist2();
    if (d2 <= 0.0f) {
        return false;
    }
    const TVec3f* player = MR::getPlayerPos();
    const TVec3f& p = pActor->mPosition;
    // The player's distance keeps the game's logic (GXV_ANIM_LOD_PDIST; by default the same distance as the camera's, as before).
    if (player != nullptr) {
        const float px = p.x - player->x, py = p.y - player->y, pz = p.z - player->z;
        if (px * px + py * py + pz * pz <= port_anim_lod_pdist2()) {
            return false;
        }
    }
    const TVec3f cam = MR::getCamPos();
    const float cx = p.x - cam.x, cy = p.y - cam.y, cz = p.z - cam.z;
    const float c2 = cx * cx + cy * cy + cz * cz;
    if (c2 > d2) {
        return true;
    }
    // GXV_ANIM_LOD_OFFSCREEN (5 Nov): nearer the camera, but all of it outside the view. The radius is the model's own bounding
    // radius (MR::calcModelBoundingRadius, taken once after its first full computation: LiveActor::calcAnim) with a margin for the
    // limbs its animation moves; within 600 units plus that of the camera never (the clipping frustum's near plane is at 500).
    if (!port_anim_lod_offscreen() || !(pActor->mLodRadius > 0.0f)) {
        return false;
    }
    float r = pActor->mLodRadius * 1.25f * MaxAbsScale(pActor->mScale) + 100.0f;
    if (r < 300.0f) {
        r = 300.0f;
    }
    if (c2 <= (600.0f + r) * (600.0f + r)) {
        return false;
    }
    return MR::isJudgedToClipFrustum(p, r);
}
// The radius GXV_ANIM_LOD_OFFSCREEN tests, once, from the joints a full computation has just made (at scale 1; 0: unusable).
void AnimLodTakeRadius(LiveActor* pActor) {
    if (pActor->mLodRadius >= 0.0f || !port_anim_lod_offscreen() || port_anim_lod_dist2() <= 0.0f || !port_anim_lod_category() ||
        pActor->mCollisionParts != nullptr || !port_on_frame_thread()) {
        return;
    }
    f32 r = 0.0f;
    MR::calcModelBoundingRadius(&r, pActor);
    const float s = MaxAbsScale(pActor->mScale);
    pActor->mLodRadius = (r > 0.0f && r < 100000.0f && s > 0.001f) ? r / s : 0.0f;
}
}  // namespace
// GXV_PAR_LATE (par2, ModelManager.cpp port_par_late_prepare): the base matrix LiveActor::calcAndSetBaseMtx (below) would set now, into
// out, without setting it (false: the actor is taken; its host's matrix is not predicted). Only a prediction: the key decides.
bool port_predict_default_base(const LiveActor* a, MtxPtr out) {
    if (MR::getTaken(a)) {
        return false;
    }
    TPos3f mtx;
    if (a->mRotation.x == 0.0f && a->mRotation.z == 0.0f) {
        MR::makeMtxTransRotateY(mtx, a);
    } else {
        MR::makeMtxTR(mtx, a);
    }
    PSMTXCopy(mtx.toMtxPtr(), out);
    return true;
}
// GXV_PAR_FLUSH (par2, ModelManager.cpp port_par_flush_prepare): the actors port_anim_flush_stale will compute, in its order.
LiveActor* const* port_anim_stale_items(size_t* n) {
    *n = sStaleAnim.size();
    return sStaleAnim.data();
}
// 1.3 (exact): the start of a drawn turn, after the deferred draw-done wait (the front end is past the last picture) and before
// beginRender. A listed far model gets the calcAnim and the view pass the last turn left out, from the same state (no update() has
// run since), so the picture shows it exactly as if it had been computed every turn; the turns in between computed nothing for it.
// The view pass needs the camera's view matrix in j3dSys, as executeCalcViewAndEntryList loads it for the 3D lists; it is restored.
// A model that died, was clipped or hidden since it was listed is skipped (it is not drawn). drop: the scene is going away.
extern "C" void port_anim_flush_stale(int drop) {
    PortHostVector< LiveActor* > work;  // (taken out first: what runs below may list actors again)
    work.swap(sStaleAnim);
    PortHostVector< LiveActor* >& sStaleAnimNow = work;
    if (!drop && !sStaleAnimNow.empty()) {
        port_live_actor_snapshot();
        const bool viewPending = port_view_pending() != 0;
        Mtx saved;
        PSMTXCopy(j3dSys.mViewMtx, saved);
        MR::loadViewMtx();
        for (size_t i = 0; i < sStaleAnimNow.size(); ++i) {
            LiveActor* a = sStaleAnimNow[i];
            if (!port_live_actor_valid(a)) {
                continue;  // (no longer an actor of this scene: not touched)
            }
            a->mStaleAnim = false;
            if (a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel || a->mFlag.mIsNoCalcAnim) {
                a->mStaleXLatch = false;
                continue;
            }
            if (a->mStaleXLatch && a->mModelManager != nullptr && a->mModelManager->mXanimePlayer != nullptr) {
                a->mModelManager->mXanimePlayer->_88 = true;  // (the left-out calcAnm's frame, _84: see LiveActor::calcAnim)
            }
            a->mStaleXLatch = false;
            a->calcAnim();          // (port_anim_lod_category() is -1 here: nothing defers)
            if (!viewPending) {
                a->calcViewAndEntry();  // (else the whole view pass follows: port_view_flush)
            }
        }
        PSMTXCopy(saved, j3dSys.mViewMtx);
    } else if (drop && !sStaleAnimNow.empty()) {
        // The scene is about to be destroyed (its memory is still there): clear the marks of the actors still in it, so one that
        // outlives the scene is listed again and never stays without its view pass.
        port_live_actor_snapshot();
        for (size_t i = 0; i < sStaleAnimNow.size(); ++i) {
            if (port_live_actor_valid(sStaleAnimNow[i])) {
                sStaleAnimNow[i]->mStaleAnim = false;
            }
        }
    }
    work.clear();
    if (sStaleAnim.empty() && sStaleAnim.capacity() < work.capacity()) {
        sStaleAnim.swap(work);
    }
}
#endif

void LiveActor::calcAnim() {
    if (mFlag.mIsNoCalcAnim) {
        return;
    }
#ifdef TARGET_PC
    PORT_ANIM_PROF_NAME(this);
    if (mModelManager != nullptr && AnimLodDefers(this)) {
        PORT_ANIM_PROF_COUNT(0);
        // The BCK clock (XanimePlayer) is latched by calcAnim: XanimePlayer::updateAfterMovement advances the frame only if the
        // last calcAnm cleared _88 (the frame before the advance is kept in _84: calcAnm poses that frame, and checkPass reads it
        // while _88 is set). A left-out calcAnim that left _88 set froze the animation at one frame per PICTURE (5x slow at
        // 10 FPS), and every turn of the freeze answered checkPass for the same window again: each BCK-synced one-time effect
        // (SyncBckEffectChecker::isCreate via EffectKeeper::syncEffectBck), sound or nerve step tied to a frame fired once per
        // turn instead of once (lit Observatory of a 50-star file, 4 Nov: ~545 effect draws a picture instead of ~80, and a GPU
        // runaway from 37 to 10 FPS). The latch is cleared here as calcAnm clears it; whether calcAnm would have posed _84 is
        // kept, and port_anim_flush_stale sets the latch back for its calcAnm (_84 is unchanged: no update() runs between the
        // last calcAnim and the flush), so the picture's pose is the same frame as before. GXV_ANIM_LOD_XLATCH=0: the old way.
        const bool sXLatch = PORT_HOT_KNOB(bool, [] {  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
            const char* e = getenv("GXV_ANIM_LOD_XLATCH");
            return !(e && *e == '0');
        }());
        if (sXLatch && mModelManager->mXanimePlayer != nullptr) {
            mStaleXLatch = mModelManager->mXanimePlayer->_88;
            mModelManager->mXanimePlayer->_88 = false;
        }
        if (!mStaleAnim) {
            mStaleAnim = true;
            sStaleAnim.push_back(this);
        }
        return;
    }
    // (computed now; if it is still listed, from a farther turn, the flush computes it once more from the same state: it must
    // pose the frame this calcAnm poses, so the latch this calcAnm finds is kept for it as above)
    mStaleXLatch = mStaleAnim && mModelManager != nullptr && mModelManager->mXanimePlayer != nullptr && mModelManager->mXanimePlayer->_88;
#endif

    calcAnmMtx();
#ifdef TARGET_PC
    if (mLodRadius < 0.0f && mModelManager != nullptr) {
        AnimLodTakeRadius(this);  // (GXV_ANIM_LOD_OFFSCREEN)
    }
#endif

    if (mCollisionParts == nullptr) {
        return;
    }

    MR::setCollisionMtx(this);
}

void LiveActor::calcAnmMtx() {
    if (mModelManager == nullptr) {
        return;
    }

    MR::getJ3DModel(this)->setBaseScale(mScale);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 familyToken = port_workload_anim_family_begin(0);
#endif
    calcAndSetBaseMtx();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_family_end(familyToken);
#endif
#ifdef TARGET_PC
    gPortCalcAnimOwner = this;  // (GXV_DL_LAZY lists only models of live actors, see ModelManager.cpp)
    mModelManager->calcAnim();
    gPortCalcAnimOwner = nullptr;
#else
    mModelManager->calcAnim();
#endif
}

void LiveActor::calcViewAndEntry() {
    if (mFlag.mIsNoCalcView) {
        return;
    }
#ifdef TARGET_PC
    if (mStaleAnim) {
        return;  // (GXV_ANIM_LOD: its view pass is made with its joints, before the next picture)
    }
#endif

    if (mModelManager == nullptr) {
        return;
    }

    if (mFlag.mIsNoCalcView) {
        return;
    }

    mModelManager->calcView();
}

bool LiveActor::receiveMessage(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (msg == ACTMES_PUSH) {
        return receiveMsgPush(pSender, pReceiver);
    }

    bool isMsgPlayerAttack = msg > ACTMES_PLAYER_ATTACK_START && msg < ACTMES_PLAYER_ATTACK_END;

    if (isMsgPlayerAttack) {
        return receiveMsgPlayerAttack(msg, pSender, pReceiver);
    }

    bool isMsgEnemyAttack = msg > ACTMES_ENEMY_ATTACK_START && msg < ACTMES_ENEMY_ATTACK_END;

    if (isMsgEnemyAttack) {
        return receiveMsgEnemyAttack(msg, pSender, pReceiver);
    }

    if (msg == ACTMES_TAKE) {
        return receiveMsgTake(pSender, pReceiver);
    }

    if (msg == ACTMES_TAKEN) {
        return receiveMsgTaken(pSender, pReceiver);
    }

    if (msg == ACTMES_THROW) {
        return receiveMsgThrow(pSender, pReceiver);
    }

    if (msg == ACTMES_APART) {
        return receiveMsgApart(pSender, pReceiver);
    }

    return receiveOtherMsg(msg, pSender, pReceiver);
}

void LiveActor::calcAndSetBaseMtx() {
    if (MR::getTaken(this)) {
        MR::setBaseTRMtx(this, MR::getTaken(this)->mHost->getTakingMtx());
    } else {
        TPos3f mtx;

        if (mRotation.x == 0.0f && mRotation.z == 0.0f) {
            MR::makeMtxTransRotateY(mtx, this);
        } else {
            MR::makeMtxTR(mtx, this);
        }

        MR::setBaseTRMtx(this, mtx);
    }
}

MtxPtr LiveActor::getTakingMtx() const {
    return getBaseMtx();
}

void LiveActor::setNerve(const Nerve* pNerve) {
    mSpine->setNerve(pNerve);
}

bool LiveActor::isNerve(const Nerve* pNerve) const {
    return mSpine->getCurrentNerve() == pNerve;
}

s32 LiveActor::getNerveStep() const {
    return mSpine->mStep;
}

HitSensor* LiveActor::getSensor(const char* pName) const {
    if (mSensorKeeper != nullptr) {
        return mSensorKeeper->getSensor(pName);
    }

    return nullptr;
}

MtxPtr LiveActor::getBaseMtx() const {
    if (MR::getJ3DModel(this) != nullptr) {
        return MR::getJ3DModel(this)->mBaseTransformMtx;
    }

    return nullptr;
}

void LiveActor::startClipped() {
    mFlag.mIsClipped = true;

    if (getSensorKeeper() != nullptr) {
        mSensorKeeper->invalidateBySystem();
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->stopEmitterOnClipped();
    }

    MR::disconnectToSceneTemporarily(this);

    if (MR::isNoEntryDrawBuffer(this)) {
        return;
    }

    MR::disconnectToDrawTemporarily(this);
}

void LiveActor::endClipped() {
    mFlag.mIsClipped = false;

    if (getSensorKeeper() != nullptr) {
        mSensorKeeper->validateBySystem();
        MR::updateHitSensorsAll(this);
    }

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->playEmitterOffClipped();
    }

    MR::connectToSceneTemporarily(this);

    if (MR::isNoEntryDrawBuffer(this)) {
        return;
    }

    MR::connectToDrawTemporarily(this);
}

void LiveActor::initModelManagerWithAnm(const char* pModelName, const char* pAnimName, bool a3) {
    mModelManager = new ModelManager();
    mModelManager->init(pModelName, pAnimName, a3);

    MR::getJ3DModel(this)->setBaseScale(mScale);
    LiveActor::calcAndSetBaseMtx();
    MR::calcJ3DModel(this);

    mAnimKeeper = ActorAnimKeeper::tryCreate(this);
    mCameraCtrl = ActorPadAndCameraCtrl::tryCreate(mModelManager, &mPosition);
}

void LiveActor::initNerve(const Nerve* pNerve) {
    mSpine = new Spine(this, pNerve);
}

void LiveActor::initHitSensor(int numSensors) {
    mSensorKeeper = new HitSensorKeeper(numSensors);
}

void LiveActor::initBinder(f32 radius, f32 offsetY, u32 planeNum) {
    mBinder = new Binder(getBaseMtx(), &mPosition, &mGravity, radius, offsetY, planeNum);

    MR::onBind(this);

    if (mEffectKeeper != nullptr) {
        mEffectKeeper->setBinder(mBinder);
    }
}

void LiveActor::initRailRider(const JMapInfoIter& rIter) {
    mRailRider = new RailRider(rIter);
}

void LiveActor::initEffectKeeper(int a1, const char* a2, bool doSort) {
    mEffectKeeper = new EffectKeeper(getName(), MR::getModelResourceHolder(this), a1, a2);

    if (doSort) {
        mEffectKeeper->enableSort();
    }

    mEffectKeeper->init(this);

    if (mBinder != nullptr) {
        mEffectKeeper->setBinder(mBinder);
    }
}

void LiveActor::initSound(int param1, bool is2D) {
    if (!is2D) {
        mSoundObject = new AudAnmSoundObject(&mPosition, param1, MR::getCurrentHeap());
    } else {
        mSoundObject = new AudAnmSoundObject(nullptr, param1, MR::getCurrentHeap());
    }
}

void LiveActor::initShadowControllerList(u32 shadowNum) {
    mShadowControllerList = new ShadowControllerList(this, shadowNum);
}

void LiveActor::initActorCollisionParts(const char* pParam1, HitSensor* pParam2, ResourceHolder* pParam3, MtxPtr pParam4, bool param5, bool param6) {
    MR::CollisionScaleType scaleType;

    if (param6) {
        scaleType = MR::CollisionScaleType_NotUsingScale;
    } else {
        scaleType = MR::CollisionScaleType_Unk2;

        if (param5) {
            scaleType = MR::CollisionScaleType_AutoEqualScale;
        }
    }

    if (pParam3 != nullptr) {
        TPos3f mtx;

        if (pParam4 != nullptr) {
            mtx.set(pParam4);
        } else {
            MR::makeMtxTRS(mtx.toMtxPtr(), this);
        }

        mCollisionParts = MR::createCollisionPartsFromResourceHolder(pParam3, pParam1, pParam2, mtx, scaleType);
    } else if (pParam4 == nullptr) {
        mCollisionParts = MR::createCollisionPartsFromLiveActor(this, pParam1, pParam2, scaleType);
    } else {
        mCollisionParts = MR::createCollisionPartsFromLiveActor(this, pParam1, pParam2, pParam4, scaleType);
    }

    MR::invalidateCollisionParts(this);
}

void LiveActor::initStageSwitch(const JMapInfoIter& rIter) {
    mStageSwitchCtrl = MR::createStageSwitchCtrl(this, rIter);
}

void LiveActor::initActorStarPointerTarget(f32 radius, const TVec3f* pTrans, MtxPtr pMtx, TVec3f offset) {
    mStarPointerTarget = new StarPointerTarget(radius, pTrans, pMtx, offset);
}

void LiveActor::initActorLightCtrl() {
    mActorLightCtrl = new ActorLightCtrl(this);
}

void LiveActor::attackSensor(HitSensor* pSender, HitSensor* pReceiver) {
}

bool LiveActor::receiveMsgApart(HitSensor* pSender, HitSensor* pReceiver) {
    MR::setHitSensorApart(pSender, pReceiver);

    return true;
}

void LiveActor::addToSoundObjHolder() {
    mSoundObject->addToSoundObjHolder();
}

void LiveActor::updateBinder() {
    if (mBinder == nullptr) {
        mPosition += mVelocity;
    } else if (mFlag.mIsNoBind) {
        mPosition += mVelocity;
        mBinder->clear();
    } else {
        mPosition += getBinder()->bind(mVelocity);
    }
}
