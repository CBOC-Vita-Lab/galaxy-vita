// Host-side test of Performance mode's way back on the steady 30 (Config::cadenceUp, GXV_AUTOQ_CADENCE_UP) and of its edge step's longer
// trial (Config::edgeCheck, GXV_AUTOQ_EDGECHECK), in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp).
// tools/test_vgx_autoq_cadence.sh builds and runs it.
//
// The pipeline model is the one of vgx_autoq_60.cpp (60 Hz, a picture at the first retrace after its largest stage), with the pacing's
// steady 30 (FrameTimes::cadence30: two retraces a picture at least) and a GPU surcharge a rung can have beyond the pixel prediction.
// 1. Beach Bowl as the console showed it (r98): chased 60 down to 608x348 with the steps on, then the steady 30 with the GPU at 25 ms:
//    one size back up (664x376), not to 720x404 where the prediction does not fit 90 % of 33.3 ms; never above 720x404.
// 2. A CPU-limited scene on the steady 30 (the front end at 26 ms): climbs to 720x404, and takes its steps off when they fit.
// 3. A GPU at 18 ms at 608x348 with the other stages at 12 ms (60 reachable at a smaller size): no climb on the cadence.
// 4. A climb whose size turns out dearer than predicted: back within the trial, that size blocked, the block doubling on the next try.
// 5. With the switch off the old decisions: the same traces stay down; and with it on, every trace off the cadence (and target 30,
//    Quality's hold30) decides exactly as with it off.
// 6. The edge step's trial (GXV_AUTOQ_EDGECHECK): kept whatever its trial reads (6 %, 4.5 % or 1 % falls with +-4 % noise); without the
//    switch a 1 % or 4.5 % fall undoes it.
#include "../platform/vgx/src/vgx_autoq.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vgx::autoq;

extern "C" int64_t port_thread_cpu_us(void) { return 100; }

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Load {
    double turnMs = 7, backendMs = 4, fixedGpuMs = 3, pixGpuMs = 18; // pixGpuMs at 640x456 (Cost 1); one game turn's CPU
    double fxGain = 1.0, bgGain = 1.1;   // GPU time without the step / with it
    double frontMs = 12;                 // the front end's CPU a picture
    double edgeMs = 2.2;                 // the present's edge smoothing: GPU time a picture, whatever the size
    double extraMs[kRungs + 1] = {};     // GPU time a rung costs beyond the pixel prediction (index rung + 1)
    bool cadence = false;                // the pacing's steady 30
};
struct Event {
    double t;
    char kind; // 'R' rung, 'F'/'f', 'B'/'b', 'E'/'e' steps on/off
    int from, to;
    Why why;
    double blockSec; // 'R' CadenceBack: how long the rung left is blocked
};
constexpr double kRetraceUs = 16683.0;

