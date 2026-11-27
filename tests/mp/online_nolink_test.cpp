// netport31: the host proof for Strikers onlogin23 ("no network backend / SOStartup -38" at the online login, console only).
// Strikers' login scene started its socket layer (so_net.cpp SOStartup) BEFORE signing in; on the console a link backend exists only once
// a radio is up or an online session was started, so the login failed before any request; the desktop never showed it because it installed
// its UDP test backend. Galaxy has no SO layer: the sign-in and every menu request are HTTPS (mp_service.cpp), and the online connector
// (mp_conn_online.cpp) opens the link only after NpNetHost / NpNetJoin installed the session's backend. This test runs that path under the
// CONSOLE's link rule on the host (no UDP fallback, no backend before a session: what -DNP_SO_CONSOLE_LINK_RULES=1 did for Strikers):
//   1. np_glue.cpp's desktop bring-up with online chosen and no session: never falls back to UDP (no backend appears);
//   2. the connector signs in, hosts and waits with NO backend at all, and never fails for it ("WORKING", never an error);
//   3. the backend appears without an address first (Start refused): still WORKING; once it has one: READY with a session socket.
//   bash tests/mp/online_nolink_build.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_config.h"
#include "mp_transport_vlink.h"
#include "port/mp_bridge.h"
#include "np_glue.h"
#include "vlink.h"

static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

MpConnector* mp_connector_online_create(const MpConnectorParams& p);

// ---- the online transport, as the connector sees it (net/ vlink_online.cpp), scripted ----------------------------------------------
static int gSigninPolls = 0, gHostCalls = 0, gPhase = 0, gPolls = 0, gLinkStartsBeforeSession = 0;
static bool gSession = false;
static uint32_t gVip = 0;   // 0 = the session's backend has no address yet
static uint32_t BeVip(void*) { return gVip; }
static int BeSend(void*, const VLink::PeerAddr*, bool, const void*, size_t len) { return int(len); }
static int BeRecv(void*, VLink::PeerAddr*, void*, size_t) { return 0; }
static void BeClose(void*) {}
static const VLink::Backend kSession = {"online (test)", 0, BeVip, BeSend, BeRecv, BeClose, 0};

extern "C" {
int mp_svc_signin_poll(void) { return ++gSigninPolls < 4 ? 0 : 1; }   // the sign-in takes a few frames (an HTTPS request on the console)
int NpNetHost(const char*) {
    ++gHostCalls;
    gSession = true;
    VLink::Default().SetBackend(&kSession);   // what InstallAndStart does: the session's backend, before any address is known
    gPhase = 1;
    return 1;
}
int NpNetJoin(const char*, const char*) { return 0; }
void NpNetPoll(void) {
    ++gPolls;
    if (!gSession && VLink::Default().IsUp()) ++gLinkStartsBeforeSession;
    if (gSession && gPolls > 12) gPhase = 3;   // NP_ROOM: the service made the room
}
void NpNetLeave(void) { gSession = false; }
int NpNetPhase(void) { return gPhase; }
const char* NpNetInviteCode(void) { return gPhase >= 3 ? "ABC234" : ""; }
// np_glue.cpp (desktop) asks the online transport whether a session exists: none here
int NpNetRadioPoll(int) { return -2; }
void NpNetRadioStop(void) {}
int NpNetActive(void) { return gSession ? 1 : 0; }
void NpNetOnLinkLost(const char*) {}
void NpNetPoll(void);
}

int main() {
    // 1. the console's rule in np_glue.cpp: online chosen, no session: no UDP backend, ever
    setenv("SMG_LAN_MODE", "online", 1);
    CHECK(NpMenuLinkMode() == kNpMenuOnline);
    CHECK(NpLanRadioPoll(0) == 0);
    CHECK(NpLanRadioPoll(1) == -1);
    CHECK(VLink::Default().GetBackend() == nullptr);
    CHECK(!VLink::Default().Start());   // (nothing to start on: exactly the console's state at the online menus)

    // 2 + 3. the connector: sign-in and hosting with no backend, then the session's backend without and with an address
    MpConnectorParams p;
    memset(&p, 0, sizeof p);
    p.host = true;
    snprintf(p.name, sizeof p.name, "Hosty");
    p.settings.maxPlayers = 4;
    MpConnector* c = mp_connector_online_create(p);
    int r = 0, polls = 0, errors = 0, workingNoBackend = 0;
    for (; polls < 200; ++polls) {
        if (polls == 30) gVip = 0x0A4D0001u;   // the session's address arrives (10.77.0.1)
        r = c->poll(uint64_t(polls) * 16);
        if (r < 0) ++errors;
        if (r == MP_CONNECTOR_WORKING && VLink::Default().GetBackend() == nullptr) ++workingNoBackend;
        if (r == MP_CONNECTOR_READY || r < 0) break;
    }
    printf("online connector under the console's link rule: sign-in polls %d, hosted %d, %d polls WORKING with no backend, READY after %d polls, "
           "errors %d\n", gSigninPolls, gHostCalls, workingNoBackend, polls, errors);
    CHECK(errors == 0);
    CHECK(gSigninPolls >= 4 && gHostCalls == 1);
    CHECK(workingNoBackend >= 3);              // the sign-in ran with no link backend at all
    CHECK(gLinkStartsBeforeSession == 0);      // and nothing started the link before the session
    CHECK(r == MP_CONNECTOR_READY && polls >= 30);   // not before the backend had an address
    CHECK(c->session() != nullptr && VLink::Default().IsUp());
    delete c;
    VLink::Default().Stop();
    VLink::Default().SetBackend(nullptr);
    printf("online_nolink_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
