#include "Game/Boss/KoopaParts.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaFireShort.hpp"
#include "Game/Boss/KoopaFireStairs.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/Boss/KoopaPlanetShadow.hpp"
#include "Game/Boss/KoopaRockBreak.hpp"
#include "Game/Boss/KoopaShockWave.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/LiveActor/ActorJointCtrl.hpp"
#include "Game/LiveActor/LiveActorGroup.hpp"
#include "Game/LiveActor/LodCtrl.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/Map/KoopaBattleMapPlanet.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"

namespace {
    static const s32 sFireStairsNum = 16;
    static const s32 sFireShortNum = 32;
    static const s32 sShockWaveNum = 8;
};  // namespace

KoopaParts::KoopaParts(Koopa* pKoopa, const JMapInfoIter& rIter)
    : mKoopa(pKoopa), mPlanetRadius(1300.0f), mThornBig(), mThornSmall(), mArmorBreak(), mThornBreak(), mPlanetLv1(), mPlanetShadow(), mShockWave(),
      mFireShort(), mFireStairs(), mKoopaSwitchKeeper(), mKoopaViewSwitchKeeper(), mKoopaPowerUpSwitch(), mRock(), mRockBreak(), mRollBall(),
      mPlanetLv2(), mPlanetLv3(), mHoleSunPlanetOutside(), mHoleSunPlanetOutsideBloom(), mHoleSunPlanetInside(), mHoleSunPlanetInsideBloom(),
      mPeach(), mKoopaJr(), mKoopaJrShip(), mMeteor1(), mMeteor2(), mMeteor3(), mActorCameraInfo() {
    mActorCameraInfo = new ActorCameraInfo(rIter);
    MR::declareCameraRegisterVec(mKoopa, 0, &mKoopa->mPosition);
}

namespace {
    PartsModel* createKoopaBodyParts(LiveActor* pActor, const char* pName, const char* pModelName, const char* pJointName) {
        PartsModel* pPartsModel = new PartsModel(pActor, pName, pModelName, nullptr, MR::DrawBufferType_Enemy, false);

        pPartsModel->loadFixedPosition(pJointName);
        pPartsModel->initWithoutIter();

        MR::initLightCtrl(pPartsModel);
        MR::registerDemoSimpleCastAll(pPartsModel);

        return pPartsModel;
    };

    KoopaBattleMapPlanet* createKoopaBattleMapPlanet(const char* pName1, const char* pName2, const char* pResetPositionName, bool a1, bool a2,
                                                     bool a3) {
        KoopaBattleMapPlanet* pBattleMapPlanet = new KoopaBattleMapPlanet(pName1, pName2, a1, a2, a3);
        MR::resetPosition(pBattleMapPlanet, pResetPositionName);
        pBattleMapPlanet->initWithoutIter();
        return pBattleMapPlanet;
    };
};  // namespace

KoopaFireStairs* KoopaParts::emitFireStairsToPos(const KoopaBattleMapStair* pBattleMapStair, const TVec3f& rPosition, bool useFront) {
    LiveActor* actor = mFireStairs->getDeadActor();

    if (actor == nullptr) {
        return nullptr;
    }

    KoopaFireStairs* pFireStairs = static_cast< KoopaFireStairs* >(actor);
    pFireStairs->mPosition.set(rPosition);

    if (useFront) {
        pFireStairs->setInfo(pBattleMapStair, KoopaFunction::getKoopaFrontPtr(mKoopa));
    } else {
        pFireStairs->setInfo(pBattleMapStair, nullptr);
    }

    pFireStairs->appear();

    return pFireStairs;
}

void KoopaParts::killFireStairsAll() {
    for (int idx = 0; idx < ::sFireStairsNum; idx++) {
        LiveActor* pActor = mFireStairs->getActor(idx);

        if (!MR::isDead(pActor)) {
            pActor->makeActorDead();
        }
    }
}

void KoopaParts::emitFireShort(bool isFast, bool isCurve) {
    KoopaFireShort* pFireShort = static_cast< KoopaFireShort* >(mFireShort->getDeadActor());

    if (pFireShort == nullptr) {
        return;
    }

    if (isCurve) {
        pFireShort->emitCurve();
    } else if (isFast) {
        pFireShort->emitFast();
    } else {
        pFireShort->emitNormal();
    }
}

void KoopaParts::emitFireLongTime() {
    KoopaFireShort* pFireShort = static_cast< KoopaFireShort* >(mFireShort->getDeadActor());

    if (pFireShort == nullptr) {
        return;
    }

    pFireShort->emitLongTime();
}

void KoopaParts::emitShockWave() {
    LiveActor* pActor = mShockWave->getDeadActor();

    if (pActor == nullptr) {
        return;
    }

    pActor->appear();
}

