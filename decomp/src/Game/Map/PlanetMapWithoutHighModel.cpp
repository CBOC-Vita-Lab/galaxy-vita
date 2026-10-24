#include "Game/Map/PlanetMapWithoutHighModel.hpp"
#include "Game/MapObj/MapObjActorInitInfo.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <cstdio>

PlanetMapWithoutHighModel::PlanetMapWithoutHighModel(const char* pName) : MapObjActor(pName) {
}

void PlanetMapWithoutHighModel::connectToScene(const MapObjActorInitInfo&) {
    MR::connectToScenePlanet(this);
}

PlanetMapWithoutHighModel::~PlanetMapWithoutHighModel() {
}

void PlanetMapWithoutHighModel::init(const JMapInfoIter& rIter) {
    MapObjActor::init(rIter);
    MapObjActorInitInfo info;
    info.setupHioNode("\x81\x9f\x81\x9f\x83\x6e\x83\x43\x83\x82\x83\x66\x83\x8b\x96\xb3\x82\xb5\x82\xcc\x98\x66\x90\xaf\x81\x9f\x81\x9f");
    info.setupDefaultPos();
    info.setupConnectToScene();
    info.setupEffect(nullptr);
    info.setupFarClipping(-1.0f);
    char modelName[256];
    snprintf(modelName, sizeof(modelName), "%sLow", mObjectName);
    info.setupModelName(modelName);
    initialize(rIter, info);
}
