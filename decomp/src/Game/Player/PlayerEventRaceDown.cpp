#include "Game/Player/PlayerEventRaceDown.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

EventRaceDown::EventRaceDown() : EventSequence(16) {
    addEventOnTime("\x8f\x89\x8a\xfa\x89\xbb", static_cast< EventFunc1 >(&EventRaceDown::init), 0);
    addEventOnTime("MISS\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8a\x4a\x8e\x6e", static_cast< EventFunc1 >(&EventRaceDown::missLayoutOpen), 45);
    addEventOnTime("\x92\xca\x8f\xed\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8f\xc1\x8b\x8e", (&EventRaceDown::closeDefaultLayout), 30);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""A", static_cast< EventFunc1 >(&EventRaceDown::sound), 30);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""B", static_cast< EventFunc1 >(&EventRaceDown::sound), 100);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""C", static_cast< EventFunc1 >(&EventRaceDown::sound), 120);
    addEventInStatus("\x83\x8f\x83\x43\x83\x76\x8a\x4a\x8e\x6e", static_cast< EventFunc1 >(&EventRaceDown::doCloseWipe),
                     static_cast< EventFunc2 >(&EventRaceDown::checkCloseWipeStart));
    addEventOnTime("\x8e\x63\x8b\x40\x82\xf0\x88\xf8\x82\xad", static_cast< EventFunc1 >(&EventRaceDown::decLeft), 120);
    addEventInPhase("\x83\x8f\x83\x43\x83\x76\x8f\x49\x97\xb9\x8c\xe3", static_cast< EventFunc1 >(&EventRaceDown::doWaitAfterWipe), 2);
}

void EventRaceDown::init(u16, u16) {
    MR::setCubeBgmChangeInvalid();
    MR::clearBgmQueue();
    MR::stopStageBGM(10);
    MR::stopSubBGM(10);
    playAnimation("\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf");
    playSound("\x90\xba\x8d\xc5\x8f\x49\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x8d\xc5\x8c\xe3\x82\xcc\x88\xea\x8c\x82");
    MR::startPlayerDownWipe();
}

void EventRaceDown::sound(u16 eventFrame, u16 sequenceFrame) {
    switch (sequenceFrame) {
    case 30:
        MR::setSoundVolumeSetting(2, 20);
        MR::startSubBGM("BGM_MISS", false);
        break;
    case 100:
    case 120:
        break;
    }
}

bool EventRaceDown::checkCloseWipeStart(u16 sequenceFrame) {
    if (isMissLayoutClosed(sequenceFrame) && getPhase() == 1) {
        return true;
    } else {
        return false;
    }
}

void EventRaceDown::missLayoutOpen(u16 eventFrame, u16 sequenceFrame) {
    MR::startMissLayout();
    nextPhase();
}
