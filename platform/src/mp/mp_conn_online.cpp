// Galaxy 3.0 multiplayer (port side, owner: core): the online path adapter. MpCore's session runs over the process-wide VLink
// (VLink::Default()), whose backend the NpNet controller installs (net/ vlink_online.cpp: STUN, hole punching, TURN relay, AEAD):
//   host:  NpNetHost(name)        -> the room code appears (NpNetInviteCode)  -> link up -> MpCore waits for HELLOs
//   guest: NpNetJoin(code, name)  -> a member is connected (phase READY)      -> link up -> MpCore broadcasts HELLO on the link
// The NpNet C API is referenced weakly, so this file links (and fails cleanly with MP_ERR_SERVICE_DOWN) in a build without net/.
// The service account (NpNetConfigureService) is set up by the service module; mp_svc_signin_poll (weak) says when it is ready.
#include "mp_config.h"
#include "mp_transport_vlink.h"

#include "port/mp_bridge.h"

#include <stdio.h>
#include <string.h>

extern "C" {
int NpNetHost(const char* playerName) __attribute__((weak));
int NpNetJoin(const char* inviteCode, const char* playerName) __attribute__((weak));
void NpNetSetRelayOnly(int on) __attribute__((weak));   // release3: the mp.cfg key relay_only
void NpNetPoll(void) __attribute__((weak));
void NpNetLeave(void) __attribute__((weak));
int NpNetPhase(void) __attribute__((weak));
const char* NpNetInviteCode(void) __attribute__((weak));
int NpNetMemberCount(void) __attribute__((weak));
int NpNetMemberCode(int slot, unsigned int* vip, char* out, int cap) __attribute__((weak));
int NpNetMemberLabel(int slot, unsigned int* vip, char* out, int cap) __attribute__((weak));   // svcfix3: a shared name's label
const char* NpNetError(void) __attribute__((weak));
int NpNetReconnect(void) __attribute__((weak));
const char* NpNetErrorCode(void) __attribute__((weak));   // fix3: the service's code behind phase FAILED ("not_member", ...)
void NpNetSetCreateBody(const char* json) __attribute__((weak));   // integ: np_signal_http.cpp (room mode / visibility / size)
int NpNetLocalEndpoint(unsigned int* ip, unsigned short* port) __attribute__((weak));        // netx: UPnP for the online host
void NpNetSetMappedCandidate(unsigned int ip, unsigned short port) __attribute__((weak));
}
#include "mp_portmap.h"
extern "C" {
void mp_portmap_request(uint16_t internalPort, uint32_t localIp, const char* who) __attribute__((weak));
void mp_portmap_release(void) __attribute__((weak));
void mp_portmap_info(MpPortmapInfo* out) __attribute__((weak));
}

#if MP_HAVE_VLINK
#include "vlink.h"
#endif

namespace {

// NpNetPhase values (np_online_api.h)
enum { NP_OFF = 0, NP_ROOM = 3, NP_WAITING = 4, NP_READY = 6, NP_RELAY_NEEDED = 7, NP_FAILED = 8, NP_LOST = 9 };

bool Contains(const char* text, const char* word) {
    if (!text || !word) return false;
    const size_t n = strlen(word);
    for (const char* p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 0x20) == (word[i] | 0x20)) i++;
        if (i == n) return true;
    }
    return false;
}

// fix3: the service's own code first (NpNetErrorCode: never shown, never worded by the transport for the game); the sentence after
// (a transport failure has no code). version_mismatch / game_mismatch = another build: "different version", not "please update".
MpUiError MapCode(const char* code) {
    if (!code || !*code) return MP_ERR_NONE;
    if (!strcmp(code, "version_mismatch") || !strcmp(code, "game_mismatch")) return MP_ERR_DIFFERENT_VERSION;
    if (!strcmp(code, "update_required") || !strcmp(code, "upgrade_required")) return MP_ERR_UPDATE_REQUIRED;
    if (!strcmp(code, "room_not_found") || !strcmp(code, "bad_code") || !strcmp(code, "room_closed") || !strcmp(code, "room_expired"))
        return MP_ERR_ROOM_NOT_FOUND;
    if (!strcmp(code, "room_full") || !strcmp(code, "room_started")) return MP_ERR_ROOM_FULL;
    if (!strcmp(code, "rate_limited") || !strcmp(code, "daily_cap")) return MP_ERR_RATE_LIMITED;
    if (!strcmp(code, "not_member")) return MP_ERR_HOST_LEFT;   // (the room went on without us; the core rejoins first)
    return MP_ERR_NONE;
}

