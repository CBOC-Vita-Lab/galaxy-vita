// Host tests for the Star Race results card with the series column and the host's next step, and the head start line on the countdown
// (decomp/src/Game/MP/MpToast.cpp, MpPanelPlan.cpp). Build: sh tests/mp/race_card_build.sh (run from the repo root).
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpUi.hpp"
#include "port/mp_chat.h"

static int gFails = 0;
#define CHECK(c) do { if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ------------------------------------------------------------------ fake port (what MpToast.cpp asks of the session)
namespace fake {
bool host = true;
int local = 0, hostSlot = 0;
bool present[4] = {true, true, true, true};
const char* names[4] = {"Mario64", "Luigi64", "Ros", "Toad"};
int64_t clockMs = 1000;   // MpGameInternal::nowMs: the frames() below advance it 16.67 ms a frame; a test moves it by hand to model a hitch
}  // namespace fake
namespace MpGameInternal { int64_t nowMs() { return fake::clockMs; } }
extern "C" {
void mp_log(const char*, ...) {}
}
bool mp_active(void) { return true; }   // (uxwait: MpToast::update drops the wait banner when the session is gone; the fake session is live)
bool mp_is_host(void) { return fake::host; }
int mp_host_slot(void) { return fake::hostSlot; }
int mp_local_slot(void) { return fake::local; }
bool mp_slot_present(int s) { return s >= 0 && s < 4 && fake::present[s]; }
const char* mp_slot_name(int s) { return s >= 0 && s < 4 ? fake::names[s] : ""; }
MpLook mp_slot_look(int s) { return (MpLook)(s % 6); }

using namespace MpSeriesLogic;

static MpMsg results(int count, const int* slots, const uint32_t* turns, const int* places, const Tail* tail) {
    MpMsg m;
    MpRaceResults r;
    r = MpRaceResults();
    r.serial = 5;
    r.count = (uint8_t)count;
    for (int i = 0; i < count; i++) { r.e[i].slot = (uint8_t)slots[i]; r.e[i].turns = turns[i]; r.e[i].place = (uint8_t)places[i]; }
    if (tail) { r.hasTail = true; r.tail = *tail; }
    MpWriter w(m.data, sizeof m.data);
    mp_write_results(w, r);
    m.type = MP_MSG_RESULTS;
    m.fromSlot = 0;
    m.len = (uint16_t)w.n;
    return m;
}
static std::string U8(const char16_t* s) {
    std::string o;
    for (; *s; ++s) o += *s < 0x80 ? (char)*s : '#';
    return o;
}
static void frames(int n, bool ok = false, bool up = false, bool down = false, bool isHost = true, bool hold = false, int frameMs3 = 50) {
    static int64_t sub = 0;   // (thirds of a ms: 50 = 16.67 ms a frame)
    for (int i = 0; i < n; i++) {
        sub += frameMs3;
        fake::clockMs += sub / 3;
        sub %= 3;
        MpOverlay::resultsInput(up && i == 0, down && i == 0, ok && i == 0, isHost);
        MpOverlay::update(false, hold);
    }
}
static float Adv(void*, uint16_t) { return 16.0f; }
static bool HasG(void*, uint16_t) { return true; }

