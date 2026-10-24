#include "Game/Boss/BossStinkBugBombHolder.hpp"
#include "Game/Boss/BossStinkBugBomb.hpp"
#include "Game/Util/LiveActorUtil.hpp"

namespace {
    static const s32 sMaxBossStinkBugBomb = 16;
};  // namespace

BossStinkBugBombHolder::BossStinkBugBombHolder(LiveActor* pHost)
    : DeriveActorGroup< BossStinkBugBomb >("\x83\x7b\x83\x58\x83\x4a\x83\x81\x83\x80\x83\x56\x94\x9a\x92\x65\x8a\xc7\x97\x9d", ::sMaxBossStinkBugBomb) {
    BossStinkBugBomb* pBomb;

    for (s32 i = 0; i < ::sMaxBossStinkBugBomb; i++) {
        pBomb = new BossStinkBugBomb("\x83\x7b\x83\x58\x83\x4a\x83\x81\x83\x80\x83\x56\x94\x9a\x92\x65");
        pBomb->initWithoutIter();

        MR::setBinderExceptActor(pBomb, pHost);
        registerActor(pBomb);
    }
}
