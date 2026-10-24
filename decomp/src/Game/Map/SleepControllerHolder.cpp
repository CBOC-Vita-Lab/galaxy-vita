#include "Game/Map/SleepControllerHolder.hpp"
#include "Game/AreaObj/AreaObj.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/SleepController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SwitchEventFunctorListener.hpp"
#include <algorithm>

namespace {
    SleepControllerHolder* getSleepControllerHolder() {
        return MR::getSceneObj< SleepControllerHolder >(SceneObj_SleepControllerHolder);
    }
};  // namespace

SleepControllerHolder::SleepControllerHolder() : NameObj("\x83\x49\x83\x75\x83\x57\x83\x46\x83\x58\x83\x8a\x81\x5b\x83\x76\x8a\xc7\x97\x9d"), mSleepControl() {
    MR::connectToSceneMapObjMovement(this);
}

void SleepControllerHolder::movement() {
    std::for_each(mSleepControl.begin(), mSleepControl.end(), std::mem_func(&SleepController::update));
}

void SleepControllerHolder::add(NameObj* pObj, const JMapInfoIter& rIter, SwitchEventListener* pListener) {
    SleepController* sleepControl = new SleepController(rIter, pListener);

    mSleepControl.push_back(sleepControl);
}

void SleepControllerHolder::initSync() {
    std::for_each(mSleepControl.begin(), mSleepControl.end(), std::mem_func(&SleepController::initSync));
}

namespace SleepControlFunc {
    void initSyncSleepController() {
        ::getSleepControllerHolder()->initSync();
    }

    void addSleepControl(AreaObj* pObj, const JMapInfoIter& rIter) {
        SwitchEventFunctorListener* listener;

        if (!MR::isExistStageSwitchSleep(rIter)) {
            return;
        }

        listener = new SwitchEventFunctorListener();
        listener->setOnFunctor(MR::Functor(pObj, &AreaObj::awake));
        listener->setOffFunctor(MR::Functor(pObj, &AreaObj::sleep));

        ::getSleepControllerHolder()->add(pObj, rIter, listener);
    }

    void addSleepControl(LiveActor* pActor, const JMapInfoIter& rIter) {
        SwitchEventFunctorListener* listener;

        if (!MR::isExistStageSwitchSleep(rIter)) {
            return;
        }

        if (MR::isExistStageSwitchAppear(rIter)) {
        }

        listener = new SwitchEventFunctorListener();
        listener->setOnFunctor(MR::Functor(pActor, &LiveActor::makeActorAppeared));
        listener->setOffFunctor(MR::Functor(pActor, &LiveActor::makeActorDead));

        ::getSleepControllerHolder()->add(pActor, rIter, listener);
    }
};  // namespace SleepControlFunc
