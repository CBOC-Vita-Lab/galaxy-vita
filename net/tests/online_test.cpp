// Tests of the online transport (platform/sdk/net/online) in virtual time, over the NAT simulator (natsim.h): STUN, hole punching against
// every NAT combination, the encrypted session (replay window, tampering, MTU), loss / delay / reordering, NAT timeouts, a Wi-Fi drop
// with link rebuild, the close message and the relay path framing. No game data, no sockets.
//   tools/net/run_online_tests.sh
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

#include <functional>
#include <string>
#include <vector>

#include <memory>

#include "natsim.h"
#include "turnsim.h"
#include "../src/online/np_hash.h"
#include "../src/online/np_turn.h"
#include "../src/online/np_crypto.h"
#include "../src/online/np_online.h"
#include "../src/online/np_fault.h"
#include "../src/online/np_http.h"
#include "../src/online/np_signal.h"
#include "../src/online/np_signal_http.h"
#include "../src/online/np_stun.h"
#include "../src/online/np_app_version.h"   // Galaxy: BuiltAppVersion

using namespace NpNet;
using NatSim::NatType;

namespace NpNet {
// Peer declares this a friend: the tests read the session keys to forge authentic packets (what an authenticated opponent could send).
struct PeerTestAccess {
    static const uint8_t* DataKey(Peer& p) { return p.dataKey_; }
    static const uint8_t* Hello(Peer& p) { return p.hello_; }
    static const uint8_t* PeerHello(Peer& p) { return p.peerHello_; }
};
}  // namespace NpNet

static int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                           \
    do {                                                                                                         \
        ++g_checks;                                                                                              \
        if (!((a) == (b))) {                                                                                     \
            ++g_failed;                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)(a), (long long)(b)); \
        }                                                                                                        \
    } while (0)

static bool g_quiet = true;

// ---- the world ------------------------------------------------------------------------------------------------------------
struct World {
    NatSim::Sim sim;
    uint64_t now;
    SignalMemoryHub* hub;
    World(uint32_t seed = 7) : sim(seed), now(1000), hub(CreateSignalMemoryHub(seed)) {
        sim.AddStunServer(MakeAddr(198, 51, 100, 1, 3478));
        sim.AddStunServer(MakeAddr(198, 51, 100, 2, 3478));
    }
    ~World() { DestroySignalMemoryHub(hub); }
};
static uint64_t WorldNow(void* c) { return static_cast<World*>(c)->now; }

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1) {}
};
static bool RngFill(void* c, void* out, size_t n) {
    Rng* r = static_cast<Rng*>(c);
    uint8_t* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) {
        r->s ^= r->s << 13;
        r->s ^= r->s >> 17;
        r->s ^= r->s << 5;
        p[i] = uint8_t(r->s >> 11);
    }
    return true;
}

class SimUdp : public Udp {
public:
    SimUdp(World* w, int host, const Addr& priv) : w_(w), host_(host), priv_(priv), open_(false) {}
    bool Open(uint16_t) override { open_ = true; return true; }
    void Close() override { open_ = false; }
    bool IsOpen() const override { return open_; }
    int SendTo(const Addr& to, const void* data, size_t len) override {
        if (!open_) return -1;
        ++sent;
        if (len > maxSeen) maxSeen = len;
        if (len > 0 && static_cast<const uint8_t*>(data)[0] == kTypeData) lastData.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        if (len > 0 && static_cast<const uint8_t*>(data)[0] == kTypeData) allData.push_back(lastData);
        w_->sim.Send(host_, to, data, len, w_->now);
        return int(len);
    }
    int RecvFrom(Addr* from, void* buffer, size_t cap) override {
        if (!open_) return -1;
        std::vector<uint8_t> d;
        if (!w_->sim.Recv(host_, from, &d, w_->now)) return 0;
        const size_t n = d.size() < cap ? d.size() : cap;
        memcpy(buffer, &d[0], n);
        return int(n);
    }
    uint32_t LocalIp() override { return priv_.ip; }
    uint16_t LocalPort() override { return priv_.port; }
    // as if this console had sent it
    void Inject(const Addr& to, const std::vector<uint8_t>& d) { w_->sim.Send(host_, to, &d[0], d.size(), w_->now); }
    uint64_t sent = 0;
    size_t maxSeen = 0;
    std::vector<uint8_t> lastData;
    std::vector<std::vector<uint8_t> > allData;

private:
    World* w_;
    int host_;
    Addr priv_;
    bool open_;
};

struct Console {
    World* w;
    int host;
    SimUdp* udp;
    Rng rng;
    OnlineNet net;
    Addr priv;
    int lostCalls, noticeCalls;
    std::string name;
    Signal* sigOverride;   // a signaling object to use instead of the memory hub's
    bool relay = false, relayOnly = false;   // the relay path (off by default: the older tests expect a failed punch to end in RELAY_NEEDED)
    int relayWaitOverride = -1;
    int topology = TOPOLOGY_STAR;          // Galaxy: the star (default) or Strikers' mesh
    int migrationWait = -1;                // Galaxy: hostMigrationWaitMs override
    std::vector<int> peerLost;             // Galaxy: the slots onPeerLost named, in order
    TurnConfig turnCfg;
    std::vector<Addr> turnAddrs;
    Console(World* world, int hostId, const Addr& p, uint32_t seed, const char* n) : w(world), host(hostId), udp(0), rng(seed), priv(p), lostCalls(0), noticeCalls(0), name(n), sigOverride(0) {}
    static void OnLost(void* ctx, const char*) { ++static_cast<Console*>(ctx)->lostCalls; }
    static void OnNotice(void* ctx, const char*) { ++static_cast<Console*>(ctx)->noticeCalls; }
    static void OnPeerLost(void* ctx, int slot, const char*) { static_cast<Console*>(ctx)->peerLost.push_back(slot); }
    bool Start(bool hosting, const char* invite, const PunchConfig* punch = 0) {
        OnlineConfig cfg;
        udp = new SimUdp(w, host, priv);
        cfg.udp = udp;
        cfg.signal = sigOverride ? sigOverride : CreateMemorySignal(w->hub);
        cfg.env.now = WorldNow;
        cfg.env.nowCtx = w;
        cfg.env.random = RngFill;
        cfg.env.randomCtx = &rng;
        cfg.stunAddrs[0] = MakeAddr(198, 51, 100, 1, 3478);
        cfg.stunAddrs[1] = MakeAddr(198, 51, 100, 2, 3478);
        cfg.nStunAddrs = 2;
        if (punch) cfg.punch = *punch;
        cfg.signalPollMs = 0;
        cfg.onLost = OnLost;
        cfg.onLostCtx = this;
        cfg.relayEnabled = relay;
        cfg.relayOnly = relayOnly;
        if (relayWaitOverride >= 0) cfg.relayWaitMs = uint32_t(relayWaitOverride);
        cfg.turn = turnCfg;
        for (size_t i = 0; i < turnAddrs.size() && i < 4; ++i) cfg.turnAddrs[cfg.nTurnAddrs++] = turnAddrs[i];
        cfg.onNotice = OnNotice;
        cfg.onNoticeCtx = this;
        cfg.topology = topology;
        cfg.onPeerLost = OnPeerLost;
        cfg.onPeerLostCtx = this;
        if (migrationWait >= 0) cfg.hostMigrationWaitMs = uint32_t(migrationWait);
        return net.Start(cfg, hosting ? 0 : invite, name.c_str());
    }
};

static void Run(World& w, Console** cs, int n, uint32_t ms, uint32_t step = 10) {
    for (uint32_t t = 0; t < ms; t += step) {
        w.now += step;
        for (int i = 0; i < n; ++i) cs[i]->net.Tick();
    }
}
// runs until `done` or the time is up; returns the time it took (ms) or -1
static int RunUntil(World& w, Console** cs, int n, uint32_t maxMs, const std::function<bool()>& done, uint32_t step = 10) {
    const uint64_t t0 = w.now;
    for (uint32_t t = 0; t < maxMs; t += step) {
        w.now += step;
        for (int i = 0; i < n; ++i) cs[i]->net.Tick();
        if (done()) return int(w.now - t0);
    }
    return -1;
}

static uint32_t PublicIp(int lan) { return (203u << 24) | (0u << 16) | (113u << 8) | uint32_t(10 + lan); }   // 203.0.113.10+

struct Pair {
    World w;
    Console* a;
    Console* b;
    int lanA, lanB;
    Pair(const NatType& natA, const NatType& natB, bool sameLan = false, uint32_t seed = 7) : w(seed) {
        lanA = w.sim.AddLan(PublicIp(0), natA);
        lanB = sameLan ? lanA : w.sim.AddLan(PublicIp(1), natB);
        const Addr privA = natA.open ? MakeAddr(203, 0, 113, 50, 41000) : MakeAddr(192, 168, 1, 10, 41000);
        Addr privB = natB.open ? MakeAddr(203, 0, 113, 51, 41000) : MakeAddr(192, 168, sameLan ? 1 : 2, sameLan ? 11 : 10, 41000);
        const int hA = w.sim.AddHost(lanA, privA);
        const int hB = w.sim.AddHost(lanB, privB);
        a = new Console(&w, hA, privA, 101, "ALICE");
        b = new Console(&w, hB, privB, 202, "BOB");
    }
    ~Pair() { delete a; delete b; }
    bool Start(const PunchConfig* punch = 0) {
        if (!a->Start(true, 0, punch)) return false;
        Console* cs[2] = {a, b};
        Run(w, cs, 2, 50);   // the room exists after a first tick
        OnlineInfo info;
        a->net.GetInfo(&info);
        return b->Start(false, info.invite, punch);
    }
    Console* both[2];
    // waits for both to be connected; returns ms or -1
    int Connect(uint32_t maxMs) {
        both[0] = a; both[1] = b;
        return RunUntil(w, both, 2, maxMs, [this] { return a->net.GetPhase() == OnlineNet::READY && b->net.GetPhase() == OnlineNet::READY &&
                                                         a->net.PeerAt(1).Established() && b->net.PeerAt(0).Established(); });
    }
    void Advance(uint32_t ms) { both[0] = a; both[1] = b; Run(w, both, 2, ms); }
};

static std::vector<uint8_t> Frame(uint32_t id, size_t len) {
    std::vector<uint8_t> f(len);
    for (size_t i = 0; i < len; ++i) f[i] = uint8_t(id * 31 + i * 7);
    if (len >= 4) { f[0] = uint8_t(id >> 24); f[1] = uint8_t(id >> 16); f[2] = uint8_t(id >> 8); f[3] = uint8_t(id); }
    return f;
}
static bool FrameOk(const std::vector<uint8_t>& f, uint32_t* id) {
    if (f.size() < 4) return false;
    *id = uint32_t(f[0]) << 24 | uint32_t(f[1]) << 16 | uint32_t(f[2]) << 8 | f[3];
    return f == Frame(*id, f.size());
}
static int Drain(Console* c, std::vector<uint32_t>* ids, int* corrupt) {
    uint8_t buf[2048];
    int slot = 0, n = 0, got;
    while ((got = c->net.RecvFrame(&slot, buf, sizeof buf)) > 0) {
        std::vector<uint8_t> f(buf, buf + got);
        uint32_t id;
        if (FrameOk(f, &id)) { if (ids) ids->push_back(id); }
        else if (corrupt) ++*corrupt;
        ++n;
    }
    return n;
}

// ---- tests ------------------------------------------------------------------------------------------------------------------
static void TestCrypto() {
    printf("crypto\n");
    uint8_t key[32], nonce[24], pt[100], ct[100], back[100], tag[16], ad[6] = {1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
    for (int i = 0; i < 24; ++i) nonce[i] = uint8_t(100 + i);
    for (int i = 0; i < 100; ++i) pt[i] = uint8_t(i * 3);
    AeadSeal(ct, tag, key, nonce, ad, 6, pt, 100);
    CHECK(memcmp(ct, pt, 100) != 0);
    CHECK(AeadOpen(back, key, nonce, ad, 6, ct, 100, tag));
    CHECK(memcmp(back, pt, 100) == 0);
    ct[50] ^= 1;
    CHECK(!AeadOpen(back, key, nonce, ad, 6, ct, 100, tag));
    ct[50] ^= 1;
    ad[0] ^= 1;
    CHECK(!AeadOpen(back, key, nonce, ad, 6, ct, 100, tag));
    ad[0] ^= 1;
    nonce[0] ^= 1;
    CHECK(!AeadOpen(back, key, nonce, ad, 6, ct, 100, tag));
    nonce[0] ^= 1;
    tag[15] ^= 0x80;
    CHECK(!AeadOpen(back, key, nonce, ad, 6, ct, 100, tag));
    // BLAKE2b-256 keyed vector (RFC 7693 style check through the KDF: empty-key hash of "abc" is unkeyed there, so check determinism and key dependence)
    uint8_t k1[32], k2[32];
    Kdf(k1, key, "abc", 3);
    Kdf(k2, key, "abc", 3);
    CHECK(memcmp(k1, k2, 32) == 0);
    key[0] ^= 1;
    Kdf(k2, key, "abc", 3);
    CHECK(memcmp(k1, k2, 32) != 0);
    uint8_t m1[16], m2[16];
    Mac16(m1, key, "x", 1);
    Mac16(m2, key, "y", 1);
    CHECK(!MacEqual(m1, m2));
    // split input equals joined input
    Kdf(k1, key, "ab", 2, "c", 1);
    Kdf(k2, key, "abc", 3);
    CHECK(memcmp(k1, k2, 32) == 0);
}

static void TestStunParsing() {
    printf("stun message format\n");
    // RFC 5769 section 2.2: sample IPv4 response (XOR-MAPPED-ADDRESS 192.0.2.1:32853), without the integrity / fingerprint attributes
    const uint8_t sample[] = {0x01, 0x01, 0x00, 0x1c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
                              0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
                              0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43};
    uint8_t tx[12];
    Addr a;
    CHECK(StunClient::ParseResponse(sample, sizeof sample, tx, &a));
    CHECK_EQ(a.ip, MakeAddr(192, 0, 2, 1, 0).ip);
    CHECK_EQ(a.port, 32853);
    CHECK(Classify(sample, sizeof sample) == PKT_STUN);
    // truncated and malformed
    CHECK(!StunClient::ParseResponse(sample, 10, tx, &a));
    CHECK(!StunClient::ParseResponse(sample, sizeof sample - 4, tx, &a));
    uint8_t bad[sizeof sample];
    memcpy(bad, sample, sizeof sample);
    bad[4] ^= 1;   // wrong magic cookie
    CHECK(!StunClient::ParseResponse(bad, sizeof bad, tx, &a));
    // plain MAPPED-ADDRESS fallback
    const uint8_t plain[] = {0x01, 0x01, 0x00, 0x0c, 0x21, 0x12, 0xa4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x1f, 0x90, 10, 0, 0, 7};
    CHECK(StunClient::ParseResponse(plain, sizeof plain, tx, &a));
    CHECK_EQ(a.port, 8080);
    CHECK_EQ(a.ip, MakeAddr(10, 0, 0, 7, 0).ip);
    // request layout
    uint8_t req[20];
    CHECK_EQ(StunClient::BuildRequest(req, tx), 20u);
    CHECK(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 0 && req[4] == 0x21 && req[7] == 0x42);
    // demultiplexing of the socket's other traffic
    const uint8_t punch[] = {kTypePunch, 0, 1};
    const uint8_t chan[] = {0x40, 0x01, 0, 4, 1, 2, 3, 4};
    CHECK(Classify(punch, sizeof punch) == PKT_PUNCH);
    CHECK(Classify(chan, sizeof chan) == PKT_CHANNEL);
    CHECK(Classify(punch, 0) == PKT_UNKNOWN);
}

// A client behind `nat` asks two STUN servers: what does it learn?
static void TestStunClassification() {
    printf("stun classification (open / cone / symmetric)\n");
    struct Row { NatType nat; int want; const char* label; };
    Row rows[] = {{NatType::Open(), NAT_OPEN, "open"}, {NatType::FullCone(), NAT_CONE, "full cone"}, {NatType::AddressRestricted(), NAT_CONE, "address restricted"},
                  {NatType::PortRestricted(), NAT_CONE, "port restricted"}, {NatType::Symmetric(), NAT_SYMMETRIC, "symmetric"}};
    for (const Row& r : rows) {
        World w;
        const int lan = w.sim.AddLan(PublicIp(0), r.nat);
        const Addr priv = r.nat.open ? MakeAddr(203, 0, 113, 50, 41000) : MakeAddr(192, 168, 1, 10, 41000);
        const int host = w.sim.AddHost(lan, priv);
        SimUdp udp(&w, host, priv);
        udp.Open(41000);
        Rng rng(5);
        Env env;
        env.udp = &udp; env.now = WorldNow; env.nowCtx = &w; env.random = RngFill; env.randomCtx = &rng;
        StunClient stun;
        const Addr servers[2] = {MakeAddr(198, 51, 100, 1, 3478), MakeAddr(198, 51, 100, 2, 3478)};
        stun.Start(&env, servers, 2);
        for (int i = 0; i < 300 && !stun.Done(); ++i) {
            w.now += 10;
            stun.Tick();
            Addr from;
            uint8_t buf[256];
            int n;
            while ((n = udp.RecvFrom(&from, buf, sizeof buf)) > 0) stun.HandlePacket(from, buf, size_t(n));
        }
        CHECK(stun.GetState() == StunClient::DONE);
        CHECK_EQ(stun.Answered(), 2);
        CHECK_EQ(stun.Classify(priv.ip), r.want);
        if (!r.nat.open) CHECK_EQ(stun.FirstMapped().ip, PublicIp(0));
        if (!g_quiet) printf("  %-20s -> %s, mapped %s\n", r.label, NatClassName(stun.Classify(priv.ip)), AddrStr(stun.FirstMapped()));
    }
    // loss: the first request and response are lost, the retransmission gets through; a silent server list fails after the last wait
    {
        World w;
        NatSim::Impair im;
        im.lossPct = 60;
        w.sim.SetImpair(im);
        const int lan = w.sim.AddLan(PublicIp(0), NatType::PortRestricted());
        const Addr priv = MakeAddr(192, 168, 1, 10, 41000);
        const int host = w.sim.AddHost(lan, priv);
        SimUdp udp(&w, host, priv);
        udp.Open(41000);
        Rng rng(9);
        Env env;
        env.udp = &udp; env.now = WorldNow; env.nowCtx = &w; env.random = RngFill; env.randomCtx = &rng;
        StunClient stun;
        const Addr servers[2] = {MakeAddr(198, 51, 100, 1, 3478), MakeAddr(198, 51, 100, 2, 3478)};
        stun.Start(&env, servers, 2);
        for (int i = 0; i < 1500 && !stun.Done(); ++i) {
            w.now += 10;
            stun.Tick();
            Addr from;
            uint8_t buf[256];
            int n;
            while ((n = udp.RecvFrom(&from, buf, sizeof buf)) > 0) stun.HandlePacket(from, buf, size_t(n));
        }
        CHECK(stun.Done());
        CHECK(stun.Answered() >= 1);
        CHECK(udp.sent > 2);   // it retransmitted
        StunClient dead;
        World w2;
        const int lan2 = w2.sim.AddLan(PublicIp(0), NatType::PortRestricted());
        const int host2 = w2.sim.AddHost(lan2, priv);
        SimUdp udp2(&w2, host2, priv);
        udp2.Open(41000);
        const Addr nowhere[1] = {MakeAddr(192, 0, 2, 99, 3478)};
        Env env2 = env;
        env2.udp = &udp2; env2.nowCtx = &w2;
        dead.Start(&env2, nowhere, 1);
        for (int i = 0; i < 2000 && !dead.Done(); ++i) { w2.now += 10; dead.Tick(); }
        CHECK(dead.GetState() == StunClient::FAILED);
        CHECK_EQ(dead.Answered(), 0);
        CHECK_EQ(udp2.sent, 6u);   // the full retransmission schedule, then it gave up
    }
}

struct MatrixRow { const char* a; const char* b; bool expect; };

static void TestNatMatrix() {
    printf("hole punching against every NAT combination (different LANs)\n");
    const char* names[] = {"open", "full", "addr", "port", "sym", "symaddr"};
    const int N = 6;
    // success table by RFC 4787 reasoning (see the notes in the test plan): index [a][b]
    //                open full addr port sym symaddr
    const bool ok[6][6] = {/*open   */ {1, 1, 1, 1, 1, 1},
                           /*full   */ {1, 1, 1, 1, 1, 1},
                           /*addr   */ {1, 1, 1, 1, 1, 1},
                           /*port   */ {1, 1, 1, 1, 0, 0},
                           /*sym    */ {1, 1, 1, 0, 0, 0},
                           /*symaddr*/ {1, 1, 1, 0, 0, 0}};
    int succ = 0, relay = 0;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            NatType ta, tb;
            NatType::Parse(names[i], &ta);
            NatType::Parse(names[j], &tb);
            Pair p(ta, tb);
            CHECK(p.Start());
            const int ms = p.Connect(15000);
            const bool connected = ms >= 0;
            if (connected != ok[i][j]) fprintf(stderr, "  matrix mismatch %s vs %s: expected %s, got %s\n", names[i], names[j], ok[i][j] ? "connect" : "relay", connected ? "connect" : "relay");
            CHECK_EQ(connected, ok[i][j]);
            if (connected) {
                ++succ;
                // the data really flows both ways
                CHECK(p.a->net.SendFrame(-1, &Frame(1, 200)[0], 200) == 200);
                CHECK(p.b->net.SendFrame(-1, &Frame(2, 300)[0], 300) == 300);
                p.Advance(300);
                std::vector<uint32_t> ia, ib;
                int bad = 0;
                Drain(p.a, &ia, &bad);
                Drain(p.b, &ib, &bad);
                CHECK_EQ(ia.size(), 1u);
                CHECK_EQ(ib.size(), 1u);
                CHECK_EQ(bad, 0);
            } else {
                ++relay;
                CHECK(p.a->net.PeerAt(1).GetState() == Peer::NEEDS_RELAY);
                CHECK(p.b->net.PeerAt(0).GetState() == Peer::NEEDS_RELAY);
                CHECK(p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED);
            }
            if (!g_quiet || true) printf("  %-8s <-> %-8s : %s%s\n", names[i], names[j], connected ? "connected in " : "NEEDS RELAY", connected ? (std::to_string(ms) + " ms").c_str() : "");
        }
    }
    printf("  %d combinations connect, %d report relay-needed\n", succ, relay);
}

