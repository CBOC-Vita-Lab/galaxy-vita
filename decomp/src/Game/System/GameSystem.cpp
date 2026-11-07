#include "Game/System/GameSystem.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystemDimmingWatcher.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemException.hpp"
#include "Game/System/GameSystemFontHolder.hpp"
#include "Game/System/GameSystemFrameControl.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/GameSystemStationedArchiveLoader.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <nw4r/lyt/init.h>
#include <revolution.h>
#include "port/game_progress.h"
#ifdef TARGET_PC
#include "port/audio_cpu1.h"
#endif

#define GX_FIFO_SIZE 0x80000

#define INIT_AUDIO_KEY "\x83\x49\x81\x5b\x83\x66\x83\x42\x83\x49\x8f\x89\x8a\xfa\x89\xbb"  // "Audio Initialization"

namespace NrvGameSystem {
    NEW_NERVE(GameSystemInitializeAudio, GameSystem, InitializeAudio);
    NEW_NERVE(GameSystemInitializeLogoScene, GameSystem, InitializeLogoScene);
    NEW_NERVE(GameSystemLoadStationedArchive, GameSystem, LoadStationedArchive);
    NEW_NERVE(GameSystemWaitForReboot, GameSystem, WaitForReboot);
    NEW_NERVE(GameSystemNormal, GameSystem, Normal);
};  // namespace NrvGameSystem

#ifdef TARGET_PC
void port_game_main(void) {
#else
#ifdef TARGET_PC
void port_game_main(void) {
#else
void main(void) {
#endif
#endif
    OSInitFastCast();
    DVDInit();
    VIInit();
    HeapMemoryWatcher::createRootHeap();
    OSInitMutex(&MR::MutexHolder< 0 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 1 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 2 >::sMutex);
    nw4r::lyt::LytInit();
    MR::setLayoutDefaultAllocator();
    SingletonHolder< HeapMemoryWatcher >::init();
    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    FileRipper::setup(0x20000, MR::getStationedHeapNapa());
    GameSystemException::init();
    MR::initAcosTable();
    SingletonHolder< GameSystem >::init();
    SingletonHolder< GameSystem >::get()->init();

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();

    while (true) {
        pGameSystem->frameLoop();
    }
}

GameSystem::GameSystem()
    : NerveExecutor("GameSystem"), mFifoBase(nullptr), mSequenceDirector(nullptr), mErrorWatcher(nullptr), mFontHolder(nullptr),
      mFrameControl(nullptr), mObjHolder(nullptr), mSceneController(nullptr), mStationedArchiveLoader(nullptr), mHomeButtonLayout(nullptr),
      mSystemWipeHolder(nullptr), mHomeButtonStateNotifier(nullptr), mIsExecuteLoadSystemArchive(false) {
}

void GameSystem::init() {
    JKRAram::create(0xE00000, 0xFFFFFFFF, 8, 7, 3);
    mObjHolder = new GameSystemObjHolder();
    mFontHolder = new GameSystemFontHolder();
    mFontHolder->createFontFromEmbeddedData();
    initNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
    mSequenceDirector = new GameSequenceDirector();
    initGX();
    DrawSyncManager::start(0x300, 15);
    mSceneController = new GameSystemSceneController();
    mObjHolder->init();
    mErrorWatcher = new GameSystemErrorWatcher();
    mFrameControl = new GameSystemFrameControl();
    SingletonHolder< GameSystemResetAndPowerProcess >::init();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->initWithoutIter();
    mStationedArchiveLoader = new GameSystemStationedArchiveLoader();
    mHomeButtonLayout = new HomeButtonLayout();
    mHomeButtonStateNotifier = new HomeButtonStateNotifier();
    mDimmingWatcher = new GameSystemDimmingWatcher();
    setNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
}

bool GameSystem::isExecuteLoadSystemArchive() const {
    return mIsExecuteLoadSystemArchive;
}

bool GameSystem::isDoneLoadSystemArchive() const {
    return isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

void GameSystem::startToLoadSystemArchive() {
    mIsExecuteLoadSystemArchive = true;

    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    SingletonHolder< NameObjRegister >::get()->setCurrentHolder(mObjHolder->mObjHolder);
    setNerve(GET_NERVE(GameSystem, GameSystemLoadStationedArchive));
}

void GameSystem::exeInitializeAudio() {
    if (MR::isFirstStep(this)) {
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_START);
        MR::startFunctionAsyncExecute(MR::Functor(mObjHolder, &GameSystemObjHolder::createAudioSystem), 14, INIT_AUDIO_KEY);
    }

    PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_UPDATE_SCENE);
    updateSceneController();

    PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_GATE);
    const bool audioCreated = MR::isEndFunctionAsyncExecute(INIT_AUDIO_KEY);
    const bool waveLoaded = audioCreated && mObjHolder->mAudioSystem->isLoadDoneWaveDataAtSystemInit();
#if defined(__vita__)
    // Report each startup gate state once, without bypassing either gate.
    static unsigned reported = 0;
    const unsigned state = unsigned(audioCreated) | (unsigned(waveLoaded) << 1);
    if (!(reported & (1u << state))) {
        reported |= 1u << state;
        port_log("native-init audio-created=%u wave-loaded=%u", unsigned(audioCreated), unsigned(waveLoaded));
    }
#endif
    if (waveLoaded) {
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_WAIT);
#if defined(__vita__)
        port_log("native-init audio-wait-begin");
#endif
        MR::waitForEndFunctionAsyncExecute(INIT_AUDIO_KEY);
#if defined(__vita__)
        port_log("native-init audio-wait-complete");
#endif
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_NERVE);
        setNerve(GET_NERVE(GameSystem, GameSystemInitializeLogoScene));
    }
}

