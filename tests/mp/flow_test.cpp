// Host unit tests for the pure flow logic (decomp/include/Game/MP/MpFlowLogic.hpp): message codecs and validation (with a fuzz
// pass), the scene start barrier, the skip vote, the star ledger, the Star Race hold / ranking, the race pose digest.
// Build and run from the repository root: sh tests/mp/flow_build.sh
//   (g++ -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=all -g -I decomp/include -I platform/include
//    tests/mp/flow_test.cpp -o /tmp/flow_test; plus a clang++ -Werror compile of the same file)
#include "Game/MP/MpFlowLogic.hpp"
#include "Game/MP/MpWorldGuard.hpp"

#include <stdio.h>
#include <vector>
#include <stdlib.h>

#include <initializer_list>

using namespace MpFlowLogic;

static int sFails = 0, sChecks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        sChecks++;                                                                \
        if (!(c)) {                                                               \
            sFails++;                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                         \
    } while (0)

static unsigned slotMask(std::initializer_list<int> slots) {
    unsigned m = 0;
    for (int s : slots) m |= 1u << s;
    return m;
}

static void testSerial() {
    CHECK(serialNewer(2, 1));
    CHECK(!serialNewer(1, 2));
    CHECK(!serialNewer(5, 5));
    CHECK(serialNewer(1, 65535));      // wrap
    CHECK(!serialNewer(65535, 1));
    CHECK(serialNewer(0x7FFF, 0));
}

static void testWhere() {
    Where w{3, 2, 4, 7, 1, 513, MOVE_STAR_SELECT};
    uint8_t b[16];
    MpWriter wr(b, sizeof b);
    writeWhere(wr, w, true);
    CHECK(wr.ok() && wr.n == kMoveBytes);
    Where r;
    CHECK(readWhere(b, wr.n, &r, true));
    CHECK(r.galaxy == 3 && r.scenario == 2 && r.selected == 4 && r.startId == 7 && r.zone == 1 && r.serial == 513 && r.moveType == 2);
    CHECK(!readWhere(b, wr.n - 1, &r, true));          // short
    CHECK(!readWhere(b, wr.n, &r, false));             // a MOVE is not a WHERE (length)
    b[8] = 5;                                          // reload after a miss is never commanded
    CHECK(!readWhere(b, wr.n, &r, true));
    b[8] = 9;
    CHECK(!readWhere(b, wr.n, &r, true));
    b[8] = 6;
    CHECK(readWhere(b, wr.n, &r, true));
    // startId below -1
    Where bad{0, 1, 0, -2, 0xFF, 1, 0};
    MpWriter w2(b, sizeof b);
    writeWhere(w2, bad, false);
    CHECK(!readWhere(b, w2.n, &r, false));
    // ranges: 40 galaxies, 7 scenarios, 3 zones
    Where ok{39, 7, 7, -1, 0xFF, 1, MOVE_DIRECT};
    CHECK(checkWhereRanges(ok, 40, 7, 3, true));
    Where g = ok; g.galaxy = 40;
    CHECK(!checkWhereRanges(g, 40, 7, 3, true));
    Where s = ok; s.scenario = 8;
    CHECK(!checkWhereRanges(s, 40, 7, 3, true));
    Where z = ok; z.scenario = 0;                      // a direct move needs a scenario
    CHECK(!checkWhereRanges(z, 40, 7, 3, true));
    z.moveType = MOVE_BACK_TO_DOME;                    // the executor decides
    CHECK(checkWhereRanges(z, 40, 7, 3, true));
    CHECK(!checkWhereRanges(z, 40, 7, 3, false));      // a WHERE always names a scenario
    Where zo = ok; zo.zone = 3;
    CHECK(!checkWhereRanges(zo, 40, 7, 3, true));
    zo.zone = 2;
    CHECK(checkWhereRanges(zo, 40, 7, 3, true));
    Where sel = ok; sel.selected = 9;
    CHECK(!checkWhereRanges(sel, 40, 7, 3, true));
    CHECK(!checkWhereRanges(ok, 0, 7, 3, true));
}

// fix2: every galaxy with hidden scenarios (hidden stars, green stars) in the real data: rows of ScenarioData.bcsv (ScenarioNo
// runs 1..rows) and the visible ones (ScenarioData::getScenarioNum()). tests/mp/scenario_data_check.py compares this table
// with /data/cooked/files/StageData/*/*Scenario.arc.
struct HiddenScenarios { const char* galaxy; int rows, visible; };
static const HiddenScenarios kHiddenScenarios[] = {
    // scenario-data-table-begin
    {"BattleShipGalaxy", 7, 5},
    {"CannonFleetGalaxy", 6, 5},
    {"CosmosGardenGalaxy", 6, 5},
    {"EggStarGalaxy", 6, 5},
    {"FactoryGalaxy", 6, 5},
    {"HeavenlyBeachGalaxy", 6, 5},
    {"HellProminenceGalaxy", 6, 5},
    {"HoneyBeeKingdomGalaxy", 6, 5},
    {"IceVolcanoGalaxy", 6, 5},
    {"OceanFloaterLandGalaxy", 2, 1},
    {"OceanPhantomCaveGalaxy", 6, 5},
    {"OceanRingGalaxy", 6, 5},
    {"PhantomGalaxy", 6, 5},
    {"ReverseKingdomGalaxy", 6, 5},
    {"SandClockGalaxy", 7, 5},
    {"StarDustGalaxy", 6, 5},
    // scenario-data-table-end
};

// A direct move / WHERE / star-select choice to any scenario of the galaxy, hidden and green stars included, passes with the
// row count (MpFlow scenarioMax); the visible count (the old check) refused them.
static void testHiddenScenarioMoves() {
    for (const HiddenScenarios& h : kHiddenScenarios) {
        for (int sc = 1; sc <= h.rows; sc++) {
            Where w{3, (uint8_t)sc, (uint8_t)sc, -1, 0xFF, 7, MOVE_DIRECT};
            uint8_t b[16];
            MpWriter wr(b, sizeof b);
            writeWhere(wr, w, true);
            Where r;
            CHECK(readWhere(b, wr.n, &r, true));
            CHECK(checkWhereRanges(r, 48, h.rows, 4, true));         // MOVE
            CHECK(checkWhereRanges(r, 48, h.rows, 4, false));        // WHERE
            if (sc > h.visible) CHECK(!checkWhereRanges(r, 48, h.visible, 4, true));
        }
        Where over{3, (uint8_t)(h.rows + 1), 1, -1, 0xFF, 7, MOVE_DIRECT};
        CHECK(!checkWhereRanges(over, 48, h.rows, 4, true));
        Where selOver{3, 1, (uint8_t)(h.rows + 1), -1, 0xFF, 7, MOVE_DIRECT};
        CHECK(!checkWhereRanges(selOver, 48, h.rows, 4, true));
    }
    CHECK(sizeof kHiddenScenarios / sizeof kHiddenScenarios[0] == 16);
}

static void testSmallCodecs() {
    uint8_t b[64];
    {
        MpWriter w(b, sizeof b);
        writeStar(w, Star{9, 3, 1, 2, 12345, 0x1234, 2});
        Star s;
        CHECK(w.n == 12 && readStar(b, w.n, &s) && s.serial == 9 && s.starId == 3 && s.grand == 1 && s.bySlot == 2 && s.turns == 12345 && s.galaxyHash == 0x1234 && s.scenario == 2);
        CHECK(!readStar(b, 9, &s));   // proto 4's 9 bytes
        b[9] = 0x80; b[10] = 0x80;   // galaxy hash 0x8080: not 15 bits
        CHECK(!readStar(b, w.n, &s));
        b[9] = 0x34; b[10] = 0x12; b[11] = 0;   // scenario 0
        CHECK(!readStar(b, w.n, &s));
        b[11] = 2;
        CHECK(!readStar(b, w.n + 1, &s));
        b[2] = 0;   // star id 0
        CHECK(!readStar(b, w.n, &s));
        b[2] = 3; b[3] = 2;   // grand = 2
        CHECK(!readStar(b, w.n, &s));
        b[3] = 1; b[4] = 4;   // slot 4
        CHECK(!readStar(b, w.n, &s));
        b[4] = 2; b[2] = 8;   // star id 8: the last scenario the save data holds
        CHECK(readStar(b, w.n, &s));
        b[2] = 9;             // review: 9..64 indexed past mMaxCoinNum[8] on every receiver
        CHECK(!readStar(b, w.n, &s));
    }
    {
        MpWriter w(b, sizeof b);
        writeFinish(w, Finish{4, 3600});
        Finish f;
        CHECK(readFinish(b, w.n, &f) && f.serial == 4 && f.frames == 3600);
        MpWriter w0(b, sizeof b);
        writeFinish(w0, Finish{4, 0});
        CHECK(!readFinish(b, w0.n, &f));
        MpWriter wbig(b, sizeof b);
        writeFinish(wbig, Finish{4, kMaxRaceFrames});
        CHECK(!readFinish(b, wbig.n, &f));
    }
    {
        MpWriter w(b, sizeof b);
        writeSkipVote(w, SkipVoteMsg{7, 0xABCDEF01u});
        SkipVoteMsg v;
        CHECK(readSkipVote(b, w.n, &v) && v.serial == 7 && v.hash == 0xABCDEF01u);
        MpWriter w0(b, sizeof b);
        writeSkipVote(w0, SkipVoteMsg{7, 0});
        CHECK(!readSkipVote(b, w0.n, &v));
    }
    {
        uint8_t t;
        uint8_t one[1] = {3};
        CHECK(readWarpReq(one, 1, &t) && t == 3);
        one[0] = 4;
        CHECK(!readWarpReq(one, 1, &t));
        uint16_t s;
        uint8_t two[2] = {1, 2};
        CHECK(readSerialOnly(two, 2, &s) && s == 0x201);
        CHECK(!readSerialOnly(two, 1, &s));
        MpWriter w(b, sizeof b);
        writeHold(w, Hold{HOLD_RACE_WAIT, 600});
        Hold h;
        CHECK(readHold(b, w.n, &h) && h.reason == HOLD_RACE_WAIT && h.turnsLeft == 600);
        b[0] = 2;
        CHECK(!readHold(b, w.n, &h));
        MpWriter wg(b, sizeof b);
        writeGo(wg, Go{3, 77});
        Go g;
        CHECK(readGo(b, wg.n, &g) && g.serial == 3 && g.startAtTurn == 77);
    }
}

