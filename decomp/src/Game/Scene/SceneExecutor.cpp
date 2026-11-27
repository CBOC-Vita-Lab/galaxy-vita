#include "Game/Map/Air.hpp"
#include "Game/MapObj/SpiderThread.hpp"
#include "Game/MapObj/SpinDriverPathDrawer.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Scene/StopSceneController.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Util.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <JSystem/J3DGraphBase/J3DSys.hpp>

void SceneFunction::movementStopSceneController() {
    MR::getSceneObj< StopSceneController >(SceneObj_StopSceneController)->movement();
}

#ifdef TARGET_PC
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF=3, game24: the movement-list function's own parts timed every turn)
#define PORT_ML_PART(id, stmt) do { PORT_LOGIC_COARSE(id); stmt; } while (0)
#else
#define PORT_ML_PART(id, stmt) stmt
#endif
#ifdef TARGET_PC
#include "Game/LiveActor/VolumeModelDrawer.hpp"  // (GXV_LIGHTVOL, lightvol25)
#include "port/actor_lod.h"  // (GXV_ALOD, actorlod23: the turn counter and this turn's camera for the update LOD of far actors)
#endif

void SceneFunction::executeMovementList() {
#ifdef TARGET_PC
    port_alod_turn_begin();  // (every turn, also a stopped one: a left-out movement's calcAnim is left out in that turn only)
#endif
    if (MR::getSceneObj< StopSceneController >(SceneObj_StopSceneController)->isSceneStopped()) {
        return;
    }

#ifdef TARGET_PC
    PORT_LOGIC_COARSE(PORT_LS_U_MOVELISTS);  // (GXV_LOGIC_PROF=3: this function whole, and its parts that are not category lists)
#endif
    PORT_ML_PART(PORT_LS_U_ML_CTRL, {
        MR::getSceneNameObjMovementController()->movement();
        MR::executeRequirementDisconnectDrawDelay();
    });
    CategoryList::execute(MR::MovementType_StopSceneDelayRequest);
    CategoryList::execute(MR::MovementType_Camera);
    CategoryList::execute(MR::MovementType_MirrorCamera);
    PORT_ML_PART(PORT_LS_U_ML_CAMMTX, {
        MR::setStarPointerCameraMtxAtGameScene();
        MR::loadViewMtx();
    });
    CategoryList::execute(MR::MovementType_ClippingDirector);
#ifdef TARGET_PC
    port_alod_camera_ready();  // (the camera of this turn: the actors' lists below use its view pyramid)
#endif
    PORT_ML_PART(PORT_LS_U_ML_REQ, {
        MR::executeRequirementConnectMovement();
        MR::executeRequirementConnectDraw();
        MR::executeRequirementDisconnectMovement();
        MR::executeRequirementDisconnectDraw();
    });
    CategoryList::execute(MR::MovementType_ScreenEffect);
    CategoryList::execute(MR::MovementType_SensorHitChecker);
    CategoryList::execute(MR::MovementType_MsgSharedGroup);
    CategoryList::execute(MR::MovementType_UNK_0x07);
    CategoryList::execute(MR::MovementType_UNK_0x14);
    CategoryList::execute(MR::MovementType_TalkDirector);
    CategoryList::execute(MR::MovementType_DemoDirector);
    CategoryList::execute(MR::MovementType_UNK_0x0C);
    CategoryList::execute(MR::MovementType_ClippedMapParts);
    CategoryList::execute(MR::MovementType_Planet);
    CategoryList::execute(MR::MovementType_CollisionMapObj);
    CategoryList::execute(MR::MovementType_CollisionEnemy);
    CategoryList::execute(MR::CalcAnimType_ClippedMapParts);
    CategoryList::execute(MR::CalcAnimType_Planet);
    CategoryList::execute(MR::CalcAnimType_CollisionMapObj);
    CategoryList::execute(MR::CalcAnimType_CollisionEnemy);
    CategoryList::execute(MR::MovementType_CollisionDirector);
    CategoryList::execute(MR::MovementType_Environment);
    CategoryList::execute(MR::MovementType_MapObj);
    CategoryList::execute(MR::MovementType_MapObjDecoration);
    CategoryList::execute(MR::MovementType_UNK_0x15);
    CategoryList::execute(MR::MovementType_NPC);
    CategoryList::execute(MR::MovementType_Ride);
    CategoryList::execute(MR::MovementType_Player);
    CategoryList::execute(MR::MovementType_PlayerDecoration);
    CategoryList::execute(MR::MovementType_Enemy);
    CategoryList::execute(MR::MovementType_EnemyDecoration);
    CategoryList::execute(MR::MovementType_Item);
    CategoryList::execute(MR::MovementType_PlayerMessenger);
    CategoryList::execute(MR::MovementType_AreaObj);
    CategoryList::execute(MR::MovementType_Layout);
    CategoryList::execute(MR::MovementType_LayoutDecoration);
    CategoryList::execute(MR::MovementType_MovieSubtitles);
    CategoryList::execute(MR::MovementType_WipeLayout);
    CategoryList::execute(MR::MovementType_Movie);
    CategoryList::execute(MR::MovementType_Sky);
    CategoryList::execute(MR::MovementType_ImageEffect);
    CategoryList::execute(MR::MovementType_AudEffectDirector);
    CategoryList::execute(MR::MovementType_AudBgmConductor);
    CategoryList::execute(MR::MovementType_AudCameraWatcher);
    CategoryList::execute(MR::MovementType_CameraCover);
    CategoryList::execute(MR::MovementType_SwitchWatcherHolder);
    PORT_ML_PART(PORT_LS_U_ML_REQ, {
        MR::executeRequirementDisconnectMovement();
        MR::executeRequirementDisconnectDraw();
    });
}

