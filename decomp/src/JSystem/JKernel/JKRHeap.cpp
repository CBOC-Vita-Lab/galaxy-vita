#include "JSystem/JKernel/JKRHeap.hpp"
#include "JSystem/JUtility/JUTException.hpp"
#include <revolution/os/OSBootInfo.h>

JKRHeap* JKRHeap::sSystemHeap;
JKRHeap* JKRHeap::sCurrentHeap;
JKRHeap* JKRHeap::sRootHeap;
JKRErrorHandler JKRHeap::mErrorHandler;

static bool byte_806B70B8;

void* JKRHeap::mCodeStart;
void* JKRHeap::mCodeEnd;
void* JKRHeap::mUserRamStart;
void* JKRHeap::mUserRamEnd;
u32 JKRHeap::mMemorySize;

static bool byte_806B26D8 = true;
u32 ARALT_AramStartAdr;

JKRHeap::JKRHeap(void* data, u32 size, JKRHeap* parent, bool error) : JKRDisposer(), mChildTree(this), mDisposerList() {
    OSInitMutex(&mMutex);
    mSize = size;
    mStart = (u8*)data;
    mEnd = (u8*)data + size;

    if (parent == nullptr) {
        JKRHeap::sSystemHeap = this;
        JKRHeap::sCurrentHeap = this;
    } else {
        parent->mChildTree.appendChild(&mChildTree);

        if (JKRHeap::sSystemHeap == JKRHeap::sRootHeap) {
            JKRHeap::sSystemHeap = this;
        }

        if (JKRHeap::sCurrentHeap == JKRHeap::sRootHeap) {
            JKRHeap::sCurrentHeap = this;
        }
    }

    mErrorFlag = error;

    if (mErrorFlag == true && mErrorHandler == nullptr) {
        mErrorHandler = JKRDefaultMemoryErrorRoutine;
    }

    _3C = byte_806B26D8;
    _3D = byte_806B70B8;
    _69 = false;
}

JKRHeap::~JKRHeap() {
    mChildTree.getParent()->removeChild(&mChildTree);
    JSUTree< JKRHeap >* nextRootHeap = sRootHeap->mChildTree.getFirstChild();

    if (sCurrentHeap == this)
        sCurrentHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();

    if (sSystemHeap == this)
        sSystemHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();
}

bool JKRHeap::initArena(char** memory, u32* size, int maxHeaps) {
    void *ramStart, *ramEnd, *arenaStart;

    void* arenaLo = OSGetArenaLo();
    void* arenaHi = OSGetArenaHi();

    OSReport("original arenaLo = %p arenaHi = %p\n", arenaLo, arenaHi);

    if (arenaLo == arenaHi) {
        return false;
    }

    arenaStart = OSInitAlloc(arenaLo, arenaHi, maxHeaps);
    OSBootInfo* code = (OSBootInfo*)OSPhysicalToCached(0);
    ramStart = (void*)(((u32)arenaStart + 31) & 0xFFFFFFE0);
    ramEnd = (void*)((u32)arenaHi & 0xFFFFFFE0);

    JKRHeap::mCodeStart = code;
    JKRHeap::mCodeEnd = ramStart;
    JKRHeap::mUserRamStart = ramStart;
    JKRHeap::mUserRamEnd = ramEnd;
    JKRHeap::mMemorySize = code->memorySize;

    OSSetArenaLo(ramEnd);
    OSSetArenaHi(ramEnd);

    *memory = (char*)ramStart;
    *size = (u32)ramEnd - (u32)ramStart;
    return true;
}

JKRHeap* JKRHeap::becomeSystemHeap() {
    JKRHeap* sys = sSystemHeap;
    sSystemHeap = this;
    return sys;
}

JKRHeap* JKRHeap::becomeCurrentHeap() {
    JKRHeap* cur = sCurrentHeap;
    sCurrentHeap = this;
    return cur;
}

void JKRHeap::destroy(JKRHeap* pHeap) {
    pHeap->do_destroy();
}

