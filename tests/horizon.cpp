// horizon25: host test of platform/include/port/horizon.h (the shadow-cone test and the occluder fit).
//   g++ -std=c++17 -O2 -fsanitize=address,undefined -I platform/include tests/horizon.cpp -o /tmp/horizon_test && /tmp/horizon_test [cases]
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <random>
#include "port/horizon.h"
using namespace port::horizon;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// exact: does the segment E->X cross the ball (C, R)?
static bool SegHitsBall(const double E[3], const double X[3], const double C[3], double R) {
    double d[3] = {X[0] - E[0], X[1] - E[1], X[2] - E[2]};
    const double len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    double w[3] = {C[0] - E[0], C[1] - E[1], C[2] - E[2]};
    double t = len2 > 0 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double p[3] = {E[0] + t * d[0] - C[0], E[1] + t * d[1] - C[1], E[2] + t * d[2] - C[2]};
    return p[0] * p[0] + p[1] * p[1] + p[2] * p[2] <= R * R;
}

static void TestCone(long cases) {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> U(-1, 1), U01(0, 1);
    long culled = 0, trulyHidden = 0, falseCull = 0, tested = 0;
    for (long n = 0; n < cases; n++) {
        const double scale = std::pow(10.0, U(rng) * 2.0);          // 0.1 .. 100 (a unit of 1000)
        double E[3] = {U(rng) * scale, U(rng) * scale, U(rng) * scale};
        double C[3] = {U(rng) * scale, U(rng) * scale, U(rng) * scale};
        double R = (0.02 + U01(rng) * 0.6) * scale;
        double P[3] = {C[0] + U(rng) * 3 * scale, C[1] + U(rng) * 3 * scale, C[2] + U(rng) * 3 * scale};
        // half the cases: put P near the shadow axis behind the ball so many are hidden
        if (n & 1) {
            double v[3] = {C[0] - E[0], C[1] - E[1], C[2] - E[2]};
            const double k = 1.0 + U01(rng) * 2.5;
            for (int i = 0; i < 3; i++) P[i] = E[i] + v[i] * k + U(rng) * R * 0.9;
        }
        const double q = U01(rng) * 0.5 * R + (n % 7 == 0 ? 0.0 : U01(rng) * 0.2 * scale);
        const float Ef[3] = {(float)E[0], (float)E[1], (float)E[2]}, Cf[3] = {(float)C[0], (float)C[1], (float)C[2]}, Pf[3] = {(float)P[0], (float)P[1], (float)P[2]};
        const float margin = 0.01f + (float)U01(rng) * 0.1f;
        const Cone k = MakeCone(Ef, Cf, (float)R, margin, margin / 3);
        if (!k.ok) continue;
        ++tested;
        const bool was = (n % 3) == 0;
        const bool hid = Hidden(k, Pf, (float)q, was);
        // ground truth by sampling the sphere: the points on the silhouette plane (extreme angles), the nearest point, and random points
        double v[3] = {P[0] - E[0], P[1] - E[1], P[2] - E[2]};
        const double e = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        bool allHit = true;
        double pts[400][3]; int np = 0;
        pts[np][0] = P[0] - q * v[0] / e; pts[np][1] = P[1] - q * v[1] / e; pts[np][2] = P[2] - q * v[2] / e; np++;
        // extreme directions seen from E: in the plane of (EP, EC) and in 24 other planes, the two tangent points of the sphere's cone
        for (int a = 0; a < 24; a++) {
            const double ang = a * 3.14159265358979 / 12.0;
            double u[3] = {std::cos(ang), std::sin(ang), 0}, w2[3];
            // basis perpendicular to v
            double t1[3], t2[3]; double aux[3] = {std::fabs(v[0]) < 0.9 * e ? 1.0 : 0.0, std::fabs(v[0]) < 0.9 * e ? 0.0 : 1.0, 0.0};
            t1[0] = v[1] * aux[2] - v[2] * aux[1]; t1[1] = v[2] * aux[0] - v[0] * aux[2]; t1[2] = v[0] * aux[1] - v[1] * aux[0];
            double l1 = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]); for (int i = 0; i < 3; i++) t1[i] /= l1;
            t2[0] = v[1] * t1[2] - v[2] * t1[1]; t2[1] = v[2] * t1[0] - v[0] * t1[2]; t2[2] = v[0] * t1[1] - v[1] * t1[0];
            double l2 = std::sqrt(t2[0] * t2[0] + t2[1] * t2[1] + t2[2] * t2[2]); for (int i = 0; i < 3; i++) t2[i] /= l2;
            for (int i = 0; i < 3; i++) w2[i] = u[0] * t1[i] + u[1] * t2[i];
            // the tangent point of the cone from E to the sphere: P + q * (cos(b) * w2 - sin(b) * vhat), sin(b) = q / e
            const double sb = q / e, cb = std::sqrt(std::max(0.0, 1 - sb * sb));
            for (int i = 0; i < 3; i++) pts[np][i] = P[i] + q * (cb * w2[i] - sb * v[i] / e);
            np++;
            for (int i = 0; i < 3; i++) pts[np][i] = P[i] + q * w2[i];
            np++;
        }
        for (int s = 0; s < 120; s++) {
            double u[3] = {U(rng), U(rng), U(rng)}; double l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); if (l < 1e-6) continue;
            const double rr = (s & 1) ? 1.0 : std::cbrt(U01(rng));
            for (int i = 0; i < 3; i++) pts[np][i] = P[i] + q * rr * u[i] / l;
            np++;
        }
        for (int i = 0; i < np && allHit; i++) allHit = SegHitsBall(E, pts[i], C, R);
        if (allHit) ++trulyHidden;
        if (hid) {
            ++culled;
            if (!allHit) {
                ++falseCull;
                if (falseCull <= 5) std::printf("  false cull: scale %g R %g q %g margin %g e %g\n", scale, R, q, (double)margin, e);
            }
        }
    }
    std::printf("cone: %ld cases (%ld with a cone), culled %ld, truly hidden by sampling %ld (we find %.1f%% of them), FALSE CULLS %ld\n", cases, tested,
                culled, trulyHidden, trulyHidden ? 100.0 * (double)culled / (double)trulyHidden : 0.0, falseCull);
    CHECK(falseCull == 0, "false culls");
    CHECK(culled > 1000 || cases < 100000, "the test culls something");
}

