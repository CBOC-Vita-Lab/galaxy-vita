// VLink backend for internet play, and the C API the menu calls (np_online.h). The link above it is the game's own LAN link: lobby, lockstep,
// frames with magic / version / fingerprint, fragmentation. This backend only moves those frames between rooms' members, encrypted, over
// the paths OnlineNet punched (online/np_online.cpp). Behind SMG_NET_* switches; nothing here runs unless an online session was started.
//
// The member slot is the backend's PeerAddr (byte 0): the link learns "vip -> slot" from the frames it receives, and a unicast to a vip it
// has not heard from goes out as a broadcast frame (to every connected member), exactly as on the other backends.
#include "vlink.h"
#include "online/np_online.h"
#include "online/np_app_version.h"
#include "online/np_http.h"
#include "online/np_signal_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace NpNet;

// Galaxy: the game side is reached through np_glue.h (NpLanLinkLost, NpLanPeerLost, NpLanPlayerName), not Strikers' np_lan.h.
#include "../include/np_glue.h"

namespace {

OnlineNet* g_net = 0;
Signal* (*g_signalFactory)(void) = 0;
bool g_lostFromGame = false;

uint32_t OnlineVip(void*) {
    if (!g_net) return 0;
    g_net->OnLinkStart();   // a link that was lost is rebuilt now, when the game's session layer starts over (STR-155 path)
    return g_net->MyVip();
}

int OnlineSend(void*, const VLink::PeerAddr* to, bool broadcast, const void* data, size_t len) {
    if (!g_net) return -1;
    if (broadcast || !to) return g_net->SendFrame(-1, data, len);
    return g_net->SendFrame(to->bytes[0], data, len);
}

int OnlineRecv(void*, VLink::PeerAddr* from, void* buffer, size_t capacity) {
    if (!g_net) return 0;
    int slot = 0;
    const int n = g_net->RecvFrame(&slot, buffer, capacity);
    if (n <= 0) return 0;
    memset(from, 0, sizeof *from);
    from->bytes[0] = uint8_t(slot);
    return n;
}

// The session layer stopped its link (the game ended its session). The room and the paths stay: the lobby starts a new session over them.
void OnlineClose(void*) {}

// maxFrame: one datagram carries kMaxDatagram - 22 bytes of frame; the link splits longer frames into fragments of its own (vlink.cpp)
const VLink::Backend kOnline = {"online", 0, OnlineVip, OnlineSend, OnlineRecv, OnlineClose, NpNet::kMaxFrame};

int EnvInt(const char* name, int fallback) {
    const char* v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

int g_relayOnlyOverride = -1;   // the menu's "Connection" setting (NpNetSetRelayOnly); -1 = not set: SMG_NET_RELAY_ONLY decides

void LostFromController(void*, const char* why) {
    // the controller found every member gone (closed, or silent for lostMs): the game ends its session with its own "connection lost"
    NpLanLinkLost(why);
}

// Galaxy: one member's path ended (a star host keeps its other guests); the session layer drops that slot
void PeerLostFromController(void*, int slot, const char* why) { NpLanPeerLost(slot, why); }

void BuildConfig(OnlineConfig* cfg) {
    if (const char* s = getenv("SMG_NET_STUN"))
        snprintf(cfg->stun, sizeof cfg->stun, "%s", s);
#if defined(__vita__)
    // mp75: the console refuses chosen UDP ports from about 32768 up (bind survey 23 Nov: 1024-30000 bind, 40000+ refused), so the
    // online socket asks for 24110 there (np_udp_vita still falls back to a system-chosen port when even that is refused).
    cfg->port = uint16_t(EnvInt("SMG_NET_PORT", 24110));
#else
    cfg->port = uint16_t(EnvInt("SMG_NET_PORT", kDefaultPort));
#endif
    cfg->punch.intervalMs = uint32_t(EnvInt("SMG_NET_PUNCH_INTERVAL_MS", int(cfg->punch.intervalMs)));
    cfg->punch.timeoutMs = uint32_t(EnvInt("SMG_NET_PUNCH_TIMEOUT_MS", int(cfg->punch.timeoutMs)));
    cfg->punch.keepaliveMs = uint32_t(EnvInt("SMG_NET_KEEPALIVE_MS", int(cfg->punch.keepaliveMs)));
    cfg->punch.staleMs = uint32_t(EnvInt("SMG_NET_STALE_MS", int(cfg->punch.staleMs)));
    // the transport declares a member lost only after the game's own keep-alive limit has passed (SMG_LAN_KEEPALIVE_MS, 20 s), never before
    cfg->punch.lostMs = uint32_t(EnvInt("SMG_NET_PEER_LOST_MS", int(cfg->punch.lostMs)));
    cfg->forceTryLocal = EnvInt("SMG_NET_TRY_LOCAL", 0) != 0;
    // A1/A12/A11: symmetric NAT policy, the fault shim, the stage limits (all in milliseconds, 0 = no limit)
    cfg->punch.symmetricRelay = EnvInt("SMG_NET_SYM_RELAY", 1) != 0;
    // the relay (TURN) path: on by default; SMG_NET_RELAY_ONLY=1 is the player setting "Connection: Relay only" (Auto = unset / 0)
    cfg->relayEnabled = EnvInt("SMG_NET_RELAY", 1) != 0;
    cfg->relayOnly = (g_relayOnlyOverride >= 0 ? g_relayOnlyOverride : EnvInt("SMG_NET_RELAY_ONLY", 0)) != 0;
    cfg->honorSteer = EnvInt("SMG_NET_HONOR_STEER", 1) != 0;
    cfg->relayLimitMs = uint32_t(EnvInt("SMG_NET_RELAY_LIMIT_MS", int(cfg->relayLimitMs)));
    cfg->relayWaitMs = uint32_t(EnvInt("SMG_NET_RELAY_WAIT_MS", int(cfg->relayWaitMs)));
    cfg->turn.lifetimeSec = uint32_t(EnvInt("SMG_NET_TURN_LIFETIME", int(cfg->turn.lifetimeSec)));
    cfg->turn.permissionRefreshMs = uint32_t(EnvInt("SMG_NET_TURN_PERM_MS", int(cfg->turn.permissionRefreshMs)));
    cfg->turn.channelRefreshMs = uint32_t(EnvInt("SMG_NET_TURN_BIND_MS", int(cfg->turn.channelRefreshMs)));
    if (const char* t = getenv("SMG_NET_TURN_ADDR")) {   // tests: relay servers by address instead of the credentials' URLs ("ip:port,ip:port")
        char copy[160];
        snprintf(copy, sizeof copy, "%s", t);
        for (char* item = strtok(copy, ","); item && cfg->nTurnAddrs < 4; item = strtok(0, ","))
            if (ParseAddr(item, 3478, &cfg->turnAddrs[cfg->nTurnAddrs])) ++cfg->nTurnAddrs;
    }
    cfg->punch.hopelessTimeoutMs = uint32_t(EnvInt("SMG_NET_HOPELESS_MS", int(cfg->punch.hopelessTimeoutMs)));
    cfg->fault = FaultConfig::FromEnvironment();
    cfg->resolveLimitMs = uint32_t(EnvInt("SMG_NET_RESOLVE_LIMIT_MS", int(cfg->resolveLimitMs)));
    cfg->roomLimitMs = uint32_t(EnvInt("SMG_NET_ROOM_LIMIT_MS", int(cfg->roomLimitMs)));
    cfg->hostWaitLimitMs = uint32_t(EnvInt("SMG_NET_HOST_WAIT_MS", int(cfg->hostWaitLimitMs)));
    cfg->rebuildLimitMs = uint32_t(EnvInt("SMG_NET_REBUILD_LIMIT_MS", int(cfg->rebuildLimitMs)));
    cfg->statsLogMs = uint32_t(EnvInt("SMG_NET_STATS_MS", int(cfg->statsLogMs)));
    cfg->maxPacketsPerPump = EnvInt("SMG_NET_MAX_PACKETS", cfg->maxPacketsPerPump);
    // Galaxy: star topology through the host by default (DESIGN.md section 2); SMG_NET_TOPOLOGY=mesh = Strikers' every pair
    if (const char* t = getenv("SMG_NET_TOPOLOGY")) cfg->topology = strcmp(t, "mesh") == 0 ? TOPOLOGY_MESH : TOPOLOGY_STAR;
    cfg->onPeerLost = PeerLostFromController;
    cfg->onLost = LostFromController;
}

bool InstallAndStart(const OnlineConfig& config, const char* invite, const char* name) {
    if (g_net) {
        g_net->Stop();
        delete g_net;
        g_net = 0;
    }
    g_net = new OnlineNet;
    VLink::Default().SetBackend(&kOnline);
    g_lostFromGame = false;
    const bool ok = g_net->Start(config, invite, name);
    return ok;
}

// The online service account (set by the menu: NpNetConfigureService). On the desktop the environment can stand in: SMG_NET_SERVER (the base
// URL, http:// to the local fake is allowed there), SMG_NET_TOKEN, SMG_NET_APP, SMG_NET_BUILD.
HttpSignalConfig g_service;
bool g_serviceSet = false;

// The code a joining test process uses: SMG_NET_INVITE, or the file the host's process writes (SMG_NET_INVITE_FILE). False until it is known.
bool TestInvite(char* out, size_t cap) {
    out[0] = 0;
    if (const char* e = getenv("SMG_NET_INVITE"))
        if (*e) { snprintf(out, cap, "%s", e); return true; }
#if !defined(NP_TARGET_VITA)
    if (const char* file = getenv("SMG_NET_INVITE_FILE")) {
        if (FILE* f = fopen(file, "r")) {
            if (fgets(out, int(cap), f)) {
                size_t n = strlen(out);
                while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
            }
            fclose(f);
        }
    }
#endif
    return out[0] != 0;
}

Signal* MakeSignal() {
#if !defined(NP_TARGET_VITA)
    if (const char* dir = getenv("SMG_NET_SIGNAL_DIR"))
        if (*dir) return CreateFileSignal(dir);
    if (!g_serviceSet) {
        const char* server = getenv("SMG_NET_SERVER");
        if (server && *server) {
            HttpSignalConfig c;
            snprintf(c.baseUrl, sizeof c.baseUrl, "%s", server);
            if (const char* t = getenv("SMG_NET_TOKEN")) snprintf(c.token, sizeof c.token, "%s", t);
            snprintf(c.appVersion, sizeof c.appVersion, "%s", AppVersionOrDefault(getenv("SMG_NET_APP")));
            if (const char* b = getenv("SMG_NET_BUILD")) snprintf(c.build, sizeof c.build, "%s", b);
            { const int w = EnvInt("SMG_NET_POLL_WAIT", 25); c.pollWaitSec = uint32_t(w < 0 ? 0 : w > 60 ? 60 : w); }
            return CreateHttpSignal(c);
        }
    }
#endif
    if (g_serviceSet) return CreateHttpSignal(g_service);
    return g_signalFactory ? g_signalFactory() : 0;
}

}  // namespace

NpNet::OnlineNet* NpNetController() { return g_net; }
void NpNetSetSignalFactory(NpNet::Signal* (*factory)(void)) { g_signalFactory = factory; }

bool NpNetStartWith(const OnlineConfig& config, const char* invite, const char* playerName) {
    return InstallAndStart(config, invite, playerName);
}

void NpNetGetInfo(NpNet::OnlineInfo* info) {
    if (g_net) g_net->GetInfo(info);
    else memset(info, 0, sizeof *info);
}

extern "C" {

void NpNetConfigureService(const char* baseUrl, const char* token, const char* appVersion, const char* buildHash) {
    g_service = HttpSignalConfig();
    snprintf(g_service.baseUrl, sizeof g_service.baseUrl, "%s", baseUrl ? baseUrl : "");
    snprintf(g_service.token, sizeof g_service.token, "%s", token ? token : "");
    snprintf(g_service.appVersion, sizeof g_service.appVersion, "%s", AppVersionOrDefault(appVersion));   // never empty: the service requires X-Msc-App
    snprintf(g_service.build, sizeof g_service.build, "%s", buildHash ? buildHash : "");
    // service review: held room polls (svcfix2: 25 s while the room forms, the service's plan after that; SMG_NET_POLL_WAIT=0 = plain polls every 1.7 s / 8 s as before)
    {
        const int w = EnvInt("SMG_NET_POLL_WAIT", 25);
        g_service.pollWaitSec = uint32_t(w < 0 ? 0 : w > 60 ? 60 : w);
    }
    g_serviceSet = g_service.baseUrl[0] != 0;
}

int NpNetHost(const char* playerName) {
    OnlineConfig cfg;
    BuildConfig(&cfg);
    cfg.signal = MakeSignal();
    return InstallAndStart(cfg, 0, playerName) ? 1 : 0;
}

int NpNetJoin(const char* inviteCode, const char* playerName) {
    if (!inviteCode || !*inviteCode) return 0;
    OnlineConfig cfg;
    BuildConfig(&cfg);
    cfg.signal = MakeSignal();
    return InstallAndStart(cfg, inviteCode, playerName) ? 1 : 0;
}

void NpNetPoll(void) {
    HttpPump();   // requests that outlive a session (the leave message) are reaped here
    if (g_net) g_net->Tick();
}

// beta diagnostics: the session's numbers the leave request carries ({"summary":...}); taken once by the next NpNetLeave.
static char g_leaveSummary[768];
void NpNetSetLeaveSummary(const char* json) {
    if (!json) { g_leaveSummary[0] = 0; return; }
    size_t n = 0;
    while (json[n] && n + 1 < sizeof g_leaveSummary) { g_leaveSummary[n] = json[n]; ++n; }
    g_leaveSummary[n] = 0;
    if (json[n]) g_leaveSummary[0] = 0;   // (too long: none, never half a JSON object)
}

void NpNetLeave(void) {
    if (!g_net) return;
    g_net->SetLeaveSummary(g_leaveSummary);
    g_leaveSummary[0] = 0;
    g_net->Stop();
    delete g_net;
    g_net = 0;
}

int NpNetActive(void) { return g_net && g_net->GetPhase() != OnlineNet::OFF ? 1 : 0; }

int NpNetReady(void) { return g_net && g_net->GetPhase() == OnlineNet::READY ? 1 : 0; }

int NpNetPhase(void) { return g_net ? int(g_net->GetPhase()) : 0; }

const char* NpNetInviteCode(void) {
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.invite;
}

int NpNetMemberCount(void) {
    OnlineInfo info;
    NpNetGetInfo(&info);
    return info.peerCount;
}

// Galaxy (core2): the public player code the service's room view gives for the member in `slot` (the core's slot: online, the service
// slot), with its virtual address. 1 with out = "XXXX-XXXX" (9 characters + NUL, cap >= 10); 0 when not in a room, no such member or
// no well-formed code. Called by the session's pump (the thread that polls the signal), never logged.
int NpNetMemberCode(int slot, unsigned int* vip, char* out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!g_net || !out || cap < 10 || slot < 0 || slot >= kMaxSlots) return 0;
    Signal* sg = g_net->GetSignal();
    if (!sg || sg->GetStatus() != Signal::IN_ROOM) return 0;
    const SigMember& m = sg->Room().members[slot];
    if (!m.present || !m.code[0] || m.slot != slot) return 0;
    if (!SigPlayerCode(m.code, out)) return 0;
    if (vip) *vip = m.vip;
    return 1;
}

// svcfix3: see np_online_api.h. Only a shared name has a label to show; every other name stays what the members told each other.
int NpNetMemberLabel(int slot, unsigned int* vip, char* out, int cap) {
    if (out && cap > 0) out[0] = 0;
    if (!g_net || !out || cap < 16 || slot < 0 || slot >= kMaxSlots) return 0;
    Signal* sg = g_net->GetSignal();
    if (!sg || sg->GetStatus() != Signal::IN_ROOM) return 0;
    const SigMember& m = sg->Room().members[slot];
    if (!m.present || m.slot != slot || !m.nameDup || !m.label[0]) return 0;
    snprintf(out, size_t(cap), "%s", m.label);
    if (vip) *vip = m.vip;
    return 1;
}

int NpNetConnectedCount(void) {
    OnlineInfo info;
    NpNetGetInfo(&info);
    return info.connectedCount;
}

const char* NpNetError(void) {
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.error;
}

const char* NpNetErrorCode(void) {   // Galaxy (fix3)
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.errorCode;
}

const char* NpNetEndReason(void) {
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.endReason;
}

const char* NpNetWarning(void) {
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.warning;
}

// "Connection: Auto / Relay only": 1 = relay only (no STUN, no raw address leaves the console, everything goes through the relay), 0 = Auto, -1 = use
// SMG_NET_RELAY_ONLY from np-player.txt. Takes effect on the next NpNetHost / NpNetJoin.
void NpNetSetRelayOnly(int on) { g_relayOnlyOverride = on; }
// The relay: 0 not used, 1 being set up, 2 allocated, 3 failed (NpNetRelayText says why). NpNetUsingRelay: 1 when a connected member is reached through it.
int NpNetRelayState(void) {
    OnlineInfo info;
    NpNetGetInfo(&info);
    return info.relayState;
}
const char* NpNetRelayText(void) {
    static OnlineInfo info;
    NpNetGetInfo(&info);
    return info.relayText;
}
int NpNetUsingRelay(void) {
    OnlineInfo info;
    NpNetGetInfo(&info);
    for (int i = 0; i < NpNet::kMaxSlots; ++i)
        if (info.peers[i].present && info.peers[i].state == NpNet::Peer::CONNECTED && info.peers[i].pathKind == NpNet::CAND_RELAY) return 1;
    return 0;
}

int NpNetRttMs(void) { return g_net ? g_net->WorstRttMs() : -1; }
int NpNetJitterMs(void) { return g_net ? g_net->WorstJitterMs() : -1; }
int NpNetLossPermille(void) { return g_net ? g_net->WorstLossPermille() : -1; }

// The menu's "Reconnect" after a lost link (phase 9): the socket is opened again, STUN runs again, the endpoints are posted again and the members
// punch afresh with new keys. 1 when a rebuild started. (The game's own lobby does not restart a link after a loss, it sends the player back to
// the menu: this is that menu's way back into the same room.)
int NpNetReconnect(void) {
    if (!g_net || g_net->GetPhase() != OnlineNet::LOST) return 0;
    g_net->OnLinkStart();
    return g_net->GetPhase() == OnlineNet::FAILED ? 0 : 1;
}

void NpNetOnLinkLost(const char* why) {
    if (g_net) g_net->OnLinkLost(why);
}

int NpNetRadioPoll(int request) {
    if (!g_net || g_net->GetPhase() == OnlineNet::OFF) {
        const char* mode = getenv("SMG_NET_MODE");
        if (!mode || !*mode) return -2;
        if (!request) return 0;
        const char* name = NpLanPlayerName();
        if (strcmp(mode, "host") == 0) NpNetHost(name && *name ? name : "HOST");
        else if (strcmp(mode, "join") == 0) {
            char invite[32];
            if (!TestInvite(invite, sizeof invite)) return 1;   // the host's code is not known yet (test harness hand-over): keep waiting
            NpNetJoin(invite, name && *name ? name : "GUEST");
        } else return -2;
        if (!g_net) return -1;
    }
    g_net->Tick();
#if !defined(NP_TARGET_VITA)
    // Test harness hand-over of the host's invite code to the joining process (SMG_NET_INVITE_FILE)
    if (const char* file = getenv("SMG_NET_INVITE_FILE")) {
        static bool written = false;
        const char* mode = getenv("SMG_NET_MODE");
        if (!written && *file && mode && strcmp(mode, "host") == 0 && NpNetInviteCode()[0]) {
            if (FILE* f = fopen(file, "w")) {
                fputs(NpNetInviteCode(), f);
                fclose(f);
                written = true;
            }
        }
    }
#endif
    switch (g_net->GetPhase()) {
    case OnlineNet::READY: return 2;
    case OnlineNet::FAILED: return -1;
    case OnlineNet::RELAY_NEEDED: return -1;   // no direct path, and relay setup failed, was disabled, or exhausted its wait
    case OnlineNet::LOST: g_net->OnLinkStart(); return 1;
    default: return 1;
    }
}

void NpNetRadioStop(void) { NpNetLeave(); }

// Galaxy (netx): UPnP port mapping of the online socket (platform/src/mp/mp_conn_online.cpp asks the router, then hands the result here)
int NpNetLocalEndpoint(unsigned int* ip, unsigned short* port) {
    uint32_t i = 0;
    uint16_t p = 0;
    if (!g_net || !g_net->LocalEndpoint(&i, &p) || !p) return 0;
    if (ip) *ip = i;
    if (port) *port = p;
    return 1;
}
void NpNetSetMappedCandidate(unsigned int ip, unsigned short port) {
    if (g_net) g_net->SetMappedCandidate(NpNet::Addr(uint32_t(ip), uint16_t(port)));
}

}  // extern "C"
