// Galaxy 3.0 multiplayer: host end-to-end test of group-restart world-sync races (KNOWN-ISSUES 16 & 17).
//   1. Pre-restart KILL/PURPLE/CHIP sent by a guest before it processes MP_MSG_RESTART arrives at host before post-restart REGCRC:
//      host drops it before delivery AND before relay to other guests; post-restart events after REGCRC deliver normally; next-serial events pass.
//   2. A guest disconnected (link-lost held slot) during a group restart receives MP_MSG_RESTART on rejoin while the session is still on
//      the same scene serial, has pre-restart events gated until its post-restart REGCRC, does NOT receive a duplicate RESTART on a later
//      rejoin after completing REGCRC, and a brand-new joiner never receives a stale RESTART.
#include "core_test_util.h"
#include "Game/MP/MpWorldLogic.hpp"

using namespace MpWorldLogic;

static void sendWhereSerial(MpCore* host, uint16_t serial) {
    uint8_t b[8] = {};
    b[0] = 1; b[1] = 1;
    b[6] = (uint8_t)(serial & 0xFFu);
    b[7] = (uint8_t)(serial >> 8);
    host->setWhere(b, sizeof b);
    CHECK(host->send(MP_MSG_WHERE, b, sizeof b, true));
}

static void sendRestart(MpCore* host, uint16_t serial) {
    uint8_t b[2] = {(uint8_t)(serial & 0xFFu), (uint8_t)(serial >> 8)};
    CHECK(host->send(MP_MSG_RESTART, b, sizeof b, true));
}

static void sendObjEvt(MpCore* c, uint8_t type, uint16_t serial, uint16_t lid) {
    uint8_t b[kObjLen] = {};
    MpObjKey k = {};
    k.zone = 1; k.nameHash = 0x1234; k.lid = lid; k.sub = 0;
    ObjMsg m = {serial, k};
    const size_t n = writeObj(b, sizeof b, m);
    CHECK(n == kObjLen);
    CHECK(c->send(type, b, (uint16_t)n, true));
}

static void sendRegCrcEvt(MpCore* c, uint16_t serial, uint32_t crc) {
    uint8_t b[kRegCrcLen] = {};
    RegCrcMsg m = {serial, crc, 0};
    const size_t n = writeRegCrc(b, sizeof b, m);
    CHECK(n == kRegCrcLen);
    CHECK(c->send(MP_MSG_REGCRC, b, (uint16_t)n, true));
}

static void drain(std::vector<MpCore*>& all) {
    MpMsg m;
    MpCoreEvent e;
    for (MpCore* c : all) {
        if (!c) continue;
        while (c->poll(&m)) {}
        while (c->pollEvent(&e)) {}
    }
}

template <class F>
static bool WaitUntil(std::vector<MpCore*>& v, F pred, int ms) {
    for (int t = 0; t < ms; t += 20) {
        if (pred()) return true;
        Step(v, 20);
    }
    return pred();
}

static void testPreRestartDropAndPostPass() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 11);
    MpCore* ga = MakeCore(hub, 2, "GuestA", 22);
    MpCore* gb = MakeCore(hub, 3, "GuestB", 33);
    std::vector<MpCore*> all = {host, ga, gb};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, ga) && WaitPhase(all, ga, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, gb) && WaitPhase(all, gb, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, ga, MP_UI_PLAYING, 2000) && WaitPhase(all, gb, MP_UI_PLAYING, 2000));
    Step(all, 200);
    sendWhereSerial(host, 7);
    Step(all, 200);
    drain(all);

    // Host initiates group restart on serial 7
    sendRestart(host, 7);
    // Before Guest A sends post-restart REGCRC(7), Guest A emits stale pre-restart KILL, PURPLE, CHIP for serial 7
    sendObjEvt(ga, MP_MSG_KILL, 7, 100);
    sendObjEvt(ga, MP_MSG_PURPLE, 7, 101);
    sendObjEvt(ga, MP_MSG_CHIP, 7, 102);
    // An event for next serial 8 must NOT be blocked by serial 7's restart gate
    sendObjEvt(ga, MP_MSG_KILL, 8, 200);
    Step(all, 300);

    int hostS7 = 0, hostS8 = 0, gbS7 = 0, gbS8 = 0, gaRestarts = 0, gbRestarts = 0;
    MpMsg m;
    while (host->poll(&m)) {
        if (m.type == MP_MSG_KILL || m.type == MP_MSG_PURPLE || m.type == MP_MSG_CHIP) {
            const uint16_t ser = (uint16_t)(m.data[0] | (m.data[1] << 8));
            if (ser == 7) hostS7++;
            if (ser == 8) hostS8++;
        }
    }
    while (gb->poll(&m)) {
        if (m.type == MP_MSG_RESTART) gbRestarts++;
        if (m.type == MP_MSG_KILL || m.type == MP_MSG_PURPLE || m.type == MP_MSG_CHIP) {
            const uint16_t ser = (uint16_t)(m.data[0] | (m.data[1] << 8));
            if (ser == 7) gbS7++;
            if (ser == 8) gbS8++;
        }
    }
    while (ga->poll(&m)) {
        if (m.type == MP_MSG_RESTART) gaRestarts++;
    }
    CHECK(gaRestarts == 1 && gbRestarts == 1);
    CHECK(hostS7 == 0 && gbS7 == 0);   // pre-restart KILL/PURPLE/CHIP dropped on host AND never relayed to Guest B
    CHECK(hostS8 == 1 && gbS8 == 1);   // next-serial event untouched

    // Guest A reloads and sends post-restart REGCRC(7), then sends a new post-restart KILL(7)
    sendRegCrcEvt(ga, 7, 0xCAFEBABEu);
    sendObjEvt(ga, MP_MSG_KILL, 7, 300);
    Step(all, 300);

    int hostPost = 0, gbPost = 0;
    while (host->poll(&m)) if (m.type == MP_MSG_KILL) hostPost++;
    while (gb->poll(&m)) if (m.type == MP_MSG_KILL) gbPost++;
    CHECK(hostPost == 1 && gbPost == 1);

    for (MpCore* c : all) delete c;
}

