#pragma once

#include "Game/Camera/Camera.hpp"

class CameraDead : public Camera {
public:
    enum CameraType {
        CameraType_FixedPos = 0,
        CameraType_Interpolate = 1,
    };

    CameraDead(const char* pName = "\x92\xca\x8f\xed\x8e\x80\x96\x53\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isInterpolationOff() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    void setParam(f32 dist, s32 deadTime, s32 cameraType) {
        mDist = dist;
        mDeadTime = deadTime;
        mCameraType = cameraType;
    }

    /* 0x4C */ f32 mFovy;
    /* 0x50 */ f32 mDist;
    /* 0x54 */ s32 mDeadTime;
    /* 0x58 */ s32 mCameraType;
    /* 0x5C */ s32 mDeadFrame;
};
