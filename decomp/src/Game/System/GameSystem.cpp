#include "Game/System/GameSystem.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
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
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF, logic23: update() and calcAnim() timed whole, so the part outside the lists shows)
#else
#define PORT_LOGIC_COARSE(id) do { } while (0)  // (game24: GXV_LOGIC_PROF=3's update parts; PC only)
#endif
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
extern "C" int port_audio_window_wide_on(void);  // (imp45, GXV_AUDIO_WINDOW_WIDE: platform/src/port/perf.cpp)
extern "C" int port_frameskip_turn(int allowed);
// GXV_HRW (hrw26, platform/src/gx/vgx_galaxy.cpp): half-rate world, full-rate camera. Nothing but a mode read when it is 0.
extern "C" void port_hrw_turn_begin(void);
extern "C" void port_hrw_draw_begin(void);
extern "C" void port_hrw_undrawn_begin(void);
extern "C" void port_hrw_fresh_draw(void);
extern "C" int port_hrw_fresh_late(void);    // (warp32 PATCH22, GXV_HRW_FRESH_LATE: vgx_galaxy.cpp)
extern "C" void port_hrw_fresh_early(void);
extern "C" void port_hrw_turn_end(int drawn);
extern "C" int port_hrw_mode(void);
extern "C" int port_hrw_pointer_on(void);
extern "C" void port_hrw_hud_note(long long ns);
extern "C" void port_turn_skipped_set(int v);
extern "C" void port_turn_booked_undrawn(void);  // (platform/src/port/perf.cpp, q30-28)
extern "C" void port_vgx_lane_draw_end(void) __attribute__((weak));   // (platform/src/gx/vgx_galaxy.cpp, pace33 PATCH5: the "fe-lane:" line's end of the game's draw)
extern "C" void port_late_turn_wait(int code);   // (platform/src/port/perf.cpp, pace32 GXV_LATE_TURN: the undrawn turn of an even 30 starts late, so its pad read is fresh)
// GXV_UNDRAWN_LEAN (undrawn26, platform/src/port/perf.cpp, ShadowController.cpp)
extern "C" void port_undrawn_turn_begin(int undrawn);
extern "C" void port_undrawn_turn_end(int drawn);
extern "C" void port_undrawn_flush(void);
extern "C" void port_shadow_flush_pending(void);
extern "C" void port_mark_frame_thread(void);  // platform/src/port/perf.cpp
#include "port/xskip.h"  // (xskip27)
extern "C" void port_flow_beat(void);          // (flow23: decomp/src/Game/Scene/GameScene.cpp)
extern "C" void port_dl_flush_stale(int drop);   // GXV_DL_LAZY: decomp/src/Game/LiveActor/ModelManager.cpp
extern "C" void port_j3d_picture_begin(void);    // decomp/src/JSystem/J3DGraphBase/J3DPacket.cpp
extern "C" void port_anim_flush_stale(int drop); // GXV_ANIM_LOD: decomp/src/Game/LiveActor/LiveActor.cpp
extern "C" void port_par_flush_prepare(void);   // GXV_PAR_FLUSH (par2): decomp/src/Game/LiveActor/ModelManager.cpp
extern "C" void port_par_flush_end(void);
// GXV_PAR_PLANT_EARLY (parplant23): the next drawn turn's plant batch listed at the end of this turn; an early batch still open when the
// game's state is about to change is ended first (decomp/src/Game/LiveActor/ModelManager.cpp)
extern "C" void port_par_plant_turn_end(int drawn);
extern "C" void port_par_plant_close_early(void);
extern "C" void port_gx_drawdone_settle(int point);
extern "C" void port_view_flush(int drop);
extern "C" void port_gx_flush_scope(int on);
// GXV_PAR_VIEW=2 (decomp/src/Game/LiveActor/ModelManager.cpp): the drawn turn's view pass may leave its arithmetic to a worker while
// draw() runs (armed around port_view_flush below); every job is done and the worker out of the batch after draw() (retire).
extern "C" void port_par_view_async_arm(int on);
extern "C" void port_par_view_retire(void);
// GXV_PAR_DL (gamedraw23; decomp/src/Game/LiveActor/ModelManager.cpp): the material lists made by a worker on the fourth core while the game
// thread makes the view pass; joined (and j3dSys merged) before the draw. 0 from begin: the lists are flushed here as before.
extern "C" int port_par_dl_begin(void);
extern "C" void port_par_dl_end(void);
extern "C" void port_view_flush_player_last(void);  // (GameScene.cpp)
extern "C" void __PortGXFifoIdlePoint(void) __attribute__((weak));  // GXV_SEG_IDLE: platform/src/gx/gx_fifo.cpp
// GXV_DRAWPIPE (drawpipe24; decomp/src/Game/Scene/SceneFunction.cpp, port/draw_pipe.h): the drawn turn's four points (0 the hand-over:
// after the flushes, before the draw; 1 after the draw; 2 after update(); 3 after calcAnim()). Nothing but a switch read when it is 0.
extern "C" void port_drawpipe_point(int point);
extern "C" void port_drawpipe_undrawn(void);
// dpengine (port/draw_thread.h, SceneFunction.cpp): modes 3 and 4. port_drawpipe_draw makes the turn's draw (on the draw thread
// while this thread waits; or recorded here and executed there beside update()), or just calls the three parts of the serial
// draw it is given, in order, when the turn does not qualify; port_drawpipe_join ends an overlap before calcAnim() (nothing to
// do otherwise).
extern "C" int port_drawpipe_mode(void);
extern "C" void port_drawpipe_draw(void (*begin)(void*), void (*middle)(void*), void (*end)(void*), void* ctx);
extern "C" int port_drawpipe_join(void);  // (1: a draw was out and has been joined)
extern "C" int port_drawpipe_join_late(void);        // (pipe61: SceneFunction.cpp, GXV_PIPE25_JOINLATE: 1 = the join of this turn is made after calcAnim())
extern "C" int port_drawpipe_join_after_calc(void);  // (pipe61: the join after calcAnim(); 1: a draw was out and has been joined)
// GXV_ANIM_PROF (platform/src/port/perf.cpp): the drawn turn's flushes timed, and the end of every turn (its log every 300 turns)
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_flush(int which, int64_t ns);
extern "C" void port_anim_prof_turn(int drawn);
// GXV_DRAW_PROF (draw7; platform/src/port/perf.cpp): the drawn turn's draw phase and flushes, per stage and per draw list
extern "C" int port_draw_prof_on(void);
extern "C" void port_draw_prof_stage(int stage, int64_t ns);
extern "C" void port_draw_prof_turn(void);
extern "C" void port_draw_prof_frame_begin(void);  // (GXV_DRAW_PROF=2, flushcut: the "turn" stage's start)
// A scope of the drawn turn timed into one of port_draw_prof_stage's stages (a branch not taken while the profile is off).
#define PORT_DRAW_LAP(t, stage)                                                \
    do {                                                                       \
        if (t) {                                                               \
            const int64_t lapNow = port_host_time_ns();                        \
            port_draw_prof_stage((stage), lapNow - (t));                       \
            (t) = lapNow;                                                      \
        }                                                                      \
    } while (0)
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
#include "port/turn_prof.h"  // (GXV_TURN_PROF, gamepar: the turn split into categories, "turn-prof:" in log.txt)
#endif

