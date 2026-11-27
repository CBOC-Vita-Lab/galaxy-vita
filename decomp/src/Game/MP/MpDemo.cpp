// Galaxy 3.0 multiplayer: the puppet demo (owner: demo). See MpDemo.hpp, port/mp_demo.h and MpDemoLogic.hpp (the script).
//
// SMG_MP_DEMO=1: the scene's init creates the puppets (MpPuppets::initScene: the real puppets of all six characters, as a session
// with drop-in allowed would), the bubble holder and the pointer marks; the demo then drives them. Every logic turn:
//   1. the script (MpDemoLogic::viewAt) says who is "there": it changes the mock roster (port/mp_demo.h), which the bridge queries answer;
//   2. every second turn a canned STATE per present puppet goes to MpPuppets::handle (the same call a received packet makes), a
//      POINTER every fourth turn for the pointer, toasts and the card are queued through MpUi;
//   3. MpPuppets::update steps the puppets (drop-in binding, appear / arrive / hide, name plates, pointer marks), MpBubble::demoStep the bubble.
// Nothing is sent, no session exists (mp_active() stays false). At the end the roster empties, the puppets go (killed by the
// lifecycle), the roster switches off and "mp demo: done, N variants shown" is logged.
//
// SMG_MP_DEMO=menus: the three hooks below press the buttons a player would on the title and entry screens and feed the open menu a
// scripted tour (MpMenu::Sequence::demoShow: a screen without its flow). Never an "ok" inside the menu: nothing is chosen.
// harvprep31: with GXV_SEEDWALK=1 as well (the shader harvest's mpmenus leg, tools/seedwalk; never in the release) the tour also shows the Multiplayer menu's
// other screens that need no flow, then opens the native Settings from the entry screen (its Settings pill: the one Cross the tour gives, on the entry screen,
// as for Multiplayer) and shows each page once, then Back: nothing is changed or written (MpHarvestTour.hpp; tests/mp/ui_menu/harvest_test.cpp).
#include "Game/MP/MpDemo.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/MP/MpBubble.hpp"
#include "Game/MP/MpCoop.hpp"
#include "Game/MP/MpDemoLogic.hpp"
#include "Game/MP/MpEntry.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpHarvestTour.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpTeam.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "port/mp_bridge.h"

extern "C" int64_t port_host_time_ns(void);

