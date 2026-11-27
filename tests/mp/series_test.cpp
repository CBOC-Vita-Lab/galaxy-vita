// Host tests for decomp/include/Game/MP/MpSeriesLogic.hpp and its use in MpFlowLogic.hpp (race: series, catch-up head start, the
// results card's next step, timing). Build: sh tests/mp/series_build.sh (run from the repo root).
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpSeriesLogic.hpp"
#include "Game/MP/MpUi.hpp"

#include <cstdio>
#include <cstring>
#include <random>

using namespace MpSeriesLogic;

static int sFail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            sFail++;                                                     \
        }                                                                \
    } while (0)

// ---- points -------------------------------------------------------------------------------------------------------------------------
static void testPoints() {
    // 4 players 3-2-1-0, 3 players 2-1-0, 2 players 1-0, one player nothing; no place = 0
    const int p4[4] = {3, 2, 1, 0}, p3[3] = {2, 1, 0}, p2[2] = {1, 0};
    for (int i = 0; i < 4; i++) CHECK(pointsForPlace(i + 1, 4) == p4[i]);
    for (int i = 0; i < 3; i++) CHECK(pointsForPlace(i + 1, 3) == p3[i]);
    for (int i = 0; i < 2; i++) CHECK(pointsForPlace(i + 1, 2) == p2[i]);
    CHECK(pointsForPlace(1, 1) == 0 && pointsForPlace(0, 4) == 0 && pointsForPlace(5, 4) == 0 && pointsForPlace(-1, 4) == 0);

    // a race: slots 0..3 placed 1,2,2,4 (a tie for second shares the place and its points)
    Series s;
    s.syncPresence(0xF);
    const Placing a[4] = {{0, 1}, {1, 2}, {2, 2}, {3, 4}};
    s.applyRace(a, 4, true);
    CHECK(s.pts[0] == 3 && s.pts[1] == 2 && s.pts[2] == 2 && s.pts[3] == 0);
    // a non-finisher (place 0) gets nothing; the others are still scored among n = the players of the race
    Series t;
    t.syncPresence(0x7);
    const Placing b[3] = {{2, 1}, {0, 2}, {1, 0}};
    t.applyRace(b, 3, true);
    CHECK(t.pts[2] == 2 && t.pts[0] == 1 && t.pts[1] == 0);
    // two players, both finished
    Series u;
    u.syncPresence(0x5);
    const Placing c[2] = {{2, 1}, {0, 2}};
    u.applyRace(c, 2, true);
    CHECK(u.pts[2] == 1 && u.pts[0] == 0);
    // nobody finished: all places 0, nobody scores, no streak
    Series v;
    v.syncPresence(0x3);
    const Placing d[2] = {{0, 0}, {1, 0}};
    v.applyRace(d, 2, true);
    CHECK(v.pts[0] == 0 && v.pts[1] == 0 && v.streakLen == 0);
    // points are capped, and accumulate over races
    Series w;
    w.syncPresence(0x3);
    const Placing e[2] = {{0, 1}, {1, 2}};
    for (int i = 0; i < 120; i++) w.applyRace(e, 2, true);
    CHECK(w.pts[0] == kMaxPoints && w.pts[1] == 0);
    CHECK(blockValid(w));
}