void SceneFunction::executeMovementListOnPlayingMovie() {
#ifdef TARGET_PC
    port_alod_turn_begin();  // (GXV_ALOD: a turn without the actors' lists still counts)
#endif
    MR::getSceneNameObjMovementController()->movement();
    MR::executeRequirementDisconnectDrawDelay();
    CategoryList::execute(MR::MovementType_StopSceneDelayRequest);
    CategoryList::execute(MR::MovementType_Layout);
    CategoryList::execute(MR::MovementType_LayoutDecoration);
    CategoryList::execute(MR::MovementType_MovieSubtitles);
    CategoryList::execute(MR::MovementType_WipeLayout);
    CategoryList::execute(MR::MovementType_Movie);
}

#ifdef TARGET_PC
extern "C" void port_par_anim_dispatch(void);  // (GXV_PAR_ANIM: decomp/src/Game/LiveActor/ModelManager.cpp)
extern "C" void port_par_late_prepare(void);   // (GXV_PAR_LATE, par2: decomp/src/Game/LiveActor/ModelManager.cpp)
extern "C" void port_par_late_end(void);
// GXV_PAR_PTCL_OVERLAP (gamepar, 8 Nov; perf.cpp, JPAEmitterManager.cpp): the particle list runs first, armed (its parallel chains go to
// the worker), the game thread then casts the shadows, then joins the particle calc. The shadow list and the particle calc share no
// data: the calc reads and writes emitters, particles, the pool and its resources, and reads (through MultiEmitterCallBack) the host
// actors' matrices / SRT and the MultiEmitters, none of which a ShadowController writes; the shadows write their controllers (and the
// shadow, gravity and collision memos), none of which the calc reads. ParticleEmitterHolder::update (now before the shadows) touches
// emitters and their callbacks only. Kind 2 callbacks, deletions and turns that are not parallel run after the shadows, as before.
extern "C" int port_par_ptcl_overlap_on(void);
extern "C" void port_ptcl_overlap_arm(int on);
extern "C" void port_ptcl_overlap_finish(void);
#include "port/turn_prof.h"  // (GXV_TURN_PROF: the shadows and the particles timed apart)
#endif

