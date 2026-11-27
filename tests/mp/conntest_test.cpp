// Host tests for the Connection Test engine (owner: conntest): see conntest_build.sh.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mp_conntest.h"

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------------------------------------------------------------- helpers
static void TestHelpers() {
    CHECK(mp_ct_ip_not_public(0x0A000001) && mp_ct_ip_not_public(0xC0A80001) && mp_ct_ip_not_public(0xAC100001) && mp_ct_ip_not_public(0xAC1F0001));
    CHECK(!mp_ct_ip_not_public(0xAC200001) && !mp_ct_ip_not_public(0xAC0F0001));      // 172.32 and 172.15 are public
    CHECK(mp_ct_ip_not_public(0x64400001) && mp_ct_ip_not_public(0x647F0001));        // 100.64 .. 100.127: carrier-grade NAT
    CHECK(!mp_ct_ip_not_public(0x64800001) && !mp_ct_ip_not_public(0x633F0001));      // 100.128 and 100.63 are public
    CHECK(mp_ct_ip_not_public(0x7F000001) && mp_ct_ip_not_public(0xA9FE0001) && mp_ct_ip_not_public(0));
    CHECK(!mp_ct_ip_not_public(0xCB007107) && !mp_ct_ip_not_public(0x08080808));

    MpCtWifi w = {true, 80};
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_GOOD);
    w.quality = MP_CT_WEAK_PERCENT - 1;
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_WEAK);
    w.quality = MP_CT_WEAK_PERCENT;
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_GOOD);
    w.quality = -1;
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_UNKNOWN);
    w.quality = 400;   // not a measurement
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_UNKNOWN);
    w.quality = 0;
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_WEAK);
    w.connected = false, w.quality = 90;
    CHECK(mp_ct_wifi_result(w) == MPCT_R_WIFI_NONE);

    CHECK(mp_ct_delay_result(0) == MPCT_R_DELAY_FAST && mp_ct_delay_result(MP_CT_FAST_MS - 1) == MPCT_R_DELAY_FAST);
    CHECK(mp_ct_delay_result(MP_CT_FAST_MS) == MPCT_R_DELAY_OK && mp_ct_delay_result(MP_CT_SLOW_MS - 1) == MPCT_R_DELAY_OK);
    CHECK(mp_ct_delay_result(MP_CT_SLOW_MS) == MPCT_R_DELAY_SLOW && mp_ct_delay_result(5000) == MPCT_R_DELAY_SLOW);
    CHECK(mp_ct_delay_result(-5) == MPCT_R_NONE && mp_ct_delay_result(1000000000) == MPCT_R_NONE && mp_ct_delay_result(-1) == MPCT_R_NONE);

    CHECK(mp_ct_version_older("3.0.0", "3.1.0") && mp_ct_version_older("1.2.0", "1.10.0") && mp_ct_version_older("2", "3.0"));
    CHECK(!mp_ct_version_older("3.0.0", "3.0.0") && !mp_ct_version_older("3.1", "3.0.9") && !mp_ct_version_older("3", "3.0.0"));
    CHECK(!mp_ct_version_older("", "3.0") && !mp_ct_version_older("3.0", "") && !mp_ct_version_older(nullptr, "3.0") && !mp_ct_version_older("3.0", nullptr));
    CHECK(!mp_ct_version_older("3.x", "4.0") && !mp_ct_version_older("3.0", "4..0") && !mp_ct_version_older("1.2.3.4.5", "9.9") &&
          !mp_ct_version_older("99999999999.0", "99999999999.1") && !mp_ct_version_older("3.0", "4.0 "));
}

// ---------------------------------------------------------------- hosting from the router and STUN
static void TestHostResult() {
    const uint32_t pub = 0xCB007107, pub2 = 0xCB007108, cg = 0x64400005, lan = 0xC0A80001;
    struct Case { bool mapAsked; MpCtMap map; bool stunAsked; int stunState; uint32_t stunIp; uint8_t want; const char* what; };
    const Case cases[] = {
        {true, {MPCT_MAP_MAPPED, false, pub}, true, 1, pub, MPCT_R_HOST_OPEN, "mapped, STUN agrees"},
        {true, {MPCT_MAP_MAPPED, false, pub}, false, 0, 0, MPCT_R_HOST_OPEN, "mapped, STUN off (stun = 0)"},
        {true, {MPCT_MAP_MAPPED, false, pub}, true, -1, 0, MPCT_R_HOST_OPEN, "mapped, STUN silent"},
        {true, {MPCT_MAP_MAPPED, false, 0}, true, 1, pub, MPCT_R_HOST_OPEN, "mapped, router did not say its address"},
        {true, {MPCT_MAP_MAPPED, true, cg}, true, 1, cg, MPCT_R_HOST_CGNAT, "mapped by a router behind carrier NAT"},
        {true, {MPCT_MAP_MAPPED, false, pub}, true, 1, pub2, MPCT_R_HOST_CGNAT, "router address differs from what the internet sees"},
        {true, {MPCT_MAP_MAPPED, false, pub}, true, 1, cg, MPCT_R_HOST_CGNAT, "STUN sees carrier range"},
        {true, {MPCT_MAP_MAPPED, false, lan}, false, 0, 0, MPCT_R_HOST_CGNAT, "router's WAN address is private"},
        {true, {MPCT_MAP_FAILED, false, pub}, true, 1, pub, MPCT_R_HOST_MAYBE, "no mapping, public address (forward by hand)"},
        {true, {MPCT_MAP_FAILED, false, 0}, true, 1, pub, MPCT_R_HOST_MAYBE, "no mapping, STUN public"},
        {true, {MPCT_MAP_FAILED, true, cg}, true, 1, pub, MPCT_R_HOST_CGNAT, "no mapping, router WAN carrier"},
        {true, {MPCT_MAP_FAILED, false, 0}, true, 1, cg, MPCT_R_HOST_CGNAT, "no mapping, STUN carrier"},
        {true, {MPCT_MAP_FAILED, false, 0}, true, -1, 0, MPCT_R_HOST_UNKNOWN, "no mapping, STUN silent"},
        {true, {MPCT_MAP_FAILED, false, 0}, false, 0, 0, MPCT_R_HOST_UNKNOWN, "no mapping, STUN off: say less"},
        {true, {MPCT_MAP_WORKING, false, 0}, false, 0, 0, MPCT_R_HOST_UNKNOWN, "router still asking when time ran out"},
        {false, {0, false, 0}, true, 1, pub, MPCT_R_HOST_MAYBE, "mapping off, STUN public"},
        {false, {0, false, 0}, true, 1, cg, MPCT_R_HOST_CGNAT, "mapping off, STUN carrier"},
        {false, {0, false, 0}, false, 0, 0, MPCT_R_HOST_UNKNOWN, "both off"},
        {true, {MPCT_MAP_MAPPED, false, pub}, true, 1, 0, MPCT_R_HOST_OPEN, "STUN answered with address 0: ignored"},
    };
    for (const Case& c : cases) {
        const uint8_t got = mp_ct_host_result(c.mapAsked, c.map, c.stunAsked, c.stunState, c.stunIp);
        CHECKF(got == c.want, "%s: got %d want %d", c.what, got, c.want);
    }
}

