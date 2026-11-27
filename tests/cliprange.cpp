// cliprange25 host test: platform/include/port/cliprange.h (bands, the "is it pure decoration" predicate on fake actors, the clip limit and its
// hysteresis, the far-pose rate, the draw ramp). tools/test_cliprange.sh builds it with ASan + UBSan.
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>
#include "port/cliprange.h"
using namespace port::cliprange;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestBands() {
    CHECK(BandOf(0) == 0, "0");
    CHECK(BandOf(1000.0f * 1000.0f) == 0, "edge 1000 is band 0");
    CHECK(BandOf(1000.1f * 1000.1f) == 1, "just over 1000");
    CHECK(BandOf(2500.0f * 2500.0f) == 1, "2500");
    CHECK(BandOf(2500.5f * 2500.5f) == 2, "2500.5");
    CHECK(BandOf(5000.0f * 5000.0f) == 2, "5000");
    CHECK(BandOf(9999.0f * 9999.0f) == 3, "9999");
    CHECK(BandOf(10000.0f * 10000.0f) == 3, "10000");
    CHECK(BandOf(1.0e12f) == 4, "huge");
    CHECK(BandOf(INFINITY) == 4, "inf");
    int last = 0;  // monotone
    for (float d = 0; d < 20000; d += 7) { const int b = BandOf(d * d); CHECK(b >= last, "monotone"); last = b; }
}

// A fake actor made of the facts the game's parts give. Each part that makes an actor observable must take it out of kDecor.
static void TestClassify() {
    Facts f;
    CHECK(Classify(f) == kNoModel, "empty = nomodel");
    f.hasModel = true;
    CHECK(Classify(f) == kDecor && PureDecor(f), "model only = decor");
    struct Case { const char* name; void (*set)(Facts&); Kind want; };
    const Case cases[] = {
        {"collision", [](Facts& x) { x.collision = true; }, kColl},
        {"sensors", [](Facts& x) { x.sensors = true; }, kSens},
        {"player", [](Facts& x) { x.player = true; }, kSens},
        {"effects", [](Facts& x) { x.effects = true; }, kDecorFx},
        {"sound", [](Facts& x) { x.sound = true; }, kDecorFx},
        {"stageSwitch", [](Facts& x) { x.stageSwitch = true; }, kDecorFx},
        {"rail", [](Facts& x) { x.rail = true; }, kDecorFx},
        {"camCtrl", [](Facts& x) { x.camCtrl = true; }, kDecorFx},
        {"pointer", [](Facts& x) { x.pointer = true; }, kDecorFx},
        {"freeJoints", [](Facts& x) { x.freeJoints = true; }, kPlant},
        {"pbDecor", [](Facts& x) { x.pbDecor = true; }, kPlant},
    };
    for (const Case& c : cases) {
        Facts g; g.hasModel = true; c.set(g);
        CHECK(Classify(g) == c.want, "%s -> %d", c.name, (int)Classify(g));
        CHECK(!PureDecor(g), "%s must not be pure decoration", c.name);
    }
    // exhaustive: of all 2^12 combinations exactly one is pure decoration (model, nothing else)
    int pure = 0;
    for (int m = 0; m < (1 << 12); ++m) {
        Facts g;
        g.hasModel = m & 1; g.collision = m & 2; g.sensors = m & 4; g.effects = m & 8; g.sound = m & 16; g.stageSwitch = m & 32;
        g.rail = m & 64; g.camCtrl = m & 128; g.freeJoints = m & 256; g.pbDecor = m & 512; g.player = m & 1024; g.pointer = m & 2048;
        if (PureDecor(g)) { ++pure; CHECK(m == 1, "pure decoration with extra bits %x", m); }
    }
    CHECK(pure == 1, "exactly one pure combination, got %d", pure);
}

