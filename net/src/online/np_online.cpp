// OnlineNet: see np_online.h.
#include "np_online.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif

namespace NpNet {

const char kDefaultStunServers[] = "stun.cloudflare.com:3478,stun.cloudflare.com:53";

namespace {
uint32_t Mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
uint32_t CandSig(const SigCandidates& c) {
    uint32_t h = 2166136261u;
    h = Mix(h, c.pub.ip);
    h = Mix(h, c.pub.port);
    h = Mix(h, c.pub2.ip);
    h = Mix(h, c.pub2.port);
    h = Mix(h, c.local.ip);
    h = Mix(h, c.local.port);
    h = Mix(h, c.nat);
    h = Mix(h, c.gen);
    return h ? h : 1;
}
void CopyStr(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = 0;
}
}  // namespace

// ---- DNS on a worker thread ---------------------------------------------------------------------------------------------------
// The thread is detached and owns nothing but this block: state 0 running, 1 finished (the owner takes the result and deletes it), 2 abandoned
// (the owner left or gave up: the thread deletes it when its lookup returns). A lookup that hangs can therefore never block the game.
struct OnlineNet::Resolver {
    pthread_t thread;
    volatile int done;
    int n;
    char host[kMaxStunServers][96];
    uint16_t port[kMaxStunServers];
    uint32_t ips[kMaxStunServers][4];
    int nips[kMaxStunServers];
    Resolver() : done(0), n(0) {
        memset(ips, 0, sizeof ips);
        memset(nips, 0, sizeof nips);
    }
};

// DNS of the relay's host names, same pattern as the Resolver above (the credentials carry names like turn.cloudflare.com).
struct OnlineNet::TurnResolve {
    pthread_t thread;
    volatile int done;
    int n;
    char host[4][96];
    uint16_t port[4];
    uint32_t ip[4];
    TurnResolve() : done(0), n(0) { memset(ip, 0, sizeof ip); memset(port, 0, sizeof port); memset(host, 0, sizeof host); }
};

namespace {
// "turn:host:port?transport=udp" -> host, port (3478 by default)
bool ParseTurnUrl(const char* url, char* host, size_t cap, uint16_t* port) {
    if (strncmp(url, "turn:", 5) != 0) return false;
    const char* p = url + 5;
    const char* end = p;
    while (*end && *end != '?') ++end;
    const char* colon = 0;
    for (const char* q = p; q < end; ++q)
        if (*q == ':') colon = q;
    const char* hostEnd = colon ? colon : end;
    if (hostEnd == p || size_t(hostEnd - p) + 1 > cap) return false;
    memcpy(host, p, size_t(hostEnd - p));
    host[hostEnd - p] = 0;
    *port = colon ? uint16_t(atoi(colon + 1)) : 3478;
    return *port != 0;
}
}  // namespace

OnlineNet::OnlineNet()
    : udp_(0), ownUdp_(false), signal_(0), phase_(OFF), mySlot_(-1), myVip_(0), gen_(0), host_(false), started_(false), published_(false),
      stunStarted_(false), rebuild_(false), refreshing_(false), startedSent_(false), closedHandled_(false), signalLost_(false), memberGone_(false), keyWaitSince_(0), lastPublishAt_(0), stagePhase_(OFF), stageAt_(0), rebuildDeadline_(0), nextStats_(0), roomVersion_(0), startedAt_(0), lastTick_(0),
      nextSignalPoll_(0), resolver_(0), relayStage_(RL_OFF), relayWanted_(false), rotating_(false), relayEnabled_(false), skipDirect_(false), swapPending_(false), credRetries_(0), relayStageAt_(0),
      credsAt_(0), usageSent_(0), swapAt_(0), rotateRetryAt_(0), haveCreds_(false), turnResolve_(0), nRelayServers_(0), relayGenSeen_(0), relayWaitDeadline_(0), nStunAddrs_(0) {
    relayWhy_[0] = relayError_[0] = 0;
    memset(relayKey_, 0, sizeof relayKey_);
    memset(reportedOk_, 0, sizeof reportedOk_);
    memset(reportedFail_, 0, sizeof reportedFail_);
    memset(memberSeen_, 0, sizeof memberSeen_);
    warning_[0] = endReason_[0] = 0;
    memberGoneWhy_[0] = 0;
    hostSlot_ = 0;
    hostEpoch_ = 0;
    migrateDeadline_ = 0;
    migrateWhy_[0] = 0;
    prevHostSlot_ = lostHost_ = -1;
    memset(peerSig_, 0, sizeof peerSig_);
    for (int i = 0; i < kMaxSlots; ++i) peerMapped_[i] = Addr();   // Galaxy (netx)
    memset(peerBegun_, 0, sizeof peerBegun_);
    memset(wasConnected_, 0, sizeof wasConnected_);
    memset(lostReported_, 0, sizeof lostReported_);
    invite_[0] = name_[0] = error_[0] = 0;
}

OnlineNet::~OnlineNet() { Stop(); }

void OnlineNet::Fail(const char* why) {
    CopyStr(error_, sizeof error_, why);
    phase_ = FAILED;
    NPN_LOG("failed: %s\n", why);
}

bool OnlineNet::Start(const OnlineConfig& config, const char* invite, const char* playerName) {
    if (started_) Stop();
    cfg_ = config;
    env_ = cfg_.env;
    relayEnabled_ = cfg_.relayEnabled || cfg_.relayOnly;
    AbandonTurnResolve();
    turn_.Reset();
    relayStage_ = RL_OFF;
    relayWanted_ = rotating_ = haveCreds_ = skipDirect_ = swapPending_ = false;
    rotateRetryAt_ = 0;
    credRetries_ = 0;
    usageSent_ = 0;
    relayWhy_[0] = relayError_[0] = 0;
    errorCode_[0] = 0;   // Galaxy (fix3)
    nRelayServers_ = 0;
    relayGenSeen_ = 0;
    relayWaitDeadline_ = 0;
    memset(relayKey_, 0, sizeof relayKey_);
    if (!env_.now) { env_.now = SteadyNowMs; env_.nowCtx = 0; }
    if (!env_.random) { env_.random = SystemRandom; env_.randomCtx = 0; }
    udp_ = cfg_.udp;
    ownUdp_ = true;
    if (!udp_) udp_ = CreatePlatformUdp();
    cfg_.udp = 0;
    if (cfg_.fault.Any()) {
        NPN_LOG("fault injection on: drop %.1f%% dup %.1f%% delay %u ms jitter %u ms\n", cfg_.fault.dropPct, cfg_.fault.dupPct, unsigned(cfg_.fault.delayMs),
                unsigned(cfg_.fault.jitterMs));
        udp_ = CreateFaultUdp(udp_, cfg_.fault, env_.now, env_.nowCtx, 0x5EEDu ^ uint32_t(env_.Now()));
    }
    env_.udp = udp_;
    signal_ = cfg_.signal;
    cfg_.signal = 0;
    if (signal_) {
        signal_->SetClock(env_.now, env_.nowCtx);
        signal_->SetRelayOnly(cfg_.relayOnly);
    }
    refreshing_ = startedSent_ = closedHandled_ = signalLost_ = memberGone_ = false;
    memberGoneWhy_[0] = 0;
    hostSlot_ = 0;
    hostEpoch_ = 0;
    migrateDeadline_ = 0;
    migrateWhy_[0] = 0;
    prevHostSlot_ = lostHost_ = -1;
    keyWaitSince_ = 0;
    memset(memberSeen_, 0, sizeof memberSeen_);
    error_[0] = warning_[0] = endReason_[0] = 0;
    memset(reportedOk_, 0, sizeof reportedOk_);
    memset(reportedFail_, 0, sizeof reportedFail_);
    stagePhase_ = OFF;
    stageAt_ = env_.Now();
    rebuildDeadline_ = 0;
    nextStats_ = env_.Now() + cfg_.statsLogMs;
    CopyStr(name_, sizeof name_, playerName ? playerName : "");
    host_ = invite == 0 || *invite == 0;
    CopyStr(invite_, sizeof invite_, host_ ? "" : invite);
    started_ = true;
    published_ = stunStarted_ = rebuild_ = false;
    gen_ = 1;
    mySlot_ = -1;
    myVip_ = 0;
    roomVersion_ = 0;
    memset(peerBegun_, 0, sizeof peerBegun_);
    memset(wasConnected_, 0, sizeof wasConnected_);
    memset(lostReported_, 0, sizeof lostReported_);
    startedAt_ = env_.Now();
    if (!signal_) {
        Fail("Online play is not set up: no online service address or sign-in.");
        return false;
    }
    {   // a dead random generator means predictable keys, hellos and STUN ids: say so now, not after the room exists
        uint8_t probe[8];
        if (!env_.Random(probe, sizeof probe)) {
            Fail(RandomFailureSentence());
            return false;
        }
    }
    if (!udp_->Open(cfg_.port)) {
        Fail(udp_->Error()[0] ? udp_->Error() : "Cannot open the network socket. Is Wi-Fi connected?");
        return false;
    }
    if (udp_->LocalPort() != cfg_.port) NPN_LOG("port %u was busy: using %u\n", unsigned(cfg_.port), unsigned(udp_->LocalPort()));
    // STUN servers: numeric ones at once, names on a worker thread (DNS blocks)
    nStunAddrs_ = 0;
    const char* list = cfg_.stun[0] ? cfg_.stun : kDefaultStunServers;
    if (cfg_.relayOnly) {
        // privacy mode: no STUN either; the only address that ever leaves this console is the relayed one
        NPN_LOG("relay-only mode: no STUN, no direct paths\n");
    } else if (cfg_.nStunAddrs > 0) {
        for (int i = 0; i < cfg_.nStunAddrs && i < kMaxStunServers; ++i) stunAddrs_[nStunAddrs_++] = cfg_.stunAddrs[i];
    } else {
        Resolver* r = new Resolver;
        char copy[256];
        CopyStr(copy, sizeof copy, list);
        bool needThread = false;
        Addr direct[kMaxStunServers];
        bool isDirect[kMaxStunServers];
        int n = 0;
        for (char* item = strtok(copy, ","); item && n < kMaxStunServers; item = strtok(0, ",")) {
            Addr a;
            if (ParseAddr(item, 3478, &a)) {
                direct[n] = a;
                isDirect[n] = true;
            } else {
                isDirect[n] = false;
                char* colon = strrchr(item, ':');
                r->port[n] = colon ? uint16_t(atoi(colon + 1)) : 3478;
                if (colon) *colon = 0;
                CopyStr(r->host[n], sizeof r->host[n], item);
                needThread = true;
            }
            ++n;
        }
        if (!needThread) {
            for (int i = 0; i < n; ++i) stunAddrs_[nStunAddrs_++] = direct[i];
            delete r;
        } else {
            // keep the order: the thread resolves the names, direct entries are merged afterwards by index
            r->n = n;
            for (int i = 0; i < n; ++i) {
                if (isDirect[i]) {
                    r->ips[i][0] = direct[i].ip;
                    r->nips[i] = -1 - int(direct[i].port);   // marker: already numeric
                    r->host[i][0] = 0;
                    r->port[i] = direct[i].port;
                }
            }
            resolver_ = r;
            // numeric entries must not be looked up: Run skips empty names
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 128 * 1024);   // getaddrinfo / sceNetResolver need little; the Vita's default is small
            const int rc = pthread_create(&r->thread, &attr, [](void* arg) -> void* {
                    Resolver* rr = static_cast<Resolver*>(arg);
                    for (int i = 0; i < rr->n; ++i)
                        if (rr->host[i][0]) rr->nips[i] = ResolveIpv4(rr->host[i], rr->ips[i], 4);
                    int expected = 0;
                    if (!__atomic_compare_exchange_n(&rr->done, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete rr;   // abandoned meanwhile
                    return 0;
                }, r);
            if (rc == 0) pthread_detach(r->thread);
            pthread_attr_destroy(&attr);
            if (rc != 0) {
                delete r;
                resolver_ = 0;
                Fail("cannot start the resolver thread");
                return false;
            }
        }
    }
    if (host_) signal_->Host(name_);
    else signal_->Join(invite_, name_);
    phase_ = cfg_.relayOnly ? ROOM : (resolver_ ? RESOLVING : STUN);
    if (!resolver_ && !cfg_.relayOnly) BeginStun();
    // (the invite code is not logged: a saved bug report would carry a room code; mp_conn_online logs it as code=, which the report scrubber hides)
    NPN_LOG("online session started (%s), local port %u, %s topology\n", host_ ? "hosting" : "joining a room", unsigned(cfg_.port),
            cfg_.topology == TOPOLOGY_MESH ? "mesh" : "star");
    return true;
}

void OnlineNet::BeginStun() {
    stunStarted_ = true;
    stun_.Start(&env_, stunAddrs_, nStunAddrs_, cfg_.stunTiming);
    if (stun_.GetState() == StunClient::FAILED) NPN_LOG("no STUN server to ask\n");
}

void OnlineNet::AbandonResolver() {
    if (!resolver_) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&resolver_->done, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { /* the thread deletes it */ }
    else delete resolver_;   // it finished in the meantime
    resolver_ = 0;
}

void OnlineNet::CheckResolver() {
    if (!resolver_ || __atomic_load_n(&resolver_->done, __ATOMIC_ACQUIRE) != 1) return;
    Resolver* r = resolver_;
    resolver_ = 0;
    nStunAddrs_ = 0;
    // The second entry for a host that resolved to two addresses uses the second address (a distinct destination IP for the
    // symmetric-NAT test); entries of one host otherwise share the first.
    for (int i = 0; i < r->n && nStunAddrs_ < kMaxStunServers; ++i) {
        if (r->nips[i] < 0) {
            stunAddrs_[nStunAddrs_++] = Addr(r->ips[i][0], r->port[i]);
            continue;
        }
        if (r->nips[i] == 0) { NPN_LOG("cannot resolve %s\n", r->host[i]); continue; }
        int use = 0;
        for (int j = 0; j < i; ++j)
            if (strcmp(r->host[j], r->host[i]) == 0 && r->nips[i] > 1) use = 1;
        stunAddrs_[nStunAddrs_++] = Addr(r->ips[i][use], r->port[i]);
    }
    delete r;
    if (phase_ == RESOLVING) phase_ = STUN;
    BeginStun();
}

void OnlineNet::CloseSocket() {
    if (udp_) udp_->Close();
}

void OnlineNet::Stop() {
    if (!started_) {
        delete signal_;
        signal_ = 0;
        return;
    }
    AbandonResolver();
    AbandonTurnResolve();
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s]) peers_[s].SendClose();
    ReleaseRelay();
    if (signal_) signal_->Leave();
    CloseSocket();
    delete signal_;
    signal_ = 0;
    if (ownUdp_) delete udp_;
    udp_ = 0;
    started_ = false;
    phase_ = OFF;
    NPN_LOG("online session stopped\n");
}

