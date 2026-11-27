// GXV_PACE24 (pace24): the steady-30 rule of frame_pace.h SteadyCadence with the display's holds (HoldMeter), on the same model of the console's
// pipeline as tests/steady_cadence_model.cpp (a model, not a console measurement: the real LegacySkip, SteadyCadence and PresentSync classes
// run on costs a scene sets). The scenes come from the c27 console logs:
//   * "766 trace": the pictures-a-second of job 766's 5 s windows (Honeyhive at Performance defaults: 54 60 50 50 54 48 55 45 | 52 54 47 | 39 x6 | 35)
//     each turned into the front-end cost that gives that rate on the model, played in order;
//   * "760 soak": a heavy area (about 48 a second), a camera cut into a light one (60), again, as the 17-minute hop through seven galaxies did.
// Three rules are compared: TODAY (the defaults of the 2.3 build: engages under 38 for 3 s, releases at 45 for 4 s, seamless: release at a cut or
// quiet moment, quick re-engage within 20 turns), PACE23 (50 / 57 / 2.5 s / 3 s, the earlier proposal) and PACE24. Measured on the pictures the
// display callback shows: breaks (a picture held for a different number of vblanks than the one before it), seconds with 7+ breaks, pictures a
// second, the steady 30's state changes, releases undone within 100 ms.
//   g++ -std=c++17 -O1 -I platform/include tests/pace24_model.cpp -o /tmp/pace24_model && /tmp/pace24_model
#include "port/frame_pace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kV = 16.68335;  // ms per vblank
using Params = port::pace::SteadyCadenceParams;

Params Today() {  // (entry.cpp at 83b0397: GXV_STEADY30=1, GXV_SEAMLESS=1, GXV_PACE23=0)
    Params p;
    p.belowFps = 38.0; p.aboveFps = 45.0; p.engageSec = 3.0; p.releaseSec = 4.0;
    p.cutRelease = true; p.cutWaitSec = 15.0; p.quickTurns = 20;
    return p;
}
Params Pace23() {
    Params p = Today();
    p.belowFps = 50.0; p.aboveFps = 57.0; p.engageSec = 2.5; p.releaseSec = 3.0;
    return p;
}
Params Pace24() {  // (perf.cpp with GXV_PACE24=1 and the other GXV_PACE24_* at their defaults)
    Params p;
    p.p24 = true; p.window = 30; p.breakFps = 7.0;
    p.belowFps = 50.0; p.aboveFps = 57.0;
    p.engageSec = 1.5; p.releaseSec = 2.0; p.dwellSec = 8.0; p.blockSec = 10.0; p.maxBlockSec = 120.0; p.confirmSec = 6.0;
    p.cutRelease = true; p.cutWaitSec = 8.0; p.quickTurns = 24;
    return p;
}

struct Scene {
    double logicMs = 6, drawMs = 4;
    double feMs = 14, beMs = 9, gpuMs = 11;
    double sd = 0.12;
    bool lowerable = false;  // (the controller has nothing left to lower: the cadence is the way out)
};

