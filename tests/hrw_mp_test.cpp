// mp30 host test: the multiplayer session's picture policy (platform/include/port/hrw_mp.h) alone, and closed-loop with the real governor (platform/vgx/src/vgx_hrw.h) and a tiny stage model.
// The closed loop mirrors the 1 Hz census in vgx_galaxy.cpp (the "mp30" block after Governor::Tick): same calls, same order.
#include "port/hrw_mp.h"
#include "../platform/vgx/src/vgx_hrw.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace port::hrwmp;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static Params P() { return Params(); }
static Sec Play(bool fits, double fps = 30, bool holds = false, bool blocked = false, GovEvent ev = kGovNothing, bool quiet = true) {
    Sec s; s.quiet = quiet; s.play = true; s.fits = fits; s.nativeFps = fps; s.holdsVerdict = holds; s.blocked = blocked; s.ev = ev; return s;
}

// ---- the decision of whether the policy is in force: single player never (the glue calls this; vgx_galaxy.cpp MpInForce) ----
static void testGate() {
    CHECK(InForce(true, true, false, 0));         // switches on, mode 5 under the governor, Performance, the stage's row silent (single player and multiplayer)
    CHECK(!InForce(false, true, false, 0));       // GXV_HRW_EVEN60=0 or "Allow in-between frame rates": the old behaviour (plain 60 with skips)
    CHECK(!InForce(true, false, false, 0));       // not mode 5 under the governor (60 Assist off, a forced run)
    CHECK(!InForce(true, true, true, 0));         // Quality mode
    CHECK(!InForce(true, true, false, 1));        // the stage's row has its own word
    CHECK(Enabled(1, 0) && !Enabled(0, 0) && !Enabled(1, 1) && !Enabled(0, 1));   // GXV_HRW_EVEN60 / GXV_HRW_ALLOW50
    // even60 with 60 Assist OFF (GXV_HRW=0)
    CHECK(InForceNoAssist(true, true, 0, false, 0, false, false));     // eligible: Performance, GXV_HRW=0, no cap, no steady30
    CHECK(!InForceNoAssist(false, true, 0, false, 0, false, false));   // GXV_HRW_EVEN60=0 or GXV_HRW_ALLOW50=1
    CHECK(!InForceNoAssist(true, false, 0, false, 0, false, false));   // GXV_HRW_EVEN60_NOASSIST=0
    CHECK(!InForceNoAssist(true, true, 5, false, 0, false, false));    // GXV_HRW=5 handled by normal InForce
    CHECK(!InForceNoAssist(true, true, 0, true, 0, false, false));     // Quality mode
    CHECK(!InForceNoAssist(true, true, 0, false, 1, false, false));    // per-stage override
    CHECK(!InForceNoAssist(true, true, 0, false, 0, true, false));     // fixed FPS cap owns display
    CHECK(!InForceNoAssist(true, true, 0, false, 0, false, true));     // GXV_STEADY30/SMOOTH60/RATE_AUTO owns cadence
}

// A driver: the stage answers with what it would measure in the mode the policy has now.
struct Trace {
    double fps30 = 29.7, fps60 = 59.5, fpsAssist = 30;   // what the stage reads at an even 30 / plain 60 / 60/30 (native pictures a second)
    bool fits = false, blocked = false;
    double badUntil = 0;                                   // seconds with a first-use hitch: plain 60 reads 51 and the second is not quiet
    double plainFpsLate = -1;                              // >= 0: plain 60 reads this from t >= lateAt
    double lateAt = 0;
    bool reblock = false;                                  // the governor blocks Assist again 3 s after an unblock (a pair that never shows its second pictures)
    int goodFor = 0;                                       // plain 60 reads 58 for this many seconds after each switch to it, then fps60 (a bimodal stage)
};
struct Drive {
    Policy p; Trace tr; int t = 0; bool blockedNow = false; int assistAge = 0, plainAge = 0; std::vector<int> modes; std::vector<int> trialStarts, trialKept, trialFails, waits; int changes = 0;
    void Begin(int start) { p.Configure(P()); p.Begin(0, start); blockedNow = tr.blocked; }
    void Run(int upto) {
        for (; t < upto;) {
            ++t;
            const int m = p.Mode();
            assistAge = m == kAssist ? assistAge + 1 : 0;
            plainAge = m == kPlain60 ? plainAge + 1 : 0;
            Sec s; s.play = true; s.fits = tr.fits; s.blocked = blockedNow;
            if (m == kAssist && tr.reblock && assistAge >= 3) { blockedNow = true; s.blocked = true; s.ev = kGovLeftOther; }
            const bool early = t <= tr.badUntil;
            s.nativeFps = m == kEven30 ? tr.fps30 : m == kAssist ? tr.fpsAssist : early ? 51.0 : plainAge >= 1 && plainAge <= tr.goodFor ? 58.0 : (tr.plainFpsLate >= 0 && t >= tr.lateAt ? tr.plainFpsLate : tr.fps60);
            s.quiet = !early;
            const Out o = p.Tick(t, s);
            if (o.unblock) blockedNow = false;
            if (o.trial == 1) trialStarts.push_back(t);
            if (o.trial == 2) trialKept.push_back(t);
            if (o.trial == 3) { trialFails.push_back(t); waits.push_back(o.nextTrialSec); }
            if (o.changed) ++changes;
            modes.push_back(p.Mode());
        }
    }
};