// ---- streak and head start -----------------------------------------------------------------------------------------------------------
static void win(Series& s, int slot, int n, bool catchUp = true) {
    Placing p[4];
    int k = 0;
    p[k++] = Placing{(uint8_t)slot, 1};
    for (int i = 0; i < n; i++) {
        if (i == slot) continue;
        p[k] = Placing{(uint8_t)i, (uint8_t)(k + 1)};
        k++;
    }
    s.applyRace(p, n, catchUp);
}
static void winMask(Series& s, int slot, unsigned mask, bool catchUp = true) {   // the players of the race = the slots of `mask`
    Placing p[4];
    int k = 0;
    p[k++] = Placing{(uint8_t)slot, 1};
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1u << i)) || i == slot) continue;
        p[k] = Placing{(uint8_t)i, (uint8_t)(k + 1)};
        k++;
    }
    s.applyRace(p, k, catchUp);
}
static void testHeadStart() {
    Series s;
    s.syncPresence(0xF);
    CHECK(s.headStartSec(0, true, 0xF) == 0);   // before any race: nobody
    win(s, 2, 4);
    CHECK(s.headStartSec(2, true, 0xF) == 3 && s.headStartSec(0, true, 0xF) == 0);   // one win: 3 s
    win(s, 2, 4);
    CHECK(s.headStartSec(2, true, 0xF) == 6);    // two in a row: 6 s
    win(s, 2, 4);
    CHECK(s.headStartSec(2, true, 0xF) == 9);    // three: 9 s
    win(s, 2, 4);
    CHECK(s.headStartSec(2, true, 0xF) == 9 && s.streakLen == 3);   // capped
    win(s, 1, 4);                                 // someone else wins: it starts over with them
    CHECK(s.headStartSec(2, true, 0xF) == 0 && s.headStartSec(1, true, 0xF) == 3);
    win(s, 2, 4);
    CHECK(s.headStartSec(2, true, 0xF) == 3 && s.headStartSec(1, true, 0xF) == 0);   // (the streak of 1 was broken by 2's win)

    // catch-up Off: no streak starts or goes on, and a switch to Off during a series stops the head start at once
    Series o;
    o.syncPresence(0x3);
    win(o, 0, 2, false);
    CHECK(o.streakLen == 0 && o.headStartSec(0, false, 0x3) == 0);
    Series p;
    p.syncPresence(0x3);
    win(p, 0, 2);
    CHECK(p.headStartSec(0, true, 0x3) == 3 && p.headStartSec(0, false, 0x3) == 0);
    CHECK(p.goBlock(false, 0x3).streakLen == 0 && p.goBlock(true, 0x3).streakLen == 1);
    CHECK(headStartFrames(p.goBlock(true, 0x3), 0) == 180 && headStartFrames(p.goBlock(true, 0x3), 1) == 0);
    CHECK(headStartFrames(p.goBlock(false, 0x3), 0) == 0);
    win(p, 0, 2, false);   // a race played with Off clears a standing streak
    CHECK(p.streakLen == 0);

    // a tie for first has no sole winner: the streak ends
    Series t;
    t.syncPresence(0xF);
    win(t, 3, 4);
    const Placing tie[4] = {{0, 1}, {3, 1}, {1, 3}, {2, 4}};
    t.applyRace(tie, 4, true);
    CHECK(t.streakLen == 0);

    // the winner is alone in the room (the others left): no head start to give
    Series l;
    l.syncPresence(0x3);
    win(l, 0, 2);
    CHECK(l.headStartSec(0, true, 0x1) == 0);
    // the streak holder leaves: the streak ends (and does not come back with a new player in the slot)
    l.syncPresence(0x2);
    CHECK(l.streakLen == 0 && (l.left & 1));
    l.syncPresence(0x3);
    CHECK(l.streakLen == 0 && l.pts[0] == 0);
    // a one-player race has no winner to handicap
    Series one;
    one.syncPresence(0x1);
    const Placing solo[1] = {{0, 1}};
    one.applyRace(solo, 1, true);
    CHECK(one.streakLen == 0 && one.pts[0] == 0);
}

// ---- joins and leaves ---------------------------------------------------------------------------------------------------------------
static void testJoinLeave() {
    Series s;
    s.syncPresence(0x7);
    win(s, 0, 3);
    win(s, 0, 3);
    CHECK(s.pts[0] == 4 && s.pts[1] == 2 && s.pts[2] == 0);   // (win() lists the others in slot order: 1 second, 2 third)
    // slot 3 joins mid-series: starts at 0, no head start for anyone
    s.syncPresence(0xF);
    CHECK((s.in & 8) && s.pts[3] == 0 && !(s.left));
    CHECK(s.streakLen == 2 && s.streakSlot == 0);   // a join does not break the streak
    // slot 1 leaves: its line stays, greyed, with its points
    s.syncPresence(0xD);
    CHECK((s.in & 2) && (s.left & 2) && s.pts[1] == 2);
    // the next race counts the players present: 3 players, 2-1-0
    Placing p[3] = {{2, 1}, {0, 2}, {3, 3}};
    s.applyRace(p, 3, true);
    CHECK(s.pts[2] == 2 && s.pts[0] == 5 && s.pts[3] == 0 && s.pts[1] == 2);
    CHECK(s.streakLen == 1 && s.streakSlot == 2);
    // a new player takes slot 1: a fresh line at 0, no longer greyed
    s.syncPresence(0xF);
    CHECK(!(s.left & 2) && s.pts[1] == 0);
    // everyone gone: every line greyed, nothing is lost until the series is reset
    s.syncPresence(0);
    CHECK(s.left == s.in && s.pts[0] == 5 && s.streakLen == 0);
    s.reset();
    CHECK(s.in == 0 && s.left == 0 && s.pts[0] == 0 && s.streakLen == 0);
    CHECK(blockValid(s));
}