// ---------------------------------------------------------------- the verdict
enum { W, L, N, H, D };
static const char* kHeadlineName[MPCT_H_COUNT] = {"none", "all_good", "join_only", "online_only", "slow", "wifi_weak", "no_wifi", "no_internet", "not_set_up", "update", "paused"};
static const char* kLineName[MPCT_L_COUNT] = {"none", "host_and_join", "host_maybe", "host_cgnat", "let_friend_host", "move_closer", "slow_lag", "try_local", "local_still",
                                              "nearby_still", "connect_wifi", "check_wifi", "not_set_up", "update", "try_later", "local_blocked"};

static void Dump(const uint8_t r[MPCT_CHECKS], const MpCtVerdict& v) {
    fprintf(stderr, "  results w=%d l=%d n=%d h=%d d=%d -> %s:", r[W], r[L], r[N], r[H], r[D], kHeadlineName[v.headline]);
    for (int i = 0; i < v.lineCount; i++) fprintf(stderr, " %s", kLineName[v.lines[i]]);
    fputc('\n', stderr);
}

struct VCase {
    const char* what;
    uint8_t w, l, n, h, d;
    uint8_t headline;
    uint8_t lines[3];
    bool weak;
};
static void TestVerdictTable() {
    const uint8_t G = MPCT_R_WIFI_GOOD, WK = MPCT_R_WIFI_WEAK, NW = MPCT_R_WIFI_NONE, UW = MPCT_R_WIFI_UNKNOWN;
    const uint8_t LF = MPCT_R_LOCAL_FOUND, LO = MPCT_R_LOCAL_OK, LB = MPCT_R_LOCAL_BLOCKED, SK = MPCT_R_SKIPPED;
    const uint8_t NO = MPCT_R_NET_OK, ND = MPCT_R_NET_DOWN, NS = MPCT_R_NET_NOT_SET_UP, NU = MPCT_R_NET_UPDATE, NF = MPCT_R_NET_OFF;
    const uint8_t HO = MPCT_R_HOST_OPEN, HM = MPCT_R_HOST_MAYBE, HC = MPCT_R_HOST_CGNAT, HU = MPCT_R_HOST_UNKNOWN;
    const uint8_t DF = MPCT_R_DELAY_FAST, DK = MPCT_R_DELAY_OK, DS = MPCT_R_DELAY_SLOW;
    const VCase cases[] = {
        {"everything good", G, LO, NO, HO, DF, MPCT_H_ALL_GOOD, {MPCT_L_HOST_AND_JOIN}, false},
        {"everything good, a room found", G, LF, NO, HO, DK, MPCT_H_ALL_GOOD, {MPCT_L_HOST_AND_JOIN}, false},
        {"desktop: Wi-Fi strength unknown", UW, LO, NO, HO, DF, MPCT_H_ALL_GOOD, {MPCT_L_HOST_AND_JOIN}, false},
        {"in a room as a guest: hosting skipped", G, SK, NO, SK, DF, MPCT_H_ALL_GOOD, {}, false},
        {"hosting may not work", G, LO, NO, HM, DF, MPCT_H_JOIN_ONLY, {MPCT_L_HOST_MAYBE, MPCT_L_LET_FRIEND_HOST}, false},
        {"hosting unknown (STUN off, no port)", G, LO, NO, HU, DK, MPCT_H_JOIN_ONLY, {MPCT_L_HOST_MAYBE, MPCT_L_LET_FRIEND_HOST}, false},
        {"carrier NAT", G, LO, NO, HC, DF, MPCT_H_JOIN_ONLY, {MPCT_L_HOST_CGNAT, MPCT_L_LET_FRIEND_HOST}, false},
        {"hosting maybe and Same Wi-Fi blocked", G, LB, NO, HM, DF, MPCT_H_JOIN_ONLY, {MPCT_L_HOST_MAYBE, MPCT_L_LET_FRIEND_HOST, MPCT_L_LOCAL_BLOCKED}, false},
        {"Same Wi-Fi blocked, the rest good", G, LB, NO, HO, DF, MPCT_H_ONLINE_ONLY, {MPCT_L_HOST_AND_JOIN, MPCT_L_LOCAL_BLOCKED}, false},
        {"slow, host fine", G, LO, NO, HO, DS, MPCT_H_SLOW, {MPCT_L_SLOW_LAG, MPCT_L_TRY_LOCAL}, true},
        {"slow, cannot host", G, LO, NO, HC, DS, MPCT_H_SLOW, {MPCT_L_SLOW_LAG, MPCT_L_LET_FRIEND_HOST, MPCT_L_TRY_LOCAL}, true},
        {"slow, Same Wi-Fi blocked", G, LB, NO, HO, DS, MPCT_H_SLOW, {MPCT_L_SLOW_LAG, MPCT_L_NEARBY_STILL}, true},
        {"Wi-Fi weak", WK, LO, NO, HO, DF, MPCT_H_WIFI_WEAK, {MPCT_L_MOVE_CLOSER}, true},
        {"Wi-Fi weak and cannot host", WK, LO, NO, HM, DK, MPCT_H_WIFI_WEAK, {MPCT_L_MOVE_CLOSER, MPCT_L_HOST_MAYBE, MPCT_L_LET_FRIEND_HOST}, true},
        {"Wi-Fi weak and no internet", WK, LO, ND, SK, SK, MPCT_H_WIFI_WEAK, {MPCT_L_MOVE_CLOSER, MPCT_L_LOCAL_STILL}, true},
        {"no Wi-Fi", NW, SK, SK, SK, SK, MPCT_H_NO_WIFI, {MPCT_L_CONNECT_WIFI, MPCT_L_NEARBY_STILL}, false},
        {"no internet", G, LO, ND, SK, SK, MPCT_H_NO_INTERNET, {MPCT_L_CHECK_WIFI, MPCT_L_LOCAL_STILL}, false},
        {"no internet, Same Wi-Fi blocked too", G, LB, ND, SK, SK, MPCT_H_NO_INTERNET, {MPCT_L_CHECK_WIFI, MPCT_L_NEARBY_STILL}, false},
        {"service not set up", G, LO, NS, SK, SK, MPCT_H_NOT_SET_UP, {MPCT_L_NOT_SET_UP, MPCT_L_LOCAL_STILL}, false},
        {"service wants an update", G, LO, NU, SK, SK, MPCT_H_UPDATE, {MPCT_L_UPDATE, MPCT_L_LOCAL_STILL}, false},
        {"service switched off", G, LO, NF, SK, SK, MPCT_H_PAUSED, {MPCT_L_TRY_LATER, MPCT_L_LOCAL_STILL}, false},
        {"a run that never reached the internet check", G, MPCT_R_NONE, MPCT_R_NONE, MPCT_R_NONE, MPCT_R_NONE, MPCT_H_NO_INTERNET, {MPCT_L_CHECK_WIFI, MPCT_L_LOCAL_STILL}, false},
        {"delay skipped (no sample)", G, LO, NO, HO, SK, MPCT_H_ALL_GOOD, {MPCT_L_HOST_AND_JOIN}, false},
    };
    for (const VCase& c : cases) {
        const uint8_t r[MPCT_CHECKS] = {c.w, c.l, c.n, c.h, c.d};
        const MpCtVerdict v = mp_ct_verdict(r);
        int want = 0;
        while (want < 3 && c.lines[want]) ++want;
        bool ok = v.headline == c.headline && v.lineCount == want && v.weakLink == c.weak && v.good == (c.headline == MPCT_H_ALL_GOOD);
        for (int i = 0; ok && i < want; i++) ok = v.lines[i] == c.lines[i];
        CHECKF(ok, "%s", c.what);
        if (!ok) Dump(r, v);
    }
}

