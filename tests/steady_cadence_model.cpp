// GXV_STEADY30 (Performance mode's steady 30, frame_pace.h SteadyCadence) on a model of the console's pipeline (a model, not a console
// measurement). The game thread runs 60 turns a second on the vblanks (a turn that draws costs its logic, its draw and the draw-done
// wait for the front end's last picture; a late turn skips its retrace wait, code 2), the "Up to 60" skip rule (port::pace::LegacySkip,
// PETARI_FRAMESKIP=5, pipe depth 2, GXV_SKIP_MAXSTEP=3), then per drawn turn a front end, a renderer and a GPU stage, one picture at a
// time; the display callback is GXV_PRESENT_SYNC=2 (PresentSync). The controller's stage hint is the model's own mean costs.
// Prints per scene the pictures a second, the holds (vblanks a picture stays on screen) and the cadence's changes, and checks:
//   1. a scene that nearly reaches 60 never engages it;
//   2. a scene the game thread holds at 30-37 engages it within ~4 s, and then (nearly) every picture is held 2 vblanks; with a pair of
//      turns at the limit (the lit Observatory's kind) most of the 1-vblank holds go (the model's front-end-limited scenes already
//      come at an even 30 without it);
//   3. a GPU-limited scene does not engage it while the controller can still make the GPU's work smaller, and does when it cannot;
//   4. a heavy area followed (after a load) by a light one releases it within ~6 s;
//   5. a release that the pictures do not confirm engages again at once and blocks the next release (no flapping);
//   6. a pause (quiet) changes nothing; a load keeps the state;
//   7. off (Configure(false)) every turn is offered, as before.
//   g++ -std=c++17 -O2 -I platform/include tests/steady_cadence_model.cpp -o /tmp/steady_cadence_model && /tmp/steady_cadence_model
#include "port/frame_pace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}
constexpr double kV = 16.68335;  // ms per vblank

struct Scene {
    double logicMs = 6, drawMs = 4;  // the game thread: every turn, and a turn that draws on top
    double feMs = 14, beMs = 9, gpuMs = 11;
    double sd = 0.12;                // relative noise of every cost
    bool lowerable = true;           // the controller could still make the GPU's work smaller
    bool hintKnown = true;
};

struct Model {
    port::pace::LegacySkip rule{5, 2, 6, nullptr, 3};
    port::pace::SteadyCadence cad;
    port::pace::PresentSync sync;
    std::mt19937 rng;
    double now = 0, feFree = 0, beFree = 0, gpuFree = 0, cbFree = 0;
    std::deque<double> beDone, cbReturn;
    uint64_t turn = 0;
    double slot = 0;  // the vblank whose retrace the next wait consumes (the game's retrace queue: one retrace a turn)
    // the stretch since Mark()
    unsigned pictures = 0, turns = 0;
    uint64_t hold[5] = {};
    uint32_t lastShown = 0;
    bool haveShown = false;
    double engagedAtMs = -1, releasedAtMs = -1;
    unsigned engages = 0, releases = 0;
    bool cadenceOn;
    explicit Model(bool on, unsigned seed, const port::pace::SteadyCadence::Params& p = port::pace::SteadyCadence::Params()) : rng(seed), cadenceOn(on) {
        cad.Configure(on, p);
        sync.Configure(true, 1, 5, 3);
    }
    double Cost(double mean, double sd) {
        std::normal_distribution<double> d(mean, mean * sd);
        const double v = d(rng);
        return v < mean * 0.3 ? mean * 0.3 : v;
    }
    void Mark() {
        pictures = turns = 0;
        for (uint64_t& h : hold) h = 0;
    }
    double Rate() const { return turns ? 60.0 * pictures / turns : 0.0; }
    double HoldShare(int vblanks) const {
        uint64_t all = 0;
        for (uint64_t h : hold) all += h;
        return all ? static_cast<double>(hold[vblanks]) / static_cast<double>(all) : 0.0;
    }
    void Turn(const Scene& s, bool allowed = true, bool quiet = false) {
        const double start = now;
        while (!beDone.empty() && beDone.front() <= start) beDone.pop_front();
        const unsigned pending = static_cast<unsigned>(beDone.size());
        auto v = rule.Turn(static_cast<int64_t>(start * 1e6), allowed, pending);
        cad.Stages(s.hintKnown, static_cast<uint32_t>(s.gpuMs * 1000.0), static_cast<uint32_t>(std::fmax(s.gpuMs, std::fmax(s.feMs, s.beMs)) * 1000.0),
                   s.lowerable);
        const bool was = cad.Engaged();
        const bool offered = cad.Begin(static_cast<uint64_t>(start * 1000.0), allowed, quiet);
        if (allowed && !offered && v.code == 0) v.code = 1;
        cad.Decision(offered || !allowed, v.code == 0);
        double work = Cost(s.logicMs, s.sd);
        if (v.code == 0) {
            // the draw-done wait (deferred): the front end must be done with the last picture before this one's lists are written;
            // the front end then parses them as they are written (its own thread, behind the game's writes)
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
            if (haveShown && !quiet) {
                const uint32_t h = target - lastShown;
                ++hold[h >= 4 ? 4 : h];
            }
            lastShown = target;
            haveShown = true;
            if (!quiet) ++pictures;
        }
        const double end = start + work;
        cad.End(static_cast<uint64_t>(end * 1000.0));
        if (!quiet) ++turns;
        // (a late turn does not wait for its retrace; the others take the next retrace of the queue, at once when it has passed)
        slot += 1.0;
        if (end > (slot + 4.0) * kV) slot = std::floor(end / kV);
        now = v.code == 2 ? end : std::fmax(end, slot * kV);
        ++turn;
        if (cad.Engaged() && !was) { ++engages; engagedAtMs = now; }
        if (!cad.Engaged() && was) { ++releases; releasedAtMs = now; }
    }
    void Run(const Scene& s, double seconds, double quietFrom = -1, double quietTo = -1) {
        const double from = now, end = now + seconds * 1000.0;
        while (now < end) {
            const double t = (now - from) / 1000.0;
            Turn(s, true, t >= quietFrom && t < quietTo);
        }
    }
    void Print(const char* name) const {
        std::printf("    %-46s %5.1f pictures a second, held 1/2/3/4+ %5.1f/%5.1f/%5.1f/%4.1f %%, cadence %s (%u on, %u off)\n", name, Rate(),
                    100.0 * HoldShare(1), 100.0 * HoldShare(2), 100.0 * HoldShare(3), 100.0 * HoldShare(4), cad.Engaged() ? "ON " : "off",
                    engages, releases);
    }
};
}  // namespace

