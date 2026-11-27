// Galaxy 3.0 multiplayer: messages between players (owner: chat). See Game/MP/MpChat.hpp and port/mp_chat.h.
//
// Balloons: the game's own short NPC speech balloon (LayoutData/TalkBalloonStretch, stationed in every scene; TalkBalloonShort's
// look and its typewriter text), one per slot, created with the scene like the name tags (MpNameTag.cpp), placed every turn just
// above the speaker's name tag, its beak pointing down at it. Hidden while the speaker is off screen (the toast still says it).
// The quick ring: in play, Select (the Wii's -) belongs to multiplayer (START opens the session menu): hold it and the ring of the
// four most used phrases opens around the middle of the picture; a direction (D-pad, or the right stick where it moves the camera)
// sends that phrase. WPadButton::update lends us Select and, while the ring is open, the directions (the camera does not turn).
// Not in the tilt levels (Select levels the tilt there), not while a menu, a window or the keyboard is up.
//
// Desktop test hook (never on the Vita): SMG_MP_TEST_CHAT="<secs>[@<serial>]:<what>[,...]": <secs> of live play in the current
//   scene, in the first scene whose flow serial is >= <serial> (default 0); each event once.
//   what = <phrase number> (the Messages page's path), ring<0..3> (Select held, then that direction: the ring's own path),
//   text:<words> (a typed message; no commas), mute:<slot> / unmute:<slot> (the Players screen's Mute).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/MP/MpAway.hpp"        // MP3 touches
#include "Game/MP/MpCharColor.hpp"   // MP3 touches
#include "Game/MP/MpChat.hpp"
#include "Game/MP/MpChatLogic.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPanelDraw.hpp"
#include "Game/MP/MpPresent.hpp"   // MP3 ingame: no toast; an off-screen speaker's balloon by their edge head
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpSeriesLogic.hpp"   // lobby: the rematch ask
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/NPC/TalkTextFormer.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MessageUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <nw4r/lyt/material.h>
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/System/Language.hpp"
#include "port/mp_core_game.h"   // MpPlayerInfo::sid (mute by the stable player id)
#include <revolution/wpad.h>

extern "C" int port_input_tilt_active(void);   // platform/src/input/wpad.cpp: a tilt-steered level is running

namespace {

namespace txt = MpMsgText;
using MpChatLogic::Ring;

// ---------------------------------------------------------------- the balloon
// TalkBalloonShort without its TalkMessageCtrl: the same layout, animations and typewriter.
class MpChatBalloon : public LayoutActor {
public:
    MpChatBalloon() : LayoutActor("MpChatBalloon", true), mTurns(0), mClosing(false) {}

    virtual void init(const JMapInfoIter&) {
        MR::connectToSceneLayout(this);
        initLayoutManager("TalkBalloonStretch", 2);
        MR::createAndAddPaneCtrl(this, "Balloon", 1);
        MR::hideScreen(this);
        kill();
    }

    // touches: the words wear the speaker's character colour, the dark shade of it (the balloon is white); before the tag processor starts from the mapping
    // (the text's colour mapping is the material's two TEV colours; the text box may sit below the named pane, so this walks the children)
    static void tintText(nw4r::lyt::Pane* pane, const MpCharColor::C& c, int depth = 0) {
        if (pane == nullptr || depth > 6) return;
        nw4r::lyt::TextBox* box = nw4r::ut::DynamicCast< nw4r::lyt::TextBox* >(pane);
        nw4r::lyt::Material* mat = box != nullptr ? box->GetMaterial() : nullptr;
        if (mat != nullptr) {
            for (u32 k = 0; k < 2; k++) {
                GXColorS10 col = mat->GetTevColor(k);
                col.r = c.r; col.g = c.g; col.b = c.b;
                mat->SetTevColor(k, col);
            }
        }
        for (nw4r::lyt::PaneList::Iterator it = pane->GetChildList().GetBeginIter(); it != pane->GetChildList().GetEndIter(); ++it) tintText(&*it, c, depth + 1);
    }

    void ink(int look) { tintText(MR::getPane(this, "Text00"), MpCharColor::ink(look)); }

