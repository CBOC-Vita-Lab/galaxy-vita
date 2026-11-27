// Galaxy 3.0 multiplayer: the net/ side of np_glue.h. Weak defaults for the functions Galaxy implements (so net/ links on its own and a
// console without a session layer still ends a dead link cleanly), the per-frame entry, and the desktop's link bring-up (the Vita's is in
// vlink_vita.cpp). New file (not in Strikers): see net/GALAXY-CHANGES.txt.
#include "np_glue.h"
#include "vlink.h"
#include "online/np_online_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_GLUE_WEAK __attribute__((weak))

extern "C" {

NP_GLUE_WEAK void NpLanLinkLost(const char* why) {
    VLink::Default().SetLost(true, why);
    if (NpNetActive()) NpNetOnLinkLost(why);
}

NP_GLUE_WEAK void NpLanPeerLost(int slot, const char* why) {
    fprintf(stderr, "[np-glue] member %d lost (%s)\n", slot, why ? why : "?");
}

NP_GLUE_WEAK void NpLanOnSuspend(void) { NpLanLinkLost("the Vita is going to sleep"); }
NP_GLUE_WEAK void NpLanOnResume(void) { NpLanLinkLost("resumed from sleep"); }

NP_GLUE_WEAK int NpMenuLinkMode(void) {
    const char* m = getenv("SMG_LAN_MODE");
    if (m && !strcmp(m, "online")) return kNpMenuOnline;
    if (m && !strcmp(m, "wifi")) return kNpMenuWifi;
    if (m && !strcmp(m, "adhoc")) return kNpMenuNearby;
#if defined(NP_TARGET_VITA)
    return kNpMenuNearby;
#else
    return kNpMenuWifi;
#endif
}

#if defined(NP_TARGET_VITA)

void np_vita_radio_pump(void);   // vlink_vita_radio.cpp: the radio, then np_vita_lan_frame (the link watch and NpNetPoll)
void NpNetFrame(void) { np_vita_radio_pump(); }

#else  // the desktop: POSIX UDP for Same Wi-Fi (vlink_udp.cpp), the online transport for Online; no ad hoc radio

void NpNetFrame(void) { NpNetPoll(); }

const char* NpLanPlayerName(void) {
    const char* n = getenv("SMG_LINK_NAME");
    return n ? n : "";
}

static VLink::Backend* g_udp = 0;
static bool g_udpFailed = false;

// Galaxy (mpwifi44): see np_glue.h. The desktop's only failure is its UDP socket: never "Wi-Fi is off".
int NpLanRadioFailKind(const char** why) {
    if (why) *why = g_udpFailed ? "the UDP link could not be set up" : "";
    return g_udpFailed ? 2 : 0;
}

int NpLanRadioPoll(int request) {
    const int online = NpNetRadioPoll(request);   // an online session was started (NpNetHost / NpNetJoin, or SMG_NET_MODE in a test)
    if (online != -2) return online;
    if (NpMenuLinkMode() == kNpMenuOnline) return request ? -1 : 0;   // online was chosen but no session exists: never fall back to UDP
    if (g_udp) return 2;
    if (!request) return 0;
    g_udp = VLink::CreateUdpBackendFromEnvironment();   // SMG_LINK_PORT (41100), SMG_LINK_BCAST, SMG_LINK_VIP
    if (!g_udp) {
        fprintf(stderr, "[np-glue] the UDP link could not be set up (SMG_LINK_BCAST?)\n");
        g_udpFailed = true;
        return -1;
    }
    g_udpFailed = false;
    VLink::Default().SetBackend(g_udp);
    return 2;
}

void NpLanRadioStop(void) {
    if (NpNetActive()) {
        NpNetRadioStop();
        return;
    }
    if (!g_udp) return;
    if (VLink::Default().GetBackend() == g_udp) {
        VLink::Default().Stop();
        VLink::Default().SetBackend(0);
    }
    VLink::DestroyUdpBackend(g_udp);
    g_udp = 0;
}

#endif

}  // extern "C"
