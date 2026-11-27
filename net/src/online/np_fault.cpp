// See np_fault.h.
#include "np_fault.h"

#include <stdlib.h>

#include <deque>
#include <vector>

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif

namespace NpNet {

FaultConfig FaultConfig::FromEnvironment() {
    FaultConfig c;
    if (const char* v = getenv("SMG_NET_DROP")) c.dropPct = atof(v);
    if (const char* v = getenv("SMG_NET_DUP")) c.dupPct = atof(v);
    if (const char* v = getenv("SMG_NET_DELAY_MS")) c.delayMs = uint32_t(atoi(v));
    if (const char* v = getenv("SMG_NET_JITTER_MS")) c.jitterMs = uint32_t(atoi(v));
    if (c.dropPct < 0) c.dropPct = 0;
    if (c.dropPct > 90) c.dropPct = 90;
    if (c.dupPct < 0) c.dupPct = 0;
    if (c.dupPct > 100) c.dupPct = 100;
    if (c.delayMs > 5000) c.delayMs = 5000;
    if (c.jitterMs > 5000) c.jitterMs = 5000;
    return c;
}

namespace {

class FaultUdp : public Udp {
public:
    FaultUdp(Udp* inner, const FaultConfig& c, uint64_t (*now)(void*), void* ctx, uint32_t seed)
        : inner_(inner), cfg_(c), now_(now), ctx_(ctx), rng_(seed ? seed : 0x9E3779B9u), sent_(0), dropped_(0), duped_(0) {
        NPN_LOG("fault injection on: drop %.1f%% dup %.1f%% delay %u ms jitter %u ms\n", c.dropPct, c.dupPct, (unsigned)c.delayMs, (unsigned)c.jitterMs);
    }
    ~FaultUdp() override {
        NPN_LOG("fault injection: %u sent, %u dropped, %u duplicated\n", NETFIX_U32C(sent_), NETFIX_U32C(dropped_), NETFIX_U32C(duped_));
        delete inner_;
    }
    bool Open(uint16_t port) override { return inner_->Open(port); }
    void Close() override {
        held_.clear();
        inner_->Close();
    }
    bool IsOpen() const override { return inner_->IsOpen(); }
    int SendTo(const Addr& to, const void* data, size_t len) override {
        Release();
        ++sent_;
        if (cfg_.dropPct > 0 && Uniform() * 100.0 < cfg_.dropPct) { ++dropped_; return int(len); }
        const int copies = (cfg_.dupPct > 0 && Uniform() * 100.0 < cfg_.dupPct) ? 2 : 1;
        if (copies == 2) ++duped_;
        for (int c = 0; c < copies; ++c) {
            const uint32_t delay = cfg_.delayMs + (cfg_.jitterMs ? uint32_t(Uniform() * cfg_.jitterMs) : 0);
            if (delay == 0) {
                inner_->SendTo(to, data, len);
            } else {
                Held h;
                h.due = now_(ctx_) + delay;
                h.to = to;
                h.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
                // keep ordered by due time: jitter reorders
                size_t pos = held_.size();
                while (pos > 0 && held_[pos - 1].due > h.due) --pos;
                held_.insert(held_.begin() + pos, h);
            }
        }
        return int(len);
    }
    int RecvFrom(Addr* from, void* buffer, size_t capacity) override {
        Release();
        return inner_->RecvFrom(from, buffer, capacity);
    }
    uint32_t LocalIp() override { return inner_->LocalIp(); }
    uint32_t GatewayIp() override { return inner_->GatewayIp(); }
    uint16_t LocalPort() override { return inner_->LocalPort(); }

private:
    struct Held {
        uint64_t due;
        Addr to;
        std::vector<uint8_t> data;
    };
    double Uniform() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return double(rng_) / 4294967296.0;
    }
    void Release() {
        if (held_.empty()) return;
        const uint64_t now = now_(ctx_);
        while (!held_.empty() && held_.front().due <= now) {
            inner_->SendTo(held_.front().to, &held_.front().data[0], held_.front().data.size());
            held_.pop_front();
        }
    }
    Udp* inner_;
    FaultConfig cfg_;
    uint64_t (*now_)(void*);
    void* ctx_;
    uint32_t rng_;
    uint64_t sent_, dropped_, duped_;
    std::deque<Held> held_;
};

}  // namespace

Udp* CreateFaultUdp(Udp* inner, const FaultConfig& config, uint64_t (*now)(void*), void* nowCtx, uint32_t seed) {
    return new FaultUdp(inner, config, now, nowCtx, seed);
}

}  // namespace NpNet
