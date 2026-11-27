#include "Game/System/GameDataPlayerStatus.hpp"
#include "Game/Util/MathUtil.hpp"
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <JSystem/JSupport/JSUMemoryOutputStream.hpp>
#ifdef TARGET_PC
#include <cstdlib>
#endif

GameDataPlayerStatus::GameDataPlayerStatus() {
    initializeData();
}

u32 GameDataPlayerStatus::makeHeaderHashCode() const {
    return 0x27C90F;
}

u32 GameDataPlayerStatus::getSignature() const {
    return 'PLAY';
}

s32 GameDataPlayerStatus::serialize(u8* pBuffer, u32 size) const {
    JSUMemoryOutputStream stream = JSUMemoryOutputStream(pBuffer, size);

    u8 storyProgress = mStoryProgress;
    stream.write(&storyProgress, sizeof(mStoryProgress));

    u32 stockedStarPiece = mStockedStarPiece;
    stream.write(&stockedStarPiece, sizeof(mStockedStarPiece));

    u16 playerLeft = mPlayerLeft;
    stream.write(&playerLeft, sizeof(mPlayerLeft));

    return stream.mPosition;
}

s32 GameDataPlayerStatus::deserialize(const u8* pBuffer, u32 size) {
    initializeData();

    JSUMemoryInputStream stream = JSUMemoryInputStream(pBuffer, size);

    stream.read(&mStoryProgress, sizeof(mStoryProgress));
    stream.read(&mStockedStarPiece, sizeof(mStockedStarPiece));

    if (stream.getAvailable() != 0) {
        stream.read(&mPlayerLeftSupply, sizeof(mPlayerLeftSupply));
    }

    mPlayerLeft = 4;

    return 0;
}

void GameDataPlayerStatus::initializeData() {
    mPlayerLeft = 4;
    mPlayerLeftSupply = 4;
    mStockedStarPiece = 0;
    mStoryProgress = 0;
}

s32 GameDataPlayerStatus::getPlayerLeft() const {
    s32 playerLeft = mPlayerLeft;
#ifdef TARGET_PC
    // PETARI_TEST_LIVES=<n> (test routing only, default unset): the lives count reads as n, so a route reaches the Game Over screen at its first miss
    // (14 Nov: the owner's bottom-strip flicker on that screen; the routes reached it only by chance).
    static const s32 sTestLives = [] { const char* e = std::getenv("PETARI_TEST_LIVES"); return e && *e ? static_cast<s32>(std::atoi(e)) : -1; }();
    if (sTestLives >= 0) playerLeft = sTestLives;
#endif

    return MR::clamp(playerLeft, 0, 99);
}

void GameDataPlayerStatus::addPlayerLeft(int num) {
    s32 playerLeft = mPlayerLeft + num;

    mPlayerLeft = MR::clamp(playerLeft, 0, 99);
}

bool GameDataPlayerStatus::isPlayerLeftSupply() const {
    return mPlayerLeftSupply >= 10;
}

void GameDataPlayerStatus::offPlayerLeftSupply() {
    mPlayerLeftSupply = 0;
}

void GameDataPlayerStatus::addStockedStarPiece(int num) {
    s32 stockedStarPiece = mStockedStarPiece + num;

    mStockedStarPiece = MR::clamp(stockedStarPiece, 0, 9999);
}