void SceneFunction::executeCalcAnimList() {
#ifdef TARGET_PC
    {
        PORT_LOGIC_COARSE(PORT_LS_C_LATE);
        port_par_late_prepare();  // (GXV_PAR_LATE: this turn's joint computations of par-anim's models handed to the fourth core)
    }
#endif
    MR::loadViewMtx();
#ifdef TARGET_PC
    port_turn_prof_cat_begin();  // (undrawn26, GXV_TURN_PROF=2: each list timed)
#endif
    CategoryList::execute(MR::CalcAnimType_Environment);
#ifdef TARGET_PC
    port_turn_prof_cat(0);
#endif
    CategoryList::execute(MR::CalcAnimType_MapObj);
#ifdef TARGET_PC
    port_turn_prof_cat(1);
#endif
    CategoryList::execute(MR::CalcAnimType_NPC);
#ifdef TARGET_PC
    port_turn_prof_cat(2);
#endif
    CategoryList::execute(MR::CalcAnimType_Ride);
#ifdef TARGET_PC
    port_turn_prof_cat(3);
#endif
    CategoryList::execute(MR::CalcAnimType_Enemy);
#ifdef TARGET_PC
    port_turn_prof_cat(4);
#endif
    CategoryList::execute(MR::CalcAnimType_Player);
#ifdef TARGET_PC
    port_turn_prof_cat(5);
#endif
    CategoryList::execute(MR::CalcAnimType_PlayerDecoration);
#ifdef TARGET_PC
    port_turn_prof_cat(6);
#endif
    CategoryList::execute(MR::CalcAnimType_MapObjDecoration);
#ifdef TARGET_PC
    port_turn_prof_cat(7);
#endif
    CategoryList::execute(MR::CalcAnimType_Item);
#ifdef TARGET_PC
    port_turn_prof_cat(8);
#endif
    CategoryList::execute(MR::CalcAnimType_Layout);
#ifdef TARGET_PC
    port_turn_prof_cat(9);
#endif
    CategoryList::execute(MR::CalcAnimType_LayoutDecoration);
#ifdef TARGET_PC
    port_turn_prof_cat(10);
#endif
    CategoryList::execute(MR::CalcAnimType_UNK_0x12);
#ifdef TARGET_PC
    port_turn_prof_cat(11);
#endif
    CategoryList::execute(MR::CalcAnimType_MirrorMapObj);
#ifdef TARGET_PC
    port_turn_prof_cat(12);
#endif
#ifdef TARGET_PC
    port_turn_prof_lap(PORT_TP_CALC);
    if (port_par_ptcl_overlap_on()) {
        port_ptcl_overlap_arm(1);
        CategoryList::execute(MR::CalcAnimType_AnimParticle);  // (the emitter holder, then the chains handed to the worker)
        port_ptcl_overlap_arm(0);
        port_turn_prof_lap(PORT_TP_PTCL);
        CategoryList::execute(MR::MovementType_ShadowControllerHolder);
        port_turn_prof_lap(PORT_TP_SHADOW);
        {
            PORT_LOGIC_COARSE(PORT_LS_C_PTCLFIN);
            port_ptcl_overlap_finish();  // (kind 2 callbacks, the claims, the join, the deletions; or the serial loops)
        }
        port_turn_prof_lap(PORT_TP_PTCL);
    } else {
        CategoryList::execute(MR::MovementType_ShadowControllerHolder);
        port_turn_prof_lap(PORT_TP_SHADOW);
        CategoryList::execute(MR::CalcAnimType_AnimParticle);
        port_turn_prof_lap(PORT_TP_PTCL);
    }
#else
    CategoryList::execute(MR::MovementType_ShadowControllerHolder);
    CategoryList::execute(MR::CalcAnimType_AnimParticle);
#endif
#ifdef TARGET_PC
    {
        PORT_LOGIC_COARSE(PORT_LS_C_ANIMDISP);
        port_par_late_end();       // (GXV_PAR_LATE: no late job is read after calc-anim)
        port_par_anim_dispatch();  // (GXV_PAR_ANIM: the next computations of the models that computed lately go to the worker)
    }
#endif
}

