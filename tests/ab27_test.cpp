// ab27: host test of the in-run A/B driver (platform/include/port/ab27.h) and of the registry's live knobs (platform/include/port/stage_knob.h).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "port/ab27.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace port::ab;

// ---- plan parsing ----
static void TestParse() {
    Plan p; Diag dg;
    const char* t = "GXV_GLOW26=0|1; GXV_ACROSS=0|2+GXV_PASSHOIST=0|2 @settle=3\n[HoneyBeeKingdomGalaxy]\n# c\nGXV_CPULOD=0|2 @window=8 @settle=2\nGXV_HSR_ORDER=1+GXV_HSR_ORDER_TIES=0|7";
    CHECK(ParseText(p, t, std::strlen(t), dg) == 4, "entries %d bad %d %s", 0, dg.bad, dg.first);
    CHECK(dg.bad == 0, "%s", dg.first);
    const Section* s = Select(p, "OtherGalaxy");
    CHECK(s && s->n == 2, "star section 2 entries");
    CHECK(s && !std::strcmp(s->e[0].name, "GXV_GLOW26=0|1"), "name '%s'", s ? s->e[0].name : "");
    CHECK(s && s->e[1].n == 2 && s->e[1].a[1].on == 2 && s->e[1].settleS == 3, "bundle + settle");
    s = Select(p, "HoneyBeeKingdomGalaxy");
    CHECK(s && s->n == 2 && s->e[0].windowS == 8 && s->e[0].settleS == 2, "stage section + options");
    CHECK(s && s->e[1].a[0].off == 1 && s->e[1].a[0].on == 1 && s->e[1].a[1].off == 0 && s->e[1].a[1].on == 7, "fixed companion");
    Plan q; Diag d2;
    const char* bad = "GXV_X=1; =3|4; GXV_Y=a|b; GXV_Z=1|2@bogus=1; GXV_OK=0|1";
    CHECK(ParseText(q, bad, std::strlen(bad), d2) == 1 && d2.bad == 4, "bad entries counted: %d", d2.bad);
    Params pr; pr.windowS = 6; pr.settleS = 1.5; pr.repeats = 3;
    const Section* st = Select(p, "OtherGalaxy");
    CHECK(std::fabs(TotalSeconds(*st, pr) - (2 * 3 * (3 + 6) + 2 * 3 * (1.5 + 6))) < 1e-9, "total %.2f", TotalSeconds(*st, pr));
}

