// One remote console of the room: hole punching and the encrypted session on top of it.
//
// Hole punching follows GameSpy natneg (the model of the retail game, native/ref/RVL_SDK/dwc): the lobby gave both sides a cookie
// and the other side's addresses (the public one STUN reported, the local ones); both start at the same time and send probes to every
// candidate at a fixed interval; the first path on which an authentic probe comes back is used (the source address of the inbound
// probe, which is what makes cone-against-symmetric work), both sides confirm it, and probes keep being answered while the other
// side may still be waiting (linger). A local (same-LAN) path is preferred over the public one; a relay path (TURN ChannelData over
// the same socket, the client comes later) ranks last.
//
// Every packet is authenticated with the room's per-match session key. Probes carry a random "hello" of each side and echo the
// other's: that proves the path works in both directions and freshness (a recorded probe cannot confirm a new attempt), and the data
// key is derived from both hellos, so a rebuilt link never reuses a key/nonce pair with sequence numbers starting over.
//
//   PUNCH  c1 | from | to | flags | hello[8] | echo[8] | cookie[4] | mac[16]            (40 bytes, BLAKE2b-MAC with the punch key)
//   DATA   c2 | from | seq[4] | ciphertext | tag[16]    XChaCha20-Poly1305, nonce = from | 0.. | seq, additional data = the 6 header bytes
//   CTRL   c3 | from | seq[4] | ciphertext(1 byte: 1 keep-alive, 2 close) | tag[16]
#ifndef NP_NET_PEER_H
#define NP_NET_PEER_H

#include <deque>
#include <vector>

#include "np_crypto.h"
#include "np_net.h"

namespace NpNet {

enum CandidateKind { CAND_LOCAL = 0, CAND_PUBLIC = 1, CAND_RELAY = 2 };   // also the preference order, best first
struct Candidate {
    Addr addr;               // where to send (for a relay: the relay server)
    uint8_t kind;            // CandidateKind
    uint16_t channel;        // relay only: the TURN channel number (0x4000..0x7FFF) this peer is bound to
    Candidate() : kind(CAND_PUBLIC), channel(0) {}
    Candidate(const Addr& a, int k, uint16_t ch = 0) : addr(a), kind(uint8_t(k)), channel(ch) {}
};

struct PunchConfig {
    uint32_t intervalMs;        // between probe rounds (natneg: 700)
    uint32_t timeoutMs;         // no confirmed path after this: needs relay
    uint32_t hopelessTimeoutMs; // the same when PunchPossible() says it cannot work (two symmetric NATs)
    uint32_t localGraceMs;      // after a public path confirmed first, wait this long for a local one
    uint32_t lingerMs;          // keep probing the chosen path this long after connecting (until the other side confirms too)
    uint32_t confirmGraceMs;    // connected is reported at the latest this long after our own confirmation
    uint32_t keepaliveMs;       // idle send interval (keeps NAT mappings and the loss detector alive)
    uint32_t staleMs;           // nothing received for this long: probe every candidate again (NAT rebinding, brief outage)
    uint32_t lostMs;            // nothing received for this long: the peer is lost
    bool symmetricRelay;        // a symmetric NAT on either side goes straight to NEEDS_RELAY after hopelessTimeoutMs (the policy for when a relay exists)
    PunchConfig()
        : intervalMs(500), timeoutMs(8000), hopelessTimeoutMs(1200), localGraceMs(300), lingerMs(5000), confirmGraceMs(1500),
          keepaliveMs(2000), staleMs(3000), lostMs(25000), symmetricRelay(false) {}
};

struct PeerParams {
    uint8_t mySlot, peerSlot;
    uint8_t key[32];            // the room's per-match session key
    uint32_t cookie;
    int myNat, peerNat;         // NatClass
    bool tryLocal;              // the peer's local candidates are on our LAN (same public address): probe them
    Addr myLocal;               // this console's own address (never probed)
    Candidate cands[8];
    int nCands;
    PunchConfig cfg;
    PeerParams() : mySlot(0), peerSlot(0), cookie(0), myNat(NAT_UNKNOWN), peerNat(NAT_UNKNOWN), tryLocal(false), nCands(0) { memset(key, 0, sizeof key); }
};

struct PeerStats {
    uint64_t punchSent, punchRecv, dataSent, dataRecv, ctrlRecv, dropAuth, dropReplay, dropShort, dropNoKey, dropTooBig, roams;
    uint64_t pingsSent, pongsRecv;
    uint64_t relayTx, relayRx;   // bytes through the relay (ChannelData framing included): reported to the service for its budget
    uint64_t punchStale;        // authentic punches of another attempt than the one we hold, ignored (a replay, or a restart we have not seen the echo of yet)
};

class Peer {
public:
    enum State {
        IDLE,
        PUNCHING,      // probing
        CONNECTED,     // a confirmed path, data flows
        NEEDS_RELAY,   // timeout without a path: the relay is the only way left
        LOST,          // connected once, then silent for lostMs
        CLOSED         // the other side said goodbye
    };

