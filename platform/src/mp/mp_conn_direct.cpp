// Galaxy 3.0 multiplayer (port side, owner: netx): internet play by address, no service needed ("Online" > "Play by Address").
//
// The host opens one UDP socket (port 41100, else the next free of 41101..41107; SMG_MP_DIRECT_PORT overrides), asks its router to
// forward that port (UPnP, mp_portmap.cpp) and learns its public address (the router's GetExternalIPAddress; else a STUN binding
// request to a public STUN server, SMG_MP_STUN, default stun.cloudflare.com:3478, off with "stun = 0" in mp.cfg; else its LAN address). The lobby shows that address
// big and plain. A guest types it ("203.0.113.7" or "203.0.113.7:41101") and its link sends to exactly that address.
//
// Same session path as Same Wi-Fi: a VLink link (frames with Galaxy's magic and link fingerprint, fragmentation, peer learning) on a
// backend of its own over that UDP socket, MpCore's HELLO / WELCOME with the version fingerprint, the reliable channel, heartbeats.
// The guest's HELLO goes out as a link broadcast, which this backend sends to the typed address only; the host learns the guest's
// address from the datagram (its NAT's outside address: no "declared" address is ever trusted) and answers there. Virtual link
// addresses are random 10.x.y.z per link (two consoles behind two routers often have the same LAN address).
// Topology: a star through the host; guests cannot reach each other through their NATs, so there is no host migration: a host that
// leaves ends the session, a silent one is waited for by the reconnect path (MpCore).
#include "mp_portmap.h"
#include "mp_transport_vlink.h"

#include "mp_config.h"
#include "mp_core_sys.h"
#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if MP_HAVE_VLINK
#include "np_net.h"
#include "np_stun.h"
#include "vlink.h"

#include <pthread.h>

namespace NpNet {
bool SystemRandom(void*, void* out, size_t n);   // np_crypto.cpp (getrandom / sceKernelGetRandomNumber)
}

namespace {

const uint16_t kDirectPort = MP_DIRECT_PORT;

// ---- STUN server names: resolved on a short-lived thread of their own (DNS blocks; never the helper or the game thread) -----------
struct DnsJob {
    char list[160];
    NpNet::Addr out[NpNet::kMaxStunServers];
    volatile int count;
    volatile int done;
    volatile int refs;   // the connector and the thread; the last one frees it
};

void* DnsMain(void* p) {
    DnsJob* j = static_cast<DnsJob*>(p);
    int n = 0;
    char copy[160];
    snprintf(copy, sizeof copy, "%s", j->list);
    for (char* item = copy; item && *item && n < NpNet::kMaxStunServers;) {
        char* comma = strchr(item, ',');
        if (comma) *comma = 0;
        char* colon = strrchr(item, ':');
        unsigned port = 3478;
        if (colon) {
            *colon = 0;
            port = unsigned(atoi(colon + 1));
        }
        NpNet::Addr literal;
        if (port > 0 && port < 65536) {
            if (NpNet::ParseAddr(item, uint16_t(port), &literal)) {
                j->out[n++] = literal;
            } else {
                uint32_t ips[2];
                const int got = NpNet::ResolveIpv4(item, ips, 2);
                if (got > 0) j->out[n++] = NpNet::Addr(ips[0], uint16_t(port));
            }
        }
        item = comma ? comma + 1 : 0;
    }
    j->count = n;
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL) == 0) free(j);
    return 0;
}

void DnsRelease(DnsJob* j) {
    if (j && __atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL) == 0) free(j);
}

uint64_t EnvNow(void*) { return mp_sys_now_ms(); }

// ---- the VLink backend: one UDP socket, a target for "broadcasts" (the guest's host), STUN answers taken out ---------------------
struct DirectBackend {
    VLink::Backend b;
    NpNet::Udp* udp;
    uint32_t vip;
    NpNet::Addr target;            // guests: the typed address; host: none (a host never broadcasts on this link)
    NpNet::StunClient* stun;       // answers to our binding requests arrive on the session socket
    uint32_t stray;                // datagrams that were neither ours nor STUN (VLink drops them; counted for the log)
};