static void testBeachBowl() {
    // plain 60 dips to 51 in the first 6 s (shaders preparing: not quiet), then 59-60: never drops, in single player
    { Drive d; d.tr.badUntil = 6; d.tr.fits = false; d.Begin(kPlain60); d.Run(300); CHECK(d.changes == 0 && d.p.Mode() == kPlain60); }
    { Drive d; d.tr.badUntil = 6; d.tr.fits = true; d.Begin(kPlain60); d.Run(300); CHECK(d.changes == 0); }
    // the same dip with every second quiet (a plain 51 for 6 s): 6 bad of 10 is not 8: no drop
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60); int ch = 0;
      for (int t = 1; t <= 200; ++t) { Sec s = Play(false, t <= 6 ? 51 : 59.5); if (p.Tick(t, s).changed) ++ch; }
      CHECK(ch == 0); }
    // nothing is judged before 8 s of play, whatever it reads
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60);
      for (int t = 1; t <= 9; ++t) CHECK(!p.Tick(t, Play(false, 40)).changed); }
    // a scene's load seconds (not play) judge nothing and do not count as play
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60);
      for (int t = 1; t <= 50; ++t) { Sec s = Play(false, 40); s.play = false; CHECK(!p.Tick(t, s).changed); } }
}
static void testSustainedDrop() {
    // plain 60 at 51 in every quiet second: dropped when two windows have passed (not before 10 s), to 60/30 if the pair fits, else 30
    { Drive d; d.tr.fps60 = 51; d.tr.fits = true; d.Begin(kPlain60); d.Run(40);
      CHECK(d.modes[8] == kPlain60 && d.p.Mode() == kAssist); }
    { Drive d; d.tr.fps60 = 51; d.tr.fits = false; d.Begin(kPlain60); d.Run(12); CHECK(d.p.Mode() == kEven30); }
    // bad only in the last window (5 of 10 are not 8, and the first half has none): stays
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60); int ch = 0;
      for (int t = 1; t <= 200; ++t) { Sec s = Play(false, (t % 10) >= 5 ? 45 : 60); if (p.Tick(t, s).changed) ++ch; }
      CHECK(ch == 0); }
    // 8 bad of 10 spread over both halves: drops at t = 10 (checked before the 10 s trial at t = 20)
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60); int ch = 0;
      for (int t = 1; t <= 15; ++t) { Sec s = Play(false, (t % 5 == 0) ? 60 : 45); if (p.Tick(t, s).changed) ++ch; }
      CHECK(ch == 1 && p.Mode() == kEven30); }
}
static void testTrial() {
    // a stage that reads 29.7 at 30 but holds 59 at 60 (multiplayer, the pair does not fit): plain 60 by trial within about 15 s, and it stays
    { Drive d; d.tr.fps30 = 29.7; d.tr.fps60 = 59; d.tr.fits = false; d.Begin(kEven30); d.Run(20);
      CHECK(d.p.Mode() == kPlain60 && d.trialStarts.size() == 1 && d.trialKept.size() == 1 && d.trialStarts[0] == 10);
      d.Run(300); CHECK(d.p.Mode() == kPlain60 && d.trialFails.empty()); }
    // the pair is blocked, plain 60 holds: the trial runs all the same (a blocked 60/30 never strands the stage)
    { Drive d; d.tr.fps60 = 59; d.tr.fits = true; d.tr.blocked = true; d.Begin(kEven30); d.Run(25); CHECK(d.p.Mode() == kPlain60); }
    // Dreadnought: about 50 at plain 60, pairs blocked: it stays at 30 with trials backed off 10 / 20 / 30 / 30 (capped at 30 s, never 2 minutes)
    { Drive d; d.tr.fps60 = 50; d.tr.fits = false; d.tr.blocked = true; d.Begin(kPlain60); d.Run(700);
      CHECK(d.p.Mode() == kEven30 || d.p.Trialing());
      CHECK(d.trialKept.empty() && d.trialFails.size() >= 15);
      CHECK(d.waits.size() >= 5 && d.waits[0] == 20 && d.waits[1] == 30 && d.waits[2] == 30 && d.waits[3] == 30 && d.waits[4] == 30);
      // at plain 60 only in the initial drop window + 5 s trials
      int plain = 0; for (int m : d.modes) plain += m == kPlain60; CHECK(plain <= 5 * 22 + 15); }
    // a trial that fails and a stage that then holds: the next trial keeps it and resets trialWait_ back to trialFirst (10 s)
    { Drive d; d.tr.fps60 = 50; d.Begin(kEven30); d.Run(20); CHECK(d.trialFails.size() == 1);
      d.tr.fps60 = 59; d.Run(200); CHECK(d.p.Mode() == kPlain60 && d.trialKept.size() == 1); }
    // a new scene resets the backoff: the first trial is at 10 s again
    { Drive d; d.tr.fps60 = 50; d.Begin(kEven30); d.Run(120); CHECK(d.waits.size() >= 2 && d.waits[1] == 30);
      d.tr.fps60 = 59; d.p.Begin(d.t, kEven30); const int at = d.t; d.trialStarts.clear(); d.Run(at + 20);
      CHECK(d.trialStarts.size() == 1 && d.trialStarts[0] - at == 10 && d.p.Mode() == kPlain60); }
    // a stale "plain 60 holds" verdict keeps nothing: the trial is judged on the measured native rate only
    { Policy p; p.Configure(P()); p.Begin(0, kEven30); bool kept = false;
      for (int t = 0; t < 19; ++t) { Sec s = Play(false, p.Mode() == kEven30 ? 29.7 : 52, p.Mode() == kPlain60); if (p.Tick(t + 1, s).trial == 2) kept = true; }
      CHECK(!kept); }
    // blocked pairs that are tried again and blocked again (Dreadnought): the trials still run on their backed-off schedule capped at 30 s (no stranding at 30)
    { Drive d; d.tr.fps60 = 50; d.tr.fits = true; d.tr.blocked = true; d.tr.reblock = true; d.Begin(kPlain60); d.Run(400);
      CHECK(d.trialKept.empty() && d.trialFails.size() >= 8);
      CHECK(d.waits.size() >= 4 && d.waits[0] == 20 && d.waits[1] == 30 && d.waits[2] == 30); }
    // a bimodal stage (58 for 4 s after the switch, then 50; pairs blocked): succeeded trials reset trialWait_ to trialFirst (10 s) so recovery stays prompt
    { Drive d; d.tr.fps60 = 50; d.tr.goodFor = 4; d.tr.fits = false; d.tr.blocked = true; d.Begin(kEven30); d.Run(200);
      CHECK(d.trialKept.size() >= 4 && d.trialFails.empty()); }
    // plain 60 that stands a minute forgets the backoff
    { Drive d; d.tr.fps60 = 59; d.tr.fits = false; d.Begin(kEven30); d.Run(200); CHECK(d.p.Mode() == kPlain60 && d.trialFails.empty()); }
}
static void testRampAndOff() {
    // multiplayer: the pair fits and nothing is blocked: 60/30 once the stage has settled (8 s of play)
    { Drive d; d.tr.fits = true; d.Begin(kEven30); d.Run(7); CHECK(d.p.Mode() == kEven30); d.Run(20); CHECK(d.p.Mode() == kAssist && d.trialStarts.empty()); }
    // never plain 60 without a trial or the governor's leave: a pair that fits for 60 s and no governor event
    { Drive d; d.tr.fits = true; d.tr.fps60 = 59; d.Begin(kEven30); d.Run(100); CHECK(d.p.Mode() == kAssist); }
    // the governor leaves 60/30 because plain 60 would hold: plain 60; for another reason or blocked: an even 30
    { Policy p; p.Configure(P()); p.Begin(0, kEven30); for (int t = 1; t <= 12; ++t) p.Tick(t, Play(true));
      CHECK(p.Mode() == kAssist);
      Out o = p.Tick(13, Play(true, 30, false, false, kGovLeftPlainHolds)); CHECK(o.byGovernor && o.mode == kPlain60); }
    { Policy p; p.Configure(P()); p.Begin(0, kEven30); for (int t = 1; t <= 12; ++t) p.Tick(t, Play(true));
      Out o = p.Tick(13, Play(false, 30, false, false, kGovLeftOther)); CHECK(o.mode == kEven30);
      Policy q; q.Configure(P()); q.Begin(0, kEven30); for (int t = 1; t <= 12; ++t) q.Tick(t, Play(true));
      CHECK(q.Tick(13, Play(true, 30, false, true)).mode == kEven30); }
    // the governor engages out of plain 60 (with assist=true): ignored before settle (judge), honored once settled
    { Policy p; p.Configure(P()); p.Begin(0, kPlain60);
      Out early = p.Tick(1, Play(true, 50, false, false, kGovEngaged));
      CHECK(!early.changed && p.Mode() == kPlain60);
      for (int t = 2; t <= 8; ++t) CHECK(!p.Tick(t, Play(true, 59.5)).changed);
      Out settled = p.Tick(9, Play(true, 50, false, false, kGovEngaged));
      CHECK(settled.changed && settled.byGovernor && settled.mode == kAssist); }
    // a blocked stage is lifted once in a while
    { Policy p; p.Configure(P()); p.Begin(0, kEven30); int unblocks = 0;
      for (int t = 1; t <= 200; ++t) { Sec s = Play(true, 29.7, false, true); if (p.Tick(t, s).unblock) ++unblocks; }
      CHECK(unblocks >= 1); }
    // the setting: the policy off changes nothing at all
    { Params q = P(); q.on = false; Policy p; p.Configure(q); p.Begin(0, kPlain60);
      for (int t = 1; t <= 300; ++t) CHECK(!p.Tick(t, Play(true, 20)).changed);
      CHECK(!InForce(Enabled(1, 1), true, false, 0)); }
}

