// Galaxy 3.0 multiplayer: puppets (owner: puppet). See MpRemotePlayer.hpp and DESIGN.md section 4 "Puppets".
//
// MpRemotePlayer is set up like GhostPlayer (XanimePlayer with its own simple group, upper-body player sharing the joint
// transforms) on the real "Mario"/"Luigi" model with MarioAnime, or on "GhostMario" (Cosmic Mario) with its btk/brk shimmer. It is
// drawn like GhostPlayer (viewCalc2 + directDraw, no MarioActor-only APIs such as setDrawView: a puppet's J3DModelX has no extra
// matrix buffers) plus, for Mario and Luigi, Mario's lighting display list (ambient from the puppet's own ActorLightCtrl, material
// colour = form tint), or the first-person alpha list for Star Race ghosts.
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/ModelManager.hpp"
#include "Game/MP/MpDropInLogic.hpp"
#include "Game/MP/MpFormation.hpp"   // wave8: suppress invisible sensor pushes while formation spreads puppets
#include "Game/MP/MpLod.hpp"   // MP3 lod2: level of detail by distance
#include "Game/MP/MpCoop.hpp"   // MP3 coop: the head spring (super jump), the greeting voice
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpQuality.hpp"
#include "Game/MP/MpTeam.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/System/ResourceInfo.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/ActorShadowUtil.hpp"
#include "Game/Util/JointUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <revolution/gd.h>
#include <revolution/gx.h>
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <string.h>
#include "port/mp_prof.h"

#ifdef TARGET_PC
extern "C" int port_turn_skipped_raw(void);   // (platform/src/port/perf.cpp: this turn draws no picture, 30 fps mode)
#endif

namespace {
    const char* cBasicAnim = "\x8a\xee\x96\x7b";   // "基本" (default group)
    const u32 cDLBytes = 0x100;

    // (perf) level of detail. Within cLodNear of the camera a puppet is always calculated (the clipping judge's near plane is
    // at 500 units: a puppet right beside the camera would read as outside the frustum). Beyond the far distance (5500 units,
    // MpQuality's level 4: 2500) it is "far" (no shadow volume, joints on every other picture) until it comes back inside the
    // far-out distance (4500; level 4: 2000). MP3 perf3: the distances and the extra half-rate rules come from MpQuality.
    const f32 cLodNear = 1500.0f;
    const f32 cLodRadius = 260.0f;    // bounding sphere around the body's middle (a 190-unit Mario, arms, a held shell)
    const char* const cShadowVolume = "MpVol";   // MP3 lod2: the two shadow controllers' names
    const char* const cShadowBlob = "MpBlob";
    const f32 cBlobScale = 1.1f;      // the circle a little wider than the volume sphere's footprint

    u32 sLastSkippedTurn = 0;
    bool sSawSkippedTurn = false;

    bool turnWasSkipped() {
#ifdef TARGET_PC
        return port_turn_skipped_raw() != 0;
#else
        return false;
#endif
    }

    bool interactionHeld(int slot, u16 flags) {
        if (MpPuppetsInt::pushHeld(flags)) {
            return true;
        }
        return slot >= 0 && slot < 32 && (MpFormation::localSpaceMask() & (1u << slot)) != 0;
    }

    s32 findJoint(J3DModelData* pData, const char* pName) {
        if (pData == nullptr || pData->mJointTree.mJointName == nullptr) {
            return -1;
        }
        s32 idx = pData->mJointTree.mJointName->getIndex(pName);
        if (idx < 0 || idx >= (s32)pData->mJointTree.getJointNum()) {
            return -1;
        }
        return idx;
    }

    // (puppetprep) the form colour comes from the host-tested mp_form_look (Cosmic / Toad: plain; unknown form: normal)
    GXColor formTint(u8 form, u32 rainbow, u8 character) {
        const MpFormLook l = mp_form_look(form, character, rainbow);
        GXColor c = {l.r, l.g, l.b, 255};
        return c;
    }

    const char* modelName(u8 character) { return mp_character_model(character); }
}  // namespace

#ifdef TARGET_PC
extern bool gPortParViewOpen;                 // decomp/src/Game/LiveActor/ModelManager.cpp (as J3DModel::calc reads it)
extern "C" void port_par_view_touch(void* buf);
#endif

namespace {
    // MP3 profile: the first of the puppet's two model passes (MarioAnimator::calc's blend pass) without the weighted envelope
    // matrices: J3DModel::calc minus calcWeightEnvelopeMtx, whose output the second pass writes again from the final joints
    // (nothing reads it in between). The envelope matrices are ~15% of a puppet's joint time on the desktop (both passes): about
    // half of that is saved. A model with anything else in calc (deform, skin, colour,
    // callbacks) takes the full calc, as before.
    void calcJointsOnly(LiveActor* pActor) {
        J3DModel* m = MR::getJ3DModel(pActor);
        if (m->mDeformData != nullptr || m->mSkinDeform != nullptr || m->mVtxColorCalc != nullptr || m->mUnkCalc1 != nullptr ||
            m->mUnkCalc2 != nullptr || m->mCalcCallBack != nullptr) {
            MR::calcJ3DModel(pActor);
            return;
        }
        OSLockMutex(&MR::MutexHolder< 0 >::sMutex);   // (as MR::calcJ3DModel)
#ifdef TARGET_PC
        if (gPortParViewOpen) {
            port_par_view_touch(m->mMtxBuffer);
        }
#endif
        j3dSys.setModel(m);
        if (m->checkFlag(J3DMdlFlag_SkinPosCpu)) {
            j3dSys.onFlag(J3DSysFlag_SkinPosCpu);
        } else {
            j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
        }
        if (m->checkFlag(J3DMdlFlag_SkinNrmCpu)) {
            j3dSys.onFlag(J3DSysFlag_SkinNrmCpu);
        } else {
            j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
        }
        m->mModelData->syncJ3DSysFlags();
        m->mVertexBuffer.setCurrentVtxPos(m->mVertexBuffer.mVtxPosArray[0]);
        m->mVertexBuffer.setCurrentVtxNrm(m->mVertexBuffer.mVtxNrmArray[0]);
        m->mVertexBuffer.setCurrentVtxCol(m->mVertexBuffer.mVtxColArray[0]);
        m->calcAnmMtx();
        OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
    }
}  // namespace

extern XanimeSwapTable luigiAnimeSwapTable[];

namespace {
    bool bckNameEq(const char* a, const char* b) {
        if (a == nullptr || b == nullptr) return false;
        for (; *a && *b; a++, b++) {
            const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
        }
        return *a == *b;
    }

    // Map a captured BCK name (from either a Mario or Luigi sender) to this puppet's character (Mario vs Luigi).
    const char* mapCharacterBck(const char* name, bool wantLuigi) {
        if (name == nullptr) return nullptr;
        for (const XanimeSwapTable* t = ::luigiAnimeSwapTable;
             t->mOriginal != nullptr && t->mSwapped != nullptr && t->mOriginal[0] != '\0'; t++) {
            if (bckNameEq(name, t->mOriginal) || bckNameEq(name, t->mSwapped)) {
                return wantLuigi ? t->mSwapped : t->mOriginal;
            }
        }
        return name;
    }
}  // namespace

// ==== MpPuppetBase ============================================================================================================
MpPuppetBase::MpPuppetBase(const char* pName, int slot, u8 character, bool self)
    : LiveActor(pName), mSlot(slot), mArrive(0), mStandIn(false), mCharacter(character), mSelf(self), mHidden(true), mMirror(false), mSensorsOn(true),
      mShadowOn(true), mAnimated(false), mCulled(false), mFar(false), mStale(true), mHasBlob(false), mLightSkipped(false), mLightInit(true), mShadowMode(MpLod::SH_VOLUME), mForm(MP_FORM_NORMAL), mFlags(0), mTrampleCooldown(0),
      mSpinCooldown(0), mSpringOn(true), mRainbow(0), mDrawPos(0.0f, 0.0f, 0.0f) {
    PSMTXIdentity(mRotMtx);
    PSMTXIdentity(mDrawRotMtx);
}

