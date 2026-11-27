// Galaxy 3.0 multiplayer (agent natpmp): host tests of the NAT-PMP / PCP client (net/src/online/np_natpmp.cpp) against a fake gateway on
// 127.0.0.1 (a UDP socket in a thread of this program that speaks RFC 6886 and RFC 6887 with switchable behaviour): success on each
// protocol, the PCP -> NAT-PMP fallback, every result code, silence (the timeouts stay in budget), retransmission, replies from the wrong
// address or port, truncated / oversized / mangled replies, lease renewal, epoch restart, delete on exit, CGNAT addresses.
// Plus the codec fed with random and mutated bytes (ASan/UBSan watch every read).
//
//   tests/mp/natpmp_build.sh          (g++ -fsanitize=address,undefined)
#include "../../net/src/online/np_natpmp.h"
#include "fake_natpmp.h"

#include <arpa/inet.h>
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

using NpNet::NatPmp;
using NpNet::NatPmpConfig;

static int gFails = 0, gChecks = 0;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
static void SleepMs(int ms) { usleep(ms * 1000); }

static std::vector<std::string> gLog;
static void LogLine(void*, const char* line) {
    gLog.push_back(line);
    if (getenv("NATPMP_TEST_VERBOSE")) fprintf(stderr, "  [natpmp] %s\n", line);
}
static int LogHas(const char* needle) {
    int n = 0;
    for (auto& l : gLog)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

static NatPmpConfig Cfg(const FakeGw& f) {
    NatPmpConfig c;
    c.internalPort = 41100;
    c.internalIp = 0x7F000001;
    c.gateway = NpNet::Addr(0x7F000001, uint16_t(f.port));
    c.log = LogLine;
    return c;
}

// Steps until `done()` or the time is up; returns the elapsed ms.
template <class F>
static uint64_t RunUntil(NatPmp& c, uint64_t maxMs, F done) {
    const uint64_t t0 = NowMs();
    while (NowMs() - t0 < maxMs) {
        c.Step(NowMs());
        if (done()) break;
        SleepMs(2);
    }
    return NowMs() - t0;
}
static uint64_t RunMapped(NatPmp& c, uint64_t maxMs) {
    return RunUntil(c, maxMs, [&] { return c.Finished() || c.GetState() == NatPmp::MAPPED; });
}
static uint64_t RunFinished(NatPmp& c, uint64_t maxMs) {
    return RunUntil(c, maxMs, [&] { return c.Finished(); });
}

static void Clear() { gLog.clear(); }

// ---- tests --------------------------------------------------------------------------------------------------------------------------
static void TestPmpOnly() {
    Clear();
    FakeGw f;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    const uint64_t took = RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED);
    CHECK(c.Protocol() == NatPmp::PROTO_NATPMP);
    CHECK(c.ExternalPort() == 41100);
    CHECK(c.ExternalIp() == 0xCB007107u);
    CHECK(!c.ExternalIsPrivate());
    CHECK(c.Lease() == 7200);
    CHECK(took < 800);   // a local gateway answers at once: well inside the 3 s budget
    CHECK(f.Count("pmp-unsupp-pcp") >= 1);   // PCP asked first, answered "unsupported version"
    CHECK(f.Count("pmp-map int=41100 ext=41100 life=7200") == 1);
    CHECK(LogHas("mapped UDP 203.0.113.7:41100 -> 127.0.0.1:41100 (lease 7200 s) via NAT-PMP") == 1);
    c.Remove(NowMs());
    RunFinished(c, 2000);
    CHECK(c.GetState() == NatPmp::REMOVED);
    CHECK(f.Count("pmp-map int=41100 ext=0 life=0") == 1);   // RFC 6886 section 3.4
    CHECK(f.TableSize() == 0);
}

