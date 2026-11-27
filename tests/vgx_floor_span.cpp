// Host check of GXV_FLOOR_SPAN (the census split of "none of these"): a game-bound Honeyhive-like stretch at target 60 on the steady 30
// (pictures of 2-3 game turns, 15.5 ms of game CPU a turn, 47.6 ms apart) is "none of these" in the old census; the split names it.
// Build and run: tools/test_vgx_floor_span.sh
#include "../platform/vgx/src/vgx_autoq.h"
#include <cstdio>
#include <cstdlib>
using namespace vgx::autoq;
extern "C" int port_autoq_stages(uint32_t* gpuUs, uint32_t* pipeUs, int* lowerable);
static int fails = 0;
static void Check(bool ok, const char* what) { std::printf("  %s: %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++fails; }
int main() {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    Controller ctl(c);
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1;
    // one picture: `interval` us after the last, `spanTurns` game turns of `turnUs` CPU each, backend 12 ms, GPU 12 ms (all fit 28 ms)
    auto feed = [&](uint64_t interval, uint32_t spanTurns, uint32_t turnUs, bool cadence) {
        game += spanTurns * turnUs;
        turns += spanTurns;
        backend += 12000;
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = 12000;
        f.cadence30 = cadence;
        ctl.Add(f);
    };
    feed(33333, 2, 15500, true);                                            // seed
    for (int i = 0; i < 60; ++i) feed(47600, 3, 15500, true);               // game-bound: 3 turns = 46.5 ms of 47.6
    for (int i = 0; i < 20; ++i) feed(40000, 3, 6000, true);                // 3 turns, but the game thread had only 18 ms of work: waits
    for (int i = 0; i < 10; ++i) feed(40000, 1, 15500, true);               // one turn: not multi, game CPU 15.5 of 40
    for (int i = 0; i < 30; ++i) feed(16700, 1, 12000, false);              // on time (not late)
    const FloorWindow w = ctl.TakeFloor();
    std::printf("  pictures %u late %u gpu %u game %u renderer %u other %u unknown %u | multi %u gameSpan %u (%.0f %%)\n", w.pictures, w.late, w.gpu, w.game,
                w.renderer, w.other, w.unknown, w.otherMulti, w.otherGameSpan, w.otherSpanUs ? 100.0 * w.otherGameSpanUs / w.otherSpanUs : 0.0);
    Check(w.late == 90 && w.other == 90, "all 90 late pictures are 'none of these' as before (the stage returned is unchanged)");
    Check(w.otherMulti == 80, "80 of them spanned 2+ turns");
    Check(w.otherGameSpan == 60, "60 of them were filled by the game thread's own turns (>= 85 % of the interval)");
    Check(w.otherSpanUs == 60ull * 47600 && w.otherGameSpanUs == 60ull * 3 * 15500, "their CPU and interval add up");
    // GXV_STEADY30_GPUSCALE: the steady 30's release estimate (port_autoq_stages) with the GPU taken to scale with the pixels or not
    {
        auto pipeFor = [&](double scale) {
            Config k;
            k.targetFps = 60;
            k.budgetMs = 1000.0 / 60.0;
            k.minRung = 0;
            k.maxRung = kTopSingle;
            k.cadenceUp = true;
            k.cadenceGpuScale = scale;
            k.startRung = kToday;
            Controller q(k);
            uint64_t t = 1, be = 1;
            uint32_t g = 1, n = 1;
            for (int i = 0; i < 400; ++i) {
                g += 2 * 8000; n += 2; be += 6000;
                FrameTimes f;
                f.startUs = t;
                t += 33333;
                f.presentUs = t;
                f.backendRunUs = be;
                f.gameRunUs = g;
                f.gameTurns = n;
                f.gameKnown = true;
                f.timers = f.busyKnown = true;
                f.gpuBusyUs = 24000;
                f.cadence30 = true;
                q.Add(f);
            }
            uint32_t gpu = 0, pipe = 0;
            int low = 0;
            const int ok = port_autoq_stages(&gpu, &pipe, &low);
            return ok ? pipe : 0u;
        };
        setenv("GXV_AUTOQ_TARGET", "60", 1);
        const uint32_t all = pipeFor(1.0), part = pipeFor(0.25), none = pipeFor(0.0);
        std::printf("  stage hint pipe: scale 1.0 -> %u us, 0.25 -> %u us, 0 -> %u us\n", all, part, none);
        Check(all != 0 && all < part && part < none && none >= 23900 && none <= 24100, "less of the GPU scaling with the pixels keeps the pipeline estimate nearer the measured 24 ms");
    }
    std::printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
