#include "Game/NPC/RosettaDemoAstroDome.hpp"
#include "Game/Demo/DemoFunction.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NPC/Rosetta.hpp"
#include "Game/NPC/TalkMessageFunc.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Screen/IconAButton.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <cstdio>

// TODO: this file is not linkable yet due to a mismatch in .data related to TalkMessageFunc

namespace NrvRosettaDemoAstroDomeExplain {
    NEW_NERVE(RosettaDemoAstroDomeExplainNrvExplainDemo, RosettaDemoAstroDomeExplain, Demo);
};  // namespace NrvRosettaDemoAstroDomeExplain

namespace NrvRosettaDemoAstroDomeFinalBattle {
    NEW_NERVE(RosettaDemoAstroDomeFinalBattleNrvFinalBattleDemo, RosettaDemoAstroDomeFinalBattle, Demo);
};  // namespace NrvRosettaDemoAstroDomeFinalBattle

namespace NrvRosettaDemoAstroDomeTalk {
    NEW_NERVE(RosettaDemoAstroDomeTalkNrvTalkDemo, RosettaDemoAstroDomeTalk, Demo);
};  // namespace NrvRosettaDemoAstroDomeTalk

template < typename T >
static void EntryDemo(T* caller, const char* pDemoName, const char* pRootName, const JMapInfoIter& rIter) {
    if (MR::isDemoExist(pDemoName)) {
        TVec3f offset(MR::getMessageBalloonFollowOffset(caller->mRosetta->mMsgCtrl));
        TalkMessageCtrl* ctrl = MR::createTalkCtrlDirectOnRootNodeAutomatic(caller->mRosetta, rIter, pRootName, offset, nullptr);
        MR::registerEventFunc(ctrl, TalkMessageFunc(caller->mRosetta, &Rosetta::eventFunc));
        DemoFunction::registerDemoTalkMessageCtrlDirect(caller->mRosetta, ctrl, pDemoName);
        MR::registerDemoActionFunctorDirect(caller->mRosetta, MR::Functor(caller, &T::startDemo), pDemoName, "\x8a\x4a\x8e\x6e");
    }
}

RosettaMonologue::RosettaMonologue() : SimpleLayout("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x82\xcc\x8c\xea\x82\xe8", "PrologueStarSteward", 2, -1), mTextFormer(this, "Text00") {
    MR::createAndAddPaneCtrl(this, "TalkBalloon", 1);
    MR::createAndAddPaneCtrl(this, "CrossFade1", 1);
    MR::createAndAddPaneCtrl(this, "CrossFade2", 1);
    MR::createAndAddPaneCtrl(this, "CrossFade3", 1);

    mIconAButton = new IconAButton(true, false);
    mIconAButton->initWithoutIter();
    mIconAButton->kill();
    mMessageNo = 0;
}

void RosettaMonologue::appear() {
    LayoutActor::appear();
    mMessageNo = 0;

    MR::startAnim(this, "FadeIn", 0);
    MR::startPaneAnim(this, "TalkBalloon", "WinFadeIn", 0);

    char messageId[256];
    snprintf(messageId, sizeof(messageId), "RosettaMonologue%03d", mMessageNo);
    mTextFormer.formMessage(MR::getGameMessageDirect(messageId), 1);
}

void RosettaMonologue::calcAnim() {
    LayoutActor::calcAnim();
    mIconAButton->calcAnim();
}

void RosettaMonologue::movement() {
    LayoutActor::movement();
    mIconAButton->movement();
}

void RosettaMonologue::control() {
    if (mMessageNo >= 4) {
        if (MR::isAnimStopped(this, 0)) {
            MR::forceCloseWipeFade();
            kill();
        }
        return;
    }

    mTextFormer.updateTalking();

    if (!mTextFormer.isTextAppearedAll()) {
        return;
    }

    if (!mIconAButton->isOpen()) {
        mIconAButton->openWithoutMessage();
    }

    MR::setLayoutPosAtPaneTrans(mIconAButton, this, "AButtonPosition");

    if (!MR::testCorePadTriggerA(WPAD_CHAN0)) {
        return;
    }

    mIconAButton->term();

    if (mTextFormer.nextPage()) {
        MR::startSystemSE("SE_SY_TALK_FOCUS_ITEM");
        return;
    }

    mMessageNo++;

    if (mMessageNo == 4) {
        MR::startSystemSE("SE_SY_TALK_OK");
        MR::startCSSound("CS_CLICK_CLOSE", nullptr, WPAD_CHAN0);
    } else {
        MR::startSystemSE("SE_SY_TALK_FOCUS_ITEM");

        char messageId[256];
        snprintf(messageId, sizeof(messageId), "RosettaMonologue%03d", mMessageNo);

        mTextFormer.formMessage(MR::getGameMessageDirect(messageId), 1);
    }

    switch (mMessageNo) {
    case 1:
        MR::startPaneAnim(this, "CrossFade1", "CrossFade1", 0);
        break;
    case 2:
        MR::startPaneAnim(this, "CrossFade2", "CrossFade2", 0);
        break;
    case 3:
        MR::startPaneAnim(this, "CrossFade3", "CrossFade3", 0);
        break;
    case 4:
        MR::startAnim(this, "FadeOut", 0);
        MR::startPaneAnim(this, "TalkBalloon", "WinFadeOut", 0);
        break;
    }
}

