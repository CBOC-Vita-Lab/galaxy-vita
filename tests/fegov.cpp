// fe26 host test: platform/include/port/fegov.h (the front-end governor). tools/test_fegov.sh builds it with ASan + UBSan.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "port/fegov.h"
using namespace port::fegov;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static Window Win(float ms, uint32_t pics = 150, uint32_t quiet = 0, uint32_t stalls = 0) {
    Window w;
    w.pictures = pics;
    w.frontPics = pics;
    w.frontSumUs = static_cast<uint64_t>(ms * 1000.0 * pics);
    w.quiet = quiet;
    w.stalls = stalls;
    return w;
}
static Config Cfg(int maxLevel) {
    Config c;
    c.maxLevel = maxLevel;
    return c;
}

// A plant: the front end's mean ms at a level is base - sum of the gains of the levels held (+ noise), as the console would show it.
struct Plant {
    float base, g1, g2, noise;
    uint32_t seed = 12345;
    float Ms(int level) {
        seed = seed * 1664525u + 1013904223u;
        const float r = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 2.0f * noise;
        float v = base + r;
        if (level >= 1) v -= g1;
        if (level >= 2) v -= g2;
        return v;
    }
};
static int Run(Governor& g, Plant& p, int windows, int* maxLevelSeen = nullptr) {
    int level = g.level(), seen = 0;
    for (int i = 0; i < windows; ++i) {
        g.Feed(Win(p.Ms(level)));
        level = g.level();
        if (level > seen) seen = level;
    }
    if (maxLevelSeen) *maxLevelSeen = seen;
    return level;
}

// revB: the game governor's measure against a counter that is brought up to date only when the thread leaves its core (the Vita's runClocks). A turn: wait for the front
// end (a core leave, 1 ms in), then 15 ms of work with no leave, then end of turn (no leave yet), then the retrace wait (a leave). Samples at the END of a turn see only
// the 1 ms; samples at consecutive BEGINs (after the retrace wait) see the whole 16.
static void MeterTest() {
    int64_t trueCpu = 100000, shown = 100000;  // microseconds; `shown` is the lazy counter
    int64_t prevBegin = 0, endSumOld = 0, begSum = 0;
    bool prevDrawn = false;
    int counted = 0;
    for (int turn = 0; turn < 300; ++turn) {
        const int64_t begin = shown;                  // read right after the retrace wait: up to date
        int64_t us;
        if (TurnCpuDelta(prevBegin, begin, prevDrawn, 100000, &us)) { begSum += us; ++counted; }
        prevBegin = begin;
        trueCpu += 1000; shown = trueCpu;             // work up to the front-end wait; the wait is a leave
        const int64_t startOld = begin;
        trueCpu += 15000;                             // the rest of the turn: no leave, the counter does not move
        endSumOld += shown - startOld;                // the old sample: end - begin
        prevDrawn = true;
        shown = trueCpu;                              // the retrace wait: a leave, the counter catches up
    }
    CHECK(counted == 299);
    CHECK(begSum == 299 * 16000);                     // exact: 16 ms a turn
    CHECK(endSumOld < 300 * 2000);                    // the old measure saw ~1 ms a turn
    int64_t us = 0;
    CHECK(!TurnCpuDelta(0, 5000, true, 100000, &us));        // no previous sample (a stage's load)
    CHECK(!TurnCpuDelta(1000, 500, true, 100000, &us));      // the counter went back
    CHECK(!TurnCpuDelta(1000, 5000, false, 100000, &us));    // the turn was not drawn
    CHECK(!TurnCpuDelta(1000, 500000, true, 100000, &us));   // a load / pause / hitch is left out
    CHECK(TurnCpuDelta(1000, 17000, true, 100000, &us) && us == 16000);
}