#ifdef TARGET_PC
// The drawn turn's draw as frameLoop always made it, in three functions: beginRender; draw() with what belongs to it; endRender.
// Modes 0-2 call them one after the other where the lines stood. GXV_DRAWPIPE 3 makes the same three calls on the draw thread.
// GXV_DRAWPIPE 4 makes the first here, the second here with the GX library recording (the draw thread executes the recording
// beside update()), and the third here at the join: beginRender and endRender make GX calls a recording cannot hold (the frame
// buffer exchange, GXSetDispCopyYScale and the copy), and the copy must follow the whole picture in the stream.
void GameSystem::portDrawBeginEntry(void* pSelf) {
    static_cast< GameSystem* >(pSelf)->portDrawBegin();
}
void GameSystem::portDrawMiddleEntry(void* pSelf) {
    static_cast< GameSystem* >(pSelf)->portDrawMiddle();
}
void GameSystem::portDrawEndEntry(void* pSelf) {
    static_cast< GameSystem* >(pSelf)->portDrawEnd();
}
void GameSystem::portDrawBegin() {
    const bool drawProf = port_draw_prof_on() != 0;
    int64_t drawLap = 0;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_BEGIN_RENDER);
    drawLap = drawProf ? port_host_time_ns() : 0;
    MainLoopFramework::sManager->beginRender();
    PORT_DRAW_LAP(drawLap, 11);
}
void GameSystem::portDrawMiddle() {
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
    port_j3d_picture_begin();  // (GXV_J3D_MATMERGE: the drawn picture's number; the "j3d lists:" counters)
    if (port_draw_prof_on()) {
        const int64_t drawT0 = port_host_time_ns();
        draw();
        port_draw_prof_stage(0, port_host_time_ns() - drawT0);
    } else {
        draw();
    }
    port_par_view_retire();  // (GXV_PAR_VIEW=2: the view jobs the draw did not reach, and the worker out of the batch)
}
void GameSystem::portDrawEnd() {
    const bool drawProf = port_draw_prof_on() != 0;
    int64_t drawLap = 0;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_END_RENDER);
    drawLap = drawProf ? port_host_time_ns() : 0;
    MainLoopFramework::sManager->endRender();
    PORT_DRAW_LAP(drawLap, 12);
    if (port_vgx_lane_draw_end) port_vgx_lane_draw_end();  // (pace33 PATCH5; one clock read)
}
#endif

