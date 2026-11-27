// Galaxy 3.0 (agent netx): a small UPnP Internet Gateway Device client. It asks the home router to forward one UDP port to this
// console (AddPortMapping) and to say what its public address is (GetExternalIPAddress), so that a host can be reached from the
// internet without the player configuring the router. Written from the standards only (UPnP Device Architecture 1.0/1.1: SSDP
// M-SEARCH, device description; IGD 1.0/2.0 WANIPConnection / WANPPPConnection service templates; SOAP 1.1), no code from any
// other client.
//
//   SEARCHING   SSDP M-SEARCH to 239.255.255.250:1900 (and, as a fallback for routers that filter multicast, to the .1 of the
//               console's /24), a few rounds; every answer's LOCATION must point at the address the answer came from.
//   DESCRIBING  HTTP GET of the description; every WANIPConnection / WANPPPConnection service with its control URL (which must be
//               on the same address as the description: a description cannot send us to another host).
//   ASKING_IP   SOAP GetExternalIPAddress on each service until one names a usable address (a Fritz!Box lists both kinds; one is idle).
//   MAPPING     SOAP AddPortMapping (UDP, external port -> internal port of this console, a lease). 718 (conflict): if the existing
//               entry is this console's own (GetSpecificPortMappingEntry, CHECKING) it is kept, else the next external port is tried;
//               725 (only permanent leases) or 402 (invalid args, some routers reject a lease): once more with lease 0.
//   MAPPED      renewed (AddPortMapping again) at half the lease. Remove() -> REMOVING (DeletePortMapping) -> REMOVED.
//
// Non-blocking: Step() makes a few non-blocking socket calls and returns; the caller pumps it from a helper thread (never the game
// thread). Every exchange has a deadline (connect + request + answer), the search has its own, nothing waits for a slow router.
// Hostile input: a router's answers are untrusted. Everything is parsed from (pointer, length) with bounded copies, the answer
// buffer has a fixed size (a longer answer is cut and parsed as far as it goes), the request text only ever contains values this
// class validated (printable, no CR/LF, no quotes or angle brackets), URLs are IPv4 literals only.
#ifndef NP_NET_UPNP_H
#define NP_NET_UPNP_H

#include "np_net.h"

namespace NpNet {

struct UpnpConfig {
    uint16_t internalPort;      // the UDP port of this console to forward to (required)
    uint32_t internalIp;        // this console's LAN address (host order); 0 = the search socket's LocalIp()
    uint16_t externalPort;      // the first external port to ask for; 0 = internalPort
    int portTries;              // external ports tried when one is taken by another device; 0 = 8
    uint32_t leaseSec;          // 0 = 3600
    const char* description;    // what the router's page lists (ASCII); null = "Super Mario Galaxy"
    Addr ssdpTarget;            // tests: the M-SEARCH goes here only (a fake gateway); zero = multicast + gateway guess
    uint32_t searchMs;          // 0 = 3000: how long the search waits for an answer
    uint32_t gatewayIp;         // corefix 8: the default-route gateway (host order): its SSDP answer is tried first, and waited for; 0 = ask the
                                // socket (Udp::GatewayIp); still 0 = unknown: every answer counts alike (as before)
    uint16_t staleExternalPort; // corefix 8: a permanent mapping (lease 0) an earlier run left behind (the caller kept it in mp.cfg): deleted
                                // first, if the router says it maps to this console
    uint32_t httpMs;            // 0 = 3000: one HTTP exchange (connect, request, answer)
    void (*log)(void* ctx, const char* line);   // one line per event; null = stderr / the Vita log ("[upnp] ...")
    void* logCtx;
    UpnpConfig()
        : internalPort(0), internalIp(0), externalPort(0), portTries(0), leaseSec(0), description(0), searchMs(0), gatewayIp(0),
          staleExternalPort(0), httpMs(0), log(0), logCtx(0) {}
};

class Upnp {
public:
    enum State { IDLE, SEARCHING, DESCRIBING, ASKING_IP, MAPPING, CHECKING, MAPPED, RENEWING, REMOVING, REMOVED, FAILED };
    enum { kMaxGateways = 4, kMaxServices = 4, kAnswerCap = 32 * 1024, kRequestCap = 2048, kTypeCap = 96, kPathCap = 192 };