void SceneFunction::executeCalcAnimListOnPlayingMovie() {
    CategoryList::execute(MR::CalcAnimType_MovieSubtitles);
}

#ifdef TARGET_PC
// Skies are modelled around the game camera; the VR renderer draws the
// draws between these markers around the eye instead.
// (hrw27, GXV_HRW: the same draws are owned by the sky: the replay re-projects them with the camera's rotation only, they are parented to the camera)
#define PORT_SKY_BUFFER(draw)                                                  \
    do {                                                                       \
        port_gx_marker(PORT_GX_MARK_SKY_BEGIN);                                \
        port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_SKY, PORT_HRW_PH_SKY));      \
        draw;                                                                  \
        port_gx_marker(PORT_GX_OWNER(0, 0));                                   \
        port_gx_marker(PORT_GX_MARK_SKY_END);                                  \
    } while (0)
// What Mario wears and carries moves with him: the VR renderer gives it his
// motion (SpaceWarp).
// (hrw27, GXV_HRW: and they are Mario's draws, owned in the phase of their pass: the pair's partial draw makes them again for the second picture's camera)
#define PORT_PLAYER_BUFFER(phase, draw)                                        \
    do {                                                                       \
        port_gx_marker(PORT_GX_MARK_PLAYER_BEGIN);                             \
        port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_MARIO, (phase)));            \
        draw;                                                                  \
        port_gx_marker(PORT_GX_OWNER(0, 0));                                   \
        port_gx_marker(PORT_GX_MARK_PLAYER_END);                               \
    } while (0)
#else
#define PORT_SKY_BUFFER(draw) draw
#define PORT_PLAYER_BUFFER(phase, draw) draw
#endif

void SceneFunction::executeCalcViewAndEntryList() {
    TMtx34f mtx;
    mtx.identity();
    PSMTXCopy(mtx, j3dSys.mViewMtx);
    CategoryList::entryDrawBuffer2D();
    MR::loadViewMtx();
    CategoryList::entryDrawBuffer3D();
}

#ifdef TARGET_PC
void SceneFunction::executeCalcViewAndEntryListPlayer() {
    MR::loadViewMtx();
    CategoryList::entryDrawBufferPlayer();
}

void SceneFunction::executeCalcViewAndEntryListExceptPlayer() {
    TMtx34f mtx;
    mtx.identity();
    PSMTXCopy(mtx, j3dSys.mViewMtx);
    CategoryList::entryDrawBuffer2D();
    MR::loadViewMtx();
    CategoryList::entryDrawBuffer3DExceptPlayer();
}
#endif

void SceneFunction::executeCalcViewAndEntryList2D() {
    TMtx34f mtx;
    mtx.identity();
    PSMTXCopy(mtx, j3dSys.mViewMtx);
    CategoryList::entryDrawBuffer2D();
}

#ifdef TARGET_PC
// geopass23 (GXV_GEOPASS): the draw phase from here on, as a GX stream marker (written only with the switch on: vgx_galaxy.cpp)
#define PORT_GEO_PHASE(n) port_gx_marker(PORT_GX_MARK_PHASE | ((n) << 8))
#else
#define PORT_GEO_PHASE(n) \
    do {                  \
    } while (0)
#endif

