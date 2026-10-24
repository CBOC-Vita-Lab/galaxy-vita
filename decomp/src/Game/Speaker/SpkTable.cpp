#include "Game/Speaker/SpkTable.hpp"

SpkTable::SpkTable() {
    mInitialized = false;
    mResourceCount = 0;
    mParameters = nullptr;
    mNames = nullptr;
}

void SpkTable::setResource(void* pRes) {
    mInitialized = false;

    s32* cursor = (s32*)pRes;

    s32 resourceCount = *cursor++;
    s32 entryOff = *cursor++;
    s32 dataOffsetsStartOff = *cursor++;
    s32* pIsDataOffsetsInitialized = cursor;
    BOOL isDataOffsetsInitialized = *cursor++;

    mResourceCount = resourceCount;

    SpkParameters* entryOffset = (SpkParameters*)((u8*)pRes + entryOff);
    mParameters = entryOffset;
    u32* names = (u32*)((u8*)pRes + dataOffsetsStartOff);  // 4-byte name slots in the file
    if (!isDataOffsetsInitialized) {
        for (s32 i = 0; i < mResourceCount; i++) {
            names[i] += (u32)(uintptr_t)pRes;
        }
    }

    mNames = (PTR32(const char)*)names;
    *pIsDataOffsetsInitialized = TRUE;
    mInitialized = true;
}
