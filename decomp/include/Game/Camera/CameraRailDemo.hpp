#pragma once

#include "Game/Camera/Camera.hpp"

class RailRider;

class CameraRailDemo : public Camera {
public:
    CameraRailDemo(const char* pName = "\x83\x8c\x81\x5b\x83\x8b\x83\x66\x83\x82\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(s32, s32, s32, s32, f32);
    void calcLinear();
    void calcEaseInOut();
    void calcDamp();

    /* 0x4C */ RailRider* mRailRider;
    /* 0x50 */ s32 mCalcType;
    /* 0x54 */ s32 mDemoTime;
    /* 0x58 */ s32 mDemoTimer;
    /* 0x5C */ f32 mDampRatio;
    /* 0x60 */ f32 mCoord;
};
