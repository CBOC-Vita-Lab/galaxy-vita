// warp36 host test: the pure logic of port/hrw36.h (the ground-plane warp's uniforms and their CPU mirror, the parallax figure, the fallback gate, the shaped player box, the grid).
// tools/test_hrw36.sh builds and runs it (g++, no dependencies).
#include "../platform/include/port/hrw36.h"
#include <cstdio>
#include <cstring>
using namespace port::hrw36;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

// 4x4 column major, as the back end's warp31::Mul4 / Inv4
static void Mul4(const double* a, const double* b, double* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) { double s = 0; for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; }
}
static bool Inv4(const double* m, double* inv) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { a[r][c] = m[c * 4 + r]; a[r][4 + c] = r == c ? 1.0 : 0.0; }
    for (int i = 0; i < 4; ++i) {
        int piv = i;
        for (int r = i + 1; r < 4; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        if (!(std::fabs(a[piv][i]) > 1e-30)) return false;
        if (piv != i) for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[piv][c]);
        const double d = 1.0 / a[i][i];
        for (int c = 0; c < 8; ++c) a[i][c] *= d;
        for (int r = 0; r < 4; ++r) { if (r == i) continue; const double f = a[r][i]; for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) inv[c * 4 + r] = a[r][4 + c];
    return true;
}
static const int CW = 960, CH = 544;
static void Projection(float P[16]) {  // the game's 45 degree view at 960 x 544, near 100, far 300000
    std::memset(P, 0, 16 * sizeof(float));
    const double cot = 1.0 / std::tan(22.5 * 3.14159265358979 / 180.0), nr = 100.0, fr = 300000.0;
    P[0] = static_cast<float>(cot / (static_cast<double>(CW) / CH));
    P[5] = static_cast<float>(cot);
    P[10] = static_cast<float>(-(fr + nr) / (fr - nr));
    P[11] = -1.0f;
    P[14] = static_cast<float>(-2.0 * fr * nr / (fr - nr));
}
// d = V_B * V_A^-1 (3 x 4 row major): a turn of `yaw` degrees about y and a move t
static void Delta(double yawDeg, double tx, double ty, double tz, float d[12]) {
    const double a = yawDeg * 3.14159265358979 / 180.0, c = std::cos(a), s = std::sin(a);
    const double r[3][3] = {{c, 0, s}, {0, 1, 0}, {-s, 0, c}};
    const double t[3] = {tx, ty, tz};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) d[i * 4 + j] = static_cast<float>(r[i][j]); d[i * 4 + 3] = static_cast<float>(t[i]); }
}
struct Rig {
    float P[16], d[12];
    double M[16];  // clip of B -> clip of A (P D^-1 P^-1), as WarpPrepare builds it
    Rig(double yaw, double tx, double ty, double tz) {
        Projection(P);
        Delta(yaw, tx, ty, tz, d);
        double D4[16], Di[16], Pm[16], Pi[16], T[16];
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) D4[c * 4 + r] = r < 3 ? static_cast<double>(d[r * 4 + c]) : (c == 3 ? 1.0 : 0.0);
        for (int i = 0; i < 16; ++i) Pm[i] = P[i];
        CHECK(Inv4(D4, Di) && Inv4(Pm, Pi));
        Mul4(Pm, Di, T);
        Mul4(T, Pi, M);
    }
    // the source pixel in A of B's pixel (x, y NDC) at inverse depth k (zn = f k - e)
    void Source(double x, double y, double k, double& sx, double& sy) const {
        const double zn = static_cast<double>(P[14]) * k - static_cast<double>(P[10]);
        const double q0 = M[0] * x + M[4] * y + M[8] * zn + M[12], q1 = M[1] * x + M[5] * y + M[9] * zn + M[13], q3 = M[3] * x + M[7] * y + M[11] * zn + M[15];
        sx = (q0 / q3 * 0.5 + 0.5) * CW;
        sy = (q1 / q3 * 0.5 + 0.5) * CH;
    }
    // where A showed the still point X (B's view space)
    void Truth(const double X[3], double& sx, double& sy) const {
        const double q[3] = {X[0] - d[3], X[1] - d[7], X[2] - d[11]};
        double a[3];
        for (int c = 0; c < 3; ++c) a[c] = d[0 * 4 + c] * q[0] + d[1 * 4 + c] * q[1] + d[2 * 4 + c] * q[2];
        sx = ((P[0] * a[0]) / -a[2] * 0.5 + 0.5) * CW;
        sy = ((P[5] * a[1]) / -a[2] * 0.5 + 0.5) * CH;
    }
};
// a camera 400 units above the ground, pitched 20 degrees down; the player's ground point 1200 units ahead on the view axis' foot
static void Ground(float g[3], float n[3]) {
    const double p = 20.0 * 3.14159265358979 / 180.0;
    n[0] = 0.0f; n[1] = static_cast<float>(std::cos(p)); n[2] = static_cast<float>(std::sin(p));
    // the plane n . X = -400; the point under the centre ray's hit: the ray (0, 0, -1) meets it at s = 400 / sin p
    const double s = 400.0 / std::sin(p);
    g[0] = 0.0f; g[1] = 0.0f; g[2] = static_cast<float>(-s);
}

