#include "Game/System/GameSequenceDirector.hpp"
#include "Game/MP/MpGame.hpp"
#include "Game/NWC24/NWC24Messenger.hpp"
#include "Game/System/FindingLuigiEventScheduler.hpp"
#include "Game/System/GameDataTemporaryInGalaxy.hpp"
#include "Game/System/GameSequenceProgress.hpp"
#include "Game/System/SaveDataHandleSequence.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>

GameSequenceDirector::GameSequenceDirector()
    : mGameDataTemporaryInGalaxy(nullptr), mGameSequenceProgress(nullptr), mSaveDataHandleSequence(nullptr), mNWC24Messenger(nullptr) {
    mSaveDataHandleSequence = new SaveDataHandleSequence();
    mSaveDataHandleSequence->registerFunctorOnSaveSuccess(MR::Functor(this, &GameSequenceDirector::executeOnSaveSuccess));
    mSaveDataHandleSequence->registerFunctorJustBeforeSave(MR::Functor(this, &GameSequenceDirector::executeJustBeforeSave));

    mGameSequenceProgress = new GameSequenceProgress();
    mGameDataTemporaryInGalaxy = new GameDataTemporaryInGalaxy();
    mNWC24Messenger = new NWC24Messenger("NWC24Messenger");
}

void GameSequenceDirector::initAfterResourceLoaded() {
    mSaveDataHandleSequence->initAfterResourceLoaded();
    mGameSequenceProgress->initAfterResourceLoaded();
    mNWC24Messenger->initAfterResourceLoaded();
    // MP3: the multiplayer menus, windows and toasts (system level, every scene); nothing with SMG_MP_MENU=0
    MpUi::createSystem();
}

void GameSequenceDirector::update() {
    mSaveDataHandleSequence->update();
    mGameSequenceProgress->update();
    mNWC24Messenger->movement();
    mNWC24Messenger->calcAnim();
    // MP3: multiplayer menus (a no-op until created; idle and cheap while no menu, window or session is up)
    MpUi::updateSystem();
}

void GameSequenceDirector::draw() const {
    mSaveDataHandleSequence->draw();
    mGameSequenceProgress->draw();
    mNWC24Messenger->draw();
    // MP3: multiplayer menus, over the scene and under the system windows drawn after the director
    MpUi::drawSystem();
}

bool GameSequenceDirector::isInitializedGameDataHolder() const {
    return mSaveDataHandleSequence->isInitializedGameDataHolder();
}

GameDataHolder* GameSequenceDirector::getGameDataHolder() {
    return mSaveDataHandleSequence->getHolder();
}

void GameSequenceDirector::executeOnSaveSuccess() {
    mGameSequenceProgress->mFindingLuigiEventScheduler->sendMail();
}

void GameSequenceDirector::executeJustBeforeSave() {
    mGameSequenceProgress->mFindingLuigiEventScheduler->writeSendSize();
}
