#include <new>
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Effect/EffectSystem.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/NameObj/NameObjListExecutor.hpp"
#include "Game/Scene/SceneDataInitializer.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"

#ifdef TARGET_PC
#include "port/compat.h"  // (PORT_GX_MARK_BGCAT, port_gx_marker: bglayer25)
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "port/diagnostics.h"
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF=3, game24: every category list timed whole)
#if defined(__vita__) && PORT_LOGGING
extern "C" u32 port_workload_anim_category_begin(unsigned domain, unsigned category);
#endif
#ifdef TARGET_PC
extern "C" void port_anim_lod_set_category(int type);
extern "C" void port_workload_anim_category_end(u32 token);
#endif
#endif

#ifdef TARGET_PC
// GXV_DRAW_PROF (draw7; platform/src/port/perf.cpp): each draw list's outermost call timed when asked for (a constant's test otherwise).
extern "C" int port_draw_prof_on(void);
extern "C" void port_draw_prof_begin(void);
extern "C" void port_draw_prof_end(int row);
#include "port/stream_census.h"  // (GXV_STREAM_CENSUS: each draw list of a counted picture is a source)
#include "port/heap_routing.h"  // (port_host_alloc for pipefix26 below: the desktop build has no implicit declaration)
#define PORT_DRAW_PROF(row, call)                                \
    do {                                                         \
        if (__builtin_expect(port_sc_turn != 0, 0))              \
            port_stream_census_src_begin(row);                   \
        if (port_draw_prof_on()) {                               \
            port_draw_prof_begin();                              \
            call;                                                \
            port_draw_prof_end(row);                             \
        } else {                                                 \
            call;                                                \
        }                                                        \
        if (__builtin_expect(port_sc_turn != 0, 0))              \
            port_stream_census_src_end();                        \
    } while (0)
#else
#define PORT_DRAW_PROF(row, call) call
#endif

namespace {
    SceneDataInitializer* getSceneDataInitializer() {
        return MR::getSceneObj< SceneDataInitializer >(SceneObj_SceneDataInitializer);
    }
};  // namespace

void SceneFunction::startStageFileLoad() {
    MR::createSceneObj(SceneObj_SceneDataInitializer);
    ::getSceneDataInitializer()->startStageFileLoad();
}

void SceneFunction::waitDoneStageFileLoad() {
    ::getSceneDataInitializer()->waitDoneStageFileLoad();
}

void SceneFunction::startActorFileLoadCommon() {
    ::getSceneDataInitializer()->startActorFileLoadCommon();
}

void SceneFunction::startActorFileLoadScenario() {
    ::getSceneDataInitializer()->startActorFileLoadScenario();
}

void SceneFunction::startActorPlacement() {
    ::getSceneDataInitializer()->startActorPlacement();
}

void SceneFunction::initAfterScenarioSelected() {
    ::getSceneDataInitializer()->startStageFileLoadAfterScenarioSelected();
    ::getSceneDataInitializer()->initAfterScenarioSelected();
}

void SceneFunction::initForNameObj() {
    MR::createSceneObj(SceneObj_NameObjExecuteHolder);
    MR::createSceneObj(SceneObj_StopSceneController);
    MR::createSceneObj(SceneObj_SceneNameObjMovementController);
}

void SceneFunction::initForLiveActor() {
    MR::createSceneObj(SceneObj_AllLiveActorGroup);
    MR::createSceneObj(SceneObj_ClippingDirector);
    MR::createSceneObj(SceneObj_DemoDirector);
    MR::createSceneObj(SceneObj_SensorHitChecker);
    MR::createSceneObj(SceneObj_CollisionDirector);
    MR::createSceneObj(SceneObj_MessageSensorHolder);
    MR::createSceneObj(SceneObj_LiveActorGroupArray);
    MR::createSceneObj(SceneObj_MovementOnOffGroupHolder);
    MR::createSceneObj(SceneObj_LightDirector);
    MR::createSceneObj(SceneObj_AreaObjContainer);
    MR::createSceneObj(SceneObj_CaptureScreenActor);
    MR::createSceneObj(SceneObj_StageSwitchContainer);
    MR::createSceneObj(SceneObj_SwitchWatcherHolder);
    MR::createSceneObj(SceneObj_SleepControllerHolder);
    MR::createSceneObj(SceneObj_TalkDirector);
    MR::createSceneObj(SceneObj_NPCDirector);
}

void SceneFunction::initEffectSystem(u32 a1, u32 a2) {
    MR::createSceneObj(SceneObj_EffectSystem);
    MR::getEffectSystem()->entry(MR::getParticleResourceHolder(), a1, a2);
}

void SceneFunction::allocateDrawBufferActorList() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    pListExecutor->allocateDrawBufferActorList();
    MR::initConnectting();
}

void CategoryList::execute(MR::MovementType type) {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 animToken = port_workload_anim_category_begin(1, static_cast<unsigned>(type));
#endif
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

#ifdef TARGET_PC
    port_anim_lod_set_category(0x100 | static_cast< int >(type));  // (GXV_ANIM_PROF's movement rows; no calc-anim type: no deferral)
    ::port::LogicCatScope logicCat(0x100 | static_cast< int >(type));  // (GXV_LOGIC_PROF=3: the list timed whole, every turn)
    pListExecutor->executeMovement(type);
    port_anim_lod_set_category(-1);
#else
    pListExecutor->executeMovement(type);
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_category_end(animToken);
#endif
}

void CategoryList::execute(MR::CalcAnimType type) {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 animToken = port_workload_anim_category_begin(0, static_cast<unsigned>(type));
#endif
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

#ifdef TARGET_PC
    port_anim_lod_set_category(static_cast< int >(type));  // (GXV_ANIM_LOD: LiveActor::calcAnim)
    ::port::LogicCatScope logicCat(static_cast< int >(type));  // (GXV_LOGIC_PROF=3)
    pListExecutor->executeCalcAnim(type);
    port_anim_lod_set_category(-1);
#else
    pListExecutor->executeCalcAnim(type);
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_category_end(animToken);
#endif
}

#ifdef TARGET_PC
// bglayer25 (GXV_BGLAYER on, GXV_DRAWPIPE=0): the draws of one draw category are bracketed by PORT_GX_MARK_BGCAT markers (1 + the buffer type, 64 + the draw type; 0 closes),
// so that the front end can count draws and indices by category (vgx_bglayer.inc). Nothing is written with the switch off: one load of a cached int.
extern "C" int port_bglayer_marks_on(void);
#define PORT_BG_CAT(id, call)                                          \
    do {                                                               \
        if (__builtin_expect(port_bglayer_marks_on() != 0, 0)) {       \
            port_gx_marker(PORT_GX_MARK_BGCAT | (static_cast< u32 >(id) << 8)); \
            call;                                                      \
            port_gx_marker(PORT_GX_MARK_BGCAT);                        \
        } else {                                                       \
            call;                                                      \
        }                                                              \
    } while (0)
#else
#define PORT_BG_CAT(id, call) call
#endif

void CategoryList::execute(MR::DrawType type) {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_BG_CAT(64 + static_cast< int >(type), PORT_DRAW_PROF(static_cast< int >(type), pListExecutor->executeDraw(type)));
}

void CategoryList::entryDrawBuffer2D() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_DRAW_PROF(0x300, pListExecutor->entryDrawBuffer2D());
}

void CategoryList::entryDrawBuffer3D() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_DRAW_PROF(0x301, pListExecutor->entryDrawBuffer3D());
}

#ifdef TARGET_PC
void CategoryList::entryDrawBufferPlayer() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    pListExecutor->entryDrawBufferPlayer();
}

void CategoryList::entryDrawBuffer3DExceptPlayer() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    pListExecutor->entryDrawBuffer3DExceptPlayer();
}
#endif

void CategoryList::entryDrawBufferMirror() {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_DRAW_PROF(0x302, pListExecutor->entryDrawBufferMirror());
}

void CategoryList::drawOpa(MR::DrawBufferType type) {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_BG_CAT(1 + static_cast< int >(type), PORT_DRAW_PROF(0x100 | static_cast< int >(type), pListExecutor->drawOpa(type)));
}

void CategoryList::drawXlu(MR::DrawBufferType type) {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_BG_CAT(1 + static_cast< int >(type), PORT_DRAW_PROF(0x200 | static_cast< int >(type), pListExecutor->drawXlu(type)));
}

void CategoryList::requestMovementOn(MR::MovementType type) {
    MR::requestMovementOnWithCategory(type);
}

void CategoryList::requestMovementOff(MR::MovementType type) {
    MR::requestMovementOffWithCategory(type);
}

void SceneFunction::createHioBasicNode(Scene*) {
}

#ifdef TARGET_PC
// ================================================================================================ GXV_DRAWPIPE (drawpipe24, 10 Nov 2026)
// The game's side of port/draw_pipe.h's driver: the four points of a drawn turn (GameSystem::frameLoop calls port_drawpipe_point:
// 0 after the flushes, 1 after the draw, 2 after update(), 3 after calcAnim()), on the game thread. The scene's draw buffers are
// frozen / walked through DrawBufferHolder's portPipe* functions; nothing here writes game state.
// dpengine (11 Nov): also the small state frozen with the lists (Game/System/PortDrawPipe.hpp), the turn's decision (the fall-back
// states of the design, each a test at the hand-over), and the game's side of the draw thread (port/draw_thread.h: modes 3 and 4).
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Camera/CameraPoseParam.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/Map/LightDirector.hpp"
#include "Game/NameObj/NameObjCategoryList.hpp"
#include "Game/Scene/Scene.hpp"
#include "Game/Screen/CaptureScreenDirector.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/System/DrawBufferHolder.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/PortDrawPipe.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include "port/draw_pipe.h"
#include "port/lockstep.h"  // (fmt32)
#include "port/draw_thread.h"
#include "port/gx_fifo.h"