void GameSystem::exeInitializeLogoScene() {
    if (GameSystemFunction::isResetProcessing()) {
        setNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
    } else {
        if (MR::isFirstStep(this)) {
            PORT_GAME_PROGRESS(PORT_GAME_PHASE_LOGO_REQUEST);
#if defined(__vita__)
            port_log("native-init logo-request-begin");
#endif
            MR::requestChangeScene("Logo");
#if defined(__vita__)
            port_log("native-init logo-request-complete");
#endif
        }

        PORT_GAME_PROGRESS(PORT_GAME_PHASE_LOGO_UPDATE_SCENE);
        updateSceneController();
    }
}

void GameSystem::exeLoadStationedArchive() {
    mStationedArchiveLoader->update();
    updateSceneController();

    if (mStationedArchiveLoader->isDone()) {
        setNerve(GET_NERVE(GameSystem, GameSystemNormal));
    }
}

void GameSystem::exeWaitForReboot() {
}

void GameSystem::exeNormal() {
    updateSceneController();
    mStationedArchiveLoader->update();
}

void GameSystem::initGX() {
    if (mFifoBase == nullptr) {
        mFifoBase = new (32) u8[GX_FIFO_SIZE];
    }

    GXInit(mFifoBase, GX_FIFO_SIZE);
}

void GameSystem::initAfterStationedResourceLoaded() {
    mFontHolder->createFontFromFile();
    mObjHolder->initAfterStationedResourceLoaded();
    mHomeButtonLayout->initWithoutIter();
    mErrorWatcher->initAfterResourceLoaded();
    mSystemWipeHolder = MR::createSystemWipeHolder();
    mSceneController->initAfterStationedResourceLoaded();
    mSequenceDirector->initAfterResourceLoaded();
}

void GameSystem::prepareReset() {
    mStationedArchiveLoader->prepareReset();
}

inline bool isSystemWaitForReboot(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
}

