// Galaxy 3.0 multiplayer: the menus' host tests, shared part (flow split of ui_menu_test.cpp, 11 Nov): the fake port's state, the
// test window, and the helpers every family's tests use (all inline). The fake port itself is fake.cpp; the runner main.cpp.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "Game/MP/MpChat.hpp"
#include "Game/MP/MpEntry.hpp"
#include "Game/MP/MpChatLogic.hpp"
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpPartyLogic.hpp"
#include "Game/MP/MpControls.hpp"
#include "Game/MP/MpUi.hpp"
#include "port/mp_ui_flow.h"
#include "mp_gather_fake.h"   // core2's fake of port/mp_gather.h
#include "port/mp_direct_code.h"
#include "include/vita_words.h"   // vitabtn: no Wii control on a Vita screen
#include "port/mp_conntest.h"
#include "port/settings_native.h"   // settingsnative: the native Settings' host (a fake) and model
#include "player_settings_files.h"   // (tests/: the memory files)

// m-files: the keep-safe and slot-state calls the file family makes (the same declarations as MpMenuFiles.cpp; both go when the real headers are merged)
#ifndef MPSAVE_HAS_SLOTSTATE
namespace MpSaveGame {
enum SlotState { SLOT_UNREADABLE = -1, SLOT_NEW = 0, SLOT_USED = 1 };
int slotState(bool coop, int slot, int* stars);
}
#endif
#include "port/mp_keepsafe.h"


inline int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ------------------------------------------------------------------ fake port
inline bool Logged(const char* line);   // (defined with the helpers below)
inline bool LoggedPart(const char* part);

