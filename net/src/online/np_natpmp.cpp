// Galaxy 3.0 (agent natpmp): NAT-PMP (RFC 6886) + PCP (RFC 6887) client. See np_natpmp.h. Written from the RFCs for this project.
#include "np_natpmp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NP_TARGET_VITA)
#include <psp2/net/netctl.h>
#elif defined(__linux__)
#include <arpa/inet.h>
#endif

namespace NpNet {

namespace {

const uint8_t kUdp = 17;
const size_t kPcpMax = 1100;                    // RFC 6887 section 7: a PCP message is at most 1100 octets
const uint32_t kSendAt[] = {0, 250, 750, 1750};   // RFC 6886 section 3.1: 250 ms, doubling (0, +250, +500, +1000); cut at the budget
const int kSends = int(sizeof kSendAt / sizeof kSendAt[0]);
const uint32_t kNatpmpGraceMs = 300;            // NAT-PMP answered, PCP did not: a PCP server would have by now
const uint32_t kRemoveMs = 1000;
const uint32_t kPollBudgetMs = 1500;
const int kMaxPerStep = 16;                     // datagrams read per Step() (a flood cannot hold the helper thread)

uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

// ::ffff:a.b.c.d (RFC 6887 section 5: IPv4 addresses travel IPv4-mapped); zero ip = the unspecified address ::
void PutMapped(uint8_t* p, uint32_t ip) {
    memset(p, 0, 16);
    if (ip) {
        p[10] = p[11] = 0xFF;
        Put32(p + 12, ip);
    }
}
bool GetMapped(const uint8_t* p, uint32_t* ip) {
    for (int i = 0; i < 10; ++i)
        if (p[i]) return false;
    if (p[10] != 0xFF || p[11] != 0xFF) return false;
    *ip = Be32(p + 12);
    return true;
}

const char* Ip4(uint32_t ip, char* out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return out;
}

}  // namespace

// ---- codec ----------------------------------------------------------------------------------------------------------------------

size_t NatPmp::BuildPmpAddrRequest(uint8_t* out) {
    out[0] = 0;   // version
    out[1] = 0;   // opcode: external address
    return 2;
}

size_t NatPmp::BuildPmpMapRequest(uint8_t* out, uint16_t intPort, uint16_t extPort, uint32_t lifetime) {
    out[0] = 0;
    out[1] = 1;   // opcode: map UDP
    out[2] = out[3] = 0;
    Put16(out + 4, intPort);
    Put16(out + 6, extPort);
    Put32(out + 8, lifetime);
    return 12;
}

size_t NatPmp::BuildPcpMapRequest(uint8_t* out, uint32_t clientIp, const uint8_t* nonce, uint16_t intPort, uint16_t extPort,
                                  uint32_t suggestedExtIp, uint32_t lifetime) {
    memset(out, 0, 60);
    out[0] = 2;   // version
    out[1] = 1;   // R = 0 (request), opcode MAP
    Put32(out + 4, lifetime);
    PutMapped(out + 8, clientIp);
    memcpy(out + 24, nonce, 12);
    out[36] = kUdp;
    Put16(out + 40, intPort);
    Put16(out + 42, extPort);
    PutMapped(out + 44, suggestedExtIp);
    return 60;
}

size_t NatPmp::BuildPcpAnnounce(uint8_t* out, uint32_t clientIp) {
    memset(out, 0, 24);
    out[0] = 2;
    out[1] = 0;   // opcode ANNOUNCE
    PutMapped(out + 8, clientIp);
    return 24;
}

bool NatPmp::ParseReply(const uint8_t* d, size_t len, Reply* r) {
    memset(r, 0, sizeof *r);
    if (!d || len < 8) return false;
    if (d[0] == 0) {   // NAT-PMP
        const uint8_t op = d[1];
        r->result = Be16(d + 2);
        r->epoch = Be32(d + 4);
        if (op == 128) {
            if (r->result == 0 ? len != 12 : len > 16) return false;
            r->kind = K_PMP_ADDR;
            if (r->result == 0) {
                r->extIp = Be32(d + 8);
                r->extIpOk = true;
            }
            return true;
        }
        if (op == 129) {
            if (r->result == 0 ? len != 16 : len > 16) return false;
            r->kind = K_PMP_MAP;
            if (len >= 16) {
                r->intPort = Be16(d + 8);
                r->extPort = Be16(d + 10);
                r->lifetime = Be32(d + 12);
            }
            return true;
        }
        return false;
    }
    if (d[0] == 2) {   // PCP
        if (len < 24 || len > kPcpMax || (len & 3)) return false;
        if (!(d[1] & 0x80)) return false;   // a request, not a response
        const uint8_t op = d[1] & 0x7F;
        r->result = d[3];
        r->lifetime = Be32(d + 4);
        r->epoch = Be32(d + 8);
        if (op == 0) {
            r->kind = K_PCP_ANNOUNCE;
            return true;
        }
        if (op != 1) return false;
        r->kind = K_PCP_MAP;
        if (len < 60) return r->result == 1;   // UNSUPP_VERSION is the one answer that is only the header
        memcpy(r->nonce, d + 24, 12);
        if (d[36] != kUdp) return false;
        r->intPort = Be16(d + 40);
        r->extPort = Be16(d + 42);
        r->extIpOk = GetMapped(d + 44, &r->extIp);
        return true;
    }
    return false;
}

bool NatPmp::EpochRestarted(uint32_t oldEpoch, uint32_t newEpoch, uint64_t elapsedMs) {
    if (newEpoch < oldEpoch) return true;
    const uint64_t elapsed = elapsedMs / 1000, delta = newEpoch - oldEpoch;
    if (delta + 2 < elapsed * 7 / 8) return true;       // the gateway's clock ran slow: it started over
    if (delta > elapsed * 9 / 8 + 2) return true;       // ... or fast (it was reset to an older state and counted on)
    return false;
}

const char* NatPmp::PmpResultText(unsigned code) {
    switch (code) {
    case 0: return "success";
    case 1: return "the router does not speak this NAT-PMP version";
    case 2: return "the router refused (port mapping not enabled)";
    case 3: return "the router has no internet connection (network failure)";
    case 4: return "the router is out of port mappings";
    case 5: return "the router does not support this NAT-PMP request";
    default: return "the router sent an unknown NAT-PMP result";
    }
}

const char* NatPmp::PcpResultText(unsigned code) {
    switch (code) {
    case 0: return "success";
    case 1: return "the router does not speak PCP version 2";
    case 2: return "the router refused (not authorized)";
    case 3: return "the router found the PCP request malformed";
    case 4: return "the router does not support this PCP request";
    case 5: case 6: return "the router rejected a PCP option";
    case 7: return "the router has no internet connection (network failure)";
    case 8: return "the router is out of port mappings";
    case 9: return "the router does not forward UDP";
    case 10: return "this console has used up its port mappings on the router";
    case 11: return "the router cannot give that external port";
    case 12: return "the router does not see this console at the address it has (address mismatch)";
    case 13: return "the router refused (too many remote peers)";
    default: return "the router sent an unknown PCP result";
    }
}

const char* NatPmp::StateName(State s) {
    static const char* const k[] = {"idle", "probing", "mapping", "mapped", "renewing", "removing", "removed", "failed"};
    return unsigned(s) < sizeof k / sizeof k[0] ? k[s] : "?";
}

// ---- the gateway ----------------------------------------------------------------------------------------------------------------

uint32_t NatPmp::DefaultGateway(uint32_t localIp, bool* guessed) {
    uint32_t gw = 0;
#if defined(NP_TARGET_VITA)
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE, &info) >= 0) {
        info.default_route[sizeof info.default_route - 1] = 0;
        unsigned p[4];
        if (sscanf(info.default_route, "%u.%u.%u.%u", &p[0], &p[1], &p[2], &p[3]) == 4 && p[0] < 256 && p[1] < 256 && p[2] < 256 && p[3] < 256)
            gw = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }
