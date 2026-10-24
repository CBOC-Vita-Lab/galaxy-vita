#include "Game/Boss/TombSpiderEnvironment.hpp"
#include "Game/LiveActor/LodCtrl.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/Map/PlanetMap.hpp"
#include "Game/MapObj/SpiderThread.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EffectUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"

TombSpiderEnvironment::TombSpiderEnvironment(LiveActor* pActor) : mActor(pActor), mPlanet(nullptr), mCocoon(nullptr) {
    MR::setEffectHostSRT(mActor, "Noctiluca", MR::getPlayerPos(), nullptr, nullptr);
    MR::createSceneObj(SceneObj_SpiderThread);
    MR::initSpiderThread(mActor->mPosition);

    mPlanet = new PlanetMap("\x90\xed\x8f\xea[\x83\x67\x83\x44\x81\x5b\x83\x80\x83\x58\x83\x70\x83\x43\x83\x5f\x81\x5b]", "TombSpiderPlanet");
    mPlanet->mPosition.set(mActor->mPosition);
    mPlanet->initWithoutIter();
    mPlanet->mLODCtrl->setDistanceToLow(25000.0f);
    MR::registerDemoSimpleCastAll(mPlanet);
    mPlanet->appear();

    mCocoon = new ModelObj("\x82\xdc\x82\xe4[\x83\x67\x83\x44\x81\x5b\x83\x80\x83\x58\x83\x70\x83\x43\x83\x5f\x81\x5b]", "TombSpiderCocoon", nullptr, -2, -2, -2, false);
    mCocoon->mPosition.set(mActor->mPosition);
    mCocoon->initWithoutIter();
    MR::invalidateClipping(mCocoon);
    MR::registerDemoSimpleCastAll(mCocoon);
    mCocoon->kill();
}
