// Galaxy 3.0 multiplayer: "lan_flood", an adversarial host test of the Same Wi-Fi room list (lanfix job C).
// Anyone on the same Wi-Fi (or in Nearby radio range) can send the discovery port anything. This runs the outside review's scenario (S2)
// and worse against MpLanScan + mp_beacon_read: beacons of every length 0..128 with random content, 10,000 distinct source addresses,
// and a friend's real beacon arriving last. Asserted: the list stays within its 16 rooms, a beacon costs bounded time, nothing is
// allocated while beacons are processed (counted operator new), and the friend's room is visible.
#include "mp_lan.h"
#include "mp_config.h"
#include "port/mp_chat.h"

#include <chrono>
#include <initializer_list>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// every heap allocation through operator new is counted (the scan itself is a fixed array; the readers use the stack)
static unsigned long g_news = 0;
void* operator new(size_t n) { g_news++; void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void* operator new[](size_t n) { g_news++; void* p = malloc(n ? n : 1); if (!p) abort(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static const uint32_t FP = 0x1234;
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t Rnd() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }

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
static void Friends(std::initializer_list<const char*> names) {
    char arr[MP_LAN_TRUSTED_PER_SOURCE][MP_NAME_BYTES + 1];
    int n = 0;
    for (const char* nm : names) { snprintf(arr[n], sizeof arr[n], "%s", nm); n++; }
    mp_lan_set_trusted_names(1, arr, n);
}
static uint64_t NowNs() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void CheckListSane(const MpLanScan& s) {
    CHECK(s.count() >= 0 && s.count() <= MP_LAN_MAX_ROOMS);
    int untrusted = 0;
    for (int i = 0; i < s.count(); i++) {
        MpLocalRoom r;
        CHECK(s.get(i, &r, 0, 0));
        CHECK(strnlen(r.host, sizeof r.host) <= MP_NAME_BYTES);
        if (s.trust(i) == MP_LAN_UNTRUSTED) untrusted++;
    }
    CHECK(untrusted <= MP_LAN_UNTRUSTED_MAX);
}

// 1. the reviewer's scenario (lanfill.cpp): 8 addresses x 2 sessions, then the legitimate host; the host is a friend
static void TestReviewerFill() {
    MpLanScan s;
    Friends({"Alex"});
    for (int a = 0; a < 8; a++)
        for (int k = 0; k < 2; k++) s.add(Addr(0x0a000100 + a), Beacon(0x1000 + a * 2 + k, "Free Stars"), 1000, FP);
    CHECK(s.add(Addr(0x0a000199), Beacon(0x9999, "Alex"), 1100, FP));
    CHECK(Has(s, 0x9999));
    CheckListSane(s);
    Friends({});
}

// 2. every length 0..128, random content (and random content behind a real header), through the reader and the list
static void TestEveryLength() {
    MpLanScan s;
    Friends({});
    uint64_t t = 1000;
    unsigned accepted = 0;
    const unsigned long newsBefore = g_news;
    for (int len = 0; len <= 128; len++) {
        for (int rep = 0; rep < 200; rep++) {
            uint8_t buf[256];
            for (int i = 0; i < len; i++) buf[i] = (uint8_t)Rnd();
            if (rep & 1 && len >= 4) memcpy(buf, "SGB1", 4);           // a real magic: the field checks run
            if (rep & 2 && len >= 8) { buf[5] = 1 + Rnd() % 3; buf[6] = 1 + Rnd() % 2; buf[7] = 2 + Rnd() % 3; }   // plausible mode / players / max
            MpBeacon b;
            memset(&b, 0xAA, sizeof b);
            if (mp_beacon_read(buf, (size_t)len, &b)) {
                accepted++;
                CHECK(len == 40 || len == 44 || len == MP_BEACON_BYTES);
                CHECK(strnlen(b.name, sizeof b.name) <= MP_NAME_BYTES);
                t += 7;
                s.add(Addr(Rnd()), b, t, FP);
            }
            mp_probe_read(buf, (size_t)len);
        }
        CheckListSane(s);
    }
    CHECK(accepted > 0);   // (the test must reach the list, not only the reader's refusals)
    CHECK(g_news == newsBefore);
}

// 3. 10,000 distinct source addresses in one burst and then spread over time, bounded time per beacon, a friend's beacon last
static void TestManyAddresses() {
    MpLanScan s;
    Friends({"Anna"});
    const unsigned long newsBefore = g_news;
    uint64_t worst = 0, total = 0;
    uint64_t t = 5000;
    for (int i = 0; i < 10000; i++) {
        if (i >= 5000) t += 3;   // the second half is spread out (30 s of strangers), the first half is one instant
        uint8_t buf[MP_BEACON_BYTES];
        MpBeacon b = Beacon(0x100000 + i, (i % 5) ? "Free Stars" : "A\xcc\x81\xcc\x81\xcc\x81\xcc\x81\xcc\x81 b", 1 + i);
        const size_t n = mp_beacon_write(buf, sizeof buf, b);
        const uint64_t a0 = NowNs();
        MpBeacon r;
        if (mp_beacon_read(buf, n, &r)) s.add(Addr(0x0b000000 + (uint32_t)i), r, t, FP);
        const uint64_t dt = NowNs() - a0;
        total += dt;
        if (dt > worst) worst = dt;
        if ((i % 997) == 0) CheckListSane(s);
    }
    CheckListSane(s);
    // the friend's real beacon arrives last
    const uint64_t a0 = NowNs();
    const bool added = s.add(Addr(0x0c0000aa), Beacon(0x7777, "Anna"), t + 1, FP);
    const uint64_t dt = NowNs() - a0;
    CHECK(added);
    CHECK(Has(s, 0x7777));
    CHECK(dt < 5000000ull);          // 5 ms under ASan: a beacon never costs more than a few comparisons
    CHECK(worst < 20000000ull);      // 20 ms worst case under ASan and a loaded machine (it is microseconds)
    CHECK(total < 2000000000ull);    // 10,000 beacons in well under 2 s even sanitised
    CHECK(g_news == newsBefore);     // allocation-free
    // the friend's room survives the strangers' churn afterwards (its own beacons keep it fresh; strangers only ever replace strangers)
    for (int i = 0; i < 3000; i++) {
        t += 10;
        s.add(Addr(0x0d000000 + (uint32_t)i), Beacon(0x200000 + i, "Spam"), t, FP);
        if (i % 40 == 0) s.add(Addr(0x0c0000aa), Beacon(0x7777, "Anna", 2 + i), t, FP);
    }
    CHECK(Has(s, 0x7777));
    CheckListSane(s);
    Friends({});
}

// 4. the friend's room arrives while the list is full of fresh strangers and each stranger keeps beaconing
static void TestFriendAgainstLiveStrangers() {
    MpLanScan s;
    Friends({"Dana"});
    uint64_t t = 1000;
    uint32_t seq = 1;
    for (int round = 0; round < 40; round++) {
        t += 250;
        for (int i = 0; i < 40; i++) s.add(Addr(0x0e000000 + (uint32_t)i), Beacon(0x300000 + i, "Stranger", ++seq), t, FP);
    }
    CHECK(s.count() <= MP_LAN_UNTRUSTED_MAX);
    CHECK(s.add(Addr(0x0f000001), Beacon(0x8888, "Dana"), t + 1, FP));
    CHECK(Has(s, 0x8888));
    Friends({});
}

int main() {
    TestReviewerFill();
    TestEveryLength();
    TestManyAddresses();
    TestFriendAgainstLiveStrangers();
    if (g_fail) { fprintf(stderr, "lan_flood: %d FAILED\n", g_fail); return 1; }
    printf("lan_flood: ok\n");
    return 0;
}
