#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioFreeze.hpp"

bool Mario::doFreeze() {
    if (mMovementStates._1F) {
        return false;
    }

    if (isInvincible()) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_Freeze) {
        return false;
    }

    if (mFreeze->_1C != 0) {
        return false;
    }

    mActor->resetPlayerModeOnDamage();

    stopJump();
    stopWalk();

    mActor->damageDropThrowMemoSensor();
    cancelSquatMode();

    stopAnimationUpper(nullptr);
    changeStatus(mFreeze);
    return true;
}

MarioFreeze::MarioFreeze(MarioActor* pActor) : MarioState(pActor, MarioStatus_Freeze), mIsFrozen(), _14(), _18(), mFreezeTimer(), _1C() {
}

bool MarioFreeze::notice() {
    if (mActor->mHealth == 0) {
        if (getNoticedStatus() == MarioStatus_Swim) {
            mActor->_B90 = true;
            mActor->forceGameOver();
        }

        return true;
    }

    return false;
}

bool MarioFreeze::start() {
    changeAnimationNonStop("\x95\x58\x8c\x8b");

    playSound("\x90\xba\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x83\x5f\x83\x81\x81\x5b\x83\x57");

    startPadVib(3);
    mActor->decLife(0);

    mFreezeTimer = 180;
    _18 = 0;
    _14 = 0.0f;

    getPlayer()->resetInline();

    mActor->showFreezeModel();

    mIsFrozen = true;

    return true;
}

bool MarioFreeze::update() {
    if (mFreezeTimer != 0) {
        mFreezeTimer--;

        if (_18 != 0) {
            if (getPlayer()->getMovementStates()._1) {
                addVelocity(getFrontVec(), -1.0f);
            }
        } else if (mFreezeTimer < 120 && mActor->mHealth != 0 && mActor->isRequestSpin()) {
            addVelocity(getFrontVec(), -10.0f);
            changeAnimation("\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8");

            playSound("\x90\xba\x83\x58\x83\x73\x83\x93");
            playSound("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
            playSound("\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");

            mActor->hideFreezeModel();
            mIsFrozen = false;
            return false;
        }
    }

    if (mFreezeTimer == 0) {
        if (_18 != 0) {
            if (mActor->mHealth != 0) {
                playSound("\x90\xba\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");
                return false;
            }
        } else {
            _18 = 1;
            if (!getPlayer()->getMovementStates()._1) {
                mFreezeTimer = 10;
            } else {
                mFreezeTimer = 20;
            }

            if (mActor->mHealth == 0) {
                if (!getPlayer()->getMovementStates()._1) {
                    mActor->forceGameOverNonStop();
                } else {
                    mActor->forceGameOver();
                }
            } else if (getPlayer()->getMovementStates()._1) {
                changeAnimation("\x95\x58\x8c\x8b\x89\xf0\x8f\x9c");
                playSound("\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");
                mActor->hideFreezeModel();
                mIsFrozen = false;
            }
        }
    }

    if (mFreezeTimer < 150) {
        if (!getPlayer()->getMovementStates()._1 || _14 < 0.0f) {
            getPlayer()->setJumpVec(getGravityVec() * _14);
            addVelocity(getGravityVec() * _14);
            _14 += 1.5f;
            if (_14 < 0.0f) {
                getPlayer()->mMovementStates._1 = false;
            }
        } else if (_14 > 0.0f) {
            if (_14 < 8.0f) {
                _14 = 0.0f;
            } else {
                _14 = 0.4f * -_14;
                getPlayer()->mMovementStates._1 = false;
                getPlayer()->setJumpVec(getGravityVec() * _14);
            }
        }
    }

    return true;
}

bool MarioFreeze::close() {
    _1C = 120;

    if (mIsFrozen) {
        playSound("\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9");
        mActor->hideFreezeModel();
    }

    return true;
}
