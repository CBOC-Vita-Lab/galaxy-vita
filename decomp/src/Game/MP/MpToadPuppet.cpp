// Galaxy 3.0 multiplayer: Toad puppet (owner: puppet). The Kinopio model (ObjectData/Kinopio.arc) drawn by the engine like the
// Toad NPCs (NPC draw buffer, its own light control), at the NPC's scale: red Toad (colorchange.brk frame 3) or Yellow Toad (4).
// Toad has none of Mario's animations: mp_toad_map (MpPoseMath.hpp, host tested) maps Mario's state onto Toad's clips.
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "port/mp_prof.h"

namespace {
    const f32 cToadScale = 1.2f;        // Kinopio.cpp sDefaultScale
    const s32 cBlendTurns = 5;
}  // namespace

MpToadPuppet::MpToadPuppet(int slot, u8 character, bool self)
    : MpPuppetBase("MpToadPuppet", slot, character == MP_LOOK_YELLOW_TOAD ? (u8)MP_LOOK_YELLOW_TOAD : (u8)MP_LOOK_TOAD, self), mAnim(-1),
      mHop(1.0f), mClassBck(nullptr), mClass(-1) {}

f32 MpToadPuppet::modelScale() const {
    return cToadScale;   // (the hop stretch is applied on top in applyAnimation)
}

void MpToadPuppet::init(const JMapInfoIter& rIter) {
    mScale.set(cToadScale);
    initModelManagerWithAnm("Kinopio", nullptr, false);
    // player movement and calcAnim (after the local Mario: the self puppet reads his pose of this turn), drawn with the NPCs
    MR::connectToScene(this, MR::MovementType_Player, MR::CalcAnimType_Player, MR::DrawBufferType_NPC, MR::DrawType_None);
    MR::initLightCtrl(this);
    initCommon(50.0f * cToadScale, 50.0f * cToadScale, 30.0f * cToadScale);
    if (MR::isBrkExist(this, "ColorChange")) {
        MR::startBrk(this, "ColorChange");
        MR::setBrkFrameAndStop(this, mp_toad_color_frame(mCharacter));
    }
    makeActorDead();
}

void MpToadPuppet::appear() {
    MpPuppetBase::appear();
    mAnim = -1;
    mHop = 1.0f;
    if (MR::isBrkExist(this, "ColorChange")) {
        MR::startBrk(this, "ColorChange");
        MR::setBrkFrameAndStop(this, mp_toad_color_frame(mCharacter));
    }
}

void MpToadPuppet::onHiddenChanged() {
    if (mHidden) {
        MR::hideModelIfShown(this);
    } else {
        MR::showModelIfHidden(this);
    }
}

void MpToadPuppet::applyAnimation(const MpPoseF& p) {
    // Mario's current bck name: simple groups carry it by hash; for a group, its first bck (XanimeGroupInfo::mBckName)
    const char* bck = nullptr;
    if (p.flags & MP_POSE_SIMPLE_BCK) {
        bck = MpPuppetsInt::names().find(p.anim);
    } else if (p.anim != 0) {
        bck = MpPuppetsInt::groupBckName(p.anim);
    }
    if (bck != mClassBck || mClass < 0) {   // (the name pointers live as long as the scene's tables, as this puppet does)
        mClassBck = bck;
        mClass = mp_toad_name_class(bck);
    }
    MpToadOut o = mp_toad_map_class(p, mClass, mAnim);
    const char* name = mp_toad_anim_name(o.anim);
    if (o.anim != mAnim) {
        if (MR::isBckExist(this, name)) {
            MR::startBckWithInterpole(this, name, cBlendTurns);
            if (MR::isBtpExist(this, name)) {
                MR::startBtp(this, name);   // the eyes for this clip
            }
        } else {
            MpPuppetsInt::logOnce(mSlot, "toad clip missing anim", (uint32_t)o.anim);
        }
        mAnim = o.anim;
    }
    if (o.holdFrame >= 0.0f) {
        MR::setBckFrameAndStop(this, o.holdFrame);
    } else {
        MR::setBckRate(this, o.rate);
    }
    mHop = o.hop;
    // (the hop is a stretch along y only)
    if (o.hop != 1.0f) {
        mScale.y *= o.hop;
        mScale.x /= o.hop;
        mScale.z /= o.hop;
    }
}

void MpToadPuppet::calcAnim() {
    if (MR::isDead(this)) {
        return;
    }
    MP_PROF_SCOPE(MP_PROF_TOTAL);         // MP3 perf3: Toad's joints in the perf line as Mario's / Luigi's are
    MP_PROF_SCOPE(MP_PROF_PUPPET_CALC);
    if (mSelf) {
        step();   // after the local Mario's calcAnim: his pose of this turn
    }
    // (perf) the engine draws Toad (NPC draw buffer): off screen, the model is hidden for the draw as well as not calculated
    const bool calc = lodWantsCalc();
    if (!mHidden) {
        if (mCulled) {
            MR::hideModelIfShown(this);
        } else {
            MR::showModelIfHidden(this);
        }
    }
    if (!calc) {
        return;
    }
    LiveActor::calcAnim();   // calcAndSetBaseMtx (ours) + the model and its material animations
    mAnimated = true;
}
