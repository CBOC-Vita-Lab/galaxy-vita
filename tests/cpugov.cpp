// honey25 host test: platform/include/port/cpugov.h (the far-draw cut's governor). tools/test_cpugov.sh builds it with ASan + UBSan.
// Negative controls are inside: each must FAIL its property when the guarded behaviour is taken away, or the test proves nothing.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "port/cpugov.h"
using namespace port::cpugov;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Config Cfg(int maxLevel) { Config c; c.maxLevel = maxLevel; return c; }
// a window of 300 pictures (5 s at 60) with `late` of them late, split by the stage that made them late
static Window Win(uint32_t late, uint32_t gpu, uint32_t game, uint32_t renderer, uint32_t front, uint32_t other = 0, uint32_t pictures = 300, uint32_t stalls = 0, uint32_t quiet = 0) {
    Window w; w.pictures = pictures; w.late = late; w.gpu = gpu; w.game = game; w.renderer = renderer; w.front = front; w.other = other; w.stalls = stalls; w.quiet = quiet; return w;
}
// the opening view of Honeyhive as the c76 log counted it: 166 pictures, 165 late, GPU 41, game 34, renderer 1, front end 89
static Window Honeyhive() { return Win(165, 41, 34, 1, 89, 0, 166); }

static void TestLevelsMonotone() {
    CHECK(LevelOf(0).lines == 0.0f && LevelOf(0).kinds == 0 && LevelOf(0).hold == 0 && LevelOf(0).flush == 0, "level 0 asks nothing");
    for (int l = 1; l <= kMaxLevel; ++l) {
        const Level a = LevelOf(l - 1), b = LevelOf(l);
        CHECK(b.lines >= a.lines, "lines monotone at %d", l);
        CHECK((b.kinds & a.kinds) == a.kinds, "classes only added at %d (%d -> %d)", l, a.kinds, b.kinds);
        CHECK(b.hold >= a.hold && b.flush >= a.flush, "hold / flush monotone at %d", l);
        CHECK(b.lines > a.lines || b.kinds != a.kinds, "every level adds something at %d", l);
        CHECK(b.lines <= 64.0f && b.lines > 0.0f, "inside the far-draw knob's range (1..64)");
        CHECK(b.kinds != 0 && (b.kinds & ~31) == 0, "kinds a ClassBit mask");
    }
    CHECK(LevelOf(-3).lines == 0.0f && LevelOf(99).lines == LevelOf(kMaxLevel).lines, "out of range levels clamp");
}

static void TestJudge() {
    const Config c = Cfg(3);
    CHECK(Judge(c, Honeyhive()) == Verdict::Hot, "the Honeyhive window is hot");
    CHECK(Judge(c, Win(0, 0, 0, 0, 0)) == Verdict::Calm, "no late picture: calm");
    CHECK(Judge(c, Win(9, 0, 9, 0, 0)) == Verdict::Calm, "3 pct late is calm (boundary: at most calmPct)");
    CHECK(Judge(c, Win(10, 0, 10, 0, 0)) == Verdict::Middle, "3.3 pct late is neither");
    CHECK(Judge(c, Win(44, 0, 44, 0, 0)) == Verdict::Middle, "14.7 pct late is neither");
    CHECK(Judge(c, Win(45, 0, 45, 0, 0)) == Verdict::Hot, "15 pct late and the game thread's: hot (boundary: at least latePct)");
    CHECK(Judge(c, Win(200, 180, 10, 5, 5)) == Verdict::Middle, "late by the GPU: not this lever's");
    CHECK(Judge(c, Win(200, 100, 50, 0, 50)) == Verdict::Hot, "exactly half CPU: hot (boundary: at least cpuPct)");
    CHECK(Judge(c, Win(200, 101, 50, 0, 49)) == Verdict::Middle, "just under half CPU: not hot");
    CHECK(Judge(c, Win(200, 0, 0, 0, 0, 200)) == Verdict::Middle, "late by 'none of these' (waits, pacing): not hot");
    CHECK(Judge(c, Win(200, 0, 100, 0, 0, 100)) == Verdict::Hot, "half the late ones the game thread's, half waits: hot");
    CHECK(Judge(c, Win(200, 0, 200, 0, 0, 0, 59)) == Verdict::Ignored, "too few pictures");
    CHECK(Judge(c, Win(200, 0, 200, 0, 0, 0, 300, 0, 100)) == Verdict::Ignored, "a quarter of the window quiet (pause, fade): ignored");
    CHECK(Judge(c, Win(200, 0, 200, 0, 0, 0, 300, 75, 0)) == Verdict::Ignored, "stalls (a load): ignored");
    CHECK(Judge(c, Win(200, 0, 200, 0, 0, 0, 300, 10, 10)) == Verdict::Hot, "a little of both: still read");
    // not a subtraction trap: unsigned counters at their extremes
    Window big = Win(0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0, 0, 0, 0xFFFFFFFFu);
    CHECK(Judge(c, big) == Verdict::Hot, "extreme counters do not wrap");
}

