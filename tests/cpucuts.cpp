// cpucuts25 host test: platform/include/port/cpucuts.h (the class gate of GXV_FARDRAW_KINDS, the stamp age / hold, the draw census, the animation clock under a
// skipped pose, the character pose cadence). tools/test_cpucuts.sh builds it with ASan + UBSan. Negative controls are inside: each one must FAIL its property
// when the guarded behaviour is taken away, or the test proves nothing.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "port/cliprange.h"
#include "port/cpucuts.h"
using namespace port::cpucuts;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestSensorClasses() {
    CHECK(ClassOfSensorType(0x14) == kCbEnemy && ClassOfSensorType(0x15) == kCbEnemy && ClassOfSensorType(0x2B) == kCbEnemy, "enemy range");
    CHECK(ClassOfSensorType(0x1F) == kSensorKeep, "the launcher sensor inside the enemy range is kept");
    CHECK(ClassOfSensorType(0x13) == kSensorKeep && ClassOfSensorType(0x2C) == kSensorKeep && ClassOfSensorType(0x44) == kSensorKeep, "bosses and the range markers are kept");
    CHECK(ClassOfSensorType(0x46) == kCbMapObj && ClassOfSensorType(0x47) == kCbMapObj, "plain map object");
    CHECK(ClassOfSensorType(0x48) == kSensorKeep && ClassOfSensorType(0x49) == kSensorKeep, "move collision / clip field are kept");
    CHECK(ClassOfSensorType(0x4A) == kSensorKeep && ClassOfSensorType(0x4B) == kSensorKeep && ClassOfSensorType(0x4C) == kSensorKeep, "coin, red coin, star piece are kept");
    CHECK(ClassOfSensorType(0x4D) == kSensorKeep && ClassOfSensorType(0x4F) == kSensorKeep && ClassOfSensorType(0x50) == kSensorKeep, "1up, switch, morph item kept");
    CHECK(ClassOfSensorType(0x67) == kSensorKeep, "power star bind kept");
    CHECK(ClassOfSensorType(0x1) == kSensorKeep && ClassOfSensorType(0x0) == kSensorKeep, "the player is kept");
    CHECK(ClassOfSensorType(0x5) == kCbNpc && ClassOfSensorType(0x7) == kCbNpc, "npc, animal");
    CHECK(ClassOfSensorType(0xFFFF) == kSensorKeep, "unknown record");
    for (uint32_t t = 0; t < 0x200; ++t) {  // nothing outside the five named ranges is ever takeable
        const uint8_t c = ClassOfSensorType(t);
        const bool named = (t >= 0x14 && t <= 0x2B && t != 0x1F) || t == 0x46 || t == 0x47 || t == 5 || t == 7;
        CHECK((c != kSensorKeep) == named, "type %u", t);
    }
    const uint32_t mixed[] = {0x15, 0x4A};
    CHECK((ClassOfSensors(mixed, 2) & kSensorKeep) != 0, "one kept sensor keeps the actor");
    const uint32_t ok[] = {0x15, 0x16, 0x47};
    CHECK(ClassOfSensors(ok, 3) == (kCbEnemy | kCbMapObj), "or of classes");
    CHECK(ClassOfSensors(ok, 0) == 0, "no sensors");
}

