// Galaxy 3.0 multiplayer (owner: conntest): the Connection Test's real probes and the C API of port/mp_conntest.h. The engine
// (mp_conntest.cpp) is pure; everything that touches the platform is here:
//   Wi-Fi      Vita: sceNetCtl state and RSSI percentage (SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE); desktop: connected, strength unknown
//   Same Wi-Fi the existing LAN discovery (mp_ui_local_scan: the probe broadcast and the answers; -1 = the scan cannot go out)
//   Internet   the service client's info call (mp_service_info_*: GET /v1/info with its own timeout; the answer is parsed there by np_json)
//   Hosting    the router port mapping (mp_portmap: UPnP / NAT-PMP / PCP) and one STUN request (np_stun) from a socket of its own
//   Delay      the info call again (keep-alive: the first call pays the TLS handshake, the next ones are the round trip)
// Game thread only (the C API). The router mapping is pumped by MpCore's helper thread, which this file keeps alive by polling the
// scan while a run goes (nothing else would pump the mapping outside a session). A hostile or broken answer never reaches the engine
// as anything but a small enum or a clamped number.
#define MP_CONNTEST_DEFINES_STRONG 1   // (this unit defines the mp_conntest_* functions: strong, see port/mp_conntest.h)
#include "mp_conntest.h"

#include "mp_config.h"
#include "mp_core_sys.h"
#include "mp_portmap.h"
#include "mp_posix_io.h"   // smallfix2
#include "mp_report.h"
#include "mp_service.h"
#include "mp_transport.h"
#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// flow (heap rule, as mp_core.cpp / mp_gather.cpp): the C API runs on the game thread, where operator new goes to the current
// JKR heap unless a host-allocation scope is open; the probes allocate (the STUN socket and its job), so every entry opens the scope.
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
namespace {
struct CtHostAllocScope {
    CtHostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~CtHostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
};
}  // namespace
#define CT_HOST_ALLOC CtHostAllocScope ctHostAllocScope_

#if defined(__has_include)
#if __has_include("np_stun.h") && __has_include("np_net.h")
#include "np_net.h"
#include "np_stun.h"
#define MP_CT_HAVE_STUN 1
#elif __has_include("online/np_stun.h") && __has_include("online/np_net.h")
#include "online/np_net.h"
#include "online/np_stun.h"
#define MP_CT_HAVE_STUN 1
#endif
#endif

#ifdef __vita__
#include <psp2/net/netctl.h>
#pragma weak sceNetCtlInit
#pragma weak sceNetCtlInetGetState
#pragma weak sceNetCtlInetGetInfo
#endif

#if defined(MP_CT_HAVE_STUN)
#include <pthread.h>
#endif
#include "mp_netctl_guard.h"

// the other modules' functions, weak: a program that links this file without them (a tool, a test) still links; the check then says so
extern "C" {
void mp_portmap_request(uint16_t internalPort, uint32_t localIp, const char* who) __attribute__((weak));
void mp_portmap_release(void) __attribute__((weak));
bool mp_portmap_request_as(uint8_t owner, uint16_t internalPort, uint32_t localIp, const char* who) __attribute__((weak));   // review2 #10: the test is its own owner
void mp_portmap_release_as(uint8_t owner) __attribute__((weak));
void mp_portmap_info(MpPortmapInfo* out) __attribute__((weak));
int mp_ui_local_scan(MpConn conn) __attribute__((weak));
bool mp_service_enabled(void) __attribute__((weak));
bool mp_service_info_begin(uint32_t timeoutMs) __attribute__((weak));
int mp_service_info_poll(MpServiceInfo* out) __attribute__((weak));
void mp_service_info_cancel(void) __attribute__((weak));
// linkfix: net/src/np_https.cpp. Weak: a test program without the transport has none (= the clock is not blamed)
int np_https_last_clock_fault(void) __attribute__((weak));
}

