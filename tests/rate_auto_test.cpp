// rate30 (13 Nov 2026): host model and test of GXV_RATE_AUTO (platform/include/port/rate_auto.h).
//   nice g++ -std=c++17 -Iplatform/include tests/rate_auto_test.cpp -o /tmp/rate_auto_test && /tmp/rate_auto_test
// The model is mine (not a console): a game thread that runs 60 turns a second and draws a picture on the turns the pacer offers and the pipeline can take, a pipeline of three
// lanes (front end, render thread, GPU) in series with queues, a display that shows a picture on the first vblank after it is done and after the previous one, and the REAL
// governors (port/fegov.h Governor, two of them: front end and game thread) whose cuts take 3 s to walk in. The even 30 is the real port::pace::SteadyCadence forced as the glue forces it.
// Three policies run the same traces: legacy (no rate control, the pacer's own 60 with misses), smooth60 (port/smooth60.h: one way, 60 -> an even 30), rate_auto (this controller).
// Every number printed is a MODEL result; the console decides.
#include "../platform/include/port/frame_pace.h"
#include "../platform/include/port/smooth60.h"
#include "../platform/include/port/fegov.h"
#include "../platform/include/port/rate_auto.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {
int g_fail = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  [FAIL] %s\n", what); }
}
#define CHECKF(c, ...) do { char _b[260]; std::snprintf(_b, sizeof _b, __VA_ARGS__); Check((c), _b); } while (0)

constexpr double kT = 16683.3;   // one refresh in us
constexpr int kMaxLevel = 4;

struct Load { double logic, draw, fe, render, gpu; };   // ms at cut level 0 (the game thread's drawn turn = logic + draw)
struct Scenario {
    std::string name;
    double seconds = 120;
    std::function<Load(double)> load;
    double gainScale = 1.0;                // the true gain of a level against the governors' assumed 0.8 / level (x0.5, x2: mis-predicted)
    std::function<double(double)> hitchMs; // extra ms on the turn that starts at t (seconds), 0 none
    double tOverload = -1, tRelief = -1;   // for the two time columns
    double breakEvery = 4.0;               // a natural break (camera cut) every this many seconds, one turn long; 0 none
};
Load Light() { return {5.0, 6.0, 9.0, 8.0, 10.0}; }
Load Heavy(double fe) { return {6.0, 10.0, fe, 14.0, 17.0}; }   // game 16 ms drawn: over the limit as well

enum Policy { kLegacy = 0, kSmooth = 1, kRate = 2, kForced = 3 };   // kForced: the seam test: a light load, the rhythm toggled by hand every 7 s
const char* kPolName[] = {"legacy", "smooth60", "rate_auto", "forced"};

struct Result {
    double sec60 = 0, sec30 = 0, fps = 0, speedPct = 0, switchesPerMin = 0, longestHold = 0, tDown = -1, tUp = -1;
    int downs = 0, ups = 0, hold3 = 0, failedUps = 0, tries = 0;
    int pics = 0;
    double meanLat = 0;
    // seams
    std::vector<std::string> seams;
};

