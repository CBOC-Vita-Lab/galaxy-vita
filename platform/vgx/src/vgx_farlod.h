// GXV_FARLOD (farlod32, 13 Nov 2026): reduced index lists for far static runs, made from the run's own vertices.
//
// A rigid triangle run of a promoted display list (vgx_dlcache.cpp Promote: one J3D shape's welded vertices and its triangle list) is
// drawn with every triangle the model has at any distance, and the GPU's vertex (TA) phase pays per index (the console fit: 19.3 ns an
// index). This makes up to kMaxLevels shorter triangle lists of the SAME vertices (nothing is added to the vertex buffer; a level is
// only more 16-bit indices after the run's own), each with a bound on how far its surface is from the model's, and the front end
// draws the shortest list whose bound is under GXV_FARLOD_PX pixels where the run is (vgx_draw.cpp EmitStaticRun). A switch between
// two lists therefore moves no surface by more than that fraction of a pixel: there is nothing to fade and nothing appears or vanishes.
//
// The method is edge collapse onto an existing vertex ("half-edge collapse": vertex v's triangles are re-pointed to a neighbour w, the
// triangles of the edge v-w go), ordered by quadric error (Garland and Heckbert 1997), in passes of independent collapses, the way
// meshoptimizer's simplifier is organised; written here from the papers (meshoptimizer's source is not on this machine).
//   * Position error: every vertex carries the sum over the planes of the model's triangles it stands for of the squared distance to the
//     plane (each plane with weight 1, so the sum is at least the largest squared distance): a collapse is allowed while the square
//     root of that sum at w stays under the level's limit, and the level's `error` is the largest such value it took.
//   * Attributes (normal, colours, texture coordinates) are per "wedge" (a vertex of the buffer; several wedges share a position along
//     a seam). Each triangle's attribute is a linear function over its plane; a wedge carries the sum over its triangles of the squared
//     difference between that function at the new place and the attribute of the wedge it becomes, in units of the channel's tolerance.
//     Flat ground with a linear colour ramp collapses freely; a lighting gradient that bends, a texture that is stretched, does not.
//   * Never moved: a vertex on an open edge of the run (the run's border: the neighbouring shape's vertices are there too, a moved
//     border would open a crack), a vertex the caller marks locked (its position is also used by another run: a decal or second layer
//     drawn on the same triangles must keep meeting the same depths), a vertex of an edge used by more than two triangles.
//   * A collapse is refused when it would turn a triangle over (normal changed by more than ~75 degrees) or pinch the surface (the two
//     ends have a common neighbour besides the edge's own triangles), and along a seam both sides must go to the same place.
//   * The triangles that are left keep their order and their winding, so a blended run draws in the model's order too.
// All arithmetic is relative to the vertex itself (each quadric is kept about its own vertex and re-based when it is merged), so
// single precision is enough for stage-sized coordinates. No allocation besides the Scratch's vectors; no global state.
// Host test: tests/vgx_farlod.cpp (this very file).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vgx {
namespace farlod {

constexpr uint32_t kMaxLevels = 3;
constexpr uint32_t kMaxCh = 12; // attribute channels (scalars) at most
constexpr uint32_t kMaxWedges = 4;

// One attribute of the vertex: `count` scalars at `offset`; type 0 = f32, 1 = u8 (value / 255). tol: the change of one scalar that
// counts like a position error of the level's limit (larger: more freedom). count 0 entries are ignored.
struct Attr {
    uint16_t offset = 0;
    uint8_t type = 0, count = 0;
    float tol = 1.0f;
};
struct Input {
    const uint8_t* vtx = nullptr; // position = 3 x f32 at offset 0 of each vertex
    uint32_t vtxCount = 0, stride = 0;
    const uint16_t* idx = nullptr; // triangles, indices 0 .. vtxCount - 1
    uint32_t idxCount = 0;
    const Attr* attrs = nullptr;
    uint32_t nAttrs = 0;
    const uint8_t* locked = nullptr; // per vertex (optional): 1 = this position stays
    uint32_t nLevels = 0;
    float levelErr[kMaxLevels] = {};      // ascending, model units
    float levelAttrTol[kMaxLevels] = {};  // the attribute tolerances' factor at that level (1 = as given)
    float minGain = 0.15f;                // a level is kept only if it has at most (1 - minGain) of the indices of the one before
};
struct Level {
    std::vector<uint16_t> idx;
    float error = 0; // model units: the bound of this list (<= its levelErr)
    float limit = 0; // the levelErr it was made for
};
struct Result {
    Level lv[kMaxLevels];
    uint32_t nLevels = 0;     // levels kept (lv[0 .. nLevels), ascending error, descending size)
    uint32_t collapses = 0, passes = 0, lockedVerts = 0, positions = 0;
};

struct GeoQ { float a[6], b[3], c; };                          // E(d) = d'Ad + 2 b.d + c, d = p - (the vertex's own position)
struct AttQ { float g[6], h[3], cc, n; };                      // + per channel: s[3], u  (see Eval)
struct Cand { float cost; uint32_t tri; uint8_t edge; };
struct Scratch {
    std::vector<float> pos, ch, chq;            // 3 per vertex; nCh per vertex; 4 * nCh per vertex (s[3], u)
    std::vector<uint32_t> pid, wmap, adjOff, adjTri, stamp, table;
    std::vector<uint16_t> tri, tri2;
    std::vector<uint8_t> flag;                  // per position: 1 locked, 2 moved this pass, 4 target this pass
    std::vector<uint8_t> nW;
    std::vector<GeoQ> gq;
    std::vector<AttQ> aq;
    std::vector<Cand> cand;
    std::vector<uint64_t> edges;
    void Release() { *this = Scratch(); }
};
// The scratch bytes Build needs for a run (to refuse runs over a budget before copying anything).
inline uint64_t ScratchBytes(uint32_t vtxCount, uint32_t idxCount, uint32_t nCh) {
    return static_cast<uint64_t>(vtxCount) * (12u + 4u * nCh + 16u * nCh + 4u * 5u + 2u + sizeof(GeoQ) + sizeof(AttQ)) +
           static_cast<uint64_t>(idxCount) * (2u + 2u + 4u + 8u * 2u + sizeof(Cand));
}

namespace detail {
inline float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void SymMul(const float* m, const float* d, float* o) { // m: xx xy xz yy yz zz
    o[0] = m[0] * d[0] + m[1] * d[1] + m[2] * d[2];
    o[1] = m[1] * d[0] + m[3] * d[1] + m[4] * d[2];
    o[2] = m[2] * d[0] + m[4] * d[1] + m[5] * d[2];
}
inline void SymAdd(float* m, const float* v, float w) {
    m[0] += w * v[0] * v[0]; m[1] += w * v[0] * v[1]; m[2] += w * v[0] * v[2];
    m[3] += w * v[1] * v[1]; m[4] += w * v[1] * v[2]; m[5] += w * v[2] * v[2];
}
inline float GeoEval(const GeoQ& q, const float* d) {
    float t[3];
    SymMul(q.a, d, t);
    return Dot(t, d) + 2.0f * Dot(q.b, d) + q.c;
}
// E(dp, da) = dp'G dp - 2 sum_k da_k (s_k . dp) + n sum_k da_k^2 + 2 dp.h - 2 sum_k da_k u_k + cc
inline float AttEval(const AttQ& q, const float* sq, uint32_t nCh, const float* dp, const float* da) {
    float t[3];
    SymMul(q.g, dp, t);
    float e = Dot(t, dp) + 2.0f * Dot(q.h, dp) + q.cc;
    for (uint32_t k = 0; k < nCh; ++k) e += da[k] * (q.n * da[k] - 2.0f * (Dot(sq + 4 * k, dp) + sq[4 * k + 3]));
    return e;
}
}  // namespace detail

// Makes the levels. False (and no level) for input it cannot work on. `out` levels hold indices of the caller's vertices.
inline bool Build(const Input& in, Result& out, Scratch& S) {
    using namespace detail;
    out.nLevels = out.collapses = out.passes = out.lockedVerts = out.positions = 0;
    for (Level& l : out.lv) { l.idx.clear(); l.error = l.limit = 0; }
    const uint32_t nv = in.vtxCount;
    if (!in.vtx || !in.idx || nv < 4 || nv > 0x10000 || in.stride < 12 || in.idxCount < 12 || in.idxCount % 3 != 0 || in.nLevels == 0 || in.nLevels > kMaxLevels) return false;
    // ---- channels (each scalar divided by its tolerance)
    uint32_t nCh = 0;
    struct ChSrc { uint16_t off; uint8_t type; float inv; } src[kMaxCh];
    for (uint32_t a = 0; a < in.nAttrs; ++a)
        for (uint32_t c = 0; c < in.attrs[a].count && nCh < kMaxCh; ++c) {
            const Attr& at = in.attrs[a];
            if (!(at.tol > 0.0f)) continue;
            src[nCh++] = ChSrc{static_cast<uint16_t>(at.offset + c * (at.type ? 1u : 4u)), at.type, (at.type ? 1.0f / 255.0f : 1.0f) / at.tol};
        }
    S.pos.resize(static_cast<size_t>(nv) * 3);
    S.ch.resize(static_cast<size_t>(nv) * nCh);
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t v = 0; v < nv; ++v) {
        const uint8_t* p = in.vtx + static_cast<size_t>(v) * in.stride;
        float q[3];
        std::memcpy(q, p, 12);
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(q[a])) return false;
            mn[a] = std::min(mn[a], q[a]);
            mx[a] = std::max(mx[a], q[a]);
        }
        for (uint32_t k = 0; k < nCh; ++k) {
            float f;
            if (src[k].type) f = static_cast<float>(p[src[k].off]);
            else std::memcpy(&f, p + src[k].off, 4);
            if (!std::isfinite(f)) return false;
            S.ch[static_cast<size_t>(v) * nCh + k] = f * src[k].inv;
        }
    }
    const float ctr[3] = {0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2])};
    // ---- positions: pid[v] = the first vertex with the same position bits
    {
        uint32_t cap = 16;
        while (cap < nv * 2u) cap <<= 1;
        S.table.assign(cap, ~0u);
        S.pid.resize(nv);
        for (uint32_t v = 0; v < nv; ++v) {
            const uint8_t* p = in.vtx + static_cast<size_t>(v) * in.stride;
            uint32_t w[3];
            std::memcpy(w, p, 12);
            uint32_t h = (w[0] * 0x9E3779B1u) ^ (w[1] * 0x85EBCA77u) ^ (w[2] * 0xC2B2AE3Du);
            h ^= h >> 15;
            for (h &= cap - 1;; h = (h + 1) & (cap - 1)) {
                const uint32_t o = S.table[h];
                if (o == ~0u) { S.table[h] = v; S.pid[v] = v; ++out.positions; break; }
                if (std::memcmp(in.vtx + static_cast<size_t>(o) * in.stride, p, 12) == 0) { S.pid[v] = o; break; }
            }
            std::memcpy(&S.pos[static_cast<size_t>(v) * 3], p, 12);
            for (int a = 0; a < 3; ++a) S.pos[static_cast<size_t>(v) * 3 + a] -= ctr[a];
        }
    }
    const float* const P = S.pos.data();
    const uint32_t* const pid = S.pid.data();
    // ---- triangles (those with a repeated position cover nothing: left out, as GXV_DLVCACHE leaves them out)
    S.tri.clear();
    for (uint32_t i = 0; i + 2 < in.idxCount; i += 3) {
        const uint32_t a = in.idx[i], b = in.idx[i + 1], c = in.idx[i + 2];
        if (a >= nv || b >= nv || c >= nv) return false;
        if (pid[a] == pid[b] || pid[b] == pid[c] || pid[a] == pid[c]) continue;
        S.tri.push_back(static_cast<uint16_t>(a)); S.tri.push_back(static_cast<uint16_t>(b)); S.tri.push_back(static_cast<uint16_t>(c));
    }
    if (S.tri.size() < 12) return false;
    // ---- quadrics of the model's own triangles
    S.gq.assign(nv, GeoQ{});
    S.aq.assign(nv, AttQ{});
    S.chq.assign(static_cast<size_t>(nv) * nCh * 4, 0.0f);
    for (size_t t = 0; t + 2 < S.tri.size(); t += 3) {
        const uint32_t x[3] = {S.tri[t], S.tri[t + 1], S.tri[t + 2]};
        const float* p0 = P + 3 * pid[x[0]];
        const float* p1 = P + 3 * pid[x[1]];
        const float* p2 = P + 3 * pid[x[2]];
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(Dot(n, n));
        if (!(len > 1e-20f)) continue;
        for (int a = 0; a < 3; ++a) n[a] /= len;
        for (int c = 0; c < 3; ++c) SymAdd(S.gq[pid[x[c]]].a, n, 1.0f);
        if (!nCh) continue;
        const float m11 = Dot(e1, e1), m12 = Dot(e1, e2), m22 = Dot(e2, e2), det = m11 * m22 - m12 * m12;
        const bool grad = det > 1e-10f * m11 * m22;
        for (int c = 0; c < 3; ++c) S.aq[x[c]].n += 1.0f;
        for (uint32_t k = 0; k < nCh; ++k) {
            const float a0 = S.ch[static_cast<size_t>(x[0]) * nCh + k], d1 = S.ch[static_cast<size_t>(x[1]) * nCh + k] - a0, d2 = S.ch[static_cast<size_t>(x[2]) * nCh + k] - a0;
            float g[3] = {0, 0, 0};
            if (grad) {
                const float s = (d1 * m22 - d2 * m12) / det, u = (d2 * m11 - d1 * m12) / det;
                for (int a = 0; a < 3; ++a) g[a] = s * e1[a] + u * e2[a];
            } else if (d1 != 0.0f || d2 != 0.0f) {
                // (a sliver whose attribute changes: its gradient is not known; a steep one keeps its vertices where they are)
                for (int a = 0; a < 3; ++a) g[a] = 1e6f;
            }
            for (int c = 0; c < 3; ++c) {
                SymAdd(S.aq[x[c]].g, g, 1.0f);
                float* sq = &S.chq[(static_cast<size_t>(x[c]) * nCh + k) * 4];
                for (int a = 0; a < 3; ++a) sq[a] += g[a];
            }
        }
    }
    S.flag.assign(nv, 0);
    S.wmap.resize(nv);
    S.stamp.assign(nv, 0);
    S.nW.assign(nv, 0);
    uint32_t stampCur = 0;
    auto nextStamp = [&]() -> uint32_t {
        if (++stampCur == 0) { std::fill(S.stamp.begin(), S.stamp.end(), 0u); stampCur = 1; }
        return stampCur;
    };
    // The wedge of position b each live wedge of position a goes to (from the triangles that hold both); false when a wedge of a
    // has no triangle with b, or two different ones. pairs: x0 y0 x1 y1 ...
    auto mapWedges = [&](uint32_t a, uint32_t b, uint32_t* pairs, uint32_t& nPairs, uint32_t& edgeTris) -> bool {
        nPairs = 0;
        edgeTris = 0;
        for (uint32_t i = S.adjOff[a]; i < S.adjOff[a + 1]; ++i) {
            const uint16_t* t = &S.tri[static_cast<size_t>(S.adjTri[i]) * 3];
            uint32_t x = ~0u, y = ~0u;
            for (int c = 0; c < 3; ++c) {
                if (pid[t[c]] == a) x = t[c];
                else if (pid[t[c]] == b) y = t[c];
            }
            if (y == ~0u) continue;
            ++edgeTris;
            bool found = false;
            for (uint32_t k = 0; k < nPairs; ++k)
                if (pairs[2 * k] == x) { if (pairs[2 * k + 1] != y) return false; found = true; }
            if (!found) {
                if (nPairs == kMaxWedges) return false;
                pairs[2 * nPairs] = x; pairs[2 * nPairs + 1] = y;
                ++nPairs;
            }
        }
        return nPairs == S.nW[a];
    };
    float levelMax = 0.0f;
    uint32_t prevIdx = static_cast<uint32_t>(S.tri.size());
    bool first = true;
    for (uint32_t L = 0; L < in.nLevels; ++L) {
        const float limit = in.levelErr[L];
        if (!(limit > 0.0f)) continue;
        const float lim2 = limit * limit, tolF = in.levelAttrTol[L] > 0.0f ? in.levelAttrTol[L] : 1.0f, attW = lim2 / (tolF * tolF);
        for (uint32_t pass = 0; pass < 48; ++pass) {
            const uint32_t nt = static_cast<uint32_t>(S.tri.size() / 3);
            if (nt < 4) break;
            ++out.passes;
            // ---- adjacency: the triangles of each position
            S.adjOff.assign(nv + 1, 0);
            for (uint32_t i = 0; i < nt * 3; ++i) ++S.adjOff[pid[S.tri[i]] + 1];
            for (uint32_t v = 0; v < nv; ++v) S.adjOff[v + 1] += S.adjOff[v];
            S.adjTri.resize(nt * 3);
            {
                std::vector<uint32_t>& fill = S.table; // (reused as the fill cursors)
                fill.assign(S.adjOff.begin(), S.adjOff.end() - 1);
                for (uint32_t t = 0; t < nt; ++t)
                    for (int c = 0; c < 3; ++c) S.adjTri[fill[pid[S.tri[t * 3 + c]]]++] = t;
            }
            // ---- live wedges per position; open and over-used edges lock their ends
            for (uint32_t v = 0; v < nv; ++v) { S.flag[v] &= 1; S.nW[v] = 0; }
            if (in.locked && first)
                for (uint32_t v = 0; v < nv; ++v)
                    if (in.locked[v]) S.flag[pid[v]] |= 1;
            for (uint32_t i = 0; i < nt * 3; ++i) {
                const uint32_t x = S.tri[i];
                if (S.stamp[x] != ~0u) { S.stamp[x] = ~0u; if (S.nW[pid[x]] < 255) ++S.nW[pid[x]]; }
            }
            for (uint32_t i = 0; i < nt * 3; ++i) S.stamp[S.tri[i]] = 0;
            stampCur = 0;
            S.edges.resize(nt * 3);
            for (uint32_t t = 0; t < nt; ++t)
                for (int c = 0; c < 3; ++c)
                    S.edges[t * 3 + c] = static_cast<uint64_t>(pid[S.tri[t * 3 + c]]) << 32 | pid[S.tri[t * 3 + (c + 1) % 3]];
            std::sort(S.edges.begin(), S.edges.end());
            for (size_t i = 0; i < S.edges.size(); ++i) {
                const uint64_t e = S.edges[i], rev = (e << 32) | (e >> 32);
                const bool dup = (i > 0 && S.edges[i - 1] == e) || (i + 1 < S.edges.size() && S.edges[i + 1] == e);
                if (dup || !std::binary_search(S.edges.begin(), S.edges.end(), rev)) {
                    S.flag[static_cast<uint32_t>(e >> 32)] |= 1;
                    S.flag[static_cast<uint32_t>(e)] |= 1;
                }
            }
            if (first) {
                first = false;
                for (uint32_t v = 0; v < nv; ++v) out.lockedVerts += (pid[v] == v && S.adjOff[v + 1] != S.adjOff[v] && (S.flag[v] & 1)) ? 1u : 0u;
            }
            // ---- candidates: each directed edge a -> b of each triangle
            S.cand.clear();
            for (uint32_t t = 0; t < nt; ++t)
                for (uint32_t c = 0; c < 3; ++c) {
                    const uint32_t a = pid[S.tri[t * 3 + c]], b = pid[S.tri[t * 3 + (c + 1) % 3]];
                    if (S.flag[a] & 1) continue;
                    if (S.nW[a] > kMaxWedges) continue;
                    float d[3];
                    for (int k = 0; k < 3; ++k) d[k] = P[3 * b + k] - P[3 * a + k];
                    float cost = GeoEval(S.gq[a], d);
                    if (!(cost <= lim2)) continue;
                    if (nCh) {
                        uint32_t pairs[2 * kMaxWedges], np, et;
                        if (!mapWedges(a, b, pairs, np, et)) continue;
                        float da[kMaxCh];
                        for (uint32_t k = 0; k < np && cost <= lim2; ++k) {
                            const uint32_t x = pairs[2 * k], y = pairs[2 * k + 1];
                            for (uint32_t q = 0; q < nCh; ++q) da[q] = S.ch[static_cast<size_t>(y) * nCh + q] - S.ch[static_cast<size_t>(x) * nCh + q];
                            cost += attW * AttEval(S.aq[x], &S.chq[static_cast<size_t>(x) * nCh * 4], nCh, d, da);
                        }
                        if (!(cost <= lim2)) continue;
                    }
                    S.cand.push_back(Cand{cost < 0.0f ? 0.0f : cost, t, static_cast<uint8_t>(c)});
                }
            if (S.cand.empty()) break;
            std::sort(S.cand.begin(), S.cand.end(), [](const Cand& p, const Cand& q) { return p.cost < q.cost || (p.cost == q.cost && (p.tri < q.tri || (p.tri == q.tri && p.edge < q.edge))); });
            for (uint32_t v = 0; v < nv; ++v) S.wmap[v] = v;
            uint32_t applied = 0;
            for (const Cand& cd : S.cand) {
                const uint32_t a = pid[S.tri[cd.tri * 3 + cd.edge]], b = pid[S.tri[cd.tri * 3 + (cd.edge + 1) % 3]];
                if ((S.flag[a] & 7) || (S.flag[b] & 6)) continue;
                uint32_t pairs[2 * kMaxWedges], np, et;
                if (!mapWedges(a, b, pairs, np, et) || et != 2) continue;
                // (the stars of a and b are as the adjacency has them only while no neighbour of either moved in this pass; the
                // neighbours of a are marked to count the ones b shares: exactly the two opposite the edge, or the surface pinches)
                const uint32_t st = nextStamp();
                bool ok = true;
                for (uint32_t i = S.adjOff[a]; i < S.adjOff[a + 1] && ok; ++i) {
                    const uint16_t* t = &S.tri[static_cast<size_t>(S.adjTri[i]) * 3];
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t q = pid[t[c]];
                        if (q == a) continue;
                        if (S.flag[q] & 2) { ok = false; break; }
                        S.stamp[q] = st;
                    }
                }
                uint32_t common = 0;
                const uint32_t st2 = ok ? nextStamp() : 0u;
                for (uint32_t i = S.adjOff[b]; i < S.adjOff[b + 1] && ok; ++i) {
                    const uint16_t* t = &S.tri[static_cast<size_t>(S.adjTri[i]) * 3];
                    for (int c = 0; c < 3; ++c) {
                        const uint32_t q = pid[t[c]];
                        if (q == b || q == a) continue;
                        if (S.flag[q] & 2) { ok = false; break; }
                        if (S.stamp[q] == st) { ++common; S.stamp[q] = st2; }
                    }
                }
                if (!ok || common != 2) continue;
                // (no triangle of a turns over or closes up)
                for (uint32_t i = S.adjOff[a]; i < S.adjOff[a + 1] && ok; ++i) {
                    const uint16_t* t = &S.tri[static_cast<size_t>(S.adjTri[i]) * 3];
                    const uint32_t q[3] = {pid[t[0]], pid[t[1]], pid[t[2]]};
                    if (q[0] == b || q[1] == b || q[2] == b) continue;
                    const float* o[3] = {P + 3 * q[0], P + 3 * q[1], P + 3 * q[2]};
                    const float* m[3] = {q[0] == a ? P + 3 * b : o[0], q[1] == a ? P + 3 * b : o[1], q[2] == a ? P + 3 * b : o[2]};
                    float n0[3], n1[3];
                    {
                        const float u[3] = {o[1][0] - o[0][0], o[1][1] - o[0][1], o[1][2] - o[0][2]}, w[3] = {o[2][0] - o[0][0], o[2][1] - o[0][1], o[2][2] - o[0][2]};
                        n0[0] = u[1] * w[2] - u[2] * w[1]; n0[1] = u[2] * w[0] - u[0] * w[2]; n0[2] = u[0] * w[1] - u[1] * w[0];
                    }
                    {
                        const float u[3] = {m[1][0] - m[0][0], m[1][1] - m[0][1], m[1][2] - m[0][2]}, w[3] = {m[2][0] - m[0][0], m[2][1] - m[0][1], m[2][2] - m[0][2]};
                        n1[0] = u[1] * w[2] - u[2] * w[1]; n1[1] = u[2] * w[0] - u[0] * w[2]; n1[2] = u[0] * w[1] - u[1] * w[0];
                    }
                    const float l0 = Dot(n0, n0), l1 = Dot(n1, n1), dp = Dot(n0, n1);
                    if (!(l1 > 1e-12f * l0) || !(dp > 0.25f * std::sqrt(l0 * l1))) ok = false;
                }
                if (!ok) continue;
                // ---- take it: the quadrics of a go to b (re-based), the wedges of a to theirs of b
                float d[3];
                for (int k = 0; k < 3; ++k) d[k] = P[3 * b + k] - P[3 * a + k];
                {
                    const GeoQ& qa = S.gq[a];
                    GeoQ& qb = S.gq[b];
                    float t[3];
                    SymMul(qa.a, d, t);
                    const float e = GeoEval(qa, d);
                    for (int k = 0; k < 6; ++k) qb.a[k] += qa.a[k];
                    for (int k = 0; k < 3; ++k) qb.b[k] += t[k] + qa.b[k];
                    qb.c += e < 0.0f ? 0.0f : e;
                }
                for (uint32_t k = 0; k < np; ++k) {
                    const uint32_t x = pairs[2 * k], y = pairs[2 * k + 1];
                    S.wmap[x] = y;
                    if (!nCh) continue;
                    float da[kMaxCh];
                    for (uint32_t q = 0; q < nCh; ++q) da[q] = S.ch[static_cast<size_t>(y) * nCh + q] - S.ch[static_cast<size_t>(x) * nCh + q];
                    const AttQ& qx = S.aq[x];
                    AttQ& qy = S.aq[y];
                    const float* sx = &S.chq[static_cast<size_t>(x) * nCh * 4];
                    float* sy = &S.chq[static_cast<size_t>(y) * nCh * 4];
                    const float e = AttEval(qx, sx, nCh, d, da);
                    float t[3];
                    SymMul(qx.g, d, t);
                    for (int c = 0; c < 3; ++c) t[c] += qx.h[c];
                    for (uint32_t q = 0; q < nCh; ++q) {
                        for (int c = 0; c < 3; ++c) t[c] -= da[q] * sx[4 * q + c];
                        sy[4 * q + 3] += Dot(sx + 4 * q, d) - qx.n * da[q] + sx[4 * q + 3];
                        for (int c = 0; c < 3; ++c) sy[4 * q + c] += sx[4 * q + c];
                    }
                    for (int c = 0; c < 6; ++c) qy.g[c] += qx.g[c];
                    for (int c = 0; c < 3; ++c) qy.h[c] += t[c];
                    qy.cc += e < 0.0f ? 0.0f : e;
                    qy.n += qx.n;
                }
                S.flag[a] |= 2;
                S.flag[b] |= 4;
                if (cd.cost > levelMax) levelMax = cd.cost;
                ++applied;
            }
            if (!applied) break;
            out.collapses += applied;
            // ---- the triangles through the pass's map, in their order; those of a collapsed edge go
            S.tri2.clear();
            for (uint32_t t = 0; t < nt; ++t) {
                const uint32_t a = S.wmap[S.tri[t * 3]], b = S.wmap[S.tri[t * 3 + 1]], c = S.wmap[S.tri[t * 3 + 2]];
                if (pid[a] == pid[b] || pid[b] == pid[c] || pid[a] == pid[c]) continue;
                S.tri2.push_back(static_cast<uint16_t>(a)); S.tri2.push_back(static_cast<uint16_t>(b)); S.tri2.push_back(static_cast<uint16_t>(c));
            }
            S.tri.swap(S.tri2);
        }
        const uint32_t now = static_cast<uint32_t>(S.tri.size());
        if (now >= 3 && static_cast<float>(now) <= static_cast<float>(prevIdx) * (1.0f - in.minGain)) {
            Level& lv = out.lv[out.nLevels++];
            lv.idx.assign(S.tri.begin(), S.tri.end());
            lv.error = std::min(limit, std::sqrt(levelMax));
            lv.limit = limit;
            prevIdx = now;
        }
    }
    return out.nLevels != 0;
}

