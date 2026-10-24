#include "Game/Enemy/WalkerStateChase.hpp"
#include "Game/Enemy/WalkerStateFunction.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"

namespace {
    WalkerStateChaseParam sDefaultParam;
};  // namespace

namespace NrvWalkerStateChase {
    NEW_NERVE(WalkerStateChaseNrvStart, WalkerStateChase, Start);
    NEW_NERVE(WalkerStateChaseNrvEnd, WalkerStateChase, End);
};  // namespace NrvWalkerStateChase

WalkerStateChaseParam::WalkerStateChaseParam() : mChaseTime(130), mForceChaseEndTime(300), mTurnMaxRateDegree(2.0f), mChaseEndWaitTime(30) {
}

WalkerStateChase::WalkerStateChase(LiveActor* pHost, TVec3f* pDirection, WalkerStateParam* pStateParam, WalkerStateChaseParam* pChaseParam)
    : ActorStateBase< LiveActor >("\x83\x4e\x83\x8a\x83\x7b\x81\x5b\x92\xc7\x82\xa2\x82\xa9\x82\xaf\x8f\xf3\x91\xd4", pHost), mStateParam(pStateParam), mChaseParam(pChaseParam), mDirection(pDirection) {
    if (mChaseParam == nullptr) {
        mChaseParam = &::sDefaultParam;
    }

    initNerve(GET_NERVE(WalkerStateChase, WalkerStateChaseNrvStart));
}

void WalkerStateChase::appear() {
    mIsDead = false;
    setNerve(GET_NERVE(WalkerStateChase, WalkerStateChaseNrvStart));
}

void WalkerStateChase::exeStart() {
    if (MR::isFirstStep(this)) {
        MR::startAction(getHost(), "Run");
    }

    bool isInSight = WalkerStateFunction::isInSightPlayer(getHost(), *mDirection, mStateParam);
    if (isInSight) {
        MR::turnDirectionToTargetUseGroundNormalDegree(getHost(), mDirection, *MR::getPlayerPos(), mChaseParam->mTurnMaxRateDegree);
    }

    MR::addVelocityMoveToDirection(getHost(), *mDirection, mChaseParam->mChaseSpeed);
    WalkerStateFunction::calcPassiveMovement(getHost(), mStateParam);

    if (MR::isFallNextMove(getHost(), 150.0f, 150.0f, 150.0f, nullptr)) {
        MR::zeroVelocity(getHost());
        setNerve(GET_NERVE(WalkerStateChase, WalkerStateChaseNrvEnd));
    } else if (MR::isGreaterStep(this, mChaseParam->mForceChaseEndTime) || (MR::isGreaterStep(this, mChaseParam->mChaseTime) && !isInSight)) {
        setNerve(GET_NERVE(WalkerStateChase, WalkerStateChaseNrvEnd));
    }
}

void WalkerStateChase::exeEnd() {
    if (MR::isFirstStep(this)) {
        MR::startAction(getHost(), "Wait");
    }

    WalkerStateFunction::calcPassiveMovement(getHost(), mStateParam);
    if (MR::isGreaterStep(this, mChaseParam->mChaseEndWaitTime)) {
        kill();
    }
}

bool WalkerStateChase::isRunning() const {
    return isNerve(GET_NERVE(WalkerStateChase, WalkerStateChaseNrvStart)) && MR::isBindedGround(getHost());
}