// bglayer25 (GXV_BGLAYER on): GameScene::draw3D hands the picture's camera to the census (DrawBufferHolder.cpp port_bglayer_picture).
extern "C" void port_bglayer_picture(const void* pHolder, const float* view, const float* cam, const float* player, float fovy, int demo);
extern "C" int port_bglayer_mode(void);
extern "C" int port_safe_cam_exists(void);
extern "C" const TVec3f* port_safe_player_pos(void);
extern "C" int port_bglayer_scene_picture(int disallow) {
    if (port_safe_cam_exists() == 0 || MR::getCameraDirector() == nullptr || MR::getCameraDirector()->mPoseParam1 == nullptr) {
        return 0;
    }
    const TPos3f& v = MR::getCameraViewMtx();
    float view[12];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            view[r * 4 + c] = v.mMtx[r][c];
        }
    }
    const TVec3f cam = MR::getCamPos();
    const float camv[3] = {cam.x, cam.y, cam.z};
    const TVec3f* pl = port_safe_player_pos();
    const float plv[3] = {pl != nullptr ? pl->x : 0.0f, pl != nullptr ? pl->y : 0.0f, pl != nullptr ? pl->z : 0.0f};
    NameObjListExecutor* pExec = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();
    const void* holder = pExec != nullptr ? pExec->mBufferHolder : nullptr;
    const float fovy = MR::getCameraDirector()->mPoseParam1->mFovy;
    const int mode = port_bglayer_mode();
    if ((mode & 2) != 0) {
        port_bglayer_picture(holder, view, camv, pl != nullptr ? plv : nullptr, fovy, MR::isDemoActive() ? 1 : 0);
    }
    return 0;
}
extern "C" void port_drawpipe_point(int point);
extern "C" int port_drawpipe_mode(void);    // (platform/src/port/perf.cpp)
extern "C" int port_drawpipe_sample(void);
extern "C" int port_drawpipe_core(void);
extern "C" int port_drawpipe_prio(void);
extern "C" int port_drawpipe_watchdog_ms(void);
extern "C" int port_drawpipe_slice_us(void);
extern "C" int port_drawpipe_settle(void);
extern "C" int port_drawpipe_stack_kb(void);
extern "C" int port_drawpipe_always(void);
extern "C" int port_drawpipe_loadcheck(void);
extern "C" int port_drawpipe_trap(void);
// (GX-HOOKS.diff's proposal for the capture layer: set for the length of an overlap, it makes every hooked GX entry ask
// port_drawpipe_game_enter while the draw thread replays. Weak: nothing without it.)
extern "C" int __PortGXCapTrap __attribute__((weak));
extern "C" void port_gx_replay_arm(uintptr_t stackLo, uintptr_t stackSpan) __attribute__((weak));  // (gx_capture.cpp)
extern "C" int port_gx_replay_arm_trap(uintptr_t helperLo, uintptr_t helperSpan) __attribute__((weak));  // (gx_capture.cpp: the late draw's owner trap, three-way)
extern "C" const char* port_gx_trap_selftest(uintptr_t helperLo, uintptr_t helperSpan) __attribute__((weak));
extern "C" void* port_gx_trap_probe_start(void) __attribute__((weak));
extern "C" int port_gx_trap_probe_wait(void* probe, uint32_t ms, int64_t* returnedAtNs) __attribute__((weak));
extern "C" void port_gx_trap_stats(uint32_t* foreign, uint32_t* gameEntries, uint32_t* stuck, uint32_t* runaway) __attribute__((weak));
extern "C" void port_frame_thread_lend(int on);
extern "C" int port_on_frame_thread(void);  // (platform/src/port/perf.cpp)
extern "C" int port_gx_capture_mode(void) __attribute__((weak));  // (GXV_GXCAPTURE: 0 off, 1 on, 2 verify)
extern "C" void __PortGXFifoFlush(void);  // (platform/src/gx/gx_fifo.cpp: publishes what the stream holds; sends nothing)
extern "C" int64_t port_host_time_ns(void);
extern "C" void port_log_summary(const char* fmt, ...);
extern "C" int port_gx_drawdone_defer_mode(void);                 // (platform/src/port/drawdone_defer.cpp)
extern "C" int port_drawpipe_scene_flags(const void* pScene, u32* pFlags);  // (GameScene.cpp)
extern "C" void port_os_lend_main(int on) __attribute__((weak));  // (platform/src/os/os_thread.cpp)
extern "C" unsigned port_os_other_guest_runs(void) __attribute__((weak));
extern "C" void* port_os_other_guest_last(int* priority, void** stackBase) __attribute__((weak));
extern "C" int port_os_other_guest_take(int i, void** thread, void** func, int* priority, unsigned* runs) __attribute__((weak));  // (pipe61: os_thread.cpp)
extern "C" volatile uintptr_t gPortPipeStackLo, gPortPipeStackSpan;
// GXV_PIPE25 (pipe27: the late draw; port/draw_thread.h class Late, port/pipe25.h, platform/src/gx/gx_fifo.cpp's raw FIFO)
extern "C" int port_pipe25_active(void);  // (platform/src/port/perf.cpp: GXV_PIPE25 when GXV_DRAWPIPE is unset, else 0)
extern "C" u32 port_light_dir_fixed_take(void);  // (hazard32; decomp/src/Game/Map/LightFunction.cpp)
extern "C" int port_pipe25_mode(void);
extern "C" int port_pipe25_joinlate(void);  // (pipe61: platform/src/port/perf.cpp, GXV_PIPE25_JOINLATE, default 0)
extern "C" int port_pipe25_core(void);
extern "C" int port_pipe25_prio(void);
extern "C" int port_pipe25_takeover_us(void);
extern "C" int port_pipe25_stack_kb(void);
extern "C" int port_pipe25_scratch_kb(void);
extern "C" int port_gx_raw_open(void);
extern "C" int port_gx_raw_token(void);
extern "C" uint32_t port_gx_raw_close(void);
extern "C" uint64_t port_gx_raw_bytes(void);
extern "C" uint64_t port_gx_raw_high(void);
extern "C" uint32_t port_gx_raw_splice_begin(void);
extern "C" void port_gx_raw_splice_step(uint32_t i, void (*token)(uint32_t id, void* ctx), void* ctx);
extern "C" void port_gx_raw_splice_end(void);
extern "C" int port_gx_raw_abandon(void (*token)(uint32_t id, void* ctx), void* ctx);
extern "C" int port_gx_scratch_begin(uint8_t* buf, uint32_t cap);
extern "C" uint32_t port_gx_scratch_end(void);
extern "C" uint32_t port_gx_seg_flips(void);
extern "C" uint32_t __PortGXPipeBytes(void);  // (decomp/src/RVL_SDK/gx/GXDisplayList.c)
extern "C" void __PortGXPipeSave(void* out);
extern "C" void __PortGXPipeLoad(const void* in);
extern "C" void __PortGXPipeFirewall(void);
extern "C" void __PortGXPipeShadowDrop(void);
extern "C" int __PortGXCap __attribute__((weak));
extern "C" int __PortGXCapPass __attribute__((weak));  // (gx_capture.cpp: raised for the length of an overlap, see PortLateGame::OverlapBegin)
// The GX capture-and-replay layer (platform/include/port/gx_capture.h; platform/src/gx is another engineer's). Weak: a build
// without the layer links, and GXV_DRAWPIPE=4 then draws every turn serially ("configuration" in the "drawpipe:" line).
struct PortGxCapture;
extern "C" bool port_gx_capture_begin(void) __attribute__((weak));
extern "C" void port_gx_capture_token(uint32_t id) __attribute__((weak));
extern "C" PortGxCapture* port_gx_capture_end(void) __attribute__((weak));
extern "C" void port_gx_replay(PortGxCapture* c, void (*token)(uint32_t id, void* ctx), void* ctx) __attribute__((weak));
// (the layer's additions the engine uses: the handler that executes a token when the layer does it itself, at an abandon and
// after one; a token whose pass keeps viewport / projection / scissor; the engine's own abandon)
extern "C" void port_gx_capture_set_token_handler(void (*token)(uint32_t id, void* ctx), void* ctx) __attribute__((weak));
extern "C" void port_gx_capture_token_keep(uint32_t id, uint32_t keep) __attribute__((weak));
extern "C" void port_gx_capture_abandon(const char* why) __attribute__((weak));
extern "C" bool port_gx_capture_open(void) __attribute__((weak));
// (the layer's totals since the start, port/gx_capture.h: the same fields, declared here because the header is the layer's and
// this file binds the layer weakly)
struct PortGxCaptureStats {
    uint64_t begun, refused, ended, abandoned, verified, replayed;
    uint64_t calls, rawBytes, recordBytes, tokens, getters;
    uint64_t verifyDiffs, foreign, contractErrors;
    uint64_t captureNs, replayNs;
};
extern "C" void port_gx_capture_stats(PortGxCaptureStats* out) __attribute__((weak));

namespace {
    port::drawpipe::Driver sPortPipeDriver;
    port::drawpipe::Pipe& sPortPipe = *new port::drawpipe::Pipe;  // (never destroyed: its thread waits on it until the process ends)
    PortPipeScene sPortPipeSceneStore;  // the small state of the frame in use
    static bool sPassDown = false;  // (pipefix31 GXV_PIPE25_SEE)
    // ---- GXV_PIPE25 (pipe27): the late draw's state. Written by the game thread while the draw is made, read by the runner (helper, or the
    // game thread at a take-over) between the hand-over and the join; the dispatch and the done semaphore order the two.
    port::drawpipe::Late* sLate = nullptr;  // (made at the first drawn turn with GXV_PIPE25 on; never destroyed: its thread waits on it)
    enum { kLateMaxTokens = 512 };
    struct PortLateState {
        bool verify = false, dropped = false, pending = false, broken = false, scratchSkip = false;
        uint32_t words = 0, n = 0;
        port::pipe25::HVec< u32 > tokId;                              // pass i's token (the portPipe token index)
        port::pipe25::HVec< port::pipe25::HVec< uint32_t > > snap, end;      // pools: GXData at the token, and as the pass left it (n of them are in use)
        port::pipe25::HVec< port::pipe25::HVec< uint32_t > > post;           // revB, verify: GXData right after the pass in place (compared with end[i] of the re-run)
        port::pipe25::HVec< uint32_t > fin;                           // GXData after the open lists, at the hand-over
        port::pipe25::PassStore store;                         // verify: the passes as the game thread made them in place
        port::pipe25::PassRec cur;                             // verify: the pass in place now
        uint8_t* startPtr = nullptr;
        uint32_t startFlips = 0;
        int64_t startNs = 0;
        uint8_t* scratch = nullptr;                            // verify: the buffer a pass is re-run into
        uint32_t scratchCap = 0;
        uint32_t noRoomRun = 0, lastMarks = 0;
    };
    PortLateState sLateState;
    bool portPipeOverlapped() { return sPortPipe.Overlapped() || (sLate != nullptr && sLate->Overlapped()); }
    u32 sPortPipeTurn = 0;              // drawn turns with the switch on
    // (the decision's memory: the drawn turn of the last form switch, fur draw, GX use by the logic; the qualifying turns in a row)
    u32 sPortPipeFormTurn = 0, sPortPipeLogicGxTurn = 0, sPortPipeOkRun = 0;
    bool sPortPipeFormSeen = false, sPortPipeLogicGxSeen = false;
    unsigned sPortPipeGuestRuns = 0;
    int sPortPipeWhy = 0;   // this turn's decision (port_drawpipe_point(0)) and every reason that held
    u32 sPortPipeHeld = 0;
    // (pipe61, GXV_PIPE25_JOINLATE) the join is made after calcAnim(): sPortLateInCalc from the end of update() to that join; a GX call / matrix-half / list
    // write by the logic in that span (calcAnim's) joins early as ever, but does not make the next turns serial, and the third one of the session takes the
    // switch back to the early join (sPortLateJoinLateOff)
    bool sPortLateInCalc = false, sPortLateJoinLateOff = false;
    u32 sPortLateCalcEarly = 0, sPortLateJoinedLate = 0, sPortLateJoinedLateNs = 0, sPortLateJoinedMat = 0, sPortLateJoinedMatNs = 0;  // (fx66: joins made by the first material work of calcAnim)
    u32 sPortPipeLoadingTurns = 0;  // turns of this log window in which another guest thread had run
    const u8* sPortPipeStreamAt = nullptr;  // the stream's write position after the draw (a serial turn): moved by the logic?
    bool sPortPipeStreamValid = false;
    // (the draw's three parts, GameSystem.cpp: beginRender; draw(); endRender)
    void (*sPortPipeBegin)(void*) = nullptr;
    void (*sPortPipeMiddle)(void*) = nullptr;
    void (*sPortPipeEnd)(void*) = nullptr;
    void* sPortPipeDrawCtx = nullptr;