// ---- migration: serialise, restore on another instance, continue ----------------------------------------------------------
static void testMigration() {
    Series host;   // the original host's state
    host.syncPresence(0xF);
    win(host, 1, 4);
    win(host, 1, 4);
    host.syncPresence(0xB);   // slot 2 left
    winMask(host, 3, 0xB);    // (3 players)

    // every console keeps the block it last got from the host (GO or RESULTS): through the wire
    uint8_t buf[64];
    MpWriter w(buf, sizeof buf);
    writeBlock(w, host);
    CHECK(w.ok() && w.n == (size_t)kBlockBytes);
    Series heir;
    MpReader r(buf, w.n);
    Block got;
    CHECK(readBlock(r, &got) && r.done() && heir.restore(got));
    CHECK(heir.block() == host.block());

    // both continue the same way: the heir (the new host) and a host that never left
    for (int i = 0; i < 3; i++) {
        host.syncPresence(0xB);
        heir.syncPresence(0xB);
        winMask(host, 0, 0xB);
        winMask(heir, 0, 0xB);
        CHECK(heir.block() == host.block());
        CHECK(heir.headStartSec(0, true, 0xB) == host.headStartSec(0, true, 0xB));
    }
    CHECK(heir.streakLen == 3 && heir.pts[2] == 2 && (heir.left & 4));   // the leaver's line is still there, greyed

    // through RESULTS (the tail) and GO as well, as the game sends them
    MpFlowLogic::Results res;
    res.serial = 7;
    res.count = 3;
    res.e[0] = {0, 3000, 1}; res.e[1] = {1, 3100, 2}; res.e[2] = {3, MpFlowLogic::kDnf, 0};
    res.hasTail = true;
    res.tail.series = host.block();
    res.tail.choice = CH_AGAIN;
    res.tail.next = 3;
    uint8_t rb[128];
    MpWriter rw(rb, sizeof rb);
    MpFlowLogic::writeResults(rw, res);
    CHECK(rw.ok() && rw.n == 3 + 3 * 6 + kTailBytes);
    MpFlowLogic::Results back;
    CHECK(MpFlowLogic::readResults(rb, rw.n, &back) && back.hasTail && back.tail.choice == CH_AGAIN && back.tail.next == 3 &&
          back.tail.series == host.block());
    MpFlowLogic::Go go;
    go.serial = 9; go.startAtTurn = 1234; go.hasSeries = true; go.series = host.goBlock(true, 0xB);
    uint8_t gb[32];
    MpWriter gw(gb, sizeof gb);
    MpFlowLogic::writeGo(gw, go);
    CHECK(gw.n == (size_t)MpFlowLogic::kGoBytes);
    MpFlowLogic::Go gback;
    CHECK(MpFlowLogic::readGo(gb, gw.n, &gback) && gback.hasSeries && gback.series == go.series && gback.serial == 9);
    // the legacy plain GO and RESULTS (no series) still read
    uint8_t pb[8];
    MpWriter pw(pb, sizeof pb);
    MpFlowLogic::Go g5;
    g5.serial = 5; g5.startAtTurn = 77;
    MpFlowLogic::writeGo(pw, g5);
    CHECK(pw.n == 6 && MpFlowLogic::readGo(pb, pw.n, &gback) && !gback.hasSeries);
}

