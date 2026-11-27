#include "Game/Screen/PauseMenu.hpp"
#include "Game/MP/MpMemProbe.hpp"  // memprobe
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/ButtonPaneController.hpp"
#include "Game/Screen/LuigiLetter.hpp"
#include "Game/Screen/SysInfoWindow.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MessageUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"

namespace {
    const s32 cBackSequenceFadeFrame = 90;

    bool isInvalidBackAstroDome() {
        return !MR::isOnGameEventFlagUseAstroDome() || (MR::isStageAstroLocation() && !MR::isEqualStageName("HeavensDoorGalaxy"));
    }

    bool isStageHideScenarioTitle() {
        return MR::isStageBeginPrologueEvent() || MR::isStageAstroLocation();
    }
}  // namespace

#ifdef TARGET_PC
// The settings of the handheld ports (camera zoom: L / R, rear touch aiming: Triangle), shown under the pause menu's buttons while
// the menu waits for a choice. platform/src/port/settings_panel.cpp draws the panel, in the game's fonts, and decides when it shows
// (port_zoom_menu_active: platform/src/port/perf.cpp keeps the values, vita/runtime/input.cpp reads the buttons), so it can change
// without a rebuild of the game. flags: 1 = the Luigi letter button is on screen (bottom middle).
extern "C" void port_zoom_menu(int selecting);
extern "C" void port_settings_panel_draw(int flags);
// settingsnative: the native Settings menu (the MP menu family's KIND_SETTINGS) is open over the paused game: the pause menu's buttons are inert
// meanwhile (Start and Select, also while a button is being captured, must not close it). platform/src/settings/settings_native.cpp.
extern "C" bool port_settings_native_menu_open(void) __attribute__((weak));
static inline bool settingsNativeOpen() { return port_settings_native_menu_open != nullptr && port_settings_native_menu_open(); }
#endif

namespace NrvPauseMenu {
    NEW_NERVE(PauseMenuNrvSelecting, PauseMenu, Selecting);
    NEW_NERVE(PauseMenuNrvDecided, PauseMenu, Decided);
    NEW_NERVE(PauseMenuNrvDisappear, PauseMenu, Disappear);
    NEW_NERVE(PauseMenuNrvConfirm, PauseMenu, Confirm);
    NEW_NERVE(PauseMenuNrvGameDataSave, PauseMenu, GameDataSave);
    NEW_NERVE(PauseMenuNrvLuigiLetter, PauseMenu, LuigiLetter);
}  // namespace NrvPauseMenu

PauseMenu::PauseMenu()
    : LayoutActor("\x83\x7c\x81\x5b\x83\x59\x83\x81\x83\x6a\x83\x85\x81\x5b", true), _20(), _24(), _28(0.0f, 50.0f), _30(), mLuigiLetter(), _38(), mStageTitleOffset(0.0f, 39.0f),
      mCoinNumPos(0.0f, 0.0f), mStarPieceNumPos(0.0f, 0.0f), _54(true) {
#ifdef TARGET_PC
    mPortMenu = true;   // (14 Nov: a menu over a live scene: no re-projected in-between pictures while it is shown; LayoutActor::movement)
#endif
}