static void testResultsCodec() {
    Results res{12, 3, {{1, 100, 1}, {0, 100, 1}, {2, kDnf, 0}, {0, 0, 0}}};
    uint8_t b[64];
    MpWriter w(b, sizeof b);
    writeResults(w, res);
    CHECK(w.ok() && w.n == 3 + 3 * 6);
    Results r;
    CHECK(readResults(b, w.n, &r) && r.count == 3 && r.e[0].slot == 1 && r.e[2].frames == kDnf && r.e[2].place == 0);
    CHECK(!readResults(b, w.n - 1, &r));
    // duplicate slot
    Results dup{12, 2, {{1, 100, 1}, {1, 200, 2}}};
    MpWriter w2(b, sizeof b);
    writeResults(w2, dup);
    CHECK(!readResults(b, w2.n, &r));
    // DNF with a place / finisher without one
    Results mis{12, 1, {{1, kDnf, 1}}};
    MpWriter w3(b, sizeof b);
    writeResults(w3, mis);
    CHECK(!readResults(b, w3.n, &r));
    Results mis2{12, 1, {{1, 50, 0}}};
    MpWriter w4(b, sizeof b);
    writeResults(w4, mis2);
    CHECK(!readResults(b, w4.n, &r));
    // count over 4
    uint8_t big[3] = {1, 0, 5};
    CHECK(!readResults(big, 3, &r));
    // place beyond count
    Results pl{12, 1, {{1, 50, 2}}};
    MpWriter w5(b, sizeof b);
    writeResults(w5, pl);
    CHECK(!readResults(b, w5.n, &r));
}

// Random bytes into every parser: no crash (ASan/UBSan), and every accepted message re-encodes to the same bytes.
static void testFuzz() {
    srand(1234);
    uint8_t b[40], o[64];
    int accepted = 0;
    for (int it = 0; it < 200000; it++) {
        size_t len = (size_t)(rand() % 32);
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(rand() & 0xFF);
        // bias some towards the exact sizes
        if (it % 3 == 0) len = (size_t)(rand() % 2 ? kMoveBytes : kWhereBytes);
        Where w; Star s; Finish f; Results r; SkipVoteMsg v; Hold h; Go g; uint16_t ser; uint8_t t;
        if (readWhere(b, len, &w, true)) {
            accepted++;
            MpWriter wr(o, sizeof o); writeWhere(wr, w, true);
            CHECK(wr.n == len && memcmp(o, b, len) == 0);
        }
        if (readWhere(b, len, &w, false)) { MpWriter wr(o, sizeof o); writeWhere(wr, w, false); CHECK(wr.n == len && memcmp(o, b, len) == 0); }
        if (readStar(b, len, &s)) { MpWriter wr(o, sizeof o); writeStar(wr, s); CHECK(wr.n == len && memcmp(o, b, len) == 0); }
        if (readFinish(b, len, &f)) { MpWriter wr(o, sizeof o); writeFinish(wr, f); CHECK(wr.n == len && memcmp(o, b, len) == 0); }
        if (readResults(b, len, &r)) {
            MpWriter wr(o, sizeof o); writeResults(wr, r);
            CHECK(wr.n == len && memcmp(o, b, len) == 0);
            CHECK(r.count <= MP_MAX_PLAYERS);
        }
        if (readSkipVote(b, len, &v)) CHECK(v.hash != 0);
        if (readHold(b, len, &h)) CHECK(h.reason <= HOLD_RACE_WAIT);
        (void)readGo(b, len, &g);
        (void)readSerialOnly(b, len, &ser);
        if (readWarpReq(b, len, &t)) CHECK(t < MP_MAX_PLAYERS);
    }
    CHECK(accepted > 0);
}

static void testBarrier() {
    Barrier br;
    const unsigned all = slotMask({0, 1, 2});
    CHECK(br.shouldRelease(all, 0, 20000));            // not armed: never holds
    br.arm(5, 1000);
    br.noteLoaded(0, 5);
    CHECK(!br.shouldRelease(all, 2000, 20000));
    br.noteLoaded(1, 4);                               // an old serial does not count
    CHECK(!br.shouldRelease(all, 2100, 20000));
    br.noteLoaded(1, 5);
    br.noteLoaded(1, 5);                               // repeated LOADED (host migration re-send): idempotent
    CHECK(br.loadedCount(all) == 2);
    CHECK(!br.shouldRelease(all, 2200, 20000));
    br.noteLoaded(2, 6);                               // already ahead (a newer serial) counts
    CHECK(br.shouldRelease(all, 2300, 20000));
    CHECK(br.waitedMs(2300) == 300);
    // a slot that never loads: 20 s from the local ready
    Barrier b2;
    b2.arm(9, 0);
    b2.noteLoaded(0, 9);
    CHECK(!b2.shouldRelease(slotMask({0, 3}), 5000, 20000));
    CHECK(!b2.shouldRelease(slotMask({0, 3}), 24999, 20000));
    CHECK(b2.shouldRelease(slotMask({0, 3}), 25000, 20000));
    // the missing player leaves: release at once
    Barrier b3;
    b3.arm(9, 0);
    b3.noteLoaded(0, 9);
    CHECK(!b3.shouldRelease(slotMask({0, 1}), 100, 20000));
    CHECK(b3.shouldRelease(slotMask({0}), 200, 20000));
    // bad slots ignored
    b3.noteLoaded(-1, 9); b3.noteLoaded(4, 9);
    CHECK(b3.loadedCount(0xF) == 1);
    // wrap: serial 65535 then 1
    Barrier b4;
    b4.arm(1, 0);
    b4.noteLoaded(0, 65535);
    CHECK(!b4.slotLoaded(0));
    b4.noteLoaded(0, 1);
    CHECK(b4.slotLoaded(0));
}

static void testSkipVote() {
    SkipVote v;
    const uint32_t h = 0x1234;
    const unsigned two = slotMask({0, 1});
    CHECK(v.vote(0, 3, h, 0));
    CHECK(!v.vote(0, 3, h, 10));                       // idempotent
    CHECK(!v.decide(3, h, two, 100, 5000));
    CHECK(!v.decide(3, h, two, 6000, 5000));           // 1 of 2 is no majority, even after 5 s
    CHECK(v.vote(1, 3, h, 6100));
    CHECK(v.decide(3, h, two, 6100, 5000));
    CHECK(!v.decide(3, h, two, 6200, 5000));           // once
    CHECK(v.isDone(3, h));
    // 4 players: 3 votes -> majority after 5 s
    SkipVote m;
    const unsigned four = slotMask({0, 1, 2, 3});
    m.vote(0, 1, 77, 0); m.vote(1, 1, 77, 100); m.vote(2, 1, 77, 200);
    CHECK(!m.decide(1, 77, four, 4999, 5000));
    CHECK(m.decide(1, 77, four, 5000, 5000));
    // 2 of 4 is not a strict majority
    SkipVote half;
    half.vote(0, 1, 77, 0); half.vote(1, 1, 77, 0);
    CHECK(!half.decide(1, 77, four, 9000, 5000));
    // a player not in the cutscene is not eligible: the local vote alone decides
    SkipVote solo;
    solo.vote(2, 4, 99, 0);
    CHECK(solo.decide(4, 99, slotMask({2}), 0, 5000));
    // different hash / serial are different cutscenes
    SkipVote d;
    d.vote(0, 1, 5, 0); d.vote(1, 1, 6, 0);
    CHECK(!d.decide(1, 5, two, 0, 5000));
    d.vote(1, 2, 5, 0);
    CHECK(!d.decide(1, 5, two, 0, 5000));
    // bad input
    CHECK(!d.vote(4, 1, 5, 0));
    CHECK(!d.vote(0, 1, 0, 0));
    // more cutscenes than records: the oldest is replaced, no overflow
    SkipVote r;
    for (uint32_t i = 1; i <= 10; i++) r.vote(0, 1, i, (int64_t)i);
    CHECK(r.find(1, 10) != nullptr && r.find(1, 1) == nullptr);
}

static void testLedger() {
    StarLedger l;
    CHECK(l.claim(5, 2, 3, 1, 9));
    CHECK(!l.claim(5, 2, 3, 0, 9));                    // both touched it: one star
    CHECK(l.find(5, 2, 3)->bySlot == 1);
    CHECK(l.claim(5, 3, 3, 0, 10));                    // another scenario
    for (int i = 0; i < 40; i++) l.claim(7, 1, (uint8_t)(i + 1), 0, 11);   // fills and wraps without overflow
    CHECK(l.n == StarLedger::kEntries);
    // gamefix 2: a mission replayed in the same session (another scene serial) is a fresh collection; twice in one serial only once
    StarLedger r;
    CHECK(r.claim(2, 1, 1, 0, 20));
    CHECK(!r.claim(2, 1, 1, 1, 20));
    CHECK(r.claim(2, 1, 1, 0, 21));                    // second visit: delivered again
    CHECK(!r.claim(2, 1, 1, 1, 21));                   // ... and the other console's copy of it is the duplicate
    CHECK(r.n == 1);                                   // renewed, not grown
    CHECK(r.find(2, 1, 1)->serial == 21 && r.claim(2, 1, 1, 0, 22));
}

// gamefix 1: a STAR is real only when its id is in the scenario's PowerStarId mask (real values from StageData/*Scenario.arc)
static void testStarMask() {
    CHECK(starInMask(1, 1) && !starInMask(1, 2));          // scenario 1 of most galaxies: star 1 only
    CHECK(starInMask(2, 2) && !starInMask(2, 1));
    CHECK(starInMask(36, 3) && starInMask(36, 6) && !starInMask(36, 1) && !starInMask(36, 4));   // CosmosGarden 3: stars 3 and 6
    CHECK(starInMask(65, 1) && starInMask(65, 7) && !starInMask(65, 2));                         // BattleShip 1
    CHECK(starInMask(64, 7) && !starInMask(64, 8));
    for (uint8_t id = 0; id < 255; id++) CHECK(!starInMask(0, id));                              // a scenario with no star
    CHECK(!starInMask(0xFFFFFFFFu, 0) && !starInMask(0xFFFFFFFFu, 9) && !starInMask(0xFFFFFFFFu, 255));   // outside 1..8
    // a wire STAR passes readStar (1..8) yet is forged for a scenario whose mask lacks it
    uint8_t b[16];
    MpWriter w(b, sizeof b);
    writeStar(w, Star{3, 5, 0, 1, 100, 0x1234, 3});
    Star st;
    CHECK(readStar(b, w.n, &st) && !starInMask(36, st.starId));
}

