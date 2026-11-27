// GXV_HSR_ORDER_TIES=4 (ties25): the geometry proofs behind the tighter tie rule. See vgx_hsrorder.h (Plan, TieOracle) for where it is used.
//
// A plain alpha-tested draw T that comes before a plain opaque draw O of the same transform may be moved behind O unless the two can give the same
// depth at one pixel (then the draw order decides who wins). This file decides "cannot", for STATIC geometry only (a static buffer: the same triangles
// every picture), with proofs that hold for the hardware's finite precision, not only for exact arithmetic:
//   * invisible: a draw whose bounding box is wholly behind the eye draws nothing.
//   * rect:   the screen rectangles of the two bounding boxes (8 corners, all in front of the eye) are separated by more than 2 pixels.
//   * depth:  the depth intervals of the two boxes (the depth of a corner; depth is monotone along any line in front of the eye, so the extremes of a
//             box are its corners) are separated by more than eta (default 2^-18 of the depth range; the depth buffer's step is 2^-24).
//   * box:    the boxes, in model space, are further apart than thr (below).
//   * gap:    the triangles of T and of O, in model space, are all further apart than thr (a cached, budgeted, exact computation: Job below).
// thr is the model-space distance that guarantees a depth difference of more than eta at every pixel the two draws can share. Two surface points at one
// pixel lie on one view ray; with the projection's z and w depending on the eye z only (checked; else no proof), depth is a Moebius function of the eye
// z, whose slope is D / w^2 (D = Az * Bw - Bz * Aw): for points a distance s apart (eye space) on one ray, |dz_eye| >= s / K (K = sqrt(1 + kx^2 + ky^2),
// the ray slope bound over the scissor), and |d depth| >= |D| |dz_eye| / wmax^2, wmax the largest w of the two boxes. s >= sigma_min * s_model
// (sigma_min of the 3 x 3 of the position matrix, bounded below by 1 / ||N^-1||_F). So s_model > thr = eta_ndc * K * wmax^2 / (|D| * sigma_min) suffices
// (eta_ndc = 2 eta / depth range). Triangles that touch, cross or overlap coplanar have gap 0: pinned (an intersection LINE of two surfaces has pixels
// of equal depth, and across the line the later draw wins on LEQUAL: the order decides there, so they are never freed). Dynamic (stream) draws and
// draws whose geometry is not known yet stay pinned: the answer is worked out off the hot path (Pump), a budget of work units per picture.
#pragma once
#include "vgx_hsrorder.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vgx {
namespace hsr {

// ------------------------------------------------------------------------------------------------ what the geometry comes from
struct StaticView {
    const uint8_t* vtx = nullptr;
    size_t vtxBytes = 0;
    const uint16_t* idx = nullptr;
    size_t idxCount = 0;
    uint32_t epoch = 0; // changes whenever the buffer id gets new contents
};
struct GeoSource {
    virtual ~GeoSource() {}
    virtual bool Get(uint32_t bufId, StaticView& v) = 0;
};

// ------------------------------------------------------------------------------------------------ small vector maths (double)
struct V3 { double x, y, z; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double Len2(V3 a) { return Dot(a, a); }

// closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5)
inline V3 ClosestOnTri(V3 p, V3 a, V3 b, V3 c) {
    const V3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const V3 bp = p - b;
    const double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { const double v = d1 / (d1 - d3); return a + ab * v; }
    const V3 cp = p - c;
    const double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const double w = d2 / (d2 - d6); return a + ac * w; }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return b + (c - b) * w; }
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}
// squared distance between segments p1q1 and p2q2 (Ericson 5.1.9)
inline double SegSeg2(V3 p1, V3 q1, V3 p2, V3 q2) {
    const V3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const double a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
    double s, t;
    const double eps = 1e-300;
    if (a <= eps && e <= eps) return Len2(r);
    if (a <= eps) { s = 0; t = std::clamp(f / e, 0.0, 1.0); }
    else {
        const double c = Dot(d1, r);
        if (e <= eps) { t = 0; s = std::clamp(-c / a, 0.0, 1.0); }
        else {
            const double b = Dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-18 * a * e ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = std::clamp(-c / a, 0.0, 1.0); }
            else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.0, 1.0); }
        }
    }
    return Len2((p1 + d1 * s) - (p2 + d2 * t));
}
// does segment pq meet triangle abc (a pierce of its interior; touching counts; a small tolerance on the safe side)
inline bool SegHitsTri(V3 p, V3 q, V3 a, V3 b, V3 c) {
    const V3 n = Cross(b - a, c - a);
    const double nn = Len2(n);
    if (nn <= 0) return false;
    const double dp = Dot(n, p - a), dq = Dot(n, q - a);
    if ((dp > 0 && dq > 0) || (dp < 0 && dq < 0)) return false;
    if (dp == dq) return false; // (in the plane: the edge-edge and vertex-triangle distances see it)
    const double t = dp / (dp - dq);
    const V3 x = p + (q - p) * t;
    // inside test by barycentric signs, tolerance 1e-9 (a hit counted a hair outside only pins more)
    const double e0 = Dot(Cross(b - a, x - a), n), e1 = Dot(Cross(c - b, x - b), n), e2 = Dot(Cross(a - c, x - c), n);
    const double tol = -1e-9 * nn;
    return e0 >= tol && e1 >= tol && e2 >= tol;
}
inline V3 TriV(const float* t, int i) { return {t[i * 3], t[i * 3 + 1], t[i * 3 + 2]}; }
// squared distance between triangles (9 floats each); 0 when they meet
inline double TriTri2(const float* A, const float* B) {
    const V3 a[3] = {TriV(A, 0), TriV(A, 1), TriV(A, 2)}, b[3] = {TriV(B, 0), TriV(B, 1), TriV(B, 2)};
    for (int i = 0; i < 3; ++i) {
        if (SegHitsTri(a[i], a[(i + 1) % 3], b[0], b[1], b[2])) return 0.0;
        if (SegHitsTri(b[i], b[(i + 1) % 3], a[0], a[1], a[2])) return 0.0;
    }
    double best = 1e300;
    for (int i = 0; i < 3; ++i) {
        best = std::min(best, Len2(a[i] - ClosestOnTri(a[i], b[0], b[1], b[2])));
        best = std::min(best, Len2(b[i] - ClosestOnTri(b[i], a[0], a[1], a[2])));
        for (int j = 0; j < 3; ++j) best = std::min(best, SegSeg2(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3]));
    }
    return best;
}
struct Aabb {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    void Add(const double* p) { for (int i = 0; i < 3; ++i) { lo[i] = std::min(lo[i], p[i]); hi[i] = std::max(hi[i], p[i]); } }
    double Diag() const { double s = 0; for (int i = 0; i < 3; ++i) s += (hi[i] - lo[i]) * (hi[i] - lo[i]); return std::sqrt(s); }
};
inline double BoxDist(const Aabb& a, const Aabb& b) {
    double s = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = std::max({0.0, a.lo[i] - b.hi[i], b.lo[i] - a.hi[i]});
        s += d * d;
    }
    return std::sqrt(s);
}

