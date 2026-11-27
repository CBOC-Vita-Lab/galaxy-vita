// TURN client (RFC 8656, with RFC 5766 / 5389 / 8489 wire details) over the game's own UDP socket: allocation with long-term credentials,
// refresh before expiry, permissions and channel bindings for the other consoles' relayed addresses, release on leave.
//
// What it does NOT do: it does not send game data. A Peer (np_peer.cpp) wraps its datagrams in ChannelData itself (4-byte header, channel number
// from here) and OnlineNet unwraps what comes back; this class is the control plane. It is a non-blocking state machine (Tick() + HandlePacket(),
// nothing waits), like the STUN client, and runs on the transport's clock.
//
// Flow per console:   Allocate (401 realm+nonce round trip, then MESSAGE-INTEGRITY with key = MD5(user:realm:password))
//                     -> XOR-RELAYED-ADDRESS (published to the other side through the room), XOR-MAPPED-ADDRESS (our public address)
//                     -> BindPeer(slot, their relayed address): CreatePermission + ChannelBind (0x4000..0x7FFF) -> ChannelData both ways
//                     -> Refresh at half the lifetime, permissions / bindings on a schedule, Refresh(0) when the session ends.
// Both consoles allocate: each sends to the other's relayed address from its own, which is what keeps raw addresses out of the room in
// relay-only mode (the room then carries relayed addresses only).
//
// Error handling: every request is retransmitted on a timer (RFC 5389 style, capped) and given up on after the last wait. A refused sign-in,
// a stale nonce (438), a lost allocation (437 / no answer to refreshes) are handled here; credentials that ran out are reported through
// CredentialsRejected() so the owner can fetch new ones (the TURN key never reaches the console, only short-lived credentials).
#ifndef NP_NET_TURN_H
#define NP_NET_TURN_H

#include "np_net.h"

namespace NpNet {

struct TurnConfig {
    uint32_t lifetimeSec;          // allocation lifetime requested (the server may shorten it)
    uint32_t refreshPct;           // the allocation is refreshed after this share of its granted lifetime
    uint32_t permissionRefreshMs;  // CreatePermission again this often (a permission lives 5 minutes)
    uint32_t channelRefreshMs;     // ChannelBind again this often (a binding lives 10 minutes)
    uint32_t rtoMs[8];             // wait after the Nth transmission of a request; 0 ends the list (then the request is given up)
    TurnConfig() : lifetimeSec(300), refreshPct(50), permissionRefreshMs(180000), channelRefreshMs(180000) {
        const uint32_t d[8] = {300, 600, 1200, 1600, 1600, 1600, 1600, 0};
        memcpy(rtoMs, d, sizeof d);
    }
};

struct TurnStats {
    uint64_t requests, retransmits, successes, errors, dataIndications, channelRx;
    uint32_t allocations, refreshes, permissions, bindings;
};

class TurnClient {
public:
    enum State {
        IDLE,
        ALLOCATING,   // the allocation is being (re)made; Relayed() is unknown
        READY,        // allocated: Relayed() is valid
        FAILED        // gave up; Error() says why
    };

    TurnClient();
    // `servers`: the TURN server addresses to try in turn (UDP). Credentials are the short-lived ones of the online service.
    void Start(Env* env, const Addr* servers, int count, const char* user, const char* password, const TurnConfig& config = TurnConfig());
    void Tick();
    // A datagram that looks like STUN (first byte 00xxxxxx). 0: not ours (a STUN binding response, another source); 1: consumed;
    // 2: a Data indication carrying a datagram of a peer that has no channel yet: *inner/*innerLen (points into `data`) and *peer are set.
    int HandlePacket(const Addr& from, const uint8_t* data, size_t len, const uint8_t** inner, size_t* innerLen, Addr* peer);
    // Frees the allocation on the server (Refresh with lifetime 0), best effort, once. The client is idle afterwards.
    void Release();
    void Reset();

