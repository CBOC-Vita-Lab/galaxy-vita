#pragma once

#include "Game/Camera/CameraTowerBase.hpp"

class CameraTowerPos : public CameraTowerBase {
public:
    CameraTowerPos(const char* pName = "\x93\x83\x83\x4a\x83\x81\x83\x89\x81\x69\x83\x54\x83\x75\x83\x5e\x81\x5b\x83\x51\x83\x62\x83\x67\x95\x74\x82\xab\x81\x6a");
    virtual ~CameraTowerPos();

    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isEnableToReset() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    void setParam(const TVec3f& wPoint, const TVec3f& axis, f32 dist, const TVec2f& angle, f32 angleXRate) {
        mWPoint.set(wPoint);
        mAxis.set(axis);
        mDist = dist;
        mAngleYMaxSpeed = angle.y;
        mAngleX = angle.x;
        mAngleXRate = angleXRate;
    }

    void calcIdealPose(bool);
    void arrangeHorizontalPose(const TVec3f&, TVec3f&);
    void arrangeWatchPos(TVec3f&, const TVec3f&);

    /* 0x6C */ TVec3f mWPoint;
    /* 0x78 */ TVec3f mAxis;
    /* 0x84 */ f32 mAngleYMaxSpeed;
    /* 0x88 */ f32 mAngleX;
    /* 0x8C */ f32 mDist;
    /* 0x90 */ f32 mAngleXRate;
    /* 0x94 */ TVec3f mPos;
};