inline bool isSystemNormal(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

bool GameSystem::isPreparedReset() const {
    return isSystemWaitForReboot(this) || isSystemNormal(this) || mStationedArchiveLoader->isPreparedReset();
}

#ifdef TARGET_PC
extern "C" int port_frameskip_turn(int allowed);
extern "C" void port_turn_skipped_set(int v);
extern "C" void port_mark_frame_thread(void);  // platform/src/port/perf.cpp
extern "C" void port_dl_flush_stale(int drop);   // GXV_DL_LAZY: decomp/src/Game/LiveActor/ModelManager.cpp
extern "C" void port_j3d_picture_begin(void);    // decomp/src/JSystem/J3DGraphBase/J3DPacket.cpp
extern "C" void port_anim_flush_stale(int drop); // GXV_ANIM_LOD: decomp/src/Game/LiveActor/LiveActor.cpp
extern "C" void port_par_flush_prepare(void);   // GXV_PAR_FLUSH (par2): decomp/src/Game/LiveActor/ModelManager.cpp
extern "C" void port_par_flush_end(void);
extern "C" void port_gx_drawdone_settle(int point);
extern "C" void port_view_flush(int drop);
extern "C" void port_gx_flush_scope(int on);
// GXV_PAR_VIEW=2 (decomp/src/Game/LiveActor/ModelManager.cpp): the drawn turn's view pass may leave its arithmetic to a worker while
// draw() runs (armed around port_view_flush below); every job is done and the worker out of the batch after draw() (retire).
extern "C" void port_par_view_async_arm(int on);
extern "C" void port_par_view_retire(void);
extern "C" void __PortGXFifoIdlePoint(void) __attribute__((weak));  // GXV_SEG_IDLE: platform/src/gx/gx_fifo.cpp
// GXV_ANIM_PROF (platform/src/port/perf.cpp): the drawn turn's flushes timed, and the end of every turn (its log every 300 turns)
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_flush(int which, int64_t ns);
extern "C" void port_anim_prof_turn(int drawn);
// GXV_DRAW_PROF (draw7; platform/src/port/perf.cpp): the drawn turn's draw phase and flushes, per stage and per draw list
extern "C" int port_draw_prof_on(void);
extern "C" void port_draw_prof_stage(int stage, int64_t ns);
extern "C" void port_draw_prof_turn(void);
#define PORT_PROF_FLUSH(which, call)                                          \
    do {                                                                      \
        if (port_anim_prof_on() || port_draw_prof_on()) {                     \
            const int64_t profT0 = port_host_time_ns();                       \
            call;                                                             \
            port_anim_prof_flush((which), port_host_time_ns() - profT0);      \
        } else {                                                              \
            call;                                                             \
        }                                                                     \
    } while (0)
#if defined(__vita__)
extern "C" void port_settings_game_boundary(int safe);
#endif
#endif

void GameSystem::frameLoop() {
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_FRAME_BEGIN);
#ifdef TARGET_PC
    // (GXV_SEG_IDLE, platform/src/gx/vgx_galaxy.cpp: the front end's staging segment changes here, between two turns, when the front
    // end has caught up, instead of filling up and making the game wait for the front end in the middle of a draw)
    if (__PortGXFifoIdlePoint) __PortGXFifoIdlePoint();
    port_mark_frame_thread();
#if defined(__vita__)
    port_settings_game_boundary(!mSceneController->isChangingScene() && !port_skip_fast_forwarding());
#endif
    port_perf_frame_begin();
    if (port_skip_fast_forwarding()) {
        // A cutscene being skipped: nothing is drawn (the headset view is
        // dimmed), and the game logic runs as often as fits in a frame.  The
        // retrace wait still gives the other threads (audio, loading) their
        // turn.
        int64_t start = port_host_time_ns();
        port_gx_drawdone_settle(3);
        port_gx_flush_scope(1);
        port_anim_flush_stale(0);
        port_dl_flush_stale(0);
        port_view_flush(0);
        port_gx_flush_scope(0);
        do {
            update();
            PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
            calcAnim();
            if (mSceneController->isChangingScene()) {
                // The skipped cutscene ends in another stage (Mario jumping
                // out of a dome into the star select): the scene change runs
                // at the normal pace, drawn, with its loading threads timed
                // as when nothing is skipped.  Fast-forwarded through, the
                // game stopped for good there.
                port_skip_scene_change();
                break;
            }
        } while (port_skip_fast_forwarding() && port_host_time_ns() - start < 12000000);
        port_perf_frame_work_done();
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_WAIT_RETRACE);
        MainLoopFramework::sManager->waitForRetrace();
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_FRAME_COMPLETE);
        return;
    }
    // Frame skipping (handhelds, port/perf.cpp): a turn that starts behind the display's clock
    // runs its logic without its picture, so that a slow frame costs pictures, not game speed.
    // Scene changes are drawn at the normal pace (see above).
    if (const int skip = port_frameskip_turn(!mSceneController->isChangingScene())) {
        port_turn_skipped_set(1);  // (GameScene::calcAnim leaves out the view and draw-list pass: nothing is drawn)
        update();
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
        calcAnim();
        port_turn_skipped_set(0);
        port_perf_frame_work_done();
        port_anim_prof_turn(0);
        // A turn skipped because the GPU is behind (1) is not late, and must not run ahead of the display's
        // clock: it takes its retrace as one that draws does. A turn skipped to catch up (2) does not wait:
        // the turns that follow it start on time again.
        if (skip == 1) {
            PORT_GAME_PROGRESS(PORT_GAME_PHASE_WAIT_RETRACE);
            MainLoopFramework::sManager->waitForRetrace();
        }
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_FRAME_COMPLETE);
        return;
    }