namespace {

// ---- Wi-Fi ------------------------------------------------------------------------------------------------------------------------
bool ProbeWifi(void*, MpCtWifi* out) {
    out->connected = true;
    out->quality = -1;
#ifdef __vita__
    // mpcrash32: the NET module may not be loaded yet (nothing online ran): its stubs jump to 0 then. The test was asked for: load it (what the
    // backends do); a refusal says nothing about the Wi-Fi: the other checks decide
    if (!mp_netctl_live() && sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
    if (!mp_netctl_live()) return false;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (sceNetCtlInit) sceNetCtlInit();   // (already up answers 0x80412102: fine; the state call below says whether it is usable)
    }
    if (!sceNetCtlInetGetState) return false;
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0) return false;   // cannot tell: the other checks decide
    out->connected = state == SCE_NETCTL_STATE_CONNECTED;
    if (out->connected && sceNetCtlInetGetInfo) {
        SceNetCtlInfo info;
        memset(&info, 0, sizeof info);
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0 && info.rssi_percentage <= 100u)
            out->quality = static_cast<int>(info.rssi_percentage);
    }
#endif
    return true;
}

// ---- Same Wi-Fi + keeping the helper thread alive ---------------------------------------------------------------------------------
uint64_t g_lastScan = 0;
int g_lastRooms = 0;
bool g_scanFailed = false;

// The scan is polled about ten times a second (each call also revives MpCore's helper thread, which pumps the router mapping).
void KeepAlive(void*) {
    if (!mp_ui_local_scan || mp_active()) return;
    const uint64_t now = mp_sys_now_ms();
    if (g_lastScan && now - g_lastScan < 100) return;
    g_lastScan = now;
    MpUiStatus st;
    mp_ui_status(&st);
    if (st.phase != MP_UI_IDLE) return;
    const int n = mp_ui_local_scan(MP_CONN_WIFI);
    if (n < 0) g_scanFailed = true;
    else g_lastRooms = n, g_scanFailed = false;
}

int ProbeLocal(void* ctx) {
    MpUiStatus st;
    mp_ui_status(&st);
    if (mp_active() || st.phase != MP_UI_IDLE) return -2;   // a room is open: its link is up by definition
    if (!mp_ui_local_scan) return -1;
    g_lastScan = 0;
    KeepAlive(ctx);
    if (g_scanFailed) return -1;
    return g_lastRooms;
}

// ---- the service -------------------------------------------------------------------------------------------------------------------
bool ProbeConfigured(void*) { return mp_service_enabled && mp_service_enabled(); }
bool InfoBegin(void*, uint32_t timeoutMs) { return mp_service_info_begin && mp_service_info_begin(timeoutMs); }
void InfoCancel(void*) { if (mp_service_info_cancel) mp_service_info_cancel(); }
int InfoPoll(void*, MpCtInfo* out) {
    out->outcome = MPCT_INFO_DOWN;
    out->ms = 0;
    MpServiceInfo info;
    if (!mp_service_info_poll) return 1;
    const int r = mp_service_info_poll(&info);
    if (r == 0) return 0;
    if (r < 0) return 1;   // (nothing running: a failed call)
    out->ms = info.ms;
    if (info.transport == 1) out->outcome = MPCT_INFO_OFFLINE;
    else if (info.transport != 0 || !info.valid) out->outcome = MPCT_INFO_DOWN;
    else if (info.needsNewer) out->outcome = MPCT_INFO_UPDATE;
    else if (info.killed) out->outcome = MPCT_INFO_KILLED;
    else out->outcome = MPCT_INFO_OK;
    return 1;
}

// ---- the router mapping -------------------------------------------------------------------------------------------------------------
int MapBegin(void*) {
    if (!mp_portmap_request || !mp_portmap_info) return 0;
    const char* off = getenv("SMG_MP_UPNP");
    if (off && off[0] == '0') return 0;   // (the player switched port mapping off)
    // (as its own owner: a room hosted on the same port keeps its mapping when the test ends; a room on another port is not replaced: no mapping asked)
    if (!mp_portmap_request_as) { mp_portmap_request(MP_DIRECT_PORT, 0, "conntest"); return 1; }
    return mp_portmap_request_as(MP_PORTMAP_TEST, MP_DIRECT_PORT, 0, "conntest") ? 1 : 0;
}
void MapPoll(void*, MpCtMap* out) {
    memset(out, 0, sizeof *out);
    if (!mp_portmap_info) return;
    MpPortmapInfo pm;
    memset(&pm, 0, sizeof pm);
    mp_portmap_info(&pm);
    out->state = pm.state == MP_PORTMAP_MAPPED ? MPCT_MAP_MAPPED : (pm.state == MP_PORTMAP_WORKING ? MPCT_MAP_WORKING : MPCT_MAP_FAILED);
    out->cgnat = pm.cgnat;
    out->extIp = pm.extIp;
}
void MapEnd(void*) {
    if (mp_portmap_release_as) mp_portmap_release_as(MP_PORTMAP_TEST);
    else if (mp_portmap_release) mp_portmap_release();
}