    // The small state now (the live objects). Fields of a scene object that is not there are left zero.
    void portPipeSceneNow(PortPipeScene* pOut, const void* pScene) {
        memset(pOut, 0, sizeof *pOut);
        pOut->mExtSerial = J3DPortExtSerial();
        if (MR::isExistSceneObj(SceneObj_CameraContext)) {
            pOut->mView = MR::getCameraViewMtx();
            pOut->mInvView = MR::getCameraInvViewMtx();
            memcpy(pOut->mProjection, &MR::getCameraProjectionMtx(), sizeof pOut->mProjection);
        }
        if (MR::isExistSceneObj(SceneObj_CameraDirector)) {
            if (const CameraPoseParam* pose = MR::getCameraDirector()->mPoseParam1) {
                pOut->mPoseBytes = sizeof(CameraPoseParam) < sizeof pOut->mPose ? static_cast< u32 >(sizeof(CameraPoseParam)) : static_cast< u32 >(sizeof pOut->mPose);
                memcpy(pOut->mPose, pose, pOut->mPoseBytes);
            }
        }
        if (MR::isExistSceneObj(SceneObj_LightDirector)) {
            const LightDirector* director = MR::getSceneObj< LightDirector >(SceneObj_LightDirector);
            // (what LightDirector::loadLightPlayer loads: the player's light loader as ActorLightCtrl::loadLight reads it, then the point light)
            if (director->_1C != nullptr && director->mPointCtrl != nullptr && director->mPointCtrl->mCurrentInfo != nullptr) {
                pOut->mLights = true;
                const ActorLightCtrl* loader = director->_1C;
                if (loader->mAreaLightInf != nullptr) {
                    pOut->mPlayerLight = true;
                    pOut->mPlayerLightInfo = loader->_1C != nullptr ? loader->mLightInfo : *loader->getTargetActorLight(loader->mAreaLightInf);
                }
                pOut->mPointLight = *director->mPointCtrl->mCurrentInfo;
            }
        }
        GameSystem* system = SingletonHolder< GameSystem >::get();
        u32 branch = 0;
        if (pScene != nullptr && port_drawpipe_scene_flags(pScene, &branch)) {
            branch |= 0x100u;  // (a GameScene)
        }
        if (system != nullptr) {
            if (system->mHomeButtonLayout != nullptr && system->mHomeButtonLayout->isActive()) branch |= 0x200u;
            if ((system->mErrorWatcher != nullptr && system->mErrorWatcher->isWarning()) || GameSystemFunction::isOccurredSystemWarning()) branch |= 0x400u;
            if (GameSequenceFunction::isActiveSaveDataHandleSequence()) branch |= 0x800u;
            if (SingletonHolder< GameSystemResetAndPowerProcess >::get() != nullptr && SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive()) branch |= 0x1000u;
            if (system->mObjHolder != nullptr && system->mObjHolder->mCaptureScreenDirector != nullptr &&
                system->mObjHolder->mCaptureScreenDirector->getCurrentTiming() != nullptr) {
                branch |= 0x2000u;
            }
            if (system->mObjHolder != nullptr && system->mObjHolder->mStarPointerDirector != nullptr) {
                pOut->mPointer[0] = static_cast< u32 >(MR::getStarPointerModeForPort());
                pOut->mPointer[1] = MR::isStarPointerInScreen(0) ? 1u : 0u;
                if (const TVec2f* pos = MR::getStarPointerScreenPosition(0)) {
                    memcpy(&pOut->mPointer[2], pos, sizeof(TVec2f) < 8 ? sizeof(TVec2f) : 8);
                }
            }
        }
        pOut->mBranch = branch;
    }
    // One hash per small class of the census (port/draw_pipe.h kCamera ..).
    u64 portPipeSceneHash(const PortPipeScene& rScene, int cls) {
        using namespace port::drawpipe;
        switch (cls) {
        case kCamera:
            return HashBytes(rScene.mPose, sizeof rScene.mPose,
                             HashBytes(rScene.mProjection, sizeof rScene.mProjection, HashBytes(&rScene.mInvView, sizeof rScene.mInvView, HashBytes(&rScene.mView, sizeof rScene.mView))));
        case kSceneLight:
            return HashBytes(&rScene.mPointLight, sizeof rScene.mPointLight,
                             HashBytes(&rScene.mPlayerLightInfo, sizeof rScene.mPlayerLightInfo, HashValue(HashValue(12, rScene.mLights), rScene.mPlayerLight)));
        case kExt:
            return HashValue(13, rScene.mExtSerial);
        case kPointer:
            return HashBytes(rScene.mPointer, sizeof rScene.mPointer, 14);
        default:
            return HashValue(15, rScene.mBranch);
        }
    }

    struct PortPipeHost : port::drawpipe::Host {
        NameObjListExecutor* mExecutor = nullptr;  // (the scene's, while it has draw buffers and is not changing)
        const void* mScene = nullptr;

        void Walk(port::drawpipe::Set& rSet, port::drawpipe::Names& rNames) override {
            using namespace port::drawpipe;
            mExecutor->mBufferHolder->portPipeWalk(rSet, rNames);
            // (the small state: one object a class, keyed by its place in the store)
            PortPipeScene now;
            portPipeSceneNow(&now, mScene);
            static const u32 kBytes[kClasses - kFirstSmall] = {sizeof(TPos3f) * 2 + sizeof(Mtx44) + 128, sizeof(ActorLightInfo) + sizeof(PointLightInfo), 4, 32, 4};
            for (int c = kFirstSmall; c < kClasses; ++c) {
                rSet.Add(c, reinterpret_cast< const u8* >(&sPortPipeSceneStore) + (c - kFirstSmall), portPipeSceneHash(now, c), 0, kBytes[c - kFirstSmall]);
            }
        }
        bool Freeze(port::drawpipe::Frame& rFrame) override { return mExecutor->mBufferHolder->portPipeFreeze(rFrame); }
        void FreezeSmall() override { portPipeSceneNow(&sPortPipeSceneStore, mScene); }
        uint32_t SmallDiffer() override {
            using namespace port::drawpipe;
            PortPipeScene now;
            portPipeSceneNow(&now, mScene);
            uint32_t differ = 0;
            for (int c = kFirstSmall; c < kClasses; ++c) {
                if (portPipeSceneHash(now, c) != portPipeSceneHash(sPortPipeSceneStore, c)) {
                    differ |= 1u << (c - kFirstSmall);
                }
            }
            return differ;
        }
        bool DrawOutstanding() override { return portPipeOverlapped(); }
        void UseFrozen(const port::drawpipe::Frame* pFrame) override {
            DrawBufferHolder::portPipeUse(pFrame, pFrame != nullptr ? &sPortPipeDriver.pass : nullptr, pFrame != nullptr ? &sPortPipeSceneStore : nullptr);
        }
        int64_t NowNs() override { return port_host_time_ns(); }
        const unsigned char* StreamPos() override { return __PortGXFifoPtr; }
        // The draw lists that are not J3D passes (NameObj::draw of everything connected with a draw type: shadows, particles,
        // layouts, the sky's clear, ...): each reads its own object's state, which the next update writes.
        void OpenLists(uint32_t* pLists, uint32_t* pObjects) override {
            const NameObjCategoryList* list = mExecutor->mDrawList;
            if (list == nullptr || list->mCategoryInfo.mArr == nullptr) {
                return;
            }
            for (int i = 0; i < list->mCategoryInfo.size(); ++i) {
                const int n = list->mCategoryInfo.mArr[i].mNameObjArr.size();
                if (n > 0) {
                    ++*pLists;
                    *pObjects += static_cast< uint32_t >(n);
                }
            }
        }
        void Log(const char* pLine) override { port_log_summary("%s", pLine); }
    };
    PortPipeHost sPortPipeHost;

    NameObjListExecutor* portPipeExecutor() {
        GameSystem* system = SingletonHolder< GameSystem >::get();
        GameSystemSceneController* controller = system != nullptr ? system->mSceneController : nullptr;
        if (controller == nullptr || controller->mScene == nullptr || controller->isChangingScene()) {
            return nullptr;
        }
        NameObjListExecutor* executor = controller->mScene->mListExecutor;
        return executor != nullptr && executor->mBufferHolder != nullptr ? executor : nullptr;
    }

    // ---- the draw thread's side (modes 3 and 4)
    bool portPipeTokenOut(u32 id, bool nothing) { return sPortPipe.Token(id, nothing); }
    void portPipeAbandon(int why) { sPortPipe.Abandon(why); }
    struct PortPipeGame : port::drawpipe::PipeHost {
        void DrawBegin() override { sPortPipeBegin(sPortPipeDrawCtx); }
        void DrawMiddle() override { sPortPipeMiddle(sPortPipeDrawCtx); }
        void DrawEnd() override { sPortPipeEnd(sPortPipeDrawCtx); }
        // (the draw thread has executed the recording: what it wrote last sits in an open staging segment until someone
        // publishes it; the game thread's endRender would, but only after its join. Bytes are not changed by this.)
        void AfterReplay() override { __PortGXFifoFlush(); }
        // Mode 3: for this job the draw thread is the game thread: the guest's main thread (interrupts, the heap, the
        // scheduler: os_thread.cpp) and "the frame thread" (perf.cpp). The game thread sleeps at the hand-over meanwhile.
        void OwnerEnter() override {
            if (port_os_lend_main) port_os_lend_main(1);
            port_frame_thread_lend(1);
        }
        void OwnerLeave() override {
            port_frame_thread_lend(0);
            if (port_os_lend_main) port_os_lend_main(0);
        }
        bool CaptureOpen() override {
            if (!sPortPipeDriver.frame.valid) {
                return false;
            }
            DrawBufferHolder::portPipeRecord(&portPipeTokenOut, &portPipeAbandon);
            return true;
        }
        void CaptureClose(bool) override { DrawBufferHolder::portPipeRecord(nullptr, nullptr); }
        void RunToken(uint32_t id, bool displaced) override { DrawBufferHolder::portPipeRunToken(id, displaced); }
        void OverlapBegin() override {
            gPortPipeOverlap = true;
            // (the guest OS's owner check, os_thread.cpp: from now until the join a guest call on the draw thread's stack does nothing and is reported)
            gPortPipeStackLo = sPortPipe.worker.StackLo();
            gPortPipeStackSpan = sPortPipe.worker.StackSpan();
            if (&__PortGXCapTrap != nullptr && port_drawpipe_trap()) {
                __PortGXCapTrap = 1;
                // (the hooks armed here, before the dispatch: a GX call of the logic before the draw thread has begun the replay
                // joins as one made during it does)
                if (port_gx_replay_arm) port_gx_replay_arm(gPortPipeStackLo, gPortPipeStackSpan);
            }
        }
        void OverlapEnd() override {
            if (&__PortGXCapTrap != nullptr) {
                __PortGXCapTrap = 0;
                if (port_gx_replay_arm) port_gx_replay_arm(0, 0);
            }
            gPortPipeStackSpan = 0;
            gPortPipeOverlap = false;
            sPortPipeHost.UseFrozen(nullptr);  // (the frame is out of use; what the passes wrote into their J3DSys goes back into j3dSys)
            sPortPipeDriver.pass.mergedDeferred += DrawBufferShapeDrawer::portMergedFinish();
            void* from = nullptr;
            if (const uint32_t calls = sPortPipe.GuestCalls(&from)) {
                port_log_summary("drawpipe: %u guest-OS calls on the draw thread during the overlap (first from %p; port_drawpipe_point at %p): each did "
                                 "nothing; whatever made it must move to the game thread", calls, from, (void*)&port_drawpipe_point);
                sPortPipe.TurnOff(*this, "the draw thread entered the guest OS");
            }
        }
        void Poll() override { port_irq_poll(); }
        int64_t NowNs() override { return port_host_time_ns(); }
        void Log(const char* pLine) override { port_log_summary("%s", pLine); }
    };
    PortPipeGame sPortPipeGame;

