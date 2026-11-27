// GXV_SHADOWFILL_SHRINK (gpugen26, 12 Nov 2026): the silhouette fill quad drawn over a smaller rectangle instead of under a smaller scissor. Pure maths, no GL, no front-end
// state: vgx_draw.cpp ShadowFillPass hands it the fill's four vertices and the draw's own transforms; tests/vgx_fillshrink_test.cpp checks it.
//
// Why. MR::fillSilhouetteColor draws one full-screen quad that samples the alpha capture and darkens where the captured alpha is above a threshold. GXV_SHADOWFILL_BOUNDS
// (shadowfill, 6 Nov) already works out the EFB pixels where that can matter (N: shadow volumes, the player's silhouette) and cuts the quad's SCISSOR to them, which is exact; the
// console A/B of r95 then measured it a net cost, and the code says why: a scissor that is not the whole target makes vitaGL draw scissor-mask quads (full-target passes) around the
// draw. The same pixels can be had without any scissor by moving the quad's four corners: the quad is an axis-aligned rectangle with equal clip w, its texture coordinate is
// (X / efbW, Y / efbH) at every corner (SfeFillGeom proves both), so a smaller rectangle with corners at the window's edges and the coordinates the same formula gives there
// rasterises the very same fragments (the same coordinate at every pixel centre inside, up to float interpolation: 1e-7 of a texel) and none outside. No scissor change, no mask pass,
// the fragment shader runs over the window's pixels only.
//
// The maths: a corner with model position p = (x, y, z) goes through the draw's model-view rows M (3x4) and projection P (4x4, column-major) to clip (cx, cy, cw); the window edge
// X = vpX + (cx / cw + 1) vpW / 2 (EFB pixels). With cw the same at all four corners (the caller's check) it does not depend on x, y, so cx and cy are affine in (x, y): three
// evaluations give the 2x2 system, solved for the target (nx cw, ny cw). The texture coordinate (S, T) = (T0 u + T1 v + T2 + T3, T4 u + T5 v + T6 + T7) likewise in (u, v). The
// result is verified by mapping the float vertices forward again; any miss (singular system, a corner that does not land within 1e-5 of its target) returns false and the caller
// keeps the draw as it was.
#pragma once
#include <cmath>
#include <cstdint>

