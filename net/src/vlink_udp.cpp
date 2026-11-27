// VLink POSIX UDP backend: host32 / armhf test builds (two game processes on 127.0.0.1, or real machines on a LAN).
// Not built for the Vita (vlink_vita.cpp has the Vita's backends).
#include "vlink.h"

#if !defined(NP_TARGET_VITA)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#if defined(__APPLE__) || defined(MP_FAKE_APPLE)
#include <ifaddrs.h>   // macbot: the default broadcast targets of a Mac (see AppleDefaultTargets)
#include <net/if.h>
#include <sys/types.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif

#define VLINK_LOG(...) fprintf(stderr, "[vlink] " __VA_ARGS__)

namespace VLink {
namespace {

// Test fault injection (Galaxy, desktop / armhf test builds only: this file is not built for the Vita): the same switches as the
// online transport's np_fault (SMG_NET_DROP=<percent>, SMG_NET_DUP=<percent>, SMG_NET_DELAY_MS=<ms>, SMG_NET_JITTER_MS=<ms>), applied to
// every datagram this process sends on the Same Wi-Fi UDP link (each broadcast target on its own). Off unless one is set. Delayed
// datagrams leave at the next send or receive call (the link pumps every frame and from its helper thread).
struct UdpFault {
    double dropPct, dupPct;
    uint32_t delayMs, jitterMs, rng;
    uint64_t sent, dropped, duped;
    struct Held { uint64_t due; sockaddr_in to; std::vector<uint8_t> data; };
    std::vector<Held> held;   // ordered by due time
    UdpFault() : dropPct(0), dupPct(0), delayMs(0), jitterMs(0), rng(0x9E3779B9u), sent(0), dropped(0), duped(0) {}
    bool on() const { return dropPct > 0 || dupPct > 0 || delayMs || jitterMs; }
    double uniform() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (rng >> 8) / double(1u << 24);
    }
};

uint64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u;
}

struct UdpState {
    Backend backend;
    int fd;
    int lastSendErr;   // macbot: the last send errno that was logged (0 none): one line per distinct failure, not one per datagram
    uint16_t port;
    uint32_t vip;
    std::vector<sockaddr_in> targets;
    UdpFault fault;
    UdpState() : fd(-1), lastSendErr(0), port(0), vip(0) { memset(&backend, 0, sizeof backend); }
};

void FaultRelease(UdpState& b) {
    if (b.fault.held.empty()) return;
    const uint64_t now = NowMs();
    size_t n = 0;
    while (n < b.fault.held.size() && b.fault.held[n].due <= now) {
        const UdpFault::Held& h = b.fault.held[n];
        ::sendto(b.fd, h.data.data(), h.data.size(), 0, reinterpret_cast<const sockaddr*>(&h.to), sizeof h.to);
        ++n;
    }
    if (n) b.fault.held.erase(b.fault.held.begin(), b.fault.held.begin() + long(n));
}

// sendto through the fault injector (a plain sendto when it is off)
ssize_t FaultSendTo(UdpState& b, const void* data, size_t len, const sockaddr_in& to) {
    UdpFault& f = b.fault;
    if (!f.on()) return ::sendto(b.fd, data, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    ++f.sent;
    if (f.dropPct > 0 && f.uniform() * 100.0 < f.dropPct) { ++f.dropped; return ssize_t(len); }
    const int copies = (f.dupPct > 0 && f.uniform() * 100.0 < f.dupPct) ? 2 : 1;
    if (copies == 2) ++f.duped;
    for (int c = 0; c < copies; ++c) {
        const uint32_t delay = f.delayMs + (f.jitterMs ? uint32_t(f.uniform() * f.jitterMs) : 0);
        if (delay == 0) {
            ::sendto(b.fd, data, len, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
            continue;
        }
        if (f.held.size() >= 4096) { ++f.dropped; continue; }   // (bounded: a queue that big is loss anyway)
        UdpFault::Held h;
        h.due = NowMs() + delay;
        h.to = to;
        h.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        size_t pos = f.held.size();
        while (pos > 0 && f.held[pos - 1].due > h.due) --pos;   // jitter reorders
        f.held.insert(f.held.begin() + long(pos), h);
    }
    return ssize_t(len);
}

#if defined(__APPLE__) || defined(MP_FAKE_APPLE)
// macbot: macOS sends a datagram to 255.255.255.255 out of the default-route interface only, and with a VPN or a second network up that
// is often not the Wi-Fi the Vita is on. Like the Vita's own Wi-Fi backend, send to the subnet broadcast address of every interface that is
// up and can broadcast (ip | ~netmask), then 255.255.255.255 as well. Returns the "a.b.c.d:port,..." list.
std::string AppleDefaultTargets(uint16_t port) {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (const ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask) continue;
            const unsigned f = a->ifa_flags;
            if (!(f & IFF_UP) || !(f & IFF_BROADCAST) || (f & IFF_LOOPBACK)) continue;
            const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr);
            const uint32_t mask = ntohl(reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr);
            if (ip == 0 || mask == 0xFFFFFFFFu) continue;   // (a /32 has no broadcast address)
            const uint32_t bc = ip | ~mask;
            char item[40];
            snprintf(item, sizeof item, "%u.%u.%u.%u:%u,", bc >> 24, (bc >> 16) & 255, (bc >> 8) & 255, bc & 255, (unsigned)port);
            if (out.find(item) == std::string::npos) out += item;
        }
        freeifaddrs(list);
    }
    out += "255.255.255.255:" + std::to_string(port);
    return out;
}
#endif

