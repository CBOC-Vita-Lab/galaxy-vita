#pragma once

#include "Game/Camera/Camera.hpp"

class CameraWaterPlanet : public Camera {
public:
    CameraWaterPlanet(const char* pName = "\x90\x85\x92\x86\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(f32 distMin, f32 distMax, f32 angleX) {
        mDistMin = distMin;
        mDistMax = distMax;
        mAngleX = angleX;
    }

    /* 0x4C */ f32 mDistMin;
    /* 0x50 */ f32 mDistMax;
    /* 0x54 */ f32 mAngleX;
};
