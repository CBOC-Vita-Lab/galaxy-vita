// The gravity fields' reach test (PlanetGravity::isOutOfReach and CubeGravity's) against the full evaluation it skips: for point,
// disk and cube fields of random and degenerate shapes (skewed and flat cubes, zero and negative radii, negative offsets, no range,
// centres up to 1e7 from the origin) and positions around each field's limit and far from it, calcOwnGravityVector must give the
// same result with the test as without it, bit for bit; then the same through calcGravity with PETARI_GRAVITY_CULL=2
// (tools/test_gravity_reach.sh).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Game/Gravity.hpp"

extern "C" int port_gravity_cull_mode(void) { return 2; }
extern "C" unsigned port_mtx_profile_begin(const void*) { return 0; }
extern "C" void port_mtx_profile_end(unsigned, unsigned) {}
static uint64_t sVerified, sVerifiedEarly, sVerifiedBad;
extern "C" void port_gravity_cull_verified(int rejectedEarly, int mismatching) {
    ++sVerified;
    sVerifiedEarly += rejectedEarly ? 1 : 0;
    sVerifiedBad += mismatching ? 1 : 0;
}

static uint64_t sRng = 0x9e3779b97f4a7c15ull;
static uint64_t next() {
    uint64_t z = (sRng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static float uniform() { return (float)(next() >> 40) / (float)(1u << 24); }  // [0, 1)
static float between(float lo, float hi) { return lo + (hi - lo) * uniform(); }
static bool chance(int percent) { return (int)(next() % 100) < percent; }
static TVec3f unitVector() {
    for (;;) {
        TVec3f v(between(-1, 1), between(-1, 1), between(-1, 1));
        const float m = v.length();
        if (m > 0.1f && m <= 1.0f) {
            return TVec3f(v.x / m, v.y / m, v.z / m);
        }
    }
}
static TVec3f without(const TVec3f& v, const TVec3f& unit) { return v - unit * v.dot(unit); }
static TVec3f centre() {
    static const float spans[] = {0.0f, 1e3f, 1e5f, 1e6f, 1e7f};
    const float s = spans[next() % 5];
    return TVec3f(between(-s, s), between(-s, s), between(-s, s));
}
static float ratio() {
    static const float around[] = {0.0f, 0.5f, 0.9f, 0.99f, 0.999f, 0.9999f, 1.0f, 1.0001f, 1.0005f, 1.001f, 1.002f, 1.01f, 1.1f, 2.0f, 10.0f, 1e3f};
    const float r = around[next() % 16];
    return chance(30) ? r * between(0.999f, 1.001f) : r;
}
static void setRange(PlanetGravity& g) {
    static const float ranges[] = {-1.0f, 0.0f, 1.0f, 50.0f, 500.0f, 3000.0f, 20000.0f};
    g.mRange = chance(20) ? between(0.0f, 30000.0f) : ranges[next() % 7];
    g.mDistant = chance(60) ? 0.0f : between(-600.0f, 600.0f);
}
static float limitOf(const PlanetGravity& g) { return g.mRange < 0.0f ? 5000.0f : g.mRange + g.mDistant; }

static uint64_t sCases, sAccepted, sEarly, sBad;
static void check(const PlanetGravity& g, const TVec3f& rPosition, const char* what) {
    TVec3f full(1.5f, -2.5f, 3.5f), tested(1.5f, -2.5f, 3.5f);
    float fullScalar = -7.0f, testedScalar = -7.0f;
    PlanetGravity::sOutOfReachTest = false;
    const bool inReach = g.calcOwnGravityVector(&full, &fullScalar, rPosition);
    PlanetGravity::sOutOfReachTest = true;
    PlanetGravity::sOutOfReachHit = false;
    const bool testedInReach = g.calcOwnGravityVector(&tested, &testedScalar, rPosition);
    PlanetGravity::sOutOfReachTest = false;
    ++sCases;
    sAccepted += inReach ? 1 : 0;
    sEarly += PlanetGravity::sOutOfReachHit ? 1 : 0;
    if (inReach != testedInReach || memcmp(&full, &tested, sizeof(full)) != 0 || memcmp(&fullScalar, &testedScalar, sizeof(float)) != 0) {
        if (sBad++ < 10) {
            std::printf("%s: position (%.9g, %.9g, %.9g) range %.9g distant %.9g: %d (%.9g) without the reach test, %d (%.9g) with it\n", what,
                        rPosition.x, rPosition.y, rPosition.z, g.mRange, g.mDistant, inReach, fullScalar, testedInReach, testedScalar);
        }
    }
    // The same through calcGravity in PETARI_GRAVITY_CULL=2 (both ways inside; the verify hook counts disagreements).
    TVec3f out(0, 0, 0);
    g.calcGravity(&out, rPosition);
}

static void points(int n) {
    for (int i = 0; i < n; ++i) {
        PointGravity g;
        setRange(g);
        g.mTranslation = centre();
        const float limit = limitOf(g);
        for (int k = 0; k < 24; ++k) {
            const TVec3f d = unitVector();
            const float r = __builtin_fabsf(limit) * ratio();
            check(g, TVec3f(g.mTranslation.x + d.x * r, g.mTranslation.y + d.y * r, g.mTranslation.z + d.z * r), "point");
        }
        check(g, centre(), "point");
    }
}

static void disks(int n) {
    for (int i = 0; i < n; ++i) {
        DiskGravity g;
        setRange(g);
        g.setLocalPosition(centre());
        g.setLocalDirection(chance(5) ? TVec3f(0, 0, 0) : unitVector() * between(0.5f, 2.0f));
        g.setSideDirection(unitVector());
        g.setRadius(chance(10) ? 0.0f : (chance(5) ? -between(0.0f, 2000.0f) : between(0.0f, 5000.0f)));
        g.setValidDegee(chance(50) ? 360.0f : between(0.0f, 360.0f));
        g.setBothSide(chance(50));
        g.setEnableEdgeGravity(chance(80));
        g.updateLocalParam();
        TPos3f mtx;
        mtx.identity();
        if (chance(50)) {
            const float s = between(0.25f, 4.0f);
            mtx.mMtx[0][0] = mtx.mMtx[1][1] = mtx.mMtx[2][2] = s;
        }
        g.updateMtx(mtx);
        const float limit = __builtin_fabsf(limitOf(g));
        const float radius = __builtin_fabsf(g.mWorldRadius);
        TVec3f across = without(unitVector(), g.mWorldNormal);  // (a direction in the disk's plane)
        if (across.length() < 1e-3f) {
            across = unitVector();
        }
        across.normalize();
        for (int k = 0; k < 32; ++k) {
            TVec3f p;
            switch (next() % 3) {
            case 0: {  // over the disk, at a height around the limit
                const float h = limit * ratio() * (chance(50) ? 1.0f : -1.0f), a = radius * between(0.0f, 1.0f);
                p = g.mWorldPosition + g.mWorldNormal * h + across * a;
                break;
            }
            case 1: {  // around the rim, at a distance around the limit
                const TVec3f d = unitVector();
                p = g.mWorldPosition + across * radius + d * (limit * ratio());
                break;
            }
            default: {  // anywhere at a distance around radius + limit
                const TVec3f d = unitVector();
                p = g.mWorldPosition + d * ((radius + limit) * ratio());
                break;
            }
            }
            check(g, p, "disk");
        }
        check(g, centre(), "disk");
    }
}

static void cubes(int n) {
    for (int i = 0; i < n; ++i) {
        CubeGravity g;
        setRange(g);
        g.mActiveFaces = chance(70) ? 63 : (u8)(next() & 63);
        TPos3f cube;
        cube.identity();
        const TVec3f c = centre();
        const int kind = (int)(next() % 5);  // 0, 1: rotated and scaled; 2: skewed; 3: flat or nearly so; 4: axes nearly parallel
        TVec3f axes[3];
        if (kind == 4) {
            const TVec3f base = unitVector();
            const float spread = chance(50) ? between(1e-6f, 1e-3f) : between(0.01f, 0.3f);
            for (TVec3f& a : axes) {
                TVec3f d = base + unitVector() * spread;
                d.normalize();
                a = d * between(1.0f, 3000.0f);
            }
        } else if (kind < 2) {
            TVec3f x = unitVector(), y = without(unitVector(), x);
            if (y.length() < 1e-3f) {
                y = TVec3f(-x.y, x.x, 0.0f);
            }
            y.normalize();
            TVec3f z;
            z.cross(x, y);
            axes[0] = x * between(1.0f, 3000.0f);
            axes[1] = y * between(1.0f, 3000.0f);
            axes[2] = z * between(1.0f, 3000.0f);
        } else {
            for (TVec3f& a : axes) {
                a = unitVector() * between(1.0f, 3000.0f);
            }
            if (kind == 3) {
                axes[2] = axes[0] * between(-1.0f, 1.0f) + axes[1] * between(-1.0f, 1.0f) + unitVector() * between(0.0f, 1.0f);
            }
        }
        for (int a = 0; a < 3; ++a) {
            cube.mMtx[0][a] = axes[a].x;
            cube.mMtx[1][a] = axes[a].y;
            cube.mMtx[2][a] = axes[a].z;
        }
        cube.mMtx[0][3] = c.x;
        cube.mMtx[1][3] = c.y;
        cube.mMtx[2][3] = c.z;
        g.setCube(cube);
        const float limit = __builtin_fabsf(limitOf(g));
        for (int k = 0; k < 40; ++k) {
            if (chance(40)) {
                // Off an edge, anywhere along its line that the region test still counts as beside the edge.
                const int e = (int)(next() % 3), i = (e + 1) % 3, j = (e + 2) % 3;
                const TVec3f edge = axes[i] * (chance(50) ? 1.0f : -1.0f) + axes[j] * (chance(50) ? 1.0f : -1.0f);
                const float length = axes[e].length();
                const TVec3f along = axes[e] * (1.0f / length);
                const TVec3f off = without(unitVector(), along);
                const float offLength = off.length();
                if (offLength < 1e-3f) {
                    continue;
                }
                const float t = -edge.dot(along) + length * between(-1.0f, 1.0f);
                check(g, c + edge + along * t + off * (limit * ratio() / offLength), "cube edge");
                continue;
            }
            TVec3f p = c;
            int corner[3];
            for (int a = 0; a < 3; ++a) {
                corner[a] = (int)(next() % 3) - 1;  // a face, edge or corner region, or the inside
                p += axes[a] * (float)corner[a];
            }
            TVec3f d = unitVector();
            if (kind == 4 && chance(50)) {
                d = axes[next() % 3];  // (out along an axis: there the bound is nearly tight)
                d.normalize();
            }
            p += d * (limit * ratio());
            if (chance(30)) {
                const int a = (int)(next() % 3);
                p += axes[a] * between(-1.0f, 1.0f);
            }
            check(g, p, "cube");
        }
        check(g, centre(), "cube");
    }
}

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 20000;
    points(n);
    disks(n);
    cubes(n);
    std::printf("gravity reach: %llu positions, %llu in reach, %llu rejected early, %llu different; through calcGravity: %llu compared, %llu "
                "rejected early, %llu different\n",
                (unsigned long long)sCases, (unsigned long long)sAccepted, (unsigned long long)sEarly, (unsigned long long)sBad,
                (unsigned long long)sVerified, (unsigned long long)sVerifiedEarly, (unsigned long long)sVerifiedBad);
    if (sBad || sVerifiedBad || sVerified != sCases) {
        return 1;
    }
    if (sAccepted < sCases / 20 || sEarly < sCases / 10 || sVerifiedEarly != sEarly) {
        std::printf("gravity reach: too few positions in reach or rejected early to say anything\n");
        return 1;
    }
    return 0;
}
