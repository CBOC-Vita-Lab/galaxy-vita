// Signaling: how two consoles find each other before there is a UDP path between them.
//
// The real implementation is HTTPS to the Cloudflare Worker (server/PROTOCOL.md, written by another worker; it sits on the TLS client
// of platform/sdk/net/np_https). This interface is all the transport needs from it. It is polled and never blocks: an implementation
// does its network work on its own thread and Poll() only moves results over.
//
// The room is the lobby: the host creates it (invite code), friends join with the code. The server decides everything that has to be
// the same everywhere: the slot of each member, its virtual IP (vip: this fixes the collision of two NATed Vitas that both report
// 192.168.1.x), the per-match session key (32 random bytes, the key of np_peer.cpp) and the punch cookie (natneg style). Each member
// publishes its candidates (the public address STUN reported, its local address, its NAT class) and sees everyone else's.
#ifndef NP_NET_SIGNAL_H
#define NP_NET_SIGNAL_H

#include "np_net.h"

namespace NpNet {

struct SigCandidates {
    bool valid;
    Addr pub;          // as the STUN servers saw it
    Addr pub2;         // as the second STUN server saw it (differs from `pub` behind a symmetric NAT: lets the server see the port allocation)
    Addr local;        // this console's address on its own network
    uint8_t nat;       // NatClass
    uint32_t gen;      // bumped whenever the console rebuilt its link (new socket, resume from sleep): the other side then punches afresh
    Addr relay;        // the console's TURN allocation (relayed address), zero until it has one; publishing it does not change `gen`
    bool relayOnly;    // privacy mode ("Connection: Relay only"): only `relay` is published, never an address of this console
    Addr mapped;       // Galaxy (netx): a port the console's router forwards to it (UPnP AddPortMapping), public ip:port; zero = none.
                       // Published as one more "srflx" candidate; a member probes it as a public candidate (fewer pairs need the relay)
    SigCandidates() : valid(false), nat(NAT_UNKNOWN), gen(0), relayOnly(false) {}
};

// Short-lived relay (TURN) credentials the service minted for this room (POST /v1/turn). The TURN key itself never reaches the console.
struct TurnInfo {
    char urls[4][160];     // "turn:host:port?transport=udp" (UDP only; the Vita has no TCP), best first
    int nUrls;
    char user[160];
    char pass[160];
    uint32_t ttlSec;       // how long the credentials work from now
    TurnInfo() : nUrls(0), ttlSec(0) { user[0] = pass[0] = 0; memset(urls, 0, sizeof urls); }
};

// What a console reports once its attempt to reach a member ended (A6): the server counts failures per pair and steers to the relay.
// Galaxy (rooms of up to 4): the report names the member it is about ("peer": slot in the HTTP body).
struct PunchReport {
    uint8_t peerSlot;
    bool ok;
    uint32_t ms;             // time from the start of the attempt to the verdict
    uint8_t pathKind;        // CandidateKind of the path that worked (ok only)
    uint8_t myNat, peerNat;  // NatClass
    Addr observed;           // the peer's address as seen on the working path (ok only)
    PunchReport() : peerSlot(0), ok(false), ms(0), pathKind(0), myNat(0), peerNat(0) {}
};

// Galaxy (core2): a public player code from the service ("K7QM-4DXT": 4 + '-' + 4 characters of ABCDEFGHJKMNPQRSTUVWXYZ23456789, the
// service's code alphabet) into out[10]; anything else (absent, other length, lower case, another character) gives "". Untrusted input.
inline bool SigPlayerCode(const char* in, char out[10]) {
    static const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    out[0] = 0;
    if (!in) return false;
    for (int i = 0; i < 9; ++i) {
        const char c = in[i];
        if (i == 4) { if (c != '-') return false; continue; }
        bool ok = false;
        for (const char* a = kAlpha; *a && !ok; ++a) ok = *a == c;
        if (!ok) return false;   // (also the terminator: too short)
    }
    if (in[9] != 0) return false;
    for (int i = 0; i < 9; ++i) out[i] = in[i];
    out[9] = 0;
    return true;
}

struct SigMember {
    bool present;
    uint8_t slot;
    uint32_t vip;          // virtual IPv4 (host order) the lobby assigned; unique in the room
    char name[16];
    SigCandidates cands;
    int link;              // the server's view of the member's presence: 0 ok, 1 quiet (5 s without a call), 2 lost (15 s)
    bool pending;          // svcfix2 item 8: the host's console has not accepted this member yet (a stranger who joined with a code): no address is exchanged
    bool stale;            // the member's endpoint is older than the NAT mapping life: it should run STUN again and republish
    // Galaxy (rooms of up to 4, the service's members[] view): the pair of this console and this member has its own start time and relay
    // advice. Zero / false = none for this pair: the room-wide values of SigRoom (a two-player view, the fakes) apply.
    uint32_t pid;          // the member's account id (0 = not given)
    char label[16];        // svcfix3: the name to show: members[].label, which adds "#" and the last 4 characters of the friend code when another member has the same name; "" = none (an older service)
    bool nameDup;          // svcfix3: members[].nameDup: another member of the room has the same name (label then differs from name)
    char code[10];         // Galaxy (core2): the member's public player code "XXXX-XXXX" (members[].code; the friend code), "" = none / malformed
    uint64_t punchAtMs;    // when this pair starts punching, on the transport's clock; 0 = at once (or SigRoom::punchAtMs)
    bool steerRelay;       // the service says: this pair skips hole punching and uses the relay
    char steerReason[24];
    SigMember() : present(false), slot(0), vip(0), link(0), pending(false), stale(false), pid(0), nameDup(false), punchAtMs(0), steerRelay(false) { name[0] = 0; label[0] = 0; nameDup = false; code[0] = 0; steerReason[0] = 0; }
};

struct SigRoom {
    char invite[16];
    uint32_t cookie;
    uint8_t key[32];        // per-match session key
    int mySlot;
    SigMember members[kMaxSlots];
    uint32_t version;       // changes whenever anything in the room changed
    uint64_t punchAtMs;     // when both sides start punching, on the transport's clock (Signal::SetClock); 0 = at once (Galaxy: a member's own value wins)
    bool steerRelay;        // the server says: skip hole punching, use the relay (relay-only profile, symmetric NAT, failures of this pair) (Galaxy: or the member's own)
    char steerReason[24];
    // Galaxy (DESIGN.md section 8, host migration): the member that is the room's host now ("host_slot"; 0 until the first host left) and how many
    // times the host role moved ("host_epoch"). The star topology is centred on hostSlot.
    int hostSlot;
    uint32_t hostEpoch;
    SigRoom() : cookie(0), mySlot(-1), version(0), punchAtMs(0), steerRelay(false), hostSlot(0), hostEpoch(0) {
        steerReason[0] = 0;
        invite[0] = 0;
        memset(key, 0, sizeof key);
    }
};

class Signal {
public:
    enum Status { IDLE, WORKING, IN_ROOM, FAILED, CLOSED };
    virtual ~Signal() {}
    virtual void Host(const char* playerName) = 0;                       // create a room; asynchronous
    virtual void Join(const char* invite, const char* playerName) = 0;   // join a room by invite code; asynchronous
    virtual void PublishCandidates(const SigCandidates& mine) = 0;       // after STUN (and again whenever they change)
    virtual void ReportPunch(const PunchReport& report) = 0;             // fire and forget
    virtual void Leave() = 0;
    // beta diagnostics: the session's numbers (a JSON object, no name / id / address) sent with the leave / close request as {"summary":...}. Set before Leave(); "" = none.
    virtual void SetLeaveSummary(const char* json) { (void)json; }
    virtual void Poll() = 0;                                              // once a frame
    virtual Status GetStatus() const = 0;
    virtual const SigRoom& Room() const = 0;                              // valid while IN_ROOM
    virtual const char* Error() const = 0;                                // when FAILED
    // Galaxy (fix3): the service's error code behind a FAILED / CLOSED status ("not_member", "version_mismatch", "room_full",
    // "banned", ...), "" when none (a transport failure, a fake). Never shown: the game words it in its own language.
    virtual const char* ErrorCode() const { return ""; }
    // svcfix3: after a failed TurnState() that was a limit of the relay (turn_room_limit / turn_player_limit / turn_budget): how long, in ms, not to ask again (0 = an ordinary failure)
    virtual uint32_t TurnWaitMs() const { return 0; }
    // Optional (defaults do nothing): the transport's clock, so a server time can be turned into "when to start punching"; the link is confirmed
    // both ways and the match is about to start (a room's TTL and match record); the STUN servers the service recommends ("host:port,host:port").
    virtual void SetClock(uint64_t (*now)(void*), void* ctx) { (void)now; (void)ctx; }
    virtual void Started() {}
    // svcfix2: the slots this console exchanges direct heartbeats with right now (bit n = member n; an HTTP signal reports it to the service so a
    // started room whose peers all heartbeat each other need not be polled; a LOSS makes it ask the service at once). Default: ignored.
    virtual void SetDirectLinks(uint32_t mask) { (void)mask; }
    // svcfix2 item 8: the HOST's decision about a member the service holds back (a stranger who joined by code: SigMember::pending). Called once
    // per joiner from Poll(); true = accept (the service then lets both sides see each other's address), false = leave him waiting. The default
    // (no policy set) accepts: the console's lobby shows the name first, and a host who kicks him is gone before the address matters.
    typedef bool (*AcceptFn)(void* ctx, int slot, const char* name);
    virtual void SetAcceptPolicy(AcceptFn fn, void* ctx) { (void)fn; (void)ctx; }
    virtual const char* StunList() const { return ""; }

