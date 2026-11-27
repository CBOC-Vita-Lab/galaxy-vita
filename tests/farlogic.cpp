// farlogic25: host test of platform/include/port/farlogic.h (classification, level rules, hysteresis, the half-rate cycle, the dry figure).
// tools/test_farlogic.sh builds it with ASan + UBSan. Exit 0 = all pass.
#include "port/farlogic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

using namespace port::farlogic;

static int sFail = 0, sChecks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++sChecks;                                                                \
        if (!(c)) {                                                               \
            ++sFail;                                                              \
            if (sFail < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
        }                                                                         \
    } while (0)

static uint32_t sRng = 12345u;
static uint32_t Rnd() { sRng = sRng * 1664525u + 1013904223u; return sRng >> 8; }
static float Rf(float lo, float hi) { return lo + (hi - lo) * (Rnd() & 0xFFFF) / 65536.0f; }

static Facts FromBits(int bits, int family) {
    Facts f;
    f.hasModel = bits & 1; f.collision = bits & 2; f.sensors = bits & 4; f.stageSwitch = bits & 8; f.rail = bits & 16;
    f.camCtrl = bits & 32; f.pointer = bits & 64; f.freeJoints = bits & 128; f.player = bits & 256;
    f.family = static_cast< uint8_t >(family);
    return f;
}

static void TestClassify() {
    int ambientNoFam = 0, ambientFam = 0, mobFam = 0, total = 0;
    for (int fam = 0; fam < kFamN; ++fam) {
        for (int bits = 0; bits < 512; ++bits) {
            const Facts f = FromBits(bits, fam);
            Why w;
            const Class c = Classify(f, &w);
            ++total;
            const bool blocked = f.player || f.freeJoints || f.collision || f.stageSwitch || f.rail || f.camCtrl;
            if (c != kNever) {
                CHECK(!blocked);                                        // nothing the player stands on, no switch / rail / camera, never the player
                CHECK(w == kNvNone);
                if (fam == kFamNone) {
                    CHECK(f.hasModel && !f.sensors && !f.pointer);      // unlisted: model and nothing observable but effects / sound
                    ++ambientNoFam;
                } else if (c == kAmbient) {
                    ++ambientFam;
                } else {
                    ++mobFam;
                }
            } else {
                CHECK(w != kNvNone);
            }
            if (blocked) CHECK(c == kNever);
            if (!blocked && fam == kFamNone && f.sensors) CHECK(c == kNever && w == kNvSensor);  // an unlisted actor with a hit sensor
            if (!blocked && fam != kFamNone) CHECK(c != kNever);
            if (fam == kFamNone && !blocked && !f.sensors && !f.hasModel) CHECK(w == kNvNoModel);
        }
    }
    CHECK(total == kFamN * 512);
    CHECK(ambientNoFam == 1);  // of the 512 fact combinations exactly one unlisted actor is touchable: "a model and nothing else"
    // families: ambient 1, 2, 6; mob 3, 4, 5, 7, each over the unblocked combinations (bits over model, sensors, pointer = 8)
    CHECK(ambientFam == 3 * 8 && mobFam == 4 * 8);
    // buckets
    Facts f; f.family = kFamCoin;
    CHECK(BucketOf(f) == kFamCoin);
    f = Facts(); f.sensors = true;
    CHECK(BucketOf(f) == 0);
    f = Facts(); f.hasModel = true;
    CHECK(BucketOf(f) == 8);
}

