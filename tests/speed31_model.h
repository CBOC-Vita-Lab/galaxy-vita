// speed31 host model of the game thread, the retrace wait and the display (shared by tests/speed31_test.cpp and tests/speed31_loop_test.cpp). It runs the REAL pacing classes
// (port::pace::Controller Fixed30 for Quality, LegacySkip + Slip for Performance) and the real guard. Same physics as tests/speed_guard_test.cpp (speed30):
//  - a turn costs D (drawn) or N (undrawn) ms of work; after it, unless the pacer said "late" (code 2), the game waits for the retrace: the game's retrace queue holds ONE message,
//    so the wait returns at once when a vblank has come since the last wait returned, else at the next vblank;
//  - pictures are shown on the first vblank after their drawn turn ended (Quality: at least 2 vblanks after the one before: PresentationSlots; Performance: 1);
//  - the back end is never the limit (pending 0): the game thread's own pacing.
#pragma once
#include "port/frame_pace.h"
#include "port/speed_guard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>
namespace m31 {
using namespace port::pace;
static constexpr int64_t P = SpeedGuard::kPeriodNs;
inline int64_t Ms(double ms) { return static_cast<int64_t>(ms * 1e6); }

struct Scn {
    std::function<double(double)> pair = [](double) { return 36.0; };   // the pair cost (D + N) at that second
    double N = 12.0;                  // an undrawn turn's work, ms
    double seconds = 60;
    double jitter = 0;                // +- fraction of the cost, uniform
    double hitchAtS = -1, hitchMs = 0;
    double stallAtS = -1, stallMs = 0;
    double loadAtS = -1, loadS = 0;
    unsigned slipMax = 20;            // GXV_SLIP_MAX (0 = no slip)
    bool steady = true;               // Performance: the steady 30's offers
    bool quality = true;
    bool guard = false;
    bool esc = true, own = true, snap = true;
    unsigned maxShare = 100, maxExtra = 1, maxExtraEsc = 8;
    long budgetMs = 30, leakPm = 10;
    unsigned seed = 12345;
    bool slipBand = false; unsigned slipBandPm = 45; double slipBandS = 10;   // (pace32: GXV_SPEED_GUARD_SLIP; off = speed31 exactly)
    // the closed loop: perTurn is called at every turn start (after the guard's BeginTurn) and returns the cut in force (ms off the pair); afterTurn after the turn's cost is known; onPicture when a
    // picture is shown (its hold in refreshes). The Quality ladder / governors are driven from these in tests/speed31_loop_test.cpp.
    std::function<double(int64_t tNs, SpeedGuard&)> perTurn;
    std::function<void(int64_t tNs, int code, bool extra, double costMs)> afterTurn;
    std::function<void(int64_t tNs, unsigned hold)> onPicture;
};
struct Res {
    double speed = 0, fps = 0, extras = 0, wall = 0, worst1s = 100, worst5s = 100;
    double h2 = 0, h3 = 0, h4 = 0;    // display holds: share of pictures held <= base / base+1 / more (Quality base 2, Performance 1)
    unsigned longestHold = 0, longestDropRun = 0;
    double dropPct = 0;               // share of the pictures that took an extra turn
    double lostMs = 0;
    uint64_t hash = 1469598103934665603ull;
    int maxBucketMs = 0;
    std::vector<double> turnsPerPic;  // histogram share 2,3,4,5,6,7,8,9,10+ (index = turns-2)
    std::vector<int64_t> turnStart;   // turn starts (ns)
    std::vector<double> pairAt;       // the pair cost in force at each second
    std::vector<unsigned> ks;         // turns per picture, in order
    unsigned changesPerMin = 0;       // changes of the turns-per-picture value, per minute
    std::vector<std::pair<double, unsigned>> holds;   // (pace32) every picture after the first: the second it was shown, the refreshes the one before it was held
    double slipS = 0, slipLostMs = 0; unsigned slipStarts = 0, drops = 0;   // (pace32) the slip band's census; pictures that took an extra turn
};

inline Res Run(const Scn& s) {
    Res r;
    Controller ctrl(false, Policy::Fixed30);
    Slip slip;
    if (s.slipMax) slip.Configure(s.slipMax);
    ctrl.UseSlip(&slip);
    LegacySkip rule(3, 2, 6, &slip, 2, 0);
    SpeedGuard g;
    {
        SpeedGuard::Config c = MakeSpeedGuardConfigPm(s.guard, s.budgetMs, s.maxShare, s.maxExtra, 3000, s.leakPm);
        c.escalate = s.esc; c.own = s.own; c.snap = s.snap; c.maxExtraEsc = s.maxExtraEsc;
        c.slip = s.slipBand && s.guard && s.esc; c.slipBandPermille = s.slipBandPm; c.slipMaxNs = static_cast<int64_t>(s.slipBandS * 1e9);
        g.Configure(c);
    }
    bool lastDrew = false;
    uint32_t seed = s.seed;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / static_cast<double>(1 << 24); };
    int64_t t = 5 * P, tr = 0;
    const int64_t end = Ms(s.seconds * 1000) + t;
    std::vector<int64_t> ready;
    std::vector<unsigned> turnsOf;
    unsigned since = 0;
    bool hitchDone = false, stallDone = false;
    uint64_t turnNo = 0;
    double cutMs = 0;
    PresentationSlots slotsOn;
    int64_t lastShownOn = -1;
    std::vector<int64_t> pairLog;
    while (t < end) {
        const double sec = (t - 5 * P) / 1e9;
        const bool loading = s.loadAtS >= 0 && sec >= s.loadAtS && sec < s.loadAtS + s.loadS;
        const bool allowed = !loading;
        const bool quiet = loading;
        if (slip.Enabled()) slip.Begin(static_cast<uint64_t>(t / 1000), allowed);
        g.BeginTurn(t, allowed, quiet);
        if (s.perTurn) cutMs = s.perTurn(t, g);
        int code;
        if (s.quality) {
            if (s.guard) { ctrl.SetOwns(g.Owns()); ctrl.SetExtra(g.WantExtra()); }
            code = ctrl.Turn(static_cast<uint64_t>(t / 1000), allowed, quiet, 0, 0, 0);
            if (ctrl.ExtraTaken()) g.MarkExtra();
        } else {
            if (s.guard) rule.SetOwns(g.Owns());
            LegacySkip::Verdict v = rule.Turn(t, allowed, 0);
            code = v.code;
            if (s.steady && allowed && lastDrew && code == 0) code = 1;
            if (s.guard) {
                const int before = code;
                code = g.Filter(code, allowed);
                if (before == 0 && code != 0) rule.ExtraTaken();
            }
        }
        g.EndDecision(code);
        r.maxBucketMs = std::max(r.maxBucketMs, static_cast<int>(g.BucketNs() / 1000000));
        lastDrew = code == 0;
        r.hash = (r.hash ^ static_cast<uint64_t>(code + 1)) * 1099511628211ull;
        const double pairNow = std::max(s.N + 1.0, s.pair(sec) - cutMs);
        double cost = code == 0 ? pairNow - s.N : s.N;
        if (loading) cost = 30.0;
        else if (s.jitter > 0) cost *= 1.0 + s.jitter * (2 * rnd() - 1);
        if (!hitchDone && s.hitchAtS >= 0 && sec >= s.hitchAtS) { cost = s.hitchMs; hitchDone = true; }
        if (!stallDone && s.stallAtS >= 0 && sec >= s.stallAtS) { cost = s.stallMs; stallDone = true; }
        const int64_t workEnd = t + Ms(cost);
        if (s.slipMax) slip.End(static_cast<uint64_t>(workEnd / 1000), code == 0);
        r.turnStart.push_back(t);
        ++since; ++turnNo;
        if (s.afterTurn) s.afterTurn(t, code, g.ExtraNow(), cost);
        if (code == 0) {
            ready.push_back(workEnd); turnsOf.push_back(since); since = 0;
            const int64_t v = workEnd / P + 1;
            int64_t sh;
            if (s.quality) sh = slotsOn.Next(static_cast<uint32_t>(v), 2), slotsOn.Shown(static_cast<uint32_t>(sh));
            else sh = std::max<int64_t>(v, lastShownOn + 1);
            if (lastShownOn >= 0 && s.onPicture) s.onPicture(workEnd, static_cast<unsigned>(sh - lastShownOn));
            if (lastShownOn >= 0) r.holds.emplace_back(sec, static_cast<unsigned>(sh - lastShownOn));
            lastShownOn = sh;
        }
        int64_t ret = workEnd;
        if (code != 2) {
            const int64_t v1 = (tr / P + 1) * P;
            ret = std::max(workEnd, v1);
            tr = ret;
        }
        t = ret;
    }
    const SpeedGuard::Census tot = g.Total();
    r.wall = (t - 5 * P) / 1e9;
    r.speed = 100.0 * turnNo * P / (t - 5 * P);
    r.lostMs = tot.lostNs() / 1e6;
    r.extras = static_cast<double>(tot.extras);
    r.slipS = tot.slipNs / 1e9; r.slipLostMs = tot.slipLostNs / 1e6; r.slipStarts = tot.slipStarts; r.drops = static_cast<unsigned>(tot.droppedPics);
    r.fps = ready.size() / r.wall;
    // worst 1 s / 5 s game speed from the turn starts (after the first 4 s: warm-up): the census's rule, n turns whose lengths add up to at least the span, speed = n * 16.683 ms / their length
    for (int span : {1, 5}) {
        double worst = 100;
        const size_t N = r.turnStart.size();
        const int64_t t0 = N ? r.turnStart.front() + Ms(4000) : 0;
        size_t i = 0;
        while (i < N && r.turnStart[i] < t0) ++i;
        for (size_t j = i; j < N; ++j) {   // window = turns j .. k-1 where start[k] - start[j] >= span
            const int64_t a = r.turnStart[j];
            const auto kk = std::lower_bound(r.turnStart.begin() + j, r.turnStart.end(), a + span * 1000000000ll);
            if (kk == r.turnStart.end()) break;
            const double a_s = (a - 5 * P) / 1e9;
            if (s.loadAtS >= 0 && a_s + span + 0.2 > s.loadAtS && a_s < s.loadAtS + s.loadS + 3.2) continue;
            if (s.stallAtS >= 0 && a_s + span + 0.2 > s.stallAtS && a_s < s.stallAtS + 1.0) continue;
            const double sp = 100.0 * static_cast<double>(kk - (r.turnStart.begin() + j)) * P / static_cast<double>(*kk - a);
            worst = std::min(worst, sp);
        }
        (span == 1 ? r.worst1s : r.worst5s) = worst;
    }
    PresentationSlots slots;
    std::vector<int64_t> shown;
    int64_t lastShown = -1;
    for (size_t i = 0; i < ready.size(); ++i) {
        int64_t v = ready[i] / P + 1, sh;
        if (s.quality) sh = slots.Next(static_cast<uint32_t>(v), 2), slots.Shown(static_cast<uint32_t>(sh));
        else sh = std::max<int64_t>(v, lastShown + 1);
        lastShown = sh;
        shown.push_back(sh);
    }
    const size_t skip = ready.size() > 400 ? 200 : 0;
    unsigned n = 0, drop = 0, run = 0;
    r.turnsPerPic.assign(9, 0.0);
    unsigned prevK = 0, chg = 0;
    for (size_t i = skip; i < turnsOf.size(); ++i) {
        ++n;
        const unsigned k = turnsOf[i];
        r.turnsPerPic[std::min<unsigned>(8, k >= 2 ? k - 2 : 0)] += 1;
        if (k >= 3) { ++drop; ++run; r.longestDropRun = std::max(r.longestDropRun, run); } else run = 0;
        if (prevK && k != prevK) ++chg;
        prevK = k;
        r.ks.push_back(k);
    }
    for (double& x : r.turnsPerPic) x = n ? 100.0 * x / n : 0;
    r.dropPct = n ? 100.0 * drop / n : 0;
    r.changesPerMin = static_cast<unsigned>(n ? chg * (60.0 * r.fps / n) : 0);
    const unsigned base = s.quality ? 2 : 1;
    unsigned nh = 0, h2 = 0, h3 = 0, h4 = 0;
    for (size_t i = skip + 1; i < shown.size(); ++i) {
        const unsigned h = static_cast<unsigned>(shown[i] - shown[i - 1]);
        ++nh;
        if (h <= base) ++h2; else if (h == base + 1) ++h3; else ++h4;
        r.longestHold = std::max(r.longestHold, h);
    }
    r.h2 = nh ? 100.0 * h2 / nh : 0; r.h3 = nh ? 100.0 * h3 / nh : 0; r.h4 = nh ? 100.0 * h4 / nh : 0;
    return r;
}
}  // namespace m31