// ---- wire: sizes, bounds, hostile input -----------------------------------------------------------------------------------------
static void testWire() {
    // everything that is sent fits the event history entry (124 bytes) and the core's per-type maximum (GO 16, RESULTS 64)
    CHECK(MpFlowLogic::kGoBytes <= 16 && 3 + 4 * 6 + kTailBytes <= 64 && 3 + 4 * 6 + kTailBytes <= 124);

    // block: every invalid shape is refused
    Block ok;
    ok.in = 0xF; ok.pts[0] = 5;
    CHECK(blockValid(ok));
    Block b = ok; b.pts[0] = 100; CHECK(!blockValid(b));
    b = ok; b.in = 0x1F; CHECK(!blockValid(b));
    b = ok; b.left = 0x1F; CHECK(!blockValid(b));
    b = ok; b.in = 0x3; b.left = 0x4; CHECK(!blockValid(b));
    b = ok; b.in = 0x1; b.pts[1] = 1; CHECK(!blockValid(b));                 // points on a slot with no line
    b = ok; b.streakLen = 4; CHECK(!blockValid(b));
    b = ok; b.streakLen = 1; b.streakSlot = 4; CHECK(!blockValid(b));
    b = ok; b.in = 0x3; b.streakLen = 1; b.streakSlot = 2; CHECK(!blockValid(b));   // the streak holder has no line
    b = ok; b.left = 0x1; b.streakLen = 1; b.streakSlot = 0; CHECK(!blockValid(b)); // ... or left
    b = ok; b.streakLen = 0; b.streakSlot = 2; CHECK(!blockValid(b));
    // a head start a guest claims for itself is just a display: only the host's messages are taken (host-only types) ...
    CHECK(mp_msg_host_only(MP_MSG_GO) && mp_msg_host_only(MP_MSG_RESULTS) && !mp_msg_host_relays(MP_MSG_GO) && !mp_msg_host_relays(MP_MSG_RESULTS));
    // ... and the series is not changed by a refused block
    Series s;
    s.syncPresence(0x3);
    win(s, 0, 2);
    const Block before = s.block();
    Block bad = ok; bad.pts[0] = 200;
    CHECK(!s.restore(bad) && s.block() == before);

    // truncated and extended reads
    uint8_t buf[40];
    MpWriter w(buf, sizeof buf);
    Tail t;
    t.series.in = 0x3; t.series.pts[0] = 2;
    t.choice = CH_NEXT; t.next = 2;
    writeTail(w, t);
    CHECK(w.n == (size_t)kTailBytes);
    Tail back;
    for (size_t n = 0; n <= w.n; n++) {
        MpReader r(buf, n);
        const bool ok2 = readTail(r, &back);
        CHECK(ok2 == (n == w.n));
    }
    t.choice = CH_COUNT; CHECK(!tailValid(t));
    t.choice = CH_NEXT; t.next = 0; CHECK(!tailValid(t));
    t.choice = CH_NONE; t.next = kMaxNextScenario + 1; CHECK(!tailValid(t));

    // RESULTS: a tail with a result for a slot that has no line, a tail of the wrong length, and extra bytes are all refused
    MpFlowLogic::Results res;
    res.serial = 1; res.count = 2;
    res.e[0] = {0, 100, 1}; res.e[1] = {1, 120, 2};
    res.hasTail = true;
    res.tail.series.in = 0x1;   // slot 1 has no line
    uint8_t rb[64];
    MpWriter rw(rb, sizeof rb);
    MpFlowLogic::writeResults(rw, res);
    MpFlowLogic::Results out;
    CHECK(!MpFlowLogic::readResults(rb, rw.n, &out));
    MpRaceResults ui;
    MpReader ur(rb, rw.n);
    CHECK(!mp_read_results(ur, &ui));
    res.tail.series.in = 0x3;
    MpWriter rw2(rb, sizeof rb);
    MpFlowLogic::writeResults(rw2, res);
    CHECK(MpFlowLogic::readResults(rb, rw2.n, &out));
    { MpReader u2(rb, rw2.n); CHECK(mp_read_results(u2, &ui) && ui.hasTail && ui.tail.series.in == 3); }
    CHECK(!MpFlowLogic::readResults(rb, rw2.n - 1, &out) && !MpFlowLogic::readResults(rb, rw2.n + 3, &out));
    { MpReader u3(rb, rw2.n - 1); CHECK(!mp_read_results(u3, &ui)); }

    // random bytes: whatever is accepted is a valid block / tail (the invariants the game relies on)
    std::mt19937 rng(12345);
    int acceptedBlocks = 0;
    for (int i = 0; i < 400000; i++) {
        uint8_t x[16];
        for (uint8_t& c : x) c = (uint8_t)rng();
        if (i & 1) { x[4] &= 0x0F; x[5] &= 0x0F; if (rng() & 1) for (int k = 0; k < 4; k++) x[k] %= 100; }
        MpReader r(x, kBlockBytes);
        Block g;
        if (readBlock(r, &g)) {
            acceptedBlocks++;
            CHECK(blockValid(g));
            for (int k = 0; k < 4; k++) CHECK(g.pts[k] <= kMaxPoints);
            CHECK(headStartFrames(g, 0) <= 540 && headStartFrames(g, 3) <= 540);
        }
        MpReader r2(x, kTailBytes);
        Tail tt;
        if (readTail(r2, &tt)) CHECK(tailValid(tt));
    }
    CHECK(acceptedBlocks > 100);   // (the fuzz does reach the accepting path)
}

