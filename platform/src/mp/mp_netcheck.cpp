// Galaxy 3.0 multiplayer (beta diagnostics): "Check Connection". See port/mp_diag.h.
//   1. STUN (the existing client, np_stun) from a socket of its own: the NAT behaviour (open / cone / symmetric / blocked / unknown) and the round trip.
//   2. With an online account: the relay loopback. POST /v1/turn/check (credentials, no room), allocate with the existing TURN client (np_turn), allow
//      this console's own public address (CreatePermission through BindPeer), send a few packets from a SECOND local socket to the relayed address, take
//      them back as Data indications on the first socket, measure the round trip, free the allocation.
// A non-blocking state machine polled once a frame by the menu; it never waits and always ends with a result (the relay part may be "skipped").
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct NcHostAllocScope {
    NcHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~NcHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define NC_HOST_ALLOC NcHostAllocScope ncHostAllocScope_

#define MP_DIAG_DEFINES_STRONG 1   // (this unit defines the mp_diag_* functions: strong, see port/mp_diag.h)
#include "port/mp_diag.h"

#include "mp_config.h"
#include "mp_core_sys.h"
#include "mp_diag_int.h"
#include "mp_service.h"
#include "port/mp_bridge.h"

#include "np_json.h"
#include "np_net.h"
#include "np_stun.h"
#include "np_turn.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

using namespace NpNet;

struct Dns {   // one host name, resolved on a short-lived thread (DNS blocks); the check and the thread each hold a reference
    char host[96];
    uint32_t ips[4];
    volatile int n;
    volatile int done;
    volatile int refs;
};
void* DnsMain(void* p) {
    Dns* d = static_cast<Dns*>(p);
    uint32_t ips[4] = {0, 0, 0, 0};
    const int n = ResolveIpv4(d->host, ips, 4);
    memcpy(d->ips, ips, sizeof ips);
    d->n = n > 0 ? n : 0;
    __atomic_store_n(&d->done, 1, __ATOMIC_RELEASE);
    if (__atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) == 0) free(d);
    return 0;
}
Dns* DnsStart(const char* host) {
    Dns* d = static_cast<Dns*>(calloc(1, sizeof(Dns)));
    if (!d) return nullptr;
    snprintf(d->host, sizeof d->host, "%s", host);
    d->refs = 2;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 128 * 1024);
    const int rc = pthread_create(&t, &at, DnsMain, d);
    pthread_attr_destroy(&at);
    if (rc != 0) { d->refs = 1; d->done = 1; d->n = 0; }
    return d;
}
void DnsDrop(Dns* d) {
    if (d && __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) == 0) free(d);
}

uint64_t NcNow(void*) { return mp_sys_now_ms(); }

enum St { ST_STUN_DNS, ST_STUN, ST_REQ, ST_TURN_DNS, ST_ALLOC, ST_BSTUN, ST_BIND, ST_LOOP, ST_DONE };
constexpr int kProbes = 5;
constexpr uint32_t kProbeGapMs = 150, kLoopWaitMs = 3000, kStunMaxMs = 6000, kDnsMaxMs = 4000, kAllocMaxMs = 8000, kBindMaxMs = 5000, kBStunMaxMs = 2500, kReqMaxMs = 15000;
const uint8_t kMagic[8] = {'S', 'M', 'G', 'C', 'H', 'K', '1', 0};

struct Chk {
    St st = ST_STUN_DNS;
    Udp* a = nullptr;
    Udp* b = nullptr;
    Env env, envB;
    StunClient stun, stunB;   // stunB: socket B's own binding request (its public address can differ from A's behind a NAT that pools addresses)
    Addr pubA;
    int bSource = 0;   // 0 not asked / no answer, 1 same public IP as A, 2 different (then that IP gets a permission too)
    TurnClient turn;
    Dns* dns = nullptr;
    uint64_t t0 = 0, stAt = 0, firstAnswerAt = 0, lastSendAt = 0;
    uint16_t stunPort = 3478;
    bool stunSkipped = false;
    Addr stunSrv[2];
    int nStunSrv = 0;
    Addr mapped;   // our public address as STUN saw it
    MpDiagCheck res;
    // relay
    char user[160], pass[160];
    char turnHost[96];
    uint16_t turnPorts[4];
    int nTurnPorts = 0;
    Addr turnSrv[4];
    int nTurnSrv = 0;
    Addr relayed;
    int sent = 0, got = 0;
    uint64_t sentAt[kProbes];
    uint64_t rttSum = 0;
    bool relayStarted = false, requestPending = false;
};
Chk* g = nullptr;