static void TestLimit() {
    const float tan = 0.4663f;  // 50 deg fovy
    // never above the game's own distance, never nearer than near + radius
    unsigned r = 1;
    auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (r >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < 200000; ++i) {
        const float base = 5000.0f + rnd() * 55000.0f, pct = 20.0f + rnd() * 79.0f, j = rnd(), rad = 20.0f + rnd() * 1500.0f;
        const float lines = rnd() * 40.0f, nearD = 2500.0f;
        const float lim = ClipLimit(base, pct, j, rad, tan, lines, nearD);
        CHECK(lim <= base + 1e-3f, "limit above base");
        CHECK(lim >= std::fmin(base, nearD + rad) - 1e-3f, "limit under near+radius");
        // a bigger sphere is never clipped sooner than a smaller one with the same other inputs (size guard is monotone)
        const float lim2 = ClipLimit(base, pct, j, rad * 1.5f, tan, lines, nearD);
        CHECK(lim2 >= lim - 1e-2f, "bigger radius clipped sooner: %f < %f", lim2, lim);
        // a bigger percent never clips sooner
        const float lim3 = ClipLimit(base, std::fmin(99.0f, pct + 10.0f), j, rad, tan, lines, nearD);
        CHECK(lim3 >= lim - 1e-2f, "bigger percent clipped sooner");
        // at the limit an actor that is clipped by the scale alone (not by the near rule) is under `lines` lines or at the scaled distance
        if (lim < base - 1.0f && lim > nearD + rad + 1.0f && lines > 0.0f) {
            const float scaled = base * pct * 0.01f * (0.9f + 0.2f * j);
            CHECK(lim >= scaled - 1e-2f, "limit under the scaled distance");
            if (lim > scaled + 1.0f) CHECK(ProjLines(rad, lim, tan) <= lines * 1.001f, "size guard: %f lines at the limit", ProjLines(rad, lim, tan));
        }
    }
    // the size guard in plain numbers: radius 300 at 50 deg, 20 lines: stays until 300*456/(0.4663*20) = 14668 -> capped by base
    CHECK(std::fabs(ClipLimit(10000.0f, 50.0f, 0.5f, 300.0f, tan, 20.0f, 2500.0f) - 10000.0f) < 1e-2f, "big object keeps the game's far distance");
    CHECK(std::fabs(ClipLimit(10000.0f, 50.0f, 0.5f, 60.0f, tan, 20.0f, 2500.0f) - 5000.0f) < 1e-2f, "small object: half");
    // zero-ish inputs do not produce NaN
    const float z = ClipLimit(10000.0f, 50.0f, 0.5f, 0.0f, 0.0f, 0.0f, 2500.0f);
    CHECK(z == z && z > 0, "no NaN");
}

static void TestHysteresis() {
    // an object approaching and leaving across the limit with a jittering camera: flips only when the jitter is bigger than the band
    const float limit = 6000.0f, hyst = 0.06f;
    int flipsPlain = 0, flipsHyst = 0; bool a = false, b = false; unsigned r = 7;
    for (int i = 0; i < 100000; ++i) {
        r = r * 1664525u + 1013904223u;
        const float noise = ((r >> 8) * (1.0f / 16777216.0f) - 0.5f) * 2.0f * 100.0f;  // +-100 units
        const float d = limit + 100.0f * std::sin(i * 0.0005f) + noise;                // wanders +-100 around the limit
        const bool na = Beyond(d, limit, false, 0.0f), nb = Beyond(d, limit, b, hyst);
        flipsPlain += na != a; a = na; flipsHyst += nb != b; b = nb;
    }
    CHECK(flipsHyst * 20 < flipsPlain, "hysteresis should cut the flips a lot: %d vs %d", flipsHyst, flipsPlain);
    CHECK(!Beyond(5999.0f, limit, false, hyst) && Beyond(6001.0f, limit, false, hyst), "entry at the limit");
    CHECK(Beyond(5900.0f, limit, true, hyst) && !Beyond(5600.0f, limit, true, hyst), "exit at limit*(1-hyst)");
    // idempotent for one position
    for (float d = 5000; d < 7000; d += 13) { const bool x = Beyond(d, limit, true, hyst), y = Beyond(d, limit, true, hyst); CHECK(x == y, "idempotent"); }
}

static void TestFarAnim() {
    for (int n = 2; n <= 4; ++n) {
        for (uint32_t seed = 0; seed < 64; ++seed) {
            uint8_t s = 0xFF; int made = 0, maxRun = 0, run = 0, calls = 4000;
            bool first = true;
            for (int i = 0; i < calls; ++i) {
                const bool skip = FarAnimSkip(s, n, seed);
                if (first) { CHECK(!skip, "first far call is made"); first = false; }
                if (skip) { ++run; if (run > maxRun) maxRun = run; } else { ++made; run = 0; }
            }
            CHECK(maxRun <= n - 1, "n=%d seed=%u: %d poses left out in a row", n, seed, maxRun);
            CHECK(made >= calls / n && made <= calls / n + 2, "n=%d seed=%u: made %d of %d", n, seed, made, calls);
        }
        // phases spread: of 64 actors starting together, no turn has more than ceil(64/n)+8 of them posing
        int poses[4][4] = {};
        uint8_t st[64]; for (auto& x : st) x = 0xFF;
        std::vector<int> perTurn;
        for (int t = 0; t < 40; ++t) {
            int c = 0;
            for (uint32_t k = 0; k < 64; ++k) c += !FarAnimSkip(st[k], n, k * 7919u + (uint32_t)(k >> 2));
            if (t >= 8) perTurn.push_back(c);
        }
        int mx = 0; for (int c : perTurn) mx = c > mx ? c : mx;
        CHECK(mx <= 64 / n + 14, "n=%d: %d of 64 actors pose on one turn", n, mx);
        (void)poses;
    }
    uint8_t s = 3;
    CHECK(!FarAnimSkip(s, 1, 5) && s == 0xFF, "n=1 is off and resets");
    CHECK(!FarAnimSkip(s, 0, 5) && s == 0xFF, "n=0 is off and resets");
    uint8_t t = 200;  // an n lowered at run time with a stale count: made, then regular
    CHECK(!FarAnimSkip(t, 3, 9) && t < 3, "stale count recovers");
    CHECK(MovedTooMuch(1.0e6f, 3000.0f, 0.4663f, 0.5f), "1000 units at 3000 is many lines");
    CHECK(!MovedTooMuch(1.0f, 3000.0f, 0.4663f, 0.5f), "1 unit at 3000 is nothing");
    CHECK(MovedTooMuch(0.0f, 0.0f, 0.4663f, 0.5f), "no distance: always pose");
    // 0.5 line at 3000 = 0.5 * 2*3000*0.4663/456 = 3.07 units
    CHECK(!MovedTooMuch(3.0f * 3.0f, 3000.0f, 0.4663f, 0.5f) && MovedTooMuch(3.2f * 3.2f, 3000.0f, 0.4663f, 0.5f), "threshold");
}