struct Model {
    port::pace::LegacySkip rule{5, 2, 6, nullptr, 3};
    port::pace::SteadyCadence cad;
    port::pace::PresentSync sync;
    port::pace::HoldMeter meter;
    std::mt19937 rng;
    double now = 0, feFree = 0, beFree = 0, gpuFree = 0, cbFree = 0;
    std::deque<double> beDone, cbReturn;
    uint32_t lastShown = 0, prevHold = 0;
    bool haveShown = false, haveHold = false;
    // census
    uint64_t pictures = 0, breaks = 0, onSteady = 0, hold[5] = {};
    unsigned changes = 0, undone = 0, releases = 0, engages = 0;
    double lastReleaseMs = -1e9, steadyMs = 0, lastStateMs = 0;
    std::vector<unsigned> secBreaks;  // breaks per second of play
    double secStart = 0;
    unsigned curSecBreaks = 0;
    bool wasEngaged = false;
    Model(const Params& p, unsigned seed) : rng(seed) {
        cad.Configure(true, p);
        if (p.p24) cad.UseMeter(&meter);
        sync.Configure(true, 1, 5, 3);
    }
    double Cost(double mean, double sd) {
        std::normal_distribution<double> d(mean, mean * sd);
        const double v = d(rng);
        return v < mean * 0.3 ? mean * 0.3 : v;
    }
    void Mark() {
        pictures = breaks = onSteady = 0;
        for (uint64_t& h : hold) h = 0;
        secBreaks.clear();
        curSecBreaks = 0;
        secStart = now;
        changes = undone = releases = engages = 0;
        steadyMs = 0;
        lastStateMs = now;
    }
    void Picture(uint32_t target) {
        if (haveShown) {
            const uint32_t h = target - lastShown;
            if (h <= 8) {
                ++pictures;
                ++hold[h >= 4 ? 4 : (h < 1 ? 1 : h)];
                if (cad.Engaged()) ++onSteady;
                if (haveHold && h != prevHold) { ++breaks; ++curSecBreaks; }
                prevHold = h;
                haveHold = true;
            } else {
                haveHold = false;
            }
        }
        meter.Shown(target);
        lastShown = target;
        haveShown = true;
    }
    double slot = 0;
    void Turn(const Scene& s, bool allowed = true, bool quiet = false) {
        const double start = now;
        while (!beDone.empty() && beDone.front() <= start) beDone.pop_front();
        const unsigned pending = static_cast<unsigned>(beDone.size());
        auto v = rule.Turn(static_cast<int64_t>(start * 1e6), allowed, pending);
        cad.Stages(true, static_cast<uint32_t>(s.gpuMs * 1000.0), static_cast<uint32_t>(std::fmax(s.gpuMs, std::fmax(s.feMs, s.beMs)) * 1000.0), s.lowerable);
        const bool was = cad.Engaged();
        const bool offered = cad.Begin(static_cast<uint64_t>(start * 1000.0), allowed, quiet);
        if (was && !cad.Engaged()) { ++releases; ++changes; lastReleaseMs = start; }  // (a release made in Begin: at a cut)
        if (allowed && !offered && v.code == 0) v.code = 1;
        cad.Decision(offered || !allowed, v.code == 0);
        double work = Cost(s.logicMs, s.sd);
        if (v.code == 0) {
            const double ready = start + work;
            if (feFree > ready) work += feFree - ready;
            const double fe0 = start + work;
            work += Cost(s.drawMs, s.sd);
            const double fe1 = std::fmax(fe0 + Cost(s.feMs, s.sd), start + work);
            feFree = fe1;
            double be1 = std::fmax(fe1, beFree) + Cost(s.beMs, s.sd);
            if (cbReturn.size() >= 2) be1 = std::fmax(be1, cbReturn[cbReturn.size() - 2]);
            beFree = be1;
            beDone.push_back(be1);
            const double g1 = std::fmax(std::fmax(be1 - 6.0, gpuFree) + Cost(s.gpuMs, s.sd), be1);
            gpuFree = g1;
            const double cb = std::fmax(g1, cbFree);
            const uint32_t earliest = static_cast<uint32_t>(std::floor(cb / kV)) + 1;
            const uint32_t stamp = static_cast<uint32_t>(std::floor(start / kV + 1e-9));
            const uint32_t target = sync.Target(earliest, true, static_cast<uint16_t>(stamp));
            sync.Shown(target, target != earliest);
            cbFree = target != earliest ? (target - 1) * kV + 0.2 : cb + 0.1;
            cbReturn.push_back(cbFree);
            if (cbReturn.size() > 4) cbReturn.pop_front();
            if (!quiet) Picture(target);
        }
        const double end = start + work;
        const bool before = cad.Engaged();
        cad.End(static_cast<uint64_t>(end * 1000.0));
        slot += 1.0;
        if (end > (slot + 4.0) * kV) slot = std::floor(end / kV);
        now = v.code == 2 ? end : std::fmax(end, slot * kV);
        if (cad.Engaged() != before) {
            ++changes;
            if (cad.Engaged()) { ++engages; if (now - lastReleaseMs < 100.0) ++undone; } else ++releases;
        }
        if (cad.Engaged()) steadyMs += now - (lastStateMs > now ? now : lastStateMs);
        lastStateMs = now;
        while (now - secStart >= 1000.0) { secBreaks.push_back(curSecBreaks); curSecBreaks = 0; secStart += 1000.0; }
    }
    void Run(const Scene& s, double seconds) {
        const double end = now + seconds * 1000.0;
        while (now < end) Turn(s);
    }
    void Cut() { cad.NoteCut(static_cast<uint64_t>(now * 1000.0)); }
    double Rate(double seconds) const { return pictures / seconds; }
    unsigned IrregularSeconds() const { unsigned n = 0; for (unsigned b : secBreaks) n += b >= 7; return n; }
};