void PutPeer(VLink::PeerAddr* p, const NpNet::Addr& a) {
    memset(p, 0, sizeof *p);
    memcpy(p->bytes, &a.ip, 4);
    memcpy(p->bytes + 4, &a.port, 2);
}
NpNet::Addr GetPeer(const VLink::PeerAddr* p) {
    NpNet::Addr a;
    memcpy(&a.ip, p->bytes, 4);
    memcpy(&a.port, p->bytes + 4, 2);
    return a;
}

uint32_t BVip(void* ctx) { return static_cast<DirectBackend*>(ctx)->vip; }

int BSend(void* ctx, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len) {
    DirectBackend& d = *static_cast<DirectBackend*>(ctx);
    if (!d.udp || !d.udp->IsOpen()) return -1;
    if (broadcast || !to) {
        if (d.target.IsZero()) return int(len);   // nobody to broadcast to on the internet
        return d.udp->SendTo(d.target, data, len);
    }
    const NpNet::Addr a = GetPeer(to);
    if (a.ip == 0 || a.port == 0) return -1;
    return d.udp->SendTo(a, data, len);
}

int BRecv(void* ctx, VLink::PeerAddr* from, void* buffer, size_t capacity) {
    DirectBackend& d = *static_cast<DirectBackend*>(ctx);
    if (!d.udp || !d.udp->IsOpen()) return 0;
    for (int i = 0; i < 64; ++i) {
        NpNet::Addr a;
        const int n = d.udp->RecvFrom(&a, buffer, capacity);
        if (n <= 0) return n < 0 ? -1 : 0;
        const uint8_t* p = static_cast<const uint8_t*>(buffer);
        if (d.stun && NpNet::Classify(p, size_t(n)) == NpNet::PKT_STUN) {
            d.stun->HandlePacket(a, p, size_t(n));   // (a STUN answer is never a frame)
            continue;
        }
        if (n < 4 || p[0] != 'S' || p[1] != 'M' || p[2] != 'G') {   // not a Galaxy link frame ("SMGK" / "SMGF"): the internet's noise
            ++d.stray;
            continue;
        }
        PutPeer(from, a);
        return n;
    }
    return 0;
}

void BClose(void*) {}

class DirectConnector : public MpConnector {
public:
    explicit DirectConnector(const MpConnectorParams& p)
        : m_p(p), m_link(0), m_session(0), m_failed(0), m_ready(false), m_dns(0), m_stunStarted(false), m_stunLogged(false),
          m_portmapAsked(false) {
        memset(&m_be, 0, sizeof m_be);
        memset(&m_info, 0, sizeof m_info);
        m_info.portmap = MP_PORTMAP_OFF;
    }
    ~DirectConnector() override {
        delete m_session;
        if (m_link) {
            m_link->Stop();
            delete m_link;
        }
        if (m_be.udp) {
            m_be.udp->Close();
            delete m_be.udp;
        }
        DnsRelease(m_dns);
        if (m_portmapAsked) mp_portmap_release();
        if (m_be.stray) mp_log("direct: %u stray datagram(s) dropped", (unsigned)m_be.stray);
    }

    int poll(uint64_t now) override {
        if (m_failed) return m_failed;
        if (!m_ready) {
            const int r = bringUp();
            if (r != MP_CONNECTOR_READY) return r;
        }
        if (m_p.host) tickAddress(now);
        return MP_CONNECTOR_READY;
    }
    MpTransport* session() override { return m_session; }
    MpTransport* discovery() override { return 0; }
    const char* name() const override { return "direct"; }
    bool addressInfo(MpAddressInfo* out) override {
        if (!out || !m_ready) return false;
        *out = m_info;
        return true;
    }

private:
    int fail(MpUiError e, const char* why) {
        mp_log("direct: %s", why);
        m_failed = -(int)e;
        return m_failed;
    }