static void TestClassOfActor() {
    // kinds: plant 0, collision 1, sensor 2, decor 3, decor+fx 4, nomodel 5 (port::cliprange::Kind)
    CHECK(ClassOfActor(3, 0) == kCbDecor && ClassOfActor(4, 0) == kCbDecorFx, "decor");
    CHECK(ClassOfActor(0, 0) == 0 && ClassOfActor(1, 0) == 0 && ClassOfActor(5, 0) == 0, "plant, collision, nomodel never");
    CHECK(ClassOfActor(2, 0) == 0, "a sensor actor whose sensor types are not known is never taken");
    CHECK(ClassOfActor(2, kSensorKeep) == 0 && ClassOfActor(2, kCbEnemy | kSensorKeep) == 0, "keep wins");
    CHECK(ClassOfActor(2, kCbEnemy) == kCbEnemy, "enemy");
    // the DEFAULT (mask 1) takes exactly what cliprange25's `kind == kDecor` took: exhaustive over kinds x sensor bits
    int taken = 0;
    for (int kind = 0; kind < 6; ++kind)
        for (int bits = 0; bits < 256; ++bits) {
            const bool now = Allowed(ClassOfActor(kind, static_cast<uint8_t>(bits)), 1u);
            const bool before = kind == port::cliprange::kDecor;
            CHECK(now == before, "kind %d bits %d", kind, bits);
            taken += now ? 1 : 0;
        }
    CHECK(taken == 256, "decor only: 256 of 1536 combinations");
    CHECK(!Allowed(0, 31u), "class 0 is never allowed whatever the mask");
    CHECK(Allowed(kCbEnemy, 4u | 1u) && !Allowed(kCbEnemy, 1u | 2u | 8u | 16u), "mask bits");
    CHECK(!Allowed(kCbEnemy | kCbMapObj, 4u) && Allowed(kCbEnemy | kCbMapObj, 12u), "an actor with two sensor classes needs both");
}

// A model of the lever's per-turn decision for one actor whose size on the picture is given turn by turn; returns the number of changes of state.
static int Flicker(const std::vector<float>& lines, float base, float rampPct, float jitter, float hyst, int hold, int* culledTurns) {
    uint8_t age = 0;
    int changes = 0, culled = 0;
    const float thr = port::cliprange::DrawThreshold(base, rampPct, jitter);
    for (float l : lines) {
        const bool was = age != 0;
        bool cull = port::cliprange::DrawCulled(l, thr, was, hyst);
        if (!cull && HoldKeeps(age, hold)) cull = true;
        if (cull != was) ++changes;
        age = cull ? AgeNext(age) : 0;
        culled += cull ? 1 : 0;
    }
    if (culledTurns) *culledTurns = culled;
    return changes;
}
static void TestHold() {
    CHECK(AgeNext(0) == 1 && AgeNext(1) == 2 && AgeNext(254) == 255 && AgeNext(255) == 255, "age counts and saturates");
    CHECK(!HoldKeeps(0, 30) && HoldKeeps(1, 30) && HoldKeeps(29, 30) && !HoldKeeps(30, 30) && !HoldKeeps(5, 0), "hold window");
    // an actor that bobs around its limit (a model turning / bobbing: its clipping sphere's size on the picture swings): +- 0.3 lines (4 %) of a limit of 8 is
    // inside the 6 % hysteresis band; +- 0.9 lines (11 %) is not: only the hold keeps that one from flickering
    for (int big = 0; big < 2; ++big) {
        std::vector<float> bob;
        for (int i = 0; i < 600; ++i) bob.push_back(8.0f + (big ? 0.9f : 0.3f) * std::sin(i * 1.3f));
        const int noHyst = Flicker(bob, 8.0f, 0.0f, 0.5f, 0.0f, 0, nullptr);
        const int hyst = Flicker(bob, 8.0f, 0.0f, 0.5f, 0.06f, 0, nullptr);
        const int hyst30 = Flicker(bob, 8.0f, 0.0f, 0.5f, 0.06f, 30, nullptr);
        std::printf("hold: changes of state in 600 turns of a model bobbing +-%s: no hysteresis %d, hysteresis 6%% %d, + hold 30 %d\n", big ? "11%" : "4%", noHyst, hyst, hyst30);
        CHECK(noHyst > 100, "negative control: without hysteresis the bobbing model flickers (%d)", noHyst);
        if (!big) CHECK(hyst <= 1, "inside the band the hysteresis is enough (%d)", hyst);
        else CHECK(hyst > 100 && hyst30 <= 2 * (600 / 30), "outside the band the hysteresis is not and the hold bounds the changes to one per 2 x hold turns (%d / %d)", hyst, hyst30);
    }
    // a model that walks away and comes back: culled while small, back when big, once each way
    std::vector<float> walk;
    for (int i = 0; i < 200; ++i) walk.push_back(20.0f - i * 0.1f);
    for (int i = 0; i < 200; ++i) walk.push_back(0.1f + i * 0.1f);
    int ct = 0;
    CHECK(Flicker(walk, 8.0f, 0.0f, 0.5f, 0.06f, 30, &ct) == 2 && ct > 100, "a model that goes away and comes back changes state twice");
}

