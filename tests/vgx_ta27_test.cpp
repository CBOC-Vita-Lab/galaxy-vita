// ta27: host test of platform/vgx/src/vgx_ta27.h (the cone test against the projected signed area of every triangle, the Morton order,
// the cluster boxes, the tally). tools/test_vgx_ta27.sh
#include "../platform/vgx/src/vgx_ta27.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; if (g_fail < 20) std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Mesh { std::vector<std::array<float, 3>> v; std::vector<uint16_t> idx; };
static Mesh Sphere(int nu, int nv, float R, float cx, float cy, float cz, bool inward) {
    Mesh m;
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i) {
            const float th = 3.14159265f * j / nv, ph = 2.0f * 3.14159265f * i / nu;
            m.v.push_back({cx + R * std::sin(th) * std::cos(ph), cy + R * std::cos(th), cz + R * std::sin(th) * std::sin(ph)});
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const uint16_t a = j * (nu + 1) + i, b = a + 1, c = a + (nu + 1), d = c + 1;
            if (!inward) { m.idx.insert(m.idx.end(), {a, c, b, b, c, d}); }
            else { m.idx.insert(m.idx.end(), {a, b, c, b, d, c}); }
        }
    return m;
}
// GL view of one triangle through the port's own projection (BuildProjC): x = p0 ex + p1 ez, y = -(p2 ey + p3 ez), w = -ez, front iff CCW.
// returns +1 front, -1 back, 0 not decidable (a corner at or behind the eye plane, or no area)
static int GlFacing(const float* const m[3], const float p[4], const float* a, const float* b, const float* c) {
    float ndc[3][2];
    const float* q[3] = {a, b, c};
    for (int k = 0; k < 3; ++k) {
        float e[3];
        for (int r = 0; r < 3; ++r) e[r] = m[r][0] * q[k][0] + m[r][1] * q[k][1] + m[r][2] * q[k][2] + m[r][3];
        if (!(e[2] < -1e-3f)) return 0;
        const float w = -e[2];
        ndc[k][0] = (p[0] * e[0] + p[1] * e[2]) / w;
        ndc[k][1] = -(p[2] * e[1] + p[3] * e[2]) / w;
    }
    const float area = (ndc[1][0] - ndc[0][0]) * (ndc[2][1] - ndc[0][1]) - (ndc[2][0] - ndc[0][0]) * (ndc[1][1] - ndc[0][1]);
    if (std::fabs(area) < 1e-12f) return 0;
    return area > 0 ? 1 : -1;
}

