// corefix2: tests of the session core's second review round (included by core_tests.cpp after its helpers).
#pragma once

#include "raw_joiner.h"

#include <time.h>

// 1. A takeover never strands a live guest for lack of a reliable window. 4 players; the host and slot 1 die together; the claimant (slot 2,
// second in line, so it holds no spare window) can get no further window at all (debugFailRelAlloc). Before: the dead slot 1 took the
// claimant's only window, the live slot 3 got none ("takeover: no reliable window"), nobody followed, both survivors left the session.
static void TestTakeoverNoWindow() {
    for (int off : {150, 800, 2700}) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 940 + (uint32_t)off);
        all[0]->uiStart();
        Step(all, 1500);
        MpCore::debugFailRelAlloc(100000);
        Step(all, off);
        hub.setDown(1, true);
        hub.setDown(2, true);
        std::vector<MpCore*> live = {all[2], all[3]};
        Step(live, 40000);
        MpCore::debugFailRelAlloc(0);
        CHECK(all[2]->active() && all[3]->active());
        CHECK((all[2]->isHost() ? 1 : 0) + (all[3]->isHost() ? 1 : 0) == 1);
        CHECK(all[2]->hostSlot() == all[3]->hostSlot());
        for (MpCore* c : live) CHECK(!c->slotPresent(0) && !c->slotPresent(1) && c->slotPresent(2) && c->slotPresent(3));
        Free(all);
    }
    printf("takeover without a window: ok\n");
}

// 2. Bans survive a host takeover: a console the host removed and then the host vanishes; the new host still refuses it. Before: m_kicked was
// host-local and the removed player rejoined the new host at once. Also: the ban list rides in the roster (a joiner after the kick has it).
static void TestBanSurvivesTakeover() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 960);   // Player (host), Luigi (successor), Peach (kicked), Toad
    all[0]->uiStart();
    Step(all, 1500);
    all[0]->uiKick(all[2]->localSlot());
    Step(all, 1500);
    CHECK(!all[2]->active() && Error(all[2]) == MP_ERR_KICKED);
    CHECK(ScanJoin(all, all[2]));
    Step(all, 2500);
    CHECK(!all[2]->active());   // refused by the original host
    hub.setDown(1, true);       // the host vanishes
    std::vector<MpCore*> live = {all[1], all[2], all[3]};
    Step(live, 15000);
    CHECK(all[1]->isHost() && all[3]->active() && !all[1]->slotPresent(2));
    CHECK(ScanJoin(live, all[2], 6000));
    Step(live, 4000);
    CHECK(!all[2]->active());   // before: joined the new host in its old slot
    CHECK(Error(all[2]) == MP_ERR_KICKED);
    CHECK(!all[1]->slotPresent(2));
    Free(all);
    printf("ban survives takeover: ok\n");
}

// 3. A slot reserved for a player whose link dropped is held for the whole window: in a full room a newcomer is refused ("full for now")
// and the returning player gets its own slot back. Before: freeSlotFor fell back to the reserved slot for the newcomer and the returning
// player was told "room full".
static void TestReservationHeld() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 980);
    MpCore* nw = MakeCore(hub, 5, "Newcomer", 985);
    all.push_back(nw);
    all[0]->uiStart();
    Step(all, 2000);
    const int slotB = all[2]->localSlot();
    hub.setDown(3, true);   // Peach's link is out
    Step(all, 25000);       // (the host removed her at 15 s and keeps her slot for 70 s)
    CHECK(!all[0]->slotPresent(slotB));
    CHECK(ScanJoin(all, nw));
    Step(all, 3000);
    CHECK(!nw->active() && Error(nw) == MP_ERR_ROOM_FULL);   // before: it took Peach's slot
    CHECK(!all[0]->slotPresent(slotB));
    hub.setDown(3, false);
    Step(all, 40000);
    CHECK(all[2]->active() && all[2]->localSlot() == slotB && Error(all[2]) == MP_ERR_NONE);
    CHECK(all[0]->slotPresent(slotB));
    // after the window (70 s from the removal) the slot is free for anybody
    hub.setDown(3, true);
    Step(all, 20000 + 75000);
    CHECK(!all[0]->slotPresent(slotB));
    CHECK(ScanJoin(all, nw));
    Step(all, 3000);
    CHECK(nw->active());
    Free(all);
    printf("reservation held: ok\n");
}