// The front-end cost that gives about `fps` pictures a second on the model with no cadence (found once by running it).
double FeFor(double fps) {
    static std::vector<std::pair<double, double>> table;
    if (table.empty()) {
        for (double fe = 13.0; fe <= 36.0; fe += 0.5) {
            Scene s;
            s.feMs = fe;
            Params off = Today();
            off.belowFps = 1.0;  // (never engages)
            Model m(off, 11);
            m.Run(s, 6);
            m.Mark();
            m.Run(s, 12);
            table.emplace_back(m.Rate(12.0), fe);
        }
    }
    double best = table.front().second, bd = 1e9;
    for (const auto& t : table)
        if (std::fabs(t.first - fps) < bd) { bd = std::fabs(t.first - fps); best = t.second; }
    return best;
}

struct Result { double rate, breaksPerMin, irregularPct, steadyPct; unsigned changes, undone; };
Result Play(const Params& p, const std::vector<std::pair<double, double>>& trace, unsigned seed, bool cuts) {
    Model m(p, seed);
    Scene s;
    s.feMs = FeFor(trace.front().second);
    m.Run(s, 3);
    m.Mark();
    double total = 0;
    for (const auto& seg : trace) {
        s.feMs = FeFor(seg.second);
        if (cuts) m.Cut();
        m.Run(s, seg.first);
        total += seg.first;
    }
    Result r;
    r.rate = m.pictures / total;
    r.breaksPerMin = m.breaks * 60.0 / total;
    r.irregularPct = m.secBreaks.empty() ? 0.0 : 100.0 * m.IrregularSeconds() / m.secBreaks.size();
    r.steadyPct = m.pictures ? 100.0 * m.onSteady / m.pictures : 0.0;
    r.changes = m.changes;
    r.undone = m.undone;
    return r;
}
void Print(const char* name, const Result& r) {
    std::printf("    %-8s %5.1f pictures a second, %6.1f breaks a minute, %4.1f %% of the seconds with 7+ breaks, %4.1f %% of the pictures on the steady 30, "
                "%u state changes, %u releases undone within 100 ms\n", name, r.rate, r.breaksPerMin, r.irregularPct, r.steadyPct, r.changes, r.undone);
}
}  // namespace

