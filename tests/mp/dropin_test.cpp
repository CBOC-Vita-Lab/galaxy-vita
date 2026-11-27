// Host tests for decomp/include/Game/MP/MpDropInLogic.hpp (drop-in spares, binding, arrival curve, memory guard).
//   clang++ -std=c++20 -fsanitize=address,undefined -g -I decomp/include tests/mp/dropin_test.cpp -o /tmp/dropin_test && /tmp/dropin_test
#include "Game/MP/MpDropInLogic.hpp"

#include <cmath>
#include <cstdio>

using namespace MpDropIn;

static int sFail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);    \
            sFail++;                                                     \
        }                                                                \
    } while (0)

enum { MARIO = 0, LUIGI = 1, TOAD = 2, COSMIC = 3, YTOAD = 4, CLUIGI = 5 };

static SceneRoster roster(int local, int maxP, bool dropIn) {
    SceneRoster r{};
    r.localSlot = local;
    r.maxPlayers = maxP;
    r.dropInAllowed = dropIn;
    return r;
}

static Binding emptyBinding() {
    Binding b;
    for (int c = 0; c < MP_DROPIN_LOOKS; c++) b.pool[c] = -2;
    for (int s = 0; s < MP_DROPIN_SLOTS; s++) {
        b.bound[s] = -1;
        b.standIn[s] = false;
    }
    return b;
}

static void testSpareLooks() {
    // host alone (Mario), 4 seats: every other character gets a spare
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    // memfix: 3 free seats -> 3 spares (the first three characters nobody has), not all five
    CHECK(spareLooks(r) == ((1u << LUIGI) | (1u << TOAD) | (1u << COSMIC)));
    CHECK(freeSeats(r) == 3);
    // drop-in off: none
    r.dropInAllowed = false;
    CHECK(spareLooks(r) == 0u);
    r.dropInAllowed = true;
    // room full (2 of 2): none
    r.maxPlayers = 2;
    r.present[1] = true;
    r.look[1] = LUIGI;
    CHECK(spareLooks(r) == 0u);
    // 2 of 4: two free seats, two spares (the first two nobody has)
    r.maxPlayers = 4;
    CHECK(spareLooks(r) == ((1u << TOAD) | (1u << COSMIC)));
    // 3 of 4: one
    r.present[2] = true; r.look[2] = TOAD;
    CHECK(spareLooks(r) == (1u << COSMIC));
    r.present[2] = false;
    // a bad look counts as Mario (as the puppets clamp it)
    r.look[1] = 200;
    CHECK((spareLooks(r) & (1u << MARIO)) == 0 && (spareLooks(r) & (1u << LUIGI)) != 0);
    // maxPlayers above the slot count is clamped
    r.maxPlayers = 9;
    r.present[2] = r.present[3] = true;
    CHECK(spareLooks(r) == 0u);
}

static void testJoinBindsSpare() {
    // the harness case: host Mario (slot 0), Luigi drops in to slot 1. Host scene: spares for 1..5.
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    Binding b = emptyBinding();
    for (int c = 1; c < 6; c++) b.pool[c] = -1;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    for (int s = 0; s < 4; s++) CHECK(st[s].action == ACT_NONE);

    r.present[1] = true;
    r.look[1] = LUIGI;
    step(r, b, st);
    CHECK(st[1].action == ACT_BIND && st[1].newChar == LUIGI);
    CHECK(b.pool[LUIGI] == 1 && b.bound[1] == LUIGI);
    CHECK(st[0].action == ACT_NONE);   // never the local slot
    // stable afterwards
    step(r, b, st);
    CHECK(st[1].action == ACT_NONE);

    // the Luigi leaves: his puppet goes back to the pool
    r.present[1] = false;
    step(r, b, st);
    CHECK(st[1].action == ACT_UNBIND && st[1].oldChar == LUIGI && b.pool[LUIGI] == -1 && b.bound[1] == -1);
    // someone else takes slot 1 as Luigi again: same puppet
    r.present[1] = true;
    step(r, b, st);
    CHECK(st[1].action == ACT_BIND && b.pool[LUIGI] == 1);
}

