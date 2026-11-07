#pragma once

#include <revolution/types.h>

class XanimePlayer;
class J3DAnmTransform;

class SyncBckEffectInfo {
public:
    class BckResourceInfo {
    public:
        BckResourceInfo(const XanimePlayer* pPlayer, const char* pName);
        bool isLoop() const;

        /* 0x00 */ const char* mName;
        /* 0x04 */ J3DAnmTransform* mResource;
    };

    SyncBckEffectInfo(const XanimePlayer* pPlayer, const char* pName, s32 capacity, f32 startFrame, f32 endFrame, bool continueAnimEnd);
    void addBck(const XanimePlayer* pPlayer, const char* pName);
    bool isRegisteredBck(const char* pName) const;
    bool isBckLoop(const char* pName) const;

    /* 0x00 */ BckResourceInfo** mBckResources;
    /* 0x04 */ s32 mCapacity;
    /* 0x08 */ s32 mCount;
    /* 0x0C */ f32 mStartFrame;
    /* 0x10 */ f32 mEndFrame;
    /* 0x14 */ bool mContinueAnimEnd;
#ifdef TARGET_PC
    // (port) The last name looked up and the index of its resource (-1: none). SyncBckEffectChecker asks with the same name, a
    // pointer into the actor's animation table, several times a turn for each emitter.
    s32 portFind(const char* pName) const;
    mutable const char* mPortLookupName;
    mutable s32 mPortLookupIndex;
#endif
};

namespace MR {
    namespace Effect {
        bool isExistSyncBckDeleteFrame(const SyncBckEffectInfo* pInfo);
    }
}  // namespace MR
