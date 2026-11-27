// STUN client. See np_stun.h.
#include "np_stun.h"

#include <stdio.h>

namespace NpNet {
namespace {
const uint32_t kMagic = 0x2112A442u;
const uint16_t kBindingRequest = 0x0001, kBindingSuccess = 0x0101, kBindingError = 0x0111;
const uint16_t kAttrMapped = 0x0001, kAttrXorMapped = 0x0020;

void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}  // namespace

StunClient::StunClient() : env_(0), state_(IDLE), count_(0), distinct_(false) { memset(servers_, 0, sizeof servers_); }

void StunClient::Reset() {
    state_ = IDLE;
    count_ = 0;
    distinct_ = false;
    memset(servers_, 0, sizeof servers_);
}

size_t StunClient::BuildRequest(uint8_t* out, const uint8_t txid[12]) {
    Put16(out, kBindingRequest);
    Put16(out + 2, 0);              // no attributes
    Put32(out + 4, kMagic);
    memcpy(out + 8, txid, 12);
    return 20;
}

bool StunClient::ParseResponse(const uint8_t* d, size_t len, uint8_t txidOut[12], Addr* mapped) {
    if (len < 20 || (d[0] & 0xC0) != 0 || Get32(d + 4) != kMagic) return false;
    const uint16_t type = Get16(d), alen = Get16(d + 2);
    if (alen % 4 != 0 || 20u + alen > len) return false;
    memcpy(txidOut, d + 8, 12);
    if (type == kBindingError) {
        *mapped = Addr();
        return true;   // ours (matching txid is checked by the caller), but no address
    }
    if (type != kBindingSuccess) return false;
    Addr plain, xored;
    size_t pos = 20;
    while (pos + 4 <= 20u + alen) {
        const uint16_t at = Get16(d + pos), al = Get16(d + pos + 2);
        const size_t padded = (size_t(al) + 3) & ~size_t(3);
        if (pos + 4 + al > 20u + alen) return false;
        const uint8_t* v = d + pos + 4;
        if ((at == kAttrXorMapped || at == kAttrMapped) && al >= 8 && v[1] == 0x01) {   // IPv4 only
            Addr a;
            if (at == kAttrXorMapped) {
                a.port = uint16_t(Get16(v + 2) ^ (kMagic >> 16));
                a.ip = Get32(v + 4) ^ kMagic;
                xored = a;
            } else {
                a.port = Get16(v + 2);
                a.ip = Get32(v + 4);
                plain = a;
            }
        }
        pos += 4 + padded;
    }
    *mapped = !xored.IsZero() ? xored : plain;
    return !mapped->IsZero();
}

void StunClient::Send(Server& s) {
    uint8_t pkt[20];
    const size_t n = BuildRequest(pkt, s.txid);
    env_->udp->SendTo(s.addr, pkt, n);
    const uint32_t wait = cfg_.retransmitMs[s.sends < 8 ? s.sends : 7];
    ++s.sends;
    s.nextMs = env_->Now() + (wait ? wait : 1000);
}

void StunClient::Start(Env* env, const Addr* servers, int count, const StunConfig& config) {
    Reset();
    env_ = env;
    cfg_ = config;
    count_ = count > kMaxStunServers ? kMaxStunServers : count;
    for (int i = 0; i < count_; ++i) {
        servers_[i].addr = servers[i];
        if (!env_->Random(servers_[i].txid, 12)) {   // a predictable transaction id lets anyone forge the answer: no STUN rather than a forged one
            state_ = FAILED;
            return;
        }
    }
    distinct_ = false;
    for (int i = 0; i < count_; ++i)
        for (int j = 0; j < i; ++j)
            if (servers_[i].addr.ip != servers_[j].addr.ip) distinct_ = true;
    if (count_ == 0) {
        state_ = FAILED;
        return;
    }
    state_ = RUNNING;
    for (int i = 0; i < count_; ++i) Send(servers_[i]);
}

void StunClient::Tick() {
    if (state_ != RUNNING) return;
    const uint64_t now = env_->Now();
    bool pending = false;
    for (int i = 0; i < count_; ++i) {
        Server& s = servers_[i];
        if (s.answered || s.gaveUp) continue;
        if (now < s.nextMs) { pending = true; continue; }
        if (s.sends >= 8 || cfg_.retransmitMs[s.sends] == 0) {   // the final wait is over
            s.gaveUp = true;
            continue;
        }
        Send(s);
        pending = true;
    }
    if (!pending) state_ = Answered() > 0 ? DONE : FAILED;
}

bool StunClient::HandlePacket(const Addr& from, const uint8_t* data, size_t len) {
    if (state_ != RUNNING && state_ != DONE) return false;
    uint8_t tx[12];
    Addr mapped;
    if (!ParseResponse(data, len, tx, &mapped)) return false;
    for (int i = 0; i < count_; ++i) {
        Server& s = servers_[i];
        if (memcmp(s.txid, tx, 12) != 0) continue;
        if (from.ip != s.addr.ip || from.port != s.addr.port) return true;   // right transaction, wrong source: ignore
        if (!s.answered && !mapped.IsZero()) {
            s.answered = true;
            s.mapped = mapped;
        } else if (mapped.IsZero()) {
            s.gaveUp = true;   // error response: do not wait for retransmissions
        }
        if (state_ == RUNNING) {
            bool all = true;
            for (int k = 0; k < count_; ++k)
                if (!servers_[k].answered && !servers_[k].gaveUp) all = false;
            if (all) state_ = Answered() > 0 ? DONE : FAILED;
        }
        return true;
    }
    return false;
}

int StunClient::Answered() const {
    int n = 0;
    for (int i = 0; i < count_; ++i)
        if (servers_[i].answered) ++n;
    return n;
}

Addr StunClient::Mapped(int server) const { return server >= 0 && server < count_ && servers_[server].answered ? servers_[server].mapped : Addr(); }

Addr StunClient::FirstMapped() const {
    for (int i = 0; i < count_; ++i)
        if (servers_[i].answered) return servers_[i].mapped;
    return Addr();
}

int StunClient::Classify(uint32_t localIp) const {
    const Addr first = FirstMapped();
    if (first.IsZero()) return NAT_UNKNOWN;
    if (first.ip == localIp) return NAT_OPEN;
    if (Answered() < 2) return NAT_UNKNOWN;
    for (int i = 0; i < count_; ++i)
        if (servers_[i].answered && servers_[i].mapped != first) return NAT_SYMMETRIC;
    return NAT_CONE;
}

}  // namespace NpNet
