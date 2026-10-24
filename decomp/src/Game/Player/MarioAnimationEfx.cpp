#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Util/HashUtil.hpp"

struct MarioAnimationCallback {
    const char* mAnimation;
    s32 mType;
    void (MarioAnimator::*mEntry)();
    void (MarioAnimator::*mUpdate)();
    void (MarioAnimator::*mClose)();
    u32 _2C;
};

MarioAnimationCallback marioCallbackTable[] = {
    {"\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8", 0, &MarioAnimator::spinEntry, &MarioAnimator::spinUpdate, &MarioAnimator::spinClose, 0},
    {"\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8", 0, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8", 1, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x90\xc3\x8e\x7e", 1, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93", 2, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86", 2, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93", 3, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86", 3, &MarioAnimator::spinEntry, nullptr, &MarioAnimator::spinClose, 0},
    {"\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""A", 0, nullptr, &MarioAnimator::stageInCheck, nullptr, 0},
    {"\x93\x8a\x82\xb0", 0, &MarioAnimator::throwEntry, &MarioAnimator::throwCheck, &MarioAnimator::throwClose, 0},
    {"\x83\x74\x83\x40\x83\x43\x83\x41\x93\x8a\x82\xb0", 1, &MarioAnimator::throwEntry, nullptr, &MarioAnimator::throwClose, 0},
    {"\x83\x54\x83\x7d\x81\x5b\x83\x5c\x83\x8b\x83\x67", 0, nullptr, &MarioAnimator::squatSpinCheck, nullptr, 0},
    {"\x83\x45\x83\x48\x81\x5b\x83\x4e\x83\x43\x83\x93", 0, nullptr, nullptr, &MarioAnimator::walkinClose, 0},
    {"\x8c\xa9\x82\xe9", 0, nullptr, nullptr, &MarioAnimator::walkinClose, 0},
    {"ResultWait", 0, nullptr, nullptr, &MarioAnimator::walkinClose, 0},
    {"ResultWaitGrandStar", 0, nullptr, nullptr, &MarioAnimator::walkinClose, 0},
    {"WatchUpMore", 0, nullptr, nullptr, &MarioAnimator::walkinClose, 0},
    {"", 0, nullptr, nullptr, nullptr, 0},
};

void MarioAnimator::initCallbackTable() {
    u32 count = 0;
    MarioAnimationCallback* callback = marioCallbackTable;
    for (;; count++, callback++) {
        if (!*callback->mAnimation) {
            break;
        }
    }

    mCallbackTable = new HashSortTable(count);
    callback = marioCallbackTable;
    for (u32 i = 0; i < count; callback++, i++) {
        mCallbackTable->add(callback->mAnimation, i, false);
    }

    mCallbackTable->sort();
    mCallbackId = -1;
    mCallbackEnded = false;
}

void MarioAnimator::entryCallback(const char* pName) {
    mCallbackEnded = false;
    closeCallback();
    u32 index;
    if (mCallbackTable->search(pName, &index)) {
        mCallbackId = index;
        if (marioCallbackTable[mCallbackId].mEntry != nullptr) {
            (this->*marioCallbackTable[mCallbackId].mEntry)();
        }
    }
}

void MarioAnimator::runningCallback() {
    if (mCallbackId == -1) {
        return;
    }

    mCallbackEnded = true;
    if (isAnimationStop() || isAnimationTerminate(nullptr)) {
        closeCallback();
        return;
    }

    mCallbackEnded = false;
    if (!isAnimationRun(marioCallbackTable[mCallbackId].mAnimation)) {
        closeCallback();
        return;
    }

    if (marioCallbackTable[mCallbackId].mUpdate != nullptr) {
        (this->*marioCallbackTable[mCallbackId].mUpdate)();
    }
}

void MarioAnimator::closeCallback() {
    if (mCallbackId != -1 && marioCallbackTable[mCallbackId].mClose != nullptr) {
        (this->*marioCallbackTable[mCallbackId].mClose)();
    }

    mCallbackId = -1;
}

void MarioAnimator::spinEntry() {
    switch (marioCallbackTable[mCallbackId].mType) {
    case 0:
        playEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
        break;
    case 1:
        playEffect("\x83\x41\x83\x43\x83\x58\x83\x58\x83\x73\x83\x93");
        break;
    case 2:
        playEffect("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93");
        break;
    case 3:
        if (gIsLuigi) {
            playEffect("\x83\x6e\x83\x60\x83\x8b\x83\x43\x81\x5b\x83\x57\x83\x58\x83\x73\x83\x93");
        } else {
            playEffect("\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93");
        }

        break;
    }
}

void MarioAnimator::spinUpdate() {
    if (getFrame() > 30.0f) {
        stopEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
    }
}

void MarioAnimator::spinClose() {
    switch (marioCallbackTable[mCallbackId].mType) {
    case 0:
        stopEffect("\x83\x58\x83\x73\x83\x93\x83\x89\x83\x43\x83\x67");
        break;
    case 1:
        stopEffect("\x83\x41\x83\x43\x83\x58\x83\x58\x83\x73\x83\x93");
        break;
    case 2:
        stopEffect("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93");
        break;
    case 3:
        if (gIsLuigi) {
            stopEffect("\x83\x6e\x83\x60\x83\x8b\x83\x43\x81\x5b\x83\x57\x83\x58\x83\x73\x83\x93");
        } else {
            stopEffect("\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93");
        }

        break;
    }
}

void MarioAnimator::stageInCheck() {
    if (static_cast< s32 >(getFrame()) == 50) {
        Mario* player = getPlayer();
        playEffectRT("\x91\xae\x90\xab\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93", player->_368, getTrans());
    }
}

void MarioAnimator::throwCheck() {
    if (mActor->_38C == 0 && getStickP() && !getPlayer()->mMovementStates.jumping) {
        stopAnimation(nullptr);
    }
}

void MarioAnimator::throwEntry() {
    switch (marioCallbackTable[mCallbackId].mType) {
    case 0:
        playEffect("\x82\xb1\x82\xa4\x82\xe7\x93\x8a\x82\xb0");
        break;
    case 1:
        playEffect("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x7b\x81\x5b\x83\x8b\x93\x8a\x82\xb0");
        break;
    }
}

void MarioAnimator::throwClose() {
    switch (marioCallbackTable[mCallbackId].mType) {
    case 0:
        stopEffect("\x82\xb1\x82\xa4\x82\xe7\x93\x8a\x82\xb0");
        break;
    case 1:
        stopEffect("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x7b\x81\x5b\x83\x8b\x93\x8a\x82\xb0");
        break;
    }
}

void MarioAnimator::squatSpinCheck() {
    if (!getPlayer()->mMovementStates._A && getFrame() >= 40.0f) {
        stopAnimation(nullptr);
    }
}

void MarioAnimator::walkinClose() {
    if (mCallbackEnded) {
        stopAnimation(nullptr);
        getPlayer()->changeAnimationInterpoleFrame(16);
    }
}