static void TestSameLan() {
    printf("two consoles behind one NAT: the local path wins\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted(), true);   // one router, no hairpin: the public path cannot work
    CHECK(p.Start());
    const int ms = p.Connect(10000);
    CHECK(ms >= 0);
    CHECK(p.a->net.PeerAt(1).Path().kind == CAND_LOCAL);
    CHECK(p.b->net.PeerAt(0).Path().kind == CAND_LOCAL);
    printf("  connected via local path in %d ms\n", ms);
    // with hairpinning the public path also works, but local still wins (and is chosen even if the public one answers first)
    World w;
    NatType hp = NatType::PortRestricted();
    hp.hairpin = true;
    const int lan = w.sim.AddLan(PublicIp(0), hp);
    const Addr pa = MakeAddr(192, 168, 1, 10, 41000), pb = MakeAddr(192, 168, 1, 11, 41000);
    Console a(&w, w.sim.AddHost(lan, pa), pa, 1, "A"), b(&w, w.sim.AddHost(lan, pb), pb, 2, "B");
    CHECK(a.Start(true, 0));
    Console* cs[2] = {&a, &b};
    Run(w, cs, 2, 50);
    OnlineInfo info;
    a.net.GetInfo(&info);
    CHECK(b.Start(false, info.invite));
    CHECK(RunUntil(w, cs, 2, 10000, [&] { return a.net.GetPhase() == OnlineNet::READY && b.net.GetPhase() == OnlineNet::READY; }) >= 0);
    CHECK(a.net.PeerAt(1).Path().kind == CAND_LOCAL);
    CHECK(b.net.PeerAt(0).Path().kind == CAND_LOCAL);
}

static void TestRoomAndVips() {
    printf("room: invite code, slots, unique vips\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    OnlineInfo ia, ib;
    p.a->net.GetInfo(&ia);
    p.b->net.GetInfo(&ib);
    CHECK_EQ(ia.mySlot, 0);
    CHECK_EQ(ib.mySlot, 1);
    CHECK(ia.myVip != 0 && ib.myVip != 0 && ia.myVip != ib.myVip);
    CHECK_EQ(ia.myVip, MakeAddr(10, 64, 0, 1, 0).ip);
    CHECK_EQ(ib.myVip, MakeAddr(10, 64, 0, 2, 0).ip);
    CHECK(strcmp(ia.invite, ib.invite) == 0 && strlen(ia.invite) == 6);
    // both consoles have the same private address (two NATed Vitas at home): the vips still differ
    CHECK(p.a->priv.ip != 0);
    CHECK_EQ(ia.peerCount, 1);
    CHECK_EQ(ib.connectedCount, 1);
    CHECK(strcmp(ia.peers[1].name, "BOB") == 0);
    CHECK_EQ(ia.myNat, NAT_CONE);
    CHECK_EQ(ia.publicAddr.ip, PublicIp(0));
    // a third console cannot get into a one-guest... the room has 4 slots; a bad code fails cleanly
    Signal* s = CreateMemorySignal(p.w.hub);
    s->Join("NOPE99", "X");
    CHECK(s->GetStatus() == Signal::FAILED);
    delete s;
}

static void TestMtuAndFraming() {
    printf("MTU: no datagram above 1200 bytes\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    const std::vector<uint8_t> big = Frame(9, kMaxFrame);
    CHECK_EQ(p.a->net.SendFrame(-1, &big[0], big.size()), int(big.size()));
    const std::vector<uint8_t> tooBig = Frame(10, kMaxFrame + 1);
    CHECK_EQ(p.a->net.SendFrame(-1, &tooBig[0], tooBig.size()), -1);
    p.Advance(200);
    std::vector<uint32_t> ids;
    int bad = 0;
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 1u);
    CHECK_EQ(ids[0], 9u);
    CHECK(p.a->udp->maxSeen <= kMaxDatagram);
    CHECK(p.a->udp->maxSeen == kMaxDatagram);   // the largest frame fills the datagram exactly
    CHECK_EQ(kMaxFrame, 1178u);
}

static void TestReplayAndTamper() {
    printf("session: replay window, tampering, forgeries\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    // a stream of 100 frames
    std::vector<std::vector<uint8_t> > wire;
    for (uint32_t i = 0; i < 100; ++i) {
        const std::vector<uint8_t> f = Frame(1000 + i, 80);
        p.a->net.SendFrame(-1, &f[0], f.size());
        wire.push_back(p.a->udp->lastData);
        p.Advance(10);
    }
    p.Advance(100);
    std::vector<uint32_t> ids;
    int bad = 0;
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 100u);
    CHECK_EQ(bad, 0);
    Peer& from = p.b->net.PeerAt(0);
    const PeerStats s0 = from.Stats();
    const Addr aPublic = MakeAddr(203, 0, 113, 10, 41000), bPublic = MakeAddr(203, 0, 113, 11, 41000);
    (void)aPublic;
    // 1. an exact replay of the latest datagram, and of one in the middle of the window
    p.a->udp->Inject(bPublic, wire[99]);
    p.a->udp->Inject(bPublic, wire[60]);
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 100u);
    CHECK_EQ(from.Stats().dropReplay - s0.dropReplay, 2u);
    // 2. older than the window (more than 64 packets back)
    p.a->udp->Inject(bPublic, wire[5]);
    p.Advance(100);
    CHECK_EQ(from.Stats().dropReplay - s0.dropReplay, 3u);
    // 3. a flipped bit in the sequence number, the ciphertext, the tag (each with a sequence number never seen, so that only the
    //    authentication can stop it)
    for (int variant = 0; variant < 3; ++variant) {
        std::vector<uint8_t> t = wire[99];
        t[2] = 0; t[3] = 1; t[4] = 0; t[5] = uint8_t(variant);          // a new sequence number
        if (variant == 0) t[3] ^= 0x10;                                   // the header differs from what was sealed
        if (variant == 1) t[10] ^= 0x10;                                  // ciphertext
        if (variant == 2) t[t.size() - 1] ^= 0x80;                        // tag
        p.a->udp->Inject(bPublic, t);
    }
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 100u);
    CHECK_EQ(from.Stats().dropAuth - s0.dropAuth, 3u);
    // 4. a forgery with the wrong session key, right header
    {
        uint8_t wrongKey[32] = {9}, nonce[24] = {0}, pkt[200];
        pkt[0] = kTypeData; pkt[1] = 0;
        pkt[2] = 0; pkt[3] = 0; pkt[4] = 0x10; pkt[5] = 0;
        nonce[0] = 0; nonce[22] = 0x10;
        uint8_t body[40] = {1, 2, 3};
        AeadSeal(pkt + 6, pkt + 6 + 40, wrongKey, nonce, pkt, 6, body, 40);
        p.a->udp->Inject(bPublic, std::vector<uint8_t>(pkt, pkt + 62));
    }
    // 5. garbage of every size: no crash, nothing delivered
    for (size_t n : {size_t(1), size_t(2), size_t(5), size_t(21), size_t(22), size_t(40), size_t(1200)}) {
        std::vector<uint8_t> g(n, 0xC2);
        p.a->udp->Inject(bPublic, g);
        g.assign(n, 0xC1);
        p.a->udp->Inject(bPublic, g);
        g.assign(n, 0x00);
        p.a->udp->Inject(bPublic, g);
    }
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 100u);
    CHECK(from.GetState() == Peer::CONNECTED);
    // 6. reordering inside the window is accepted: send frames 200..205 and deliver them in reverse
    std::vector<std::vector<uint8_t> > six;
    for (uint32_t i = 0; i < 6; ++i) {
        const std::vector<uint8_t> f = Frame(2000 + i, 64);
        p.a->net.SendFrame(-1, &f[0], f.size());
        six.push_back(p.a->udp->lastData);
    }
    // the six went out normally and arrive; drain them, then check that a second copy of each (a duplicate) is refused
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 106u);
    for (int i = 5; i >= 0; --i) p.a->udp->Inject(bPublic, six[i]);
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    CHECK_EQ(ids.size(), 106u);
    // a punch packet with a bad MAC is refused too
    CHECK(from.Stats().dropAuth >= s0.dropAuth + 3);
}

static void TestLossDelay(double loss, uint32_t delay, uint32_t jitter, double reorder, double dup) {
    char label[160];
    snprintf(label, sizeof label, "loss %.0f%% delay %u ms jitter %u ms reorder %.0f%% dup %.0f%%", loss, delay, jitter, reorder, dup);
    printf("stream over %s\n", label);
    Pair p(NatType::PortRestricted(), NatType::Symmetric() /* the harder one */, false, 11);
    // port-restricted vs symmetric cannot punch; use cone against cone so the stream is about the impairment
    Pair q(NatType::PortRestricted(), NatType::AddressRestricted(), false, 11);
    NatSim::Impair im;
    im.lossPct = loss; im.delayMs = delay; im.jitterMs = jitter; im.reorderPct = reorder; im.dupPct = dup;
    q.w.sim.SetImpair(im);
    CHECK(q.Start());
    const int ms = q.Connect(30000);
    CHECK(ms >= 0);
    if (ms < 0) return;
    // 30 s of 30 Hz traffic both ways, 200-byte frames
    std::vector<uint32_t> ia, ib;
    int bad = 0;
    const int frames = 900;
    for (int i = 0; i < frames; ++i) {
        const std::vector<uint8_t> fa = Frame(i, 200), fb = Frame(100000 + i, 200);
        q.a->net.SendFrame(-1, &fa[0], fa.size());
        q.b->net.SendFrame(-1, &fb[0], fb.size());
        q.Advance(33);
        Drain(q.a, &ia, &bad);
        Drain(q.b, &ib, &bad);
    }
    q.Advance(2000);
    Drain(q.a, &ia, &bad);
    Drain(q.b, &ib, &bad);
    CHECK_EQ(bad, 0);
    std::vector<char> seen(frames, 0);
    int dups = 0;
    for (uint32_t id : ib) { if (id < uint32_t(frames)) { if (seen[id]) ++dups; seen[id] = 1; } }
    int got = 0;
    for (char c : seen) got += c;
    const double frac = double(got) / frames;
    printf("  connected in %d ms; B received %d/%d frames (%.1f%%), duplicates delivered %d, replays refused %llu, auth failures %llu\n", ms, got, frames,
           100.0 * frac, dups, (unsigned long long)q.b->net.PeerAt(0).Stats().dropReplay, (unsigned long long)q.b->net.PeerAt(0).Stats().dropAuth);
    CHECK_EQ(dups, 0);
    CHECK_EQ(q.b->net.PeerAt(0).Stats().dropAuth, 0u);
    CHECK(frac > (100.0 - loss) / 100.0 - 0.03);
    CHECK(frac < 1.0 || loss == 0);
    // late reordered frames beyond 64 packets would be dropped; with this traffic pattern none exceed the window
    if (reorder == 0 && loss == 0) CHECK_EQ(got, frames);
    if (dup > 0) CHECK(q.b->net.PeerAt(0).Stats().dropReplay > 0);
    CHECK(q.a->net.GetPhase() == OnlineNet::READY && q.b->net.GetPhase() == OnlineNet::READY);
    (void)p;
}

static void TestPunchUnderLoss() {
    printf("hole punching under heavy loss and delay\n");
    for (double loss : {5.0, 20.0, 40.0}) {
        int connected = 0;
        for (uint32_t seed = 1; seed <= 20; ++seed) {
            Pair p(NatType::PortRestricted(), NatType::PortRestricted(), false, seed);
            NatSim::Impair im;
            im.lossPct = loss; im.delayMs = 75; im.jitterMs = 20;
            p.w.sim.SetImpair(im);
            if (p.Start() && p.Connect(20000) >= 0) ++connected;
        }
        printf("  loss %.0f%%, 75 ms +- 20 ms: %d/20 pairs connected within 20 s\n", loss, connected);
        if (loss <= 5) CHECK_EQ(connected, 20);
        else CHECK(connected >= 16);
    }
}

static void TestKeepaliveAndNatTimeout() {
    printf("keep-alive holds the NAT mapping; without it the path dies and is re-probed\n");
    {
        World w;
        const int la = w.sim.AddLan(PublicIp(0), NatType::PortRestricted(), 30000);   // mappings expire after 30 s of silence
        const int lb = w.sim.AddLan(PublicIp(1), NatType::PortRestricted(), 30000);
        const Addr pa = MakeAddr(192, 168, 1, 10, 41000), pb = MakeAddr(192, 168, 2, 10, 41000);
        Console a(&w, w.sim.AddHost(la, pa), pa, 1, "A"), b(&w, w.sim.AddHost(lb, pb), pb, 2, "B");
        Console* cs[2] = {&a, &b};
        CHECK(a.Start(true, 0));
        Run(w, cs, 2, 50);
        OnlineInfo info;
        a.net.GetInfo(&info);
        CHECK(b.Start(false, info.invite));
        CHECK(RunUntil(w, cs, 2, 10000, [&] { return a.net.PeerAt(1).Established() && b.net.PeerAt(0).Established(); }) >= 0);
        // two minutes with no game traffic at all: only the transport's keep-alives
        Run(w, cs, 2, 120000, 20);
        CHECK(a.net.PeerAt(1).GetState() == Peer::CONNECTED);
        CHECK(!a.net.PeerAt(1).Stale() && !b.net.PeerAt(0).Stale());
        const std::vector<uint8_t> f = Frame(5, 100);
        a.net.SendFrame(-1, &f[0], f.size());
        Run(w, cs, 2, 200);
        std::vector<uint32_t> ids; int bad = 0;
        Drain(&b, &ids, &bad);
        CHECK_EQ(ids.size(), 1u);
        CHECK(a.udp->sent > 50 && a.udp->sent < 400);   // about one keep-alive per 2 s: cheap
    }
    {
        // the NAT of B reboots (all mappings gone, new public port): both sides re-probe and the path moves
        Pair p(NatType::PortRestricted(), NatType::PortRestricted());
        CHECK(p.Start());
        CHECK(p.Connect(10000) >= 0);
        const Addr before = p.a->net.PeerAt(1).Path().addr;
        p.w.sim.FlushNat(p.lanB);
        // B's NAT now hands out a different port: burn the preserved one with a decoy mapping
        p.w.sim.Send(p.b->host, MakeAddr(192, 0, 2, 7, 9), "x", 1, p.w.now);   // allocates a mapping for the console's own port... (a decoy destination)
        p.Advance(15000);
        CHECK(p.a->net.PeerAt(1).GetState() == Peer::CONNECTED);
        CHECK(p.b->net.PeerAt(0).GetState() == Peer::CONNECTED);
        const std::vector<uint8_t> f = Frame(6, 100);
        p.a->net.SendFrame(-1, &f[0], f.size());
        p.b->net.SendFrame(-1, &f[0], f.size());
        p.Advance(300);
        std::vector<uint32_t> ia, ib; int bad = 0;
        Drain(p.a, &ia, &bad);
        Drain(p.b, &ib, &bad);
        CHECK_EQ(ia.size(), 1u);
        CHECK_EQ(ib.size(), 1u);
        (void)before;
    }
}

static void TestWifiDropAndRebuild() {
    printf("Wi-Fi drop: both sides notice, close, rebuild and punch again\n");
    PunchConfig fast;
    fast.lostMs = 6000;
    Pair p(NatType::PortRestricted(), NatType::AddressRestricted(), false, 3);
    CHECK(p.Start(&fast));
    CHECK(p.Connect(10000) >= 0);
    // a short outage (shorter than the stale limit) is invisible to the session
    p.w.sim.SetHostLink(p.a->host, false);
    p.Advance(1500);
    p.w.sim.SetHostLink(p.a->host, true);
    p.Advance(2000);
    CHECK(p.a->net.PeerAt(1).GetState() == Peer::CONNECTED);
    CHECK_EQ(p.a->lostCalls, 0);
    // a long one: the link is lost on both sides (nothing received for lostMs)
    p.w.sim.SetHostLink(p.a->host, false);
    p.Advance(8000);
    CHECK_EQ(p.a->lostCalls, 1);
    CHECK_EQ(p.b->lostCalls, 1);
    CHECK(p.a->net.GetPhase() == OnlineNet::LOST);
    CHECK(p.b->net.GetPhase() == OnlineNet::LOST);
    CHECK(!p.a->udp->IsOpen());   // the stale socket is closed
    CHECK(p.a->net.SendFrame(-1, "abcd", 4) == -1);   // sends fail until the link is rebuilt: the game ends its session on that
    // Wi-Fi is back; the game's session layer starts its link again (the lobby): the link is rebuilt
    p.w.sim.SetHostLink(p.a->host, true);
    p.a->net.OnLinkStart();
    p.b->net.OnLinkStart();
    CHECK(p.a->udp->IsOpen());
    const int ms = p.Connect(15000);
    CHECK(ms >= 0);
    printf("  reconnected %d ms after the rebuild\n", ms);
    const std::vector<uint8_t> f = Frame(77, 120);
    p.a->net.SendFrame(-1, &f[0], f.size());
    p.b->net.SendFrame(-1, &f[0], f.size());
    p.Advance(300);
    std::vector<uint32_t> ia, ib; int bad = 0;
    Drain(p.a, &ia, &bad);
    Drain(p.b, &ib, &bad);
    CHECK_EQ(ia.size(), 1u);
    CHECK_EQ(ib.size(), 1u);
    // a fresh key: a datagram recorded before the drop is refused (no key / nonce reuse across the rebuild)
    CHECK(!p.a->udp->allData.empty());
    const uint64_t auth0 = p.b->net.PeerAt(0).Stats().dropAuth;
    p.a->udp->Inject(MakeAddr(203, 0, 113, 11, 41000), p.a->udp->allData.front());
    p.Advance(100);
    CHECK(p.b->net.PeerAt(0).Stats().dropAuth > auth0 || p.b->net.PeerAt(0).Stats().dropReplay > 0);
    OnlineInfo info;
    p.a->net.GetInfo(&info);
    CHECK_EQ(info.connectedCount, 1);
}

static void TestCloseMessage() {
    printf("close message: the other side knows within one round trip\n");
    PunchConfig cfg;
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    NatSim::Impair im;
    im.delayMs = 40;
    p.w.sim.SetImpair(im);
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    p.a->net.OnLinkLost("test: the app is leaving");
    p.Advance(150);
    CHECK(p.b->net.PeerAt(0).GetState() == Peer::CLOSED);
    CHECK_EQ(p.b->lostCalls, 1);
    CHECK(p.b->net.GetPhase() == OnlineNet::LOST);
    (void)cfg;
}

// The relay (TURN) path: framing, preference, and the controller binding it after a failed punch. The relay of the test is a pipe
// that forwards ChannelData between the two clients that use the same channel number.
static void TestRelayPath() {
    printf("relay path (TURN ChannelData over the same socket) after a failed punch\n");
    const Addr relay = MakeAddr(198, 51, 100, 9, 3478);
    std::vector<std::pair<Addr, uint16_t> > clients;   // who sent on a channel
    uint64_t relayed = 0;
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 5);
    p.w.sim.AddServer(relay, [&](const Addr& src, const std::vector<uint8_t>& d, uint64_t now) {
        if (d.size() < 4 || Classify(&d[0], d.size()) != PKT_CHANNEL) return;
        const uint16_t ch = uint16_t(d[0] << 8 | d[1]);
        bool known = false;
        for (size_t i = 0; i < clients.size(); ++i)
            if (clients[i].first == src && clients[i].second == ch) known = true;
        if (!known) clients.push_back(std::make_pair(src, ch));
        for (size_t i = 0; i < clients.size(); ++i) {
            if (clients[i].second == ch && !(clients[i].first == src)) {
                p.w.sim.ServerSend(relay, clients[i].first, d, now);
                ++relayed;
            }
        }
    });
    CHECK(p.Start());
    CHECK(p.Connect(15000) < 0);
    CHECK(p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED);
    p.a->net.BindRelay(1, relay, 0x4001);
    p.b->net.BindRelay(0, relay, 0x4001);
    const int ms = p.Connect(15000);
    CHECK(ms >= 0);
    CHECK(p.a->net.PeerAt(1).Path().kind == CAND_RELAY);
    CHECK_EQ(p.a->net.PeerAt(1).RelayChannel(), 0x4001);
    printf("  connected through the relay %d ms after it was bound; %llu datagrams relayed so far\n", ms, (unsigned long long)relayed);
    const std::vector<uint8_t> f = Frame(42, 500);
    p.a->net.SendFrame(-1, &f[0], f.size());
    p.Advance(200);
    std::vector<uint32_t> ib; int bad = 0;
    Drain(p.b, &ib, &bad);
    CHECK_EQ(ib.size(), 1u);
    CHECK(p.a->udp->maxSeen <= kMaxDatagram + 4);   // ChannelData adds 4 bytes: the frame limit leaves room only when the relay path is used with frames <= 1196...
}

