#include "Game/Map/LightDataHolder.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Util.hpp"

#ifdef TARGET_PC
// The scene-start parse below is ~60 ms of CSV lookups with no allocation in it: a safe point per entry keeps the audio interrupts
// (and with them the music) going during a stage load. (platform/src/os/os_thread.cpp)
extern "C" void port_irq_poll(void);
#endif

namespace {
    static const char* sDefaultAreaLightName = "\x83\x66\x83\x74\x83\x48\x83\x8b\x83\x67";

    static LightInfoCoin sDefaultLightSetCoin = {{{0xFF, 0xFF, 0, 0}, {0.0f, 0.0f, 0.0f}, true}, {0xFF, 0xFF, 0xFF, 0, 65.0f}};
};  // namespace

LightDataHolder::LightDataHolder() {
    mLightCount = 0;
    mLights = 0;
}

void LightDataHolder::initLightData() {
    JMapInfo* data = NULL;
    mLightCount = LightFunction::createLightDataParser(&data);
    mLights = new AreaLightInfo[mLightCount];

    for (s32 i = 0; i < mLightCount; i++) {
        LightFunction::getAreaLightLightData(data, i, &mLights[i]);
#ifdef TARGET_PC
        port_irq_poll();
#endif
    }

    _8 = ::sDefaultLightSetCoin;
}

AreaLightInfo* LightDataHolder::findAreaLight(const char* pName) const {
    for (s32 i = 0; i < mLightCount; i++) {
        AreaLightInfo* inf = getLightInfo(i);

        if (MR::isEqualString(pName, inf->mAreaLightName)) {
            return inf;
        }
    }

    return mLights;
}

const char* LightDataHolder::getDefaultAreaLightName() const {
    return ::sDefaultAreaLightName;
}

s32 LightDataHolder::getDefaultStepInterpolate() const {
    return 0x1E;
}