// 4. A forged HELLO1 flood from varied addresses does not starve honest joins (cookies spend no shared budget). Before: the 40/s cookie +
// refusal budget was gone for as long as the flood lasted (honest join: never). Also measures what a cookie costs (the cap is chosen from it).
static double CpuNs() {   // (this process's CPU time: a busy machine does not move it)
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
static int JoinUnderHello1Flood(int perSec, int* joinedAtOut, double* usPerCookie) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all;
    MpCore* c[2];
    for (int i = 0; i < 2; i++) {
        c[i] = MakeCore(hub, (uint32_t)i + 1, i ? "G" : "H", 1080 + (uint32_t)i);
        all.push_back(c[i]);
    }
    MpHostParams hp = HostParams();
    c[0]->uiHost(&hp);
    Step(all, 500);
    RawJoiner j(9, c[0]->debugLocalAddr());
    j.fp = c[0]->fingerprint();
    const uint64_t start = g_now, end = start + 12000;
    int joinedAt = -1;
    uint32_t fake = 100;
    double cookieNs = 0;
    long cookies = 0;
    while (g_now < end) {
        g_now += 5;
        if (perSec > 0)
            for (int k = 0; k < (perSec * 5 + 999) / 1000; k++) {
                j.me.host = fake++;
                j.hello1(hub);
            }
        const double t0 = CpuNs();
        c[0]->pump();
        if (perSec > 0) {
            cookieNs += CpuNs() - t0;
            cookies += (perSec * 5 + 999) / 1000;
        }
        c[1]->pump();
        if (g_now > start + 1500 && !c[1]->active() && ((g_now - start) % 50) == 0) {
            int n = c[1]->uiLocalScan(MP_CONN_LOOPBACK);
            for (int i = 0; i < n; i++) {
                MpLocalRoom r;
                if (c[1]->uiLocalGet(i, &r) && r.compatible && Phase(c[1]) != MP_UI_CONNECTING) {
                    c[1]->uiJoinLocal(MP_CONN_LOOPBACK, i);
                    break;
                }
            }
        }
        if (joinedAt < 0 && c[1]->active()) joinedAt = (int)(g_now - (start + 1500));
    }
    *joinedAtOut = joinedAt;
    *usPerCookie = cookies ? cookieNs / 1000.0 / (double)cookies : 0;
    const bool in = c[1]->active();
    Free(all);
    return in;
}
static void TestHello1FloodFair() {
    for (int rate : {0, 1000, 2000}) {
        int at = -1;
        double us = 0;
        const int in = JoinUnderHello1Flood(rate, &at, &us);
        printf("hello1 flood %d/s: honest joined=%d after %d ms; %.1f us per forged HELLO1 (the host's whole pump)\n", rate, in, at, us);
        CHECK(in && at >= 0 && at <= 2000);
        if (rate) CHECK(us * 10.0 * 1000 < 400000.0);   // (a Vita is slower than this build by a factor of 10 at most: under 40% of a core at the cap of MP_HELLO1_PER_SEC = 1000)
    }
    printf("hello1 flood: ok\n");
}

// 5. Several real addresses, each with a valid cookie, sending wrong-proof HELLO2: the ceiling is fair, an honest newcomer still joins in 2 s.
// Before: four addresses filled the 16/s strangers' ceiling and delayed it 7.5 s.
static int JoinUnderBadProofs(int K, int perSrc, int periodMs, int* joinedAtOut) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all;
    MpCore* c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = MakeCore(hub, (uint32_t)i + 1, i ? "G" : "H", 1180 + (uint32_t)i);
        all.push_back(c[i]);
    }
    MpHostParams hp = HostParams();
    c[0]->uiHost(&hp);
    Step(all, 500);
    CHECK(ScanJoin(all, c[1]));
    Step(all, 1500);
    CHECK(ScanJoin(all, c[2]));
    Step(all, 1500);
    RawJoiner::Hello h;
    h.badProof = true;
    std::vector<RawJoiner*> js;
    std::vector<std::vector<uint8_t>> bad;
    auto refresh = [&]() {
        for (auto* j : js) delete j;
        js.clear();
        bad.clear();
        for (int k = 0; k < K; k++) {
            RawJoiner* j = new RawJoiner(20 + (uint32_t)k, c[0]->debugLocalAddr());
            j->fp = c[0]->fingerprint();
            hub.setTap(RawJoiner::Tap, j);
            j->hello1(hub);
            for (int i = 0; i < 20 && !j->haveCookie; i++) Step(all, 5);
            hub.setTap(0, 0);
            bad.push_back(j->hello2(h, 0x4321 + (uint32_t)k));
            js.push_back(j);
        }
    };
    refresh();
    const uint64_t start = g_now, end = start + 30000;
    uint64_t lastRef = g_now;
    int joinedAt = -1;
    while (g_now < end) {
        g_now += 5;
        for (auto* x : all) x->pump();
        if (g_now - lastRef >= 8000) {
            refresh();
            lastRef = g_now;
        }
        if (K > 0 && (g_now - start) % (uint64_t)periodMs < 5)
            for (int k = 0; k < K; k++)
                for (int m = 0; m < perSrc; m++) hub.inject(js[k]->me, js[k]->host, bad[k].data(), bad[k].size());
        if (g_now > start + 1000 && !c[3]->active() && ((g_now - start) % 50) == 0) {
            int n = c[3]->uiLocalScan(MP_CONN_LOOPBACK);
            for (int i = 0; i < n; i++) {
                MpLocalRoom r;
                if (c[3]->uiLocalGet(i, &r) && r.compatible && Phase(c[3]) != MP_UI_CONNECTING) {
                    c[3]->uiJoinLocal(MP_CONN_LOOPBACK, i);
                    break;
                }
            }
        }
        if (joinedAt < 0 && c[3]->active()) joinedAt = (int)(g_now - (start + 1000));
    }
    *joinedAtOut = joinedAt;
    const bool in = c[3]->active();
    for (auto* j : js) delete j;
    Free(all);
    return in;
}
static void TestProofCeilingFair() {
    for (int K : {4, 8, 12}) {
        int at = -1;
        const int in = JoinUnderBadProofs(K, 4, 1000, &at);
        printf("bad proofs from %d addresses: honest joined=%d after %d ms\n", K, in, at);
        CHECK(in && at >= 0 && at <= 2000);
    }
    printf("proof ceiling fair: ok\n");
}

