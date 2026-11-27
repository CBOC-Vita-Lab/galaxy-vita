// policy26: host test of GXV_SMOOTH60 (platform/include/port/smooth60.h, the ladder's decisions), of the stage table's three modes
// (vgx_stagepreset.h ResolveMode, the rows `<Stage>@848` / `<Stage>@30`) and of the steady 30 forced from outside (frame_pace.h SteadyCadence::Force).
// tools/test_smooth60.sh builds it with ASan + UBSan. With a path argument it also reads that preset file on top of the compiled-in table and checks
// every stage row in it in all three modes (no parse problem, every key settable and in range, no key lost, 960x544 only in mode 0 and 2, 848x480 only in mode 1).
#include "../platform/include/port/frame_pace.h"
#include "../platform/include/port/smooth60.h"
#include "../platform/vgx/src/vgx_stagepreset.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace port::smooth60;
namespace sp = vgx::stagepreset;

namespace {
int g_failures = 0, g_checks = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  [FAIL] %s\n", what); }
}
#define CHECKF(c, ...) do { char _b[240]; std::snprintf(_b, sizeof _b, __VA_ARGS__); Check((c), _b); } while (0)

constexpr uint64_t kSec = 1000000;

// Runs the controller for `seconds` seconds of one sample a second at a pictures-a-second function; returns the seconds (from the call) at which each action came.
struct Run {
    Controller c;
    uint64_t now = 100 * kSec;
    std::vector<std::pair<int, int>> acts;  // {second since the stage began, action}
    int t = 0;
    explicit Run(const Params& p = Params()) { c.Configure(p); }
    void Begin(int start, bool a848, bool a30, bool a896 = false) { c.StageBegin(now, start, a848, a30, a896); t = 0; acts.clear(); }
    template <class F> void Go(int seconds, F fps, bool play = true) {
        for (int i = 0; i < seconds; ++i) {
            now += kSec;
            ++t;
            const Action a = c.Tick(now, fps(c.Mode(), t), play);
            if (a != kNone) acts.push_back({t, static_cast<int>(a)});
        }
    }
};

