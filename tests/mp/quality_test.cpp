// Host tests for decomp/include/Game/MP/MpQualityLogic.hpp (perf3: the automatic puppet quality governor).
//   g++ -std=c++20 -fsanitize=address,undefined -g -Wall -Wextra -I decomp/include tests/mp/quality_test.cpp -o /tmp/quality_test && /tmp/quality_test
// (clang++ works the same where its sanitizer runtime is installed.)
#include "Game/MP/MpQualityLogic.hpp"

#include <cstdio>
#include <functional>
#include <vector>

using namespace MpQuality;

static int sFail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            sFail++;                                                     \
        }                                                                \
    } while (0)

namespace {
// optfix: the product default never steps (maxLevel 0); the level rules themselves are tested with the testing ceiling.
Config fullCfg() { Config c; c.maxLevel = LEVEL_MAX; return c; }

// A simulated console: the picture interval and the work estimate depend on the governor's level (a load model).
struct Sim {
    Governor g{fullCfg()};
    uint64_t now = 1000000;
    std::vector<int> levelPerWindow;   // the level after each window
    uint32_t windows = 0, changes = 0;   // (windows: the governor's count, followed)

    explicit Sim(uint32_t targetUs = 33333) { g.setTargetUs(targetUs); }

    // Runs pictures until `n` windows closed. interval(level) / work(level): the model; shown: puppets on screen.
    void run(uint32_t n, const std::function<uint32_t(int)>& interval, const std::function<uint32_t(int)>& work, bool shown = true) {
        const uint32_t target = g.windows() + n;
        while (g.windows() < target) {
            const uint32_t iv = interval(g.level());
            now += iv;
            const int before = g.level();
            const uint32_t changesBefore = g.changes();
            const bool changed = g.onPicture(now, iv, shown, work(g.level()));
            if (changed) {
                changes++;
                CHECK(g.changes() == changesBefore + 1);
                const int d = g.level() - before;
                CHECK(d == 1 || d == -1);   // one step at a time
            }
            if (g.windows() != windows) {
                windows = g.windows();
                levelPerWindow.push_back(g.level());
            }
        }
    }
    uint32_t lastIv = 0;
};

}  // namespace

static void testWindowPercentile() {
    FrameWindow w;
    CHECK(w.percentileUs(95) == 0);
    for (int i = 0; i < 95; i++) w.add(33333);
    for (int i = 0; i < 5; i++) w.add(50000);
    CHECK(w.count() == 100);
    // 95 of 100 within 33.5 ms: p95 is the 33.0-33.5 bin, capped at the max seen when lower
    CHECK(w.percentileUs(95) == 33500);
    CHECK(w.percentileUs(96) == 50000);   // the 96th falls in the 50.0-50.5 bin: capped at the max seen (50.000)
    CHECK(w.maxUs() == 50000);
    w.add(900000);   // beyond the last bin
    CHECK(w.percentileUs(100) == 900000);
    w.reset();
    CHECK(w.count() == 0 && w.maxUs() == 0);
}

static void testParamsMonotone() {
    for (int l = 0; l < LEVEL_MAX; l++) {
        const LevelParams a = paramsFor(l), b = paramsFor(l + 1);
        CHECK(!(b.shadows && !a.shadows));
        CHECK(b.farIn <= a.farIn && b.farOut <= a.farOut);
        CHECK(a.farOut < a.farIn && b.farOut < b.farIn);   // hysteresis
        CHECK(!(a.halfIn > 0 && b.halfIn <= 0));
        CHECK(!(a.nearestTwo && !b.nearestTwo));
    }
    CHECK(paramsFor(0).shadows && paramsFor(0).farIn == 5500.0f && paramsFor(0).halfIn == 0.0f && !paramsFor(0).nearestTwo);
    CHECK(!paramsFor(1).shadows);
    CHECK(paramsFor(2).halfIn > 0.0f && paramsFor(2).halfOut < paramsFor(2).halfIn);
    CHECK(paramsFor(3).nearestTwo);
    CHECK(paramsFor(4).farIn < paramsFor(3).farIn);
}

