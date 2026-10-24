#include "Game/Map/OceanHomeMapCtrl.hpp"
#include "Game/LiveActor/LodCtrl.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/Map/PlanetMap.hpp"
#include "Game/Map/WaterAreaHolder.hpp"
#include "Game/Map/WaterInfo.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

// Not entirely sure if the createSceneObj would've been here or not. There's no evidence for or against it being here.
namespace {
    OceanHomeMapCtrl* getOceanHomeMapCtrl() {
        MR::createSceneObj(SceneObj_OceanHomeMapCtrl);

        return MR::getSceneObj< OceanHomeMapCtrl >(SceneObj_OceanHomeMapCtrl);
    }
};  // namespace

OceanHomeMapCtrl::OceanHomeMapCtrl() : NameObj("\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x7a\x81\x5b\x83\x80\x92\x6e\x8c\x60\x90\xa7\x8c\xe4") {
    mOceanHomePlanet = nullptr;
    mOceanRingPlanet = nullptr;
    _14 = 0;
    mOceanRingPlanetLowInWater = nullptr;
    _1C = 0;
    _20 = 0;
}

void OceanHomeMapCtrl::entryMapRing(PlanetMap* pPlanet) {
    mOceanRingPlanet = pPlanet;
    mOceanRingPlanetLowInWater = MR::createModelObjPlanetLow("\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x8a\x83\x93\x83\x4f\x81\x69\x90\x85\x92\x86\x97\x70Low\x81\x6a", "OceanRingPlanetLowInWater", nullptr);

    MR::copyTransRotateScale(mOceanRingPlanet, mOceanRingPlanetLowInWater);
    MR::invalidateClipping(mOceanRingPlanetLowInWater);
    MR::tryStartAllAnim(mOceanRingPlanetLowInWater, "OceanRingPlanetLowInWater");
    mOceanRingPlanetLowInWater->kill();
}

void OceanHomeMapFunction::tryEntryOceanHomeMap(PlanetMap* pPlanet) {
    if (strcmp(pPlanet->mName, "\x8a\x43\x97\x6d\x83\x7a\x81\x5b\x83\x80\x98\x66\x90\xaf") == 0) {
        ::getOceanHomeMapCtrl()->mOceanHomePlanet = pPlanet;
    } else if (strcmp(pPlanet->mName, "\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x8a\x83\x93\x83\x4f\x98\x66\x90\xaf") == 0) {
        ::getOceanHomeMapCtrl()->entryMapRing(pPlanet);
    }
}

void OceanHomeMapCtrl::init(const JMapInfoIter&) {
    MR::connectToSceneMapObjMovement(this);
}

void OceanHomeMapCtrl::movement() {
    if (MR::isClipped(mOceanRingPlanet)) {
        return;
    }

    if (WaterAreaFunction::getCameraWaterInfo()->mOceanBowl) {
        if (MR::isDead(mOceanRingPlanetLowInWater)) {
            mOceanRingPlanetLowInWater->makeActorAppeared();
        } else if (!MR::isHiddenModel(mOceanRingPlanet)) {
            MR::hideModel(mOceanRingPlanet);
            mOceanRingPlanet->mLODCtrl->invalidate();
        }
    } else if (!mOceanRingPlanet->mLODCtrl->isShowLowModel() && MR::isHiddenModel(mOceanRingPlanet)) {
        MR::showModel(mOceanRingPlanet);
        mOceanRingPlanet->mLODCtrl->validate();
    } else if (!MR::isDead(mOceanRingPlanetLowInWater)) {
        mOceanRingPlanetLowInWater->makeActorDead();
    }
}

OceanHomeMapCtrl::~OceanHomeMapCtrl() {
}