    void say(const wchar_t* text, int lines, int turns, int look = 0) {
        appear();
        MR::showScreen(this);
        MR::setTextBoxMessageRecursive(this, "Text00", text);
        ink(look);
        MR::initTagProcessorRecursive(this, "Text00", 0);
        MR::startAnim(this, "Appear", 0);
        MR::startPaneAnim(this, "Balloon", "Beak", 0);
        // the beak straight down (TalkBalloon::updateBalloon's frame for a speaker right under the balloon)
        MR::setPaneAnimFrameAndStop(this, "Balloon", MR::getPaneAnimFrameMax(this, "Balloon", 0) * 0.5f, 0);
        MR::startAnim(this, lines > 1 ? "TwoLine" : "OneLine", 1);
        MR::setAnimFrameAndStopAdjustTextWidth(this, "TxtText", 1);
        mTurns = turns;
        mClosing = false;
    }

    // once a turn while alive: the typewriter, the place, the timer, the closing animation
    void step(bool visible, f32 x, f32 y) {
        if (MR::isDead(this)) return;
        MR::nextStepTagProcessorRecursive(this, "Text00");
        if (visible) {
            MR::showScreen(this);
            setTrans(TVec2f(x, y));
        } else {
            MR::hideScreen(this);
        }
        if (!mClosing) {
            if (--mTurns <= 0) {
                MR::startAnim(this, "End", 0);
                mClosing = true;
            }
        } else if (MR::isAnimStopped(this, 0)) {
            kill();
        }
    }

    int mTurns;
    bool mClosing;
};

constexpr f32 kBalloonLift = 40.0f;   // screen units above the head's projection: clear of the name tag
constexpr f32 kEdgeLift = 34.0f, kEdgeSide = 160.0f, kEdgeTop = 70.0f;   // MP3 ingame: a balloon by an edge head (screen units)
constexpr int kPendingMs = 2500;      // a message that arrives while no scene runs still gets its balloon this soon after
// 13 Nov (A3): characters on one line before the balloon takes two. A typed message is at most MP_CHAT_TEXT_CHARS (24) characters; 24 of the
// widest letters on one line were wider than the balloon's text box and were cut mid-word. Over 14 the message wraps at the space nearest
// its middle (else after 14): no line is longer than 14 characters (the box holds 14 of the widest), and nothing is ever cut.
constexpr int kBalloonLine = 14;

struct Pending {
    bool on;
    int64_t at;
    uint8_t kind;
    int lines, turns;
    wchar_t text[MP_CHAT_TEXT_CHARS + 4];
};

bool sInit = false;
Ring sRing;
MpChatLogic::Mute sMute;
// the stable id of the player in `s` (smallfix2: from the rejoin commitment; 0 = unknown or this console's own slot: the name key)
uint32_t sidOf(int s) {
    MpPlayerInfo pi;
    return mp_core_slot_player(s, &pi) ? pi.sid : 0u;
}
MpChatBucket sSend;
MpChatBucket sShowLimit[MP_MAX_PLAYERS];
MpChatBalloon* sBalloon[MP_MAX_PLAYERS];   // scene objects: valid only between initScene() and endScene()
bool sSceneBalloons = false;
Pending sPend[MP_MAX_PLAYERS];
uint32_t sShowDrops = 0;

// the ring
enum RingPhase { RING_CLOSED = 0, RING_OPEN, RING_PICKED, RING_FADE };
volatile bool sRingAllowed = false;   // set by update() (game thread), read by filterPad (the same thread, at the frame's start)
int sRingPhase = RING_CLOSED;
int sRingAge = 0, sRingPicked = -1, sRingLeft = 0;
uint32_t sRawHold = 0, sRawTrig = 0;
bool sRawFresh = false;               // the triggers of this frame were not read yet (a frame may hold two logic turns)
uint32_t sInjHold = 0, sInjTrig = 0;  // (test hook) buttons the ring sees this turn

void ensureInit() {
    if (sInit) return;
    sInit = true;
    sRing.reset();
    sMute.reset();
    mp_chat_bucket_reset(&sSend);
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_chat_bucket_reset(&sShowLimit[i]);
        sBalloon[i] = nullptr;
        sPend[i].on = false;
    }
}

int64_t nowMs() { return MpGameInternal::nowMs(); }

