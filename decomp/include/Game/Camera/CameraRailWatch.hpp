#pragma once

#include "Game/Camera/Camera.hpp"

class RailRider;

class CameraRailWatch : public Camera {
public:
    CameraRailWatch(const char* pName = "\x83\x8c\x81\x5b\x83\x8b\x92\x8d\x96\xda\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(s32, s32, s32, f32, f32, f32);

    /* 0x4C */ RailRider* mRailRider;
    /* 0x50 */ s32 mDirection;
    /* 0x54 */ s32 mSetDirection;
    /* 0x58 */ f32 mRailCoordOffset;
    /* 0x5C */ f32 mDist;
    /* 0x60 */ f32 mAngleX;
};
