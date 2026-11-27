// Galaxy 3.0 multiplayer: Wave 3 P0 reconnect tests (KNOWN-ISSUES #19 & requestEnd race).
//   1. Unicast MP_PROBE rejoin when broadcast beacons are blocked by the network.
//   2. Direct m_rejoinAddr fallback after ~2 s when discovery port MP_LAN_PORT (21318) is completely blocked.
//   3. requestEnd(..., reconnect=true) publishes MP_UI_RECONNECTING + heldFlag BEFORE active flips false.
#include "core_test_util.h"
#include "../../platform/src/mp/mp_lan.h"

struct MpCoreAccess {
    static void callRequestEnd(MpCore* c, MpUiError err, const char* why, bool reconnect) {
        c->requestEnd(err, why, reconnect);
    }
    static void callFinishEnd(MpCore* c) {
        c->finishEnd();
        c->publishView();
    }
};

static MpCore* MakeCoreFlags(MpLoopHub& hub, uint32_t ep, const char* name, uint32_t seed, bool* activeFlag, bool* heldFlag) {
    MpCoreOptions o;
    memset(&o, 0, sizeof o);
    o.useThread = false;
    o.now = VNow;
    o.seed = seed + SeedAdd() * 7919u;
    o.activeFlag = activeFlag;
    o.heldFlag = heldFlag;
    MpCore* c = new MpCore(o);
    c->setLoopback(&hub, ep);
    c->setProfile(name, MP_LOOK_MARIO);
    return c;
}

template <class F>
static bool WaitUntil(std::vector<MpCore*>& v, F pred, int ms) {
    for (int t = 0; t < ms; t += 10) {
        if (pred()) return true;
        Step(v, 10);
    }
    return pred();
}

static void drainAll(std::vector<MpCore*>& v) {
    MpMsg m;
    MpCoreEvent e;
    for (MpCore* c : v) {
        if (!c) continue;
        while (c->poll(&m)) {}
        while (c->pollEvent(&e)) {}
    }
}

static void testUnicastProbeRejoinWhenBroadcastDropped() {
    g_log.clear();
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 101);
    MpCore* guest = MakeCore(hub, 2, "Guest", 202);
    std::vector<MpCore*> all = {host, guest};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, guest) && WaitPhase(all, guest, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, guest, MP_UI_PLAYING, 2000));
    const int origSlot = guest->localSlot();
    CHECK(origSlot >= 1);
    Step(all, 200);
    drainAll(all);

    // Block ALL broadcast traffic (both host broadcast beacons and guest broadcast probes):
    // guest can only rediscover the host via active unicast MP_PROBE to m_rejoinAddr / m_rejoinPeers!
    struct FilterCtx {
        int unicastProbes = 0;
        int unicastBeacons = 0;
        int droppedBroadcasts = 0;
    } ctx;
    hub.setFilter([](void* raw, const MpAddr& from, const MpAddr& to, const void* data, size_t len) -> bool {
        auto* c = (FilterCtx*)raw;
        if (to.host == MP_ADDR_BROADCAST) {
            c->droppedBroadcasts++;
            return false;
        }
        if (from.host == 2 && to.host == 1 && to.port == MP_LAN_PORT && mp_probe_read((const uint8_t*)data, len)) {
            c->unicastProbes++;
        }
        MpBeacon b;
        if (from.host == 1 && to.host == 2 && to.port == MP_LAN_PORT && mp_beacon_read((const uint8_t*)data, len, &b)) {
            c->unicastBeacons++;
        }
        return true;
    }, &ctx);

    // Trigger Wi-Fi loss on guest -> enters MP_UI_RECONNECTING and starts rejoin after 500 ms.
    guest->debugWatch(MP_WATCH_WIFI_LOST);
    CHECK(Phase(guest) == MP_UI_RECONNECTING);
    CHECK(!guest->active());
    CHECK(guest->debugHeld());

    // Must rejoin quickly via unicast MP_PROBE -> host unicast beacon (well before 2 s direct fallback!).
    CHECK(WaitUntil(all, [&] { return guest->active() && Phase(guest) == MP_UI_PLAYING; }, 2200));
    CHECK(guest->localSlot() == origSlot);
    CHECK(ctx.unicastProbes > 0);
    CHECK(ctx.unicastBeacons > 0);
    CHECK(ctx.droppedBroadcasts > 0);

    // Verify session traffic works after rejoin.
    uint8_t payload[4] = {1, 2, 3, 4};
    CHECK(guest->send(MP_MSG_STAR, payload, sizeof payload, true));
    bool got = WaitUntil(all, [&] {
        MpMsg m;
        while (host->poll(&m)) {
            if (m.type == MP_MSG_STAR && m.fromSlot == (uint8_t)origSlot) return true;
        }
        return false;
    }, 1000);
    CHECK(got);
    printf("unicast probe rejoin: probes=%d droppedBcast=%d\n", ctx.unicastProbes, ctx.droppedBroadcasts);

    hub.setFilter(nullptr, nullptr);
    delete guest;
    delete host;
}