void PauseMenu::init(const JMapInfoIter& rIter) {
    initLayoutManager("PauseMenu", 1);
    MR::createAndAddPaneCtrl(this, "BG", 1);
    MR::createAndAddPaneCtrl(this, "PicStagebase", 1);
    MR::createAndAddPaneCtrl(this, "PicStagebase1", 1);
    MR::createAndAddPaneCtrl(this, "Counter", 1);
    MR::createAndAddPaneCtrl(this, "Stars", 2);
    MR::createAndAddPaneCtrl(this, "Coin", 1);
    MR::createAndAddPaneCtrl(this, "Piece", 1);
    MR::setFollowPos(&mCoinNumPos, this, "Coin");
    MR::setFollowTypeReplace(this, "Coin");
    MR::setFollowPos(&mStarPieceNumPos, this, "Piece");
    MR::setFollowTypeReplace(this, "Piece");

    if (::isStageHideScenarioTitle()) {
        const char* pPaneName = "ShaStageTitle";

        MR::createAndAddPaneCtrl(this, pPaneName, 1);
        MR::setFollowPos(&mStageTitleOffset, this, pPaneName);
        MR::setFollowTypeAdd(this, pPaneName);
    }

    MR::connectToSceneLayoutOnPause(this);

    _20 = new ButtonPaneController(this, "NBackUp", "BoxButton1", 0, true);
    _20->_22 = false;

    _24 = new ButtonPaneController(this, "NBackDown", "BoxButton2", 0, true);
    _24->_22 = false;

    MR::setTextBoxGameMessageRecursive(this, "NBackDown", ::isInvalidBackAstroDome() ? "PauseMenu_ButtonEndGame" : "PauseMenu_ButtonBackAstroDome");

    if (MR::isEqualStageName("AstroDome")) {
        MR::setTextBoxMessageRecursive(this, "ShaStageTitle", MR::getCurrentScenarioNameOnCurrentLanguage());
    } else {
        MR::setTextBoxMessageRecursive(this, "ShaStageTitle", MR::getCurrentGalaxyNameOnCurrentLanguage());
    }

    if (::isStageHideScenarioTitle()) {
        MR::hidePaneRecursive(this, "ScenarioTitle");
    } else {
        MR::setTextBoxMessageRecursive(this, "ScenarioTitle", MR::getCurrentScenarioNameOnCurrentLanguage());
    }

    updateStarPane();

    _30 = MR::createSysInfoWindow();

    MR::connectToSceneLayoutOnPause(_30);

    if (MR::getLuigiLetterGalaxyName()) {
        mLuigiLetter = MR::createLuigiLetterForMenu();
        mLuigiLetter->initWithoutIter();

        _38 = new ButtonPaneController(this, "LetterButton", "BoxButton3", 0, true);
        _38->_22 = false;
    } else {
        MR::hidePaneRecursive(this, "LetterButton");
    }

    initNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting));
}

void PauseMenu::appear() {
    MpMemProbe::log("pause");
    s32 coinNum = MR::getCoinNum();
    s32 starPieceNum = MR::getStarPieceNum();

    _54 = MR::isActiveDefaultGameLayout();

    if (_54) {
        MR::deactivateDefaultGameLayout();
    }

    MR::setTextBoxNumberRecursive(this, "ShaStarNum", MR::getPowerStarNum());
    MR::setTextBoxNumberRecursive(this, "ShaMarioNum", MR::getPlayerLeft());
    MR::setTextBoxNumberRecursive(this, "ShaCoinNum", coinNum);
    MR::setTextBoxNumberRecursive(this, "ShaPieceNum", starPieceNum);
    MR::setTextBoxGameMessageRecursive(this, "ShaMario", "PlayerLeft");

    wchar_t starPieceMessage[3];

    starPieceMessage[0] = MR::isStageAstroLocation() ? 0x57 : 0x3B;
    starPieceMessage[1] = 0x40;
    starPieceMessage[2] = '\0';

    MR::setTextBoxMessageRecursive(this, "ShaPiece", starPieceMessage);
    startPaneAnimWithoutButton("Appear");
    _20->appear();

    if (_24 != nullptr) {
        _24->appear();
    }

    if (MR::isOnLuigiHiding()) {
        _38->appear();
    }

    setNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting));
    LayoutActor::appear();

    const char* pCoinPanePos = "CPosition1";
    const char* pPiecePanePos = "PPosition1";

    if (coinNum >= 100) {
        pCoinPanePos = "CPosition3";
    } else if (coinNum >= 10) {
        pCoinPanePos = "CPosition2";
    }

    if (starPieceNum >= 1000) {
        pPiecePanePos = "PPosition4";
    } else if (starPieceNum >= 100) {
        pPiecePanePos = "PPosition3";
    } else if (starPieceNum >= 10) {
        pPiecePanePos = "PPosition2";
    }

    MR::copyPaneTrans(&mCoinNumPos, this, pCoinPanePos);
    MR::copyPaneTrans(&mStarPieceNumPos, this, pPiecePanePos);
}

