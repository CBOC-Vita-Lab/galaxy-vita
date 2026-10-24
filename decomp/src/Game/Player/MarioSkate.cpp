#include "Game/Player/MarioSkate.hpp"
#include "Game/Map/CollisionCode.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"

void MarioSkate_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)-1.0f;
    (void)3.1415927f;
    (void)20.0f;
    (void)50.0f;
    (void)1.25f;
    (void)1.5f;
    (void)1.2f;
    (void)0.04f;
    (void)0.1f;
    (void)-0.1f;
    (void)-0.3926991f;
    (void)0.3926991f;
    (void)-0.5235988f;
    (void)0.5235988f;
    (void)0.8f;
    (void)0.9f;
    (void)0.95f;
    (void)0.05f;
}

bool Mario::isSkatableFloor() const {
    if (_960 == CollisionFloorCode_Ice) {
        return true;
    }

    return _960 == CollisionFloorCode_GlassIce;
}

bool Mario::doSkate() {
    changeStatus(mSkate);
    return true;
}

MarioSkate::MarioSkate(MarioActor* pActor) : MarioState(pActor, MarioStatus_Skate) {
    _14 = 0;
    _20 = 0.0f;
    _18 = 0;
    _19 = 0;
    _1A = 0;
    _1B = 0;
    _1C = 0;
    _1D = 0;
    _24 = 0.0f;
}

bool MarioSkate::postureCtrl(MtxPtr pMtx) {
    getPlayer()->postureCtrl(pMtx);
    f32 rotation = _20;
    rotation *= 3.1415927f;
    PSMTXConcat(pMtx, MR::tmpMtxRotYRad(rotation), pMtx);
    PSMTXConcat(pMtx, MR::tmpMtxRotZRad(_24), pMtx);
    Mtx direction;
    getPlayer()->createDirectionMtx(direction);
    PSMTXConcat(direction, pMtx, pMtx);
    return true;
}

void MarioSkate::exitJump() {
    _18 = 1;
    getPlayer()->tryJump();
    playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76");
}

bool MarioSkate::start() {
    _14 = 0;
    _1B = 0;
    _1C = 0;
    _18 = 0;
    _1D = 1;
    _19 = 0;
    _24 = 0.0f;
    _1A = 0;

    if (isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76") || isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2") || isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3")) {
        _1D = 1;
        changeAnimation(nullptr, "\x8a\xee\x96\x7b");
        changeAnimationNonStop("\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e");
        playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x89\x45");
        playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x8d\xb6");
        playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e");
        _19 = 0;
        _20 = 0.0f;
    } else {
        _20 = 0.0f;
        if (getPlayer()->mTargetWalkSpeedIndex < 3) {
            _1A = 1;
            changeAnimationNonStop("\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x90\xc3\x8e\x7e");
        } else {
            changeAnimationNonStop("\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8");
            playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x8d\xb6");
        }

        playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x58\x83\x73\x83\x93");
        playSound("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
        playSound("\x90\xba\x83\x58\x83\x73\x83\x93");
    }

    return true;
}

