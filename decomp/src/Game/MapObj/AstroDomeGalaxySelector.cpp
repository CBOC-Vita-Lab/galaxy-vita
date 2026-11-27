#include "Game/MapObj/AstroDomeGalaxySelector.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/SphereSelector.hpp"
#include "Game/MapObj/AstroDomeCameraController.hpp"
#include "Game/MapObj/MiniatureGalaxy.hpp"
#include "Game/MapObj/MiniatureGalaxyHolder.hpp"
#include "Game/Screen/GalaxyConfirmLayout.hpp"
#include "Game/Screen/GalaxySelectBackButton.hpp"
#include "Game/Screen/GalaxySelectInfo.hpp"
#include "Game/Util/ActorSwitchUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#ifdef TARGET_PC
extern "C" void port_hrw_menu_turn(void);   // (vgx_galaxy.cpp, 14 Nov: no re-projected in-between pictures on the galaxy map)
#include "Game/MP/MpFlow.hpp"  // MP3: guests in a session do not start galaxies
#endif

namespace {
    const char* const cDemoNameDomeLecture = "\x83\x68\x81\x5b\x83\x80\x83\x8c\x83\x4e\x83\x60\x83\x83\x81\x5b\x82\x51";
    const char* const cDemoNameJumpOut = "\x83\x7d\x83\x8a\x83\x49\x94\xf2\x82\xd1\x8f\x6f\x82\xb5";
};  // namespace

namespace {
    MiniatureGalaxy* getSelectedMiniatureGalaxy() {
        return static_cast< MiniatureGalaxy* >(SphereSelectorFunction::getSelectedTarget());
    }
};  // namespace

namespace NrvAstroDomeGalaxySelector {
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxySelectStart, AstroDomeGalaxySelector, GalaxySelectStart);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxySelect, AstroDomeGalaxySelector, GalaxySelect);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxySelectCancel, AstroDomeGalaxySelector, GalaxySelectCancel);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxyConfirmStart, AstroDomeGalaxySelector, GalaxyConfirmStart);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxyConfirm, AstroDomeGalaxySelector, GalaxyConfirm);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxyConfirmCancel, AstroDomeGalaxySelector, GalaxyConfirmCancel);
    NEW_NERVE(AstroDomeGalaxySelectorNrvDemoJumpOut, AstroDomeGalaxySelector, DemoJumpOut);
    NEW_NERVE(AstroDomeGalaxySelectorNrvDemoDomeLecture, AstroDomeGalaxySelector, DemoDomeLecture);
    NEW_NERVE(AstroDomeGalaxySelectorNrvGalaxyMoveLecture, AstroDomeGalaxySelector, GalaxyMoveLecture);
    NEW_NERVE(AstroDomeGalaxySelectorNrvWaitStartDemo, AstroDomeGalaxySelector, WaitStartDemo);
};  // namespace NrvAstroDomeGalaxySelector

