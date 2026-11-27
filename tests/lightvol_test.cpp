// GXV_LIGHTVOL (lightvol25): the pure logic of platform/include/port/lightvol.h. No GX, no GL, no game data.
//  1 ProjectBox: no false cull (random boxes and views: a sampled point of the box that lies on the screen means "not culled"), the unclipped rectangle holds every
//    sample, a box behind the eye is culled, a box across the eye plane is "not known" with the whole screen as its footprint. NEG: a centre-only cull is caught.
//  2 Group: pairwise disjoint inside a group, every active item in exactly one, deterministic; NEG: a grouping that ignores overlap is caught.
//  3 Plan: identity with every cut off; K respected; the guard; fade monotone in size and in distance and continuous; selection stability under jitter (hysteresis + slew
//    against the naive choice: flips counted, the largest step of f per picture); convergence of the slew.
#include "port/lightvol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace port::lightvol;
static unsigned failures = 0, checks = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
static uint64_t s = 88172645463325252ull;
static float Rnd() {  // xorshift, [0,1)
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return static_cast<float>((s >> 11) & 0xFFFFFF) / 16777216.0f;
}
static float Rng(float a, float b) { return a + (b - a) * Rnd(); }

// a random rotation * translation view (3x4), looking down -z, camera at the origin
static void RandView(float mv[3][4]) {
    const float a = Rng(0, 6.28f), b = Rng(-0.6f, 0.6f), c = Rng(-0.3f, 0.3f);
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    // R = Rz(c) Rx(b) Ry(a)
    const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
    const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
    const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
    float t[3][3], r[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { t[i][j] = 0; for (int k = 0; k < 3; ++k) t[i][j] += rx[i][k] * ry[k][j]; }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { r[i][j] = 0; for (int k = 0; k < 3; ++k) r[i][j] += rz[i][k] * t[k][j]; }
    const float reach = Rnd() < 0.5f ? 600.0f : 5000.0f;  // (half of the views have the box near the axis, half far to the side or behind)
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) mv[i][j] = r[i][j]; mv[i][3] = i == 2 ? Rng(-reach * 3.0f, reach * 0.4f) : Rng(-reach, reach); }
}
static bool ProjPoint(const float mv[3][4], const Proj& pr, const float p[3], float& nx, float& ny) {
    const float vx = mv[0][0] * p[0] + mv[0][1] * p[1] + mv[0][2] * p[2] + mv[0][3];
    const float vy = mv[1][0] * p[0] + mv[1][1] * p[1] + mv[1][2] * p[2] + mv[1][3];
    const float vz = mv[2][0] * p[0] + mv[2][1] * p[1] + mv[2][2] * p[2] + mv[2][3];
    const float w = -vz;
    if (!(w > 1e-3f)) return false;
    nx = (pr.p00 * vx + pr.p02 * vz) / w;
    ny = (pr.p11 * vy + pr.p12 * vz) / w;
    return true;
}