static void TestCrowdThinning() {
    // 200 actors of one size class shrink together as the camera pulls away: with the ramp and the jitter they leave one at a time, not in a crowd
    const int N = 200;
    std::vector<float> thr(N);
    for (int i = 0; i < N; ++i) thr[i] = port::cliprange::DrawThreshold(8.0f, 50.0f, port::cliprange::Jitter01(static_cast<uintptr_t>(0x10000 + i * 0x230)));
    int maxLeave = 0, prev = 0;
    for (float size = 14.0f; size > 6.0f; size -= 0.02f) {
        int cull = 0;
        for (int i = 0; i < N; ++i) cull += port::cliprange::DrawCulled(size, thr[i], false, 0.0f) ? 1 : 0;
        if (cull - prev > maxLeave) maxLeave = cull - prev;
        prev = cull;
    }
    int maxLeave0 = 0;
    prev = 0;
    for (float size = 14.0f; size > 6.0f; size -= 0.02f) {
        int cull = 0;
        for (int i = 0; i < N; ++i) cull += port::cliprange::DrawCulled(size, 8.0f, false, 0.0f) ? 1 : 0;
        if (cull - prev > maxLeave0) maxLeave0 = cull - prev;
        prev = cull;
    }
    std::printf("crowd: most actors leaving in one 0.02-line step: with the ramp %d of %d, without %d\n", maxLeave, N, maxLeave0);
    CHECK(maxLeave <= 6, "with the ramp at most a few leave together (%d)", maxLeave);
    CHECK(maxLeave0 == N, "negative control: one threshold takes the whole crowd at once (%d)", maxLeave0);
}

// ---- the animation clock -------------------------------------------------------------------------------------------------------------
static void TestClock() {
    // reference: pose every turn. Each turn: Movement() then (maybe) Pose().
    const int T = 600;
    std::vector<double> ref(T);
    {
        ClockModel c;
        for (int t = 0; t < T; ++t) { c.Movement(); ref[t] = c.Pose(); }
    }
    for (int t = 1; t < T; ++t) CHECK(ref[t] - ref[t - 1] == 1.0, "reference advances by one pose-turn at a time (%d)", t);
    // skip patterns: every 2nd, every 3rd, every 4th, a pseudo-random one with at most 3 in a row
    for (int pattern = 0; pattern < 4; ++pattern) {
        ClockModel c;
        int skipsInRow = 0, maxRow = 0, posed = 0;
        uint32_t rng = 12345;
        for (int t = 0; t < T; ++t) {
            c.Movement();
            bool skip;
            if (pattern == 0) skip = (t & 1) != 0;
            else if (pattern == 1) skip = t % 3 != 0;
            else if (pattern == 2) skip = t % 4 != 0;
            else { rng = rng * 1664525u + 1013904223u; skip = ((rng >> 24) & 3) != 0 && skipsInRow < 3; }
            if (skip) { c.LeftOut(); ++skipsInRow; maxRow = skipsInRow > maxRow ? skipsInRow : maxRow; }
            else {
                skipsInRow = 0;
                const double shown = c.Pose();
                ++posed;
                CHECK(shown == ref[t], "pattern %d turn %d: the posed turn shows frame %.0f, the game would show %.0f", pattern, t, shown, ref[t]);
            }
            CHECK(c.frame == t + 1.0, "pattern %d turn %d: the animation frame is %.1f (one per movement)", pattern, t, c.frame);
        }
        CHECK(posed > T / 5, "pattern %d posed %d of %d", pattern, posed, T);
    }
    // negative control: a left-out calcAnim that does NOT clear the latch (what the first PB_ANIM did before its fix) freezes the clock: the frame no longer
    // advances on the movement after a skip
    {
        ClockModel c;
        int stalled = 0;
        for (int t = 0; t < T; ++t) {
            c.Movement();
            if ((t & 1) != 0) { /* skip, latch left set */ } else (void)c.Pose();
            if (c.frame != t + 1.0) ++stalled;
        }
        CHECK(stalled > T / 4, "negative control: the clock must stall when the latch is not released (%d)", stalled);
    }
}

