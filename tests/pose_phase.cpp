// ratecut31 host test: platform/include/port/pose_phase.h (GXV_POSE_PHASE, plan-perf31 B3). tools/test_pose_phase.sh builds it with ASan + UBSan.
//   1. identity: mode 0 of every rule is the old function, bit for bit, counters included (random sequences).
//   2. the turn / picture model: patterns of drawn and undrawn turns (60 Assist's pair, catch-up turns, broken pairs, a plain 60, runs of undrawn turns, random),
//      200 far actors under the six pose cuts (with near/far changes, movement guards, 60 Assist's pair set), the pose age every real picture and every second
//      picture shows, measured from the turn each pose was made in (ground truth) and as the census books it.
//      Required: mode 1: age 0 in every picture of every pattern without a broken pair, at most 1 and only after a broken pair otherwise; never two poses left
//      out in a row; an every-2nd cut saves what it saved in a steady pair (within the restarts); the census equals the ground truth.
//      mode 2: age 0 for the every-2nd cuts; mode 0: the census shows today's ages (> 0).
//   3. the flower members (GXV_HR_PLANTMOVE): the animation clock with its latch (XanimePlayer::updateAfterMovement / calcAnm), the replay of the owed step and the
//      drawn turn's flush (mStaleXLatch): the frame each picture shows equals the original game's, mode 0 and 1, every pattern, three deferral variants; mode 1
//      leaves no step owed at a picture in a steady pair.
//   4. mutants (each must be caught): 1 skip on every drawn turn, 2 skip on the undrawn turn, 3 no cap at 2, 4 the pair set not exempted, 5 the census books the
//      wrong turn, 6 the flush does not give the latch back, 7 the alternation's mode 0 phase flipped.
#define PORT_POSE_PHASE_MUTANT 1
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "port/pose_phase.h"

namespace port { namespace posephase { int gMutant = 0; } }
using namespace port::posephase;

static int fails = 0;
static bool gQuiet = false;
#define CHECK(c, ...) do { if (!(c)) { ++fails; if (!gQuiet && fails < 40) { std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

static uint32_t sRng = 12345;
static uint32_t Rnd() { sRng = sRng * 1664525u + 1013904223u; return sRng >> 8; }
static bool Chance(uint32_t perMille) { return Rnd() % 1000u < perMille; }

// ---- 1. identity of mode 0 ---------------------------------------------------------------------------------------------------------------------------------
// The budget's alternation as LiveActor.cpp PbAnimSkips wrote it (copied from integ-3.1, e44c45d8).
static bool OrigAlternate(uint8_t& half, uintptr_t addr) {
    switch (half) {
    case 1: half = 2; return true;
    case 2: half = 1; return false;
    default: half = (addr >> 4) & 1 ? 1 : 2; return false;
    }
}
static void TestIdentity() {
    for (int run = 0; run < 2000; ++run) {
        uint8_t a = 0xFF, b = 0xFF, c = 0xFF, d = 0xFF, h1 = 0, h2 = 0;
        const uint32_t seed = Rnd();
        const uintptr_t addr = static_cast<uintptr_t>(Rnd()) << 4;
        for (int t = 0; t < 64; ++t) {
            const int n = static_cast<int>(Rnd() % 6);  // 0..5 (the callers pass 0 or 2..4; the rule must agree everywhere)
            const bool skipTurn = Chance(500);
            if (Chance(80)) { a = b = 0xFF; }
            if (Chance(80)) { c = d = 0xFF; }
            if (Chance(80)) { h1 = h2 = 0; }
            if (n >= 0) {
                const bool r0 = port::cliprange::FarAnimSkip(a, n, seed);
                const bool r1 = Cadence(b, n, seed, 0, skipTurn);
                CHECK(r0 == r1 && a == b, "Cadence mode 0 = FarAnimSkip (n %d)", n);
                if (n >= 3) {  // mode 2 leaves the every-3rd / 4th as they are
                    uint8_t x = a, y = a;
                    const bool q0 = port::cliprange::FarAnimSkip(x, n, seed);
                    const bool q1 = Cadence(y, n, seed, 2, skipTurn);
                    CHECK(q0 == q1 && x == y, "Cadence mode 2 = FarAnimSkip for n %d", n);
                }
            }
            const float moved2 = static_cast<float>(Rnd() % 400), dist = static_cast<float>(Rnd() % 9000), tanHalf = (Rnd() % 4) ? 0.45f : 0.0f;
            const float lines = static_cast<float>(Rnd() % 4);
            const bool s0 = port::cpucuts::CharPoseSkip(c, n, seed, moved2, dist, tanHalf, lines);
            const bool s1 = CharCadence(d, n, seed, moved2, dist, tanHalf, lines, 0, skipTurn);
            CHECK(s0 == s1 && c == d, "CharCadence mode 0 = CharPoseSkip");
            uint8_t h3 = h2;
            const bool p0 = OrigAlternate(h1, addr);
            const bool p1 = Alternate(h2, static_cast<uint32_t>(addr >> 4), 0, skipTurn);
            (void)h3;
            CHECK(p0 == p1 && h1 == h2, "Alternate mode 0 = the old switch");
            const unsigned parity = Rnd();
            CHECK(StarTurnSkip(parity, seed, 0, skipTurn) == (((parity + seed) & 1u) != 0u), "star bits mode 0 = the parity rule");
            const int i = static_cast<int>(Rnd() % 9);
            const uint8_t tick = static_cast<uint8_t>(Rnd() & 1);
            CHECK(PlantRestTurn(i, tick, 0, skipTurn) == (((i ^ tick) & 1) != 0), "plants mode 0 = the group's half");
        }
    }
    CHECK(StarFlushAllowed(0) && StarFlushAllowed(2) && !StarFlushAllowed(1), "the flush rule: off in mode 1 only");
    CHECK(ParseMode(nullptr) == 0 && ParseMode("") == 0 && ParseMode("0") == 0 && ParseMode("1") == 1 && ParseMode("2") == 2 && ParseMode("3") == 0 && ParseMode("x") == 0,
          "ParseMode");
}

