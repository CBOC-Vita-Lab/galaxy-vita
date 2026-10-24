#pragma once

#include "Game/Camera/Camera.hpp"

class CameraMtxRegParallel : public Camera {
public:
    CameraMtxRegParallel(const char* pName = "\x83\x7d\x83\x67\x83\x8a\x83\x4e\x83\x58\x83\x8c\x83\x57\x83\x58\x83\x5e\x95\xc0\x8d\x73\x83\x4a\x83\x81\x83\x89");
    virtual ~CameraMtxRegParallel();

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(const char* str, f32 dist, const TVec2f& angle, const TVec3f& wPoint) {
        mString = str;
        mAngleX = angle.x;
        mAngleY = angle.y;
        mDist = dist;
        mWPoint = wPoint;
    }

    void calcIdealPose();

    /* 0x4C */ const char* mString;
    /* 0x50 */ f32 mAngleX;
    /* 0x54 */ f32 mAngleY;
    /* 0x58 */ f32 mDist;
    /* 0x5C */ TVec3f mWPoint;
};