namespace fake {
// conntest: the Connection Test as the menus see it (the tests fill the view in by hand)
struct Ct { MpCtView view; int starts, pumps, cancels, reports; bool startOk, lastInRoom, reportOk, relay, updatePending, clockWrong; };   // (m-conntest: clockWrong appended)
inline Ct ct;
inline MpUiStatus status;
inline bool active, host;
inline int local, hostSlot;
inline uint16_t epoch;
inline MpMode mode;
struct P { bool present; char name[17]; MpLook look; int ping; };
inline P players[4];
inline MpProfile profile;
inline char savedName[17] = "Player";   // wire: the name in mp.cfg (mp_core_saved_name): never empty on a real console
inline int friendsState;
inline MpFriend friends[3] = {{"Luigi64", "1234-5678", true, true, 1}, {"Star", "2222-3333", true, false, 0}, {"Ros", "9876-5432", false, false, 0}};
// friendsui: incoming friend requests, the service's answers to Add / Not Now / Block / Unblock
inline MpFriend incoming[4] = {{"Daisy", "5555-6666", false, false, 0}, {"Wario", "7777-8888", false, false, 0}, {"Waluigi", "1111-2222", false, false, 0}, {"Peach", "3333-4444", false, false, 0}};
inline int incomingCount, incomingState;   // incomingState: >= 0 follows incomingCount; -2 error
inline int friendRequests;                  // uxwait: mp_ui_friends_request calls
inline int reloading;                      // get calls that still answer "loading" after a request
inline bool opFails;
inline std::vector<std::string> ops;       // "add:5555-6666", "dismiss:...", "block:...", "unblock:..."
inline int boardState;
inline int roomCount;
inline MpLocalRoom rooms[3] = {{"Luigi64", 1, 2, 4, true, 0x1111}, {"Old", 1, 1, 4, false, 0x2222}, {"Ros", 2, 1, 4, true, 0x3333}};
inline uint32_t joinedRoom;
inline MpUiError refuseJoin;
inline int kbResult;
inline bool codeAvailable;
inline bool mismatchLeft;    // uxfix #2: MpSaveGame::takeLeftForMismatch
inline bool settingsRO;      // flow: MpSaveGame::settingsReadOnly   // flow: mp_gather_code_available
inline char kbText[64];
inline int leaves, kicks, starts, keptSlot, writeBacks;
inline bool recoverConfirmed;   // savebug: the third argument of recoveryKeep
inline int leaveChoice, leaveSlot, recoverChoice, recoverSlot, recoveryDiscards, backups, spSession, wishes;
inline bool recovery, grant, noCoop, onMpRoot;
inline unsigned spMissing;   // entry: single-player files that do not exist (bit f-1)
inline int leaveFails, rootLeaves, abandons;
inline bool startFails, roomPublic;
inline bool recSingle, recSpNewer;
inline int recSpStars;
inline MpLook wish;
inline MpSettings settings;     // bubble: the session's (mp_settings) and what mp_core_update_settings sent
inline int settingsSent;
inline uint8_t savedDowned;     // mp.cfg "downed"
inline bool rulesWanted, rulesHidden, rulesDone, rulesNever;   // touches: mp.cfg's host-rules card
inline uint32_t rulesKey;
inline MpHostParams hosted;     // the last mp_ui_host call
inline const char* svcCode = "";   // fix3: mp_service_last_code
inline int registerState, friendOpState;
inline int acctDeleteState = 1, acctDeletes;   // delete my online account: the service's answer (1 done, -1 running, -2 failed), the calls
inline bool acctDeleteRefused;                   // (the start itself is refused)
inline std::vector<std::string> log;
inline MpHostParams lastHost;
// MP3 chat: the chat module as the menus see it
inline bool chatTyped, chatRefuse, chatMuted[4];
inline std::vector<int> chatPhrases;
inline std::vector<std::string> chatTexts;
// netx: by address
inline MpConn hostConn;
inline bool hostByAddr;
inline MpAddressInfo addrInfo;
inline std::string joinedAddr;
inline int registers;
// flow: what the menus remember (flow.cfg / coop.cfg), the network as the menus see it, rejoin, friend codes
inline MpFlowStore flow;
inline MpCoopMeta coop[6];
inline bool wifi, service, rejoinOk, coopFull, coopZero, enterFails;
inline int rejoins, flowSaves, slotReads;
inline const char* codes[4];
inline bool announcedOnly = false;   // wire: the codes are those a console ANNOUNCED in a Same Wi-Fi / Nearby / spoken-code room (not the service's verified ones)
// m-files: files that cannot be read (bit slot-1: co-op / single-player), whether a leave decision is pending, and keepsafe as the menus see it
inline unsigned coopBad, spBad;
inline bool leavePending;
struct Ks {
    std::vector<MpKsCopy> copies;   // newest first, as keepsafe lists them
    int makeRes, inspectRes, slotRes, accountRes, settingsRes;
    MpKsCopyInfo info;              // what inspect says about any copy
    std::vector<std::string> calls;
    int64_t now;
    int notice;                     // what mp_keepsafe_notice_take answers once
    bool noMigrate;                 // mp_keepsafe_may_migrate() is false
    std::string lastName;
    int lastSlot, lastConfirmed, lastFlags, lastWhich;
    int madeCount;
};
inline Ks ks;
inline void resetMine() {
    coopBad = spBad = 0;
    leavePending = false;
    ks = Ks();
    ks.now = 1794400000;   // 2026-11-11
    memset(&ks.info, 0, sizeof ks.info);
    for (int i = 0; i < 6; i++) ks.info.copyStars[i] = -1, ks.info.liveStars[i] = -1;
}
inline MpKsCopy ksCopy(int kind, const char* name, int64_t when) {
    MpKsCopy c;
    memset(&c, 0, sizeof c);
    c.kind = kind;
    snprintf(c.name, sizeof c.name, "%s", name);
    c.created = when;
    snprintf(c.version, sizeof c.version, "3.0.0");
    c.teamFiles = 1;
    return c;
}
// settingsnative: the native Settings' host as the menus see it: memory files, no env.txt overrides, a relaunch that does not restart (it
// counts and answers relaunchOk), the disc's languages, the Config this run started with (boot). reset() installs it for every test.
// The live table the host gives the model: a value index per row (-1: no reader), what was set / committed / bound.
struct Live {
    int val[port::settings::kNativeRows];
    unsigned sets = 0, commits = 0, binds = 0;
    bool bindsOk = true;
    port::settings::NativeLiveOps ops;
};
struct Cfg {
    Live live;
    bool idle = true;               // the card is quiet (relaunchReady)
    std::vector<std::string> env;   // the env.txt keys that were set at start
    UiFiles files;
    port::settings::FileOps ops;
    std::vector<port::settings::Override> overrides;
    port::settings::Config boot;
    port::settings::NativeHost host;
    std::vector<std::string> languages;
    int relaunches;
    bool relaunchOk;
    unsigned saveProgress = 0, saveFailures = 0;   // setfix31b: the save layer's counters (NativeHost::saveProgress / saveFailures)
    bool relaunchSafe = true;                       // Safe Saves on
};
inline Cfg cfg;
inline bool CfgRelaunch() { ++cfg.relaunches; return cfg.relaunchOk; }
inline const char* kCfgPath = "mem/player-settings.cfg";
inline void cfgLiveReset() {
    using port::settings::NativeRow;
    Live& l = cfg.live;
    for (auto& v : l.val) v = 0;
    l.val[(int)NativeRow::TiltStrength] = 4, l.val[(int)NativeRow::CameraSpeed] = 4, l.val[(int)NativeRow::CameraZoom] = 5, l.val[(int)NativeRow::TapToJump] = 1;
    l.sets = l.commits = l.binds = 0, l.bindsOk = true;
    l.ops = {&l,
        [](void* p, NativeRow r) -> int { return static_cast<Live*>(p)->val[(int)r]; },
        [](void* p, NativeRow r, int i) -> bool { auto& f = *static_cast<Live*>(p); ++f.sets; f.val[(int)r] = i; return true; },
        [](void* p) { ++static_cast<Live*>(p)->commits; },
        [](void* p, const port::settings::Config&, port::controls::ProfileId) -> bool { auto& f = *static_cast<Live*>(p); ++f.binds; return f.bindsOk; },
        nullptr};
}
inline void cfgOverride(const char* key) { cfg.env.push_back(key); }
inline void cfgReset(bool fixed30 = true, bool motion = true, bool available = false) {
    cfgLiveReset();
    cfg.idle = true;
    cfg.env.clear();
    cfg.files = UiFiles();
    cfg.ops = cfg.files.ops();
    cfg.overrides.clear();
    cfg.boot = port::settings::DefaultConfig(port::controls::GetControlApi());
    cfg.languages = {"English", "French", "Spanish"};
    cfg.relaunches = 0;
    cfg.relaunchOk = false;
    cfg.host = {kCfgPath, &cfg.ops, &cfg.boot, &cfg.overrides, CfgRelaunch, available, fixed30, motion, &cfg.live.ops,
                []() { return cfg.idle; }, []() -> const char* { return "english"; },
                [](const char* k) { for (auto& e : cfg.env) if (e == k) return true; return false; }, false,
                []() { return cfg.saveProgress; }, []() { return cfg.saveFailures; }, []() { return cfg.relaunchSafe; }};
    cfg.saveProgress = cfg.saveFailures = 0;
    cfg.relaunchSafe = true;
    port::settings::NativeHostSetForTest(&cfg.host);
}
inline void reset() {
    cfgReset();
    memset(&flow, 0, sizeof flow);
    flow.charPicked = 1;   // (most tests: the character was picked before; the first-time tests clear it)
    flow.fallsAsked = 1;   // (most tests: the falls question was answered before; the one-door tests clear it)
    memset(coop, 0, sizeof coop);
    for (MpCoopMeta& c : coop) c.galaxy = -1;
    wifi = service = true, rejoinOk = true, coopFull = false, coopZero = false, enterFails = false;
    rejoins = flowSaves = 0;
    for (const char*& c : codes) c = nullptr;
    svcCode = "";
    registerState = 0, friendOpState = 1;
    acctDeleteState = 1, acctDeletes = 0, acctDeleteRefused = false;
    status = {MP_UI_IDLE, MP_ERR_NONE, 1, "", 0};
    active = false, host = true, local = 0, hostSlot = 0, epoch = 0, mode = MP_MODE_TOGETHER;
    const char* names[4] = {"Player", "Luigi64", "Ros", "Toad"};
    for (int i = 0; i < 4; i++) {
        players[i].present = i < 3;
        snprintf(players[i].name, sizeof players[i].name, "%s", names[i]);
        players[i].look = static_cast<MpLook>(i);   // (unique: Mario, Luigi, Toad, Cosmic Mario)
        players[i].ping = 40 * i;
    }
    profile = {"Player", "4827-1193", true};
    snprintf(savedName, sizeof savedName, "Player");
    friendsState = 3, boardState = 2, roomCount = 2;
    incoming[0] = {"Daisy", "5555-6666", false, false, 0}, incoming[1] = {"Wario", "7777-8888", false, false, 0};
    incoming[2] = {"Waluigi", "1111-2222", false, false, 0}, incoming[3] = {"Peach", "3333-4444", false, false, 0};
    incomingCount = 2, incomingState = 0, reloading = 0, opFails = false;
    ops.clear();
    joinedRoom = 0;
    refuseJoin = MP_ERR_NONE;
    rooms[0] = {"Luigi64", 1, 2, 4, true, 0x1111};
    rooms[1] = {"Old", 1, 1, 4, false, 0x2222};
    rooms[2] = {"Ros", 2, 1, 4, true, 0x3333};
    kbResult = 0;
    codeAvailable = false;
    settingsRO = false;
    mismatchLeft = false;
    snprintf(kbText, sizeof kbText, "K7QM4D");
    leaves = kicks = starts = writeBacks = 0;
    keptSlot = 0;
    recoverConfirmed = false;
    leaveChoice = leaveSlot = recoverChoice = recoverSlot = recoveryDiscards = backups = spSession = wishes = 0;
    recovery = false;
    noCoop = false;
    spMissing = 0;
    onMpRoot = false;
    leaveFails = rootLeaves = abandons = 0;
    startFails = false;
    recSingle = recSpNewer = false;
    recSpStars = -1;
    grant = true;
    wish = MP_LOOK_MARIO;
    memset(&settings, 0, sizeof settings);
    settings.mode = MP_MODE_TOGETHER;
    settings.maxPlayers = 4;
    settingsSent = 0;
    savedDowned = 0;
    rulesWanted = rulesHidden = rulesDone = rulesNever = false;
    rulesKey = 0;
    memset(&hosted, 0, sizeof hosted);
    log.clear();
    memset(&lastHost, 0, sizeof lastHost);
    chatTyped = true, chatRefuse = false;
    for (bool& m : chatMuted) m = false;
    chatPhrases.clear();
    chatTexts.clear();
    hostConn = MP_CONN_NONE;
    hostByAddr = false;
    addrInfo = MpAddressInfo();
    joinedAddr.clear();
    registers = 0;
    memset(&ct, 0, sizeof ct);
    ct.startOk = true;
    ct.reportOk = true;
    resetMine();
}
}  // namespace fake

struct TestIo : MpMenu::Io {
    MpMenu::DlgState answer = MpMenu::DlgState::Yes;
    bool hold = false;   // menus4: a window stays open until the test answers it (the breadth-first reach tests count a window's answer as a press)
    bool open = false;
    uint64_t held = ~0ull;   // settingsnative: the raw pad (~0: none, the button list is used); a test sets the buttons held now
    bool finger = false;
    uint64_t rawButtons() override { return held; }
    bool touched() override { return finger; }
    int opened = 0;
    std::string last;
    std::string lastText;   // m-files: the window's words (UTF-8, pictures as '?')
    std::vector<std::string> texts, ids;   // m-files: every window, in order
    std::vector<MpMenu::Se> sounds;
    void sound(MpMenu::Se se) override { sounds.push_back(se); }
    int count(MpMenu::Se se) const { int n = 0; for (auto x : sounds) n += x == se; return n; }
    void dialogOpen(MpMenu::Dlg, const char* id) override {
        open = true;
        ++opened;
        last = id;
        CHECK(MpMsgText::findDialog(id) != nullptr);   // (every window id resolves through the game's fallback)
        lastText.clear();
        if (const char16_t* t = MpMsgText::findDialog(id))
            for (t += 3; *t; ++t) lastText += *t == u'\n' ? '\n' : (*t < 128 ? static_cast<char>(*t) : '?');
        texts.push_back(lastText);
        ids.push_back(id);
        CHECK(MpMsgText::findDialog((std::string(id) + "_Yes").c_str()) != nullptr);
    }
    MpMenu::DlgState dialogState() override {
        if (!open) return MpMenu::DlgState::None;
        if (hold) return MpMenu::DlgState::Open;
        open = false;
        return answer;
    }
    int galaxyCount() override { return MpMsgText::galaxyCount(); }
    int galaxyName(int, char16_t* out, int cap) override { MpPanel::copy(out, cap, u"Galaxy"); return 6; }
    int scenarioCount(int) override { return 3; }
    int scenarioName(int, int, char16_t* out, int cap) override { MpPanel::copy(out, cap, u"Star"); return 4; }
};
inline TestIo io;

