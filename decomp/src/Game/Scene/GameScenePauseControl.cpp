#include "Game/Scene/GameScenePauseControl.hpp"
#include "Game/AudioLib/AudSystem.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Scene/GameScene.hpp"
#include "Game/Screen/GamePauseSequence.hpp"
#include "Game/System/PauseButtonCheckerInGame.hpp"
#ifdef TARGET_PC
#include "Game/MP/MpGame.hpp"  // MP3: the session menu replaces the pause menu
#include "Game/MP/MpUi.hpp"    // MP3 fix3: ... also while the session reconnects
#endif

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
#ifdef TARGET_PC
    tryStartPauseMenuMp();   // (MP3: the session menu in front of tryStartPauseMenu)
#else
    tryStartPauseMenu();
#endif

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

#ifdef TARGET_PC
// MP3: in a multiplayer session the pause press opens the session menu; the world keeps running (no pause nerve).
// fix3: also while the session reconnects (mp_active() is false then; two bool reads in single-player).
// (merge of 3.0 on c181: a wrapper in front of tryStartPauseMenu, which keeps c181's text, control flow and PGO profile; same order as before: permit, update, the session's press, then the game's)
bool GameScenePauseControl::tryStartPauseMenuMp() {
    if ((mp_active() || MpUi::reconnecting()) && MpFlow::replacePause() && mScene->isPermitToPauseMenu()) {
        mPauseChecker->update();
        const bool press = port_input_take_pause_request() || mPauseChecker->isPermitToMinusPause() || mPauseChecker->isPermitToPlusPause();
        if (press && !MpUi::isMenuOpen()) {
            MpUi::openSessionMenu();
        }
        return false;
    }
    return tryStartPauseMenu();
}
#endif

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