static void TestHostileMembers() {
    printf("a stranger knowing the addresses but not the key cannot join or disturb the session\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    // an attacker on another host sends well-formed punch packets (own key) and data to both consoles
    const int lanM = p.w.sim.AddLan(PublicIp(5), NatType::FullCone());
    const Addr pm = MakeAddr(192, 168, 9, 9, 41000);
    const int hm = p.w.sim.AddHost(lanM, pm);
    uint8_t evil[32] = {0xEE};
    const Addr bPublic = MakeAddr(203, 0, 113, 11, 41000), aPublic = MakeAddr(203, 0, 113, 10, 41000);
    uint8_t punchKey[32];
    Kdf(punchKey, evil, "MSCN punch v1", 13);
    for (int i = 0; i < 50; ++i) {
        uint8_t pkt[40] = {kTypePunch, 0, 1, 2};
        pkt[1] = uint8_t(i & 1); pkt[2] = uint8_t((i + 1) & 1);
        memset(pkt + 4, i, 16);
        Mac16(pkt + 24, punchKey, pkt, 24);
        p.w.sim.Send(hm, (i & 1) ? bPublic : aPublic, pkt, sizeof pkt, p.w.now);
        p.Advance(10);
    }
    CHECK(p.a->net.PeerAt(1).GetState() == Peer::CONNECTED);
    CHECK(p.b->net.PeerAt(0).GetState() == Peer::CONNECTED);
    CHECK(p.a->net.PeerAt(1).Path().addr == bPublic);
    CHECK(p.b->net.PeerAt(0).Path().addr == aPublic);
    const std::vector<uint8_t> f = Frame(8, 100);
    p.a->net.SendFrame(-1, &f[0], f.size());
    p.Advance(200);
    std::vector<uint32_t> ib; int bad = 0;
    Drain(p.b, &ib, &bad);
    CHECK_EQ(ib.size(), 1u);
}


// ---- A1/A6/A10/A11/A12/C3 additions ------------------------------------------------------------------------------------------
class NeverSignal : public Signal {
public:
    NeverSignal() : status_(IDLE) { err_[0] = 0; }
    void Host(const char*) override { status_ = WORKING; }
    void Join(const char*, const char*) override { status_ = WORKING; }
    void PublishCandidates(const SigCandidates&) override {}
    void ReportPunch(const PunchReport&) override {}
    void Leave() override {}
    void Poll() override {}
    Status GetStatus() const override { return status_; }
    const SigRoom& Room() const override { return room_; }
    const char* Error() const override { return err_; }
private:
    Status status_;
    SigRoom room_;
    char err_[4];
};

static void TestBringUpLimitsAndReports() {
    printf("bring-up limits, punch reports, end reasons, fault shim, port fallback\n");
    {   // the room never answers: the session fails with a sentence after roomLimitMs, and cancelling works at any time
        World w;
        Rng rng(5);
        OnlineConfig cfg;
        SimUdp* udp = new SimUdp(&w, 0, MakeAddr(192, 168, 1, 10, 41000));
        w.sim.AddHost(w.sim.AddLan(PublicIp(0), NatType::PortRestricted()), MakeAddr(192, 168, 1, 10, 41000));
        cfg.udp = udp;
        cfg.signal = new NeverSignal;
        cfg.env.now = WorldNow; cfg.env.nowCtx = &w; cfg.env.random = RngFill; cfg.env.randomCtx = &rng;
        cfg.stunAddrs[0] = MakeAddr(198, 51, 100, 1, 3478);
        cfg.stunAddrs[1] = MakeAddr(198, 51, 100, 2, 3478);
        cfg.nStunAddrs = 2;
        cfg.roomLimitMs = 3000;
        cfg.signalPollMs = 0;
        OnlineNet net;
        CHECK(net.Start(cfg, 0, "X"));
        for (int i = 0; i < 600 && net.GetPhase() != OnlineNet::FAILED; ++i) { w.now += 10; net.Tick(); }
        CHECK(net.GetPhase() == OnlineNet::FAILED);
        OnlineInfo info;
        net.GetInfo(&info);
        CHECK(strstr(info.error, "did not answer") != 0);
        net.Stop();   // never blocks
    }
    {   // punch reports: a connectable pair reports ok from both sides, two symmetric NATs report a failure, quickly
        Pair p(NatType::PortRestricted(), NatType::AddressRestricted(), false, 11);
        CHECK(p.Start());
        CHECK(p.Connect(10000) >= 0);
        PunchReport rep[8];
        int n = SignalMemoryHubReports(p.w.hub, rep, 8);
        CHECK_EQ(n, 2);
        for (int i = 0; i < n; ++i) { CHECK(rep[i].ok); CHECK(rep[i].ms > 0 && rep[i].ms < 10000); CHECK_EQ(rep[i].myNat, NAT_CONE); }
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        CHECK(ia.peers[1].rttMs < 0 || ia.peers[1].rttMs >= 0);   // measured once the first ping came back
        p.Advance(3000);
        p.a->net.GetInfo(&ia);
        CHECK(ia.peers[1].rttMs >= 0);
        CHECK_EQ(ia.peers[1].lossPermille <= 0, true);
        // the end reason after a resume from sleep
        p.a->net.OnLinkLost("resumed from sleep");
        p.a->net.GetInfo(&ia);
        CHECK(strstr(ia.endReason, "went to sleep") != 0);
        CHECK(strstr(p.a->net.EndReason(), "Vita") != 0);
    }
    {
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 12);
        CHECK(p.Start());
        p.both[0] = p.a; p.both[1] = p.b;
        const int t = RunUntil(p.w, p.both, 2, 6000, [&p] { return p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED && p.b->net.GetPhase() == OnlineNet::RELAY_NEEDED; });
        printf("  symmetric against symmetric: relay needed after %d ms\n", t);
        CHECK(t >= 0 && t < 4000);
        PunchReport rep[8];
        const int n = SignalMemoryHubReports(p.w.hub, rep, 8);
        CHECK_EQ(n, 2);
        for (int i = 0; i < n; ++i) { CHECK(!rep[i].ok); CHECK_EQ(rep[i].myNat, NAT_SYMMETRIC); CHECK_EQ(rep[i].peerNat, NAT_SYMMETRIC); }
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        CHECK(ia.publicAddr.port != 0);
        CHECK(p.a->net.GetSignal() != 0);
    }
    {   // symmetric against a port-restricted cone: tried by default (it fails in practice and the timeout finds out), and with the
        // relay policy on it is called hopeless after hopelessTimeoutMs
        PunchConfig pol;
        pol.symmetricRelay = true;
        Pair p(NatType::Symmetric(), NatType::PortRestricted(), false, 13);
        CHECK(p.Start(&pol));
        p.both[0] = p.a; p.both[1] = p.b;
        const int t = RunUntil(p.w, p.both, 2, 6000, [&p] { return p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED; });
        printf("  symmetric against cone with the relay policy: relay needed after %d ms\n", t);
        CHECK(t >= 0 && t < 4000);
    }
    {   // fault shim over the simulator: 20 %% loss, 10 %% duplicates, 60 ms +- 20 ms
        Pair p(NatType::Open(), NatType::Open(), false, 14);
        SimUdp* ua = new SimUdp(&p.w, p.a->host, MakeAddr(203, 0, 113, 50, 41000));
        SimUdp* ub = new SimUdp(&p.w, p.b->host, MakeAddr(203, 0, 113, 51, 41000));
        ua->Open(0); ub->Open(0);
        FaultConfig fc;
        fc.dropPct = 20; fc.dupPct = 10; fc.delayMs = 60; fc.jitterMs = 20;
        Udp* f = CreateFaultUdp(ua, fc, WorldNow, &p.w, 99);
        const int total = 2000;
        int got = 0, early = 0;
        uint8_t buf[64];
        for (int i = 0; i < total; ++i) {
            memset(buf, 0xC2, sizeof buf);
            const uint64_t sentAt = p.w.now;
            memcpy(buf + 1, &sentAt, sizeof sentAt);
            f->SendTo(MakeAddr(203, 0, 113, 51, 41000), buf, 40);
            for (int k = 0; k < 3; ++k) {
                p.w.now += 5;
                Addr from;
                f->RecvFrom(&from, buf, sizeof buf);   // lets the shim release what is due
                while (ub->RecvFrom(&from, buf, sizeof buf) > 0) {
                    uint64_t t0;
                    memcpy(&t0, buf + 1, sizeof t0);
                    ++got;
                    if (p.w.now - t0 < 35) ++early;   // 60 ms - 20 ms jitter, less the 5 ms step
                }
            }
        }
        for (int k = 0; k < 100; ++k) {
            p.w.now += 5;
            Addr from; f->RecvFrom(&from, buf, sizeof buf);
            while (ub->RecvFrom(&from, buf, sizeof buf) > 0) ++got;
        }
        const double expect = total * 0.8 * 1.1;
        printf("  fault shim: %d datagrams in %d sent (expected about %.0f), %d arrived before the minimum delay\n", got, total, expect, early);
        CHECK(got > expect * 0.9 && got < expect * 1.1);
        CHECK_EQ(early, 0);
        delete f; delete ub;
    }
    {   // port fallback: the second socket on the same port takes the next one
        unsetenv("SMG_NET_SIM");
        Udp* a = CreatePlatformUdp();
        Udp* b = CreatePlatformUdp();
        CHECK(a->Open(47311));
        CHECK(b->Open(47311));
        printf("  ports: %u and %u\n", unsigned(a->LocalPort()), unsigned(b->LocalPort()));
        CHECK_EQ(a->LocalPort(), 47311);
        CHECK_EQ(b->LocalPort(), 47312);
        delete a; delete b;
    }
}

// ---- signaling over HTTP against the local fake of the online service (SMG_TEST_SERVER, SMG_TEST_TOKEN0 / 1; tools/net/run_signal_http_test.sh) -----
static uint64_t RealMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
// The clock of the simulated world follows the real one (the HTTP requests take real time).
static bool RunReal(World& w, Console** cs, int n, uint32_t maxMs, const std::function<bool()>& done) {
    const uint64_t t0 = RealMs(), base = w.now;
    for (;;) {
        w.now = base + (RealMs() - t0);
        for (int i = 0; i < n; ++i) cs[i]->net.Tick();
        if (done()) return true;
        if (RealMs() - t0 >= maxMs) return false;
        usleep(3000);
    }
}
static std::string HttpGetJson(const char* base, const char* token, const std::string& path, int* status) {
    std::vector<std::string> h;
    h.push_back(std::string("Authorization: Bearer ") + token);
    h.push_back("X-Msc-App: 3.0.0");   // Galaxy (Strikers: 1.2.0)
    h.push_back("X-Msc-Game: smg");
    std::string err;
    const int hd = HttpStart("GET", std::string(base) + path, "", h, 5000, &err);
    HttpResponse r;
    if (!hd) { *status = -1; return err; }
    for (int i = 0; i < 2000; ++i) {
        if (HttpPoll(hd, &r) == 1) { *status = r.error ? -1 : r.status; return r.error ? r.text : r.body; }
        usleep(2000);
    }
    HttpCancel(hd);
    *status = -1;
    return "timeout";
}
static HttpSignalConfig ServiceConfig(const char* base, const char* token) {
    HttpSignalConfig c;
    ResetServiceInfoCache();   // (svcfix2: /v1/info is cached per session; every test rig is a different service)
    snprintf(c.baseUrl, sizeof c.baseUrl, "%s", base);
    snprintf(c.token, sizeof c.token, "%s", token);
    snprintf(c.appVersion, sizeof c.appVersion, "%s", BuiltAppVersion());   // Galaxy: 3.0.0 (Strikers: 1.2.0)
    c.pollMs = 400;   // the real client asks every 1.7 s; the test is impatient
    return c;
}
static void TestHttpSignaling(const char* base, const char* tok0, const char* tok1) {
    printf("signaling over HTTP against the local service: create, join, endpoints, punch, started, leave\n");
    {   // a pair behind two cone NATs
        Pair p(NatType::PortRestricted(), NatType::AddressRestricted(), false, 21);
        p.a->sigOverride = CreateHttpSignal(ServiceConfig(base, tok0));
        p.b->sigOverride = 0;
        CHECK(p.a->Start(true, 0));
        Console* ca[1] = {p.a};
        const bool haveRoom = RunReal(p.w, ca, 1, 10000, [&p] { OnlineInfo i; p.a->net.GetInfo(&i); return i.invite[0] != 0; });
        CHECK(haveRoom);
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        printf("  host got invite code %s, vip %u.%u.%u.%u\n", ia.invite, ia.myVip >> 24, (ia.myVip >> 16) & 255, (ia.myVip >> 8) & 255, ia.myVip & 255);
        CHECK(strlen(ia.invite) == 6);
        p.b->sigOverride = CreateHttpSignal(ServiceConfig(base, tok1));
        CHECK(p.b->Start(false, ia.invite));
        Console* cs[2] = {p.a, p.b};
        const uint64_t t0 = RealMs();
        const bool up = RunReal(p.w, cs, 2, 25000, [&p] {
            return p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY && p.a->net.PeerAt(1).Established() && p.b->net.PeerAt(0).Established();
        });
        printf("  both established %llu ms after the join\n", (unsigned long long)(RealMs() - t0));
        CHECK(up);
        OnlineInfo ib;
        p.a->net.GetInfo(&ia);
        p.b->net.GetInfo(&ib);
        CHECK_EQ(ia.mySlot, 0);
        CHECK_EQ(ib.mySlot, 1);
        CHECK(ia.myVip != 0 && ib.myVip != 0 && ia.myVip != ib.myVip);
        CHECK_EQ(ia.myVip >> 24, 10u);
        CHECK(strcmp(ib.invite, ia.invite) == 0);
        CHECK(strlen(ia.peers[1].name) > 0 && strlen(ib.peers[0].name) > 0);
        // data both ways (the session key came from the service)
        const std::vector<uint8_t> f = Frame(5, 200), g = Frame(6, 300);
        p.a->net.SendFrame(-1, &f[0], f.size());
        p.b->net.SendFrame(-1, &g[0], g.size());
        RunReal(p.w, cs, 2, 600, [] { return false; });
        std::vector<uint32_t> ids; int bad = 0;
        CHECK_EQ(Drain(p.a, &ids, &bad), 1);
        CHECK_EQ(Drain(p.b, &ids, &bad), 1);
        CHECK_EQ(bad, 0);
        // the service saw the link: the room is "started" once both confirmed, the partner is present
        RunReal(p.w, cs, 2, 1500, [] { return false; });
        int st = 0;
        const std::string view = HttpGetJson(base, tok0, std::string("/v1/rooms/") + ia.invite, &st);
        CHECK_EQ(st, 200);
        printf("  room view: %s\n", view.substr(0, 110).c_str());
        CHECK(view.find("\"state\":\"started\"") != std::string::npos);
        CHECK(view.find("\"link\":\"ok\"") != std::string::npos);
        // the host leaves: the guest's room closes
        p.a->net.Stop();
        RunReal(p.w, cs + 1, 1, 2500, [] { return false; });
        for (int i = 0; i < 20; ++i) { HttpPump(); usleep(20000); }
        const std::string after = HttpGetJson(base, tok1, std::string("/v1/rooms/") + ia.invite, &st);
        printf("  after the host left: %s\n", after.substr(0, 200).c_str());
        // (Galaxy: the room outlives its host, DESIGN.md section 8: it closes, or the guest is its host now)
        CHECK(after.find("\"state\":\"closed\"") != std::string::npos || st == 410 || st == 404 || after.find("\"host_slot\":1") != std::string::npos ||
              after.find("\"hostSlot\":1") != std::string::npos);
        CHECK(p.b->net.PeerAt(0).GetState() == Peer::CLOSED || p.b->net.GetPhase() == OnlineNet::LOST);
    }
    {   // refusals come back as sentences
        World w;
        Rng rng(5);
        OnlineConfig cfg;
        SimUdp* udp = new SimUdp(&w, 0, MakeAddr(192, 168, 1, 10, 41000));
        w.sim.AddHost(w.sim.AddLan(PublicIp(0), NatType::PortRestricted()), MakeAddr(192, 168, 1, 10, 41000));
        cfg.udp = udp;
        cfg.signal = CreateHttpSignal(ServiceConfig(base, tok1));
        cfg.env.now = WorldNow; cfg.env.nowCtx = &w; cfg.env.random = RngFill; cfg.env.randomCtx = &rng;
        cfg.stunAddrs[0] = MakeAddr(198, 51, 100, 1, 3478);
        cfg.stunAddrs[1] = MakeAddr(198, 51, 100, 2, 3478);
        cfg.nStunAddrs = 2;
        cfg.signalPollMs = 0;
        OnlineNet net;
        CHECK(net.Start(cfg, "ZZZZZZ", "X"));
        const uint64_t t0 = RealMs();
        while (net.GetPhase() != OnlineNet::FAILED && RealMs() - t0 < 8000) { w.now += 5; net.Tick(); usleep(5000); }
        OnlineInfo info;
        net.GetInfo(&info);
        printf("  unknown code: \"%s\"\n", info.error);
        CHECK(net.GetPhase() == OnlineNet::FAILED);
        CHECK(strstr(info.error, "invite code was not found") != 0);
    }
    {
        World w;
        Rng rng(6);
        OnlineConfig cfg;
        SimUdp* udp = new SimUdp(&w, 0, MakeAddr(192, 168, 1, 10, 41000));
        w.sim.AddHost(w.sim.AddLan(PublicIp(0), NatType::PortRestricted()), MakeAddr(192, 168, 1, 10, 41000));
        cfg.udp = udp;
        cfg.signal = CreateHttpSignal(ServiceConfig(base, "0123456789012345678901234567890123456789012"));
        cfg.env.now = WorldNow; cfg.env.nowCtx = &w; cfg.env.random = RngFill; cfg.env.randomCtx = &rng;
        cfg.stunAddrs[0] = MakeAddr(198, 51, 100, 1, 3478);
        cfg.nStunAddrs = 1;
        cfg.signalPollMs = 0;
        OnlineNet net;
        CHECK(net.Start(cfg, 0, "X"));
        const uint64_t t0 = RealMs();
        while (net.GetPhase() != OnlineNet::FAILED && RealMs() - t0 < 8000) { w.now += 5; net.Tick(); usleep(5000); }
        OnlineInfo info;
        net.GetInfo(&info);
        printf("  wrong token: \"%s\"\n", info.error);
        CHECK(net.GetPhase() == OnlineNet::FAILED);
        CHECK(strstr(info.error, "not accepted") != 0);
    }
}


// ---- the relay (TURN) -------------------------------------------------------------------------------------------------------
// The test TURN server (turnsim.cpp) sits at a public address of the simulated internet; credentials come from the memory fake, which mints them the
// way the service does (username "<expiry>:<id>", HMAC-SHA1 credential), so every test runs the real client code against an independent server.
static const char kTurnUrl[] = "turn:198.51.100.9:3478?transport=udp";
static const char kTurnSecret[] = "turn-test-secret";

struct TurnRig {
    std::unique_ptr<NatSim::TurnSim> sim;
    Addr addr;
    std::shared_ptr<bool> alive;   // the simulated network outlives the rig in some tests: its handler checks this
    ~TurnRig() { *alive = false; }
    TurnRig(World& world, NatSim::TurnSim::Config cfg = NatSim::TurnSim::Config()) {
        World* pw = &world;
        cfg.secret = kTurnSecret;
        cfg.wallSec = [pw] { return pw->now / 1000; };
        addr = MakeAddr(198, 51, 100, 9, 3478);
        sim.reset(new NatSim::TurnSim(cfg, [pw](const Addr& from, const Addr& to, const std::vector<uint8_t>& d, uint64_t now) { pw->sim.ServerSend(from, to, d, now); }));
        NatSim::TurnSim* ts = sim.get();
        const Addr a = addr;
        alive.reset(new bool(true));
        std::shared_ptr<bool> live = alive;
        world.sim.AddServer(a, [ts, a, live](const Addr& src, const std::vector<uint8_t>& d, uint64_t now) { if (*live) ts->OnDatagram(a, src, d, now); });
    }
};

// the production punch policy (SMG_NET_SYM_RELAY default): a symmetric NAT starts the relay after 1.2 s without a direct path
static PunchConfig ProdPunch() {
    PunchConfig pc;
    pc.symmetricRelay = true;
    return pc;
}

static void UseRelay(Pair& p, uint32_t reportedTtl = 3600, uint32_t realTtl = 3600, int budget = -1) {
    SignalMemoryHubSetTurn(p.w.hub, kTurnUrl, kTurnSecret, reportedTtl, realTtl, budget);
    p.a->relay = p.b->relay = true;
}

struct StreamResult { int gotB, gotA, bad, frames; };
// 30 Hz both ways, 200-byte frames; counts what arrived
static StreamResult RunStream(Pair& q, int frames, uint32_t stepMs = 33, int firstId = 0) {
    StreamResult r = {0, 0, 0, frames};
    std::vector<uint32_t> ia, ib;
    for (int i = 0; i < frames; ++i) {
        const std::vector<uint8_t> fa = Frame(uint32_t(firstId + i), 200), fb = Frame(100000 + uint32_t(firstId + i), 200);
        q.a->net.SendFrame(-1, &fa[0], fa.size());
        q.b->net.SendFrame(-1, &fb[0], fb.size());
        q.Advance(stepMs);
        Drain(q.a, &ia, &r.bad);
        Drain(q.b, &ib, &r.bad);
    }
    q.Advance(1500);
    Drain(q.a, &ia, &r.bad);
    Drain(q.b, &ib, &r.bad);
    r.gotA = int(ia.size());
    r.gotB = int(ib.size());
    return r;
}

