#include "Game/NPC/ButlerExplain.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace NrvButlerExplain {
    NEW_NERVE(ButlerExplainNrvDemo, ButlerExplain, Demo);
};  // namespace NrvButlerExplain

ButlerExplain::ButlerExplain(const char* pName) : NPCActor(pName) {
}

void ButlerExplain::init(const JMapInfoIter& rIter) {
    NPCActorCaps caps("ButlerExplain");
    caps.setDefault();
    caps.mObjectName = "Butler";

    NPCActor::initialize(rIter, caps);

    if (MR::tryRegisterDemoCast(this, rIter)) {
        DemoFunction::tryCreateDemoTalkAnimCtrlForActor(this, "DemoWithButler", nullptr);
        MR::registerDemoActionFunctor(this, MR::Functor(this, &ButlerExplain::startDemo), "\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe[\x8a\x4a\x8e\x6e]");
        DemoFunction::registerDemoTalkMessageCtrl(this, mMsgCtrl);
    }

    MR::tryRegisterDemoCast(this, "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x50\x8b\x41\x8a\xd2", rIter);
}

void ButlerExplain::control() {
    if (_D8) {
        MR::startSound(this, "SE_SM_NPC_TRAMPLED");
        MR::startSound(this, "SE_SV_BUTLER_TRAMPLED");
    }

    NPCActor::control();
}

void ButlerExplain::startDemo() {
    setNerve(GET_NERVE(ButlerExplain, ButlerExplainNrvDemo));
}

void ButlerExplain::exeDemo() {
}