// Sustained misses: one step per window, down to the last level, never further.
static void testStepPerWindow() {
    Governor g{fullCfg()};
    g.setTargetUs(33333);
    uint64_t now = 1;
    int lastLevel = 0;
    uint32_t seen = 0;
    for (int i = 0; i < 60 * 30; i++) {   // 50 ms pictures: ~60 a window
        now += 50000;
        const bool changed = g.onPicture(now, 50000, true, 0);
        if (changed) {
            CHECK(g.level() == lastLevel + 1);
            lastLevel = g.level();
            CHECK(g.lastP95Us() >= 50000 && g.lastP95Us() <= 50500);
        }
        if (g.windows() != seen) {
            seen = g.windows();
            CHECK(g.lastVerdict() == V_MISS);
            CHECK(g.level() == (int)(seen < (uint32_t)LEVEL_MAX ? seen : (uint32_t)LEVEL_MAX));   // the k-th window: level k
        }
    }
    CHECK(seen >= 25);
    CHECK(g.level() == LEVEL_MAX);
    CHECK(g.changes() == LEVEL_MAX);
}

// Late pictures while no remote puppet is on screen: nothing to give up (no step down), and no step up either.
static void testNoStepWithoutPuppets() {
    Governor g{fullCfg()};
    g.setTargetUs(33333);
    uint64_t now = 1;
    for (int i = 0; i < 60 * 10; i++) {
        now += 50000;
        g.onPicture(now, 50000, false, 0);
    }
    CHECK(g.level() == 0);
    CHECK(g.lastVerdict() == V_HELD);
}

// A gap (a load) restarts the window: pictures before it never count; a window with too few pictures gives no verdict.
static void testGapAndShortWindows() {
    Governor g{fullCfg()};
    g.setTargetUs(33333);
    uint64_t now = 1;
    for (int i = 0; i < 50; i++) {   // 2.5 s of late pictures
        now += 50000;
        g.onPicture(now, 50000, true, 0);
    }
    now += 2000000;
    g.onPicture(now, 2000000, true, 0);   // a load
    for (int i = 0; i < 20; i++) {         // 1 s more: the window (restarted) is not full yet
        now += 50000;
        g.onPicture(now, 50000, true, 0);
    }
    CHECK(g.level() == 0);
    // 3 s with only 20 pictures (150 ms each, under the gap limit): too few for a verdict
    Governor h{fullCfg()};
    h.setTargetUs(33333);
    now = 1;
    for (int i = 0; i < 21; i++) {
        now += 150000;
        h.onPicture(now, 150000, true, 0);
    }
    CHECK(h.level() == 0 && h.lastVerdict() == V_NONE);
}

// Under a 30 cap with the work estimate saying "no room": one step down, then it stays (a rare trial, spaced further each time).
static void testNoOscillationWithWork() {
    Sim s;
    s.lastIv = 40000;
    s.run(600, [](int l) { return l == 0 ? 40000u : 33333u; }, [](int l) { return l == 0 ? 36000u : 30000u; });
    int atZero = 0;
    for (size_t i = 5; i < s.levelPerWindow.size(); i++) atZero += s.levelPerWindow[i] == 0;
    CHECK(s.levelPerWindow.back() == 1 || s.levelPerWindow.back() == 0);
    std::printf("capped, work known: %u changes in %zu windows, %d windows at the missing level\n", s.changes, s.levelPerWindow.size(), atZero);
    CHECK(s.changes <= 10);   // 30 minutes: a handful of trial steps
    CHECK(atZero <= 6);       // each failed trial costs at most a window or two at level 0
    // trials get rarer: the spacing between successive returns to level 0 grows
    std::vector<int> trials;
    for (size_t i = 1; i < s.levelPerWindow.size(); i++)
        if (s.levelPerWindow[i] == 0 && s.levelPerWindow[i - 1] == 1) trials.push_back((int)i);
    for (size_t i = 2; i < trials.size(); i++) CHECK(trials[i] - trials[i - 1] >= trials[i - 1] - trials[i - 2]);
    CHECK(!trials.empty());
}

