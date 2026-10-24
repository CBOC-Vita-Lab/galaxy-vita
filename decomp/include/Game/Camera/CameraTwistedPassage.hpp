#pragma once

#include "Game/Camera/Camera.hpp"
#include "Game/LiveActor/RailRider.hpp"

class CameraTwistedPassage : public Camera {
public:
    CameraTwistedPassage(const char* pName = "\x82\xcb\x82\xb6\x82\xea\x89\xf1\x98\x4c\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(s32, s32, f32, f32);
    void initStartPos();
    f32 calcNearestCoord(const TVec3f&) const;
    f32 calcNearestCoordBetweenTwoRailCoord(f32, f32, const TVec3f&) const;

    /* 0x4C */ RailRider* mRailRider;
    /* 0x50 */ f32 mDistMin;
    /* 0x54 */ f32 mDistMax;
    /* 0x58 */ bool mIsReversed;
};