using MpMenu::Scr;
inline MpMenu::Sequence& S() { return MpMenu::sequence(); }
inline void step(MpMenu::Input in = MpMenu::Input()) { S().update(in); MpOverlay::update(in.ok); }
inline void frames(int n) { for (int i = 0; i < n; i++) step(); }
inline void press(int what) {
    MpMenu::Input in;
    switch (what) {
    case 0: in.ok = true; break;
    case 1: in.back = true; break;
    case 2: in.down = true; break;
    case 3: in.tri = true; break;
    case 4: in.sq = true; break;
    case 5: in.left = true; break;
    case 6: in.right = true; break;
    case 7: in.start = true; break;
    case 8: in.up = true; break;
    }
    step(in);
    frames(2);
}
inline void ok() { press(0); }
inline void back() { press(1); }
inline void down(int n = 1) { for (int i = 0; i < n; i++) press(2); }
inline void right(int n = 1) { for (int i = 0; i < n; i++) press(6); }

// beta diagnostics: the port's report / check functions (fake.cpp), scripted by the test
namespace fake {
inline bool dgAccount = true, dgBeginOk = true, dgErrPending = false, dgCheckBeginOk = true;
inline int dgReportResult = 0 /* MP_DIAG_PENDING */, dgCheckResult = 0, dgBegins, dgCancels, dgCheckBegins, dgCheckCancels, dgErrClears, dgLastKind = -1, dgLastWhere = -1;
inline char dgId[16] = "K7QM2XPA", dgNote[1024];
inline MpDiagCheck dgCheck;
inline void dgReset() {
    dgAccount = dgBeginOk = dgCheckBeginOk = true, dgErrPending = false;
    dgReportResult = dgCheckResult = 0, dgBegins = dgCancels = dgCheckBegins = dgCheckCancels = dgErrClears = 0, dgLastKind = dgLastWhere = -1;
    dgNote[0] = 0;
    memset(&dgCheck, 0, sizeof dgCheck);
    dgCheck.valid = true;
    snprintf(dgCheck.nat, sizeof dgCheck.nat, "cone"), dgCheck.stunMs = 40, snprintf(dgCheck.relay, sizeof dgCheck.relay, "ok"), dgCheck.relayRttMs = 85;
}
}  // namespace fake
inline void fresh() {
    fake::reset();
    mp_gather_fake_reset();
    MpOverlay::reset();
    S().close();
    S().init(&io);
    io.answer = MpMenu::DlgState::Yes;
    io.open = false;
    io.held = ~0ull, io.finger = false;   // settingsnative: no raw pad
    io.texts.clear(), io.ids.clear(), io.lastText.clear();
    fake::dgReset();
}
inline void openMain() { S().openMain(1); frames(3); }
inline void session(bool host, int local, MpUiPhase phase) {
    fake::active = true;
    fake::host = host;
    fake::local = local;
    fake::status.phase = phase;
    snprintf(fake::status.roomCode, sizeof fake::status.roomCode, "ABCDEF");
}

// flow (one door): the steps of the screen graph
// Multiplayer: [Rejoin], Play Together, Star Race, Join with a Code, Safe Copies. Play Together / Star Race: the gathering screen at once.
// The gathering is core2's fake (tests/mp/mp_gather_fake.h): heads by id (1 = this console "Player", 2 = Luigi64, 3 = Ros), the leader.
inline void toGather(bool race = false) { openMain(); if (race) down(); ok(); frames(2); }
inline void gatherHeads(int n, uint32_t leader) {   // n heads (this console first), the leader, the room settled
    const char* names[3] = {"Player", "Luigi64", "Ros"};
    for (int i = 0; i < n && i < 3; i++) mp_gather_fake_add_head(static_cast<uint32_t>(i + 1), names[i], static_cast<uint8_t>(i));
    mp_gather_fake_set_leader(leader);
    mp_gather_fake_open_room();
    frames(2);
}
inline int startsAll() { return fake::starts + mp_gather_fake_begins(); }   // (the lobby's Start or the gathering's)
// (the old names: a gathering this console leads with Luigi64 in it / one Luigi64 leads; the core's room as the fake session says)
inline void hostLobby(bool race = false) { toGather(race); gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(3); }
inline void guestLobby() { toGather(); gatherHeads(2, 2); session(false, 1, MP_UI_LOBBY); frames(3); }
inline void toSave() { hostLobby(); fake::players[1].present = true; frames(2); ok(); }  // (Start with co-op file 2 in use: Which file?)
// Join list rows (wifi, service, account; Luigi64 is a friend in a room; two rooms on the Wi-Fi of which one of another version):
// Luigi64's room (online), Luigi64's room (Wi-Fi), Old's room, Room Code, Play with a Console Next to You, Type an Address
constexpr int kJoinCode = 3, kJoinSwitch = 4, kJoinAddr = 5;
// a guest's session drops (the core gave up rejoining): "Save your progress?" No; the Rejoin row is offered for 2 minutes
inline void dropOut() {
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    fake::active = false, fake::status.phase = MP_UI_IDLE, fake::status.error = MP_ERR_NO_WIFI;
    io.answer = MpMenu::DlgState::No;
    frames(10);
    io.answer = MpMenu::DlgState::Yes;
    fake::status.error = MP_ERR_NONE;
}
// conntest: a finished run for the paths and tests: good (everything works), bad (no Wi-Fi), slow (weak Wi-Fi, slow delay, hosting maybe)
enum CtKind { CT_GOOD, CT_BAD, CT_SLOW };
inline void ctFinish(CtKind k) {
    MpCtView& v = fake::ct.view;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < MPCT_CHECKS; i++) v.state[i] = MPCT_ST_DONE, v.detail[i] = -1;
    v.done = true;
    MpCtVerdict& d = v.verdict;
    if (k == CT_GOOD) {
        const uint8_t r[MPCT_CHECKS] = {MPCT_R_WIFI_GOOD, MPCT_R_LOCAL_FOUND, MPCT_R_NET_OK, MPCT_R_HOST_OPEN, MPCT_R_DELAY_FAST};
        memcpy(v.result, r, sizeof r);
        v.detail[4] = 38;
        d.headline = MPCT_H_ALL_GOOD, d.lineCount = 1, d.lines[0] = MPCT_L_HOST_AND_JOIN, d.good = true;
    } else if (k == CT_BAD) {
        const uint8_t r[MPCT_CHECKS] = {MPCT_R_WIFI_NONE, MPCT_R_SKIPPED, MPCT_R_SKIPPED, MPCT_R_SKIPPED, MPCT_R_SKIPPED};
        memcpy(v.result, r, sizeof r);
        d.headline = MPCT_H_NO_WIFI, d.lineCount = 2, d.lines[0] = MPCT_L_CONNECT_WIFI, d.lines[1] = MPCT_L_NEARBY_STILL;
    } else {
        const uint8_t r[MPCT_CHECKS] = {MPCT_R_WIFI_WEAK, MPCT_R_LOCAL_OK, MPCT_R_NET_OK, MPCT_R_HOST_MAYBE, MPCT_R_DELAY_SLOW};
        memcpy(v.result, r, sizeof r);
        v.detail[0] = 20, v.detail[4] = 310;
        d.headline = MPCT_H_WIFI_WEAK, d.lineCount = 3, d.lines[0] = MPCT_L_MOVE_CLOSER, d.lines[1] = MPCT_L_HOST_MAYBE, d.lines[2] = MPCT_L_TRY_LOCAL;
        d.weakLink = true;
    }
    frames(6);   // (the verdict's window and the tip are answered)
}

