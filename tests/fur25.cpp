// reflfur25: host test of platform/include/port/fur25.h (the shell count, its hysteresis and rate, the groups, the alpha merge, the census).
#include <cstdio>
#include <cmath>
#include <vector>
#include "port/fur25.h"
using namespace port::fur25;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// FurDrawer::CLayerParam::calcValue
static float Calc(float tip, float root, float curve, int layer, int n) { return root + std::pow(float(layer + 1) / n, curve) * (tip - root); }
static std::vector<Konst> Profile(int n, float brTip, float brRoot, float brCurve, float alTip, float alRoot, float alCurve, float skTip, float skRoot, float skCurve) {
    std::vector<Konst> v(n);
    for (int i = 0; i < n; ++i) {  // setupLayerMaterial
        v[i].r = Calc(brTip, brRoot, brCurve, i, n);
        v[i].g = Calc(alTip, alRoot, alCurve, n - i - 1, n);
        v[i].b = Calc(skTip, skRoot, skCurve, n - i - 1, n);
        if (v[i].g + v[i].b > 255.0f) v[i].g = 255.0f - v[i].b;  // (the game's constants keep strand alpha under 255)
    }
    return v;
}

int main() {
    const Params p1 = ParamsFor(1);
    // fur31: GXV_PB_FUR for every fur: surface distance, a band wider than a governor step, the base latched against governor steps.
    // The Queen Bee (climbable, coat radius 817.6) is the regression: no shell-count change while the camera orbits at 2200 / 2700 from her origin,
    // and at least as many shells as C183's rule (limit + radius, 8 % band) at every distance of a climb.
    {
        const float radius = 817.6186f;  // FurHoneyQueenBody_v, high/middle authored SHP1
        auto c183 = [&](bool was, float camD) {   // the C183 Queen rule, for comparison
            const float r = SurfaceReach(was ? 2500.0f - 25.0f * 8.0f : 2500.0f, radius, 1, 1, 1);
            return camD * camD > r * r;
        };
        bool legacy = false, general = false, queen = false;
        int legacyChanges = 0, generalChanges = 0;
        for (int i = 0; i < 16; ++i) {
            const float camD = (i & 1) ? 2200.0f : 2700.0f;
            const bool a = camD > 2500.0f;
            const bool b = PbThinAt(general, camD, 2500.0f, 0.0f, radius, 1, 1, 1);
            legacyChanges += a != legacy; generalChanges += b != general; legacy = a; general = b; queen = c183(queen, camD);
        }
        CHECK(legacyChanges == 16 && generalChanges == 0, "camera orbit: old %d vs surface %d", legacyChanges, generalChanges);
        // never fewer shells than C183's Queen rule: whenever the general rule thins, C183 thinned too (both with their own state), over a sweep both ways
        bool g = false, q = false; int fewer = 0;
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i <= 700; ++i) {
                const float d = pass == 0 ? 500.0f + 7.0f * i : 5400.0f - 7.0f * i;
                g = PbThinAt(g, d, 2500.0f, 0.0f, radius, 1, 1, 1); q = c183(q, d);
                if (pass == 0 && g && !q) ++fewer;
            }
        CHECK(fewer == 0, "outbound sweep: %d pictures with fewer shells than C183", fewer);
        CHECK(PbThinAt(false, 3350, 2500, 0, radius, 1, 1, 1) && !PbThinAt(false, 3300, 2500, 0, radius, 1, 1, 1), "thins at limit + radius");
        CHECK(!PbThinAt(true, 3300 - 599, 2500, 0, radius, 1, 1, 1) && PbThinAt(true, 3300 - 500, 2500, 0, radius, 1, 1, 1), "the band is %.0f units, wider than a governor step (500)", kPbMinBand);
        CHECK(PbBand(2500, 0) > 500.0f && PbBand(2000, 0) > 500.0f && PbBand(1500, 0) > 500.0f && PbBand(10000, 12) == 1200.0f, "band floor and a wider global setting");
        CHECK(!PbThinAt(false, 3500, 2500, 0, radius, 1, -2, 0.5f) && PbThinAt(false, 3500, 2500, 0, radius, 0.5f, 1, 1), "uses live absolute scale");
        CHECK(PbThinAt(false, 2600, 2500, 0, 0, 1, 1, 1) && PbThinAt(false, 2600, 2500, 0, -1, 1, 1, 1) && !PbThinAt(false, 2400, 2500, 0, -1, 1, 1, 1), "a missing bound keeps the old origin rule");
        // A coat in view with a governor step: 2500 -> 2000 -> 1500 in 500 steps while the camera sits at 3000 from the origin (surface 2183): never flips while held.
        {
            PbLatch st; bool was = false; int flips = 0; uint32_t pic = 10;
            const float bases[] = {2500, 2500, 2000, 2000, 1500, 1500, 2000, 2500};
            for (float base : bases) {
                for (int k = 0; k < 5; ++k) {
                    const bool t = PbThin(st, pic++, base, false, [&](float b) { return PbThinAt(was, 3000.0f, b, 0.0f, radius, 1, 1, 1); });
                    flips += t != was; was = t;
                }
            }
            CHECK(flips == 0, "governor steps flipped a coat in view %d times", flips);
            // the unlatched rule flips at the same steps (the mutant this protects against)
            bool w2 = false; int f2 = 0;
            for (float base : bases) for (int k = 0; k < 5; ++k) { const bool t = PbThinAt(w2, 3000.0f, base, 0.0f, 0.0f, 1, 1, 1); f2 += t != w2; w2 = t; }
            CHECK(f2 > 0, "sanity: the origin rule flips under the same steps (%d)", f2);
            // held, then released: off for 4 pictures -> takes the new base; or free under the floor
            PbLatch a; a.base = 2500; a.pic = 100; bool wa = false;
            const bool held = PbThin(a, 101, 1500, false, [&](float b) { return PbThinAt(wa, 2200.0f, b, 0.0f, 0.0f, 1, 1, 1); });
            CHECK(!held && a.base == 2500, "in view: held");
            const bool rel = PbThin(a, 110, 1500, false, [&](float b) { return PbThinAt(wa, 2200.0f, b, 0.0f, 0.0f, 1, 1, 1); });
            CHECK(rel && a.base == 1500, "off for a few pictures: takes the new base");
            PbLatch c; c.base = 2500; c.pic = 100;
            const bool fr = PbThin(c, 101, 1500, true, [&](float b) { return PbThinAt(false, 2200.0f, b, 0.0f, 0.0f, 1, 1, 1); });
            CHECK(fr && c.base == 1500, "under the floor: takes the new base");
            // a distance crossing still thins and restores (the latch holds only the governor's change)
            PbLatch d; bool wd = false, ta = false, tb = false;
            ta = PbThin(d, 1, 2500, false, [&](float b) { return PbThinAt(wd, 3400.0f, b, 0.0f, radius, 1, 1, 1); }); wd = ta;
            tb = PbThin(d, 2, 2500, false, [&](float b) { return PbThinAt(wd, 2800.0f, b, 0.0f, radius, 1, 1, 1); });
            CHECK(ta && tb, "inside the band it stays thin") ;
            wd = tb; const bool tc = PbThin(d, 3, 2500, false, [&](float b) { return PbThinAt(wd, 2500.0f, b, 0.0f, radius, 1, 1, 1); });
            CHECK(!tc, "and comes back past the band");
        }
        std::printf("fur31 Queen fur orbit: %d legacy transitions, %d surface transitions\n", legacyChanges, generalChanges);
    }
    // ---- the projection and the supported shells
    {
        const float a = PixelsPerUnit(544, 45.0f, 1000.0f), b = PixelsPerUnit(544, 45.0f, 2000.0f);
        CHECK(std::fabs(a / b - 2.0f) < 1e-4f, "px per unit halves with the distance");
        CHECK(a > 0.6f && a < 0.7f, "45 deg, 544 lines, 1000 units: %.3f px/unit", a);
        CHECK(NearestDist(100, 400) == 1.0f && NearestDist(900, 400) == 500.0f, "nearest point");
        CHECK(Supported(0.0f, 0.75f) == 1.0f, "a flat fur supports one shell");
    }
    // ---- the raw count is monotone in the thickness and bounded [2, n]
    for (int n = 3; n <= 16; ++n) {
        int prev = 0;
        for (float s = 0.0f; s < 40.0f; s += 0.05f) {
            const int k = RawKeep(n, s);
            CHECK(k >= 2 && k <= n && k >= prev, "monotone raw keep n=%d s=%.2f k=%d prev=%d", n, s, k, prev);
            prev = k;
        }
        CHECK(RawKeep(n, 1000.0f) == n && RawKeep(n, 0.0f) == 2, "ends");
    }
    CHECK(RawKeep(2, 0.0f) == 2 && RawKeep(1, 0.0f) == 1, "n <= 2 is never thinned");
    // ---- the count falls smoothly (monotonically) with distance once settled, one shell at a time, and full inside FULL
    for (int level = 1; level <= 2; ++level) {
        const Params p = ParamsFor(level);
        for (int n = 4; n <= 12; n += 4) {
            for (float tip = 4.0f; tip <= 40.0f; tip += 12.0f) {
                State st; uint32_t pic = 1;
                int prevK = n + 1, maxStep = 0, changes = 0, lastChangePic = 0, minGapPics = 1 << 30;
                for (float d = 300.0f; d <= 20000.0f; d *= 1.0015f) {  // a camera backing away
                    const float thick = tip * PixelsPerUnit(544, 45.0f, d);
                    const int full = FullRule(d, 1e9f, p);
                    const int k = Step(st, pic, n, full, Supported(thick, p.gapPx), p);
                    if (prevK <= n) {
                        CHECK(k <= prevK, "non-increasing with distance n=%d d=%.0f k=%d prev=%d", n, d, k, prevK);
                        const int dk = prevK - k;
                        if (dk > maxStep) maxStep = dk;
                        if (dk) { ++changes; if ((int)pic - lastChangePic < minGapPics && lastChangePic) minGapPics = (int)pic - lastChangePic; lastChangePic = (int)pic; }
                    }
                    if (d < p.fullDist) CHECK(k == n || pic < 2, "full inside FULL (n=%d d=%.0f k=%d)", n, d, k);
                    prevK = k; ++pic;
                }
                CHECK(maxStep <= 1, "one shell at a time (n=%d tip=%.0f): %d", n, tip, maxStep);
                CHECK(minGapPics >= p.stepPics, "changes at least stepPics apart (n=%d tip=%.0f): %d < %d", n, tip, minGapPics, p.stepPics);
                CHECK(prevK == 2 || prevK >= 2, "end");
            }
        }
    }
    // ---- hysteresis: s wobbling by +-(hyst - eps) around any boundary never changes the count; +-(hyst + 0.3) does (at most 1 per step)
    {
        const int n = 10;
        for (int k0 = 3; k0 <= 9; ++k0) {
            State st; uint32_t pic = 1;
            // settle at s = k0 + 0.5 (inside the band of k0)
            for (int i = 0; i < 4; ++i) Step(st, pic++, n, 0, k0 + 0.5f, p1);
            CHECK(st.keep == k0, "settled at %d (got %d)", k0, st.keep);
            int changes = 0;
            for (int i = 0; i < 2000; ++i) {
                const float wob = (i & 1) ? p1.hyst - 0.02f : -(p1.hyst - 0.02f);
                const int before = st.keep;
                const float s = (i & 2) ? (float)k0 + wob : (float)k0 + 1.0f + wob;  // wobble around the lower and the upper edge of the band
                Step(st, pic++, n, 0, s, p1);
                changes += st.keep != before;
            }
            CHECK(changes == 0, "no change inside the hysteresis band (k0=%d): %d", k0, changes);
            // outside the band: it moves, one per stepPics
            int moves = 0; const int t0 = (int)pic;
            for (int i = 0; i < 400; ++i) { const int b = st.keep; Step(st, pic++, n, 0, (float)k0 - p1.hyst - 0.5f, p1); moves += st.keep != b; }
            CHECK(moves >= 1 && moves <= 400 / p1.stepPics + 1 && (int)pic - t0 == 400, "moves outside the band %d", moves);
        }
    }
    // ---- unseen / hard rules
    {
        State st; uint32_t pic = 100;
        CHECK(Step(st, pic, 8, 0, 3.4f, p1) == 3, "first draw: the target at once");
        for (int i = 0; i < 3; ++i) Step(st, ++pic, 8, 0, 3.4f, p1);
        pic += 50;  // not drawn for 50 pictures
        CHECK(Step(st, pic, 8, 0, 6.9f, p1) == 6, "unseen: snaps to the target (6) at once");
        CHECK(Step(st, ++pic, 8, 2, 1.0f, p1) == 8, "HARD full (the player near): all shells at once");
        CHECK(Step(st, ++pic, 8, 1, 1.0f, p1) == 8, "soft full keeps what it is");
        State s2; Step(s2, 1, 8, 0, 3.0f, p1);
        int k = Step(s2, 2, 8, 1, 1.0f, p1);
        CHECK(k == 3, "soft full steps up, not at once");
        for (uint32_t q = 3; q < 3u + 6u * (uint32_t)p1.stepPics + 6u; ++q) k = Step(s2, q, 8, 1, 1.0f, p1);
        CHECK(k == 8, "soft full reaches n: %d", k);
    }
    // ---- the shells kept: nested sets, one shell per step, the tip kept, evenly spread
    for (int n = 3; n <= 24; ++n) {
        uint8_t prev[kMaxShells];
        KeepFlags(n, n, prev);
        for (int i = 0; i < n; ++i) CHECK(prev[i], "k == n keeps all");
        for (int k = n - 1; k >= 2; --k) {
            uint8_t cur[kMaxShells];
            KeepFlags(n, k, cur);
            int kept = 0, dropped = 0, run = 0, maxRun = 0;
            for (int i = 0; i < n; ++i) {
                kept += cur[i];
                CHECK(!cur[i] || prev[i], "nested: k=%d keeps no shell the larger set dropped (n=%d i=%d)", k, n, i);
                dropped += prev[i] && !cur[i];
                run = cur[i] ? 1 : run + 1;  // the group's size so far
                if (run > maxRun) maxRun = run;
            }
            CHECK(kept == k && dropped == 1 && cur[n - 1], "n=%d k=%d: kept %d dropped %d tip %d", n, k, kept, dropped, cur[n - 1]);
            // groups: runs of dropped shells under a kept one; their sizes within 1 of the even size once the set is thinner than half
            int covered = 0;
            for (int i = 0; i < n; ++i) if (cur[i]) { const int lo = GroupLo(cur, i); CHECK(lo >= covered && lo <= i, "group"); covered = i + 1; }
            CHECK(covered == n, "groups cover all shells (n=%d k=%d)", n, k);
            CHECK(maxRun <= 2 * ((n + k - 1) / k), "evenly spread: the largest group %d of n=%d k=%d", maxRun, n, k);
            for (int i = 0; i < n; ++i) prev[i] = cur[i];
        }
    }
    // ---- the merge: density (sum of optical depths) kept, colour to first order, no new opacity beyond the clamp
    struct Prof { const char* name; std::vector<Konst> v; };
    std::vector<Prof> profs;
    profs.push_back({"game default (6)", Profile(6, 0.0f, 0.0f, 0.6f, 255.0f, 0.0f, 1.0f, 80.0f, 0.0f, 5.42f)});
    profs.push_back({"10 layers soft", Profile(10, 60.0f, 20.0f, 1.0f, 200.0f, 0.0f, 1.4f, 120.0f, 0.0f, 3.0f)});
    profs.push_back({"8 layers dense", Profile(8, 90.0f, 10.0f, 0.8f, 230.0f, 40.0f, 0.7f, 150.0f, 10.0f, 2.0f)});
    profs.push_back({"8 layers graded brightness, translucent", Profile(8, 255.0f, 0.0f, 1.0f, 110.0f, 0.0f, 1.0f, 25.0f, 0.0f, 1.0f)});
    profs.push_back({"12 layers thin", Profile(12, 30.0f, 0.0f, 1.0f, 120.0f, 0.0f, 1.0f, 40.0f, 0.0f, 4.0f)});
    double worstDepth = 0, worstCol = 0;
    for (const Prof& pr : profs) {
        const int n = (int)pr.v.size();
        double depthAll = 0, depthAllK = 0;
        for (int i = 0; i < n; ++i) { depthAll += Depth(Opacity(pr.v[i].g + pr.v[i].b)); depthAllK += Depth(Opacity(pr.v[i].b)); }
        for (int k = 2; k <= n; ++k) {
            double dS = 0, dK = 0; std::vector<Konst> kept;
            uint8_t kf[kMaxShells]; KeepFlags(n, k, kf);
            for (int hi = 0; hi < n; ++hi) {
                if (!kf[hi]) continue;
                const int lo = GroupLo(kf, hi);
                const Konst m = Merge(pr.v.data(), lo, hi);
                CHECK(m.r >= 0 && m.r <= 255 && m.g >= 0 && m.g <= 255 && m.b >= 0 && m.b <= 255, "range");
                CHECK(m.g + m.b <= 255.001f, "strand total within 255 (%s k=%d)", pr.name, k);
                dS += Depth(Opacity(m.g + m.b)); dK += Depth(Opacity(m.b));
                kept.push_back(m);
            }
            // (the sum of optical depths, for a pelt with no (nearly) opaque shell; and always the transmittance, which is the same statement without the log)
            double tAll = 1.0, tKept = 1.0, maxA = 0.0;
            for (int i = 0; i < n; ++i) { tAll *= 1.0 - Opacity(pr.v[i].g + pr.v[i].b); maxA = std::fmax(maxA, Opacity(pr.v[i].g + pr.v[i].b)); }
            for (const Konst& m : kept) tKept *= 1.0 - Opacity(m.g + m.b);
            CHECK(std::fabs(tAll - tKept) < 1e-5, "strand transmittance kept: %s k=%d %.6f vs %.6f", pr.name, k, tAll, tKept);
            const double eS = maxA < 0.98 ? std::fabs(dS - depthAll) / depthAll : 0.0, eK = depthAllK > 1e-9 && depthAllK < 8.0 ? std::fabs(dK - depthAllK) / depthAllK : 0.0;
            if (eS > worstDepth) worstDepth = eS;
            if (eK > worstDepth) worstDepth = eK;
            CHECK(eS < 0.001 && eK < 0.001, "density kept: %s k=%d strand %.5f skin %.5f", pr.name, k, eS, eK);
            // the composite along a strand ray over a background of 0.3: src-over root to tip, shell colour = brightness / 255 (+ 0.2)
            auto comp = [&](const std::vector<Konst>& L) { double c = 0.3; for (const Konst& x : L) { const double a = Opacity(x.g + x.b); c = c * (1.0 - a) + (0.2 + x.r / 255.0 * 0.8) * a; } return c; };
            const double cFull = comp(pr.v), cK = comp(kept);
            const double ec = std::fabs(cFull - cK);
            if (ec > worstCol) worstCol = ec;
            if (k >= (n + 1) / 2) CHECK(ec < 0.05, "colour within 0.05 of the full pelt for k>=n/2: %s k=%d err %.4f", pr.name, k, ec);
        }
        // k == n is the identity
        for (int i = 0; i < n; ++i) { const Konst m = Merge(pr.v.data(), i, i); CHECK(m.r == pr.v[i].r && m.g == pr.v[i].g && m.b == pr.v[i].b, "identity"); }
    }
    // pathological: fully opaque shells never exceed 255 and never go negative
    {
        Konst L[3] = {{10, 200, 55}, {20, 255, 0}, {30, 0, 255}};
        const Konst m = Merge(L, 0, 2);
        CHECK(m.g >= 0 && m.b <= 255 && m.g + m.b <= 255.001f, "opaque group: %.1f %.1f %.1f", m.r, m.g, m.b);
    }
    // ---- census
    {
        Census c; c.pictures = 300;
        for (int i = 0; i < 300; ++i) c.Add(i % 2 ? 1200.0f : 5000.0f, i % 3 ? 0.5f : 9.0f, 6, 4, 2);
        char b[900]; const int w = Format(b, sizeof b, c, 1, 5.0);
        CHECK(w > 0 && w < 899 && std::strstr(b, "6.00 shells drawn of") == nullptr, "format: %s", b);
        CHECK(c.objects == 300 && c.possible == 1800 && c.drawn == 1200 && c.savedBy25 == 600, "sums");
        CHECK(DistBand(1499) == 0 && DistBand(1500) == 1 && DistBand(8000) == 4 && PxBand(0.9f) == 0 && PxBand(9.0f) == 4, "bands");
    }
    std::printf("fur25: worst density error %.6f, worst colour error %.5f: %s\n", worstDepth, worstCol, fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