// ---- ranking with a head start, and what the leaderboard gets ----------------------------------------------------------------
static void testRankAndTiming() {
    // slot 2 won the last race: 3 s head start (180 frames). It finishes 50.0 s after ITS start, slot 0 52.0 s after its own (the
    // room's countdown): the room's clock says slot 2 53.0 s, so slot 0 is first.
    MpFlowLogic::RaceBoard bd;
    bd.reset(4);
    bd.headStart[2] = 180;
    CHECK(bd.noteFinish(2, 3000, 1000) && bd.noteFinish(0, 3120, 2000) && bd.noteFinish(1, 3400, 3000));
    MpFlowLogic::Results res;
    bd.rank(0x7, &res);
    CHECK(res.count == 3 && res.e[0].slot == 0 && res.e[0].frames == 3120 && res.e[0].place == 1);
    CHECK(res.e[1].slot == 2 && res.e[1].frames == 3180 && res.e[1].place == 2);
    CHECK(res.e[2].slot == 1 && res.e[2].place == 3);
    CHECK(bd.frames[2] == 3000);   // (the board keeps the player's own frames)
    // a tie on the room's clock shares the place
    MpFlowLogic::RaceBoard t;
    t.reset(5);
    t.headStart[1] = 360;
    t.noteFinish(0, 4360, 1); t.noteFinish(1, 4000, 2);
    t.rank(0x3, &res);
    CHECK(res.e[0].place == 1 && res.e[1].place == 1 && res.e[0].frames == res.e[1].frames);
    // the clock never leaves the valid range
    MpFlowLogic::RaceBoard h;
    h.reset(6);
    h.headStart[0] = 540;
    h.noteFinish(0, MpFlowLogic::kMaxRaceFrames - 1, 1);
    h.rank(0x1, &res);
    CHECK(res.e[0].frames < MpFlowLogic::kMaxRaceFrames);
    uint8_t rb[64];
    MpWriter rw(rb, sizeof rb);
    MpFlowLogic::writeResults(rw, res);
    CHECK(MpFlowLogic::readResults(rb, rw.n, &res));

    // timing: the racing console counts its own turns from its own release. For every head start the leaderboard gets exactly the
    // turns raced (never less: a head start cannot make a time better than the truth) and the room's clock is the sum.
    for (uint32_t head : {0u, 180u, 360u, 540u}) {
        StartGate g;
        g.begin(head);
        uint32_t turn = 0, releaseTurn = 0;
        bool released = false;
        if (head == 0) released = true;   // (released at once: the game takes the release at begin())
        const uint32_t finishAfter = 2500;   // turns of the countdown's end (room clock) when the star is touched
        for (; turn < finishAfter + head; turn++) {
            const bool r = g.tick();
            if (r && !released) released = true;
            if (g.racing && releaseTurn == 0 && g.frames == 1) releaseTurn = turn;
        }
        CHECK(released && g.racing);
        CHECK(releaseTurn == head);   // the first counted turn is `head` turns after the room's
        CHECK(g.ownFrames() == finishAfter && leaderboardFrames(g) == g.ownFrames());
        CHECK(g.roomFrames() == finishAfter + head);
        // finishing on the same turn of the room's clock: the handicapped player's own time is never below the truth
        CHECK(leaderboardFrames(g) == (turn - releaseTurn));
    }
    // held the whole head start: nothing counts while waiting
    StartGate w;
    w.begin(300);
    for (int i = 0; i < 300; i++) { CHECK(!w.tick()); CHECK(w.frames == 0 && !w.racing); }
    CHECK(w.tick() && w.racing && w.frames == 1);
}

static void testNextStar() {
    CHECK(nextScenario(1, 3, 0x1) == 2 && nextScenario(2, 3, 0x3) == 3 && nextScenario(3, 3, 0x7) == 0);   // none after the last
    CHECK(nextScenario(1, 3, 0) == 2);                // (the star just played counts as collected)
    CHECK(nextScenario(2, 3, 0x0) == 0);              // star 1 never collected: star 3 is not offered by the star select
    CHECK(nextScenario(0, 3, 0) == 0 && nextScenario(1, 0, 0) == 0 && nextScenario(5, 3, 0xFF) == 0 && nextScenario(1, 1, 1) == 0);
}

int main() {
    testPoints();
    testHeadStart();
    testJoinLeave();
    testMigration();
    testWire();
    testRankAndTiming();
    testNextStar();
    if (sFail) { std::printf("series_test: %d FAILED\n", sFail); return 1; }
    std::printf("series_test: ok\n");
    return 0;
}