static void TestPcp() {
    Clear();
    FakeGw f;
    f.pcp = true;   // a server that speaks both: PCP wins, no NAT-PMP mapping is made
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED);
    CHECK(c.Protocol() == NatPmp::PROTO_PCP);
    CHECK(c.ExternalPort() == 41100 && c.ExternalIp() == 0xCB007107u);
    CHECK(f.Count("pcp-map int=41100 ext=41100 life=7200") >= 1);
    CHECK(f.Count("pmp-map") == 0);
    CHECK(LogHas("via PCP") == 1);
    c.Remove(NowMs());
    RunFinished(c, 2000);
    CHECK(c.GetState() == NatPmp::REMOVED);
    CHECK(f.Count("pcp-map int=41100 ext=41100 life=0") == 1);
    CHECK(f.TableSize() == 0);
}

static void TestPcpOnly() {
    Clear();
    FakeGw f;
    f.pcp = true;
    f.pmp = false;   // ignores NAT-PMP packets completely
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED && c.Protocol() == NatPmp::PROTO_PCP);
}

static void TestNoPcpConfig() {
    Clear();
    FakeGw f;
    f.pcp = true;
    CHECK(f.Start());
    NatPmpConfig cfg = Cfg(f);
    cfg.noPcp = true;
    NatPmp c;
    CHECK(c.Start(cfg, NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED && c.Protocol() == NatPmp::PROTO_NATPMP);
    CHECK(f.Count("pcp-map") == 0);
}

// a PCP-format UNSUPP_VERSION from a server that also speaks NAT-PMP: falls back
static void TestPcpUnsuppFallsBack() {
    Clear();
    FakeGw f;
    f.pcp = true;
    f.pcpResult = 1;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED && c.Protocol() == NatPmp::PROTO_NATPMP);
}

static void TestPmpErrorCodes() {
    for (int code = 1; code <= 7; ++code) {
        for (int where = 0; where < 2; ++where) {   // 0: the address request fails, 1: the mapping request fails
            Clear();
            FakeGw f;
            (where ? f.pmpMapResult : f.pmpAddrResult) = code;
            CHECK(f.Start());
            NatPmp c;
            CHECK(c.Start(Cfg(f), NowMs()));
            const uint64_t took = RunFinished(c, 4000);
            CHECK(c.GetState() == NatPmp::FAILED);
            CHECK(strcmp(c.Error(), NatPmp::PmpResultText(unsigned(code))) == 0);
            CHECK(took < 1500);   // an answered error ends at once
            CHECK(c.ExternalPort() == 0);
            CHECK(f.TableSize() == 0);
        }
    }
    CHECK(strcmp(NatPmp::PmpResultText(3), "the router has no internet connection (network failure)") == 0);
}

static void TestPcpErrorCodes() {
    for (int code = 2; code <= 13; ++code) {
        if (code == 11) continue;
        Clear();
        FakeGw f;
        f.pcp = true;
        f.pcpResult = code;
        CHECK(f.Start());
        NatPmp c;
        CHECK(c.Start(Cfg(f), NowMs()));
        const uint64_t took = RunFinished(c, 4000);
        CHECK(c.GetState() == NatPmp::FAILED);
        CHECK(strcmp(c.Error(), NatPmp::PcpResultText(unsigned(code))) == 0);
        CHECK(took < 1500);
    }
}

static void TestPcpCannotProvide() {   // 11: asked again without a suggested port
    Clear();
    FakeGw f;
    f.pcp = true;
    f.pcpRejectSuggested = true;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 4000);
    CHECK(c.GetState() == NatPmp::MAPPED && c.Protocol() == NatPmp::PROTO_PCP);
    CHECK(f.Count("pcp-map int=41100 ext=0 life=7200") >= 1);
    CHECK(c.ExternalPort() == 41100);   // (the fake assigns the internal port when none is suggested)
    // 11 that persists after the retry is a failure
    Clear();
    FakeGw g;
    g.pcp = true;
    g.pcpResult = 11;
    CHECK(g.Start());
    NatPmp d;
    CHECK(d.Start(Cfg(g), NowMs()));
    RunFinished(d, 4000);
    CHECK(d.GetState() == NatPmp::FAILED && strcmp(d.Error(), NatPmp::PcpResultText(11)) == 0);
}