// ---- closed loop: the real governor, a stage model ----
struct Stage {
    double game = 12, fe = 10, gpu = 13;   // plain 60, per first picture, ms
    double bShown = 1.0;
    bool engaged = false, even = true;
    vgx::hrw::GovSample Sample(bool plainHolds) {
        (void)plainHolds;
        vgx::hrw::GovSample s;
        s.play = true;
        s.engaged = engaged;
        s.costs.warpMs = 2.0;
        s.costs.beMs = 7.5;
        s.costs.gameDrawnMs = game; s.costs.gameUndrawnMs = std::min(9.0, std::max(4.0, 0.4 * game)); s.costs.feMs = fe;
        if (!engaged && !even) {   // plain 60: every turn drawn
            s.nativeFps = std::min(60.0, 1000.0 / std::max({game, fe, gpu}));
            s.costs.gpuMs = gpu;
        } else if (!engaged) {      // an even 30: one picture in two turns, 30 drawn
            s.nativeFps = 30.0;
            s.costs.gpuMs = gpu;
        } else {                    // the pairs
            s.nativeFps = 30.0;
            s.costs.gpuMs = (gpu + 2.0) / 2.0;
            s.costs.bShown = bShown;
            s.costs.pairGpuMs = gpu + 2.0;
        }
        return s;
    }
};
static vgx::hrw::Governor::Params GP() {   // the numbers of Gov5Params() in vgx_galaxy.cpp
    vgx::hrw::Governor::Params p;
    p.warp = true; p.engageBelowFps = 55; p.settleSec = 4; p.gapSec = 8; p.maxChanges = 6; p.win = 12; p.needFit = 10; p.needLeave = 9; p.ladderWaitSec = 12;
    p.gov77 = true; p.leaveMs = 15.5; p.gpuLimitMs = 16.0;
    return p;
}
struct Loop {
    Stage st; Policy pol; vgx::hrw::Governor gov; std::vector<int> modes; double fpsSeen = 0;
    int skipping60Seconds = 0;   // seconds spent at a plain 60 that ran under the skip limit
    void Begin(double t, int start = kEven30) {
        gov.Configure(GP()); gov.StageBegin(t);
        pol.Configure(P()); pol.Begin(t, start);
        gov.External(t, false, "first"); st.engaged = false; st.even = start == kEven30;
    }
    void Tick(double t) {
        const vgx::hrw::GovSample sm = st.Sample(false);
        const bool plainSample = !st.engaged && !st.even;   // (the second was drawn as plain 60)
        const bool mpEven = pol.Mode() == kEven30;
        const vgx::hrw::GovAction a = mpEven ? vgx::hrw::kGovNone : gov.Tick(t, sm);
        if (a == vgx::hrw::kGovEngage) st.engaged = true;
        else if (a == vgx::hrw::kGovLeave) st.engaged = false;
        static const vgx::hrw::FitParams fit = GP().fit;
        Sec ps; ps.play = true; ps.nativeFps = sm.nativeFps;
        vgx::hrw::Costs pc = sm.costs;
        if (gov.LearnedWarpMs() > 0) pc.warpMs = gov.LearnedWarpMs();
        ps.fits = pc.gameDrawnMs > 0 && pc.gameUndrawnMs > 0 && pc.gpuMs > 0 && vgx::hrw::PairFit(pc, fit).All();
        ps.holdsVerdict = gov.VerdictCode() == vgx::hrw::Governor::kVHolds;
        ps.blocked = gov.Blocked();
        ps.ev = a == vgx::hrw::kGovEngage ? kGovEngaged : a == vgx::hrw::kGovLeave ? (gov.LeftBecausePlainHolds() ? kGovLeftPlainHolds : kGovLeftOther) : kGovNothing;
        const Out o = pol.Tick(t, ps);
        if (o.unblock) gov.Unblock();
        if (o.changed && !o.byGovernor) { gov.External(t, o.mode == kAssist, o.why); st.engaged = o.mode == kAssist; }
        else if (o.changed && o.mode == kEven30) st.engaged = false;
        st.even = pol.Mode() == kEven30;
        modes.push_back(pol.Mode());
        if (plainSample && sm.nativeFps < 55.0) ++skipping60Seconds;
    }
    double First(int mode) const { for (size_t i = 0; i < modes.size(); ++i) if (modes[i] == mode) return static_cast<double>(i + 1); return -1; }
};

