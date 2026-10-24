#include "Game/Enemy/KameckHolder.hpp"
#include "Game/Enemy/Kameck.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

KameckHolder::KameckHolder(s32 numMax) : DeriveActorGroup< Kameck >("\x83\x4a\x83\x81\x83\x62\x83\x4e\x8a\xc7\x97\x9d", numMax) {
}

void KameckHolder::startDemoAppear() {
    for (s32 i = 0; i < getObjNum(); i++) {
        Kameck* pActor = getMember(i);

        if (!MR::isDead(pActor)) {
            continue;
        }

        pActor->startDemoAppear();
        MR::requestMovementOn(pActor);
    }
}

void KameckHolder::endDemoAppear() {
    for (s32 i = 0; i < getObjNum(); i++) {
        getMember(i)->endDemoAppear();
    }
}

void KameckHolder::deadForceAll() {
    for (s32 i = 0; i < getObjNum(); i++) {
        Kameck* pActor = getMember(i);

        if (MR::isDead(pActor)) {
            continue;
        }

        pActor->makeActorDeadForce();
    }
}
