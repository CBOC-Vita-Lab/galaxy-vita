#include "Game/Scene/GameScene.hpp"
#include "Game/Player/MarioAccess.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/AudioLib/AudSceneMgr.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Map/SleepControllerHolder.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/MapObj/StarPieceDirector.hpp"
#include "Game/NPC/EventDirector.hpp"
#include "Game/NPC/NPCFunction.hpp"
#include "Game/NPC/TalkDirector.hpp"
#include "Game/Scene/GameSceneFunction.hpp"
#include "Game/Scene/GameScenePauseControl.hpp"
#include "Game/Scene/GameSceneScenarioOpeningCameraState.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/CometRetryButton.hpp"
#include "Game/Screen/GamePauseSequence.hpp"
#include "Game/Screen/GameStageClearSequence.hpp"
#include "Game/Screen/LensFlare.hpp"
#include "Game/Screen/MoviePlayingSequence.hpp"
#include "Game/Screen/OdhConverter.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/System/GalaxyMapController.hpp"
#include "Game/System/GalaxyMoveArgument.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/Camera/CameraDirector.hpp"
#ifdef TARGET_PC
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "port/tour_scenario.h"
#include "port/mario_time.h"  // (mariolag31: CameraDelta)
#include "port/gt71.h"  // (gt71: exact game-thread overhead cuts, one switch GXV_GT71)
#include "Game/MP/MpFlow.hpp"  // MP3: multiplayer scene hooks (decomp/src/Game/MP)
#include "Game/MP/MpDemo.hpp"  // MP3 demo (SMG_MP_DEMO)
#include "Game/MP/MpMemProbe.hpp"  // memprobe
#include "port/diagnostics.h"
#include "port/hot_knob.h"  // (PETARI_VIEW_PLAYER)
#include "port/xskip.h"  // (xskip27)
#include "port/lockstep.h"  // (fmt32)
extern "C" int port_xskip_cinema_blank(void);  // (Screen/CinemaFrame.cpp)
#include "Game/LiveActor/LiveActor.hpp"  // (boss23, the boss tour at the end of this file)
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/GravityUtil.hpp"
#include "Game/Map/HitInfo.hpp"  // (warp36: Triangle::getFaceNormal, port_hrw_player36)
#include "Game/Map/CollisionParts.hpp"  // (hold36: has the ground under him moved lately, port_hrw_player36)
#include "Game/LiveActor/Binder.hpp"  // (ground38: the ground polygon of what he rides, port_hrw_player36)
#include "port/hrw36.h"  // (hold36: HoldGround)
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/JMapIdInfo.hpp"  // (routes25: PETARI_STAGE "Galaxy:n#id", PETARI_SURVEY)
#include "Game/Util/SequenceUtil.hpp"
#include <cmath>
#include <cstring>
extern "C" void port_log_summary(const char* fmt, ...);
extern "C" void port_log_flush(void);  // (the Vita log is written in batches; a scripted end must be on the card before whatever stops the process)
extern "C" int port_turn_skipped(void);  // (platform/src/port/perf.cpp)
extern "C" int port_turn_skipped_raw(void);
extern "C" int port_gx_drawdone_defer_mode(void);
extern "C" int port_hrw_mode(void);  // (platform/src/gx/vgx_galaxy.cpp, GXV_HRW)
extern "C" void port_hrw_fresh_draw(void);
extern "C" void port_hrw_fresh_finish(void);  // (MainLoopFramework.cpp)
// The view pass of the turns without a picture, left out (PETARI_SKIP_VIEWCALC or GXV_DRAWDONE_DEFER=3): made at the start of the
// next turn that draws, after the deferred draw-done wait, from the same state (no update() has run since the last calcAnim), with
// the same lists and view matrix as the left-out pass. drop: the scene is going away.
static bool sViewPending = false;
// GXV_PAR_VIEW (par13; decomp/src/Game/LiveActor/ModelManager.cpp): the view pass's matrix arithmetic is recorded between begin and
// end and computed at end on the game thread and a worker; end returns when all of it is done. Off: both do nothing.
extern "C" void port_par_view_begin(void);
extern "C" void port_par_view_end(void);
extern "C" int port_view_pending(void) { return sViewPending ? 1 : 0; }
// (hrw28) whether this configuration leaves the view pass to the next picture's flush in EVERY turn (calcAnim below: GXV_DRAWDONE_DEFER=4, or GXV_PAR_DL_VIEW with the player's pass): then the second picture
// of a pair finds the actors' view matrices made for the FIRST picture's camera and pose, and the significance set (PortHrwSet.cpp) must not draw them (Mario's own view pass is made every turn).
// PETARI_VIEW_PLAYER (forkB, 6 Nov; after Producdevity's fork, ee487a3; default 1): when the pass is left out of a turn, the player's
// draw buffer still has its pass in that turn (GameScene::calcAnim) and the flush makes the rest. MarioActor::calcViewAndEntry is
// not only matrices: decideShadowMode, calcFogLighting (a fade stepped once a call), the footprints and the blur model advance once
// a turn in the original; left out with the rest, they advanced once a picture (slower fades with frame skipping or
// GXV_DRAWDONE_DEFER=4). Its pass runs only where it may write its buffers without waiting: the last picture's draw-done wait made
// (always with GXV_DRAWDONE_DEFER <= 2, whose wait comes before calcAnim); otherwise (modes 3 and 4 with the front end still on the
// last picture) the flush makes the whole pass as before. 0: the whole pass is left to the flush, as before.
// sPlayerViewDone: the player's pass was made in the last calcAnim (the flush makes the rest only).
static bool sPlayerViewDone = false;
extern "C" int port_gx_drawdone_pending(void);
static bool portViewPlayerOn() {
    return PORT_HOT_KNOB(int, [] { const char* e = getenv("PETARI_VIEW_PLAYER"); return e && *e == '0' ? 0 : 1; }()) != 0;  // (GXV_HOT_KNOBS)
}
extern "C" int port_par_dl_defer_view(void);  // (platform/src/port/perf.cpp)
extern "C" int port_hrw_view_deferred_cfg(void) {
    return port_gx_drawdone_defer_mode() == 4 || (portViewPlayerOn() && port_par_dl_defer_view()) ? 1 : 0;
}
// GXV_PAR_DL (gamedraw23; ModelManager.cpp): the material lists overlap the flush's view pass only when that pass leaves the player out
// (its pass was made in calc-anim, or first in the flush: MarioActor::calcViewAndEntry uses GD and the guest's prohibit, updateLightDL).
extern "C" int port_view_pending_except_player(void) { return sViewPending && sPlayerViewDone ? 1 : 0; }
// GXV_PAR_DL_VIEW (default 1, acts only with GXV_PAR_DL): every turn leaves its view pass (but the player's, where it may be made here) to
// the flush, as a turn without a picture does (the same state: no update() runs before the flush), so that the material lists have it to
// overlap at 60 pictures a second too. Draw-done modes 0-3 (mode 4 leaves it to the flush anyway); with PETARI_VIEW_PLAYER only.
extern "C" int port_par_dl_defer_view(void);  // (platform/src/port/perf.cpp)
// GXV_PAR_DL (gamedraw23, round 2): the pending pass includes the player's (draw-done modes 3 and 4 while the front end was still on the
// last picture at calc-anim, Settings' "Logic overlap: Max", the default: the player's pass could not be made there without waiting). The
// player's view pass is the one part of the pass that uses the material path's shared state (MarioActor::calcFogLighting ->
// updateLightDL: GD's current list, the guest's prohibit; MarioActor::calcViewSearchLight and its hand models' J3DModel::calc write
// j3dSys), so it may not run beside the material lists. Split: the flush makes the rest now ("except the player", overlapped), and the
// player's part after the join (port_view_flush_player_last, GameSystem::frameLoop). The player's pass reads only its own models'
// joints and base matrices, collision and its own state, and no other group's pass reads what it writes (its models, shadow / reflection
// / blur models, light and eyelid lists; j3dSys's view matrix is put back by MR::loadViewMtx), so where it falls among the groups changes
// nothing; made last, its j3dSys and GD writes are the last ones, as in the serial pass (no later group writes either).
// 1: split; 0: nothing pending, already made, or PETARI_VIEW_PLAYER=0.
static bool sPlayerLast = false;
extern "C" int port_view_split_player_last(void) {
    if (!sViewPending || sPlayerViewDone || !portViewPlayerOn()) {
        return 0;
    }
    sPlayerViewDone = true;  // (port_view_flush makes the rest: executeCalcViewAndEntryListExceptPlayer)
    sPlayerLast = true;
    return 1;
}
// After the join: the player's part of a split pass. Not recorded for GXV_PAR_VIEW (its async batch of the rest is outstanding until the
// draw's retire; a new recording would retire it here): every model of it computes its matrices at once, the same arithmetic.
extern "C" void port_view_flush_player_last(void) {
    if (!sPlayerLast) {
        return;
    }
    sPlayerLast = false;
    SceneFunction::executeCalcViewAndEntryListPlayer();
}
// (GXV_PAR_DL's log: why a drawn turn had nothing to overlap) 0: no pass pending; 1: pending with the player's included and
// PETARI_VIEW_PLAYER=0; 2: pending without the player's
extern "C" int port_view_pending_state(void) {
    return !sViewPending ? 0 : sPlayerViewDone ? 2 : (portViewPlayerOn() ? 3 : 1);
}
extern "C" void port_view_flush(int drop) {
    if (drop) {
        sPlayerLast = false;  // (GXV_PAR_DL round 2: the scene is going)
    }
    if (!sViewPending) {
        return;
    }
    sViewPending = false;
    const bool playerDone = sPlayerViewDone;
    sPlayerViewDone = false;
    if (!drop) {
        port_par_view_begin();
        if (playerDone) {
            SceneFunction::executeCalcViewAndEntryListExceptPlayer();
        } else {
            SceneFunction::executeCalcViewAndEntryList();
        }
        port_par_view_end();
    }
}
// PETARI_PAUSE_FREEZE (pauseFreezeStep below): the renderer's picture-size generation (platform/src/port/perf.cpp), and whether
// this is a handheld (platform/src/gx/vgx_galaxy.cpp).
extern "C" u32 port_render_size_gen(void);
extern "C" int port_handheld(void);
// GXV_DRAW_PROF (draw7; platform/src/port/perf.cpp): GameScene::draw's stages timed when asked for.
extern "C" int port_draw_prof_on(void);
extern "C" int port_bglayer_mode(void);           // (bglayer25: DrawBufferHolder.cpp)
extern "C" int port_bglayer_scene_picture(int disallow);  // (SceneFunction.cpp: the census)
extern "C" void port_draw_prof_stage(int stage, int64_t ns);
#include "port/turn_prof.h"  // (GXV_TURN_PROF, gamepar)
// GXV_TURN_PROF=2 / GXV_DRAW_PROF=2 (gamecpu23): a part of draw() timed (platform/src/port/perf.cpp port_draw_part_*: one load when off)
#define PORT_DPART(part, call)          \
    do {                                \
        port_draw_part_begin(part);     \
        call;                           \
        port_draw_part_end(part);       \
    } while (0)
// boss23 (8 Nov): the boss tour (per-leg warps of PETARI_STAGE, "boss:" log lines) and the boss-fight cuts; at the end of this file.
static void portBossSceneBegin();
static u32 sHrw36Turn = 0;   // (hold36: game turns, port_flow_beat; port_hrw_player36's clock)
static u32 sHrw36Scene = 0;  // (hold36: game scenes begun, GameScene::init)
static void portBossLegSet(const char* pStage, const char* pSpec);
static void portBossTurn(bool playing);
// flow23 (8 Nov): PETARI_FLOW, the normal galaxy entry (star select, then the stage) for the console; "flow:" log lines. End of this file.
static void portFlowSceneBegin();
static bool portFlowActive();
static void portFlowTurn(bool action, bool playing, bool starter);
// routes25 (10 Nov): PETARI_STAGE "Galaxy:n#id[/zone]" (the game's own Mario start id) and PETARI_SURVEY (the stage's start points); end of this file.
static void portSurveyTurn(u32 gen, bool playing);
extern "C" void port_domemap_turn(int inAction);  // (prog32a: MapObj/AstroDomeGalaxySelector.cpp; PETARI_DOMEMAP_OPEN / _PICK_LOG)
static bool portSurveyBusy();
static void portStartOverrideSet(bool on);
static u32 portScriptSceneBegin();                      // (route24: PETARI_SCRIPT_ANCHOR=play, end of this file)
static void portScriptTurn(u32 gen, bool playing);
static void portScriptSceneEnd(u32 gen);
// fastboot32 (13 Nov): PETARI_BOOT, from the logos straight into a galaxy with save file 1 loaded (no title, file select, hub). End of this file.
static void portBootTurn(bool playing);
extern "C" void port_boot_play_began(void);  // perf.cpp: ends the Vita's loading screen (no title on this path)
static bool portTourEnter(const char* entry, int listIndex);
static int sPortTourIndex = 0;  // PETARI_STAGE: the next entry of the list (was a static of GameScene::update)
#if defined(__vita__) && PORT_LOGGING
extern "C" u32 port_workload_anim_begin(void);
extern "C" void port_workload_anim_end(u32 token);
extern "C" void port_workload_view_entry_begin(void);
extern "C" void port_workload_view_entry_end(void);
#endif
#include "Game/Camera/CameraHolder.hpp"
#include "Game/Camera/CameraManEvent.hpp"
#include "Game/Camera/CameraManGame.hpp"
#include "Game/Camera/CameraManPause.hpp"
#include "Game/Camera/CameraManSubjective.hpp"
#include "Game/Camera/CameraParamChunk.hpp"
#include "Game/Camera/CameraParamChunkID.hpp"
#else
#define PORT_DPART(part, call) call
#define port_draw_part_begin(part)
#define port_draw_part_end(part)
#endif
#include "Game/System/StarPointerOnOffController.hpp"
#include "Game/Camera/CameraPoseParam.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/JMapIdInfo.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#ifdef TARGET_PC
#include "Game/Effect/EffectSystem.hpp"
#include "Game/Screen/CaptureScreenDirector.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/JParticle/JPAEmitterManager.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#endif

namespace {
    CometRetryButton* getCometRetryButton() {
        return MR::getSceneObj< CometRetryButton >(SceneObj_CometRetryButton);
    }
};  // namespace

namespace NrvGameScene {
    NEW_NERVE(GameSceneScenarioOpeningCamera, GameScene, ScenarioOpeningCamera);
    NEW_NERVE(GameSceneScenarioStarter, GameScene, ScenarioStarter);
    NEW_NERVE(GameSceneAction, GameScene, SceneAction);
    NEW_NERVE(GameScenePauseMenu, GameScene, PauseMenu);
    NEW_NERVE(GameScenePowerStarGet, GameScene, PowerStarGet);
    NEW_NERVE(GameSceneGrandStarGet, GameScene, GrandStarGet);
    NEW_NERVE(GameSceneGameOver, GameScene, GameOver);
    NEW_NERVE(GameSceneCometRetryAfterMiss, GameScene, CometRetryAfterMiss);
    NEW_NERVE(GameSceneSaveAfterGameOver, GameScene, SaveAfterGameOver);
    NEW_NERVE(GameScenePlayMovie, GameScene, PlayMovie);
    NEW_NERVE(GameSceneTimeUp, GameScene, TimeUp);
    NEW_NERVE(GameSceneGalaxyMap, GameScene, GalaxyMap);
    NEW_NERVE(GameSceneStaffRoll, GameScene, StaffRoll);
};  // namespace NrvGameScene

#ifdef TARGET_PC
// GXV_DRAWPIPE (dpengine; SceneFunction.cpp port_drawpipe_decide): the GameScene that is up, and the flags that pick the branches
// of its draw, read at the hand-over. They are what GameScene::draw / draw3D / draw2D test (3D on, a movie, the nerve, the mirror,
// an ODH capture, the pause pictures), plus "a demo is active". 0: pScene is not the GameScene (another scene class).
namespace {
    const GameScene* sPortPipeGameScene = nullptr;
}
extern "C" int port_drawpipe_scene_flags(const void* pScene, u32* pFlags) {
    const GameScene* scene = sPortPipeGameScene;
    if (scene == nullptr || scene != pScene) {
        return 0;
    }
    *pFlags = scene->portPipeFlags();
    return 1;
}
u32 GameScene::portPipeFlags() const {
    u32 flags = 0;
    flags |= mDraw3D ? 1u : 0u;
    flags |= isPlayMovie() ? 2u : 0u;
    flags |= isNerve(GET_NERVE(GameScene, GameSceneAction)) ? 4u : 0u;
    flags |= isNerve(GET_NERVE(GameScene, GameScenePauseMenu)) ? 8u : 0u;
    flags |= isDrawMirror() ? 0x10u : 0u;
    flags |= (_29 && MR::isRequestedCaptureOdhImage()) ? 0x20u : 0u;
    flags |= MR::isDemoActive() ? 0x40u : 0u;
    flags |= mPauseFreezePictures != 0 ? 0x80u : 0u;
    return flags;
}
#endif

GameScene::GameScene()
    : Scene("GameScene"), _14(0), mScenarioCamera(nullptr), mPauseCtrl(nullptr), mPauseSeq(nullptr), mStageClearSeq(nullptr), mDraw3D(true), _29(1) {
#ifdef TARGET_PC
    sPortPipeGameScene = this;
    mPauseFreezePictures = 0;
    mPauseFreezeSizeGen = 0;
    mPortScriptGen = 0;
#endif
}

#ifdef TARGET_PC
extern "C" void port_hrw_scene_forget(const void* scene);  // (crash1114, below: the fresh player's scene pointer)
#endif
GameScene::~GameScene() {
#ifdef TARGET_PC
    MpGame::onGameSceneEnd();  // MP3: puppets / world / race layout forget this scene (no-op unless a session set them up)
    if (sPortPipeGameScene == this) {
        sPortPipeGameScene = nullptr;
    }
    port_hrw_scene_forget(this);  // (crash1114: the late partial draw must never reach a scene that is gone)
    portScriptSceneEnd(mPortScriptGen);  // (route24: the input script's clock stops with its scene)
#endif
    MR::destroySceneMessage();
    NPCFunction::deleteNPCData();
    MR::onStarPointerSceneOut();
}

void GameScene::init() {
#ifdef TARGET_PC
    portBossSceneBegin();  // (boss23: a new stage, its bosses are noted again as they are placed)
    ++sHrw36Scene;  // (hold36: a ground remembered in the scene before is not this scene's)
    portFlowSceneBegin();  // (flow23: PETARI_FLOW / PETARI_FLOW_LOG see a new scene)
    port_xskip_scene_begin();  // (xskip27: a new story of a stage entry)
    mPortScriptGen = portScriptSceneBegin();  // (route24: the input script's clock restarts with each new scene)
#endif
    SceneFunction::createHioBasicNode(this);
    SceneFunction::startStageFileLoad();
    MR::requestChangeArchivePlayer(MR::isPlayerLuigi() == false);
    initNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera));
    SceneFunction::initForNameObj();
    SceneFunction::initForLiveActor();
    initEffect();
    MR::createSceneObj(SceneObj_CameraContext);
    MR::createSceneObj(SceneObj_NameObjGroup);
    MR::createSceneObj(SceneObj_PlanetGravityManager);
    MR::createSceneObj(SceneObj_MarioHolder);
    MR::createSceneObj(SceneObj_AudCameraWatcher);
    MR::createSceneObj(SceneObj_AudEffectDirector);
    MR::createSceneObj(SceneObj_AudBgmConductor);
    MR::createSceneObj(SceneObj_ResourceShare);
    MR::createSceneObj(SceneObj_EventSequencer);
    MR::createSceneObj(SceneObj_ScenePlayingResult);
    MR::createSceneObj(SceneObj_FurDrawManager);
    MR::createSceneObj(SceneObj_PlacementStateChecker);
    MR::createScreenAlphaSceneObj(0, 1.0f);
    MR::createSceneObj(SceneObj_GroupCheckManager);
    MR::createSceneObj(SceneObj_CinemaFrame);
    MR::createSceneObj(SceneObj_PlanetMapCreator);

    if (MR::isStageEpilogueDemo()) {
        MR::createSceneObj(SceneObj_StaffRoll);
    }

    if (MR::isStageEpilogueDemo() || GameSequenceFunction::isNeedMoviePlayerForStorySequenceEvent()) {
        MR::createMoviePlayingSequence();
    }
#ifdef TARGET_PC
    // movie44, TEST ONLY (PETARI_MOVIE set; default unset: nothing changes): every stage gets the movie player, so the key works in
    // any galaxy. Before, it did nothing where the game makes none (the Observatory outside the story's movie events: console
    // session 39 played no movie). createSceneObj makes it once, so a stage that has one already is not changed.
    else if (getenv("PETARI_MOVIE") != nullptr && getenv("PETARI_MOVIE")[0] != 0) {
        MR::createMoviePlayingSequence();
        port_log_summary("movie-test: PETARI_MOVIE=%s: a movie player was made for this stage", getenv("PETARI_MOVIE"));
    }
#endif

    SceneFunction::waitDoneStageFileLoad();
    MR::waitEndChangeArchivePlayer();
    SceneFunction::startActorFileLoadCommon();
    MR::suspendAsyncExecuteThread("\x83\x56\x81\x5b\x83\x93\x8f\x89\x8a\xfa\x89\xbb");

    if (!MR::isScenarioDecided()) {
        MR::receiveAllRequestedFile();
    } else {
        SceneFunction::initAfterScenarioSelected();
        NPCFunction::createNPCData();
        SceneFunction::startActorFileLoadScenario();
        MR::createSceneObj(SceneObj_EventDirector);
        MR::initSceneMessage();
        MR::createSceneObj(SceneObj_CameraDirector);
        MR::createSceneObj(SceneObj_GameSceneLayoutHolder);
        MR::createSceneObj(SceneObj_StarPieceDirector);
        MR::createSceneObj(SceneObj_SceneWipeHolder);
        MR::createSceneObj(SceneObj_NamePosHolder);
        MR::createSceneObj(SceneObj_InformationObserver);

        if (MR::isGalaxyAnyCometAppearInCurrentStage()) {
            MR::createSceneObj(SceneObj_CometRetryButton);
        }

        LightFunction::initLightData();
        initSequences();
        SceneFunction::startActorPlacement();
#ifdef TARGET_PC
        MpGame::onGameSceneInitAfterPlacement();  // MP3: puppets, race layout (only in a session; marks the scene alive)
#endif
        GameSceneFunction::loadScenarioWaveData();
        MR::createStarPiece();
        MR::completeCameraParameters();
        MR::initStarPointerGameScene();
        MR::initEventSystemAfterPlacement();
        MR::endInitLiveActorSystemInfo();
        MR::setInitializeStateAfterPlacement();
        MR::callMethodAllSceneNameObj(&NameObj::initAfterPlacement);
        SleepControlFunc::initSyncSleepController();

        while (!GameSceneFunction::isLoadDoneScenarioWaveData()) {
#ifdef TARGET_PC
            // Let interrupts in (see MR::waitEndChangeArchivePlayer).
            port_host_sleep_ns(200000);
            port_irq_poll();
#endif
        }
    }
}