#elif defined(__linux__)
    // /proc/net/route: Iface Destination Gateway Flags RefCnt Use Metric ... (addresses as the raw network-order word in hex)
    if (FILE* f = fopen("/proc/net/route", "r")) {
        char line[256];
        int bestMetric = 0x7FFFFFFF;
        while (fgets(line, sizeof line, f)) {
            char iface[32];
            unsigned long dest, gate;
            unsigned flags;
            int refs, use, metric;
            if (sscanf(line, "%31s %lx %lx %x %d %d %d", iface, &dest, &gate, &flags, &refs, &use, &metric) != 7) continue;   // (the header line)
            if (dest != 0 || !(flags & 2) || gate == 0) continue;   // RTF_GATEWAY default route
            if (metric < bestMetric) {
                bestMetric = metric;
                gw = ntohl(uint32_t(gate));
            }
        }
        fclose(f);
    }
#endif
    if (gw && gw != localIp && (gw >> 28) != 0xE && (gw >> 24) != 127) {
        *guessed = false;
        return gw;
    }
    *guessed = true;
    const uint32_t g = (localIp & 0xFFFFFF00u) | 1u;
    return g != localIp ? g : 0;
}

// ---- the client -----------------------------------------------------------------------------------------------------------------

NatPmp::NatPmp()
    : state_(IDLE), proto_(PROTO_NONE), udp_(0), localIp_(0), extIp_(0), suggestPort_(0), mappedPort_(0), lease_(0), wantLease_(0), pcpOk_(false),
      exchStart_(0), deadline_(0), sendIdx_(0), pcpUnsupported_(false), natSeen_(false), natSeenAt_(0), natErr_(-1), haveEpoch_(false), epoch_(0),
      epochAt_(0), renewAtMs_(0), leaseEndMs_(0), pollAtMs_(0), polling_(false), retried11_(false), mayExist_(false), answered_(false), renewals_(0) {
    memset(nonce_, 0, sizeof nonce_);
    error_[0] = 0;
}

