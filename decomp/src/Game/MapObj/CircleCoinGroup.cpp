#include "Game/MapObj/CircleCoinGroup.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"
#include <JSystem/JMath/JMATrigonometric.hpp>

void CircleCoinGroup_FORCE_MATCH_SDATA2() {
    (void)0.0f;
}

CircleCoinGroup::CircleCoinGroup(const char* pName) : CoinGroup(pName) {
    mCoinRadius = 200.0f;
}

void CircleCoinGroup::initCoinArray(const JMapInfoIter& rIter) {
    MR::getJMapInfoArg2NoInit(rIter, &mCoinRadius);
    MR::initDefaultPos(this, rIter);
}

#pragma push
#pragma opt_propagation off
void CircleCoinGroup::placementCoin() {
    TPos3f mtx;
    MR::makeMtxTR(mtx, this);

    TVec3f front, side;
    mtx.getXDir(side);
    mtx.getZDir(front);

    const TVec3f center(*getPosition());
    f32 angle = 0.0f;
    f32 interval = (TWO_PI / mCoinCount);

    for (s32 i = 0; i < mCoinCount; i++) {
        f32 c = mCoinRadius * MR::cos(angle);
        f32 s = mCoinRadius * MR::sin(angle);

        setCoinTrans(i, front * c + side * s + center);
        angle += interval;
    }
}
#pragma pop

namespace MR {
    NameObj* createCircleCoinGroup(const char* pName) {
        CircleCoinGroup* group = new CircleCoinGroup(pName);
        return group;
    }

    NameObj* createCirclePurpleCoinGroup(const char* pName) {
        CircleCoinGroup* group = new CircleCoinGroup(pName);
        group->mIsPurpleCoinGroup = true;
        return group;
    }
};  // namespace MR

CircleCoinGroup::~CircleCoinGroup() {
}

const char* CircleCoinGroup::getCoinName() const {
    return mIsPurpleCoinGroup ? "\x83\x70\x81\x5b\x83\x76\x83\x8b\x83\x52\x83\x43\x83\x93(\x89\x7e\x8c\x60\x94\x7a\x92\x75)" : "\x83\x52\x83\x43\x83\x93(\x89\x7e\x8c\x60\x94\x7a\x92\x75)";
}