void GameScene::start() {
    AudWrap::getSceneMgr()->startScene();
    MpMemProbe::log("scene start");

    if (MR::isGlobalTimerEnd()) {
        MR::forceCloseWipeCircle();
    } else if (!MR::hasRetryGalaxySequence()) {
        startStagePlayFirst();
    } else {
        startStagePlayRetry();
    }
}

#ifdef TARGET_PC
// PETARI_STAGE: enter the galaxy of one list entry "Galaxy[:scenario][#id[/zone]][@where]" (the body GameScene::update had inline; fastboot32
// calls it from the end of the logos too). listIndex: the entry's number in the list, for the log.
static bool portTourEnter(const char* entry, int listIndex) {
    static char name[64];
    snprintf(name, sizeof(name), "%s", entry);
    if (char* comma = strchr(name, ',')) {
        *comma = 0;
    }
    // boss23: "Galaxy:scenario@<where>" also moves the player in that leg (see portBossLegSet at the end of the file):
    // @boss[:up[:side]] near the stage's boss, @x/y/z a world position; +a / +appear then start the fight.
    char* at = strchr(name, '@');
    if (at) {
        *at = 0;
    }
    // routes25: "Galaxy:scenario#id[/zone]": the Mario start entry ("MarioNo" id, in the zone) the game's own restart uses
    // (requestChangeStageAfterMiss passes such an id); the listing of a stage's ids: PETARI_SURVEY=1.
    s32 startId = -1;
    s32 startZone = 0;
    if (char* hash = strchr(name, '#')) {
        *hash = 0;
        s32 a = -1;
        s32 b = 0;
        const int n = sscanf(hash + 1, "%d/%d", &a, &b);
        if (n >= 1 && a >= 0) {
            startId = a;
            startZone = n >= 2 ? b : 0;
        }
    }
    s32 scenario = 1;
    if (char* colon = strchr(name, ':')) {
        *colon = 0;
        if (!port::TourScenarioNumber(colon + 1, scenario)) {
            port_log_summary("tour: invalid scenario number for '%s'; no transition requested", name);
            return false;
        }
    }
    const GalaxyStatusAccessor access = MR::makeGalaxyStatusAccessor(name);
    if (!access.mScenarioData || !access.mScenarioData->getScenarioDataIter(scenario).isValid()) {
        port_log_summary("tour: ignoring unknown stage or scenario '%s:%d'; no transition requested", name, (int)scenario);
        return false;
    }
    portBossLegSet(at ? name : nullptr, at ? at + 1 : nullptr);
    OSReport("port: jumping to %s scenario %d\n", name, (int)scenario);
    port_log_summary("tour: jumping to %s scenario %d (%d of the list)", name, (int)scenario, listIndex);
    // tour183 (test harness only): enter the way the star select does (ScenarioSelectScene::trySetCurrentScenarioNo). A hidden star is
    // played in the scenario where it is placed (MR::getPlacedHiddenStarScenarioNo: the normal scenario whose power stars include it), with
    // the hidden one as the selected scenario; every other entry is its own selected scenario. The tour used to load a hidden star's own
    // number with no selected scenario (-1), which the game never does: BattleShipGalaxy:6, FactoryGalaxy:6 and PhantomGalaxy:6 have no
    // Mario start in that scenario's layers, and the load read through a null start (the Vita walk's crashes of 15 Nov).
    s32 placed = scenario;
    if (access.isHiddenStar(scenario)) {
        const s32 p = MR::getPlacedHiddenStarScenarioNo(name, scenario);
        port_log_summary("tour: %s scenario %d is a hidden star: %s", name, (int)scenario,
                         p > 0 ? "loaded as the star select does, in the scenario where it is placed" : "no scenario places it (loaded as asked)");
        if (p > 0) {
            placed = p;
            port_log_summary("tour: hidden star %s:%d -> scenario %d, selected scenario %d", name, (int)scenario, (int)placed, (int)scenario);
        }
    }
    portStartOverrideSet(startId >= 0);
    const JMapIdInfo startInfo(startId >= 0 ? startId : MR::getInitializeStartIdInfo()._0, startId >= 0 ? startZone : MR::getInitializeStartIdInfo().mZoneID);
    if (startId >= 0) {
        port_log_summary("tour: start id %d zone %d asked for %s scenario %d", (int)startId, (int)startZone, name, (int)scenario);
    }
    GalaxyMoveArgument move(0, name, placed, &startInfo);  // (MR::requestChangeStageInGameMoving's argument, plus the selected scenario)
    move._C = scenario;
    GameSequenceFunction::requestGalaxyMove(move);
    return true;
}
#endif

#ifdef TARGET_PC
// (merge of 3.0 on c181: the multiplayer turn is ONE call to a non-inlined helper, so that GameScene::update keeps c181's control flow, and with it its PGO profile)
static void PortMpScenePreUpdate() __attribute__((noinline));
static void PortMpScenePreUpdate() {
    MpGame::onGameScenePreUpdate();
}
static void PortMpScenePostCalcAnim() __attribute__((noinline));
static void PortMpScenePostCalcAnim() {
    MpGame::onGameScenePostCalcAnim();
}
static void PortMpSceneUpdate(GameScene* pScene) __attribute__((noinline));
static void PortMpSceneUpdate(GameScene* pScene) {
    // MP3: the multiplayer turn (messages, puppets, world sync, flow); nothing when no session runs
    if (mp_active() || MpDemo::enabled()) {   // (MP3 demo: the puppet demo needs the scene's nerve too)
        s32 mpState = MP_SCENE_OTHER;
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneAction))) {
            mpState = MP_SCENE_ACTION;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera))) {
            mpState = MP_SCENE_OPENING;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter))) {
            mpState = MP_SCENE_STARTER;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameScenePauseMenu))) {
            mpState = MP_SCENE_PAUSE;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameScenePowerStarGet))) {
            mpState = MP_SCENE_STAR_GET;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGrandStarGet))) {
            mpState = MP_SCENE_GRAND_STAR_GET;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGameOver))) {
            mpState = MP_SCENE_GAME_OVER;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneCometRetryAfterMiss))) {
            mpState = MP_SCENE_COMET_RETRY;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneSaveAfterGameOver))) {
            mpState = MP_SCENE_SAVE_AFTER_GAME_OVER;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameScenePlayMovie))) {
            mpState = MP_SCENE_MOVIE;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGalaxyMap))) {
            mpState = MP_SCENE_GALAXY_MAP;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneStaffRoll))) {
            mpState = MP_SCENE_STAFF_ROLL;
        } else if (pScene->isNerve(GET_NERVE(GameScene, GameSceneTimeUp))) {
            mpState = MP_SCENE_TIME_UP;
        }
        MpFlowHooks::noteGameSceneState(mpState);
    }
    MpGame::onGameSceneUpdate();  // MP3: (returns at once without a session; cleans up once when one just ended)
}
#endif

void GameScene::update() {
#ifdef TARGET_PC
    {
        // What holding A would skip now (see port_skip_context).
        s32 skipKind = PORT_SKIP_NONE;
        if (isPlayMovie()) {
            skipKind = PORT_SKIP_MOVIE;
        } else if (isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera))) {
            skipKind = PORT_SKIP_OPENING;
        } else if (isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter))) {
            skipKind = PORT_SKIP_STARTER;
        } else if (MR::isDemoActive()) {
            skipKind = PORT_SKIP_DEMO;
        } else if (MR::isSystemTalking()) {
            skipKind = PORT_SKIP_TALK;  // most people's talk is no cutscene
        }
        port_skip_context(skipKind);
    }
#endif
    mPauseCtrl->updateNerve();
#ifdef TARGET_PC
    PortMpScenePreUpdate();   // wave8: drain fresh STATEs before puppets sample in updateNerve()
#endif
    updateNerve();
#ifdef TARGET_PC
    PortMpSceneUpdate(this);  // MP3: the multiplayer turn (messages, puppets, world sync, flow); nothing when no session runs
#endif
#ifdef TARGET_PC
    {
        // Debug: PETARI_STAGE="Galaxy[:scenario]" enters that galaxy once
        // ordinary gameplay has run for two seconds (for testing levels).
        // A list "A:1,B:2,C" with PETARI_STAGE_HOLD=<frames> is a tour: each
        // further galaxy is entered after that many frames of gameplay in the
        // one before (or four times as many frames of anything: a demo that
        // never ends does not end the tour).
        static int sGameplayFrames = 0;
        static int sAllFrames = 0;
        int& sIndex = sPortTourIndex;  // (fastboot32: PETARI_BOOT=1 takes the first entry at boot)
        static const char* stage = getenv("PETARI_STAGE");
        static const int hold = getenv("PETARI_STAGE_HOLD") ? atoi(getenv("PETARI_STAGE_HOLD")) : 0;
        if (stage && *stage && (sIndex == 0 || hold > 0) && !MR::isStageFileSelect() && !portFlowActive()) {  // (PETARI_FLOW wins)
            // the sIndex-th entry of the list
            const char* entry = stage;
            for (int i = 0; entry && i < sIndex; i++) {
                entry = strchr(entry, ',');
                if (entry) {
                    entry++;
                }
            }
            if (entry && *entry) {
                const bool playing = isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive();
                sAllFrames++;
                if (playing) {
                    sGameplayFrames++;
                }
                const int need = sIndex == 0 ? 120 : hold;
                if (((playing && sGameplayFrames >= need) || (sIndex > 0 && sAllFrames >= need * 4 && isNerve(GET_NERVE(GameScene, GameSceneAction)))) &&
                    !portSurveyBusy()) {  // (routes25: not while PETARI_SURVEY_HOP is still moving the player through the stage's start points)
                    sIndex++;
                    sGameplayFrames = 0;
                    sAllFrames = 0;
                    portTourEnter(entry, sIndex);  // (fastboot32: the entry's parsing and the request moved to a function, unchanged)
                }
            }
        }
    }
    portBossTurn(isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive());  // (boss23: leg warps, boss log, fight state)
    portFlowTurn(isNerve(GET_NERVE(GameScene, GameSceneAction)), isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive(),
                 isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter)));
    port_xskip_scene_turn(isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera)) ? 1 : isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter)) ? 2 : isNerve(GET_NERVE(GameScene, GameSceneAction)) ? 3 : 0,
                          isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive(), port_xskip_cinema_blank(), MR::getCurrentStageName(),
                          static_cast< int >(MR::getCurrentScenarioNo()));  // (xskip27: the markers of a skip; one story per stage entry)
    portScriptTurn(mPortScriptGen, isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive());  // (route24)
    portBootTurn(isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive());  // (fastboot32: PETARI_BOOT's "play began" line)
    portSurveyTurn(mPortScriptGen, isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive());  // (routes25: PETARI_SURVEY)
    port_domemap_turn(isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive() ? 1 : 0);  // (prog32a: PETARI_DOMEMAP_OPEN / _PICK_LOG)
    {
        // Debug: PETARI_WARP="<t>:<x>,<y>,<z>;..." puts the player at each
        // of those world positions once <t> seconds have passed since the
        // headless launcher booted the game (it sets PETARI_T0_MS).
        static int sWarpsDone = 0;
        static const char* warps = getenv("PETARI_WARP");
        // (read each time: the Vita sets it when its input script starts, which can be after this scene's first update)
        const char* t0 = warps && *warps ? getenv("PETARI_T0_MS") : nullptr;
        if (warps && *warps && t0 && isNerve(GET_NERVE(GameScene, GameSceneAction))) {
            f64 nowMs = port_host_time_ns() / 1000000.0 - atof(t0);
            s32 index = 0;
            for (const char* p = warps; p != nullptr; index++) {
                f32 t, x, y, z;
                if (sscanf(p, "%f:%f,%f,%f", &t, &x, &y, &z) != 4) {
                    break;
                }
                if (index >= sWarpsDone) {
                    if (nowMs < t * 1000.0) {
                        break;
                    }
                    OSReport("port: warping the player to %.0f %.0f %.0f\n", x, y, z);
                    port_log_summary("tour: warping the player to %.0f %.0f %.0f", x, y, z);
                    MR::setPlayerPos(TVec3f(x, y, z));
                    sWarpsDone = index + 1;
                }
                p = strchr(p, ';');
                if (p != nullptr) {
                    p++;
                }
            }
        }
    }
    {
        // Debug: PETARI_SWITCH="<t>:<id>;..." turns on global stage switch
        // <id> (1000 and up, as placements number them) <t> seconds after
        // boot (PETARI_T0_MS), e.g. to open a cage without its puzzle;
        // "<t>:<zone name>/<id>" turns on switch <id> (0-127) of that zone.
        static s32 sSwitchesDone = 0;
        static const char* switches = getenv("PETARI_SWITCH");
        static const char* t0 = getenv("PETARI_T0_MS");
        if (switches && *switches && t0 && isNerve(GET_NERVE(GameScene, GameSceneAction)) &&
            MR::isExistSceneObj(SceneObj_StageSwitchContainer)) {
            f64 nowMs = port_host_time_ns() / 1000000.0 - atof(t0);
            s32 index = 0;
            for (const char* p = switches; p != nullptr; index++) {
                f32 t;
                s32 id;
                char zone[64];
                bool inZone = sscanf(p, "%f:%63[^/;]/%d", &t, zone, &id) == 3;
                if (!inZone && sscanf(p, "%f:%d", &t, &id) != 2) {
                    break;
                }
                if (index >= sSwitchesDone) {
                    if (nowMs < t * 1000.0) {
                        break;
                    }
                    StageSwitchContainer* container = MR::getSceneObj< StageSwitchContainer >(SceneObj_StageSwitchContainer);
                    if (inZone) {
                        for (s32 z = 0; z < MR::getZoneNum(); z++) {
                            ZoneSwitch* sw = MR::isEqualString(MR::getZoneNameFromZoneId(z), zone) ? container->getZoneSwitches(z) : nullptr;
                            if (sw != nullptr && id >= 0 && id < 128) {
                                OSReport("port: turning on switch %d of %s\n", (int)id, zone);
                                sw->set(id, true);
                            }
                        }
                    } else if (id >= 1000 && id < 1128) {
                        OSReport("port: turning on switch %d\n", (int)id);
                        container->getGlobalSwitches()->set(id - 1000, true);
                    }
                    sSwitchesDone = index + 1;
                }
                p = strchr(p, ';');
                if (p != nullptr) {
                    p++;
                }
            }
        }
    }
    {
        // Debug: PETARI_POSLOG=1 logs the player's position once a second; =2 every frame, with the camera's position and whether the
        // camera and the player are in water (the water-surface crossings).
        static const char* posLogEnv = getenv("PETARI_POSLOG");
        static const bool posLog = posLogEnv != nullptr;
        static const s32 posEvery = posLogEnv != nullptr && atoi(posLogEnv) >= 2 ? 1 : 60;
        static s32 sPosFrames = 0;
        if (posLog && MR::isExistMario() && isNerve(GET_NERVE(GameScene, GameSceneAction)) && ++sPosFrames % posEvery == 0) {
            const TVec3f* p = MR::getPlayerPos();
            if (posEvery == 1) {
                const TVec3f cam = MR::getCamPos();
                OSReport("port: player at %.1f %.1f %.1f%s cam %.1f %.1f %.1f%s%s\n", p->x, p->y, p->z, MR::isOnGroundPlayer() ? " (on the ground)" : "", cam.x, cam.y, cam.z,
                         MR::isCameraInWater() ? " (camera in water)" : "", MR::isPlayerSwimming() ? " (swimming)" : "");
            } else {
                OSReport("port: player at %.1f %.1f %.1f%s\n", p->x, p->y, p->z, MR::isOnGroundPlayer() ? " (on the ground)" : "");
            }
        }
    }
    {
        // Debug: PETARI_STARBITS="<t>:<n>" gives the player n star bits <t>
        // seconds after boot (PETARI_T0_MS), to test shooting them.
        static bool sGiven = false;
        static const char* starBits = getenv("PETARI_STARBITS");
        static const char* t0 = getenv("PETARI_T0_MS");
        f32 t;
        s32 n;
        if (!sGiven && starBits && t0 && sscanf(starBits, "%f:%d", &t, &n) == 2 && isNerve(GET_NERVE(GameScene, GameSceneAction)) &&
            port_host_time_ns() / 1000000.0 - atof(t0) >= t * 1000.0) {
            sGiven = true;
            OSReport("port: giving %d star bits\n", (int)n);
            MR::addStarPiece(n);
        }
    }
    {
        // Debug: PETARI_SETEST="<t0>-<t1>:<name>:<volume>:<fx send>;..." plays
        // that level sound on the player each frame from t0 to t1 seconds
        // after boot (PETARI_T0_MS), like an actor near the player would.
        static const char* tests = getenv("PETARI_SETEST");
        static const char* t0 = getenv("PETARI_T0_MS");
        if (tests && *tests && t0 && isNerve(GET_NERVE(GameScene, GameSceneAction))) {
            f64 nowMs = port_host_time_ns() / 1000000.0 - atof(t0);
            for (const char* p = tests; p != nullptr;) {
                f32 from, to;
                s32 volume, fxSend;
                char name[64];
                if (sscanf(p, "%f-%f:%63[^:]:%d:%d", &from, &to, name, &volume, &fxSend) != 5) {
                    break;
                }
                if (nowMs >= from * 1000.0 && nowMs < to * 1000.0) {
                    MR::startLevelSound(MarioAccess::getPlayerActor(), name, volume, fxSend);
                }
                p = strchr(p, ';');
                if (p != nullptr) {
                    p++;
                }
            }
        }
    }
    {
        // Debug: PETARI_MOVIE=<type> (0 = PrologueA .. 6 = EndingB) plays that
        // movie after two seconds of gameplay in a stage with a movie player,
        // such as the castle garden or the observatory.
        static int sMovieFrames = 0;
        static bool sMovieStarted = false;
        // gt71 ENV (port/gt71.h): the environment scan (getenv) was made every turn; the variable is set before the game starts, so the first answer is kept.
        static const char* sMovieEnv = nullptr;
        static bool sMovieEnvRead = false;
        const char* movie;
        if (::port::gt71::On(::port::gt71::kEnv)) {
            if (!sMovieEnvRead) {
                sMovieEnv = getenv("PETARI_MOVIE");
                sMovieEnvRead = true;
            }
            movie = sMovieEnv;
        } else {
            movie = getenv("PETARI_MOVIE");
        }
        if (!sMovieStarted && movie && *movie && isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive() &&
            MR::isExistSceneObj(SceneObj_MoviePlayingSequenceHolder)) {
            if (++sMovieFrames >= 120) {
                sMovieStarted = true;
                // movie44: the type is checked (4 has no player: the game plays EpilogueB another way; a wrong number was a crash)
                // and the start is said in the basic log (OSReport does not reach a player build's log.txt).
                const int type = atoi(movie);
                const char* name = type >= 0 && type <= 6 ? MoviePlayingSequence::getMovieName(MoviePlayingSequence::MovieType(type)) : nullptr;
                if (name == nullptr) {
                    port_log_summary("movie-test: PETARI_MOVIE=%s is no movie (0 PrologueA, 1 PrologueB, 2 FinalBattle, 3 EpilogueA, 5 EndingA, 6 EndingB): nothing played", movie);
                } else {
                    port_log_summary("movie-test: starting movie %d (%s.thp), decode %s (GXV_THPHW)", type, name,
                                     getenv("GXV_THPHW") && atoi(getenv("GXV_THPHW")) != 0 ? "hardware" : "software");
                    OSReport("port: starting movie %d\n", type);
                    MR::startMovie(type);
                }
            }
        }
    }
#endif

    bool isTimeUp = MR::isGlobalTimerEnd() && !isNerve(GET_NERVE(GameScene, GameSceneTimeUp)) && MR::isGreaterEqualStep(this, 2);

    if (isTimeUp) {
        setNerve(GET_NERVE(GameScene, GameSceneTimeUp));
    }
}

#ifdef TARGET_PC
// GXV_LIGHT_DEDUP=2 (decomp/src/Game/Map/LightFunction.cpp): the whole scene's drawing is one scope in which a light load already in
// light memory is not sent again (level 1: only the draw-buffer passes, DrawBufferGroup.cpp).
extern "C" int port_light_scope_enter(int level);
extern "C" void port_light_scope_exit(int opened);
#endif

#ifdef TARGET_PC
namespace {
    // The scene the last picture was drawn by (GXV_HRW): the partial draw of the pair's second picture is a part of its draw, made from a turn without one. Cleared at the start of every
    // picture (port_hrw_scene_arm) and set again by its draw.
    // crash1114 (14 Nov): "a scene change is always a drawn turn, so it is never stale" held only while the partial draw came BEFORE the undrawn turn's update. Since c142
    // (GXV_HRW_FRESH_LATE=1, the default) it comes AFTER that update (GameSystem::frameLoop: port_hrw_fresh_early, update(), port_hrw_fresh_draw), and the update is where the scene
    // controller destroys a scene. When a stage change began in the undrawn turn of a pair that had a fresh player pending, the partial draw then walked the destroyed scene's
    // Mario (its draw buffers, display lists and the GameScene itself, all in freed memory): the C heap was damaged and the game died a moment later. Console c153 / c157 / c163:
    // 4 deaths in about a dozen trips in and out of Rosalina's library (the fresh player is on there; the Observatory's pairs have no room for it), dumps with a job list or a
    // free list full of another owner's data, J3DShape::drawFast with a display list in freed memory; 0 in 5 trips with GXV_HRW=0 and with GXV_HRW_WARP_FRESH=0.
    // Now the pointer is also cleared when a scene is destroyed (port_hrw_scene_drop, GameSystemSceneController::exeDestroyScene, before anything of the scene goes) and by the
    // scene's own destructor; port_hrw_fresh_scene then returns 0 and the pair's second picture goes out as the plain re-projection.
    const GameScene* sPortHrwScene = nullptr;
    unsigned sPortHrwSceneDrops = 0;  // (crash1114: scenes destroyed while the pointer still named them: the "hrw: fresh" line's "scene gone")
}  // namespace

extern "C" void port_hrw_scene_arm(void) {
    sPortHrwScene = nullptr;
}
// crash1114: a scene is going (exeDestroyScene) or gone (~GameScene).
extern "C" void port_hrw_scene_drop(void) {
    if (sPortHrwScene != nullptr) {
        ++sPortHrwSceneDrops;
    }
    sPortHrwScene = nullptr;
}
extern "C" void port_hrw_scene_forget(const void* scene) {
    if (sPortHrwScene == scene) {
        ++sPortHrwSceneDrops;
        sPortHrwScene = nullptr;
    }
}
extern "C" unsigned port_hrw_scene_drops(void) {
    return sPortHrwSceneDrops;
}