// ---- 2. the turn / picture model ---------------------------------------------------------------------------------------------------------------------------
enum Pattern { kPair, kPairCatchUp, kPairBroken, kPlain60, kUndrawnRuns, kRandom, kPatterns };
static const char* const kPatName[kPatterns] = {"pair", "pair+catch-up", "pair+broken", "plain 60", "undrawn runs", "random"};
static std::vector<bool> MakePattern(Pattern p, int turns) {
    std::vector<bool> d;
    d.reserve(turns + 8);
    bool drawn = true;
    while (static_cast<int>(d.size()) < turns) {
        switch (p) {
        case kPair: d.push_back(drawn); drawn = !drawn; break;
        case kPairCatchUp: d.push_back(drawn); if (!drawn && Chance(60)) d.push_back(false); drawn = !drawn; break;
        case kPairBroken: d.push_back(drawn); if (drawn && Chance(60)) d.push_back(true); drawn = !drawn; break;
        case kPlain60: d.push_back(true); break;
        case kUndrawnRuns: { const int run = 1 + static_cast<int>(Rnd() % 5); for (int k = 0; k < run; ++k) d.push_back(false); d.push_back(true); break; }
        default: d.push_back(Chance(500)); break;
        }
    }
    d.resize(turns);
    return d;
}

