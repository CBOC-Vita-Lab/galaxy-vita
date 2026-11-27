// Galaxy 3.0 multiplayer (agent netx): host tests of the UPnP IGD client (net/src/online/np_upnp.cpp) against a fake gateway on
// 127.0.0.1 (an SSDP responder and an HTTP/SOAP server in a thread of this program), with descriptions shaped like real routers'
// (miniupnpd, a Fritz!Box with both WAN services), hostile and cut answers, a slow router, chunked answers, port conflicts, leases.
// Plus the parsers fed with mutated answers (ASan/UBSan watch every read).
//
//   tests/mp/upnp_build.sh            (g++ -fsanitize=address,undefined; also builds and runs direct_test)
#include "../../net/src/online/np_upnp.h"
#include "fake_igd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

using NpNet::Upnp;
using NpNet::UpnpConfig;

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<std::string> gLog;
static void LogLine(void*, const char* line) {
    gLog.push_back(line);
    if (getenv("UPNP_TEST_VERBOSE")) fprintf(stderr, "  [upnp] %s\n", line);
}
static int LogHas(const char* needle) {
    int n = 0;
    for (auto& l : gLog) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

static UpnpConfig Cfg(const FakeIgd& f) {
    UpnpConfig c;
    c.internalPort = 41100;
    c.internalIp = 0x7F000001;
    c.ssdpTarget = NpNet::Addr(0x7F000001, uint16_t(f.ssdpPort));
    c.searchMs = 1200;
    c.httpMs = 1000;
    c.log = LogLine;
    return c;
}

// Steps until the client is done (MAPPED counts as done unless `untilFinished`), or the time is up. Returns the elapsed ms.
static uint64_t Run(Upnp& u, uint64_t maxMs, bool untilFinished = false) {
    const uint64_t t0 = NowMs();
    while (NowMs() - t0 < maxMs) {
        u.Step(NowMs());
        if (u.Finished() || (!untilFinished && u.GetState() == Upnp::MAPPED)) break;
        SleepMs(2);
    }
    return NowMs() - t0;
}

// ---- end-to-end against the fake ---------------------------------------------------------------------------------------------------
static void TestMiniupnpd() {
    FakeIgd f;
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(u.ExternalPort() == 41100);
    CHECK(u.ExternalIp() == 0xCB007107u);
    CHECK(!u.ExternalIsPrivate());
    CHECK(f.Count("AddPortMapping 41100 lease=3600 127.0.0.1:41100 Super Mario Galaxy") == 1);
    CHECK(LogHas("mapped UDP 203.0.113.7:41100 -> 127.0.0.1:41100 (lease 3600 s)") == 1);
    // remove on exit
    u.Remove(NowMs());
    Run(u, 3000, true);
    CHECK(u.GetState() == Upnp::REMOVED);
    CHECK(f.Count("DeletePortMapping 41100") == 1);
    CHECK(f.table.empty());
    f.Stop();
}

static void TestFritz() {
    FakeIgd f;
    f.desc = FritzDesc();
    f.emptyIpFor = "/igdupnp/control/WANIPConn1";   // the idle IP service: the client must go on to the PPP one
    f.st = "urn:schemas-upnp-org:service:WANIPConnection:1";
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(f.Count("GetExternalIPAddress /igdupnp/control/WANIPConn1") == 1);
    CHECK(f.Count("GetExternalIPAddress /igdupnp/control/WANPPPConn1") == 1);
    CHECK(f.Count("AddPortMapping 41100") == 1);
    {
        std::lock_guard<std::mutex> g(f.mu);
        CHECK(!f.actions.empty() && f.actions.back().find("AddPortMapping") == 0);
    }
    u.Abort();
    f.Stop();
}

static void TestChunked() {
    FakeIgd f;
    f.descMode = FakeIgd::CHUNKED;
    f.desc = FritzDesc();
    CHECK(f.Start());
    Upnp u;
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED);
    u.Abort();
    f.Stop();
}

