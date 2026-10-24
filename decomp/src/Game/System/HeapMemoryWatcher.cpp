#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <revolution/os.h>
#include <revolution/wpad.h>
#if defined(PORT_ARMHF_HARNESS)
#include "Game/Util/SingletonHolder.hpp"
#endif

JKRExpHeap* HeapMemoryWatcher::sRootHeapGDDR3;

#if defined(__vita__)
// (JKRHeap.cpp: where a `new` goes while the current heap is a root heap)
extern bool g_portHostFallbackOn;
extern JKRHeap* g_portRootHeap2;
#endif

namespace {
    JKRExpHeap* createExpHeap(u32 size, JKRHeap* pHeap, bool a3) NO_INLINE {
        JKRExpHeap* heap;

        if (a3) {
            void* data = new (pHeap, -4) u8[size];
            heap = JKRExpHeap::create(data, size, pHeap, true);
        } else {
            heap = JKRExpHeap::create(size, pHeap, true);
        }

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    JKRSolidHeap* createSolidHeap(u32 size, JKRHeap* pHeap) {
        JKRSolidHeap* heap = JKRSolidHeap::create(size, pHeap, true);

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    void destroyHeapAndSetNULL(JKRHeap** pHeap) {
        if (*pHeap != nullptr) {
            JKRHeap::destroy(*pHeap);
            *pHeap = nullptr;
        }
    }
};  // namespace

JKRHeap* HeapMemoryWatcher::getHeapNapa(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapNapa;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapNapa;
    }

    return nullptr;
}

JKRHeap* HeapMemoryWatcher::getHeapGDDR3(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapGDDR;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapGDDR;
    }

    return nullptr;
}

void HeapMemoryWatcher::createFileCacheHeapOnGameHeap(u32 size) {
    mFileCacheHeap = ::createSolidHeap(size, mGameHeapGDDR);
}

void HeapMemoryWatcher::createSceneHeapOnGameHeap() {
    mSceneHeapNapa = ::createSolidHeap(-1, mGameHeapNapa);
    mSceneHeapGDDR = ::createSolidHeap(-1, mGameHeapGDDR);
}

void HeapMemoryWatcher::adjustStationedHeaps() {
    MR::adjustHeapSize(mStationedHeapNapa, 0);
    MR::adjustHeapSize(mStationedHeapGDDR, 0);
}

void HeapMemoryWatcher::setCurrentHeapToStationedHeap() {
    MR::becomeCurrentHeap(mStationedHeapNapa);
}

void HeapMemoryWatcher::setCurrentHeapToGameHeap() {
    MR::becomeCurrentHeap(JKRHeap::sSystemHeap);
}

void HeapMemoryWatcher::setCurrentHeapToSceneHeap() {
    MR::becomeCurrentHeap(mSceneHeapNapa);
}

void HeapMemoryWatcher::destroySceneHeap() {
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
}

void HeapMemoryWatcher::destroyGameHeap() {
    if (mSceneHeapNapa != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    }

    if (mSceneHeapGDDR != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
    }

    if (mFileCacheHeap != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mFileCacheHeap));
    }

    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapGDDR));
    createGameHeap();
}

void HeapMemoryWatcher::createRootHeap() {
    JKRExpHeap* pHeap;
    void* newHi;
    u32 arenaHi, arenaLo;

    JKRExpHeap::createRoot(1, true);
    arenaLo = reinterpret_cast< u32 >(OSGetMEM2ArenaLo());
    arenaHi = reinterpret_cast< u32 >(OSGetMEM2ArenaHi());
    newHi = reinterpret_cast< void* >(arenaLo + 0xE00000);
    OSSetMEM2ArenaHi(newHi);
    JKRHeap::setAltAramStartAdr(arenaLo);
    pHeap = JKRExpHeap::create(newHi, arenaHi - reinterpret_cast< u32 >(newHi), JKRHeap::sRootHeap, true);

    if (MR::isEqualCurrentHeap(pHeap)) {
        JKRHeap::sRootHeap->becomeCurrentHeap();
    }

    HeapMemoryWatcher::sRootHeapGDDR3 = pHeap;
#if defined(__vita__)
    g_portRootHeap2 = pHeap;
#endif
}