static void TestRampAndHold() {
    Governor g(Cfg(5));
    int steps = 0;
    for (int i = 0; i < 20; ++i) {
        const int before = g.level();
        const bool ch = g.Feed(Honeyhive());
        CHECK(ch == (g.level() != before), "Feed's answer is whether the level changed");
        CHECK(g.level() - before <= 1, "one level a window");
        if (ch) ++steps;
    }
    CHECK(g.level() == 5 && steps == 5, "hot windows climb to the top (5 s a step) and stay: level %d steps %d", g.level(), steps);
    Governor h(Cfg(2));
    for (int i = 0; i < 10; ++i) h.Feed(Honeyhive());
    CHECK(h.level() == 2, "GXV_CPUGOV=2 stops at 2");
    Config slow = Cfg(5); slow.upWindows = 3;
    Governor s(slow);
    s.Feed(Honeyhive()); s.Feed(Honeyhive());
    CHECK(s.level() == 0, "two hot windows are not three");
    s.Feed(Honeyhive());
    CHECK(s.level() == 1, "the third makes the step");
    s.Feed(Win(0, 0, 0, 0, 0)); s.Feed(Honeyhive()); s.Feed(Honeyhive());
    CHECK(s.level() == 1, "a calm window in between breaks the run");
}

static void TestOffAndGpu() {
    Governor off(Cfg(0));
    for (int i = 0; i < 10; ++i) CHECK(!off.Feed(Honeyhive()), "off never changes");
    CHECK(off.level() == 0, "off stays 0");
    Governor g(Cfg(5));
    for (int i = 0; i < 30; ++i) g.Feed(Win(250, 230, 10, 5, 5, 0, 300));
    CHECK(g.level() == 0, "a GPU-late stage never raises it (autoq's lever)");
    for (int i = 0; i < 30; ++i) g.Feed(Win(0, 0, 0, 0, 0));
    CHECK(g.level() == 0, "a calm stage stays at 0");
    // negative control: a governor that did not read the cause would climb here
    Config blind = Cfg(5); blind.cpuPct = 0;
    Governor b(blind);
    for (int i = 0; i < 6; ++i) b.Feed(Win(250, 230, 10, 5, 5, 0, 300));
    CHECK(b.level() == 5, "NEGATIVE CONTROL: with cpuPct 0 the GPU-late stage does climb (the test can tell)");
}

static void TestLowering() {
    Governor g(Cfg(5));
    for (int i = 0; i < 5; ++i) g.Feed(Honeyhive());
    CHECK(g.level() == 5, "setup");
    for (int i = 0; i < 5; ++i) g.Feed(Win(0, 0, 0, 0, 0));
    CHECK(g.level() == 5, "five calm windows are not six");
    g.Feed(Win(20, 0, 20, 0, 0));  // 6.7 %: neither
    CHECK(g.calmRun() == 0, "a middle window resets the calm run");
    for (int i = 0; i < 5; ++i) g.Feed(Win(0, 0, 0, 0, 0));
    CHECK(g.level() == 5, "so the run starts again");
    CHECK(g.Feed(Win(0, 0, 0, 0, 0)) && g.level() == 4, "the sixth calm window lowers one level");
    for (int i = 0; i < 6; ++i) g.Feed(Win(0, 0, 0, 0, 0));
    CHECK(g.level() == 3, "and the next six the next");
    // ignored windows (a load, a pause) neither count nor break
    Governor q(Cfg(3));
    q.Feed(Honeyhive()); q.Feed(Honeyhive()); q.Feed(Honeyhive());
    for (int i = 0; i < 5; ++i) q.Feed(Win(0, 0, 0, 0, 0));
    q.Feed(Win(0, 0, 0, 0, 0, 0, 300, 0, 300));  // a pause
    CHECK(q.calmRun() == 5 && q.level() == 3, "an ignored window keeps the run: %d", q.calmRun());
    q.Feed(Win(0, 0, 0, 0, 0));
    CHECK(q.level() == 2, "the sixth real calm window lowers");
}

static void TestBackoff() {
    // the limit cycle: stage needs level 3; at 3 it is calm, at 2 it is hot again. Without the backoff the level would go 3-2-3-2 every 6 + 1 windows.
    auto run = [](bool backoff, int windows, int* changes, int* downs) {
        Config c = Cfg(5);
        if (!backoff) c.backoffSpan = 0;
        Governor g(c);
        *changes = 0; *downs = 0;
        for (int i = 0; i < windows; ++i) {
            const int need = 3;
            const Window w = g.level() >= need ? Win(0, 0, 0, 0, 0) : Honeyhive();
            const int before = g.level();
            if (g.Feed(w)) { ++*changes; if (g.level() < before) ++*downs; }
        }
    };
    int chOn, dnOn, chOff, dnOff;
    run(true, 720, &chOn, &dnOn);   // one hour
    run(false, 720, &chOff, &dnOff);
    CHECK(dnOff > 20, "NEGATIVE CONTROL: without the backoff the level keeps going down and up (%d lowerings an hour)", dnOff);
    CHECK(dnOn <= 10 && dnOff >= 5 * dnOn, "with the backoff it settles to a probe every ten minutes: %d lowerings an hour against %d", dnOn, dnOff);
    Governor g(Cfg(5));
    int prevNeed = g.downNeed();
    for (int i = 0; i < 400; ++i) {
        const Window w = g.level() >= 3 ? Win(0, 0, 0, 0, 0) : Honeyhive();
        g.Feed(w);
        CHECK(g.downNeed() >= prevNeed && g.downNeed() <= g.config().downWindowsMax, "the wait only grows, to its cap");
        prevNeed = g.downNeed();
    }
    CHECK(g.downNeed() == g.config().downWindowsMax || g.level() == 3, "capped or settled");
}