MpUiError MapError(const char* text) {
    if (Contains(text, "version") || Contains(text, "update")) return MP_ERR_UPDATE_REQUIRED;
    if (Contains(text, "not found") || Contains(text, "no room") || Contains(text, "unknown") || Contains(text, "expired") ||
        Contains(text, "invalid code"))
        return MP_ERR_ROOM_NOT_FOUND;
    if (Contains(text, "full")) return MP_ERR_ROOM_FULL;
    if (Contains(text, "rate") || Contains(text, "too many")) return MP_ERR_RATE_LIMITED;
    if (Contains(text, "wi-fi") || Contains(text, "wifi") || Contains(text, "network") || Contains(text, "resolve"))
        return MP_ERR_NO_WIFI;
    if (Contains(text, "nat") || Contains(text, "relay")) return MP_ERR_NAT;
    return MP_ERR_SERVICE_DOWN;
}

class OnlineConnector : public MpConnector {
public:
    explicit OnlineConnector(const MpConnectorParams& p)
        : m_p(p), m_started(false), m_ready(false), m_announced(false), m_rejoin(false), m_session(0), m_lastReconnectMs(0), m_players(1),
          m_mapPort(0), m_mapSerial(0) {
        m_code[0] = 0;
    }
    ~OnlineConnector() override {
        if (m_mapPort && mp_portmap_release) mp_portmap_release();
        delete m_session;
#if MP_HAVE_VLINK
        if (m_ready) VLink::Default().Stop();
#endif
        if (m_announced && mp_svc_room_closed) mp_svc_room_closed(m_code);
        if (m_started && NpNetLeave) NpNetLeave();
    }

