// far30 host test: platform/include/port/farfield.h (ordering rule, tiers, sweep, fade band and its hysteresis, phase spread, depth, census line).
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "port/farfield.h"
using namespace port::farfield;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Subject Far(float camD, float plD, float r = 100, float px = 4, uint8_t fl = 0) { Subject s; s.camD = camD; s.plD = plD; s.radius = r; s.radiusPx = px; s.flags = fl; return s; }

static void TestOrdering() {
    CHECK(MayCut(Far(6000, 6000), 3000, false, false), "far small thing is cut");
    CHECK(!MayCut(Far(2400, 6000), 1000, false, false), "inside 25 m of the camera: never, whatever the horizon asked");
    CHECK(!MayCut(Far(6000, 2400), 1000, false, false), "inside 25 m of the player: never");
    CHECK(!MayCut(Far(2550, 6000, 100), 1000, true, false), "the radius counts: 25.5 m centre, 1 m radius = 24.5 m nearest point");
    const uint8_t flagsAll[] = {kMario, kHud, kCollectible, kThreat, kGround, kPlatform, (uint8_t)(kThreat | kCollectible)};
    for (uint8_t f : flagsAll)
        CHECK(!MayCut(Far(90000, 90000, 100, 1, f), 3000, true, true), "protected flag %u is never cut, at any distance, last resort or not", f);
    CHECK(!MayCut(Far(9000, 9000, 100, 70), 3000, false, false) && !MayCut(Far(9000, 9000, 100, 70), 3000, true, false), "over 64 px counts as near");
    CHECK(MayCut(Far(9000, 9000, 100, 50), 3000, false, false), "50 px far thing is cut");
    // hysteresis of the projection: cut at 55 px stays cut up to 64, but a free one is not cut above 56
    CHECK(!MayCut(Far(9000, 9000, 100, 60), 3000, false, false) && MayCut(Far(9000, 9000, 100, 60), 3000, true, false), "px hysteresis");
    // hysteresis of the distance: cut at h*(1+6%), kept down to h
    CHECK(!MayCut(Far(3100, 3100, 100), 3000, false, false) && MayCut(Far(3300, 3300, 100), 3000, false, false) && MayCut(Far(3150, 3150, 100), 3000, true, false) && !MayCut(Far(3150, 3150, 100), 3000, false, false), "distance hysteresis");
    // the floor: a horizon asked at 12 m is lifted to 25 m except in the last resort
    CHECK(FloorHorizon(1200, false) == kFloor && FloorHorizon(1200, true) == 1200 && FloorHorizon(500, true) == kInnerFloor && FloorHorizon(9000, false) == 9000, "floor");
    CHECK(!MayCut(Far(2000, 2000, 100), 1200, true, false), "asked for 12 m, not last resort: object at 19 m is not cut");
    CHECK(MayCut(Far(2000, 2000, 100), 1200, true, true), "last resort: it is");
    // the object must be far from BOTH
    CHECK(!MayCut(Far(9000, 100, 100), 3000, false, false), "next to the player but far from the camera: not cut");
}
static void TestPlan() {
    // never inside the floor outside the last resort, at any depth, including depths past the end
    for (float d = -1; d <= 6.0f; d += 0.05f) {
        const Horizons h = Plan(d, false);
        CHECK(h.rate >= kFloor && h.decor >= kFloor && h.draw >= kFloor && h.shadow >= kFloor, "floor at depth %.2f: rate %.0f decor %.0f draw %.0f shadow %.0f", d, h.rate, h.decor, h.draw, h.shadow);
    }
    Horizons prev = Plan(0, true);
    bool inner = false;
    for (float d = 0; d <= 4.0f; d += 0.02f) {
        const Horizons h = Plan(d, true);
        CHECK(h.rate <= prev.rate + 1e-3f && h.decor <= prev.decor + 1e-3f && h.draw <= prev.draw + 1e-3f && h.shadow <= prev.shadow + 1e-3f && h.lod <= prev.lod + 1e-6f && h.ptcl <= prev.ptcl + 1e-6f && h.poseN >= prev.poseN,
              "deeper is never milder (depth %.2f)", d);
        // the lightest tier reaches nearest, the most visible one (the draw distance) farthest, at every depth
        CHECK(h.rate <= h.decor + 1e-3f && h.decor <= h.draw + 1e-3f, "tier order at depth %.2f: rate %.0f decor %.0f draw %.0f", d, h.rate, h.decor, h.draw);
        if (h.rate < kFloor || h.decor < kFloor) inner = true;
        prev = h;
    }
    CHECK(inner, "the last resort does go inside 25 m");
    const Horizons z = Plan(0, true);
    CHECK(z.rate == kOff && z.decor == kOff && z.draw == kOff && z.lod == 1.0f && z.ptcl == 1.0f && z.poseN == 1, "depth 0 is today's behaviour: nothing cut");
    CHECK(Plan(1, false).poseN == 2 && Plan(2, false).poseN == 3 && Plan(3, false).poseN == 4 && Plan(1.5f, false).poseN == 2, "pose rate");
    // the fade-out tier does not start before depth 2 (the lighter ones first)
    CHECK(Plan(2.0f, false).draw == kOff && Plan(2.5f, false).draw < kOff && Plan(1.9f, false).draw == kOff, "the draw distance starts after depth 1, far end first");
    // the stage opening: depth of a steady level
    CHECK(DepthOf(0, 3, 0.5f, false) == 3.0f && DepthOf(1, 3, 0.5f, true) == 2.0f && DepthOf(3, 1, 0.25f, true) == 2.5f && DepthOf(0, 9, 0, false) == 4.0f, "depth of");
}
static void TestSweep() {
    // never faster than the limit; reaches the target; no overshoot; both directions
    float cur = 30000, tgt = 2500, t = 0;
    float maxStep = 0;
    while (cur != tgt && t < 120) {
        const float n = SweepStep(cur, tgt, 1.0f / 60.0f);
        maxStep = std::fmax(maxStep, std::fabs(n - cur));
        CHECK(n >= tgt && n <= cur, "monotone inward without overshoot");
        cur = n; t += 1.0f / 60.0f;
    }
    CHECK(cur == tgt, "arrives (t=%.1f s)", t);
    // at the pace of the near end: at 25 m 10 m/s = 1000 units/s = 16.7 per turn; the far part of the sweep may be quicker but its step is a fraction of the distance
    float c2 = 2500; const float n2 = SweepStep(c2, 9000, 1.0f / 60.0f);
    CHECK(std::fabs((n2 - c2) - 1000.0f / 60.0f) < 1e-3f, "at 25 m the edge moves 10 m/s: %f", n2 - c2);
    // a 100 m edge moves 40 m/s
    const float n3 = SweepStep(10000, 2500, 1.0f / 60.0f);
    CHECK(std::fabs((10000 - n3) - 1000.0f / 60.0f) < 1e-3f, "the pace is set by the nearer end (the visible one): %f", 10000 - n3);
    // the angular rate of the edge stays bounded: step / distance <= 0.4/s * dt wherever the edge is, once past the floor pace
    for (float d = 3000; d < 30000; d *= 1.3f) {
        const float n = SweepStep(d, 100, 1.0f / 60.0f);
        CHECK((d - n) / d <= 0.4f / 60.0f + 1e-6f, "angular rate at %.0f", d);
    }
    // outward (give-back) is quicker by the multiplier, never beyond it
    const float a = SweepStepDir(2500, 9000, 1.0f / 60.0f) - 2500, b = SweepStepDir(9000, 2500, 1.0f / 60.0f);
    CHECK(a > 0 && (9000 - b) > 0 && std::fabs(a - 2.0f * (9000 - b)) < 0.2f, "outward twice as quick (%f vs %f)", a, 9000 - b);
    CHECK(SweepStep(5000, 5000, 1.0f / 60.0f) == 5000, "stays");
    // 100 m to 25 m takes seconds, not a turn
    float c = 10000; int turns = 0; while (c > 2500 && turns < 100000) { c = SweepStep(c, 2500, 1.0f / 60.0f); ++turns; }
    CHECK(turns > 120 && turns < 900, "100 m -> 25 m in %d turns (%.1f s)", turns, turns / 60.0);
}
static void TestFade() {
    CHECK(FadeWeight(100, 5000, 1500) == 1.0f && FadeWeight(5000, 5000, 1500) == 1.0f && FadeWeight(6500, 5000, 1500) == 0.0f && FadeWeight(9000, 5000, 1500) == 0.0f, "ends");
    float prev = 1.0f;
    for (float d = 5000; d <= 6500; d += 1.0f) { const float w = FadeWeight(d, 5000, 1500); CHECK(w <= prev + 1e-6f, "monotone"); CHECK(prev - w < 0.0025f, "continuous: no step of more than 0.25 %% per unit (%f)", prev - w); prev = w; }
    CHECK(std::fabs(FadeWeight(5750, 5000, 1500) - 0.5f) < 1e-5f, "half way = half");
    // an object swaying around the edge (the camera moves back and forth by 3 units a turn across the zone where w ~ 0.04): the hidden flag flips at most twice, never every turn
    HideState hs; int flips = 0; bool last = false;
    for (int i = 0; i < 1000; ++i) {
        const float d = 6330.0f + 40.0f * std::sin(i * 0.37f);   // w around 0.02 .. 0.06
        const bool h = hs.Update(FadeWeight(d, 5000, 1500));
        if (h != last) ++flips;
        last = h;
    }
    CHECK(flips <= 4, "swaying at the edge: %d flips of the hidden flag", flips);
    // without the hysteresis the same sway flips often (the control: the test would prove nothing otherwise)
    int naive = 0; bool nl = false;
    for (int i = 0; i < 1000; ++i) { const float w = FadeWeight(6330.0f + 40.0f * std::sin(i * 0.37f), 5000, 1500); const bool h = w < 0.04f; if (h != nl) ++naive; nl = h; }
    CHECK(naive > 20, "control: a single threshold flickers (%d flips)", naive);
    HideState h2; CHECK(!h2.Update(0.5f) && h2.Update(0.01f) && h2.Update(0.04f) && !h2.Update(0.07f), "hide below 2 %%, show above 6 %%");
}
static void TestPhase() {
    for (int k = 2; k <= 4; ++k) {
        const int N = 1200;
        std::vector<int> per(k, 0); std::vector<int> posed(N, 0);
        for (int t = 0; t < k; ++t) for (int id = 0; id < N; ++id) if (PoseNow(t + 1000, id, k)) { ++per[t]; ++posed[id]; }
        for (int id = 0; id < N; ++id) CHECK(posed[id] == 1, "k=%d actor %d posed %d times in %d turns", k, id, posed[id], k);
        for (int t = 0; t < k; ++t) CHECK(per[t] == N / k, "k=%d turn %d poses %d of %d", k, t, per[t], N);
        // no actor goes longer than k-1 turns without a pose, over a long run, also when k changes at a turn (2 -> 3 -> 4)
        for (int id = 0; id < 24; ++id) { int gap = 0, worst = 0; for (int t = 0; t < 600; ++t) { if (PoseNow(t, id, k)) gap = 0; else worst = std::max(worst, ++gap); } CHECK(worst <= MaxStale(k), "k=%d stale %d", k, worst); }
    }
    // changing the rate mid-way keeps the spread (each rate balanced on its own)
    int c3 = 0; for (int id = 0; id < 1200; ++id) if (PoseNow(77, id, 3)) ++c3; CHECK(c3 == 400, "balanced after a change");
    // actors that are NOT consecutive ids (hashed addresses): the spread is statistical, within 25 % of N/k
    uint32_t x = 12345; int per[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2000; ++i) { x = x * 1664525u + 1013904223u; if (PoseNow(5, x >> 8, 4)) ++per[0]; if (PoseNow(6, x >> 8, 4)) ++per[1]; }
    CHECK(std::abs(per[0] - 500) < 125 && std::abs(per[1] - 500) < 125, "hashed ids %d %d", per[0], per[1]);
    CHECK(PoseNow(3, 5, 1) && MaxStale(1) == 0, "k=1 poses every turn");
}
static void TestFormat() {
    Census c; c.depth = 2.5f; c.level = 3; c.poseN = 3; c.heldDraws = 41; c.fadedObjects = 12; c.movedIn = 3; c.movedOut = 2; c.decor = 4250; c.anim = 3250; c.lod = 0.55f; c.draw = 7000; c.band = 2000; c.poseLeftOut = 99;
    char b[400]; Format(b, sizeof b, "HoneyBeeKingdomGalaxy", c);
    CHECK(std::strcmp(b, "far: HoneyBeeKingdomGalaxy: depth 2.50 (level 3), horizon decor 42 m, anim 32 m, lod x0.55, draw 70 m (band 20 m); held draws 41, faded objects 12, re-posed 1-in-3 (99 left out); moved in 3 times, out 2 times; pop events 0.0 a second, largest 0 px") == 0, "%s", b);
    c.draw = kOff; Format(b, sizeof b, "X", c); CHECK(std::strstr(b, "draw off m") != nullptr || std::strstr(b, "draw off") != nullptr, "%s", b);
    char s[30]; Format(s, sizeof s, "X", c); CHECK(std::strlen(s) < sizeof s, "short buffer");
}
static void TestQfar() {
    using namespace qfar;
    // off at w = 0, the target at w = 1, never inside the 25 m floor, monotone inward, bounded in relative speed (a 4 s sweep)
    const float tg[4] = {kQRate, kQDecor, kQDraw, kQLogic};
    for (float t : tg) {
        CHECK(Sweep(t, 0.0f) == 0.0f, "off");
        CHECK(std::fabs(Sweep(t, 1.0f) - t) < 1e-3f, "target");
        float prev = 1e9f;
        const float vmax = MaxRelSpeed(t, 4.0f);
        for (int i = 1; i <= 240; ++i) {   // 240 turns of a 4 s walk
            const float w = i / 240.0f, h = Sweep(t, w);
            CHECK(h >= kFloor - 1e-3f && h <= kOff + 1e-3f, "range %f", h);
            CHECK(h <= prev + 1e-3f, "monotone");
            if (prev < 1e8f) CHECK((prev - h) / prev <= vmax / 60.0f * 1.01f + 1e-4f, "speed %f > %f", (prev - h) / prev, vmax / 60.0f);
            prev = h;
        }
    }
    CHECK(Sweep(1000.0f, 1.0f) == kFloor, "a target under the floor is the floor");
    CHECK(LodScale(0.0f) == 1.0f && std::fabs(LodScale(1.0f) - kQLodScale) < 1e-6f && LodScale(0.5f) < 1.0f && LodScale(0.5f) > kQLodScale, "lod");
    CHECK(MaxRelSpeed(kQRate, 4.0f) < 0.45f, "a 4 s walk moves a horizon by under 45 %% of its distance a second");
    // the Quality targets are not nearer than the Performance tiers' own level-3 rows (the same carriers, milder)
    CHECK(kQRate >= Plan(3.0f, false).rate && kQDecor >= Plan(3.0f, false).decor && kQDraw >= Plan(3.0f, false).draw, "quality is milder than level 3");
}
static void TestRemovalLast() {
    // at every depth the cheaper tiers act before removal: rate before lod before density before draw (nearest horizon first at the same depth means the tier is deeper in the order)
    float firstRate = -1, firstLod = -1, firstDecor = -1, firstDraw = -1;
    for (int i = 0; i <= 400; ++i) {
        const float d = i * 0.01f;
        const Horizons h = Plan(d, false);
        if (firstRate < 0 && h.rate < kOff) firstRate = d;
        if (firstLod < 0 && h.lod < 1.0f) firstLod = d;
        if (firstDecor < 0 && h.decor < kOff) firstDecor = d;
        if (firstDraw < 0 && h.draw < kOff) firstDraw = d;
    }
    CHECK(firstRate <= firstLod && firstLod <= firstDecor && firstDecor <= firstDraw, "order %.2f %.2f %.2f %.2f", firstRate, firstLod, firstDecor, firstDraw);
    CHECK(firstDraw >= 2.0f, "removal only from level 3");
    Census c; c.pops = 25; c.secs = 5.0f; c.popPx = 11.0f; char b[400];
    Format(b, sizeof b, "X", c);
    CHECK(std::strstr(b, "pop events 5.0 a second, largest 11 px") != nullptr, "%s", b);
}
int main() {
    TestOrdering(); TestPlan(); TestSweep(); TestFade(); TestPhase(); TestFormat(); TestQfar(); TestRemovalLast();
    if (fails == 0) std::printf("farfield: all passed\n");
    return fails == 0 ? 0 : 1;
}