void SceneFunction::executeDrawBufferListNormalOpaBeforeVolumeShadow() {
    PORT_GEO_PHASE(1);
    CategoryList::drawOpa(MR::DrawBufferType_CrystalItem);
    CategoryList::drawXlu(MR::DrawBufferType_CrystalItem);
    CategoryList::drawOpa(MR::DrawBufferType_Crystal);
    CategoryList::drawXlu(MR::DrawBufferType_Crystal);
    CategoryList::drawOpa(MR::DrawBufferType_AstroDomeSky);
    CategoryList::drawXlu(MR::DrawBufferType_AstroDomeSky);

    if (MR::isExistPriorDrawAir()) {
        PORT_SKY_BUFFER(CategoryList::drawOpa(MR::DrawBufferType_Sky));
        CategoryList::drawOpa(MR::DrawBufferType_Air);
        CategoryList::drawOpa(MR::DrawBufferType_Sun);
        PORT_SKY_BUFFER(CategoryList::drawXlu(MR::DrawBufferType_Sky));
        CategoryList::drawXlu(MR::DrawBufferType_Air);
        CategoryList::drawXlu(MR::DrawBufferType_Sun);
    }

    if (MR::isDrawSpinDriverPathAtOpa()) {
        MR::reinitGX();
        CategoryList::execute(MR::DrawType_SpinDriverPathDrawer);
    }

    PORT_GEO_PHASE(2);
    CategoryList::drawOpa(MR::DrawBufferType_Planet);
    CategoryList::drawOpa(MR::DrawBufferType_PlanetLow);
    CategoryList::execute(MR::DrawType_FlexibleSphere);
    PORT_GEO_PHASE(3);
    CategoryList::drawOpa(MR::DrawBufferType_Environment);
    CategoryList::drawOpa(MR::DrawBufferType_MapObj);
    CategoryList::drawOpa(MR::DrawBufferType_MapObjWeakLight);
    CategoryList::drawOpa(MR::DrawBufferType_MapObjStrongLight);
    CategoryList::drawOpa(MR::DrawBufferType_TripodBoss);
}

void SceneFunction::executeDrawBufferListNormalOpaBeforeSilhouette() {
    CategoryList::drawOpa(MR::DrawBufferType_NoShadowedMapObj);
    CategoryList::drawOpa(MR::DrawBufferType_NoShadowedMapObjStrongLight);
}

void SceneFunction::executeDrawBufferListNormalOpa() {
    CategoryList::drawOpa(MR::DrawBufferType_NoSilhouettedMapObj);
    CategoryList::drawOpa(MR::DrawBufferType_NoSilhouettedMapObjWeakLight);
    CategoryList::drawOpa(MR::DrawBufferType_NoSilhouettedMapObjStrongLight);
    CategoryList::drawOpa(MR::DrawBufferType_NPC);
    CategoryList::drawOpa(MR::DrawBufferType_Ride);
    CategoryList::drawOpa(MR::DrawBufferType_Enemy);
    CategoryList::drawOpa(MR::DrawBufferType_EnemyDecoration);
    PORT_PLAYER_BUFFER(PORT_HRW_PH_PLAYER_DECO_OPA, CategoryList::drawOpa(MR::DrawBufferType_PlayerDecoration));

    PORT_GEO_PHASE(8);
    if (!MR::isExistPriorDrawAir()) {
        PORT_SKY_BUFFER(CategoryList::drawOpa(MR::DrawBufferType_Sky));
        CategoryList::drawOpa(MR::DrawBufferType_Air);
        CategoryList::drawOpa(MR::DrawBufferType_Sun);
        PORT_SKY_BUFFER(CategoryList::drawXlu(MR::DrawBufferType_Sky));
        CategoryList::drawXlu(MR::DrawBufferType_Air);
        CategoryList::drawXlu(MR::DrawBufferType_Sun);
    }
}

