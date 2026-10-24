#include "Game/NPC/MiiFaceIconHolder.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NPC/MiiFaceIcon.hpp"
#include "Game/NameObj/NameObjAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ObjUtil.hpp"

MiiFaceIconHolder::MiiFaceIconHolder(u32 iconNum, const char* pName)
    : NameObj(pName), mIconNumMax(iconNum), mIconNum(0), mIcon(new MiiFaceIcon*[iconNum]) {
    MR::connectToScene(MR::createDrawAdaptor("Mii\x83\x41\x83\x43\x83\x52\x83\x93\x90\xb6\x90\xac", MR::Functor(this, &MiiFaceIconHolder::drawIcons)), MR::MovementType_None,
                       MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_MiiFaceIcon);
}

void MiiFaceIconHolder::drawIcons() {
    for (int i = 0; i < mIconNum; i++) {
        if (mIcon[i]->mIsRequestMakeIcon) {
            mIcon[i]->drawIcon();
            break;
        }
    }
}

void MiiFaceIconHolder::registerIcon(MiiFaceIcon* pIcon) {
    mIcon[mIconNum++] = pIcon;
}

namespace MR {
    MiiFaceIconHolder* getMiiFaceIconHolder() {
        return MR::getSceneObj< MiiFaceIconHolder >(SceneObj_MiiFaceIconHolder);
    }

    void registerMiiFaceIcon(MiiFaceIcon* pIcon) {
        MR::createSceneObj(SceneObj_MiiFaceIconHolder);
        getMiiFaceIconHolder()->registerIcon(pIcon);
    }
};  // namespace MR
