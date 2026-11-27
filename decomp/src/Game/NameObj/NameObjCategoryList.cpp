#include "Game/NameObj/NameObjCategoryList.hpp"
#include <algorithm>
#ifdef TARGET_PC
#include "Game/LiveActor/LiveActor.hpp"  // (GXV_UPD_PREFETCH2: the offsets of a LiveActor's parts)
#include "Game/LiveActor/ModelManager.hpp"      // (GXV_UPD_PREFETCH3: the model manager's model and pose player)
#include "Game/LiveActor/HitSensorKeeper.hpp"   // (GXV_UPD_PREFETCH3: the keeper's array of sensor infos)
#include "Game/LiveActor/ActorAnimKeeper.hpp"   // (GXV_UPD_PREFETCH3: the animation keeper's info array)
#include "port/upd26.h"                         // (GXV_UPD_PREFETCH3: the stages' reach, host-tested)
#endif

#ifdef TARGET_PC
// GXV_UPD_PREFETCH (update23, 8 Nov; exact: cache hints only; platform/src/port/perf.cpp): while an object of a list runs, the lines of
// the object two places on are asked for (the first 256 bytes: a LiveActor's flags and its keeper / manager pointers that movement and
// calcAnim read first). The objects are scattered heap blocks and each list call starts with misses on them.
extern "C" int port_upd_prefetch_on(void);
extern "C" int port_upd_prefetch2_on(void);       // GXV_UPD_PREFETCH2 (update25), platform/src/port/perf.cpp
extern "C" int port_anim_lod_raw_category(void);  // the list CategoryList::execute is running: 0x100 | MovementType, or a CalcAnimType, or -1
// GXV_UPD_PREFETCH2 (update25, 11 Nov; exact: cache hints only; nothing read here changes a result, and a prefetch of a bad address
// does not fault). The movement lists of LiveActors (map parts, planets, collision objects, map objects, NPCs, rides, enemies, items):
// the object one place on has had its first 256 bytes asked for one step ago (the prefetch above), so its pointers to the parts that
// LiveActor::movement reads first are in the cache; ask for the first line of each of those parts now. Each is a separate heap block,
// cold after the turns since; without this the movement waits on them one miss after another (model manager, nerve, sensor keeper,
// binder, effect keeper, sound object, light controller, shadow list). Only objects the snapshot of the scene's actors holds (a list can also hold a short NameObj that is not an actor: its block is not read).
static inline bool PortListIsLiveActors(int raw) {
    switch (raw - 0x100) {
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C:
        return true;
    default:
        return false;
    }
}
// (update26) the calcAnim lists of actors too: CalcAnimType 0x00..0x0C (clipped map parts .. mirror map objects) and 0x10 (items)
static inline bool PortListIsActorList(int raw) {
    if (raw >= 0 && raw < 0x100) {
        return raw <= 0x0C || raw == 0x10;
    }
    return PortListIsLiveActors(raw);
}
extern "C" int port_live_actor_known(const void* p);  // (LiveActor.cpp: one of the snapshot's actors: the table of the last drawn turn's flush)
static inline void PortListPrefetch2(NameObj* const* pNext, NameObj* const* pEnd) {
    if (pNext < pEnd && port_live_actor_known(*pNext)) {
        const LiveActor* a = reinterpret_cast< const LiveActor* >(*pNext);
        __builtin_prefetch(a->mModelManager);
        __builtin_prefetch(a->mSpine);
        __builtin_prefetch(a->mSensorKeeper);
        __builtin_prefetch(a->mBinder);
        __builtin_prefetch(a->mEffectKeeper);
        __builtin_prefetch(a->mSoundObject);
        __builtin_prefetch(a->mActorLightCtrl);
        __builtin_prefetch(a->mShadowControllerList);
    }
}
static inline void PortListPrefetch(NameObj* const* pNext, NameObj* const* pEnd) {
    if (pNext < pEnd) {
        const char* p = reinterpret_cast< const char* >(*pNext);
        for (int off = 0; off < 256; off += 32) {
            __builtin_prefetch(p + off);
        }
    }
}
// GXV_UPD_PREFETCH3 (update26, 12 Nov; exact: cache hints only, nothing read here changes a result). The second level of the pipeline above.
// With it on the list loop runs three stages ahead: the object's own lines 3 places on, the parts' first lines (PREFETCH2) 2 on, and now, for
// the object ONE place on (its parts' lines were asked for one step ago, so the pointers inside them are in the cache or on their way), the
// blocks those parts point to that the movement and the calcAnim of the list reach first: the J3D model and the pose player behind the model
// manager, the keeper's array of sensor infos, the animation keeper's info array. They are separate heap blocks and each is a dependent miss
// after the part's own (actor -> part -> block). The lists are the movement lists of LiveActors and their calcAnim lists (the same objects:
// port_live_actor_known holds only actors of the scene's snapshot, so a list of layouts or particles is left alone). The pointers read are
// those of an actor in the snapshot (live: parts are created at init and deleted with the actor), a null is skipped, a prefetch of any
// address does not fault. The counts (hints issued, objects left out as unknown) go to the "upd26:" line of the log.
extern "C" int port_upd_prefetch3_on(void);  // (platform/src/port/perf.cpp)
extern "C" void port_upd26_count(int what, unsigned n);
static inline void PortListPrefetch3(NameObj* const* pNext, NameObj* const* pEnd) {
    if (pNext >= pEnd) {
        return;
    }
    if (!port_live_actor_known(*pNext)) {
        port_upd26_count(1, 1);
        return;
    }
    const LiveActor* a = reinterpret_cast< const LiveActor* >(*pNext);
    unsigned n = 0;
    const ModelManager* mm = a->mModelManager;
    if (mm != nullptr) {
        __builtin_prefetch(mm->mModel);
        __builtin_prefetch(mm->mXanimePlayer);
        n += 2;
    }
    const HitSensorKeeper* keeper = a->mSensorKeeper;
    if (keeper != nullptr) {
        __builtin_prefetch(keeper->mSensorInfos);
        n++;
    }
    const ActorAnimKeeper* animKeeper = a->mAnimKeeper;
    if (animKeeper != nullptr) {
        __builtin_prefetch(animKeeper->mInfoArray);
        n++;
    }
    port_upd26_count(0, n);
}
#include "port/diagnostics.h"
#include "port/gt71.h"
#include "port/logic_prof.h"  // (GXV_LOGIC_PROF, logic23: every object of a movement / calc-anim list timed per class when asked for)
// GXV_CLIPCENSUS (cliprange25; ClippingActorHolder.cpp): the actors of the LiveActor lists counted by distance band and kind, one turn in four timed.
// Off (the default): one load and a branch of a global per object.
extern "C" int port_cc_flag;
extern "C" int64_t port_cc_obj_begin(const void* pObj);
extern "C" void port_cc_obj_end(int64_t t0);
#if PORT_LOGGING
// GXV_ANIM_PROF (platform/src/port/perf.cpp): every object of a list timed, when asked for (one test of a constant otherwise).
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_begin(void);
extern "C" void port_anim_prof_end(const char* objName);
#define PORT_ANIM_PROF_CALL(call, obj)        \
    do {                                      \
        if (animProfOn) {                     \
            port_anim_prof_begin();           \
            call(obj);                        \
            port_anim_prof_end((obj)->mName); \
        } else {                              \
            call(obj);                        \
        }                                     \
    } while (0)