static void testRace() {
    RaceBoard b;
    b.reset(4);
    const unsigned three = slotMask({0, 1, 2});
    CHECK(!b.shouldPublish(three, 60000, 0));
    CHECK(b.noteFinish(1, 3000, 1000));
    CHECK(!b.noteFinish(1, 2000, 1100));               // repeated FINISH (migration re-send): first one stays
    CHECK(b.frames[1] == 3000);
    CHECK(!b.shouldPublish(three, 60000, 60999));
    CHECK(b.secondsLeft(60000, 31000) == 30);
    CHECK(b.shouldPublish(three, 60000, 61000));       // time limit after the first finisher
    CHECK(!b.shouldPublish(three, 0, 500000));         // no limit: wait for everyone...
    CHECK(b.secondsLeft(0, 5000) == -1);
    // fix2: ...but never longer than 10 min after the first finisher (at 1000 ms here); the last minute shows the countdown
    CHECK(!b.shouldPublish(three, 0, 1000 + RaceBoard::kNoLimitCapMs - 1));
    CHECK(b.shouldPublish(three, 0, 1000 + RaceBoard::kNoLimitCapMs));
    CHECK(b.secondsLeft(0, 1000 + RaceBoard::kNoLimitCapMs - 60001) == -1);
    CHECK(b.secondsLeft(0, 1000 + RaceBoard::kNoLimitCapMs - 60000) == 60);
    CHECK(b.secondsLeft(0, 1000 + RaceBoard::kNoLimitCapMs + 5) == 0);
    CHECK(RaceBoard::kNoLimitCapMs == 600000);
    b.noteFinish(0, 2500, 2000);
    b.noteFinish(2, 3000, 2500);
    CHECK(b.shouldPublish(three, 0, 2600));            // everyone finished
    Results r;
    b.rank(slotMask({0, 1, 2, 3}), &r);                    // slot 3 present, never finished
    CHECK(r.count == 4);
    CHECK(r.e[0].slot == 0 && r.e[0].place == 1 && r.e[0].frames == 2500);
    CHECK(r.e[1].slot == 1 && r.e[1].place == 2);
    CHECK(r.e[2].slot == 2 && r.e[2].place == 2);      // tie shares the place
    CHECK(r.e[3].slot == 3 && r.e[3].place == 0 && r.e[3].frames == kDnf);
    uint8_t buf[64];
    MpWriter w(buf, sizeof buf);
    writeResults(w, r);
    Results back;
    CHECK(readResults(buf, w.n, &back) && back.count == 4);
    // a finisher who left is not ranked
    b.rank(slotMask({1, 2}), &r);
    CHECK(r.count == 2 && r.e[0].slot == 1);
    // bad finishes ignored
    RaceBoard c;
    c.reset(1);
    CHECK(!c.noteFinish(0, 0, 0));
    CHECK(!c.noteFinish(5, 10, 0));
    CHECK(!c.noteFinish(0, kMaxRaceFrames, 0));
    RaceBoard off;
    CHECK(!off.noteFinish(0, 10, 0));                  // not active
}

static void testDigest() {
    PoseDigest a, b;
    for (uint32_t i = 0; i < 100; i++) { a.add(1.0f * i, 2.0f, 3.0f, i); b.add(1.0f * i, 2.0f, 3.0f, i); }
    uint8_t da[16], db[16];
    a.out(da); b.out(db);
    CHECK(memcmp(da, db, 16) == 0);
    b.add(0, 0, 0, 101);
    b.out(db);
    CHECK(memcmp(da, db, 16) != 0);
    CHECK(crc32Update(0, "123456789", 9) == 0xCBF43926u);   // standard CRC-32 check value
    CHECK(hashName("", 0) != 0);
    CHECK(hashName("Demo", 4) != hashName("Demo", 2));
}


// gamefix2: a guest holding the button at a galaxy start: one refusal per episode, however often the site asks (60 / s for 10 s)
static void testRefusals() {
    MpFlow::RefusalGate g;
    int fresh = 0;
    for (int frame = 0; frame < 600; frame++) fresh += g.call(0, 100000 + frame * 16);   // held: 10 s at 60 / s
    CHECK(fresh == 1);
    // let go, press again later: a new episode, told how many the first one had
    uint32_t rep = 0;
    CHECK(g.call(0, 120000, &rep) && rep == 600);
    for (int frame = 1; frame < 100; frame++) CHECK(!g.call(0, 120000 + frame * 16));
    // two sites asking in the same frame (the move and a dome jump-out) do not re-arm each other
    MpFlow::RefusalGate h;
    int n = 0;
    for (int frame = 0; frame < 600; frame++) { n += h.call(0, frame * 16); n += h.call(1, frame * 16); }
    CHECK(n == 2);
    // walking away and back (more than the quiet time): a new episode each time
    MpFlow::RefusalGate w;
    n = 0;
    for (int visit = 0; visit < 5; visit++) for (int f = 0; f < 30; f++) n += w.call(1, visit * 5000 + f * 16);
    CHECK(n == 5);
    // a clock that went backwards and out-of-range kinds are safe
    MpFlow::RefusalGate b;
    b.call(2, 50000);
    CHECK(b.call(2, 100));
    CHECK(b.call(-5, 0) || true);
    CHECK(b.call(99, 0) || true);
}


// glitchfix G2 / G3 / G4 / S1: the decisions MpFlow.cpp now takes through these functions
static void testGlitchfix() {
    // G2: a second star while a demo runs is never dropped silently
    CHECK(lateStarPlan(false, false, false, true) == LateStar::Deliver);      // idle: the normal delivery
    CHECK(lateStarPlan(true, false, false, true) == LateStar::Record);        // already in our own star demo: recorded directly
    CHECK(lateStarPlan(false, true, false, true) == LateStar::Record);        // a delivery is running
    CHECK(lateStarPlan(true, false, true, true) == LateStar::NoticeOnly);     // a grand star cannot be flagged like a power star
    CHECK(lateStarPlan(true, false, false, false) == LateStar::NoticeOnly);   // holder-only star (not in the scenario mask)
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int g = 0; g < 2; g++) for (int m = 0; m < 2; m++) {
        const LateStar p = lateStarPlan(a, b, g, m);
        CHECK(a || b ? p != LateStar::Deliver : p == LateStar::Deliver);      // busy -> never the (dropped) delivery path
    }
    // G3: ids the game may give a star
    CHECK(ownStarRule(true, 1, true) == OwnStar::Send && ownStarRule(true, 7, true) == OwnStar::Send && ownStarRule(true, 8, true) == OwnStar::Send);
    CHECK(ownStarRule(true, 0, true) == OwnStar::NoId && ownStarRule(true, -1, true) == OwnStar::NoId && ownStarRule(true, 9, true) == OwnStar::NoId);
    CHECK(ownStarRule(false, 3, true) == OwnStar::Skip && ownStarRule(true, 3, false) == OwnStar::Skip);
    CHECK(clearHoldPlan(false, false, 1000, 60000) == ClearHold::Hold);
    CHECK(clearHoldPlan(false, true, 0, 60000) == ClearHold::ReleaseNoShare);          // no wait at all for a star nobody heard of
    CHECK(clearHoldPlan(true, true, 0, 60000) == ClearHold::ReleaseMove);              // a host move still wins
    CHECK(clearHoldPlan(false, false, 60001, 60000) == ClearHold::Timeout && clearHoldPlan(false, false, 60000, 60000) == ClearHold::Hold);
    // G4: a bound player is not given the star; bounded
    CHECK(bindHoldsDelivery(true, 0) && bindHoldsDelivery(true, 3001) && bindHoldsDelivery(true, kBindWaitMs - 1));
    CHECK(!bindHoldsDelivery(true, kBindWaitMs) && !bindHoldsDelivery(false, 0));
    CHECK(kBindWaitMs < 15000);   // inside the delivery's own 15 s limit
    // S1: a single-player file takes another player's claim only for a star that appeared in its own scene
    CHECK(claimVerdict(false, false, false, false, true) == ClaimVerdict::Accept);    // co-op file: the old rule (mask) stands
    CHECK(claimVerdict(true, true, false, false, true) == ClaimVerdict::Accept);      // our own star
    CHECK(claimVerdict(true, false, true, true, true) == ClaimVerdict::Accept);       // appeared here
    CHECK(claimVerdict(true, false, true, false, true) == ClaimVerdict::Retry);       // holder has it, not appeared yet: sync lag
    CHECK(claimVerdict(true, false, false, false, true) == ClaimVerdict::Retry);      // only the mask knows it: wait, then refuse
    CHECK(claimVerdict(true, false, false, false, false) == ClaimVerdict::Refuse);
    CHECK(kClaimRetryMs == 3000);
}