uint32_t OnlineNet::MyVip() const { return myVip_; }

void OnlineNet::PumpSocket() {
    if (!udp_ || !udp_->IsOpen()) return;
    for (int budget = 0; budget < cfg_.maxPacketsPerPump; ++budget) {
        Addr from;
        const int n = udp_->RecvFrom(&from, rxBuf_, sizeof rxBuf_);
        if (n <= 0) break;
        const PacketClass c = Classify(rxBuf_, size_t(n));
        if (c == PKT_STUN) {
            const uint8_t* inner = 0;
            size_t innerLen = 0;
            Addr peerRelay;
            const int t = turn_.HandlePacket(from, rxBuf_, size_t(n), &inner, &innerLen, &peerRelay);
            if (t == 0) {
                stun_.HandlePacket(from, rxBuf_, size_t(n));
            } else if (t == 2) {
                // a Data indication: a member's datagram that arrived before its channel was bound
                int slot = -1;
                if (innerLen <= kMaxDatagram && Peer::PeekFromSlot(inner, innerLen, &slot) && slot >= 0 && slot < kMaxSlots && slot != mySlot_ && peerBegun_[slot])
                    peers_[slot].OnPacket(from, inner, innerLen, true);
            }
        } else if (c == PKT_PUNCH || c == PKT_DATA || c == PKT_CTRL) {
            const int slot = rxBuf_[1];
            if (slot < kMaxSlots && slot != mySlot_) peers_[slot].OnPacket(from, rxBuf_, size_t(n), false);
        } else if (c == PKT_CHANNEL && n >= 4 + 2) {
            const uint16_t chan = uint16_t(rxBuf_[0] << 8 | rxBuf_[1]);
            const size_t len = size_t(rxBuf_[2] << 8 | rxBuf_[3]);
            if (len + 4 > size_t(n) || len < 2 || len > kMaxDatagram) continue;   // the length field must fit the datagram we got, and no datagram of ours is larger than kMaxDatagram (the peer's decrypt buffer)
            const uint8_t* inner = rxBuf_ + 4;
            // the inner packet names its sender; the channel must be the one our allocation bound to that member (checked below: a frame on any
            // other channel, or from anything but the relay server, is dropped)
            int slot = -1;
            turn_.CountChannelRx();
            if (Peer::PeekFromSlot(inner, len, &slot) && slot >= 0 && slot < kMaxSlots && slot != mySlot_ && peerBegun_[slot] && peers_[slot].IsRelayChannel(from, chan))
                peers_[slot].OnPacket(from, inner, len, true);
        }
    }
}

