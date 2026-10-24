#pragma once

#include "Game/Camera/Camera.hpp"

class CameraObjParallel : public Camera {
public:
    CameraObjParallel(const char* pName = "\x83\x49\x83\x75\x83\x57\x83\x46\x95\xc0\x8d\x73\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(f32 dist, f32 angleX, f32 angleY) {
        mDist = dist;
        mAngleX = angleX;
        mAngleY = angleY;
    }

    /* 0x4C */ f32 mAngleX;
    /* 0x50 */ f32 mAngleY;
    /* 0x54 */ f32 mDist;
};