TVec3f& KoopaParts::getPlanetPos() const {
    if (KoopaFunction::isKoopaVs1(mKoopa) || KoopaFunction::isKoopaVs2(mKoopa)) {
        return mPlanetLv1->mPosition;
    }

    if (KoopaFunction::isKoopaLv1(mKoopa)) {
        return mPlanetLv1->mPosition;
    }

    if (KoopaFunction::isKoopaLv2(mKoopa)) {
        return mPlanetLv2->mPosition;
    }

    return mPlanetLv3->mPosition;
}

f32 KoopaParts::getPlanetRadius() const {
    return mPlanetRadius;
}

void KoopaParts::appearHoleSunPlanetInside() {
    mHoleSunPlanetInside->appear();
}

void KoopaParts::appearHoleSunPlanetOutside() {
    mHoleSunPlanetOutside->appear();
}

void KoopaParts::killHoleSunPlanetOutside() {
    mHoleSunPlanetOutside->kill();
}

void KoopaParts::createPlanetShadow() {
    mPlanetShadow = new KoopaPlanetShadow(mKoopa);
    mPlanetShadow->initWithoutIter();
    mPlanetShadow->kill();
}

void KoopaParts::initVs1() {
    createFireStairs(false);

    mPlanetLv1 = ::createKoopaBattleMapPlanet("\x83\x4e\x83\x62\x83\x70\x98\x66\x90\xaf", "KoopaBattleMapPlanet", "\x98\x66\x90\xaf\x92\x86\x90\x53", false, false, false);

    createPlanetShadow();
    createCommonParts();
}

void KoopaParts::initVs2() {
    mPlanetLv1 = ::createKoopaBattleMapPlanet("\x83\x4e\x83\x62\x83\x70\x98\x66\x90\xaf", "KoopaBattleMapPlanetVs2", "\x98\x66\x90\xaf\x92\x86\x90\x53", true, false, false);

    createCommonParts();
}

namespace {
    ModelObjNpc* createDemoNpc(const char* pName, const char* pModelArcName) {
        ModelObjNpc* pModelObjNpc = new ModelObjNpc(pName, pModelArcName, nullptr);
        pModelObjNpc->initWithoutIter();
        MR::resetPosition(pModelObjNpc, "\x83\x66\x83\x82\x92\x86\x90\x53");

        pModelObjNpc->mLodCtrl->invalidateClipping();
        pModelObjNpc->mLodCtrl->invalidate();

        pModelObjNpc->mJointCtrl->endFaceCtrl(-1);

        MR::initLightCtrl(pModelObjNpc);

        pModelObjNpc->kill();

        return pModelObjNpc;
    };

    ModelObj* createDemoEnemy(const char* pName1, const char* pName2) {
        ModelObj* pModelObj = MR::createModelObjEnemy(pName1, pName2, nullptr);
        pModelObj->initWithoutIter();
        MR::resetPosition(pModelObj, "\x83\x66\x83\x82\x92\x86\x90\x53");
        pModelObj->kill();
        return pModelObj;
    };
};  // namespace