    State GetState() const { return state_; }
    bool Ready() const { return state_ == READY; }
    const char* Error() const { return error_; }
    // Authenticated requests were refused after the allocation had worked: the credentials ran out (or were revoked). The owner fetches
    // new ones and calls Start() again (Release() first while the old ones still work).
    bool CredentialsRejected() const { return credsRejected_; }
    bool AuthRefused() const { return authRefused_; }   // FAILED because the server did not accept the credentials (they may have run out: ask for new ones)
    Addr Server() const;                        // the server in use
    Addr Relayed() const { return state_ == READY ? relayed_ : Addr(); }
    Addr Mapped() const { return mapped_; }     // our public address as the TURN server saw it
    uint64_t ExpiresAt() const { return expiresAt_; }   // when the allocation ends if it is not refreshed again (the owner's clock, ms)
    uint32_t Generation() const { return generation_; }   // grows with every new allocation (new relayed address, bindings start afresh)
    uint32_t LifetimeSec() const { return lifetime_; }

    // The relayed address of the member in `slot`: starts CreatePermission + ChannelBind. Returns the channel number (0x4000..0x7FFF), 0 when
    // the slot is unusable. Calling it again with the same address is free; a new address gets a new channel.
    uint16_t BindPeer(int slot, const Addr& peerRelayed);
    bool ChannelBound(int slot) const;
    uint16_t ChannelOf(int slot) const;
    int SlotForChannel(uint16_t channel) const;   // -1 when no slot uses it
    void CountChannelRx() { ++stats_.channelRx; }
    const TurnStats& Stats() const { return stats_; }

    // Exposed for tests: the long-term key and the integrity of a finished message.
    static void LongTermKey(uint8_t out[16], const char* user, const char* realm, const char* password);
    static bool AddIntegrity(uint8_t* msg, size_t* len, size_t cap, const uint8_t key[16]);
    // Hex dump of the last authenticated request (tests check its MESSAGE-INTEGRITY with an independent implementation).
    size_t LastAuthRequest(uint8_t* out, size_t cap, char* realm, char* user, size_t strCap) const;

private:
    enum Kind { TK_ALLOC, TK_REFRESH, TK_RELEASE, TK_PERM, TK_BIND };
    struct Txn {
        bool active;
        uint8_t kind;
        int arg;                 // slot for PERM / BIND
        uint8_t txid[12];
        uint8_t msg[512];
        size_t len;
        int sends;
        uint64_t nextMs;
        int authRetries;
        bool authed;
        uint32_t peerIp;         // PERM / BIND: the address and channel the request was built for (the slot may be bound elsewhere by the time the answer comes)
        uint16_t peerPort, channel;
    };
    struct Slot {
        bool used;
        Addr peer;
        uint16_t channel;
        bool permOk, bindOk, permBusy, bindBusy;
        uint64_t permAt, bindAt, retryAt;
    };
    struct Parsed;

    bool StaleSlotTxn(const Txn* t) const;
    Txn* Free();
    Txn* Find(const uint8_t txid[12]);
    void Begin(Kind kind, int arg);
    void Rebuild(Txn* t);
    void Transmit(Txn* t);
    void TxnTimeout(Txn* t);
    void OnSuccess(Txn* t, const Parsed& p);
    void OnError(Txn* t, const Parsed& p);
    void StartAlloc();
    void NextServerOrFail(const char* why);
    void Fail(const char* why);
    void SlotsReset();
    void LostAllocation(const char* why);
    uint64_t Now() const { return env_->Now(); }
    void SendRaw(const uint8_t* d, size_t n);
    void RefreshSchedule(uint64_t now);

    Env* env_;
    TurnConfig cfg_;
    State state_;
    Addr servers_[4];
    int nServers_, serverIdx_;
    char user_[160], pass_[160], realm_[128], nonce_[160];
    bool haveNonce_;
    uint8_t key_[16];
    Addr relayed_, mapped_;
    uint32_t lifetime_, generation_;
    uint64_t refreshAt_, expiresAt_;
    bool credsRejected_, authRefused_, randomFailed_, released_, wasReady_, reallocAfterRelease_;
    uint64_t allocRetryAt_, orphanSince_;   // an allocation of earlier credentials still holds this socket's 5-tuple: wait for it to expire
    int refreshFails_;
    Txn txns_[8];
    Slot slots_[kMaxSlots];
    uint16_t nextChannel_;
    char error_[128];
    TurnStats stats_;
    uint8_t lastAuth_[512];
    size_t lastAuthLen_;
};

}  // namespace NpNet

#endif