    // ================================================================================================ GXV_PIPE25 (pipe27, 12 Nov 2026)
    // The game's side of port/draw_thread.h's Late engine. The picture is made as ever on the game thread (G) up to the J3D passes. A pass
    // does not run: portPipeDraw (DrawBufferHolder.cpp) has made its token (the frozen lists, the J3D context before / after) and hands it to
    // portLateTokenOut, which (1) sends the GX library's lazy state into the stream (__PortGXPipeFirewall: dirtyState 0, no primitive
    // flush pending, array / light / TEV-colour shadows dropped), (2) copies GXData, (3) redirects the FIFO into the side chain
    // (port_gx_raw_token, gx_fifo.cpp): the open lists' bytes up to the next pass go there. At the hand-over the helper runs pass i from
    // snapshot i into the stream and copies the chain bytes behind it, in order, while G does update(). End: GXData as the serial draw leaves it
    // (MergeSerial), then G's endRender at the join.
    //
    // WHAT STAYS ON THE GAME THREAD, and why (the state-read table of agents/pipe26/REPORT.txt, as built):
    //   - every open draw list (shadows, particles, 2D layouts and the HUD, models drawn by hand, the sky's clear, the mirror): they read live
    //     objects (positions, particle pools, layout panes) that update() writes; recording them costs what executing them does (why
    //     GXV_DRAWPIPE=4 lost). Their bytes are the chain.
    //   - whole turns: the decision's reasons (no scene, loading, demo/movie/non-action nerve, pause, HOME/warning/save/reset, mirror/VR/capture,
    //     a form switch, logic GX in the last 30 turns, the first 8 turns after any of these): the same list as mode 4.
    //   - a pass inside such a turn that cannot be handed over ends the deferral at that point (Late::Abandon): the passes made so far run
    //     now, in order, on G and the rest of the draw is serial. Reasons, counted by name in the "pipe25:" line: a pass asked for from the live
    //     lists / another holder's frame (kWhyListMoved), the lists moved since the hand-over (epoch), no memory for the token or no room
    //     for the chain (kWhyNoMemory), a matrix half or list written inside the draw (kWhyDrawWrite: port_drawpipe_game_enter), a fur draw's
    //     write of its shape's matrix-index words after a token was made (kWhyReview: the words a deferred pass must see are the old ones;
    //     handled by running the earlier passes before the write, so nothing needs saving), the draw inside a display list recording (the
    //     raw FIFO is not opened).
    //   - no J3D buffer type is excluded by name: a pass reads the frozen lists, the draw-side J3D context, the scene state frozen with the
    //     lists, and the halves of the double-buffered matrices / lists the flushes (before the hand-over) wrote and update() does not.
    //     That the REST of what a pass reads (material animation, shape words) is not written by update() is the unverified claim:
    //     GXV_PIPE25=2 is its proof (every pass re-run from its snapshot beside update, compared byte for byte).
    port::drawpipe::LateHost& portLateGameRef();
    constexpr uint32_t kLateNoRoomMax = 3;  // chain allocations refused this many times in a row turn the late draw off
    void lateSaveGx(port::pipe25::HVec< uint32_t >& v) {
        v.resize(sLateState.words);
        __PortGXPipeSave(v.data());
    }
    // Pass i where the calling thread is (the helper, or G at a take-over / an abandon): GXData from its snapshot, the pass, GXData as it left it.
    void lateRunPass(uint32_t i, bool displaced) {
        PortLateState& st = sLateState;
        if (i >= st.tokId.size() || i >= st.n) {
            return;
        }
        __PortGXPipeLoad(st.snap[i].data());
        __PortGXPipeFirewall();
        DrawBufferHolder::portPipeRunToken(st.tokId[i], displaced);
        __PortGXPipeSave(st.end[i].data());
    }
    void lateSpliceToken(uint32_t i, void* pDisplaced) { lateRunPass(i, pDisplaced != nullptr); }
    // GXData as the serial draw leaves it: the library's data after the open lists, with what each pass wrote put in (pipe25.h MergeSerial).
    void lateMerge() {
        PortLateState& st = sLateState;
        const uint32_t taken = port::pipe25::MergeSerialN(st.fin.data(), st.snap, st.end, st.words, st.n);
        sLate->c.mergeWords += taken;
        __PortGXPipeLoad(st.fin.data());
        __PortGXPipeShadowDrop();  // (what the shadows hold is of the last pass, not of the stream's end)
    }

    bool portLateTokenOut(u32 id, bool) {
        PortLateState& st = sLateState;
        if (sLate == nullptr || !sLate->Capturing()) {
            return false;
        }
        const int64_t t0 = port_host_time_ns();
        if (st.tokId.size() >= kLateMaxTokens) {
            sLate->Abandon(portLateGameRef(), port::drawpipe::kWhyNoMemory);
            return false;
        }
        __PortGXPipeFirewall();
        const size_t at = st.tokId.size();
        if (st.snap.size() <= at) {
            st.snap.resize(at + 1);
            st.end.resize(at + 1);
        }
        st.snap[at].resize(st.words);
        st.end[at].resize(st.words);
        __PortGXPipeSave(st.snap[at].data());
        if (!st.verify) {
            if (!port_gx_raw_token()) {
                // (no chain memory, or the draw is inside a display list: this pass cannot wait. The ones before it run now.)
                if (++st.noRoomRun >= kLateNoRoomMax) {
                    sLate->TurnOff(portLateGameRef(), "the side chain could not be allocated, several draws in a row");
                }
                sLate->Abandon(portLateGameRef(), port::drawpipe::kWhyNoMemory);
                return false;
            }
            st.noRoomRun = 0;
        } else {
            // (verify: the pass runs in place; what it writes is kept for the compare, see portLateDone)
            s32 type = -1;
            bool xlu = false;
            DrawBufferHolder::portPipeTokenInfo(id, &type, &xlu);
            if (st.post.size() <= at) {
                st.post.resize(at + 1);
            }
            st.post[at].resize(st.words);
            st.cur = port::pipe25::PassRec();
            st.cur.id = static_cast< uint32_t >(at);
            st.cur.type = type;
            st.cur.xlu = xlu;
            st.cur.models.push_back(std::make_pair(0u, port::pipe25::HStr(DrawBufferHolder::portPipeGroupName(type))));  // (heap36: HStr: the name's characters are host memory)
            st.startPtr = __PortGXFifoPtr;
            st.startFlips = port_gx_seg_flips();
            st.pending = true;
        }
        st.tokId.push_back(id);
        sLate->CountToken();
        const int64_t t1 = port_host_time_ns();
        sLate->c.tokenNs += static_cast< uint64_t >(t1 - t0);
        st.startNs = t1;
        return !st.verify;  // (verify: false = the pass is executed where it stands)
    }
    // Verify: the pass just executed in place. Its bytes, if they lie in one segment and the scratch buffer can hold them.
    void portLateDone(u32) {
        PortLateState& st = sLateState;
        if (!st.pending) {
            return;
        }
        st.pending = false;
        if (st.cur.id < st.post.size()) {
            __PortGXPipeSave(st.post[st.cur.id].data());  // (revB: the library's data as this pass left it, for the re-run's compare)
        }
        sLate->c.gameInPassNs += static_cast< uint64_t >(port_host_time_ns() - st.startNs);
        uint8_t* const p1 = __PortGXFifoPtr;
        port::pipe25::PassRec rec = st.cur;
        rec.offset = static_cast< uint32_t >(st.store.bytes.size());
        const bool contiguous = port_gx_seg_flips() == st.startFlips && p1 >= st.startPtr && static_cast< size_t >(p1 - st.startPtr) < st.scratchCap;
        if (contiguous && st.store.bytes.size() + static_cast< size_t >(p1 - st.startPtr) < (8u << 20)) {
            rec.length = static_cast< uint32_t >(p1 - st.startPtr);
            st.store.bytes.insert(st.store.bytes.end(), st.startPtr, p1);
            rec.comparable = true;
        } else {
            rec.length = 0;
            rec.comparable = false;  // (crossed a segment, or larger than the scratch buffer: left out, counted)
        }
        st.store.passes.push_back(rec);
    }
    void portLateAbandon(int why) {
        if (sLate != nullptr) {
            sLate->Abandon(portLateGameRef(), why);
        }
    }