void Free() {
    if (!g) return;
    if (g->turn.GetState() == TurnClient::READY) g->turn.Release();   // (two Refresh(0) datagrams: best effort)
    if (g->a) { g->a->Close(); delete g->a; }
    if (g->b) { g->b->Close(); delete g->b; }
    DnsDrop(g->dns);
    mp_service_post_cancel();
    delete g;
    g = nullptr;
}

void Finish(const char* relay, const char* err) {
    snprintf(g->res.relay, sizeof g->res.relay, "%s", relay);
    if (err && *err && !g->res.err[0]) snprintf(g->res.err, sizeof g->res.err, "%s", err);
    g->res.valid = true;
    g->st = ST_DONE;
    mp_diag_store_check(&g->res);
    mp_log("diag: check nat=%s stun_ms=%d relay=%s relay_rtt_ms=%d err=%s probe_src=%s", g->res.nat, g->res.stunMs, g->res.relay, g->res.relayRttMs, g->res.err[0] ? g->res.err : "-",
           g->bSource == 1 ? "same" : (g->bSource == 2 ? "different" : "unknown"));
}

// "host:port" (port 3478 when absent): the first entry of the list
void SplitHostPort(const char* list, char* host, size_t cap, uint16_t* port) {
    char copy[160];
    snprintf(copy, sizeof copy, "%s", list);
    char* comma = strchr(copy, ',');
    if (comma) *comma = 0;
    unsigned p = 3478;
    char* colon = strrchr(copy, ':');
    if (colon) { *colon = 0; p = unsigned(atoi(colon + 1)); }
    snprintf(host, cap, "%s", copy);
    *port = (p > 0 && p < 65536) ? uint16_t(p) : 3478;
}

void StartRelayPart() {
    g->relayStarted = true;
    if (!mp_service_enabled() || !mp_diag_has_account()) { Finish("skipped", ""); return; }
    if (!mp_service_post_begin("/v1/turn/check", "{}", 2, kReqMaxMs)) { Finish("skipped", "service"); return; }
    g->st = ST_REQ;
    g->stAt = mp_sys_now_ms();
}

// "turn:host:port?transport=udp" -> host, port
bool ParseTurnUrl(const char* url, char* host, size_t cap, uint16_t* port) {
    if (strncmp(url, "turn:", 5) != 0 || strstr(url, "transport=tcp")) return false;
    const char* p = url + 5;
    const char* end = p;
    while (*end && *end != '?') ++end;
    const char* colon = nullptr;
    for (const char* q = p; q < end; ++q) if (*q == ':') colon = q;
    const char* he = colon ? colon : end;
    if (he == p || size_t(he - p) + 1 > cap) return false;
    memcpy(host, p, size_t(he - p));
    host[he - p] = 0;
    const int pr = colon ? atoi(colon + 1) : 3478;
    if (pr <= 0 || pr > 65535) return false;
    *port = uint16_t(pr);
    return true;
}

void StunDone() {
    const uint32_t local = g->a->LocalIp();
    const int answered = g->stun.Answered();
    if (answered == 0) {
        snprintf(g->res.nat, sizeof g->res.nat, "blocked");
        snprintf(g->res.err, sizeof g->res.err, "stun_no_answer");
    } else {
        g->mapped = g->stun.FirstMapped();
        const int c = g->stun.Classify(local);
        snprintf(g->res.nat, sizeof g->res.nat, "%s", c == NAT_OPEN ? "open" : c == NAT_CONE ? "cone" : c == NAT_SYMMETRIC ? "symmetric" : "unknown");
        if (g->firstAnswerAt) g->res.stunMs = int(g->firstAnswerAt - g->t0);
    }
    StartRelayPart();
}