// ---- GXV_CHARPOSE cadence ------------------------------------------------------------------------------------------------------------
static void TestCharPose() {
    const float tanHalf = std::tan(22.5f * 3.14159265f / 180.0f);
    const float dist = 6000.0f;
    // still actor: n = 2 -> alternates after the first pose; n = 3 -> at most 2 skips in a row
    for (int n = 2; n <= 4; ++n) {
        uint8_t skips = 0xFF;
        int row = 0, maxRow = 0, skipped = 0;
        const int T = 4000;
        for (int t = 0; t < T; ++t) {
            const bool s = CharPoseSkip(skips, n, 0x1234u, 0.0f, dist, tanHalf, 1.0f);
            if (s) { ++row; ++skipped; if (row > maxRow) maxRow = row; } else row = 0;
        }
        CHECK(maxRow == n - 1, "n=%d at most n-1 skips in a row (%d)", n, maxRow);
        CHECK(std::fabs(static_cast<double>(skipped) / T - (n - 1.0) / n) < 0.01, "n=%d skip share %.3f", n, static_cast<double>(skipped) / T);
    }
    // a mover (1.2 lines a turn on the picture) is posed every turn
    {
        const float unitsPerLine = 2.0f * dist * tanHalf / 456.0f;
        uint8_t skips = 0xFF;
        int skipped = 0;
        for (int t = 0; t < 500; ++t) skipped += CharPoseSkip(skips, 2, 77u, std::pow(1.2f * unitsPerLine, 2.0f), dist, tanHalf, 1.0f) ? 1 : 0;
        CHECK(skipped == 0, "an actor that moved more than the gate is always posed (%d)", skipped);
        skips = 0xFF;
        skipped = 0;
        for (int t = 0; t < 500; ++t) skipped += CharPoseSkip(skips, 2, 77u, std::pow(0.9f * unitsPerLine, 2.0f), dist, tanHalf, 1.0f) ? 1 : 0;
        CHECK(skipped > 200, "under the gate it is skipped (%d)", skipped);
    }
    // a crowd of 64 still actors: on any turn about half of them skip (the phases are spread by address), never all or none
    {
        std::vector<uint8_t> sk(64, 0xFF);
        int minSkip = 64, maxSkip = 0;
        for (int t = 0; t < 200; ++t) {
            int cnt = 0;
            for (int i = 0; i < 64; ++i) cnt += CharPoseSkip(sk[i], 2, static_cast<uint32_t>(0x4000 + i * 37) >> 0, 0.0f, dist, tanHalf, 1.0f) ? 1 : 0;
            if (t > 4) { if (cnt < minSkip) minSkip = cnt; if (cnt > maxSkip) maxSkip = cnt; }
        }
        std::printf("charpose: a crowd of 64 still actors, skipping on a turn: min %d max %d\n", minSkip, maxSkip);
        CHECK(minSkip >= 16 && maxSkip <= 48, "phases spread (%d..%d)", minSkip, maxSkip);
    }
    // n < 2 and unknown geometry always pose
    uint8_t sk = 5;
    CHECK(!CharPoseSkip(sk, 1, 1, 0, dist, tanHalf, 1) && sk == 0xFF, "n=1 off");
    sk = 0xFF;
    CHECK(!CharPoseSkip(sk, 2, 1, 0, 0.5f, tanHalf, 1) && !CharPoseSkip(sk, 2, 1, 0, dist, 0.0f, 1), "unknown geometry poses");
}

