// A minimal TURN server (RFC 8656 / 5766 subset) for the tests of the relay client (platform/sdk/net/online/np_turn.cpp): UDP only, long-term
// credentials in the "TURN REST" form the online service mints (username "<expiry unix seconds>:<id>", password = base64(HMAC-SHA1(secret, username))),
// Allocate / Refresh / CreatePermission / ChannelBind, ChannelData and Send / Data, relay-to-relay forwarding between two allocations of this server.
// It has no sockets: the owner feeds datagrams in and receives datagrams to send (natsim.h's Sim::AddServer / ServerSend, in the unit tests and in
// natsim_proxy). Written independently of the client's message code so that the two check each other.
//
// Faults for tests: short lifetimes (allocation, permission, channel binding, nonce), a capacity limit (508), expiry of the credentials (401 with a
// fresh nonce, as coturn answers), and counters of what happened.
#ifndef TURNSIM_H
#define TURNSIM_H

#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../src/online/np_net.h"

namespace NatSim {

using NpNet::Addr;

class TurnSim {
public:
    struct Config {
        std::string secret;               // shared with the credential minting (the service or the signaling fake)
        std::string realm;
        uint32_t maxLifetimeSec;          // allocation lifetime cap; also the lifetime granted when the client does not ask
        uint32_t permLifeSec;
        uint32_t bindLifeSec;
        uint32_t nonceLifeSec;            // a nonce older than this is stale (438)
        int maxAllocations;               // 0 = no limit, negative = always full; beyond it: 508 Insufficient Capacity
        std::function<uint64_t()> wallSec;   // unix seconds, for the expiry inside usernames; null = never expired
        bool refuseAll;                   // every Allocate gets 486 (the server's own quota)
        Config() : secret("turn-test-secret"), realm("turn.test"), maxLifetimeSec(600), permLifeSec(300), bindLifeSec(600), nonceLifeSec(3600), maxAllocations(0),
                   refuseAll(false) {}
    };
    struct Stats {
        uint64_t allocations, refreshes, releases, permissions, bindings, channelIn, channelOut, dataIndOut, sendInd, relayedPackets, relayedBytes;
        uint64_t unauthorized, staleNonce, expiredCreds, mismatch, noPermission, expiredAllocs, badIntegrity, noTarget, unknownChannel;
    };
    // (listen address the datagram went to / is sent from, destination, bytes, now in ms)
    typedef std::function<void(const Addr&, const Addr&, const std::vector<uint8_t>&, uint64_t)> SendFn;
    struct Msg;   // (parsed message; defined in turnsim.cpp)

    TurnSim(const Config& config, SendFn send);
    // `listen`: the server address the client used; `client`: its address as the server sees it.
    void OnDatagram(const Addr& listen, const Addr& client, const std::vector<uint8_t>& data, uint64_t nowMs);
    const Stats& GetStats() const { return stats_; }
    int LiveAllocations() const { return int(allocs_.size()); }
    Addr ClientOf(const Addr& relayed) { Alloc* a = FindRelayed(relayed); return a ? a->client : Addr(); }   // (tests: where a datagram for that allocation's owner goes)
    Config& Cfg() { return cfg_; }
    // Credentials the way the service mints them (for tests that need them directly).
    static std::string MakePassword(const std::string& secret, const std::string& username);

private:
    struct Channel { uint16_t number; Addr peer; uint64_t expires; };
    struct Alloc {
        Addr client, listen, relayed;
        std::string user;
        uint64_t expires;
        std::map<uint32_t, uint64_t> perms;   // peer ip -> expiry
        std::vector<Channel> channels;
        uint8_t txid[12];
        std::vector<uint8_t> okResponse;      // for retransmissions of the Allocate
    };
    void Sweep(uint64_t now);
    Alloc* FindAlloc(const Addr& listen, const Addr& client);
    Alloc* FindRelayed(const Addr& a);
    std::string Nonce(uint64_t now) const;
    bool NonceFresh(const std::string& nonce, uint64_t now) const;
    // 0: authenticated (user in *user, key in key); otherwise the answer was sent
    bool Authenticate(const Addr& listen, const Addr& client, const std::vector<uint8_t>& d, const Msg& m, uint64_t now, std::string* user, uint8_t key[16]);
    void SendError(const Addr& listen, const Addr& client, const Msg& m, int code, const char* text, bool withChallenge, uint64_t now, const uint8_t* key = 0);
    void Forward(Alloc* from, const Addr& peer, const uint8_t* data, size_t len, uint64_t now);
    void DeliverTo(Alloc* to, const Addr& fromRelayed, const uint8_t* data, size_t len, uint64_t now);

    Config cfg_;
    SendFn send_;
    std::vector<Alloc> allocs_;
    uint16_t nextPort_;
    Stats stats_;
};

}  // namespace NatSim

#endif
