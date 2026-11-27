// keepawake33: exhaustive host verification of port::keepawake OLED-safe policy + atomic guard nesting/concurrency.
#include "port/keep_awake.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

using namespace port::keepawake;

namespace {

std::atomic<int> g_counter{0};
struct Guard {
    Guard() { g_counter.fetch_add(1, std::memory_order_relaxed); }
    ~Guard() { g_counter.fetch_sub(1, std::memory_order_relaxed); }
};

void TestTruthTable() {
    unsigned checked = 0;
    const uint64_t frames[] = {0u, 1u, 2u, 120u};
    for (int fatal = 0; fatal <= 1; ++fatal)
    for (int settings = 0; settings <= 1; ++settings)
    for (int flag = 0; flag <= 1; ++flag)
    for (int script = 0; script <= 1; ++script)
    for (int movie = 0; movie <= 1; ++movie)
    for (int cawake = 0; cawake <= 1; ++cawake)
    for (int boot = 0; boot <= 1; ++boot)
    for (int stalled = 0; stalled <= 1; ++stalled)
    for (int compiling = 0; compiling <= 1; ++compiling)
    for (uint64_t f : frames) {
        State s{};
        s.fatalShown = fatal != 0;
        s.settingsOpen = settings != 0;
        s.keepAwakeFlag = flag != 0;
        s.scriptLive = script != 0;
        s.inMovie = movie != 0;
        s.compileAwake = cawake != 0;
        s.bootLoading = boot != 0;
        s.bootStalled = stalled != 0;
        s.vgxFrames = f;
        s.shaderCompiling = compiling != 0;

        const Reason r = TickReason(s);
        const bool tick = ShouldTick(s);
        assert(tick == (r != Reason::None));

        // Hard OLED-safety invariants: fatal screen and Settings NEVER hold the screen awake.
        if (s.fatalShown || s.settingsOpen) {
            assert(!tick && r == Reason::None);
        } else if (s.keepAwakeFlag) {
            assert(tick && r == Reason::KeepAwakeFlag);
        } else if (s.scriptLive) {
            assert(tick && r == Reason::Script);
        } else if (s.inMovie) {
            assert(tick && r == Reason::Movie);
        } else if (s.compileAwake && !s.bootStalled && (s.bootLoading || s.vgxFrames < 2u || s.shaderCompiling)) {
            assert(tick && r == Reason::BootOrCompile);
        } else {
            // Idle normal player (menus, pause, standing still in gameplay) or stalled boot MUST allow OLED standby.
            assert(!tick && r == Reason::None);
        }
        ++checked;
    }
    assert(checked == 2048u);

    // Explicit scenario sanity checks
    {
        // Normal idle player in gameplay / pause / menus -> OLED sleeps
        State idle{};
        idle.vgxFrames = 2u;
        assert(!ShouldTick(idle));
        // Normal player during mid-game shader compile -> stays awake
        State compiling = idle;
        compiling.shaderCompiling = true;
        assert(ShouldTick(compiling) && TickReason(compiling) == Reason::BootOrCompile);
        // Same with GXV_COMPILE_AWAKE=0 -> respects opt-out
        compiling.compileAwake = false;
        assert(!ShouldTick(compiling));
        // Cold boot before frame 2 -> stays awake
        State boot{};
        boot.vgxFrames = 1u;
        assert(ShouldTick(boot));
        // Boot loading screen active even if frames >= 2 (e.g. deferred warm-up behind splash) -> stays awake
        State splash = idle;
        splash.bootLoading = true;
        assert(ShouldTick(splash));
        // Fatal error overrides EVERYTHING (including keepAwakeFlag & scriptLive & movie & compile)
        State fatal{};
        fatal.fatalShown = true;
        fatal.keepAwakeFlag = true;
        fatal.scriptLive = true;
        fatal.inMovie = true;
        fatal.bootLoading = true;
        fatal.shaderCompiling = true;
        assert(!ShouldTick(fatal));
        // Settings overrides EVERYTHING
        State settings = fatal;
        settings.fatalShown = false;
        settings.settingsOpen = true;
        assert(!ShouldTick(settings));
    }

    // ShouldSplashTick truth table
    for (int ca = 0; ca <= 1; ++ca)
    for (int fat = 0; fat <= 1; ++fat)
    for (int act = 0; act <= 1; ++act) {
        const bool expect = (ca != 0) && (fat == 0) && (act != 0);
        assert(ShouldSplashTick(ca != 0, fat != 0, act != 0) == expect);
    }
}

void TestGuardNestingAndConcurrency() {
    assert(g_counter.load() == 0);
    {
        Guard a;
        assert(g_counter.load() == 1);
        {
            Guard b;
            assert(g_counter.load() == 2);
        }
        assert(g_counter.load() == 1);
    }
    assert(g_counter.load() == 0);

    // Simulate render thread + async shader worker compiling concurrently
    auto work = [] {
        for (int i = 0; i < 50000; ++i) {
            Guard outer;
            if ((i & 3) == 0) {
                Guard inner;
                assert(g_counter.load(std::memory_order_relaxed) >= 2);
            }
            assert(g_counter.load(std::memory_order_relaxed) >= 1);
        }
    };
    std::thread t1(work), t2(work);
    t1.join();
    t2.join();
    assert(g_counter.load() == 0);
}

}  // namespace

int main() {
    TestTruthTable();
    TestGuardNestingAndConcurrency();
    std::puts("keep_awake_test: ok (2048 truth-table states + guard nesting/concurrency verified)");
    return 0;
}