// Galaxy (netx) ------------------------------------------------------------------------------------------------------------------
bool OnlineNet::LocalEndpoint(uint32_t* ip, uint16_t* port) const {
    if (!udp_ || !udp_->IsOpen()) return false;
    if (ip) *ip = udp_->LocalIp();
    if (port) *port = udp_->LocalPort();
    return true;
}

void OnlineNet::SetMappedCandidate(const Addr& a) {
    if (a == mapped_) return;
    mapped_ = a;
    NPN_LOG("router-mapped candidate %s\n", a.IsZero() ? "withdrawn" : AddrLog(a));
    if (published_ && !mine_.relayOnly && signal_ && signal_->GetStatus() == Signal::IN_ROOM) {
        mine_.mapped = a;
        signal_->PublishCandidates(mine_);
        lastPublishAt_ = env_.Now();
    }
}

void OnlineNet::PublishIfReady() {
    if (published_ || !signal_ || signal_->GetStatus() != Signal::IN_ROOM) return;
    if (cfg_.relayOnly) {
        // nothing but the relayed address is ever published, and only once there is one
        if (relayStage_ != RL_READY || !turn_.Ready()) return;
        mine_ = SigCandidates();
        mine_.valid = true;
        mine_.relayOnly = true;
        mine_.gen = gen_;
        mine_.relay = turn_.Relayed();
        mine_.nat = NAT_UNKNOWN;
        relayGenSeen_ = turn_.Generation();
        NPN_LOG("candidates: relay %s only\n", AddrLog(mine_.relay));
        signal_->PublishCandidates(mine_);
        published_ = true;
        lastPublishAt_ = env_.Now();
        return;
    }
    if (!stunStarted_ || !stun_.Done()) return;
    mine_ = SigCandidates();
    mine_.valid = true;
    if (relayStage_ == RL_READY && turn_.Ready()) {
        mine_.relay = turn_.Relayed();
        relayGenSeen_ = turn_.Generation();
    }
    mine_.pub = stun_.FirstMapped();
    {
        int seen = 0;
        for (int i = 0; i < kMaxStunServers; ++i) {
            const Addr m = stun_.Mapped(i);
            if (m.IsZero()) continue;
            if (++seen == 2) { mine_.pub2 = m; break; }
        }
    }
    mine_.local = Addr(udp_->LocalIp(), udp_->LocalPort());
    mine_.nat = uint8_t(stun_.Classify(mine_.local.ip));
    mine_.gen = gen_;
    mine_.mapped = mapped_;   // Galaxy (netx): UPnP
    if (stun_.GetState() == StunClient::FAILED) {
        NPN_LOG("no STUN answer: only same-network paths are possible\n");
        if (!warning_[0]) CopyStr(warning_, sizeof warning_, "The internet address could not be found: only players on the same network can connect.");
    }
    NPN_LOG("candidates: public %s (second server: %s), local %s, NAT %s%s\n", AddrLog(mine_.pub), AddrLog(mine_.pub2), AddrLog(mine_.local), NatClassName(mine_.nat),
            (stun_.Answered() >= 2 && !stun_.ServersDistinct()) ? " (both STUN answers from one IP: only the destination port was varied)" : "");
    signal_->PublishCandidates(mine_);
    published_ = true;
    lastPublishAt_ = env_.Now();
}

// A NAT mapping lives about 30 s without traffic and the service marks an endpoint older than that stale: while nobody is connected (the host
// waiting for a friend, a punch that does not come), STUN runs again every refreshMs and the endpoint is posted again; the same
// moment a member appears. An unchanged address keeps its generation (the partner does not restart its punching), a new one bumps it.
void OnlineNet::RefreshCandidates(uint64_t now) {
    if (!published_ || rebuild_ || !signal_ || signal_->GetStatus() != Signal::IN_ROOM || nStunAddrs_ == 0) return;
    if (refreshing_) {
        if (!stun_.Done()) return;
        refreshing_ = false;
        const Addr pub = stun_.FirstMapped();
        if (!pub.IsZero()) {
            if (pub != mine_.pub) {
                NPN_LOG("public address changed: %s -> %s\n", AddrLog(mine_.pub), AddrLog(pub));
                ++gen_;
                mine_.gen = gen_;
            }
            mine_.pub = pub;
            mine_.pub2 = Addr();
            int seen = 0;
            for (int i = 0; i < kMaxStunServers; ++i) {
                const Addr m = stun_.Mapped(i);
                if (!m.IsZero() && ++seen == 2) { mine_.pub2 = m; break; }
            }
            mine_.nat = uint8_t(stun_.Classify(mine_.local.ip));
        }
        signal_->PublishCandidates(mine_);
        lastPublishAt_ = now;
        return;
    }
    bool anyConnected = false, joined = false;
    const SigRoom& room = signal_->Room();
    for (int s = 0; s < kMaxSlots; ++s) {
        if (!TopologyPeer(s, mySlot_)) continue;   // Galaxy: only the members this console makes a path to
        const bool present = room.members[s].present;
        if (present && !memberSeen_[s]) joined = true;
        memberSeen_[s] = present;
        if (peerBegun_[s] && peers_[s].GetState() == Peer::CONNECTED) anyConnected = true;
    }
    if (anyConnected) {
        // Galaxy (rooms of up to 4): a member joins while others are connected. No new STUN (a new public port would bump the generation and make the
        // connected members punch afresh); the same candidates are posted again so the service's copy is fresh for the newcomer.
        if (joined) {
            signal_->PublishCandidates(mine_);
            lastPublishAt_ = now;
            NPN_LOG("a member joined: candidates posted again (generation %u)\n", unsigned(mine_.gen));
        }
        return;
    }
    // Galaxy (service review, request budget): alone in the room (a host waiting for a friend), the periodic refresh is not needed: a member
    // who joins triggers a fresh STUN and post (`joined` above), which the held room poll reports at once. 180 requests an hour saved per
    // waiting host (each post also woke its own held poll).
    bool anyOther = false;
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && room.members[s].present) anyOther = true;
    if (!joined && !anyOther) return;
    if (joined || SinceMs(now, lastPublishAt_) >= cfg_.refreshMs) {
        stun_.Start(&env_, stunAddrs_, nStunAddrs_, cfg_.stunTiming);
        refreshing_ = true;
    }
}

