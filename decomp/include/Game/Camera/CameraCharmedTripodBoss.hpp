#pragma once

#include "Game/Camera/Camera.hpp"

class CameraCharmedTripodBoss : public Camera {
public:
    CameraCharmedTripodBoss(const char* pName = "\x8e\x4f\x8b\x72\x83\x7b\x83\x58\x83\x57\x83\x87\x83\x43\x83\x93\x83\x67\x92\x8d\x8e\x8b\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void setParam(s32, TVec3f, const TVec3f&, const TVec2f&);

    /* 0x4C */ s32 mJointId;
    /* 0x50 */ TVec3f mUp;
    /* 0x5C */ TVec3f mWPoint;
    /* 0x68 */ f32 mAngleX;
    /* 0x6C */ f32 mAngleY;
};
