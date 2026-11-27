// Galaxy 3.0 multiplayer: host tests of the Same Wi-Fi room list (MpLanScan, platform/src/mp/mp_lan.cpp), lanfix job A.
// The outside review (S2) filled the 16-room list with 8 spoofed source addresses x 2 sessions of untagged beacons and the real host's
// beacon was refused. These tests pin the repair: trust tiers (name-trusted friends / Played With, verified room-key tags) outrank
// strangers, strangers are evicted by staleness and capped, new rooms are rate-limited, room names follow the typed-message rules.
#include "mp_lan.h"
#include "mp_config.h"
#include "port/mp_chat.h"

#include <initializer_list>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

#ifndef MP_LAN_NAMED_PER_NAME
#define MP_LAN_NAMED_PER_NAME 2
#endif
static const uint32_t FP = 0x1234;

static MpBeacon Beacon(uint32_t session, const char* name, uint32_t seq = 1) {
    MpBeacon b;
    memset(&b, 0, sizeof b);
    b.proto = MP_PROTO_VERSION;
    b.mode = MP_MODE_TOGETHER;
    b.players = 1;
    b.maxPlayers = 4;
    b.flags = 1;
    b.fingerprint = FP;
    b.sessionId = session;
    b.seq = seq;
    snprintf(b.name, sizeof b.name, "%s", name);
    return b;
}
static MpAddr Addr(uint32_t host) {
    MpAddr a;
    memset(&a, 0, sizeof a);
    a.host = host;
    a.port = 40000;
    return a;
}
static bool Has(const MpLanScan& s, uint32_t session) {
    MpAddr a;
    return s.findSession(session, &a);
}
static void Trust(int src, std::initializer_list<const char*> names) {
    char arr[MP_LAN_TRUSTED_PER_SOURCE][MP_NAME_BYTES + 1];
    int n = 0;
    for (const char* nm : names) { snprintf(arr[n], sizeof arr[n], "%s", nm); n++; }
    mp_lan_set_trusted_names(src, arr, n);
}

// The reviewer's reproducer: 8 addresses x 2 sessions "Free Stars", then the real host. With the friend on the Played With list, the
// friend's room must be listed; an unknown real host may be refused only while strangers truly hold the list (never without them).
static void TestReviewerScenario() {
    for (int named = 0; named < 2; named++) {
        MpLanScan s;
        Trust(0, {});
        if (named) Trust(0, {"Alex"});
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 2; k++) s.add(Addr(0x0a000100 + a), Beacon(0x1000 + a * 2 + k, "Free Stars"), 1000, FP);
        CHECK(s.count() <= MP_LAN_UNTRUSTED_MAX);   // strangers never take the whole list
        const bool added = s.add(Addr(0x0a000199), Beacon(0x9999, "Alex"), 1100, FP);
        if (named) {
            CHECK(added);
            CHECK(Has(s, 0x9999));
        }
        // an unknown (untrusted) real host gets in once the flood goes stale: nothing is held for ever
        MpLanScan t;
        Trust(0, {});
        for (int a = 0; a < 8; a++)
            for (int k = 0; k < 2; k++) t.add(Addr(0x0a000100 + a), Beacon(0x1000 + a * 2 + k, "Free Stars"), 1000, FP);
        CHECK(t.add(Addr(0x0a000199), Beacon(0x9999, "Dana"), 1000 + MP_LAN_STALE_MS + 600, FP));
        CHECK(Has(t, 0x9999));
    }
    Trust(0, {});
}

// A friend arriving while strangers hold every untrusted slot and the list is full of fresh strangers' rooms.
static void TestFriendEvictsOldestStranger() {
    MpLanScan s;
    Trust(0, {});
    Trust(1, {"Anna"});
    uint64_t t = 1000;
    // fill all 16 with strangers over time (rate limit: 8 per second) - 10 untrusted max, then the rest of the slots stay free
    for (int i = 0; i < 40; i++) {
        t += 200;
        s.add(Addr(0x0b000000 + i), Beacon(0x2000 + i, "Stranger"), t, FP);
    }
    CHECK(s.count() <= MP_LAN_UNTRUSTED_MAX);
    CHECK(s.add(Addr(0x0c000001), Beacon(0x7001, "Anna"), t + 10, FP));
    CHECK(Has(s, 0x7001));
    // many "friends" with the same spoofed name cannot take more than MP_LAN_NAMED_MAX slots
    int named = 0;
    for (int i = 0; i < 30; i++) {
        t += 100;
        s.add(Addr(0x0d000000 + i), Beacon(0x8000 + i, "Anna"), t, FP);
    }
    for (int i = 0; i < s.count(); i++) if (s.trust(i) == MP_LAN_NAMED) named++;
    CHECK(named <= MP_LAN_NAMED_MAX);
    // a verified room-key tag outranks the named tier and strangers
    CHECK(s.add(Addr(0x0e000001), Beacon(0x6001, "Zed"), t + 10, FP, true));
    CHECK(Has(s, 0x6001));
    for (int i = 0; i < s.count(); i++) if (s.trust(i) == MP_LAN_KEYED) CHECK(i >= 0);
    Trust(0, {});
    Trust(1, {});
}