namespace MpDemo {

int gMode = -1;
bool gLive = false;

int readMode() {
    gMode = mp_demo_mode();
    return gMode;
}

bool persistOk() { return mode() == MP_DEMO_OFF; }

namespace {

using namespace MpDemoLogic;

int64_t nowMs() { return port_host_time_ns() / 1000000; }

// The mock players' names on the plates and the strip: the character, spelled out (the plates show the name as sent).
const char* mockName(int look) {
    switch (look) {
    case MP_LOOK_MARIO: return "Mario";
    case MP_LOOK_LUIGI: return "Luigi";
    case MP_LOOK_TOAD: return "Toad";
    case MP_LOOK_COSMIC: return "Cosmic Mario";
    case MP_LOOK_YELLOW_TOAD: return "Yellow Toad";
    case MP_LOOK_COSMIC_LUIGI: return "Cosmic Luigi";
    default: return "Player";
    }
}

// ---- the puppet demo -------------------------------------------------------------------------------------------------------
enum Phase { PH_IDLE = 0, PH_WAIT, PH_RUN, PH_END, PH_DONE };
Phase sPhase = PH_IDLE;
uint32_t sTurn;
uint32_t sReadyTurns;
uint32_t sWaitTurns;
int64_t sStartMs;
float sSecs;
bool sInWater;
float sTheta[kSlots];
bool sWasPresent[kSlots];
bool sPointerSent, sPointerWas;
bool sBubbleWas, sCardShown, sStampShown;
uint32_t sEndTurns;
int sLocalLook;

// what was shown (the log's count)
bool sSeenChar[MP_LOOK_COUNT];
bool sSeenForm[MP_FORM_COUNT];
uint32_t sFormTurns[MP_FORM_COUNT];
bool sSeenArrival, sSeenPlates, sSeenStrip, sSeenBubble, sSeenPointer, sSeenToast, sSeenCard;
bool sSeenAct[ACT_COUNT];

void roster(const View& v) {
    for (int i = 0; i < kSlots; i++) {
        mp_demo_roster_set(i + 1, v.present[i], v.look[i], mockName(v.look[i]), v.ping[i], v.quiet[i]);
    }
}

void rosterIdle() {
    View v;
    viewAt(-1.0f, sSecs, false, &v);
    roster(v);
}

bool localFrame(Ref* ref, MpPoseF* lp) {
    if (!MpLocalCapture::capture(lp, sTurn)) {
        return false;
    }
    float q[4] = {lp->quat[0], lp->quat[1], lp->quat[2], lp->quat[3]};
    float m[3][4];
    memset(m, 0, sizeof m);
    MpPoseMath::mtxFromQuat(q, m);
    for (int i = 0; i < 3; i++) {
        ref->pos[i] = lp->pos[i];
        ref->up[i] = m[i][1];
        ref->front[i] = m[i][2];
    }
    return true;
}

void logShownOnce(bool* flag, const char* what) {
    if (!*flag) {
        *flag = true;
        mp_log("mp demo: shown %s (t=%.1f s)", what, (double)(nowMs() - sStartMs) / 1000.0);
    }
}

int countShown() {
    int n = 0;
    for (int c = 0; c < MP_LOOK_COUNT; c++) n += sSeenChar[c] ? 1 : 0;
    for (int f = 0; f < MP_FORM_COUNT; f++) n += (f != MP_FORM_NORMAL && sSeenForm[f]) ? 1 : 0;
    n += sSeenArrival + sSeenPlates + sSeenStrip + sSeenBubble + sSeenPointer + sSeenToast + sSeenCard;
    return n;
}

void track(const View& v, bool pointerOn) {
    for (int i = 0; i < kSlots; i++) {
        if (!v.present[i]) {
            continue;
        }
        MpPuppetBase* p = MpPuppetsInt::puppet(i + 1);
        if (p == nullptr || MR::isDead(p) || p->getCharacter() != v.look[i] || !p->isDrawable() || p->isCulled()) {
            continue;   // (a stand-in, hidden, off screen: not counted)
        }
        if (!sSeenChar[v.look[i]]) {
            sSeenChar[v.look[i]] = true;
            mp_log("mp demo: shown character %s (t=%.1f s)", mp_character_name(v.look[i]), (double)(nowMs() - sStartMs) / 1000.0);
        }
        if (p->isArriving()) {
            logShownOnce(&sSeenArrival, "arrival fade (see-through list)");
        }
        if (v.form[i] != MP_FORM_NORMAL && v.form[i] < MP_FORM_COUNT && (v.look[i] == MP_LOOK_MARIO || v.look[i] == MP_LOOK_LUIGI)) {
            if (++sFormTurns[v.form[i]] == 24) {   // (turns on screen: a slow desktop gives ~15 a second, the Vita 30 or more)
                sSeenForm[v.form[i]] = true;
                mp_log("mp demo: shown form %u on %s (t=%.1f s)", (unsigned)v.form[i], mp_character_name(v.look[i]), (double)(nowMs() - sStartMs) / 1000.0);
            }
        }
        if (!sSeenAct[v.act[i]]) {
            sSeenAct[v.act[i]] = true;
        }
    }
    if (MpNameTags::shown() > 0) logShownOnce(&sSeenPlates, "name plates");
    if (MpUi::demoStripShown()) logShownOnce(&sSeenStrip, "party strip");
    if (v.bubble && MpBubble::demoBubbleShown(2)) logShownOnce(&sSeenBubble, "bubble");
    float pt[3];
    if (pointerOn && MpCoop::pointerPoint(3, pt)) logShownOnce(&sSeenPointer, "pointer");
    if (MpOverlay::toastCount() > 0) logShownOnce(&sSeenToast, "toast");
    if (MpOverlay::starCardShowing()) logShownOnce(&sSeenCard, "card");
}

void finish(const char* why) {
    if (sPhase == PH_DONE) {
        return;
    }
    sPhase = PH_DONE;
    mp_demo_roster_enable(false);
    gLive = false;
    MpUi::hideStarCard();
    MpOverlay::reset();   // (no toast of the demo stays queued)
    char missing[256];
    missing[0] = '\0';
    size_t n = 0;
    for (int c = 0; c < MP_LOOK_COUNT && n + 24 < sizeof missing; c++) {
        if (!sSeenChar[c]) n += (size_t)snprintf(missing + n, sizeof missing - n, " %s", mp_character_name(c));
    }
    for (int f = 1; f < MP_FORM_COUNT && n + 12 < sizeof missing; f++) {
        if (!sSeenForm[f]) n += (size_t)snprintf(missing + n, sizeof missing - n, " form%d", f);
    }
    const struct { bool seen; const char* name; } ui[] = {{sSeenArrival, "arrival"}, {sSeenPlates, "plates"}, {sSeenStrip, "strip"}, {sSeenBubble, "bubble"},
                                                          {sSeenPointer, "pointer"}, {sSeenToast, "toast"}, {sSeenCard, "card"}};
    for (const auto& u : ui) {
        if (!u.seen && n + 16 < sizeof missing) n += (size_t)snprintf(missing + n, sizeof missing - n, " %s", u.name);
    }
    mp_log("mp demo: done, %d variants shown (%s)", countShown(), why);
    mp_log("mp demo: not shown:%s", n > 0 ? missing : " none");
}

void begin() {
    memset(sSeenChar, 0, sizeof sSeenChar);
    memset(sSeenForm, 0, sizeof sSeenForm);
    memset(sFormTurns, 0, sizeof sFormTurns);
    memset(sSeenAct, 0, sizeof sSeenAct);
    sSeenArrival = sSeenPlates = sSeenStrip = sSeenBubble = sSeenPointer = sSeenToast = sSeenCard = false;
    sPointerSent = sPointerWas = sBubbleWas = sCardShown = sStampShown = false;
    for (int i = 0; i < kSlots; i++) {
        sTheta[i] = (float)i * 2.0944f + 0.6f;
        sWasPresent[i] = false;
    }
    sStartMs = nowMs();
    sPhase = PH_RUN;
    mp_log("mp demo: begin in %s secs=%d in_water=%d local=%s", MR::getCurrentStageName(), (int)sSecs, sInWater ? 1 : 0, mp_character_name(sLocalLook));
}

void queueEvents(float u, const View& v) {
    for (int i = 0; i < kSlots; i++) {
        if (v.present[i] && !sWasPresent[i]) {
            MpUi::toast(MP_T_JOINED, i + 1, 0);   // "<name> joined."
            mp_log("mp demo: t=%.1f join slot=%d character=%s", (double)u * (double)sSecs, i + 1, mp_character_name(v.look[i]));
        }
        sWasPresent[i] = v.present[i];
    }
    if (u >= 0.62f && !sStampShown) {
        sStampShown = true;
        MpUi::toast(MP_T_STAMP_0, 1, 0);   // a greeting
    }
    if (v.bubble && !sBubbleWas) {
        MpUi::toast(MP_T_BUBBLED, 2, 0);
    } else if (!v.bubble && sBubbleWas) {
        MpUi::toast(MP_T_POPPED, 3, 2);
    }
    sBubbleWas = v.bubble;
    if (u >= 0.80f && !sCardShown) {
        sCardShown = true;
        MpStarCard card;
        memset(&card, 0, sizeof card);
        card.count = 4;
        card.by = 3;
        card.team = true;
        card.teamBits = 42;
        for (int i = 0; i < 4; i++) {
            card.e[i].slot = (uint8_t)i;
            card.e[i].have = true;
            card.e[i].starBits = (uint16_t)(10 + 7 * i);
            card.e[i].coins = (uint16_t)(20 + 3 * i);
            card.e[i].helped = (uint8_t)(i == 1 ? 2 : 0);
        }
        MpUi::showStarCard(card);
    }
}

void sendPoses(const View& v, const Ref& ref, bool pointerOn) {
    static MpMsg m;
    for (int i = 0; i < kSlots; i++) {
        if (!v.present[i]) {
            continue;
        }
        if (v.act[i] == ACT_WALK || v.act[i] == ACT_SWIM) {
            sTheta[i] += walkOmega() * 2.0f;
        }
        MpPoseF p;
        uint8_t act = v.act[i];
        const bool bubbled = i == 1 && v.bubble;
        if (bubbled) act = ACT_WAIT;
        makePose(ref, sTheta[i], act, v.actAge[i], v.form[i], sTurn, &p);
        if (bubbled) p.flags |= MP_POSE_BUBBLE;
        m.type = MP_MSG_STATE;
        m.fromSlot = (uint8_t)(i + 1);
        m.len = (uint16_t)MpPoseMath::writeState(m.data, sizeof m.data, p);
        if (m.len != 0) {
            MpPuppets::handle(m);
        }
    }
    if ((sTurn & 3u) == 0 && (pointerOn || sPointerWas)) {
        MpPointerMsg pm;
        pm.serial = mp_scene_serial();
        pm.on = pointerOn ? 1 : 0;
        pointerPoint(ref, (float)(nowMs() - sStartMs) / 1000.0f, pm.pos);
        MpWriter w(m.data, sizeof m.data);
        mp_write_pointer(w, pm);
        m.type = MP_MSG_POINTER;
        m.fromSlot = 3;
        m.len = (uint16_t)(w.ok() ? w.n : 0);
        if (m.len != 0) {
            MpPuppets::handle(m);
        }
        sPointerWas = pointerOn;
    }
}

}  // namespace

void addArchives(void* collector) {
    if (mp_demo_mode() != MP_DEMO_PUPPETS || mp_active() || collector == nullptr || !MR::isEqualSceneName("Game")) {
        return;   // (only the stages: the title screen's scene is a game scene too, and the demo has nothing to do there)
    }
    const bool localLuigi = MR::isPlayerLuigi();
    const char* added[MP_LOOK_COUNT];
    int n = 0;
    for (int c = 0; c < MP_LOOK_COUNT; c++) {
        const char* arc = mp_character_archive(c, localLuigi);
        if (arc == nullptr) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < n; i++) dup = dup || strcmp(added[i], arc) == 0;
        if (dup) {
            continue;
        }
        added[n++] = arc;
        static_cast< NameObjArchiveListCollector* >(collector)->addArchive(arc);
        mp_log("mp demo: archive %s", arc);
    }
    MpBubble::addArchives(collector);
}