// The same without a work estimate (the desktop, or the renderer's estimate unknown): trials are more frequent at first but back off.
static void testNoOscillationWithoutWork() {
    Sim s;
    s.lastIv = 40000;
    s.run(600, [](int l) { return l == 0 ? 40000u : 33333u; }, [](int) { return 0u; });
    int atZero = 0;
    for (size_t i = 5; i < s.levelPerWindow.size(); i++) atZero += s.levelPerWindow[i] == 0;
    std::printf("capped, work unknown: %u changes in %zu windows, %d windows at the missing level\n", s.changes, s.levelPerWindow.size(), atZero);
    CHECK(s.changes <= 20);
    CHECK(atZero * 100 <= (int)s.levelPerWindow.size() * 3);   // under 3 % of the time at the level that misses
}

// Uncapped (the desktop): the governor settles at the first level with room and does not go below it.
static void testSettlesUncapped() {
    Sim s;
    s.lastIv = 45000;
    s.run(400, [](int l) { return l == 0 ? 45000u : l == 1 ? 38000u : 20000u; }, [](int) { return 0u; });
    int at2 = 0, below2 = 0;
    for (size_t i = 10; i < s.levelPerWindow.size(); i++) {
        at2 += s.levelPerWindow[i] == 2;
        below2 += s.levelPerWindow[i] > 2;
    }
    CHECK(below2 == 0);
    CHECK(at2 * 100 >= (int)(s.levelPerWindow.size() - 10) * 90);
    std::printf("settles uncapped: %u changes in %zu windows, %d at level 2\n", s.changes, s.levelPerWindow.size(), at2);
    CHECK(s.changes <= 24);   // trials back off to one every 64 windows (3 min)
}

// Room again (the heavy part of the stage is behind): back to level 0, one step per two room windows.
static void testRecovery() {
    Sim s;
    s.lastIv = 50000;
    s.run(6, [](int) { return 50000u; }, [](int) { return 0u; });
    CHECK(s.g.level() == LEVEL_MAX);
    const uint32_t before = s.windows;
    s.run(LEVEL_MAX * 2, [](int) { return 16667u; }, [](int) { return 0u; });
    CHECK(s.g.level() == 0);
    CHECK(s.windows - before == LEVEL_MAX * 2);
    // capped at 30 with a work estimate showing room (20 ms of 33.3): also back to 0
    Sim c;
    c.lastIv = 50000;
    c.run(6, [](int) { return 50000u; }, [](int) { return 0u; });
    c.run(LEVEL_MAX * 2, [](int) { return 33333u; }, [](int) { return 20000u; });
    CHECK(c.g.level() == 0);
}

// Target 60 (Performance mode): 33 ms pictures miss it; 16.7 ms ones hold it.
static void testTarget60() {
    Sim s(16667);
    s.lastIv = 33333;
    s.run(2, [](int) { return 33333u; }, [](int) { return 0u; });
    CHECK(s.g.level() == 2);
    Sim h(16667);
    h.lastIv = 16667;
    h.run(10, [](int) { return 16667u; }, [](int) { return 0u; });
    CHECK(h.g.level() == 0 && h.g.lastVerdict() == V_CALM);
}

// A step up that holds resets the backoff; restart() keeps the level.
static void testBackoffResetAndRestart() {
    Sim s;
    s.lastIv = 40000;
    // fail some trials first
    s.run(120, [](int l) { return l == 0 ? 40000u : 33333u; }, [](int) { return 0u; });
    CHECK(s.g.backoff() > 1);
    // then the load goes: the next trial holds and the backoff resets
    s.run(200, [](int) { return 33333u; }, [](int) { return 0u; });
    CHECK(s.g.level() == 0);
    CHECK(s.g.backoff() == 1);
    Governor g{fullCfg()};
    g.setTargetUs(33333);
    uint64_t now = 1;
    while (g.windows() < 3) {
        now += 50000;
        g.onPicture(now, 50000, true, 0);
    }
    const int l = g.level();
    CHECK(l == 3);
    g.restart();
    CHECK(g.level() == l);
}