// ------------------------------------------------------------------------------------------------ triangles of a static draw
// 9 floats a triangle, positions as the vertex shader gets them (a_pos.xyz: floats, or whole numbers for VL_QPOS: the scale is in the matrices).
// Addressing as the dump's StaticAreaPx: vtxOffset + index * stride, the indices from the draw's own range of the static index buffer.
struct RangeRef { // the fields of a static draw that say which triangles it draws
    uint32_t vbuf = 0, vtxOffset = 0, idxOffset = 0, idxCount = 0, layoutMask = 0;
    uint8_t prim = PRIM_TRIANGLES;
    RangeRef() {}
    explicit RangeRef(const DrawRec& d) : vbuf(d.vbuf), vtxOffset(d.vtxOffset), idxOffset(d.idxOffset), idxCount(d.idxCount), layoutMask(d.layoutMask), prim(d.prim) {}
};
inline bool ExtractTris(const RangeRef& d, const StaticView& v, uint32_t maxTris, std::vector<float>& out, Aabb& bb, uint64_t& hash) {
    out.clear();
    bb = Aabb{};
    hash = 1469598103934665603ull;
    if (!d.vbuf || d.prim != PRIM_TRIANGLES || d.idxCount < 3 || !v.vtx || !v.idx) return false;
    if (static_cast<size_t>(d.idxOffset) + d.idxCount > v.idxCount) return false;
    const uint32_t tris = d.idxCount / 3;
    if (tris > maxTris) return false;
    const VtxLayout lay = MakeLayout(d.layoutMask);
    const bool qpos = (d.layoutMask & VL_QPOS) != 0;
    const uint16_t* ix = v.idx + d.idxOffset;
    out.reserve(static_cast<size_t>(tris) * 9);
    for (uint32_t i = 0; i < tris * 3; ++i) {
        const size_t off = static_cast<size_t>(d.vtxOffset) + static_cast<size_t>(ix[i]) * lay.stride;
        float p[3];
        if (qpos) {
            if (off > v.vtxBytes || v.vtxBytes - off < 6) return false;
            int16_t q[3];
            std::memcpy(q, v.vtx + off, 6);
            p[0] = q[0]; p[1] = q[1]; p[2] = q[2];
        } else {
            if (off > v.vtxBytes || v.vtxBytes - off < 12) return false;
            std::memcpy(p, v.vtx + off, 12);
        }
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
        const double dp[3] = {p[0], p[1], p[2]};
        bb.Add(dp);
        for (int k = 0; k < 3; ++k) { uint32_t w; std::memcpy(&w, &p[k], 4); hash = (hash ^ w) * 1099511628211ull; }
        out.push_back(p[0]); out.push_back(p[1]); out.push_back(p[2]);
    }
    hash = (hash ^ tris) * 1099511628211ull;
    return true;
}