void sceneInit() {
    if (mp_demo_mode() != MP_DEMO_PUPPETS) {
        return;
    }
    sPhase = PH_IDLE;
    if (!MR::isEqualSceneName("Game") || MR::isStageFileSelect()) {
        return;   // the title screen, the file select and the other scenes: nothing (the next stage that loads starts the demo)
    }
    if (mp_active()) {
        mp_log("mp demo: a session is active: the demo does not run");
        return;
    }
    memset(sSeenChar, 0, sizeof sSeenChar);
    sSecs = (float)mp_demo_secs();
    sLocalLook = MR::isPlayerLuigi() ? MP_LOOK_LUIGI : MP_LOOK_MARIO;
    // the roster as set A on slots 1..3, so MpPuppets::initScene makes the puppets of set A and a spare for each of set B's characters;
    // then everybody "leaves" until the script says otherwise
    mp_demo_roster_enable(true);
    mp_demo_roster_local(0);
    mp_demo_roster_host(false);
    mp_demo_roster_set(0, false, sLocalLook, mockName(sLocalLook), 20, false);
    for (int i = 0; i < kSlots; i++) {
        mp_demo_roster_set(i + 1, true, kSetA[i], mockName(kSetA[i]), 40, false);
    }
    gLive = true;
    MpPuppets::initScene();
    MpBubble::sceneInit();
    rosterIdle();
    sTurn = 0;
    sWaitTurns = 0;
    sReadyTurns = 0;
    sPhase = PH_WAIT;
    mp_log("mp demo: scene ready in %s (puppets made, waiting for play)", MR::getCurrentStageName());
}

