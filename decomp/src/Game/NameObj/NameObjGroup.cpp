#include "Game/NameObj/NameObjGroup.hpp"
#include "Game/Util/ObjUtil.hpp"

#ifdef TARGET_PC
// GXV_HOTPROF_LIVESNAP (hotprof25; LiveActor.cpp port_live_actor_snapshot): moves at every write of any group's array. The array is
// private and written in exactly two places, both below (initObjArray makes it, registerObj appends), so "this count has not moved"
// proves "no group's entries or number changed, and no group was made" since it was read.
unsigned gPortNameObjGroupEpoch = 1;
#endif

NameObjGroup::NameObjGroup(const char* pName, int numMax) : NameObj(pName), mObjNumMax(), mObjNum(), mObjArray() {
    initObjArray(numMax);
}

NameObjGroup::~NameObjGroup() {
    delete[] mObjArray;
}

void NameObjGroup::registerObj(NameObj* pObj) {
#ifdef TARGET_PC
    gPortNameObjGroupEpoch++;  // (GXV_HOTPROF_LIVESNAP, above)
#endif
    mObjArray[mObjNum] = pObj;
    mObjNum++;
}

void NameObjGroup::pauseOffAll() const {
    for (s32 i = 0; i < mObjNum; i++) {
        MR::requestMovementOn(mObjArray[i]);
    }
}

void NameObjGroup::initObjArray(int numMax) {
#ifdef TARGET_PC
    gPortNameObjGroupEpoch++;  // (GXV_HOTPROF_LIVESNAP, above)
#endif
    mObjNumMax = numMax;
    mObjArray = new NameObj*[numMax];

    for (s32 i = 0; i < mObjNumMax; i++) {
        mObjArray[i] = nullptr;
    }
}