// every combination of results the checks can produce: one headline, <= 3 distinct lines, and the promises each headline makes
static void TestVerdictExhaustive() {
    const uint8_t wifi[] = {MPCT_R_NONE, MPCT_R_SKIPPED, MPCT_R_WIFI_GOOD, MPCT_R_WIFI_WEAK, MPCT_R_WIFI_NONE, MPCT_R_WIFI_UNKNOWN};
    const uint8_t local[] = {MPCT_R_NONE, MPCT_R_SKIPPED, MPCT_R_LOCAL_FOUND, MPCT_R_LOCAL_OK, MPCT_R_LOCAL_BLOCKED};
    const uint8_t net[] = {MPCT_R_NONE, MPCT_R_SKIPPED, MPCT_R_NET_OK, MPCT_R_NET_DOWN, MPCT_R_NET_NOT_SET_UP, MPCT_R_NET_UPDATE, MPCT_R_NET_OFF};
    const uint8_t host[] = {MPCT_R_NONE, MPCT_R_SKIPPED, MPCT_R_HOST_OPEN, MPCT_R_HOST_MAYBE, MPCT_R_HOST_CGNAT, MPCT_R_HOST_UNKNOWN};
    const uint8_t delay[] = {MPCT_R_NONE, MPCT_R_SKIPPED, MPCT_R_DELAY_FAST, MPCT_R_DELAY_OK, MPCT_R_DELAY_SLOW};
    int combos = 0, seenHeadline[MPCT_H_COUNT] = {0}, seenLine[MPCT_L_COUNT] = {0};
    for (uint8_t w : wifi) for (uint8_t l : local) for (uint8_t n : net) for (uint8_t h : host) for (uint8_t d : delay) {
        const uint8_t r[MPCT_CHECKS] = {w, l, n, h, d};
        const MpCtVerdict v = mp_ct_verdict(r);
        ++combos;
        bool ok = v.headline > MPCT_H_NONE && v.headline < MPCT_H_COUNT && v.lineCount <= MPCT_MAX_LINES;
        for (int i = 0; ok && i < v.lineCount; i++) {
            ok = v.lines[i] > MPCT_L_NONE && v.lines[i] < MPCT_L_COUNT;
            for (int k = 0; ok && k < i; k++) ok = v.lines[k] != v.lines[i];
        }
        // the promises
        const bool wifiOk = w != MPCT_R_WIFI_WEAK && w != MPCT_R_WIFI_NONE;
        const bool online = wifiOk && n == MPCT_R_NET_OK;
        if (v.headline == MPCT_H_ALL_GOOD) {
            ok = ok && online && d != MPCT_R_DELAY_SLOW && h != MPCT_R_HOST_MAYBE && h != MPCT_R_HOST_CGNAT && h != MPCT_R_HOST_UNKNOWN && l != MPCT_R_LOCAL_BLOCKED && v.good;
        } else {
            ok = ok && !v.good;
        }
        // "you can host" is only ever said when the router really opened a port, and never with a bad Wi-Fi or a down service
        for (int i = 0; i < v.lineCount; i++)
            if (v.lines[i] == MPCT_L_HOST_AND_JOIN) ok = ok && online && h == MPCT_R_HOST_OPEN && d != MPCT_R_DELAY_SLOW;
        // every online headline needs the service: without it nothing says online play works
        if (v.headline == MPCT_H_JOIN_ONLY || v.headline == MPCT_H_ONLINE_ONLY || v.headline == MPCT_H_SLOW) ok = ok && online;
        // a bad Wi-Fi always wins the headline, no Wi-Fi first
        if (w == MPCT_R_WIFI_NONE) ok = ok && v.headline == MPCT_H_NO_WIFI;
        else if (w == MPCT_R_WIFI_WEAK) ok = ok && v.headline == MPCT_H_WIFI_WEAK;
        // a Wi-Fi tip only when the Wi-Fi or the delay is bad
        ok = ok && v.weakLink == (w == MPCT_R_WIFI_WEAK || d == MPCT_R_DELAY_SLOW);
        // a host problem is always told (a headline that is not about something worse says "may not work" / "provider blocks")
        if (online && d != MPCT_R_DELAY_SLOW && (h == MPCT_R_HOST_MAYBE || h == MPCT_R_HOST_UNKNOWN || h == MPCT_R_HOST_CGNAT)) {
            bool told = false;
            for (int i = 0; i < v.lineCount; i++) told = told || v.lines[i] == MPCT_L_HOST_MAYBE || v.lines[i] == MPCT_L_HOST_CGNAT;
            ok = ok && told && v.headline == MPCT_H_JOIN_ONLY;
        }
        // offline answers always say what still works (Same Wi-Fi / Nearby) unless the Wi-Fi is the whole problem
        if (n != MPCT_R_NET_OK && wifiOk) {
            bool still = false;
            for (int i = 0; i < v.lineCount; i++) still = still || v.lines[i] == MPCT_L_LOCAL_STILL || v.lines[i] == MPCT_L_NEARBY_STILL;
            ok = ok && still;
        }
        CHECKF(ok, "combo %d", combos);
        if (!ok) Dump(r, v);
        seenHeadline[v.headline]++;
        for (int i = 0; i < v.lineCount; i++) seenLine[v.lines[i]]++;
    }
    for (int h = 1; h < MPCT_H_COUNT; h++) CHECKF(seenHeadline[h] > 0, "headline %s is never produced", kHeadlineName[h]);
    for (int l = 1; l < MPCT_L_COUNT; l++) CHECKF(seenLine[l] > 0, "line %s is never produced", kLineName[l]);
    CHECK(combos == 6 * 5 * 7 * 6 * 5);
}

