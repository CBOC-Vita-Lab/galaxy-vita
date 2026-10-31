// Stand-in for the game's WPad class, for tests/spin_detect.cpp: just what the swing detector (WPadHVSwing) and the acceleration
// history (WPadAcceleration) of the decompilation ask of it.  The real WPad owns more (buttons, pointer, rumble...); both
// detector sources are compiled unchanged against this.
#pragma once

#include <JSystem/JGeometry/TVec.hpp>
#include <revolution/kpad.h>
#include <revolution/types.h>
#include <revolution/wpad.h>

class WPadAcceleration;
struct KPADStatus;

class WPad {
public:
    KPADStatus* getKPadStatus(u32) const;
    s32 getValidStatusCount() const;
    s32 getEnableAccelPastCount(u32) const;
    bool getAcceleration(TVec3f*, u32) const;
    bool getPastAcceleration(TVec3f*, s32, u32) const;

    s32 mChannel = 0;
    WPadAcceleration* mCorePadAccel = nullptr;
    WPadAcceleration* mSubPadAccel = nullptr;
    KPADStatus mStatus[120] = {};
    s32 mValid = 0;
};

namespace MR {
    bool isDeviceFreeStyle(const KPADStatus*);
}