static void testNoStandIn() {
    // no spare of the joiner's character (room was full at init, or memory guard): hidden, never another character
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    Binding b = emptyBinding();   // no spares at all
    r.present[2] = true;
    r.look[2] = TOAD;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[2].action == ACT_HIDDEN && st[2].newChar == TOAD && b.bound[2] == -1);
    // the local MarioActor's character (Mario) is never handed out either: the pool has no Mario
    r.look[2] = MARIO;
    step(r, b, st);
    CHECK(st[2].action == ACT_HIDDEN && b.bound[2] == -1);
}

static void testSwapSameTurn() {
    // slot 1 leaves (Toad) and slot 2 joins with Toad in the same turn: unbinding runs first, slot 2 gets the puppet
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    Binding b = emptyBinding();
    b.pool[TOAD] = 1;
    b.bound[1] = TOAD;
    r.present[2] = true;
    r.look[2] = TOAD;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[1].action == ACT_UNBIND && st[2].action == ACT_BIND && b.pool[TOAD] == 2 && b.bound[2] == TOAD);
}

static void testLookChange() {
    // slot 1 (Luigi) changes to Cosmic Luigi mid-scene: he keeps Luigi until the next scene (his own console's self puppet does too)
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    r.present[1] = true;
    r.look[1] = CLUIGI;
    Binding b = emptyBinding();
    b.pool[LUIGI] = 1;
    b.bound[1] = LUIGI;
    b.pool[CLUIGI] = -1;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[1].action == ACT_NONE && b.bound[1] == LUIGI);
    // now a Luigi drops in to slot 2 (Luigi is free in the roster): slot 1 gives Luigi up and takes the Cosmic Luigi spare,
    // slot 2 gets Luigi; never two Luigis
    r.present[2] = true;
    r.look[2] = LUIGI;
    step(r, b, st);
    CHECK(st[1].action == ACT_REBIND && st[1].oldChar == LUIGI && st[1].newChar == CLUIGI);
    CHECK(st[2].action == ACT_BIND && st[2].newChar == LUIGI);
    CHECK(b.pool[LUIGI] == 2 && b.pool[CLUIGI] == 1 && b.bound[1] == CLUIGI && b.bound[2] == LUIGI);
    // same, but no spare of slot 1's new character: slot 1 is hidden until the next scene, slot 2 is Luigi
    Binding c = emptyBinding();
    c.pool[LUIGI] = 1;
    c.bound[1] = LUIGI;
    r.look[1] = TOAD;
    step(r, c, st);
    CHECK(st[1].action == ACT_UNBIND && st[1].oldChar == LUIGI && st[1].newChar == TOAD && c.bound[1] == -1);
    CHECK(st[2].action == ACT_BIND && c.pool[LUIGI] == 2);
}

static void testDuplicateLookGlitch() {
    // a stale roster says two players are Luigi: the second gets nothing (hidden), never a second Luigi puppet
    SceneRoster r = roster(0, 4, true);
    r.present[0] = r.present[1] = r.present[2] = true;
    r.look[0] = MARIO;
    r.look[1] = LUIGI;
    r.look[2] = LUIGI;
    Binding b = emptyBinding();
    b.pool[LUIGI] = 1;
    b.bound[1] = LUIGI;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[1].action == ACT_NONE && st[2].action == ACT_HIDDEN && b.pool[LUIGI] == 1);
}

