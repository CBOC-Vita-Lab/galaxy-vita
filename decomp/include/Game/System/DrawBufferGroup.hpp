#pragma once

#include "Game/System/DrawBufferExecuter.hpp"
#include "Game/Util/Array.hpp"

class LiveActor;
class J3DModel;

class DrawBufferGroup {
public:
    DrawBufferGroup();

    void init(s32);
    s32 registerDrawBuffer(LiveActor*);
    void allocateActorListBuffer();
    void active(LiveActor*, s32);
    void deactive(LiveActor*, s32);
    void findLightInfo(LiveActor*, s32);
    void entry();
    void drawOpa() const;
    void drawXlu() const;
    void setDrawCameraType(s32);
    void setLightType(s32);
    s32 findExecuterIndex(const char*) const;
#ifdef TARGET_PC
    // GXV_HRW (hrw28, PortHrwSet.cpp): this group's pass for ONE executor and ONE model of it (the pair's partial draw): the group's setup as drawOpa / drawXlu make it, then the executor's
    // drawers send only that model's packets (gPortHrwOnly).
    void portHrwDrawOne(const DrawBufferExecuter* pExecuter, bool xlu, const J3DModel* pOnly) const;
#endif

    bool isExecutorEmpty(int index) const {
        return mExecutors[index]->mNumActors == 0;
    }

    /* 0x00 */ MR::Vector< MR::AssignableArray< DrawBufferExecuter* > > mExecutors;
    /* 0x0C */ MR::Vector< MR::AssignableArray< DrawBufferExecuter* > > mActiveExecutors;
    /* 0x18 */ s32 mDrawCameraType;
    /* 0x1C */ s32 mLightType;
    /* 0x20 */ s32 mLightLoadType;
};