#endif
#endif

NameObjCategoryList::NameObjCategoryList(u32 count, const CategoryListInitialTable* pTable, NameObjMethod pMethod, bool allocateByCheck,
                                         const char*) {
    NameObjMethod method;
    method = pMethod;
    mDelegator = new NameObjRealDelegator< NameObjMethod >(method);
    mAllocateByCheck = allocateByCheck;
    mBufferAllocated = false;
    initTable(count, pTable);
}

NameObjCategoryList::NameObjCategoryList(u32 count, const CategoryListInitialTable* pTable, NameObjMethodConst pMethod, bool allocateByCheck,
                                         const char*) {
    NameObjMethodConst method;
    method = pMethod;
    mDelegatorConst = new NameObjRealDelegator< NameObjMethodConst >(method);
    mAllocateByCheck = allocateByCheck;
    mBufferAllocated = false;
    initTable(count, pTable);
}

NameObjCategoryList::~NameObjCategoryList() {
    delete mDelegator;
}

void NameObjCategoryList::execute(int idx) {
    CategoryInfo* pCategoryInfo = &mCategoryInfo[idx];

    if (pCategoryInfo->mNameObjArr.size() == 0) {
        return;
    }

    if (pCategoryInfo->mExecuteBeforeFunction != nullptr) {
        (*pCategoryInfo->mExecuteBeforeFunction)();
    }

#ifdef TARGET_PC
    const bool prefetch = port_upd_prefetch_on() != 0;
    const bool prefetch2 = prefetch && port_upd_prefetch2_on() != 0 && PortListIsLiveActors(port_anim_lod_raw_category());
    const bool prefetch3 = prefetch && port_upd_prefetch3_on() != 0;  // (update26: three stages; the lists of LiveActors, movement or calcAnim)
    const bool actorList = prefetch3 && PortListIsActorList(port_anim_lod_raw_category());
#endif
#if defined(TARGET_PC) && PORT_LOGGING
    const bool animProfOn = ::port::gt71::AnimProfOn();  // (gt71 PROF: asked once a list, not by a call an object; the switch does not change inside a list)
#endif
    for (NameObj** pNameObj = pCategoryInfo->mNameObjArr.begin(); pNameObj != pCategoryInfo->mNameObjArr.end(); pNameObj++) {
#ifdef TARGET_PC
        if (prefetch3) {
            NameObj* const* const pEnd = pCategoryInfo->mNameObjArr.end();
            if (NameObj* const* pAhead = ::port::upd26::Ahead(pNameObj, ::port::upd26::kStageLines, pEnd)) {
                PortListPrefetch(pAhead, pEnd);  // (GXV_UPD_PREFETCH3: the object's own lines, one step earlier than PREFETCH's)
            }
            if (actorList) {
                if (NameObj* const* pAhead = ::port::upd26::Ahead(pNameObj, ::port::upd26::kStageParts, pEnd)) {
                    PortListPrefetch2(pAhead, pEnd);  // (the parts' first lines, one step earlier)
                }
                if (NameObj* const* pAhead = ::port::upd26::Ahead(pNameObj, ::port::upd26::kStageBlocks, pEnd)) {
                    PortListPrefetch3(pAhead, pEnd);  // (the blocks behind the parts)
                }
            }
        } else if (prefetch) {
            PortListPrefetch(pNameObj + 2, pCategoryInfo->mNameObjArr.end());  // (GXV_UPD_PREFETCH; the end read as the loop does)
            if (prefetch2) {
                PortListPrefetch2(pNameObj + 1, pCategoryInfo->mNameObjArr.end());  // (GXV_UPD_PREFETCH2: the parts of the next actor)
            }
        }
        if (__builtin_expect((port_cc_flag & 1) != 0, 0)) {  // (GXV_CLIPCENSUS; do not combine with GXV_LOGIC_PROF: its rows would skip these objects)
            NameObj* pObj = *pNameObj;
            const int64_t ccT0 = port_cc_obj_begin(pObj);
            (*mDelegator)(pObj);
            port_cc_obj_end(ccT0);
            continue;
        }
        if (::port::LogicProfOn()) {  // (GXV_LOGIC_PROF: the same call, timed under the object's class)
            NameObj* pObj = *pNameObj;
            port_logic_prof_obj_begin(pObj, pObj->mName);
            (*mDelegator)(pObj);
            port_logic_prof_obj_end();
            continue;
        }
#endif
#if defined(TARGET_PC) && PORT_LOGGING
        PORT_ANIM_PROF_CALL((*mDelegator), *pNameObj);
#else
        (*mDelegator)(*pNameObj);
#endif
    }
}

