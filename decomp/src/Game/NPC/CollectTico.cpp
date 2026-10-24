#include "Game/NPC/CollectTico.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NPC/StrayTico.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EffectUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <JSystem/JMath/JMath.hpp>

namespace NrvCollectTico {
    NEW_NERVE(CollectTicoNrvWait, CollectTico, Wait);
    NEW_NERVE(CollectTicoNrvTryStartDemo, CollectTico, TryStartDemo);
    NEW_NERVE(CollectTicoNrvCompleteDemo, CollectTico, CompleteDemo);
    NEW_NERVE(CollectTicoNrvFlash, CollectTico, Flash);
    NEW_NERVE(CollectTicoNrvAppearPowerStar, CollectTico, AppearPowerStar);
};  // namespace NrvCollectTico

CollectTico::CollectTico(const char* pName) : LiveActor(pName) {
    mStrayTicos = nullptr;
    mTicoNum = 0;
    _A0 = 0;
}

void CollectTico::init(const JMapInfoIter& rIter) {
    MR::connectToSceneNpcMovement(this);
    mTicoNum = MR::getChildObjNum(rIter);
    mStrayTicos = new StrayTico*[mTicoNum];
    for (s32 i = 0; i < mTicoNum; i++) {
        mStrayTicos[i] = new StrayTico("\x82\xcd\x82\xae\x82\xea\x83\x60\x83\x52", this);
        MR::initChildObj(mStrayTicos[i], rIter, i);
    }

    initEffectKeeper(0, "CollectTico", false);
    initSound(2, false);
    initNerve(GET_NERVE(CollectTico, CollectTicoNrvWait));
    if (MR::tryRegisterDemoCast(this, rIter)) {
        MR::registerDemoActionFunctor(this, MR::Functor(this, &CollectTico::startAppearPowerStar), "\x8f\x57\x82\xdf\x83\x60\x83\x52\x83\x58\x83\x5e\x81\x5b\x8f\x6f\x8c\xbb");
        _A0 = 1;
    }

    MR::useStageSwitchWriteA(this, rIter);
    MR::declarePowerStar(this);
    MR::invalidateClipping(this);
    makeActorAppeared();
}

void CollectTico::exeWait() {
    bool needsDemo = true;
    for (s32 i = 0; i < mTicoNum; i++) {
        if (!mStrayTicos[i]->isRescued()) {
            needsDemo = false;
            break;
        }
    }

    if (needsDemo && !MR::isPlayerDead()) {
        MR::requestStartDemo(this, "\x83\x60\x83\x52\x8f\x57\x82\xdf\x83\x52\x83\x93\x83\x76\x83\x8a\x81\x5b\x83\x67", GET_NERVE(CollectTico, CollectTicoNrvCompleteDemo),
                             GET_NERVE(CollectTico, CollectTicoNrvTryStartDemo));
    }
}

void CollectTico::exeCompleteDemo() {
    if (MR::isFirstStep(this)) {
        mPosition.set(*MR::getPlayerPos());
        MR::calcGravity(this);

        _94.scaleAdd(300.0f, -mGravity, mPosition);
        mPosition.set(_94);

        for (s32 i = 0; i < mTicoNum; i++) {
            MR::requestMovementOn(mStrayTicos[i]);
            mStrayTicos[i]->requestCompleteDemo(_94, mGravity, 360.0f * ((f32)i / mTicoNum));
        }
    }

    MR::startLevelSound(this, "SE_SM_LV_STRAYTICO_COMPLETE");
    bool needFlash = true;
    for (s32 i = 0; i < mTicoNum; i++) {
        if (!mStrayTicos[i]->isCompleteDemoEnd()) {
            needFlash = false;
            break;
        }
    }

    if (needFlash) {
        setNerve(GET_NERVE(CollectTico, CollectTicoNrvFlash));
    }
}

void CollectTico::exeFlash() {
    if (MR::isFirstStep(this)) {
        MR::emitEffect(this, "CollectTicoLight");
        MR::startSound(this, "SE_SM_STRAYTICO_FUSION");
    }

    if (MR::isStep(this, 10)) {
        for (s32 i = 0; i < mTicoNum; i++) {
            mStrayTicos[i]->kill();
        }
    }

    if (MR::isStep(this, 60)) {
        if (!_A0) {
            MR::requestAppearPowerStar(this, _94);
            setNerve(GET_NERVE(CollectTico, CollectTicoNrvAppearPowerStar));
        }

        MR::endDemo(this, "\x83\x60\x83\x52\x8f\x57\x82\xdf\x83\x52\x83\x93\x83\x76\x83\x8a\x81\x5b\x83\x67");
        if (MR::isValidSwitchA(this)) {
            MR::onSwitchA(this);
        }
    }
}

void CollectTico::exeAppearPowerStar() {
    if (MR::isFirstStep(this)) {
        for (s32 i = 0; i < mTicoNum; i++) {
            mStrayTicos[i]->noticeEnd();
        }

        kill();
    }
}

s32 CollectTico::calcNoRescuedCount() const {
    s32 count = 0;
    for (s32 i = 0; i < mTicoNum; i++) {
        if (!mStrayTicos[i]->isRescued()) {
            count++;
        }
    }

    return count;
}

void CollectTico::startAppearPowerStar() {
    MR::requestAppearPowerStar(this, _94);
    setNerve(GET_NERVE(CollectTico, CollectTicoNrvAppearPowerStar));
}

void CollectTico::exeTryStartDemo() {
}
