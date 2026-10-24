#include "Game/Map/SwitchWatcher.hpp"
#include "Game/Map/ActorAppearSwitchListener.hpp"
#include "Game/Map/StageSwitch.hpp"

SwitchWatcher::SwitchWatcher(const StageSwitchCtrl* pSwitchCtrl)
    : NameObj("\x83\x58\x83\x43\x83\x62\x83\x60\x8a\xc4\x8e\x8b"), mFlags(), mSwitchCtrl(pSwitchCtrl), mSwitchListenerA(), mSwitchListenerB(), mSwitchListenerAppear() {
}

void SwitchWatcher::movement() {
    if (mSwitchListenerA != nullptr) {
        checkSwitch(mSwitchListenerA, 1, mSwitchCtrl->isOnSwitchA());
    }

    if (mSwitchListenerB != nullptr) {
        checkSwitch(mSwitchListenerB, 2, mSwitchCtrl->isOnSwitchB());
    }

    if (mSwitchListenerAppear != nullptr) {
        checkSwitch(mSwitchListenerAppear, 4, mSwitchCtrl->isOnSwitchAppear());
    }
}

void SwitchWatcher::checkSwitch(SwitchEventListener* pListener, u32 type, bool isOn) {
    if (isOn) {
        if ((mFlags & type) == 0) {
            pListener->listenSwitchOnEvent();
        }

        mFlags |= type;
    } else {
        if ((mFlags & type) != 0) {
            pListener->listenSwitchOffEvent();
        }

        mFlags &= ~type;
    }
}

bool SwitchWatcher::isSameSwitch(const StageSwitchCtrl* pSwitchCtrl) const {
    return mSwitchCtrl == pSwitchCtrl;
}

void SwitchWatcher::addSwitchListener(SwitchEventListener* pListener, u32 type) {
    switch (type) {
    case 1:
        mSwitchListenerA = pListener;
        break;
    case 2:
        mSwitchListenerB = pListener;
        break;
    case 4:
        mSwitchListenerAppear = pListener;
        break;
    }
}