static void testLoop() {
    // a light stage in multiplayer: 30, then 60/30, then plain 60 on the governor's leave (or a trial), never a skipping second
    { Loop l; l.st.game = 7; l.st.fe = 6; l.st.gpu = 9; l.Begin(0);
      for (int t = 1; t <= 150; ++t) l.Tick(t);
      CHECK(l.modes.front() == kEven30 && l.First(kPlain60) > 0 && l.modes.back() == kPlain60 && l.skipping60Seconds == 0); }
    // a heavy stage (the game thread at 22 ms): never plain 60 for good
    { Loop l; l.st.game = 22; l.st.fe = 12; l.st.gpu = 13; l.Begin(0);
      for (int t = 1; t <= 200; ++t) l.Tick(t);
      CHECK(l.First(kAssist) > 0 && l.modes.back() != kPlain60); }
    // single player, a light stage: the 60 lock from the first second, for good
    { Loop l; l.st.game = 7; l.st.fe = 6; l.st.gpu = 9; l.Begin(0, kPlain60);
      for (int t = 1; t <= 200; ++t) l.Tick(t);
      CHECK(l.First(kEven30) < 0 && l.First(kAssist) < 0 && l.skipping60Seconds == 0); }
    // single player, about 50 (the game thread at 20 ms): 60 first, 60/30 after two windows
    { Loop l; l.st.game = 20; l.st.fe = 12; l.st.gpu = 13; l.Begin(0, kPlain60);
      for (int t = 1; t <= 100; ++t) l.Tick(t);
      CHECK(l.modes.front() == kPlain60 && l.First(kAssist) > 0 && l.First(kAssist) <= 16 && l.First(kEven30) < 0); }
}