// 6c. A second takeover soon after the first: the host dies, slot 1 takes over, and slot 1 dies within 3 s. The two guests left converge on one
// host (slot 2), the roster is {2, 3}, and the new channel carries reliable messages both ways.
static void TestDoubleTakeover() {
    for (int gap : {200, 1200, 2800}) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 1300 + (uint32_t)gap);
        all[0]->uiStart();
        Step(all, 1500);
        hub.setDown(1, true);
        std::vector<MpCore*> live = {all[1], all[2], all[3]};
        int waited = 0;
        while (!all[1]->isHost() && waited < 20000) {
            Step(live, 10);
            waited += 10;
        }
        CHECK(all[1]->isHost());
        Step(live, gap);
        hub.setDown(2, true);   // the second host is gone within 3 s of taking over
        std::vector<MpCore*> rest = {all[2], all[3]};
        Step(rest, 40000);
        CHECK(all[2]->active() && all[3]->active());
        CHECK((all[2]->isHost() ? 1 : 0) + (all[3]->isHost() ? 1 : 0) == 1);
        CHECK(all[2]->hostSlot() == all[3]->hostSlot());
        for (MpCore* c : rest) CHECK(!c->slotPresent(0) && !c->slotPresent(1) && c->slotPresent(2) && c->slotPresent(3));
        uint8_t ev[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        MpCore* h = all[2]->isHost() ? all[2] : all[3];
        MpCore* g = h == all[2] ? all[3] : all[2];
        MpMsg m;
        while (g->poll(&m)) {}
        CHECK(h->send(MP_MSG_STAR, ev, 8, true));
        Step(rest, 1000);
        bool got = false;
        while (g->poll(&m)) got |= m.type == MP_MSG_STAR;
        CHECK(got);
        Free(all);
    }
    printf("double takeover: ok\n");
}

// 6d. A takeover during a snapshot transfer: the host dies while it sends the world image to a guest. The survivors are one session, nothing
// hangs (no transfer is left half-open), and the new host's own snapshot reaches the guest whole.
static void TestTakeoverDuringSnapshot() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 1400);
    static uint8_t img[9000], img2[7000], got[MP_SNAPSHOT_MAX];
    for (int i = 0; i < 9000; i++) img[i] = (uint8_t)(i * 131 + 7);
    for (int i = 0; i < 7000; i++) img2[i] = (uint8_t)(i * 29 + 3);
    MpLoopImpairment imp = {0.25f, 0.03f, 40, 150, 0};   // (slow: the transfer takes seconds)
    hub.setImpairment(imp, 91);
    all[0]->snapshotOffer(img, sizeof img);
    Step(all, 600);
    CHECK(!all[2]->snapshotReady());   // half done: the guest has not got it whole
    hub.setDown(1, true);   // the host dies with the transfer half done
    std::vector<MpCore*> rest = {all[1], all[2]};
    Step(rest, 30000);
    CHECK(all[1]->active() && all[2]->active());
    CHECK(all[1]->isHost() && !all[2]->isHost() && all[2]->hostSlot() == all[1]->localSlot());
    const uint32_t n = all[2]->snapshotReady();   // (whole or nothing: never a part)
    CHECK(n == 0 || n == sizeof img);
    if (n) {
        CHECK(all[2]->snapshotGet(got, sizeof got) == sizeof img && !memcmp(got, img, sizeof img));
        all[2]->snapshotConsumed();
    }
    all[1]->snapshotOffer(img2, sizeof img2);
    for (int t = 0; t < 30000 && all[2]->snapshotReady() != sizeof img2; t += 10) Step(rest, 10);
    CHECK(all[2]->snapshotReady() == sizeof img2);
    CHECK(all[2]->snapshotGet(got, sizeof got) == sizeof img2 && !memcmp(got, img2, sizeof img2));
    Free(all);
    printf("takeover during snapshot: ok\n");
}
