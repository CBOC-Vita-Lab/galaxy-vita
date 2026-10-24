#include "Game/Boss/BossAccessor.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Scene/SceneObjHolder.hpp"

namespace {
    BossAccessor* getBossAccessor() {
        return MR::getSceneObj< BossAccessor >(SceneObj_BossAccessor);
    }
};  // namespace

BossAccessor::BossAccessor() : NameObj("\x83\x7b\x83\x58\x82\xd6\x82\xcc\x83\x41\x83\x4e\x83\x5a\x83\x58"), mBoss() {
}

namespace BossAccess {
    void setBossAccessorKoopa(Koopa* pKoopa) {
        MR::createSceneObj(SceneObj_BossAccessor);
        MR::getSceneObj< BossAccessor >(SceneObj_BossAccessor)->setBoss(pKoopa);
    }

    Koopa* getBossAccessorKoopa() {
        if (MR::isExistSceneObj(SceneObj_BossAccessor)) {
            return static_cast< Koopa* >(::getBossAccessor()->getBoss());
        }

        return nullptr;
    }
};  // namespace BossAccess
