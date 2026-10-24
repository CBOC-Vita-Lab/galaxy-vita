#include "Game/Enemy/KarikariDirector.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Map/CollisionParts.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioMapCode.hpp"
#include "Game/Player/MarioModule.hpp"
#include "Game/Player/MarioMove.hpp"
#include "Game/Player/MarioSwim.hpp"
#include "Game/Util.hpp"
#include <cstdio>

namespace {
    static f32 sSpeedTableA[] = {0.15f, 0.3f, 0.45f, 0.6f, 0.7f, 0.85f, 0.99f};
    static f32 sSpeedTableB[] = {0.02f, 0.2f, 0.4f, 0.5f, 0.65f, 0.75f, 0.98f};
    static f32 sWalkTargetTable[] = {0.0f, 0.15f, 0.25f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f};
    static f32 sWeightTable[8][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.0f},   {0.75f, 0.25f, 0.0f, 0.0f}, {0.25f, 0.75f, 0.0f, 0.0f},
                                     {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.75f, 0.25f, 0.0f}, {0.0f, 0.25f, 0.75f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
    static f32 sFootStep[] = {1.2f, 1.5f, 1.3f, 0.0f};
    static f32 sFootStepBeeWallWalk[] = {0.5f, 0.5f, 0.5f, 0.0f};
};  // namespace

void Mario::stopWalk() {
    mTargetWalkSpeedIndex = 0;
    mWalkSpeed = 0.0f;
    _71E = 0;
    _71F = 0;
    _3D4 = 0;

    getAnimator()->initWalkWeight();
    getAnimator()->resetTilt();

    mMovementStates._10 = false;
    mMovementStates._23 = false;
    cancelSquatMode();

    mMovementStates.turning = false;
    _3D2 = 0;
    _8F0 = 0.0f;
    _3F4 = 0.0f;

    stopEffect("\x8b\xa4\x92\xca\x83\x58\x83\x8a\x83\x62\x83\x76\x8d\xe2");
    stopAnimation("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1);
    stopAnimation("\x83\x75\x83\x8c\x81\x5b\x83\x4c");
}

void Mario::cancelSquatMode() {
    if (!mMovementStates._A) {
        return;
    }

    calcDistToCeil(false);

    if (_20._A || mMovementStates._A) {
        _10._F = true;
    }

    mMovementStates._A = false;
    _20._A = false;
    stopAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b");

    if (mMovementStates._1 || !mMovementStates.jumping) {
        if (!isSwimming()) {
            changeAnimation(nullptr, "\x8a\xee\x96\x7b");
        }

        if (!isAnimationRun("\x83\x54\x83\x7d\x81\x5b\x83\x5c\x83\x8b\x83\x67") && _10._F) {
            if (mWalkSpeed > 0.1f) {
                changeAnimationUpperWeak("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9");
            } else {
                changeAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9");
            }
        }
    }

    mMovementStates._26 = false;
}

f32 Mario::getTargetWalkSpeed() const {
    if (mMovementStates._A) {
        return 0.0f;
    }

    f32 targetWalkSpeed = ::sWalkTargetTable[mTargetWalkSpeedIndex];

    if (mSinkTimer != 0) {
        if (checkCurrentFloorCodeSevere(25)) {
            targetWalkSpeed = 0.0f;
        } else if (checkCurrentFloorCodeSevere(31)) {
            targetWalkSpeed = 0.0f;
        } else {
            targetWalkSpeed *= 1.0f - static_cast< f32 >(mSinkTimer) / 256.0f;
        }
    }

    targetWalkSpeed *= 1.0f - _2D0;

    if (_434 != 0) {
        targetWalkSpeed *= mActor->getConst().getTable()->mItemDashRatio;
    }

    return targetWalkSpeed;
}

void Mario::decideSquatWalkAnimation() {
    const f32 walkWeights1[] = {1.0f, 0.0f, 0.0f, 0.0f};
    const f32 walkWeights2[] = {0.0f, 1.0f, 0.0f, 0.0f};
    _3F4 = 0.0f;

    if (!mMovementStates._A) {
        if (mWalkSpeed > 0.1f) {
            stopAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd", "\x8a\xee\x96\x7b");
            changeAnimationUpperWeak("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9");
        } else {
            changeAnimation("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9", "\x8a\xee\x96\x7b");
        }

        mMovementStates._26 = false;
        return;
    }

    if (isAnimationRun("\x95\xc7\x89\x9f\x82\xb5", 0)) {
        stopAnimation(nullptr);
    }

    if (!_20._A || !isAnimationRun("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b")) {
        stopAnimation("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1);
        changeAnimation(nullptr, "\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b");
        stopAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8f\x80\x94\xf5", 4);
        getAnimator()->setWalkWeight(&walkWeights1[0]);
        mMovementStates._26 = false;
        if (!mMovementStates.jumping && !isAnimationRun("\x83\x54\x83\x7d\x81\x5b\x83\x5c\x83\x8b\x83\x67")) {
            playSound("\x90\xba\x82\xb5\x82\xe1\x82\xaa\x82\xde");
        }
    }

    if (mWalkSpeed < mActor->getConst().getTable()->mSpeedSquatWalkLower) {
        if (isStickOn()) {
            mWalkSpeed = mActor->getConst().getTable()->mSquatWalkMinSpeed;
            getAnimator()->setWalkWeight(&walkWeights2[0]);
            mTargetWalkSpeedIndex = 1;
            mMovementStates._26 = true;
        } else {
            getAnimator()->setWalkWeight(&walkWeights1[0]);
            mTargetWalkSpeedIndex = 0;
            mMovementStates._26 = false;
        }
    } else if (!isStickOn()) {
        getAnimator()->setWalkWeight(&walkWeights1[0]);
        mTargetWalkSpeedIndex = 0;
    } else if (mMovementStates._26) {
        MarioConstTable* table = mActor->getConst().getTable();
        mWalkSpeed = (table->mSquatWalkMaxSpeed - table->mSquatWalkMinSpeed) * mStickPos.z + table->mSquatWalkMinSpeed;
        getAnimator()->setWalkWeight(&walkWeights2[0]);
        mTargetWalkSpeedIndex = 1;
    }

    if (isAnimationRun("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b")) {
        f32 animspeed = mActor->getConst().getTable()->mSquatWalkStep;
        animspeed = 60.0f / animspeed * mWalkSpeed;
        if (!mMovementStates._26) {
            animspeed = 1.0f;
        }

        getAnimator()->getXanimePlayer()->changeSpeed(animspeed);
    }

    if (getFloorCode() == 32 && mTargetWalkSpeedIndex != 0) {
        _3F4 = 0.2f;
    }

    f32 animFrame = getAnimator()->getFrame();

    if (mTargetWalkSpeedIndex != 0) {
        if (mDrawStates.mIsUnderwater || mDrawStates._13) {
            if (mPrevAnimFrame > animFrame) {
                playSound("\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab\x8f\xac");
                playEffect("\x90\x85\x82\xcd\x82\xcb\x8d\xb6\x8e\xe3");
                playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mGroundPos - mSideVec * 20.0f) + _368 * _738);
            }

            if (mPrevAnimFrame < 30.0f && animFrame >= 30.0f) {
                playSound("\x90\x85\x92\xb5\x82\xcb\x89\x45\x91\xab\x8f\xac");
                playEffect("\x90\x85\x82\xcd\x82\xcb\x89\x45\x8e\xe3");
                playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mGroundPos + mSideVec * 20.0f) + _368 * _738);
            }
        }
    } else if (mDrawStates.mIsUnderwater && mActor->_37C % 64 == 0) {
        playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mShadowPos + mSideVec * (MR::getRandom() - 0.5f) * 20.0f) + _368 * _738);
    }

    mPrevAnimFrame = animFrame;
}

