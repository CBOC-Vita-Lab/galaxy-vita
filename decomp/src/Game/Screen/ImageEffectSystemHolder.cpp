#include "Game/Screen/ImageEffectSystemHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/ImageEffectDirector.hpp"
#include "Game/Screen/ImageEffectResource.hpp"
#include "Game/Util/ObjUtil.hpp"

ImageEffectSystemHolder::ImageEffectSystemHolder() : NameObj("\x89\xe6\x91\x9c\x8c\xf8\x89\xca\x8a\xc7\x97\x9d") {
    mResource = new ImageEffectResource();
    mDirector = nullptr;
    mDirector = new ImageEffectDirector("\x91\x53\x89\xe6\x96\xca\x83\x47\x83\x74\x83\x46\x83\x4e\x83\x67\x8a\xc7\x97\x9d");
}

void ImageEffectSystemHolder::pauseOff() {
    if (mDirector != nullptr) {
        MR::requestMovementOn(mDirector);
    }
}

namespace MR {
    void createImageEffectSystemHolder() {
        createSceneObj(SceneObj_ImageEffectSystemHolder);
    }

    ImageEffectSystemHolder* getImageEffectSystemHolder() {
        return getSceneObj< ImageEffectSystemHolder >(SceneObj_ImageEffectSystemHolder);
    }

    bool isExistImageEffectDirector() {
        if (isExistSceneObj(SceneObj_ImageEffectSystemHolder)) {
            return getImageEffectDirector() != nullptr;
        }

        return false;
    }

    ImageEffectDirector* getImageEffectDirector() {
        return getImageEffectSystemHolder()->mDirector;
    }

    ImageEffectResource* getImageEffectResource() {
        return getImageEffectSystemHolder()->mResource;
    }
};  // namespace MR