void NameObjCategoryList::incrementCheck(NameObj*, int index) {
    mCategoryInfo[index].mCheck++;
}

void NameObjCategoryList::allocateBuffer() {
    if (mAllocateByCheck) {
        for (int i = 0; i < mCategoryInfo.size(); i++) {
            NameObjCategoryList::CategoryInfo* pInfo = &mCategoryInfo[i];
            u32 size = pInfo->mCheck;
            NameObj** pNameObjArr = new NameObj*[size];
            MR::Vector< MR::AssignableArray< NameObj* > >* pArray = &mCategoryInfo[i].mNameObjArr;
            pArray->mArray.mArr = pNameObjArr;
            pArray->mArray.mMaxSize = size;
        }

        mBufferAllocated = true;
    }
}

void NameObjCategoryList::add(NameObj* pObj, int idx) {
    mCategoryInfo[idx].mNameObjArr.push_back(pObj);
}

void NameObjCategoryList::remove(NameObj* pObj, int idx) {
    MR::Vector< MR::AssignableArray< NameObj* > >& rArray = mCategoryInfo[idx].mNameObjArr;
    s32 size = rArray.size();
    NameObj** pBegin = rArray.begin();
    NameObj** pFound = std::find(pBegin, pBegin + size, pObj);
    rArray[pFound - pBegin] = rArray[rArray.size() - 1];
    rArray.pop_back();
}

void NameObjCategoryList::registerExecuteBeforeFunction(const MR::FunctorBase& rFunc, int idx) {
    NameObjCategoryList::CategoryInfo* pCategoryInfo = &mCategoryInfo[idx];

    pCategoryInfo->mExecuteBeforeFunction = rFunc.clone(nullptr);
}

void NameObjCategoryList::initTable(u32 count, const CategoryListInitialTable* pTable) {
    mCategoryInfo.init(count);

    for (CategoryInfo* pCategoryInfo = mCategoryInfo.begin(); pCategoryInfo != mCategoryInfo.end(); pCategoryInfo++) {
        pCategoryInfo->mExecuteBeforeFunction = nullptr;
    }

    for (const CategoryListInitialTable* pEntry = &pTable[0]; pEntry->mIndex != -1; pEntry++) {
        if (!mAllocateByCheck) {
            u32 size = pEntry->mCount;
            NameObj** pArray = new NameObj*[size];
            NameObjCategoryList::CategoryInfo* pInfo = &mCategoryInfo[pEntry->mIndex];
            pInfo->mNameObjArr.mArray.mArr = pArray;
            pInfo->mNameObjArr.mArray.mMaxSize = size;
            mBufferAllocated = true;
        }

        mCategoryInfo[pEntry->mIndex].mCheck = 0;
    }
}

NameObjCategoryList::CategoryInfo::CategoryInfo() : mNameObjArr() {
}

NameObjCategoryList::CategoryInfo::~CategoryInfo() {
}
