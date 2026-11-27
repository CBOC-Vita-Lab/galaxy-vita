// gpugen26: host test of platform/vgx/src/vgx_fillshrink.h (GXV_SHADOWFILL_SHRINK). Pure maths: the silhouette fill quad's four corners moved to a pixel window must rasterise
// exactly the window's pixels with the texture coordinate (X / efbW, Y / efbH) the full quad gives at each of them. tools/test_vgx_fillshrink.sh builds it with ASan + UBSan.
#include "../platform/vgx/src/vgx_fillshrink.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace vgx::fillshrink;

namespace {
int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  [FAIL] %s\n", what); }
}
#define CHECKF(c, ...) do { char _b[200]; std::snprintf(_b, sizeof _b, __VA_ARGS__); Check((c), _b); } while (0)

struct Scene {
    float P[16];  // column-major
    float M[12];
    float T[8];
    int efbW, efbH;
    Xf xf() const { return Xf{P, M, T, 0.0, 0.0, static_cast<double>(efbW), static_cast<double>(efbH)}; }
};
// The game's own fill: positions in EFB pixels (0..W, 0..H), an orthographic projection with the Y flip the front end folds in, a texture matrix scaling the coordinate to 0..1.
Scene Standard(int W, int H) {
    Scene s{};
    s.efbW = W; s.efbH = H;
    s.P[0] = 2.0f / W;   s.P[12] = -1.0f;    // x: 0..W -> -1..1
    s.P[5] = 2.0f / H;   s.P[13] = -1.0f;    // y: 0..H -> -1..1 (EFB row 0 at NDC -1: the flipped projection)
    s.P[10] = 1.0f;
    s.P[15] = 1.0f;
    s.M[0] = s.M[5] = s.M[10] = 1.0f;
    s.T[0] = 1.0f / W; s.T[5] = 1.0f / H;    // S = u / W, T = v / H (u, v are pixel positions)
    return s;
}
void FullQuad(const Scene& s, Vtx v[4]) {
    const float W = static_cast<float>(s.efbW), H = static_cast<float>(s.efbH);
    const float c[4][2] = {{0, 0}, {W, 0}, {W, H}, {0, H}};
    for (int i = 0; i < 4; ++i) { v[i].p[0] = c[i][0]; v[i].p[1] = c[i][1]; v[i].p[2] = 0.0f; v[i].uv[0] = c[i][0]; v[i].uv[1] = c[i][1]; }
}
// rasterise two triangles (0 1 2, 0 2 3) at pixel centres; returns the number covered, and for each covered pixel the interpolated (S, T) (affine on the screen: equal w)
struct Cov { bool in[1100][700]; double S[1100][700], T[1100][700]; int count; };
void Raster(const Scene& s, const Vtx v[4], Cov& c, int W, int H) {
    const Xf f = s.xf();
    double X[4], Y[4], Sx[4], Tx[4], w;
    for (int i = 0; i < 4; ++i) { ScreenOf(f, v[i].p, X[i], Y[i], w); TexOf(f, v[i].uv, Sx[i], Tx[i]); }
    c.count = 0;
    const int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            c.in[x][y] = false;
            const double px = x + 0.5, py = y + 0.5;
            for (int t = 0; t < 2 && !c.in[x][y]; ++t) {
                const int a = tri[t][0], b = tri[t][1], d = tri[t][2];
                const double det = (X[b] - X[a]) * (Y[d] - Y[a]) - (X[d] - X[a]) * (Y[b] - Y[a]);
                if (std::fabs(det) < 1e-12) continue;
                const double l1 = ((px - X[a]) * (Y[d] - Y[a]) - (X[d] - X[a]) * (py - Y[a])) / det;
                const double l2 = ((X[b] - X[a]) * (py - Y[a]) - (px - X[a]) * (Y[b] - Y[a])) / det;
                const double l0 = 1.0 - l1 - l2;
                if (l0 >= 0 && l1 >= 0 && l2 >= 0) {
                    c.in[x][y] = true;
                    c.S[x][y] = l0 * Sx[a] + l1 * Sx[b] + l2 * Sx[d];
                    c.T[x][y] = l0 * Tx[a] + l1 * Tx[b] + l2 * Tx[d];
                    ++c.count;
                }
            }
        }
}
Cov g_a, g_b;

void TestExact(const char* name, const Scene& s, int wx, int wy, int ww, int wh) {
    Vtx in[4], out[4];
    FullQuad(s, in);
    const bool ok = Shrink(s.xf(), s.efbW, s.efbH, in, wx, wy, ww, wh, out);
    CHECKF(ok, "%s: Shrink succeeds", name);
    if (!ok) return;
    Raster(s, in, g_a, s.efbW, s.efbH);
    Raster(s, out, g_b, s.efbW, s.efbH);
    int inside = 0, outside = 0, badTex = 0, missing = 0;
    const int x0 = std::max(wx, 0), y0 = std::max(wy, 0), x1 = std::min(wx + ww, s.efbW), y1 = std::min(wy + wh, s.efbH);
    for (int y = 0; y < s.efbH; ++y)
        for (int x = 0; x < s.efbW; ++x) {
            const bool inWin = x >= x0 && x < x1 && y >= y0 && y < y1;
            if (g_b.in[x][y] && !inWin) ++outside;
            if (inWin) {
                ++inside;
                if (!g_b.in[x][y]) ++missing;
                else if (std::fabs(g_b.S[x][y] - g_a.S[x][y]) > 1e-6 || std::fabs(g_b.T[x][y] - g_a.T[x][y]) > 1e-6) ++badTex;
            }
        }
    CHECKF(g_a.count == s.efbW * s.efbH, "%s: the full quad covers every pixel (%d)", name, g_a.count);
    CHECKF(outside == 0, "%s: no pixel outside the window is drawn (%d)", name, outside);
    CHECKF(missing == 0, "%s: every pixel of the window is drawn (%d missing of %d)", name, missing, inside);
    CHECKF(badTex == 0, "%s: the same texture coordinate at every pixel (%d differ)", name, badTex);
    CHECKF(g_b.count == inside, "%s: the new quad covers exactly the window's %d pixels (%d)", name, inside, g_b.count);
}
}  // namespace