void OnlineNet::SyncMembers() {
    if (!signal_ || signal_->GetStatus() != Signal::IN_ROOM) return;
    const SigRoom& room = signal_->Room();
    mySlot_ = room.mySlot;
    if (mySlot_ >= 0 && mySlot_ < kMaxSlots) myVip_ = room.members[mySlot_].vip;
    CopyStr(invite_, sizeof invite_, room.invite);
    {   // Galaxy host migration: the star re-centres on the room's host (the loop below tears down the path to a former host, begins new ones)
        const int host = HostSlot();
        if (host != hostSlot_) {
            NPN_LOG("the host moved: member %d -> member %d (epoch %u)%s\n", hostSlot_, host, unsigned(room.hostEpoch),
                    host == mySlot_ ? ": this console is the host now" : "");
            prevHostSlot_ = hostSlot_;
            hostSlot_ = host;
        }
        hostEpoch_ = room.hostEpoch;
    }
    if (!published_) return;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s == mySlot_) continue;
        const SigMember& m = room.members[s];
        if (!m.present || !TopologyPeer(s, mySlot_)) {   // Galaxy: a member this console does not peer with (star) is treated like an absent one
            if (peerBegun_[s]) {
                NPN_LOG("member %d left the room\n", s);
                if (wasConnected_[s] && !lostReported_[s]) {
                    snprintf(memberGoneWhy_, sizeof memberGoneWhy_, "member %d left the room", s);
                    ReportPeerLost(s, memberGoneWhy_);
                    memberGone_ = true;
                }
                peers_[s].SendClose();
                peerBegun_[s] = false;
            }
            continue;
        }
        if (!m.cands.valid) continue;
        const uint32_t sig = CandSig(m.cands);
        if (peerBegun_[s] && peerSig_[s] == sig) {
            // Galaxy (netx): a member's UPnP-mapped address that came after we began: one more public candidate (no restart of a
            // connected path; an attempt that ran out of paths probes again)
            if (!skipDirect_ && !m.cands.mapped.IsZero() && m.cands.mapped != peerMapped_[s] && m.cands.mapped != m.cands.pub) {
                peerMapped_[s] = m.cands.mapped;
                peers_[s].AddCandidate(Candidate(m.cands.mapped, CAND_PUBLIC));
                NPN_LOG("member %d: router-mapped candidate %s added\n", s, AddrLog(m.cands.mapped));
                if (peers_[s].GetState() == Peer::NEEDS_RELAY && !peers_[s].Restart()) {
                    Fail(RandomFailureSentence());
                    return;
                }
            }
            continue;
        }
        // the service names the moment both sides start (they know each other's addresses from then on); Galaxy: per pair (members[]), else the room's
        const uint64_t punchAt = m.punchAtMs ? m.punchAtMs : room.punchAtMs;
        if (punchAt && env_.Now() < punchAt) continue;
        // No key, no peer: an all-zero key would be a key everyone knows (every packet of the match forgeable). The key comes with the room view
        // that shows the partner; if it does not arrive within a few seconds the service is broken and the room fails with a sentence.
        {
            bool keyOk = false;
            for (int i = 0; i < 32; ++i)
                if (room.key[i]) keyOk = true;
            if (!keyOk) {
                if (!keyWaitSince_) keyWaitSince_ = env_.Now();
                else if (SinceMs(env_.Now(), keyWaitSince_) >= 5000) {
                    Fail("The online service did not send the key for this match, so a secure link cannot be made. Try again.");
                    return;
                }
                continue;
            }
            keyWaitSince_ = 0;
        }
        PeerParams p;
        p.mySlot = uint8_t(mySlot_);
        p.peerSlot = uint8_t(s);
        memcpy(p.key, room.key, 32);
        p.cookie = room.cookie;
        p.myNat = mine_.nat;
        p.peerNat = m.cands.nat;
        p.myLocal = mine_.local;
        p.tryLocal = cfg_.forceTryLocal || mine_.pub.IsZero() || m.cands.pub.IsZero() || mine_.pub.ip == m.cands.pub.ip;
        p.cfg = cfg_.punch;
        bool skipDirect = cfg_.relayOnly;
        const bool steer = m.steerRelay || room.steerRelay;   // Galaxy: the pair's own advice (members[]), else the room's
        const char* steerReason = m.steerRelay ? m.steerReason : room.steerReason;
        if (steer) {
            // The service says the relay: "nat" with a pair that can punch (one symmetric, one cone) is still probed in parallel (it often works and
            // costs no relay traffic); relay-only players and pairs that failed before skip the probes.
            skipDirect = skipDirect || (cfg_.honorSteer && relayEnabled_ && (strcmp(steerReason, "nat") != 0 || !PunchPossible(mine_.nat, m.cands.nat)));
            NPN_LOG("the service advises the relay for member %d (%s)%s\n", s, steerReason,
                    !(cfg_.honorSteer && relayEnabled_) ? ": punching anyway (relay not used)" : (skipDirect ? ": not punching" : ": punching in parallel"));
        }
        if (skipDirect) skipDirect_ = true;
        if (!skipDirect) {
            if (!m.cands.pub.IsZero()) p.cands[p.nCands++] = Candidate(m.cands.pub, CAND_PUBLIC);
            if (!m.cands.local.IsZero()) p.cands[p.nCands++] = Candidate(m.cands.local, CAND_LOCAL);
            // Galaxy (netx): the member's router forwards this port to it (UPnP): reachable even when punching would not be
            if (!m.cands.mapped.IsZero() && m.cands.mapped != m.cands.pub) p.cands[p.nCands++] = Candidate(m.cands.mapped, CAND_PUBLIC);
        }
        peerMapped_[s] = skipDirect ? Addr() : m.cands.mapped;
        if (!peers_[s].Begin(&env_, p)) {
            Fail(RandomFailureSentence());
            return;
        }
        relayKey_[s] = 0;   // Begin dropped the relay candidate: ServiceRelayPaths adds it again
        reportedOk_[s] = reportedFail_[s] = false;
        peerBegun_[s] = true;
        peerSig_[s] = sig;
        wasConnected_[s] = false;
        lostReported_[s] = false;
    }
}

static const uint32_t kLocalStallMs = 1500;   // a gap between ticks longer than this is the local side's own stall

void OnlineNet::Tick() {
    if (!started_ || phase_ == FAILED) return;
    // A long gap between two ticks is this console not running (the first frame of a scene can take many seconds), not the partner going quiet: both sides
    // stall like that when they enter the lobby together, and each would otherwise find the other silent for more than lostMs on its first tick afterwards.
    // (Galaxy: this is the transport's own clock; MpCore::receiveAll gives the session layer's peers the same credit on its own timers: no shared counter.)
    {
        const uint64_t t = env_.Now();
        if (lastTick_ && t > lastTick_ + kLocalStallMs) {
            const uint64_t gap = t - lastTick_;
            NPN_LOG("this console did not run for %u ms: the partners' silence is counted from now on\n", unsigned(gap));
            for (int s = 0; s < kMaxSlots; ++s)
                if (peerBegun_[s]) peers_[s].CreditLocalStall(gap);
        }
    }
    CheckResolver();
    PumpSocket();
    stun_.Tick();
    lastTick_ = env_.Now();
    if (signal_ && !signalLost_ && lastTick_ >= nextSignalPoll_) {
        nextSignalPoll_ = lastTick_ + cfg_.signalPollMs;
        signal_->Poll();
        if (signal_->GetStatus() == Signal::FAILED) {
            // The service is only needed to make a link, not to keep one. With a link up (a match under way) a refusal now (426 after a
            // release, a ban, the kill switch) must not end it: stop asking, tell the player, keep the sockets and peers running.
            if (AnyLinkUp()) SignalingLost(signal_->Error());
            else {
                CopyStr(errorCode_, sizeof errorCode_, signal_->ErrorCode());   // Galaxy (fix3)
                Fail(signal_->Error());
                return;
            }
        }
    }
    UpdateRelay(lastTick_);
    PublishIfReady();
    RefreshCandidates(lastTick_);
    SyncMembers();
    ServiceRelayPaths();
    bool anyLost = false;
    char why[64] = {0};
    for (int s = 0; s < kMaxSlots; ++s) {
        if (!peerBegun_[s]) continue;
        peers_[s].Tick();
        const Peer::State st = peers_[s].GetState();
        if (st == Peer::CONNECTED) wasConnected_[s] = true;
        if ((st == Peer::LOST || st == Peer::CLOSED) && wasConnected_[s] && !lostReported_[s]) {
            anyLost = true;
            snprintf(why, sizeof why, st == Peer::CLOSED ? "member %d closed the online link" : "member %d is silent", s);
            ReportPeerLost(s, why);   // Galaxy: every member's loss on its own (a host keeps its other guests)
        }
    }
    if (signal_ && !signalLost_) {   // svcfix2: which members this console heartbeats with directly (the signal reports it; a loss makes it ask at once)
        uint32_t mask = 0;
        for (int s = 0; s < kMaxSlots; ++s)
            if (s != mySlot_ && peerBegun_[s] && peers_[s].GetState() == Peer::CONNECTED) mask |= 1u << s;
        signal_->SetDirectLinks(mask);
    }
    if (memberGone_) {   // Galaxy: a connected member left the room (SyncMembers)
        memberGone_ = false;
        if (!anyLost) snprintf(why, sizeof why, "%s", memberGoneWhy_);
        anyLost = true;
    }
    if (anyLost && !rebuild_) {
        // Every member that was connected is gone? Then the link is gone. (With three members one leaving is the game's business.)
        bool anyUp = false;
        for (int s = 0; s < kMaxSlots; ++s)
            if (peerBegun_[s] && peers_[s].GetState() == Peer::CONNECTED) anyUp = true;
        if (!anyUp) {
            if (MigrationPossible()) {
                // Galaxy host migration: the path to the host ended but other members are still in the room: one of them becomes the host
                // (the service's host_slot) and the star re-centres on it. The link is only lost if that does not happen in time.
                if (!migrateDeadline_) {
                    migrateDeadline_ = lastTick_ + cfg_.hostMigrationWaitMs;
                    CopyStr(migrateWhy_, sizeof migrateWhy_, why);
                    NPN_LOG("the path to the host ended (%s): waiting up to %u ms for a new host\n", why, unsigned(cfg_.hostMigrationWaitMs));
                }
            } else {
                NPN_LOG("%s\n", why);
                if (cfg_.onLost) cfg_.onLost(cfg_.onLostCtx, why);
                if (!rebuild_) OnLinkLost(why);
            }
        }
    }
    if (migrateDeadline_ && !rebuild_) {
        if (AnyLinkUp()) {
            NPN_LOG("host migration done: the host is member %d (epoch %u)\n", hostSlot_, unsigned(hostEpoch_));
            migrateDeadline_ = 0;
            lostHost_ = -1;
        } else if (lastTick_ >= migrateDeadline_ || !MigrationPossible()) {
            char text[64];
            snprintf(text, sizeof text, "%s", lastTick_ >= migrateDeadline_ ? "no new host took over in time" : "nobody is left to take over as host");
            migrateDeadline_ = 0;
            lostHost_ = -1;
            NPN_LOG("host migration failed: %s (%s)\n", text, migrateWhy_);
            if (cfg_.onLost) cfg_.onLost(cfg_.onLostCtx, text);
            if (!rebuild_) OnLinkLost(text);
        }
    }
    UpdatePhase();
    ReportPunches();
    if (relayStage_ == RL_FAILED && phase_ != FAILED) {
        // The relay is not available. That only ends the session when nothing else can work: relay-only play, a pair the service told not to
        // punch, or every direct attempt already failed.
        bool anyUp = false, allNeed = true;
        int begun = 0, others = 0;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (!TopologyPeer(s, mySlot_) || !signal_ || !signal_->Room().members[s].present) continue;
            ++others;
            if (!peerBegun_[s]) continue;
            ++begun;
            const Peer::State st = peers_[s].GetState();
            if (st == Peer::CONNECTED) anyUp = true;
            if (st != Peer::NEEDS_RELAY && st != Peer::LOST && st != Peer::CLOSED) allNeed = false;
        }
        if (!anyUp && others > 0 && (cfg_.relayOnly || skipDirect_ || (begun == others && allNeed))) {
            Fail(relayError_);
            return;
        }
    }
    if (signal_ && !closedHandled_ && signal_->GetStatus() == Signal::CLOSED) {
        closedHandled_ = true;   // the room is gone (the host left, it timed out): a link that was never made cannot be made any more
        bool anyUp = false;
        for (int s = 0; s < kMaxSlots; ++s)
            if (peerBegun_[s] && peers_[s].GetState() == Peer::CONNECTED) anyUp = true;
        // Galaxy (fix3): not_member = the service dropped THIS console (silent > 60 s: asleep, a long stall). The room goes on
        // without us and its host tears our path down: playing on would be a session nobody else is in. Fail with the code, so the
        // game joins the room afresh (or leaves with a sentence) instead of waiting on a path that is going away.
        const bool dropped = strcmp(signal_->ErrorCode(), "not_member") == 0;
        if (!anyUp || dropped) {
            CopyStr(errorCode_, sizeof errorCode_, signal_->ErrorCode());
            Fail(signal_->Error()[0] ? signal_->Error() : "The room was closed.");
            return;
        }
        NPN_LOG("the room was closed (%s); the connected players play on\n", signal_->Error());
    }
    if (!startedSent_ && signal_ && signal_->GetStatus() == Signal::IN_ROOM && published_) {
        // the link is confirmed both ways with every member that is in the room: the match is about to begin (the service starts its record and
        // keeps the room for the length of a match)
        int others = 0, established = 0;
        for (int s = 0; s < kMaxSlots; ++s) {
            if (!TopologyPeer(s, mySlot_) || !signal_->Room().members[s].present) continue;   // Galaxy: the members this console peers with
            ++others;
            if (peerBegun_[s] && peers_[s].Established()) ++established;
        }
        if (others && established == others) {
            signal_->Started();
            startedSent_ = true;
        }
    }
    CheckLimits(lastTick_);
    LogStats(lastTick_);
}

