// Galaxy 3.0 multiplayer (port side, owner: integ): the glue between the session layer (MpCore) and the vendored net/ stack.
//   - the strong np_glue.h functions net/ calls into the game (link lost, member lost, sleep / resume, the chosen link mode);
//   - mp_net_link_poll / mp_net_link_release (mp_transport.h) on the Vita: Same Wi-Fi (infrastructure UDP) and Nearby (ad hoc)
//     are brought up by net/ (NpLanRadioPoll) on VLink::Default(), which MpCore then opens its sockets on. The desktop keeps
//     MpCore's own POSIX UDP link (SMG_MP_UDP_*): no hook there;
//   - mp_netglue_frame(): NpNetFrame() at most once per ~15 ms, called by MpCore's pump under its lock (both the game thread and
//     the helper thread pump, so net/ is only ever entered by one thread at a time) while multiplayer uses the network.
// Strong definitions in a file of their own that is always linked: MpCore references mp_netglue_frame (strong) so this object is
// pulled out of the Vita runtime archive, and with it every strong np_glue.h function (net/src/np_glue.cpp has weak defaults).
#include "mp_transport.h"
#include "mp_transport_vlink.h"

#include "port/mp_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if MP_HAVE_VLINK
#include "np_glue.h"
#include "np_online_api.h"
#include "vlink.h"
#endif

#include "mp_service.h"

extern "C" void mp_netglue_frame(void);

// MpCore and mp_conn_online.cpp reference the service's hooks weakly (their host tests link without it). A weak reference does
// not pull an object out of a static archive (the Vita runtime is one): these strong references do, so the real functions are
// linked whenever the session layer is.
extern "C" const int mp_conn_direct_anchor;   // netx: mp_conn_direct.cpp (and through it mp_portmap.cpp), referenced weakly by MpCore
extern "C" void* const mp_netglue_keep[] __attribute__((used)) = {
    (void*)&mp_conn_direct_anchor,
    (void*)&mp_service_pump, (void*)&mp_service_set_build, (void*)&mp_svc_signin_poll, (void*)&mp_svc_room_lookup_begin,
    (void*)&mp_svc_room_lookup_poll, (void*)&mp_svc_room_lookup_cancel, (void*)&mp_svc_room_hosted, (void*)&mp_svc_room_closed,
};

#if MP_HAVE_VLINK

namespace {
int g_linkMode = -1;   // kNpMenu* chosen by mp_net_link_poll (-1: none yet)
#if defined(__vita__)
bool g_linkUp = false;
int g_failKind = 0;        // mpwifi44: NpLanRadioFailKind of the last failed bring-up
char g_failWhy[96] = "";
#endif
}  // namespace

extern "C" {

// ---- np_glue.h: implemented by Galaxy -------------------------------------------------------------------------------------------
void NpLanLinkLost(const char* why) {
    mp_log("net: link lost (%s)", why ? why : "?");
    // Every send on the link fails from now on: MpCore's transport reports lost() and the session ends / reconnects.
    VLink::Default().SetLost(true, why);
    if (NpNetActive()) NpNetOnLinkLost(why);
}

void NpLanPeerLost(int slot, const char* why) {
    // (MpCore notices the silent peer itself: host 15 s, guests 5 s for the host -> migration)
    mp_log("net: member %d path lost (%s)", slot, why ? why : "?");
}

void NpLanOnSuspend(void) { NpLanLinkLost("the Vita is going to sleep"); }
void NpLanOnResume(void) { NpLanLinkLost("resumed from sleep"); }

int NpMenuLinkMode(void) {
    if (g_linkMode >= 0) return g_linkMode;
    const char* m = getenv("SMG_LAN_MODE");
    if (m && !strcmp(m, "online")) return kNpMenuOnline;
    if (m && !strcmp(m, "wifi")) return kNpMenuWifi;
    if (m && !strcmp(m, "adhoc")) return kNpMenuNearby;
#if defined(__vita__)
    return kNpMenuNearby;
#else
    return kNpMenuWifi;
#endif
}

#if defined(__vita__)
// ---- mp_transport.h hooks (Vita only; the desktop's link is MpCore's own POSIX UDP VLink) ------------------------------------
int mp_net_link_poll(int conn, int request, void** linkOut) {
    if (conn != MP_CONN_WIFI && conn != MP_CONN_NEARBY) return -2;
    if (request) g_linkMode = conn == MP_CONN_WIFI ? kNpMenuWifi : kNpMenuNearby;
    const int r = NpLanRadioPoll(request);
    if (r == 2) {
        VLink::Link& link = VLink::Default();
        link.SetFingerprint(MP_LINK_FINGERPRINT);
        if (!link.IsUp() && !link.Start()) return 1;   // the backend has no address yet: next poll
        if (!g_linkUp) mp_log("net: %s link up", conn == MP_CONN_WIFI ? "wifi" : "nearby");
        g_linkUp = true;
        if (linkOut) *linkOut = &link;
        return 2;
    }
    if (r < 0) {
        {   // mpwifi44: the reason, taken before the release below (which changes nothing about it, but the order is the safe one)
            const char* why = "";
            g_failKind = NpLanRadioFailKind(&why);
            snprintf(g_failWhy, sizeof g_failWhy, "%s", why ? why : "");
        }
        mp_log("net: %s link failed: %s (kind %d)", conn == MP_CONN_WIFI ? "wifi" : "nearby", g_failWhy[0] ? g_failWhy : "no reason given", g_failKind);
        // (Strikers 1.3 fix: a failed bring-up must release what it opened, or the socket / radio stays claimed)
        NpLanRadioStop();
        g_linkMode = -1;
        return -1;
    }
    return 1;   // working (0 = idle only happens with request 0: still "working" for the connector)
}

// mpwifi44 (mp_transport.h): why the last mp_net_link_poll answered -1
int mp_net_link_fail(int conn, const char** why) {
    (void)conn;
    if (why) *why = g_failWhy;
    return g_failKind == 1 ? MP_LINK_FAIL_WIFI_DOWN : g_failKind == 2 ? MP_LINK_FAIL_OTHER : MP_LINK_FAIL_UNKNOWN;
}

void mp_net_link_release(int conn) {
    mp_log("net: %s link released", conn == MP_CONN_WIFI ? "wifi" : conn == MP_CONN_NEARBY ? "nearby" : "?");
    VLink::Default().Stop();
    NpLanRadioStop();
    g_linkUp = false;
    g_linkMode = -1;
}
#endif

// ---- per frame -------------------------------------------------------------------------------------------------------------------
void mp_netglue_frame(void) { NpNetFrame(); }

}  // extern "C"

#else  // !MP_HAVE_VLINK: no net/ in this build

extern "C" void mp_netglue_frame(void) {}

#endif