    int bringUp() {
        if (!m_be.udp) {
            m_be.udp = NpNet::CreatePlatformUdp();
            uint16_t port = 0;   // guests: any port (the host answers wherever our NAT puts us)
            if (m_p.host) {
                port = kDirectPort;
                if (const char* e = getenv("SMG_MP_DIRECT_PORT")) {
                    const int v = atoi(e);
                    if (v > 0 && v < 65536) port = (uint16_t)v;
                }
            }
            bool opened = m_be.udp && m_be.udp->Open(port);
            if (!opened && m_be.udp && port) {   // mp74: the console refuses every chosen port (0x8041010D): let the system choose; the address carries the port bound
                mp_log("direct: UDP port %u could not be bound (%s): the system chooses one", (unsigned)port, m_be.udp->Error());
                opened = m_be.udp->Open(0);
            }
            if (!opened) {
                char why[160];
                const char* e = m_be.udp ? m_be.udp->Error() : "no socket";
                snprintf(why, sizeof why, "the UDP socket could not be opened (%s)", e);
                // mpwifi44: "check your Wi-Fi settings" only when Wi-Fi is the reason; a busy port or the ad hoc radio in use is not
                // (the socket's own sentence says which; none = not known = as before)
                const bool notWifi = strstr(e, "port is busy") || strstr(e, "ad hoc radio") || !strcmp(e, "no socket");
                return fail(notWifi ? MP_ERR_INTERNAL : MP_ERR_NO_WIFI, why);
            }
        }
        m_be.b.name = "direct";
        m_be.b.ctx = &m_be;
        m_be.b.local_vip = BVip;
        m_be.b.send = BSend;
        m_be.b.recv = BRecv;
        m_be.b.close = BClose;
        m_be.b.maxFrame = NpNet::kMaxDatagram;   // (1200: an internet path's MTU with headroom; VLink splits anything longer)
        // the link address comes from the console's id for this session (corefix 6: not a fixed id): a guest that rejoins after a hiccup comes back with the
        // same one, so the host knows it is the same console (and gives its slot back) even while the old socket's last datagrams arrive
        uint32_t x = m_p.linkId ? m_p.linkId : mp_sys_random();
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        m_be.vip = 0x0A000000u | (x & 0x00FFFFFEu) | 1u;
        if (!m_p.host) m_be.target = NpNet::Addr(m_p.directIp, m_p.directPort);
        m_link = new VLink::Link();
        m_link->SetBackend(&m_be.b);
        m_link->SetFingerprint(MP_LINK_FINGERPRINT);
        if (!m_link->Start()) return fail(MP_ERR_NO_WIFI, "the link did not start");
        m_session = mp_vlink_socket_open(m_link, MP_SESSION_PORT, "direct");
        if (!m_session) return fail(MP_ERR_INTERNAL, "the session socket could not be bound");
        m_ready = true;
        char ip[16];
        const uint32_t lip = m_be.udp->LocalIp();
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", lip >> 24, (lip >> 16) & 255, (lip >> 8) & 255, lip & 255);
        if (m_p.host) {
            mp_log("direct: host on UDP port %u (local %s)", (unsigned)m_be.udp->LocalPort(), ip);
            m_info.port = m_be.udp->LocalPort();   // (no address shown until the router and STUN had their say: a few seconds)
            if (lip) {
                mp_portmap_request(m_be.udp->LocalPort(), lip, "direct host");
                m_portmapAsked = true;
            }
            startDns();
        } else {
            const uint32_t t = m_p.directIp;
            mp_log("direct: joining %u.%u.%u.%u:%u", t >> 24, (t >> 16) & 255, (t >> 8) & 255, t & 255, (unsigned)m_p.directPort);
        }
        return MP_CONNECTOR_READY;
    }