static void TestProject() {
    Proj pr;
    pr.p00 = 1.0f / (std::tan(0.5f * 0.7854f) * (16.0f / 9.0f));
    pr.p11 = 1.0f / std::tan(0.5f * 0.7854f);
    unsigned culled = 0, kept = 0, falseCull = 0, outsideSample = 0, negFalse = 0, knownN = 0, notKnown = 0, behindAll = 0, areaBad = 0;
    for (int it = 0; it < 4000; ++it) {
        float mv[3][4];
        RandView(mv);
        float bmin[3], bmax[3];
        for (int k = 0; k < 3; ++k) { bmin[k] = Rng(-400, 200); bmax[k] = bmin[k] + Rng(20, 900) * (k == 1 ? 3.0f : 1.0f); }
        const BoxView bv = ProjectBox(mv, bmin, bmax, pr, 0.1f, 0.02f);
        // samples of the (unpadded) box
        bool anyOn = false, allBehind = true;
        float sx0 = 1e30f, sx1 = -1e30f, sy0 = 1e30f, sy1 = -1e30f;
        bool anyBehind = false;
        for (int q = 0; q < 400; ++q) {
            const float p[3] = {Rng(bmin[0], bmax[0]), Rng(bmin[1], bmax[1]), Rng(bmin[2], bmax[2])};
            float nx, ny;
            if (!ProjPoint(mv, pr, p, nx, ny)) { anyBehind = true; continue; }
            allBehind = false;
            if (nx >= -1 && nx <= 1 && ny >= -1 && ny <= 1) anyOn = true;
            sx0 = nx < sx0 ? nx : sx0; sx1 = nx > sx1 ? nx : sx1; sy0 = ny < sy0 ? ny : sy0; sy1 = ny > sy1 ? ny : sy1;
        }
        if (bv.culled) { ++culled; if (anyOn) ++falseCull; } else ++kept;
        if (bv.known) {
            ++knownN;
            if (!(bv.unclipped.x0 <= sx0 && bv.unclipped.x1 >= sx1 && bv.unclipped.y0 <= sy0 && bv.unclipped.y1 >= sy1)) ++areaBad;
            if (!(bv.area >= 0.0f && bv.area <= 1.0f)) ++areaBad;
        } else if (!bv.culled) {
            ++notKnown;
            if (!(bv.foot.x0 == -1 && bv.foot.x1 == 1 && bv.foot.y0 == -1 && bv.foot.y1 == 1)) ++areaBad;
        }
        if (bv.culled && !bv.known && allBehind) ++behindAll;
        if (anyBehind && !allBehind && bv.culled) ++falseCull;  // across the eye plane: must never be culled
        // NEG: a cull decided by the box's centre alone
        float c[3] = {0.5f * (bmin[0] + bmax[0]), 0.5f * (bmin[1] + bmax[1]), 0.5f * (bmin[2] + bmax[2])};
        float nx, ny;
        const bool centreOff = !ProjPoint(mv, pr, c, nx, ny) || nx < -1 || nx > 1 || ny < -1 || ny > 1;
        if (centreOff && anyOn) ++negFalse;
        outsideSample += !anyOn;
    }
    std::printf("project: %u culled, %u kept, %u known, %u across the eye, %u wholly behind; samples never on screen %u\n", culled, kept, knownN, notKnown, behindAll, outsideSample);
    Check(culled > 200 && kept > 200, "project: the random views exercise both culled and kept boxes");
    Check(falseCull == 0, "project: no box with a sampled point on the screen (or across the eye plane) is ever culled");
    Check(areaBad == 0, "project: the unclipped rectangle holds every sampled point, areas in [0,1], a box across the eye plane has the whole screen as its footprint");
    Check(behindAll > 20, "project: boxes wholly behind the eye are culled");
    Check(negFalse > 50, "NEG: culling by the centre alone would hide visible volumes (the test sees it)");
}

static void TestGroup() {
    unsigned bad = 0, missing = 0, multi = 0, groupsTotal = 0, negOverlap = 0, trials = 0;
    for (int it = 0; it < 3000; ++it) {
        const int n = 1 + static_cast<int>(Rnd() * 30);
        Rect r[kMax];
        bool act[kMax];
        int g[kMax], g2[kMax];
        for (int i = 0; i < n; ++i) {
            const float x = Rng(-1, 0.9f), y = Rng(-1, 0.9f);
            r[i] = Rect{x, y, x + Rng(0.02f, 0.5f), y + Rng(0.02f, 0.5f)};
            act[i] = Rnd() > 0.1f;
        }
        const int ng = Group(r, act, n, g);
        const int ng2 = Group(r, act, n, g2);
        groupsTotal += ng;
        ++trials;
        if (ng != ng2 || std::memcmp(g, g2, sizeof(int) * n) != 0) ++bad;
        for (int i = 0; i < n; ++i) {
            if (act[i] != (g[i] >= 0)) ++missing;
            if (g[i] >= ng) ++multi;
            for (int j = 0; j < i; ++j)
                if (act[i] && act[j] && g[i] == g[j] && !Disjoint(r[i], r[j])) ++bad;
        }
        // NEG: one group for everything overlaps
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < i; ++j)
                if (act[i] && act[j] && !Disjoint(r[i], r[j])) { ++negOverlap; i = n; break; }
    }
    std::printf("group: %u trials, %.2f groups per trial\n", trials, static_cast<double>(groupsTotal) / trials);
    Check(bad == 0, "group: every pair inside a group is disjoint; the grouping is deterministic");
    Check(missing == 0 && multi == 0, "group: every active item is in one group, inactive ones in none");
    Check(negOverlap > 500, "NEG: a single group for everything would overlap in most trials (the test sees it)");
}