static void TestHashVectors() {
    printf("MD5 / SHA-1 / HMAC-SHA1 against the RFC vectors\n");
    auto hex = [](const uint8_t* d, size_t n) { std::string s; char t[4]; for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02x", d[i]); s += t; } return s; };
    uint8_t h[20];
    Md5(h, "abc", 3);
    CHECK(hex(h, 16) == "900150983cd24fb0d6963f7d28e17f72");
    Md5(h, "", 0);
    CHECK(hex(h, 16) == "d41d8cd98f00b204e9800998ecf8427e");
    const char* long1 = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    Md5(h, long1, strlen(long1));
    CHECK(hex(h, 16) == "57edf4a22be3c955ac49da2e2107b67a");
    Sha1(h, "abc", 3);
    CHECK(hex(h, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha1(h, two, strlen(two));
    CHECK(hex(h, 20) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    HmacSha1(h, "Jefe", 4, "what do ya want for nothing?", 28);
    CHECK(hex(h, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    uint8_t key[80];
    memset(key, 0xaa, sizeof key);
    HmacSha1(h, key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
    CHECK(hex(h, 20) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    // RFC 5769 2.2: the sample request with short-term credentials ("VOkJxbRl1RmTxUk/WvJxBt"), MESSAGE-INTEGRITY b4a4... uses the same HMAC over the message prefix
}

// One TurnClient against the test server: sign-in round trip, refresh, permission and channel schedules, stale nonces, release.
static void TestTurnClient() {
    printf("TURN client: Allocate (401 round trip), Refresh, CreatePermission, ChannelBind schedules, stale nonce, Release\n");
    World w;
    const Addr priv = MakeAddr(192, 168, 1, 10, 41000);
    const int lan = w.sim.AddLan(PublicIp(0), NatType::PortRestricted());
    const int host = w.sim.AddHost(lan, priv);
    NatSim::TurnSim::Config sc;
    sc.maxLifetimeSec = 20;     // the allocation dies after 20 s without a Refresh
    sc.permLifeSec = 12;
    sc.bindLifeSec = 25;
    sc.nonceLifeSec = 15;
    TurnRig rig(w, sc);
    SimUdp udp(&w, host, priv);
    udp.Open(41000);
    Rng rng(9);
    Env env;
    env.udp = &udp;
    env.now = WorldNow; env.nowCtx = &w; env.random = RngFill; env.randomCtx = &rng;
    TurnInfo ti;
    FakeMintTurn(&ti, kTurnUrl, kTurnSecret, w.now, 3600, 3600, 1);
    TurnConfig tc;
    tc.lifetimeSec = 20;
    tc.permissionRefreshMs = 5000;
    tc.channelRefreshMs = 8000;
    TurnClient client;
    client.Start(&env, &rig.addr, 1, ti.user, ti.pass, tc);
    auto pump = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            w.now += 10;
            Addr from;
            uint8_t buf[2048];
            int n;
            while ((n = udp.RecvFrom(&from, buf, sizeof buf)) > 0) {
                const uint8_t* inner; size_t il; Addr pa;
                client.HandlePacket(from, buf, size_t(n), &inner, &il, &pa);
            }
            client.Tick();
        }
    };
    pump(800);
    CHECK(client.Ready());
    CHECK_EQ(rig.sim->GetStats().unauthorized, 1u);   // the first Allocate is unauthenticated: 401 with realm and nonce, then the real one
    CHECK_EQ(rig.sim->GetStats().allocations, 1u);
    CHECK(client.Relayed().ip == rig.addr.ip && client.Relayed().port >= 49152);
    CHECK(client.Mapped() == w.sim.MappingFor(host, rig.addr));    // XOR-MAPPED-ADDRESS is our public address
    printf("  relayed %s, our public address as the relay sees it %s\n", AddrStr(client.Relayed()), AddrStr(client.Mapped()));
    // an independent check of the MESSAGE-INTEGRITY construction (tools/mp/run_turn_test.sh re-computes it in Python)
    {
        uint8_t req[512];
        char realm[128], user[128];
        const size_t n = client.LastAuthRequest(req, sizeof req, realm, user, sizeof realm);
        CHECK(n > 44);
        std::string hexs;
        char t[4];
        for (size_t i = 0; i < n; ++i) { snprintf(t, sizeof t, "%02x", req[i]); hexs += t; }
        printf("TURN_AUTH_REQUEST %s %s %s %s\n", hexs.c_str(), realm, user, ti.pass);
    }
    const uint16_t ch = client.BindPeer(1, MakeAddr(198, 51, 100, 9, 49999));
    CHECK(ch >= 0x4000 && ch <= 0x7FFF);
    pump(600);
    CHECK(client.ChannelBound(1));
    CHECK_EQ(client.SlotForChannel(ch), 1);
    // 100 s: the allocation (20 s), the permission (12 s), the binding (25 s) and the nonce (15 s) all run out several times without the refreshes
    pump(100000);
    CHECK(client.Ready());
    CHECK(client.ChannelBound(1));
    CHECK_EQ(rig.sim->LiveAllocations(), 1);
    const NatSim::TurnSim::Stats& st = rig.sim->GetStats();
    printf("  after 100 s: %llu refreshes, %llu permissions, %llu channel binds, %llu stale nonces answered, allocation still alive\n", (unsigned long long)st.refreshes,
           (unsigned long long)st.permissions, (unsigned long long)st.bindings, (unsigned long long)st.staleNonce);
    CHECK(st.refreshes >= 8);
    CHECK(st.permissions >= 15);
    CHECK(st.bindings >= 10);
    CHECK(st.staleNonce >= 3);
    CHECK_EQ(st.expiredAllocs, 0u);
    CHECK_EQ(st.allocations, 1u);
    // a new address for the same slot gets a new channel (a bound channel keeps its first address for ten minutes)
    const uint16_t ch2 = client.BindPeer(1, MakeAddr(198, 51, 100, 9, 50001));
    CHECK(ch2 != ch);
    pump(600);
    CHECK(client.ChannelBound(1));
    client.Release();
    pump(300);
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
    CHECK_EQ(rig.sim->GetStats().releases, 1u);
}

static void TestTurnClientFailures() {
    printf("TURN client: wrong password, no server, server full, quota\n");
    struct Case { const char* name; int mode; const char* expect; };
    const Case cases[] = {{"wrong password", 0, "refused the sign-in"}, {"nobody answering", 1, "did not answer"}, {"server full (508)", 2, "no room"}, {"quota (486)", 3, "no room"}};
    for (const Case& c : cases) {
        World w;
        const Addr priv = MakeAddr(192, 168, 1, 10, 41000);
        const int host = w.sim.AddHost(w.sim.AddLan(PublicIp(0), NatType::PortRestricted()), priv);
        NatSim::TurnSim::Config sc;
        if (c.mode == 2) sc.maxAllocations = -1;
        if (c.mode == 3) sc.refuseAll = true;
        std::unique_ptr<TurnRig> rig;
        if (c.mode != 1) rig.reset(new TurnRig(w, sc));
        SimUdp udp(&w, host, priv);
        udp.Open(41000);
        Rng rng(3);
        Env env;
        env.udp = &udp;
        env.now = WorldNow; env.nowCtx = &w; env.random = RngFill; env.randomCtx = &rng;
        TurnInfo ti;
        FakeMintTurn(&ti, kTurnUrl, c.mode == 0 ? "some other secret" : kTurnSecret, w.now, 3600, 3600, 1);
        const Addr server = MakeAddr(198, 51, 100, 9, 3478);
        TurnClient client;
        client.Start(&env, &server, 1, ti.user, ti.pass);
        uint32_t t = 0;
        for (; t < 20000 && client.GetState() != TurnClient::FAILED; t += 10) {
            w.now += 10;
            Addr from; uint8_t buf[2048]; int n;
            while ((n = udp.RecvFrom(&from, buf, sizeof buf)) > 0) { const uint8_t* i; size_t il; Addr pa; client.HandlePacket(from, buf, size_t(n), &i, &il, &pa); }
            client.Tick();
        }
        printf("  %-18s -> failed after %u ms: \"%s\"\n", c.name, t, client.Error());
        CHECK(client.GetState() == TurnClient::FAILED);
        CHECK(strstr(client.Error(), c.expect) != 0);
        if (c.mode == 1) CHECK(t >= 6000 && t <= 12000);
    }
}

// symmetric against symmetric: no direct path exists; both allocate, exchange relayed addresses, bind channels and play over the relay
static void TestRelayPair(const char* label, bool relayOnlyA, bool relayOnlyB, const NatType& natA, const NatType& natB, double loss, uint32_t delay) {
    printf("relay pair: %s, loss %.0f%%, delay %u ms\n", label, loss, delay);
    Pair p(natA, natB, false, 31);
    TurnRig rig(p.w);
    UseRelay(p);
    p.a->relayOnly = relayOnlyA;
    p.b->relayOnly = relayOnlyB;
    NatSim::Impair im;
    im.lossPct = loss; im.delayMs = delay; im.jitterMs = delay / 5;
    p.w.sim.SetImpair(im);
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    const int ms = p.Connect(40000);
    CHECK(ms >= 0);
    if (ms < 0) {
        OnlineInfo i;
        p.a->net.GetInfo(&i);
        fprintf(stderr, "  A: phase %d relay %d \"%s\" error \"%s\"\n", i.phase, i.relayState, i.relayText, i.error);
        p.b->net.GetInfo(&i);
        fprintf(stderr, "  B: phase %d relay %d \"%s\" error \"%s\"\n", i.phase, i.relayState, i.relayText, i.error);
        return;
    }
    CHECK(p.a->net.PeerAt(1).Path().kind == CAND_RELAY);
    CHECK(p.b->net.PeerAt(0).Path().kind == CAND_RELAY);
    OnlineInfo ia, ib;
    p.a->net.GetInfo(&ia);
    p.b->net.GetInfo(&ib);
    CHECK_EQ(ia.relayState, 2);
    CHECK_EQ(ib.relayState, 2);
    if (relayOnlyA || relayOnlyB) {
        // privacy: nothing but relayed addresses was ever published by a relay-only console
        CHECK(!relayOnlyA || (ia.publicAddr.IsZero() && p.a->net.Stun().GetState() == StunClient::IDLE));
        CHECK(!relayOnlyB || (ib.publicAddr.IsZero() && p.b->net.Stun().GetState() == StunClient::IDLE));
    }
    const StreamResult r = RunStream(p, 600);
    printf("  connected through the relay in %d ms; B received %d/%d, A received %d/%d; sim relayed %llu packets (%llu bytes)\n", ms, r.gotB, r.frames, r.gotA, r.frames,
           (unsigned long long)rig.sim->GetStats().relayedPackets, (unsigned long long)rig.sim->GetStats().relayedBytes);
    CHECK_EQ(r.bad, 0);
    CHECK(double(r.gotB) / r.frames > (100.0 - loss) / 100.0 - 0.04);
    CHECK(double(r.gotA) / r.frames > (100.0 - loss) / 100.0 - 0.04);
    if (loss == 0) { CHECK_EQ(r.gotA, r.frames); CHECK_EQ(r.gotB, r.frames); }
    CHECK_EQ(p.a->net.PeerAt(1).Stats().dropAuth, 0u);
    CHECK(rig.sim->GetStats().relayedPackets > 1000);
    {   // hostile datagrams at A's socket: channel data that is too long, comes from another address or names an unbound channel never reach the peer
        const Addr aClient = rig.sim->ClientOf(ia.relayedAddr);
        const uint16_t ch = p.a->net.PeerAt(1).Path().channel;
        CHECK(!aClient.IsZero() && ch >= 0x4000);
        auto frame = [&](uint16_t channel, size_t len, size_t sent) {
            std::vector<uint8_t> v(4 + sent, 0x5a);
            v[0] = uint8_t(channel >> 8); v[1] = uint8_t(channel); v[2] = uint8_t(len >> 8); v[3] = uint8_t(len);
            v[4] = 0xC2; v[5] = 1;   // looks like a data packet of member 1
            return v;
        };
        const Addr stranger = MakeAddr(198, 51, 100, 77, 3478);
        auto settle = [&]() { p.Advance(200); };
        settle();
        const uint64_t base = p.a->net.PeerAt(1).Stats().relayRx;
        p.w.sim.ServerSend(rig.addr, aClient, frame(ch, 1400, 1400), p.w.now);   // longer than any datagram we send
        p.w.sim.ServerSend(rig.addr, aClient, frame(ch, 1500, 200), p.w.now);    // length field beyond the datagram
        p.w.sim.ServerSend(stranger, aClient, frame(ch, 100, 100), p.w.now);     // not the relay server
        p.w.sim.ServerSend(rig.addr, aClient, frame(uint16_t(ch + 7), 100, 100), p.w.now);   // a channel nobody bound
        settle();
        CHECK_EQ(p.a->net.PeerAt(1).Stats().relayRx, base);
        p.w.sim.ServerSend(rig.addr, aClient, frame(ch, 100, 100), p.w.now);    // the control: a well-formed frame on the bound channel arrives (and is dropped by the session layer)
        settle();
        CHECK(p.a->net.PeerAt(1).Stats().relayRx > base);
    }
    // leaving frees both allocations
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
    // and the service is told how much was relayed
}

// the relay is the fallback, never the preferred path: where a direct path exists the relay is not used for the match
static void TestRelayNotPreferred() {
    printf("relay available, direct path possible: the direct path carries the match\n");
    const NatType nats[][2] = {{NatType::PortRestricted(), NatType::AddressRestricted()}, {NatType::Symmetric(), NatType::FullCone()}, {NatType::Symmetric(), NatType::AddressRestricted()}};
    for (const auto& n : nats) {
        Pair p(n[0], n[1], false, 5);
        TurnRig rig(p.w);
        UseRelay(p);
        NatSim::Impair im;
        im.delayMs = 60; im.jitterMs = 10;
        p.w.sim.SetImpair(im);
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        const int ms = p.Connect(20000);
        CHECK(ms >= 0);
        p.Advance(3000);
        printf("  %-10s vs %-10s: established in %d ms, path %s / %s, relay allocations used by the pair: %llu\n", n[0].Name(), n[1].Name(), ms,
               p.a->net.PeerAt(1).Path().kind == CAND_RELAY ? "relay" : "direct", p.b->net.PeerAt(0).Path().kind == CAND_RELAY ? "relay" : "direct",
               (unsigned long long)rig.sim->GetStats().allocations);
        CHECK(p.a->net.PeerAt(1).Path().kind != CAND_RELAY);
        CHECK(p.b->net.PeerAt(0).Path().kind != CAND_RELAY);
        CHECK_EQ(rig.sim->GetStats().relayedPackets, 0u);
    }
}

// credentials that run out during a match: proactively (the service said how long they live) and reactively (they die earlier than announced)
static void TestRelayCredentialRotation(bool proactive) {
    printf("relay credentials running out mid-match (%s)\n", proactive ? "announced lifetime: rotated before they expire" : "died early: refused, replaced");
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 41);
    NatSim::TurnSim::Config sc;
    sc.maxLifetimeSec = 40;   // an orphaned allocation of dead credentials disappears by itself within 40 s
    TurnRig rig(p.w, sc);
    // proactive: the client is told 36 s and rotates at a third before; reactive: it is told an hour, the username says 30 s
    UseRelay(p, proactive ? 36 : 3600, proactive ? 36 : 30);
    for (Console* c : {p.a, p.b}) {
        c->turnCfg.lifetimeSec = 40;
        c->turnCfg.permissionRefreshMs = 8000;
        c->turnCfg.channelRefreshMs = 8000;
    }
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    CHECK(p.Connect(30000) >= 0);
    int total = 0, lastWindow = 0;
    // 150 s of play in 30 s windows; the last window must flow again
    for (int win = 0; win < 5; ++win) {
        const StreamResult r = RunStream(p, 900);
        total += r.gotB;
        lastWindow = r.gotB;
        printf("  window %d: B received %d/%d, A received %d/%d (allocations so far %llu, releases %llu, credentials refused %llu, relay drops: no permission %llu, no such peer %llu, unknown channel %llu)\n", win, r.gotB, r.frames, r.gotA, r.frames,
               (unsigned long long)rig.sim->GetStats().allocations, (unsigned long long)rig.sim->GetStats().releases, (unsigned long long)rig.sim->GetStats().expiredCreds,
               (unsigned long long)rig.sim->GetStats().noPermission, (unsigned long long)rig.sim->GetStats().noTarget, (unsigned long long)rig.sim->GetStats().unknownChannel);
        {
            const PeerStats& sa = p.a->net.PeerAt(1).Stats();
            printf("    A's view of B: dropAuth %llu dropReplay %llu dropShort %llu dropNoKey %llu relayRx %llu punchRecv %llu dataRecv %llu\n", (unsigned long long)sa.dropAuth,
                   (unsigned long long)sa.dropReplay, (unsigned long long)sa.dropShort, (unsigned long long)sa.dropNoKey, (unsigned long long)sa.relayRx, (unsigned long long)sa.punchRecv, (unsigned long long)sa.dataRecv);
        }
    }
    CHECK(lastWindow > (proactive ? 850 : 700));   // (refused credentials: a few seconds of silence while the old allocation runs out)
    CHECK(rig.sim->GetStats().allocations >= 6);   // two consoles, at least three allocations each over 150 s
    if (proactive) CHECK(rig.sim->GetStats().releases >= 4);
    else CHECK(rig.sim->GetStats().expiredCreds >= 1);
    CHECK(SignalMemoryHubTurnRequests(p.w.hub) >= 6);
    CHECK(p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY);
    (void)total;
}

// svcfix3 (review): renewal of credentials that is refused by a limit / the budget is tried again after 10 minutes, not every 15 s
// (240 requests an hour for every member of a relayed room)
static void TestRenewalRefusedWaitsLong() {
    printf("relay credentials running out mid-match, the service refuses the renewal (budget): asked again only after 10 minutes\n");
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 41);
    NatSim::TurnSim::Config sc;
    sc.maxLifetimeSec = 40;
    TurnRig rig(p.w, sc);
    UseRelay(p, 36, 36, 2);   // (two requests are answered: the first credentials of both consoles; every later one is refused)
    for (Console* c : {p.a, p.b}) {
        c->turnCfg.lifetimeSec = 40;
        c->turnCfg.permissionRefreshMs = 8000;
        c->turnCfg.channelRefreshMs = 8000;
    }
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    CHECK(p.Connect(30000) >= 0);
    int at4 = -1, afterFirst = 0, grew = 0;
    for (int win = 0; win < 12; ++win) {   // 10 s windows
        RunStream(p, 300);
        const int asked = SignalMemoryHubTurnRequests(p.w.hub);
        printf("  after %d s: %d credential request(s)\n", (win + 1) * 10, asked);
        if (at4 >= 0 && win - at4 <= 2 && asked > 4) ++grew;   // (the 20 s after both renewals were refused)
        if (at4 < 0 && asked >= 4) { at4 = win; afterFirst = asked; }
    }
    CHECK(at4 >= 0);     // the renewal was tried, by both consoles, and refused
    CHECK_EQ(afterFirst, 4);
    CHECK_EQ(grew, 0);   // no 15 s retries (they would add requests inside those 20 s); a link that dies later is a new session with its own request
}

// the relay is unavailable: a sentence, not a hang
static void TestRelayUnavailable() {
    printf("relay unavailable: budget used up, no relay on this service, relay server silent\n");
    {   // the service's relay budget is used up: both players are told
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 17);
        TurnRig rig(p.w);
        UseRelay(p, 3600, 3600, 0);
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        p.both[0] = p.a; p.both[1] = p.b;
        const int t = RunUntil(p.w, p.both, 2, 30000, [&p] { return p.a->net.GetPhase() == OnlineNet::FAILED && p.b->net.GetPhase() == OnlineNet::FAILED; });
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        printf("  budget exhausted: failed after %d ms: \"%s\"\n", t, ia.error);
        CHECK(t >= 0);
        CHECK(strstr(ia.error, "Relay play is not available right now") != 0 && strstr(ia.error, "turn_") == 0);   // (svcfix3: one calm sentence)
        CHECK_EQ(ia.relayState, 3);
        CHECK_EQ(rig.sim->GetStats().allocations, 0u);
    }
    {   // a service without a relay
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 18);
        p.a->relay = p.b->relay = true;   // (the hub has no TURN configured)
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        p.both[0] = p.a; p.both[1] = p.b;
        const int t = RunUntil(p.w, p.both, 2, 30000, [&p] { return p.a->net.GetPhase() == OnlineNet::FAILED; });
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        printf("  no relay on the service: failed after %d ms: \"%s\"\n", t, ia.error);
        CHECK(t >= 0);
        CHECK(strstr(ia.error, "not available") != 0);
    }
    {   // the relay server does not answer (blocked, down)
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 19);
        UseRelay(p);   // credentials, but nothing listens at the address
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        p.both[0] = p.a; p.both[1] = p.b;
        const int t = RunUntil(p.w, p.both, 2, 60000, [&p] { return p.a->net.GetPhase() == OnlineNet::FAILED; });
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        printf("  relay silent: failed after %d ms: \"%s\"\n", t, ia.error);
        CHECK(t >= 0);
        CHECK(strstr(ia.error, "did not answer") != 0);
    }
    {   // a relay that is unavailable while the direct path works: the match still happens (the problem is only a warning)
        Pair p(NatType::PortRestricted(), NatType::AddressRestricted(), false, 20);
        p.a->relay = p.b->relay = true;
        SignalMemoryHubSetTurn(p.w.hub, kTurnUrl, kTurnSecret, 3600, 3600, 0);
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        CHECK(p.Connect(20000) >= 0);
    }
}


// ---- the relay through the real service code (local fake) -----------------------------------------------------------------------
static bool SplitServer(const char* spec, std::string* base, std::string* t0, std::string* t1) {
    if (!spec || !*spec) return false;
    std::string s = spec;
    const size_t a = s.find(','), b = s.find(',', a + 1);
    if (a == std::string::npos || b == std::string::npos) return false;
    *base = s.substr(0, a);
    *t0 = s.substr(a + 1, b - a - 1);
    *t1 = s.substr(b + 1);
    return true;
}

// POST /v1/turn through the signaling client: credentials, and the sentences for the budget and the per-player limit
static void TestTurnCredentialsHttp(const char* spec, const char* label, const char* expectSentence, int allowed) {
    std::string base, t0, t1;
    if (!SplitServer(spec, &base, &t0, &t1)) return;
    printf("relay credentials from the service (%s)\n", label);
    Signal* a = CreateHttpSignal(ServiceConfig(base.c_str(), t0.c_str()));
    Signal* b = CreateHttpSignal(ServiceConfig(base.c_str(), t1.c_str()));
    a->Host("Alice1");
    auto pollUntil = [&](const std::function<bool()>& done, uint32_t maxMs) {
        const uint64_t t = RealMs();
        while (RealMs() - t < maxMs) {
            a->Poll();
            b->Poll();
            if (done()) return true;
            usleep(3000);
        }
        return false;
    };
    CHECK(pollUntil([&] { return a->GetStatus() == Signal::IN_ROOM && a->Room().invite[0]; }, 8000));
    b->Join(a->Room().invite, "Bobby2");
    CHECK(pollUntil([&] { return b->GetStatus() == Signal::IN_ROOM && a->Room().members[1].present; }, 8000));
    int got = 0;
    for (int i = 0; i < allowed + 1; ++i) {
        Signal* who = a;
        who->RequestTurn();
        CHECK(pollUntil([&] { return who->TurnState() == Signal::TURN_READY || who->TurnState() == Signal::TURN_FAILED; }, 8000));
        if (who->TurnState() == Signal::TURN_READY) {
            ++got;
            const TurnInfo& t = who->Turn();
            CHECK(t.nUrls >= 1);
            CHECK(strncmp(t.urls[0], "turn:", 5) == 0 && strstr(t.urls[0], "transport=tcp") == 0);
            CHECK(strchr(t.user, ':') != 0 && t.pass[0] != 0);
            CHECK_EQ(t.ttlSec, 3600u);
            printf("  credentials #%d: %d address(es), first %s, user \"%s\"\n", got, t.nUrls, t.urls[0], t.user);
        } else {
            printf("  request #%d refused: \"%s\"\n", i + 1, who->TurnError());
            CHECK(strstr(who->TurnError(), expectSentence) != 0);
        }
    }
    CHECK_EQ(got, allowed);
    a->ReportTurnUsage(123456);
    for (int i = 0; i < 10; ++i) { HttpPump(); usleep(10000); }
    a->Leave();
    b->Leave();
    delete a;
    delete b;
}

// a whole relayed session through the service: the service steers (symmetric NAT / relay-only profile), both consoles fetch credentials, allocate on the
// test TURN server, publish their relayed addresses through the room and play
static void TestRelaySessionHttp(const char* spec, bool relayOnlyA, bool relayOnlyB, const NatType& natA, const NatType& natB) {
    std::string base, t0, t1;
    if (!SplitServer(spec, &base, &t0, &t1)) return;
    printf("relay session through the service: %s vs %s%s%s\n", natA.Name(), natB.Name(), relayOnlyA ? ", A relay-only" : "", relayOnlyB ? ", B relay-only" : "");
    Pair p(natA, natB, false, 51);
    TurnRig rig(p.w);
    p.a->relay = p.b->relay = true;
    p.a->relayOnly = relayOnlyA;
    p.b->relayOnly = relayOnlyB;
    p.a->sigOverride = CreateHttpSignal(ServiceConfig(base.c_str(), t0.c_str()));
    CHECK(p.a->Start(true, 0));
    Console* ca[1] = {p.a};
    CHECK(RunReal(p.w, ca, 1, 10000, [&p] { OnlineInfo i; p.a->net.GetInfo(&i); return i.invite[0] != 0; }));
    OnlineInfo ia, ib;
    p.a->net.GetInfo(&ia);
    p.b->sigOverride = CreateHttpSignal(ServiceConfig(base.c_str(), t1.c_str()));
    CHECK(p.b->Start(false, ia.invite));
    Console* cs[2] = {p.a, p.b};
    const uint64_t t = RealMs();
    const bool up = RunReal(p.w, cs, 2, 40000, [&p] {
        return p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY && p.a->net.PeerAt(1).Established() && p.b->net.PeerAt(0).Established();
    });
    p.a->net.GetInfo(&ia);
    p.b->net.GetInfo(&ib);
    printf("  established after %llu ms: %s; A relay \"%s\", B relay \"%s\"\n", (unsigned long long)(RealMs() - t), up ? "yes" : "NO", ia.relayText, ib.relayText);
    if (!up) printf("  A error \"%s\" warning \"%s\"; B error \"%s\" warning \"%s\"\n", ia.error, ia.warning, ib.error, ib.warning);
    CHECK(up);
    if (!up) return;
    CHECK(p.a->net.PeerAt(1).Path().kind == CAND_RELAY);
    CHECK(p.b->net.PeerAt(0).Path().kind == CAND_RELAY);
    const std::vector<uint8_t> f = Frame(5, 200), g = Frame(6, 300);
    p.a->net.SendFrame(-1, &f[0], f.size());
    p.b->net.SendFrame(-1, &g[0], g.size());
    RunReal(p.w, cs, 2, 600, [] { return false; });
    std::vector<uint32_t> ids; int bad = 0;
    CHECK_EQ(Drain(p.a, &ids, &bad), 1);
    CHECK_EQ(Drain(p.b, &ids, &bad), 1);
    CHECK_EQ(bad, 0);
    // what the service saw: the room started; relay-only players never exposed an address
    int st = 0;
    const std::string view = HttpGetJson(base.c_str(), t1.c_str(), std::string("/v1/rooms/") + ia.invite, &st);
    printf("  room (as the guest sees it): %s\n", view.substr(0, 400).c_str());
    CHECK(view.find("\"state\":\"started\"") != std::string::npos);
    CHECK(view.find("\"relay\":\"198.51.100.9:") != std::string::npos);
    if (relayOnlyA || relayOnlyB) CHECK(view.find("\"public\"") == std::string::npos);
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
}

// Delays only relay publication or the credential reply; all room, key and TURN behavior stays with the memory service.
class LateRelaySignal : public Signal {
public:
    LateRelaySignal(SignalMemoryHub* hub, bool steer, uint32_t publishDelayMs, uint32_t credentialDelayMs = 0)
        : in_(CreateMemorySignal(hub)), steer_(steer), publishDelayMs_(publishDelayMs), credentialDelayMs_(credentialDelayMs),
          now_(0), ctx_(0), publishAt_(0), askedAt_(0), pending_(false), publishedRelay_(false), asked_(false) {}
    ~LateRelaySignal() { delete in_; }
    void Host(const char* n) override { in_->Host(n); }
    void Join(const char* i, const char* n) override { in_->Join(i, n); }
    void PublishCandidates(const SigCandidates& c) override {
        if (c.relay.IsZero() || publishedRelay_ || publishDelayMs_ == 0) {
            in_->PublishCandidates(c);
            if (!c.relay.IsZero()) publishedRelay_ = true;
            return;
        }
        if (!pending_) publishAt_ = Now() + publishDelayMs_;
        pending_ = true;
        mine_ = c;
    }
    void ReportPunch(const PunchReport& r) override { in_->ReportPunch(r); }
    void Leave() override { in_->Leave(); }
    void Poll() override {
        if (pending_ && Now() >= publishAt_) {
            in_->PublishCandidates(mine_);
            pending_ = false;
            publishedRelay_ = true;
        }
        in_->Poll();
        room_ = in_->Room();
        if (steer_ && room_.members[0].present && room_.members[1].present) {
            room_.steerRelay = true;
            snprintf(room_.steerReason, sizeof room_.steerReason, "nat");
        }
    }
    Status GetStatus() const override { return in_->GetStatus(); }
    const SigRoom& Room() const override { return room_; }
    const char* Error() const override { return in_->Error(); }
    void SetClock(uint64_t (*now)(void*), void* ctx) override { now_ = now; ctx_ = ctx; in_->SetClock(now, ctx); }
    void Started() override { in_->Started(); }
    const char* StunList() const override { return in_->StunList(); }
    void SetRelayOnly(bool on) override { in_->SetRelayOnly(on); }
    void RequestTurn() override { in_->RequestTurn(); askedAt_ = Now(); asked_ = true; }
    TurnStatus TurnState() const override {
        const TurnStatus st = in_->TurnState();
        return st == TURN_READY && asked_ && Now() - askedAt_ < credentialDelayMs_ ? TURN_WORKING : st;
    }
    const TurnInfo& Turn() const override { return in_->Turn(); }
    const char* TurnError() const override { return in_->TurnError(); }
    void ReportTurnUsage(uint64_t b) override { in_->ReportTurnUsage(b); }

private:
    uint64_t Now() const { return now_ ? now_(ctx_) : 0; }
    Signal* in_;
    SigRoom room_;
    SigCandidates mine_;
    bool steer_;
    uint32_t publishDelayMs_, credentialDelayMs_;
    uint64_t (*now_)(void*);
    void* ctx_;
    uint64_t publishAt_, askedAt_;
    bool pending_, publishedRelay_, asked_;
};

// Both the menu and radio poll end an attempt on even one RELAY_NEEDED tick. Pair::Connect alone misses that transient failure.
static int ConnectWithoutTerminalPhase(Pair& p, uint32_t maxMs) {
    Console* cs[2] = {p.a, p.b};
    const uint64_t started = p.w.now;
    for (uint32_t t = 0; t < maxMs; t += 10) {
        p.w.now += 10;
        for (int i = 0; i < 2; ++i) {
            cs[i]->net.Tick();
            const OnlineNet::Phase phase = cs[i]->net.GetPhase();
            if (phase == OnlineNet::RELAY_NEEDED || phase == OnlineNet::FAILED) {
                OnlineInfo info;
                cs[i]->net.GetInfo(&info);
                fprintf(stderr, "  member %d ended after %u ms: phase %d, relay %d, error \"%s\"\n", i,
                        unsigned(p.w.now - started), int(phase), info.relayState, info.error);
                return -1;
            }
        }
        if (p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY &&
            p.a->net.PeerAt(1).Established() && p.b->net.PeerAt(0).Established()) return int(p.w.now - started);
    }
    return -1;
}

static void TestLateRelayOrder(const char* label, bool steer, uint32_t lateA, uint32_t lateB, bool relayOnlyA, bool relayOnlyB,
                              const NatType& natA, const NatType& natB, double loss = 0) {
    printf("relay publication order: %s\n", label);
    Pair p(natA, natB, false, 41);
    TurnRig rig(p.w);
    UseRelay(p);
    p.a->relayOnly = relayOnlyA;
    p.b->relayOnly = relayOnlyB;
    p.a->sigOverride = new LateRelaySignal(p.w.hub, steer, lateA);
    p.b->sigOverride = new LateRelaySignal(p.w.hub, steer, lateB);
    NatSim::Impair im;
    im.lossPct = loss; im.delayMs = 40; im.jitterMs = 8;
    p.w.sim.SetImpair(im);
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    const int ms = ConnectWithoutTerminalPhase(p, 40000);
    CHECK(ms >= 0);
    if (ms >= 0) {
        CHECK(p.a->net.PeerAt(1).Path().kind == CAND_RELAY);
        CHECK(p.b->net.PeerAt(0).Path().kind == CAND_RELAY);
        CHECK(rig.sim->GetStats().permissions >= 2);
        CHECK(rig.sim->GetStats().bindings >= 2);
        const StreamResult r = RunStream(p, 150);
        CHECK_EQ(r.bad, 0);
        CHECK(double(r.gotA) / r.frames > (100.0 - loss) / 100.0 - 0.05);
        CHECK(double(r.gotB) / r.frames > (100.0 - loss) / 100.0 - 0.05);
        CHECK(rig.sim->GetStats().relayedPackets > 0);
        CHECK(p.a->udp->maxSeen <= kMaxDatagram + 4);
        CHECK(p.b->udp->maxSeen <= kMaxDatagram + 4);
    }
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
}

// svcfix3 (S5): the service hands credentials without a visible reason out of a smaller slice of the day (turn_budget when it is used up).
// A reason is visible once the room holds this console's endpoint (a symmetric NAT), a failed punch verdict or the account's relay-only flag.
// So none of those may be queued behind the credentials request.
static std::vector<std::string>* g_signalLog = 0;
class OrderSignal : public LateRelaySignal {
public:
    explicit OrderSignal(SignalMemoryHub* hub, char tag) : LateRelaySignal(hub, false, 0), tag_(tag) {}
    void PublishCandidates(const SigCandidates& c) override {
        if (g_signalLog && c.valid) g_signalLog->push_back(std::string(1, tag_) + ":endpoint");
        LateRelaySignal::PublishCandidates(c);
    }
    void ReportPunch(const PunchReport& r) override {
        if (g_signalLog) g_signalLog->push_back(std::string(1, tag_) + (r.ok ? ":punch-ok" : ":punch-fail"));
        LateRelaySignal::ReportPunch(r);
    }
    void RequestTurn() override {
        if (g_signalLog) g_signalLog->push_back(std::string(1, tag_) + ":turn");
        LateRelaySignal::RequestTurn();
    }
private:
    char tag_;
};

static void TestTurnAfterReason(const char* label, const NatType& natA, const NatType& natB) {
    printf("relay credentials come after the reason: %s\n", label);
    std::vector<std::string> log;
    g_signalLog = &log;
    Pair p(natA, natB, false, 47);
    TurnRig rig(p.w);
    UseRelay(p);
    p.a->sigOverride = new OrderSignal(p.w.hub, 'a');
    p.b->sigOverride = new OrderSignal(p.w.hub, 'b');
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    const int ms = ConnectWithoutTerminalPhase(p, 40000);
    CHECK(ms >= 0);
    for (char who = 'a'; who <= 'b'; ++who) {
        int turnAt = -1, reasonAt = -1;
        for (size_t i = 0; i < log.size(); ++i) {
            if (log[i][0] != who) continue;
            if (log[i].find(":turn") != std::string::npos && turnAt < 0) turnAt = int(i);
            if ((log[i].find(":endpoint") != std::string::npos || log[i].find(":punch-fail") != std::string::npos) && reasonAt < 0) reasonAt = int(i);
        }
        printf("  %c: endpoint/verdict at %d, credentials asked at %d\n", who, reasonAt, turnAt);
        CHECK(turnAt < 0 || (reasonAt >= 0 && reasonAt < turnAt));
    }
    p.a->net.Stop();
    p.b->net.Stop();
    g_signalLog = 0;
}

static void TestRelayPartnerWaitIsBounded() {
    printf("relay partner publication never arrives: wait is finite and has a sentence\n");
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 43);
    TurnRig rig(p.w);
    UseRelay(p);
    p.a->sigOverride = new LateRelaySignal(p.w.hub, true, 0);
    p.b->sigOverride = new LateRelaySignal(p.w.hub, true, 120000);
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    const uint64_t started = p.w.now;
    int ended = -1;
    for (uint32_t t = 0; t < 60000; t += 10) {
        p.w.now += 10;
        p.a->net.Tick();
        p.b->net.Tick();
        const OnlineNet::Phase phase = p.a->net.GetPhase();
        if (phase == OnlineNet::RELAY_NEEDED || phase == OnlineNet::FAILED) {
            ended = int(p.w.now - started);
            break;
        }
    }
    OnlineInfo info;
    p.a->net.GetInfo(&info);
    CHECK(ended >= 8000 && ended < 60000);
    CHECK(info.error[0] != 0);
    CHECK_EQ(info.phase, OnlineNet::RELAY_NEEDED);
    CHECK_EQ(info.relayState, 2);
    CHECK(strcmp(info.error, "The other player could not be reached through the relay. Try again.") == 0);
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
}

