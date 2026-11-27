// netfix B5a: the host of a GATHERING room (not started) goes; a guest takes over by migration. Its beacon must still say GATHERING, or
// the rooms nearby stop seeing it, and a console that arrives later cannot join it.
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "../../platform/src/mp/mp_lan.h"

int main() {
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    const char* names[4] = {"Ann", "Bob", "Cid", "Dee"};
    std::vector<MpCore*> cs;
    std::vector<MpGather*> gs;
    for (int i = 0; i < 3; i++) {
        cs.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 700 + (uint32_t)i));
        gs.push_back(new MpGather(cs.back()));
        gs.back()->start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, g_now);
    }
    int host = -1;
    for (int ms = 0; ms < 40000 && host < 0; ms += 10) {
        Step(cs, 10, 5);
        for (MpGather* g : gs) g->tick(g_now);
        int act = 0, h = -1;
        for (int i = 0; i < 3; i++) {
            if (cs[i]->active() && gs[i]->state() == MP_GATHER_GATHERING) act++;
            if (cs[i]->isHost()) h = i;
        }
        if (act == 3 && h >= 0 && gs[h]->canStart(g_now)) host = h;
    }
    CHECK(host >= 0);
    if (host < 0) return 1;
    // the host's console goes silent (nothing pumps it any more); the two others carry on
    std::vector<MpCore*> rest;
    std::vector<MpGather*> restG;
    for (int i = 0; i < 3; i++)
        if (i != host) { rest.push_back(cs[i]); restG.push_back(gs[i]); }
    int newHost = -1;
    for (int ms = 0; ms < 40000 && newHost < 0; ms += 10) {
        Step(rest, 10, 5);
        for (MpGather* g : restG) g->tick(g_now);
        for (size_t i = 0; i < rest.size(); i++) if (rest[i]->isHost() && rest[i]->active()) newHost = (int)i;
    }
    CHECK(newHost >= 0);
    if (newHost < 0) return 1;
    // an observer console (not in the room) scans: the room must show GATHERING and OPEN
    MpCore* obs = MakeCore(hub, 9, "Eve", 900);
    rest.push_back(obs);
    CHECK(obs->uiLocalScan(MP_CONN_LOOPBACK) >= 0);
    bool sawGathering = false, sawRoom = false;
    for (int ms = 0; ms < 8000; ms += 10) {
        Step(rest, 10, 5);
        for (MpGather* g : restG) g->tick(g_now);
        MpBeacon hb[16];
        const int n = obs->heardRooms(hb, nullptr, nullptr, 16);
        for (int k = 0; k < n; k++)
            if (hb[k].sessionId == rest[newHost]->debugSessionId()) {
                sawRoom = true;
                if (hb[k].flags & MP_BEACON_GATHERING) sawGathering = true;
            }
    }
    printf("gather migrate: the new host's room is heard=%d, with the GATHERING flag=%d, canStart=%d\n", sawRoom, sawGathering, restG[newHost]->canStart(g_now));
    CHECK(sawRoom);
    CHECK(sawGathering);
    CHECK(restG[newHost]->canStart(g_now));
    for (MpGather* g : gs) delete g;
    for (MpCore* c : cs) delete c;
    delete obs;
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("gather migrate test passed\n");
    return 0;
}