NatPmp::~NatPmp() { Abort(); }

void NatPmp::Log(const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (cfg_.log) cfg_.log(cfg_.logCtx, line);
    else NpLogf("natpmp", "%s\n", line);
}

void NatPmp::Fail(const char* why) {
    if (udp_) udp_->Close();
    snprintf(error_, sizeof error_, "%s", why ? why : "failed");
    state_ = FAILED;
    Log("failed: %s", error_);
}

void NatPmp::Abort() {
    if (udp_) {
        delete udp_;
        udp_ = 0;
    }
    if (state_ != FAILED && state_ != IDLE) state_ = REMOVED;
}

bool NatPmp::Start(const NatPmpConfig& cfg, uint64_t now, Udp* udp) {
    cfg_ = cfg;
    if (!cfg_.internalPort) {
        Fail("no port to forward");
        return false;
    }
    if (!cfg_.probeMs) cfg_.probeMs = 3000;
    if (!cfg_.mapMs) cfg_.mapMs = 2000;
    if (!cfg_.pollMs) cfg_.pollMs = 300000;
    wantLease_ = cfg_.leaseSec ? cfg_.leaseSec : 7200;
    udp_ = udp ? udp : CreatePlatformUdp();
    if (!udp_ || !udp_->Open(0)) {
        Fail(udp_ && udp_->Error()[0] ? udp_->Error() : "no socket for NAT-PMP");
        return false;
    }
    localIp_ = cfg_.internalIp ? cfg_.internalIp : udp_->LocalIp();
    if (!localIp_) {
        Fail("this console has no network address");
        return false;
    }
    gw_ = cfg_.gateway;
    if (!gw_.ip) {
        bool guessed = false;
        gw_.ip = DefaultGateway(localIp_, &guessed);
        char t[16];
        if (gw_.ip) Log("gateway %s (%s)", Ip4(gw_.ip, t), guessed ? "guessed: the .1 of this /24" : "default route");
    }
    if (!gw_.port) gw_.port = kServerPort;
    if (!gw_.ip) {
        Fail("no router address");
        return false;
    }
    // the PCP nonce must be random (an off-path sender must not guess it): without a working generator only NAT-PMP is used
    pcpOk_ = !cfg_.noPcp && SystemRandom(0, nonce_, sizeof nonce_);
    if (!cfg_.noPcp && !pcpOk_) Log("no random numbers: PCP skipped, NAT-PMP only");
    suggestPort_ = cfg_.externalPort ? cfg_.externalPort : cfg_.internalPort;
    state_ = PROBING;
    BeginExchange(now, cfg_.probeMs);
    return true;
}

void NatPmp::BeginExchange(uint64_t now, uint32_t budgetMs) {
    exchStart_ = now;
    deadline_ = now + budgetMs;
    sendIdx_ = 0;
}