// ---------------------------------------------------------------- the run through fake probes
struct Fake {
    uint64_t now = 1000;
    // wifi
    bool wifiProbe = true;
    MpCtWifi wifi = {true, 80};
    // local
    int localAnswer = 0;        // what local() returns; kLocalAfterMs: from that many ms after the step began
    int localLater = 0;
    uint64_t localAfter = 0;
    int localCalls = 0;
    // service
    bool configured = true;
    bool infoStarts = true;
    uint64_t infoLatency = 120;          // ms until the answer
    bool infoNever = false;              // never answers
    int infoOutcome = MPCT_INFO_OK;
    int infoMs = 40;
    int infoCalls = 0, infoCancels = 0;
    uint32_t lastInfoTimeout = 0;
    bool infoPending = false;
    uint64_t infoDue = 0;
    // map
    int mapBegin = 1;
    MpCtMap map = {MPCT_MAP_MAPPED, false, 0xCB007107};
    uint64_t mapLatency = 300;
    uint64_t mapAskedAt = 0;
    bool mapNever = false;
    int mapBegins = 0, mapEnds = 0;
    // stun
    int stunBegin = 1;
    int stunAnswer = 1;
    uint32_t stunIp = 0xCB007107;
    uint64_t stunLatency = 200;
    uint64_t stunAskedAt = 0;
    int stunBegins = 0, stunEnds = 0;
    // room
    int hostRtt = -1;
    int busyCalls = 0;
    // for the info calls' sample sequence
    int sampleMs[8] = {40, 40, 40, 40, 40, 40, 40, 40};
};