void SceneFunction::executeDrawBufferListNormalXlu() {
    CategoryList::drawXlu(MR::DrawBufferType_Planet);
    CategoryList::drawXlu(MR::DrawBufferType_PlanetLow);
    CategoryList::drawXlu(MR::DrawBufferType_Environment);
    CategoryList::drawXlu(MR::DrawBufferType_EnvironmentStrongLight);
    CategoryList::drawXlu(MR::DrawBufferType_MapObj);
    CategoryList::drawXlu(MR::DrawBufferType_MapObjWeakLight);
    CategoryList::drawXlu(MR::DrawBufferType_MapObjStrongLight);
    CategoryList::drawXlu(MR::DrawBufferType_TripodBoss);
    CategoryList::drawXlu(MR::DrawBufferType_NoShadowedMapObj);
    CategoryList::drawXlu(MR::DrawBufferType_NoShadowedMapObjStrongLight);
    CategoryList::drawXlu(MR::DrawBufferType_NoSilhouettedMapObj);
    CategoryList::drawXlu(MR::DrawBufferType_NoSilhouettedMapObjWeakLight);
    CategoryList::drawXlu(MR::DrawBufferType_NoSilhouettedMapObjStrongLight);
    CategoryList::drawXlu(MR::DrawBufferType_NPC);
    CategoryList::drawXlu(MR::DrawBufferType_Ride);
    CategoryList::drawXlu(MR::DrawBufferType_Enemy);
    CategoryList::drawXlu(MR::DrawBufferType_EnemyDecoration);
    PORT_PLAYER_BUFFER(PORT_HRW_PH_PLAYER_DECO_XLU, CategoryList::drawXlu(MR::DrawBufferType_PlayerDecoration));
}

#ifdef TARGET_PC
// GXV_HRW (hrw27, stage d): the draws of Mario alone, in the order and the states the picture draws them in (the decoration group, Mario's body, the decoration group's translucent half),
// with the owner tags of the full picture. The render thread puts them in the place of the retained picture's own, drawn with the second picture's camera. The caller has set the draw state
// (GameScene::portHrwFresh). Returns 0 when there is no Mario to draw.
extern "C" u32 port_gx_draw_serial(void);  // (hitch33 13: the count through its getter, which also answers inside a GX capture scope, where the global stands still)
extern "C" u32 __PortGXDrawSerial;  // (the GX library's count of draws begun: GXBegin and display-list calls; read as JPAEmitterManager.cpp reads it)
int SceneFunction::executeDrawFreshPlayer() {
    if (!MR::isExistMario()) {
        return 0;
    }
    // warp32 PATCH17: MarioActor::draw draws nothing while the player is hidden (its own flags: a stage's arrival, a pipe, a cannon, a cut to a demo model); this
    // function still said "one partial draw", and the pair got an EMPTY partial draw (c130: "partial draw of pair 1: 0 commands" at the first pairs of a stage,
    // emulator and console). Now it says so when no draw was begun: the pair's ask then goes plain, with no job, as when there is no player.
    const u32 serial0 = port_gx_draw_serial();
    GXSetAlphaUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    PORT_PLAYER_BUFFER(PORT_HRW_PH_PLAYER_DECO_OPA, CategoryList::drawOpa(MR::DrawBufferType_PlayerDecoration));
    MR::loadLightPlayer();
    port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_MARIO, PORT_HRW_PH_PLAYER_DRAW));
    CategoryList::execute(MR::DrawType_Player);
    port_gx_marker(PORT_GX_OWNER(0, 0));
    PORT_PLAYER_BUFFER(PORT_HRW_PH_PLAYER_DECO_XLU, CategoryList::drawXlu(MR::DrawBufferType_PlayerDecoration));
    return port_gx_draw_serial() != serial0 ? 1 : 0;
}
#endif

void SceneFunction::executeDrawListOpa() {
    CategoryList::execute(MR::DrawType_0x3);
    MR::reinitGX();
    CategoryList::execute(MR::DrawType_0x52);
    CategoryList::execute(MR::DrawType_WhirlPoolAccelerator);
    CategoryList::execute(MR::DrawType_OceanRingPipeOutside);
    CategoryList::execute(MR::DrawType_0x32);
    CategoryList::execute(MR::DrawType_SwingRope);
    CategoryList::execute(MR::DrawType_0x1);
    CategoryList::execute(MR::DrawType_PlantStalk);
    CategoryList::execute(MR::DrawType_Plant);
    CategoryList::execute(MR::DrawType_Creeper);
    CategoryList::execute(MR::DrawType_Trapeze);
    CategoryList::execute(MR::DrawType_SpaceCocoon);
    CategoryList::execute(MR::DrawType_0x17);
    CategoryList::execute(MR::DrawType_WarpPodPath);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevDirect(GX_TEVSTAGE1);
    GXSetTevDirect(GX_TEVSTAGE2);
    GXSetTevDirect(GX_TEVSTAGE3);
    CategoryList::execute(MR::DrawType_KirairaChain);
    CategoryList::execute(MR::DrawType_WaterPlant);
    CategoryList::execute(MR::DrawType_BigBubbleGoal);
    CategoryList::execute(MR::DrawType_MiiFacePartsHolder);
    CategoryList::execute(MR::DrawType_AstroDomeOrbit);
    MR::loadLightPlayer();
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_OWNER(PORT_HRW_OWN_MARIO, PORT_HRW_PH_PLAYER_DRAW));  // (hrw27, GXV_HRW: Mario's body and what MarioActor::draw draws with it)
#endif
    CategoryList::execute(MR::DrawType_Player);
