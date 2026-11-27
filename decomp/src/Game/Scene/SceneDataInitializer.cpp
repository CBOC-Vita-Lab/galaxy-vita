#include "Game/Scene/SceneDataInitializer.hpp"
#include "Game/MP/MpBubble.hpp"  // MP3: Play Together "Bubbles" falls
#include "Game/MP/MpGame.hpp"
#include "Game/MP/MpDemo.hpp"  // MP3 demo (SMG_MP_DEMO)
#include "Game/MP/MpHead3D.hpp"  // MP3 heads3d: the file select's Cosmic heads
#include "Game/MP/MpUi.hpp"  // integ: the layout archives of a session scene
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Scene/StageDataHolder.hpp"
#include "Game/Scene/StageFileLoader.hpp"
#include "Game/Screen/LuigiLetter.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <JSystem/JKernel/JKRSolidHeap.hpp>  // MP3 memfix

SceneDataInitializer::SceneDataInitializer() : NameObj("SceneDataInitializer") {
    mFileLoader = nullptr;
    mDataHolder = nullptr;
}

void SceneDataInitializer::startStageFileLoad() {
    mFileLoader = new StageFileLoader(MR::getCurrentStageName());
    mFileLoader->startLoadingStageFile();

    if (!MR::isStageDisablePauseMenu()) {
        MR::mountAsyncArchive("/LayoutData/PauseMenu.arc", MR::getAproposHeapForSceneArchive(0.03f));
    }
}

void SceneDataInitializer::startStageFileLoadAfterScenarioSelected() {
    NameObjArchiveListCollector collector;

    LuigiLetter::makeArchiveListForMenu(&collector, JMapInfoIter());

    // MP3: puppets of the other character need his body model (the stationed set holds only the local character's)
    int mpFirst = collector.getArchiveNum();
    if (mp_active()) {
        MpPuppets::addArchives(&collector);
        MpBubble::addArchives(&collector);  // MP3: Play Together "Bubbles": the Bubble Breeze bubble's textures (BigBubble.arc)
    } else if (MpDemo::enabled()) {
        MpDemo::addArchives(&collector);  // MP3 demo (SMG_MP_DEMO=1): the archives of all six characters and the bubble; nothing without the switch
    }
    const int mpEnd = collector.getArchiveNum();   // memfix: [mpFirst, mpEnd) are the puppets' and the bubble's
    // integ (B1): MpUiSystem makes its layouts on the scene init thread: their LayoutData archives must be requested HERE, with the others
    // (nothing can be mounted from that thread later). Same heap rule as the multiplayer archives below. No-op without a session.
    MpUi::addArchives(MR::getSceneHeapGDDR3() != nullptr && MR::getSceneHeapGDDR3()->getFreeSize() > 0x400000 ? MR::getSceneHeapGDDR3() : nullptr);
    MpHead3D::addArchives(&collector);  // MP3 heads3d: the file select only (GhostMario / GhostLuigi for the character select)

    for (int i = 0; i < collector.getArchiveNum(); i++) {
        // MP3 memfix: the file cache's free ratio is read when the mount is requested, long before the stage's own files fill it:
        // 4 players in CosmosGardenGalaxy:1 left 25 KB of 17 MB and the scene load died in a heap panic (desktop run, 11 Nov). The
        // multiplayer archives (0.25-0.9 MB) go to the scene GDDR (13 MB free on the Vita, freed with the scene) when it has 4 MiB
        // to spare; everything else mounts as before.
        JKRHeap* heap = nullptr;
        if (i >= mpFirst && i < mpEnd && MR::getSceneHeapGDDR3() != nullptr && MR::getSceneHeapGDDR3()->getFreeSize() > 0x400000) {
            heap = MR::getSceneHeapGDDR3();
        }
        MR::mountAsyncArchiveByObjectOrLayoutName(collector.getArchive(i), heap);
    }
}

void SceneDataInitializer::waitDoneStageFileLoad() {
    mFileLoader->waitLoadedStageFile();
    mDataHolder = static_cast< StageDataHolder* >(MR::createSceneObj(SceneObj_StageDataHolder));
}

void SceneDataInitializer::startActorFileLoadCommon() {
    mDataHolder->requestFileLoadCommon();
}

void SceneDataInitializer::startActorFileLoadScenario() {
    mDataHolder->requestFileLoadScenario();
}

void SceneDataInitializer::startActorPlacement() {
    mDataHolder->initPlacement();
}

void SceneDataInitializer::initAfterScenarioSelected() {
    mDataHolder->initAfterScenarioSelected();
}

SceneDataInitializer::~SceneDataInitializer() {
}
