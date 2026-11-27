// Desktop UDP socket of the online transport (host32 / armhf test builds). The Vita's is np_udp_vita.cpp.
//
// Test hook: with SMG_NET_SIM=<ip:port> every datagram goes to the NAT simulator (tools/net/natsim) instead of the network, wrapped
// as   0xFE | client id | peer ip[4] | peer port[2] | payload      (client id = SMG_NET_SIM_ID, default 0)
// and the simulator hands datagrams back as   source ip[4] | source port[2] | payload.   The simulator plays the NATs and "the
// internet" (including the STUN servers), so everything above this class is the production code.
//   SMG_NET_SIM_PRIV=<ip>   the console's private address in the simulated LAN (the local candidate), port 41100 (Galaxy)
//   SMG_NET_LOCAL_IP=<ip>   the local candidate without the simulator
#include "np_net.h"

#if !defined(NP_TARGET_VITA)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NpNet {
namespace {

class PosixUdp : public Udp {
public:
    PosixUdp() : fd_(-1), boundPort_(0), simId_(0), sim_(false) {
        memset(&simTo_, 0, sizeof simTo_);
        if (const char* s = getenv("SMG_NET_SIM")) {
            Addr a;
            if (ParseAddr(s, 0, &a) && a.port) {
                sim_ = true;
                simTo_.sin_family = AF_INET;
                simTo_.sin_addr.s_addr = htonl(a.ip);
                simTo_.sin_port = htons(a.port);
                if (const char* id = getenv("SMG_NET_SIM_ID")) simId_ = uint8_t(atoi(id));
            }
        }
    }
    ~PosixUdp() override { Close(); }