// -- the fit on synthetic meshes ----------------------------------------------------------------------------------------------------
struct Mesh {
    std::vector<double> v;   // xyz
    std::vector<int> t;      // triples
    std::vector<double> n;   // per-triangle normal (3)
    void AddTri(const double a[3], const double b[3], const double c[3], double sign = 1.0) {
        const int base = (int)v.size() / 3;
        for (int i = 0; i < 3; i++) v.push_back(a[i]);
        for (int i = 0; i < 3; i++) v.push_back(b[i]);
        for (int i = 0; i < 3; i++) v.push_back(c[i]);
        t.push_back(base); t.push_back(base + 1); t.push_back(base + 2);
        double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        double l = std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        for (int i = 0; i < 3; i++) n.push_back(sign * cr[i] / (l > 0 ? l : 1));
    }
    int Count() const { return (int)t.size() / 3; }
};
// a UV sphere of radius r about c, outward CCW; normalsOut: the stored normals point out (+1) or in (-1); shuffleWinding: random vertex order
static void AddSphere(Mesh& m, const double c[3], double r, int nu, int nv, double normalSign, bool shuffleWinding, std::mt19937& rng, double bump = 0.0) {
    auto P = [&](int i, int j, double out[3]) {
        const double th = 3.14159265358979 * j / nv, ph = 2 * 3.14159265358979 * (i % nu) / nu;
        double rr = r * (1.0 + bump * std::sin(3 * ph) * std::sin(2 * th));
        out[0] = c[0] + rr * std::sin(th) * std::cos(ph); out[1] = c[1] + rr * std::cos(th); out[2] = c[2] + rr * std::sin(th) * std::sin(ph);
    };
    for (int j = 0; j < nv; j++) {
        for (int i = 0; i < nu; i++) {
            double a[3], b[3], d[3], e[3];
            P(i, j, a); P(i + 1, j, b); P(i, j + 1, d); P(i + 1, j + 1, e);
            // outward CCW: (a, d, b) and (b, d, e) for this parametrisation
            double* tris[2][3] = {{a, d, b}, {b, d, e}};
            for (int k = 0; k < 2; k++) {
                double *x = tris[k][0], *y = tris[k][1], *z = tris[k][2];
                const double dx[3] = {y[0] - x[0], y[1] - x[1], y[2] - x[2]}, dz[3] = {z[0] - x[0], z[1] - x[1], z[2] - x[2]};
                const double cr2 = dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2];
                if (cr2 < 1e-18) continue;
                double cr[3] = {dx[1] * dz[2] - dx[2] * dz[1], dx[2] * dz[0] - dx[0] * dz[2], dx[0] * dz[1] - dx[1] * dz[0]};
                if (std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) < 1e-12) continue;  // pole sliver
                // make it outward whatever the parametrisation says
                double mid[3] = {(x[0] + y[0] + z[0]) / 3 - c[0], (x[1] + y[1] + z[1]) / 3 - c[1], (x[2] + y[2] + z[2]) / 3 - c[2]};
                const bool outward = cr[0] * mid[0] + cr[1] * mid[1] + cr[2] * mid[2] > 0;
                if (outward) m.AddTri(x, y, z, normalSign); else m.AddTri(x, z, y, normalSign);
                if (shuffleWinding && (rng() & 1)) {   // reverse the stored vertex order but keep the stored normal
                    const int s = (int)m.v.size();
                    for (int q = 0; q < 3; q++) std::swap(m.v[s - 9 + 3 + q], m.v[s - 9 + 6 + q]);
                }
            }
        }
    }
}
static Fit Run(const Mesh& m) {
    return FitOccluder(m.Count(), [&](int i, double v[3][3], double n[3]) {
        for (int k = 0; k < 3; k++) for (int j = 0; j < 3; j++) v[k][j] = m.v[(size_t)(i * 3 + k) * 3 + j];
        for (int j = 0; j < 3; j++) n[j] = m.n[(size_t)i * 3 + j];
        double e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]}, e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
        double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        return cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2] > 1e-18;
    });
}
static void TestFit() {
    std::mt19937 rng(7);
    const double O[3] = {0, 0, 0}, off[3] = {5.0, -3.0, 2.0};
    {   // closed sphere radius 100, centre off-origin: the fit finds ~the centre and ~r
        Mesh m; AddSphere(m, off, 100, 24, 16, 1.0, false, rng);
        Fit f = Run(m);
        CHECK(f.ok, "closed sphere: %s", f.why);
        CHECK(f.ok && std::fabs(f.winding - 1) < 0.02, "winding %g", f.winding);
        CHECK(f.ok && f.dmin > 93 && f.dmin <= 100.0001, "inscribed %g of a 100 sphere (UV facets)", f.dmin);
        std::printf("fit sphere: dmin %.2f (r 100), centre off by %.2f, %d tris\n", f.dmin, std::sqrt((f.c[0]-off[0])*(f.c[0]-off[0])+(f.c[1]-off[1])*(f.c[1]-off[1])+(f.c[2]-off[2])*(f.c[2]-off[2])), m.Count());
    }
    {   // same with random vertex order (stored normals outward)
        Mesh m; AddSphere(m, O, 100, 24, 16, 1.0, true, rng);
        Fit f = Run(m);
        CHECK(f.ok && f.dmin > 93, "shuffled winding: %s", f.why);
    }
    {   // lumpy planet (+-15%): the fit is the least distance, never more than the nearest surface point
        Mesh m; AddSphere(m, O, 100, 40, 28, 1.0, false, rng, 0.15);
        Fit f = Run(m);
        CHECK(f.ok, "lumpy: %s", f.why);
        CHECK(f.ok && f.dmin <= 90.0, "lumpy dmin %g (the surface dips to 85..90)", f.dmin);
        std::printf("fit lumpy: dmin %.2f (surface 85..115)\n", f.dmin);
    }
    {   // inward normals
        Mesh m; AddSphere(m, O, 100, 24, 16, -1.0, false, rng);
        Fit f = Run(m); CHECK(!f.ok, "inward normals accepted"); std::printf("fit inward: rejected: %s\n", f.why);
    }
    {   // hollow planet: outer sphere + an inner cavity whose normals point into the cavity
        Mesh m; AddSphere(m, O, 100, 24, 16, 1.0, false, rng); AddSphere(m, O, 60, 24, 16, -1.0, false, rng);
        Fit f = Run(m); CHECK(!f.ok, "hollow accepted"); std::printf("fit hollow: rejected: %s\n", f.why);
    }
    {   // hollow planet whose centre is solid: a cavity off-centre is fine for an inscribed ball that avoids it, and the fit must keep clear of it
        Mesh m; AddSphere(m, O, 100, 24, 16, 1.0, false, rng);
        const double cav[3] = {45, 0, 0}; AddSphere(m, cav, 30, 16, 12, -1.0, false, rng);
        Fit f = Run(m);
        if (f.ok) {   // whichever centre it took, the ball must be clear of the cavity (distance to the cavity centre >= 30 + dmin) and of the shell
            const double dc = std::sqrt((f.c[0]-45)*(f.c[0]-45)+f.c[1]*f.c[1]+f.c[2]*f.c[2]);
            CHECK(dc >= 30 + f.dmin - 1.0, "ball overlaps the cavity: dc %g dmin %g", dc, f.dmin);
            CHECK(std::sqrt(f.c[0]*f.c[0]+f.c[1]*f.c[1]+f.c[2]*f.c[2]) + f.dmin <= 100.5, "ball outside the shell");
        }
        std::printf("fit off-centre cavity: %s dmin %.2f\n", f.ok ? "accepted" : f.why, f.dmin);
    }
    {   // nested outward shells (a planet with a second skin): winding 2
        Mesh m; AddSphere(m, O, 100, 24, 16, 1.0, false, rng); AddSphere(m, O, 60, 24, 16, 1.0, false, rng);
        Fit f = Run(m); CHECK(!f.ok, "nested accepted"); std::printf("fit nested: rejected: %s\n", f.why);
    }
    {   // open mesh: the upper half of a sphere
        Mesh full; AddSphere(full, O, 100, 24, 16, 1.0, false, rng);
        Mesh m;
        for (int i = 0; i < full.Count(); i++) {
            double cy = 0; for (int k = 0; k < 3; k++) cy += full.v[(size_t)(i * 3 + k) * 3 + 1];
            if (cy / 3 < 0) continue;
            m.AddTri(&full.v[(size_t)i * 9], &full.v[(size_t)i * 9 + 3], &full.v[(size_t)i * 9 + 6]);
            for (int j = 0; j < 3; j++) m.n[m.n.size() - 3 + j] = full.n[(size_t)i * 3 + j];
        }
        Fit f = Run(m); CHECK(!f.ok, "open hemisphere accepted"); std::printf("fit hemisphere: rejected: %s\n", f.why);
    }
    {   // a cube (12 triangles; stored normals outward): closed solid, ball = half the side
        Mesh m; const double s = 50;
        const double V[8][3] = {{-s,-s,-s},{s,-s,-s},{s,s,-s},{-s,s,-s},{-s,-s,s},{s,-s,s},{s,s,s},{-s,s,s}};
        const int F[12][3] = {{0,3,2},{0,2,1},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{1,2,6},{1,6,5},{3,0,4},{3,4,7}};
        for (auto& f : F) m.AddTri(V[f[0]], V[f[1]], V[f[2]]);
        Fit f = Run(m);
        CHECK(f.ok && std::fabs(f.dmin - 50) < 0.01, "cube dmin %g", f.dmin);
        std::printf("fit cube: dmin %.3f (side half 50)\n", f.dmin);
    }
    {   // a disc (flat closed slab 200 x 200 x 4): thin: rejected
        Mesh m; const double sx = 100, sy = 2;
        const double V[8][3] = {{-sx,-sy,-sx},{sx,-sy,-sx},{sx,sy,-sx},{-sx,sy,-sx},{-sx,-sy,sx},{sx,-sy,sx},{sx,sy,sx},{-sx,sy,sx}};
        const int F[12][3] = {{0,3,2},{0,2,1},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{2,3,7},{2,7,6},{1,2,6},{1,6,5},{3,0,4},{3,4,7}};
        for (auto& f : F) m.AddTri(V[f[0]], V[f[1]], V[f[2]]);
        Fit f = Run(m); CHECK(!f.ok, "slab accepted"); std::printf("fit slab: rejected: %s\n", f.why);
    }
    {   // the fit's ball really is inside: sample points of the ball against the sphere's analytic solid
        Mesh m; AddSphere(m, off, 100, 32, 20, 1.0, false, rng, 0.08);
        Fit f = Run(m);
        CHECK(f.ok, "bumpy");
        if (f.ok) {
            int bad = 0;
            for (int s = 0; s < 20000; s++) {
                double u[3] = {(double)(rng() % 2001) / 1000.0 - 1, (double)(rng() % 2001) / 1000.0 - 1, (double)(rng() % 2001) / 1000.0 - 1};
                double l = std::sqrt(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); if (l < 1e-6) continue;
                double p[3] = {f.c[0] + f.dmin * u[0] / l, f.c[1] + f.dmin * u[1] / l, f.c[2] + f.dmin * u[2] / l};
                // analytic surface radius at p (the bump formula): inside iff |p - off| <= radius(direction)
                double d[3] = {p[0]-off[0], p[1]-off[1], p[2]-off[2]};
                double rl = std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
                double th = std::acos(d[1] / rl), ph = std::atan2(d[2], d[0]); if (ph < 0) ph += 2 * 3.14159265358979;
                double rs = 100 * (1.0 + 0.08 * std::sin(3 * ph) * std::sin(2 * th));
                if (rl > rs * 1.02) ++bad;   // (2%: the facets cut the analytic surface in places)
            }
            CHECK(bad == 0, "%d sample points of the ball are outside the bumpy planet", bad);
        }
    }
}