// mp31 / Wave 4: with Params::assist = false (the game's default, GXV_HRW_EVEN60_ASSIST=0) the policy never chooses 60/30,
// and kGovEngaged NEVER drops kPlain60 -> kEven30 (only the sustained 2-window drop rule may drop).
static void testNoAssist() {
    Policy p; Params q; q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
    int t = 0;
    for (; t < 30; ++t) p.Tick(t, Play(true, 45));          // plain 60 skips with the pair fitting: an even 30, not 60/30
    CHECK(p.Mode() == kEven30);
    for (; t < 60; ++t) { p.Tick(t, Play(true, 30)); CHECK(p.Mode() != kAssist); }   // the pair fits at 30: still no 60/30 (a trial of plain 60 may run)
    // Bug #1 fix: kGovEngaged must NOT drop kPlain60 -> kEven30 under !assist
    Policy g; g.Configure(q); g.Begin(0, kPlain60);
    for (int u = 1; u <= 12; ++u) CHECK(!g.Tick(u, Play(false, 60)).changed);
    Out o = g.Tick(13, Play(false, 48, false, false, kGovEngaged));
    CHECK(!o.changed && g.Mode() == kPlain60);
}

// Wave 4: spike immunity, sustained <52 fps drop, succeeded trial backoff reset, 30 s cap, and 3 s fast headroom recovery
static void testWave4SpikeAndFastReturn() {
    // 1. Brief 4 s spike under !assist does NOT drop to 30 even if kGovEngaged fires on every spike second
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
        for (int t = 1; t <= 12; ++t) CHECK(!p.Tick(t, Play(false, 59.5)).changed);
        for (int t = 13; t <= 16; ++t) CHECK(!p.Tick(t, Play(false, 44.0, false, false, kGovEngaged)).changed);
        for (int t = 17; t <= 40; ++t) CHECK(!p.Tick(t, Play(false, 59.5)).changed);
        CHECK(p.Mode() == kPlain60);
    }
    // 2. Minor 53-54 fps dip (above dropBelowFps = 52.0) does NOT drop to 30; sustained 10 s <52 fps DOES drop cleanly
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
        for (int t = 1; t <= 40; ++t) CHECK(!p.Tick(t, Play(false, 53.5)).changed);
        CHECK(p.Mode() == kPlain60);
        int droppedAt = -1;
        for (int t = 41; t <= 60; ++t) {
            if (p.Tick(t, Play(false, 50.0)).changed && p.Mode() == kEven30) { droppedAt = t; break; }
        }
        CHECK(droppedAt == 49 && p.Mode() == kEven30);   // 9 consecutive bad secs = 4 in first half + 5 in second half >= 8
    }
    // 3. Failed trials back off 10 -> 20 -> 30 and cap at 30; passing a trial RESETS trialWait_ back to 10 (NOT doubling!)
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kEven30);
        int t = 0;
        std::vector<int> failWaits;
        // Run until 3 failed trials: waits must be 20, 30, 30
        while (failWaits.size() < 3 && t < 200) {
            ++t;
            Out o = p.Tick(t, Play(false, p.Mode() == kEven30 ? 30.0 : 48.0));
            if (o.trial == 3) failWaits.push_back(o.nextTrialSec);
        }
        CHECK(failWaits.size() == 3 && failWaits[0] == 20 && failWaits[1] == 30 && failWaits[2] == 30);
        // Next trial (after 30 s wait) succeeds at 56.0 fps (exact new trialFps threshold) -> must reset trialWait_ to 10!
        bool kept = false;
        for (int i = 0; i < 40 && !kept; ++i) {
            ++t;
            Out o = p.Tick(t, Play(false, p.Mode() == kEven30 ? 30.0 : 56.0));
            if (o.trial == 2) kept = true;
        }
        CHECK(kept && p.Mode() == kPlain60);
        // Immediately drop back to kEven30 via sustained <52 fps (10 s)
        bool redropped = false;
        for (int i = 0; i < 15 && !redropped; ++i) {
            ++t;
            if (p.Tick(t, Play(false, 48.0)).changed && p.Mode() == kEven30) redropped = true;
        }
        CHECK(redropped);
        const int droppedAt = t;
        // Because the previous trial SUCCEEDED, trialWait_ was reset to 10 s (NOT 30 s)!
        int nextTrialAt = -1;
        for (int i = 0; i < 20 && nextTrialAt < 0; ++i) {
            ++t;
            if (p.Tick(t, Play(false, 30.0)).trial == 1) nextTrialAt = t;
        }
        CHECK(nextTrialAt - droppedAt == 10);
    }
    // 4. Fast headroom return: 3 consecutive quiet seconds of holdsVerdict in kEven30 triggers immediate trial (respecting gapSec)
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kEven30);
        int t = 0;
        // Back off trialWait_ to 30 s first so we prove fast return skips the 30 s timer
        int failsSeen = 0;
        while (failsSeen < 2 && t < 100) {
            ++t;
            if (p.Tick(t, Play(false, p.Mode() == kEven30 ? 30.0 : 48.0)).trial == 3) ++failsSeen;
        }
        CHECK(failsSeen == 2 && p.Mode() == kEven30);
        // Spend 6 s in heavy area (satisfies gapSec=6; still 24 s left on trialWait_=30)
        for (int i = 0; i < 6; ++i) CHECK(!p.Tick(++t, Play(false, 30.0, false)).changed);
        // 2 holds + 1 non-quiet + 2 holds must NOT trigger (consecutive quiet counter resets)
        CHECK(!p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, true)).changed);
        CHECK(!p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, true)).changed);
        CHECK(!p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, false)).changed);
        CHECK(!p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, true)).changed);
        CHECK(!p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, true)).changed);
        CHECK(p.Mode() == kEven30);
        // 3rd consecutive quiet holdsVerdict second triggers immediate trial recovery!
        Out fast = p.Tick(++t, Play(false, 30.0, true, false, kGovNothing, true));
        CHECK(fast.changed && fast.trial == 1 && p.Mode() == kPlain60 && p.Trialing());
        // Trial holds at 59 fps -> stays at 60 fps!
        bool kept = false;
        for (int i = 0; i < 5 && !kept; ++i) {
            if (p.Tick(++t, Play(false, 59.0)).trial == 2) kept = true;
        }
        CHECK(kept && p.Mode() == kPlain60);
    }
    // 5. Fast headroom return respects gapSec (cannot thrash immediately after dropping to kEven30)
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
        int t = 0;
        for (int i = 0; i < 10; ++i) p.Tick(++t, Play(false, 48.0));
        CHECK(p.Mode() == kEven30);
        const int droppedAt = t;
        int trialAt = -1;
        for (int i = 1; i <= 10 && trialAt < 0; ++i) {
            if (p.Tick(++t, Play(false, 30.0, true)).trial == 1) trialAt = t;
        }
        CHECK(trialAt - droppedAt == 6);   // waited full gapSec (6 s), not 3 s, and still faster than trialFirst (10 s)
    }
    // 6. PlainCostsHold60 helper checks (including pipeMs backend bottleneck guard)
    {
        CHECK(PlainCostsHold60(12.0, 10.0, 13.0));
        CHECK(PlainCostsHold60(15.5, 15.5, 15.5, 15.5));
        CHECK(!PlainCostsHold60(15.6, 10.0, 13.0));
        CHECK(!PlainCostsHold60(12.0, 15.6, 13.0));
        CHECK(!PlainCostsHold60(12.0, 10.0, 15.6));
        CHECK(!PlainCostsHold60(12.0, 10.0, 13.0, 15.6));   // backend/pipeline bottleneck blocks fast return
        CHECK(PlainCostsHold60(12.0, 10.0, 13.0, 0.0));     // unmeasured pipeMs does not block
        CHECK(!PlainCostsHold60(0.0, 10.0, 13.0));
        CHECK(!PlainCostsHold60(12.0, 0.0, 13.0));
        CHECK(!PlainCostsHold60(12.0, 10.0, 0.0));
    }
    // 7. Anti-thrash: optimistic holdsVerdict==true at kEven30 while plain 60 runs 51 fps MUST NOT bypass trialWait_ backoff (Carbon Pass 2 Blocking #1)
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kEven30);
        int t = 0, trialStarts = 0, trialFails = 0;
        for (int i = 0; i < 120; ++i) {
            const bool inEven = p.Mode() == kEven30;
            Out o = p.Tick(++t, Play(false, inEven ? 30.0 : 51.0, inEven));   // holdsVerdict stays true in kEven30, plain 60 only hits 51 fps (< 56)
            if (o.trial == 1) ++trialStarts;
            if (o.trial == 3) ++trialFails;
        }
        // First trial may fire via fast return (~8-10 s), then backoff 20 s -> 30 s -> 30 s: <= 4 trials in 120 s (NOT 10 thrashing trials!)
        CHECK(trialStarts >= 3 && trialStarts <= 5);
        CHECK(trialFails == trialStarts);
    }
}

