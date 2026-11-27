// The menus' host tests: the conntest family (flow split of ui_menu_test.cpp; m-conntest rewrite). Shared helpers: common.hpp.
//
// Part 1 (testConnTest): the Connection Test screen: the three questions, every verdict combination, the clock line, the absent third
//   question, Back at every moment, the waiting line, the watchdog, the queries for the gathering screen, the offer after a failed join.
// Part 2 (testConnTestWords): no network or role word in any visible string, in all five languages. Its own family (the screen's words)
//   must be clean; every other family's hits are counted against a ratchet (see kKnownOtherHits) so a NEW one fails the test.
#include "common.hpp"

#include <algorithm>
#include <set>

#include "../../../decomp/src/Game/MP/MpMenuInternal.hpp"

using MpMenu::CtAnswer;
using MpMenu::CtAnswers;
using MpMenu::CtVerdict;
using MpMenu::CTV_CHOPPY;
using MpMenu::CTV_GOOD;
using MpMenu::CTV_NOT_NOW;
using MpMenu::CTV_UNKNOWN;

namespace {

constexpr int kLangCount = 5;
const char* const kLangNames[kLangCount] = {"UsEnglish", "UsFrench", "EuSpanish", "EuGerman", "EuItalian"};
void useLang(int l) { MpMsgText::setLanguage(l); }

struct RowInfo { std::string label, value; int ping; bool isStatic; };
std::vector<RowInfo> rowsNow() {
    static MpPanel::Model m;
    S().buildModel(&m);
    std::vector<RowInfo> v;
    for (int i = 0; i < m.rowCount; i++) v.push_back({U8(m.rows[i].label), U8(m.rows[i].value), m.rows[i].ping, (m.rows[i].marks & MpPanel::MARK_STATIC) != 0});
    return v;
}
std::string subtitleNow() {
    static MpPanel::Model m;
    S().buildModel(&m);
    return U8(m.subtitle);
}
std::vector<std::string> buttonsNow() {
    static MpPanel::Model m;
    S().buildModel(&m);
    std::vector<std::string> v;
    for (int i = 0; i < m.buttonCount; i++) v.push_back(U8(m.buttons[i].label));
    return v;
}
int wordIndex(const char* english) {   // the word table's index of an English text (-1 none)
    for (int i = 0; i < MpMenu::ConnTestWordCount(); i++)
        if (U8(MpMenu::ConnTestWord(i, 0)) == english) return i;
    return -1;
}
std::string wordEn(int i) { return i >= 0 ? U8(MpMenu::ConnTestWord(i, 0)) : ""; }

void openTest() { fresh(); reachConnTest(); }   // the screen, running
using R = uint8_t;
constexpr R W_GOOD = MPCT_R_WIFI_GOOD, W_WEAK = MPCT_R_WIFI_WEAK, W_NONE = MPCT_R_WIFI_NONE, W_UNK = MPCT_R_WIFI_UNKNOWN, SKIP = MPCT_R_SKIPPED;
constexpr R L_FOUND = MPCT_R_LOCAL_FOUND, L_OK = MPCT_R_LOCAL_OK, L_BLOCKED = MPCT_R_LOCAL_BLOCKED;
constexpr R N_OK = MPCT_R_NET_OK, N_DOWN = MPCT_R_NET_DOWN, N_NOSVC = MPCT_R_NET_NOT_SET_UP, N_UPD = MPCT_R_NET_UPDATE, N_OFF = MPCT_R_NET_OFF;
constexpr R H_OPEN = MPCT_R_HOST_OPEN, H_MAYBE = MPCT_R_HOST_MAYBE, H_CGNAT = MPCT_R_HOST_CGNAT, H_UNK = MPCT_R_HOST_UNKNOWN;
constexpr R D_FAST = MPCT_R_DELAY_FAST, D_OK = MPCT_R_DELAY_OK, D_SLOW = MPCT_R_DELAY_SLOW;

// ---------------------------------------------------------------- the verdict table: results -> the three answers
struct Case {
    const char* name;
    R r[MPCT_CHECKS];             // Wi-Fi, Same Wi-Fi, Internet, Hosting, Delay
    bool service, clock, relay;   // the build has the online service; the console's date and time are wrong; the privacy setting
    CtVerdict room, far, online;  // expected answers (online CTV_UNKNOWN + onlineRow false: the question is absent)
    const char *roomLine, *farLine, *onlineLine;   // the English line under each ("" none)
    bool farRow, onlineRow;       // the question is on the screen
};
const Case kCases[] = {
    {"everything good", {W_GOOD, L_FOUND, N_OK, H_OPEN, D_FAST}, true, false, false, CTV_GOOD, CTV_GOOD, CTV_GOOD, "", "", "", true, true},
    {"nobody in the room yet", {W_GOOD, L_OK, N_OK, H_OPEN, D_OK}, true, false, false, CTV_GOOD, CTV_GOOD, CTV_GOOD, "", "", "", true, true},
    {"wifi level unknown (desktop)", {W_UNK, L_OK, N_OK, H_OPEN, D_OK}, true, false, false, CTV_GOOD, CTV_GOOD, CTV_GOOD, "", "", "", true, true},
    {"weak wifi", {W_WEAK, L_OK, N_OK, H_OPEN, D_OK}, true, false, false, CTV_CHOPPY, CTV_CHOPPY, CTV_CHOPPY, "Move closer to your Wi-Fi.",
     "Move closer to your Wi-Fi.", "Move closer to your Wi-Fi.", true, true},
    {"no wifi: nearby still works, the rest cannot", {W_NONE, SKIP, SKIP, SKIP, SKIP}, true, false, false, CTV_GOOD, CTV_NOT_NOW, CTV_NOT_NOW, "",
     "Connect to Wi-Fi in the system settings.", "Connect to Wi-Fi in the system settings.", true, true},
    {"no wifi and the clock is wrong: wifi first", {W_NONE, SKIP, SKIP, SKIP, SKIP}, true, true, false, CTV_GOOD, CTV_NOT_NOW, CTV_NOT_NOW, "",
     "Connect to Wi-Fi in the system settings.", "Connect to Wi-Fi in the system settings.", true, true},
    {"wifi hides consoles", {W_GOOD, L_BLOCKED, N_OK, H_OPEN, D_FAST}, true, false, false, CTV_CHOPPY, CTV_GOOD, CTV_GOOD,
     "Friends may not see you on this Wi-Fi.", "", "", true, true},
    {"home network blocks a friend", {W_GOOD, L_OK, N_OK, H_CGNAT, D_FAST}, true, false, false, CTV_GOOD, CTV_NOT_NOW, CTV_GOOD, "",
     "Friends can't reach you here. Let a friend start the game.", "", true, true},
    {"home network may block a friend", {W_GOOD, L_OK, N_OK, H_MAYBE, D_FAST}, true, false, false, CTV_GOOD, CTV_CHOPPY, CTV_GOOD, "",
     "May not work from here. Let a friend start the game.", "", true, true},
    {"home network unknown", {W_GOOD, L_OK, N_OK, H_UNK, D_FAST}, true, false, false, CTV_GOOD, CTV_CHOPPY, CTV_GOOD, "",
     "May not work from here. Let a friend start the game.", "", true, true},
    {"slow service", {W_GOOD, L_OK, N_OK, H_OPEN, D_SLOW}, true, false, false, CTV_GOOD, CTV_GOOD, CTV_CHOPPY, "", "",
     "Playing in the same room will be smoother.", true, true},
    {"service unreachable", {W_GOOD, L_OK, N_DOWN, SKIP, SKIP}, true, false, false, CTV_GOOD, CTV_UNKNOWN, CTV_NOT_NOW, "", "",
     "Check your Wi-Fi, or try again later.", false, true},
    {"service needs an update", {W_GOOD, L_OK, N_UPD, SKIP, SKIP}, true, false, false, CTV_GOOD, CTV_UNKNOWN, CTV_NOT_NOW, "", "",
     "Please update the game to play online.", false, true},
    {"service paused", {W_GOOD, L_OK, N_OFF, SKIP, SKIP}, true, false, false, CTV_GOOD, CTV_UNKNOWN, CTV_NOT_NOW, "", "", "Please try again later.", false, true},
    {"the console's date and time are wrong", {W_GOOD, L_OK, N_DOWN, SKIP, SKIP}, true, true, false, CTV_GOOD, CTV_UNKNOWN, CTV_NOT_NOW, "", "",
     "Please set the date and time in the system settings.", false, true},
    {"clock flag but the service answered", {W_GOOD, L_OK, N_OK, H_OPEN, D_FAST}, true, true, false, CTV_GOOD, CTV_GOOD, CTV_GOOD, "", "", "", true, true},
    {"no online service in this build: the third question is absent", {W_GOOD, L_OK, N_NOSVC, SKIP, SKIP}, false, false, false, CTV_GOOD, CTV_UNKNOWN,
     CTV_UNKNOWN, "", "", "", false, false},
    {"no online service, hosting tested anyway", {W_GOOD, L_OK, N_NOSVC, H_CGNAT, SKIP}, false, false, false, CTV_GOOD, CTV_NOT_NOW, CTV_UNKNOWN, "",
     "Friends can't reach you here. Let a friend start the game.", "", true, false},
    {"no online service, the view says so too", {W_GOOD, L_OK, N_NOSVC, H_OPEN, SKIP}, true, false, false, CTV_GOOD, CTV_GOOD, CTV_UNKNOWN, "", "", "", true, false},
    {"privacy setting on: the code goes through the service", {W_GOOD, L_OK, N_OK, SKIP, D_OK}, true, false, true, CTV_GOOD, CTV_GOOD, CTV_GOOD, "", "", "", true, true},
    {"in a room: hosting not tested", {W_GOOD, L_OK, N_OK, SKIP, D_OK}, true, false, false, CTV_GOOD, CTV_UNKNOWN, CTV_GOOD, "", "", "", false, true},
    {"everything bad but nearby", {W_NONE, SKIP, SKIP, SKIP, SKIP}, false, false, false, CTV_GOOD, CTV_NOT_NOW, CTV_UNKNOWN, "",
     "Connect to Wi-Fi in the system settings.", "", true, false},
};

CtVerdict toV(uint8_t v) { return static_cast<CtVerdict>(v); }
const char* vName(int v) { return v == CTV_GOOD ? "Good" : (v == CTV_CHOPPY ? "May be choppy" : "Not right now"); }

// the screen's rows with the line under the title that goes with each (the cursor is put on each row in turn, as a touch does)
struct Shown { std::string label, value; int ping; std::string line; };
std::vector<Shown> shownNow() {
    static MpPanel::Model m;
    std::vector<Shown> out;
    S().buildModel(&m);
    const int n = m.rowCount;
    for (int i = 0; i < n; i++) {
        S().pointAtVisible(i);
        S().buildModel(&m);
        out.push_back({U8(m.rows[i].label), U8(m.rows[i].value), m.rows[i].ping, U8(m.hint)});
    }
    return out;
}

void checkCase(const Case& c) {
    openTest();
    fake::service = c.service;
    fake::ct.clockWrong = c.clock;
    fake::ct.relay = c.relay;
    ctSet(c.r, std::string(c.name) == "in a room: hosting not tested");
    frames(2);
    const CtAnswers a = MpMenu::ConnTestJudge(fake::ct.view, c.service, c.clock, c.relay);
    CHECK(toV(a.room.verdict) == c.room && a.room.ready);
    CHECK(toV(a.far.verdict) == c.far);
    CHECK(c.onlineRow ? toV(a.online.verdict) == c.online : a.onlineAbsent);
    CHECK(wordEn(a.room.line) == c.roomLine && wordEn(a.far.line) == c.farLine && (!c.onlineRow || wordEn(a.online.line) == c.onlineLine));
    // the screen: one row per question
    const std::vector<Shown> sh = shownNow();
    const size_t want = 1 + (c.farRow ? 1 : 0) + (c.onlineRow ? 1 : 0);
    CHECK(sh.size() == want);
    if (sh.size() != want) { fprintf(stderr, "  rows: %s: %zu, wanted %zu\n", c.name, sh.size(), want); return; }
    size_t i = 0;
    CHECK(sh[i].label == "People in this room" && sh[i].value == vName(c.room));
    CHECK(sh[i].line == c.roomLine);
    CHECK(sh[i].ping == (c.room == CTV_GOOD ? 4 : (c.room == CTV_CHOPPY ? 2 : 1)));
    ++i;
    if (c.farRow) {
        CHECK(sh[i].label == "A friend far away" && sh[i].value == vName(c.far));
        CHECK(sh[i].line == c.farLine);
        CHECK(sh[i].ping == (c.far == CTV_GOOD ? 4 : (c.far == CTV_CHOPPY ? 2 : 1)));
        ++i;
    }
    if (c.onlineRow) {
        CHECK(sh[i].label == "Anyone online" && sh[i].value == vName(c.online));
        CHECK(sh[i].line == c.onlineLine);
        // a signal picture per verdict: full, half, nearly empty
        CHECK(sh[i].ping == (c.online == CTV_GOOD ? 4 : (c.online == CTV_CHOPPY ? 2 : 1)));
    }
    // nothing else on the screen: no numbers anywhere, no window
    for (const Shown& r : sh) CHECK(r.value.find_first_of("0123456789") == std::string::npos && r.line.size() <= 79);
    CHECK(S().screen() == MpMenu::S_CONNTEST && !io.open);
}

}  // namespace