static uint64_t FNow(void* c) { return static_cast<Fake*>(c)->now; }
static bool FWifi(void* c, MpCtWifi* o) { Fake* f = static_cast<Fake*>(c); *o = f->wifi; return f->wifiProbe; }
static int FLocal(void* c) {
    Fake* f = static_cast<Fake*>(c);
    ++f->localCalls;
    if (f->localLater && f->now >= f->localAfter) return f->localLater;
    return f->localAnswer;
}
static bool FConfigured(void* c) { return static_cast<Fake*>(c)->configured; }
static bool FInfoBegin(void* c, uint32_t t) {
    Fake* f = static_cast<Fake*>(c);
    if (!f->infoStarts) return false;
    f->lastInfoTimeout = t;
    f->infoPending = true;
    f->infoDue = f->now + f->infoLatency;
    ++f->infoCalls;
    return true;
}
static int FInfoPoll(void* c, MpCtInfo* o) {
    Fake* f = static_cast<Fake*>(c);
    if (!f->infoPending) return 1;   // (a poll with nothing running)
    if (f->infoNever) return 0;
    if (f->now < f->infoDue) return 0;
    f->infoPending = false;
    o->outcome = static_cast<uint8_t>(f->infoOutcome);
    const int idx = f->infoCalls - 1;
    o->ms = f->sampleMs[idx < 8 ? idx : 7] + (f->infoMs - 40);
    return 1;
}
static void FInfoCancel(void* c) { Fake* f = static_cast<Fake*>(c); f->infoPending = false; ++f->infoCancels; }
static int FMapBegin(void* c) { Fake* f = static_cast<Fake*>(c); ++f->mapBegins; f->mapAskedAt = f->now; return f->mapBegin; }
static void FMapPoll(void* c, MpCtMap* o) {
    Fake* f = static_cast<Fake*>(c);
    *o = f->map;
    if (f->mapNever || f->now < f->mapAskedAt + f->mapLatency) *o = MpCtMap{MPCT_MAP_WORKING, false, 0};   // (still asking: nothing known yet)
}
static void FMapEnd(void* c) { ++static_cast<Fake*>(c)->mapEnds; }
static int FStunBegin(void* c) { Fake* f = static_cast<Fake*>(c); ++f->stunBegins; f->stunAskedAt = f->now; return f->stunBegin; }
static int FStunPoll(void* c, uint32_t* ip) {
    Fake* f = static_cast<Fake*>(c);
    if (f->now < f->stunAskedAt + f->stunLatency) return 0;
    if (f->stunAnswer == 1) *ip = f->stunIp;
    return f->stunAnswer;
}
static void FStunEnd(void* c) { ++static_cast<Fake*>(c)->stunEnds; }
static int FHostRtt(void* c) { return static_cast<Fake*>(c)->hostRtt; }
static void FBusy(void* c) { ++static_cast<Fake*>(c)->busyCalls; }

static MpCtProbes Probes(Fake* f) {
    MpCtProbes p;
    memset(&p, 0, sizeof p);
    p.ctx = f;
    p.now = FNow, p.wifi = FWifi, p.local = FLocal, p.serviceConfigured = FConfigured;
    p.infoBegin = FInfoBegin, p.infoPoll = FInfoPoll, p.infoCancel = FInfoCancel;
    p.mapBegin = FMapBegin, p.mapPoll = FMapPoll, p.mapEnd = FMapEnd;
    p.stunBegin = FStunBegin, p.stunPoll = FStunPoll, p.stunEnd = FStunEnd;
    p.hostRtt = FHostRtt, p.busy = FBusy;
    return p;
}

// runs to the end in 20 ms frames; returns the elapsed milliseconds since start (and checks the budget)
static uint64_t RunToEnd(MpConnTest& t, Fake& f, uint64_t startAt) {
    int frames = 0;
    while (t.running() && frames < 2000) {
        f.now += 20;
        t.pump();
        ++frames;
    }
    CHECK(!t.running());
    CHECKF(f.now - startAt <= MP_CT_BUDGET_MS + 40, "the run took %llu ms", static_cast<unsigned long long>(f.now - startAt));
    return f.now - startAt;
}

static void TestRunHappy() {
    Fake f;
    MpConnTest t;
    MpCtOptions o = {false, false};
    f.localLater = 2;
    f.localAfter = f.now + 400;
    f.sampleMs[0] = 90;
    t.start(Probes(&f), o);
    CHECK(t.running() && t.view().state[MPCT_WIFI] == MPCT_ST_RUNNING && t.view().state[MPCT_LOCAL] == MPCT_ST_WAITING && !t.view().done);
    // checks report one after another: no check is DONE while an earlier one is not
    int order[MPCT_CHECKS] = {0}, finished = 0;
    uint64_t t0 = f.now;
    int frames = 0;
    while (t.running() && frames++ < 1000) {
        f.now += 20;
        t.pump();
        const MpCtView& v = t.view();
        bool gap = false;
        for (int i = 0; i < MPCT_CHECKS; i++) {
            if (v.state[i] == MPCT_ST_DONE && !order[i]) order[i] = ++finished;
            if (v.state[i] == MPCT_ST_DONE && i > 0 && v.state[i - 1] != MPCT_ST_DONE) gap = true;
            if (v.state[i] == MPCT_ST_RUNNING)   // exactly one runs, everything after waits
                for (int k = i + 1; k < MPCT_CHECKS; k++) gap = gap || v.state[k] != MPCT_ST_WAITING;
        }
        CHECK(!gap);
    }
    const MpCtView& v = t.view();
    CHECK(v.done && !v.running);
    for (int i = 0; i < MPCT_CHECKS; i++) CHECKF(order[i] == i + 1, "check %d finished %dth", i, order[i]);
    CHECK(v.result[MPCT_WIFI] == MPCT_R_WIFI_GOOD && v.detail[MPCT_WIFI] == 80);
    CHECK(v.result[MPCT_LOCAL] == MPCT_R_LOCAL_FOUND && v.detail[MPCT_LOCAL] == 2);
    CHECK(v.result[MPCT_INTERNET] == MPCT_R_NET_OK);
    CHECK(v.result[MPCT_HOSTING] == MPCT_R_HOST_OPEN);
    CHECK(v.result[MPCT_DELAY] == MPCT_R_DELAY_FAST && v.detail[MPCT_DELAY] == 40);   // the best sample, not the first one's handshake
    CHECK(v.verdict.headline == MPCT_H_ALL_GOOD && v.verdict.good && v.verdict.lines[0] == MPCT_L_HOST_AND_JOIN);
    CHECKF(f.now - t0 < 4000, "a good run takes %llu ms", static_cast<unsigned long long>(f.now - t0));
    CHECK(f.mapBegins == 1 && f.mapEnds == 1 && f.stunBegins == 1 && f.stunEnds == 1);   // the router mapping let go after the test
    CHECK(f.infoCalls == 1 + MP_CT_RTT_SAMPLES);
    CHECK(f.busyCalls > 0);
}