// warp32 PATCH21: partial draws for which the player's view pass was [0] made first (it was pending), [1] already made (or not deferred), [2] pending and left (GXV_HRW_FRESH_VIEW=0)
// PATCH22: [3] of [0], made at the turn's start without a wait (GXV_HRW_FRESH_LATE, draw-done mode 4); [4] late draws that still found the first picture's draw-done pending
static unsigned sPortHrwFreshView[5] = {0, 0, 0, 0, 0};
extern "C" void port_hrw_fresh_view_take(unsigned* out) {
    for (int i = 0; i < 5; ++i) {
        out[i] = sPortHrwFreshView[i];
        sPortHrwFreshView[i] = 0;
    }
}
static bool portHrwFreshViewEnv() {
    static const bool on = [] { const char* e = getenv("GXV_HRW_FRESH_VIEW"); return !(e && *e == '0'); }();
    return on;
}
// warp32 PATCH22 (GXV_HRW_FRESH_LATE; GameSystem::frameLoop, the undrawn turn's start, vgx_galaxy.cpp port_hrw_fresh_early): the player's view pass the partial draw needs, made
// BEFORE the turn's update (the second picture's camera and pose), while the draws themselves follow AFTER the update (port_hrw_fresh_draw), when the front end has all but always
// finished the first picture. c135 on the console: the partial draw cost 0.7-0.9 ms of work and 0-2.3 ms (mean of a 5 s window; single pairs 5-21 ms) of WAITING for the front
// end's finish of the first picture, in this pass (PATCH21) and, had the pass not waited, in the partial draw's own GXSetDrawDone.
// No wait here in draw-done mode 4: this is the one pass since the first picture's flush, so it writes the half of each double buffer that picture does not read: the situation of
// every mode 4 flush (GameSystem: "what follows writes the half the front end does not read"), hence its flush scope. A SECOND pass before that picture is finished would write
// the half it reads: the late draw makes sure it is finished first (port_hrw_fresh_scene below; the wait is counted, [4]). Modes below 4: as PATCH21 (the write waits if it must).
static bool sPortHrwFreshEarly = false;   // the pass of this turn's partial draw is made (not counted again by portHrwFresh)
static bool sPortHrwFreshNoWait = false;  // ... and without a wait: the first picture must be finished before anything else writes the player's buffers
extern "C" void port_gx_flush_scope(int on);
extern "C" void port_gx_view_write(void);
extern "C" void port_hrw_fresh_scene_view(void) {
    sPortHrwFreshEarly = false;
    if (!sPortHrwScene || !sViewPending || sPlayerViewDone || !portViewPlayerOn() || !portHrwFreshViewEnv()) {
        return;
    }
    const bool noWait = port_gx_drawdone_defer_mode() == 4;
    if (noWait) {
        port_gx_flush_scope(1);
    }
    port_par_view_begin();
    SceneFunction::executeCalcViewAndEntryListPlayer();
    port_par_view_end();
    if (noWait) {
        port_gx_flush_scope(0);
        sPortHrwFreshNoWait = true;
        ++sPortHrwFreshView[3];
    }
    sPlayerViewDone = true;
    sPortHrwFreshEarly = true;
    ++sPortHrwFreshView[0];
}

// mariolag31 (GXV_HRW_MARIOTIME, port/mario_time.h): with GXV_HRW_FRESH_LATE the fresh draws come after the undrawn turn's update, which has moved the camera on by a turn,
// while Mario's view pass (above) was made before it, from the pose's own turn. The camera of that turn is taken here (vgx_galaxy.cpp port_hrw_fresh_early, before the
// update) and, in modes 1 and 2, loaded for the draws instead of the camera after the update: his draws, the warp (V_B) and his pose are then all the same turn's. The
// census compares the two in any mode. Game thread.
extern "C" int port_hrw_mariotime(void);
extern "C" void port_hrw_mariotime_cam(int moved, double deg, double units, int snap);
static bool sPortFreshCamOk = false;
static TPos3f sPortFreshView;
static TProj3f sPortFreshProj;
extern "C" void port_hrw_fresh_camera_take(void) {
    sPortFreshCamOk = false;
    if (!sPortHrwScene || !MR::isExistSceneObj(SceneObj_CameraContext)) {
        return;
    }
    sPortFreshView = MR::getCameraViewMtx();
    sPortFreshProj = MR::getCameraProjectionMtx();
    sPortFreshCamOk = true;
}
// The camera for the fresh draws: the pose's turn's in modes 1 / 2 when it was taken; else the camera now (as before). The census line counts both.
static void portHrwFreshLoadCamera() {
    bool snap = false;
    if (sPortFreshCamOk) {
        sPortFreshCamOk = false;
        const TPos3f& now = MR::getCameraViewMtx();
        float a[12], b[12];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                a[r * 4 + c] = sPortFreshView.mMtx[r][c];
                b[r * 4 + c] = now.mMtx[r][c];
            }
        }
        bool same = true;
        for (int i = 0; i < 12 && same; i++) {
            same = a[i] == b[i];
        }
        double deg = 0.0, units = 0.0;
        if (!same) {
            port::mariotime::CameraDelta(a, b, &deg, &units);
        }
        snap = port_hrw_mariotime() >= 1;
        port_hrw_mariotime_cam(same ? 0 : 1, deg, units, snap ? 1 : 0);
    }
    if (snap) {
        PSMTXCopy(sPortFreshView, j3dSys.mViewMtx);
        GXSetProjection(sPortFreshProj, (GXProjectionType)0);
    } else {
        MR::loadViewMtx();
        MR::loadProjectionMtx();
    }
}

extern "C" int port_hrw_fresh_scene(unsigned pairId, int pointer) {
    if (sPortHrwFreshNoWait) {  // (PATCH22: the first picture is finished before the partial draw and before any later pass of the player's buffers; all but always it already is)
        sPortHrwFreshNoWait = false;
        if (port_gx_drawdone_pending()) {
            ++sPortHrwFreshView[4];
        }
        port_gx_view_write();
    }
    const int n = sPortHrwScene ? sPortHrwScene->portHrwFresh(pairId, pointer != 0) : 0;
    sPortFreshCamOk = false;  // (mariolag31: the camera taken is this pair's only, used or not)
    return n;
}

// GXV_HRW stage d: Mario drawn again with the camera the turn starts with. The state the picture's draw3D sets before its passes is set the same way (drawInit, lights, view, projection, viewport,
// update flags); then Mario's draws, tagged with their owner, and the end marker that hands the stream over as a partial draw. `pointer`: the star pointer (reserved, GXV_HRW_POINTER, not built).
extern "C" int port_hrw_set_draw(unsigned pairId, int verify);
extern "C" void port_hrw_pointer_draw(void);
int GameScene::portHrwFresh(u32 pairId, bool pointer) const {
    if (!mDraw3D || isPlayMovie() || mPauseFreezePictures != 0) {
        return 0;
    }
    // warp32 PATCH21 (GXV_HRW_FRESH_VIEW, default 1): with "Logic overlap: Max" (GXV_DRAWDONE_DEFER=4, or 3 / GXV_PAR_DL_VIEW) the drawn turn's calc-anim leaves the view pass to the
    // next picture's flush, and the player's own pass is made there only when the front end has already finished the picture (calcAnim below). When it was not, his draw matrices
    // here were still the FIRST picture's (its camera and its pose): the partial draw repeated the first picture's Mario exactly (c134 flip-books). The pass that calc-anim owed is
    // made now, from the state the drawn turn's update and calc-anim left (the camera of the second picture, the pose a picture of this turn would show). Once: sPlayerViewDone tells
    // a flush before another calc-anim that it is made; this turn's own calc-anim decides again for its turn. The write waits for the front end where it must (J3DModelX /
    // J3DModel: port_gx_view_write_from, counted in "drawdone-defer: N waits ... outside the drawn turn's flush").
    if (sPortHrwFreshEarly) {
        sPortHrwFreshEarly = false;  // (PATCH22: made at the turn's start and counted there)
    } else if (sViewPending && !sPlayerViewDone && portViewPlayerOn()) {
        const bool freshView = portHrwFreshViewEnv();
        if (freshView) {
            port_par_view_begin();
            SceneFunction::executeCalcViewAndEntryListPlayer();
            port_par_view_end();
            sPlayerViewDone = true;
            ++sPortHrwFreshView[0];
        } else {
            ++sPortHrwFreshView[2];
        }
    } else {
        ++sPortHrwFreshView[1];
    }
    const int lightScope = port_light_scope_enter(2);
    MR::drawInit();
    LightFunction::initLightRegisterAll();
    portHrwFreshLoadCamera();  // (mariolag31: MR::loadViewMtx + MR::loadProjectionMtx, or the pose's turn's camera with GXV_HRW_MARIOTIME)
    MR::setDefaultViewportAndScissor();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
    int n = SceneFunction::executeDrawFreshPlayer();
    n += port_hrw_set_draw(pairId, port_hrw_mode() == 4 ? 1 : 0);  // (hrw28: the significance set, after Mario, inside its budget; tagging ends here)
    if (pointer) {
        port_hrw_pointer_draw();  // (hrw28, GXV_HRW_POINTER)
        n += 1;
    }
    MR::reinitGX();
    port_light_scope_exit(lightScope);
    // hrw29: no draw, no partial draw: the end marker hands the stream over as a job, and with nothing in it the pair's ask was sent twice (once on that job, once plain: the second replayed the pair again, or was
    // dropped as "not presentable") whenever the front end read the marker before the game thread had taken the ask back.
    if (n > 0) port_gx_marker(PORT_GX_MARK_FRESH_END | (pairId << 8));
    return n;
}
#endif

void GameScene::draw() const {
#ifdef TARGET_PC
    const int lightScope = port_light_scope_enter(2);
    sPortHrwScene = this;
    if (port_hrw_mode() == 4) {
        port_hrw_fresh_draw();  // (verify-fresh: the pair's partial draw, made inside the picture with its own camera, to be compared with it)
    }
#endif
    MR::drawInit();
    LightFunction::initLightRegisterAll();
    drawOdhCapture();
    CategoryList::execute(MR::DrawType_MiiFaceIcon);
    CategoryList::execute(MR::DrawType_MiiFaceNew);
#ifdef TARGET_PC
    // (PETARI_PAUSE_FREEZE: behind the pause menu, the 3D picture of the first pictures again instead of the whole scene)
    const s32 freeze = pauseFreezeStep();
    if (freeze == 2) {
        drawPauseFreeze();
    } else if (port_draw_prof_on()) {
        // (GXV_DRAW_PROF: the stages timed)
        const int64_t t0 = port_host_time_ns();
        PORT_DPART(PORT_DP_MIRROR, drawMirror());
        const int64_t t1 = port_host_time_ns();
        PORT_DPART(PORT_DP_3D, draw3D());
        port_draw_prof_stage(3, t1 - t0);
        port_draw_prof_stage(4, port_host_time_ns() - t1);
        if (freeze == 1) {
            capturePauseFreeze();
        }
    } else {
        PORT_DPART(PORT_DP_MIRROR, drawMirror());
        PORT_DPART(PORT_DP_3D, draw3D());
        if (freeze == 1) {
            capturePauseFreeze();
        }
    }
#else
    drawMirror();
    draw3D();
#endif
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_HUD_BEGIN);
    if (port_draw_prof_on()) {
        const int64_t t0 = port_host_time_ns();
        PORT_DPART(PORT_DP_2D, draw2D());
        port_draw_prof_stage(5, port_host_time_ns() - t0);
    } else {
        PORT_DPART(PORT_DP_2D, draw2D());
    }
#else
    draw2D();
#endif
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_MARK_HUD_END);
#endif
    MR::reinitGX();
#ifdef TARGET_PC
    port_light_scope_exit(lightScope);
#endif
}

#ifdef TARGET_PC
// PETARI_PAUSE_FREEZE=1 (the Vita's default, vita/vgx-game/entry.cpp; 0 = off, the original drawing): while the pause menu is up
// the game's logic stands still (exePauseMenu moves only the menu's layouts), yet every picture drew the whole 3D scene again,
// with its effects, bloom and indirect passes: the same picture, at ~88 ms of GPU a picture (the 3D scene and its particle
// sprites; ~10 FPS measured on 4 Nov). Now the first pictures of the pause draw the scene as always and copy it, at the end of
// draw3D, into the game's own screen-capture texture (CaptureScreenDirector, the one the indirect effects, the camera cover and
// the system wipe use); the pictures after that draw that copy as one screen-sized rectangle (as CameraCover does) and skip
// the mirror and draw3D. The 2D (the menu, its dimmed backdrop, 2D effects) is drawn as always on top. The menu closing puts
// the nerve back to GameSceneAction: the next picture draws the scene again (calcAnim kept running every turn, so its lists
// are current).
//   The copy is taken again whenever the renderer changes its picture size (the automatic quality steps in quiet moments,
// such as this one: port_render_size_gen), and the scene is drawn normally whenever someone else asked for the screen
// texture (a capture timing: the system wipe of "quit", the screen preserver), so the copy never stands in for theirs.
//   Not identical in one respect: effects that move in a pause (the "PAUSE_IGNORE" 3D group, few) stand still in the copy.
// PETARI_PAUSE_FREEZE=2 freezes only while no such effect is alive (exact, but then not in every stage).
namespace {
    // the pictures drawn and copied before the copy stands in (the first after the menu opened already shows the final state:
    // GXV_DL_LAZY / GXV_ANIM_LOD / the deferred view pass are flushed before every picture; the second is margin)
    const s32 cPauseFreezeCapturePictures = 2;

    int pauseFreezeMode() {
        static const int mode = [] {
            const char* e = getenv("PETARI_PAUSE_FREEZE");
            return e && *e ? atoi(e) : 0;
        }();
        return mode;
    }

    // The 3D emitters that keep moving while the game is paused (ParticleCalcExecutor::movementIgnorePause3D: group 1).
    s32 pauseIgnoreEmitters3D() {
        if (!MR::isExistSceneObj(SceneObj_EffectSystem)) {
            return 0;
        }
        const JPAEmitterManager* pManager = MR::getEffectSystem()->mEmitterManager;
        if (pManager == nullptr || pManager->gidMax < 2) {
            return 0;
        }
        return static_cast< s32 >(pManager->pEmtrUseList[1].getNumLinks());
    }
}  // namespace

// 0: draw as always; 1: draw as always and copy the 3D picture; 2: draw the copy instead of the 3D scene.
s32 GameScene::pauseFreezeStep() const {
    const s32 was = mPauseFreezePictures;
    mPauseFreezePictures = 0;
    const int mode = pauseFreezeMode();
    if (mode <= 0 || !port_handheld() || !mDraw3D || isPlayMovie() || !isNerve(GET_NERVE(GameScene, GameScenePauseMenu))) {
        return 0;
    }
    const GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    const CaptureScreenDirector* pDirector = pSystem != nullptr && pSystem->mObjHolder != nullptr ? pSystem->mObjHolder->mCaptureScreenDirector : nullptr;
    if (pDirector == nullptr || pDirector->getCurrentTiming() != nullptr) {
        return 0;  // (the texture is someone else's now: their capture must not be painted over, nor stand in for the scene)
    }
    const s32 moving = pauseIgnoreEmitters3D();
    if (mode == 2 && moving != 0) {
        return 0;
    }
    s32 n = was + 1;
    const u32 sizeGen = port_render_size_gen();
    if (n > cPauseFreezeCapturePictures && sizeGen != mPauseFreezeSizeGen) {
        n = 1;  // (a new picture size: the copy has the old one; draw and copy again)
    }
    if (n > cPauseFreezeCapturePictures + 1) {
        n = cPauseFreezeCapturePictures + 1;  // (frozen; the count stays put)
    }
    mPauseFreezePictures = n;
    if (n <= cPauseFreezeCapturePictures) {
        mPauseFreezeSizeGen = sizeGen;
        return 1;
    }
    if (was == cPauseFreezeCapturePictures) {
        port_log_summary("pause: the 3D picture behind the menu is drawn from a copy (PETARI_PAUSE_FREEZE=%d; %d effects that move in a pause held still)",
                         mode, (int)moving);
    }
    return 2;
}

// The 3D picture, as draw3D left it, into the screen-capture texture (as the camera cover and the system wipe capture it).
void GameScene::capturePauseFreeze() const {
    SingletonHolder< GameSystem >::get()->mObjHolder->mCaptureScreenDirector->capture();
}

// The copy over the whole picture (no blending, no depth), then the state draw3D leaves for draw2D: the default viewport, the
// camera's view and projection, the player's light and a cleared Z buffer. (The copy's alpha is not written: the EFB keeps its
// cleared alpha, which nothing in the 2D reads.)
void GameScene::drawPauseFreeze() const {
    MR::setDefaultViewportAndScissor();
    JUTTexture texture(MR::getScreenResTIMG(), 0);
    ImageEffectLocalUtil::setupDrawTexture();
    ImageEffectLocalUtil::drawTexture(&texture, 1, 0, 255, ImageEffectLocalUtil::TexDrawType_0);
    GXSetDither(GX_TRUE);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    MR::loadLightPlayer();
    MR::clearZBuffer();
}
#endif

void GameScene::calcAnim() {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 animToken = port_workload_anim_begin();
#endif
    if (isPlayMovie()) {
        SceneFunction::executeCalcAnimListOnPlayingMovie();
    } else {
        SceneFunction::executeCalcAnimList();
    }

    if (!isNerve(GET_NERVE(GameScene, GameSceneTimeUp))) {
        CategoryList::execute(MR::CalcAnimType_AnimParticleIgnorePause);
    }
#ifdef TARGET_PC
    port_turn_prof_lap(PORT_TP_PTCL);  // (GXV_TURN_PROF)
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_end(animToken);
#endif

#ifdef TARGET_PC
    PortMpScenePostCalcAnim();   // wave8: capture/send local MP pose after executeCalcAnimList() (before skipped-turn early return)
    // A turn without a picture (frame skipping on the handhelds) needs no view matrices or draw-list entries: the next turn that
    // draws computes them again from the same state (they are not read by the game's logic).
    const int ddMode = port_gx_drawdone_defer_mode();
    if (port_turn_skipped() || (ddMode == 3 && port_turn_skipped_raw()) || ddMode == 4 ||
        (portViewPlayerOn() && port_par_dl_defer_view())) {
        sViewPending = true;  // (made by port_view_flush before the next picture)
        // PETARI_VIEW_PLAYER (above): the player's pass in every turn, where it needs no wait for the front end
        sPlayerViewDone = portViewPlayerOn() && (ddMode < 3 || !port_gx_drawdone_pending());
        if (sPlayerViewDone) {
            port_par_view_begin();  // (GXV_PAR_VIEW)
            SceneFunction::executeCalcViewAndEntryListPlayer();
            port_par_view_end();
        }
        return;
    }
    sViewPending = false;
    sPlayerViewDone = false;
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_view_entry_begin();
#endif
#ifdef TARGET_PC
    port_par_view_begin();  // (GXV_PAR_VIEW)
#endif
    SceneFunction::executeCalcViewAndEntryList();
#ifdef TARGET_PC
    port_par_view_end();
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_view_entry_end();
#endif
}

void GameScene::notifyEndScenarioStarter() {
    setNerve(GET_NERVE(GameScene, GameSceneAction));
}

void GameScene::requestPlayMovieDemo() {
    setNerve(GET_NERVE(GameScene, GameScenePlayMovie));
}

void GameScene::requestStartGameOverDemo() {
    if (!isNerve(GET_NERVE(GameScene, GameSceneGameOver))) {
        setNerve(GET_NERVE(GameScene, GameSceneGameOver));
    }
}

void GameScene::requestEndGameOverDemo() {
#ifdef TARGET_PC
    // MP3: no game over in a multiplayer session (it would reset to the title and save): restart like a miss
    if (mp_active() && MpFlow::blockGameOver()) {
        mp_log("flow lives clamp (game over turned into a miss)");
        requestEndMissDemo();
        return;
    }
#endif
    setNerve(GET_NERVE(GameScene, GameSceneSaveAfterGameOver));
}

void GameScene::requestEndMissDemo() {
    if (MR::isExistSceneObj(SceneObj_CometRetryButton)) {
        setNerve(GET_NERVE(GameScene, GameSceneCometRetryAfterMiss));
    } else {
        MR::requestChangeStageAfterMiss();
    }
}

void GameScene::requestPowerStarGetDemo() {
    setNerve(GET_NERVE(GameScene, GameScenePowerStarGet));
}

void GameScene::requestGrandStarGetDemo() {
    setNerve(GET_NERVE(GameScene, GameSceneGrandStarGet));
}

void GameScene::setNerveAfterPauseMenu() {
    setNerve(GET_NERVE(GameScene, GameSceneAction));
}

bool GameScene::isExecScenarioOpeningCamera() const {
    return isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera));
}

bool GameScene::isExecScenarioStarter() const {
    return isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter));
}

bool GameScene::isExecStageClearDemo() const {
    return isNerve(GET_NERVE(GameScene, GameScenePowerStarGet)) || isNerve(GET_NERVE(GameScene, GameSceneGrandStarGet));
}

void GameScene::exeScenarioOpeningCamera() {
    mScenarioCamera->update();
    SceneFunction::movementStopSceneController();
    SceneFunction::executeMovementList();

    if (mScenarioCamera->isDone()) {
        if (!MR::isBeginScenarioStarter()) {
            MR::activateDefaultGameLayout();
            MR::tryFrameToScreenCinemaFrame();
            MR::stopSubBGM(0);
            MR::stopStageBGM(0);
            MR::startStageBGMFromStageName("Game", MR::getCurrentStageName(), MR::getCurrentScenarioNo());
            setNerve(GET_NERVE(GameScene, GameSceneAction));
        } else {
            setNerve(GET_NERVE(GameScene, GameSceneScenarioStarter));
        }
    }
}

void GameScene::exeCometRetryAfterMiss() {
    CometRetryButton* pCometRetryButton = ::getCometRetryButton();

    if (MR::isFirstStep(this)) {
        ::getCometRetryButton()->appear();
        MR::forceOpenWipeCircle();
    }

    pCometRetryButton->movement();
}

void GameScene::exeSaveAfterGameOver() {
    if (MR::isFirstStep(this)) {
        MR::incPlayerGameOverNum();
        GameSequenceFunction::startGameDataSaveSequence(true, true);
        MR::startStarPointerModePauseMenu(this);
    }

    if (!GameSequenceFunction::isActiveSaveDataHandleSequence()) {
        MR::endStarPointerMode(this);
        MR::requestChangeSceneAfterGameOver();
    }
}