    void startDns() {
        const char* list = getenv("SMG_MP_STUN");
        // corefix 6: the default is a request to a third party (Cloudflare's public STUN server); "stun = 0" in mp.cfg turns it off
        // (the lobby then shows the router's address from UPnP, else the LAN address). SMG_MP_STUN overrides the file.
        if (!list && !mp_config()->stun) {
            mp_log("direct: stun is off (mp.cfg)");
            return;
        }
        if (!list) list = "stun.cloudflare.com:3478";
        if (!*list || !strcmp(list, "0")) return;   // (no STUN)
        DnsJob* j = (DnsJob*)calloc(1, sizeof *j);
        if (!j) return;
        snprintf(j->list, sizeof j->list, "%s", list);
        j->refs = 2;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, DnsMain, j) != 0) {
            free(j);
            j = 0;
        }
        pthread_attr_destroy(&attr);
        m_dns = j;
        m_dnsAt = mp_sys_now_ms();
    }

    void setAddress(uint32_t ip, uint16_t port, uint8_t source, bool cgnat) {
        char text[24] = {0};
        if (ip) snprintf(text, sizeof text, "%u.%u.%u.%u:%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, (unsigned)port);
        const bool changed = strcmp(text, m_info.addr) != 0 || m_info.source != source || m_info.cgnat != cgnat;
        snprintf(m_info.addr, sizeof m_info.addr, "%s", text);
        m_info.source = source;
        m_info.cgnat = cgnat;
        static const char* const kSrc[] = {"none", "lan", "stun", "router"};
        if (changed && source != MP_ADDR_SRC_NONE)
            mp_log("direct: address %s source=%s%s", text, kSrc[source < 4 ? source : 0], cgnat ? " (behind another NAT: not reachable)" : "");
    }

    // Host: what the lobby shows. The router's mapping (its public address and the forwarded port) wins; else the STUN answer with our
    // own port (a manual forward of that port works); else, once both had their say, the LAN address (players on the same network).
    void tickAddress(uint64_t now) {
        // STUN: the server names resolved -> binding requests on the session socket (answers come back through BRecv)
        if (m_dns && !m_stunStarted && __atomic_load_n(&m_dns->done, __ATOMIC_ACQUIRE)) {
            m_stunStarted = true;
            if (m_dns->count > 0) {
                m_env.udp = m_be.udp;
                m_env.now = EnvNow;
                m_env.random = NpNet::SystemRandom;
                m_be.stun = &m_stun;
                m_stun.Start(&m_env, m_dns->out, m_dns->count);
            } else {
                mp_log("direct: stun server name did not resolve");
            }
        } else if (m_dns && !m_stunStarted && now - m_dnsAt > 8000) {
            m_stunStarted = true;   // (the resolver hangs: no STUN; the thread frees the job when it ends)
            mp_log("direct: stun server name did not resolve in time");
        }
        if (m_be.stun) m_stun.Tick();
        const bool stunDone = !m_dns || (m_stunStarted && (!m_be.stun || m_stun.Done()));
        NpNet::Addr stunAddr;
        if (m_be.stun && m_stun.Done()) stunAddr = m_stun.FirstMapped();
        if (stunDone && !m_stunLogged) {
            m_stunLogged = true;
            if (!stunAddr.IsZero())
                mp_log("direct: stun public %u.%u.%u.%u:%u", stunAddr.ip >> 24, (stunAddr.ip >> 16) & 255, (stunAddr.ip >> 8) & 255,
                       stunAddr.ip & 255, (unsigned)stunAddr.port);
            else if (m_be.stun)
                mp_log("direct: stun no answer");
        }
        MpPortmapInfo pm;
        memset(&pm, 0, sizeof pm);
        if (m_portmapAsked) mp_portmap_info(&pm);
        m_info.portmap = pm.state;
        const bool pmDone = !m_portmapAsked || pm.state == MP_PORTMAP_MAPPED || pm.state == MP_PORTMAP_FAILED || pm.state == MP_PORTMAP_OFF;
        if (pm.state == MP_PORTMAP_MAPPED && pm.extIp && !pm.cgnat) {
            setAddress(pm.extIp, pm.extPort, MP_ADDR_SRC_ROUTER, false);
        } else if (!stunAddr.IsZero() && (pmDone || pm.state == MP_PORTMAP_MAPPED)) {
            // a CGNAT router mapped a port that is not reachable: the STUN address is the true public one, the warning stays
            setAddress(stunAddr.ip, m_info.port, MP_ADDR_SRC_STUN, pm.cgnat || (pm.extIp && NpNet::IsPrivateIp(pm.extIp)));
        } else if (pmDone && stunDone) {
            if (pm.extIp && !NpNet::IsPrivateIp(pm.extIp)) setAddress(pm.extIp, m_info.port, MP_ADDR_SRC_ROUTER, false);   // known, not mapped
            else setAddress(m_be.udp->LocalIp(), m_info.port, MP_ADDR_SRC_LAN, pm.cgnat);
        }
    }

    MpConnectorParams m_p;
    DirectBackend m_be;
    VLink::Link* m_link;
    MpTransport* m_session;
    int m_failed;
    bool m_ready;
    DnsJob* m_dns;
    uint64_t m_dnsAt = 0;
    bool m_stunStarted, m_stunLogged;
    NpNet::Env m_env;
    NpNet::StunClient m_stun;
    bool m_portmapAsked;
    MpAddressInfo m_info;
};

}  // namespace