static void TestDraw() {
    const float lines = 8.0f, ramp = 50.0f, hyst = 0.08f;
    // each actor's threshold is within [lines, lines*(1+ramp)]
    for (float j = 0; j < 1.0f; j += 0.01f) {
        const float t = DrawThreshold(lines, ramp, j);
        CHECK(t >= lines - 1e-4f && t <= lines * 1.5f + 1e-4f, "threshold range");
    }
    // a shrinking crowd leaves one by one: count how many actors change state on each step of 0.1 line
    int worstStep = 0; std::vector<float> js; for (int i = 0; i < 400; ++i) js.push_back(port::cliprange::Jitter01(0x80000000u + i * 0x58u));
    std::vector<char> was(js.size(), 0);
    for (float sz = 14.0f; sz > 4.0f; sz -= 0.1f) {
        int flips = 0;
        for (size_t i = 0; i < js.size(); ++i) { const bool c = DrawCulled(sz, DrawThreshold(lines, ramp, js[i]), was[i], hyst); flips += c != (bool)was[i]; was[i] = c; }
        if (flips > worstStep) worstStep = flips;
    }
    CHECK(worstStep <= 400 / 15, "crowd leaves together: %d of 400 in one 0.1-line step", worstStep);
    // nothing is culled above lines*(1+ramp)*(1+hyst); everything under `lines` is culled
    for (size_t i = 0; i < js.size(); ++i) {
        CHECK(!DrawCulled(lines * 1.5f * 1.001f, DrawThreshold(lines, ramp, js[i]), false, hyst), "over the ramp is drawn");
        CHECK(DrawCulled(lines * 0.99f, DrawThreshold(lines, ramp, js[i]), false, hyst), "under the threshold is culled");
        CHECK(!DrawCulled(lines * 1.5f * (1.0f + hyst) * 1.001f, DrawThreshold(lines, ramp, js[i]), true, hyst), "returns above hysteresis");
    }
    CHECK(Jitter01(0) >= 0 && Jitter01(~(uintptr_t)0) < 1.0f, "jitter range");
    // the jitter is spread: 4096 actor-like addresses cover all 10 deciles
    int dec[10] = {}; for (int i = 0; i < 4096; ++i) ++dec[(int)(Jitter01(0x81000000u + i * 0x1B0u) * 10.0f)];
    for (int d = 0; d < 10; ++d) CHECK(dec[d] > 250 && dec[d] < 600, "jitter decile %d has %d", d, dec[d]);
}

static void TestMisc() {
    CHECK(ProjLines(100, 5000, 0.4663f) > 19.5f && ProjLines(100, 5000, 0.4663f) < 19.7f, "proj %f", ProjLines(100, 5000, 0.4663f));
    CHECK(ProjLines(100, 50, 0.4663f) > 1e8f, "inside the sphere: huge");
    CHECK(SizeBucket(1) == 0 && SizeBucket(2) == 1 && SizeBucket(6) == 2 && SizeBucket(14) == 3 && SizeBucket(30) == 4 && SizeBucket(1e9f) == 4, "buckets");
    Census c; c.cell[kDecor][2].alive = 5; c.turns = 3; c.Reset();
    CHECK(c.cell[kDecor][2].alive == 0 && c.turns == 0, "reset");
}

int main() {
    TestBands(); TestClassify(); TestLimit(); TestHysteresis(); TestFarAnim(); TestDraw(); TestMisc();
    if (fails) { std::printf("cliprange: %d FAILED\n", fails); return 1; }
    std::printf("cliprange: ALL PASS\n");
    return 0;
}
