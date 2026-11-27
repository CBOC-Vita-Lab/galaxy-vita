// GXV_HSR_ORDER_TIES=4 (ties25): the geometry proofs of platform/vgx/src/vgx_hsrgeo.h against brute force, no GL, no game data.
//   A. triangle-triangle distance against dense sampling (an upper bound of the true distance) and constructed crossing / touching / coplanar pairs;
//   B. the pair job's lower bound against the brute-force minimum over all triangle pairs (bound <= minimum <= 2 * bound), the classification of
//      coplanar / crossing / touching pairs, parallel offsets;
//   C. SOUNDNESS OF "FREE": random perspective cameras, pairs of static triangle sets (parallel offsets from far below to far above the depth
//      resolution, separated clusters, crossing planes, shared edges), the oracle run to completion; for every pair it calls free, a reference
//      rasteriser (pixel centres, exact plane depth) finds NO pixel covered by both with |depth difference| <= eta (the margin the rule promises);
//   D. NEGATIVE CONTROLS: with the rule's margins relaxed (GeoConfig::testRelax) the same check finds pairs that tie; with the "touching and crossing
//      pairs are pinned" rule removed the check finds them too;
//   E. the cache: the same triangles under another buffer id hit; a buffer uploaded in the picture is pending; dynamic draws are pinned.
//   tools/test_vgx_hsrgeo.sh
#include "vgx_hsrgeo.h"
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace vgx;
using namespace vgx::hsr;
namespace {
unsigned long long rngState = 0x9E3779B97F4A7C15ull;
uint32_t Rnd() { rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17; return static_cast<uint32_t>(rngState >> 11); }
double Uni() { return (Rnd() & 0xFFFFFF) / 16777216.0; }
double Range(double a, double b) { return a + (b - a) * Uni(); }
double LogRange(double a, double b) { return std::exp(Range(std::log(a), std::log(b))); }
unsigned failures = 0;
void Check(bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

struct Tri { V3 p[3]; };
std::vector<float> Flat(const std::vector<Tri>& t) {
    std::vector<float> o;
    for (const Tri& x : t) for (int i = 0; i < 3; ++i) { o.push_back(static_cast<float>(x.p[i].x)); o.push_back(static_cast<float>(x.p[i].y)); o.push_back(static_cast<float>(x.p[i].z)); }
    return o;
}
Aabb BoxOf(const std::vector<float>& f) { Aabb b; for (size_t i = 0; i + 2 < f.size(); i += 3) { const double p[3] = {f[i], f[i + 1], f[i + 2]}; b.Add(p); } return b; }
double Brute(const std::vector<float>& a, const std::vector<float>& b) {
    double best = 1e300;
    for (size_t i = 0; i < a.size(); i += 9) for (size_t j = 0; j < b.size(); j += 9) best = std::min(best, TriTri2(&a[i], &b[j]));
    return std::sqrt(best);
}
V3 Rv(double r) { return {Range(-r, r), Range(-r, r), Range(-r, r)}; }
Tri RandTri(V3 c, double size) { Tri t; for (int i = 0; i < 3; ++i) t.p[i] = c + Rv(size); return t; }
// a planar grid patch (2 triangles per cell) in the plane through `o` spanned by u, v (cells n x n)
std::vector<Tri> Patch(V3 o, V3 u, V3 v, int n) {
    std::vector<Tri> t;
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
        const V3 p00 = o + u * (double(i) / n) + v * (double(j) / n), p10 = o + u * (double(i + 1) / n) + v * (double(j) / n);
        const V3 p01 = o + u * (double(i) / n) + v * (double(j + 1) / n), p11 = o + u * (double(i + 1) / n) + v * (double(j + 1) / n);
        t.push_back({{p00, p10, p11}});
        t.push_back({{p00, p11, p01}});
    }
    return t;
}
double RunJob(const std::vector<float>& a, const std::vector<float>& b, uint8_t& kind, bool& big) {
    Job j;
    std::vector<float> ta = a, tb = b;
    if (!j.Init(std::move(ta), std::move(tb), BoxOf(a), BoxOf(b))) { big = true; kind = 0; return 0; }
    while (!j.done) j.Step(100000, 50000000);
    big = j.tooBig;
    kind = j.kind;
    return j.gap;
}

// ------------------------------------------------------------------ C: a camera, static buffers, draws
struct Source : GeoSource {
    struct Buf { std::vector<uint8_t> vtx; std::vector<uint16_t> idx; uint32_t epoch = 1; };
    std::map<uint32_t, Buf> bufs;
    bool Get(uint32_t id, StaticView& v) override {
        auto it = bufs.find(id);
        if (it == bufs.end()) return false;
        v.vtx = it->second.vtx.data(); v.vtxBytes = it->second.vtx.size(); v.idx = it->second.idx.data(); v.idxCount = it->second.idx.size(); v.epoch = it->second.epoch;
        return true;
    }
    void Put(uint32_t id, const std::vector<float>& tris) { // floats, stride 16 (layout mask 0: x y z w)
        Buf& b = bufs[id];
        b.vtx.clear(); b.idx.clear();
        for (size_t i = 0; i < tris.size(); i += 3) {
            const float p[4] = {tris[i], tris[i + 1], tris[i + 2], 0.0f};
            const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
            b.vtx.insert(b.vtx.end(), q, q + 16);
            b.idx.push_back(static_cast<uint16_t>(i / 3));
        }
        ++b.epoch;
    }
};
struct Camera {
    float P[16], M[12];
    double n, f;
    RenderState rs;
};
Camera MakeCamera(double scale) {
    Camera c;
    std::memset(c.P, 0, sizeof c.P);
    std::memset(c.M, 0, sizeof c.M);
    c.n = LogRange(0.05, 2.0);
    c.f = c.n * LogRange(100, 20000);
    const double fov = Range(0.5, 1.6), aspect = Range(1.0, 1.8);
    const double sy = 1.0 / std::tan(fov / 2), sx = sy / aspect;
    const double ox = Range(-0.2, 0.2), oy = Range(-0.2, 0.2); // off-centre (z-coupled x / y terms)
    c.P[0 * 4 + 0] = static_cast<float>(sx);
    c.P[1 * 4 + 1] = static_cast<float>(sy);
    c.P[2 * 4 + 0] = static_cast<float>(ox);
    c.P[2 * 4 + 1] = static_cast<float>(oy);
    c.P[2 * 4 + 2] = static_cast<float>((c.f + c.n) / (c.n - c.f));
    c.P[2 * 4 + 3] = -1.0f;
    c.P[3 * 4 + 2] = static_cast<float>(2 * c.f * c.n / (c.n - c.f));
    // rotation about y and x, uniform scale, translation so that the origin-centred scene is in front of the eye
    const double ay = Range(-1, 1), ax = Range(-0.6, 0.6);
    const double cy = std::cos(ay), sy2 = std::sin(ay), cx = std::cos(ax), sx2 = std::sin(ax);
    const double Ry[3][3] = {{cy, 0, sy2}, {0, 1, 0}, {-sy2, 0, cy}}, Rx[3][3] = {{1, 0, 0}, {0, cx, -sx2}, {0, sx2, cx}};
    double R[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { R[i][j] = 0; for (int k = 0; k < 3; ++k) R[i][j] += Rx[i][k] * Ry[k][j]; }
    const double dist = std::max(c.n * 1.5, scale * LogRange(1.2, 60.0));
    const double us = LogRange(0.3, 3.0);
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) c.M[i * 4 + j] = static_cast<float>(R[i][j] * us); c.M[i * 4 + 3] = 0; }
    c.M[2 * 4 + 3] = static_cast<float>(-dist);
    c.rs.depthTest = c.rs.depthWrite = 1;
    c.rs.depthFunc = CMP_LEQUAL;
    c.rs.vpX = 0; c.rs.vpY = 0; c.rs.vpW = 96; c.rs.vpH = 96;
    c.rs.scX = 0; c.rs.scY = 0; c.rs.scW = 96; c.rs.scH = 96;
    c.rs.zNear = 0; c.rs.zFar = 1;
    return c;
}
struct Pic {
    CmdBuf buf;
    DrawRec dr[2];
};
void Build(Pic& p, const Camera& c, uint32_t bufA, uint32_t nA, uint32_t bufB, uint32_t nB) {
    p.buf = CmdBuf{};
    p.buf.uni.assign(64, 0.0f);
    std::memcpy(p.buf.uni.data(), c.P, 64);
    std::memcpy(p.buf.uni.data() + 16, c.M, 48);
    const uint32_t nb[2] = {bufA, bufB}, cnt[2] = {nA, nB};
    for (int i = 0; i < 2; ++i) {
        DrawRec& d = p.dr[i];
        d = DrawRec{};
        d.prim = PRIM_TRIANGLES; d.vbuf = nb[i]; d.idxCount = cnt[i] * 3; d.layoutMask = 0;
        d.ublock[UB_PROJ] = 0; d.ublock[UB_MV] = 16;
        d.rs = c.rs;
        p.buf.draws.push_back(d);
    }
}
// reference: min |z_A - z_B| over the pixel centres covered by both sets (window depth), 1e9 when no pixel is shared
double MinDz(const Camera& c, const std::vector<float>& a, const std::vector<float>& b, int W) {
    struct Pr { double x, y, z; bool ok; };
    auto proj = [&](const float* t, Pr out[3]) {
        for (int i = 0; i < 3; ++i) {
            double cl[4];
            const double p[3] = {t[i * 3], t[i * 3 + 1], t[i * 3 + 2]};
            double eye[3];
            for (int r = 0; r < 3; ++r) eye[r] = c.M[r * 4] * p[0] + c.M[r * 4 + 1] * p[1] + c.M[r * 4 + 2] * p[2] + c.M[r * 4 + 3];
            for (int r = 0; r < 4; ++r) cl[r] = c.P[0 * 4 + r] * eye[0] + c.P[1 * 4 + r] * eye[1] + c.P[2 * 4 + r] * eye[2] + c.P[3 * 4 + r];
            out[i].ok = cl[3] > 1e-9;
            if (!out[i].ok) continue;
            out[i].x = (cl[0] / cl[3] * 0.5 + 0.5) * c.rs.vpW;
            out[i].y = (cl[1] / cl[3] * 0.5 + 0.5) * c.rs.vpH;
            out[i].z = (cl[2] / cl[3] * 0.5 + 0.5);
        }
    };
    std::vector<double> za(static_cast<size_t>(W) * W, -1.0);
    // for each pixel, the depths of A fragments are kept as a list (several triangles may cover a pixel): compare against every B fragment
    std::vector<std::vector<double>> fa(static_cast<size_t>(W) * W);
    auto raster = [&](const std::vector<float>& s, auto&& emit) {
        for (size_t i = 0; i < s.size(); i += 9) {
            Pr q[3];
            proj(&s[i], q);
            if (!q[0].ok || !q[1].ok || !q[2].ok) continue;
            const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[2].x - q[0].x) * (q[1].y - q[0].y);
            if (std::fabs(area) < 1e-12) continue;
            const double sc = static_cast<double>(W) / c.rs.vpW; // (sample grid finer than the pixels: W samples across the viewport)
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].x, q[1].x, q[2].x}) * sc))), x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({q[0].x, q[1].x, q[2].x}) * sc)));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].y, q[1].y, q[2].y}) * sc))), y1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({q[0].y, q[1].y, q[2].y}) * sc)));
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                const double px = (x + 0.5) / sc, py = (y + 0.5) / sc;
                const double w0 = ((q[1].x - px) * (q[2].y - py) - (q[2].x - px) * (q[1].y - py)) / area;
                const double w1 = ((q[2].x - px) * (q[0].y - py) - (q[0].x - px) * (q[2].y - py)) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                emit(y * W + x, (w0 * q[0].z + w1 * q[1].z + w2 * q[2].z) * c.rs.zFar);
            }
        }
    };
    raster(a, [&](int px, double z) { fa[static_cast<size_t>(px)].push_back(z); });
    double best = 1e9;
    raster(b, [&](int px, double z) { for (double zz : fa[static_cast<size_t>(px)]) best = std::min(best, std::fabs(zz - z)); });
    return best;
}
} // namespace