class Sim {
public:
    Sim(const Scenario& sc, int pol, const port::rate::Params& rp = port::rate::Params()) : sc_(sc), pol_(pol), rp_(rp) {}
    Result Run(bool wantSeams = false);

private:
    struct Pic { int turn; double displayV; double finish; double drawStart; double renderEnd; };
    double Rnd() { s_ ^= s_ << 13; s_ ^= s_ >> 7; s_ ^= s_ << 17; return (static_cast<double>(s_ % 2000001) / 1000000.0 - 1.0); }
    void Windows(double t);
    double CutLevel(double t) const {
        if (t >= walkEnd_) return walkTo_;
        if (t <= walkStart_) return walkFrom_;
        return walkFrom_ + (walkTo_ - walkFrom_) * (t - walkStart_) / (walkEnd_ - walkStart_);
    }
    void Retarget(double t) {
        const int target = std::max(fe_.level(), game_.level());
        if (target != target_) { walkFrom_ = CutLevel(t); walkTo_ = target; walkStart_ = t; walkEnd_ = t + 3.0; target_ = target; }
    }
    Scenario sc_;
    int pol_;
    port::rate::Params rp_;
    uint64_t s_ = 88172645463325252ull;
    port::fegov::Governor fe_, game_;
    int target_ = 0;
    long forcedSlot_ = -1;
    double walkFrom_ = 0, walkTo_ = 0, walkStart_ = 0, walkEnd_ = 0;
    // window sums
    unsigned wPics_ = 0, wFe_ = 0, wDrawn_ = 0;
    double wFeSum_ = 0, wGameSum_ = 0;
    bool giveback_ = false;
    void ConfigGov(bool g30) {
        port::fegov::Config c;
        c.maxLevel = kMaxLevel;
        c.budgetMs = g30 ? port::fegov::kBudget30Ms : 15.5f;
        c.recover = true;
        fe_.Update(c);
        port::fegov::Config d = c;
        d.budgetMs = g30 ? port::fegov::kGameBudget30Ms : 15.5f;
        d.minPictures = 60;
        game_.Update(d);
    }
};

void Sim::Windows(double t) {
    port::fegov::Window w;
    w.pictures = wPics_;
    w.frontPics = wFe_;
    w.frontSumUs = static_cast<uint64_t>(wFeSum_ * 1000.0);
    ConfigGov(giveback_);
    fe_.Feed(w);
    port::fegov::Window g;
    g.pictures = wPics_;
    g.frontPics = wDrawn_;
    g.frontSumUs = static_cast<uint64_t>(wGameSum_ * 1000.0);
    game_.Feed(g);
    wPics_ = wFe_ = wDrawn_ = 0;
    wFeSum_ = wGameSum_ = 0;
    Retarget(t);
}

