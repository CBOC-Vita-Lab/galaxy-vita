#pragma once

#include "Game/NameObj/NameObj.hpp"
#include "Game/Util/Color.hpp"
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <revolution.h>

class VolumeModelDrawer : public NameObj {
public:
    VolumeModelDrawer(const char*, const char*, MtxPtr);

    virtual void draw() const;

    void loadModelDrawMtx() const NO_INLINE;

    /* 0x0C */ MtxPtr mMtx;
    /* 0x10 */ J3DModelData* mModelData;
    /* 0x14 */ Color8 mColor;

#ifdef TARGET_PC
    // GXV_LIGHTVOL (lightvol25, port/lightvol.h): the draw is recorded and the whole list drawn after the category's pass (flushDeferred, SceneFunction::executeDrawListXlu).
    // The members below are the port's own (the drawer is a heap object of the game's, never copied from a table of fixed layout).
    bool portRecord() const;
    bool portBox(float* pMin, float* pMax) const;
    static void portBegin();
    static void flushDeferred();
    mutable float mLvMin[3] = {0, 0, 0};
    mutable float mLvMax[3] = {0, 0, 0};
    mutable signed char mLvBox = 0;  // 0 not read, 1 the model's box, -1 none
    mutable bool mLvHave = false;    // f / keep below are of the last picture's plan
    mutable bool mLvKeep = false;
    mutable float mLvF = 1.0f;
    mutable unsigned mLvPic = 0;     // the picture of that plan (a plan older than the last picture is dropped: the fade restarts)
    char mLvName[28] = {0};
#endif
};

class VolumeModelDrawInit : public NameObj {
public:
    VolumeModelDrawInit();
};