int main() {
    std::printf("calibration: the model with no cadence, front end 17 ms (about 50 a second, as 766 Honeyhive play: held 1/2/3 73/25/2 %%)\n");
    {
        Params off = Today();
        off.belowFps = 1.0;
        Model m(off, 3);
        Scene s;
        s.feMs = FeFor(50.0);
        m.Run(s, 6);
        m.Mark();
        m.Run(s, 30);
        const double all = static_cast<double>(m.hold[1] + m.hold[2] + m.hold[3] + m.hold[4]);
        std::printf("    fe %.1f ms: %.1f pictures a second, held 1/2/3/4+ %.0f/%.0f/%.0f/%.0f %%, %.1f breaks a second\n", s.feMs, m.Rate(30.0), 100.0 * m.hold[1] / all,
                    100.0 * m.hold[2] / all, 100.0 * m.hold[3] / all, 100.0 * m.hold[4] / all, m.breaks / 30.0);
        Check(m.Rate(30.0) > 46.0 && m.Rate(30.0) < 54.0 && m.hold[1] > m.hold[2] && m.breaks / 30.0 >= 7.0, "the model reproduces the log: ~50 a second, mostly 1-vblank holds, 7+ breaks a second");
    }

    // The 5 s windows of job 766 (03:36:36 ... 03:39:12), shown pictures a second. The 28.0 (a load) and the steady-30 windows (29.6-35.1, 30.x)
    // are left out: the cadence's own numbers are not a scene's; the heavy part (38-39) stays.
    const double w766[] = {54.1, 60.1, 50.3, 50.5, 54.2, 47.6, 54.6, 45.1, 52.4, 53.6, 46.8, 38.8, 38.4, 37.8, 39.0, 38.4, 39.0, 53.7, 55.4, 53.5, 52.9, 53.7, 53.9, 53.8, 54.3, 54.2};
    std::vector<std::pair<double, double>> trace766;
    for (double f : w766) trace766.emplace_back(5.0, f);
    std::printf("the 766 trace (Honeyhive, %zu windows of 5 s; the rungs' own changes are not modelled), mean of 4 seeds\n", trace766.size());
    Result today{}, p23{}, p24{};
    auto add = [](Result& a, const Result& b) { a.rate += b.rate / 4; a.breaksPerMin += b.breaksPerMin / 4; a.irregularPct += b.irregularPct / 4; a.steadyPct += b.steadyPct / 4; a.changes += b.changes; a.undone += b.undone; };
    for (unsigned seed = 1; seed <= 4; ++seed) {
        add(today, Play(Today(), trace766, seed, false));
        add(p23, Play(Pace23(), trace766, seed, false));
        add(p24, Play(Pace24(), trace766, seed, false));
    }
    today.changes /= 4; p23.changes /= 4; p24.changes /= 4; today.undone /= 4; p23.undone /= 4; p24.undone /= 4;
    Print("TODAY", today);
    Print("PACE23", p23);
    Print("PACE24", p24);
    {
        char line[240];
        std::snprintf(line, sizeof line, "PACE24 breaks a minute %.0f against %.0f today (%.0f %% fewer), at %.1f pictures a second against %.1f", p24.breaksPerMin, today.breaksPerMin,
                      100.0 * (1.0 - p24.breaksPerMin / today.breaksPerMin), p24.rate, today.rate);
        Check(p24.breaksPerMin <= today.breaksPerMin * 0.75, line);
        std::snprintf(line, sizeof line, "and the seconds with 7+ breaks fall from %.0f %% to %.0f %%", today.irregularPct, p24.irregularPct);
        Check(p24.irregularPct < today.irregularPct, line);
        Check(p24.changes <= 4, "the steady 30 changes state at most 4 times in the trace (no flapping)");
    }

    // 760 soak: a heavy area (CPU-held at ~38 a second), a light one (the scene can do 60) entered at a camera cut, with two more cuts in it, repeated.
    std::printf("the 760 soak: heavy (38 a second) 25 s, light (60) 35 s with camera cuts at 6 s and 20 s, three times\n");
    {
        Result a{}, b{}, c{};
        for (unsigned seed = 1; seed <= 4; ++seed) {
            Model mt(Today(), seed), m3(Pace23(), seed), m4(Pace24(), seed);
            Model* ms[3] = {&mt, &m3, &m4};
            Scene s;
            for (Model* m : ms) { s.feMs = FeFor(38.0); m->Run(s, 3); m->Mark(); }
            double total = 0;
            for (int rep = 0; rep < 3; ++rep) {
                s.feMs = FeFor(38.0);
                for (Model* m : ms) m->Run(s, 25.0);
                s.feMs = FeFor(60.0);
                for (Model* m : ms) { m->Cut(); m->Run(s, 6.0); m->Cut(); m->Run(s, 14.0); m->Cut(); m->Run(s, 15.0); }
                total += 60.0;
            }
            Result* rs[3] = {&a, &b, &c};
            for (int k = 0; k < 3; ++k) {
                Model& m = *ms[k];
                rs[k]->rate += m.pictures / total / 4;
                rs[k]->breaksPerMin += m.breaks * 60.0 / total / 4;
                rs[k]->steadyPct += (m.pictures ? 100.0 * m.onSteady / m.pictures : 0.0) / 4;
                rs[k]->irregularPct += (m.secBreaks.empty() ? 0.0 : 100.0 * m.IrregularSeconds() / m.secBreaks.size()) / 4;
                rs[k]->changes += m.changes;
                rs[k]->undone += m.undone;
            }
        }
        a.changes /= 4; b.changes /= 4; c.changes /= 4; a.undone /= 4; b.undone /= 4; c.undone /= 4;
        Print("TODAY", a);
        Print("PACE23", b);
        Print("PACE24", c);
        Check(a.undone >= 1, "today: a release at a cut is undone within 100 ms (the stale window's quick-engage check)");
        Check(c.undone == 0, "PACE24: no release is undone within 100 ms");
        char line[240];
        std::snprintf(line, sizeof line, "PACE24 shows %.1f pictures a second where today shows %.1f (the light areas leave the steady 30 at a cut)", c.rate, a.rate);
        Check(c.rate > a.rate + 3.0, line);
        std::snprintf(line, sizeof line, "and fewer breaks a minute: %.0f against %.0f", c.breaksPerMin, a.breaksPerMin);
        Check(c.breaksPerMin <= a.breaksPerMin, line);
    }

    std::printf("a regular rhythm is left alone: a scene that holds 54 a second with a rare doubled hold does not engage PACE24\n");
    {
        Scene s;
        s.feMs = FeFor(56.0);
        Model m(Pace24(), 5);
        m.Run(s, 40);
        Check(m.engages == 0, "56 a second: no engage");
    }
    std::printf("a CPU-held 40 a second engages PACE24 within 3 s and every picture is then held 2 vblanks\n");
    {
        Scene s;
        s.feMs = FeFor(40.0);
        Model m(Pace24(), 6);
        m.Run(s, 3);
        const bool on = m.cad.Engaged();
        m.Run(s, 2);
        m.Mark();
        m.Run(s, 20);
        const double all = static_cast<double>(m.hold[1] + m.hold[2] + m.hold[3] + m.hold[4]);
        char line[200];
        std::snprintf(line, sizeof line, "engaged after 3 s (%d), then %.0f %% of the pictures are held 2 vblanks, %.1f breaks a second", on ? 1 : 0, 100.0 * m.hold[2] / all, m.breaks / 20.0);
        Check(on && m.hold[2] / all >= 0.95, line);
    }
    std::printf("the display census (HoldMeter, SmoothCensus)\n");
    {
        port::pace::HoldMeter hm;
        uint32_t v = 100;
        for (int i = 0; i < 20; ++i) { hm.Shown(v); v += 2; }       // a steady 30: 19 holds of 2, no break
        auto a = hm.Read();
        Check(a.pictures == 19 && a.breaks == 0 && a.hold[2] == 19, "a steady 30 is 19 holds of 2 vblanks and no break");
        for (int i = 0; i < 10; ++i) { hm.Shown(v); v += (i & 1) ? 2 : 1; }  // 1-2-1-2: every hold after the first differs
        auto b = hm.Read();
        Check(b.breaks - a.breaks >= 9, "1-2-1-2 breaks on every picture");
        hm.Shown(v + 40);  // a stall: neither a hold nor a break
        auto c = hm.Read();
        Check(c.pictures == b.pictures && c.breaks == b.breaks, "a gap over 8 vblanks is no hold and no break");
        port::pace::SmoothCensus sc;
        sc.Due(1000000, 10000000);
        for (int i = 0; i < 100; ++i) sc.Picture(i % 10 == 0 ? 2 : 1, 1, false);
        sc.Picture(3, 1, false);
        sc.Picture(0, 1, false);
        const auto w = sc.Take(11000000, 3);
        char line[400];
        port::pace::SmoothCensus::Format(line, sizeof line, w, false, 4);
        std::printf("    %s\n", line);
        Check(w.pictures == 101 && w.late == 11 && w.breaks >= 20 && w.hold[3] == 1 && w.maxMs > 49.0 && w.changes == 3, "pictures, late, breaks, the 50 ms hold, longest and changes are counted");
        Check(std::strstr(line, "smooth: 10.0 s, 101 pictures") != nullptr && std::strstr(line, "state changes 3") != nullptr, "the line carries the window");
    }
    std::printf("off: PACE24 params change nothing when p24 is false\n");
    {
        Params a = Today(), b = Today();
        b.window = 30; b.breakFps = 7.0; b.dwellSec = 8.0;  // (ignored without p24)
        Model ma(a, 9), mb(b, 9);
        Scene s;
        s.feMs = FeFor(46.0);
        ma.Run(s, 40);
        mb.Run(s, 40);
        Check(ma.pictures == mb.pictures && ma.breaks == mb.breaks && ma.changes == mb.changes, "identical pictures, breaks and changes");
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