// ---- starfix (DESIGN-star revision 4) --------------------------------------------------------------------------------------------------
// The LeftScene snapshot (bit id-1 masks, frozen at the MOVE), the host's late-claim judgement, STARV, the claimer's pending table.
static const uint16_t kHashEgg = mp_name_hash15("EggStarGalaxy");
static LeftScene leftEgg1(uint16_t serial, int64_t leftAt) {   // Good Egg 1 (star 1) just left: the star has appeared; slots 0 and 1 were in the scene
    LeftScene L;
    L.valid = true; L.galaxy = 5; L.scenario = 1; L.galaxyHash = kHashEgg; L.serial = serial; L.leftAtMs = leftAt;
    L.hasMask = 1; L.appearedMask = 1; L.scenMask = 1; L.inSerialMask = 0x3;
    return L;
}
static Star lateStar(uint16_t serial, uint8_t id, uint8_t by) { return Star{serial, id, 0, by, 0, kHashEgg, 1}; }
static void testStarCodecs() {
    uint8_t b[32];
    MpWriter w(b, sizeof b);
    writeStarv(w, Starv{9, 3, 4, 1, 0, kHashEgg, 2, SV_CREDITED});
    Starv v;
    CHECK(w.n == 11 && w.n == kStarvBytes && readStarv(b, w.n, &v) && v.serial == 9 && v.epoch == 3 && v.starId == 4 && v.by == 1 && v.grand == 0 && v.galaxyHash == kHashEgg && v.scenario == 2 && v.verdict == SV_CREDITED);
    for (size_t n = 0; n < 32; n++) if (n != 11) CHECK(!readStarv(b, n, &v));
    uint8_t c[11];
    memcpy(c, b, 11); c[4] = 0;  CHECK(!readStarv(c, 11, &v));        // star id 0
    memcpy(c, b, 11); c[4] = 9;  CHECK(!readStarv(c, 11, &v));        // star id 9
    memcpy(c, b, 11); c[5] = 4;  CHECK(!readStarv(c, 11, &v));        // slot 4
    memcpy(c, b, 11); c[6] = 2;  CHECK(!readStarv(c, 11, &v));        // grand 2
    memcpy(c, b, 11); c[8] = 0x80; CHECK(!readStarv(c, 11, &v));      // hash over 15 bits
    memcpy(c, b, 11); c[9] = 0;  CHECK(!readStarv(c, 11, &v));        // scenario 0
    memcpy(c, b, 11); c[10] = 0; CHECK(!readStarv(c, 11, &v));        // verdict 0
    memcpy(c, b, 11); c[10] = SV_LAST + 1; CHECK(!readStarv(c, 11, &v));
    for (int i = 0; i < 100000; i++) {   // every accepted STARV re-encodes to the same bytes
        for (int k = 0; k < 11; k++) c[k] = (uint8_t)rand();
        if (readStarv(c, 11, &v)) { uint8_t o[16]; MpWriter wo(o, sizeof o); writeStarv(wo, v); CHECK(wo.n == 11 && memcmp(o, c, 11) == 0); }
    }
    // only the current host's slot AND the epoch it was sent in
    CHECK(starvAccepted(0, 0, 3, 3));
    CHECK(!starvAccepted(1, 0, 3, 3));                        // a guest (or a stranger) pretending
    CHECK(!starvAccepted(0, 0, 2, 3) && !starvAccepted(0, 0, 4, 3));   // an old (or future) epoch
    CHECK(!starvAccepted(0, -1, 3, 3));                       // no host known
    CHECK(!starvAccepted(2, 1, 3, 3) && starvAccepted(1, 1, 3, 3));    // after a migration the NEW host's slot
}
static void testLateClaim() {
    const int64_t at = 100000;
    LeftScene L = leftEgg1(41, at);
    const Star s = lateStar(41, 1, 1);
    // credited: same serial as the left scene, inside the window, claimer was in the scene, key equal, the star appeared there
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at + 1000, true) == LateResult::Credit);
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at + 1000, false) == LateResult::Credit);          // co-op file: the same
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at + kLateClaimMs, true) == LateResult::Credit);   // the window is inclusive
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at + kLateClaimMs + 1, true) == LateResult::TooLate);   // claim after the 5 s window
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at - 1, true) == LateResult::TooLate);             // a clock that went back
    CHECK(lateClaimJudge(LeftScene(), s, s.bySlot, true, at, true) == LateResult::NoScene);       // nothing remembered
    // serial: equality only. Two moves behind, ahead, and the wrap
    CHECK(lateClaimJudge(L, lateStar(40, 1, 1), lateStar(40, 1, 1).bySlot, true, at, true) == LateResult::WrongSerial);
    CHECK(lateClaimJudge(L, lateStar(42, 1, 1), lateStar(42, 1, 1).bySlot, true, at, true) == LateResult::WrongSerial);
    CHECK(lateClaimJudge(L, lateStar(39, 1, 1), lateStar(39, 1, 1).bySlot, true, at, true) == LateResult::WrongSerial);
    LeftScene W = leftEgg1(65535, at);
    CHECK(lateClaimJudge(W, lateStar(65535, 1, 1), lateStar(65535, 1, 1).bySlot, true, at, true) == LateResult::Credit);   // the serial about to wrap to 1
    CHECK(lateClaimJudge(W, lateStar(1, 1, 1), lateStar(1, 1, 1).bySlot, true, at, true) == LateResult::WrongSerial);   // (the NEW scene's serial is not "one less" arithmetic)
    CHECK(lateClaimJudge(W, lateStar(0, 1, 1), lateStar(0, 1, 1).bySlot, true, at, true) == LateResult::WrongSerial);
    // the claimer: absent slot, not in the left serial (frozen mask), and a slot the mask does not know
    CHECK(lateClaimJudge(L, s, s.bySlot, false, at, true) == LateResult::SlotAbsent);
    CHECK(lateClaimJudge(L, lateStar(41, 1, 2), lateStar(41, 1, 2).bySlot, true, at, true) == LateResult::NotInScene);   // slot 2 was in another scene
    L.inSerialMask = 0x1;
    CHECK(lateClaimJudge(L, s, s.bySlot, true, at, true) == LateResult::NotInScene);                   // slot 1 was not there at the MOVE
    L.inSerialMask = 0x3;
    // the key is a cross-check: another galaxy or scenario than the one the host left is refused
    { Star k = s; k.galaxyHash = (uint16_t)(kHashEgg ^ 1); CHECK(lateClaimJudge(L, k, k.bySlot, true, at, true) == LateResult::WrongKey); }
    { Star k = s; k.scenario = 2; CHECK(lateClaimJudge(L, k, k.bySlot, true, at, true) == LateResult::WrongKey); }
    // (c) a late GRAND star is refused, never credited silently
    { Star g = s; g.grand = 1; CHECK(lateClaimJudge(L, g, g.bySlot, true, at, true) == LateResult::GrandLate); CHECK(lateClaimJudge(L, g, g.bySlot, true, at, false) == LateResult::GrandLate); }
    // an id the scenario does not have
    CHECK(lateClaimJudge(L, lateStar(41, 2, 1), lateStar(41, 2, 1).bySlot, true, at, true) == LateResult::NoSuchStar);
    CHECK(lateClaimJudge(L, lateStar(41, 8, 1), lateStar(41, 8, 1).bySlot, true, at, false) == LateResult::NoSuchStar);
    // (b) Retry is an immediate refusal on the late path: single-player file, the star had not appeared in the left scene
    { LeftScene n = L; n.appearedMask = 0; CHECK(lateClaimJudge(n, s, s.bySlot, true, at, true) == LateResult::Credit); }    // fix2's console-only kind (the holder has it, the claimer was in the scene): counts
    { LeftScene n = L; n.hasMask = 0; n.appearedMask = 0; CHECK(lateClaimJudge(n, s, s.bySlot, true, at, false) == LateResult::Credit); }   // a co-op file: the scenario mask is the proof
    { LeftScene n = L; n.hasMask = 0; n.appearedMask = 0; CHECK(lateClaimJudge(n, s, s.bySlot, true, at, true) == LateResult::NotAppeared); }   // the mask alone is not proof
    // the verdict codes a refusal carries
    CHECK(lateVerdictCode(LateResult::Credit) == SV_CREDITED && lateVerdictCode(LateResult::GrandLate) == SV_GRAND_LATE);
    CHECK(lateVerdictCode(LateResult::TooLate) == SV_TOO_LATE && lateVerdictCode(LateResult::WrongSerial) == SV_TOO_LATE && lateVerdictCode(LateResult::NoScene) == SV_TOO_LATE);
    CHECK(lateVerdictCode(LateResult::NotInScene) == SV_NOT_THERE && lateVerdictCode(LateResult::NotAppeared) == SV_NOT_THERE && lateVerdictCode(LateResult::NoSuchStar) == SV_NO_STAR);
    // the judgement reads only the snapshot: random snapshots and claims never credit outside the rules (a model check)
    srand(77);
    int credits = 0;
    for (int i = 0; i < 200000; i++) {
        LeftScene n;
        n.valid = rand() % 8 != 0; n.galaxyHash = rand() % 3 ? kHashEgg : (uint16_t)(rand() & 0x7FFF); n.scenario = (uint8_t)(1 + rand() % 3); n.serial = (uint16_t)(rand() % 4);
        n.leftAtMs = rand() % 10000; n.hasMask = (uint8_t)rand(); n.appearedMask = (uint8_t)rand(); n.scenMask = (uint8_t)rand(); n.inSerialMask = (uint8_t)(rand() & 15);
        Star c{(uint16_t)(rand() % 4), (uint8_t)(1 + rand() % 8), (uint8_t)(rand() % 2), (uint8_t)(rand() % 4), 0, rand() % 3 ? kHashEgg : (uint16_t)(rand() & 0x7FFF), (uint8_t)(1 + rand() % 3)};
        const int64_t now = rand() % 20000;
        const bool present = rand() % 5 != 0, sp = rand() % 2;
        if (lateClaimJudge(n, c, c.bySlot, present, now, sp) != LateResult::Credit) continue;
        credits++;
        const uint8_t bit = (uint8_t)(1u << (c.starId - 1));
        CHECK(n.valid && c.serial == n.serial && now >= n.leftAtMs && now - n.leftAtMs <= kLateClaimMs && present && c.galaxyHash == n.galaxyHash && c.scenario == n.scenario);
        CHECK(!c.grand && (n.scenMask & bit) && (n.inSerialMask & (1u << c.bySlot)) && (!sp || (n.hasMask & bit)));
    }
    CHECK(credits > 100);
}
// Order independence: the host's STAR (late), its MOVE and the snapshot in any order credit the star exactly once. The model is the host's flow in
// miniature: the LeftScene is set by the MOVE, the ledger and the verdict are idempotent, a STAR that comes before the MOVE is judged live.
static void testLateOrder() {
    struct Ev { int kind; };   // 0 = MOVE executed at the host, 1 = the claimer's STAR (old serial) arrives, 2 = a snapshot is offered
    static const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& pm : perms) {
        StarLedger led; LeftScene L; uint16_t hostSerial = 41; int credited = 0, refused = 0, snapshots = 0;
        int64_t clock = 100000;
        const Star s = lateStar(41, 1, 1);
        for (int k = 0; k < 3; k++, clock += 700) {
            switch (pm[k]) {
            case 0: L = leftEgg1(hostSerial, clock); hostSerial = 42; break;
            case 1: {
                if (s.serial == hostSerial) { if (led.claim(5, 1, s.starId, 1, s.serial)) credited++; break; }   // live: the host is still in that scene
                const LateResult r = lateClaimJudge(L, s, s.bySlot, true, clock, true);
                if (r == LateResult::Credit) { if (led.claim(L.galaxy, L.scenario, s.starId, 1, s.serial)) credited++; } else refused++;
                break;
            }
            case 2: snapshots++; break;
            }
        }
        CHECK(credited == 1 && refused == 0 && snapshots == 1 && led.n == 1);
        // a repeat of the same STAR (a resend after a host change) is a duplicate, not a second credit
        const LateResult again = lateClaimJudge(L, s, s.bySlot, true, clock, true);
        if (again == LateResult::Credit) CHECK(!led.claim(L.galaxy, L.scenario, s.starId, 1, s.serial));
    }
    // a STAR whose MOVE never came to this console (no LeftScene) and whose serial is not ours: refused, nothing credited
    CHECK(lateClaimJudge(LeftScene(), lateStar(41, 1, 1), lateStar(41, 1, 1).bySlot, true, 0, true) == LateResult::NoScene);
}
static void testStarPending() {
    StarPending p;
    const Star a = lateStar(7, 1, 1), b = lateStar(7, 2, 1), c = lateStar(8, 3, 1);
    p.add(a, 1000); p.add(b, 1100);
    CHECK(p.count() == 2);
    p.add(a, 1200);                                       // the same claim again: one entry
    CHECK(p.count() == 2);
    p.add(c, 1300);                                       // a third fits (4 at once)
    CHECK(p.count() == 3);
    CHECK(p.answer(8, 3) && !p.answer(8, 3));             // answered by the STARV (serial, id): gone
    CHECK(!p.answer(9, 1));
    Star out;
    CHECK(!p.takeExpired(1000 + 59000, 60000, &out));    // not yet
    CHECK(p.takeExpired(1100 + 60000, 60000, &out) && out.starId == 2);   // b after 60 s
    CHECK(p.takeExpired(1200 + 60000, 60000, &out) && out.starId == 1);   // a (re-added at 1200)
    CHECK(p.count() == 0 && !p.takeExpired(999999, 60000, &out));
    p.add(a, 5000); p.clear(); CHECK(p.count() == 0);
    p.add(a, 9000);
    CHECK(p.takeExpired(100, 60000, &out));              // a clock that went back expires the entry instead of holding it forever
}
static void testLedgerErase() {
    StarLedger l;
    CHECK(l.claim(5, 1, 1, 1, 41) && l.claim(5, 1, 2, 1, 41));
    CHECK(!l.erase(5, 1, 1, 40) && !l.erase(5, 2, 1, 41) && l.n == 2);   // another serial or key: not that claim
    CHECK(l.erase(5, 1, 1, 41) && l.n == 1 && !l.find(5, 1, 1) && l.find(5, 1, 2));
    CHECK(l.claim(5, 1, 1, 0, 41));                                       // collectable again after the refusal
}
// The galaxy mapping is identical on every console: a 15-bit hash of the name, unique over the galaxy names (the cooked table is checked by
// tests/mp/scenario_data_check.py); an unknown hash maps to nothing.
static void testGalaxyHash() {
    static const char* const names[] = {"AstroGalaxy", "BattleShipGalaxy", "BeltConveyerExGalaxy", "BreakDownPlanetGalaxy", "CannonFleetGalaxy", "CocoonExGalaxy",
        "CosmosGardenGalaxy", "DarkRoomGalaxy", "EggStarGalaxy", "FactoryGalaxy", "FishTunnelGalaxy", "FlipPanelExGalaxy", "FloaterOtaKingGalaxy", "HeavenlyBeachGalaxy",
        "HeavensDoorGalaxy", "HellProminenceGalaxy", "HoneyBeeExGalaxy", "HoneyBeeKingdomGalaxy", "IceVolcanoGalaxy", "OceanFloaterLandGalaxy", "OceanPhantomCaveGalaxy",
        "OceanRingGalaxy", "PeachCastleFinalGalaxy", "PeachCastleGardenGalaxy", "PhantomGalaxy", "ReverseKingdomGalaxy", "SandClockGalaxy", "SkullSharkGalaxy",
        "SnowCapsuleGalaxy", "SpinDriverPathStorageGalaxy", "StarDustGalaxy", "TearDropGalaxy", "TransformationExGalaxy"};
    const int n = (int)(sizeof names / sizeof names[0]);
    for (int i = 0; i < n; i++) {
        CHECK(mp_name_hash15(names[i]) < 0x8000);
        for (int j = i + 1; j < n; j++) CHECK(mp_name_hash15(names[i]) != mp_name_hash15(names[j]));
    }
    CHECK(mp_name_hash15("EggStarGalaxy") == 0x097F);   // the value tests/mp/scenario_data_check.py computes (same function, python side)
    // signedness: the bytes are hashed as uint8_t (a name with a high byte hashes the same on x86 and ARM)
    CHECK(mp_name_hash15("Ab\xE9") == mp_name_hash15("Ab\xE9"));
}