// A6: once per attempt, tell the signaling server whether the direct path worked (it counts failures per pair and steers to the relay).
void OnlineNet::ReportPunches() {
    if (!signal_ || signal_->GetStatus() != Signal::IN_ROOM) return;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s == mySlot_ || !peerBegun_[s]) continue;
        const Peer::State st = peers_[s].GetState();
        const bool ok = st == Peer::CONNECTED && !reportedOk_[s];
        // A failure is reported when the direct attempt used its whole time or cannot work at all (two symmetric NATs). The early NEEDS_RELAY of a
        // symmetric NAT against a cone one (hopelessTimeoutMs, so the relay can start sooner) is not one yet: the direct path may still win, and a wrong report would steer this pair to the relay for good.
        const bool fail = st == Peer::NEEDS_RELAY && !reportedFail_[s] &&
                          (SinceMs(lastTick_, peers_[s].StartedAtMs()) >= cfg_.punch.timeoutMs || !PunchPossible(mine_.nat, signal_->Room().members[s].cands.nat));
        if (!ok && !fail) continue;
        PunchReport r;
        r.peerSlot = uint8_t(s);
        r.ok = ok;
        r.myNat = mine_.nat;
        r.peerNat = signal_->Room().members[s].cands.nat;
        if (ok) {
            r.ms = uint32_t(SinceMs(peers_[s].ConnectedAtMs(), peers_[s].StartedAtMs()));
            r.pathKind = uint8_t(peers_[s].Path().kind);
            r.observed = peers_[s].Path().addr;
            reportedOk_[s] = true;
        } else {
            r.ms = uint32_t(SinceMs(lastTick_, peers_[s].StartedAtMs()));
            reportedFail_[s] = true;
        }
        signal_->ReportPunch(r);
    }
}

// A11 / V5: the stage the session is in has a limit and a sentence for the player when it runs out.
void OnlineNet::CheckLimits(uint64_t now) {
    if (phase_ == FAILED || !started_) return;
    if (phase_ != stagePhase_) {
        stagePhase_ = phase_;
        stageAt_ = now;
    }
    const uint64_t inStage = SinceMs(now, stageAt_);
    if (phase_ == RESOLVING && cfg_.resolveLimitMs && inStage >= cfg_.resolveLimitMs) {
        NPN_LOG("looking up the STUN servers took more than %u ms: going on without STUN\n", unsigned(cfg_.resolveLimitMs));
        AbandonResolver();
        nStunAddrs_ = 0;
        CopyStr(warning_, sizeof warning_, "The internet address could not be found: only players on the same network can connect.");
        phase_ = STUN;
        BeginStun();
    } else if (phase_ == ROOM && cfg_.roomLimitMs && inStage >= cfg_.roomLimitMs) {
        Fail("The online service did not answer. Check the Wi-Fi connection and try again.");
    } else if (phase_ == WAITING && cfg_.hostWaitLimitMs && inStage >= cfg_.hostWaitLimitMs) {
        Fail(host_ ? "Nobody joined the room in time." : "The room is empty: the host left.");
    }
    if (rebuildDeadline_) {
        if (phase_ == READY) rebuildDeadline_ = 0;
        else if (now >= rebuildDeadline_) {
            rebuildDeadline_ = 0;
            Fail("The online connection could not be restored.");
        }
    }
}

int OnlineNet::WorstRttMs() const {
    int worst = -1;
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s] && peers_[s].Connected() && peers_[s].RttMs() > worst) worst = peers_[s].RttMs();
    return worst;
}
int OnlineNet::WorstJitterMs() const {
    int worst = -1;
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s] && peers_[s].Connected() && peers_[s].JitterMs() > worst) worst = peers_[s].JitterMs();
    return worst;
}
int OnlineNet::WorstLossPermille() const {
    int worst = -1;
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s] && peers_[s].Connected() && peers_[s].LossPermille() > worst) worst = peers_[s].LossPermille();
    return worst;
}

// A8: link quality in the log every few seconds, so a console report says what the network did.
void OnlineNet::LogStats(uint64_t now) {
    if (!cfg_.statsLogMs || now < nextStats_) return;
    nextStats_ = now + cfg_.statsLogMs;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s == mySlot_ || !peerBegun_[s] || !peers_[s].Connected()) continue;
        const Peer& p = peers_[s];
        NPN_LOG("link to member %d: %s path, rtt %d ms (min %d), jitter %d ms, loss %d.%d%%, sent %u recv %u, auth-fail %u replay-drop %u\n", s,
                p.Path().kind == CAND_LOCAL ? "local" : (p.Path().kind == CAND_RELAY ? "relay" : "public"), p.RttMs(), p.RttMinMs(), p.JitterMs(),
                p.LossPermille() < 0 ? 0 : p.LossPermille() / 10, p.LossPermille() < 0 ? 0 : p.LossPermille() % 10, NETFIX_U32C(p.Stats().dataSent),
                NETFIX_U32C(p.Stats().dataRecv), NETFIX_U32C(p.Stats().dropAuth), NETFIX_U32C(p.Stats().dropReplay));
    }
}

// Galaxy 3.0: star topology. The host (slot 0) makes a path to every guest; a guest only to the host. MESH keeps Strikers' every-pair behaviour.
bool OnlineNet::TopologyPeer(int slot, int mySlot) const {
    if (slot < 0 || slot >= kMaxSlots || slot == mySlot) return false;
    if (cfg_.topology == TOPOLOGY_MESH) return true;
    if (mySlot < 0) return false;
    const int host = HostSlot();   // (host migration: the centre is the room's host, slot 0 until the first host left)
    return mySlot == host ? true : slot == host;
}