void GameSystem::frameLoop() {
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_FRAME_BEGIN);
#ifdef TARGET_PC
    // (GXV_SEG_IDLE, platform/src/gx/vgx_galaxy.cpp: the front end's staging segment changes here, between two turns, when the front
    // end has caught up, instead of filling up and making the game wait for the front end in the middle of a draw)
    if (__PortGXFifoIdlePoint) __PortGXFifoIdlePoint();
    port_mark_frame_thread();
    port_flow_beat();  // (flow23, PETARI_FLOW_LOG / PETARI_FLOW: the "flow:" changes and heartbeat, every turn, drawn or not)
#if defined(__vita__)
    port_settings_game_boundary(!mSceneController->isChangingScene() && !port_skip_fast_forwarding());
#endif
    port_perf_frame_begin();
    port_draw_prof_frame_begin();
    port_turn_prof_begin();
    port_hrw_turn_begin();
    if (port_skip_fast_forwarding()) {
        // A cutscene being skipped: nothing is drawn (the headset view is
        // dimmed), and the game logic runs as often as fits in a frame.  The
        // retrace wait still gives the other threads (audio, loading) their
        // turn.
        int64_t start = port_host_time_ns();
        port_xskip_frame(0);  // (xskip27)
        port_gx_drawdone_settle(3);
        port_gx_flush_scope(1);
        port_anim_flush_stale(0);
        port_dl_flush_stale(0);
        port_view_flush(0);
        port_gx_flush_scope(0);
        port_shadow_flush_pending();  // (GXV_UNDRAWN_LEAN)
        port_undrawn_flush();
        port_undrawn_turn_begin(1);
        // (xskip27, GXV_XSKIP_FF_PREDICT, default 1) the loop stops when the next update would not fit the 12 ms (the longest of the last update and the running mean), not after it
        // has overrun: the old test ran one more update whenever 11.9 ms were used, so a turn lasted up to 12 ms + one update (a 5-8 ms update made 17-20 ms turns). The first
        // update of a turn always runs.
        static const bool ffPredict = !(getenv("GXV_XSKIP_FF_PREDICT") && *getenv("GXV_XSKIP_FF_PREDICT") == '0');
        static int64_t ffMeanNs = 0;
        int64_t ffLap = port_host_time_ns();
        do {
            update();
            PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
            calcAnim();
            {
                const int64_t t = port_host_time_ns();
                const int64_t one = t - ffLap;
                ffLap = t;
                ffMeanNs = ffMeanNs == 0 ? one : (ffMeanNs * 3 + one) / 4;
                if (ffPredict && !mSceneController->isChangingScene() && port_skip_fast_forwarding() && t - start + (one > ffMeanNs ? one : ffMeanNs) >= 12000000) {
                    break;
                }
            }
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
    if (const int skip = port_frameskip_turn(!mSceneController->isChangingScene() && !port_xskip_calm_turn())) {  // (xskip27: no catch-up in an accepted skip's window)
        port_turn_skipped_set(1);  // (GameScene::calcAnim leaves out the view and draw-list pass: nothing is drawn)
        port_hrw_undrawn_begin();  // (GXV_HRW: the camera the last update made; the second picture of the pair the last drawn turn opened)
        // (warp32 PATCH22, GXV_HRW_FRESH_LATE: only the player's view pass now, without a wait; his draws and the ask after the update, when the front end has finished the first picture)
        const int hrwFreshLate = port_hrw_fresh_late();
        if (hrwFreshLate) {
            port_hrw_fresh_early();
        } else {
            port_hrw_fresh_draw();  // (GXV_HRW_FRESH: Mario, drawn now with this turn's camera for the pair's second picture; the ask goes with it)
        }
        port_undrawn_turn_begin(1);  // (GXV_UNDRAWN_LEAN: the turn's mode, the stage's turn count)
        port_xskip_frame(0);  // (xskip27)
        port_turn_prof_lap(PORT_TP_OTHER);
        port_late_turn_wait(skip);  // (pace32 GXV_LATE_TURN, default off: nothing)
        update();
        port_turn_prof_lap(PORT_TP_UPDATE);
        if (hrwFreshLate) {
            port_hrw_fresh_draw();
            port_turn_prof_lap(PORT_TP_OTHER);
        }
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
        calcAnim();
        port_turn_prof_lap(PORT_TP_VIEW);
        port_turn_booked_undrawn();  // (q30-28: the turn's work is booked as an UNDRAWN one by port_perf_frame_work_done below: the flag is cleared before that call, and was read as 0 there)
        port_turn_skipped_set(0);
        port_par_plant_turn_end(0);  // (GXV_PAR_PLANT_EARLY: the next turn's flush batch, if it is predicted to draw)
        port_drawpipe_undrawn();     // (GXV_DRAWPIPE=2: this turn's writes are not the next hand-over's flushes)
        port_perf_frame_work_done();
        port_anim_prof_turn(0);
        port_turn_prof_end(0);
        port_undrawn_turn_end(0);  // (GXV_UNDRAWN_LEAN: the census and the state hash, after the turn's own timers)
        port_hrw_turn_end(0);
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
    port_turn_prof_mark(PORT_TPM_DECIDE);  // (GXV_TURN_PROF: the pacer's decision, a drawn turn)
    port_undrawn_turn_begin(0);
    port_xskip_frame(1);  // (xskip27)
#endif
#ifdef TARGET_PC
    // (GXV_DL_LAZY) the materials the turns without a picture left out, from the same state, before this picture is drawn; the
    // front end must be past the last picture's lists first (a deferred draw-done wait is made here if one is still outstanding)
    // imp45 (22 Nov), GXV_AUDIO_WINDOW_WIDE=1 (default 0): the audio window (GXV_AUDIO_CPU1, platform/src/os/os_thread.cpp) opens here, before the
    // flushes, instead of at draw(): the flushes (the far joints, the material lists, the view pass, the left-out shadow casts; ~3 ms of a heavy drawn
    // turn) do not drive JAudio either, and the JAI / JASTrack guards (PORT_AUDIO_TOUCH) end a window at once should anything in it do so, as they do
    // for calcAnim() and draw(). draw()'s own opening is then a no-op; its end closes the window as before. Why: the console logs of 22 Nov show the
    // audio thread taking 0.49-0.51 ms a turn from the game thread on core 0 at Beach Bowl and Honeyhive (runs outside the windows, and waits at window
    // ends); the "audio cpu:" line (taken from the game thread, runs on core 0 / core 1, windows) is the measure.
    if (port_audio_window_wide_on()) {
        PORT_AUDIO_WINDOW(1);
    }
    port_par_flush_prepare();    // (GXV_PAR_FLUSH: the far models' joints handed to the fourth core, before the wait below)
    port_turn_prof_mark(PORT_TPM_PREPARE);
    const bool drawProf = port_draw_prof_on() != 0;
    int64_t drawLap = drawProf ? port_host_time_ns() : 0;
    port_turn_prof_lap(PORT_TP_OTHER);
    port_gx_drawdone_settle(3);  // (mode 3: the only wait of the turns since the last picture; mode 4: none)
    PORT_DRAW_LAP(drawLap, 13);  // (GXV_DRAW_PROF: the wait for the front end)
    port_turn_prof_lap(PORT_TP_FEWAIT);
    port_gx_flush_scope(1);      // (modes 3, 4: what follows writes the half the front end does not read)
    PORT_PROF_FLUSH(0, port_anim_flush_stale(0));  // (GXV_ANIM_LOD: the far models' joints, exactly)
    port_par_flush_end();        // (GXV_PAR_FLUSH: no flush job is read after the flush)
    port_turn_prof_lap(PORT_TP_FL_ANIM);
    const int parDl = port_par_dl_begin();  // (GXV_PAR_DL: 1 = the material lists are the worker's, joined below)
    if (!parDl) {
        PORT_PROF_FLUSH(1, port_dl_flush_stale(0));
    }
    port_turn_prof_lap(PORT_TP_FL_DL);
    port_par_view_async_arm(1);                    // (GXV_PAR_VIEW=2: draw() follows the view pass at once)
    PORT_PROF_FLUSH(2, port_view_flush(0));        // (GXV_DRAWDONE_DEFER 3, 4: the view pass left out since the last picture)
    port_par_view_async_arm(0);
    if (parDl) {
        port_par_dl_end();  // (GXV_PAR_DL: the join; with the turn-prof line, booked under "view")
        port_view_flush_player_last();  // (GXV_PAR_DL round 2: a split pass's player part, after the join)
    }
    port_gx_flush_scope(0);
    port_turn_prof_lap(PORT_TP_FL_VIEW);
    port_shadow_flush_pending();  // (GXV_UNDRAWN_LEAN: the shadow casts the turns since the last picture left out, from the state they ended in)
    port_undrawn_flush();         // (... and the joints and casts left out are made: clean)
    port_turn_prof_lap(PORT_TP_SHADOW);
    port_drawpipe_point(0);  // (GXV_DRAWPIPE: the hand-over; the lists the J3D passes walk are frozen here)
#endif
#ifdef TARGET_PC
    port_hrw_draw_begin();  // (GXV_HRW: V_A, the camera this picture is drawn with; the picture is marked for the render thread to keep)
    if (port_drawpipe_mode() < 3) {
        portDrawBegin();
        portDrawMiddle();
        portDrawEnd();
    } else {
        // (GXV_DRAWPIPE 3 / 4: the same draw, on the draw thread or recorded for it; here when the turn does not qualify)
        port_drawpipe_draw(&GameSystem::portDrawBeginEntry, &GameSystem::portDrawMiddleEntry, &GameSystem::portDrawEndEntry, this);
    }
    port_turn_prof_lap(PORT_TP_DRAW);
    port_drawpipe_point(1);  // (GXV_DRAWPIPE: the draw is over, or handed over; no pass is asked for after this)
    PORT_AUDIO_WINDOW(0);    // (imp45, GXV_AUDIO_WINDOW_WIDE: never open into update(), whatever path the draw took; a no-op when draw() closed it)
#else
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_BEGIN_RENDER);
    MainLoopFramework::sManager->beginRender();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
    draw();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_END_RENDER);
    MainLoopFramework::sManager->endRender();
#endif
    update();
#ifdef TARGET_PC
    port_turn_prof_lap(PORT_TP_UPDATE);
    // (GXV_DRAWPIPE=4: the draw thread's draw of this turn is over before calc-anim writes what it reads; endRender is made here
    // then, and the wait and it are the draw's time in the turn profile)
    // (pipe61, GXV_PIPE25_JOINLATE=1: the late draw's helper is joined AFTER calcAnim(), not before it: the overlap is update + calc-anim; port_drawpipe_join_after_calc)
    const bool joinLate = port_drawpipe_join_late() != 0;
    if (!joinLate && port_drawpipe_join()) {
        port_turn_prof_lap(PORT_TP_DRAW);
    }
    port_drawpipe_point(2);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
    calcAnim();
#ifdef TARGET_PC
    port_turn_prof_lap(PORT_TP_VIEW);
    if (joinLate && port_drawpipe_join_after_calc()) {
        port_turn_prof_lap(PORT_TP_DRAW);
    }
    port_turn_prof_mark(-1);
    port_drawpipe_point(3);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CAPTURE);
    mObjHolder->captureIfAllowForScreenPreserver();