// A failed step up is recognised even when windows without puppets on screen (HELD) come between it and the miss.
static void testHeldDoesNotHideAFailedTrial() {
    Governor g{fullCfg()};
    g.setTargetUs(33333);
    uint64_t now = 1;
    auto window = [&](uint32_t iv, bool shown) {
        const uint32_t w = g.windows();
        while (g.windows() == w) {
            now += iv;
            g.onPicture(now, iv, shown, 0);
        }
    };
    window(50000, true);   // MISS: level 1
    CHECK(g.level() == 1);
    window(16667, true);   // ROOM
    window(16667, true);   // ROOM: step up to 0
    CHECK(g.level() == 0 && g.backoff() == 1);
    window(50000, false);  // HELD (late, no puppets on screen)
    window(50000, false);  // HELD
    window(50000, false);  // HELD
    window(50000, true);   // MISS: the step up failed
    CHECK(g.level() == 1);
    CHECK(g.backoff() == 2);
}

static void testNearestAndDistant() {
    float d[4] = {-1.0f, 400.0f, 100.0f, 900.0f};
    CHECK(nearestTwoMask(d, 4) == ((1u << 2) | (1u << 1)));
    float e[4] = {-1.0f, -1.0f, 5.0f, -1.0f};
    CHECK(nearestTwoMask(e, 4) == (1u << 2));
    float f[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    CHECK(nearestTwoMask(f, 4) == 0);
    float g[4] = {7.0f, 7.0f, 7.0f, 1.0f};   // ties: the lower slot
    CHECK(nearestTwoMask(g, 4) == ((1u << 3) | (1u << 0)));
    const LevelParams p0 = paramsFor(0), p2 = paramsFor(2);
    CHECK(!distant(false, 1e12f, p0));
    CHECK(!distant(false, 3200.0f * 3200.0f, p2));   // inside halfIn
    CHECK(distant(false, 3600.0f * 3600.0f, p2));
    CHECK(distant(true, 3200.0f * 3200.0f, p2));     // hysteresis: stays distant until inside halfOut
    CHECK(!distant(true, 2900.0f * 2900.0f, p2));
}

// optfix: the product default (Config() / a Governor with no ceiling set) never leaves level 0, however late the pictures are, and its
// target is 60 fps; the stronger levels exist only behind an explicit ceiling.
static void testDefaultNeverLowersQuality() {
    Governor g;   // (no Config: what the game builds)
    CHECK(g.targetUs() == 16667);
    uint64_t now = 1000000;
    for (int i = 0; i < 60 * 3 * 40; i++) {   // 40 windows of pictures at 20 fps with puppets on screen
        now += 50000;
        CHECK(!g.onPicture(now, 50000, true, 0));
    }
    CHECK(g.windows() >= 30);
    CHECK(g.level() == 0 && g.changes() == 0);
    const LevelParams p = paramsFor(g.level());   // level 0: shadows on, the puppets' own far distance, no half rate, no nearest-two
    CHECK(p.shadows && p.farIn == 5500.0f && p.halfIn == 0.0f && !p.nearestTwo);
    g.setTargetUs(0);
    CHECK(g.targetUs() == 16667);   // (no 30 fps fallback)
    // the testing ceiling: 2 allows levels 1 and 2 and no more
    Governor t;
    t.setMaxLevel(2);
    for (int i = 0; i < 60 * 3 * 40; i++) {
        now += 50000;
        t.onPicture(now, 50000, true, 0);
    }
    CHECK(t.level() == 2);
    t.setMaxLevel(99);
    CHECK(true);   // (clamped to LEVEL_MAX, not an overflow)
    for (int i = 0; i < 60 * 3 * 40; i++) {
        now += 50000;
        t.onPicture(now, 50000, true, 0);
    }
    CHECK(t.level() == LEVEL_MAX);
}

int main() {
    testDefaultNeverLowersQuality();
    testWindowPercentile();
    testParamsMonotone();
    testStepPerWindow();
    testNoStepWithoutPuppets();
    testGapAndShortWindows();
    testNoOscillationWithWork();
    testNoOscillationWithoutWork();
    testSettlesUncapped();
    testRecovery();
    testTarget60();
    testBackoffResetAndRestart();
    testHeldDoesNotHideAFailedTrial();
    testNearestAndDistant();
    if (sFail) {
        std::printf("quality_test: %d FAILED\n", sFail);
        return 1;
    }
    std::printf("quality_test: all passed\n");
    return 0;
}
