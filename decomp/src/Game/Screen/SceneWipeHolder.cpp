#include "Game/Screen/SceneWipeHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Screen/WipeFade.hpp"
#include "Game/Screen/WipeGameOver.hpp"
#include "Game/Screen/WipeKoopa.hpp"
#include "Game/Screen/WipeRing.hpp"
#include "Game/Util/ObjUtil.hpp"

SceneWipeHolder::SceneWipeHolder() : WipeHolderBase(8, "\x83\x56\x81\x5b\x83\x93\x83\x8f\x83\x43\x83\x76\x95\xdb\x8e\x9d") {
    WipeLayoutBase* pWipeLayout;

    pWipeLayout = new WipeRing(1, "\x89\x7e\x83\x8f\x83\x43\x83\x76");
    addWipeLayout(pWipeLayout);
    MR::connectToSceneWipeLayout(pWipeLayout);
    MR::joinToNameObjGroup(pWipeLayout, "IgnorePauseNameObj");

    pWipeLayout = new WipeFade("\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x8f\x83\x43\x83\x76", Color8(0, 0, 0, 255));
    addWipeLayout(pWipeLayout);
    MR::connectToSceneWipeLayout(pWipeLayout);
    MR::joinToNameObjGroup(pWipeLayout, "IgnorePauseNameObj");

    pWipeLayout = new WipeFade("\x94\x92\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x8f\x83\x43\x83\x76", Color8(255, 255, 255, 255));
    addWipeLayout(pWipeLayout);
    MR::connectToSceneWipeLayout(pWipeLayout);
    MR::joinToNameObjGroup(pWipeLayout, "IgnorePauseNameObj");

    pWipeLayout = new WipeGameOver();
    addWipeLayout(pWipeLayout);
    MR::connectToSceneWipeLayout(pWipeLayout);
    MR::joinToNameObjGroup(pWipeLayout, "IgnorePauseNameObj");

    pWipeLayout = new WipeKoopa();
    addWipeLayout(pWipeLayout);
    MR::connectToSceneWipeLayout(pWipeLayout);
    MR::joinToNameObjGroup(pWipeLayout, "IgnorePauseNameObj");
}

namespace SceneWipeHolderFunction {
    SceneWipeHolder* getSceneWipeHolder() {
        return MR::getSceneObj< SceneWipeHolder >(SceneObj_SceneWipeHolder);
    }

    void openWipe(const char* pWipeName, s32 frame) {
        getSceneWipeHolder()->forceClose(pWipeName);
        getSceneWipeHolder()->wipe(nullptr, frame);
    }

    void closeWipe(const char* pWipeName, s32 frame) {
        getSceneWipeHolder()->forceOpen(pWipeName);
        getSceneWipeHolder()->wipe(nullptr, frame);
    }

    void forceOpenWipe(const char* pWipeName) {
        getSceneWipeHolder()->forceOpen(pWipeName);
    }

    void forceCloseWipe(const char* pWipeName) {
        getSceneWipeHolder()->forceClose(pWipeName);
    }
}  // namespace SceneWipeHolderFunction