// Does the game's message font have this character? (no fonts yet: assume so)
struct FontHas {
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    bool ok;
    FontHas() { ok = MpPanelDraw::fonts(fm); }
    bool operator()(char16_t c) const { return !ok || !fm[0].has || fm[0].has(fm[0].context, c); }
};

int phraseText(int phrase, char16_t* out, int cap) {
    return txt::format(out, cap, static_cast<MpText>(MP_T_PHRASE_0 + phrase), txt::Args());
}

uint8_t headIcon(int slot) {
    const int look = mp_slot_look(slot);
    return static_cast<uint8_t>(MpPanel::ICON_MARIO + (look >= 0 && look < MP_LOOK_COUNT ? look : 0));
}

// What everyone sees: the toast line "<name>: <words>" with the speaker's head, and the balloon over the speaker.
void show(int slot, const MpChatMsg& m, bool toast, bool balloon) {
    txt::setLanguage(txt::languageIndex(MR::getCurrentLanguagePrefix()));
    char16_t body[MP_CHAT_TEXT_CHARS + 4];
    int chars;
    if (m.kind == MP_CHAT_PHRASE && MpSeriesLogic::asksAsQuestion(m.kind, m.phrase, MpOverlay::resultsShowing())) {
        chars = txt::format(body, MP_CHAT_TEXT_CHARS + 2, MP_T_RACE_ASK, txt::Args());   // menus4: on a results card phrase 3 is the rematch ask: "Race Again?"
    } else if (m.kind == MP_CHAT_PHRASE) {
        chars = phraseText(m.phrase, body, MP_CHAT_TEXT_CHARS + 2);
    } else {
        FontHas has;
        chars = MpChatLogic::toShown(m.text, m.len, body, MP_CHAT_TEXT_CHARS + 2, has);
    }
    if (chars <= 0) return;
    // MP3 ingame: the balloon is the message (over the speaker, or by their head at the picture's edge): no toast line as well
    if (toast && !MpOverlay::policy()) {
        char16_t line[96];
        txt::Args a;
        a.str = mp_slot_name(slot);
        int n = txt::format(line, 96, MP_T_CHAT_FROM, a);
        for (int i = 0; body[i] && n < 95; i++) line[n++] = body[i];
        line[n] = 0;
        MpOverlay::toastText(line, headIcon(slot));
    }
    if (balloon && slot >= 0 && slot < MP_MAX_PLAYERS) {
        Pending& p = sPend[slot];
        for (int i = 0; i <= chars; i++) p.text[i] = static_cast<wchar_t>(body[i]);
        p.lines = MpChatLogic::wrapTwoLines(reinterpret_cast<char16_t*>(p.text), MP_CHAT_TEXT_CHARS + 4, kBalloonLine);
        p.turns = MpChatLogic::balloonTurns(m.kind, chars);
        p.kind = m.kind;
        p.at = nowMs();
        p.on = true;
    }
}

bool sendMsg(const MpChatMsg& m) {
    uint8_t buf[MP_CHAT_MAX_PAYLOAD];
    MpWriter w(buf, sizeof buf);
    mp_write_chat(w, m);
    return w.ok() && mp_send(MP_MSG_CHAT, buf, static_cast<uint16_t>(w.n), true);
}

// The speaker's head in the world: a remote player's puppet, or our own character.
bool headOf(int slot, TVec3f* out) {
    MpPuppetBase* p = MpPuppetsInt::puppet(slot);
    if (slot == mp_local_slot()) {
        if (p != nullptr && p->isDrawable()) {   // (the self puppet draws our character)
            p->headPosition(out);
            return true;
        }
        if (MpPuppetsInt::localActor() == nullptr) return false;
        TVec3f up;
        MR::getPlayerUpVec(&up);
        const TVec3f* pos = MR::getPlayerPos();
        out->set(pos->x + up.x * 170.0f, pos->y + up.y * 170.0f, pos->z + up.z * 170.0f);
        return true;
    }
    if (p == nullptr || !p->isDrawable() || p->isCulled() || !mp_slot_present(slot)) return false;
    p->headPosition(out);
    return true;
}