int main() {
    MeterTest();
    // ---- off: never changes
    {
        Governor g(Cfg(0));
        for (int i = 0; i < 50; ++i) CHECK(!g.Feed(Win(30.0f)));
        CHECK(g.level() == 0);
    }
    // ---- windows that say nothing
    {
        Governor g(Cfg(2));
        for (int i = 0; i < 20; ++i) g.Feed(Win(30.0f, 40));          // too few pictures
        CHECK(g.level() == 0 && g.hotRun() == 0);
        for (int i = 0; i < 20; ++i) g.Feed(Win(30.0f, 150, 80, 0));  // a third of it was a pause
        CHECK(g.level() == 0 && g.hotRun() == 0);
        for (int i = 0; i < 20; ++i) g.Feed(Win(30.0f, 150, 0, 60));  // stalls
        CHECK(g.level() == 0);
        // an ignored window neither counts nor breaks a run
        g.Feed(Win(17.0f));
        CHECK(g.hotRun() == 1);
        g.Feed(Win(30.0f, 10));
        CHECK(g.hotRun() == 1 && g.level() == 0);
        CHECK(g.Feed(Win(17.0f)) && g.level() == 1);
    }
    // ---- up: two hot windows a step, capped at maxLevel
    {
        Governor g(Cfg(2));
        CHECK(!g.Feed(Win(18.0f))); CHECK(g.Feed(Win(18.0f))); CHECK(g.level() == 1);
        CHECK(!g.Feed(Win(18.0f))); CHECK(g.Feed(Win(18.0f))); CHECK(g.level() == 2);
        for (int i = 0; i < 10; ++i) CHECK(!g.Feed(Win(25.0f)));
        CHECK(g.level() == 2);
        Governor h(Cfg(1));
        for (int i = 0; i < 10; ++i) h.Feed(Win(25.0f));
        CHECK(h.level() == 1);
        // a middle window breaks the run
        Governor k(Cfg(2));
        k.Feed(Win(18.0f)); k.Feed(Win(15.0f)); k.Feed(Win(18.0f));
        CHECK(k.level() == 0);
    }
    // ---- a plant that needs level 1 only: settles at 1, no cycling
    {
        Governor g(Cfg(2));
        Plant p{16.6f, 1.4f, 1.0f, 0.2f};
        int seen = 0;
        const int lv = Run(g, p, 400, &seen);
        CHECK(lv == 1);
        CHECK(seen == 1);
        CHECK(g.changes() <= 3);  // 1 raise, at most one probing lowering and its re-raise
    }
    // ---- a plant that needs level 2
    {
        Governor g(Cfg(2));
        Plant p{19.0f, 1.8f, 1.9f, 0.2f};
        int seen = 0;
        const int lv = Run(g, p, 300, &seen);
        CHECK(lv == 2);
        CHECK(g.changes() <= 4);
    }
    // ---- a plant that is fine: stays 0
    {
        Governor g(Cfg(2));
        Plant p{14.0f, 1.0f, 1.0f, 0.3f};
        CHECK(Run(g, p, 200) == 0 && g.changes() == 0);
    }
    // ---- a plant whose cuts give nothing (the limit is elsewhere): the governor goes to its maximum, learns the gains are ~0 and does not come back down while hot
    {
        Governor g(Cfg(2));
        Plant p{18.0f, 0.0f, 0.0f, 0.1f};
        CHECK(Run(g, p, 100) == 2);
        CHECK(g.GainKnown(1) && g.GainOf(1) < 0.3f);
    }
    // ---- the gain is measured at the raise
    {
        Governor g(Cfg(2));
        Plant p{17.0f, 1.5f, 1.0f, 0.0f};
        Run(g, p, 12);
        CHECK(g.level() == 1);
        CHECK(g.GainKnown(1) && std::fabs(g.GainOf(1) - 1.5f) < 0.05f);
    }
    // ---- lowering needs headroom for the gain: base 16.0 gain 1.0 -> 15.0 at level 1; giving it back would be 16.0 > 15.5, so it stays
    {
        Governor g(Cfg(2));
        Plant p{16.0f, 1.0f, 1.0f, 0.0f};
        CHECK(Run(g, p, 300) == 1);
        CHECK(g.changes() == 1);
    }
    // ---- a stage that gets easier: comes down after 6 calm windows, one level at a time
    {
        Governor g(Cfg(2));
        for (int i = 0; i < 8; ++i) g.Feed(Win(19.0f));
        CHECK(g.level() == 2);
        int downAt = -1;
        for (int i = 0; i < 20 && downAt < 0; ++i)
            if (g.Feed(Win(10.0f))) downAt = i + 1;
        CHECK(downAt == 6);
        CHECK(g.level() == 1);
        for (int i = 0; i < 20; ++i) g.Feed(Win(10.0f));
        CHECK(g.level() == 0);
    }
    // ---- back-off: a hot window right after a lowering doubles the wait
    {
        Governor g(Cfg(2));
        for (int i = 0; i < 4; ++i) g.Feed(Win(19.0f));
        CHECK(g.level() == 2);
        for (int i = 0; i < 6; ++i) g.Feed(Win(8.0f));
        CHECK(g.level() == 1 && g.downNeed() == 6);
        g.Feed(Win(18.0f));
        CHECK(g.downNeed() == 12);
        for (int i = 0; i < 400; ++i) g.Feed(Win(18.0f));
        CHECK(g.downNeed() <= g.config().downWindowsMax);
    }
    // ---- negative control: with no band, no gain estimate and no back-off the same plant cycles, and the real governor does not
    {
        Plant a{15.9f, 1.2f, 1.0f, 0.0f}, b = a;
        Governor real(Cfg(2));
        Config naive = Cfg(2);
        naive.bandMs = 0.0f; naive.defaultGainMs = 0.0f; naive.backoffSpan = 0; naive.upWindows = 1; naive.downWindows = 1;
        Governor bad(naive);
        // gain is never learned by `bad` because defaultGain 0 and the measured one is used only in Calm: emulate a naive controller by hand
        int cyc = 0, level = 0;
        for (int i = 0; i < 200; ++i) {   // naive: up when above the budget, down when below it
            const float ms = b.Ms(level);
            const int want = ms > 15.5f ? level + 1 : level - 1;
            const int nl = want < 0 ? 0 : (want > 2 ? 2 : want);
            if (nl != level) ++cyc;
            level = nl;
        }
        Run(real, a, 200);
        CHECK(cyc > 20);               // the naive controller flips every window
        CHECK(real.changes() <= 3);
        (void)bad;
    }
    // ---- Update keeps the state; Reset clears the level and the gains
    {
        Governor g(Cfg(2));
        for (int i = 0; i < 4; ++i) g.Feed(Win(19.0f));
        CHECK(g.level() == 2);
        Config c = g.config();
        c.maxLevel = 1;
        g.Update(c);
        CHECK(g.level() == 1);  // the cap applies at once
        c.maxLevel = 0;
        g.Update(c);
        CHECK(g.level() == 0);
        g.Reset();
        CHECK(g.level() == 0 && g.changes() == 0 && !g.GainKnown(1));
    }
    // ---- cpu2-26: level 3, and two governors on one level (the game thread's and the front end's; the cuts read the larger)
    {
        CHECK(kMaxLevel == 4);   // (far30: 4 = the last resort)
        Governor g(Cfg(3));
        for (int i = 0; i < 30; ++i) g.Feed(Win(30.0f));
        CHECK(g.level() == 3);                   // a stage that needs everything gets level 3, and no more
        Governor h(Cfg(2));
        for (int i = 0; i < 30; ++i) h.Feed(Win(30.0f));
        CHECK(h.level() == 2);                   // the cap still holds
        // a level-3 gain is learned at the raise to 3
        Governor k(Cfg(3));
        struct P3 { float base, g[4]; float Ms(int l) const { float v = base; for (int i = 1; i <= l; ++i) v -= g[i]; return v; } } p3{20.0f, {0, 1.5f, 1.5f, 1.2f}};
        int lv = 0;
        for (int i = 0; i < 40; ++i) { k.Feed(Win(p3.Ms(lv))); lv = k.level(); }
        CHECK(lv == 3 && k.GainKnown(3) && std::fabs(k.GainOf(3) - 1.2f) < 0.06f);
    }
    {
        // Two plants share one level L = max(game, fe). game(L) = D - sum of its gains, fe(L) likewise; both governors see the same L they jointly made.
        struct Joint {
            float d, f; float dg[4], fg[4];
            float Game(int l) const { float v = d; for (int i = 1; i <= l; ++i) v -= dg[i]; return v; }
            float Fe(int l) const { float v = f; for (int i = 1; i <= l; ++i) v -= fg[i]; return v; }
        };
        auto sim = [&](const Joint& j, int windows, int* lGame, int* lFe) {
            Governor gg(Cfg(3)), ff(Cfg(3));
            int L = 0;
            for (int i = 0; i < windows; ++i) {
                gg.Feed(Win(j.Game(L)));
                ff.Feed(Win(j.Fe(L)));
                L = gg.level() > ff.level() ? gg.level() : ff.level();
            }
            *lGame = gg.level(); *lFe = ff.level();
            return L;
        };
        int a, b;
        // Honeyhive-like: the game thread needs 3, the front end 2: the shared level is 3
        Joint hh{19.5f, 18.0f, {0, 1.8f, 1.8f, 1.2f}, {0, 1.2f, 1.2f, 0.8f}};
        CHECK(sim(hh, 300, &a, &b) == 3 && a == 3);
        // front end bound, game thread fine: the game governor never raises and never pulls the front end's level down
        Joint fe{13.0f, 17.5f, {0, 0.8f, 0.8f, 0.5f}, {0, 1.2f, 1.2f, 0.8f}};
        CHECK(sim(fe, 300, &a, &b) == 2 && a == 0 && b == 2);
        // both fine: stays 0
        Joint ok{14.0f, 14.5f, {0, 0.8f, 0.8f, 0.5f}, {0, 1.2f, 1.2f, 0.8f}};
        CHECK(sim(ok, 200, &a, &b) == 0);
        // a stage that eases off: both come down
        Joint eas{19.5f, 18.0f, {0, 1.8f, 1.8f, 1.2f}, {0, 1.2f, 1.2f, 0.8f}};
        int L = sim(eas, 100, &a, &b);
        CHECK(L == 3);
        Governor gg(Cfg(3)); Governor ff(Cfg(3));
        // restart them at the level they reached, then feed an easy stage
        for (int i = 0; i < 40; ++i) { gg.Feed(Win(30.0f)); ff.Feed(Win(30.0f)); }
        CHECK(gg.level() == 3 && ff.level() == 3);
        for (int i = 0; i < 80; ++i) { gg.Feed(Win(9.0f)); ff.Feed(Win(9.0f)); }
        CHECK(gg.level() == 0 && ff.level() == 0);
    }
    // ---- combine helpers
    {
        CHECK(CountMax(0, 0) == 0 && CountMax(3, 0) == 3 && CountMax(0, 2) == 2 && CountMax(4, 2) == 4 && CountMax(2, 3) == 3 && CountMax(1, 2) == 2);
        CHECK(DistFor(0, 3000, 0, 2200) == 3000 && DistFor(0, 3000, 2, 2200) == 2200 && DistFor(2, 3000, 3, 2200) == 2200 && DistFor(3, 1800, 2, 3000) == 1800);
        CHECK(SizeFor(0, 12, 0, 24) == 12 && SizeFor(0, 12, 2, 24) == 24 && SizeFor(2, 30, 3, 24) == 30 && SizeFor(2, 12, 3, 24) == 24);
        CHECK(KeepMin(0.0f, 0.0f) == 0.0f && KeepMin(0.5f, 0.0f) == 0.5f && KeepMin(0.0f, 0.35f) == 0.35f && KeepMin(0.3f, 0.6f) == 0.3f && KeepMin(0.6f, 0.35f) == 0.35f);
        CHECK(FarDrawLevel(0) == 0 && FarDrawLevel(1) == 3 && FarDrawLevel(2) == 4 && FarDrawLevel(3) == 5);
        // level 0 changes nothing: every combine at gov n = 0/1 returns the static value
        for (int n = 0; n < 5; ++n) CHECK(CountMax(n, 0) == n && CountMax(n, 1) == n);
    }
    // ---- c161: the floor (the stage's static level less one): the governor starts there, its first raise is the step above, a give-back stops at the floor
    {
        Config c = Cfg(3);
        c.minLevel = 2;
        Governor g(c);
        CHECK(g.level() == 2);
        for (int i = 0; i < c.upWindows; ++i) g.Feed(Win(30.0f));     // hot: one step, straight to the real one
        CHECK(g.level() == 3);
        for (int i = 0; i < 400; ++i) g.Feed(Win(5.0f));              // calm for a long time
        CHECK(g.level() == 2);                                         // never under the floor
        c.minLevel = 5;                                                // a floor above the cap is the cap
        g.Configure(c);
        CHECK(g.level() == 3);
        c.minLevel = 0;                                                // no floor: as before
        g.Configure(c);
        CHECK(g.level() == 0);
        c.minLevel = 1;                                                // a floor that rises in play lifts the level at the next window's Update
        g.Update(c);
        CHECK(g.level() == 1);
    }
    if (fails) { std::printf("fegov test: %d FAILED\n", fails); return 1; }
    std::printf("fegov test: all checks passed\n");
    return 0;
}
