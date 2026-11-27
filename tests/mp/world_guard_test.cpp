// Host tests for decomp/include/Game/MP/MpWorldGuard.hpp (security review finding 12): per-sender rate limits, distance plausibility,
// REGCRC reply amplification, and "honest play never trips it".
//   g++ -std=c++20 -fsanitize=address,undefined -g -Wall -Wextra -Idecomp/include tests/mp/world_guard_test.cpp -o /tmp/world_guard_test
#include "Game/MP/MpWorldGuard.hpp"
#include <stdio.h>
#include <stdlib.h>
using namespace MpWorldGuard;
static int gChecks = 0, gFail = 0;
#define CHECK(c) do { gChecks++; if (!(c)) { gFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// a flood is cut to burst + rate * time; one honest sender is never refused at several times a real player's pace
static void testRates() {
    for (int k = 0; k < K_COUNT; k++) {
        SenderLimits l;
        int ok = 0;
        for (int i = 0; i < 6000; i++) ok += l.allow(1, (Kind)k, 1000u + (uint32_t)(i / 60 * 0));   // 6000 at once
        CHECK(ok == kLimit[k].burst);                                                                  // only the burst gets through
        SenderLimits f;
        ok = 0;
        for (uint32_t t = 0; t < 600; t++) for (int j = 0; j < 100; j++) ok += f.allow(2, (Kind)k, t);   // 100 / turn for 10 s
        if (ok > kLimit[k].burst + 10 * kLimit[k].perSecond + 2 || ok < 10 * kLimit[k].perSecond) printf("kind %d ok=%d\n", k, ok);
        CHECK(ok <= kLimit[k].burst + 10 * kLimit[k].perSecond + 2 && ok >= 10 * kLimit[k].perSecond);   // burst + the sustained rate
        // another sender is unaffected by it
        CHECK(f.allow(3, (Kind)k, 600));
        // an honest player: half the sustained rate, forever: never refused
        SenderLimits h;
        int refused = 0;
        const int gap = 120 / kLimit[k].perSecond;   // a event every 2 / rate seconds... (60 / rate turns = rate; double = half rate)
        for (uint32_t t = 0; t < 60000; t += (uint32_t)gap) refused += !h.allow(0, (Kind)k, t);
        CHECK(refused == 0);
        // a burst of honest size (a cutscene's switches, a bomb chain) after a quiet second
        SenderLimits b;
        refused = 0;
        for (int i = 0; i < kLimit[k].burst; i++) refused += !b.allow(0, (Kind)k, 500);
        CHECK(refused == 0);
    }
    SenderLimits l;
    CHECK(!l.allow(-1, K_KILL, 0) && !l.allow(4, K_KILL, 0) && !l.allow(0, (Kind)K_COUNT, 0));
    // a turn counter that goes backwards never panics and never unlocks a flood
    SenderLimits r;
    for (int i = 0; i < 200; i++) r.allow(1, K_KILL, 5000);
    int ok = 0;
    for (int i = 0; i < 200; i++) ok += r.allow(1, K_KILL, 10);
    CHECK(ok <= 1);
}

static void testDistance() {
    const float a[3] = {0, 0, 0};
    float b[3] = {1500, 200, -900};              // a launch star's second of flight, a long jump: near
    CHECK(nearEnough(a, b, kDefaultMaxDistance));
    b[0] = 29000; b[1] = 0; b[2] = 0;
    CHECK(nearEnough(a, b, kDefaultMaxDistance));
    b[0] = 1e6f;                                  // nowhere in a galaxy
    CHECK(!nearEnough(a, b, kDefaultMaxDistance));
    b[0] = 0.0f / 0.0f;
    CHECK(!nearEnough(a, b, kDefaultMaxDistance));
    b[0] = 1.0f / 0.0f;
    CHECK(!nearEnough(a, b, kDefaultMaxDistance));
}

// worldsync: a repair request (REGCRC with the repair flag) is answered although the guest's CRC is the same as at its placement
static void testRepairGate() {
    ReplyGate g;
    g.newPlacement();
    CHECK(g.allow(1, 0xabc, 0));                                  // the placement announcement
    CHECK(!g.allow(1, 0xabc, 5000000000LL));                      // a plain repeat: never again (this was the repair path before)
    CHECK(g.allowRepair(1, 5000000000LL));                        // a repair is answered
    CHECK(!g.allowRepair(1, 5000000000LL + 500000000LL));         // not twice inside a second
    CHECK(g.allowRepair(1, 7000000000LL));
    int n = 2;
    for (int i = 0; i < 100; i++) n += g.allowRepair(1, 7000000000LL + (int64_t)(i + 1) * 1000000000LL);   // one a second for 100 s
    CHECK(n > ReplyGate::kMaxRepairs && n <= ReplyGate::kMaxRepairs + 10);   // the allowance of 8 refills 1 per 10 s: not a hard stop, not a flood
    ReplyGate h;
    h.newPlacement();
    int burst = 0;
    for (int i = 0; i < 50; i++) burst += h.allowRepair(1, (int64_t)i * 1100000000LL);        // 50 requests, 1.1 s apart: the first 8 + the refill
    CHECK(burst >= ReplyGate::kMaxRepairs && burst <= ReplyGate::kMaxRepairs + 6);
    ReplyGate k;
    k.newPlacement();
    for (int i = 0; i < ReplyGate::kMaxRepairs; i++) CHECK(k.allowRepair(1, (int64_t)i * 1100000000LL));
    CHECK(!k.allowRepair(1, 9000000000LL));                                                     // spent (1.1 s after the 8th)
    CHECK(k.allowRepair(1, 9000000000LL + 10000000000LL));                                      // one back after 10 s
    CHECK(k.allowRepair(1, 9000000000LL + 100000000000LL) && k.allowRepair(1, 9000000000LL + 102000000000LL));   // long later: refilled
    CHECK(g.allowRepair(2, 0) && !g.allowRepair(-1, 0) && !g.allowRepair(kSlots, 0));
    g.newPlacement();
    CHECK(g.allowRepair(1, 400000000000LL));                      // a new placement starts over
}

