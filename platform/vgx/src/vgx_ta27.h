// ta27 (13 Nov 2026): GXV_TA27 census of the GPU's vertex (TA) phase, the pure part (no GL, no game types; host test tests/vgx_ta27_test.cpp).
//
// Why: the console's gpu-scenes lines fit (1626 five-second windows, c86) TA ms = 19 ns x indices + 1.0 ms x EFB scenes + 1.0 ms x
// texture/pass scenes (R2 0.925); draws and light loops add nothing. In an index-bound stage (Dreadnought R4: 296k indices, 5.7 of
// 8.7 ms; Honeyhive 312k; Sand Clock 353k; Cube Bubble 476k) the only exact vertex-side lever that is left is to submit fewer
// indices. vgx_draw.cpp "clusters of a static run" (GXV_CLUSTERCULL) cuts clusters of 64 consecutive triangles in the order of the
// game's strips by a frustum test. This file adds, as a CENSUS (nothing in the picture changes), the two things that decide whether a
// finer cut can pay:
//   * a normal cone per cluster: a cluster whose every triangle faces away from the eye is removed by the GPU's back-face cull AFTER
//     its vertices were shaded and binned; leaving it out of the draw saves the TA work (exact: those triangles draw no pixel);
//   * the same clusters made after a one-time spatial sort of the run's triangles (Morton order of the centroids), the order a
//     "cluster the geometry at load time" build would draw: how much of the run it would leave out by frustum, by cone, by both, and
//     in how many separate index ranges (each range is a draw for the front end).
// Exactness of the cone test: front facing as the GL pipeline of this port sees it (glFrontFace CCW on the BuildProjC clip space) is   s * n . (p - E) > 0   with n = (v1 - v0) x (v2 - v0) in model space
// (the decoded winding), E the eye in model space (E = -M3^-1 t for the position matrix [M3 | t]), and s = sign(det M3) * sign(-p0 p2)
// for the projection x = p0 ex + p1 ez, y = -(p2 ey + p3 ez) (BuildProjC); a triangle behind the face of its plane seen from E is back
// facing for every point of it. The test below says "all back facing" only when the cone of the cluster's unit normals, widened by
// the bounding sphere of its points, lies entirely on the back side with a margin. The host test checks it against the projected
// signed area of every triangle (the GL view), thousands of random clusters and cameras, 0 contradictions allowed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ta27 {

struct Cl {
    float mn[3], mx[3];  // the box of the cluster's vertex positions (x, y, z as the decoded vertex has them)
    float c[3], r;       // bounding sphere of the box
    float ax[3];         // unit axis of the normal cone (the normalised sum of the unit normals of the triangles with an area)
    float cut;           // cos of the widest angle between a triangle's unit normal and the axis; <= 0.05: no usable cone
    uint32_t row;        // first index of the cluster within the run's (original or sorted) index order
    uint32_t tris;       // triangles with an area in it
};

// One run's clusters in the game's own triangle order (o) and after the spatial sort (s).
struct RunSet { std::vector<Cl> o, s; };

inline void Cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
inline float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The cluster of the triangles whose three corners are p[3 * k + 0..2] (positions, 3 floats each), k = 0 .. n - 1 triangles.
// false: a corner is not finite (no cluster).
inline bool MakeCl(const float (*p)[3], uint32_t n, Cl& o) {
    for (int a = 0; a < 3; ++a) { o.mn[a] = 1e30f; o.mx[a] = -1e30f; o.ax[a] = 0.0f; }
    std::vector<float> nrm;
    nrm.reserve(static_cast<size_t>(n) * 3);
    for (uint32_t t = 0; t < n; ++t) {
        for (int k = 0; k < 3; ++k)
            for (int a = 0; a < 3; ++a) {
                const float v = p[3 * t + k][a];
                if (!std::isfinite(v)) return false;
                o.mn[a] = std::min(o.mn[a], v);
                o.mx[a] = std::max(o.mx[a], v);
            }
        float e1[3], e2[3], nn[3];
        for (int a = 0; a < 3; ++a) { e1[a] = p[3 * t + 1][a] - p[3 * t][a]; e2[a] = p[3 * t + 2][a] - p[3 * t][a]; }
        Cross(e1, e2, nn);
        const float len = std::sqrt(Dot(nn, nn));
        if (!(len > 1e-20f)) continue; // (no area: draws nothing whichever way it faces)
        for (int a = 0; a < 3; ++a) nn[a] /= len;
        nrm.insert(nrm.end(), nn, nn + 3);
    }
    for (int a = 0; a < 3; ++a) o.c[a] = 0.5f * (o.mn[a] + o.mx[a]);
    const float hx = o.mx[0] - o.c[0], hy = o.mx[1] - o.c[1], hz = o.mx[2] - o.c[2];
    o.r = std::sqrt(hx * hx + hy * hy + hz * hz);
    o.tris = static_cast<uint32_t>(nrm.size() / 3);
    o.cut = -1.0f;
    if (o.tris == 0) return true;
    float s[3] = {0, 0, 0};
    for (size_t i = 0; i < nrm.size(); i += 3)
        for (int a = 0; a < 3; ++a) s[a] += nrm[i + a];
    const float sl = std::sqrt(Dot(s, s));
    if (!(sl > 1e-12f)) return true;
    for (int a = 0; a < 3; ++a) o.ax[a] = s[a] / sl;
    float cut = 1.0f;
    for (size_t i = 0; i < nrm.size(); i += 3) cut = std::min(cut, Dot(&nrm[i], o.ax));
    o.cut = cut;
    return true;
}