static void TestResetAndUpdate() {
    Governor g(Cfg(4));
    for (int i = 0; i < 4; ++i) g.Feed(Honeyhive());
    CHECK(g.level() == 4, "setup");
    g.Reset();
    CHECK(g.level() == 0 && g.hotRun() == 0 && g.calmRun() == 0 && g.downNeed() == 6, "a stage load starts again");
    for (int i = 0; i < 4; ++i) g.Feed(Honeyhive());
    Config lower = Cfg(2);
    g.Update(lower);
    CHECK(g.level() == 2, "a preset that lowers the cap acts at once (level() is clamped)");
    Config off = Cfg(0);
    g.Update(off);
    CHECK(g.level() == 0, "a preset that turns it off acts at once");
    Config on = Cfg(4);
    g.Update(on);
    CHECK(g.level() == 4, "and back on: the state was kept");
}

static void TestCombine() {
    // level 0 is the static values exactly
    for (float lines : {0.0f, 6.0f, 8.0f, 16.0f})
        for (int kinds : {1, 3, 31})
            for (int hold : {0, 20, 60})
                for (int flush : {0, 1}) {
                    const Combined c = Combine(lines, kinds, hold, flush, 0);
                    CHECK(c.lines == lines && c.kinds == kinds && c.hold == hold && c.flush == flush, "level 0 changes nothing");
                }
    // static off (lines 0): the governor's values stand alone (the default kinds 1 of the static knob must not leak in)
    const Combined a = Combine(0.0f, 1, 0, 0, 2);
    CHECK(a.lines == LevelOf(2).lines && a.kinds == LevelOf(2).kinds && a.hold == 20 && a.flush == 1, "static off: the level alone");
    // static on: the larger lines, the union of classes, the longer hold
    const Combined b = Combine(12.0f, 4, 40, 0, 2);
    CHECK(b.lines == 12.0f && b.kinds == (4 | LevelOf(2).kinds) && b.hold == 40 && b.flush == 1, "static on: combined");
    const Combined c = Combine(4.0f, 4, 0, 1, 5);
    CHECK(c.lines == LevelOf(5).lines && c.kinds == 31 && c.flush == 1, "the governor's larger lines win");
    // never gives back what the static knob took
    for (int l = 0; l <= kMaxLevel; ++l) {
        const Combined d = Combine(10.0f, 8, 30, 1, l);
        CHECK(d.lines >= 10.0f && (d.kinds & 8) == 8 && d.hold >= 30 && d.flush == 1, "static values are a floor at level %d", l);
    }
    // monotone in the level for any static input
    for (float lines : {0.0f, 7.0f})
        for (int l = 1; l <= kMaxLevel; ++l) {
            const Combined x = Combine(lines, 1, 0, 0, l - 1), y = Combine(lines, 1, 0, 0, l);
            CHECK(y.lines >= x.lines && (y.kinds & x.kinds) == (l == 1 && lines == 0.0f ? (x.kinds & y.kinds) : x.kinds), "monotone at %d", l);
        }
}

static void TestHoneyhiveScenario() {
    // one run of the opening view: hot for 40 s, then the player walks into a calmer part (3 % late) for two minutes, then a load (ignored), then hot again
    Governor g(Cfg(5));
    std::vector<int> trace;
    for (int i = 0; i < 8; ++i) { g.Feed(Honeyhive()); trace.push_back(g.level()); }
    CHECK(trace[4] == 5 && trace[7] == 5, "the top in 25 s");
    for (int i = 0; i < 24; ++i) g.Feed(Win(6, 0, 6, 0, 0));  // 2 %: calm
    CHECK(g.level() == 1, "two minutes of calm give back four levels (one every 30 s): level %d", g.level());
    for (int i = 0; i < 3; ++i) g.Feed(Win(0, 0, 0, 0, 0, 0, 10, 4, 0));  // a load
    CHECK(g.level() == 1, "a load changes nothing");
    g.Feed(Honeyhive());
    CHECK(g.level() == 2, "hot again: up");
}

int main() {
    TestLevelsMonotone();
    TestJudge();
    TestRampAndHold();
    TestOffAndGpu();
    TestLowering();
    TestBackoff();
    TestResetAndUpdate();
    TestCombine();
    TestHoneyhiveScenario();
    if (fails) { std::printf("cpugov test: %d FAILED\n", fails); return 1; }
    std::printf("cpugov test: all checks passed\n");
    return 0;
}