RosettaDemoAstroDomeExplain::RosettaDemoAstroDomeExplain(Rosetta* pRosetta, const JMapInfoIter& rIter)
    : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mRosetta(pRosetta) {
    const char* sDemoExplain = "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe\x83\x66\x83\x82";
    DemoFunction::tryCreateDemoTalkAnimCtrlForSceneDirect(mRosetta, sDemoExplain, rIter, "DemoWithButler", nullptr, 0, 0);
    DemoFunction::registerDemoTalkMessageCtrlDirect(mRosetta, mRosetta->mMsgCtrl, sDemoExplain);
    MR::registerDemoActionFunctorDirect(mRosetta, MR::Functor(this, &RosettaDemoAstroDomeExplain::startDemo), sDemoExplain, "\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe[\x8a\x4a\x8e\x6e]");
    mMonologue = new RosettaMonologue();
    initNerve(GET_NERVE(RosettaDemoAstroDomeExplain, RosettaDemoAstroDomeExplainNrvExplainDemo));
}

void RosettaDemoAstroDomeExplain::makeArchiveList(NameObjArchiveListCollector* pCollector, const JMapInfoIter& rIter) {
    pCollector->addArchive("PrologueStarSteward");
}

void RosettaDemoAstroDomeExplain::startDemo() {
    mRosetta->startDemo(this);
}

void RosettaDemoAstroDomeExplain::exeDemo() {
    if (MR::isDemoPartActive("\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe[\x8a\x47\x96\x7b\x95\x5c\x8e\xa6]")) {
        if (MR::isDemoPartFirstStep("\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe[\x8a\x47\x96\x7b\x95\x5c\x8e\xa6]")) {
            MR::tryFrameToScreenCinemaFrame();
            mMonologue->appear();
            MR::pauseTimeKeepDemo(mRosetta);
        }

        mMonologue->movement();
        mMonologue->calcAnim();

        if (MR::isDead(mMonologue)) {
            MR::resumeTimeKeepDemo(mRosetta);
        }
    }

    if (MR::isDemoLastStep()) {
        mRosetta->endDemo();
    }
}

RosettaDemoAstroDomeFinalBattle::RosettaDemoAstroDomeFinalBattle(Rosetta* pRosetta, const JMapInfoIter& rIter)
    : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8d\xc5\x8f\x49\x8c\x88\x90\xed\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mRosetta(pRosetta) {
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8d\xc5\x8f\x49\x8c\x88\x90\xed\x83\x66\x83\x82", "AstroGalaxy_Rosetta300", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x6d\x81\x5b\x83\x7d\x83\x8b\x83\x47\x83\x93\x83\x66\x83\x42\x83\x93\x83\x4f\x8c\xe3\x83\x66\x83\x82", "AstroGalaxy_Rosetta400", rIter);

    MR::needStageSwitchWriteA(mRosetta, rIter);
    initNerve(GET_NERVE(RosettaDemoAstroDomeFinalBattle, RosettaDemoAstroDomeFinalBattleNrvFinalBattleDemo));
}

void RosettaDemoAstroDomeFinalBattle::startDemo() {
    mRosetta->startDemo(this);
}

void RosettaDemoAstroDomeFinalBattle::exeDemo() {
    if (MR::isDemoLastStep()) {
        mRosetta->endDemoWithInterpole();
    }
}

RosettaDemoAstroDomeTalk::RosettaDemoAstroDomeTalk(Rosetta* pRosetta, const JMapInfoIter& rIter)
    : NerveExecutor("\x83\x8d\x83\x5b\x83\x62\x83\x5e\x89\xef\x98\x62\x83\x66\x83\x82\x8e\xc0\x8d\x73\x8e\xd2"), mRosetta(pRosetta) {
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x4c\x83\x6d\x83\x73\x83\x49\x92\x54\x8c\x9f\x91\xe0\x83\x66\x83\x82", "AstroGalaxy_Rosetta080", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\xe0\x96\xbe\x83\x66\x83\x82", "AstroGalaxy_Rosetta020", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x52\x83\x81\x83\x62\x83\x67\x90\xe0\x96\xbe\x83\x66\x83\x82", "AstroGalaxy_Rosetta030", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8b\xe2\x89\xcd\x82\xcc\x92\x86\x90\x53\x90\xe0\x96\xbe\x83\x66\x83\x82", "AstroGalaxy_Rosetta040", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x93\x56\x95\xb6\x91\xe4\x8b\x40\x94\x5c\x89\xf1\x95\x9c\x83\x66\x83\x82", "AstroGalaxy_Rosetta050", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x4a\x83\x45\x83\x93\x83\x67\x83\x5f\x83\x45\x83\x93\x8a\x4a\x8e\x6e\x83\x66\x83\x82", "AstroGalaxy_Rosetta060", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\x69\x92\xbb\x83\x66\x83\x82", "AstroGalaxy_Rosetta084", rIter);
    EntryDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x8b\x83\x43\x81\x5b\x83\x57\x83\x66\x83\x82", "AstroGalaxy_Rosetta054", rIter);

    initNerve(GET_NERVE(RosettaDemoAstroDomeTalk, RosettaDemoAstroDomeTalkNrvTalkDemo));
}

void RosettaDemoAstroDomeTalk::startDemo() {
    mRosetta->startDemo(this);
}

void RosettaDemoAstroDomeTalk::exeDemo() {
    if (MR::isDemoLastStep()) {
        mRosetta->endDemo();
    }
}
