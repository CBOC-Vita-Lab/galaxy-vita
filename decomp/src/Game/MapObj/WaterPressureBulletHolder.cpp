#include "Game/MapObj/WaterPressureBulletHolder.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/MapObj/WaterPressureBullet.hpp"
#include "Game/Util.hpp"
#include "Game/Util/LiveActorUtil.hpp"

WaterPressureBulletHolder::WaterPressureBulletHolder(const char* pName) : NameObj(pName) {
}

WaterPressureBullet* WaterPressureBulletHolder::callEmptyBullet() {
    for (u32 i = 0; i < ARRAY_SIZE(mBullet); i++) {
        if (MR::isDead(mBullet[i])) {
            return mBullet[i];
        }
    }

    return nullptr;
}

WaterPressureBulletHolder::~WaterPressureBulletHolder() {
}

void WaterPressureBulletHolder::init(const JMapInfoIter& rIter) {
    for (u32 i = 0; i < ARRAY_SIZE(mBullet); i++) {
        mBullet[i] = new WaterPressureBullet("\x83\x45\x83\x48\x81\x5b\x83\x5e\x81\x5b\x83\x76\x83\x8c\x83\x62\x83\x56\x83\x83\x81\x5b\x82\xcc\x92\x65");
        mBullet[i]->initWithoutIter();
    }
}