// conntest: the Connection Test screen
void testConnTest() {
    static MpPanel::Model m;
    useLang(MpMsgText::LANG_EN);

    // ---- reaching the screen and leaving it
    // (flow: its homes are the Multiplayer menu and the Join list; Square is the guide button)
    for (int from = 0; from < 1; from++) {   // (flow, one door: How to Play on the Multiplayer menu, then "Run a Connection Test?")
        fresh();
        const Scr was = MpMenu::S_MAIN;
        reachConnTest();   // (uxfix #7: after a join that did not work, not after How to Play)
        CHECK(S().screen() == MpMenu::S_CONNTEST && fake::ct.starts == 1);
        CHECK(fake::ct.lastInRoom == false);
        frames(5);
        CHECK(fake::ct.pumps >= 5);
        back();
        CHECK(S().screen() == was && fake::ct.cancels == 1);
        const int pumps = fake::ct.pumps;
        frames(5);
        CHECK(fake::ct.pumps == pumps);   // (not pumped once the screen is gone)
    }
    // the guide shows the button on those screens (and not elsewhere)
    fresh();
    openMain();
    S().buildModel(&m);
    bool sq = false;
    for (int i = 0; i < m.buttonCount; i++) sq = sq || U8(m.buttons[i].label) == "Test";
    CHECK(!sq && m.buttonCount <= MpPanel::kMaxButtons);   // (review: "Test" is no button of its own any more)
    // cannot start: a buzzer, no screen
    fresh();
    fake::ct.startOk = false;
    reachConnTest();
    CHECK(S().screen() == MpMenu::S_MAIN);

    // ---- running: the first question "Checking...", the rest waiting (dim); the waiting line says what is being checked, in every step
    openTest();
    {
        const std::vector<RowInfo> rs = rowsNow();
        CHECK(rs.size() == 3 && rs[0].label == "People in this room" && rs[0].value == "Checking..." && rs[1].value.empty() && rs[2].value.empty());
        S().buildModel(&m);
        CHECK(U8(m.title) == "Who Can I Play With?" && U8(m.subtitle) == "Looking for friends in this room...");
        CHECK(m.rows[1].marks & MpPanel::MARK_DISABLED);
        const std::vector<std::string> b = buttonsNow();
        CHECK(b.size() == 1 && b[0] == "Back");   // (Back only while it runs)
    }
    {   // each step names what it waits for; a question shows its answer as soon as its checks are done
        struct Step { int running; const char* sub; const char* row0; const char* row1; const char* row2; } steps[] = {
            {MPCT_WIFI, "Looking for friends in this room...", "Checking...", "", ""},
            {MPCT_LOCAL, "Looking for friends in this room...", "Checking...", "", ""},
            {MPCT_INTERNET, "Checking online play...", "Good", "", "Checking..."},
            {MPCT_HOSTING, "Checking if a faraway friend can play...", "Good", "Checking...", ""},
            {MPCT_DELAY, "Checking online play...", "Good", "Good", "Checking..."},
        };
        for (const Step& st : steps) {
            openTest();
            MpCtView& v = fake::ct.view;
            for (int i = 0; i < MPCT_CHECKS; i++) {
                v.state[i] = i < st.running ? MPCT_ST_DONE : (i == st.running ? MPCT_ST_RUNNING : MPCT_ST_WAITING);
                v.result[i] = 0;
            }
            v.result[MPCT_WIFI] = W_GOOD, v.result[MPCT_LOCAL] = L_OK, v.result[MPCT_INTERNET] = N_OK, v.result[MPCT_HOSTING] = H_OPEN;
            if (st.running == MPCT_WIFI) v.state[MPCT_WIFI] = MPCT_ST_RUNNING;
            frames(2);
            const std::vector<RowInfo> rs = rowsNow();
            CHECK(rs.size() == 3);
            if (rs.size() == 3) CHECK(rs[0].value == st.row0 && rs[1].value == st.row1 && rs[2].value == st.row2);
            CHECK(subtitleNow() == st.sub);
            for (int l = 0; l < kLangCount; l++) {   // (and in every language the waiting line is there)
                useLang(l);
                CHECK(!subtitleNow().empty() && subtitleNow() != "?");
                S().buildModel(&m);
                CHECK(m.subtitle[0] != 0);
            }
            useLang(MpMsgText::LANG_EN);
        }
    }

    // ---- the verdict table (judgement and screen)
    for (const Case& c : kCases) checkCase(c);

    // ---- finished: Check Again and Back; no Details, no Save Report, no window, no numbers
    openTest();
    const int opened0 = io.opened;   // (windows opened so far: the menu's own, if any)
    ctFinish(CT_GOOD);
    {
        const std::vector<std::string> b = buttonsNow();
        CHECK(b.size() == 2 && b[0] == "Check Again" && b[1] == "Back");
        CHECK(subtitleNow().empty() && !io.open && io.opened == opened0);
        press(3); press(4);   // (Triangle / Square: nothing any more)
        CHECK(fake::ct.reports == 0 && io.opened == opened0 && S().screen() == MpMenu::S_CONNTEST);
    }
    const int starts = fake::ct.starts;
    ok();
    CHECK(fake::ct.starts == starts + 1 && S().screen() == MpMenu::S_CONNTEST);
    CHECK(subtitleNow() == "Looking for friends in this room...");
    ok();   // (Cross while it runs does nothing)
    CHECK(fake::ct.starts == starts + 1);
    ctFinish(CT_BAD);
    CHECK(io.opened == opened0);   // (a bad run opens no window either)
    back();
    CHECK(S().screen() == MpMenu::S_MAIN && fake::ct.cancels >= 1);

    // ---- the weak-link tip: never on this screen; due when the menu opens again (once), not after a good run
    fresh();
    reachConnTest();
    const int opened1 = io.opened;
    ctFinish(CT_SLOW);
    frames(3);
    CHECK(io.opened == opened1 && logCount("ui: window MP_POWER_SAVE") == 0);
    back();
    S().close();
    frames(30);
    openMain();
    CHECK(logCount("ui: window MP_POWER_SAVE") == 1);
    for (CtKind k : {CT_GOOD, CT_BAD}) {
        fresh();
        reachConnTest();
        ctFinish(k);
        frames(3);
        back();
        S().close();
        frames(30);
        openMain();
        CHECK(logCount("ui: window MP_POWER_SAVE") == 0);
    }
    // the tip is quiet up front: opening the session menu with a healthy link shows nothing, a slow friend shows it (once)
    fresh();
    session(true, 0, MP_UI_PLAYING);
    fake::players[0] = {true, "Me", MP_LOOK_MARIO, 30};
    fake::players[1] = {true, "Ros", MP_LOOK_LUIGI, 40};
    fake::local = 0;
    frames(2); S().openSession(); frames(3);
    CHECK(logCount("ui: window MP_POWER_SAVE") == 0);
    S().close();
    frames(120);   // (past the reopen guard)
    fake::players[1].ping = 400;
    S().openSession(); frames(3);
    CHECK(logCount("ui: window MP_POWER_SAVE") == 1);

    // ---- leaving the screen any other way cancels the run (a window's Start / close)
    fresh();
    reachConnTest();
    S().close();
    frames(3);
    CHECK(fake::ct.cancels == 1);

    // ---- Back at every moment: running (first frame, mid-run), the very frame the verdict arrives, finished, timed out, after run again
    struct Moment { const char* name; void (*arrange)(); };
    const Moment moments[] = {
        {"running, first frame", [] {}},
        {"running, second check", [] { fake::ct.view.state[0] = MPCT_ST_DONE, fake::ct.view.result[0] = W_GOOD, fake::ct.view.state[1] = MPCT_ST_RUNNING; frames(2); }},
        {"running, hosting", [] { for (int i = 0; i < 3; i++) fake::ct.view.state[i] = MPCT_ST_DONE; fake::ct.view.state[3] = MPCT_ST_RUNNING; frames(2); }},
        {"finished", [] { ctFinish(CT_GOOD); }},
        {"finished, bad", [] { ctFinish(CT_BAD); }},
        {"timed out", [] { frames(15 * 60 + 20); }},
        {"run again, running", [] { ctFinish(CT_GOOD); ok(); frames(2); }},
    };
    for (const Moment& mo : moments) {
        openTest();
        mo.arrange();
        CHECK(S().screen() == MpMenu::S_CONNTEST);
        back();
        CHECK(S().screen() == MpMenu::S_MAIN && fake::ct.cancels >= 1);
        if (S().screen() != MpMenu::S_MAIN) fprintf(stderr, "  Back failed: %s\n", mo.name);
    }
    {   // the frame the verdict arrives: the same step carries Back
        openTest();
        const uint8_t r[MPCT_CHECKS] = {W_GOOD, L_FOUND, N_OK, H_OPEN, D_FAST};
        ctSet(r);
        MpMenu::Input in;
        in.back = true;
        step(in);
        CHECK(S().screen() == MpMenu::S_MAIN);
    }

    // ---- the watchdog: a run that never ends stops being waited for; the screen says so; Check Again starts over; Back always works
    openTest();
    for (int f = 0; f < 15 * 60 - 40; f++) {   // (uxfix #7: the way in takes a few frames longer now)
        step();
        if (f % 97 == 0) CHECK(!subtitleNow().empty());   // (every frame of the wait carries its line)
    }
    CHECK(fake::ct.cancels == 0 && S().screen() == MpMenu::S_CONNTEST);
    frames(30);
    CHECK(fake::ct.cancels == 1 && subtitleNow() == "That took too long.");
    {
        const std::vector<std::string> b = buttonsNow();
        CHECK(b.size() == 2 && b[0] == "Check Again");
    }
    {
        const int st = fake::ct.starts;
        ok();
        CHECK(fake::ct.starts == st + 1 && subtitleNow() == "Looking for friends in this room...");
        frames(15 * 60 + 30);
        CHECK(subtitleNow() == "That took too long.");   // (the clock starts over with each run)
        back();
        CHECK(S().screen() == MpMenu::S_MAIN);
    }

    // ---- the clock line is exactly that sentence, and only when the service check failed (also through the service's own code)
    {
        openTest();
        fake::svcCode = "clock";
        const uint8_t r[MPCT_CHECKS] = {W_GOOD, L_OK, N_DOWN, SKIP, SKIP};
        ctSet(r);
        frames(2);
        const std::vector<Shown> sh = shownNow();
        CHECK(sh.size() == 2 && sh[1].label == "Anyone online" && sh[1].value == "Not right now" &&
              sh[1].line == "Please set the date and time in the system settings.");
        fake::svcCode = "";
        frames(2);
        CHECK(shownNow()[1].line == "Check your Wi-Fi, or try again later.");   // (a generic failure is not the clock)
    }

    // ---- the absent third question: no row, no line, no dead row, in every language; the screen keeps working
    {
        openTest();
        fake::service = false;
        const uint8_t r[MPCT_CHECKS] = {W_GOOD, L_OK, N_NOSVC, SKIP, SKIP};
        ctSet(r);
        for (int l = 0; l < kLangCount; l++) {
            useLang(l);
            const std::vector<RowInfo> rs = rowsNow();
            CHECK(rs.size() == 1);   // (the room question alone)
            for (const RowInfo& x : rs) CHECK(!x.label.empty() && x.label != "?");
            CHECK(U8(MpMenu::ConnTestWord(0, l)).size() > 0);
        }
        useLang(MpMsgText::LANG_EN);
        // while it runs too: a build without the service never shows the third question, not even "Checking..."
        openTest();
        fake::service = false;
        frames(2);
        CHECK(rowsNow().size() == 2);   // (room, far friend: the third is absent from the first frame)
    }

    // ---- the queries for the gathering screen: the far-friend verdict, and when to steer to the online service
    {
        fresh();
        CHECK(MpMenu::ConnTestFarVerdict() == CTV_UNKNOWN && !MpMenu::ConnTestSteerFarToOnline());   // (no test yet)
        reachConnTest();
        CHECK(MpMenu::ConnTestFarVerdict() == CTV_UNKNOWN);   // (running)
        struct Q { const char* name; R r[MPCT_CHECKS]; bool service, relay, wifi; CtVerdict far; bool steer; } qs[] = {
            {"good", {W_GOOD, L_OK, N_OK, H_OPEN, D_FAST}, true, false, true, CTV_GOOD, false},
            {"maybe", {W_GOOD, L_OK, N_OK, H_MAYBE, D_FAST}, true, false, true, CTV_CHOPPY, false},
            {"blocked: steer", {W_GOOD, L_OK, N_OK, H_CGNAT, D_FAST}, true, false, true, CTV_NOT_NOW, true},
            {"blocked, service choppy: steer", {W_GOOD, L_OK, N_OK, H_CGNAT, D_SLOW}, true, false, true, CTV_NOT_NOW, true},
            {"blocked, service down: nowhere to steer", {W_GOOD, L_OK, N_DOWN, H_CGNAT, SKIP}, true, false, true, CTV_NOT_NOW, false},
            {"blocked, no service in this build", {W_GOOD, L_OK, N_NOSVC, H_CGNAT, SKIP}, false, false, true, CTV_NOT_NOW, false},
            {"no wifi", {W_NONE, SKIP, SKIP, SKIP, SKIP}, true, false, false, CTV_NOT_NOW, false},
            {"not tested", {W_GOOD, L_OK, N_OK, SKIP, D_OK}, true, false, true, CTV_UNKNOWN, false},
            {"privacy setting", {W_GOOD, L_OK, N_OK, SKIP, D_OK}, true, true, true, CTV_GOOD, false},
        };
        for (const Q& q : qs) {
            openTest();
            fake::service = q.service, fake::ct.relay = q.relay, fake::wifi = q.wifi;
            ctSet(q.r);
            if (MpMenu::ConnTestFarVerdict() != q.far || MpMenu::ConnTestSteerFarToOnline() != q.steer) fprintf(stderr, "  query: %s\n", q.name);
            CHECK(MpMenu::ConnTestFarVerdict() == q.far);
            CHECK(MpMenu::ConnTestSteerFarToOnline() == q.steer);
            // the answer outlives the screen (the gathering screen asks after Back)
            back();
            CHECK(S().screen() == MpMenu::S_MAIN);
            CHECK(MpMenu::ConnTestFarVerdict() == q.far && MpMenu::ConnTestSteerFarToOnline() == q.steer);
        }
        // the Wi-Fi came or went since the test: it says nothing
        openTest();
        const uint8_t r[MPCT_CHECKS] = {W_GOOD, L_OK, N_OK, H_CGNAT, D_FAST};
        ctSet(r);
        CHECK(MpMenu::ConnTestSteerFarToOnline());
        fake::wifi = false;
        CHECK(MpMenu::ConnTestFarVerdict() == CTV_UNKNOWN && !MpMenu::ConnTestSteerFarToOnline());
        fake::wifi = true;
        // a test cancelled half way is not an answer
        openTest();
        fake::ct.view.state[0] = fake::ct.view.state[1] = fake::ct.view.state[2] = MPCT_ST_DONE;
        fake::ct.view.result[3] = H_CGNAT;
        back();
        CHECK(MpMenu::ConnTestFarVerdict() == CTV_UNKNOWN);
    }

    // ---- every combination of results, every language: three rows at most, every line within the title band's 79, the line is the word's
    // own text (nothing truncated), the first line of a finished test is where the cursor starts
    {
        const std::vector<R> wifi = {W_GOOD, W_WEAK, W_NONE, W_UNK, SKIP}, local = {L_FOUND, L_OK, L_BLOCKED, SKIP}, net = {N_OK, N_DOWN, N_NOSVC, N_UPD, N_OFF, SKIP},
                             host = {H_OPEN, H_MAYBE, H_CGNAT, H_UNK, SKIP, MPCT_R_NONE}, delay = {D_FAST, D_OK, D_SLOW, SKIP};
        openTest();
        int combos = 0;
        static MpPanel::Model mm;
        for (int l = 0; l < kLangCount; l++) {
            useLang(l);
            for (R w : wifi) for (R lo : local) for (R n : net) for (R h : host) for (R d : delay)
            for (int flags = 0; flags < 8; flags++) {
                const bool service = flags & 1, clock = flags & 2, relay = flags & 4;
                fake::service = service, fake::ct.clockWrong = clock, fake::ct.relay = relay;
                const R r[MPCT_CHECKS] = {w, lo, n, h, d};
                ctSet(r);
                const CtAnswers a = MpMenu::ConnTestJudge(fake::ct.view, service, clock, relay);
                S().buildModel(&mm);
                ++combos;
                if (mm.rowCount < 1 || mm.rowCount > 3) { CHECK(mm.rowCount >= 1 && mm.rowCount <= 3); continue; }
                const int rows = 1 + (a.far.verdict != CTV_UNKNOWN ? 1 : 0) + (a.onlineAbsent ? 0 : 1);
                CHECK(mm.rowCount == rows);
                const CtAnswer* qs[3] = {&a.room, &a.far, &a.online};
                int row = 0;
                for (int k = 0; k < 3; k++) {
                    if ((k == 1 && a.far.verdict == CTV_UNKNOWN) || (k == 2 && a.onlineAbsent)) continue;
                    S().pointAtVisible(row);
                    S().buildModel(&mm);
                    const char16_t* want = qs[k]->line >= 0 ? MpMenu::ConnTestWord(qs[k]->line, l) : u"";
                    bool same = true;
                    for (int c = 0;; c++) { if (mm.hint[c] != want[c]) same = false; if (!want[c] || !mm.hint[c]) break; }
                    if (!same) CHECK(same);
                    CHECK(mm.rows[row].label[0] != 0 && mm.rows[row].value[0] != 0);
                    int len = 0;
                    while (mm.rows[row].value[len]) ++len;
                    CHECK(len <= 27);
                    ++row;
                }
            }
        }
        useLang(MpMsgText::LANG_EN);
        fprintf(stderr, "  connection test: %d combinations x 5 languages checked\n", combos / kLangCount);
    }

    // ---- the cursor: it starts on the first question there is something to do about, moves over the questions, a touch picks one
    {
        struct K { const char* name; R r[MPCT_CHECKS]; const char* first; } ks[] = {
            {"all good: the first", {W_GOOD, L_OK, N_OK, H_OPEN, D_FAST}, "People in this room"},
            {"far friend blocked", {W_GOOD, L_OK, N_OK, H_CGNAT, D_FAST}, "A friend far away"},
            {"service slow", {W_GOOD, L_OK, N_OK, H_OPEN, D_SLOW}, "Anyone online"},
            {"weak wifi: the room first", {W_WEAK, L_OK, N_OK, H_OPEN, D_OK}, "People in this room"},
        };
        for (const K& k : ks) {
            openTest();
            ctSet(k.r);
            frames(3);
            CHECK(cursorLabel() == k.first);
            if (cursorLabel() != k.first) fprintf(stderr, "  cursor: %s: %s\n", k.name, cursorLabel().c_str());
        }
        openTest();
        const uint8_t r[MPCT_CHECKS] = {W_GOOD, L_OK, N_OK, H_OPEN, D_FAST};
        ctSet(r);
        frames(3);
        down();
        CHECK(cursorLabel() == "A friend far away");
        down(2);
        CHECK(cursorLabel() == "People in this room");   // (it wraps)
        press(8);
        CHECK(cursorLabel() == "Anyone online");
        S().pointAtVisible(1);
        CHECK(cursorLabel() == "A friend far away");
        CHECK(S().screen() == MpMenu::S_CONNTEST && fake::ct.starts == 1);   // (moving never starts anything)
        ok();
        CHECK(fake::ct.starts == 2);   // (Cross: Check Again, from any row)
    }

    // ---- the picture: every verdict case, every language, both pictures (16:9 and 4:3), the cursor on each row (its line under the title):
    // the shared geometry rules (words not too small, inside the window, clear of the bands, the guide on its band)
    {
        MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
        for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
        static MpPanel::Model gm;
        static MpPanel::Plan plan;
        static MpPanel::Scene scene;
        const char* langs[] = {"UsEnglish", "UsFrench", "UsSpanish", "EuGerman", "EuItalian"};
        int composed = 0;
        for (float width : {810.6667f, 608.0f})
        for (const char* l : langs) {
            for (int ci = 0; ci < static_cast<int>(sizeof kCases / sizeof kCases[0]) + 2; ci++) {
                openTest();
                MpMsgText::setLanguage(MpMsgText::languageIndex(l));
                const Case* c = ci < static_cast<int>(sizeof kCases / sizeof kCases[0]) ? &kCases[ci] : nullptr;
                if (c) {
                    fake::service = c->service, fake::ct.clockWrong = c->clock, fake::ct.relay = c->relay;
                    ctSet(c->r);
                } else if (ci % 2 == 0) {
                    fake::ct.view.state[0] = fake::ct.view.state[1] = MPCT_ST_DONE;   // running: the waiting line
                    fake::ct.view.state[2] = MPCT_ST_RUNNING;
                    fake::ct.view.result[0] = W_GOOD, fake::ct.view.result[1] = L_OK;
                } else {
                    frames(15 * 60 + 20);   // timed out
                }
                frames(2);
                S().buildModel(&gm);
                const int rows = gm.rowCount;
                for (int row = 0; row < (rows > 0 ? rows : 1); row++) {
                    S().pointAtVisible(row);
                    S().buildModel(&gm);
                    MpPanel::Screen scr;
                    scr.width = width;
                    MpPanel::beginScene(&scene);
                    MpPanel::begin(&plan);
                    MpPanel::compose(gm, scr, fm, 0.25f, &scene, &plan);
                    CHECK(!plan.overflow);
                    uinativeGeometry(c ? c->name : "conntest(running or timed out)", l, gm, scr, scene, plan);
                    ++composed;
                }
            }
        }
        useLang(MpMsgText::LANG_EN);
        fprintf(stderr, "  connection test: %d pictures checked (cases x rows x 5 languages x 2 widths)\n", composed);
    }

    // ---- the words of the screen: all five languages present, short enough for their place
    for (int l = 0; l < kLangCount; l++) {
        for (int i = 0; i < MpMenu::ConnTestWordCount(); i++) {
            const char16_t* t = MpMenu::ConnTestWord(i, l);
            CHECK(t && *t);
            int n = 0;
            for (const char16_t* p = t; p && *p; ++p) ++n;
            const bool title = i == wordIndex("Who Can I Play With?"), line = wordEn(i).size() > 28 && i != wordIndex("Looking for friends in this room...");
            if (title) CHECK(n <= 47);
            else CHECK(n <= 78);   // (a sentence: two rows of 39)
            if (wordEn(i) == "Good" || wordEn(i) == "May be choppy" || wordEn(i) == "Not right now" || wordEn(i) == "Checking...") CHECK(n <= 27);   // (a value)
            if (wordEn(i) == "People in this room" || wordEn(i) == "A friend far away" || wordEn(i) == "Anyone online") CHECK(n <= 39);   // (a question)
            if (wordEn(i) == "Check Again") CHECK(n <= 19);   // (a guide button)
            (void)line;
        }
    }

    // ---- the offer after a join that failed for a connection reason, never for the others
    struct Case2 { MpUiError err; bool offer; } cases[] = {{MP_ERR_TIMEOUT, true}, {MP_ERR_NO_WIFI, true}, {MP_ERR_SERVICE_DOWN, true}, {MP_ERR_NAT, true},
                                                          {MP_ERR_ROOM_FULL, false}, {MP_ERR_ROOM_NOT_FOUND, false}};
    for (const Case2& c : cases) {
        fresh();
        toGather(); gatherHeads(1, 1); ok();   // Play with Anyone (Quick Match): busy
        CHECK(S().screen() == MpMenu::S_BUSY);
        fake::status.phase = MP_UI_FAILED;
        fake::status.error = c.err;
        frames(6);
        // (the test io answers every window Yes at once: the error window, then the offer, which takes us to the screen)
        CHECK((S().screen() == MpMenu::S_CONNTEST) == c.offer);
        CHECK((logCount("ui: window MP_CT_OFFER") == 1) == c.offer);
        CHECK(fake::ct.starts == (c.offer ? 1 : 0));
        if (c.offer) { back(); CHECK(S().screen() == MpMenu::S_MAIN); }
    }
    // the offer answered No: the usual screen, no test
    fresh();
    toGather(); gatherHeads(1, 1); ok();
    fake::status.phase = MP_UI_FAILED, fake::status.error = MP_ERR_TIMEOUT;
    io.answer = MpMenu::DlgState::No;
    frames(6);
    CHECK(S().screen() == MpMenu::S_GATHER && fake::ct.starts == 0 && logCount("ui: window MP_CT_OFFER") == 1);   // (uxfix #3: Play with Anyone came from the gathering screen: it looks for friends again)

    // 13 Nov: the silent sign-in that Play with Anyone needs fails for a connection reason (no reason of the service's own): the offer follows too
    fresh();
    fake::ensureResult = -static_cast<int>(MP_ERR_SERVICE_DOWN);
    toGather(); gatherHeads(1, 1);
    ok(); frames(8);   // (the first row is Play with Anyone, as in the cases above)
    CHECK(S().screen() == MpMenu::S_CONNTEST && fake::ct.starts == 1 && logCount("ui: window MP_CT_OFFER") == 1);
    fake::ensureResult = 1;

    // ---- the update notice: a toast, once, in any state (the menu closed, a session)
    fresh();
    frames(2);
    CHECK(logCount("ui: update notice shown") == 0);
    fake::ct.updatePending = true;
    frames(2);
    CHECK(logCount("ui: update notice shown") == 1 && logCount("ui: toast \"A newer version of the game is available.\"") == 1);
    frames(5);
    CHECK(logCount("ui: update notice shown") == 1);
    fresh();
    session(true, 0, MP_UI_PLAYING);
    fake::ct.updatePending = true;
    frames(3);
    CHECK(logCount("ui: update notice shown") == 1);

    useLang(MpMsgText::LANG_EN);
    fresh();
    fprintf(stderr, "connection test: ok\n");
}