void update() {
    if (mp_demo_mode() != MP_DEMO_PUPPETS || !gLive) {
        return;
    }
    if (mp_active()) {   // a real session came up: the demo steps aside at once
        MpPuppets::update();
        finish("a session started");
        return;
    }
    ++sTurn;
    if (sPhase == PH_WAIT) {
        // the stage's own opening first: the scenario camera, the starter, any demo; then a short settle
        if (++sWaitTurns > 60 * 240) {
            mp_log("mp demo: the scene never reached play: not started");
            finish("never in play");
            return;
        }
        Ref ref;
        MpPoseF lp;
        if (MpGameInternal::sceneState() == MP_SCENE_ACTION && !MR::isDemoActive() && localFrame(&ref, &lp)) {
            if (++sReadyTurns >= 120) {
                sInWater = (lp.flags & MP_POSE_IN_WATER) != 0;
                begin();
            }
        } else {
            sReadyTurns = 0;
        }
        return;
    }
    if (sPhase == PH_END) {
        // the roster is empty: the lifecycle kills what is left (a few turns), then the demo switches off
        MpTeam::noteHud(true);
        MpPuppets::update();
        MpBubble::demoStep();
        if (++sEndTurns > 30) {
            finish("script end");
        }
        return;
    }
    if (sPhase != PH_RUN) {
        return;
    }
    // ---- the script
    const float t = (float)(nowMs() - sStartMs) / 1000.0f;
    const float u = t / sSecs;
    Ref ref;
    MpPoseF lp;
    const bool haveRef = localFrame(&ref, &lp);
    if (haveRef && (sTurn % 60) == 0) sInWater = (lp.flags & MP_POSE_IN_WATER) != 0;
    View v;
    viewAt(u, sSecs, sInWater, &v);
    roster(v);
    queueEvents(u, v);
    MpTeam::noteHud(true);   // the strip follows the life counter's HUD; the demo keeps it shown
    if (haveRef && (sTurn & 1u) == 0) {
        sendPoses(v, ref, v.pointer);
    }
    MpPuppets::update();
    MpBubble::demoStep();
    track(v, v.pointer);
    if (v.done) {
        for (int i = 0; i < kSlots; i++) {
            if (sWasPresent[i]) mp_log("mp demo: t=%.1f leave slot=%d", (double)t, i + 1);
            sWasPresent[i] = false;
        }
        rosterIdle();
        sEndTurns = 0;
        sPhase = PH_END;
    }
}