Result Sim::Run(bool wantSeams) {
    Result r;
    port::fegov::Config c;
    c.maxLevel = kMaxLevel;
    c.recover = true;
    c.minPictures = 60;
    fe_.Configure(c);
    game_.Configure(c);
    ConfigGov(false);
    port::pace::SteadyCadence cad;
    cad.Configure(true);
    cad.Force(0, 0);
    port::smooth60::Controller s60;
    s60.StageBegin(0, 0, false, true);
    port::rate::Controller rc;
    rc.Configure(rp_);
    rc.StageBegin(0, 0);
    bool even30 = false;
    double gfree = 0, base = 0, prevStart = 0, gpuFree = 0, rdFree = 0, feFree = 0, prevDisp = 0;
    std::vector<Pic> pics;
    std::vector<double> pendFinish;   // gpu finish times of pictures not yet reported
    struct Rep { double at; float fe, rd, gpu; };
    std::vector<Rep> reps;
    port::rate::Obs obs;
    obs.play = true;
    double endT = sc_.seconds * 1e6;
    long turns = 0;
    double lastSecT = 0;
    uint64_t lastWin = 0;
    std::vector<std::pair<double, int>> switches;   // time (s), +1 up to 60 / -1 down to 30
    double t60 = 0, t30 = 0, tprev = 0;
    bool lastOffered = true;
    int lastPh = 0;
    int lastDrawnTurn = -1;
    for (int k = 0;; ++k) {
        const double boundary = base + k * kT;
        double s = std::max(gfree, boundary);
        const double behind = gfree > boundary ? gfree - boundary : 0.0;
        if (s >= endT) break;
        const double tsec = s / 1e6;
        (even30 ? t30 : t60) += (s - tprev);
        tprev = s;
        // reports of pictures finished by now
        for (size_t i = 0; i < reps.size();) {
            if (reps[i].at <= s) { rc.Picture(static_cast<uint64_t>(reps[i].at), reps[i].fe, reps[i].rd, reps[i].gpu, true); reps.erase(reps.begin() + i); }
            else ++i;
        }
        // governors' 5 s windows
        const uint64_t winIdx = static_cast<uint64_t>(tsec / 5.0);
        if (winIdx != lastWin) { lastWin = winIdx; giveback_ = (pol_ == kRate && rc.Giveback30()) || (pol_ == kSmooth && even30 && s60.Mode() == port::smooth60::k30); Windows(tsec); }
        // the policy
        bool breakNow = false;
        if (sc_.breakEvery > 0) { const double ph = std::fmod(tsec, sc_.breakEvery); breakNow = ph < kT / 1e6; }
        int act = 0;   // +1 to 60, -1 to 30
        if (pol_ == kForced) {
            static_cast<void>(0);
            const long slot = static_cast<long>((tsec - 8.0) / 7.0);
            if (tsec >= 8.0 && slot != forcedSlot_) { forcedSlot_ = slot; act = (slot % 2 == 0) ? -1 : +1; }
        } else if (pol_ == kSmooth) {
            if (tsec - lastSecT >= 1.0) {
                int cnt = 0;
                for (auto& p : pics) if (p.displayV * kT >= lastSecT * 1e6 && p.displayV * kT < tsec * 1e6) ++cnt;
                const double fps = cnt / (tsec - lastSecT);
                lastSecT = tsec;
                if (s60.Tick(static_cast<uint64_t>(s), fps, true) == port::smooth60::kTo30) act = -1;
            }
        } else if (pol_ == kRate) {
            port::rate::Gov g;
            const int lvl = std::max(fe_.level(), game_.level());
            g.atMax = lvl >= kMaxLevel;
            for (int l = lvl + 1; l <= kMaxLevel; ++l) { g.gainLeftFeMs += fe_.GainKnown(l) ? fe_.GainOf(l) : 1.0f; g.gainLeftGameMs += game_.GainKnown(l) ? game_.GainOf(l) : 1.0f; }   // (an unmeasured level is assumed to give 1.0 ms, GXV_RATE_GAIN_ASSUME_MS)
            obs.atBreak = breakNow;
            const port::rate::Action a = rc.Step(static_cast<uint64_t>(s), obs, g);
            if (std::getenv("RATE_TRACE") && rc.PhaseNow() != lastPh) { std::printf("      [t=%.2f] phase %d -> %d (levels fe %d game %d)\n", tsec, lastPh, rc.PhaseNow(), fe_.level(), game_.level()); lastPh = rc.PhaseNow(); }
            if (rc.TakeRaise()) { fe_.ForceLevel(kMaxLevel); game_.ForceLevel(kMaxLevel); Retarget(tsec); }
            { float lf, lg; if (rc.TakeLearned(&lf, &lg)) { fe_.LearnJump(lf); game_.LearnJump(lg); } }
            if (a == port::rate::kTo30) act = -1;
            if (a == port::rate::kTo60) act = +1;
        }
        if (act != 0) {
            even30 = act < 0;
            cad.Force(even30 ? 1 : 0, static_cast<uint64_t>(s));
            switches.push_back({tsec, act});
            if (act < 0 && sc_.tOverload >= 0 && r.tDown < 0 && tsec >= sc_.tOverload) r.tDown = tsec - sc_.tOverload;
            if (act > 0 && sc_.tRelief >= 0 && r.tUp < 0 && tsec >= sc_.tRelief) r.tUp = tsec - sc_.tRelief;
            if (wantSeams) { char b[200]; std::snprintf(b, sizeof b, "t=%.2f s turn %d to %s", tsec, k, act < 0 ? "even 30" : "60"); r.seams.push_back(b); }
        }
        // the turn
        const Load L = sc_.load(tsec);
        const double lvl = CutLevel(tsec);
        const double gs = sc_.gainScale;
        const double gameGain = 1.0 * gs * lvl, feGain = 1.5 * gs * lvl, rdGain = 0.6 * gs * lvl, gpuGain = 0.8 * gs * lvl;
        const double n1 = 1.0 + 0.04 * Rnd();
        const double drawnCost = std::max(2.0, (L.logic + L.draw - gameGain)) * n1 * 1000.0;
        const double undrawnCost = L.logic * n1 * 1000.0;
        double hitch = sc_.hitchMs ? sc_.hitchMs(tsec) : 0.0;
        // the pacer
        const bool offered = cad.Begin(static_cast<uint64_t>(s), true, false);
        int pending = 0;   // pictures the back end holds that its render thread has not handed to the GPU yet (port_vgx_backend_pending: queued - shown)
        for (auto& p : pics) if (p.renderEnd > s) ++pending;
        const bool late = behind > 0.5 * kT, busy = pending >= 2;
        const bool drawn = offered && !late && !busy;
        cad.Decision(offered, drawn);
        const bool missed = offered && !drawn;
        double cost = (drawn ? drawnCost : undrawnCost) + hitch * 1000.0;
        const double e = s + cost;
        if (drawn) {
            const double fe = std::max(1.0, L.fe - feGain) * (1.0 + 0.04 * Rnd()) * 1000.0;
            const double rd = std::max(1.0, L.render - rdGain) * (1.0 + 0.04 * Rnd()) * 1000.0;
            const double gpu = std::max(1.0, L.gpu - gpuGain) * (1.0 + 0.04 * Rnd()) * 1000.0;
            const double fs = std::max(e, feFree), fend = fs + fe;
            feFree = fend;
            // the render thread may not hand over a third picture to the GPU: it waits for the picture two back
            const double back = pics.size() >= 2 ? pics[pics.size() - 2].finish : 0.0;
            const double rs = std::max({fend, rdFree, back}), rend = rs + rd;
            rdFree = rend;
            const double gs2 = std::max(rend, gpuFree), gend = gs2 + gpu;
            gpuFree = gend;
            double v = std::max({std::ceil(gend / kT), prevDisp + 1.0, static_cast<double>(k + 1)});
            prevDisp = v;
            pics.push_back({k, v, gend, s, rend});
            reps.push_back({gend, static_cast<float>(fe / 1000.0), static_cast<float>(rd / 1000.0), static_cast<float>(gpu / 1000.0)});
            ++wPics_; ++wFe_; ++wDrawn_;
            wFeSum_ += fe / 1000.0;
            wGameSum_ += drawnCost / 1000.0;
            lastDrawnTurn = k;
        }
        (void)lastOffered; (void)lastDrawnTurn;
        lastOffered = offered;
        obs.drawn = drawn;
        obs.missed = missed && !even30;
        obs.gameMs = static_cast<float>(cost / 1000.0);
        obs.gapMs = static_cast<float>((s - prevStart) / 1000.0);
        prevStart = s;
        gfree = e;
        ++turns;
        // a stall of 80 ms or more is forgiven by the skip rule (the clock is re-based: no debt is paid back)
        if (cost >= 80000.0) base = e - (k + 1) * kT;
        // a hitch under 80 ms leaves debt: the next turns are late (code 2): the clock is not moved
    }
    const double dur = std::max(1.0, tprev / 1e6);
    r.sec60 = t60 / 1e6;
    r.sec30 = t30 / 1e6;
    r.pics = static_cast<int>(pics.size());
    r.fps = pics.size() / dur;
    r.speedPct = 100.0 * turns * kT / tprev;
    double prev = 0, longest = 0, latSum = 0;
    for (auto& p : pics) {
        const double hold = prev > 0 ? p.displayV - prev : 1;
        if (hold > longest && p.displayV * kT / 1e6 > 2.0) longest = hold;
        if (hold >= 3) ++r.hold3;
        prev = p.displayV;
        latSum += p.displayV - p.turn;
    }
    r.meanLat = pics.empty() ? 0 : latSum / pics.size();
    r.longestHold = longest;
    r.downs = 0; r.ups = 0;
    for (auto& sw : switches) (sw.second < 0 ? r.downs : r.ups)++;
    r.switchesPerMin = (r.downs + r.ups) * 60.0 / dur;
    if (pol_ == kRate) { r.failedUps = static_cast<int>(rc.FailedUps()); r.tries = static_cast<int>(rc.Ups() + rc.FailedUps()); }
    // the seam table: turn to picture and holds of the 10 pictures before / after each switch (first 3 switches)
    if (wantSeams) {
        int idx = 0;
        for (auto& sw : switches) {
            if (idx++ >= 4) break;
            const double tcut = sw.first * 1e6;
            double b = 0, a = 0, bm = 0, am = 0, hm = 0, pv = 0;
            int nb = 0, na = 0;
            for (auto& p : pics) {
                const double lat = p.displayV - p.turn;
                const double hold = pv > 0 ? p.displayV - pv : 1;
                pv = p.displayV;
                if (p.drawStart < tcut && p.drawStart > tcut - 700000) { b += lat; bm = std::max(bm, lat); ++nb; }
                if (p.drawStart >= tcut && p.drawStart < tcut + 700000) { a += lat; am = std::max(am, lat); ++na; }
                if (p.drawStart > tcut - 3.5 * kT && p.drawStart < tcut + 3.5 * kT) hm = std::max(hm, hold);   // the holds of the pictures within 3 turns of the seam
            }
            char buf[260];
            std::snprintf(buf, sizeof buf, "    seam %s: turn to picture before %.2f (max %.0f) at/after %.2f (max %.0f) refreshes, longest hold of the pictures within 3 turns of the seam %.0f refreshes", sw.second < 0 ? "60->30" : "30->60",
                          nb ? b / nb : 0.0, bm, na ? a / na : 0.0, am, hm);
            r.seams.push_back(buf);
        }
    }
    return r;
}