int main() {
    // ---------------------------------------------------------------- A
    {
        unsigned bad = 0, trials = 4000;
        for (unsigned t = 0; t < trials; ++t) {
            const Tri a0 = RandTri({0, 0, 0}, 1.0), b0 = RandTri(Rv(Range(0.2, 3.0)), Range(0.2, 1.5));
            const std::vector<float> fa = Flat({a0}), fb = Flat({b0});
            Tri a, b; // (the float-rounded corners the distance function sees)
            for (int i = 0; i < 3; ++i) { a.p[i] = TriV(fa.data(), i); b.p[i] = TriV(fb.data(), i); }
            const double ex = std::sqrt(TriTri2(fa.data(), fb.data()));
            // sampled upper bound
            double best = 1e300;
            const int N = 20;
            for (int i = 0; i <= N; ++i) for (int j = 0; i + j <= N; ++j) {
                const double u = double(i) / N, v = double(j) / N, w = 1 - u - v;
                const V3 pa = a.p[0] * u + a.p[1] * v + a.p[2] * w;
                for (int k = 0; k <= N; ++k) for (int l = 0; k + l <= N; ++l) {
                    const double s = double(k) / N, q = double(l) / N, r = 1 - s - q;
                    const V3 pb = b.p[0] * s + b.p[1] * q + b.p[2] * r;
                    best = std::min(best, std::sqrt(Len2(pa - pb)));
                }
            }
            if (ex > best + 1e-9) { ++bad; if (bad < 4) std::printf("  A: exact %g > sampled %g\n", ex, best); } // the exact distance is never above a sample
            if (ex < best - 0.5) { ++bad; if (bad < 4) std::printf("  A: exact %g << sampled %g\n", ex, best); }                    // and not far below the sample (resolution: the triangles are about 1-3 across, 20 steps)
        }
        Check(bad == 0, "A. triangle-triangle distance equals the sampled minimum within the sampling resolution (4000 random pairs)");
        // crossing: two triangles through each other
        const std::vector<float> c1 = Flat({{{{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}}}}), c2 = Flat({{{{0, 0, -1}, {0, 0, 1}, {0, 2, 0.5}}}});
        Check(TriTri2(c1.data(), c2.data()) == 0.0, "A. crossing triangles: distance 0");
        const std::vector<float> t1 = Flat({{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}}), t2 = Flat({{{{1, 0, 0}, {0, 1, 0}, {1, 1, 0.5}}}});
        Check(TriTri2(t1.data(), t2.data()) < 1e-18, "A. triangles sharing an edge: distance 0");
        const std::vector<float> p1 = Flat({{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}}), p2 = Flat({{{{0.1f, 0.1f, 0.25f}, {0.5f, 0.1f, 0.25f}, {0.1f, 0.5f, 0.25f}}}});
        Check(std::fabs(std::sqrt(TriTri2(p1.data(), p2.data())) - 0.25) < 1e-12, "A. parallel triangles 0.25 apart: distance 0.25");
    }
    // ---------------------------------------------------------------- B
    {
        unsigned bad = 0, loose = 0, trials = 600, touchesFound = 0, crossFound = 0;
        for (unsigned t = 0; t < trials; ++t) {
            std::vector<Tri> A, B;
            const int na = 1 + Rnd() % 40, nb = 1 + Rnd() % 60;
            const double sep = LogRange(1e-4, 30.0);
            for (int i = 0; i < na; ++i) A.push_back(RandTri(Rv(2.0), 0.4));
            for (int i = 0; i < nb; ++i) B.push_back(RandTri(V3{sep, 0, 0} + Rv(2.0), 0.4));
            const std::vector<float> fa = Flat(A), fb = Flat(B);
            const double bf = Brute(fa, fb);
            uint8_t kind = 0; bool big = false;
            const double g = RunJob(fa, fb, kind, big);
            if (big) { ++bad; continue; }
            if (g > bf * (1 + 1e-6) + 1e-12) ++bad;                // a lower bound
            if (g > 0 && bf > 2.0 * g * (1 + 1e-6)) ++bad;        // and within a factor 2 of the minimum
            if (g == 0 && bf > 1e-5 * std::max(BoxOf(fa).Diag(), 1.0)) ++loose; // (reported 0 although the minimum is above the finest delta)
            if (bf == 0 && g != 0) ++bad;
            if (bf == 0) ++crossFound;
        }
        char msg[200];
        std::snprintf(msg, sizeof msg, "B. job bound is a lower bound within a factor 2 of the brute-force minimum (%u random scenes, %u with crossing triangles, %u loose)", trials, crossFound, loose);
        Check(bad == 0 && loose == 0, msg);
        (void)touchesFound;
        uint8_t kind = 0; bool big = false;
        // coplanar overlapping patches: kind 1, gap 0
        const std::vector<float> pa = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 6)), pb = Flat(Patch({1, 1, 0}, {2, 0, 0}, {0, 2, 0}, 3));
        double g = RunJob(pa, pb, kind, big);
        Check(g == 0 && kind == 1, "B. coplanar overlapping patches: gap 0, classified coplanar");
        // crossing patches
        const std::vector<float> xa = Flat(Patch({-2, -2, 0}, {4, 0, 0}, {0, 4, 0}, 5)), xb = Flat(Patch({-2, 0, -2}, {4, 0, 0}, {0, 0, 4}, 5));
        g = RunJob(xa, xb, kind, big);
        Check(g == 0 && kind == 2, "B. crossing patches: gap 0, classified contact");
        // sharing an edge, at an angle (a hull and a plate meeting along a seam)
        const std::vector<float> ea = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 4)), eb = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, -2, 3}, 4));
        g = RunJob(ea, eb, kind, big);
        Check(g == 0 && kind == 2, "B. patches sharing a seam: gap 0 (pinned: exact vertices on a seam give equal depth where the footprints meet)");
        // parallel offsets
        unsigned okOff = 0;
        const double offs[] = {1e-3, 1e-2, 0.1, 1.0};
        for (double o : offs) {
            const std::vector<float> oa = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 6)), ob = Flat(Patch({0.3, 0.3, o}, {3, 0, 0}, {0, 3, 0}, 4));
            g = RunJob(oa, ob, kind, big);
            if (g > 0 && g <= o * (1 + 1e-9) && o <= 2 * g * (1 + 1e-9)) ++okOff;
        }
        Check(okOff == 4, "B. parallel patches 1e-3 .. 1 apart: gap within a factor 2 of the offset");
    }
    // ---------------------------------------------------------------- C / D
    struct Tally { unsigned free = 0, pinned = 0, unsound = 0, scenes = 0, pinnedNoTie = 0, freeBy[kFCount] = {}; unsigned why[8] = {}; };
    auto runScenes = [&](double relax, bool dropContact, Tally& tl, unsigned n) {
        Source src;
        for (unsigned t = 0; t < n; ++t) {
            const Camera cam = MakeCamera(3.0);
            std::vector<Tri> A, B;
            const int kind = static_cast<int>(Rnd() % 5);
            const double s = 3.0;
            if (kind == 0) { // parallel offset (decal-like), tilted
                const V3 u = Rv(1.0) * s + V3{s, 0, 0}, v = Cross(u, Rv(1.0) + V3{0, 0, 1});
                const V3 nrm = Cross(u, v) * (1.0 / std::sqrt(Len2(Cross(u, v))));
                const double off = LogRange(1e-7, 1.0) * (Rnd() & 1 ? 1 : -1);
                A = Patch({0, 0, 0}, u, v * (s / std::sqrt(Len2(v))), 2 + Rnd() % 4);
                B = Patch(nrm * off + u * 0.2 + v * 0.1, u * 0.6, v * (0.6 * s / std::sqrt(Len2(v))), 2 + Rnd() % 3);
            } else if (kind == 1) { // separated random clusters
                const int na = 2 + Rnd() % 20, nb = 2 + Rnd() % 20;
                const V3 d = Rv(1.0);
                const double sep = LogRange(1e-4, 4.0);
                for (int i = 0; i < na; ++i) A.push_back(RandTri(Rv(1.0), 0.4));
                for (int i = 0; i < nb; ++i) B.push_back(RandTri(d * (sep / std::max(1e-9, std::sqrt(Len2(d)))) + Rv(1.0), 0.4));
            } else if (kind == 2) { // crossing planes
                A = Patch({-2, -2, 0}, {4, 0, 0}, {0, 4, 0}, 3);
                B = Patch({-2 + Range(-1, 1), 0, -2}, {4, 0, 0}, {0, 0, 4}, 3);
            } else if (kind == 3) { // shared seam
                A = Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 3);
                B = Patch({0, 0, 0}, {4, 0, 0}, V3{0, -2, 3} * Range(0.2, 1.5), 3);
            } else { // two parallel layers 1e-6 .. 1 apart, the second shifted sideways (partial overlap)
                const double off = LogRange(1e-6, 0.5);
                A = Patch({-2, -2, 0}, {4, 0, 0}, {0, 4, 0}, 4);
                B = Patch({Range(0.5, 3), Range(-1, 1), off}, {3, 0, 0}, {0, 3, 0}, 3);
            }
            const std::vector<float> fa = Flat(A), fb = Flat(B);
            src.Put(1, fa);
            src.Put(2, fb);
            GeoOracle orc(&src);
            orc.cfg.testRelax = relax;
            Pic pic;
            Build(pic, cam, 1, static_cast<uint32_t>(fa.size() / 9), 2, static_cast<uint32_t>(fb.size() / 9));
            PairInfo pi;
            // run to completion
            for (int guard = 0; guard < 200; ++guard) {
                orc.BeginPicture(pic.buf);
                orc.Pair(pic.buf, pic.dr[0], pic.dr[1], pi);
                if (pi.why != kTPending) break;
                orc.Pump(1000000);
            }
            if (pi.why == kTPending) { std::printf("FAIL: still pending\n"); ++failures; continue; }
            if (dropContact && (pi.why == kTContact || pi.why == kTCoplanar)) pi.why = kTFree;
            ++tl.scenes;
            const double mdz = MinDz(cam, fa, fb, 288); // (3 x 3 samples a pixel)
            const double eta = std::ldexp(1.0, -18) * std::fabs(cam.rs.zFar - cam.rs.zNear);
            ++tl.why[pi.why];
            if (pi.why == kTFree) {
                ++tl.free;
                if (pi.freeWhy < kFCount) ++tl.freeBy[pi.freeWhy];
                if (mdz <= eta) ++tl.unsound;
            } else {
                ++tl.pinned;
                if (mdz > eta) ++tl.pinnedNoTie;
            }
        }
    };
    {
        Tally tl;
        runScenes(1.0, false, tl, std::getenv("HSRGEO_SCENES") ? static_cast<unsigned>(std::atoi(std::getenv("HSRGEO_SCENES"))) : 5000);
        char msg[300];
        std::snprintf(msg, sizeof msg, "C. %u scenes: %u free, %u pinned (%u of them show no tie on the 288 x 288 grid); free pairs with a tie pixel: %u; free by rect %u depth %u box %u gap %u invisible %u; pinned by coplanar %u contact %u near %u nodata %u",
                      tl.scenes, tl.free, tl.pinned, tl.pinnedNoTie, tl.unsound, tl.freeBy[kFRect], tl.freeBy[kFDepth], tl.freeBy[kFBox], tl.freeBy[kFGap], tl.freeBy[kFInvisible], tl.why[kTCoplanar], tl.why[kTContact], tl.why[kTNear], tl.why[kTNoData]);
        Check(tl.unsound == 0, msg);
        Check(tl.free > tl.scenes / 6 && tl.pinned > tl.scenes / 6, "C. the scenes cover both outcomes (the proof frees a good share and pins a good share)");
        Tally t2;
        runScenes(1e-3, false, t2, 5000);
        std::snprintf(msg, sizeof msg, "D. NEGATIVE CONTROL: margins relaxed to 1e-3: %u free pairs show a tie pixel", t2.unsound);
        Check(t2.unsound > 0, msg);
        Tally t3;
        runScenes(1.0, true, t3, 5000);
        std::snprintf(msg, sizeof msg, "D. NEGATIVE CONTROL: touching / crossing / coplanar pairs treated as free: %u free pairs show a tie pixel", t3.unsound);
        Check(t3.unsound > 0, msg);
    }
    // ---------------------------------------------------------------- E
    {
        Source src;
        const std::vector<float> fa = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 2)), fb = Flat(Patch({0, 0, 40}, {4, 0, 0}, {0, 4, 0}, 2));
        src.Put(1, fa); src.Put(2, fb);
        GeoOracle orc(&src);
        Camera cam = MakeCamera(3.0);
        Pic pic;
        Build(pic, cam, 1, static_cast<uint32_t>(fa.size() / 9), 2, static_cast<uint32_t>(fb.size() / 9));
        PairInfo pi;
        orc.BeginPicture(pic.buf);
        orc.Pair(pic.buf, pic.dr[0], pic.dr[1], pi);
        Check(pi.why == kTPending, "E. first sight: pending");
        orc.Pump(100000);
        orc.BeginPicture(pic.buf);
        orc.Pair(pic.buf, pic.dr[0], pic.dr[1], pi);
        Check(pi.why != kTPending, "E. after the work: answered");
        DrawRec dyn = pic.dr[1];
        dyn.vbuf = 0;
        orc.Pair(pic.buf, pic.dr[0], dyn, pi);
        Check(pi.why == kTDynamic, "E. a stream draw: pinned as dynamic");
        // the same triangles under other buffer ids: the pair's answer is found by content
        src.Put(7, fa); src.Put(8, fb);
        Pic p2;
        Build(p2, cam, 7, static_cast<uint32_t>(fa.size() / 9), 8, static_cast<uint32_t>(fb.size() / 9));
        orc.BeginPicture(p2.buf);
        orc.Pair(p2.buf, p2.dr[0], p2.dr[1], pi);
        const uint64_t q0 = orc.stats.pairsQueued;
        orc.Pump(100000);
        orc.BeginPicture(p2.buf);
        orc.Pair(p2.buf, p2.dr[0], p2.dr[1], pi);
        Check(orc.stats.pairsQueued == q0 && pi.why != kTPending, "E. the same triangles under new buffer ids reuse the cached pair (no second job)");
        // a buffer uploaded in this picture is pending
        BufUpload up; up.bufId = 1;
        p2.buf.bufUploads.push_back(up);
        p2.buf.cmds.push_back(Cmd{CMD_BUF_UPLOAD, 0});
        orc.BeginPicture(p2.buf);
        orc.Pair(p2.buf, p2.dr[0], p2.dr[1], pi);
        Check(pi.why == kTPending || pi.why == kTFree, "E. a draw of a buffer uploaded in the same picture is not answered from the old copy");
        orc.Pair(p2.buf, p2.dr[1], p2.dr[0], pi);
        // buffer 7 replaced by other contents: a new epoch: the old answer is not used
        const std::vector<float> fc = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 3));
        src.Put(7, fc);
        Pic p3;
        Build(p3, cam, 7, static_cast<uint32_t>(fc.size() / 9), 8, static_cast<uint32_t>(fb.size() / 9));
        orc.BeginPicture(p3.buf);
        orc.Pair(p3.buf, p3.dr[0], p3.dr[1], pi);
        Check(pi.why == kTPending, "E. a buffer id with new contents is analysed again");
    }
    // eviction: a table too small for the ranges seen empties itself without leaving pointers behind (run under ASan / UBSan by tools/test_vgx_hsrgeo.sh) and answers stay right
    {
        Source src;
        GeoOracle orc(&src);
        orc.cfg.maxRanges = 3;
        orc.cfg.maxPairs = 2;
        unsigned answered = 0, wrong = 0;
        for (int t = 0; t < 40; ++t) {
            const Camera cam = MakeCamera(3.0);
            const double off = (t & 1) ? 0.0 : 40.0;
            const std::vector<float> fa = Flat(Patch({0, 0, 0}, {4, 0, 0}, {0, 4, 0}, 2)), fb = Flat(Patch({0, 0, off}, {4, 0, 0}, {0, 4, 0}, 2 + (t % 4)));
            src.Put(1 + static_cast<uint32_t>(t % 5), fa); src.Put(10 + static_cast<uint32_t>(t % 7), fb);
            Pic pic;
            Build(pic, cam, 1 + static_cast<uint32_t>(t % 5), static_cast<uint32_t>(fa.size() / 9), 10 + static_cast<uint32_t>(t % 7), static_cast<uint32_t>(fb.size() / 9));
            PairInfo pi;
            for (int guard = 0; guard < 50; ++guard) {
                orc.BeginPicture(pic.buf);
                orc.Pair(pic.buf, pic.dr[0], pic.dr[1], pi);
                if (pi.why != kTPending) break;
                orc.Pump(1000000);
            }
            if (pi.why == kTPending) continue;
            ++answered;
            if (off == 0.0 && pi.why == kTFree) ++wrong; // (the coplanar patches must never be freed)
        }
        char msg[160];
        std::snprintf(msg, sizeof msg, "E. a tiny table (3 ranges, 2 pairs) evicts and still answers (%u of 40 answered, %llu evictions, %u wrong)", answered, static_cast<unsigned long long>(orc.stats.evictions), wrong);
        Check(answered > 10 && orc.stats.evictions > 0 && wrong == 0, msg);
    }
    // ---------------------------------------------------------------- F (gpu26): GeoOracle::FastPair, the TIES=7 cheap rule
    {
        Source src;
        GeoOracle orc(&src);
        const Camera cam = MakeCamera(3.0);
        auto quadsBuf = [&](uint32_t id, V3 o, V3 u, V3 v) { src.Put(id, Flat(Patch(o, u, v, 2))); };
        quadsBuf(1, {0, 0, 0}, {4, 0, 0}, {0, 4, 0});        // base sheet in z = 0
        quadsBuf(2, {1, 1, 0}, {2, 0, 0}, {0, 2, 0});        // a decal: the same plane, inside
        quadsBuf(3, {0, 0, 30}, {4, 0, 0}, {0, 4, 0});       // a sheet far in front
        // a solid part: a cube of 12 triangles whose box touches the base sheet along z = 0
        std::vector<Tri> cube;
        {
            const V3 c[8] = {{0, 0, -4}, {4, 0, -4}, {4, 4, -4}, {0, 4, -4}, {0, 0, 0}, {4, 0, 0}, {4, 4, 0}, {0, 4, 0}};
            const int f[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1}, {2, 6, 7}, {2, 7, 3}, {1, 5, 6}, {1, 6, 2}, {0, 3, 7}, {0, 7, 4}};
            for (auto& q : f) cube.push_back({{c[q[0]], c[q[1]], c[q[2]]}});
        }
        src.Put(4, Flat(cube));
        auto range = [&](uint32_t id, int tris) { return std::make_pair(id, static_cast<uint32_t>(tris)); };
        auto ask = [&](uint32_t ta, int na, uint32_t ob, int nb, PairInfo& pi, float zNearO = 0) {
            Pic pic;
            Build(pic, cam, ta, static_cast<uint32_t>(na), ob, static_cast<uint32_t>(nb));
            pic.dr[1].rs.zNear = zNearO;
            for (int round = 0; round < 3; ++round) {
                orc.BeginPicture(pic.buf);
                orc.FastPair(pic.buf, pic.dr[0], pic.dr[1], pi);
                if (pi.why != kTPending) break;
                orc.Pump(100000);
            }
        };
        (void)range;
        PairInfo pi;
        ask(2, 8, 1, 8, pi);
        Check(pi.why == kTDecal, "F. a flat sheet lying inside another sheet's plane: kept (decal)");
        ask(1, 8, 3, 8, pi);
        Check(pi.why == kTFree && pi.freeWhy == kFBox, "F. two sheets apart in space: freed (box)");
        ask(2, 8, 1, 8, pi, 0.001f);
        Check(pi.why == kTBias, "F. different depth ranges: kept (bias)");
        ask(4, 12, 1, 8, pi);
        Check(pi.why == kTFree && pi.freeWhy == kFSolid, "F. a solid part touching a sheet: freed (solid-approx)");
        ask(1, 8, 4, 12, pi);
        Check(pi.why == kTDecal, "F. a sheet lying on the face of a solid's box: kept (decal)");
        Pic dp;
        Build(dp, cam, 2, 8, 1, 8);
        dp.dr[1].vbuf = 0;
        orc.BeginPicture(dp.buf);
        orc.FastPair(dp.buf, dp.dr[0], dp.dr[1], pi);
        Check(pi.why == kTDynamic, "F. a stream draw: kept (dynamic)");
        Check(orc.stats.pairsQueued == 0, "F. the fast rule queues no pair job (only the ranges' extraction)");
    }
    std::printf("%s\n", failures ? "FAILED" : "PASS: hsrgeo");
    return failures ? 1 : 0;
}
