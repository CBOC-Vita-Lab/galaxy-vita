// natsim_proxy: the NAT simulator (natsim.h) as a UDP service on loopback, for two or more real game processes.
//
// Each console sends every datagram to this process wrapped as  0xFE | client id | dst ip[4] | dst port[2] | payload
// (online/np_udp_posix.cpp, SMG_NET_SIM / SMG_NET_SIM_ID) and gets datagrams back as  src ip[4] | src port[2] | payload.
// The world: client k sits on its own LAN 192.168.<k+1>.0/24 as 192.168.<k+1>.10:41100 (NpNet::kDefaultPort, Galaxy) behind a NAT with public address
// 203.0.113.<10+k> (or all on one LAN with --same-lan), two STUN servers at 198.51.100.1:3478 and 198.51.100.2:3478.
//
//   natsim_proxy --port 47000 --nat0 port --nat1 sym [--same-lan] [--loss 2] [--delay 75] [--jitter 10] [--reorder 1] [--dup 0] [--idle 120000]
//                [--turn --turn-secret S --turn-lifetime 600 --turn-perm 300 --turn-bind 600 --turn-nonce 3600]
// --turn adds the test TURN server (turnsim.h) at 198.51.100.9:3478 and :53; credentials are the TURN REST form signed with --turn-secret.
// NAT names: full addr port sym symaddr open. Commands on stdin: "down K", "up K" (Wi-Fi of client K lost / back), "flush K" (the NAT
// of client K forgets its mappings), "impair loss delay jitter", "stats". Prints one "ready" line when it listens.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include <memory>

#include "natsim.h"
#include "turnsim.h"

using NatSim::NatType;
using NpNet::Addr;

static volatile int g_stop = 0;
static void OnSignal(int) { g_stop = 1; }
static uint64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec / 1000000);
}

