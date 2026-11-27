// Galaxy 3.0 multiplayer (port side, owner: core): in-process loopback network for unit tests and mp_bot --loopback.
// N endpoints (consoles) in one process; each endpoint opens sockets on ports. Datagrams sent through the hub are impaired as
// configured (loss, duplicates, delay + jitter = reordering, bandwidth cap per sender) and become receivable once due on the
// hub's clock (injectable: tests use virtual time).
#ifndef PORT_MP_TRANSPORT_LOOP_H
#define PORT_MP_TRANSPORT_LOOP_H

#include "mp_transport.h"

#include <mutex>

struct MpLoopImpairment {
    float loss;              // 0..1 probability a datagram is lost
    float dup;               // 0..1 probability it is delivered twice
    uint32_t delayMs;        // fixed one-way delay
    uint32_t jitterMs;       // extra random 0..jitterMs (reorders)
    uint32_t bytesPerSec;    // per-sender bandwidth cap (0 = none); a datagram queued more than 1 s behind is dropped
};

class MpLoopSocket;

class MpLoopHub {
public:
    enum { kMaxSockets = 32, kMaxQueued = 4096 };
    MpLoopHub();
    ~MpLoopHub();
    void setClock(uint64_t (*nowMs)(void*), void* ctx);
    void setImpairment(const MpLoopImpairment& imp, uint32_t seed);
    uint64_t now() const;
    // Open a socket for endpoint `host` (1..) on `port`. Null when the port is taken or the hub is full. Owned by the hub until
    // closeSocket (or the hub dies).
    MpLoopSocket* open(uint32_t host, uint16_t port);
    void closeSocket(MpLoopSocket* s);
    // Raw injection (fuzz tests): deliver a datagram from any address, unimpaired, due now.
    void inject(const MpAddr& from, const MpAddr& to, const void* data, size_t len);
    // Cut an endpoint off (it neither sends nor receives): simulates a console that vanished.
    void setDown(uint32_t host, bool down);
    // The endpoint's link reports itself lost (MpTransport::lost): Wi-Fi off / sleep as the transport sees it.
    void setLost(uint32_t host, bool lost);
    bool isLost(uint32_t host) const;
    // fix3 (tests): the endpoint's connector fails its next poll with `err` (rejoinable: as the online connector after the service
    // dropped the console from the room); one failure per call. takeFailure is the connector's side.
    void failConnector(uint32_t host, MpUiError err, bool rejoinable);
    bool takeFailure(uint32_t host, MpUiError* err, bool* rejoinable);
    // core2 (tests): the stand-in for the online service's room view: slot -> (address, public player code); code null clears.
    // Every console of the hub sees the same view (MpConnector::memberCode).
    void setMemberCode(int slot, uint32_t host, const char* code);
    bool memberCode(int slot, uint32_t* host, char out[10]) const;
    // svcfix3 (tests): the same for the label of a shared name (members[].label); null clears
    void setMemberLabel(int slot, uint32_t host, const char* label);
    bool memberLabel(int slot, uint32_t* host, char out[17]) const;
    // Optional tap: every datagram accepted for sending (before impairment). Tests capture corpora with it.
    void setTap(void (*tap)(void* ctx, const MpAddr& from, const MpAddr& to, const void* data, size_t len), void* ctx);
    // Optional filter (tests): return false to lose this datagram (before impairment).
    void setFilter(bool (*filter)(void* ctx, const MpAddr& from, const MpAddr& to, const void* data, size_t len), void* ctx);
    uint64_t sent() const { std::lock_guard<std::recursive_mutex> g(m_mu); return m_sent; }
    uint64_t dropped() const { std::lock_guard<std::recursive_mutex> g(m_mu); return m_dropped; }

    // used by MpLoopSocket
    int sendFrom(MpLoopSocket* s, const MpAddr& to, const void* data, size_t len);
    int recvFor(MpLoopSocket* s, MpAddr* from, void* buffer, size_t capacity);

private:
    // (tsan) mp_bot --loopback runs several MpCores, each with its own helper thread, on one hub: every entry point takes this.
    // Recursive: a tap / filter callback may call back into the hub (counters, setDown).
    mutable std::recursive_mutex m_mu;
    struct Item {
        MpAddr from, to;
        uint64_t due;
        uint32_t order;
        int next;             // next item queued for the same socket, -1 = end
        uint16_t len;
        uint8_t data[1500];
    };
    struct Busy { uint32_t host; uint64_t until; };
    MpLoopSocket* m_sockets[kMaxSockets];
    Item* m_items;            // kMaxQueued slots
    int m_free;               // free list head
    int m_head[kMaxSockets];  // per-socket queue (unsorted; recv picks the earliest due)
    uint32_t m_order;
    uint64_t (*m_now)(void*);
    void* m_nowCtx;
    MpLoopImpairment m_imp;
    uint32_t m_rng;
    Busy m_busy[kMaxSockets];
    uint32_t m_down[16];
    int m_downCount;
    uint32_t m_lostHosts[16];
    int m_lostCount;
    struct Failure { uint32_t host; MpUiError err; bool rejoinable; };
    Failure m_fail[8];
    int m_failCount = 0;
    uint32_t m_codeHost[4] = {0, 0, 0, 0};
    char m_code[4][10] = {};
    uint32_t m_labelHost[4] = {0, 0, 0, 0};
    char m_label[4][17] = {};
    void (*m_tap)(void*, const MpAddr&, const MpAddr&, const void*, size_t);
    void* m_tapCtx;
    bool (*m_filter)(void*, const MpAddr&, const MpAddr&, const void*, size_t);
    void* m_filterCtx;
    uint64_t m_sent, m_dropped;
    float uniform();
    bool isDown(uint32_t host) const;
    void enqueue(int sock, const MpAddr& from, const MpAddr& to, const void* data, size_t len, uint64_t due);
    void route(const MpAddr& from, const MpAddr& to, const void* data, size_t len, uint64_t due);
};

class MpLoopSocket : public MpTransport {
public:
    MpLoopSocket(MpLoopHub* hub, const MpAddr& addr) : m_hub(hub), m_addr(addr) {}
    int send(const MpAddr& to, const void* data, size_t len) override { return m_hub->sendFrom(this, to, data, len); }
    int recv(MpAddr* from, void* buffer, size_t capacity) override { return m_hub->recvFor(this, from, buffer, capacity); }
    MpAddr local() const override { return m_addr; }
    bool lost() const override { return m_hub->isLost(m_addr.host); }
    const char* name() const override { return "loopback"; }
private:
    MpLoopHub* m_hub;
    MpAddr m_addr;
};

#endif  // PORT_MP_TRANSPORT_LOOP_H