static void testCard() {
    static MpPanel::Model m;
    MpOverlay::reset();
    fake::host = true; fake::local = 0; fake::hostSlot = 0;
    for (bool& p : fake::present) p = true;
    fake::present[3] = false;
    // three players in a race: Ros (slot 2) won, Mario64 second, Luigi64 did not finish; the series (slot 3 left earlier with 4 points)
    Tail t;
    t.series.in = 0xF; t.series.left = 0x8;
    t.series.pts[0] = 5; t.series.pts[1] = 1; t.series.pts[2] = 7; t.series.pts[3] = 4;
    t.series.streakSlot = 2; t.series.streakLen = 1;
    t.next = 2;
    const int slots[3] = {2, 0, 1};
    const uint32_t turns[3] = {3000, 3300, MP_RACE_DNF};
    const int places[3] = {1, 2, 0};
    // the names of every slot are known to the card from earlier races
    fake::present[3] = true;
    { MpMsg first = results(1, (const int[]){3}, (const uint32_t[]){2500}, (const int[]){1}, nullptr); CHECK(MpOverlay::showResults(first)); frames(1); }
    fake::present[3] = false;
    const MpMsg msg = results(3, slots, turns, places, &t);
    CHECK(MpOverlay::showResults(msg));
    CHECK(MpOverlay::resultsHaveChoices());
    CHECK(MpOverlay::buildResults(&m));
    // rows: Ros, Mario64, Luigi64, the greyed Toad (left), then Race Again, Next Star, Leave Race
    CHECK(m.rowCount == 8);
    CHECK(m.rows[0].label[0] == 0 && (m.rows[0].marks & MpPanel::MARK_DISABLED) && m.rows[0].value[0] == u'\t' && U8(m.rows[0].value + 1) == "Series");   // the heading over the points column
    CHECK(U8(m.rows[1].label) == "Ros" && U8(m.rows[2].label) == "Mario64" && U8(m.rows[3].label) == "Luigi64" && U8(m.rows[4].label) == "Toad");
    CHECK(m.rows[4].marks & MpPanel::MARK_DISABLED);                       // the leaver's line is greyed
    CHECK(!(m.rows[1].marks & MpPanel::MARK_DISABLED));
    const char16_t star = MpMsgText::picChar(MpMsgText::PIC_STAR);
    auto series = [&](int row) {   // the second cell: a Power Star and the points
        const char16_t* v = m.rows[row].value;
        while (*v && *v != u'\t') ++v;
        CHECK(*v == u'\t' && v[1] == star);
        return U8(v + 2);
    };
    CHECK(series(1) == "7" && series(2) == "5" && series(3) == "1" && series(4) == "4");
    CHECK(U8(m.rows[5].label) == "Race Again" && U8(m.rows[6].label) == "Next Star" && U8(m.rows[7].label) == "Leave Race");
    CHECK(m.cursor == 5);                                                  // the default cursor is on Race Again
    CHECK(m.buttonCount == 1 && m.status[0] == 0);                         // the host: one button, no waiting line
    // no timer: still up after a minute
    frames(60 * 60);
    CHECK(MpOverlay::resultsShowing() && MpOverlay::buildResults(&m) && m.cursor == 5);
    // uxfix #9: a leader who walked away: after 90 s in all the card moves on by itself (Race Again); a press in between starts the wait over
    frames(20 * 60, false, true);
    frames(60 * 60);
    { int w = 0; CHECK(MpOverlay::resultsShowing() && !MpOverlay::takeResultsChoice(&w)); }
    frames(60 * 60);
    { int w = 0; CHECK(MpOverlay::takeResultsChoice(&w) && w == CH_AGAIN && MpOverlay::buildResults(&m) && m.cursor == 5 && (m.rows[6].marks & MpPanel::MARK_DISABLED)); }
    MpOverlay::dismissResults();
    CHECK(MpOverlay::showResults(msg));
    frames(40);
    // the host moves the cursor and chooses with one press; the swallow time of the first half second applies
    frames(1, false, false, true);
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 6);
    frames(1, false, false, true);
    frames(1, false, false, true);                                         // (stops at the last row)
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 7);
    frames(1, false, true);
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 6);
    int c = 0;
    CHECK(!MpOverlay::takeResultsChoice(&c));
    frames(1, true);
    CHECK(MpOverlay::takeResultsChoice(&c) && c == CH_NEXT && !MpOverlay::takeResultsChoice(&c));
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 6 && (m.rows[5].marks & MpPanel::MARK_DISABLED) && !(m.rows[6].marks & MpPanel::MARK_DISABLED));
    frames(1, true);   // a second press chooses nothing more
    CHECK(!MpOverlay::takeResultsChoice(&c));
    CHECK(MpOverlay::resultsShowing());
    MpOverlay::dismissResults();
    CHECK(!MpOverlay::resultsShowing() && !MpOverlay::buildResults(&m));

    // a guest: sees the host's line, the rows dim, no button, no input; then the host's choice appears as the cursor
    fake::host = false; fake::local = 1;
    CHECK(MpOverlay::showResults(msg));
    // smallfix: Leave Race is a live row of a player who is not the leader (the Cross hint shows); Next Star stays greyed. lobby: and Race Again?
    // (the ask), the cursor's first place
    CHECK(MpOverlay::buildResults(&m) && m.buttonCount == 1 && m.cursor == 5 && U8(m.status) == "Mario64 is choosing...");
    CHECK(U8(m.rows[5].label) == "Race Again?" && !(m.rows[5].marks & MpPanel::MARK_DISABLED) && (m.rows[6].marks & MpPanel::MARK_DISABLED) && !(m.rows[7].marks & MpPanel::MARK_DISABLED));
    CHECK(MpOverlay::resultsPoint(7) && !MpOverlay::resultsPoint(6) && !MpOverlay::resultsPoint(1) && MpOverlay::resultsPoint(5));   // (touch: only those two rows)
    int d;
    frames(40, false, false, false, false);                                // (the first half second swallows keys)
    frames(100, false, false, true, false);                                // (a guest's Down: Leave Race; Up: back to the ask; nothing is chosen by it)
    CHECK(!MpOverlay::takeResultsChoice(&d) && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsAsk() && MpOverlay::buildResults(&m) && m.cursor == 7);
    frames(1, false, true, false, false);
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 5);
    frames(60 * 60, false, false, false, false);                           // (no timer: a minute on the card and nothing happens)
    CHECK(MpOverlay::resultsShowing() && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsAsk());
    frames(1, true, false, false, false);                                  // Cross on Race Again?: asks the leader, once per card; the row greys, the line says so
    CHECK(MpOverlay::takeResultsAsk() && !MpOverlay::takeResultsAsk() && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsChoice(&d));
    CHECK(MpOverlay::buildResults(&m) && (m.rows[5].marks & MpPanel::MARK_DISABLED) && m.cursor == 7 && U8(m.status) == "Asked! Waiting for Mario64...");
    frames(1, true, false, false, false);                                  // the second Cross is on Leave Race: never a second ask
    CHECK(MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsAsk() && !MpOverlay::takeResultsChoice(&d));
    CHECK(MpOverlay::buildResults(&m) && m.buttonCount == 0 && m.cursor == 7 && !(m.rows[7].marks & MpPanel::MARK_DISABLED));
    frames(1, true, false, false, false);
    CHECK(!MpOverlay::takeResultsLeave());
    // lobby: the leader sees a guest's ask as a small nudge on his card ("<name> wants to race again"), once per player per card
    MpOverlay::dismissResults();
    fake::host = true; fake::local = 0;
    for (bool& p : fake::present) p = true;
    CHECK(MpOverlay::showResults(msg));
    frames(40);
    CHECK(MpOverlay::toastCount() == 0);
    MpOverlay::noteRaceAgainAsk(1);
    frames(60);
    CHECK(MpOverlay::toastCount() == 1 && U8(MpOverlay::toasts()[0].text) == "Luigi64 wants to race again");
    MpOverlay::noteRaceAgainAsk(1);                                        // (the same player asking again: no second nudge)
    MpOverlay::noteRaceAgainAsk(0);                                        // (the leader's own phrase is no ask)
    MpOverlay::noteRaceAgainAsk(7);                                        // (a slot that is not there)
    frames(2);
    CHECK(MpOverlay::toastCount() == 1);
    MpOverlay::noteRaceAgainAsk(2);                                        // (another player: their own nudge)
    frames(2);
    CHECK(MpOverlay::toastCount() == 2 && U8(MpOverlay::toasts()[1].text) == "Ros wants to race again");
    frames(60 * 10);                                                       // (the toasts go by themselves; the card stays)
    CHECK(MpOverlay::toastCount() == 0 && MpOverlay::resultsShowing());
    MpOverlay::dismissResults();
    CHECK(MpOverlay::showResults(msg));                                    // (a new card: asks count again)
    MpOverlay::noteRaceAgainAsk(1);
    frames(2);
    CHECK(MpOverlay::toastCount() == 1);
    frames(1, true);                                                       // (after the leader chose, or with no card, an ask shows nothing)
    frames(40);
    frames(1, true);
    int cc;
    MpOverlay::takeResultsChoice(&cc);
    MpOverlay::dismissResults();
    frames(60 * 10);
    MpOverlay::noteRaceAgainAsk(1);
    frames(2);
    CHECK(MpOverlay::toastCount() == 0);
    // a guest whose ask could not be sent (the chat's rate limit) may press again; a sent one may not
    fake::host = false; fake::local = 1;
    fake::present[3] = false;
    CHECK(MpOverlay::showResults(msg));
    frames(40, false, false, false, false);
    frames(1, true, false, false, false);
    CHECK(MpOverlay::takeResultsAsk());
    MpOverlay::resultsAskFailed();
    CHECK(MpOverlay::buildResults(&m));
    CHECK(!(m.rows[5].marks & MpPanel::MARK_DISABLED) && U8(m.status) == "Mario64 is choosing...");
    frames(1, true, false, false, false);
    CHECK(MpOverlay::takeResultsAsk());
    frames(1, true, false, false, false);                                  // (the cursor went to Leave Race after the ask: this press leaves)
    CHECK(!MpOverlay::takeResultsAsk() && MpOverlay::takeResultsLeave());
    // series card auto-advance cannot loop for nobody: Race Again twice by itself, the third idle expiry ends the series (Leave Race)
    fake::host = true; fake::local = 0;
    for (bool& p : fake::present) p = true;
    MpOverlay::dismissResults();
    MpOverlay::forgetSeriesNames();                                        // (a new series: the count of automatic picks starts over)
    for (int round = 0; round < 3; round++) {
        CHECK(MpOverlay::showResults(msg));
        frames(90 * 60 + 5);
        int w = 0;
        CHECK(MpOverlay::takeResultsChoice(&w) && w == (round < 2 ? CH_AGAIN : CH_LEAVE));
        MpOverlay::dismissResults();
    }
    // ...a press, a finger or a guest's ask in between starts the count over
    for (int how = 0; how < 3; how++) {
        MpOverlay::forgetSeriesNames();
        for (int round = 0; round < 2; round++) {
            CHECK(MpOverlay::showResults(msg));
            frames(90 * 60 + 5);
            int w = 0;
            CHECK(MpOverlay::takeResultsChoice(&w) && w == CH_AGAIN);
            MpOverlay::dismissResults();
        }
        CHECK(MpOverlay::showResults(msg));
        frames(40);
        if (how == 0) frames(1, false, false, true);
        else if (how == 1) MpOverlay::resultsPoint(5);
        else MpOverlay::noteRaceAgainAsk(1);
        frames(90 * 60 - 100);
        int w = 0;
        CHECK(!MpOverlay::takeResultsChoice(&w));                          // (the 90 s start over from the press)
        frames(200);
        CHECK(MpOverlay::takeResultsChoice(&w) && w == CH_AGAIN);          // (and it is Race Again again, not the end)
        MpOverlay::dismissResults();
    }
    MpOverlay::forgetSeriesNames();
    {   // (the names of every slot are known again for what follows, as at the top of the test)
        MpOverlay::dismissResults();
        const MpMsg first = results(1, (const int[]){3}, (const uint32_t[]){2500}, (const int[]){1}, nullptr);
        CHECK(MpOverlay::showResults(first));
        MpOverlay::dismissResults();
        fake::host = false; fake::local = 1;
        fake::present[3] = false;
        CHECK(MpOverlay::showResults(msg));
    }
    // the carrier: the ask is the existing quick phrase 3, nothing new on the wire
    CHECK(isAgainAsk(0, 3) && !isAgainAsk(0, 4) && !isAgainAsk(1, 3) && kAskPhrase < MP_CHAT_PHRASES);
    CHECK(asksAsQuestion(0, 3, true) && !asksAsQuestion(0, 3, false) && !asksAsQuestion(0, 4, true) && !asksAsQuestion(1, 3, true));   // menus4: the bubble reads "Race Again?" only on a card
    fake::host = false; fake::local = 1;
    CHECK(MpOverlay::showResults(msg));
    // a player who left while the card is up shows greyed at once (no place, no time), the leaver's line rule
    fake::present[0] = false;
    CHECK(MpOverlay::buildResults(&m) && (m.rows[2].marks & MpPanel::MARK_DISABLED) && m.rows[2].lead[0] == u'-' && U8(m.rows[2].label) == "Mario64");
    CHECK(!(m.rows[1].marks & MpPanel::MARK_DISABLED) && !(m.rows[3].marks & MpPanel::MARK_DISABLED));   // (Ros and the local player are as they were)
    fake::present[0] = true;
    // (after the leader's choice the guest sees it, as before)
    CHECK(MpOverlay::showResults(msg));
    MpOverlay::setResultsChoice(CH_LEAVE);
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 7 && m.status[0] == 0 && !(m.rows[7].marks & MpPanel::MARK_DISABLED));
    CHECK(m.rows[3].marks & MpPanel::MARK_YOU);                            // (Luigi64 is the local player)
    frames(60 * 20, false, false, false, false);
    CHECK(MpOverlay::resultsShowing());                                    // (still no timer for a guest)

    // no next star: the row is not there
    fake::host = true; fake::local = 0;
    t.next = 0;
    CHECK(MpOverlay::showResults(results(3, slots, turns, places, &t)));
    CHECK(MpOverlay::buildResults(&m) && m.rowCount == 7 && U8(m.rows[6].label) == "Leave Race");
    frames(40, false, false, true);
    frames(1, false, false, true);
    frames(1, false, false, true);
    CHECK(MpOverlay::buildResults(&m) && m.cursor == 6);                   // (two choices: the cursor stops on the last)
    MpOverlay::dismissResults();

    // an old-style card (no tail) is the old one: Continue, closes by itself after 8 s
    CHECK(MpOverlay::showResults(results(3, slots, turns, places, nullptr)));
    CHECK(!MpOverlay::resultsHaveChoices() && MpOverlay::buildResults(&m) && m.rowCount == 3 && m.buttonCount == 1 && m.cursor == -1);
    frames(8 * 60 + 2);
    CHECK(!MpOverlay::resultsShowing());
}