void* JKRHeap::alloc(u32 size, int align, JKRHeap* pHeap) {
    if (pHeap != nullptr) {
        return pHeap->alloc(size, align);
    }

    if (JKRHeap::sCurrentHeap != nullptr) {
        return JKRHeap::sCurrentHeap->alloc(size, align);
    }

    return nullptr;
}

void* JKRHeap::alloc(u32 size, int align) {
    return do_alloc(size, align);
}

void JKRHeap::free(void* pData, JKRHeap* pHeap) {
    if (!pHeap) {
        pHeap = findFromRoot(pData);

        if (!pHeap) {
            return;
        }
    }

    pHeap->do_free(pData);
}

void JKRHeap::free(void* pData) {
    do_free(pData);
}

void JKRHeap::callAllDisposer() {
    while (mDisposerList.mHead != nullptr) {
        reinterpret_cast< JKRDisposer* >(mDisposerList.mHead->mData)->~JKRDisposer();
    }
}

void JKRHeap::freeAll() {
    do_freeAll();
}

void JKRHeap::freeTail() {
    do_freeTail();
}

s32 JKRHeap::resize(void* pData, u32 size) {
    return do_resize(pData, size);
}

s32 JKRHeap::getFreeSize() {
    return do_getFreeSize();
}

void* JKRHeap::getMaxFreeBlock() {
    return do_getMaxFreeBlock();
}

s32 JKRHeap::getTotalFreeSize() {
    return do_getTotalFreeSize();
}

JKRHeap* JKRHeap::findFromRoot(void* pData) {
    JKRHeap* root = sRootHeap;

    if (root == nullptr) {
        return nullptr;
    }

    if ((void*)root->mStart <= pData && pData < (void*)root->mEnd) {
        return root->find(pData);
    }

    return root->findAllHeap(pData);
}