int main() {
    std::mt19937 rng(12345);
    auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    // 1. the sign convention: for random affine matrices and random triangles, GL front <=> fs * n.(p-E) < 0
    {
        int agree = 0, tot = 0;
        for (int it = 0; it < 20000; ++it) {
            float M[3][4];
            // a random rotation-ish matrix with non-uniform scale and a random sign flip, camera in front of the origin
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) M[r][c] = U(-1, 1);
            M[2][3] = U(-8, -3);
            if (it & 1) for (int c = 0; c < 3; ++c) M[0][c] = -M[0][c];
            const float* m[3] = {M[0], M[1], M[2]};
            float p[4] = {U(0.5f, 3.0f), U(-0.1f, 0.1f), U(0.5f, 3.0f), U(-0.1f, 0.1f)};
            if (it % 5 == 0) p[2] = -p[2];
            float E[3], fs;
            if (!ta27::EyeInModel(m, p[0], p[2], E, fs)) continue;
            float t[3][3];
            for (auto& q : t) for (float& x : q) x = U(-1, 1);
            const int gl = GlFacing(m, p, t[0], t[1], t[2]);
            if (gl == 0) continue;
            float e1[3], e2[3], n[3], w[3];
            for (int a = 0; a < 3; ++a) { e1[a] = t[1][a] - t[0][a]; e2[a] = t[2][a] - t[0][a]; w[a] = t[0][a] - E[a]; }
            ta27::Cross(e1, e2, n);
            const float dd = fs * ta27::Dot(n, w);
            if (std::fabs(dd) < 1e-5f) continue;
            ++tot;
            if ((gl > 0) == (dd > 0)) ++agree; // (the assumption of AllCulled: front iff fs n.(p-E) > 0)
        }
        std::printf("convention: %d of %d agree with 'front iff fs n.(p-E) > 0'\n", agree, tot);
        CHECK(tot > 1000);
        CHECK(agree == tot || agree == 0);
        if (agree == 0) std::printf("NOTE: the convention is the opposite one: flip it in vgx_ta27.h\n");
        CHECK(agree == tot);
    }
    // 2. AllCulled never contradicts the GL view: every triangle of a cluster flagged culled is back facing (cull back) / front (cull front)
    {
        long flagged = 0, clusters = 0, contradictions = 0, trisChecked = 0;
        for (int mesh = 0; mesh < 6; ++mesh) {
            const Mesh mm = Sphere(24 + mesh * 7, 16 + mesh * 3, U(1, 4), U(-3, 3), U(-3, 3), U(-3, 3), mesh & 1);
            auto pos = [&](uint16_t i) { return mm.v[i].data(); };
            std::vector<uint32_t> order;
            ta27::MortonOrder(mm.idx.data(), static_cast<uint32_t>(mm.idx.size()), pos, order);
            CHECK(order.size() == mm.idx.size() / 3);
            std::vector<uint32_t> seen(order.size(), 0);
            for (uint32_t t : order) { CHECK(t < seen.size()); if (t < seen.size()) ++seen[t]; }
            bool perm = true;
            for (uint32_t s : seen) perm = perm && s == 1;
            CHECK(perm);
            for (int pass = 0; pass < 2; ++pass) {
                std::vector<ta27::Cl> cl;
                CHECK(ta27::BuildClusters(mm.idx.data(), static_cast<uint32_t>(mm.idx.size()), 64 * 3, pos, pass ? order.data() : nullptr, cl));
                // boxes hold every vertex; rows are in order and tile the run
                uint32_t rowExpect = 0;
                for (size_t c = 0; c < cl.size(); ++c) {
                    CHECK(cl[c].row == rowExpect);
                    const uint32_t w = ta27::ClusterIdx(cl.data(), static_cast<uint32_t>(cl.size()), static_cast<uint32_t>(c), static_cast<uint32_t>(mm.idx.size()));
                    rowExpect += w;
                    for (uint32_t k = 0; k < w; ++k) {
                        const uint32_t i = cl[c].row + k, tri = pass ? order[i / 3] : i / 3;
                        const float* q = pos(mm.idx[tri * 3 + i % 3]);
                        for (int a = 0; a < 3; ++a) CHECK(q[a] >= cl[c].mn[a] && q[a] <= cl[c].mx[a]);
                    }
                }
                CHECK(rowExpect == mm.idx.size());
                for (int cam = 0; cam < 400; ++cam) {
                    float M[3][4];
                    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) M[r][c] = U(-1, 1);
                    M[2][3] = U(-12, -4);
                    if (cam & 1) for (int c = 0; c < 3; ++c) M[1][c] = -M[1][c];
                    const float* m[3] = {M[0], M[1], M[2]};
                    float p[4] = {U(0.5f, 3.0f), U(-0.1f, 0.1f), U(0.5f, 3.0f), U(-0.1f, 0.1f)};
                    float E[3], fs;
                    if (!ta27::EyeInModel(m, p[0], p[2], E, fs)) continue;
                    for (int cullBack = 0; cullBack < 2; ++cullBack)
                        for (size_t c = 0; c < cl.size(); ++c) {
                            ++clusters;
                            if (!ta27::AllCulled(cl[c], E, fs, cullBack != 0)) continue;
                            ++flagged;
                            const uint32_t w = ta27::ClusterIdx(cl.data(), static_cast<uint32_t>(cl.size()), static_cast<uint32_t>(c), static_cast<uint32_t>(mm.idx.size()));
                            for (uint32_t k = 0; k < w; k += 3) {
                                const uint32_t tri = pass ? order[(cl[c].row + k) / 3] : (cl[c].row + k) / 3;
                                const int gl = GlFacing(m, p, pos(mm.idx[tri * 3]), pos(mm.idx[tri * 3 + 1]), pos(mm.idx[tri * 3 + 2]));
                                if (gl == 0) continue;
                                ++trisChecked;
                                // culled for cull back means every triangle is GL back (-1); for cull front, GL front (+1)
                                if ((cullBack && gl > 0) || (!cullBack && gl < 0)) ++contradictions;
                            }
                        }
                }
            }
        }
        std::printf("cone: %ld cluster tests, %ld flagged culled, %ld triangles checked in them, %ld contradictions\n", clusters, flagged, trisChecked, contradictions);
        CHECK(flagged > 2000);
        CHECK(contradictions == 0);
    }
    // 3. the spatial order makes tighter boxes than the strip order of a shuffled mesh (the point of the what-if): total box volume
    {
        const Mesh mm = Sphere(64, 48, 3.0f, 0, 0, 0, false);
        // a shuffled order of the triangles stands for strips with no locality
        std::vector<uint16_t> idx;
        std::vector<uint32_t> perm(mm.idx.size() / 3);
        for (size_t i = 0; i < perm.size(); ++i) perm[i] = static_cast<uint32_t>(i);
        std::shuffle(perm.begin(), perm.end(), rng);
        for (uint32_t t : perm) idx.insert(idx.end(), mm.idx.begin() + t * 3, mm.idx.begin() + t * 3 + 3);
        auto pos = [&](uint16_t i) { return mm.v[i].data(); };
        std::vector<uint32_t> order;
        ta27::MortonOrder(idx.data(), static_cast<uint32_t>(idx.size()), pos, order);
        std::vector<ta27::Cl> a, b;
        CHECK(ta27::BuildClusters(idx.data(), static_cast<uint32_t>(idx.size()), 192, pos, nullptr, a));
        CHECK(ta27::BuildClusters(idx.data(), static_cast<uint32_t>(idx.size()), 192, pos, order.data(), b));
        double ra = 0, rb = 0;
        for (auto& c : a) ra += c.r;
        for (auto& c : b) rb += c.r;
        std::printf("mean cluster radius: strip-like (shuffled) %.3f, Morton %.3f\n", ra / a.size(), rb / b.size());
        CHECK(rb < ra * 0.5);
    }
    // 4. a non-finite position gives no cluster; an empty/degenerate cluster is culled; tally
    {
        std::vector<std::array<float, 3>> v = {{0, 0, 0}, {1, 0, 0}, {std::nanf(""), 0, 0}};
        uint16_t idx[3] = {0, 1, 2};
        auto pos = [&](uint16_t i) { return v[i].data(); };
        std::vector<ta27::Cl> cl;
        CHECK(!ta27::BuildClusters(idx, 3, 192, pos, nullptr, cl));
        CHECK(cl.empty());
        ta27::Cl a[3] = {};
        a[0].row = 0; a[1].row = 6; a[2].row = 12;
        bool fo[3] = {false, true, false}, bk[3] = {false, false, false};
        ta27::Tally t;
        ta27::Accumulate(a, 3, 18, fo, bk, t);
        CHECK(t.idxOut == 6 && t.idxUnion == 6 && t.ranges == 2 && t.tested == 3 && t.outN == 1);
        bool fo2[3] = {false, false, false}, bk2[3] = {true, true, false};
        ta27::Tally t2;
        ta27::Accumulate(a, 3, 18, fo2, bk2, t2);
        CHECK(t2.idxBack == 12 && t2.ranges == 1 && t2.backN == 2);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
