#pragma once

#include "Game/Camera/Camera.hpp"

class CameraCubePlanet : public Camera {
public:
    CameraCubePlanet(const char* pName = "\x83\x4c\x83\x85\x81\x5b\x83\x75\x98\x66\x90\xaf\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(f32 dist, f32 angleX, f32 angleY) {
        mDist = dist;
        mAngleX = angleX;
        mAngleY = angleY;
    }

    /* 0x4C */ f32 mDist;
    /* 0x50 */ f32 mAngleX;
    /* 0x54 */ f32 mAngleY;
    /* 0x58 */ TVec3f mUp;
};