#ifdef TARGET_PC
    port_gx_marker(PORT_GX_OWNER(0, 0));
#endif
    CategoryList::execute(MR::DrawType_0x23);
    CategoryList::execute(MR::DrawType_Fur);
    CategoryList::execute(MR::DrawType_OceanSphere);
    CategoryList::execute(MR::DrawType_Flag);
    MR::resetTextureCacheSize();
}

void SceneFunction::executeDrawListXlu() {
    MR::reinitGX();
    CategoryList::execute(MR::DrawType_VolumeModel);
#ifdef TARGET_PC
    VolumeModelDrawer::flushDeferred();  // (GXV_LIGHTVOL, lightvol25: the recorded volumes, drawn as a list; a no-op with the switch at 0)
#endif
    CategoryList::execute(MR::DrawType_0x30);
    if (!MR::isDrawSpinDriverPathAtOpa()) {
        CategoryList::execute(MR::DrawType_SpinDriverPathDrawer);
    }
    CategoryList::execute(MR::DrawType_ClipAreaDropLaser);
    CategoryList::execute(MR::DrawType_SpiderThread);
    CategoryList::execute(MR::DrawType_DashRing);
    MR::resetTextureCacheSize();
}

#ifdef TARGET_PC
extern "C" int port_silhouette_hint(void);
#endif
void SceneFunction::executeDrawSilhouetteAndFillShadow() {
#ifdef TARGET_PC
    // GXV_SILH_GATE (shadow25): the hint for the silhouette fill's capture, written into the vgx GX stream only with the switch on
    if (const int hint = port_silhouette_hint()) {
        port_gx_marker(PORT_GX_MARK_SILH_HINT | (hint << 8));
    }
#endif
    MR::fillSilhouetteColor();
    MR::loadViewMtx();
    MR::loadProjectionMtx();
}

void SceneFunction::executeDrawAlphaShadow() {
    CategoryList::execute(MR::DrawType_AlphaShadow);
    MR::loadViewMtx();
}

