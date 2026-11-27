// detail25: host test of platform/include/port/detail.h (the governor, the plant thinning, the fur layers, the apply rules).
#include <cstdio>
#include <vector>
#include <cmath>
#include "port/detail.h"
using namespace port::detail;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// a synthetic game: load(level) = base - save*level (+ noise), in ms
struct Sim { double base, save; double Load(int l, double noise) const { return base - save * l + noise; } };

static int RunGov(const Sim& s, double secs, int startLevel, int* changes, int* maxStep, bool freezeMid, int* changesInFreeze, double noiseAmp = 0.0) {
    Governor g; g.level = startLevel;
    int ch = 0, prev = g.level; double t = 0; unsigned rng = 12345;
    int inFreeze = 0; int lastChangeT = -100000;
    *maxStep = 0;
    for (; t < secs * 1000.0; t += 500.0) {
        rng = rng * 1664525u + 1013904223u;
        const double noise = noiseAmp * (((rng >> 16) & 0xFFFF) / 32768.0 - 1.0);
        const bool freeze = freezeMid && t > 20000 && t < 40000;
        const double game = s.Load(g.level, noise), fe = game * 0.9;
        const int nl = g.Update(t, game, fe, freeze);
        if (nl != prev) {
            ++ch; if (freeze) ++inFreeze;
            const int step = nl > prev ? nl - prev : prev - nl;
            if (step > *maxStep) *maxStep = step;
            CHECK(t - lastChangeT >= 1000, "two changes within a second (%d ms apart)", (int)(t - lastChangeT));
            lastChangeT = (int)t; prev = nl;
        }
    }
    *changes = ch; *changesInFreeze = inFreeze;
    return g.level;
}

