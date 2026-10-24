#include "Game/System/GameEventValueChecker.hpp"
#include "Game/System/FindingLuigiEventScheduler.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <JSystem/JSupport/JSUMemoryOutputStream.hpp>

namespace {
    const GameEventValue cGameEventValueTable[] = {
        {"\x83\x79\x83\x93\x83\x4d\x83\x93\x83\x8c\x81\x5b\x83\x58[\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x8a\x83\x93\x83\x4f]/hi", 0},
        {"\x83\x79\x83\x93\x83\x4d\x83\x93\x83\x8c\x81\x5b\x83\x58[\x83\x49\x81\x5b\x83\x56\x83\x83\x83\x93\x83\x8a\x83\x93\x83\x4f]/lo", 90 * 60},
        {"\x83\x65\x83\x8c\x83\x54\x83\x8c\x81\x5b\x83\x58[\x83\x74\x83\x40\x83\x93\x83\x67\x83\x80]/hi", 0},
        {"\x83\x65\x83\x8c\x83\x54\x83\x8c\x81\x5b\x83\x58[\x83\x74\x83\x40\x83\x93\x83\x67\x83\x80]/lo", 90 * 60},
        {"\x83\x65\x83\x8c\x83\x54\x83\x8c\x81\x5b\x83\x58[\x83\x66\x83\x58\x83\x76\x83\x8d\x83\x80\x83\x69\x81\x5b\x83\x68]/hi", 0},
        {"\x83\x65\x83\x8c\x83\x54\x83\x8c\x81\x5b\x83\x58[\x83\x66\x83\x58\x83\x76\x83\x8d\x83\x80\x83\x69\x81\x5b\x83\x68]/lo", 90 * 60},
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93[\x83\x67\x83\x89\x83\x43\x83\x41\x83\x8b]/hi", 0},
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93[\x83\x67\x83\x89\x83\x43\x83\x41\x83\x8b]/lo", 90 * 60},
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93[\x83\x60\x83\x83\x83\x8c\x83\x93\x83\x57]/hi", 0},
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93[\x83\x60\x83\x83\x83\x8c\x83\x93\x83\x57]/lo", 90 * 60},
        {"LibraryOpenNewStarCount", 1},
        {"\x8a\x47\x96\x7b\x8a\xf9\x93\xc7\x8f\xcd", 0},
        {"MsgLedPattern", 1},
        {"LuigiEventState", FindingLuigiEventScheduler::STATE_NULL},
        {"WarpPodSaveBits", 0},
        {"TicoGalaxyAlreadyTalk", 0},
        {"MessageAlreadyRead", 0},
        {"MissPointForLetter", 0},
        {"MissNum", 0},
        {"Comet1Status", 0},
        {"Comet2Status", 0},
        {"Comet3Status", 0},
        {"Comet4Status", 0},
        {"Comet5Status", 0},
        {"Comet6Status", 0},
    };
}  // namespace

GameEventValueChecker::GameEventValueChecker() : mValues(), mNumValues() {
    mValues = new u16[ARRAY_SIZE(::cGameEventValueTable)];
    mNumValues = ARRAY_SIZE(::cGameEventValueTable);
    initializeData();
}

u32 GameEventValueChecker::getValue(const char* pName) const {
    return mValues[findIndex(pName)];
}

void GameEventValueChecker::setValue(const char* pName, u16 value) {
    mValues[findIndex(pName)] = value;
}

u32 GameEventValueChecker::makeHeaderHashCode() const {
    return getSignature();
}

u32 GameEventValueChecker::getSignature() const {
    return 'VLE1';
}

s32 GameEventValueChecker::serialize(u8* pData, u32 maxBufferSize) const {
    JSUMemoryOutputStream stream(pData, maxBufferSize);

    s32 hash;

    for (s32 idx = 0; idx < mNumValues; idx++) {
        hash = MR::getHashCode(::cGameEventValueTable[idx].mName);
        const u16 value = mValues[idx];

        stream.writeU16(hash);
        stream.writeU16(value);
    }

    return stream.mPosition;
}

s32 GameEventValueChecker::deserialize(const u8* pData, u32 maxBufferSize) {
    s32 readError = 0;

    JSUMemoryInputStream stream(pData, maxBufferSize);

    s32 numEntries = static_cast< s32 >(maxBufferSize) / 2;

    for (s32 idx = 0; idx < numEntries; idx++) {
        u16 hash = stream.readU16();
        u16 value = stream.readU16();

        s32 valueIndex = findIndexFromHashCode(hash);

        if (valueIndex >= 0) {
            mValues[valueIndex] = value;
        } else {
            readError = 1;
        }
    }

    if (stream.mState != JSUIosBase::IO_OK && stream.getState(JSUIosBase::IO_MEMORY_ERROR) == 0) {
        stream.mState = JSUIosBase::IO_OK;
    }

    return readError ? 1 : 0;
}

void GameEventValueChecker::initializeData() {
    for (s32 idx = 0; idx < mNumValues; idx++) {
        mValues[idx] = ::cGameEventValueTable[idx].mDefaultValue;
    }
}

s32 GameEventValueChecker::findIndex(const char* pName) const {
    for (s32 idx = 0; idx < mNumValues; idx++) {
        if (MR::isEqualString(pName, ::cGameEventValueTable[idx].mName)) {
            return idx;
        }
    }

    return -1;
}

s32 GameEventValueChecker::findIndexFromHashCode(u16 hash) const {
    for (s32 idx = 0; idx < mNumValues; idx++) {
        if (hash == static_cast< u16 >(MR::getHashCode(::cGameEventValueTable[idx].mName))) {
            return idx;
        }
    }

    return -1;
}