int OnlineNet::HostSlot() const {
    if (RoomUsable()) {
        const int h = signal_->Room().hostSlot;
        if (h >= 0 && h < kMaxSlots) return h;
    }
    return hostSlot_;
}

// A star console lost its path to a host (the current one or the one before the last move), and another member is still in the room to take over.
bool OnlineNet::MigrationPossible() const {
    if (cfg_.topology != TOPOLOGY_STAR || !cfg_.hostMigrationWaitMs || lostHost_ < 0 || signalLost_ || !signal_ || signal_->GetStatus() != Signal::IN_ROOM)
        return false;
    const SigRoom& room = signal_->Room();
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && s != lostHost_ && room.members[s].present) return true;
    return false;
}

// Galaxy: one call per member whose connected path ended (before the room-wide onLost, which follows only when no member is connected any more).
void OnlineNet::ReportPeerLost(int slot, const char* why) {
    if (slot < 0 || slot >= kMaxSlots || lostReported_[slot]) return;
    lostReported_[slot] = true;
    if (slot == hostSlot_ || slot == prevHostSlot_) lostHost_ = slot;   // (host migration: this was the star's centre)
    NPN_LOG("member %d lost: %s\n", slot, why ? why : "?");
    if (cfg_.onPeerLost) cfg_.onPeerLost(cfg_.onPeerLostCtx, slot, why ? why : "");
}

bool OnlineNet::AnyLinkUp() const {
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s] && peers_[s].GetState() == Peer::CONNECTED) return true;
    return false;
}

void OnlineNet::SignalingLost(const char* why) {
    signalLost_ = true;
    char text[96];
    snprintf(text, sizeof text, "%s The match goes on.", why && *why ? why : "The online service cannot be reached.");
    if (strlen(text) >= sizeof text - 1) CopyStr(text, sizeof text, "The online service cannot be used any more. The match goes on.");
    CopyStr(warning_, sizeof warning_, text);
    NPN_LOG("signaling failed while a link is up (%s): polling stops, the match goes on\n", why ? why : "?");
    if (cfg_.onNotice) cfg_.onNotice(cfg_.onNoticeCtx, warning_);
}

bool OnlineNet::RoomUsable() const {
    if (!signal_) return false;
    const Signal::Status st = signal_->GetStatus();
    // a room whose service failed under a live link is still the room of that link (members, vips, names)
    return st == Signal::IN_ROOM || (st == Signal::CLOSED && signal_->Room().mySlot >= 0) || (signalLost_ && signal_->Room().mySlot >= 0);
}

void OnlineNet::UpdatePhase() {
    if (phase_ == FAILED || !started_) return;
    if (rebuild_) { phase_ = LOST; return; }
    if (resolver_) { phase_ = RESOLVING; return; }
    if (!RoomUsable()) { phase_ = (stunStarted_ && !stun_.Done()) ? STUN : ROOM; return; }
    const SigRoom& room = signal_->Room();
    int others = 0, connected = 0, punching = 0, needRelay = 0;
    for (int s = 0; s < kMaxSlots; ++s) {
        if (!TopologyPeer(s, mySlot_) || !room.members[s].present) continue;   // Galaxy: a star guest's phase is its path to the host
        ++others;
        if (!peerBegun_[s]) continue;
        switch (peers_[s].GetState()) {
        case Peer::CONNECTED: ++connected; break;
        case Peer::PUNCHING: ++punching; break;
        case Peer::NEEDS_RELAY: ++needRelay; break;
        default: break;
        }
    }
    if (connected || punching || !needRelay || RelayInProgress()) relayWaitDeadline_ = 0;
    if (connected) phase_ = READY;
    else if (punching) phase_ = PUNCHING;
    else if (needRelay) phase_ = RelayInProgress() || WaitingForRelayPath() ? PUNCHING : RELAY_NEEDED;
    else if (!published_) phase_ = cfg_.relayOnly ? (relayStage_ != RL_OFF ? PUNCHING : WAITING) : STUN;
    else phase_ = others ? PUNCHING : WAITING;
}

// An allocated relay is not yet a usable route: the partner may still be publishing its address or binding our address. RELAY_NEEDED ends
// the menu's attempt, so keep polling for a bounded grace period. RL_OFF also needs this grace: UpdateRelay sees a new NEEDS_RELAY next Tick.
bool OnlineNet::WaitingForRelayPath() {
    if (!relayEnabled_ || relayStage_ == RL_FAILED || !cfg_.relayWaitMs) return false;
    const uint64_t now = env_.Now();
    if (!relayWaitDeadline_) {
        relayWaitDeadline_ = now + cfg_.relayWaitMs;
        NPN_LOG("no direct path: waiting up to %u ms for the relay path\n", unsigned(cfg_.relayWaitMs));
    }
    if (now < relayWaitDeadline_) return true;
    if (!error_[0]) {
        CopyStr(error_, sizeof error_, "The other player could not be reached through the relay. Try again.");
        NPN_LOG("relay path wait ended: %s\n", error_);
    }
    return false;
}

void OnlineNet::OnLinkLost(const char* why) {
    if (!started_ || rebuild_ || phase_ == FAILED) return;
    NPN_LOG("link lost (%s): closing it\n", why ? why : "?");
    {
        const char* w = why ? why : "";
        const char* sentence = w;
        if (strstr(w, "resumed") || strstr(w, "sleep")) sentence = "The online link ended because the Vita went to sleep.";
        else if (strstr(w, "address changed")) sentence = "The online link ended because the Wi-Fi network changed.";
        else if (strstr(w, "Wi-Fi")) sentence = "The online link ended because the Wi-Fi connection was lost.";
        else if (strstr(w, "closed the online link") || strstr(w, "left the room")) sentence = "The other player left the match.";
        else if (strstr(w, "new host") || strstr(w, "take over as host")) sentence = "The host left and nobody could take over.";
        else if (strstr(w, "is silent")) sentence = "The other player stopped responding.";
        CopyStr(endReason_, sizeof endReason_, sentence);
    }
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s == mySlot_ || !peerBegun_[s]) continue;
        peers_[s].SendClose();
        lostReported_[s] = true;
    }
    ReleaseRelay();     // best effort: the socket is about to go; an allocation that stays is released on the next Allocate (437)
    CloseSocket();      // the stale socket: its address and NAT mapping are not trusted any more
    if (signalLost_) {
        // a new link needs the service (STUN result, endpoints, key): it is gone, so there is nothing to rebuild
        Fail("The online connection ended, and the online service cannot be reached to start it again.");
        return;
    }
    rebuild_ = true;
    phase_ = LOST;
}

void OnlineNet::OnLinkStart() {
    if (!started_ || !rebuild_ || phase_ == FAILED) return;
    rebuild_ = false;
    refreshing_ = false;
    rebuildDeadline_ = cfg_.rebuildLimitMs ? env_.Now() + cfg_.rebuildLimitMs : 0;
    ++gen_;
    if (!udp_->Open(cfg_.port)) {
        Fail(udp_->Error()[0] ? udp_->Error() : "The network socket could not be reopened. Is Wi-Fi connected?");
        return;
    }
    NPN_LOG("rebuilding the link (generation %u)\n", gen_);
    migrateDeadline_ = 0;
    lostHost_ = -1;
    published_ = false;
    memset(peerBegun_, 0, sizeof peerBegun_);   // every member punches afresh once our new candidates are out
    memset(relayKey_, 0, sizeof relayKey_);
    relayWaitDeadline_ = 0;
    mine_.relay = Addr();
    if (cfg_.relayOnly) { /* the relay comes first; nothing is published before it */ }
    else if (nStunAddrs_ > 0) BeginStun();
    else stunStarted_ = true;
    phase_ = cfg_.relayOnly ? WAITING : STUN;
}

int OnlineNet::SendFrame(int slot, const void* data, size_t len) {
    if (!started_ || rebuild_) return -1;
    if (len > kMaxFrame) return -1;
    if (slot >= 0) {
        if (slot < kMaxSlots && slot != mySlot_ && peerBegun_[slot]) peers_[slot].SendFrame(data, len);
        return int(len);
    }
    for (int s = 0; s < kMaxSlots; ++s)
        if (s != mySlot_ && peerBegun_[s] && peers_[s].Connected()) peers_[s].SendFrame(data, len);
    return int(len);
}