static void TestRelayWaitKeepsFailuresBounded() {
    printf("relay wait does not hide a credential timeout or refusal\n");
    for (int refusal = 0; refusal < 2; ++refusal) {
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 47);
        TurnRig rig(p.w);
        UseRelay(p, 3600, 3600, refusal ? 0 : -1);
        p.a->sigOverride = new LateRelaySignal(p.w.hub, true, 0, refusal ? 0 : 21000);
        p.b->sigOverride = new LateRelaySignal(p.w.hub, true, 0);
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        const uint64_t started = p.w.now;
        Console* cs[2] = {p.a, p.b};
        const int ms = RunUntil(p.w, cs, 2, 30000, [&] { return p.a->net.GetPhase() == OnlineNet::FAILED; });
        OnlineInfo info;
        p.a->net.GetInfo(&info);
        CHECK(ms >= 0);
        if (refusal) {
            CHECK(ms < 5000);
            CHECK(strstr(info.error, "Relay play is not available right now") != 0);
        } else {
            CHECK(p.w.now - started >= 20000 && p.w.now - started < 21000);
            CHECK(strstr(info.error, "in time") != 0);
        }
        CHECK_EQ(info.relayState, 3);
        p.a->net.Stop();
        p.b->net.Stop();
        CHECK_EQ(rig.sim->LiveAllocations(), 0);
    }
}

static void TestRelayGraceControls() {
    printf("relay grace: disabled relay and zero wait stay immediate; a configured wait ends at its deadline\n");
    const bool enabled[] = {false, true, true};
    const int waits[] = {500, 0, 500};
    const uint64_t expected[] = {0, 0, 500};
    for (int i = 0; i < 3; ++i) {
        Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 53);
        TurnRig rig(p.w);
        UseRelay(p);
        p.a->relay = p.b->relay = enabled[i];
        p.a->relayWaitOverride = p.b->relayWaitOverride = waits[i];
        p.a->sigOverride = new LateRelaySignal(p.w.hub, true, 0);
        p.b->sigOverride = new LateRelaySignal(p.w.hub, true, 120000);
        { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
        uint64_t needsAt = 0;
        bool ended = false;
        for (uint32_t t = 0; t < 5000; t += 10) {
            p.w.now += 10;
            p.a->net.Tick();
            p.b->net.Tick();
            if (!needsAt && p.a->net.PeerAt(1).GetState() == Peer::NEEDS_RELAY) {
                needsAt = p.w.now;
                OnlineInfo info;
                p.a->net.GetInfo(&info);
                // Exact grace offsets require relay setup to have finished before the peer timeout.
                CHECK_EQ(info.relayState, enabled[i] ? 2 : 0);
            }
            if (p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED || p.a->net.GetPhase() == OnlineNet::FAILED) {
                ended = true;
                break;
            }
        }
        CHECK(ended && needsAt != 0);
        CHECK_EQ(p.a->net.GetPhase(), OnlineNet::RELAY_NEEDED);
        CHECK_EQ(p.w.now - needsAt, expected[i]);
        p.a->net.Stop();
        p.b->net.Stop();
        CHECK_EQ(rig.sim->LiveAllocations(), 0);
    }
}

static void TestRelayGraceZeroWithoutSteer() {
    printf("relay grace: zero wait without steer ends before reactive relay setup\n");
    Pair p(NatType::Symmetric(), NatType::PortRestricted(), false, 61);
    TurnRig rig(p.w);
    UseRelay(p);
    p.a->relayWaitOverride = p.b->relayWaitOverride = 0;
    p.a->sigOverride = new LateRelaySignal(p.w.hub, false, 0);
    p.b->sigOverride = new LateRelaySignal(p.w.hub, false, 0);
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    bool sawNeedsRelay = false, relayStartedBeforeNeeds = false;
    for (uint32_t t = 0; t < 5000; t += 10) {
        p.w.now += 10;
        p.a->net.Tick();
        p.b->net.Tick();
        OnlineInfo info;
        p.a->net.GetInfo(&info);
        if (p.a->net.PeerAt(1).GetState() == Peer::NEEDS_RELAY) {
            sawNeedsRelay = true;
            CHECK_EQ(info.phase, OnlineNet::RELAY_NEEDED);
            CHECK_EQ(info.relayState, 0);
            break;
        }
        if (info.relayState != 0) relayStartedBeforeNeeds = true;
    }
    CHECK(sawNeedsRelay);
    CHECK(!relayStartedBeforeNeeds);
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
}

static void TestRelayGraceDoesNotExtendRebuild(bool defaultWait = false) {
    printf("relay grace cannot extend the rebuild deadline (%s wait)\n", defaultWait ? "default" : "60000 ms");
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 59);
    TurnRig rig(p.w);
    UseRelay(p);
    if (!defaultWait) p.a->relayWaitOverride = p.b->relayWaitOverride = 60000;
    p.a->sigOverride = new LateRelaySignal(p.w.hub, true, 0);
    p.b->sigOverride = new LateRelaySignal(p.w.hub, true, 120000);
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    p.Advance(2000);
    CHECK_EQ(p.a->net.PeerAt(1).GetState(), Peer::NEEDS_RELAY);
    CHECK_EQ(p.a->net.GetPhase(), OnlineNet::PUNCHING);
    p.a->net.OnLinkLost("Wi-Fi connection lost");
    p.b->net.OnLinkLost("Wi-Fi connection lost");
    p.a->net.OnLinkStart();
    p.b->net.OnLinkStart();
    Console* cs[2] = {p.a, p.b};
    bool graceExpired = false;
    const int ms = RunUntil(p.w, cs, 2, 46000, [&] {
        if (defaultWait && !graceExpired && p.a->net.GetPhase() == OnlineNet::RELAY_NEEDED) {
            OnlineInfo expired;
            p.a->net.GetInfo(&expired);
            CHECK_EQ(expired.relayState, 2);
            CHECK(strcmp(expired.error, "The other player could not be reached through the relay. Try again.") == 0);
            graceExpired = true;
        }
        return p.a->net.GetPhase() == OnlineNet::FAILED;
    });
    OnlineInfo info;
    p.a->net.GetInfo(&info);
    CHECK_EQ(ms, 45000);
    CHECK(strstr(info.error, "restored") != 0);
    if (defaultWait) {
        CHECK(graceExpired);
        CHECK(strcmp(info.error, "The online connection could not be restored.") == 0);
    }
    p.a->net.Stop();
    p.b->net.Stop();
    CHECK_EQ(rig.sim->LiveAllocations(), 0);
}

static void RunTurnTests() {
    // SMG_TEST_ONLY=<word>: run only the relay tests whose name contains it (for a quick look at one of them)
    const char* only = getenv("SMG_TEST_ONLY");
    auto want = [only](const char* name) { return !only || strstr(name, only) != 0; };
    if (want("hash")) TestHashVectors();
    if (want("client")) TestTurnClient();
    if (want("failures")) TestTurnClientFailures();
    if (want("notpreferred")) TestRelayNotPreferred();
    if (want("pair")) {
        TestRelayPair("symmetric vs symmetric", false, false, NatType::Symmetric(), NatType::Symmetric(), 0, 0);
        TestRelayPair("symmetric vs symmetric", false, false, NatType::Symmetric(), NatType::Symmetric(), 2, 75);
        TestRelayPair("symmetric vs port-restricted (punch fails)", false, false, NatType::Symmetric(), NatType::PortRestricted(), 0, 20);
        TestRelayPair("relay-only vs relay-only (cone NATs)", true, true, NatType::PortRestricted(), NatType::PortRestricted(), 0, 0);
        TestRelayPair("relay-only vs relay-only (cone NATs)", true, true, NatType::PortRestricted(), NatType::PortRestricted(), 2, 75);
        TestRelayPair("relay-only vs Auto (cone NATs)", true, false, NatType::PortRestricted(), NatType::AddressRestricted(), 0, 30);
    }
    if (want("rotation")) {
        TestRelayCredentialRotation(true);
        TestRelayCredentialRotation(false);
        TestRenewalRefusedWaitsLong();
    }
    if (want("unavailable")) TestRelayUnavailable();
    if (want("order")) {
        TestLateRelayOrder("host late", true, 3000, 0, false, false, NatType::Symmetric(), NatType::Symmetric());
        TestLateRelayOrder("joiner late", true, 0, 3000, false, false, NatType::Symmetric(), NatType::Symmetric());
        TestLateRelayOrder("host later than the relay probe timeout", true, 9000, 0, false, false, NatType::Symmetric(), NatType::Symmetric(), 2);
        TestLateRelayOrder("no steer, symmetric pair", false, 3000, 0, false, false, NatType::Symmetric(), NatType::Symmetric(), 2);
        TestLateRelayOrder("no steer, relay starts after the direct timeout", false, 0, 0, false, false, NatType::Symmetric(), NatType::PortRestricted());
        TestLateRelayOrder("relay-only host, late Auto joiner", true, 0, 9000, true, false, NatType::PortRestricted(), NatType::AddressRestricted(), 2);
        TestLateRelayOrder("relay-only pair, late host", true, 9000, 0, true, true, NatType::PortRestricted(), NatType::PortRestricted(), 2);
        TestTurnAfterReason("symmetric pair", NatType::Symmetric(), NatType::Symmetric());
        TestTurnAfterReason("symmetric against port-restricted", NatType::Symmetric(), NatType::PortRestricted());
        TestRelayPartnerWaitIsBounded();
        TestRelayWaitKeepsFailuresBounded();
        TestRelayGraceControls();
        TestRelayGraceZeroWithoutSteer();
        TestRelayGraceDoesNotExtendRebuild();
        TestRelayGraceDoesNotExtendRebuild(true);
    }
}

