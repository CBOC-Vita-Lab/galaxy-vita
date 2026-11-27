// Host-side test of the shed order (Config::shed, GXV_AUTOQ_SHED) in the automatic quality controller (platform/vgx/src/vgx_autoq.cpp).
// tools/test_vgx_autoq_shed.sh builds and runs it (ASan + UBSan).
//
// The GPU is MODELLED from the three console scenes of 11 Nov (c52 Melty Molten, c52 Deep Dark, c49 Honeyhive at 960x544 without the edge pass):
//   3D = EFB 3D x (fixed share + pixel share x pixels / pixels at 960x544) + texture/pass scenes + display (edge pass 4.58 ms, plain 1.26, direct 0.26 ms)
//   busy = max(TA, 3D) + k x min(TA, 3D), k measured per scene ((busy - max) / min).
// The controller's own belief (shedPixShare 0.5, edge 3.0 ms, direct 1.0 ms) differs from this model on purpose: the test is about the decisions
// settling, not about a perfect prediction. These are traces of a MODEL, not console runs.
#include "../platform/vgx/src/vgx_autoq.h"
#include "../platform/vgx/src/vgx_shed.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace vgx::autoq;

extern "C" int64_t port_thread_cpu_us(void) { return 100; }

namespace {
int g_failures = 0;
bool g_verbose = false;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kRetraceUs = 16683.0;

struct Scene {
    const char* name;
    double efb, tex, ta, k;       // EFB 3D at 960x544 (ms), texture/pass 3D, TA, overlap
    double turn, backend, front;  // CPU stages (ms)
    double fixedShare;            // the EFB's share that does not fall with the pixels
    bool noEdge = false;
    double edgeMs = 3.3, directMs = 1.0, cutMs = 0.9; // real savings in the model (edge pass 4.58 -> 1.26; direct to ~0.26)
};
const Scene kMelty{"Melty Molten", 13.35, 1.38, 9.40, 0.217, 9.0, 5.9, 13.2, 0.25};
const Scene kDeep{"Deep Dark", 17.34, 1.35, 12.91, 0.333, 11.6, 7.3, 13.7, 0.25};
const Scene kHoney{"Honeyhive (960, no edge pass)", 13.03, 1.47, 12.60, 0.152, 10.0, 6.0, 15.5, 0.25, true};

struct Event { double t; int from, to; bool edge, cut; Why why; };

struct Sim {
    Controller ctl;
    std::mt19937 rng;
    double noise;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1, front = 1, frontPictures = 1;
    double t = 0;
    unsigned pictures = 0, retraces = 0, hitches = 0;
    double lastIv = 0;
    std::vector<Event> ev;
    int prevRung;
    bool prevEdge = false, prevCut = false;
    double cutEvery = 0, nextCut = 0;
    std::vector<double> ivs; // every picture's interval (ms)
    Sim(const Config& c, uint32_t seed, double n) : ctl(c), rng(seed), noise(n), prevRung(ctl.Current()) {}
    double Gpu(const Scene& s, double scale) {
        const Rung& r = RungAt(ctl.Current());
        const bool native = r.w == 960 && r.h == 544;
        const double px = static_cast<double>(r.w) * r.h / (960.0 * 544.0);
        const bool edgeOff = ctl.EdgeStepOn() || s.noEdge;
        double disp = edgeOff ? 1.26 : 4.58;
        if (native && edgeOff && ctl.Cfg().shedDirectMs > 0) disp = 0.26;
        double efb = s.efb * (s.fixedShare + (1 - s.fixedShare) * px);
        double d3 = (efb + s.tex + disp - (ctl.ShedCutOn() ? s.cutMs : 0.0)) * scale;
        const double ta = s.ta * scale;
        const double busy = std::max(ta, d3) + s.k * std::min(ta, d3);
        return busy;
    }
    void Frame(const Scene& s, double scale = 1.0, bool quiet = false) {
        std::uniform_real_distribution<double> u(-noise, noise);
        const double gpu = Gpu(s, scale) * (1.0 + u(rng));
        const double tms = s.turn * (1.0 + u(rng)), bms = s.backend * (1.0 + u(rng)), fms = s.front * (1.0 + u(rng));
        const double work = std::max({tms, bms, gpu, fms});
        const unsigned n = static_cast<unsigned>(std::max(1.0, std::ceil(work * 1000.0 / kRetraceUs - 1e-9)));
        const uint64_t interval = static_cast<uint64_t>(n * kRetraceUs);
        game += static_cast<uint32_t>(tms * 1000.0) * n;
        turns += n;
        backend += static_cast<uint32_t>(bms * 1000.0);
        front += static_cast<uint32_t>(fms * 1000.0);
        ++frontPictures;
        FrameTimes f;
        f.startUs = now;
        now += interval;
        f.presentUs = now;
        f.backendRunUs = backend;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.frontRunUs = front;
        f.frontPictures = frontPictures;
        f.frontKnown = true;
        f.quiet = quiet;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>(gpu * 1000.0);
        if (cutEvery > 0 && t >= nextCut) { f.cut = true; nextCut = t + cutEvery; }
        ++pictures;
        retraces += n;
        ivs.push_back(interval / 1000.0);
        const bool changed = ctl.Add(f);
        t += static_cast<double>(interval) / 1e6;
        const bool edge = ctl.EdgeStepOn(), cut = ctl.ShedCutOn();
        if (changed || ctl.Current() != prevRung || edge != prevEdge || cut != prevCut) {
            ev.push_back({t, prevRung, ctl.Current(), edge, cut, ctl.LastChange().why});
            prevRung = ctl.Current();
            prevEdge = edge;
            prevCut = cut;
        }
    }
    void Run(const Scene& s, double seconds, double scale = 1.0) {
        const double end = t + seconds;
        while (t < end) Frame(s, scale);
    }
    double Rate(unsigned fromPicture) const {
        unsigned n = 0;
        double sum = 0;
        for (size_t i = fromPicture; i < ivs.size(); ++i) { sum += ivs[i]; ++n; }
        return sum > 0 ? 1000.0 * n / sum : 0.0;
    }
    unsigned ChangesAfter(double after) const {
        unsigned n = 0;
        for (const Event& e : ev) n += e.t >= after;
        return n;
    }
    void Print(const char* title) const {
        std::printf("    %s (rung %d, edge pass %s, captures %s at the end):\n", title, ctl.Current(), ctl.EdgeStepOn() ? "off" : "on", ctl.ShedCutOn() ? "half" : "full");
        for (const Event& e : ev)
            std::printf("      t=%7.2f s  rung %d -> %d, edge pass %s, captures %s (%s)\n", e.t, e.from, e.to, e.edge ? "off" : "on", e.cut ? "half" : "full", WhyText(e.why));
    }
};

Config Console(bool shed, bool cuts, bool direct, bool edge = true, bool qg = false) {
    Config c;
    c.targetFps = 60;
    c.budgetMs = 1000.0 / 60.0;
    c.minRung = 0;
    c.maxRung = kTopSingle;
    c.startRung = kTopSingle;
    c.recover = true;
    c.recoverHoldSec = 20.0;
    c.overload = c.overloadFx = true;
    c.freeFrac = 0.85;
    c.wipe = c.lateRoom = true;
    c.cadenceUp = true;
    c.bgStep = true;
    c.edgeStep = edge;
    c.edgeCheck = edge;
    c.aniso = true;
    c.anisoRung = 2;
    c.fast60 = true;
    c.qg = qg;
    c.seam = true;
    c.shed = shed;
    c.shedCuts = cuts;
    c.shedDirectMs = direct ? 1.0 : 0.0;
    return c;
}
std::string Lvl(const Controller& c) {
    char b[160];
    std::snprintf(b, sizeof b, "rung %d (%dx%d), edge pass %s, captures %s", c.Current(), RungAt(c.Current()).w, RungAt(c.Current()).h, c.EdgeStepOn() ? "off" : "on", c.ShedCutOn() ? "half rate" : "full rate");
    return b;
}
} // namespace