static void TestAddressMismatch() {   // the console believes another address than the router sees: PCP 12 (fake checks the client-address field)
    Clear();
    FakeGw f;
    f.pcp = true;
    f.pmp = false;
    CHECK(f.Start());
    NatPmpConfig cfg = Cfg(f);
    cfg.internalIp = 0x7F000005;
    NatPmp c;
    CHECK(c.Start(cfg, NowMs()));
    RunFinished(c, 4000);
    CHECK(c.GetState() == NatPmp::FAILED && strcmp(c.Error(), NatPmp::PcpResultText(12)) == 0);
}

static void TestSilence() {
    Clear();
    FakeGw f;
    f.silent = true;
    f.pcp = true;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    const uint64_t took = RunFinished(c, 5000);
    CHECK(c.GetState() == NatPmp::FAILED);
    CHECK(took >= 2900 && took < 3300);   // the default probe budget: the result is there within ~3 s
    CHECK(strstr(c.Error(), "neither NAT-PMP nor PCP") != nullptr);
    // retransmission: the RFC schedule is 0, 250, 750, 1750 ms (four rounds fit in the budget)
    CHECK(f.Count("pmp-addr") == 4);
    CHECK(f.Count("pcp-map") == 4);
    // a shorter budget is kept
    Clear();
    NatPmpConfig cfg = Cfg(f);
    cfg.probeMs = 500;
    NatPmp d;
    CHECK(d.Start(cfg, NowMs()));
    const uint64_t t2 = RunFinished(d, 3000);
    CHECK(d.GetState() == NatPmp::FAILED && t2 >= 480 && t2 < 800);
}

static void TestMapSilence() {   // answers the address, then goes quiet: the mapping exchange has its own budget
    Clear();
    FakeGw f;
    CHECK(f.Start());
    NatPmpConfig cfg = Cfg(f);
    cfg.mapMs = 600;
    NatPmp c;
    CHECK(c.Start(cfg, NowMs()));
    RunUntil(c, 2000, [&] { return c.GetState() == NatPmp::MAPPING || c.Finished(); });
    CHECK(c.GetState() == NatPmp::MAPPING || c.GetState() == NatPmp::MAPPED);
    f.mu.lock();
    f.silent = true;
    f.mu.unlock();
    const uint64_t took = RunFinished(c, 3000);
    if (c.GetState() == NatPmp::FAILED) {
        CHECK(took < 900);
        CHECK(strstr(c.Error(), "did not answer the mapping") != nullptr);
    }
}

static void TestRetransmission() {
    Clear();
    FakeGw f;
    f.dropFirst = 2;   // the first two requests of each kind are lost
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    const uint64_t took = RunMapped(c, 4000);
    CHECK(c.GetState() == NatPmp::MAPPED);
    CHECK(took >= 700 && took < 2500);   // answered by the third round (750 ms), then the mapping request's own third round
    CHECK(f.Count("pmp-addr") <= 4);
}

static void TestWrongSource() {
    Clear();
    FakeGw f;
    f.pcp = true;
    f.rogueOnly = true;   // valid answers, from another address and from another port; the real gateway is silent
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    const uint64_t took = RunFinished(c, 5000);
    CHECK(c.GetState() == NatPmp::FAILED);
    CHECK(took < 3300);
    CHECK(c.ExternalPort() == 0 && c.ExternalIp() == 0);
    // with the real gateway answering too, the mapping is the real one
    Clear();
    FakeGw g;
    g.rogue = true;
    g.extIp = 0xCB007108;
    CHECK(g.Start());
    NatPmp d;
    CHECK(d.Start(Cfg(g), NowMs()));
    RunMapped(d, 3000);
    CHECK(d.GetState() == NatPmp::MAPPED && d.ExternalIp() == 0xCB007108u);
}