void sceneEnd() {
    if (mp_demo_mode() != MP_DEMO_PUPPETS) {
        return;
    }
    MpPuppets::endScene();   // (MpGame only ends the puppets of a session scene; safe when none are live)
    if (gLive) {
        finish("scene ended");
    }
    mp_demo_roster_enable(false);
    gLive = false;
    sPhase = PH_IDLE;
}

// ---- the menu tour (SMG_MP_DEMO=menus) -------------------------------------------------------------------------------------
namespace {

enum Tour { T_TITLE = 0, T_ENTRY_FIRST, T_MENU, T_ENTRY_BACK, T_DONE, T_ENTRY_SETTINGS, T_SETTINGS };   // (the last two: harvprep31, harvest only)
bool sMenuSeen;
Tour sTour = T_TITLE;
int64_t sTourStartMs;
int64_t sStepMs;       // when the current step began
int sStep;
bool sTourLogged;

// (harvprep31) the shader harvest's walk (GXV_SEEDWALK=1, tools/seedwalk): the tour goes on to the other screens and the Settings (MpHarvestTour.hpp)
bool harvest() {
    static const int on = [] { const char* e = getenv("GXV_SEEDWALK"); return e != nullptr && *e != '\0' && atoi(e) != 0 ? 1 : 0; }();
    return on != 0;
}
MpHarvestTour::Walk sMpWalk, sSetWalk;
bool sSetSeen, sSetWalked;
int sDowns;

float scale() { return (float)mp_demo_secs() / 60.0f; }
int64_t ms(float seconds) { return (int64_t)(seconds * 1000.0f * scale()); }

void tourStart() {
    if (!sTourLogged) {
        sTourLogged = true;
        sTourStartMs = nowMs();
        sStepMs = nowMs();
        sStep = 0;
        mp_log("mp demo: menus tour begin secs=%d", mp_demo_secs());
    }
}

bool sShownScr[MpMenu::S_COUNT];   // the menu screens the sequence itself reported on screen (Sequence::screen(), asked every turn)

void tourDone(const char* why) {
    if (sTour == T_DONE) {
        return;
    }
    sTour = T_DONE;
    mp_demo_roster_enable(false);
    char shown[512] = "";
    char missing[512] = "";
    int n = 1;   // the entry screen (its own class, not a menu screen)
    size_t a = 0, b = 0;
    for (int i = 1; i < MpMenu::S_COUNT; i++) {
        const char* nm = MpMenu::screenName((MpMenu::Scr)i);
        if (sShownScr[i]) {
            n++;
            a += (size_t)snprintf(shown + a, sizeof shown - a, " %s", nm);
        } else {
            b += (size_t)snprintf(missing + b, sizeof missing - b, " %s", nm);
        }
    }
    mp_log("mp demo: done, %d screens shown (entry,%s; %s)", n, shown, why);
    // (asked of the menu system, not a fixed list: a screen the tour has no step for is named here)
    mp_log("mp demo: not shown (menu screens the tour never reached; the connection test is shown but not started):%s", missing);
}

void tourWatchdog() {
    if (sTourLogged && sTour != T_DONE && nowMs() - sTourStartMs > ms(harvest() ? 330.0f : 150.0f)) {   // (harvprep31: the harvest's longer tour)
        mp_log("mp demo: the menus tour stalled (step %d, tour %d): stopped", sStep, (int)sTour);
        MpMenu::sequence().close();
        tourDone("stalled");
    }
}

}  // namespace

