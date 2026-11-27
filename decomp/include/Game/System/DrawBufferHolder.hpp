#pragma once

#include "Game/Util/Array.hpp"

class DrawBufferGroup;
class LiveActor;
#ifdef TARGET_PC
struct PortPipeScene;  // (Game/System/PortDrawPipe.hpp)
namespace port {
    namespace drawpipe {
        struct Frame;
        struct PassStats;
        class Set;
        class Names;
    }  // namespace drawpipe
}  // namespace port
#endif

typedef MR::Vector< MR::AssignableArray< DrawBufferGroup* > > ExecutorList;

struct DrawBufferInitialTable {
    /* 0x00 */ u32 mDrawBufferType;
    /* 0x04 */ s32 mCapacity;
    /* 0x08 */ u32 mLightType;
    /* 0x0C */ u32 mDrawCameraType;
};

class DrawBufferHolder {
public:
    DrawBufferHolder();

    void initTable(const DrawBufferInitialTable*, s32);

    void allocateActorListBuffer();
    void active(LiveActor*, s32, s32);
    s32 registerDrawBuffer(LiveActor*, s32);
    void deactive(LiveActor*, s32, s32);
    void findLightInfo(LiveActor*, s32, s32);
    void entry(s32);
#ifdef TARGET_PC
    void entryPlayer();
    void entryExceptPlayer(s32);
    // GXV_DRAWPIPE (drawpipe24; port/draw_pipe.h, DrawBufferHolder.cpp): the lists the J3D passes walk copied into a frame at the
    // hand-over; a pass drawn from the frame in use (false: the caller draws it from the live lists); everything the passes read,
    // hashed for the census.
    bool portPipeFreeze(port::drawpipe::Frame& rFrame) const;
    bool portPipeDraw(s32 drawBufferType, bool xlu) const;
    void portPipeWalk(port::drawpipe::Set& rSet, port::drawpipe::Names& rNames) const;
    u64 portPipeFreezeGroup(port::drawpipe::Frame* pFrame, s32 drawBufferType) const;
    // (dpengine) the frame in use, with the scene state that goes with it (DrawBufferHolder.cpp has the J3D context's story);
    // the recorded draw's token hand-out; a token executed; a pass drawn from the frame
    static void portPipeUse(const port::drawpipe::Frame* pFrame, port::drawpipe::PassStats* pStats, const PortPipeScene* pScene = nullptr);
    static void portPipeRecord(bool (*pTokenOut)(u32, bool), void (*pAbandon)(int));
    static void portPipeRunToken(u32 id, bool displaced);
    // (pipe27, GXV_PIPE25: the late draw; DrawBufferHolder.cpp)
    static void portPipeLateHooks(void (*pDone)(u32), bool late);
    static void portPipeRewind();
    static bool portPipeTokenInfo(u32 id, s32* pType, bool* pXlu);
    static const char* portPipeGroupName(s32 type);
    void portPipeDrawFrozen(const port::drawpipe::Frame& rFrame, s32 drawBufferType, bool xlu) const;
#endif

    void drawOpa(s32) const;
    void drawXlu(s32) const;

    ExecutorList& getExecuteList(s32 drawBufferType);
    bool isBufferGroupEmpty(s32 drawBufferType) const;
    DrawBufferGroup* getDrawBufferGroup(s32 drawBufferType);

    // debug symbol map functions
    s32 getActiveActorCount(const char*);
    void onEntryAll();
    void offEntryAll();
    void onDrawOpaAll();
    void offDrawOpaAll();
    void onDrawXluAll();
    void offDrawXluAll();

    // TODO: see .cpp
    void dummy(s32);

    /* 0x00 */ MR::AssignableArray< DrawBufferGroup > mBufferGroups;
    /* 0x08 */ ExecutorList mExecuteLists[3];
    /* 0x2C */ bool mTableInitialized;
};
