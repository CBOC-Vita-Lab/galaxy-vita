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
#include "port/diagnostics.h"
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
#define PORT_DRAW_PROF(row, call)            \
    do {                                     \
        if (port_draw_prof_on()) {           \
            port_draw_prof_begin();          \
            call;                            \
            port_draw_prof_end(row);         \
        } else {                             \
            call;                            \
        }                                    \
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
    pListExecutor->executeCalcAnim(type);
    port_anim_lod_set_category(-1);
#else
    pListExecutor->executeCalcAnim(type);
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_category_end(animToken);
#endif
}

void CategoryList::execute(MR::DrawType type) {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_DRAW_PROF(static_cast< int >(type), pListExecutor->executeDraw(type));
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

    PORT_DRAW_PROF(0x100 | static_cast< int >(type), pListExecutor->drawOpa(type));
}

void CategoryList::drawXlu(MR::DrawBufferType type) {
    NameObjListExecutor* pListExecutor = SingletonHolder< GameSystem >::get()->mSceneController->getNameObjListExecutor();

    PORT_DRAW_PROF(0x200 | static_cast< int >(type), pListExecutor->drawXlu(type));
}

void CategoryList::requestMovementOn(MR::MovementType type) {
    MR::requestMovementOnWithCategory(type);
}

void CategoryList::requestMovementOff(MR::MovementType type) {
    MR::requestMovementOffWithCategory(type);
}

void SceneFunction::createHioBasicNode(Scene*) {
}