#ifdef TARGET_PC
    port_turn_prof_mark(PORT_TPM_CAPTURE);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_END_FRAME);
#ifdef TARGET_PC
    const int64_t endWaitT0 = port_endwait_begin();  // (GXV_ENDWAIT_LOG: 0 when off)
#endif
    MainLoopFramework::sManager->endFrame();
#ifdef TARGET_PC
    if (endWaitT0) port_endwait_end(endWaitT0, 0);
    port_turn_prof_mark(PORT_TPM_ENDFRAME);
    port_par_plant_turn_end(1);  // (GXV_PAR_PLANT_EARLY: the next turn's flush batch, if it is predicted to draw)
    port_perf_frame_work_done();
    port_anim_prof_turn(1);
    port_draw_prof_turn();
    port_turn_prof_mark(PORT_TPM_TURNEND);
    port_turn_prof_end(1);
    port_undrawn_turn_end(1);
    port_hrw_turn_end(1);
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
    ImageEffectLocalUtil::portPictureTick();  // (gpu23: PETARI_BLOOM_HALFRATE / PETARI_SHADOWTEX_HALFRATE's picture count)
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
    const int64_t hrwHudT0 = port_hrw_mode() == 3 ? port_host_time_ns() : 0;  // (hrw28, census: what a fresh HUD would cost)