void MpPuppetBase::initCommon(f32 sensorRadius, f32 sensorHeight, f32 shadowRadius) {
    initHitSensor(2);
    MR::addHitSensorRide(this, "body", 8, sensorRadius, TVec3f(0.0f, sensorHeight, 0.0f));
    // MP3 coop: the head spring (the Spring Topman's sensor type): Mario's own trample code gives its big bounce. Live only
    // while MpCoop::superJumpArmed() (the jump button held in the air); then the body refuses the trample (receiveMsgPlayerAttack)
    MR::addHitSensor(this, "jump", ATYPE_PLAYER_AUTO_JUMP, 8, sensorRadius, TVec3f(0.0f, sensorHeight, 0.0f));
    initSound(4, false);   // MP3 coop: the greeting's voice comes from the player it belongs to
    // MP3 lod2: two shadows, one active at a time: the engine's volume (extra geometry passes) for a near puppet, the round surface
    // circle the small enemies use (one flat disc) for a far one. Switch off (SMG_MP_LOD2_SHADOW=0): the volume alone, as before.
    mHasBlob = MpLod::shadowOn();
    if (mHasBlob) {
        MR::initShadowController(this, 2);
        MR::addShadowVolumeSphere(this, cShadowVolume, shadowRadius);
        MR::addShadowSurfaceCircle(this, cShadowBlob, shadowRadius * cBlobScale);
        MR::setShadowDropPositionPtr(this, cShadowVolume, &mDrawPos);   // MP3 formation: the shadow falls from the drawn position
        MR::setShadowDropPositionPtr(this, cShadowBlob, &mDrawPos);
        MR::onCalcShadow(this, cShadowVolume);
        MR::onCalcShadow(this, cShadowBlob);
        MR::onCalcShadowDropGravity(this, cShadowVolume);
        MR::onCalcShadowDropGravity(this, cShadowBlob);
    } else {
        MR::initShadowVolumeSphere(this, shadowRadius);
        MR::setShadowDropPositionPtr(this, nullptr, &mDrawPos);   // MP3 formation: the shadow falls from the drawn position
        MR::onCalcShadow(this, nullptr);
        MR::onCalcShadowDropGravity(this, nullptr);
    }
    MR::invalidateClipping(this);
    mGravity.set(0.0f, -1.0f, 0.0f);
}

void MpPuppetBase::appear() {
    LiveActor::appear();
    mSensorsOn = true;
    mShadowOn = true;
    mSpringOn = true;   // (validated with the rest: updateInteraction decides)
    MR::validateHitSensors(this);
    setShadowMode(MpLod::SH_VOLUME);   // (MP3 lod2: the volume, as before; updateInteraction picks the mode from the next turn)
    mLightSkipped = false;
    mLightInit = true;
    mHidden = true;
    mDrawPos = mPosition;   // MP3 formation (the shadow's drop position until the first step)
    mAnimated = false;   // not drawn before the first pose and animation calc
    mCulled = false;
    mFar = false;
    mStale = true;
    onHiddenChanged();
    mp_log("puppet slot=%d appear model=%s self=%d", mSlot, modelName(mCharacter), mSelf ? 1 : 0);
}

// drop-in: a spare (slot -1) is dead and samples nothing; bound, it takes the slot's poses from the next turn on
void MpPuppetBase::bindSlot(int slot) {
    if (slot < 0 && !MR::isDead(this)) {
        kill();
    }
    mSlot = slot;
    mArrive = 0;
    mStandIn = false;
    mLightInit = true;
}

void MpPuppetBase::startArrival() {
    mArrive = MpDropIn::ARRIVE_TURNS;
}

u8 MpPuppetBase::arrivalAlpha() const {
    return MpDropIn::arrivalAlpha(mArrive);
}

bool MpPuppetBase::isDrawable() const {
    return !MR::isDead(this) && !mHidden && mAnimated;
}

void MpPuppetBase::headPosition(TVec3f* pOut) const {
    TVec3f u(mDrawRotMtx[0][1], mDrawRotMtx[1][1], mDrawRotMtx[2][1]);   // MP3 formation: the drawn pose (name tag, balloon)
    f32 h = headHeight() * mScale.y;
    pOut->set(mDrawPos.x + u.x * h, mDrawPos.y + u.y * h, mDrawPos.z + u.z * h);
}

void MpPuppetBase::capPosition(TVec3f* pOut) const {
    TVec3f u(mDrawRotMtx[0][1], mDrawRotMtx[1][1], mDrawRotMtx[2][1]);
    f32 h = capHeight() * mScale.y;
    pOut->set(mDrawPos.x + u.x * h, mDrawPos.y + u.y * h, mDrawPos.z + u.z * h);
}

// MP3 formation
void MpPuppetBase::setDrawPose(const TVec3f& rPos, const f32 rot[3][3]) {
    mDrawPos = rPos;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            mDrawRotMtx[r][c] = rot[r][c];
        }
        mDrawRotMtx[r][3] = 0.0f;
    }
}

void MpPuppetBase::trueRotation(f32 out[3][3]) const {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = mRotMtx[r][c];
        }
    }
}

void MpPuppetBase::updateInteraction() {
    const bool bubbled = (mFlags & MP_POSE_BUBBLE) != 0;   // bubble: in a bubble nobody stomps or pushes him (a touch pops it)
    const bool held = interactionHeld(mSlot, mFlags);
    bool sensors = !mSelf && !mHidden && !bubbled && !held && MpPuppetsInt::interactionOn();
    if (sensors != mSensorsOn) {
        mSensorsOn = sensors;
        if (sensors) {
            MR::validateHitSensors(this);
            mSpringOn = true;
        } else {
            MR::invalidateHitSensors(this);
            mSpringOn = false;
        }
    }
    // MP3 coop: the head spring is live only while the local player holds the jump button in the air (super jump)
    const bool spring = mSensorsOn && MpCoop::superJumpArmed();
    if (spring != mSpringOn) {
        mSpringOn = spring;
        if (spring) {
            MR::validateHitSensor(this, "jump");
        } else {
            MR::invalidateHitSensor(this, "jump");
        }
    }
    // the self puppet stands where the local Mario stands: his own shadow is kept. (perf) A far puppet has none: its shadow
    // volume would be a few pixels for a full stencil pass.
    bool shadow = !mSelf && !mHidden && !mFar && !bubbled && MpQuality::shadows() && MpPuppetsInt::style() == MP_PUPPET_SOLID;   // MP3 perf3: level 1+; bubble: the bubble has one
    u8 mode = shadow ? (u8)MpLod::SH_VOLUME : (u8)MpLod::SH_OFF;
    if (mHasBlob && !mSelf && !mHidden && !bubbled && MpPuppetsInt::style() == MP_PUPPET_SOLID && MpLod::enabled()) {
        // MP3 lod2: the volume near, the round surface shadow beyond (and from the governor's level 1 on, where the volume went before)
        mode = MpLod::slot(mSlot).shadow;
        if (mFar) {
            mode = MpLod::SH_OFF;
        }
    }
    if (mode != mShadowMode) {
        setShadowMode(mode);
    }
    mShadowOn = mode != MpLod::SH_OFF;
}

