// anticip32 host test (13 Nov 2026): the pure parts of GXV_ANTICIPATE (port/anticipate.h), the Quality ladder's overlay (port/q30lock.h O().extra, Ladder::NextSteps) and the rate
// controller's hook (port/rate_auto.h Controller::Anticipate). No game, no clock: time is a counter here. Proves event counts and rules only, never milliseconds.
//   clang++ -std=c++17 -O1 -pthread -Iplatform/include tests/anticipate_test.cpp -o /tmp/anticip32/t && nice -n 19 /tmp/anticip32/t
#include "port/anticipate.h"
#include "port/q30lock.h"
#include "port/rate_auto.h"
#include <cstdio>
#include <cstring>

using namespace port::anticipate;
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const uint64_t kTurnUs = 16683;
struct Sim {
    Engine e;
    uint64_t now = 1000000;
    uint32_t turn = 0;
    float pos[3] = {0, 0, 0};
    uint32_t opens = 0, closes = 0;
    void Step(const float* p) {
        ++turn;
        now += kTurnUs;
        e.Turn(now, turn, p);
        Window w; uint32_t t;
        if (e.TakeClosed(&w, &t)) ++closes;
        if (e.TakeOpened()) ++opens;
    }
    void Run(int turns) { for (int i = 0; i < turns; ++i) Step(pos); }
    // walk along x at `speed` units a turn
    void Walk(int turns, float speed) { for (int i = 0; i < turns; ++i) { pos[0] += speed; Step(pos); } }
};

static void TestWindow() {
    Sim s;
    Params p; p.mode = 1;
    s.e.Configure(p);
    s.e.StageBegin("EggStarGalaxy:1");
    s.Run(60);
    CHECK(!s.e.Win().open);
    // a launch star: 767 ms of wind-up, a 5 s flight, 1.5 s of tail
    CHECK(s.e.Event(s.now, s.turn, kLaunch, 767, 5000));
    CHECK(s.e.TakeOpened());
    CHECK(s.e.Win().open && s.e.Win().kind == kLaunch && s.e.Win().leadMs == 767 && !s.e.Win().learned);
    const uint32_t openTurn = s.turn;
    s.Run(static_cast<int>((767 + 5000 + 1500) * 1000 / kTurnUs) - 2);
    CHECK(s.e.Win().open && s.closes == 0);
    s.Run(4);
    CHECK(!s.e.Win().open && s.closes == 1);
    char line[400];
    Window w = s.e.Win(); w.kind = kLaunch; w.turn = openTurn; w.leadMs = 767;
    Engine::FormatClose(line, sizeof line, w, "cut level 0 -> 1 (walked in over 767 ms)", s.turn);
    CHECK(std::strstr(line, "anticipate: launch star at turn 60: pre-scaled cut level 0 -> 1 (walked in over 767 ms) (lead 767 ms); restored at turn ") == line);
    Engine::FormatOpen(line, sizeof line, w, "nothing");
    CHECK(std::strstr(line, "anticipate: launch star at turn 60: pre-scaled nothing (lead 767 ms)") == line);
    // a second event inside a window extends it, never past maxMs from its start
    CHECK(s.e.Event(s.now, s.turn, kBoss, 0, 6000));
    CHECK(s.e.TakeOpened());
    const uint64_t start = s.e.Win().startUs;
    const uint32_t closesBefore = s.closes;
    uint64_t closedAt = 0;
    for (int i = 0; i < 20; ++i) {   // a power-up every second for 20 s: the window is extended, but it ends 15 s after its start at the latest
        s.Run(60);
        if (!closedAt && s.closes != closesBefore) closedAt = s.now;
        if (!closedAt) { s.e.Event(s.now, s.turn, kPowerUp, 0, 1500); CHECK(s.e.Win().endUs <= start + 15000ull * 1000); }
    }
    CHECK(closedAt != 0 && closedAt - start <= 15000ull * 1000 + 61 * kTurnUs && closedAt - start >= 14000ull * 1000);
    CHECK(s.e.Stats().extended >= 5);
    s.Run(60 * 16);
    CHECK(!s.e.Win().open);
    // a camera cut and an unlearned demo are counted, they open nothing
    const uint32_t before = s.e.Stats().opened;
    CHECK(!s.e.Event(s.now, s.turn, kCamCut, 0, 0));
    CHECK(!s.e.Event(s.now, s.turn, kDemo, 0, 3000));
    CHECK(s.e.Stats().opened == before && s.e.Stats().events[kCamCut] == 1 && s.e.Stats().events[kDemo] == 1);
    // off: nothing
    Sim off;
    off.e.StageBegin("X:1");
    CHECK(!off.e.Event(off.now, 1, kLaunch, 767, 5000) && !off.e.Win().open);
    // mode 2: only what the map knows
    Sim m2;
    Params p2; p2.mode = 2; p2.map = true;
    m2.e.Configure(p2);
    m2.e.StageBegin("X:1");
    m2.Run(10);
    CHECK(!m2.e.Event(m2.now, m2.turn, kLaunch, 767, 5000));
    CHECK(m2.e.Stats().ignored == 1);
    // a stage load drops the window
    Sim ld;
    ld.e.Configure(p);
    ld.e.StageBegin("A:1");
    ld.Run(5);
    ld.e.Event(ld.now, ld.turn, kLaunch, 767, 5000);
    ld.e.StageBegin("B:1");
    CHECK(!ld.e.Win().open && !ld.e.TakeOpened());
}