void stepBalloons() {
    const int64_t now = nowMs();
    for (int s = 0; s < MP_MAX_PLAYERS; s++) {
        MpChatBalloon* b = sBalloon[s];
        Pending& p = sPend[s];
        if (p.on && now - p.at > kPendingMs) p.on = false;   // (too late: the toast said it)
        if (b == nullptr) continue;
        if (p.on) {
            p.on = false;
            if (!sMute.muted(s) || s == mp_local_slot()) b->say(p.text, p.lines, p.turns, static_cast<int>(mp_slot_look(s)));
        }
        if (MR::isDead(b)) continue;
        bool visible = false;
        f32 x = 0.0f, y = 0.0f;
        TVec3f head;
        if (headOf(s, &head) && !(sMute.muted(s) && s != mp_local_slot())) {
            TVec3f view;
            MR::getCameraViewMtx().mult(head, view);
            if (view.z < 0.0f) {   // (in front of the camera: it looks down -z)
                TVec3f screen;
                MR::calcScreenPosition(&screen, head);
                x = screen.x;
                y = screen.y - kBalloonLift;
                visible = x > -40.0f && x < MR::getScreenWidth() + 40.0f && y > -40.0f && y < MR::getScreenHeight() + 40.0f;
            }
        }
        // MP3 ingame: an off-screen speaker's balloon stands by their head at the picture's edge (MpNameTags' edge heads)
        f32 ex = 0.0f, ey = 0.0f;
        if (!visible && !(sMute.muted(s) && s != mp_local_slot()) && MpNameTags::edgeOf(s, &ex, &ey)) {
            const MpPanel::Screen scr = MpPanelDraw::screen();
            const f32 sw = (f32)MR::getScreenWidth(), sh = (f32)MR::getScreenHeight();
            x = scr.width > 0.0f ? ex * (sw / scr.width) : ex;
            y = ey - kEdgeLift;
            // (kept clear of the HUD's corners: inside the middle of the width and above the bottom counters' band)
            const f32 side = kEdgeSide * (sw / 640.0f);
            if (x < side) x = side;
            if (x > sw - side) x = sw - side;
            if (y < kEdgeTop) y = ey + kEdgeLift;   // (a head on the top edge: the balloon under it)
            if (y > sh * 0.7f) y = sh * 0.7f;
            visible = true;
        }
        b->step(visible, x, y);
    }
}

