#include "Game/Player/MarioMagic.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioModule.hpp"

void Mario::stopPunch() {
    if (isStatusActive(MarioStatus_Magic)) {
        closeStatus(mMagic);
    }

    MarioActor* actor = mActor;

    if (!actor->_944) {
        actor->_945 = 0;
        actor->_974 = 0;
    }

    actor->_944 = 0;
}

void Mario::startMagic() {
    if (!mMovementStates.jumping) {
        if (!mActor->_468) {
            if (!mMovementStates._23) {
                if (!isStatusActive(MarioStatus_Slider)) {
                    if (isSkatableFloor()) {
                        doSkate();
                    } else {
                        clearSlope();
                        changeStatus(mMagic);
                        stopAnimationUpper(nullptr);
                        _10._1 = 1;
                    }
                }
            }
        }
    }
}

MarioMagic::MarioMagic(MarioActor* pActor) : MarioState(pActor, MarioStatus_Magic) {
    _12 = 0;
}

bool MarioMagic::start() {
    changeAnimation("\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8");
    stopEffect("\x83\x70\x83\x93\x83\x60\x83\x75\x83\x89\x81\x5b\x8d\xb6");
    stopEffect("\x83\x70\x83\x93\x83\x60\x83\x75\x83\x89\x81\x5b\x89\x45");
    playEffect("\x8b\xa4\x92\xca\x92\x6e\x8f\xe3\x83\x58\x83\x73\x83\x93");
    playSound("\x90\xba\x83\x58\x83\x73\x83\x93");
    playSound("\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76");
    startPadVib(2);
    _12 = 0;
    return true;
}

bool MarioMagic::close() {
    stopEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");

    if (_12 < 0x1A) {
        playEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67\x8f\xc1\x8b\x8e");
    }

    return true;
}

bool MarioMagic::update() {
    if (mActor->isRequestJump()) {
        getPlayer()->tryJump();
        return false;
    } else if (!isAnimationRun("\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8")) {
        return false;
    }

    _12++;

    if (_12 == 25) {
        stopEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
        playEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67\x8f\xc1\x8b\x8e");
    }

    if (getPlayer()->mMovementStates.jumping) {
        getPlayer()->procJump(false);
    } else {
        getPlayer()->mainMove();
    }

    getPlayer()->updateWalkSpeed();
    return true;
}