static void TestPlan() {
    Params off;
    Item it[kMax];
    Out out[kMax];
    for (int i = 0; i < 20; ++i) { it[i].dist = Rng(100, 9000); it[i].area = Rng(0.0001f, 0.2f); }
    Plan(off, it, 20, out);
    bool allOne = true;
    for (int i = 0; i < 20; ++i) allOne = allOne && out[i].f == 1.0f && out[i].target == 1.0f;
    Check(allOne, "plan: identity with the cap, the size cut and the guard all off: f == 1 for every volume");

    // monotone in size and in key, continuity of the fades
    Params p;
    p.keep = 5; p.areaCut = 0.004f;
    bool monoS = true, monoR = true, range = true;
    float prev = -1, maxStepS = 0, maxStepR = 0;
    for (int i = 0; i <= 20000; ++i) {
        const float a = 0.0f + i * (0.02f / 20000.0f);
        const float f = SizeFactor(a, p, false);
        if (f < prev) monoS = false;
        if (prev >= 0 && f - prev > maxStepS) maxStepS = f - prev;
        if (f < 0 || f > 1) range = false;
        prev = f;
    }
    prev = 2;
    for (int i = 0; i <= 20000; ++i) {
        const float k = 1000.0f + i * (2000.0f / 20000.0f);
        const float f = RankFactor(k, 1500.0f, p);
        if (f > prev) monoR = false;
        if (prev <= 1 && prev - f > maxStepR) maxStepR = prev - f;
        if (f < 0 || f > 1) range = false;
        prev = f;
    }
    Check(monoS, "fade: f is monotone non-decreasing in the screen size");
    Check(monoR, "fade: f is monotone non-increasing in the distance beyond the cut-off");
    Check(range, "fade: f stays in [0, 1]");
    Check(maxStepS < 0.01f && maxStepR < 0.01f, "fade: continuous (no step of f above 0.01 over a fine sweep: nothing pops in the size or the distance)");
    Check(SizeFactor(0.004f * 0.99f, p, false) == 0.0f && SizeFactor(0.004f * 1.26f, p, false) == 1.0f, "fade: the whole fade lies in the top 20% of the range above the cut (0 at the cut, 1 at cut / 0.8)");
    Check(RankFactor(1500, 1500, p) == 1.0f && RankFactor(1500 * 1.2f + 1, 1500, p) == 0.0f, "fade: 1 up to the cut-off distance, 0 at 20% beyond it");

    // the cap and the guard
    Params k;
    k.keep = 14;
    for (int i = 0; i < 20; ++i) { it[i] = Item{}; it[i].dist = 1000.0f + 400.0f * i; it[i].area = 0.01f; }
    Plan(k, it, 20, out);
    int full = 0, zero = 0;
    for (int i = 0; i < 20; ++i) { full += out[i].target == 1.0f; zero += out[i].target == 0.0f; }
    Check(full == 14, "cap: with 20 volumes and keep 14 exactly the 14 nearest keep f == 1");
    Check(out[0].target == 1.0f && out[13].target == 1.0f && out[19].target == 0.0f, "cap: the nearest are full, the farthest is cut");
    Check(zero >= 1 && zero <= 6, "cap: only the beyond-the-band volumes are zero, the others fade");
    k.nearGuard = 20000.0f;
    Plan(k, it, 20, out);
    full = 0;
    for (int i = 0; i < 20; ++i) full += out[i].target == 1.0f;
    Check(full == 20, "guard: nothing within the guard distance of the player is ever reduced");
    Params sz;
    sz.areaCut = 0.01f;
    for (int i = 0; i < 4; ++i) { it[i] = Item{}; it[i].dist = 100; it[i].area = i == 0 ? 0.0001f : (i == 1 ? 0.0099f : (i == 2 ? 0.0115f : 0.05f)); }
    Plan(sz, it, 4, out);
    Check(out[0].target == 0.0f && out[1].target == 0.0f && out[2].target > 0.0f && out[2].target < 1.0f && out[3].target == 1.0f, "size: tiny cut, band faded, large full");
    // a culled volume is neither ranked nor changed
    Params c2; c2.keep = 2;
    for (int i = 0; i < 4; ++i) { it[i] = Item{}; it[i].dist = 100.0f * (i + 1); }
    it[0].culled = true;
    Plan(c2, it, 4, out);
    Check(out[1].target == 1.0f && out[2].target == 1.0f && out[3].target < 1.0f, "cap: a culled volume does not take a slot");

    // stability under jitter: 20 volumes, the 14th and 15th within 1% of each other, distances jittered +-4% every picture; and a size just at the cut
    Params st;
    st.keep = 14; st.areaCut = 0.004f;
    Params naive = st;
    naive.hyst = 0.0f; naive.slew = 1.0f;
    unsigned flipsH = 0, flipsN = 0;
    double tvH = 0, tvN = 0;  // total variation of f: sum of |f - f of the picture before|
    float maxStepH = 0, maxStepN = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const Params& pp = pass == 0 ? st : naive;
        Item cur[kMax];
        float fprev[kMax];
        bool keepPrev[kMax];
        bool inKeep[kMax];
        for (int i = 0; i < 20; ++i) { fprev[i] = -1; keepPrev[i] = false; inKeep[i] = false; }
        for (int pic = 0; pic < 4000; ++pic) {
            for (int i = 0; i < 20; ++i) {
                const float base = 1000.0f + (i < 14 ? 40.0f * i : 40.0f * 13 + 6.0f * (i - 13));  // 14th (i = 13) and 15th (i = 14) 6 units apart
                cur[i].dist = base * (1.0f + 0.04f * (Rnd() * 2 - 1));
                cur[i].area = (i == 7 ? 0.0036f : 0.02f) * (1.0f + 0.15f * (Rnd() * 2 - 1));   // volume 7 sits at the size cut
                cur[i].culled = false;
                cur[i].fPrev = fprev[i];
                cur[i].keepPrev = keepPrev[i];
            }
            Plan(pp, cur, 20, out);
            for (int i = 0; i < 20; ++i) {
                if (pic > 20) {
                    const bool k2 = out[i].f >= 0.5f;
                    if (k2 != inKeep[i]) (pass == 0 ? flipsH : flipsN)++;
                    const float d = std::fabs(out[i].f - fprev[i]);
                    float& m = pass == 0 ? maxStepH : maxStepN;
                    if (fprev[i] >= 0 && d > m) m = d;
                    (pass == 0 ? tvH : tvN) += d;
                }
                inKeep[i] = out[i].f >= 0.5f;
                fprev[i] = out[i].f;
                keepPrev[i] = out[i].keep;
            }
        }
    }
    std::printf("stability: 4000 pictures, 20 volumes, jitter +-4%% distance +-15%% size: set flips hysteresis+slew %u vs naive %u; total variation of f %.1f vs %.1f; largest step of f %.3f vs %.3f\n", flipsH, flipsN, tvH, tvN, maxStepH, maxStepN);
    Check(flipsH <= flipsN && tvH * 1.5 < tvN && tvN > 100.0, "stability: hysteresis + slew move f at least 1.5x less over the run than the naive choice, and flip the kept set no more often (the naive one flickers)");
    Check(maxStepH <= 0.1251f, "stability: f never moves by more than the slew in one picture (a fade, never a pop)");
    Check(maxStepN > 0.5f, "NEG: the naive choice does pop (a step of f above 0.5 is seen)");

    // the slew converges to the target and holds
    Params sl;
    sl.keep = 1;
    it[0] = Item{}; it[0].dist = 100; it[1] = Item{}; it[1].dist = 5000; it[1].fPrev = 1.0f; it[1].keepPrev = true;
    float f = 1.0f;
    int pics = 0;
    for (; pics < 100; ++pics) {
        it[1].fPrev = f;
        Plan(sl, it, 2, out);
        it[1].keepPrev = out[1].keep;
        f = out[1].f;
        if (f == 0.0f) break;
    }
    Check(f == 0.0f && pics + 1 >= 8 && pics + 1 <= 16, "slew: a volume that must go fades out in 8 to 16 pictures (no jump) and ends at exactly 0");
}

int main() {
    TestProject();
    TestGroup();
    TestPlan();
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