static void TestMangledReplies() {
    for (int kind = 1; kind <= 6; ++kind) {
        for (int pcp = 0; pcp < 2; ++pcp) {
            Clear();
            FakeGw f;
            f.pcp = pcp != 0;
            f.junkBefore = kind;   // a mangled copy first, then the real answer: the real mapping must come out
            CHECK(f.Start());
            NatPmp c;
            CHECK(c.Start(Cfg(f), NowMs()));
            RunMapped(c, 3500);
            CHECK(c.GetState() == NatPmp::MAPPED);
            CHECK(c.ExternalPort() == 41100 && c.ExternalIp() == 0xCB007107u);
        }
    }
}

static void TestJunkOnly() {   // a gateway that only ever sends mangled replies
    struct Case { const char* name; std::vector<uint8_t> bytes; };
    std::vector<Case> cases;
    {
        std::vector<uint8_t> a(12, 0);   // address reply, version 0 op 128, result 0, but 12 bytes of zeros (ext 0.0.0.0)
        a[1] = 128;
        cases.push_back({"zero address", a});
        std::vector<uint8_t> b(10, 0);   // truncated
        b[1] = 128;
        cases.push_back({"truncated", b});
        std::vector<uint8_t> o(1400, 0);   // oversized
        o[1] = 128;
        cases.push_back({"oversized", o});
        std::vector<uint8_t> v(12, 0);   // version 1
        v[0] = 1; v[1] = 128;
        cases.push_back({"version 1", v});
        std::vector<uint8_t> q(12, 0);   // not a response (high bit clear on the opcode)
        q[1] = 0;
        cases.push_back({"opcode without the response bit", q});
    }
    for (auto& cs : cases) {
        Clear();
        FakeGw f;
        f.silent = true;
        CHECK(f.Start());
        const int s = FakeGw::Bind("127.0.0.1", 0);
        sockaddr_in a;
        socklen_t l = sizeof a;
        getsockname(s, (sockaddr*)&a, &l);
        // the client talks to port `a.port`, where only this test's raw socket answers with junk
        NatPmpConfig cfg;
        cfg.internalPort = 41100;
        cfg.internalIp = 0x7F000001;
        cfg.gateway = NpNet::Addr(0x7F000001, ntohs(a.sin_port));
        cfg.probeMs = 600;
        cfg.log = LogLine;
        NatPmp c;
        CHECK(c.Start(cfg, NowMs()));
        const uint64_t t0 = NowMs();
        while (NowMs() - t0 < 1500 && !c.Finished()) {
            c.Step(NowMs());
            uint8_t in[256];
            sockaddr_in from;
            socklen_t fl = sizeof from;
            const ssize_t n = recvfrom(s, in, sizeof in, MSG_DONTWAIT, (sockaddr*)&from, &fl);
            if (n > 0) sendto(s, cs.bytes.data(), cs.bytes.size(), 0, (sockaddr*)&from, fl);
            SleepMs(2);
        }
        if (c.GetState() != NatPmp::FAILED) fprintf(stderr, "junk case %s not failed (state %s)\n", cs.name, NatPmp::StateName(c.GetState()));
        CHECK(c.GetState() == NatPmp::FAILED);
        CHECK(c.ExternalPort() == 0);
        close(s);
    }
}

static void TestRenewal() {
    for (int pcp = 0; pcp < 2; ++pcp) {
        Clear();
        FakeGw f;
        f.pcp = pcp != 0;
        f.maxLease = 2;   // the router shortens the lease to 2 s: renew at 1 s
        CHECK(f.Start());
        NatPmpConfig cfg = Cfg(f);
        cfg.leaseSec = 600;
        NatPmp c;
        CHECK(c.Start(cfg, NowMs()));
        RunMapped(c, 3000);
        CHECK(c.GetState() == NatPmp::MAPPED);
        CHECK(c.Lease() == 2);
        RunUntil(c, 3500, [&] { return c.Renewals() >= 2 || c.Finished(); });
        CHECK(c.Renewals() >= 2);
        CHECK(c.GetState() == NatPmp::MAPPED || c.GetState() == NatPmp::RENEWING);
        CHECK(c.ExternalPort() == 41100);
        CHECK(f.Count(pcp ? "pcp-map int=41100 ext=41100 life=600" : "pmp-map int=41100 ext=41100 life=600") >= 3);   // first + two renewals
        CHECK(LogHas("renewed (lease 2 s)") >= 2);
    }
}