// ------------------------------------------------------------------------------------------------ the camera's side
struct Xf {
    bool ok = false;           // the bounds below are usable
    double R[4][4] = {};        // clip x, y, z, w = R * (p, 1)
    double sigmaMin = 0;        // lower bound of the position matrix's smallest singular value
    double dz = 0;              // depth (ndc) slope numerator D
    double K = 0;               // ray slope bound
    double dr = 1, zNear = 0;   // depth range
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
};
// P: the draw's UB_PROJ block (16 floats, column major: P[col * 4 + row]); M: the position matrix (3 rows of 4); rs: viewport, scissor, depth range
inline Xf MakeXf(const float* P, const float* M, const RenderState& rs) {
    Xf x;
    auto pc = [&](int col, int row) { return static_cast<double>(P[col * 4 + row]); };
    double Mr[3][4];
    for (int j = 0; j < 3; ++j) for (int q = 0; q < 4; ++q) Mr[j][q] = M[j * 4 + q];
    for (int r = 0; r < 4; ++r) {
        for (int q = 0; q < 4; ++q) {
            double s = 0;
            for (int j = 0; j < 3; ++j) s += pc(j, r) * Mr[j][q];
            if (q == 3) s += pc(3, r);
            x.R[r][q] = s;
        }
    }
    x.vpX = rs.vpX; x.vpY = rs.vpY; x.vpW = rs.vpW; x.vpH = rs.vpH;
    x.zNear = rs.zNear;
    x.dr = static_cast<double>(rs.zFar) - static_cast<double>(rs.zNear);
    // the structure the thr derivation needs: z and w of the clip position depend on the eye z only, x and y do not couple
    if (pc(0, 2) != 0 || pc(1, 2) != 0 || pc(0, 3) != 0 || pc(1, 3) != 0 || pc(1, 0) != 0 || pc(0, 1) != 0) return x;
    if (!(std::fabs(x.dr) > 1e-6) || !(std::fabs(pc(0, 0)) > 0) || !(std::fabs(pc(1, 1)) > 0)) return x;
    if (!(std::fabs(rs.vpW) > 0) || !(std::fabs(rs.vpH) > 0)) return x;
    x.dz = pc(2, 2) * pc(3, 3) - pc(3, 2) * pc(2, 3);
    if (!(std::fabs(x.dz) > 0)) return x;
    // smallest singular value of the 3 x 3: 1 / ||N^-1||_F
    const double a = Mr[0][0], b = Mr[0][1], c = Mr[0][2], d = Mr[1][0], e = Mr[1][1], f = Mr[1][2], g = Mr[2][0], h = Mr[2][1], i = Mr[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-30)) return x;
    const double inv[9] = {(e * i - f * h), (c * h - b * i), (b * f - c * e), (f * g - d * i), (a * i - c * g), (c * d - a * f), (d * h - e * g), (b * g - a * h), (a * e - b * d)};
    double fro = 0;
    for (int k = 0; k < 9; ++k) fro += (inv[k] / det) * (inv[k] / det);
    fro = std::sqrt(fro);
    if (!(fro > 0) || !std::isfinite(fro)) return x;
    x.sigmaMin = 1.0 / fro;
    // ray slope bound over the scissor: |x_ndc|, |y_ndc| <= xm, ym
    const double sx0 = 2.0 * (rs.scX - rs.vpX) / rs.vpW - 1.0, sx1 = 2.0 * (rs.scX + rs.scW - rs.vpX) / rs.vpW - 1.0;
    const double sy0 = 2.0 * (rs.scY - rs.vpY) / rs.vpH - 1.0, sy1 = 2.0 * (rs.scY + rs.scH - rs.vpY) / rs.vpH - 1.0;
    const double xm = std::max(std::fabs(sx0), std::fabs(sx1)) + 0.01, ym = std::max(std::fabs(sy0), std::fabs(sy1)) + 0.01;
    const double kx = (xm * std::fabs(pc(2, 3)) + std::fabs(pc(2, 0))) / std::fabs(pc(0, 0));
    const double ky = (ym * std::fabs(pc(2, 3)) + std::fabs(pc(2, 1))) / std::fabs(pc(1, 1));
    x.K = std::sqrt(1.0 + kx * kx + ky * ky);
    x.ok = std::isfinite(x.K) && std::isfinite(x.sigmaMin) && std::isfinite(x.dz);
    return x;
}
struct BoxProj {
    bool inFront = false;      // every corner has w > 0
    bool visible = true;       // some corner has w > 0
    double wmax = 0;
    double xmin = 0, xmax = 0, ymin = 0, ymax = 0, zmin = 0, zmax = 0; // pixels, window depth (valid when inFront)
};
inline BoxProj ProjectBox(const Xf& x, const Aabb& bb) {
    BoxProj r;
    r.inFront = true;
    bool any = false;
    r.xmin = r.ymin = r.zmin = 1e300;
    r.xmax = r.ymax = r.zmax = -1e300;
    for (int c = 0; c < 8; ++c) {
        const double p[3] = {c & 1 ? bb.hi[0] : bb.lo[0], c & 2 ? bb.hi[1] : bb.lo[1], c & 4 ? bb.hi[2] : bb.lo[2]};
        double cl[4];
        for (int k = 0; k < 4; ++k) cl[k] = x.R[k][0] * p[0] + x.R[k][1] * p[1] + x.R[k][2] * p[2] + x.R[k][3];
        if (cl[3] > 0) {
            any = true;
            r.wmax = std::max(r.wmax, cl[3]);
            const double nx = cl[0] / cl[3], ny = cl[1] / cl[3], nz = cl[2] / cl[3];
            const double px = x.vpX + (nx * 0.5 + 0.5) * x.vpW, py = x.vpY + (ny * 0.5 + 0.5) * x.vpH, pz = x.zNear + (nz * 0.5 + 0.5) * x.dr;
            r.xmin = std::min(r.xmin, px); r.xmax = std::max(r.xmax, px);
            r.ymin = std::min(r.ymin, py); r.ymax = std::max(r.ymax, py);
            r.zmin = std::min(r.zmin, pz); r.zmax = std::max(r.zmax, pz);
        } else r.inFront = false;
    }
    r.visible = any;
    return r;
}

