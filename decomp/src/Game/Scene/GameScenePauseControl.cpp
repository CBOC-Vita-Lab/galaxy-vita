#include "Game/Scene/GameScenePauseControl.hpp"
#include "Game/AudioLib/AudSystem.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/GameScene.hpp"
#include "Game/Screen/GamePauseSequence.hpp"
#include "Game/System/PauseButtonCheckerInGame.hpp"

namespace {
    NEW_NERVE(GameScenePauseControlNormal, GameScenePauseControl, Normal);
};  // namespace

GameScenePauseControl::GameScenePauseControl(GameScene* pScene) : NerveExecutor("GameScene\x83\x7c\x81\x5b\x83\x59\x90\xa7\x8c\xe4") {
    mScene = pScene;
    mPauseChecker = nullptr;
    mPauseMenuOff = false;
    mPauseMenuNerve = nullptr;
    initNerve(GET_NERVE_ANON(GameScenePauseControlNormal));
    mPauseChecker = new PauseButtonCheckerInGame();
}

void GameScenePauseControl::registerNervePauseMenu(const Nerve* pNerve) {
    mPauseMenuNerve = pNerve;
}

void GameScenePauseControl::requestPauseMenuOff() {
    mPauseMenuOff = true;
}

void GameScenePauseControl::exeNormal() {
    tryStartPauseMenu();

    if (mPauseMenuOff) {
#ifdef TARGET_PC
        port_input_discard_pause_request();  // a press that closed the menu does not open it again
#endif
        AudWrap::getSystem()->exitPauseMenu();
        mScene->setNerveAfterPauseMenu();
        mPauseMenuOff = false;
        mScene->mPauseSeq->deactivate();
    }
}

bool GameScenePauseControl::tryStartPauseMenu() {
    if (mScene->isPermitToPauseMenu()) {
        mPauseChecker->update();

#ifdef TARGET_PC
        // X or Menu on the VR controllers: one press opens the menu (the
        // Wii needed + or - held for 12 frames, and no A, B or shake).
        if (port_input_take_pause_request()) {
            mScene->mPauseSeq->startPause(GamePauseSequence::ActivePause);
            mScene->setNerve(mPauseMenuNerve);
            return true;
        }
#endif

        if (mPauseChecker->isPermitToMinusPause()) {
            mScene->mPauseSeq->startPause(GamePauseSequence::ActivePause);
            mScene->setNerve(mPauseMenuNerve);
            return true;
        }

        if (mPauseChecker->isPermitToPlusPause()) {
            mScene->mPauseSeq->startPause(GamePauseSequence::ActivePause);
            mScene->setNerve(mPauseMenuNerve);
            return true;
        }
    }

    return false;
}

GameScenePauseControl::~GameScenePauseControl() {
}
