#include "Game/Player/MarioFlip.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioDamage.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Util/MathUtil.hpp"

bool Mario::doFlipJump(const TVec3f& rVec) {
    // FIXME: regswap
    if (isAnimationRun("\x95\xc7\x82\xcd\x82\xb6\x82\xab")) {
        return false;
    }

    stopWalk();
    TVec3f vec(rVec);
    vec.setLength(mActor->mConst->getTable()->mWallSpinFlipGround);
    vec -= getGravityVec()->scaleInline(mActor->mConst->getTable()->mWallSpinHopGround);
    tryForcePowerJump(vec, true);

    changeAnimation("\x95\xc7\x82\xcd\x82\xb6\x82\xab");

    stopPunch();
    mMovementStates._2B = true;
    _430 = 3;
    startPadVib(2);

    return true;
}

bool Mario::doFlipBackRoll(const TVec3f& rVec) {
    TVec3f vec20;
    MR::vecKillElement(rVec, getAirGravityVec(), &vec20);

    if (doFlipLarge(vec20)) {
        checkDamage();
        changeAnimationNonStop("\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1");

        mDamage->setStrings("\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x8b\xf3\x92\x86", "\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x92\x85\x92\x6e");

        TVec3f vec28;
        MR::vecKillElement(rVec, getAirGravityVec(), &vec28);
        setFrontVecKeepUp(-vec28, static_cast< u32 >(12));

        return true;
    }

    return false;
}

bool MarioFlip::start() {
    _12 = 0;
    _14 = 0;
    _24 = 0.0f;
    _28 = -0.75f;

    changeAnimation("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");

    playSound("\x90\xba\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x95\xc7\x8f\xd5\x93\xcb");
    playEffect("\x83\x5f\x83\x81\x81\x5b\x83\x57");
    startPadVib(2);

    addVelocity(_18);

    return true;
}

bool MarioFlip::update() {
    if (_12 == 0) {
        changeAnimationNonStop("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
    }

    _12++;
    TVec3f velocity(_18);
    TVec3f direction(_18);
    MR::normalizeOrZero(&direction);
    f32 control;
    if (_12 < 30) {
        control = static_cast< f32 >(_12) / 30.0f;
    } else {
        control = 1.0f;
    }

    TVec3f lateral;
    f32 along = MR::vecKillElement(getWorldPadDir(), direction, &lateral);
    if (along <= 0.0f) {
        _18 += direction * mActor->mConst->getTable()->mFlipFriction3 * along * control;
    }

    _18 += lateral * mActor->mConst->getTable()->mFlipFriction3;
    switch (_14) {
    case 0:
        _24 += _28;
        _28 *= 0.98f;
        playSound("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
        addVelocity(velocity);
        _18.scale(mActor->mConst->getTable()->mFlipFriction1);
        if (_12 == mActor->mConst->getTable()->mFlipTimer1) {
            _14++;
        }

        break;
    case 1:
        _24 += _28;
        _28 *= 0.97f;
        playSound("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
        if (!getPlayer()->mMovementStates._1) {
            return false;
        }

        addVelocity(velocity);
        _18.scale(mActor->mConst->getTable()->mFlipFriction2);
        if (_12 == mActor->getConst().getTable()->mFlipTimer1 + mActor->mConst->getTable()->mFlipTimer2) {
            _14++;
        }

        break;
    case 2:
        addVelocity(velocity);
        _18.scale(mActor->mConst->getTable()->mFlipFriction2);
        _28 *= 0.97f;
        playSound("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
        _24 += _28;
        if (MR::isAngleBetween(_24, -0.1f, 0.1f)) {
            changeAnimation("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea\x8f\x49\x97\xb9");
            _14++;
        }

        break;
    case 3:
        _24 = 0.0f;
        if (!isAnimationRun("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea\x8f\x49\x97\xb9")) {
            return false;
        }

        break;
    }

    _24 = MR::normalizeAngleAbs(_24);
    setYangleOffset(_24);
    if (getPlayer()->mMovementStates._8 || getPlayer()->mMovementStates._1A || getPlayer()->mMovementStates._19) {
        TVec3f normal(getPlayer()->getWallNorm());
        TVec3f tangent;
        f32 incoming = MR::vecKillElement(_18, normal, &tangent);
        if (incoming < 0.0f) {
            stopAnimation(nullptr);
            changeAnimationNonStop("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
            playEffectTrans("\x95\xc7\x83\x71\x83\x62\x83\x67", getPlayer()->getWallPos());
            _18 = tangent + normal * -incoming * 1.2f;
            addVelocity(_18, 2.0f);
            _28 *= 1.2f;
            getPlayer()->setFrontVecKeepUp(-_18);
        }
    }

    if (_14 >= 1 && checkTrgA()) {
        getPlayer()->tryJump();
        return false;
    }

    if (_14 == 3 && getStickP() != 0.0f) {
        return false;
    }

    return true;
}

bool Mario::doFlipRot(const TVec3f& rVec) {
    getCurrentStatus();
    if (getCurrentStatus() == MarioStatus_FireDamage) {
        return false;
    }

    if (mMovementStates._1B) {
        return false;
    }

    mFlip->setVec(rVec);

    stopJump();
    stopWalk();
    forceStopTornado();

    changeStatus(mFlip);

    return true;
}

bool Mario::doFlipLarge(const TVec3f& rVec) {
    if (getCurrentStatus() == MarioStatus_Damage) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_Flip) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_FireDamage) {
        return false;
    }

    if (mMovementStates._1B) {
        return false;
    }

    return flipLarge(rVec);
}

MarioFlip::MarioFlip(MarioActor* pActor) : MarioState(pActor, MarioStatus_Flip), _12(), _14() {
    _18.zero();
    _24 = 0.0f;
    _28 = 0.0f;
}

void MarioFlip::setVec(const TVec3f& rVec) {
    MR::vecKillElement(rVec, mActor->_240, &_18);

    if (mActor->_3E5) {
        _18.setLength(mActor->mConst->getTable()->mSlideDistFlipSpin);
    } else {
        _18.setLength(mActor->mConst->getTable()->mSlideDistFlip);
    }

    addVelocity(_18, 5.0f);

    _12 = 0;
    _14 = 0;
    _28 = -0.75f;

    if (getPlayer()->isStatusActive(MarioStatus_Hang)) {
        _18.zero();
        return;
    }

    getPlayer()->setFrontVecKeepUp(-_18);
}

bool MarioFlip::close() {
    setYangleOffset(0.0f);

    stopAnimation("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea");
    stopAnimation("\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea\x8f\x49\x97\xb9");

    if (mActor->mHealth == 0) {
        if (!getPlayer()->getMovementStates()._1) {
            mActor->forceGameOverNonStop();
        } else {
            mActor->forceGameOver();
        }
    }

    return true;
}