std::vector<Scenario> Scenarios() {
    std::vector<Scenario> v;
    auto zeroHitch = std::function<double(double)>();
    {   // 1. light, heavy 20 s (the cuts cannot close it: fe 22), light
        Scenario s; s.name = "light-heavy20-light"; s.seconds = 70; s.tOverload = 20; s.tRelief = 40;
        s.load = [](double t) { return t >= 20 && t < 40 ? Heavy(22) : Light(); };
        v.push_back(s);
    }
    {   // 1b. the same with a heavy the cuts CAN close (fe 17.2, 4 levels of 1.5)
        Scenario s; s.name = "heavy20 closable by cuts"; s.seconds = 70; s.tOverload = 20; s.tRelief = 40;
        s.load = [](double t) { Load l = t >= 20 && t < 40 ? Heavy(17.2) : Light(); if (t >= 20 && t < 40) { l.logic = 5; l.draw = 6; l.render = 10; l.gpu = 12; } return l; };
        v.push_back(s);
    }
    {   // 2. a load that sits exactly at one refresh (every lane at 16.7 +- 4 %)
        Scenario s; s.name = "exactly at one refresh"; s.seconds = 120;
        s.load = [](double) { return Load{5.0, 6.7, 16.7, 15.0, 16.0}; };
        v.push_back(s);
    }
    for (double per : {2.0, 5.0, 10.0, 30.0}) {   // 3. oscillation
        Scenario s; char b[60]; std::snprintf(b, sizeof b, "oscillates, period %.0f s", per); s.name = b; s.seconds = 120;
        s.load = [per](double t) { return std::fmod(t, per) < per / 2 ? Light() : Heavy(22); };
        v.push_back(s);
    }
    {   // 4. 70 ms hitches every 3 s on a light stage
        Scenario s; s.name = "70 ms hitch every 3 s"; s.seconds = 120;
        s.load = [](double) { return Light(); };
        s.hitchMs = [](double t) { static double last = -10; if (t - last >= 3.0 && t > 5) { last = t; return 70.0; } return 0.0; };
        v.push_back(s);
    }
    {   // 5. a 600 ms stall once
        Scenario s; s.name = "one 600 ms stall"; s.seconds = 120;
        s.load = [](double) { return Light(); };
        s.hitchMs = [](double t) { static bool done = false; if (!done && t > 20) { done = true; return 600.0; } return 0.0; };
        v.push_back(s);
    }
    for (double gs : {0.5, 2.0}) {   // 6. gains mis-predicted (the governors assume 0.8 ms a level until they measure; the true gain is gs x the model's)
        Scenario s; char b[60]; std::snprintf(b, sizeof b, "cut gains x%.1f of expected", gs); s.name = b; s.seconds = 90; s.gainScale = gs; s.tOverload = 20; s.tRelief = 55;
        s.load = [](double t) { Load l = t >= 20 && t < 55 ? Heavy(17.2) : Light(); if (t >= 20 && t < 55) { l.logic = 5; l.draw = 6; l.render = 10; l.gpu = 12; } return l; };
        v.push_back(s);
    }
    {   // 7. cannot hold 60 at all (fe 26)
        Scenario s; s.name = "cannot hold 60 (fe 26)"; s.seconds = 120; s.tOverload = 0;
        s.load = [](double) { return Heavy(26); };
        v.push_back(s);
    }
    {   // 8. the emulator: every lane three times as slow (a third of real speed): the controller must settle at 30 and stay (no flapping); game speed is the emulator's own
        Scenario s; s.name = "every lane x3 (emulator)"; s.seconds = 120; s.tOverload = 0;
        s.load = [](double) { return Load{15.0, 18.0, 27.0, 24.0, 30.0}; };
        v.push_back(s);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && !std::strcmp(argv[1], "-v");
    std::printf("rate_auto model: legacy = the pacer's own 60 (no rate control), smooth60 = port/smooth60.h one way, rate_auto = port/rate_auto.h\n");
    std::printf("%-28s %-9s %6s %6s %5s %7s %8s %9s %9s %6s %6s %6s\n", "scenario", "policy", "s@60", "s@30", "sw/min", "maxhold", "overl->30", "relief->60", "speed %", "fps", "hold3+", "ups/fail");
    std::vector<std::vector<Result>> all;
    for (const Scenario& sc0 : Scenarios()) {
        std::vector<Result> row;
        for (int pol = 0; pol < 3; ++pol) {
            Scenario sc = sc0;
            // the hitch lambdas hold a static: rebuild per run
            if (sc.name == "70 ms hitch every 3 s") sc.hitchMs = [last = -10.0](double t) mutable { if (t - last >= 3.0 && t > 5) { last = t; return 70.0; } return 0.0; };
            if (sc.name == "one 600 ms stall") sc.hitchMs = [done = false](double t) mutable { if (!done && t > 20) { done = true; return 600.0; } return 0.0; };
            if (const char* rb = std::getenv("RATE_BREAK")) sc.breakEvery = std::atof(rb);   // (a probe: 0 = no natural break ever)
            Sim sim(sc, pol);
            Result r = sim.Run(verbose && pol == 2);
            row.push_back(r);
            char d[24], u[24];
            if (r.tDown >= 0) std::snprintf(d, sizeof d, "%.1f s", r.tDown); else std::snprintf(d, sizeof d, "-");
            if (r.tUp >= 0) std::snprintf(u, sizeof u, "%.1f s", r.tUp); else std::snprintf(u, sizeof u, "-");
            std::printf("%-28s %-9s %6.1f %6.1f %5.2f %7.0f %8s %9s %9.1f %6.1f %6d %4d/%d\n", sc.name.c_str(), kPolName[pol], r.sec60, r.sec30, r.switchesPerMin, r.longestHold, d, u, r.speedPct, r.fps, r.hold3, r.ups, r.failedUps);
            if (verbose && pol == 2) for (auto& s : r.seams) std::printf("%s\n", s.c_str());
        }
        all.push_back(row);
    }
    // ---- the seam test: a light load, the rhythm toggled by hand (the real SteadyCadence::Force), six seams: what the picture queue and the holds do at a switch
    {
        Scenario sc; sc.name = "seam test"; sc.seconds = 60; sc.load = [](double) { return Light(); }; sc.breakEvery = 0;
        Sim sim(sc, kForced);
        Result r = sim.Run(true);
        std::printf("seam test (light load, six forced switches, no controller): turn to picture and holds of the pictures within +-0.7 s / 3 turns of each switch\n");
        double worst = 0;
        for (auto& l : r.seams) { std::printf("%s\n", l.c_str()); const char* h = std::strstr(l.c_str(), "seam: "); (void)h; const char* q = std::strstr(l.c_str(), "of the seam "); if (q) worst = std::max(worst, std::atof(q + 12)); }
        CHECKF(worst <= 2.0, "seam test: longest hold within 3 turns of a forced switch is %.0f refreshes (must be 2)", worst);
        CHECKF(r.longestHold <= 2.0, "seam test: longest hold of the whole run %.0f refreshes (must be 2)", r.longestHold);
    }
    // ---- checks (rate_auto = index 2)
    const auto sc = Scenarios();
    auto R = [&](size_t i) -> const Result& { return all[i][2]; };
    auto S = [&](size_t i) -> const Result& { return all[i][1]; };
    // 0 light-heavy-light
    CHECKF(R(0).tDown >= 0 && R(0).tDown < 1.5, "overload -> even 30 within 1.5 s (got %.2f)", R(0).tDown);
    CHECKF(R(0).tUp >= 0 && R(0).tUp < 9.0, "relief -> 60 within 9 s (got %.2f)", R(0).tUp);
    CHECKF(R(0).speedPct > 99.0, "game speed %.2f %% in light-heavy-light", R(0).speedPct);
    CHECKF(S(0).tUp < 0, "smooth60 never goes back up inside a stage");
    CHECKF(R(0).longestHold <= 3.0, "longest hold %.0f refreshes in light-heavy-light", R(0).longestHold);
    // 1 closable by cuts: no switch to 30, or a short one
    CHECKF(R(1).sec30 < 10.0, "a load the cuts can close spends %.1f s at 30", R(1).sec30);
    // 2 exactly at one refresh: few switches
    CHECKF(R(2).switchesPerMin < 3.0, "at one refresh: %.2f switches a minute", R(2).switchesPerMin);
    // 3 oscillations: never more than 6 switches a minute
    for (size_t i = 3; i <= 6; ++i) CHECKF(R(i).switchesPerMin <= 6.0, "%s: %.2f switches a minute", sc[i].name.c_str(), R(i).switchesPerMin);
    // 7 hitches: must stay at 60
    CHECKF(R(7).downs == 0, "70 ms hitches every 3 s: %d switches down (must be 0)", R(7).downs);
    // 8 stall
    CHECKF(R(8).downs == 0, "a 600 ms stall: %d switches down (must be 0)", R(8).downs);
    // 9, 10 mis-predicted gains
    for (size_t i = 9; i <= 10; ++i) CHECKF(R(i).speedPct > 99.0 && R(i).failedUps <= 2, "%s: speed %.2f, failed ups %d", sc[i].name.c_str(), R(i).speedPct, R(i).failedUps);
    // 11 cannot hold 60: settles at 30 with at most a couple of tries
    CHECKF(R(11).tries <= 3 && R(11).sec30 > 100.0, "cannot hold 60: %.1f s at 30, %d tries up", R(11).sec30, R(11).tries);
    CHECKF(R(12).downs == 1 && R(12).ups == 0 && R(12).sec30 > 100.0, "emulator-like (lanes x3): %d down, %d up, %.1f s at 30 (must settle: 1 / 0 / >100)", R(12).downs, R(12).ups, R(12).sec30);
    for (size_t i = 0; i < all.size(); ++i)
        CHECKF(R(i).speedPct >= all[i][0].speedPct - 0.2, "%s: game speed %.2f %% (legacy %.2f %%: a hitch or a stall costs its own time in every policy)", sc[i].name.c_str(), R(i).speedPct, all[i][0].speedPct);
    for (size_t i = 0; i < all.size(); ++i)
        if (i != 7 && i != 8 && i != 12) CHECKF(R(i).speedPct > 99.0, "%s: game speed %.2f %%", sc[i].name.c_str(), R(i).speedPct);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