static void TestRenewalLost() {   // the router stops answering: the mapping runs out with its lease and the client reports it
    Clear();
    FakeGw f;
    f.maxLease = 2;
    CHECK(f.Start());
    NatPmpConfig cfg = Cfg(f);
    cfg.mapMs = 400;
    NatPmp c;
    CHECK(c.Start(cfg, NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED);
    f.mu.lock();
    f.silent = true;
    f.mu.unlock();
    RunFinished(c, 5000);
    CHECK(c.GetState() == NatPmp::FAILED);
    CHECK(c.ExternalPort() == 0);
}

static void TestPortChangedOnRenewal() {
    Clear();
    FakeGw f;
    f.maxLease = 2;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 3000);
    CHECK(c.ExternalPort() == 41100);
    f.mu.lock();
    f.takenFirst = true;   // the port got taken meanwhile: the router answers the renewal with another one
    f.mu.unlock();
    RunUntil(c, 3000, [&] { return c.ExternalPort() != 41100; });
    CHECK(c.ExternalPort() == 41101);
    CHECK(LogHas("the external port changed 41100 -> 41101") == 1);
}

static void TestEpoch() {
    for (int pcp = 0; pcp < 2; ++pcp) {
        Clear();
        FakeGw f;
        f.pcp = pcp != 0;
        f.epochFixed = 1000;
        CHECK(f.Start());
        NatPmpConfig cfg = Cfg(f);
        cfg.pollMs = 150;
        NatPmp c;
        CHECK(c.Start(cfg, NowMs()));
        RunMapped(c, 3000);
        CHECK(c.GetState() == NatPmp::MAPPED);
        const char* mapPrefix = pcp ? "pcp-map" : "pmp-map";
        const int before = f.Count(mapPrefix);
        // epoch goes on normally for a while: polls, no new mapping request
        f.mu.lock();
        f.epochFixed = 0;
        f.epochBias = 1000;
        f.mu.unlock();
        RunUntil(c, 900, [&] { return false; });
        CHECK(f.Count(pcp ? "pcp-announce" : "pmp-addr") >= (pcp ? 1 : 2));
        // (a normal clock: no restart noticed) ... then the router restarts: its epoch starts over
        const int normal = f.Count(mapPrefix);
        CHECK(normal == before);
        f.mu.lock();
        f.epochBias = -int64_t((NowMs() - f.startMs) / 1000);   // epoch ~ 0 again
        f.mu.unlock();
        RunUntil(c, 1500, [&] { return f.Count(mapPrefix) > normal; });
        CHECK(f.Count(mapPrefix) > normal);
        CHECK(LogHas("the router restarted") >= 1);
        CHECK(c.GetState() == NatPmp::MAPPED || c.GetState() == NatPmp::RENEWING);
    }
}

static void TestEpochRule() {
    CHECK(!NatPmp::EpochRestarted(100, 100, 0));
    CHECK(!NatPmp::EpochRestarted(100, 160, 60000));
    CHECK(!NatPmp::EpochRestarted(100, 158, 60000));    // a little slow: within 7/8 - 2 s
    CHECK(NatPmp::EpochRestarted(100, 50, 10000));      // went back
    CHECK(NatPmp::EpochRestarted(5000, 20, 60000));
    CHECK(NatPmp::EpochRestarted(100, 110, 60000));     // ran far too slow
    CHECK(NatPmp::EpochRestarted(100, 400, 60000));     // jumped ahead
    CHECK(!NatPmp::EpochRestarted(0, 0, 500));
}