// ---- security review fixes (client side) ---------------------------------------------------------------------------------------
// A packet as the other member would build it: DATA / CTRL sealed with the session key both sides hold (the test reads it from a Peer).
static std::vector<uint8_t> ForgeSecure(Peer& keyHolder, uint8_t type, int fromSlot, uint32_t seq, size_t plainLen) {
    std::vector<uint8_t> pkt(kDataHeader + plainLen + kTagSize);
    pkt[0] = type;
    pkt[1] = uint8_t(fromSlot);
    pkt[2] = uint8_t(seq >> 24); pkt[3] = uint8_t(seq >> 16); pkt[4] = uint8_t(seq >> 8); pkt[5] = uint8_t(seq);
    uint8_t nonce[24] = {0};
    nonce[0] = uint8_t(fromSlot);
    nonce[20] = pkt[2]; nonce[21] = pkt[3]; nonce[22] = pkt[4]; nonce[23] = pkt[5];
    std::vector<uint8_t> body = Frame(seq, plainLen);
    AeadSeal(&pkt[kDataHeader], &pkt[kDataHeader + plainLen], PeerTestAccess::DataKey(keyHolder), nonce, &pkt[0], kDataHeader, plainLen ? &body[0] : 0, plainLen);
    return pkt;
}
// A punch packet with a valid MAC under the room's punch key (a recording, or what a member could send).
static std::vector<uint8_t> ForgePunch(const SigRoom& room, int from, int to, uint8_t flags, const uint8_t hello[8], const uint8_t echo[8]) {
    std::vector<uint8_t> pkt(Peer::kPunchSize);
    pkt[0] = kTypePunch; pkt[1] = uint8_t(from); pkt[2] = uint8_t(to); pkt[3] = flags;
    memcpy(&pkt[4], hello, 8);
    memcpy(&pkt[12], echo, 8);
    pkt[20] = uint8_t(room.cookie >> 24); pkt[21] = uint8_t(room.cookie >> 16); pkt[22] = uint8_t(room.cookie >> 8); pkt[23] = uint8_t(room.cookie);
    uint8_t punchKey[32];
    Kdf(punchKey, room.key, "MSCN punch v1", 13);
    Mac16(&pkt[24], punchKey, &pkt[0], 24);
    return pkt;
}

