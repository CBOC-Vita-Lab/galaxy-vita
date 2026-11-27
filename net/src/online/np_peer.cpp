// Hole punching and the encrypted session with one remote console. See np_peer.h.
#include "np_peer.h"

#include <stdio.h>
#include <stdlib.h>

namespace NpNet {
namespace {
const uint8_t kFlagAck = 1, kFlagConfirmed = 2;
const uint8_t kCtrlKeepAlive = 1, kCtrlClose = 2, kCtrlPing = 3, kCtrlPong = 4;
const uint32_t kPingIntervalMs = 1000;
const size_t kRxQueueLimit = 512;
const uint32_t kLingerProbeMs = 250;
const char kPunchLabel[] = "MSCN punch v1";
const char kDataLabel[] = "MSCN data v1";

void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void MakeNonce(uint8_t nonce[24], int slot, uint32_t seq) {
    memset(nonce, 0, 24);
    nonce[0] = uint8_t(slot);
    Put32(nonce + 20, seq);
}
}  // namespace

Peer::Peer()
    : env_(0), state_(IDLE), peerHelloKnown_(false), myConfirmed_(false), peerConfirmed_(false), keysReady_(false), sendSeq_(0), rxAny_(false),
      rxTop_(0), rxBits_(0), startedAt_(0), nextProbe_(0), connectedAt_(0), lastRx_(0), lastTx_(0), confirmedAt_(0), localDeadline_(0), rxSinceCredit_(false),
      pathChosen_(false), stale_(false), closeSent_(false), rttValid_(false), srttUs_(0), rttVarUs_(0), minRttUs_(0), nextPing_(0), rxCount_(0), rxFirstSeq_(0), nRetired_(0), retiredNext_(0) {
    memset(retired_, 0, sizeof retired_);
    memset(hello_, 0, sizeof hello_);
    memset(peerHello_, 0, sizeof peerHello_);
    memset(punchKey_, 0, sizeof punchKey_);
    memset(dataKey_, 0, sizeof dataKey_);
    memset(&stats_, 0, sizeof stats_);
}

bool Peer::PeekFromSlot(const uint8_t* d, size_t len, int* slot) {
    if (len < 2 || (d[0] != kTypePunch && d[0] != kTypeData && d[0] != kTypeCtrl)) return false;
    *slot = d[1];
    return true;
}

int Peer::LossPermille() const {
    if (!rxAny_ || rxCount_ < 20) return -1;
    const uint64_t expected = uint64_t(rxTop_ - rxFirstSeq_) + 1;
    if (expected <= rxCount_) return 0;
    return int((expected - rxCount_) * 1000u / expected);
}

int Peer::RelayChannel() const { return path_.kind == CAND_RELAY ? path_.channel : 0; }

// A fresh random hello (the first four bytes are never all zero: zero means "no hello" on the wire). False when the system generator failed or
// keeps returning zeros (broken): the loop is bounded and the caller fails the session instead of spinning on a dead generator.
bool Peer::FreshHello(uint8_t out[8]) const {
    for (int tries = 0; tries < 16; ++tries) {
        if (!env_->Random(out, 8)) return false;
        if (out[0] || out[1] || out[2] || out[3]) return true;
    }
    return false;
}

bool Peer::IsRetired(const uint8_t* hello) const {
    for (size_t i = 0; i < nRetired_; ++i)
        if (memcmp(retired_[i], hello, 8) == 0) return true;
    return false;
}

void Peer::Retire(const uint8_t* hello) {
    if (IsRetired(hello)) return;
    memcpy(retired_[retiredNext_], hello, 8);
    retiredNext_ = (retiredNext_ + 1) % kRetiredHellos;
    if (nRetired_ < kRetiredHellos) ++nRetired_;
}

void Peer::DeriveKeys() {
    const uint8_t* lo = p_.mySlot < p_.peerSlot ? hello_ : peerHello_;
    const uint8_t* hi = p_.mySlot < p_.peerSlot ? peerHello_ : hello_;
    uint8_t both[16];
    memcpy(both, lo, 8);
    memcpy(both + 8, hi, 8);
    Kdf(dataKey_, p_.key, kDataLabel, sizeof kDataLabel - 1, both, sizeof both);
    keysReady_ = true;
}

void Peer::EnterPunching() {
    state_ = PUNCHING;
    startedAt_ = env_->Now();
    nextProbe_ = startedAt_;
    myConfirmed_ = peerConfirmed_ = false;
    pathChosen_ = false;
    stale_ = false;
    confirmed_.clear();
    localDeadline_ = 0;
    closeSent_ = false;
}

bool Peer::Begin(Env* env, const PeerParams& params) {
    env_ = env;
    uint8_t fresh[8];
    if (!FreshHello(fresh)) return false;
    p_ = params;
    Kdf(punchKey_, p_.key, kPunchLabel, sizeof kPunchLabel - 1);
    dynCands_.clear();
    learned_.clear();
    rx_.clear();
    RestartWith(fresh);
    return true;
}

bool Peer::Restart() {
    uint8_t fresh[8];
    if (!FreshHello(fresh)) return false;
    RestartWith(fresh);
    return true;
}

void Peer::RestartWith(const uint8_t hello[8]) {
    memcpy(hello_, hello, 8);
    peerHelloKnown_ = false;
    keysReady_ = false;
    memset(peerHello_, 0, sizeof peerHello_);
    sendSeq_ = 0;
    rxAny_ = false;
    rxTop_ = 0;
    rxBits_ = 0;
    rttValid_ = false;
    rxCount_ = 0;
    learned_.clear();
    EnterPunching();
    NPN_LOG("peer %d: punching (cookie %08x, my NAT %s, theirs %s, %d candidate(s)%s)\n", p_.peerSlot, p_.cookie, NatClassName(p_.myNat),
            NatClassName(p_.peerNat), p_.nCands + int(dynCands_.size()), p_.tryLocal ? ", local path allowed" : "");
}

bool Peer::HasRelayCandidate() const {
    for (size_t i = 0; i < dynCands_.size(); ++i)
        if (dynCands_[i].kind == CAND_RELAY) return true;
    return false;
}

bool Peer::SetRelay(const Addr& server, uint16_t channel) {
    bool found = false;
    for (size_t i = 0; i < dynCands_.size(); ++i) {
        if (dynCands_[i].kind != CAND_RELAY) continue;
        dynCands_[i].addr = server;
        dynCands_[i].channel = channel;
        found = true;
    }
    if (!found) dynCands_.push_back(Candidate(server, CAND_RELAY, channel));
    if (path_.kind == CAND_RELAY && state_ == CONNECTED) {
        path_.addr = server;
        path_.channel = channel;
    }
    for (size_t i = 0; i < learned_.size(); ++i)
        if (learned_[i].relay) { learned_[i].addr = server; learned_[i].channel = channel; }
    for (size_t i = 0; i < confirmed_.size(); ++i)
        if (confirmed_[i].relay) { confirmed_[i].addr = server; confirmed_[i].channel = channel; }
    if (state_ == NEEDS_RELAY) return Restart();
    return true;
}

bool Peer::IsRelayChannel(const Addr& server, uint16_t channel) const {
    for (size_t i = 0; i < dynCands_.size(); ++i)
        if (dynCands_[i].kind == CAND_RELAY && dynCands_[i].addr == server && dynCands_[i].channel == channel) return true;
    return false;
}

void Peer::AddCandidate(const Candidate& c) {
    for (size_t i = 0; i < dynCands_.size(); ++i)
        if (dynCands_[i].addr == c.addr && dynCands_[i].kind == c.kind && dynCands_[i].channel == c.channel) return;
    dynCands_.push_back(c);
}

void Peer::SendRaw(const Candidate& to, const uint8_t* pkt, size_t len) {
    if (!env_ || !env_->udp) return;
    if (to.kind == CAND_RELAY) {
        uint8_t wrapped[kMaxDatagram + 12];
        if (len > kMaxDatagram) return;
        wrapped[0] = uint8_t(to.channel >> 8);
        wrapped[1] = uint8_t(to.channel);
        wrapped[2] = uint8_t(len >> 8);
        wrapped[3] = uint8_t(len);
        memcpy(wrapped + 4, pkt, len);
        // ChannelData is padded to a multiple of four (required over TCP, allowed over UDP; every server accepts it); the length field does not count it
        size_t total = len + 4;
        while (total & 3) wrapped[total++] = 0;
        env_->udp->SendTo(to.addr, wrapped, total);
        stats_.relayTx += total;
    } else {
        env_->udp->SendTo(to.addr, pkt, len);
    }
    lastTx_ = env_->Now();
}

void Peer::SendPunch(const Candidate& to, bool ack) {
    uint8_t pkt[kPunchSize];
    pkt[0] = kTypePunch;
    pkt[1] = p_.mySlot;
    pkt[2] = p_.peerSlot;
    pkt[3] = uint8_t((ack ? kFlagAck : 0) | (myConfirmed_ ? kFlagConfirmed : 0));
    memcpy(pkt + 4, hello_, 8);
    if (peerHelloKnown_) memcpy(pkt + 12, peerHello_, 8);
    else memset(pkt + 12, 0, 8);
    Put32(pkt + 20, p_.cookie);
    Mac16(pkt + 24, punchKey_, pkt, 24);
    SendRaw(to, pkt, sizeof pkt);
    ++stats_.punchSent;
}

void Peer::ProbeRound() {
    const int total = p_.nCands + int(dynCands_.size());
    for (int i = 0; i < total; ++i) {
        const Candidate& c = i < p_.nCands ? p_.cands[i] : dynCands_[size_t(i - p_.nCands)];
        if (c.kind == CAND_LOCAL && !p_.tryLocal) continue;
        if (c.addr == p_.myLocal && c.kind != CAND_RELAY) continue;
        SendPunch(c, false);
    }
    for (size_t i = 0; i < learned_.size(); ++i) {
        const Learned& l = learned_[i];
        SendPunch(l.relay ? Candidate(l.addr, CAND_RELAY, l.channel) : Candidate(l.addr, IsPrivateIp(l.addr.ip) ? CAND_LOCAL : CAND_PUBLIC), false);
    }
}

Candidate Peer::CandidateFor(const Addr& from, bool viaRelay) const {
    if (viaRelay) {
        const int total = p_.nCands + int(dynCands_.size());
        for (int i = 0; i < total; ++i) {
            const Candidate& c = i < p_.nCands ? p_.cands[i] : dynCands_[size_t(i - p_.nCands)];
            if (c.kind == CAND_RELAY && c.addr == from) return c;
        }
        return Candidate(from, CAND_RELAY, 0);
    }
    return Candidate(from, IsPrivateIp(from.ip) ? CAND_LOCAL : CAND_PUBLIC);
}

// Source addresses of authentic probes that are not candidates. Bounded: a replayed probe carries any source address the replayer likes, so the
// list must neither grow without limit (every entry is probed every round) nor be fillable to keep the real source out. When full, the entry
// that was heard from longest ago goes, unless it is a confirmed path (the real peer probes every round, so it is always the freshest).
void Peer::LearnSource(const Addr& from, bool viaRelay, uint16_t channel, uint64_t now) {
    const int total = p_.nCands + int(dynCands_.size());
    for (int i = 0; i < total; ++i) {
        const Candidate& c = i < p_.nCands ? p_.cands[i] : dynCands_[size_t(i - p_.nCands)];
        if (c.addr == from && (c.kind == CAND_RELAY) == viaRelay) return;   // already a candidate
    }
    for (size_t i = 0; i < learned_.size(); ++i)
        if (learned_[i].addr == from && learned_[i].relay == viaRelay) {
            learned_[i].lastMs = now;
            return;
        }
    if (learned_.size() >= kMaxLearned) {
        size_t victim = learned_.size();
        for (size_t i = 0; i < learned_.size(); ++i) {
            bool confirmed = false;
            for (size_t j = 0; j < confirmed_.size(); ++j)
                if (confirmed_[j].addr == learned_[i].addr && confirmed_[j].relay == learned_[i].relay) confirmed = true;
            if (confirmed) continue;
            if (victim == learned_.size() || learned_[i].lastMs < learned_[victim].lastMs) victim = i;
        }
        if (victim == learned_.size()) return;   // every entry is a confirmed path
        learned_.erase(learned_.begin() + long(victim));
    }
    Learned l;
    l.addr = from;
    l.relay = viaRelay;
    l.channel = channel;
    l.firstMs = l.lastMs = now;
    l.authentic = false;
    learned_.push_back(l);
    NPN_LOG("peer %d: learned path %s%s\n", p_.peerSlot, AddrLog(from), viaRelay ? " (relay)" : "");
}

// A path on which our hello came back (a probe) or an AEAD packet arrived. Bounded the same way; paths that carried an authenticated packet are
// never pushed out by ones that only carried a probe.
void Peer::AddConfirmed(const Addr& from, bool viaRelay, uint16_t channel, uint64_t now, bool authentic) {
    for (size_t i = 0; i < confirmed_.size(); ++i)
        if (confirmed_[i].addr == from && confirmed_[i].relay == viaRelay) {
            confirmed_[i].lastMs = now;
            if (authentic) confirmed_[i].authentic = true;
            return;
        }
    if (confirmed_.size() >= kMaxConfirmed) {
        size_t victim = confirmed_.size();
        for (size_t i = 0; i < confirmed_.size(); ++i) {
            if (confirmed_[i].authentic) continue;
            if (victim == confirmed_.size() || confirmed_[i].lastMs < confirmed_[victim].lastMs) victim = i;
        }
        if (victim == confirmed_.size()) return;
        confirmed_.erase(confirmed_.begin() + long(victim));
    }
    Learned l;
    l.addr = from;
    l.relay = viaRelay;
    l.channel = channel;
    l.firstMs = l.lastMs = now;
    l.authentic = authentic;
    confirmed_.push_back(l);
}

void Peer::Enqueue(const uint8_t* data, size_t len) {
    if (rx_.size() >= kRxQueueLimit) rx_.pop_front();
    rx_.push_back(std::vector<uint8_t>(data, data + len));
}

bool Peer::OnPacket(const Addr& from, const uint8_t* d, size_t n, bool viaRelay) {
    if (!env_ || state_ == IDLE || n < 2) return false;
    if (viaRelay) stats_.relayRx += n + 4;
    switch (d[0]) {
    case kTypePunch: return OnPunch(from, d, n, viaRelay);
    case kTypeData:
    case kTypeCtrl: return OnSecure(from, d, n, viaRelay);
    default: return false;
    }
}

bool Peer::OnPunch(const Addr& from, const uint8_t* d, size_t n, bool viaRelay) {
    if (n != kPunchSize) { ++stats_.dropShort; return false; }
    if (d[1] != p_.peerSlot || d[2] != p_.mySlot || Get32(d + 20) != p_.cookie) return false;
    uint8_t mac[16];
    Mac16(mac, punchKey_, d, 24);
    if (!MacEqual(mac, d + 24)) { ++stats_.dropAuth; return false; }
    ++stats_.punchRecv;
    const uint64_t now = env_->Now();
    const uint8_t flags = d[3];
    const uint8_t* hello = d + 4;
    const uint8_t* echo = d + 12;
    const Candidate via = CandidateFor(from, viaRelay);

    if (!peerHelloKnown_ || memcmp(peerHello_, hello, 8) != 0) {
        // The other side's hello changed (or is new to us). A punch carries a valid MAC, but the MAC alone proves nothing about freshness: an old
        // packet recorded earlier verifies just as well. A new attempt of the other side is therefore adopted only when
        //   * we hold no hello of it yet (first contact), or the packet echoes OUR current hello (it was made after seeing it: our hello is random
        //     per attempt, so no recording of an older attempt can contain it), and
        //   * the hello is not one we already moved away from (a recording of an earlier attempt made while our hello was the same).
        // Anything else is ignored; a probe is still answered, which is how a restarted peer learns our hello and starts echoing it. This keeps a
        // replayed punch from tearing down a live session, and costs a restarted peer one more round trip.
        const bool proof = !peerHelloKnown_ || memcmp(echo, hello_, 8) == 0;
        if (!proof || IsRetired(hello)) {
            ++stats_.punchStale;
            if (!(flags & kFlagAck)) SendPunch(via, true);
            return true;
        }
        if (peerHelloKnown_) Retire(peerHello_);
        const bool wasUp = state_ == CONNECTED;
        memcpy(peerHello_, hello, 8);
        peerHelloKnown_ = true;
        DeriveKeys();
        rxAny_ = false;
        rxTop_ = 0;
        rxBits_ = 0;
        rxCount_ = 0;
        rttValid_ = false;
        if (wasUp || state_ == LOST || state_ == NEEDS_RELAY || state_ == CLOSED) {
            NPN_LOG("peer %d: it started a new attempt (state was %d): punching again\n", p_.peerSlot, int(state_));
            EnterPunching();
        } else {
            myConfirmed_ = peerConfirmed_ = false;
            confirmed_.clear();
            pathChosen_ = false;
        }
    }
    bool freshConfirm = false;
    if (memcmp(echo, hello_, 8) == 0) {
        freshConfirm = true;
        if (!myConfirmed_) {
            myConfirmed_ = true;
            confirmedAt_ = now;
        }
        AddConfirmed(from, viaRelay, via.channel, now, false);
        if ((flags & kFlagConfirmed)) peerConfirmed_ = true;
    }
    // remember the source: probes go there too (the symmetric-NAT peer's real port is only known from its first probe)
    LearnSource(from, viaRelay, via.channel, now);
    if (state_ == CONNECTED) {
        // A punch never moves the path and never counts as proof of life from anywhere but the path itself: it is authenticated by the room key
        // only, so a replayed copy could carry any source address. The path moves only on packets that passed AEAD (OnSecure: DATA / CTRL).
        if (freshConfirm && from == path_.addr) { lastRx_ = now; rxSinceCredit_ = true; }
    } else if (state_ == NEEDS_RELAY && myConfirmed_) {
        state_ = PUNCHING;   // late success after the timeout
    }
    // answer probes (not answers): the other side may still be waiting for our echo
    if (!(flags & kFlagAck)) {
        SendPunch(via, true);
    }
    if (state_ == PUNCHING) {
        if (myConfirmed_ && !pathChosen_) {
            int best = 9;
            for (size_t i = 0; i < confirmed_.size(); ++i) {
                const int r = confirmed_[i].relay ? int(CAND_RELAY) : (IsPrivateIp(confirmed_[i].addr.ip) ? int(CAND_LOCAL) : int(CAND_PUBLIC));
                if (r < best) best = r;
            }
            // a local path is taken at once; a public one waits localGraceMs for a local one when the peer is on our LAN
            localDeadline_ = (best == int(CAND_LOCAL) || !p_.tryLocal) ? 0 : confirmedAt_ + p_.cfg.localGraceMs;
            if (localDeadline_ == 0) Connect();
        }
    }
    return true;
}

void Peer::Connect() {
    if (state_ == CONNECTED && pathChosen_) return;
    // best confirmed path: local < public < relay; ties go to the one confirmed first
    int bestRank = 9;
    const Learned* best = 0;
    for (size_t i = 0; i < confirmed_.size(); ++i) {
        const Learned& l = confirmed_[i];
        const int r = l.relay ? int(CAND_RELAY) : (IsPrivateIp(l.addr.ip) ? int(CAND_LOCAL) : int(CAND_PUBLIC));
        if (r < bestRank) {
            bestRank = r;
            best = &l;
        }
    }
    if (!best) return;
    path_ = best->relay ? Candidate(best->addr, CAND_RELAY, best->channel) : Candidate(best->addr, IsPrivateIp(best->addr.ip) ? CAND_LOCAL : CAND_PUBLIC);
    pathChosen_ = true;
    state_ = CONNECTED;
    stale_ = false;
    connectedAt_ = env_->Now();
    lastRx_ = connectedAt_;
    rxSinceCredit_ = true;
    nextProbe_ = connectedAt_;
    NPN_LOG("peer %d: connected via %s %s after %u ms\n", p_.peerSlot, path_.kind == CAND_LOCAL ? "local" : (path_.kind == CAND_RELAY ? "relay" : "public"),
            AddrLog(path_.addr), unsigned(connectedAt_ - startedAt_));
}

void Peer::CreditLocalStall(uint64_t ms) {
    if (state_ != CONNECTED || !env_) return;
    // one credit per packet heard: a link whose every tick gap is long and that hears nothing in between is dead, not stalled
    if (!rxSinceCredit_) return;
    rxSinceCredit_ = false;
    const uint64_t now = env_->Now();
    lastRx_ = (lastRx_ + ms > now) ? now : lastRx_ + ms;
}

void Peer::Tick() {
    if (!env_ || state_ == IDLE) return;
    const uint64_t now = env_->Now();
    if (state_ == PUNCHING) {
        if (myConfirmed_ && !pathChosen_ && now >= localDeadline_) Connect();
        if (state_ != PUNCHING) return;
        if (now >= nextProbe_) {
            ProbeRound();
            nextProbe_ = now + p_.cfg.intervalMs;
        }
        const bool symmetric = p_.myNat == NAT_SYMMETRIC || p_.peerNat == NAT_SYMMETRIC;
        const bool hopeless = !PunchPossible(p_.myNat, p_.peerNat) || (p_.cfg.symmetricRelay && symmetric);
        // with a relay candidate in play the attempt gets its full time (the relay needs a round trip or two to confirm)
        const uint32_t limit = (hopeless && !HasRelayCandidate()) ? p_.cfg.hopelessTimeoutMs : p_.cfg.timeoutMs;
        if (!myConfirmed_ && SinceMs(now, startedAt_) >= limit) {
            state_ = NEEDS_RELAY;
            NPN_LOG("peer %d: no direct path after %u ms (my NAT %s, theirs %s): needs relay\n", p_.peerSlot, unsigned(SinceMs(now, startedAt_)), NatClassName(p_.myNat),
                    NatClassName(p_.peerNat));
        }
        return;
    }
    if (state_ != CONNECTED) return;
    // linger: keep probing the chosen path until the other side has confirmed too (or the linger time is over)
    if (!peerConfirmed_ && SinceMs(now, connectedAt_) < p_.cfg.lingerMs && now >= nextProbe_) {
        SendPunch(path_, false);
        nextProbe_ = now + kLingerProbeMs;
    }
    const uint64_t silent = SinceMs(now, lastRx_);
    if (silent >= p_.cfg.lostMs) {
        state_ = LOST;
        NPN_LOG("peer %d: lost (nothing received for %u ms)\n", p_.peerSlot, unsigned(silent));
        return;
    }
    if (silent >= p_.cfg.staleMs) {
        if (!stale_) NPN_LOG("peer %d: path is quiet for %u ms, probing every candidate again\n", p_.peerSlot, unsigned(silent));
        stale_ = true;
        if (now >= nextProbe_) {
            ProbeRound();
            SendPunch(path_, false);
            nextProbe_ = now + p_.cfg.intervalMs;
        }
    }
    if (now >= nextPing_) {
        nextPing_ = now + kPingIntervalMs;
        uint8_t ping[9];
        ping[0] = kCtrlPing;
        const uint64_t us = now * 1000u;
        for (int i = 0; i < 8; ++i) ping[1 + i] = uint8_t(us >> (56 - 8 * i));
        if (SendSecure(kTypeCtrl, ping, sizeof ping)) ++stats_.pingsSent;
    } else if (SinceMs(now, lastTx_) >= p_.cfg.keepaliveMs) {
        const uint8_t op = kCtrlKeepAlive;
        SendSecure(kTypeCtrl, &op, 1);
    }
}

bool Peer::SendSecure(uint8_t type, const uint8_t* plain, size_t len) {
    if (!keysReady_ || state_ != CONNECTED) return false;
    if (len > kMaxFrame) { ++stats_.dropTooBig; return false; }
    uint8_t pkt[kMaxDatagram];
    pkt[0] = type;
    pkt[1] = p_.mySlot;
    const uint32_t seq = ++sendSeq_;
    Put32(pkt + 2, seq);
    uint8_t nonce[24];
    MakeNonce(nonce, p_.mySlot, seq);
    AeadSeal(pkt + kDataHeader, pkt + kDataHeader + len, dataKey_, nonce, pkt, kDataHeader, plain, len);
    SendRaw(path_, pkt, kDataOverhead + len);
    if (type == kTypeData) ++stats_.dataSent;
    return true;
}

bool Peer::SendFrame(const void* data, size_t len) {
    if (state_ != CONNECTED) return false;
    return SendSecure(kTypeData, static_cast<const uint8_t*>(data), len);
}

void Peer::SendClose() {
    if (closeSent_ || !keysReady_ || state_ != CONNECTED) return;
    closeSent_ = true;
    const uint8_t op = kCtrlClose;
    for (int i = 0; i < 2; ++i) SendSecure(kTypeCtrl, &op, 1);   // twice: best effort over UDP
}

bool Peer::OnSecure(const Addr& from, const uint8_t* d, size_t n, bool viaRelay) {
    if (n < kDataOverhead) { ++stats_.dropShort; return false; }
    // `plain` below holds one datagram: a longer packet (the receive buffer is bigger than a datagram only by the relay header) is refused before
    // anything is decrypted, whoever sent it. The wire never carries more than kMaxDatagram (SendSecure), so this costs nothing legitimate.
    if (n > kMaxDatagram) { ++stats_.dropTooBig; return false; }
    if (d[1] != p_.peerSlot) return false;
    if (!keysReady_ || (state_ != CONNECTED && state_ != PUNCHING && state_ != NEEDS_RELAY)) { ++stats_.dropNoKey; return false; }
    const uint32_t seq = Get32(d + 2);
    // replay window (64 packets), checked before the decryption and committed after it
    if (rxAny_) {
        if (seq <= rxTop_) {
            const uint32_t back = rxTop_ - seq;
            if (back >= 64 || (rxBits_ >> back & 1)) { ++stats_.dropReplay; return false; }
        }
    }
    const size_t len = n - kDataOverhead;
    uint8_t plain[kMaxDatagram];
    uint8_t nonce[24];
    MakeNonce(nonce, p_.peerSlot, seq);
    if (!AeadOpen(plain, dataKey_, nonce, d, kDataHeader, d + kDataHeader, len, d + kDataHeader + len)) { ++stats_.dropAuth; return false; }
    if (!rxAny_) rxFirstSeq_ = seq;
    ++rxCount_;
    const bool newest = !rxAny_ || seq > rxTop_;
    if (newest) {
        const uint32_t shift = rxAny_ ? seq - rxTop_ : 0;
        rxBits_ = (shift >= 64 || !rxAny_) ? 0 : rxBits_ << shift;
        rxBits_ |= 1;
        rxTop_ = seq;
        rxAny_ = true;
    } else {
        rxBits_ |= uint64_t(1) << (rxTop_ - seq);
    }
    const uint64_t now = env_->Now();
    const Candidate via = CandidateFor(from, viaRelay);
    if (state_ == PUNCHING || state_ == NEEDS_RELAY) {
        // an authentic packet under the key derived from OUR hello: the other side has it and the path works both ways
        if (!myConfirmed_) {
            myConfirmed_ = true;
            confirmedAt_ = now;
        }
        peerConfirmed_ = true;
        AddConfirmed(from, viaRelay, via.channel, now, true);
        state_ = PUNCHING;
        Connect();
    } else if (state_ == CONNECTED) {
        AddConfirmed(from, viaRelay, via.channel, now, true);
    }
    lastRx_ = now;
    rxSinceCredit_ = true;
    stale_ = false;
    if (state_ == CONNECTED) {
        // the path follows authentic traffic only, and only the newest of it (an older packet of the window, delayed or re-sent from elsewhere,
        // must not move it)
        if (newest && !viaRelay && (path_.kind == CAND_RELAY || path_.addr != from)) {
            NPN_LOG("peer %d: path moved to %s\n", p_.peerSlot, AddrLog(from));
            path_ = via;
            ++stats_.roams;
        }
    }
    if (d[0] == kTypeData) {
        ++stats_.dataRecv;
        Enqueue(plain, len);
    } else {
        ++stats_.ctrlRecv;
        if (len >= 1 && plain[0] == kCtrlClose) {
            state_ = CLOSED;
            NPN_LOG("peer %d: closed the link\n", p_.peerSlot);
        } else if (len == 9 && plain[0] == kCtrlPing && state_ == CONNECTED) {
            uint8_t pong[9];
            memcpy(pong, plain, 9);
            pong[0] = kCtrlPong;
            SendSecure(kTypeCtrl, pong, sizeof pong);
        } else if (len == 9 && plain[0] == kCtrlPong) {
            uint64_t us = 0;
            for (int i = 0; i < 8; ++i) us = (us << 8) | plain[1 + i];
            const uint64_t nowUs = now * 1000u;
            if (nowUs >= us && nowUs - us < 60u * 1000000u) {
                const uint32_t rtt = uint32_t(nowUs - us);
                ++stats_.pongsRecv;
                if (!rttValid_) {
                    rttValid_ = true;
                    srttUs_ = rtt;
                    rttVarUs_ = rtt / 2;
                    minRttUs_ = rtt;
                } else {
                    const uint32_t dev = rtt > srttUs_ ? rtt - srttUs_ : srttUs_ - rtt;
                    rttVarUs_ = (rttVarUs_ * 3 + dev) / 4;
                    srttUs_ = (srttUs_ * 7 + rtt) / 8;
                    if (rtt < minRttUs_) minRttUs_ = rtt;
                }
            }
        }
    }
    return true;
}

int Peer::PopFrame(void* buffer, size_t capacity) {
    if (rx_.empty()) return 0;
    const std::vector<uint8_t>& f = rx_.front();
    const size_t n = f.size() < capacity ? f.size() : capacity;
    if (n) memcpy(buffer, &f[0], n);
    rx_.pop_front();
    return int(n);   // frames are never empty (a VLink frame has a 24-byte header)
}

}  // namespace NpNet