// ------------------------------------------------------------------------------------------------ the pair job
// The smallest distance between the triangles of A and of B as a lower bound on a ladder (dmin * 2^L, L = 0..21; dmin = D / 2^20, D the diagonal of the union of
// the boxes): "does the triangle i of B have a triangle of A within delta" is answered by exact triangle distances on candidates from grids of A (cell sizes
// cell0 * 2^k, built when a query needs one, the finest cell no smaller than the query radius). The search DESCENDS: it starts at a delta that every
// triangle satisfies and lowers it whenever a triangle of B finds a partner within delta; a triangle that finds none at delta has none at any smaller one, so the
// cursor over B only moves forward and the whole search costs about one query per triangle of B plus one per level. The result when the cursor reaches the end at
// level L: every triangle of B is farther than dmin * 2^L from A (the bound), while some pair was within dmin * 2^(L+1): the bound is within a factor 2 of
// the minimum. Reaching level 0 with a pair still within dmin: gap 0, the pair is classified (coplanar / contact).
struct Job {
    std::vector<float> a, b;      // triangles (9 floats)
    std::vector<Aabb> ab;         // per triangle of A
    double D = 1, dmin = 0;       // union box diagonal; the finest delta
    double cell0 = 1, org[3] = {};
    struct Ent { uint64_t key; uint32_t tri; };
    struct Grid { double cell = 0; std::vector<Ent> ent; std::vector<uint32_t> big; bool built = false; };
    static constexpr int kGrids = 26;
    Grid grids[kGrids];
    int level = 0;                // the delta being tested: dmin * 2^level
    uint32_t ib = 0;              // the cursor over B
    uint64_t units = 0;
    bool done = false;
    double gap = 0;               // lower bound of the distance when done (0: touching or crossing)
    uint8_t kind = 0;             // when gap == 0: 1 coplanar, 2 contact (crossing / touching)
    bool tooBig = false;
    static constexpr int kLevels = 22;
    static uint64_t Key(int64_t x, int64_t y, int64_t z) { return (static_cast<uint64_t>(x & 0x1FFFFF) << 42) | (static_cast<uint64_t>(y & 0x1FFFFF) << 21) | static_cast<uint64_t>(z & 0x1FFFFF); }
    double Delta(int L) const { return dmin * std::ldexp(1.0, L); }
    static Aabb TriBox(const float* t) {
        Aabb bb;
        for (int i = 0; i < 3; ++i) { const double p[3] = {t[i * 3], t[i * 3 + 1], t[i * 3 + 2]}; bb.Add(p); }
        return bb;
    }
    bool Build(int k) { // the grid with cells cell0 * 2^k (false: too many large triangles at the finest cell size)
        Grid& g = grids[k];
        g.cell = cell0 * std::ldexp(1.0, k);
        g.ent.clear();
        g.big.clear();
        const uint32_t na = static_cast<uint32_t>(ab.size());
        for (uint32_t i = 0; i < na; ++i) {
            int64_t lo[3], hi[3];
            uint64_t cells = 1;
            for (int q = 0; q < 3; ++q) {
                lo[q] = static_cast<int64_t>(std::floor((ab[i].lo[q] - org[q]) / g.cell));
                hi[q] = static_cast<int64_t>(std::floor((ab[i].hi[q] - org[q]) / g.cell));
                cells *= static_cast<uint64_t>(hi[q] - lo[q] + 1);
            }
            units += 1;
            if (cells > 64) { g.big.push_back(i); continue; }
            for (int64_t x = lo[0]; x <= hi[0]; ++x) for (int64_t y = lo[1]; y <= hi[1]; ++y) for (int64_t z = lo[2]; z <= hi[2]; ++z) g.ent.push_back({Key(x, y, z), i});
            units += cells;
        }
        if (g.big.size() > 48) return false;
        std::sort(g.ent.begin(), g.ent.end(), [](const Ent& p, const Ent& q) { return p.key < q.key || (p.key == q.key && p.tri < q.tri); });
        units += g.ent.size() / 4 + 1;
        g.built = true;
        return true;
    }
    bool Init(std::vector<float>&& ta, std::vector<float>&& tb, const Aabb& ua, const Aabb& ub) {
        a = std::move(ta);
        b = std::move(tb);
        if (a.size() > b.size()) std::swap(a, b); // (the smaller set is gridded)
        Aabb u = ua;
        for (int i = 0; i < 3; ++i) { u.lo[i] = std::min(ua.lo[i], ub.lo[i]); u.hi[i] = std::max(ua.hi[i], ub.hi[i]); }
        D = std::max(u.Diag(), 1e-9);
        dmin = D * std::ldexp(1.0, -(kLevels - 2));
        for (int i = 0; i < 3; ++i) org[i] = u.lo[i];
        const uint32_t na = static_cast<uint32_t>(a.size() / 9);
        ab.resize(na);
        double sumExt = 0;
        for (uint32_t i = 0; i < na; ++i) {
            ab[i] = TriBox(&a[i * 9]);
            sumExt += std::max({ab[i].hi[0] - ab[i].lo[0], ab[i].hi[1] - ab[i].lo[1], ab[i].hi[2] - ab[i].lo[2]});
        }
        cell0 = std::max(2.0 * sumExt / std::max<uint32_t>(na, 1), D / 256.0);
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (Build(0)) break;
            cell0 *= 2.5;
            if (attempt == 7) return false;
        }
        level = kLevels - 1; // (delta >= 2 D: every triangle of B has a partner within it)
        return true;
    }
    // coplanar classification of a pair found at the finest level
    void Classify(uint32_t ia, uint32_t ib2) {
        const V3 a0 = TriV(&a[ia * 9], 0), a1 = TriV(&a[ia * 9], 1), a2 = TriV(&a[ia * 9], 2);
        const V3 b0 = TriV(&b[ib2 * 9], 0), b1 = TriV(&b[ib2 * 9], 1), b2 = TriV(&b[ib2 * 9], 2);
        V3 na = Cross(a1 - a0, a2 - a0), nb = Cross(b1 - b0, b2 - b0);
        const double la = std::sqrt(Len2(na)), lb = std::sqrt(Len2(nb));
        kind = 2;
        if (la <= 0 || lb <= 0) return;
        na = na * (1.0 / la); nb = nb * (1.0 / lb);
        const double tol = dmin * 2;
        if (std::fabs(Dot(na, nb)) > 1.0 - 1e-6 && std::fabs(Dot(na, b0 - a0)) <= tol && std::fabs(Dot(na, b1 - a0)) <= tol && std::fabs(Dot(na, b2 - a0)) <= tol) kind = 1;
    }
    // does B's triangle i have a triangle of A within delta
    bool Query(uint32_t i, double delta, uint32_t& hitA) {
        const float* t = &b[static_cast<size_t>(i) * 9];
        const Aabb q = TriBox(t);
        const double lim = delta * (1.0 + 1e-9);
        const double lim2 = lim * lim;
        auto test = [&](uint32_t ia) {
            units += 2;
            if (BoxDist(ab[ia], q) > lim) return false;
            units += 6;
            if (TriTri2(&a[static_cast<size_t>(ia) * 9], t) <= lim2) { hitA = ia; return true; }
            return false;
        };
        int k = 0;
        while (k < kGrids - 1 && cell0 * std::ldexp(1.0, k) < delta) ++k;
        if (!grids[k].built && !Build(k)) { // (the grid has too many large triangles: every triangle of A is a candidate)
            for (uint32_t ia = 0; ia < ab.size(); ++ia) if (test(ia)) return true;
            return false;
        }
        const Grid& g = grids[k];
        for (uint32_t ia : g.big) if (test(ia)) return true;
        int64_t lo[3], hi[3];
        uint64_t cells = 1;
        for (int c = 0; c < 3; ++c) {
            lo[c] = static_cast<int64_t>(std::floor((q.lo[c] - delta - org[c]) / g.cell));
            hi[c] = static_cast<int64_t>(std::floor((q.hi[c] + delta - org[c]) / g.cell));
            cells *= static_cast<uint64_t>(hi[c] - lo[c] + 1);
        }
        if (cells > 600) { // (a huge query triangle: every triangle of A is a candidate)
            for (uint32_t ia = 0; ia < ab.size(); ++ia) if (test(ia)) return true;
            return false;
        }
        for (int64_t x = lo[0]; x <= hi[0]; ++x) for (int64_t y = lo[1]; y <= hi[1]; ++y) for (int64_t z = lo[2]; z <= hi[2]; ++z) {
            const uint64_t key = Key(x, y, z);
            units += 1;
            auto it = std::lower_bound(g.ent.begin(), g.ent.end(), key, [](const Ent& e, uint64_t kk) { return e.key < kk; });
            for (; it != g.ent.end() && it->key == key; ++it) if (test(it->tri)) return true;
        }
        return false;
    }
    // work until `budget` units are spent (or done); the cap on the whole job: tooBig
    void Step(uint64_t budget, uint64_t cap) {
        const uint64_t stop = units + budget;
        const uint32_t nb = static_cast<uint32_t>(b.size() / 9);
        while (!done && units < stop) {
            if (units > cap) { tooBig = true; done = true; gap = 0; kind = 2; return; }
            if (ib >= nb) { // every triangle of B is farther than Delta(level)
                gap = level >= kLevels - 1 ? D : Delta(level);
                done = true;
                return;
            }
            uint32_t hit = 0;
            if (Query(ib, Delta(level), hit)) {
                if (level == 0) { Classify(hit, ib); gap = 0; done = true; return; }
                --level; // (a partner within delta: look for one within half of it; the triangle is asked again)
            } else ++ib;
        }
    }
};