// m-conntest: a finished run with the five results given (Wi-Fi, Same Wi-Fi, Internet, Hosting, Delay); no window follows any more
inline void ctSet(const uint8_t r[MPCT_CHECKS], bool inRoom = false) {
    MpCtView& v = fake::ct.view;
    memset(&v, 0, sizeof v);
    for (int i = 0; i < MPCT_CHECKS; i++) v.state[i] = MPCT_ST_DONE, v.result[i] = r[i], v.detail[i] = -1;
    v.done = true;
    v.inRoom = inRoom;
    v.serial++;
}

inline void toQuick() { toGather(); gatherHeads(1, 1); ok(); }   // (alone: Play with Anyone)
// uxfix #7: the Connection Test is offered after a join that did not work (not after How to Play): a failed Play with Anyone, the offer answered Yes
inline void reachConnTest() {
    toQuick();
    fake::status.phase = MP_UI_FAILED;
    fake::status.error = MP_ERR_NO_WIFI;
    frames(14);
}

// paths from the file select (the state machine's own screens)
struct Path { const char* name; Scr want; void (*go)(); };
inline void validCode() {   // a far-away code that decodes (the fake core takes it): typed in the keyboard
    const uint8_t secret[MP_DIRECT_SECRET_BYTES] = {1, 2, 3, 4, 5};
    mp_direct_code_encode(0xC0A80102u, 0, secret, fake::kbText, sizeof fake::kbText);
    fake::kbResult = 1;
}
inline const Path kPaths[] = {
    {"main", MpMenu::S_MAIN, [] { openMain(); }},
    {"main(rejoin)", MpMenu::S_MAIN, [] { dropOut(); openMain(); }},
    // beta diagnostics: Having Trouble? (Main's Start button), the consent screen (after the two "what is sent" windows), the two waits
    {"help", MpMenu::S_HELP, [] { openMain(); press(7); }},
    {"report", MpMenu::S_REPORT, [] { openMain(); press(7); down(); ok(); frames(4); }},
    {"rpwait(send)", MpMenu::S_RPWAIT, [] { openMain(); press(7); down(); ok(); frames(4); ok(); }},
    {"rpwait(check)", MpMenu::S_RPWAIT, [] { openMain(); press(7); ok(); }},
    {"main(no service)", MpMenu::S_MAIN, [] { fake::service = false; openMain(); }},
    {"chars(first play)", MpMenu::S_CHARS, [] { fake::flow.charPicked = 0; openMain(); ok(); frames(6); }},
    // flow (one door): the gathering screen
    {"gather(listening)", MpMenu::S_GATHER, [] { toGather(); }},
    {"gather(alone)", MpMenu::S_GATHER, [] { toGather(); gatherHeads(1, 1); }},
    {"gather(nobody)", MpMenu::S_GATHER, [] { toGather(); gatherHeads(1, 1); frames(20 * 60 + 2); }},
    {"gather(playing nearby)", MpMenu::S_GATHER, [] { toGather(); gatherHeads(1, 1); mp_gather_fake_add_playing(77, "Ros", MP_MODE_TOGETHER, 2); frames(2); }},
    {"gather(leader)", MpMenu::S_GATHER, [] { hostLobby(); }},
    {"gather(three)", MpMenu::S_GATHER, [] { toGather(); gatherHeads(3, 1); }},
    {"gather(merging)", MpMenu::S_GATHER, [] { hostLobby(); mp_gather_fake_merge_begin(); frames(2); }},
    {"gather(guest)", MpMenu::S_GATHER, [] { guestLobby(); }},
    {"gather(race)", MpMenu::S_GATHER, [] { toGather(true); gatherHeads(1, 1); }},
    {"gather(code)", MpMenu::S_GATHER, [] { openMain(); down(2); validCode(); ok(); frames(2); }},
    {"falls", MpMenu::S_FALLS, [] { fake::flow.fallsAsked = 0; hostLobby(); press(7); }},
    {"busy(quick)", MpMenu::S_BUSY, [] { toGather(); gatherHeads(1, 1); ok(); }},
    {"gather(room)", MpMenu::S_GATHER, [] { toGather(); gatherHeads(1, 1); ok(); session(true, 0, MP_UI_LOBBY); frames(3); }},   // (Play with Anyone found nobody: this console's room)
    {"busy(rejoin)", MpMenu::S_BUSY, [] { dropOut(); openMain(); ok(); }},
    {"board", MpMenu::S_BOARD, [] { toGather(true); gatherHeads(1, 1); down(); ok(); }},
    {"save", MpMenu::S_SAVE, [] { toSave(); }},
    {"spfile", MpMenu::S_SPFILE, [] { toSave(); down(2); ok(); }},        // (Which file?: co-op 2, New Co-op File, Start from My Own File)
    {"fileuse", MpMenu::S_FILEUSE, [] { toSave(); down(2); ok(); ok(); }},
    {"slot(copy)", MpMenu::S_SLOT, [] { fake::coopFull = true; toSave(); down(6); ok(); ok(); ok(); }},   // (six co-op files: which to replace)
    {"spfile(entry)", MpMenu::S_SPFILE, [] { S().openFromEntry(); frames(3); ok(); frames(2); gatherHeads(2, 1); session(true, 0, MP_UI_LOBBY); frames(3);
                                              ok(); down(2); ok(); }},
    {"friends", MpMenu::S_FRIENDS, [] { openMain(); press(4); }},
    {"played", MpMenu::S_PLAYED, [] { fake::flow.playedCount = 1; snprintf(fake::flow.playedWith[0], 17, "Ros"); openMain(); press(4); ok(); }},
    {"requests", MpMenu::S_REQUESTS, [] { openMain(); press(4); ok(); }},                  // (friendsui: Friends: Requests (N) is the first row)
    {"request", MpMenu::S_REQUEST, [] { openMain(); press(4); ok(); ok(); }},
    {"blocked", MpMenu::S_BLOCKED, [] { openMain(); press(4); down(4); ok(); }},          // (the last row: Requests, three friends, Blocked Players)
    {"chars(gather)", MpMenu::S_CHARS, [] { guestLobby(); press(3); }},
    {"session", MpMenu::S_SESSION, [] { session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); }},
    {"warp", MpMenu::S_WARP, [] { session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); ok(); }},
    {"messages", MpMenu::S_MESSAGES, [] { session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); press(4); }},
    {"messages(no typing)", MpMenu::S_MESSAGES, [] { session(false, 1, MP_UI_PLAYING); fake::chatTyped = false; frames(2); S().openSession(); frames(2); press(4); }},
    {"keep", MpMenu::S_KEEP, [] { toSave(); press(3); }},   // m-files: Which file? > Triangle: Safe Copies (uxfix #5)
    // menus4: the Safe Copies screens WITH copies present: the three kinds of copy a list can hold, and the two screens with a copy that holds everything
    {"keep(copies)", MpMenu::S_KEEP, [] { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); toSave(); press(3); down(2); }},
    {"keeplist(three kinds)", MpMenu::S_KEEPLIST, [] {
        fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_BEFORE, "b.smgmp", 1794390000));
        fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_VERSION, "v.smgmp", 1794380000));
        fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794300000));
        toSave(); press(3); down(2); ok(); down(); }},
    {"save(unreadable, copy)", MpMenu::S_SAVE, [] { fake::coopBad = 1u << 1; fake::flow.lastCoop = 2; fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); toSave(); down(3); }},
    {"spfile(unreadable)", MpMenu::S_SPFILE, [] { fake::spBad = 1u << 0; toSave(); down(2); ok(); }},
    {"slot(unreadable)", MpMenu::S_SLOT, [] { fake::coopFull = true; fake::coopBad = 1u << 0; fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); toSave();
                                              for (int i = 0; i < 9; i++) { static MpPanel::Model m; S().buildModel(&m); if (m.cursor >= 0 && m.cursor < m.rowCount && std::u16string(m.rows[m.cursor].label) == u"Start from My File") break; down(); }
                                              ok(); ok(); ok(); }},
    {"keeplist", MpMenu::S_KEEPLIST, [] { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); toSave(); press(3); down(2); ok(); }},
    {"keepcopy", MpMenu::S_KEEPCOPY, [] { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); fake::ks.info.copyStars[1] = 12; fake::ks.info.account = 1; toSave(); press(3); down(2); ok(); ok(); }},
    {"keepcopy(everything)", MpMenu::S_KEEPCOPY, [] { fake::ks.copies.push_back(fake::ksCopy(MP_KS_COPY_MADE, "a.smgmp", 1794400000)); for (int i = 0; i < 6; i++) fake::ks.info.copyStars[i] = 120; fake::ks.info.account = 1; fake::ks.info.settings = 7; toSave(); press(3); down(2); ok(); ok(); }},
    {"conntest(running)", MpMenu::S_CONNTEST, [] { reachConnTest(); fake::ct.view.state[0] = MPCT_ST_DONE; fake::ct.view.result[0] = MPCT_R_WIFI_GOOD; fake::ct.view.state[1] = MPCT_ST_RUNNING; frames(2); }},
    {"conntest(good)", MpMenu::S_CONNTEST, [] { reachConnTest(); ctFinish(CT_GOOD); }},
    {"conntest(bad)", MpMenu::S_CONNTEST, [] { reachConnTest(); ctFinish(CT_BAD); }},
    {"players", MpMenu::S_PLAYERS, [] { session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); press(3); }},
    // settingsnative: the Settings screens (Picture, Language, Controls, More; Tilt, Tilt Strength, Pointer, Change Buttons; Layout and the moves)
    {"settings", MpMenu::S_SET_MAIN, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); }},
    {"settings(camera)", MpMenu::S_SET_CAMERA, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(2); ok(); }},   // (setnat31: 60 Assist is row 1 in Smooth Motion)
    {"settings(controls)", MpMenu::S_SET_CONTROLS, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(3); ok(); }},
    {"settings(more)", MpMenu::S_SET_MORE, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(4); ok(); }},
    {"settings(buttons)", MpMenu::S_SET_BUTTONS, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(3); ok(); down(5); ok(); }},
    {"settings(pick)", MpMenu::S_SET_PICK, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(3); ok(); down(5); ok(); ok(); }},
    {"settings(capture)", MpMenu::S_SET_CAPTURE, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); down(3); ok(); down(5); ok(); io.held = 0; ok(); }},
    {"settings(leave)", MpMenu::S_SET_LEAVE, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); ok(); back(); }},
    {"settings(restart)", MpMenu::S_SET_RESTART, [] { fake::cfgReset(true, true, true); S().openSettings(); frames(3); ok(); back(); fake::cfg.idle = false; ok(); frames(3); }},   // setfix31b: a save in flight
    {"player(session)", MpMenu::S_PLAYER, [] { session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); press(3); down(); ok(); }},
};