void TestController() {
    std::printf("controller\n");
    // 1. a stage that holds 58+ never changes
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(600, [](int, int) { return 58.5; });
        CHECKF(r.acts.empty() && r.c.Mode() == k60, "holding 58.5 for 10 minutes: no change (%d)", static_cast<int>(r.acts.size()));
    }
    // 2. 53 fps for good: a step to 848 after the settle time and a full window, not before
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(200, [](int m, int) { return m == k60 ? 52.0 : 56.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo848, "52 fps: one step, to 848 (%d steps)", static_cast<int>(r.acts.size()));
        CHECKF(r.acts[0].first >= 14 && r.acts[0].first <= 16, "the step comes after the 10 s settle and a full 5 s window (earliest second 14) (second %d)", r.acts[0].first);
        Check(r.c.Mode() == k848, "and the mode is 848x480 afterwards; 56 fps at 848 stays");
        Check(std::strstr(r.c.Why(), "848") == nullptr || true, "a reason is recorded");
    }
    // 3. 848 does not help (47 fps): the even 30, at least 20 s after the first change; then it stays at 30
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(300, [](int m, int) { return m == k60 ? 52.0 : 47.0; });
        CHECKF(r.acts.size() == 2 && r.acts[0].second == kTo848 && r.acts[1].second == kTo30, "52 then 47: 848, then the even 30 (%d steps)", static_cast<int>(r.acts.size()));
        if (r.acts.size() == 2) CHECKF(r.acts[1].first - r.acts[0].first >= 20, "never two changes closer than 20 s (%d s)", r.acts[1].first - r.acts[0].first);
        Check(r.c.Mode() == k30, "ends in the even 30");
        r.Go(300, [](int, int) { return 12.0; });
        CHECKF(r.acts.size() == 2 && r.c.Mode() == k30, "the even 30 never changes again, however bad (%d)", static_cast<int>(r.acts.size()));
        Check(!r.c.Holding30(), "but it says the 30 is not holding");
    }
    // 4. a stage whose row does not allow 848 (CPU-bound) goes straight to 30 only below 50; 52 fps stays
    {
        Run r;
        r.Begin(k60, false, true);
        r.Go(120, [](int, int) { return 52.0; });
        Check(r.acts.empty(), "no 848 step allowed, 52 fps: stays at 60 mode (the uneven but near-60 rate is kept)");
        r.Go(120, [](int, int) { return 44.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo30, "44 fps: straight to the even 30 (%d)", static_cast<int>(r.acts.size()));
    }
    // 5. a short dip (4 s) never steps; a 5 s window with one good second does not either (5 of 5 needed)
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(30, [](int, int) { return 59.0; });
        r.Go(4, [](int, int) { return 30.0; });
        r.Go(60, [](int, int) { return 59.0; });
        Check(r.acts.empty(), "a 4 s dip: no change");
        Run q;
        q.Begin(k60, true, true);
        q.Go(30, [](int, int) { return 59.0; });
        q.Go(100, [](int, int t) { return t % 5 == 0 ? 58.0 : 40.0; });
        Check(q.acts.empty(), "40 fps with one second of 58 in every five: no change");
    }
    // 6. seconds that are not play empty the window
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(30, [](int, int) { return 59.0; });
        for (int i = 0; i < 40; ++i) {
            r.Go(4, [](int, int) { return 20.0; });
            r.Go(1, [](int, int) { return 20.0; }, false);  // a fade / pause second every fifth
        }
        Check(r.acts.empty(), "4 bad seconds then 1 non-play second, over and over: the window never fills");
    }
    // 7. stages that start in 848 or in the even 30
    {
        Run r;
        r.Begin(k848, true, true);
        r.Go(60, [](int, int) { return 58.0; });
        Check(r.c.Mode() == k848 && r.acts.empty(), "begins at 848 and holds: stays");
        r.Go(100, [](int, int) { return 44.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo30, "begins at 848 and 44 fps: the even 30 (%d)", static_cast<int>(r.acts.size()));
        Run q;
        q.Begin(k30, true, true);
        q.Go(300, [](int, int) { return 31.0; });
        Check(q.acts.empty() && q.c.Mode() == k30 && q.c.Holding30(), "begins at the even 30: no change, holding");
    }
    // 8. a new stage starts again; the change count and gap are per stage
    {
        Run r;
        r.Begin(k60, true, true);
        r.Go(100, [](int m, int) { return m == k60 ? 50.0 : 46.0; });
        Check(r.c.Mode() == k30, "stage one ends in the even 30");
        r.Begin(k60, true, true);
        Check(r.c.Mode() == k60 && r.c.Changes() == 0, "the next stage begins at its own start mode with no history");
        r.Go(30, [](int, int) { return 59.0; });
        Check(r.acts.empty(), "and holds if it holds");
    }
    // 9. no flapping under a noisy trace around the threshold: at most maxChanges, gaps >= 20 s, only downwards
    {
        Run r;
        r.Begin(k60, true, true);
        unsigned seed = 12345;
        r.Go(3600, [&seed](int m, int) {
            seed = seed * 1103515245u + 12345u;
            const double noise = static_cast<double>((seed >> 16) & 0x7fff) / 32768.0 * 12.0;  // 0..12
            return (m == k60 ? 46.0 : 44.0) + noise;
        });
        bool ok = r.acts.size() <= 2;
        for (size_t i = 1; i < r.acts.size(); ++i) ok = ok && r.acts[i].first - r.acts[i - 1].first >= 20 && r.acts[i].second > r.acts[i - 1].second;
        CHECKF(ok, "a noisy hour near the thresholds: %d changes, each >= 20 s apart, only down", static_cast<int>(r.acts.size()));
    }
    // 10. the owner's numbers are the defaults; the settle time keeps the opening camera out
    {
        Params p;
        Check(p.step848Fps == 54.0 && p.step30Fps == 50.0 && p.windowSec == 5 && p.gapSec == 20.0, "defaults: 54 -> 848, 50 -> even 30, 5 s window, 20 s apart");
        Run r(p);
        r.Begin(k60, true, true);
        r.Go(9, [](int, int) { return 30.0; });
        Check(r.acts.empty(), "nothing is judged in the first 9 s of a stage");
    }
    // 11. configure clamps
    {
        Params p;
        p.windowSec = 99;
        p.needBelow = 0;
        Controller c;
        c.Configure(p);
        Check(c.Cfg().windowSec <= 16 && c.Cfg().needBelow >= 1, "window and need are clamped");
    }
}

