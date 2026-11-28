// Galaxy 3.0 multiplayer: the ui module's game side (owner: ui). MpUi (MpGame.hpp, MpUi.hpp) on top of the pure state machine
// (MpMenuSequence.cpp), the overlays (MpToast.cpp) and the drawer (MpPanelDraw.cpp).
//
// A system-level object like SaveDataHandleSequence: GameSequenceDirector creates it after the resources are loaded, updates and
// draws it every frame in every scene (file select and game scenes). It owns one SysInfoWindow (the game's message window with
// Yes / No) and the panel; it never waits on the network (every step is a poll). While a menu, a window, the keyboard or the
// results card is up, MpUi::isMenuOpen() is true and, in a game scene, the player's controls are held (MR::offPlayerControl).
// With SMG_MP_MENU=0 (env) nothing here runs and the file select's 2P button opens the game's own 2P manual as before.
#define SETTINGS_NATIVE_DEFINES_STRONG 1   // port_settings_native_request is defined here: strong (T), see port/settings_native.h
#include "Game/MP/MpTextStrip.hpp"
#include <atomic>
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <stdio.h>
#include "port/settings_native.h"
#include <stdlib.h>
#include <string.h>

#include "Game/MP/MpCoop.hpp"   // MP3 coop: stamps are greetings
#include "Game/MP/MpDropInLogic.hpp"   // memfix: kKeepNapa, the scene heap's reserve
#include "Game/MP/MpUiLayouts.hpp"      // memfix: scene-scoped layout groups
#include "Game/MP/MpChat.hpp"   // MP3 chat: the quick ring, the Messages page
#include "Game/MP/MpControls.hpp"
#include "Game/MP/MpDemo.hpp"   // MP3 demo
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPanelDraw.hpp"
#include "Game/MP/MpSeriesLogic.hpp"   // lobby: the rematch ask
#include "Game/MP/MpPresent.hpp"   // MP3 ingame
#include "Game/MP/MpTeam.hpp"
#include "Game/MP/MpComposeKey.hpp"   // optfix
#include "Game/MP/MpUi.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/FileUtil.hpp"   // integ: the layout archives
#include "Game/Screen/SysInfoWindow.hpp"
#include "Game/System/Language.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MessageUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"   // flow: the galaxy being played (coop.cfg)
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "port/compat.h"   // port_host_time_ns
#include "port/mp_prof.h"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>

