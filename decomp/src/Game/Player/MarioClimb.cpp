#include "Game/Player/MarioClimb.hpp"
#include "Game/Player/MarioActor.hpp"

void Mario::connectToClimb() {
    _790 = -getShadowNorm();
    changeStatus(mClimb);
}

MarioClimb::MarioClimb(MarioActor* pActor) : MarioState(pActor, MarioStatus_Climb), mTimer() {
}

bool MarioClimb::update() {
    if (mTimer < 15 && checkTrgA()) {
        getPlayer()->tryJump();
        return false;
    }

    if (mTimer != 0) {
        mTimer--;
    }

    addVelocity(getFrontVec(), 6.0f);

    if (mTimer == 0) {
        return false;
    }

    return true;
}

bool MarioClimb::start() {
    changeAnimation("\x99\xb3\x99\xb4\x91\x4f\x90\x69", "\x99\xb3\x99\xb4\x91\x4f\x90\x69");

    if (mActor->_468 != 0) {
        changeAnimationUpper("\x82\xd0\x82\xeb\x82\xa2\x83\x45\x83\x47\x83\x43\x83\x67");
    }

    mTimer = 15;
    return true;
}

bool MarioClimb::close() {
    getPlayer()->mWalkSpeed = getStickP();

    if (getPlayer()->getMovementStates()._1) {
        stopAnimation("\x99\xb3\x99\xb4\x91\x4f\x90\x69", "\x8a\xee\x96\x7b");
    } else {
        stopAnimation("\x99\xb3\x99\xb4\x91\x4f\x90\x69", "\x97\x8e\x89\xba");
        getPlayer()->set3BC(8);
    }

    return true;
}