void NatPmp::Send(const uint8_t* p, size_t n) {
    if (udp_ && udp_->SendTo(gw_, p, n) < 0 && state_ != REMOVING) Fail("cannot send to the router");
}

void NatPmp::SendRound(uint64_t now) {
    while (sendIdx_ < kSends && now >= exchStart_ + kSendAt[sendIdx_] && now < deadline_ && udp_) {
        ++sendIdx_;
        uint8_t p[64];
        switch (state_) {
        case PROBING:
            if (pcpOk_) Send(p, BuildPcpMapRequest(p, localIp_, nonce_, cfg_.internalPort, suggestPort_, 0, wantLease_));
            if (state_ == PROBING) Send(p, BuildPmpAddrRequest(p));
            break;
        case MAPPING: Send(p, BuildPmpMapRequest(p, cfg_.internalPort, suggestPort_, wantLease_)); break;
        case RENEWING:
            if (proto_ == PROTO_PCP) Send(p, BuildPcpMapRequest(p, localIp_, nonce_, cfg_.internalPort, mappedPort_, extIp_, wantLease_));
            else Send(p, BuildPmpMapRequest(p, cfg_.internalPort, mappedPort_, wantLease_));
            break;
        case MAPPED:   // polling the epoch
            if (proto_ == PROTO_PCP) Send(p, BuildPcpAnnounce(p, localIp_));
            else Send(p, BuildPmpAddrRequest(p));
            break;
        case REMOVING:
            if (proto_ == PROTO_PCP) Send(p, BuildPcpMapRequest(p, localIp_, nonce_, cfg_.internalPort, mappedPort_ ? mappedPort_ : suggestPort_, 0, 0));
            else Send(p, BuildPmpMapRequest(p, cfg_.internalPort, 0, 0));   // RFC 6886 section 3.4: the external port is zero on a delete
            break;
        default: break;
        }
    }
}

bool NatPmp::PublicEnough(uint32_t ip) const { return ip != 0 && (ip >> 24) != 127 && (ip >> 28) < 0xE; }

void NatPmp::NoteEpoch(uint32_t epoch, uint64_t now) {
    if (haveEpoch_ && EpochRestarted(epoch_, epoch, SinceMs(now, epochAt_))) {
        Log("the router restarted (epoch %u after %u): mapping again", epoch, epoch_);
        if (state_ == MAPPED) renewAtMs_ = now;   // its table is gone: ask again at once
    }
    haveEpoch_ = true;
    epoch_ = epoch;
    epochAt_ = now;
}

void NatPmp::Mapped(const Reply& r, uint64_t now, const char* how) {
    if (!r.extPort) return Fail("the router gave external port 0");
    if (!r.lifetime) return Fail("the router granted a lease of 0 seconds");
    const bool again = state_ == RENEWING;
    const uint16_t oldPort = mappedPort_;
    mappedPort_ = r.extPort;
    suggestPort_ = r.extPort;
    lease_ = r.lifetime;
    state_ = MAPPED;
    mayExist_ = true;
    polling_ = false;
    const uint64_t half = uint64_t(lease_) * 500;
    renewAtMs_ = now + (half < 1000 ? 1000 : half);
    leaseEndMs_ = now + uint64_t(lease_) * 1000;
    pollAtMs_ = now + cfg_.pollMs;
    char a[16], b[16];
    if (again) {
        ++renewals_;
        if (oldPort != mappedPort_) Log("renewed: the external port changed %u -> %u", unsigned(oldPort), unsigned(mappedPort_));
        else Log("renewed (lease %u s)", unsigned(lease_));
    } else {
        Log("mapped UDP %s:%u -> %s:%u (lease %u s) via %s", Ip4(extIp_, a), unsigned(mappedPort_), Ip4(localIp_, b), unsigned(cfg_.internalPort),
            unsigned(lease_), how);
    }
}

void NatPmp::StartNatpmpMapping(uint64_t now) {
    proto_ = PROTO_NATPMP;
    state_ = MAPPING;
    BeginExchange(now, cfg_.mapMs);
}