bool ParseTargets(UdpState& b, const char* specText) {
    b.targets.clear();
    std::string spec = specText ? specText : "";
#if defined(__APPLE__) || defined(MP_FAKE_APPLE)
    if (spec.empty()) spec = AppleDefaultTargets(b.port);
#endif
    if (spec.empty()) spec = "255.255.255.255:" + std::to_string(b.port);
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        const std::string item = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? spec.size() + 1 : comma + 1;
        if (item.empty()) continue;
        const size_t colon = item.rfind(':');
        sockaddr_in t;
        memset(&t, 0, sizeof t);
        t.sin_family = AF_INET;
        const std::string host = colon == std::string::npos ? item : item.substr(0, colon);
        const int port = colon == std::string::npos ? b.port : atoi(item.c_str() + colon + 1);
        if (inet_pton(AF_INET, host.c_str(), &t.sin_addr) != 1 || port <= 0 || port > 65535) {
            VLINK_LOG("bad broadcast target '%s'\n", item.c_str());
            return false;
        }
        t.sin_port = htons(uint16_t(port));
#if defined(__APPLE__)
        t.sin_len = sizeof t;   // (BSD sockaddr length byte; the kernel sets it from the call's length anyway)
#endif
        b.targets.push_back(t);
    }
    {   // macbot: which networks the discovery beacons go to (the partner's log shows it; "no beacons seen" is often a wrong subnet)
        std::string list;
        for (const sockaddr_in& t : b.targets) {
            char item[32];
            snprintf(item, sizeof item, "%s%s:%u", list.empty() ? "" : ", ", inet_ntoa(t.sin_addr), (unsigned)ntohs(t.sin_port));
            list += item;
        }
        VLINK_LOG("broadcast targets: %s\n", list.c_str());
    }
    return !b.targets.empty();
}

bool UdpOpen(UdpState& b) {
    if (b.fd >= 0) return true;
    b.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (b.fd < 0) return false;
    int one = 1;
    if (setsockopt(b.fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) != 0) VLINK_LOG("SO_BROADCAST refused (errno %d)\n", errno);   // Galaxy: logged
    sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(b.port);
#if defined(__APPLE__)
    local.sin_len = sizeof local;
#endif
    if (::bind(b.fd, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
        VLINK_LOG("cannot bind UDP port %u (errno %d)\n", b.port, errno);
        ::close(b.fd);
        b.fd = -1;
        return false;
    }
    fcntl(b.fd, F_SETFL, fcntl(b.fd, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

uint32_t UdpLocalVip(void* ctx) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (!UdpOpen(b)) return 0;
    return b.vip;
}

// macbot: a send that fails is logged once per distinct errno (macOS answers "No route to host" for every datagram when Terminal has
// not been allowed to use the local network: Privacy & Security > Local Network). Reads errno right after the failed call.
void NoteSendError(UdpState& b, int result) {
    if (result >= 0) return;
    const int e = errno;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == b.lastSendErr) return;
    b.lastSendErr = e;
    VLINK_LOG("send failed (errno %d: %s)\n", e, strerror(e));
}

int UdpSend(void* ctx, const PeerAddr* to, bool broadcast, const void* data, size_t len) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (!UdpOpen(b)) return -1;
    FaultRelease(b);
    if (!broadcast && to) {
        sockaddr_in addr;
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, to->bytes, 4);
        memcpy(&addr.sin_port, to->bytes + 4, 2);
#if defined(__APPLE__)
        addr.sin_len = sizeof addr;
#endif
        const int n = int(FaultSendTo(b, data, len, addr));
        NoteSendError(b, n);
        return n;
    }
    int best = -1;
    for (size_t i = 0; i < b.targets.size(); ++i) {
        const sockaddr_in& t = b.targets[i];
        // never to ourselves: a loopback target on our own port is this process
        if (t.sin_port == htons(b.port) && t.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
        const ssize_t n = FaultSendTo(b, data, len, t);
        NoteSendError(b, int(n));
        if (n >= 0) best = int(n);
    }
    return best;
}

int UdpRecv(void* ctx, PeerAddr* from, void* buffer, size_t capacity) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (b.fd < 0) return 0;
    FaultRelease(b);
    sockaddr_in addr;
    socklen_t addrLen = sizeof addr;
    const ssize_t n = ::recvfrom(b.fd, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    memset(from, 0, sizeof *from);
    memcpy(from->bytes, &addr.sin_addr.s_addr, 4);
    memcpy(from->bytes + 4, &addr.sin_port, 2);
    return int(n);
}

void UdpClose(void* ctx) {
    UdpState& b = *static_cast<UdpState*>(ctx);
    if (b.fault.on())
        VLINK_LOG("udp fault injection: %u sent, %u dropped, %u duplicated, %u still held\n", NETFIX_U32C(b.fault.sent),
                  NETFIX_U32C(b.fault.dropped), NETFIX_U32C(b.fault.duped), (unsigned)b.fault.held.size());
    b.fault.held.clear();
    if (b.fd >= 0) ::close(b.fd);
    b.fd = -1;
}

}  // namespace

