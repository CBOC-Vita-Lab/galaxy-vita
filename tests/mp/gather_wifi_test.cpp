// mpwifi44: Play Together when the Same Wi-Fi link cannot be opened (the owner's console: "Couldn't connect to Wi-Fi. Check your Wi-Fi
// settings." although the Vita was on Wi-Fi and the connection check said the backup route works).
//
// The real gathering driver and the real core, with the Vita's link hook (mp_net_link_poll / mp_net_link_fail of mp_transport.h) scripted
// here: the hook answers what net/src/vlink_vita.cpp answers on the console.
//   1. Wi-Fi off (the link says "not connected"): no gathering, the error is MP_ERR_NO_WIFI, the log names "link-wifi-down".
//   2. The failure does not stay: the next start with Wi-Fi back works (it used to be refused until the game was restarted).
//   3. Wi-Fi connected but the Same Wi-Fi socket cannot be opened: the gathering starts without the local link; the far-away door works
//      (a code, a friend joins by it); the link is tried again and the room opens when it works.
//   4. Wi-Fi connected, the link cannot be opened and the gathering without a link is switched off (SMG_MP_GATHER_NOLAN=0): the error is
//      MP_ERR_INTERNAL ("Something went wrong"), never the Wi-Fi message.
//   5. Without a local link and then Wi-Fi goes: FAILED with MP_ERR_NO_WIFI.
//   built by tests/mp/gather_core_build.sh (gather_wifi_test); quick: run directly
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "../../platform/src/mp/mp_transport_vlink.h"
#include "port/mp_direct_code.h"
#include "vlink.h"

#include <unistd.h>

static bool gWifi = true, gNearby = false;
extern "C" bool mp_gather_wifi_connected(void) { return gWifi; }
extern "C" bool mp_gather_nearby_possible(void) { return gNearby; }

// ---- the Vita's link hook, scripted ---------------------------------------------------------------------------------------------
enum LinkScript { LINK_OK, LINK_WIFI_DOWN, LINK_BIND_REFUSED, LINK_WORKING_THEN_BIND_REFUSED };
static LinkScript gLink = LINK_OK;
static int gPolls = 0, gReleases = 0, gFailKind = MP_LINK_FAIL_UNKNOWN;
static const char* gFailWhy = "";
static VLink::Backend* gBackend = 0;
static VLink::Link* gLinkObj = 0;

extern "C" int mp_net_link_poll(int conn, int request, void** linkOut) {
    (void)request;
    gPolls++;
    if (conn == MP_CONN_NEARBY) {   // (no ad hoc radio in this test: its own failure, no kind)
        gFailKind = MP_LINK_FAIL_UNKNOWN;
        gFailWhy = "the ad hoc start was refused (test)";
        return -1;
    }
    switch (gLink) {
    case LINK_WIFI_DOWN:
        gFailKind = MP_LINK_FAIL_WIFI_DOWN;
        gFailWhy = "Wi-Fi is not connected (state 0)";
        return -1;
    case LINK_WORKING_THEN_BIND_REFUSED:
        gLink = LINK_BIND_REFUSED;
        return 1;
    case LINK_BIND_REFUSED:
        gFailKind = MP_LINK_FAIL_OTHER;
        gFailWhy = "bind to UDP port 41100 failed 0x80410130";
        return -1;
    default: break;
    }
    if (!gLinkObj) {
        const int port = 43000 + (int)(getpid() % 2000);
        gBackend = VLink::CreateUdpBackend((uint16_t)port, "127.0.0.1:1", 0x0A000042u);
        if (!gBackend) return -1;
        gLinkObj = new VLink::Link();
        gLinkObj->SetBackend(gBackend);
        gLinkObj->SetFingerprint(MP_LINK_FINGERPRINT);
        if (!gLinkObj->Start()) return -1;
    }
    if (!gLinkObj->IsUp() && !gLinkObj->Start()) return 1;
    if (linkOut) *linkOut = gLinkObj;
    return 2;
}
extern "C" void mp_net_link_release(int) {
    gReleases++;
    if (gLinkObj) gLinkObj->Stop();
}
extern "C" int mp_net_link_fail(int, const char** why) {
    if (why) *why = gFailWhy;
    return gFailKind;
}

static void StepAll(std::vector<MpCore*>& cs, std::vector<MpGather*>& gs, int ms) {
    for (int k = 0; k < ms; k += 10) {
        Step(cs, 10, 5);
        for (MpGather* g : gs) g->tick(g_now);
    }
}
static MpGatherStatus St(MpGather* g) {
    MpGatherStatus s;
    g->status(&s, g_now);
    return s;
}