#ifdef TARGET_PC
// flow23 (PETARI_FLOW_DOME, decomp/src/Game/Scene/GameScene.cpp): the dome's galaxy selector of the scene being built notes itself, so the
// console's unattended entry can start its jump-out demo (Mario's flight out of the dome, the white flash) for a galaxy of the list, as a
// confirmed choice on the map does. Reset at every GameScene::init (before placement); read by the game thread once the scene runs.
static AstroDomeGalaxySelector* sPortFlowSelector = nullptr;
static const char* sPortFlowGalaxy = nullptr;
extern "C" void port_log_summary(const char* fmt, ...);
extern "C" const void* port_miniature_galaxy_at(int index);  // (MiniatureGalaxyHolder.cpp)
static unsigned sPortDomeDead = 0, sPortDomeOpens = 0, sPortDomeOpenTurns = 0;
extern "C" void port_flow_dome_reset(void) {
    sPortFlowSelector = nullptr;
    sPortFlowGalaxy = nullptr;
    sPortDomeDead = sPortDomeOpens = sPortDomeOpenTurns = 0;
}
// prog32a (20 Nov), debug only, default off: PETARI_DOMEMAP_OPEN=<n> opens the dome's galaxy map by itself n turns after the dome scene's play
// began (the call a SELECT_START message makes: AstroDomeGalaxySelector::appear, so the lecture demo of a first visit and the miniatures'
// first open run as for a player), and, with PETARI_DOMEMAP_OPEN_EVERY=<m>, again m turns after the map was left (the pad's B / Back
// button), so the first open and the reopens can be compared by a script (the emulator has no route to the map otherwise).
// PETARI_DOMEMAP_PICK_LOG=1 (alone or with it): while the map is open, every 120 turns one "[domepick]" line: the pointer's place, what it
// points at, and for every miniature the screen place of the matrix the pick is anchored to (the drawn model's base matrix) against the
// actor's position (the "[domemap]" line's): a pick that disagrees with the drawn place shows as a difference between the two.
// Called once a turn by GameScene::update (inAction: the game scene's action nerve and no demo).
extern "C" void port_domemap_turn(int inAction) {
    static int sOpen = -2, sEvery = 0, sLog = -1;
    if (sOpen == -2) {
        const char* o = getenv("PETARI_DOMEMAP_OPEN");
        const char* e = getenv("PETARI_DOMEMAP_OPEN_EVERY");
        const char* l = getenv("PETARI_DOMEMAP_PICK_LOG");
        sOpen = o != nullptr && atoi(o) > 0 ? atoi(o) : -1;
        sEvery = e != nullptr && atoi(e) > 0 ? atoi(e) : 0;
        sLog = l != nullptr && atoi(l) != 0 ? 1 : 0;
    }
    AstroDomeGalaxySelector* pSelector = sPortFlowSelector;
    if ((sOpen < 0 && sLog == 0) || pSelector == nullptr) {
        return;
    }
    if (MR::isDead(pSelector)) {
        sPortDomeOpenTurns = 0;
        if (!inAction || sOpen < 0) {
            return;
        }
        ++sPortDomeDead;
        const unsigned wait = sPortDomeOpens == 0 ? static_cast< unsigned >(sOpen) : static_cast< unsigned >(sEvery);
        if (wait != 0 && sPortDomeDead >= wait && !MR::isDemoActive()) {
            sPortDomeDead = 0;
            ++sPortDomeOpens;
            port_log_summary("domemap: opening the galaxy map by PETARI_DOMEMAP_OPEN, open number %u in this scene", sPortDomeOpens);
            // (dome33) start the SPHERE selector, as the blue Pull Star's bind does (AstroDomeBlueStar::exeBindHold): it sends the select-start message to the
            // selector, the miniatures and the dome. Calling only pSelector->appear() (prog32a) left the sphere selector idle: the selector waited for
            // isSelectWait() for ever and no map was drawn (repro33 run d33-a).
            SphereSelectorFunction::selectStart();
        }
        return;
    }
    sPortDomeDead = 0;
    ++sPortDomeOpenTurns;
    if (sLog == 0 || sPortDomeOpenTurns % 120u != 0u) {
        return;
    }
    const TVec2f* pointer = MR::getStarPointerScreenPosition(0);
    const MiniatureGalaxy* pointed = MiniatureGalaxyFunction::getPointingMiniatureGalaxy();
    port_log_summary("domepick: open %u, %u turns open: pointer (%.3f %.3f) valid %d, pointing at %s", sPortDomeOpens, sPortDomeOpenTurns,
                     pointer != nullptr ? pointer->x : 0.0f, pointer != nullptr ? pointer->y : 0.0f, SphereSelectorFunction::isValidPointing() ? 1 : 0,
                     pointed != nullptr ? pointed->mGalaxyName : "nothing");
    for (s32 i = 0; i < MiniatureGalaxyFunction::getMiniatureGalaxyNum(); ++i) {
        // (the holder keeps them in a group; the pick anchor of each is the base matrix's translation)
        const MiniatureGalaxy* g = static_cast< const MiniatureGalaxy* >(port_miniature_galaxy_at(i));
        if (g == nullptr) {
            continue;
        }
        MtxPtr base = const_cast< MiniatureGalaxy* >(g)->getBaseMtx();
        TVec3f anchor(base != nullptr ? base[0][3] : g->mPosition.x, base != nullptr ? base[1][3] : g->mPosition.y, base != nullptr ? base[2][3] : g->mPosition.z);
        TVec3f a, b;
        MR::calcScreenPosition(&a, anchor);
        MR::calcScreenPosition(&b, g->mPosition);
        port_log_summary("domepick:   %-24s anchor screen (%6.1f %6.1f) actor position screen (%6.1f %6.1f)%s%s", g->mGalaxyName, a.x, a.y, b.x, b.y,
                         g == pointed ? " <- pointed" : "", base == nullptr ? " (no base matrix)" : "");
    }
}
extern "C" int port_flow_dome_jump_out(const char* pGalaxy) {
    AstroDomeGalaxySelector* pSelector = sPortFlowSelector;
    if (pSelector == nullptr || pGalaxy == nullptr) {
        return 0;
    }
#ifdef TARGET_PC
    // MP3 (merge fix): test automation obeys the guest rule too: in a session only the host chooses the galaxy (same refusal and toast as a
    // guest's Yes at the dome's confirm). Not in a session: unchanged.
    if (mp_active() && MpFlowHooks::refuseGuestGalaxyStart()) {
        return 0;
    }
#endif
    sPortFlowGalaxy = pGalaxy;
    if (MR::isDead(pSelector)) {
        pSelector->LiveActor::appear();  // (alive for its nerve, without opening the map: AstroDomeGalaxySelector::appear would)
    }
    // (the call exeGalaxyConfirm makes on "yes"; its DemoJumpOut asks the star select when the demo is over)
    MR::requestStartTimeKeepDemoMarioPuppetable(pSelector, ::cDemoNameJumpOut, GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvDemoJumpOut),
                                                GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvWaitStartDemo), 0);
    return 1;
}
#endif