void OnTurnAnswer(int status, const char* body) {
    if (status < 200 || status >= 300) {
        const char* why = status == 429 ? "check_limit" : status == 503 ? "unavailable" : status == 404 ? "no_endpoint" : status == 401 || status == 403 ? "refused" : "http";
        Finish("skipped", why);
        return;
    }
    Json doc;
    if (!doc.Parse(body, strlen(body))) { Finish("skipped", "bad_answer"); return; }
    const int root = doc.Root();
    const int servers = doc.Get(root, "servers");
    g->turnHost[0] = 0;
    for (int i = 0; i < doc.Size(servers); ++i) {
        const int sv = doc.At(servers, i);
        const std::string& user = doc.GetStr(sv, "username");
        if (user.empty()) continue;   // the plain STUN entry
        snprintf(g->user, sizeof g->user, "%s", user.c_str());
        snprintf(g->pass, sizeof g->pass, "%s", doc.GetStr(sv, "credential").c_str());
        const int urls = doc.Get(sv, "urls");
        for (int u = 0; u < doc.Size(urls) && g->nTurnPorts < 4; ++u) {
            char host[96];
            uint16_t port = 0;
            if (!ParseTurnUrl(doc.Str(doc.At(urls, u)).c_str(), host, sizeof host, &port)) continue;
            if (!g->turnHost[0]) snprintf(g->turnHost, sizeof g->turnHost, "%s", host);
            else if (strcmp(g->turnHost, host) != 0) continue;   // (one host: its ports)
            g->turnPorts[g->nTurnPorts++] = port;
        }
        break;
    }
    if (!g->turnHost[0] || !g->user[0]) { Finish("skipped", "bad_answer"); return; }
    Addr lit;
    if (ParseAddr(g->turnHost, 0, &lit)) {
        for (int i = 0; i < g->nTurnPorts; ++i) g->turnSrv[g->nTurnSrv++] = Addr(lit.ip, g->turnPorts[i]);
        g->st = ST_TURN_DNS;   // (resolved: the next poll starts the allocation)
        return;
    }
    g->dns = DnsStart(g->turnHost);
    g->st = ST_TURN_DNS;
    g->stAt = mp_sys_now_ms();
}

void StartAlloc() {
    TurnConfig tc;
    tc.lifetimeSec = 120;
    g->turn.Start(&g->env, g->turnSrv, g->nTurnSrv, g->user, g->pass, tc);
    memset(g->pass, 0, sizeof g->pass);
    g->st = ST_ALLOC;
    g->stAt = mp_sys_now_ms();
}

// Everything that arrives on socket A: STUN answers (the STUN stage), TURN answers and Data indications (the relay stages).
void Drain() {
    uint8_t buf[1500];
    for (int i = 0; i < 32; ++i) {
        Addr from;
        const int n = g->a->RecvFrom(&from, buf, sizeof buf);
        if (n <= 0) break;
        if (Classify(buf, size_t(n)) != PKT_STUN) continue;
        if (g->st == ST_STUN) {
            const bool was = g->stun.Answered() > 0;
            g->stun.HandlePacket(from, buf, size_t(n));
            if (!was && g->stun.Answered() > 0) g->firstAnswerAt = mp_sys_now_ms();
        } else if (g->st >= ST_ALLOC && g->st <= ST_LOOP) {
            const uint8_t* inner = nullptr;
            size_t il = 0;
            Addr peer;
            const int r = g->turn.HandlePacket(from, buf, size_t(n), &inner, &il, &peer);
            if (r == 2 && g->st == ST_LOOP && il >= 9 && !memcmp(inner, kMagic, 8) && inner[8] < kProbes) {
                const int seq = inner[8];
                if (g->sentAt[seq]) {
                    g->rttSum += mp_sys_now_ms() - g->sentAt[seq];
                    g->sentAt[seq] = 0;   // (a duplicate does not count twice)
                    ++g->got;
                }
            }
        }
    }
}

}  // namespace

