#pragma once

#include "Game/Camera/Camera.hpp"

class CameraFooFighterPlanet : public Camera {
public:
    CameraFooFighterPlanet(const char* pName = "\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x83\x76\x83\x89\x83\x6c\x83\x62\x83\x67\x83\x4a\x83\x81\x83\x89");
    virtual ~CameraFooFighterPlanet();

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual CamTranslatorBase* createTranslator();

    void goRoundBehind(TVec3f&, TVec3f&, TVec3f&);

    void setParam(f32 distMin, f32 distMax, f32 pitchMax) {
        mDistMin = distMin;
        mDistMax = distMax;
        mPitchMax = pitchMax;
    }

    /* 0x4C */ f32 mDistMin;
    /* 0x50 */ f32 mDistMax;
    /* 0x54 */ f32 mPitchMax;
};