    bool Open(uint16_t port) override {
        if (fd_ >= 0) return true;
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in local;
        memset(&local, 0, sizeof local);
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        // C3: when the port is taken, the next few are tried (the simulator keys on the client id, not the socket's port: it binds any)
        bool bound = false;
        for (int t = 0; t < (sim_ ? 1 : kPortTries) && !bound; ++t) {
            local.sin_port = htons(sim_ ? 0 : uint16_t(port + t));
            // SMG_NET_REFUSE_CHOSEN=1 (test only, mp74): the PS Vita's behaviour on the console log c48: every chosen port is refused (EACCES), port 0 works
            if (local.sin_port != 0 && getenv("SMG_NET_REFUSE_CHOSEN")) {
                errno = EACCES;
                NPN_LOG("cannot bind UDP port %u (errno %d)\n", unsigned(port + t), errno);
                continue;
            }
            if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) == 0) {
                bound = true;
                port = uint16_t(port + t);
            } else {
                NPN_LOG("cannot bind UDP port %u (errno %d)\n", unsigned(port + t), errno);
                if (errno != EADDRINUSE && errno != EACCES) break;
            }
        }
        if (!bound) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
        int big = 256 * 1024;
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &big, sizeof big) != 0) NPN_LOG("SO_RCVBUF refused (errno %d)\n", errno);   // Galaxy: logged
        if (!port && !sim_) {   // mp74: bound to port 0: the port the system chose (the room's address carries it)
            sockaddr_in got;
            socklen_t gl = sizeof got;
            memset(&got, 0, sizeof got);
            if (getsockname(fd_, reinterpret_cast<sockaddr*>(&got), &gl) == 0) port = ntohs(got.sin_port);
        }
        boundPort_ = sim_ ? kDefaultPort : port;   // Galaxy: the simulator's fixed private port is kDefaultPort (natsim_proxy.cpp)
        return true;
    }
    void Close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    bool IsOpen() const override { return fd_ >= 0; }

    int SendTo(const Addr& to, const void* data, size_t len) override {
        if (fd_ < 0) return -1;
        if (sim_) {
            uint8_t buf[1400];
            if (len + 8 > sizeof buf) return -1;
            buf[0] = 0xFE;
            buf[1] = simId_;
            buf[2] = uint8_t(to.ip >> 24);
            buf[3] = uint8_t(to.ip >> 16);
            buf[4] = uint8_t(to.ip >> 8);
            buf[5] = uint8_t(to.ip);
            buf[6] = uint8_t(to.port >> 8);
            buf[7] = uint8_t(to.port);
            memcpy(buf + 8, data, len);
            const ssize_t n = ::sendto(fd_, buf, len + 8, 0, reinterpret_cast<const sockaddr*>(&simTo_), sizeof simTo_);
            return n < 0 ? Err() : int(len);
        }
        sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(to.ip);
        addr.sin_port = htons(to.port);
        const ssize_t n = ::sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        return n < 0 ? Err() : int(n);
    }

    int RecvFrom(Addr* from, void* buffer, size_t capacity) override {
        if (fd_ < 0) return -1;
        sockaddr_in addr;
        socklen_t addrLen = sizeof addr;
        if (sim_) {
            uint8_t buf[1400];
            const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
            if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED) ? 0 : -1;
            if (n < 6) return 0;
            from->ip = uint32_t(buf[0]) << 24 | uint32_t(buf[1]) << 16 | uint32_t(buf[2]) << 8 | buf[3];
            from->port = uint16_t(buf[4] << 8 | buf[5]);
            const size_t m = size_t(n) - 6 < capacity ? size_t(n) - 6 : capacity;
            memcpy(buffer, buf + 6, m);
            return int(m);
        }
        const ssize_t n = ::recvfrom(fd_, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED) ? 0 : -1;
        from->ip = ntohl(addr.sin_addr.s_addr);
        from->port = ntohs(addr.sin_port);
        return int(n);
    }

    uint32_t LocalIp() override {
        Addr a;
        if (const char* e = sim_ ? getenv("SMG_NET_SIM_PRIV") : getenv("SMG_NET_LOCAL_IP"))
            if (ParseAddr(e, 0, &a)) return a.ip;
        if (sim_) return 0;
        // the interface the default route would use (a connected UDP socket sends nothing)
        const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) return 0;
        sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        to.sin_addr.s_addr = htonl(0x08080808u);
        uint32_t ip = 0;
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
            sockaddr_in me;
            socklen_t l = sizeof me;
            if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &l) == 0) ip = ntohl(me.sin_addr.s_addr);
        }
        ::close(s);
        return ip;
    }
    uint32_t GatewayIp() override {
        if (sim_) return 0;
#if defined(__linux__)
        // the default route: /proc/net/route, destination 00000000 with the gateway flag; the gateway is stored in network byte order
        FILE* f = fopen("/proc/net/route", "r");
        if (!f) return 0;
        char line[256];
        uint32_t best = 0;
        unsigned bestMetric = 0xFFFFFFFFu;
        if (fgets(line, sizeof line, f)) {   // (header)
            while (fgets(line, sizeof line, f)) {
                char ifname[32];
                unsigned dest = 0, gw = 0, flags = 0, ref = 0, use = 0, metric = 0;
                if (sscanf(line, "%31s %x %x %x %u %u %u", ifname, &dest, &gw, &flags, &ref, &use, &metric) != 7) continue;
                if (dest != 0 || !(flags & 2) || gw == 0 || metric >= bestMetric) continue;
                best = ntohl(gw);
                bestMetric = metric;
            }
        }
        fclose(f);
        return best;
#else
        return 0;
#endif
    }
    uint16_t LocalPort() override { return boundPort_; }

private:
    int Err() const { return (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS || errno == ECONNREFUSED || errno == EINTR) ? 0 : -1; }
    int fd_;
    uint16_t boundPort_;
    uint8_t simId_;
    bool sim_;
    sockaddr_in simTo_;
};

}  // namespace

Udp* CreatePlatformUdp() { return new PosixUdp; }

int ResolveIpv4(const char* host, uint32_t* out, int max) {
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = 0;
    if (getaddrinfo(host, 0, &hints, &res) != 0 || !res) return 0;
    int n = 0;
    for (addrinfo* p = res; p && n < max; p = p->ai_next) {
        const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr.s_addr);
        bool dup = false;
        for (int i = 0; i < n; ++i)
            if (out[i] == ip) dup = true;
        if (!dup) out[n++] = ip;
    }
    freeaddrinfo(res);
    return n;
}

}  // namespace NpNet

#endif