extern "C" bool mp_diag_check_begin(void) {
    NC_HOST_ALLOC;
    Free();
    g = new Chk();
    g->env.now = NcNow;
    g->env.random = SystemRandom;
    g->t0 = mp_sys_now_ms();
    g->stAt = g->t0;
    memset(&g->res, 0, sizeof g->res);
    snprintf(g->res.nat, sizeof g->res.nat, "unknown");
    snprintf(g->res.relay, sizeof g->res.relay, "skipped");
    g->res.stunMs = -1;
    g->res.relayRttMs = -1;
    g->a = CreatePlatformUdp();
    if (!g->a || !g->a->Open(0)) {
        snprintf(g->res.err, sizeof g->res.err, "socket");
        g->res.valid = true;
        g->st = ST_DONE;
        mp_diag_store_check(&g->res);
        return true;   // (the result says it)
    }
    g->env.udp = g->a;
    const char* list = getenv("SMG_MP_STUN");
    if (!list) list = "stun.cloudflare.com:3478";
    if (!list || !*list || !strcmp(list, "0") || (!getenv("SMG_MP_STUN") && !mp_config()->stun)) {
        // the player's mp.cfg says no request to a third party: the NAT stays unknown, the relay part can still run
        g->stunSkipped = true;
        snprintf(g->res.err, sizeof g->res.err, "stun_off");
        StartRelayPart();
        return true;
    }
    char host[96];
    SplitHostPort(list, host, sizeof host, &g->stunPort);
    Addr lit;
    if (ParseAddr(host, g->stunPort, &lit)) {
        g->stunSrv[g->nStunSrv++] = lit;
        g->stun.Start(&g->env, g->stunSrv, g->nStunSrv);
        g->st = ST_STUN;
        g->stAt = mp_sys_now_ms();
    } else {
        g->dns = DnsStart(host);
        g->st = ST_STUN_DNS;
    }
    return true;
}