int main(int argc, char**) {
    g_echo = argc > 1;
    mp_log_set_sink(LogSink, true);
    unsetenv("SMG_MP_GATHER_NOLAN");
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> cs;
    std::vector<MpGather*> gs;
    const char* names[3] = {"Ann", "Bob", "Cid"};
    for (int i = 0; i < 3; i++) {
        cs.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 700 + (uint32_t)i));
        gs.push_back(new MpGather(cs.back()));
    }
    std::vector<MpCore*> one(cs.begin(), cs.begin() + 1);
    std::vector<MpGather*> g1(gs.begin(), gs.begin() + 1);
    MpGather* g = gs[0];
    MpCore* c = cs[0];

    // ---- 1. Wi-Fi off: the Wi-Fi message, and only then -----------------------------------------------------------------------------
    // (a) the system says the console is connected (or cannot tell: the Vita before the network control layer is up), the link finds it is not
    gWifi = true;
    gLink = LINK_WIFI_DOWN;
    CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->lastError() == MP_ERR_NO_WIFI && !strcmp(g->lastReason(), "link-wifi-down"));
    CHECK(g->state() == MP_GATHER_OFF && !g->noLan());   // (not FAILED: nothing stays)
    CHECK(LogCount("gather: failed reason=link-wifi-down") == 1);
    CHECK(LogCount("Wi-Fi is not connected (state 0)") >= 1);   // (the link's own sentence is in the log)
    CHECK(Phase(c) == MP_UI_IDLE);
    // (b) the system itself says Wi-Fi is off: Nearby is tried (as before); it fails here: the same message, its own reason
    gWifi = false;
    CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->lastError() == MP_ERR_NO_WIFI && !strcmp(g->lastReason(), "link-nearby-failed") && g->state() == MP_GATHER_OFF);
    // (c) the system says off, whatever the link says: the Wi-Fi message
    gLink = LINK_BIND_REFUSED;
    CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_WIFI, g_now));
    CHECK(g->lastError() == MP_ERR_NO_WIFI && !strcmp(g->lastReason(), "link-wifi-down") && g->state() == MP_GATHER_OFF);

    // ---- 2. the failure does not stay (the owner's console: the message came back every time until the game was restarted) ---------
    gWifi = true;
    gLink = LINK_WIFI_DOWN;
    for (int i = 0; i < 3; i++) CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    gLink = LINK_OK;   // Wi-Fi is back
    const int pollsBefore = gPolls;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(gPolls > pollsBefore);   // (the link was really tried again, not remembered as failed)
    CHECK(g->lastError() == MP_ERR_NONE && g->state() == MP_GATHER_LISTENING && !g->noLan());
    bool room = false;
    for (int t = 0; t < 6000 && !room; t += 10) {
        StepAll(one, g1, 10);
        room = g->state() == MP_GATHER_GATHERING && c->active() && c->isHost();
    }
    CHECK(room);
    {
        const MpGatherStatus st = St(g);
        CHECK(st.conn == MP_CONN_WIFI && st.headCount == 1 && (st.heads[0].flags & MP_HEAD_LEADER) && !strcmp(st.heads[0].name, "Ann"));
    }
    g->stop();
    Step(one, 200);
    CHECK(g->state() == MP_GATHER_OFF && Phase(c) == MP_UI_IDLE);

    // ---- 3. Wi-Fi connected, the Same Wi-Fi socket cannot be opened: gathering without the local link ------------------------------
    gLink = LINK_BIND_REFUSED;
    g_log.clear();
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->noLan() && g->state() == MP_GATHER_GATHERING && g->lastError() == MP_ERR_NONE);
    CHECK(LogCount("the Same Wi-Fi link could not be opened though Wi-Fi is connected (bind to UDP port 41100 failed 0x80410130)") == 1);
    CHECK(LogCount("scan: wifi link failed (internal): bind to UDP port 41100 failed 0x80410130") == 1);
    {
        const MpGatherStatus st = St(g);
        // the gathering screen: this console alone, its own name, the leader (the code button), Wi-Fi connected (Play with Anyone), no Start
        CHECK(st.state == MP_GATHER_GATHERING && st.headCount == 1 && (st.heads[0].flags & MP_HEAD_SELF) && (st.heads[0].flags & MP_HEAD_LEADER));
        CHECK(!strcmp(st.heads[0].name, "Ann") && st.wifiConnected && !st.canStart && !st.code[0]);
        CHECK(!c->active() && Phase(c) == MP_UI_IDLE);
        CHECK(g->codeAvailable(g_now));
    }
    // nothing is asked of a router, a STUN server or the service by the gathering itself (PRIVACY.md): the core stays idle until the code is asked for
    StepAll(one, g1, 2000);
    CHECK(!c->active() && Phase(c) == MP_UI_IDLE && g->noLan());
    // the far-away door: the code, and a friend joins by it
    char code[MP_DIRECT_CODE_MAX] = {0};
    bool got = g->getCode(code, sizeof code, g_now);
    for (int t = 0; t < 10000 && !got; t += 10) {
        StepAll(one, g1, 10);
        got = g->getCode(code, sizeof code, g_now);
    }
    CHECK(got && strlen(code) >= 15 && !g->noLan());
    CHECK(c->active() && c->isHost() && St(g).conn == MP_CONN_DIRECT);
    std::vector<MpCore*> two(cs.begin(), cs.begin() + 2);
    std::vector<MpGather*> g2(gs.begin(), gs.begin() + 2);
    CHECK(gs[1]->joinCode(code, g_now) == MP_CODE_OK);
    bool both = false;
    for (int t = 0; t < 15000 && !both; t += 10) {
        StepAll(two, g2, 10);
        both = St(g).headCount == 2 && St(gs[1]).headCount == 2 && St(gs[1]).state == MP_GATHER_GATHERING && cs[1]->active();
    }
    CHECK(both);
    bool canStart = false;
    for (int t = 0; t < 5000 && !canStart; t += 10) {
        StepAll(two, g2, 10);
        canStart = g->canStart(g_now);
    }
    CHECK(canStart);   // (the leader's Press START, with the friend who came through the far-away door)
    gs[1]->stop();
    g->stop();
    for (int t = 0; t < 5000 && (Phase(c) != MP_UI_IDLE || Phase(cs[1]) != MP_UI_IDLE); t += 10) Step(two, 10);   // (the goodbyes go out first)
    CHECK(g->state() == MP_GATHER_OFF && Phase(c) == MP_UI_IDLE && Phase(cs[1]) == MP_UI_IDLE);

    // the link is tried again while gathering without it, and the room opens when it works
    gLink = LINK_BIND_REFUSED;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->noLan());
    const int pollsAt = gPolls;
    StepAll(one, g1, (int)MpGather::kLanRetryMs * 2 + 200);
    CHECK(gPolls >= pollsAt + 2 && gPolls <= pollsAt + 3 && g->noLan());   // (once per kLanRetryMs: not every frame)
    gLink = LINK_OK;
    room = false;
    for (int t = 0; t < 9000 && !room; t += 10) {
        StepAll(one, g1, 10);
        room = !g->noLan() && g->state() == MP_GATHER_GATHERING && c->active() && c->isHost();
    }
    CHECK(room);
    CHECK(LogCount("gather: the Same Wi-Fi link opens now") == 1);
    CHECK(St(g).conn == MP_CONN_WIFI);
    g->stop();
    Step(one, 200);

    // a bring-up that takes more than one poll and then fails (the Vita: the ad hoc radio of an earlier session still closing): the same
    gLink = LINK_WORKING_THEN_BIND_REFUSED;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->state() == MP_GATHER_LISTENING && !g->noLan());
    StepAll(one, g1, 100);
    CHECK(g->noLan() && g->state() == MP_GATHER_GATHERING && g->lastError() == MP_ERR_NONE);
    g->stop();
    Step(one, 200);

    // ---- 4. the link cannot be opened and gathering without it is off: an accurate error, never the Wi-Fi message -----------------
    setenv("SMG_MP_GATHER_NOLAN", "0", 1);
    gLink = LINK_BIND_REFUSED;
    g_log.clear();
    CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->lastError() == MP_ERR_INTERNAL && !strcmp(g->lastReason(), "link-open-failed") && g->state() == MP_GATHER_OFF && !g->noLan());
    CHECK(LogCount("gather: failed reason=link-open-failed error=14 conn=wifi wifi=1") == 1);
    CHECK(LogCount("detail=bind to UDP port 41100 failed 0x80410130") == 1);
    // (and that one does not stay either)
    gLink = LINK_OK;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    g->stop();
    Step(one, 200);
    unsetenv("SMG_MP_GATHER_NOLAN");

    // ---- 5. without a local link, then Wi-Fi goes: the Wi-Fi message ----------------------------------------------------------------
    gLink = LINK_BIND_REFUSED;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->noLan());
    gWifi = false;
    StepAll(one, g1, (int)MpGather::kLanRetryMs + 200);
    CHECK(g->state() == MP_GATHER_FAILED && g->lastError() == MP_ERR_NO_WIFI && !strcmp(g->lastReason(), "link-wifi-down"));
    // (the menu stops a failed gathering; one that was not stopped no longer blocks the next start)
    gWifi = true;
    gLink = LINK_OK;
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(g->state() == MP_GATHER_LISTENING);
    g->stop();
    Step(one, 200);

    // ---- a start refused for another reason says which (it used to read "Couldn't connect to Wi-Fi" too) ---------------------------
    CHECK(g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));
    CHECK(!g->start(MP_MODE_TOGETHER, MP_CONN_NONE, g_now));   // (already gathering: refused, the running one is not touched)
    CHECK(g->lastError() == MP_ERR_INTERNAL && !strcmp(g->lastReason(), "already-gathering") && g->state() == MP_GATHER_LISTENING);
    g->stop();
    Step(one, 200);

    for (MpGather* x : gs) delete x;
    for (MpCore* x : cs) delete x;
    if (gLinkObj) { gLinkObj->Stop(); delete gLinkObj; }
    if (gBackend) VLink::DestroyUdpBackend(gBackend);
    printf("gather_wifi_test: %s (%d failed)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
