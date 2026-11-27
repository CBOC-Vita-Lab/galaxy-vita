// NAT / internet simulator. See natsim.h.
#include "natsim.h"

#include <algorithm>

namespace NatSim {

bool NatType::Parse(const char* name, NatType* out) {
    if (!name) return false;
    if (!strcmp(name, "full")) *out = FullCone();
    else if (!strcmp(name, "addr")) *out = AddressRestricted();
    else if (!strcmp(name, "port")) *out = PortRestricted();
    else if (!strcmp(name, "sym")) *out = Symmetric();
    else if (!strcmp(name, "symaddr")) *out = SymmetricAddressFiltered();
    else if (!strcmp(name, "open")) *out = Open();
    else return false;
    return true;
}

const char* NatType::Name() const {
    if (open) return "open";
    if (map == MAP_INDEPENDENT && filt == FILT_INDEPENDENT) return "full-cone";
    if (map == MAP_INDEPENDENT && filt == FILT_ADDRESS) return "address-restricted";
    if (map == MAP_INDEPENDENT && filt == FILT_ADDRESS_PORT) return "port-restricted";
    if (map == MAP_ADDRESS_PORT && filt == FILT_ADDRESS_PORT) return "symmetric";
    if (map == MAP_ADDRESS_PORT && filt == FILT_ADDRESS) return "symmetric-addr-filtered";
    return "custom";
}

Sim::Sim(uint32_t seed) : rng_(seed ? seed : 1) { memset(&stats_, 0, sizeof stats_); }

double Sim::Uniform() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return double(rng_) / 4294967296.0;
}

uint64_t Sim::Delay() {
    double d = impair_.delayMs + Uniform() * impair_.jitterMs;
    if (impair_.reorderPct > 0 && Uniform() * 100.0 < impair_.reorderPct) d += 20 + Uniform() * (2.0 * (impair_.delayMs + impair_.jitterMs) + 30);
    return uint64_t(d);
}

int Sim::AddLan(uint32_t publicIp, const NatType& nat, uint32_t idleTimeoutMs) {
    Lan l;
    l.publicIp = publicIp;
    l.nat = nat;
    l.idleMs = idleTimeoutMs;
    l.nextPort = uint16_t(50000 + 1000 * lans_.size());
    lans_.push_back(l);
    return int(lans_.size()) - 1;
}

int Sim::AddHost(int lan, const Addr& priv) {
    Host h;
    h.lan = lan;
    h.priv = priv;
    h.up = true;
    hosts_.push_back(h);
    return int(hosts_.size()) - 1;
}

void Sim::AddStunServer(const Addr& a) { stun_.push_back(a); }
void Sim::AddServer(const Addr& a, std::function<void(const Addr&, const std::vector<uint8_t>&, uint64_t)> handler) {
    Server s;
    s.addr = a;
    s.handler = handler;
    servers_.push_back(s);
}

void Sim::ServerSend(const Addr& server, const Addr& dst, const std::vector<uint8_t>& data, uint64_t now) { ArriveAtPublic(server, dst, data, now, -1); }

void Sim::SetHostLink(int host, bool up) { hosts_[host].up = up; }
void Sim::FlushNat(int lan) { lans_[lan].maps.clear(); }

uint64_t Sim::Key(Filtering f, const Addr& a) {
    if (f == FILT_INDEPENDENT) return 0;
    if (f == FILT_ADDRESS) return uint64_t(a.ip) | (uint64_t(1) << 40);
    return (uint64_t(a.ip) << 16) | a.port | (uint64_t(1) << 50);
}

int Sim::HostByPrivate(int lan, const Addr& a) const {
    for (size_t i = 0; i < hosts_.size(); ++i)
        if (hosts_[i].lan == lan && hosts_[i].priv == a) return int(i);
    return -1;
}

int Sim::LanByPublic(uint32_t ip) const {
    for (size_t i = 0; i < lans_.size(); ++i)
        if (!lans_[i].nat.open && lans_[i].publicIp == ip) return int(i);
    return -1;
}