    int poll(uint64_t nowMs) override {
#if !MP_HAVE_VLINK
        (void)nowMs;
        return -(int)MP_ERR_SERVICE_DOWN;
#else
        if (!NpNetHost || !NpNetJoin || !NpNetPoll || !NpNetLeave || !NpNetPhase || !NpNetInviteCode) {
            mp_log("online: this build has no online transport");
            return -(int)MP_ERR_SERVICE_DOWN;
        }
        if (!m_started) {
            if (mp_svc_signin_poll) {
                const int s = mp_svc_signin_poll();
                if (s == 0) return MP_CONNECTOR_WORKING;
                if (s < 0) return s;
            }
            if (m_p.host && NpNetSetCreateBody) {
                // the service lists the room by these (quick match = public rooms of the mode; join by friend = friends / public)
                static const char* const kVis[3] = {"code", "friends", "public"};
                const int maxP = m_p.settings.maxPlayers >= 2 && m_p.settings.maxPlayers <= MP_MAX_PLAYERS ? m_p.settings.maxPlayers : MP_MAX_PLAYERS;
                char body[96];
                snprintf(body, sizeof body, "{\"mode\":\"%s\",\"maxMembers\":%d,\"visibility\":\"%s\"}",
                         m_p.settings.mode == MP_MODE_RACE ? "race" : "coop", maxP, kVis[m_p.visibility < 3 ? m_p.visibility : 0]);
                NpNetSetCreateBody(body);
            }
            // release3: mp.cfg relay_only = 1 is the same switch as SMG_NET_RELAY_ONLY=1 (on: relay only; off: leave the env setting alone)
            if (NpNetSetRelayOnly) NpNetSetRelayOnly(mp_config()->relayOnly ? 1 : -1);
            const int ok = m_p.host ? NpNetHost(m_p.name) : NpNetJoin(m_p.code, m_p.name);
            if (!ok) {
                mp_log("online: %s could not start", m_p.host ? "host" : "join");
                return -(int)MP_ERR_SERVICE_DOWN;
            }
            m_started = true;
            mp_log("online: %s started%s%s", m_p.host ? "host" : "join", m_p.host ? "" : " code=", m_p.host ? "" : m_p.code);
        }
        NpNetPoll();
        const int phase = NpNetPhase();
        if (phase == NP_FAILED) {
            const char* why = NpNetError ? NpNetError() : "";
            const char* code = NpNetErrorCode ? NpNetErrorCode() : "";
            if (!code) code = "";
            mp_log("online: failed (%s) code=%s", why ? why : "", *code ? code : "-");
            if (mp_service_note_code) mp_service_note_code(code);   // fix3: the menus word it (mp_service_last_code)
            // fix3: the service dropped this console from the room it was playing in (silent for a minute: asleep, a long stall).
            // The room may go on: the core joins it afresh (a new connector, a new HELLO) instead of ending or hanging here.
            m_rejoin = m_ready && !strcmp(code, "not_member");
            const MpUiError byCode = MapCode(code);
            return -(int)(byCode != MP_ERR_NONE ? byCode : MapError(why));
        }
        if (phase == NP_RELAY_NEEDED) return -(int)MP_ERR_NAT;
        if (phase == NP_LOST) {
            if (!m_p.host) return -(int)(m_ready ? MP_ERR_HOST_LEFT : MP_ERR_NAT);
            // host: every member is gone; keep the room and rebuild the link for the next guest
            if (NpNetReconnect && nowMs - m_lastReconnectMs > 5000) {
                m_lastReconnectMs = nowMs;
                NpNetReconnect();
            }
        }
        if (m_p.host) pumpPortmap();
        if (!m_code[0]) {
            const char* c = NpNetInviteCode();
            if (c && *c) {
                snprintf(m_code, sizeof m_code, "%s", c);
                mp_log("online: room code=%s", m_code);
            }
        }
        if (m_ready) {
            if (m_p.host && !m_announced && m_code[0] && mp_svc_room_hosted) {
                mp_svc_room_hosted(m_code, &m_p.settings, m_p.visibility, m_players);
                m_announced = true;
            }
            return MP_CONNECTOR_READY;
        }
        // host: ready as soon as the room exists; guest: once a member (the host) is connected
        const bool canStart = m_p.host ? (m_code[0] != 0 && phase >= NP_ROOM) : phase == NP_READY;
        if (!canStart) return MP_CONNECTOR_WORKING;
        VLink::Link& link = VLink::Default();
        link.SetFingerprint(MP_LINK_FINGERPRINT);
        if (!link.IsUp() && !link.Start()) return MP_CONNECTOR_WORKING;   // the backend has no address yet: next poll
        m_session = mp_vlink_socket_open(&link, MP_SESSION_PORT, "online");
        if (!m_session) {
            mp_log("online: session socket could not be bound");
            return -(int)MP_ERR_INTERNAL;
        }
        m_ready = true;
        mp_log("online: link up");
        return MP_CONNECTOR_READY;
#endif
    }
    MpTransport* session() override { return m_session; }
    MpTransport* discovery() override { return 0; }
    void roomCode(char* out, size_t cap) override {
        if (cap) snprintf(out, cap, "%s", m_code);
    }
    int roomMembers() override { return NpNetMemberCount ? NpNetMemberCount() : -1; }
    bool memberCode(int slot, uint32_t* vip, char out[10]) override {   // core2
        out[0] = 0;
        unsigned int v = 0;
        if (!m_ready || !NpNetMemberCode || !NpNetMemberCode(slot, &v, out, 10)) { out[0] = 0; return false; }
        *vip = v;
        return true;
    }
    bool memberLabel(int slot, uint32_t* vip, char out[17]) override {   // svcfix3
        out[0] = 0;
        unsigned int v = 0;
        if (!m_ready || !NpNetMemberLabel || !NpNetMemberLabel(slot, &v, out, 17)) { out[0] = 0; return false; }
        *vip = v;
        return true;
    }
    bool rejoinable() override { return m_rejoin; }
    void playersChanged(int players) override {
        m_players = players;
        if (m_announced && mp_svc_room_hosted) mp_svc_room_hosted(m_code, &m_p.settings, m_p.visibility, players);
    }
    const char* name() const override { return "online"; }

private:
    // netx: the host asks its router to forward NpNet's UDP port (UPnP, on the helper thread: mp_portmap.cpp); once forwarded, the
    // public ip:port is one more candidate for the members (a pair that cannot punch reaches it directly instead of through the relay)
    void pumpPortmap() {
        if (!NpNetLocalEndpoint || !NpNetSetMappedCandidate || !mp_portmap_request || !mp_portmap_info) return;
        unsigned int ip = 0;
        unsigned short port = 0;
        if (!NpNetLocalEndpoint(&ip, &port)) return;
        if (port != m_mapPort) {   // (a rebuilt socket may have another port)
            m_mapPort = port;
            mp_portmap_request(port, ip, "online host");
        }
        MpPortmapInfo pm;
        mp_portmap_info(&pm);
        if (pm.serial == m_mapSerial) return;
        m_mapSerial = pm.serial;
        const bool usable = pm.state == MP_PORTMAP_MAPPED && pm.intPort == port && pm.extIp && !pm.cgnat;
        NpNetSetMappedCandidate(usable ? pm.extIp : 0, usable ? pm.extPort : 0);
    }

    MpConnectorParams m_p;
    bool m_started, m_ready, m_announced;
    bool m_rejoin;   // fix3: the last failure was not_member in a session (rejoinable)
    MpTransport* m_session;
    uint64_t m_lastReconnectMs;
    int m_players;
    uint16_t m_mapPort;
    uint32_t m_mapSerial;
    char m_code[16];
};

}  // namespace

MpConnector* mp_connector_online_create(const MpConnectorParams& p) { return new OnlineConnector(p); }