bool MarioSkate::update() {
    f32 speed;
    if (!getPlayer()->mMovementStates._1 || getPlayer()->mMovementStates.jumping) {
        return false;
    }

    if (checkTrgA() || getPlayer()->mMovementStates._38) {
        exitJump();
        return false;
    }

    if (!getPlayer()->isSkatableFloor()) {
        return false;
    }

    if (!_1A) {
        getPlayer()->mainMove();
    } else {
        return !isAnimationTerminate(nullptr);
    }

    if (getPlayer()->mMovementStates._10) {
        getPlayer()->mMovementStates._10 = false;
        getPlayer()->_3D2 = 0;
    }

    if (checkTrgZ()) {
        bool reverse = !_19;
        _14 = 20;
        _19 = reverse;
        _1C = 1;
        _1B = 1 - _1B;
        playSound("\x90\xba\x95\xc7\x89\x9f\x82\xb5");
    }

    getPlayer()->updateWalkSpeed();
    if (isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e")) {
        if (isAnimationTerminate(nullptr)) {
            _1C = 1;
        }
    } else if (_1D == 1) {
        f32 stick = getStickP();
        f32 frame = 20.0f + 50.0f * (1.0f - stick);
        if (getAnimator()->getFrame() > frame && getStickP()) {
            _1C = 1;
        }
    }

    if (mActor->isRequestSpin()) {
        if (_1D && (!isAnimationRun("\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x88\xda\x93\xae") || isAnimationTerminate(nullptr))) {
            stopAnimation(nullptr);
            changeAnimationNonStop("\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x88\xda\x93\xae");
            playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x58\x83\x73\x83\x93");
            playSound("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
            playSound("\x90\xba\x83\x70\x83\x93\x83\x60");
            if (getPlayer()->mWalkSpeed < 1.25f) {
                getPlayer()->mWalkSpeed = 1.5f * getPlayer()->mWalkSpeed;
            }
        }

        if (getAnimator()->getFrame() > 20.0f) {
            _1C = 1;
        }
    }

    speed = getPlayer()->mWalkSpeed;
    if (speed < 1.2f * getStickP()) {
        getPlayer()->mWalkSpeed = 1.2f * getStickP();
    }

    if (speed > 0.0f && !isAnimationRun("\x8a\xee\x96\x7b")) {
        playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x8a\x8a\x82\xe8");
    }

    if (_19) {
        _20 += 0.04f;
        _20 = MR::clamp(_20, -1.0f, 1.0f);
    } else {
        if (_20 == 1.0f) {
            _20 = -1.0f;
        }

        bool negative = false;
        if (_20 <= 0.0f) {
            negative = true;
        }

        _20 += 0.04f;
        if (negative) {
            _20 = -MR::clamp(-_20, 0.0f, 1.0f);
        } else {
            _20 = MR::clamp(_20, -1.0f, 1.0f);
        }
    }

    f32 angle = MR::diffAngleAbsHorizontal(getWorldPadDir(), getFrontVec(), getGravityVec());
    TVec3f cross;
    const TVec3f& rPad = getWorldPadDir();
    cross.cross(getFrontVec(), rPad);
    if (cross.dot(getGravityVec()) < 0.0f) {
        angle = -angle;
    }

    if (mActor->_3E5) {
        _14 = 15;
    } else {
        _14++;
    }

    u32 interval = 30;
    if (_19) {
        interval = 60;
    }

    if (_14 >= interval) {
        if (_1C) {
            if (!(angle >= 0.1f && _1B == 1) && !(angle <= -0.1f && _1B == 0)) {
                _1C = 0;
                _14 = 0;
                _1B = 1 - _1B;
                if (_1D < 1) {
                    _1D++;
                } else {
                    switch (_1B) {
                    case 0:
                        if (_19) {
                            changeAnimationNonStop("\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x89\x45");
                        } else {
                            changeAnimationNonStop("\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x8d\xb6");
                        }

                        playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x8d\xb6");
                        stopEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x89\x45");
                        playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x91\xab");
                        break;
                    case 1:
                        if (_19) {
                            changeAnimationNonStop("\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x8d\xb6");
                        } else {
                            changeAnimationNonStop("\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x89\x45");
                        }

                        playEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x89\x45");
                        stopEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x8d\xb6");
                        playSound("\x83\x58\x83\x50\x81\x5b\x83\x67\x91\xab");
                        break;
                    }
                }
            }
        } else if (_14 > 90) {
            return false;
        }
    }

    f32 animationSpeed = 1.0f;
    f32 animationScale = 1.0f - 0.5f * (1.0f - getStickP());
    if (!isAnimationRun("\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e")) {
        getAnimator()->setSpeed(animationSpeed * animationScale);
    }

    f32 tilt;
    if (_19) {
        tilt = MR::clamp(angle, -0.3926991f, 0.3926991f);
    } else {
        tilt = MR::clamp(angle, -0.5235988f, 0.5235988f);
    }

    f32 targetTilt = tilt * (1.0f - 0.8f * (1.0f - getStickP()));
    if (MR::abs(targetTilt) > MR::abs(_24)) {
        _24 = 0.9f * _24 + 0.1f * targetTilt;
    } else {
        _24 = 0.95f * _24 + 0.05f * targetTilt;
    }

    return true;
}

bool MarioSkate::close() {
    if (_18) {
        if (!_1A) {
            switch (getPlayer()->_430) {
            case 1:
                changeAnimationNonStop("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2");
                break;
            case 2:
                changeAnimationNonStop("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3");
                break;
            default:
                changeAnimationNonStop("\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76");
                break;
            }

            TVec3f velocity;
            Mario* pPlayer = getPlayer();
            f32 vertical = MR::vecKillElement(pPlayer->mJumpVec, getGravityVec(), &velocity);
            velocity *= 1.5f;
            velocity += getGravityVec() * vertical;
            getPlayer()->setJumpVec(velocity);
        }
    } else if (getPlayer()->mMovementStates._1) {
        stopAnimation(nullptr, "\x8a\xee\x96\x7b");
    } else {
        stopAnimation(nullptr, "\x97\x8e\x89\xba");
    }

    stopEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x8d\xb6");
    stopEffect("\x83\x58\x83\x50\x81\x5b\x83\x67\x89\x45");
    return true;
}
