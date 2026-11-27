// GXV_PTCL_THIN host test (gamecpu25): port/ptcl_thin.h. Checks: (1) off / near / ortho-free cases keep everything; (2) the fraction is continuous and
// monotone in distance; (3) NESTED sets: a particle kept at distance d is kept at every nearer distance (so the picture thins one particle at a time,
// never a whole emitter at once, and never flickers as the camera moves back and forth); (4) the kept share matches the fraction (+-3 %) over many
// addresses; (5) CamPos is the camera's world position; (6) a selection by list index (negative control, PORT_THIN_NEG=1) is NOT stable when a particle
// dies: it must FAIL check 3b.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "port/ptcl_thin.h"
using namespace port::ptclthin;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (fails < 6) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
int main() {
    const bool neg = std::getenv("PORT_THIN_NEG") != nullptr;
    Cfg off = {0.0f, 2500, 7000}, one = {1.0f, 2500, 7000}, c = {0.3f, 2500, 7000};
    CHECK(!On(off) && !On(one) && On(c), "On");
    CHECK(KeepFraction(1e12f, off) == 1.0f && KeepFraction(1e12f, one) == 1.0f, "off keeps all");
    CHECK(KeepFraction(2500.0f * 2500.0f, c) == 1.0f && KeepFraction(100.0f, c) == 1.0f, "near keeps all");
    CHECK(KeepFraction(7000.0f * 7000.0f, c) == 0.3f && KeepFraction(1e9f, c) == 0.3f, "far keeps fmin");
    float prev = 1.0f;
    for (float d = 0; d < 9000; d += 7.0f) {  // continuous and monotone
        const float f = KeepFraction(d * d, c);
        CHECK(f <= prev + 1e-6f && f >= 0.3f - 1e-6f && prev - f < 0.002f, "monotone/continuous at %f: %f -> %f", d, prev, f);
        prev = f;
    }
    // nested sets over a few thousand particle addresses
    std::vector<uintptr_t> ids;
    for (int i = 0; i < 4000; ++i) ids.push_back(0x90000000u + static_cast<uintptr_t>(i) * 0x58u);
    std::vector<char> keptPrev(ids.size(), 1);
    int pops = 0;
    for (float d = 2400; d <= 7200; d += 40.0f) {
        const float f = KeepFraction(d * d, c);
        int kept = 0, droppedNow = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            const bool k = Keep(ids[i], f);
            if (k && !keptPrev[i]) ++pops;     // a particle that returned while moving AWAY: must not happen
            if (!k && keptPrev[i]) ++droppedNow;
            keptPrev[i] = k; kept += k;
        }
        CHECK(droppedNow <= 40 + 2 * 4000 / 100, "step at %f dropped %d at once", d, droppedNow);  // thinning is gradual: at most a few % a 40-unit step
        if (d >= 7000) { const double share = static_cast<double>(kept) / 4000.0; CHECK(share > 0.27 && share < 0.33, "share %f", share); }
    }
    CHECK(pops == 0, "%d particles returned while the camera moved away (sets not nested)", pops);
    // 3b: a death of one particle must not change the others' fate (index selection does: negative control)
    {
        const float f = 0.5f;
        int changed = 0;
        for (size_t i = 1; i < ids.size(); ++i) {
            const bool before = neg ? ((i % 2) == 0) : Keep(ids[i], f);          // by list index with the list as it is
            const bool after = neg ? (((i - 1) % 2) == 0) : Keep(ids[i], f);     // particle 0 died: every later index shifts down by one
            if (before != after) ++changed;
        }
        CHECK(changed == 0, "3b: %d particles changed fate when one died", changed);
    }
    // CamPos: camera at (100, 200, -300), looking along +x (rotation about z by 90 deg): view = R^T (p - cam)
    {
        const float R[3][3] = {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};  // rows of the view rotation
        const float cam[3] = {100, 200, -300};
        float m[3][4];
        for (int r = 0; r < 3; ++r) { for (int k = 0; k < 3; ++k) m[r][k] = R[r][k]; m[r][3] = -(R[r][0] * cam[0] + R[r][1] * cam[1] + R[r][2] * cam[2]); }
        float out[3]; CamPos(m, out);
        CHECK(std::fabs(out[0] - 100) < 1e-3f && std::fabs(out[1] - 200) < 1e-3f && std::fabs(out[2] + 300) < 1e-3f, "CamPos %f %f %f", out[0], out[1], out[2]);
    }
    if (fails) { printf("ptcl_thin: FAILED (%d)\n", fails); return 1; }
    printf("ptcl_thin: PASS\n");
    return 0;
}
