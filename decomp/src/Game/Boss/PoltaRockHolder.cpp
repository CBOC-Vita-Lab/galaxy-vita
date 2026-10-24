#include "Game/Boss/PoltaRockHolder.hpp"
#include "Game/Boss/PoltaRock.hpp"

namespace {
    static const s32 sMaxPoltaRock = 16;
};  // namespace

PoltaRockHolder::PoltaRockHolder() : DeriveActorGroup< PoltaRock >("\x83\x7c\x83\x8b\x83\x5e\x82\xcc\x8a\xe2\x8a\xc7\x97\x9d", ::sMaxPoltaRock) {
    PoltaRock* pRock;

    for (int i = 0; i < ::sMaxPoltaRock; i++) {
        pRock = new PoltaRock("\x83\x7c\x83\x8b\x83\x5e\x8a\xe2");
        pRock->initWithoutIter();
        registerActor(pRock);
    }
}
