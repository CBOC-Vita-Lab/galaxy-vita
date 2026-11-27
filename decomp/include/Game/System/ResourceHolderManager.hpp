#pragma once

#include "Game/Util/Array.hpp"
#include <revolution/types.h>

class CreateResourceHolderArgs;
class JKRHeap;
class LayoutHolder;
class ResourceHolder;
class ResourceHolderManager;

typedef bool (*MakeArchiveFileNameFuncPtr)(char*, u32, const char*);
typedef void (*FuncPtrB)(const char*, CreateResourceHolderArgs*);
typedef void (ResourceHolderManager::*FuncPtrC)(const char*, CreateResourceHolderArgs*);

class CreateResourceHolderArgs {
public:
    /// @brief Creates a new `CreateResourceHolderArgs`.
    CreateResourceHolderArgs();

    /* 0x0 */ ResourceHolder* mResourceHolder;
    /* 0x4 */ LayoutHolder* mLayoutHolder;
    /* 0x8 */ JKRHeap* mHeap;
};

class ResourceHolderManagerName2Resource {
public:
    /// @brief Creates a new `ResourceHolderManagerName2Resource`.
    ResourceHolderManagerName2Resource();

    /// @brief Copies the contents from `other` to `this`.
    /// @param other A reference to the `ResourceHolderManagerName2Resource` to copy.
    /// @return A reference to `this`.
    ResourceHolderManagerName2Resource& operator=(const ResourceHolderManagerName2Resource& other);

    /* 0x0 */ ResourceHolder* mResourceHolder;
    /* 0x4 */ LayoutHolder* mLayoutHolder;
    /* 0x8 */ u32 mHash;
    /* 0xC */ JKRHeap* mHeap;
};

class ResourceHolderManager {
public:
    /// @brief Creates a new `ResourceHolderManager`.
    ResourceHolderManager();

    ResourceHolder* createAndAdd(const char*, JKRHeap*);
    ResourceHolder* createAndAddStationed(const char*);
    LayoutHolder* createAndAddLayoutHolder(const char*, JKRHeap*);
    LayoutHolder* createAndAddLayoutHolderStationed(const char*);
    LayoutHolder* createAndAddLayoutHolderRawData(const char*);
    void removeIfIsEqualHeap(JKRHeap*);
    static void startCreateResourceHolderOnMainThread(const char*, CreateResourceHolderArgs*);
    static void startCreateLayoutHolderOnMainThread(const char*, CreateResourceHolderArgs*);
#ifdef TARGET_PC
    // precomp26 (GXV_STAGE_ENUM): the holders loaded, in the order they were added (game thread; PortStageEnum.cpp)
    // (xcheck31, 13 Nov: the init thread adds holders while the frame thread walks them, and MR::Vector::push_back raises its count before it stores the
    // element, so a walker could read a slot not written yet. The walkers read a count PUBLISHED after the element is complete: ResourceHolderManager.cpp.)
    u32 portHolderCount() const;
    ResourceHolder* portHolderAt(u32 i) const { return i < portHolderCount() ? mResourceArray[static_cast< int >(i)].mResourceHolder : nullptr; }
#endif

private:
    ResourceHolderManagerName2Resource* createAndAddInner(const char*, MakeArchiveFileNameFuncPtr, FuncPtrB);
    ResourceHolderManagerName2Resource* createAndAddInnerStationed(const char*, FuncPtrC) NO_INLINE;
    void createResourceHolder(const char*, CreateResourceHolderArgs*);
    void createLayoutHolder(const char*, CreateResourceHolderArgs*);
    ResourceHolderManagerName2Resource* add(const char*, const CreateResourceHolderArgs&);
    ResourceHolderManagerName2Resource* find(const char*);

private:
    /* 0x0 */ MR::Vector< MR::FixedArray< ResourceHolderManagerName2Resource, 512 > > mResourceArray;
};