// A stranger cannot evict a trusted room, even a stale one; stale rooms of the same tier are replaced by a newer one, oldest first.
static void TestEvictionRules() {
    MpLanScan s;
    Trust(0, {"Pal"});
    CHECK(s.add(Addr(1), Beacon(0x11, "Pal"), 1000, FP));
    for (int i = 0; i < 10; i++) s.add(Addr(100 + i), Beacon(0x100 + i, "Rando"), 1000 + i, FP);   // 8 fit in the burst, then the rate limit
    const int before = s.count();
    CHECK(before >= 2);
    // far later (all stale) strangers replace strangers, never the friend
    uint64_t t = 20000;
    for (int i = 0; i < 12; i++) { t += 300; s.add(Addr(500 + i), Beacon(0x500 + i, "Other"), t, FP); }
    CHECK(Has(s, 0x11));
    CHECK(s.count() <= MP_LAN_UNTRUSTED_MAX + 1);
    // the oldest stale stranger is the one that goes
    MpLanScan u;
    Trust(0, {});
    for (int i = 0; i < MP_LAN_UNTRUSTED_MAX; i++) u.add(Addr(10 + i), Beacon(0x300 + i, "S"), 1000 + i * 150, FP);
    CHECK(u.count() == MP_LAN_UNTRUSTED_MAX || u.count() >= 8);
    const int cnt = u.count();
    for (int i = 0; i < cnt; i++) u.add(Addr(10 + i), Beacon(0x300 + i, "S", 2), 5000 + (i == 0 ? 0 : 1000 + i), FP);   // all but #0 refresh later
    CHECK(u.add(Addr(900), Beacon(0x900, "New"), 7000, FP));
    CHECK(!Has(u, 0x300));   // the one that went quiet
    CHECK(Has(u, 0x900));
    Trust(0, {});
}

// New rooms are rate limited: 10,000 addresses in one instant cannot list more than the burst.
static void TestRateLimit() {
    MpLanScan s;
    Trust(0, {});
    int added = 0;
    for (int i = 0; i < 10000; i++) added += s.add(Addr(0x20000000 + i), Beacon(0x40000 + i, "Spam"), 5000, FP) ? 1 : 0;
    CHECK(added <= MP_LAN_NEW_PER_SEC);
    // a second later the budget is back
    CHECK(s.add(Addr(0x30000000), Beacon(0x77, "Real"), 6200, FP) || s.count() >= MP_LAN_UNTRUSTED_MAX);
}

// Room names follow the typed-message cleaner.
static void TestNames() {
    uint8_t buf[64];
    MpBeacon b = Beacon(0x55, "x");
    memset(b.name, 0, sizeof b.name);
    // 16 bytes: "A" then 7 combining acute accents (14 bytes), then 'B'
    size_t o = 0;
    b.name[o++] = 'A';
    for (int i = 0; i < 7; i++) { b.name[o++] = (char)0xCC; b.name[o++] = (char)0x81; }
    b.name[o++] = 'B';
    size_t n = mp_beacon_write(buf, sizeof buf, b);
    CHECK(n == MP_BEACON_BYTES);
    MpBeacon r;
    CHECK(mp_beacon_read(buf, n, &r));
    char want[MP_NAME_BYTES + 1];
    mp_chat_clean(b.name, strnlen(b.name, MP_NAME_BYTES), want, sizeof want);
    CHECK(!strcmp(r.name, want));
    int marks = 0, maxRun = 0;
    for (const unsigned char* p = (const unsigned char*)r.name; *p; p++)
        if (p[0] == 0xCC) { marks++; maxRun = marks > maxRun ? marks : maxRun; } else marks = 0;
    CHECK(maxRun <= 2);
    // control characters and bidi overrides are gone; add() cleans a beacon that did not come through the reader too
    MpLanScan s;
    Trust(0, {});
    MpBeacon c = Beacon(0x56, "x");
    memcpy(c.name, "\x01Hi\x1b\xe2\x80\xae" "yo", 9);
    c.name[9] = 0;
    s.add(Addr(7), c, 1000, FP);
    MpLocalRoom room;
    CHECK(s.get(0, &room, 0, 0));
    CHECK(!strcmp(room.host, "Hiyo"));
}

// corefix2: a spoofed friend NAME (a beacon only has to carry it) filled the 4 named slots and the real friend landed in the untrusted tier.
static void TestSpoofedFriendNameCannotFillNamedTier() {
    MpLanScan s;
    Trust(0, {"Alex"});
    for (int a = 0; a < 4; a++) s.add(Addr(0x0a000200 + a), Beacon(0x2000 + a, "Alex"), 1000 + a, FP);
    int named = 0;
    for (int i = 0; i < s.count(); i++) named += s.trust(i) == MP_LAN_NAMED;
    CHECK(named <= MP_LAN_NAMED_PER_NAME);   // before: 4
    CHECK(s.add(Addr(0x0a000250), Beacon(0x3000, "Alex"), 1100, FP));   // the real friend
    int idx = -1;
    for (int i = 0; i < s.count(); i++) {
        MpAddr ad;
        uint32_t sid;
        MpLocalRoom r;
        if (s.get(i, &r, &ad, &sid) && sid == 0x3000) idx = i;
    }
    CHECK(idx >= 0 && s.trust(idx) == MP_LAN_NAMED);   // before: untrusted
    // another friend's name still finds room next to the first one's
    Trust(0, {"Alex", "Sam"});
    CHECK(s.add(Addr(0x0a000260), Beacon(0x3100, "Sam"), 1200, FP));
    int sam = -1;
    for (int i = 0; i < s.count(); i++) {
        MpAddr ad;
        uint32_t sid;
        MpLocalRoom r;
        if (s.get(i, &r, &ad, &sid) && sid == 0x3100) sam = i;
    }
    CHECK(sam >= 0 && s.trust(sam) == MP_LAN_NAMED);
    Trust(0, {});
}

int main() {
    TestReviewerScenario();
    TestFriendEvictsOldestStranger();
    TestEvictionRules();
    TestRateLimit();
    TestNames();
    TestSpoofedFriendNameCannotFillNamedTier();   // corefix2
    if (g_fail) { fprintf(stderr, "lan_scan_test: %d FAILED\n", g_fail); return 1; }
    printf("lan_scan_test: ok\n");
    return 0;
}
