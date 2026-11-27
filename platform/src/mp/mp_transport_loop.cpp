// Galaxy 3.0 multiplayer (port side, owner: core): in-process loopback network. See mp_transport_loop.h.
#include "mp_transport_loop.h"

#include "mp_core_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
uint64_t RealNow(void*) { return mp_sys_now_ms(); }
}

MpLoopHub::MpLoopHub()
    : m_items(0), m_free(-1), m_order(0), m_now(RealNow), m_nowCtx(0), m_rng(0x12345678u), m_downCount(0), m_lostCount(0), m_tap(0), m_tapCtx(0), m_filter(0), m_filterCtx(0),
      m_sent(0), m_dropped(0) {
    memset(m_sockets, 0, sizeof m_sockets);
    memset(&m_imp, 0, sizeof m_imp);
    memset(m_busy, 0, sizeof m_busy);
    memset(m_down, 0, sizeof m_down);
    for (int i = 0; i < kMaxSockets; i++) m_head[i] = -1;
    m_items = (Item*)calloc(kMaxQueued, sizeof(Item));
    if (m_items) {
        for (int i = 0; i < kMaxQueued; i++) m_items[i].next = i + 1 < kMaxQueued ? i + 1 : -1;
        m_free = 0;
    }
}

MpLoopHub::~MpLoopHub() {
    for (int i = 0; i < kMaxSockets; i++) delete m_sockets[i];
    free(m_items);
}

void MpLoopHub::setClock(uint64_t (*nowMs)(void*), void* ctx) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    m_now = nowMs ? nowMs : RealNow;
    m_nowCtx = ctx;
}

void MpLoopHub::setImpairment(const MpLoopImpairment& imp, uint32_t seed) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    m_imp = imp;
    m_rng = seed ? seed : 1;
}

uint64_t MpLoopHub::now() const {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    return m_now(m_nowCtx);
}

float MpLoopHub::uniform() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return (float)(m_rng >> 8) / 16777216.0f;
}

MpLoopSocket* MpLoopHub::open(uint32_t host, uint16_t port) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (host == 0 || host == MP_ADDR_BROADCAST) return 0;
    int freeIdx = -1;
    for (int i = 0; i < kMaxSockets; i++) {
        if (!m_sockets[i]) {
            if (freeIdx < 0) freeIdx = i;
            continue;
        }
        const MpAddr a = m_sockets[i]->local();
        if (a.host == host && a.port == port) return 0;
    }
    if (freeIdx < 0) return 0;
    MpAddr a;
    a.host = host;
    a.port = port;
    m_sockets[freeIdx] = new MpLoopSocket(this, a);
    m_head[freeIdx] = -1;
    return m_sockets[freeIdx];
}

void MpLoopHub::closeSocket(MpLoopSocket* s) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    for (int i = 0; i < kMaxSockets; i++) {
        if (m_sockets[i] != s) continue;
        for (int it = m_head[i]; it >= 0;) {
            const int next = m_items[it].next;
            m_items[it].next = m_free;
            m_free = it;
            it = next;
        }
        m_head[i] = -1;
        delete s;
        m_sockets[i] = 0;
        return;
    }
}

void MpLoopHub::setDown(uint32_t host, bool down) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    for (int i = 0; i < m_downCount; i++) {
        if (m_down[i] == host) {
            if (!down) m_down[i] = m_down[--m_downCount];
            return;
        }
    }
    if (down && m_downCount < 16) m_down[m_downCount++] = host;
}

void MpLoopHub::setLost(uint32_t host, bool lost) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    for (int i = 0; i < m_lostCount; i++) {
        if (m_lostHosts[i] == host) {
            if (!lost) {
                m_lostHosts[i] = m_lostHosts[--m_lostCount];
                setDown(host, false);
            }
            return;
        }
    }
    if (lost && m_lostCount < 16) m_lostHosts[m_lostCount++] = host;
    setDown(host, lost);
}