void KoopaParts::initVs3() {
    createFireStairs(true);

    mPlanetLv1 = ::createKoopaBattleMapPlanet("\x83\x4e\x83\x62\x83\x70\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x50", "KoopaBattleMapPlanetVs3Lv1", "\x98\x66\x90\xaf\x92\x86\x90\x53", false, false, true);
    mPlanetLv2 = ::createKoopaBattleMapPlanet("\x83\x4e\x83\x62\x83\x70\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x51", "KoopaBattleMapPlanetVs3Lv2", "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x51", false, false, true);
    mPlanetLv3 = ::createKoopaBattleMapPlanet("\x83\x4e\x83\x62\x83\x70\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52", "KoopaBattleMapPlanetVs3Lv3", "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52", false, true, false);

    MR::startBrk(mPlanetLv1, "Wait");
    MR::startBrk(mPlanetLv2, "Wait");

    createPlanetShadow();

    mHoleSunPlanetOutside = MR::createModelObjMapObj("\x8c\x8a\x82\xa0\x82\xab\x91\xbe\x97\x7a\x81\x69\x8a\x4f\x91\xa4\x81\x6a", "KoopaVS3HoleSunPlanet", nullptr);
    MR::setClippingTypeSphere(mHoleSunPlanetOutside, 40000.0f);
    MR::setClippingFarMax(mHoleSunPlanetOutside);
    MR::resetPosition(mHoleSunPlanetOutside, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52");
    MR::tryStartAllAnim(mHoleSunPlanetOutside, "KoopaVS3HoleSunPlanet");

    mHoleSunPlanetOutsideBloom = MR::createBloomModel(mHoleSunPlanetOutside, nullptr);

    mHoleSunPlanetInside = MR::createModelObjMapObj("\x8c\x8a\x82\xa0\x82\xab\x91\xbe\x97\x7a\x81\x69\x93\xe0\x91\xa4\x81\x6a", "KoopaVS3HoleSunInsidePlanet", nullptr);
    MR::invalidateClipping(mHoleSunPlanetInside);
    MR::resetPosition(mHoleSunPlanetInside, "\x98\x66\x90\xaf\x82\x6b\x82\x96\x82\x52");
    MR::tryStartAllAnim(mHoleSunPlanetInside, "KoopaVS3HoleSunInsidePlanet");

    mHoleSunPlanetInsideBloom = MR::createBloomModel(mHoleSunPlanetInside, nullptr);
    mHoleSunPlanetInside->kill();

    MR::registerDemoSimpleCastAll(mHoleSunPlanetOutside);
    MR::registerDemoSimpleCastAll(mHoleSunPlanetOutsideBloom);
    MR::registerDemoSimpleCastAll(mHoleSunPlanetInside);
    MR::registerDemoSimpleCastAll(mHoleSunPlanetInsideBloom);

    mPeach = ::createDemoNpc("\x83\x73\x81\x5b\x83\x60", "Peach");
    mKoopaJr = ::createDemoNpc("\x83\x4e\x83\x62\x83\x70\x82\x69\x82\x92", "KoopaJr");
    mKoopaJrShip = ::createDemoNpc("\x83\x4e\x83\x62\x83\x70\x82\x69\x82\x92\x90\xed\x8a\xcd", "KoopaJrShip");

    mMeteor1 = ::createDemoEnemy("\x83\x66\x83\x82\x96\x43\x92\x65\x82\x50", "MeteorStrike");
    mMeteor2 = ::createDemoEnemy("\x83\x66\x83\x82\x96\x43\x92\x65\x82\x51", "MeteorStrike");
    mMeteor3 = ::createDemoEnemy("\x83\x66\x83\x82\x96\x43\x92\x65\x82\x52", "MeteorStrike");

    createCommonParts();
}

void KoopaParts::createRock() {
    if (mRock != nullptr) {
        return;
    }

    mRock = ::createKoopaBodyParts(mKoopa, "\x83\x4e\x83\x62\x83\x70\x8a\xe2", "KoopaRock", "RockFixPos");
    mRock->kill();

    mRockBreak = new KoopaRockBreak(mKoopa);
    mRockBreak->initWithoutIter();
}

void KoopaParts::createRollBall() {
    if (mRollBall != nullptr) {
        return;
    }

    mRollBall = ::createKoopaBodyParts(mKoopa, "\x89\xf1\x93\x5d\x8d\x55\x8c\x82\x83\x7b\x81\x5b\x83\x8b", "KoopaRollBall", "RollBallFixPos");
    mRollBall->kill();
}

void KoopaParts::createCommonParts() {
    mThornBig = ::createKoopaBodyParts(mKoopa, "\x90\x4b\x94\xf6\x82\xcc\x83\x67\x83\x51\x81\x69\x91\xe5\x81\x6a", "KoopaThorn", "TailThornBigFixPos");
    mThornSmall = ::createKoopaBodyParts(mKoopa, "\x90\x4b\x94\xf6\x82\xcc\x83\x67\x83\x51\x81\x69\x8f\xac\x81\x6a", "KoopaThorn", "TailThornSmallFixPos");
    mArmorBreak = ::createKoopaBodyParts(mKoopa, "\x89\xf3\x82\xea\x8d\x62\x97\x85", "KoopaArmorBreak", "ArmorBreakFixPos");
    mArmorBreak->kill();

    mThornBreak = ::createKoopaBodyParts(mKoopa, "\x83\x67\x83\x51\x94\x6a\x95\xd0", "KoopaThornBreak", "ThornBreakFixPos");
    mThornBreak->kill();

    mFireShort = new LiveActorGroup("\x83\x56\x83\x87\x81\x5b\x83\x67\x89\x8a", ::sFireShortNum);
    mFireShort->initWithoutIter();

    for (int idx = 0; idx < ::sFireShortNum; idx++) {
        KoopaFireShort* pFireShort = new KoopaFireShort(mKoopa);
        pFireShort->initWithoutIter();
        mFireShort->registerActor(pFireShort);
    }

    mShockWave = new LiveActorGroup("\x8f\xd5\x8c\x82\x94\x67\x81\x69\x8b\x85\x8f\xf3\x81\x6a", ::sShockWaveNum);
    mShockWave->initWithoutIter();

    for (int idx = 0; idx < ::sShockWaveNum; idx++) {
        KoopaShockWave* pShockWave = new KoopaShockWave(mKoopa);
        pShockWave->initWithoutIter();
        mShockWave->registerActor(pShockWave);
    }
}

void KoopaParts::createFireStairs(bool a1) {
    mFireStairs = new LiveActorGroup("\x89\x8a\x81\x69\x8a\x4b\x92\x69\x97\x70\x81\x6a\x95\xdb\x8e\x9d", ::sFireStairsNum);
    mFireStairs->initWithoutIter();

    for (int idx = 0; idx < ::sFireStairsNum; idx++) {
        KoopaFireStairs* pFireStairs = new KoopaFireStairs("\x89\x8a\x81\x69\x8a\x4b\x92\x69\x97\x70\x81\x6a", a1);
        pFireStairs->initWithoutIter();
        mFireStairs->registerActor(pFireStairs);
    }
}