    // ---- relay (TURN) credentials ----------------------------------------------------------------------------------------
    // RequestTurn() asks the service for credentials for this room (only possible once a second member joined); asynchronous, can be called
    // again for fresh ones. TurnState() then goes WORKING -> READY (Turn() valid) or FAILED (TurnError() is a sentence for the player: "the relay budget
    // is used up", ...). ReportTurnUsage() tells the service roughly how many bytes were relayed (its budget); fire and forget.
    enum TurnStatus { TURN_NONE, TURN_WORKING, TURN_READY, TURN_FAILED };
    virtual void SetRelayOnly(bool on) { (void)on; }   // the player's "Connection: Relay only" setting, before Host() / Join(): the service learns it as the profile's relay-only flag
    virtual void RequestTurn() {}
    virtual TurnStatus TurnState() const { return TURN_NONE; }
    virtual const TurnInfo& Turn() const { static TurnInfo none; return none; }
    virtual const char* TurnError() const { return ""; }
    virtual void ReportTurnUsage(uint64_t bytes) { (void)bytes; }
};

// Test fakes (np_signal_fake.cpp, not built for the Vita).
//   SignalMemoryHub: rooms shared by Signal objects of one process.  SignalFileDir: rooms in a directory, shared by processes.
class SignalMemoryHub;
SignalMemoryHub* CreateSignalMemoryHub(uint32_t seed);
void DestroySignalMemoryHub(SignalMemoryHub* hub);
Signal* CreateMemorySignal(SignalMemoryHub* hub);
// The punch reports a hub received (tests), oldest first; the count.
int SignalMemoryHubReports(SignalMemoryHub* hub, PunchReport* out, int max);
Signal* CreateFileSignal(const char* directory);
// Relay credentials of the fakes. They mint what the real service mints: username "<expiry unix seconds>:<id>", credential = base64(HMAC-SHA1(secret,
// username)) (the TURN REST scheme), which the test TURN server (tools/net/turnsim.cpp) checks. `urls` is comma separated. The memory fake mints with
// the transport's clock; `reportedTtl` is what the client is told, `realTtl` what is inside the username (tests make them differ to exercise the
// reactive path), `budget` the number of credential requests that succeed (-1 = unlimited; then "turn_budget" is returned).
void SignalMemoryHubSetTurn(SignalMemoryHub* hub, const char* urls, const char* secret, uint32_t reportedTtlSec, uint32_t realTtlSec, int budget);
int SignalMemoryHubTurnRequests(SignalMemoryHub* hub);
// Galaxy: what the service does with a member that went silent (link "lost"): it leaves the room; if it was the host, the lowest remaining slot becomes
// the host (host_slot, host_epoch + 1; DESIGN.md section 8). A member that calls Leave() is handled the same way.
void SignalMemoryHubEvict(SignalMemoryHub* hub, const char* invite, int slot);
void FakeMintTurn(TurnInfo* out, const char* urls, const char* secret, uint64_t nowMs, uint32_t reportedTtlSec, uint32_t realTtlSec, uint32_t serial);

}  // namespace NpNet

#endif
