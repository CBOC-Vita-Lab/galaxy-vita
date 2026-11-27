// gov5 host test: the governor of 60 Assist in mode 5 (GXV_HRW_GOV5; platform/vgx/src/vgx_hrw.h, Params::warp). Pure logic, traces of one sample a second.
// A tiny stage model supplies what the game thread would measure: plain 60 (disengaged) or the pairs (engaged), and the GPU ladder (steps that each take GPU time off, while GPU-limited).
#include "../platform/vgx/src/vgx_hrw.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace vgx::hrw;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static bool g_fix77 = false;
static double g_pad = 0;
static Governor::Params P(bool gov77 = true) {   // the same numbers as Gov5Params() in vgx_galaxy.cpp (gov77: leave under 15.5 ms, engage from 17.0 ms)
    Governor::Params p;
    p.warp = true; p.engageBelowFps = 55; p.settleSec = 4; p.gapSec = 8; p.maxChanges = 6; p.win = 7; p.needFit = 5; p.needLeave = 4; p.ladderWaitSec = 12;
    p.gov77 = gov77;
    p.fix77 = g_fix77; p.pairPadMs = g_pad; p.gpuLimitMs = 16.0;
    if (gov77) p.leaveMs = 15.5;
    return p;
}
struct Stage {
    double game = 12, fe = 10, gpu = 13;   // plain 60, per first picture, ms
    int ladderSteps = 0;                    // steps the ladder can still take (each gains ladderGain ms of GPU, one per 2.2 s, only while the GPU is over 15 ms)
    double ladderGain = 3.0;
    double noise = 0;                       // +- ms on the game thread's turn
    double bShown = 1.0;                    // engaged: the share of second pictures shown
    double warpReal = 2.0;                  // the second picture's real GPU cost (the pair's GPU = gpu + warpReal)
    double pairGpuOverride = 0;             // engaged: the measured pair GPU time when set
    bool gpuUnmeasured = false;             // plain: the GPU's time is not known yet (a stage just loaded)
    bool engaged = false;
    double lastLadder = -100;
    // what the sampler sees in second t
    GovSample Sample(double t, unsigned& seed) {
        GovSample s;
        s.play = true;
        const double nz = noise ? (static_cast<double>(rand_r(&seed) % 2001) / 1000.0 - 1.0) * noise : 0.0;
        if (gpu >= 15.0 && ladderSteps > 0 && t - lastLadder >= 2.2) { gpu -= ladderGain; --ladderSteps; lastLadder = t; }
        const double g = game + nz;
        const double worst = std::max({g, fe, gpu});
        s.ladderRoom = ladderSteps > 0;
        s.engaged = engaged;
        s.costs.warpMs = 2.0;
        if (!engaged) {
            s.nativeFps = std::min(60.0, 1000.0 / worst);
            s.costs.gameDrawnMs = g; s.costs.gameUndrawnMs = 0.4 * g; s.costs.feMs = fe; s.costs.gpuMs = gpu;
            s.costs.gameUndrawnMs = std::min(9.0, std::max(4.0, s.costs.gameUndrawnMs));   // (the caller's fallback)
        } else {
            s.nativeFps = 30.0;   // the pairs' own rate: not used for engaging
            s.costs.gameDrawnMs = g; s.costs.gameUndrawnMs = 0.4 * g; s.costs.feMs = fe;
            s.costs.gpuMs = (gpu + warpReal) / 2.0;   // the stage's figure is the mean of A and B
            s.costs.bShown = bShown;
            s.costs.pairGpuMs = pairGpuOverride > 0 ? pairGpuOverride : gpu + warpReal;
        }
        if (!engaged && gpuUnmeasured) s.costs.gpuMs = 0;
        s.costs.beMs = 7.5;
        return s;
    }
};
struct Run { int engages = 0, leaves = 0; double firstEngage = -1, firstLeave = -1; std::string whyE, whyL; int status = 0; };
static Run Simulate(Stage st, double secs, double t0 = 0, unsigned seed = 1, Governor* gp = nullptr, Stage* after = nullptr, bool gov77 = true) {
    Governor local; Governor& g = gp ? *gp : local;
    g.Configure(P(gov77)); g.StageBegin(0);
    Run r;
    for (double t = 1; t <= secs; t += 1) {
        const GovSample s = st.Sample(t + t0, seed);
        const GovAction a = g.Tick(t, s);
        if (a == kGovEngage) { st.engaged = true; ++r.engages; if (r.firstEngage < 0) r.firstEngage = t; r.whyE = g.Why(); }
        if (a == kGovLeave) { st.engaged = false; ++r.leaves; if (r.firstLeave < 0) r.firstLeave = t; r.whyL = g.Why(); }
    }
    r.status = g.Status();
    if (after) *after = st;
    return r;
}
int main() {
    // 1. the stage holds 60: never engaged, declined as "holds 60 natively"
    { Stage s; Run r = Simulate(s, 120); CHECK(r.engages == 0 && r.leaves == 0); CHECK(r.status == kGovNotNeeded); }
    // 1b. holds 60 with the game thread noisy around 12 ms
    { Stage s; s.noise = 3; Run r = Simulate(s, 300, 0, 7); CHECK(r.engages == 0); }
    // 1c. close to 60 and fluctuating (51-60 fps, dips of a second or two): plain 60 stays, no assist
    { Stage s; s.game = 16.0; s.noise = 2.2; Run r = Simulate(s, 600, 0, 3); CHECK(r.engages == 0); }
    // 1d. a short dip (4 s at 40 fps) is no verdict
    {
        Governor g; g.Configure(P()); g.StageBegin(0); Stage s; unsigned seed = 1; int e = 0;
        for (double t = 1; t <= 120; t += 1) { s.game = (t >= 20 && t < 24) ? 25 : 12; if (g.Tick(t, s.Sample(t, seed)) == kGovEngage) ++e; }
        CHECK(e == 0);
    }
    // 1e. pause / loads (not play) restart the window
    {
        Governor g; g.Configure(P()); g.StageBegin(0); Stage s; s.game = 25; unsigned seed = 1; int e = 0;
        for (double t = 1; t <= 60; t += 1) { GovSample x = s.Sample(t, seed); x.play = std::fmod(t, 4.0) != 0; if (g.Tick(t, x) == kGovEngage) ++e; }
        CHECK(e == 0);   // a window of 7 seconds of play never completes with a pause every 4th second
    }
    // 2. 45 fps, GPU-bound, the ladder can fix it (22 -> 13 ms in three steps): the ladder first, the pairs never
    { Stage s; s.gpu = 22; s.ladderSteps = 3; Stage a; Run r = Simulate(s, 120, 0, 1, nullptr, &a); CHECK(r.engages == 0); CHECK(a.gpu < 15.0); CHECK(r.status == kGovNotNeeded); }
    // 2b. the ladder tries and cannot fix it (steps that fail are 'refused': room goes false): the pairs then, after the ladder's time, not before
    { Stage s; s.gpu = 22; s.ladderSteps = 1; s.ladderGain = 0.5; Run r = Simulate(s, 120, 0, 1, nullptr, nullptr, false); CHECK(r.engages == 1); CHECK(r.firstEngage >= 5); CHECK(r.firstEngage <= 12); CHECK(r.leaves == 0);
      Run r7 = Simulate(s, 120); CHECK(r7.engages == 0); CHECK(r7.status == kGovGpuLimit); }   // (gov77: a GPU-limited stage never takes the pair)
    // 2c. the ladder never runs out of room but never helps either (room stuck true): the wait is capped (settle 4 + ladder 12 s)
    { Stage s; s.gpu = 22; s.ladderSteps = 1000; s.ladderGain = 0.0; Run r = Simulate(s, 120, 0, 1, nullptr, nullptr, false); CHECK(r.engages == 1); CHECK(r.firstEngage >= 16 && r.firstEngage <= 26);
      Run r7 = Simulate(s, 120); CHECK(r7.engages == 0); }
    // 3. GPU-bound and nothing left in the ladder: the pairs, quickly (settle 4 s + 3 samples)
    { Stage s; s.gpu = 22; s.ladderSteps = 0; Run r = Simulate(s, 60, 0, 1, nullptr, nullptr, false); CHECK(r.engages == 1); CHECK(r.firstEngage >= 5 && r.firstEngage <= 10); CHECK(r.whyE.find("native rate under 55") != std::string::npos);
      Run r7 = Simulate(s, 60); CHECK(r7.engages == 0); }
    // 4. 45 fps CPU-bound (game thread 22 ms, GPU 12): the ladder cannot help, so the pairs do not wait for it
    { Stage s; s.game = 22; s.gpu = 12; s.ladderSteps = 5; Run r = Simulate(s, 60); CHECK(r.engages == 1); CHECK(r.firstEngage <= 10); }
    // 5. the pair does not fit (GPU 40 ms): never engages (the even 30 owns it)
    { Stage s; s.gpu = 40; Run r = Simulate(s, 120, 0, 1, nullptr, nullptr, false); CHECK(r.engages == 0); CHECK(r.status == kGovNoFit); }
    // 6. recovers: CPU-bound for 30 s, then the game thread eases (a quiet corner of the level): leaves, within a few seconds of the gap
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1;
        int engages = 0, leaves = 0; double leftAt = -1; std::string why;
        for (double t = 1; t <= 90; t += 1) {
            if (t == 31) s.game = 11;
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++engages; }
            if (a == kGovLeave) { s.engaged = false; ++leaves; leftAt = t; why = g.Why(); }
        }
        CHECK(engages == 1 && leaves == 1);
        CHECK(leftAt >= 31 && leftAt <= 31 + 8);
        CHECK(why.find("plain 60 would hold") != std::string::npos);
    }
    // 6b. a GPU that only just fits plain 60 does not make it leave (the mean of A and B reads dearly): 17.6 ms is over the leave limit of 17
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int leaves = 0;
        for (double t = 1; t <= 90; t += 1) {
            if (t == 31) { s.game = 11; s.gpu = 17.6; }
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) s.engaged = true;
            if (a == kGovLeave) { s.engaged = false; ++leaves; }
        }
        CHECK(leaves == 0);
    }
    // 7. noisy traces around the threshold: no flapping (changes bounded by maxChanges, never two within the gap)
    for (unsigned seed = 1; seed <= 20; ++seed) {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 16.5; s.gpu = 12; s.noise = 4.0;   // plain 60 ~ 50-60 fps, the game thread straddles 16.7
        double lastAt = -100; bool gapOk = true; unsigned sd = seed;
        for (double t = 1; t <= 600; t += 1) {
            const GovAction a = g.Tick(t, s.Sample(t, sd));
            if (a != kGovNone) { gapOk = gapOk && t - lastAt >= 8.0; lastAt = t; s.engaged = a == kGovEngage; }
        }
        CHECK(g.Changes() <= 6); CHECK(gapOk);
    }
    // 8. a leave that has to be taken back is not repeated lightly: the next leave asks for more room (relapse backoff)
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int engages = 0, leaves = 0;
        for (double t = 1; t <= 200; t += 1) {
            // the game thread sits at 17.0 ms engaged (reads "plain 60 holds") but is 19 ms (52 fps) when plain: it comes back at once
            if (t >= 31) s.game = s.engaged ? 15.0 : 19.0;
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++engages; }
            if (a == kGovLeave) { s.engaged = false; ++leaves; }
        }
        // the first leave (15.0 < 15.5) happens, the plain 60 does not hold, it engages again; the second leave needs under 15.5 * 0.94 = 14.57: never
        CHECK(leaves == 1 && engages == 2);
    }
    // 9. the pair fit for a warp pair: B is cheap (gpu + warp), mode 1's replay doubles the GPU
    {
        Costs c; c.gameDrawnMs = 21.6; c.gameUndrawnMs = 8.7; c.feMs = 19.9; c.beMs = 7.5; c.gpuMs = 19.2;
        CHECK(!PairFit(c).All() && std::string(PairFit(c).Limit()) == "gpu");   // the replay: 38.4 ms of GPU in 33.3
        c.warpMs = 2.0;
        CHECK(PairFit(c).All());                                                // the warp: 21.2
        c.gpuMs = 30.5; CHECK(!PairFit(c).All());
    }
    // 10. Quality-style off switch: the plain governor (mode 1) is unchanged by the new fields
    {
        Governor g; Governor::Params p; g.Configure(p); g.StageBegin(0);
        GovSample s; s.play = true; s.nativeFps = 40; s.costs.gameDrawnMs = 21.6; s.costs.gameUndrawnMs = 8.7; s.costs.feMs = 19.9; s.costs.beMs = 7.5; s.costs.gpuMs = 14.5;
        int e = 0; for (double t = 1; t < 40; ++t) if (g.Tick(t, s) == kGovEngage) ++e;
        CHECK(e == 1);
    }
    // 11. gov77 scenarios from the console night57 logs
    // 11a. Dreadnought plain (DNP/DNS): 47.8 fps, game thread drawn 10.3 ms, front end 15.1, GPU 19.3 ms a picture: the GPU is the limit, Assist is never taken, the reason is logged
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 10.3; s.fe = 15.1; s.gpu = 19.3; unsigned seed = 1; int e = 0; int codeChanges = 0, lastCode = -1;
        for (double t = 1; t <= 300; t += 1) {
            if (g.Tick(t, s.Sample(t, seed)) == kGovEngage) ++e;
            if (g.VerdictCode() != lastCode) { lastCode = g.VerdictCode(); ++codeChanges; }
        }
        CHECK(e == 0); CHECK(g.Status() == kGovGpuLimit); CHECK(g.VerdictCode() == Governor::kVGpuLimit);
        CHECK(std::string(g.VerdictWhy()).find("GPU, not the game thread") != std::string::npos);
        CHECK(codeChanges <= 3);   // (settling, then the reason: no chatter)
    }
    // 11b. the old rule (gov77 off) engaged exactly this stage: the pair fits (19.3 + 2 < 31.6) and 47.8 < 55
    { Stage s; s.game = 10.3; s.fe = 15.1; s.gpu = 19.3; Run r = Simulate(s, 120, 0, 1, nullptr, nullptr, false); CHECK(r.engages == 1); }
    // 11c. Freezeflame: a stage-load transient (hitching, 33-49 fps for ~15 s) with the GPU's time not measured yet and the game thread at 15.4 ms drawn, then 60 held: no Assist
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 15.4; s.fe = 11.3; s.gpu = 13; unsigned seed = 1; int e = 0;
        for (double t = 1; t <= 120; t += 1) {
            const bool transient = t < 28;
            s.gpuUnmeasured = transient;
            GovSample x = s.Sample(t, seed);
            if (transient) x.nativeFps = 40;   // (shader compiles, hitches)
            if (g.Tick(t, x) == kGovEngage) ++e;
        }
        CHECK(e == 0);
    }
    // 11d. the game thread is the limit (22 ms drawn) but the GPU's time is unknown: no Assist on a guess; once it is known, it engages
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int e = 0; double at = -1;
        for (double t = 1; t <= 60; t += 1) {
            s.gpuUnmeasured = t < 20;
            if (g.Tick(t, s.Sample(t, seed)) == kGovEngage) { ++e; at = t; s.engaged = true; }
            if (t == 19) CHECK(g.VerdictCode() == Governor::kVNoGpuFigure);
        }
        CHECK(e == 1 && at >= 20);
    }
    // 11e. game thread limited, the GPU has no room for the second picture (plain 30 ms + 2): no engage, "does not fit"
    { Stage s; s.game = 22; s.gpu = 30.5; Run r = Simulate(s, 120); CHECK(r.engages == 0); CHECK(r.status == kGovNoFit); }
    // 11f. Assist on but the second pictures are lost (DNA: 24 % shown; DNS: 21 of 39): plain at once (3 s after the changeover), blocked for the stage, never again
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int e = 0, l = 0; double engAt = -1, leaveAt = -1; std::string why;
        for (double t = 1; t <= 200; t += 1) {
            s.bShown = 0.24;
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++e; engAt = t; }
            if (a == kGovLeave) { s.engaged = false; ++l; leaveAt = t; why = g.Why(); }
        }
        CHECK(e == 1 && l == 1); CHECK(leaveAt - engAt <= 3.0 + 1e-9); CHECK(why.find("loses its second pictures") != std::string::npos);
        CHECK(g.Blocked() && g.Status() == kGovBlocked);
    }
    // 11g. B shown 100 % but the pair is over the GPU's budget (the extra picture costs 25 ms): back to plain at once, blocked
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int e = 0, l = 0; double engAt = -1, leaveAt = -1; std::string why;
        for (double t = 1; t <= 120; t += 1) {
            s.pairGpuOverride = s.engaged ? 40.0 : 0.0;
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++e; engAt = t; }
            if (a == kGovLeave) { s.engaged = false; ++l; leaveAt = t; why = g.Why(); }
        }
        CHECK(e == 1 && l == 1); CHECK(leaveAt - engAt <= 3.0 + 1e-9); CHECK(why.find("over the GPU's budget") != std::string::npos); CHECK(g.Blocked());
    }
    // 11h. a healthy pair (B shown 97 %, GPU 14 of 31.6) stays on and teaches the second picture's GPU cost (here 4.0 ms)
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; s.warpReal = 4.0; s.bShown = 0.97; unsigned seed = 1; int e = 0, l = 0;
        for (double t = 1; t <= 120; t += 1) {
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++e; }
            if (a == kGovLeave) { s.engaged = false; ++l; }
        }
        CHECK(e == 1 && l == 0); CHECK(!g.Blocked()); CHECK(std::fabs(g.LearnedWarpMs() - 4.0) < 0.6);
    }
    // 11i. one second of B shown under 90 % is no verdict (a camera cut)
    {
        Governor g; g.Configure(P()); g.StageBegin(0);
        Stage s; s.game = 22; s.gpu = 12; unsigned seed = 1; int e = 0, l = 0;
        for (double t = 1; t <= 120; t += 1) {
            s.bShown = std::fmod(t, 5.0) == 0 ? 0.5 : 1.0;
            const GovAction a = g.Tick(t, s.Sample(t, seed));
            if (a == kGovEngage) { s.engaged = true; ++e; }
            if (a == kGovLeave) { s.engaged = false; ++l; }
        }
        CHECK(e == 1 && l == 0);
    }
    if (fails) { printf("hrw_gov5_test: %d FAILED\n", fails); return 1; }
    // 12. gov24: the two verdicts seen in night58 (02-HHS.log lines 439 and 834), with GXV_GOV77_FIX off (as logged) and on
    for (int fix = 0; fix < 2; ++fix) {
        g_fix77 = fix != 0;
        // 12a. Honeyhive 439: native 20.7 fps, game 27.7 ms in the last second, GPU 0.0 (unmeasured); the window held seconds with no figures (0 = unknown) and a few
        // with the game thread under 17: the old rule counted every one of them as "GPU-limited" (game under 17 first, the GPU never looked at)
        {
            Governor g; g.Configure(P()); g.StageBegin(0);
            int e = 0;
            for (int t = 1; t <= 12; ++t) {
                GovSample x; x.play = true; x.costs.warpMs = 2.0; x.costs.gameUndrawnMs = 8;
                if (t <= 4) { x.nativeFps = 0; }                                             // no turn drawn: nothing measured
                else if (t <= 8) { x.nativeFps = 40; x.costs.gameDrawnMs = 14; x.costs.feMs = 12; }   // a few light seconds, GPU still unmeasured
                else { x.nativeFps = 20.7; x.costs.gameDrawnMs = 27.7; x.costs.feMs = 26.6; }
                x.costs.gpuMs = 0;
                if (g.Tick(t, x) == kGovEngage) ++e;
            }
            CHECK(e == 0);
            if (!fix) CHECK(g.VerdictCode() == Governor::kVGpuLimit);        // the logged wrong verdict
            else CHECK(g.VerdictCode() != Governor::kVGpuLimit && g.VerdictCode() == Governor::kVNoGpuFigure);
        }
        // 12b. Honeyhive 834: native 48.2 fps, game 9.9, front end 9.4, GPU 15.3 ms: nothing over budget
        {
            Governor g; g.Configure(P()); g.StageBegin(0);
            int e = 0;
            for (int t = 1; t <= 12; ++t) {
                GovSample x; x.play = true; x.costs.warpMs = 2.0; x.costs.gameUndrawnMs = 4; x.nativeFps = 48.2;
                x.costs.gameDrawnMs = 9.9; x.costs.feMs = 9.4; x.costs.gpuMs = 15.3;
                if (g.Tick(t, x) == kGovEngage) ++e;
            }
            CHECK(e == 0);
            if (!fix) CHECK(g.VerdictCode() == Governor::kVGpuLimit);
            else { CHECK(g.VerdictCode() == Governor::kVNeither); CHECK(g.Status() != kGovGpuLimit); }
        }
        // 12c. Dreadnought (GPU 20 ms, game 10): still the GPU, both ways; a real game-thread stage (22 ms, GPU 12) still engages both ways
        {
            Governor g; g.Configure(P()); g.StageBegin(0);
            Stage s; s.game = 10.3; s.fe = 15.1; s.gpu = 20.0; unsigned seed = 1;
            for (double t = 1; t <= 30; t += 1) g.Tick(t, s.Sample(t, seed));
            CHECK(g.VerdictCode() == Governor::kVGpuLimit && g.Status() == kGovGpuLimit);
            Stage s2; s2.game = 22; s2.gpu = 12; s2.ladderSteps = 5; Run r = Simulate(s2, 60); CHECK(r.engages == 1 && r.firstEngage <= 10);
        }
    }
    g_fix77 = false;
    // 13. gov24 PAIRPAD: Beach Bowl 01-BBS 09:14:34: plain drawn 21.5, front end 18.3, GPU 17.4, native 35 with undrawn 9.7 (predicted 31.2 of 32.3); the pairs measured 33.3: engaged and left after 3 s
    for (int pad = 0; pad < 2; ++pad) {
        g_pad = pad ? 2.1 : 0.0;
        Governor g; g.Configure(P()); g.StageBegin(0);
        int e = 0;
        for (int t = 1; t <= 40; ++t) {
            GovSample x; x.play = true; x.nativeFps = 35; x.costs.warpMs = 2.0; x.costs.gameDrawnMs = 21.5; x.costs.gameUndrawnMs = 9.7; x.costs.feMs = 18.3; x.costs.gpuMs = 17.4;
            if (g.Tick(t, x) == kGovEngage) { ++e; break; }
        }
        if (!pad) CHECK(e == 1); else { CHECK(e == 0); CHECK(g.VerdictCode() == Governor::kVNoFit); }
        // a lighter stage (drawn 19.0 + 8.0 = 27.0, +2.1 = 29.1) still engages with the pad
        Governor h; h.Configure(P()); h.StageBegin(0);
        int e2 = 0;
        for (int t = 1; t <= 40; ++t) {
            GovSample x; x.play = true; x.nativeFps = 38; x.costs.warpMs = 2.0; x.costs.gameDrawnMs = 19.0; x.costs.gameUndrawnMs = 8.0; x.costs.feMs = 15.0; x.costs.gpuMs = 14.0;
            if (h.Tick(t, x) == kGovEngage) { ++e2; break; }
        }
        CHECK(e2 == 1);
    }
    g_pad = 0;
    printf("hrw_gov5_test: ok\n");
    return 0;
}