// MP3 lod2: which of the two shadows is valid (the legacy single volume when there is no round one).
void MpPuppetBase::setShadowMode(u8 mode) {
    mShadowMode = mode;
    if (!mHasBlob) {
        if (mode == MpLod::SH_OFF) {
            MR::invalidateShadow(this, nullptr);
        } else {
            MR::validateShadow(this, nullptr);
        }
        return;
    }
    if (mode == MpLod::SH_VOLUME) {
        MR::validateShadow(this, cShadowVolume);
    } else {
        MR::invalidateShadow(this, cShadowVolume);
    }
    if (mode == MpLod::SH_BLOB) {
        MR::validateShadow(this, cShadowBlob);
    } else {
        MR::invalidateShadow(this, cShadowBlob);
    }
}

void MpPuppetBase::control() {
    if (mTrampleCooldown != 0) {
        mTrampleCooldown--;
    }
    if (mSpinCooldown != 0) {
        mSpinCooldown--;
    }
    mRainbow++;
    if (!mSelf) {
        step();
    }
}

void MpPuppetBase::step() {
    MpPoseF p;
    MpSampleResult r = MpPuppetsInt::samplePose(mSlot, &p);
    bool hidden;
    if (r == MP_SAMPLE_NONE) {
        hidden = true;
    } else {
        hidden = (p.flags & MP_POSE_VISIBLE) == 0 || (!mSelf && MpPuppetsInt::style() == MP_PUPPET_HIDDEN);
        mFlags = p.flags;
        mForm = mp_form_clamp(p.form);   // (puppetprep) never index anything by a raw form id
        mMirror = (p.flags & MP_POSE_MIRROR) != 0;

        mPosition.set(p.pos[0], p.pos[1], p.pos[2]);
        MpPoseMath::mtxFromQuat(p.quat, reinterpret_cast< f32(*)[4] >(&mRotMtx[0][0]));
        mVelocity.set(p.vel[0], p.vel[1], p.vel[2]);
        f32 s = p.scale * modelScale();
        if (mArrive != 0 && (p.flags & MP_POSE_VISIBLE) != 0) {
            if (mArrive == MpDropIn::ARRIVE_TURNS) {
                mp_log("puppet slot=%d arrive shown model=%s standin=%d", mSlot, mp_character_model(mCharacter), mStandIn ? 1 : 0);
            }
            s *= MpDropIn::arrivalScale(mArrive);   // drop-in: grows in at its first appearance (counted on visible turns)
            mArrive--;
        }
        if (p.flags & MP_POSE_SQUASHED) {
            mScale.set(s * 1.25f, s * 0.55f, s * 1.25f);
        } else {
            mScale.set(s, s, s);
        }
        mGravity.set(-mRotMtx[0][1], -mRotMtx[1][1], -mRotMtx[2][1]);

        MpTeam::celebrate(&p);   // MP3 celebrate: the star-get demo runs: every puppet plays the star-get animation
        applyAnimation(p);
    }
    mDrawPos = mPosition;   // MP3 formation: drawn at the true pose unless MpFormation moves it later this turn
    PSMTXCopy(mRotMtx, mDrawRotMtx);
    if (hidden != mHidden) {
        mHidden = hidden;
        onHiddenChanged();
    }
    // (a direct light control; the engine-drawn Toad's belongs to its draw buffer. (perf) not while hidden: nothing is lit)
    if (mActorLightCtrl != nullptr && r != MP_SAMPLE_NONE && usesDrawer() && !mHidden) {
        if (mCulled && MpLod::rateOn() && MpLod::enabled()) {   // MP3 lod2: off screen / tiny: nothing is lit (it catches up when it returns)
            mLightSkipped = true;
            MpLod::count(MpLod::C_LIGHT_SKIP);
        } else {
            const bool snap = mLightInit || mLightSkipped;
            mLightInit = false;
            mLightSkipped = false;
            mActorLightCtrl->update(snap);
        }
    }
    updateInteraction();
}

// MP3 lod2: the body's middle where the puppet is drawn (the point the level of detail measures the camera distance to).
void MpPuppetBase::lodCenter(TVec3f* pOut) const {
    TVec3f up(mDrawRotMtx[0][1], mDrawRotMtx[1][1], mDrawRotMtx[2][1]);
    f32 mid = headHeight() * 0.5f * mScale.y;
    pOut->set(mDrawPos.x + up.x * mid, mDrawPos.y + up.y * mid, mDrawPos.z + up.z * mid);
}

// (perf) Does this turn need the puppet's joints? Not while hidden; not off screen (the drawer leaves it out; its shadow, name
// tag and sensors follow mPosition, which control() keeps current); not on a turn that draws no picture (30 fps mode: the next
// turn's picture computes them from that turn's pose, exact); and a far puppet on every other picture. The first picture after
// any of those (mStale) is always computed. The self puppet is the player: only the no-picture rule applies.
bool MpPuppetBase::lodWantsCalc() {
    mCulled = false;
    bool half = false;   // MP3 perf3: the quality governor's half-rate rules (levels 2, 3)
    int period = 1;      // MP3 lod2: joints every `period` pictures by distance (level 0 too)
    u8 phase = 0;
    if (!mSelf) {
        if (mHidden) {
            mCulled = true;
            mStale = true;
            return false;
        }
        TVec3f center;
        lodCenter(&center);   // MP3 formation: where it is drawn
        TVec3f cam = MR::getCamPos();
        f32 dx = center.x - cam.x, dy = center.y - cam.y, dz = center.z - cam.z;
        f32 d2 = dx * dx + dy * dy + dz * dz;
        const MpQuality::LevelParams& q = MpQuality::params();
        mFar = mFar ? d2 > q.farOut * q.farOut : d2 > q.farIn * q.farIn;
        half = MpQuality::halfRate(mSlot, d2);
        const bool lod = MpLod::enabled();
        const MpLod::Slot& L = MpLod::slot(mSlot);
        if (lod && MpLod::tinyOn() && L.tiny) {   // MP3 lod2: a few pixels tall: skipped like an off-screen puppet
            mCulled = true;
            mStale = true;
            MpLod::count(MpLod::C_SKIP_TINY);
            return false;
        }
        if (d2 > cLodNear * cLodNear && MR::isJudgedToClipFrustum(center, cLodRadius * MpLod::cullMargin() * mScale.y)) {
            mCulled = true;
            mStale = true;
            MpLod::count(MpLod::C_SKIP_CULL);
            return false;
        }
        if (lod && MpLod::rateOn()) {
            period = L.period;
            phase = L.phase;
        }
    }
    // wave8: GameSystem::frameLoop() runs draw() BEFORE update() and calcAnim(), so an undrawn turn (port_turn_skipped_raw() != 0)
    // is the exact turn whose pose will be drawn at the start of the NEXT frame! Never skip calcPoseNow() on undrawn turns:
    // doing so froze selfP (Luigi) and remote puppets 1 full turn behind the fast face-first fly-in camera (clipping Luigi's body
    // across the near plane) and added 1-turn lag between mDrawPos (shadows/tags) and drawn geometry at 30 fps.
    if (turnWasSkipped()) {
        sLastSkippedTurn = MpPuppetsInt::turn();
        sSawSkippedTurn = true;
    }
    // At 30 fps (alternate logic turns already skipped above, or a 30 fps frame target), near/mid-range visible puppets
    // (!mFar) already update joints at 30 Hz; skipping alternate drawn pictures would drop them to choppy 15 Hz animation.
    // Keep full drawn-picture cadence unless truly far away (mFar), preserving 4P savings for distant puppets.
    // Cache mp_frame_target_us() (refreshed every 30 turns) to avoid per-puppet getenv() scans at 60 fps.
    static bool sTarget30Init = false;
    static bool sTarget30 = false;
    static u32 sTarget30Turn = 0;
    const u32 curTurn = MpPuppetsInt::turn();
    if (!sTarget30Init || (u32)(curTurn - sTarget30Turn) >= 30u) {
        sTarget30Init = true;
        sTarget30Turn = curTurn;
        sTarget30 = mp_frame_target_us() >= 30000u;
    }
    const bool local30 = (sSawSkippedTurn && (u32)(curTurn - sLastSkippedTurn) <= 3u) || sTarget30;
    if (local30 && !mFar) {
        period = 1;
        half = false;
    }
    if (period > 1) {
        // MP3 lod2: the distance band's period (2..4), staggered; a far / governed puppet is at least every other picture
        if ((mFar || half) && period < 2) {
            period = 2;
        }
        if (mAnimated && !mStale && MpLod::skipPicture(MpPuppetsInt::pictures(), phase, period)) {
            MpLod::count(MpLod::C_SKIP_RATE);
            return false;
        }
    } else if ((mFar || half) && mAnimated && !mStale && ((MpPuppetsInt::pictures() + (u32)mSlot) & 1) != 0) {
        MpLod::count(MpLod::C_SKIP_RATE);
        return false;
    }
    mStale = false;
    if (mLightSkipped) {   // MP3 lod2: back in view: the light control catches up at once
        mLightSkipped = false;
        if (mActorLightCtrl != nullptr && usesDrawer() && !mHidden) {
            mActorLightCtrl->update(true);
        }
    }
    MpLod::count(MpLod::C_CALC);
    return true;
}