void GameScene::exePlayMovie() {
    if (!MR::isActiveMoviePlayer() && !MR::isMoviePlayingOnSequence()) {
        setNerve(GET_NERVE(GameScene, GameSceneAction));
        SceneFunction::movementStopSceneController();
        SceneFunction::executeMovementList();
    } else {
        SceneFunction::executeMovementListOnPlayingMovie();
    }
}

void GameScene::exeGalaxyMap() {
    if (MR::isFirstStep(this)) {
        MR::startStarPointerModePauseMenu(this);
    }

    if (!MR::isActiveGalaxyMapLayout()) {
        MR::endStarPointerMode(this);
        setNerve(GET_NERVE(GameScene, GameSceneAction));
    } else {
        CategoryList::execute(MR::MovementType_LayoutOnPause);
    }
}

void GameScene::initSequences() {
    GameStageClearSequence* pStageClearSeq = new GameStageClearSequence();
    pStageClearSeq->initWithoutIter();
    mStageClearSeq = pStageClearSeq;

    GamePauseSequence* pPauseSeq = new GamePauseSequence();
    pPauseSeq->initWithoutIter();
    mPauseSeq = pPauseSeq;

    mScenarioCamera = new GameSceneScenarioOpeningCameraState();
    mPauseCtrl = new GameScenePauseControl(this);
    mPauseCtrl->registerNervePauseMenu(GET_NERVE(GameScene, GameScenePauseMenu));

    mPauseSeq->initWindowMenu(MR::Functor(mPauseCtrl, &GameScenePauseControl::requestPauseMenuOff));
}

void GameScene::initEffect() {
    if (MR::isEqualStageName("CosmosGardenGalaxy")) {
        SceneFunction::initEffectSystem(5120, 384);
    } else if (MR::isEqualStageName("KoopaBattleVs1Galaxy")) {
        SceneFunction::initEffectSystem(6144, 512);
    } else if (MR::isEqualStageName("AstroGalaxy")) {
        SceneFunction::initEffectSystem(3072, 512);
    } else {
        SceneFunction::initEffectSystem(3072, 256);
    }
}

void GameScene::drawMirror() const {
    if (isDrawMirror()) {
        PSMTXCopy(MR::getMirrorCameraViewMtx(), j3dSys.mViewMtx);
        MR::loadProjectionMtx();
        CategoryList::entryDrawBufferMirror();
        GXSetAlphaUpdate(GX_FALSE);
        GXSetColorUpdate(GX_TRUE);
        CategoryList::drawOpa(MR::DrawBufferType_MirrorMapObj);
        CategoryList::drawXlu(MR::DrawBufferType_MirrorMapObj);
        CategoryList::execute(MR::DrawType_CaptureScreenIndirect);
        MR::clearZBuffer();
        GXColor c = {0, 0, 255, 0};
        MR::fillScreen(c);
    }
}

#ifdef TARGET_PC
namespace {
    // Debug (PETARI_CAMLOG=1): which game camera is active, where it watches
    // and where the VR eye is, relative to the player and his gravity, twice
    // a second and whenever the camera changes.  "blocked" means the map lies
    // between the player and the eye: the eye is behind or inside geometry
    // (seen from inside, the map's faces are not hit, so the eye's own test
    // for the cutaway misses that).
    void logCameraForVr(const TVec3f& player, const TVec3f& up, const TVec3f& watch, const TVec3f* eye, u32 flags) {
        static s32 sFrames = 0;
        static const CameraParamChunk* sLastChunk = nullptr;
        CameraDirector* director = MR::getCameraDirector();
        CameraMan* man = director->getCurrentCameraMan();
        const char* kind = "other";
        const CameraParamChunk* chunk = nullptr;
        if (man == director->mCameraManGame) {
            kind = "game";
            chunk = director->mCameraManGame->mChunk;
        } else if (man == director->mCameraManEvent) {
            kind = "event";
            chunk = director->mCameraManEvent->mChunk;
        } else if (man == director->mCameraManPause) {
            kind = "pause";
        } else if (man == director->mCameraManSubjective) {
            kind = "subjective";
        }
        if (++sFrames % 30 != 0 && chunk == sLastChunk) {
            return;
        }
        sLastChunk = chunk;
        char id[96] = "-";
        if (chunk != nullptr && chunk->mParamChunkID != nullptr && chunk->mParamChunkID->mName != nullptr) {
            // Event chunk names are Shift-JIS: escape the bytes.
            char* out = id;
            for (const u8* p = (const u8*)chunk->mParamChunkID->mName; *p && out < id + sizeof(id) - 5; p++) {
                if (*p >= 0x20 && *p < 0x7F) {
                    *out++ = (char)*p;
                } else {
                    out += snprintf(out, 5, "\\x%02x", *p);
                }
            }
            *out = 0;
        }
        const char* type = chunk != nullptr ? director->mHolder->getNameStrOf(chunk->mCameraTypeIndex) : "-";
        TVec3f w = watch - player;
        f32 wUp = w.dot(up);
        TVec3f wSide = w - up * wUp;
        char eyeText[160] = "no VR eye";
        if (eye != nullptr) {
            TVec3f e = *eye - player;
            f32 eUp = e.dot(up);
            TVec3f eSide = e - up * eUp;
            TVec3f hit;
            bool blocked = MR::getFirstPolyOnLineToMap(&hit, nullptr, player, e);
            snprintf(eyeText, sizeof(eyeText), "eye %.0f %.0f %.0f: %.0f up %.0f out%s", eye->x, eye->y, eye->z, eUp, eSide.length(),
                     blocked ? "" : ", clear");
            if (blocked) {
                size_t n = strlen(eyeText);
                snprintf(eyeText + n, sizeof(eyeText) - n, ", blocked %.0f from the player", hit.distance(player));
            }
        }
        TVec3f cam = MR::getCamPos();
        OSReport("camlog: %s %s %s, watch %.0f up %.0f out, camera %.0f %.0f %.0f, player %.0f %.0f %.0f up %.2f %.2f %.2f%s, diorama on %s, %s\n", kind,
                 type, id, wUp, wSide.length(), cam.x, cam.y, cam.z, player.x, player.y, player.z, up.x, up.y, up.z,
                 (flags & PORT_GX_CAMERA_GROUNDED) ? " on the ground" : "", (flags & PORT_GX_CAMERA_CENTRE_PLAYER) ? "the player" : "the watched point",
                 eyeText);
    }
}  // namespace
#endif

#ifdef TARGET_PC
// geopass23 (GXV_GEOPASS): the draw phase from here on, as a GX stream marker (written only with the switch on: vgx_galaxy.cpp)
#define PORT_GEO_PHASE(n) port_gx_marker(PORT_GX_MARK_PHASE | ((n) << 8))
#else
#define PORT_GEO_PHASE(n) \
    do {                  \
    } while (0)
#endif

// inline
bool GameScene::isPlayMovie() const {
    return MR::isActiveMoviePlayer() || MR::isMoviePlayingOnSequence() || isNerve(GET_NERVE(GameScene, GameScenePlayMovie));
}

void GameScene::draw3D() const {
    if (!mDraw3D) {
        return;
    }

    if (isPlayMovie()) {
        return;
    }

#ifdef TARGET_PC
    // GXV_DRAW_PROF=2 (flushcut): draw3D in five parts (pre: sky clear and the camera handed on; opa: the opaque lists with the shadows; xlu; fx: the
    // effect lists and what captures the screen; post: the image effects to the end), a clock read at each of the five joints
    int64_t d3t = port_draw_prof_on() ? port_host_time_ns() : 0;
#define PORT_D3_LAP(stage)                                          \
    do {                                                            \
        if (d3t) {                                                  \
            const int64_t d3now = port_host_time_ns();              \
            port_draw_prof_stage((stage), d3now - d3t);             \
            d3t = d3now;                                            \
        }                                                           \
    } while (0)
#else
#define PORT_D3_LAP(stage) \
    do {                   \
    } while (0)
#endif
    PORT_GEO_PHASE(0);
    CategoryList::execute(MR::DrawType_AstroDomeSkyClear);
    MR::loadViewMtx();
#ifdef TARGET_PC
    if (port_bglayer_mode() != 0) {
        // (bglayer25: GXV_BGLAYER=2, the census of the picture's lists and camera; nothing is changed)
        port_bglayer_scene_picture(0);
    }
    {
        // Hand the camera to the VR renderer.
        const CameraPoseParam* pose = MR::getCameraDirector()->mPoseParam1;
        float cam[PORT_GX_CAMERA_WORDS];
        const TPos3f& view = MR::getCameraViewMtx();
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                cam[r * 4 + c] = view.mMtx[r][c];
            }
        }
        const TVec3f* v[4] = {&pose->mPos, &pose->mWatchPos, &pose->mUpVec, &pose->mWatchUpVec};
        for (int i = 0; i < 4; i++) {
            cam[12 + i * 3] = v[i]->x;
            cam[13 + i * 3] = v[i]->y;
            cam[14 + i * 3] = v[i]->z;
        }
        cam[24] = pose->mFovy;
        u32 flags = 0;
        s32 pointerMode = MR::getStarPointerModeForPort();
        // Choosing a galaxy in an observatory dome means pointing at and
        // dragging the galaxy spheres: shown on the flat screen, where the
        // pointer lands exactly where the player aims.
        bool domeSelect = pointerMode == StarPointerMode_SphereSelectorOnReaction || pointerMode == StarPointerMode_SphereSelectorFinger;
        // The first-person view looks out through Mario's eyes: also flat.
        // So are scripted transits (port_vr_transit_begin) while Mario is
        // still bound to the object carrying him: in the diorama he would
        // leave the view mid-flight, while the world turned with the gravity
        // of every planet he passed, and a pipe through a planet would put
        // the viewer inside it (the game camera pulls back to show it whole).
        bool transit = false;
        if (MR::isExistMario()) {
            const HitSensor* rush = MR::isPlayerInRush() ? MarioAccess::getPlayerActor()->_924 : nullptr;
            if (rush == nullptr) {
                port_vr_transit_end(port_vr_transit_host());
            } else {
                transit = rush->mHost == port_vr_transit_host();
            }
        }
        // A galaxy's opening shots and Mario's flight into it are camera
        // work too: shown on the screen, the diorama flew the whole world
        // around the player.
        bool arriving = isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera)) || isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter));
        if (!MR::isStageFileSelect() && !MR::isDemoActive() && !domeSelect && !MR::isFirstPersonCamera() && !transit && !arriving) {
            flags |= PORT_GX_CAMERA_DIORAMA;
        }
        if (pointerMode <= StarPointerMode_PictureBook || domeSelect) {
            flags |= PORT_GX_CAMERA_POINTER_UI;
        }
        cam[27] = cam[28] = cam[29] = 0.0f;
        cam[30] = cam[31] = cam[32] = 0.0f;
        if (MR::isExistMario()) {
            const TVec3f* player = MR::getPlayerCenterPos();
            cam[27] = player->x;
            cam[28] = player->y;
            cam[29] = player->z;
            // The diorama keeps this up: the camera's own idea of up
            // (its watch-up vector) flips during some camera modes.
            const TVec3f* gravity = MR::getPlayerGravity();
            cam[30] = -gravity->x;
            cam[31] = -gravity->y;
            cam[32] = -gravity->z;
            flags |= PORT_GX_CAMERA_PLAYER;
            if (MR::isOnGroundPlayer()) {
                flags |= PORT_GX_CAMERA_GROUNDED;
            }
            // The diorama centres Mario, except while the game camera shows
            // something else: the galaxy's opening shots, and event cameras
            // watching a point far from him (a door opening elsewhere).  An
            // event camera near him, like the one on the Dino Piranha after
            // each hit, still centres him: the diorama shows that anyway.
            static bool sShowingElsewhere = false;
            CameraDirector* director = MR::getCameraDirector();
            bool eventCamera = director->getCurrentCameraMan() == director->mCameraManEvent;
            f32 watchDistance = pose->mWatchPos.distance(*player);
            sShowingElsewhere = eventCamera && watchDistance > (sShowingElsewhere ? 1500.0f : 2500.0f);
            if (!sShowingElsewhere && !isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera))) {
                flags |= PORT_GX_CAMERA_CENTRE_PLAYER;
            }
            // Is level geometry between the headset and Mario?  The VR layer
            // then cuts it away.  The ray stops short of Mario's centre so the
            // ground he stands on does not count.
            f32 head[12] = {}, tanX, tanY;
            bool vrEye = port_vr_cull_view(head, &tanX, &tanY) != 0;
            TVec3f eye(head[3], head[7], head[11]);
            if (vrEye) {
                TVec3f toPlayer(player->x - eye.x, player->y - eye.y, player->z - eye.z);
                f32 dist = toPlayer.length();
                if (dist > 150.0f) {
                    toPlayer.scale((dist - 100.0f) / dist);
                    if (MR::getFirstPolyOnLineToMap(nullptr, nullptr, eye, toPlayer)) {
                        flags |= PORT_GX_CAMERA_OCCLUDED;
                    }
                }
            }
            static const bool sCamLog = getenv("PETARI_CAMLOG") != nullptr;
            if (sCamLog) {
                logCameraForVr(*player, TVec3f(-gravity->x, -gravity->y, -gravity->z), pose->mWatchPos, vrEye ? &eye : nullptr, flags);
            }
        }
        memcpy(&cam[25], &flags, 4);
        cam[26] = MR::getAspect();
        port_gx_camera(cam);
        // The presentation hints as they change, for the log.
        static s32 sLoggedHints = -1;
        s32 hints = (flags & (PORT_GX_CAMERA_DIORAMA | PORT_GX_CAMERA_POINTER_UI)) | (MR::isDemoActive() ? 0x10000 : 0);
        if (hints != sLoggedHints) {
            sLoggedHints = hints;
            port_log("vr hints: %s, pointer %s (mode %d)%s", (flags & PORT_GX_CAMERA_DIORAMA) ? "diorama" : "flat screen",
                     (flags & PORT_GX_CAMERA_POINTER_UI) ? "on menus" : "in the world", (int)pointerMode, MR::isDemoActive() ? ", cutscene" : "");
        }
    }
#endif
    MR::loadProjectionMtx();
    MR::setDefaultViewportAndScissor();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
    PORT_D3_LAP(15);
    CategoryList::drawOpa(MR::DrawBufferType_ClippedMapParts);
    CategoryList::execute(MR::DrawType_ClipArea);
    CategoryList::execute(MR::DrawType_FallOutFieldDraw);
    SceneFunction::executeDrawBufferListNormalOpaBeforeVolumeShadow();
    GXSetAlphaUpdate(GX_TRUE);
    PORT_GEO_PHASE(4);
    PORT_DPART(PORT_DP_SHADOW, PORT_DPART(PORT_DP_SHV, CategoryList::execute(MR::DrawType_ShadowVolume)));
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
    PORT_GEO_PHASE(5);
    SceneFunction::executeDrawBufferListNormalOpaBeforeSilhouette();
    CategoryList::execute(MR::DrawType_0x28);
    port_draw_part_begin(PORT_DP_SHADOW);
    PORT_GEO_PHASE(6);
    PORT_DPART(PORT_DP_SIL, SceneFunction::executeDrawSilhouetteAndFillShadow());
    PORT_DPART(PORT_DP_SHA, SceneFunction::executeDrawAlphaShadow());
    port_draw_part_end(PORT_DP_SHADOW);
    PORT_GEO_PHASE(7);
    CategoryList::execute(MR::DrawType_BrightSun);
    MR::setLensFlareDrawSyncToken();
    GXSetAlphaUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    SceneFunction::executeDrawBufferListNormalOpa();
    PORT_GEO_PHASE(9);
    SceneFunction::executeDrawListOpa();
    CategoryList::drawOpa(MR::DrawBufferType_UNK_0x18);
    PORT_D3_LAP(16);
    PORT_GEO_PHASE(10);
#ifdef TARGET_PC
    // hrw41 (GXV_HRW41_DEPTH / GXV_HRW41_BOXKEY, the pairs' depth mask): the OPAQUE scene's depth is complete here, the player included. The translucent draws that
    // follow may write depth in front of what is seen through them. Sub-kind 1 in bits 8-15; the vgx glue passes one of the game's two places
    // (GXV_HRW41_MASK_AT: default the other one, PORT_GX_MARK_SCENE_DEPTH after the whole 3D below; this one only with GXV_HRW41_MASK_AT=1).
    if (port_handheld()) port_gx_marker(PORT_GX_MARK_SCENE_DEPTH | (1u << 8));
#endif
    SceneFunction::executeDrawBufferListNormalXlu();
    SceneFunction::executeDrawListXlu();
    CategoryList::drawXlu(MR::DrawBufferType_UNK_0x18);
    PORT_D3_LAP(17);
    PORT_GEO_PHASE(11);
    PORT_DPART(PORT_DP_SHADOW, PORT_DPART(PORT_DP_SHS, CategoryList::execute(MR::DrawType_ShadowSurface)));
    PORT_GEO_PHASE(12);
    port_draw_part_begin(PORT_DP_EFFECT);
    CategoryList::execute(MR::DrawType_EffectDraw3D);
    CategoryList::execute(MR::DrawType_EffectDrawForBloomEffect);
    port_draw_part_end(PORT_DP_EFFECT);
    CategoryList::execute(MR::DrawType_CenterScreenBlur);
    CategoryList::execute(MR::DrawType_CaptureScreenIndirect);
    PORT_GEO_PHASE(13);
    SceneFunction::executeDrawAfterIndirect();
    CategoryList::execute(MR::DrawType_0x33);
    PORT_D3_LAP(18);
    MR::setStarPointerDrawSyncToken();
    MR::setTalkDirectorDrawSyncToken();
#ifdef TARGET_PC
    // The scene's depth, before the image effects: the VR renderer keeps it
    // for SpaceWarp (the frame ends with a Z clear).
    port_gx_marker(PORT_GX_MARK_SCENE_DEPTH);
#endif
    PORT_GEO_PHASE(14);
    SceneFunction::executeDrawImageEffect();
    GXSetDither(GX_TRUE);
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    PORT_DPART(PORT_DP_EFFECT, CategoryList::execute(MR::DrawType_EffectDrawAfterImageEffect));
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    MR::loadLightPlayer();
    CategoryList::execute(MR::DrawType_0x24);
    CategoryList::execute(MR::DrawType_0x3B);
    MR::clearZBuffer();
    PORT_D3_LAP(19);
}
#undef PORT_D3_LAP

void GameScene::draw2D() const {
    PORT_GEO_PHASE(15);
    if (isPlayMovie()) {
        SceneFunction::executeDrawList2DMovie();
    } else {
        SceneFunction::executeDrawList2DNormal();
    }

    CategoryList::execute(MR::DrawType_CameraCover);
    CategoryList::execute(MR::DrawType_LayoutOnPause);
    mPauseSeq->draw();
    MR::drawInitFor2DModel();
    CategoryList::drawOpa(MR::DrawBufferType_0x26);
    CategoryList::drawXlu(MR::DrawBufferType_0x26);
    CategoryList::execute(MR::DrawType_WipeLayout);
}

bool GameScene::isValidScenarioOpeningCamera() const {
    return MR::hasStartAnimCamera() != false;
}

void GameScene::drawOdhCapture() const {
    if (!_29) {
        return;
    }

    if (!MR::isRequestedCaptureOdhImage()) {
        return;
    }

    MR::setPortCaptureOdhImage();
    CategoryList::execute(MR::DrawType_MessageBoardCapture);
    MR::captureOdhImage();
    MR::reinitGX();
}

#ifdef TARGET_PC
// (hitch33 9, GXV_WIPE_OPEN_NOWIPE; platform/vgx/src/vgx_backend_gl.cpp) 1 when a stage load has just ended and the port wants the stage's first pictures made behind a closed
// screen. The three ways a stage begins WITHOUT an opening wipe (an opening camera, a scenario starter, "begin without wipe") showed those pictures: console job 2710, Peach's
// castle: 702 ms and 974 ms frames 4.5 s after the load, no opening hold. With it the black fade is closed and asked to open in 2 frames; WipeFade holds its first step until
// the port lets it go (GXV_WIPE_OPEN_HOLD), then it is open two turns later: what the stage shows from there on is unchanged.
// (hitch33 14) port_wipe_open_site: the same question with the place that asks (1 opening camera, 2 scenario starter, 3 without wipe, 5 a start with its own wipe:
// trace only), so the log names the path a stage took. The opening camera's own start (GameSceneScenarioOpeningCameraState::start) forces the fade open one turn
// later: it asks too (site 4), or the hold begun here was dropped at once (console job 2890, Honeyhive).
extern "C" int port_wipe_open_site(int site) __attribute__((weak));
static bool portOpenBehindBlack(int site) {
    if (!port_wipe_open_site || !port_wipe_open_site(site)) {
        return false;
    }
    MR::forceCloseWipeFade();
    MR::openWipeFade(2);
    return true;
}
#endif

void GameScene::startStagePlayFirst() {
    if (isValidScenarioOpeningCamera()) {
#ifdef TARGET_PC
        portOpenBehindBlack(1);
#endif
        setNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera));

        if (!MR::isBeginScenarioStarter()) {
            MR::stopSubBGM(0);
            MR::stopStageBGM(0);
            MR::startStageBGMFromStageName("Game", MR::getCurrentStageName(), MR::getCurrentScenarioNo());
        }
    } else if (MR::isBeginScenarioStarter()) {
        MR::forceToBlankCinemaFrame();
#ifdef TARGET_PC
        if (!portOpenBehindBlack(2))
#endif
        MR::forceOpenWipeFade();
        setNerve(GET_NERVE(GameScene, GameSceneScenarioStarter));
    } else {
#ifdef TARGET_PC
        if (port_wipe_open_site && !MR::isStageBeginWithoutWipe()) {
            port_wipe_open_site(5);  // (hitch33 14: trace only; the stage's own wipe asks port_wipe_open_hold)
        }
#endif
        if (MR::isStageBeginFadeWipe()) {
            MR::openWipeFade(180);
        } else if (MR::isStageBeginTitleWipe()) {
            MR::openWipeFade(30);
        } else if (MR::isStageBeginWithoutWipe()) {
#ifdef TARGET_PC
            if (!portOpenBehindBlack(3))
#endif
            MR::forceOpenWipeFade();
        } else {
            MR::openWipeCircle();
        }

        MR::stopSubBGM(0);
        MR::stopStageBGM(0);
        MR::startStageBGMFromStageName("Game", MR::getCurrentStageName(), MR::getCurrentScenarioNo());
        MR::executeOnWelcomeAndRetry();
        setNerve(GET_NERVE(GameScene, GameSceneAction));
    }
}