// ---- the census --------------------------------------------------------------------------------------------------------------------------
static void TestCensus() {
    CHECK(DcSize(0.0f) == 0 && DcSize(1.99f) == 0 && DcSize(2.0f) == 1 && DcSize(5.9f) == 3 && DcSize(8.0f) == 5 && DcSize(31.9f) == 8 && DcSize(32.0f) == 9 && DcSize(1e9f) == 9, "size buckets");
    int last = 0;
    for (float l = 0; l < 100; l += 0.1f) { const int s = DcSize(l); CHECK(s >= last, "monotone"); last = s; }
    CHECK(DcBandOfCamera(0) == 0 && DcBandOfCamera(1000.0f * 1000.0f) == 0 && DcBandOfCamera(1001.0f * 1001.0f) == 1 && DcBandOfCamera(1e12f) == 4, "bands");
    DrawCensus* c = new DrawCensus();
    c->Add("HoneyBee", 3, 5.0f, kCbEnemy, 2, true);
    c->Add("HoneyBee", 3, 7.0f, kCbEnemy, 2, true);
    c->Add("HoneyBee", 1, 40.0f, kCbEnemy, 2, false);
    c->Add("Tree", 4, 3.0f, kCbDecor, 3, true);
    c->Add("Rock", 4, 20.0f, 0, 4, true);
    CHECK(c->drawnActors == 5 && c->drawnShapes == 13, "totals");
    DcFam* bee = c->Find("HoneyBee", false);
    CHECK(bee != nullptr && bee->actors == 3 && bee->shapes == 6 && bee->farActors == 2 && bee->farShapes == 4 && bee->under8 == 4 && bee->under16 == 4, "family");
    uint64_t a = 0, s = 0;
    c->Under(DcSize(6.0f) + 0, DcRowsOfMask(kCbEnemy), &a, &s);  // under 6 lines (buckets 0..3): the 5-line bee only
    CHECK(a == 1 && s == 2, "enemy under 6 lines: %llu / %llu", static_cast<unsigned long long>(a), static_cast<unsigned long long>(s));
    c->Under(5, DcRowsOfMask(kCbEnemy | kCbDecor), &a, &s);  // under 8 lines
    CHECK(a == 3 && s == 7, "enemy + decor under 8 lines: %llu / %llu", static_cast<unsigned long long>(a), static_cast<unsigned long long>(s));
    c->Under(8, 0xFFu, &a, &s);  // under 24 lines, any class: bees 5, 7; tree; rock (20)
    CHECK(a == 4 && s == 11, "all far under 24 lines: %llu / %llu", static_cast<unsigned long long>(a), static_cast<unsigned long long>(s));
    // a full table: 500 families, the rest is counted as unnamed, never a crash or a wrong total
    c->Reset();
    char name[32];
    for (int i = 0; i < 500; ++i) { std::snprintf(name, sizeof name, "Fam%03d", i); c->Add(name, 2, 10.0f, kCbMapObj, 1, true); }
    CHECK(c->drawnActors == 500 && c->famUsed == static_cast<uint32_t>(kDcFamCap) && c->famOverflowActors == 500u - static_cast<uint32_t>(kDcFamCap), "family overflow %u", c->famOverflowActors);
    uint64_t sum = 0;
    for (int i = 0; i < kDcFamCap; ++i) sum += c->fam[i].shapes;
    CHECK(sum + c->famOverflowShapes == 500, "no shape lost");
    // a very long name is truncated, not overrun
    c->Reset();
    c->Add("AVeryLongObjectNameThatIsLongerThanTheFamilyFieldAllows", 0, 1, kCbDecor, 1, false);
    DcFam* lf = c->Find("AVeryLongObjectNameThatIsLongerThanTheFamilyFieldAllows", false);
    CHECK(lf != nullptr && std::strlen(lf->name) == static_cast<size_t>(kDcFamName - 1), "name truncated");
    delete c;
    const DcEst e = DcEstimate(100, 200, 12.0, 25.0, 40.0);
    CHECK(std::fabs(e.gameMs - (200 * 12.0 + 100 * 25.0) * 1e-3) < 1e-9 && std::fabs(e.feMs - 8.0) < 1e-9, "estimate %.3f %.3f", e.gameMs, e.feMs);
    CHECK(DcRowsOfMask(1) == 2u && DcRowsOfMask(31) == 0x3Eu, "rows of mask");
    CHECK(DcClassIndex(kCbDecor) == 1 && DcClassIndex(kCbEnemy | kCbNpc) == 3 && DcClassIndex(0) == 6, "class index");
}

int main() {
    TestSensorClasses();
    TestClassOfActor();
    TestHold();
    TestCrowdThinning();
    TestClock();
    TestCharPose();
    TestCensus();
    if (fails == 0) std::printf("cpucuts: ALL PASS\n");
    else std::printf("cpucuts: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
