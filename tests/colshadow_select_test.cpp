// Host test of Mario's drop-shadow triangle selection (platform/include/port/colshadow_select.h; GXV_COLSHADOW_STABLE). No GL, no game data.
// Fixture: a small sphere (icosphere, radius 300, ~40-unit triangles), Mario on it with the camera far away looking at the planet so that the ground under him
// sits ON the game's n . camZ > 0.707 threshold (the far side of a small planet). 1) camera jitter of +-1 degree: the set must not blink with the fix and does
// without. 2) the cap: the box holds more triangles than the cap, Mario's drop point jitters +-1 unit: the cut must not move with the fix and does without.
#include "port/colshadow_select.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace port::colshadow;
struct V { double x, y, z; };
static V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V norm(V a) { double l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }
static V mid(V a, V b) { return norm({a.x + b.x, a.y + b.y, a.z + b.z}); }
struct Tri { V a, b, c, n, centre; uint64_t key; };
static std::vector<Tri> Sphere(double R, int subdiv) {
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    std::vector<V> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (auto& p : v) p = norm(p);
    std::vector<std::array<int, 3>> f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                         {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int s = 0; s < subdiv; ++s) {
        std::vector<std::array<int, 3>> g;
        for (auto& tr : f) {
            const int a = tr[0], b = tr[1], c = tr[2];
            const int ab = (int)v.size(); v.push_back(mid(v[a], v[b]));
            const int bc = (int)v.size(); v.push_back(mid(v[b], v[c]));
            const int ca = (int)v.size(); v.push_back(mid(v[c], v[a]));
            g.push_back({a, ab, ca}); g.push_back({b, bc, ab}); g.push_back({c, ca, bc}); g.push_back({ab, bc, ca});
        }
        f = g;
    }
    std::vector<Tri> out;
    uint64_t k = 1;
    for (auto& tr : f) {
        Tri t2;
        t2.a = {v[tr[0]].x * R, v[tr[0]].y * R, v[tr[0]].z * R};
        t2.b = {v[tr[1]].x * R, v[tr[1]].y * R, v[tr[1]].z * R};
        t2.c = {v[tr[2]].x * R, v[tr[2]].y * R, v[tr[2]].z * R};
        V e1 = sub(t2.b, t2.a), e2 = sub(t2.c, t2.a);
        t2.n = norm({e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x});
        t2.centre = {(t2.a.x + t2.b.x + t2.c.x) / 3, (t2.a.y + t2.b.y + t2.c.y) / 3, (t2.a.z + t2.b.z + t2.c.z) / 3};
        if (dot(t2.n, t2.centre) < 0) { t2.n = {-t2.n.x, -t2.n.y, -t2.n.z}; }
        t2.key = SetMix(k++) | 1;
        out.push_back(t2);
    }
    return out;
}
static double frand() { return (std::rand() / (double)RAND_MAX) * 2.0 - 1.0; }
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); ++g_fail; } } while (0)

static std::vector<uint64_t> SetOf(const std::vector<Cand>& c, const uint8_t* cls) {
    std::vector<uint64_t> s;
    for (size_t i = 0; i < c.size(); ++i) if (cls[i]) s.push_back(c[i].key * 4 + cls[i]);
    std::sort(s.begin(), s.end());
    return s;
}