static void TestConflicts() {
    FakeIgd f;
    f.table[41100] = "192.168.1.50:41100";   // another console
    f.table[41101] = "192.168.1.51:41100";
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 6000);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(u.ExternalPort() == 41102);
    CHECK(f.Count("GetSpecificPortMappingEntry") == 3);   // (corefix 8: every external port is looked at before it is asked for)
    CHECK(f.Count("AddPortMapping 41100") == 0 && f.Count("AddPortMapping 41101") == 0);   // (the taken ones are never written to)
    u.Abort();
    f.Stop();

    // all taken
    FakeIgd g;
    g.table[41100] = "10.0.0.2:41100";
    g.table[41101] = "10.0.0.3:41100";
    g.table[41102] = "10.0.0.4:41100";
    CHECK(g.Start());
    UpnpConfig c = Cfg(g);
    c.portTries = 3;
    Upnp v;
    CHECK(v.Start(c, NowMs()));
    Run(v, 6000);
    CHECK(v.GetState() == Upnp::FAILED);
    CHECK(strstr(v.Error(), "taken") != nullptr);
    g.Stop();

}

// corefix 8: a router that overwrites a mapping of another device on AddPortMapping: the client looks first, finds the port mapped to
// another LAN host and takes the next one; the other device's forward is untouched.
static void TestNoOverwrite() {
    FakeIgd f;
    f.overwrite = true;
    f.table[41100] = "192.168.1.50:41100";
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED && u.ExternalPort() == 41101);
    CHECK(f.Count("AddPortMapping 41100") == 0);
    CHECK(f.Count("GetSpecificPortMappingEntry 41100") == 1 && f.Count("AddPortMapping 41101 lease=3600") == 1);
    {
        std::lock_guard<std::mutex> g(f.mu);
        CHECK(f.table[41100] == "192.168.1.50:41100" && f.table[41101] == "127.0.0.1:41100");
    }
    CHECK(LogHas("mapped to another device on the LAN") == 1);
    u.Abort();
    f.Stop();
}

