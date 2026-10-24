#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/AreaObj/AreaObjContainer.hpp"
#include "Game/Boss/BossAccessor.hpp"
#include "Game/Boss/SkeletalFishBabyRailHolder.hpp"
#include "Game/Boss/SkeletalFishBossRailHolder.hpp"
#include "Game/Boss/TripodBossAccesser.hpp"
#include "Game/Camera/CameraContext.hpp"
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Demo/DemoDirector.hpp"
#include "Game/Demo/PrologueDirector.hpp"
#include "Game/Effect/EffectSystem.hpp"
#include "Game/Enemy/BegomanBase.hpp"
#include "Game/Enemy/KabokuriFireHolder.hpp"
#include "Game/Enemy/KameckBeamHolder.hpp"
#include "Game/Enemy/KarikariDirector.hpp"
#include "Game/Enemy/TakoHeiInkHolder.hpp"
#include "Game/GameAudio/AudBgmConductor.hpp"
#include "Game/GameAudio/AudCameraWatcher.hpp"
#include "Game/GameAudio/AudEffectDirector.hpp"
#include "Game/Gravity/PlanetGravityManager.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/LiveActor/LiveActorGroupArray.hpp"
#include "Game/LiveActor/MessageSensorHolder.hpp"
#include "Game/LiveActor/MirrorCamera.hpp"
#include "Game/LiveActor/SensorHitChecker.hpp"
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/LiveActor/ShadowSurfaceDrawer.hpp"
#include "Game/LiveActor/ShadowVolumeDrawer.hpp"
#include "Game/LiveActor/VolumeModelDrawer.hpp"
#include "Game/Map/Air.hpp"
#include "Game/Map/CollisionDirector.hpp"
#include "Game/Map/LightDirector.hpp"
#include "Game/Map/NamePosHolder.hpp"
#include "Game/Map/OceanHomeMapCtrl.hpp"
#include "Game/Map/PlanetMapCreator.hpp"
#include "Game/Map/QuakeEffectGenerator.hpp"
#include "Game/Map/RaceManager.hpp"
#include "Game/Map/SleepControllerHolder.hpp"
#include "Game/Map/SphereSelector.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/Map/SunshadeMapHolder.hpp"
#include "Game/Map/SwitchWatcherHolder.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Map/WaterPlant.hpp"
#include "Game/MapObj/AirBubbleHolder.hpp"
#include "Game/MapObj/ArrowSwitchMultiHolder.hpp"
#include "Game/MapObj/BigBubbleHolder.hpp"
#include "Game/MapObj/BigFanHolder.hpp"
#include "Game/MapObj/ChipHolder.hpp"
#include "Game/MapObj/ClipAreaDropHolder.hpp"
#include "Game/MapObj/ClipAreaHolder.hpp"
#include "Game/MapObj/ClipFieldFillDraw.hpp"
#include "Game/MapObj/CoinHolder.hpp"
#include "Game/MapObj/CoinRotater.hpp"
#include "Game/MapObj/EarthenPipe.hpp"
#include "Game/MapObj/ElectricRailHolder.hpp"
#include "Game/MapObj/FallOutFieldDraw.hpp"
#include "Game/MapObj/FirePressureBulletHolder.hpp"
#include "Game/MapObj/GCapture.hpp"
#include "Game/MapObj/MapPartsRailGuideHolder.hpp"
#include "Game/MapObj/MiniatureGalaxyHolder.hpp"
#include "Game/MapObj/Note.hpp"
#include "Game/MapObj/PurpleCoinHolder.hpp"
#include "Game/MapObj/SpiderThread.hpp"
#include "Game/MapObj/SpinDriverPathDrawer.hpp"
#include "Game/MapObj/StarPieceDirector.hpp"
#include "Game/MapObj/WarpPod.hpp"
#include "Game/MapObj/WaterPressureBulletHolder.hpp"
#include "Game/NPC/EventDirector.hpp"
#include "Game/NPC/MiiFaceIconHolder.hpp"
#include "Game/NPC/MiiFacePartsHolder.hpp"
#include "Game/NPC/NPCDirector.hpp"
#include "Game/NPC/TalkDirector.hpp"
#include "Game/NameObj/MovementOnOffGroupHolder.hpp"
#include "Game/NameObj/NameObjExecuteHolder.hpp"
#include "Game/NameObj/NameObjGroup.hpp"
#include "Game/Player/GroupChecker.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Player/PlayerEvent.hpp"
#include "Game/Ride/FluffWind.hpp"
#include "Game/Ride/PlantLeaf.hpp"
#include "Game/Ride/PlantStalk.hpp"
#include "Game/Ride/SwingRope.hpp"
#include "Game/Ride/Trapeze.hpp"
#include "Game/Scene/PlacementStateChecker.hpp"
#include "Game/Scene/SceneDataInitializer.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Scene/ScenePlayingResult.hpp"
#include "Game/Scene/StageDataHolder.hpp"
#include "Game/Scene/StopSceneController.hpp"
#include "Game/Screen/BloomEffect.hpp"
#include "Game/Screen/BloomEffectSimple.hpp"
#include "Game/Screen/CaptureScreenDirector.hpp"
#include "Game/Screen/CenterScreenBlur.hpp"
#include "Game/Screen/CinemaFrame.hpp"
#include "Game/Screen/CometRetryButton.hpp"
#include "Game/Screen/DepthOfFieldBlur.hpp"
#include "Game/Screen/GalaxyMapController.hpp"
#include "Game/Screen/GalaxyNamePlateDrawer.hpp"
#include "Game/Screen/GameSceneLayoutHolder.hpp"
#include "Game/Screen/HeatHazeEffect.hpp"
#include "Game/Screen/ImageEffectSystemHolder.hpp"
#include "Game/Screen/InformationObserver.hpp"
#include "Game/Screen/LensFlare.hpp"
#include "Game/Screen/MoviePlayerSimple.hpp"
#include "Game/Screen/MoviePlayingSequence.hpp"
#include "Game/Screen/OdhConverter.hpp"
#include "Game/Screen/PlayerActionGuidance.hpp"
#include "Game/Screen/SceneWipeHolder.hpp"
#include "Game/Screen/ScreenAlphaCapture.hpp"
#include "Game/Screen/ScreenBlurEffect.hpp"
#include "Game/Screen/StaffRoll.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/BaseMatrixFollowTargetHolder.hpp"
#include "Game/Util/FurCtrl.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ShareUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"