void GameScene::startStagePlayRetry() {
    MR::openWipeCircle();

    if (MR::isEqualStageName("SurfingLv1Galaxy") && MR::getPlayerRestartIdInfo()->_0 == 1) {
        MR::stopSubBGM(0);
        MR::stopStageBGM(0);
    } else if (MR::isEqualStageName("SurfingLv2Galaxy") && MR::getPlayerRestartIdInfo()->_0 == 1) {
        MR::stopSubBGM(0);
        MR::stopStageBGM(0);
    } else {
        MR::stopSubBGM(0);
        MR::stopStageBGM(0);
        MR::startStageBGMFromStageName("Game", MR::getCurrentStageName(), MR::getCurrentScenarioNo());
    }

    MR::executeOnWelcomeAndRetry();
    setNerve(GET_NERVE(GameScene, GameSceneAction));
}

bool GameScene::isPermitToPauseMenu() const {
    return !MR::isStageDisablePauseMenu() && isNerve(GET_NERVE(GameScene, GameSceneAction)) && !MR::isDemoActive() && !MR::isWipeActive() &&
           !MR::isWipeBlank() && !MR::isPlayerDead() && !MR::isPlayerDamaging();
}

void GameScene::requestShowGalaxyMap() {
    setNerve(GET_NERVE(GameScene, GameSceneGalaxyMap));
}

void GameScene::requestStaffRoll() {
    setNerve(GET_NERVE(GameScene, GameSceneStaffRoll));
}

bool GameScene::isDrawMirror() const {
    if (!MR::isExistMirrorCamera()) {
        return false;
    }

    if (isPlayMovie()) {
        return false;
    }

    return MR::isPlayerInAreaObj("MirrorArea");
}

void GameScene::stageClear() {
    if (MR::isFirstStep(this)) {
        if (isNerve(GET_NERVE(GameScene, GameScenePowerStarGet))) {
            mStageClearSeq->startPowerStarGetDemo();
        } else if (isNerve(GET_NERVE(GameScene, GameSceneGrandStarGet))) {
            mStageClearSeq->startGrandStarGetDemo();
        }
    }

    mStageClearSeq->movement();
    SceneFunction::movementStopSceneController();
    SceneFunction::executeMovementList();
}

void GameScene::exeStaffRoll() {
    MR::getSceneNameObjMovementController()->movement();
    CategoryList::execute(MR::MovementType_Layout);
    CategoryList::execute(MR::DrawType_LayoutDecoration);
    CategoryList::execute(MR::MovementType_WipeLayout);
}

void GameScene::exeTimeUp() {
    if (MR::isFirstStep(this)) {
        MR::startGlobalTimerTimeUp();
    }
}

void GameScene::exeGameOver() {
    SceneFunction::movementStopSceneController();
    SceneFunction::executeMovementList();
}

void GameScene::exeGrandStarGet() {
    stageClear();
}

void GameScene::exePowerStarGet() {
    stageClear();
}

void GameScene::exePauseMenu() {
    if (MR::isFirstStep(this)) {
    }

    mPauseSeq->movement();
    CategoryList::execute(MR::MovementType_LayoutOnPause);
}

void GameScene::exeSceneAction() {
    SceneFunction::movementStopSceneController();
    SceneFunction::executeMovementList();
}

void GameScene::exeScenarioStarter() {
    SceneFunction::movementStopSceneController();
    SceneFunction::executeMovementList();
}

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// boss23 (8 Nov): the boss tour and the boss-fight cuts.
//
// The boss actors of a stage are noted as they are placed (PlacementInfoOrdered.cpp: port_boss_kind / port_boss_note; the list below
// is by placement class name). With them:
//   PETARI_STAGE legs "Galaxy:scenario@<where>" (GameScene::update's tour): after PETARI_TOUR_WARP_S seconds (default 4) of play in
//     that leg (play: GameSceneAction with no demo; the counting starts when the leg's stage has been built), the player is put:
//       @boss[:up[:side]]  above the stage's boss, `up` units against the gravity at the boss (default PETARI_BOSS_UP, 600) and
//                          `side` units to one side (default PETARI_BOSS_SIDE, 1200): on its arena, usually its planet;
//       @x/y/z             at that world position (as PETARI_WARP, but per leg and timed from the leg's own start).
//     A restart of the stage in the leg (Mario lost a life) applies it again (3 times at most a leg).
//     A suffix then starts the fight 1.5 s later the way the arena's trigger would: +a turns on the boss's switch A (Dino Piranha,
//     Fiery Dino Piranha, King Kaliente, Bugaboom, Megaleg, Kamella, Major Burrows, Bouldergeist, Baron Brrr, Kingfin: their start
//     switch), +appear the appear switches of every noted actor (Bowser 3: the switch keepers). "+a" alone (no place) only switches.
//   PETARI_BOSS_LOG=1: "boss: placed ..." lines when a stage is built, and one "boss:" line every 5 s while the stage has a boss: its
//     state (alive / hidden / dead), distance to the player, nerve step, demo, the share of turns with the fight on (alive, shown,
//     within PETARI_BOSS_RANGE = 6000 of the player), the live particles and emitters (mean / peak), the boss particle cut and heat haze.
// The cuts (each 0 = the game as it was, the default; 1 = on in every mode; perf = on in Performance mode only):
//   PETARI_BOSS_PTCL (MINOR CHANGE): while a boss fight is on and more than PETARI_BOSS_PTCL_CAP (default 1200) particles are alive,
//     every emitter's emission rate is scaled by cap / live (never below PETARI_BOSS_PTCL_MIN, 0.4) for the turn
//     (JSystem/JParticle/JPADynamicsBlock.cpp). The factor is fixed at the start of the turn's particle calc, so the parallel particle
//     calc (GXV_PAR_PTCL, also its verify mode 2) sees one value. One-shot bursts still emit at least one particle; no emitter stops.
//   PETARI_BOSS_HAZE (MINOR CHANGE): 1 / perf: no heat-haze shimmer while a boss fight is on (Screen/HeatHazeEffect.cpp: the full-screen
//     ShimmerBoard and, when nothing else draws after it, the "Indirect" screen copy with GXV_INDCAP_SKIP); 2: no heat haze anywhere.
// Round 2 (cheaper look-alikes; each 0 = off, the default; 1 = every mode; perf = Performance mode only):
//   PETARI_BOSS_PSHADOW (MINOR CHANGE, nearly invisible): the sphere shadow volumes of small boss projectiles (Bowser's KoopaFireShort
//     and KoopaFireStairs, Fiery Dino Piranha's DinoPackunFire, Bugaboom's bombs) are drawn as one flat disc of the same radius on the
//     ground under them, in the same alpha-shadow pass with the same darkening (LiveActor/ShadowVolumeSphere.cpp). The bosses', Mario's
//     and every other shadow keep their volumes.
//   PETARI_BOSS_PTCL_GROW (with PETARI_BOSS_PTCL): the particles a thinned emitter makes are 1/sqrt(factor) larger (at most 1.6x), so
//     the storm keeps its covered area and, for additive sprites, its total light: fewer, larger particles instead of a sparser storm.
// Off (no list entry, no log, no cut) the per-turn cost is one test of the noted-boss count.
extern "C" int port_perf_mode_60(void);  // (platform/src/port/perf.cpp)
extern "C" {
float g_port_boss_ptcl_scale = 1.0f;  // (read by JPADynamicsBlock::create)
float g_port_boss_ptcl_grow = 1.0f;   // (round 2, PETARI_BOSS_PTCL_GROW: the size factor of the particles a thinned emitter makes)
}
namespace {
    struct PortBossKind {
        const char* mObjName;
        const char* mLabel;
        bool mPrimary;  // a boss (else a helper whose switches +appear turns on)
    };
    // In order of preference for @boss when a stage has more than one.
    const PortBossKind cPortBossKinds[] = {
        {"KoopaVs1", "Bowser 1", true},
        {"KoopaVs2", "Bowser 2", true},
        {"KoopaVs3", "Bowser 3", true},
        {"DinoPackun", "Dino Piranha", true},
        {"DinoPackunVs2", "Fiery Dino Piranha", true},
        {"OtaKing", "King Kaliente", true},
        {"BossStinkBug", "Bugaboom", true},
        {"TripodBoss", "Megaleg", true},
        {"Tripod2Boss", "Megaleg 2", true},
        {"BossBegoman", "Topmaniac", true},
        {"BossKameck", "Kamella", true},
        {"BossKameck2", "Kamella 2", true},
        {"TombSpider", "Tarantox", true},
        {"Dodoryu", "Major Burrows", true},
        {"Polta", "Bouldergeist", true},
        {"IceMerameraKing", "Baron Brrr", true},
        {"SkeletalFishBoss", "Kingfin", true},
        {"KoopaJrShip", "Bowser Jr. airship", true},
        {"KoopaSwitchKeeper", "Bowser switch keeper", false},
        {"KoopaViewSwitchKeeper", "Bowser view switch keeper", false},
    };
    const s32 cPortBossKindNum = sizeof(cPortBossKinds) / sizeof(cPortBossKinds[0]);

    struct PortBoss {
        LiveActor* mActor;
        s32 mKind;
    };
    PortBoss sPortBosses[24];
    s32 sPortBossNum = 0;
    u32 sPortSceneGen = 0;

    // The tour leg's place (portBossLegSet) and its progress.
    char sLegSpec[48];
    char sLegStage[48];
    u32 sLegGen = 0;        // the scene generation the jump was asked in (the leg's stage is a later one)
    u32 sLegDoneGen = 0;    // the generation whose warp (and switches) are done
    u32 sLegFramesGen = 0;  // the generation sLegFrames counts in
    bool sLegActive = false;
    s32 sLegArms = 0;       // how many times the leg's place was applied (a restart after a miss applies it again, 3 times at most)
    s32 sLegFrames = 0;
    s32 sLegSwitchAt = -1;
    bool sLegSwitchA = false;
    bool sLegSwitchAppear = false;

    // Fight state (portBossTurn) and the 5 s window of the "boss:" line.
    bool sBossFight = false;
    u32 sWinTurns = 0, sWinFight = 0, sWinPtclTurns = 0, sWinThinned = 0, sWinHaze = 0, sWinHazeCut = 0, sWinBloom = 0;
    f64 sWinLiveSum = 0.0, sWinEmtrSum = 0.0, sWinScaleSum = 0.0;
    u32 sWinLivePeak = 0, sWinEmtrPeak = 0, sPoolSize = 0;
    u32 sWinPshadow = 0, sWinPshadowDisc = 0;  // (round 2: projectile shadows drawn, of them as discs)

    int portBossModeKnob(const char* pName) {
        const char* e = getenv(pName);
        if (e == nullptr || *e == 0) {
            return 0;
        }
        if (*e == 'p' || *e == 'P') {
            return port_perf_mode_60() ? 1 : 0;
        }
        return atoi(e);
    }
    bool portBossLogOn() {
        return PORT_HOT_KNOB(int, [] { const char* e = getenv("PETARI_BOSS_LOG"); return e && *e == '1' ? 1 : 0; }()) != 0;
    }
    int portBossPtclMode() { return PORT_HOT_KNOB(int, portBossModeKnob("PETARI_BOSS_PTCL")); }
    int portBossPtclGrow() { return PORT_HOT_KNOB(int, portBossModeKnob("PETARI_BOSS_PTCL_GROW")); }
    int portBossPshadowMode() { return PORT_HOT_KNOB(int, portBossModeKnob("PETARI_BOSS_PSHADOW")); }
    int portBossHazeMode() { return PORT_HOT_KNOB(int, portBossModeKnob("PETARI_BOSS_HAZE")); }
    f32 portEnvF(const char* pName, f32 fallback) {
        const char* e = getenv(pName);
        return e && *e ? static_cast< f32 >(atof(e)) : fallback;
    }

    // The preferred primary boss of the stage, or -1.
    s32 portBossPrimary() {
        s32 best = -1;
        for (s32 i = 0; i < sPortBossNum; i++) {
            const s32 k = sPortBosses[i].mKind;
            if (cPortBossKinds[k].mPrimary && (best < 0 || k < sPortBosses[best].mKind)) {
                best = i;
            }
        }
        return best;
    }

    void portBossLegWarp() {
        const char* spec = sLegSpec;
        TVec3f pos;
        bool place = false;
        if (strncmp(spec, "boss", 4) == 0) {
            const s32 b = portBossPrimary();
            if (b < 0) {
                port_log_summary("tour: @boss: this stage has no boss of the list (%s scenario %d): the player stays", MR::getCurrentStageName(),
                                 (int)MR::getCurrentScenarioNo());
            } else {
                f32 up = portEnvF("PETARI_BOSS_UP", 600.0f);
                f32 side = portEnvF("PETARI_BOSS_SIDE", 1200.0f);
                sscanf(spec + 4, ":%f:%f", &up, &side);
                LiveActor* pBoss = sPortBosses[b].mActor;
                TVec3f grav(0.0f, 0.0f, 0.0f);
                MR::calcGravityVectorOrZero(pBoss, pBoss->mPosition, &grav, nullptr, 0);
                TVec3f upDir(-grav.x, -grav.y, -grav.z);
                const f32 len = sqrtf(upDir.x * upDir.x + upDir.y * upDir.y + upDir.z * upDir.z);
                if (len < 0.001f) {
                    upDir.set(0.0f, 1.0f, 0.0f);
                } else {
                    upDir.x /= len;
                    upDir.y /= len;
                    upDir.z /= len;
                }
                // a side direction: across the up direction (x, or z when up is nearly x)
                TVec3f ref = (upDir.x > 0.9f || upDir.x < -0.9f) ? TVec3f(0.0f, 0.0f, 1.0f) : TVec3f(1.0f, 0.0f, 0.0f);
                TVec3f t(upDir.y * ref.z - upDir.z * ref.y, upDir.z * ref.x - upDir.x * ref.z, upDir.x * ref.y - upDir.y * ref.x);
                const f32 tl = sqrtf(t.x * t.x + t.y * t.y + t.z * t.z);
                if (tl > 0.001f) {
                    t.x /= tl;
                    t.y /= tl;
                    t.z /= tl;
                }
                pos.set(pBoss->mPosition.x + upDir.x * up + t.x * side, pBoss->mPosition.y + upDir.y * up + t.y * side,
                        pBoss->mPosition.z + upDir.z * up + t.z * side);
                place = true;
                port_log_summary("tour: warping the player near %s at %.0f %.0f %.0f (up %.2f %.2f %.2f%s): to %.0f %.0f %.0f",
                                 cPortBossKinds[sPortBosses[b].mKind].mLabel, pBoss->mPosition.x, pBoss->mPosition.y, pBoss->mPosition.z, upDir.x,
                                 upDir.y, upDir.z, len < 0.001f ? ", no gravity there" : "", pos.x, pos.y, pos.z);
            }
        } else if (sscanf(spec, "%f/%f/%f", &pos.x, &pos.y, &pos.z) == 3) {
            place = true;
            port_log_summary("tour: warping the player to %.0f %.0f %.0f", pos.x, pos.y, pos.z);
        }
        if (place) {
            MR::setPlayerPos(pos);
        }
    }

    void portBossLegSwitch() {
        const s32 b = portBossPrimary();
        if (sLegSwitchA) {
            if (b < 0) {
                port_log_summary("tour: +a: no boss to start");
            } else {
                LiveActor* pBoss = sPortBosses[b].mActor;
                if (!MR::isValidSwitchA(pBoss)) {
                    port_log_summary("tour: +a: %s has no switch A (start it in play)", cPortBossKinds[sPortBosses[b].mKind].mLabel);
                } else if (MR::isOnSwitchA(pBoss)) {
                    port_log_summary("tour: +a: %s's switch A is on already", cPortBossKinds[sPortBosses[b].mKind].mLabel);
                } else {
                    MR::onSwitchA(pBoss);
                    port_log_summary("tour: +a: turned on %s's switch A (the fight's start)", cPortBossKinds[sPortBosses[b].mKind].mLabel);
                }
            }
        }
        if (sLegSwitchAppear) {
            s32 n = 0;
            for (s32 i = 0; i < sPortBossNum; i++) {
                StageSwitchCtrl* pCtrl = sPortBosses[i].mActor->mStageSwitchCtrl;
                if (pCtrl != nullptr && pCtrl->isValidSwitchAppear() && !pCtrl->isOnSwitchAppear()) {
                    StageSwitchFunction::onSwitchBySwitchIdInfo(*pCtrl->mSW_Appear);
                    n++;
                }
            }
            port_log_summary("tour: +appear: turned on %d appear switch(es) of the stage's boss actors", (int)n);
        }
    }

    void portBossLogLine() {
        const f64 t = sWinTurns ? sWinTurns : 1;
        const f64 tp = sWinPtclTurns ? sWinPtclTurns : 1;
        const s32 ptclMode = portBossPtclMode();
        const s32 hazeMode = portBossHazeMode();
        const TVec3f* pPlayer = MR::getPlayerPos();
        for (s32 i = 0; i < sPortBossNum; i++) {
            const PortBoss& rBoss = sPortBosses[i];
            if (!cPortBossKinds[rBoss.mKind].mPrimary) {
                continue;
            }
            // crash35: some boss classes never make a nerve (Dino Piranha runs its own sequence): mSpine is null, and getNerveStep() read through it
            // (console22, boss tour A, the first "boss:" line 5 s after the fight began). The step is then logged as -1.
            const LiveActor* pA = rBoss.mActor;
            const f32 dist = pPlayer != nullptr ? pA->mPosition.distance(*pPlayer) : -1.0f;
            port_log_summary("boss: %s:%d %s (%s) %s at %.0f %.0f %.0f, player %.0f away, nerve step %d%s; fight on %.0f%% of %u turns; particles "
                             "live %.0f mean / %u peak of %u, emitters %.0f / %u; boss particle cut %s (cap %.0f): thinned %.0f%% of turns, "
                             "scale %.2f mean%s; heat haze in %.0f%% of turns, %s (PETARI_BOSS_HAZE=%d); normal bloom on in %.0f%% of turns; "
                             "projectile shadows %.1f a turn, %.0f%% as discs (PETARI_BOSS_PSHADOW=%d)",
                             MR::getCurrentStageName(), (int)MR::getCurrentScenarioNo(), cPortBossKinds[rBoss.mKind].mLabel,
                             cPortBossKinds[rBoss.mKind].mObjName,
                             MR::isDead(pA) ? "dead" : (MR::isHiddenModel(pA) ? "hidden" : "alive"), pA->mPosition.x, pA->mPosition.y, pA->mPosition.z,
                             dist, pA->mSpine != nullptr ? (int)pA->getNerveStep() : -1, MR::isDemoActive() ? ", in a demo" : "", 100.0 * sWinFight / t, sWinTurns,
                             sWinLiveSum / tp, sWinLivePeak, sPoolSize, sWinEmtrSum / tp, sWinEmtrPeak, ptclMode ? "ON" : "off",
                             portEnvF("PETARI_BOSS_PTCL_CAP", 1200.0f), 100.0 * sWinThinned / tp, sWinPtclTurns ? sWinScaleSum / tp : 1.0,
                             portBossPtclGrow() ? ", grown to keep the area" : "",
                             100.0 * sWinHaze / t, sWinHazeCut ? "cut while the fight is on" : "as the game", (int)hazeMode, 100.0 * sWinBloom / t,
                             sWinPshadow / t, sWinPshadow ? 100.0 * sWinPshadowDisc / sWinPshadow : 0.0, portBossPshadowMode());
        }
        sWinTurns = sWinFight = sWinPtclTurns = sWinThinned = sWinHaze = sWinHazeCut = sWinBloom = sWinPshadow = sWinPshadowDisc = 0;
        sWinLiveSum = sWinEmtrSum = sWinScaleSum = 0.0;
        sWinLivePeak = sWinEmtrPeak = 0;
    }
}  // namespace

static void portBossSceneBegin() {
    sPortBossNum = 0;
    sPortSceneGen++;
    sBossFight = false;
    g_port_boss_ptcl_scale = 1.0f;
    sWinTurns = sWinFight = sWinPtclTurns = sWinThinned = sWinHaze = sWinHazeCut = sWinBloom = sWinPshadow = sWinPshadowDisc = 0;
    sWinLiveSum = sWinEmtrSum = sWinScaleSum = 0.0;
    sWinLivePeak = sWinEmtrPeak = 0;
}

static void portBossLegSet(const char* pStage, const char* pSpec) {
    sLegActive = false;
    sLegSwitchA = sLegSwitchAppear = false;
    sLegSwitchAt = -1;
    sLegFrames = 0;
    sLegArms = 0;
    sLegGen = sPortSceneGen;
    sLegDoneGen = sLegFramesGen = sPortSceneGen;
    if (pSpec == nullptr || *pSpec == 0) {
        return;
    }
    snprintf(sLegStage, sizeof(sLegStage), "%s", pStage != nullptr ? pStage : "");
    snprintf(sLegSpec, sizeof(sLegSpec), "%s", pSpec);
    if (char* plus = strchr(sLegSpec, '+')) {
        // the suffixes: "+a", "+appear" (either or both, in any order)
        for (const char* p = plus; p != nullptr; p = strchr(p + 1, '+')) {
            if (strncmp(p, "+appear", 7) == 0) {
                sLegSwitchAppear = true;
            } else if (p[1] == 'a' && (p[2] == 0 || p[2] == '+')) {
                sLegSwitchA = true;
            }
        }
        *plus = 0;
    }
    sLegActive = true;
}

extern "C" int port_boss_kind(const char* pObjName) {
    if (pObjName == nullptr) {
        return -1;
    }
    for (s32 i = 0; i < cPortBossKindNum; i++) {
        if (strcmp(pObjName, cPortBossKinds[i].mObjName) == 0) {
            return i;
        }
    }
    return -1;
}

extern "C" void port_boss_note(int kind, NameObj* pObj, const char* pName) {
    (void)pName;
    if (kind < 0 || kind >= cPortBossKindNum || pObj == nullptr || sPortBossNum >= (s32)(sizeof(sPortBosses) / sizeof(sPortBosses[0]))) {
        return;
    }
    LiveActor* pActor = static_cast< LiveActor* >(pObj);  // (every class of the list is a LiveActor)
    sPortBosses[sPortBossNum].mActor = pActor;
    sPortBosses[sPortBossNum].mKind = kind;
    sPortBossNum++;
    if (portBossLogOn()) {
        port_log_summary("boss: placed %s (%s) at %.0f %.0f %.0f%s%s%s", cPortBossKinds[kind].mLabel, cPortBossKinds[kind].mObjName, pActor->mPosition.x,
                         pActor->mPosition.y, pActor->mPosition.z, MR::isValidSwitchA(pActor) ? ", switch A" : "",
                         MR::isValidSwitchAppear(pActor) ? ", appear switch" : "", MR::isDead(pActor) ? ", dead until it appears" : "");
    }
}