// -- hysteresis: an object crossing the cone edge slowly, with a noisy camera -----------------------------------------------------------
static void TestHysteresis() {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> U(-1, 1);
    const float C[3] = {0, 0, 0};
    int flipsHyst = 0, flipsPlain = 0, steps = 0;
    bool wasH = false, wasP = false;
    for (int s = 0; s < 4000; s++) {
        // the camera orbits slowly; the object sits near the shadow edge; the camera jitters by 0.2 units each turn
        const float t = s * 0.002f;
        const float E[3] = {1000.0f * std::cos(t) + U(rng) * 0.2f, 20.0f + U(rng) * 0.2f, 1000.0f * std::sin(t)};
        const float R = 300.0f;
        const float P[3] = {-1500.0f * std::cos(0.0f) + 0, 340.0f + 0.01f * s, 0};
        const Cone kh = MakeCone(E, C, R, 0.03f, 0.01f), kp = MakeCone(E, C, R, 0.03f, 0.03f);
        if (!kh.ok) continue;
        const bool h = Hidden(kh, P, 40.0f, wasH), p = Hidden(kp, P, 40.0f, wasP);
        if (h != wasH) ++flipsHyst;
        if (p != wasP) ++flipsPlain;
        wasH = h; wasP = p; ++steps;
    }
    std::printf("hysteresis: %d steps, state changes with the exit margin %d, without %d\n", steps, flipsHyst, flipsPlain);
    CHECK(flipsHyst <= flipsPlain, "hysteresis flips more");
    // a hidden object is never un-hidden by the exit cone while a visible one is not hidden by it
    const float E[3] = {1000, 0, 0}, P[3] = {-1500, 0, 0};
    const Cone k = MakeCone(E, C, 300.0f, 0.05f, 0.01f);
    CHECK(Hidden(k, P, 50.0f, false) && Hidden(k, P, 50.0f, true), "behind the centre must be hidden");
    const float Pn[3] = {-100, 500, 0};
    CHECK(!Hidden(k, Pn, 50.0f, false) && !Hidden(k, Pn, 50.0f, true), "on the near side must not be hidden");
    const float Pl[3] = {-1500, 305, 0};   // just inside the exit cone's edge but not the enter cone's: hidden only when it was
    CHECK(MakeCone(E, C, 300.0f, 0.0f, 0.0f).ok, "cone without margin");
    // camera inside the ball: no cone
    const float Ei[3] = {10, 0, 0};
    CHECK(!MakeCone(Ei, C, 300.0f, 0.03f, 0.01f).ok, "camera inside the ball gives a cone");
    (void)Pl;
}

int main(int argc, char** argv) {
    const long cases = argc > 1 ? std::atol(argv[1]) : 3000000;
    TestCone(cases);
    TestFit();
    TestHysteresis();
    std::printf(fails ? "horizon: %d FAILURES\n" : "horizon: all passed\n", fails);
    return fails != 0;
}
