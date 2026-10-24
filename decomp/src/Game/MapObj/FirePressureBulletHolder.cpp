#include "Game/MapObj/FirePressureBulletHolder.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/MapObj/FirePressureBullet.hpp"
#include "Game/Util.hpp"
#include "Game/Util/LiveActorUtil.hpp"

FirePressureBulletHolder::FirePressureBulletHolder(const char* pName) : NameObj(pName) {
}

FirePressureBullet* FirePressureBulletHolder::callEmptyBullet() {
    for (u32 i = 0; i < ARRAY_SIZE(mBullet); i++) {
        if (MR::isDead(mBullet[i])) {
            return mBullet[i];
        }
    }

    return nullptr;
}

FirePressureBulletHolder::~FirePressureBulletHolder() {
}

void FirePressureBulletHolder::init(const JMapInfoIter& rIter) {
    for (u32 i = 0; i < ARRAY_SIZE(mBullet); i++) {
        mBullet[i] = new FirePressureBullet("\x83\x74\x83\x40\x83\x43\x83\x41\x83\x76\x83\x8c\x83\x62\x83\x56\x83\x83\x81\x5b\x82\xcc\x92\x65");
        mBullet[i]->initWithoutIter();
    }
}