static void TestCgnat() {
    struct Case { uint32_t ip; bool priv; bool fail; } cases[] = {
        {0x64400101, true, false},    // 100.64.1.1 carrier-grade NAT
        {0xC0A80105, true, false},    // 192.168.1.5
        {0x0A000001, true, false},    // 10.0.0.1
        {0xCB007107, false, false},
        {0x00000000, false, true},    // no address yet
        {0x7F000001, false, true},    // loopback
    };
    for (int pcp = 0; pcp < 2; ++pcp) {
        for (auto& cs : cases) {
            Clear();
            FakeGw f;
            f.pcp = pcp != 0;
            f.extIp = cs.ip;
            CHECK(f.Start());
            NatPmp c;
            CHECK(c.Start(Cfg(f), NowMs()));
            RunFinished(c, 500);
            RunMapped(c, 3000);
            if (cs.fail) {
                CHECK(c.GetState() == NatPmp::FAILED);
                CHECK(strstr(c.Error(), "no usable public address") != nullptr);
            } else {
                CHECK(c.GetState() == NatPmp::MAPPED);
                CHECK(c.ExternalIsPrivate() == cs.priv);
            }
        }
    }
}

static void TestBadGrants() {
    for (int what = 0; what < 2; ++what) {
        for (int pcp = 0; pcp < 2; ++pcp) {
            Clear();
            FakeGw f;
            f.pcp = pcp != 0;
            if (what == 0) f.zeroLease = 1;
            else f.zeroExtPort = true;
            CHECK(f.Start());
            NatPmp c;
            CHECK(c.Start(Cfg(f), NowMs()));
            RunFinished(c, 4000);
            CHECK(c.GetState() == NatPmp::FAILED);
            CHECK(c.ExternalPort() == 0);
        }
    }
}

static void TestPortTaken() {
    Clear();
    FakeGw f;
    f.takenFirst = true;   // the router gives another external port than the suggested one
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    RunMapped(c, 3000);
    CHECK(c.GetState() == NatPmp::MAPPED && c.ExternalPort() == 41101 && c.InternalPort() == 41100);
}

static void TestRemoveEarly() {
    // Remove while probing with PCP on the wire: a lifetime-0 PCP request goes out (the create may have landed)
    Clear();
    FakeGw f;
    f.pcp = true;
    f.dropFirst = 1;
    CHECK(f.Start());
    NatPmp c;
    CHECK(c.Start(Cfg(f), NowMs()));
    c.Step(NowMs());
    SleepMs(30);
    c.Remove(NowMs());
    RunFinished(c, 2000);
    CHECK(c.GetState() == NatPmp::REMOVED);
    CHECK(f.Count("pcp-map int=41100 ext=41100 life=0") >= 1);
    CHECK(f.TableSize() == 0);
    // Remove before anything went out: nothing to delete
    NatPmp d;
    FakeGw g;
    CHECK(g.Start());
    NatPmpConfig cfg = Cfg(g);
    CHECK(d.Start(cfg, NowMs()));
    d.Abort();
    CHECK(d.Finished());
    // Remove while the router is silent ends within the delete budget
    Clear();
    FakeGw h;
    CHECK(h.Start());
    NatPmp e;
    CHECK(e.Start(Cfg(h), NowMs()));
    RunMapped(e, 3000);
    h.mu.lock();
    h.silent = true;
    h.mu.unlock();
    e.Remove(NowMs());
    const uint64_t took = RunFinished(e, 3000);
    CHECK(e.GetState() == NatPmp::REMOVED && took >= 900 && took < 1300);
}

static void TestNoGateway() {
    NatPmpConfig c;
    c.internalPort = 0;
    NatPmp n;
    CHECK(!n.Start(c, NowMs()));
    CHECK(n.GetState() == NatPmp::FAILED);
    // the gateway rule: a platform answer or the .1 of the /24
    bool guessed = false;
    const uint32_t g = NatPmp::DefaultGateway(0xC0A80709, &guessed);
    if (guessed) CHECK(g == 0xC0A80701u);
    else CHECK(g != 0 && g != 0xC0A80709u);
    guessed = false;
    CHECK(NatPmp::DefaultGateway(0xC0A80701, &guessed) != 0xC0A80701u);   // never itself
}