    Peer();
    // (Re)starts punching with fresh hellos. False when the system random generator failed (nothing was started; the state stays as it was):
    // the caller fails the session with RandomFailureSentence().
    bool Begin(Env* env, const PeerParams& params);
    void AddCandidate(const Candidate& c);                  // e.g. the relay after NEEDS_RELAY; call Restart() afterwards
    bool Restart();                                         // fresh hellos, same params, new timeout (a rebuilt socket, a relay candidate); false: see Begin
    // The relay candidate (our allocation's server, the channel bound to this member) was made or changed: replaces the earlier one, moves a
    // relay path that is in use, and starts probing again when the attempt had run out of paths.
    bool SetRelay(const Addr& server, uint16_t channel);   // false: the random generator failed on the restart (see Begin)
    bool HasRelayCandidate() const;
    bool IsRelayChannel(const Addr& server, uint16_t channel) const;   // true when `channel` on `server` is the relay path bound to this member (what a ChannelData frame must match)
    void Tick();

    // A datagram from the socket (`via` = the relay server it came from, set when the controller unwrapped a ChannelData message
    // and `from` is that server). True when it was an authentic packet of this peer.
    bool OnPacket(const Addr& from, const uint8_t* data, size_t len, bool viaRelay = false);

    // One VLink frame (<= kMaxFrame bytes). False when no path is confirmed or the frame is too big.
    bool SendFrame(const void* data, size_t len);
    // Next received frame, its length; 0 when none.
    int PopFrame(void* buffer, size_t capacity);
    // Tells the other side we leave (best effort, once); the peer then reports CLOSED.
    void SendClose();

    State GetState() const { return state_; }
    bool Connected() const { return state_ == CONNECTED; }
    bool Established() const { return state_ == CONNECTED && myConfirmed_ && peerConfirmed_; }   // both sides confirmed
    bool Stale() const { return state_ == CONNECTED && stale_; }
    Candidate Path() const { return path_; }
    const PeerStats& Stats() const { return stats_; }
    uint64_t ConnectedAtMs() const { return connectedAt_; }
    uint64_t StartedAtMs() const { return startedAt_; }
    // Link quality from the once-a-second ping (CTRL op 3/4, authenticated like all traffic): smoothed round trip, its mean deviation (jitter),
    // and the share of the other side's datagrams that never arrived (from the gaps in its sequence numbers), per mille. -1 = not measured yet.
    // The lockstep's input-delay choice (A7) and the log lines read these.
    int RttMs() const { return rttValid_ ? int(srttUs_ / 1000) : -1; }
    int RttMinMs() const { return rttValid_ ? int(minRttUs_ / 1000) : -1; }
    int JitterMs() const { return rttValid_ ? int(rttVarUs_ / 1000) : -1; }
    int LossPermille() const;
    uint64_t LastRxMs() const { return lastRx_; }
    // The local side was not running for `ms` (a long frame: a scene load; a debugger stop): the peer could not be heard in that time, so it does not count as its silence.
    void CreditLocalStall(uint64_t ms);
    uint32_t Cookie() const { return p_.cookie; }
    int PeerSlot() const { return p_.peerSlot; }
    int RelayChannel() const;
    size_t LearnedCount() const { return learned_.size(); }      // bounded lists of source addresses (tests, logs)
    size_t ConfirmedCount() const { return confirmed_.size(); }

