#include "Game/Player/PlayerEventGameOver.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

EventGameOver::EventGameOver() : EventSequence(16) {
    addEventOnTime("\x8f\x89\x8a\xfa\x89\xbb", static_cast< EventFunc1 >(&EventGameOver::init), 0);
    addEventOnTime("\x92\xca\x8f\xed\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8f\xc1\x8b\x8e", static_cast< EventFunc1 >(&EventGameOver::closeDefaultLayout), 60);
    addEventOnTime("\x83\x54\x83\x45\x83\x93\x83\x68""A", static_cast< EventFunc1 >(&EventGameOver::sound), 90);
    addEventOnTime("\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67\x8a\x4a\x8e\x6e", static_cast< EventFunc1 >(&EventGameOver::reqLayout), 160);
    addEventOnTime("\x8f\x49\x97\xb9", static_cast< EventFunc1 >(&EventGameOver::finish), 420);
}

void EventGameOver::init(u16 eventFrame, u16 sequenceFrame) {
    MR::requestStartGameOverDemo();
    MR::setCubeBgmChangeInvalid();
    MR::clearBgmQueue();
    MR::stopStageBGM(10);
    MR::stopSubBGM(10);
    playSound("\x90\xba\x8d\xc5\x8f\x49\x83\x5f\x83\x81\x81\x5b\x83\x57");
    playSound("\x8d\xc5\x8c\xe3\x82\xcc\x88\xea\x8c\x82");
    _28 = 20.0f;
}

void EventGameOver::sound(u16 eventFrame, u16 sequenceFrame) {
    switch (sequenceFrame) {
    case 90:
        MR::setSoundVolumeSetting(2, 30);
        MR::startSubBGM("BGM_GAMEOVER", false);
        break;
    }
}

void EventGameOver::updateAfter() {
}

void EventGameOver::reqLayout(u16 eventFrame, u16 sequenceFrame) {
    MR::startGameOverWipe();
}

void EventGameOver::finish(u16 eventFrame, u16 sequenceFrame) {
    MR::requestEndGameOverDemo();
    stopSequence();
}
