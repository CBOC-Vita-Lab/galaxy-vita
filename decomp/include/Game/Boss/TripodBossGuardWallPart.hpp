#pragma once

#include "Game/LiveActor/LiveActor.hpp"
#include <JSystem/JGeometry/TMatrix.hpp>

class TripodBossGuardWallPart : public LiveActor {
public:
    /// @brief Creates a new `TripodBossGuardWallPart`.
    /// @param pName A pointer to the null-terminated name of the object.
    TripodBossGuardWallPart(const char* pName = "\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x52\x83\x41\x96\x68\x95\xc7\x95\x94\x95\x69");

    virtual void init(const JMapInfoIter&);
    virtual void makeActorAppeared();
    virtual void kill();
    virtual void control();
    virtual void calcAndSetBaseMtx();
    virtual bool receiveMsgEnemyAttack(u32, HitSensor*, HitSensor*);

    void requestStartDemo();
    bool requestBreak();
    bool isEndDemo() const;
    void setHostMatrix(const TPos3f*);
    void setPlacementAngle(f32);
    void setStartTiming(s32);

    void exeNonActive();
    void exeDemo();
    void exeActive();
    void exeBreak();
    void exeRepair();

    /* 0x8C */ const TPos3f* mHostMtx;
    /* 0x90 */ f32 mPlacementAngle;
    /* 0x94 */ s32 mStartTiming;
};
