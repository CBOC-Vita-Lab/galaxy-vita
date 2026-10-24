#include "Game/MapObj/PowerStarAppearPoint.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"
#include "Game/Util/ActorCameraUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"

PowerStarAppearPoint::PowerStarAppearPoint(const char* pName) : LiveActor(pName), mCameraInfo() {
}

PowerStarAppearPoint::~PowerStarAppearPoint() {
}

void PowerStarAppearPoint::init(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    MR::joinToGroupArray(this, rIter, "\x83\x70\x83\x8f\x81\x5b\x83\x58\x83\x5e\x81\x5b\x8f\x6f\x8c\xbb\x83\x7c\x83\x43\x83\x93\x83\x67\x83\x4f\x83\x8b\x81\x5b\x83\x76", 16);
    MR::initActorCamera(this, rIter, &mCameraInfo);
    makeActorAppeared();
}
