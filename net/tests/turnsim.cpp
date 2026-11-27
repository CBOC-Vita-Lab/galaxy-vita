// TURN test server: see turnsim.h.
#include "turnsim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/online/np_hash.h"
#include "../src/online/np_json.h"

namespace NatSim {

namespace {
const uint32_t kCookie = 0x2112A442u;
uint32_t G16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t G32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void P16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void P32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

struct Out {
    std::vector<uint8_t> b;
    Out(uint16_t type, const uint8_t* txid) : b(20) {
        P16(&b[0], type);
        P32(&b[4], kCookie);
        memcpy(&b[8], txid, 12);
    }
    void Attr(uint16_t t, const void* v, size_t l) {
        const size_t o = b.size();
        b.resize(o + 4 + ((l + 3) & ~size_t(3)), 0);
        P16(&b[o], t);
        P16(&b[o + 2], uint32_t(l));
        if (l) memcpy(&b[o + 4], v, l);
        P16(&b[2], uint32_t(b.size() - 20));
    }
    void U32(uint16_t t, uint32_t v) { uint8_t x[4]; P32(x, v); Attr(t, x, 4); }
    void Str(uint16_t t, const std::string& s) { Attr(t, s.data(), s.size()); }
    void Xor(uint16_t t, const Addr& a) {
        uint8_t x[8] = {0, 1};
        P16(x + 2, a.port ^ (kCookie >> 16));
        P32(x + 4, a.ip ^ kCookie);
        Attr(t, x, 8);
    }
    void Integrity(const uint8_t key[16]) {
        const size_t len = b.size();
        P16(&b[2], uint32_t(len - 20 + 24));
        uint8_t mac[20];
        NpNet::HmacSha1(mac, key, 16, &b[0], len);
        b.resize(len + 24);
        P16(&b[len], 0x0008);
        P16(&b[len + 2], 20);
        memcpy(&b[len + 4], mac, 20);
    }
};

bool DecodeXor(const uint8_t* v, size_t l, Addr* a) {
    if (l < 8 || v[1] != 1) return false;
    a->port = uint16_t(G16(v + 2) ^ (kCookie >> 16));
    a->ip = G32(v + 4) ^ kCookie;
    return true;
}
std::string Hex(uint64_t v) {
    char t[24];
    snprintf(t, sizeof t, "%llx", (unsigned long long)v);
    return t;
}
}  // namespace

struct TurnSim::Msg {
    uint16_t type;
    uint8_t txid[12];
    std::string user, realm, nonce;
    bool hasIntegrity, hasLifetime, hasTransport, hasChannel, hasData;
    size_t integrityAt;          // offset of the MESSAGE-INTEGRITY attribute
    uint32_t lifetime, transport;
    uint16_t channel;
    std::vector<Addr> peers;
    const uint8_t* data;
    size_t dataLen;
    Msg() : type(0), hasIntegrity(false), hasLifetime(false), hasTransport(false), hasChannel(false), hasData(false), integrityAt(0), lifetime(0), transport(0), channel(0), data(0), dataLen(0) {}
};

static bool ParseMsg(const std::vector<uint8_t>& d, TurnSim::Msg* m);

TurnSim::TurnSim(const Config& config, SendFn send) : cfg_(config), send_(send), nextPort_(49152) { memset(&stats_, 0, sizeof stats_); }

std::string TurnSim::MakePassword(const std::string& secret, const std::string& username) {
    uint8_t mac[20];
    NpNet::HmacSha1(mac, secret.data(), secret.size(), username.data(), username.size());
    return NpNet::Base64Encode(mac, 20);
}

std::string TurnSim::Nonce(uint64_t now) const { return "n" + Hex(now); }
bool TurnSim::NonceFresh(const std::string& nonce, uint64_t now) const {
    if (nonce.size() < 2 || nonce[0] != 'n') return false;
    const uint64_t at = strtoull(nonce.c_str() + 1, 0, 16);
    return now >= at && now - at <= uint64_t(cfg_.nonceLifeSec) * 1000u;
}

static bool ParseMsg(const std::vector<uint8_t>& d, TurnSim::Msg* m) {
    if (d.size() < 20 || (d[0] & 0xC0) != 0 || G32(&d[4]) != kCookie) return false;
    const size_t body = G16(&d[2]);
    if (20 + body > d.size() || (body & 3)) return false;
    m->type = uint16_t(G16(&d[0]));
    memcpy(m->txid, &d[8], 12);
    size_t o = 20;
    while (o + 4 <= 20 + body) {
        const uint16_t t = uint16_t(G16(&d[o]));
        const size_t l = G16(&d[o + 2]);
        if (o + 4 + l > 20 + body) return false;
        const uint8_t* v = &d[o + 4];
        switch (t) {
        case 0x0006: m->user.assign(reinterpret_cast<const char*>(v), l); break;
        case 0x0014: m->realm.assign(reinterpret_cast<const char*>(v), l); break;
        case 0x0015: m->nonce.assign(reinterpret_cast<const char*>(v), l); break;
        case 0x0008: m->hasIntegrity = true; m->integrityAt = o; break;
        case 0x000D: if (l >= 4) { m->hasLifetime = true; m->lifetime = G32(v); } break;
        case 0x0019: if (l >= 4) { m->hasTransport = true; m->transport = v[0]; } break;
        case 0x000C: if (l >= 2) { m->hasChannel = true; m->channel = uint16_t(G16(v)); } break;
        case 0x0012: { Addr a; if (DecodeXor(v, l, &a)) m->peers.push_back(a); break; }
        case 0x0013: m->hasData = true; m->data = v; m->dataLen = l; break;
        default: break;
        }
        o += 4 + ((l + 3) & ~size_t(3));
    }
    return true;
}

void TurnSim::Sweep(uint64_t now) {
    for (size_t i = 0; i < allocs_.size();) {
        if (now >= allocs_[i].expires) {
            ++stats_.expiredAllocs;
            allocs_.erase(allocs_.begin() + long(i));
        } else {
            ++i;
        }
    }
}

TurnSim::Alloc* TurnSim::FindAlloc(const Addr& listen, const Addr& client) {
    for (size_t i = 0; i < allocs_.size(); ++i)
        if (allocs_[i].client == client && allocs_[i].listen == listen) return &allocs_[i];
    return 0;
}
TurnSim::Alloc* TurnSim::FindRelayed(const Addr& a) {
    for (size_t i = 0; i < allocs_.size(); ++i)
        if (allocs_[i].relayed == a) return &allocs_[i];
    return 0;
}

void TurnSim::SendError(const Addr& listen, const Addr& client, const Msg& m, int code, const char* text, bool challenge, uint64_t now, const uint8_t* key) {
    Out o(uint16_t((m.type & 0x000F) | 0x0110), m.txid);
    uint8_t ec[4 + 64] = {0, 0, uint8_t(code / 100), uint8_t(code % 100)};
    const size_t tl = strlen(text);
    memcpy(ec + 4, text, tl);
    o.Attr(0x0009, ec, 4 + tl);
    if (challenge) {
        o.Str(0x0014, cfg_.realm);
        o.Str(0x0015, Nonce(now));
    }
    if (key) o.Integrity(key);
    send_(listen, client, o.b, now);
}

bool TurnSim::Authenticate(const Addr& listen, const Addr& client, const std::vector<uint8_t>& d, const Msg& m, uint64_t now, std::string* user, uint8_t key[16]) {
    if (!m.hasIntegrity || m.user.empty()) {
        ++stats_.unauthorized;
        SendError(listen, client, m, 401, "Unauthorized", true, now);
        return false;
    }
    if (!NonceFresh(m.nonce, now)) {
        ++stats_.staleNonce;
        SendError(listen, client, m, 438, "Stale Nonce", true, now);
        return false;
    }
    // expiry inside the username
    const size_t colon = m.user.find(':');
    if (cfg_.wallSec && colon != std::string::npos) {
        const uint64_t exp = strtoull(m.user.substr(0, colon).c_str(), 0, 10);
        if (cfg_.wallSec() > exp) {
            ++stats_.expiredCreds;
            SendError(listen, client, m, 401, "Unauthorized", true, now);
            return false;
        }
    }
    const std::string pass = MakePassword(cfg_.secret, m.user);
    const std::string base = m.user + ":" + cfg_.realm + ":" + pass;
    NpNet::Md5(key, base.data(), base.size());
    // MESSAGE-INTEGRITY: HMAC-SHA1 over the message up to the attribute, with the header length counting the attribute and everything before it
    std::vector<uint8_t> c(d.begin(), d.begin() + long(m.integrityAt));
    P16(&c[2], uint32_t(m.integrityAt - 20 + 24));
    uint8_t mac[20];
    NpNet::HmacSha1(mac, key, 16, &c[0], c.size());
    if (memcmp(mac, &d[m.integrityAt + 4], 20) != 0) {
        ++stats_.badIntegrity;
        SendError(listen, client, m, 401, "Unauthorized", true, now);
        return false;
    }
    *user = m.user;
    return true;
}

void TurnSim::DeliverTo(Alloc* to, const Addr& fromRelayed, const uint8_t* data, size_t len, uint64_t now) {
    auto perm = to->perms.find(fromRelayed.ip);
    if (perm == to->perms.end() || now >= perm->second) {
        ++stats_.noPermission;
        return;
    }
    ++stats_.relayedPackets;
    stats_.relayedBytes += len;
    for (size_t i = 0; i < to->channels.size(); ++i) {
        const Channel& c = to->channels[i];
        if (c.peer == fromRelayed && now < c.expires) {
            std::vector<uint8_t> out(4 + ((len + 3) & ~size_t(3)), 0);
            P16(&out[0], c.number);
            P16(&out[2], uint32_t(len));
            memcpy(&out[4], data, len);
            ++stats_.channelOut;
            send_(to->listen, to->client, out, now);
            return;
        }
    }
    static const uint8_t zero[12] = {0};
    uint8_t txid[12];
    memcpy(txid, zero, 12);
    txid[0] = uint8_t(stats_.dataIndOut);
    Out o(0x0017, txid);
    o.Xor(0x0012, fromRelayed);
    o.Attr(0x0013, data, len);
    ++stats_.dataIndOut;
    send_(to->listen, to->client, o.b, now);
}

void TurnSim::Forward(Alloc* from, const Addr& peer, const uint8_t* data, size_t len, uint64_t now) {
    auto perm = from->perms.find(peer.ip);
    if (perm == from->perms.end() || now >= perm->second) {
        ++stats_.noPermission;
        return;
    }
    Alloc* to = FindRelayed(peer);
    if (!to) ++stats_.noTarget;
    if (to) DeliverTo(to, from->relayed, data, len, now);   // (peers outside this server do not exist in the tests)
}

void TurnSim::OnDatagram(const Addr& listen, const Addr& client, const std::vector<uint8_t>& d, uint64_t now) {
    Sweep(now);
    if (d.empty()) return;
    if ((d[0] & 0xC0) == 0x40) {   // ChannelData
        if (d.size() < 4) return;
        Alloc* a = FindAlloc(listen, client);
        if (!a) return;
        const uint16_t ch = uint16_t(G16(&d[0]));
        const size_t len = G16(&d[2]);
        if (4 + len > d.size()) return;
        for (size_t i = 0; i < a->channels.size(); ++i)
            if (a->channels[i].number == ch && now < a->channels[i].expires) {
                ++stats_.channelIn;
                Forward(a, a->channels[i].peer, &d[4], len, now);
                return;
            }
        ++stats_.unknownChannel;
        return;
    }
    Msg m;
    if (!ParseMsg(d, &m)) return;
    const uint16_t method = uint16_t(m.type & 0x000F);
    const uint16_t cls = uint16_t(m.type & 0x0110);
    if (m.type == 0x0001) {   // Binding request: the server is also a STUN server
        Out o(0x0101, m.txid);
        o.Xor(0x0020, client);
        send_(listen, client, o.b, now);
        return;
    }
    if (m.type == 0x0016 && m.hasData && !m.peers.empty()) {   // Send indication
        Alloc* a = FindAlloc(listen, client);
        if (a) {
            ++stats_.sendInd;
            Forward(a, m.peers[0], m.data, m.dataLen, now);
        }
        return;
    }
    if (cls != 0 || method < 3 || method > 9) return;   // requests only from here
    std::string user;
    uint8_t key[16];
    if (!Authenticate(listen, client, d, m, now, &user, key)) return;
    Alloc* a = FindAlloc(listen, client);
    if (method == 0x0003) {   // Allocate
        if (a) {
            if (memcmp(a->txid, m.txid, 12) == 0 && !a->okResponse.empty()) send_(listen, client, a->okResponse, now);   // a retransmission
            else { ++stats_.mismatch; SendError(listen, client, m, 437, "Allocation Mismatch", false, now, key); }
            return;
        }
        if (!m.hasTransport || m.transport != 17) { SendError(listen, client, m, 442, "Unsupported Transport Protocol", false, now, key); return; }
        if (cfg_.refuseAll) { SendError(listen, client, m, 486, "Allocation Quota Reached", false, now, key); return; }
        if (cfg_.maxAllocations < 0 || (cfg_.maxAllocations > 0 && int(allocs_.size()) >= cfg_.maxAllocations)) { SendError(listen, client, m, 508, "Insufficient Capacity", false, now, key); return; }
        Alloc n;
        n.client = client;
        n.listen = listen;
        n.relayed = Addr(listen.ip, nextPort_++);
        n.user = user;
        uint32_t life = m.hasLifetime ? m.lifetime : cfg_.maxLifetimeSec;
        if (life > cfg_.maxLifetimeSec) life = cfg_.maxLifetimeSec;
        n.expires = now + uint64_t(life) * 1000u;
        memcpy(n.txid, m.txid, 12);
        Out o(0x0103, m.txid);
        o.Xor(0x0016, n.relayed);
        o.U32(0x000D, life);
        o.Xor(0x0020, client);
        o.Integrity(key);
        n.okResponse = o.b;
        allocs_.push_back(n);
        ++stats_.allocations;
        send_(listen, client, o.b, now);
        return;
    }
    if (!a) { SendError(listen, client, m, 437, "Allocation Mismatch", false, now, key); ++stats_.mismatch; return; }
    if (a->user != user) { SendError(listen, client, m, 441, "Wrong Credentials", false, now, key); return; }
    Out ok(uint16_t(0x0100 | method), m.txid);
    switch (method) {
    case 0x0004: {   // Refresh
        uint32_t life = m.hasLifetime ? m.lifetime : cfg_.maxLifetimeSec;
        if (life > cfg_.maxLifetimeSec) life = cfg_.maxLifetimeSec;
        if (m.hasLifetime && m.lifetime == 0) {
            ++stats_.releases;
            ok.U32(0x000D, 0);
            ok.Integrity(key);
            send_(listen, client, ok.b, now);
            for (size_t i = 0; i < allocs_.size(); ++i)
                if (&allocs_[i] == a) { allocs_.erase(allocs_.begin() + long(i)); break; }
            return;
        }
        a->expires = now + uint64_t(life) * 1000u;
        ++stats_.refreshes;
        ok.U32(0x000D, life);
        break;
    }
    case 0x0008:   // CreatePermission
        if (m.peers.empty()) { SendError(listen, client, m, 400, "Bad Request", false, now, key); return; }
        for (size_t i = 0; i < m.peers.size(); ++i) a->perms[m.peers[i].ip] = now + uint64_t(cfg_.permLifeSec) * 1000u;
        ++stats_.permissions;
        break;
    case 0x0009: {   // ChannelBind
        if (!m.hasChannel || m.peers.empty() || m.channel < 0x4000 || m.channel > 0x7FFE) { SendError(listen, client, m, 400, "Bad Request", false, now, key); return; }
        Channel* mine = 0;
        for (size_t i = 0; i < a->channels.size(); ++i) {
            Channel& c = a->channels[i];
            const bool live = now < c.expires;
            if (live && c.number == m.channel && !(c.peer == m.peers[0])) { SendError(listen, client, m, 400, "Channel Bound To Other Peer", false, now, key); return; }
            if (live && c.peer == m.peers[0] && c.number != m.channel) { SendError(listen, client, m, 400, "Peer Bound To Other Channel", false, now, key); return; }
            if (c.number == m.channel) mine = &c;
        }
        if (!mine) {
            Channel c;
            c.number = m.channel;
            a->channels.push_back(c);
            mine = &a->channels.back();
        }
        mine->peer = m.peers[0];
        mine->expires = now + uint64_t(cfg_.bindLifeSec) * 1000u;
        a->perms[m.peers[0].ip] = now + uint64_t(cfg_.permLifeSec) * 1000u;
        ++stats_.bindings;
        break;
    }
    default:
        return;
    }
    ok.Integrity(key);
    send_(listen, client, ok.b, now);
}

}  // namespace NatSim
