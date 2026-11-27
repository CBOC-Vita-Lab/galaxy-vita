// hrwfeel36: port/hrw_feel.h, the "hrw: feel |" line's counting (isolated losses, bursts, runs, kinds, changes).
#include "port/hrw_feel.h"
#include <cstdio>
#include <cstring>
using namespace port::hrwfeel;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    {   // 10 s at 30 pairs a second, all shown with the depth warp: 100 %, no loss, a run of the whole window
        Feel f; double t = 0;
        for (int i = 0; i < 300; ++i, t += 33.3) f.Shown(t, kDepth);
        const Feel::Report r = f.Take(t);
        CHECK(r.shown == 300 && r.lost == 0 && r.shownPct == 100.0 && r.isolated == 0 && r.bursts == 0 && r.changes == 0 && r.kind[kDepth] == 300);
        CHECK(r.longestRunS > 9.9 && r.longestRunS < 10.0 && r.seconds > 9.9);
    }
    {   // isolated losses: one lost pair between shown ones; two in a row is not isolated; a lost pair at the end is open
        Feel f; double t = 0;
        auto S = [&] { f.Shown(t, kDepth); t += 33.3; };
        auto L = [&](int side, int why = 0) { f.Lost(t, side, why); t += 33.3; };
        S(); S(); L(kWarp, 10); S(); S(); L(kGame); L(kRender); S(); S(); L(kWarp, 10); S();
        const Feel::Report r = f.Take(t);
        CHECK(r.shown == 7 && r.lost == 4 && r.isolated == 2 && r.bursts == 1);   // (three losses within a second: one burst)
        CHECK(r.lostSide[kGame] == 1 && r.lostSide[kRender] == 1 && r.lostSide[kWarp] == 2 && r.why[10] == 2);
        CHECK(r.longestRunS > 0.03 && r.longestRunS < 0.1);   // S S: 33 ms between first and last
        char b[900];
        const char* names[kWhy] = {}; names[10] = "edge strip too wide";
        Feel::Format(b, sizeof b, r, names);
        CHECK(std::strstr(b, "hrw: feel |") && std::strstr(b, "edge strip too wide 2") && std::strstr(b, "isolated losses 2"));
    }
    {   // bursts: 3 lost within 1 s count once; two lost, then a gap over a second, does not
        Feel f; double t = 0;
        f.Lost(t, kRender); f.Lost(t += 300, kRender); f.Lost(t += 300, kRender);
        Feel::Report r = f.Take(t + 1);
        CHECK(r.bursts == 1 && r.lost == 3);
        f.Lost(t += 2000, kRender); f.Lost(t += 600, kRender); f.Lost(t += 600, kRender);
        f.Lost(t += 100, kRender); f.Lost(t += 100, kRender);     // 600 -> 600 -> 100 -> 100: the last three are within 800 ms
        r = f.Take(t + 1);
        CHECK(r.bursts == 1 && r.lost == 5);
    }
    {   // warp kinds and changes of kind; runs carry across a window; snapped
        Feel f; double t = 0;
        for (int i = 0; i < 10; ++i, t += 100) f.Shown(t, kDepth);
        f.Shown(t += 100, kPlane); f.Shown(t += 100, kRot); f.Shown(t += 100, kRot); f.Shown(t += 100, kDepth);
        f.Snapped(); f.Snapped();
        Feel::Report r = f.Take(t);
        CHECK(r.kind[kDepth] == 11 && r.kind[kPlane] == 1 && r.kind[kRot] == 2 && r.changes == 3 && r.snapped == 2);
        for (int i = 0; i < 10; ++i) f.Shown(t += 100, kDepth);
        r = f.Take(t);
        CHECK(r.changes == 0 && r.snapped == 0 && r.longestRunS > 0.85);   // the run goes on from the earlier window: counted from this window's start
    }
    {   // out of range arguments are clamped, an empty window is empty
        Feel f;
        f.Shown(0, 99); f.Lost(10, 77, 400);
        Feel::Report r = f.Take(20);
        CHECK(r.kind[kRot] == 1 && r.lostSide[kRender] == 1);
        Feel g;
        r = g.Take(5);
        CHECK(r.shown == 0 && r.lost == 0 && r.shownPct == 0.0 && r.seconds == 0.0);
    }
    std::printf(fails ? "hrw_feel_test: %d FAILED\n" : "hrw_feel_test: ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