// ====================================================================================================================================
// Part 2: banned words. No technical or role word (terms.md: host / guest / address / router / ...) in anything a player can read, in
// any of the five languages. Tokens are whole words (letters only), compared in lower case.
// ====================================================================================================================================
namespace {

// words nobody may read, any language (technical words are English everywhere)
const char16_t* const kBannedAll[] = {u"nat", u"cgnat", u"upnp", u"stun", u"udp", u"tcp", u"ip", u"port", u"ports", u"router", u"routers", u"ping", u"packet", u"packets",
    u"relay", u"relays", u"server", u"servers", u"address", u"addresses", u"host", u"hosts", u"hosting", u"hosted", u"guest", u"guests", u"latency",
    u"delay", u"provider", u"isp", u"ms", u"mbps", u"kbps",
    u"join", u"joins", u"joining", u"rejoin", u"rejoins"};   // (wire; owner: no Host / Join. "joined" stays: a player who came in is announced)
const char16_t* const kBannedLang[kLangCount][20] = {
    {nullptr},
    {u"serveur", u"serveurs", u"adresse", u"adresses", u"hôte", u"hôtes", u"hébergement", u"héberger", u"routeur", u"box", u"relais", u"paquet", u"opérateur", u"latence",
     u"rejoindre", u"rejoins", u"rejoignez", nullptr},
    {u"servidor", u"servidores", u"dirección", u"direcciones", u"anfitrión", u"anfitriona", u"puerto", u"paquete", u"relé", u"proveedor", u"latencia", u"retardo", u"huésped", u"unirse", u"unirte", u"únete", u"unete", u"unirnos", nullptr},
    {u"adresse", u"adressen", u"gastgeber", u"gastgeberin", u"paket", u"pakete", u"anbieter", u"latenz", u"verzögerung", u"gast", u"gäste", u"beitreten", u"beitrittst", nullptr},
    {u"indirizzo", u"indirizzi", u"pacchetto", u"pacchetti", u"relè", u"latenza", u"ritardo", u"ospite", u"ospiti", u"unirsi", u"unirti", u"unisciti", nullptr},
};
bool isLetter(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= 0xC0 && c < 0x250 && c != 0xD7 && c != 0xF7); }
char16_t lower(char16_t c) {
    if (c >= u'A' && c <= u'Z') return static_cast<char16_t>(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return static_cast<char16_t>(c + 32);
    return c;
}
// the banned word in `text` (UTF-8, "" none); text is markup-free after formatting (pictures are U+E0xx: not letters)
std::string bannedIn(const char16_t* text, int lang) {
    std::u16string tok;
    auto check = [&](const std::u16string& t) -> std::string {
        if (t.empty()) return "";
        for (const char16_t* b : kBannedAll) if (t == b) return std::string(b, b + std::char_traits<char16_t>::length(b));
        for (const char16_t* b : kBannedLang[lang]) {
            if (!b) break;
            if (t == b) { std::string o; for (char16_t c : t) o += c < 128 ? static_cast<char>(c) : '?'; return o; }
        }
        return "";
    };
    std::u16string prev;
    for (const char16_t* p = text;; ++p) {
        if (*p && isLetter(*p)) { tok += lower(*p); continue; }
        std::string hit = check(tok);
        // (flow: Spanish "una dirección" / "+ dirección" is a direction of the D-pad, not an address)
        if (lang == 2 && tok == u"direcci\u00f3n" && (prev == u"una" || (p - tok.size() - 2 >= text && *(p - tok.size() - 2) == u'+'))) hit.clear();
        if (!hit.empty()) return hit;
        if (!tok.empty()) prev = tok;
        tok.clear();
        if (!*p) break;
    }
    return "";
}

std::string ascii(const char16_t* t) { std::string o; for (; *t; ++t) o += *t < 128 ? static_cast<char>(*t) : '?'; return o; }

// the fields of a model a player can read
void visibleStrings(const MpPanel::Model& m, std::vector<std::pair<std::string, std::u16string>>* out) {
    auto add = [out](const char* field, const char16_t* t) { if (t && *t) out->push_back({field, t}); };
    add("title", m.title), add("subtitle", m.subtitle), add("bigLabel", m.bigLabel), add("big", m.big), add("status", m.status), add("hint", m.hint);
    for (int i = 0; i < m.tabCount && i < 3; i++) add("tab", m.tabs[i]);
    for (int i = 0; i < m.rowCount; i++) add("row.lead", m.rows[i].lead), add("row.label", m.rows[i].label), add("row.value", m.rows[i].value);
    for (int i = 0; i < m.buttonCount; i++) add("button", m.buttons[i].label);
    for (int i = 0; i < m.cellCount; i++) add("cell", m.cells[i].label);
}

// Hits in other families' strings at the split (11 Nov), counted by (kind, language): the screens' own strings and the whole word table.
// Flow's rewrite (words/rewrite.tsv) removes them; each fix lowers the real count, and this number must then be lowered with it. A NEW
// banned word anywhere raises the count and fails the test. Set MP_WORDS_LIST=1 to print every hit.
constexpr int kKnownScreenHits = 0;   // (flow, words pass: none left; a new one fails the test)
constexpr int kKnownTableHits = 0;
}  // namespace

