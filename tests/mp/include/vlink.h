// VLink: a tiny virtual IPv4/UDP network for the native port's LAN multiplayer.
//
// Mario Strikers Charged ships a complete LAN mode (NetworkSession::InitializeLAN, LANLobby broadcast
// discovery, the Create/Join lobby, draft, the deterministic input router, results, series). It sits on nine
// TransportSocket wrappers around Nintendo's SO sockets: a UDP "direct" socket on port 1000 and a UDP
// broadcast socket on port 1001. VLink carries exactly those datagrams between consoles that have no router:
// every console gets a virtual IPv4 address (vip); a guest datagram is wrapped in a 24-byte frame (magic,
// fingerprint, src/dst vip, src/dst port) and handed to a Backend that knows how to reach the other consoles:
//   - Vita ad hoc PDP between MAC addresses (default on the Vita, no access point needed),
//   - Vita Wi-Fi UDP on the access point the console is already on (also what Vita3K offers),
//   - plain POSIX UDP (host32 / armhf test builds, two processes on 127.0.0.1).
// The game keeps its own protocol (lobby, reliability, lockstep); this layer only moves datagrams.
//
// Frame layout and semantics are those of the recompiled port's MscLink (port/runtime/src/hle/net/msc_link.*),
// so the two can share tests; the native port uses its own FINGERPRINT (see so_net.cpp), because a recompiled
// and a native console do not simulate bit-identically and must not pair.
//
// This module knows nothing about the game or the SDK. It is instance based (tests run two consoles in one
// process); so_net.cpp owns the process-wide default instance that the SO API uses.
#ifndef NP_VLINK_H
#define NP_VLINK_H

#include <stddef.h>
#include <stdint.h>

namespace VLink {

const uint32_t kBroadcastVip = 0xFFFFFFFFu;
const size_t kMaxPayload = 1472;  // largest guest datagram carried (the game's packet buffers are 1465)
const size_t kPeerAddrSize = 8;   // opaque backend address: IPv4 + port, or a 6-byte MAC
const int kMaxSockets = 8;
const size_t kFragHeader = 12;       // fragment of a longer frame: magic "MSLF", id, index, count, offset, total (see vlink.cpp)
const unsigned kReassemblyMs = 1000; // an incomplete frame is dropped after this long

struct PeerAddr {
    uint8_t bytes[kPeerAddrSize];
};

// A transport that moves whole frames between consoles. Calls happen under the link's lock, never re-entrantly.
struct Backend {
    const char* name;
    void* ctx;
    // Virtual IP for this console (host byte order, never 0 or broadcast).
    uint32_t (*local_vip)(void* ctx);
    // Unicast to a learned peer (broadcast=false) or to every console (broadcast=true). Bytes sent, or <0.
    int (*send)(void* ctx, const PeerAddr* to, bool broadcast, const void* data, size_t len);
    // Non-blocking. Frame length, 0 when nothing is waiting, <0 on a hard error.
    int (*recv)(void* ctx, PeerAddr* from, void* buffer, size_t capacity);
    // Releases backend resources. The backend object stays valid for a later Start().
    void (*close)(void* ctx);
    // The largest frame (header + payload) one send() can carry; 0 = no limit below header + kMaxPayload. The Link splits a longer frame into
    // fragments (kFragHeader bytes of header each) and puts it together on the receiving console, so the game's datagram sizes never meet the
    // medium's: the Vita's ad hoc PDP carries at most SCE_NET_ADHOC_PDP_MFS = 1444 bytes per datagram, the game sends up to 1,415 + our
    // 24-byte header, and the whole frame can be 1,496 bytes (kMaxPayload). Aggregate initialisers may leave it out.
    size_t maxFrame;
};

struct Stats {
    uint64_t framesSent, framesReceived, bytesSent, bytesReceived;
    uint64_t droppedFingerprint, droppedMalformed, droppedNoSocket, droppedQueueFull, sendErrors;
    // Fragmentation (Backend::maxFrame): frames split on send / put together on receipt, fragments that never completed (lost, or timed out
    // after kReassemblyMs) or were malformed; the largest guest payload the game has sent and the largest frame handed to the backend.
    uint64_t framesSplit, fragmentsSent, framesReassembled, droppedFragments;
    uint32_t maxPayloadSent, maxBackendFrame;
    // Instrumentation (wave 5 NET): every call of the backend's send (fragments and broadcast copies included) is timed: calls, total and
    // largest time in microseconds (the PdpSend / sendto cost on the game thread); the most datagrams one socket's queue held (the game
    // drains it once a frame); sends refused because the link was declared lost (SetLost).
    uint64_t backendSends, sendUsTotal;
    uint32_t sendUsMax, queueHighWater;
    uint64_t sendsRefusedLost;
    // Wave 12 (STR-146): frames addressed to another console (wrong destination vip), and stale frames thrown away when the link started.
    uint64_t droppedAddress, droppedStale;
};

// The last console refused for a different fingerprint (build, protocol or rule set). `rules` is its rule byte (header byte 5, Link::SetRules).
struct Incompatible {
    uint64_t count;       // frames refused since Start()
    uint32_t fingerprint; // the refused console's
    uint32_t srcVip;
    uint64_t lastMs;      // Link clock
    uint8_t rules;
};

// Maxima since the previous TakeWindow() call (the "[mp-lan] net" log line of np_lan.cpp takes one per line).
struct WindowStats {
    uint32_t sendUsMax, queueHighWater, payloadSentMax, payloadReceivedMax;
};

// Test impairment applied to the frames THIS console sends (never compiled out: it is cheap, but only
// SetImpairment / the environment switch of the default instance turn it on).
struct Impairment {
    double drop;        // probability 0..1 a frame is lost
    double dup;         // probability 0..1 a frame is sent twice
    uint32_t delayMs;   // fixed delay
    uint32_t jitterMs;  // extra random delay 0..jitterMs (reorders)
};

struct LinkImpl;

class Link {
public:
    Link();
    ~Link();