static void TestRules() {
    CHECK(ClampLevel(-3) == 0 && ClampLevel(0) == 0 && ClampLevel(2) == 2 && ClampLevel(3) == 3 && ClampLevel(99) == 3);
    // MP3: in a session every level is 0, outside one the level is untouched
    for (int lv = 0; lv <= 3; ++lv) CHECK(LevelInSession(lv, true) == 0 && LevelInSession(lv, false) == lv);
    for (int lv = 0; lv <= 3; ++lv) {
        const Rule r = RuleFor(lv);
        CHECK((lv == 0) == (BaseLimit(kAmbient, r) == 0.0f));
        CHECK(BaseLimit(kNever, r) == 0.0f);                              // never is never, at any level
    }
    CHECK(BaseLimit(kAmbient, RuleFor(1)) == 5000.0f && BaseLimit(kMob, RuleFor(1)) == 0.0f);  // level 1: safe families only, 5000
    CHECK(BaseLimit(kAmbient, RuleFor(2)) == 2500.0f && BaseLimit(kMob, RuleFor(2)) == 2500.0f);
    CHECK(BaseLimit(kAmbient, RuleFor(3)) == 2500.0f && RuleFor(3).pose && !RuleFor(2).pose && !RuleFor(1).pose);
    // a rule under the near floor is no rule
    Rule low{1500.0f, 1999.0f, false};
    CHECK(BaseLimit(kAmbient, low) == 0.0f && BaseLimit(kMob, low) == 0.0f);
    Rule ok{2000.0f, 2000.0f, false};
    CHECK(BaseLimit(kAmbient, ok) == 2000.0f);
    // the levels only add: what is far at level N stays far at level N+1 (same distance); level 2's ambient limit is under level 1's
    for (int lv = 1; lv < 3; ++lv) {
        CHECK(BaseLimit(kAmbient, RuleFor(lv + 1)) <= BaseLimit(kAmbient, RuleFor(lv)));
        CHECK(BaseLimit(kMob, RuleFor(lv + 1)) >= BaseLimit(kMob, RuleFor(lv)) || BaseLimit(kMob, RuleFor(lv)) == 0.0f);
    }
    // effective limit: never under the base (times zoom), monotone in every input, clamped inputs
    for (int i = 0; i < 20000; ++i) {
        const float base = Rf(2000.0f, 6000.0f), rad = Rf(-10.0f, 800.0f), reach = Rf(-10.0f, 600.0f), sp = Rf(-5.0f, 3000.0f), z = Rf(0.2f, 6.0f);
        const float e = Effective(base, rad, reach, sp, z);
        CHECK(e >= base);                                                  // zoom under 1 is clamped to 1
        CHECK(Effective(base, rad + 10.0f, reach, sp, z) >= e);
        CHECK(Effective(base, rad, reach + 10.0f, sp, z) >= e);
        CHECK(Effective(base, rad, reach, sp + 10.0f, z) >= e);
        CHECK(Effective(base, rad, reach, sp, z + 0.5f) >= e);
        CHECK(e <= (base + 800.0f + 600.0f + 4000.0f) * 4.0f);             // bounded
    }
    CHECK(Effective(5000.0f, 0, 0, 0, 1.0f) == 5000.0f);
}

static int Transitions(const std::vector< float >& d, float limit, float hyst, bool withCam, float camOffset) {
    bool was = false;
    int n = 0;
    for (float x : d) {
        const float p2 = x * x, c2 = (x + camOffset) * (x + camOffset);
        bool now;
        if (hyst > 0.0f) {
            now = FarState(was, p2, withCam ? c2 : 1e12f, limit);
        } else {
            now = limit > 0.0f && p2 > limit * limit && (withCam ? c2 : 1e12f) > limit * limit;
        }
        if (now != was) ++n;
        was = now;
    }
    return n;
}

