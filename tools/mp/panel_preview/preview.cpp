// Host side of tools/mp/panel_preview/render.py: runs the real menu state machine (decomp/src/Game/MP/MpMenuSequence.cpp,
// MpToast.cpp, MpMessages.cpp) and composer (MpPanelPlan.cpp) against a fake port (the mp_* bridge, MpSaveGame) to reach every
// screen, and prints each screen's layout uses and GX plan as text. Font metrics come from a file render.py writes from the game's
// own fonts (local data, never part of the tree).
//   preview <metrics-file> <screen> <language prefix>      screens: see kScreens; "list" prints them
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "Game/MP/MpChat.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPartyLogic.hpp"
#include "Game/MP/MpUi.hpp"

// ------------------------------------------------------------------ a fake port
namespace fake {
MpUiStatus status = {MP_UI_IDLE, MP_ERR_NONE, 1, "", 0};
bool active = false, host = true;
int local = 0, hostSlot = 0;
MpMode mode = MP_MODE_TOGETHER;
struct P { bool present; const char* name; MpLook look; int ping; bool quiet; } players[4] = {
    {true, "Player", MP_LOOK_MARIO, -1, false}, {true, "Luigi64", MP_LOOK_COSMIC_LUIGI, 38, false},
    {true, "Rosalina", MP_LOOK_YELLOW_TOAD, 140, false}, {false, "Toadsworth", MP_LOOK_TOAD, 60, false}};
bool recovery = false;
MpProfile profile = {"Player", "4827-1193", true};
int friendsState = 0;   // count, -1 loading
MpFriend friends[] = {{"Luigi64", "1234-5678", true, true, 1}, {"StarBunny", "2222-3333", true, false, 0}, {"Rosalina", "9876-5432", false, false, 0},
                      {"Toadsworth", "5555-1111", false, false, 0}};
MpFriend incoming[] = {{"Daisy", "5555-6666", false, false, 0}, {"Wario", "7777-8888", false, false, 0}, {"Waluigi", "1111-2222", false, false, 0}};
int incomingCount = 3;   // friendsui: players who asked to be friends
bool hostByAddr = false;   // netx: the lobby of a room hosted by address
MpAddressInfo addrInfo = {"203.0.113.77:41100", MP_ADDR_SRC_ROUTER, 2, false, 41100};
int boardState = 6;
MpBoardRow board[] = {{1, "Rosalina", 4 * 60 * 60 + 1234, 1, true, false}, {2, "Player", 4 * 60 * 60 + 2210, 2, true, true},
                      {3, "Luigi64", 4 * 60 * 60 + 3005, 1, true, false}, {4, "StarBunny", 5 * 60 * 60 + 12, 1, false, false},
                      {5, "Toadsworth", 5 * 60 * 60 + 2020, 3, true, false}, {6, "Polari", 6 * 60 * 60 + 300, 1, false, false}};
MpLocalRoom rooms[] = {{"Luigi64", 1, 2, 4, true}, {"Rosalina", 2, 1, 2, true}, {"OldVersion", 1, 1, 4, false}};
int roomCount = 3;
char kbText[64] = "";
int kbResult = 0;
std::string lastDialog;
}  // namespace fake

