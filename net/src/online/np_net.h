// Internet transport for online play: common types of the NpNet modules.
//
// Layout of the online stack (all of it sits BELOW VLink: the game, its lobby and its lockstep are untouched):
//
//   game <-> SO API (so_net.cpp) <-> VLink::Link (frames, vips, fragmentation) <-> OnlineBackend (vlink_online.cpp)
//        OnlineNet (np_online.cpp): STUN, signaling, hole punching, per-peer sessions     <- this directory
//        Udp (np_udp_*.cpp): the one UDP socket on port 41100 (Galaxy); every datagram below goes through it
//
// One UDP socket carries four kinds of datagram; the first byte tells them apart (RFC 7983 demultiplexing):
//   00xxxxxx  STUN (RFC 5389): binding requests from this console, their responses
//   01xxxxxx  TURN ChannelData (RFC 8656): the relay path (the TURN client comes later, the framing is here: np_peer.cpp)
//   11000001  PUNCH: authenticated hole punching probe (np_peer.cpp)
//   11000010  DATA:  encrypted VLink frame
//   11000011  CTRL:  encrypted keep-alive / close
//
// Nothing in here knows the game. Time and randomness come in through Env so that the desktop tests run the whole stack in virtual time.
#ifndef NP_NET_ONLINE_H
#define NP_NET_ONLINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../np_log.h"
#define NPN_LOG(...) NpLogf("np-net", __VA_ARGS__)   // Galaxy: also the Vita log.txt (np_log.h)