int main(int argc, char** argv) {
    int port = 47000;
    bool sameLan = false;
    NatSim::Impair im;
    uint32_t idle = 120000;
    uint32_t seed = 1;
    std::vector<NatType> nats;
    bool turn = false;
    NatSim::TurnSim::Config tcfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") port = atoi(next());
        else if (a == "--same-lan") sameLan = true;
        else if (a == "--loss") im.lossPct = atof(next());
        else if (a == "--delay") im.delayMs = uint32_t(atoi(next()));
        else if (a == "--jitter") im.jitterMs = uint32_t(atoi(next()));
        else if (a == "--reorder") im.reorderPct = atof(next());
        else if (a == "--dup") im.dupPct = atof(next());
        else if (a == "--turn") turn = true;
        else if (a == "--turn-secret") tcfg.secret = next();
        else if (a == "--turn-lifetime") tcfg.maxLifetimeSec = uint32_t(atoi(next()));
        else if (a == "--turn-perm") tcfg.permLifeSec = uint32_t(atoi(next()));
        else if (a == "--turn-bind") tcfg.bindLifeSec = uint32_t(atoi(next()));
        else if (a == "--turn-nonce") tcfg.nonceLifeSec = uint32_t(atoi(next()));
        else if (a == "--turn-max") tcfg.maxAllocations = atoi(next());
        else if (a == "--idle") idle = uint32_t(atoi(next()));
        else if (a == "--seed") seed = uint32_t(atoi(next()));
        else if (a.compare(0, 5, "--nat") == 0) {
            const size_t k = size_t(atoi(a.c_str() + 5));
            NatType t;
            if (!NatType::Parse(next(), &t)) { fprintf(stderr, "bad NAT type for %s\n", a.c_str()); return 2; }
            if (nats.size() <= k) nats.resize(k + 1);
            nats[k] = t;
        } else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (nats.size() < 2) nats.resize(2, NatType::PortRestricted());

    NatSim::Sim sim(seed);
    sim.SetImpair(im);
    sim.AddStunServer(NpNet::MakeAddr(198, 51, 100, 1, 3478));
    sim.AddStunServer(NpNet::MakeAddr(198, 51, 100, 2, 3478));
    std::unique_ptr<NatSim::TurnSim> turnSim;
    if (turn) {
        tcfg.wallSec = [] { return uint64_t(time(0)); };
        NatSim::Sim* sp = &sim;
        turnSim.reset(new NatSim::TurnSim(tcfg, [sp](const Addr& from, const Addr& to, const std::vector<uint8_t>& d, uint64_t now) { sp->ServerSend(from, to, d, now); }));
        NatSim::TurnSim* ts = turnSim.get();
        for (int port : {3478, 53}) {
            const Addr a = NpNet::MakeAddr(198, 51, 100, 9, port);
            sim.AddServer(a, [ts, a](const Addr& src, const std::vector<uint8_t>& d, uint64_t now) { ts->OnDatagram(a, src, d, now); });
        }
    }
    struct Client { int host; sockaddr_in real; bool known; };
    std::vector<Client> clients;
    int sharedLan = -1;
    for (size_t k = 0; k < nats.size(); ++k) {
        int lan;
        Addr priv;
        if (sameLan) {
            if (sharedLan < 0) sharedLan = sim.AddLan((203u << 24) | (113u << 8) | 10u, nats[0], idle);
            lan = sharedLan;
            priv = NpNet::MakeAddr(192, 168, 1, 10 + int(k), NpNet::kDefaultPort);
        } else {
            lan = sim.AddLan((203u << 24) | (113u << 8) | uint32_t(10 + k), nats[k], idle);
            priv = nats[k].open ? NpNet::MakeAddr(203, 0, 113, int(50 + k), NpNet::kDefaultPort) : NpNet::MakeAddr(192, 168, int(k + 1), 10, NpNet::kDefaultPort);
        }
        Client c;
        c.host = sim.AddHost(lan, priv);
        memset(&c.real, 0, sizeof c.real);
        c.known = false;
        clients.push_back(c);
        printf("client %zu: private %s, NAT %s\n", k, NpNet::AddrStr(priv), nats[sameLan ? 0 : k].Name());
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in me;
    memset(&me, 0, sizeof me);
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    me.sin_port = htons(uint16_t(port));
    int big = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    if (bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) { perror("bind"); return 1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    signal(SIGTERM, OnSignal);
    signal(SIGINT, OnSignal);
    printf("ready on 127.0.0.1:%d, stun 198.51.100.1:3478 198.51.100.2:3478\n", port);
    fflush(stdout);

    std::string stdinBuf;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) | O_NONBLOCK);
    uint8_t buf[2048];
    while (!g_stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        FD_SET(0, &rf);
        timeval tv = {0, 1000};
        select(fd + 1, &rf, 0, 0, &tv);
        const uint64_t now = NowMs();
        for (int n = 0; n < 512; ++n) {
            sockaddr_in from;
            socklen_t fl = sizeof from;
            const ssize_t len = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (len < 0) break;
            if (len >= 8 && buf[0] == 0xFE && buf[1] < clients.size()) {
                Client& c = clients[buf[1]];
                c.real = from;
                c.known = true;
                Addr dst(uint32_t(buf[2]) << 24 | uint32_t(buf[3]) << 16 | uint32_t(buf[4]) << 8 | buf[5], uint16_t(buf[6] << 8 | buf[7]));
                sim.Send(c.host, dst, buf + 8, size_t(len) - 8, now);
            }
        }
        // commands
        char tmp[256];
        const ssize_t got = read(0, tmp, sizeof tmp);
        if (got > 0) stdinBuf.append(tmp, size_t(got));
        size_t nl;
        while ((nl = stdinBuf.find('\n')) != std::string::npos) {
            const std::string line = stdinBuf.substr(0, nl);
            stdinBuf.erase(0, nl + 1);
            int k = 0;
            double l = 0, d = 0, j = 0;
            if (sscanf(line.c_str(), "down %d", &k) == 1 && k >= 0 && k < int(clients.size())) { sim.SetHostLink(clients[k].host, false); printf("client %d down\n", k); }
            else if (sscanf(line.c_str(), "up %d", &k) == 1 && k >= 0 && k < int(clients.size())) { sim.SetHostLink(clients[k].host, true); printf("client %d up\n", k); }
            else if (sscanf(line.c_str(), "flush %d", &k) == 1 && k >= 0) { sim.FlushNat(k); printf("NAT %d flushed\n", k); }
            else if (sscanf(line.c_str(), "impair %lf %lf %lf", &l, &d, &j) == 3) { NatSim::Impair x = im; x.lossPct = l; x.delayMs = uint32_t(d); x.jitterMs = uint32_t(j); im = x; sim.SetImpair(im); printf("impair loss %.1f delay %.0f jitter %.0f\n", l, d, j); }
            else if (line == "stats") {
                const NatSim::SimStats& s = sim.Stats();
                printf("stats: sent %llu delivered %llu lost %llu filtered %llu noroute %llu blackholed %llu stun %llu dups %llu\n", (unsigned long long)s.sent,
                       (unsigned long long)s.delivered, (unsigned long long)s.lost, (unsigned long long)s.filtered, (unsigned long long)s.noRoute,
                       (unsigned long long)s.blackholed, (unsigned long long)s.stunAnswered, (unsigned long long)s.dups);
                if (turnSim) {
                    const NatSim::TurnSim::Stats& t = turnSim->GetStats();
                    printf("turn: allocations %llu refreshes %llu releases %llu permissions %llu bindings %llu channel-in %llu channel-out %llu data-ind %llu relayed %llu packets / %llu bytes, 401 %llu stale-nonce %llu expired-creds %llu mismatch %llu no-permission %llu live %d\n",
                           (unsigned long long)t.allocations, (unsigned long long)t.refreshes, (unsigned long long)t.releases, (unsigned long long)t.permissions, (unsigned long long)t.bindings,
                           (unsigned long long)t.channelIn, (unsigned long long)t.channelOut, (unsigned long long)t.dataIndOut, (unsigned long long)t.relayedPackets, (unsigned long long)t.relayedBytes,
                           (unsigned long long)t.unauthorized, (unsigned long long)t.staleNonce, (unsigned long long)t.expiredCreds, (unsigned long long)t.mismatch, (unsigned long long)t.noPermission,
                           turnSim->LiveAllocations());
                }
            }
            fflush(stdout);
        }
        // deliveries that are due
        for (size_t k = 0; k < clients.size(); ++k) {
            Addr src;
            std::vector<uint8_t> data;
            while (sim.Recv(clients[k].host, &src, &data, now)) {
                if (!clients[k].known || data.size() + 6 > sizeof buf) continue;
                buf[0] = uint8_t(src.ip >> 24); buf[1] = uint8_t(src.ip >> 16); buf[2] = uint8_t(src.ip >> 8); buf[3] = uint8_t(src.ip);
                buf[4] = uint8_t(src.port >> 8); buf[5] = uint8_t(src.port);
                memcpy(buf + 6, &data[0], data.size());
                sendto(fd, buf, data.size() + 6, 0, reinterpret_cast<sockaddr*>(&clients[k].real), sizeof clients[k].real);
            }
        }
    }
    const NatSim::SimStats& s = sim.Stats();
    printf("final stats: sent %llu delivered %llu lost %llu filtered %llu noroute %llu blackholed %llu stun %llu dups %llu\n", (unsigned long long)s.sent,
           (unsigned long long)s.delivered, (unsigned long long)s.lost, (unsigned long long)s.filtered, (unsigned long long)s.noRoute,
           (unsigned long long)s.blackholed, (unsigned long long)s.stunAnswered, (unsigned long long)s.dups);
    return 0;
}
