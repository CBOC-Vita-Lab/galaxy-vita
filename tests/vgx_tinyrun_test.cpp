// dread25: host test of platform/vgx/src/vgx_tinyrun.h (GXV_TINYRUN: the projected area of a static run's box and the decision to leave a tiny run out).
//   tools/test_vgx_tinyrun.sh
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "../platform/vgx/src/vgx_tinyrun.h"

using namespace vgx::tinyrun;
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345u;
static float rnd() { rng = rng * 1664525u + 1013904223u; return static_cast<float>(rng >> 8) / 16777216.0f; }
static float rr(float a, float b) { return a + (b - a) * rnd(); }

struct Mat { float r[3][4]; const float* row(int i) const { return r[i]; } };
static void rowsOf(const Mat& M, const float* out[3]) { for (int i = 0; i < 3; ++i) out[i] = M.r[i]; }
static Mat fromRotTrans(float ax, float ay, float az, float tx, float ty, float tz) {
    const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
    Mat M;
    M.r[0][0] = cy * cz; M.r[0][1] = -cy * sz; M.r[0][2] = sy;
    M.r[1][0] = sx * sy * cz + cx * sz; M.r[1][1] = -sx * sy * sz + cx * cz; M.r[1][2] = -sx * cy;
    M.r[2][0] = -cx * sy * cz + sx * sz; M.r[2][1] = cx * sy * sz + sx * cz; M.r[2][2] = cx * cy;
    M.r[0][3] = tx; M.r[1][3] = ty; M.r[2][3] = tz;
    return M;
}
static bool projectPoint(const float p[6], bool ortho, const Mat& M, const float v[3], float* nx, float* ny) {
    const float ex = M.r[0][0] * v[0] + M.r[0][1] * v[1] + M.r[0][2] * v[2] + M.r[0][3];
    const float ey = M.r[1][0] * v[0] + M.r[1][1] * v[1] + M.r[1][2] * v[2] + M.r[1][3];
    const float ez = M.r[2][0] * v[0] + M.r[2][1] * v[1] + M.r[2][2] * v[2] + M.r[2][3];
    float cx, cy, cw;
    if (ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
    else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
    if (!(cw > 1e-4f)) return false;
    *nx = cx / cw; *ny = cy / cw;
    return true;
}

int main() {
    const float persp[6] = {1.2f, 0.0f, 1.6f, 0.0f, -1.0f, -2.0f};
    const float vpW = 640.0f, vpH = 456.0f;
    // 1. a plain box straight ahead: area = (p0 w / d) (p2 h / d) / 4 vpW vpH
    {
        Mat M = fromRotTrans(0, 0, 0, 0, 0, -1000.0f);
        const float* m[3]; rowsOf(M, m);
        const float mn[3] = {-50, -50, -50}, mx[3] = {50, 50, 50};
        // the nearest face (z = +50 in model, ez = -950) is the largest; the rectangle is that of the near face's corners
        const float a = ProjectedAreaPx(persp, false, m, mn, mx, vpW, vpH);
        const float w = 1.2f * 100.0f / 950.0f, h = 1.6f * 100.0f / 950.0f;
        CHECK(std::fabs(a - w * h * 0.25f * vpW * vpH) < 0.01f * a);
        CHECK(a > 0.0f);
    }
    // 2. area falls with the square of the distance (a thin box, so the depth extent does not matter)
    {
        const float mn[3] = {-10, -10, -0.01f}, mx[3] = {10, 10, 0.01f};
        Mat M1 = fromRotTrans(0, 0, 0, 0, 0, -500.0f), M2 = fromRotTrans(0, 0, 0, 0, 0, -1000.0f);
        const float *m1[3], *m2[3]; rowsOf(M1, m1); rowsOf(M2, m2);
        const float a1 = ProjectedAreaPx(persp, false, m1, mn, mx, vpW, vpH), a2 = ProjectedAreaPx(persp, false, m2, mn, mx, vpW, vpH);
        CHECK(std::fabs(a1 / a2 - 4.0f) < 0.01f);
    }
    // 3. behind the eye, and across the near plane: not known
    {
        const float mn[3] = {-10, -10, -10}, mx[3] = {10, 10, 10};
        Mat B = fromRotTrans(0, 0, 0, 0, 0, 500.0f);   // ez = +500: behind
        Mat X = fromRotTrans(0, 0, 0, 0, 0, -5.0f);    // the box reaches ez = +5
        const float *mb[3], *mx_[3]; rowsOf(B, mb); rowsOf(X, mx_);
        CHECK(ProjectedAreaPx(persp, false, mb, mn, mx, vpW, vpH) < 0.0f);
        CHECK(ProjectedAreaPx(persp, false, mx_, mn, mx, vpW, vpH) < 0.0f);
    }
    // 4. bad numbers and bad viewports
    {
        const float mn[3] = {-1, -1, -1}, mx[3] = {1, 1, 1};
        Mat M = fromRotTrans(0, 0, 0, 0, 0, -100.0f);
        const float* m[3]; rowsOf(M, m);
        CHECK(ProjectedAreaPx(persp, false, m, mn, mx, 0.0f, vpH) < 0.0f);
        CHECK(ProjectedAreaPx(persp, false, m, mn, mx, vpW, -3.0f) < 0.0f);
        CHECK(ProjectedAreaPx(persp, false, m, mn, mx, NAN, vpH) < 0.0f);
        const float nanBox[3] = {NAN, 0, 0};
        CHECK(ProjectedAreaPx(persp, false, m, nanBox, mx, vpW, vpH) < 0.0f);
        float big[6] = {1e30f, 0, 1e30f, 0, 0, 0};
        const float a = ProjectedAreaPx(big, false, m, mn, mx, vpW, vpH);
        CHECK(a < 0.0f || std::isfinite(a));
    }
    // 5. orthographic: the area does not depend on the depth
    {
        const float ortho[6] = {0.01f, 0.0f, 0.01f, 0.0f, -0.001f, -1.0f};
        const float mn[3] = {-30, -30, -30}, mx[3] = {30, 30, 30};
        Mat M1 = fromRotTrans(0, 0, 0, 0, 0, -100.0f), M2 = fromRotTrans(0, 0, 0, 0, 0, -9000.0f);
        const float *m1[3], *m2[3]; rowsOf(M1, m1); rowsOf(M2, m2);
        const float a1 = ProjectedAreaPx(ortho, true, m1, mn, mx, vpW, vpH), a2 = ProjectedAreaPx(ortho, true, m2, mn, mx, vpW, vpH);
        CHECK(a1 > 0.0f && std::fabs(a1 - a2) < 1e-3f * a1);
        CHECK(std::fabs(a1 - (0.6f * 0.6f * 0.25f * vpW * vpH)) < 0.01f * a1);
    }
    // 6. property: every point of a random box, through a random rigid matrix and a random projection, lies inside the rectangle whose area is
    //    returned, so no triangle of the run covers more than that: the sampled points' bounding rectangle is never larger (and not by much smaller
    //    for a box of tiny depth extent)
    int cases = 0, bounded = 0;
    for (int it = 0; it < 4000; ++it) {
        const float pr[6] = {rr(0.5f, 3.0f), rr(-0.3f, 0.3f), rr(0.5f, 3.0f), rr(-0.3f, 0.3f), -1.0f, -2.0f};
        const bool ortho = (it % 7) == 0;
        float p[6];
        for (int i = 0; i < 6; ++i) p[i] = pr[i];
        if (ortho) { p[0] = rr(0.001f, 0.02f); p[1] = rr(-0.5f, 0.5f); p[2] = rr(0.001f, 0.02f); p[3] = rr(-0.5f, 0.5f); }
        Mat M = fromRotTrans(rr(-3, 3), rr(-3, 3), rr(-3, 3), rr(-2000, 2000), rr(-2000, 2000), -rr(50.0f, 20000.0f));
        const float* m[3]; rowsOf(M, m);
        float mn[3], mx[3];
        for (int k = 0; k < 3; ++k) { const float c = rr(-300, 300), e = rr(0.5f, 120); mn[k] = c - e; mx[k] = c + e; }
        const float a = ProjectedAreaPx(p, ortho, m, mn, mx, vpW, vpH);
        if (a < 0.0f) continue;
        ++cases;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        bool ok = true;
        for (int s = 0; s < 200 && ok; ++s) {
            const float v[3] = {rr(mn[0], mx[0]), rr(mn[1], mx[1]), rr(mn[2], mx[2])};
            float nx, ny;
            if (!projectPoint(p, ortho, M, v, &nx, &ny)) { ok = false; break; }  // (a point behind the eye while all corners are in front cannot happen)
            x0 = std::fmin(x0, nx); x1 = std::fmax(x1, nx); y0 = std::fmin(y0, ny); y1 = std::fmax(y1, ny);
        }
        CHECK(ok);
        const float sampled = (x1 - x0) * (y1 - y0) * 0.25f * vpW * vpH;
        if (sampled <= a * 1.0001f + 1e-3f) ++bounded;
        else std::fprintf(stderr, "case %d: sampled %.4f > bound %.4f\n", it, static_cast<double>(sampled), static_cast<double>(a));
    }
    CHECK(cases > 1500);
    CHECK(bounded == cases);
    // 7. the decision
    CHECK(!IsTiny(1.0f, 0.0f));
    CHECK(!IsTiny(1.0f, -1.0f));
    CHECK(!IsTiny(-1.0f, 2.0f));      // unknown: kept
    CHECK(IsTiny(0.0f, 2.0f));        // a point-sized box
    CHECK(IsTiny(1.99f, 2.0f));
    CHECK(!IsTiny(2.0f, 2.0f));
    CHECK(!IsTiny(1e6f, 2.0f));
    // 8. the census buckets
    CHECK(Bucket(-1.0f) == -1);
    CHECK(Bucket(0.0f) == 0 && Bucket(0.99f) == 0);
    CHECK(Bucket(1.0f) == 1 && Bucket(1.99f) == 1);
    CHECK(Bucket(2.0f) == 2 && Bucket(3.9f) == 2);
    CHECK(Bucket(4.0f) == 3 && Bucket(8.0f) == 4 && Bucket(16.0f) == 5 && Bucket(32.0f) == 6);
    CHECK(Bucket(63.9f) == 6 && Bucket(64.0f) == 7 && Bucket(1e9f) == 7);
    CHECK(Bucket(NAN) == -1);
    std::printf("vgx_tinyrun_test: %d checks, %d failures (%d random cases measured)\n", g_n, g_fail, cases);
    return g_fail ? 1 : 0;
}