// ---- a synthetic run: fps depends on the arm of a feature ----
struct Sim {
    double fpsOff = 50, fpsOn = 53, noise = 0.3;  // fps, window noise
    double busyOff = 18, busyOn = 17;
    uint64_t t = 1000000;
    int arm = 0, entry = -1;
    uint32_t rng = 12345;
    double Rand() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 65535.0 - 0.5; }
    Frame Next(double* wobble) {
        const double base = (entry == 0 ? (arm ? fpsOn : fpsOff) : 50.0) + *wobble;
        const double fps = base + noise * Rand();
        t += static_cast<uint64_t>(1e6 / fps);
        Frame f; f.presentUs = t;
        f.busyKnown = true; f.gpuN = 1; f.gpuBusyUs = static_cast<uint32_t>(1000.0 * (entry == 0 ? (arm ? busyOn : busyOff) : 18.0));
        f.gameKnown = true; f.gameTurns = 1; f.gameUs = 9000;
        f.frontKnown = true; f.frontPics = 1; f.frontUs = 12000;
        f.beKnown = true; f.beUs = 8000;
        return f;
    }
};
static Section MakeSec(int n, const char* const* names) {
    Section s; s.n = n;
    for (int i = 0; i < n; ++i) { Diag d; ParseEntry(names[i], s.e[i], d); }
    std::snprintf(s.stage, sizeof s.stage, "T");
    return s;
}
static void RunSim(Driver& d, Sim& sim, int maxFrames, int* flips, bool dropPlay = false) {
    double wob = 0;
    int n = 0;
    while (d.Running() && n++ < maxFrames) {
        Frame f = sim.Next(&wob);
        const bool play = !(dropPlay && n > 120 && n < 180);
        Action a = d.OnFrame(f, play);
        if (a.kind == kFlip) { sim.entry = a.entry; sim.arm = a.arm; ++*flips; }
        if (a.kind == kDone) break;
    }
}
static void TestDriver() {
    const char* names[2] = {"GXV_FEAT=0|1", "GXV_NOOP=0|1"};
    Section s = MakeSec(2, names);
    Params q; q.windowS = 2; q.settleS = 0.5; q.repeats = 3;
    Driver d; d.Begin(s, q, "TestStage");
    CHECK(d.Steps() == 12, "steps %d", d.Steps());
    Sim sim; int flips = 0;
    RunSim(d, sim, 200000, &flips);
    CHECK(d.Finished() && !d.Aborted(), "finished");
    CHECK(flips == 12, "flips %d", flips);
    const KeyResult& k0 = d.Result(0);
    CHECK(k0.nOff == 3 && k0.nOn == 3, "windows %d %d", k0.nOff, k0.nOn);
    const Delta df = DeltaOf(k0, kFps);
    CHECK(std::fabs(df.d - 3.0) < 0.5, "fps delta %.2f", df.d);
    const Delta dg = DeltaOf(k0, kBusy);
    CHECK(std::fabs(dg.d + 1.0) < 0.05, "gpu delta %.3f", dg.d);
    int am = -1;
    CHECK(VerdictOf(k0, &am) == kActed, "feature 0 acted");
    const KeyResult& k1 = d.Result(1);
    const Verdict v1 = VerdictOf(k1);
    CHECK(v1 == kNoEffect || v1 == kWithinNoise, "feature 1 has no effect: %s", VerdictText(v1));
    char line[512];
    FormatKeyLine(line, sizeof line, "TestStage", k0);
    CHECK(std::strstr(line, "acted") && std::strstr(line, "fps +") && std::strstr(line, "[TestStage]"), "%s", line);
    std::printf("%s\n", line);
    FormatDataLine(line, sizeof line, "TestStage", k0);
    CHECK(std::strstr(line, "verdict=acted") && std::strstr(line, "fps=+"), "%s", line);
    std::printf("%s\n", line);
    FormatKeyLine(line, sizeof line, "TestStage", k1);
    std::printf("%s\n", line);
}
static void TestNoise() {  // a big noise swamps a 0.5 fps effect: within noise
    const char* names[1] = {"GXV_FEAT=0|1"};
    Section s = MakeSec(1, names);
    Params q; q.windowS = 1; q.settleS = 0.3; q.repeats = 3;
    Driver d; d.Begin(s, q, "Noisy");
    Sim sim; sim.fpsOff = 50; sim.fpsOn = 50.6; sim.noise = 0; int flips = 0;
    // add window-level wobble by alternating the base each window: emulate via noise of 6 fps per frame average -> small; use direct windows instead
    sim.noise = 14.0;  // per-picture jitter: window means stay close, so make the arms' bases differ per window
    RunSim(d, sim, 200000, &flips);
    Verdict v = VerdictOf(d.Result(0));
    CHECK(v == kNoEffect || v == kWithinNoise || v == kActed, "ran");
    // direct: build a KeyResult by hand
    KeyResult k; std::snprintf(k.entry.name, sizeof k.entry.name, "X");
    const double off[3] = {50.0, 53.0, 47.0}, on[3] = {50.5, 52.0, 49.0};
    for (int i = 0; i < 3; ++i) { k.off[i].valid = true; k.off[i].fps = off[i]; k.on[i].valid = true; k.on[i].fps = on[i]; }
    k.nOff = k.nOn = 3;
    CHECK(VerdictOf(k) == kWithinNoise, "within noise %s", VerdictText(VerdictOf(k)));
    for (int i = 0; i < 3; ++i) k.on[i].fps = 50 + i * 0.02, k.off[i].fps = 50 + i * 0.02;
    CHECK(VerdictOf(k) == kNoEffect, "no effect %s", VerdictText(VerdictOf(k)));
    for (int i = 0; i < 3; ++i) { k.off[i].fps = 40 + i * 0.1; k.on[i].fps = 45 + i * 0.1; }
    CHECK(VerdictOf(k) == kActed, "acted");
    k.nOff = k.nOn = 1;
    CHECK(VerdictOf(k) == kInconclusive, "one window is inconclusive");
}
static void TestDisturbance() {
    const char* names[1] = {"GXV_FEAT=0|1"};
    Section s = MakeSec(1, names);
    Params q; q.windowS = 2; q.settleS = 0.5; q.repeats = 2;
    Driver d; d.Begin(s, q, "Dist");
    Sim sim; int flips = 0;
    RunSim(d, sim, 400000, &flips, true);
    CHECK(d.Finished() && d.Discarded() >= 1, "discarded %d", d.Discarded());
    CHECK(d.Result(0).nOff == 2 && d.Result(0).nOn == 2, "all windows still recorded %d %d", d.Result(0).nOff, d.Result(0).nOn);
    Driver e; e.Begin(s, q, "Abort");
    Frame f; f.presentUs = 1000000;
    Action a = e.OnFrame(f, true);
    CHECK(a.kind == kFlip && a.arm == 0, "first flip: off");
    a = e.Abort();
    CHECK(a.kind == kDone && e.Finished() && e.Aborted(), "abort");
}
// ---- the registry's live knobs ----
static int g_cbN = 0; static long g_cbOld, g_cbNew;
static void Cb(const char*, long o, long n) { ++g_cbN; g_cbOld = o; g_cbNew = n; }
static void TestLive() {
    using namespace port::stageknob;
    unsetenv("GXV_T27_A");
    std::atomic<int32_t>& a = LiveInt("GXV_T27_A", 5);
    CHECK(a.load() == 5, "default %d", a.load());
    CHECK(&a == &LiveInt("GXV_T27_A", 9), "same slot");
    port_knob_on_change("GXV_T27_A", 5, Cb);
    Set("GXV_T27_A", 7);
    CHECK(a.load() == 5, "not before Commit");
    Commit();
    CHECK(a.load() == 7 && g_cbN == 1 && g_cbOld == 5 && g_cbNew == 7, "after Commit %d cb %d", a.load(), g_cbN);
    Commit();
    CHECK(g_cbN == 1, "no callback when unchanged");
    ClearAll(); Commit();
    CHECK(a.load() == 5 && g_cbN == 2, "back to the default");
    setenv("GXV_T27_B", "3", 1);
    CHECK(LiveInt("GXV_T27_B", 0).load() == 3, "environment");
    CHECK(LiveValue("GXV_T27_B", -1) == 3 && LiveValue("GXV_T27_NONE", -1) == -1, "LiveValue");
    LiveInt("GXV_T27_C", 0, false);
    CHECK(!FlipSafeName("GXV_T27_C") && FlipSafeName("GXV_T27_A") && FlipSafeName("GXV_GLOW26") && !FlipSafeName("GXV_S60_848") && FlipSafeName("GXV_PASSHOIST") && !FlipSafeName("GXV_NOT_A_KNOB"), "flip classes");
    CHECK(FlipClassOfName("GXV_EVEN26") == 'w' && FlipClassOfName("GXV_ACROSS") == 'y' && FlipClassOfName("GXV_FE_GOV") == 'n', "classes w y n");
    // PORT_STAGE_KNOB sites follow the same Set / Commit
    auto site = [] { return PORT_STAGE_KNOB(int32_t, static_cast<int32_t>(Int("GXV_T27_S", 1))); };
    CHECK(site() == 1, "site default");
    Set("GXV_T27_S", 4); Commit();
    CHECK(site() == 4, "site flipped");
    ClearAll(); Commit();
    CHECK(site() == 1, "site restored");
}
int main() {
    TestParse(); TestDriver(); TestNoise(); TestDisturbance(); TestLive();
    std::printf(g_fail ? "ab27 test: %d FAILED\n" : "ab27 test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
