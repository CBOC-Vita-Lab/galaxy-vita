#include "Game/Boss/KoopaBattleStairsVs3.hpp"
#include "Game/Boss/Koopa.hpp"
#include "Game/Boss/KoopaFunction.hpp"
#include "Game/LiveActor/LiveActorGroup.hpp"
#include "Game/LiveActor/ModelObj.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/KoopaBattleMapStair.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

namespace {
    static const f32 sFireSpeed = 30.0f;
};  // namespace

namespace NrvKoopaBattleStairsVs3 {
    NEW_NERVE(KoopaBattleStairsVs3NrvWaitDemo, KoopaBattleStairsVs3, WaitDemo);
    NEW_NERVE(KoopaBattleStairsVs3NrvDemo, KoopaBattleStairsVs3, Demo);
    NEW_NERVE(KoopaBattleStairsVs3NrvWait, KoopaBattleStairsVs3, Wait);
};  // namespace NrvKoopaBattleStairsVs3

KoopaBattleStairsVs3::KoopaBattleStairsVs3(Koopa* pKoopa) : KoopaBattleStairsBase(pKoopa), mNamePos(0.0f, 0.0f, 0.0f) {
    initNerve(GET_NERVE(KoopaBattleStairsVs3, KoopaBattleStairsVs3NrvWaitDemo));
    MR::findNamePos("\x83\x4e\x83\x62\x83\x70\x8a\x4b\x92\x69\x90\xed\x82\xcc\x96\x43\x92\x65\x8f\x6f\x8c\xbb", &mNamePos, nullptr);
    KoopaFunction::initKoopaAnimCamera(mKoopa, "DemoKoopaBattleStairsVs3Start");
}

s32 KoopaBattleStairsVs3::registerStair(KoopaBattleMapStair* pBattleMapStair) {
    mStairsGroup->registerActor(pBattleMapStair);

    return calcFireAttackStep(pBattleMapStair, ::sFireSpeed, 0, mNamePos);
}

void KoopaBattleStairsVs3::exeWaitDemo() {
    if (!KoopaFunction::tryStartKoopaCameraDemo(mKoopa, "\x8a\x4b\x92\x69\x82\xcc\x90\xed\x82\xa2\x8a\x4a\x8e\x6e\x83\x66\x83\x82", "DemoKoopaBattleStairsVs3Start", "\x83\x66\x83\x82\x92\x86\x90\x53")) {
        return;
    }

    KoopaFunction::getKoopaDemoMeteor1(mKoopa)->appear();
    KoopaFunction::getKoopaDemoMeteor2(mKoopa)->appear();
    KoopaFunction::getKoopaDemoMeteor3(mKoopa)->appear();

    MR::requestMovementOn(KoopaFunction::getKoopaDemoPeach(mKoopa));
    MR::requestMovementOn(KoopaFunction::getKoopaDemoKoopaJr(mKoopa));
    MR::requestMovementOn(KoopaFunction::getKoopaDemoKoopaJrShip(mKoopa));
    MR::requestMovementOn(KoopaFunction::getKoopaDemoMeteor1(mKoopa));
    MR::requestMovementOn(KoopaFunction::getKoopaDemoMeteor2(mKoopa));
    MR::requestMovementOn(KoopaFunction::getKoopaDemoMeteor3(mKoopa));

    MR::startAction(KoopaFunction::getKoopaDemoPeach(mKoopa), "DemoKoopaBattleStairsVs3Start");
    MR::startAction(KoopaFunction::getKoopaDemoKoopaJr(mKoopa), "DemoKoopaBattleStairsVs3Start");
    MR::startAction(KoopaFunction::getKoopaDemoKoopaJrShip(mKoopa), "DemoKoopaBattleStairsVs3Start");
    MR::startAction(KoopaFunction::getKoopaDemoMeteor1(mKoopa), "DemoKoopaBattleStairsVs3Start01");
    MR::startAction(KoopaFunction::getKoopaDemoMeteor2(mKoopa), "DemoKoopaBattleStairsVs3Start02");
    MR::startAction(KoopaFunction::getKoopaDemoMeteor3(mKoopa), "DemoKoopaBattleStairsVs3Start03");

    KoopaFunction::endFaceCtrl(mKoopa, -1);

    setNerve(GET_NERVE(KoopaBattleStairsVs3, KoopaBattleStairsVs3NrvDemo));
}

void KoopaBattleStairsVs3::exeDemo() {
    if (!KoopaFunction::tryEndKoopaCameraDemo(mKoopa, "\x8a\x4b\x92\x69\x82\xcc\x90\xed\x82\xa2\x8a\x4a\x8e\x6e\x83\x66\x83\x82", "DemoKoopaBattleStairsVs3Start")) {
        return;
    }

    KoopaFunction::startFaceCtrl(mKoopa);

    MR::startAction(KoopaFunction::getKoopaDemoPeach(mKoopa), "DemoKoopaVs3Wait");
    MR::startAction(KoopaFunction::getKoopaDemoKoopaJr(mKoopa), "DemoKoopaVs3Wait");
    MR::startAction(KoopaFunction::getKoopaDemoKoopaJrShip(mKoopa), "DemoKoopaVs3Wait");

    KoopaFunction::getKoopaDemoMeteor1(mKoopa)->kill();
    KoopaFunction::getKoopaDemoMeteor2(mKoopa)->kill();
    KoopaFunction::getKoopaDemoMeteor3(mKoopa)->kill();

    setNerve(GET_NERVE(KoopaBattleStairsVs3, KoopaBattleStairsVs3NrvWait));
}

void KoopaBattleStairsVs3::exeWait() {
    if (MR::isFirstStep(this)) {
        MR::startAction(mKoopa, "Wait");
    }

    tryAttack();
}

void KoopaBattleStairsVs3::tryAttack() {
    KoopaBattleMapStair* pBattleMapStair;

    for (int idx = 0; idx < mStairsGroup->getObjNum(); idx++) {
        pBattleMapStair = static_cast< KoopaBattleMapStair* >(mStairsGroup->getActor(idx));

        if (pBattleMapStair->isRequestAttackVs3()) {
            pBattleMapStair->_A6 = true;
            KoopaFunction::emitFireStairsToTarget(mKoopa, pBattleMapStair, mNamePos, true);
        }
    }
}

KoopaBattleStairsVs3::~KoopaBattleStairsVs3() {
}
