#include "Game/NPC/RosettaDemoEpilogue.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/LiveActor/LodCtrl.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NPC/Rosetta.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace NrvRosettaDemoEpilogue {
    NEW_NERVE(RosettaDemoEpilogueNrvDemo, RosettaDemoEpilogue, Demo);
};  // namespace NrvRosettaDemoEpilogue

RosettaDemoEpilogue::RosettaDemoEpilogue(Rosetta* pRosetta, const JMapInfoIter& rIter)
    : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mRosetta(pRosetta), mIsFadeOut() {
    DemoFunction::tryCreateDemoTalkAnimCtrlForScene(pRosetta, rIter, "DemoEpilogueB", nullptr, 0, 0);
    DemoFunction::registerDemoTalkMessageCtrl(mRosetta, mRosetta->mMsgCtrl);
    MR::registerDemoActionFunctor(mRosetta, MR::Functor(this, &RosettaDemoEpilogue::startDemo), "\x83\x47\x83\x73\x83\x8d\x81\x5b\x83\x4f[\x8a\x4a\x8e\x6e]");
    mRosetta->mLodCtrl->invalidate();
    initNerve(GET_NERVE(RosettaDemoEpilogue, RosettaDemoEpilogueNrvDemo));
}

void RosettaDemoEpilogue::startDemo() {
    mIsFadeOut = false;

    mRosetta->startDemo(this);
}

void RosettaDemoEpilogue::exeDemo() {
    if (MR::isDemoPartActive("\x83\x47\x83\x73\x83\x8d\x81\x5b\x83\x4f[\x83\x74\x83\x46\x81\x5b\x83\x68\x83\x41\x83\x45\x83\x67]")) {
        mIsFadeOut = true;
    }

    if (!mIsFadeOut) {
        MR::startAtmosphereLevelSE("SE_DM_LV_EPILOGUE_BABY_CRY");
    }

    if (MR::isDemoLastStep()) {
        mRosetta->endDemo();
    }
}