void NatPmp::OnProbeReply(const Reply& r, uint64_t now) {
    switch (r.kind) {
    case K_PCP_MAP: {
        if (!pcpOk_) return;
        if (r.result == 1) {   // PCP-format "unsupported version" (no nonce to check)
            pcpUnsupported_ = true;
            return;
        }
        if (memcmp(r.nonce, nonce_, sizeof nonce_) != 0 || r.intPort != cfg_.internalPort) return;
        if (r.result == 0) {
            if (!r.extIpOk) return Fail("the router gave an IPv6 address (not supported)");
            if (!PublicEnough(r.extIp)) return Fail("the router reports no usable public address");
            extIp_ = r.extIp;
            proto_ = PROTO_PCP;
            NoteEpoch(r.epoch, now);
            Mapped(r, now, "PCP");
        } else if (r.result == 11 && suggestPort_ != 0 && !retried11_) {
            retried11_ = true;
            suggestPort_ = 0;
            Log("PCP: that external port is not available: letting the router choose");
        } else {
            Fail(PcpResultText(r.result));
        }
        return;
    }
    case K_PMP_MAP:   // a version-0 answer to our version-2 request: a NAT-PMP-only server (RFC 6886 section 3.5)
        if (r.result != 0 && pcpOk_) pcpUnsupported_ = true;
        return;
    case K_PMP_ADDR:
        if (r.result != 0) {
            natErr_ = int(r.result);
        } else if (!PublicEnough(r.extIp)) {
            natErr_ = 100;
        } else if (!natSeen_) {
            natSeen_ = true;
            natSeenAt_ = now;
            extIp_ = r.extIp;
            NoteEpoch(r.epoch, now);
        }
        return;
    default: return;
    }
}

void NatPmp::OnMapReply(const Reply& r, uint64_t now) {
    const bool renewing = state_ == RENEWING;
    char why[96];
    if (proto_ == PROTO_NATPMP) {
        if (r.kind != K_PMP_MAP) return;
        if (r.result == 0 && r.intPort != cfg_.internalPort) return;   // not about our mapping
        if (r.result != 0) {
            snprintf(why, sizeof why, "%s", PmpResultText(r.result));
        } else {
            NoteEpoch(r.epoch, now);
            return Mapped(r, now, "NAT-PMP");
        }
    } else {
        if (r.kind != K_PCP_MAP || memcmp(r.nonce, nonce_, sizeof nonce_) != 0 || r.intPort != cfg_.internalPort) return;
        if (r.result == 0) {
            if (r.extIpOk && PublicEnough(r.extIp) && r.extIp != extIp_) {
                char a[16], b[16];
                Log("external address changed %s -> %s", Ip4(extIp_, a), Ip4(r.extIp, b));
                extIp_ = r.extIp;
            }
            NoteEpoch(r.epoch, now);
            return Mapped(r, now, "PCP");
        }
        if (r.result == 11 && suggestPort_ != 0 && !retried11_ && !renewing) {
            retried11_ = true;
            suggestPort_ = 0;
            return;
        }
        snprintf(why, sizeof why, "%s", PcpResultText(r.result));
    }
    if (!renewing) return Fail(why);
    // a renewal that was refused: the mapping may still stand until its lease ends; try again later
    Log("renewal refused: %s", why);
    const uint64_t left = leaseEndMs_ > now ? leaseEndMs_ - now : 0;
    if (!left) return Fail(why);
    state_ = MAPPED;
    renewAtMs_ = now + (left / 2 < 30000 ? 30000 : left / 2);
}

void NatPmp::OnReply(const Reply& r, uint64_t now) {
    switch (state_) {
    case PROBING: OnProbeReply(r, now); break;
    case MAPPING:
    case RENEWING: OnMapReply(r, now); break;
    case MAPPED:
        if (!polling_) break;
        if (proto_ == PROTO_NATPMP && r.kind == K_PMP_ADDR && r.result == 0) {
            if (PublicEnough(r.extIp) && r.extIp != extIp_) {
                char a[16], b[16];
                Log("external address changed %s -> %s", Ip4(extIp_, a), Ip4(r.extIp, b));
                extIp_ = r.extIp;
                renewAtMs_ = now;   // the mapping may not stand on the new address
            }
            NoteEpoch(r.epoch, now);
            polling_ = false;
            pollAtMs_ = now + cfg_.pollMs;
        } else if (proto_ == PROTO_PCP && r.kind == K_PCP_ANNOUNCE && r.result == 0) {
            NoteEpoch(r.epoch, now);
            polling_ = false;
            pollAtMs_ = now + cfg_.pollMs;
        }
        break;
    case REMOVING: {
        // the answer to the delete (lifetime 0); an answer to the earlier create (lifetime > 0) does not count
        const bool ours = proto_ == PROTO_PCP ? (r.kind == K_PCP_MAP && memcmp(r.nonce, nonce_, sizeof nonce_) == 0 && r.intPort == cfg_.internalPort)
                                              : r.kind == K_PMP_MAP;
        if (ours && (r.result != 0 || r.lifetime == 0)) {
            Log("mapping deleted (%s)", r.result ? (proto_ == PROTO_PCP ? PcpResultText(r.result) : PmpResultText(r.result)) : "ok");
            state_ = REMOVED;
        }
        break;
    }
    default: break;
    }
}

