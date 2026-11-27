// horizon25 (11 Nov): HORIZON CULLING geometry. Header only, no game types, so the host test (tests/horizon.cpp) runs it as the game does.
//
// An occluder is a ball B(C, R) known to lie wholly inside an opaque solid (see FitOccluder: the inscribed ball of a planet's collision mesh).
// From the camera E, an object whose bounding sphere S(P, q) lies wholly inside the shadow of B can never be seen: every ray from E to a
// point X of S crosses B (so it crosses the solid's surface, which is opaque) before it reaches X.
//
// Proof. Let d = |EC|, alpha = asin(R / d), T = sqrt(d^2 - R^2). A ray from E at angle phi <= alpha from EC enters B at
// t_in = d cos(phi) - sqrt(R^2 - d^2 sin^2(phi)); t_in grows with phi, so t_in <= T (the tangent length, reached at phi = alpha).
//   (a) every point of S is at least T from E:            |EP| - q >= T          (then t_X >= T >= t_in: X is beyond the entry point)
//   (b) every direction to a point of S is within alpha - m of EC:   theta + beta <= alpha - m,
//       theta = angle(EP, EC), beta = asin(q / |EP|) (the angular radius of S).   (then every ray hits B)
// m (the margin, radians) covers a camera or an object that moved since the judgement and float rounding. Checked against a brute-force
// segment test over millions of random cases in tests/horizon.cpp: no case is culled that has a visible sample point.
#pragma once

#include <cmath>
#include <cstdint>