// w25f: opening-camera / cutscene play gate + shaderWorkMs quiet check (Beach Bowl false-30 drop fix, night82/01-E-BBS.log)
static void testPlayAndShaderQuiet() {
    // PlayActive: real play passes, any opening camera / starter / movie / demo / load / pre-play flow leg is blocked
    CHECK(PlayActive(true, false, 0, 0, -2));       // normal SP play (no PETARI_FLOW)
    CHECK(PlayActive(true, false, 0, 0, 120));      // PETARI_FLOW in leg, play turns > 0
    CHECK(PlayActive(true, false, 0, 0, -1));       // PETARI_FLOW outside leg (e.g. AstroDome play)
    CHECK(PlayActive(true, false, 5, 0, -2));       // dialogue outside cutscene (PORT_SKIP_TALK=5)
    CHECK(PlayActive(true, false, 0, 5, -2));       // fast camera during normal play (kFlightFast=5)
    CHECK(!PlayActive(false, false, 0, 0, -2));     // HrwPlay() false (quiet screen, menu, no scene)
    CHECK(!PlayActive(true, true, 0, 0, -2));       // loading active
    CHECK(!PlayActive(true, false, 0, 0, 0));       // PETARI_FLOW leg with 0 play turns (opening camera / demo)
    CHECK(!PlayActive(true, false, 1, 0, -2));      // PORT_SKIP_MOVIE
    CHECK(!PlayActive(true, false, 2, 0, -2));      // PORT_SKIP_OPENING
    CHECK(!PlayActive(true, false, 3, 0, -2));      // PORT_SKIP_STARTER
    CHECK(!PlayActive(true, false, 4, 0, -2));      // PORT_SKIP_DEMO (MR::isDemoActive)
    CHECK(!PlayActive(true, false, 0, 1, -2));      // kFlightOpening (even if cutscene_skip stale after hitch)
    CHECK(!PlayActive(true, false, 0, 2, -2));      // kFlightStarter
    CHECK(!PlayActive(true, false, 0, 3, -2));      // kFlightStarGet
    CHECK(!PlayActive(true, false, 0, 4, -2));      // kFlightDemo

    // ShaderQuiet: 366 ms of shader work with 0 compiles and 0 links (night82/01-E-BBS.log) MUST NOT be quiet,
    // while sub-millisecond timer/slice overhead (< 1.0 ms) MUST stay quiet so GXV_NO_MIDHOLD=1 doesn't stall even60.
    CHECK(ShaderQuiet(10, 10, 5, 5, 120.0, 120.0));
    CHECK(ShaderQuiet(10, 10, 5, 5, 120.5, 120.0));   // sub-ms overhead stays quiet
    CHECK(!ShaderQuiet(10, 10, 5, 5, 121.0, 120.0));  // >= 1.0 ms shader work is NOT quiet
    CHECK(!ShaderQuiet(10, 10, 5, 5, 486.0, 120.0));  // +366 ms shaderWorkMs, 0 compiles, 0 links
    CHECK(!ShaderQuiet(11, 10, 5, 5, 120.0, 120.0));  // compile without ms
    CHECK(!ShaderQuiet(10, 10, 6, 5, 120.0, 120.0));  // link without ms

    // Beach Bowl night82 closed-loop replay (Policy + real Governor with vgx_galaxy.cpp govSm.play gate):
    // 15 s opening camera at 45 fps (flowPlayTurns=0, skipKind=2), then 12 s of early play with 366 ms shader work
    // (0 compiles/links) at 48 fps, then 60 s of quiet play at 59.5 fps (with 0.05 ms/s slice timer noise).
    // Neither Policy nor Governor may drop to 30 or engage Assist, and wantEngaged must stay false throughout.
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
        vgx::hrw::Governor gov; gov.Configure(GP()); gov.StageBegin(0); gov.External(0, false, "first");
        bool wantEngaged = false;
        double swMs = 100.0;
        int drops = 0, govEngages = 0;
        for (int t = 1; t <= 87; ++t) {
            const bool opening = t <= 15;
            const bool shaderBusy = t > 15 && t <= 27;
            const double prevSw = swMs;
            if (shaderBusy) swMs += 30.5;  // 12 * 30.5 = 366 ms with 0 compiles & 0 links
            else swMs += 0.05;             // sub-ms timer/slice noise
            Sec s;
            s.play = PlayActive(true, false, opening ? 2 : 0, opening ? 1 : 0, opening ? 0 : (t - 15) * 60);
            s.quiet = ShaderQuiet(0, 0, 0, 0, swMs, prevSw);
            s.nativeFps = opening ? 45.0 : (shaderBusy ? 48.0 : 59.5);
            s.fits = false;
            vgx::hrw::GovSample govSm;
            govSm.play = s.play && s.quiet && !p.Trialing() && (p.Mode() == kAssist || p.Settled());
            govSm.nativeFps = s.nativeFps;
            govSm.costs.gameDrawnMs = opening ? 18.0 : (shaderBusy ? 18.0 : 12.0);
            govSm.costs.gameUndrawnMs = 6.0;
            govSm.costs.feMs = 8.0;
            govSm.costs.gpuMs = 13.0;
            govSm.costs.warpMs = 2.0;
            govSm.costs.beMs = 7.5;
            const vgx::hrw::GovAction a = p.Mode() == kEven30 ? vgx::hrw::kGovNone : gov.Tick(t, govSm);
            if (a == vgx::hrw::kGovEngage) ++govEngages;
            s.ev = a == vgx::hrw::kGovEngage ? kGovEngaged
                 : a == vgx::hrw::kGovLeave ? (gov.LeftBecausePlainHolds() ? kGovLeftPlainHolds : kGovLeftOther) : kGovNothing;
            const Out o = p.Tick(t, s);
            if (o.changed) {
                ++drops;
                if (!o.byGovernor) gov.External(t, o.mode == kAssist, o.why);
                wantEngaged = (o.mode == kAssist);
            }
            CHECK(wantEngaged == (p.Mode() == kAssist));
        }
        CHECK(drops == 0 && govEngages == 0 && p.Mode() == kPlain60 && !wantEngaged);
    }

    // Sub-ms shaderWorkMs creep must NOT prevent a genuinely slow SP stage from dropping to kEven30 (checked before the 15 s trial).
    {
        Policy p; Params q = P(); q.assist = false; p.Configure(q); p.Begin(0, kPlain60);
        double swMs = 10.0;
        for (int t = 1; t <= 18; ++t) {
            const double prevSw = swMs;
            swMs += 0.05;  // 50 us/s overhead
            Sec s = Play(false, 46.0);
            s.quiet = ShaderQuiet(0, 0, 0, 0, swMs, prevSw);
            p.Tick(t, s);
        }
        CHECK(p.Mode() == kEven30);
    }
}

int main() {
    testNoAssist();
    testWave4SpikeAndFastReturn();
    testGate();
    testPlayAndShaderQuiet();
    testBeachBowl();
    testSustainedDrop();
    testTrial();
    testRampAndOff();
    testLoop();
    if (fails) { printf("hrw_mp_test: %d FAILED\n", fails); return 1; }
    printf("hrw_mp_test: all passed\n");
    return 0;
}