// corefix 8: a second device answers the M-SEARCH before the router. With the default gateway known its answer is used, the rogue's
// description is never fetched. (Without a gateway that answers, the first answer still counts, as before.)
static void TestRogueResponder() {
    {
        FakeIgd f;
        f.rogue = true;
        CHECK(f.Start());
        UpnpConfig c = Cfg(f);
        c.gatewayIp = 0x7F000001;
        Upnp u;
        gLog.clear();
        CHECK(u.Start(c, NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::MAPPED && u.ExternalPort() == 41100);
        CHECK(f.rogueHits == 0);
        CHECK(LogHas("preferring the answer of the default gateway") == 1);
        CHECK(f.Count("AddPortMapping 41100") == 1);
        u.Abort();
        f.Stop();
    }
    {
        FakeIgd f;   // the gateway is silent: the only answer is the rogue's: used (the old behaviour), the log says so
        f.rogue = true;
        f.answerSsdp = false;
        CHECK(f.Start());
        UpnpConfig c = Cfg(f);
        c.gatewayIp = 0x7F000001;
        Upnp u;
        gLog.clear();
        CHECK(u.Start(c, NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED || u.GetState() == Upnp::SEARCHING || u.GetState() == Upnp::DESCRIBING);   // (the rogue has no router behind it)
        u.Abort();
        f.Stop();
    }
}

// corefix 8: a router that only takes lease 0: the mapping is flagged permanent (the caller records it), deleted on exit; an earlier
// run's permanent mapping (staleExternalPort) is deleted first when it is ours, and left alone when it belongs to another device.
static void TestPermanentRecordAndStale() {
    {
        FakeIgd f;
        f.onlyPermanent = true;
        CHECK(f.Start());
        Upnp u;
        gLog.clear();
        CHECK(u.Start(Cfg(f), NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::MAPPED && u.Permanent() && u.Lease() == 0);
        CHECK(LogHas("NO lease") == 1);
        u.Remove(NowMs());
        Run(u, 3000, true);
        CHECK(u.GetState() == Upnp::REMOVED && u.RemovedOk() && f.table.empty());
        f.Stop();
    }
    {
        FakeIgd f;   // the stale permanent mapping of an earlier run (ours): deleted, then the usual mapping
        f.table[41100] = "127.0.0.1:41100";
        CHECK(f.Start());
        UpnpConfig c = Cfg(f);
        c.staleExternalPort = 41100;
        Upnp u;
        gLog.clear();
        CHECK(u.Start(c, NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::MAPPED && u.ExternalPort() == 41100 && !u.Permanent());
        CHECK(f.Count("DeletePortMapping 41100") == 1 && LogHas("deleting the permanent mapping") == 1);
        {
            std::lock_guard<std::mutex> g(f.mu);
            size_t del = 0, add = 0;
            for (size_t i = 0; i < f.actions.size(); i++) {
                if (f.actions[i].find("DeletePortMapping") == 0) del = i;
                if (f.actions[i].find("AddPortMapping") == 0) add = i;
            }
            CHECK(del != 0 && add > del);
        }
        u.Abort();
        f.Stop();
    }
    {
        FakeIgd f;   // the recorded port now belongs to another device: not ours, never deleted
        f.table[41105] = "192.168.1.9:41100";
        CHECK(f.Start());
        UpnpConfig c = Cfg(f);
        c.staleExternalPort = 41105;
        Upnp u;
        CHECK(u.Start(c, NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::MAPPED && u.ExternalPort() == 41100);
        CHECK(f.Count("DeletePortMapping") == 0);
        {
            std::lock_guard<std::mutex> g(f.mu);
            CHECK(f.table.count(41105) == 1);
        }
        u.Abort();
        f.Stop();
    }
}

static void TestOwnEntryConflict() {
    // a router that answers 718 even for the same client (some do): GetSpecificPortMappingEntry shows it is ours -> kept
    FakeIgd f;
    f.table[41100] = "127.0.0.1:41100";
    CHECK(f.Start());
    // emulate by a table entry with a different internal port spelling that still parses to ours: "127.0.0.1:041100" -> 41100
    {
        std::lock_guard<std::mutex> g(f.mu);
        f.table[41100] = "127.0.0.1:041100";
    }
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED && u.ExternalPort() == 41100);
    CHECK(f.Count("GetSpecificPortMappingEntry 41100") == 1);
    CHECK(LogHas("already ours") == 1);
    u.Abort();
    f.Stop();
}

static void TestPermanentOnly() {
    FakeIgd f;
    f.onlyPermanent = true;
    CHECK(f.Start());
    Upnp u;
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(u.Lease() == 0);
    CHECK(f.Count("AddPortMapping 41100 lease=0") == 1);
    u.Abort();
    f.Stop();
}

static void TestRenewal() {
    FakeIgd f;
    CHECK(f.Start());
    UpnpConfig c = Cfg(f);
    c.leaseSec = 2;   // renew at 1 s
    Upnp u;
    gLog.clear();
    CHECK(u.Start(c, NowMs()));
    Run(u, 3000);
    CHECK(u.GetState() == Upnp::MAPPED);
    const uint64_t t0 = NowMs();
    while (NowMs() - t0 < 2600) { u.Step(NowMs()); SleepMs(5); }
    CHECK(u.Renewals() >= 2);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(f.Count("AddPortMapping 41100 lease=2") >= 3);
    u.Abort();
    f.Stop();
}

static void TestCgnat() {
    FakeIgd f;
    f.extIp = "100.64.12.34";
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    Run(u, 5000);
    CHECK(u.GetState() == Upnp::MAPPED);
    CHECK(u.ExternalIsPrivate());
    CHECK(LogHas("another NAT") == 1);
    u.Abort();
    f.Stop();
}

static void TestNoRouter() {
    FakeIgd f;
    f.answerSsdp = false;
    CHECK(f.Start());
    Upnp u;
    gLog.clear();
    CHECK(u.Start(Cfg(f), NowMs()));
    const uint64_t took = Run(u, 5000);
    CHECK(u.GetState() == Upnp::FAILED);
    CHECK(took >= 1100 && took < 1800);   // the search's own limit (1200 ms here)
    CHECK(LogHas("no UPnP router answered") == 1);
    f.Stop();
}

static void TestHostile() {
    // the answer names a description on another host than the one that answered: not followed
    {
        FakeIgd f;
        f.location = "http://10.1.2.3:5000/rootDesc.xml";
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED);
        CHECK(f.Count("") == 0);
        f.Stop();
    }
    // a control URL on another host: dropped -> no service
    {
        FakeIgd f;
        f.desc = MiniupnpdDesc();
        const size_t p = f.desc.find("<controlURL>/ctl/IPConn</controlURL>");
        f.desc.replace(p, strlen("<controlURL>/ctl/IPConn</controlURL>"), "<controlURL>http://8.8.8.8:80/ctl/IPConn</controlURL>");
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED);
        CHECK(f.Count("GetExternalIPAddress") == 0);
        f.Stop();
    }
    // header injection through the service type: refused
    {
        FakeIgd f;
        f.desc = MiniupnpdDesc();
        const std::string t = "urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>";
        const size_t p = f.desc.find(t);
        f.desc.replace(p, t.size(), "urn:schemas-upnp-org:service:WANIPConnection:1\"&#13;&#10;X-Evil: 1</serviceType>");
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED);
        f.Stop();
    }
    // a huge description (cut at the buffer) with unclosed elements, garbage, an answer without a header end
    const FakeIgd::DescMode modes[] = {FakeIgd::GARBAGE, FakeIgd::NO_HEADER_END, FakeIgd::TRUNCATED};
    for (FakeIgd::DescMode m : modes) {
        FakeIgd f;
        f.descMode = m;
        CHECK(f.Start());
        Upnp u;
        gLog.clear();
        CHECK(u.Start(Cfg(f), NowMs()));
        const uint64_t took = Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED);
        CHECK(took < 2500);
        if (m == FakeIgd::TRUNCATED) CHECK(LogHas("cut short") == 1);
        f.Stop();
    }
    {
        FakeIgd f;
        std::string huge = "<?xml version=\"1.0\"?><root>";
        for (int i = 0; i < 6000; ++i) huge += "<service><serviceType>urn:x";
        huge += "</root>";
        f.desc = huge;   // ~170 KB: more than the client keeps
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        Run(u, 5000);
        CHECK(u.GetState() == Upnp::FAILED);
        f.Stop();
    }
}