// friendsui: friend requests (Add / Not Now / Block), Blocked Players (Unblock, By Code)
inline std::string U8(const char16_t* t) {
    std::string o;
    for (; *t; ++t) o += *t < 128 ? static_cast<char>(*t) : '?';
    return o;
}
inline std::string rowLabel(int i) {   // (the screen's choices: heads in front of them, in the look, do not count)
    static MpPanel::Model m;
    S().buildModel(&m);
    i += m.headCount;
    return i < m.rowCount ? U8(m.rows[i].label) : "<none>";
}
inline std::string rowValue(int i) {   // m-files
    static MpPanel::Model m;
    S().buildModel(&m);
    return i < m.rowCount ? U8(m.rows[i].value) : "<none>";
}
inline bool rowDisabled(int i) {
    static MpPanel::Model m;
    S().buildModel(&m);
    return i < m.rowCount && (m.rows[i].marks & MpPanel::MARK_DISABLED) != 0;
}
inline std::string hintLine() {
    static MpPanel::Model m;
    S().buildModel(&m);
    return U8(m.hint);
}
inline std::string subtitleLine() {
    static MpPanel::Model m;
    S().buildModel(&m);
    return U8(m.subtitle);
}
// m-files: press Cross and answer the windows it opens in turn (a window opened is answered with the next answer; Yes after the list)
inline void okDrive(std::vector<MpMenu::DlgState> a) {
    int seen = io.opened;
    size_t k = 0;
    MpMenu::Input in;
    in.ok = true;
    step(in);
    for (int f = 0; f < 40; f++) {
        if (io.opened != seen) {
            seen = io.opened;
            io.answer = k < a.size() ? a[k] : MpMenu::DlgState::Yes;
            ++k;
        }
        step();
    }
    io.answer = MpMenu::DlgState::Yes;
}
inline bool windowFits(const std::string& t) {   // at most 3 lines of at most 44 characters (the game's message window)
    int lines = 1, run = 0;
    for (char c : t) {
        if (c == '\n') { ++lines; run = 0; } else if (++run > 44) return false;
    }
    return lines <= 3;
}
inline bool anyText(const char* part) {
    for (auto& t : io.texts) if (t.find(part) != std::string::npos) return true;
    return false;
}
inline std::string cursorLabel() {   // the selected row's label (the list may be scrolled: the model's rows are the visible window)
    static MpPanel::Model m;
    S().buildModel(&m);
    return m.cursor >= 0 && m.cursor < m.rowCount ? U8(m.rows[m.cursor].label) : "<none>";
}
inline std::string statusLine() {
    static MpPanel::Model m;
    S().buildModel(&m);
    return U8(m.status);
}
inline int logCount(const char* prefix) {
    int n = 0;
    for (auto& l : fake::log) n += l.rfind(prefix, 0) == 0;
    return n;
}
inline bool logHas(const char* line) {
    for (auto& l : fake::log) if (l == line) return true;
    return false;
}
inline void toFriends() { openMain(); press(4); frames(2); }   // (flow, one door: Friends is Square on the Multiplayer menu)
inline void toBoard() { toGather(true); gatherHeads(1, 1); down(); ok(); }   // (Star Race, alone: Play with Anyone, Leaderboards)
   // (alone: Play with Anyone)

// fixed-width font metrics: the composer's checks do not need the game's fonts
inline float Adv(void*, uint16_t) { return 16.0f; }
inline bool HasG(void*, uint16_t) { return true; }

