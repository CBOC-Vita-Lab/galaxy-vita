// GXV_FARLOD (farlod32): vgx_farlod.h's reduced triangle lists.
//   * synthetic meshes: flat grid (interior collapses, border kept edge for edge), closed sphere (stays closed, the measured distance
//     of every model vertex to the reduced surface is within the level's bound), colour ramp against colour checker (attributes hold
//     collapses back only where they bend), a cylinder with a texture seam (stays closed), locked vertices (their triangles are
//     untouched), every level a list of valid, non-degenerate triangles of the caller's vertices, shorter than the one before,
//     the same result twice; the draw-time choice (UnitsPerPixel, Choose).
//   * --mesh <file> [px]: runs on meshes extracted from the game's models by tools/farlod_extract.py and prints, per distance, the
//     indices left (the estimate table of new/farlod32/REPORT.txt). Not part of the pass / fail result.
// Build: clang++ -std=c++17 -O1 -Iplatform/vgx/src tests/vgx_farlod.cpp -o vgx_farlod && ./vgx_farlod
#include "vgx_farlod.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace vgx;

namespace {
unsigned failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
struct V { float p[4]; float n[3]; uint8_t c[4]; float uv[2]; };
const farlod::Attr kAttrs[3] = {{16, 0, 3, 0.15f}, {28, 1, 4, 8.0f / 255.0f}, {32, 0, 2, 0.02f}};

struct Mesh { std::vector<V> v; std::vector<uint16_t> i; };
uint64_t PosKey(const V& v) { uint32_t w[3]; std::memcpy(w, v.p, 12); return (uint64_t)w[0] * 0x9E3779B97F4A7C15ull ^ (uint64_t)w[1] * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)w[2] * 0x165667B19E3779F9ull; }
// open edges in position space (directed edges without their reverse)
std::set<std::pair<uint64_t, uint64_t>> OpenEdges(const Mesh& m, const std::vector<uint16_t>& idx) {
    std::map<std::pair<uint64_t, uint64_t>, int> e;
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int c = 0; c < 3; ++c) ++e[{PosKey(m.v[idx[t + c]]), PosKey(m.v[idx[t + (c + 1) % 3]])}];
    std::set<std::pair<uint64_t, uint64_t>> open;
    for (auto& kv : e)
        if (!e.count({kv.first.second, kv.first.first})) open.insert(kv.first);
    return open;
}
float PointTri(const float* p, const float* a, const float* b, const float* c) {
    // (Ericson, Real-Time Collision Detection 5.1.5)
    auto sub = [](const float* x, const float* y, float* o) { for (int k = 0; k < 3; ++k) o[k] = x[k] - y[k]; };
    auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    float ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    auto dist = [&](const float* x) { float d[3]; sub(p, x, d); return std::sqrt(dot(d, d)); };
    if (d1 <= 0 && d2 <= 0) return dist(a);
    sub(p, b, bp);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return dist(b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) q[k] = a[k] + v * ab[k]; return dist(q); }
    sub(p, c, cp);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return dist(c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) q[k] = a[k] + w * ac[k]; return dist(q); }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; ++k) q[k] = b[k] + w * (c[k] - b[k]); return dist(q); }
    const float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * v + ac[k] * w;
    return dist(q);
}
// the largest distance of a model vertex to the reduced surface
float MaxDist(const Mesh& m, const std::vector<uint16_t>& idx) {
    float worst = 0;
    for (const V& v : m.v) {
        float best = 1e30f;
        for (size_t t = 0; t + 2 < idx.size() && best > 0; t += 3) best = std::min(best, PointTri(v.p, m.v[idx[t]].p, m.v[idx[t + 1]].p, m.v[idx[t + 2]].p));
        worst = std::max(worst, best);
    }
    return worst;
}
Mesh Grid(uint32_t w, uint32_t h, float cell, int colourMode, float bump) {
    Mesh m;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            V v{};
            v.p[0] = x * cell; v.p[2] = y * cell;
            v.p[1] = bump * std::sin(x * 0.7f) * std::cos(y * 0.5f);
            v.n[1] = 1;
            const uint8_t ramp = static_cast<uint8_t>(x * 255 / (w - 1)), chk = ((x / 2 + y / 2) & 1) ? 255 : 40;
            const uint8_t c = colourMode == 0 ? 200 : colourMode == 1 ? ramp : chk;
            v.c[0] = v.c[1] = v.c[2] = c; v.c[3] = 255;
            v.uv[0] = x * 0.25f; v.uv[1] = y * 0.25f;
            m.v.push_back(v);
        }
    for (uint32_t y = 0; y + 1 < h; ++y)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const uint16_t a = static_cast<uint16_t>(y * w + x), b = a + 1, c = static_cast<uint16_t>(a + w), d = c + 1;
            for (uint16_t q : {a, c, b, b, c, d}) m.i.push_back(q);
        }
    return m;
}
// closed: rings x segs, the seam column either shared (seam = false) or doubled with different texture coordinates
Mesh Sphere(uint32_t rings, uint32_t segs, float r, bool seam, bool cylinder) {
    Mesh m;
    const uint32_t cols = seam ? segs + 1 : segs;
    auto at = [&](uint32_t ring, uint32_t s) { return static_cast<uint16_t>(1 + (ring - 1) * cols + (seam ? s : s % segs)); };
    V top{}, bot{};
    top.p[1] = r; top.n[1] = 1; bot.p[1] = -r; bot.n[1] = -1;
    top.c[0] = top.c[1] = top.c[2] = top.c[3] = bot.c[0] = bot.c[1] = bot.c[2] = bot.c[3] = 255;
    top.uv[0] = bot.uv[0] = 0.5f; bot.uv[1] = 1;
    m.v.push_back(top);
    for (uint32_t ring = 1; ring < rings; ++ring)
        for (uint32_t s = 0; s < cols; ++s) {
            const float th = 3.14159265f * ring / rings, ph = 6.2831853f * (s % segs) / segs;
            V v{};
            const float rad = cylinder ? r : r * std::sin(th);
            v.p[0] = rad * std::cos(ph); v.p[1] = r * std::cos(th); v.p[2] = rad * std::sin(ph);
            const float l = std::sqrt(v.p[0] * v.p[0] + v.p[1] * v.p[1] + v.p[2] * v.p[2]);
            for (int k = 0; k < 3; ++k) v.n[k] = v.p[k] / l;
            v.c[0] = v.c[1] = v.c[2] = v.c[3] = 255;
            v.uv[0] = static_cast<float>(s) / segs; v.uv[1] = static_cast<float>(ring) / rings;
            m.v.push_back(v);
        }
    m.v.push_back(bot);
    const uint16_t last = static_cast<uint16_t>(m.v.size() - 1);
    for (uint32_t s = 0; s < segs; ++s) {
        for (uint16_t q : {(uint16_t)0, at(1, s + 1), at(1, s)}) m.i.push_back(q);
        for (uint32_t ring = 1; ring + 1 < rings; ++ring)
            for (uint16_t q : {at(ring, s), at(ring, s + 1), at(ring + 1, s), at(ring + 1, s), at(ring, s + 1), at(ring + 1, s + 1)}) m.i.push_back(q);
        for (uint16_t q : {last, at(rings - 1, s), at(rings - 1, s + 1)}) m.i.push_back(q);
    }
    return m;
}
farlod::Input In(const Mesh& m, std::initializer_list<float> errs, bool attrs) {
    farlod::Input in;
    in.vtx = reinterpret_cast<const uint8_t*>(m.v.data());
    in.vtxCount = static_cast<uint32_t>(m.v.size());
    in.stride = sizeof(V);
    in.idx = m.i.data();
    in.idxCount = static_cast<uint32_t>(m.i.size());
    in.attrs = attrs ? kAttrs : nullptr;
    in.nAttrs = attrs ? 3 : 0;
    for (float e : errs) { in.levelErr[in.nLevels] = e; in.levelAttrTol[in.nLevels] = 1.0f; ++in.nLevels; }
    in.minGain = 0.05f;
    return in;
}
void Valid(const Mesh& m, const farlod::Result& r, const char* name) {
    uint32_t prev = static_cast<uint32_t>(m.i.size());
    float prevErr = 0;
    for (uint32_t L = 0; L < r.nLevels; ++L) {
        const auto& ix = r.lv[L].idx;
        bool ok = !ix.empty() && ix.size() % 3 == 0 && ix.size() < prev && r.lv[L].error <= r.lv[L].limit && r.lv[L].error >= prevErr;
        for (size_t t = 0; t + 2 < ix.size() && ok; t += 3) {
            ok = ix[t] < m.v.size() && ix[t + 1] < m.v.size() && ix[t + 2] < m.v.size();
            ok = ok && PosKey(m.v[ix[t]]) != PosKey(m.v[ix[t + 1]]) && PosKey(m.v[ix[t + 1]]) != PosKey(m.v[ix[t + 2]]) && PosKey(m.v[ix[t]]) != PosKey(m.v[ix[t + 2]]);
        }
        if (!ok) { std::printf("FAIL: %s: level %u is not a valid shorter list\n", name, L); ++failures; }
        prev = static_cast<uint32_t>(ix.size());
        prevErr = r.lv[L].error;
    }
}

