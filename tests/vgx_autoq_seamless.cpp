// seamless23: Config::seam (GXV_SEAMLESS_CUT, platform/vgx/src/vgx_autoq.cpp Controller::SeamHold): a step that raises the visible quality
// waits for a camera cut, a closed fade or the pause menu (FrameTimes::cut / quiet), at most seamWaitSec; reductions are not held.
// tools/test_vgx_seamless.sh builds and runs it.
//   1. Off: the decisions are exactly those without the option (the same change log, frame for frame).
//   2. On, no cut ever: the climb after an overload comes seamWaitSec later than without the option, as a timed-out wait.
//   3. On, a camera cut every 7 s: the climb comes at a cut (within seamCutMemSec of one), and no later than the first cut after the rule asks.
//   4. The step down for the overload is taken at the same picture with and without the option (the overload is never held).
//   5. QGUARD-style restore and the hold30 rules are covered by the existing suites with the option off; here the generic gate only.
#include "../platform/vgx/src/vgx_autoq.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
struct Ev { double t; int from, to; Why why; bool cut; };
// 400 s at 30 a second; the GPU light (5 ms at 640x456) except a heavy bump (40 ms) at 150-153 s; cutEvery > 0: a camera cut every cutEvery s.
std::vector<Ev> Run(bool seam, double cutEvery, const Controller::SeamStats** stats = nullptr) {
    static Controller* keep = nullptr;
    Config c;
    c.recover = true;
    c.targetFps = 30;
    c.minRung = kEmergency;
    c.budgetMs = 1000.0 / 30.0;
    c.seam = seam;
    delete keep;
    keep = new Controller(c);
    Controller& ctl = *keep;
    uint64_t now = 1, backend = 1;
    uint32_t game = 1, turns = 1;
    std::vector<Ev> log;
    double t = 0;
    for (long i = 0; i < 400L * 30L; ++i) {
        const bool bump = t >= 150.0 && t < 153.0;
        FrameTimes f;
        f.startUs = now;
        now += 33333;
        f.presentUs = now;
        backend += 3000;
        f.backendRunUs = backend;
        game += 12000;
        turns += 2;
        f.gameRunUs = game;
        f.gameTurns = turns;
        f.gameKnown = true;
        f.timers = f.busyKnown = true;
        f.gpuBusyUs = static_cast<uint32_t>((bump ? 40.0 : 5.0) * Cost(ctl.Current()) * 1000.0);
        f.cut = cutEvery > 0.0 && i % std::lround(cutEvery * 30.0) == 0;
        if (ctl.Add(f)) {
            const Change& ch = ctl.LastChange();
            log.push_back({t, ch.from, ch.to, ch.why, f.cut});
        }
        t = static_cast<double>(i + 1) / 30.0;
    }
    if (stats) *stats = &ctl.Seam();
    return log;
}
double FirstUpAfterDown(const std::vector<Ev>& log, double* downAt) {
    double down = -1;
    for (const Ev& e : log) {
        if (e.to < e.from && down < 0) down = e.t;
        if (down >= 0 && e.to > e.from) { if (downAt) *downAt = down; return e.t; }
    }
    if (downAt) *downAt = down;
    return -1;
}
}

int main() {
    std::printf("Config::seam (GXV_SEAMLESS_CUT)\n");
    const std::vector<Ev> off = Run(false, 0.0), offCuts = Run(false, 7.0);
    bool same = off.size() == offCuts.size();
    for (size_t i = 0; same && i < off.size(); ++i) same = off[i].t == offCuts[i].t && off[i].to == offCuts[i].to && off[i].why == offCuts[i].why;
    Check(same, "off: camera cuts change nothing (the same changes at the same pictures)");
    double dOff = 0, dWait = 0, dCut = 0;
    const double upOff = FirstUpAfterDown(off, &dOff);
    const Controller::SeamStats* st = nullptr;
    const std::vector<Ev> wait = Run(true, 0.0, &st);
    const double upWait = FirstUpAfterDown(wait, &dWait);
    const unsigned timedOut = st->timedOut;
    const std::vector<Ev> cut = Run(true, 7.0, &st);
    const double upCut = FirstUpAfterDown(cut, &dCut);
    const unsigned atCut = st->atCut;
    if (std::getenv("SEAM_VERBOSE")) {
        for (const Ev& e : off) std::printf("    off  t=%7.2f %d -> %d %s%s\n", e.t, e.from, e.to, WhyText(e.why), e.cut ? " (cut)" : "");
        for (const Ev& e : cut) std::printf("    cut  t=%7.2f %d -> %d %s%s\n", e.t, e.from, e.to, WhyText(e.why), e.cut ? " (cut)" : "");
    }
    std::printf("    step down at %.2f / %.2f / %.2f s; first step up after it at %.2f s (off), %.2f s (on, no cut), %.2f s (on, a cut every 7 s)\n", dOff, dWait,
                dCut, upOff, upWait, upCut);
    // (the window's phase moves with a cut's early evaluation: the same rule, the same overload hold, at most one window apart)
    Check(dOff > 0 && dOff == dWait && std::fabs(dCut - dOff) <= 0.45, "the overload's step down is never held (the same picture; with cuts within one window)");
    Check(upOff > 0 && upWait >= upOff + 14.0 && upWait <= upOff + 16.5 && timedOut >= 1, "no cut: the step up waits seamWaitSec (15 s), then is taken");
    double cutAt = -1;
    for (const Ev& e : cut) if (e.t == upCut) cutAt = std::fmod(e.t, 7.0);
    Check(upCut >= upOff && upCut <= upOff + 7.0 + 0.4 && cutAt >= 0 && cutAt <= 0.36 && atCut >= 1,
          "a cut every 7 s: the step up comes at a cut (within 0.35 s of it), at the first cut after the rule asks");
    unsigned upsInPlayOff = 0, upsOnCut = 0, upsOn = 0;
    for (const Ev& e : off) upsInPlayOff += e.to > e.from ? 1u : 0u;
    for (const Ev& e : cut) if (e.to > e.from) { ++upsOn; upsOnCut += std::fmod(e.t, 7.0) <= 0.36 ? 1u : 0u; }
    std::printf("    steps up: %u without the option; with it %u, %u of them at a cut\n", upsInPlayOff, upsOn, upsOnCut);
    Check(upsOn == upsOnCut, "every step up with the option (and cuts) is at a cut");
    std::printf("%s\n", g_failures ? "FAIL" : "PASS autoq seamless");
    return g_failures ? 1 : 0;
}