void PauseMenu::draw() const {
    LayoutActor::draw();

    if (mLuigiLetter != nullptr) {
        mLuigiLetter->draw();
    }
#ifdef TARGET_PC
    port_settings_panel_draw(_38 != nullptr && !_38->isHidden() ? 1 : 0);
#endif
}

void PauseMenu::kill() {
    LayoutActor::kill();
#ifdef TARGET_PC
    port_vr_pause_menu(0);
    port_zoom_menu(0);
#endif

    if (_54) {
        MR::activateDefaultGameLayout();
    }
}

#ifdef TARGET_PC
extern "C" void port_log(const char* fmt, ...);
static bool sGlyphSettingsSeen = false;   // (SMG_GLYPH_TEST: Settings has been open since the pause menu came up)
#endif
void PauseMenu::control() {
#ifdef TARGET_PC
    static bool sButtonsHidden = false;
    if (settingsNativeOpen()) {
        sGlyphSettingsSeen = true;   // settingsnative: nothing of the pause menu runs while Settings is open; the settings file is still kept (port_zoom_menu)
        port_zoom_menu(0);
        if (!sButtonsHidden) {   // its own pills (Back, Quit / Back to the Observatory, the letter) stay out of the Settings' way, the leave question's too
            MR::hidePaneRecursive(this, "NBackUp");
            if (_24 != nullptr) MR::hidePaneRecursive(this, "NBackDown");
            if (_38 != nullptr) MR::hidePaneRecursive(this, "LetterButton");
            sButtonsHidden = true;
        }
        return;
    }
    if (sButtonsHidden) {
        MR::showPaneRecursive(this, "NBackUp");
        if (_24 != nullptr) MR::showPaneRecursive(this, "NBackDown");
        if (_38 != nullptr) MR::showPaneRecursive(this, "LetterButton");
        sButtonsHidden = false;
    }
#endif
#ifdef TARGET_PC
    {   // harness only (SMG_GLYPH_TEST=1, desktop): the pause menu's own message window, with its A picture (Type_Key), is opened 40 pause turns after the pause menu
        // came up and shut at 120 (before the settings test opens Settings at about 150); once Settings has been shut again it is opened a second time and left up.
        // It shows the A button picture the renderer patches (platform/src/port/handheld_ui.cpp), before and after a swap, in the same scene.
        static const bool glyphTest = [] { const char* e = getenv("SMG_GLYPH_TEST"); return e && *e && atoi(e) != 0; }();
        static int sTurn = 0, sBox = 0;
        if (glyphTest && isNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting))) {
            if (sGlyphSettingsSeen && sBox == 2) { sBox = 3; sTurn = 0; }   // Settings was shut: the second box after 40 more turns
            ++sTurn;
            if ((sBox == 0 || sBox == 3) && sTurn == 40) {
                _30->appear("PauseMenu_ConfirmEndGame", SysInfoWindow::Type_Key, SysInfoWindow::TextPos_Center, SysInfoWindow::MessageType_System);
                port_log("glyph test: message window %d up", sBox == 0 ? 1 : 2);
                if (sBox == 0) sBox = 1; else sBox = 4;
            } else if (sBox == 1 && sTurn == 120) {
                _30->disappear();
                port_log("glyph test: message window 1 shut");
                sBox = 2;
            }
        }
    }
#endif
    _20->update();

    if (_24 != nullptr) {
        _24->update();
    } else {
        _28.x = 0.0f;
        _28.y = 50.0f;
    }

    if (_38 != nullptr) {
        _38->update();
    }

    mStageTitleOffset.y = 39.0f;
#ifdef TARGET_PC
    // The VR layer shows its settings panel while the buttons take input.
    port_vr_pause_menu(isNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting)));
    port_zoom_menu(isNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting)));
#endif
}