void Mario::decideWalkSpeed() {
    u32 i;
    bool canIndexDecrease = false;
    if (mTargetWalkSpeedIndex != 0) {
        f32 threshold = ::sSpeedTableB[mTargetWalkSpeedIndex - 1];
        if (mStickPos.z < threshold) {
            canIndexDecrease = true;
        }
    }

    for (i = 0; i < ARRAY_SIZE(::sSpeedTableA); i++) {
        if (mStickPos.z < ::sSpeedTableA[i]) {
            break;
        }
    }

    if (mTargetWalkSpeedIndex <= i || canIndexDecrease) {
        mTargetWalkSpeedIndex = i;
    }

    s32 clingNum = MR::getKarikariClingNum();
    if (clingNum != 0) {
        if (clingNum > 5) {
            clingNum = 5;
        }

        if (mTargetWalkSpeedIndex > 5 - clingNum) {
            mTargetWalkSpeedIndex = 5 - clingNum;
        }
    }

    if (mDrawStates.mIsUnderwater && mTargetWalkSpeedIndex > 6) {
        mTargetWalkSpeedIndex = 6;
    }

    if (mActor->mBeeWallWalk != 0 && mTargetWalkSpeedIndex > 4) {
        mTargetWalkSpeedIndex = 4;
    }

    if (_960 == 32) {
        if (mTargetWalkSpeedIndex > 3) {
            mTargetWalkSpeedIndex = 3;
        }

        if (mTargetWalkSpeedIndex != 0) {
            startPadVib(1);
        }

        if (mTargetWalkSpeedIndex > 2) {
            getAnimator()->getXanimePlayer()->_0C = 0.5f;
        } else {
            getAnimator()->getXanimePlayer()->_0C = 1.0f;
        }
    } else {
        f32 speed = getAnimator()->getXanimePlayer()->_0C;
        f32 new0C = speed;
        new0C += 0.1f;

        if (new0C > 1.0f) {
            new0C = 1.0f;
        }

        getAnimator()->getXanimePlayer()->_0C = new0C;
    }
}

