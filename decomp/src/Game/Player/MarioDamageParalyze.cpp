#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioParalyze.hpp"
#include "Game/Player/MarioState.hpp"
#include <revolution/types.h>

bool Mario::doParalyze() {
    if (mMovementStates._1F) {
        return false;
    }

    if (isInvincible()) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_Paralyze) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_FireDamage) {
        return false;
    }

    if (mMovementStates._1B) {
        return false;
    }

    if (getDamageAfterTimer() != 0) {
        return false;
    }

    if (isDamaging()) {
        return false;
    }

    mActor->damageDropThrowMemoSensor();
    mActor->resetPlayerModeOnDamage();

    stopJump();
    stopWalk();

    changeStatus(mParalyze);
    return true;
}

MarioParalyze::MarioParalyze(MarioActor* pActor) : MarioState(pActor, MarioStatus_Paralyze), _12(), mTimer(), _16(), mNotDecLife() {
}

bool MarioParalyze::start() {
    changeAnimationNonStop("\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x90\xba\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playEffect("\x83\x72\x83\x8a\x83\x72\x83\x8a");

    startPadVib(3);

    if (!mNotDecLife) {
        mActor->decLifeLarge();
    }

    mNotDecLife = false;
    mTimer = 60;
    _12 = 0;

    return true;
}

bool MarioParalyze::update() {
    if (mTimer != 0) {
        mTimer--;
        if (_12 != 0 && getPlayer()->getMovementStates()._1) {
            addVelocity(getFrontVec(), -3.0f);
        }
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
            changeAnimation("\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");
            playSound("\x90\xba\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");
        }

        if (mActor->mHealth == 0) {
            if (!getPlayer()->mMovementStates._1) {
                mActor->forceGameOverNonStop();
            } else {
                mActor->forceGameOver();
            }

            mActor->changeGameOverAnimation();
        }
    }

    return true;
}

bool MarioParalyze::close() {
    if (mActor->mHealth == 0) {
        if (!getPlayer()->mMovementStates._1) {
            mActor->forceGameOverNonStop();
        } else {
            mActor->forceGameOver();
        }

        mActor->changeGameOverAnimation();
    }

    stopAnimation("\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57");
    stopEffect("\x83\x72\x83\x8a\x83\x72\x83\x8a");

    _16 = 120;

    if (!getPlayer()->isStatusActive(MarioStatus_Swim) && !getPlayer()->getMovementStates()._1) {
        getPlayer()->tryFreeJump(getFrontVec() * -10.0f, true);
    }

    return true;
}