// ---- STUN: one request from a socket of its own (any port); the server names resolve on a short-lived thread (DNS blocks) ----------
#if defined(MP_CT_HAVE_STUN)
struct DnsJob {
    char list[160];
    NpNet::Addr out[NpNet::kMaxStunServers];
    volatile int count;
    volatile int done;
    volatile int refs;   // the probe and the thread; the last one frees it
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
                if (NpNet::ResolveIpv4(item, ips, 2) > 0) j->out[n++] = NpNet::Addr(ips[0], uint16_t(port));
            }
        }
        item = comma ? comma + 1 : 0;
    }
    j->count = n;
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL) == 0) free(j);
    return 0;
}
uint64_t StunNow(void*) { return mp_sys_now_ms(); }

struct Stun {
    NpNet::Udp* udp = nullptr;
    NpNet::Env env;
    NpNet::StunClient client;
    DnsJob* dns = nullptr;
    bool started = false;
    uint64_t begun = 0;
};
Stun* g_stun = nullptr;

void StunEnd(void*) {
    if (!g_stun) return;
    if (g_stun->udp) {
        g_stun->udp->Close();
        delete g_stun->udp;
    }
    if (g_stun->dns && __atomic_sub_fetch(&g_stun->dns->refs, 1, __ATOMIC_ACQ_REL) == 0) free(g_stun->dns);
    delete g_stun;
    g_stun = nullptr;
}

int StunBegin(void*) {
    StunEnd(nullptr);
    const char* list = getenv("SMG_MP_STUN");
    if (!list && !mp_config()->stun) return 0;   // mp.cfg "stun = 0": no request to a third party, the check says less
    if (!list) list = "stun.cloudflare.com:3478";
    if (!*list || !strcmp(list, "0")) return 0;
    DnsJob* job = static_cast<DnsJob*>(calloc(1, sizeof(DnsJob)));
    if (!job) return 0;
    Stun* s = new Stun();
    s->udp = NpNet::CreatePlatformUdp();
    if (!s->udp || !s->udp->Open(0)) {
        if (s->udp) delete s->udp;
        delete s;
        free(job);
        return 0;
    }
    s->env.udp = s->udp;
    s->env.now = StunNow;
    s->env.random = NpNet::SystemRandom;
    s->dns = job;
    snprintf(s->dns->list, sizeof s->dns->list, "%s", list);
    s->dns->refs = 2;
    s->begun = mp_sys_now_ms();
    g_stun = s;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&t, &at, DnsMain, s->dns);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        s->dns->refs = 1;   // (no thread: ours only)
        s->dns->done = 1;
        s->dns->count = 0;
    }
    return 1;
}

int StunPoll(void*, uint32_t* ipOut) {
    Stun* s = g_stun;
    if (!s) return -1;
    if (!s->started) {
        if (__atomic_load_n(&s->dns->done, __ATOMIC_ACQUIRE)) {
            if (s->dns->count <= 0) return -1;   // the names did not resolve
            s->started = true;
            s->client.Start(&s->env, s->dns->out, s->dns->count);
        } else {
            return mp_sys_now_ms() - s->begun > 2000 ? -1 : 0;   // (a resolver that hangs: no answer)
        }
    }
    // the answers arrive on our socket: only STUN-looking datagrams are handed to the client, everything else is dropped unread
    uint8_t buf[1500];
    for (int i = 0; i < 16; ++i) {
        NpNet::Addr from;
        const int n = s->udp->RecvFrom(&from, buf, sizeof buf);
        if (n <= 0) break;
        if (NpNet::Classify(buf, size_t(n)) == NpNet::PKT_STUN) s->client.HandlePacket(from, buf, size_t(n));
    }
    s->client.Tick();
    if (!s->client.Done()) return 0;
    const NpNet::Addr m = s->client.FirstMapped();
    if (m.IsZero()) return -1;
    *ipOut = m.ip;
    return 1;
}
#else
int StunBegin(void*) { return 0; }
int StunPoll(void*, uint32_t*) { return -1; }
void StunEnd(void*) {}
#endif

