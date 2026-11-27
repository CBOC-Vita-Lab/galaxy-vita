#include "Game/Scene/GameSceneScenarioOpeningCameraState.hpp"
#include "Game/AudioLib/AudSystem.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/ScenarioTitle.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#ifdef TARGET_PC
#include "port/xskip.h"  // (xskip27)
#endif

#ifdef TARGET_PC
extern "C" int port_wipe_open_site(int site) __attribute__((weak));  // (hitch33 14: platform/vgx/src/vgx_backend_gl.cpp; see GameScene.cpp)
#endif

namespace {
    NEW_NERVE(GameSceneScenarioOpeningCameraStateWait, GameSceneScenarioOpeningCameraState, Wait);
    NEW_NERVE(GameSceneScenarioOpeningCameraStatePlay, GameSceneScenarioOpeningCameraState, Play);
};  // namespace

GameSceneScenarioOpeningCameraState::GameSceneScenarioOpeningCameraState() : NerveExecutor("\x83\x56\x83\x69\x83\x8a\x83\x49\x8a\x4a\x8e\x6e\x83\x4a\x83\x81\x83\x89\x8d\xc4\x90\xb6") {
    mBaseMtx.identity();
    initNerve(GET_NERVE_ANON(GameSceneScenarioOpeningCameraStatePlay));
    mScenarioTitle = new ScenarioTitle();
    mScenarioTitle->initWithoutIter();
    mScenarioTitle->kill();
}

void GameSceneScenarioOpeningCameraState::update() {
    updateNerve();
}

bool GameSceneScenarioOpeningCameraState::isDone() const {
    return isNerve(GET_NERVE_ANON(GameSceneScenarioOpeningCameraStateWait));
}

void GameSceneScenarioOpeningCameraState::start() {
    MR::stopSceneForScenarioOpeningCamera();
    MR::hidePlayer();
    MR::startStartAnimCamera();
    MR::startStarPointerModeDemo(this);
#ifdef TARGET_PC
    // (hitch33 14, GXV_WIPE_OPEN_CAMERA; console job 2890) GameScene::startStagePlayFirst closed the black fade for the stage's first pictures (GXV_WIPE_OPEN_NOWIPE);
    // the line below forced it open one turn later, so the hold was dropped and Honeyhive's first pictures and queue links were shown. While the port's hold for
    // this scene is still owed the fade stays closed and opens in 2 turns when the port lets it go; the opening camera runs behind it meanwhile.
    if (port_wipe_open_site && port_wipe_open_site(4)) {
        MR::forceCloseWipeFade();
        MR::openWipeFade(2);
    } else
#endif
    MR::forceOpenWipeFade();
    MR::forceToFrameCinemaFrame();
    MR::startStageBGM("BGM_START_DEMO", false);
    PSMTXCopy(MR::getPlayerBaseMtx(), mBaseMtx);

    TVec3f namePos;

    if (MR::tryFindNamePos("\x83\x58\x83\x5e\x81\x5b\x83\x67\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x8a\x83\x49\x8d\xc0\x95\x57", &namePos, nullptr)) {
        MR::setPlayerPos("\x83\x58\x83\x5e\x81\x5b\x83\x67\x83\x4a\x83\x81\x83\x89\x83\x7d\x83\x8a\x83\x49\x8d\xc0\x95\x57");
    }
}

void GameSceneScenarioOpeningCameraState::end() {
    MR::stopStageBGM(10);
    MR::playSceneForScenarioOpeningCamera();
    MR::showPlayer();
    MR::endStartAnimCamera();
    MR::endStarPointerMode(this);
    mScenarioTitle->kill();
    MR::setPlayerBaseMtx(mBaseMtx);
}

void GameSceneScenarioOpeningCameraState::exeWait() {
}

void GameSceneScenarioOpeningCameraState::exePlay() {
    if (MR::isFirstStep(this)) {
        mScenarioTitle->start();
        MR::deactivateDefaultGameLayout();
        start();
    }

    if (trySkipTrigger()) {
#ifdef TARGET_PC
        port_xskip_accept(PORT_XSKIP_OPENING);  // (xskip27: the marker; the cinema frame is black at once below, so the cover is opaque from this turn)
#endif
        end();
        MR::forceToBlankCinemaFrame();
#ifdef TARGET_PC
        port_xskip_cut(PORT_XSKIP_OPENING);
#endif
        setNerve(GET_NERVE_ANON(GameSceneScenarioOpeningCameraStateWait));
    } else {
        if (MR::isStep(this, MR::getStartAnimCameraFrame() - 60)) {
            MR::tryFrameToBlankCinemaFrame();
            mScenarioTitle->end();
        }

        if (MR::isStep(this, MR::getStartAnimCameraFrame() - 20)) {
            AudWrap::getSystem()->set830(30);
        }

        if (MR::isStartAnimCameraEnd() && MR::isStopCinemaFrame()) {
            end();
            setNerve(GET_NERVE_ANON(GameSceneScenarioOpeningCameraStateWait));
        }
    }
}

bool GameSceneScenarioOpeningCameraState::trySkipTrigger() const {
    if (MR::isFirstStep(this)) {
        return false;
    }

#ifdef TARGET_PC
    // Also on the first visit, once A has been held (port_skip_context).
    if (port_skip_take(PORT_SKIP_OPENING)) {
        return true;
    }
#endif

    return !MR::isAlreadyVisitedCurrentStageAndScenario() ? false : MR::testSystemPadTriggerDecide();
}
