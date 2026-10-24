#pragma once

#include "Game/Camera/Camera.hpp"

class CameraWaterFollow : public Camera {
public:
    CameraWaterFollow(const char* pName = "\x90\x85\x92\x86\x83\x74\x83\x48\x83\x8d\x81\x5b");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isEnableToReset() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    void setParam(f32 distMin, f32 distMax, f32 blendRateMin) {
        mDistMin = distMin;
        mDistMax = distMax;
        mSideBlendRateMin = blendRateMin;
    }

    /* 0x4C */ f32 mDistMin;
    /* 0x50 */ f32 mDistMax;
    /* 0x54 */ f32 mSideBlendRateMin;
    /* 0x58 */ f32 mSideBlendRate;
    /* 0x5C */ s32 mCollideCount;
    /* 0x60 */ TVec3f mLastMoveDir;
    /* 0x6C */ bool mIsRounding;
    /* 0x70 */ s32 mRoundingFrame;
    /* 0x74 */ TVec3f mSide;
};