// ---- starfix review 14 Nov: the flow clock, the real sender, the retry flush, replay after a snapshot, the pending table, a host change ---------------
static void testFlowClockAndSender() {
    const int64_t at = 0;
    FlowClock clk;
    clk.tick(1000, kFlowStepCapMs);
    LeftScene L = leftEgg1(41, clk.t);                       // the MOVE, on the flow clock
    const Star s = lateStar(41, 1, 1);
    // 20 s of loading with nothing pumped, then the first pump: one step only, so the claim (which arrived 100 ms after the MOVE) still credits
    const int64_t afterLoad = clk.tick(1000 + 20000, kFlowStepCapMs);
    CHECK(afterLoad - L.leftAtMs <= kFlowStepCapMs);
    CHECK(lateClaimJudge(L, s, 1, true, afterLoad, true) == LateResult::Credit);
    // real pumping time still ages it: 6 s of 100 ms pumps
    int64_t real = 21000;
    for (int i = 0; i < 60; i++) { real += 100; clk.tick(real, kFlowStepCapMs); }
    CHECK(clk.t - L.leftAtMs > kLateClaimMs && lateClaimJudge(L, s, 1, true, clk.t, true) == LateResult::TooLate);
    // a hitch inside a pump counts as one capped step; a clock that goes back adds nothing
    FlowClock c2; c2.tick(100, kFlowStepCapMs);
    CHECK(c2.tick(100 + 30000, kFlowStepCapMs) == kFlowStepCapMs && c2.tick(50, kFlowStepCapMs) == kFlowStepCapMs);
    (void)at;
    // the REAL sender decides "in the scene", not the claimer's own bySlot field (slot 1 was in the scene, slot 2 was not)
    LeftScene M = leftEgg1(41, 100);
    Star liar = lateStar(41, 1, 1);                          // claims to be slot 1 ...
    CHECK(lateClaimJudge(M, liar, 2, true, 100, true) == LateResult::NotInScene);       // ... but slot 2 sent it
    Star honest = lateStar(41, 1, 2);                        // claims to be slot 2 ...
    CHECK(lateClaimJudge(M, honest, 1, true, 100, true) == LateResult::Credit);         // ... slot 1 sent it (in the scene): judged by the sender
    CHECK(lateClaimJudge(M, liar, -1, true, 100, true) == LateResult::NoSuchStar && lateClaimJudge(M, liar, 4, true, 100, true) == LateResult::NoSuchStar);
}
static void testPendingAndReplay() {
    StarPending p;
    for (int i = 1; i <= StarPending::kEntries; i++) CHECK(p.add(lateStar(7, (uint8_t)i, 1), 1000 + i));
    CHECK(!p.add(lateStar(7, 7, 1), 2000) && p.count() == StarPending::kEntries);       // one more is reported (not tracked), nothing is overwritten
    CHECK(p.add(lateStar(7, 2, 1), 2000) && p.count() == StarPending::kEntries);                            // the same claim again is not a new one
    Star out;
    CHECK(!p.takeResend(1004 + 9000, kStarResendMs, &out));
    int n = 0; while (p.takeResend(1004 + kStarResendMs, kStarResendMs, &out)) n++;
    CHECK(n == 3);                                                                        // the three older ones are due; the re-added one (2000) is not
    CHECK(!p.takeResend(1004 + kStarResendMs, kStarResendMs, &out));                    // each is sent once per interval
    CHECK(p.takeResend(1004 + 2 * kStarResendMs, kStarResendMs, &out));
    // replay: a snapshot older than this session's credits must not lose them
    StarLedger l;
    l.claim(5, 1, 1, 1, 41); l.claim(5, 1, 2, 0, 41); l.claim(6, 2, 3, 1, 50); l.claim(5, 1, 7, 1, 41);
    int set[8][3] = {}, k = 0;
    const int r = replayLedgerFlags(l, [](uint8_t g, uint8_t sc) { return (g == 5 && sc == 1) ? 0x03u : (g == 6 && sc == 2) ? 0x04u : 0u; },
                                    [&](uint8_t g, uint8_t sc, uint8_t id) { set[k][0] = g; set[k][1] = sc; set[k][2] = id; k++; });
    CHECK(r == 3 && k == 3);                                                              // star 7 is not in the scenario mask (a grand / holder-only star): no flag
    CHECK(set[0][2] == 1 && set[1][2] == 2 && set[2][0] == 6 && set[2][2] == 3);
    StarLedger empty;
    CHECK(replayLedgerFlags(empty, [](uint8_t, uint8_t) { return 0xFFu; }, [](uint8_t, uint8_t, uint8_t) {}) == 0);
    // the retry flush: a claim waiting in the 3 s table when the host moves is judged against the scene just left (here: credited from the frozen snapshot)
    ClaimRetryTable rt;
    LeftScene L = leftEgg1(41, 0); L.appearedMask = 0;                                    // (a hidden star: the holder has it, it has not appeared)
    CHECK(rt.add(lateStar(41, 1, 1), 1, true, 3000));
    int credited = 0;
    for (int i = 0; i < kClaimRetryMax; i++) if (rt.slot[i].valid) { const ClaimRetryEntry e = rt.slot[i]; rt.slot[i].valid = false; if (lateClaimJudge(L, e.s, e.from, true, 100, true) == LateResult::Credit) credited++; }
    CHECK(credited == 1 && rt.count() == 0);
    // a repeat of a credited claim is recognised (same serial), another scene's is not
    StarLedger h; h.claim(5, 1, 1, 1, 41);
    CHECK(starAlreadyCredited(h, 5, 1, lateStar(41, 1, 1)) && !starAlreadyCredited(h, 5, 1, lateStar(42, 1, 1)) && !starAlreadyCredited(h, 5, 1, lateStar(41, 2, 1)));
}
// Three consoles, a host that vanishes without a verdict (Fable's case): H (slot 0, old host), C (slot 1, the claimer), G (slot 2, becomes the host).
// The decisions are PRODUCTION code (hostDecideStar, decideStarv, becameHostAnswers, expireStarClaim, ReplyCap, replayLedgerFlags: MpFlowLogic.hpp, called by MpFlow.cpp);
// only the effects (ledger, flags, toasts: a few lines each in MpFlow.cpp) are modelled here.
struct Con {
    int slot = 0; uint16_t serial = 0; uint16_t epoch = 0; int hostSlot = 0; bool sp = true;
    StarLedger led; LeftScene left; StarPending pend; FlowClock clk; MpWorldGuard::SenderLimits lim; ReplyCap cap;
    bool flag[8] = {}; bool toast = false; int flagSets = 0; int privateReplies = 0;
};
struct HostOut { StarDecision d; int fresh = 0; bool sentCredited = false; bool sentPrivateDropped = false; uint8_t verdict = 0; };
// handleStar in miniature: the probes are the scene's; `appeared` / `sameScene` are what the live scene would say
static HostOut hostStar(Con& h, const Star& s, int from, bool appeared = true, bool sameScene = true, bool slotPresent = true) {
    HostOut o;
    StarClaimIn in;
    in.s = s; in.from = from; in.sceneKnown = true; in.slotPresent = slotPresent; in.spFile = h.sp; in.flowNow = h.clk.t; in.hostSerial = h.serial; in.claimGalaxyIdx = 5;
    if (s.serial == h.serial) { in.liveReal = true; in.liveSynthOk = true; in.liveHas = true; in.liveAppeared = appeared; in.claimerSameScene = sameScene; }
    o.d = hostDecideStar(in, h.led, h.left, [&] { return from == h.hostSlot || h.lim.allow(from, MpWorldGuard::K_STAR, (uint32_t)(h.clk.t * 3 / 50)); });
    switch (o.d.act) {
    case StarAct::DropReply: if (h.cap.allow(from, h.clk.t + 1)) { o.sentPrivateDropped = true; h.privateReplies++; } o.verdict = SV_DROPPED; break;
    case StarAct::AnswerAgain: if (h.cap.allow(from, h.clk.t + 1)) { o.sentCredited = true; h.privateReplies++; } o.verdict = SV_CREDITED; break;
    case StarAct::CreditLive:
        if (h.led.claim(5, 1, s.starId, from, s.serial, s.grand != 0)) { o.fresh = 1; h.flag[s.starId] = true; h.flagSets++; }
        o.sentCredited = true; o.verdict = SV_CREDITED; break;
    case StarAct::CreditLate:
        if (h.led.claim(h.left.galaxy, h.left.scenario, s.starId, from, s.serial, false)) { o.fresh = 1; h.flag[s.starId] = true; h.flagSets++; }
        o.sentCredited = true; o.verdict = SV_CREDITED; break;
    default: o.verdict = o.d.verdict; break;   // refused: a STARV with the reason (and the toast)
    }
    return o;
}
// handleStarv in miniature: true when it was accepted
static bool onStarv(Con& c, const Starv& v, int fromSlot, bool live = false) {
    StarvIn in;
    in.v = v; in.fromSlot = fromSlot; in.hostSlot = c.hostSlot; in.hostEpoch = c.epoch; in.isHost = c.hostSlot == c.slot; in.together = true; in.localSlot = c.slot;
    in.galaxyIdx = 5; in.scenMask = 0x03; in.live = live; in.liveHolderReal = live;
    const StarvAct act = decideStarv(in);
    if (act == StarvAct::Drop) return false;
    if (act == StarvAct::Ignore) return true;
    if (v.by == c.slot) c.pend.answer(v.serial, v.starId);
    switch (act) {
    case StarvAct::RefusedOwn: case StarvAct::RefusedOwnToast: c.led.erase(5, v.scenario, v.starId, v.serial); c.toast = true; break;
    case StarvAct::CreditedOwn: if (!v.grand) { c.flag[v.starId] = true; } break;
    case StarvAct::CreditLive: case StarvAct::RecordLate:
        if (c.led.claim(5, v.scenario, v.starId, v.by, v.serial, v.grand != 0)) { c.flag[v.starId] = true; c.flagSets++; }
        break;
    default: break;
    }
    return true;
}
static Starv starvOf(const Star& s, uint16_t epoch, int by, uint8_t verdict) { return Starv{s.serial, epoch, s.starId, (uint8_t)by, s.grand, s.galaxyHash, s.scenario, verdict}; }
static void testHostChange() {
    const int64_t kT0 = 10000;
    // C claimed star 1 in serial 41 under host H (epoch 3): its own ledger entry, its flag, its pending entry. H moved to 42 and vanished.
    auto makeC = []() {
        Con c; c.slot = 1; c.serial = 42; c.epoch = 3; c.hostSlot = 0;
        c.led.claim(5, 1, 1, 1, 41); c.flag[1] = true; c.pend.add(lateStar(41, 1, 1), 5000);
        return c;
    };
    auto makeG = [](uint16_t serial) { Con g; g.slot = 2; g.serial = serial; g.epoch = 4; g.hostSlot = 2; return g; };
    auto change = [](Con& c) { c.epoch = 4; c.hostSlot = 2; };   // the core learns the new host
    const Star claim = lateStar(41, 1, 1);
    // (1) G is still in serial 41: judged live, credited once; C's pending is answered, C keeps its flag. On a single-player file the live judgement needs the claimer's
    //     last pose in G's serial: without it the claim is NOT credited live (Retry, then refused)
    {
        Con c = makeC(), g = makeG(41);
        change(c);
        const HostOut o = hostStar(g, claim, 1);
        CHECK(o.d.act == StarAct::CreditLive && o.fresh == 1 && g.flag[1] && g.led.n == 1);
        CHECK(onStarv(c, starvOf(claim, 4, 1, o.verdict), 2) && c.pend.count() == 0 && c.flag[1] && !c.toast && c.led.n == 1);
        // (4) the same re-send delivered twice: no second credit, still answered credited, C's pending answered exactly once
        const HostOut o2 = hostStar(g, claim, 1);
        CHECK(o2.d.act == StarAct::AnswerAgain && o2.verdict == SV_CREDITED && o2.fresh == 0 && g.flagSets == 1 && g.led.n == 1);
        CHECK(onStarv(c, starvOf(claim, 4, 1, o2.verdict), 2) && c.pend.count() == 0 && c.led.n == 1 && !c.pend.answer(41, 1));
        Con g2 = makeG(41);
        CHECK(hostStar(g2, claim, 1, false, false).d.act == StarAct::Retry);                 // single-player file, pose not in G's serial: waits (then refused)
        CHECK(hostStar(g2, claim, 1, false, true).d.act == StarAct::CreditLive);            // the console-only kind (holder has it, not appeared), pose in the serial: credited
        Con g3 = makeG(41); g3.sp = false;
        CHECK(hostStar(g3, claim, 1, false, false).d.act == StarAct::CreditLive);           // a co-op file: the old rules
    }
    // (2) G already moved (its own LeftScene from its MOVE), the re-send is inside ITS window: credited once from that
    {
        Con c = makeC(), g = makeG(42);
        g.clk.tick(kT0, kFlowStepCapMs); g.left = leftEgg1(41, g.clk.t);
        change(c);
        g.clk.tick(kT0 + 300, kFlowStepCapMs);
        const HostOut o = hostStar(g, claim, 1);
        CHECK(o.d.act == StarAct::CreditLate && o.fresh == 1 && g.led.n == 1 && g.flag[1]);
        CHECK(onStarv(c, starvOf(claim, 4, 1, o.verdict), 2) && c.pend.count() == 0 && !c.toast);
        // a resend after G's window (the claimer's 10 s repeat) is answered credited again, not refused
        for (int i = 0; i < 40; i++) g.clk.tick(kT0 + 300 + 100 * (i + 1) * 2, kFlowStepCapMs);
        CHECK(g.clk.t - g.left.leftAtMs > kLateClaimMs);
        const HostOut o2 = hostStar(g, claim, 1);
        CHECK(o2.d.act == StarAct::AnswerAgain && o2.verdict == SV_CREDITED && o2.fresh == 0 && g.led.n == 1);
        // the claimer's slot went absent during the window: the frozen bit goes, a newcomer does not inherit it
        Con g4 = makeG(42); g4.clk.tick(kT0, kFlowStepCapMs); g4.left = leftEgg1(41, g4.clk.t); g4.left.slotGone(1);
        CHECK(hostStar(g4, claim, 1).d.late == LateResult::NotInScene);
    }
    // (3) G outside its window: refused; C's pending entry cleared, its ledger entry erased, the toast flagged (never silent)
    {
        Con c = makeC(), g = makeG(42);
        g.clk.tick(kT0, kFlowStepCapMs); g.left = leftEgg1(41, g.clk.t);
        for (int i = 0; i < 20; i++) g.clk.tick(kT0 + 500 * (i + 1), kFlowStepCapMs);
        change(c);
        const HostOut o = hostStar(g, claim, 1);
        CHECK(o.d.act == StarAct::RefuseLate && o.verdict == SV_TOO_LATE && o.fresh == 0 && g.led.n == 0 && !g.flag[1]);
        CHECK(onStarv(c, starvOf(claim, 4, 1, o.verdict), 2));
        CHECK(c.pend.count() == 0 && c.led.n == 0 && c.toast);
    }
    // (5) a STARV of the old epoch from the old host's slot is rejected after the change; epoch 4 from G's slot is accepted
    {
        Con c = makeC(); change(c);
        CHECK(!onStarv(c, starvOf(claim, 3, 1, SV_CREDITED), 0) && c.pend.count() == 1);
        CHECK(!onStarv(c, starvOf(claim, 4, 1, SV_CREDITED), 0));                         // right epoch, wrong slot
        CHECK(!onStarv(c, starvOf(claim, 3, 1, SV_CREDITED), 2));                         // right slot, old epoch
        CHECK(onStarv(c, starvOf(claim, 4, 1, SV_CREDITED), 2) && c.pend.count() == 0);
        // a STARV that was dropped (the epoch was not known yet) leaves the claim pending: it is re-sent until a verdict arrives
        Con d = makeC();
        CHECK(!onStarv(d, starvOf(claim, 4, 1, SV_CREDITED), 2) && d.pend.count() == 1);   // (d has not learned epoch 4)
        Star out; CHECK(d.pend.takeResend(5000 + kStarResendMs, kStarResendMs, &out) && out.starId == 1);
    }
    // (6) G is the claimer: becameHostAnswers empties its pending table and the others credit once on its epoch-4 STARV
    {
        Con g; g.slot = 2; g.serial = 42; g.epoch = 3; g.hostSlot = 0;
        g.led.claim(5, 1, 1, 2, 41); g.flag[1] = true; g.pend.add(lateStar(41, 1, 2), 5000);
        Con c; c.slot = 1; c.serial = 42; c.epoch = 3; c.hostSlot = 0;
        g.epoch = 4; g.hostSlot = 2; c.epoch = 4; c.hostSlot = 2;
        Starv sent[StarPending::kEntries];
        const int n = becameHostAnswers(g.pend, 2, 4, sent, StarPending::kEntries);
        CHECK(n == 1 && g.pend.count() == 0 && sent[0].epoch == 4 && sent[0].by == 2 && sent[0].verdict == SV_CREDITED);
        CHECK(onStarv(c, sent[0], 2) && c.flag[1] && c.led.n == 1 && c.flagSets == 1);
        CHECK(onStarv(c, sent[0], 2) && c.flagSets == 1 && c.led.n == 1);                  // delivered twice: credited once
        CHECK(becameHostAnswers(g.pend, 2, 4, sent, StarPending::kEntries) == 0);
    }
}
// The rate cap (burst 4, 1 / s per sender) is part of the model: a flood is dropped with a private reply at most once a second; a repeat of a credited claim is
// answered "credited" even when the cap is spent (never "dropped")
static void testRateAndReplies() {
    Con h; h.slot = 0; h.serial = 41; h.epoch = 3; h.hostSlot = 0;
    int credited = 0, dropped = 0;
    for (uint8_t id = 1; id <= 8; id++) {                       // 8 different stars of the scenario in one turn: (mask 0x03 only has 1 and 2: the rest are "no such star", after the cap)
        const HostOut o = hostStar(h, lateStar(41, id, 1), 1);
        if (o.d.act == StarAct::CreditLive) credited++;
        if (o.d.act == StarAct::DropReply) dropped++;
    }
    // the cap is 4 tokens: claims 5..8 are dropped; only star 1 is in the scene's mask here (leftEgg1-style probes say every id is real in hostStar, so ids 1..4 credit)
    CHECK(credited == 4 && dropped == 4);
    CHECK(h.privateReplies == 1);                               // 4 drops, one private reply (1 / s / sender)
    const HostOut again = hostStar(h, lateStar(41, 1, 1), 1);   // star 1 was credited: answered again even though the sender is over its cap
    CHECK(again.d.act == StarAct::AnswerAgain && again.verdict == SV_CREDITED);
    // no scene known yet: dropped with a private reply, nothing judged
    StarClaimIn in; in.s = lateStar(41, 1, 1); in.from = 1; in.sceneKnown = false;
    StarLedger led; LeftScene none;
    CHECK(hostDecideStar(in, led, none, [] { return true; }).act == StarAct::DropReply);
    // ReplyCap: once a second per sender, independent per sender, a clock that goes back is tolerated, bad slots refused
    ReplyCap rc;
    CHECK(rc.allow(1, 1000) && !rc.allow(1, 1500) && rc.allow(2, 1500) && rc.allow(1, 2000) && !rc.allow(1, 2999) && rc.allow(1, 3000));
    CHECK(rc.allow(1, 100) && !rc.allow(-1, 0) && !rc.allow(4, 0));
}
// verdict 7 (dropped) on the claimer: its ledger entry is taken back and it toasts itself (the host sent no TOAST for it); other refusals only erase (the host's TOAST shows)
static void testDroppedVerdictOnClaimer() {
    const Star s = lateStar(41, 1, 1);
    StarvIn in;
    in.v = starvOf(s, 4, 1, SV_DROPPED); in.fromSlot = 2; in.hostSlot = 2; in.hostEpoch = 4; in.localSlot = 1; in.galaxyIdx = 5; in.scenMask = 3;
    CHECK(decideStarv(in) == StarvAct::RefusedOwnToast);
    in.v.verdict = SV_TOO_LATE; CHECK(decideStarv(in) == StarvAct::RefusedOwn);
    in.localSlot = 3; in.v.verdict = SV_DROPPED; CHECK(decideStarv(in) == StarvAct::RefusedOther);   // someone else's claim: nothing to take back
    in.localSlot = 1; in.fromSlot = 0; CHECK(decideStarv(in) == StarvAct::Drop);                       // not from the host's slot
    in.fromSlot = 2; in.isHost = true; CHECK(decideStarv(in) == StarvAct::Ignore);
    // credited, not ours: the guest's veto is the STATIC mask (or the live scene's holder), a grand star outside its scene is not recorded
    StarvIn g;
    g.v = starvOf(s, 4, 0, SV_CREDITED); g.fromSlot = 2; g.hostSlot = 2; g.hostEpoch = 4; g.localSlot = 1; g.galaxyIdx = 5; g.scenMask = 1;
    CHECK(decideStarv(g) == StarvAct::RecordLate);
    g.scenMask = 2; CHECK(decideStarv(g) == StarvAct::Veto);                                           // id 1 is not in the mask
    g.live = true; g.liveHolderReal = true; CHECK(decideStarv(g) == StarvAct::CreditLive);             // in that very scene, the holder has it
    g.live = false; g.scenMask = 1; g.v.grand = 1; CHECK(decideStarv(g) == StarvAct::NotRecorded);
    g.v.grand = 0; g.galaxyIdx = -1; CHECK(decideStarv(g) == StarvAct::Veto);                          // unknown galaxy: nothing is known about it
    g.galaxyIdx = 5; g.v.by = 1; CHECK(decideStarv(g) == StarvAct::CreditedOwn);                       // our own: the flag is set again
}
// review round 2: a grand star is never replayed (even when its id is in the scenario mask), an expired own claim leaves the ledger so no replay sets its flag
static void testReplayGrandAndExpiry() {
    StarLedger l;
    l.claim(5, 1, 1, 1, 41, false);
    l.claim(5, 1, 2, 1, 41, true);                       // a grand star whose id IS in the mask
    int sets = 0, last = 0;
    auto maskOf = [](uint8_t, uint8_t) { return 0x03u; };
    auto setFlag = [&](uint8_t, uint8_t, uint8_t id) { sets++; last = id; };
    CHECK(replayLedgerFlags(l, maskOf, setFlag) == 1 && sets == 1 && last == 1);                       // only the non-grand star
    CHECK(l.find(5, 1, 2) && l.find(5, 1, 2)->grand == 1 && l.find(5, 1, 1)->grand == 0);
    CHECK(l.claim(5, 1, 2, 1, 42, false) && l.find(5, 1, 2)->grand == 0);                              // renewed by a later visit: the new kind
    // the 60 s expiry of an own claim without a verdict takes the ledger entry back
    StarLedger m; StarPending p;
    const Star s = lateStar(41, 1, 1);
    m.claim(5, 1, 1, 1, 41); p.add(s, 1000);
    Star out;
    auto idxOf = [](uint16_t h) { return h == kHashEgg ? 5 : -1; };
    CHECK(!expireStarClaim(p, m, 1000 + 59999, 60000, idxOf, &out) && m.n == 1);
    CHECK(expireStarClaim(p, m, 1000 + 60000, 60000, idxOf, &out) && out.starId == 1 && m.n == 0 && p.count() == 0);
    sets = 0;
    CHECK(replayLedgerFlags(m, maskOf, setFlag) == 0 && sets == 0);                                    // a later snapshot replays nothing for it
    // an unknown galaxy hash cannot erase anything
    StarLedger k; StarPending q; k.claim(5, 1, 1, 1, 41); q.add(lateStar(41, 1, 1), 0);
    CHECK(expireStarClaim(q, k, 60000, 60000, [](uint16_t) { return -1; }, &out) && k.n == 1);
    // starfix round 3: a 7th simultaneous unanswered claim cannot be tracked: its ledger entry is erased at once (no expiry could ever take it back)
    StarLedger z; StarPending zp;
    for (int i = 1; i <= StarPending::kEntries; i++) { Star c = lateStar(41, (uint8_t)i, 1); z.claim(5, 1, (uint8_t)i, 1, 41); CHECK(trackOwnClaim(zp, z, c, 1000, idxOf)); }
    CHECK(zp.count() == StarPending::kEntries && z.n == StarPending::kEntries);
    Star seventh = lateStar(41, 7, 1); z.claim(5, 1, 7, 1, 41);
    CHECK(!trackOwnClaim(zp, z, seventh, 1000, idxOf) && z.n == StarPending::kEntries && !z.find(5, 1, 7) && zp.count() == StarPending::kEntries);
    CHECK(expireStarClaim(zp, z, 1000 + 60000, 60000, idxOf, &out) && z.n == StarPending::kEntries - 1);   // the tracked ones still expire as before
}

