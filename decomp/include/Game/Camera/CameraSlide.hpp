#pragma once

#include "Game/Camera/Camera.hpp"

class CameraSlide : public Camera {
public:
    CameraSlide(const char* pName = "\x83\x58\x83\x89\x83\x43\x83\x68\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(const TVec3f& axis, const TVec3f& up, const TVec3f& wPoint, f32 angle, f32 dist) {
        mAxis.set(axis);
        mUp.set(up);
        mWPoint.set(wPoint);
        mAngleA = angle;
        mDist = dist;
    }

    /* 0x4C */ TVec3f mAxis;
    /* 0x58 */ TVec3f mUp;
    /* 0x64 */ TVec3f mWPoint;
    /* 0x70 */ f32 mAngleA;
    /* 0x74 */ f32 mDist;
};