static void testReconnectDuringGroupRestart() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* host = MakeCore(hub, 1, "Host", 101);
    MpCore* ga = MakeCore(hub, 2, "GuestA", 102);
    MpCore* gb = MakeCore(hub, 3, "GuestB", 103);
    std::vector<MpCore*> all = {host, ga, gb};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(host->uiHost(&hp));
    CHECK(WaitPhase(all, host, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, ga) && WaitPhase(all, ga, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, gb) && WaitPhase(all, gb, MP_UI_LOBBY, 3000));
    CHECK(host->uiStart());
    CHECK(WaitPhase(all, ga, MP_UI_PLAYING, 2000) && WaitPhase(all, gb, MP_UI_PLAYING, 2000));
    Step(all, 200);
    sendWhereSerial(host, 12);
    Step(all, 200);
    drain(all);

    // Cut Guest B's link by stopping pumps for Guest B until host times it out and holds its reservation
    const int gbSlot = gb->localSlot();
    CHECK(gbSlot > 0);
    std::vector<MpCore*> withoutB = {host, ga};
    Step(withoutB, MP_PEER_LOST_MS + 1500);
    CHECK(!host->slotPresent(gbSlot));   // timed out, slot held in m_res

    // While Guest B is away, host triggers a group restart on serial 12
    sendRestart(host, 12);
    Step(withoutB, 300);
    sendRegCrcEvt(ga, 12, 0x11111111u);
    Step(withoutB, 200);
    drain(withoutB);

    // Guest B wakes up, detects lost host, and reconnects to its held slot on the same console key
    uint8_t gbKey[16];
    gb->debugConsoleKey(gbKey);
    const uint32_t gbWire = gb->debugWireId();
    (void)gbWire;
    // Recreate Guest B with the same persistent console key to simulate a clean rejoin after Wi-Fi drop
    delete gb;
    MpCoreOptions o;
    memset(&o, 0, sizeof o);
    o.useThread = false;
    o.now = VNow;
    o.seed = 203;
    gb = new MpCore(o);
    gb->setConsoleKey(gbKey);
    gb->setLoopback(&hub, 3);
    gb->setProfile("GuestB", MP_LOOK_MARIO);
    all[2] = gb;

    CHECK(ScanJoin(all, gb) && WaitPhase(all, gb, MP_UI_PLAYING, 3000));
    CHECK(gb->localSlot() == gbSlot);   // rejoined its held slot
    Step(all, 300);

    // Guest B MUST receive MP_MSG_RESTART(12) so it clears its stale pre-restart world!
    int gotRestart = 0;
    uint16_t gotSerial = 0;
    MpMsg m;
    while (gb->poll(&m)) {
        if (m.type == MP_MSG_RESTART && m.len == 2) {
            gotRestart++;
            gotSerial = (uint16_t)(m.data[0] | (m.data[1] << 8));
        }
    }
    CHECK(gotRestart == 1 && gotSerial == 12);

    // Before Guest B sends post-restart REGCRC(12), a stale pre-restart KILL(12) from Guest B is blocked
    sendObjEvt(gb, MP_MSG_KILL, 12, 555);
    Step(all, 250);
    int leaked = 0;
    while (host->poll(&m)) if (m.type == MP_MSG_KILL) leaked++;
    while (ga->poll(&m)) if (m.type == MP_MSG_KILL) leaked++;
    CHECK(leaked == 0);

    // Guest B reloads and sends REGCRC(12), then a post-restart KILL(12) succeeds
    sendRegCrcEvt(gb, 12, 0x22222222u);
    sendObjEvt(gb, MP_MSG_KILL, 12, 556);
    Step(all, 250);
    int postHost = 0, postGa = 0;
    while (host->poll(&m)) if (m.type == MP_MSG_KILL) postHost++;
    while (ga->poll(&m)) if (m.type == MP_MSG_KILL) postGa++;
    CHECK(postHost == 1 && postGa == 1);

    // A brand-new player (Guest C) joining fresh after the restart must NOT receive a spurious RESTART
    MpCore* gc = MakeCore(hub, 4, "GuestC", 104);
    all.push_back(gc);
    CHECK(ScanJoin(all, gc) && WaitPhase(all, gc, MP_UI_PLAYING, 3000));
    Step(all, 300);
    int gcRestarts = 0;
    while (gc->poll(&m)) if (m.type == MP_MSG_RESTART) gcRestarts++;
    CHECK(gcRestarts == 0);

    // If Guest B disconnects and rejoins AGAIN after already completing REGCRC(12), no duplicate RESTART is sent
    std::vector<MpCore*> withoutB2 = {host, ga, gc};
    Step(withoutB2, MP_PEER_LOST_MS + 1500);
    CHECK(!host->slotPresent(gbSlot));
    delete gb;
    gb = new MpCore(o);
    gb->setConsoleKey(gbKey);
    gb->setLoopback(&hub, 3);
    gb->setProfile("GuestB", MP_LOOK_MARIO);
    all[2] = gb;
    CHECK(ScanJoin(all, gb) && WaitPhase(all, gb, MP_UI_PLAYING, 3000));
    Step(all, 300);
    int dupRestart = 0;
    while (gb->poll(&m)) if (m.type == MP_MSG_RESTART) dupRestart++;
    CHECK(dupRestart == 0);

    for (MpCore* c : all) delete c;
}