    Upnp();
    ~Upnp();
    // Starts the search. `udp` = a socket for SSDP (not opened yet); null = a platform socket of its own. Owned by this object.
    bool Start(const UpnpConfig& cfg, uint64_t nowMs, Udp* udp = 0);
    void Step(uint64_t nowMs);
    // Delete the mapping if there may be one (MAPPED, RENEWING, or a mapping request in flight); otherwise straight to REMOVED.
    // Keep calling Step() until Finished().
    void Remove(uint64_t nowMs);
    void Abort();                          // closes everything now (a mapping that exists runs out with its lease)
    State GetState() const { return state_; }
    bool Finished() const { return state_ == IDLE || state_ == REMOVED || state_ == FAILED; }
    uint32_t ExternalIp() const { return extIp_; }              // 0 = not known (known from ASKING_IP on, kept on failure)
    uint16_t ExternalPort() const { return state_ == MAPPED || state_ == RENEWING ? mappedPort_ : 0; }
    uint16_t InternalPort() const { return cfg_.internalPort; }
    uint32_t InternalIp() const { return localIp_; }
    uint32_t Lease() const { return lease_; }
    // corefix 8: the router keeps this mapping for ever (it only takes lease 0): the caller records it so a crash does not leave it behind.
    bool RemovedOk() const { return removedOk_; }   // corefix 8: the last DeletePortMapping of the mapping was answered 200
    bool Permanent() const { return (state_ == MAPPED || state_ == RENEWING) && lease_ == 0; }
    // The "public" address is itself private or carrier-grade NAT (10/8, 172.16/12, 192.168/16, 100.64/10): the router sits behind
    // another NAT and the forwarded port is NOT reachable from the internet, however well the mapping went.
    bool ExternalIsPrivate() const { return extIp_ != 0 && IsPrivateIp(extIp_); }
    const char* Error() const { return error_; }
    int Renewals() const { return renewals_; }
    static const char* StateName(State s);

    // ---- parsers (static, pure: the unit tests feed them hostile text) ----------------------------------------------------------
    // An SSDP answer: true with the LOCATION value when it is a "200" answer naming a gateway (ST or USN mentions an IGD or a WAN
    // connection service). `location` gets at most cap-1 bytes and is always terminated.
    static bool ParseSsdpAnswer(const char* data, size_t len, char* location, size_t cap);
    // "http://a.b.c.d[:port][/path]" -> ip (host order), port (80 by default), path ("/" when absent). IPv4 literals only; the path
    // must be printable ASCII without spaces, quotes or angle brackets. False on anything else.
    static bool ParseUrl(const char* url, uint32_t* ip, uint16_t* port, char* path, size_t pathCap);
    struct Service {
        char type[kTypeCap];    // "urn:schemas-upnp-org:service:WANIPConnection:1"
        uint32_t ip;
        uint16_t port;
        char path[kPathCap];
    };
    // The WAN connection services of a description, with control URLs resolved against URLBase / the description's own address.
    // Services whose control URL names another host are dropped. Returns how many (at most `max`).
    static int ParseDescription(const char* xml, size_t len, uint32_t descIp, uint16_t descPort, Service* out, int max);
    // The text of the first element <name> (any namespace prefix) in xml[0..len), entities &amp; &lt; &gt; &quot; &apos; decoded.
    static bool ElementText(const char* xml, size_t len, const char* name, char* out, size_t cap);
    // A complete HTTP answer in buf[0..len): status, where the body starts and how long it is. `eof` = the peer closed (a body
    // without a length ends there). Chunked bodies are decoded in place. Returns 1 complete, 0 not complete (more
    // to come, or cut short when `eof`), -1 malformed.
    static int ParseHttp(char* buf, size_t len, bool eof, bool truncated, int* status, size_t* bodyOff, size_t* bodyLen);
    static bool ParseIp(const char* text, uint32_t* ip);   // strict dotted quad, nothing around it but blanks
    static int SoapErrorCode(const char* body, size_t len);   // <errorCode>N</errorCode>, -1 when absent

private:
    struct Http;
    enum Kind { REQ_DESC, REQ_IP, REQ_ADD, REQ_CHECK, REQ_DEL };
    void BeginMapping(uint64_t now);               // corefix 8: (stale cleanup,) check that the external port is free, then AddPortMapping
    bool StartCheck(uint64_t now, uint16_t port);
    bool StartDelete(uint64_t now, uint16_t port);
    void Fail(const char* why);
    void Log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void SendSearch(uint64_t now);
    void StepSearch(uint64_t now);
    bool StartGet(uint64_t now);
    bool StartSoap(Kind kind, uint64_t now);
    int StepHttp(uint64_t now);   // 1 done (answer in buf_), 0 running, -1 failed (error_ says why)
    void CloseHttp();
    void OnAnswer(uint64_t now);
    void NextGateway(uint64_t now, const char* why);
    void NextService(uint64_t now, const char* why);

    UpnpConfig cfg_;
    State state_;
    Udp* udp_;
    struct Gateway { uint32_t ip; uint16_t port; char path[kPathCap]; };
    Gateway gw_[kMaxGateways];
    int nGw_, gwIdx_;
    Service svc_[kMaxServices];
    int nSvc_, svcIdx_;
    Http* http_;
    Kind kind_;
    char* buf_;      // kAnswerCap + 1
    char* req_;      // kRequestCap
    char desc_[48];
    uint32_t localIp_, extIp_;
    uint16_t tryPort_, mappedPort_;
    int triesLeft_;
    uint32_t lease_;
    bool leaseRetried_, removeAfter_, mappingMayExist_;
    bool removedOk_;
    bool preCheck_;        // corefix 8: CHECKING is the look before AddPortMapping (is the external port mapped to another device?)
    uint8_t cleanup_;      // corefix 8: 1 = looking at the stale mapping, 2 = deleting it
    uint16_t opPort_;      // the external port of the CHECK / DEL in flight
    uint64_t startMs_, firstAnswerMs_, nextSendMs_, renewAtMs_, leaseEndMs_;
    int sends_, renewals_, answersSeen_;
    char error_[96];
};

}  // namespace NpNet

#endif