// ---------------------------------------------------------------- ingame: typed messages from the ring
// The console's own keyboard (mp_keyboard_open: the Vita's IME), at most MP_CHAT_TEXT_CHARS characters; done = sent as a typed
// message (MpChat::sendText: cleaned, rate limited, shown in the sender's balloon like a quick message). Polled here while open.
bool sKb = false;
bool kbOpen() {
    if (sKb || !MpChat::typedAllowed()) return false;
    char16_t t16[48];
    txt::format(t16, 48, MP_T_KB_CHAT, txt::Args());
    char t8[3 * 48 + 1];
    int n = 0;
    for (int i = 0; t16[i] && n < static_cast<int>(sizeof t8) - 4; i++) {
        const char16_t c = t16[i];
        if (txt::isPicChar(c)) continue;
        if (c < 0x80) t8[n++] = static_cast<char>(c);
        else if (c < 0x800) { t8[n++] = static_cast<char>(0xC0 | (c >> 6)); t8[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
        else { t8[n++] = static_cast<char>(0xE0 | (c >> 12)); t8[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F)); t8[n++] = static_cast<char>(0x80 | (c & 0x3F)); }
    }
    t8[n] = 0;
    if (!mp_keyboard_open(t8, "", MP_CHAT_TEXT_CHARS, false)) {
        mp_log("ui: chat keyboard could not open");
        return false;
    }
    sKb = true;
    mp_log("ui: chat keyboard open (ring)");
    return true;
}
void kbStep() {
    if (!sKb) return;
    char out[4 * MP_CHAT_TEXT_CHARS + 8];
    memset(out, 0, sizeof out);
    const int r = mp_keyboard_poll(out, sizeof out);
    if (r == 0) return;
    sKb = false;
    out[sizeof out - 1] = 0;
    if (r > 0 && out[0]) {
        const bool sent = MpChat::sendText(out);
        mp_log("ui: chat keyboard done (%s)", sent ? "sent" : "not sent");
    } else {
        mp_log("ui: chat keyboard cancelled");
    }
}

// ---------------------------------------------------------------- the quick ring
bool ringAllowed() {
    return mp_active() && MpGame::isGameSceneLive() && !MpUi::isMenuOpen() && !mp_dialog_active() && port_input_tilt_active() == 0 && !sKb;
}

void ringClose(int phase, int turns) {
    sRingPhase = phase;
    sRingLeft = turns;
}

void stepRing() {
    uint32_t hold = sRawHold | sInjHold, trig = sRawFresh ? (sRawTrig | sInjTrig) : sInjTrig;
    sRawFresh = false;
    sInjTrig = 0;
    const bool allowed = ringAllowed();
    sRingAllowed = allowed;
    if (!allowed) {
        if (sRingPhase == RING_OPEN) mp_log("ui: chat ring closed (not now)");
        if (sRingPhase != RING_CLOSED) ringClose(RING_CLOSED, 0);
        return;
    }
    ++sRingAge;
    if (sRingPhase != RING_OPEN) {
        if (sRingPhase != RING_CLOSED && --sRingLeft <= 0) sRingPhase = RING_CLOSED;
        if (!(trig & WPAD_BUTTON_MINUS)) return;
        sRingPhase = RING_OPEN;   // (a direction pressed on the same frame as Select picks at once, below)
        sRingAge = 0;
        sRingPicked = -1;
        MR::startSystemSE("SE_SY_SELECT_PAUSE_ITEM", -1, -1);
        mp_log("ui: chat ring open");
    }
    switch (sRingPhase) {
    case RING_OPEN: {
        int dir = -1;
        if (trig & WPAD_BUTTON_UP) dir = 0;
        else if (trig & WPAD_BUTTON_RIGHT) dir = 1;
        else if (trig & WPAD_BUTTON_DOWN) dir = 2;
        else if (trig & WPAD_BUTTON_LEFT) dir = 3;
        if (dir == MpChatLogic::kWriteDir) {   // ingame: "Write a Message": the console's own keyboard
            mp_log("ui: chat ring pick dir=%d write", dir);
            if (kbOpen()) {
                MR::startSystemSE("SE_SY_TALK_OK", -1, -1);
                sRingPicked = dir;
                ringClose(RING_PICKED, 16);
            } else {
                MR::startSystemSE("SE_SY_CAMERA_NG", -1, -1);
                ringClose(RING_FADE, 8);
            }
        } else if (dir >= 0) {
            const int phrase = sRing.slot[dir];
            mp_log("ui: chat ring pick dir=%d phrase=%d", dir, phrase);
            if (MpChat::sendPhrase(phrase)) {
                MR::startSystemSE("SE_SY_TALK_OK", -1, -1);
                sRingPicked = dir;
                ringClose(RING_PICKED, 16);
            } else {
                MR::startSystemSE("SE_SY_CAMERA_NG", -1, -1);
                ringClose(RING_FADE, 8);
            }
        } else if (!(hold & WPAD_BUTTON_MINUS)) {
            ringClose(RING_FADE, 8);   // let go without a direction: nothing sent
        }
        return;
    }
    default: ringClose(RING_CLOSED, 0); return;
    }
}

// ---------------------------------------------------------------- the desktop test hook
#if defined(TARGET_PC) && !defined(__vita__)
struct TestEvent {
    int turn;            // live turns of the scene
    int serial;          // from this flow serial on
    char what[48];
    bool done;
};
TestEvent sTest[8];
int sTestCount = -1;
uint32_t sLiveTurns = 0, sSceneLiveTurns = 0;
void testSceneStart() { sSceneLiveTurns = 0; }
int sTestRingDir = -1, sTestRingTurn = 0;

void testParse() {
    sTestCount = 0;
    const char* e = getenv("SMG_MP_TEST_CHAT");
    if (!e || !*e) return;
    const char* p = e;
    while (*p && sTestCount < 8) {
        const char* end = strchr(p, ',');
        const size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
        char item[64];
        const size_t c = n < sizeof item - 1 ? n : sizeof item - 1;
        memcpy(item, p, c);
        item[c] = 0;
        char* colon = strchr(item, ':');
        if (colon) {
            *colon = 0;
            TestEvent& t = sTest[sTestCount++];
            t.turn = static_cast<int>(atof(item) * 60.0);
            const char* at = strchr(item, '@');
            t.serial = at ? atoi(at + 1) : 0;
            snprintf(t.what, sizeof t.what, "%s", colon + 1);
            t.done = false;
        }
        p += n;
        if (*p == ',') ++p;
    }
    mp_log("chat test: %d events (SMG_MP_TEST_CHAT)", sTestCount);
}

void testTick() {
    if (sTestCount < 0) testParse();
    sInjHold = 0;
    if (!MpGame::isGameSceneLive()) return;
    ++sLiveTurns;
    ++sSceneLiveTurns;
    if (sTestRingDir >= 0) {
        // Select held from the turn after the event; the direction pressed 1 s later (the ring stays up long enough for a 2 s screenshot)
        const int age = static_cast<int>(sLiveTurns) - sTestRingTurn;
        sInjHold = WPAD_BUTTON_MINUS;
        if (age == 1) sInjTrig |= WPAD_BUTTON_MINUS;
        if (age == 61) {
            static const uint32_t kDir[4] = {WPAD_BUTTON_UP, WPAD_BUTTON_RIGHT, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT};
            sInjTrig |= kDir[sTestRingDir & 3];
        }
        if (age >= 80) sTestRingDir = -1;
    }
    for (int i = 0; i < sTestCount; i++) {
        TestEvent& t = sTest[i];
        if (t.done || static_cast<int>(sSceneLiveTurns) < t.turn || static_cast<int>(MpGameInternal::serial()) < t.serial) continue;
        t.done = true;
        mp_log("chat test: %s", t.what);
        if (!strncmp(t.what, "text:", 5)) {
            MpChat::sendText(t.what + 5);
        } else if (!strncmp(t.what, "ring", 4)) {
            sTestRingDir = atoi(t.what + 4) & 3;
            sTestRingTurn = static_cast<int>(sLiveTurns);
        } else if (!strncmp(t.what, "mute:", 5)) {
            MpChat::setMuted(atoi(t.what + 5), true);
        } else if (!strncmp(t.what, "unmute:", 7)) {
            MpChat::setMuted(atoi(t.what + 7), false);
        } else {
            MpChat::sendPhrase(atoi(t.what));
        }
    }
}
#else
void testTick() {}
void testSceneStart() {}
#endif

}  // namespace