#endif
#ifdef TARGET_PC
    // (GXV_DL_LAZY) the materials the turns without a picture left out, from the same state, before this picture is drawn; the
    // front end must be past the last picture's lists first (a deferred draw-done wait is made here if one is still outstanding)
    port_par_flush_prepare();    // (GXV_PAR_FLUSH: the far models' joints handed to the fourth core, before the wait below)
    port_gx_drawdone_settle(3);  // (mode 3: the only wait of the turns since the last picture; mode 4: none)
    port_gx_flush_scope(1);      // (modes 3, 4: what follows writes the half the front end does not read)
    PORT_PROF_FLUSH(0, port_anim_flush_stale(0));  // (GXV_ANIM_LOD: the far models' joints, exactly)
    port_par_flush_end();        // (GXV_PAR_FLUSH: no flush job is read after the flush)
    PORT_PROF_FLUSH(1, port_dl_flush_stale(0));
    port_par_view_async_arm(1);                    // (GXV_PAR_VIEW=2: draw() follows the view pass at once)
    PORT_PROF_FLUSH(2, port_view_flush(0));        // (GXV_DRAWDONE_DEFER 3, 4: the view pass left out since the last picture)
    port_par_view_async_arm(0);
    port_gx_flush_scope(0);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_BEGIN_RENDER);
    MainLoopFramework::sManager->beginRender();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
#ifdef TARGET_PC
    port_j3d_picture_begin();  // (GXV_J3D_MATMERGE: the drawn picture's number; the "j3d lists:" counters)
#endif
#ifdef TARGET_PC
    if (port_draw_prof_on()) {
        const int64_t drawT0 = port_host_time_ns();
        draw();
        port_draw_prof_stage(0, port_host_time_ns() - drawT0);
    } else {
        draw();
    }
#else
    draw();
#endif
#ifdef TARGET_PC
    port_par_view_retire();  // (GXV_PAR_VIEW=2: the view jobs the draw did not reach, and the worker out of the batch)
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_END_RENDER);
    MainLoopFramework::sManager->endRender();
    update();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
    calcAnim();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CAPTURE);
    mObjHolder->captureIfAllowForScreenPreserver();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_END_FRAME);
    MainLoopFramework::sManager->endFrame();
#ifdef TARGET_PC
    port_perf_frame_work_done();
    port_anim_prof_turn(1);
    port_draw_prof_turn();
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_WAIT_RETRACE);
    MainLoopFramework::sManager->waitForRetrace();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_FRAME_COMPLETE);
}