static void testArrivalCurve() {
    CHECK(arrivalScale(0) == 1.0f);
    CHECK(arrivalAlpha(0) == 255);
    CHECK(std::fabs(arrivalScale(ARRIVE_TURNS) - 0.35f) < 1e-4f);
    float last = arrivalScale(ARRIVE_TURNS);
    bool overshoot = false;
    for (int left = ARRIVE_TURNS; left >= 1; left--) {
        const float sc = arrivalScale(left);
        CHECK(sc > 0.3f && sc < 1.25f && std::isfinite(sc));
        overshoot = overshoot || sc > 1.0f;
        last = sc;
    }
    CHECK(overshoot);
    CHECK(std::fabs(last - 1.0f) < 0.05f);   // the last arrival turn is within 5 % of the final size: no visible snap
    int prevA = -1;
    for (int left = ARRIVE_TURNS; left >= 1; left--) {
        const int a = arrivalAlpha(left);
        CHECK(a >= prevA && a >= 40 && a <= 255);
        prevA = a;
    }
    CHECK(arrivalAlpha(1) == 255);
    CHECK(arrivalScale(1000) == arrivalScale(ARRIVE_TURNS) && arrivalAlpha(-5) == 255);
}

static void testMemoryGuard() {
    const uint32_t MiB = 1u << 20;
    CHECK(spareFits(8 * MiB, 0x100000));
    CHECK(!spareFits(kKeepNapa, 1));
    CHECK(!spareFits(kKeepNapa - 1, 0));            // no underflow
    // fix2: the NameObjHolder (fixed 0x1300 places) keeps 64 free
    CHECK(objSlotsOk(-1) && objSlotsOk(64) && objSlotsOk(0x1300) && !objSlotsOk(63) && !objSlotsOk(0) && !objSlotsOk(1));
    CHECK(spareFits(kKeepNapa + 0x30000, 0x30000));
    CHECK(!spareFits(kKeepNapa + 0x30000, 0x30001));
}

static void testStandIn() {
    // host Mario; the Luigi spare was refused by the guard but a Cosmic Luigi spare exists: a Luigi joiner gets it as a stand-in
    SceneRoster r = roster(0, 4, true);
    r.present[0] = true;
    r.look[0] = MARIO;
    r.present[1] = true;
    r.look[1] = LUIGI;
    Binding b = emptyBinding();
    b.pool[CLUIGI] = -1;
    b.pool[TOAD] = -1;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[1].action == ACT_HIDDEN && st[1].newChar == LUIGI);
    CHECK(standInFor(r, b, LUIGI) == CLUIGI);
    CHECK(standInFor(r, b, MARIO) == -1);            // no Cosmic Mario spare
    CHECK(standInFor(r, b, YTOAD) == -1);            // Toads: never (no see-through), even with a red Toad spare
    bindStandIn(b, 1, CLUIGI);
    step(r, b, st);
    CHECK(st[1].action == ACT_NONE && b.bound[1] == CLUIGI && b.standIn[1]);
    // a real Cosmic Luigi drops in: the stand-in gives way (slot 1 hidden again), slot 2 gets its own character
    r.present[2] = true;
    r.look[2] = CLUIGI;
    step(r, b, st);
    CHECK(st[1].action == ACT_UNBIND && st[1].newChar == LUIGI && b.bound[1] == -1 && !b.standIn[1]);
    CHECK(st[2].action == ACT_BIND && b.pool[CLUIGI] == 2);
    CHECK(standInFor(r, b, LUIGI) == -1);
}

static void testStandInTradesForReal() {
    // slot 1 (Luigi) on a Cosmic Luigi stand-in; the Luigi puppet becomes free (its holder left): slot 1 takes the real one
    SceneRoster r = roster(0, 4, true);
    r.present[0] = r.present[1] = true;
    r.look[0] = MARIO;
    r.look[1] = LUIGI;
    Binding b = emptyBinding();
    b.pool[CLUIGI] = -1;
    bindStandIn(b, 1, CLUIGI);
    b.pool[LUIGI] = 3;      // slot 3 had Luigi (stale) and left
    b.bound[3] = LUIGI;
    SlotStep st[MP_DROPIN_SLOTS];
    step(r, b, st);
    CHECK(st[3].action == ACT_UNBIND);
    // (slot 3 unbinds this turn; slot 1 trades on the next)
    step(r, b, st);
    CHECK(st[1].action == ACT_REBIND && st[1].oldChar == CLUIGI && st[1].newChar == LUIGI && !b.standIn[1]);
    CHECK(b.pool[CLUIGI] == -1 && b.pool[LUIGI] == 1);
}