bool titleConfirm() {
    if (mode() != MP_DEMO_MENUS || sTour != T_TITLE) {
        return false;
    }
    tourStart();
    tourWatchdog();
    if (nowMs() - sStepMs < ms(4.0f)) {   // the title logo stays on screen for a while first
        return false;
    }
    sTour = T_ENTRY_FIRST;
    sStepMs = nowMs();
    sStep = 0;
    mp_log("mp demo: title -> entry screen");
    return true;
}

void entryInput(MpEntry::In* in, int state, int cursor) {
    if (mode() != MP_DEMO_MENUS || sTour == T_DONE || sTour == T_TITLE) {
        return;
    }
    in->up = in->down = in->ok = in->back = false;   // (the tour presses; the player's own presses are not read here)
    tourWatchdog();
    const int ST_CHOOSE = 2;
    if (state != ST_CHOOSE || !in->shown) {
        return;
    }
    const int64_t d = nowMs() - sStepMs;
    if (sTour == T_ENTRY_FIRST) {
        if (sStep == 0 && d > ms(3.5f)) {   // the two buttons, the cursor on Single Player
            sStep = 1;
            sStepMs = nowMs();
            if (cursor == 0) in->down = true;
            mp_log("mp demo: entry screen: cursor to Multiplayer");
        } else if (sStep == 1 && d > ms(2.5f)) {
            sStep = 2;
            sStepMs = nowMs();
            in->ok = true;
            sTour = T_MENU;
            sMenuSeen = false;
            mp_log("mp demo: entry screen: Multiplayer");
        }
    } else if (sTour == T_ENTRY_BACK && harvest() && !sSetWalked) {   // (harvprep31) the entry screen again: to the Settings pill (the third), then its Cross
        if (d > ms(1.5f)) {
            sStepMs = nowMs();
            if (cursor == MpEntry::CHOICE_SETTINGS) {
                in->ok = true;
                sTour = T_ENTRY_SETTINGS;
                sSetSeen = false;
                mp_log("mp demo: entry screen: Settings (harvest)");
            } else if (sDowns++ < 4) {
                in->down = true;
            } else {
                sSetWalked = true;   // (no Settings pill on this console: the tour ends as before)
                mp_log("mp demo: entry screen: no Settings to show (harvest)");
            }
        }
    } else if (sTour == T_ENTRY_BACK) {
        if (d > ms(2.5f)) {
            in->back = true;
            mp_log("mp demo: entry screen: back to the title");
            tourDone("back at the title");
        }
    } else if (sTour == T_ENTRY_SETTINGS || sTour == T_SETTINGS) {   // (harvprep31) the entry screen back while the Settings tour runs: they closed
        if (sTour == T_SETTINGS || d > ms(6.0f)) {
            sSetWalked = true;
            sTour = T_ENTRY_BACK;
            sStepMs = nowMs();
            mp_log("mp demo: settings closed: entry screen (harvest)");
        }
    }
}