int main(int argc, char** argv) {
    g_verbose = argc > 1 && std::string(argv[1]) == "verbose";
    setenv("GXV_WIDE_RUNGS", "1", 1);
    char line[300];

    std::printf("the switch: off in Config's own defaults and forced off outside target 60\n");
    {
        Config d;
        Check(!d.shed, "Config's own defaults: off");
        Config c = Console(true, true, true);
        c.targetFps = 30;
        c.budgetMs = 1000.0 / 30.0;
        Controller q(c);
        Check(!q.ShedOn() && q.ShedLevels() == 0, "target 30 (Quality, Locked 30): forced off, no table");
        Controller p(Console(true, true, true));
        Check(p.ShedOn() && p.ShedLevels() == 3 + 4, "target 60: the table holds full, edge off, captures, then rungs 5..2 (7 levels)");
        Check(vgx::shed::g_cutGate.load() == 0, "the half-rate captures are armed, not engaged");
        vgx::shed::g_cutGate.store(1);
        Controller n(Console(true, false, false, false));
        Check(n.ShedLevels() == 1 + 4, "no edge step, no captures: full + rungs 5..2");
    }

    std::printf("what the controller does TODAY (shed off) in Melty Molten and Deep Dark at the 60 target, steady 30 off\n");
    int todayMelty = -9, todayDeep = -9;
    for (const Scene* sc : {&kMelty, &kDeep}) {
        Sim s(Console(false, false, false), 5, 0.01);
        s.Run(*sc, 40);
        unsigned from = static_cast<unsigned>(s.ivs.size());
        s.Run(*sc, 20);
        std::snprintf(line, sizeof line, "%s today: after 40 s %s; %.1f pictures a second over the next 20 s; %zu changes", sc->name, Lvl(s.ctl).c_str(), s.Rate(from), s.ev.size());
        std::printf("    %s\n", line);
        if (g_verbose) s.Print("today");
        (sc == &kMelty ? todayMelty : todayDeep) = s.ctl.Current();
    }
    Check(todayMelty >= 0, "(informational) the rung today's rules reach in Melty is printed above");
    (void)todayDeep;

    std::printf("the shed order in Melty Molten: edge pass (and direct display) first, then half-rate captures, rungs last; settles, no oscillation\n");
    for (int cuts = 0; cuts <= 1; ++cuts) {
        Sim s(Console(true, cuts != 0, true), 5, 0.01);
        s.Run(kMelty, 20);
        const double t20 = s.t;
        const unsigned p20 = static_cast<unsigned>(s.ivs.size());
        s.Run(kMelty, 40);
        if (g_verbose || cuts) s.Print(cuts ? "Melty, shed with captures" : "Melty, shed");
        std::snprintf(line, sizeof line, "captures %s: state after 60 s: %s; %.1f pictures a second after 20 s", cuts ? "level on" : "level off", Lvl(s.ctl).c_str(), s.Rate(p20));
        std::printf("    %s\n", line);
        Check(!s.ev.empty() && s.ev[0].edge, "the FIRST change already has the edge pass off (it is never a smaller picture with the edge pass still on)");
        { bool ok = true; for (const Event& e : s.ev) if (e.to < kTopSingle && !e.edge) ok = false; Check(ok, "no change below 960x544 ever has the edge pass on"); }
        Check(s.ChangesAfter(t20 + 10) == 0, "settled: no change after the first 30 s (no oscillation)");
        Check(s.Rate(p20) >= 54.0, "pictures come at 54+ a second in the end");
        Check(s.ctl.Current() >= todayMelty, "the size is never below what today's rules end at (720x404)");
        if (cuts) Check(s.ctl.Current() >= 5, "with the half-rate captures level the size stays at 896x512 or above");
        Check(s.ctl.ShedHitches() == 0, "no hitch in the transition log");
    }

    std::printf("Deep Dark: 60 out of reach (the table's last level predicted over 18.2 ms): nothing is given up\n");
    {
        Sim s(Console(true, true, true), 5, 0.01);
        s.Run(kDeep, 60);
        if (g_verbose) s.Print("Deep Dark, shed");
        std::snprintf(line, sizeof line, "after 60 s: %s, %zu changes", Lvl(s.ctl).c_str(), s.ev.size());
        std::printf("    %s\n", line);
        Check(s.ev.empty() && s.ctl.Current() == kTopSingle && !s.ctl.EdgeStepOn(), "960x544 with the edge pass on, no change at all (the steady 30 is the way)");
    }

    std::printf("Honeyhive at 960x544, no edge pass (GXV_EDGE_AA_NATIVE=0): direct display is already free; a rung step is passed over when it saves too little\n");
    {
        Config c = Console(true, false, true, false);
        c.shedEdgeAtNative = false;
        Sim s(c, 5, 0.01);
        s.Run(kHoney, 40);
        if (g_verbose) s.Print("Honeyhive, shed");
        std::printf("    after 40 s: %s, %zu changes\n", Lvl(s.ctl).c_str(), s.ev.size());
        Check(s.ev.size() <= 2, "at most two changes (it is CPU-limited there: the front end 15.5 ms)");
    }

    std::printf("the way back: Melty heavy for 40 s, then the load falls to 60 %% for 60 s; ups wait for a camera cut (one every 6 s) and go level by level\n");
    {
        Config c = Console(true, true, true);
        Sim s(c, 7, 0.01);
        s.cutEvery = 6.0;
        s.Run(kMelty, 40);
        const int low = s.ctl.Current();
        const double tDrop = s.t;
        s.Run(kMelty, 90, 0.6);
        if (g_verbose) s.Print("Melty heavy then light");
        std::snprintf(line, sizeof line, "heavy: rung %d; after the load fell: %s", low, Lvl(s.ctl).c_str());
        std::printf("    %s\n", line);
        Check(s.ctl.Current() == kTopSingle && !s.ctl.EdgeStepOn(), "back at 960x544 (the edge pass last)");
        // after the drop every up-step comes at a cut
        bool order = true;
        for (const Event& e : s.ev)
            if (e.t > tDrop && e.edge == false && e.to < kTopSingle) order = false; // the edge pass must not return below the top rung
        Check(order, "the edge pass is back only at the top size (size first, edge pass last)");
        Check(s.ChangesAfter(tDrop + 80) <= 1, "no oscillation after the recovery");
    }

    std::printf("a load that lies: the prediction is too optimistic (pixel share 90 %% believed, 40 %% real): the level is judged and undone, not kept\n");
    {
        Config c = Console(true, false, true);
        c.shedPixShare = 0.9;
        Scene lie = kMelty;
        lie.fixedShare = 0.80;
        Sim s(c, 3, 0.01);
        s.Run(lie, 60);
        if (g_verbose) s.Print("lying model");
        std::printf("    after 60 s: %s, %zu changes\n", Lvl(s.ctl).c_str(), s.ev.size());
        Check(s.ev.size() <= 8, "a bounded number of changes (a failed level is blocked, doubling)");
    }

    std::printf("the stress cycle (GXV_AUTOQ_SHED_CYCLE=4): every level in turn, whatever the load, 400 s; transition log with the pictures around each switch\n");
    {
        Config c = Console(true, true, true);
        c.shedCycleSec = 4.0;
        Sim s(c, 9, 0.01);
        s.Run(kHoney, 400, 0.5);
        const unsigned tx = s.ctl.ShedTransitions();
        std::printf("    %zu changes, %u transitions logged, %u hitches\n", s.ev.size(), tx, s.ctl.ShedHitches());
        Check(s.ev.size() >= 90, "about 100 transitions in 400 s");
        Check(tx + 1 >= s.ev.size() - 1, "every change has its transition line (the picture before, at and after)");
        Check(s.ctl.ShedHitches() == 0, "no hitch in the model's pictures");
        bool seen[16] = {};
        for (const Event& e : s.ev) {
            const int idx = e.to == kTopSingle ? (e.cut ? 2 : e.edge ? 1 : 0) : 2 + (kTopSingle - e.to);
            if (idx >= 0 && idx < 16) seen[idx] = true;
        }
        bool all = true;
        for (int i = 0; i < 7; ++i) all = all && seen[i];
        Check(all, "all 7 levels of the table were visited");
        double minGap = 1e9;
        for (size_t i = 1; i < s.ev.size(); ++i) minGap = std::min(minGap, s.ev[i].t - s.ev[i - 1].t);
        std::snprintf(line, sizeof line, "the shortest time between two changes: %.2f s (>= the dwell 3 s, longer than the back end's 30-picture preparation deadline of 0.5 s)", minGap);
        Check(minGap >= 3.0, line);
    }

    std::printf("quiet moments, a movie or a closed fade never carry a shed decision; Quality (target 30) and shed off decide as before\n");
    {
        Sim a(Console(false, false, false), 5, 0.01), b(Console(true, true, true), 5, 0.01);
        // a calm 60 trace: both identical (nothing to shed)
        a.Run(kMelty, 30, 0.45);
        b.Run(kMelty, 30, 0.45);
        Check(a.ev.size() == b.ev.size() && a.ctl.Current() == b.ctl.Current(), "a calm trace: shed on decides as shed off");
        Sim q(Console(true, true, true), 5, 0.01);
        for (int i = 0; i < 900; ++i) q.Frame(kMelty, 1.0, true); // all quiet
        Check(q.ev.empty(), "900 quiet pictures: no change");
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