#endif
    mSequenceDirector->draw();
#ifdef TARGET_PC
    if (hrwHudT0) {
        port_hrw_hud_note(port_host_time_ns() - hrwHudT0);
    }
    if (prof) {
        port_draw_prof_stage(2, port_host_time_ns() - profT);
    }
    port_gx_marker(PORT_GX_MARK_POINTER_BEGIN);
    const bool hrwPointer = port_hrw_pointer_on() != 0;
    if (hrwPointer) {
        port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_POINTER, PORT_HRW_PH_POINTER));  // (hrw28, GXV_HRW_POINTER: drawn again for the pair's second picture)
    }
#endif
    mObjHolder->drawStarPointer();
#ifdef TARGET_PC
    if (hrwPointer) {
        port_gx_marker(PORT_GX_OWNER(0, 0));
    }
    port_gx_marker(PORT_GX_MARK_POINTER_END);
#endif
    mObjHolder->drawBeforeEndRender();
#ifdef TARGET_PC
    if (prof) {
        const int64_t now = port_host_time_ns();
        port_draw_prof_stage(9, now - profT);  // (GXV_DRAW_PROF: the star pointer and what draws before the end of the render)
        profT = now;
    }
#endif

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->draw();
    }

    mErrorWatcher->draw();
    mHomeButtonLayout->draw();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->draw();