bool MpLoopHub::isLost(uint32_t host) const {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    for (int i = 0; i < m_lostCount; i++)
        if (m_lostHosts[i] == host) return true;
    return false;
}

void MpLoopHub::failConnector(uint32_t host, MpUiError err, bool rejoinable) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (m_failCount < 8) m_fail[m_failCount++] = Failure{host, err, rejoinable};
}

void MpLoopHub::setMemberCode(int slot, uint32_t host, const char* code) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (slot < 0 || slot >= 4) return;
    m_codeHost[slot] = code ? host : 0;
    snprintf(m_code[slot], sizeof m_code[slot], "%s", code ? code : "");
}

bool MpLoopHub::memberCode(int slot, uint32_t* host, char out[10]) const {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    out[0] = 0;
    if (slot < 0 || slot >= 4 || !m_code[slot][0]) return false;
    memcpy(out, m_code[slot], 10);
    *host = m_codeHost[slot];
    return true;
}

void MpLoopHub::setMemberLabel(int slot, uint32_t host, const char* label) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (slot < 0 || slot >= 4) return;
    m_labelHost[slot] = label ? host : 0;
    snprintf(m_label[slot], sizeof m_label[slot], "%s", label ? label : "");
}

bool MpLoopHub::memberLabel(int slot, uint32_t* host, char out[17]) const {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    out[0] = 0;
    if (slot < 0 || slot >= 4 || !m_label[slot][0]) return false;
    memcpy(out, m_label[slot], 17);
    *host = m_labelHost[slot];
    return true;
}

bool MpLoopHub::takeFailure(uint32_t host, MpUiError* err, bool* rejoinable) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (m_failCount == 0) return false;
    for (int i = 0; i < m_failCount; i++) {
        if (m_fail[i].host != host) continue;
        *err = m_fail[i].err;
        *rejoinable = m_fail[i].rejoinable;
        for (int k = i + 1; k < m_failCount; k++) m_fail[k - 1] = m_fail[k];
        m_failCount--;
        return true;
    }
    return false;
}

bool MpLoopHub::isDown(uint32_t host) const {
    for (int i = 0; i < m_downCount; i++)
        if (m_down[i] == host) return true;
    return false;
}

void MpLoopHub::setTap(void (*tap)(void*, const MpAddr&, const MpAddr&, const void*, size_t), void* ctx) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    m_tap = tap;
    m_tapCtx = ctx;
}

void MpLoopHub::setFilter(bool (*filter)(void*, const MpAddr&, const MpAddr&, const void*, size_t), void* ctx) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    m_filter = filter;
    m_filterCtx = ctx;
}

void MpLoopHub::enqueue(int sock, const MpAddr& from, const MpAddr& to, const void* data, size_t len, uint64_t due) {
    if (m_free < 0 || len > sizeof m_items[0].data) {
        m_dropped++;
        return;
    }
    const int it = m_free;
    Item& item = m_items[it];
    m_free = item.next;
    item.from = from;
    item.to = to;
    item.due = due;
    item.order = m_order++;
    item.len = (uint16_t)len;
    if (len) memcpy(item.data, data, len);
    item.next = m_head[sock];
    m_head[sock] = it;
}

void MpLoopHub::route(const MpAddr& from, const MpAddr& to, const void* data, size_t len, uint64_t due) {
    for (int i = 0; i < kMaxSockets; i++) {
        MpLoopSocket* s = m_sockets[i];
        if (!s) continue;
        const MpAddr a = s->local();
        if (a.port != to.port || isDown(a.host)) continue;
        if (to.host == MP_ADDR_BROADCAST) {
            if (a.host == from.host) continue;   // a broadcast does not come back to its sender
        } else if (a.host != to.host) {
            continue;
        }
        enqueue(i, from, to, data, len, due);
    }
}