static void testCatchUpSkipsSuppressedPreRestart() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h0 = MakeCore(hub, 1, "Host0", 301);
    MpCore* ga = MakeCore(hub, 2, "GuestA", 302);
    MpCore* gb = MakeCore(hub, 3, "GuestB", 303);
    MpCore* gc = MakeCore(hub, 4, "GuestC", 304);
    std::vector<MpCore*> all = {h0, ga, gb, gc};

    MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
    CHECK(h0->uiHost(&hp));
    CHECK(WaitPhase(all, h0, MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, ga) && WaitPhase(all, ga, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, gb) && WaitPhase(all, gb, MP_UI_LOBBY, 3000));
    CHECK(ScanJoin(all, gc) && WaitPhase(all, gc, MP_UI_LOBBY, 3000));
    CHECK(h0->uiStart());
    CHECK(WaitPhase(all, ga, MP_UI_PLAYING, 2000) && WaitPhase(all, gb, MP_UI_PLAYING, 2000) && WaitPhase(all, gc, MP_UI_PLAYING, 2000));
    Step(all, 200);
    sendWhereSerial(h0, 9);
    Step(all, 200);
    drain(all);

    // Host0 leaves cleanly -> GuestA becomes host; hold GuestC back briefly so it resumes AFTER a restart-gate drop
    h0->uiLeave();
    std::vector<MpCore*> ab = {h0, ga, gb};
    CHECK(WaitUntil(ab, [&] { return ga->isHost() && gb->hostSlot() == ga->localSlot(); }, 6000));
    Step(ab, 200);
    drain(ab);

    // New host GuestA triggers group restart on serial 9 while GuestC has not resumed yet
    sendRestart(ga, 9);
    // GuestB sends stale pre-restart KILL(9, lid=701) -> host drops & marks HistEntry::suppressed
    sendObjEvt(gb, MP_MSG_KILL, 9, 701);
    Step(ab, 200);
    // GuestB reloads, sends REGCRC(9), then sends genuine post-restart KILL(9, lid=702)
    sendRegCrcEvt(gb, 9, 0x33333333u);
    sendObjEvt(gb, MP_MSG_KILL, 9, 702);
    Step(ab, 200);
    drain(ab);

    // Now GuestC resumes to GuestA: sendCatchUp MUST keep seq contiguity (no MP_BYE_RESYNC), skip lid=701, deliver lid=702 + RESTART(9)
    std::vector<MpCore*> abc = {ga, gb, gc};
    CHECK(WaitUntil(abc, [&] { return gc->hostSlot() == ga->localSlot() && gc->active(); }, 4000));
    Step(abc, 400);
    CHECK(gc->active() && gc->hostSlot() == ga->localSlot());

    int gcRestart = 0, gcPreKill = 0, gcPostKill = 0;
    MpMsg m;
    while (gc->poll(&m)) {
        if (m.type == MP_MSG_RESTART && m.len == 2) {
            const uint16_t ser = (uint16_t)(m.data[0] | (m.data[1] << 8));
            if (ser == 9) gcRestart++;
        } else if (m.type == MP_MSG_KILL) {
            ObjMsg om;
            if (readObj(m.data, m.len, 32, &om)) {
                if (om.key.lid == 701) gcPreKill++;
                if (om.key.lid == 702) gcPostKill++;
            }
        }
    }
    CHECK(gcRestart == 1);
    CHECK(gcPreKill == 0);
    CHECK(gcPostKill == 1);

    for (MpCore* c : all) delete c;
}

int main() {
    mp_log_set_sink(LogSink, false);
    testPreRestartDropAndPostPass();
    testReconnectDuringGroupRestart();
    testCatchUpSkipsSuppressedPreRestart();
    if (g_fail) {
        fprintf(stderr, "restart_sync_test FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("restart_sync_test: all passed\n");
    return 0;
}