// uinative: the layout rules of the native redesign, for one composed screen.
//  - the button guide sits on the lower band, inside the picture (and its 4:3 middle), in words at least 18 units high;
//  - a list's words (everything but the guide) stay inside its window, the window between the two bands, nothing squeezed past
//    its minimum, and no word smaller than 18 units (rows: 24); a list never shows more than kListRows rows;
//  - the big buttons stay between the bands and clear of each other; the selected item has the Star Pointer's hand on the
//    picture; a list with pages has the star list's page column beside the window, inside the picture.
// (The old WIP check counted the guide's runs as rows and failed for every screen with hints: the guide is runs [0, hintRuns).)
extern int g_zoomCapable;
inline float RunWidth(const MpPanel::Plan& plan, const MpPanel::TextRun& r) {
    float w = 0;
    for (int k = 0; k < r.length; k++) w += Adv(nullptr, plan.text[r.start + k]) * r.scaleX;
    return w;
}
inline void uinativeGeometry(const char* name, const char* lang, const MpPanel::Model& m, const MpPanel::Screen& scr, const MpPanel::Scene& scene,
                      const MpPanel::Plan& plan) {
    using namespace MpPanel;
    const float footTop = layoutToSquareY(scr, kFootTop);
    // (the title band's lower edge: slimmer when the frame has no line)
    bool alone = false, bare = false, tight = false;
    for (int i = 0; i < scene.useCount; i++)
        if (scene.uses[i].kind == USE_FRAME)
            alone = (scene.uses[i].state & kFrameNoLine) != 0, bare = (scene.uses[i].state & kFrameNoTitle) != 0, tight = (scene.uses[i].state & kFrameTight) != 0;
    const bool look = m.look == LOOK_GAME;   // flow look: no bands; the content between the picture's top and the line's window (or the guide)
    const float bandBottom = look ? layoutToSquareY(scr, m.line[0] && ((m.kind == KIND_BUTTONS && m.rowCount <= 2) || (m.kind == KIND_LIST && m.rowCount <= 2)) ? kLookTop : kContentTopBare)
                                  : layoutToSquareY(scr, kBandBottom + (alone ? kBandShrink : 0.0f) + (tight ? kBandTight : 0.0f));
    const float roomBottom = look ? layoutToSquareY(scr, m.line[0] ? kLookBottom : (m.zoomRow ? kZoomRowTop : kContentBottom)) : footTop;
    int bad = 0;
    auto fail = [&](const char* what, float a, float b) {
        if (bad++ < 3) fprintf(stderr, "  [y0 %.0f y1 %.0f band %.0f room %.0f] geometry %s %s (%.0f wide): %s %.1f %.1f [look %d line %d rows %d status %d hint %d tabs %d]\n", plan.y0, plan.y1, bandBottom, roomBottom, name, lang, scr.width, what, a, b, (int)m.look, m.line[0] != 0, m.rowCount, m.status[0] != 0, m.hint[0] != 0, m.tabCount);
        CHECK(false);
    };
    // uinative4: the band's words and the buttons come from one model: the frame's title is this screen's own, and the scene carries
    // the screen's key (the drawer holds the old words while the old buttons leave, MpPanelDraw sHold)
    for (int i = 0; i < scene.useCount; i++)
        if (scene.uses[i].kind == USE_FRAME && scene.uses[i].text >= 0 && m.title[0] && m.look != LOOK_GAME) {   // (the game's look: no title)
            const char16_t* w = scene.texts[scene.uses[i].text];
            for (int k = 0; m.title[k]; k++)
                if (w[k] != m.title[k]) { fail("band title is another screen's", w[k], m.title[k]); break; }
            if (scene.screenKey != m.screenKey) fail("scene key differs from the model's", scene.screenKey, m.screenKey);
        }
    const bool overlay = m.kind == KIND_WAITING || m.kind == KIND_RESULTS;
    // the guide
    CHECK(plan.hintRuns >= 0 && plan.hintRuns <= plan.runCount);
    if (!overlay && m.buttonCount > 0) {
        CHECK(plan.hintRuns > 0);
        if (plan.guideX0 < 0 || plan.guideX1 > scr.width) fail("guide outside the picture", plan.guideX0, plan.guideX1);
        if (scr.width < 700 && (plan.guideX0 < 24 || plan.guideX1 > scr.width - 24)) fail("guide outside 4:3 margins", plan.guideX0, plan.guideX1);
    }
    for (int i = 0; i < plan.hintRuns; i++) {
        const TextRun& r = plan.runs[i];
        const float h = 33.0f * r.scaleY;
        if (r.y < footTop - 4.0f || r.y + h > scr.height + 4.0f) fail("guide off its band", r.y, r.y + h);
        if (r.font == FONT_MESSAGE && h < 18.0f) fail("guide words too small", h, 18);
    }
    // uxfix #8 (touch): a button of the guide is a 64-unit band down to the screen's edge (a thumb hits it without looking)
    for (int i = 0; i < plan.guideHitCount && i < 4; i++) {
        const MpPanel::PlanBase::GuideHit& h = plan.guideHits[i];
        if (h.y1 < scr.height - 0.5f || h.y1 - h.y0 < kGuideBandH - 0.5f) fail("guide touch band too small", h.y1 - h.y0, kGuideBandH);
    }
    // uxfix B2: the game's bottom message (one or two lines) ends above the guide's row: no overlap with its words
    if (look && m.line[0] && plan.hintRuns > 0) {
        int lines = 1;
        for (const char16_t* q = m.line; *q; ++q) lines += *q == u'\n';
        const float bandBottomSq = layoutToSquareY(scr, kLineY - (lines > 1 ? kLineTwoH : kLineOneH));
        float guideTop = 1e9f;
        for (int i = 0; i < plan.hintRuns; i++) guideTop = plan.runs[i].y < guideTop ? plan.runs[i].y : guideTop;
        if (bandBottomSq > guideTop - 2.0f) fail("message band over the guide row", bandBottomSq, guideTop);
    }
    // uxfix #8: the rows of a paged list are 67 units (80 px) high to touch
    if (m.kind == KIND_LIST && m.pages > 0 && m.rowCount > 0 && !m.big[0]) {   // (a screen with a big code on it has no room: slim rows)
        int tall = 0;
        for (int i = 0; i < plan.hitCount; i++) tall += plan.hits[i].y1 - plan.hits[i].y0 >= kTouchMin - 1.0f;
        if (tall < m.rowCount) fail("list rows too short to touch", plan.hitCount ? plan.hits[0].y1 - plan.hits[0].y0 : -1.0f, static_cast<float>(plan.hitCount));
    }
    CHECK(!plan.textClipped);
    // the content
    if (m.kind == KIND_LIST || m.kind == KIND_BUSY) {
        CHECK(m.rowCount <= kListRows || m.kind != KIND_LIST || m.pages == 0);   // (a report with no pages (the Connection Test) may be longer)
        if (plan.y0 < bandBottom - 1.0f) fail("window under the title band", plan.y0, bandBottom);
        if (m.buttonCount > 0 && plan.y1 > roomBottom + 1.0f) fail("window over the guide band", plan.y1, roomBottom);
        // uinative4: the list box stands centred between the bands (equal margins; the window floats 2 units)
        // (menus2: a few rows with a line in the game's look stand on the picture's middle, kLookMid, not between the bands)
        const bool lowLook = look && m.line[0] && m.rowCount <= 2 && !m.big[0] && m.tabCount == 0;
        if (m.kind == KIND_LIST && m.buttonCount > 0 && lowLook) {
            if (fabsf((plan.y0 + plan.y1) * 0.5f - layoutToSquareY(scr, kLookMid)) > 5.0f)
                fail("list box not on the picture's middle", (plan.y0 + plan.y1) * 0.5f, layoutToSquareY(scr, kLookMid));
        } else if (m.kind == KIND_LIST && m.buttonCount > 0 && fabsf((plan.y0 - bandBottom) - (roomBottom - plan.y1)) > 5.0f)
            fail("list box not centred between the bands", plan.y0 - bandBottom, roomBottom - plan.y1);
        if (m.kind == KIND_LIST && m.buttonCount > 0 && (plan.y0 - bandBottom < 10.0f || roomBottom - plan.y1 < 10.0f)) fail("list box margin under 10", plan.y0 - bandBottom, roomBottom - plan.y1);
        if (plan.x0 < 0 || plan.x1 > scr.width) fail("window off the picture", plan.x0, plan.x1);
        for (int i = plan.hintRuns; i < plan.runCount; i++) {
            const TextRun& r = plan.runs[i];
            const float h = 33.0f * r.scaleY;
            if (r.font != FONT_MESSAGE) continue;
            if (r.x < plan.x0 || r.x + RunWidth(plan, r) > plan.x1 + 0.5f) fail("words outside the window (x)", r.x, r.x + RunWidth(plan, r));
            if (r.y < plan.y0 || r.y + h * 0.8f > plan.y1) fail("words outside the window (y)", r.y, r.y + h);
            if (h < (strncmp(name, "settings", 8) == 0 ? 12.0f : 17.0f)) { std::string w; for (int k = 0; k < r.length && k < 40; k++) w += plan.text[r.start + k] < 128 ? static_cast<char>(plan.text[r.start + k]) : '?'; fprintf(stderr, "  small words: \"%s\"\n", w.c_str()); fail("words too small", h, 17); }
        }
    }
    if (m.kind == KIND_LIST && m.rowCount > 0) {
        // the rows' own words (taller than the window's hint) are big
        float tallest = 0;
        for (int i = plan.hintRuns; i < plan.runCount; i++)
            if (plan.runs[i].font == FONT_MESSAGE && 33.0f * plan.runs[i].scaleY > tallest) tallest = 33.0f * plan.runs[i].scaleY;
        bool pictures = m.rowCount > 0;   // (a list of button pictures only: no words to measure)
        for (int i = 0; i < m.rowCount && i < kMaxRows; i++) if (m.rows[i].label[0] < 0xE000 || m.rows[i].label[0] > 0xF8FF) pictures = false;
        if (tallest < 23.0f && !pictures) fail("list rows too small", tallest, 23);
    }
    // the layouts placed
    int pointers = 0, scrolls = 0;
    float lastBottom = 1e9f, lastRestBottom = 1e9f;
    bool firstButton = true;
    for (int i = 0; i < scene.useCount; i++) {
        const LayoutUse& u = scene.uses[i];
        if (u.kind == USE_POINTER) {
            ++pointers;
            const float x = layoutToSquareX(scr, u.x), y = layoutToSquareY(scr, u.y);
            if (x - 50.0f * u.w < 0 || x > scr.width || y < 0 || y > scr.height) fail("pointer off the picture", x, y);
        } else if (u.kind == USE_SCROLL) {
            ++scrolls;
            const float x = layoutToSquareX(scr, u.x);
            if (x - 32 < plan.x1 || x + 32 > scr.width) fail("page column off its place", x, plan.x1);
        } else if (u.kind == USE_BUTTON) {
            // uinative4: the pause menu's rhythm: 70.4 units high at scale 1 (64 x 1.1), the selected one grows 1.15
            const float mid = u.y + 48.0f * u.w, rest = 0.5f * kButtonH * u.w, half = rest * (u.state == BUTTON_SELECTED ? 1.15f : 1.0f);
            const float cTop = (bare ? kContentTopBare : (tight ? kContentTopAlone + kBandTight : (alone ? kContentTopAlone : kContentTop)));
            const float cBottom = m.look == LOOK_GAME && m.line[0] ? kLookBottom : (m.zoomRow ? kZoomRowTop : kContentBottom);   // (flow look: over the line's window)
            if (mid + half > cTop + 2.0f || mid - half < cBottom - 2.0f) fail("button outside the content", mid + half, mid - half);
            const float minW = m.rowCount >= 4 ? 0.84f : 0.9f;   // (four rows over the big buttons: a little tighter)
            if (u.w < minW) fail("big button shrunk (less than 0.9)", u.w, minW);
            if (!firstButton) {
                // a clear gap at rest (>= 20 % of the height), and the grown selected one never touches a neighbour
                const float restGap = lastRestBottom - (mid + rest);
                if (restGap < 0.2f * 2.0f * rest) fail("buttons too close at rest", restGap, 0.2f * 2.0f * rest);
                if (lastBottom - (mid + half) < 6.0f) fail("selected button touches its neighbour", lastBottom, mid + half);
            }
            firstButton = false;
            lastRestBottom = mid - rest;
            lastBottom = mid - half;
        }
    }
    // (flow look: no hand; the selected button's own highlight is the cursor)
    if ((m.kind == KIND_LIST || m.kind == KIND_BUTTONS || m.kind == KIND_CHARS || m.kind == KIND_GRID) && m.cursor >= 0) CHECK(pointers == (m.look == LOOK_GAME || m.dialogUp ? 0 : 1));
    if (m.look == LOOK_GAME) {
        CHECK(pointers == 0);
        int lines = 0;
        for (int i = 0; i < scene.useCount; i++) {
            lines += scene.uses[i].kind == USE_LINE;
            if (scene.uses[i].kind == USE_LINE) {   // menus5: the line is centred in the band (its text's bounding box centre = the band's centre, within 2 units)
                CHECK(scene.uses[i].state & kLineCentred);
                CHECK(std::fabs((scene.uses[i].x + lineTextCentreX()) - (scene.uses[i].x + kLineBandCentreX)) <= 2.0f);
            }
        }
        CHECK(lines == (m.line[0] && !overlay ? 1 : 0));
    }
    CHECK(pointers <= 1);
    CHECK(scrolls == (m.kind == KIND_LIST && m.pages > 1 ? 1 : 0));
}