int OnlineNet::RecvFrame(int* slot, void* buffer, size_t capacity) {
    if (!started_) return 0;
    // The link calls this up to 256 times per pump: the frames already queued go first; the full Tick (timers, signaling) runs at most every
    // 4 ms, in between only the socket is drained.
    for (int pass = 0; pass < 2; ++pass) {
        for (int s = 0; s < kMaxSlots; ++s) {
            if (s == mySlot_ || !peerBegun_[s]) continue;
            const int n = peers_[s].PopFrame(buffer, capacity);
            if (n > 0) {
                *slot = s;
                return n;
            }
        }
        if (pass == 0) {
            if (SinceMs(env_.Now(), lastTick_) >= 4) Tick();
            else PumpSocket();
        }
    }
    return 0;
}

// ---- the relay (TURN) path --------------------------------------------------------------------------------------------------------
// Wanted when: the player chose relay-only, the service steers there (relay-only profile, symmetric NAT, failures of the pair), the partner already
// has an allocation (both sides need one: each sends to the other's relayed address), both NATs are symmetric, or a punch ran out of time.
// Then: credentials from the service (a room request, the TURN key stays on the server) -> DNS of the relay host on a worker thread -> Allocate
// -> the relayed address is published in the room -> for each member: CreatePermission + ChannelBind of its relayed address -> the relay becomes
// a candidate of that member's Peer, which probes, confirms and prefers a direct path whenever one exists (relay ranks last).
bool OnlineNet::RelayInProgress() const { return relayStage_ == RL_CREDS || relayStage_ == RL_RESOLVE || relayStage_ == RL_ALLOC || rotating_; }

bool OnlineNet::CredentialsAlive(uint64_t now) const {
    if (!haveCreds_) return false;
    // one credential per room and hour is all the service gives out: renew only shortly before it ends (a minute; a third of it for the very short test ones)
    const uint64_t margin = creds_.ttlSec > 360 ? 60000u : uint64_t(creds_.ttlSec) * 1000u / 3u;
    return now < credsAt_ + uint64_t(creds_.ttlSec) * 1000u - margin;
}

void OnlineNet::RelayFail(const char* why) {
    CopyStr(relayError_, sizeof relayError_, why);
    relayStage_ = RL_FAILED;
    rotating_ = swapPending_ = false;
    turn_.Reset();
    CopyStr(warning_, sizeof warning_, why);
    NPN_LOG("relay failed: %s\n", why);
}

void OnlineNet::AbandonTurnResolve() {
    if (!turnResolve_) return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&turnResolve_->done, &expected, 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete turnResolve_;
    turnResolve_ = 0;
}

void OnlineNet::ReleaseRelay() {
    if (turn_.Ready()) turn_.Release();
    if (signal_ && relayStage_ != RL_OFF) {
        uint64_t bytes = 0;
        for (int s = 0; s < kMaxSlots; ++s) bytes += peers_[s].Stats().relayTx;   // what we sent into the relay; the partner reports its own (no double count)
        if (bytes > usageSent_) {
            signal_->ReportTurnUsage(bytes - usageSent_);
            usageSent_ = bytes;
        }
    }
    relayStage_ = relayStage_ == RL_FAILED ? RL_FAILED : RL_OFF;
    turn_.Reset();
    AbandonTurnResolve();
}

// URLs of the credentials -> addresses. Numeric hosts need no DNS.
void OnlineNet::StartTurnResolve() {
    AbandonTurnResolve();
    nRelayServers_ = 0;
    if (cfg_.nTurnAddrs > 0) {
        for (int i = 0; i < cfg_.nTurnAddrs && i < 4; ++i) relayServers_[nRelayServers_++] = cfg_.turnAddrs[i];
        FinishRelayStart();
        return;
    }
    TurnResolve* r = new TurnResolve;
    bool needThread = false;
    for (int i = 0; i < creds_.nUrls && r->n < 4; ++i) {
        char host[96];
        uint16_t port = 0;
        if (!ParseTurnUrl(creds_.urls[i], host, sizeof host, &port)) continue;
        Addr a;
        if (ParseAddr(host, port, &a)) {
            r->ip[r->n] = a.ip;
        } else {
            needThread = true;
            CopyStr(r->host[r->n], sizeof r->host[0], host);
        }
        r->port[r->n] = port;
        ++r->n;
    }
    if (r->n == 0) {
        delete r;
        RelayFail("The online service sent relay addresses the game cannot use.");
        return;
    }
    if (!needThread) {
        for (int i = 0; i < r->n; ++i) relayServers_[nRelayServers_++] = Addr(r->ip[i], r->port[i]);
        delete r;
        FinishRelayStart();
        return;
    }
    turnResolve_ = r;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    const int rc = pthread_create(&r->thread, &attr, [](void* arg) -> void* {
            TurnResolve* rr = static_cast<TurnResolve*>(arg);
            for (int i = 0; i < rr->n; ++i) {
                if (!rr->host[i][0]) continue;
                uint32_t ips[4] = {0};
                rr->ip[i] = ResolveIpv4(rr->host[i], ips, 4) > 0 ? ips[0] : 0;
            }
            int expected = 0;
            if (!__atomic_compare_exchange_n(&rr->done, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) delete rr;
            return 0;
        }, r);
    if (rc == 0) pthread_detach(r->thread);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete r;
        turnResolve_ = 0;
        RelayFail("cannot start the resolver thread");
        return;
    }
    relayStage_ = RL_RESOLVE;
    relayStageAt_ = env_.Now();
}

void OnlineNet::FinishRelayStart() {
    if (nRelayServers_ == 0) {
        RelayFail("The relay server could not be found. Check the Wi-Fi connection and try again.");
        return;
    }
    TurnConfig tc = cfg_.turn;
    turn_.Start(&env_, relayServers_, nRelayServers_, creds_.user, creds_.pass, tc);
    relayStage_ = RL_ALLOC;
    relayStageAt_ = env_.Now();
    NPN_LOG("relay: allocating on %s\n", AddrLog(relayServers_[0]));
}

