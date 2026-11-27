// popin24 (10 Nov): host test of platform/include/port/popin.h (the pure parts of the pop-in audit): the hysteresis, the ramp, the zoom
// ratio, the projected size, LodCtrl's distance chain with and without hysteresis, and the census (counting, the three largest, the line).
#include "port/popin.h"
#include "port/honey_shadow.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace port::popin;

static int g_fail = 0;
#define CHECK(c, ...)                                      \
    do {                                                   \
        if (!(c)) {                                        \
            ++g_fail;                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                      \
            std::printf("\n");                             \
        }                                                  \
    } while (0)

// the game's own chain (LodCtrl::update after the view group's forcing), 0 High, 1 Middle, 2 Low, -1 nothing
static int Original(float distance, float mid, float low, bool hasMiddle, bool hasLow) {
    if (distance < mid) return 0;
    if (!hasMiddle && distance < low) return 0;
    if (hasMiddle && distance < low) return 1;
    if (hasLow) return 2;
    return -1;
}

// popin31b: the knobs as the game reads them (once, from the environment); tools/test_popin.sh runs the binary with the environment of each case
static int EnvCase(const char* which) {
    float want[kFloorKinds];
    float base = 0.0f;
    if (std::strcmp(which, "env-a") == 0) {           // GXV_PB_MINPX=3 GXV_PB_MINPX_SHADOW=0 GXV_PB_MINPX_FUR=6
        base = 3.0f;
        for (float& w : want) w = 3.0f;
        want[kFlShadow] = 0.0f;
        want[kFlFur] = 6.0f;
    } else if (std::strcmp(which, "env-b") == 0) {    // GXV_PB_MINPX_DECOR=2 only
        for (float& w : want) w = 0.0f;
        want[kFlDecor] = 2.0f;
    } else {                                          // GXV_PB_MINPX=4: the single knob of popin31 sets every kind
        base = 4.0f;
        for (float& w : want) w = 4.0f;
    }
    CHECK(MinPx() == base, "%s: GXV_PB_MINPX %.1f", which, static_cast<double>(MinPx()));
    for (int k = 0; k < kFloorKinds; ++k)
        CHECK(MinPxKind(k) == want[k], "%s: %s is %.1f, want %.1f", which, FloorKindEnv(k), static_cast<double>(MinPxKind(k)), static_cast<double>(want[k]));
    CHECK(AnyFloorOn(), "%s: a floor is on", which);
    std::printf(g_fail ? "popin %s: FAILED (%d)\n" : "popin %s: PASS\n", which, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return EnvCase(argv[1]);
    {
        HoneyShadowHolds holds;
        int ids[6];
        bool last[6] = {};
        int flips = 0, baselineFlips = 0;
        bool baseline = true;
        for (int turn = 0; turn < 66; ++turn) {
            const float z = turn == 0 ? 2900 : turn == 65 ? 2880 : (turn & 1) ? 2790 : 2810;
            holds.begin();
            int added = 0;
            for (int j = 0; j < 6; ++j) {
                const int i = (turn > 2 && (turn & 2)) ? 5-j : j;
                const bool baseFar = z >= 2800;
                const bool keep = holds.keep(&ids[i], last[i], baseFar, z, 2800);
                const bool visible = !baseFar || keep;
                if (keep) ++added;
                if (i < 2 && turn && last[i] != visible) ++flips;
                CHECK(!keep || turn > 0, "never promotes a newly seen far shadow");
                if (z < 2800) CHECK(visible, "never delays an approaching shadow");
                if (z >= 2880) CHECK(!visible, "bounded departure band");
                last[i] = visible;
            }
            holds.end();
            if ((z >= 2800) != baseline) ++baselineFlips;
            baseline = z >= 2800;
            CHECK(added <= 2, "two-controller upper bound, got %d", added);
        }
        CHECK(flips == 4 && baselineFlips == 64, "incumbents must stay stable through reversals: %d / %d", flips, baselineFlips);
        holds.begin();
        CHECK(!holds.keep(ids, true, true, 1545, 1500), "shorter cuts get only a 2.5 percent band");
        holds.end();
        std::printf("Honeyhive: incumbent changed twice over 64 legacy threshold crossings, at most 2 extra eligible shadows\n");
    }
    std::mt19937 rng(1234);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    // 1. hysteresis 0 is the plain compare; with a band a swing inside the band does not flicker
    for (int i = 0; i < 20000; ++i) {
        const float limit = uni(1000, 6000), d = uni(0, 8000);
        const bool was = rng() & 1;
        CHECK(FarHyst(was, d, limit, 0.0f) == (d > limit), "band 0 is a plain compare");
        const float band = Band(limit, uni(1, 30));
        const bool h = FarHyst(was, d, limit, band);
        if (d > limit) CHECK(h, "beyond the limit is always far");
        if (d <= limit - band) CHECK(!h, "inside limit - band is always near");
        if (!was && d <= limit) CHECK(!h, "near stays near up to the limit");
    }
    {
        // a camera swinging +-3 % around the limit: flickers without the band, never with 8 %
        const float limit = 5000.0f;
        int flipsPlain = 0, flipsHyst = 0;
        bool a = false, b = false;
        for (int t = 0; t < 2000; ++t) {
            const float d = limit * (1.0f + 0.03f * std::sin(t * 0.37f));
            const bool a2 = FarHyst(a, d, limit, 0.0f), b2 = FarHyst(b, d, limit, Band(limit, 8.0f));
            flipsPlain += a2 != a;
            flipsHyst += b2 != b;
            a = a2;
            b = b2;
        }
        CHECK(flipsPlain > 100, "the plain compare flickers (%d)", flipsPlain);
        CHECK(flipsHyst <= 2, "the 8%% band does not (%d)", flipsHyst);
        std::printf("swing +-3%% around the limit: %d flips plain, %d with the 8%% band\n", flipsPlain, flipsHyst);
    }
    // idempotence (the decor / shadow / fur state is updated by every judgement of one position)
    for (int i = 0; i < 20000; ++i) {
        const float limit = uni(1000, 6000), d = uni(0, 8000), band = Band(limit, uni(0, 30));
        const bool was = rng() & 1;
        const bool once = FarHyst(was, d, limit, band);
        CHECK(FarHyst(once, d, limit, band) == once, "judging the same position twice gives the same state");
    }

    // 2. the ramp
    CHECK(Ramp(0, 2500, 400, 0.5f) == 1.0f && Ramp(2500, 2500, 400, 0.5f) == 1.0f, "1 up to the start");
    CHECK(Ramp(2900, 2500, 400, 0.5f) == 0.5f && Ramp(9000, 2500, 400, 0.5f) == 0.5f, "lo from start + span");
    CHECK(std::fabs(Ramp(2700, 2500, 400, 0.5f) - 0.75f) < 1e-6f, "linear in between");
    CHECK(Ramp(2501, 2500, 0, 0.5f) == 0.5f, "span 0 is a step");
    {
        float prev = 1.0f;
        for (float d = 2400; d < 3100; d += 10) {
            const float r = Ramp(d, 2500, 400, 0.5f);
            CHECK(r <= prev + 1e-6f && r >= 0.5f && r <= 1.0f, "monotone, within [lo, 1]");
            prev = r;
        }
    }

    // 3. zoom ratio: CameraContext.cpp zoomedFovy divides tan(fovy/2) by Z; the ratio gives Z back
    for (int i = 0; i < 2000; ++i) {
        const float game = uni(30, 60), Z = uni(1.0f, 2.0f);
        const float pic = 2.0f * std::atan(std::tan(game * 0.0087266463f) / Z) * 57.29578f;
        CHECK(std::fabs(ZoomRatio(game, pic) - Z) < 2e-3f, "ratio %.4f vs zoom %.4f", ZoomRatio(game, pic), Z);
    }
    CHECK(ZoomRatio(45, 45) == 1.0f, "not zoomed: 1");
    CHECK(ZoomRatio(45, 50) == 1.0f, "never below 1");
    CHECK(ZoomRatio(45, 0.0f) == 1.0f, "bad input: 1");
    CHECK(ZoomRatio(45, 2.0f) == 2.5f, "clamped at 2.5");

    // 4. projected size: a sphere of radius 100 at 2000, fov 45: 200 / (2 * 2000 * tan 22.5) = 12.07 % of the height
    CHECK(std::fabs(SizeFrac(100, 2000, 45) - 0.12071f) < 1e-4f, "size %.5f", SizeFrac(100, 2000, 45));
    CHECK(std::fabs(SizeFrac(100, 2000, 45.0f / 1.5f * 1.0f) - SizeFrac(100, 2000, 45)) > 0.0f, "a narrower fov shows it larger");
    {
        // the same size at zoom Z and distance * Z (what GXV_POPIN_ZOOM keeps)
        const float Z = 1.5f, pic = 2.0f * std::atan(std::tan(45 * 0.0087266463f) / Z) * 57.29578f;
        CHECK(std::fabs(SizeFrac(100, 2000 * Z, pic) - SizeFrac(100, 2000, 45)) < 1e-4f, "distance x zoom keeps the size: %.5f vs %.5f",
              SizeFrac(100, 2000 * Z, pic), SizeFrac(100, 2000, 45));
    }
    CHECK(SizeFrac(100, 0.5f, 45) == 0.0f, "behind / at the camera: 0");

    // 5. LodCtrl's chain: hysteresis 0 is the game's chain exactly; with a band the model stays at its farther level longer, never nearer
    long checks = 0;
    for (int i = 0; i < 200000; ++i) {
        const bool hm = rng() & 1, hl = rng() & 1;
        const float mid = uni(500, 3000), low = mid + uni(-500, 2000), d = uni(0, 6000);
        const int shown = static_cast<int>(rng() % 3);
        const int o = Original(d, mid, low, hm, hl);
        CHECK(LodLevel(d, mid, low, hm, hl, shown, 0.0f) == o, "hyst 0 differs from the game's chain");
        const int h = LodLevel(d, mid, low, hm, hl, shown, uni(1, 30));
        if (o >= 0 && h >= 0) CHECK(h >= o, "hysteresis never shows a nearer model than the game's chain (%d < %d)", h, o);
        if (shown == 0) CHECK(h == o, "a model showing High has no band");
        ++checks;
    }
    {
        // a Middle model oscillating around its distance (Mario running to and fro): swaps with and without hysteresis
        const float mid = 2000, low = 3000;
        int swapsPlain = 0, swapsHyst = 0, sp = 0, sh = 0;
        for (int t = 0; t < 2000; ++t) {
            const float d = mid * (1.0f + 0.05f * std::sin(t * 0.21f));
            const int p = LodLevel(d, mid, low, true, true, sp, 0.0f), q = LodLevel(d, mid, low, true, true, sh, 8.0f);
            swapsPlain += p != sp;
            swapsHyst += q != sh;
            sp = p;
            sh = q;
        }
        CHECK(swapsPlain > 50 && swapsHyst <= 2, "swaps %d plain, %d with the band", swapsPlain, swapsHyst);
        std::printf("LOD distance swinging +-5%% around the Middle distance: %d swaps plain, %d with the 8%% band (%ld chain checks)\n", swapsPlain, swapsHyst, checks);
    }

    // 6. the census
    {
        Census c;
        auto ev = [](int kind, bool appear, bool in, bool core, float size, const char* name) {
            Event e;
            e.kind = kind;
            e.appear = appear;
            e.inFrame = in;
            e.core = core;
            e.size = size;
            std::snprintf(e.name, sizeof e.name, "%s", name);
            return e;
        };
        c.turns = 600;
        c.Add(ev(kDecor, true, true, true, 0.01f, "FlowerA"));
        c.Add(ev(kLodPort, true, true, true, 0.20f, "Kuribo"));
        c.Add(ev(kLodOrig, true, true, false, 0.05f, "Rock"));
        c.Add(ev(kOrigClip, true, false, false, 0.30f, "Far"));  // outside: not an in-frame appearance
        c.Add(ev(kShadow, false, true, true, 0.04f, "Shadow"));  // a hide
        c.Add(ev(kFur, true, true, true, 0.60f, "Planet"));
        c.Add(ev(kZoomCull, true, true, false, 0.03f, "Goomba"));
        CHECK(c.Sum(c.appIn) == 5 && c.Sum(c.appOut) == 1, "in %u out %u", c.Sum(c.appIn), c.Sum(c.appOut));
        CHECK(c.Sum(c.appVis) == 4, "visible %u", c.Sum(c.appVis));
        CHECK(c.SumPort(c.appIn) == 4 && c.hideIn[kShadow] == 1 && c.hideVis[kShadow] == 1, "port-made %u", c.SumPort(c.appIn));
        CHECK(c.nTop == 3, "three largest kept");
        float lo = 9, hi = 0;
        for (int i = 0; i < c.nTop; ++i) { lo = std::fmin(lo, c.top[i].size); hi = std::fmax(hi, c.top[i].size); }
        CHECK(hi == 0.60f && lo == 0.05f, "kept sizes %.2f .. %.2f (0.60, 0.20, 0.05 expected)", lo, hi);
        char line[900];
        c.Format(line, sizeof line, 10.0);
        std::printf("%s\n", line);
        CHECK(std::strstr(line, "popin: 10 s (600 turns)") && std::strstr(line, "PORT-made 4") && std::strstr(line, "lod-port 1/1/0"), "line");
        c.Reset();
        CHECK(c.Sum(c.appIn) == 0 && c.nTop == 0 && c.turns == 0, "reset");
    }

    // 7. the knobs read once from the environment (default off)
    CHECK(LogOn() == 0 && HystPct() == 0.0f && ZoomMode() == 0 && ClipPadFactor() == 1.0f, "defaults are off");
    CHECK(CensusOn() == 0 && DemoMode() == 0 && LeadTurns() == 0.0f, "popin25 defaults are off");
    CHECK(DemoWiden() == 1.5f && LeadClipMax() == 600.0f && FlightSpeedMin() == 60.0f && FlightFarCam() == 2500.0f, "popin25 parameter defaults");

    // ---- popin25 (fly-overs) ---------------------------------------------------------------------------------------------------------
    // 8. the flight state: the game's own demo flags win; else a camera that is fast AND far from the player
    CHECK(ClassifyFlight(kFlightOpening, 0, 0, 60, 2500) == kFlightOpening, "opening camera");
    CHECK(ClassifyFlight(kFlightStarter, 500, 9000, 60, 2500) == kFlightStarter, "the game's flag wins over the camera rule");
    CHECK(ClassifyFlight(kFlightDemo, 0, 0, 60, 2500) == kFlightDemo && ClassifyFlight(kFlightStarGet, 0, 0, 60, 2500) == kFlightStarGet, "demo flags");
    CHECK(ClassifyFlight(0, 100, 3000, 60, 2500) == kFlightFar, "fast and far: a Launch Star flight");
    CHECK(ClassifyFlight(0, 100, 1500, 60, 2500) == kFlightNone, "fast but near the player (Mario running with the camera): no flight");
    CHECK(ClassifyFlight(0, 20, 6000, 60, 2500) == kFlightNone, "far but slow (a distant fixed camera): no flight");
    CHECK(ClassifyFlight(kFlightFar, 0, 0, 60, 2500) == kFlightNone, "kFlightFar is never a demo kind");
    // 9. the camera's speed: peak-held, a cut is no speed
    {
        CamTrack t;
        float pos[3] = {0, 0, 0};
        CHECK(!t.Update(pos, 1000.0f) && t.speed == 0.0f, "first sample: no speed");
        for (int i = 1; i <= 20; ++i) { pos[0] = 150.0f * i; t.Update(pos, 1000.0f); }
        CHECK(std::fabs(t.speed - 150.0f) < 1e-3f, "steady 150 u/turn (%.2f)", t.speed);
        pos[0] += 20.0f; t.Update(pos, 1000.0f);  // the camera slows to 20
        CHECK(t.speed > 100.0f && t.speed < 150.0f, "peak-held after the slow-down (%.2f)", t.speed);
        for (int i = 0; i < 60; ++i) { pos[0] += 20.0f; t.Update(pos, 1000.0f); }
        CHECK(t.speed < 30.0f, "decays to the real speed (%.2f)", t.speed);
        const float before = t.speed;
        pos[0] += 50000.0f;
        CHECK(t.Update(pos, 1000.0f) && t.speed <= before, "a jump of 50000 is a cut, not a speed (%.2f)", t.speed);
    }
    // 9b. the camera's turn rate and the frustum pad for a turning camera
    {
        CamTrack t;
        float d[3] = {0, 0, 1};
        t.UpdateDir(d);
        CHECK(t.turn == 0.0f, "first direction: no turn");
        for (int i = 1; i <= 10; ++i) { const float a = 0.03f * i; d[0] = std::sin(a); d[1] = 0; d[2] = std::cos(a); t.UpdateDir(d); }
        CHECK(std::fabs(t.turn - 0.03f) < 1e-4f, "0.03 rad a turn (%.5f)", t.turn);
        d[0] = 1; d[1] = 0; d[2] = 0;  // 90 degrees at once: a cut
        const float before = t.turn;
        CHECK(t.UpdateDir(d) && t.turn <= before, "a cut is not a turn rate");
        CHECK(LeadFramePad(0.03f, 0, 45, 0.25f) == 0.0f && LeadFramePad(0, 6, 45, 0.25f) == 0.0f, "no pad when off / not turning");
        const float p6 = LeadFramePad(0.03f, 6, 45, 0.25f);  // 0.18 / 0.414 = 0.435, capped 0.25
        CHECK(p6 == 0.25f, "capped (%.3f)", p6);
        CHECK(std::fabs(LeadFramePad(0.01f, 6, 45, 0.25f) - 0.06f / 0.41421356f) < 1e-4f, "0.01 rad a turn x 6 turns / tan(22.5 deg)");
        CHECK(LeadFramePad(0.01f, 6, 30.6f, 0.25f) > LeadFramePad(0.01f, 6, 45, 0.25f), "a narrower (zoomed) view needs more pad for the same turn");
    }
    // 10. GXV_PB_DEMO: mode 0 changes nothing; 1 widens and goes camera-centred in a flight; 2 turns the budget off
    for (int fl = 0; fl < kFlights; ++fl) {
        CHECK(!BudgetOffInFlight(0, fl) && !BudgetOffInFlight(1, fl), "modes 0 and 1 never switch the budget off");
        CHECK(FlightWiden(0, fl, 1.5f) == 1.0f, "mode 0: no widening");
        CHECK(FlightLodDistance(0, fl, 3000.0f, 200.0f) == 3000.0f, "mode 0: the game's own LOD distance");
        if (fl == kFlightNone) {
            CHECK(!BudgetOffInFlight(2, fl) && FlightWiden(1, fl, 1.5f) == 1.0f && FlightLodDistance(2, fl, 3000.0f, 200.0f) == 3000.0f, "no flight: nothing changes in any mode");
        } else {
            CHECK(BudgetOffInFlight(2, fl) && FlightWiden(1, fl, 1.5f) == 1.5f && FlightWiden(2, fl, 1.5f) == 1.0f, "flight %d", fl);
            CHECK(FlightLodDistance(1, fl, 3000.0f, 200.0f) == 200.0f && FlightLodDistance(1, fl, 100.0f, 2000.0f) == 100.0f, "the nearer of player and camera");
        }
    }
    CHECK(FlightLodScale(0.6f, 1.5f, false) > 0.89f && FlightLodScale(0.6f, 1.5f, false) < 0.91f, "PB_LOD 0.6 x 1.5 = 0.9");
    CHECK(FlightLodScale(0.8f, 1.5f, false) == 1.0f && FlightLodScale(0.6f, 1.0f, false) == 0.6f && FlightLodScale(0.6f, 3.0f, true) == 1.0f, "capped at 1; widen 1 keeps it; off = 1");
    // 11. the lead: 0 with the switch off or a standing camera; grows with the speed; capped
    CHECK(LeadMargin(150, 0, 5000) == 0.0f && LeadMargin(0, 6, 5000) == 0.0f && LeadMargin(150, 6, 0) == 0.0f, "no lead when off / standing");
    CHECK(LeadMargin(100, 6, 5000) == 600.0f && LeadMargin(300, 6, 5000) == 1250.0f && LeadMargin(300, 6, 1000) == 250.0f, "lead = speed x turns, at most a quarter of the distance");
    CHECK(LeadClipMargin(100, 6, 600) == 600.0f && LeadClipMargin(50, 6, 600) == 300.0f && LeadClipMargin(100, 0, 600) == 0.0f && LeadClipMargin(0, 6, 600) == 0.0f, "clip lead");
    {
        // 12. a fly-over past decoration groups (GXV_PB_DECOR 5000, group radius 600): the size of the pop on the picture when the group's near edge comes
        // within the distance. The camera flies at 150 u/turn straight at the group, fov 45. Without anything: pops at 5000. Widened 1.5: 7500. Lead 6
        // turns (900): 5900. Budget off: never. The pop's size is radius / (depth * tan(fov/2)): it shrinks as 1/depth but does not vanish.
        const float dist = 5000.0f, r = 600.0f, speed = 150.0f;
        auto popAt = [&](bool off, float widen, float lead) {
            if (off) return 0.0f;
            const float lim = dist * widen, lim2 = lim + LeadMargin(speed, lead, lim);
            return SizeFrac(r, lim2 + r, 45.0f);  // (the group's near edge at lim2: its centre r farther)
        };
        const float base = popAt(false, 1.0f, 0), wid = popAt(false, 1.5f, 0), lead = popAt(false, 1.0f, 6), both = popAt(false, 1.5f, 6), off = popAt(true, 1, 0);
        std::printf("fly-over, decor group r 600: pop %.1f%% of the height as is, %.1f%% widened x1.5, %.1f%% with a 6-turn lead, %.1f%% both, %.1f%% budget off\n",
                    base * 100, wid * 100, lead * 100, both * 100, off * 100);
        CHECK(base > lead && lead > 0 && wid > both && both > 0 && base > wid && off == 0.0f, "each rule makes the pop smaller; only 'off' removes it");
        // the hysteresis state is unchanged by the lead: FarHyst with margin added to the limit is still idempotent and plain at band 0
        for (int i = 0; i < 20000; ++i) {
            const float limit = uni(1000, 6000), d = uni(0, 9000), m = LeadMargin(uni(0, 300), uni(0, 12), limit);
            const bool was = rng() & 1;
            CHECK(FarHyst(was, d, limit + m, 0.0f) == (d > limit + m), "lead + band 0 is a plain compare");
            const bool once = FarHyst(was, d, limit + m, Band(limit + m, 8.0f));
            CHECK(FarHyst(once, d, limit + m, Band(limit + m, 8.0f)) == once, "lead + band: idempotent");
            if (d <= limit) CHECK(!FarHyst(false, d, limit + m, 0.0f), "a margin only appears things earlier, never later");
        }
    }
    {
        // 13. the census by cause: orig-far counts as the game's own; the line names the flight and the causes; a run resets
        FlightCensus fc;
        auto ev = [](int kind, bool in, bool core, float size, const char* name) {
            Event e; e.kind = kind; e.appear = true; e.inFrame = in; e.core = core; e.size = size; e.flight = kFlightOpening;
            std::snprintf(e.name, sizeof e.name, "%s", name); return e;
        };
        fc.turnsAll = 301; fc.turnsFlight = 301; fc.speedMax = 83; fc.camPlayerMax = 5200; fc.renderSkips = 2; fc.noModel = 40;
        fc.c.Add(ev(kOrigFar, true, true, 0.20f, "Tree"));
        fc.c.Add(ev(kOrigClip, true, false, 0.04f, "Rock"));
        fc.c.Add(ev(kDecor, true, true, 0.29f, "FlowerPatch"));
        fc.c.Add(ev(kFur, true, true, 1.10f, "Planet"));
        CHECK(fc.c.SumGame(fc.c.appIn) == 2 && fc.c.SumPort(fc.c.appIn) == 2 && fc.c.Sum(fc.c.appIn) == 4, "game %u port %u", fc.c.SumGame(fc.c.appIn), fc.c.SumPort(fc.c.appIn));
        char line[1100];
        fc.Format(line, sizeof line, 5.0, "opening-camera");
        std::printf("%s\n", line);
        CHECK(std::strstr(line, "popin25: 5 s [opening-camera") && std::strstr(line, "pops on screen 4 (centre well inside 3") && std::strstr(line, "orig-far 1/1") &&
              std::strstr(line, "decor 1/1") && std::strstr(line, "render-skip 2") && std::strstr(line, "no-model clip events ignored 40") && std::strstr(line, "up to 5200 from the player"), "line");
        CHECK(std::fabs(PixelHeight(0.29f, 544.0f) - 157.76f) < 0.01f, "pixel height");
        CHECK(std::strcmp(FlightName(kFlightOpening), "opening-camera") == 0 && std::strcmp(KindName(kOrigFar), "orig-far") == 0, "names");
        fc.Reset();
        CHECK(fc.c.Sum(fc.c.appIn) == 0 && fc.renderSkips == 0 && fc.turnsAll == 0, "reset");
    }

    // ================================================================ popin31 (17 Nov) ================================================================
    // 14. B0: the census's new kinds (far-speck, piece-thin) are the port's; per kind the largest pop; "at or above the floor" by ONE element; the governor
    //     attribution; the play / flight split's line
    {
        CHECK(std::strcmp(KindName(kFarSpeck), "far-speck") == 0 && std::strcmp(KindName(kPieceThin), "piece-thin") == 0 && kKinds == 11, "new kinds");
        CHECK(PortKind(kFarSpeck) && PortKind(kPieceThin) && PortKind(kDecor) && !PortKind(kOrigClip) && !PortKind(kOrigFar) && !PortKind(kLodOrig), "port kinds");
        // fix31: C185's plant reveal is counted (a member held back one picture: a port-made appearance)
        CHECK(std::strcmp(KindName(kPlantReveal), "plant-reveal") == 0 && PortKind(kPlantReveal), "plant-reveal kind");
        {
            Census c; Event e; e.kind = kPlantReveal; e.appear = true; e.inFrame = true; e.size = 0.01f;
            c.Add(e);
            CHECK(c.SumPort(c.appIn) == 1 && c.SumGame(c.appIn) == 0, "plant-reveal is port-made (%u)", c.SumPort(c.appIn));
        }
        FlightCensus fc;
        fc.c.floorFrac = 3.0f / 544.0f;
        auto ev = [](int kind, bool in, float size, float unit, uint8_t gov) {
            Event e; e.kind = kind; e.appear = true; e.inFrame = in; e.core = in; e.size = size; e.unit = unit; e.gov = gov;
            std::snprintf(e.name, sizeof e.name, "x"); return e;
        };
        fc.turnsAll = 300; fc.turnsRelease = 40;
        fc.c.Add(ev(kFarSpeck, true, 16.0f / 544.0f, -1.0f, 1));   // an enemy speck of 16 px, after a governor change
        fc.c.Add(ev(kPieceThin, true, 2.0f / 544.0f, -1.0f, 0));   // a star bit of 2 px: under the floor
        fc.c.Add(ev(kDecor, true, 0.22f, 2.5f / 544.0f, 1));       // a flower patch 22 % of the height, each flower 2.5 px: not at or above the floor
        fc.c.Add(ev(kOrigFar, true, 0.10f, -1.0f, 1));             // the game's own: no governor attribution
        CHECK(fc.c.SumPort(fc.c.appIn) == 3 && fc.c.SumGame(fc.c.appIn) == 1, "far-speck and piece-thin are port-made (%u)", fc.c.SumPort(fc.c.appIn));
        CHECK(fc.c.appFloor[kFarSpeck] == 1 && fc.c.appFloor[kPieceThin] == 0 && fc.c.appFloor[kDecor] == 0 && fc.c.appFloor[kOrigFar] == 1, "at or above the floor by one element");
        CHECK(fc.c.appGov == 2, "port-made pops after a governor change: %u", fc.c.appGov);
        CHECK(std::fabs(fc.c.maxSize[kDecor] - 0.22f) < 1e-6f && std::fabs(fc.c.maxSize[kFarSpeck] * 544.0f - 16.0f) < 1e-3f, "largest by kind");
        fc.govFe = 2; fc.govGame = 1; fc.govCpu = 2; fc.govMax = 3; fc.govChanges = 1;
        fc.ptclScaled = 600; fc.ptclThinDraws = 300; fc.ptclLeftOut = 3000;
        fc.floorKept[kShadow] = 1500; fc.releaseHeld[kDecor] = 12;
        char line[1600];
        fc.Format31(line, sizeof line, 5.0, "play", 3.0f, 544.0f);
        std::printf("%s\n", line);
        CHECK(std::strstr(line, "popin31: 5 s [play, 300 turns, 0 in a flight, 40 in the release") && std::strstr(line, "governors fe 2 game 1 far-draw 2 (max 3, 1 changes") &&
              std::strstr(line, "far-speck 16") && std::strstr(line, "decor 120") && std::strstr(line, "port-made pops at or above it (one element) 1, game's own 1") &&
              std::strstr(line, "shadow 5.0") && std::strstr(line, "release holds 12 (object-turns) decor 12") && std::strstr(line, "emitters scaled 2.0 a turn") &&
              std::strstr(line, "particles left out 10.0 a turn"), "popin31 line");
        fc.Format31(line, sizeof line, 5.0, "play", 0.0f, 544.0f);
        CHECK(std::strstr(line, "GXV_PB_MINPX off") != nullptr, "floor off in the line");
        fc.Reset();
        CHECK(fc.c.Sum(fc.c.appIn) == 0 && fc.c.appGov == 0 && fc.ptclScaled == 0 && fc.floorKept[kShadow] == 0 && fc.c.floorFrac == 3.0f / 544.0f, "reset keeps the floor only");
        Census off;  // (the floor off: nothing is counted at or above it)
        off.Add(ev(kFarSpeck, true, 0.5f, -1.0f, 0));
        CHECK(off.appFloor[kFarSpeck] == 0, "floor off: no floor count");
    }
    // 15. the popin31 knobs read once from the environment (default off / their documented values)
    CHECK(MinPx() == 0.0f && std::fabs(MinPxBand() - 0.25f) < 1e-6f && MinPxPtclRadius() == 200.0f && ReleaseRampTurns() == 60 && ReleaseQuietTurns() == 8, "popin31 defaults");

    // 16. mode 0 is unchanged: no floor = every cut may act, no release, factors untouched
    {
        FloorTurn f;  // (all zero: what port_popin_turn leaves with GXV_PB_MINPX unset)
        for (int i = 0; i < 20000; ++i) {
            const float r = uni(0, 5000), d = uni(0, 90000);
            const bool was = rng() & 1;
            CHECK(FloorMayCut(f, r, d, was) && FloorMayCut2(f, r, d * d, was), "no floor: the cut acts as before");
            CHECK(!ReleaseHolds(false, was, rng() & 1), "no release: nothing held");
            const float fac = uni(0.01f, 1.0f);
            CHECK(RampedFactor(fac, 1.0f) == fac, "weight 1: the factor bit for bit");
        }
        CHECK(FloorDist2(f, 200.0f) == 0.0f && FloorK(0.0f, 544, 0.4f, 0.4f, 1.76f) == 0.0f, "no floor: no particle distance, k 0");
        Release rel;
        CHECK(!rel.on && rel.PtclWeight(60) == 1.0f, "never flown: no release, full weight");
        for (int t = 0; t < 100; ++t) rel.Turn(false, (t % 7) == 0, 3, 8);  // (mode 0 / 1 never calls it; even if it ran with no flight it stays off)
        CHECK(!rel.on && rel.PtclWeight(60) == 1.0f, "no flight: never releasing");
    }

    // 17. B2: the floor never lets a cut take a thing that is at or above N px anywhere in the picture (exact projection of a sphere, 20,000 random cases each
    //     for a thing shown and a thing already cut), and it is monotone in size, zoom and rows
    {
        // the largest extent (pixels) of a sphere's outline on the picture plane, its centre `theta` off the axis
        auto truePx = [](double r, double d, double theta, double rows, double tP) {
            const double a = std::asin(r / d);
            if (theta + a >= 1.5707963) return 1e9;
            return (std::tan(theta + a) - std::tan(theta - a)) * rows / (2.0 * tP);
        };
        const float rowsSet[4] = {272.0f, 408.0f, 544.0f, 720.0f};
        int allowed = 0, checked = 0;
        double worst = 0.0;
        for (int i = 0; i < 40000; ++i) {
            const float tG = uni(0.25f, 0.80f), Z = uni(1.0f, 2.5f), tP = tG / Z;
            const float aspect = (rng() & 1) ? 960.0f / 544.0f : 4.0f / 3.0f;
            const float rows = rowsSet[rng() % 4], N = uni(0.5f, 64.0f), band = uni(0.0f, 0.5f), r = uni(1.0f, 3000.0f);
            const bool was = i & 1;
            FloorTurn f;
            FloorSet(f, N, band, rows, tP, tG, aspect);
            CHECK(f.kHeld > 0.0f && f.kNew >= f.kHeld, "k set, the band only asks more of a shown thing");
            const float k = was ? f.kHeld : f.kNew;
            const float d = (i % 5) == 0 ? uni(r * 1.01f, r * k * 4.0f) : r * k * uni(0.9f, 1.2f);
            const bool cut = FloorMayCut(f, r, d, was);
            CHECK(cut == FloorMayCut2(f, r, d * d, was) || std::fabs(d - r * k) < 1e-3f * d, "root and squared forms agree");
            ++checked;
            if (!cut) continue;
            ++allowed;
            const double cornerTrue = std::atan(tP * std::sqrt(1.0 + aspect * aspect));  // (the real picture's corner)
            const double a = std::asin(r / d);
            const double px = truePx(r, d, cornerTrue + a, rows, tP);  // (the worst place a sphere touching the picture can be)
            const double pxMid = truePx(r, d, uni(0.0f, static_cast<float>(cornerTrue + a)), rows, tP);
            CHECK(px < N && pxMid < N, "a %.1f px thing (limit %.2f) was let go: r %.1f d %.1f tP %.3f rows %.0f", px, N, r, d, tP, rows);
            CHECK(ProjPxBound(r, d, rows, tP, tG, aspect) < N * 1.0001f, "the bound agrees with k");
            CHECK(ProjPxBound(r, d, rows, tP, tG, aspect) >= px * 0.9999, "the bound is a bound (%.3f < %.3f)", ProjPxBound(r, d, rows, tP, tG, aspect), px);
            if (px / N > worst) worst = px / N;
        }
        CHECK(allowed > 5000, "enough cases let through (%d of %d)", allowed, checked);
        std::printf("floor: %d of %d random cases let a cut act; the largest of them was %.1f %% of the floor (never at or above it)\n", allowed, checked, worst * 100.0);
        // monotone in size (a larger thing is cut only farther), zoom (a zoomed picture asks more distance) and rows
        for (int i = 0; i < 20000; ++i) {
            const float tG = uni(0.25f, 0.80f), N = uni(0.5f, 64.0f), rows = uni(200.0f, 1100.0f), aspect = 960.0f / 544.0f;
            const float Z1 = uni(1.0f, 2.5f), Z2 = Z1 * uni(1.0f, 1.5f);
            CHECK(FloorK(N, rows, tG / Z2, tG, aspect) >= FloorK(N, rows, tG / Z1, tG, aspect) * 0.99999f, "zoom: k never falls");
            CHECK(FloorK(N, rows * uni(1.0f, 2.0f), tG / Z1, tG, aspect) >= FloorK(N, rows, tG / Z1, tG, aspect) * 0.99999f, "rows: k never falls");
            FloorTurn f;
            FloorSet(f, N, 0.25f, rows, tG / Z1, tG, aspect);
            const float r1 = uni(1.0f, 2000.0f), r2 = r1 * uni(1.0f, 3.0f), d = uni(10.0f, 400000.0f);
            const bool was = rng() & 1;
            if (FloorMayCut(f, r2, d, was)) CHECK(FloorMayCut(f, r1, d, was), "size: a smaller thing is cut wherever a larger one is");
            if (FloorMayCut(f, r1, d, false)) CHECK(FloorMayCut(f, r1, d, true), "a thing already cut stays cut wherever a shown one may be cut");
        }
        // the band: a distance swinging +-5 % round the floor's distance flickers the cut without it, never with 25 %
        {
            FloorTurn f0, f25;
            FloorSet(f0, 3.0f, 0.0f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
            FloorSet(f25, 3.0f, 0.25f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
            const float r = 100.0f, lim = r * f25.kHeld;
            bool a = false, b = false;
            int fa = 0, fb = 0;
            for (int t = 0; t < 2000; ++t) {
                const float d = lim * (1.0f + 0.05f * std::sin(t * 0.3f));
                const bool a2 = FloorMayCut(f0, r, d, a), b2 = FloorMayCut(f25, r, d, b);
                fa += a2 != a; fb += b2 != b; a = a2; b = b2;
            }
            CHECK(fa > 50 && fb <= 1, "band: %d flips without, %d with", fa, fb);
            std::printf("floor band: a thing swinging +-5%% round the floor's distance: %d flips with no band, %d with 25%%; 3 px at fov 45, 544 rows: a 100-unit radius is cut only beyond %.0f units\n",
                        fa, fb, static_cast<double>(r * f25.kHeld));
        }
        // the particle floor's distance and the size radius
        {
            FloorTurn f;
            FloorSet(f, 3.0f, 0.25f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
            CHECK(std::fabs(std::sqrt(FloorDist2(f, 200.0f)) - 200.0f * f.kNew) < 1.0f, "particle floor distance");
            CHECK(SizeRadius(300.0f, 0.0f, 0.0f) == 300.0f && SizeRadius(100.0f, 200.0f, 10.0f) == 260.0f && SizeRadius(500.0f, 200.0f, 10.0f) == 500.0f, "size radius");
        }
        // a flower group: the nearest member is never nearer than the bound says, and a member is never larger than the bound's radius (members placed as
        // PlantGroup places them: a box of half side h round the group's position, the clipping radius 160 + h * scale)
        for (int i = 0; i < 20000; ++i) {
            const float h = uni(0.0f, 1200.0f), sc = uni(0.3f, 3.0f), R = 160.0f + h * sc;
            const float cx = uni(-20000, 20000), cy = uni(-20000, 20000), cz = uni(-20000, 20000);
            const float gd = std::sqrt(cx * cx + cy * cy + cz * cz);
            float nearest = 0.0f;
            const float mr = PlantMember(gd, R, sc, &nearest);
            float trueNearest = 1e30f;
            for (int m = 0; m < 8; ++m) {
                const float mx = cx + uni(-h, h), my = cy + uni(-h, h), mz = cz + uni(-h, h);
                trueNearest = std::fmin(trueNearest, std::sqrt(mx * mx + my * my + mz * mz));
            }
            CHECK(nearest <= trueNearest + 1e-2f * gd + 1.0f, "nearest member %.1f > %.1f", nearest, trueNearest);
            CHECK(mr >= 200.0f, "member radius at least the spacing with the margin");
        }
    }

    // 18. B1: the release after a flight. 300 things beyond the budget's limit; the flight shows them all; after it a thing in the picture is never taken
    //     until it leaves the picture (or the camera cuts); the release ends once nothing was held for its quiet turns; the particle weight ramps
    {
        struct Thing { bool cut = false, inView = false; int leaveAt = 0; };
        for (int run = 0; run < 2; ++run) {
            Thing th[300];
            for (int i = 0; i < 300; ++i) th[i].leaveAt = 1 + static_cast<int>(rng() % 200);
            Release rel;
            uint32_t held = 0;
            int vanishedInView = 0, endTurn = -1;
            const int cutAt = run == 1 ? 50 : -1;  // run 1: a camera cut 50 turns after the flight
            for (int t = -30; t < 400; ++t) {
                const bool off = t < 0;                  // the flight
                rel.Turn(off, t == cutAt, held, 8);
                if (t == -1) CHECK(rel.PtclWeight(60) == 0.0f, "no particle thinning in a flight");
                held = 0;
                for (Thing& x : th) {
                    x.inView = t < x.leaveAt && !(cutAt >= 0 && t >= cutAt);  // (the camera turns away from them one by one; a cut changes the picture)
                    const bool rule = !off;               // (the budget wants every one of them, they are beyond its limit)
                    bool cut = rule && !(off) ;
                    if (cut && ReleaseHolds(rel.on, x.cut, x.inView)) { cut = false; ++held; }
                    if (cut && !x.cut && x.inView && !(cutAt >= 0 && t == cutAt)) ++vanishedInView;
                    x.cut = off ? false : cut;
                }
                if (!off && !rel.on && endTurn < 0) endTurn = t;
                if (t == 30 && run == 0) CHECK(rel.PtclWeight(60) > 0.45f && rel.PtclWeight(60) < 0.6f, "the ramp half way (%.2f)", rel.PtclWeight(60));
            }
            CHECK(vanishedInView == 0, "run %d: %d things vanished in the picture after the flight", run, vanishedInView);
            if (run == 0) CHECK(endTurn > 190 && endTurn < 230, "the release ends after the last thing left the picture plus the quiet turns (%d)", endTurn);
            if (run == 1) CHECK(endTurn == cutAt && rel.PtclWeight(60) == 1.0f, "a camera cut ends the release and the ramp (%d)", endTurn);
            int stillShown = 0;
            for (const Thing& x : th) stillShown += x.cut ? 0 : 1;
            CHECK(stillShown == 0, "every thing is cut once out of the picture (%d left)", stillShown);
        }
        std::printf("release: after a flight 300 far things left the picture one by one over 200 turns; none vanished in it; the release ended 8 quiet turns after the last; a camera cut ends it at once\n");
    }

    // ================================================================ popin31b (17 Nov): the floor by kind ===============================================
    // 19. the knobs: an unset kind takes GXV_PB_MINPX, a set one (0 included) its own; the census and floor kinds; nothing else is changed when every kind is off
    {
        CHECK(MinPxResolve(nullptr, 3.0f) == 3.0f && MinPxResolve("", 3.0f) == 3.0f, "an unset kind inherits GXV_PB_MINPX");
        CHECK(MinPxResolve("0", 3.0f) == 0.0f && MinPxResolve("6", 3.0f) == 6.0f && MinPxResolve("6", 0.0f) == 6.0f, "a set kind is its own (0 = off)");
        CHECK(MinPxResolve("0.1", 0.0f) == 0.5f && MinPxResolve("100", 0.0f) == 64.0f && MinPxResolve("-2", 5.0f) == 0.0f && MinPxResolve(nullptr, 0.0f) == 0.0f,
              "the same clamp as GXV_PB_MINPX");
        for (int k = 0; k < kFloorKinds; ++k) CHECK(MinPxKind(k) == 0.0f, "default: %s off", FloorKindEnv(k));
        CHECK(!AnyFloorOn(), "default: no floor");
        const int want[kKinds] = {-1, -1, kFlLod, kFlDecor, kFlShadow, kFlFur, -1, -1, kFlSpeck, kFlStarBit, kFlDecor};
        for (int k = 0; k < kKinds; ++k) CHECK(FloorKindOf(k) == want[k], "census kind %s -> floor kind %d (want %d)", KindName(k), FloorKindOf(k), want[k]);
        for (int k = 0; k < kKinds; ++k) if (!PortKind(k) || k == kZoomCull) CHECK(FloorKindOf(k) < 0, "the game's own kinds and the zoom cull have no floor");
        CHECK(std::strcmp(FloorKindName(kFlStarBit), "starbit") == 0 && std::strcmp(FloorKindEnv(kFlParticle), "GXV_PB_MINPX_PARTICLE") == 0, "names");
    }
    // 20. FloorSetKinds: each kind's constants are exactly FloorSet's for its px (the 40,000-case proof of 17 holds per kind), kinds are independent, and a
    //     kind at 0 never refuses a cut; the guards follow the kinds and the release
    {
        int cases = 0, allowed = 0;
        double worst = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const float tG = uni(0.25f, 0.80f), Z = uni(1.0f, 2.5f), tP = tG / Z, rows = (rng() & 1) ? 544.0f : 408.0f, band = uni(0.0f, 0.5f);
            const float aspect = 960.0f / 544.0f;
            float px[kFloorKinds];
            for (float& v : px) {
                const unsigned r = rng() % 4;
                v = r == 0 ? 0.0f : (r == 1 ? 3.0f : uni(0.5f, 64.0f));   // (repeats on purpose: kinds sharing a value share the constants)
            }
            FloorTurn f;
            FloorSetKinds(f, px, band, rows, tP, tG, aspect);
            for (int k = 0; k < kFloorKinds; ++k) {
                FloorTurn one;
                FloorSet(one, px[k], band, rows, tP, tG, aspect);
                CHECK(f.kHeldK[k] == one.kHeld && f.kNewK[k] == one.kNew, "kind %s: the constants of its own px (%.6g/%.6g vs %.6g/%.6g)", FloorKindName(k),
                      static_cast<double>(f.kHeldK[k]), static_cast<double>(f.kNewK[k]), static_cast<double>(one.kHeld), static_cast<double>(one.kNew));
            }
            CHECK(f.kHeld == 0.0f && f.kNew == 0.0f, "the kinds leave the base constants alone");
            // one random kind, one thing: a cut it allows is under that kind's floor anywhere in the picture
            const int k = static_cast<int>(rng() % kFloorKinds);
            const float r = uni(1.0f, 3000.0f);
            const bool was = rng() & 1;
            const float kk = was ? f.kHeldK[k] : f.kNewK[k];
            const float d = kk > 0.0f ? r * kk * uni(0.9f, 1.2f) : uni(r * 1.01f, r * 4000.0f);
            const bool cut = FloorMayCutK(f, k, r, d, was);
            CHECK(cut == FloorMayCut2K(f, k, r, d * d, was) || std::fabs(d - r * kk) < 1e-3f * d, "root and squared forms agree by kind");
            if (!(px[k] > 0.0f)) { CHECK(cut, "a kind at 0 never refuses (%s)", FloorKindName(k)); continue; }
            ++cases;
            if (!cut) continue;
            ++allowed;
            const double bound = ProjPxBound(r, d, rows, tP, tG, aspect);
            CHECK(bound < px[k] * 1.0001f, "kind %s let a %.2f px thing go (floor %.2f)", FloorKindName(k), bound, static_cast<double>(px[k]));
            if (bound / px[k] > worst) worst = bound / px[k];
        }
        CHECK(allowed > 2000 && cases > allowed, "enough kind cases (%d of %d)", allowed, cases);
        std::printf("floor by kind: %d of %d random kind cases let a cut act; the largest was %.1f %% of that kind's floor\n", allowed, cases, worst * 100.0);
        // independence: only the shadows on; every other kind cuts as without a floor, at any distance
        {
            float px[kFloorKinds] = {3.0f, 0, 0, 0, 0, 0, 0};
            FloorTurn f;
            FloorSetKinds(f, px, 0.25f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
            FloorGuards(f);
            CHECK(f.guardK[kFlShadow] == 1 && f.guard == 1, "shadows on: their guard and the any-guard");
            for (int k = 1; k < kFloorKinds; ++k) {
                CHECK(f.guardK[k] == 0, "%s: no guard", FloorKindName(k));
                CHECK(FloorMayCutK(f, k, 500.0f, 600.0f, false) && FloorMayCut2K(f, k, 500.0f, 600.0f * 600.0f, false) && FloorDist2K(f, k, 200.0f) == 0.0f,
                      "%s cuts as before", FloorKindName(k));
            }
            CHECK(!FloorMayCutK(f, kFlShadow, 500.0f, 600.0f, false), "a near large shadow is kept");
            CHECK(FloorMayCutK(f, -1, 500.0f, 600.0f, false) && FloorMayCutK(f, kFloorKinds, 500.0f, 600.0f, false), "no kind: no floor");
            f.releasing = 1;
            FloorGuards(f);
            for (int k = 0; k < kFloorKinds; ++k) CHECK(f.guardK[k] == 1, "the release guards every kind (%s)", FloorKindName(k));
            FloorClearKinds(f);
            f.releasing = 0;
            FloorGuards(f);
            CHECK(f.guard == 0, "cleared: no guard");
            for (int k = 0; k < kFloorKinds; ++k) CHECK(f.guardK[k] == 0 && FloorMayCutK(f, k, 1e4f, 1.0f, false), "cleared: %s acts as before", FloorKindName(k));
        }
        // the band per kind: a distance swinging +-5 % round the floor's distance flips the kind's cut at most once
        {
            float px[kFloorKinds] = {0, 3.0f, 0, 0, 0, 0, 0};
            FloorTurn f;
            FloorSetKinds(f, px, 0.25f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
            const float r = 100.0f, lim = r * f.kHeldK[kFlDecor];
            bool cut = false;
            int flips = 0;
            for (int t = 0; t < 2000; ++t) {
                const float d = lim * (1.0f + 0.05f * std::sin(t * 0.3f));
                const bool c2 = FloorMayCutK(f, kFlDecor, r, d, cut);
                flips += c2 != cut;
                cut = c2;
            }
            CHECK(flips <= 1, "band by kind: %d flips", flips);
            CHECK(std::fabs(std::sqrt(FloorDist2K(f, kFlDecor, 200.0f)) - 200.0f * f.kNewK[kFlDecor]) < 1.0f, "particle-style distance by kind");
        }
        // dedupe: kinds sharing a px share one FloorK; the result is the same as computing each (checked above bit for bit)
    }
    // 21. the census by kind: a kind's pops are counted at or above ITS floor; a kind at 0 counts none; the game's own kinds use the base
    {
        auto ev = [](int kind, float size) { Event e; e.kind = kind; e.appear = true; e.inFrame = true; e.size = size; return e; };
        FlightCensus fc;
        const float px[kFloorKinds] = {3.0f, 0.0f, 6.0f, 3.0f, 3.0f, 3.0f, 3.0f};
        fc.c.floorFrac = 3.0f / 544.0f;
        fc.c.SetKindFloors(px, 544.0f);
        fc.c.Add(ev(kShadow, 4.0f / 544.0f));     // 4 px shadow, floor 3: counted
        fc.c.Add(ev(kDecor, 40.0f / 544.0f));     // decor has no floor: never counted
        fc.c.Add(ev(kPlantReveal, 40.0f / 544.0f));
        fc.c.Add(ev(kFur, 5.0f / 544.0f));        // fur floor 6: a 5 px pop is not
        fc.c.Add(ev(kFur, 7.0f / 544.0f));        // ... 7 px is
        fc.c.Add(ev(kOrigFar, 4.0f / 544.0f));    // the game's own: the base 3 px
        CHECK(fc.c.appFloor[kShadow] == 1 && fc.c.appFloor[kDecor] == 0 && fc.c.appFloor[kPlantReveal] == 0 && fc.c.appFloor[kFur] == 1 && fc.c.appFloor[kOrigFar] == 1,
              "at or above each kind's own floor (shadow %u decor %u reveal %u fur %u orig-far %u)", fc.c.appFloor[kShadow], fc.c.appFloor[kDecor],
              fc.c.appFloor[kPlantReveal], fc.c.appFloor[kFur], fc.c.appFloor[kOrigFar]);
        fc.Reset();
        CHECK(fc.c.floorFracK[kShadow] == 3.0f / 544.0f && fc.c.floorFracK[kDecor] == 0.0f && fc.c.floorFracK[kOrigFar] < 0.0f, "reset keeps the kinds' floors");
        // the lines: popin31 names each kind's floor; the price line
        fc.turnsAll = 300;
        fc.floorKept[kShadow] = 3000;      // 10 kept shadows a turn
        fc.costN[kFlShadow] = 100; fc.costNs[kFlShadow] = 100 * 2000;   // 2 us each
        fc.shadowMemoLook = 200; fc.shadowMemoHit = 150;
        fc.floorKept[kFur] = 300;          // 1 kept coat a turn, its given-back shells 30 us
        fc.costN[kFlFur] = 10; fc.costNs[kFlFur] = 10 * 30000;
        fc.floorKept[kLodPort] = 600;      // 2 a turn, not timed
        fc.ptclFloorKept = 150;            // 0.5 emitters a turn
        CHECK(fc.KeptOf(kFlShadow) == 3000 && fc.KeptOf(kFlLod) == 600 && fc.KeptOf(kFlParticle) == 150 && fc.KeptOf(kFlDecor) == 0, "kept by floor kind");
        char line[1600];
        fc.Format31(line, sizeof line, 5.0, "play", 3.0f, 544.0f, px);
        std::printf("%s\n", line);
        CHECK(std::strstr(line, "GXV_PB_MINPX by kind shadow 3.0 decor 0.0 fur 6.0 lod 3.0") != nullptr, "popin31 line names the kinds");
        fc.Format31(line, sizeof line, 5.0, "play", 0.0f, 544.0f, px);
        CHECK(std::strstr(line, "GXV_PB_MINPX off") == nullptr && std::strstr(line, "by kind") != nullptr, "base off, a kind on: the floor part is printed");
        const float same[kFloorKinds] = {3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f};
        fc.Format31(line, sizeof line, 5.0, "play", 3.0f, 544.0f, same);
        CHECK(std::strstr(line, "; GXV_PB_MINPX 3.0: port-made") != nullptr, "every kind at the base: popin31's text");
        fc.Format31b(line, sizeof line, 5.0, "play", px);
        std::printf("%s\n", line);
        CHECK(std::strstr(line, "popin31b: 5 s [play, 300 turns]") && std::strstr(line, "shadow 3.0: 10.0 kept, 2.00 us each (100 timed; ground probe answered by the memo 75 %) = 20.0 us") &&
              std::strstr(line, "decor off") && std::strstr(line, "fur 6.0: 1.0 kept, 30.00 us each (10 timed) = 30.0 us") &&
              std::strstr(line, "lod 3.0: 2.0 kept (not timed") && std::strstr(line, "particle 3.0: 0.5 kept") &&
              std::strstr(line, "CPU given back by the timed kinds 50.0 us a turn (the untimed kinds"), "popin31b price line");
        const float off[kFloorKinds] = {0, 0, 0, 0, 0, 0, 0};
        fc.Format31b(line, sizeof line, 5.0, "play", off);
        CHECK(std::strstr(line, "shadow off; decor off") && std::strstr(line, "0.0 us a turn") && !std::strstr(line, "untimed"), "every kind off");
    }
    // 22. mode identity: with every kind off and no release, every site's guard is 0 and every cut acts (20,000 cases); the base form is unchanged
    {
        FloorTurn f;
        const float px[kFloorKinds] = {0, 0, 0, 0, 0, 0, 0};
        FloorSetKinds(f, px, 0.25f, 544.0f, 0.414f, 0.414f, 960.0f / 544.0f);
        FloorGuards(f);
        CHECK(f.guard == 0, "all off: no guard");
        for (int i = 0; i < 20000; ++i) {
            const int k = static_cast<int>(rng() % kFloorKinds);
            const float r = uni(0, 5000), d = uni(0, 90000);
            const bool was = rng() & 1;
            CHECK(f.guardK[k] == 0 && FloorMayCutK(f, k, r, d, was) && FloorMayCut2K(f, k, r, d * d, was) && FloorMayCut(f, r, d, was), "all off: the cut acts as before");
        }
    }

    std::printf(g_fail ? "popin: FAILED (%d)\n" : "popin: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