// (harvprep31, harvest only) the Settings opened from the entry screen: each page once, then Back on the main page (nothing changed: nothing is written)
bool settingsInput(MpMenu::Input* in, MpMenu::Sequence& seq) {
    *in = MpMenu::Input();
    tourWatchdog();
    if (!seq.isOpen()) {
        return true;   // (closed: the entry screen takes over, entryInput)
    }
    if ((int)seq.screen() > 0 && (int)seq.screen() < MpMenu::S_COUNT) sShownScr[seq.screen()] = true;
    if (sTour == T_ENTRY_SETTINGS) {
        if (!seq.settingsOpen() || seq.screen() != MpMenu::S_SET_MAIN) return true;
        sTour = T_SETTINGS;
        sSetSeen = true;
        sSetWalk = MpHarvestTour::Walk();
        sStep = 0;
        sStepMs = nowMs();
        mp_log("mp demo: settings: the pages (harvest)");
    }
    if (sStep == 0) {
        if (sSetWalk.step(seq, in, MpHarvestTour::kSetScreens, nowMs(), ms(2.0f), sShownScr)) {
            mp_log("mp demo: settings: %u pages shown, %u left by themselves (harvest); back", sSetWalk.reached, sSetWalk.missed);
            sStep = 1;
            sStepMs = nowMs();
        }
    } else if (sStep == 1 && nowMs() - sStepMs > ms(1.5f) && seq.screen() == MpMenu::S_SET_MAIN) {
        in->back = true;   // (the main page's Back: not dirty, so the Settings close without a question and write nothing)
        sStep = 2;
        sStepMs = nowMs();
    }
    return true;
}

