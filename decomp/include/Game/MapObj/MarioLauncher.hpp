#pragma once

#include "Game/Camera/CameraTargetMtx.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/SimpleLayout.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>

class MarioLauncherShell;
class MarioLauncherLayout;

class MarioLauncher : public LiveActor {
public:
    enum LauncherType {
        LauncherType_Front = 0,
        LauncherType_Up = 1,
    };

    MarioLauncher(const char*);

    virtual void init(const JMapInfoIter& rIter);
    virtual void attackSensor(HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveMsgPush(HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveMsgEnemyAttack(u32 msg, HitSensor* pSender, HitSensor* pReceiver);
    virtual bool receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver);

    void exeWait();
    void exeReady();
    void exePlayerIn();
    void exePrep();
    void exeAim();
    void exeFlight();
    void exeKill();
    void exeLand();
    void exeReject();

    bool doReady(HitSensor*);
    bool doPrep();
    bool doAim();
    bool doReject();
    bool doEject();
    bool doFlight();
    bool doLanding(s32);

    bool testHitCollision() NO_INLINE;
    bool isBreak(const Triangle*);

    void onSwitchA();
    void offSwitchA();

    /* 0x08C */ MarioLauncherLayout* mLayout;
    /* 0x090 */ MarioLauncherShell* mShell;
    /* 0x094 */ LiveActor* mHost;
    /* 0x098 */ bool mIsInAttractor;
    /* 0x09C */ s32 mLauncherType;  // arg0
    /* 0x0A0 */ TPos3f mEffectMtxSmoke;
    /* 0x0D0 */ TPos3f mEffectMtxGravity;
    /* 0x100 */ CameraTargetMtx mCameraTargetMtx;
    /* 0x190 */ TPos3f mPlayerMtx;
    /* 0x1C0 */ TPos3f mPlayerBaseMtx;
    /* 0x1F0 */ TPos3f mLauncherBaseMtx;
};

class MarioLauncherLayout : public LayoutActor {
public:
    MarioLauncherLayout() : LayoutActor("\x88\xda\x93\xae\x97\x70\x96\x43\x91\xe4\x97\x70\x83\x8c\x83\x43\x83\x41\x83\x45\x83\x67", true), mLayout() {
    }

    virtual void init(const JMapInfoIter& rIter);
    virtual void kill();

    void appearAt(const TVec2f& pos) {
        mLayout->appear();
        mLayout->setTrans(pos);
    }

    void initLayout() {
        initLayoutManager("Telescope", 1);
        mLayout = new SimpleLayout("\x88\xda\x93\xae\x97\x70\x96\x43\x91\xe4\x8f\xc6\x8f\x80", "Sight", 1, -1);
        mLayout->kill();
    }

    /* 0x20 */ SimpleLayout* mLayout;
};

class MarioLauncherShell : public LiveActor {
public:
    MarioLauncherShell(const char* pName) : LiveActor(pName) {
        mBaseMtx.identity();
        mReboundDir.zero();
    }

    virtual void init(const JMapInfoIter& rIter);
    virtual MtxPtr getBaseMtx() const;
    virtual void calcAndSetBaseMtx();
    virtual void calcBaseMtx();

    TVec3f& getPosition() {
        return mPosition;
    }

    /* 0x8C */ TPos3f mBaseMtx;
    /* 0xBC */ TQuat4f mRotQuat;
    /* 0xCC */ TVec3f _CC;
    /* 0xD8 */ TVec3f mReboundDir;
};