#ifdef TARGET_PC
    if (prof) {
        port_draw_prof_stage(10, port_host_time_ns() - profT);  // (the system wipe, the error and home layouts, the reset process)
    }
    port_gx_marker(PORT_GX_MARK_HUD_END);
    PORT_AUDIO_WINDOW(0);
#endif
}

#ifdef TARGET_PC
extern "C" void port_gx_drawdone_settle(int point);  // GXV_DRAWDONE_DEFER: platform/src/port/perf.cpp, MainLoopFramework.cpp
#endif

void GameSystem::update() {
#ifdef TARGET_PC
    PORT_LOGIC_COARSE(PORT_LS_PH_UPDATE);
    port_par_plant_close_early();  // (GXV_PAR_PLANT_EARLY: a plant batch listed for a picture this turn does not make ends before any write)
    port_gx_drawdone_settle(1);
#endif
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_RESET);
    {
        PORT_LOGIC_COARSE(PORT_LS_U_RESET);  // (GXV_LOGIC_PROF=3: update's parts outside the actor lists, timed every turn)
        SingletonHolder< GameSystemResetAndPowerProcess >::get()->movement();
    }
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_SCENE);
    {
        PORT_LOGIC_COARSE(PORT_LS_U_SCENECHG);
        mSceneController->checkRequestAndChangeScene();
    }
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_OBJECTS);
    {
        PORT_LOGIC_COARSE(PORT_LS_U_OBJHOLDER);
        mObjHolder->update();
    }
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_HOME);
    {
        PORT_LOGIC_COARSE(PORT_LS_U_HOME);
        mHomeButtonLayout->movement();

        if (!mHomeButtonLayout->isActive()) {
            PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_ERROR);
            mErrorWatcher->movement();
        }

        PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_DIMMING);
        mDimmingWatcher->_5 = mErrorWatcher->isWarning() || mHomeButtonLayout->isActive() || GameSequenceFunction::isActiveSaveDataHandleSequence();
        mDimmingWatcher->update();
    }
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_NERVE);
    {
        PORT_LOGIC_COARSE(PORT_LS_U_NERVE);
        updateNerve();
    }
}