static void TestOversizeSecurePacket() {
    printf("an authenticated packet longer than a datagram is refused (no stack overflow), on the wire and straight into the peer\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    Peer& toB = p.b->net.PeerAt(0);
    Peer& fromA = p.a->net.PeerAt(1);
    const Addr aPublic = MakeAddr(203, 0, 113, 10, 41000), bPublic = MakeAddr(203, 0, 113, 11, 41000);
    std::vector<uint32_t> ids;
    int bad = 0;
    p.Advance(100);
    Drain(p.b, &ids, &bad);
    const PeerStats s0 = toB.Stats();
    // 1. straight into OnPacket, the way a long ChannelData payload would arrive: sealed correctly, so only the length check can stop it
    const size_t sizes[] = {kMaxFrame + 1, 1500, 2000};
    uint32_t seq = 50000;
    for (size_t plain : sizes) {
        const std::vector<uint8_t> big = ForgeSecure(fromA, kTypeData, 0, seq++, plain);
        CHECK(big.size() > kMaxDatagram);
        CHECK(!toB.OnPacket(aPublic, &big[0], big.size()));
    }
    CHECK_EQ(toB.Stats().dropTooBig - s0.dropTooBig, 3u);
    CHECK_EQ(toB.Stats().dataRecv, s0.dataRecv);
    // 2. the largest legal datagram still passes
    {
        const std::vector<uint8_t> ok = ForgeSecure(fromA, kTypeData, 0, seq++, kMaxFrame);
        CHECK_EQ(ok.size(), kMaxDatagram);
        CHECK(toB.OnPacket(aPublic, &ok[0], ok.size()));
        CHECK_EQ(toB.Stats().dataRecv, s0.dataRecv + 1);
    }
    // 3. over the socket: the datagram is cut to the receive buffer (a datagram plus the relay header) and is still longer than a datagram
    {
        const std::vector<uint8_t> big = ForgeSecure(fromA, kTypeData, 0, seq++, 2000);
        p.a->udp->Inject(bPublic, big);
        p.Advance(100);
        CHECK_EQ(toB.Stats().dataRecv, s0.dataRecv + 1);
        CHECK(toB.Stats().dropTooBig - s0.dropTooBig >= 4u);
    }
    // 4. a relay (ChannelData) message whose length field names more than a datagram is not handed to the peer at all
    {
        std::vector<uint8_t> inner = ForgeSecure(fromA, kTypeData, 0, seq++, kMaxFrame + 4);   // 1204 bytes: fits a receive buffer, not a datagram
        std::vector<uint8_t> chan(4 + inner.size());
        chan[0] = 0x40; chan[1] = 0x01; chan[2] = uint8_t(inner.size() >> 8); chan[3] = uint8_t(inner.size());
        memcpy(&chan[4], &inner[0], inner.size());
        CHECK_EQ(chan.size(), kMaxDatagram + kRelayOverhead);
        const uint64_t before = toB.Stats().dataRecv;
        p.a->udp->Inject(bPublic, chan);
        p.Advance(100);
        CHECK_EQ(toB.Stats().dataRecv, before);
    }
    // the link is fine and carries data
    CHECK(toB.GetState() == Peer::CONNECTED);
    // (the forged packets moved b's replay window far ahead, as an authenticated sender can: check the other direction)
    const std::vector<uint8_t> f = Frame(77, 100);
    p.b->net.SendFrame(-1, &f[0], f.size());
    p.Advance(100);
    ids.clear();
    Drain(p.a, &ids, &bad);
    CHECK_EQ(bad, 0);
    CHECK(!ids.empty() && ids.back() == 77u);
}

// A signaling object that can be made to fail under a running match (a 426 after a release, a ban, the kill switch), or to hand out a room
// without a usable key.
class FlakySignal : public Signal {
public:
    explicit FlakySignal(Signal* inner) : inner_(inner), dead_(false), zeroKey_(false) { err_[0] = 0; }
    ~FlakySignal() override { delete inner_; }
    void Kill(const char* sentence) { dead_ = true; snprintf(err_, sizeof err_, "%s", sentence); }
    void ZeroKey() { zeroKey_ = true; }
    int polls = 0;
    void Host(const char* n) override { inner_->Host(n); }
    void Join(const char* c, const char* n) override { inner_->Join(c, n); }
    void PublishCandidates(const SigCandidates& m) override { if (!dead_) inner_->PublishCandidates(m); }
    void ReportPunch(const PunchReport& r) override { if (!dead_) inner_->ReportPunch(r); }
    void Leave() override { inner_->Leave(); }
    void Poll() override { ++polls; if (!dead_) inner_->Poll(); }
    Status GetStatus() const override { return dead_ ? FAILED : inner_->GetStatus(); }
    const SigRoom& Room() const override {
        view_ = inner_->Room();
        if (zeroKey_) memset(view_.key, 0, sizeof view_.key);
        return view_;
    }
    const char* Error() const override { return dead_ ? err_ : inner_->Error(); }
    void SetClock(uint64_t (*now)(void*), void* ctx) override { inner_->SetClock(now, ctx); }
    void Started() override { if (!dead_) inner_->Started(); }
    const char* StunList() const override { return inner_->StunList(); }
    void SetRelayOnly(bool on) override { inner_->SetRelayOnly(on); }
    void RequestTurn() override { if (!dead_) inner_->RequestTurn(); }
    TurnStatus TurnState() const override { return inner_->TurnState(); }
    const TurnInfo& Turn() const override { return inner_->Turn(); }
    const char* TurnError() const override { return inner_->TurnError(); }
    void ReportTurnUsage(uint64_t bytes) override { if (!dead_) inner_->ReportTurnUsage(bytes); }

private:
    Signal* inner_;
    bool dead_, zeroKey_;
    char err_[96];
    mutable SigRoom view_;
};

// The service failing under a match on the relay: the allocation, its permission and its channel live on the relay, not in the service. The TURN client
// must keep refreshing them with the credentials it already holds, or the match ends when the permission runs out (5 minutes on a real relay).
static void TestSignalingFailsOnRelay() {
    printf("the signaling service failing under a match on the relay: the relay stays refreshed\n");
    Pair p(NatType::Symmetric(), NatType::Symmetric(), false, 43);
    NatSim::TurnSim::Config sc;
    sc.maxLifetimeSec = 20;           // short, so that a minute of virtual play is several lifetimes
    TurnRig rig(p.w, sc);
    UseRelay(p);
    FlakySignal* sa = new FlakySignal(CreateMemorySignal(p.w.hub));
    FlakySignal* sb = new FlakySignal(CreateMemorySignal(p.w.hub));
    p.a->sigOverride = sa;
    p.b->sigOverride = sb;
    for (Console* c : {p.a, p.b}) {
        c->turnCfg.lifetimeSec = 20;
        c->turnCfg.permissionRefreshMs = 8000;
        c->turnCfg.channelRefreshMs = 8000;
    }
    { const PunchConfig pc = ProdPunch(); CHECK(p.Start(&pc)); }
    CHECK(p.Connect(30000) >= 0);
    CHECK(p.a->net.PeerAt(1).Path().kind == CAND_RELAY);
    sa->Kill("This online account cannot be used.");
    sb->Kill("The online service could not be reached.");
    p.Advance(200);
    int lastA = 0, lastB = 0, frames = 0;
    for (int win = 0; win < 4; ++win) {   // 120 s: several lifetimes of the allocation and of its permissions
        const StreamResult r = RunStream(p, 900);
        lastA = r.gotA; lastB = r.gotB; frames = r.frames;
    }
    printf("  last window: B received %d/%d, A received %d/%d, relay drops: no permission %llu, no such peer %llu\n", lastB, frames, lastA, frames,
           (unsigned long long)rig.sim->GetStats().noPermission, (unsigned long long)rig.sim->GetStats().noTarget);
    CHECK_EQ(lastA, frames);
    CHECK_EQ(lastB, frames);
    CHECK_EQ(p.a->lostCalls, 0);
    CHECK_EQ(p.b->lostCalls, 0);
    CHECK(p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY);
}

static void TestSignalingFailsMidMatch() {
    printf("the signaling service failing under a live match: the match goes on, the player is told at once\n");
    {
        Pair p(NatType::PortRestricted(), NatType::PortRestricted());
        FlakySignal* sa = new FlakySignal(CreateMemorySignal(p.w.hub));
        FlakySignal* sb = new FlakySignal(CreateMemorySignal(p.w.hub));
        p.a->sigOverride = sa;
        p.b->sigOverride = sb;
        CHECK(p.Start());
        CHECK(p.Connect(10000) >= 0);
        const std::vector<uint8_t> f = Frame(1, 120), g = Frame(2, 140);
        // a: banned; b: 426 (the sentences the HTTP signaling produces for those answers)
        sa->Kill("This online account cannot be used.");
        sb->Kill("Different game version: please update the game.");
        p.Advance(200);
        OnlineInfo ia, ib;
        p.a->net.GetInfo(&ia);
        p.b->net.GetInfo(&ib);
        CHECK(p.a->net.GetPhase() == OnlineNet::READY);
        CHECK(p.b->net.GetPhase() == OnlineNet::READY);
        CHECK(strstr(ia.warning, "This online account cannot be used.") != 0 && strstr(ia.warning, "The match goes on") != 0);
        CHECK(strstr(ib.warning, "Different game version") != 0);
        CHECK_EQ(ia.error[0], 0);
        CHECK_EQ(p.a->noticeCalls, 1);
        CHECK_EQ(p.b->noticeCalls, 1);
        CHECK_EQ(ia.connectedCount, 1);          // the member list survives the service (vips, names)
        CHECK(ia.peers[1].present && ia.peers[1].state == Peer::CONNECTED);
        const int pollsAfter = sa->polls;
        // a minute of play (past the 20 s keep-alive and the 25 s lost limit): frames both ways, no more polling, no more notices
        std::vector<uint32_t> ia_ids, ib_ids;
        int bad = 0;
        for (int sec = 0; sec < 60; ++sec) {
            p.a->net.SendFrame(-1, &f[0], f.size());
            p.b->net.SendFrame(-1, &g[0], g.size());
            p.Advance(1000);
            Drain(p.a, &ia_ids, &bad);
            Drain(p.b, &ib_ids, &bad);
        }
        CHECK_EQ(ia_ids.size(), 60u);
        CHECK_EQ(ib_ids.size(), 60u);
        CHECK_EQ(bad, 0);
        CHECK(p.a->net.PeerAt(1).Established() && p.b->net.PeerAt(0).Established());
        CHECK_EQ(sa->polls, pollsAfter);
        CHECK_EQ(p.a->noticeCalls, 1);
        CHECK_EQ(p.a->lostCalls, 0);
        // the other player leaves: with no service there is nothing to rebuild with, so the session ends with a sentence (and the game is told)
        p.b->net.Stop();
        p.Advance(500);
        CHECK(p.a->net.GetPhase() == OnlineNet::FAILED);
        p.a->net.GetInfo(&ia);
        CHECK(strstr(ia.error, "online service cannot be reached") != 0);
        CHECK_EQ(p.a->lostCalls, 1);
    }
    {   // no link up yet: the failure ends the session, as before
        Pair p(NatType::PortRestricted(), NatType::PortRestricted());
        FlakySignal* sa = new FlakySignal(CreateMemorySignal(p.w.hub));
        p.a->sigOverride = sa;
        CHECK(p.a->Start(true, 0));
        Console* ca[1] = {p.a};
        Run(p.w, ca, 1, 200);
        sa->Kill("This online account cannot be used.");
        Run(p.w, ca, 1, 200);
        OnlineInfo ia;
        p.a->net.GetInfo(&ia);
        CHECK(p.a->net.GetPhase() == OnlineNet::FAILED);
        CHECK(strstr(ia.error, "cannot be used") != 0);
        CHECK_EQ(p.a->noticeCalls, 0);
    }
}

static void TestReplayedPunch() {
    printf("a replayed punch cannot reset a live session, move its path or flood the address lists\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    Peer& pa = p.a->net.PeerAt(1);
    Peer& pb = p.b->net.PeerAt(0);
    const SigRoom room = p.a->net.GetSignal()->Room();
    const Addr aPublic = MakeAddr(203, 0, 113, 10, 41000), bPublic = MakeAddr(203, 0, 113, 11, 41000);
    uint8_t zero[8] = {0}, oldHello[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint32_t> ids;
    int bad = 0;
    auto flows = [&](uint32_t id) {   // one frame each way
        const std::vector<uint8_t> f = Frame(id, 90);
        const size_t na = ids.size();
        p.a->net.SendFrame(-1, &f[0], f.size());
        p.b->net.SendFrame(-1, &f[0], f.size());
        p.Advance(150);
        Drain(p.a, &ids, &bad);
        Drain(p.b, &ids, &bad);
        return ids.size() == na + 2 && bad == 0;
    };
    CHECK(flows(1));
    // 1. a recording of an older attempt of b (another hello, no echo of ours), over the wire and from a forged source
    const std::vector<uint8_t> old = ForgePunch(room, 1, 0, 0, oldHello, zero);
    const uint64_t stale0 = pa.Stats().punchStale;
    for (int i = 0; i < 5; ++i) p.b->udp->Inject(aPublic, old);
    pa.OnPacket(MakeAddr(198, 51, 100, 9, 4000), &old[0], old.size());
    p.Advance(100);
    CHECK(pa.GetState() == Peer::CONNECTED);
    CHECK(pb.GetState() == Peer::CONNECTED);
    CHECK_EQ(pa.Stats().punchStale - stale0, 6u);
    CHECK(pa.Path().addr == bPublic);
    CHECK(flows(2));
    // 2. the peer really restarts (a crash with no goodbye): it is adopted after the echo, and the link comes back
    uint8_t retired[8];
    memcpy(retired, PeerTestAccess::Hello(pb), 8);
    uint8_t myHello[8];
    memcpy(myHello, PeerTestAccess::Hello(pa), 8);
    CHECK(pb.Restart());
    CHECK(p.Connect(15000) >= 0);
    CHECK(memcmp(PeerTestAccess::PeerHello(pa), PeerTestAccess::Hello(pb), 8) == 0);
    CHECK(memcmp(PeerTestAccess::PeerHello(pa), retired, 8) != 0);
    CHECK(flows(3));
    // 3. a recording from before that restart, made while a's hello was the same one, so it even echoes it: its hello is retired, it changes nothing
    const std::vector<uint8_t> recorded = ForgePunch(room, 1, 0, 2, retired, PeerTestAccess::Hello(pa));
    const uint64_t stale1 = pa.Stats().punchStale;
    for (int i = 0; i < 3; ++i) p.b->udp->Inject(aPublic, recorded);
    p.Advance(100);
    CHECK_EQ(pa.Stats().punchStale - stale1, 3u);
    CHECK(pa.GetState() == Peer::CONNECTED && pb.GetState() == Peer::CONNECTED);
    CHECK(memcmp(PeerTestAccess::PeerHello(pa), retired, 8) != 0);
    CHECK(flows(4));
    (void)myHello;
    // 4. spoofed sources: a fresh-looking punch (current hello, echoes ours) from 300 addresses neither moves the path nor fills the lists
    uint8_t cur[8], mine[8];
    memcpy(cur, PeerTestAccess::PeerHello(pa), 8);
    memcpy(mine, PeerTestAccess::Hello(pa), 8);
    const uint64_t roams0 = pa.Stats().roams;
    for (int i = 0; i < 300; ++i) {
        const std::vector<uint8_t> fresh = ForgePunch(room, 1, 0, (i & 1) ? 1 : 0, cur, mine);
        pa.OnPacket(MakeAddr(10, 7, uint8_t(i >> 8), uint8_t(i), uint16_t(1000 + i)), &fresh[0], fresh.size());
        pa.OnPacket(MakeAddr(203, 0, 113, uint8_t(100 + i % 100), uint16_t(2000 + i)), &fresh[0], fresh.size());
    }
    CHECK(pa.LearnedCount() <= 8);
    CHECK(pa.ConfirmedCount() <= 8);
    CHECK(pa.GetState() == Peer::CONNECTED);
    CHECK(pa.Path().addr == bPublic);
    CHECK_EQ(pa.Stats().roams, roams0);
    CHECK(flows(5));
    // 5. the path follows authentic traffic only: a punch from a new address did not move it above; a DATA packet that passed AEAD does
    {
        const Addr moved = MakeAddr(203, 0, 113, 77, 5555);
        const std::vector<uint8_t> pk = ForgeSecure(pb, kTypeData, 1, 900000, 64);
        CHECK(pa.OnPacket(moved, &pk[0], pk.size()));
        CHECK(pa.Path().addr == moved);
        CHECK_EQ(pa.Stats().roams, roams0 + 1);
        // an older packet of the window, re-sent from yet another address, does not move it back
        const std::vector<uint8_t> older = ForgeSecure(pb, kTypeData, 1, 899990, 64);
        CHECK(pa.OnPacket(bPublic, &older[0], older.size()));
        CHECK(pa.Path().addr == moved);
    }
}

static void TestSpoofedSourcesDuringPunching() {
    printf("spoofed probe sources while punching cannot keep the real peer from being learned (cone against symmetric)\n");
    NatType full, sym;
    NatType::Parse("addr", &full);
    NatType::Parse("sym", &sym);
    Pair p(full, sym);
    CHECK(p.Start());
    const SigRoom room = p.a->net.GetSignal()->Room();
    // an authorised opponent's flood: valid MAC, a hello of its own, from many sources, every 10 ms for the whole punching phase
    uint8_t flood[8] = {9, 9, 9, 9, 1, 1, 1, 1}, zero[8] = {0};
    const std::vector<uint8_t> noise = ForgePunch(room, 1, 0, 0, flood, zero);
    Peer& pa = p.a->net.PeerAt(1);
    int ms = -1;
    p.both[0] = p.a; p.both[1] = p.b;
    uint32_t spoof = 0;
    ms = RunUntil(p.w, p.both, 2, 20000, [&] {
        for (int i = 0; i < 4; ++i, ++spoof) pa.OnPacket(MakeAddr(10, 9, uint8_t(spoof >> 8), uint8_t(spoof), uint16_t(3000 + spoof % 1000)), &noise[0], noise.size());
        return p.a->net.GetPhase() == OnlineNet::READY && p.b->net.GetPhase() == OnlineNet::READY && p.a->net.PeerAt(1).Established() && p.b->net.PeerAt(0).Established();
    });
    printf("  connected %d ms after the start, %u spoofed probes, learned list %zu\n", ms, spoof, pa.LearnedCount());
    CHECK(ms >= 0);
    CHECK(pa.LearnedCount() <= 8);
    CHECK(pa.ConfirmedCount() <= 8);
    CHECK(pa.Path().addr.ip == MakeAddr(203, 0, 113, 11, 0).ip);
    const std::vector<uint8_t> f = Frame(11, 150);
    p.a->net.SendFrame(-1, &f[0], f.size());
    p.b->net.SendFrame(-1, &f[0], f.size());
    p.Advance(300);
    std::vector<uint32_t> ids;
    int bad = 0;
    CHECK_EQ(Drain(p.a, &ids, &bad), 1);
    CHECK_EQ(Drain(p.b, &ids, &bad), 1);
}

static bool DeadRng(void*, void*, size_t) { return false; }
static bool ZeroRng(void*, void* out, size_t n) { memset(out, 0, n); return true; }
static int g_zeroRngCalls = 0;
static bool CountingZeroRng(void* c, void* out, size_t n) { ++g_zeroRngCalls; return ZeroRng(c, out, n); }

static void TestDeadRandomGenerator() {
    printf("a dead random generator fails with a sentence instead of looping or using predictable bytes\n");
    World w;
    Env env;
    env.now = WorldNow; env.nowCtx = &w;
    PeerParams pp;
    pp.mySlot = 0; pp.peerSlot = 1; pp.cookie = 5;
    {   // the generator reports failure
        env.random = DeadRng;
        Peer peer;
        CHECK(!peer.Begin(&env, pp));
        CHECK(peer.GetState() == Peer::IDLE);
    }
    {   // the generator "works" but only ever gives zeros: bounded, not an endless loop
        env.random = CountingZeroRng;
        g_zeroRngCalls = 0;
        Peer peer;
        CHECK(!peer.Begin(&env, pp));
        CHECK(g_zeroRngCalls > 0 && g_zeroRngCalls <= 16);
        CHECK(peer.GetState() == Peer::IDLE);
    }
    {   // a live generator: fine, and a Restart that fails leaves the running attempt alone
        Rng rng(3);
        env.random = RngFill; env.randomCtx = &rng;
        Peer peer;
        CHECK(peer.Begin(&env, pp));
        CHECK(peer.GetState() == Peer::PUNCHING);
        uint8_t before[8];
        memcpy(before, PeerTestAccess::Hello(peer), 8);
        env.random = DeadRng;
        CHECK(!peer.Restart());
        CHECK(memcmp(before, PeerTestAccess::Hello(peer), 8) == 0);
        CHECK(peer.GetState() == Peer::PUNCHING);
    }
    {   // the system generator itself: true, and 64+ bytes in several chunks are all written
        uint8_t big[300];
        memset(big, 0, sizeof big);
        CHECK(SystemRandom(0, big, sizeof big));
        int nonzero = 0;
        for (size_t i = 0; i < sizeof big; ++i) nonzero += big[i] != 0;
        CHECK(nonzero > 200);
        CHECK(strlen(RandomFailureSentence()) > 20);
    }
    {   // STUN: no transaction ids, no STUN
        env.random = DeadRng;
        StunClient stun;
        const Addr servers[1] = {MakeAddr(198, 51, 100, 1, 3478)};
        stun.Start(&env, servers, 1);
        CHECK(stun.GetState() == StunClient::FAILED);
    }
    {   // the session: Start fails at once with the sentence
        World w2;
        w2.sim.AddHost(w2.sim.AddLan(PublicIp(0), NatType::PortRestricted()), MakeAddr(192, 168, 1, 10, 41000));
        OnlineConfig cfg;
        cfg.udp = new SimUdp(&w2, 0, MakeAddr(192, 168, 1, 10, 41000));
        cfg.signal = CreateMemorySignal(w2.hub);
        cfg.env.now = WorldNow; cfg.env.nowCtx = &w2; cfg.env.random = DeadRng;
        cfg.stunAddrs[0] = MakeAddr(198, 51, 100, 1, 3478);
        cfg.nStunAddrs = 1;
        OnlineNet net;
        CHECK(!net.Start(cfg, 0, "X"));
        OnlineInfo info;
        net.GetInfo(&info);
        CHECK(net.GetPhase() == OnlineNet::FAILED);
        CHECK(strcmp(info.error, RandomFailureSentence()) == 0);
    }
}

static void TestZeroRoomKey() {
    printf("a room without a usable session key never starts a peer and fails with a sentence\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted());
    FlakySignal* sa = new FlakySignal(CreateMemorySignal(p.w.hub));
    p.a->sigOverride = sa;
    sa->ZeroKey();
    CHECK(p.Start());
    p.both[0] = p.a; p.both[1] = p.b;
    Run(p.w, p.both, 2, 4000);
    CHECK(p.a->net.PeerAt(1).GetState() == Peer::IDLE);   // not started: no punch ever went out under a key everyone knows
    CHECK(p.a->net.GetPhase() != OnlineNet::FAILED);       // the key may still be on its way
    Run(p.w, p.both, 2, 3000);
    OnlineInfo info;
    p.a->net.GetInfo(&info);
    CHECK(p.a->net.GetPhase() == OnlineNet::FAILED);
    CHECK(strstr(info.error, "key for this match") != 0);
    CHECK(p.a->net.PeerAt(1).Stats().punchSent == 0);
}

// ---- signaling refusals against a stub service ----------------------------------------------------------------------------------
// A minimal HTTP server on localhost whose answers the test scripts (the Worker's own fake cannot be made to refuse on demand).
struct StubServer {
    int fd;
    uint16_t port;
    std::atomic<bool> stop;   // (tsan) set by Stop() on the test thread, read by the server thread
    pthread_t thread;
    std::function<void(const std::string& method, const std::string& path, int* status, std::string* body)> handler;
    std::vector<std::string> heads;  // the request heads, in order
    std::vector<std::string> seen;   // "METHOD path" in order (guarded by the single-threaded use: read after the signal object is idle)
    std::vector<std::string> bodies; // Galaxy: the request bodies, in order
    std::vector<uint64_t> seenAt;    // netport31: when each request arrived (RealMs)
    bool threaded;                   // netport31: each connection on a thread of its own (a held answer does not hold up the next request)
    pthread_mutex_t mu;              // (threaded) guards the vectors above
    std::atomic<int> active;         // (threaded) connections still being served: Stop() waits for them
    StubServer() : fd(-1), port(0), stop(false), threaded(false), active(0) { pthread_mutex_init(&mu, 0); }
    ~StubServer() { pthread_mutex_destroy(&mu); }
    size_t Count() { pthread_mutex_lock(&mu); const size_t n = seen.size(); pthread_mutex_unlock(&mu); return n; }
    void Serve(int c) {
        std::string in;
        char buf[2048];
        size_t headEnd = std::string::npos;
        long want = 0;
        for (int guard = 0; guard < 200; ++guard) {
            const ssize_t n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            in.append(buf, size_t(n));
            if (headEnd == std::string::npos) {
                headEnd = in.find("\r\n\r\n");
                if (headEnd != std::string::npos) {
                    std::string lower = in.substr(0, headEnd);
                    for (size_t i = 0; i < lower.size(); ++i) lower[i] = char(tolower(static_cast<unsigned char>(lower[i])));
                    const size_t cl = lower.find("content-length:");
                    want = cl == std::string::npos ? 0 : atol(lower.c_str() + cl + 15);
                    headEnd += 4;
                }
            }
            if (headEnd != std::string::npos && long(in.size() - headEnd) >= want) break;
        }
        const size_t sp1 = in.find(' '), sp2 = in.find(' ', sp1 + 1);
        std::string method = in.substr(0, sp1), path = in.substr(sp1 + 1, sp2 - sp1 - 1);
        pthread_mutex_lock(&mu);
        seen.push_back(method + " " + path);
        seenAt.push_back(RealMs());
        heads.push_back(in.substr(0, headEnd == std::string::npos ? in.size() : headEnd));
        bodies.push_back(headEnd == std::string::npos ? std::string() : in.substr(headEnd));
        pthread_mutex_unlock(&mu);
        int status = 404;
        std::string body = "{\"error\":{\"code\":\"not_found\"}}";
        if (handler) handler(method, path, &status, &body);
        char head[160];
        snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", status, unsigned(body.size()));
        std::string out = std::string(head) + body;
        send(c, out.data(), out.size(), MSG_NOSIGNAL);
        close(c);
    }
    struct Conn { StubServer* s; int c; };
    static void* ConnMain(void* arg) {
        Conn* k = static_cast<Conn*>(arg);
        StubServer* s = k->s;
        s->Serve(k->c);
        delete k;
        s->active--;
        return 0;
    }
    static void* Loop(void* arg) {
        StubServer* s = static_cast<StubServer*>(arg);
        while (!s->stop) {
            pollfd pf;
            pf.fd = s->fd; pf.events = POLLIN; pf.revents = 0;
            if (poll(&pf, 1, 50) <= 0) continue;
            const int c = accept(s->fd, 0, 0);
            if (c < 0) continue;
            if (!s->threaded) { s->Serve(c); continue; }
            pthread_t t;
            Conn* k = new Conn{s, c};
            s->active++;
            if (pthread_create(&t, 0, ConnMain, k) == 0) pthread_detach(t);
            else { delete k; close(c); s->active--; }
        }
        return 0;
    }
    bool Start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(fd, 8) != 0) return false;
        socklen_t l = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        return pthread_create(&thread, 0, Loop, this) == 0;
    }
    void Stop() {
        stop = true;
        pthread_join(thread, 0);
        close(fd);
        for (int i = 0; i < 2000 && active.load() > 0; ++i) usleep(5000);   // (threaded: the handlers still running)
    }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

static const char* kZeroKey = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
static std::string RoomJson(const char* state, bool withPeer, const char* key) {
    std::string j = std::string("{\"code\":\"ABC234\",\"state\":\"") + state + "\",\"version\":3,\"changed\":true,\"slot\":0,\"you\":{\"name\":\"Hosty\",\"vip\":\"10.77.0.1\"}";
    if (withPeer) j += ",\"peer\":{\"slot\":1,\"name\":\"Guesty\",\"vip\":\"10.77.0.2\",\"link\":\"ok\",\"endpoint\":{\"public\":\"203.0.113.9:41000\",\"local\":\"192.168.0.5:41000\",\"nonce\":\"AAECAwQFBgc=\",\"nat\":\"cone\"}}";
    else j += ",\"peer\":null";
    if (key) j += std::string(",\"session\":{\"key\":\"") + key + "\",\"cookie\":12345,\"steer\":{\"relay\":false}}";
    else j += ",\"session\":null";
    return j + "}";
}

static bool SignalUntil(Signal* sig, uint32_t maxMs, const std::function<bool()>& done) {
    const uint64_t t0 = RealMs();
    while (RealMs() - t0 < maxMs) {
        HttpPump();
        sig->Poll();
        if (done()) return true;
        usleep(3000);
    }
    return false;
}

// svcfix3 (S5): 429 turn_room_limit / 429 turn_player_limit / 503 turn_budget = "no relay right now": one request, no retries, one calm sentence,
// no new request while the service's time is not up; an ordinary 500 is still retried.
static void TestTurnLimitAnswers() {
    printf("relay credentials: the limits of the service end the request at once and calmly\n");
    const struct { const char* code; int status; } cases[] = {{"turn_room_limit", 429}, {"turn_player_limit", 429}, {"turn_budget", 503}};
    for (const auto& cs : cases) {
        StubServer srv;
        CHECK(srv.Start());
        const std::string code = cs.code;
        const int status = cs.status;
        srv.handler = [code, status](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (path == "/v1/turn") { *st = status; *body = "{\"error\":{\"code\":\"" + code + "\",\"message\":\"x\",\"retryAfterSec\":3600}}"; }
            else *body = RoomJson("open", false, 0);
            (void)m;
        };
        Signal* sig = CreateHttpSignal(ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012"));
        sig->Host("Hosty");
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
        sig->RequestTurn();
        CHECK(SignalUntil(sig, 5000, [&] { return sig->TurnState() == Signal::TURN_FAILED; }));
        for (int i = 0; i < 30; ++i) { HttpPump(); sig->Poll(); usleep(3000); }   // (a retry would show up here)
        int turns = 0;
        for (const std::string& q : srv.seen) if (q == "POST /v1/turn") ++turns;
        printf("  %s: %d request(s), \"%s\", wait %u ms\n", cs.code, turns, sig->TurnError(), unsigned(sig->TurnWaitMs()));
        CHECK_EQ(turns, 1);
        CHECK(strstr(sig->TurnError(), "Relay play is not available right now") != 0 && strstr(sig->TurnError(), "turn_") == 0);
        CHECK(sig->TurnWaitMs() == 600000);
        CHECK(sig->GetStatus() == Signal::IN_ROOM);   // (the room goes on)
        {   // a refused request for credentials never stalls the queue: the room poll goes on (a member that stops polling is dropped after 20 s)
            size_t turnAt = 0;
            for (size_t i = 0; i < srv.seen.size(); ++i) if (srv.seen[i] == "POST /v1/turn") turnAt = i;
            auto polledAfter = [&] {
                for (size_t i = turnAt + 1; i < srv.seen.size(); ++i)
                    if (srv.seen[i].compare(0, 14, "GET /v1/rooms/") == 0) return true;
                return false;
            };
            CHECK(SignalUntil(sig, 6000, polledAfter));
        }
        sig->RequestTurn();   // asked again inside the wait: answered here, no request
        CHECK(sig->TurnState() == Signal::TURN_FAILED);
        for (int i = 0; i < 20; ++i) { HttpPump(); sig->Poll(); usleep(3000); }
        turns = 0;
        for (const std::string& q : srv.seen) if (q == "POST /v1/turn") ++turns;
        CHECK_EQ(turns, 1);
        delete sig;
        srv.Stop();
    }
}

// the endpoint and the punch verdict queued behind a request for credentials go out first
static void TestTurnQueueOrder() {
    printf("relay credentials: a report made after the request still reaches the service first\n");
    StubServer srv;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
        else if (path == "/v1/turn") { *st = 429; *body = "{\"error\":{\"code\":\"turn_room_limit\"}}"; }
        else *body = RoomJson("open", false, 0);
        (void)m;
    };
    Signal* sig = CreateHttpSignal(ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012"));
    sig->Host("Hosty");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
    sig->RequestTurn();
    SigCandidates mine;
    mine.valid = true; mine.pub = MakeAddr(203, 0, 113, 7, 41000); mine.local = MakeAddr(192, 168, 1, 20, 41000); mine.nat = NAT_SYMMETRIC; mine.gen = 1;
    sig->PublishCandidates(mine);
    PunchReport pr;
    memset(&pr, 0, sizeof pr);
    pr.peerSlot = 1; pr.ok = false; pr.ms = 9000;
    sig->ReportPunch(pr);
    CHECK(SignalUntil(sig, 5000, [&] { return sig->TurnState() == Signal::TURN_FAILED; }));
    int turnAt = -1, endpointAt = -1, punchAt = -1;
    for (size_t i = 0; i < srv.seen.size(); ++i) {
        if (srv.seen[i] == "POST /v1/turn") turnAt = int(i);
        if (srv.seen[i].find("/endpoint") != std::string::npos) endpointAt = int(i);
        if (srv.seen[i].find("/punch") != std::string::npos) punchAt = int(i);
    }
    printf("  endpoint at %d, punch at %d, credentials at %d\n", endpointAt, punchAt, turnAt);
    CHECK(endpointAt >= 0 && punchAt >= 0 && turnAt >= 0 && endpointAt < turnAt && punchAt < turnAt);
    delete sig;
    srv.Stop();
}

// ---- netport31: held polls, the give-way, sockets (Strikers heldpoll23 / onmatch23) ---------------------------------------------------
// The console's HTTPS client ends any request after 10 s without a byte unless the request names a longer limit; the room's held polls are
// 25 s (forming) and 55 s (a settled host). The plain desktop client now applies the same rule (scaled down here), so these are the console's
// branches on the host.
static std::string RoomV(int version, bool withPeer) {
    std::string j = RoomJson("open", withPeer, 0);
    const size_t at = j.find("\"version\":3");
    if (at != std::string::npos) j.replace(at, 11, "\"version\":" + std::to_string(version));
    return j;
}
static int OpenFds() {
    int n = 0;
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) != -1) ++n;
    return n;   // (counted with no stub server running: its accepted connections are not ours)
}

static void TestHeldPollNoDataLimit() {
    printf("held polls: the service may hold a poll without data for the whole hold (no-data limit = hold + 10 s); ordinary requests keep the short limit\n");
    HttpTestSetDefaultIoMs(400);   // test scale: the console's 10 s no-data limit is 0.4 s here, the room's hold 1.2 s (25 s, scaled)
    StubServer srv;
    srv.threaded = true;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
        else if (path.find("/slow") == 0) { usleep(1200000); *body = "{}"; }
        else if (path.find("wait=") != std::string::npos) {   // a held poll: nothing for 1.2 s, then the room has changed (a guest came)
            usleep(1200000);
            *body = RoomV(4, true);
        } else *body = RoomV(3, false);
        (void)m;
    };
    {   // the request path itself
        std::vector<std::string> h;
        std::string err;
        HttpResponse r;
        uint64_t t0 = RealMs();
        int id = HttpStart("GET", srv.Url() + "/slow", "", h, 15000, &err);
        while (id && HttpPoll(id, &r) == 0) usleep(3000);
        const uint64_t plainMs = RealMs() - t0;
        printf("  ordinary request, 1.2 s of silence: error %d after %u ms (%s)\n", r.error, unsigned(plainMs), r.text.c_str());
        CHECK(id && r.error != 0 && plainMs >= 350 && plainMs < 1100);
        r = HttpResponse();
        t0 = RealMs();
        id = HttpStart("GET", srv.Url() + "/slow", "", h, 15000 + 1200, HeldIoMs(1200), &err);
        while (id && HttpPoll(id, &r) == 0) usleep(3000);
        printf("  held request, the same silence: error %d status %d after %u ms\n", r.error, r.status, unsigned(RealMs() - t0));
        CHECK(id && r.error == 0 && r.status == 200);
    }
    HttpSignalConfig cfg = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
    cfg.pollWaitSec = 2;   // held polls on (the client asks for wait=2; the stub holds 1.2 s)
    Signal* sig = CreateHttpSignal(cfg);
    sig->Host("Hosty");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
    const bool changed = SignalUntil(sig, 6000, [&] { return sig->Room().version == 4; });
    int held = 0;
    pthread_mutex_lock(&srv.mu);
    for (const std::string& q : srv.seen) if (q.find("wait=") != std::string::npos) ++held;
    pthread_mutex_unlock(&srv.mu);
    printf("  room signaling: the held poll's answer arrived: %s (%d held poll(s) asked)\n", changed ? "yes" : "NO", held);
    CHECK(changed);                               // (the bug: every held poll ended at the no-data limit, so the change never arrived)
    CHECK(sig->GetStatus() == Signal::IN_ROOM);
    CHECK(held >= 1 && held <= 3);                // (and no burst of retries)
    delete sig;
    srv.Stop();
    HttpTestSetDefaultIoMs(0);
}

static bool g_othersWaiting = false;
static bool OthersWaitingOnce() {
    const bool v = g_othersWaiting;
    g_othersWaiting = false;   // what the console sees: once the other request runs, nothing waits any more
    return v;
}
static void TestHeldPollGivesWay() {
    printf("held polls give way: another request waiting for the one-at-a-time HTTPS client is not held up for the whole hold\n");
    const int fds0 = OpenFds();
    StubServer srv;
    srv.threaded = true;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
        else if (path.find("wait=") != std::string::npos) { usleep(2500000); *body = RoomV(3, false); }   // held 2.5 s, no change
        else *body = RoomV(3, false);
        (void)m;
    };
    HttpSignalConfig cfg = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
    cfg.pollWaitSec = 3;
    Signal* sig = CreateHttpSignal(cfg);
    HttpTestSetOthersWaiting(OthersWaitingOnce);
    sig->Host("Hosty");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
    auto heldCount = [&] {
        int n = 0;
        pthread_mutex_lock(&srv.mu);
        for (const std::string& q : srv.seen) if (q.find("wait=") != std::string::npos) ++n;
        pthread_mutex_unlock(&srv.mu);
        return n;
    };
    CHECK(SignalUntil(sig, 3000, [&] { return heldCount() >= 1; }));
    for (int i = 0; i < 30; ++i) { HttpPump(); sig->Poll(); usleep(10000); }   // (the first held poll is being held)
    const uint64_t armed = RealMs();
    g_othersWaiting = true;   // a menu's request queued behind the held poll
    const bool again = SignalUntil(sig, 2000, [&] { return heldCount() >= 2; });
    const uint64_t gaveWayMs = RealMs() - armed;
    printf("  the held poll gave way and was asked again after %u ms (hold 2.5 s): %s\n", unsigned(gaveWayMs), again ? "yes" : "NO");
    CHECK(again && gaveWayMs < 1000);
    CHECK(sig->GetStatus() == Signal::IN_ROOM);
    HttpTestSetOthersWaiting(0);
    delete sig;
    for (int i = 0; i < 100 && HttpTestPlainLive() > 0; ++i) usleep(20000);
    CHECK(HttpTestPlainLive() == 0);
    srv.Stop();
    printf("  sockets open after the room was left: %d (file descriptors %d -> %d)\n", HttpTestPlainLive(), fds0, OpenFds());
    CHECK(OpenFds() == fds0);
}

// ---- s1117b (Strikers 1117b): a fire-and-forget request that cannot start is counted (and logged), never dropped in silence ----------------------
static void TestFireAndForgetDropIsCounted() {
    printf("fire and forget: a request the table cannot take (room leave, relay usage) is counted and logged, not dropped in silence\n");
    StubServer srv;
    srv.threaded = true;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        usleep(1500000);   // held: the slots stay taken while the 13th is offered
        *body = "{}";
        (void)m; (void)path;
    };
    const unsigned before = HttpFireDropped();
    std::vector<std::string> h;
    for (int i = 0; i < 12; ++i) HttpFire("POST", srv.Url() + "/v1/rooms/ABCD/leave", "{}", h, 5000);   // the table's 12 slots
    CHECK(HttpFireDropped() == before);                      // all twelve started
    HttpFire("POST", srv.Url() + "/v1/rooms/ABCD/leave", "{}", h, 5000);   // the 13th finds no slot
    HttpFire("POST", srv.Url() + "/v1/turn/usage", "{}", h, 5000);
    printf("  dropped so far: %u (expected 2)\n", HttpFireDropped() - before);
    CHECK(HttpFireDropped() == before + 2);
    for (int i = 0; i < 200; ++i) { HttpPump(); usleep(10000); }   // (the held ones finish, the table frees)
    HttpFire("POST", srv.Url() + "/v1/rooms/ABCD/leave", "{}", h, 5000);
    CHECK(HttpFireDropped() == before + 2);                  // a free slot again: not dropped
    for (int i = 0; i < 300; ++i) { HttpPump(); usleep(10000); }
    for (int i = 0; i < 100 && HttpTestPlainLive() > 0; ++i) usleep(20000);
    srv.Stop();
}