void MpPuppetBase::calcAndSetBaseMtx() {
    MtxPtr base = MR::getJ3DModel(this)->getBaseTRMtx();
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            base[r][c] = mDrawRotMtx[r][c];   // MP3 formation: the drawn pose (= the true one unless MpFormation moved it)
        }
    }
    MR::setMtxTrans(base, mDrawPos.x, mDrawPos.y, mDrawPos.z);
    MR::getJ3DModel(this)->setBaseScale(mScale);
}

// ---- interaction ---------------------------------------------------------------------------------------------------------
void MpPuppetBase::attackSensor(HitSensor* pSender, HitSensor* pReceiver) {
    if (mSelf || mHidden || !MpPuppetsInt::interactionOn()) {
        return;
    }
    if (interactionHeld(mSlot, mFlags)) {
        return;   // mppause / wave8: fly-in / landing / formation: two players on one spawn point do not push each other
    }
    if (MR::isSensorPlayer(pReceiver) && !pSender->isType(ATYPE_PLAYER_AUTO_JUMP)) {   // (MP3 coop: the head spring never pushes)
        MR::sendMsgPush(pReceiver, pSender);   // bodies push apart gently
    }
}

bool MpPuppetBase::receiveMsgPlayerAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (mSelf || mHidden || !MpPuppetsInt::interactionOn() || interactionHeld(mSlot, mFlags)) {
        return false;
    }
    if (MR::isMsgPlayerTrample(msg)) {
        // MP3 coop: with the head spring live, the body leaves the trample to it (Mario's doTrampleJump bounces by sensor type)
        const bool spring = pReceiver != nullptr && pReceiver->isType(ATYPE_PLAYER_AUTO_JUMP);
        if (!spring && mSpringOn) {
            return false;
        }
        if (mTrampleCooldown != 0) {
            return false;
        }
        mTrampleCooldown = 10;
        MpCoop::noteTrample(mSlot, spring);
        TVec3f dir(mPosition);
        if (pSender != nullptr) {
            dir -= pSender->mPosition;
        }
        MpPuppetsInt::sendBump(mSlot, MP_BUMP_STOMP, dir);
        return true;   // the local player gets the trample bounce
    }
    if (MR::isMsgPlayerSpinAttack(msg)) {
        if (mSpinCooldown != 0) {
            return false;
        }
        mSpinCooldown = 30;
        TVec3f dir(mPosition);
        if (pSender != nullptr) {
            dir -= pSender->mPosition;
        }
        MpPuppetsInt::sendBump(mSlot, MP_BUMP_SPIN, dir);
        return true;   // the spin's hit hop
    }
    return false;      // kicks (walking into us) and everything else: false, or Mario starts a running kick
}

bool MpPuppetBase::receiveMsgPush(HitSensor* pSender, HitSensor* pReceiver) {
    return false;
}

bool MpPuppetBase::receiveMsgEnemyAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    return false;
}

bool MpPuppetBase::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    return false;   // GhostPlayer answers true to everything: unsafe for an actor that stays in the world
}

// ==== MpRemotePlayer (Mario / Luigi / Cosmic Mario) ==========================================================================
MpRemotePlayer::MpRemotePlayer(int slot, u8 character, bool self)
    : MpPuppetBase("MpRemotePlayer", slot, character, self), mUpperActive(false), mCurSimple(false), mHasColorBtp(false), mBtpForm(0xFF),
      mCurAnim(0), mCurUpper(0), mMissHash(0), mJointSpine1(0), mJointPartsControl(0), mXanimePlayer(nullptr),
      mXanimePlayerUpper(nullptr), mShowShapeCount(0), mDLSize(0), mCurDL(0), mDLKey(0), mDLGhost(false), mWarm(false), mWarmGhost(false), mDLAlpha(0xFF) {
    for (int i = 0; i < 4; i++) {
        mTrackHash[i] = 0;
        mTrackRes[i] = nullptr;
    }
    mDL[0] = mDL[1] = nullptr;
    PSMTXIdentity(mMirrorMtx);
}

void MpRemotePlayer::init(const JMapInfoIter& rIter) {
    if (isCosmic()) {
        // GhostPlayer::init: the GhostMario model alone (its archive holds the btk/brk shimmer), Mario's animations come from
        // the player's tables
        initModelManagerWithAnm(modelName(mCharacter), nullptr, false);   // GhostMario / GhostLuigi
    } else {
        // Same model resources as MarioActor::initDrawAndModel (deferred display lists, fog control), so the material lists match.
        initModelManagerWithAnm(mCharacter == MP_LOOK_LUIGI ? "Luigi" : "Mario", "MarioAnime", true);
        MR::initDLMakerFog(this, true);
        MR::newDifferedDLBuffer(this);
    }

    MR::connectToScene(this, MR::MovementType_Player, MR::CalcAnimType_Player, MR::DrawBufferType_None, MR::DrawType_None);

    initActorLightCtrl();
    mActorLightCtrl->init(0, true);   // player light type, direct (no draw buffer: init(-1) would need one)

    initAnimation();

    J3DModelData* data = MR::getJ3DModelData(this);
    s32 spine = findJoint(data, "Spine1");
    s32 parts = findJoint(data, "PartsControl");
    mJointSpine1 = spine >= 0 ? (u16)spine : 0;
    mJointPartsControl = parts >= 0 ? (u16)parts : 0;
    if (spine < 0 || parts < 0) {
        mp_log("puppet slot=%d joints missing spine=%d parts=%d", mSlot, (int)spine, (int)parts);
    }

    if (!isCosmic()) {
        static const char* const cShown[] = {"Face0", "HandL0", "HandR0"};
        for (u32 j = 0; j < sizeof(cShown) / sizeof(cShown[0]); j++) {
            s32 idx = findJoint(data, cShown[j]);
            if (idx < 0) {
                continue;
            }
            J3DJoint* joint = data->mJointTree.mJointNodePointer[idx];
            for (J3DMaterial* mat = joint != nullptr ? joint->mMesh : nullptr; mat != nullptr; mat = mat->mNext) {
                if (mat->mShape != nullptr && mShowShapeCount < SHOW_SHAPES_MAX) {
                    mShowShapes[mShowShapeCount++] = mat->mShape;
                }
            }
        }
    }

    // MarioActor::calcAnim: btp ColorChange frame 0 normal, 1 flying, 2 fire (on the shared materials: set per puppet)
    mHasColorBtp = !isCosmic() && MR::isBtpExist(this, "ColorChange");
    if (mHasColorBtp) {
        MR::startBtpAndSetFrameAndStop(this, "ColorChange", 0.0f);
    }

    initCommon(50.0f, 80.0f, 50.0f);

    mDL[0] = new (0x20) u8[cDLBytes];
    mDL[1] = new (0x20) u8[cDLBytes];

    makeActorDead();
}

