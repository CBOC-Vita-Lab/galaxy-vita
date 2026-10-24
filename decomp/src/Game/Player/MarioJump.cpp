#include "Game/Enemy/KariKariDirector.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioRabbit.hpp"
#include "Game/Player/MarioSkate.hpp"
#include "Game/Player/MarioSukekiyo.hpp"
#include "Game/Player/MarioSwim.hpp"
#include "Game/Player/MarioWall.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    f32 cDropFrontSpeed = 2.0f;
};  // namespace

void MarioJump_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)1.57079637f;
    (void)2.0f;
    (void)0.00100000005f;
    (void)15.0f;
    (void)0.300000012f;
    (void)4.0f;
    (void)10.0f;
    (void)0.899999976f;
    (void)500.0f;
    (void)0.100000001f;
    (void)5.0f;
    (void)100.0f;
    (void)50.0f;
    (void)150.0f;
    (void)0.600000024f;
    (void)80.0f;
    (void)20.0f;
    (void)8.0f;
    (void)0.200000003f;
    (void)0.800000012f;
    (void)0.00390625f;
    (void)45.0f;
    (void)160.0f;
    (void)1.79999995f;
    (void)0.25f;
    (void)300.0f;
    (void)200.0f;
    (void)-0.100000001f;
    (void)-0.800000012f;
    (void)30.0f;
    (void)-0.200000003f;
    (void)0.785398185f;
    (void)0.998000026f;
    (void)0.00999999978f;
    (void)6.0f;
    (void)7.0f;
    (void)90.0f;
}

bool Mario::isRising() const {
    if (getPlayerMode() == 4 || getPlayerMode() == 6) {
        if (_16C.dot(*getGravityVec()) < 0.0f) {
            return true;
        }

        return false;
    }

    return mJumpVec.dot(*getGravityVec()) < 0.0f;
}

void Mario::checkWallRiseAndSlipFront() {
    mMovementStates._6 = false;
    if (mMovementStates._8) {
        mMovementStates._9 = false;
        mWalkSpeed = 0.0f;
        return;
    }

    mMovementStates._9 = true;
}