// The turn's start of the particle calc (JPAEmitterManager::calcNormal): the boss particle cut's factor, and the log's counts.
extern "C" void port_boss_ptcl_turn(u32 live, u32 emitters, u32 poolSize) {
    if (sPortBossNum == 0) {
        g_port_boss_ptcl_scale = 1.0f;
        return;
    }
    f32 scale = 1.0f;
    if (sBossFight && portBossPtclMode() != 0 && !port::lockstep::On()) {  // (fmt32: no thinning in a lockstep session: the cut is chosen by this console's picture mode)
        const f32 cap = PORT_HOT_KNOB(f32, [] { const f32 v = portEnvF("PETARI_BOSS_PTCL_CAP", 1200.0f); return v < 100.0f ? 100.0f : v; }());
        const f32 floor = PORT_HOT_KNOB(f32, [] { const f32 v = portEnvF("PETARI_BOSS_PTCL_MIN", 0.4f); return v < 0.1f ? 0.1f : v > 1.0f ? 1.0f : v; }());
        if (static_cast< f32 >(live) > cap) {
            scale = cap / static_cast< f32 >(live);
            if (scale < floor) {
                scale = floor;
            }
            sWinThinned++;
        }
    }
    g_port_boss_ptcl_scale = scale;
    // round 2: fewer but larger particles: the size of each new particle of a thinned emitter times 1/sqrt(scale), so emitted area
    // (rate x size^2: the storm's coverage and, for additive sprites, its total light) stays what it was (at most x1.6).
    g_port_boss_ptcl_grow = (scale < 1.0f && portBossPtclGrow() != 0) ? (scale > 0.39f ? 1.0f / sqrtf(scale) : 1.6f) : 1.0f;
    sWinPtclTurns++;
    sWinScaleSum += scale;
    sWinLiveSum += live;
    sWinEmtrSum += emitters;
    if (live > sWinLivePeak) {
        sWinLivePeak = live;
    }
    if (emitters > sWinEmtrPeak) {
        sWinEmtrPeak = emitters;
    }
    sPoolSize = poolSize;
}

// ShadowVolumeSphere::draw (round 2): a boss projectile's shadow drawn as a disc (PETARI_BOSS_PSHADOW), and the count for the log.
extern "C" int port_boss_pshadow_on(void) {
    return portBossPshadowMode() != 0 ? 1 : 0;
}
extern "C" void port_boss_pshadow_count(int disc) {
    sWinPshadow++;
    if (disc) {
        sWinPshadowDisc++;
    }
}

// HeatHazeDirector::movement: 1 = leave the heat haze out this turn (PETARI_BOSS_HAZE).
extern "C" int port_boss_haze_off(int hazeHere) {
    if (hazeHere && sPortBossNum > 0) {
        sWinHaze++;
    }
    const int mode = portBossHazeMode();
    if (mode == 0) {
        return 0;
    }
    const int off = mode >= 2 || sBossFight;
    if (off && hazeHere && sPortBossNum > 0) {
        sWinHazeCut++;
    }
    return off;
}

static void portBossTurn(bool playing) {
    // The tour leg's place: in the leg's stage (a later scene than the jump's), once per build of it (a restart after a miss
    // builds it again: the place is applied again, 3 times at most per leg).
    if (sLegActive && sPortSceneGen != sLegGen && sPortSceneGen != sLegDoneGen && sLegArms < 3 &&
        strcmp(MR::getCurrentStageName(), sLegStage) == 0) {
        if (sLegFramesGen != sPortSceneGen) {
            sLegFramesGen = sPortSceneGen;
            sLegFrames = 0;
            sLegSwitchAt = -1;
        }
        if (playing) {
            sLegFrames++;
        }
        const s32 warpAt = PORT_HOT_KNOB(s32, [] { const f32 v = portEnvF("PETARI_TOUR_WARP_S", 4.0f); return static_cast< s32 >((v < 0.5f ? 0.5f : v) * 60.0f); }());
        if (sLegSwitchAt < 0 && sLegFrames >= warpAt) {
            portBossLegWarp();
            sLegSwitchAt = warpAt + 90;
            if (!sLegSwitchA && !sLegSwitchAppear) {
                sLegDoneGen = sPortSceneGen;
                sLegArms++;
            }
        } else if (sLegSwitchAt >= 0 && sLegFrames >= sLegSwitchAt) {
            portBossLegSwitch();
            sLegDoneGen = sPortSceneGen;
            sLegArms++;
        }
    }
    if (sPortBossNum == 0) {
        return;
    }
    // The fight is on: a boss of the list alive, shown and near the player.
    const TVec3f* pPlayer = MR::getPlayerPos();
    const f32 range = PORT_HOT_KNOB(f32, [] { return portEnvF("PETARI_BOSS_RANGE", 6000.0f); }());
    bool fight = false;
    for (s32 i = 0; i < sPortBossNum && pPlayer != nullptr; i++) {
        const LiveActor* pA = sPortBosses[i].mActor;
        if (cPortBossKinds[sPortBosses[i].mKind].mPrimary && !MR::isDead(pA) && !MR::isHiddenModel(pA) &&
            pA->mPosition.squared(*pPlayer) < range * range) {
            fight = true;
            break;
        }
    }
    sBossFight = fight;
    sWinTurns++;
    if (fight) {
        sWinFight++;
    }
    if (portBossLogOn() && MR::isNormalBloomOn()) {
        sWinBloom++;
    }
    if (sWinTurns >= 300) {
        if (portBossLogOn()) {
            portBossLogLine();
        } else {
            sWinTurns = sWinFight = sWinPtclTurns = sWinThinned = sWinHaze = sWinHazeCut = sWinBloom = sWinPshadow = sWinPshadowDisc = 0;
            sWinLiveSum = sWinEmtrSum = sWinScaleSum = 0.0;
            sWinLivePeak = sWinEmtrPeak = 0;
        }
    }
}
#endif

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// flow23 (8 Nov): PETARI_FLOW, the normal galaxy entry for the console, and the "flow:" lines.
//
// Every console test before 2.3 test build c10 entered stages with PETARI_STAGE (MR::requestChangeStageInGameMoving: the stage is built
// behind a closed screen, no star select). A player goes Observatory -> dome -> galaxy map -> the flight out of the dome (its white
// flash) -> the star select (ScenarioSelectScene, its own camera, particles and star models, drawn on the game thread WHILE the loading
// thread builds the stage behind it; the stage's MarioHolder exists, its MarioActor does not until a star is chosen) -> the stage,
// which starts under the star select's 3-second white fade-in. That path crashed on c10 (Performance mode) and nothing tested it.
//
//   PETARI_FLOW=Galaxy[:star][,Galaxy[:star]...]   e.g. PETARI_FLOW=EggStarGalaxy:1,HoneyBeeKingdomGalaxy:1
//     After PETARI_FLOW_START turns of play anywhere (default 120 = 2 s; the Observatory after the file select), each entry is entered
//     the way the dome hands over: MR::requestStartScenarioSelect(Galaxy) (the call AstroDomeGalaxySelector makes after the flight;
//     GalaxyMoveArgument type 2: the star select in front, the stage loading behind it). On the star select, PETARI_FLOW_PICK turns
//     (default 90) after it waits for a choice, the star with that number is chosen as the pad's A would choose it (star 1 when no
//     number is given; when that star is not on the select in this save, the first star shown, logged). ":back" presses Back instead
//     (the cancel path: back to where the request came from). The stage then plays for PETARI_FLOW_HOLD turns of play (default 1200 =
//     20 s; play = GameSceneAction without a demo; or 4x as many turns of anything) and the next entry is requested from there.
//     PETARI_FLOW_LOOP=1 starts the list again after the last entry (a soak); PETARI_STAGE is ignored while PETARI_FLOW is set.
//   PETARI_FLOW_DOME=<dome scenario 1-6> (optional, best effort): each entry first goes to the dome (AstroDome, that scenario), and after
//     PETARI_FLOW_START turns of play there the dome's own jump-out demo is started (the flight and its white flash; the galaxy selector's
//     DemoJumpOut, which then asks the star select for the entry's galaxy). If the demo has not handed over within 15 s, the star select
//     is asked directly (logged).
//   PETARI_FLOW_LOG=1: the "flow:" lines without the hook (also with PETARI_FLOW): every change of the scene controller's phase, of the
//     star select's state and of the game scene's state, at once, and a heartbeat every PETARI_FLOW_BEAT_S seconds (default 2; 0 = changes
//     only) from the game thread's loop, also in turns that draw nothing (frame skipping, a cutscene skip's fast-forward): turns, pictures,
//     skipped turns, fast-forward, whether a wipe has the screen closed. The last "flow:" lines before a crash or a stall say which step.
// Debug only. Unset: one cached test a turn in GameScene::update and in GameSystem::frameLoop.
// ---------------------------------------------------------------------------------------------------------------------------------
#include "Game/System/GameSystemSceneController.hpp"
extern "C" const char* port_scene_ctrl_phase(void);   // (GameSystemSceneController.cpp)
extern "C" const char* port_star_select_state(void);  // (ScenarioSelectScene.cpp)
extern "C" int port_flow_dome_jump_out(const char* pGalaxy);  // (MapObj/AstroDomeGalaxySelector.cpp: 1 started, 0 no selector)
extern "C" void port_flow_dome_reset(void);
extern "C" uint32_t port_frameskip_count(void) __attribute__((weak));  // (platform/src/port/perf.cpp)
extern "C" int port_screen_quiet(void) __attribute__((weak));          // (vita/runtime/flat_view.cpp)
namespace {
    struct PortFlowLeg {
        char mStage[48];
        s32 mStar;  // > 0 the star to choose, -1 Back
    };
    enum PortFlowState { cFlowIdle, cFlowRequested, cFlowInLeg, cFlowToDome, cFlowDone };
    PortFlowLeg sFlowLegs[24];
    s32 sFlowNum = -1;  // (-1: not parsed yet)
    s32 sFlowIndex = 0;  // the next entry
    s32 sFlowCur = -1;   // the entry asked for last
    s32 sFlowState = cFlowIdle;
    bool sFlowViaDome = false;
    s32 sFlowAsks = 0;
    volatile u32 sFlowSceneGen = 0;  // (GameScene::init, loading thread; read by GameScene::update after the scene started)
    u32 sFlowSeenGen = 0;
    s32 sFlowPlay = 0, sFlowAll = 0;
    s32 sFlowWant = 0;  // what the star select chooses: > 0 a star, -1 Back, 0 nothing (ScenarioSelectLayout.cpp)
    bool sFlowPlayLogged = false;
    s64 sFlowAskNs = 0, sFlowSceneNs = 0;
    s32 sFlowHold = 1200, sFlowStart = 120, sFlowPick = 90, sFlowDome = 0;
    bool sFlowLoop = false;

    s32 portFlowEnvI(const char* pName, s32 fallback, s32 lo, s32 hi) {
        const char* e = getenv(pName);
        if (e == nullptr || *e == 0) {
            return fallback;
        }
        s32 v = atoi(e);
        return v < lo ? lo : v > hi ? hi : v;
    }
    f64 portFlowSec(s64 fromNs) {
        return fromNs != 0 ? (port_host_time_ns() - fromNs) / 1e9 : 0.0;
    }
    void portFlowParse() {
        sFlowNum = 0;
        const char* e = getenv("PETARI_FLOW");
        if (e == nullptr || *e == 0) {
            return;
        }
        sFlowHold = portFlowEnvI("PETARI_FLOW_HOLD", 1200, 60, 1000000);
        sFlowStart = portFlowEnvI("PETARI_FLOW_START", 120, 1, 1000000);
        sFlowPick = portFlowEnvI("PETARI_FLOW_PICK", 90, 0, 100000);
        sFlowDome = portFlowEnvI("PETARI_FLOW_DOME", 0, 0, 16);
        sFlowLoop = portFlowEnvI("PETARI_FLOW_LOOP", 0, 0, 1) != 0;
        for (const char* p = e; *p != 0 && sFlowNum < static_cast< s32 >(sizeof(sFlowLegs) / sizeof(sFlowLegs[0]));) {
            const char* end = strchr(p, ',');
            const size_t n = end != nullptr ? static_cast< size_t >(end - p) : strlen(p);
            char item[64];
            snprintf(item, sizeof(item), "%.*s", static_cast< int >(n < sizeof(item) - 1 ? n : sizeof(item) - 1), p);
            PortFlowLeg& leg = sFlowLegs[sFlowNum];
            leg.mStar = 1;
            if (char* colon = strchr(item, ':')) {
                *colon = 0;
                leg.mStar = strcmp(colon + 1, "back") == 0 ? -1 : atoi(colon + 1);
                if (leg.mStar == 0) {
                    leg.mStar = 1;
                }
            }
            snprintf(leg.mStage, sizeof(leg.mStage), "%s", item);
            if (leg.mStage[0] != 0) {
                sFlowNum++;
            }
            p = end != nullptr ? end + 1 : p + n;
        }
        port_log_summary("flow: PETARI_FLOW armed: %d entries, %d turns of play before the first, %d on the star select before choosing, %d of "
                         "play in each stage%s%s",
                         static_cast< int >(sFlowNum), static_cast< int >(sFlowStart), static_cast< int >(sFlowPick),
                         static_cast< int >(sFlowHold), sFlowDome ? ", through the dome's jump-out demo" : "", sFlowLoop ? ", looping" : "");
    }
    bool portFlowLogOn() {
        static int sOn = -1;
        if (sOn < 0) {
            const char* l = getenv("PETARI_FLOW_LOG");
            const char* f = getenv("PETARI_FLOW");
            sOn = (l != nullptr && *l != 0 && *l != '0') || (f != nullptr && *f != 0) ? 1 : 0;
        }
        return sOn != 0;
    }
    // Ask for the star select of entry sFlowIndex (or for the dome first).
    void portFlowAsk(bool direct) {
        const PortFlowLeg& leg = sFlowLegs[sFlowCur];
        sFlowAskNs = port_host_time_ns();
        sFlowAll = 0;
        sFlowPlay = 0;
        if (!direct && sFlowDome > 0 && !MR::isEqualStageName("AstroDome")) {
            port_log_summary("flow: entry %d of %d (%s:%d): going to the dome first (AstroDome scenario %d)", static_cast< int >(sFlowCur + 1),
                             static_cast< int >(sFlowNum), leg.mStage, static_cast< int >(leg.mStar), static_cast< int >(sFlowDome));
            MR::requestChangeStageInGameMoving("AstroDome", sFlowDome);
            sFlowState = cFlowToDome;
            return;
        }
        sFlowWant = leg.mStar;
        sFlowState = cFlowRequested;
        sFlowAsks++;
        if (!direct && sFlowDome > 0) {
            const int r = port_flow_dome_jump_out(leg.mStage);
            port_log_summary("flow: entry %d of %d (%s:%d): the dome's jump-out demo (the flight) %s", static_cast< int >(sFlowCur + 1),
                             static_cast< int >(sFlowNum), leg.mStage, static_cast< int >(leg.mStar),
                             r ? "asked for" : "has no galaxy selector here: asking the star select directly");
            if (r) {
                sFlowViaDome = true;
                return;
            }
        }
        sFlowViaDome = false;
        port_log_summary("flow: entry %d of %d: asking for the star select of %s (from %s scenario %d), then star %d", static_cast< int >(sFlowCur + 1),
                         static_cast< int >(sFlowNum), leg.mStage, MR::getCurrentStageName(), static_cast< int >(MR::getCurrentScenarioNo()),
                         static_cast< int >(leg.mStar));
        MR::requestStartScenarioSelect(leg.mStage);
    }
    void portFlowNext() {
        if (sFlowIndex >= sFlowNum) {
            if (!sFlowLoop) {
                port_log_summary("flow: done: all %d entries", static_cast< int >(sFlowNum));
                port_log_flush();  // (strikers2audit: the tour is over, the process is stopped from outside: the last lines are on the card first)
                sFlowState = cFlowDone;
                return;
            }
            port_log_summary("flow: the list again (PETARI_FLOW_LOOP)");
            sFlowIndex = 0;
        }
        sFlowCur = sFlowIndex++;
        sFlowAsks = 0;
        portFlowAsk(false);
    }
    const char* portGameSceneState(const GameScene* pScene) {
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneScenarioOpeningCamera))) return "opening camera";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneScenarioStarter))) return "scenario starter";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneAction))) return MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isDemoActive() ? "action (demo)" : "action";
        if (pScene->isNerve(GET_NERVE(GameScene, GameScenePauseMenu))) return "pause menu";
        if (pScene->isNerve(GET_NERVE(GameScene, GameScenePowerStarGet))) return "power star get";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGrandStarGet))) return "grand star get";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGameOver))) return "game over";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneCometRetryAfterMiss))) return "comet retry";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneSaveAfterGameOver))) return "save after game over";
        if (pScene->isNerve(GET_NERVE(GameScene, GameScenePlayMovie))) return "movie";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneTimeUp))) return "time up";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneGalaxyMap))) return "galaxy map";
        if (pScene->isNerve(GET_NERVE(GameScene, GameSceneStaffRoll))) return "staff roll";
        return "?";
    }
}  // namespace

static void portFlowSceneBegin() {
    sFlowSceneGen = sFlowSceneGen + 1;
    port_flow_dome_reset();  // (the dome's galaxy selector notes itself again when it is placed)
}

static bool portFlowActive() {
    if (sFlowNum < 0) {
        portFlowParse();
    }
    return sFlowNum > 0;
}

// GameScene::update, every turn (game thread). action: GameSceneAction; playing: and no demo.
static void portFlowTurn(bool action, bool playing, bool starter) {
    if (!portFlowActive() || MR::isStageFileSelect()) {
        return;
    }
    // flow76: a scenario starter that does not end (Honeyhive on the console, night54: the starter waits for Mario's rush message / a held A, and a
    // first-visit save has no A-to-skip) must not eat the leg: after PETARI_FLOW_STARTER_S seconds (default 40; 0 = off) in the starter the flow
    // ends it the way ScenarioStarter::exeShowWelcomeLayout does (notify the scene, cinema frame open, game layout on). Only with PETARI_FLOW.
    {
        static s64 sStarterNs = 0;
        static u32 sStarterGen = 0;
        static bool sStarterForced = false;
        static const s32 limitS = portFlowEnvI("PETARI_FLOW_STARTER_S", 40, 0, 3600);
        if (sStarterGen != sFlowSceneGen) {
            sStarterGen = sFlowSceneGen;
            sStarterNs = 0;
            sStarterForced = false;
        }
        if (!starter) {
            sStarterNs = 0;
        } else if (limitS > 0 && !sStarterForced) {
            if (sStarterNs == 0) {
                sStarterNs = port_host_time_ns();
            } else if (port_host_time_ns() - sStarterNs >= static_cast< s64 >(limitS) * 1000000000LL) {
                sStarterForced = true;
                port_log_summary("flow: %s scenario %d: still in the scenario starter after %d s: ending it (PETARI_FLOW_STARTER_S)",
                                 MR::getCurrentStageName(), static_cast< int >(MR::getCurrentScenarioNo()), static_cast< int >(limitS));
                GameSceneFunction::notifyEndScenarioStarter();
                MR::tryFrameToScreenCinemaFrame();
                MR::forceAppearDefaultGameLayout();
            }
        }
    }
    if (sFlowSeenGen != sFlowSceneGen) {
        // a new game scene runs
        sFlowSeenGen = sFlowSceneGen;
        sFlowPlay = sFlowAll = 0;
        sFlowPlayLogged = false;
        sFlowSceneNs = port_host_time_ns();
        const char* stage = MR::getCurrentStageName();
        port_log_summary("flow: scene started: %s scenario %d (star %d)%s", stage, static_cast< int >(MR::getCurrentScenarioNo()),
                         static_cast< int >(MR::getCurrentSelectedScenarioNo()), sFlowAskNs != 0 ? "" : " (before any request)");
        if (sFlowState == cFlowRequested && sFlowCur >= 0) {
            const PortFlowLeg& leg = sFlowLegs[sFlowCur];
            sFlowWant = 0;
            if (leg.mStar > 0 && MR::isEqualString(stage, leg.mStage)) {
                port_log_summary("flow: entry %d of %d: in %s with star %d (asked %d), %.1f s after the request", static_cast< int >(sFlowCur + 1),
                                 static_cast< int >(sFlowNum), stage, static_cast< int >(MR::getCurrentSelectedScenarioNo()),
                                 static_cast< int >(leg.mStar), portFlowSec(sFlowAskNs));
                sFlowState = cFlowInLeg;
            } else {
                port_log_summary("flow: entry %d of %d: %s instead of %s (%s), %.1f s after the request; next entry after %d turns of play",
                                 static_cast< int >(sFlowCur + 1), static_cast< int >(sFlowNum), stage, leg.mStage,
                                 leg.mStar < 0 ? "Back on the star select" : "not the stage asked for", portFlowSec(sFlowAskNs),
                                 static_cast< int >(sFlowStart));
                sFlowState = cFlowIdle;
            }
        } else if (sFlowState == cFlowToDome) {
            if (MR::isEqualStageName("AstroDome")) {
                port_log_summary("flow: in the dome (scenario %d); the jump-out after %d turns of play", static_cast< int >(MR::getCurrentScenarioNo()),
                                 static_cast< int >(sFlowStart));
            } else {
                port_log_summary("flow: asked for the dome, got %s: asking the star select directly after %d turns of play",
                                 MR::getCurrentStageName(), static_cast< int >(sFlowStart));
            }
        }
    }
    sFlowAll++;
    if (playing) {
        sFlowPlay++;
        if (!sFlowPlayLogged) {
            sFlowPlayLogged = true;
            port_log_summary("flow: play began in %s scenario %d, %.1f s after the scene started", MR::getCurrentStageName(),
                             static_cast< int >(MR::getCurrentScenarioNo()), portFlowSec(sFlowSceneNs));
        }
    }
    switch (sFlowState) {
    case cFlowIdle:
        if (playing && sFlowPlay >= sFlowStart) {
            portFlowNext();
        }
        break;
    case cFlowInLeg:
        if ((playing && sFlowPlay >= sFlowHold) || (action && sFlowAll >= sFlowHold * 4)) {
            port_log_summary("flow: entry %d of %d (%s) done: %d turns of play, %d in all", static_cast< int >(sFlowCur + 1),
                             static_cast< int >(sFlowNum), MR::getCurrentStageName(), static_cast< int >(sFlowPlay), static_cast< int >(sFlowAll));
            portFlowNext();
        }
        break;
    case cFlowToDome:
        if (playing && sFlowPlay >= sFlowStart && sFlowAll >= sFlowStart) {
            portFlowAsk(!MR::isEqualStageName("AstroDome"));
        }
        break;
    case cFlowRequested:
        // (a request the game did not act on: a demo or a scene change of its own in the way; or the dome demo never handed over)
        if (action && sFlowAll >= (sFlowViaDome ? 900 : 600)) {
            if (sFlowAsks >= 3) {
                port_log_summary("flow: entry %d of %d: no star select after 3 requests; going on with the next", static_cast< int >(sFlowCur + 1),
                                 static_cast< int >(sFlowNum));
                sFlowWant = 0;
                portFlowNext();
            } else {
                port_log_summary("flow: entry %d of %d: still in %s %d turns after the request%s: asking again", static_cast< int >(sFlowCur + 1),
                                 static_cast< int >(sFlowNum), MR::getCurrentStageName(), static_cast< int >(sFlowAll),
                                 sFlowViaDome ? " (the dome's demo did not hand over)" : "");
                portFlowAsk(true);
            }
        }
        break;
    default:
        break;
    }
}