// The eye in model space and the facing sign from the position matrix rows m (3 rows of 4: eye = M3 * p + t) and the projection's
// p0, p2 (BuildProjC). false: not invertible, not finite, or no usable sign. fs * n . (p - E) > 0 means front.
inline bool EyeInModel(const float* const m[3], float p0, float p2, float E[3], float& fs) {
    const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    const float inv = 1.0f / det;
    // M3^-1 = adj / det
    const float r00 = A * inv, r01 = -(b * i - c * h) * inv, r02 = (b * f - c * e) * inv;
    const float r10 = B * inv, r11 = (a * i - c * g) * inv, r12 = -(a * f - c * d) * inv;
    const float r20 = C * inv, r21 = -(a * h - b * g) * inv, r22 = (a * e - b * d) * inv;
    const float t0 = m[0][3], t1 = m[1][3], t2 = m[2][3];
    E[0] = -(r00 * t0 + r01 * t1 + r02 * t2);
    E[1] = -(r10 * t0 + r11 * t1 + r12 * t2);
    E[2] = -(r20 * t0 + r21 * t1 + r22 * t2);
    if (!std::isfinite(E[0] + E[1] + E[2])) return false;
    const float pp = p0 * p2;
    if (!std::isfinite(pp) || pp == 0.0f) return false;
    fs = (det > 0.0f ? 1.0f : -1.0f) * (pp > 0.0f ? 1.0f : -1.0f);
    return true;
}

// true: every triangle of cluster k with an area faces away from the eye E for the draw's cull mode (cullBack: GX "cull back" removes
// triangles facing away; the other mode, "cull front", removes those facing the eye: pass cullBack = false) and so draws no pixel.
// Conservative: the widest cone (axis angle phi to the direction from E to the cluster's centre, normals up to theta off the axis) and
// the sphere of radius r around the centre: all back facing when  d cos(phi + theta) > r  with a relative and an absolute margin.
inline bool AllCulled(const Cl& k, const float E[3], float fs, bool cullBack) {
    if (k.tris == 0) return true; // (nothing with an area: nothing is drawn)
    if (!(k.cut > 0.05f)) return false;
    float w[3] = {k.c[0] - E[0], k.c[1] - E[1], k.c[2] - E[2]};
    // facing as the GL pipeline sees it: front iff fs * n . (p - E) > 0. cull back removes fs * n . (p - E) < 0; cull front removes > 0.
    // sg * n . (p - E) > 0 for every triangle of a culled cluster: the axis is taken with the sign sg.
    const float sg = (cullBack ? -fs : fs);
    const float d2 = Dot(w, w);
    const float d = std::sqrt(d2);
    if (!(d > 1e-6f) || !std::isfinite(d)) return false;
    const float cphi = sg * Dot(w, k.ax) / d;      // cos of the angle between the (signed) axis and w
    if (!(cphi > 0.0f)) return false;
    const float sphi = std::sqrt(std::max(0.0f, 1.0f - cphi * cphi));
    const float cth = std::min(1.0f, k.cut), sth = std::sqrt(std::max(0.0f, 1.0f - cth * cth));
    const float cmin = cphi * cth - sphi * sth;    // cos(phi + theta), valid while phi + theta <= pi (cmin > 0 is what we need)
    if (!(cmin > 0.0f)) return false;
    // margin: float error of the cross products and the sphere radius (relative 1e-3 of d plus 1e-3 of r, and an absolute 1e-5)
    return d * cmin > k.r * 1.001f + 1e-3f * d + 1e-5f;
}

