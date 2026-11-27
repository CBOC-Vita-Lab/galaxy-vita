// Galaxy 3.0 (agent natpmp): a small NAT-PMP (RFC 6886) and PCP (RFC 6887) client. Like np_upnp it asks the home router to forward one UDP
// port to this console and to say what its public address is, for routers that do not speak UPnP IGD (Apple AirPort / Time Capsule,
// OpenWrt with only natpmpd, some ISP boxes). Written from the two RFCs only, no code from any other client; public domain like the rest.
//
//   PROBING   to the gateway's UDP port 5351, every retry round (0, 250, 750, 1750 ms: RFC 6886 doubling from 250 ms, cut short to fit
//             the UI: `probeMs`, 3 s): a PCP MAP request (version 2) AND a NAT-PMP "external address" request (version 0, no side effect).
//             A PCP server answers the MAP: mapped. A NAT-PMP-only server answers the PCP request with "unsupported version" (RFC 6886
//             section 3.5, RFC 6887 section 9) and the other with the address: continue with NAT-PMP. Both lines are the RFCs' own
//             fallback rule; sending them together only saves the round trip.
//   MAPPING   NAT-PMP only: the UDP mapping request (lease 2 h; `mapMs`, 2 s).
//   MAPPED    renewed at half the granted lease (RENEWING), polled every 5 min with a side-effect-free request (NAT-PMP external address,
//             PCP ANNOUNCE) to see the gateway's epoch: a gateway that restarted (epoch went back or ran slow, RFC 6886 section 3.6,
//             RFC 6887 section 8.5) lost the mapping, so it is asked for again at once.
//   REMOVING  lifetime 0 (RFC 6886 section 3.4, RFC 6887 section 15). Then REMOVED.
//
// Every result code (NAT-PMP 1..5, PCP 1..13) ends in FAILED with a sentence, except PCP 11 (the suggested external port is not
// available: asked once more without a suggestion). Non-blocking, same pump style as Upnp: Step() from the helper thread, never the
// game thread.
//
// Hostile input: a reply is accepted only from the gateway's address AND port 5351, with the exact lengths of the RFC, version and
// opcode checked, for PCP the 96-bit nonce, protocol (UDP) and internal port echoed, for NAT-PMP the internal port echoed. Anything
// else is dropped silently. The gateway address comes from the platform (Linux: the default route in /proc/net/route; Vita:
// sceNetCtl's default route); where that is unknown the .1 of this console's /24 is used, and the log says "guessed".
#ifndef NP_NET_NATPMP_H
#define NP_NET_NATPMP_H

#include "np_net.h"

namespace NpNet {

struct NatPmpConfig {
    uint16_t internalPort;      // the UDP port of this console to forward to (required)
    uint32_t internalIp;        // this console's LAN address (host order); 0 = the socket's LocalIp()
    uint16_t externalPort;      // the external port to suggest; 0 = internalPort
    uint32_t leaseSec;          // 0 = 7200
    Addr gateway;               // the router; zero = the default route (else the .1 guess); port 0 = 5351 (tests use another port)
    bool noPcp;                 // NAT-PMP only (also forced when the random generator fails: the PCP nonce must be random)
    uint32_t probeMs;           // 0 = 3000: how long to wait for the first answer
    uint32_t mapMs;             // 0 = 2000: one mapping exchange once the router has answered
    uint32_t pollMs;            // 0 = 300000: epoch poll while mapped
    void (*log)(void* ctx, const char* line);   // one line per event; null = stderr / the Vita log ("[natpmp] ...")
    void* logCtx;
    NatPmpConfig()
        : internalPort(0), internalIp(0), externalPort(0), leaseSec(0), noPcp(false), probeMs(0), mapMs(0), pollMs(0), log(0), logCtx(0) {}
};

class NatPmp {
public:
    enum State { IDLE, PROBING, MAPPING, MAPPED, RENEWING, REMOVING, REMOVED, FAILED };
    enum Proto { PROTO_NONE, PROTO_NATPMP, PROTO_PCP };
    enum { kServerPort = 5351 };

