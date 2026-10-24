#include "Game/Demo/DemoCastGroupHolder.hpp"
#include "Game/Demo/DemoCastGroup.hpp"
#include "Game/Util/ObjUtil.hpp"

DemoCastGroupHolder::DemoCastGroupHolder() : NameObjGroup("\x83\x66\x83\x82\x8a\xd6\x8c\x57\x8e\xd2\x83\x4f\x83\x8b\x81\x5b\x83\x76\x95\xdb\x8e\x9d", 32) {
}

bool DemoCastGroupHolder::tryRegisterDemoActor(LiveActor* pActor, const JMapInfoIter& rIter, const JMapIdInfo& rInfo) const {
    for (s32 i = 0; i < getObjNum(); i++) {
        if (getCastGroup(i)->tryRegisterDemoActor(pActor, rIter, rInfo)) {
            return true;
        }
    }

    return false;
}

bool DemoCastGroupHolder::tryRegisterDemoActor(LiveActor* pActor, const char* pName, const JMapInfoIter& rIter) const {
    for (s32 i = 0; i < getObjNum(); i++) {
        if (getCastGroup(i)->tryRegisterDemoActor(pActor, pName, rIter)) {
            return true;
        }
    }

    return false;
}

DemoCastGroup* DemoCastGroupHolder::getCastGroup(int index) const {
    return static_cast< DemoCastGroup* >(getObj(index));
}

DemoCastGroup* DemoCastGroupHolder::findCastGroup(const char* pName) const {
    for (s32 i = 0; i < getObjNum(); i++) {
        DemoCastGroup* castGroup = getCastGroup(i);

        if (MR::isName(castGroup, pName)) {
            return castGroup;
        }
    }

    return nullptr;
}
