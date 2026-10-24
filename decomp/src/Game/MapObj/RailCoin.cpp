#include "Game/MapObj/RailCoin.hpp"
#include "Game/AreaObj/MercatorTransformCube.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"

namespace {
    class RailCoinMercatorDivInfo : public DivideMercatorRailPosInfo {
    public:
        inline RailCoinMercatorDivInfo(CoinGroup* pGroup) {
            mGroup = pGroup;
        }

        virtual void setPosition(s32, const TVec3f&);

        /* 0x04 */ CoinGroup* mGroup;
    };
};  // namespace

RailCoin::RailCoin(const char* pName) : CoinGroup(pName) {
}

void RailCoin::initCoinArray(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    initRailRider(rIter);
}

void RailCoin::placementNormalRail() {
    f32 speed;
    f32 length = MR::getRailTotalLength(this);
    u32 coinCount = mCoinCount;

    if (mCoinCount <= 1) {
        speed = 0.0f;
    } else {
        if (MR::isLoopRail(this)) {
            speed = length / coinCount;
        } else {
            speed = length / (coinCount - 1);
        }
    }

    MR::moveCoordToStartPos(this);
    MR::setRailCoordSpeed(this, speed);

    for (s32 i = 0; i < coinCount; i++) {
        setCoinTrans(i, MR::getRailPos(this));
        MR::moveRailRider(this);
    }
}

void RailCoin::placementMercatorRail() {
    ::RailCoinMercatorDivInfo info(this);
    MR::getDivideMercatorRailPosition(&info, this, mCoinCount, 10.0f, 10);
}

namespace MR {
    NameObj* createRailCoin(const char* pName) {
        return new RailCoin(pName);
    }

    NameObj* createRailPurpleCoin(const char* pName) {
        RailCoin* coin = new RailCoin(pName);
        coin->mIsPurpleCoinGroup = true;
        return coin;
    }
};  // namespace MR

namespace {
    void RailCoinMercatorDivInfo::setPosition(s32 idx, const TVec3f& rPos) {
        mGroup->setCoinTrans(idx, rPos);
    }
};  // namespace

RailCoin::~RailCoin() {
}

void RailCoin::placementCoin() {
    if (MR::isInAreaObj("MercatorCube", mPosition)) {
        placementMercatorRail();
    } else {
        placementNormalRail();
    }
}

const char* RailCoin::getCoinName() const {
    return mIsPurpleCoinGroup ? "\x83\x70\x81\x5b\x83\x76\x83\x8b\x83\x52\x83\x43\x83\x93(\x83\x8c\x81\x5b\x83\x8b\x94\x7a\x92\x75)" : "\x83\x52\x83\x43\x83\x93(\x83\x8c\x81\x5b\x83\x8b\x94\x7a\x92\x75)";
}