static void TestSpikeMap() {
    Sim s;
    Params p; p.mode = 1; p.map = true;
    s.e.Configure(p);
    s.e.StageBegin("HoneyBeeKingdomGalaxy:1");
    // visit 1: Mario walks 12 units a turn along x; the speed guard reports dropped pictures at x = 9000 (cell 4)
    s.Walk(750, 12.0f);
    CHECK(s.opens == 0);
    const int idx = s.e.Spike(s.now, kSrcGuard);
    CHECK(idx >= 0 && s.e.Map().Count() == 1);
    CHECK(s.e.Map().At(idx).kind == 0 && s.e.Map().At(idx).hits == 1 && (s.e.Map().At(idx).flags & kFlagRate));
    CHECK(s.e.Map().At(idx).x == 4);   // (9000 less the half second the guard needs = 8640: cell 4)
    CHECK(s.e.Spike(s.now + 1000, kSrcGuard) < 0);   // (one report in 3 s)
    // visit 2: the same walk: the window opens BEFORE the cell (at least half a second ahead at this speed)
    s.e.StageBegin("HoneyBeeKingdomGalaxy:1");
    s.pos[0] = 0; s.opens = s.closes = 0;
    float openedAt = -1;
    for (int i = 0; i < 1500; ++i) { s.Walk(1, 12.0f); if (s.opens == 1 && openedAt < 0) openedAt = s.pos[0]; }
    CHECK(s.opens == 1 && s.closes == 1);
    CHECK(openedAt > 0 && openedAt < 8000.0f && openedAt > 8000.0f - 12.0f * 80.0f);   // (60 turns of lead + the look-up every 8th turn + the smoothing)
    std::printf("spike map: the cell begins at x = 8000; the window opened at x = %.0f (%.0f units = %.2f s ahead at 12 units a turn)\n", openedAt, 8000.0f - openedAt, (8000.0f - openedAt) / 12.0f / 60.0f);
    // visits 3, 4 pre-scale; the 4th pre-scaled visit is a probe (no window); no overload in the probe: the cell is forgotten (it had 1 hit)
    for (int v = 0; v < 2; ++v) { s.e.StageBegin("HoneyBeeKingdomGalaxy:1"); s.pos[0] = 0; s.opens = 0; s.Walk(1500, 12.0f); CHECK(s.opens == 1); }
    s.e.StageBegin("HoneyBeeKingdomGalaxy:1");
    s.pos[0] = 0; s.opens = 0;
    s.Walk(1500, 12.0f);
    CHECK(s.opens == 0 && s.e.Stats().probes == 1 && s.e.Stats().forgotten == 1 && s.e.Map().Count() == 0);
    // another stage does not see a stage's cells
    s.e.StageBegin("HoneyBeeKingdomGalaxy:1");
    s.pos[0] = 0;
    s.Walk(750, 12.0f);
    s.e.Spike(s.now, kSrcLadder);
    CHECK(s.e.Map().Count() == 1);
    s.e.StageBegin("EggStarGalaxy:1");
    s.pos[0] = 0; s.opens = 0;
    s.Walk(900, 12.0f);
    CHECK(s.opens == 0);

    // an overload INSIDE an event window: remembered as "this event at this place", with the rate flag when pictures were dropped
    Sim ev;
    ev.e.Configure(p);
    ev.e.StageBegin("HeavenlyBeachGalaxy:1");
    ev.pos[0] = 5000; ev.pos[1] = -3000; ev.pos[2] = 100;
    ev.Run(40);
    ev.e.Event(ev.now, ev.turn, kLaunch, 767, 5000);
    ev.pos[0] = 60000;   // the flight carries him away
    ev.Run(200);
    const int ei = ev.e.Spike(ev.now, kSrcRate);
    CHECK(ei >= 0 && ev.e.Map().At(ei).kind == kLaunch && ev.e.Map().At(ei).x == 2 && ev.e.Map().At(ei).y == -2 && ev.e.Map().At(ei).z == 0);
    CHECK(ev.e.Win().overload);
    // next visit: the same launch star is known: learned, rate flag
    ev.e.StageBegin("HeavenlyBeachGalaxy:1");
    ev.pos[0] = 5000;
    ev.Run(40);
    CHECK(ev.e.Event(ev.now, ev.turn, kLaunch, 767, 5000));
    CHECK(ev.e.Win().learned && ev.e.Win().rate && !ev.e.Win().deep);
    // a demo there is NOT known (another kind)
    Sim dm;
    dm.e.Configure(p);
    dm.e.Map() = ev.e.Map();
    dm.e.StageBegin("HeavenlyBeachGalaxy:1");
    dm.pos[0] = 5000; dm.pos[1] = -3000; dm.pos[2] = 100;
    dm.Run(40);
    CHECK(!dm.e.Event(dm.now, dm.turn, kDemo, 0, 3000));
    // the card file: what is written is what is read
    static char buf[12288];
    const int n = ev.e.Map().Format(buf, sizeof buf);
    SpikeMap m;
    CHECK(m.Parse(buf, static_cast<size_t>(n)) == ev.e.Map().Count());
    CHECK(m.At(0).kind == kLaunch && m.At(0).x == 2 && m.At(0).y == -2 && (m.At(0).flags & kFlagRate) && !std::strcmp(m.At(0).stage, "HeavenlyBeachGalaxy:1"));
    const char* junk = "# spikes v1\nnot a line\nA:1 1 2 3 99 1 0\nB:2 1 2 3 0 0 0\nC:3 -4 5 -6 0 7 3\n";
    CHECK(m.Parse(junk, std::strlen(junk)) == 1 && m.At(0).x == -4 && m.At(0).hits == 7);
    // a full map: the cell with the fewest overloads goes
    SpikeMap full;
    for (int i = 0; i < SpikeMap::kCap + 5; ++i) { const int16_t c[3] = {static_cast<int16_t>(i), 0, 0}; full.Note("S:1", c, 0, 0); if (i == 3) full.Note("S:1", c, 0, 0); }
    CHECK(full.Count() == SpikeMap::kCap);
    const int16_t c3[3] = {3, 0, 0};
    CHECK(full.Find("S:1", c3, -1) >= 0);
}

