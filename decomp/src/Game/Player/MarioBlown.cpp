#include "Game/Player/MarioBlown.hpp"
#include "Game/Player/FireMarioBall.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Util/MathUtil.hpp"
#include <revolution/types.h>

bool Mario::blown(const TVec3f& rVec) {
    if (getCurrentStatus() == MarioStatus_Blown) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_Damage) {
        return false;
    }

    if (getCurrentStatus() == MarioStatus_13) {
        return false;
    }

    stopWalk();

    mBlown->vecKillActor240(rVec);

    mMovementStates._2C = true;
    mMovementStates._A = false;
    _430 = 3;

    return true;
}

MarioBlown::MarioBlown(MarioActor* pActor) : MarioState(pActor, MarioStatus_Blown), mTimer(), _14() {
    _18.zero();
    _24 = false;
    _25 = false;
}

bool MarioBlown::start() {
    mTimer = 0;
    _14 = 0;
    _24 = false;
    _25 = false;

    changeAnimation("\x95\xc7\x83\x71\x83\x62\x83\x67", "\x8a\xee\x96\x7b");
    playSound("\x90\xba\x95\xc7\x91\xcc\x93\x96\x82\xbd\x82\xe8");
    playSound("\x95\xc7\x8f\xd5\x93\xcb");

    playEffectTrans("\x95\xc7\x83\x71\x83\x62\x83\x67", getPlayer()->getWallPos());

    getPlayer()->mMovementStates._1 = false;
    getPlayer()->mMovementStates.jumping = true;
    getPlayer()->mMovementStates._B = false;

    _18 += -mActor->_240 * mActor->getConst().getTable()->mJumpHeightBlown;
    getPlayer()->setJumpVec(_18);
    addVelocity(_18);
    getPlayer()->lockGroundCheck(this, true);
    return true;
}

bool MarioBlown::update() {
    mTimer++;

    if (getPlayerMode() == PlayerMode_Teresa) {
        return false;
    }

    switch (_14) {
    case 0:
    case 2:
        addVelocity(_18);
        _18 += mActor->_240 * mActor->getConst().getTable()->mGravityBlown;
        if (mTimer > 120) {
            changeAnimation("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86");
        }

        if (mTimer > 60) {
            TVec3f killed;
            if (MR::isNearZero(MR::vecKillElement(mActor->getLastMove(), mActor->_240, &killed))) {
                return false;
            }
        }

        if (getPlayer()->getMovementStates()._1 || MR::isNearZero(getPlayer()->mVerticalSpeed)) {
            getPlayer()->mMovementStates.jumping = false;
            getPlayer()->mMovementStates._D = true;

            if (getPlayer()->damagePolygonCheck(getPlayer()->getGroundPolygon())) {
                getPlayer()->_1C._16 = true;
                _24 = true;
                return false;
            }

            if (_14 != 2 || mTimer >= 3) {
                playSound("\x90\x81\x82\xc1\x94\xf2\x82\xd1\x93\x7c\x82\xea");
                changeAnimation("\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e");
                playEffect("\x8b\xa4\x92\xca\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e");
                MR::vecKillElement(_18, mActor->_240, &_18);
            }

            _14 = 1;
            mTimer = 0;
        }

        break;
    case 1:
        if (!getPlayer()->getMovementStates()._1 && !MR::isNearZero(getPlayer()->mVerticalSpeed)) {
            _25 = true;
            return false;
        }

        addVelocity(_18);
        _18.mult(0.95f);

        if (!isAnimationRun("\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e")) {
            return false;
        }

        if (mTimer > 15 && checkTrgA()) {
            getPlayer()->stopJump();
            getPlayer()->tryJump();
            _24 = true;
            return false;
        }

        break;
    }

    f32 dot = vecKillActor240(_18);

    if (_18.length() > 10.0f) {
        _18.mult(0.5f);
    }

    _18 += mActor->_240 * MR::clamp(dot, 0.0f, 40.0f);
    getPlayer()->setJumpVec(_18);
    return true;
}

bool MarioBlown::close() {
    if (!_24) {
        getPlayer()->stopJump();
    }

    stopAnimation("\x95\xc7\x83\x71\x83\x62\x83\x67");

    if (_25) {
        stopAnimation("\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
    }

    mActor->setBlendMtxTimer(6);
    getPlayer()->unlockGroundCheck(this);

    if (!_24) {
        getPlayer()->mMovementStates._36 = false;
    }

    return true;
}