MpConnector* mp_connector_direct_create(const MpConnectorParams& p) {
    if (!p.host && (p.directIp == 0 || p.directPort == 0)) return 0;
    return new DirectConnector(p);
}

// core2: the LAN address the members of a gathering room use to follow it into Play by Address (a throwaway socket's local address)
uint32_t mp_direct_local_ip(void) {
    NpNet::Udp* u = NpNet::CreatePlatformUdp();
    if (!u) return 0;
    uint32_t ip = 0;
    if (u->Open(0)) ip = u->LocalIp();
    u->Close();
    delete u;
    return ip;
}

#else  // !MP_HAVE_VLINK

MpConnector* mp_connector_direct_create(const MpConnectorParams&) {
    mp_log("direct: this build has no VLink (net/ missing)");
    return 0;
}
uint32_t mp_direct_local_ip(void) { return 0; }

#endif

// mp_netglue.cpp references this (strongly) so that the Vita runtime archive links this object; MpCore only knows it weakly.
extern "C" const int mp_conn_direct_anchor = 1;

// "203.0.113.7", "203.0.113.7:41101", "203.0.113.7 41101" (blanks around are ignored). Unicast IPv4 only.
bool mp_direct_parse_address(const char* text, uint32_t* ip, uint16_t* port) {
    if (!text) return false;
    char t[40];
    size_t n = 0;
    while (*text == ' ' || *text == '\t') ++text;
    for (; *text && n + 1 < sizeof t; ++text) t[n++] = *text;
    if (*text) return false;
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) --n;
    t[n] = 0;
    // one blank (or ", ") between address and port works like ':'
    for (size_t i = 0; i < n; ++i)
        if (t[i] == ' ' || t[i] == ',') {
            size_t j = i;
            while (j < n && (t[j] == ' ' || t[j] == ',')) ++j;
            memmove(t + i + 1, t + j, n - j + 1);
            t[i] = ':';
            n -= j - i - 1;
            break;
        }
    unsigned p[4];
    for (int i = 0; i < 4; i++) p[i] = 0;
    size_t pos = 0;
    for (int part = 0; part < 4; part++) {
        int digits = 0;
        unsigned v = 0;
        while (pos < n && t[pos] >= '0' && t[pos] <= '9') {
            v = v * 10 + (unsigned)(t[pos] - '0');
            if (++digits > 3 || v > 255) return false;
            pos++;
        }
        if (!digits) return false;
        p[part] = v;
        if (part < 3) {
            if (pos >= n || t[pos] != '.') return false;
            pos++;
        }
    }
    unsigned prt = MP_DIRECT_PORT;
    if (pos < n) {
        if (t[pos] != ':') return false;
        pos++;
        int digits = 0;
        unsigned v = 0;
        while (pos < n && t[pos] >= '0' && t[pos] <= '9') {
            v = v * 10 + (unsigned)(t[pos] - '0');
            if (++digits > 5) return false;
            pos++;
        }
        if (!digits || pos != n || v == 0 || v > 65535) return false;
        prt = v;
    }
    const uint32_t a = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    if (p[0] == 0 || p[0] >= 224 || a == 0xFFFFFFFFu) return false;   // "this network", multicast, broadcast, reserved
    if (ip) *ip = a;
    if (port) *port = (uint16_t)prt;
    return true;
}