    // Backend and compatibility fingerprint (both consoles must agree: build and gameplay-relevant settings).
    void SetBackend(const Backend* backend);
    const Backend* GetBackend() const;
    void SetFingerprint(uint32_t fingerprint);
    uint32_t Fingerprint() const;
    // The rule byte (np_lan_rules: the settings that change what a match computes) goes into byte 5 of every frame header, so that a console
    // that refuses a frame for its fingerprint can say which rules the other one runs. It is information only; the fingerprint decides.
    void SetRules(uint8_t rules);
    // True when a frame was refused for another fingerprint since Start(); `out` receives the last one.
    bool GetIncompatible(Incompatible* out) const;
    // Millisecond clock for delayed (impaired) frames; default is the steady clock.
    void SetClock(uint64_t (*nowMs)(void*), void* ctx);
    void SetImpairment(const Impairment& impairment, uint32_t seed);
    // MSC_LINK_TRACE=N: log the first N frames sent and received to stderr.
    void SetTrace(int frames);

    // Lifecycle (the game's SocketNetworkStartup/Shutdown). Start needs a backend with a usable address.
    bool Start();
    void Stop();
    bool IsUp() const;
    uint32_t LocalVip() const;

    // UDP-like sockets. Handles are small positive integers (1..kMaxSockets).
    int OpenSocket();
    bool Bind(int handle, uint16_t port);  // port 0 picks an ephemeral port; false when the port is taken
    void Close(int handle);
    bool IsOpen(int handle) const;
    uint16_t LocalPort(int handle) const;
    // Bytes sent, or -1. A unicast to an unknown vip goes out as a broadcast frame (only the addressee keeps it).
    int SendTo(int handle, uint32_t dstVip, uint16_t dstPort, const void* data, size_t len);
    // Bytes copied (UDP truncation to `capacity`), or -1 when nothing is queued for this socket.
    int RecvFrom(int handle, void* buffer, size_t capacity, uint32_t* srcVip, uint16_t* srcPort);

    // Drains the backend into the socket queues and releases due delayed frames. RecvFrom calls it.
    void Pump();
    // Releases the delayed (impaired) frames that are due, nothing else: the desktop's impairment clock thread calls it every
    // millisecond (so_net.cpp, MSC_LINK_DELAY_THREAD), so a test delay is wall-clock time and not "until this console next sends".
    void ReleaseDue();
    Stats GetStats() const;
    WindowStats TakeWindow();

    // The link is gone (the Wi-Fi watch, a resume from sleep, a test): every SendTo fails from now on, so the game's transport sees a
    // socket error at its next send and ends the session with its own "connection lost". Cleared by Start() and SetLost(false).
    void SetLost(bool lost, const char* why);
    bool IsLost() const;

private:
    Link(const Link&);
    Link& operator=(const Link&);
    LinkImpl* m;
};

// ---- built-in backends -------------------------------------------------------------------------------------
// POSIX UDP (host32 / armhf). bindPort: local UDP port (0 = 41000). broadcastTargets: "host:port,host:port"
// (null or empty = 255.255.255.255:bindPort). vip: 0 picks a random 10.x.y.z. Every call makes a new backend
// (tests run several consoles in one process); the UDP socket is opened lazily by Link::Start. Returns nullptr
// on bad arguments, and always on the Vita (vlink_vita.cpp has the Vita's own).
Backend* CreateUdpBackend(uint16_t bindPort, const char* broadcastTargets, uint32_t vip);
// The same from the environment: MSC_LINK_PORT, MSC_LINK_BCAST, MSC_LINK_VIP (desktop tests).
Backend* CreateUdpBackendFromEnvironment();
void DestroyUdpBackend(Backend* backend);

// ---- helpers shared with the SO layer ----------------------------------------------------------------------
// The process-wide link used by the SO API (so_net.cpp).
Link& Default();

}  // namespace VLink

#endif  // NP_VLINK_H