    NatPmp();
    ~NatPmp();
    // `udp` = a socket (not opened yet); null = a platform socket of its own. Owned by this object.
    bool Start(const NatPmpConfig& cfg, uint64_t nowMs, Udp* udp = 0);
    void Step(uint64_t nowMs);
    // Delete the mapping if there may be one (mapped, being asked for, or a PCP request in flight); otherwise straight to REMOVED.
    // Keep calling Step() until Finished() (at most ~1 s).
    void Remove(uint64_t nowMs);
    void Abort();                          // closes everything now (a mapping that exists runs out with its lease)
    State GetState() const { return state_; }
    bool Finished() const { return state_ == IDLE || state_ == REMOVED || state_ == FAILED; }
    Proto Protocol() const { return proto_; }
    uint32_t ExternalIp() const { return extIp_; }              // 0 = not known
    uint16_t ExternalPort() const { return state_ == MAPPED || state_ == RENEWING ? mappedPort_ : 0; }
    uint16_t InternalPort() const { return cfg_.internalPort; }
    uint32_t InternalIp() const { return localIp_; }
    uint32_t Lease() const { return lease_; }
    bool ExternalIsPrivate() const { return extIp_ != 0 && IsPrivateIp(extIp_); }   // 10/8, 172.16/12, 192.168/16, 100.64/10: another NAT
    const char* Error() const { return error_; }
    bool Answered() const { return answered_; }                 // some valid reply from the gateway was seen (it is a NAT-PMP / PCP router)
    int Renewals() const { return renewals_; }
    Addr Gateway() const { return gw_; }
    static const char* StateName(State s);

    // The router's address: the default route when the platform tells (*guessed = false), else the .1 of localIp's /24 (*guessed = true).
    static uint32_t DefaultGateway(uint32_t localIp, bool* guessed);

    // ---- codec (static, pure: the unit tests feed it hostile bytes) -------------------------------------------------------------
    enum Kind { K_NONE = 0, K_PMP_ADDR, K_PMP_MAP, K_PCP_MAP, K_PCP_ANNOUNCE };
    struct Reply {
        Kind kind;
        uint16_t result;       // 0 = success
        uint32_t epoch;        // seconds since the gateway's table was (re)started
        uint32_t extIp;        // NAT-PMP address reply, PCP MAP (IPv4-mapped only: extIpOk says so)
        bool extIpOk;
        uint16_t intPort, extPort;
        uint32_t lifetime;
        uint8_t nonce[12];
    };
    static size_t BuildPmpAddrRequest(uint8_t* out);                                                           // 2 bytes
    static size_t BuildPmpMapRequest(uint8_t* out, uint16_t intPort, uint16_t extPort, uint32_t lifetime);     // 12 bytes
    static size_t BuildPcpMapRequest(uint8_t* out, uint32_t clientIp, const uint8_t* nonce, uint16_t intPort, uint16_t extPort,
                                     uint32_t suggestedExtIp, uint32_t lifetime);                              // 60 bytes
    static size_t BuildPcpAnnounce(uint8_t* out, uint32_t clientIp);                                           // 24 bytes
    // True when the datagram is a well-formed reply of this client's kinds (lengths, version, opcode); fields are then filled.
    // Error replies may be short (RFC 6886: 8 bytes up; RFC 6887 UNSUPP_VERSION: 24 bytes); their unknown fields are 0.
    // A version 0 answer to a PCP MAP request (a NAT-PMP-only server) is K_PMP_MAP with a non-zero result.
    static bool ParseReply(const uint8_t* d, size_t len, Reply* out);
    // True when the epoch went back or ran slow (the gateway restarted): RFC 6886 section 3.6 / RFC 6887 section 8.5.
    static bool EpochRestarted(uint32_t oldEpoch, uint32_t newEpoch, uint64_t elapsedMs);
    static const char* PmpResultText(unsigned code);
    static const char* PcpResultText(unsigned code);

private:
    void Fail(const char* why);
    void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void BeginExchange(uint64_t now, uint32_t budgetMs);
    void SendRound(uint64_t now);
    void Send(const uint8_t* p, size_t n);
    void Receive(uint64_t now);
    void OnReply(const Reply& r, uint64_t now);
    void OnProbeReply(const Reply& r, uint64_t now);
    void OnMapReply(const Reply& r, uint64_t now);
    void Mapped(const Reply& r, uint64_t now, const char* how);
    void StartNatpmpMapping(uint64_t now);
    void NoteEpoch(uint32_t epoch, uint64_t now);
    bool PublicEnough(uint32_t ip) const;

    NatPmpConfig cfg_;
    State state_;
    Proto proto_;
    Udp* udp_;
    Addr gw_;
    uint32_t localIp_, extIp_;
    uint16_t suggestPort_, mappedPort_;
    uint32_t lease_, wantLease_;
    uint8_t nonce_[12];
    bool pcpOk_;
    // exchange
    uint64_t exchStart_, deadline_;
    int sendIdx_;
    // probe
    bool pcpUnsupported_, natSeen_;
    uint64_t natSeenAt_;
    int natErr_;                // a NAT-PMP error code seen in PROBING (-1 none)
    // epoch
    bool haveEpoch_;
    uint32_t epoch_;
    uint64_t epochAt_;
    // mapped
    uint64_t renewAtMs_, leaseEndMs_, pollAtMs_;
    bool polling_, retried11_, mayExist_, answered_;
    int renewals_;
    char error_[96];
};

}  // namespace NpNet

#endif