extern "C" int mp_diag_check_poll(MpDiagCheck* out) {
    NC_HOST_ALLOC;
    if (!g) return MP_DIAG_FAILED;
    const uint64_t now = mp_sys_now_ms();
    switch (g->st) {
    case ST_STUN_DNS:
        if (__atomic_load_n(&g->dns->done, __ATOMIC_ACQUIRE)) {
            if (g->dns->n <= 0) {
                snprintf(g->res.err, sizeof g->res.err, "dns");
                StartRelayPart();
                break;
            }
            // two destinations tell a cone NAT from a symmetric one: the host's second address, else the same address on the other port of the service
            g->stunSrv[0] = Addr(g->dns->ips[0], g->stunPort);
            g->stunSrv[1] = g->dns->n > 1 ? Addr(g->dns->ips[1], g->stunPort) : Addr(g->dns->ips[0], g->stunPort == 3478 ? 53 : g->stunPort);
            g->nStunSrv = g->stunSrv[0] == g->stunSrv[1] ? 1 : 2;
            g->stun.Start(&g->env, g->stunSrv, g->nStunSrv);
            g->st = ST_STUN;
            g->stAt = now;
        } else if (now - g->stAt > kDnsMaxMs) {
            snprintf(g->res.err, sizeof g->res.err, "dns");
            StartRelayPart();
        }
        break;
    case ST_STUN:
        Drain();
        g->stun.Tick();
        if (g->stun.Done() || now - g->stAt > kStunMaxMs) StunDone();
        break;
    case ST_REQ: {
        int transport = 2, status = 0;
        char body[2048];
        const int r = mp_service_post_poll(&transport, &status, body, sizeof body);
        if (r == 0) { if (now - g->stAt > kReqMaxMs) { mp_service_post_cancel(); Finish("skipped", "service"); } break; }
        if (r < 0 || transport != 0) { Finish("skipped", "service"); break; }
        OnTurnAnswer(status, body);
        break;
    }
    case ST_TURN_DNS:
        if (g->nTurnSrv > 0) { StartAlloc(); break; }   // (an address literal)
        if (g->dns && __atomic_load_n(&g->dns->done, __ATOMIC_ACQUIRE)) {
            if (g->dns->n <= 0) { Finish("fail", "turn_dns"); break; }
            for (int i = 0; i < g->nTurnPorts; ++i) g->turnSrv[g->nTurnSrv++] = Addr(g->dns->ips[0], g->turnPorts[i]);
            StartAlloc();
        } else if (now - g->stAt > kDnsMaxMs) {
            Finish("fail", "turn_dns");
        }
        break;
    case ST_ALLOC:
        Drain();
        g->turn.Tick();
        if (g->turn.GetState() == TurnClient::READY) {
            g->relayed = g->turn.Relayed();
            Addr pub = g->mapped.IsZero() ? g->turn.Mapped() : g->mapped;   // this console's own public address (STUN's; the relay's view when STUN gave none)
            if (pub.IsZero() || g->relayed.IsZero()) { Finish("fail", "no_address"); break; }
            // CreatePermission for our own public IP (the ChannelBind to an unused port of it comes with it; the loopback packets arrive as Data indications)
            g->turn.BindPeer(0, Addr(pub.ip, 9));
            g->b = CreatePlatformUdp();
            if (!g->b || !g->b->Open(0)) { Finish("fail", "socket"); break; }
            g->pubA = pub;
            if (g->nStunSrv > 0 && !g->stunSkipped) {
                // the probes leave from socket B: ask STUN what B's public address is (it can be another IP than A's: then it needs its own permission)
                g->envB = g->env;
                g->envB.udp = g->b;
                g->stunB.Start(&g->envB, g->stunSrv, 1);
                g->st = ST_BSTUN;
            } else {
                g->st = ST_BIND;
            }
            g->stAt = now;
        } else if (g->turn.GetState() == TurnClient::FAILED) {
            Finish("fail", g->turn.AuthRefused() ? "auth" : "alloc");
        } else if (now - g->stAt > kAllocMaxMs) {
            Finish("fail", "alloc_timeout");
        }
        break;
    case ST_BSTUN: {
        Drain();
        g->turn.Tick();
        uint8_t buf[1500];
        for (int i = 0; i < 8; ++i) {
            Addr from;
            const int n = g->b->RecvFrom(&from, buf, sizeof buf);
            if (n <= 0) break;
            g->stunB.HandlePacket(from, buf, size_t(n));
        }
        g->stunB.Tick();
        if (g->stunB.Done() || now - g->stAt > kBStunMaxMs) {
            const Addr mb = g->stunB.FirstMapped();
            g->bSource = mp_diag_probe_source(g->pubA.ip, mb.ip, g->stunB.Answered() > 0 && !mb.IsZero());
            if (g->bSource == 2) g->turn.BindPeer(1, Addr(mb.ip, 9));   // (a second permission: for B's own public IP)
            g->st = ST_BIND;
            g->stAt = now;
        }
        break;
    }
    case ST_BIND:
        Drain();
        g->turn.Tick();
        if (g->turn.ChannelBound(0) && (g->bSource != 2 || g->turn.ChannelBound(1))) { g->st = ST_LOOP; g->stAt = now; g->lastSendAt = 0; }
        else if (now - g->stAt > kBindMaxMs) Finish("fail", "permission");
        break;
    case ST_LOOP:
        Drain();
        g->turn.Tick();
        if (g->sent < kProbes && (g->lastSendAt == 0 || now - g->lastSendAt >= kProbeGapMs)) {
            uint8_t pkt[24];
            memset(pkt, 0, sizeof pkt);
            memcpy(pkt, kMagic, 8);
            pkt[8] = uint8_t(g->sent);
            g->sentAt[g->sent] = now ? now : 1;
            g->b->SendTo(g->relayed, pkt, sizeof pkt);
            ++g->sent;
            g->lastSendAt = now;
        }
        if (g->got >= kProbes || (g->sent >= kProbes && now - g->lastSendAt > kLoopWaitMs)) {
            if (g->got > 0) {
                g->res.relayRttMs = int(g->rttSum / uint64_t(g->got));
                Finish("ok", "");
            } else {
                Finish("fail", "no_loopback");
            }
        }
        break;
    case ST_DONE:
        break;
    }
    if (g->st != ST_DONE) return MP_DIAG_PENDING;
    if (out) *out = g->res;
    const int r = MP_DIAG_OK;
    Free();   // (the allocation is released, the sockets closed; the result is kept by mp_diag_store_check)
    return r;
}

extern "C" void mp_diag_check_cancel(void) {
    NC_HOST_ALLOC;
    Free();
}