static void TestPlaneExact() {
    Rig R(0.6, 11.0, -2.0, 6.0);  // a run: 0.6 degrees and 13 units a pair
    float g[3], n[3];
    Ground(g, n);
    const float t[3] = {R.d[3], R.d[7], R.d[11]};
    Plane pl;
    CHECK(MakePlane(R.P, g, n, t, CW, CH, 1500.0, 1000.0, 60.0, pl) == kPlaneOk);
    CHECK(pl.on && std::fabs(pl.camH - 400.0f) < 0.01f);
    double worst = 0.0, worstRot = 0.0;
    int hits = 0;
    for (int iy = 0; iy <= 16; ++iy)
        for (int ix = 0; ix <= 16; ++ix) {
            const double x = -1.0 + ix / 8.0, y = -1.0 + iy / 8.0;
            const double raw = RawK(pl, x, y), k = EvalK(pl, x, y);
            if (raw <= 0.0) { CHECK(k == 0.0); continue; }  // (above the plane's horizon: infinity)
            const double X[3] = {(x * pl.ray[0] + pl.ray[2]) / raw, (y * pl.ray[1] + pl.ray[3]) / raw, -1.0 / raw};
            CHECK(std::fabs(n[0] * X[0] + n[1] * X[1] + n[2] * X[2] + 400.0) < 0.05);  // (the hit lies on the plane)
            const double dist = std::sqrt((X[0] - g[0]) * (X[0] - g[0]) + (X[1] - g[1]) * (X[1] - g[1]) + (X[2] - g[2]) * (X[2] - g[2]));
            if (dist > 1500.0) { CHECK(k <= raw + 1e-12); if (dist > 3000.0) CHECK(k == 0.0); continue; }
            CHECK(std::fabs(k - raw) < 1e-9);
            double sx, sy, tx, ty, rx, ry;
            R.Source(x, y, k, sx, sy);
            R.Truth(X, tx, ty);
            R.Source(x, y, 0.0, rx, ry);
            worst = std::max(worst, std::hypot(sx - tx, sy - ty));
            worstRot = std::max(worstRot, std::hypot(rx - tx, ry - ty));
            ++hits;
        }
    printf("plane: %d grid points on the ground within the radius: the plane warp is off by at most %.4f px, the rotation-only warp by up to %.1f px\n", hits, worst, worstRot);
    CHECK(hits > 20);
    CHECK(worst < 0.02);
    CHECK(worstRot > 5.0);
    CHECK(EvalK(pl, 0.0, 0.9) == 0.0);  // the sky
}
static void TestPlaneLimits() {
    Rig R(0.0, 40.0, 0.0, 0.0);  // a large sideways move
    float g[3], n[3];
    Ground(g, n);
    const float t[3] = {R.d[3], R.d[7], R.d[11]};
    Plane pl;
    CHECK(MakePlane(R.P, g, n, t, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneOk);
    double far = 0.0;
    for (int iy = 0; iy <= 18; ++iy)
        for (int ix = 0; ix <= 32; ++ix) {
            const double x = -1.0 + ix / 16.0, y = -1.0 + iy / 9.0;
            double sx, sy, rx, ry;
            R.Source(x, y, EvalK(pl, x, y), sx, sy);
            R.Source(x, y, 0.0, rx, ry);
            far = std::max(far, std::hypot(sx - rx, sy - ry));
        }
    printf("limit: a 40 unit move with a 12 px limit moves a pixel %.2f px at most\n", far);
    CHECK(far <= 12.0 * 1.02 && far > 6.0);
    // refusals
    float low[3] = {0.0f, -20.0f, -1000.0f}, up[3] = {0.0f, 1.0f, 0.0f}, zero[3] = {0.0f, 0.0f, 0.0f};
    CHECK(MakePlane(R.P, low, up, t, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneLow && !pl.on);          // 20 units above the plane
    float above[3] = {0.0f, 300.0f, -1000.0f};
    CHECK(MakePlane(R.P, above, up, t, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneLow);                   // the camera under the plane
    CHECK(MakePlane(R.P, g, zero, t, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneNoGround);
    float bad[16];
    Projection(bad);
    bad[11] = 0.0f;
    CHECK(MakePlane(bad, g, n, t, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneProjection);
    // a still camera: no limit beyond the near end, and the warp is the identity whatever the depth
    Rig S(0.0, 0.0, 0.0, 0.0);
    const float t0[3] = {0, 0, 0};
    CHECK(MakePlane(S.P, g, n, t0, CW, CH, 1500.0, 12.0, 60.0, pl) == kPlaneOk);
    double sx, sy;
    S.Source(0.25, -0.5, EvalK(pl, 0.25, -0.5), sx, sy);
    CHECK(std::fabs(sx - (0.25 * 0.5 + 0.5) * CW) < 1e-6 && std::fabs(sy - (-0.5 * 0.5 + 0.5) * CH) < 1e-6);
}
static void TestParallax() {
    // the header's numbers: 13 units sideways; 300 / 1000 / 1200 / 5000 units away at the picture's centre
    Rig R(0.0, 13.0, 0.0, 0.0);
    const double F = 0.5 * CW * R.P[0];
    printf("parallax: focal length %.1f px; 13 units sideways:", F);
    for (const double z : {300.0, 1000.0, 1200.0, 5000.0}) {
        const float X[3] = {0.0f, 0.0f, static_cast<float>(-z)};
        const double px = ParallaxPx(R.P, R.d, X, CW, CH);
        printf(" %.0f units -> %.1f px;", z, px);
        CHECK(std::fabs(px - F * 13.0 / z) < 0.02 * F * 13.0 / z + 0.01);
    }
    printf("\n");
    // forwards: nothing at the centre, r t / z at radius r
    Rig Fw(0.0, 0.0, 0.0, 13.0);
    const float C[3] = {0.0f, 0.0f, -600.0f};
    CHECK(ParallaxPx(Fw.P, Fw.d, C, CW, CH) < 0.01);
    const float E[3] = {0.0f, -600.0f * 250.0f / (0.5f * CH * Fw.P[5]), -600.0f};  // 250 px under the centre, 600 units deep
    const double pe = ParallaxPx(Fw.P, Fw.d, E, CW, CH);
    printf("parallax: 13 units forwards, 250 px from the centre, 600 units deep: %.1f px\n", pe);
    CHECK(std::fabs(pe - 250.0 * 13.0 / 600.0) < 0.3);
    const float behind[3] = {0.0f, 0.0f, 5.0f};
    CHECK(ParallaxPx(R.P, R.d, behind, CW, CH) < 0.0);
}
static void TestGate() {
    ParGate g;
    CHECK(!g.Update(5.0, 8.0));
    CHECK(g.Update(8.5, 8.0));
    CHECK(g.Update(7.0, 8.0));    // (still off above 75 %)
    CHECK(!g.Update(5.9, 8.0));
    CHECK(!g.Update(7.9, 8.0));   // (and on again only over the limit)
    CHECK(g.Update(100.0, 8.0));
    CHECK(!g.Update(100.0, 0.0)); // (0: the rule is off, and forgets)
    CHECK(!g.off);
}
// plane37, GXV_HRW36_PLANE_GATE: a honey platform (a disc of 700 units round the ground point under him) over the void, a second platform far below it, a wall on it.
// The first picture's depth at the plane's source decides: the platform keeps the plane (exact), the void and the lower platform are at infinity again.
static void TestDepthGate() {
    Rig R(0.3, 11.0, -2.0, 6.0);
    float g[3], n[3];
    Ground(g, n);
    const float t[3] = {R.d[3], R.d[7], R.d[11]};
    Plane pl;
    CHECK(MakePlane(R.P, g, n, t, CW, CH, 1500.0, 40.0, 60.0, pl) == kPlaneOk);
    const Gate G = MakeGate(R.P, pl.kMax, 1.5, 4.0);
    CHECK(G.on && G.kMin > 0.0f && G.sharp == 4.0f);
    // A's view depth of the still point X (B's view space): -z of D^-1 X
    const auto depthInA = [&](const double X[3]) {
        const double q[3] = {X[0] - R.d[3], X[1] - R.d[7], X[2] - R.d[11]};
        return -(R.d[0 * 4 + 2] * q[0] + R.d[1 * 4 + 2] * q[1] + R.d[2 * 4 + 2] * q[2]);
    };
    int onPlat = 0, inVoid = 0, voidMoved = 0;
    double worstPlat = 0.0, swim = 0.0, worstVoid = 0.0;
    for (int iy = 0; iy <= 72; ++iy)
        for (int ix = 0; ix <= 128; ++ix) {
            const double x = -1.0 + ix / 64.0, y = -1.0 + iy / 36.0;
            const double raw = RawK(pl, x, y), k = EvalK(pl, x, y);
            if (raw <= 0.0) {  // (the sky above the plane's horizon: the cleared depth; nothing to gate, nothing moves)
                CHECK(EvalKGated(pl, G, x, y, 1.0) == 0.0);
                continue;
            }
            const double X[3] = {(x * pl.ray[0] + pl.ray[2]) / raw, (y * pl.ray[1] + pl.ray[3]) / raw, -1.0 / raw};
            const double dist = std::sqrt((X[0] - g[0]) * (X[0] - g[0]) + (X[1] - g[1]) * (X[1] - g[1]) + (X[2] - g[2]) * (X[2] - g[2]));
            double sx, sy, rx, ry;
            R.Source(x, y, 0.0, rx, ry);
            if (dist < 700.0) {  // the platform: the first picture has the ground itself at the plane's source
                const double kg = EvalKGated(pl, G, x, y, WindowDepth(R.P, depthInA(X)));
                CHECK(std::fabs(kg - k) < 1e-12);
                double tx, ty;
                R.Source(x, y, kg, sx, sy);
                R.Truth(X, tx, ty);
                if (std::fabs(k - raw) < 1e-9) worstPlat = std::max(worstPlat, std::hypot(sx - tx, sy - ty));
                ++onPlat;
                // a wall or a coin in front of the ground (0.6 of its distance) passes as before the gate
                CHECK(std::fabs(EvalKGated(pl, G, x, y, WindowDepth(R.P, 0.6 * depthInA(X))) - k) < 1e-12);
            } else {  // the void past the edge: the cleared depth (1), or stars drawn at the far end
                ++inVoid;
                R.Source(x, y, k, sx, sy);
                swim = std::max(swim, std::hypot(sx - rx, sy - ry));  // (what the plane without the gate does to a star there)
                if (k > 0.0) ++voidMoved;
                const double kv = EvalKGated(pl, G, x, y, 1.0);
                CHECK(kv == 0.0);
                R.Source(x, y, kv, sx, sy);
                worstVoid = std::max(worstVoid, std::hypot(sx - rx, sy - ry));
                CHECK(EvalKGated(pl, G, x, y, WindowDepth(R.P, 250000.0)) == 0.0);
                // a platform far below (2.5 times the plane's distance on this ray): at infinity too; one 1.15 times as far (a step down): still the plane
                CHECK(EvalKGated(pl, G, x, y, WindowDepth(R.P, 2.5 * depthInA(X))) == 0.0);
                CHECK(std::fabs(EvalKGated(pl, G, x, y, WindowDepth(R.P, 1.15 * depthInA(X))) - k) < 1e-12);
            }
        }
    printf("depth gate: %d points on the platform (plane warp off by at most %.4f px), %d in the void, %d of them moved by the plane without the gate (up to %.1f px): with the gate %.4f px\n",
           onPlat, worstPlat, inVoid, voidMoved, swim, worstVoid);
    CHECK(onPlat > 300 && inVoid > 1000 && voidMoved > 300);
    CHECK(worstPlat < 0.02);
    CHECK(swim > 3.0);
    CHECK(worstVoid == 0.0);
    // the weight between the two limits: 1 up to 1.2 times the plane's distance, 0 from 1.5 times, falling between; at any k, the horizon's 0 included
    for (double k : {0.0, 1e-7, 1e-5, 1.0 / 3000.0, 1.0 / 1200.0, 1.0 / 300.0, static_cast<double>(pl.kMax)}) {
        float cx, cy;
        GateCoef(G, static_cast<float>(k), cx, cy);
        CHECK(std::isfinite(cx) && std::isfinite(cy));
        const double kg = std::max(k, static_cast<double>(G.kMin));
        float last = 2.0f;
        for (double rel = 0.25; rel <= 8.0; rel *= 1.02) {
            const float m = GateM(cx, cy, static_cast<float>(WindowDepth(R.P, rel / kg)));
            CHECK(m >= 0.0f && m <= 1.0f && m <= last + 1e-3f);
            if (rel < 1.19) CHECK(m > 0.97f);
            if (rel > 1.52 && rel / kg < 200000.0) CHECK(m < 0.03f);
            last = m;
        }
        if (k >= 1e-5) CHECK(GateM(cx, cy, 1.0f) == 0.0f);  // the cleared depth (at the plane's horizon the floor kMin is as far as the far end itself: the plane moves nothing there)
        CHECK(GateM(cx, cy, 0.0f) == 1.0f);  // the near end
    }
    // not a perspective projection, no limit: no gate
    float bad[16];
    std::memcpy(bad, R.P, sizeof bad);
    bad[11] = 0.0f;
    CHECK(!MakeGate(bad, pl.kMax, 1.5, 4.0).on);
    CHECK(!MakeGate(R.P, 0.0f, 1.5, 4.0).on);
    CHECK(!MakeGate(R.P, pl.kMax, 0.9, 4.0).on);
}
// plane38: the same scene through the quarter-size mask's 8-bit code (GXV_HRW36_PLANE_GATE_SRC=1)
static void TestDepthGateLog() {
    Rig R(0.3, 11.0, -2.0, 6.0);
    float g[3], n[3];
    Ground(g, n);
    const float t[3] = {R.d[3], R.d[7], R.d[11]};
    Plane pl;
    CHECK(MakePlane(R.P, g, n, t, CW, CH, 1500.0, 40.0, 60.0, pl) == kPlaneOk);
    const Gate G = MakeGateLog(pl.kMax, 1.5, 4.0);
    CHECK(G.on);
    const auto q8 = [](double c) { return std::floor(c * 255.0 + 0.5) / 255.0; };  // (the mask is 8 bits)
    CHECK(DepthCode(32.0) == 0.0 && DepthCode(1048576.0) == 1.0 && std::fabs(DepthCode(1024.0) - 1.0 / 3.0) < 1e-12);
    int onPlat = 0, inVoid = 0;
    for (int iy = 0; iy <= 72; ++iy)
        for (int ix = 0; ix <= 128; ++ix) {
            const double x = -1.0 + ix / 64.0, y = -1.0 + iy / 36.0;
            const double raw = RawK(pl, x, y), k = EvalK(pl, x, y);
            if (raw <= 0.0) { CHECK(EvalKGatedLog(pl, G, x, y, 1.0) == 0.0); continue; }
            const double dist = 1.0 / raw;  // (the plane's distance on this ray; the first picture's differs by the camera's few units)
            if (k > 0.0) {
                CHECK(std::fabs(EvalKGatedLog(pl, G, x, y, q8(DepthCode(dist))) - k) < 1e-12);         // the ground itself
                CHECK(std::fabs(EvalKGatedLog(pl, G, x, y, q8(DepthCode(dist * 0.6))) - k) < 1e-12);   // a wall in front of it
                CHECK(std::fabs(EvalKGatedLog(pl, G, x, y, q8(DepthCode(dist * 1.12))) - k) < 1e-12);  // a step down
                ++onPlat;
            }
            CHECK(EvalKGatedLog(pl, G, x, y, 1.0) == 0.0);                          // the void: the far end's code
            CHECK(EvalKGatedLog(pl, G, x, y, q8(DepthCode(250000.0))) == 0.0);      // the sky dome
            if (dist * 1.6 < 200000.0) CHECK(EvalKGatedLog(pl, G, x, y, q8(DepthCode(dist * 1.6))) == 0.0);  // a platform below
            ++inVoid;
        }
    CHECK(onPlat > 1000 && inVoid > 3000);
    // monotone between the limits, at any k
    for (double k : {0.0, 1e-5, 1.0 / 3000.0, 1.0 / 1200.0, 1.0 / 300.0, static_cast<double>(pl.kMax)}) {
        float cx, cy;
        GateCoefLog(G, static_cast<float>(k), cx, cy);
        CHECK(std::isfinite(cx) && std::isfinite(cy));
        const double kg = std::max(k, static_cast<double>(G.kMin));
        float last = 2.0f;
        for (double rel = 0.25; rel <= 8.0; rel *= 1.02) {
            const float m = GateM(cx, cy, static_cast<float>(DepthCode(rel / kg)));
            CHECK(m >= 0.0f && m <= 1.0f && m <= last + 1e-3f);
            if (rel < 1.19 && rel / kg > 40.0) CHECK(m > 0.97f);
            if (rel > 1.52 && rel / kg < 900000.0) CHECK(m < 0.03f);
            last = m;
        }
    }
    printf("depth gate, mask form: %d points keep the plane through the 8-bit code (the ground, a wall, a step down), %d rays give it up for the void, the sky, a platform below\n", onPlat, inVoid);
    CHECK(!MakeGateLog(0.0f, 1.5, 4.0).on && !MakeGateLog(pl.kMax, 0.5, 4.0).on);
}
static void TestBox() {
    // the Observatory: the player 1024 units away: 0.64 px a unit; his centre 75 units above his feet, upright on the screen (y grows upwards here)
    const double s = 657.0 / 1024.0;
    const Box b = ShapedBox(480.0, 272.0, 480.0, 272.0 - 75.0 * s, s, 85.0, 1.6, 8.0, 8.0, CW, CH);
    CHECK(b.ok);
    printf("box: shaped %d x %d px = %d px (the square of 130 units + 16 px: 200 x 200 = 40000 px)\n", b.x1 - b.x0, b.y1 - b.y0, (b.x1 - b.x0) * (b.y1 - b.y0));
    CHECK(b.y0 >= static_cast<int>(272.0 - 75.0 * s - 8.0) - 2 && b.y0 <= static_cast<int>(272.0 - 75.0 * s - 8.0) + 1);  // footPx under his feet, no more
    CHECK(b.y1 >= static_cast<int>(272.0 + 1.6 * 75.0 * s));                                                             // his head is inside
    CHECK(b.x1 - b.x0 <= 2 * static_cast<int>(85.0 * s + 8.0) + 4 && b.x1 - b.x0 >= 2 * static_cast<int>(85.0 * s + 8.0) - 2);
    CHECK((b.x1 - b.x0) * (b.y1 - b.y0) < 40000 / 2);
    CHECK(!(b.x0 & 1) && !(b.y0 & 1) && !(b.x1 & 1) && !(b.y1 & 1));
    // sideways on a planet: the long side is horizontal
    const Box q = ShapedBox(480.0, 272.0, 480.0 - 75.0 * s, 272.0, s, 85.0, 1.6, 8.0, 8.0, CW, CH);
    CHECK(q.ok && q.x1 - q.x0 == b.y1 - b.y0 && q.y1 - q.y0 == b.x1 - b.x0);
    // seen along his axis: a square
    const Box sq = ShapedBox(480.0, 272.0, 480.5, 272.0, s, 85.0, 1.6, 8.0, 8.0, CW, CH);
    CHECK(sq.ok && sq.x1 - sq.x0 == sq.y1 - sq.y0);
    // at the picture's edge it is clipped; off the picture it is refused
    const Box e = ShapedBox(4.0, 272.0, 4.0, 224.0, s, 85.0, 1.6, 8.0, 8.0, CW, CH);
    CHECK(e.ok && e.x0 == 0);
    CHECK(!ShapedBox(-500.0, 272.0, -500.0, 224.0, s, 85.0, 1.6, 8.0, 8.0, CW, CH).ok);
    CHECK(!ShapedBox(std::nan(""), 272.0, 4.0, 224.0, s, 85.0, 1.6, 8.0, 8.0, CW, CH).ok);
    const Box off = ShapedBox(1.e35, 272., 1.e35, 224., s, 85., 1.6, 8., 8., CW, CH);
    CHECK(!off.ok && off.x0 == CW && off.x1 == CW);
}
static void TestGrid() {
    static float v[2 * kGridVerts];
    GridStrip(v);
    double area = 0.0;
    for (int i = 0; i + 2 < kGridVerts; ++i) {
        const double ax = v[2 * i], ay = v[2 * i + 1], bx = v[2 * i + 2], by = v[2 * i + 3], cx = v[2 * i + 4], cy = v[2 * i + 5];
        area += 0.5 * std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
    }
    for (int i = 0; i < 2 * kGridVerts; ++i) CHECK(v[i] >= -1.0f && v[i] <= 1.0f);
    printf("grid: %d x %d cells, one strip of %d vertices, area %.6f (the picture: 4)\n", kGridX, kGridY, kGridVerts, area);
    CHECK(std::fabs(area - 4.0) < 1e-4);  // (every pixel once: the triangles between rows have no area)
}
static void TestStats() {
    Stats s;
    CHECK(s.planeHeld == 0 && s.planeNoPlayer == 0 && s.planeNoPoly == 0 && s.planeRange == 0);
    for (const double px : {0.5, 3.0, 7.0, 7.5, 20.0}) s.Parallax(px);
    CHECK(s.n == 5 && s.bins[0] == 1 && s.bins[1] == 1 && s.bins[2] == 2 && s.bins[3] == 0 && s.bins[4] == 1 && s.max == 20.0);
}
// hold36: HoldGround, the ground plane through a jump
static void TestHold() {
    const float up[3] = {0.0f, 1.0f, 0.0f}, g0[3] = {100.0f, 0.0f, 50.0f}, fn[3] = {0.0f, 0.9962f, 0.0872f}, none[3] = {};
    const auto feet = [](float x, float y, float z, float* o) { o[0] = x; o[1] = y; o[2] = z; };
    float f[3];
    {   // a jump over a gap: held for 45 turns after the turn the ground was seen, then nothing
        HoldGround h;
        feet(100.0f, 0.0f, 50.0f, f);
        CHECK(h.Step(10, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyNothing);  // (nothing remembered yet)
        CHECK(h.Step(11, 7, f, up, true, g0, fn, false, false, 45) == kHoldNow && h.have && h.poly);
        int held = 0, r = 0;
        for (unsigned t = 12; t <= 60; ++t) {
            feet(100.0f + 10.0f * (t - 11), 150.0f, 50.0f, f);
            r = h.Step(t, 7, f, up, false, none, nullptr, false, false, 45);
            if (r == kHoldHeld) ++held;
            if (t == 12) CHECK(r == kHoldHeld && h.g[0] == g0[0] && h.g[2] == g0[2] && h.n[1] == fn[1] && h.up[1] == 1.0f);
            if (t == 56) CHECK(r == kHoldHeld);                       // (the 45th turn after 11)
            if (t == 57) CHECK(r == kHoldNone && h.why == kHoldWhyExpired && !h.have);
        }
        CHECK(held == 45 && r == kHoldNone);
        // ground again: remembered anew, without a polygon his up is the normal
        CHECK(h.Step(61, 7, f, up, true, g0, nullptr, false, false, 45) == kHoldNow && h.have && !h.poly && h.n[1] == 1.0f);
        // 0 turns = off: never held, and nothing else changes
        CHECK(h.Step(62, 7, f, up, false, none, nullptr, false, false, 0) == kHoldNone && h.why == kHoldWhyOff);
    }
    const auto seen = [&](HoldGround& h) { feet(100.0f, 0.0f, 50.0f, f); CHECK(h.Step(100, 7, f, up, true, g0, fn, false, false, 45) == kHoldNow); };
    {   // the height range against the remembered point: above 1500 and under -200 no plane, back inside it is held again
        HoldGround h; seen(h);
        feet(100.0f, 1499.0f, 50.0f, f); CHECK(h.Step(101, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);
        feet(100.0f, 1501.0f, 50.0f, f); CHECK(h.Step(102, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyRange && h.have);
        feet(100.0f, 900.0f, 50.0f, f);  CHECK(h.Step(103, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);
        feet(100.0f, -150.0f, 50.0f, f); CHECK(h.Step(104, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);
        feet(100.0f, -250.0f, 50.0f, f); CHECK(h.Step(105, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyRange);
    }
    {   // his gravity turns: 20 degrees is the same ground, 30 is another planet (forgotten: turning back does not bring it back)
        HoldGround h; seen(h);
        const double d = 3.14159265358979 / 180.0;
        const float up20[3] = {static_cast<float>(std::sin(20 * d)), static_cast<float>(std::cos(20 * d)), 0.0f}, up30[3] = {static_cast<float>(std::sin(30 * d)), static_cast<float>(std::cos(30 * d)), 0.0f};
        feet(100.0f, 100.0f, 50.0f, f);
        CHECK(h.Step(101, 7, f, up20, false, none, nullptr, false, false, 45) == kHoldHeld);
        CHECK(h.Step(102, 7, f, up30, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyGravity && !h.have);
        CHECK(h.Step(103, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone);
    }
    {   // a warp: more than 1500 units in one turn, even back into the range
        HoldGround h; seen(h);
        feet(100.0f, 100.0f, 1400.0f, f); CHECK(h.Step(101, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);  // (1350 units: a launch, not a warp)
        feet(100.0f, 100.0f, 3000.0f, f); CHECK(h.Step(102, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyTeleport && !h.have);
        const float bad[3] = {std::nanf(""), 0.0f, 0.0f};
        HoldGround n; seen(n);
        CHECK(n.Step(101, 7, bad, up, false, none, nullptr, false, false, 45) == kHoldNone && !n.have);
    }
    {   // another scene, stage or scenario (the key), a death, a gap in the turns (a demo)
        HoldGround h; seen(h);
        feet(100.0f, 100.0f, 50.0f, f);
        CHECK(h.Step(101, 8, f, up, false, none, nullptr, false, false, 45) == kHoldNone && h.why == kHoldWhyScene && !h.have);
        HoldGround d; seen(d);
        CHECK(d.Step(101, 7, f, up, false, none, nullptr, false, true, 45) == kHoldNone && d.why == kHoldWhyDead && !d.have);
        HoldGround gp; seen(gp);
        CHECK(gp.Step(100 + kHoldGapTurns, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);
        CHECK(gp.Step(100 + 2 * kHoldGapTurns + 1, 7, f, up, false, none, nullptr, false, false, 45) == kHoldNone && gp.why == kHoldWhyGap && !gp.have);
        // the turn counter wraps: differences only
        HoldGround w;
        feet(100.0f, 0.0f, 50.0f, f);
        CHECK(w.Step(0xFFFFFFFEu, 7, f, up, true, g0, fn, false, false, 45) == kHoldNow);
        CHECK(w.Step(3u, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld);
    }
    {   // ground38: ground that moves (a lift, a turning planetoid) is remembered, held 15 turns at most, and goes with its collision
        HoldGround h;
        feet(100.0f, 0.0f, 50.0f, f);
        CHECK(h.Step(100, 7, f, up, true, g0, fn, true, false, 45) == kHoldNow && h.have && h.movingGround);
        feet(100.0f, 100.0f, 50.0f, f);
        const float m1[3] = {100.0f, 40.0f, 50.0f}, far[3] = {100.0f, 301.0f, 50.0f};
        CHECK(h.Step(101, 7, f, up, false, none, nullptr, false, false, 45, m1) == kHoldHeld && h.g[1] == 40.0f && h.g0[1] == 0.0f);  // (the lift rose 40: so did the plane)
        CHECK(h.Step(102, 7, f, up, false, none, nullptr, false, false, 45) == kHoldHeld && h.g[1] == 40.0f);                      // (no news of it: where it was last)
        for (unsigned t = 103; t <= 100 + kHoldMovingTurns; ++t) CHECK(h.Step(t, 7, f, up, false, none, nullptr, false, false, 45, m1) == kHoldHeld);
        CHECK(h.Step(101 + kHoldMovingTurns, 7, f, up, false, none, nullptr, false, false, 45, m1) == kHoldNone && h.why == kHoldWhyExpired && !h.have);
        HoldGround k;
        feet(100.0f, 0.0f, 50.0f, f);
        CHECK(k.Step(100, 7, f, up, true, g0, fn, true, false, 45) == kHoldNow);
        CHECK(k.Step(101, 7, f, up, false, none, nullptr, false, false, 45, far) == kHoldNone && k.why == kHoldWhyMoving && !k.have);  // (carried over 300 units away)
        HoldGround sh;   // a shorter GXV_HRW36_HOLD_TURNS wins; ground that stands still ignores gMoved
        CHECK(sh.Step(100, 7, f, up, true, g0, fn, true, false, 5) == kHoldNow);
        CHECK(sh.Step(106, 7, f, up, false, none, nullptr, false, false, 5, m1) == kHoldNone && sh.why == kHoldWhyExpired);
        HoldGround st; seen(st);
        feet(100.0f, 100.0f, 50.0f, f);
        CHECK(st.Step(101, 7, f, up, false, none, nullptr, false, false, 45, far) == kHoldHeld && st.g[1] == 0.0f);
    }
    printf("hold: expiry, range, gravity, warp, scene, death, gap, moving ground checked\n");
}
int main() {
    TestHold();
    TestPlaneExact();
    TestPlaneLimits();
    TestParallax();
    TestGate();
    TestDepthGate();
    TestDepthGateLog();
    TestBox();
    TestGrid();
    TestStats();
    printf(fails ? "hrw36_test: %d FAILED\n" : "hrw36_test: all passed\n", fails);
    return fails ? 1 : 0;
}