int MpLoopHub::sendFrom(MpLoopSocket* s, const MpAddr& to, const void* data, size_t len) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    if (len > 1500) return -1;
    const MpAddr from = s->local();
    if (isDown(from.host)) return (int)len;   // vanished console: its datagrams go nowhere
    m_sent++;
    if (m_tap) m_tap(m_tapCtx, from, to, data, len);
    if (m_filter && !m_filter(m_filterCtx, from, to, data, len)) {
        m_dropped++;
        return (int)len;
    }
    const uint64_t t = now();
    if (m_imp.loss > 0 && uniform() < m_imp.loss) {
        m_dropped++;
        return (int)len;
    }
    uint64_t base = t;
    if (m_imp.bytesPerSec) {
        int bi = -1;
        for (int i = 0; i < kMaxSockets; i++) {
            if (m_busy[i].host == from.host) { bi = i; break; }
            if (bi < 0 && m_busy[i].host == 0) bi = i;
        }
        if (bi >= 0) {
            m_busy[bi].host = from.host;
            uint64_t start = m_busy[bi].until > t ? m_busy[bi].until : t;
            if (start > t + 1000) {
                m_dropped++;   // the sender's queue is a second behind: tail drop
                return (int)len;
            }
            const uint64_t txMs = ((uint64_t)(len + 28) * 1000u + m_imp.bytesPerSec - 1) / m_imp.bytesPerSec;
            m_busy[bi].until = start + txMs;
            base = m_busy[bi].until;
        }
    }
    const int copies = (m_imp.dup > 0 && uniform() < m_imp.dup) ? 2 : 1;
    for (int c = 0; c < copies; c++) {
        const uint64_t due = base + m_imp.delayMs + (m_imp.jitterMs ? (uint64_t)(uniform() * (float)m_imp.jitterMs) : 0);
        route(from, to, data, len, due);
    }
    return (int)len;
}

void MpLoopHub::inject(const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    route(from, to, data, len, now());
}

int MpLoopHub::recvFor(MpLoopSocket* s, MpAddr* from, void* buffer, size_t capacity) {
    std::lock_guard<std::recursive_mutex> g(m_mu);
    int si = -1;
    for (int i = 0; i < kMaxSockets; i++)
        if (m_sockets[i] == s) { si = i; break; }
    if (si < 0) return -1;
    if (isDown(s->local().host)) return 0;
    const uint64_t t = now();
    int best = -1, bestPrev = -1, prev = -1;
    for (int it = m_head[si]; it >= 0; prev = it, it = m_items[it].next) {
        const Item& item = m_items[it];
        if (item.due > t) continue;
        if (best < 0 || item.due < m_items[best].due || (item.due == m_items[best].due && item.order < m_items[best].order)) {
            best = it;
            bestPrev = prev;
        }
    }
    if (best < 0) return 0;
    Item& item = m_items[best];
    if (bestPrev < 0) m_head[si] = item.next;
    else m_items[bestPrev].next = item.next;
    const size_t n = item.len < capacity ? item.len : capacity;
    if (n) memcpy(buffer, item.data, n);
    if (from) *from = item.from;
    item.next = m_free;
    m_free = best;
    return (int)n;
}

// ---- connector -----------------------------------------------------------------------------------------------------------------
namespace {
class LoopConnector : public MpConnector {
public:
    LoopConnector(MpLoopHub* hub, uint32_t endpoint) : m_hub(hub), m_endpoint(endpoint), m_session(0), m_discovery(0), m_rejoin(false) {}
    ~LoopConnector() override {
        if (m_session) m_hub->closeSocket(m_session);
        if (m_discovery) m_hub->closeSocket(m_discovery);
    }
    int poll(uint64_t) override {
        MpUiError err;
        if (m_hub->takeFailure(m_endpoint, &err, &m_rejoin)) return -(int)err;   // fix3 (tests)
        if (!m_session) m_session = m_hub->open(m_endpoint, MP_SESSION_PORT);
        if (!m_discovery) m_discovery = m_hub->open(m_endpoint, MP_LAN_PORT);
        return m_session && m_discovery ? MP_CONNECTOR_READY : -(int)MP_ERR_INTERNAL;
    }
    MpTransport* session() override { return m_session; }
    MpTransport* discovery() override { return m_discovery; }
    bool rejoinable() override { return m_rejoin; }
    bool memberCode(int slot, uint32_t* vip, char out[10]) override { return m_session && m_hub->memberCode(slot, vip, out); }   // core2 (tests)
    bool memberLabel(int slot, uint32_t* vip, char out[17]) override { return m_session && m_hub->memberLabel(slot, vip, out); }   // svcfix3 (tests)
    const char* name() const override { return "loopback"; }
private:
    MpLoopHub* m_hub;
    uint32_t m_endpoint;
    MpLoopSocket* m_session;
    MpLoopSocket* m_discovery;
    bool m_rejoin;
};
}  // namespace