void PauseMenu::updateStarPane() {
    GalaxyStatusAccessor accessor = MR::makeCurrentGalaxyStatusAccessor();
    const char* starPaneNames[7] = {"ShaStarA", "ShaStarB", "ShaStarC", "ShaStarD", "ShaStarE", "ShaStarF", "ShaStarG"};
    const char* starPicPaneNames[7] = {"PicStarA", "PicStarB", "PicStarC", "PicStarD", "PicStarE", "PicStarF", "PicStarG"};

    for (s32 i = 0; i < ARRAY_SIZEU(starPaneNames); i++) {
        MR::hidePaneRecursive(this, starPaneNames[i]);
    }

    if (::isStageHideScenarioTitle()) {
        return;
    }

    s32 extraStarCount = 0;
    bool isInvalidAstroDome = !MR::isOnGameEventFlagUseAstroDome();

    for (s32 i = 0; i < ARRAY_SIZEU(starPaneNames); i++) {
        if (i < accessor.getPowerStarNum()) {
            if (MR::hasPowerStarInCurrentStage(i + 1)) {
                if (i < accessor.getNormalScenarioNum()) {
                    MR::showPaneRecursive(this, starPaneNames[i]);
                } else {
                    MR::showPaneRecursive(this, starPaneNames[accessor.getNormalScenarioNum() + extraStarCount]);
                    extraStarCount++;
                }
            } else if (i < accessor.getNormalScenarioNum()) {
                MR::showPaneRecursive(this, starPaneNames[i]);
                MR::hidePaneRecursive(this, starPicPaneNames[i]);
            }
        }

        if (isInvalidAstroDome) {
            break;
        }
    }

    MR::startPaneAnim(this, "Stars", "Star", 1);

    f32 frame = isInvalidAstroDome ? 0.0f : accessor.getNormalScenarioNum() + extraStarCount - 1;
    MR::setPaneAnimFrameAndStop(this, "Stars", frame, 1);
}

void PauseMenu::startPaneAnimWithoutButton(const char* pAnimName) {
    MR::startPaneAnim(this, "BG", pAnimName, 0);
    MR::startPaneAnim(this, "PicStagebase", pAnimName, 0);
    MR::startPaneAnim(this, "PicStagebase1", pAnimName, 0);
    MR::startPaneAnim(this, "Stars", pAnimName, 0);
    MR::startPaneAnim(this, "Counter", pAnimName, 0);
}

bool PauseMenu::isPaneAnimStoppedWithoutButton() const {
    return MR::isPaneAnimStopped(this, "BG", 0) && MR::isPaneAnimStopped(this, "PicStagebase", 0) &&
           MR::isPaneAnimStopped(this, "PicStagebase1", 0) && MR::isPaneAnimStopped(this, "Stars", 0) && MR::isPaneAnimStopped(this, "Counter", 0);
}

void PauseMenu::forceToWaitAllButton() {
    _20->forceToWait();

    if (_24 != nullptr) {
        _24->forceToWait();
    }

    bool b = _38 == nullptr || _38->isHidden();

    if (!b) {
        _38->forceToWait();
    }
}

void PauseMenu::exeSelecting() {
#ifdef TARGET_PC
    if (settingsNativeOpen()) return;   // settingsnative: Start / Select / touch belong to the Settings menu, not the pause menu
#endif
    bool isPointingTrigger = _20->isPointingTrigger() || (_24 != nullptr && _24->isPointingTrigger()) ||
                             (!(_38 == nullptr || _38->isHidden()) && _38->isPointingTrigger());

    if (isPointingTrigger) {
        MR::startSystemSE("SE_SY_SELECT_PAUSE_ITEM", -1, -1);
    }

    bool isLetterHidden;

    if (_20->trySelect() || (_24 != nullptr && _24->trySelect()) || (!(isLetterHidden = (_38 == nullptr || _38->isHidden())) && _38->trySelect())) {
        setNerve(GET_NERVE(PauseMenu, PauseMenuNrvDecided));
        return;
    }

    bool isAnimStopped = isPaneAnimStoppedWithoutButton() && !_20->isAppearing() && (_24 == nullptr || !_24->isAppearing()) &&
                         ((_38 == nullptr || _38->isHidden()) || !_38->isAppearing());

    if (isAnimStopped && (MR::testCorePadTriggerPlus(0) || MR::testCorePadTriggerMinus(0))) {
        MR::startSystemSE("SE_SY_PAUSE_OFF", -1, -1);
        MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
        setNerve(GET_NERVE(PauseMenu, PauseMenuNrvDisappear));
    }
}

