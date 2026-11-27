// undrawn26: host test of platform/include/port/undrawn_lean.h (the rules of GXV_UNDRAWN_LEAN): mode parsing, the tables, the eligibility rules, the
// dirty-since-skipped / violation protocol on a small simulated world, and the state hash.
#include "port/undrawn_lean.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace port::undrawn;

static unsigned gChecks = 0, gFail = 0;
#define REQUIRE(c, msg)                                                  \
    do {                                                                 \
        ++gChecks;                                                       \
        if (!(c)) {                                                      \
            ++gFail;                                                     \
            if (gFail <= 20) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                                \
    } while (0)

static uint32_t sRng = 12345u;
static uint32_t Rnd() { return sRng = sRng * 1664525u + 1013904223u; }

static JointFacts Plain() {
    JointFacts f;
    f.category = kCatEnemy;
    f.collision = f.camera = f.exported = f.jointCallback = f.stale = false;
    f.onFrameThread = true;
    f.posedOnce = true;
    return f;
}
static ShadowFacts PlainShadow() {
    ShadowFacts f;
    f.logicRead = false;
    f.collMode = 1;
    f.gravMode = 1;
    f.pbShadow = false;
    f.age = 500;
    f.minAge = 120;
    return f;
}

static void TestMode() {
    REQUIRE(ParseMode(nullptr) == 0, "unset is 0");
    REQUIRE(ParseMode("") == 0, "empty is 0");
    REQUIRE(ParseMode("0") == 0, "0");
    REQUIRE(ParseMode("1") == 1, "1");
    REQUIRE(ParseMode("2") == 2, "2");
    REQUIRE(ParseMode("3") == 0, "3 is not a mode");
    REQUIRE(ParseMode("11") == 0, "11 is not a mode");
    REQUIRE(ParseMode("on") == 0, "words are not modes");
}

static void TestTables() {
    static Book<6> b;
    b.Clear();
    int a[40];
    for (int i = 0; i < 40; ++i) REQUIRE(!b.Dirty(&a[i]), "clean at start");
    for (int i = 0; i < 20; ++i) b.Mark(&a[i], &a[39]);
    for (int i = 0; i < 20; ++i) REQUIRE(b.Dirty(&a[i]), "marked is dirty");
    for (int i = 20; i < 40; ++i) REQUIRE(!b.Dirty(&a[i]), "unmarked is clean");
    REQUIRE(b.Owner(&a[3]) == &a[39], "owner kept");
    REQUIRE(b.Owner(&a[30]) == nullptr, "no owner for an unmarked object");
    b.Flush();
    for (int i = 0; i < 40; ++i) REQUIRE(!b.Dirty(&a[i]), "a flush makes everything clean");
    b.Mark(&a[5], nullptr);
    REQUIRE(b.Dirty(&a[5]) && !b.Dirty(&a[6]), "marked again after a flush");
    // a table that fills up is cleared by flushes, never wrong
    static Book<5> small;
    small.Clear();
    static int many[500];
    unsigned wrong = 0;
    for (int round = 0; round < 200; ++round) {
        const int base = static_cast<int>(Rnd() % 400);
        for (int i = 0; i < 12; ++i) small.Mark(&many[base + i], nullptr);
        for (int i = 0; i < 12; ++i) wrong += small.full || small.Dirty(&many[base + i]) ? 0u : 1u;
        small.Flush();
        for (int i = 0; i < 12; ++i) wrong += small.Dirty(&many[base + i]) ? 1u : 0u;
    }
    REQUIRE(wrong == 0, "a small table stays right through many flushes");
    // export set
    static ExportSet<5> e;
    e.Clear();
    REQUIRE(!e.Exported(&a[0]), "nothing exported at start");
    REQUIRE(e.Add(&a[0]), "first add is new");
    REQUIRE(!e.Add(&a[0]), "second add is not new");
    REQUIRE(e.Exported(&a[0]) && !e.Exported(&a[1]), "exported is exact");
    for (int i = 0; i < 400; ++i) e.Add(&many[i]);
    REQUIRE(e.full, "the set fills up");
    REQUIRE(e.Exported(&a[17]), "a full set exports everything (conservative)");
    e.Clear();
    REQUIRE(!e.Exported(&a[17]) && !e.full, "clear empties it");
}

static void TestRules() {
    JointFacts f = Plain();
    REQUIRE(JointDefers(f), "a plain enemy's joints may be left out");
    f.category = kCatItem;
    REQUIRE(JointDefers(f), "an item");
    f.category = kCatPlayer;
    REQUIRE(!JointDefers(f), "the player never");
    f.category = kCatPlayerDeco;
    REQUIRE(!JointDefers(f), "the player's decoration never");
    f.category = 13;
    REQUIRE(!JointDefers(f), "a layout never");
    f.category = -1;
    REQUIRE(!JointDefers(f), "an unknown category never");
    f = Plain();
    f.collision = true;
    REQUIRE(JointRefusal(f) == 0, "collision parts: refused (0)");
    f = Plain();
    f.camera = true;
    REQUIRE(JointRefusal(f) == 1, "camera: refused (1)");
    f = Plain();
    f.exported = true;
    REQUIRE(JointRefusal(f) == 2, "exported: refused (2)");
    f = Plain();
    f.jointCallback = true;
    REQUIRE(JointRefusal(f) == 3, "callback: refused (3)");
    f = Plain();
    f.stale = true;
    REQUIRE(JointRefusal(f) == 4, "already stale: the old rules decide (4)");
    f = Plain();
    f.onFrameThread = false;
    REQUIRE(JointRefusal(f) == 4, "loading thread: refused");
    f = Plain();
    f.posedOnce = false;
    REQUIRE(JointRefusal(f) == 4, "never posed: refused");
    // every combination of the five blockers: it defers only with none of them
    for (unsigned m = 0; m < 32; ++m) {
        JointFacts g = Plain();
        g.collision = m & 1;
        g.camera = m & 2;
        g.exported = m & 4;
        g.jointCallback = m & 8;
        g.stale = m & 16;
        REQUIRE(JointDefers(g) == (m == 0), "defers exactly when no blocker holds");
    }
    ShadowFacts s = PlainShadow();
    REQUIRE(ShadowDefers(s), "a plain shadow may be left out");
    s.logicRead = true;
    REQUIRE(!ShadowDefers(s), "read by logic: never");
    s = PlainShadow();
    s.pbShadow = true;
    REQUIRE(!ShadowDefers(s), "Performance budget: never");
    s = PlainShadow();
    s.collMode = 2;
    REQUIRE(!ShadowDefers(s), "one-time collision cast is counted: never");
    s = PlainShadow();
    s.gravMode = 2;
    REQUIRE(!ShadowDefers(s), "one-time gravity is counted: never");
    s.gravMode = 5;
    REQUIRE(!ShadowDefers(s), "one-time private gravity is counted: never");
    for (unsigned g : {0u, 1u, 3u, 4u}) {
        s = PlainShadow();
        s.gravMode = g;
        REQUIRE(ShadowDefers(s), "other gravity modes may");
    }
    s = PlainShadow();
    s.age = 119;
    REQUIRE(!ShadowDefers(s), "too young");
    s.age = 120;
    REQUIRE(ShadowDefers(s), "old enough");
}

// A small world: M models with joints, some read by logic at some turns. The protocol of GameSystem::frameLoop:
//   every turn: decide for each model (calc-anim); logic reads happen in the update of a turn (before its calc-anim);
//   a picture turn: flush before the draw.
// Mode 2 must report a violation exactly when a read finds a model that mode 1 would have skipped and not yet flushed; mode 1 must report the same reads as late.
// And: a model the logic reads at any turn, once it has been read, is never skipped again; a model whose joints are exported at "init" is never skipped.
struct World {
    int model[64];
    bool initExport[64];  // exported at init (a sensor on a joint, a part)
    int readTurn[64];     // the turn at which the logic reads it first (or -1)
};
static unsigned RunWorld(int mode, uint32_t seed, unsigned* skipsOut, unsigned* readsOfSkipped, bool* lateAfterExportOut) {
    sRng = seed;
    static Tracker t;
    t.Init();
    World w;
    for (int i = 0; i < 64; ++i) {
        w.initExport[i] = (Rnd() % 4) == 0;
        w.readTurn[i] = (Rnd() % 3) == 0 ? static_cast<int>(Rnd() % 200) : -1;
        if (w.initExport[i]) t.exports.Add(&w.model[i]);
    }
    unsigned violations = 0, skips = 0, reads = 0;
    bool lateAfterExport = false;
    bool skippedSinceFlush[64] = {};
    bool everRead[64] = {};
    bool skippedAfterRead = false;
    bool prevUndrawn = false;
    for (int turn = 0; turn < 400; ++turn) {
        t.turn++;
        const bool undrawn = (Rnd() % 3) != 0;
        // update: the logic reads
        for (int i = 0; i < 64; ++i) {
            if (w.readTurn[i] >= 0 && turn >= w.readTurn[i] && ((turn - w.readTurn[i]) % 7 == 0)) {
                const bool wasDirty = skippedSinceFlush[i];
                const bool dirty = t.Read(kJoint, mode, &w.model[i]);
                REQUIRE(dirty == wasDirty, "the tracker agrees with the world about a read of a left-out model");
                if (dirty) ++violations;
                if (wasDirty) ++reads;
                everRead[i] = true;
            }
        }
        // calc-anim: the decisions
        if (undrawn || prevUndrawn) {
            for (int i = 0; i < 64; ++i) {
                JointFacts f = Plain();
                const bool skip = t.JointDecide(mode, &w.model[i], &w.model[0], f);
                const bool would = !w.initExport[i] && !everRead[i];
                if (mode == 1) {
                    REQUIRE(skip == would, "mode 1 skips exactly the models nothing has asked for");
                    if (skip && (w.initExport[i] || everRead[i])) skippedAfterRead = true;
                } else if (mode == 2) {
                    REQUIRE(!skip, "mode 2 never skips");
                }
                if (mode == 1 ? skip : would) {
                    skippedSinceFlush[i] = true;
                    ++skips;
                }
            }
        }
        if (!undrawn) {  // a picture is drawn at the start of the next turn... here: flush before this turn's draw
            t.Flush();
            memset(skippedSinceFlush, 0, sizeof skippedSinceFlush);
        }
        prevUndrawn = undrawn;
    }
    *skipsOut = skips;
    *readsOfSkipped = reads;
    *lateAfterExportOut = lateAfterExport || skippedAfterRead;
    if (mode == 2) REQUIRE(t.c.violations[kJoint] == violations, "census counts the violations");
    if (mode == 1) REQUIRE(t.c.lateReads[kJoint] == violations, "census counts the late reads");
    return violations;
}

static void TestProtocol() {
    unsigned total1 = 0, total2 = 0, v1 = 0, v2 = 0;
    for (uint32_t seed = 1; seed <= 60; ++seed) {
        unsigned s1, r1, s2, r2;
        bool bad1, bad2;
        v1 += RunWorld(1, seed, &s1, &r1, &bad1);
        v2 += RunWorld(2, seed, &s2, &r2, &bad2);
        REQUIRE(!bad1 && !bad2, "a model read or exported is never skipped afterwards");
        REQUIRE(s1 == s2, "mode 2 counts exactly what mode 1 skips (same world)");
        total1 += s1;
        total2 += s2;
    }
    REQUIRE(total1 > 1000, "the world skips plenty (the test is not vacuous)");
    REQUIRE(v1 == v2, "mode 1's late reads are mode 2's violations");
    REQUIRE(v2 > 0, "the world has reads of left-out models (the violation path is exercised)");
    // negative control: a tracker that forgets exports would skip an exported model
    static Tracker t;
    t.Init();
    int m;
    JointFacts f = Plain();
    t.exports.Add(&m);
    REQUIRE(!t.JointDecide(1, &m, nullptr, f), "an exported model is not skipped");
    t.Init();
    REQUIRE(t.JointDecide(1, &m, nullptr, f), "negative control: without the export it is");
    // a read of a skipped model: late in mode 1; the model is exported from then on
    REQUIRE(t.Read(kJoint, 1, &m), "mode 1: the read finds it left out");
    REQUIRE(t.c.lateReads[kJoint] == 1 && t.c.violations[kJoint] == 0, "counted as late");
    t.Flush();
    REQUIRE(!t.JointDecide(1, &m, nullptr, f), "never skipped after the read");
    // shadows: the age gate, the read gate
    int ctrl;
    ShadowFacts sf = PlainShadow();
    sf.age = 0;
    t.Init();
    bool first = true;
    unsigned firstSkip = 0;
    for (unsigned turn = 1; turn <= 200; ++turn) {
        t.turn = turn;
        if (t.ShadowDecide(1, &ctrl, nullptr, sf) && first) {
            first = false;
            firstSkip = turn;
        }
    }
    REQUIRE(firstSkip == 121, "a controller is first left out after 120 turns of asking");
    REQUIRE(t.Read(kShadow, 1, &ctrl), "the logic reads a left-out cast: late");
    t.Flush();
    REQUIRE(!t.ShadowDecide(1, &ctrl, nullptr, sf), "never left out after the read");
    // mode 2 for shadows: counted, never skipped
    t.Init();
    for (unsigned turn = 1; turn <= 200; ++turn) {
        t.turn = turn;
        REQUIRE(!t.ShadowDecide(2, &ctrl, nullptr, sf), "mode 2 never skips a cast");
    }
    REQUIRE(t.c.wouldSkip[kShadow] == 80, "mode 2 counts the casts mode 1 would leave out");
    REQUIRE(t.Read(kShadow, 2, &ctrl), "mode 2: the read of a would-be-skipped cast is a violation");
    REQUIRE(t.c.violations[kShadow] == 1, "violation counted");
    t.Flush();
    REQUIRE(!t.Read(kShadow, 2, &ctrl), "after the flush it is clean");
    REQUIRE(t.c.flushed[kShadow] == 1, "one distinct cast was flushed (80 turns of skips, one product)");
}

static void TestHash() {
    Fnv64 a, b;
    REQUIRE(a.h == 14695981039346656037ull, "FNV-1a offset basis");
    a.Byte('a');
    REQUIRE(a.h == 0xaf63dc4c8601ec8cull, "FNV-1a of \"a\" (published vector)");
    a = Fnv64();
    const char* s = "foobar";
    for (const char* c = s; *c; ++c) a.Byte(static_cast<uint8_t>(*c));
    REQUIRE(a.h == 0x85944171f73967e8ull, "FNV-1a of \"foobar\" (published vector)");
    Fnv64 x, y;
    x.F32(1.0f);
    x.F32(2.0f);
    y.F32(2.0f);
    y.F32(1.0f);
    REQUIRE(x.h != y.h, "order matters");
    Fnv64 z0, z1;
    z0.F32(0.0f);
    z1.F32(-0.0f);
    REQUIRE(z0.h != z1.h, "-0.0 differs from 0.0: a bit-identical hash");
    Fnv64 n1, n2;
    n1.F32(1.0f);
    n2.F32(1.0f + 1.1920929e-7f);
    REQUIRE(n1.h != n2.h, "one ulp differs");
    // a hash of a simulated world: same inputs, same hash; one changed bit, another hash
    unsigned same = 0, diff = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        sRng = seed;
        float v[100];
        for (float& f : v) f = static_cast<float>(Rnd() % 100000) * 0.01f;
        Fnv64 h1, h2, h3;
        for (float f : v) { h1.F32(f); h2.F32(f); }
        for (int i = 0; i < 100; ++i) h3.F32(i == 37 ? v[i] + 0.01f : v[i]);
        same += h1.h == h2.h;
        diff += h1.h != h3.h;
    }
    REQUIRE(same == 200 && diff == 200, "the hash is stable and sensitive");
}

int main() {
    TestMode();
    TestTables();
    TestRules();
    TestProtocol();
    TestHash();
    printf("undrawn_lean_test: %u checks, %u failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