void MpRemotePlayer::initAnimation() {
    // Luigi and Cosmic Luigi: the Luigi swap table; Mario and Cosmic Mario: none
    XanimeResourceTable* table = MpPuppetsInt::xanimeTable(usesLuigiSwap());

    mXanimePlayer = new XanimePlayer(MR::getJ3DModel(this), table);
    mXanimePlayer->duplicateSimpleGroup();   // else the simple-group bcks overwrite the local player's (GhostPlayer does the same)
    mModelManager->mXanimePlayer = mXanimePlayer;
    mXanimePlayer->setDefaultAnimation(cBasicAnim);
    mXanimePlayer->changeAnimation(cBasicAnim);
    mXanimePlayer->getCore()->enableJointTransform(MR::getJ3DModelData(this));

    mXanimePlayerUpper = new XanimePlayer(MR::getJ3DModel(this), table, mXanimePlayer);
    mXanimePlayerUpper->duplicateSimpleGroup();
    mXanimePlayerUpper->changeAnimation(cBasicAnim);
    mXanimePlayerUpper->mCore->shareJointTransform(mXanimePlayer->mCore);
}

void MpRemotePlayer::appear() {
    MpPuppetBase::appear();
    mCurAnim = 0;
    mCurUpper = 0;
    mUpperActive = false;
    for (int i = 0; i < 4; i++) {
        mTrackHash[i] = 0;
        mTrackRes[i] = nullptr;
    }
    mBtpForm = 0xFF;
    if (isCosmic()) {
        MR::startBtk(this, modelName(mCharacter));   // GhostPlayer::appear: the shimmer ("GhostMario" / "GhostLuigi")
        MR::startBrk(this, modelName(mCharacter));
    }
}

void MpRemotePlayer::setFrame(XanimePlayer* pPlayer, f32 frame) const {
    XanimeFrameCtrl* ctrl = pPlayer->getFrameCtrl();
    if (ctrl == nullptr) {
        return;
    }
    // Hold an ended one-shot animation on its last frame (never let the frame control snap back to the start).
    f32 end = ctrl->getEnd();
    if (end > 0.0f && frame > end) {
        frame = end;
    }
    if (frame < 0.0f) {
        frame = 0.0f;
    }
    ctrl->setFrame(frame);
    // calcAnm poses _84 while _88 is set (XanimePlayer::updateAfterMovement already advanced the control this turn)
    pPlayer->_84 = frame;
    pPlayer->_88 = true;
}

void MpRemotePlayer::applyAnimation(const MpPoseF& p) {
    const MpAnimNameMap& names = MpPuppetsInt::names();
    const bool simple = (p.flags & MP_POSE_SIMPLE_BCK) != 0;

    // ---- lower body
    if (p.anim != 0 && (p.anim != mCurAnim || simple != mCurSimple)) {
        bool ok = false;
        if (simple) {
            const char* name = mapCharacterBck(names.find(p.anim), usesLuigiSwap());
            ok = name != nullptr && mXanimePlayer->changeSimpleBck(name);
        } else {
            mXanimePlayer->changeAnimationByHash(p.anim);
            const XanimeGroupInfo* cur = mXanimePlayer->mCurrentAnimation;
            ok = cur != nullptr && !mXanimePlayer->isAnimationRunSimple() && cur->mHash == p.anim;
        }
        if (ok) {
            mCurAnim = p.anim;
            mCurSimple = simple;
            if (simple) {
                // wave8: changeAnimationSimple() calls mCore->doFreeze() (_28=1) and sets _08=0, _20->_14=1. Because selfP calls
                // step() inside calcAnim() (after LiveActor::movement()), snap simple BCK transitions (SpaceFlyStart/Loop/End,
                // Fall, SpaceWait) immediately so the first frame of a fly-in animation never freezes on the prior pose.
                mXanimePlayer->changeInterpoleFrame(0);
                if (XanimeCore* c = mXanimePlayer->getCore()) {
                    c->_1C = 1.0f;
                    c->_20 = 1.0f;
                    c->_28 = 0;
                    c->_29 = 0;
                }
            }
            for (int i = 0; i < 4; i++) {
                mTrackHash[i] = 0;
                mTrackRes[i] = nullptr;
            }
        } else if (mMissHash != p.anim) {
            mMissHash = p.anim;   // unknown: keep the previous animation
            mp_log("puppet slot=%d anim miss hash=%08x simple=%d", mSlot, (unsigned)p.anim, simple ? 1 : 0);
        }
    }

    if (p.anim != 0 && p.anim == mCurAnim) {
        mXanimePlayer->changeSpeed(p.rate);
        setFrame(mXanimePlayer, p.frame);

        const XanimeGroupInfo* cur = mXanimePlayer->mCurrentAnimation;
        XanimeCore* core = mXanimePlayer->getCore();
        u32 variant = cur != nullptr ? cur->mBckTableVariant : 0;
        if (variant > 4) {
            variant = 4;
        }
        if (core != nullptr && variant > core->mTrackCount) {
            variant = core->mTrackCount;
        }

        // track-swapped bcks (slope idle, ...) by resource-name hash
        if (p.hasTracks && core != nullptr && !simple) {
            for (u32 i = 0; i < variant; i++) {
                if (p.tracks[i] == 0) {
                    continue;
                }
                if (p.tracks[i] != mTrackHash[i]) {
                    mTrackHash[i] = p.tracks[i];
                    const char* name = mapCharacterBck(names.find(p.tracks[i]), usesLuigiSwap());
                    ResTable* motions = mXanimePlayer->mResourceTable != nullptr && mXanimePlayer->mResourceTable->mResourceHolder != nullptr ?
                                            mXanimePlayer->mResourceTable->mResourceHolder->mMotionResTable : nullptr;   // (puppetprep: null-safe)
                    mTrackRes[i] = (name != nullptr && motions != nullptr && motions->isExistRes(name)) ? static_cast< J3DAnmTransform* >(motions->getRes(name)) : nullptr;
                    if (mTrackRes[i] == nullptr) {
                        MpPuppetsInt::logOnce(mSlot, "track miss", p.tracks[i]);
                    }
                }
                if (mTrackRes[i] != nullptr && core->mTrackList[i]._0 != mTrackRes[i]) {
                    core->setBck(i, mTrackRes[i]);
                }
            }
        }

        for (u32 i = 0; i < 4; i++) {
            if (mXanimePlayer->changeTrackWeight(i, p.weights[i]) && core != nullptr && i < core->mTrackCount) {
                core->setWeight((u8)i, p.weights[i]);
            }
        }
    }

    // ---- upper body
    if (p.upperAnim != 0) {
        if (p.upperAnim != mCurUpper) {
            mXanimePlayerUpper->changeAnimationByHash(p.upperAnim);
            const XanimeGroupInfo* cur = mXanimePlayerUpper->mCurrentAnimation;
            if (cur != nullptr && cur->mHash == p.upperAnim) {
                mCurUpper = p.upperAnim;
            } else if (mMissHash != p.upperAnim) {
                mMissHash = p.upperAnim;
                mp_log("puppet slot=%d anim miss hash=%08x upper=1", mSlot, (unsigned)p.upperAnim);
            }
        }
        mUpperActive = mCurUpper == p.upperAnim;
        if (mUpperActive) {
            mXanimePlayerUpper->updateBeforeMovement();
            mXanimePlayerUpper->updateAfterMovement();
            setFrame(mXanimePlayerUpper, p.upperFrame);
        }
    } else {
        mUpperActive = false;
    }

    if (mHasColorBtp && mForm != mBtpForm) {
        mBtpForm = mForm;
        const int btp = mp_form_look(mForm, mCharacter, 0).btpFrame;
        f32 frame = btp < 0 ? 0.0f : (f32)btp;
        MR::startBtpAndSetFrameAndStop(this, "ColorChange", frame);
    }
}