static void testDirectFallbackWhenDiscoveryPortBlocked() {
    g_log.clear();
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 303);
    MpCore* guest = MakeCore(hub, 2, "Guest", 404);
    std::vector<MpCore*> all = {host, guest};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, guest) && WaitPhase(all, guest, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, guest, MP_UI_PLAYING, 2000));
    const int origSlot = guest->localSlot();
    Step(all, 200);
    drainAll(all);

    // Completely blackhole discovery port MP_LAN_PORT (21318) in BOTH directions!
    // Zero beacons can ever arrive; guest must fall back directly to m_rejoinAddr after 2 s.
    int blockedLan = 0;
    hub.setFilter([](void* raw, const MpAddr& from, const MpAddr& to, const void*, size_t) -> bool {
        if (to.port == MP_LAN_PORT || from.port == MP_LAN_PORT) {
            (*(int*)raw)++;
            return false;
        }
        return true;
    }, &blockedLan);

    guest->debugWatch(MP_WATCH_WIFI_LOST);
    CHECK(Phase(guest) == MP_UI_RECONNECTING);

    // 500 ms reconnect delay + 2000 ms probe window + handshake -> should succeed within 5 s.
    CHECK(WaitUntil(all, [&] { return guest->active() && Phase(guest) == MP_UI_PLAYING; }, 5000));
    CHECK(guest->localSlot() == origSlot);
    CHECK(blockedLan > 0);
    CHECK(LogCount("trying direct reconnect") >= 1);

    // Verify reliable message flows both ways after direct fallback rejoin.
    uint8_t payload[4] = {9, 8, 7, 6};
    CHECK(host->send(MP_MSG_WHERE, payload, sizeof payload, true));
    bool got = WaitUntil(all, [&] {
        MpMsg m;
        while (guest->poll(&m)) {
            if (m.type == MP_MSG_WHERE) return true;
        }
        return false;
    }, 1000);
    CHECK(got);
    printf("direct fallback rejoin: blockedLan=%d\n", blockedLan);

    hub.setFilter(nullptr, nullptr);
    delete guest;
    delete host;
}

static void testRequestEndPublishesReconnectingBeforeInactive() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    bool hostAct = false, hostHeld = false;
    bool guestAct = false, guestHeld = false;
    MpCore* host = MakeCoreFlags(hub, 1, "Host", 505, &hostAct, &hostHeld);
    MpCore* guest = MakeCoreFlags(hub, 2, "Guest", 606, &guestAct, &guestHeld);
    std::vector<MpCore*> all = {host, guest};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, guest) && WaitPhase(all, guest, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, guest, MP_UI_PLAYING, 2000));
    CHECK(guestAct && guestHeld);
    CHECK(Phase(guest) == MP_UI_PLAYING);

    // Window between requestEnd(..., reconnect=true) and finishEnd():
    // guestAct flips false, while uiStatus().phase MUST ALREADY be MP_UI_RECONNECTING and guestHeld MUST stay true!
    MpCoreAccess::callRequestEnd(guest, MP_ERR_NO_WIFI, "simulated drop", true);
    CHECK(!guestAct);
    CHECK(!guest->active());
    CHECK(guestHeld);
    CHECK(guest->debugHeld());
    CHECK(Phase(guest) == MP_UI_RECONNECTING);

    // After finishEnd(), still reconnecting and held.
    MpCoreAccess::callFinishEnd(guest);
    CHECK(!guestAct);
    CHECK(guestHeld);
    CHECK(Phase(guest) == MP_UI_RECONNECTING);

    // Contrast with non-reconnect requestEnd(..., reconnect=false) on host:
    // phase must NOT become MP_UI_RECONNECTING, and held MUST already drop to false immediately on requestEnd().
    CHECK(hostAct && hostHeld);
    MpCoreAccess::callRequestEnd(host, MP_ERR_HOST_LEFT, "explicit leave", false);
    CHECK(!hostAct);
    CHECK(!hostHeld);
    CHECK(!host->debugHeld());
    CHECK(Phase(host) != MP_UI_RECONNECTING);
    MpCoreAccess::callFinishEnd(host);
    CHECK(!hostAct && !hostHeld);
    CHECK(Phase(host) == MP_UI_IDLE);
    printf("requestEnd race check: ok\n");

    delete guest;
    delete host;
}