namespace vgx {
namespace fillshrink {

struct Xf {
    const float* P;  // u_proj, column-major 4x4 (the first, second and fourth rows are read: x, y, w)
    const float* M;  // u_mv, three rows of four
    const float* T;  // the texture matrix block of the coordinate: S = T[0] u + T[1] v + T[2] + T[3], T = T[4] u + T[5] v + T[6] + T[7]
    double vpX, vpY, vpW, vpH;
};
struct Vtx {
    float p[3];
    float uv[2];
};

// Forward map of one position: clip x, y, w.
inline void Clip(const Xf& f, const float p[3], double c[3]) {
    double e[3];
    for (int r = 0; r < 3; ++r)
        e[r] = static_cast<double>(f.M[r * 4]) * p[0] + static_cast<double>(f.M[r * 4 + 1]) * p[1] + static_cast<double>(f.M[r * 4 + 2]) * p[2] + f.M[r * 4 + 3];
    c[0] = f.P[0] * e[0] + f.P[4] * e[1] + f.P[8] * e[2] + f.P[12];
    c[1] = f.P[1] * e[0] + f.P[5] * e[1] + f.P[9] * e[2] + f.P[13];
    c[2] = f.P[3] * e[0] + f.P[7] * e[1] + f.P[11] * e[2] + f.P[15];
}
inline void ScreenOf(const Xf& f, const float p[3], double& X, double& Y, double& w) {
    double c[3];
    Clip(f, p, c);
    w = c[2];
    X = f.vpX + (c[0] / c[2] + 1.0) * 0.5 * f.vpW;
    Y = f.vpY + (c[1] / c[2] + 1.0) * 0.5 * f.vpH;
}
inline void TexOf(const Xf& f, const float uv[2], double& S, double& Tt) {
    S = f.T[0] * static_cast<double>(uv[0]) + f.T[1] * static_cast<double>(uv[1]) + f.T[2] + f.T[3];
    Tt = f.T[4] * static_cast<double>(uv[0]) + f.T[5] * static_cast<double>(uv[1]) + f.T[6] + f.T[7];
}

// The four corners of an axis-aligned rectangle covering at least the window [wx, wx + ww) x [wy, wy + wh) (EFB pixels, already padded by the caller) are moved to the window's
// edges (the window is cut to the rectangle the vertices span, so it never grows it). `in` are the original vertices in any order, `out` receives the new ones in the same order.
// efbW / efbH: the size the coordinate is normalised by. Returns false when anything is not as assumed (the caller then leaves the draw alone).
inline bool Shrink(const Xf& f, int efbW, int efbH, const Vtx in[4], int wx, int wy, int ww, int wh, Vtx out[4]) {
    if (efbW <= 0 || efbH <= 0 || ww <= 0 || wh <= 0 || !(f.vpW > 0.0) || !(f.vpH > 0.0)) return false;
    double X[4], Y[4], W[4];
    for (int i = 0; i < 4; ++i) {
        ScreenOf(f, in[i].p, X[i], Y[i], W[i]);
        if (!(W[i] > 0.0) || !std::isfinite(X[i]) || !std::isfinite(Y[i])) return false;
        if (std::fabs(W[i] - W[0]) > 1e-6 * W[0]) return false;
    }
    double xa = X[0], xb = X[0], ya = Y[0], yb = Y[0];
    for (int i = 1; i < 4; ++i) { xa = std::fmin(xa, X[i]); xb = std::fmax(xb, X[i]); ya = std::fmin(ya, Y[i]); yb = std::fmax(yb, Y[i]); }
    if (!(xb - xa > 1e-3) || !(yb - ya > 1e-3)) return false;
    // the window, cut to the rectangle (a window outside it: nothing to draw, the caller's other rules own that case)
    const double wx0 = std::fmax(static_cast<double>(wx), xa), wx1 = std::fmin(static_cast<double>(wx) + ww, xb);
    const double wy0 = std::fmax(static_cast<double>(wy), ya), wy1 = std::fmin(static_cast<double>(wy) + wh, yb);
    if (!(wx1 - wx0 > 0.0) || !(wy1 - wy0 > 0.0)) return false;
    // position solve: clip x, y are affine in (p.x, p.y) at the corner's own p.z
    const double w0 = W[0];
    unsigned seen = 0;  // each of the four corners (low / high x, low / high y) exactly once
    for (int i = 0; i < 4; ++i) {
        const bool hx = std::fabs(X[i] - xb) <= 1e-6 * (1.0 + std::fabs(xb)), hy = std::fabs(Y[i] - yb) <= 1e-6 * (1.0 + std::fabs(yb));
        const unsigned bit = 1u << ((hx ? 1 : 0) | (hy ? 2 : 0));
        if (seen & bit) return false;
        seen |= bit;
    }
    for (int i = 0; i < 4; ++i) {
        const bool hx = std::fabs(X[i] - xb) <= 1e-6 * (1.0 + std::fabs(xb)), lx = std::fabs(X[i] - xa) <= 1e-6 * (1.0 + std::fabs(xa));
        const bool hy = std::fabs(Y[i] - yb) <= 1e-6 * (1.0 + std::fabs(yb)), ly = std::fabs(Y[i] - ya) <= 1e-6 * (1.0 + std::fabs(ya));
        if (lx == hx || ly == hy) return false;  // not an axis-aligned corner
        const double tX = hx ? wx1 : wx0, tY = hy ? wy1 : wy0;
        const double ncx = (2.0 * (tX - f.vpX) / f.vpW - 1.0) * w0, ncy = (2.0 * (tY - f.vpY) / f.vpH - 1.0) * w0;
        const float z = in[i].p[2];
        const float o[3] = {0.0f, 0.0f, z}, ox[3] = {1.0f, 0.0f, z}, oy[3] = {0.0f, 1.0f, z};
        double c0[3], cx[3], cy[3];
        Clip(f, o, c0);
        Clip(f, ox, cx);
        Clip(f, oy, cy);
        if (std::fabs(cx[2] - c0[2]) > 1e-9 * (1.0 + std::fabs(c0[2])) || std::fabs(cy[2] - c0[2]) > 1e-9 * (1.0 + std::fabs(c0[2]))) return false;  // w depends on the position: not an ortho fill
        const double a = cx[0] - c0[0], b = cy[0] - c0[0], c = cx[1] - c0[1], d = cy[1] - c0[1];
        const double det = a * d - b * c;
        if (!(std::fabs(det) > 1e-12)) return false;
        const double rx = ncx - c0[0], ry = ncy - c0[1];
        out[i].p[0] = static_cast<float>((rx * d - b * ry) / det);
        out[i].p[1] = static_cast<float>((a * ry - rx * c) / det);
        out[i].p[2] = z;
        // texture solve: (S, T) must equal (X / efbW, Y / efbH) at the corner's new place
        const double tS = tX / efbW, tT = tY / efbH;
        const double A = f.T[0], B = f.T[1], C = f.T[4], D = f.T[5];
        const double detT = A * D - B * C;
        if (!(std::fabs(detT) > 1e-12)) return false;
        const double rs = tS - f.T[2] - f.T[3], rt = tT - f.T[6] - f.T[7];
        out[i].uv[0] = static_cast<float>((rs * D - B * rt) / detT);
        out[i].uv[1] = static_cast<float>((A * rt - rs * C) / detT);
        if (!std::isfinite(out[i].p[0]) || !std::isfinite(out[i].p[1]) || !std::isfinite(out[i].uv[0]) || !std::isfinite(out[i].uv[1])) return false;
        // verification: the float vertex, mapped forward, lands within 1e-4 of an EFB pixel and 1e-6 of a texture on its target
        double vX, vY, vW, vS, vT;
        ScreenOf(f, out[i].p, vX, vY, vW);
        TexOf(f, out[i].uv, vS, vT);
        if (std::fabs(vW - w0) > 1e-6 * w0 || std::fabs(vX - tX) > 1e-4 || std::fabs(vY - tY) > 1e-4 || std::fabs(vS - tS) > 1e-6 || std::fabs(vT - tT) > 1e-6) return false;
    }
    return true;
}

}  // namespace fillshrink
}  // namespace vgx
