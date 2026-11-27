// port/colshadow_clip.h (GXV_COLSHADOW_CLIP, dreadblend25): pure tests, no GX.
//  - wholly-inside polygons come back unchanged (bit-identical vertices, same order); wholly-outside ones return 0
//  - the clipped polygon covers exactly the part of the original inside the box (dense point sampling: points of the original inside the box are inside the clip, and
//    no point of the clip lies outside the original or the box), uv linear in position is preserved (the new vertices lie on the plane, uv = the linear map)
//  - watertight: two triangles sharing an edge, walked in either direction, cut it at bit-identical points
//  - negative controls: a Cross without the endpoint ordering is NOT bit-identical (the test sees it); a clip to a smaller box loses sample points (the test sees it)
#include "port/colshadow_clip.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace port::colshadow;
static unsigned failures;
static void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
static std::mt19937 rng(12345);
static float R(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
// uv = 0.5 + s*(x,y), z = 2x - y + 3 (a plane)
static ClipVtx Mk(float x, float y, float s) { return ClipVtx{x, y, 2 * x - y + 3, 0.5f + s * x, 0.5f - s * y}; }
static bool InTri(const ClipVtx* t, int n, double px, double py) {  // convex polygon, any winding
    int pos = 0, neg = 0;
    for (int i = 0; i < n; ++i) {
        const ClipVtx &a = t[i], &b = t[(i + 1) % n];
        const double c = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        pos += c > 0;
        neg += c < 0;
    }
    return pos == 0 || neg == 0;
}
// A Cross WITHOUT the endpoint ordering (negative control)
static ClipVtx NaiveCross(const ClipVtx& a, const ClipVtx& b, int coord, float bound) {
    const float ca = coord ? a.v : a.u, cb = coord ? b.v : b.u;
    const float t = (bound - ca) / (cb - ca);
    ClipVtx r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 0, 0};
    r.u = coord ? a.u + (b.u - a.u) * t : bound;
    r.v = coord ? bound : a.v + (b.v - a.v) * t;
    return r;
}
static bool Same(const ClipVtx& a, const ClipVtx& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
int main() {
    const float s = 2.0f, lo = -1.0f / 160, hi = 1.0f + 1.0f / 160;
    int inside = 0, outside = 0, cut = 0, badCover = 0, badPlane = 0;
    double worstPlane = 0;
    for (int it = 0; it < 20000; ++it) {
        const float span = it % 3 == 0 ? 3.0f : it % 3 == 1 ? 0.6f : 0.15f;
        const float cx = R(-0.8f, 0.8f), cy = R(-0.8f, 0.8f);
        ClipVtx t[3];
        for (auto& v : t) v = Mk(cx + R(-span, span), cy + R(-span, span), s);
        ClipVtx out[kClipMax];
        const int m = ClipUvBox(t, 3, lo, hi, out);
        if (m == 3 && std::memcmp(out, t, sizeof t) == 0) {
            ++inside;
            bool all = true;
            for (auto& v : t) all &= v.u >= lo && v.u <= hi && v.v >= lo && v.v <= hi;
            if (!all) ++badCover;
            continue;
        }
        if (m == 0) {
            ++outside;
            for (int k = 0; k < 40; ++k) {  // no point of the triangle lies inside the box (area > 0 part)
                const double a = R(0, 1), b = R(0, 1) * (1 - a), c = 1 - a - b;
                const double u = a * t[0].u + b * t[1].u + c * t[2].u, v = a * t[0].v + b * t[1].v + c * t[2].v;
                if (u > lo + 1e-4 && u < hi - 1e-4 && v > lo + 1e-4 && v < hi - 1e-4) { ++badCover; break; }
            }
            continue;
        }
        ++cut;
        for (int i = 0; i < m; ++i) {  // vertices on the plane z = 2x - y + 3 and uv = the linear map
            const double ez = std::fabs(out[i].z - (2 * out[i].x - out[i].y + 3));
            const double eu = std::fabs(out[i].u - (0.5 + s * out[i].x)), ev = std::fabs(out[i].v - (0.5 - s * out[i].y));
            const double e = std::fmax(ez, std::fmax(eu, ev));
            if (e > worstPlane) worstPlane = e;
            if (e > 2e-4) ++badPlane;
        }
        for (int k = 0; k < 60; ++k) {  // random points of the screen: in original AND box <=> in the clip
            const double px = R(-2, 2), py = R(-2, 2);
            const double u = 0.5 + s * px, v = 0.5 - s * py;
            const bool inBox = u > lo + 1e-3 && u < hi - 1e-3 && v > lo + 1e-3 && v < hi - 1e-3;
            const bool inBoxOut = u < lo - 1e-3 || u > hi + 1e-3 || v < lo - 1e-3 || v > hi + 1e-3;
            const bool o = InTri(t, 3, px, py);
            // (a point very near an edge of the original is skipped by the float tolerance of InTri: only strict interior / exterior points are counted)
            const bool c = InTri(out, m, px, py);
            if (inBox && o && !c) ++badCover;     // lost a visible point
            if (c && inBoxOut) ++badCover;  // a point outside the box that the clip keeps
        }
    }
    std::printf("polygons: inside(unchanged)=%d outside(dropped)=%d cut=%d  worst plane/uv error of a new vertex=%.3g\n", inside, outside, cut, worstPlane);
    Check(inside > 100 && outside > 1000 && cut > 1000, "the random set holds all three kinds in numbers");
    Check(badCover == 0, "wholly inside unchanged; wholly outside has no area in the box; a cut keeps every box point of the original");
    Check(badPlane == 0, "new vertices lie on the polygon's plane, uv exactly the linear map (float rounding only)");
    // watertight: neighbour triangles sharing the edge A-B, walked A->B and B->A
    int neq = 0, naiveNeq = 0, edges = 0;
    for (int it = 0; it < 20000; ++it) {
        ClipVtx A = Mk(R(-1.5f, 1.5f), R(-1.5f, 1.5f), s), B = Mk(R(-1.5f, 1.5f), R(-1.5f, 1.5f), s);
        ClipVtx C = Mk(R(-1.5f, 1.5f), R(-1.5f, 1.5f), s), D = Mk(R(-1.5f, 1.5f), R(-1.5f, 1.5f), s);
        ClipVtx t1[3] = {A, B, C}, t2[3] = {B, A, D};
        ClipVtx o1[kClipMax], o2[kClipMax];
        const int m1 = ClipUvBox(t1, 3, lo, hi, o1), m2 = ClipUvBox(t2, 3, lo, hi, o2);
        if (m1 < 3 || m2 < 3) continue;
        for (int side = 0; side < 4; ++side) {  // every border the edge crosses
            const int coord = side >> 1;
            const float bound = (side & 1) ? hi : lo;
            const float ca = coord ? A.v : A.u, cb = coord ? B.v : B.u;
            if (!((ca < bound) != (cb < bound))) continue;
            ++edges;
            // the cut point as each polygon computes it: the vertex in o1 / o2 whose coordinate equals the bound and that lies on the edge A-B (found by the line)
            const ClipVtx p = detail::Cross(A, B, coord, bound), q = detail::Cross(B, A, coord, bound);
            if (!Same(p, q)) ++neq;
            if (!Same(NaiveCross(A, B, coord, bound), NaiveCross(B, A, coord, bound))) ++naiveNeq;
        }
    }
    std::printf("shared edges cut: %d  ordered-Cross mismatches=%d  naive-Cross mismatches=%d\n", edges, neq, naiveNeq);
    Check(edges > 2000 && neq == 0, "a shared edge is cut at bit-identical points from either direction (watertight)");
    Check(naiveNeq > 0, "NEG1: a Cross without the endpoint ordering is not bit-identical (the test would see it)");
    // negative control: a clip to a smaller box loses visible points
    int lost = 0;
    for (int it = 0; it < 2000; ++it) {
        ClipVtx t[3] = {Mk(R(-1, 1), R(-1, 1), s), Mk(R(-1, 1), R(-1, 1), s), Mk(R(-1, 1), R(-1, 1), s)};
        ClipVtx out[kClipMax];
        const int m = ClipUvBox(t, 3, 0.3f, 0.7f, out);
        for (int k = 0; k < 40; ++k) {
            const double px = R(-0.5f, 0.5f), py = R(-0.5f, 0.5f);
            const double u = 0.5 + s * px, v = 0.5 - s * py;
            if (u > lo && u < hi && v > lo && v < hi && InTri(t, 3, px, py) && !(m >= 3 && InTri(out, m, px, py))) { ++lost; break; }
        }
    }
    std::printf("NEG2 smaller box: polygons that lost visible points=%d\n", lost);
    Check(lost > 100, "NEG2: a clip to a smaller box than the shadow's own loses visible points (the coverage test would see it)");
    Check(UvArea(nullptr, 0) == 0, "UvArea of nothing is 0");
    return failures ? 1 : 0;
}