// MarioAnimator::calc for a puppet: lower body on joint 0, upper body from Spine1 (with PartsControl), two model passes.
void MpRemotePlayer::calcAnim() {
    if (MR::isDead(this) || mFlag.mIsNoCalcAnim || mXanimePlayer == nullptr) {
        return;
    }
    MP_PROF_SCOPE(MP_PROF_TOTAL);
    MP_PROF_SCOPE(MP_PROF_PUPPET_CALC);
    if (mSelf) {
        step();   // the local Mario's movement and calcAnim ran before us this turn: the pose is current
    }
    if (!lodWantsCalc()) {
        MP_PROF_COUNT(3);
        return;
    }
    MP_PROF_COUNT(2);
    calcPoseNow();
}

// The joints of the pose already applied (the body of calcAnim after the level-of-detail test; MpWarm calls it on a dead puppet).
void MpRemotePlayer::calcPoseNow() {
    calcAndSetBaseMtx();

    XanimeCore* core = mXanimePlayer->mCore;
    XjointTransform* joint0 = core->getJointTransform(0);
    if (joint0 != nullptr) {
        joint0->_38.zero();
        joint0->_64 = nullptr;   // (root flight/correction orientation from _E3C is already baked into base via MpLocalCapture)
        if (mMirror) {
            joint0->_14.set(1.0f, 1.0f, -1.0f);
            MtxPtr base = MR::getJ3DModel(this)->getBaseTRMtx();
            Mtx inv;
            PSMTXInverse(base, inv);
            MR::multMtx(mMirrorMtx, inv, MR::tmpMtxRotYRad(3.14159265f));
            MR::multMtx(mMirrorMtx, mMirrorMtx, base);
            joint0->_6C = mMirrorMtx;
        } else {
            joint0->_14.set(1.0f, 1.0f, 1.0f);
            joint0->_6C = nullptr;
        }
    }

    J3DModelData* data = MR::getJ3DModelData(this);
    if (mUpperActive) {
        mXanimePlayer->calcAnm(0);
        mXanimePlayerUpper->calcAnm(mJointSpine1);
        mXanimePlayerUpper->overWriteMtxCalc(mJointPartsControl);
    } else {
        data->mJointTree.mJointNodePointer[mJointSpine1]->setMtxCalc(nullptr);
        mXanimePlayer->calcAnm(0);
    }

    // MP3 lod2: one model pass when no blend runs (MpLodLogic.hpp onePassOk): _6 = 0 is the ordinary calc, which for a core at rest
    // computes what the blend pass (_6 = 1) and the final pass (_6 = 2) compute together. SMG_MP_LOD2_CHECK=1 proves it on the desktop.
    bool onePass = false;
    bool blend = true;
    if (!mWarm && MpLod::enabled() && MpLod::onePassOn()) {
        blend = blendRunning();
        TVec3f c, cam = MR::getCamPos();
        lodCenter(&c);
        const f32 dx = c.x - cam.x, dy = c.y - cam.y, dz = c.z - cam.z;
        onePass = MpLod::onePassOk(blend, dx * dx + dy * dy + dz * dz);
    }
    if (onePass && !blend && MpLod::checkMode() != 0) {
        checkPasses();   // (leaves the state as the two passes made it: the check never changes what is drawn)
    } else if (onePass) {
        core->_6 = 0;
        MR::calcJ3DModel(this);
        MpLod::count(MpLod::C_ONE_PASS);
    } else {
        core->_6 = 1;
        calcJointsOnly(this);   // MP3 profile: the blend pass's joints (its envelope matrices would be overwritten by the next pass)
        core->_6 = 2;
        MR::calcJ3DModel(this);
        MpLod::count(MpLod::C_TWO_PASS);
    }
    mXanimePlayer->clearAnm(0);

    if (mUpperActive) {
        mXanimePlayer->clearAnm(mJointSpine1);
        mXanimePlayerUpper->clearMtxCalc(mJointPartsControl);
    }
    // joints are shared with the local Mario's model data when we draw the same character: leave no calc pointer behind
    data->mJointTree.mJointNodePointer[mJointSpine1]->setMtxCalc(nullptr);

    // wave8: under GXV_DL_LAZY=2, MR::updateModelDiffDL() defers to next drawn turn's flush. Force an immediate build during
    // warmDraw() (already inside draw()) or whenever any shape packet's differed display list is still empty (mSize == 0).
    bool needImmediateDL = mWarm;
    if (!needImmediateDL && !isCosmic()) {
        J3DModel* mdl = MR::getJ3DModel(this);
        const u16 sn = data->getShapeNum();
        for (u16 i = 0; i < sn; i++) {
            J3DShapePacket* pkt = mdl->getShapePacket(i);
            if (pkt != nullptr && pkt->getDisplayListObj() != nullptr && pkt->getDisplayListObj()->mSize == 0) {
                needImmediateDL = true;
                break;
            }
        }
    }
    if (needImmediateDL && mModelManager != nullptr) {
        mModelManager->updateDL(true);
    } else {
        MR::updateModelDiffDL(this);
    }
    mAnimated = true;
}

// MP3 lod2: no blend is running: the lower core at rest (_1C 1, _20 1, no freeze, every joint's _5C 1) and the upper one too.
// (The upper core is never in the blend pass: it calculates normally in both passes, so its own blend would be applied twice.)
bool MpRemotePlayer::blendRunning() const {
    XanimeCore* core = mXanimePlayer->mCore;
    if (core == nullptr || core->_1C != 1.0f || core->_20 != 1.0f || core->_29 != 0) {
        return true;
    }
    XjointInfo* list = core->mJointList;
    for (u32 i = 0; i < core->mJointCount; i++) {
        if (list[i]._5C != 1.0f) {
            return true;
        }
    }
    if (mUpperActive) {
        XanimeCore* up = mXanimePlayerUpper->mCore;
        if (up == nullptr || up->_1C != 1.0f || up->_20 != 1.0f || up->_29 != 0) {
            return true;
        }
    }
    return false;
}

#if !defined(__vita__)   // (TARGET_PC is also defined in the Vita build)
namespace {
    // the self-check's buffers (desktop only: SMG_MP_LOD2_CHECK=1): one model's matrices after each kind of pass, and the joint state
    const u32 cCheckJoints = 160;
    const u32 cCheckEvlp = 160;
    XjointInfo sSaveJoints[cCheckJoints], sOneJoints[cCheckJoints];
    f32 sOneAnm[cCheckJoints][3][4], sOneEvlp[cCheckEvlp][3][4];
    u8 sOneScale[cCheckJoints];

    void compareFloats(const f32* a, const f32* b, u32 n, int* diffs, f32* maxAbs) {
        for (u32 i = 0; i < n; i++) {
            u32 x, y;
            memcpy(&x, &a[i], 4);
            memcpy(&y, &b[i], 4);
            if (x != y) {
                (*diffs)++;
                const f32 d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
                if (d == d && d > *maxAbs) {
                    *maxAbs = d;
                }
            }
        }
    }
}  // namespace
#endif