namespace port {
namespace horizon {

struct Cone {
    bool ok = false;
    float ex = 0, ey = 0, ez = 0;     // the camera
    float ax = 0, ay = 0, az = 0;     // unit vector E -> C
    float d = 0, tang = 0;            // |EC|, tangent length sqrt(d^2 - R^2)
    float cosIn = 1, sinIn = 0;       // cos / sin of (alpha - enter margin): a visible object is hidden when it is inside this cone
    float cosOut = 1, sinOut = 0;     // cos / sin of (alpha - exit margin): a hidden object stays hidden while it is inside this one
};

// The cone of ball (C, R) seen from E. enterMargin >= exitMargin >= 0 (radians). Not ok: the camera is inside (or too near) the ball, or the
// margin leaves no cone.
inline Cone MakeCone(const float E[3], const float C[3], float R, float enterMargin, float exitMargin) {
    Cone k;
    const float vx = C[0] - E[0], vy = C[1] - E[1], vz = C[2] - E[2];
    const float d2 = vx * vx + vy * vy + vz * vz;
    if (!(R > 0.0f) || !(d2 > 0.0f)) {
        return k;
    }
    const float d = std::sqrt(d2);
    if (!(d > R * 1.02f)) {
        return k;
    }
    const float alpha = std::asin(R / d);
    const float aIn = alpha - enterMargin, aOut = alpha - exitMargin;
    if (!(aIn > 0.005f) || !(aOut >= aIn)) {
        return k;
    }
    k.ex = E[0]; k.ey = E[1]; k.ez = E[2];
    k.ax = vx / d; k.ay = vy / d; k.az = vz / d;
    k.d = d;
    k.tang = std::sqrt(d2 - R * R);
    k.cosIn = std::cos(aIn);  k.sinIn = std::sin(aIn);
    k.cosOut = std::cos(aOut); k.sinOut = std::sin(aOut);
    k.ok = true;
    return k;
}

// Is the sphere (P, q) wholly hidden behind the ball? was: it was hidden at the last judgement (hysteresis: the looser exit cone).
inline bool Hidden(const Cone& k, const float P[3], float q, bool was) {
    const float vx = P[0] - k.ex, vy = P[1] - k.ey, vz = P[2] - k.ez;
    const float e2 = vx * vx + vy * vy + vz * vz;
    const float tq = k.tang + q;
    if (!(e2 > tq * tq)) {
        return false;  // (a): some point of the sphere may be nearer than the tangent length
    }
    const float e = std::sqrt(e2);
    const float s = q / e;
    const float sinA = was ? k.sinOut : k.sinIn, cosA = was ? k.cosOut : k.cosIn;
    if (!(s < sinA)) {
        return false;  // the sphere looks wider than the cone
    }
    const float cb = std::sqrt(1.0f - s * s);
    const float cosLim = cosA * cb + sinA * s;  // cos(a - beta)
    return vx * k.ax + vy * k.ay + vz * k.az >= cosLim * e;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The occluder from a triangle mesh (a planet's collision), in the mesh's own space. The ball is centred on a point C inside the solid:
//   the winding number of the mesh at C must be 1 (the mesh is a closed surface with outward normals around C: an open mesh, a hollow
//   planet (winding 0), a doubled shell (2) or inward normals (-1) are not occluders), and the radius is the least distance from C to any
//   triangle (so the ball touches no surface: it lies wholly inside the solid). C is moved to enlarge that distance; a step shorter than the
//   current distance cannot cross a surface, so the winding number stays 1.
// ---------------------------------------------------------------------------------------------------------------------------------
struct Fit {
    bool ok = false;
    const char* why = "";
    double c[3] = {0, 0, 0};
    double dmin = 0;       // the least distance from c to a triangle
    double winding = 0;
    double extent = 0;     // the largest distance of a vertex from the bounding box centre
    int tris = 0, used = 0;
};

namespace detail {
inline double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Sub(const double a[3], const double b[3], double o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
inline void Cross(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
// The point of triangle (a, b, c) nearest p (Ericson, Real-Time Collision Detection 5.1.5). out gets the point.
inline void ClosestOnTriangle(const double p[3], const double a[3], const double b[3], const double c[3], double out[3]) {
    double ab[3], ac[3], ap[3];
    Sub(b, a, ab); Sub(c, a, ac); Sub(p, a, ap);
    const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; return; }
    double bp[3]; Sub(p, b, bp);
    const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { out[0] = b[0]; out[1] = b[1]; out[2] = b[2]; return; }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double v = d1 / (d1 - d3);
        for (int i = 0; i < 3; i++) out[i] = a[i] + v * ab[i];
        return;
    }
    double cp[3]; Sub(p, c, cp);
    const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; return; }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double w = d2 / (d2 - d6);
        for (int i = 0; i < 3; i++) out[i] = a[i] + w * ac[i];
        return;
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; i++) out[i] = b[i] + w * (c[i] - b[i]);
        return;
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}
// signed solid angle of triangle (a, b, c) seen from p (positive when the triangle's anticlockwise side faces p's side as for an outward
// normal around p: for a closed outward mesh the sum over all triangles is 4 pi for p inside)
inline double SolidAngle(const double p[3], const double a0[3], const double b0[3], const double c0[3]) {
    double a[3], b[3], c[3];
    Sub(a0, p, a); Sub(b0, p, b); Sub(c0, p, c);
    const double la = std::sqrt(Dot(a, a)), lb = std::sqrt(Dot(b, b)), lc = std::sqrt(Dot(c, c));
    double bc[3]; Cross(b, c, bc);
    const double num = Dot(a, bc);
    const double den = la * lb * lc + Dot(a, b) * lc + Dot(a, c) * lb + Dot(b, c) * la;
    return 2.0 * std::atan2(num, den);
}
}  // namespace detail

// get(i, v, n): fills v[3][3] (the vertices) and n[3] (the stored face normal, or zeros) of triangle i, false when it is unusable
// (degenerate). The triangles are oriented by the normal (flipped when the stored normal says so), so a mesh whose winding order is
// arbitrary but whose normals point outward is accepted.
template <class Get>
inline Fit FitOccluder(int count, Get get, int maxRefine = 6) {
    Fit f;
    f.tris = count;
    if (count < 8) { f.why = "too few triangles"; return f; }
    struct Tri { double v[3][3]; };
    // The mesh is walked from `get` each pass (no copy: a 20000-triangle planet would cost a megabyte).
    double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300}, cen[3] = {0, 0, 0}, areaSum = 0;
    int used = 0;
    for (int i = 0; i < count; i++) {
        double v[3][3], n[3];
        if (!get(i, v, n)) continue;
        ++used;
        double e1[3], e2[3], cr[3];
        detail::Sub(v[1], v[0], e1); detail::Sub(v[2], v[0], e2); detail::Cross(e1, e2, cr);
        const double area = 0.5 * std::sqrt(detail::Dot(cr, cr));
        for (int k = 0; k < 3; k++) {
            for (int j = 0; j < 3; j++) {
                if (v[k][j] < mn[j]) mn[j] = v[k][j];
                if (v[k][j] > mx[j]) mx[j] = v[k][j];
                cen[j] += area * v[k][j] / 3.0;
            }
        }
        areaSum += area;
    }
    f.used = used;
    if (used < count - count / 20) { f.why = "more than 5% degenerate triangles"; return f; }
    if (!(areaSum > 0)) { f.why = "no area"; return f; }
    double bb[3] = {0.5 * (mn[0] + mx[0]), 0.5 * (mn[1] + mx[1]), 0.5 * (mn[2] + mx[2])};
    double ext = 0;
    for (int j = 0; j < 3; j++) { if (mx[j] - mn[j] > ext) ext = mx[j] - mn[j]; }
    f.extent = 0.5 * ext;
    // candidates: the box centre, the area centroid, the origin
    double cand[3][3] = {{bb[0], bb[1], bb[2]}, {cen[0] / areaSum, cen[1] / areaSum, cen[2] / areaSum}, {0, 0, 0}};
    auto Eval = [&](const double p[3], double* pDmin, double nearest[3], double* pWind) {
        double dm = 1e300, w = 0;
        for (int i = 0; i < count; i++) {
            double v[3][3], n[3];
            if (!get(i, v, n)) continue;
            double e1[3], e2[3], cr[3];
            detail::Sub(v[1], v[0], e1); detail::Sub(v[2], v[0], e2); detail::Cross(e1, e2, cr);
            const bool flip = detail::Dot(cr, n) < 0.0;
            double q[3];
            detail::ClosestOnTriangle(p, v[0], v[1], v[2], q);
            double dv[3]; detail::Sub(p, q, dv);
            const double dd = detail::Dot(dv, dv);
            if (dd < dm) { dm = dd; if (nearest) { nearest[0] = q[0]; nearest[1] = q[1]; nearest[2] = q[2]; } }
            if (pWind) {
                const double sa = flip ? detail::SolidAngle(p, v[0], v[2], v[1]) : detail::SolidAngle(p, v[0], v[1], v[2]);
                // the stored normal decides the side; without one (zero normal) the vertex order does
                const bool hasN = detail::Dot(n, n) > 1e-12;
                w += hasN ? sa : detail::SolidAngle(p, v[0], v[1], v[2]);
            }
        }
        *pDmin = std::sqrt(dm);
        if (pWind) *pWind = w / (4.0 * 3.14159265358979323846);
    };
    // the best candidate by (winding ~ 1, then the largest distance)
    double best = -1, bestW = 0, bestC[3] = {0, 0, 0};
    double lastWind = 0;
    for (int k = 0; k < 3; k++) {
        double dm, w;
        Eval(cand[k], &dm, nullptr, &w);
        lastWind = w;
        if (w > 0.95 && w < 1.05 && dm > best) { best = dm; bestW = w; bestC[0] = cand[k][0]; bestC[1] = cand[k][1]; bestC[2] = cand[k][2]; }
    }
    if (best < 0) {
        f.winding = lastWind;
        f.why = (lastWind < -0.95 && lastWind > -1.05) ? "inward normals (winding -1)"
                : (lastWind > 1.95) ? "doubled or nested shells (winding >= 2)"
                : (lastWind > -0.05 && lastWind < 0.05) ? "centre not inside the solid (winding 0: hollow, open or off-centre)"
                : "not a closed mesh around its centre (winding not 1)";
        return f;
    }
    // enlarge the distance: step away from the nearest surface point; a step under the distance cannot cross a surface
    double step = best * 0.5;
    for (int it = 0; it < maxRefine && step > 1e-6 * (f.extent > 0 ? f.extent : 1.0); it++) {
        double dm, nearest[3];
        Eval(bestC, &dm, nearest, nullptr);
        double away[3]; detail::Sub(bestC, nearest, away);
        const double la = std::sqrt(detail::Dot(away, away));
        if (!(la > 1e-12)) break;
        const double s = step < 0.9 * dm ? step : 0.9 * dm;
        double t[3] = {bestC[0] + away[0] / la * s, bestC[1] + away[1] / la * s, bestC[2] + away[2] / la * s}, dm2;
        Eval(t, &dm2, nullptr, nullptr);
        if (dm2 > dm) { bestC[0] = t[0]; bestC[1] = t[1]; bestC[2] = t[2]; best = dm2; } else { step *= 0.5; }
    }
    double dmF, wF;
    Eval(bestC, &dmF, nullptr, &wF);  // (the final winding is checked again, a safety net against a step that crossed a thin sliver)
    if (!(wF > 0.95 && wF < 1.05)) { f.winding = wF; f.why = "winding changed while centring"; return f; }
    f.c[0] = bestC[0]; f.c[1] = bestC[1]; f.c[2] = bestC[2];
    f.dmin = dmF; f.winding = wF;
    (void)bestW;
    if (!(f.dmin > 0.05 * f.extent)) { f.why = "inscribed ball under 5% of the extent (thin or tiny)"; return f; }
    f.ok = true;
    return f;
}

}  // namespace horizon
}  // namespace port