#if defined(PORT_ARMHF_HARNESS)
// armhf/qemu harness: PETARI_HEAP_SCALE=<k> multiplies the stationed heaps (default 1: the Vita's / the Wii's sizes), to see
// whether a scene that runs out of memory with the Vita sizes is just short of stationed-heap space.
#include <stdlib.h>
static u32 portHeapScale(u32 size) {
    static const char* e = getenv("PETARI_HEAP_SCALE");
    const int k = e ? atoi(e) : 1;
    return size * (u32)(k > 0 ? k : 1);
}
#define PORT_HEAP(size) portHeapScale(size)
#elif defined(TARGET_PC) && !defined(__vita__)
// Objects are larger on the 64-bit port (pointers, vtables); the port's
// desktop memory banks are 128 MiB each. Vita retains the original Wii heaps.
#define PORT_HEAP(size) ((size) * 2)
#else
#define PORT_HEAP(size) (size)
#endif

void HeapMemoryWatcher::createHeaps() {
    MR::CurrentHeapRestorer heapRestorer = MR::CurrentHeapRestorer(JKRHeap::sRootHeap);
    ::createExpHeap(PORT_HEAP(0x40000), JKRHeap::sRootHeap, false)->becomeSystemHeap();
    mAudSystemHeap = ::createSolidHeap(PORT_HEAP(0x1E0000), JKRHeap::sRootHeap);
    mStationedHeapNapa = ::createExpHeap(PORT_HEAP(0x900000), JKRHeap::sRootHeap, false);
    JKRHeap* pRootHeapGDDR = HeapMemoryWatcher::sRootHeapGDDR3;
    u32 wpadHeapSize = OSRoundUp32B(WPADGetWorkMemorySize()) + 0xD0;
    mWPadHeap = ::createExpHeap(wpadHeapSize, pRootHeapGDDR, false);
    mHomeButtonLayoutHeap = ::createExpHeap(PORT_HEAP(0x80000), HeapMemoryWatcher::sRootHeapGDDR3, false);
    mStationedHeapGDDR = ::createExpHeap(PORT_HEAP(0x1400000), HeapMemoryWatcher::sRootHeapGDDR3, false);
    createGameHeap();
}

void HeapMemoryWatcher::createGameHeap() {
    mGameHeapNapa = ::createExpHeap(-1, JKRHeap::sRootHeap, false);
    mGameHeapGDDR = ::createExpHeap(-1, HeapMemoryWatcher::sRootHeapGDDR3, false);
#if defined(__vita__)
    g_portHostFallbackOn = true;
#endif
}

HeapMemoryWatcher::HeapMemoryWatcher()
    : mStationedHeapNapa(nullptr), mStationedHeapGDDR(nullptr), mGameHeapNapa(nullptr), mGameHeapGDDR(nullptr), mFileCacheHeap(nullptr),
      mSceneHeapNapa(nullptr), mSceneHeapGDDR(nullptr), mWPadHeap(nullptr), mHomeButtonLayoutHeap(nullptr), mAudSystemHeap(nullptr) {
    JKRHeap::setErrorHandler(HeapMemoryWatcher::memoryErrorCallback);
    createHeaps();
}

void HeapMemoryWatcher::memoryErrorCallback(void* heap, u32 size, int alignment) {
#if defined(PORT_ARMHF_HARNESS)
    OSReport("heap error: heap %p size %u alignment %d\n", heap, size, alignment);
    {
        HeapMemoryWatcher* w = SingletonHolder< HeapMemoryWatcher >::get();
        auto show = [](const char* name, JKRHeap* h) {
            if (h) OSReport("heap error:   %-14s %p [%p..%p) free %d total free %d max alloc %u\n", name, (void*)h, (void*)h->getStart(), (void*)h->getEnd(),
                            (int)h->getFreeSize(), (int)h->getTotalFreeSize(), (unsigned)h->getMaxAllocatableSize(4));
            else OSReport("heap error:   %-14s (none)\n", name);
        };
        show("root GDDR3", HeapMemoryWatcher::sRootHeapGDDR3);
        show("game GDDR", w->mGameHeapGDDR);
        show("game Napa", w->mGameHeapNapa);
        show("stationed GDDR", w->mStationedHeapGDDR);
        show("stationed Napa", w->mStationedHeapNapa);
        show("file cache", w->mFileCacheHeap);
        show("scene GDDR", w->mSceneHeapGDDR);
        show("scene Napa", w->mSceneHeapNapa);
        show("failing heap", (JKRHeap*)heap);
    }
#endif
    OSPanic(__FILE__, 537, "");
}

void HeapMemoryWatcher::checkRestMemory() {
}