// ---- the codec on hostile bytes -----------------------------------------------------------------------------------------------------
static void TestCodec() {
    uint8_t b[64];
    NatPmp::Reply r;
    CHECK(NatPmp::BuildPmpAddrRequest(b) == 2 && b[0] == 0 && b[1] == 0);
    CHECK(NatPmp::BuildPmpMapRequest(b, 0xA1B2, 0xC3D4, 0x01020304) == 12 && b[1] == 1 && b[4] == 0xA1 && b[7] == 0xD4 && b[8] == 1 && b[11] == 4);
    uint8_t nonce[12];
    for (int i = 0; i < 12; ++i) nonce[i] = uint8_t(i + 1);
    CHECK(NatPmp::BuildPcpMapRequest(b, 0xC0A80109, nonce, 41100, 41100, 0, 7200) == 60);
    CHECK(b[0] == 2 && b[1] == 1 && R32(b + 4) == 7200 && b[18] == 0xFF && b[19] == 0xFF && R32(b + 20) == 0xC0A80109);
    CHECK(memcmp(b + 24, nonce, 12) == 0 && b[36] == 17 && R16(b + 40) == 41100 && R16(b + 42) == 41100);
    CHECK(NatPmp::BuildPcpAnnounce(b, 0x01020304) == 24 && b[1] == 0);
    // NAT-PMP replies
    const uint8_t okAddr[12] = {0, 128, 0, 0, 0, 0, 0, 9, 203, 0, 113, 7};
    CHECK(NatPmp::ParseReply(okAddr, 12, &r) && r.kind == NatPmp::K_PMP_ADDR && r.result == 0 && r.epoch == 9 && r.extIp == 0xCB007107u);
    CHECK(!NatPmp::ParseReply(okAddr, 11, &r) && !NatPmp::ParseReply(okAddr, 13, &r) && !NatPmp::ParseReply(okAddr, 0, &r));
    const uint8_t okMap[16] = {0, 129, 0, 0, 0, 0, 0, 9, 0xA0, 0x8C, 0xA0, 0x8D, 0, 0, 0x1C, 0x20};
    CHECK(NatPmp::ParseReply(okMap, 16, &r) && r.kind == NatPmp::K_PMP_MAP && r.intPort == 41100 && r.extPort == 41101 && r.lifetime == 7200);
    CHECK(!NatPmp::ParseReply(okMap, 15, &r) && !NatPmp::ParseReply(okMap, 17, &r));
    uint8_t err[16];
    memcpy(err, okMap, 16);
    err[3] = 2;   // an error may be short (8 bytes up) or full
    CHECK(NatPmp::ParseReply(err, 8, &r) && r.kind == NatPmp::K_PMP_MAP && r.result == 2 && r.extPort == 0);
    CHECK(NatPmp::ParseReply(err, 16, &r) && r.result == 2);
    CHECK(!NatPmp::ParseReply(err, 7, &r) && !NatPmp::ParseReply(err, 17, &r));
    uint8_t v[16];
    memcpy(v, okMap, 16);
    v[0] = 1;
    CHECK(!NatPmp::ParseReply(v, 16, &r));
    memcpy(v, okMap, 16);
    v[1] = 130;   // TCP mapping answer: not ours
    CHECK(!NatPmp::ParseReply(v, 16, &r));
    // PCP replies
    uint8_t p[60];
    memset(p, 0, sizeof p);
    p[0] = 2; p[1] = 0x81;
    W32(p + 4, 7200); W32(p + 8, 77);
    memcpy(p + 24, nonce, 12);
    p[36] = 17;
    W16(p + 40, 41100); W16(p + 42, 5555);
    p[54] = p[55] = 0xFF;
    W32(p + 56, 0xCB007107);
    CHECK(NatPmp::ParseReply(p, 60, &r) && r.kind == NatPmp::K_PCP_MAP && r.result == 0 && r.lifetime == 7200 && r.epoch == 77 && r.extPort == 5555 &&
          r.extIpOk && r.extIp == 0xCB007107u && memcmp(r.nonce, nonce, 12) == 0);
    CHECK(!NatPmp::ParseReply(p, 59, &r) && !NatPmp::ParseReply(p, 56, &r) && !NatPmp::ParseReply(p, 62, &r) && !NatPmp::ParseReply(p, 1104, &r));
    uint8_t q[60];
    memcpy(q, p, 60);
    q[1] = 0x01;   // R bit clear: a request, not a reply
    CHECK(!NatPmp::ParseReply(q, 60, &r));
    memcpy(q, p, 60);
    q[1] = 0x83;   // unknown opcode
    CHECK(!NatPmp::ParseReply(q, 60, &r));
    memcpy(q, p, 60);
    q[36] = 6;     // protocol TCP
    CHECK(!NatPmp::ParseReply(q, 60, &r));
    memcpy(q, p, 60);
    q[50] = 1;     // an IPv6 external address: parsed, flagged as not IPv4
    CHECK(NatPmp::ParseReply(q, 60, &r) && !r.extIpOk);
    memcpy(q, p, 60);
    q[0] = 3;
    CHECK(!NatPmp::ParseReply(q, 60, &r));
    uint8_t u[24];
    memset(u, 0, sizeof u);
    u[0] = 2; u[1] = 0x81; u[3] = 1;   // UNSUPP_VERSION: header only
    CHECK(NatPmp::ParseReply(u, 24, &r) && r.kind == NatPmp::K_PCP_MAP && r.result == 1);
    u[3] = 2;
    CHECK(!NatPmp::ParseReply(u, 24, &r));   // any other MAP answer must carry the body (nonce, ports)
    u[1] = 0x80; u[3] = 0;
    CHECK(NatPmp::ParseReply(u, 24, &r) && r.kind == NatPmp::K_PCP_ANNOUNCE);
    CHECK(strlen(NatPmp::PmpResultText(77)) > 0 && strlen(NatPmp::PcpResultText(99)) > 0);
    // fuzz: random bytes and mutated real replies, every length
    std::mt19937 rng(7);
    for (int i = 0; i < 200000; ++i) {
        uint8_t m[1200];
        const int pick = int(rng() % 4);
        const size_t len = rng() % 1150;
        for (size_t k = 0; k < len; ++k) m[k] = uint8_t(rng());
        const uint8_t* src = pick == 0 ? okAddr : pick == 1 ? okMap : pick == 2 ? p : u;
        const size_t srcLen = pick == 0 ? 12 : pick == 1 ? 16 : pick == 2 ? 60 : 24;
        if (pick != 3 || (i & 1)) {
            memcpy(m, src, srcLen < len ? srcLen : len);
            for (int k = 0; k < 3; ++k)
                if (len) m[rng() % len] = uint8_t(rng());
        }
        if (NatPmp::ParseReply(m, len, &r)) {
            CHECK(r.kind != NatPmp::K_NONE);
            CHECK(len >= 8);
        }
    }
}

int main() {
    TestCodec();
    TestEpochRule();
    TestNoGateway();
    TestPmpOnly();
    TestPcp();
    TestPcpOnly();
    TestNoPcpConfig();
    TestPcpUnsuppFallsBack();
    TestPmpErrorCodes();
    TestPcpErrorCodes();
    TestPcpCannotProvide();
    TestAddressMismatch();
    TestSilence();
    TestMapSilence();
    TestRetransmission();
    TestWrongSource();
    TestMangledReplies();
    TestJunkOnly();
    TestRenewal();
    TestRenewalLost();
    TestPortChangedOnRenewal();
    TestEpoch();
    TestCgnat();
    TestBadGrants();
    TestPortTaken();
    TestRemoveEarly();
    printf("natpmp_test: %d checks, %d failures\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
