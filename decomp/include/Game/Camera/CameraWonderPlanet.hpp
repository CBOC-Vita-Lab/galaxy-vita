#pragma once

#include "Game/Camera/Camera.hpp"

class CameraWonderPlanet : public Camera {
public:
    CameraWonderPlanet(const char* pName = "\x83\x8f\x83\x93\x83\x5f\x81\x5b\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67\x83\x4a\x83\x81\x83\x89");
    virtual ~CameraWonderPlanet();

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isEnableToReset() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    void updateCameraReset();
    void rotateCamera();
    void checkReset();

    void setParam(f32 distMin, f32 distMax, f32 angleX) {
        mDistMin = distMin;
        mDistMax = distMax;
        mAngleX = angleX;
    }

    /* 0x4C */ f32 mDistMin;
    /* 0x50 */ f32 mDistMax;
    /* 0x54 */ f32 mAngleX;
    /* 0x58 */ bool mIsRotating;
    /* 0x5C */ s32 mRotateFrame;
};