// kinds 0..5 as the census's; n: the cut's count (0 budget alternation: 2; 5 star bits: 2)
struct Actor {
    int kind, n;
    uint32_t seed;
    uint8_t skips = 0xFF, half = 0;
    bool far = true, tagged = false;
    int posed = -1, inRow = 0, maxInRow = 0;
    uint64_t skipped = 0;
};
struct ModelResult {
    uint64_t truthA[kKinds][kAges] = {}, truthB[kAges] = {};
    uint64_t pictures = 0, brokenPictures = 0, skipsByKind[kKinds] = {}, decisionsByKind[kKinds] = {};
    int maxInRow = 0;
    Census census;
};
static int gGlueMutant = 0;  // 4: the pair set is not exempted
static void RunModel(int mode, Pattern pat, int turns, ModelResult& r, bool hrw = true) {
    std::vector<bool> drawnAt = MakePattern(pat, turns);
    std::vector<Actor> actors;
    for (int i = 0; i < 200; ++i) {
        Actor a;
        a.kind = i % 6;
        a.n = (a.kind == 0 || a.kind == 5) ? 2 : 2 + static_cast<int>(Rnd() % 3);
        a.seed = Rnd();
        actors.push_back(a);
    }
    Phase phase;
    bool prevDrawn = true;
    for (int t = 0; t < turns; ++t) {
        const bool drawn = drawnAt[t];
        const bool skip = phase.Begin(drawn);
        const bool ppSkip = mode != 0 && skip;
        const bool broke = phase.Broken(drawn);
        r.census.TurnBegin(drawn, skip, broke);
        // the picture(s) of this turn's start
        if (drawn && t > 0) {
            ++r.pictures;
            if (broke) ++r.brokenPictures;
            for (Actor& a : actors) {
                if (a.posed < 0) continue;
                const int age = (t - 1) - a.posed;
                ++r.truthA[a.kind][Census::Age(age)];
                if (age > 0 && mode == 1 && !broke) CHECK(false, "mode 1: a picture shows a pose %d turns old (pattern %s, kind %d, turn %d)", age, kPatName[pat], a.kind, t);
                if (age > 1 && mode == 1) CHECK(false, "mode 1: a pose more than one turn old even after a broken pair (kind %d)", a.kind);
            }
        }
        if (!drawn && prevDrawn && t > 0 && hrw) {  // 60 Assist's second picture: the pair set from the drawn turn's pose
            for (Actor& a : actors)
                if (a.tagged && a.posed >= 0) {
                    const int age = (t - 1) - a.posed;
                    ++r.truthB[Census::Age(age)];
                    if (age > 0 && mode == 1) CHECK(false, "mode 1: the second picture shows a pair-set pose %d turns old", age);
                }
        }
        // the pair set: tagged in a drawn turn's picture, cleared at the next turn's start
        for (Actor& a : actors) a.tagged = drawn && hrw && Chance(100);
        // this turn's calc-anim (after update())
        for (Actor& a : actors) {
            if (Chance(15)) a.far = !a.far;
            if (a.posed < 0 || !a.far) {  // near (or never posed): posed every turn, the counts start again (as the callers do)
                a.posed = t;
                a.skips = 0xFF;
                a.half = 0;
                a.inRow = 0;
                continue;
            }
            if (mode != 0 && a.tagged && gGlueMutant != 4) {  // (LiveActor.cpp PbAnimSkips / port_cpulod_mat_skip / port_starbatch_far_skip: posed every turn)
                a.posed = t;
                a.skips = 0xFF;
                a.half = 0;
                a.inRow = 0;
                r.census.Note(a.kind, 0, true, drawn);
                continue;
            }
            bool leave = false;
            int noted = 0;
            switch (a.kind) {
            case 0: leave = Alternate(a.half, a.seed, mode, ppSkip); noted = leave ? 1 : 0; break;
            case 2: {
                const bool moved = Chance(30);
                leave = CharCadence(a.skips, a.n, a.seed, moved ? 1e6f : 0.0f, 5000.0f, 0.45f, 1.0f, mode, ppSkip);
                noted = leave ? a.skips : 0;
                break;
            }
            case 5: leave = StarTurnSkip(static_cast<unsigned>(t), a.seed, mode, ppSkip); noted = leave ? 1 : 0; break;
            default: leave = Cadence(a.skips, a.n, a.seed, mode, ppSkip); noted = leave ? a.skips : 0; break;
            }
            ++r.decisionsByKind[a.kind];
            if (leave) {
                ++a.skipped;
                ++r.skipsByKind[a.kind];
                ++a.inRow;
                if (a.inRow > a.maxInRow) a.maxInRow = a.inRow;
            } else {
                a.posed = t;
                a.inRow = 0;
            }
            r.census.Note(a.kind, noted, a.tagged, drawn);
        }
        prevDrawn = drawn;
    }
    for (const Actor& a : actors) if (a.maxInRow > r.maxInRow) r.maxInRow = a.maxInRow;
}
static bool HasOld(const ModelResult& r, int kind) { return r.truthA[kind][1] + r.truthA[kind][2] + r.truthA[kind][3] != 0; }
// The census books each left-out pose with the cut's own count (mCrSkip / mCpDlSkip after the decision). Whether a picture showed an old pose is booked
// exactly in every mode (a decision left out in the turn before the picture <=> the picture shows a pose >= 1 turn old). The age itself is exact where the
// count is the run of poses left out: every aligned cut (modes 1 / 2), the alternation and the star rule; for an every-3rd / 4th cut in mode 0 the count
// starts at its phase (seed % n) after a fresh start, so ages 2 / 3 are an upper bound there.
static void CheckCensusEqualsTruth(const ModelResult& r, int mode, const char* what) {
    for (int k = 0; k < 6; ++k) {
        uint64_t c = 0, t = 0;
        for (int a = 1; a < kAges; ++a) { c += r.census.seen[k][a]; t += r.truthA[k][a]; }
        CHECK(c == t, "%s: census kind %d books %llu old poses in pictures, the pictures showed %llu", what, k, static_cast<unsigned long long>(c),
              static_cast<unsigned long long>(t));
        const bool exactAge = mode == 1 || k == 0 || k == 5;
        if (exactAge)
            for (int a = 1; a < kAges; ++a)
                CHECK(r.census.seen[k][a] == r.truthA[k][a], "%s: census kind %d age %d books %llu, the pictures showed %llu", what, k, a,
                      static_cast<unsigned long long>(r.census.seen[k][a]), static_cast<unsigned long long>(r.truthA[k][a]));
    }
}
static void TestModel(bool print) {
    for (int p = 0; p < kPatterns; ++p) {
        const Pattern pat = static_cast<Pattern>(p);
        ModelResult m0, m1, m2;
        sRng = 777 + p;
        RunModel(0, pat, 6000, m0);
        sRng = 777 + p;
        RunModel(1, pat, 6000, m1);
        sRng = 777 + p;
        RunModel(2, pat, 6000, m2);
        const bool clean = pat != kPairBroken && pat != kRandom;
        // mode 1
        CHECK(m1.maxInRow <= 1, "mode 1 (%s): %d poses left out in a row", kPatName[p], m1.maxInRow);
        CHECK(m1.census.MaxAgeSeen() <= (clean ? 0 : 1), "mode 1 (%s): census max age %d", kPatName[p], m1.census.MaxAgeSeen());
        if (clean) CHECK(m1.census.broken == 0, "mode 1 (%s): no broken pair", kPatName[p]);
        for (int a = 1; a < kAges; ++a) CHECK(m1.census.pairB[a] == 0 && m1.truthB[a] == 0, "mode 1 (%s): the pair set is posed every turn", kPatName[p]);
        if (pat == kPlain60) for (int k = 0; k < 6; ++k) CHECK(m1.skipsByKind[k] == 0, "mode 1, plain 60: nothing is left out (kind %d)", k);
        // mode 2: the every-2nd cuts exact
        if (clean) {
            CHECK(!HasOld(m2, 0) && !HasOld(m2, 5), "mode 2 (%s): the every-2nd cuts show age 0", kPatName[p]);
        }
        // the census books what the pictures showed, in every mode
        CheckCensusEqualsTruth(m0, 0, "mode 0");
        CheckCensusEqualsTruth(m1, 1, "mode 1");
        CheckCensusEqualsTruth(m2, 2, "mode 2");
        // cost in the steady pair: an every-2nd cut leaves out what it left out (within the restarts of the far state)
        if (pat == kPair) {
            // (without 60 Assist's pair set: the model tags 10 % of the actors in every picture, far more than the game's few, and a tagged model is posed)
            ModelResult c0, c1;
            sRng = 4242;
            RunModel(0, pat, 6000, c0, false);
            sRng = 4242;
            RunModel(1, pat, 6000, c1, false);
            for (int k : {0, 5}) {
                const double a = static_cast<double>(c0.skipsByKind[k]), b = static_cast<double>(c1.skipsByKind[k]);
                CHECK(a > 0 && b > 0 && (b > a ? b - a : a - b) / a < 0.03, "pair: kind %d leaves out %.0f (mode 0) vs %.0f (mode 1)", k, a, b);
                if (print && !gQuiet) std::printf("  pair, every-2nd cut %s: poses left out %.0f (mode 0) vs %.0f (mode 1)\n", KindName(k), a, b);
            }
            CHECK(HasOld(m0, 0) && HasOld(m0, 1) && m0.census.MaxAgeSeen() >= 2, "mode 0 (pair): the census sees today's old poses");
        }
        if (print && !gQuiet) {
            auto share = [](const ModelResult& r) {
                uint64_t old = 0, all = 0;
                for (int k = 0; k < 6; ++k) for (int a = 0; a < kAges; ++a) { all += r.truthA[k][a]; if (a) old += r.truthA[k][a]; }
                return all ? 100.0 * static_cast<double>(old) / static_cast<double>(all) : 0.0;
            };
            uint64_t s0 = 0, s1 = 0, s2 = 0;
            for (int k = 0; k < 6; ++k) { s0 += m0.skipsByKind[k]; s1 += m1.skipsByKind[k]; s2 += m2.skipsByKind[k]; }
            std::printf("  %-14s pictures %5llu  old poses in pictures: mode0 %5.1f%% (max age %d)  mode1 %4.1f%% (max %d, broken %u)  mode2 %4.1f%% (max %d)  "
                        "poses left out: mode0 %llu mode1 %llu mode2 %llu\n",
                        kPatName[p], static_cast<unsigned long long>(m1.pictures), share(m0), m0.census.MaxAgeSeen(), share(m1), m1.census.MaxAgeSeen(),
                        m1.census.broken, share(m2), m2.census.MaxAgeSeen(), static_cast<unsigned long long>(s0), static_cast<unsigned long long>(s1),
                        static_cast<unsigned long long>(s2));
        }
    }
}