// MP3 lod2: SMG_MP_LOD2_CHECK=1. The one-pass result is computed on the real state, saved with the joint state, the joint state is put
// back and the two passes run for real; both outputs (anm and weighted envelope matrices, scale flags, the joints' blended pose)
// are compared bit for bit and the result goes to the "[mp] lod2" line / a log line on the first differences.
void MpRemotePlayer::checkPasses() {
#if defined(__vita__)
    // (optfix) the check's buffers (47 KB of bss) are desktop only: on the console this is just the ordinary two passes
    XanimeCore* core = mXanimePlayer->mCore;
    core->_6 = 1;
    calcJointsOnly(this);
    core->_6 = 2;
    MR::calcJ3DModel(this);
#else
    XanimeCore* core = mXanimePlayer->mCore;
    J3DModel* m = MR::getJ3DModel(this);
    J3DModelData* data = MR::getJ3DModelData(this);
    const u32 nj = data->getJointNum(), ne = data->getWEvlpMtxNum();
    if (core->mJointCount > cCheckJoints || nj > cCheckJoints || ne > cCheckEvlp || core->mJointCount != nj) {
        core->_6 = 1;
        calcJointsOnly(this);
        core->_6 = 2;
        MR::calcJ3DModel(this);
        return;
    }
    memcpy(sSaveJoints, core->mJointList, core->mJointCount * sizeof(XjointInfo));
    core->_6 = 0;
    MR::calcJ3DModel(this);
#ifdef TARGET_PC
    if (gPortParViewOpen) {
        port_par_view_touch(m->mMtxBuffer);
    }
#endif
    J3DMtxBuffer* mb = m->mMtxBuffer;
    for (u32 i = 0; i < nj; i++) {
        memcpy(sOneAnm[i], mb->getAnmMtx((int)i), sizeof(sOneAnm[i]));
        sOneScale[i] = mb->getScaleFlag((int)i);
    }
    for (u32 i = 0; i < ne; i++) {
        memcpy(sOneEvlp[i], mb->getWeightAnmMtx((int)i), sizeof(sOneEvlp[i]));
    }
    memcpy(sOneJoints, core->mJointList, core->mJointCount * sizeof(XjointInfo));
    memcpy(core->mJointList, sSaveJoints, core->mJointCount * sizeof(XjointInfo));   // (back to the state before the passes)
    core->_6 = 1;
    calcJointsOnly(this);
    core->_6 = 2;
    MR::calcJ3DModel(this);
#ifdef TARGET_PC
    if (gPortParViewOpen) {
        port_par_view_touch(m->mMtxBuffer);
    }
#endif
    int diffs = 0;
    f32 maxAbs = 0.0f;
    for (u32 i = 0; i < nj; i++) {
        compareFloats(&sOneAnm[i][0][0], &mb->getAnmMtx((int)i)[0][0], 12, &diffs, &maxAbs);
        if (sOneScale[i] != mb->getScaleFlag((int)i)) {
            diffs++;
        }
    }
    for (u32 i = 0; i < ne; i++) {
        compareFloats(&sOneEvlp[i][0][0], &mb->getWeightAnmMtx((int)i)[0][0], 12, &diffs, &maxAbs);
    }
    compareFloats(reinterpret_cast< const f32* >(sOneJoints), reinterpret_cast< const f32* >(core->mJointList),
                  core->mJointCount * (u32)(sizeof(XjointInfo) / sizeof(f32)), &diffs, &maxAbs);
    MpLod::checkNote(diffs == 0, diffs, maxAbs);
#endif  // __vita__
}

void MpRemotePlayer::buildDL() const {
    // (perf) the list depends only on the style and two colours: rebuilt when one of them changes, not on every draw.
    // A drop-in's arrival uses the see-through list with its fade-in alpha (the ghost list of Star Race at 0x80).
    const bool raceGhost = mWarmGhost || (!mSelf && (MpPuppetsInt::style() == MP_PUPPET_GHOST || mStandIn));   // (a stand-in is see-through too)
    const u8 fade = arrivalAlpha();
    const u8 formAlpha = mp_form_look(mForm, mCharacter, 0).alpha;   // (puppetprep) a Boo is see-through
    const bool ghost = raceGhost || fade != 0xFF || formAlpha != 0xFF;
    u8 alphaValue = fade < formAlpha ? fade : formAlpha;
    if (raceGhost && alphaValue > 0x80) alphaValue = 0x80;
    u64 key = ghost ? (u64)alphaValue : 0;   // (the ghost list has no colour in it, only its alpha)
    if (!ghost) {
        GXColor amb = *MR::getLightAmbientColor(this);
        GXColor mat = formTint(mForm, mRainbow, mCharacter);
        key = (u64)amb.r << 56 | (u64)amb.g << 48 | (u64)amb.b << 40 | (u64)amb.a << 32 | (u64)mat.r << 24 | (u64)mat.g << 16 |
              (u64)mat.b << 8 | (u64)mat.a;
    }
    if (mDLSize != 0 && key == mDLKey && ghost == mDLGhost) {
        return;
    }
    mDLKey = key;
    mDLGhost = ghost;
    mDLAlpha = alphaValue;
    int next = 1 - mCurDL;
    u8* buf = mDL[next];
    if (buf == nullptr) {
        return;
    }
    GDLObj dl;
    MR::ProhibitSchedulerAndInterrupts lock(false);
    GDInitGDLObj(&dl, buf, cDLBytes);
    GDLObj* prev = __GDCurrentDL;
    __GDCurrentDL = &dl;
    if (ghost) {
        // MarioActor::updateAlphaDL (first-person see-through Mario) at half alpha
        GXColor alpha = {0, 0, 0, alphaValue};
        GDSetTevColor(GX_TEVREG2, alpha);
        GDSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
        GDSetGenMode2(1, 0, 1, 0, GX_CULL_BACK);
        GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
        GDSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
        GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                                 GX_TEVPREV, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GDSetTevColorCalc(GX_TEVSTAGE0, GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE,
                          GX_TEVPREV);
        GDSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
    } else {
        // MarioActor::updateLightDL without the damage fog; the material colour carries the form tint
        GXColor amb = *MR::getLightAmbientColor(this);
        GXColor mat = formTint(mForm, mRainbow, mCharacter);
        GDSetChanAmbColor(GX_COLOR0A0, amb);
        GDSetChanMatColor(GX_COLOR0A0, mat);
        GDSetChanCtrl(GX_COLOR1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_CLAMP, GX_AF_NONE);
    }
    GDPadCurr32();
    u32 len = (u32)(dl.ptr - dl.start);
    __GDCurrentDL = prev;
    DCStoreRange(buf, (len + 31) & ~31u);
    mDLSize = len;
    mCurDL = next;
}