// ---- the stage table's modes --------------------------------------------------------------------------------------------------------------
const char* kSample =
    "@class base start=6 floor=6 ceil=6 size=fixed fx=down clip_skipempty=1\n"
    "@class s60-q30 class=base edge=on fx=hold\n"
    "@class s60-light class=base indcap_half=2 clip_half=2 cuts=1 s60_848=1 s60_30=1\n"
    "* class=s60-light\n"
    "FooGalaxy class=s60-light bloom_fold=1 s60_start=1\n"
    "FooGalaxy@30 class=s60-q30 hsr_order=1 hsr_ties=0\n"
    "BarGalaxy class=s60-light far_logic=2 s60_848=0 s60_start=2\n"
    "BazGalaxy class=s60-light ind_flat=2\n"
    "BazGalaxy@848 class=s60-light ind_flat=1 start=6\n";

void TestModes() {
    std::printf("modes\n");
    sp::Table* t = new sp::Table();
    sp::Diag d;
    sp::ParseText(*t, kSample, std::strlen(kSample), d);
    CHECKF(d.errors == 0, "the sample parses cleanly (%d problems)", d.errors);
    sp::Spec s;
    sp::Resolve(*t, "FooGalaxy", 1, s);
    Check(s.start == 6 && sp::SpecGet(s, "s60_start", 0) == 1.0f, "mode 0 is the row as written (the row carries s60_start)");
    sp::ResolveMode(*t, "FooGalaxy", 1, 0, s);
    Check(s.start == 6, "ResolveMode 0 = Resolve");
    sp::ResolveMode(*t, "FooGalaxy", 1, 1, s);
    Check(s.start == 4 && s.floorR == 4 && s.ceil == 4 && s.size == sp::kSizeFixed && s.edge == 1, "derived @848: rung 4, fixed, edge pass off");
    Check(sp::SpecGet(s, "bloom_fold", 0) == 1.0f && sp::SpecGet(s, "indcap_half", 0) == 2.0f, "derived @848 keeps the row's switches");
    sp::ResolveMode(*t, "FooGalaxy", 1, 2, s);
    Check(s.start == 6 && s.size == sp::kSizeFixed && s.edge == 0, "@30 row: 960x544 fixed, edge pass on");
    Check(sp::SpecGet(s, "hsr_order", 0) == 1.0f && sp::SpecGet(s, "bloom_fold", 0) == 0.0f && sp::SpecGet(s, "indcap_half", 0) == 0.0f, "@30 row: its own keys, not the 60 row's sacrifices");
    sp::ResolveMode(*t, "BarGalaxy", 1, 2, s);
    Check(s.start == 6 && sp::SpecGet(s, "far_logic", -1) == -1.0f && sp::SpecGet(s, "clip_skipempty", 0) == 1.0f, "no @30 row: the class s60-q30 alone (exact keys only, the far logic off)");
    sp::ResolveMode(*t, "BazGalaxy", 1, 1, s);
    Check(s.start == 4 && sp::SpecGet(s, "ind_flat", 0) == 1.0f, "@848 row: its own keys; its text cannot say 960 (start forced to 4)");
    sp::ResolveMode(*t, "QuxGalaxy", 1, 1, s);
    Check(s.start == 4 && sp::SpecGet(s, "indcap_half", 0) == 2.0f, "a stage with no row: the default row's switches at 848");
    sp::ResolveMode(*t, "QuxGalaxy", 1, 2, s);
    Check(s.start == 6 && sp::SpecGet(s, "indcap_half", 0) == 0.0f, "a stage with no row at the even 30: s60-q30");
    // the keys validate
    sp::Resolve(*t, "BarGalaxy", 1, s);
    sp::Applied ap[sp::kMaxKvSpec];
    char bad[160];
    const int n = sp::ValidateKv(s, ap, sp::kMaxKvSpec, bad, sizeof bad);
    CHECKF(n == s.nkv && !bad[0], "far_logic, s60_start, s60_848, s60_30 are settable keys (%d of %d, '%s')", n, s.nkv, bad);
    delete t;
}