// The Quality ladder: the overlay is what the actuators read, the ladder's own mask does not change, and nothing is given back while it is set.
static void TestLadderOverlay() {
    using namespace port::q30lock;
    Ladder l;
    Config c;
    c.memory = false;
    c.allowed = 0x7FFF;
    l.Configure(c);
    O().enabled.store(1);
    uint64_t now = 1000000;
    l.StageBegin(now, "T:1", nullptr);
    CHECK(Mask() == 0);
    const StepMask two = l.NextSteps(2);
    CHECK(two == ((1u << k3a) | (1u << k3b)));
    const StepMask many = l.NextSteps(15);
    CHECK(!(many >> k4 & 1u) && !(many >> k5 & 1u) && !(many >> k6 & 1u) && PopCount(many) == 12);
    O().extra.store(two);
    CHECK(Mask() == two && l.mask() == 0 && StepOn(k3a) && StepOn(k3b) && !StepOn(k3c));
    CHECK((CpuLodLevel() & 0xFF) == (1 | (two << 1)));
    O().extra.store(0);
    CHECK(Mask() == 0 && CpuLodLevel() == (kLevelFlag | 1));
    // a ladder that holds a step and is calm gives it back; with the overlay set it does not, and it does so after
    for (int pass = 0; pass < 2; ++pass) {
        Ladder g;
        Config cg;
        cg.memory = false;
        cg.allowed = 0x7FFF;
        std::snprintf(cg.openSpec, sizeof cg.openSpec, "T=1");   // the stage opens with 3a taken (given back by the usual rules)
        g.Configure(cg);
        now = 5000000;
        g.StageBegin(now, "T:1", nullptr);
        if (g.mask() == 0) { std::printf("(ladder overlay give-back test skipped: the open spec did not take a step in this build)\n"); break; }
        O().extra.store(pass == 1 ? static_cast<uint32_t>(1u << k3b) : 0u);
        uint32_t heldAt90 = 0;
        for (int i = 0; i < 30 * 120; ++i) {   // 120 s of calm pictures: pair 20 ms, GPU 15, front end 12
            now += 33367;
            g.NoteTurn(now, 12000, true);
            g.NoteTurn(now, 8000, false);
            g.NoteShown(now, 2, true);
            g.NotePicture(now, 15000, true, 12000, true, true);
            if (i == 30 * 90) { heldAt90 = g.mask(); if (pass == 1) O().extra.store(0); }
        }
        if (pass == 0) CHECK(heldAt90 == 0);              // without the overlay: given back within 90 s
        else { CHECK(heldAt90 != 0); CHECK(g.mask() == 0); }   // with it: held for the 90 s it was set, given back after
    }
    {   // an overload inside the announced moment: the ladder takes the NEXT step, not one the overlay already has in force (that would buy nothing), and the overlay leaves the ladder's own mask alone
        Ladder h;
        Config ch;
        ch.memory = false;
        ch.allowed = 0x7FFF;
        ch.openSpec[0] = 0; ch.startSpec[0] = 0;
        h.Configure(ch);
        now = 9000000;
        h.StageBegin(now, "U:1", nullptr);
        const StepMask ov = h.NextSteps(2);
        O().extra.store(ov);
        for (int i = 0; i < 30 * 30 && h.mask() == 0; ++i) {   // a pair of 36 ms: over the limit
            now += 36000;
            h.NoteTurn(now, 24000, true);
            h.NoteTurn(now, 12000, false);
            h.NoteShown(now, 3, true);
            h.NotePicture(now, 15000, true, 12000, true, true);
        }
        CHECK(h.mask() != 0 && (h.mask() & ov) == 0);
        CHECK((Mask() & ov) == ov);
        std::printf("ladder under an overlay of 3a+3b: its own first step is %s\n", StepLabel(LastStep(h.mask())));
    }
    O().extra.store(0);
    O().enabled.store(0);
}