void GameSystem::draw() {
#ifdef TARGET_PC
    // GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp): the game's audio thread may run on core 1 meanwhile; drawing does not drive JAudio
    PORT_AUDIO_WINDOW(1);
    const bool prof = port_draw_prof_on() != 0;
    int64_t profT = prof ? port_host_time_ns() : 0;
    mSceneController->drawScene();
    if (prof) {
        const int64_t now = port_host_time_ns();
        port_draw_prof_stage(1, now - profT);
        profT = now;
    }
#else
    mSceneController->drawScene();
#endif
#ifdef TARGET_PC
    // Everything drawn over the scene is 2D too: the save and error windows,
    // the pointer's cursor, the system wipe.  In VR it goes on the HUD panel
    // with the scene's own 2D, where the pointer aims (drawn into the eyes'
    // images instead, it covered the whole view, out of the pointer's reach).
    port_gx_marker(PORT_GX_MARK_HUD_BEGIN);
#endif
    mSequenceDirector->draw();
#ifdef TARGET_PC
    if (prof) {
        port_draw_prof_stage(2, port_host_time_ns() - profT);
    }
    port_gx_marker(PORT_GX_MARK_POINTER_BEGIN);
#endif
    mObjHolder->drawStarPointer();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_POINTER_END);
#endif
    mObjHolder->drawBeforeEndRender();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->draw();
    }

    mErrorWatcher->draw();
    mHomeButtonLayout->draw();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->draw();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_HUD_END);
    PORT_AUDIO_WINDOW(0);
#endif
}

#ifdef TARGET_PC
extern "C" void port_gx_drawdone_settle(int point);  // GXV_DRAWDONE_DEFER: platform/src/port/perf.cpp, MainLoopFramework.cpp
#endif

void GameSystem::update() {
#ifdef TARGET_PC
    port_gx_drawdone_settle(1);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_RESET);
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->movement();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_SCENE);
    mSceneController->checkRequestAndChangeScene();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_OBJECTS);
    mObjHolder->update();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_HOME);
    mHomeButtonLayout->movement();

    if (!mHomeButtonLayout->isActive()) {
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_ERROR);
        mErrorWatcher->movement();
    }

    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_DIMMING);
    mDimmingWatcher->_5 = mErrorWatcher->isWarning() || mHomeButtonLayout->isActive() || GameSequenceFunction::isActiveSaveDataHandleSequence();
    mDimmingWatcher->update();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_NERVE);
    updateNerve();
}

void GameSystem::updateSceneController() {
    bool isSceneUpdate = true;
    bool isResetProcessing = SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive();

    mObjHolder->updateAudioSystem();

    if (isResetProcessing) {
        isSceneUpdate = false;
    }

    if (mHomeButtonLayout->isActive()) {
        isSceneUpdate = false;
    }

    if (GameSystemFunction::isOccurredSystemWarning()) {
        isSceneUpdate = false;
    }

    mHomeButtonStateNotifier->update(mHomeButtonLayout->isActive() || GameSystemFunction::isOccurredSystemWarning());

    if (isSceneUpdate || isResetProcessing) {
        mSequenceDirector->update();
    }

    if (isSceneUpdate || mSceneController->isFirstUpdateSceneNerveNormal()) {
        if (mSystemWipeHolder != nullptr) {
            mSystemWipeHolder->movement();
        }

        // (the profile's "update-scene" phase from here: the scene's movement, apart from the sound objects before it)
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_SCENE);
        mSceneController->updateScene();
    }

    if (isResetProcessing) {
        mSceneController->updateSceneDuringResetProcessing();
    }
}

void GameSystem::calcAnim() {
#ifdef TARGET_PC
    // GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp): the game's audio thread may run on core 1 meanwhile (also through the front-end
    // wait below); the animation, effect and view passes do not drive JAudio (the sound starts and stops are in update())
    PORT_AUDIO_WINDOW(1);
    port_gx_drawdone_settle(2);
#endif
    mSceneController->calcAnimScene();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->calcAnim();
    }
#ifdef TARGET_PC
    PORT_AUDIO_WINDOW(0);
#endif
}
