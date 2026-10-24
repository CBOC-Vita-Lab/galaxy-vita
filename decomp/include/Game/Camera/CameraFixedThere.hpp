#pragma once

#include "Game/Camera/Camera.hpp"

class CameraFixedThere : public Camera {
public:
    enum CameraType {
        /* 0x0 */ CameraType_GravityUp,
        /* 0x1 */ CameraType_WorldUp,
    };

    CameraFixedThere(const char* pName = "\x82\xbb\x82\xcc\x8f\xea\x92\xe8\x93\x5f\x83\x4a\x83\x81\x83\x89");

    virtual ~CameraFixedThere();
    virtual void reset();
    virtual CameraTargetObj* calc();
    virtual bool isCorrectingErpPositionOff() const {
        return true;
    }
    virtual CamTranslatorBase* createTranslator();

    void setParam(u32 cameraType, bool isFovyFixed) {
        mCameraType = cameraType;
        mIsFovyFixed = isFovyFixed;
    }

    void copyStatusFromPrevCamera();
    bool calcEyeDir(TVec3f*);
    void makeAxisAndRoll();
    void updateUpVec(const TVec3f&);
    void updateNormalUpVec(const TVec3f&);

    /* 0x4C */ u32 mCameraType;
    /* 0x50 */ bool mIsFovyFixed;
    /* 0x54 */ TVec3f mUp;
    /* 0x60 */ TVec3f mAxis;
    /* 0x6C */ f32 mRoll;
};