    // Wire helpers, exposed for tests.
    static const size_t kPunchSize = 40;
    static bool PeekFromSlot(const uint8_t* data, size_t len, int* slot);

private:
    friend struct PeerTestAccess;   // tools/net/online_test.cpp reads the keys to forge authentic packets
    struct Learned {
        Addr addr;
        bool relay;
        uint16_t channel;
        uint64_t firstMs;
        uint64_t lastMs;       // the last authentic probe from this source
        bool authentic;        // an AEAD-authenticated packet came from it (confirmed_ only)
    };
    static const size_t kMaxLearned = 8, kMaxConfirmed = 8, kRetiredHellos = 8;
    bool FreshHello(uint8_t out[8]) const;
    void RestartWith(const uint8_t hello[8]);
    bool IsRetired(const uint8_t* hello) const;
    void Retire(const uint8_t* hello);
    void LearnSource(const Addr& from, bool viaRelay, uint16_t channel, uint64_t now);
    void AddConfirmed(const Addr& from, bool viaRelay, uint16_t channel, uint64_t now, bool authentic);
    void DeriveKeys();
    void SendPunch(const Candidate& to, bool ack);
    void SendRaw(const Candidate& to, const uint8_t* pkt, size_t len);
    void ProbeRound();
    bool OnPunch(const Addr& from, const uint8_t* d, size_t n, bool viaRelay);
    bool OnSecure(const Addr& from, const uint8_t* d, size_t n, bool viaRelay);
    bool SendSecure(uint8_t type, const uint8_t* plain, size_t len);
    void Connect();
    Candidate CandidateFor(const Addr& from, bool viaRelay) const;
    void EnterPunching();
    void Enqueue(const uint8_t* data, size_t len);

    Env* env_;
    PeerParams p_;
    State state_;
    uint8_t hello_[8], peerHello_[8];
    bool peerHelloKnown_, myConfirmed_, peerConfirmed_;
    uint8_t punchKey_[32], dataKey_[32];
    bool keysReady_;
    uint32_t sendSeq_;
    // replay window over the sequence numbers of authentic packets
    bool rxAny_;
    uint32_t rxTop_;
    uint64_t rxBits_;
    uint64_t startedAt_, nextProbe_, connectedAt_, lastRx_, lastTx_, confirmedAt_, localDeadline_;
    bool rxSinceCredit_;   // a packet was heard since the last local-stall credit (CreditLocalStall gives one credit per packet heard)
    bool pathChosen_, stale_, closeSent_;
    // measurements
    bool rttValid_;
    uint32_t srttUs_, rttVarUs_, minRttUs_;
    uint64_t nextPing_, rxCount_;
    uint32_t rxFirstSeq_;
    Candidate path_;
    std::vector<Candidate> dynCands_;   // candidates added after Begin (relay)
    std::vector<Learned> learned_;      // source addresses of authentic probes that are not candidates
    std::vector<Learned> confirmed_;    // paths on which our hello came back
    uint8_t retired_[kRetiredHellos][8];   // peer hellos we moved away from: never adopted again (a recorded old punch cannot bring them back)
    size_t nRetired_, retiredNext_;
    std::deque<std::vector<uint8_t> > rx_;
    PeerStats stats_;
};

}  // namespace NpNet

#endif