Backend* CreateUdpBackend(uint16_t bindPort, const char* broadcastTargets, uint32_t vip) {
    UdpState* b = new UdpState;
    b->port = bindPort ? bindPort : 41100;   // Galaxy: 41100 (Strikers 41000)
    if (vip == 0) {
        static uint32_t counter = 0;  // process id, time and a counter: unique enough between two consoles
        uint32_t x = uint32_t(getpid()) * 2654435761u ^ uint32_t(time(0)) * 40503u ^ (++counter * 2246822519u);
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        vip = 0x0A000000u | (x & 0x00FFFFFEu) | 1u;  // 10.x.y.z, never .0
    }
    b->vip = vip;
    {
        UdpFault& f = b->fault;
        if (const char* v = getenv("SMG_NET_DROP")) f.dropPct = atof(v);
        if (const char* v = getenv("SMG_NET_DUP")) f.dupPct = atof(v);
        if (const char* v = getenv("SMG_NET_DELAY_MS")) f.delayMs = uint32_t(atoi(v));
        if (const char* v = getenv("SMG_NET_JITTER_MS")) f.jitterMs = uint32_t(atoi(v));
        if (f.dropPct < 0) f.dropPct = 0;
        if (f.dropPct > 90) f.dropPct = 90;
        if (f.dupPct < 0) f.dupPct = 0;
        if (f.dupPct > 100) f.dupPct = 100;
        if (f.delayMs > 5000) f.delayMs = 5000;
        if (f.jitterMs > 5000) f.jitterMs = 5000;
        f.rng ^= uint32_t(b->port) * 2654435761u;
        if (f.on())
            VLINK_LOG("udp fault injection on: drop %.1f%% dup %.1f%% delay %u ms jitter %u ms\n", f.dropPct, f.dupPct, (unsigned)f.delayMs,
                      (unsigned)f.jitterMs);
    }
    if (!ParseTargets(*b, broadcastTargets)) {
        delete b;
        return 0;
    }
    b->backend.name = "udp";
    b->backend.ctx = b;
    b->backend.local_vip = UdpLocalVip;
    b->backend.send = UdpSend;
    b->backend.recv = UdpRecv;
    b->backend.close = UdpClose;
    return &b->backend;
}

Backend* CreateUdpBackendFromEnvironment() {
    const char* port = getenv("SMG_LINK_PORT");
    const char* targets = getenv("SMG_LINK_BCAST");
    const char* vipText = getenv("SMG_LINK_VIP");
    uint32_t vip = 0;
    if (vipText && *vipText) {
        in_addr a;
        if (inet_pton(AF_INET, vipText, &a) == 1) vip = ntohl(a.s_addr);
    }
    Backend* backend = CreateUdpBackend(port ? uint16_t(atoi(port)) : 41100, targets, vip);   // Galaxy: 41100
    // SMG_LINK_MFS=<bytes>: pretend the medium carries at most this many bytes per datagram (the Vita's ad hoc PDP: 1444), so that a desktop run
    // exercises the link's fragmentation with the real game (tools/mp/turn.sh --env SMG_LINK_MFS=700). Test only.
    if (backend)
        if (const char* mfs = getenv("SMG_LINK_MFS"))
            if (atoi(mfs) > int(kFragHeader) + 16) backend->maxFrame = size_t(atoi(mfs));
    return backend;
}

void DestroyUdpBackend(Backend* backend) {
    if (!backend) return;
    UdpState* b = static_cast<UdpState*>(backend->ctx);
    if (b->fd >= 0) ::close(b->fd);
    delete b;
}

}  // namespace VLink

#else  // NP_TARGET_VITA

namespace VLink {
Backend* CreateUdpBackend(uint16_t, const char*, uint32_t) { return 0; }
Backend* CreateUdpBackendFromEnvironment() { return 0; }
void DestroyUdpBackend(Backend*) {}
}  // namespace VLink

#endif