Sim::Mapped* Sim::FindOut(Lan& lan, const Addr& internal, const Addr& dst, uint64_t now) {
    for (size_t i = 0; i < lan.maps.size();) {
        if (now - lan.maps[i].lastUse > lan.idleMs) {
            lan.maps.erase(lan.maps.begin() + i);
            continue;
        }
        ++i;
    }
    for (size_t i = 0; i < lan.maps.size(); ++i) {
        Mapped& m = lan.maps[i];
        if (m.internal != internal) continue;
        if (lan.nat.map == MAP_ADDRESS && m.remote.ip != dst.ip) continue;
        if (lan.nat.map == MAP_ADDRESS_PORT && m.remote != dst) continue;
        m.lastUse = now;
        m.allowed.insert(Key(lan.nat.filt, dst));
        return &m;
    }
    Mapped m;
    m.internal = internal;
    m.remote = lan.nat.map == MAP_INDEPENDENT ? Addr() : dst;
    if (lan.nat.map == MAP_ADDRESS) m.remote.port = 0;
    m.lastUse = now;
    uint16_t port = 0;
    if (lan.nat.map == MAP_INDEPENDENT) {
        bool used = false;
        for (size_t i = 0; i < lan.maps.size(); ++i)
            if (lan.maps[i].external == internal.port) used = true;
        if (!used) port = internal.port;   // port preserving while possible
    }
    while (!port) {
        port = lan.nextPort++;
        for (size_t i = 0; i < lan.maps.size(); ++i)
            if (lan.maps[i].external == port) port = 0;
    }
    m.external = port;
    m.allowed.insert(Key(lan.nat.filt, dst));
    lan.maps.push_back(m);
    return &lan.maps.back();
}

Sim::Mapped* Sim::FindIn(Lan& lan, uint16_t external, uint64_t now) {
    for (size_t i = 0; i < lan.maps.size(); ++i) {
        Mapped& m = lan.maps[i];
        if (m.external != external) continue;
        if (now - m.lastUse > lan.idleMs) return 0;
        return &m;
    }
    return 0;
}

Addr Sim::MappingFor(int host, const Addr& dst) const {
    const Host& h = hosts_[host];
    const Lan& l = lans_[h.lan];
    if (l.nat.open) return h.priv;
    for (size_t i = 0; i < l.maps.size(); ++i) {
        const Mapped& m = l.maps[i];
        if (m.internal != h.priv) continue;
        if (l.nat.map == MAP_ADDRESS && m.remote.ip != dst.ip) continue;
        if (l.nat.map == MAP_ADDRESS_PORT && m.remote != dst) continue;
        return Addr(l.publicIp, m.external);
    }
    return Addr();
}

void Sim::Deliver(int host, const Addr& src, const std::vector<uint8_t>& data, uint64_t due) {
    Host& h = hosts_[host];
    if (!h.up) { ++stats_.blackholed; return; }
    std::pair<uint64_t, std::pair<Addr, std::vector<uint8_t> > > item(due, std::make_pair(src, data));
    // keep the inbox ordered by due time (stable)
    size_t pos = h.inbox.size();
    while (pos > 0 && h.inbox[pos - 1].first > due) --pos;
    h.inbox.insert(h.inbox.begin() + pos, item);
    ++stats_.delivered;
}

void Sim::StunReply(const Addr& server, const Addr& srcSeen, const std::vector<uint8_t>& req, int, uint64_t now) {
    // Binding success response with XOR-MAPPED-ADDRESS; routed back like any packet from the server to the mapped address
    std::vector<uint8_t> r(32);
    r[0] = 0x01; r[1] = 0x01;
    r[2] = 0; r[3] = 12;
    r[4] = 0x21; r[5] = 0x12; r[6] = 0xA4; r[7] = 0x42;
    memcpy(&r[8], &req[8], 12);
    r[20] = 0x00; r[21] = 0x20; r[22] = 0; r[23] = 8;
    r[24] = 0; r[25] = 1;
    const uint16_t xp = uint16_t(srcSeen.port ^ 0x2112);
    const uint32_t xi = srcSeen.ip ^ 0x2112A442u;
    r[26] = uint8_t(xp >> 8); r[27] = uint8_t(xp);
    r[28] = uint8_t(xi >> 24); r[29] = uint8_t(xi >> 16); r[30] = uint8_t(xi >> 8); r[31] = uint8_t(xi);
    ++stats_.stunAnswered;
    ArriveAtPublic(server, srcSeen, r, now, -1);
}

