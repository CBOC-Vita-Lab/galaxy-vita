#pragma once

#include "Game/Camera/Camera.hpp"

class CameraSubjective : public Camera {
public:
    CameraSubjective(const char* pName = "\x8e\xe5\x8a\xcf\x83\x4a\x83\x81\x83\x89");

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isCollisionOff() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    /* 0x4C */ TVec2f mAngleVel;
    /* 0x54 */ TVec2f mAngle;
    /* 0x5C */ TPos3f mMtx;
};