int main() {
    // 1. settles: a heavy scene (19 ms at level 1, saves 1.2 ms a level) must go up until under 15.5, then stay.
    { int ch, ms, fz; Sim s{20.2, 1.2};  // level1 = 19.0
      int lv = RunGov(s, 120, 1, &ch, &ms, false, &fz);
      CHECK(s.Load(lv, 0) <= 15.5, "settled load %.2f at level %d", s.Load(lv, 0), lv);
      CHECK(ms == 1, "max step %d", ms);
      CHECK(ch <= 5, "changes %d (should be a climb of 3, no oscillation)", ch);
      std::printf("heavy: settled level %d load %.2f changes %d\n", lv, s.Load(lv, 0), ch); }
    // 2. light scene: relaxes to level 0 (wider) after holding, and stays.
    { int ch, ms, fz; Sim s{11.0, 0.5};
      int lv = RunGov(s, 120, 1, &ch, &ms, false, &fz);
      CHECK(lv == 0, "light scene level %d", lv); CHECK(ch == 1, "changes %d", ch);
      std::printf("light: level %d changes %d\n", lv, ch); }
    // 3. recovers: heavy then light.
    { Governor g; g.level = 1; double t = 0; int peak = 0;
      for (; t < 60000; t += 500) { g.Update(t, 19.0 - 1.2 * (g.level - 1), 14.0, false); if (g.level > peak) peak = g.level; }
      for (; t < 180000; t += 500) g.Update(t, 10.0, 9.0, false);
      CHECK(peak >= 3, "peak %d", peak); CHECK(g.level <= 1, "recovered to %d", g.level);
      std::printf("recover: peak %d, end level %d\n", peak, g.level); }
    // 4. exactly on a threshold (load oscillating around hi/lo): no flicker (changes bounded).
    { int ch, ms, fz; Sim s{15.5 + 1.2, 1.2};  // level1 = 15.5+0.0: exactly hi at level 1, under hi at level 2
      int lv = RunGov(s, 600, 1, &ch, &ms, false, &fz, 0.4);
      std::printf("on-threshold (noise +-0.4): level %d changes in 600 s: %d\n", lv, ch);
      CHECK(ch <= 12, "flicker: %d changes in 10 minutes", ch); }
    { int ch, ms, fz; Sim s{13.0 + 0.6, 0.6};  // sits exactly at lo at level 1
      int lv = RunGov(s, 600, 1, &ch, &ms, false, &fz, 0.3);
      std::printf("on-lo (noise +-0.3): level %d changes in 600 s: %d\n", lv, ch);
      CHECK(ch <= 12, "flicker at lo: %d", ch); }
    // 5. never moves in a freeze (flight): heavy load, freeze from 20 s to 40 s.
    { int ch, ms, fz; Sim s{24.0, 1.0};
      RunGov(s, 60, 1, &ch, &ms, true, &fz);
      CHECK(fz == 0, "%d changes during the freeze", fz);
      std::printf("flight freeze: changes inside the flight %d (total %d)\n", fz, ch); }
    // 6. thinning: stable, hysteresis, idempotent, brute force.
    { int bad = 0;
      for (int half = 2900; half <= 4800; half += 300) for (int quarter = half + 400; quarter <= half + 1400; quarter += 500)
        for (int st = 0; st < 3; ++st) for (float d = 0; d < 8000; d += 37) {
          const int a = ThinNext(st, d, (float)half, (float)quarter, 8.0f);
          const int b = ThinNext(a, d, (float)half, (float)quarter, 8.0f);
          if (a != b) ++bad;
        }
      CHECK(bad == 0, "ThinNext not idempotent %d times", bad);
      // walking out and back: states change at the right distances, back with hysteresis
      int st = 0; float outHalf = 0, outQuarter = 0, backHalf = 0, backQuarter = 0;
      for (float d = 3000; d < 6000; d += 5) { st = ThinNext(st, d, 4000, 5000, 8.0f); if (st == 1 && outHalf == 0) outHalf = d; if (st == 2 && outQuarter == 0) outQuarter = d; }
      for (float d = 6000; d > 3000; d -= 5) { st = ThinNext(st, d, 4000, 5000, 8.0f); if (st == 1 && backQuarter == 0) backQuarter = d; if (st == 0 && backHalf == 0) backHalf = d; }
      CHECK(outHalf > 4000 && outHalf < 4010 && outQuarter > 5000 && outQuarter < 5010, "out %.0f %.0f", outHalf, outQuarter);
      CHECK(backHalf < 3700 && backHalf > 3600 && backQuarter < 4700 && backQuarter >= 4599, "back %.0f %.0f", backHalf, backQuarter);
      // a player jittering across the threshold does not flicker
      int flips = 0; st = 0; int prev = 0;
      for (int i = 0; i < 2000; ++i) { const float d = 4000 + 120.0f * std::sin(i * 0.37f); st = ThinNext(st, d, 4000, 5000, 8.0f); if (st != prev) { ++flips; prev = st; } }
      CHECK(flips <= 1, "jitter flips %d", flips);
      // stable choice: the same members are kept for a state, member 0 always, 1/2 and 1/4 of the group, quarter subset of half
      for (int n = 1; n < 200; ++n) {
        CHECK(KeepMember(0, 1) && KeepMember(0, 2), "centre");
        const int k1 = ThinCountKept(n, 1), k2 = ThinCountKept(n, 2);
        CHECK(k1 == (n + 1) / 2 && k2 == (n + 3) / 4, "n %d kept %d %d", n, k1, k2);
        for (int i = 0; i < n; ++i) CHECK(!KeepMember(i, 2) || KeepMember(i, 1), "quarter not subset");
      }
      std::printf("thinning: out at %.0f/%.0f, back at %.0f/%.0f, jitter flips %d\n", outHalf, outQuarter, backHalf, backQuarter, flips); }
    // 7. apply rules: bounded per turn; unseen at once; fallback; restore waits longer.
    { int budget = 3, applied = 0;
      for (int i = 0; i < 100; ++i) applied += MayApply(0, 1, false, false, 1000, 120, budget) ? 1 : 0;
      CHECK(applied == 3, "on-screen applied %d (budget 3)", applied);
      budget = 3; applied = 0;
      for (int i = 0; i < 100; ++i) applied += MayApply(0, 1, true, false, 0, 120, budget) ? 1 : 0;
      CHECK(applied == 100, "unseen applied %d", applied);
      budget = 3;
      CHECK(!MayApply(0, 1, false, false, 100, 120, budget), "before fallback");
      CHECK(MayApply(0, 1, false, false, 120, 120, budget), "at fallback");
      budget = 3;
      CHECK(!MayApply(1, 0, false, false, 200, 120, budget) && MayApply(1, 0, false, false, 300, 120, budget), "restore waits 2.5x");
      budget = 16; CHECK(MayApply(1, 0, false, true, 0, 120, budget), "cut restores");
      // a whole group of 200 members whose target changes: at most 3 a turn on screen -> 67 turns, never a whole group at once
      int turns = 0, left = 200; std::vector<int> st(200, 0);
      while (left > 0 && turns < 1000) { int b = 3, mx = 0; for (int i = 0; i < 200; ++i) if (st[i] == 0 && MayApply(0, 1, false, false, 999, 120, b)) { st[i] = 1; --left; ++mx; } CHECK(mx <= 3, "turn changed %d", mx); ++turns; }
      std::printf("group of 200 on screen: %d turns, <= 3 members a turn\n", turns); }
    // 8. fur: layers, tip always, gain, fade weights, silhouette (tip) kept.
    { for (int n = 3; n <= 16; ++n) for (int st = 0; st < 3; ++st) { CHECK(FurDrawLayer(st, n - 1, n, 2), "tip"); CHECK(FurKept(st, n, 2) <= n, "kept"); }
      CHECK(FurKept(1, 8, 2) == 5 && FurKept(2, 8, 2) == 2 && FurKept(2, 8, 1) == 1, "kept 8: %d %d %d", FurKept(1, 8, 2), FurKept(2, 8, 2), FurKept(2, 8, 1));
      CHECK(FurGain(0, 8, 2, 0.5f) == 1.0f && FurGain(1, 8, 2, 0.5f) > 1.0f && FurGain(2, 8, 2, 0.5f) <= 1.6f, "gain");
      // fade 0 -> 1 from full to half: no weight jumps more than 1/30 + gain change per step; end weights equal the target
      float prevW[8]; for (int i = 0; i < 8; ++i) prevW[i] = FurWeight(0, 1, 0.0f, i, 8, 2, 0.5f);
      float maxJump = 0;
      for (int k = 1; k <= 30; ++k) for (int i = 0; i < 8; ++i) { const float w = FurWeight(0, 1, k / 30.0f, i, 8, 2, 0.5f); const float j = std::fabs(w - prevW[i]); if (j > maxJump) maxJump = j; prevW[i] = w; }
      CHECK(maxJump < 0.05f, "fade jump %.3f", (double)maxJump);
      for (int i = 0; i < 8; ++i) CHECK(std::fabs(prevW[i] - (FurDrawLayer(1, i, 8, 2) ? FurGain(1, 8, 2, 0.5f) : 0.0f)) < 1e-5f, "end weight");
      std::printf("fur: 8 layers kept %d / %d (far 1: %d), max weight step per fade turn %.3f\n", FurKept(1, 8, 2), FurKept(2, 8, 2), FurKept(2, 8, 1), (double)maxJump); }
    // 9. no level change lengthens a turn: the per-turn work is bounded by the budgets (above) and the slew is bounded
    { float c = 1.0f; int turns = 0; while (c != 0.7f && turns < 100000) { c = Slew(c, 0.7f, kSlewStep); ++turns; }
      CHECK(turns >= 495 && turns <= 505, "slew turns %d", turns); std::printf("slew 1.0 -> 0.7: %d turns (%.1f s)\n", turns, turns / 60.0); }
    // 10. levels and floors
    { for (int l = 0; l < kLevels; ++l) for (int r = 0; r < kRules; ++r) CHECK(Scaled(5000, LevelRow(l).mult[r], r) >= std::fmin(5000.0f, Floor(r)), "floor");
      CHECK(Scaled(5000, 1.0f, 0) == 5000.0f && LevelRow(1).plantHalf == 0.0f, "level 1 is today");
      CHECK(CycleLevel(0, 1000) == 0 && CycleLevel(5000, 1000) == 5 && CycleLevel(6000, 1000) == 4 && CycleLevel(10000, 1000) == 0, "cycle");
      CHECK(std::fabs(DrawnTurnMs(16.6, 1.0, 0.5) - 16.6) < 1e-9 && DrawnTurnMs(12.0, 0.5, 0.5) > 15.0, "drawn estimate"); }
    std::printf(fails ? "FAILED: %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