MpConnector* mp_connector_loop_create(MpLoopHub* hub, uint32_t endpoint) {
    if (!hub || endpoint == 0) return 0;
    return new LoopConnector(hub, endpoint);
}

// core2 (tests): Play by Address over the hub. The host's address is "10.0.<endpoint>:41100" (the low 16 bits of the address are the
// endpoint); a guest's link sends its broadcasts (the first HELLO) to the typed address only, as mp_conn_direct.cpp's backend does.
namespace {
class LoopDirectSocket : public MpTransport {
public:
    LoopDirectSocket(MpLoopSocket* s, uint32_t target) : m_s(s), m_target(target) {}
    int send(const MpAddr& to, const void* data, size_t len) override {
        MpAddr t = to;
        if (to.host == MP_ADDR_BROADCAST) {
            if (!m_target) return (int)len;   // (a host has nowhere to broadcast)
            t.host = m_target;
        }
        return m_s->send(t, data, len);
    }
    int recv(MpAddr* from, void* buffer, size_t capacity) override { return m_s->recv(from, buffer, capacity); }
    MpAddr local() const override { return m_s->local(); }
    bool lost() const override { return m_s->lost(); }
    const char* name() const override { return "loopback-direct"; }
private:
    MpLoopSocket* m_s;
    uint32_t m_target;
};
class LoopDirectConnector : public MpConnector {
public:
    LoopDirectConnector(MpLoopHub* hub, uint32_t endpoint, const MpConnectorParams& p)
        : m_hub(hub), m_endpoint(endpoint), m_p(p), m_sock(0), m_session(0) {}
    ~LoopDirectConnector() override {
        delete m_session;
        if (m_sock) m_hub->closeSocket(m_sock);
    }
    int poll(uint64_t) override {
        MpUiError err;
        bool rj;
        if (m_hub->takeFailure(m_endpoint, &err, &rj)) return -(int)err;
        if (!m_sock) {
            m_sock = m_hub->open(m_endpoint, MP_SESSION_PORT);
            if (m_sock) m_session = new LoopDirectSocket(m_sock, m_p.host ? 0 : (m_p.directIp & 0xFFFFu));
        }
        return m_session ? MP_CONNECTOR_READY : -(int)MP_ERR_INTERNAL;
    }
    MpTransport* session() override { return m_session; }
    MpTransport* discovery() override { return 0; }
    bool addressInfo(MpAddressInfo* out) override {
        if (!out || !m_p.host || !m_session) return false;
        memset(out, 0, sizeof *out);
        snprintf(out->addr, sizeof out->addr, "10.0.%u.%u:%u", (unsigned)((m_endpoint >> 8) & 255), (unsigned)(m_endpoint & 255), 41100u);
        out->source = MP_ADDR_SRC_LAN;
        out->port = 41100;
        return true;
    }
    const char* name() const override { return "loopback-direct"; }
private:
    MpLoopHub* m_hub;
    uint32_t m_endpoint;
    MpConnectorParams m_p;
    MpLoopSocket* m_sock;
    LoopDirectSocket* m_session;
};
}  // namespace

MpConnector* mp_connector_loop_direct_create(MpLoopHub* hub, uint32_t endpoint, const MpConnectorParams& p) {
    if (!hub || endpoint == 0) return 0;
    return new LoopDirectConnector(hub, endpoint, p);
}
