#include "Game/Map/SunshadeMapHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/AreaObjUtil.hpp"
#include "Game/Util/MapUtil.hpp"

void SunshadeMapHolder_FORCE_MATCH_SDATA2() {
    (void)1.0f;
}

SunshadeMapHolder::SunshadeMapHolder() : NameObj("\x93\xfa\x82\xe6\x82\xaf\x83\x52\x83\x8a\x83\x57\x83\x87\x83\x93\x8a\xc7\x97\x9d"), _C(0.0f, 1.0f, 0.0f) {
}

namespace MR {
    bool isInShadeFromTheSun(const TVec3f& rPos, f32 a2) {
        if (!MR::isInAreaObj("SunLightArea", rPos)) {
            return true;
        }

        if (!MR::isExistSceneObj(SceneObj_SunshadeMapHolder)) {
            return false;
        }

        SunshadeMapHolder* holder = MR::getSceneObj< SunshadeMapHolder >(SceneObj_SunshadeMapHolder);

        return Collision::checkStrikeLineToSunshade(rPos, holder->_C * a2, 0, nullptr, nullptr);
    }

    void createSunshadeMapHolder() {
        MR::createSceneObj(SceneObj_SunshadeMapHolder);
    }
};  // namespace MR
