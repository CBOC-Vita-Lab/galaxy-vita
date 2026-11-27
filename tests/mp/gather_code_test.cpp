// core2: Play by Address from the gathering screen (port/mp_gather.h): the leader of a 3-console gathering room asks for a code; the
// room becomes a Play by Address room, its members follow (LAN address + secret, sealed); a friend joins with the code and the status
// shows the friend's room; a code with the wrong secret goes FAILED; Start works in the code room.
//   built by bcore.sh like core_tests (gather_code_test target); quick: run directly
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "port/mp_direct_code.h"
#include "port/mp_demo.h"

// wire: the platform's radio questions (platform/src/mp/mp_ui_flow.cpp defines them in the game): the driver asks them at start and shows
// the answers in the status, for the screen's "other radio" button. Cut that call and the status keeps the values of construction.
static bool gWifi = true, gNearby = false;
extern "C" bool mp_gather_wifi_connected(void) { return gWifi; }
extern "C" bool mp_gather_nearby_possible(void) { return gNearby; }

static void StepAll(std::vector<MpCore*>& cs, std::vector<MpGather*>& gs, int ms) {
    for (int k = 0; k < ms; k += 10) {
        Step(cs, 10, 5);
        for (MpGather* g : gs) g->tick(g_now);
    }
}

int main(int argc, char**) {
    g_echo = argc > 1;
    mp_log_set_sink(LogSink, true);
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    const char* names[5] = {"Ann", "Bob", "Cid", "Dee", "Eve"};
    std::vector<MpCore*> cs;
    std::vector<MpGather*> gs;
    for (int i = 0; i < 5; i++) {
        cs.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 900 + (uint32_t)i));
        gs.push_back(new MpGather(cs.back()));
    }
    // wire: the puppet demo's menu tour shows the gathering screen from the mock roster (no core, no radio): four heads, this console first
    // and the leader, Start possible, the code button offered; a roster that is off leaves the real gathering alone
    {
        MpGatherStatus ds;
        mp_demo_roster_enable(true);
        mp_demo_roster_local(2);
        mp_demo_roster_host(true);
        for (int i = 0; i < MP_MAX_PLAYERS; i++) mp_demo_roster_set(i, true, i, names[i], 30, false);
        mp_gather_status(&ds);
        CHECK(ds.state == MP_GATHER_GATHERING && ds.headCount == 4 && ds.canStart && (ds.heads[0].flags & MP_HEAD_SELF) && (ds.heads[0].flags & MP_HEAD_LEADER));
        CHECK(!strcmp(ds.heads[0].name, names[2]) && !strcmp(ds.heads[1].name, names[0]) && ds.heads[1].id != ds.heads[0].id && mp_gather_code_available());
        mp_demo_roster_enable(false);
        mp_gather_status(&ds);
        CHECK(ds.state == MP_GATHER_OFF && ds.headCount == 0 && !mp_gather_code_available());
    }
    gWifi = false, gNearby = true;   // (changed after the drivers were made)
    std::vector<MpCore*> three(cs.begin(), cs.begin() + 3);
    std::vector<MpGather*> g3(gs.begin(), gs.begin() + 3);
    for (int i = 0; i < 3; i++) CHECK(gs[i]->start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, g_now));
    {
        MpGatherStatus rs;
        gs[0]->status(&rs, g_now);
        CHECK(!rs.wifiConnected && rs.nearbyPossible);
    }
    int leader = -1;
    for (int t = 0; t < 20000 && leader < 0; t += 10) {
        StepAll(three, g3, 10);
        for (int i = 0; i < 3; i++)
            if (gs[i]->canStart(g_now)) leader = i;
    }
    CHECK(leader >= 0);
    if (leader < 0) return 1;
    for (int i = 0; i < 3; i++) CHECK(i == leader ? gs[i]->codeAvailable(g_now) : !gs[i]->codeAvailable(g_now));
    // the code: the first call switches the room; the code comes once the address is known
    char code[MP_DIRECT_CODE_MAX] = {0};
    bool got = gs[leader]->getCode(code, sizeof code, g_now);
    for (int t = 0; t < 10000 && !got; t += 10) {
        StepAll(three, g3, 10);
        got = gs[leader]->getCode(code, sizeof code, g_now);
    }
    printf("gather code: leader %s got code %s\n", names[leader], code);
    CHECK(got && strlen(code) >= 15);
    MpGatherStatus st;
    gs[leader]->status(&st, g_now);
    CHECK(!strcmp(st.code, code));
    // the members follow into the by-address room
    bool all3 = false;
    for (int t = 0; t < 15000 && !all3; t += 10) {
        StepAll(three, g3, 10);
        all3 = true;
        for (int i = 0; i < 3; i++) {
            gs[i]->status(&st, g_now);
            all3 = all3 && st.state == MP_GATHER_GATHERING && st.headCount == 3 && cs[i]->active();
        }
    }
    CHECK(all3);
    CHECK(cs[leader]->isHost());
    // a friend far away joins with the code (not gathering before): its status shows the friend's room
    std::vector<MpCore*> four(cs.begin(), cs.begin() + 4);
    std::vector<MpGather*> g4(gs.begin(), gs.begin() + 4);
    CHECK(gs[3]->joinCode(code, g_now) == MP_CODE_OK);
    bool in = false;
    for (int t = 0; t < 15000 && !in; t += 10) {
        StepAll(four, g4, 10);
        gs[3]->status(&st, g_now);
        in = st.state == MP_GATHER_GATHERING && st.headCount == 4;
    }
    CHECK(in);
    int leaders = 0;
    for (int i = 0; i < st.headCount; i++) leaders += (st.heads[i].flags & MP_HEAD_LEADER) ? 1 : 0;
    CHECK(leaders == 1 && !(st.heads[0].flags & MP_HEAD_LEADER));
    // the same address with a wrong secret: nobody answers; FAILED within kCodeJoinMs
    uint32_t ip;
    uint16_t port;
    uint8_t sec[MP_DIRECT_SECRET_BYTES];
    CHECK(mp_direct_code_decode(code, &ip, &port, sec) == MP_CODE_OK);
    sec[0] ^= 1;
    char wrong[MP_DIRECT_CODE_MAX];
    CHECK(mp_direct_code_encode(ip, port, sec, wrong, sizeof wrong) > 0);
    CHECK(gs[4]->joinCode(wrong, g_now) == MP_CODE_OK);
    uint64_t failedAt = 0;
    for (int t = 0; t < 30000 && !failedAt; t += 10) {
        StepAll(cs, gs, 10);
        gs[4]->status(&st, g_now);
        if (st.state == MP_GATHER_FAILED) failedAt = (uint64_t)t;
    }
    printf("gather code: a wrong secret gives FAILED after %llu ms\n", (unsigned long long)failedAt);
    CHECK(failedAt > 0 && failedAt <= MpGather::kCodeJoinMs + 1000);
    // Start in the code room
    StepAll(four, g4, 1500);
    CHECK(gs[leader]->begin());
    StepAll(four, g4, 1000);
    for (int i = 0; i < 4; i++) {
        gs[i]->status(&st, g_now);
        CHECK(st.state == MP_GATHER_STARTED);
    }
    for (MpGather* g : gs) delete g;
    for (MpCore* c : cs) delete c;
    printf("%s\n", g_fail ? "gather code test FAILED" : "gather code test: ok");
    return g_fail ? 1 : 0;
}