// racefix: the synchronized race start (RaceStartGate): the host's GO decision and each console's start rule
static void testRaceStartGate() {
    const unsigned all3 = 0x7;
    {   // all racers ready: one GO, as soon as the last one reports (the host is in Action)
        RaceStartGate g; g.reset(5);
        CHECK(!g.shouldSendGo(all3, 1000));            // the host itself is not in Action yet
        g.hostEntered(0, 2000);
        CHECK(!g.shouldSendGo(all3, 2000));
        g.noteReady(1, 5);
        CHECK(!g.shouldSendGo(all3, 2500));
        g.noteReady(2, 5);
        CHECK(g.shouldSendGo(all3, 3000));
        g.goSent = true;
        CHECK(!g.shouldSendGo(all3, 3001));            // once
    }
    {   // one racer slow: GO waits for it, then for the fallback measured on the host
        RaceStartGate g; g.reset(5);
        g.hostEntered(0, 1000);
        g.noteReady(1, 5);
        CHECK(!g.shouldSendGo(all3, 1000 + kRaceGoWaitMs - 1));
        CHECK(g.shouldSendGo(all3, 1000 + kRaceGoWaitMs));
        RaceStartGate h; h.reset(5);                   // (or the slow one reports in time)
        h.hostEntered(0, 1000); h.noteReady(1, 5);
        CHECK(!h.shouldSendGo(all3, 5000));
        h.noteReady(2, 5);
        CHECK(h.shouldSendGo(all3, 5000));
    }
    {   // a stale serial, a bad slot and an absent slot's READY change nothing; a racer who left is not waited for
        RaceStartGate g; g.reset(5);
        g.hostEntered(0, 0);
        g.noteReady(1, 4); g.noteReady(9, 5); g.noteReady(-1, 5);
        CHECK(g.ready == 1u);
        g.noteReady(1, 5);
        CHECK(!g.shouldSendGo(all3, 10));
        CHECK(g.shouldSendGo(0x3, 10));                // slot 2 left (present mask without it)
        CHECK(!g.shouldSendGo(0, 10));                 // nobody present: never
    }
    {   // a racer joining late: present while the host waits, so GO waits for it; if it is ready later but before the fallback, one GO
        RaceStartGate g; g.reset(7);
        g.hostEntered(0, 0); g.noteReady(1, 7);
        CHECK(g.shouldSendGo(0x3, 100));               // (two present: go)
        RaceStartGate h; h.reset(7);
        h.hostEntered(0, 0); h.noteReady(1, 7);
        CHECK(!h.shouldSendGo(0x7, 4000));             // slot 2 joined meanwhile
        h.noteReady(2, 7);
        CHECK(h.shouldSendGo(0x7, 4001));
    }
    {   // host migration during the wait: the new host's wait begins again, READY comes again from the guests
        RaceStartGate g; g.reset(8);
        g.hostEntered(1, 0);                           // (slot 1 became host; it was in Action since t=0)
        CHECK(g.shouldSendGo(0x2, 0) == true);         // alone: goes at once
        RaceStartGate h; h.reset(8);
        h.hostEntered(1, 0);
        h.hostChanged(9000);                           // the old host went away at 9 s
        CHECK(!h.shouldSendGo(0x7, 9000 + kRaceGoWaitMs - 1));   // not judged late for the old host's time
        h.noteReady(0, 8); h.noteReady(2, 8);
        CHECK(h.shouldSendGo(0x7, 9500));
        RaceStartGate n; n.reset(8);
        n.hostChanged(100);                            // not in Action yet: nothing begins
        CHECK(n.waitSinceMs < 0 && !n.shouldSendGo(0x7, 99999));
    }
    {   // a console's own rule
        CHECK(!RaceStartGate::consoleStarts(false, true, 0, 99999));                    // never before its own Action
        CHECK(!RaceStartGate::consoleStarts(true, false, 1000, 1000 + kRaceGuestGoWaitMs - 1));
        CHECK(RaceStartGate::consoleStarts(true, true, 1000, 1000));                    // GO known (even before its Action: starts at once)
        CHECK(RaceStartGate::consoleStarts(true, false, 1000, 1000 + kRaceGuestGoWaitMs));   // lost host: alone
        CHECK(kRaceGuestGoWaitMs > kRaceGoWaitMs);
        CHECK(!RaceStartGate::shouldSendReady(false, false, -1, 0));
        CHECK(RaceStartGate::shouldSendReady(true, false, -1, 0));
        CHECK(!RaceStartGate::shouldSendReady(true, false, 100, 100 + kRaceReadyResendMs - 1));
        CHECK(RaceStartGate::shouldSendReady(true, false, 100, 100 + kRaceReadyResendMs));
        CHECK(!RaceStartGate::shouldSendReady(true, true, -1, 0));                      // GO known: stop reporting
    }
    {   // two consoles whose openings end 6 s apart (one tapped A): both start on the same GO
        RaceStartGate host; host.reset(1);
        const int64_t hostAction = 8000, guestAction = 14000;
        int64_t goAt = -1;
        for (int64_t t = 0; t < 30000 && goAt < 0; t += 50) {
            if (t >= hostAction) host.hostEntered(0, t);
            if (t >= guestAction) host.noteReady(1, 1);
            if (t >= hostAction && host.shouldSendGo(0x3, t)) goAt = t;
        }
        CHECK(goAt >= guestAction && goAt < guestAction + 100);
    }
    {   // racefix review: the host's GO does not depend on its scene: after its Action entry only the gate and the clock decide
        RaceStartGate g; g.reset(3);
        g.hostEntered(0, 1000);                        // (then the host pauses or dies: hostEntered is not called again)
        g.noteReady(1, 3);
        CHECK(!g.shouldSendGo(0x7, 1000 + kRaceGoWaitMs - 1));
        CHECK(g.shouldSendGo(0x7, 1000 + kRaceGoWaitMs));   // the fallback still releases the others
        g.noteReady(2, 3);
        CHECK(g.shouldSendGo(0x7, 1500));              // and a late READY releases them at once
    }
    {   // READY after GO: the stored GO goes to that slot alone, until the results exist
        CHECK(RaceStartGate::answerLateReady(true, true, false, 12, 2, 0));
        CHECK(!RaceStartGate::answerLateReady(true, false, false, 0, 2, 0));    // no GO yet
        CHECK(!RaceStartGate::answerLateReady(true, true, true, 12, 2, 0));     // results known
        CHECK(!RaceStartGate::answerLateReady(false, true, false, 12, 2, 0));   // not the host
        CHECK(!RaceStartGate::answerLateReady(true, true, false, 12, 0, 0));    // never to itself
        CHECK(!RaceStartGate::answerLateReady(true, true, false, 12, 4, 0));    // slot out of range
        CHECK(!RaceStartGate::answerLateReady(true, true, false, 12, -1, 0));
    }
    {   // the READY handler's drops
        CHECK(RaceStartGate::acceptReady(true, true, 7, 7, 1));
        CHECK(!RaceStartGate::acceptReady(false, true, 7, 7, 1));   // a guest never collects READY
        CHECK(!RaceStartGate::acceptReady(true, false, 7, 7, 1));   // no race
        CHECK(!RaceStartGate::acceptReady(true, true, 7, 8, 1));    // another serial
        CHECK(!RaceStartGate::acceptReady(true, true, 7, 7, 4));
        CHECK(!RaceStartGate::acceptReady(true, true, 7, 7, -1));
    }
    {   // a late or repeated GO
        CHECK(RaceStartGate::acceptGo(true, 7, 7, false));
        CHECK(!RaceStartGate::acceptGo(true, 7, 7, true));          // repeated (the host's re-send, a migrated host's GO)
        CHECK(!RaceStartGate::acceptGo(true, 7, 6, false));         // an old race's GO
        CHECK(!RaceStartGate::acceptGo(false, 7, 7, false));        // not armed
    }
    {   // onBecameHost
        CHECK(RaceStartGate::resendGoAsNewHost(true, false));
        CHECK(RaceStartGate::resendGoAsNewHost(false, true));       // started alone: still releases the others
        CHECK(RaceStartGate::resendGoAsNewHost(true, true));
        CHECK(!RaceStartGate::resendGoAsNewHost(false, false));     // no GO known: it decides itself from the READYs
    }
    {   // a reused slot's stale READY is dropped; the host's own bit stays
        RaceStartGate g; g.reset(2);
        g.hostEntered(0, 0); g.noteReady(1, 2); g.noteReady(2, 2);
        g.prune(0x5, 0);                               // slot 1 left (slot 2 and the host remain)
        CHECK(g.ready == 0x5u);
        g.prune(0x0, 0);
        CHECK(g.ready == 0x1u);
    }
    {   // a miss during the countdown or the head start wait: the new scene's player is held again
        CHECK(RaceStartGate::needsRehold(true, false, false, false, true, true));
        CHECK(!RaceStartGate::needsRehold(false, false, false, false, true, true));   // no countdown yet (the first hold does it)
        CHECK(!RaceStartGate::needsRehold(true, true, false, false, true, true));     // racing: free
        CHECK(!RaceStartGate::needsRehold(true, false, true, false, true, true));     // finished
        CHECK(!RaceStartGate::needsRehold(true, false, false, true, true, true));     // already held
        CHECK(!RaceStartGate::needsRehold(true, false, false, false, false, true));   // not in Action yet
        CHECK(!RaceStartGate::needsRehold(true, false, false, false, true, false));   // dead
    }
    {   // the READY message is a serial only
        uint8_t b[4]; MpWriter w(b, sizeof b); w.u16(0x1234);
        uint16_t s; CHECK(readSerialOnly(b, w.n, &s) && s == 0x1234 && !readSerialOnly(b, 3, &s) && !readSerialOnly(b, 1, &s));
        CHECK(MP_MSG_RACEREADY == 0x47 && !mp_msg_host_only(MP_MSG_RACEREADY) && !mp_msg_host_relays(MP_MSG_RACEREADY));
    }
}