void GameSystem::updateSceneController() {
    bool isSceneUpdate = true;
    bool isResetProcessing = SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive();

    {
        PORT_LOGIC_COARSE(PORT_LS_U_AUDIO);
        mObjHolder->updateAudioSystem();
    }

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
        PORT_LOGIC_COARSE(PORT_LS_U_SEQDIR);
        mSequenceDirector->update();
    }

    if (isSceneUpdate || mSceneController->isFirstUpdateSceneNerveNormal()) {
        if (mSystemWipeHolder != nullptr) {
            PORT_LOGIC_COARSE(PORT_LS_U_WIPE);
            mSystemWipeHolder->movement();
        }

        // (the profile's "update-scene" phase from here: the scene's movement, apart from the sound objects before it)
        PORT_GAME_PROGRESS(PORT_GAME_PHASE_UPDATE_SCENE);
        PORT_LOGIC_COARSE(PORT_LS_U_SCENE);
        mSceneController->updateScene();
    }

    if (isResetProcessing) {
        mSceneController->updateSceneDuringResetProcessing();
    }
}

void GameSystem::calcAnim() {
#ifdef TARGET_PC
    PORT_LOGIC_COARSE(PORT_LS_PH_CALC);
    // GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp): the game's audio thread may run on core 1 meanwhile (also through the front-end
    // wait below); the animation, effect and view passes do not drive JAudio (the sound starts and stops are in update())
    PORT_AUDIO_WINDOW(1);
    port_turn_prof_lap(PORT_TP_OTHER);
    port_gx_drawdone_settle(2);
    port_turn_prof_lap(PORT_TP_FEWAIT);  // (GXV_TURN_PROF: the deferred draw-done wait)
#endif
    mSceneController->calcAnimScene();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->calcAnim();
    }
#ifdef TARGET_PC
    PORT_AUDIO_WINDOW(0);
#endif
}