static void testDirectFallbackSwitchesToMigratedHostOnResume() {
    g_log.clear();
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 707);
    MpCore* p2 = MakeCore(hub, 2, "P2", 808);
    MpCore* p3 = MakeCore(hub, 3, "P3", 909);
    MpCore* p4 = MakeCore(hub, 4, "P4", 1010);
    std::vector<MpCore*> all = {host, p2, p3, p4};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, p2) && WaitPhase(all, p2, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, p3) && WaitPhase(all, p3, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, p4) && WaitPhase(all, p4, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, p2, MP_UI_PLAYING, 2000));
    CHECK(WaitPhase(all, p3, MP_UI_PLAYING, 2000));
    CHECK(WaitPhase(all, p4, MP_UI_PLAYING, 2000));
    Step(all, 200);
    drainAll(all);

    // P3 suspends cleanly (let the roster update reach P2/P4), then original host 1 vanishes while P2 migrates with P4.
    p3->suspend();
    CHECK(Phase(p3) == MP_UI_IDLE);
    Step(all, 300);
    hub.setDown(1, true);   // original host disappears

    // Keep P3 offline until its resume() enters the 3.5 s direct-reconnect attempt to dead host 1.
    hub.setDown(3, true);
    CHECK(p3->resume());
    Step(all, 4000);
    CHECK(Phase(p3) == MP_UI_CONNECTING);
    CHECK(LogCount("trying direct reconnect") >= 1);

    // Restore P3's link: P2 finishes host takeover (with P4 following) and beacons -> P3 switches from dead host 1 to migrated host 2!
    hub.setDown(3, false);
    CHECK(WaitUntil(all, [&] { return p2->active() && p2->isHost() && p3->active() && Phase(p3) == MP_UI_PLAYING; }, 22000));
    CHECK(p3->hostSlot() == p2->localSlot());
    CHECK(LogCount("migrated host beacon") >= 1);
    printf("direct fallback migrated-host switch: ok (host=%d)\n", p3->hostSlot());

    delete p4;
    delete p3;
    delete p2;
    delete host;
}

static void testDirectFallbackTimeoutReturnsToJoiningOnResume() {
    g_log.clear();
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 1111);
    MpCore* p2 = MakeCore(hub, 2, "P2", 1212);
    MpCore* p3 = MakeCore(hub, 3, "P3", 1313);
    MpCore* p4 = MakeCore(hub, 4, "P4", 1414);
    std::vector<MpCore*> all = {host, p2, p3, p4};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, p2) && WaitPhase(all, p2, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, p3) && WaitPhase(all, p3, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, p4) && WaitPhase(all, p4, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, p2, MP_UI_PLAYING, 2000));
    CHECK(WaitPhase(all, p3, MP_UI_PLAYING, 2000));
    CHECK(WaitPhase(all, p4, MP_UI_PLAYING, 2000));
    Step(all, 200);
    drainAll(all);

    p3->suspend();
    Step(all, 300);
    hub.setDown(1, true);   // original host dies; P2 migrates with P4

    // Keep P3 offline past BOTH the 3.5 s direct-fallback trigger AND the 10 s MP_CONNECT_TIMEOUT_MS!
    // resume() must NOT abort to MP_UI_IDLE; it must return to MP_UI_JOINING and still catch P2 when link returns at 15 s.
    hub.setDown(3, true);
    CHECK(p3->resume());
    Step(all, 4000);
    CHECK(Phase(p3) == MP_UI_CONNECTING);
    Step(all, 11000);   // direct fallback times out -> must return to MP_UI_JOINING, NOT MP_UI_IDLE!
    CHECK(Phase(p3) == MP_UI_JOINING);
    CHECK(LogCount("direct reconnect timed out, returning to beacon listen") >= 1);

    // Link returns inside the 30 s MP_LINK_TIMEOUT_MS window -> catches P2's beacon and rejoins!
    hub.setDown(3, false);
    CHECK(WaitUntil(all, [&] { return p2->active() && p2->isHost() && p3->active() && Phase(p3) == MP_UI_PLAYING; }, 10000));
    CHECK(p3->hostSlot() == p2->localSlot());
    printf("direct fallback timeout -> joining -> rejoin: ok (host=%d)\n", p3->hostSlot());

    delete p4;
    delete p3;
    delete p2;
    delete host;
}

int main() {
    testUnicastProbeRejoinWhenBroadcastDropped();
    testDirectFallbackWhenDiscoveryPortBlocked();
    testRequestEndPublishesReconnectingBeforeInactive();
    testDirectFallbackSwitchesToMigratedHostOnResume();
    testDirectFallbackTimeoutReturnsToJoiningOnResume();
    printf("reconnect_probe_test: %s (%d failed)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