int main() {
    std::printf("fillshrink\n");
    const Scene s = Standard(640, 456);
    TestExact("inner window", s, 200, 100, 120, 90);
    TestExact("corner window", s, 0, 0, 50, 40);
    TestExact("far corner", s, 600, 420, 40, 36);
    TestExact("one pixel", s, 300, 200, 1, 1);
    TestExact("window beyond the screen is cut", s, 580, 400, 200, 200);
    TestExact("whole screen is the same quad", s, 0, 0, 640, 456);
    {  // another EFB size, a scaled and shifted model-view, a different viewport size
        Scene t = Standard(608, 346);
        const float W = 608, H = 346;
        t.M[0] = 0.5f; t.M[3] = 10.0f; t.M[5] = 0.5f; t.M[7] = -4.0f;   // e = 0.5 p + (10, -4); the projection reads e as the screen position
        Vtx in[4], out[4];
        const float c[4][2] = {{0, 0}, {W, 0}, {W, H}, {0, H}};
        for (int i = 0; i < 4; ++i) {
            in[i].p[0] = (c[i][0] - 10.0f) / 0.5f;
            in[i].p[1] = (c[i][1] + 4.0f) / 0.5f;
            in[i].p[2] = 3.0f;
            in[i].uv[0] = c[i][0]; in[i].uv[1] = c[i][1];
        }
        double X0, Y0, w0, X2, Y2, w2;
        ScreenOf(t.xf(), in[0].p, X0, Y0, w0);
        ScreenOf(t.xf(), in[2].p, X2, Y2, w2);
        CHECKF(std::fabs(X0) < 1e-3 && std::fabs(X2 - W) < 1e-3 && std::fabs(Y0) < 1e-3 && std::fabs(Y2 - H) < 1e-3, "scaled scene: the model corners land on the screen corners (%g %g %g %g)", X0, Y0, X2, Y2);
        const bool ok = Shrink(t.xf(), 608, 346, in, 100, 50, 80, 60, out);
        Check(ok, "scaled scene: Shrink succeeds");
        if (ok) {
            double X, Y, w, S, T;
            double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
            for (int i = 0; i < 4; ++i) {
                ScreenOf(t.xf(), out[i].p, X, Y, w);
                TexOf(t.xf(), out[i].uv, S, T);
                minX = std::fmin(minX, X); maxX = std::fmax(maxX, X); minY = std::fmin(minY, Y); maxY = std::fmax(maxY, Y);
                CHECKF(std::fabs(S - X / 608) < 1e-6 && std::fabs(T - Y / 346) < 1e-6, "scaled scene corner %d: the coordinate is X / W, Y / H", i);
            }
            CHECKF(std::fabs(minX - 100) < 1e-3 && std::fabs(maxX - 180) < 1e-3 && std::fabs(minY - 50) < 1e-3 && std::fabs(maxY - 110) < 1e-3, "scaled scene: the corners are the window's (%g %g %g %g)", minX, maxX, minY, maxY);
        }
    }
    {  // vertex order does not matter (the same corners, listed 2 0 3 1)
        Vtx in[4], perm[4], out[4];
        FullQuad(s, in);
        const int o[4] = {2, 0, 3, 1};
        for (int i = 0; i < 4; ++i) perm[i] = in[o[i]];
        const bool ok = Shrink(s.xf(), s.efbW, s.efbH, perm, 100, 100, 100, 100, out);
        Check(ok, "any vertex order is accepted");
        if (ok) {
            double X, Y, w;
            ScreenOf(s.xf(), out[0].p, X, Y, w);
            Check(std::fabs(X - 200) < 1e-3 && std::fabs(Y - 200) < 1e-3, "and the corner stays the corner it was (the high-x, high-y one)");
        }
    }
    {  // refusals
        Vtx in[4], out[4];
        FullQuad(s, in);
        Check(!Shrink(s.xf(), s.efbW, s.efbH, in, 10, 10, 0, 5, out), "an empty window is refused");
        Check(!Shrink(s.xf(), s.efbW, s.efbH, in, 700, 10, 20, 20, out), "a window wholly outside the rectangle is refused");
        Check(!Shrink(s.xf(), 0, s.efbH, in, 10, 10, 5, 5, out), "a zero EFB width is refused");
        Scene persp = s;
        persp.P[7] = 0.001f;  // w now depends on y: not an ortho fill
        Check(!Shrink(persp.xf(), s.efbW, s.efbH, in, 10, 10, 50, 50, out), "a projection whose w depends on the position is refused");
        Scene sing = s;
        sing.T[0] = sing.T[1] = 0.0f;  // the texture matrix cannot be inverted
        Check(!Shrink(sing.xf(), s.efbW, s.efbH, in, 10, 10, 50, 50, out), "a singular texture matrix is refused");
        Scene rot = s;
        rot.P[1] = 0.002f;  // x leaks into clip y: the rectangle is no longer axis-aligned
        Check(!Shrink(rot.xf(), s.efbW, s.efbH, in, 10, 10, 50, 50, out), "a sheared projection is refused (corners not axis-aligned)");
        Vtx tri[4] = {in[0], in[1], in[2], in[2]};
        Check(!Shrink(s.xf(), s.efbW, s.efbH, tri, 10, 10, 50, 50, out), "a degenerate quad (a repeated corner) is refused");
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