SceneObjHolder::SceneObjHolder() {
    for (int i = 0; i < SceneObj_NumMax; i++) {
        mObj[i] = nullptr;
    }
}

NameObj* SceneObjHolder::create(int id) {
    NameObj* pObj = mObj[id];

    if (pObj != nullptr) {
        return pObj;
    }

    pObj = newEachObj(id);
    pObj->initWithoutIter();

    mObj[id] = pObj;

    return pObj;
}

NameObj* SceneObjHolder::getObj(int id) const {
    return mObj[id];
}

bool SceneObjHolder::isExist(int id) const {
    return mObj[id] != nullptr;
}

NameObj* SceneObjHolder::newEachObj(int id) {
    switch (id) {
    case SceneObj_SensorHitChecker:
        return new SensorHitChecker("\x83\x5a\x83\x93\x83\x54\x81\x5b\x93\x96\x82\xbd\x82\xe8");
    case SceneObj_CollisionDirector:
        return new CollisionDirector();
    case SceneObj_ClippingDirector:
        return new ClippingDirector();
    case SceneObj_DemoDirector:
        return new DemoDirector("\x83\x66\x83\x82\x8e\x77\x8a\xf6");
    case SceneObj_EventDirector:
        return new EventDirector();
    case SceneObj_EffectSystem:
        return new EffectSystem("\x83\x47\x83\x74\x83\x46\x83\x4e\x83\x67\x83\x56\x83\x58\x83\x65\x83\x80", true);
    case SceneObj_LightDirector:
        return new LightDirector();
    case SceneObj_SceneDataInitializer:
        return new SceneDataInitializer();
    case SceneObj_StageDataHolder:
        return new StageDataHolder(MR::getCurrentStageName(), 0, true);
    case SceneObj_MessageSensorHolder:
        return new MessageSensorHolder("\x83\x56\x83\x58\x83\x65\x83\x80\x94\xc4\x97\x70\x83\x5a\x83\x93\x83\x54\x81\x5b");
    case SceneObj_StageSwitchContainer:
        return new StageSwitchContainer();
    case SceneObj_SwitchWatcherHolder:
        return new SwitchWatcherHolder();
    case SceneObj_SleepControllerHolder:
        return new SleepControllerHolder();
    case SceneObj_AreaObjContainer:
        return new AreaObjContainer("\x83\x47\x83\x8a\x83\x41\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67\x83\x52\x83\x93\x83\x65\x83\x69\x8a\xc7\x97\x9d");
    case SceneObj_LiveActorGroupArray:
        return new LiveActorGroupArray("\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67\x83\x4f\x83\x8b\x81\x5b\x83\x76");
    case SceneObj_MovementOnOffGroupHolder:
        return new MovementOnOffGroupHolder("Movement\x83\x4f\x83\x8b\x81\x5b\x83\x76\x8a\xc7\x97\x9d");
    case SceneObj_CaptureScreenActor:
        return new CaptureScreenActor(MR::DrawType_CaptureScreenIndirect, "Indirect");
    case SceneObj_AudCameraWatcher:
        return new AudCameraWatcher();
    case SceneObj_AudEffectDirector:
        return new AudEffectDirector();
    case SceneObj_AudBgmConductor:
        return new AudBgmConductor();
    case SceneObj_MarioHolder:
        return new MarioHolder();
    case SceneObj_MirrorCamera:
        return new MirrorCamera("\x8b\xbe\x97\x70\x83\x4a\x83\x81\x83\x89");
    case SceneObj_CameraContext:
        return new CameraContext();
    case SceneObj_NameObjGroup:
        return new NameObjGroup("IgnorePauseNameObj", 16);
    case SceneObj_TalkDirector:
        return new TalkDirector("\x89\xef\x98\x62\x83\x66\x83\x42\x83\x8c\x83\x4e\x83\x5e\x81\x5b");
    case SceneObj_EventSequencer:
        return new EventSequencer();
    case SceneObj_StopSceneController:
        return new StopSceneController();
    case SceneObj_SceneNameObjMovementController:
        return new SceneNameObjMovementController();
    case SceneObj_ImageEffectSystemHolder:
        return new ImageEffectSystemHolder();
    case SceneObj_BloomEffect:
        return new BloomEffect("\x83\x75\x83\x8b\x81\x5b\x83\x80");
    case SceneObj_BloomEffectSimple:
        return new BloomEffectSimple();
    case SceneObj_ScreenBlurEffect:
        return new ScreenBlurEffect("\x89\xe6\x96\xca\x83\x75\x83\x89\x81\x5b");
    case SceneObj_DepthOfFieldBlur:
        return new DepthOfFieldBlur("\x94\xed\x8e\xca\x8a\x45\x90\x5b\x93\x78\x83\x75\x83\x89\x81\x5b");
    case SceneObj_SceneWipeHolder:
        return new SceneWipeHolder();
    case SceneObj_PlayerActionGuidance:
        return new PlayerActionGuidance();
    case SceneObj_ScenePlayingResult:
        return new ScenePlayingResult();
    case SceneObj_LensFlareDirector:
        return new LensFlareDirector();
    case SceneObj_FurDrawManager:
        return new FurDrawManager(64);
    case SceneObj_PlacementStateChecker:
        return new PlacementStateChecker("\x83\x49\x83\x75\x83\x57\x83\x46\x83\x4e\x83\x67\x94\x7a\x92\x75\x8f\xf3\x91\xd4\x82\xcc\x8a\xc4\x8e\x8b");
    case SceneObj_NamePosHolder:
        return new NamePosHolder();
    case SceneObj_NPCDirector:
        return new NPCDirector();
    case SceneObj_ResourceShare:
        return new ResourceShare();
    case SceneObj_MoviePlayerSimple:
        return new MoviePlayerSimple();
    case SceneObj_InformationObserver:
        return new InformationObserver();
    case SceneObj_CenterScreenBlur:
        return new CenterScreenBlur();
    case SceneObj_OdhConverter:
        return new OdhConverter();
    case SceneObj_CometRetryButton:
        return new CometRetryButton("\x83\x52\x83\x81\x83\x62\x83\x67\x83\x8a\x83\x67\x83\x89\x83\x43\x83\x7b\x83\x5e\x83\x93");
    case SceneObj_AllLiveActorGroup:
        return new AllLiveActorGroup();
    case SceneObj_CameraDirector:
        return new CameraDirector("\x83\x4a\x83\x81\x83\x89\x8a\xc7\x97\x9d");
    case SceneObj_PlanetGravityManager:
        return new PlanetGravityManager("\x8f\x64\x97\xcd");
    case SceneObj_BaseMatrixFollowTargetHolder:
        return new BaseMatrixFollowTargetHolder("\x8d\x73\x97\xf1\x92\xc7\x90\x8f\x90\xe6\x83\x8a\x83\x58\x83\x67", 256, 256);
    case SceneObj_GameSceneLayoutHolder:
        return new GameSceneLayoutHolder();
    case SceneObj_TripodBossAccesser:
        return new TripodBossAccesser("\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x41\x83\x4e\x83\x5a\x83\x54");
    case SceneObj_KameckBeamHolder:
        return new KameckBeamHolder();
    case SceneObj_KameckFireBallHolder:
        return new KameckFireBallHolder();
    case SceneObj_KameckBeamTurtleHolder:
        return new KameckBeamTurtleHolder();
    case SceneObj_KabokuriFireHolder:
        return new KabokuriFireHolder();
    case SceneObj_TakoHeiInkHolder:
        return new TakoHeiInkHolder();
    case SceneObj_SwingRopeGroup:
        return new SwingRopeGroup("\x83\x58\x83\x43\x83\x93\x83\x4f\x83\x8d\x81\x5b\x83\x76\x95\x60\x89\xe6");
    case SceneObj_CoinHolder:
        return new CoinHolder("\x83\x52\x83\x43\x83\x93\x8a\xc7\x97\x9d");
    case SceneObj_PurpleCoinHolder:
        return new PurpleCoinHolder();
    case SceneObj_CoinRotater:
        return new CoinRotater("\x83\x52\x83\x43\x83\x93\x89\xf1\x93\x5d\x8a\xc7\x97\x9d");
    case SceneObj_AirBubbleHolder:
        return new AirBubbleHolder("\x8b\xf3\x8b\x43\x83\x41\x83\x8f\x8a\xc7\x97\x9d");
    case SceneObj_StarPieceDirector:
        return new StarPieceDirector("\x83\x58\x83\x5e\x81\x5b\x83\x73\x81\x5b\x83\x58\x8e\x77\x8a\xf6");
    case SceneObj_BegomanAttackPermitter:
        return new BegomanAttackPermitter("\x83\x78\x81\x5b\x83\x53\x83\x7d\x83\x93\x8d\x55\x8c\x82\x8b\x96\x89\xc2\x8e\xd2");
    case SceneObj_BigFanHolder:
        return new BigFanHolder();
    case SceneObj_KarikariDirector:
        return new KarikariDirector("\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x83\x66\x83\x42\x83\x8c\x83\x4e\x83\x5e\x81\x5b");
    case SceneObj_ShadowControllerHolder:
        return new ShadowControllerHolder();
    case SceneObj_ShadowVolumeDrawInit:
        return new ShadowVolumeDrawInit();
    case SceneObj_ShadowSurfaceDrawInit:
        return new ShadowSurfaceDrawInit("\x90\x85\x96\xca\x89\x65\x95\x60\x89\xe6\x8f\x89\x8a\xfa\x89\xbb");
    case SceneObj_PlantStalkDrawInit:
        return new PlantStalkDrawInit("\x90\x41\x95\xa8\x82\xcc\x8c\x73\x95\x60\x89\xe6\x8f\x89\x8a\xfa\x89\xbb");
    case SceneObj_PlantLeafDrawInit:
        return new PlantLeafDrawInit("\x95\x60\x89\xe6\x8f\x89\x8a\xfa\x89\xbb[\x90\x41\x95\xa8\x82\xcc\x97\x74]");
    case SceneObj_TrapezeRopeDrawInit:
        return new TrapezeRopeDrawInit("\x8b\xf3\x92\x86\x83\x75\x83\x89\x83\x93\x83\x52\x83\x8d\x81\x5b\x83\x76\x95\x60\x89\xe6");
    case SceneObj_VolumeModelDrawInit:
        return new VolumeModelDrawInit();
    case SceneObj_SpinDriverPathDrawInit:
        return new SpinDriverPathDrawInit();
    case SceneObj_NoteGroup:
        return new NoteGroup();
    case SceneObj_ClipAreaHolder:
        return new ClipAreaHolder("\x83\x4e\x83\x8a\x83\x62\x83\x76\x83\x47\x83\x8a\x83\x41\x83\x7a\x83\x8b\x83\x5f\x81\x5b");
    case SceneObj_ArrowSwitchMultiHolder:
        return new ArrowSwitchMultiHolder();
    case SceneObj_ClipAreaDropHolder:
        return new ClipAreaDropHolder();
    case SceneObj_FallOutFieldDraw:
        return new FallOutFieldDraw("\x83\x4e\x83\x8a\x83\x62\x83\x76\x83\x47\x83\x8a\x83\x41\x95\x60\x89\xe6[\x94\xb2\x82\xab]");
    case SceneObj_ClipFieldFillDraw:
        return new ClipFieldFillDraw("\x83\x4e\x83\x8a\x83\x62\x83\x76\x83\x47\x83\x8a\x83\x41\x95\x60\x89\xe6[\x93\x68\x82\xe8\x82\xc2\x82\xd4\x82\xb5]");
    case SceneObj_ScreenAlphaCapture:
        return new ScreenAlphaCapture("\x83\x41\x83\x8b\x83\x74\x83\x40\x83\x65\x83\x4e\x83\x58\x83\x60\x83\x83\x8e\xe6\x82\xe8\x8d\x9e\x82\xdd");
    case SceneObj_MapPartsRailGuideHolder:
        return new MapPartsRailGuideHolder();
    case SceneObj_GCapture:
        return new GCapture("G\x83\x4c\x83\x83\x83\x76\x83\x60\x83\x83\x81\x5b");
    case SceneObj_NameObjExecuteHolder:
        return new NameObjExecuteHolder(4096);
    case SceneObj_ElectricRailHolder:
        return new ElectricRailHolder("\x93\x64\x8c\x82\x83\x8c\x81\x5b\x83\x8b\x95\xdb\x8e\x9d");
    case SceneObj_SpiderThread:
        return new SpiderThread("\x83\x4e\x83\x82\x82\xcc\x91\x83");
    case SceneObj_QuakeEffectGenerator:
        return new QuakeEffectGenerator();
    case SceneObj_HeatHazeDirector:
        return new HeatHazeDirector("\x97\x7a\x89\x8a\x90\xa7\x8c\xe4");
    case SceneObj_BlueChipHolder:
        return new ChipHolder("\x83\x75\x83\x8b\x81\x5b\x83\x60\x83\x62\x83\x76\x83\x7a\x83\x8b\x83\x5f\x81\x5b", 0);
    case SceneObj_YellowChipHolder:
        return new ChipHolder("\x83\x43\x83\x47\x83\x8d\x81\x5b\x81\x5b\x83\x60\x83\x62\x83\x76\x83\x7a\x83\x8b\x83\x5f\x81\x5b", 1);
    case SceneObj_BigBubbleHolder:
        return new BigBubbleHolder("\x83\x49\x83\x49\x83\x41\x83\x8f\x83\x7a\x83\x8b\x83\x5f\x81\x5b");
    case SceneObj_EarthenPipeMediator:
        return new EarthenPipeMediator();
    case SceneObj_WaterAreaHolder:
        return new WaterAreaHolder();
    case SceneObj_WaterPlantDrawInit:
        return new WaterPlantDrawInit();
    case SceneObj_OceanHomeMapCtrl:
        return new OceanHomeMapCtrl();
    case SceneObj_RaceManager:
        return new RaceManager();
    case SceneObj_GroupCheckManager:
        return new GroupCheckManager("\x91\xae\x90\xab\x83\x4f\x83\x8b\x81\x5b\x83\x76\x83\x7d\x83\x6c\x81\x5b\x83\x57\x83\x83\x81\x5b");
    case SceneObj_SkeletalFishBabyRailHolder:
        return new SkeletalFishBabyRailHolder("\x83\x58\x83\x4a\x83\x8b\x83\x56\x83\x83\x81\x5b\x83\x4e\x83\x78\x83\x72\x81\x5b\x83\x8c\x81\x5b\x83\x8b\x8a\xc7\x97\x9d");
    case SceneObj_SkeletalFishBossRailHolder:
        return new SkeletalFishBossRailHolder("\x83\x58\x83\x4a\x83\x8b\x83\x56\x83\x83\x81\x5b\x83\x4e\x83\x7b\x83\x58\x83\x8c\x81\x5b\x83\x8b\x8a\xc7\x97\x9d");
    case SceneObj_WaterPressureBulletHolder:
        return new WaterPressureBulletHolder("\x83\x45\x83\x48\x81\x5b\x83\x5e\x81\x5b\x83\x76\x83\x8c\x83\x62\x83\x56\x83\x83\x81\x5b\x8b\xca\x83\x7a\x83\x8b\x83\x5f\x81\x7c");
    case SceneObj_FirePressureBulletHolder:
        return new FirePressureBulletHolder("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x76\x83\x8c\x83\x62\x83\x56\x83\x83\x81\x5b\x8b\xca\x83\x7a\x83\x8b\x83\x5f\x81\x7c");
    case SceneObj_SunshadeMapHolder:
        return new SunshadeMapHolder();
    case SceneObj_MiiFacePartsHolder:
        return new MiiFacePartsHolder(128);
    case SceneObj_MiiFaceIconHolder:
        return new MiiFaceIconHolder(16, "Mii\x83\x41\x83\x43\x83\x52\x83\x93\x95\xdb\x8e\x9d\x8a\xc7\x97\x9d");
    case SceneObj_FluffWindHolder:
        return new FluffWindHolder();
    case SceneObj_SphereSelector:
        return new SphereSelector();
    case SceneObj_GalaxyNamePlateDrawer:
        return new GalaxyNamePlateDrawer();
    case SceneObj_CinemaFrame:
        return new CinemaFrame(true);
    case SceneObj_BossAccessor:
        return new BossAccessor();
    case SceneObj_MiniatureGalaxyHolder:
        return new MiniatureGalaxyHolder();
    case SceneObj_PlanetMapCreator:
        return new PlanetMapCreator("\x98\x66\x90\xaf\x83\x4e\x83\x8a\x83\x47\x83\x43\x83\x5e");
    case SceneObj_WarpPodMgr:
        return new WarpPodMgr("\x83\x8f\x81\x5b\x83\x76\x83\x7c\x83\x62\x83\x68\x8a\xc7\x97\x9d\x8b\xc7");
    case SceneObj_PriorDrawAirHolder:
        return new PriorDrawAirHolder();
    case SceneObj_GalaxyMapController:
        return new GalaxyMapController();
    case SceneObj_MoviePlayingSequenceHolder:
        return new MoviePlayingSequenceHolder("\x83\x80\x81\x5b\x83\x72\x81\x5b\x8a\xc7\x97\x9d\x95\xdb\x8e\x9d");
    case SceneObj_PrologueHolder:
        return new PrologueHolder("\x83\x76\x83\x8d\x83\x8d\x81\x5b\x83\x4f\x95\xdb\x8e\x9d");
    case SceneObj_StaffRoll:
        return new StaffRoll("\x83\x58\x83\x5e\x83\x62\x83\x74\x83\x8d\x81\x5b\x83\x8b");
    default:
        return nullptr;
    }
}

namespace MR {
    NameObj* createSceneObj(int id) {
        return getSceneObjHolder()->create(id);
    }

    SceneObjHolder* getSceneObjHolder() {
        return SingletonHolder< GameSystem >::get()->mSceneController->getSceneObjHolder();
    }

    bool isExistSceneObj(int id) {
        GameSystemSceneController* pSceneController = SingletonHolder< GameSystem >::get()->mSceneController;

        if (pSceneController == nullptr) {
            return false;
        }

        if (!pSceneController->isExistSceneObjHolder()) {
            return false;
        }

        return MR::getSceneObjHolder()->isExist(id);
    }
};  // namespace MR