JKRHeap* JKRHeap::find(void* pData) const {
    if (mStart <= pData && pData < mEnd) {
        const JSUTree< JKRHeap >& tree = mChildTree;

        if (tree.getNumChildren() != 0) {
            for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
                JKRHeap* result = iterator->find(pData);

                if (result) {
                    return result;
                }
            }
        }

        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

JKRHeap* JKRHeap::findAllHeap(void* ptr) const {
    if (mChildTree.getNumChildren() != 0) {
        for (JSUTreeIterator< JKRHeap > iterator(mChildTree.getFirstChild()); iterator != mChildTree.getEndChild(); ++iterator) {
            JKRHeap* heap = iterator->findAllHeap(ptr);

            if (heap != nullptr) {
                return heap;
            }
        }
    }

    if (mStart <= ptr && ptr < mEnd) {
        return const_cast< JKRHeap* >(this);
    }

    return nullptr;
}

void JKRHeap::dispose_subroutine(u32 start, u32 end) {
    JSUListIterator< JKRDisposer > it(mDisposerList.getFirst());
    JSUListIterator< JKRDisposer > last_it;

    JSULink< JKRDisposer >* link;
    while ((link = it.mLink) != nullptr) {
        JKRDisposer* disp = link->getObject();

        if (reinterpret_cast< void* >(start) <= disp && disp < reinterpret_cast< void* >(end)) {
            disp->~JKRDisposer();

            if (last_it == nullptr) {
                it = mDisposerList.getFirst();
            } else {
                it = last_it;
                it++;
            }
        } else {
            last_it = it;
            it++;
        }
    }
}

bool JKRHeap::dispose(void* ptr, u32 size) {
    u32 begin = (u32)ptr;
    u32 end = (u32)ptr + size;
    dispose_subroutine(begin, end);
    return false;
}

void JKRHeap::dispose(void* begin, void* end) {
    dispose_subroutine((u32)begin, (u32)end);
}

void JKRHeap::dispose() {
    const JSUList< JKRDisposer >& list = mDisposerList;
    JSUListIterator< JKRDisposer > iterator;

    while (list.getFirst() != list.getEnd()) {
        iterator = list.getFirst();
        iterator->~JKRDisposer();
    }
}

void JKRHeap::copyMemory(void* pDst, void* pSrc, u32 size) {
    u32 count = (size + 3) / 4;
    u32* dst_32 = (u32*)pDst;
    u32* src_32 = (u32*)pSrc;

    while (count > 0) {
        *dst_32 = *src_32;
        dst_32++;
        src_32++;
        count--;
    }
}

void JKRDefaultMemoryErrorRoutine(void* pHeap, u32 size, int alignment) {
    JUTException::panic(__FILE__, 0x355, "abort\n");
}

JKRErrorHandler JKRHeap::setErrorHandler(JKRErrorHandler errorHandler) {
    JKRErrorHandler prev = JKRHeap::mErrorHandler;

    if (!errorHandler) {
        errorHandler = JKRDefaultMemoryErrorRoutine;
    }

    JKRHeap::mErrorHandler = errorHandler;
    return prev;
}

#ifdef TARGET_PC
// The original game routes every `new` into the current JKRHeap.  On the port the
// same process also hosts the renderer, audio device and libc++, which must not
// allocate from (or race on) the game heaps.  Only emulated game threads outside a
// host-allocation scope use JKRHeap; everything else goes to malloc.
#include "port/heap_routing.h"
extern "C" void port_irq_poll_at(void* site);
#include <stdlib.h>

// (PETARI_NEWPOLL=0 leaves the safe point out, for tests)
static bool newPollOn() {
    static const bool on = [] {
        const char* e = getenv("PETARI_NEWPOLL");
        return !(e && *e == '0');
    }();
    return on;
}

#if defined(__vita__)
// PS Vita port, C2-12828-1 (twice on 18 October 2026): when the "current" heap is destroyed (the end of a scene), ~JKRHeap hands
// "current" to the first child of the root heap, which here is the root heap of MEM2 and has nothing free.  On the console it stays
// there until the next scene sets its own heap, and nothing allocates in between.  In the port a game thread does (always 22 bytes,
// as soon as the main thread waits for the drawing at the end of the scene), and the game stopped with HEAP FULL.  Once the game
// heaps exist, a `new` that would go to a root heap is served by the host allocator instead: such a block is in no game heap, so
// no heap teardown can take it away from its owner, and delete frees it (gameFree below tells the two kinds of pointer apart).
// (No logging here: `new` is called from everywhere, the logger included; entry.cpp reports the counters with its status lines.)
bool g_portHostFallbackOn = false;   // set by HeapMemoryWatcher::createGameHeap
JKRHeap* g_portRootHeap2 = nullptr;  // the MEM2 root heap (HeapMemoryWatcher::createRootHeap)
unsigned g_portRootAllocs = 0;       // how many requests were served that way,
unsigned g_portRootAllocFirst = 0;   // the size of the first,
unsigned g_portRootAllocLast = 0;    // and of the latest
static bool portCurrentIsRootHeap(size_t size, int align) {
    (void)align;
    if (!g_portHostFallbackOn) {
        return false;
    }
    JKRHeap* current = JKRHeap::getCurrentHeap();
    if (current != JKRHeap::sRootHeap && current != g_portRootHeap2) {
        return false;
    }
    if (g_portRootAllocs++ == 0) {
        g_portRootAllocFirst = (unsigned)size;
    }
    g_portRootAllocLast = (unsigned)size;
    return true;
}
#endif

// (`site`: the code that called operator new, named in the safe-point gap log, PETARI_GAPLOG)
static inline __attribute__((always_inline)) void* gameAlloc(size_t size, int align, JKRHeap* pHeap, void* site) {
    if (!port_use_game_heap() || (pHeap == nullptr && JKRHeap::getCurrentHeap() == nullptr)
#if defined(__vita__)
        || (pHeap == nullptr && portCurrentIsRootHeap(size, align))
#endif
    ) {
        void* p = port_host_alloc(size, align < 0 ? -align : align);
        if (p == nullptr) {
            port_host_alloc_failed(size, align < 0 ? -align : align);  // (never returns: `new` must not give back null)
        }
        return p;
    }
    // A safe point before the allocation (not inside it): loading code allocates all the time, and this is where the audio thread
    // (which the console's higher priority would let in) gets its turn.  Nothing is held here.
    if (newPollOn()) {
        port_irq_poll_at(site);
    }
    return JKRHeap::alloc(size, align, pHeap);
}

static inline void gameFree(void* pData) {
    if (pData == nullptr) {
        return;
    }
    if (!port_is_game_heap_ptr(pData)) {
        port_host_free(pData);
        return;
    }
    JKRHeap::free(pData, nullptr);
}

void* operator new(size_t size) {
    return gameAlloc(size, 4, nullptr, __builtin_return_address(0));
}

void* operator new(size_t size, int align) {
    return gameAlloc(size, align, nullptr, __builtin_return_address(0));
}

void* operator new(size_t size, JKRHeap* pHeap) {
    return gameAlloc(size, 4, pHeap, __builtin_return_address(0));
}

void* operator new(size_t size, JKRHeap* pHeap, int align) {
    return gameAlloc(size, align, pHeap, __builtin_return_address(0));
}

void* operator new[](size_t size) {
    return gameAlloc(size, 4, nullptr, __builtin_return_address(0));
}

void* operator new[](size_t size, int align) {
    return gameAlloc(size, align, nullptr, __builtin_return_address(0));
}

void* operator new[](size_t size, JKRHeap* pHeap, int align) {
    return gameAlloc(size, align, pHeap, __builtin_return_address(0));
}

void operator delete(void* pData) noexcept {
    gameFree(pData);
}

void operator delete[](void* pData) noexcept {
    gameFree(pData);
}

void operator delete(void* pData, size_t) noexcept {
    gameFree(pData);
}

void operator delete[](void* pData, size_t) noexcept {
    gameFree(pData);
}

void operator delete(void* pData, int) noexcept {
    gameFree(pData);
}

void operator delete(void* pData, JKRHeap*, int) noexcept {
    gameFree(pData);
}

void operator delete[](void* pData, int) noexcept {
    gameFree(pData);
}

void operator delete[](void* pData, JKRHeap*, int) noexcept {
    gameFree(pData);
}
#else
void* operator new(u32 size) {
    return JKRHeap::alloc(size, 4, nullptr);
}

void* operator new(u32 size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new(u32 size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void* operator new[](u32 size) {
    return JKRHeap::alloc(size, 4, nullptr);
}

void* operator new[](u32 size, int align) {
    return JKRHeap::alloc(size, align, nullptr);
}

void* operator new[](u32 size, JKRHeap* pHeap, int align) {
    return JKRHeap::alloc(size, align, pHeap);
}

void operator delete(void* pData) {
    JKRHeap::free(pData, nullptr);
}

void operator delete[](void* pData) {
    JKRHeap::free(pData, nullptr);
}

#endif

void JKRHeap::state_register(TState*, u32) const {
    return;
}

bool JKRHeap::state_compare(const TState& lhs, const TState& rhs) const {
    return lhs.mCheckCode == rhs.mCheckCode;
}

void JKRHeap::state_dump(const TState&) const {
    return;
}

void JKRHeap::setAltAramStartAdr(u32 addr) {
    ARALT_AramStartAdr = addr;
}

u32 JKRHeap::getAltAramStartAdr() {
    return ARALT_AramStartAdr ? ARALT_AramStartAdr : (u32)(uintptr_t)OSPhysicalToCached(0x10000000);
}

s32 JKRHeap::do_changeGroupID(u8) {
    return 0;
}

u8 JKRHeap::do_getCurrentGroupId() {
    return 0;
}

bool JKRHeap::dump_sort() {
    return true;
}