namespace {

namespace txt = MpMsgText;

bool sCreated = false;
SysInfoWindow* sWindow = nullptr;
bool sWindowOpen = false;
MpMenu::Dlg sWindowKind = MpMenu::Dlg::Ok;
int sSwallow = 0;            // frames of input ignored after the menu opens (the button that opened it)
bool sWasOpen = false;
bool sPlayerHeld = false;
bool sLastSwing = false;
int sHeld[4] = {0, 0, 0, 0};
bool sQuiet = false;         // (perf) nothing of ours on screen this frame: the panel, the window and the draw are skipped
// settingsnative: the Settings menu (opened from the entry screen) is drawn and read WITHOUT mp_mark_entered(): that flag also turns on the
// per-frame multiplayer hooks of MpGame (the core, its thread and sockets, mp.cfg) and the lazily made session object; Settings needs none
// of them, only this file's own update and draw. Set when it opens; cleared when nothing of ours is left on screen (its toast included).
bool sSettingsLive = false;
std::atomic<bool> sSettingsOpenAtom{false};   // keepawake33: cross-thread snapshot of MpMenu::sequence().settingsOpen() for entry.cpp OLED-safe keep-awake
int sLastPointed = -1;
bool sScreenChanged = false;   // flow: a new screen this frame (the pointer's move does not choose there)

// ---- galaxy names (the leaderboard)
int sScenarioCount[64];

int Strip(const wchar_t* in, char16_t* out, int cap) {
    // a game message without its tags (MpTextStrip: never reads past the end of a short message after a 0x1A)
    return MpTextStrip::strip(reinterpret_cast<const uint16_t*>(in), out, cap);
}

extern "C" int port_input_touched(void);
#ifdef TARGET_PC
extern "C" int port_turn_skipped_raw(void);   // (platform/src/port/perf.cpp: this turn draws no picture, 30 fps mode)
#endif
extern "C" uint64_t port_vita_input_held_buttons(void) __attribute__((weak));   // agent D (input.cpp): ~0 where there is no raw pad (the desktop)
extern "C" int port_settings_open_take(void) __attribute__((weak));       // agent D: the pause menu's Triangle sets it; taking it clears it
extern "C" int port_zoom_menu_active(void) __attribute__((weak));         // agent D: the pause menu is up (a single-player level)
// mppause: the session menu's zoom row (the pause menu's, perf.cpp / settings_panel.cpp): the menu's first screen on a handheld sets it up (L / R change the
// zoom and are not sent to the game), draws it after its own pictures (flag 2: the one-line "keeps running" note in its second row)
extern "C" void port_zoom_menu_session(int on) __attribute__((weak));
extern "C" void port_settings_panel_draw(int flags) __attribute__((weak));
namespace { bool sZoomSessionOn = false; }

struct GameIo : MpMenu::Io {
    uint64_t rawButtons() override {
        static const bool capTest = [] { const char* e = getenv("SMG_SETTINGS_CAPTURE_TEST"); return e && *e && atoi(e) != 0; }();   // harness only: a pad that is there and nothing held, so the capture screen shows on the desktop
        if (capTest) return 0;
        return port_vita_input_held_buttons ? port_vita_input_held_buttons() : ~0ull;
    }
    bool touched() override { return port_input_touched() != 0; }
    void sound(MpMenu::Se se) override {
        const char* name = nullptr;
        switch (se) {
        case MpMenu::Se::Cursor: name = "SE_SY_SELECT_PAUSE_ITEM"; break;
        case MpMenu::Se::Decide: name = "SE_SY_FILE_SEL_UPPER_DECIDE"; break;   // (uinative: the file select's big buttons)
        case MpMenu::Se::Cancel: name = "SE_SY_GALAXY_DECIDE_CANCEL"; break;
        case MpMenu::Se::Open: name = "SE_SY_FILE_SEL_TIPS_OPEN"; break;
        case MpMenu::Se::Close: name = "SE_SY_PAUSE_OFF"; break;
        case MpMenu::Se::Buzzer: name = "SE_SY_CAMERA_NG"; break;
        case MpMenu::Se::Success: name = "SE_SY_SAVE_SUCCESS"; break;
        case MpMenu::Se::Tab: name = "SE_SY_BUTTON_CURSOR_ON"; break;
        // touches: the game's own sounds, nothing new: the 1-Up's rising chime for a player who joins, the comet retry's soft "no" for
        // one who leaves, the short "complete" jingle for a Team Star
        case MpMenu::Se::Join: name = "SE_SY_ITEM_APPEAR"; break;
        case MpMenu::Se::Leave: name = "SE_SY_TALK_SELECT_NO"; break;
        case MpMenu::Se::Fanfare: name = "SE_SY_TOTAL_COMPLETE"; break;
        }
        if (name) MR::startSystemSE(name, -1, -1);
    }
    void dialogOpen(MpMenu::Dlg kind, const char* id) override {
        if (!sWindow) {   // memfix: no layouts in this scene (none could be made): answered at once below, the safe way (No / Ok)
            sWindowOpen = true;
            sWindowKind = kind;
            return;
        }
        if (!MR::isDead(sWindow)) sWindow->forceKill();
        sWindow->appear(id, kind == MpMenu::Dlg::YesNo ? SysInfoWindow::Type_YesNo : SysInfoWindow::Type_Key, SysInfoWindow::TextPos_Center,
                        SysInfoWindow::MessageType_System);
        if (kind == MpMenu::Dlg::YesNo) sWindow->setPortPadYesNo(true);   // setfix31: the pad answers it too (focus on No; Cross answers, Circle is No)
        sWindowOpen = true;
        sWindowKind = kind;
    }
    MpMenu::DlgState dialogState() override {
        if (!sWindowOpen) return MpMenu::DlgState::None;
        if (!sWindow) {   // (see dialogOpen)
            sWindowOpen = false;
            return sWindowKind == MpMenu::Dlg::YesNo ? MpMenu::DlgState::No : MpMenu::DlgState::Yes;
        }
        if (!MR::isDead(sWindow)) return MpMenu::DlgState::Open;
        sWindowOpen = false;
        if (sWindowKind == MpMenu::Dlg::YesNo) return sWindow->isSelectedYes() ? MpMenu::DlgState::Yes : MpMenu::DlgState::No;
        return MpMenu::DlgState::Yes;
    }
    int galaxyCount() override { return txt::galaxyCount(); }
    int galaxyName(int g, char16_t* out, int cap) override {
        const char* stage = txt::galaxyStage(g);
        if (!stage) return 0;
        char id[64];
        snprintf(id, sizeof id, "GalaxyName_%s", stage);
        return Strip(MR::getGameMessageDirect(id), out, cap);
    }
    int scenarioCount(int g) override {
        const char* stage = txt::galaxyStage(g);
        if (!stage || g >= 64) return 0;
        if (sScenarioCount[g] == 0) {
            int n = 0;
            for (int s = 1; s <= 8; s++) {
                char id[64];
                snprintf(id, sizeof id, "ScenarioName_%s%d", stage, s);
                if (!MR::getGameMessageDirect(id)) break;
                n = s;
            }
            sScenarioCount[g] = n > 0 ? n : -1;
        }
        return sScenarioCount[g] > 0 ? sScenarioCount[g] : 1;
    }
    int currentGalaxy() override {   // flow: (asked every 5 s while a session plays; -1 outside a galaxy, e.g. the Observatory)
        const char* stage = MR::getCurrentStageName();
        return stage ? txt::galaxyIndexOf(stage) : -1;
    }
    int scenarioName(int g, int s, char16_t* out, int cap) override {
        const char* stage = txt::galaxyStage(g);
        if (!stage) return 0;
        char id[64];
        snprintf(id, sizeof id, "ScenarioName_%s%d", stage, s);
        return Strip(MR::getGameMessageDirect(id), out, cap);
    }
};
GameIo sIo;

bool Enabled() {
    static const bool on = [] {
        const char* e = getenv("SMG_MP_MENU");
        return !(e && *e) || atoi(e) != 0;
    }();
    return on;
}

void SetLanguage() { txt::setLanguage(txt::languageIndex(MR::getCurrentLanguagePrefix())); }

// A direction: once on the press, then again every 6 frames after 24.
bool Repeat(int k, bool held, bool trigger) {
    if (!held && !trigger) {
        sHeld[k] = 0;
        return false;
    }
    ++sHeld[k];
    if (trigger || sHeld[k] == 1) return true;
    return sHeld[k] > 24 && (sHeld[k] - 24) % 6 == 0;
}

MpMenu::Input ReadInput() {
    MpMenu::Input in;
    const float sy = MR::getSubPadStickY(0), sx = MR::getSubPadStickX(0);
    in.up = Repeat(0, MR::testCorePadButtonUp(0) || sy > 0.6f, MR::testCorePadTriggerUp(0) || MR::testSubPadStickTriggerUp(0));
    in.down = Repeat(1, MR::testCorePadButtonDown(0) || sy < -0.6f, MR::testCorePadTriggerDown(0) || MR::testSubPadStickTriggerDown(0));
    in.left = Repeat(2, sx < -0.6f, MR::testCorePadTriggerLeft(0) || MR::testSubPadStickTriggerLeft(0));
    in.right = Repeat(3, sx > 0.6f, MR::testCorePadTriggerRight(0) || MR::testSubPadStickTriggerRight(0));
    in.ok = MR::testCorePadTriggerA(0);
    in.back = MR::testCorePadTriggerB(0);
    in.tri = MR::testSubPadTriggerC(0);   // (Triangle is the Nunchuk's C on the handhelds)
    in.start = MR::testCorePadTriggerPlus(0);
    // Square shakes the remote on the handhelds (a spin): its first swing frame, or Select (-)
    const bool swing = MR::isCorePadSwing(0);
    in.sq = (swing && !sLastSwing) || MR::testCorePadTriggerMinus(0);
    sLastSwing = swing;
    return in;
}

// In a game scene, the player stands still while a menu or a window is up (the world keeps running).
void HoldPlayer(bool want) {
    MarioActor* actor = nullptr;
    if (MR::isExistSceneObj(SceneObj_MarioHolder)) {
        MarioHolder* holder = MR::getMarioHolder();
        actor = holder ? holder->getMarioActor() : nullptr;
    }
    if (!actor) {
        sPlayerHeld = false;   // (no player, or the scene changed: nothing to give back)
        return;
    }
    if (want && !sPlayerHeld) {
        if (!MR::isOffPlayerControl()) {
            MR::offPlayerControl();
            sPlayerHeld = true;
            mp_log("ui: player held");
        }
    } else if (!want && sPlayerHeld) {
        if (MR::isOffPlayerControl()) MR::onPlayerControl(false);
        sPlayerHeld = false;
        mp_log("ui: player released");
    }
}

MpPanel::Model sModel;
MpPanel::Strip sStrip;   // MP3 celebrate: the party strip beside the life counter
MpPanel::Plan sMenuPlan;
MpPanel::ToastPlan sToastPlan;
MpPanel::StripPlan sStripPlan;   // memfix: the toasts and the strip need a fraction of the menu's room
MpPanel::Scene sMenuScene, sToastScene, sStripScene;
bool sStripOn = false;   // the party strip has something to draw this frame
MpPanel::Edge sEdgeModel;   // MP3 ingame: off-screen players' heads at the picture's edge
MpPanel::Badge sBadgeModel; // MP3 ingame: the watch badge
bool sEdgeOn = false, sBadgeOn = false;
MpPanel::Prompt sPrompt;     // MP3 ingame: the bubble's button prompt
bool sPromptOn = false;
MpPanel::StarRow sStarRow;   // MP3 ingame: Play Together's Power Star: the faces under "You got a Star!"
bool sStarRowOn = false;
MpPanel::Ring sRingModel;   // MP3 chat: the quick ring (hold Select in play)
bool sRingOn = false;
uint32_t sFrames = 0;

// ---- memfix: the layouts of this scene (MpUiLayouts.hpp). The SysInfoWindow (Yes / No dialogs) is a group of its own.
bool sEager = false;           // SMG_MP_UI_EAGER=1 (measuring): everything at boot in the stationed heap, as before memfix
volatile bool sInitBusy = false;   // integ: between sceneBegin (the init thread) and the scene's first update: the main thread makes no layouts then (two threads in one scene heap / one file loader)
bool sGaveUp = false;          // integ: this session scene's init-thread creation was refused: the main thread does not retry (a mount there races the scene's own init)
bool sNoLayoutsLogged = false; // (per scene) "wanted but cannot be made" said once
bool sSceneLive = false;       // a scene is between its start and its end (its heap exists; none in the gaps between scenes)

void CreateDialog() {
    sWindow = MR::createSysInfoWindowExecuteWithChildren();
    sWindow->kill();
}
void ForgetDialog() {
    sWindow = nullptr;
    sWindowOpen = false;
}
constexpr uint32_t kDialogBudget = 96 * 1024;
const char* const kDialogArchives[] = {"SysInfoWindow", "IconAButton"};   // (integ: SysInfoWindow::init and its A-button icon)
const MpUiLayouts::Group kDialogGroup = {"dialog", MpUiLayouts::kMenu | MpUiLayouts::kSession | MpUiLayouts::kSettings, 1, kDialogBudget, CreateDialog, ForgetDialog,
                                         kDialogArchives, (uint8_t)(sizeof kDialogArchives / sizeof kDialogArchives[0])};

// integ: a LayoutData archive by the prefix initLayoutManager takes: the file the game mounts for it (the same name rules as
// LayoutManager's constructor and ResourceHolderManager::createAndAddInner).
bool LayoutArchivePath(const char* prefix, char* path, size_t cap) {
    return MR::makeLayoutArchiveFileNameFromPrefix(path, (u32)cap, prefix, false);
}
bool LayoutArchiveMounted(const char* prefix) {
    char path[128];
    if (!LayoutArchivePath(prefix, path, sizeof path)) return true;   // (no such file: the game's own fallback decides; nothing to wait for)
    const bool ok = MR::isLoadedFile(path);
    if (!ok) mp_log("mem ui archive %s (%s) is not loaded", prefix, path);
    return ok;
}

const char* KindName(MpUiLayouts::Kind k) { return k == MpUiLayouts::KIND_MENU ? "menu" : (k == MpUiLayouts::KIND_SESSION ? "session" : (k == MpUiLayouts::KIND_SETTINGS ? "settings" : (k == MpUiLayouts::KIND_TOAST ? "toast" : "none"))); }

uint32_t FreeOf(JKRHeap* h) {
    const s32 f = h ? h->getTotalFreeSize() : 0;
    return f > 0 ? static_cast<uint32_t>(f) : 0u;
}

// Make this scene's layouts now (in the scene heap), when the free size allows; true when they are there. `why`: the log.
bool EnsureLayouts(const char* why, bool initThread = false) {
    if (sEager || MpUiLayouts::ready()) return true;
    // a session that began after this scene loaded (the scene had none): it gets the session's layouts, guarded by the free size
    if (MpUiLayouts::sceneKind() == MpUiLayouts::KIND_NONE && sSceneLive && MpGame::sessionHeld()) MpUiLayouts::beginScene(MpUiLayouts::KIND_SESSION);
    if (MpUiLayouts::sceneKind() == MpUiLayouts::KIND_NONE) return false;
    JKRHeap* napa = MR::getSceneHeapNapa();
    if (!napa) return false;
    const uint32_t before = FreeOf(napa);
    const int64_t t0 = port_host_time_ns();
    int layouts = 0;
    const char* missingArc = nullptr;
    MpUiLayouts::Result r;
    {
        // The layouts go to the scene heap and die with it. NOT MR::CurrentHeapRestorer: it holds the heap mutex, and a layout whose
        // archive holder does not exist yet is built by the MAIN thread while this (scene init) thread waits for it: found as a
        // deadlock in the first harness run. On the scene init thread the current heap already is the scene heap; on the main thread
        // (the file select's menu) nothing else allocates while a menu opens, so the plain switch is safe.
        JKRHeap* previous = MR::getCurrentHeap();
        if (previous != napa) MR::becomeCurrentHeap(napa);
        // integ (B1): on the scene init thread nothing may be mounted from here (the game's holder is then built by the main thread on a
        // null archive: a crash): a layout archive the archive phase did not get mounted means no multiplayer UI in this scene
        r = MpUiLayouts::ensure(before, MpDropIn::kKeepNapa, &layouts, initThread ? LayoutArchiveMounted : nullptr, &missingArc);
        if (previous != napa) MR::becomeCurrentHeap(previous);
    }
    if (r == MpUiLayouts::R_CREATED) {
        const uint32_t after = FreeOf(napa);
        mp_log("mem ui layouts kind=%s why=%s count=%d bytes=%u napa-free=%u->%u ms=%d", KindName(MpUiLayouts::sceneKind()), why, layouts,
               static_cast<unsigned>(before - after), static_cast<unsigned>(before), static_cast<unsigned>(after),
               static_cast<int>((port_host_time_ns() - t0) / 1000000));
    } else if (r == MpUiLayouts::R_NO_ARCHIVE && !sNoLayoutsLogged) {
        sNoLayoutsLogged = true;
        mp_log("mem ui layouts REFUSED kind=%s why=%s: layout archive %s is not mounted (not asked for in the archive phase): no multiplayer UI in this scene",
               KindName(MpUiLayouts::sceneKind()), why, missingArc ? missingArc : "?");
    } else if (r == MpUiLayouts::R_NO_MEMORY && !sNoLayoutsLogged) {
        sNoLayoutsLogged = true;
        mp_log("mem ui layouts REFUSED kind=%s why=%s napa-free=%u need=%u reserve=%u: no multiplayer UI in this scene", KindName(MpUiLayouts::sceneKind()),
               why, static_cast<unsigned>(before), static_cast<unsigned>(MpUiLayouts::bytesNeeded()), static_cast<unsigned>(MpDropIn::kKeepNapa));
    }
    return MpUiLayouts::ready();
}

// Something of ours has to be shown or answered (the pieces that need layouts).
bool Wanted() {
    MpMenu::Sequence& seq = MpMenu::sequence();
    return seq.isOpen() || seq.isBusy() || MpOverlay::resultsShowing() || MpOverlay::waitingShowing() || MpOverlay::starCardShowing() || sStripOn || sRingOn ||
           MpOverlay::toastCount() > 0 || sWindowOpen;
}

// optfix item 3: the in-play panels (strip, edge heads, badge, star row, prompt) are composed again only when their models changed
// (MpComposeKey.hpp), and only on frames where nothing else shares the scenes. The last composition stands otherwise: MpPanelDraw::update
// and draw still run every frame. SMG_MP_UI_RECOMPOSE=1 composes every frame as before.
uint64_t sComposeKey = 0;
bool sComposeKept = false;   // the plans hold a finished in-play-only composition for sComposeKey
bool ComposeReuseOn() {
    static const bool off = [] { const char* e = getenv("SMG_MP_UI_RECOMPOSE"); return e && *e && atoi(e) != 0; }();
    return !off;
}

// The scenes of this frame: the waiting screen / results card, the menu, the toasts.
bool Compose() {
    MpPanel::beginScene(&sMenuScene);
    MpPanel::beginScene(&sToastScene);
    MpPanel::beginScene(&sStripScene);
    MpPanel::begin(&sMenuPlan);
    MpPanel::begin(&sToastPlan);
    MpPanel::begin(&sStripPlan);
    MpPanel::FontMetrics fonts[MpPanel::FONT_COUNT];
    if (!MpPanelDraw::fonts(fonts)) return false;
    const MpPanel::Screen screen = MpPanelDraw::screen();
    const float bob = (sFrames % 180) / 180.0f;
    MpMenu::Sequence& seq = MpMenu::sequence();
    if (sStripOn) MpPanel::composeStrip(sStrip, screen, fonts, bob, &sStripScene, &sStripPlan);
    if (sEdgeOn) MpPanel::composeEdges(sEdgeModel, screen, &sStripScene, &sStripPlan);   // MP3 ingame
    if (sBadgeOn) MpPanel::composeBadge(sBadgeModel, screen, fonts, &sStripScene, &sStripPlan);
    if (sStarRowOn) MpPanel::composeStarRow(sStarRow, screen, fonts, &sStripScene, &sStripPlan);
    if (sPromptOn) MpPanel::composePrompt(sPrompt, screen, fonts, &sStripPlan);
    if (MpOverlay::buildResults(&sModel) || MpOverlay::buildStarCard(&sModel) || MpOverlay::buildWaiting(&sModel) || MpOverlay::buildHeadStart(&sModel)) MpPanel::compose(sModel, screen, fonts, bob, &sMenuScene, &sMenuPlan);
    if (seq.isOpen()) {
        seq.buildModel(&sModel);
        MpPanel::compose(sModel, screen, fonts, bob, &sMenuScene, &sMenuPlan);
        if (sMenuPlan.overflow) mp_log("ui: panel OVERFLOW on %s", MpMenu::screenName(seq.screen()));
    }
    const int n = MpOverlay::toastCount();
    if (n > 0) MpPanel::composeToasts(MpOverlay::toasts(), n, screen, fonts, &sToastScene, &sToastPlan);
    if (sRingOn) MpPanel::composeRing(sRingModel, screen, fonts, &sToastPlan);   // MP3 chat
    return !sMenuPlan.overflow && !sToastPlan.overflow && !sStripPlan.overflow;
}

// Compose() unless the in-play panels alone are shown and unchanged since the composition the plans still hold.
// Returns true iff the previous composition was reused unchanged (false whenever Compose() ran).
bool ComposeOrKeep() {
    MpMenu::Sequence& seq = MpMenu::sequence();
    const bool alone = ComposeReuseOn() && !seq.isOpen() && !seq.isBusy() && !MpOverlay::resultsShowing() && !MpOverlay::waitingShowing() &&
                       !MpOverlay::starCardShowing() && !MpOverlay::headStartShowing() && MpOverlay::toastCount() == 0 && !sRingOn;
    if (!alone) {
        sComposeKept = false;
        Compose();
        return false;
    }
    MpComposeKey::Inputs in;
    in.stripOn = sStripOn, in.edgeOn = sEdgeOn, in.badgeOn = sBadgeOn, in.starRowOn = sStarRowOn, in.promptOn = sPromptOn;
    in.strip = &sStrip, in.edge = &sEdgeModel, in.badge = &sBadgeModel, in.starRow = &sStarRow, in.prompt = &sPrompt;
    in.screen = MpPanelDraw::screen();
    in.frame = sFrames;
    const uint64_t key = MpComposeKey::key(in);
    if (sComposeKept && key == sComposeKey) return true;   // the same composition: the plans and scenes stand
    sComposeKept = Compose();
    sComposeKey = key;
    return false;
}

// vitabtn: a finger on the front screen (the Vita; false elsewhere). The pointer is only trusted to choose rows and heads while one is down:
// on the file select the sticks move the pointer too, and a drifting pointer must not fight the D-pad's cursor.
extern "C" int port_input_touched(void);
bool sFinger = false, sTouchFresh = false;
int sTouchSwallow = 0;   // frames in which the tap that followed a touch on the guide band is not a Cross as well

bool PointerSquare(float* x, float* y) {
    if (!MR::isStarPointerInScreen(0)) return false;
    const TVec2f* p = MR::getStarPointerScreenPosition(0);
    if (!p) return false;
    const MpPanel::Screen scr = MpPanelDraw::screen();
    *x = p->x * (scr.width / static_cast<float>(MR::getScreenWidth()));
    *y = p->y;
    return true;
}

// vitabtn: the row, head or tile of the open page under the finger (model order), -1 none
int PagePointed() {
    float x, y;
    if (!sFinger || !PointerSquare(&x, &y)) return -1;
    return MpPanel::hitCell(sMenuPlan, x, y);
}

// MP3 chat: the Messages page's tile under the star pointer (touch on the Vita), -1 none
int GridPointed() {
    if (MpMenu::sequence().screen() != MpMenu::S_MESSAGES || !MR::isStarPointerInScreen(0)) return -1;
    const TVec2f* p = MR::getStarPointerScreenPosition(0);
    if (!p) return -1;
    const MpPanel::Screen scr = MpPanelDraw::screen();
    const float sx = p->x * (scr.width / static_cast<float>(MR::getScreenWidth()));
    return MpPanel::hitCell(sMenuPlan, sx, p->y);
}

uint32_t sSetOpenFree = 0;   // the scene Napa's free size just after the Settings opened in this scene (0: not open)
void SettingsHeapLine(const char* what, uint32_t cost) {   // free scene Napa, its largest block, the file cache, and what the set cost
    JKRHeap* napa = MR::getSceneHeapNapa();
    JKRHeap* cache = SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap;
    auto kib = [](JKRHeap* h, bool total) -> long { return h ? static_cast<long>((total ? h->getTotalFreeSize() : h->getFreeSize()) / 1024) : -1L; };
    mp_log("settings: heap %s galaxy=%s napa_free=%ldK napa_largest=%ldK cache_free=%ldK cache_largest=%ldK set_cost=%uB", what, MR::getCurrentStageName(), kib(napa, true), kib(napa, false),
           kib(cache, true), kib(cache, false), static_cast<unsigned>(cost));
}

// settingsnative: the Settings' own memory (the model's strings and lists, the menu's vectors) comes from the host, never from whatever game heap is
// current at the pause: in some levels that is a small solid heap (Egg Star, Honeybee Kingdom: JKRSolidHeap::do_alloc panicked inside NativeModel::Open).
// With no current heap `new` takes the host allocator (JKRHeap.cpp gameAlloc); frees find their own heap.
struct HostAllocScope {
    JKRHeap* mPrev;
    bool mOn;
    explicit HostAllocScope(bool on) : mPrev(JKRHeap::getCurrentHeap()), mOn(on) { if (mOn) JKRHeap::setCurrentHeap(nullptr); }
    ~HostAllocScope() { if (mOn) JKRHeap::setCurrentHeap(mPrev); }
};

}  // namespace