    struct PortLateGame : port::drawpipe::LateHost {
        void DrawBegin() override { sPortPipeBegin(sPortPipeDrawCtx); }
        void DrawMiddle() override { sPortPipeMiddle(sPortPipeDrawCtx); }
        void DrawEnd() override { sPortPipeEnd(sPortPipeDrawCtx); }
        bool Open(bool verify) override {
            PortLateState& st = sLateState;
            if (!sPortPipeDriver.frame.valid || &__PortGXCap == nullptr || __PortGXCap != 0 || (verify && st.scratch == nullptr) || sLate->worker.StackSpan() == 0) {
                // (the trap needs the helper's stack range, known once it has reported in; until then the turn is serial)
                return false;  // (the owner trap arms only on an idle capture layer: it must work at the hand-over, there is no way back then)
            }
            if (!verify && !port_gx_raw_open()) {
                return false;
            }
            st.verify = verify;
            st.dropped = st.pending = st.broken = false;
            st.n = 0;
            st.tokId.clear();
            st.store.Clear();
            DrawBufferHolder::portPipeRecord(&portLateTokenOut, &portLateAbandon);
            DrawBufferHolder::portPipeLateHooks(verify ? &portLateDone : nullptr, true);
            return true;
        }
        uint32_t Close(bool verify, bool abandoned) override {
            PortLateState& st = sLateState;
            DrawBufferHolder::portPipeRecord(nullptr, nullptr);
            DrawBufferHolder::portPipeLateHooks(nullptr, false);
            if (verify) {
                if (st.dropped || st.tokId.empty() || st.store.passes.size() != st.tokId.size()) {
                    return 0;
                }
                st.n = static_cast< uint32_t >(st.tokId.size());
                lateSaveGx(st.fin);
                DrawBufferHolder::portPipeRewind();  // (the J3D context back to the first pass's: the helper runs the picture's passes a second time)
                ++sLate->vs.pictures;
                return st.n;
            }
            if (!st.tokId.empty()) {
                lateSaveGx(st.fin);  // (GXData after the open lists; the chain is closed next, the FIFO points at a sink until the splice)
            }
            const uint32_t marks = port_gx_raw_close();
            if (marks == 0) {
                return 0;  // (no pass was handed over, or the abandon ran them all and the stream is whole)
            }
            st.n = static_cast< uint32_t >(st.tokId.size());
            if (marks != st.n) {
                char line[160];
                snprintf(line, sizeof line, "pipe25: BUG: %u chain marks for %u tokens (extra marks run no pass)", marks, st.n);
                Log(line);
            }
            sLate->c.chainBytes += port_gx_raw_bytes();
            const uint64_t high = port_gx_raw_high();
            if (high > sLate->c.chainHigh) sLate->c.chainHigh = high;
            (void)abandoned;
            return marks;
        }
        uint32_t Begin(bool verify) override { return verify ? sLateState.n : port_gx_raw_splice_begin(); }
        void Step(uint32_t i, bool verify) override {
            PortLateState& st = sLateState;
            if (!verify) {
                port_gx_raw_splice_step(i, &lateSpliceToken, nullptr);
                return;
            }
            // VERIFY: pass i again, from its snapshot, into the scratch buffer; the stream is not touched (G's update() beside us uses no GX:
            // the owner trap joins first if it tries).
            if (i >= st.store.passes.size() || st.scratchSkip) {
                ++sLate->vs.passes;
                ++sLate->vs.skipped;
                return;
            }
            port::pipe25::PassRec& rec = st.store.passes[i];
            __PortGXPipeLoad(st.snap[i].data());
            __PortGXPipeFirewall();  // (this one writes nothing: the snapshot was taken after the same call)
            if (!port_gx_scratch_begin(st.scratch, st.scratchCap)) {
                st.scratchSkip = true;  // (the passes after this one cannot be re-run in order: the rest of the picture is left out)
                ++sLate->vs.passes;
                ++sLate->vs.skipped;
                return;
            }
            DrawBufferHolder::portPipeRunToken(st.tokId[i], false);
            const uint32_t len = port_gx_scratch_end();
            if (len == 0xFFFFFFFFu) {
                rec.comparable = false;
                ++sLate->vs.passes;
                ++sLate->vs.skipped;
                return;
            }
            if (i < st.post.size() && i < st.end.size()) {
                __PortGXPipeSave(st.end[i].data());  // (revB: GXData as the re-run left it, against what the pass in place left)
                if (!port::pipe25::CompareGxEnd(st.post[i].data(), st.end[i].data(), st.words, sLate->vs, static_cast< uint32_t >(sLate->vs.pictures), i) && sLate->vs.stateDiffer == 1) {
                    port_log_summary("pipe25: VERIFY end state: %s", sLate->vs.stateFirst);  // (the first one at once)
                }
            }
            const uint64_t before = sLate->vs.mismatches;
            port::pipe25::ComparePass(rec, st.store.bytes.data() + rec.offset, st.scratch, len, sLate->vs, static_cast< uint32_t >(sLate->vs.pictures));
            if (sLate->vs.mismatches != before && before == 0) {
                port_log_summary("pipe25: VERIFY: %s", sLate->vs.first);  // (the first one at once; the census line repeats it)
            }
            if (sLate->vs.mismatches != before && sLate->vs.mismatches <= 12) {
                // (pipefix31: what differs: eight bytes of each side at the first differing byte, and which pass kind)
                const uint8_t* a = st.store.bytes.data() + rec.offset;
                const uint32_t at = sLate->vs.lastAt, n = rec.length < len ? rec.length : len;
                char sa[40] = "", sb[40] = "";
                for (uint32_t k = 0; k < 8 && at + k < n; ++k) {
                    snprintf(sa + k * 3, sizeof sa - k * 3, "%02X ", a[at + k]);
                    snprintf(sb + k * 3, sizeof sb - k * 3, "%02X ", st.scratch[at + k]);
                }
                port_log_summary("pipe25: VERIFY mismatch #%u: picture %u pass %u type 0x%X len %u/%u first difference at byte %u: in place [%s] re-run [%s]", static_cast< unsigned >(sLate->vs.mismatches),
                                 static_cast< unsigned >(sLate->vs.pictures), static_cast< unsigned >(i), static_cast< unsigned >(rec.type), static_cast< unsigned >(rec.length), static_cast< unsigned >(len), at, sa, sb);
                {
                    // (hazard32: the compare names the FIRST difference only; how much differs behind it says whether one cause explains the pass)
                    uint32_t differ = 0, runs = 0, last = at;
                    bool in = false;
                    for (uint32_t k = at; k < n; ++k) {
                        const bool d = a[k] != st.scratch[k];
                        if (d) { ++differ; last = k; }
                        if (d && !in) ++runs;
                        in = d;
                    }
                    port_log_summary("pipe25: VERIFY mismatch #%u: %u bytes differ in %u runs, the last at byte %u", static_cast< unsigned >(sLate->vs.mismatches), static_cast< unsigned >(differ),
                                     static_cast< unsigned >(runs), static_cast< unsigned >(last));
                }
                if (sLate->vs.mismatches <= 4) {
                    // (p31e: both whole passes to files, for decoding the command stream offline: ux0:data/smg/p25vm<k>_inplace.bin / _rerun.bin, and the
                    // models of the pass with their byte offsets in the log)
                    char path[96];
                    const unsigned k = static_cast< unsigned >(sLate->vs.mismatches);
                    snprintf(path, sizeof path, "ux0:data/smg/p25vm%u_inplace.bin", k);
                    if (FILE* f = fopen(path, "wb")) { fwrite(a, 1, rec.length, f); fclose(f); }
                    snprintf(path, sizeof path, "ux0:data/smg/p25vm%u_rerun.bin", k);
                    if (FILE* f = fopen(path, "wb")) { fwrite(st.scratch, 1, len, f); fclose(f); }
                    for (const auto& m : rec.models) {
                        port_log_summary("pipe25: VERIFY mismatch #%u: model '%s' from byte %u", k, m.second.c_str(), static_cast< unsigned >(m.first));
                    }
                }
            }
        }
        void End(bool verify) override {
            PortLateState& st = sLateState;
            if (verify) {
                __PortGXPipeLoad(st.fin.data());  // (the game thread's own library state, as it was at the hand-over)
                __PortGXPipeShadowDrop();
                st.scratchSkip = false;
                return;
            }
            lateMerge();
            port_gx_raw_splice_end();
        }
        void AbandonNow(bool verify) override {
            PortLateState& st = sLateState;
            if (verify) {
                st.dropped = true;  // (the passes already ran in place; this picture is not compared)
                st.pending = false;
                return;
            }
            // The passes made so far run now, here, in order: GXData from each snapshot, the chain spliced in behind each, then the
            // merge. G is the library's owner (the abandon is G's, mid-draw, between two GX calls).
            if (st.tokId.empty()) {
                port_gx_raw_abandon(&lateSpliceToken, nullptr);
                return;
            }
            lateSaveGx(st.fin);
            st.n = static_cast< uint32_t >(st.tokId.size());
            if (!port_gx_raw_abandon(&lateSpliceToken, reinterpret_cast< void* >(1))) {
                ++sLate->c.abandonFail;
                st.broken = true;
                sLate->TurnOff(*this, "an abandon inside a display list recording (the deferred passes go on to the hand-over)");
                return;
            }
            lateMerge();
            st.tokId.clear();
            st.n = 0;
        }
        bool OverlapBegin() override {
            gPortPipeOverlap = true;
            gPortPipeStackLo = sLate->worker.StackLo();
            gPortPipeStackSpan = sLate->worker.StackSpan();
            // (the owner trap, three-way, gx_capture.cpp trapLate: the helper's calls pass by its stack; the game thread joins the helper at its first GX call;
            // every other thread waits until no job is out and then runs its real call under its own exempt window. The pass word is raised for the overlap so that
            // the helper's calls go straight in, as the replay's do in mode 4. Open() has seen to it that the layer is idle. GXV_PIPE25_SEE=1 leaves the pass word
            // down, so that the layer sees every call of the helper too: a diagnostic, slower.)
            __PortGXCapTrap = 1;
            const bool armed = port_gx_replay_arm_trap != nullptr && port_gx_replay_arm_trap(sLate->worker.StackLo(), sLate->worker.StackSpan()) != 0;
            if (!sPassDown) __atomic_fetch_add(&__PortGXCapPass, 1, __ATOMIC_RELAXED);  // (also when the arm failed: OverlapEnd takes it down again)
            return armed;  // (false: the job is not handed over, the game thread runs the passes itself and the feature goes off)
        }
        // (the game thread runs the job itself, in the take-over: its GX calls are the draw's own, the trap must be down)
        void SelfRunBegin() override { if (port_gx_replay_arm) port_gx_replay_arm(0, 0); }
        void LogExtra() override {
            uint32_t f = 0, g = 0, st = 0, rn = 0;
            if (port_gx_trap_stats) port_gx_trap_stats(&f, &g, &st, &rn);
            port_log_summary("pipe25: owner trap: %u game-thread entries (each joined the helper), %u calls of OTHER threads (each waited for the helper's job, then ran under its own "
                             "exempt window; the start-up check makes 6 of these; %u waited past the watchdog and kept waiting), %u armed after the join (disarmed by force; the start-up check makes 1), %u runaways", g, f,
                             static_cast< unsigned >(sLate->OtherWaitsOverWatchdog()), st, rn);
            // (pipe61) which guest threads other than main / audio / JAudio's disc thread ran in the window (the 21 % "another guest thread ran" turns), and the join's place
            if (port_os_other_guest_take) {
                char who[400];
                int o = 0;
                who[0] = 0;
                void* thread = nullptr;
                void* func = nullptr;
                int priority = 0;
                unsigned runs = 0;
                for (int i = 0; i < 8 && o < 340 && port_os_other_guest_take(i, &thread, &func, &priority, &runs); ++i) {
                    o += snprintf(who + o, sizeof who - static_cast< size_t >(o), " [OSThread %p entry %p guest priority %d: %u runs]", thread, func, priority, runs);
                }
                port_log_summary("pipe25: other guest threads that ran in the window (GXV_DRAWPIPE_LOADCHECK=1 makes the turn serial for each hand-over one ran before):%s",
                                 o != 0 ? who : " none");
            }
            if (port_pipe25_joinlate() != 0) {
                port_log_summary("pipe25: GXV_PIPE25_JOINLATE=1%s: %u joins made after calcAnim() (%.3f ms a join), %u early joins by a write inside calcAnim(), %u joins before calcAnim's material work (%.3f ms a join; fx66)",
                                 sPortLateJoinLateOff ? " (OFF: see the line above)" : "", sPortLateJoinedLate,
                                 sPortLateJoinedLate != 0 ? sPortLateJoinedLateNs / 1000.0 / sPortLateJoinedLate : 0.0, sPortLateCalcEarly, sPortLateJoinedMat,
                                 sPortLateJoinedMat != 0 ? sPortLateJoinedMatNs / 1000.0 / sPortLateJoinedMat : 0.0);
                sPortLateJoinedLate = 0, sPortLateJoinedLateNs = 0, sPortLateJoinedMat = 0, sPortLateJoinedMatNs = 0;
            }
        }
        void OverlapEnd() override {
            if (!sPassDown) __atomic_fetch_sub(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
            __PortGXCapTrap = 0;
            port_gx_replay_arm(0, 0);
            gPortPipeStackSpan = 0;
            gPortPipeOverlap = false;
            sPortPipeHost.UseFrozen(nullptr);  // (the frame is out of use; what the passes wrote into their J3DSys goes back into j3dSys)
            sPortPipeDriver.pass.mergedDeferred += DrawBufferShapeDrawer::portMergedFinish();
            void* from = nullptr;
            if (const uint32_t calls = sLate->GuestCalls(&from)) {
                port_log_summary("pipe25: %u guest-OS calls on the helper during the overlap (first from %p): each did nothing; whatever made it must "
                                 "move to the game thread", calls, from);
                sLate->TurnOff(*this, "the helper entered the guest OS");
            }
        }
        void Poll() override { port_irq_poll(); }
        int64_t NowNs() override { return port_host_time_ns(); }
        void Log(const char* pLine) override { port_log_summary("%s", pLine); }
    };
    PortLateGame sPortLateGame;
    port::drawpipe::LateHost& portLateGameRef() { return sPortLateGame; }