// ---- the room ------------------------------------------------------------------------------------------------------------------------
int HostRtt(void*) {
    if (!mp_active()) return -1;
    const int local = mp_local_slot();
    int best = -1;
    if (mp_is_host()) {   // a host: the slowest guest is the room's delay
        for (int k = 0; k < MP_MAX_PLAYERS; ++k)
            if (k != local && mp_slot_present(k)) {
                const int ms = mp_slot_ping_ms(k);
                if (ms > best) best = ms;
            }
        return best;
    }
    const int host = mp_host_slot();
    return host >= 0 && host < MP_MAX_PLAYERS ? mp_slot_ping_ms(host) : -1;
}

uint64_t Now(void*) { return mp_sys_now_ms(); }

MpCtProbes Probes() {
    MpCtProbes p;
    memset(&p, 0, sizeof p);
    p.now = Now;
    p.wifi = ProbeWifi;
    p.local = ProbeLocal;
    p.serviceConfigured = ProbeConfigured;
    p.infoBegin = InfoBegin;
    p.infoPoll = InfoPoll;
    p.infoCancel = InfoCancel;
    p.mapBegin = MapBegin;
    p.mapPoll = MapPoll;
    p.mapEnd = MapEnd;
    p.stunBegin = StunBegin;
    p.stunPoll = StunPoll;
    p.stunEnd = StunEnd;
    p.hostRtt = HostRtt;
    p.busy = KeepAlive;
    return p;
}

MpConnTest& Test() {
    static MpConnTest t;
    return t;
}
bool g_weak = false;   // the last finished run found a weak Wi-Fi or a slow link

// Relay only, as a session decides it (mp_conn_online.cpp -> vlink_online.cpp): mp.cfg relay_only = 1 forces it on (SMG_NET_RELAY_ONLY=0 cannot turn
// it off); relay_only = 0 leaves SMG_NET_RELAY_ONLY in charge (any value but 0 is on). So: on when either says on.
bool RelayOnlyOn() {
    if (mp_config()->relayOnly) return true;
    const char* v = getenv("SMG_NET_RELAY_ONLY");
    return v && *v && atoi(v) != 0;
}

}  // namespace

// release3: the last finished run as plain text lines for the bug report (mp_report.cpp reads <data dir>/mp/conntest.txt, at most
// MP_REPORT_CONNTEST_KB). Words only: no address, no name. A config-side file in the multiplayer folder, never next to the saves.
static const char* const kCheckName[MPCT_CHECKS] = {"wifi", "same_wifi", "internet", "hosting", "delay"};
static const char* const kResultName[] = {"none", "skipped", "wifi_good", "wifi_weak", "wifi_none", "wifi_unknown", "local_found", "local_ok",
                                          "local_blocked", "net_ok", "net_down", "net_not_set_up", "net_update", "net_off", "host_open",
                                          "host_maybe", "host_cgnat", "host_unknown", "delay_fast", "delay_ok", "delay_slow"};
static const char* const kHeadName[] = {"none", "all_good", "join_only", "online_only", "slow", "wifi_weak", "no_wifi", "no_internet",
                                        "not_set_up", "update", "paused"};
