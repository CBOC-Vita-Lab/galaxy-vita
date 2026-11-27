// Galaxy 3.0 multiplayer (port side, owner: core): MpTransport on a VLink socket, and the Same Wi-Fi / Nearby connector.
//   Vita: the radio / Wi-Fi bring-up belongs to net/ (weak hook mp_net_link_poll, see mp_transport.h); MpCore only opens sockets.
//   Desktop: no hook: a POSIX UDP VLink is made here. SMG_MP_UDP_PORT (default 41100), SMG_MP_UDP_BCAST ("host:port,..."; default
//   255.255.255.255:<port>), SMG_MP_UDP_VIP (default random 10.x.y.z). Two processes on one machine: different ports, and each
//   lists the other's port in SMG_MP_UDP_BCAST.
#include "mp_transport_vlink.h"

#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if MP_HAVE_VLINK
#include "vlink.h"

namespace {

class VLinkSocket : public MpTransport {
public:
    VLinkSocket(VLink::Link* link, int handle, const char* name) : m_link(link), m_handle(handle), m_name(name ? name : "vlink") {}
    ~VLinkSocket() override { if (m_link && m_handle > 0) m_link->Close(m_handle); }
    int send(const MpAddr& to, const void* data, size_t len) override {
        return m_link->SendTo(m_handle, to.host == MP_ADDR_BROADCAST ? VLink::kBroadcastVip : to.host, to.port, data, len);
    }
    int recv(MpAddr* from, void* buffer, size_t capacity) override {
        if (!m_link->IsUp()) return -1;
        uint32_t vip = 0;
        uint16_t port = 0;
        const int n = m_link->RecvFrom(m_handle, buffer, capacity, &vip, &port);
        if (n < 0) return 0;   // nothing queued
        if (from) {
            from->host = vip;
            from->port = port;
        }
        return n == 0 ? 0 : n;   // (an empty datagram carries nothing for us)
    }
    MpAddr local() const override {
        MpAddr a;
        a.host = m_link->LocalVip();
        a.port = m_link->LocalPort(m_handle);
        return a;
    }
    bool lost() const override { return m_link->IsLost() || !m_link->IsUp(); }
    const char* name() const override { return m_name; }
private:
    VLink::Link* m_link;
    int m_handle;
    const char* m_name;
};

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

class VLinkConnector : public MpConnector {
public:
    explicit VLinkConnector(MpConn conn)
        : m_conn(conn), m_link(0), m_owned(0), m_backend(0), m_session(0), m_discovery(0), m_viaHook(false), m_failed(0) {}
    ~VLinkConnector() override {
        delete m_session;
        delete m_discovery;
        if (m_owned) {
            m_owned->Stop();
            delete m_owned;
        }
#if !defined(__vita__)
        if (m_backend) VLink::DestroyUdpBackend(m_backend);
#endif
        if (m_viaHook && mp_net_link_release) mp_net_link_release((int)m_conn);
    }
    int poll(uint64_t) override {
        if (m_failed) return m_failed;
        if (m_session) return MP_CONNECTOR_READY;
        if (!m_link) {
            if (mp_net_link_poll) {
                void* l = 0;
                const int r = mp_net_link_poll((int)m_conn, 1, &l);
                m_viaHook = true;
                if (r == 1) return MP_CONNECTOR_WORKING;
                if (r != 2 || !l) {
                    // mpwifi44: only a console that is not connected to an access point is told to check its Wi-Fi settings; a socket, a bind,
                    // the network module or the stack refusing is "something went wrong", and the log names which
                    const char* why = 0;
                    const int kind = mp_net_link_fail ? mp_net_link_fail((int)m_conn, &why) : MP_LINK_FAIL_UNKNOWN;
                    snprintf(m_why, sizeof m_why, "%s", why && *why ? why : "the link provider gave no reason");
                    mp_log("link: %s bring-up failed (%d): %s", name(), r, m_why);
                    return m_failed = -(int)(kind == MP_LINK_FAIL_OTHER ? MP_ERR_INTERNAL : MP_ERR_NO_WIFI);
                }
                m_link = (VLink::Link*)l;
            } else {
#if defined(__vita__)
                mp_log("link: no radio provider in this build");
                snprintf(m_why, sizeof m_why, "no radio provider in this build");
                return m_failed = -(int)MP_ERR_INTERNAL;   // (mpwifi44: not a Wi-Fi question)
#else
                const int port = EnvInt("SMG_MP_UDP_PORT", 41100);
                const char* bcast = getenv("SMG_MP_UDP_BCAST");
                const uint32_t vip = (uint32_t)strtoul(getenv("SMG_MP_UDP_VIP") ? getenv("SMG_MP_UDP_VIP") : "0", 0, 0);
                m_backend = VLink::CreateUdpBackend((uint16_t)port, bcast, vip);
                if (!m_backend) {
                    mp_log("link: udp backend could not be made (port %d)", port);
                    snprintf(m_why, sizeof m_why, "the udp backend could not be made (port %d)", port);
                    return m_failed = -(int)MP_ERR_NO_WIFI;
                }
                m_owned = new VLink::Link();
                m_owned->SetBackend(m_backend);
                m_owned->SetFingerprint(MP_LINK_FINGERPRINT);
                if (!m_owned->Start()) {
                    mp_log("link: udp link did not start (port %d in use?)", port);
                    snprintf(m_why, sizeof m_why, "the udp link did not start (port %d in use?)", port);
                    return m_failed = -(int)MP_ERR_NO_WIFI;
                }
                m_link = m_owned;
                mp_log("link: udp port %d up", port);
#endif
            }
        }
        if (!m_link->IsUp() && !m_link->Start()) {
            snprintf(m_why, sizeof m_why, "the link did not start");
            mp_log("link: %s link did not start", name());
            return m_failed = -(int)MP_ERR_NO_WIFI;
        }
        m_session = mp_vlink_socket_open(m_link, MP_SESSION_PORT, m_conn == MP_CONN_NEARBY ? "nearby" : "wifi");
        m_discovery = mp_vlink_socket_open(m_link, MP_LAN_PORT, "lan");
        if (!m_session || !m_discovery) {
            mp_log("link: sockets could not be bound");
            snprintf(m_why, sizeof m_why, "the link's sockets could not be bound");
            return m_failed = -(int)MP_ERR_INTERNAL;
        }
        return MP_CONNECTOR_READY;
    }
    MpTransport* session() override { return m_session; }
    MpTransport* discovery() override { return m_discovery; }
    const char* name() const override { return m_conn == MP_CONN_NEARBY ? "nearby" : "wifi"; }
    const char* failWhy() const override { return m_why; }
private:
    char m_why[96] = {0};
    MpConn m_conn;
    VLink::Link* m_link;
    VLink::Link* m_owned;
    VLink::Backend* m_backend;
    MpTransport* m_session;
    MpTransport* m_discovery;
    bool m_viaHook;
    int m_failed;
};

}  // namespace

MpTransport* mp_vlink_socket_open(void* link, uint16_t port, const char* name) {
    VLink::Link* l = (VLink::Link*)link;
    if (!l || !l->IsUp()) return 0;
    const int h = l->OpenSocket();
    if (h <= 0) return 0;
    if (!l->Bind(h, port)) {
        l->Close(h);
        return 0;
    }
    return new VLinkSocket(l, h, name);
}

MpConnector* mp_connector_vlink_create(MpConn conn) {
    if (conn != MP_CONN_WIFI && conn != MP_CONN_NEARBY) return 0;
    return new VLinkConnector(conn);
}

#else  // !MP_HAVE_VLINK: net/ is not in this build yet

MpTransport* mp_vlink_socket_open(void*, uint16_t, const char*) { return 0; }
MpConnector* mp_connector_vlink_create(MpConn) {
    mp_log("link: this build has no VLink (net/ missing)");
    return 0;
}

#endif
