// smallfix: the Star Race results card for a player who is not the leader: Leave Race is the one live row; Cross on it takes the player
// out of the race the way the pause screen's Leave does (ended by us: the save offer, never "everyone left"). Shared helpers: common.hpp.
#include "common.hpp"
#include "Game/MP/MpSeriesLogic.hpp"

static MpMsg raceResults(bool withTail) {
    MpMsg m;
    MpRaceResults r = MpRaceResults();
    r.serial = 7;
    r.count = 2;
    r.e[0] = {0, 1, 3000};
    r.e[1] = {1, 2, 3400};
    if (withTail) {
        r.hasTail = true;
        r.tail.series.in = 0x3;
        r.tail.series.pts[0] = 3;
        r.tail.series.pts[1] = 1;
        r.tail.next = 2;
    }
    MpWriter w(m.data, sizeof m.data);
    mp_write_results(w, r);
    m.type = MP_MSG_RESULTS;
    m.fromSlot = 0;
    m.len = (uint16_t)w.n;
    return m;
}
static void cardFrames(int n, bool ok, bool host) {
    for (int i = 0; i < n; i++) {
        MpOverlay::resultsInput(false, false, ok && i == n - 1, host);
        MpOverlay::update(false);
    }
}

void testRaceLeave() {
    static MpPanel::Model m;
    // a guest: only Leave Race is live; Cross on it is one press, fetched once; the leader's choices are never the guest's
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    CHECK(MpOverlay::showResults(raceResults(true)));
    CHECK(MpOverlay::buildResults(&m));
    const int leave = m.rowCount - 1;
    // lobby: Leave Race and Race Again? (the ask) are live, Next Star is not; the cursor starts on the ask
    CHECK(m.rows[leave].marks != 0 && !(m.rows[leave].marks & MpPanel::MARK_DISABLED) && m.cursor == leave - 2);
    CHECK((m.rows[leave - 1].marks & MpPanel::MARK_DISABLED) && !(m.rows[leave - 2].marks & MpPanel::MARK_DISABLED));
    CHECK(m.buttonCount == 1 && m.status[0] != 0);   // the Cross hint, and "<name> is choosing..."
    CHECK(MpOverlay::resultsPoint(leave) && !MpOverlay::resultsPoint(leave - 1) && !MpOverlay::resultsPoint(0) && !MpOverlay::resultsPoint(99));
    CHECK(MpOverlay::resultsPoint(leave - 2) && MpOverlay::buildResults(&m) && m.cursor == leave - 2);
    cardFrames(10, true, false);   // (the first half second swallows the press that ended the star wipe)
    CHECK(!MpOverlay::takeResultsLeave());
    cardFrames(40, false, false);
    CHECK(!MpOverlay::takeResultsLeave());   // (no timer: nothing happens by itself)
    MpOverlay::resultsInput(false, true, false, false);   // Down: Leave Race
    MpOverlay::update(false);
    cardFrames(1, true, false);
    CHECK(MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsLeave() && !MpOverlay::takeResultsAsk());
    int c;
    CHECK(!MpOverlay::takeResultsChoice(&c));   // (a guest never makes the leader's choice)
    cardFrames(1, true, false);
    CHECK(!MpOverlay::takeResultsLeave());      // (pressed once)
    // the leave itself: as the pause screen's Leave (ended by us), not as "everyone left"
    const int leaves = fake::leaves;
    S().leaveByUs();
    CHECK(fake::leaves == leaves + 1);
    frames(6);
    CHECK(!Logged("ui: window MP_EVERYONE_LEFT") && Logged("ui: window MP_SAVE_ASK (yes/no)"));

    // the leader's own card is unchanged: Leave is a choice among three, and the guest's press does not exist for it
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    CHECK(MpOverlay::showResults(raceResults(true)));
    cardFrames(40, false, true);
    CHECK(MpOverlay::buildResults(&m) && !(m.rows[m.rowCount - 3].marks & MpPanel::MARK_DISABLED) && m.cursor == m.rowCount - 3);
    CHECK(MpOverlay::resultsPoint(m.rowCount - 1) && MpOverlay::buildResults(&m) && m.cursor == m.rowCount - 1);   // (a finger on Leave Race: the cursor follows)
    CHECK(!MpOverlay::resultsPoint(0));
    cardFrames(1, false, true);
    CHECK(MpOverlay::buildResults(&m));
    for (int i = 0; i < 3; i++) { MpOverlay::resultsInput(false, false, false, true); }
    CHECK(MpOverlay::resultsPoint(m.rowCount - 3) && MpOverlay::buildResults(&m) && m.cursor == m.rowCount - 3);
    cardFrames(1, true, true);
    CHECK(!MpOverlay::takeResultsLeave());
    CHECK(MpOverlay::takeResultsChoice(&c));

    // a card without a series (old style): nothing to leave
    fresh();
    session(false, 1, MP_UI_PLAYING);
    CHECK(MpOverlay::showResults(raceResults(false)));
    cardFrames(60, true, false);
    CHECK(!MpOverlay::takeResultsLeave());
}

// menus4 item 5: the rematch ask rides quick phrase 3 ("Let's go!") and nothing else tells it apart on the wire. So: on a guest's own card a
// hand-sent "Let's go!" IS the ask (the card shows "Asked!" and the row is spent, one ask per card); everyone whose card is up reads the
// phrase as "Race Again?" (bubble and toast), everyone else as "Let's go!"; a phrase that is not 3, or no card, is never the ask.
void testRematchAskByHand() {
    static MpPanel::Model m;
    // a guest's card, nothing asked yet: the hand-sent phrase counts, once, and nothing is sent twice
    fresh();
    session(false, 1, MP_UI_PLAYING);
    frames(3);
    CHECK(!MpOverlay::noteAskSentByHand());                 // (no card: not an ask)
    CHECK(MpOverlay::showResults(raceResults(true)));
    cardFrames(40, false, false);
    CHECK(MpOverlay::buildResults(&m));
    const int ask = m.rowCount - 3;
    CHECK(!(m.rows[ask].marks & MpPanel::MARK_DISABLED));
    CHECK(MpOverlay::noteAskSentByHand());                  // the guest used the quick-phrase wheel: this is the ask
    CHECK(MpOverlay::buildResults(&m) && (m.rows[ask].marks & MpPanel::MARK_DISABLED) && U8(m.status).find("Asked") == 0);
    CHECK(!MpOverlay::takeResultsAsk());                    // (the phrase has gone already: the card sends nothing more)
    CHECK(!MpOverlay::noteAskSentByHand());                 // (one ask per card)
    cardFrames(5, true, false);
    CHECK(!MpOverlay::takeResultsAsk());
    // the leader: a hand-sent phrase is just a phrase
    fresh();
    session(true, 0, MP_UI_PLAYING);
    frames(3);
    CHECK(MpOverlay::showResults(raceResults(true)));
    cardFrames(40, false, true);
    CHECK(!MpOverlay::noteAskSentByHand());
    // a card without a series: just a phrase
    fresh();
    session(false, 1, MP_UI_PLAYING);
    CHECK(MpOverlay::showResults(raceResults(false)));
    cardFrames(40, false, false);
    CHECK(!MpOverlay::noteAskSentByHand());
    // the words: with a card up the ask reads "Race Again?", otherwise "Let's go!"
    CHECK(MpSeriesLogic::asksAsQuestion(0, 3, true) && !MpSeriesLogic::asksAsQuestion(0, 3, false) && !MpSeriesLogic::asksAsQuestion(0, 4, true) && !MpSeriesLogic::asksAsQuestion(1, 3, true));
}
