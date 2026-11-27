// Galaxy 3.0 multiplayer (agent natpmp): host test of mp_portmap.cpp with BOTH fake gateways on 127.0.0.1 (the UPnP IGD of fake_igd.h and
// the NAT-PMP/PCP one of fake_natpmp.h): the first protocol to map wins, the other is cancelled and unmapped; one protocol alone; none;
// a CGNAT address from NAT-PMP; release deletes the mapping. Pumped on this thread like MpCore's helper thread does.
//
//   tests/mp/natpmp_build.sh          (builds and runs this too)
#include "../../platform/src/mp/mp_portmap.h"
#include "../../platform/src/mp/mp_core_sys.h"
#include "fake_igd.h"
#include "fake_natpmp.h"

#include <stdarg.h>

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> gLog;
extern "C" void mp_log(const char* fmt, ...) {
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    gLog.push_back(b);
    if (getenv("PORTMAP_TEST_VERBOSE")) fprintf(stderr, "  [mp] %s\n", b);
}
static int LogHas(const char* s) {
    int n = 0;
    for (auto& l : gLog) if (l.find(s) != std::string::npos) ++n;
    return n;
}

static void Pump(uint64_t ms, bool (*stop)(MpPortmapInfo&) = nullptr) {
    const uint64_t t0 = NowMs();
    while (NowMs() - t0 < ms) {
        mp_portmap_pump();
        if (stop) {
            MpPortmapInfo i;
            mp_portmap_info(&i);
            if (stop(i)) break;
        }
        SleepMs(3);
    }
}
static bool Settled(MpPortmapInfo& i) { return i.state == MP_PORTMAP_MAPPED || i.state == MP_PORTMAP_FAILED; }
static void Drain() { for (int t = 0; t < 3000 && mp_portmap_busy(); t += 5) { mp_portmap_pump(); SleepMs(5); } }

static void Env(FakeIgd* igd, FakeGw* gw) {
    if (igd) { char s[40]; snprintf(s, sizeof s, "127.0.0.1:%d", igd->ssdpPort); setenv("SMG_MP_UPNP_SSDP", s, 1); }
    else setenv("SMG_MP_UPNP_SSDP", "127.0.0.1:9", 1);
    if (gw) { char s[40]; snprintf(s, sizeof s, "127.0.0.1:%d", gw->port); setenv("SMG_MP_NATPMP_GW", s, 1); }
    else setenv("SMG_MP_NATPMP_GW", "127.0.0.1:9", 1);
}

static void TestBoth() {
    for (int round = 0; round < 3; ++round) {
        gLog.clear();
        FakeIgd igd;
        igd.extIp = "203.0.113.7";
        CHECK(igd.Start());
        FakeGw gw;
        gw.extIp = 0xCB007108;   // 203.0.113.8: tells which one won
        gw.pcp = round == 2;
        CHECK(gw.Start());
        Env(&igd, &gw);
        mp_portmap_request(41100, 0x7F000001, "test");
        Pump(5000, Settled);
        MpPortmapInfo i;
        mp_portmap_info(&i);
        CHECK(i.state == MP_PORTMAP_MAPPED && i.extPort == 41100 && !i.cgnat);
        CHECK(i.extIp == 0xCB007107u || i.extIp == 0xCB007108u);
        const bool upnpWon = i.extIp == 0xCB007107u;
        Pump(600);   // the loser finishes its cancel
        const size_t igdMaps = igd.table.size(), gwMaps = gw.TableSize();
        CHECK(igdMaps + gwMaps == 1);                      // exactly one mapping stands at the end
        CHECK(upnpWon ? igdMaps == 1 : gwMaps == 1);
        CHECK(LogHas(upnpWon ? "natpmp: cancelled" : "upnp: cancelled") == 1);
        mp_portmap_info(&i);
        CHECK(i.state == MP_PORTMAP_MAPPED && i.extIp == (upnpWon ? 0xCB007107u : 0xCB007108u));
        mp_portmap_release();
        Drain();
        CHECK(igd.table.empty() && gw.TableSize() == 0);
        mp_portmap_info(&i);
        CHECK(i.state == MP_PORTMAP_OFF);
        CHECK(!mp_portmap_busy());
        igd.Stop();
    }
}

static void TestNatpmpOnly() {
    gLog.clear();
    FakeGw gw;
    gw.extIp = 0xCB007109;
    CHECK(gw.Start());
    Env(nullptr, &gw);
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(5000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && i.extIp == 0xCB007109u && i.extPort == 41100 && i.intPort == 41100);
    CHECK(LogHas("natpmp: mapped UDP 203.0.113.9:41100") == 1);
    CHECK(gw.TableSize() == 1);
    mp_portmap_finish(3000);   // shutdown path
    CHECK(gw.TableSize() == 0 && gw.Count("pmp-map int=41100 ext=0 life=0") == 1);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_OFF && !mp_portmap_busy());
}

static void TestUpnpOnly() {
    gLog.clear();
    FakeIgd igd;
    CHECK(igd.Start());
    Env(&igd, nullptr);
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(5000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && i.extIp == 0xCB007107u);
    mp_portmap_release();
    Drain();
    CHECK(igd.table.empty());
    igd.Stop();
}