// vitabtn: every word and picture a screen shows names no Wii control, and every guide button answers a touch
inline void vitaCheckModel(const MpPanel::Model& m, const char* screen, const char* lang) {
    auto text = [&](const char16_t* t, const char* what) {
        if (!t || !t[0]) return;
        const std::u16string s(t);
        const std::string why = vitawords::WhyWii(s), pic = vitawords::WhyPic(s);
        if (!why.empty() || !pic.empty()) fprintf(stderr, "  vita: %s / %s / %s: %s%s [%s]\n", screen, lang, what, why.c_str(), pic.c_str(), vitawords::Ascii(s).c_str());
        CHECK(why.empty() && pic.empty());
    };
    text(m.title, "title"); text(m.subtitle, "subtitle"); text(m.bigLabel, "bigLabel"); text(m.big, "big"); text(m.status, "status"); text(m.hint, "hint");
    for (int i = 0; i < m.tabCount && i < 3; i++) text(m.tabs[i], "tab");
    for (int i = 0; i < m.rowCount && i < MpPanel::kMaxRows; i++) text(m.rows[i].lead, "lead"), text(m.rows[i].label, "label"), text(m.rows[i].value, "value");
    for (int i = 0; i < m.cellCount && i < MpPanel::kMaxCells; i++) text(m.cells[i].label, "cell");
    for (int i = 0; i < m.buttonCount && i < 4; i++) {
        text(m.buttons[i].label, "button");
        CHECK(MpControls::guideInput(m.buttons[i].pic) != MpControls::G_NONE || m.buttons[i].pic == MpMsgText::PIC_HAND);   // (touchable; the hand is the touch itself: the capture screen's "Touch: list")
    }
}