AstroDomeGalaxySelector::AstroDomeGalaxySelector(const char* pName)
    : LiveActor(pName), mCameraController(), mBackButton(), mInfo(), mConfirmLayout(), _9C() {
}

void AstroDomeGalaxySelector::init(const JMapInfoIter& rIter) {
    MR::connectToSceneMapObjMovement(this);
    MR::invalidateClipping(this);

    mCameraController = new AstroDomeCameraController("\x93\x56\x95\xb6\x83\x68\x81\x5b\x83\x80\x83\x4a\x83\x81\x83\x89\x90\xa7\x8c\xe4");
    mCameraController->initWithoutIter();

    mBackButton = new GalaxySelectBackButton();
    mBackButton->initWithoutIter();

    mInfo = new GalaxySelectInfo(false);
    mInfo->initWithoutIter();

    mConfirmLayout = new GalaxyConfirmLayout();
    mConfirmLayout->initWithoutIter();

    MR::needStageSwitchWriteA(this, rIter);
    MR::onSwitchA(this);
    initNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelectStart));
    MR::tryRegisterDemoCast(this, rIter);
    SphereSelectorFunction::registerTarget(this);
    makeActorDead();
#ifdef TARGET_PC
    sPortFlowSelector = this;  // (flow23)
#endif
}

void AstroDomeGalaxySelector::appear() {
    LiveActor::appear();
    MR::offSwitchA(this);
    setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelectStart));
}

void AstroDomeGalaxySelector::kill() {
    MR::resetYesNoSelectorSE();
    MR::onSwitchA(this);
    LiveActor::kill();
    mInfo->kill();
    MR::disappearStarCounter();
}