static void TestSlow() {
    {
        FakeIgd f;
        f.descMode = FakeIgd::SLOW;
        CHECK(f.Start());
        Upnp u;
        gLog.clear();
        CHECK(u.Start(Cfg(f), NowMs()));
        const uint64_t took = Run(u, 6000);
        CHECK(u.GetState() == Upnp::FAILED);
        CHECK(took < 1600);   // httpMs = 1000 after the search: never the router's 5 s
        CHECK(LogHas("no answer from the router in 1000 ms") == 1);
        f.Stop();
    }
    {
        FakeIgd f;
        f.slowSoap = true;
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        const uint64_t took = Run(u, 6000);
        CHECK(u.GetState() == Upnp::FAILED);
        CHECK(took < 1700);
        f.Stop();
    }
    // Step() itself never waits: the longest single call stays far below a frame
    {
        FakeIgd f;
        f.descMode = FakeIgd::SLOW;
        CHECK(f.Start());
        Upnp u;
        CHECK(u.Start(Cfg(f), NowMs()));
        uint64_t worst = 0;
        const uint64_t t0 = NowMs();
        while (!u.Finished() && NowMs() - t0 < 3000) {
            const uint64_t a = NowMs();
            u.Step(a);
            const uint64_t d = NowMs() - a;
            if (d > worst) worst = d;
            SleepMs(1);
        }
        CHECK(worst <= 5);
        f.Stop();
    }
}