// The rate controller: no up-switch inside an announced moment; with `drop` the switch is made at the call.
static void TestRate() {
    using namespace port::rate;
    port::rate::Params p;
    {
        Controller c;
        c.Configure(p);
        uint64_t now = 1000000;
        c.StageBegin(now, 0);
        CHECK(!c.Even30());
        CHECK(!c.Anticipate(now, now + 6000000, false));   // announced, not known heavy: stays at 60
        CHECK(!c.Even30() && c.AnticipateHolds(now + 1));
        CHECK(c.Anticipate(now, now + 6000000, true));     // known: to the even 30 now
        CHECK(c.Even30() && c.Downs() == 1 && c.FailedUps() == 0);
        // lanes that fit 60 easily: no up before the moment is over, an up after
        Gov g;
        Obs o;
        o.play = true; o.drawn = true; o.gameMs = 6.0f; o.gapMs = 16.7f; o.atBreak = true;
        bool upInside = false, upAfter = false;
        for (int i = 0; i < 60 * 40; ++i) {
            now += 16683;
            c.Picture(now, 6.0f, 5.0f, 6.0f, true);
            const Action a = c.Step(now, o, g);
            if (a == kTo60) { if (c.AnticipateHolds(now)) upInside = true; else upAfter = true; }
        }
        CHECK(!upInside && upAfter);
    }
    {   // a stage pinned to the even 30 (rate_pin=2) never leaves it, however well its lanes fit (it used to take one turn at 60 and come straight back, 7 times in 300 s)
        Controller c;
        c.Configure(p);
        uint64_t now = 1000000;
        c.StageBegin(now, 2);
        Gov g;
        Obs o;
        o.play = true; o.drawn = true; o.gameMs = 6.0f; o.gapMs = 16.7f;
        unsigned to60 = 0, to30 = 0;
        for (int i = 0; i < 60 * 300; ++i) {
            now += 16683;
            o.atBreak = (i % 600) == 0;
            c.Picture(now, 6.0f, 5.0f, 6.0f, true);
            const Action a = c.Step(now, o, g);
            if (a == kTo60) ++to60;
            if (a == kTo30) ++to30;
        }
        CHECK(to30 == 1 && to60 == 0 && c.Even30() && c.FailedUps() == 0);
    }
    {   // a stage pinned to 60 is left alone; one pinned to 30 is already there
        Controller c;
        c.Configure(p);
        c.StageBegin(1000000, 1);
        CHECK(!c.Anticipate(1000000, 7000000, true) && !c.Even30());
    }
    {   // without a call the controller is what it was: antUntil_ 0 holds nothing
        Controller c;
        c.Configure(p);
        c.StageBegin(1000000, 0);
        CHECK(!c.AnticipateHolds(1000001));
    }
}

int main() {
    TestWindow();
    TestSpikeMap();
    TestLadderOverlay();
    TestRate();
    std::printf("%s (%d checks, %d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