void SceneFunction::executeDrawAfterIndirect() {
    CategoryList::drawOpa(MR::DrawBufferType_IndirectPlanet);
    CategoryList::drawOpa(MR::DrawBufferType_IndirectMapObj);
    CategoryList::drawOpa(MR::DrawBufferType_IndirectMapObjStrongLight);
    CategoryList::drawOpa(MR::DrawBufferType_IndirectNpc);
    CategoryList::drawOpa(MR::DrawBufferType_IndirectEnemy);
    CategoryList::drawOpa(MR::DrawBufferType_GlaringLight);
    CategoryList::drawOpa(MR::DrawBufferType_UNK_0x17);
    CategoryList::drawOpa(MR::DrawBufferType_CrystalBox);
    CategoryList::drawXlu(MR::DrawBufferType_IndirectPlanet);
    CategoryList::drawXlu(MR::DrawBufferType_IndirectMapObj);
    CategoryList::drawXlu(MR::DrawBufferType_IndirectMapObjStrongLight);
    CategoryList::drawXlu(MR::DrawBufferType_IndirectNpc);
    CategoryList::drawXlu(MR::DrawBufferType_IndirectEnemy);
    CategoryList::drawXlu(MR::DrawBufferType_GlaringLight);
    CategoryList::drawXlu(MR::DrawBufferType_UNK_0x17);
    CategoryList::drawXlu(MR::DrawBufferType_CrystalBox);
    MR::reinitGX();
    CategoryList::execute(MR::DrawType_0x11);
    CategoryList::execute(MR::DrawType_GCapture);
    CategoryList::execute(MR::DrawType_WaterRoad);
    CategoryList::execute(MR::DrawType_BigBubble);
    CategoryList::execute(MR::DrawType_ElectricRailHolder);
    CategoryList::execute(MR::DrawType_OceanRing);
    CategoryList::execute(MR::DrawType_OceanBowl);
    CategoryList::execute(MR::DrawType_0x20);
    CategoryList::execute(MR::DrawType_EffectDrawIndirect);
    CategoryList::execute(MR::DrawType_EffectDrawAfterIndirect);
    CategoryList::execute(MR::DrawType_OceanRingPipeInside);
    CategoryList::execute(MR::DrawType_WaterCameraFilter);
    MR::resetTextureCacheSize();
}

void SceneFunction::executeDrawImageEffect() {
    if (MR::isNormalBloomOn()) {
        MR::reinitGX();
        CategoryList::execute(MR::DrawType_BloomEffectPreDraw);

#ifdef TARGET_PC
        // (PETARI_BLOOM_HALFRATE: on a picture that adds the previous glow the glow models are not drawn: nothing captures them)
        if (MR::isNormalBloomForEachModel() && !ImageEffectLocalUtil::portBloomReusingNow()) {
#else
        if (MR::isNormalBloomForEachModel()) {
#endif
            CategoryList::drawOpa(MR::DrawBufferType_BloomModel);
            CategoryList::drawXlu(MR::DrawBufferType_BloomModel);
            CategoryList::execute(MR::DrawType_BloomModel);
            CategoryList::execute(MR::DrawType_EffectDrawForBloomEffect);
            MR::drawSpiderThreadBloom();
            CategoryList::execute(MR::DrawType_OceanBowlBloomDrawer);
        }

        CategoryList::execute(MR::DrawType_BloomEffectPostDraw);
    }

    CategoryList::execute(MR::DrawType_ImageEffect);
}

#ifdef TARGET_PC
extern "C" void port_layoutwarm_frame(void);  // (Game/Screen/LayoutManager.cpp: GXV_LAYOUTWARM / GXV_LAYOUTWARM_LOG)
#endif

void SceneFunction::executeDrawList2DNormal() {
    MR::drawInitFor2DModel();
#ifdef TARGET_PC
    port_layoutwarm_frame();
#endif
    CategoryList::drawOpa(MR::DrawBufferType_Model3DFor2D);
    CategoryList::drawXlu(MR::DrawBufferType_Model3DFor2D);
    CategoryList::execute(MR::DrawType_CometScreenFilter);
    CategoryList::execute(MR::DrawType_GalaxyNamePlate);
    CategoryList::execute(MR::DrawType_Layout);
    CategoryList::execute(MR::DrawType_LayoutDecoration);
    CategoryList::execute(MR::DrawType_CinemaFrame);
    CategoryList::execute(MR::DrawType_TalkLayout);
    CategoryList::execute(MR::DrawType_0x44);
    MR::drawInitFor2DModel();
    CategoryList::drawOpa(MR::DrawBufferType_0x25);
    CategoryList::drawXlu(MR::DrawBufferType_0x25);
    CategoryList::execute(MR::DrawType_EffectDraw2D);
    CategoryList::execute(MR::DrawType_EffectDrawFor2DModel);
}

void SceneFunction::executeDrawList2DMovie() {
    MR::drawInitFor2DModel();
    CategoryList::execute(MR::DrawType_Movie);
    CategoryList::execute(MR::DrawType_MovieSubtitles);
}