static void WriteResultFile(const MpCtView& v) {
    char dir[200];
#ifdef __vita__
    snprintf(dir, sizeof dir, "ux0:data/smg/mp");
#else
    const char* d = getenv("PETARI_DATA_DIR");
    snprintf(dir, sizeof dir, "%s/mp", d && *d ? d : ".");
#endif
    mkdir(dir, 0777);   // (exists already once mp.cfg was written; the error is fine)
    char text[1024];
    int n = snprintf(text, sizeof text, "connection test (last run%s)\n", v.inRoom ? ", in a room" : "");
    for (int i = 0; i < MPCT_CHECKS && n > 0 && n < static_cast<int>(sizeof text) - 80; i++) {
        const unsigned r = v.result[i];
        const char* word = r < sizeof kResultName / sizeof *kResultName ? kResultName[r] : "?";
        if (i == MPCT_HOSTING && r == MPCT_R_SKIPPED && !v.inRoom && RelayOnlyOn()) word = "relay_only";   // (not tested: nothing asked of the router or STUN)
        n += snprintf(text + n, sizeof text - n, "%s = %s", kCheckName[i], word);
        if (v.detail[i] >= 0 && (i == MPCT_WIFI || i == MPCT_DELAY)) n += snprintf(text + n, sizeof text - n, " (%d%s)", v.detail[i], i == MPCT_WIFI ? "%" : " ms");
        n += snprintf(text + n, sizeof text - n, "\n");
    }
    const unsigned h = v.verdict.headline;
    if (n > 0 && n < static_cast<int>(sizeof text) - 80)
        n += snprintf(text + n, sizeof text - n, "verdict = %s\nrelay_only = %d\nstun = %d\n", h < sizeof kHeadName / sizeof *kHeadName ? kHeadName[h] : "?",
                      RelayOnlyOn() ? 1 : 0, mp_config()->stun ? 1 : 0);
    char path[240], tmp[250];
    snprintf(path, sizeof path, "%s/conntest.txt", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n <= 0) return;
    const size_t len = static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : static_cast<int>(sizeof text) - 1);
    // smallfix2: POSIX calls (not the game's wrapped fopen), flushed to storage before the rename
    bool ok = false;
    {
        const int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0660);
        if (fd < 0) return;
        size_t at = 0;
        ok = true;
        while (at < len) {
            const ssize_t w = write(fd, text + at, len - at);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            at += (size_t)w;
        }
        if (ok && fsync(fd) != 0) ok = false;
        ok = close(fd) == 0 && ok;
    }
    if (ok) { remove(path); rename(tmp, path); }   // (rename over an existing file is not allowed on every target)
    else remove(tmp);
}

extern "C" {

bool mp_conntest_start(bool inRoom) {
    CT_HOST_ALLOC;
    MpCtOptions o;
    o.inRoom = inRoom && mp_active();
    o.isHost = o.inRoom && mp_is_host();
    o.relayOnly = RelayOnlyOn();
    g_lastScan = 0, g_lastRooms = 0, g_scanFailed = false;
    Test().start(Probes(), o);
    mp_log("conntest: started%s", o.inRoom ? " (in a room)" : "");
    return true;
}

void mp_conntest_pump(void) {
    CT_HOST_ALLOC;
    MpConnTest& t = Test();
    const bool was = t.running();
    t.pump();
    if (was && !t.running() && t.view().done) {
        const MpCtView& v = t.view();
        g_weak = v.verdict.weakLink;
        WriteResultFile(v);
        mp_log("conntest: done wifi=%d local=%d net=%d host=%d delay=%d (%d ms) -> headline=%d weak=%d", v.result[MPCT_WIFI], v.result[MPCT_LOCAL],
               v.result[MPCT_INTERNET], v.result[MPCT_HOSTING], v.result[MPCT_DELAY], v.detail[MPCT_DELAY], v.verdict.headline, v.verdict.weakLink ? 1 : 0);
    }
}

void mp_conntest_view(MpCtView* out) {
    CT_HOST_ALLOC;
    if (out) *out = Test().view();
}

void mp_conntest_cancel(void) {
    CT_HOST_ALLOC;
    if (Test().running()) mp_log("conntest: cancelled");
    Test().cancel();
}

bool mp_conntest_weak_link(void) { return g_weak; }

bool mp_conntest_save_report(void) {
    CT_HOST_ALLOC;
    char path[260];
    const bool ok = mp_report_save(nullptr, path, sizeof path);
    mp_log("conntest: report %s", ok ? "saved" : "FAILED");
    return ok;
}

bool mp_conntest_relay_only(void) { return RelayOnlyOn(); }

// linkfix: the last HTTPS handshake to the service failed only because of the certificate DATES (expired / not yet valid, the rest of the chain
// fine; or a clock earlier than this build), until the next successful handshake. The Connection Test then says "set the date and time"
// (decomp/src/Game/MP/MpMenuConnTest.cpp ClockWrong). A console with a wrong date never gets as far as the service's own "clock" answer.
bool mp_conntest_clock_wrong(void) { return np_https_last_clock_fault && np_https_last_clock_fault() != 0; }

}  // extern "C"