// REGCRC: 6 bytes in, a 568-byte SWITCHMAP + REGCRC out. A guest repeating it every turn for a minute:
static void testAmplification() {
    const int inBytes = 6, outBytes = 568 + 6;
    // before (no gate): every message is answered
    long before = 0;
    for (int i = 0; i < 3600; i++) before += outBytes;
    ReplyGate g;
    long after = 0;
    int answers = 0;
    g.newPlacement();
    for (int i = 0; i < 3600; i++) if (g.allow(1, 0xabc, (int64_t)i * 16666667)) { after += outBytes; answers++; }
    printf("regcrc amplification over 60 s of one repeated REGCRC per turn: before %ld bytes out for %d in (x%d), after %ld bytes (%d answers)\n",
           before, 3600 * inBytes, outBytes / inBytes, after, answers);
    CHECK(answers == 1);                              // same CRC, same placement: once
    CHECK(after * 100 <= before);
    // a changing CRC is answered at most once a second
    ReplyGate c;
    c.newPlacement();
    int n = 0;
    for (int i = 0; i < 3600; i++) n += c.allow(1, 1000u + (uint32_t)i, (int64_t)i * 16666667);
    CHECK(n >= 55 && n <= 61);
    // honest: the guest's CRC at each placement is answered at once, also after a reload of the same serial
    ReplyGate h;
    int64_t t = 0;
    for (int placement = 0; placement < 5; placement++) {
        h.newPlacement();
        t += 3000000000LL;
        CHECK(h.allow(1, 0x1111u + (uint32_t)placement, t));
        CHECK(h.allow(2, 0x2222u, t));             // another guest: its own
        CHECK(!h.allow(1, 0x1111u + (uint32_t)placement, t + 1000));
    }
    // a repeat of the same CRC after a placement started again is answered once more
    h.newPlacement();
    CHECK(h.allow(1, 0x1111u + 4u, t + 4000000000LL));
    ReplyGate x;
    CHECK(!x.allow(-1, 1, 0) && !x.allow(9, 1, 0));
}

static void testFuzz() {
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    SenderLimits l;
    ReplyGate g;
    for (int i = 0; i < 2000000; i++) {
        const int slot = (int)(rnd() % 7) - 1;
        (void)l.allow(slot, (Kind)(rnd() % 7), rnd() % 100000);
        (void)g.allow(slot, rnd() % 4, (int64_t)rnd() * 100000 - 5000000000LL);
        if ((i & 1023) == 0) g.newPlacement();
    }
    CHECK(true);
}

// smallfix: refusals are reported to the core (a fake of mp_core_report_bad here)
static void testReports() {
    struct Fake { int calls = 0, slot = -1; const char* why = nullptr; };
    Fake f;
    auto rep = [&](int s, const char* w) { f.calls++; f.slot = s; f.why = w; };
    // a rate refusal is always reported, with the short constant, for the right slot
    CHECK(reportRate(true, 2, rep) && f.calls == 1 && f.slot == 2 && f.why == kWhyRate);
    // never from a guest console, never for a bad slot
    CHECK(!reportRate(false, 2, rep) && !reportRate(true, -1, rep) && !reportRate(true, 9, rep) && f.calls == 1);
    // far: at or under twice the limit, not reported; past it, reported
    const float s[3] = {0, 0, 0};
    const float max = 30000.0f;
    const float justOver[3] = {max, 0, 0}, twice[3] = {2 * max, 0, 0}, past[3] = {2 * max + 100, 0, 0}, nan[3] = {NAN, 0, 0};
    CHECK(!nearEnough(s, justOver, max - 1) && !reportFar(true, 1, s, justOver, max, rep));   // refused as far, but not reported
    CHECK(!reportFar(true, 1, s, twice, max, rep) && f.calls == 1);
    CHECK(reportFar(true, 1, s, past, max, rep) && f.calls == 2 && f.slot == 1 && f.why == kWhyFar);
    CHECK(!reportFar(false, 1, s, past, max, rep) && f.calls == 2);   // not from a guest console
    CHECK(!reportFar(true, 1, s, nan, max, rep) && f.calls == 2);
    // end to end with the limiter: a flood reports once per refusal, honest pace never
    SenderLimits l;
    int refused = 0;
    for (int i = 0; i < 100; i++) if (!l.allow(1, K_STAR, 500)) { refused++; reportRate(true, 1, rep); }
    CHECK(refused == 100 - kLimit[K_STAR].burst && f.calls == 2 + refused);
}

int main() {
    testReports();
    testRates();
    testDistance();
    testAmplification();
    testRepairGate();
    testFuzz();
    printf("world_guard_test: %d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