int main() {
    std::printf("a scene that nearly reaches 60\n");
    {
        Scene light;  // front end 14 ms: ~58 a second
        Model m(true, 1);
        m.Run(light, 60);
        m.Print("front end 14 ms");
        Check(m.engages == 0 && m.Rate() > 52.0, "the cadence never engages; the pictures stay near 60");
    }
    std::printf("scenes the CPU holds at 30-37 a second\n");
    {
        // The game thread: logic 10 ms a turn, 11 ms more when it draws: ~36 a second, a third of the pictures held 1 vblank
        Scene game;
        game.logicMs = 10;
        game.drawMs = 11;
        game.feMs = 18;
        Model off(false, 2), on(true, 2);
        off.Run(game, 30);
        on.Run(game, 30);
        off.Mark();
        on.Mark();
        off.Run(game, 30);
        on.Run(game, 30);
        off.Print("game 10 + 11 ms, no cadence");
        on.Print("game 10 + 11 ms, steady 30");
        char line[240];
        std::snprintf(line, sizeof line, "the cadence engages within 4.5 s (at %.1f s) and stays on (%u changes)", on.engagedAtMs / 1000.0, on.engages + on.releases);
        Check(on.engages == 1 && on.releases == 0 && on.engagedAtMs > 0 && on.engagedAtMs <= 4500.0, line);
        std::snprintf(line, sizeof line, "then %.0f %% of the pictures are held 2 vblanks (without it %.0f %%), at %.1f a second (without it %.1f)",
                      100.0 * on.HoldShare(2), 100.0 * off.HoldShare(2), on.Rate(), off.Rate());
        Check(on.HoldShare(2) >= 0.95 && off.HoldShare(2) < 0.8 && on.Rate() >= 29.0 && on.Rate() <= 30.5 && on.Rate() >= off.Rate() - 7.0, line);
    }
    {
        Scene game;   // the lit Observatory's kind: logic 10 ms a turn, 13 ms more when it draws (a pair just about two retraces)
        game.logicMs = 10;
        game.drawMs = 13;
        game.feMs = 18;
        Model off(false, 3), on(true, 3);
        off.Run(game, 30);
        on.Run(game, 30);
        off.Mark();
        on.Mark();
        off.Run(game, 30);
        on.Run(game, 30);
        off.Print("game 10 + 13 ms, no cadence");
        on.Print("game 10 + 13 ms, steady 30");
        char line[240];
        std::snprintf(line, sizeof line, "a pair of turns at the limit: 2-vblank holds %.0f -> %.0f %%, 1-vblank holds %.0f -> %.0f %% (%.1f -> %.1f a second)",
                      100.0 * off.HoldShare(2), 100.0 * on.HoldShare(2), 100.0 * off.HoldShare(1), 100.0 * on.HoldShare(1), off.Rate(), on.Rate());
        Check(on.cad.Engaged() && on.HoldShare(2) >= off.HoldShare(2) + 0.1 && on.HoldShare(1) <= off.HoldShare(1) * 0.5 && on.Rate() >= off.Rate() - 2.0, line);
    }
    std::printf("GPU-limited scenes are the controller's first\n");
    {
        Scene gpu;    // the GPU at 29 ms: ~32 a second
        gpu.gpuMs = 29;
        gpu.feMs = 12;
        Model m(true, 4);
        m.Run(gpu, 30);
        m.Print("GPU 29 ms, smaller picture possible");
        Check(m.engages == 0, "while a smaller picture or a step is left, the cadence stays off");
        gpu.lowerable = false;
        m.Mark();
        m.Run(gpu, 30);
        m.Print("GPU 29 ms, nothing left to lower");
        Check(m.engages == 1 && m.cad.Engaged(), "with nothing left to lower, it engages");
    }
    std::printf("the way back\n");
    {
        Scene heavy;
        heavy.logicMs = 10;
        heavy.drawMs = 11;
        heavy.feMs = 18;
        Scene light;
        Model m(true, 5);
        m.Run(heavy, 20);
        const bool engaged = m.cad.Engaged();
        const double from = m.now;
        m.Turn(light, false);  // (a load between the two areas)
        m.Mark();
        m.Run(light, 30);
        m.Print("game 10 + 11 ms, a load, then a light area");
        char line[200];
        std::snprintf(line, sizeof line, "a light area releases it within 6 s (after %.1f s), then the pictures are near 60 again (%.1f)",
                      (m.releasedAtMs - from) / 1000.0, m.Rate());
        Check(engaged && !m.cad.Engaged() && m.releases == 1 && m.releasedAtMs - from <= 6000.0 && m.Rate() > 50.0, line);
    }
    std::printf("the release block (unit check)\n");
    {
        // Driven turn by turn: engaged by a CPU limit, released by a good estimate, then pictures that fall again.
        port::pace::SteadyCadence c;
        c.Configure(true);
        uint64_t t = 1000;
        auto window = [&](bool drawEvery, double drawnMs, double otherMs, uint32_t pipeUs) {
            // 60 turns: a picture every turn (drawEvery) or every second turn
            for (int i = 0; i < 60; ++i) {
                c.Stages(true, 9000, pipeUs, true);
                const bool offered = c.Begin(t, true, false);
                const bool draw = offered && (drawEvery || (i % 2) == 0);
                c.Decision(offered, draw);
                c.End(t + static_cast<uint64_t>((draw ? drawnMs : otherMs) * 1000.0));
                t += 16683;
            }
        };
        // 33 pictures a second by the turns' own costs: d 24 ms, n 8 ms -> 60 (16.7 - 8) / (24 - 8) = 32.6
        for (int w = 0; w < 4; ++w) window(false, 24.0, 8.0, 20000);
        Check(c.Engaged(), "engaged after 3 windows under 38 with an estimate under 45");
        double releasedAt = -1;
        for (int w = 0; w < 6 && c.Engaged(); ++w) { window(false, 12.0, 6.0, 15000); if (!c.Engaged()) releasedAt = w; }
        Check(!c.Engaged() && releasedAt >= 3, "released after 4 windows whose estimate is 45 or more");
        window(false, 24.0, 8.0, 20000);   // one window under 38 within the confirmation time
        Check(c.Engaged() && c.BlockSec() == 30.0, "a release the pictures do not confirm engages again at once, the next release blocked 30 s");
        int windows = 0;
        while (c.Engaged() && windows < 120) { window(false, 12.0, 6.0, 15000); ++windows; }
        char line[160];
        std::snprintf(line, sizeof line, "... the next release comes only after the block (%d windows)", windows);
        Check(windows >= 30 && windows <= 36, line);
        window(false, 24.0, 8.0, 20000);
        Check(c.Engaged() && c.BlockSec() == 60.0, "a second unconfirmed release doubles the block");
    }
    std::printf("pauses and loads\n");
    {
        Scene heavy;
        heavy.feMs = 28;
        Model m(true, 7);
        m.Run(heavy, 10);
        const bool on = m.cad.Engaged();
        Scene pause;  // the pause menu: cheap, but quiet
        pause.feMs = 6;
        pause.gpuMs = 4;
        m.Run(pause, 20, 0, 20);
        Check(on && m.cad.Engaged() && m.releases == 0, "a 20 s pause menu (quiet, cheap) changes nothing");
        m.Turn(heavy, false);  // a load
        m.Run(heavy, 10);
        Check(m.cad.Engaged() && m.releases == 0, "a load into the same heavy area keeps it");
    }
    std::printf("off\n");
    {
        port::pace::SteadyCadence c;
        c.Configure(false);
        bool all = true;
        for (int i = 0; i < 600; ++i) { all = all && c.Begin(1000 + i * 16683ull, true, false); c.Decision(true, false); c.End(1000 + i * 16683ull + 30000); }
        Check(all && !c.Engaged(), "every turn is offered and nothing engages");
    }
    std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "all checks passed", g_failures);
    return g_failures ? 1 : 0;
}