// MP3 celebrate: the team star card (title, rows, no ranking, auto-dismiss, never holds anything) and the party strip composer.
inline std::string Utf8(const char16_t* s) {
    std::string o;
    for (; *s; ++s) o += *s < 0x80 ? (char)*s : '?';
    return o;
}
inline bool Has(const char16_t* s, const char16_t* needle) {
    for (; *s; ++s) {
        const char16_t *a = s, *b = needle;
        while (*a && *b && *a == *b) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}
// fix2: the core's silent rejoin (MP_UI_RECONNECTING) is not the end of the session: a sticky "Reconnecting..." toast, no error,
// no "Save your progress?", no discard, no "left" toasts; it ends only when the core gives up.
inline bool Logged(const char* line) {
    for (auto& l : fake::log) if (l == line) return true;
    return false;
}
inline bool LoggedPart(const char* part) {
    for (auto& l : fake::log) if (l.find(part) != std::string::npos) return true;
    return false;
}
inline bool ReconnectToastUp() {
    char16_t want[96];
    MpMsgText::format(want, 96, MP_T_RECONNECTING, MpMsgText::Args());
    const int n = MpOverlay::toastCount();
    for (int i = 0; i < n; i++) if (std::u16string(MpOverlay::toasts()[i].text) == want) return true;
    return false;
}
// bubble: Play Together "Falls" (MpSettings.downed): one row, two plain choices, Bubbles by default; remembered in mp.cfg;
// the host can change it from the session menu (SETTINGS to everyone); Star Race and guests have no such row.
inline bool rowShows(MpText label, MpText value) {
    static MpPanel::Model model;
    S().buildModel(&model);
    char16_t l[64], v[64];
    MpMsgText::format(l, 64, label, MpMsgText::Args());
    MpMsgText::format(v, 64, value, MpMsgText::Args());
    for (int i = 0; i < model.rowCount; i++) {
        const auto& r = model.rows[i];
        if (std::u16string(r.label) == std::u16string(l) && std::u16string(r.value) == std::u16string(v)) return true;
    }
    return false;
}
inline float Adv(void*, uint16_t);
inline bool HasG(void*, uint16_t);
// netx: Online > Play by Address: host (MP_CONN_DIRECT, the address big in the lobby with the router's verdict), join (the keyboard,
// a bad address, nobody answering), and the online account asked only for the service's rows
inline bool Has16(const char16_t* s, const char* ascii) {
    std::u16string a(s), b;
    for (const char* p = ascii; *p; ++p) b += static_cast<char16_t>(*p);
    return a.find(b) != std::u16string::npos;
}
// MP3 chat: the Messages page (grid, phrases in the grid's order, Write a Message only where typing is allowed), the keyboard, the
// Players screen's Mute (every player, never yourself), the More Options row "Typed Messages" (not for rooms open to anyone).
inline bool Same16(const char16_t* a, const char16_t* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
// ------------------------------------------------------------------ entry: the entry screen (Single Player / Multiplayer)
// The entry's state machine against the real menu sequence; a fake game side plays the buttons' animations (a few frames each).
struct EntryHost : MpEntry::Host {
    int saved = 0, saves = 0, persists = 0, opens = 0;   // persists: saveChoice calls that asked for the file to be written at once
    int settingsOpens = 0;      // settingsnative: Host::openSettings calls
    bool settings = true;       // settingsnative: Host::settingsAvailable (the third pill)
    bool latched = false;       // settingsnative: the old LiveArea tile asked for Settings
    bool settingsLatched() override { const bool l = latched; latched = false; return l; }
    std::vector<int> sounds;
    void sound(MpEntry::Se se) override { sounds.push_back(se); }
    int savedChoice() override { return saved; }
    void saveChoice(int c, bool persistNow) override { saved = c; ++saves; if (persistNow) ++persists; }
    void openMenu() override { ++opens; S().openFromEntry(); }
    bool menuOpen() override { return S().isBusy(); }
    bool takeLaunch(int* f) override { return S().takeLaunch(f); }
    bool takeManual() override { return S().takeManual(); }
    bool singleBlocked() override { return S().singlePlayerGate(); }
    bool settingsAvailable() override { return settings; }
    void openSettings() override { ++settingsOpens; S().openSettings(); }
};
struct EntryRig {
    EntryHost host;
    MpEntry::Logic logic;
    int appearIn = -1, decideIn = -1, hideIn = -1, manualFor = 0;
    bool shown = false, decided = false, hidden = true, manual = false;
    std::vector<MpEntry::Cmd> cmds;
    int launched = 0, pointed = -1;
    void begin(bool after = false) { logic.begin(&host, after); }
    // one frame: the menu first (it reads the buttons only while it is up), then the entry (as the game: system, then scene)
    void frame(MpMenu::Input in = MpMenu::Input()) {
        MpMenu::Input menuIn = S().isBusy() ? in : MpMenu::Input();
        step(menuIn);
        MpEntry::In e;
        e.up = in.up, e.down = in.down, e.ok = in.ok, e.back = in.back, e.pointed = pointed;
        e.shown = shown, e.decided = decided, e.hidden = hidden, e.manualOpen = manual;
        const MpEntry::Out o = logic.update(e);
        if (o.cmd != MpEntry::CMD_NONE) cmds.push_back(o.cmd);
        switch (o.cmd) {
        case MpEntry::CMD_APPEAR: shown = false, hidden = false, decided = false, appearIn = 3; break;
        case MpEntry::CMD_DECIDE: decideIn = 3; break;
        case MpEntry::CMD_DISAPPEAR: shown = false, hideIn = 3; break;
        case MpEntry::CMD_MANUAL: manual = true, manualFor = 5; break;
        case MpEntry::CMD_LAUNCH: launched = o.file; break;
        default: break;
        }
        if (appearIn > 0 && --appearIn == 0) shown = true;
        if (decideIn > 0 && --decideIn == 0) decided = true;
        if (hideIn > 0 && --hideIn == 0) hidden = true, decided = false;
        if (manualFor > 0 && --manualFor == 0) manual = false;
    }
    void frames(int n) { for (int i = 0; i < n; i++) frame(); }
    void press(int what) {
        MpMenu::Input in;
        if (what == 0) in.ok = true;
        if (what == 1) in.back = true;
        if (what == 2) in.down = true;
        if (what == 3) in.tri = true;
        if (what == 4) in.sq = true;
        if (what == 7) in.start = true;
        if (what == 8) in.up = true;
        frame(in);
        frames(2);
    }
    void ok() { press(0); }
    void back() { press(1); }
    void down(int n = 1) { for (int i = 0; i < n; i++) press(2); }
    bool has(MpEntry::Cmd c) const { for (auto x : cmds) if (x == c) return true; return false; }
    void settle() { frames(12); }   // (the buttons' animations)
};


// session family: a row's / a guide button's words by id (so the tests do not hold English), the session menu opened in play
inline std::u16string Fmt16(MpText id) {
    char16_t t[96];
    MpMsgText::format(t, 96, id, MpMsgText::Args());
    return t;
}
inline bool rowIs(int i, MpText id) {   // (rows are the screen's choices: the heads over them, in the look, do not count)
    static MpPanel::Model m;
    S().buildModel(&m);
    i += m.headCount;
    return i < m.rowCount && std::u16string(m.rows[i].label) == Fmt16(id);
}
inline bool guideHas(MpText id) {
    static MpPanel::Model m;
    S().buildModel(&m);
    for (int i = 0; i < m.buttonCount && i < 4; i++)
        if (std::u16string(m.buttons[i].label) == Fmt16(id)) return true;
    return false;
}
inline bool rowsAre(std::initializer_list<MpText> ids) {   // exactly these choices, in this order
    static MpPanel::Model m;
    S().buildModel(&m);
    if (m.rowCount - m.headCount != static_cast<int>(ids.size())) return false;
    int i = m.headCount;
    for (MpText id : ids) if (std::u16string(m.rows[i++].label) != Fmt16(id)) return false;
    return true;
}
// (all through the real screens: who opened the game is known only that way)
inline void hostPlaying(bool race = false, const char* codeOfSlot1 = nullptr) {   // this console opened the game; Start, the file, then playing with Luigi64 and Ros; the menu is up
    fresh();
    fake::codes[1] = codeOfSlot1;   // (what the other console announced)
    hostLobby(race);
    fake::mode = race ? MP_MODE_RACE : MP_MODE_TOGETHER;
    fake::players[1].present = true;
    frames(2);
    ok(); ok(); frames(2);
    fake::status.phase = MP_UI_PLAYING;
    frames(3);
    int file = 0;
    S().takeLaunch(&file);
    frames(30);   // (the reopen guard after the menu closed for the launch)
    S().openSession();
    frames(2);
}
inline void guestPlaying(bool race = false) {   // joined a game (this console is slot 1: Player and Ros are the others); the menu is up
    fresh();
    guestLobby();
    fake::mode = race ? MP_MODE_RACE : MP_MODE_TOGETHER;
    fake::status.phase = MP_UI_PLAYING;
    frames(3);
    int file = 0;
    S().takeLaunch(&file);
    frames(30);   // (the reopen guard after the menu closed for the launch)
    S().openSession();
    frames(2);
}
// m-friends: the friends family's fakes (reset by friendsFresh(); the defaults leave every other family's tests as they were)
namespace fake {
inline bool useMany;                              // the friends / incoming lists come from the vectors below (a list of 50, not 3)
inline std::vector<MpFriend> manyFriends, manyIncoming;
inline int ensureWork, ensureCalls;               // mp_service_ensure_account: answers 0 this many calls, then ensureResult
inline int ensureResult = 1;                      // 1 ready, or -(MpUiError): a failure (the reason is svcCode)
inline int kbMax;                                 // the last keyboard's character limit
inline char kbInitial[64];                        // uxfix: the text the last keyboard opened with
inline bool kbOnce;                               // uxfix: the keyboard answers once, then stays open (a reopened keyboard waits for the player)
inline int kbOpens;                               // uxfix: keyboards opened
inline void friendsReset() {
    useMany = false, manyFriends.clear(), manyIncoming.clear();
    ensureWork = ensureCalls = 0, ensureResult = 1, kbMax = 0, kbOpens = 0, kbInitial[0] = 0, kbOnce = false;
    friends[0] = {"Luigi64", "1234-5678", true, true, 1}, friends[1] = {"Star", "2222-3333", true, false, 0}, friends[2] = {"Ros", "9876-5432", false, false, 0};
}
}  // namespace fake
inline void friendsFresh() { fresh(); fake::friendsReset(); }