namespace MpChat {

namespace { bool sKnownPublic = false; }   // (kept for the menu; no longer blocks typed text)
void noteRoomPublic(bool isPublic) { sKnownPublic = isPublic; }
// ingame (owner, 12 Nov): typed messages in every kind of game, a Quick Match / public room too: only the host's own Typed Messages switch decides
bool typedAllowed() { return mp_active() && mp_chat_typed_allowed(mp_settings()); }

bool muted(int slot) {
    ensureInit();
    return sMute.muted(slot);
}

void setMuted(int slot, bool on) {
    ensureInit();
    if (slot < 0 || slot >= MP_MAX_PLAYERS || slot == mp_local_slot()) return;
    if (sMute.muted(slot) == on) return;
    sMute.set(slot, on, mp_slot_name(slot), sidOf(slot));   // ingame: remembered for that player for the session (a reconnect stays hidden)
    if (on && sBalloon[slot] != nullptr && !MR::isDead(sBalloon[slot])) sBalloon[slot]->kill();
    sPend[slot].on = sPend[slot].on && !on;
    mp_log("ui: chat mute slot=%d on=%d", slot, on ? 1 : 0);
    mp_log("chat hide slot=%d target=%d on=%d", mp_local_slot(), slot, on ? 1 : 0);   // hooks: the scenarios' line (the hiding is this console's only)
}

bool sendPhrase(int phrase) {
    ensureInit();
    if (!mp_active() || phrase < 0 || phrase >= MP_CHAT_PHRASES) return false;
    if (!mp_chat_bucket_take(&sSend, static_cast<uint64_t>(nowMs()), MP_CHAT_SEND_PERIOD_MS, MP_CHAT_SEND_BURST)) {
        mp_log("ui: chat not sent id=%d (too soon)", phrase);
        return false;
    }
    MpChatMsg m;
    memset(&m, 0, sizeof m);
    m.kind = MP_CHAT_PHRASE;
    m.phrase = static_cast<uint8_t>(phrase);
    if (!sendMsg(m)) {
        mp_log("ui: chat NOT sent id=%d", phrase);
        return false;
    }
    const int at = sRing.used(phrase);
    mp_log("ui: chat sent id=%d ring=%d", phrase, at);
    if (MpSeriesLogic::isAgainAsk(m.kind, m.phrase)) MpOverlay::noteAskSentByHand();   // menus4: on a guest's results card this phrase is the rematch ask (no-op when the card's own row sent it)
    show(mp_local_slot(), m, true, true);
    if (phrase == MP_CHAT_PHRASE_BRB) MpAway::brb();   // MP3 touches: "Be right back" = away until we move
    return true;
}

bool sendText(const char* utf8) {
    ensureInit();
    if (!utf8 || !mp_active()) return false;
    if (!typedAllowed()) {
        mp_log("ui: chat text refused (typed messages are off in this room)");
        return false;
    }
    MpChatMsg m;
    memset(&m, 0, sizeof m);
    if (!mp_chat_make_text(&m, utf8, strlen(utf8))) {
        mp_log("ui: chat text refused (nothing left after cleaning)");
        return false;
    }
    if (!mp_chat_bucket_take(&sSend, static_cast<uint64_t>(nowMs()), MP_CHAT_SEND_PERIOD_MS, MP_CHAT_SEND_BURST)) {
        mp_log("ui: chat not sent id=text (too soon)");
        return false;
    }
    if (!sendMsg(m)) {
        mp_log("ui: chat NOT sent id=text");
        return false;
    }
    int chars = 0;
    for (int i = 0; i < m.len; i++)
        if ((static_cast<uint8_t>(m.text[i]) & 0xC0) != 0x80) ++chars;
    mp_log("ui: chat sent id=text chars=%d bytes=%d", chars, m.len);
    show(mp_local_slot(), m, true, true);
    return true;
}

// MP3 ingame
bool balloonUp(int slot) {
    if (slot < 0 || slot >= MP_MAX_PLAYERS || !sInit) return false;
    if (sPend[slot].on) return true;
    const MpChatBalloon* b = sBalloon[slot];
    return b != nullptr && !MR::isDead(b) && !b->mClosing;
}

void greetingBalloon(int slot, int stamp) {
    ensureInit();
    if (slot < 0 || slot >= MP_MAX_PLAYERS || stamp < 0 || stamp > 3) return;
    if (slot != mp_local_slot() && sMute.muted(slot)) return;
    MpChatMsg m;
    memset(&m, 0, sizeof m);
    m.kind = MP_CHAT_PHRASE;
    m.phrase = static_cast<uint8_t>(stamp);   // (the Messages page's phrases 0..3 are the four greetings' words)
    show(slot, m, false, true);
}

bool handleMessage(const MpMsg& msg) {
    if (msg.type != MP_MSG_CHAT) return false;
    ensureInit();
    const int from = msg.fromSlot;
    MpChatMsg m;
    MpReader r(msg.data, msg.len <= MP_MAX_PAYLOAD ? msg.len : 0);
    if (from >= MP_MAX_PLAYERS || !mp_read_chat(r, &m)) {
        mp_log("ui: chat dropped from=%d (bad payload)", from);
        return true;
    }
    if (from == mp_local_slot()) return true;   // (ours: already shown when sent)
    if (m.kind == MP_CHAT_TEXT && !typedAllowed()) {
        mp_log("ui: chat dropped from=%d kind=text (typed messages are off in this room)", from);
        return true;
    }
    if (!mp_chat_bucket_take(&sShowLimit[from], static_cast<uint64_t>(nowMs()), MP_CHAT_SHOW_PERIOD_MS, MP_CHAT_SHOW_BURST)) {
        if (sShowDrops++ < 8 || (sShowDrops % 32) == 0) mp_log("ui: chat dropped from=%d (too many)", from);
        return true;
    }
    const char* kind = m.kind == MP_CHAT_PHRASE ? "phrase" : "text";
    const MpChatLogic::Show d = MpChatLogic::decide(m.kind, sMute.muted(from));
    if (!d.toast && !d.balloon) {
        mp_log("ui: chat hidden from=%d kind=%s (muted)", from, kind);
        return true;
    }
    show(from, m, d.toast, d.balloon);
    if (MpSeriesLogic::isAgainAsk(m.kind, m.phrase)) MpOverlay::noteRaceAgainAsk(from);   // lobby: on a leader's open results card a guest's "Let's go!" is the ask for a rematch
    if (m.kind == MP_CHAT_PHRASE) mp_log("ui: chat shown from=%d kind=phrase id=%d%s", from, m.phrase, d.balloon ? "" : " (muted: no balloon)");
    else mp_log("ui: chat shown from=%d kind=text chars=%d", from, static_cast<int>(m.len));
    return true;
}

void initScene() {
    ensureInit();
    testSceneStart();
    sSceneBalloons = false;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) sBalloon[i] = nullptr;
    if (!MR::isLoadedObjectOrLayoutArchive("TalkBalloonStretch")) {
        mp_log("ui: chat balloons off (no TalkBalloonStretch)");
        return;
    }
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        MpChatBalloon* b = new MpChatBalloon();
        b->initWithoutIter();
        sBalloon[i] = b;
    }
    sSceneBalloons = true;
}

