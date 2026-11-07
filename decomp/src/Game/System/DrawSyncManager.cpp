#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/Util/SingletonHolder.hpp"
#ifdef TARGET_PC
#include "port/hot_knob.h"
#include <cstdlib>

// GXV_DRAWSYNC_QUIET (default 1, exact; ported from Producdevity's fork, "DrawSync: no thread wake-ups for break points the port
// ignores"): the DrawSync thread only keeps a FIFO of break-point addresses to drive GXEnableBreakPt / GXDisableBreakPt, which do
// nothing in the port (platform/src/gx/gx_fifo.cpp), and nothing else reads that FIFO. Each pushBreakPoint and each draw-sync token
// sent it a message, which made it ready at a higher priority than the game's thread: two guest thread switches (two host-thread
// hand-offs of the "CPU", os_thread.cpp) per break point and per token, on core 0. With the switch on, no message is sent; the
// token callbacks (TalkPeekZ, StarPointerDirector, LensFlare) still run from the token interrupt as before. 0 = the old messages.
static bool drawSyncQuiet() {
    return PORT_HOT_KNOB(bool, [] { const char* e = getenv("GXV_DRAWSYNC_QUIET"); return !(e && *e && atoi(e) == 0); }());
}
#endif

DrawSyncManager* DrawSyncManager::sInstance;

DrawSyncManager* DrawSyncManager::start(u32 capacity, s32 priority) {
    if (sInstance == nullptr) {
        sInstance = new DrawSyncManager(capacity, priority);
    }

    return sInstance;
}

void DrawSyncManager::prepareReset() {
    if (sInstance == nullptr) {
        return;
    }

    sInstance->reset(true);
}

void DrawSyncManager::resetIfAborted() {
    if (sInstance == nullptr) {
        return;
    }

    sInstance->reset(false);
}

void DrawSyncManager::clearFifo() {
    while (mFifo->getCount() != 0) {
        mFifo->pop();
    }
}

void* Fifo::pop() {
    void* pValue = mArray[mLoopIdx];
    mLoopIdx = getLoopIdx(mLoopIdx + 1);
    return pValue;
}

u32 Fifo::getLoopIdx(u32 index) {
    if (index >= mCount + 1) {
        index = 0;
    }

    return index;
}

u32 Fifo::getCount() {
    if (mLoopIdx <= mWriteIndex) {
        return mWriteIndex - mLoopIdx;
    }

    return mWriteIndex + mCount + 1 - mLoopIdx;
}

void DrawSyncManager::end() {
    if (sInstance == nullptr) {
        return;
    }

    delete sInstance;
    sInstance = nullptr;
}

void DrawSyncManager::drawSyncCallback(u16 token) {
    if (sInstance != nullptr) {
        sInstance->drawSyncCallbackSub(token);
    }
}

void* DrawSyncManager::threadFunc(void* pArg) {
    DrawSyncManager* pManager = static_cast< DrawSyncManager* >(pArg);

    while (true) {
        OSMessage message;
        OSReceiveMessage(&pManager->mQueue, &message, OS_MESSAGE_BLOCK);

        if (port_is_native_range(message, 1)) {
            pManager->mFifo->push(message);
            const bool aborted = pManager->mAborted;

            if (pManager->mFifo->getCount() == 2) {
                GXEnableBreakPt(message);
            }
        } else if (reinterpret_cast< u32 >(message) < 0x10000) {
            pManager->mFifo->pop();
            const bool aborted = pManager->mAborted;

            u32 count = pManager->mFifo->getCount();
            if (count != 0) {
                if (count == 1) {
                    GXDisableBreakPt();
                } else if (count >= 2) {
                    GXEnableBreakPt(pManager->mFifo->peek(1));
                }
            }
        } else {
            break;
        }
    }

    return nullptr;
}

DrawSyncManager::DrawSyncManager(u32 capacity, s32 priority) {
    mPreparingReset = false;
    mStack = nullptr;
    mMessageBuffer = nullptr;
    mFlags = 0;
    mLowTokenCount = 0;
    mHighTokenCount = 0;
    mAborted = false;

    mStack = new u8[0x8000];
    OSCreateThread(&mThread, threadFunc, this, mStack + 0x8000, 0x8000, priority, 0);
    mMessageBuffer = new OSMessage[20];
    OSInitMessageQueue(&mQueue, mMessageBuffer, 20);
    mFifo = new Fifo(capacity);
    OSResumeThread(&mThread);
    GXSetDrawSyncCallback(drawSyncCallback);
}

DrawSyncManager::~DrawSyncManager() {
    GXSetDrawSyncCallback(nullptr);
    GXDisableBreakPt();
    OSSendMessage(&mQueue, reinterpret_cast< OSMessage >(0x10000), OS_MESSAGE_BLOCK);
    OSJoinThread(&mThread, nullptr);
}

u16 DrawSyncManager::setCallback(u32 index, u16 count, DrawSyncCallback* pCallback) {
    if (index < 3) {
        u16 start = mLowTokenCount + 1;
        mTokenRanges[index] = TDrawSyncTokenRange(start, start + count - 1, pCallback);
        mLowTokenCount += count;
        return start;
    }

    u16 start = mHighTokenCount + 0xA000;
    mTokenRanges[index] = TDrawSyncTokenRange(start, start + count - 1, pCallback);
    mHighTokenCount += count;
    return start;
}

void DrawSyncManager::reset(bool preparing) {
    if (preparing) {
        mPreparingReset = true;
        return;
    }

    if (mPreparingReset) {
        mPreparingReset = false;
        mAborted = true;
        GXSetDrawSyncCallback(drawSyncCallback);
        SingletonHolder< GameSystem >::get()->initGX();
    }
}

void DrawSyncManager::drawSyncCallbackSub(u16 token) {
    if (token == 0) {
#ifdef TARGET_PC
        if (drawSyncQuiet()) return;
#endif
        if ((mFlags & 2) == 0) {
            OSSendMessage(&mQueue, reinterpret_cast< OSMessage >(token), OS_MESSAGE_BLOCK);
        }

        return;
    }

    for (u16 i = 0; i < 5; i++) {
        if (mTokenRanges[i].mCallback != nullptr && mTokenRanges[i].mStart <= token && token <= mTokenRanges[i].mEnd) {
            mTokenRanges[i].mCallback->drawSyncCallback(token);

#ifdef TARGET_PC
            if (drawSyncQuiet()) break;
#endif
            if ((mFlags & 2) == 0) {
                OSSendMessage(&mQueue, reinterpret_cast< OSMessage >(token), OS_MESSAGE_BLOCK);
            }

            break;
        }
    }
}

void DrawSyncManager::pushBreakPoint() {
    if ((mFlags & 3) == 0) {
        GXFlush();
#ifdef TARGET_PC
        if (drawSyncQuiet()) return;  // (the break point's address would only feed the FIFO above)
#endif
        GXFifoObj fifoObj;
        GXGetCPUFifo(&fifoObj);

        void* pRead;
        void* pWrite;
        GXGetFifoPtrs(&fifoObj, &pRead, &pWrite);
        OSSendMessage(&mQueue, pWrite, OS_MESSAGE_BLOCK);
    }
}