static void TestRunNoWifi() {
    Fake f;
    f.wifi = {false, -1};
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    RunToEnd(t, f, f.now);
    const MpCtView& v = t.view();
    CHECK(v.result[MPCT_WIFI] == MPCT_R_WIFI_NONE);
    for (int i = 1; i < MPCT_CHECKS; i++) CHECKF(v.result[i] == MPCT_R_SKIPPED, "check %d: %d", i, v.result[i]);
    CHECK(v.verdict.headline == MPCT_H_NO_WIFI);
    CHECK(f.infoCalls == 0 && f.mapBegins == 0 && f.localCalls == 0);
}

static void TestRunNoServiceAndStunOff() {
    Fake f;
    f.configured = false;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    RunToEnd(t, f, f.now);
    CHECK(t.view().result[MPCT_INTERNET] == MPCT_R_NET_NOT_SET_UP && t.view().result[MPCT_HOSTING] == MPCT_R_SKIPPED && t.view().result[MPCT_DELAY] == MPCT_R_SKIPPED);
    CHECK(t.view().verdict.headline == MPCT_H_NOT_SET_UP && f.infoCalls == 0 && f.mapBegins == 0);

    // stun = 0: asked about nothing; with a router that opened the port the answer is still "open"
    Fake g;
    g.stunBegin = 0;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false});
    RunToEnd(t2, g, g.now);
    CHECK(t2.view().result[MPCT_HOSTING] == MPCT_R_HOST_OPEN && g.stunBegins == 1 && g.stunEnds == 0);
    // ... and with no router mapping either the verdict says less, never "you can host"
    Fake h;
    h.stunBegin = 0;
    h.map = {MPCT_MAP_FAILED, false, 0};
    MpConnTest t3;
    t3.start(Probes(&h), {false, false});
    RunToEnd(t3, h, h.now);
    CHECK(t3.view().result[MPCT_HOSTING] == MPCT_R_HOST_UNKNOWN && t3.view().verdict.headline == MPCT_H_JOIN_ONLY);
    // both off: nothing to wait for
    Fake i;
    i.stunBegin = 0, i.mapBegin = 0;
    MpConnTest t4;
    t4.start(Probes(&i), {false, false});
    RunToEnd(t4, i, i.now);
    CHECK(t4.view().result[MPCT_HOSTING] == MPCT_R_HOST_UNKNOWN && i.mapEnds == 0);
}

static void TestRunInternetProblems() {
    struct C { int outcome; uint8_t result; uint8_t headline; } cases[] = {
        {MPCT_INFO_DOWN, MPCT_R_NET_DOWN, MPCT_H_NO_INTERNET}, {MPCT_INFO_OFFLINE, MPCT_R_NET_DOWN, MPCT_H_NO_INTERNET},
        {MPCT_INFO_KILLED, MPCT_R_NET_OFF, MPCT_H_PAUSED}, {MPCT_INFO_UPDATE, MPCT_R_NET_UPDATE, MPCT_H_UPDATE},
        {99, MPCT_R_NET_DOWN, MPCT_H_NO_INTERNET}, {-7, MPCT_R_NET_DOWN, MPCT_H_NO_INTERNET},   // (a code nobody knows)
    };
    for (const C& c : cases) {
        Fake f;
        f.infoOutcome = c.outcome;
        MpConnTest t;
        t.start(Probes(&f), {false, false});
        RunToEnd(t, f, f.now);
        CHECKF(t.view().result[MPCT_INTERNET] == c.result && t.view().verdict.headline == c.headline, "outcome %d", c.outcome);
        CHECK(t.view().result[MPCT_HOSTING] == MPCT_R_SKIPPED && t.view().result[MPCT_DELAY] == MPCT_R_SKIPPED && f.mapBegins == 0);
        CHECK(t.view().result[MPCT_LOCAL] == MPCT_R_LOCAL_OK);   // (the Same Wi-Fi answer stands on its own)
    }
    // the service never answers: the call gets its own timeout, is cancelled, and the run still ends inside the budget
    Fake f;
    f.infoNever = true;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    const uint64_t took = RunToEnd(t, f, f.now);
    CHECK(t.view().result[MPCT_INTERNET] == MPCT_R_NET_DOWN && f.infoCancels >= 1 && f.lastInfoTimeout <= MP_CT_INFO_MS);
    CHECKF(took >= MP_CT_LOCAL_MS + MP_CT_INFO_MS - 100 && took <= MP_CT_BUDGET_MS, "took %llu", static_cast<unsigned long long>(took));
    // the call cannot even start
    Fake g;
    g.infoStarts = false;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false});
    RunToEnd(t2, g, g.now);
    CHECK(t2.view().result[MPCT_INTERNET] == MPCT_R_NET_DOWN);
}

static void TestRunHosting() {
    Fake f;   // a router that never answers and a STUN server that is silent: the step ends at its own limit with "unknown"
    f.mapNever = true;
    f.stunAnswer = -1;
    f.stunLatency = 100000;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    const uint64_t took = RunToEnd(t, f, f.now);
    CHECK(t.view().result[MPCT_HOSTING] == MPCT_R_HOST_UNKNOWN && f.mapEnds == 1 && f.stunEnds == 1);
    CHECKF(took <= MP_CT_BUDGET_MS, "%llu", static_cast<unsigned long long>(took));
    // carrier-grade NAT: the router mapped a port behind the provider's NAT
    Fake g;
    g.map = {MPCT_MAP_MAPPED, true, 0x64400005};
    g.stunIp = 0x64400005;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false});
    RunToEnd(t2, g, g.now);
    CHECK(t2.view().result[MPCT_HOSTING] == MPCT_R_HOST_CGNAT && t2.view().verdict.headline == MPCT_H_JOIN_ONLY && t2.view().verdict.lines[0] == MPCT_L_HOST_CGNAT);
    // the router refuses, STUN shows a public address: hosting may work with a forwarded port
    Fake h;
    h.map = {MPCT_MAP_FAILED, false, 0};
    MpConnTest t3;
    t3.start(Probes(&h), {false, false});
    RunToEnd(t3, h, h.now);
    CHECK(t3.view().result[MPCT_HOSTING] == MPCT_R_HOST_MAYBE && h.mapEnds == 1);
}