void endScene() {
    for (int i = 0; i < MP_MAX_PLAYERS; i++) sBalloon[i] = nullptr;
    sSceneBalloons = false;
    if (sRingPhase != RING_CLOSED) ringClose(RING_CLOSED, 0);
    sRingAllowed = false;
}

void sessionTick() {
    ensureInit();
    if (!mp_active()) {
        if (sRingPhase != RING_CLOSED) ringClose(RING_CLOSED, 0);
        sRingAllowed = false;
        // ingame: the session is over (not a silent rejoin): its "Hide messages" choices go with it
        if (!MpUi::reconnecting() && sMute.keyCount > 0) {
            sMute.reset();
            mp_log("ui: chat hidden players forgotten (session over)");
        }
        return;
    }
    sMute.sync([](int s) { return mp_slot_present(s); }, [](int s) { return mp_slot_name(s); }, sidOf);   // ingame: per player (by the stable id)
}

void update() {
    ensureInit();
    sessionTick();
    if (!mp_active()) return;
    testTick();
    MpAway::tick();   // MP3 touches: away (Be right back / idle)
    kbStep();   // ingame: the ring's keyboard
    stepRing();
    if (sSceneBalloons) stepBalloons();
}

bool ringModel(MpPanel::Ring* out) {
    if (sRingPhase == RING_CLOSED || !out) return false;
    txt::setLanguage(txt::languageIndex(MR::getCurrentLanguagePrefix()));
    for (int i = 0; i < 4; i++) {
        if (i == MpChatLogic::kWriteDir) txt::format(out->label[i], MpPanel::kCellText, MP_T_WRITE, txt::Args());   // ingame
        else phraseText(sRing.slot[i], out->label[i], MpPanel::kCellText);
    }
    out->picked = sRingPhase == RING_PICKED ? sRingPicked : -1;
    if (sRingPhase == RING_OPEN) {
        out->alpha = sRingAge >= 5 ? 1.0f : (sRingAge + 1) / 6.0f;
        out->pop = 0.0f;
    } else {
        out->alpha = sRingLeft > 6 ? 1.0f : sRingLeft / 6.0f;
        out->pop = sRingPhase == RING_PICKED ? (sRingLeft > 8 ? 1.0f : sRingLeft / 8.0f) : 0.0f;
    }
    return out->alpha > 0.0f;
}

void filterPad(uint32_t* hold, uint32_t* trigger, uint32_t* release, uint32_t* repeat) {
    sRawHold = *hold;
    sRawTrig = *trigger;
    sRawFresh = true;
    if (!sRingAllowed) return;
    uint32_t mask = WPAD_BUTTON_MINUS;   // in play Select is the ring's (START opens the session menu)
    if (sRingPhase == RING_OPEN || (sRawHold & WPAD_BUTTON_MINUS))   // (while Select is held the directions pick, the camera stays)
        mask |= WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT;
    *hold &= ~mask;
    *trigger &= ~mask;
    *release &= ~mask;
    *repeat &= ~mask;
}

}  // namespace MpChat
