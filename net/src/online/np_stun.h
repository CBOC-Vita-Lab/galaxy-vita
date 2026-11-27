// STUN client (RFC 5389 binding requests) driven from the game's own UDP socket, so the public address and port it learns are the
// ones the NAT gives the game's traffic (port 41100, Galaxy). Several servers are asked at once: a NAT that maps the same local port to a
// different public port for every destination is "symmetric" and cannot be punched through (np_peer.cpp), which is why at least two
// distinct destinations are needed.
//
// Non-blocking: Start() sends, Tick() resends (RFC 5389 section 7.2.1: retransmission with a doubling timeout, here capped), the
// owner of the socket passes every STUN-looking datagram to HandlePacket().
#ifndef NP_NET_STUN_H
#define NP_NET_STUN_H

#include "np_net.h"

namespace NpNet {

const int kMaxStunServers = 4;

struct StunConfig {
    uint32_t retransmitMs[8];   // wait after the Nth send (the last one is the final wait); 0 ends the list
    StunConfig() {
        const uint32_t d[8] = {250, 500, 1000, 1500, 2000, 2000, 0, 0};
        memcpy(retransmitMs, d, sizeof d);
    }
};

class StunClient {
public:
    enum State { IDLE, RUNNING, DONE, FAILED };

    StunClient();
    // Sends the first requests. `servers` are resolved addresses (the caller's DNS), at most kMaxStunServers.
    void Start(Env* env, const Addr* servers, int count, const StunConfig& config = StunConfig());
    void Tick();
    // true when the datagram was a response to one of our requests (consumed).
    bool HandlePacket(const Addr& from, const uint8_t* data, size_t len);
    void Reset();

    State GetState() const { return state_; }
    bool Done() const { return state_ == DONE || state_ == FAILED; }
    // Valid once Done(): how many servers answered, the mapped (public) address each reported.
    int Answered() const;
    Addr Mapped(int server) const;            // zero when that server did not answer
    Addr FirstMapped() const;                  // the first answer
    // NAT_CONE when every answer carries the same public ip:port, NAT_SYMMETRIC when they differ, NAT_OPEN when the mapped address is
    // the local one, NAT_UNKNOWN with fewer than two answers.
    int Classify(uint32_t localIp) const;
    // Which servers share an address (a "two server" result from one ip only tests the destination port).
    bool ServersDistinct() const { return distinct_; }

    // Message building / parsing exposed for tests.
    static size_t BuildRequest(uint8_t* out, const uint8_t txid[12]);
    static bool ParseResponse(const uint8_t* data, size_t len, uint8_t txidOut[12], Addr* mapped);

private:
    struct Server {
        Addr addr;
        uint8_t txid[12];
        int sends;
        uint64_t nextMs;
        bool answered, gaveUp;
        Addr mapped;
    };
    void Send(Server& s);
    Env* env_;
    State state_;
    StunConfig cfg_;
    Server servers_[kMaxStunServers];
    int count_;
    bool distinct_;
};

}  // namespace NpNet

#endif