static void TestRemoveWhileMapping() {
    FakeIgd f;
    CHECK(f.Start());
    Upnp u;
    CHECK(u.Start(Cfg(f), NowMs()));
    while (u.GetState() != Upnp::MAPPING && !u.Finished()) { u.Step(NowMs()); SleepMs(1); }
    CHECK(u.GetState() == Upnp::MAPPING);
    u.Remove(NowMs());   // the Add in flight: deleted after it answered
    Run(u, 3000, true);
    CHECK(u.GetState() == Upnp::REMOVED);
    CHECK(f.Count("DeletePortMapping 41100") == 1);
    CHECK(f.table.empty());
    f.Stop();
}

// ---- parsers -----------------------------------------------------------------------------------------------------------------------
static bool ParseSsdpCopy(const std::string& s, char* loc, size_t cap) {
    std::vector<char> b(s.begin(), s.end());   // exactly sized: ASan sees any read past the end
    return Upnp::ParseSsdpAnswer(b.data(), b.size(), loc, cap);
}

static void TestParsers() {
    char loc[256];
    CHECK(ParseSsdpCopy("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\n\r\n", loc, sizeof loc));
    CHECK(strcmp(loc, "http://192.168.1.1:5000/rootDesc.xml") == 0);
    CHECK(ParseSsdpCopy("HTTP/1.1 200 OK\r\nusn: uuid:x::urn:schemas-upnp-org:service:WANPPPConnection:1\r\nlocation:http://192.168.178.1:49000/igddesc.xml\r\n", loc, sizeof loc));
    CHECK(!ParseSsdpCopy("NOTIFY * HTTP/1.1\r\nNT: upnp:rootdevice\r\nLOCATION: http://192.168.1.1/x\r\n\r\n", loc, sizeof loc));
    CHECK(!ParseSsdpCopy("HTTP/1.1 200 OK\r\nST: urn:dial-multiscreen-org:service:dial:1\r\nLOCATION: http://192.168.1.9/dd.xml\r\n\r\n", loc, sizeof loc));   // a TV
    CHECK(!ParseSsdpCopy("HTTP/1.1 404 Not Found\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: http://1.2.3.4/\r\n\r\n", loc, sizeof loc));
    CHECK(!ParseSsdpCopy("HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: " + std::string(400, 'a') + "\r\n\r\n", loc, sizeof loc));

    uint32_t ip;
    uint16_t port;
    char path[64];
    CHECK(Upnp::ParseUrl("http://192.168.1.1:5000/rootDesc.xml", &ip, &port, path, sizeof path) && ip == 0xC0A80101 && port == 5000 && !strcmp(path, "/rootDesc.xml"));
    CHECK(Upnp::ParseUrl("HTTP://10.0.0.138", &ip, &port, path, sizeof path) && port == 80 && !strcmp(path, "/"));
    CHECK(!Upnp::ParseUrl("http://router.local:5000/x", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.1:0/x", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.1:65536/x", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.256/x", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.1/a b", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.1/a\r\nX: y", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("https://192.168.1.1/x", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl("http://192.168.1.1/x\"", &ip, &port, path, sizeof path));
    CHECK(!Upnp::ParseUrl(("http://1.2.3.4/" + std::string(100, 'p')).c_str(), &ip, &port, path, sizeof path));

    Upnp::Service s[4];
    const std::string fr = FritzDesc();
    int n = Upnp::ParseDescription(fr.data(), fr.size(), 0xC0A8B201, 49000, s, 4);
    CHECK(n == 2);
    CHECK(n == 2 && !strcmp(s[0].type, "urn:schemas-upnp-org:service:WANIPConnection:1") && !strcmp(s[0].path, "/igdupnp/control/WANIPConn1") &&
          s[0].ip == 0xC0A8B201 && s[0].port == 49000);
    CHECK(n == 2 && !strcmp(s[1].type, "urn:schemas-upnp-org:service:WANPPPConnection:1"));
    // URLBase on the same host moves the port; on another host it is ignored
    std::string withBase = MiniupnpdDesc();
    withBase.insert(withBase.find("<device>"), "<URLBase>http://192.168.1.1:5555/</URLBase>");
    n = Upnp::ParseDescription(withBase.data(), withBase.size(), 0xC0A80101, 5000, s, 4);
    CHECK(n == 1 && s[0].port == 5555 && !strcmp(s[0].path, "/ctl/IPConn"));
    withBase = MiniupnpdDesc();
    withBase.insert(withBase.find("<device>"), "<URLBase>http://6.6.6.6:80/</URLBase>");
    n = Upnp::ParseDescription(withBase.data(), withBase.size(), 0xC0A80101, 5000, s, 4);
    CHECK(n == 1 && s[0].ip == 0xC0A80101 && s[0].port == 5000);
    // an absolute control URL on another host than the description's: never followed (a description cannot send us elsewhere)
    {
        std::string other = MiniupnpdDesc();
        const std::string c = "<controlURL>/ctl/IPConn</controlURL>";
        other.replace(other.find(c), c.size(), "<controlURL>http://8.8.8.8:80/ctl/IPConn</controlURL>");
        CHECK(Upnp::ParseDescription(other.data(), other.size(), 0xC0A80101, 5000, s, 4) == 0);
        other = MiniupnpdDesc();
        other.replace(other.find(c), c.size(), "<controlURL>http://192.168.1.1:5001/ctl/IPConn</controlURL>");
        CHECK(Upnp::ParseDescription(other.data(), other.size(), 0xC0A80101, 5000, s, 4) == 1 && s[0].port == 5001);
    }
    // prefixed elements and a relative control URL without a slash
    const std::string pre = "<r:root><r:service><r:serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</r:serviceType>"
                            "<r:controlURL>upnp/control/WANIPConn</r:controlURL></r:service></r:root>";
    n = Upnp::ParseDescription(pre.data(), pre.size(), 0x0A000001, 80, s, 4);
    CHECK(n == 1 && !strcmp(s[0].path, "/upnp/control/WANIPConn"));

    char text[32];
    const std::string soap = FakeIgd::Soap("GetExternalIPAddress", "<NewExternalIPAddress> 203.0.113.9 </NewExternalIPAddress>");
    CHECK(Upnp::ElementText(soap.data(), soap.size(), "NewExternalIPAddress", text, sizeof text) && Upnp::ParseIp(text, &ip) && ip == 0xCB007109);
    const std::string fault = FakeIgd::Fault(718, "ConflictInMappingEntry");
    CHECK(Upnp::SoapErrorCode(fault.data(), fault.size()) == 718);
    CHECK(Upnp::SoapErrorCode(soap.data(), soap.size()) == -1);
    CHECK(!Upnp::ParseIp("1.2.3", &ip) && !Upnp::ParseIp("1.2.3.4.5", &ip) && !Upnp::ParseIp("1.2.3.1000", &ip) && !Upnp::ParseIp("01234.1.1.1", &ip));

    // HTTP framing
    {
        std::string a = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
        int st;
        size_t bo, bl;
        CHECK(Upnp::ParseHttp(&a[0], a.size(), false, false, &st, &bo, &bl) == 1 && st == 200 && bl == 5 && a.compare(bo, 5, "hello") == 0);
        std::string b = "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nhello";
        CHECK(Upnp::ParseHttp(&b[0], b.size(), false, false, &st, &bo, &bl) == 0);
        CHECK(Upnp::ParseHttp(&b[0], b.size(), true, false, &st, &bo, &bl) == 0);   // (cut: the caller has eof)
        std::string c = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;x=y\r\n world\r\n0\r\n\r\n";
        CHECK(Upnp::ParseHttp(&c[0], c.size(), false, false, &st, &bo, &bl) == 1 && bl == 11 && c.compare(bo, 11, "hello world") == 0);
        std::string d = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFF\r\nx";
        CHECK(Upnp::ParseHttp(&d[0], d.size(), true, false, &st, &bo, &bl) == -1);
        std::string e = "HTTP/1.0 500 Internal Server Error\n\nbody until close";
        CHECK(Upnp::ParseHttp(&e[0], e.size(), true, false, &st, &bo, &bl) == 1 && st == 500 && bl == 16);
        std::string f = "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n";
        CHECK(Upnp::ParseHttp(&f[0], f.size(), true, false, &st, &bo, &bl) == -1);
    }

    // mutation fuzz: every parser over cut / flipped / spliced answers, exactly-sized heap copies
    std::mt19937 rng(12345);
    const std::string seeds[] = {FritzDesc(), MiniupnpdDesc(), soap, fault, FakeIgd::Http(200, FritzDesc()),
                                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
                                 "HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: http://192.168.1.1:5000/d.xml\r\n\r\n"};
    const char* const tokens[] = {"<", ">", "</", "/>", "<service>", "</service>", "<controlURL>", "</controlURL>", "&amp;", "&", "\r\n", "\r\n\r\n",
                                  "http://", ":", "0\r\n\r\n", "FFFF\r\n", "<errorCode>", "\x00", "\xff", "Content-Length: 99999999999\r\n"};
    for (int iter = 0; iter < 20000; ++iter) {
        std::string m = seeds[rng() % (sizeof seeds / sizeof seeds[0])];
        const int edits = 1 + int(rng() % 6);
        for (int k = 0; k < edits && !m.empty(); ++k) {
            const size_t at = rng() % (m.size() + 1);
            switch (rng() % 4) {
            case 0: m.resize(at); break;
            case 1: if (at < m.size()) m[at] = char(rng()); break;
            case 2: m.insert(at, tokens[rng() % (sizeof tokens / sizeof tokens[0])]); break;
            case 3: if (at < m.size()) m.erase(at, rng() % 64); break;
            }
        }
        std::vector<char> buf(m.begin(), m.end());
        char* p = buf.empty() ? nullptr : buf.data();
        const size_t len = buf.size();
        if (!p) continue;
        Upnp::ParseDescription(p, len, 0xC0A80101, 5000, s, 4);
        char o[64];
        Upnp::ElementText(p, len, "controlURL", o, sizeof o);
        Upnp::SoapErrorCode(p, len);
        Upnp::ParseSsdpAnswer(p, len, loc, sizeof loc);
        int st;
        size_t bo, bl;
        const int r = Upnp::ParseHttp(p, len, (iter & 1) != 0, (iter & 2) != 0, &st, &bo, &bl);
        if (r == 1) CHECK(bo <= len && bl <= len - bo);
        for (int i = 0; i < 4; ++i) {
            CHECK(strlen(s[i].path) < sizeof s[i].path);
        }
    }
}

int main() {
    TestParsers();
    TestMiniupnpd();
    TestFritz();
    TestChunked();
    TestConflicts();
    TestOwnEntryConflict();
    TestNoOverwrite();
    TestRogueResponder();
    TestPermanentRecordAndStale();
    TestPermanentOnly();
    TestRenewal();
    TestCgnat();
    TestNoRouter();
    TestHostile();
    TestSlow();
    TestRemoveWhileMapping();
    printf("upnp_test: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
