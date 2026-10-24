#include "Game/MapObj/RosettaChair.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"

RosettaChair::RosettaChair(const char* pName) : LiveActor(pName), mDefaultPosition(0.0f, 0.0f, 0.0f), mDefaultRotation(0.0f, 0.0f, 0.0f) {
    mScaleMtx.identity();
}

void RosettaChair::init(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    initModelManagerWithAnm("RosettaChair", nullptr, false);
    MR::connectToSceneMapObj(this);
    initHitSensor(1);
    MR::addBodyMessageSensorMapObj(this);

    mScaleMtx.set(getBaseMtx());
    mScaleMtx.scaleXYZ(1.7f);
    MR::initCollisionPartsAutoEqualScale(this, "RosettaChair", getSensor("body"), mScaleMtx);

    MR::setClippingTypeSphere(this, 500.0f);
    MR::tryRegisterDemoCast(this, rIter);
    MR::registerDemoActionFunctor(this, MR::Functor(this, &RosettaChair::startDemo), "\x98\x4e\x93\xc7\x8a\x4a\x8e\x6e");
    MR::registerDemoActionFunctor(this, MR::Functor(this, &RosettaChair::setDefaultPose), "\x83\x4c\x83\x83\x83\x58\x83\x67\x93\xfc\x82\xea\x8a\xb7\x82\xa6");
    mDefaultPosition.set(mPosition);
    mDefaultRotation.set(mRotation);
    MR::startBck(this, "RosettaChair");
    makeActorAppeared();
}

void RosettaChair::setDefaultPose() {
    mPosition.set(mDefaultPosition);
    mRotation.set(mDefaultRotation);
    MR::startBck(this, "RosettaChair");
    MR::validateCollisionParts(this);
}

void RosettaChair::startDemo() {
    MR::startBck(this, "DemoRosettaReading");
    MR::invalidateCollisionParts(this);
}