static void TestRunDelay() {
    struct C { int ms; uint8_t result; } cases[] = {{15, MPCT_R_DELAY_FAST}, {130, MPCT_R_DELAY_OK}, {400, MPCT_R_DELAY_SLOW}};
    for (const C& c : cases) {
        Fake f;
        for (int& s : f.sampleMs) s = c.ms;
        MpConnTest t;
        t.start(Probes(&f), {false, false});
        RunToEnd(t, f, f.now);
        CHECKF(t.view().result[MPCT_DELAY] == c.result && t.view().detail[MPCT_DELAY] == c.ms, "ms %d -> %d", c.ms, t.view().result[MPCT_DELAY]);
        CHECK(t.view().verdict.weakLink == (c.result == MPCT_R_DELAY_SLOW));
    }
    // the delay calls fail: the first call's time stands (it is what we know)
    Fake g;
    g.sampleMs[0] = 150;
    MpConnTest t;
    // make the later calls fail: outcome flips after the first
    struct Flip { static int Poll(void* c, MpCtInfo* o) { Fake* f = static_cast<Fake*>(c); int r = FInfoPoll(c, o); if (r == 1 && f->infoCalls > 1) o->outcome = MPCT_INFO_DOWN; return r; } };
    MpCtProbes p = Probes(&g);
    p.infoPoll = Flip::Poll;
    t.start(p, {false, false});
    RunToEnd(t, g, g.now);
    CHECK(t.view().result[MPCT_DELAY] == MPCT_R_DELAY_OK && t.view().detail[MPCT_DELAY] == 150);
}

static void TestRunHostileProbes() {
    Fake f;
    f.wifi = {true, 400};          // not a quality
    for (int& s : f.sampleMs) s = -5;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    RunToEnd(t, f, f.now);
    CHECK(t.view().result[MPCT_WIFI] == MPCT_R_WIFI_UNKNOWN);
    CHECK(t.view().detail[MPCT_WIFI] <= 32000);
    CHECK(t.view().result[MPCT_DELAY] == MPCT_R_SKIPPED && t.view().verdict.headline > MPCT_H_NONE);   // (no usable number: no claim)
    Fake g;
    for (int& s : g.sampleMs) s = 1000000000;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false});
    RunToEnd(t2, g, g.now);
    CHECK(t2.view().result[MPCT_DELAY] == MPCT_R_SKIPPED);
    // a probe set with nothing in it at all: a run that ends, all checks answered, no crash
    MpCtProbes empty;
    memset(&empty, 0, sizeof empty);
    Fake h;
    empty.now = FNow;
    empty.ctx = &h;
    MpConnTest t3;
    t3.start(empty, {false, false});
    RunToEnd(t3, h, h.now);
    CHECK(t3.view().done && t3.view().result[MPCT_INTERNET] == MPCT_R_NET_NOT_SET_UP);
    // no clock: the budget counts as spent after the first pump
    MpCtProbes noclock;
    memset(&noclock, 0, sizeof noclock);
    MpConnTest t4;
    t4.start(noclock, {false, false});
    for (int i = 0; i < 10 && t4.running(); i++) t4.pump();
    CHECK(!t4.running() && t4.view().done);
}

static void TestRunBudget() {
    // everything as slow as it can be: the Same Wi-Fi search runs to its limit, the service takes its whole timeout, the rest is out of time
    Fake f;
    f.infoLatency = 2900;
    f.mapNever = true;
    f.stunAnswer = -1, f.stunLatency = 100000;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    const uint64_t took = RunToEnd(t, f, f.now);
    CHECKF(took <= MP_CT_BUDGET_MS, "%llu", static_cast<unsigned long long>(took));
    CHECK(t.view().done);
    for (int i = 0; i < MPCT_CHECKS; i++) CHECK(t.view().state[i] == MPCT_ST_DONE);
    CHECK(f.mapEnds == f.mapBegins && f.stunEnds == f.stunBegins);   // nothing left behind
    // a service that answers at the very end of the budget: the delay is skipped, never claimed
    Fake g;
    g.localLater = 0;
    g.infoLatency = 2999;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false});
    const uint64_t took2 = RunToEnd(t2, g, g.now);
    CHECK(took2 <= MP_CT_BUDGET_MS && t2.view().done);
}

static void TestRunCancelAndRestart() {
    Fake f;
    MpConnTest t;
    t.start(Probes(&f), {false, false});
    // run until the hosting step holds the router mapping
    int frames = 0;
    while (t.running() && t.view().state[MPCT_HOSTING] != MPCT_ST_RUNNING && frames++ < 500) { f.now += 20; t.pump(); }
    for (int i = 0; i < 3; i++) { f.now += 20; t.pump(); }
    CHECK(f.mapBegins == 1 && f.mapEnds == 0 && t.view().state[MPCT_HOSTING] == MPCT_ST_RUNNING);
    t.cancel();
    CHECK(!t.running() && !t.view().done && f.mapEnds == 1 && f.stunEnds == 1);   // cancelling lets the mapping go
    CHECK(t.view().state[MPCT_HOSTING] == MPCT_ST_WAITING && t.view().state[MPCT_WIFI] == MPCT_ST_DONE);
    t.cancel();   // twice is harmless
    CHECK(f.mapEnds == 1);
    t.pump();     // so is pumping a stopped test
    // a new run replaces a running one
    t.start(Probes(&f), {false, false});
    const uint32_t s1 = t.view().serial;
    t.start(Probes(&f), {false, false});
    CHECK(t.view().serial != s1 && t.running() && t.view().state[MPCT_WIFI] == MPCT_ST_RUNNING && t.view().state[MPCT_LOCAL] == MPCT_ST_WAITING);
    RunToEnd(t, f, f.now);
    CHECK(t.view().done && f.mapEnds == f.mapBegins);
    // cancel after the end keeps the finished result
    t.cancel();
    CHECK(t.view().done && t.view().verdict.headline == MPCT_H_ALL_GOOD);
}