void Mario::decideWalkAnimation() {
    if (mTargetWalkSpeedIndex == 0 && mWalkSpeed < 0.2f && isBlendWaitGround()) {
        getAnimator()->controlWaitAnimation();
    } else {
        getPlayer()->_10._F = false;

        if (getPlayer()->mTargetWalkSpeedIndex == 0 && mSwim->_1B2) {
            bool prepare = !isPlayerModeBee();
            if (prepare) {
                changeAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8f\x80\x94\xf5", 4);
                return;
            }
        }

        if (mSinkTimer == 0) {
            if (getPlayer()->mTargetWalkSpeedIndex != 0) {
                getAnimator()->stopWaitAnimation();
            }

            getAnimator()->setWalkWeight(::sWeightTable[mTargetWalkSpeedIndex]);
        } else {
            f32 weights[] = {0, 0, 0, 0};
            if (mTargetWalkSpeedIndex != 0) {
                f32 f1 = 1.0f;
                if (mTargetWalkSpeedIndex <= 3) {
                    if (mTargetWalkSpeedIndex == 2) {
                        f1 = 0.75f;
                    }

                    if (mTargetWalkSpeedIndex == 3) {
                        f1 = 0.5f;
                    }

                    weights[0] = f1;
                    weights[1] = 1.0f - f1;
                } else {
                    if (mTargetWalkSpeedIndex >= 5) {
                        f1 = static_cast< f32 >(mSinkTimer) / 100.0f;
                    }

                    f1 = MR::clamp(f1, 0.0f, 1.0f);
                    weights[1] = f1;
                    weights[2] = 1.0f - f1;
                }
            } else {
                weights[3] = 1.0f;
            }

            getAnimator()->setWalkWeight(&weights[0]);
        }
    }

    stopAnimation("\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8f\x80\x94\xf5", 4);

    f32 animFrame = getAnimator()->getFrame();
    if (mTargetWalkSpeedIndex != 0) {
        if (mDrawStates.mIsUnderwater || mDrawStates._13) {
            if (mPrevAnimFrame > animFrame) {
                if (mTargetWalkSpeedIndex >= 2) {
                    if (mTargetWalkSpeedIndex < 6) {
                        playSound("\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab\x8f\xac");
                        playEffect("\x90\x85\x82\xcd\x82\xcb\x8d\xb6\x8e\xe3");
                    } else {
                        playSound("\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab");
                        playEffect("\x90\x85\x82\xcd\x82\xcb\x8d\xb6");
                    }
                }

                playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mGroundPos - mSideVec * 20.0f) + _368 * _738);
            }

            if (mPrevAnimFrame < 30.0f && animFrame >= 30.0f) {
                if (mTargetWalkSpeedIndex >= 2) {
                    if (mTargetWalkSpeedIndex < 6) {
                        playSound("\x90\x85\x92\xb5\x82\xcb\x89\x45\x91\xab\x8f\xac");
                        playEffect("\x90\x85\x82\xcd\x82\xcb\x89\x45\x8e\xe3");
                    } else {
                        playSound("\x90\x85\x92\xb5\x82\xcb\x89\x45\x91\xab");
                        playEffect("\x90\x85\x82\xcd\x82\xcb\x89\x45");
                    }
                }

                playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mGroundPos + mSideVec * 20.0f) + _368 * _738);
            }
        }
    } else if (mDrawStates.mIsUnderwater && mActor->_37C % 64 == 0) {
        playEffectSRT("\x90\x85\x94\x67\x96\xe4", 0.2f, _73C, (mShadowPos + mSideVec * (MR::getRandom() - 0.5f) * 20.0f) + _368 * _738);
    }

    mPrevAnimFrame = animFrame;

    f32* footStep = ::sFootStep;
    f32 f3 = 0.0f;

    if (mActor->mBeeWallWalk != 0) {
        footStep = ::sFootStepBeeWallWalk;
    }

    for (int i = 0; i < ARRAY_SIZE(*::sWeightTable); i++) {
        f3 += footStep[i] * ::sWeightTable[mTargetWalkSpeedIndex][i];
    }

    f32 animationSpeed;

    if (f3 == 0.0f) {
        animationSpeed = 0.33f;
    } else {
        animationSpeed = 0.5f * (60.0f * ((0.01f * (mWalkSpeed * mActor->getConst().getTable()->mWalkSpeed)) / f3));
    }

    if (_8F0 > 0.0f && !mDrawStates._4) {
        animationSpeed *= 1.0f + ((_8F0 / 10.0f) * mActor->getConst().getTable()->mSlopeAnimeRatio);

        if (_8F0 > 5.0f && animationSpeed > 4.0f) {
            animationSpeed *= mActor->getConst().getTable()->mSlopeSpinAnimeRatio;
            changeAnimation("\x82\xaa\x82\xf1\x82\xce\x82\xe8\x91\x96\x82\xe8");
            startBas("RunSlope", false, 0.0f, 0.0f);
        }
    } else if (_3FE != 0) {
        animationSpeed *= _8F4;
    }

    f32 diffFromTargetSpeed = getTargetWalkSpeed() - mWalkSpeed;

    if (mWalkSpeed > ::sWalkTargetTable[5]) {
        f32 squared = diffFromTargetSpeed * diffFromTargetSpeed;
        f32 factor = (mWalkSpeed - ::sWalkTargetTable[5]) / (1.0f - ::sWalkTargetTable[5]);
        diffFromTargetSpeed = (diffFromTargetSpeed * (1.0f - factor)) + (squared * factor);
    } else if (mWalkSpeed < ::sWalkTargetTable[3]) {
        f32 sqrt = MR::fastSqrtf(diffFromTargetSpeed);
        f32 factor = (::sWalkTargetTable[3] - mWalkSpeed) / ::sWalkTargetTable[3];
        diffFromTargetSpeed = (diffFromTargetSpeed * factor) + (sqrt * (1.0f - factor));
    }

    f32 f4 = mActor->getConst().getTable()->mStartSpinAnimeRatio;
    f32 f5 = 1.0f + 4.0f * diffFromTargetSpeed;

    if (f5 > 2.0f) {
        f5 = 2.0f;
    }

    if (f5 < 1.0f) {
        f5 = 1.0f;
    }

    if (getTargetWalkSpeed() < ::sWalkTargetTable[6]) {
        f5 = 1.0f;
    }

    if (getFloorCode() == 32 && mTargetWalkSpeedIndex != 0) {
        f5 = 1.2f;
    }

    f32 f6 = f4 * (0.5f * (60.0f * ((getTargetWalkSpeed() * mActor->getConst().getTable()->mWalkSpeed * 0.01f) / footStep[2])));
    f32 factor2 = 2.0f - f5;

    if (animationSpeed < f6) {
        animationSpeed = animationSpeed * factor2 + f6 * (1.0f - factor2);
    }

    if (mSinkTimer != 0 && mTargetWalkSpeedIndex != 0 && animationSpeed < 1.0f) {
        animationSpeed = 1.0f;
    }

    animationSpeed *= (1.0f - _2D0);
    _3F4 = f5 - 1.0f;
    getAnimator()->getXanimePlayer()->changeSpeed(animationSpeed);

    if (!mActor->_EA4 && mTargetWalkSpeedIndex == 0 && mActor->mHealth == 1 && mActor->mMaxHealth > 2 &&
        (_970 == nullptr || strcmp(_970, "DamageWait"))) {
        getAnimator()->getXanimePlayer()->changeTrackAnimation(3, "\x83\x5f\x83\x81\x81\x5b\x83\x57\x83\x45\x83\x47\x83\x43\x83\x67");
        startBas("DamageWait", false, 0.0f, 0.0f);
        mActor->setBlink("DamageWait");
    }

    checkWallPush();

    f32 f7 = 0.9f;

    if (gIsLuigi) {
        f7 = 0.3f;
    }

    if (mTargetWalkSpeedIndex > 5 && mWalkSpeed > f7 && !mDrawStates._4 && !mMovementStates._35) {
        _71E = mActor->getConst().getTable()->mBrakeFirstTimer;
    }

    if (_71E != 0) {
        _71E--;
    }

    if (mTargetWalkSpeedIndex == 0) {
        if (_71E != 0 && !isSlipPolygon(mGroundPolygon) && !mDrawStates._5) {
            doBrakingAnimation();
            _71F = mActor->getConst().getTable()->mBrakeSecondTimer;
        }

        _71E = 0;

        s32 clingNum = MR::getKarikariClingNum();
        if (clingNum >= 1) {
            changeAnimationUpper("\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45");
            stopAnimation("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1);
        }

        if (clingNum < 1 && isAnimationRun("\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45")) {
            stopAnimationUpper("\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45");
        }
    }
}