void testConnTestWords() {
    const bool list = getenv("MP_WORDS_LIST") != nullptr;
    // (a) the screen's own words: zero tolerance
    int own = 0;
    for (int l = 0; l < kLangCount; l++)
        for (int i = 0; i < MpMenu::ConnTestWordCount(); i++) {
            const std::string hit = bannedIn(MpMenu::ConnTestWord(i, l), l);
            if (!hit.empty()) { ++own; fprintf(stderr, "  BANNED in the Connection Test (%s, word %d): \"%s\" in \"%s\"\n", kLangNames[l], i, hit.c_str(), ascii(MpMenu::ConnTestWord(i, l)).c_str()); }
        }
    CHECK(own == 0);
    // (b) the checker itself: it finds what it must (so an emptied list cannot pass silently)
    CHECK(bannedIn(u"Your Address 127.0.0.1", 0) == "address");
    CHECK(bannedIn(u"Open UDP port 41100 on your router", 0) == "udp");
    CHECK(bannedIn(u"Nur der Gastgeber", 3) == "gastgeber");
    CHECK(bannedIn(u"Hôte : Mario", 1) == "hôte" || bannedIn(u"Hôte : Mario", 1) == "h?te");
    CHECK(bannedIn(u"Tu proveedor bloquea", 2) == "proveedor");
    CHECK(bannedIn(u"L'host sceglie", 4) == "host");
    CHECK(bannedIn(u"Press Start to support the Portal", 0).empty());   // (whole words only)
    CHECK(bannedIn(u"Wi-Fi is weak. Please try again in 5 minutes.", 0).empty());
    CHECK(bannedIn(u"Retourner à l'Observatoire", 1).empty());
    CHECK(bannedIn(u"Mantén \uE017 + dirección: mensaje rápido.", 2).empty() && bannedIn(u"oprime una dirección", 2).empty());
    CHECK(bannedIn(u"Escribe tu dirección", 2) == "direcci?n");
    CHECK(bannedIn(u"Join with a Code", 0) == "join" && bannedIn(u"Joining the game...", 0) == "joining" && bannedIn(u"Mario joined.", 0).empty());   // wire
    CHECK(bannedIn(u"Rejoindre avec un code", 1) == "rejoindre" && bannedIn(u"Unirse con un código", 2) == "unirse" && bannedIn(u"Mit Code beitreten", 3) == "beitreten" &&
          bannedIn(u"Entra con un codice", 4).empty() && bannedIn(u"Potrai unirti a lui", 4) == "unirti");

    // (c) every screen, every language (the real menu code on the fake port): what each screen shows
    std::set<std::string> screenHits;
    for (int l = 0; l < kLangCount; l++) {
        for (const Path& p : kPaths) {
            useLang(l);
            fresh();
            p.go();
            static MpPanel::Model m;
            S().buildModel(&m);
            std::vector<std::pair<std::string, std::u16string>> vis;
            visibleStrings(m, &vis);
            for (const auto& v : vis) {
                const std::string hit = bannedIn(v.second.c_str(), l);
                if (!hit.empty()) screenHits.insert(std::string(p.name) + " | " + kLangNames[l] + " | " + v.first + " | " + hit + " | " + ascii(v.second.c_str()));
            }
        }
    }
    useLang(MpMsgText::LANG_EN);
    fresh();
    // (d) the whole word table (windows, toasts, menus): a string a screen could show later
    std::set<std::string> tableHits;
    for (int l = 0; l < kLangCount; l++) {
        useLang(l);
        for (int id = 1; id < MP_T_COUNT; id++) {
            const char16_t* t = MpMsgText::raw(static_cast<MpText>(id));
            if (!t || !*t) continue;
            const std::string hit = bannedIn(t, l);
            if (!hit.empty()) tableHits.insert(std::to_string(id) + " | " + kLangNames[l] + " | " + hit + " | " + ascii(t));
        }
    }
    useLang(MpMsgText::LANG_EN);
    if (list) {
        for (const std::string& s : screenHits) fprintf(stderr, "  SCREEN %s\n", s.c_str());
        for (const std::string& s : tableHits) fprintf(stderr, "  TABLE %s\n", s.c_str());
    }
    fprintf(stderr, "  banned words: %zu on screens, %zu in the word table (known at the split: %d, %d)\n", screenHits.size(), tableHits.size(), kKnownScreenHits,
            kKnownTableHits);
    CHECK(static_cast<int>(screenHits.size()) <= kKnownScreenHits);
    CHECK(static_cast<int>(tableHits.size()) <= kKnownTableHits);
    fprintf(stderr, "banned words: ok\n");
}

