#include "Game/Enemy/WalkerStateBlowDamage.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"

void WalkerStateBlowDamage_FORCE_MATCH_SDATA2() {
    (void)1.0f;
}

namespace {
    static const f32 sAirFric = 0.99f;
    static const f32 sAirGravityAccel = 1.0f;
    static const f32 sDamageTurnLimit = 15.0f;
    static const s32 sDamageLandTime = 30;
};  // namespace

namespace NrvWalkerStateBlowDamage {
    NEW_NERVE(WalkerStateBlowDamageNrvBlow, WalkerStateBlowDamage, Blow);
    NEW_NERVE(WalkerStateBlowDamageNrvBlowLand, WalkerStateBlowDamage, BlowLand);
};  // namespace NrvWalkerStateBlowDamage

WalkerStateBlowDamage::WalkerStateBlowDamage(LiveActor* pHost, TVec3f* pDirection, WalkerStateBlowDamageParam* pBlowDamageParam)
    : ActorStateBase< LiveActor >("\x90\x81\x82\xab\x94\xf2\x82\xd1\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\xf3\x91\xd4", pHost), mDirection(pDirection), mBlowDamageParam(pBlowDamageParam) {
    initNerve(GET_NERVE(WalkerStateBlowDamage, WalkerStateBlowDamageNrvBlow));
}

void WalkerStateBlowDamage::appear() {
    mIsDead = false;
    setNerve(GET_NERVE(WalkerStateBlowDamage, WalkerStateBlowDamageNrvBlow));
}

void WalkerStateBlowDamage::exeBlow() {
    if (MR::isFirstStep(this)) {
        MR::startAction(getHost(), "Damage");
    }
    MR::attenuateVelocity(getHost(), ::sAirFric);
    MR::addVelocityToGravity(getHost(), ::sAirGravityAccel);
    MR::turnDirectionDegree(getHost(), mDirection, -getHost()->mVelocity, ::sDamageTurnLimit);

    if (MR::isGreaterStep(this, 5)) {
        if (MR::isBindedGround(getHost())) {
            MR::startAction(getHost(), "DamageLand");
            MR::zeroVelocity(getHost());
            setNerve(GET_NERVE(WalkerStateBlowDamage, WalkerStateBlowDamageNrvBlowLand));
        }
    }
}

void WalkerStateBlowDamage::exeBlowLand() {
    if (MR::isGreaterStep(this, ::sDamageLandTime)) {
        kill();
    }
}