int RunMeshFile(const char* path, float px, uint32_t maxVerts);
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--mesh")) return RunMeshFile(argv[2], argc >= 4 ? static_cast<float>(std::atof(argv[3])) : 1.0f, argc >= 5 ? static_cast<uint32_t>(std::atoi(argv[4])) : 65536u);
    farlod::Scratch S;
    farlod::Result r;
    { // flat grid, no attributes: nearly everything inside goes, the border stays edge for edge
        const Mesh m = Grid(33, 33, 100.0f, 0, 0.0f);
        Check(farlod::Build(In(m, {1.0f}, false), r, S), "flat grid: a level is made");
        Valid(m, r, "flat grid");
        Check(r.nLevels == 1 && r.lv[0].idx.size() * 4 < m.i.size(), "flat grid: under a quarter of the indices are left");
        Check(OpenEdges(m, m.i) == OpenEdges(m, r.lv[0].idx), "flat grid: the border is the model's, edge for edge");
        Check(MaxDist(m, r.lv[0].idx) < 1.0f, "flat grid: the surface did not move (within the point-to-triangle rounding on 3200-unit slivers)");
        std::printf("flat grid 33x33: %u -> %u indices, error %.3f, %u collapses in %u passes, %u locked of %u positions\n", (unsigned)m.i.size(), (unsigned)r.lv[0].idx.size(),
                    r.lv[0].error, r.collapses, r.passes, r.lockedVerts, r.positions);
        farlod::Result r2;
        farlod::Scratch S2;
        farlod::Build(In(m, {1.0f}, false), r2, S2);
        Check(r2.nLevels == r.nLevels && r2.lv[0].idx == r.lv[0].idx, "the same input gives the same list");
    }
    { // flat grid with attributes: a uniform colour and a colour ramp collapse; a checker pattern does not
        const Mesh a = Grid(33, 33, 100.0f, 0, 0.0f), b = Grid(33, 33, 100.0f, 1, 0.0f), c = Grid(33, 33, 100.0f, 2, 0.0f);
        farlod::Result ra, rb, rc;
        farlod::Build(In(a, {1.0f}, true), ra, S);
        farlod::Build(In(b, {1.0f}, true), rb, S);
        const bool mc = farlod::Build(In(c, {1.0f}, true), rc, S);
        Valid(a, ra, "uniform grid"); Valid(b, rb, "ramp grid");
        Check(ra.nLevels == 1 && ra.lv[0].idx.size() * 4 < a.i.size(), "uniform colour, linear texture coordinates: collapses as without attributes");
        Check(rb.nLevels == 1 && rb.lv[0].idx.size() * 2 < b.i.size(), "colour ramp: collapses (the ramp is linear up to its rounding)");
        Check(!mc || rc.lv[0].idx.size() * 10 > c.i.size() * 9, "colour checker: (nearly) nothing collapses");
        std::printf("attributes: uniform %u, ramp %u, checker %u of %u indices\n", ra.nLevels ? (unsigned)ra.lv[0].idx.size() : 0u, rb.nLevels ? (unsigned)rb.lv[0].idx.size() : 0u,
                    mc ? (unsigned)rc.lv[0].idx.size() : (unsigned)c.i.size(), (unsigned)c.i.size());
    }
    { // closed sphere, radius 1000: three levels, each within its bound, still closed
        const Mesh m = Sphere(32, 48, 1000.0f, false, false);
        Check(OpenEdges(m, m.i).empty(), "(the test sphere is closed)");
        Check(farlod::Build(In(m, {2.0f, 8.0f, 32.0f}, false), r, S), "sphere: levels are made");
        Valid(m, r, "sphere");
        Check(r.nLevels >= 2, "sphere: at least two levels");
        for (uint32_t L = 0; L < r.nLevels; ++L) {
            const float d = MaxDist(m, r.lv[L].idx);
            std::printf("sphere r=1000 (%u indices): level %u limit %.0f: %u indices, bound %.2f, measured vertex distance %.2f\n", (unsigned)m.i.size(), L, r.lv[L].limit,
                        (unsigned)r.lv[L].idx.size(), r.lv[L].error, d);
            Check(d <= r.lv[L].limit * 1.01f, "sphere: every model vertex is within the level's limit of the reduced surface");
            Check(OpenEdges(m, r.lv[L].idx).empty(), "sphere: the reduced list is closed");
        }
        Check(r.lv[r.nLevels - 1].idx.size() * 3 < m.i.size(), "sphere: the last level has under a third of the indices");
    }
    { // bumpy ground with attributes: bound holds
        const Mesh m = Grid(40, 40, 100.0f, 0, 30.0f);
        Check(farlod::Build(In(m, {5.0f, 20.0f}, true), r, S), "bumpy ground: levels are made");
        Valid(m, r, "bumpy ground");
        for (uint32_t L = 0; L < r.nLevels; ++L) {
            const float d = MaxDist(m, r.lv[L].idx);
            std::printf("bumpy ground (%u indices): level %u limit %.0f: %u indices, bound %.2f, measured %.2f\n", (unsigned)m.i.size(), L, r.lv[L].limit, (unsigned)r.lv[L].idx.size(), r.lv[L].error, d);
            Check(d <= r.lv[L].limit * 1.01f, "bumpy ground: within the limit");
            Check(OpenEdges(m, m.i) == OpenEdges(m, r.lv[L].idx), "bumpy ground: the border is the model's");
        }
    }
    { // a closed capsule-like cylinder with a texture seam: stays closed, both sides of the seam go together
        const Mesh m = Sphere(24, 32, 500.0f, true, true);
        Check(OpenEdges(m, m.i).empty(), "(the seam cylinder is closed in position space)");
        const bool made = farlod::Build(In(m, {4.0f, 40.0f}, true), r, S);
        Check(made, "seam cylinder: levels are made");
        Valid(m, r, "seam cylinder");
        for (uint32_t L = 0; L < r.nLevels; ++L) {
            Check(OpenEdges(m, r.lv[L].idx).empty(), "seam cylinder: no crack along the seam");
            std::printf("seam cylinder (%u indices): level %u: %u indices, bound %.2f, measured %.2f\n", (unsigned)m.i.size(), L, (unsigned)r.lv[L].idx.size(), r.lv[L].error, MaxDist(m, r.lv[L].idx));
        }
    }
    { // locked vertices: a triangle whose three vertices are locked is in every level
        const Mesh m = Grid(33, 33, 100.0f, 0, 0.0f);
        std::vector<uint8_t> lock(m.v.size(), 0);
        for (uint32_t y = 10; y < 20; ++y)
            for (uint32_t x = 10; x < 20; ++x) lock[y * 33 + x] = 1;
        farlod::Input in = In(m, {1.0f}, false);
        in.locked = lock.data();
        Check(farlod::Build(in, r, S), "locked patch: a level is made");
        std::set<std::tuple<uint16_t, uint16_t, uint16_t>> have;
        for (size_t t = 0; t + 2 < r.lv[0].idx.size(); t += 3) have.insert({r.lv[0].idx[t], r.lv[0].idx[t + 1], r.lv[0].idx[t + 2]});
        bool all = true;
        uint32_t n = 0;
        for (size_t t = 0; t + 2 < m.i.size(); t += 3)
            if (lock[m.i[t]] && lock[m.i[t + 1]] && lock[m.i[t + 2]]) { ++n; all = all && have.count({m.i[t], m.i[t + 1], m.i[t + 2]}); }
        Check(all && n == 162, "locked patch: its 162 triangles are in the level unchanged");
        // (order: the level's triangles that are also the model's come in the model's order)
        std::map<std::tuple<uint16_t, uint16_t, uint16_t>, size_t> where;
        for (size_t t = 0; t + 2 < m.i.size(); t += 3) where[{m.i[t], m.i[t + 1], m.i[t + 2]}] = t;
        size_t lastAt = 0;
        bool ordered = true;
        for (size_t t = 0; t + 2 < r.lv[0].idx.size(); t += 3) {
            auto it = where.find({r.lv[0].idx[t], r.lv[0].idx[t + 1], r.lv[0].idx[t + 2]});
            if (it == where.end()) continue;
            ordered = ordered && it->second >= lastAt;
            lastAt = it->second;
        }
        Check(ordered, "the triangles that are left keep the model's order");
    }
    { // refused input
        const Mesh m = Grid(4, 4, 1.0f, 0, 0.0f);
        farlod::Input in = In(m, {1.0f}, false);
        in.idxCount = 7;
        Check(!farlod::Build(in, r, S) && r.nLevels == 0, "an index count that is not whole triangles is refused");
        Mesh bad = m;
        bad.v[3].p[0] = NAN;
        Check(!farlod::Build(In(bad, {1.0f}, false), r, S), "a position that is not a number is refused");
        Mesh oob = m;
        oob.i[5] = 999;
        Check(!farlod::Build(In(oob, {1.0f}, false), r, S), "an index outside the vertices is refused");
    }
    { // the draw-time choice
        const float p[6] = {1.3f, 0, 2.414f, 0, 0, 0}; // fovy 45 degrees
        const float r0[4] = {1, 0, 0, 0}, r1[4] = {0, 1, 0, 0}, r2[4] = {0, 0, 1, -5000.0f};
        const float* const m[3] = {r0, r1, r2};
        const float mn[3] = {-100, -100, -100}, mx[3] = {100, 100, 100};
        float depth = 0;
        const float u = farlod::UnitsPerPixel(p, false, m, mn, mx, 640.0f, 456.0f, &depth);
        Check(std::fabs(depth - 4900.0f) < 0.5f, "the depth is the nearest corner's");
        Check(std::fabs(u - 4900.0f / (2.414f * 228.0f)) < 0.01f, "units per pixel = depth / focal length in pixels");
        const float errs[3] = {4.0f, 12.0f, 40.0f};
        Check(farlod::Choose(errs, 3, u, 1.0f) == 0 && farlod::Choose(errs, 3, u, 2.0f) == 1 && farlod::Choose(errs, 3, u, 0.25f) == -1, "the coarsest level under the pixel limit is chosen");
        const float s0[4] = {2, 0, 0, 0}, s1[4] = {0, 2, 0, 0}, s2[4] = {0, 0, 2, -5000.0f};
        const float* const ms[3] = {s0, s1, s2};
        Check(std::fabs(farlod::UnitsPerPixel(p, false, ms, mn, mx, 640.0f, 456.0f, nullptr) - 4800.0f / (2.414f * 228.0f * 2.0f)) < 0.01f, "a scaled model: its units are larger on screen");
        const float n2[4] = {0, 0, 1, -50.0f};
        const float* const mnr[3] = {r0, r1, n2};
        Check(farlod::UnitsPerPixel(p, false, mnr, mn, mx, 640.0f, 456.0f, nullptr) == 0.0f, "a box that reaches the eye: not known, no reduced list");
        Check(farlod::UnitsPerPixel(p, true, m, mn, mx, 640.0f, 456.0f, nullptr) == 0.0f, "orthographic: no reduced list");
        Check(farlod::Choose(errs, 3, 0.0f, 1.0f) == -1, "not known: the run's own list");
    }
    std::printf(failures ? "vgx_farlod: %u FAILED\n" : "vgx_farlod: all passed\n", failures);
    return failures ? 1 : 0;
}

