#include "Game/Boss/PoltaGroundRockHolder.hpp"
#include "Game/Boss/PoltaGroundRock.hpp"

namespace {
    static const s32 sMaxPoltaGroundRock = 16;
};  // namespace

PoltaGroundRockHolder::PoltaGroundRockHolder() : DeriveActorGroup< PoltaGroundRock >("\x83\x7c\x83\x8b\x83\x5e\x92\x6e\x96\xca\x8a\xe2\x8a\xc7\x97\x9d", ::sMaxPoltaGroundRock) {
    PoltaGroundRock* pGroundRock;

    for (int i = 0; i < ::sMaxPoltaGroundRock; i++) {
        pGroundRock = new PoltaGroundRock("\x83\x7c\x83\x8b\x83\x5e\x92\x6e\x96\xca\x8a\xe2");
        pGroundRock->initWithoutIter();
        registerActor(pGroundRock);
    }
}

void PoltaGroundRockHolder::breakAll() {
    int memberNum = getObjNum();

    for (int i = 0; i < memberNum; i++) {
        getMember(i)->requestBreak();
    }
}