void Mario::tryJump() {
    bool isMudFloorJump = false;
    bool useStickJumpAnim = false;

    TVec3f stickTotal(_190 + _19C);

    if (mMovementStates.jumping) {
        return;
    }

    if (mSinkTimer != 0) {
        if (checkCurrentFloorCodeSevere(0x19)) {
            if (mSinkTimer > 32) {
                mSinkTimer -= 32;
            }

            if (mSinkTimer < 64) {
                mSinkTimer = 64;
            }

            mMovementStates._38 = false;
            return;
        }

        if (checkCurrentFloorCodeSevere(0x1F)) {
            return;
        }

        if (mSinkTimer > 100) {
            if (mSinkTimer > 200) {
                mSinkTimer = 128;
                changeAnimation("\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""A");
            } else {
                mSinkTimer = 32;
                changeAnimation("\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""B");
            }

            if (checkCurrentFloorCodeSevere(0x12)) {
                playSound("\x93\xc5\x8f\xc0\x92\x45\x8f\x6f");
            } else {
                playSound("\x8d\xbb\x92\x45\x8f\x6f");
            }

            playSound("\x90\xba\x8d\xbb\x92\x45\x8f\x6f");
            return;
        }
    }

    playEffect("\x8b\xa4\x92\xca\x92\xb5\x96\xf4");
    stopEffectForce("\x83\x58\x83\x73\x83\x93\x83\x8a\x83\x93\x83\x4f");

    const bool isSquat = checkSquat(true);
    mMovementStates._21 = true;
    mMovementStates._29 = false;
    mMovementStates._38 = false;
    _10._23 = true;
    mMovementStates._B = false;
    _42A = 0;

    if (_3CE > mActor->getConst().getTable()->mJumpConnectTime || !mMovementStates._5) {
        _430 = 0;
    } else {
        _430++;
    }

    if (_430 > 2) {
        _430 = 0;
    }

    if (_430 == 2 && mWalkSpeed < mActor->getConst().getTable()->mJumpConnectSpeed) {
        _430 = 0;
    }

    if (getPlayerMode() == 4 || getPlayerMode() == 6) {
        _430 = 0;
    }

    if (_1C._B) {
        _430 = 2;
        MR::start2PJumpAssistJustSound();
    } else if (_1C._A) {
        MR::start2PJumpAssistSound();
    }

    if (isPlayerModeInvincible() && mWalkSpeed > 1.0f) {
        _430 = 1;
        _42A = 1;
    }

    if (!MR::isNearZero(stickTotal) && mSinkTimer != 0) {
        _430 = 0;
        mMovementStates._4 = false;
        _3D2 = 0;
    }

    forceStopTornado();

    u32 floorCode = getFloorCode();
    if (floorCode != 0x20 && getPlayerMode() != 4 && (((mMovementStates._4) != 0) || (_3D2 != 0 && static_cast< u8 >(checkStickFrontBack()) == 2))) {
        if ((mActor->mConst->getTable()->mTurnSlipTime - _3D0) >= mActor->getConst().getTable()->mTurnJumpInhibitTime) {
            tryTurnJump();
            return;
        }

        _3D0 = 0;
        mMovementStates._4 = false;
    }

    checkWallRiseAndSlipFront();

    const f32 floorAngle = calcPolygonAngleD(mGroundPolygon);
    useStickJumpAnim = false;
    if (!MR::isNearZero(_8F8) && (mMovementStates._23)) {
        useStickJumpAnim = true;
    }

    if (floorAngle < 15.0f) {
        mMovementStates._23 = false;
        _8F8.zero();
    }

    if (getPlayerMode() != 6 && !_10._1A) {
        if (mDrawStates._C && isSlipPolygon(_45C)) {
            const bool dotFront = mFrontVec.dot(_368) < 0.0f;
            const bool dotPad = mWorldPadDir.dot(_368) <= 0.0f;
            const bool diff = MR::diffAngleAbsHorizontal(_16C, _368, getAirGravityVec()) > 1.5707964f;
            const bool movementstate = mMovementStates._A;

            if (dotFront || dotPad || diff || movementstate) {
                if (dotFront && !dotPad) {
                    mMovementStates._2B = true;
                    _430 = 0;
                } else {
                    mWalkSpeed *= 0.5f;
                    if (getPlayerMode() != 4) {
                        _402 = 0;
                        _430 = 3;
                        mMovementStates._2B = true;
                    }

                    useStickJumpAnim = false;
                }
            } else {
                mMovementStates._23 = false;
            }
        }

        if (checkCurrentFloorCodeSevere(0x11)) {
            isMudFloorJump = true;
            _430 = 3;
            mWalkSpeed *= 0.5f;
        }
    }

    floorCode = getFloorCode();
    if (floorCode != 0x20 && getPlayerMode() != 4 && _430 != 3) {
        if (isSquat && mWalkSpeed > 0.3f && ((mMovementStates._23) == 0)) {
            trySquatJump();
            return;
        }

        if ((mMovementStates._A) && ((mMovementStates._23) == 0)) {
            tryBackJump();
            return;
        }
    }

    if (mDrawStates._5) {
        mJumpVec = _334 * mWalkSpeed * mActor->getConst().getTable()->mJumpFrontSpeed;
    } else {
        mJumpVec = mFrontVec * mWalkSpeed * mActor->getConst().getTable()->mJumpFrontSpeed;
    }

    _10._17 = false;
    if (mDrawStates._B) {
        recordJumpEnforceMove();

        f32 enforce;
        if (getStickP() == 0.0f) {
            mJumpVec = _184;
            mJumpVec.zero();
            enforce = 1.0f;
        } else {
            const f32 angle = MR::diffAngleAbsHorizontal(mFrontVec, _184, getAirGravityVec());
            enforce = 1.0f - (angle / 1.5707964f);
            if (enforce < 0.0f) {
                enforce = 0.0f;
            }

            const f32 speed = mJumpVec.length();
            const f32 speedBonus = MR::clamp(1.0f - ((speed - 4.0f) / 10.0f), 0.0f, 1.0f);
            enforce += speedBonus;
            enforce = MR::clamp(enforce, 0.0f, 1.0f);
        }

        doEnforceJump(enforce);
    }

    if (!MR::isNearZero(stickTotal)) {
        addVelocity(-stickTotal);

        MR::vecKillElement(stickTotal, mActor->_240, &stickTotal);
        cutGravityElementFromJumpVec(true);

        if (stickTotal.dot(mJumpVec) > 0.0f) {
            mJumpVec += stickTotal * 1.0f;
        } else {
            mJumpVec += stickTotal * 0.5f;
        }
    }

    if (useStickJumpAnim) {
        mJumpVec += _16C;
        cutGravityElementFromJumpVec(true);
    }

    _340 = 1.0f - mWalkSpeed;
    if (_340 < 0.0f) {
        _340 = 0.0f;
    }

    f32 jumpRatio = 1.0f;
    if (getFloorCode() == 0x20) {
        jumpRatio = mActor->getConst().getTable()->mMudFloorJumpWeakRatio;
    }

    if (_1C._B) {
        jumpRatio = 0.9f;
    }

    mJumpVec += -mActor->_240 * mActor->getConst().getTable()->mJumpHeight[_430] * jumpRatio;

    mMovementStates._E = false;
    mMovementStates._1 = false;
    if (getPlayerMode() != 4 && getFloorCode() == 0x20) {
        _430 = 3;
        isMudFloorJump = true;
        mMovementStates._2B = true;
    }

    if (_10._1A) {
        _430 = 0;
    }

    procJump(true);
    mMovementStates.jumping = true;

    if (_42A == 1 && _430 == 1) {
        _430 = 2;
    }

    switch (_430) {
    case 0:
        if ((mMovementStates._17) != 0) {
            changeAnimation("\x95\xc7\x8f\xe3\x8f\xb8", "\x97\x8e\x89\xba");
        } else {
            changeAnimation("\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        }

        if (mActor->mBeeWallWalk != 0) {
            playSound("\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76");
        } else {
            playSound("\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");
        }

        if (getPlayerMode() != 6) {
            playSound("\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");
            playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
        }

        if (useStickJumpAnim) {
            changeAnimation("\x90\x4b\x8a\x8a\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76");
            _428 = 0x1E;
        }

        if (getPlayerMode() == 4) {
            changeAnimation("\x83\x6e\x83\x60\x83\x57\x83\x83\x83\x93\x83\x76");
            mActor->syncJumpBeeStickMode();
        }

        if (_10._1A) {
            changeAnimation("\x83\x56\x83\x87\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        }

        break;
    case 1:
        changeAnimationNonStop("\x83\x57\x83\x83\x83\x93\x83\x76""B");
        changeAnimation(nullptr, "\x97\x8e\x89\xba");
        playSound("\x92\x86\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x90\xba\x92\x86\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
        break;
    case 2:
        playSound("\x91\xe5\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x90\xba\x91\xe5\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
        playEffect("\x8b\xa4\x92\xca\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76");

        if (_1C._B) {
            changeAnimation("\x83\x58\x83\x4a\x83\x43\x83\x89\x83\x75\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        } else if (getPlayerMode() == 1 && mWalkSpeed > 1.0f) {
            changeAnimation("\x83\x5f\x83\x62\x83\x56\x83\x85\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        } else {
            changeAnimation("\x83\x57\x83\x83\x83\x93\x83\x76""C", "\x97\x8e\x89\xba");
        }

        break;
    case 3:
        if (isMudFloorJump) {
            changeAnimation("\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        } else {
            changeAnimation("\x95\xa0\x82\xce\x82\xa2\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
            playSound("\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");
            playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
        }

        break;
    default:
        break;
    }

    if (_42A == 1 && _430 == 2) {
        _430 = 1;
    }

    const f32 waterDist = mSwim->checkUnderWaterFull(mFrontVec);
    if (waterDist > 500.0f && mStickPos.z == 0.0f) {
        mMovementStates._E = true;
        changeAnimationNonStop("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x90\xba\x8d\x82\x94\xf2\x82\xd1\x8d\x9e\x82\xdd");
        playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");

        if (mStickPos.z < 0.1f) {
            mJumpVec += mFrontVec * 5.0f;
        }
    }

    mMovementStates._5 = true;
    return;
}

void Mario::tryTurnJump() {
    _430 = 0x4;

    if (_3D2 != 0) {
        TVec3f horizontal;
        MR::vecKillElement(_3E4, getAirGravityVec(), &horizontal);
        MR::normalizeOrZero(&horizontal);

        const f32 turnJumpFrontSpeed = mActor->getConst().getTable()->mTurnJumpFrontSpeed;
        mJumpVec = -horizontal * turnJumpFrontSpeed;

        setFrontVecKeepUp(-_3E4);
        _220 = _3E4;
    } else {
        TVec3f horizontal;
        MR::vecKillElement(_220, getAirGravityVec(), &horizontal);
        MR::normalizeOrZero(&horizontal);

        const f32 turnJumpFrontSpeed = mActor->getConst().getTable()->mTurnJumpFrontSpeed;
        mJumpVec = -horizontal * turnJumpFrontSpeed;
    }

    const f32 jumpHeight = mActor->getConst().getTable()->mJumpHeight[_430];
    mJumpVec += -mActor->_240 * jumpHeight;

    _10._17 = false;
    if (mDrawStates._B) {
        recordJumpEnforceMove();
        doEnforceJump(1.0f);
    }

    mMovementStates._E = true;
    procJump(true);
    changeAnimation("\x83\x5e\x81\x5b\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
    playSound("\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x90\xba\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
    playEffect("\x8b\xa4\x92\xca\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76");

    _3D0 = 0;
    mMovementStates.jumping = true;
    mMovementStates._1 = false;
    mMovementStates._4 = false;
    mMovementStates._5 = false;
    mWalkSpeed = 0.0f;
    _10._A = false;
    return;
}

void Mario::trySquatJump() {
    mMovementStates._21 = false;
    _430 = 0x5;

    mJumpVec = getAirFrontVec() * mActor->getConst().getTable()->mSquatJumpFrontSpeed;

    TVec3f stickVec(_190 + _19C);
    if (!MR::isNearZero(stickVec)) {
        addVelocity(-stickVec);

        MR::vecKillElement(stickVec, mActor->_240, &stickVec);
        cutGravityElementFromJumpVec(true);

        mJumpVec += stickVec * 1.0f;
    }

    const f32 jumpHeight = mActor->getConst().getTable()->mJumpHeight[_430];
    mJumpVec += -mActor->_240 * jumpHeight;

    _10._17 = false;
    if (mDrawStates._B) {
        recordJumpEnforceMove();

        TVec3f moveKeep;
        f32 gravityMove = MR::vecKillElement(_184, getAirGravityVec(), &moveKeep);

        f32 enforceScale = 1.0f - (MR::diffAngleAbs(mFrontVec, moveKeep) / 1.5707964f);
        if (enforceScale < 0.0f) {
            enforceScale = 0.0f;
        }

        const f32 jumpMag = mJumpVec.length();
        const f32 jumpScale = MR::clamp(1.0f - ((jumpMag - 4.0f) / 10.0f), 0.0f, 1.0f);
        enforceScale += jumpScale;
        enforceScale = MR::clamp(enforceScale, 0.0f, 1.0f);

        if (mJumpVec.dot(moveKeep) < 0.0f) {
            mDrawStates._1B = true;
            initActiveJumpVec();
        }

        mVelocity -= moveKeep;

        mJumpVec += moveKeep * enforceScale;

        if (gravityMove < 0.0f) {
            mJumpVec += getAirGravityVec() * gravityMove;
        }

        invalidateRelativePosition();
    }

    mMovementStates._E = true;
    mMovementStates._1 = false;
    procJump(true);
    changeAnimationNonStop("\x95\x9d\x82\xc6\x82\xd1");
    changeAnimation(nullptr, "\x97\x8e\x89\xba");
    playSound("\x95\x9d\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x90\xba\x95\x9d\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");

    mMovementStates.jumping = true;
    _3D0 = 0;
    mMovementStates._5 = false;
    mMovementStates._4 = false;
    return;
}

void Mario::tryBackJump() {
    mMovementStates._21 = true;
    mMovementStates._A = false;
    _430 = 0x6;

    const f32 backSpeed = mActor->getConst().getTable()->mSquatJumpBackSpeed;
    mJumpVec = -getAirFrontVec() * backSpeed;

    const f32 jumpHeight = mActor->getConst().getTable()->mJumpHeight[_430];
    mJumpVec += -mActor->_240 * jumpHeight;

    _10._17 = false;
    if (mDrawStates._B) {
        recordJumpEnforceMove();
        doEnforceJump(1.0f);
    }

    mMovementStates._E = true;
    procJump(true);
    changeAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
    playSound("\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x90\xba\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
    playEffect("\x8b\xa4\x92\xca\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76");

    if (mSwim->checkUnderWaterFull(-mFrontVec) > 500.0f && mStickPos.z == 0.0f) {
        changeAnimationNonStop("\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x90\xba\x8d\x82\x94\xf2\x82\xd1\x8d\x9e\x82\xdd");
        playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
    }

    mWalkSpeed = 0.0f;
    mMovementStates.jumping = true;
    _3D0 = 0;
    mMovementStates._1 = false;
    mMovementStates._4 = false;
    mMovementStates._5 = false;
    return;
}

void Mario::tryTornadoJump() {
    _430 = 0x8;
    _42A = 0;

    const f32 frontSpeed = mWalkSpeed;
    const f32 tornadoSpeed = mActor->getConst().getTable()->mJumpTornadoSpeed;

    mJumpVec = mFrontVec * frontSpeed * tornadoSpeed;

    const f32 jumpHeight = mActor->getConst().getTable()->mJumpHeight[_430];
    mJumpVec += -mActor->_240 * jumpHeight;

    mMovementStates._E = false;
    mMovementStates._F = true;
    procJump(true);
    changeAnimation("\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
    playSound("\x90\xba\x83\x58\x83\x73\x83\x93");
    playSound("\x83\x67\x83\x8b\x83\x6c\x81\x5b\x83\x68\x83\x57\x83\x83\x83\x93\x83\x76");

    mMovementStates.jumping = true;
    _3D0 = 0;
    mMovementStates._4 = false;
    mMovementStates._5 = false;
    mMovementStates._1D = false;
    mWalkSpeed = 0.0f;

    _402 = mActor->getConst().getTable()->mAirWalkTimeTornado;
    _424 = mActor->getConst().getTable()->mTornadoZeroGravityTimer;

    const f32 boosterPower = mActor->getConst().getTable()->mTornadoBoostPower;
    setRocketBooster(-(*getGravityVec()) * boosterPower, mActor->getConst().getTable()->mTornadoBoostAttn,
                     mActor->getConst().getTable()->mTornadoBoostTimer);
}

void Mario::startTornadoCentering(HitSensor* pSensor) {
    pushTask(reinterpret_cast< Task >(&Mario::taskOnTornadoCentering), 0x400);
    _A38 = reinterpret_cast< u32 >(pSensor);
    _A34 = 0x1E;
}

bool Mario::taskOnTornadoCentering(u32 a1) {
    if (_A34 != 0) {
        TVec3f sensorOffset(reinterpret_cast< HitSensor* >(_A38)->mHost->mPosition - mPosition);
        MR::vecKillElement(sensorOffset, *getGravityVec(), &sensorOffset);

        f32 mag = sensorOffset.length();
        if (mag < 10.0f) {
            mag = 10.0f;
        }

        f32 ratio = 10.0f / mag;
        if (ratio < 0.1f) {
            ratio = 0.1f;
        }

        addTrans(sensorOffset * ratio, nullptr);

        _A34--;

        if (_A34 == 0) {
            return false;
        }

        return true;
    }

    return false;
}

void Mario::trySpinJump(u8 a1) {
    if (mMovementStates._B) {
        return;
    }

    if (_430 == 8) {
        return;
    }

    if (isStatusActive(0x17)) {
        return;
    }

    if (getPlayerMode() == 4) {
        return;
    }

    if (getPlayerMode() == 6) {
        return;
    }

    if (getPlayerMode() == 7 && mVerticalSpeed > 100.0f) {
        tryStartFoo();
        return;
    }

    if (isAnimationRun("\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae")) {
        return;
    }

    if (isAnimationRun("\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76")) {
        return;
    }

    _430 = 0x8;
    _42A = a1 + 1;

    const f32 frontSpeed = mWalkSpeed;
    const f32 tornadoSpeed = mActor->getConst().getTable()->mJumpTornadoSpeed;
    mJumpVec = mFrontVec * frontSpeed * tornadoSpeed;

    const f32 spinJumpHeight = mActor->getConst().getTable()->mSpinJumpHeight;
    mJumpVec += -mActor->_240 * spinJumpHeight;

    mMovementStates._E = false;
    _402 = mActor->getConst().getTable()->mAirWalkTimeSpin;
    procJump(true);

    if (!mMovementStates._B) {
        stopAnimationUpper(nullptr);
        changeAnimation("\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8", "\x97\x8e\x89\xba");
        playSound("\x90\xba\x83\x58\x83\x73\x83\x93");
        playSound("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
        startPadVib(2);

        mMovementStates.jumping = true;
        mMovementStates._4 = false;
        mMovementStates._5 = false;
        mMovementStates._1D = false;
        _10._19 = true;
        _3D0 = 0;
        mWalkSpeed = 0.0f;
        _10._1 = true;

        if (isStatusActive(MarioStatus_Rabbit)) {
            mRabbit->hop();
        }
    }
}

void Mario::tryForceJumpDelay(const TVec3f& rVec) {
    mMovementStates._2D = true;
    _304 = rVec;
}

void Mario::tryFreeJumpDelay(const TVec3f& rVec) {
    mMovementStates._2E = true;
    _304 = rVec;
}

void Mario::tryForceJump(const TVec3f& rVec, bool a2) {
    if (rVec.dot(*getGravityVec()) > 0.0f) {
        mMovementStates._20 = true;
    }

    mMovementStates.jumping = true;
    mMovementStates._1 = false;
    mMovementStates._5 = false;
    mMovementStates._E = a2;
    mMovementStates._B = false;
    mJumpVec = rVec;

    if (getPlayerMode() == 4 || getPlayerMode() == 6) {
        _16C = mJumpVec;
    }

    resetTornado();

    _3BC = 0;
    _3CA = 0;
    _3CC = 0;
    mMovementStates._13 = true;
    mMovementStates._11 = false;
    _10._8 = false;
    _3BE = 0;
    _344 = mSideVec;
    _4B0 = mPosition;
    _42C = 0;
    _76C = 0;
    procJump(false);
    changeAnimation(nullptr, "\x97\x8e\x89\xba");
    mRabbit->forceJump();
    return;
}

void Mario::tryForceFreeJump(const TVec3f& rVec) {
    mMovementStates._6 = false;
    _430 = 0xC;
    mMovementStates._2A = true;
    mMovementStates._9 = false;

    tryForceJump(rVec, true);
    _3BC = 0;
    mMovementStates._11 = false;
    _402 = mActor->getConst().getTable()->mAirWalkTime;

    if (getPlayerMode() == 4) {
        playSound("\x83\x6e\x83\x60\x91\xcc\x97\xcd\x8a\xae\x91\x53\x89\xf1\x95\x9c");
    }

    _76C = 0;
    return;
}

void Mario::tryForcePowerJump(const TVec3f& rVec, bool a2) {
    mMovementStates._6 = false;
    _430 = 0xD;
    mMovementStates._2A = true;
    mMovementStates._9 = false;
    const bool _2FPrev = mMovementStates._2F;
    mMovementStates._2F = true;
    tryForceJump(rVec, true);
    mMovementStates._2F = _2FPrev;
    _3BC = 0;
    mMovementStates._11 = false;

    if (!a2) {
        _402 = mActor->getConst().getTable()->mAirWalkTime;

        if (getPlayerMode() == 4) {
            playSound("\x83\x6e\x83\x60\x91\xcc\x97\xcd\x8a\xae\x91\x53\x89\xf1\x95\x9c");
        }
    }

    _76C = 0;
    return;
}

void Mario::tryFreeJump(const TVec3f& rVec, bool a2) {
    _430 = 0;

    if (rVec.dot(*getGravityVec()) > 0.0f) {
        mMovementStates._20 = true;
    }

    mMovementStates.jumping = true;
    mMovementStates._1 = false;
    mMovementStates._5 = false;
    mMovementStates._E = a2;
    mMovementStates._B = false;
    mJumpVec = rVec;
    resetTornado();

    _3BC = 0;
    _3CA = 0;
    _3CC = 0;
    mMovementStates._13 = true;
    mMovementStates._11 = false;
    _3BE = 0;
    _76C = 0;
    _344 = mSideVec;
    procJump(false);
    changeAnimation(nullptr, "\x97\x8e\x89\xba");
    startPadVib(2);
    mRabbit->forceJump();
    return;
}

void Mario::tryWallJump(const TVec3f& rVec, bool a2) {
    _430 = 0x7;

    mMovementStates.jumping = true;
    mMovementStates._5 = false;
    mMovementStates._E = true;
    mMovementStates._B = false;
    mMovementStates._6 = false;
    mMovementStates._28 = false;
    _20._28 = false;
    mMovementStates._29 = false;

    if (a2) {
        mMovementStates._9 = true;
    } else {
        mMovementStates._9 = false;
    }

    mJumpVec = rVec;
    _4B0 = mPosition;
    _3BC = 0;
    mMovementStates._13 = true;
    mMovementStates._11 = false;
    _3CA = 0;
    _3CC = 0;
    _3BE = 0;
    mMovementStates._1D = true;

    if (getPlayerMode() == 5) {
        changeAnimationNonStop("\x83\x7a\x83\x62\x83\x70\x81\x5b\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76");
    } else {
        changeAnimation("\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
        playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
    }

    playSound("\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");

    if (a2) {
        setFrontVecKeepUp(rVec);
    }

    stopWalk();
    mRabbit->forceJump();
    return;
}

void Mario::tryStickJump(const TVec3f& rVec) {
    stopJump();
    initJumpParam();

    mMovementStates.jumping = true;
    mMovementStates._5 = false;
    mMovementStates._E = true;
    mMovementStates._B = false;
    _430 = 0;
    _20._28 = false;
    mMovementStates._28 = false;
    mMovementStates._29 = false;
    mMovementStates._6 = false;
    mMovementStates._9 = true;
    mJumpVec = rVec;
    _3CA = 0;
    _3CC = 0;
    _3BE = 0;
    mMovementStates._13 = true;
    mMovementStates._11 = false;
    mMovementStates._1D = true;
    _76C = mActor->getConst().getTable()->mBeeGravityReviveTime;
    _770 = 0.0f;
    _3BC = 0xA;
    playSound("\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76");
    playSound("\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");
    setFrontVecKeepUp(rVec);
    changeAnimationNonStop("\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76");
    return;
}

void Mario::trySlipUpJump() {
    if (mMovementStates._1) {
        mMovementStates._6 = false;
        mMovementStates._28 = false;
        return;
    }

    changeAnimation("\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    playSound("\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    playEffect("\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    startPadVib("\x83\x7d\x83\x8a\x83\x49[\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76]");

    mJumpVec = -getAirGravityVec() * mActor->getConst().getTable()->mSlipUpHeight;

    f32 ratio = 1.0f;
    TVec3f predictPos(mPosition - *getGravityVec() * 100.0f);

    TVec3f frontVec;
    MR::vecKillElement(mJumpVec, mFrontVec, &frontVec);
    MR::vecKillElement(frontVec, *getGravityVec(), &frontVec);
    frontVec += mFrontVec * mActor->getConst().getTable()->mSlipUpFront;
    MR::normalizeOrZero(&frontVec);

    f32 step = 50.0f;
    u32 hitCount;
    TVec3f hitPos;
    for (hitCount = 0; hitCount < 10; hitCount++) {
        Triangle* pTmpPolygon = getTmpPolygon();
        predictPos += frontVec * step;

        if (!MR::getFirstPolyOnLineBFast(predictPos, *getGravityVec() * 150.0f, &hitPos, pTmpPolygon)) {
            break;
        }
    }

    ratio *= static_cast< f32 >(hitCount) / 10.0f;
    cutVecElementFromJumpVec(mFrontVec);

    if (mMovementStates._9) {
        mJumpVec += mFrontVec * mActor->getConst().getTable()->mSlipUpFront * ratio;
        mMovementStates._29 = true;
    } else {
        mJumpVec += mFrontVec * mActor->getConst().getTable()->mSlipUpFrontWeak * ratio;
    }

    mMovementStates._6 = false;
    mMovementStates._E = true;
    initJumpParam();
    _430 = 0xA;
    return;
}

void Mario::tryHangSlipUp() {
    mMovementStates._5 = false;
    getPlayer()->tryJump();
    changeAnimation("\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    playSound("\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    playEffect("\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76");
    startPadVib("\x83\x7d\x83\x8a\x83\x49[\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76]");

    const f32 slipUpHeight = mActor->getConst().getTable()->mSlipUpHeightHang;
    mJumpVec = -getAirGravityVec() * slipUpHeight;
    cutVecElementFromJumpVec(mFrontVec);

    const f32 slipUpFront = mActor->getConst().getTable()->mSlipUpFrontHang;
    mJumpVec += mFrontVec * slipUpFront;

    mMovementStates._6 = false;
    mMovementStates._E = true;
    _430 = 0xA;
    mActor->setBlendMtxTimer(0x8);
    mTargetWalkSpeedIndex = 0x7;
    mWalkSpeed = 0.6f;
    return;
}

void Mario::tryDrop() {
    if ((mMovementStates._8) && !mDrawStates._B) {
        TVec3f horizontal;
        MR::vecKillElement(_4E8 - mGroundPos, *getGravityVec(), &horizontal);

        if (horizontal.dot(*mFrontWallTriangle->getNormal(0)) < 0.0f && horizontal.length() < 80.0f) {
            getPlayer()->mMovementStates._1 = true;
            push(*mFrontWallTriangle->getNormal(0) * (80.0f - horizontal.length()) * 0.1f);
            return;
        }
    }

    if ((mMovementStates._1A) && !mDrawStates._B) {
        TVec3f horizontal;
        MR::vecKillElement(_500 - mGroundPos, *getGravityVec(), &horizontal);

        if (horizontal.dot(*mSideWallTriangle->getNormal(0)) < 0.0f && horizontal.length() < 80.0f) {
            push(*mSideWallTriangle->getNormal(0) * (80.0f - horizontal.length()) * 0.1f);
            getPlayer()->mMovementStates._1 = true;
            return;
        }
    }

    mMovementStates._21 = true;
    mMovementStates._B = false;

    mJumpVec = mFrontVec * mWalkSpeed * ::cDropFrontSpeed;

    if (MR::isNearZero(_8F8) && (mMovementStates._23)) {
        TVec3f side;
        side.cross(getAirGravityVec(), _368);
        MR::normalizeOrZero(&side);
        _8F8.cross(_368, side);
        MR::normalizeOrZero(&_8F8);
    }

    if (!MR::isNearZero(_8F8)) {
        mJumpVec = _8F8;
        cutGravityElementFromJumpVec(false);

        if (_16C.length() > 20.0f) {
            mJumpVec += -mActor->getAirGravityVec() * 10.0f;
        } else {
            f32 gravScale = mActor->_288.length();
            if (gravScale > 8.0f) {
                gravScale = 8.0f;
            }

            mJumpVec += -mActor->getAirGravityVec() * gravScale;
            _8F8.zero();
        }
    }

    TVec3f carryVec(_184);
    MR::vecKillElement(carryVec, *getGravityVec(), &carryVec);
    invalidateRelativePosition();

    TVec3f groundDeltaNoGravity;
    MR::vecKillElement(mGroundPos - mPosition, *getGravityVec(), &groundDeltaNoGravity);

    if (carryVec.dot(groundDeltaNoGravity) > 0.0f) {
        carryVec = carryVec * 0.1f;
    }

    mJumpVec += carryVec;

    _340 = 1.0f - mWalkSpeed;
    if (_340 < 0.0f) {
        _340 = 0.0f;
    }

    _430 = 0;
    procJump(true);

    mMovementStates.jumping = true;
    mMovementStates._5 = false;
    mMovementStates._E = true;
    _10._23 = false;
    mMovementStates._B = false;

    if (isAnimationRun("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9", 2) || isAnimationRun("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf", 3)) {
        _428 = 0xF;
    }

    changeAnimation(nullptr, "\x97\x8e\x89\xba");

    if (getPlayerMode() == 4) {
        _408 = mActor->getConst().getTable()->mBeeGravityPowerTimeD;
        _3BC = mActor->getConst().getTable()->mBeeAirWalkInhibitTimeD - 5;
    }

    if (isAnimationRun("\x83\x56\x83\x87\x81\x5b\x83\x67\x92\x85\x92\x6e")) {
        mMovementStates._23 = true;
    }

    if (mMovementStates._8) {
        stopWalk();
    }

    return;
}

bool Mario::isDigitalJump() const NO_INLINE {
    return mMovementStates._E;
}

void Mario::initActiveJumpVec() {
    _2EC = getAirGravityVec();
    MR::vecKillElement(mJumpVec, _2EC, &_2E0);

    if (MR::normalizeOrZero(&_2E0)) {
        _2E0 = mFrontVec;
    }
}

void Mario::initJumpParam() {
    const u32 flags1 = _1C_WORD;
    mMovementStates._13 = true;

    if ((_1C._A) || (_1C._B)) {
        _10._1F = true;
    }

    mMovementStates._38 = false;
    _10._8 = false;
    mMovementStates._2F = false;
    _10._E = false;

    if (!mDrawStates._1B) {
        initActiveJumpVec();
    }

    _42C = 0;
    _3BC = 0;
    _3CA = 0;
    _3CC = 0;
    _774 = 0;
    mMovementStates._11 = false;
    _3BE = 0;
    _344 = mSideVec;

    _408 = 0;
    _76C = 0;
    mMovementStates._C = false;
    mMovementStates._10 = false;
    mMovementStates._4 = false;
    mMovementStates._20 = false;
    _10._C = false;
    _3D2 = 0;
    _3D0 = 0;
    _3CE = 0;
    mMovementStates._2A = mMovementStates._8;

    if (_430 != 5) {
        cancelSquatMode();
    }

    _4B0 = mPosition;
    _426 = 0;

    if (mSinkTimer != 0) {
        mJumpVec.scale(0.2f + (0.8f * ((0x100 - mSinkTimer) / 256.0f)));
        mSinkTimer = 0;
    }
}

bool Mario::isEnableFutureJump() const {
    if (_430 == 0x3) {
        return false;
    }

    if (!isRising()) {
        const f32 dot = mJumpVec.dot(*getGravityVec());
        const s16 limit = mActor->getConst().getTable()->mFutureJumpReqLimitTime;
        if (mVerticalSpeed < dot * static_cast< f32 >(limit)) {
            return true;
        }
    }

    return false;
}

void Mario::procJump(bool a1) {
    f32 jumpGravity;
    f32 gravityScale = 1.0f;
    if (!a1 && !_10._1F && _430 == 0 && _3BC < 6 && mActor->isRequestJump2P()) {
        _1C_WORD |= 0x00100000;
        mMovementStates.jumping = false;
        _3BC = 6;
        _10._1F = true;
        tryJump();
        _3BC = 6;
    }

    if (mMovementStates._B) {
        procHipDrop();
        return;
    }

    if (_424 != 0) {
        _424--;
        return;
    }

    procRocketBooster();

    if (_10._13) {
        fixFrontVecByGravity();
    }

    if (_10._1F) {
        if (!mActor->isKeepJump2P()) {
            mMovementStates._13 = false;
        }
    } else if (!mActor->isKeepJump()) {
        mMovementStates._13 = false;
    }

    if (checkTrgA()) {
        _558 = mActor->_37C;
        if (isEnableFutureJump()) {
            mMovementStates._38 = true;
        }
    }

    if (mActor->isRequestJump2P()) {
        _558 = mActor->_37C;
        if (isEnableFutureJump()) {
            mMovementStates._38 = true;
        }
    }

    bool useConnectGravity = false;
    if (mMovementStates._13 && !isDigitalJump() && _430 == 9 && isRising()) {
        useConnectGravity = true;
    }

    if (a1 || mMovementStates._13 || isDigitalJump() || !isRising() || _430 == 2) {
        useConnectGravity = true;
    }

    if (_430 == 8 && _42A == 0) {
        s32 timer = _426;
        if (timer == 0) {
            useConnectGravity = true;
        } else {
            useConnectGravity = timer > (mActor->getConst().getTable()->mTornadoBoostTimer - 0x10);
        }
    }

    if (useConnectGravity) {
        MarioConstTable* pTable = mActor->getConst().getTable();
        gravityScale = pTable->mGravityRatioA;
        f32 connectRatio = _3BC <= 45 ? 0.0f : 1.0f;
        if (_3BC <= 45) {
            connectRatio = static_cast< f32 >(_3BC) / 45.0f;
        }
    }

    if (a1) {
        initJumpParam();
    } else if (mActor->isRequestHipDrop() && jumpToHipDrop()) {
        return;
    }

    if (!isRising()) {
        if (isAnimationRun("\x83\x57\x83\x83\x83\x93\x83\x76""B")) {
            stopAnimation(nullptr);
        }

        if (!mSwim->_1B2) {
            if (isAnimationRun("\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae")) {
                stopAnimation(nullptr);
            }

            if (isAnimationRun("\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76")) {
                stopAnimation(nullptr);
            }

            if (isAnimationRun("\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67")) {
                changeAnimation("\x8b\xf3\x92\x86\x88\xea\x89\xf1\x93\x5d");
            }
        }

        _3BE++;
    } else {
        f32 ceilDist = calcDistToCeil(false);
        if (ceilDist < 160.0f) {
            jumpGravity = cutGravityElementFromJumpVec(true);

            playEffectTrans("\x93\x56\x88\xe4\x83\x71\x83\x62\x83\x67", mPosition - getAirGravityVec() * ceilDist);

            f32 reduce;
            switch (_430) {
            case 4:
            case 6:
                reduce = 0.9f;
                break;
            case 5:
            default:
                reduce = 0.6f;
                break;
            }

            mJumpVec += getAirGravityVec() * jumpGravity * reduce;
        }
    }

    bool wasAirWalk = false;
    if (_3CC != 0) {
        wasAirWalk = true;
        _3CC--;
    }

    if (_3BC > 10 && checkTrgA() && (mMovementStates._17)) {
        mMovementStates._17 = false;
        if (mWall->startJump()) {
            return;
        }
    }

    mMovementStates._12 = false;
    if (_430 == 8) {
        if (checkTrgZ()) {
            resetTornado();
            cancelTornadoJump();
            stopAnimation(nullptr);
            changeAnimationInterpoleFrame(1);
            mDrawStates._8 = true;
            playSound("\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76");

            if (_42A != 0) {
                playEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67\x8f\xc1\x8b\x8e");
            }
        } else {
            mMovementStates._11 = false;
        }

        if (_42A != 0) {
            if (_402 != 0) {
                _402--;
            }

            if (_402 == 0) {
                cancelTornadoJump();
                playEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67\x8f\xc1\x8b\x8e");

                if (getPlayerMode() != 4) {
                    _402 = mActor->getConst().getTable()->mAirWalkTime;
                }
            }
        }
    }

    if (beeMarioOnAir()) {
        _76C = mActor->getConst().getTable()->mBeeGravityReviveTime;
    }

    if (_406 != 0) {
        _406--;
    }

    f32 jumpAcceleration;
    if (_430 == 0xC) {
        jumpAcceleration = 1.8f;
    } else if (_430 == 0xD) {
        jumpAcceleration = mActor->getConst().getTable()->mGravityJumping[9];
    } else {
        jumpAcceleration = mActor->getConst().getTable()->mGravityJumping[_430];
    }

    if (_430 == 0xB && (isRising() || _3BE < mActor->getConst().getTable()->mTrampleBegomaOpenTime)) {
        jumpAcceleration = mActor->getConst().getTable()->mGravityJumping[0];
    }

    if (_430 == 8 && _42A == 1) {
        jumpAcceleration = mActor->getConst().getTable()->mSpinJumpGravity;
    }

    if (_430 == 8 && !isRising()) {
        jumpAcceleration *= 0.25f;
    }

    const s32 clingNum = MR::getKarikariClingNum();
    f32 clingScale = 1.0f;
    if (clingNum != 0) {
        clingScale = (2.0f + static_cast< f32 >(clingNum)) / 2.0f;
        if (getPlayerMode() == 4) {
            clingScale = 1.0f;
        }

        jumpAcceleration *= clingScale;
    }

    if (wasAirWalk) {
        jumpAcceleration = mActor->getConst().getTable()->mGravityAirWalk;
    }

    if (_76C != 0) {
        const u16 maxTimer = mActor->mConst->getTable()->mBeeGravityReviveTime;
        const f32 t = static_cast< f32 >(_76C) / static_cast< f32 >(maxTimer);
        jumpAcceleration = ((1.0f - t) + (t * mActor->getConst().getTable()->mGravityAirWalk));
        _76C--;
        jumpAcceleration *= clingScale;
    }

    if (_430 == 0xD && (isDigitalJump() || !isRising())) {
        gravityScale = 1.0f;
    }

    gravityScale *= mActor->getGravityRatio();
    if (mActor->_334 != 0) {
        gravityScale *= 0.3f;
        if (!mActor->isInZeroGravitySpot() && mVerticalSpeed < 300.0f) {
            gravityScale = 0.0f;
            if (mVerticalSpeed < 200.0f) {
                cutGravityElementFromJumpVec(true);
                gravityScale = -0.1f;
            }
        }
    }

    if (getPlayerMode() == 6) {
        gravityScale = 0.0f;
    }

    if (mMovementStates._17) {
        f32 wallRatio = static_cast< f32 >(_3BC) / static_cast< f32 >(mActor->mConst->getTable()->mSlipUpSpdCtrlTimer);
        if (wallRatio > 1.0f) {
            wallRatio = 1.0f;
        }

        const f32 wallScale =
            mActor->getConst().getTable()->mSlipUpSpdRatio + ((1.0f - wallRatio) * (1.0f - mActor->getConst().getTable()->mSlipUpSpdRatio));
        addVelocity(mJumpVec, wallScale);

        mJumpVec += mActor->_240 * jumpAcceleration * gravityScale * wallScale;
        moveWallSlide(1.0f);
    } else if ((mMovementStates._1) == 0) {
        addVelocity(mJumpVec);

        mJumpVec += mActor->_240 * jumpAcceleration * gravityScale;
    }

    checkWallRising();
    checkWallJumpHit();

    if ((mMovementStates._1) == 0) {
        fixFrontVecFromUpSide();
    }

    decideSlipUp();

    if (_430 != 0xD || !mMovementStates._20) {
        TVec3f side;
        const f32 dropSpeed = MR::vecKillElement(mJumpVec, *getGravityVec(), &side);

        f32 maxDrop = mActor->getConst().getTable()->mMaxDropSpeed[mActor->getGravityLevel() & 0xFF];
        if (_430 == 0xB && !isRising() && _3BE >= mActor->mConst->getTable()->mTrampleBegomaOpenTime) {
            maxDrop = mActor->getConst().getTable()->mMaxDropSpeed[3];
        }

        if (getPlayerMode() == 4 && !isRising()) {
            maxDrop = mActor->getConst().getTable()->mMaxDropSpeed[4];
        }

        if (dropSpeed >= maxDrop) {
            mJumpVec = *getGravityVec() * maxDrop + side;
        }
    }

    if ((mMovementStates._1) != 0 && !a1) {
        doLanding();

        if (calcDistToCeil(false) < 80.0f) {
            bool pressedA = false;
            const HitSensor* sensorA = _730;
            if (sensorA != nullptr && MR::isSensorPressObj(sensorA)) {
                pressedA = true;
            }

            bool pressedB = false;
            const HitSensor* sensorB = mGroundPolygon->mSensor;
            if (sensorB != nullptr && MR::isSensorPressObj(sensorB)) {
                pressedB = true;
            }

            if (_730 != mGroundPolygon->mSensor && (pressedA || pressedB)) {
                mActor->setPress(0, 0);
                mActor->_3B4 = *mGroundPolygon->getNormal(0);
            }
        }
    } else {
        doAirWalk();
    }

    _3BC++;
    return;
}

void Mario::checkWallRising() {
}

void Mario::checkWallJumpHit() {
    if (_430 == 0x5) {
        if ((mMovementStates._8) && !checkWallJumpCode()) {
            const f32 wallDot = cutVecElementFromJumpVec(getWallNorm());
            if (checkWallCode("NoAction", true)) {
                return;
            }

            mJumpVec -= getWallNorm() * wallDot;

            blown(mJumpVec * 0.2f);

            TVec3f horizontal;
            MR::vecKillElement(mJumpVec, *getGravityVec(), &horizontal);
            MR::normalizeOrZero(&horizontal);
            if (!MR::isNearZero(horizontal)) {
                setFrontVecKeepUp(-horizontal);
            }

            mMovementStates._2B = true;
            _402 = 0;
            _428 = 0x3C;
        }

        return;
    }

    if (!isRising() && _3BC > 10 && (mMovementStates._8) && calcPolygonAngleD(mFrontWallTriangle) < 80.0f) {
        if ((_10._E) == 0) {
            mJumpVec = *mFrontWallTriangle->getNormal(0) * 5.0f + getAirGravityVec() * cutGravityElementFromJumpVec(true);
            _10._E = true;
            changeAnimation("\x95\xc7\x82\xcd\x82\xb6\x82\xab");
        } else {
            const f32 jumpMag = mJumpVec.length();
            cutVecElementFromJumpVec(*mFrontWallTriangle->getNormal(0));
            mJumpVec.setLength(jumpMag);
        }
    }
}

void Mario::decideSlipUp() {
    if (isAnimationRun("\x95\xc7\x82\xcd\x82\xb6\x82\xab")) {
        return;
    }

    if (!mMovementStates._9) {
        return;
    }

    if (mMovementStates._F) {
        mMovementStates._6 = false;
        return;
    }

    if (!mMovementStates._6) {
        if (checkWallCode("NotWallSlip", true)) {
            return;
        }

        if (checkWallCode("NoAction", true)) {
            return;
        }

        bool slipFront = false;
        if (isStickOn()) {
            const TVec3f& worldPadDir = getWorldPadDir();
            if (worldPadDir.dot(getWallNorm()) < -0.8f) {
                slipFront = true;
            }
        }

        if (_430 == 0x7) {
            slipFront = true;
        }

        if ((mMovementStates._2A) == 0 && (mMovementStates._8) && isRising() && slipFront) {
            mMovementStates._6 = true;
        }

        if (isRising() && getPlayerMode() != 4 && getPlayerMode() != 6 && getPlayerMode() != 5) {
            if (_4E0 < mActor->getConst().getTable()->mSlipUpContinueHeight) {
                if (mMovementStates._8 && mMovementStates._9 && slipFront && mMovementStates._29) {
                    mMovementStates._6 = true;
                    cutGravityElementFromJumpVec(true);

                    mJumpVec += -getAirGravityVec() * 30.0f;
                }
            }
        }
    } else if (!mMovementStates._7 && isRising() && !mMovementStates._8 && (mMovementStates._15)) {
        trySlipUpJump();
    }
}

void Mario::moveWallSlide(f32 a1) {
    TVec3f crossVec;
    const TVec3f& wallNorm = getWallNorm();
    crossVec.cross(*getGravityVec(), wallNorm);

    if (!MR::normalizeOrZero(&crossVec) && isStickOn()) {
        f32 dot = crossVec.dot(getWorldPadDir());
        const f32 absDot = MR::abs(dot);
        if (absDot > 0.2f) {
            if (dot < -0.2f) {
                dot = (0.2f + dot) / 0.8f;
            } else {
                dot = (dot - 0.2f) / 0.8f;
            }

            addVelocity(crossVec, (10.0f * dot) * a1);
        }
    }
}

bool Mario::jumpToHipDrop() {
    if (mMovementStates._B) {
        return false;
    }

    switch (_430) {
    case 5:
        return false;

    case 9:
    case 11:
    case 12:
    case 13:
        if (getPlayerMode() != 4 && isRising()) {
            return false;
        }
    }

    if (mVerticalSpeed < mActor->getConst().getTable()->mHipDropLimitHeight) {
        return false;
    }

    if (MR::isNearZero(mActor->_240)) {
        return false;
    }

    if (getPlayerMode() == 6) {
        return false;
    }

    bool isNormalDrop = false;
    if (mActor->_3E5) {
        isNormalDrop = true;
    }

    resetTornado();
    cancelTornadoJump();

    mDrawStates._8 = true;
    mMovementStates._21 = false;
    mMovementStates._B = true;
    mJumpVec.zero();
    _422 = 0;

    if (_430 == 4) {
        setFrontVecKeepUp(-_220);
        _430 = 0;
    }

    if (isAnimationRun("\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45")) {
        stopAnimationUpper(nullptr);
    }

    if (isPlayerModeHopper()) {
        _720 = getAnimationStringPointer("\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e");
        _724 = getAnimationStringPointer("\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76");
        _728 = getAnimationStringPointer("\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
        startHipDropBlur();
    } else if (getPlayerMode() == 4) {
        _720 = getAnimationStringPointer("\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e");
        _724 = getAnimationStringPointer("\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76");

        if (mActor->mBeeWallWalk != 0) {
            _728 = getAnimationStringPointer("\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x95\xc7\x92\x85\x92\x6e");
        } else {
            _728 = getAnimationStringPointer("\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
        }
    } else if (isNormalDrop) {
        _720 = getAnimationStringPointer("\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e");
        _724 = getAnimationStringPointer("\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76");
        _728 = getAnimationStringPointer("\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
        stopEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
        _10._27 = true;
    } else {
        _720 = getAnimationStringPointer("\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e");
        _724 = getAnimationStringPointer("\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76");
        _728 = getAnimationStringPointer("\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
        startHipDropBlur();
    }

    changeAnimation(_720, _724);
    playSound("\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x89\xf1\x93\x5d");

    if (isNormalDrop) {
        playSound("\x90\xba\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76");
        playSound("\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x89\xf1\x93\x5d");

    } else {
        playSound("\x90\xba\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76");
    }

    _424 = mActor->getConst().getTable()->mHipDropZeroGrTime;
    mRabbit->forceJump();
    mMovementStates._3E = 0;
    return true;
}

void Mario::procHipDrop() {
    f32 gravityHipDrop;
    if (isAnimationRun(_720)) {
        if (!isAnimationTerminate(nullptr)) {
            return;
        }

        if (_424 != 0) {
            _424--;
            return;
        }

        stopAnimation(nullptr);
        return;
    }

    if (_424 != 0) {
        _424--;
        return;
    }

    if (mActor->_38C != 0) {
        return;
    }

    if (MR::isNearZero(mActor->_240)) {
        mMovementStates.digitalJump = true;
    }

    if (mActor->_334 != 0) {
        _2F8.x *= 0.5f;
        _2F8.y *= 0.5f;
        _2F8.z *= 0.5f;
        mJumpVec.x *= 0.5f;
        mJumpVec.y *= 0.5f;
        mJumpVec.z *= 0.5f;
    }

    if ((mMovementStates._1) != 0 || isAnimationRun(_728)) {
        fixFrontVecByGravity();

        if (isAnimationRun(_728)) {
            bool shouldEnd = false;

            if (mActor->_334 != 0) {
                shouldEnd = true;
            } else if ((mMovementStates._1) == 0 && _3BC > 3 && mVerticalSpeed > 10.0f) {
                changeAnimation(_724);
                mJumpVec = _2F8;
                goto PROC_HIP_DROP_MOVE;
            }

            u16 endLimit = 0xF;
            if (isAnimationRun("\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e")) {
                endLimit = 0x2D;
            }

            if (_3CE > endLimit) {
                if (isStickOn()) {
                    shouldEnd = true;
                }

                if (mDrawStates._C) {
                    shouldEnd = true;
                }
            }

            if (_3CE > 5 && checkTrgA()) {
                shouldEnd = true;
            }

            if (isAnimationTerminate(nullptr) || shouldEnd) {
                mMovementStates.jumping = false;
                mMovementStates._B = false;
                stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                stopEffect("\x91\xae\x90\xab\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76");

                if (checkTrgA()) {
                    tryJump();
                    return;
                }
            }

            mWalkSpeed = 0.0f;
        } else {
            _3CE = 0;
            mWalkSpeed = 0.0f;
            _71E = 0;

            if (isCurrentFloorSand()) {
                changeStatus(mBury);
                return;
            }

            if (isCurrentFloorSink()) {
                mMovementStates.jumping = false;
                mMovementStates._B = false;
                stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                mSinkTimer = 200;
                return;
            }

            playSound("\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
            playSound("\x90\xba\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e");
            playEffectRT("\x91\xae\x90\xab\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76", _368, mPosition);
            startPadVib("\x8d\xc5\x8b\xad");
            startCamVib(0);
            mDrawStates._14 = true;
            MR::removeAllClingingKarikari();

            if (_960 == 0x1B || _960 == 0x1C || _960 == 9) {
                mMovementStates.jumping = false;
                mMovementStates._B = false;
                stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                return;
            }

            changeAnimationWithAttr(_728, 1);
            _2F8 = mJumpVec;
            clearSlope();

            if (mActor->_B90) {
                mMovementStates.jumping = false;
                mMovementStates._B = false;
            }
        }

        mJumpVec.zero();
    } else {
        _3BC++;

        if (MR::isNearZero(mActor->getLastMove(), 0.001f)) {
            _422++;
            if (_422 == 0xF) {
                stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                mMovementStates.jumping = false;
                mMovementStates._B = false;
                mJumpVec.zero();
                mMovementStates._1 = true;
                return;
            }
        }
    }

PROC_HIP_DROP_MOVE:
    TVec3f moveVec(mJumpVec);
    if (moveVec.length() >= mVerticalSpeed) {
        moveVec.setLength(mVerticalSpeed);
    }

    addVelocity(moveVec);

    if (!mMovementStates._1) {
        const f32 speedRate = MR::clamp(0.1f + (1.0f - (mJumpVec.length() / mActor->getConst().getTable()->mLimitSpeedHipDrop)), 0.0f, 1.0f);
        gravityHipDrop = mActor->getConst().getTable()->mGravityHipDrop;

        mJumpVec += *getGravityVec() * gravityHipDrop * speedRate;

        const f32 jumpGravity = cutGravityElementFromJumpVec(true);
        if (mJumpVec.length() > 10.0f) {
            mJumpVec.x *= 0.5f;
            mJumpVec.y *= 0.5f;
            mJumpVec.z *= 0.5f;
        }

        mJumpVec += getAirGravityVec() * jumpGravity;

        if (mJumpVec.length() > mActor->getConst().getTable()->mLimitSpeedHipDrop) {
            mJumpVec.setLength(mActor->getConst().getTable()->mLimitSpeedHipDrop);
        }

        if (_10._27 && isAnimationRun("\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76")) {
            playSound("\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x97\x8e\x89\xba");

            Triangle strikeTriangles[0x20];
            HitSensor* homingSensor = nullptr;

            f32 maxAngle = 45.0f;
            f32 speed = mVerticalSpeed;
            if (speed >= 500.0f) {
                speed = 500.0f;
            }

            s32 strikeNum = Collision::checkStrikeBallToMapWithThickness(mShadowPos, speed, speed, nullptr, nullptr);

            for (u32 i = 0; i < strikeNum; i++) {
                const HitInfo* strikeInfo = Collision::getStrikeInfoMap(i);

                if (mActor->selectHomingInSuperHipDrop(strikeInfo->mParentTriangle.mSensor->mHost->mName)) {
                    TVec3f sensorPos(strikeInfo->mParentTriangle.mSensor->mPosition);

                    if (MR::diffAngleAbs(sensorPos - mActor->_2A0, *getGravityVec()) < maxAngle) {
                        homingSensor = strikeInfo->mParentTriangle.mSensor;
                    }
                }
            }

            if (homingSensor == nullptr) {
                homingSensor = reinterpret_cast< HitSensor* >(mActor->_4A8);
            }

            if (homingSensor != nullptr) {
                TVec3f targetJump(homingSensor->mPosition - mActor->_2A0);
                targetJump.setLength(mJumpVec.length());

                TVec3f lastMove;
                mActor->getLastMove(&lastMove);
                if (lastMove.dot(mJumpVec) < 0.0f) {
                    const f32 hitGravity = cutGravityElementFromJumpVec(true);
                    mJumpVec = -mJumpVec + getAirGravityVec() * hitGravity;
                } else if (mJumpVec.dot(targetJump) > 0.0f) {
                    mJumpVec = targetJump;
                }
            }

            if ((mMovementStates._8) || (mMovementStates._1A) || (mMovementStates._19)) {
                _10._27 = false;
            }
        }

        fixFrontVecFromUpSide();
    }
}

void Mario::doAirWalk() {
    if (isAnimationRun("\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67")) {
        return;
    }

    if (_430 == 3) {
        return;
    }

    if (mMovementStates._2F) {
        return;
    }

    if (isAnimationRun("\x95\xc7\x82\xcd\x82\xb6\x82\xab")) {
        return;
    }

    if (mMovementStates._1D) {
        if (_3BC > mActor->getConst().getTable()->mWaitNeutralTimer) {
            mMovementStates._1D = false;
        }

        return;
    }

    if (_428 != 0) {
        _428--;
        if (_428 == 0) {
            stopAnimation("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9", 2);
            stopAnimation("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf", 3);
        }

        return;
    }

    f32 speedKiller;
    TVec3f moveDir;
    TVec3f sideMove;
    calcMoveDir(mStickPos.x, mStickPos.y, &moveDir, true);

    if (mActor->_334 != 0) {
        moveDir.zero();
    }

    if (mDrawStates._D) {
        if (MR::diffAngleAbs(getAirGravityVec(), _2EC) > 0.7853982f) {
            TVec3f sideVec;
            sideVec.cross(mFrontVec, getAirGravityVec());
            MR::normalizeOrZero(&sideVec);

            Mtx rotMtx;
            PSMTXRotAxisRad(rotMtx, &sideVec, MR::diffAngleSignedHorizontal(_2EC, getAirGravityVec(), sideVec));

            TVec3f frontRot;
            PSMTXMultVecSR(rotMtx, &_2E0, &frontRot);
            if (frontRot.dot(mFrontVec) < 0.0f) {
                moveDir = -mFrontVec;
            } else {
                moveDir = mFrontVec;
            }
        }
    }

    MR::vecKillElement(moveDir, *getGravityVec(), &moveDir);

    bool inhibitFrontAdjust = false;
    f32 frontDot = MR::vecKillElement(moveDir, mFrontVec, &sideMove);

    if (_430 == 0xB || _430 == 8 || getPlayerMode() == 4 || getPlayerMode() == 6) {
        inhibitFrontAdjust = true;
    } else if (frontDot >= 0.0f) {
        f32 frontReduction = mActor->getConst().getTable()->mJumpFrontReduction;
        s16 reduceBegin = mActor->getConst().getTable()->mJumpFrontReductionBeginTime;
        if (_430 == 5) {
            frontReduction = mActor->getConst().getTable()->mSquatJumpFrontReduction;
            reduceBegin = mActor->getConst().getTable()->mSquatJumpFrontReductionBTime;
        }

        if (_10._17 && _8D8 == _45C->mSensor) {
            reduceBegin = 0xB4;
            if (MR::isSameMtx(_8E8->getBaseMtx()->toMtxPtr(), _8E8->getPrevBaseMtx()->toMtxPtr())) {
                MR::vecKillElement(_8DC, getAirGravityVec(), &_8DC);
                mJumpVec -= _8DC;
                _8DC.zero();
            }
        }

        if ((mMovementStates._11) == 0 && _3BC > reduceBegin) {
            const f32 jumpGravity = cutGravityElementFromJumpVec(true);
            const f32 jumpMag = mJumpVec.length();
            mJumpVec.setLength(frontReduction * jumpMag);

            mJumpVec += getAirGravityVec() * jumpGravity;
        }
    } else {
        if (!isRising() && mVerticalSpeed < mActor->getConst().getTable()->mLandTurnHeight && (mMovementStates._11) == 0) {
            mMovementStates._10 = true;
        } else {
            _3CA++;
        }

        if (isAnimationRun("\x83\x5e\x81\x5b\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76")) {
            _3CA = 0;
            speedKiller = 0.1f;
        } else if (_3CA < mActor->mConst->getTable()->mBackJumpLimitFrame) {
            const f32 remain = static_cast< f32 >(mActor->getConst().getTable()->mBackJumpLimitFrame - _3CA);
            speedKiller =
                mActor->getConst().getTable()->mBackJumpRatio * (remain / static_cast< f32 >(mActor->getConst().getTable()->mBackJumpLimitFrame));
        } else {
            speedKiller = 0.0f;
        }

        if (mMovementStates._11) {
            moveDir = sideMove + mFrontVec * mActor->getConst().getTable()->mAirWalkBackBonus * frontDot;
        } else {
            moveDir = sideMove + mFrontVec * frontDot * speedKiller;
        }

        const f32 backDot = -moveDir.dot(mFrontVec);
        if (backDot > mActor->getConst().getTable()->mMaxBackJumpSpeed) {
            moveDir.setLength(mActor->getConst().getTable()->mMaxBackJumpSpeed);
        } else if (backDot < 0.0f && _3CA < mActor->mConst->getTable()->mBackJumpLimitFrame) {
            _3CC = 0xA;
        }
    }

    TVec3f moveNorm(moveDir);
    MR::normalizeOrZero(&moveNorm);

    TVec3f jumpNorm(mJumpVec);
    MR::normalizeOrZero(&jumpNorm);

    if (!inhibitFrontAdjust) {
        f32 alignment = moveNorm.dot(jumpNorm);
        if (alignment < 0.0f) {
            const f32 jumpGravity = cutGravityElementFromJumpVec(true);
            mJumpVec += mJumpVec * mActor->getConst().getTable()->mAirWalkSpeedKiller * alignment;

            mJumpVec += getAirGravityVec() * jumpGravity;
        }
    }

    if (_430 == 8) {
        mJumpVec += moveDir * mActor->getConst().getTable()->mWalkSpeed * 5.0f / mActor->getConst().getTable()->mAirWalkTimerFact2;
    } else if (_430 == 0xB && !isRising()) {
        mJumpVec += moveDir * mActor->getConst().getTable()->mWalkSpeed * 5.0f / mActor->getConst().getTable()->mAirWalkTimerFact2;
        playSound("\x91\xd8\x8b\xf3\x92\x86");
    } else if (getPlayerMode() == 4) {
        mJumpVec += moveDir * mActor->getConst().getTable()->mBeeAirWalkAcc;
        playSound("\x91\xd8\x8b\xf3\x92\x86");
    } else if (getPlayerMode() == 6) {
        if (mJumpVec.dot(moveDir) >= 0.0f) {
            f32 frontGravity = moveDir.dot(mFrontVec);
            if (frontGravity > 0.0f) {
                frontGravity = -frontGravity;
            }

            TVec3f normJump(mJumpVec);
            if (!MR::normalizeOrZero(&normJump)) {
                f32 alignment = normJump.dot(mFrontVec);
                alignment += 1.0f;
                f32 halfAlignment = alignment / 2.0f;
                mJumpVec *= 0.998f - 0.01f * (1.0f - halfAlignment);
            }
        }
    } else {
        mJumpVec += moveDir * mActor->getConst().getTable()->mWalkSpeed * mActor->getConst().getTable()->mAirWalkTimerFact1 /
                    (_3BC + mActor->getConst().getTable()->mAirWalkTimerFact2);
    }

    f32 accelerationRate = 1.0f;
    if (_430 != 0xB && getPlayerMode() != 6 && !isRising()) {
        addVelocity(moveDir, mActor->getConst().getTable()->mWalkSpeed * mActor->getConst().getTable()->mAirWalkTimerFact3 * accelerationRate);
    }

    const f32 jumpGravity = cutGravityElementFromJumpVec(true);
    if (_430 == 8) {
        if (mJumpVec.length() > 6.0f) {
            mJumpVec.setLength(6.0f);
        }
    } else if (_430 == 0xB) {
        if (mJumpVec.length() > 7.0f) {
            mJumpVec.setLength(7.0f);
        }
    } else if (getPlayerMode() == 4) {
        if (_774 == 0) {
            if (mJumpVec.length() > mActor->getConst().getTable()->mBeeAirWalkLimit) {
                mJumpVec.setLength(mActor->getConst().getTable()->mBeeAirWalkLimit);
            }
        } else if (_774 > 0x1E) {
            if (mJumpVec.length() > mActor->getConst().getTable()->mMaxJumpSpeed) {
                mJumpVec.setLength(mActor->getConst().getTable()->mMaxJumpSpeed);
            }
        } else {
            f32 t = static_cast< f32 >(_774) / 30.0f;
            const f32 limit = (t * mActor->mConst->getTable()->mMaxJumpSpeed) + ((1.0f - t) * mActor->getConst().getTable()->mBeeAirWalkLimit);
            if (mJumpVec.length() > limit) {
                mJumpVec.setLength(limit);
            }
        }
    } else if (getPlayerMode() != 6) {
        if (mJumpVec.length() > mActor->getConst().getTable()->mMaxJumpSpeed) {
            mJumpVec.setLength(mActor->getConst().getTable()->mMaxJumpSpeed);
        }
    }

    mJumpVec += getAirGravityVec() * jumpGravity;
}

void Mario::stopJump() {
    fixFrontVecByGravity();
    mJumpVec.zero();

    mMovementStates.jumping = false;
    mMovementStates._B = false;
    mMovementStates._6 = false;

    mMovementStates._28 = false;
    mMovementStates._2B = false;

    _10._17 = false;
    _10._19 = false;
    _10._1F = false;

    if (getPlayerMode() != 4) {
        _402 = mActor->getConst().getTable()->mAirWalkTime;
    }

    _3BC = 0;
    _424 = 0;
    _426 = 0;
    _428 = 0;

    if (isDefaultAnimationRun("\x97\x8e\x89\xba")) {
        changeAnimation(nullptr, "\x8a\xee\x96\x7b");
    }

    if (isDefaultAnimationRun(_724)) {
        changeAnimation(nullptr, "\x8a\xee\x96\x7b");
    }

    mWall->_1C = 0;
}

void Mario::cancelTornadoJump() {
    _430 = 0;

    if ((mMovementStates._1) == 0) {
        stopAnimation("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76", "\x97\x8e\x89\xba");
    } else {
        stopAnimation("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
    }

    _4B0 = mPosition;
    mMovementStates._2B = true;
    _426 = 0;
}

void Mario::setRocketBooster(const TVec3f& rVec, f32 a2, u16 a3) {
    _426 = a3;
    _454 = a2;
    _448 = rVec;
}

void Mario::procRocketBooster() {
    if (_426 == 0) {
        return;
    }

    _426--;
    mJumpVec += _448;
    _448.scale(_454);
}

bool Mario::isSoftLandingFloor() const {
    switch (_960) {
    case 0x6:
    case 0x7:
    case 0x8:
    case 0xB:
    case 0x22:
        return true;
    default:
        return false;
    }
}

void Mario::checkAndTryForceJump() {
    bool doForceJump = false;
    TVec3f forceJumpVec;

    if ((getMovementStates()._1 && !mMovementStates.jumping) || isAnimationRun(_728)) {
        switch (_960) {
        case 0x6:
        case 0x22: {
            const f32 jumpPower = mActor->getConst().getTable()->mCodeJumpPower[0];
            forceJumpVec = *mGroundPolygon->getNormal(0) * jumpPower;
            doForceJump = true;
            break;
        }

        case 0x7: {
            const f32 jumpPower = mActor->getConst().getTable()->mCodeJumpPower[1];
            forceJumpVec = *mGroundPolygon->getNormal(0) * jumpPower;
            doForceJump = true;
            break;
        }

        case 0x8: {
            const f32 jumpPower = mActor->getConst().getTable()->mCodeJumpPower[2];
            forceJumpVec = *mGroundPolygon->getNormal(0) * jumpPower;
            doForceJump = true;
            break;
        }

        case 0xB: {
            MR::vecKillElement(_16C, *getGravityVec(), &forceJumpVec);
            forceJumpVec.setLength(10.0f);

            const f32 jumpPower = mActor->getConst().getTable()->mCodeJumpPower[3];
            forceJumpVec += -(*getGravityVec()) * jumpPower;
            doForceJump = true;
            break;
        }

        default:
            break;
        }
    }

    if (mMovementStates._2D) {
        doForceJump = false;

        if (mActor->mHealth != 0) {
            if (getCurrentStatus() == 2) {
                closeStatus(nullptr);
            }

            tryForceFreeJump(_304);
        }

        mMovementStates._2D = false;
    }

    if (mMovementStates._2E) {
        doForceJump = false;

        if (mActor->mHealth != 0) {
            if (getCurrentStatus() == 2) {
                closeStatus(nullptr);
            }

            tryFreeJump(_304, true);
        }

        mMovementStates._2E = false;
    }

    if (doForceJump) {
        mActor->sendMsgToSensor(mGroundPolygon->mSensor, 2);

        if (getCurrentStatus() == 2) {
            if (mActor->mHealth == 0) {
                mActor->forceGameOverNonStop();
            }

            closeStatus(nullptr);
        }

        mMovementStates._21 = true;
        mMovementStates._2E = true;
        _304 = forceJumpVec;
        changeAnimation("\x83\x74\x83\x8a\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76");

        if (mActor->isEnableNerveChange()) {
            playSound("\x83\x67\x83\x89\x83\x93\x83\x7c\x83\x8a\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76\x91\xe5");
            playSound("\x90\xba\x91\xe5\x83\x57\x83\x83\x83\x93\x83\x76");
            playSound("\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8");
        }
    }
}

void Mario::doLanding() {
    bool doHardLanding = false;
    bool blendWait = false;
    bool keepFrontSlip = false;

    if (getPlayerMode() == 6) {
        return;
    }

    if (mMovementStates._37) {
        afterLanding2D();
    }

    mMovementStates.jumping = false;
    _10._1F = false;
    _10._21 = false;
    _3CE = 0;
    _1C_WORD |= 0x2000;

    f32 deltadot = MR::abs((mPosition - _4B0).dot(*getGravityVec()));
    if (_3BC <= 3 && deltadot < 1.0f && mJumpVec.length() < 10.0f) {
        stopAnimation(nullptr, "\x8a\xee\x96\x7b");
        return;
    }

    if (mActor->_EEB && (mMovementStates._2B) && _10._19 && _430 != 3) {
        _10._19 = false;
        mActor->_946 = 0x10;
    }

    resetTornado();

    if (mActor->mBeeWallWalk != 0) {
        playSound("\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab");
    } else {
        playSound("\x92\x85\x92\x6e");
    }

    if ((isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76") || isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2") || isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3")) &&
        (getPlayerMode() == 3 || getPlayerMode() == 0)) {
        playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e");
        if (getStickP() == 0.0f) {
            mJumpVec.zero();
            stopWalk();
            changeAnimation("\x83\x58\x83\x50\x81\x5b\x83\x67\x90\xc3\x8e\x7e\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        } else {
            changeStatus(mSkate);
        }

        return;
    }

    if (_430 == 5 && isSkatableFloor()) {
        if (checkSquat(false)) {
            mMovementStates._A = true;
            mWalkSpeed = 1.0f;
            stopAnimation(nullptr, "\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b");
        } else {
            cancelSquatMode();
            stopAnimationUpper(nullptr);
            changeAnimation("\x83\x58\x83\x50\x81\x5b\x83\x67\x90\xc3\x8e\x7e\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        }

        return;
    }

    if (isAnimationRun("\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x92\x85\x92\x6e")) {
        stopAnimation(nullptr, "\x8a\xee\x96\x7b");
        return;
    }

    if (getPlayer()->_10._2) {
        getPlayer()->_10._2 = false;
        _430 = 0xE;
    } else {
        const u32 landingType = mMovementStates._3E;
        if (landingType == 2) {
            getPlayer()->mMovementStates._3E = 0;
            stopWalk();
            playSound("\x8f\x64\x82\xa2\x92\x85\x92\x6e");
            playSound("\x90\xba\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93\x92\x85\x92\x6e");
            changeAnimation("\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""B", "\x8a\xee\x96\x7b");
            startCamVib(4);
            return;
        }

        if (landingType == 1) {
            getPlayer()->mMovementStates._3E = 0;
            playEffect("\x83\x58\x81\x5b\x83\x70\x81\x5b\x83\x58\x83\x73\x83\x93\x83\x68\x83\x89\x83\x43\x83\x6f\x8f\x49\x97\xb9");
            playEffect("\x8b\xa4\x92\xca\x92\x85\x92\x6e\x91\xe5");
            if (!isStickFull()) {
                keepFrontSlip = true;
            }

            doHardLanding = true;
            if (_1FC.dot(_368) < 0.0f) {
                if (_960 == 0xD || _960 == 0x1E || _960 == 5 || _960 == 0x17) {
                    changeStatus(mSukekiyo);
                } else {
                    stopWalk();
                    startCamVib(4);
                    changeAnimation("\x83\x6e\x81\x5b\x83\x68\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
                    _3CE += 0x10;
                    forceSetHeadVecKeepSide(_368);
                }

                return;
            }
        } else {
            if (isCurrentFloorSink()) {
                stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                mSinkTimer = 64;
                return;
            }

            if ((_960 == 0x1B || _960 == 0x1C)) {
                if (!strcmp(MR::getSoundCodeString(_45C), "Sand")) {
                    stopAnimation(nullptr, "\x8a\xee\x96\x7b");
                    mSinkTimer = 32;
                    return;
                }
            } else if (_3BC > 10) {
                playEffect("\x8b\xa4\x92\xca\x92\x85\x92\x6e\x95\x81\x92\xca");
            }
        }
    }

    switch (_430) {
    case 0:
    case 0xA:
    case 0xB:
    case 0xE:
        startPadVib(static_cast< u32 >(0));
        break;
    default:
        startPadVib(2);
        break;
    case 2:
    case 4:
    case 6:
        startPadVib(3);
        break;
    }

    getAnimator()->addRumblePower(5.0f, 0x3C);
    mMovementStates._21 = false;

    if (getPlayerMode() != 4) {
        _402 = mActor->getConst().getTable()->mAirWalkTime;
    } else {
        _71E = 0;
    }

    if (!isSoftLandingFloor()) {
        f32 deltadot = (_4B0 - mPosition).dot(-(*getGravityVec()));
        if (deltadot > mActor->getConst().getTable()->mHardLandingHeight) {
            doHardLanding = true;
        }
    }

    if (doHardLanding && isCurrentFloorSand()) {
        changeStatus(mBury);
        return;
    }

    if (isBlendWaitGround()) {
        blendWait = true;
    }

    fixFrontVecByGravity();

    if (!doHardLanding && !mDrawStates._C && _430 != 0xE && !isAnimationRun("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76") && !isAnimationRun("\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76") &&
        !_10._8 && !isAnimationRun("\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86")) {
        if (_430 == 5 && checkSquat(false)) {
            mMovementStates._A = true;
            mWalkSpeed = 1.0f;
            stopAnimation(nullptr, "\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b");
            goto POST_LANDING;
        }

        if (((isStickFull() || _60D) && _3CA == 0) || (((mMovementStates._B) == 0) && checkSquat(false))) {
            stopAnimation(nullptr, "\x8a\xee\x96\x7b");
            if (!_10._A) {
                recordTurnSlipAngle();
            }

            if (_430 == 5) {
                cancelSquatMode();
            }

            goto POST_LANDING;
        }

        switch (_430) {
        case 2:
        case 4:
        case 5:
        case 6:
        case 8:
        case 0xB:
            keepFrontSlip = true;
            break;
        default:
            break;
        }

        if (_10._17 && _8D8 == _45C->mSensor) {
            TVec3f jumpDelta(mJumpVec - _8DC);
            if (jumpDelta.dot(mJumpVec) < 0.0f) {
                mJumpVec.zero();
            } else {
                mJumpVec = jumpDelta;
            }
        }

        const f32 frontDot = mFrontVec.dot(mJumpVec);
        if (frontDot > 6.0f && !keepFrontSlip) {
            mWalkSpeed = frontDot / mActor->getConst().getTable()->mJumpFrontSpeed;
            mWalkSpeed = MR::clamp(mWalkSpeed, 0.0f, 2.0f);
            stopAnimation(nullptr, "\x8a\xee\x96\x7b");
            _3FA = 0;
            _71E = 0;

            if (!_10._A) {
                recordTurnSlipAngle();
            }

            goto POST_LANDING;
        }
    }

    if (_430 == 4) {
        setFrontVecKeepUp(-_220);
    }

    if (isAnimationRun("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76") || isAnimationRun("\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76") || _10._8) {
        if (isAnimationRun("\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76")) {
            setFrontVecKeepUp(-mFrontVec);
        }

        if (_10._8) {
            doHardLanding = true;
        }

        stopWalk();
        if (doHardLanding || isAnimationRun("\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76")) {
            if (doHardLanding) {
                changeAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x89\xf1\x93\x5d\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
                startSlidingTask(8, 5.0f, 0x19);
                startCamVib(0);
            } else {
                changeAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            }
        } else {
            changeAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        }

        goto POST_LANDING;
    }

    if (!mDrawStates._C) {
        clearSlope();
        switch (_430) {
        case 0:
        case 7:
            changeAnimation("\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            break;
        case 1:
            changeAnimation("\x92\x85\x92\x6e""B", "\x8a\xee\x96\x7b");
            break;
        case 2:
            changeAnimation("\x92\x85\x92\x6e""C", "\x8a\xee\x96\x7b");
            break;
        case 4:
            changeAnimation("\x92\x85\x92\x6e\x83\x5e\x81\x5b\x83\x93", "\x8a\xee\x96\x7b");
            break;
        case 5:
            if (!checkSquat(false)) {
                cancelSquatMode();
            }

            if (mActor->_468 == 0) {
                stopAnimationUpper(nullptr);
            }

            changeAnimation("\x92\x85\x92\x6e\x95\x9d\x82\xc6\x82\xd1", "\x8a\xee\x96\x7b");
            break;
        case 6:
            changeAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            break;
        default:
            if (isAnimationRun("\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86")) {
                changeAnimation("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            } else {
                changeAnimation("\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            }

            break;
        case 3:
        case 0xA:
            stopAnimation(nullptr, "\x8a\xee\x96\x7b");
            break;
        case 0xE:
            playSound("\x93\x7c\x82\xea");
            playSound("\x92\x85\x92\x6e");
            playSound("\x90\xba\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
            playEffect("\x8b\xa4\x92\xca\x88\xf8\x82\xab\x96\xdf\x82\xb5\x92\x85\x92\x6e");
            changeAnimation("\x88\xf8\x82\xab\x96\xdf\x82\xb5\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            doHardLanding = false;
            blendWait = false;
            break;
        }

        stopWalk();
        if (blendWait) {
            changeAnimation("\x83\x56\x83\x87\x81\x5b\x83\x67\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        }

        if (doHardLanding) {
            changeAnimation("\x83\x6e\x81\x5b\x83\x68\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
            playSound("\x8f\x64\x82\xa2\x92\x85\x92\x6e");
            playSound("\x90\xba\x92\x85\x92\x6e\x92\xe2\x8e\x7e");
            startCamVib(4);
            _3CE += 0x10;
        }

        if (mActor->mBeeWallWalk != 0) {
            changeAnimation("\x83\x6e\x83\x60\x99\xb3\x99\xb4\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        }
    } else {
        if (!isAnimationRun("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9", 2) && !isAnimationRun("\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf", 3)) {
            stopAnimation(nullptr, "\x8a\xee\x96\x7b");
        }

        changeAnimation("\x82\xb7\x82\xd7\x82\xe8\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");
        _71E = 0;
    }

POST_LANDING:
    if (mDrawStates._C) {
        TVec3f jumpNoGrav;
        MR::vecKillElement(mJumpVec, *getGravityVec(), &jumpNoGrav);

        if (jumpNoGrav.dot(*mGroundPolygon->getNormal(0)) > 0.0f) {
            mMovementStates._23 = true;
            _8F0 = 10.0f;
        } else {
            const f32 ratio = (90.0f - calcPolygonAngleD(mGroundPolygon)) / 90.0f;
            mWalkSpeed = mWalkSpeed * ratio;
            mWalkSpeed = MR::clamp(mWalkSpeed, 0.0f, 2.0f);

            const f32 minVel = 10.0f - (10.0f * ratio);
            if (_8F0 < minVel) {
                _8F0 = minVel;
            }
        }

        if (jumpNoGrav.length() < _8F8.length()) {
            _8F8.setLength(0.5f * (_8F8.length() + jumpNoGrav.length()));
        }
    } else {
        _8F8.zero();
    }

    return;
}

void Mario::startSlidingTask(u32 a1, f32 a2, u16 a3) {
    _A40 = mFrontVec * a2;
    _A3C = a3;
    pushTask(reinterpret_cast< Task >(&Mario::taskOnSlide), a1);
}

bool Mario::taskOnSlide(u32 a1) {
    (void)a1;
    addVelocity(_A40);
    _A3C--;
    return _A3C != 0;
}

void MarioJump_FORCE_MATCH_DATA() {
    Mario::Task task = &Mario::taskOnWallRising;
    (void)task;
}

bool Mario::taskOnWallRising(u32 a1) {
    (void)a1;

    if ((mMovementStates._17) == 0) {
        stopEffect("\x8b\xa4\x92\xca\x95\xc7\x8f\xe3\x8f\xb8");
        return false;
    }

    playSound("\x83\x58\x83\x8a\x83\x62\x83\x76");
    return true;
}

void Mario::incAirWalkTimer() {
    const u16 timer = _402;
    const u16 limit = mActor->getConst().getTable()->mAirWalkTime;

    if (timer >= limit) {
        return;
    }

    _402 = timer + 1;

    if (getPlayerMode() != 4) {
        return;
    }

    const u16 nextTimer = _402;
    const u16 nextLimit = mActor->getConst().getTable()->mAirWalkTime;

    if (nextTimer >= nextLimit) {
        playSound("\x83\x6e\x83\x60\x91\xcc\x97\xcd\x8a\xae\x91\x53\x89\xf1\x95\x9c");
    } else {
        playSound("\x83\x6e\x83\x60\x91\xcc\x97\xcd\x89\xf1\x95\x9c");
    }
}