static void testGeometry() {
    // every language, the Vita's 16:9 and a 4:3 picture: the card (4 players + 3 choices, the most rows) and the head start line compose
    // without overflow and stay inside the picture
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Model m;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    const char* langs[] = {"UsEnglish", "UsFrench", "UsSpanish", "EuGerman", "EuItalian"};
    for (float width : {810.6667f, 608.0f})
    for (const char* l : langs)
    for (int variant = 0; variant < 3; variant++) {
        MpMsgText::setLanguage(MpMsgText::languageIndex(l));
        MpOverlay::reset();
        for (bool& p : fake::present) p = true;
        fake::host = variant != 1; fake::local = 0;
        Tail t;
        t.series.in = 0xF;
        t.series.left = variant == 2 ? 0x8 : 0;
        for (int i = 0; i < 4; i++) t.series.pts[i] = (uint8_t)(variant == 2 ? 99 - i : 9 - i);
        t.next = 3;
        const int slots[4] = {1, 0, 2, 3};
        const uint32_t turns[4] = {5999 * 60 + 59, 61234, 71234, MP_RACE_DNF};
        const int places[4] = {1, 2, 3, 0};
        const int count = variant == 2 ? 3 : 4;
        if (variant == 2) fake::present[3] = false;
        MpOverlay::showResults(results(count, slots, turns, places, &t));
        MpOverlay::buildResults(&m);
        if (variant == 2) CHECK(m.rowCount <= MpPanel::kMaxRows);
        frames(40);
        MpPanel::Screen scr;
        scr.width = width;
        MpOverlay::buildResults(&m);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(m, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow && !plan.textClipped);
        CHECK(plan.x0 >= 0 && plan.x1 <= width && plan.y0 >= 0 && plan.y1 <= scr.height);
        for (int i = 0; i < scene.useCount; i++) CHECK(scene.uses[i].x > -304 && scene.uses[i].x < 304);
        {   // lobby: the series card is in the file select's look: no glove, and every choice this player can press is a big row (>= 67 units)
            CHECK(m.look == MpPanel::LOOK_GAME);
            for (int i = 0; i < scene.useCount; i++) CHECK(scene.uses[i].kind != MpPanel::USE_POINTER);
            int choices = 0;
            for (int i = 0; i < m.rowCount; i++) {
                if (!(m.rows[i].marks & MpPanel::MARK_CHOICE)) continue;
                ++choices;
                CHECK(i < plan.hitCount && plan.hits[i].y1 - plan.hits[i].y0 >= 66.9f);
            }
            CHECK(choices >= 1);
            if (fake::host) CHECK(choices == 3);
        }
        // the head start line
        MpOverlay::dismissResults();
        MpOverlay::showHeadStart(1, 6);
        CHECK(MpOverlay::headStartShowing() && MpOverlay::buildHeadStart(&m));
        CHECK(m.rowCount == 1 && U8(m.rows[0].label) == "Luigi64" && m.lift < 0);
        const std::string line = U8(m.status);
        CHECK(line.find('6') != std::string::npos);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(m, scr, fm, 0.25f, &scene, &plan);
        CHECK(!plan.overflow && !plan.textClipped);
        CHECK(plan.x0 >= 0 && plan.x1 <= width && plan.y0 >= 0 && plan.y1 <= scr.height);
        // low on the picture, clear of the middle where the countdown's numbers are
        CHECK(plan.y0 > scr.height * 0.5f + 20.0f);
        MpOverlay::showHeadStart(0, 0);
        CHECK(!MpOverlay::headStartShowing() && !MpOverlay::buildHeadStart(&m));
    }
}