bool AstroDomeGalaxySelector::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (SphereSelectorFunction::isMsgSelectStart(msg)) {
        appear();
        return true;
    }

    if (SphereSelectorFunction::isMsgConfirmStart(msg)) {
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirmStart));
        return true;
    }

    if (SphereSelectorFunction::isMsgTargetSelected(msg)) {
        mBackButton->decide();
        SphereSelectorFunction::selectCancel(false);
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelectCancel));
        return true;
    }

    return false;
}

void AstroDomeGalaxySelector::showGalaxyInfo(const MiniatureGalaxy* pMiniGalaxy) {
    s32 state = 3;
    s32 cometID = -1;

    if (pMiniGalaxy->mState == 2) {
        state = 2;
    } else if (pMiniGalaxy->mState == 1) {
        state = 1;
    } else if (pMiniGalaxy->mState == 0) {
        state = 0;
    }

    if (state == 3 && pMiniGalaxy == MiniatureGalaxyFunction::getCometLandMiniatureGalaxy()) {
        cometID = MiniatureGalaxyFunction::getCometNameId();
    }

    mInfo->show(pMiniGalaxy->mGalaxyName, state, cometID);
}

bool AstroDomeGalaxySelector::tryStartLectureDemo(const Nerve* pNerve) {
    if (isNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelectStart))) {
        if (!MR::isOnGameEventFlagEndButlerDomeLecture()) {
            MR::requestStartTimeKeepDemoWithoutCinemaFrame(this, ::cDemoNameDomeLecture, pNerve,
                                                           GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvWaitStartDemo), 0);
            return true;
        }
    } else if (isNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirmStart))) {
        if (!MR::isOnGameEventFlagEndButlerGalaxyMoveLecture()) {
            MR::requestStartTimeKeepDemoWithoutCinemaFrame(this, ::cDemoNameDomeLecture, pNerve,
                                                           GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvWaitStartDemo), 0);
            return true;
        }
    }

    return false;
}

void AstroDomeGalaxySelector::exeGalaxySelectStart() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (!SphereSelectorFunction::isSelectWait()) {
        return;
    }

    if (tryStartLectureDemo(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvDemoDomeLecture))) {
        return;
    }

    MR::appearStarCounter();
    setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelect));
}

void AstroDomeGalaxySelector::exeGalaxySelect() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (MR::isFirstStep(this)) {
        mBackButton->appear();
    }

    if (SphereSelectorFunction::isValidPointing() && MR::testCorePadTriggerB(WPAD_CHAN0)) {
        MR::startSystemSE("SE_SY_GALAXY_DECIDE_CANCEL");

        if (mBackButton->isAppearing()) {
            mBackButton->kill();
        } else {
            mBackButton->disappear();
        }

        SphereSelectorFunction::selectCancel(false);
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelectCancel));
    } else {
        if (!MR::isOnGameEventFlagOffAstroDomeGuidance()) {
            MR::requestPointerGuidanceNoInformation();
        }

        if (!SphereSelectorFunction::isHandleHolding() && mBackButton->isPointing()) {
            SphereSelectorFunction::registerPointingTarget(this, HandlePointingPriority(2));

            if (!_9C) {
                MR::startSystemSE("SE_SY_BUTTON_CURSOR_ON");
            }

            _9C = true;
        } else {
            _9C = false;
        }

        if (SphereSelectorFunction::isPointingTarget(this)) {
            mBackButton->tryPointing();
        }

        MiniatureGalaxy* pointingMiniatureGalaxy = MiniatureGalaxyFunction::getPointingMiniatureGalaxy();

        if (pointingMiniatureGalaxy != nullptr) {
            showGalaxyInfo(pointingMiniatureGalaxy);
        }
    }
}

void AstroDomeGalaxySelector::exeGalaxySelectCancel() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (mBackButton->isDisappearing() || MR::isDead(mBackButton)) {
        SphereSelectorFunction::selectEnd();
        kill();
    }
}