// ====================================================================================================================================
// Part 3 (native Settings, 14 Nov): the Settings words are plain. No technical word (frame rate, resolution, config, profile, binding, ...)
// in any new text (MP_T_SET_ENTRY .. MP_T_COUNT - 1), in any of the five languages. Same whole-word rule as part 2, plus a few phrases.
// ====================================================================================================================================
namespace {
const char16_t* const kSettingsBannedAll[] = {u"fps", u"hz", u"gpu", u"cpu", u"core", u"cores", u"renderer", u"shader", u"shaders", u"texture", u"textures",
    u"resolution", u"frame", u"frames", u"framerate", u"env", u"cfg", u"config", u"configs", u"configuration", u"profile", u"profiles", u"binding", u"bindings",
    u"remap", u"remapping", u"override", u"overrides", u"overridden", u"reboot", u"vsync", u"hd", u"debug"};
const char16_t* const kSettingsBannedLang[kLangCount][24] = {
    {nullptr},
    {u"fréquence", u"résolution", u"configuration", u"profil", u"profils", u"textures", u"rendu", u"fichier-cfg", u"remappage", u"redémarrage", nullptr},
    {u"configuración", u"resolución", u"perfil", u"perfiles", u"fotogramas", u"cuadros", u"textura", u"texturas", u"reasignar", u"reasignación", nullptr},
    {u"auflösung", u"bildrate", u"bildfrequenz", u"konfiguration", u"profil", u"profile", u"textur", u"texturen", u"neubelegung", u"überschreiben", nullptr},
    {u"risoluzione", u"configurazione", u"profilo", u"profili", u"fotogrammi", u"frequenza", u"texture", u"riassegnazione", u"sovrascrivere", nullptr},
};
// the settings' own banned list on top of part 2's checker; phrases are looked up as lower-case substrings
std::string settingsBannedIn(const char16_t* text, int lang) {
    std::u16string tok, all;
    std::string hit;
    auto check = [&](const std::u16string& t) {
        if (t.empty() || !hit.empty()) return;
        for (const char16_t* b : kSettingsBannedAll) if (t == b) { hit = ascii(b); return; }
        for (const char16_t* b : kSettingsBannedLang[lang]) {
            if (!b) break;
            if (t == b) { hit = ascii(b); return; }
        }
    };
    for (const char16_t* p = text;; ++p) {
        if (*p && isLetter(*p)) { tok += lower(*p); all += lower(*p); continue; }
        check(tok);
        if (!tok.empty()) all += u' ';
        tok.clear();
        if (!*p) break;
    }
    if (hit.empty())
        for (const char16_t* ph : {u"frames per second", u"images par seconde", u"fotogramas por segundo", u"frames pro sekunde", u"fotogrammi al secondo", u"refresh rate",
                                   u"frame rate", u"environment", u"hardware", u"driver"})
            if (all.find(ph) != std::u16string::npos) { hit = ascii(ph); break; }
    return hit;
}
}  // namespace