int main() {
    std::srand(7);
    // 0) the rule with no memory is the game's rule; the band only keeps more
    for (int i = 0; i < 100000; ++i) {
        Cand c{(float)frand(), (float)frand(), 0, 1, (std::rand() & 7) == 0};
        // the game: facing > 0 skip; camDot > 0.707 skip; |facing| < 0.15 -> edge (2); floor skip -> 0; else 1
        uint8_t game = c.facing > 0 ? 0 : (c.camDot > 0.707f ? 0 : (-c.facing < 0.15f ? 2 : (c.floorSkip ? 0 : 1)));
        CHECK(Classify(c, 0) == game, "legacy rule differs at %f %f", c.camDot, c.facing);
        for (uint8_t prev = 1; prev <= 2; ++prev) {
            const uint8_t s = Classify(c, prev);
            if (game) CHECK(s != 0, "a triangle the game lists was dropped (prev %d, camDot %f facing %f)", prev, c.camDot, c.facing);
        }
    }
    // memory table
    { Mem m; m.Clear(); for (uint64_t k = 1; k <= 200; ++k) m.Add(SetMix(k) | 1, 1 + (k & 1)); bool ok = true; for (uint64_t k = 1; k <= 200; ++k) ok &= m.Find(SetMix(k) | 1) == 1 + (k & 1); CHECK(ok && m.Find(12345) == 0, "memory table"); }

    const double R = 300.0;
    std::vector<Tri> sph = Sphere(R, 3);
    printf("sphere: %u triangles\n", (unsigned)sph.size());
    const V up = norm({0.3, 1.0, 0.2});
    const V drop = {up.x * R, up.y * R, up.z * R};
    const V shadowDir = {-up.x, -up.y, -up.z};

    // 1) camera jitter +-1 degree, camZ . up = 0.707 (the planet's far side)
    {
        V side = norm({up.z, 0, -up.x});
        // camZ = a*up + b*side, a = 0.707
        const V base = {0.7071 * up.x + 0.7071 * side.x, 0.7071 * up.y + 0.7071 * side.y, 0.7071 * up.z + 0.7071 * side.z};
        std::vector<int> box;
        for (size_t i = 0; i < sph.size(); ++i) { V d = sub(sph[i].centre, drop); if (std::sqrt(dot(d, d)) < 110) box.push_back((int)i); }
        printf("box: %u triangles\n", (unsigned)box.size());
        Mem mem[2]; mem[0].Clear(); mem[1].Clear(); int cur = 0;
        std::vector<uint64_t> prevL, prevS;
        unsigned chL = 0, chS = 0, chSlate = 0, minL = 9999, maxL = 0, nS = 0;
        const int frames = 300;
        for (int f = 0; f < frames; ++f) {
            V camZ = norm({base.x + 0.0175 * frand(), base.y + 0.0175 * frand(), base.z + 0.0175 * frand()});
            std::vector<Cand> c;
            for (int i : box) { Cand k; k.camDot = (float)dot(sph[i].n, camZ); k.facing = (float)dot(sph[i].n, shadowDir); V d = sub(sph[i].centre, drop); k.dist = (float)std::sqrt(dot(d, d)); k.key = sph[i].key; k.floorSkip = false; c.push_back(k); }
            std::vector<uint8_t> cl(c.size()), cs(c.size());
            SelectLegacy(c.data(), (unsigned)c.size(), cl.data());
            SelectStable(c.data(), (unsigned)c.size(), mem[cur], mem[cur ^ 1], 128, 128, cs.data()); cur ^= 1;
            auto sL = SetOf(c, cl.data()), sS = SetOf(c, cs.data());
            // never fewer: every legacy triangle is in the stable union
            for (size_t i = 0; i < c.size(); ++i) CHECK(!cl[i] || cs[i], "frame %d: stable dropped a triangle the game lists", f);
            if (f > 0) { if (sL != prevL) ++chL; if (sS != prevS) { ++chS; if (f >= 100) ++chSlate; } }
            minL = std::min<unsigned>(minL, (unsigned)sL.size()); maxL = std::max<unsigned>(maxL, (unsigned)sL.size()); nS = (unsigned)sS.size();
            prevL = sL; prevS = sS;
        }
        printf("camera jitter +-1 deg, %d frames: game set changed %u times (size %u..%u), stable set changed %u times (%u after frame 100, final size %u)\n", frames, chL, minL, maxL, chS, chSlate, nS);
        CHECK(chL >= 20, "fixture does not show the instability without the fix (%u)", chL);
        CHECK(chSlate == 0, "stable set still changes after warm-up (%u)", chSlate);
        CHECK(chS * 4 <= chL, "stable changes %u not clearly fewer than %u", chS, chL);
    }

    // 2) the cap: camera facing the ground (all pass), a box of radius 110 holds far more than the cap of 24; Mario's drop point jitters +-1 unit
    {
        const V camZ = {-up.x, -up.y, -up.z};
        const unsigned cap = 24;
        Mem mem[2]; mem[0].Clear(); mem[1].Clear(); int cur = 0;
        std::vector<uint64_t> prevL, prevS;
        unsigned chL = 0, chS = 0, chSlate = 0, hits = 0;
        for (int f = 0; f < 300; ++f) {
            const V d0 = norm({up.x + 0.0033 * frand(), up.y + 0.0033 * frand(), up.z + 0.0033 * frand()});  // ~1 unit at R 300
            const V dp = {d0.x * R, d0.y * R, d0.z * R};
            std::vector<Cand> all;
            for (auto& t : sph) { V d = sub(t.centre, dp); const double dl = std::sqrt(dot(d, d)); if (dl >= 110) continue; Cand k; k.camDot = (float)dot(t.n, camZ); k.facing = (float)dot(t.n, shadowDir); k.dist = (float)dl; k.key = t.key; k.floorSkip = false; all.push_back(k); }
            // the keeper stops at the cap, in its own (index) order, BEFORE any filter: the game's list is the first `cap` of the box
            std::vector<Cand> legacyIn(all.begin(), all.begin() + std::min<size_t>(all.size(), cap));
            if (all.size() > cap) ++hits;
            std::vector<uint8_t> cl(legacyIn.size()), cs(all.size());
            SelectLegacy(legacyIn.data(), (unsigned)legacyIn.size(), cl.data());
            SelectStable(all.data(), (unsigned)all.size(), mem[cur], mem[cur ^ 1], cap, 128, cs.data()); cur ^= 1;
            auto sL = SetOf(legacyIn, cl.data()), sS = SetOf(all, cs.data());
            CHECK(sS.size() <= cap, "stable exceeded the cap (%u)", (unsigned)sS.size());
            if (f > 0) { if (sL != prevL) ++chL; if (sS != prevS) { ++chS; if (f >= 100) ++chSlate; } }
            prevL = sL; prevS = sS;
        }
        printf("cap %u, drop point jitter +-1 unit, 300 frames (box over the cap in %u): game set changed %u times, stable %u times (%u after frame 100)\n", cap, hits, chL, chS, chSlate);
        CHECK(hits == 300, "box not over the cap");
        CHECK(chL >= 3, "fixture does not show the cap instability without the fix (%u)", chL);
        CHECK(chSlate == 0, "stable cap selection still changes after warm-up (%u)", chSlate);
    }

    // 3) a ROTATING planet (the Dreadnought's disc, a planetoid that turns): Mario still on the spin axis, camera still, the ground turns 0.1 degree a turn, the camera jitters +-1 degree. The
    //    world-space vertices change every turn, so a vertex-hash identity never matches last turn's (the memory is defeated); the collision part + triangle index does.
    {
        V side = norm({up.z, 0, -up.x});
        const V base = {0.7071 * up.x + 0.7071 * side.x, 0.7071 * up.y + 0.7071 * side.y, 0.7071 * up.z + 0.7071 * side.z};
        std::vector<int> box;
        for (size_t i = 0; i < sph.size(); ++i) { V d = sub(sph[i].centre, drop); if (std::sqrt(dot(d, d)) < 110) box.push_back((int)i); }  // (membership frozen: isolates the thresholds)
        Mem memIdx[2], memHash[2]; memIdx[0].Clear(); memIdx[1].Clear(); memHash[0].Clear(); memHash[1].Clear(); int ci = 0, ch = 0;
        std::vector<uint64_t> pL, pI, pH;
        unsigned chL = 0, chI = 0, chH = 0, chIlate = 0;
        const double w = 0.1 * 3.14159265358979 / 180.0;
        for (int f = 0; f < 400; ++f) {
            const double ang = w * f, ca = std::cos(ang), sa = std::sin(ang);
            auto rot = [&](V n) { // Rodrigues about `up`
                const double d = dot(n, up);
                V cr = {up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z, up.x * n.y - up.y * n.x};
                return V{n.x * ca + cr.x * sa + up.x * d * (1 - ca), n.y * ca + cr.y * sa + up.y * d * (1 - ca), n.z * ca + cr.z * sa + up.z * d * (1 - ca)};
            };
            const V camJ = norm({base.x + 0.0175 * frand(), base.y + 0.0175 * frand(), base.z + 0.0175 * frand()});
            std::vector<Cand> cI, cH;
            for (int i : box) {
                const V n = rot(sph[i].n);
                Cand k; k.camDot = (float)dot(n, camJ); k.facing = (float)dot(n, shadowDir); V d = sub(sph[i].centre, drop); k.dist = (float)std::sqrt(dot(d, d)); k.floorSkip = false;
                k.key = sph[i].key; cI.push_back(k);  // collision part + index: does not move with the rotation
                uint64_t h = 1469598103934665603ull; float nf[3] = {(float)n.x, (float)n.y, (float)n.z}; for (float q : nf) { uint32_t b; std::memcpy(&b, &q, 4); h = Mix(h, b); }
                k.key = h | 1; cH.push_back(k);  // world-space identity: new every turn
            }
            std::vector<uint8_t> a(cI.size()), b(cI.size()), g(cI.size());
            SelectLegacy(cI.data(), (unsigned)cI.size(), g.data());
            SelectStable(cI.data(), (unsigned)cI.size(), memIdx[ci], memIdx[ci ^ 1], 128, 128, a.data()); ci ^= 1;
            SelectStable(cH.data(), (unsigned)cH.size(), memHash[ch], memHash[ch ^ 1], 128, 128, b.data()); ch ^= 1;
            auto sL = SetOf(cI, g.data()), sI = SetOf(cI, a.data()), sH = SetOf(cI, b.data());  // (sets by the stable index key, for all three: comparable)
            if (f > 0) { if (sL != pL) ++chL; if (sI != pI) { ++chI; if (f >= 100) ++chIlate; } if (sH != pH) ++chH; }
            pL = sL; pI = sI; pH = sH;
        }
        printf("rotating planet (0.1 deg a turn, camera +-1 deg), 400 turns: game set changed %u times, stable with part+index identity %u (%u after turn 100), stable with a world-vertex identity %u (memory defeated)\n", chL, chI, chIlate, chH);
        CHECK(chL >= 20, "rotating fixture shows no instability (%u)", chL);
        CHECK(chI * 4 <= chL, "stable (index identity) changes %u, not clearly fewer than %u", chI, chL);
        CHECK(chH * 5 >= chL * 4, "world-vertex identity should defeat the memory (%u vs %u)", chH, chL);
    }

    // 4) the radius feedback on a ROTATING disc of big tiles (the Dreadnought start): 60-unit tiles, Mario still, the disc turns 0.3 degree a turn; the box is the game's
    //    (half-size r; the count is the tiles it overlaps). Game: grows at <= 10, shrinks by 1 above: the next ring of tiles adds many, so it swings between two sizes.
    {
        auto boxCount = [&](double r, int turn) {
            const double T = 60.0, ang = 0.3 * 3.14159265358979 / 180.0 * turn, ca = std::cos(ang), sa = std::sin(ang);
            const double mx = 70.0, my = 0.0;
            unsigned n = 0;
            for (int i = -6; i < 6; ++i) for (int j = -6; j < 6; ++j) {
                const double cx = (i + 0.5) * T, cy = (j + 0.5) * T, x = cx * ca - cy * sa, y = cx * sa + cy * ca;
                if (std::fabs(x - mx) <= r + T * 0.5 && std::fabs(y - my) <= r + T * 0.5) ++n;
            }
            return n;
        };
        for (int mode = 0; mode < 2; ++mode) {
            double r = 120.0; RadiusMem rm;
            unsigned swings = 0, rch = 0, minC = 9999, maxC = 0, prev = 0; double rmin = 1e9, rmax = 0;
            for (int t = 0; t < 600; ++t) {
                const unsigned c = boxCount(r, t);
                if (t >= 200) { if (prev && (c > prev ? c - prev : prev - c) >= 3) ++swings; minC = std::min(minC, c); maxC = std::max(maxC, c); rmin = std::min(rmin, r); rmax = std::max(rmax, r); }
                prev = c;
                const double r0 = r; r = RadiusNext((float)r, c, rm, mode == 1); if (r < 1) r = 1; if (r > 120) r = 120;
                if (t >= 200 && r != r0) ++rch;
            }
            printf("disc, radius feedback, %s, turns 200..600: tiles in box %u..%u, radius %.0f..%.0f, radius changes %u, count swings >= 3: %u\n", mode ? "stable" : "game", minC, maxC, rmin, rmax, rch, swings);
            static double gameMin, gameMax; static unsigned gameMinC;
            if (mode == 0) { gameMin = rmin; gameMax = rmax; gameMinC = minC; CHECK(rch >= 100, "the game's radius feedback does not change every turn on the fixture (%u)", rch); }
            else { CHECK(rch == 0 && swings == 0, "stable radius / count still changes (%u, %u)", rch, swings); CHECK(rmin >= gameMax - 1e-9 && minC >= gameMinC, "stable shows less than the game's larger state (r %.0f vs %.0f, tiles %u vs %u)", rmin, gameMax, minC, gameMinC); (void)gameMin; }
        }
    }
    if (g_fail) { printf("FAILED %d\n", g_fail); return 1; }
    printf("ok: colshadow_select\n");
    return 0;
}
