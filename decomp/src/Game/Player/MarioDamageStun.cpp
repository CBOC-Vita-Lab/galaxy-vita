#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioStun.hpp"

MarioStun::MarioStun(MarioActor* pActor) : MarioState(pActor, MarioStatus_Stun), _12(0), _14(0) {
}

bool MarioStun::close() {
    stopAnimation("\x82\xb5\x82\xd1\x82\xea");  // "hesitation"
    return true;
}

bool MarioStun::start() {
    changeAnimationNonStop("\x82\xb5\x82\xd1\x82\xea");
    startPadVib("\x83\x7d\x83\x8a\x83\x49[\x82\xb5\x82\xd1\x82\xea]");
    playSound("\x90\xba\x82\xb5\x82\xd1\x82\xea");
    _14 = 0x3c;
    _12 = 0;
    return true;
}

bool MarioStun::update() {
    if (_14 != 0) {
        _14--;
    }

    if (_14 == 0) {
        if (_12 != 0) {
            return false;
        }

        _12 = 1;
        if (!getPlayer()->mMovementStates._1) {
            _14 = 0xa;
        } else {
            _14 = 0x1e;
        }

        if (getPlayer()->mMovementStates._1) {
            changeAnimation("\x82\xb5\x82\xd1\x82\xea\x89\xf1\x95\x9c");
        }
    }

    if (_12 != 0 && (mActor->isRequestRush() || checkTrgA())) {
        stopAnimation(nullptr);
        if (checkTrgA()) {
            getPlayer()->tryJump();
        }

        return false;
    }

    return true;
}