namespace NpNet {

// IPv4 + UDP port, host byte order.
struct Addr {
    uint32_t ip;
    uint16_t port;
    Addr() : ip(0), port(0) {}
    Addr(uint32_t i, uint16_t p) : ip(i), port(p) {}
    bool IsZero() const { return ip == 0 && port == 0; }
    bool operator==(const Addr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Addr& o) const { return !(*this == o); }
};
inline Addr MakeAddr(int a, int b, int c, int d, int port) {
    return Addr((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d), uint16_t(port));
}
// "a.b.c.d:port" into a small static ring of buffers (log lines only).
const char* AddrStr(const Addr& a);
// The same for log lines: on the Vita the last two parts of a public IPv4 address are hidden ("203.0.x.x:41100") unless SMG_NET_LOG_ADDRS=1,
// because players post their logs; the desktop test builds show everything. Never used for what is sent.
const char* AddrLog(const Addr& a);
// "a.b.c.d:port" or "a.b.c.d" (port defaults to `defaultPort`). Dotted quads only (no names: see Resolve in np_online.cpp).
bool ParseAddr(const char* text, uint16_t defaultPort, Addr* out);
// RFC 1918, link-local and CGNAT (100.64/10) addresses.
bool IsPrivateIp(uint32_t ip);

// The one UDP socket. Implementations: np_udp_posix.cpp (desktop, with the test simulator hook), np_udp_vita.cpp (sceNet).
class Udp {
public:
    virtual ~Udp() {}
    virtual bool Open(uint16_t port) = 0;   // bind 0.0.0.0:port, non-blocking
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;
    // Bytes sent, or <0 on a hard error (a full buffer counts as sent: UDP).
    virtual int SendTo(const Addr& to, const void* data, size_t len) = 0;
    // One datagram: its length, 0 when nothing is waiting, <0 when the socket is broken.
    virtual int RecvFrom(Addr* from, void* buffer, size_t capacity) = 0;
    // This console's own address on the network it uses (the "local candidate"), 0 when unknown.
    virtual uint32_t LocalIp() = 0;
    // The default-route gateway's address (host order) where the platform can tell, else 0. UPnP prefers the SSDP answer that comes from it.
    virtual uint32_t GatewayIp() { return 0; }
    virtual uint16_t LocalPort() = 0;       // the port other consoles on the same LAN reach us on (the port actually bound: Open falls back to the next few)
    virtual const char* Error() const { return ""; }   // a sentence for the player when Open() failed
};
const int kPortTries = 8;                  // Open(port) tries port .. port + 7 (C3: the Vita sometimes refuses the first bind)

// Time and randomness (virtual in the unit tests).
struct Env {
    Udp* udp;
    uint64_t (*now)(void* ctx);                   // milliseconds, monotonic
    void* nowCtx;
    bool (*random)(void* ctx, void* out, size_t n);   // false: the generator failed (the bytes are then NOT random: never use them)
    void* randomCtx;
    Env() : udp(0), now(0), nowCtx(0), random(0), randomCtx(0) {}
    uint64_t Now() const { return now(nowCtx); }
    bool Random(void* out, size_t n) const { return random(randomCtx, out, n); }
};
// Platform UDP socket: POSIX on the desktop (SMG_NET_SIM=<ip:port> routes it through the NAT simulator of tools/net), sceNet on the Vita.
Udp* CreatePlatformUdp();
// Blocking DNS A-record lookup (call it from a worker thread). Up to `max` addresses, the count, 0 on failure.
int ResolveIpv4(const char* host, uint32_t* out, int max);

// Elapsed milliseconds that never wraps: a timestamp taken from a later clock read than `now` (a timer started inside the same tick) gives 0, not 2^64-x.
inline uint64_t SinceMs(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }

uint64_t SteadyNowMs(void*);                       // CLOCK_MONOTONIC / sceKernelGetProcessTimeWide
// getrandom / sceKernelGetRandomNumber (np_crypto.cpp). False when the generator failed: `out` is then zeroed and must not be used as a key,
// nonce or transaction id. RandomFailureSentence() is what the player reads.
bool SystemRandom(void*, void* out, size_t n);
const char* RandomFailureSentence();

// ---- limits ------------------------------------------------------------------------------------------------------------
const int kMaxPeers = 3;                // other consoles in a room (the game has 4 machines at most)
const int kMaxSlots = kMaxPeers + 1;
const size_t kMaxDatagram = 1200;       // largest UDP payload this transport sends (PDP fragmentation limits, IPv4 path MTU of 1280 with headroom)
const size_t kDataHeader = 6;           // DATA/CTRL: type, sender slot, 32-bit sequence number
const size_t kTagSize = 16;             // Poly1305
const size_t kDataOverhead = kDataHeader + kTagSize;
const size_t kMaxFrame = kMaxDatagram - kDataOverhead;   // the VLink frame (header + payload) one datagram carries; VLink::Backend::maxFrame
const size_t kRelayOverhead = 8;        // TURN ChannelData header (4 bytes) with headroom: the receive buffer is kMaxDatagram + this, so a longer datagram is cut and refused
const uint16_t kDefaultPort = 41100;   // Galaxy: 41100 (Strikers 41000), DESIGN.md section 3

// First-byte classes.
enum PacketClass { PKT_STUN, PKT_CHANNEL, PKT_PUNCH, PKT_DATA, PKT_CTRL, PKT_UNKNOWN };
const uint8_t kTypePunch = 0xC1, kTypeData = 0xC2, kTypeCtrl = 0xC3;
PacketClass Classify(const uint8_t* data, size_t len);

// NAT behaviour of this console's router as the STUN servers see it.
enum NatClass {
    NAT_UNKNOWN = 0,      // not measured (one STUN server answered, or none)
    NAT_OPEN = 1,         // public address == local address (no NAT)
    NAT_CONE = 2,         // endpoint-independent mapping: the same public port for every destination
    NAT_SYMMETRIC = 3     // the public port depends on the destination: hole punching only works against a cone NAT with address-only filtering
};
const char* NatClassName(int nat);
// What hole punching can do between two NAT behaviours (STUN cannot see the inbound filter, so this is the optimistic answer):
// false only when it cannot work: two symmetric NATs (neither side can name the other's port). true = worth trying; a symmetric NAT
// against a port-restricted cone NAT fails in practice and is found out by the punch timeout.
bool PunchPossible(int natA, int natB);

}  // namespace NpNet

#endif  // NP_NET_ONLINE_H