void NatPmp::Receive(uint64_t now) {
    uint8_t buf[1500];
    for (int i = 0; i < kMaxPerStep && udp_ && (state_ == PROBING || state_ == MAPPING || state_ == MAPPED || state_ == RENEWING || state_ == REMOVING); ++i) {
        Addr from;
        const int n = udp_->RecvFrom(&from, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (state_ == REMOVING) state_ = REMOVED;
            else Fail("the network socket broke");
            return;
        }
        if (from != gw_) continue;   // RFC 6886 section 3.2.1: only the gateway's address and port 5351
        Reply r;
        if (!ParseReply(buf, size_t(n), &r)) continue;
        answered_ = true;
        OnReply(r, now);
    }
}

void NatPmp::Step(uint64_t now) {
    if (!udp_ || Finished()) return;
    Receive(now);
    switch (state_) {
    case PROBING:
        if (natSeen_ && (!pcpOk_ || pcpUnsupported_ || now >= natSeenAt_ + kNatpmpGraceMs)) {
            StartNatpmpMapping(now);
        } else if (natErr_ >= 0 && (!pcpOk_ || pcpUnsupported_)) {
            Fail(natErr_ == 100 ? "the router reports no usable public address" : PmpResultText(unsigned(natErr_)));
        } else if (now >= deadline_) {
            Fail(natErr_ >= 0 ? (natErr_ == 100 ? "the router reports no usable public address" : PmpResultText(unsigned(natErr_)))
                              : "the router answers neither NAT-PMP nor PCP");
        } else {
            SendRound(now);
        }
        break;
    case MAPPING:
        if (now >= deadline_) Fail("the router did not answer the mapping request");
        else SendRound(now);
        break;
    case RENEWING:
        if (now >= deadline_) {
            const uint64_t left = leaseEndMs_ > now ? leaseEndMs_ - now : 0;
            if (!left) return Fail("the mapping ran out: the router did not answer the renewal");
            Log("renewal got no answer: trying again later");
            state_ = MAPPED;
            renewAtMs_ = now + (left / 2 < 30000 ? 30000 : left / 2);
        } else {
            SendRound(now);
        }
        break;
    case MAPPED:
        if (now >= leaseEndMs_) return Fail("the mapping ran out");
        if (now >= renewAtMs_) {
            state_ = RENEWING;
            polling_ = false;
            BeginExchange(now, cfg_.mapMs);
            SendRound(now);
        } else if (polling_) {
            if (now >= deadline_) {
                polling_ = false;
                pollAtMs_ = now + cfg_.pollMs;
            } else {
                SendRound(now);
            }
        } else if (now >= pollAtMs_) {
            polling_ = true;
            BeginExchange(now, kPollBudgetMs);
            SendRound(now);
        }
        break;
    case REMOVING:
        if (now >= deadline_) {
            Log("mapping delete got no answer (it runs out with its lease)");
            state_ = REMOVED;
        } else {
            SendRound(now);
        }
        break;
    default: break;
    }
}

void NatPmp::Remove(uint64_t now) {
    switch (state_) {
    case MAPPED:
    case RENEWING:
        break;                                  // proto_ is known
    case MAPPING: proto_ = PROTO_NATPMP; break;   // the request may have landed
    case PROBING:
        if (pcpOk_ && sendIdx_ > 0) {           // a PCP request went out: it may have made a mapping
            proto_ = PROTO_PCP;
            break;
        }
        state_ = REMOVED;
        return;
    default: return;
    }
    polling_ = false;
    state_ = REMOVING;
    BeginExchange(now, kRemoveMs);
    SendRound(now);
}

}  // namespace NpNet
