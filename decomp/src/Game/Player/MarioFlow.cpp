#include "Game/Player/MarioFlow.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Util/MathUtil.hpp"

bool Mario::doFlow() {
    if (isStatusActive(MarioStatus_Flow)) {
        return false;
    }

    if (isAnimationRun("\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8")) {
        return false;
    }

    changeStatus(mFlow);

    return true;
}

bool MarioFlow::start() {
    _12 = 0;
    _14 = 0;

    changeAnimationNonStop("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57");
    startPadVib(2);

    getPlayer()->mMovementStates._1 = false;
    getPlayer()->mMovementStates.jumping = true;
    getPlayer()->mMovementStates._B = false;
    getPlayer()->mMovementStates._2B = true;

    _18 = getFrontVec() * mActor->mConst->getTable()->mJumpDistFlow;
    _18 += -mActor->_240 * mActor->mConst->getTable()->mJumpHeightFlow;
    getPlayer()->mJumpVec = _18;

    addVelocity(_18);

    getPlayer()->stopWalk();

    return true;
}

bool MarioFlow::update() {
    _12++;

    switch (_14) {
    case 0:
        addVelocity(_18);

        _18 += mActor->_240 * mActor->getConst().getTable()->mGravityFlow;

        if (_12 == 20) {
            changeAnimation("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86");
        }

        if (_12 > 30 && mActor->isRequestRush()) {
            getPlayer()->trySpinJump(0);

            return false;
        }

        if (getPlayer()->getMovementStates()._1) {
            getPlayer()->mMovementStates.jumping = false;

            changeAnimation("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e");

            playEffect("\x8b\xa4\x92\xca\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e");
            playSound("\x90\x81\x82\xc1\x94\xf2\x82\xd1\x93\x7c\x82\xea");

            MR::vecKillElement(_18, mActor->_240, &_18);

            _12 = 0;
            _14++;
        }
        break;
    case 1:
        if (getPlayer()->getMovementStates()._1 == false) {
            getPlayer()->mMovementStates.jumping = true;
            _14 = 0;
        } else {
            addVelocity(_18);

            _18.x *= 0.95f;
            _18.y *= 0.95f;
            _18.z *= 0.95f;

            if (!isAnimationRun("\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e")) {
                return false;
            }

            if (_12 > 15 && checkTrgA()) {
                getPlayer()->tryJump();

                return false;
            }
        }
        break;
    }

    getPlayer()->mJumpVec = _18;

    return true;
}

bool MarioFlow::close() {
    stopAnimation("\x83\x5f\x83\x81\x81\x5b\x83\x57");
    stopAnimation("\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e", "\x8a\xee\x96\x7b");

    return true;
}

MarioFlow::MarioFlow(MarioActor* pActor) : MarioState(pActor, MarioStatus_Flow), _12(), _14() {
    _18.zero();
}