    // Once, at the first drawn turn with GXV_PIPE25 on: the helper thread, the buffers, the log line that says what there is.
    bool portLateTrapOk() { return &__PortGXCapTrap != nullptr && &__PortGXCapPass != nullptr && port_gx_replay_arm != nullptr && port_drawpipe_trap() != 0; }
    // (p31e) the self-test's job waits for this: the probe thread's call has reached the trap
    uint32_t sProbeForeign0 = 0;
    uint32_t sTrapTestJobMs = 0, sTrapTestBackUs = 0;  // (pipe33: how long the start-up check's job was out, and how long after its end the probe came back)
    bool portLateProbeTrapped() {
        uint32_t f = 0, g = 0, st = 0, rn = 0;
        if (port_gx_trap_stats) port_gx_trap_stats(&f, &g, &st, &rn);
        return f > sProbeForeign0;
    }
    // The start-up check of the trap, on the game thread before the first draw. nullptr: it behaves; else the reason.
    const char* portLateTrapTest(port::drawpipe::Late* late) {
        const uintptr_t lo = late->worker.StackLo(), span = late->worker.StackSpan();
        if (port_gx_trap_selftest == nullptr || port_gx_trap_probe_start == nullptr || port_gx_trap_probe_wait == nullptr || span == 0) return "the trap's self-test is absent";
        if (const char* bad = port_gx_trap_selftest(lo, span)) return bad;
        // the case that matters: a thread makes hooked calls while a REAL job is out on the helper. It must come back only after the job has ended.
        __PortGXCapTrap = 1;
        if (port_gx_replay_arm_trap == nullptr || port_gx_replay_arm_trap(lo, span) == 0) {
            __PortGXCapTrap = 0;
            return "the trap cannot arm";
        }
        __atomic_fetch_add(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
        int64_t jobEnd = 0, backAt = 0;
        const char* bad = nullptr;
        // (p31e) the job does not end on a timer: it holds the helper until the probe thread has been TRAPPED (the trap's count of other threads' calls
        // went up), then 4 ms more. So the probe's call is in the trap while the job is live, whatever the start-up of the probe thread costs.
        uint32_t f0 = 0, g0 = 0, s0 = 0, r0 = 0;
        if (port_gx_trap_stats) port_gx_trap_stats(&f0, &g0, &s0, &r0);
        sProbeForeign0 = f0;
        if (!late->TestJobBegin(portLateGameRef(), 4000, &jobEnd, &portLateProbeTrapped)) {
            bad = "the helper could not be given the test job";
        } else {
            // (pipe33) The job is waited for FIRST, with a long limit, and the probe gets its own limit from the job's end. Before, the probe had 2 s from its
            // start: on the console (c111, two runs) the helper's test job took 2.4 s, because the test runs beside the graphics start-up and the helper gets
            // little of its core then. The probe was held by the trap for all of it, which is the trap WORKING, and the limit read it as a failure: mode 1 was
            // turned off for the session ("did not return while a job was out"); mode 2 passed the same test with 20 ms to spare.
            const int64_t tJob0 = port_host_time_ns();
            void* pr = port_gx_trap_probe_start();
            const bool jobOk = late->TestJobEnd(30000000);
            const int64_t tJob1 = port_host_time_ns();
            const int back = pr != nullptr && port_gx_trap_probe_wait(pr, 5000, &backAt) != 0;
            sTrapTestJobMs = static_cast< uint32_t >((tJob1 - tJob0) / 1000000);
            sTrapTestBackUs = back && backAt > jobEnd ? static_cast< uint32_t >((backAt - jobEnd) / 1000) : 0;
            if (!jobOk) bad = "the helper did not finish its test job within 30 s";
            else if (!back) bad = "another thread's GX call did not return within 5 s of the job's end";
            else if (!late->TestJobReady()) bad = "another thread's GX call was not trapped while a job was out (the trap did not see it)";
            else if (backAt < jobEnd) bad = "another thread's GX call returned while a job was still out (the trap did not hold it)";
        }
        __atomic_fetch_sub(&__PortGXCapPass, 1, __ATOMIC_RELAXED);
        if (__PortGXCap == 3) __PortGXCap = 0;
        __PortGXCapTrap = 0;
        return bad;
    }
    void portLateInit() {
        static bool sDone = false;
        if (sDone) {
            return;
        }
        sDone = true;
        sPassDown = getenv("GXV_PIPE25_SEE") != nullptr && atoi(getenv("GXV_PIPE25_SEE")) != 0;  // (diagnostic: pass word down, the layer sees and counts every call; slower)
        const int mode = port_pipe25_active();
        // (pipefix26: the host heap, never the scene's JKRHeap: the helper thread holds this object's address for the whole session)
        void* const lateMem = port_host_alloc(sizeof(port::drawpipe::Late), 64);
        port::drawpipe::Late* late = lateMem != nullptr ? new (lateMem) port::drawpipe::Late : nullptr;
        if (late == nullptr) {
            return;
        }
        late->watchdogUs = static_cast< uint32_t >(port_drawpipe_watchdog_ms()) * 1000u;
        late->sliceUs = static_cast< uint32_t >(port_drawpipe_slice_us());
        late->takeoverUs = static_cast< uint32_t >(port_pipe25_takeover_us());
        PortLateState& st = sLateState;
        st.words = __PortGXPipeBytes() / 4u;
        bool scratchOk = true;
        if (mode == 2) {
            st.scratchCap = static_cast< uint32_t >(port_pipe25_scratch_kb()) * 1024u;
            st.scratch = static_cast< uint8_t* >(malloc(st.scratchCap));
            scratchOk = st.scratch != nullptr;
            st.store.bytes.reserve(256u << 10);
        }
        st.fin.reserve(st.words);
        st.tokId.reserve(64);
        const bool thread = scratchOk && late->worker.Start(port_pipe25_core(), port_pipe25_prio() == 0 ? -1 : port_pipe25_prio(),
                                                           static_cast< uint32_t >(port_pipe25_stack_kb()) * 1024u);
        port_log_summary("pipe25: %s", late->worker.SelfResult() == 1 ? "tls ok (pthread keys and thread_local work on the helper)" : late->worker.SelfResult() == 2 ? "DISABLED: helper self-test failed" : "tls self-test not reported yet");
        if (late->worker.SelfResult() == 2) {
            port_log_summary("pipe25: DISABLED: %s", late->worker.SelfWhy());
        }
        sLate = late;  // (a late draw with no thread, or no buffer, finds the worker not ready and draws every turn serially)
        // (pipefix31) the owner trap, tried once before use: another thread's GX call under an armed trap must go through, the calling thread's must not stay trapped
        if (const char* e = getenv("GXV_PIPE25_NETLOSS")) late->netLoss = atoi(e) != 0;
        if (const char* e = getenv("GXV_PIPE25_JUDGE_PASS_US")) late->kMinJudgedPassMs = atoi(e) / 1000.0;  // (pipe60: the net-loss rule judges only windows whose passes average this many us a late turn; 0 = every window)
        if (thread && late->worker.SelfResult() != 2 && portLateTrapOk() && port_gx_capture_mode != nullptr && port_gx_capture_mode() == 0) {
            const char* bad = portLateTrapTest(late);
            if (bad != nullptr) {
                port_log_summary("pipe25: DISABLED: owner trap self-test: %s (the test job was out %u ms)", bad, static_cast< unsigned >(sTrapTestJobMs));
                late->TurnOff(portLateGameRef(), "the owner trap self-test failed");
            } else {
                port_log_summary("pipe25: owner trap self-test ok (another thread's GX calls under an armed trap: returned with no job out, returned only AFTER a running job ended, "
                                 "no recursion; the game thread's own call comes down by itself); the test job was out %u ms, the other thread's call came back %u us after it ended",
                                 static_cast< unsigned >(sTrapTestJobMs), static_cast< unsigned >(sTrapTestBackUs));
            }
        }
        const bool defer = port_gx_drawdone_defer_mode() == 4;
        const bool capture = port_gx_capture_mode == nullptr || port_gx_capture_mode() == 0;
        const bool trap = portLateTrapOk();
        port_log_summary("pipe25: GXV_PIPE25=%d (%s): helper thread %s (core %d, priority %s 0x%X; GXV_PIPE25_CORE=%d GXV_PIPE25_PRIO=0x%X); take-over after %d us, "
                         "watchdog %d ms; needs GXV_DRAWDONE_DEFER=4 (%s), GX capture layer off (%s), owner trap (%s, GXV_DRAWPIPE_TRAP=%d)%s; "
                         "GXData %u bytes; verify scratch %u KiB",
                         mode, mode == 2 ? "VERIFY: passes run in place, re-run beside update and compared" : "ON", !scratchOk ? "NOT CREATED (no scratch memory): every draw is serial"
                         : !thread ? "NOT CREATED: every draw is serial" : late->worker.Reported() != 0 ? "created and running" : "created; it has not run yet (a job it does not start is taken back)",
                         late->worker.Core(), port_pipe25_prio() == 0 ? "= the game thread's" : "set", static_cast< unsigned >(late->worker.AskedPriority()), port_pipe25_core(),
                         static_cast< unsigned >(port_pipe25_prio()), port_pipe25_takeover_us(), port_drawpipe_watchdog_ms(), defer ? "ok" : "NOT 4: every draw serial",
                         capture ? "ok" : "ON: every draw serial", trap ? "armed per overlap" : "ABSENT: every draw serial", port_drawpipe_trap(),
                         port_pipe25_mode() != 0 && port_pipe25_active() == 0 ? "; GXV_DRAWPIPE is set: GXV_PIPE25 is ignored" : "", st.words * 4u,
                         st.scratchCap >> 10);
    }

    void* portPipeApiEnd() { return port_gx_capture_end(); }
    void portPipeApiReplay(void* pCapture, void (*pToken)(uint32_t, void*), void* pCtx) { port_gx_replay(static_cast< PortGxCapture* >(pCapture), pToken, pCtx); }
    // Once, at the first drawn turn with mode 3 or more: the thread, the capture layer's functions, the log line that says what there is.
    void portPipeInit(int mode) {
        if (port_pipe25_active() != 0) {
            portLateInit();  // (GXV_PIPE25: the late draw's helper and buffers; the capture-layer engine below stays unused)
            return;
        }
        static bool sDone = false;
        if (sDone) {
            return;
        }
        sDone = true;
        if (port_pipe25_mode() != 0) {
            port_log_summary("pipe25: GXV_PIPE25=%d is ignored: GXV_DRAWPIPE=%d is set (the two do not run together)", port_pipe25_mode(), mode);
        }
        sPortPipe.watchdogUs = static_cast< uint32_t >(port_drawpipe_watchdog_ms()) * 1000u;
        sPortPipe.sliceUs = static_cast< uint32_t >(port_drawpipe_slice_us());
        if (port_gx_capture_begin && port_gx_capture_token && port_gx_capture_end && port_gx_replay) {
            sPortPipe.api.begin = port_gx_capture_begin;
            sPortPipe.api.token = port_gx_capture_token;
            sPortPipe.api.end = &portPipeApiEnd;
            sPortPipe.api.replay = &portPipeApiReplay;
        }
        if (port_gx_capture_set_token_handler) {
            sPortPipe.api.setHandler = port_gx_capture_set_token_handler;
        }
        if (port_gx_capture_token_keep) {
            sPortPipe.api.tokenKeep = port_gx_capture_token_keep;
        }
        if (port_gx_capture_abandon) {
            sPortPipe.api.abandon = port_gx_capture_abandon;
        }
        if (port_gx_capture_open) {
            sPortPipe.api.open = port_gx_capture_open;
        }
        // (the thread reports in before it moves itself to its core and priority, so "running" is known at once; the priority it
        // then gives itself is in every window line. A thread that was created is used: a job it does not start is taken back.)
        const bool thread = sPortPipe.worker.Start(port_drawpipe_core(), port_drawpipe_prio(), static_cast< uint32_t >(port_drawpipe_stack_kb()) * 1024u);
        port_log_summary("drawpipe: GXV_DRAWPIPE=%d%s: draw thread %s (asked for core %d, priority 0x%X; GXV_DRAWPIPE_CORE=%d GXV_DRAWPIPE_PRIO=0x%X); GX capture layer: %s; "
                         "watchdog %d ms, join slice %d us, settle %d turns, always %d; GXV_DRAWDONE_DEFER=%d%s; GXV_GXCAPTURE=%d%s; owner trap %s",
                         mode, mode >= 5 ? " (stage B is not built: runs as 4)" : "", !thread ? "NOT CREATED: every draw is serial"
                         : sPortPipe.worker.Reported() != 0 ? "created and running"
                                                            : "created; it has not run yet (a job it does not start is taken back by the game thread)",
                         sPortPipe.worker.Core(), static_cast< unsigned >(sPortPipe.worker.AskedPriority()), port_drawpipe_core(), static_cast< unsigned >(port_drawpipe_prio()),
                         sPortPipe.api.Complete() ? "present (it needs GXV_GXCAPTURE=1)"
                                                  : sPortPipe.api.begin != nullptr ? "present WITHOUT its token handler / abandon (mode 4 stays serial)"
                                                                                   : "absent (mode 4 stays serial)",
                         port_drawpipe_watchdog_ms(), port_drawpipe_slice_us(), port_drawpipe_settle(), port_drawpipe_always(), port_gx_drawdone_defer_mode(),
                         mode >= 4 && port_gx_drawdone_defer_mode() != 4 ? " (mode 4 needs 4: stays serial)" : "",
                         port_gx_capture_mode ? port_gx_capture_mode() : -1,
                         mode >= 4 && port_gx_capture_mode && port_gx_capture_mode() == 0 ? " (mode 4 needs 1: stays serial)"
                         : mode >= 4 && port_gx_capture_mode && port_gx_capture_mode() == 2 ? " (the layer's verify: recorded, nothing handed over)" : "",
                         &__PortGXCapTrap == nullptr ? "absent (a GX call by the logic during an overlap is not caught: GX-HOOKS.diff)"
                                                     : port_drawpipe_trap() ? "on" : "off (GXV_DRAWPIPE_TRAP=0)");
    }

    // After the engine's line (mode 4): what the capture layer counted in the same window, a capture begun = a drawn turn that
    // recorded. Read with the line above it: "recorded calls" are the OPEN lists' calls only (a J3D pass is one token, its calls
    // are made directly by the draw thread and never recorded); the layer's "capture open" is the game thread's time between
    // begin and end, its "replay" the draw thread's inside port_gx_replay (the passes included).
    void portPipeLayerLine() {
        if (!port_gx_capture_stats) {
            return;
        }
        static PortGxCaptureStats sLast;
        PortGxCaptureStats now;
        port_gx_capture_stats(&now);
        const uint64_t begun = now.begun - sLast.begun;
        const double per = begun ? 1.0 / static_cast< double >(begun) : 0.0;
        port_log_summary("drawpipe: the capture layer in that window: %llu captures begun, %llu ended and handed over, %llu abandoned, %llu refused; "
                         "a capture: %.0f recorded calls, %.1f KiB of records, %.1f KiB raw, %.1f tokens; capture open %.3f ms, replay %.3f ms "
                         "(the passes included); foreign %llu, contract errors %llu",
                         (unsigned long long)begun, (unsigned long long)(now.ended - sLast.ended), (unsigned long long)(now.abandoned - sLast.abandoned),
                         (unsigned long long)(now.refused - sLast.refused), (now.calls - sLast.calls) * per, (now.recordBytes - sLast.recordBytes) * per / 1024.0,
                         (now.rawBytes - sLast.rawBytes) * per / 1024.0, (now.tokens - sLast.tokens) * per, (now.captureNs - sLast.captureNs) * per / 1e6,
                         (now.replayNs - sLast.replayNs) * (now.replayed - sLast.replayed ? 1.0 / static_cast< double >(now.replayed - sLast.replayed) : 0.0) / 1e6,
                         (unsigned long long)(now.foreign - sLast.foreign), (unsigned long long)(now.contractErrors - sLast.contractErrors));
        sLast = now;
    }

    // The turn's decision (DESIGN.md 1.4): port::drawpipe::kGo, or why this turn's draw is today's serial draw. Every test is
    // made every turn and *pHeld gets a bit for each that holds; the turn is counted under the first in the order below.
    //   A fur draw is no reason any more (dpengine 2): it is recorded like every other draw list, and the one thing it changes
    //   under the passes is carried (DrawBufferHolder.cpp, port_drawpipe_shape_words).
    //   Another guest thread having run makes THIS turn serial and nothing more: it used to restart the settling count too,
    //   and a thread that runs every eighth turn (the title screen has one) then kept every turn serial.
    int portPipeDecide(int mode, u32* pHeld) {
        using namespace port::drawpipe;
        u32 held = 0;
        const u32 branch = sPortPipeSceneStore.mBranch;  // (frozen at this hand-over: the flags the draw's branches test)
        f32 head[12], tanX, tanY;
        // (what the engine itself would refuse: known here so that such a turn freezes nothing either)
        const bool late = port_pipe25_active() != 0;  // (GXV_PIPE25: the late draw; the engine's own tests below, not the capture layer's)
        if (late ? (sLate == nullptr || sLate->Off()) : sPortPipe.Off()) {
            held |= 1u << kWhyOff;
        }
        if (late ? (sLate == nullptr || !sLate->worker.Ready() || !portLateTrapOk()) : (!sPortPipe.worker.Ready() || (mode >= 4 && !sPortPipe.api.Complete()))) {
            held |= 1u << kWhyConfig;
        }
        if (sPortPipeHost.mExecutor == nullptr) {
            held |= 1u << kWhyNoScene;
        } else {
            if (port_skip_fast_forwarding()) {
                held |= 1u << kWhySkip;
            }
            if (port::lockstep::On()) {
                // (fmt32: a lockstep session: every draw is made in order on the game thread, as with the pipeline off. The helper's draw overlaps the next turn's update; that
                // it writes nothing the logic reads is not proven (the mode 2 compare is unexplained), and two consoles must not depend on it.)
                held |= 1u << kWhySystem;
            }
            if ((branch & 0x100u) == 0) {
                held |= 1u << kWhySceneClass;
            } else {
                if ((branch & 0x88u) != 0) {
                    held |= 1u << kWhyPause;
                }
                if ((branch & 0x4u) == 0 || (branch & 0x42u) != 0 || (branch & 0x1u) == 0) {
                    held |= 1u << kWhyDemo;  // (another nerve than the action nerve, a demo, a movie, the 3D picture off)
                }
            }
            if ((branch & 0x1E00u) != 0) {
                held |= 1u << kWhySystem;
            }
            if ((branch & 0x2030u) != 0 || port_vr_cull_view(head, &tanX, &tanY) != 0) {
                held |= 1u << kWhyCapture;
            }
            if (sPortPipeFormSeen && sPortPipeTurn - sPortPipeFormTurn <= 2) {
                held |= 1u << kWhyFormSwitch;
            }
            if (sPortPipeLogicGxSeen && sPortPipeTurn - sPortPipeLogicGxTurn <= 30) {
                held |= 1u << kWhyLogicGx;
            }
            if (late && (port_gx_drawdone_defer_mode() != 4 || (port_gx_capture_mode && port_gx_capture_mode() != 0))) {
                // (the late draw needs the deferred draw-done of mode 4 and the capture layer OFF: it has its own FIFO redirect and trap)
                held |= 1u << kWhyConfig;
            } else if (!late && mode >= 4 && (port_gx_drawdone_defer_mode() != 4 || (port_gx_capture_mode && port_gx_capture_mode() == 0))) {
                // (the recording needs the deferred draw-done of mode 4 and the capture layer switched on. With the layer's
                // verify, GXV_GXCAPTURE=2, the draw is recorded and every pass executed at its token on this thread: nothing is
                // handed over, each turn counts as "capture abandoned", and the layer compares every recorded call and checks
                // the tokens' claims.)
                held |= 1u << kWhyConfig;
            }
        }
        // (the turns that must qualify in a row: counted over everything above)
        if (held != 0) {
            sPortPipeOkRun = 0;
        } else if (++sPortPipeOkRun <= static_cast< u32 >(port_drawpipe_settle())) {
            held |= 1u << kWhySettling;
        }
        // (another guest thread ran since the last hand-over: a loading thread at work, the scene's init. This turn only.)
        const unsigned runs = port_os_other_guest_runs ? port_os_other_guest_runs() : 0;
        if (runs != sPortPipeGuestRuns && port_drawpipe_loadcheck()) {
            held |= 1u << kWhyLoading;
            ++sPortPipeLoadingTurns;
        }
        sPortPipeGuestRuns = runs;
        *pHeld = held;
        static const int kOrder[] = {kWhyOff, kWhyNoScene, kWhySkip, kWhySceneClass, kWhyPause, kWhyDemo, kWhySystem, kWhyCapture, kWhyFormSwitch,
                                     kWhyLogicGx, kWhyConfig, kWhyLoading, kWhySettling};
        for (const int r : kOrder) {
            if (held & (1u << r)) {
                return r;
            }
        }
        return kGo;
    }
}  // namespace

extern "C" void port_drawpipe_point(int point) {
    const int mode = port_drawpipe_mode();
    if (mode == 0) {
        return;
    }
    GameSystem* system = SingletonHolder< GameSystem >::get();
    NameObjListExecutor* executor = portPipeExecutor();
    if (executor != sPortPipeHost.mExecutor) {
        // (another scene, or none: nothing hashed or frozen in the old one may be compared, named or drawn from again)
        DrawBufferHolder::portPipeUse(nullptr, nullptr);
        sPortPipeDriver.Reset();
        sPortPipeHost.mExecutor = executor;
    }
    sPortPipeHost.mScene = executor != nullptr ? system->mSceneController->mScene : nullptr;
    if (point == 0) {
        ++sPortPipeTurn;
        sPortPipeDriver.skipFreeze = false;
        if (mode >= 3) {
            // The turn's decision, before anything is frozen: a turn whose draw will be serial anyway freezes nothing and makes
            // no token (the first console runs paid mode 1's bookkeeping, about half a millisecond, on every serial turn).
            // The decision reads the scene's flags: the small state is taken first (again by the freeze, if there is one).
            portPipeInit(mode);
            if (executor != nullptr) {
                sPortPipeHost.FreezeSmall();
            } else {
                sPortPipeSceneStore.mBranch = 0;  // (no scene: the decision says so before it reads a flag)
            }
            sPortPipeWhy = portPipeDecide(mode, &sPortPipeHeld);
            if (mode == 3 && port_drawpipe_always() && executor != nullptr) {
                sPortPipeWhy = port::drawpipe::kGo;  // (GXV_DRAWPIPE_ALWAYS=1: the whole draw on the draw thread in every state; the game thread waits)
            }
            // (mode 3 draws live on the draw thread: the frame is not needed there either. Mode 4 needs it on a turn that goes.)
            sPortPipeDriver.skipFreeze = mode == 3 || sPortPipeWhy != port::drawpipe::kGo;
        }
    }
    sPortPipeDriver.Point(sPortPipeHost, point, mode, port_drawpipe_sample(), executor != nullptr);
    if (mode >= 3) {
        // The logic's own GX use in update() (fall-back state 11): the stream's write position after the draw against the one
        // after update(). Only a turn whose draw was finished at point 1 can tell (a serial turn, or mode 3: the draw thread
        // moves the position otherwise, and the pointer is not read then). What calcAnim() sends does not matter: the draw
        // thread is joined before it.
        if (point == 1) {
            sPortPipeStreamValid = !portPipeOverlapped();
            if (sPortPipeStreamValid) {
                sPortPipeStreamAt = __PortGXFifoPtr;
            }
        } else if (point == 2) {
            if (sPortPipeStreamValid && !portPipeOverlapped() && __PortGXFifoPtr != sPortPipeStreamAt) {
                sPortPipeLogicGxSeen = true;
                sPortPipeLogicGxTurn = sPortPipeTurn;
            }
            sPortPipeStreamValid = false;
        }
        if (point == 3 && sLate != nullptr && port_pipe25_active() != 0) {
            sLate->MaybeLog(portLateGameRef(), port_pipe25_active(), 300);
            // (hazard32) the proof line of the light-direction fix (LightFunction.cpp), every 300 turns as the census
            static u32 sDirTurns = 0;
            if (++sDirTurns >= 300) {
                sDirTurns = 0;
                port_log_summary("pipe25: light directions: %u light objects (diffuse lights, the point light) loaded with the constant direction in the last 300 turns "
                                 "(the game leaves it unset: the stack's bytes, which differ between the game thread and the helper; never used by the lighting)",
                                 static_cast< unsigned >(port_light_dir_fixed_take()));
            }
        } else if (point == 3 && sPortPipe.MaybeLog(sPortPipeGame, mode, 300)) {
            if (sPortPipeLoadingTurns != 0 && port_os_other_guest_last) {
                int priority = -1;
                void* stack = nullptr;
                void* thread = port_os_other_guest_last(&priority, &stack);
                port_log_summary("drawpipe: another guest thread (not main, not audio) had run at %u hand-overs of that window; the last one: OSThread %p, "
                                 "guest priority %d, stack base %p (JAudio's disc thread is not counted; GXV_DRAWPIPE_LOADCHECK=0 stops counting them)", sPortPipeLoadingTurns, thread, priority, stack);
            }
            sPortPipeLoadingTurns = 0;
            if (mode >= 4) {
                portPipeLayerLine();
            }
        }
    }
}

// A turn without a picture ran (its update and calc-anim are not between a hand-over and the next).
extern "C" void port_drawpipe_undrawn(void) {
    if (port_drawpipe_mode() == 2) {
        sPortPipeDriver.Undrawn();
    }
}

// GXV_DRAWPIPE >= 3 (GameSystem::frameLoop, in place of the serial draw): the turn's draw, in its three parts: pBegin
// (beginRender), pMiddle (draw()), pEnd (endRender). Mode 3: all three run on the draw thread while the caller waits. Mode 4:
// the begin runs here; the middle runs here with the GX library recording and the recording is handed to the draw thread; the
// end runs here at the join (port_drawpipe_join before calcAnim(), or earlier when the logic needs GX), after the draw thread
// has put the middle into the stream. Whenever the turn does not qualify, or anything is missing, the three simply run here,
// one after the other, as always.
extern "C" void port_drawpipe_draw(void (*pBegin)(void*), void (*pMiddle)(void*), void (*pEnd)(void*), void* pCtx) {
    const int mode = port_drawpipe_mode();
    portPipeInit(mode);
    sPortPipeBegin = pBegin;
    sPortPipeMiddle = pMiddle;
    sPortPipeEnd = pEnd;
    sPortPipeDrawCtx = pCtx;
    // (decided at the hand-over, before the freeze: port_drawpipe_point(0). A turn that qualified and whose lists could not be
    // frozen is serial too.)
    int why = sPortPipeWhy;
    u32 held = sPortPipeHeld;
    if (why == port::drawpipe::kGo && mode >= 4 && !sPortPipeDriver.frame.valid) {
        why = port::drawpipe::kWhyNoScene;
        held |= 1u << port::drawpipe::kWhyNoScene;
    }
    if (port_pipe25_active() != 0 && sLate != nullptr) {
        // GXV_PIPE25: the late draw (port/draw_thread.h class Late): 6 / 7 handed to the helper, 0 drawn here (also after an abandon)
        sLate->NoteHeld(held);
        sLate->Draw(portLateGameRef(), port_pipe25_active(), why);
        return;
    }
    sPortPipe.NoteHeld(held);
    sPortPipe.Draw(sPortPipeGame, mode >= 4 ? 4 : 3, why);
}
// Before calcAnim(), and before anything that frees what the draw reads (a scene's destruction): the draw thread's draw is over.
extern "C" int port_drawpipe_join(void) {
    if (sLate != nullptr && sLate->Overlapped()) {
        sLate->Join(portLateGameRef());
        return 1;
    }
    if (!sPortPipe.Overlapped()) {
        return 0;
    }
    sPortPipe.Join(sPortPipeGame);
    return 1;
}
// pipe61, GXV_PIPE25_JOINLATE=1 (default 0): the late draw's helper is joined after calcAnim() instead of before it. GameSystem::frameLoop asks right after
// update(): 1 = the join is made later, by port_drawpipe_join_after_calc (calcAnim() runs beside the helper too; the overlap is update + calc-anim, ~1 ms
// longer: the join waits of the console runs (0.3-0.67 ms a late turn, 20-30 % of the joins waited, 3.6 ms the longest) are what it takes back).
// The fence claim is the one the header states for the logic of turn N+1: it writes only the half the picture does not read. GXV_PIPE25=2 compares every
// pass with the helper running beside update AND calcAnim: that is its proof.
extern "C" int port_drawpipe_join_late(void) {
    if (sLate == nullptr || port_pipe25_active() == 0 || port_pipe25_joinlate() == 0 || sPortLateJoinLateOff || !sLate->Overlapped()) {
        sPortLateInCalc = false;
        return 0;
    }
    sPortLateInCalc = true;
    return 1;
}
// fx66: calcAnim's material work (ModelManager::updateDL: material animation players, calcMaterial's texture matrices and material blocks, the display-list diff) runs
// inside the flush scope, so port_gx_view_write_from does not reach port_drawpipe_game_enter there. With the join after calcAnim() it must not overlap the helper's passes,
// which read those blocks / matrices: the join is made here, first, and counted apart (no strike against JOINLATE). Nothing outside that span / with no helper out.
extern "C" void port_drawpipe_join_before_material(void) {
    if (!sPortLateInCalc || sLate == nullptr || !sLate->Overlapped() || !port_on_frame_thread()) {
        return;
    }
    sPortLateInCalc = false;
    const int64_t t0 = port_host_time_ns();
    sLate->Join(portLateGameRef());
    ++sPortLateJoinedMat;
    sPortLateJoinedMatNs += static_cast< u32 >((port_host_time_ns() - t0) / 1000);
}
extern "C" int port_drawpipe_join_after_calc(void) {
    sPortLateInCalc = false;
    if (sLate == nullptr || !sLate->Overlapped()) {
        return 0;  // (joined early by a write in calcAnim(), see port_drawpipe_game_enter)
    }
    const int64_t t0 = port_host_time_ns();
    sLate->Join(portLateGameRef());
    ++sPortLateJoinedLate;
    sPortLateJoinedLateNs += static_cast< u32 >((port_host_time_ns() - t0) / 1000);
    return 1;
}
// The logic is about to use GX or to write a list / matrix half while the draw thread draws (port_gx_view_write_from, and the GX
// library's own entry points once the capture layer calls this: GX-HOOKS.diff): the draw is joined first, and the next turns are
// serial. Nothing on the draw thread itself, and nothing when no draw is out.
extern "C" void port_drawpipe_game_enter(void) {
    if (sLate != nullptr && port_pipe25_active() != 0) {
        // GXV_PIPE25: the same four cases as below, for the late draw's helper
        if (sLate->OnHelper()) {
            return;
        }
        if (!port_on_frame_thread()) {
            sLate->WaitOther(portLateGameRef());
            return;
        }
        if (sLate->Capturing()) {
            sLate->Abandon(portLateGameRef(), port::drawpipe::kWhyDrawWrite);  // (a list or matrix half written inside the draw: the passes made so far run now)
            return;
        }
        if (sLate->Overlapped() && sPortLateInCalc) {
            // (pipe61) the logic's own write inside calcAnim() with the join not made yet: joined here (the write waits for it), and counted apart from update()'s
            if (sLate->GameEnter(portLateGameRef()) && ++sPortLateCalcEarly >= 3 && !sPortLateJoinLateOff) {
                sPortLateJoinLateOff = true;
                port_log_summary("pipe25: GXV_PIPE25_JOINLATE: OFF for this session: calcAnim() wrote a matrix half, a list or called GX with the helper out %u times "
                                 "(each joined it at the write); the join goes back before calcAnim()", sPortLateCalcEarly);
            }
            sPortLateInCalc = false;
            return;
        }
        if (sLate->Overlapped() && sLate->GameEnter(portLateGameRef())) {
            sPortPipeLogicGxSeen = true;
            sPortPipeLogicGxTurn = sPortPipeTurn;
        }
        return;
    }
    if (sPortPipe.OnDrawThread()) {
        return;  // (the draw thread's own GX calls pass the same hooks: the cheapest test first)
    }
    if (!port_on_frame_thread()) {
        // (another guest thread, a loading thread writing a list: it waits until the draw thread's job is over, and does not
        // touch the join, which is the game thread's)
        sPortPipe.WaitOther(sPortPipeGame);
        return;
    }
    if (sPortPipe.Capturing()) {
        // (a list or matrix half written inside the draw itself while it is recorded: the recorded calls carry pointers to it)
        sPortPipe.Abandon(port::drawpipe::kWhyDrawWrite);
        return;
    }
    if (sPortPipe.Overlapped() && sPortPipe.GameEnter(sPortPipeGame)) {
        sPortPipeLogicGxSeen = true;
        sPortPipeLogicGxTurn = sPortPipeTurn;
    }
}
// (os_thread.cpp's owner check: a guest-OS call on the draw thread during an overlap; it did nothing)
extern "C" void port_drawpipe_guest_call(void* pFrom) {
    if (sLate != nullptr && sLate->JobOut()) {
        sLate->NoteGuestCall(pFrom);
        return;
    }
    sPortPipe.NoteGuestCall(pFrom);
}
// (pipefix31, gx_capture.cpp's owner trap) the caller is the late draw's helper, with a job out
extern "C" int port_drawpipe_on_helper(void) { return sLate != nullptr && sLate->OnHelper() ? 1 : 0; }
// What the decision remembers: 0 a player form switch (MarioActor::changeDisplayMode).
extern "C" void port_drawpipe_note(int what) {
    if (port_drawpipe_mode() < 3) {
        return;
    }
    if (what == 0) {
        sPortPipeFormSeen = true;
        sPortPipeFormTurn = sPortPipeTurn;
    }
}
#endif