// starfix round 3: a console that loads runs NOTHING of the multiplayer pump: no message read, no flow clock tick (MpFlow::update and holdSceneStart both skip it,
// MpGame.cpp testStalled). 20 s with no tick between the MOVE and the STAR, then the first pump (one capped step): the claim still credits; real pumping time ages it.
static void testStallNoPumpNoClock() {
    FlowClock clk;
    for (int64_t t = 0; t <= 3000; t += 100) clk.tick(t, kFlowStepCapMs);           // pumping before the MOVE
    LeftScene L = leftEgg1(41, clk.t);                                               // the MOVE (the stall starts here: nothing below ticks the clock)
    const Star s = lateStar(41, 1, 1);
    const int64_t before = clk.t;
    // 20 s stall: no pump, no tick (the guest's STAR, sent 1 s after the MOVE, waits in the core's queue)
    const int64_t afterStall = clk.tick(3000 + 20000, kFlowStepCapMs);               // the first pump after the load: one capped step
    CHECK(afterStall - before == kFlowStepCapMs && afterStall - L.leftAtMs <= kLateClaimMs);
    CHECK(lateClaimJudge(L, s, 1, true, afterStall, true) == LateResult::Credit);
    // and the clock goes on with real pumping time only: 5 s later the window is over
    int64_t real = 23000;
    for (int i = 0; i < 50; i++) { real += 100; clk.tick(real, kFlowStepCapMs); }
    CHECK(lateClaimJudge(L, s, 1, true, clk.t, true) == LateResult::TooLate);
}