extern "C" {
bool mp_active(void) { return fake::active; }
MpMode mp_mode(void) { return fake::active ? fake::mode : MP_MODE_NONE; }
bool mp_is_host(void) { return fake::host; }
int mp_local_slot(void) { return fake::active ? fake::local : -1; }
int mp_host_slot(void) { return fake::active ? fake::hostSlot : -1; }
uint16_t mp_host_epoch(void) { return 0; }
bool mp_slot_present(int s) { return s >= 0 && s < 4 && fake::players[s].present; }
const char* mp_slot_name(int s) { return s >= 0 && s < 4 ? fake::players[s].name : ""; }
MpLook mp_slot_look(int s) { return s >= 0 && s < 4 ? fake::players[s].look : MP_LOOK_MARIO; }
int mp_slot_ping_ms(int s) { return s >= 0 && s < 4 ? fake::players[s].ping : -1; }
bool mp_slot_quiet(int s) { return s >= 0 && s < 4 && fake::players[s].quiet; }
const MpSettings* mp_settings(void) { static MpSettings s; return &s; }
uint16_t mp_scene_serial(void) { return 0; }
bool mp_send(uint8_t, const void*, uint16_t, bool) { return true; }
bool mp_send_to(int, uint8_t, const void*, uint16_t, bool) { return true; }
bool mp_poll(MpMsg*) { return false; }
uint32_t mp_poll_many(MpMsg*, uint32_t) { return 0; }
void mp_tick_game(uint32_t) {}
bool mp_save_suppressed(void) { return false; }
bool mp_save_autoconfirm(void) { return false; }
void mp_snapshot_offer(const void*, uint32_t) {}
uint32_t mp_snapshot_ready(void) { return 0; }
uint32_t mp_snapshot_get(void*, uint32_t) { return 0; }
void mp_snapshot_consumed(void) {}
uint8_t mp_core_saved_look(void) { return MP_LOOK_MARIO; }
uint8_t mp_core_saved_downed(void) { return 0; }
void mp_core_save_downed(uint8_t) {}
bool mp_core_rules_wanted(uint32_t) { return false; }
bool mp_core_rules_hidden() { return true; }
void mp_core_rules_done(uint32_t, bool) {}
bool mp_core_update_settings(const MpSettings*) { return true; }
void mp_ui_set_profile(const char*, MpLook) {}
bool mp_ui_host(const MpHostParams*) { fake::status.phase = MP_UI_CREATING; return true; }
bool mp_ui_join_code(MpConn, const char*) { fake::status.phase = MP_UI_CONNECTING; return true; }
bool mp_ui_join_address(const char*) { fake::status.phase = MP_UI_CONNECTING; return true; }   // netx
bool mp_ui_host_address(MpAddressInfo* out) {
    if (!fake::hostByAddr) return false;
    *out = fake::addrInfo;
    return true;
}
bool mp_ui_join_friend(const char*) { fake::status.phase = MP_UI_CONNECTING; return true; }
bool mp_ui_quick_match(MpMode) { fake::status.phase = MP_UI_SEARCHING; return true; }
bool mp_ui_join_local(MpConn, int) { fake::status.phase = MP_UI_JOINING; return true; }
bool mp_ui_join_local_room(MpConn, uint32_t) { fake::status.phase = MP_UI_JOINING; return true; }
bool mp_ui_start(void) { return true; }
void mp_ui_leave(void) { fake::status.phase = MP_UI_IDLE; fake::active = false; }
void mp_ui_kick(int) {}
void mp_ui_status(MpUiStatus* out) { *out = fake::status; }
int mp_ui_local_scan(MpConn) { return fake::roomCount; }
bool mp_ui_local_get(int i, MpLocalRoom* out) { if (i < 0 || i >= fake::roomCount) return false; *out = fake::rooms[i]; return true; }
void mp_ui_profile(MpProfile* out) { *out = fake::profile; }
bool mp_ui_register(const char*) { return true; }
bool mp_ui_friends_request(void) { return true; }
int mp_ui_friends_get(MpFriend* rows, int max) {
    if (fake::friendsState < 0) return fake::friendsState;
    const int n = fake::friendsState < max ? fake::friendsState : max;
    for (int i = 0; i < n; i++) rows[i] = fake::friends[i];
    return n;
}
static void dropIncoming(const char* code) {
    for (int i = 0; i < fake::incomingCount; i++)
        if (!strcmp(fake::incoming[i].code, code)) {
            for (int k = i; k + 1 < fake::incomingCount; k++) fake::incoming[k] = fake::incoming[k + 1];
            --fake::incomingCount;
            break;
        }
}
bool mp_ui_friend_add(const char* c) { dropIncoming(c); return true; }
bool mp_ui_friend_remove(const char*) { return true; }
int mp_ui_incoming_get(MpFriend* rows, int max) {
    if (fake::friendsState < 0) return fake::friendsState;
    const int n = fake::incomingCount < max ? fake::incomingCount : max;
    for (int i = 0; i < n; i++) rows[i] = fake::incoming[i];
    return n;
}
bool mp_ui_friend_dismiss(const char* c) { dropIncoming(c); return true; }
bool mp_ui_friend_block(const char* c) { dropIncoming(c); return true; }
bool mp_ui_friend_unblock(const char*) { return true; }
bool mp_ui_board_request(uint8_t, uint8_t, uint8_t) { return true; }
int mp_ui_board_get(MpBoardRow* rows, int max) {
    if (fake::boardState < 0) return fake::boardState;
    const int n = fake::boardState < max ? fake::boardState : max;
    for (int i = 0; i < n; i++) rows[i] = fake::board[i];
    return n;
}
void mp_race_submit(uint8_t, uint8_t, uint32_t, const uint8_t*) {}
bool mp_keyboard_open(const char*, const char*, int, bool) { return true; }
int mp_keyboard_poll(char* out, int cap) { snprintf(out, cap, "%s", fake::kbText); return fake::kbResult; }
bool mp_dialog_active(void) { return false; }
void mp_log(const char* fmt, ...) {
    if (!getenv("PREVIEW_LOG")) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[mp] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
}

namespace MpSaveGame {
bool enterSessionRoot() { return true; }
bool leaveSessionRoot() { return true; }
bool isSessionRoot() { return false; }
bool heldAfterSession() { return false; }
bool importSinglePlayerFile(int, int) { return true; }
bool writeBackToSinglePlayer(int, int) { return true; }
bool backupSinglePlayerFile() { return true; }
void beginSinglePlayerSession(int) {}
int compareImportProgress(int, int) { return 1; }
int slotStars(bool coop, int f) {
    const int stars[7] = {-1, 23, -1, 61, -1, -1, -1};
    return coop ? (f >= 1 && f <= 6 ? stars[f] : -1) : 42;
}
bool offerSaveOnLeave(LeaveOffer* o) {
    *o = LeaveOffer();
    o->offer = o->coop = true;
    o->single = true;
    o->singleSlot = 2;
    o->sessionSlot = 1;
    o->stars = 30;
    const int stars[6] = {23, -1, 61, -1, -1, -1};
    for (int i = 0; i < 6; i++) o->coopUsed[i] = stars[i] >= 0, o->coopStars[i] = stars[i];
    return true;
}
bool saveOnLeave(int, int) { return true; }
bool recoveryPending(RecoveryInfo* out) {
    if (!fake::recovery) return false;
    *out = RecoveryInfo();
    out->stars = 18;
    out->single = false;
    return true;
}
bool recoveryKeep(int, int) { return true; }
bool recoveryDiscard() { return true; }
int sessionSlot() { return 1; }
void abandonHostChoice() {}
}  // namespace MpSaveGame

namespace MpUi {
void sendStamp(int) {}
}
namespace fake {
bool chatTyped = true;
bool chatMuted[4] = {false, false, false, false};
}
namespace MpChat {   // (chat: the module's menu side, faked)
void noteRoomPublic(bool) {}
bool sendPhrase(int) { return true; }
bool sendText(const char*) { return true; }
bool typedAllowed() { return fake::active && fake::chatTyped; }
bool muted(int s) { return s >= 0 && s < 4 && fake::chatMuted[s]; }
void setMuted(int s, bool on) { if (s >= 0 && s < 4) fake::chatMuted[s] = on; }
}

// ------------------------------------------------------------------ the fake game side of the menus
struct FakeIo : MpMenu::Io {
    MpMenu::DlgState next = MpMenu::DlgState::Yes;
    bool open = false;
    void sound(MpMenu::Se) override {}
    void dialogOpen(MpMenu::Dlg, const char* id) override { open = true; fake::lastDialog = id; }
    MpMenu::DlgState dialogState() override {
        if (!open) return MpMenu::DlgState::None;
        open = false;
        return next;
    }
    int galaxyCount() override { return MpMsgText::galaxyCount(); }
    int galaxyName(int, char16_t* out, int cap) override { MpPanel::copy(out, cap, u"Good Egg Galaxy"); return 15; }
    int scenarioCount(int) override { return 7; }
    int scenarioName(int, int s, char16_t* out, int cap) override {
        static const char16_t* const k[] = {u"Dino Piranha", u"A Snack of Cosmic Proportions", u"King Kaliente's Battle Fleet",
                                            u"Luigi on the Roof", u"Purple Coin Omelet", u"Sweet Mystery Galaxy", u"Dino Piranha Speed Run"};
        MpPanel::copy(out, cap, k[(s - 1) % 7]);
        return 1;
    }
};
FakeIo gIo;

// ------------------------------------------------------------------ font metrics (from render.py)
struct FontFile { std::map<int, float> advance; };
FontFile gFonts[2];
float Advance(void* c, uint16_t code) {
    auto* f = static_cast<FontFile*>(c);
    auto it = f->advance.find(code);
    return it == f->advance.end() ? 0.0f : it->second;
}
bool Has(void* c, uint16_t code) { return static_cast<FontFile*>(c)->advance.count(code) != 0; }

// ------------------------------------------------------------------ driving the menus
MpMenu::Sequence& seq() { return MpMenu::sequence(); }
void step(MpMenu::Input in = MpMenu::Input()) { seq().update(in); MpOverlay::update(in.ok); }
void frames(int n) { for (int i = 0; i < n; i++) step(); }
void ok() { MpMenu::Input in; in.ok = true; step(in); frames(2); }
void back() { MpMenu::Input in; in.back = true; step(in); frames(2); }
void down(int n = 1) { for (int i = 0; i < n; i++) { MpMenu::Input in; in.down = true; step(in); } frames(2); }
void tri() { MpMenu::Input in; in.tri = true; step(in); frames(2); }
void settle() { frames(12); }   // (the fade-in)

void openMain() { seq().openMain(2); frames(2); /* the Wi-Fi tip window answers */ settle(); }
void joinSession(bool asHost, int players) {
    fake::active = true;
    fake::host = asHost;
    fake::local = asHost ? 0 : 1;
    for (int i = 0; i < 4; i++) fake::players[i].present = i < players;
    fake::status.phase = MP_UI_LOBBY;
    snprintf(fake::status.roomCode, sizeof fake::status.roomCode, "K7QM4D");
}

MpPanel::Strip gStrip;
bool gStripOn = false;
MpPanel::Ring gRing;   // chat: the quick ring
bool gRingOn = false;

// MP3 celebrate: the party strip (the other players' heads beside the life counter) in each state, and the team star card.
bool reachParty(const std::string& s) {
    using namespace MpPartyLogic;
    if (s == "party" || s == "party-bubble" || s == "party-down") {
        joinSession(true, 4);
        PartyInput in;
        in.localSlot = 0;
        in.presentMask = 0xF;
        for (int i = 0; i < 4; i++) in.look[i] = static_cast<uint8_t>(fake::players[i].look), in.state[i] = MpPanel::PARTY_NORMAL, in.flash[i] = -1.0f, in.link[i] = MpPanel::LINK_GOOD, in.away[i] = false;
        if (s == "party") in.state[2] = MpPanel::PARTY_QUIET, in.flash[1] = 0.32f, in.link[1] = MpPanel::LINK_WEAK, in.link[2] = MpPanel::LINK_LOST, in.away[3] = true;   // (touches: bars, away)
        if (s == "party-bubble") in.state[1] = MpPanel::PARTY_BUBBLE, in.state[3] = MpPanel::PARTY_QUIET, in.flash[2] = 0.1f;
        if (s == "party-down") in.state[3] = MpPanel::PARTY_DOWN, in.state[1] = MpPanel::PARTY_BUBBLE;
        makeStrip(in, 1.0f, &gStrip);
        gStripOn = true;
        return true;
    }
    if (s == "starcard" || s == "starcard-team") {
        joinSession(true, 4);
        MpStarCard c;
        memset(&c, 0, sizeof c);
        c.count = 4;
        c.by = 1;
        c.team = s == "starcard-team";
        const MpStarCard::Entry es[4] = {{0, true, 14, 23, 1}, {1, true, 3, 41, 0}, {2, true, 27, 8, 2}, {3, c.team, 0, 5, 0}};
        for (int i = 0; i < 4; i++) c.e[i] = es[i];
        MpOverlay::showStarCard(c);
        frames(14);
        return true;
    }
    return false;
}

bool reach(const std::string& s) {
    if (reachParty(s)) return true;
    fake::status = {MP_UI_IDLE, MP_ERR_NONE, 1, "", 0};
    if (s == "main") { openMain(); return true; }
    if (s == "connection") { openMain(); ok(); settle(); return true; }
    if (s == "chars") { openMain(); ok(); ok(); settle(); return true; }
    if (s == "hostjoin") { openMain(); ok(); ok(); ok(); settle(); return true; }
    if (s == "hostjoin-local") { openMain(); down(); ok(); down(); ok(); ok(); settle(); return true; }
    if (s == "setup") { openMain(); ok(); ok(); ok(); ok(); settle(); return true; }
    if (s == "setup-race") { openMain(); down(); ok(); ok(); ok(); ok(); settle(); return true; }
    if (s == "more") { openMain(); ok(); ok(); ok(); ok(); down(4); ok(); settle(); return true; }
    if (s == "more-race") { openMain(); down(); ok(); ok(); ok(); ok(); down(3); ok(); settle(); return true; }
    if (s == "save") { openMain(); ok(); ok(); ok(); ok(); down(4); ok(); ok(); settle(); return true; }
    if (s == "rooms") { openMain(); ok(); down(); ok(); ok(); down(); ok(); frames(31); settle(); return true; }
    if (s == "recovery") { fake::recovery = true; openMain(); frames(2); settle(); return true; }
    if (s == "friends") { openMain(); down(3); ok(); frames(2); settle(); fake::friendsState = 4; frames(2); return true; }
    if (s == "friends-none" || s == "requests" || s == "request" || s == "blocked" || s == "blocked-empty") {   // friendsui
        if (s == "friends-none") fake::incomingCount = 0;
        openMain(); down(3); ok(); frames(2); settle(); fake::friendsState = 4; frames(2);
        if (s == "requests" || s == "request") ok(), settle();
        if (s == "request") ok(), settle();
        if (s == "blocked") {   // Block the first request (the window answers Yes), then Friends > Blocked Players
            ok(); ok(); down(2); ok(); settle(); back(); down(5); ok(); settle();
        }
        if (s == "blocked-empty") { down(5); ok(); settle(); }
        if (s == "friends-none") settle();
        return true;
    }
    if (s == "board") { openMain(); down(2); ok(); frames(2); settle(); return true; }
    if (s == "busy") {
        openMain(); ok(); ok(); ok(); down(2); ok(); ok();   // Quick Match (fix2: then its save question: the first choice)
        frames(30); settle(); return true;
    }
    if (s == "busy-receiving") {
        openMain(); ok(); ok(); ok(); down(2); ok(); ok();
        fake::status.phase = MP_UI_CONNECTING; fake::status.progress = 45; frames(30); settle(); return true;
    }
    if (s == "lobby-host" || s == "chars-taken") {
        openMain(); ok(); ok(); ok(); ok(); down(5); ok();   // open the room
        joinSession(true, 3); frames(3); settle();
        if (s == "chars-taken") { tri(); MpMenu::Input in; in.right = true; step(in); settle(); }   // Triangle; the cursor on Luigi
        return true;
    }
    if (s == "address") { openMain(); ok(); ok(); ok(); tri(); settle(); return true; }   // netx: Online > Triangle: Play by Address
    if (s == "lobby-address" || s == "lobby-address-forward") {
        openMain(); ok(); ok(); ok(); tri(); ok(); down(5); ok();
        fake::hostByAddr = true;
        if (s == "lobby-address-forward") fake::addrInfo.source = MP_ADDR_SRC_STUN, fake::addrInfo.portmap = 3;
        joinSession(true, 2); fake::status.roomCode[0] = 0; frames(3); settle(); return true;
    }
    if (s == "lobby-guest") {
        openMain(); ok(); ok(); ok(); down(2); ok(); ok();
        joinSession(false, 2); frames(3); settle(); return true;
    }
    if (s == "session" || s == "players" || s == "warp") {
        joinSession(true, 3);
        fake::status.phase = MP_UI_PLAYING;
        frames(3);
        seq().openSession(); settle();
        if (s == "players") { tri(); settle(); }
        if (s == "warp") { ok(); settle(); }
        return true;
    }
    // chat: the Messages page (a private room: Write a Message under the grid; a public one: the grid alone), Mute, the ring, toasts
    if (s == "messages" || s == "messages-public") {
        joinSession(s == "messages", 3);
        fake::chatTyped = s == "messages";
        fake::status.phase = MP_UI_PLAYING;
        frames(3);
        seq().openSession(); settle();
        down(); ok(); settle();
        if (s == "messages-public") { MpMenu::Input in; in.right = true; step(in); down(2); }
        return true;
    }
    if (s == "players-mute") {
        joinSession(false, 3);
        fake::status.phase = MP_UI_PLAYING;
        frames(3);
        seq().openSession(); settle();
        tri(); settle();
        ok();   // (mute the host, the first row)
        down(2); settle();
        return true;
    }
    if (s == "ring" || s == "ring-picked") {
        joinSession(false, 3);
        static const int kRing[4] = {0, 3, 2, 1};
        for (int i = 0; i < 4; i++) MpMsgText::format(gRing.label[i], MpPanel::kCellText, static_cast<MpText>(MP_T_PHRASE_0 + kRing[i]), MpMsgText::Args());
        gRing.alpha = 1.0f;
        gRing.picked = s == "ring-picked" ? 1 : -1;
        gRing.pop = s == "ring-picked" ? 0.6f : 0.0f;
        gRingOn = true;
        return true;
    }
    if (s == "chat-toasts") {
        joinSession(true, 3);
        const char* who[3] = {"Luigi64", "Rosalina", "Player"};
        const int slots[3] = {1, 2, 0};
        for (int i = 0; i < 3; i++) {
            char16_t line[96], body[64];
            MpMsgText::Args a;
            a.str = who[i];
            int n = MpMsgText::format(line, 96, MP_T_CHAT_FROM, a);
            if (i == 1) MpMsgText::utf8To16(body, 64, "Meet me by the big tree, past the bridge");
            else MpMsgText::format(body, 64, static_cast<MpText>(MP_T_PHRASE_0 + (i == 0 ? 7 : 4)), MpMsgText::Args());
            for (int k = 0; body[k] && n < 95; k++) line[n++] = body[k];
            line[n] = 0;
            MpOverlay::toastText(line, static_cast<uint8_t>(MpPanel::ICON_MARIO + fake::players[slots[i]].look));
        }
        frames(20);
        return true;
    }
    if (s == "slot-leave") {
        joinSession(false, 2);
        fake::status.phase = MP_UI_PLAYING; frames(3);
        fake::status.phase = MP_UI_FAILED; fake::status.error = MP_ERR_HOST_LEFT; fake::active = false;
        frames(2);   // the "host left" window, answered
        frames(2);   // "Save your progress?", answered Yes
        settle();
        return true;
    }
    if (s == "waiting") {
        joinSession(true, 3);
        fake::status.phase = MP_UI_PLAYING;
        MpOverlay::showWaiting(true, 1, 3, 154);
        MpOverlay::setStanding(1, 60 * 83 + 27);
        frames(3);
        return true;
    }
    if (s == "results") {
        joinSession(true, 3);
        MpMsg m;
        memset(&m, 0, sizeof m);
        MpWriter w(m.data, sizeof m.data);
        MpRaceResults r = {7, 3, {{1, 1, 60 * 83 + 27}, {0, 2, 60 * 95 + 3}, {2, 0, MP_RACE_DNF}}};
        mp_write_results(w, r);
        m.type = MP_MSG_RESULTS;
        m.len = static_cast<uint16_t>(w.n);
        MpOverlay::showResults(m);
        frames(12);
        return true;
    }
    if (s == "results-race" || s == "results-race-guest" || s == "results-race-chosen" || s == "results-race-left") {
        // race: the card with the series column and the host's next step (a RESULTS with a tail)
        const bool guest = s != "results-race" && s != "results-race-left";
        joinSession(!guest, 3);
        if (s == "results-race-left") {   // (the card has seen slot 3 in an earlier race: its name stays for the greyed line)
            fake::players[3].present = true;
            MpRaceResults e;
            e.serial = 6; e.count = 1; e.e[0] = {3, 1, 60 * 70};
            MpMsg em;
            memset(&em, 0, sizeof em);
            MpWriter ew(em.data, sizeof em.data);
            mp_write_results(ew, e);
            em.type = MP_MSG_RESULTS;
            em.len = static_cast<uint16_t>(ew.n);
            MpOverlay::showResults(em);
            frames(2);
            fake::players[3].present = false;
        }
        MpSeriesLogic::Tail t;
        t.series.in = s == "results-race-left" ? 0xF : 0x7;
        t.series.left = s == "results-race-left" ? 0x8 : 0;
        t.series.pts[0] = 5; t.series.pts[1] = 7; t.series.pts[2] = 1; t.series.pts[3] = s == "results-race-left" ? 4 : 0;
        t.series.streakSlot = 1; t.series.streakLen = 1;
        t.next = 2;
        MpRaceResults r;
        r.serial = 7; r.count = 3;
        r.e[0] = {1, 1, 60 * 83 + 27}; r.e[1] = {0, 2, 60 * 95 + 3}; r.e[2] = {2, 0, MP_RACE_DNF};
        r.hasTail = true; r.tail = t;
        MpMsg m;
        memset(&m, 0, sizeof m);
        MpWriter w(m.data, sizeof m.data);
        mp_write_results(w, r);
        m.type = MP_MSG_RESULTS;
        m.len = static_cast<uint16_t>(w.n);
        MpOverlay::showResults(m);
        if (s == "results-race-chosen") MpOverlay::setResultsChoice(MpSeriesLogic::CH_AGAIN);
        frames(40);
        return true;
    }
    if (s == "headstart") {
        joinSession(true, 3);
        MpOverlay::showHeadStart(1, 3);
        frames(4);
        return true;
    }
    if (s == "toasts") {
        joinSession(true, 3);
        MpOverlay::toast(MP_T_JOINED, 2, 0);
        MpOverlay::toast(MP_T_GOT_STAR, 1, 0);
        MpOverlay::toast(MP_T_FINISHED, 0, 60 * 83 + 27);
        frames(20);
        return true;
    }
    return false;
}

const char* const kScreens[] = {"main", "connection", "hostjoin", "hostjoin-local", "setup", "setup-race", "more", "more-race", "save", "rooms", "friends", "friends-none", "requests", "request", "blocked", "blocked-empty", "board",
                                "busy", "busy-receiving", "lobby-host", "address", "lobby-address", "lobby-address-forward", "lobby-guest", "session", "players", "messages", "messages-public", "players-mute",
                                "ring", "ring-picked", "chat-toasts", "warp", "slot-leave", "recovery",
                                "chars", "chars-taken",
                                "waiting", "results", "results-race", "results-race-guest", "results-race-chosen", "results-race-left", "headstart", "toasts", "party", "party-bubble", "party-down", "starcard", "starcard-team"};

void printScene(const char* tag, const MpPanel::Scene& sc, const MpPanel::PlanBase& plan) {
    printf("%s\n", tag);
    for (int i = 0; i < sc.textCount; i++) {
        printf("T %d", i);
        for (const char16_t* p = sc.texts[i]; *p; ++p) printf(" %d", *p);
        printf("\n");
    }
    for (int i = 0; i < sc.useCount; i++) {
        const MpPanel::LayoutUse& u = sc.uses[i];
        printf("U %d %d %d %d %d %.3f %.3f %.3f %.3f %.3f\n", u.kind, u.slot, u.state, u.text, u.text2, u.x, u.y, u.w, u.h, u.alpha);
    }
    for (int i = 0; i < plan.vertexCount; i++) {
        const auto& v = plan.vertices[i];
        printf("V %.3f %.3f %d %d %d %d\n", v.x, v.y, v.c.r, v.c.g, v.c.b, v.c.a);
    }
    for (int i = 0; i < plan.runCount; i++) {
        const auto& r = plan.runs[i];
        printf("R %d %.3f %.3f %.5f %.5f %d %d %d %d", r.font, r.x, r.y, r.scaleX, r.scaleY, r.color.r, r.color.g, r.color.b, r.color.a);
        for (int k = 0; k < r.length; k++) printf(" %d", plan.text[r.start + k]);
        printf("\n");
    }
    printf("E %d %d\n", plan.overflow ? 1 : 0, plan.textClipped ? 1 : 0);
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        for (const char* s : kScreens) printf("%s\n", s);
        return 0;
    }
    if (argc < 4) return 2;
    MpPanel::FontMetrics metrics[MpPanel::FONT_COUNT];   // (the display face stays empty: the preview falls back to the message font)
    FILE* f = fopen(argv[1], "r");
    if (!f) return 3;
    char kind;
    int font;
    while (fscanf(f, " %c %d", &kind, &font) == 2) {
        if (kind == 'H') {
            float h, b;
            if (fscanf(f, "%f %f", &h, &b) != 2) return 4;
            metrics[font].height = h, metrics[font].baseline = b;
        } else {
            int code;
            float a;
            if (fscanf(f, "%d %f", &code, &a) != 2) return 4;
            gFonts[font].advance[code] = a;
        }
    }
    fclose(f);
    for (int i = 0; i < 2; i++) metrics[i].context = &gFonts[i], metrics[i].advance = Advance, metrics[i].has = Has;
    MpMsgText::setLanguage(MpMsgText::languageIndex(argv[3]));

    // dialogs: "dialog:<id>" prints the window text of an id (and its Yes / No)
    if (strncmp(argv[2], "dialog:", 7) == 0) {
        for (const char* suffix : {"", "_Yes", "_No"}) {
            std::string id = std::string(argv[2] + 7) + suffix;
            const char16_t* t = MpMsgText::findDialog(id.c_str());
            printf("D%s", suffix);
            for (const char16_t* p = t; p && *p; ++p) printf(" %d", *p);
            printf("\n");
        }
        return 0;
    }

    seq().init(&gIo);
    MpOverlay::reset();
    if (strcmp(argv[2], "entry-mock") == 0) {
        // uinative: the entry screen's style (the entry agent's screen after the title): two big buttons, no title band
        MpPanel::Screen screen;
        if (const char* a = getenv("PREVIEW_ASPECT"); a && strcmp(a, "43") == 0) screen.width = 608.0f;
        static MpPanel::Model model;
        static MpPanel::Plan plan;
        static MpPanel::Scene scene;
        MpPanel::clear(&model);
        model.kind = MpPanel::KIND_BUTTONS;
        model.rowCount = 2;
        MpPanel::copy(model.rows[0].label, MpPanel::kLabel, u"Single Player");
        MpPanel::copy(model.rows[1].label, MpPanel::kLabel, u"Multiplayer");
        model.cursor = 1;
        model.buttonCount = 1;
        model.buttons[0].pic = MpMsgText::PIC_CROSS;
        MpPanel::copy(model.buttons[0].label, 20, u"Select");
        printf("S %f %f entry-mock \n", screen.width, screen.height);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, screen, metrics, 0.0f, &scene, &plan);
        printScene("MENU", scene, plan);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        printScene("TOAST", scene, plan);
        printScene("STRIP", scene, plan);
        return 0;
    }
    if (!reach(argv[2])) {
        fprintf(stderr, "unknown screen %s\n", argv[2]);
        return 5;
    }
    MpPanel::Screen screen;
    if (const char* a = getenv("PREVIEW_ASPECT"); a && strcmp(a, "43") == 0) screen.width = 608.0f;   // (uinative: a 4:3 picture)
    static MpPanel::Model model;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    printf("S %f %f %s %s\n", screen.width, screen.height, MpMenu::screenName(seq().screen()), fake::lastDialog.c_str());
    MpPanel::beginScene(&scene);
    MpPanel::begin(&plan);
    if (MpOverlay::buildResults(&model) || MpOverlay::buildStarCard(&model) || MpOverlay::buildWaiting(&model) || MpOverlay::buildHeadStart(&model)) MpPanel::compose(model, screen, metrics, 0.0f, &scene, &plan);
    if (seq().isOpen()) {
        seq().buildModel(&model);
        MpPanel::compose(model, screen, metrics, 0.0f, &scene, &plan);
    }
    printScene("MENU", scene, plan);
    MpPanel::beginScene(&scene);
    MpPanel::begin(&plan);
    const int n = MpOverlay::toastCount();
    if (n > 0) MpPanel::composeToasts(MpOverlay::toasts(), n, screen, metrics, &scene, &plan);
    if (gRingOn) MpPanel::composeRing(gRing, screen, metrics, &plan);   // (chat)
    printScene("TOAST", scene, plan);
    MpPanel::beginScene(&scene);
    MpPanel::begin(&plan);
    if (gStripOn) MpPanel::composeStrip(gStrip, screen, metrics, 0.25f, &scene, &plan);
    printScene("STRIP", scene, plan);
    return 0;
}