void testSettingsWords() {
    // the checker finds what it must
    CHECK(settingsBannedIn(u"Set the FPS limit", 0) == "fps" && settingsBannedIn(u"Change the resolution", 0) == "resolution" && settingsBannedIn(u"Load the profile", 0) == "profile");
    CHECK(settingsBannedIn(u"Use all CPU cores", 0) == "cpu" && settingsBannedIn(u"Sets the frame rate", 0) == "frame" && settingsBannedIn(u"A config file", 0) == "config");
    CHECK(settingsBannedIn(u"Binding and remap", 0) == "binding" && settingsBannedIn(u"Override the env", 0) == "override" && settingsBannedIn(u"Reboot now", 0) == "reboot");
    CHECK(!settingsBannedIn(u"Résolution de l'image", 1).empty() && !settingsBannedIn(u"Fréquence d'images", 1).empty() &&
          settingsBannedIn(u"Mon profil", 1) == "profil");
    CHECK(!settingsBannedIn(u"Configuración", 2).empty() && !settingsBannedIn(u"La resolución", 2).empty() && !settingsBannedIn(u"Tu perfil", 2).empty());
    CHECK(!settingsBannedIn(u"Die Auflösung", 3).empty() && !settingsBannedIn(u"Dein Profil", 3).empty() && !settingsBannedIn(u"Konfiguration", 3).empty());
    CHECK(settingsBannedIn(u"La risoluzione", 4) == "risoluzione" && settingsBannedIn(u"Il profilo", 4) == "profilo" && settingsBannedIn(u"La configurazione", 4) == "configurazione");
    CHECK(settingsBannedIn(u"Tilt Strength: 5. A small core of the Star Ball", 0) == "core");
    CHECK(settingsBannedIn(u"Moves at its smoothest.", 0).empty() && settingsBannedIn(u"Reset Aim", 0).empty() && settingsBannedIn(u"Hardly any motion", 0).empty());
    // every new text, every language
    int checked = 0, bad = 0;
    for (int l = 0; l < kLangCount; l++) {
        useLang(l);
        for (int id = MP_T_SET_ENTRY; id < MP_T_COUNT; id++) {
            const char16_t* t = MpMsgText::raw(static_cast<MpText>(id));
            CHECK(t && *t);
            if (!t || !*t) continue;
            ++checked;
            std::string hit = settingsBannedIn(t, l);
            if (hit.empty()) hit = bannedIn(t, l);   // (part 2's words too: host, join, address, ...)
            if (!hit.empty()) { ++bad; fprintf(stderr, "  BANNED in Settings (%s, id %d): \"%s\" in \"%s\"\n", kLangNames[l], id, hit.c_str(), ascii(t).c_str()); }
        }
    }
    useLang(MpMsgText::LANG_EN);
    CHECK(checked == kLangCount * (MP_T_COUNT - MP_T_SET_ENTRY) && checked > 5 * 60);
    CHECK(bad == 0);
    fprintf(stderr, "settings words: %d texts, none banned\n", checked);
}
