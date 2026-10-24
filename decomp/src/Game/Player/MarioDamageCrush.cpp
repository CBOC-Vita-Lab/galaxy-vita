#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioCrush.hpp"
#include "Game/Player/MarioState.hpp"
#include <revolution/types.h>

bool Mario::requestCrush() {
    _10._18 = true;
    return true;
}

bool Mario::tryCrush() {
    if (mMovementStates._1F) {
        return false;
    }

    if (isInvincible()) {
        return false;
    }

    if (mMovementStates.jumping || !mMovementStates._1) {
        setTrans(mShadowPos, nullptr);
    }

    if (getCurrentStatus() == MarioStatus_Crush) {
        return false;
    }

    mActor->damageDropThrowMemoSensor();
    mActor->resetPlayerModeOnDamage();

    stopJump();
    stopWalk();

    changeStatus(mCrush);
    return true;
}

MarioCrush::MarioCrush(MarioActor* pActor) : MarioState(pActor, MarioStatus_Crush), _12(), mTimer() {
}

bool MarioCrush::close() {
    getPlayer()->mMovementStates._3C = true;
    mActor->mScale.set(1.0f);
    stopAnimation("\x82\xb5\x82\xd1\x82\xea");
    getPlayer()->set41E(120);
    return true;
}

bool MarioCrush::start() {
    changeAnimationNonStop("\x82\xb5\x82\xd1\x82\xea");
    startPadVib("\x83\x7d\x83\x8a\x83\x49[\x82\xb5\x82\xd1\x82\xea]");
    playSound("\x90\xba\x82\xb5\x82\xd1\x82\xea");

    getPlayer()->mMovementStates._3C = true;
    mActor->mScale.set(1.0f, 0.2f, 1.0f);
    startPadVib(3);

    mActor->decLifeLarge();

    if (mActor->mHealth == 0) {
        mActor->forceGameOver();
    }

    mTimer = 180;
    _12 = 0;

    return true;
}

bool MarioCrush::update() {
    if (mTimer != 0) {
        mTimer--;
    }

    if (mTimer == 0) {
        if (_12 != 0) {
            return false;
        }

        _12 = 1;

        if (!getPlayer()->getMovementStates()._1) {
            mTimer = 10;
        } else {
            mTimer = 30;
        }

        if (getPlayer()->getMovementStates()._1) {
            changeAnimation("\x82\xb5\x82\xd1\x82\xea\x89\xf1\x95\x9c");
        }
    }

    if (_12 != 0 && (mActor->isRequestRush() || checkTrgA())) {
        stopAnimation(nullptr);

        if (checkTrgA()) {
            getPlayer()->tryJump();
        }

        return false;
    }

    return true;
}