bool menuInput(MpMenu::Input* in, MpMenu::Sequence& seq) {
    if (mode() == MP_DEMO_MENUS && harvest() && (sTour == T_ENTRY_SETTINGS || sTour == T_SETTINGS)) {
        return settingsInput(in, seq);
    }
    if (mode() != MP_DEMO_MENUS || sTour != T_MENU) {
        return false;
    }
    *in = MpMenu::Input();
    tourWatchdog();
    if (!seq.isOpen()) {
        if (sMenuSeen) {   // the menu closed by our last Back: the entry buttons again
            sTour = T_ENTRY_BACK;
            sStepMs = nowMs();
            sStep = 0;
        }
        return true;
    }
    if ((int)seq.screen() > 0 && (int)seq.screen() < MpMenu::S_COUNT) sShownScr[seq.screen()] = true;
    if (!sMenuSeen) {
        sMenuSeen = true;
        sStep = 0;
        sStepMs = nowMs();
    }
    using MpMenu::Scr;
    const int64_t d = nowMs() - sStepMs;
    auto next = [&](int s) { sStep = s; sStepMs = nowMs(); };
    switch (sStep) {
    case 0:   // the menu just opened on its first screen: a moment, then the character select over it
        if (d > ms(1.5f)) {
            seq.demoShow(MpMenu::S_CHARS);
            mp_log("mp demo: character select");
            next(1);
        }
        break;
    case 1: case 2: case 3: case 4: case 5:   // the cursor over all six heads: right, right, down, left, left
        if (d > ms(sStep == 1 ? 2.5f : 1.8f)) {
            if (sStep == 1 || sStep == 2) in->right = true;
            else if (sStep == 3) in->down = true;
            else in->left = true;
            next(sStep + 1);
        }
        break;
    case 6:
        if (d > ms(1.8f)) {
            seq.demoBack();   // (not a choice: nothing is saved, no profile is set)
            mp_log("mp demo: multiplayer menu");
            next(7);
        }
        break;
    case 7: case 8: case 9:   // the four rows of the menu
        if (d > ms(1.7f)) {
            in->down = true;
            next(sStep + 1);
        }
        break;
    case 10:
        if (d > ms(1.7f)) {
            // wire: four mock players on the gathering screen (the one door: there is no lobby any more; the status comes from the mock
            // roster, platform/src/mp/mp_gather.cpp DemoStatus). Nothing is started, nothing is sent.
            mp_demo_roster_enable(true);
            mp_demo_roster_local(0);
            mp_demo_roster_host(true);
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                static const uint8_t look[4] = {MP_LOOK_MARIO, MP_LOOK_LUIGI, MP_LOOK_TOAD, MP_LOOK_COSMIC};
                mp_demo_roster_set(i, true, look[i], mockName(look[i]), i == 0 ? -1 : 30 + 40 * i, false);
            }
            seq.demoShow(MpMenu::S_GATHER);
            mp_log("mp demo: gathering screen with four mock players");
            next(11);
        }
        break;
    case 11: case 12:   // a moment on the heads (the cursor moves over the buttons)
        if (d > ms(2.0f)) {
            in->down = true;
            next(sStep + 1);
        }
        break;
    case 13:
        if (d > ms(2.0f)) {
            seq.demoBack();
            mp_demo_roster_enable(false);
            seq.demoShow(MpMenu::S_CONNTEST);   // (the screen only: no test is started, nothing is sent)
            mp_log("mp demo: connection test screen (not started)");
            next(14);
        }
        break;
    case 14:
        if (d > ms(4.5f)) {
            seq.demoBack();
            mp_log("mp demo: back to the multiplayer menu, then out");
            next(harvest() ? 20 : 15);
        }
        break;
    case 20:   // (harvprep31, harvest only) the menu's other screens without their flow, each once (MpHarvestTour::kMpScreens), then out as before
        if (sMpWalk.step(seq, in, MpHarvestTour::kMpScreens, nowMs(), ms(2.0f), sShownScr)) {
            mp_log("mp demo: multiplayer screens (harvest): %u shown, %u left by themselves", sMpWalk.reached, sMpWalk.missed);
            next(15);
        }
        break;
    case 15:
        if (d > ms(1.5f)) {
            in->back = true;   // the menu's own Back on its first screen: it closes
            next(40);
        }
        break;
    default: break;
    }
    return true;
}

}  // namespace MpDemo