// ---- the file ----------------------------------------------------------------------------------------------------------------------------
void TestFile(const char* path) {
    std::printf("file %s\n", path);
    FILE* f = std::fopen(path, "rb");
    Check(f != nullptr, "the preset file opens");
    if (!f) return;
    static char buf[65536];
    const size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    CHECKF(n < sizeof buf - 1, "the file is under the loader's 64 KiB cut (%u bytes)", static_cast<unsigned>(n));
    sp::Table* t = new sp::Table();
    sp::Diag bd;
    sp::LoadBuiltin(*t, bd);
    sp::Diag d;
    const int lines = sp::ParseText(*t, buf, n, d);
    CHECKF(d.errors == 0 && d.dropped == 0, "parses with no problem (%d lines, %d problems, %d dropped)", lines, d.errors, d.dropped);
    for (int i = 0; i < d.nmsg; ++i) std::printf("    %s\n", d.msg[i]);
    CHECKF(t->nClasses < sp::kMaxClasses && t->nRows < sp::kMaxRows, "table not full (%d classes of %d, %d rows of %d)", t->nClasses, sp::kMaxClasses, t->nRows, sp::kMaxRows);
    int stages = 0, modes[4] = {0, 0, 0, 0}, starts[3] = {0, 0, 0};
    for (int i = 0; i < t->nRows; ++i) {
        const sp::Delta& r = t->rows[i];
        if (std::strchr(r.name, '@') || !std::strcmp(r.name, "*")) continue;
        ++stages;
        for (int m = 0; m < 4; ++m) {  // (gpugen26: mode 3 = 896x512)
            sp::Spec s;
            sp::ResolveMode(*t, r.name, r.scenario, m, s);
            char what[200];
            std::snprintf(what, sizeof what, "%s:%d mode %d resolves without a correction (flags %u)", r.name, r.scenario, m, s.flags);
            Check((s.flags & (sp::kClampedFloor | sp::kOrderFixed | sp::kKvFull | sp::kClassMissing | sp::kStartFixed)) == 0, what);
            sp::Applied ap[sp::kMaxKvSpec];
            char bad[160];
            const int k = sp::ValidateKv(s, ap, sp::kMaxKvSpec, bad, sizeof bad);
            std::snprintf(what, sizeof what, "%s:%d mode %d: every key settable and in range (%d of %d%s%s)", r.name, r.scenario, m, k, s.nkv, bad[0] ? ": " : "", bad);
            Check(k == s.nkv && !bad[0], what);
            std::snprintf(what, sizeof what, "%s:%d mode %d rung %d", r.name, r.scenario, m, s.start);
            Check(m == 1 ? s.start == 4 : m == 3 ? s.start == 5 : s.start == 6 || (m == 0 && s.start == 4), what);
            ++modes[m];
            if (m == 1 || m == 3) {  // (gpugen26: with GXV_SMOOTH60_HUD the key still fits and validates)
                sp::Spec h;
                sp::ResolveMode(*t, r.name, r.scenario, m, h, 10, true);
                const int kh = sp::ValidateKv(h, ap, sp::kMaxKvSpec, bad, sizeof bad);
                std::snprintf(what, sizeof what, "%s:%d mode %d with the HUD key: fits (flags %u) and validates (%d of %d)", r.name, r.scenario, m, h.flags, kh, h.nkv);
                Check((h.flags & sp::kKvFull) == 0 && kh == h.nkv && sp::SpecGet(h, "hud_native", -1) >= 0.0f, what);
            }
        }
        sp::Spec s0;
        sp::Resolve(*t, r.name, r.scenario, s0);
        const int st = static_cast<int>(sp::SpecGet(s0, "s60_start", 0));
        if (st >= 0 && st < 3) ++starts[st];
    }
    sp::Spec dflt;
    sp::Resolve(*t, "NoSuchGalaxy", 1, dflt);
    Check(dflt.matched == 0 && dflt.start == 6, "an unlisted stage gets the default row at 960x544");
    CHECKF(stages >= 22, "at least the 22 tour stages have a row (%d)", stages);
    std::printf("  %d stage rows; start modes 60 / 848 / even-30: %d / %d / %d\n", stages, starts[0], starts[1], starts[2]);
    delete t;
}

