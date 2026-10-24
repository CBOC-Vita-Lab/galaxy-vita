#include "Game/Player/MarioFaint.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioAccess.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Player/MarioSwim.hpp"
#include "Game/Util/MathUtil.hpp"
#include <revolution/types.h>

bool Mario::doFlipWeak(const TVec3f& rVec) {
    if (mMovementStates._1B) {
        return false;
    }

    mFaint->mNoDamage = true;

    if (faint(rVec)) {
        if (isStatusActive(MarioStatus_Swim)) {
            mSwim->mDamageType = 1;
            mFaint->mNoDamage = false;
        }

        return true;
    }

    mFaint->mNoDamage = false;
    forceStopTornado();
    return false;
}

bool Mario::faint(const TVec3f& rVec) {
    _7C4 = rVec;
    if (!isEnableAddDamage()) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_Damage) {
        return false;
    }

    mFaint->setVec(rVec);
    stopWalk();
    forceStopTornado();
    mActor->damageDropThrowMemoSensor();

    if (getCurrentStatus() != MarioStatus_Faint) {
        mMovementStates._27 = true;
        return true;
    }

    return mMovementStates._27;
}

MarioFaint::MarioFaint(MarioActor* pActor) : MarioState(pActor, MarioStatus_Faint), mTimer(), _14(), _16() {
    _18.zero();
    mNoDamage = false;
    mTookDamage = false;
}

void MarioFaint::setVec(const TVec3f& rVec) {
    MR::vecKillElement(rVec, mActor->_240, &_18);
    _18.setLength(mActor->getConst().getTable()->mSlideDistFaint);
    mTimer = 0;
    _16 = 0;

    if (getPlayer()->isStatusActive(MarioStatus_Hang)) {
        _18.zero();
    }
}

bool MarioFaint::update() {
    mTimer++;

    switch (_16) {
    case 0:
        addVelocity(_18);
        _18.scale(mActor->getConst().getTable()->mFaintFriction1);
        if (mTimer == mActor->getConst().getTable()->mFaintTimer1) {
            _16++;
        }

        break;
    case 1:
        if (!getPlayer()->getMovementStates()._1) {
            return false;
        }

        addVelocity(_18);
        _18.scale(mActor->getConst().getTable()->mFaintFriction2);

        if (checkTrgA()) {
            getPlayer()->tryJump();
            return false;
        }

        if (mTimer == mActor->mConst->getTable()->mFaintTimer1 + mActor->getConst().getTable()->mFaintTimer2) {
            return false;
        }

        break;
    }

    return true;
}

bool MarioFaint::start() {
    mTimer = 0;
    _16 = 0;
    getPlayer()->mMovementStates._B = false;
    getPlayer()->mMovementStates.jumping = false;

    if (_18.dot(getPlayer()->mFrontVec) > 0.0f) {
        getPlayer()->setFrontVecKeepUp(_18);
        changeAnimation("\x8c\xe3\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
    } else {
        getPlayer()->setFrontVecKeepUp(-_18);
        changeAnimation("\x91\x4f\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
    }

    if (mNoDamage) {
        changeAnimation("\x83\x6d\x81\x5b\x83\x5f\x83\x81\x81\x5b\x83\x57");
    }

    playSound("\x90\xba\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playEffect("\x83\x5f\x83\x81\x81\x5b\x83\x57");
    startPadVib(2);

    addVelocity(_18);

    mTookDamage = !mNoDamage;

    if (!mNoDamage) {
        mActor->decLife(0);
        mActor->resetPlayerModeOnDamage();

        if (mActor->mHealth == 0) {
            if (!getPlayer()->getMovementStates()._1) {
                mActor->forceGameOverNonStop();
            } else {
                mActor->forceGameOver();
            }
        }

        return true;
    }

    mNoDamage = false;
    mActor->resetPlayerModeOnNoDamage();
    return true;
}

bool MarioFaint::close() {
    if (getPlayer()->getMovementStates()._1) {
        stopAnimation("\x8c\xe3\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
        stopAnimation("\x91\x4f\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57", "\x8a\xee\x96\x7b");
    }

    if (mTookDamage) {
        _14 = 120;
    }

    return true;
}