// relay only (the player asked to hide the address): the router and the STUN server are never asked, hosting is not tested, the rest runs
static void TestRunRelayOnly() {
    Fake f;
    MpConnTest t;
    t.start(Probes(&f), {false, false, true});
    RunToEnd(t, f, f.now);
    const MpCtView& v = t.view();
    CHECK(v.done && f.mapBegins == 0 && f.stunBegins == 0 && f.mapEnds == 0);
    CHECK(v.result[MPCT_HOSTING] == MPCT_R_SKIPPED);
    CHECK(v.result[MPCT_INTERNET] == MPCT_R_NET_OK && v.result[MPCT_DELAY] != MPCT_R_NONE && f.infoCalls >= 1);   // the service is still asked (that is the Internet check)
    // the same run without relay only does ask both
    Fake g;
    MpConnTest t2;
    t2.start(Probes(&g), {false, false, false});
    RunToEnd(t2, g, g.now);
    CHECK(g.mapBegins == 1 && g.stunBegins == 1 && t2.view().result[MPCT_HOSTING] == MPCT_R_HOST_OPEN);
}

static void TestRunInRoom() {
    // a guest: the host's delay is what counts; the router is not asked, nothing released; hosting is skipped
    Fake f;
    f.hostRtt = 260;
    MpConnTest t;
    t.start(Probes(&f), {true, false});
    RunToEnd(t, f, f.now);
    const MpCtView& v = t.view();
    CHECK(v.inRoom && v.result[MPCT_HOSTING] == MPCT_R_SKIPPED && f.mapBegins == 0 && f.mapEnds == 0 && f.stunBegins == 0);
    CHECK(v.result[MPCT_DELAY] == MPCT_R_DELAY_SLOW && v.detail[MPCT_DELAY] == 260 && f.infoCalls == 1);   // (no extra service calls for the delay)
    CHECK(v.verdict.headline == MPCT_H_SLOW && v.verdict.weakLink);
    // in a room the Same Wi-Fi search is not ours to start
    Fake g;
    g.localAnswer = -2;
    g.hostRtt = 30;
    MpConnTest t2;
    t2.start(Probes(&g), {true, false});
    RunToEnd(t2, g, g.now);
    CHECK(t2.view().result[MPCT_LOCAL] == MPCT_R_SKIPPED && t2.view().result[MPCT_DELAY] == MPCT_R_DELAY_FAST);
    CHECK(t2.view().verdict.headline == MPCT_H_ALL_GOOD && t2.view().verdict.lineCount == 0);
    // the host of a room: what its own mapping says is read, not asked for, and not released
    Fake h;
    h.hostRtt = 70;
    MpConnTest t3;
    t3.start(Probes(&h), {true, true});
    RunToEnd(t3, h, h.now);
    CHECK(t3.view().result[MPCT_HOSTING] == MPCT_R_HOST_OPEN && h.mapBegins == 0 && h.mapEnds == 0);
    // in a room with the host's delay not known yet: the service's round trip stands in
    Fake i;
    i.hostRtt = -1;
    MpConnTest t4;
    t4.start(Probes(&i), {true, false});
    RunToEnd(t4, i, i.now);
    CHECK(t4.view().result[MPCT_DELAY] == MPCT_R_DELAY_FAST && i.infoCalls == 1 + MP_CT_RTT_SAMPLES);
}

static void TestRunLocal() {
    struct C { int answer; uint8_t result; } cases[] = {{-1, MPCT_R_LOCAL_BLOCKED}, {-2, MPCT_R_SKIPPED}, {3, MPCT_R_LOCAL_FOUND}, {0, MPCT_R_LOCAL_OK}, {-9, MPCT_R_LOCAL_BLOCKED}};
    for (const C& c : cases) {
        Fake f;
        f.localAnswer = c.answer;
        MpConnTest t;
        t.start(Probes(&f), {false, false});
        RunToEnd(t, f, f.now);
        CHECKF(t.view().result[MPCT_LOCAL] == c.result, "answer %d -> %d", c.answer, t.view().result[MPCT_LOCAL]);
    }
    // a quiet network: the search gets its whole time, not more
    Fake f;
    MpConnTest t;
    const uint64_t t0 = f.now;
    t.start(Probes(&f), {false, false});
    int frames = 0;
    while (t.running() && t.view().state[MPCT_INTERNET] == MPCT_ST_WAITING && t.view().state[MPCT_LOCAL] != MPCT_ST_DONE && frames++ < 500) { f.now += 20; t.pump(); }
    CHECK(f.now - t0 >= MP_CT_LOCAL_MS && f.now - t0 <= MP_CT_LOCAL_MS + 60);
}

int main() {
    TestHelpers();
    TestHostResult();
    TestVerdictTable();
    TestVerdictExhaustive();
    TestRunHappy();
    TestRunNoWifi();
    TestRunNoServiceAndStunOff();
    TestRunInternetProblems();
    TestRunHosting();
    TestRunDelay();
    TestRunHostileProbes();
    TestRunBudget();
    TestRunCancelAndRestart();
    TestRunInRoom();
    TestRunRelayOnly();
    TestRunLocal();
    printf("conntest_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
