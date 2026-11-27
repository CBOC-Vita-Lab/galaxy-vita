#include "Game/Util/LightUtil.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/LightDirector.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Map/LightPointCtrl.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#ifdef TARGET_PC
#include "Game/System/PortDrawPipe.hpp"
namespace {
    // GXV_DRAWPIPE (dpengine; Game/System/PortDrawPipe.hpp): the player's light and the point light in a pass drawn from a frozen
    // frame: what LightDirector::loadLightPlayer loads (the player's light loader, then the point light), from the copies made at
    // the hand-over. The next update moves both (ActorLightCtrl::update, LightPointCtrl::update).
    bool portLoadFrozenPlayerLight() {
        const PortPipeScene* scene = gPortPipeScene;
        if (scene == nullptr || !scene->mLights) {
            return false;
        }
        if (scene->mPlayerLight) {
            LightFunction::loadActorLightInfo(&scene->mPlayerLightInfo);
        }
        LightFunction::loadPointLightInfo(&scene->mPointLight);
        return true;
    }
}  // namespace
#endif

namespace MR {
    void loadLight(s32 type) {
#ifdef TARGET_PC
        if (type == MR::LightType_Player && portLoadFrozenPlayerLight()) {
            return;
        }
#endif
        if (type == MR::LightType_Coin) {
            MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->loadLightCoin();
            return;
        }

        if (type == MR::LightType_Player) {
            MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->loadLightPlayer();
            return;
        }

        AreaLightInfo* inf = MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mDefaultAreaLight;

        if (inf != nullptr) {
            if (type == MR::LightType_Strong) {
                LightFunction::loadActorLightInfo(&inf->mStrongLight);
            } else if (type == MR::LightType_Weak) {
                LightFunction::loadActorLightInfo(&inf->mWeakLight);
            } else if (type == MR::LightType_Planet) {
                LightFunction::loadActorLightInfo(&inf->mPlanetLight);
            }
        }
    }

    void loadLightPlayer() {
#ifdef TARGET_PC
        if (portLoadFrozenPlayerLight()) {
            return;
        }
#endif
        MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->loadLightPlayer();
    }

    void initActorLightInfoLightType(LiveActor* pActor, s32 a2) {
        pActor->mActorLightCtrl->_4 = a2;
    }

    void initActorLightInfoDrawBuffer(LiveActor* pActor, DrawBuffer* pDrawBuffer) {
        pActor->mActorLightCtrl->_8 = pDrawBuffer;
    }

    void requestPointLight(const LiveActor* pActor, TVec3f pos, Color8 color, f32 intensity, s32 duration) {
        return MR::getSceneObj< LightDirector >(SceneObj_LightDirector)->mPointCtrl->requestPointLight(pActor, pos, color, intensity, duration);
    }
};  // namespace MR