// ---- 3. the flower members (GXV_HR_PLANTMOVE) --------------------------------------------------------------------------------------------------------------
// XanimePlayer's clock: updateAfterMovement (a step when the latch is clear; _84 keeps the frame before it) and calcAnm (poses _84 while latched, else the frame).
struct Clock {
    int frame = 0, f84 = 0;
    bool l88 = false;
    void Update(int maxF) { if (!l88) { f84 = frame; frame = (frame + 1) % maxF; l88 = true; } }
    int Calc() { const int f = l88 ? f84 : frame; l88 = false; return f; }
};
struct Member {
    Clock c;
    bool owed = false, stale = false, xlatch = false;
    int shown = -1;
};
static int gPlantMutant = 0;  // 6: the flush does not give the latch back
static void PlantRun(int mode, Pattern pat, int defer, uint64_t* frameMismatch, uint64_t* owedAtPicture, uint64_t* skipped) {
    const int kM = 7, maxF = 97, turns = 4000;
    std::vector<bool> drawnAt = MakePattern(pat, turns);
    Member ref[kM], cut[kM];
    Phase phase;
    uint8_t tick = 0;
    for (int t = 0; t < turns; ++t) {
        const bool drawn = drawnAt[t];
        const bool skip = phase.Begin(drawn) && mode != 0;
        if (drawn) {
            for (int i = 0; i < kM; ++i) {
                Member& m = cut[i];
                if (m.stale) {  // the drawn turn's flush (LiveActor.cpp port_anim_flush_stale)
                    if (m.xlatch && gPlantMutant != 6) m.c.l88 = true;
                    m.xlatch = false;
                    m.stale = false;
                    m.shown = m.c.Calc();
                }
                if (t > 0) {
                    if (m.shown != ref[i].shown) ++*frameMismatch;
                    if (m.owed) ++*owedAtPicture;
                }
            }
        }
        // movement: the reference moves every turn; the cut's resting members (the group is far and they rest throughout)
        tick ^= 1;
        for (int i = 0; i < kM; ++i) {
            ref[i].c.Update(maxF);
            Member& m = cut[i];
            if (!m.owed && PlantRestTurn(i, tick, mode, skip)) {
                m.owed = true;
                ++*skipped;
            } else {
                if (m.owed) {  // PlantMember::portPayOwed: the left-out turn's step, then the latch as that turn's calc-anim left it
                    m.owed = false;
                    m.c.Update(maxF);
                    m.c.l88 = false;
                }
                m.c.Update(maxF);
            }
        }
        // calc-anim: the reference in the turn; the cut deferred (defer 1: every turn, 2: undrawn turns only) or in the turn (0)
        for (int i = 0; i < kM; ++i) {
            ref[i].shown = ref[i].c.Calc();
            Member& m = cut[i];
            const bool deferNow = defer == 1 || (defer == 2 && !drawn);
            if (deferNow) {
                m.xlatch = m.c.l88;
                m.c.l88 = false;
                m.stale = true;
            } else {
                m.xlatch = m.stale && m.c.l88;
                m.shown = m.c.Calc();
            }
        }
    }
}
static void TestPlants(bool print) {
    for (int p = 0; p < kPatterns; ++p)
        for (int mode = 0; mode <= 1; ++mode)
            for (int defer = 0; defer <= 2; ++defer) {
                uint64_t mis = 0, owed = 0, skipped = 0;
                sRng = 99 + p;
                PlantRun(mode, static_cast<Pattern>(p), defer, &mis, &owed, &skipped);
                CHECK(mis == 0, "plants (%s, mode %d, defer %d): %llu pictures show another frame than the original", kPatName[p], mode, defer,
                      static_cast<unsigned long long>(mis));
                if (mode == 1 && (p == kPair || p == kPairCatchUp || p == kPlain60 || p == kUndrawnRuns))
                    CHECK(owed == 0, "plants (%s, mode 1): a step owed at %llu pictures", kPatName[p], static_cast<unsigned long long>(owed));
                if (mode == 0 && p == kPair) CHECK(owed > 0, "plants (pair, mode 0): the half by tick leaves steps owed at pictures (the light / sound lag)");
                if (p == kPair) CHECK(skipped > 0, "plants (pair, mode %d): something is left out", mode);
                if (print && !gQuiet && defer == 1 && (p == kPair || p == kPlain60))
                    std::printf("  plants %-9s mode %d: frame mismatches %llu, steps owed at a picture %llu, movements left out %llu\n", kPatName[p], mode,
                                static_cast<unsigned long long>(mis), static_cast<unsigned long long>(owed), static_cast<unsigned long long>(skipped));
            }
}

