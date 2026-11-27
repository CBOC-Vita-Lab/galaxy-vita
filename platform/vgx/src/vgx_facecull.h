// GXV_FACECULL (farlod32, 13 Nov 2026): a rigid static run drawn without the triangles that face away from the camera.
//
// The game's map shapes are closed surfaces drawn with back-face culling: from any place about half of a planet's triangles face away
// (measured on five map models from the disc: 47-53 % of their indices, tools/farlod_extract.py). The GPU still receives every index
// of such a run, runs the vertex program for its vertices and only then throws the triangle away. Here the front end keeps, per run, a
// "view list": the run's triangles that can face the camera from where the camera is, and a compact copy of the vertices they use, in a
// static buffer of its own, and draws that instead. The list stays right while the camera (in the model's space) is within `margin` of
// the place it was made for; then it is made again (a budget per picture; while there is no valid list the run is drawn whole, as before).
//
// Exactness. GX draws a triangle (p0, p1, p2) when it is front-facing (clockwise on the screen) under GX_CULL_BACK, back-facing under
// GX_CULL_FRONT. With n = (p1 - p0) x (p2 - p0) and the eye E in model space, the triangle is counter-clockwise on the screen exactly
// when k * n.(E - p0) > 0, k = the sign of the position matrix's determinant times the signs of the projection's and the viewport's x
// and y scales (+1 for a plain camera): a property of the eye point and the plane, the same for every vertex of the triangle and for
// the pieces the clipper leaves of it. A triangle is left out only if k * n.(E - p0) is beyond a threshold on the culled side for the
// eye the list was made for, the threshold being the margin plus the error of the stored plane (the unit normal is kept in 3 x 8 bits:
// off by at most 0.0069 times the distance to the eye, taken as 0.0075 times the largest distance from the eye to the run) plus one
// unit for rounding. For every eye within the margin the triangle is then still on the culled side, so the GPU would have culled it:
// the picture is the same, pixel for pixel. The triangles that are left keep their order and winding (blended runs draw as before).
// Zero-area triangles have a zero plane and are always kept. Not used: per-vertex matrices, orthographic projections, GX_CULL_NONE.
//
// Pure functions and plain data; the buffers, the budget and the log are in vgx_draw.cpp ("view lists") and vgx_dlcache.cpp (Promote).
// Host test: tests/vgx_facecull.cpp (this very file).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vgx {
namespace facecull {

struct Plane {
    int8_t n[3];  // the unit normal * 127, rounded
    int8_t zero;  // 1: a zero-area triangle (always kept)
    float d;      // (n / 127) . p0
};
// What Promote keeps of an eligible run (CPU side) and the state of its view list.
struct Run {
    std::vector<uint16_t> idx;    // the run's triangles, indices within its vertex block
    std::vector<Plane> planes;    // one per triangle
    std::vector<uint8_t> vtx;     // the run's vertex block as the static buffer holds it
    uint32_t stride = 0, vtxCount = 0;
    float c[3] = {}, r = 0;       // the positions' bounding sphere (of their box)
    // the view list
    uint32_t bufId = 0;           // its static buffer (0: none)
    uint32_t idxKept = 0, vtxKept = 0;
    float eye[3] = {}, margin = 0;
    int8_t side = 0;              // +1: triangles with k.f > threshold were left out, -1: with k.f < -threshold
    uint8_t conflicts = 0;
    uint8_t sleepWhy = 0;         // why it sleeps (for the log): 1 a list that left out too little, 2 drawn from several places a picture
    uint16_t sleepLen = 0;
    uint64_t builtFrame = 0, usedFrame = ~0ull, sleepUntil = 0;
    uint32_t Bytes() const { return static_cast<uint32_t>(idx.capacity() * 2 + planes.capacity() * sizeof(Plane) + vtx.capacity()); }
};

constexpr float kQuantSlack = 0.0075f; // (sqrt(3) * 0.5 / 127 = 0.00682, and room for the rounding of the products)

// The planes and the bounding sphere of a run: positions are 3 x f32 at offset 0 of each vertex. False for input it cannot take
// (an index outside the block, a position that is not finite).
inline bool BuildPlanes(const uint8_t* vtx, uint32_t vtxCount, uint32_t stride, const uint16_t* idx, uint32_t idxCount, std::vector<Plane>& out, float c[3], float* r) {
    out.clear();
    if (!vtx || !idx || stride < 12 || vtxCount < 3 || idxCount < 3 || idxCount % 3 != 0) return false;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t v = 0; v < vtxCount; ++v) {
        float p[3];
        std::memcpy(p, vtx + static_cast<size_t>(v) * stride, 12);
        for (int a = 0; a < 3; ++a) {
            if (!std::isfinite(p[a])) return false;
            if (p[a] < mn[a]) mn[a] = p[a];
            if (p[a] > mx[a]) mx[a] = p[a];
        }
    }
    float rr = 0;
    for (int a = 0; a < 3; ++a) { c[a] = 0.5f * (mn[a] + mx[a]); const float h = 0.5f * (mx[a] - mn[a]); rr += h * h; }
    *r = std::sqrt(rr) * 1.0001f;
    out.resize(idxCount / 3);
    for (uint32_t t = 0; t < idxCount / 3; ++t) {
        const uint32_t i0 = idx[t * 3], i1 = idx[t * 3 + 1], i2 = idx[t * 3 + 2];
        if (i0 >= vtxCount || i1 >= vtxCount || i2 >= vtxCount) { out.clear(); return false; }
        float p0[3], p1[3], p2[3];
        std::memcpy(p0, vtx + static_cast<size_t>(i0) * stride, 12);
        std::memcpy(p1, vtx + static_cast<size_t>(i1) * stride, 12);
        std::memcpy(p2, vtx + static_cast<size_t>(i2) * stride, 12);
        // (in double: the cross product of two edges of a stage-sized model)
        const double e1[3] = {double(p1[0]) - p0[0], double(p1[1]) - p0[1], double(p1[2]) - p0[2]}, e2[3] = {double(p2[0]) - p0[0], double(p2[1]) - p0[1], double(p2[2]) - p0[2]};
        const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        Plane& pl = out[t];
        if (!(len > 1e-30) || !std::isfinite(len)) { pl.n[0] = pl.n[1] = pl.n[2] = 0; pl.zero = 1; pl.d = 0; continue; }
        pl.zero = 0;
        for (int a = 0; a < 3; ++a) pl.n[a] = static_cast<int8_t>(std::lround(n[a] / len * 127.0));
        // (about the sphere's centre, so that d is small: the test adds the centre back)
        pl.d = static_cast<float>((pl.n[0] * (double(p0[0]) - c[0]) + pl.n[1] * (double(p0[1]) - c[1]) + pl.n[2] * (double(p0[2]) - c[2])) / 127.0);
    }
    return true;
}