namespace {
// File of tools/farlod_extract.py: "FLD1", u32 shapes; per shape: char name[48], u32 flags (1 skinned, 2 nrm, 4 col, 8 tex), u32 nv, u32 ni,
// nv x 12 floats (pos 3, nrm 3, col 4 [0..1], uv 2), ni x u32.
int RunMeshFile(const char* path, float px, uint32_t maxVerts) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    char magic[4];
    uint32_t n = 0;
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, "FLD1", 4) || std::fread(&n, 4, 1, f) != 1) return 2;
    // 1 px (GX viewport, fovy 45 degrees over 456 lines: 550 px focal length) at 25 / 50 / 100 / 200 / 400 m
    const float kDist[5] = {2500, 5000, 10000, 20000, 40000};
    const float focal = 228.0f / std::tan(0.3927f);
    uint64_t total = 0, skinned = 0, tooBig = 0, small = 0, left[5] = {}, eligible = 0, lockedV = 0, posV = 0;
    double ms = 0;
    farlod::Scratch S;
    for (uint32_t s = 0; s < n; ++s) {
        char name[48];
        uint32_t hd[3];
        if (std::fread(name, 1, 48, f) != 48 || std::fread(hd, 4, 3, f) != 3) break;
        std::vector<float> fv(static_cast<size_t>(hd[1]) * 12);
        std::vector<uint32_t> ix(hd[2]);
        if (std::fread(fv.data(), 4, fv.size(), f) != fv.size() || std::fread(ix.data(), 4, ix.size(), f) != ix.size()) break;
        total += hd[2];
        if (hd[0] & 1) { skinned += hd[2]; for (auto& l : left) l += hd[2]; continue; }
        if (hd[1] > maxVerts || hd[1] > 0xFFFF) { tooBig += hd[2]; for (auto& l : left) l += hd[2]; continue; }
        if (hd[2] < 48) { small += hd[2]; for (auto& l : left) l += hd[2]; continue; }
        Mesh m;
        m.v.resize(hd[1]);
        for (uint32_t v = 0; v < hd[1]; ++v) {
            const float* q = &fv[static_cast<size_t>(v) * 12];
            V& o = m.v[v];
            o.p[0] = q[0]; o.p[1] = q[1]; o.p[2] = q[2]; o.p[3] = 0;
            o.n[0] = q[3]; o.n[1] = q[4]; o.n[2] = q[5];
            for (int k = 0; k < 4; ++k) o.c[k] = static_cast<uint8_t>(std::lround(std::clamp(q[6 + k], 0.0f, 1.0f) * 255.0f));
            o.uv[0] = q[10]; o.uv[1] = q[11];
        }
        m.i.assign(ix.begin(), ix.end());
        farlod::Attr at[3] = {kAttrs[0], kAttrs[1], kAttrs[2]};
        if (!(hd[0] & 2)) at[0].count = 0;
        if (!(hd[0] & 4)) at[1].count = 0;
        if (!(hd[0] & 8)) at[2].count = 0;
        // (exploration: FARLOD_TEST_ATTR = a mask of the attributes kept [1 normal, 2 colour, 4 texture], FARLOD_TEST_TOL = a factor on the tolerances)
        if (const char* e = std::getenv("FARLOD_TEST_ATTR")) { const int mk = std::atoi(e); for (int k = 0; k < 3; ++k) if (!(mk >> k & 1)) at[k].count = 0; }
        if (const char* e = std::getenv("FARLOD_TEST_TOL")) for (int k = 0; k < 3; ++k) at[k].tol *= static_cast<float>(std::atof(e));
        eligible += hd[2];
        // five separate builds, one per distance (the shipped feature keeps three levels; this is the curve)
        for (int d = 0; d < 5; ++d) {
            farlod::Input in = In(m, {px * kDist[d] / focal}, true);
            in.attrs = at;
            in.levelAttrTol[0] = d < 2 ? 1.0f : d < 4 ? 2.0f : 4.0f;
            in.minGain = 0.0f;
            farlod::Result r;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = farlod::Build(in, r, S);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            left[d] += ok && r.nLevels ? r.lv[0].idx.size() : hd[2];
            if (d == 0) { lockedV += r.lockedVerts; posV += r.positions; }
        }
    }
    std::fclose(f);
    std::printf("%s: %u shapes, %llu indices (skinned %llu, over %u vertices %llu, under 48 indices %llu, eligible %llu; locked %llu of %llu positions); left at %.2f px:", path, n,
                (unsigned long long)total, (unsigned long long)skinned, maxVerts, (unsigned long long)tooBig, (unsigned long long)small, (unsigned long long)eligible,
                (unsigned long long)lockedV, (unsigned long long)posV, px);
    for (int d = 0; d < 5; ++d) std::printf(" %.0fm %llu (%.0f%%)", kDist[d] / 100.0f, (unsigned long long)left[d], total ? 100.0 * left[d] / total : 0.0);
    std::printf("; host build time %.0f ms\n", ms);
    return 0;
}
}  // namespace
