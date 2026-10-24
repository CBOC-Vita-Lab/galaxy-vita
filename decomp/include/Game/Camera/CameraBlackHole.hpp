#pragma once

#include "Game/Camera/Camera.hpp"

class CameraBlackHole : public Camera {
public:
    CameraBlackHole(const char* pName = "\x83\x75\x83\x89\x83\x62\x83\x4e\x83\x7a\x81\x5b\x83\x8b\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(const TVec3f rWPoint, const TVec3f rBasePos) {
        mWPoint.set(rWPoint);
        mBasePos.set(rBasePos);
    }

    /* 0x4C */ f32 mFovy;
    /* 0x50 */ f32 mRoll;
    /* 0x54 */ TVec3f mWPoint;
    /* 0x60 */ TVec3f mBasePos;
};