// The eye in model space (E = -R^-1 t for the position matrix [R | t]) and the sign of det R. False when R is singular or not finite.
inline bool EyeOf(const float* const m[3], float eye[3], int* detSign) {
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
    const double c0 = e * i - f * h, c1 = f * g - d * i, c2 = d * h - e * g;
    const double det = a * c0 + b * c1 + c * c2;
    const double scale = std::fabs(a * c0) + std::fabs(b * c1) + std::fabs(c * c2);
    if (!std::isfinite(det) || !(std::fabs(det) > 1e-9 * scale) || !(scale > 0.0)) return false;
    const double t[3] = {m[0][3], m[1][3], m[2][3]};
    // R^-1 = adj(R) / det
    const double inv[3][3] = {{c0, c * h - b * i, b * f - c * e}, {c1, a * i - c * g, c * d - a * f}, {c2, b * g - a * h, a * e - b * d}};
    for (int r = 0; r < 3; ++r) {
        const double v = -(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]) / det;
        if (!std::isfinite(v)) return false;
        eye[r] = static_cast<float>(v);
    }
    *detSign = det > 0 ? 1 : -1;
    return true;
}
// k (see the file's head): detSign and the signs of the projection's x / y scales (p[0], p[2]) and of the viewport's (xfReg 0x1A: half
// the width, 0x1B: minus half the height). 0: not known (a zero scale).
inline int Handedness(int detSign, float projX, float projY, float vpScaleX, float vpScaleY) {
    if (projX == 0.0f || projY == 0.0f || vpScaleX == 0.0f || vpScaleY == 0.0f || !std::isfinite(projX) || !std::isfinite(projY)) return 0;
    int k = detSign;
    if (projX < 0) k = -k;
    if (projY < 0) k = -k;
    if (vpScaleX < 0) k = -k;
    if (vpScaleY > 0) k = -k; // (GX keeps minus half the height: a positive value is a viewport turned over)
    return k;
}
// The largest distance from the eye to the run, and the threshold of a list made at this eye with this margin.
inline float Reach(const Run& r, const float eye[3]) {
    const float d[3] = {eye[0] - r.c[0], eye[1] - r.c[1], eye[2] - r.c[2]};
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + r.r;
}
inline float Threshold(float margin, float reach) { return margin + kQuantSlack * reach + 1.0f; }

// side = +1: GX_CULL_BACK with k = +1 (or GX_CULL_FRONT with k = -1): the triangles with f > 0 are the culled ones; -1: those with f < 0.
// Writes the kept triangles (indices into the compact vertex list) to outIdx and the compact vertices to outVtx; remap is scratch.
// invert (a test picture): keeps exactly the triangles that would be left out. Returns the indices kept.
inline uint32_t Filter(const Run& r, const float eye[3], int side, float threshold, bool invert, std::vector<uint16_t>& outIdx, std::vector<uint8_t>& outVtx,
                       std::vector<uint16_t>& remap) {
    outIdx.clear();
    outVtx.clear();
    remap.assign(r.vtxCount, 0xFFFF);
    const float ex = (eye[0] - r.c[0]) / 127.0f, ey = (eye[1] - r.c[1]) / 127.0f, ez = (eye[2] - r.c[2]) / 127.0f;
    const float s = static_cast<float>(side);
    const uint32_t tris = static_cast<uint32_t>(r.planes.size());
    uint32_t nv = 0;
    for (uint32_t t = 0; t < tris; ++t) {
        const Plane& pl = r.planes[t];
        const float f = pl.n[0] * ex + pl.n[1] * ey + pl.n[2] * ez - pl.d;
        const bool drop = !pl.zero && s * f > threshold;
        if (drop != invert) continue;
        for (int k = 0; k < 3; ++k) {
            const uint16_t v = r.idx[t * 3 + k];
            uint16_t m = remap[v];
            if (m == 0xFFFF) {
                m = remap[v] = static_cast<uint16_t>(nv++);
                const uint8_t* p = r.vtx.data() + static_cast<size_t>(v) * r.stride;
                outVtx.insert(outVtx.end(), p, p + r.stride);
            }
            outIdx.push_back(m);
        }
    }
    return static_cast<uint32_t>(outIdx.size());
}
// Whether the list made at r.eye for `side` (r.margin > 0: there is one) may be drawn from `eye` for `side`.
inline bool Valid(const Run& r, const float eye[3], int side) {
    if (!(r.margin > 0.0f) || r.side != side) return false;
    const float d[3] = {eye[0] - r.eye[0], eye[1] - r.eye[1], eye[2] - r.eye[2]};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r.margin * r.margin;
}

}  // namespace facecull
}  // namespace vgx