struct Sim {
    Controller ctl;
    std::mt19937 rng;
    double noise;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPictures = 1;
    double t = 0;
    std::vector<Event> ev;
    unsigned stepsPrev = 0;
    unsigned pictures = 0, retraces = 0;
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n) {}
    void Frame(const Load& l) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double tms = l.turnMs * (1.0 + u(rng)), bms = l.backendMs * (1.0 + u(rng));
        const double gpu = (l.fixedGpuMs + l.pixGpuMs * Cost(ctl.Current()) + l.extraMs[ctl.Current() + 1]) * (1.0 + u(rng)) /
                           (ctl.FxStepOn() ? l.fxGain : 1.0) / (ctl.BgStepOn() ? l.bgGain : 1.0) + (ctl.EdgeStepOn() ? 0.0 : l.edgeMs);
        const double fms = l.frontMs * (1.0 + u(rng));
        const double work = std::max({tms, bms, gpu, fms});
        const unsigned n = static_cast<unsigned>(std::max(l.cadence ? 2.0 : 1.0, std::ceil(work * 1000.0 / kRetraceUs - 1e-9)));
        const uint64_t interval = static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(tms * 1000.0) * n;
        turns += n;
        backend += static_cast<uint32_t>(bms * 1000.0);
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        front += static_cast<uint32_t>(fms * 1000.0);
        ++frontPictures;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = true;
        f.cadence30 = l.cadence;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        ++pictures;
        retraces += n;
        const int before = ctl.Current();
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        if (changed || ctl.Current() != before) {
            const uint64_t until = ctl.BlockedUntilUs(before);
            ev.push_back({t, 'R', before, ctl.Current(), ctl.LastChange().why, until > now ? static_cast<double>(until - now) / 1e6 : 0.0});
        }
        if (ctl.Steps() != stepsPrev) {
            for (unsigned bit = 1; bit <= 4; bit <<= 1) {
                const bool was = (stepsPrev & bit) != 0, is = (ctl.Steps() & bit) != 0;
                if (was != is) ev.push_back({t, bit == 1 ? (is ? 'F' : 'f') : bit == 2 ? (is ? 'B' : 'b') : (is ? 'E' : 'e'), ctl.Current(), ctl.Current(),
                                             ctl.LastFxChange().why, 0.0});
            }
            stepsPrev = ctl.Steps();
        }
    }
    void Run(const Load& l, double seconds) {
        const double end = t + seconds;
        while (t < end) Frame(l);
    }
    void Mark() { pictures = retraces = 0; }
    double Rate() const { return retraces ? 60.0 * pictures / retraces : 0.0; }
    unsigned Count(Why why, double after = 0) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == 'R' && e.why == why && e.t >= after;
        return n;
    }
    unsigned Ups(double after) const { // changes of size upwards after `after`
        unsigned n = 0;
        for (const Event& e : ev) n += e.kind == 'R' && e.t >= after && e.to > e.from;
        return n;
    }
    int Highest(double after) const {
        int h = -9;
        for (const Event& e : ev)
            if (e.kind == 'R' && e.t >= after) h = std::max(h, e.to);
        return h;
    }
    int Index(char kind, double after = -1) const {
        for (size_t i = 0; i < ev.size(); ++i)
            if (ev[i].t >= after && ev[i].kind == kind) return static_cast<int>(i);
        return -1;
    }
    void Print(const char* title) const {
        std::printf("    %s:\n", title);
        for (const Event& e : ev) {
            if (e.kind == 'R') std::printf("      t=%7.1f s  rung %2d -> %2d (%s)%s\n", e.t, e.from, e.to, WhyText(e.why), e.blockSec > 0 ? " [blocked]" : "");
            else std::printf("      t=%7.1f s  %s %s at rung %d (%s)\n", e.t, e.kind == 'F' || e.kind == 'f' ? "half-size effects and blur" :
                             e.kind == 'E' || e.kind == 'e' ? "edge smoothing given up     " : "cheaper background          ",
                             e.kind == 'F' || e.kind == 'B' || e.kind == 'E' ? "ON " : "off", e.from, WhyText(e.why));
        }
    }
};
bool SameEvents(const Sim& a, const Sim& b) {
    if (a.ev.size() != b.ev.size() || a.ctl.Current() != b.ctl.Current() || a.ctl.Steps() != b.ctl.Steps()) return false;
    for (size_t i = 0; i < a.ev.size(); ++i)
        if (a.ev[i].t != b.ev[i].t || a.ev[i].kind != b.ev[i].kind || a.ev[i].from != b.ev[i].from || a.ev[i].to != b.ev[i].to || a.ev[i].why != b.ev[i].why)
            return false;
    return true;
}

// Performance mode as the Vita runs it (entry.cpp + settings_launch.cpp; vgx_autoq_60.cpp Console()): target 60, sizes 608x348 (rung 0 of
// the wide ladder) to 960x544, the present's edge smoothing and the cheaper background as steps, recovery after 20 s of room, play-time
// quality, severe overloads, anisotropic filtering from rung 2; and the two switches of the back end (default on there).
Config Console(bool cadenceUp = true, bool edgeCheck = true) {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.bgStep = c.edgeStep = true;
    c.overload = c.overloadFx = true;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    c.aniso = true;
    c.anisoRung = 2;
    c.cadenceUp = cadenceUp;
    c.edgeCheck = edgeCheck;
    return c;
}
// GPU (fixed 3 + pix x Cost) with the cheaper background (1.1) and without the edge smoothing: `ms` at rung `rung`
double PixFor(double ms, int rung, double bgGain = 1.1) { return (ms * bgGain - 3.0) / Cost(rung); }