// ------------------------------------------------------------------------------------------------ the oracle
struct GeoStats {
    uint64_t rangesReady = 0, rangesBad = 0, pairsDone = 0, pairsBig = 0, pairsQueued = 0, workUnits = 0, extractions = 0, evictions = 0;
    uint32_t rangesLive = 0, pairsLive = 0, jobsPending = 0;
};
struct GeoConfig {
    double etaLog2 = 18;                // eta = 2^-etaLog2 of the depth range
    bool testNoContact = false;         // (tests only: touching / crossing / coplanar / near pairs are called free: the negative control of the pin; false in the game)
    double testRelax = 1.0;             // (tests only: the negative control multiplies thr and the depth / rect margins by this; always 1 in the game)
    double fastFlat = 0.01;             // (TIES=7) a range is a flat sheet when its thinnest box side is at most this fraction of the box diagonal
    uint32_t maxTris = 24000;           // a static range of more triangles is not analysed (pinned: nodata)
    uint64_t jobCap = 3000000;          // work units a pair may cost
    size_t maxRanges = 8192, maxPairs = 65536;
};
class GeoOracle : public TieOracle {
public:
    GeoConfig cfg;
    GeoStats stats;
    explicit GeoOracle(GeoSource* s) : src_(s) {}
    // the picture's static buffer uploads and frees: draws of such buffers are not analysed in this picture
    void BeginPicture(const CmdBuf& buf) override {
        touched_.clear();
        for (const BufUpload& u : buf.bufUploads) touched_.push_back(u.bufId);
        for (const Cmd& c : buf.cmds) if (c.type == CMD_BUF_FREE) touched_.push_back(c.arg);
        std::sort(touched_.begin(), touched_.end());
        eta_ = std::ldexp(1.0, -static_cast<int>(cfg.etaLog2));
    }
    void Pair(const CmdBuf& buf, const DrawRec& t, const DrawRec& o, PairInfo& pi) override {
        pi = PairInfo{};
        if (!t.vbuf || !o.vbuf) { pi.why = kTDynamic; return; }
        if (t.prim != PRIM_TRIANGLES || o.prim != PRIM_TRIANGLES) { pi.why = kTDynamic; return; }
        const uint64_t ev = stats.evictions;
        Geo* ga = Lookup(t);
        Geo* gb = Lookup(o);
        if (stats.evictions != ev) { ga = Lookup(t); gb = Lookup(o); } // (the second lookup emptied the table under the first pointer)
        if (!ga || !gb) { pi.why = kTPending; return; } // (a buffer being uploaded in this picture)
        if (ga->state == 2 || gb->state == 2) { pi.why = kTNoData; return; }
        if (ga->state == 0 || gb->state == 0) { pi.why = kTPending; return; }
        // the camera
        const uint32_t pb = t.ublock[UB_PROJ], mb = t.ublock[UB_MV];
        if (pb == kNoBlock || mb == kNoBlock || pb > buf.uni.size() || buf.uni.size() - pb < 16 || mb > buf.uni.size() || buf.uni.size() - mb < 12) { pi.why = kTNoData; return; }
        const Xf xa = MakeXf(buf.uni.data() + pb, buf.uni.data() + mb, t.rs);
        if (!xa.ok) { pi.why = kTNoData; return; }
        const bool sameView = t.rs.vpX == o.rs.vpX && t.rs.vpY == o.rs.vpY && t.rs.vpW == o.rs.vpW && t.rs.vpH == o.rs.vpH && t.rs.zNear == o.rs.zNear && t.rs.zFar == o.rs.zFar;
        const BoxProj pa = ProjectBox(xa, ga->bb), pbx = ProjectBox(xa, gb->bb);
        if (!pa.visible || !pbx.visible) { pi.why = kTFree; pi.freeWhy = kFInvisible; return; }
        if (sameView && pa.inFront && pbx.inFront) {
            const double m = 2.0 * cfg.testRelax;
            if (pa.xmax + m < pbx.xmin || pbx.xmax + m < pa.xmin || pa.ymax + m < pbx.ymin || pbx.ymax + m < pa.ymin) { pi.why = kTFree; pi.freeWhy = kFRect; return; }
            const double e = eta_ * std::fabs(xa.dr) * cfg.testRelax;
            if (pa.zmax + e < pbx.zmin || pbx.zmax + e < pa.zmin) { pi.why = kTFree; pi.freeWhy = kFDepth; return; }
        }
        // the model-space distance that guarantees an ndc depth difference above eta_ndc
        const double wmax = std::max(pa.wmax, pbx.wmax);
        const double thr = cfg.testRelax * (2.0 * eta_ / std::fabs(xa.dr)) * xa.K * wmax * wmax / (std::fabs(xa.dz) * xa.sigmaMin);
        pi.thr = static_cast<float>(thr);
        const double boxGap = BoxDist(ga->bb, gb->bb);
        if (boxGap > thr) { pi.why = kTFree; pi.freeWhy = kFBox; pi.gap = static_cast<float>(boxGap); return; }
        PairRes& pr = PairFor(*ga, *gb, t, o);
        if (pr.state == 0) { pi.why = kTPending; return; }
        pi.gap = static_cast<float>(pr.gap);
        if (pr.state == 2) { pi.why = kTNoData; return; }
        if (pr.gap > thr) { pi.why = kTFree; pi.freeWhy = kFGap; return; }
        pi.why = pr.gap > 0 ? kTNear : pr.kind == 1 ? kTCoplanar : kTContact;
        if (__builtin_expect(cfg.testNoContact, 0)) { pi.why = kTFree; pi.freeWhy = kFGap; }
    }
    // (gpu26, GXV_HSR_ORDER_TIES=7) the cheap approximate rule, no triangle work and no pair jobs (so nothing but the ranges' extraction is ever queued):
    //   1. different depth ranges (zNear / zFar) on the two draws: a deliberate bias, the game's own decal separation: keep the order (kTBias)
    //   2. a range not extracted yet / no CPU copy / a stream draw: keep the order (pending / nodata / dynamic), as ties 4
    //   3. the model-space boxes are apart: with one transform two surfaces share a pixel AND a depth only at a common 3D point, so no tie: free (box)
    //   4. the boxes meet or overlap: T is kept only when it is a flat sheet (thinnest side <= fastFlat * diagonal) lying on a face of O's box (within the same
    //      tolerance) or when O is as thin as it: the shape of a decal on a surface (kTDecal). Anything else (a solid part, a seam, a card inside a volume)
    //      is freed (solid-approx): there the two can only tie along an edge or a line, a few pixels wide. APPROXIMATE: a decal on a curved or inner
    //      surface of a thick O is not recognised and may flip (the later draw's colour shows instead of the earlier one's, at the tied pixels).
    void FastPair(const CmdBuf&, const DrawRec& t, const DrawRec& o, PairInfo& pi) override {
        pi = PairInfo{};
        if (t.rs.zNear != o.rs.zNear || t.rs.zFar != o.rs.zFar) { pi.why = kTBias; return; }
        if (!t.vbuf || !o.vbuf || t.prim != PRIM_TRIANGLES || o.prim != PRIM_TRIANGLES) { pi.why = kTDynamic; return; }
        const uint64_t ev = stats.evictions;
        Geo* ga = Lookup(t);
        Geo* gb = Lookup(o);
        if (stats.evictions != ev) { ga = Lookup(t); gb = Lookup(o); }
        if (!ga || !gb) { pi.why = kTPending; return; }
        if (ga->state == 2 || gb->state == 2) { pi.why = kTNoData; return; }
        if (ga->state == 0 || gb->state == 0) { pi.why = kTPending; return; }
        const double gap = BoxDist(ga->bb, gb->bb);
        pi.gap = static_cast<float>(gap);
        if (gap > 0) { pi.why = kTFree; pi.freeWhy = kFBox; return; }
        const Aabb& a = ga->bb;
        const Aabb& b = gb->bb;
        int k = 0;
        double ext[3];
        for (int i = 0; i < 3; ++i) { ext[i] = a.hi[i] - a.lo[i]; if (ext[i] < ext[k]) k = i; }
        const double diagA = a.Diag(), diagB = b.Diag();
        pi.thr = static_cast<float>(cfg.fastFlat * diagA);
        if (!(diagA > 0) || ext[k] > cfg.fastFlat * diagA) { pi.why = kTFree; pi.freeWhy = kFSolid; return; } // (T is a solid part)
        const double tol = cfg.fastFlat * diagB + ext[k];
        const double c0 = a.lo[k], c1 = a.hi[k];
        const bool onFace = std::fabs(c0 - b.lo[k]) <= tol || std::fabs(c1 - b.lo[k]) <= tol || std::fabs(c0 - b.hi[k]) <= tol || std::fabs(c1 - b.hi[k]) <= tol;
        const bool bThin = b.hi[k] - b.lo[k] <= tol;
        if (onFace || bThin) { pi.why = kTDecal; return; }
        pi.why = kTFree;
        pi.freeWhy = kFSolid;
    }
    // work off the queues: at most about `budget` units (extractions count their index entries). Call after the picture's planning.
    void Pump(uint64_t budget) override {
        uint64_t left = budget;
        while (left > 0 && !geoQ_.empty()) {
            const uint64_t key = geoQ_.back();
            geoQ_.pop_back();
            auto it = geo_.find(key);
            if (it == geo_.end() || it->second.state != 0) continue;
            Geo& g = it->second;
            StaticView v;
            std::vector<float> tmp;
            uint64_t h = 0;
            Aabb bb;
            ++stats.extractions;
            const uint64_t cost = g.dr.idxCount + 16;
            left = left > cost ? left - cost : 0;
            stats.workUnits += cost;
            if (src_->Get(g.dr.vbuf, v) && v.epoch == g.epoch && ExtractTris(g.dr, v, cfg.maxTris, tmp, bb, h)) { g.bb = bb; g.hash = h; g.ntris = static_cast<uint32_t>(tmp.size() / 9); g.state = 1; ++stats.rangesReady; }
            else { g.state = 2; ++stats.rangesBad; }
        }
        while (left > 0 && (curJob_ || !pairQ_.empty())) {
            if (!curJob_) {
                const PairKey pk = pairQ_.front();
                pairQ_.erase(pairQ_.begin());
                auto it = pairs_.find(pk);
                if (it == pairs_.end() || it->second.state != 0) continue;
                // the two ranges
                Geo* ga = FindGeoByKey(it->second.ka);
                Geo* gb = FindGeoByKey(it->second.kb);
                StaticView va, vb;
                std::vector<float> ta, tb;
                Aabb ba, bb;
                uint64_t ha = 0, hb = 0;
                if (!ga || !gb || ga->state != 1 || gb->state != 1 || !src_->Get(ga->dr.vbuf, va) || !src_->Get(gb->dr.vbuf, vb) || va.epoch != ga->epoch || vb.epoch != gb->epoch ||
                    !ExtractTris(ga->dr, va, cfg.maxTris, ta, ba, ha) || !ExtractTris(gb->dr, vb, cfg.maxTris, tb, bb, hb) || ha != ga->hash || hb != gb->hash) {
                    pairs_.erase(it); // (changed or gone: asked again when seen again)
                    continue;
                }
                curJob_.reset(new Job());
                curKey_ = pk;
                const uint64_t cost = (ta.size() + tb.size()) / 3 + 16;
                left = left > cost ? left - cost : 0;
                stats.workUnits += cost;
                if (!curJob_->Init(std::move(ta), std::move(tb), ba, bb)) {
                    PairRes& r = pairs_[pk];
                    r.state = 2; r.gap = 0; ++stats.pairsBig;
                    curJob_.reset();
                    continue;
                }
                continue;
            }
            const uint64_t before = curJob_->units;
            curJob_->Step(left, cfg.jobCap);
            const uint64_t used = curJob_->units - before;
            stats.workUnits += used;
            left = left > used ? left - used : 0;
            if (curJob_->done) {
                PairRes& r = pairs_[curKey_];
                if (curJob_->tooBig) { r.state = 2; r.gap = 0; ++stats.pairsBig; }
                else { r.state = 1; r.gap = curJob_->gap; r.kind = curJob_->kind; ++stats.pairsDone; }
                curJob_.reset();
            }
        }
        stats.jobsPending = static_cast<uint32_t>(pairQ_.size() + (curJob_ ? 1 : 0));
        stats.rangesLive = static_cast<uint32_t>(geo_.size());
        stats.pairsLive = static_cast<uint32_t>(pairs_.size());
    }
    // the content hash of a draw's triangles when known (the dump), else 0
    uint64_t GeoHashOf(const DrawRec& d) override {
        if (!d.vbuf) return 0;
        Geo* g = LookupNoQueue(d);
        return g && g->state == 1 ? g->hash : 0;
    }
    void Reset() { geo_.clear(); pairs_.clear(); geoQ_.clear(); pairQ_.clear(); curJob_.reset(); stats = GeoStats{}; }

private:
    struct Geo {
        RangeRef dr;
        uint32_t epoch = 0;
        uint8_t state = 0;        // 0 queued, 1 ready, 2 bad (no data / unreadable / too many triangles)
        Aabb bb;
        uint64_t hash = 0;
        uint32_t ntris = 0;
        uint64_t key = 0;
    };
    struct PairKey {
        uint64_t a, b;
        bool operator==(const PairKey& o) const { return a == o.a && b == o.b; }
    };
    struct PairKeyHash { size_t operator()(const PairKey& k) const { return static_cast<size_t>((k.a * 0x9E3779B97F4A7C15ull) ^ (k.b + 0x7F4A7C15ull + (k.a << 6))); } };
    struct PairRes { uint8_t state = 0; uint8_t kind = 0; double gap = 0; uint64_t ka = 0, kb = 0; };
    GeoSource* src_;
    std::vector<uint32_t> touched_;
    double eta_ = 0;
    std::unordered_map<uint64_t, Geo> geo_;
    std::vector<uint64_t> geoQ_;
    std::unordered_map<PairKey, PairRes, PairKeyHash> pairs_;
    std::vector<PairKey> pairQ_;
    std::unique_ptr<Job> curJob_;
    PairKey curKey_{0, 0};
    static uint64_t RangeKey(const DrawRec& d, uint32_t epoch) {
        uint64_t h = 1469598103934665603ull;
        const uint64_t v[7] = {d.vbuf, epoch, d.vtxOffset, d.idxOffset, d.idxCount, static_cast<uint64_t>(d.layoutMask & VL_QPOS) | (static_cast<uint64_t>(MakeLayout(d.layoutMask).stride) << 8), 0};
        for (int i = 0; i < 6; ++i) h = (h ^ v[i]) * 1099511628211ull;
        return h ? h : 1;
    }
    void Evict() {
        geo_.clear(); geoQ_.clear(); pairQ_.clear(); curJob_.reset();
        for (auto it = pairs_.begin(); it != pairs_.end();) { if (it->second.state == 0) it = pairs_.erase(it); else ++it; }
        ++stats.evictions;
    }
    Geo* FindGeoByKey(uint64_t key) { auto it = geo_.find(key); return it == geo_.end() ? nullptr : &it->second; }
    bool Touched(uint32_t buf) const { return std::binary_search(touched_.begin(), touched_.end(), buf); }
    Geo* LookupNoQueue(const DrawRec& d) {
        StaticView v;
        if (Touched(d.vbuf) || !src_->Get(d.vbuf, v)) return nullptr;
        return FindGeoByKey(RangeKey(d, v.epoch));
    }
    // the range's entry (made, and queued for extraction, on first sight); null: a buffer uploaded / freed in this picture
    Geo* Lookup(const DrawRec& d) {
        if (Touched(d.vbuf)) return nullptr;
        StaticView v;
        if (!src_->Get(d.vbuf, v)) { // (no copy kept: counted as bad, under a key of its own, until the buffer is seen)
            static Geo none;
            none.state = 2;
            return &none;
        }
        const uint64_t key = RangeKey(d, v.epoch);
        auto it = geo_.find(key);
        if (it != geo_.end()) return &it->second;
        if (geo_.size() >= cfg.maxRanges) Evict(); // (ranges that change from picture to picture: start over, the answered pairs are kept)
        Geo& g = geo_[key];
        g.dr = RangeRef(d);
        g.epoch = v.epoch;
        g.key = key;
        g.state = 0;
        geoQ_.push_back(key);
        return &g;
    }
    PairRes& PairFor(const Geo& a, const Geo& b, const DrawRec&, const DrawRec&) {
        // keyed by the content hashes (the same triangles under another buffer id hit)
        PairKey pk{std::min(a.hash, b.hash), std::max(a.hash, b.hash)};
        auto it = pairs_.find(pk);
        if (it != pairs_.end()) return it->second;
        if (pairs_.size() >= cfg.maxPairs) { pairs_.clear(); pairQ_.clear(); curJob_.reset(); }
        PairRes& r = pairs_[pk];
        r.state = 0;
        r.ka = a.hash <= b.hash ? a.key : b.key;
        r.kb = a.hash <= b.hash ? b.key : a.key;
        pairQ_.push_back(pk);
        ++stats.pairsQueued;
        return r;
    }
};

} // namespace hsr
} // namespace vgx