// joinfix: a host whose flow session starts with a guest already present (both consoles in play when the session opens, the in-play autopilot)
// sees no join edge in presenceCheck, so the guest got no progress snapshot: the session start offers it, exactly when the join edge would have.
static void testOfferAtSessionStart() {
    CHECK(offerSnapshotAtSessionStart(true, true, false, true));       // host, scene up, guest here: offer
    CHECK(!offerSnapshotAtSessionStart(true, true, false, false));     // alone: nothing to send (a later join edge offers)
    CHECK(!offerSnapshotAtSessionStart(false, true, false, true));     // a guest never offers
    CHECK(!offerSnapshotAtSessionStart(true, false, false, true));     // no scene yet: the host's file is not loaded
    CHECK(!offerSnapshotAtSessionStart(true, true, true, true));       // at the file select: MOVE_AFTER_LOAD offers after the file loads
}

int main() {
    testRaceStartGate();
    testOfferAtSessionStart();
    testRefusals();
    testSerial();
    testWhere();
    testHiddenScenarioMoves();
    testSmallCodecs();
    testResultsCodec();
    testFuzz();
    testBarrier();
    testSkipVote();
    testLedger();
    testStarMask();
    testStarCodecs();
    testLateClaim();
    testLateOrder();
    testStarPending();
    testLedgerErase();
    testStallNoPumpNoClock();
    testGalaxyHash();
    testFlowClockAndSender();
    testPendingAndReplay();
    testHostChange();
    testRateAndReplies();
    testDroppedVerdictOnClaimer();
    testReplayGrandAndExpiry();
    testGlitchfix();
    testRace();
    testDigest();
    printf("flow_test: %d checks, %d failures\n", sChecks, sFails);
    return sFails ? 1 : 0;
}
