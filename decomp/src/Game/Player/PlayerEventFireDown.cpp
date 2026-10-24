#include "Game/Player/PlayerEventFireDown.hpp"
#include "Game/Player/MarioAccess.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

EventFireDown::EventFireDown() : EventSequence(16) {
    addEventOnTime("\x8f\x89\x8a\xfa\x89\xbb", static_cast< EventFunc1 >(&EventFireDown::init), 0);
    addEventOnTime("\x92\xca\x8f\xed\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8f\xc1\x8b\x8e", static_cast< EventFunc1 >(&EventFireDown::closeDefaultLayout), 100);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""A", static_cast< EventFunc1 >(&EventFireDown::sound), 30);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""B", static_cast< EventFunc1 >(&EventFireDown::sound2), 50);
    addEventInStatus("\x83\x8f\x83\x43\x83\x76\x8a\x4a\x8e\x6e", static_cast< EventFunc1 >(&EventFireDown::doCloseWipe),
                     static_cast< EventFunc2 >(&EventSequence::isMissLayoutClosed));
    addEventOnTime("\x8e\x63\x8b\x40\x82\xf0\x88\xf8\x82\xad", static_cast< EventFunc1 >(&EventFireDown::decLeft), 120);
    addEventInPhase("\x83\x8f\x83\x43\x83\x76\x8f\x49\x97\xb9\x8c\xe3", static_cast< EventFunc1 >(&EventFireDown::doWaitAfterWipe), 2);
}

void EventFireDown::init(u16 eventFrame, u16 sequenceFrame) {
    MR::setCubeBgmChangeInvalid();
    MR::clearBgmQueue();
    MR::stopStageBGM(10);
    MR::stopSubBGM(10);
    MarioAccess::getPlayerActor()->changeGameOverAnimation();
    playSound("\x90\xba\x8d\xc5\x8f\x49\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x8d\xc5\x8c\xe3\x82\xcc\x88\xea\x8c\x82");
    _28 = 20.0f;
    MR::startPlayerDownWipe();
    MR::startMissLayout();
}

void EventFireDown::sound(u16 eventFrame, u16 sequenceFrame) {
    MR::setSoundVolumeSetting(2, 20);
    MR::startSubBGM("BGM_MISS", false);
}

void EventFireDown::updateAfter() {
    setSpot(_28, _24);
}

void EventFireDown::sound2(u16 eventFrame, u16 sequenceFrame) {
    MR::setSoundVolumeSetting(7, 70);
}