void Sim::ArriveAtPublic(const Addr& srcSeen, const Addr& dst, const std::vector<uint8_t>& data, uint64_t now, int fromLan) {
    for (size_t i = 0; i < stun_.size(); ++i) {
        if (stun_[i] != dst) continue;
        if (data.size() >= 20 && data[0] == 0x00 && data[1] == 0x01 && data[4] == 0x21) StunReply(dst, srcSeen, data, -1, now);
        return;
    }
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (servers_[i].addr == dst) {
            servers_[i].handler(srcSeen, data, now);
            return;
        }
    }
    // hosts without NAT
    for (size_t i = 0; i < hosts_.size(); ++i) {
        if (lans_[hosts_[i].lan].nat.open && hosts_[i].priv == dst) {
            Deliver(int(i), srcSeen, data, now + Delay());
            return;
        }
    }
    const int lanId = LanByPublic(dst.ip);
    if (lanId < 0) { ++stats_.noRoute; return; }
    Lan& lan = lans_[lanId];
    if (lanId == fromLan && !lan.nat.hairpin) { ++stats_.noRoute; return; }
    Mapped* m = FindIn(lan, dst.port, now);
    if (!m) { ++stats_.filtered; return; }
    if (lan.nat.filt != FILT_INDEPENDENT && m->allowed.find(Key(lan.nat.filt, srcSeen)) == m->allowed.end()) { ++stats_.filtered; return; }
    const int host = HostByPrivate(lanId, m->internal);
    if (host < 0) { ++stats_.noRoute; return; }
    Deliver(host, srcSeen, data, now + Delay());
}

void Sim::SendFromHost(int host, const Addr& dst, const std::vector<uint8_t>& data, uint64_t now) {
    Host& h = hosts_[host];
    Lan& lan = lans_[h.lan];
    if (!lan.nat.open) {
        // same private network: no NAT in between
        const int peer = HostByPrivate(h.lan, dst);
        if (peer >= 0 && peer != host) {
            Deliver(peer, h.priv, data, now + Delay());
            return;
        }
        if (NpNet::IsPrivateIp(dst.ip)) { ++stats_.noRoute; return; }   // a private address that is not on this LAN
        Mapped* m = FindOut(lan, h.priv, dst, now);
        ArriveAtPublic(Addr(lan.publicIp, m->external), dst, data, now, h.lan);
        return;
    }
    ArriveAtPublic(h.priv, dst, data, now, h.lan);
}

void Sim::Send(int host, const Addr& dst, const void* data, size_t len, uint64_t now) {
    ++stats_.sent;
    if (!hosts_[host].up) { ++stats_.blackholed; return; }
    if (impair_.lossPct > 0 && Uniform() * 100.0 < impair_.lossPct) { ++stats_.lost; return; }
    std::vector<uint8_t> d(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    SendFromHost(host, dst, d, now);
    if (impair_.dupPct > 0 && Uniform() * 100.0 < impair_.dupPct) {
        ++stats_.dups;
        SendFromHost(host, dst, d, now);
    }
}

bool Sim::Recv(int host, Addr* src, std::vector<uint8_t>* data, uint64_t now) {
    Host& h = hosts_[host];
    if (!h.up || h.inbox.empty() || h.inbox.front().first > now) return false;
    *src = h.inbox.front().second.first;
    *data = h.inbox.front().second.second;
    h.inbox.pop_front();
    return true;
}

}  // namespace NatSim