void Mario::doBrakingAnimation() {
    changeAnimation("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1);
    getAnimator()->getXanimePlayer()->_20->mAttribute = 1;
    if (gIsLuigi) {
        getAnimator()->getXanimePlayer()->changeSpeed(0.5f);
    }

    playEffect("\x8b\xa4\x92\xca\x83\x75\x83\x8c\x81\x5b\x83\x4c");
    _71F = 0;
}

void Mario::checkWallPush() {
    if (mTargetWalkSpeedIndex != 0 && (mMovementStates._8 || mMovementStates._32) && checkWallJumpCode()) {
        return;
    }

    f32 angle = MR::diffAngleAbsHorizontal(mFrontVec, -getWallNorm(), *getGravityVec());
    bool sideStep = false;
    f32 wallPushAngleRange = getActor()->getConst().getTable()->mWallPushAngleRange;

    bool checkAngle = mTargetWalkSpeedIndex != 0 && mMovementStates._8;

    if (checkAngle && angle < MR::toRadian(wallPushAngleRange)) {
        sideStep = true;
    }

    if (mDrawStates._A) {
        sideStep = false;
    }

    if (mDrawStates._C) {
        sideStep = false;
    }

    if (calcAngleD(getWallNorm()) < getActor()->getConst().getTable()->mForceWallAngle) {
        sideStep = false;
        if (mMovementStates._8 && mTargetWalkSpeedIndex != 0) {
            mTargetWalkSpeedIndex = 1;
            mWalkSpeed = 0.0f;
        }
    }

    if (!isAnimationRun("\x95\xc7\x89\x9f\x82\xb5", 0) && sideStep) {
        doSideStep();
    }
}

void Mario::updateBrakeAnimation() {
    if (_71F != 0) {
        if (!isAnimationRun("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1)) {
            _71F = 0;
        } else {
            _71F--;
            if (!MR::isNearZero(mStickPos.z)) {
                _71F = 0;
            }

            if (_71F == 0) {
                stopAnimation(nullptr);
                stopWalk();
            }
        }
    } else if (isAnimationRun("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1) && (isAnimationTerminate(nullptr) || mTargetWalkSpeedIndex != 0)) {
        stopAnimation(nullptr);
    }

    if (!gIsLuigi) {
        return;
    }

    if (!isAnimationRun("\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c", 1) && !isAnimationRun("\x83\x75\x83\x8c\x81\x5b\x83\x4c")) {
        return;
    }

    if (mMovementStates._8 || mMovementStates._32) {
        stopAnimation(nullptr);
        _71F = 0;
        _71E = 0;
        _3D0 = 0;
        _3D2 = 0;
    } else if (!MR::isDemoActive() && mMovementStates._1) {
        playSound("\x83\x8b\x83\x43\x81\x5b\x83\x57\x8a\x8a\x82\xe8");
    }
}

#pragma push
#pragma opt_common_subs off
void Mario::updateWalkSpeed() {
    f32 targetWalkSpeed = getTargetWalkSpeed();
    f32 f2 = 1.0f;

    if (targetWalkSpeed == 0.0f) {
        _404 = getActor()->getConst().getTable()->mSlowStartTime;
    }

    if (_404 != 0) {
        f2 = getActor()->getConst().getTable()->mSlowStartTime - _404;
        f2 /= getActor()->getConst().getTable()->mSlowStartTime;
        _404--;
    }

    targetWalkSpeed *= f2 * f2;
    if (mMovementStates._F || isStatusActive(17)) {
        targetWalkSpeed *= getActor()->getConst().getTable()->mTornadoMultiply;
    }

    bool press = mMovementStates._A;
    if (mMovementStates._A && _1C._F) {
        press = false;
        if (_95C->getCode(_4C8) == 29) {
            press = true;
        } else if (MR::isSensorPressObj(_730)) {
            TVec3f result;
            if (MR::vecKillElement(_184, *getGravityVec(), &result) < -0.5f) {
                press = true;
            } else {
                TVec3f collisionTrans;
                TVec3f collisionPrevTrans;
                MR::extractMtxTrans(_730->mHost->mCollisionParts->mBaseMatrix, &collisionTrans);
                MR::extractMtxTrans(_730->mHost->mCollisionParts->mPrevBaseMatrix, &collisionPrevTrans);
                if (MR::vecKillElement(collisionTrans - collisionPrevTrans, *getGravityVec(), &result) < -0.5f) {
                    press = true;
                }
            }
        }

        if (mMovementStates._1 && (strstr(getGroundPolygon()->mSensor->mHost->mName, "TriPod") != nullptr ||
                                   strstr(getGroundPolygon()->mSensor->mHost->mName, "Tripod") != nullptr)) {
            press = false;
        }

        if (mMovementStates._1 && _730 == getGroundPolygon()->getSensor()) {
            press = false;
        }

        if (_730 != nullptr && press) {
            mActor->_3B4 = _368;
            mActor->setPress(0, 0);
        }
    } else {
        mMovementStates._A = false;

        if (_436 == 0 && _434 == 0 && checkSquat(false) && mSinkTimer <= 32 && !isStatusActive(31)) {
            if (!checkLockOnHoming()) {
                mMovementStates._A = true;
            }

            if (!press && mMovementStates._A && (mMovementStates._8 || mMovementStates._32)) {
                mTargetWalkSpeedIndex = 0;
                mWalkSpeed = 0.0f;
            }
        }

        if (_1C._F && !mMovementStates._A && isAnimationRun("\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9")) {
            mMovementStates._A = true;
        }

        if (!mMovementStates._A && press) {
            mMovementStates._A = true;
            cancelSquatMode();
            _71E = 0;
        }
    }

    if (_3D0 != 0) {
        targetWalkSpeed = 0.0f;
    }

    if (mMovementStates._10) {
        targetWalkSpeed = 0.0f;
    }

    f32 inertia = decideInertia(targetWalkSpeed);

    if (!mMovementStates._A && getPlayerMode() == 1) {
        if (mWalkSpeed >= 0.9999f) {
            targetWalkSpeed *= getActor()->getConst().getTable()->mDashMultiply;
            if (targetWalkSpeed > mWalkSpeed) {
                inertia = 0.99f;
            }

            if (getPlayer()->mWalkSpeed >= 1.5f) {
                getAnimator()->getXanimePlayer()->changeTrackAnimation(2, "\x83\x81\x83\x5e\x83\x8b\x83\x5f\x83\x62\x83\x56\x83\x85");
            }
        } else {
            getAnimator()->stopWaitAnimation();
        }
    }

    targetWalkSpeed = targetWalkSpeed * static_cast< f32 >(256 - mSinkTimer) / 256.0f;
    mWalkSpeed = mWalkSpeed * inertia + targetWalkSpeed * (1.0f - inertia);
}
#pragma pop

void Mario::decideOnIceAnimation() {
    if (mTargetWalkSpeedIndex == 0) {
        if (mWalkSpeed > 0.2f && !isAnimationRun("\x95\x58\x8f\xe3\x8a\xb5\x90\xab\x91\x96\x8d\x73")) {
            changeAnimationWithAttr("\x95\x58\x8f\xe3\x8a\xb5\x90\xab\x91\x96\x8d\x73", 1);
            mIceAnimFoot = 1 - mIceAnimFoot;
        }
    } else {
        decideWalkAnimation();
        if (mWalkSpeed > 0.7f) {
            switch (mIceAnimFoot) {
            case 0:
                getAnimator()->getXanimePlayer()->changeTrackAnimation(2, "\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x8d\xb6");
                break;
            default:
                getAnimator()->getXanimePlayer()->changeTrackAnimation(2, "\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x89\x45");
            }
        }
    }

    if (mTargetWalkSpeedIndex != 0 || mWalkSpeed <= 0.2f) {
        stopAnimation("\x95\x58\x8f\xe3\x8a\xb5\x90\xab\x91\x96\x8d\x73");
    }
}

void Mario::updateOnSand() {
    if (mMovementStates._1F) {
        return;
    }

    if (mMovementStates._1) {
        if (_960 == 27 || _960 == 28) {
            if (!strcmp(MR::getSoundCodeString(_45C), "Sand") && mSinkTimer < 64) {
                mSinkTimer++;
            }
        } else if (isCurrentFloorSink()) {
            if (mSinkTimer < 255) {
                mSinkTimer++;
                if (_960 == 25 || _960 == 31) {
                    if (_960 == 31) {
                        if (mSinkTimer == 1) {
                            playSound("\x90\xba\x8f\xc0\x92\xbe\x82\xdd");
                        }

                        playSound("\x8f\xc0\x8b\xad\x90\xa7\x92\xbe\x82\xdd");
                    } else {
                        if (mSinkTimer == 1) {
                            playSound("\x90\xba\x8d\xbb\x92\xbe\x82\xdd");
                        }

                        playSound("\x8d\xbb\x8b\xad\x90\xa7\x92\xbe\x82\xdd");
                    }

                    stopWalk();
                    mSinkTimer = MR::clamp(static_cast< s32 >(mSinkTimer) + 3, 0, 255);

                    if (getAirGravityVec().dot(_368) > -0.99f) {
                        TVec3f vec1;
                        MR::vecKillElement(_368, getAirGravityVec(), &vec1);
                        TVec3f vec2;
                        vec2.cross(vec1, _368);
                        MR::normalize(&vec2);
                        vec1.cross(_368, vec2);
                        addVelocity(vec1 * 6.0f);
                    }
                } else {
                    playSound("\x8d\xbb\x92\xbe\x82\xdd");
                }
            } else {
                mActor->forceGameOverSink();
                return;
            }

            if (!isAnimationRun(nullptr)) {
                getAnimator()->getXanimePlayer()->changeTrackAnimation(1, "\x96\x84\x82\xdc\x82\xe8\x95\xe0\x8d\x73");
            }
        } else {
            if (mSinkTimer != 0 && !isAnimationRun(nullptr)) {
                getAnimator()->getXanimePlayer()->changeTrackAnimation(1, "\x95\xe0\x8d\x73");
            }

            mSinkTimer = 0;
        }
    }

    if (mMovementStates.jumping) {
        mSinkTimer = 0;
    }

    if (isStatusActive(6)) {
        mSinkTimer = 0;
    }
}

void Mario::updateOnPoison() {
    if (mMovementStates._1) {
        if (checkCurrentFloorCodeSevere(18)) {
            if (mPoisonTimer == 0) {
                mActor->decLife(0);
                playSound("\x93\xc5\x8f\xc0\x83\x5f\x83\x81\x81\x5b\x83\x57");
                playSound("\x83\x5f\x83\x81\x81\x5b\x83\x57");
                playSound("\x90\xba\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57");
                if (mActor->mHealth == 0) {
                    mActor->forceGameOver();
                }

                startCamVib(0);
                mActor->_BC4 = 1;
            }

            if (mPoisonTimer < 255) {
                mPoisonTimer++;
            } else {
                mPoisonTimer = 0;
            }
        } else {
            mPoisonTimer = 0;
        }
    } else if (mMovementStates.jumping && _3BC > 10) {
        mPoisonTimer = 0;
    }

    if (isStatusActive(6)) {
        mPoisonTimer = 0;
    }
}

void Mario::updateOnWater() {
    if (mMovementStates._1) {
        switch (_960) {
        case 20:
        case 21:
        case 22:
            touchWater();
            _738 = 20.0f;
            _73C = _368;

            switch (_962) {
            case 20:
                _738 += 20.0f;
            case 21:
                _738 += 20.0f;
            case 22:
                mDrawStates.mIsUnderwater = true;
            }
        }

        if (_960 == 23 && _962 == 23) {
            touchWater();
            mDrawStates._13 = true;
            _738 = 3.0f;
            _73C = _368;
        }
    }

    switch (_962) {
    case 20:
    case 21:
    case 22:
    case 23:
        mDrawStates._1D = true;
    }
}