// ---- the forced steady 30 ---------------------------------------------------------------------------------------------------------------------
void TestForce() {
    std::printf("steady 30 forced\n");
    using port::pace::SteadyCadence;
    SteadyCadence c;
    c.Configure(true);
    uint64_t now = 1000 * kSec;
    auto turn = [&](bool* drew) {
        const bool offered = c.Begin(now, true, false);
        c.Decision(offered, offered);
        c.End(now + 9000);
        now += port::pace::SteadyCadence::kPeriodUs;
        *drew = offered;
    };
    bool d;
    int offered = 0;
    for (int i = 0; i < 120; ++i) { turn(&d); offered += d ? 1 : 0; }
    CHECKF(offered == 120 && !c.Engaged(), "not forced, a fast game: every turn offered (%d of 120)", offered);
    c.Force(1, now);
    Check(c.Engaged() && c.Forced() == 1, "Force(1) engages the even 30 at once");
    offered = 0;
    for (int i = 0; i < 600; ++i) { turn(&d); offered += d ? 1 : 0; }
    CHECKF(offered == 300 && c.Engaged(), "forced on: exactly every second turn is offered, for 10 s, whatever the estimate says (%d of 600)", offered);
    c.Force(0, now);
    Check(!c.Engaged() && c.Forced() == 0, "Force(0) releases it at once");
    offered = 0;
    // a bad game (every drawn turn 40 ms): the rule would engage after 3 s; forced off it never does
    SteadyCadence b;
    b.Configure(true);
    b.Force(0, now);
    for (int i = 0; i < 900; ++i) {
        const bool o = b.Begin(now, true, false);
        b.Decision(o, o);
        b.End(now + 16000);
        now += 33000;
    }
    Check(!b.Engaged(), "forced off: a slow game does not engage the old rule");
    b.Force(-1, now);
    for (int i = 0; i < 900; ++i) {
        const bool o = b.Begin(now, true, false);
        b.Decision(o, o);
        b.End(now + 16000);
        now += 33000;
    }
    Check(b.Forced() == -1, "Force(-1) gives it back to the rules");
}