namespace MpUi {

bool featureEnabled() { return Enabled(); }

const wchar_t* balloonText() {
    static char16_t buf[48];
    SetLanguage();
    txt::format(buf, 48, MP_T_FILESEL_BALLOON, txt::Args());
    return reinterpret_cast<const wchar_t*>(buf);
}

const wchar_t* fileSelectLabel() {
    static char16_t buf[48];
    SetLanguage();
    txt::format(buf, 48, MP_T_FILESEL_ICON, txt::Args());
    return reinterpret_cast<const wchar_t*>(buf);
}

void createSystem() {
    if (sCreated) return;   // settingsnative: the system is always made (the pause menu opens the Settings with the MP menu off too); only the MP openers test Enabled()
    sCreated = true;
    SetLanguage();
    // memfix: no layout is made here any more (they were 38 + the dialog window in the stationed Napa heap, for every player, always):
    // the groups are registered, and made per scene when a scene needs them (EnsureLayouts). SMG_MP_UI_EAGER=1 keeps the old
    // behaviour for measuring.
    MpPanelDraw::registerLayouts();
    MpUiLayouts::add(&kDialogGroup);
    MpMenu::sequence().init(&sIo);
    MpOverlay::reset();
    JKRHeap* stationed = MR::getStationedHeapNapa();
    const uint32_t before = FreeOf(stationed);
    int layouts = 0;
    const char* e = getenv("SMG_MP_UI_EAGER");
    if (e && *e && atoi(e) != 0) {
        sEager = true;
        MpUiLayouts::ensureAll(&layouts);
    }
    mp_log("mem ui boot stationed-napa-free=%u->%u layouts=%d eager=%d", static_cast<unsigned>(before), static_cast<unsigned>(FreeOf(stationed)), layouts,
           sEager ? 1 : 0);
    MpOverlay::setPolicy(true);   // MP3 ingame: a toast only for what the player cannot see
    mp_log("ui: system ready (%s)", MR::getCurrentLanguagePrefix());
}

// MP3 memfix: a scene starts (GameScene::init after placement) / ends (GameScene destructor). A session scene makes its layouts now
// (the scene init thread, the scene heap: never mid-play); the file select (KIND_MENU, set by onFileSelect) waits for a first need.
void sceneBegin(bool session) {
    if (!sCreated || sEager) return;
    sNoLayoutsLogged = false;
    sGaveUp = false;
    sInitBusy = true;
    sSceneLive = true;
    if (MpUiLayouts::sceneKind() != MpUiLayouts::KIND_MENU) MpUiLayouts::beginScene(session ? MpUiLayouts::KIND_SESSION : MpUiLayouts::KIND_NONE);
    if (MpUiLayouts::sceneKind() == MpUiLayouts::KIND_SESSION && !EnsureLayouts("scene", true)) sGaveUp = true;
}

// integ (B1): the scene's archive-request phase (SceneDataInitializer::startStageFileLoadAfterScenarioSelected) of a scene that starts with a
// session: the layout archives its UI needs, mounted into `heap` (null: the game's apropos heap) before any actor is made. Nothing without
// a session (the file select's menu mounts its own, on the main thread, when it opens).
void addArchives(void* heapPtr) {
    if (sEager) return;
    // settingsnative: a single-player level whose pause menu can open the native Settings (the lean set, KIND_SETTINGS) gets the archives that set
    // needs mounted now: opening it later builds the layouts on the main thread, and an archive that was not mounted by then is a crash.
    const port::settings::NativeHost* setHost = port::settings::NativeHostGet();
    const bool mpScene = MpGame::sessionHeld();
    const bool forSettings = !mpScene && setHost && setHost->available;
    if (!mpScene && !forSettings) return;
    JKRHeap* heap = static_cast<JKRHeap*>(heapPtr);
    const char* names[16];
    const int n = MpUiLayouts::archivesWanted(forSettings ? MpUiLayouts::KIND_SETTINGS : MpUiLayouts::KIND_SESSION, names, 16);
    int asked = 0;
    for (int i = 0; i < n; i++) {
        char path[128];
        if (!LayoutArchivePath(names[i], path, sizeof path) || MR::isLoadedFile(path)) continue;
        MR::mountAsyncArchive(path, heap != nullptr ? heap : MR::getAproposHeapForSceneArchive(0.03f));
        asked++;
        mp_log("mem ui archive request %s (%s) loaded-now=%d", names[i], path, MR::isLoadedFile(path) ? 1 : 0);
    }
    if (n > 0) mp_log("mem ui archives wanted=%d requested=%d heap=%s", n, asked, heap != nullptr ? "scene-gddr" : "apropos");
}

void sceneRunning() { sInitBusy = false; }

void sceneEnd() {
    if (!sCreated || sEager) return;
    sGaveUp = false;
    sInitBusy = false;
    if (MpMenu::sequence().settingsOpen()) {
        HostAllocScope hostScope(true);
        MpMenu::sequence().close();   // settingsnative: the scene goes: what is changed is saved
    }
    sSettingsLive = false;
    sSettingsOpenAtom.store(false, std::memory_order_relaxed);
    sSetOpenFree = 0;
    MpUiLayouts::endScene();
    sSceneLive = false;
    sQuiet = true;   // (no layout is left: the next scene's first frame decides again)
    sComposeKept = false;
    sLastPointed = -1;
}

void PollPauseRequest();   // settingsnative (below)

void updateSystem() {
    sSettingsOpenAtom.store(sCreated && MpMenu::sequence().settingsOpen(), std::memory_order_relaxed);
    {   // mppause: the zoom row while the session menu's first screen is up (every turn: port_zoom_menu counts the idle turns for the save); off the turn it goes
        const bool want = sCreated && port_zoom_menu_session && MpMenu::sequence().zoomRowShown();
        if (want || sZoomSessionOn) {
            if (port_zoom_menu_session) port_zoom_menu_session(want ? 1 : 0);
            sZoomSessionOn = want;
        }
    }
    if (sCreated && !sSettingsLive) {   // settingsnative
        if (!mp_active()) PollPauseRequest();
        else if (port_settings_open_take) port_settings_open_take();   // (a request made before a session started is dropped, never taken later)
    }
    if ((!mp_entered() && !sSettingsLive && !MpDemo::live()) || !sCreated) return;   // (MP3 demo: the puppet demo shows toasts, the card and the strip without a session)   // MP3 netpack: never entered in this boot: no menu, toast or session to watch
    HostAllocScope hostScope(sSettingsLive);   // (the Settings' memory is the host's)
    MP_PROF_SCOPE(MP_PROF_TOTAL);
    MP_PROF_SCOPE(MP_PROF_UI_UPDATE);
    MpMenu::Sequence& seq = MpMenu::sequence();
    {   // harness only (SMG_SETTINGS_SWAP_TEST=1): the pause menu's Settings, a while after it opened, swaps Jump to Circle and closes (the picture-follow check)
        static const bool swapTest = [] { const char* e = getenv("SMG_SETTINGS_SWAP_TEST"); return e && *e && atoi(e) != 0; }();
        static int sAge = 0;
        if (swapTest && seq.settingsOpen() && ++sAge == 150) seq.settingsTestSwap();
    }
    const bool open = seq.isOpen();
    const bool menuBefore = open || seq.isBusy();   // smallfix: a press that works the menu or a window is not a press on the results card
    if (open && !sWasOpen) sSwallow = 8;
    sWasOpen = open;
    MpMenu::Input in;
    const bool wantInput = seq.isBusy() || MpOverlay::resultsShowing();
    if (MpDemo::enabled() && MpDemo::menuInput(&in, seq)) {   // MP3 demo (SMG_MP_DEMO=menus): the tour presses, not the player
    } else if (wantInput && !mp_dialog_active()) {
        in = ReadInput();
        // vitabtn: a finger put down on a button of the guide band is that button (Select / Back / the third and fourth choices). The pointer
        // may only be valid from the second frame of a touch: the first frame with one decides (sTouchFresh), once per touch.
        const bool finger = port_input_touched() != 0;
        bool guide = false;
        if (finger && !sFinger) sTouchFresh = true;
        if (!finger) sTouchFresh = false;
        float gx, gy;
        if (finger && sTouchFresh && seq.isOpen() && PointerSquare(&gx, &gy)) {
            sTouchFresh = false;
            switch (MpControls::guideInput(MpPanel::hitGuide(sMenuPlan, gx, gy))) {
            case MpControls::G_OK: in.ok = guide = true; break;
            case MpControls::G_BACK: in.back = guide = true; break;
            case MpControls::G_TRI: in.tri = guide = true; break;
            case MpControls::G_SQ: in.sq = guide = true; break;
            case MpControls::G_START: in.start = guide = true; break;   // flow (one door): a touch on the gathering's START
            default: break;
            }
            if (guide) sTouchSwallow = 16;
            else if (seq.settingsOpen() && seq.screen() >= MpMenu::S_SET_MAIN && seq.screen() <= MpMenu::S_SET_CAPTURE) {
                // settingsnative: an arrow of the selected row is Left / Right (and not a Cross); the first tap on another row only selects it
                const int dir = MpPanel::hitArrow(sMenuPlan, gx, gy);
                if (dir) {
                    (dir < 0 ? in.left : in.right) = true;
                    sTouchSwallow = 16;
                } else if (seq.firstTapPoints(MpPanel::hitCell(sMenuPlan, gx, gy))) {
                    sTouchSwallow = 16;
                }
            }
            else if (seq.screen() == MpMenu::S_MESSAGES && seq.firstTapPoints(GridPointed())) sTouchSwallow = 16;   // uxfix #8: the first tap on a tile points it; the second sends
        }
        if (sTouchSwallow > 0) {
            --sTouchSwallow;
            if (!guide) in.ok = false;   // (the tap's own Cross comes a few frames after the finger went down)
        }
        sFinger = finger;
    } else {
        sFinger = port_input_touched() != 0;
    }
    if (sSwallow > 0) {
        --sSwallow;
        in = MpMenu::Input();
    }
    seq.update(in);
    if (open && !seq.isOpen()) port_input_discard_pause_request();   // fix2: the Start that closed the menu does not open it again
    {   // smallfix: the results card: a finger on a row points it (the leader's cursor follows); a tap anywhere else presses nothing
        static int sCardFingerAge = 999;
        static bool sCardFingerLive = false;
        bool ok = in.ok && !menuBefore && !seq.isBusy();
        if (MpOverlay::resultsShowing() && !seq.isOpen()) {
            if (port_input_touched() != 0) {
                sCardFingerAge = 0;
                float fx, fy;
                sCardFingerLive = PointerSquare(&fx, &fy) && MpOverlay::resultsPoint(MpPanel::hitCell(sMenuPlan, fx, fy));
            } else if (sCardFingerAge < 999) {
                ++sCardFingerAge;
            }
            if (sCardFingerAge <= 16 && !sCardFingerLive) ok = false;
        } else {
            sCardFingerAge = 999;
        }
        bool testDown = false;
#if defined(TARGET_PC) && !defined(__vita__)
        {   // desktop test hook: SMG_MP_TEST_RACE_LEAVE="<n>[:<secs>]": on the n-th results card with choices this console is shown (counting from 1),
            // <secs> (default 6) after it came up, the OK button is pressed for it: on a guest that is the card's only live row, Leave Race, and the
            // press goes through resultsInput / takeResultsLeave / leaveByUs exactly as a player's. Nothing without the env.
            static int sInit = -1, sNth = 0, sCards = 0;
            static uint32_t sDelayFrames = 360, sCardFrame = 0;
            static bool sWasUp = false;
            if (sInit < 0) {
                const char* e = getenv("SMG_MP_TEST_RACE_LEAVE");
                sInit = (e && *e && atoi(e) > 0) ? 1 : 0;
                if (sInit) {
                    sNth = atoi(e);
                    const char* c = strchr(e, ':');
                    if (c) sDelayFrames = (uint32_t)(atof(c + 1) * 60.0);
                    mp_log("race test leave armed: card %d, %u frames after it came up", sNth, (unsigned)sDelayFrames);
                }
            }
            if (sInit == 1) {
                const bool up = MpOverlay::resultsShowing() && MpOverlay::resultsHaveChoices();
                if (up && !sWasUp) { ++sCards; sCardFrame = sFrames; }
                sWasUp = up;
                if (up && sCards == sNth && sFrames - sCardFrame >= sDelayFrames) {
                    static int sLogged = -1;
                    if (sLogged != sCards) { sLogged = sCards; mp_log("race test leave %d", sCards); }
                    ok = true;
                    testDown = true;   // (the guest's cursor starts on Race Again?: move to Leave Race in the same press)
                }
            }
        }
#endif
        MpOverlay::resultsInput(in.up, in.down || testDown, ok, mp_is_host());
        if (MpOverlay::takeResultsLeave()) seq.leaveByUs();
        if (MpOverlay::takeResultsAsk() && !MpChat::sendPhrase(MpSeriesLogic::kAskPhrase)) MpOverlay::resultsAskFailed();   // lobby: a guest asks the leader for a rematch
        if (MpOverlay::resultsShowing() && !mp_active() && !seq.reconnecting()) MpOverlay::dismissResults();   // (the session is over: no card)
    }
    MpOverlay::update(in.ok, seq.reconnecting());
    ++sFrames;
    // (perf) single-player, and a session with nothing of ours on screen: the sequence above still watches the session (joins,
    // leaves, host changes queue their toasts), but no panel is composed, run or drawn while every layout is dead
    sStripOn = MpTeam::stripModel(&sStrip) && !MpOverlay::resultsShowing();   // (race: not under the results card)   // MP3 celebrate (a bool read without a session)
    sRingOn = MpChat::ringModel(&sRingModel);  // MP3 chat (one compare when the ring is closed)
    sEdgeOn = MpNameTags::edgeModel(&sEdgeModel);     // MP3 ingame
    sBadgeOn = MpSpectate::badgeModel(&sBadgeModel);
    sStarRowOn = MpTeam::starRowModel(&sStarRow);
    sPromptOn = MpBubble::promptModel(&sPrompt);
    // memfix: the scene's layouts are made on the first need (the file select; a session scene made them at its start). A scene
    // that has none and cannot make them shows nothing: a menu left open would hold the input unseen, so it is closed.
    if (!sEager && !MpUiLayouts::ready() && Wanted() && (sInitBusy || sGaveUp || !EnsureLayouts("need"))) {
        if (!sInitBusy && (seq.isOpen() || seq.isBusy())) {
            seq.close();
            mp_log("ui: menu closed (this scene has no layouts)");
        }
        sQuiet = true;
        sComposeKept = false;
        sLastPointed = -1;
        return;
    }
    sQuiet = !seq.isOpen() && !seq.isBusy() && !MpOverlay::resultsShowing() && !MpOverlay::waitingShowing() && !MpOverlay::headStartShowing() && !MpOverlay::starCardShowing() && !sStripOn && !sRingOn && !sEdgeOn && !sBadgeOn && !sStarRowOn && !sPromptOn && MpOverlay::toastCount() == 0 &&
             !sWindowOpen && (!sWindow || MR::isDead(sWindow)) && !sPlayerHeld && sSwallow == 0 &&
             (sQuiet || MpPanelDraw::allDead());   // (while quiet nothing runs the layouts: they stay dead)
    if (sQuiet) {
        sComposeKept = false;
        sLastPointed = -1;
        if (sSettingsLive && !seq.settingsOpen()) {   // settingsnative: closed, and its toast has gone: back to the early-out
            sSettingsLive = false;
            if (sSetOpenFree) {
                const uint32_t now = FreeOf(MR::getSceneHeapNapa());
                SettingsHeapLine("close", 0);
                mp_log(now == sSetOpenFree ? "settings: heap ok: closing gave back exactly what opening left (the layouts stay for the scene)" : "settings: HEAP CHANGED between open and close: %u -> %u", sSetOpenFree, now);
                sSetOpenFree = 0;
            }
        }
        return;
    }
    const bool composeReused = ComposeOrKeep();
#ifdef TARGET_PC
    const bool undrawnSteadyHud = port_turn_skipped_raw() != 0 && composeReused && !seq.isOpen() && !seq.isBusy() &&
                                  !MpOverlay::resultsShowing() && !MpOverlay::waitingShowing() &&
                                  !MpOverlay::starCardShowing() && !MpOverlay::headStartShowing() &&
                                  MpOverlay::toastCount() == 0 && !sRingOn && MpPanelDraw::onlyHudSteady();
#else
    const bool undrawnSteadyHud = false;
    (void)composeReused;
#endif
    if (undrawnSteadyHud) MpPanelDraw::tickSkipped();
    else MpPanelDraw::update(sMenuScene, sToastScene, sStripScene);
    // the star pointer: moving onto a big button points it (the d-pad keeps working; the pointer only acts when it moves on)
    int pointed = seq.isOpen() ? MpPanelDraw::pointedButton() : -1;
    int visible = -1;   // vitabtn: a row, head or tile of the page (the list may have scrolled: Sequence::pointAtVisible)
    if (pointed < 0 && seq.isOpen()) visible = GridPointed();   // MP3 chat: the Messages page's tiles
    if (pointed < 0 && visible < 0 && seq.isOpen()) visible = PagePointed();
    const int key = pointed >= 0 ? pointed : (visible >= 0 ? 1000 + visible : -1);
    // flow: a new screen (or a window closing) keeps its own first choice: a pointer resting where a button appears does not take
    // it; moving onto another button still points it
    {
        static int sLastScreen = -1, sLastDepth = -1;
        static bool sLastBusy = false;
        const int scr = seq.isOpen() ? static_cast<int>(seq.screen()) : -1;
        const bool busy = seq.dialogActive() || seq.keyboardActive();
        if (scr != sLastScreen || seq.depth() != sLastDepth || busy != sLastBusy) sLastPointed = key, sScreenChanged = true;
        sLastScreen = scr, sLastDepth = seq.depth(), sLastBusy = busy;
    }
    // flow (screenshots, 12 Nov): the pointer acts only when it MOVES onto a button. A pointer resting in the middle of the picture
    // while the screen's buttons grow in under it (the port shows it there on menus) took the middle button: the third of four on the
    // Multiplayer menu was chosen with no input at all.
    bool moved = false;
    {   // (a new screen: the hand's way there, from the screen before or to the middle where the port shows it, is no choice)
        static float sLastX = -1e9f, sLastY = -1e9f;
        float px = 0, py = 0;
        if (MR::isStarPointerInScreen(0)) {
            if (const TVec2f* p = MR::getStarPointerScreenPosition(0)) px = p->x, py = p->y;
        }
        static int sMovedFor = 0;   // (the button under a moved hand is known a frame or two later: the move counts for 10 frames)
        if (sLastX < -1e8f) sLastX = px, sLastY = py;   // (the first look is no move)
        if ((px - sLastX) * (px - sLastX) + (py - sLastY) * (py - sLastY) > 4.0f) sMovedFor = 10;   // (2 pixels: a still hand never chooses)
        else if (sMovedFor > 0) --sMovedFor;
        if (sScreenChanged) sMovedFor = 0, sScreenChanged = false;
        moved = sMovedFor > 0;
        sLastX = px, sLastY = py;
    }
    if (key != sLastPointed && moved) {
        if (pointed >= 0) seq.pointAt(pointed);
        else if (visible >= 0) seq.pointAtVisible(visible);
    }
    sLastPointed = key;
    if (sWindow) {
        sWindow->movement();
        sWindow->calcAnim();
    }
    HoldPlayer((seq.isBusy() || MpOverlay::resultsShowing()) && !(seq.settingsOpen() && seq.settingsOrigin() == MpMenu::Sequence::SO_PAUSE));   // (the pause menu already holds the game)
}

void drawSystem() {
    if ((!mp_entered() && !sSettingsLive && !MpDemo::live()) || !sCreated || sQuiet) return;
    MP_PROF_SCOPE(MP_PROF_TOTAL);
    MP_PROF_SCOPE(MP_PROF_UI_DRAW);
    MpPanelDraw::draw(sMenuPlan, sToastPlan, sStripPlan);
    if (sWindow) sWindow->draw();
    if (sZoomSessionOn && port_settings_panel_draw) port_settings_panel_draw(2);   // mppause: after the layouts, as the pause menu draws it
}

void openMultiplayerMenu(int selectedFile) {
    if (!sCreated || !Enabled() || !EnsureLayouts("open")) return;   // memfix: (the file select's scene heap has room; refused only when it has not)
    mp_mark_entered();   // MP3 netpack: from here the per-frame multiplayer hooks run
    SetLanguage();
    MpMenu::sequence().openMain(selectedFile);
}

void openMultiplayerMenuFromEntry() {   // entry
    if (!sCreated || !Enabled() || !EnsureLayouts("open")) return;   // memfix: the layouts of this scene on the first open
    mp_mark_entered();   // MP3 netcore (merge of netpack and entry): without it updateSystem / drawSystem skip the menu just opened
    SetLanguage();
    MpMenu::sequence().openFromEntry();
}

void openSettingsFromEntry() {   // settingsnative
    if (!sCreated || !EnsureLayouts("open")) return;
    sSettingsLive = true;   // (NOT mp_mark_entered(): see sSettingsLive)
    SetLanguage();
    HostAllocScope hostScope(true);
    if (!MpMenu::sequence().openSettings(MpMenu::Sequence::SO_ENTRY)) sSettingsLive = false;
}

// settingsnative: the pause menu's Settings (a single-player level). The panel and the dialog are made now in the scene heap (KIND_SETTINGS) when
// the heap has room; a refusal is safe (nothing is created, the menu does not open): false.
bool openSettingsFromPause() {
    if (!sCreated || mp_active() || MpMenu::sequence().settingsOpen() || MpMenu::sequence().isBusy()) return false;
    const port::settings::NativeHost* host = port::settings::NativeHostGet();
    if (!host || !host->available) return false;
    if (MpUiLayouts::sceneKind() == MpUiLayouts::KIND_TOAST) return false;   // (refused before in this scene: only the toast's windows are here)
    if (!sEager && sSceneLive && MpUiLayouts::sceneKind() == MpUiLayouts::KIND_NONE) MpUiLayouts::beginScene(MpUiLayouts::KIND_SETTINGS);
    static const bool forceRefuse = [] { const char* e = getenv("SMG_SETTINGS_NO_ROOM_TEST"); return e && *e && atoi(e) != 0; }();   // harness only: the refusal path
    const uint32_t before = FreeOf(MR::getSceneHeapNapa());
    SettingsHeapLine("before", 0);
    if (forceRefuse || !EnsureLayouts("settings")) {
        mp_log("settings: no room for the menu here (%s)%s", KindName(MpUiLayouts::sceneKind()), forceRefuse ? " (forced)" : "");
        // the refusal's toast needs a window to draw in: the small kind (frame and windows), made now if there is room for even that
        if (MpUiLayouts::sceneKind() == MpUiLayouts::KIND_SETTINGS) {
            MpUiLayouts::setSceneKind(MpUiLayouts::KIND_TOAST);
            if (EnsureLayouts("toast")) sSettingsLive = true, sSetOpenFree = 0;   // (the system runs for the toast; the quiet path turns it off after)
            else mp_log("settings: no room for the toast either");
        }
        return false;
    }
    sSetOpenFree = FreeOf(MR::getSceneHeapNapa());
    SettingsHeapLine("open", before > sSetOpenFree ? before - sSetOpenFree : 0);
    sSettingsLive = true;
    SetLanguage();
    HostAllocScope hostScope(true);
    if (!MpMenu::sequence().openSettings(MpMenu::Sequence::SO_PAUSE)) {
        sSettingsLive = false;
        return false;
    }
    return true;
}

// settingsnative: Triangle on the pause menu (agent D), or the stale LiveArea tile (port_settings_native_request), asks for the Settings.
std::atomic<bool> sPauseAsk{false};
std::atomic<bool> sColdLatch{false};
void PollPauseRequest() {
    bool ask = sPauseAsk.exchange(false);
    {   // harness only (SMG_SETTINGS_PAUSE_TEST=1): the desktop has no Triangle request, so the pause menu being up is the request
        static const bool test = [] { const char* e = getenv("SMG_SETTINGS_PAUSE_TEST"); return e && *e && atoi(e) != 0; }();
        static int sAge = 0;
        if (test && port_zoom_menu_active && port_zoom_menu_active() && ++sAge == 150) ask = true;
    }
    if (!ask && port_settings_open_take && port_zoom_menu_active && port_zoom_menu_active() && !mp_active()) ask = port_settings_open_take() != 0;
    if (!ask) return;
    if (!openSettingsFromPause()) {
        mp_log("settings: the pause menu could not open it");
        sIo.sound(MpMenu::Se::Buzzer);
        char16_t t[96];
        MpMsgText::format(t, 96, MP_T_SET_TOAST_NO_ROOM, MpMsgText::Args());   // (a safety net: the lean set should always fit)
        MpOverlay::toastText(t, 0);
    }
}
bool takeSettingsLatch() { return sColdLatch.exchange(false); }
bool requestSettings(bool cold) {
    if (cold) {
        sColdLatch = true;
        mp_log("settings: the old tile: Settings at the entry screen");
        return true;
    }
    if (port_zoom_menu_active && port_zoom_menu_active() && !mp_active()) {
        sPauseAsk = true;   // (the next update opens it from the pause menu)
        return true;
    }
    mp_log("settings: the old tile: not in a level's pause menu");
    sIo.sound(MpMenu::Se::Buzzer);   // (the "can't" sound; nothing more)
    return true;
}

void openSessionMenu() {
    if (!sCreated || !EnsureLayouts("open")) return;
    mp_mark_entered();
    SetLanguage();
    MpMenu::sequence().openSession();
}

bool demoStripShown() { return sStripOn; }

bool isMenuOpen() { return sCreated && (MpMenu::sequence().isBusy() || MpOverlay::resultsShowing()); }

// MP3 menus5 (owner 12 Nov, the star pointer parked in a corner of the gathering picture): the Vita's pad code keeps the star pointer on a 2D menu for
// good (the left stick and the d-pad move it: port_pointer_on_menus). Our menus are driven by the cursor (d-pad / stick / Cross) and by touch, which
// shows the pointer for three seconds by itself: while one is open the stick has no pointer to move, so the pointer shows only under a finger.
extern "C" int mp_menu_wants_no_idle_pointer(void) { return isMenuOpen() ? 1 : 0; }

// optfix: nothing of multiplayer is on screen or waiting in the UI (no menu, toast, card, strip, ring, name edge, window, hold) and the
// core is not rejoining: the entered flag may be retired (MpGame.cpp). True too when the system was never made.
bool settledQuiet() {
    if (!sCreated) return true;
    if (MpDemo::live()) return false;
    return sQuiet && !MpMenu::sequence().isOpen() && !MpMenu::sequence().isBusy() && !MpMenu::sequence().reconnecting() && !MpOverlay::resultsShowing();
}

bool reconnecting() { return mp_entered() && sCreated && MpMenu::sequence().reconnecting(); }

const wchar_t* findMessage(const char* id) {
    if (!sCreated) return nullptr;   // settingsnative: gated on the system, not on the MP menu
    return reinterpret_cast<const wchar_t*>(txt::findDialog(id));
}

void toast(uint16_t textId, int slot, uint32_t arg) {
    if (sCreated) MpOverlay::toast(textId, slot, arg);
}
void showWaiting(bool on, int done, int total, int secondsLeft) {
    if (sCreated) MpOverlay::showWaiting(on, done, total, secondsLeft);
}
void waitBanner(int slot) {
    if (sCreated) MpOverlay::waitBanner(slot);
}
void showResults(const MpMsg& results) {
    if (sCreated) MpOverlay::showResults(results);
}
void closeResults() {
    if (sCreated) MpOverlay::dismissResults();
}
void setResultsChoice(int choice) {
    if (sCreated) MpOverlay::setResultsChoice(choice);
}
bool takeResultsChoice(int* choice) { return sCreated && MpOverlay::takeResultsChoice(choice); }
void showHeadStart(int slot, int seconds) {
    if (sCreated) MpOverlay::showHeadStart(slot, seconds);
}
void forgetSeriesNames() {
    if (sCreated) MpOverlay::forgetSeriesNames();
}
void showStarCard(const MpStarCard& card) {
    if (!sCreated) return;
    const bool fresh = !MpOverlay::starCardShowing();
    MpOverlay::showStarCard(card);
    if (card.team && fresh) sIo.sound(MpMenu::Se::Fanfare);   // MP3 touches: a Team Star has its short fanfare
}
void hideStarCard() {
    if (sCreated) MpOverlay::dismissStarCard();
}
void setStanding(int slot, uint32_t turns) { MpOverlay::setStanding(slot, turns); }
void announcePick(int slot, const char* stage, int star) {
    if (!sCreated || !stage || !*stage) return;
    SetLanguage();
    char id[64];
    snprintf(id, sizeof id, "GalaxyName_%s", stage);
    char16_t name[48];
    if (Strip(MR::getGameMessageDirect(id), name, 48) <= 0) return;
    MpOverlay::toastPick(slot, name, star);
}

bool handleMessage(const MpMsg& m) {
    if (m.type == MP_MSG_TOAST) {
        MpReader r(m.data, m.len <= MP_MAX_PAYLOAD ? m.len : 0);
        const uint16_t id = r.u16();
        const uint8_t slot = r.u8();
        const uint32_t arg = r.u32();
        if (!r.done() || id == MP_T_NONE || id > MP_T_TOAST_LAST || (slot != 0xFF && slot >= MP_MAX_PLAYERS)) {
            mp_log("ui: toast message dropped (bad payload)");
            return true;
        }
        toast(id, slot == 0xFF ? -1 : slot, arg);
        return true;
    }
    if (m.type == MP_MSG_STAMP) {
        MpReader r(m.data, m.len <= MP_MAX_PAYLOAD ? m.len : 0);
        const uint8_t stamp = r.u8();
        if (!r.done() || stamp > 3 || m.fromSlot >= MP_MAX_PLAYERS) {
            mp_log("ui: stamp message dropped (bad payload)");
            return true;
        }
        toast(static_cast<uint16_t>(MP_T_STAMP_0 + stamp), m.fromSlot, 0);
        return true;
    }
    return false;
}

void sendStamp(int stamp) {
    if (stamp < 0 || stamp > 3) return;
    // MP3 coop: a stamp is a greeting (EMOTE): the text is the toast on every console, plus the gesture (when standing idle),
    // the character's voice and the hand on the name tag. (A STAMP from an older build is still shown above.)
    const bool sent = MpCoop::requestEmote(stamp, true);
    mp_log("ui: stamp %d %s", stamp, sent ? "sent" : "NOT sent");
}

bool takeLaunchRequest(int* fileId) { return sCreated && MpMenu::sequence().takeLaunch(fileId); }
bool takeWarpRequest(int* slot) { return sCreated && MpMenu::sequence().takeWarp(slot); }
bool takeDomeRequest() { return sCreated && MpMenu::sequence().takeDome(); }
bool takeManualRequest() { return sCreated && MpMenu::sequence().takeManual(); }
void onFileSelect() {
    if (!sCreated || !Enabled()) return;
    if (!sEager) {   // memfix: this scene is the menu scene (layouts on the first need, or now when a window is already wanted)
        MpUiLayouts::beginScene(MpUiLayouts::KIND_MENU);
        sNoLayoutsLogged = false;
        sSceneLive = true;
    }
    MpMenu::sequence().onFileSelect();
    if (!sEager && Wanted()) EnsureLayouts("scene");
}
bool fileSelectTick(bool canWarn) { return sCreated && MpMenu::sequence().fileSelectTick(canWarn); }
bool singlePlayerGate() { return sCreated && MpMenu::sequence().singlePlayerGate(); }   // flow (review 2 #4)

}  // namespace MpUi

// settingsnative: the LiveArea Settings tile of an old install still sends -settings; it opens the NATIVE Settings (contract: port/settings_native.h).
extern "C" bool port_settings_native_request(int cold) {
    return MpUi::requestSettings(cold != 0);
}
// keepawake33: 1 while native in-game Settings is open (entry.cpp uses this to ensure Settings never holds the OLED awake).
extern "C" int port_settings_native_open(void) {
    return sSettingsOpenAtom.load(std::memory_order_relaxed) ? 1 : 0;
}