// ScenarioSelectLayout::exeWaitScenarioSelect (game thread): what to choose (> 0 a star, -1 Back, 0 nothing: the player chooses).
// ab27 (GXV_AB_PLAN): the A/B driver (vgx_backend_gl.cpp abrun) reads the leg's turns of play to time its start (-1: a flow but not in a leg, -2: no flow) and ends the leg when its plan is done.
extern "C" int port_flow_play_turns(void) {
    if (sFlowNum <= 0) return -2;
    return sFlowState == cFlowInLeg ? static_cast< int >(sFlowPlay) : -1;
}
extern "C" void port_flow_finish_leg(void) {
    if (sFlowNum > 0 && sFlowState == cFlowInLeg) {
        port_log_summary("flow: entry %d of %d: ended by the A/B driver (GXV_AB_PLAN)", static_cast< int >(sFlowCur + 1), static_cast< int >(sFlowNum));
        sFlowPlay = sFlowHold;
    }
}
extern "C" int port_flow_want_star(void) {
    return sFlowNum > 0 && sFlowState == cFlowRequested ? sFlowWant : 0;
}
extern "C" int port_flow_pick_steps(void) {
    return sFlowPick;
}
// The choice made (shown: bit n = star n is on the select).
extern "C" void port_flow_note_pick(int want, int picked, unsigned shown) {
    char list[40];
    int n = 0;
    list[0] = 0;
    for (int i = 1; i < 16 && n < static_cast< int >(sizeof(list)) - 4; i++) {
        if (shown & (1u << i)) {
            n += snprintf(list + n, sizeof(list) - n, "%s%d", n ? " " : "", i);
        }
    }
    if (want < 0) {
        port_log_summary("flow: star select: Back (stars shown: %s)", list);
    } else if (picked == want) {
        port_log_summary("flow: star select: choosing star %d (stars shown: %s), %.1f s after the request", picked, list, portFlowSec(sFlowAskNs));
    } else {
        port_log_summary("flow: star select: star %d is not on the select in this save (stars shown: %s): choosing star %d", want, list, picked);
    }
    sFlowWant = 0;  // (once)
}

// GameSystem::frameLoop, at the start of every turn (game thread): the changes and the heartbeat (PETARI_FLOW_LOG / PETARI_FLOW).
extern "C" void port_flow_beat(void) {
    ++sHrw36Turn;  // (hold36: the turn's number for port_hrw_player36's hold; the game thread)
    if (!portFlowLogOn()) {
        return;
    }
    static const char* sCtrl = nullptr;
    static const char* sSel = nullptr;
    static const char* sGame = nullptr;
    static u32 sTurns = 0, sForward = 0;
    static u32 sSkipped0 = 0;
    static s64 sBeatNs = 0;
    static const s64 beatNs = static_cast< s64 >(portFlowEnvI("PETARI_FLOW_BEAT_S", 2, 0, 3600)) * 1000000000LL;
    GameSystem* pSystem = SingletonHolder< GameSystem >::get();
    GameSystemSceneController* pCtrl = pSystem != nullptr ? pSystem->mSceneController : nullptr;
    if (pCtrl == nullptr) {
        return;
    }
    const s64 now = port_host_time_ns();
    sTurns++;
    const bool forward = port_skip_fast_forwarding() != 0;
    if (forward) {
        sForward++;
    }
    const char* ctrl = port_scene_ctrl_phase();
    const char* sel = port_star_select_state();
    const char* game = "-";
    if (strcmp(ctrl, "normal") == 0 && pCtrl->mScene != nullptr && MR::isEqualSceneName("Game")) {
        game = portGameSceneState(static_cast< const GameScene* >(pCtrl->mScene));
    }
    if (ctrl != sCtrl) {
        port_log_summary("flow: scene controller: %s -> %s (scene %s, stage %s scenario %d)", sCtrl != nullptr ? sCtrl : "-", ctrl,
                         pCtrl->mCurrSceneControlInfo.mScene, pCtrl->mCurrSceneControlInfo.mStage,
                         static_cast< int >(pCtrl->mCurrSceneControlInfo.mScenarioNo));
        sCtrl = ctrl;
    }
    if (sel != sSel) {
        port_log_summary("flow: star select: %s -> %s", sSel != nullptr ? sSel : "-", sel);
        sSel = sel;
    }
    if (game != sGame) {
        port_log_summary("flow: game scene: %s -> %s (%s scenario %d)", sGame != nullptr ? sGame : "-", game, MR::getCurrentStageName(),
                         static_cast< int >(MR::getCurrentScenarioNo()));
        sGame = game;
    }
    if (beatNs > 0 && now - sBeatNs >= beatNs) {
        const u32 skipped = port_frameskip_count ? port_frameskip_count() : 0;
        const u32 sk = skipped - sSkipped0;
        const u32 drawn = sTurns > sk + sForward ? sTurns - sk - sForward : 0;
        port_log_summary("flow: beat: %u turns in %.1f s (pictures %u, skipped %u, cutscene fast-forward %u); controller %s; star select %s; game "
                         "scene %s; %s scenario %d; screen %s",
                         sTurns, sBeatNs != 0 ? (now - sBeatNs) / 1e9 : 0.0, drawn, sk, sForward, ctrl, sel, game, MR::getCurrentStageName(),
                         static_cast< int >(MR::getCurrentScenarioNo()),
                         port_screen_quiet != nullptr && port_screen_quiet() == 2 ? "closed by a wipe" :
                         port_screen_quiet != nullptr && port_screen_quiet() == 1 ? "pause menu" : "open");
        sBeatNs = now;
        sTurns = sForward = 0;
        sSkipped0 = skipped;
    }
}
#endif

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// route24: the input script's clock counted in GAME TURNS from the start of play in a stage.
//
//   PETARI_SCRIPT_ANCHOR=play   (read by vita/vgx-game/entry.cpp together with ux0:data/smg/input.txt; unset = the old clocks)
//     t = 0 of the script is the first turn of play (GameSceneAction, no demo) of a game scene, and each further GameScene::update turn
//     (pauses and demos included, drawn or skipped) is 1/60 s: the script lands on the same game moment whatever the picture rate, which
//     the pictures-based PETARI_GAME_CLOCK does not (a skipped turn draws nothing, so in a 30-picture mode a script second was two game
//     seconds). Every new game scene (a stage entered by PETARI_STAGE / PETARI_FLOW, a restart after a lost life) starts the script again
//     from 0; no input before the first play turn, and none in scenes that do not match.
//   Which scenes match: the stages named in PETARI_SCRIPT_STAGE (comma list; "Galaxy" or "Galaxy:star"), else those of PETARI_FLOW, else
//     those of PETARI_STAGE (only the name before ':' or '@' counts), else every game scene (the Comet Observatory too).
//   PETARI_SCRIPT_LOG=1: "script:" lines when a scene's clock starts or a scene that does not match begins play.
//     Also (imp45) every 300 script turns "script: route t = N s: player x y z, camera x y z": two runs are a pair only where these agree.
// Off (the variable unset) the cost is one cached test a turn.
// ---------------------------------------------------------------------------------------------------------------------------------
#include <atomic>
namespace {
    std::atomic< u32 > sScriptSceneCount{0};
    std::atomic< unsigned long long > sScriptTurn{0};  // 0: no clock; else (turns since the first play turn) + 1
    u32 sScriptSeenGen = 0;
    bool sScriptStarted = false;
    bool sScriptNoteDone = false;
    int sScriptState = -1;  // -1 not read yet, 0 off, 1 on
    bool sScriptLog = false;
    char sScriptNames[512];
    bool sScriptAny = true;

    void portScriptRead() {
        const char* a = getenv("PETARI_SCRIPT_ANCHOR");
        sScriptState = a != nullptr && strcmp(a, "play") == 0 ? 1 : 0;
        const char* l = getenv("PETARI_SCRIPT_LOG");
        sScriptLog = l != nullptr && atoi(l) != 0;
        const char* list = getenv("PETARI_SCRIPT_STAGE");
        if (list == nullptr || *list == 0) {
            list = getenv("PETARI_FLOW");
        }
        if (list == nullptr || *list == 0) {
            list = getenv("PETARI_STAGE");
        }
        if (list == nullptr || *list == 0) {
            // fastboot32: PETARI_BOOT=<Galaxy>[:scenario] names the scripted stage too (not its keywords 0 / 1 / flow / stage)
            const char* b = getenv("PETARI_BOOT");
            if (b != nullptr && *b >= 'A' && *b <= 'Z') {
                list = b;
            }
        }
        sScriptAny = list == nullptr || *list == 0;
        if (!sScriptAny) {
            snprintf(sScriptNames, sizeof(sScriptNames), "%s", list);
        }
    }
    bool portScriptStageMatches() {
        if (sScriptAny) {
            return true;
        }
        const char* stage = MR::getCurrentStageName();
        if (stage == nullptr) {
            return false;
        }
        const size_t len = strlen(stage);
        for (const char* p = sScriptNames; *p != 0;) {
            const char* end = p;
            while (*end != 0 && *end != ',' && *end != ':' && *end != '@') {
                end++;
            }
            if (static_cast< size_t >(end - p) == len && strncmp(p, stage, len) == 0) {
                return true;
            }
            while (*end != 0 && *end != ',') {
                end++;
            }
            p = *end == ',' ? end + 1 : end;
        }
        return false;
    }
}  // namespace

static u32 portScriptSceneBegin() {
    return sScriptSceneCount.fetch_add(1) + 1;  // (GameScene::init, loading thread)
}

// entry.cpp's pad loop (any thread): 0 = the script has not started in this scene; n = n - 1 turns since its t = 0.
extern "C" unsigned long long port_script_scene_turn(void) {
    return sScriptTurn.load(std::memory_order_relaxed);
}

// ~GameScene: this scene's clock ends with it (a later scene may already have started its own: only the running clock is stopped).
static void portScriptSceneEnd(u32 gen) {
    if (sScriptState == 1 && sScriptSeenGen == gen) {
        sScriptTurn.store(0, std::memory_order_relaxed);
    }
}

// GameScene::update, every turn (game thread). playing: the action nerve and no demo.
static void portScriptTurn(u32 gen, bool playing) {
    if (sScriptState < 0) {
        portScriptRead();
    }
    if (sScriptState == 0) {
        return;
    }
    if (gen != sScriptSeenGen) {  // a new game scene runs
        sScriptSeenGen = gen;
        sScriptStarted = false;
        sScriptNoteDone = false;
        sScriptTurn.store(0, std::memory_order_relaxed);
    }
    if (!sScriptStarted) {
        if (!playing) {
            return;
        }
        if (!portScriptStageMatches()) {
            if (sScriptLog && !sScriptNoteDone) {
                sScriptNoteDone = true;
                port_log_summary("script: play began in %s: not a scripted stage, no input", MR::getCurrentStageName());
            }
            return;
        }
        sScriptStarted = true;
        sScriptTurn.store(1, std::memory_order_relaxed);
        if (sScriptLog) {
            port_log_summary("script: clock started: t = 0 at the first play turn in %s scenario %d", MR::getCurrentStageName(),
                             static_cast< int >(MR::getCurrentScenarioNo()));
        }
        return;
    }
    const unsigned long long turn = sScriptTurn.fetch_add(1, std::memory_order_relaxed);  // (the value before: turns since the first play turn + 1)
    // imp45: where the route is, every 300 script turns (5 s), with PETARI_SCRIPT_LOG=1. Two runs of one script whose lines differ are not on the same
    // route (the night39 logs: the same script ended at places with 110 or 350 draws a picture), so their fps figures are not a pair.
    if (sScriptLog && turn % 300 == 0 && MR::isExistMario()) {
        const TVec3f* p = MR::getPlayerPos();
        if (p != nullptr) {
            const TVec3f cam = MR::getCamPos();
            port_log_summary("script: route t = %u s: player %.0f %.0f %.0f, camera %.0f %.0f %.0f%s", static_cast< unsigned >(turn / 60), p->x, p->y, p->z,
                             cam.x, cam.y, cam.z, playing ? "" : " (not in play: pause or demo)");
        }
    }
}
// ---------------------------------------------------------------------------------------------------------------------------------
// routes25 (10 Nov): benchmark moments without a human. Debug only; every switch unset = off (one test of a file-scope int a turn).
//
//   PETARI_STAGE=Galaxy:scenario#id[/zone][@where]   the stage is entered with that Mario start id instead of id 0 (the id the game's own
//     restart after a miss passes: requestChangeStageAfterMiss(getPlayerRestartIdInfo)). It is the "MarioNo" of one of the stage's start
//     entries; PETARI_SURVEY=1 lists them. An id the stage does not have: the default start (id 0), "start:" log line (never a null).
//   PETARI_SURVEY=1   once per scene, at its first play turn: "survey:" lines: the stage's Mario start entries (MarioNo, zone, camera id,
//     world position and rotation), its GeneralPos entries (name, position; PETARI_SURVEY_GPOS_MAX lines, default 300), the zone names,
//     the start the scene used, and where Mario stands.
//   PETARI_SURVEY_HOP=<seconds>  also: every <seconds> of play the player is put at the next start entry (PETARI_SURVEY_MAX of them,
//     default 24; PETARI_SURVEY_GPOS=1 then the GeneralPos entries too), a "survey: hop k of n" line says where, and one 1.5 s later says
//     whether Mario landed ("on the ground" or not). The 5 s profile lines of the log (turn-prof, gpu-scenes, workload fe-frame, fps)
//     that fall inside a hop are that place's costs (tools/surveysum.py reads them). PETARI_STAGE tour legs do not end while hops remain.
//   If a scene restarts (Mario lost a life) the hops of the same stage go on where they were.
// ---------------------------------------------------------------------------------------------------------------------------------
namespace MR {
    bool portSurveyStart(int idx, int* pMarioNo, int* pZone, int* pCamera, f32* pPos, f32* pDir);
    bool portSurveyGeneralPos(int idx, const char** ppName, f32* pPos);
}  // namespace MR

namespace {
    volatile int sStartOverride = 0;
    volatile int sStartNotes = 0;

    int sSvMode = -1;  // -1 not read, 0 off, 1 lines, 2 lines + hops
    f32 sSvHopSec = 0.0f;
    int sSvHopMax = 24;
    int sSvGposMax = 300;
    int sSvGposHops = 0;
    u32 sSvGen = 0;
    bool sSvListed = false;
    bool sSvBusy = false;
    int sSvStartNum = 0;
    int sSvHopNum = 0;
    int sSvHopIdx = 0;   // the next hop
    int sSvHopTurns = 0;  // play turns since the last hop (or since the listing)
    int sSvIdle = 0;      // consecutive turns that were not play while hops remain
    int sSvPlayTurns = 0;
    char sSvStage[48];
    int sSvScenario = -1;

    void portSvRead() {
        const char* e = getenv("PETARI_SURVEY");
        const char* h = getenv("PETARI_SURVEY_HOP");
        sSvMode = 0;
        if (h != nullptr && *h != 0 && atof(h) > 0.0) {
            sSvHopSec = static_cast< f32 >(atof(h));
            sSvMode = 2;
        } else if (e != nullptr && *e != 0 && *e != '0') {
            sSvMode = 1;
        }
        const char* m = getenv("PETARI_SURVEY_MAX");
        sSvHopMax = m != nullptr && *m != 0 ? atoi(m) : 24;
        const char* g = getenv("PETARI_SURVEY_GPOS_MAX");
        sSvGposMax = g != nullptr && *g != 0 ? atoi(g) : 300;
        const char* gh = getenv("PETARI_SURVEY_GPOS");
        sSvGposHops = gh != nullptr && *gh != 0 && *gh != '0' ? 1 : 0;
        if (sSvMode != 0) {
            port_log_summary("survey: PETARI_SURVEY armed: %s, hop %.1f s, at most %d hops%s, %d GeneralPos lines", sSvMode == 2 ? "start entries + hops" : "listing only",
                             sSvHopSec, sSvHopMax, sSvGposHops ? " (GeneralPos entries too)" : "", sSvGposMax);
        }
    }

    // Shift-JIS and other non-ASCII bytes as ~xx, the way the log's other name lists do.
    void portSvName(const char* pIn, char* pOut, size_t size) {
        size_t o = 0;
        for (const char* p = pIn != nullptr ? pIn : ""; *p != 0 && o + 4 < size; p++) {
            const unsigned char c = static_cast< unsigned char >(*p);
            if (c >= 0x20 && c < 0x7f) {
                pOut[o++] = static_cast< char >(c);
            } else {
                o += static_cast< size_t >(snprintf(pOut + o, size - o, "~%02x", c));
            }
        }
        pOut[o] = 0;
    }

    void portSvList() {
        const int starts = MR::getStartPosNum();
        const int gposN = MR::getGeneralPosNum();
        const int zones = MR::getZoneNum();
        const TVec3f* pPlayer = MR::getPlayerPos();
        port_log_summary("survey: %s scenario %d: %d start entries, %d GeneralPos entries, %d zones; this scene's start: id %d zone %d; player at %.0f %.0f %.0f",
                         MR::getCurrentStageName(), static_cast< int >(MR::getCurrentScenarioNo()), starts, gposN, zones,
                         static_cast< int >(MR::getCurrentMarioStartIdInfo()._0), static_cast< int >(MR::getCurrentMarioStartIdInfo().mZoneID),
                         pPlayer != nullptr ? pPlayer->x : 0.0f, pPlayer != nullptr ? pPlayer->y : 0.0f, pPlayer != nullptr ? pPlayer->z : 0.0f);
        for (int z = 0; z < zones && z < 64; z++) {
            char name[96];
            portSvName(MR::getZoneNameFromZoneId(z), name, sizeof(name));
            port_log_summary("survey: zone %d = %s", z, name);
        }
        for (int i = 0; i < starts; i++) {
            int marioNo, zone, camera;
            f32 pos[3], dir[3];
            if (!MR::portSurveyStart(i, &marioNo, &zone, &camera, pos, dir)) {
                port_log_summary("survey: start %d of %d: not readable", i + 1, starts);
                continue;
            }
            char zname[96];
            portSvName(zone >= 0 ? MR::getZoneNameFromZoneId(zone) : "?", zname, sizeof(zname));
            port_log_summary("survey: start %d of %d: id %d zone %d (%s) camera %d at %.0f %.0f %.0f dir %.0f %.0f %.0f", i + 1, starts, marioNo, zone, zname,
                             camera, pos[0], pos[1], pos[2], dir[0], dir[1], dir[2]);
        }
        for (int i = 0; i < gposN && i < sSvGposMax; i++) {
            const char* pName = "";
            f32 pos[3];
            if (!MR::portSurveyGeneralPos(i, &pName, pos)) {
                continue;
            }
            char name[96];
            portSvName(pName, name, sizeof(name));
            port_log_summary("survey: gpos %d of %d: '%s' at %.0f %.0f %.0f", i + 1, gposN, name, pos[0], pos[1], pos[2]);
        }
        sSvStartNum = starts;
        sSvHopNum = starts + (sSvGposHops ? (gposN < sSvGposMax ? gposN : sSvGposMax) : 0);
        if (sSvHopNum > sSvHopMax) {
            sSvHopNum = sSvHopMax;
        }
    }
}  // namespace

extern "C" int port_start_override_active(void) {
    return sStartOverride;
}

extern "C" void port_start_override_note(int id, int zone, int found) {
    if (sStartNotes++ < 3) {  // (the camera and the zone lookups ask again: three lines are enough)
        port_log_summary("start: PETARI_STAGE asked for start id %d zone %d: %s", id, zone,
                         found ? "found, Mario starts there" : "NOT in this stage: the default start (id 0) is used");
    }
}

static void portStartOverrideSet(bool on) {
    sStartOverride = on ? 1 : 0;
    sStartNotes = 0;
}

static bool portSurveyBusy() {
    return sSvBusy;
}

// GameScene::update, every turn (game thread). playing: the action nerve and no demo.
static void portSurveyTurn(u32 gen, bool playing) {
    if (sSvMode < 0) {
        portSvRead();
    }
    if (sSvMode == 0) {
        return;
    }
    if (gen != sSvGen) {  // a new game scene
        sSvGen = gen;
        sSvListed = false;
        sSvPlayTurns = 0;
        sSvHopTurns = 0;
        sSvIdle = 0;
    }
    if (!playing) {
        if (sSvBusy && ++sSvIdle > 7200) {  // (a stage that never plays again: let the tour go on)
            sSvBusy = false;
            port_log_summary("survey: no play turn for 7200 turns: the hops are given up");
        }
        return;
    }
    sSvIdle = 0;
    sSvPlayTurns++;
    const char* stage = MR::getCurrentStageName();
    const int scenario = static_cast< int >(MR::getCurrentScenarioNo());
    if (!sSvListed) {
        if (sSvPlayTurns < 60) {  // (a second of play first: Mario has landed)
            return;
        }
        sSvListed = true;
        const bool sameStage = sSvScenario == scenario && strcmp(sSvStage, stage) == 0 && sSvHopIdx > 0 && sSvBusy;
        if (sameStage) {
            port_log_summary("survey: %s scenario %d restarted: the hops go on at hop %d", stage, scenario, sSvHopIdx + 1);
        } else {
            portSvList();
            snprintf(sSvStage, sizeof(sSvStage), "%s", stage);
            sSvScenario = scenario;
            sSvHopIdx = 0;
            sSvBusy = sSvMode == 2 && sSvHopNum > 0;
        }
        sSvHopTurns = 0;
        return;
    }
    if (sSvMode != 2 || !sSvBusy) {
        return;
    }
    sSvHopTurns++;
    const int hopTurns = static_cast< int >(sSvHopSec * 60.0f);
    if (sSvHopTurns == 90 && sSvHopIdx > 0) {
        const TVec3f* p = MR::getPlayerPos();
        port_log_summary("survey: hop %d settled: player at %.0f %.0f %.0f%s", sSvHopIdx, p->x, p->y, p->z, MR::isOnGroundPlayer() ? " (on the ground)" : " (NOT on the ground)");
    }
    if (sSvHopIdx > 0 && sSvHopTurns < hopTurns) {
        return;
    }
    if (sSvHopIdx >= sSvHopNum) {
        sSvBusy = false;
        port_log_summary("survey: %d hops done in %s scenario %d", sSvHopNum, stage, scenario);
        return;
    }
    // the next hop
    const int k = sSvHopIdx;
    f32 pos[3] = {0.0f, 0.0f, 0.0f};
    char what[160];
    if (k < sSvStartNum) {
        int marioNo, zone, camera;
        f32 dir[3];
        if (!MR::portSurveyStart(k, &marioNo, &zone, &camera, pos, dir)) {
            sSvHopIdx++;
            return;
        }
        snprintf(what, sizeof(what), "start entry %d: id %d zone %d camera %d", k + 1, marioNo, zone, camera);
    } else {
        const char* pName = "";
        if (!MR::portSurveyGeneralPos(k - sSvStartNum, &pName, pos)) {
            sSvHopIdx++;
            return;
        }
        char n[96];
        portSvName(pName, n, sizeof(n));
        snprintf(what, sizeof(what), "GeneralPos %d '%s'", k - sSvStartNum + 1, n);
    }
    port_log_summary("survey: hop %d of %d: %s at %.0f %.0f %.0f", k + 1, sSvHopNum, what, pos[0], pos[1], pos[2]);
    MR::setPlayerPos(TVec3f(pos[0], pos[1], pos[2]));
    sSvHopIdx++;
    sSvHopTurns = 0;
}
#endif