static void TestHysteresis() {
    const float L = 2500.0f;
    // jitter +-5 % around the limit: one entry at most, never flicker; without hysteresis (the negative control) it flickers every step
    std::vector< float > jit;
    for (int i = 0; i < 4000; ++i) jit.push_back(L * (1.0f + ((i & 1) ? 0.05f : -0.05f)));
    CHECK(Transitions(jit, L, kHyst, false, 0.0f) <= 1);
    CHECK(Transitions(jit, L, 0.0f, false, 0.0f) > 1000);  // control: the test can see a flicker
    // jitter +-9 %: still no flicker
    jit.clear();
    for (int i = 0; i < 4000; ++i) jit.push_back(L * (1.0f + ((i & 1) ? 0.09f : -0.09f)));
    CHECK(Transitions(jit, L, kHyst, false, 0.0f) <= 1);
    // random walks: transitions with hysteresis never exceed the plain ones, and a change needs a real crossing
    for (int w = 0; w < 300; ++w) {
        std::vector< float > d;
        float x = Rf(0.5f * L, 1.5f * L);
        for (int i = 0; i < 3000; ++i) {
            x += Rf(-0.03f * L, 0.03f * L);
            if (x < 0.1f * L) x = 0.1f * L;
            d.push_back(x);
        }
        const int a = Transitions(d, L, kHyst, false, 0.0f), b = Transitions(d, L, 0.0f, false, 0.0f);
        CHECK(a <= b);
    }
    // the state is true only while both distances are beyond 0.9 * limit; entering needs both beyond the limit
    for (int i = 0; i < 100000; ++i) {
        const float pd = Rf(0.0f, 2.0f * L), cd = Rf(0.0f, 2.0f * L);
        const bool was = Rnd() & 1;
        const bool now = FarState(was, pd * pd, cd * cd, L);
        if (now) CHECK(pd > 0.9f * L - 1.0f && cd > 0.9f * L - 1.0f);
        if (!was && now) CHECK(pd > L && cd > L);
        if (!now) CHECK(!(pd > L && cd > L));                              // beyond both limits is always far
        CHECK(FarState(now, pd * pd, cd * cd, L) == now || was != now);   // idempotent once settled at one position
        CHECK(FarState(was, pd * pd, cd * cd, 0.0f) == false);             // limit 0 = never
    }
    // the camera counts as the player does: a far player with the camera at 2000 units is not far
    CHECK(!FarState(false, 9000.0f * 9000.0f, 2000.0f * 2000.0f, 5000.0f));
    CHECK(!FarState(true, 9000.0f * 9000.0f, 2000.0f * 2000.0f, 5000.0f));
    CHECK(FarState(false, 9000.0f * 9000.0f, 9000.0f * 9000.0f, 5000.0f));
    CHECK(FarState(true, 4600.0f * 4600.0f, 4600.0f * 4600.0f, 5000.0f));   // inside the band: stays
    CHECK(!FarState(false, 4600.0f * 4600.0f, 4600.0f * 4600.0f, 5000.0f)); // ... but does not enter there
    CHECK(!FarState(true, 4400.0f * 4400.0f, 9000.0f * 9000.0f, 5000.0f));  // leaves under 4500
}

static void TestCycle() {
    // half rate: exactly half of the turns, no two left-out turns in a row, any phase
    for (uint32_t phase = 0; phase < 64; ++phase) {
        int skips = 0, run = 0, maxRun = 0;
        for (uint32_t t = 1; t <= 2000; ++t) {
            if (SkipThisTurn(t, phase)) {
                ++skips;
                ++run;
                if (run > maxRun) maxRun = run;
            } else {
                run = 0;
            }
        }
        CHECK(skips == 1000 && maxRun == 1);
    }
    // the phases spread: heap blocks are 16-byte aligned, 64 neighbours, about half on each parity
    int odd = 0;
    for (int i = 0; i < 256; ++i) {
        const void* p = reinterpret_cast< const void* >(static_cast< uintptr_t >(0x88000000u + i * 0x70u));
        odd += PhaseOf(p) & 1;
    }
    CHECK(odd > 64 && odd < 192);
    // the time an actor in the far state lives: over an even window it takes half the steps (half speed, not coarser steps)
    float pos = 0.0f;
    for (uint32_t t = 1; t <= 600; ++t) {
        if (!SkipThisTurn(t, 7)) pos += 3.0f;  // a step is 3 units
    }
    CHECK(pos == 900.0f);  // 300 steps in 600 turns (the full-rate actor made 600)
    // the class read: once, then one in 32, the first read of each actor on its own turn of the 32
    uint8_t age = 0xFF;
    CHECK(ReadDue(age, 0x1234u, true) && age == (0x1234u & 31u));
    int reads = 0;
    for (int i = 0; i < 3200; ++i) reads += ReadDue(age, 0, false);
    CHECK(reads == 100);
}

static void TestDry() {
    CHECK(SavedMsPerTurn(0, 0, 0) == 0.0);
    CHECK(fabs(SavedMsPerTurn(2000000, 0, 1) - 1.0) < 1e-9);                 // 2 ms of far actors in one timed turn: half rate saves 1 ms
    CHECK(fabs(SavedMsPerTurn(3000000, 1000000, 8) - 0.25) < 1e-9);
    CHECK(PoseCategory(8) && PoseCategory(0x10) && !PoseCategory(5) && !PoseCategory(6) && !PoseCategory(3) && !PoseCategory(9) && !PoseCategory(-1));
    CHECK(WhyName(kNvSensor)[0] == 's' && WhyName(99)[0] == '?');
}

int main() {
    TestClassify();
    TestRules();
    TestHysteresis();
    TestCycle();
    TestDry();
    printf("farlogic: %d checks, %d failed: %s\n", sChecks, sFail, sFail ? "FAIL" : "ALL PASS");
    return sFail ? 1 : 0;
}