// ---- the choice at a draw (vgx_draw.cpp EmitStaticRun)
// The model-space length that shows as one pixel at the nearest corner of the run's box: depth / (focal length in pixels * the
// matrix's scale). 0 when not known (a corner at or behind the eye, an orthographic projection, numbers not finite): no reduced list.
// p: xfReg 0x20.. (p[0] and p[2] the projection's x and y scale), m: the position matrix rows, vpW / vpH: the viewport in pixels.
inline float UnitsPerPixel(const float p[6], bool ortho, const float* const m[3], const float mn[3], const float mx[3], float vpW, float vpH, float* depthOut) {
    if (depthOut) *depthOut = 0.0f;
    if (ortho || !(vpW > 0.0f && vpH > 0.0f && vpW < 1e5f && vpH < 1e5f)) return 0.0f;
    float zmin = 1e30f;
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? mx[0] : mn[0], y = (k & 2) ? mx[1] : mn[1], z = (k & 4) ? mx[2] : mn[2];
        const float d = -(m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]);
        if (!(d > 1e-3f)) return 0.0f;
        if (d < zmin) zmin = d;
    }
    float s2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float n = m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c];
        if (n > s2) s2 = n;
    }
    const float focal = std::max(std::fabs(p[0]) * vpW, std::fabs(p[2]) * vpH) * 0.5f, scale = std::sqrt(s2);
    const float u = zmin / (focal * scale);
    if (!(focal > 0.0f && scale > 0.0f) || !std::isfinite(u)) return 0.0f;
    if (depthOut) *depthOut = zmin;
    return u;
}
// The level to draw: the highest whose error is at most px * unitsPerPixel (errors ascending); -1: the run's own list.
inline int Choose(const float* errors, uint32_t nLevels, float unitsPerPixel, float px) {
    if (!(unitsPerPixel > 0.0f) || !(px > 0.0f)) return -1;
    const float allow = unitsPerPixel * px;
    int best = -1;
    for (uint32_t i = 0; i < nLevels; ++i)
        if (errors[i] <= allow) best = static_cast<int>(i);
    return best;
}

}  // namespace farlod
}  // namespace vgx