// ---- building ------------------------------------------------------------------------------------------------------------------
// The clusters of `per` consecutive indices (a multiple of 3) of the run whose index i names the position pos(idx[i]) (callable
// returning const float* of 3 floats); order[] (optional, triangle numbers) gives the triangles in sorted order. Clusters get row =
// the first index of the cluster in that order. false when a position is not finite.
template <class PosOf>
bool BuildClusters(const uint16_t* idx, uint32_t nIdx, uint32_t per, PosOf pos, const uint32_t* order, std::vector<Cl>& out) {
    out.clear();
    const uint32_t nT = nIdx / 3;
    const uint32_t perT = std::max(1u, per / 3);
    for (uint32_t t0 = 0; t0 < nT; t0 += perT) {
        const uint32_t t1 = std::min(nT, t0 + perT);
        std::vector<float> pts;
        pts.reserve(static_cast<size_t>(t1 - t0) * 9);
        for (uint32_t t = t0; t < t1; ++t) {
            const uint32_t tri = order ? order[t] : t;
            for (int k = 0; k < 3; ++k) {
                const float* q = pos(idx[tri * 3 + k]);
                pts.push_back(q[0]); pts.push_back(q[1]); pts.push_back(q[2]);
            }
        }
        Cl c;
        if (!MakeCl(reinterpret_cast<const float(*)[3]>(pts.data()), t1 - t0, c)) { out.clear(); return false; }
        c.row = t0 * 3;
        out.push_back(c);
    }
    return true;
}

// Morton order (10 bits an axis over the run's box) of the triangles' centroids: order[k] = the k-th triangle to draw.
template <class PosOf>
void MortonOrder(const uint16_t* idx, uint32_t nIdx, PosOf pos, std::vector<uint32_t>& order) {
    const uint32_t nT = nIdx / 3;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<float> cen(static_cast<size_t>(nT) * 3);
    for (uint32_t t = 0; t < nT; ++t) {
        float c[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            const float* q = pos(idx[t * 3 + k]);
            for (int a = 0; a < 3; ++a) c[a] += q[a] * (1.0f / 3.0f);
        }
        for (int a = 0; a < 3; ++a) { cen[t * 3 + a] = c[a]; mn[a] = std::min(mn[a], c[a]); mx[a] = std::max(mx[a], c[a]); }
    }
    auto spread = [](uint32_t v) { // 10 bits to every third bit
        v &= 0x3FFu;
        v = (v | (v << 16)) & 0x030000FFu;
        v = (v | (v << 8)) & 0x0300F00Fu;
        v = (v | (v << 4)) & 0x030C30C3u;
        v = (v | (v << 2)) & 0x09249249u;
        return v;
    };
    std::vector<std::pair<uint32_t, uint32_t>> key(nT);
    for (uint32_t t = 0; t < nT; ++t) {
        uint32_t q[3];
        for (int a = 0; a < 3; ++a) {
            const float ext = mx[a] - mn[a];
            const float u = ext > 0.0f ? (cen[t * 3 + a] - mn[a]) / ext : 0.0f;
            q[a] = static_cast<uint32_t>(std::min(1023.0f, std::max(0.0f, u * 1023.0f)));
        }
        key[t] = {spread(q[0]) | (spread(q[1]) << 1) | (spread(q[2]) << 2), t};
    }
    std::sort(key.begin(), key.end());
    order.resize(nT);
    for (uint32_t t = 0; t < nT; ++t) order[t] = key[t].second;
}

// What the clusters in `cl` (n of them, covering idxCount indices) would leave out: out[k] flags (1: left out). Counts the indices in
// left-out clusters and the number of separate ranges the kept clusters form.
struct Tally { uint64_t idxOut = 0, idxBack = 0, idxUnion = 0, ranges = 0, tested = 0, outN = 0, backN = 0; };
inline uint32_t ClusterIdx(const Cl* cl, uint32_t n, uint32_t i, uint32_t idxCount) { return (i + 1 < n ? cl[i + 1].row : idxCount) - cl[i].row; }
inline void Accumulate(const Cl* cl, uint32_t n, uint32_t idxCount, const bool* frustumOut, const bool* back, Tally& t) {
    bool prevKept = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t w = ClusterIdx(cl, n, i, idxCount);
        ++t.tested;
        if (frustumOut[i]) { t.idxOut += w; ++t.outN; }
        if (back[i]) { t.idxBack += w; ++t.backN; }
        const bool gone = frustumOut[i] || back[i];
        if (gone) t.idxUnion += w;
        if (!gone && !prevKept) ++t.ranges;
        prevKept = !gone;
    }
}

} // namespace ta27