void PauseMenu::exeDecided() {
    if (_20->mIsSelected) {
        if (_20->isTimingForSelectedSe()) {
            MR::startSystemSE("SE_SY_PAUSE_OFF", -1, -1);
            MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
        }

        if (_20->isDecidedWait()) {
            setNerve(GET_NERVE(PauseMenu, PauseMenuNrvDisappear));
        }
    } else if (_24 != nullptr && _24->mIsSelected) {
        if (_24->isTimingForSelectedSe()) {
            MR::startSystemSE("SE_SY_PAUSE_OFF", -1, -1);
            MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
        }

        if (_24->isDecidedWait()) {
            if (::isInvalidBackAstroDome()) {
                setNerve(GET_NERVE(PauseMenu, PauseMenuNrvGameDataSave));
            } else {
                setNerve(GET_NERVE(PauseMenu, PauseMenuNrvConfirm));
            }
        }
    } else {
        bool isLetterHidden = _38 == nullptr || _38->isHidden();

        if (!isLetterHidden && _38->mIsSelected) {
            if (_38->isTimingForSelectedSe()) {
                MR::startSystemSE("SE_SY_FILE_SEL_UPPER_DECIDE", -1, -1);
                MR::startCSSound("CS_CLICK_CLOSE", nullptr, 0);
            }

            if (_38->isDecidedWait()) {
                setNerve(GET_NERVE(PauseMenu, PauseMenuNrvLuigiLetter));
            }
        }
    }
}

void PauseMenu::exeDisappear() {
    if (MR::isFirstStep(this)) {
        startPaneAnimWithoutButton("End");
        _20->disappear();

        if (_24 != nullptr) {
            _24->disappear();
        }

        bool b = _38 == nullptr || _38->isHidden();

        if (!b) {
            _38->disappear();
        }
    }

    if (isPaneAnimStoppedWithoutButton() && _20->isHidden() && (_24 == nullptr || _24->isHidden())) {
        bool b = _38 == nullptr || _38->isHidden();

        if (b) {
            kill();
        }
    }
}

void PauseMenu::exeConfirm() {
    if (MR::isFirstStep(this)) {
        const char* pMessageId = ::isInvalidBackAstroDome() ? "PauseMenu_ConfirmEndGame" : "PauseMenu_ConfirmBackAstroDome";

        _30->appear(pMessageId, SysInfoWindow::Type_YesNo, SysInfoWindow::TextPos_Center, SysInfoWindow::MessageType_System);
    }

    if (MR::isDead(_30)) {
        if (_30->isSelectedYes()) {
            MR::closeSystemWipeFadeWithCaptureScreen(::cBackSequenceFadeFrame);

            if (::isInvalidBackAstroDome()) {
                MR::requestChangeSceneTitle();
            } else {
                MR::requestChangeStageGoBackAstroDome();
            }
        } else {
            forceToWaitAllButton();
            setNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting));
        }
    }
}

void PauseMenu::exeGameDataSave() {
    if (MR::isFirstStep(this)) {
        GameSequenceFunction::startGameDataSaveSequence(false, true);
    }

    if (GameSequenceFunction::isActiveSaveDataHandleSequence()) {
        return;
    }

    if (GameSequenceFunction::isSuccessSaveDataHandleSequence()) {
        setNerve(GET_NERVE(PauseMenu, PauseMenuNrvConfirm));
    } else {
        forceToWaitAllButton();
        setNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting));
    }
}

void PauseMenu::exeLuigiLetter() {
    if (MR::isFirstStep(this)) {
        mLuigiLetter->appear();
    }

    if (MR::isDead(mLuigiLetter)) {
        forceToWaitAllButton();
        setNerve(GET_NERVE(PauseMenu, PauseMenuNrvSelecting));
    }
}