// B6: a leader who is connected but never chooses (the console left on, Wi-Fi up) must not hold every guest on the card forever. The leader's own
// card picks by itself after 90 s (uxfix #9); a guest whose card still has no choice after 120 s leaves the way the Leave Race row does, and says so.
static void testIdleLeave() {
    static MpPanel::Model m;
    MpMsgText::setLanguage(MpMsgText::languageIndex("UsEnglish"));   // (the geometry test before this one walks every language)
    MpOverlay::reset();
    for (bool& p : fake::present) p = true;
    Tail t;
    t.series.in = 0x3; t.series.pts[0] = 3; t.series.pts[1] = 1;
    t.next = 2;
    const int slots[2] = {0, 1};
    const uint32_t turns[2] = {3000, 3300};
    const int places[2] = {1, 2};
    fake::host = false; fake::local = 1; fake::hostSlot = 0;
    CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
    int c = 0;
    frames(119 * 60, false, false, false, false);
    CHECK(MpOverlay::resultsShowing() && !MpOverlay::takeResultsLeave() && MpOverlay::toastCount() == 0);   // (generous: still waiting at 119 s)
    frames(2 * 60, false, false, false, false);
    CHECK(MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsChoice(&c));   // the existing leave, once
    CHECK(MpOverlay::toastCount() == 1 && U8(MpOverlay::toasts()[0].text) == "No one chose, so you left the race.");
    frames(5 * 60, false, false, false, false);
    CHECK(!MpOverlay::takeResultsLeave());                                  // (never a second leave from the same card)
    // a leader who chose in time: nothing happens to the guest
    MpOverlay::dismissResults();
    MpOverlay::reset();
    t.choice = CH_AGAIN;
    CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
    frames(300 * 60, false, false, false, false);
    CHECK(!MpOverlay::takeResultsLeave() && MpOverlay::toastCount() == 0);
    // the leader's own card is not the guest's rule: it chooses by itself at 90 s, never "leaves" through this
    MpOverlay::dismissResults();
    MpOverlay::reset();
    fake::host = true; fake::local = 0;
    t.choice = CH_NONE;
    CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
    frames(121 * 60, false, false, false, true);
    CHECK(!MpOverlay::takeResultsLeave() && MpOverlay::takeResultsChoice(&c) && c == CH_AGAIN);
    // B6 (review): the waits are wall-clock time, not frames
    {   // a stage at 20 fps (50 ms a frame): 120 s are 2400 frames, not 7200
        MpOverlay::dismissResults();
        MpOverlay::reset();
        fake::host = false; fake::local = 1; fake::hostSlot = 0;
        t.choice = CH_NONE;
        CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
        frames(118 * 20, false, false, false, false, false, 150);
        CHECK(!MpOverlay::takeResultsLeave());
        frames(3 * 20, false, false, false, false, false, 150);
        CHECK(MpOverlay::takeResultsLeave());
    }
    {   // a host change restarts the guest's count: the new leader gets its own 120 s
        MpOverlay::dismissResults();
        MpOverlay::reset();
        fake::host = false; fake::local = 2; fake::hostSlot = 0;
        CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
        frames(100 * 60, false, false, false, false);
        fake::hostSlot = 1;   // (host migration)
        frames(100 * 60, false, false, false, false);
        CHECK(!MpOverlay::takeResultsLeave());   // 200 s in all, 100 s under this host
        frames(21 * 60, false, false, false, false);
        CHECK(MpOverlay::takeResultsLeave());
        fake::hostSlot = 0;
    }
    {   // nothing counts while the session is being rejoined; the count goes on afterwards
        MpOverlay::dismissResults();
        MpOverlay::reset();
        fake::host = false; fake::local = 1; fake::hostSlot = 0;
        CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
        frames(100 * 60, false, false, false, false);
        frames(300 * 60, false, false, false, false, true);   // (rejoining for five minutes)
        CHECK(!MpOverlay::takeResultsLeave());
        frames(19 * 60, false, false, false, false);
        CHECK(!MpOverlay::takeResultsLeave());
        frames(2 * 60, false, false, false, false);
        CHECK(MpOverlay::takeResultsLeave());
    }
    {   // the leader's own wait is wall-clock too (20 fps: still 90 s), and stands still while rejoining
        MpOverlay::dismissResults();
        MpOverlay::reset();
        fake::host = true; fake::local = 0; fake::hostSlot = 0;
        CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
        frames(60 * 60, false, false, false, true);
        frames(200 * 60, false, false, false, true, true);
        CHECK(!MpOverlay::takeResultsChoice(&c));
        frames(29 * 60, false, false, false, true);
        CHECK(!MpOverlay::takeResultsChoice(&c));
        frames(2 * 60, false, false, false, true);
        CHECK(MpOverlay::takeResultsChoice(&c) && c == CH_AGAIN);
        MpOverlay::dismissResults();
        MpOverlay::reset();
        CHECK(MpOverlay::showResults(results(2, slots, turns, places, &t)));
        frames(88 * 20, false, false, false, true, false, 150);
        CHECK(!MpOverlay::takeResultsChoice(&c));
        frames(3 * 20, false, false, false, true, false, 150);
        CHECK(MpOverlay::takeResultsChoice(&c) && c == CH_AGAIN);
    }
    // the two waits keep their order whatever is tuned later: the guest never leaves before the leader's card has chosen
    static_assert(kCardIdleGuestMs > kCardIdleHostMs, "guests wait longer than the leader's own card");
    (void)m;
}

int main() {
    MpMsgText::setLanguage(MpMsgText::languageIndex("UsEnglish"));
    testCard();
    testGeometry();
    testIdleLeave();
    if (gFails) { fprintf(stderr, "race_card_test: %d FAILED\n", gFails); return 1; }
    printf("race_card_test: ok\n");
    return 0;
}