void OnlineNet::UpdateRelay(uint64_t now) {
    if (!relayEnabled_ || !signal_) return;
    // The allocation, its permissions and its channels live on the relay, with credentials we already hold: they are refreshed whatever the service does.
    // Behind the status check below a match on the relay outlived a failed service (or a closed room) by one permission lifetime only (5 minutes), then went silent.
    turn_.Tick();
    if (signal_->GetStatus() != Signal::IN_ROOM) return;
    const SigRoom& room = signal_->Room();
    bool peerPresent = false;
    for (int s = 0; s < kMaxSlots; ++s)
        if (TopologyPeer(s, room.mySlot) && room.members[s].present) peerPresent = true;   // (mySlot_ is only set once the room has been looked at)
    // 1. is the relay wanted? (Galaxy: only for the members this console peers with; a pair's own steer advice counts like the room's)
    if (!relayWanted_ && relayStage_ == RL_OFF && peerPresent) {
        const char* why = 0;
        if (cfg_.relayOnly) why = "relay-only mode";
        else if (cfg_.honorSteer && room.steerRelay) why = room.steerReason[0] ? room.steerReason : "advised by the online service";
        for (int s = 0; s < kMaxSlots && !why; ++s) {
            if (!TopologyPeer(s, room.mySlot) || !room.members[s].present) continue;
            const SigMember& m = room.members[s];
            if (cfg_.honorSteer && m.steerRelay) { why = m.steerReason[0] ? m.steerReason : "advised by the online service"; break; }
            if (!m.cands.relay.IsZero()) why = "the other player uses the relay";
            else if (published_ && m.cands.valid && !PunchPossible(mine_.nat, m.cands.nat)) why = "both NATs are symmetric";
            else if (peerBegun_[s] && peers_[s].GetState() == Peer::NEEDS_RELAY) why = "no direct path";
        }
        if (why) {
            relayWanted_ = true;
            snprintf(relayWhy_, sizeof relayWhy_, "Using the relay (%s).", why);
            NPN_LOG("relay wanted: %s\n", why);
        }
    }
    if (!relayWanted_) return;
    // 2. the stages
    switch (relayStage_) {
    case RL_OFF:
        if (CredentialsAlive(now)) {
            StartTurnResolve();   // an earlier session of this room (a rebuilt link) still has working credentials
        } else {
            ReportPunches();   // svcfix3 (S5): the punch verdict reaches the service BEFORE the request for credentials (a reason it can see)
            signal_->RequestTurn();
            relayStage_ = RL_CREDS;
            relayStageAt_ = now;
        }
        break;
    case RL_CREDS: {
        const Signal::TurnStatus st = signal_->TurnState();
        if (st == Signal::TURN_READY) {
            if (rotating_ && turn_.Ready() && turn_.CredentialsRejected()) {
                // The old credentials ran out before we replaced them: the allocation made with them can neither be refreshed nor released (only
                // its own user may do that), but it keeps carrying the match until its lifetime ends. Keep playing on it; the new credentials
                // take over when it is gone.
                pendingCreds_ = signal_->Turn();
                swapPending_ = true;
                swapAt_ = turn_.ExpiresAt() > 1500 ? turn_.ExpiresAt() - 1500 : 0;
                rotating_ = false;
                relayStage_ = RL_READY;
                NPN_LOG("relay: new credentials wait for the old allocation to end\n");
                break;
            }
            creds_ = signal_->Turn();
            credsAt_ = now;
            haveCreds_ = true;
            if (rotating_) {
                // fresh credentials replace the ones that are about to run out: the old allocation is released while its credentials still work,
                // then a new one is made (a new relayed address: published, and the members bind it again)
                turn_.Release();
                rotating_ = false;
                FinishRelayStart();
            } else {
                StartTurnResolve();
            }
        } else if (st == Signal::TURN_FAILED) {
            if (rotating_) { rotating_ = false; relayStage_ = RL_READY; rotateRetryAt_ = now + (signal_->TurnWaitMs() > 15000 ? signal_->TurnWaitMs() : 15000); CopyStr(warning_, sizeof warning_, signal_->TurnError()); }
            else RelayFail(signal_->TurnError());
        } else if (cfg_.relayLimitMs && SinceMs(now, relayStageAt_) >= cfg_.relayLimitMs) {
            RelayFail("The online service did not hand out relay credentials in time. Try again.");
        }
        break;
    }
    case RL_RESOLVE:
        if (turnResolve_ && __atomic_load_n(&turnResolve_->done, __ATOMIC_ACQUIRE) == 1) {
            TurnResolve* r = turnResolve_;
            turnResolve_ = 0;
            nRelayServers_ = 0;
            for (int i = 0; i < r->n; ++i) {
                if (r->ip[i] == 0) { NPN_LOG("cannot resolve %s\n", r->host[i]); continue; }
                relayServers_[nRelayServers_++] = Addr(r->ip[i], r->port[i]);
            }
            delete r;
            FinishRelayStart();
        } else if (cfg_.relayLimitMs && SinceMs(now, relayStageAt_) >= cfg_.relayLimitMs) {
            AbandonTurnResolve();
            RelayFail("The relay server could not be found. Check the Wi-Fi connection and try again.");
        }
        break;
    case RL_ALLOC:
        if (turn_.Ready()) {
            relayStage_ = RL_READY;
            relayStageAt_ = now;
            credRetries_ = 0;
            NPN_LOG("relay ready: %s\n", AddrLog(turn_.Relayed()));
        } else if (turn_.GetState() == TurnClient::FAILED) {
            if (turn_.AuthRefused() && credRetries_ < 2) {
                // the credentials ran out while they waited (or the service's clock and ours disagree): ask for fresh ones
                ++credRetries_;
                NPN_LOG("relay: the credentials were refused: asking for new ones (%d)\n", credRetries_);
                turn_.Reset();
                haveCreds_ = false;
                signal_->RequestTurn();
                relayStage_ = RL_CREDS;
                relayStageAt_ = now;
            } else {
                RelayFail(turn_.Error());
            }
        }
        break;
    case RL_READY: {
        // a new allocation (refresh lost, credentials rotated) has a new relayed address: publish it
        if (turn_.Ready() && turn_.Generation() != relayGenSeen_ && published_) {
            relayGenSeen_ = turn_.Generation();
            mine_.relay = turn_.Relayed();
            signal_->PublishCandidates(mine_);
            lastPublishAt_ = now;
            NPN_LOG("relay address published: %s\n", AddrLog(mine_.relay));
        }
        if (turn_.GetState() == TurnClient::FAILED) {
            RelayFail(turn_.Error());
            break;
        }
        if (swapPending_ && (now >= swapAt_ || !turn_.Ready())) {
            swapPending_ = false;
            creds_ = pendingCreds_;
            credsAt_ = now;
            haveCreds_ = true;
            NPN_LOG("relay: the old allocation ends: starting a new one with the new credentials\n");
            FinishRelayStart();
            break;
        }
        if (!rotating_ && !swapPending_ && now >= rotateRetryAt_ && (turn_.CredentialsRejected() || !CredentialsAlive(now))) {
            NPN_LOG("relay credentials %s: asking for new ones\n", turn_.CredentialsRejected() ? "refused" : "about to run out");
            rotating_ = true;
            relayStageAt_ = now;
            signal_->RequestTurn();
            relayStage_ = RL_CREDS;
        }
        break;
    }
    case RL_FAILED:
        break;
    }
}

// Every member whose relayed address is known gets a channel on our allocation and, once bound, the relay as a candidate of its Peer.
void OnlineNet::ServiceRelayPaths() {
    if (relayStage_ != RL_READY || !turn_.Ready() || !signal_ || signal_->GetStatus() != Signal::IN_ROOM) return;
    const SigRoom& room = signal_->Room();
    for (int s = 0; s < kMaxSlots; ++s) {
        if (s == mySlot_ || !peerBegun_[s]) continue;
        const Addr theirs = room.members[s].cands.relay;
        if (theirs.IsZero()) continue;
        const uint16_t ch = turn_.BindPeer(s, theirs);
        if (!ch || !turn_.ChannelBound(s)) continue;
        const Addr srv = turn_.Server();
        uint32_t key = 2166136261u;
        key = Mix(key, theirs.ip);
        key = Mix(key, theirs.port);
        key = Mix(key, ch);
        key = Mix(key, turn_.Generation());
        key = Mix(key, srv.ip);
        key = Mix(key, srv.port);
        key = key ? key : 1;
        if (key == relayKey_[s]) continue;
        relayKey_[s] = key;
        NPN_LOG("relay path to member %d: server %s, channel 0x%04x\n", s, AddrLog(srv), unsigned(ch));
        if (!peers_[s].SetRelay(srv, ch)) { Fail(RandomFailureSentence()); return; }
    }
}

void OnlineNet::BindRelay(int slot, const Addr& relayServer, uint16_t channel) {
    if (slot < 0 || slot >= kMaxSlots || slot == mySlot_ || !peerBegun_[slot]) return;
    peers_[slot].AddCandidate(Candidate(relayServer, CAND_RELAY, channel));
    if (!peers_[slot].Restart()) Fail(RandomFailureSentence());
}

void OnlineNet::GetInfo(OnlineInfo* info) const {
    memset(info, 0, sizeof *info);
    info->relayState = relayStage_ == RL_READY ? 2 : (relayStage_ == RL_FAILED ? 3 : (relayStage_ == RL_OFF ? 0 : 1));
    if (relayStage_ == RL_FAILED) CopyStr(info->relayText, sizeof info->relayText, relayError_);
    else if (relayStage_ != RL_OFF) CopyStr(info->relayText, sizeof info->relayText, relayWhy_);
    info->relayedAddr = relayStage_ == RL_READY ? turn_.Relayed() : Addr();
    info->phase = phase_;
    CopyStr(info->invite, sizeof info->invite, invite_);
    info->mySlot = mySlot_;
    info->myVip = myVip_;
    info->myNat = mine_.nat;
    info->publicAddr = mine_.pub;
    info->localAddr = mine_.local;
    CopyStr(info->error, sizeof info->error, error_);
    CopyStr(info->errorCode, sizeof info->errorCode, phase_ == FAILED ? errorCode_ : "");
    CopyStr(info->warning, sizeof info->warning, warning_);
    CopyStr(info->endReason, sizeof info->endReason, endReason_);
    info->hostSlot = hostSlot_;
    info->hostEpoch = hostEpoch_;
    info->migrating = migrateDeadline_ ? 1 : 0;
    if (!RoomUsable()) return;
    const SigRoom& room = signal_->Room();
    for (int s = 0; s < kMaxSlots; ++s) {
        PeerInfo& p = info->peers[s];
        const SigMember& m = room.members[s];
        p.present = m.present;
        p.slot = s;
        CopyStr(p.name, sizeof p.name, m.name);
        p.vip = m.vip;
        p.nat = m.cands.nat;
        if (!m.present || s == mySlot_) continue;
        ++info->peerCount;
        if (!peerBegun_[s]) continue;
        const Peer& peer = peers_[s];
        p.state = peer.GetState();
        p.pathKind = peer.Path().kind;
        p.established = peer.Established();
        p.dataSent = peer.Stats().dataSent;
        p.dataRecv = peer.Stats().dataRecv;
        p.dropAuth = peer.Stats().dropAuth;
        p.dropReplay = peer.Stats().dropReplay;
        p.rttMs = peer.RttMs();
        p.rttMinMs = peer.RttMinMs();
        p.jitterMs = peer.JitterMs();
        p.lossPermille = peer.LossPermille();
        if (peer.GetState() == Peer::CONNECTED) ++info->connectedCount;
    }
}

}  // namespace NpNet
