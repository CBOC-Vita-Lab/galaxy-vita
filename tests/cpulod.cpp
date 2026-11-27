// cpulod26 host test: platform/include/port/cpulod.h (the umbrella table and its precedence, the skip cadence of MATLOD / FAMPOSE, the movement guard,
// the star-bit thinning's keep fraction and stability, the distance rule). tools/test_cpulod.sh builds it with ASan + UBSan.
// Each negative control must FAIL its property when the guarded behaviour is taken away.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "port/cpulod.h"
using namespace port::cpulod;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestTable() {
    // every member has increasing strength from level 1 to 2 where that means a bigger number, and the names are unique
    for (int i = 0; i < kMemberCount; ++i)
        for (int j = i + 1; j < kMemberCount; ++j) CHECK(std::strcmp(kMembers[i].name, kMembers[j].name) != 0, "duplicate %s", kMembers[i].name);
    float v;
    CHECK(!TableValue(0, "GXV_MATLOD", &v), "level 0 gives nothing");
    CHECK(TableValue(1, "GXV_MATLOD", &v) && v == 2.0f, "mild matlod 2");
    CHECK(TableValue(2, "GXV_MATLOD", &v) && v == 3.0f, "strong matlod 3");
    CHECK(TableValue(3, "GXV_MATLOD", &v) && v == 4.0f, "level 3 matlod 4");
    CHECK(TableValue(4, "GXV_MATLOD", &v) && v == 4.0f, "level 4 (far30, the last resort) reads row 3");
    CHECK(!TableValue(5, "GXV_MATLOD", &v), "no level 5");
    CHECK(!TableValue(1, "GXV_NOPE", &v), "unknown name");
    for (int i = 0; i < kMemberCount; ++i) {
        const Member& m = kMembers[i];
        CHECK(m.lv[1] > 0.0f && m.lv[2] > 0.0f && m.lv[3] > 0.0f, "%s has values", m.name);
        // strength never goes down with the level (a bigger count / distance-down / fraction-down): by the member's direction
        const bool smallerStricter = std::strstr(m.name, "_DIST") || std::strstr(m.name, "PB_") || std::strstr(m.name, "PTCL_THIN") || std::strstr(m.name, "_D0") || std::strstr(m.name, "_D1");
        const bool largerStricter = !smallerStricter;
        for (int l = 1; l < kMaxLevel; ++l) {
            if (largerStricter) CHECK(m.lv[l + 1] >= m.lv[l], "%s: level %d not weaker than %d", m.name, l + 1, l);
            else CHECK(m.lv[l + 1] <= m.lv[l], "%s: level %d not weaker than %d", m.name, l + 1, l);
        }
        // the ranges of the registry rows (stage_knob.h) accept the values: checked by name where a row exists
        const port::stageknob::Desc* d = port::stageknob::FindDesc(m.name);
        if (d) CHECK(m.lv[1] >= d->lo && m.lv[1] <= d->hi && m.lv[2] >= d->lo && m.lv[2] <= d->hi && m.lv[3] >= d->lo && m.lv[3] <= d->hi, "%s: values inside its registry range", m.name);
        else std::printf("note: %s has no stage-registry row\n", m.name);
    }
    // the skip counts the new cuts take are in 2..4 at both levels
    for (const char* nm : {"GXV_MATLOD", "GXV_FAMPOSE", "GXV_PIECETHIN", "GXV_CHARPOSE"})
        for (int l = 1; l <= 3; ++l) { TableValue(l, nm, &v); CHECK(v >= 2 && v <= 4, "%s level %d count", nm, l); }
}
static void TestPrecedence() {
    unsetenv("GXV_CPULOD"); unsetenv("GXV_MATLOD"); port::stageknob::ClearAll();
    CHECK(Num("GXV_MATLOD", 0) == 0 && N("GXV_MATLOD") == 0, "all unset: default");
    setenv("GXV_CPULOD", "1", 1);
    CHECK(Level() == 1 && N("GXV_MATLOD") == 2 && Num("GXV_MATLOD_DIST", 0) == 3000, "env level 1 fills the member");
    setenv("GXV_CPULOD", "2", 1);
    CHECK(N("GXV_MATLOD") == 3, "level 2");
    setenv("GXV_MATLOD", "0", 1);
    CHECK(N("GXV_MATLOD") == 0, "an env member set to 0 beats the level (0 = off)");
    setenv("GXV_MATLOD", "4", 1);
    CHECK(N("GXV_MATLOD") == 4, "an env member beats the level");
    unsetenv("GXV_MATLOD");
    port::stageknob::Set("GXV_CPULOD", 1.0f);
    CHECK(Level() == 1 && N("GXV_MATLOD") == 2, "the stage's level beats the environment's");
    port::stageknob::Set("GXV_MATLOD", 4.0f);
    CHECK(N("GXV_MATLOD") == 4, "the stage's member beats the stage's level");
    port::stageknob::ClearAll();
    setenv("GXV_CPULOD", "3", 1);
    CHECK(Level() == 3 && N("GXV_MATLOD") == 4, "level 3");
    setenv("GXV_CPULOD", "9", 1);
    CHECK(Level() == 3, "clamped");
    setenv("GXV_CPULOD", "-3", 1);
    CHECK(Level() == 0, "clamped low");
    unsetenv("GXV_CPULOD");
    // Expand: names a line set itself are left alone
    int emitted = 0; bool sawMatlod = false;
    Expand(2, [](const char* n) { return std::strcmp(n, "GXV_MATLOD") == 0; }, [&](const char* n, float) { ++emitted; if (!std::strcmp(n, "GXV_MATLOD")) sawMatlod = true; });
    int perf = 0;
    for (int i = 0; i < kMemberCount; ++i) perf += kMembers[i].perfOnly ? 1 : 0;
    CHECK(perf == 4, "four perf-only members (PB_LOD, PB_DECOR, PB_FUR, PB_PTCL)");
    CHECK(emitted == kMemberCount - 1 - perf && !sawMatlod, "expand skips the named member and the perf-only ones (%d)", emitted);
    emitted = 0;
    Expand(0, [](const char*) { return false; }, [&](const char*, float) { ++emitted; });
    CHECK(emitted == 0, "level 0 expands to nothing");
}
static void TestSkipCadence() {
    for (int n = 2; n <= 4; ++n) {
        int maxRun = 0, run = 0, made = 0, total = 6000;
        uint8_t s = 0xFF;
        for (int t = 0; t < total; ++t) {
            if (Skip(s, n, 12345u)) { ++run; if (run > maxRun) maxRun = run; } else { run = 0; ++made; }
        }
        CHECK(maxRun == n - 1, "n=%d: never more than n-1 skipped in a row (got %d)", n, maxRun);
        CHECK(std::abs(made - total / n) <= 2, "n=%d: one pose in n (made %d of %d)", n, made, total);
    }
    // the phase is spread: 400 actors with different seeds do not all pose on the same turn
    const int n = 3;
    uint8_t s[400]; std::memset(s, 0xFF, sizeof s);
    int poseOnTurn[12] = {};
    for (int t = 0; t < 12; ++t)
        for (int a = 0; a < 400; ++a) if (!Skip(s[a], n, Mix(static_cast<uint32_t>(a) * 16u))) ++poseOnTurn[t];
    int worst = 0, best = 1000;
    for (int t = 3; t < 12; ++t) { worst = std::max(worst, poseOnTurn[t]); best = std::min(best, poseOnTurn[t]); }
    CHECK(worst < 200 && best > 80, "phases spread (%d..%d of 400 pose a turn at n=3)", best, worst);
    // negative control: the same seed for all: every actor poses on the same turn (the spread above would fail)
    uint8_t s2[400]; std::memset(s2, 0xFF, sizeof s2);
    int same[12] = {};
    for (int t = 0; t < 12; ++t) for (int a = 0; a < 400; ++a) if (!Skip(s2[a], n, 7u)) ++same[t];
    int w2 = 0; for (int t = 3; t < 12; ++t) w2 = std::max(w2, same[t]);
    CHECK(w2 == 400, "negative control: one shared seed poses all 400 together (got %d)", w2);
    // n lowered at run time makes the next pose happen at once
    uint8_t s3 = 0; (void)Skip(s3, 4, 1u); s3 = 3; CHECK(!Skip(s3, 2, 1u), "a count above the new n poses at once");
    // n < 2 never skips and resets
    uint8_t s4 = 1; CHECK(!Skip(s4, 1, 1u) && s4 == 0xFF, "off");
}
static void TestMoveGuard() {
    const float tanHalf = 0.4142f;
    // at 5000 units one line is 2*5000*0.4142/456 = 9.08 units
    CHECK(!MovedTooFar(8.0f * 8.0f, 5000.0f, tanHalf, 2.0f), "8 units at 5000 = 0.9 line: not too far for 2 lines");
    CHECK(MovedTooFar(20.0f * 20.0f, 5000.0f, tanHalf, 2.0f), "20 units = 2.2 lines: too far");
    CHECK(MovedTooFar(1.0f, 0.5f, tanHalf, 2.0f), "no usable distance: pose");
    CHECK(MovedTooFar(1.0f, 5000.0f, 0.0f, 2.0f), "no fov: pose");
    // a larger distance lets more movement through
    CHECK(!MovedTooFar(20.0f * 20.0f, 10000.0f, tanHalf, 2.0f), "20 units at 10000 = 1.1 line");
}
static void TestPieceThin() {
    for (int n = 2; n <= 4; ++n) {
        int kept = 0, total = 20000;
        for (int i = 0; i < total; ++i) if (PieceKept(static_cast<uint32_t>(i) * 16u, n)) ++kept;  // addresses 16 apart, as a pool's objects
        const double frac = double(kept) / total;
        CHECK(std::fabs(frac - 1.0 / n) < 0.02, "n=%d keeps %.3f (want %.3f)", n, frac, 1.0 / n);
    }
    // consecutive members of one ring are not all kept or all dropped: runs of dropped ones stay short
    int maxDropRun = 0, run = 0;
    for (int i = 0; i < 20000; ++i) { if (!PieceKept(static_cast<uint32_t>(i) * 16u, 2)) { ++run; maxDropRun = std::max(maxDropRun, run); } else run = 0; }
    CHECK(maxDropRun <= 24, "at n=2 no long run of neighbours is dropped (%d)", maxDropRun);
    // stable per actor and n < 2 keeps all
    CHECK(PieceKept(777u, 3) == PieceKept(777u, 3), "stable");
    CHECK(PieceKept(5u, 1) && PieceKept(5u, 0), "off keeps all");
    // negative control: a plain modulo of the address (all 16 apart, n=2) keeps all or none
    int plain = 0; for (int i = 0; i < 1000; ++i) if ((static_cast<uint32_t>(i) * 16u) % 2u == 0) ++plain;
    CHECK(plain == 1000, "negative control: a bare modulo of 16-aligned addresses keeps everything");
}
static void TestDistance() {
    const float D = 3000.0f;
    CHECK(FarFromBoth(3100.0f * 3100.0f, 3100.0f * 3100.0f, D, 1.0f, 0.0f), "both beyond");
    CHECK(!FarFromBoth(3100.0f * 3100.0f, 2900.0f * 2900.0f, D, 1.0f, 0.0f), "player near: not far");
    CHECK(!FarFromBoth(2900.0f * 2900.0f, 3100.0f * 3100.0f, D, 1.0f, 0.0f), "camera near: not far");
    CHECK(!FarFromBoth(3100.0f * 3100.0f, 3100.0f * 3100.0f, D, 1.0f, 500.0f), "the radius pushes the limit out");
    CHECK(!FarFromBoth(3100.0f * 3100.0f, 3100.0f * 3100.0f, D, 1.5f, 0.0f), "the handheld zoom pushes the limit out");
    CHECK(HystDist(3000.0f, true, 0.06f) == 3000.0f && std::fabs(HystDist(3000.0f, false, 0.06f) - 3180.0f) < 0.01f, "hysteresis");
    // NaN positions are never far (comparisons false): nothing is skipped on garbage
    CHECK(!FarFromBoth(NAN, NAN, D, 1.0f, 0.0f), "NaN is not far");
}
static void TestPbStricter() {
    // kind 0: a factor (smaller stricter, >= 1 or <= 0 = off); kind 1: a distance (smaller stricter, 0 = off)
    CHECK(PbStricter(0, 0.6f, 0.45f) == 0.45f && PbStricter(0, 0.4f, 0.45f) == 0.4f, "factor: the smaller wins");
    CHECK(PbStricter(0, 1.0f, 0.45f) == 0.45f && PbStricter(0, 0.0f, 0.45f) == 0.45f, "factor: an off budget takes the row");
    CHECK(PbStricter(1, 5000.0f, 4000.0f) == 4000.0f && PbStricter(1, 3000.0f, 4000.0f) == 3000.0f, "distance: the smaller wins");
    CHECK(PbStricter(1, 0.0f, 4000.0f) == 4000.0f, "distance: off takes the row");
    // monotone in the level for every perf-only member: a higher level never gives back what a lower one took
    for (int i = 0; i < kMemberCount; ++i) {
        if (!kMembers[i].perfOnly) continue;
        const int kind = (std::strstr(kMembers[i].name, "DECOR") || std::strstr(kMembers[i].name, "FUR")) ? 1 : 0;
        float prev = kMembers[i].lv[1];
        float acc = kind == 0 ? 1.0f : 0.0f;  // off
        for (int l = 1; l <= kMaxLevel; ++l) {
            acc = PbStricter(kind, acc, kMembers[i].lv[l]);
            CHECK(acc <= prev + 1e-6f || l == 1, "%s level %d", kMembers[i].name, l);
            prev = acc;
        }
    }
}
int main() {
    TestTable(); TestPrecedence(); TestSkipCadence(); TestMoveGuard(); TestPieceThin(); TestDistance(); TestPbStricter();
    std::printf(fails ? "cpulod: %d FAILED\n" : "cpulod: all checks passed\n", fails);
    return fails ? 1 : 0;
}