void AstroDomeGalaxySelector::exeGalaxyConfirmStart() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (MR::isFirstStep(this)) {
        mBackButton->disappear();
    }

    showGalaxyInfo(::getSelectedMiniatureGalaxy());

    if (SphereSelectorFunction::isConfirmWait() &&
        !tryStartLectureDemo(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyMoveLecture))) {
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirm));
    }
}

void AstroDomeGalaxySelector::exeGalaxyConfirm() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (MR::isFirstStep(this)) {
        mConfirmLayout->appear();
    }

    showGalaxyInfo(::getSelectedMiniatureGalaxy());

    if (!mConfirmLayout->isSelected()) {
        return;
    }

#ifdef TARGET_PC
    // MP3: the host chooses the galaxy: a guest's Yes is refused (toast) and taken as No
    if (mConfirmLayout->isSelectedYes() && mp_active() && MpFlowHooks::refuseGuestGalaxyStart()) {
        SphereSelectorFunction::confirmCancel(false);
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirmCancel));
        return;
    }
#endif
    if (mConfirmLayout->isSelectedYes()) {
        SphereSelectorFunction::confirmed();
        MR::requestStartTimeKeepDemoMarioPuppetable(this, ::cDemoNameJumpOut,
                                                    GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvDemoJumpOut),
                                                    GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvWaitStartDemo), 0);
    } else {
        SphereSelectorFunction::confirmCancel(false);
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirmCancel));
    }
}

void AstroDomeGalaxySelector::exeGalaxyConfirmCancel() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (SphereSelectorFunction::isSelectWait()) {
        setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelect));
    }
}

void AstroDomeGalaxySelector::exeDemoJumpOut() {
    if (MR::isFirstStep(this)) {
        MR::disappearStarCounter();
        mConfirmLayout->requestMovementOn();
        MR::requestMovementOn(mInfo);
    }

    if (MR::isDemoActive(::cDemoNameJumpOut)) {
        return;
    }

#ifdef TARGET_PC
    if (sPortFlowGalaxy != nullptr) {
        // flow23: the demo started for PETARI_FLOW (no galaxy chosen on the map): the list's galaxy
        const char* pFlowGalaxy = sPortFlowGalaxy;
        sPortFlowGalaxy = nullptr;
        port_log_summary("flow: the dome's jump-out demo is over: asking for the star select of %s", pFlowGalaxy);
        MR::requestStartScenarioSelect(pFlowGalaxy);
        kill();
        return;
    }
#endif
    const char* pGalaxyName = ::getSelectedMiniatureGalaxy()->mGalaxyName;
    MiniatureGalaxy* pCometGalaxy = MiniatureGalaxyFunction::getCometLandMiniatureGalaxy();

    if (pCometGalaxy != nullptr && pCometGalaxy == SphereSelectorFunction::getSelectedTarget()) {
        MR::requestStartScenarioSelectForComet(pGalaxyName, MR::getEncounterGalaxyCometPowerStarId(pGalaxyName));
    } else {
        MR::requestStartScenarioSelect(pGalaxyName);
    }

    kill();
}

void AstroDomeGalaxySelector::exeDemoDomeLecture() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (MR::isFirstStep(this)) {
        MR::requestMovementOnPlayer();
    }

    if (MR::isTimeKeepDemoActive()) {
        return;
    }

    MR::onGameEventFlagEndButlerDomeLecture();
    MR::appearStarCounter();
    setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxySelect));
}

void AstroDomeGalaxySelector::exeGalaxyMoveLecture() {
#ifdef TARGET_PC
    port_hrw_menu_turn();
#endif
    if (MR::isFirstStep(this)) {
        MR::requestMovementOnPlayer();
    }

    if (MR::isTimeKeepDemoActive()) {
        return;
    }

    MR::onGameEventFlagEndButlerGalaxyMoveLecture();
    setNerve(GET_NERVE(AstroDomeGalaxySelector, AstroDomeGalaxySelectorNrvGalaxyConfirm));
}

void AstroDomeGalaxySelector::exeWaitStartDemo() {
}