static void testArrivalPoint() {
    // identity rotation: front = +z, up = +y: behind = -z, above = +y
    const float pos[3] = {100.0f, 200.0f, 300.0f};
    const int16_t id[4] = {0, 0, 0, 32767};
    float out[3];
    CHECK(arrivalPoint(pos, id, out));
    CHECK(std::fabs(out[0] - 100.0f) < 0.5f && std::fabs(out[1] - (200.0f + kArriveAbove)) < 0.5f &&
          std::fabs(out[2] - (300.0f - kArriveBehind)) < 0.5f);
    // upside down on a planet's underside (180 degrees about x): up = -y, front = -z
    const int16_t flip[4] = {32767, 0, 0, 0};
    CHECK(arrivalPoint(pos, flip, out));
    CHECK(std::fabs(out[1] - (200.0f - kArriveAbove)) < 0.5f && std::fabs(out[2] - (300.0f + kArriveBehind)) < 0.5f);
    // never inside him: at least kArriveBehind away for any rotation
    const int16_t odd[4] = {12000, -20000, 9000, 15000};
    CHECK(arrivalPoint(pos, odd, out));
    const float d = std::sqrt((out[0] - pos[0]) * (out[0] - pos[0]) + (out[1] - pos[1]) * (out[1] - pos[1]) + (out[2] - pos[2]) * (out[2] - pos[2]));
    CHECK(d > kArriveBehind * 0.99f && d < (kArriveBehind + kArriveAbove) * 1.01f);
    // hostile input: zero quaternion, huge / NaN position
    const int16_t zero[4] = {0, 0, 0, 0};
    CHECK(!arrivalPoint(pos, zero, out));
    const float far[3] = {2e6f, 0.0f, 0.0f};
    CHECK(!arrivalPoint(far, id, out));
    const float nan[3] = {std::nanf(""), 0.0f, 0.0f};
    CHECK(!arrivalPoint(nan, id, out));
}


// integ B1: the archives a scene must request in its archive phase = everybody present's + the spares' (nothing can be mounted later)
static void archiveLooksTest() {
    SceneRoster r = {};
    r.maxPlayers = 4; r.dropInAllowed = true; r.localSlot = 0;
    r.present[0] = r.present[1] = true; r.look[0] = MARIO; r.look[1] = LUIGI;
    CHECK(archiveLooks(r) == ((1u << MARIO) | (1u << LUIGI) | (1u << TOAD) | (1u << COSMIC)));   // two free seats: two spares (Toad, Cosmic)
    r.dropInAllowed = false;
    CHECK(archiveLooks(r) == ((1u << MARIO) | (1u << LUIGI)));
    r.dropInAllowed = true; r.present[2] = r.present[3] = true; r.look[2] = TOAD; r.look[3] = YTOAD;
    CHECK(archiveLooks(r) == ((1u << MARIO) | (1u << LUIGI) | (1u << TOAD) | (1u << YTOAD)));   // full room: no spares
    r.present[0] = r.present[1] = r.present[2] = r.present[3] = false;
    CHECK(archiveLooks(r) == 0u || archiveLooks(r) == spareLooks(r));
    r.look[0] = 77;   // a stranger's out-of-range look: clamped like the puppets do
    r.present[0] = true; r.dropInAllowed = false;
    CHECK(archiveLooks(r) == (1u << MARIO));
}

int main() {
    archiveLooksTest();
    testArrivalPoint();
    testSpareLooks();
    testJoinBindsSpare();
    testNoStandIn();
    testSwapSameTurn();
    testLookChange();
    testDuplicateLookGlitch();
    testArrivalCurve();
    testMemoryGuard();
    testStandIn();
    testStandInTradesForReal();
    if (sFail) {
        std::printf("dropin_test: %d failure(s)\n", sFail);
        return 1;
    }
    std::printf("dropin_test: all passed\n");
    return 0;
}