// MpWarm: one draw of this puppet (dead spare, already-alive remote, or self puppet) cleanly inside the frustum at tiny 0.001 scale
// behind the closed opening wipe, so its model's textures and both solid / ghost shader variants pass StaticRunCulled and reach the
// back end before the first visible picture. Every mutated field (and for alive puppets, the model matrices and light) is restored.
void MpRemotePlayer::warmDraw(int variant) {
    if (mXanimePlayer == nullptr || mActorLightCtrl == nullptr) {
        return;
    }
    // review2 #2: the base matrix reads the DRAWN pose (mDrawPos / mDrawRotMtx): set that one, keep the true pose, restore everything
    const TVec3f savedScale = mScale;
    const TVec3f savedDrawPos = mDrawPos;
    Mtx savedDrawRot;
    PSMTXCopy(mDrawRotMtx, savedDrawRot);
    const bool savedMirror = mMirror;
    const bool savedUpper = mUpperActive;
    const u8 savedForm = mForm;
    const bool savedAnimated = mAnimated;
    // Read-only J3D matrix rebuild from existing mJointList[j]._28 (core->_6 = 2 calcScaleBlendSpecial only).
    // Never calls updateFrame() or calcBlendSpecial(), so an already-alive puppet's XanimeCore blend state is untouched.
    auto recomputeReadOnly = [&]() {
        calcAndSetBaseMtx();
        XanimeCore* core = mXanimePlayer->mCore;
        XjointTransform* joint0 = core->getJointTransform(0);
        if (joint0 != nullptr) {
            joint0->_38.zero();
            joint0->_64 = nullptr;
            if (mMirror) {
                joint0->_14.set(1.0f, 1.0f, -1.0f);
                MtxPtr base = MR::getJ3DModel(this)->getBaseTRMtx();
                Mtx inv;
                PSMTXInverse(base, inv);
                MR::multMtx(mMirrorMtx, inv, MR::tmpMtxRotYRad(3.14159265f));
                MR::multMtx(mMirrorMtx, mMirrorMtx, base);
                joint0->_6C = mMirrorMtx;
            } else {
                joint0->_14.set(1.0f, 1.0f, 1.0f);
                joint0->_6C = nullptr;
            }
        }
        J3DModelData* data = MR::getJ3DModelData(this);
        const bool useUpper = mUpperActive && mXanimePlayerUpper != nullptr && mXanimePlayerUpper->mCore != nullptr;
        XanimeCore* upperCore = useUpper ? mXanimePlayerUpper->mCore : nullptr;
        u8 savedUpperMode = 0;
        if (useUpper) {
            savedUpperMode = upperCore->_6;
            upperCore->_6 = 2;
            data->mJointTree.mJointNodePointer[mJointSpine1]->setMtxCalc(upperCore);
            mXanimePlayerUpper->overWriteMtxCalc(mJointPartsControl);
        } else {
            data->mJointTree.mJointNodePointer[mJointSpine1]->setMtxCalc(nullptr);
        }
        data->mJointTree.mJointNodePointer[0]->setMtxCalc(core);
        const u8 savedMode = core->_6;
        core->_6 = 2;
        MR::calcJ3DModel(this);
        core->_6 = savedMode;
        data->mJointTree.mJointNodePointer[0]->setMtxCalc(nullptr);
        if (useUpper) {
            upperCore->_6 = savedUpperMode;
            mXanimePlayerUpper->clearMtxCalc(mJointPartsControl);
        }
        data->mJointTree.mJointNodePointer[mJointSpine1]->setMtxCalc(nullptr);
        if (mWarm && mModelManager != nullptr) {
            mModelManager->updateDL(true);
        } else {
            MR::updateModelDiffDL(this);
        }
    };
    const bool keepAlivePose = !MR::isDead(this) && savedAnimated;
    const TVec3f cam = MR::getCamPos();
    const TVec3f fwd = MR::getCamZdir();
    const MpWarmPose wp = mp_warm_pose(cam.x, cam.y, cam.z, fwd.x, fwd.y, fwd.z, MR::getNearZ());
    setDrawPose(TVec3f(wp.pos[0], wp.pos[1], wp.pos[2]), wp.rot);
    mScale.set(wp.scale, wp.scale, wp.scale);
    mMirror = false;
    mUpperActive = false;
    mForm = MP_FORM_NORMAL;
    mWarm = true;
    mWarmGhost = variant == 1;
    if (keepAlivePose) {
        recomputeReadOnly();
    } else {
        calcPoseNow();
    }
    mActorLightCtrl->update(true);
    MR::loadActorLight(this);
    drawPuppet();
    mWarmGhost = false;
    mScale = savedScale;
    mDrawPos = savedDrawPos;
    PSMTXCopy(savedDrawRot, mDrawRotMtx);
    mMirror = savedMirror;
    mUpperActive = savedUpper;
    mForm = savedForm;
    mDLSize = 0;   // (rebuilt for the real draw: the key test needs a list)
    mDLKey = 0;
    if (keepAlivePose) {
        recomputeReadOnly();
        mActorLightCtrl->update(true);
    } else {
        mAnimated = savedAnimated;
    }
    mWarm = false;
}

void MpRemotePlayer::drawPuppet() const {
    if (!mWarm && !isDrawable()) {
        return;
    }
    J3DModelX* model = static_cast< J3DModelX* >(MR::getJ3DModel(this));

    // wave8: reload the active camera view matrix on normal draw turns in case an earlier custom drawer in DrawType_Player /
    // fly-in (SpinDriverPathDrawer, MiiFacePartsHolder, etc.) left j3dSys.mViewMtx dirty; save/restore so portHrwFresh() keeps sPortFreshView.
    Mtx savedView;
    PSMTXCopy(j3dSys.mViewMtx, savedView);
    if (!turnWasSkipped()) {
        PSMTXCopy(MR::getCameraViewMtx(), j3dSys.mViewMtx);
    }
    model->viewCalc2();
    GXInvalidateVtxCache();
    model->mFlags.clear();
    if (mMirror) {
        *reinterpret_cast< u32* >(&model->mFlags) |= 1;   // cull front (MarioAnimator::switchMirrorMode)
    }
    const bool raceGhost = mWarmGhost || (!mSelf && (MpPuppetsInt::style() == MP_PUPPET_GHOST || mStandIn));
    if (isCosmic() && !raceGhost && arrivalAlpha() == 0xFF) {
        model->setDynamicDL(nullptr, 0);   // GhostPlayer::draw: the model's own materials (shimmer btk/brk), solid
    } else {
        buildDL();
        model->setDynamicDL(mDL[mCurDL], mDLSize);
    }

    // wave8: un-hide all non-trimmed shapes (not just Face0/HandL0/HandR0) for this draw and restore afterwards so Body1_v
    // can never be skipped by a stale shape flag on shared model data; keep MpLod::trimOn() for far remote face/hands.
    const bool trim = !mSelf && MpLod::trimOn() && MpLod::slot(mSlot).trim;
    J3DModelData* data = model->getModelData();
    const int nShapes = (!isCosmic() && data != nullptr) ? (int)(data->getShapeNum() < SHOW_SHAPES_MAX ? data->getShapeNum() : SHOW_SHAPES_MAX) : 0;
    u32 saved[SHOW_SHAPES_MAX];
    bool touched[SHOW_SHAPES_MAX];
    for (int i = 0; i < nShapes; i++) {
        J3DShape* shp = data->getShapeNodePointer((u16)i);
        bool isTrimShape = false;
        if (trim && shp != nullptr) {
            for (int k = 0; k < mShowShapeCount; k++) {
                if (mShowShapes[k] == shp) {
                    isTrimShape = true;
                    break;
                }
            }
        }
        if (shp != nullptr && !isTrimShape) {
            saved[i] = shp->mFlags;
            shp->mFlags &= ~1u;
            touched[i] = true;
        } else {
            touched[i] = false;
        }
    }
    // wave8: reset J3D's VCD/VAT cache before directDraw() so custom GX drawers (SpinDriverPathDrawer during fly-ins)
    // can never leave a stale VCD active when Luigi's skinned Body1_v (GX_VA_PNMTXIDX) is drawn.
    J3DShape::resetVcdVatCache();
    model->directDraw(nullptr);
    for (int i = 0; i < nShapes; i++) {
        if (touched[i]) {
            data->getShapeNodePointer((u16)i)->mFlags = saved[i];
        }
    }

    model->mFlags.clear();
    PSMTXCopy(savedView, j3dSys.mViewMtx);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_FALSE, 0);
}