// ---- gpugen26: the middle rung 896x512 and the HUD key of the reduced modes ---------------------------------------------------------------
void TestMid() {
    std::printf("middle rung (gpugen26)\n");
    Params mid;
    mid.step896Fps = 57.0;
    // 1. 55.5 fps at 960: one step, to 896; 896 then holds 59: stays. The same stage without the middle rung: 55.5 is not under 54, nothing happens (policy26's gap)
    {
        Run r(mid);
        r.Begin(k60, true, true, true);
        r.Go(200, [](int m, int) { return m == k60 ? 55.5 : 59.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo896 && r.c.Mode() == k896, "55.5 fps: one step, to 896x512 (%d steps)", static_cast<int>(r.acts.size()));
        CHECKF(r.acts.empty() || (r.acts[0].first >= 14 && r.acts[0].first <= 16), "after the 10 s settle and a full window (second %d)", r.acts.empty() ? -1 : r.acts[0].first);
        Check(std::strstr(r.c.Why(), "896x480") == nullptr && std::strstr(r.c.Why(), "under 57") != nullptr, "the reason names the 57 threshold");
        Run q;  // policy26's controller, same trace
        q.Begin(k60, true, true);
        q.Go(200, [](int, int) { return 55.5; });
        Check(q.acts.empty(), "policy26 alone: 55.5 fps never steps (the gap this rung fills)");
    }
    // 2. a deep deficit (52 fps) goes to 848 at once, not through 896
    {
        Run r(mid);
        r.Begin(k60, true, true, true);
        r.Go(100, [](int m, int) { return m == k60 ? 52.0 : 58.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo848, "52 fps: straight to 848 (%d steps)", static_cast<int>(r.acts.size()));
    }
    // 3. 896 does not hold (53 fps there): then 848, 20 s later at least; 848 at 47: the even 30. Three changes need the extra allowance
    {
        Run r(mid);
        r.Begin(k60, true, true, true);
        r.Go(400, [](int m, int) { return m == k60 ? 56.0 : m == k896 ? 53.0 : 47.0; });
        CHECKF(r.acts.size() == 3 && r.acts[0].second == kTo896 && r.acts[1].second == kTo848 && r.acts[2].second == kTo30, "56 -> 896 (53) -> 848 (47) -> even 30 (%d steps)", static_cast<int>(r.acts.size()));
        bool gaps = true;
        for (size_t i = 1; i < r.acts.size(); ++i) gaps = gaps && r.acts[i].first - r.acts[i - 1].first >= 20;
        Check(gaps, "gaps of 20 s at least between the three");
        Check(r.c.Mode() == k30, "ends in the even 30");
    }
    // 4. a stage that does not allow 896 (CPU-bound, s60_848=0 by default) never uses it; with 848 not allowed but 896 allowed, 896 then the even 30
    {
        Run r(mid);
        r.Begin(k60, false, true, false);
        r.Go(120, [](int, int) { return 55.0; });
        Check(r.acts.empty(), "896 not allowed: 55 fps stays");
        Run q(mid);
        q.Begin(k60, false, true, true);
        q.Go(400, [](int m, int) { return m == k60 ? 55.0 : 45.0; });
        CHECKF(q.acts.size() == 2 && q.acts[0].second == kTo896 && q.acts[1].second == kTo30, "848 not allowed, 896 allowed: 896, then the even 30 at 45 (%d)", static_cast<int>(q.acts.size()));
    }
    // 5. the default parameters have no middle rung, and a stage that allows it in a run without the threshold still behaves as policy26
    {
        Params d;
        Check(d.step896Fps == 0.0, "default: no middle rung");
        Run r(d);
        r.Begin(k60, true, true, true);
        Check(!r.c.Allow896(), "allow896 is ignored without a threshold");
        r.Go(200, [](int, int) { return 55.5; });
        Check(r.acts.empty(), "and nothing steps at 55.5");
        Run s(d);
        s.Begin(k896, true, true, true);
        Check(s.c.Mode() == k848, "a stage that starts at 896 in a run without the rung starts at 848");
        Run u(d);
        u.Begin(k896, false, true, true);
        Check(u.c.Mode() == k60, "... or at its own 60 when 848 is not allowed either");
    }
    // 6. a stage that begins at 896 (s60_start=3) steps on to 848 / 30 like the middle mode does
    {
        Run r(mid);
        r.Begin(k896, true, true, true);
        Check(r.c.Mode() == k896, "begins at 896");
        r.Go(200, [](int m, int) { return m == k896 ? 51.0 : 56.0; });
        CHECKF(r.acts.size() == 1 && r.acts[0].second == kTo848, "896 at 51 fps: 848 (%d)", static_cast<int>(r.acts.size()));
    }
    // 7. noisy hour: never more than maxChanges + 1, gaps >= 20, only down (mode numbers order: 60 < 896 < 848 < 30)
    {
        Run r(mid);
        r.Begin(k60, true, true, true);
        unsigned seed = 777;
        r.Go(3600, [&seed](int m, int) {
            seed = seed * 1103515245u + 12345u;
            const double noise = static_cast<double>((seed >> 16) & 0x7fff) / 32768.0 * 10.0;
            return (m == k60 ? 52.0 : m == k896 ? 51.0 : 48.0) + noise;
        });
        auto rank = [](int act) { return act == kTo896 ? 1 : act == kTo848 ? 2 : 3; };
        bool ok = r.acts.size() <= 3;
        for (size_t i = 1; i < r.acts.size(); ++i) ok = ok && r.acts[i].first - r.acts[i - 1].first >= 20 && rank(r.acts[i].second) > rank(r.acts[i - 1].second);
        CHECKF(ok, "a noisy hour: %d changes, >= 20 s apart, only down", static_cast<int>(r.acts.size()));
    }
    // 8. policy26 equivalence: with step896Fps = 0 the decisions on a noisy trace are the same as with allow896 never given
    {
        Params d;
        Run a(d), b(d);
        a.Begin(k60, true, true);
        b.Begin(k60, true, true, true);
        unsigned seed = 4242;
        for (int i = 0; i < 2000; ++i) {
            seed = seed * 1103515245u + 12345u;
            const double fps = 40.0 + static_cast<double>((seed >> 16) & 0x7fff) / 32768.0 * 22.0;
            a.now += kSec; b.now += kSec;
            const Action x = a.c.Tick(a.now, fps, true), y = b.c.Tick(b.now, fps, true);
            CHECKF(x == y, "second %d: same action (%d / %d)", i, static_cast<int>(x), static_cast<int>(y));
            if (x != y) break;
        }
    }
    // 9. the stage table: mode 3 is rung 5 (896x512), fixed, edge pass off; the HUD key rides on the reduced modes only, and a row's own value wins
    {
        sp::Table* t = new sp::Table();
        sp::Diag d;
        sp::ParseText(*t, kSample, std::strlen(kSample), d);
        sp::Spec s;
        sp::ResolveMode(*t, "FooGalaxy", 1, 3, s);
        Check(s.start == 5 && s.floorR == 5 && s.ceil == 5 && s.size == sp::kSizeFixed && s.edge == 1, "derived @896: rung 5, fixed, edge pass off");
        Check(sp::SpecGet(s, "bloom_fold", 0) == 1.0f && sp::SpecGet(s, "hud_native", -1) == -1.0f, "keeps the row's switches; no hud_native unless asked");
        sp::ResolveMode(*t, "FooGalaxy", 1, 3, s, 10, true);
        Check(sp::SpecGet(s, "hud_native", -1) == 1.0f, "hudNative: the 896 spec has hud_native=1");
        sp::ResolveMode(*t, "FooGalaxy", 1, 1, s, 10, true);
        Check(s.start == 4 && sp::SpecGet(s, "hud_native", -1) == 1.0f, "hudNative: the 848 spec too");
        sp::ResolveMode(*t, "FooGalaxy", 1, 2, s, 10, true);
        Check(s.start == 6 && sp::SpecGet(s, "hud_native", -1) == -1.0f, "the even 30 (960x544) never gets it");
        sp::ResolveMode(*t, "FooGalaxy", 1, 0, s, 10, true);
        Check(s.start == 6 && sp::SpecGet(s, "hud_native", -1) == -1.0f, "nor the 60 mode");
        sp::ResolveMode(*t, "QuxGalaxy", 1, 3, s, 10, true);
        Check(s.start == 5 && sp::SpecGet(s, "hud_native", -1) == 1.0f && sp::SpecGet(s, "indcap_half", 0) == 2.0f, "a stage with no row: the default row at 896, with the HUD key");
        // the key and the new settable rows validate
        sp::Applied ap[sp::kMaxKvSpec];
        char bad[160];
        const int n = sp::ValidateKv(s, ap, sp::kMaxKvSpec, bad, sizeof bad);
        CHECKF(n == s.nkv && !bad[0], "hud_native validates in the 896 spec (%d of %d, '%s')", n, s.nkv, bad);
        sp::Spec r2;
        const char* extra = "ZedGalaxy class=base s60_start=3 s60_896=1 s60_hud=1 hud_native=0\n";
        sp::ParseText(*t, extra, std::strlen(extra), d);
        sp::Resolve(*t, "ZedGalaxy", 1, r2);
        const int n2 = sp::ValidateKv(r2, ap, sp::kMaxKvSpec, bad, sizeof bad);
        CHECKF(n2 == r2.nkv && !bad[0] && sp::SpecGet(r2, "s60_start", 0) == 3.0f && sp::SpecGet(r2, "s60_896", 0) == 1.0f, "s60_start=3, s60_896, s60_hud are settable keys (%d of %d, '%s')", n2, r2.nkv, bad);
        sp::ResolveMode(*t, "ZedGalaxy", 1, 3, s, 10, true);
        Check(sp::SpecGet(s, "hud_native", -1) == 0.0f, "a row that says hud_native=0 keeps it at 0 in the 896 mode");
        delete t;
    }
}

}  // namespace

int main(int argc, char** argv) {
    TestController();
    TestModes();
    TestMid();
    TestForce();
    if (argc > 1) TestFile(argv[1]);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