// ---- 4. mutants --------------------------------------------------------------------------------------------------------------------------------------------
static void TestMutants() {
    struct M { const char* name; int core, glue, plant; } ms[] = {
        {"1 skip on every drawn turn", 1, 0, 0}, {"2 skip on the undrawn turn", 2, 0, 0}, {"3 no cap at every 2nd", 3, 0, 0},
        {"4 pair set not exempted", 0, 4, 0}, {"5 census books the wrong turn", 5, 0, 0}, {"6 flush keeps the latch", 0, 0, 6},
    };
    int caught = 0;
    const int saved = fails;
    for (const M& m : ms) {
        port::posephase::gMutant = m.core;
        gGlueMutant = m.glue;
        gPlantMutant = m.plant;
        gQuiet = true;
        const int before = fails;
        TestModel(false);
        TestPlants(false);
        const bool c = fails > before;
        gQuiet = false;
        std::printf("  mutant %-34s %s\n", m.name, c ? "caught" : "NOT CAUGHT");
        caught += c ? 1 : 0;
        fails = before;
    }
    // 7: mode 0's alternation phase flipped (a mode-0 change the identity test must see)
    {
        gQuiet = true;
        const int before = fails;
        uint8_t h1 = 0, h2 = 0;
        for (int t = 0; t < 8; ++t) {
            const bool a = OrigAlternate(h1, 0x10);
            uint8_t hx = h2;
            bool b = Alternate(hx, 0, 0, false);  // (the mutant: address bit 0 where the old rule reads 1)
            h2 = hx;
            CHECK(a == b && h1 == h2, "mutant 7");
        }
        const bool c = fails > before;
        gQuiet = false;
        std::printf("  mutant %-34s %s\n", "7 alternation phase flipped", c ? "caught" : "NOT CAUGHT");
        caught += c ? 1 : 0;
        fails = before;
    }
    port::posephase::gMutant = 0;
    gGlueMutant = 0;
    gPlantMutant = 0;
    fails = saved;
    CHECK(caught == 7, "%d of 7 mutants caught", caught);
}

int main() {
    TestIdentity();
    std::printf("turn / picture model (6000 turns, 200 far actors, near/far changes, 60 Assist's pair set):\n");
    TestModel(true);
    std::printf("flower members (GXV_HR_PLANTMOVE, the clock latch and the flush):\n");
    TestPlants(true);
    std::printf("mutants:\n");
    TestMutants();
    if (fails) {
        std::printf("pose_phase: FAIL (%d)\n", fails);
        return 1;
    }
    std::printf("pose_phase: PASS\n");
    return 0;
}
