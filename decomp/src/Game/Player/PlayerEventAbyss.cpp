#include "Game/Player/PlayerEventAbyss.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

EventAbyss::EventAbyss() : EventSequence(12) {
    addEventOnTime("\x8f\x89\x8a\xfa\x89\xbb", static_cast< EventFunc1 >(&EventAbyss::init), 0);
    addEventOnTime("\x92\xca\x8f\xed\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8f\xc1\x8b\x8e", static_cast< EventFunc1 >(&EventAbyss::closeDefaultLayout), 25);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""A", static_cast< EventFunc1 >(&EventAbyss::sound), 30);
    addEventInStatus("\x83\x8f\x83\x43\x83\x76\x8a\x4a\x8e\x6e", static_cast< EventFunc1 >(&EventAbyss::doCloseWipe), static_cast< EventFunc2 >(&EventAbyss::isMissLayoutClosed));
    addEventOnTime("\x8e\x63\x8b\x40\x82\xf0\x88\xf8\x82\xad", static_cast< EventFunc1 >(&EventAbyss::decLeft), 120);
    addEventInPhase("\x83\x8f\x83\x43\x83\x76\x8f\x49\x97\xb9\x8c\xe3", static_cast< EventFunc1 >(&EventAbyss::doWaitAfterWipe), 2);
}

void EventAbyss::init(u16 eventFrame, u16 sequenceFrame) {
    MR::requestStartGameOverDemo();
    playSound("\x90\xba\x97\x8e\x89\xba\x8e\x80\x96\x53");
    MR::setCubeBgmChangeInvalid();
    MR::clearBgmQueue();
    MR::stopStageBGM(10);
    MR::stopSubBGM(10);
    MR::startMissLayout();
}

void EventAbyss::sound(u16 eventFrame, u16 sequenceFrame) {
    MR::setSoundVolumeSetting(2, 20);
    MR::startSubBGM("BGM_MISS", false);
}

void EventAbyss::updateAfter() {
    setSpot(_28, _24);
}
