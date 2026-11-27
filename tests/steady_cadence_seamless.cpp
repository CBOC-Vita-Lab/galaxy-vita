// seamless23: the steady 30 <-> up to 60 switch without a burst of uneven pictures (frame_pace.h SteadyCadence, Params::cutRelease /
// cutWaitSec / quickTurns; GXV_SEAMLESS in perf.cpp). tools/test_vgx_seamless.sh builds and runs it.
//   1. Off (the defaults): the release comes at the end of the window that makes it due, as before.
//   2. cutRelease, no cut: the release waits cutWaitSec, then is made.
//   3. cutRelease, a camera cut 2 s after it is due: the release is made at the cut's turn.
//   4. cutRelease, the estimate goes bad while waiting: no release.
//   5. quickTurns: a release the pictures do not confirm engages again after quickTurns turns (not after a whole window of 60) and blocks the
//      next release as the confirm rule does.
#include "../platform/include/port/frame_pace.h"

#include <cstdio>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
struct Drive {
    port::pace::SteadyCadence c;
    uint64_t t = 1000;
    long turn = 0, releasedTurn = -1, engagedTurn = -1;
    long cutAtTurn = -1;
    // one turn: a picture every turn while not engaged when drawEvery, else every second turn; drawn/other turn costs (ms); pipeline (us)
    void Turn(bool drawEvery, double drawnMs, double otherMs, uint32_t pipeUs) {
        if (turn == cutAtTurn) c.NoteCut(t);
        c.Stages(true, 9000, pipeUs, true);
        const bool was = c.Engaged();
        const bool offered = c.Begin(t, true, false);
        if (was && !c.Engaged() && releasedTurn < 0) releasedTurn = turn;
        const bool draw = offered && (drawEvery || (turn % 2) == 0);
        c.Decision(offered, draw);
        const bool before = c.Engaged();
        c.End(t + static_cast<uint64_t>((draw ? drawnMs : otherMs) * 1000.0));
        if (before && !c.Engaged() && releasedTurn < 0) releasedTurn = turn;
        if (!before && c.Engaged()) engagedTurn = turn;
        t += 16683;
        ++turn;
    }
    void Turns(int n, bool drawEvery, double drawnMs, double otherMs, uint32_t pipeUs) {
        for (int i = 0; i < n; ++i) Turn(drawEvery, drawnMs, otherMs, pipeUs);
    }
    void EngageHeavy() { Turns(4 * 60, false, 24.0, 8.0, 20000); }      // 33 a second by the turns' costs: engaged
    void Light(int n) { Turns(n, true, 12.0, 6.0, 15000); }             // an estimate of 45+: the release becomes due (every offered turn draws)
};
port::pace::SteadyCadence::Params Seam(double waitSec, unsigned quick) {
    port::pace::SteadyCadence::Params p;
    p.cutRelease = true;
    p.cutWaitSec = waitSec;
    p.quickTurns = quick;
    return p;
}
}

int main() {
    std::printf("steady 30 <-> up to 60 (GXV_SEAMLESS)\n");
    long dueTurn = -1;
    {
        Drive d;
        d.c.Configure(true);
        d.EngageHeavy();
        Check(d.c.Engaged(), "off: engaged by the CPU limit");
        const long t0 = d.turn;
        d.Light(10 * 60);
        dueTurn = d.releasedTurn;
        std::printf("    off: released %.1f s after the light area began\n", (d.releasedTurn - t0) / 60.0);
        Check(!d.c.Engaged() && d.releasedTurn > 0 && d.releasedTurn % 60 == 59, "off: released at the end of the window that makes it due (as before)");
        dueTurn -= t0;
    }
    {
        Drive d;
        d.c.Configure(true, Seam(15.0, 0));
        d.EngageHeavy();
        const long t0 = d.turn;
        d.Light(30 * 60);
        const double after = (d.releasedTurn - t0 - dueTurn) / 60.0;
        std::printf("    cutRelease, no cut: released %.1f s after it was due\n", after);
        Check(d.releasedTurn > 0 && after >= 14.0 && after <= 16.1 && d.c.ReleasesTimedOut() == 1, "no cut: the release waits cutWaitSec (15 s), then is made");
    }
    {
        Drive d;
        d.c.Configure(true, Seam(15.0, 0));
        d.EngageHeavy();
        const long t0 = d.turn;
        d.cutAtTurn = t0 + dueTurn + 2 * 60 + 17;
        d.Light(30 * 60);
        std::printf("    cutRelease, a cut 2.3 s after it was due: released at turn %+ld of the cut\n", d.releasedTurn - d.cutAtTurn);
        Check(d.releasedTurn == d.cutAtTurn && d.c.ReleasesAtCut() == 1 && d.c.LastAtCut(), "a camera cut: the release is made at the cut's turn");
    }
    {
        Drive d;
        d.c.Configure(true, Seam(15.0, 0));
        d.EngageHeavy();
        const long t0 = d.turn;
        d.Light(dueTurn + 30);                  // due, waiting for a cut ...
        d.EngageHeavy();                        // ... the area is heavy again
        d.cutAtTurn = d.turn + 5;
        d.Turns(60, false, 24.0, 8.0, 20000);
        Check(d.c.Engaged() && d.releasedTurn < 0, "the estimate bad again while waiting: no release, not even at the cut");
        (void)t0;
    }
    {
        Drive d;
        d.c.Configure(true, Seam(0.0, 20));       // (the release at once when due, to test the quick confirm)
        d.EngageHeavy();
        d.Light(10 * 60);
        Check(!d.c.Engaged(), "released");
        const long rel = d.turn;
        d.Turns(60, false, 24.0, 8.0, 20000);    // uneven again right after the release
        std::printf("    quickTurns 20: engaged again %ld turns after the release\n", d.engagedTurn - rel + 1);
        Check(d.c.Engaged() && d.engagedTurn - rel + 1 == 20 && d.c.QuickEngages() == 1 && d.c.BlockSec() == 30.0,
              "a release the pictures do not confirm engages again after 20 turns (not 60), the next release blocked 30 s");
    }
    {
        Drive d;
        d.c.Configure(true, Seam(0.0, 20));
        d.EngageHeavy();
        d.Light(10 * 60);
        d.Light(30);                             // (good for 30 more turns: the first run of 20 confirms nothing bad)
        const long rel = d.turn;
        d.Turns(60, false, 24.0, 8.0, 20000);    // then uneven in the middle of a window
        std::printf("    quickTurns 20, uneven from the middle of a window: engaged again %ld turns after\n", d.engagedTurn - rel + 1);
        Check(d.c.Engaged() && d.engagedTurn - rel + 1 <= 30, "uneven pictures in the middle of a window engage again within 30 turns");
    }
    {
        Drive d;
        d.c.Configure(true);                      // (the defaults: the whole window)
        d.EngageHeavy();
        d.Light(10 * 60);
        const long rel = d.turn;
        d.Turns(60, false, 24.0, 8.0, 20000);
        Check(d.c.Engaged() && d.engagedTurn - rel + 1 == 60, "off: the confirm rule engages again after the whole window (60 turns), as before");
    }
    std::printf("%s\n", g_failures ? "FAIL" : "PASS steady cadence seamless");
    return g_failures ? 1 : 0;
}