// Chase 60 first (no steady 30): a GPU of about 48 ms at 720x404 (42 with the steps) steps down to 608x348 with the steps on, as the console
// did (this model shows pictures at whole retraces, so the series must gain pictures to be kept: 36 ms at 664x376, 31 ms at 608x348)
void Chase(Sim& s) {
    Load heavy;
    heavy.pixGpuMs = 43;
    s.Run(heavy, 90);
}
} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);  // (the console's wide ladder: 608x348, 664x376, 720x404 ...)
    setenv("GXV_AUTOQ_TARGET", "60", 1);
    std::printf("the configuration\n");
    {
        Config c = Console();
        c.targetFps = 30;
        c.budgetMs = 1000.0 / 30.0;
        Controller q(c);
        Check(!q.Cfg().cadenceUp && !q.Cfg().edgeCheck, "target 30: the switches are off whatever was given (Quality and Locked 30 unchanged)");
        Controller p(Console());
        Check(p.Cfg().cadenceUp && std::abs(p.Cfg().cadenceReachFps - 45.0) < 1e-9 && std::abs(p.Cfg().cadenceUpFrac - 0.9) < 1e-9,
              "target 60: on, reach 45 a second (frame_pace.h kSteady30AboveFps), margin 90 % of 33.3 ms");
        Config d;
        Check(!d.cadenceUp && !d.edgeCheck, "Config's own defaults: off (the back end turns them on)");
    }

    std::printf("Beach Bowl on the steady 30 (GPU 25 ms at 608x348, other stages 12 ms)\n");
    double beachRate = 0;
    for (int on = 1; on >= 0; --on) {
        Sim s(Console(on != 0), 31, 0.03);
        Chase(s);
        const int low = s.ctl.Current();
        const unsigned steps = s.ctl.Steps();
        Load beach;
        beach.cadence = true;
        beach.pixGpuMs = PixFor(25.0, 0);
        const double from = s.t;
        s.Run(beach, 240);
        if (verbose) s.Print(on ? "chase 60, then the steady 30 (CADENCE_UP=1)" : "chase 60, then the steady 30 (CADENCE_UP=0)");
        s.Mark();
        s.Run(beach, 20);
        char line[240];
        if (on) {
            Check(low == 0 && (steps & 6u) == 6u, "setup: chasing 60 ends at 608x348 with edge smoothing and the cheaper background given up");
            const int hi = s.Highest(from);
            std::snprintf(line, sizeof line, "climbs 608x348 -> 664x376 on the cadence (rung %d, %u climbs, highest %d), not to 720x404 (predicted %.1f ms > 30)",
                          s.ctl.Current(), s.Count(Why::CadenceUp, from), hi, 25.0 * 1.18 * Cost(2) / Cost(1));
            Check(s.ctl.Current() == 1 && s.Count(Why::CadenceUp, from) == 1 && hi <= kToday && s.Count(Why::CadenceBack) == 0, line);
            std::snprintf(line, sizeof line, "... the steady 30 holds there (%.1f a second), steps still on below 720x404", s.Rate());
            Check(s.Rate() >= 29.5 && (s.ctl.Steps() & 6u) == 6u, line);
            beachRate = s.Rate();
        } else {
            std::snprintf(line, sizeof line, "GXV_AUTOQ_CADENCE_UP=0: stays at 608x348 with the steps (rung %d, %u climbs; the r98 console run)", s.ctl.Current(), s.Ups(from));
            Check(s.ctl.Current() == 0 && s.Ups(from) == 0 && (s.ctl.Steps() & 6u) == 6u && s.Rate() >= 29.5 && beachRate >= 29.5, line);
        }
    }

    std::printf("a CPU-limited scene on the steady 30 (front end 26 ms)\n");
    {
        // GPU 21 ms at 608x348: 664x376 predicted 24.8 ms, over the old free climb (90 % of the front end's 26 ms = 23.4), within 30
        for (int on = 1; on >= 0; --on) {
            Sim s(Console(on != 0), 32, 0.03);
            Chase(s);
            Load cpu;
            cpu.cadence = true;
            cpu.frontMs = 26;
            cpu.pixGpuMs = PixFor(21.0, 0);
            const double from = s.t;
            s.Run(cpu, 240);
            if (verbose) s.Print(on ? "front end 26 ms, GPU 21 ms at 608x348 (CADENCE_UP=1)" : "the same (CADENCE_UP=0)");
            char line[200];
            std::snprintf(line, sizeof line, "%s: rung %d, highest %d, %u climbs on the cadence, %u back", on ? "on" : "off (old decisions: no climb)", s.ctl.Current(),
                          s.Highest(from), s.Count(Why::CadenceUp), s.Count(Why::CadenceBack));
            if (on) Check(s.ctl.Current() == kToday && s.Highest(from) <= kToday && s.Count(Why::CadenceBack) == 0, line);
            else Check(s.ctl.Current() == 0 && s.Ups(from) == 0, line);
        }
        // GPU 14 ms at 608x348: 720x404 and then the steps off fit (bg 18.9 ms, edge smoothing 20.3 ms predicted)
        for (int on = 1; on >= 0; --on) {
            Sim s(Console(on != 0), 33, 0.03);
            Chase(s);
            Load cpu;
            cpu.cadence = true;
            cpu.frontMs = 26;
            cpu.pixGpuMs = PixFor(14.0, 0);
            const double from = s.t;
            s.Run(cpu, 240);
            if (verbose) s.Print(on ? "front end 26 ms, GPU 14 ms at 608x348 (CADENCE_UP=1)" : "the same (CADENCE_UP=0)");
            const int bgOff = s.Index('b', from), edgeOff = s.Index('e', from);
            char line[220];
            if (on) {
                std::snprintf(line, sizeof line, "light GPU: up to 720x404 (rung %d, highest %d), then the cheaper background off (event %d), then edge smoothing (event %d)",
                              s.ctl.Current(), s.Highest(from), bgOff, edgeOff);
                Check(s.ctl.Current() == kToday && s.Highest(from) <= kToday && bgOff >= 0 && edgeOff > bgOff && s.ctl.Steps() == 0, line);
            } else {
                // (the old free climb already brings this one to 720x404, but nothing takes a step off against 16.7 ms)
                std::snprintf(line, sizeof line, "off: the old free climb (rung %d), the steps stay on (steps %u)", s.ctl.Current(), s.ctl.Steps());
                Check(s.ctl.Current() <= kToday && s.ctl.Steps() != 0 && bgOff < 0 && edgeOff < 0, line);
            }
        }
    }

    std::printf("60 reachable at a smaller size: no climb on the cadence\n");
    {
        Sim s(Console(), 34, 0.03);
        Chase(s);
        Load near;           // GPU 18 ms at 608x348, other stages 12 ms: under 22.2 ms at the floor rung, the steady 30 may still be released
        near.cadence = true;
        near.pixGpuMs = PixFor(18.0, 0);
        const double from = s.t;
        const unsigned steps = s.ctl.Steps();
        s.Run(near, 240);
        if (verbose) s.Print("GPU 18 ms at 608x348 on the steady 30");
        char line[200];
        std::snprintf(line, sizeof line, "GPU 18 ms at 608x348: rung %d, %u climbs, steps %u -> %u", s.ctl.Current(), s.Ups(from), steps, s.ctl.Steps());
        Check(s.ctl.Current() == 0 && s.Ups(from) == 0 && s.ctl.Steps() == steps, line);
    }

    std::printf("a climb dearer than predicted goes back and is blocked\n");
    {
        Sim s(Console(), 35, 0.02);
        Chase(s);
        Load cliff;          // 664x376 predicted 29.5 ms, but 36 ms on the console (a cliff the pixels do not predict)
        cliff.cadence = true;
        cliff.pixGpuMs = PixFor(25.0, 0);
        cliff.extraMs[1 + 1] = 7.0;
        const double from = s.t;
        s.Run(cliff, 300);
        if (verbose) s.Print("664x376 costs 7 ms more than predicted");
        std::vector<const Event*> ups, backs;
        for (const Event& e : s.ev)
            if (e.kind == 'R' && e.t >= from) {
                if (e.why == Why::CadenceUp) ups.push_back(&e);
                if (e.why == Why::CadenceBack) backs.push_back(&e);
            }
        char line[240];
        const bool paired = !ups.empty() && backs.size() >= ups.size() - 1 && backs.size() <= ups.size();
        bool quick = paired;
        for (size_t i = 0; i < backs.size() && i < ups.size(); ++i) quick = quick && backs[i]->t - ups[i]->t <= 8.5 && backs[i]->to == 0 && backs[i]->from == 1;
        std::snprintf(line, sizeof line, "%zu climbs to 664x376, %zu back within the trial (first after %.1f s)", ups.size(), backs.size(),
                      !ups.empty() && !backs.empty() ? backs[0]->t - ups[0]->t : -1.0);
        Check(ups.size() >= 2 && quick, line);
        std::snprintf(line, sizeof line, "... the size blocked %.0f s, then %.0f s (doubling); the next try waits for it (%.0f s)", backs.size() > 0 ? backs[0]->blockSec : 0.0,
                      backs.size() > 1 ? backs[1]->blockSec : 0.0, ups.size() > 1 ? ups[1]->t - backs[0]->t : 0.0);
        Check(backs.size() >= 2 && std::abs(backs[0]->blockSec - 60.0) < 1.0 && std::abs(backs[1]->blockSec - 120.0) < 1.0 && ups[1]->t - backs[0]->t >= 60.0, line);
        Check(s.Highest(from) <= 1, "... and nothing above it is tried");
    }

    std::printf("the switch off decides as before; off the cadence nothing changes\n");
    {
        struct Case { const char* name; Load l; Config c; double secs; };
        std::vector<Case> cases;
        Load gpu;            // GPU-limited at 16.7 ms
        gpu.pixGpuMs = 18; gpu.edgeMs = 2.2; gpu.frontMs = 10;
        Load heavy;          // GPU 40 ms
        heavy.pixGpuMs = 37;
        Load light;          // room at 16.7 ms: sizes above 720x404 and anisotropic filtering
        light.pixGpuMs = 5; light.turnMs = 5; light.frontMs = 8; light.edgeMs = 1.0;
        Load fe;             // the front end 26 ms without the steady 30
        fe.frontMs = 26; fe.pixGpuMs = 14;
        Load lit;            // the lit Observatory on the steady 30 at 720x404 (GPU 22 ms, front end 24): unchanged too (60 reachable)
        lit.cadence = true; lit.frontMs = 24; lit.turnMs = 14; lit.pixGpuMs = 19; lit.edgeMs = 0;
        Config q = Console();
        q.targetFps = 30;
        q.budgetMs = 1000.0 / 30.0;
        Config hold = q;
        hold.hold30 = true;
        hold.minRung = 0;
        hold.startRung = kTopSingle;
        Load q30 = heavy;    // Quality: GPU 40 ms at 720x404
        cases.push_back({"target 60, GPU-limited", gpu, Console(), 150});
        cases.push_back({"target 60, GPU 40 ms", heavy, Console(), 150});
        cases.push_back({"target 60, room at 16.7 ms", light, Console(), 200});
        cases.push_back({"target 60, front end 26 ms", fe, Console(), 150});
        cases.push_back({"target 60, the lit Observatory on the steady 30", lit, Console(), 150});
        cases.push_back({"target 30", q30, q, 150});
        cases.push_back({"target 30 with hold30 (Quality)", q30, hold, 150});
        for (const Case& k : cases) {
            Config off = k.c;
            off.cadenceUp = false;
            Sim a(k.c, 40, 0.04), b(off, 40, 0.04);
            a.Run(k.l, k.secs);
            b.Run(k.l, k.secs);
            Load after = light;  // then room
            a.Run(after, 100);
            b.Run(after, 100);
            char line[200];
            std::snprintf(line, sizeof line, "%s, then room: identical decisions with GXV_AUTOQ_CADENCE_UP on and off (%zu events)", k.name, a.ev.size());
            Check(SameEvents(a, b), line);
        }
    }

    std::printf("the edge step's trial (GXV_AUTOQ_EDGECHECK)\n");
    {
        // GPU-limited at 720x404, edge smoothing a share of the GPU time; per-picture noise +-4 %
        auto run = [](double fall, bool edgeCheck, uint32_t seed, bool verbose) {
            Load l;
            l.frontMs = 10;
            const double total = 20.0;
            l.edgeMs = total * fall;
            l.pixGpuMs = (total - l.edgeMs - 3.0) / Cost(kToday);
            l.bgGain = 1.15;
            Sim s(Console(true, edgeCheck), seed, 0.04);
            s.Run(l, 30);
            if (verbose) s.Print(edgeCheck ? "edge step, EDGECHECK=1" : "edge step, EDGECHECK=0");
            const int on = s.Index('E'), off = s.Index('e');
            if (on < 0) return -1;                                          // (no trial)
            return off >= 0 && s.ev[off].why == Why::FxNoGain ? 0 : 1;      // 0 undone for no gain, 1 kept
        };
        int kept6 = 0, undone1 = 0, kept45 = 0, kept45old = 0, kept6old = 0, trials = 0;
        const int seeds = 10;
        for (int i = 0; i < seeds; ++i) {
            const int a = run(0.06, true, 50 + i, verbose && i == 0), b = run(0.01, true, 50 + i, false);
            trials += a >= 0 && b >= 0;
            kept6 += a == 1;
            undone1 += b == 0;
            kept45 += run(0.045, true, 50 + i, false) == 1;
            kept45old += run(0.045, false, 50 + i, false) == 1;
            kept6old += run(0.06, false, 50 + i, false) == 1;
        }
        char line[220];
        std::snprintf(line, sizeof line, "a 6 %% fall with +-4 %% noise keeps the step (%d of %d seeds; %d without the switch)", kept6, seeds, kept6old);
        Check(trials == seeds && kept6 == seeds, line);
        std::snprintf(line, sizeof line, "a 1 %% fall: kept with the switch (%d of %d undone; its saving is the present's, hidden by the scene)", undone1, seeds);
        Check(undone1 == 0, line);
        std::snprintf(line, sizeof line, "a 4.5 %% fall: kept %d of %d with the switch, %d without (betterFrac 5 %%)", kept45, seeds, kept45old);
        Check(kept45 == seeds && kept45old == 0, line);
    }

    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