#ifdef TARGET_PC
// GXV_HRW (hrw26): the camera's view matrix now (3x4, row major) for the half-rate-world pair, and whether a pair may be re-projected at all: not in a demo, a movie
// or a scene change (the game's own cameras cut there), where the second picture is the first again.
extern "C" int port_hrw_camera(float out[12]) {
    // hrw29: the scene objects do not exist before the first scene is up, in a scene change's gap and in the title: MR::isDemoActive / getCameraViewMtx then read a null object (offset 0x10..0x5c: the console's
    // crash at FileSelect -> stage in c90 with GXV_HRW=3; the emulator reads zeros there). A pair is never opened without them.
    if (!MR::isExistSceneObj(SceneObj_DemoDirector) || !MR::isExistSceneObj(SceneObj_CameraContext)) return 0;
    if (MR::isDemoActive()) return 0;
    const TPos3f& view = MR::getCameraViewMtx();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = view.mMtx[r][c];
    return 1;
}
// GXV_HRW=5 (warp32): the player's centre in world space now (the point the follow camera keeps on the screen), for the warp's focus plane (GXV_HRW_WARP_DEPTH=2) and the
// "hrw: player steadiness" figures. 0 when there is no player (the same guards as the camera's, and the player's holder and actor).
extern "C" int port_hrw_player(float out[3]) {
    if (!MR::isExistSceneObj(SceneObj_DemoDirector) || !MR::isExistSceneObj(SceneObj_CameraContext) || !MR::isExistSceneObj(SceneObj_MarioHolder)) return 0;
    if (MR::isDemoActive() || !MarioAccess::getPlayerActor()) return 0;
    const TVec3f* p = MR::getPlayerCenterPos();
    out[0] = p->x;
    out[1] = p->y;
    out[2] = p->z;
    return 1;
}
// warp36 (14 Nov 2026; port/hrw36.h): what the warp needs beside his centre, in world space, now. out[0..2] his feet (the actor's position), out[3..5] his up vector
// (against his gravity, unit length), out[6..8] the ground point under him (Mario::mShadowPos: where his shadow falls, also while he is in the air: a jump does not
// lift the ground), out[9..11] the normal of the polygon his shadow falls on. Returns bits: 1 = the player (feet and up are valid), 2 = the ground point is valid (a
// polygon was found under him within 1500 units; without one the game puts the point 2500 units down his gravity), 4 = the polygon's normal is valid. 0: no player
// (the guards of port_hrw_player). Reads only; the game thread, where port_hrw_player is called.
// hold36 (15 Nov; port::hrw36::HoldGround has the rules): on a turn without ground (no shadow polygon, or its point out of range) the last turn's with ground is
// handed over for up to GXV_HRW36_HOLD_TURNS turns (default 45 = 1.5 s; 0 = never, the behaviour before): bits 2 | 8 (8 = held), out[6..8] the remembered point,
// out[3..5] his up vector ON THE TURN IT WAS SEEN (the plane's normal for GXV_HRW36_PLANE=1; the back end uses "up" for nothing else), out[9..11] and bit 4 the
// remembered polygon normal if it had one. Bit 16: no ground this turn although a shadow polygon exists (its point is out of the range): the log's reason.
// ground38 (15 Nov; the console's c174: "no shadow polygon" on 1049 pairs of a run, a quarter of all, 82 held): two more sources of ground.
//  - No shadow polygon (Mario's movement state _2 clear: Mario::calcShadowPos found nothing within 2400 units down his gravity, or the states were cleared: riding,
//    MarioActorRush) but he STANDS (state _1, or what he rides is bound to ground): the ground polygon and his feet. Bits 2 | 32, bit 4 with its normal.
//  - The hold takes ground that is a moving collision too (hold36 refused it): for 15 turns at most, the point carried along by that collision's matrix (the point
//    in the collision's own space is kept); bit 64 beside 8.
// "hrw: ground38" every 300 turns with pairs: the turns without a shadow polygon by his state, for the next console run.
extern "C" int port_hrw_player36(float out[12]) {
    static port::hrw36::HoldGround sHold;
    static const CollisionParts* sHoldParts = nullptr;  // (the moving collision of the remembered ground and the point in its space; read only while sHold has it)
    static float sHoldLocal[3] = {0.0f, 0.0f, 0.0f};
    static u32 sN = 0, sNoPoly = 0, sNoPolyRush = 0, sNoPolySwim = 0, sNoPolyStand = 0, sNoPolyJump = 0, sFeet = 0, sHeld = 0, sHeldMoving = 0, sWhy[port::hrw36::kHoldWhyCount] = {};
    if (!MR::isExistSceneObj(SceneObj_DemoDirector) || !MR::isExistSceneObj(SceneObj_CameraContext) || !MR::isExistSceneObj(SceneObj_MarioHolder)) { sHold.Forget(); return 0; }
    MarioActor* actor = MarioAccess::getPlayerActor();
    if (MR::isDemoActive() || !actor || !actor->mMario) { sHold.Forget(); return 0; }
    const TVec3f& pos = actor->mPosition;
    const TVec3f& grav = actor->getGravityVec();
    const float gl = std::sqrt(grav.x * grav.x + grav.y * grav.y + grav.z * grav.z);
    if (!(gl > 1.0e-4f)) { sHold.Forget(); return 0; }
    out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
    out[3] = -grav.x / gl; out[4] = -grav.y / gl; out[5] = -grav.z / gl;
    int bits = 1;
    for (int i = 6; i < 12; i++) out[i] = 0.0f;
    const float* polyN = nullptr;
    bool moving = false;
    const CollisionParts* movingParts = nullptr;
    if (actor->getMovementStates()._2) {
        const TVec3f* sh = actor->getShadowPos();
        const float down = -((sh->x - pos.x) * out[3] + (sh->y - pos.y) * out[4] + (sh->z - pos.z) * out[5]);  // (how far under his feet, along his gravity)
        if (down > -200.0f && down < 1500.0f) {
            out[6] = sh->x; out[7] = sh->y; out[8] = sh->z;
            bits |= 2;
            if (const Triangle* tri = MarioAccess::getShadowingPolygon()) {
                const TVec3f* fn = tri->getFaceNormal();
                if (fn) { out[9] = fn->x; out[10] = fn->y; out[11] = fn->z; bits |= 4; polyN = out + 9; }
                // (hold36: a collision that moved in the last 3 s (CollisionParts::updateMtx counts the turns its matrix stood still) is a lift, a turning planet:
                //  the ground remembered from it would stay behind)
                moving = tri->mParts == nullptr || tri->mParts->_D4 < 90;
                movingParts = tri->mParts;
            } else moving = true;
        } else bits |= 16;
    } else {
        // (ground38) no shadow polygon: the polygon he stands on, if he stands (MarioAccess::getGroundingPolygon's two cases, with their pointers checked)
        const Mario* mario = actor->mMario;
        const Triangle* tri = nullptr;
        if (actor->_934) {
            const LiveActor* host = actor->_924 ? actor->_924->mHost : nullptr;
            if (host && host->mBinder && MR::isOnGround(host)) tri = &host->mBinder->mGroundInfo.mParentTriangle;
        } else if (mario->getMovementStates()._1) tri = mario->mGroundPolygon;
        ++sNoPoly;
        ++(actor->_934 ? sNoPolyRush : mario->isSwimming() ? sNoPolySwim : mario->getMovementStates()._1 ? sNoPolyStand : mario->getMovementStates().jumping ? sNoPolyJump : sWhy[0]);
        if (tri && tri->isValid() && tri->mParts) {
            out[6] = pos.x; out[7] = pos.y; out[8] = pos.z;
            bits |= 2 | 32;
            ++sFeet;
            const TVec3f* fn = tri->getFaceNormal();
            const float dt = fn ? fn->x * out[3] + fn->y * out[4] + fn->z * out[5] : 0.0f;
            if (fn && dt > 0.5f) { out[9] = fn->x; out[10] = fn->y; out[11] = fn->z; bits |= 4; polyN = out + 9; }  // (a floor to him, not a wall he touches)
            moving = tri->mParts->_D4 < 90;
            movingParts = tri->mParts;
        }
    }
    // (the scene, the stage and the scenario in one number: the scene's count, the scenario, the stage name's characters)
    static const int holdTurns = [] { const char* e = getenv("GXV_HRW36_HOLD_TURNS"); const int v = e && *e ? atoi(e) : 45; return v < 0 ? 0 : v > 600 ? 600 : v; }();
    u32 key = sHrw36Scene * 2654435761u + static_cast< u32 >(MR::getCurrentScenarioNo()) * 40503u;
    if (const char* name = MR::getCurrentStageName())
        for (int i = 0; i < 48 && name[i]; i++) key = key * 31u + static_cast< u8 >(name[i]);
    const float feetNow[3] = {out[0], out[1], out[2]}, upNow[3] = {out[3], out[4], out[5]}, gNow[3] = {out[6], out[7], out[8]};
    const float pn[3] = {out[9], out[10], out[11]};
    // (ground38: where the moving collision of the remembered ground has its point now. The pointer is read only in the scene it was taken in (the key) and while the
    //  collision is in use; collisions live as long as their scene)
    float gMoved[3];
    bool haveMoved = false;
    if (!(bits & 2) && sHold.have && sHold.movingGround && sHold.key == key && sHoldParts && sHoldParts->_CC) {
        const TPos3f& m = sHoldParts->mBaseMatrix;
        for (int r = 0; r < 3; r++) gMoved[r] = m.mMtx[r][0] * sHoldLocal[0] + m.mMtx[r][1] * sHoldLocal[1] + m.mMtx[r][2] * sHoldLocal[2] + m.mMtx[r][3];
        haveMoved = true;
    }
    const int r = sHold.Step(sHrw36Turn, key, feetNow, upNow, (bits & 2) != 0, gNow, polyN ? pn : nullptr, moving, !actor->isEnableNerveChange(), holdTurns, haveMoved ? gMoved : nullptr);
    if ((bits & 2) && sHold.have) {  // (remembered this turn)
        sHoldParts = moving ? movingParts : nullptr;
        if (sHoldParts) {
            const TPos3f& m = sHoldParts->mInvBaseMatrix;
            for (int k = 0; k < 3; k++) sHoldLocal[k] = m.mMtx[k][0] * gNow[0] + m.mMtx[k][1] * gNow[1] + m.mMtx[k][2] * gNow[2] + m.mMtx[k][3];
        }
    }
    if (r == port::hrw36::kHoldHeld) {
        for (int i = 0; i < 3; i++) { out[3 + i] = sHold.up[i]; out[6 + i] = sHold.g[i]; out[9 + i] = sHold.poly ? sHold.n[i] : 0.0f; }
        bits |= 2 | 8 | (sHold.poly ? 4 : 0) | (sHold.movingGround ? 64 : 0);
        ++sHeld;
        if (sHold.movingGround) ++sHeldMoving;
    } else if (!(bits & 2) && sHold.why > 0 && sHold.why < port::hrw36::kHoldWhyCount) ++sWhy[sHold.why];
    if (++sN >= 300) {
        using namespace port::hrw36;
        port_log_summary("hrw: ground38 | %u turns with a pair: no shadow polygon %u (riding %u, swimming %u, standing %u, jumping %u, other %u), of them on his ground polygon %u | "
                         "held %u (moving ground %u), not held: nothing remembered %u, expired %u, gravity turned %u, out of range %u, moved away %u, warp %u, scene %u, dead %u, gap %u, off %u",
                         sN, sNoPoly, sNoPolyRush, sNoPolySwim, sNoPolyStand, sNoPolyJump, sWhy[0], sFeet, sHeld, sHeldMoving, sWhy[kHoldWhyNothing], sWhy[kHoldWhyExpired], sWhy[kHoldWhyGravity],
                         sWhy[kHoldWhyRange], sWhy[kHoldWhyMoving], sWhy[kHoldWhyTeleport], sWhy[kHoldWhyScene], sWhy[kHoldWhyDead], sWhy[kHoldWhyGap], sWhy[kHoldWhyOff]);
        sN = sNoPoly = sNoPolyRush = sNoPolySwim = sNoPolyStand = sNoPolyJump = sFeet = sHeld = sHeldMoving = 0;
        for (u32& w : sWhy) w = 0;
    }
    return bits;
}
#endif

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------
// fastboot32 (13 Nov): PETARI_BOOT, a test-only start that skips the title, the file select and the hub. Unset (or 0) = off: the only
// cost is one cached test when the logos end, one a game turn, and one per save request.
//
//   PETARI_BOOT=<Galaxy>[:scenario][#id[/zone]][@where]   (the syntax of one PETARI_STAGE entry)
//       When the logo scene ends (GameSequenceProgress::endScene), save file PETARI_BOOT_FILE (1..6; unset = the first file that exists
//       in the save: the file select's planets are files 1 2 4 6 5 3, the test save's file is number 5) is loaded as the file
//       select's "Play This File" does (the file's last player, Mario or Luigi) and the galaxy is asked for at once with
//       MR::requestChangeStageInGameMoving: the first game scene of the run is that galaxy. PETARI_STAGE / PETARI_FLOW, if also set,
//       then work from that scene as they do from the hub.
//   PETARI_BOOT=1 (or "flow" / "stage")
//       with PETARI_FLOW set: its first entry is asked for at the end of the logos through the star select, as a player enters (the
//       flow goes on with entry 2 after PETARI_FLOW_HOLD turns); else with PETARI_STAGE set: its first entry, directly, and the tour
//       goes on with the second.
//   No such file in the save, a file that does not load, or nothing to enter: one "boot:" line and the normal title.
//   The save is NEVER written while PETARI_BOOT is on: every save request ends at once as the game's own "continue without saving"
//   mode does (SaveDataHandleSequence::tryNoSave: no question, no write, "saved" reported to the caller), with a "boot:" line.
//   PETARI_BOOT_HOLD=<turns>: "boot: done: ..." after that many turns of play in the first scene (a line for the run script's UNTIL;
//   nothing else happens). Unset = no such line.
//   Log: "boot: PETARI_BOOT: ..." at the request, "boot: play began in ..." at the first turn of play (the time to judge a run by).
// ---------------------------------------------------------------------------------------------------------------------------------
namespace {
    enum { cBootUnread = -1, cBootOff = 0, cBootEntry = 1, cBootStageFirst = 2, cBootFlowFirst = 3 };
    int sBootMode = cBootUnread;
    int sBootFile = 1;
    const char* sBootEntry = nullptr;
    char sBootStage[48];
    int sBootState = 0;  // 0 nothing asked, 1 asked (waiting for the first turn of play), 2 play began (counting to the hold), 3 over
    int sBootHold = 0;
    u32 sBootPlay = 0;
    s64 sBootAskNs = 0;
    u32 sBootTurns = 0;
    u32 sBootSaveSkips = 0;

    void portBootRead() {
        sBootMode = cBootOff;
        const char* e = getenv("PETARI_BOOT");
        if (e == nullptr || *e == 0 || strcmp(e, "0") == 0) {
            return;
        }
        const char* f = getenv("PETARI_BOOT_FILE");
        const int file = f != nullptr ? atoi(f) : 0;
        sBootFile = file >= 1 && file <= 6 ? file : -1;  // (-1: the first file of the save that exists; the test save's only file is number 5)
        const char* h = getenv("PETARI_BOOT_HOLD");
        sBootHold = h != nullptr && atoi(h) > 0 ? atoi(h) : 0;
        if (strcmp(e, "1") == 0 || strcmp(e, "flow") == 0 || strcmp(e, "stage") == 0) {
            const char* stage = getenv("PETARI_STAGE");
            if (strcmp(e, "stage") != 0 && portFlowActive()) {
                sBootMode = cBootFlowFirst;
            } else if (stage != nullptr && *stage != 0 && *stage != ',') {
                sBootMode = cBootStageFirst;
                sBootEntry = stage;
            } else {
                port_log_summary("boot: PETARI_BOOT=%s needs PETARI_FLOW or PETARI_STAGE (or name the galaxy: PETARI_BOOT=Galaxy:scenario): off", e);
            }
            return;
        }
        sBootMode = cBootEntry;
        sBootEntry = e;
    }
}  // namespace

// GameSequenceProgress::endScene (the logos ended; game thread): 0 = PETARI_BOOT is off, 1..6 = the save file asked for, -1 = the first
// file that exists. port_boot_file_found: the file that was loaded (for the log lines).
extern "C" void port_boot_file_found(int file) {
    sBootFile = file;
}
extern "C" int port_boot_file(void) {
    if (sBootMode == cBootUnread) {
        portBootRead();
    }
    return sBootMode != cBootOff ? sBootFile : 0;
}

// SaveDataHandleSequence (game thread): 1 = never write the save (PETARI_BOOT is on). note: this call is a save request being dropped.
extern "C" int port_boot_nosave(int note) {
    // (14 Nov) PETARI_TEST_LIVES (GameDataPlayerStatus.cpp: a route reaches the Game Over screen at its first miss) never writes the save either:
    // the Game Over screen offers "Save and Quit", and a test must not touch the owner's save.
    static const bool sTestLives = [] { const char* e = getenv("PETARI_TEST_LIVES"); return e && *e; }();
    if (sTestLives) {
        if (note) port_log_summary("boot: a save request was dropped, nothing written (PETARI_TEST_LIVES never writes the save)");
        return 1;
    }
    if (sBootMode == cBootUnread) {
        portBootRead();
    }
    if (sBootMode == cBootOff) {
        return 0;
    }
    if (note) {
        sBootSaveSkips++;
        port_log_summary("boot: a save request was dropped, nothing written (PETARI_BOOT never writes the save; %u so far)", sBootSaveSkips);
    }
    return 1;
}

// GameSequenceProgress::endScene, after the save file was loaded: ask for the galaxy. 0 = nothing asked (the caller goes to the hub).
extern "C" int port_boot_request(void) {
    if (port_boot_file() == 0 || sBootState != 0) {
        return 0;
    }
    if (mp_active()) {  // MP3: never act inside a multiplayer session (it cannot be up at the logo's end; belt and braces)
        port_log_summary("boot: PETARI_BOOT: not acted on, a multiplayer session is active");
        return 0;
    }
    const char* from = nullptr;
    if (sBootMode == cBootFlowFirst) {
        if (!portFlowActive() || sFlowLegs[0].mStar <= 0) {
            port_log_summary("boot: PETARI_BOOT: the first PETARI_FLOW entry is not a star to choose: the hub first, as \"Play This File\"");
            return 0;
        }
        from = sFlowLegs[0].mStage;
    } else {
        from = sBootEntry;
    }
    size_t n = 0;
    while (from[n] != 0 && from[n] != ':' && from[n] != ',' && from[n] != '#' && from[n] != '@' && n < sizeof(sBootStage) - 1) {
        sBootStage[n] = from[n];
        n++;
    }
    sBootStage[n] = 0;
    if (n == 0) {
        port_log_summary("boot: PETARI_BOOT: no galaxy name: the hub first, as \"Play This File\"");
        return 0;
    }
    sBootAskNs = port_host_time_ns();
    sBootTurns = 0;
    sBootState = 1;
    if (sBootMode == cBootFlowFirst) {
        port_log_summary("boot: PETARI_BOOT: save file %d loaded at the end of the logos; no title, no file select, no hub: the star select of %s "
                         "(PETARI_FLOW entry 1, star %d)",
                         sBootFile, sBootStage, static_cast< int >(sFlowLegs[0].mStar));
        sFlowIndex = 1;
        sFlowCur = 0;
        sFlowAsks = 0;
        portFlowAsk(true);
    } else {
        port_log_summary("boot: PETARI_BOOT: save file %d loaded at the end of the logos; no title, no file select, no hub: straight into %s%s",
                         sBootFile, sBootStage, sBootMode == cBootStageFirst ? " (PETARI_STAGE entry 1)" : "");
        if (sBootMode == cBootStageFirst) {
            sPortTourIndex = 1;  // the tour goes on with the second entry
        }
        if (!portTourEnter(sBootEntry, sBootMode == cBootStageFirst ? 1 : 0)) {
            sBootState = 3;
            return 0;  // Let the regular sequence enter the hub.
        }
    }
    return 1;
}

// GameScene::update, every turn (game thread). playing: the action nerve and no demo.
static void portBootTurn(bool playing) {
    if (sBootState != 1) {
        if (sBootState == 2 && playing && ++sBootPlay >= static_cast< u32 >(sBootHold)) {
            sBootState = 3;
            port_log_summary("boot: done: %u turns of play in %s (PETARI_BOOT_HOLD)", sBootPlay, MR::getCurrentStageName());
            port_log_flush();
        }
        return;
    }
    sBootTurns++;
    if (!playing) {
        return;
    }
    sBootState = sBootHold > 0 ? 2 : 3;
    sBootPlay = 0;
    port_boot_play_began();
    const f64 sec = (port_host_time_ns() - sBootAskNs) / 1e9;
    if (MR::isEqualStageName(sBootStage)) {
        port_log_summary("boot: play began in %s scenario %d, %.1f s after the logos ended (%u turns of this scene before it; PETARI_BOOT; "
                         "save requests dropped so far: %u)",
                         MR::getCurrentStageName(), static_cast< int >(MR::getCurrentScenarioNo()), sec, sBootTurns, sBootSaveSkips);
    } else {
        port_log_summary("boot: play began in %s scenario %d, NOT the galaxy asked for (%s), %.1f s after the logos ended", MR::getCurrentStageName(),
                         static_cast< int >(MR::getCurrentScenarioNo()), sBootStage, sec);
    }
}
#endif
