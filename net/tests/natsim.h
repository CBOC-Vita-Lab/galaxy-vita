// NAT / internet simulator for the tests of the online transport (no sockets, no threads, virtual or real time as the caller likes).
//
// A small world: hosts on private LANs behind NAT boxes with a public address each, STUN servers on public addresses, and "the
// internet" between them with loss, delay, jitter, reordering and duplication. The NAT boxes follow RFC 4787: the mapping behaviour
// (endpoint-independent / address-dependent / address-and-port-dependent) and the filtering behaviour decide whether a punch works.
// Presets: full cone, address-restricted cone, port-restricted cone, symmetric.
//
// Users: online_test.cpp (in-process, virtual clock, the NAT matrix), natsim_proxy.cpp (two real game processes through UDP on
// loopback: the host's UDP class wraps every datagram, see online/np_udp_posix.cpp).
#ifndef NATSIM_H
#define NATSIM_H

#include <stdint.h>
#include <string.h>

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "../src/online/np_net.h"

namespace NatSim {

using NpNet::Addr;

enum Mapping { MAP_INDEPENDENT = 0, MAP_ADDRESS = 1, MAP_ADDRESS_PORT = 2 };
enum Filtering { FILT_INDEPENDENT = 0, FILT_ADDRESS = 1, FILT_ADDRESS_PORT = 2 };

struct NatType {
    Mapping map;
    Filtering filt;
    bool hairpin;
    bool open;   // no NAT at all (public address on the host)
    NatType(Mapping m = MAP_INDEPENDENT, Filtering f = FILT_ADDRESS_PORT, bool h = false, bool o = false) : map(m), filt(f), hairpin(h), open(o) {}
    static NatType FullCone() { return NatType(MAP_INDEPENDENT, FILT_INDEPENDENT); }
    static NatType AddressRestricted() { return NatType(MAP_INDEPENDENT, FILT_ADDRESS); }
    static NatType PortRestricted() { return NatType(MAP_INDEPENDENT, FILT_ADDRESS_PORT); }
    static NatType Symmetric() { return NatType(MAP_ADDRESS_PORT, FILT_ADDRESS_PORT); }
    // A symmetric-mapping NAT with address-only filtering (some carrier boxes): the peer's real port is found from its first probe.
    static NatType SymmetricAddressFiltered() { return NatType(MAP_ADDRESS_PORT, FILT_ADDRESS); }
    static NatType Open() { return NatType(MAP_INDEPENDENT, FILT_INDEPENDENT, false, true); }
    // "full" "addr" "port" "sym" "symaddr" "open"
    static bool Parse(const char* name, NatType* out);
    const char* Name() const;
};

struct Impair {
    double lossPct;      // 0..100
    double dupPct;
    double reorderPct;   // this share of datagrams gets extra delay (so later ones overtake)
    uint32_t delayMs;    // one-way
    uint32_t jitterMs;
    Impair() : lossPct(0), dupPct(0), reorderPct(0), delayMs(0), jitterMs(0) {}
};

struct SimStats {
    uint64_t sent, delivered, lost, filtered, noRoute, blackholed, stunAnswered, dups;
};

class Sim {
public:
    explicit Sim(uint32_t seed = 1);
    // A LAN behind a NAT box whose public address is publicIp. Returns the lan id.
    int AddLan(uint32_t publicIp, const NatType& nat, uint32_t idleTimeoutMs = 120000);
    // A host on a LAN (private address; for an open NAT the private address is the public one).
    int AddHost(int lan, const Addr& priv);
    // A STUN server at a public address.
    void AddStunServer(const Addr& addr);
    // A server at a public address that sees every datagram sent to it (the relay of the tests): handler(src as seen, data, now).
    void AddServer(const Addr& addr, std::function<void(const Addr&, const std::vector<uint8_t>&, uint64_t)> handler);
    // A server answers: a datagram from `server` to `dst` (a public address) enters the network.
    void ServerSend(const Addr& server, const Addr& dst, const std::vector<uint8_t>& data, uint64_t nowMs);
    void SetImpair(const Impair& i) { impair_ = i; }
    void SetHostLink(int host, bool up);          // false: everything to and from the host is dropped (Wi-Fi lost)
    // Drops all NAT mappings of a LAN (a router reboot / mapping expiry).
    void FlushNat(int lan);

    void Send(int host, const Addr& dst, const void* data, size_t len, uint64_t nowMs);
    // The next datagram due for `host` at `nowMs`.
    bool Recv(int host, Addr* src, std::vector<uint8_t>* data, uint64_t nowMs);
    const SimStats& Stats() const { return stats_; }
    // The public address a host's traffic to `dst` currently maps to (0 when no mapping exists yet).
    Addr MappingFor(int host, const Addr& dst) const;

private:
    struct Mapped {
        Addr internal;
        uint16_t external;
        Addr remote;               // for dependent mappings: the destination the mapping belongs to
        uint64_t lastUse;
        std::set<uint64_t> allowed;   // destinations the inside host sent to: (ip) or (ip,port) keyed
    };
    struct Lan {
        uint32_t publicIp;
        NatType nat;
        uint32_t idleMs;
        std::vector<Mapped> maps;
        uint16_t nextPort;
    };
    struct Host {
        int lan;
        Addr priv;
        bool up;
        std::deque<std::pair<uint64_t, std::pair<Addr, std::vector<uint8_t> > > > inbox;   // sorted by due time on insert
    };
    struct Pending {
        uint64_t due;
        int toHost;
        Addr src;
        std::vector<uint8_t> data;
        bool operator<(const Pending& o) const { return due < o.due; }
    };

    double Uniform();
    uint64_t Delay();
    void Deliver(int host, const Addr& src, const std::vector<uint8_t>& data, uint64_t due);
    Mapped* FindOut(Lan& lan, const Addr& internal, const Addr& dst, uint64_t now);
    Mapped* FindIn(Lan& lan, uint16_t external, uint64_t now);
    static uint64_t Key(Filtering f, const Addr& a);
    int HostByPrivate(int lan, const Addr& a) const;
    int LanByPublic(uint32_t ip) const;
    void SendFromHost(int host, const Addr& dst, const std::vector<uint8_t>& data, uint64_t now);
    void ArriveAtPublic(const Addr& srcSeen, const Addr& dst, const std::vector<uint8_t>& data, uint64_t now, int fromLan);
    void StunReply(const Addr& server, const Addr& srcSeen, const std::vector<uint8_t>& request, int toHost, uint64_t now);

    std::vector<Lan> lans_;
    std::vector<Host> hosts_;
    std::vector<Addr> stun_;
    struct Server {
        Addr addr;
        std::function<void(const Addr&, const std::vector<uint8_t>&, uint64_t)> handler;
    };
    std::vector<Server> servers_;
    Impair impair_;
    uint32_t rng_;
    SimStats stats_;
};

}  // namespace NatSim

#endif