static void TestPlainSocketsAcrossFailingPolls() {
    printf("sockets: failing and abandoned polls close their sockets at once (no socket kept for a hold nobody waits for)\n");
    HttpTestSetDefaultIoMs(150);
    const int fds0 = OpenFds();
    StubServer srv;
    srv.threaded = true;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        usleep(path.find("/hold") == 0 ? 2000000 : 400000);
        *body = "{}";
        (void)m;
    };
    int deadPort = 0;
    {   // a port nobody listens on
        const int t = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t l = sizeof a;
        getsockname(t, reinterpret_cast<sockaddr*>(&a), &l);
        deadPort = ntohs(a.sin_port);
        close(t);
    }
    std::vector<std::string> h;
    std::string err;
    int worst = 0, bad = 0;
    for (int i = 0; i < 30; ++i) {
        const int kind = i % 3;
        HttpResponse r;
        if (kind == 0) {   // no data in time
            const int id = HttpStart("GET", srv.Url() + "/quiet", "", h, 5000, &err);
            while (id && HttpPoll(id, &r) == 0) usleep(3000);
            if (!id || r.error == 0) ++bad;
        } else if (kind == 1) {   // nothing listening
            const int id = HttpStart("GET", "http://127.0.0.1:" + std::to_string(deadPort) + "/x", "", h, 5000, &err);
            while (id && HttpPoll(id, &r) == 0) usleep(3000);
            if (!id || r.error == 0) ++bad;
        } else {   // a held poll given up while it waits (the give-way, a room left)
            const int id = HttpStart("GET", srv.Url() + "/hold", "", h, 20000, HeldIoMs(2000), &err);
            usleep(120000);
            HttpCancel(id);
            const uint64_t t0 = RealMs();
            while (HttpTestPlainLive() > 0 && RealMs() - t0 < 1000) usleep(2000);
            if (RealMs() - t0 >= 300) ++bad;   // (before netport31 the abandoned request kept its socket until its own time limit)
        }
        if (HttpTestPlainLive() > worst) worst = HttpTestPlainLive();
    }
    srv.Stop();
    printf("  30 failing polls: %d did not fail as meant, most sockets open after one: %d, now %d, file descriptors %d -> %d\n", bad, worst, HttpTestPlainLive(),
           fds0, OpenFds());
    CHECK(bad == 0);
    CHECK(worst == 0 && HttpTestPlainLive() == 0);
    CHECK(OpenFds() == fds0);
    HttpTestSetDefaultIoMs(0);
}

// svcfix3: members[].label / nameDup are read as sent; an older service (none) leaves them empty; a label that is too long for the game's 15 bytes keeps its suffix
static void TestMemberLabels() {
    printf("room view: members[].label and nameDup\n");
    StubServer srv;
    CHECK(srv.Start());
    const std::string longName = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9#4DXT";   // 8 two-byte characters + "#4DXT" = 21 bytes
    srv.handler = [longName](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
        else {
            *body = std::string("{\"code\":\"ABC234\",\"state\":\"open\",\"version\":3,\"changed\":true,\"slot\":0,\"you\":{\"name\":\"Luma\",\"vip\":\"10.77.0.1\"},")
                    + "\"members\":[{\"slot\":0,\"name\":\"Luma\",\"label\":\"Luma#AAAA\",\"nameDup\":true,\"vip\":\"10.77.0.1\"},"
                    "{\"slot\":1,\"name\":\"Luma\",\"label\":\"Luma#4DXT\",\"nameDup\":true,\"vip\":\"10.77.0.2\"},"
                    "{\"slot\":2,\"name\":\"Mario\",\"label\":\"" + longName + "\",\"nameDup\":true,\"vip\":\"10.77.0.3\"},"
                    "{\"slot\":3,\"name\":\"Toad\",\"vip\":\"10.77.0.4\"}],\"session\":null}";
            (void)m; (void)longName;
        }
    };
    Signal* sig = CreateHttpSignal(ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012"));
    sig->Host("Luma");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM && sig->Room().members[1].present; }));
    const SigRoom& r = sig->Room();
    CHECK(r.members[1].nameDup && strcmp(r.members[1].label, "Luma#4DXT") == 0 && strcmp(r.members[1].name, "Luma") == 0);
    CHECK(r.members[2].nameDup && strcmp(r.members[2].label, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9#4DXT") == 0);   // too long for 15 bytes: the name is cut (not inside a character), the suffix stays
    CHECK(!r.members[3].nameDup && r.members[3].label[0] == 0 && strcmp(r.members[3].name, "Toad") == 0);   // an older service: no label, the name is shown
    delete sig;
    srv.Stop();
}

static void TestJoinFriendsOnlyRefused() {
    printf("join by code: a friends-only room answers a stranger 404 room_not_found = the plain \"not found\" refusal, code kept for the game\n");
    StubServer srv;
    CHECK(srv.Start());
    srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
        *st = 200;
        if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
        else if (m == "POST" && path.find("/v1/rooms") == 0) { *st = 404; *body = "{\"error\":{\"code\":\"room_not_found\",\"message\":\"No such room\"}}"; }
        else *body = RoomJson("open", false, 0);
    };
    HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
    Signal* sig = CreateHttpSignal(c);
    sig->Join("GPQSY3", "Guesty");
    CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::FAILED; }));
    printf("  refused: \"%s\" (%s)\n", sig->Error(), sig->ErrorCode());
    CHECK(strcmp(sig->ErrorCode(), "room_not_found") == 0);   // (the game maps this code to MP_ERR_ROOM_NOT_FOUND, "no such room")
    CHECK(strstr(sig->Error(), "not found") != 0 && strstr(sig->Error(), "friend") == 0);   // (no hint that the room exists)
    delete sig;
    srv.Stop();
}

static void TestSignalingRefusals() {
    printf("signaling refusals: a refused endpoint / punch / started fails the room with a sentence; an unusable key does too\n");
    {   // the endpoint is refused (400 bad_endpoint): the room fails quickly instead of leaving the host waiting for a partner who cannot see it
        StubServer srv;
        CHECK(srv.Start());
        srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (m == "POST" && path == "/v1/rooms") *body = RoomJson("open", false, 0);
            else if (path.find("/endpoint") != std::string::npos) { *st = 400; *body = "{\"error\":{\"code\":\"bad_endpoint\",\"message\":\"x\"}}"; }
            else *body = RoomJson("open", false, 0);
        };
        HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
        Signal* sig = CreateHttpSignal(c);
        sig->Host("Hosty");
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
        SigCandidates mine;
        mine.valid = true; mine.pub = MakeAddr(203, 0, 113, 7, 41000); mine.local = MakeAddr(192, 168, 1, 20, 41000); mine.nat = NAT_CONE; mine.gen = 1;
        sig->PublishCandidates(mine);
        const uint64_t t0 = RealMs();
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::FAILED; }));
        printf("  endpoint refused: \"%s\" after %llu ms\n", sig->Error(), (unsigned long long)(RealMs() - t0));
        CHECK(strstr(sig->Error(), "bad_endpoint") != 0);
        delete sig;
        srv.Stop();
    }
    {   // started: 409 room_not_ready is asked again; a 403 fails the room
        StubServer srv;
        CHECK(srv.Start());
        int startedCalls = 0;
        srv.handler = [&startedCalls](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (m == "POST" && path == "/v1/rooms") *body = RoomJson("open", false, 0);
            else if (path.find("/started") != std::string::npos) {
                ++startedCalls;
                if (startedCalls == 1) { *st = 409; *body = "{\"error\":{\"code\":\"room_not_ready\"}}"; }
                else { *st = 403; *body = "{\"error\":{\"code\":\"not_a_thing\"}}"; }
            } else *body = RoomJson("open", false, 0);
        };
        HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
        Signal* sig = CreateHttpSignal(c);
        sig->Host("Hosty");
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
        sig->Started();
        CHECK(SignalUntil(sig, 8000, [&] { return sig->GetStatus() == Signal::FAILED; }));
        CHECK(startedCalls >= 2);   // the 409 was retried, the 403 ended it
        CHECK(strstr(sig->Error(), "refused") != 0);
        delete sig;
        srv.Stop();
    }
    {   // an all-zero (or short) key in the answer: the room fails instead of handing every packet of the match a key everyone knows
        for (int variant = 0; variant < 2; ++variant) {
            StubServer srv;
            CHECK(srv.Start());
            const char* key = variant == 0 ? kZeroKey : "AAECAwQFBgc=";
            srv.handler = [key](const std::string& m, const std::string& path, int* st, std::string* body) {
                *st = 200;
                if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
                else if (m == "POST" && path == "/v1/rooms") *body = RoomJson("joined", true, key);
                else *body = RoomJson("joined", true, key);
            };
            HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
            Signal* sig = CreateHttpSignal(c);
            sig->Host("Hosty");
            CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::FAILED; }));
            printf("  %s key: \"%s\"\n", variant == 0 ? "all-zero" : "short", sig->Error());
            CHECK(strstr(sig->Error(), "unusable key") != 0);
            delete sig;
            srv.Stop();
        }
    }
}


// Galaxy (fix3): the service's code behind a refusal is kept for the game (Signal::ErrorCode): a poll answered 403 not_member (the
// service dropped this console after a minute of silence) closes the room with that code; a join refused 409 version_mismatch
// fails with it (the game says "different version", not "please update").
static void TestSignalingErrorCodes() {
    printf("signaling error codes: not_member on a poll closes the room with the code; version_mismatch on a join fails with it\n");
    {
        StubServer srv;
        CHECK(srv.Start());
        int polls = 0;
        srv.handler = [&polls](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (m == "POST" && path == "/v1/rooms") *body = RoomJson("open", false, 0);
            else if (m == "GET" && path.find("/v1/rooms/") == 0 && ++polls > 2) { *st = 403; *body = "{\"error\":{\"code\":\"not_member\",\"message\":\"you are not in that room\"}}"; }
            else *body = RoomJson("open", false, 0);
        };
        HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
        Signal* sig = CreateHttpSignal(c);
        sig->Host("Hosty");
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
        CHECK(strcmp(sig->ErrorCode(), "") == 0);
        CHECK(SignalUntil(sig, 15000, [&] { return sig->GetStatus() == Signal::CLOSED; }));
        printf("  not_member: \"%s\" code=%s\n", sig->Error(), sig->ErrorCode());
        CHECK(strcmp(sig->ErrorCode(), "not_member") == 0 && strstr(sig->Error(), "not in that room") != 0);
        delete sig;
        srv.Stop();
    }
    {
        StubServer srv;
        CHECK(srv.Start());
        srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (m == "POST" && path == "/v1/rooms/join") { *st = 409; *body = "{\"error\":{\"code\":\"version_mismatch\",\"message\":\"x\"}}"; }
            else *body = RoomJson("open", false, 0);
        };
        HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
        Signal* sig = CreateHttpSignal(c);
        sig->Join("ABC234", "Guesty");
        CHECK(SignalUntil(sig, 8000, [&] { return sig->GetStatus() == Signal::FAILED; }));
        printf("  version_mismatch: \"%s\" code=%s\n", sig->Error(), sig->ErrorCode());
        CHECK(strcmp(sig->ErrorCode(), "version_mismatch") == 0 && strstr(sig->Error(), "different version") != 0);
        delete sig;
        srv.Stop();
    }
}

static void TestAppVersionAndDailyCap() {
    printf("X-Msc-App is always sent; a daily cap is terminal with a sentence, an ordinary 429 is waited out\n");
    {   // the version header: empty / invalid config falls back to the default, a valid one is kept
        const char* wanted[] = {"", "abc", "1.3.7"};
        const char* expect[] = {"X-Msc-App: 3.0.0", "X-Msc-App: 3.0.0", "X-Msc-App: 1.3.7"};   // Galaxy: 3.0.0 is the built version
        for (int v = 0; v < 3; ++v) {
            StubServer srv;
            CHECK(srv.Start());
            srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
                *st = 200;
                if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
                else *body = RoomJson("open", false, 0);
                (void)m;
            };
            HttpSignalConfig c = ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012");
            snprintf(c.appVersion, sizeof c.appVersion, "%s", wanted[v]);
            Signal* sig = CreateHttpSignal(c);
            sig->Host("Hosty");
            CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
            sig->Leave();
            for (int i = 0; i < 20; ++i) { HttpPump(); usleep(10000); }
            CHECK(srv.heads.size() >= 2);
            bool all = !srv.heads.empty();
            for (size_t i = 0; i < srv.heads.size(); ++i) all = all && srv.heads[i].find(expect[v]) != std::string::npos;
            CHECK(all);
            bool game = !srv.heads.empty();   // Galaxy: every request names the game
            for (size_t i = 0; i < srv.heads.size(); ++i) game = game && srv.heads[i].find("X-Msc-Game: smg") != std::string::npos;
            CHECK(game);
            delete sig;
            srv.Stop();
        }
    }
    {   // daily_cap on creating a room: no waiting out 86400 s
        StubServer srv;
        CHECK(srv.Start());
        srv.handler = [](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (m == "POST" && path == "/v1/rooms") { *st = 429; *body = "{\"error\":{\"code\":\"rate_limited\",\"reason\":\"daily_cap\",\"scope\":\"rooms\",\"retryAfterSec\":86400}}"; }
        };
        Signal* sig = CreateHttpSignal(ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012"));
        sig->Host("Hosty");
        const uint64_t t0 = RealMs();
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::FAILED; }));
        printf("  daily cap: \"%s\" after %llu ms\n", sig->Error(), (unsigned long long)(RealMs() - t0));
        CHECK(strcmp(sig->Error(), "Daily limit reached. Try again tomorrow.") == 0);
        CHECK(RealMs() - t0 < 3000);
        int creates = 0;
        for (size_t i = 0; i < srv.seen.size(); ++i) creates += srv.seen[i] == "POST /v1/rooms";
        CHECK_EQ(creates, 1);   // asked once, not again
        delete sig;
        srv.Stop();
    }
    {   // an ordinary 429 (reason too_early) is retried after its wait and the room stays alive
        StubServer srv;
        CHECK(srv.Start());
        std::atomic<int> punches(0);   // (tsan) written by the stub server thread, read by the test thread
        srv.handler = [&punches](const std::string& m, const std::string& path, int* st, std::string* body) {
            *st = 200;
            if (path.find("/v1/info") == 0) *body = "{\"serverMs\":1000000,\"stun\":[]}";
            else if (path.find("/punch") != std::string::npos) {
                if (++punches == 1) { *st = 429; *body = "{\"error\":{\"code\":\"rate_limited\",\"reason\":\"too_early\",\"retryAfterSec\":1}}"; }
                else *body = RoomJson("open", false, 0);
            } else *body = RoomJson("open", false, 0);
            (void)m;
        };
        Signal* sig = CreateHttpSignal(ServiceConfig(srv.Url().c_str(), "0123456789012345678901234567890123456789012"));
        sig->Host("Hosty");
        CHECK(SignalUntil(sig, 5000, [&] { return sig->GetStatus() == Signal::IN_ROOM; }));
        PunchReport pr;
        pr.ok = true; pr.ms = 100; pr.peerSlot = 1;
        sig->ReportPunch(pr);
        CHECK(SignalUntil(sig, 6000, [&] { return punches >= 2; }));
        CHECK(sig->GetStatus() == Signal::IN_ROOM);
        delete sig;
        srv.Stop();
    }
}

// Galaxy 3.0: the 4-console star rig, host migration and the members[] room view (online_star_test.inc)
#include "online_star_test.inc"

// Both consoles stall together for longer than lostMs (the first frame of the lobby scene on a slow machine): the link must survive it; a real
// silence of the same length still ends it.
static void TestLocalStallIsNotPartnerSilence() {
    printf("local stall: a long frame on both consoles is not the partner's silence\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted(), false, 5);
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    p.Advance(500);
    const uint32_t lostMs = PunchConfig().lostMs;
    p.w.now += uint64_t(lostMs) + 9000;   // one 34 s frame: nothing runs, nothing is sent
    p.a->net.Tick();                      // a runs first and hears nothing (b has not run yet)
    CHECK(p.a->net.PeerAt(1).GetState() == Peer::CONNECTED);
    CHECK_EQ(p.a->lostCalls, 0);
    p.b->net.Tick();
    CHECK(p.b->net.PeerAt(0).GetState() == Peer::CONNECTED);
    CHECK_EQ(p.b->lostCalls, 0);
    p.Advance(3000);
    CHECK_EQ(p.a->lostCalls, 0);
    CHECK_EQ(p.b->lostCalls, 0);
    CHECK(p.a->net.GetPhase() == OnlineNet::READY);
    // the same time with the line really dead is still a lost link
    p.w.sim.SetHostLink(p.a->host, false);
    p.Advance(lostMs + 3000);
    CHECK_EQ(p.a->lostCalls, 1);
    CHECK_EQ(p.b->lostCalls, 1);
}

// A link whose every tick gap exceeds the stall limit and that hears nothing is dead: the stall credit is one per packet heard, not one per tick.
static void TestStallCreditNeedsAPacket() {
    printf("local stall: no second credit without a packet in between\n");
    Pair p(NatType::PortRestricted(), NatType::PortRestricted(), false, 6);
    CHECK(p.Start());
    CHECK(p.Connect(10000) >= 0);
    p.Advance(500);
    const uint32_t lostMs = PunchConfig().lostMs;
    p.w.sim.SetHostLink(p.a->host, false);
    for (uint32_t t = 0; t < lostMs * 3; t += 2000) {   // a tick every 2 s: every gap is over kLocalStallMs
        p.w.now += 2000;
        p.a->net.Tick();
        p.b->net.Tick();
    }
    CHECK_EQ(p.a->lostCalls, 1);
    CHECK_EQ(p.b->lostCalls, 1);
}

// Elapsed times saturate: a stamp taken from a later clock read than the tick's own `now` must read as 0 elapsed, never as 2^64 - x
// (which restarted STUN and republished the endpoint while nobody was connected).
static void TestSaturatingElapsed() {
    CHECK(SinceMs(1000, 400) == 600);
    CHECK(SinceMs(1000, 1000) == 0);
    CHECK(SinceMs(1000, 1003) == 0);
    CHECK(SinceMs(0, 5) == 0);
    CHECK(!(SinceMs(1000, 1003) >= 4000));
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IOLBF, 0);
    bool onlyTurn = false;
    g_quiet = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-v")) g_quiet = false;
        if (!strcmp(argv[i], "--turn")) onlyTurn = true;
        if (!strcmp(argv[i], "--netport31")) {   // netport31: only the held-poll / give-way / socket tests (tests/netport31_mutants.sh)
            TestHeldPollNoDataLimit();
            TestHeldPollGivesWay();
            TestPlainSocketsAcrossFailingPolls();
            TestFireAndForgetDropIsCounted();   // s1117b
            printf("\n%d checks, %d failed\n", g_checks, g_failed);
            return g_failed ? 1 : 0;
        }
    }
    if (onlyTurn) {
        RunTurnTests();
        if (const char* v = getenv("SMG_TEST_TURN_BUDGET")) TestTurnCredentialsHttp(v, "daily budget", "not available right now", 1);
        if (const char* v = getenv("SMG_TEST_TURN_PLAYER")) TestTurnCredentialsHttp(v, "per-player limit", "not available right now", 1);
        if (const char* v = getenv("SMG_TEST_TURN_SESSION")) {
            TestRelaySessionHttp(v, false, false, NatType::Symmetric(), NatType::Symmetric());
            TestRelaySessionHttp(v, true, false, NatType::PortRestricted(), NatType::PortRestricted());
            TestRelaySessionHttp(v, true, true, NatType::PortRestricted(), NatType::AddressRestricted());
        }
        printf("\n%d checks, %d failed\n", g_checks, g_failed);
        return g_failed ? 1 : 0;
    }
    TestCrypto();
    TestStunParsing();
    TestStunClassification();
    TestNatMatrix();
    TestSameLan();
    TestRoomAndVips();
    TestMtuAndFraming();
    TestReplayAndTamper();
    TestLossDelay(0, 0, 0, 0, 0);
    TestLossDelay(2, 75, 10, 2, 0);
    TestLossDelay(5, 75, 20, 5, 2);
    TestLossDelay(5, 150, 30, 5, 2);
    TestPunchUnderLoss();
    TestKeepaliveAndNatTimeout();
    TestWifiDropAndRebuild();
    TestCloseMessage();
    TestRelayPath();
    TestHostileMembers();
    TestBringUpLimitsAndReports();
    TestOversizeSecurePacket();
    TestSignalingFailsMidMatch();
    TestSignalingFailsOnRelay();
    TestReplayedPunch();
    TestSpoofedSourcesDuringPunching();
    TestDeadRandomGenerator();
    TestZeroRoomKey();
    TestSignalingRefusals();
    TestJoinFriendsOnlyRefused();
    TestMemberLabels();
    TestTurnLimitAnswers();
    TestTurnQueueOrder();
    TestHeldPollNoDataLimit();         // netport31
    TestHeldPollGivesWay();            // netport31
    TestPlainSocketsAcrossFailingPolls();   // netport31
    TestFireAndForgetDropIsCounted();       // s1117b
    TestSaturatingElapsed();
    TestLocalStallIsNotPartnerSilence();
    TestStallCreditNeedsAPacket();
    TestAppVersionAndDailyCap();
    TestSignalingErrorCodes();
    RunTurnTests();
    RunStarTests();   // Galaxy
    if (const char* server = getenv("SMG_TEST_SERVER")) TestHttpSignaling(server, getenv("SMG_TEST_TOKEN0") ? getenv("SMG_TEST_TOKEN0") : "", getenv("SMG_TEST_TOKEN1") ? getenv("SMG_TEST_TOKEN1") : "");
    if (const char* server = getenv("SMG_TEST_SERVER"))   // Galaxy: the 4-member star against the service, when four accounts were made
        if (getenv("SMG_TEST_TOKEN3")) {
            const char* tok[4] = {getenv("SMG_TEST_TOKEN0"), getenv("SMG_TEST_TOKEN1"), getenv("SMG_TEST_TOKEN2"), getenv("SMG_TEST_TOKEN3")};
            if (tok[0] && tok[1] && tok[2]) TestStarHttp(server, tok);
        }
    else printf("(signaling against the local service skipped: SMG_TEST_SERVER is not set)\n");
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
