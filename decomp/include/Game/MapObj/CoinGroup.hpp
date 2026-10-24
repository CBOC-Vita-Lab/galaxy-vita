#pragma once

#include "Game/LiveActor/LiveActor.hpp"

class ActorCameraInfo;
class Coin;

class CoinGroup : public LiveActor {
public:
    CoinGroup(const char*);

    virtual void init(const JMapInfoIter&);
    virtual void appear();

    virtual void initCoinArray(const JMapInfoIter&) = 0;
    virtual void placementCoin() {
    }
    virtual const char* getCoinName() const {
        return "\x83\x52\x83\x43\x83\x93(\x83\x4f\x83\x8b\x81\x5b\x83\x76\x94\x7a\x92\x75)";
    }

    void killCoinAll();
    void appearCoinAll();
    void appearCoinFix();
    void appearCoinAllTimer();
    void setCoinTrans(s32, const TVec3f&);
    void exeAppear();
    void exeTryStartDemo();
    void exeDemoAppear();
    void exeKill();

    /* 0x8C */ Coin** mCoinArray;
    /* 0x90 */ ActorCameraInfo* mCameraInfo;
    /* 0x94 */ u32 mCoinCount;
    /* 0x98 */ s32 mTimeLimit;
    /* 0x9C */ bool mIsPurpleCoinGroup;
};