// review2 #10: a Connection Test and a room are different owners of the one mapping.
static void TestOwners() {
    gLog.clear();
    FakeIgd igd;
    CHECK(igd.Start());
    Env(&igd, nullptr);
    MpPortmapInfo i;
    // a room holds the mapping; a test on the same port joins it and, when it ends, does not delete it
    CHECK(mp_portmap_request_as(MP_PORTMAP_SESSION, 41100, 0x7F000001, "direct host"));
    Pump(5000, Settled);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && igd.table.size() == 1);
    CHECK(mp_portmap_request_as(MP_PORTMAP_TEST, 41100, 0, "conntest"));   // (a test passes no local address: it joins the room's mapping)
    mp_portmap_release_as(MP_PORTMAP_TEST);
    Pump(400);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && igd.table.size() == 1);
    // the same address: the test joins the running mapping and leaves it alone
    CHECK(mp_portmap_request_as(MP_PORTMAP_TEST, 41100, 0x7F000001, "conntest"));
    Pump(300);
    mp_portmap_release_as(MP_PORTMAP_TEST);
    Pump(600);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && igd.table.size() == 1 && LogHas("natpmp: asking") <= 1);
    // a test cannot take over a room's mapping with another port, and its release does not touch the room's
    CHECK(!mp_portmap_request_as(MP_PORTMAP_TEST, 41101, 0x7F000001, "conntest"));
    mp_portmap_release_as(MP_PORTMAP_TEST);
    Pump(300);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && i.intPort == 41100 && igd.table.size() == 1);
    // releasing an owner that holds nothing is nothing
    mp_portmap_release_as(MP_PORTMAP_TEST);
    mp_portmap_release_as(MP_PORTMAP_TEST);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED);
    // the room ends: now it goes
    mp_portmap_release();
    Drain();
    CHECK(igd.table.empty());
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_OFF);
    // a test alone: its release deletes its own mapping
    CHECK(mp_portmap_request_as(MP_PORTMAP_TEST, 41100, 0x7F000001, "conntest"));
    Pump(5000, Settled);
    CHECK(igd.table.size() == 1);
    mp_portmap_release_as(MP_PORTMAP_TEST);
    Drain();
    CHECK(igd.table.empty());
    // a room that starts while the test holds the mapping keeps it when the test ends (the order of the two releases does not matter)
    CHECK(mp_portmap_request_as(MP_PORTMAP_TEST, 41100, 0x7F000001, "conntest"));
    Pump(5000, Settled);
    CHECK(mp_portmap_request_as(MP_PORTMAP_SESSION, 41100, 0x7F000001, "direct host"));
    mp_portmap_release_as(MP_PORTMAP_TEST);
    Pump(600);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && igd.table.size() == 1);
    mp_portmap_release();
    Drain();
    CHECK(igd.table.empty());
    // a room asking for another port replaces a test's mapping
    CHECK(mp_portmap_request_as(MP_PORTMAP_TEST, 41100, 0x7F000001, "conntest"));
    Pump(5000, Settled);
    CHECK(mp_portmap_request_as(MP_PORTMAP_SESSION, 41101, 0x7F000001, "direct host"));
    Pump(5000, Settled);
    mp_portmap_release_as(MP_PORTMAP_TEST);   // (the test's claim was replaced: this must not touch the room's mapping)
    Pump(600);
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && i.intPort == 41101);
    mp_portmap_release();
    Drain();
    CHECK(igd.table.empty());
    igd.Stop();
}

static void TestNone() {
    gLog.clear();
    Env(nullptr, nullptr);
    const uint64_t t0 = NowMs();
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(6000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_FAILED);
    CHECK(NowMs() - t0 < 4000);   // both searches run in parallel: the verdict is there in ~3 s, not 6
    CHECK(strstr(i.error, "no UPnP router answered") != nullptr);
    CHECK(LogHas("upnp: failed: no UPnP router answered") == 1 && LogHas("natpmp: failed:") == 1);
    mp_portmap_release();
    Drain();
}

static void TestCgnat() {
    gLog.clear();
    FakeGw gw;
    gw.extIp = 0x64400101;   // 100.64.1.1
    CHECK(gw.Start());
    Env(nullptr, &gw);
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(5000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_MAPPED && i.cgnat && i.extIp == 0x64400101u);
    mp_portmap_release();
    Drain();
}

static void TestNatpmpRefused() {   // a router that answers NAT-PMP with "refused" and has no UPnP: its reason is the one reported
    gLog.clear();
    FakeGw gw;
    gw.pmpAddrResult = 2;
    CHECK(gw.Start());
    Env(nullptr, &gw);
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(6000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_FAILED);
    CHECK(strstr(i.error, "refused") != nullptr);
    mp_portmap_release();
    Drain();
}

static void TestOff() {
    gLog.clear();
    setenv("SMG_MP_NATPMP", "0", 1);
    FakeGw gw;
    CHECK(gw.Start());
    Env(nullptr, &gw);
    mp_portmap_request(41100, 0x7F000001, "test");
    Pump(5000, Settled);
    MpPortmapInfo i;
    mp_portmap_info(&i);
    CHECK(i.state == MP_PORTMAP_FAILED && gw.Count("pmp") == 0 && gw.Count("pcp") == 0);
    mp_portmap_release();
    Drain();
    unsetenv("SMG_MP_NATPMP");
}

int main() {
    TestBoth();
    TestNatpmpOnly();
    TestUpnpOnly();
    TestOwners();
    TestNone();
    TestCgnat();
    TestNatpmpRefused();
    TestOff();
    printf("portmap_test: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
