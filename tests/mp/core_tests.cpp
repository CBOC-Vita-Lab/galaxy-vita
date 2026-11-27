// Galaxy 3.0 multiplayer: session-layer host tests (virtual time, loopback network with impairment). ASan + UBSan.
// Build/run: tests/mp/build.sh
#include "core_test_util.h"
#include "raw_joiner.h"
#include <strings.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <map>
#include <tuple>

// ---------------------------------------------------------------------------------------------------------------------------
// 1. MpReliable alone: 20% loss + 10% duplicates + reordering (200 ms jitter); every message exactly once, in order.
namespace rel_test {
struct Wire {
    uint64_t due;
    int dir;   // 0: a->b data, 1: b->a ack
    uint16_t seq, ack, bits, len;
    uint8_t type;
    std::vector<uint8_t> data;
};
std::vector<Wire> g_wire;
uint32_t g_rng = 99;
float U() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (float)(g_rng >> 8) / 16777216.0f;
}
void Put(Wire w) {
    if (U() < 0.20f) return;
    const int copies = U() < 0.10f ? 2 : 1;
    for (int c = 0; c < copies; c++) {
        Wire x = w;
        x.due = g_now + 20 + (uint64_t)(U() * 200.0f);
        g_wire.push_back(x);
    }
}
MpReliable* g_a;
MpReliable* g_b;
std::vector<uint32_t> g_got;
bool Emit(void*, const MpRelMsg& m) {
    Wire w;
    w.dir = 0; w.seq = m.seq; w.type = m.type; w.len = m.len; w.ack = 0; w.bits = 0;
    w.data.assign(m.data, m.data + m.len);
    Put(w);
    return true;
}
void Deliver(void*, const MpRelMsg& m) {
    uint32_t v = 0;
    if (m.len >= 4) memcpy(&v, m.data, 4);
    // the payload is the counter repeated: check the whole message
    for (uint16_t i = 4; i + 4 <= m.len; i += 4) {
        uint32_t x;
        memcpy(&x, m.data + i, 4);
        if (x != v) { CHECK(!"payload corrupted"); break; }
    }
    g_got.push_back(v);
}

void Run() {
    g_a = new MpReliable();
    g_b = new MpReliable();
    g_a->reset(65000);   // wraps through 0
    g_b->reset(1);
    g_b->startRecv(65000);
    const uint32_t total = 6000;
    uint32_t next = 0;
    uint8_t buf[MP_MAX_PAYLOAD];
    uint64_t lastAck = 0;
    for (int iter = 0; iter < 400000 && g_got.size() < total; iter++) {
        g_now += 2;
        // the sender pushes as fast as its backlog allows; sizes 4..1024
        while (next < total) {
            const uint16_t len = (uint16_t)(4 + 4 * ((next * 37) % 256));
            for (uint16_t i = 0; i + 4 <= len; i += 4) memcpy(buf + i, &next, 4);
            if (!g_a->push(0x34, 1, buf, len)) break;
            next++;
        }
        g_a->emitDue(g_now, 120, 32, Emit, 0);
        // deliver due wire items
        for (size_t i = 0; i < g_wire.size();) {
            if (g_wire[i].due > g_now) { i++; continue; }
            Wire w = g_wire[i];
            g_wire.erase(g_wire.begin() + (long)i);
            if (w.dir == 0) {
                MpRelMsg m;
                m.seq = w.seq; m.type = w.type; m.from = 1; m.len = w.len; m.data = w.data.data();
                g_b->receive(m, Deliver, 0);
            } else {
                g_a->onAck(w.ack, w.bits, g_now);
            }
        }
        if (g_b->ackDirty || g_now - lastAck > 50) {
            Wire w;
            w.dir = 1; w.seq = 0; w.type = 0; w.len = 0;
            g_b->ackFields(&w.ack, &w.bits);
            g_b->ackDirty = false;
            lastAck = g_now;
            Put(w);
        }
    }
    CHECK(g_got.size() == total);
    bool order = true;
    for (uint32_t i = 0; i < g_got.size(); i++)
        if (g_got[i] != i) { order = false; break; }
    CHECK(order);
    printf("reliable channel: %zu/%u delivered in order, %u resends\n", g_got.size(), total, (unsigned)g_a->resends());
    delete g_a;
    delete g_b;
    g_wire.clear();
}
}  // namespace rel_test

// ---------------------------------------------------------------------------------------------------------------------------
// Event bookkeeping for end-to-end tests: every console sends reliable STAR events {counter u32}; every console must get the
// events of every other console exactly once and in order (relayed by the host).
struct Book {
    std::map<std::pair<MpCore*, int>, std::vector<uint32_t>> got;   // (receiver, origin slot) -> counters
    std::map<MpCore*, uint32_t> sent;
    std::map<MpCore*, int> slotOf;
};

static void SendStar(Book& b, MpCore* c) {
    uint32_t v = b.sent[c];
    uint8_t p[8];
    memcpy(p, &v, 4);
    p[4] = 0xA5; p[5] = 0x5A; p[6] = 0; p[7] = 0;
    if (c->send(MP_MSG_STAR, p, 8, true)) b.sent[c] = v + 1;
}

static int Drain(Book& b, MpCore* c) {
    MpMsg m;
    int n = 0;
    while (c->poll(&m)) {
        n++;
        if (m.type != MP_MSG_STAR) continue;
        CHECK(m.len == 8);
        uint32_t v;
        memcpy(&v, m.data, 4);
        b.got[{c, m.fromSlot}].push_back(v);
    }
    return n;
}

// exactly once and in order (from the first one seen: a late joiner starts in the middle)
static bool InOrder(const std::vector<uint32_t>& v) {
    for (size_t i = 1; i < v.size(); i++)
        if (v[i] != v[i - 1] + 1) return false;
    return true;
}

static std::vector<MpCore*> StartSession(MpLoopHub& hub, int n, uint32_t seed, MpMode mode = MP_MODE_TOGETHER) {
    std::vector<MpCore*> all;
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    for (int i = 0; i < n; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], seed + (uint32_t)i * 7919u));
    MpHostParams hp = HostParams(mode);
    CHECK(all[0]->uiHost(&hp));
    CHECK(WaitPhase(all, all[0], MP_UI_LOBBY, 2000));
    for (int i = 1; i < n; i++) {
        CHECK(ScanJoin(all, all[i]));
        CHECK(WaitPhase(all, all[i], MP_UI_LOBBY, 8000));
    }
    return all;
}

static void Free(std::vector<MpCore*>& all) {
    for (MpCore*& c : all) { delete c; c = 0; }
}

// corefix: restart as the same console (the persistent key is what mp.cfg keeps)
static void SameConsole(MpCore* dst, MpCore* src) {
    uint8_t k[16];
    src->debugConsoleKey(k);
    dst->setConsoleKey(k);
}

// 2. join, slots, game messages both ways, leave
static void TestBasic() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 11);
    MpCore* h = all[0];
    MpCore* g = all[1];
    CHECK(h->active() && g->active());
    CHECK(h->isHost() && !g->isHost());
    CHECK(h->localSlot() == 0 && g->localSlot() == 1);
    CHECK(h->slotPresent(1) && g->slotPresent(0));
    char name[17];
    g->slotName(0, name);
    CHECK(!strcmp(name, "Player"));
    CHECK(h->hostSlot() == 0 && g->hostSlot() == 0 && g->hostEpoch() == 0);
    CHECK(h->saveAutoconfirm() && !h->saveSuppressed());
    CHECK(g->saveSuppressed() && !g->saveAutoconfirm());
    CHECK(h->uiStart());
    Step(all, 300);
    CHECK(Phase(g) == MP_UI_PLAYING);
    Book b;
    for (int i = 0; i < 50; i++) { SendStar(b, h); SendStar(b, g); Step(all, 20); }
    Step(all, 1000);
    Drain(b, h);
    Drain(b, g);
    { auto& v = b.got[std::make_pair(g, 0)]; CHECK(v.size() == 50 && InOrder(v)); }
    { auto& v = b.got[std::make_pair(h, 1)]; CHECK(v.size() == 50 && InOrder(v)); }
    // pose (unreliable)
    MpPose pose;
    memset(&pose, 0, sizeof pose);
    pose.pos[0] = 12.5f;
    uint8_t pb[64];
    MpWriter w(pb, sizeof pb);
    mp_write_pose(w, pose);
    CHECK(g->send(MP_MSG_STATE, pb, (uint16_t)w.n, false));
    Step(all, 100);
    MpMsg m;
    bool gotPose = false;
    while (h->poll(&m))
        if (m.type == MP_MSG_STATE && m.fromSlot == 1) gotPose = true;
    CHECK(gotPose);
    // a guest may not send host-only types
    uint8_t where[8] = {0};
    CHECK(!g->send(MP_MSG_WHERE, where, 8, true));
    CHECK(h->send(MP_MSG_WHERE, where, 8, true));
    // too long / unknown types are refused
    uint8_t big[100] = {0};
    CHECK(!g->send(MP_MSG_LOOK, big, 100, true));
    CHECK(!g->send(0x5E, big, 4, true));
    g->uiLeave();
    CHECK(!g->active());
    Step(all, 1000);
    CHECK(Phase(g) == MP_UI_IDLE && Error(g) == MP_ERR_NONE);
    CHECK(!h->slotPresent(1) && h->active());
    CHECK(LogCount("slot 1 left reason=leave") == 1);
    h->uiLeave();
    Step(all, 100);
    CHECK(Phase(h) == MP_UI_IDLE);
    Free(all);
    printf("basic: ok\n");
}

// merge fix 5: the app is closed by the system (goodbye(), not shutdown()): the others see the console leave within the next pump, with a
// BYE, instead of timing it out; the call itself is bounded and does not tear anything down.
static void TestGoodbyeOnQuit() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 12);
    MpCore* h = all[0];
    CHECK(h->uiStart());
    Step(all, 300);
    CHECK(h->slotPresent(1) && h->slotPresent(2));
    // a guest quits: the host (and the other guest) drop its slot well inside 1 s (the timeout is 15 s), reason = leave
    CHECK(all[1]->goodbye(300));
    hub.setDown(2, true);   // the console is gone for good (endpoint 2 = slot 1)
    Step(all, 400);
    CHECK(!h->slotPresent(1) && h->slotPresent(2) && h->active());
    CHECK(LogCount("slot 1 left reason=leave") >= 1);
    // the host quits: the remaining guest learns it at once as well (host-leaving BYE), before any 15 s timeout
    CHECK(h->goodbye(300));
    hub.setDown(1, true);
    Step(all, 400);
    CHECK(LogCount("slot 0 left") >= 1 || !all[2]->slotPresent(0));
    Free(all);
    printf("goodbye on quit: ok\n");
}

// 3. end-to-end reliability: 4 consoles, 20% loss + dup + 200 ms jitter on every datagram; all events exactly once, in order
// (coordinator, from the practice-partner agent's find) A pump takes its time once, then its connector and handlers stamp things
// with later clock reads. Elapsed times were unsigned "t - stamp": with a stamp later than t they wrapped, so a join could end at
// once with "no answer from the host", and a just-accepted guest could be dropped as lost in the same pump.
static void TestClockReadsWithinAPump() {
    g_creep = 2;
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        const size_t log0 = g_log.size();
        std::vector<MpCore*> all = StartSession(hub, 3, 77);
        CHECK(all[0]->active() && all[1]->active() && all[2]->active());
        CHECK(all[0]->uiStart());
        Step(all, 3000);
        CHECK(Phase(all[1]) == MP_UI_PLAYING && Phase(all[2]) == MP_UI_PLAYING);
        CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2));
        int bad = 0;
        for (size_t i = log0; i < g_log.size(); i++)
            if (g_log[i].find("no answer from the host") != std::string::npos || g_log[i].find("lost") != std::string::npos ||
                g_log[i].find("bring-up took too long") != std::string::npos || g_log[i].find("claim") != std::string::npos)
                bad++;
        CHECK(bad == 0);
        for (MpCore* c : all) delete c;
    }
    g_creep = 0;
}

static void TestReliableE2E() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 21);
    all[0]->uiStart();
    Step(all, 500);
    MpLoopImpairment imp = {0.20f, 0.10f, 10, 200, 0};
    hub.setImpairment(imp, 1234);
    Book b;
    for (int turn = 0; turn < 3000; turn++) {
        for (MpCore* c : all)
            if ((turn + (int)(intptr_t)c) % 3 == 0) SendStar(b, c);
        Step(all, 16, 8);
        for (MpCore* c : all) Drain(b, c);
    }
    MpLoopImpairment none = {0, 0, 0, 0, 0};
    hub.setImpairment(none, 1);
    Step(all, 8000);
    for (MpCore* c : all) Drain(b, c);
    int ok = 0, total = 0;
    for (MpCore* r : all) {
        for (MpCore* s : all) {
            if (r == s) continue;
            const std::vector<uint32_t>& v = b.got[{r, s->localSlot()}];
            total++;
            if (v.size() == b.sent[s] && InOrder(v) && (v.empty() || v[0] == 0)) ok++;
            else fprintf(stderr, "  slot %d from %d: got %zu of %u inorder=%d\n", r->localSlot(), s->localSlot(), v.size(), b.sent[s], InOrder(v));
        }
        CHECK(r->active());
    }
    CHECK(ok == total);
    printf("reliable e2e (20%% loss, dup, 200 ms jitter): %d/%d streams exact, %u events each\n", ok, total, b.sent[all[1]]);
    Free(all);
}

// 4. failure paths always end in IDLE, and a new host / join works afterwards
static void TestFailurePaths() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all;
    MpCore* a = MakeCore(hub, 1, "Alpha", 31);
    MpCore* b = MakeCore(hub, 2, "Beta", 32);
    all.push_back(a);
    all.push_back(b);
    // (a) the host vanishes while the guest connects: CONNECTING times out
    MpHostParams hp = HostParams();
    CHECK(a->uiHost(&hp));
    Step(all, 100);
    int n = 0;
    for (int t = 0; t < 2000 && n == 0; t += 50) { n = b->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 1);
    hub.setDown(1, true);
    CHECK(b->uiJoinLocal(MP_CONN_LOOPBACK, 0));
    Step(all, MP_CONNECT_TIMEOUT_MS + 500);
    CHECK(Phase(b) == MP_UI_IDLE && Error(b) == MP_ERR_TIMEOUT && !b->active());
    hub.setDown(1, false);
    a->uiLeave();
    Step(all, 100);
    // (b) different version: REJECT(VERSION) -> DIFFERENT_VERSION, IDLE
    CHECK(a->uiHost(&hp));
    Step(all, 600);
    n = 0;
    for (int t = 0; t < 2000 && n == 0; t += 50) { n = b->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 1);
    b->setDataHash(0xBADBAD);   // (after the scan, so the HELLO itself carries the other fingerprint)
    CHECK(b->uiJoinLocal(MP_CONN_LOOPBACK, 0));
    Step(all, 1000);
    CHECK(Phase(b) == MP_UI_IDLE && Error(b) == MP_ERR_DIFFERENT_VERSION);
    CHECK(LogCount("hello rejected reason=version") >= 1);
    // the scan now marks the room incompatible, and joining it is refused up front
    n = 0;
    for (int t = 0; t < 2000 && n == 0; t += 50) { n = b->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    MpLocalRoom room;
    CHECK(b->uiLocalGet(0, &room) && !room.compatible);
    CHECK(!b->uiJoinLocal(MP_CONN_LOOPBACK, 0) && Error(b) == MP_ERR_DIFFERENT_VERSION);
    b->setDataHash(0);
    b->uiLeave();
    // (c) after all that, b can still join...
    CHECK(ScanJoin(all, b));
    CHECK(WaitPhase(all, b, MP_UI_LOBBY, 3000));
    // (d) the host goes silent (2 players: nobody to migrate to). corefix D1: a console alone after silence suspects its own link
    // first: RECONNECTING for MP_LINKLOST_RECONNECT_MS (the host could be back), then IDLE with the honest reason "connection lost"
    // (NO_WIFI). (Before: HOST_LEFT, IDLE after 5 s, whether it was the host or the guest's own link that had gone.)
    hub.setDown(1, true);
    Step(all, MP_HOST_LOST_MS + 1000);
    CHECK(Phase(b) == MP_UI_RECONNECTING && !b->active());
    Step(all, MP_LINKLOST_RECONNECT_MS + 4000);
    CHECK(Phase(b) == MP_UI_IDLE && Error(b) == MP_ERR_NO_WIFI && !b->active());
    hub.setDown(1, false);
    a->uiLeave();
    Step(all, 100);
    // (e) ...and host a new session that a joins
    CHECK(b->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, a));
    CHECK(WaitPhase(all, a, MP_UI_LOBBY, 3000));
    CHECK(a->localSlot() == 1 && b->isHost());
    // (f) kicked: KICKED, IDLE; a rejoin is refused
    b->uiKick(1);
    Step(all, 1000);
    CHECK(Phase(a) == MP_UI_IDLE && Error(a) == MP_ERR_KICKED);
    CHECK(ScanJoin(all, a));
    Step(all, 1000);
    CHECK(Phase(a) == MP_UI_IDLE && Error(a) == MP_ERR_KICKED);
    // (g) leave in every phase ends in IDLE
    a->uiLeave();
    CHECK(Phase(a) == MP_UI_IDLE && Error(a) == MP_ERR_NONE);
    b->uiLeave();
    Step(all, 1000);
    CHECK(Phase(b) == MP_UI_IDLE);
    CHECK(b->uiHost(&hp));
    b->uiLeave();   // while creating
    CHECK(Phase(b) == MP_UI_IDLE);
    Free(all);
    printf("failure paths: ok\n");
}


static void WritePacket(std::vector<uint8_t>& out, uint8_t type, uint8_t from, uint8_t flags, uint16_t seq, const void* p, uint16_t len) {
    out.assign(12 + len, 0);
    out[0] = 'G'; out[1] = 'M'; out[2] = MP_PROTO_VERSION; out[3] = type; out[4] = from; out[5] = flags;
    out[6] = (uint8_t)seq; out[7] = (uint8_t)(seq >> 8);
    if (len) memcpy(out.data() + 12, p, len);
}

// core2: a datagram from a member, sealed with its pair key (as the member itself would send it)
static void InjectAs(MpLoopHub& hub, MpCore* from, MpCore* to, const void* pkt, size_t len) {
    uint8_t out[MP_DATAGRAM_MAX + 64];
    const size_t n = from->debugSealAs(to->localSlot(), pkt, len, out, sizeof out);
    CHECK(n > 0);
    if (n) hub.inject(from->debugLocalAddr(), to->debugLocalAddr(), out, n);
}

// 5. per-peer rate cap: a guest flooding the host is cut off and kicked
// scenarios-final: SMG_MP_TEST_RECONNECT_MS (desktop-only hook): the pure parse of the shortened silent-link window
extern uint32_t mp_linklost_window_from(const char* e);
static void TestLinkLostWindowOverride() {
    CHECK(mp_linklost_window_from(nullptr) == MP_LINKLOST_RECONNECT_MS);
    CHECK(mp_linklost_window_from("") == MP_LINKLOST_RECONNECT_MS);
    CHECK(mp_linklost_window_from("6000") == 6000);
    CHECK(mp_linklost_window_from("999") == MP_LINKLOST_RECONNECT_MS);      // below 1 s: ignored
    CHECK(mp_linklost_window_from("75001") == MP_LINKLOST_RECONNECT_MS);    // above the real window: ignored
    CHECK(mp_linklost_window_from("junk") == MP_LINKLOST_RECONNECT_MS);
    CHECK(mp_linklost_window_from("30000") != MP_RECONNECT_MS);             // never the Wi-Fi-gone window (the code tells them apart by value)
}

static void TestRateCap() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 41);
    const MpAddr gaddr = all[1]->debugLocalAddr(); (void)gaddr;
    const MpAddr haddr = all[0]->debugLocalAddr(); (void)haddr;
    uint8_t pose[64];
    memset(pose, 0, sizeof pose);
    std::vector<uint8_t> pkt;
    uint16_t seq = 1000;
    for (int ms = 0; ms < 3000 && all[0]->slotPresent(1); ms += 10) {
        for (int k = 0; k < 6; k++) {   // 600 messages per second
            WritePacket(pkt, MP_MSG_STATE, 1, 0, seq++, pose, 51);
            InjectAs(hub, all[1], all[0], pkt.data(), pkt.size());
        }
        Step(all, 10);
    }
    CHECK(!all[0]->slotPresent(1));
    CHECK(LogCount("kicked slot=1 reason=bad-messages") == 1);
    Step(all, 2000);
    CHECK(Phase(all[1]) == MP_UI_IDLE && Error(all[1]) == MP_ERR_KICKED);
    CHECK(all[2]->active() && all[2]->slotPresent(2) && !all[2]->slotPresent(1));   // the others are fine
    Free(all);
    printf("rate cap: ok\n");
}

// 5b. a host that stalled (a level load, a debugger) drains a backlog; that is not a flood
static void TestStallBacklog() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 47);
    Step(all, 500);
    const MpAddr gaddr = all[1]->debugLocalAddr(); (void)gaddr;
    const MpAddr haddr = all[0]->debugLocalAddr(); (void)haddr;
    uint8_t pose[64];
    memset(pose, 0, sizeof pose);
    std::vector<uint8_t> pkt;
    uint16_t seq = 2000;
    std::vector<MpCore*> guests(all.begin() + 1, all.end());
    Step(guests, 2000);   // the host does not run
    for (int k = 0; k < 400; k++) {
        WritePacket(pkt, MP_MSG_STATE, 1, 0, seq++, pose, 51);
        InjectAs(hub, all[1], all[0], pkt.data(), pkt.size());
    }
    const int kicksBefore = LogCount("kicked slot=1 reason=bad-messages");
    Step(all, 3000);
    CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2));
    CHECK(LogCount("kicked slot=1 reason=bad-messages") == kicksBefore);
    Free(all);
    printf("stall backlog: ok\n");
}

// 6. snapshot transfer under 20% loss (and the guest's copy matches)
static void TestSnapshot() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 51);
    static uint8_t img[8000], got[MP_SNAPSHOT_MAX];
    for (int i = 0; i < 8000; i++) img[i] = (uint8_t)(i * 131 + 7);
    MpLoopImpairment imp = {0.20f, 0.05f, 10, 100, 0};
    hub.setImpairment(imp, 77);
    all[0]->snapshotOffer(img, sizeof img);
    MpUiStatus st;
    bool sawProgress = false;
    for (int t = 0; t < 20000 && !all[1]->snapshotReady(); t += 10) {
        all[1]->uiStatus(&st);
        if (st.progress > 0 && st.progress < 100) sawProgress = true;
        Step(all, 10);
    }
    CHECK(all[1]->snapshotReady() == sizeof img);
    CHECK(all[1]->snapshotGet(got, sizeof got) == sizeof img && !memcmp(got, img, sizeof img));
    all[1]->snapshotConsumed();
    CHECK(all[1]->snapshotReady() == 0);
    Step(all, 2000);
    CHECK(LogCount("snapshot delivered slot=1") == 1);
    (void)sawProgress;
    // a second offer (after a star) replaces it
    img[5] ^= 0xFF;
    all[0]->snapshotOffer(img, 4000);
    for (int t = 0; t < 20000 && !all[1]->snapshotReady(); t += 10) Step(all, 10);
    CHECK(all[1]->snapshotReady() == 4000);
    CHECK(all[1]->snapshotGet(got, sizeof got) == 4000 && !memcmp(got, img, 4000));
    Free(all);
    printf("snapshot under loss: ok\n");
}

// 7. receive queue policy: unreliable overflow drops the oldest; reliable messages wait (back-pressure, core2)
static void TestQueueOverflow() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 61);
    uint8_t pose[64] = {0};
    for (int i = 0; i < 400; i++) {
        pose[0] = (uint8_t)i;
        CHECK(all[0]->send(MP_MSG_STATE, pose, 51, false));
        if (i % 20 == 0) Step(all, 5);
    }
    Step(all, 200);
    MpMsg m;
    int n = 0;
    uint8_t last = 0;
    while (all[1]->poll(&m))
        if (m.type == MP_MSG_STATE) { n++; last = m.data[0]; }
    CHECK(n == MP_QUEUE_SIZE);
    CHECK(last == (uint8_t)399);   // the newest survived
    CHECK(all[1]->active());
    uint8_t ev[8] = {0};
    for (int i = 0; i < 300; i++) {
        all[0]->send(MP_MSG_STAR, ev, 8, true);
        if (i % 20 == 0) Step(all, 5);
    }
    Step(all, 1000);
    // core2: back-pressure: what does not fit waits in the sender's backlog (not acknowledged); once the game takes its messages every
    // one of the 300 arrives, in order, nobody leaves
    CHECK(all[1]->active() && all[0]->slotPresent(1));
    int stars = 0;
    for (int t = 0; t < 20000 && stars < 300; t += 20) {
        Step(all, 20);
        while (all[1]->poll(&m)) if (m.type == MP_MSG_STAR) stars++;
    }
    CHECK(stars == 300 && all[1]->active() && LogCount("receive queue overflow") == 0);
    Free(all);
    printf("queue overflow: ok\n");
}

// starfix round 3: a console that loads for 40 s reads nothing (no poll). Another console keeps sending reliable game messages at `rate` per second.
// loader 0 = the host loads, guest 1 sends; loader 1 = guest 1 loads, guest 2 sends (the host relays: origin = guest 2). Returns whether the loader is still in
// the session afterwards; counts what was queued while it did not read, and checks that every message arrives once it reads again.
static void StepDrain(std::vector<MpCore*>& all, int ms, int loader) {   // the consoles that do not load read their messages every 5 ms
    MpMsg d;
    for (int t = 0; t < ms; t += 5) {
        Step(all, 5, 5);
        for (int i = 0; i < (int)all.size(); i++) if (i != loader) while (all[i]->poll(&d)) {}
    }
}
static bool LongLoadRun(int loader, int rate, int loadMs, int* queued, int* arrived, int* sent, int payload = 8, uint8_t type = MP_MSG_KILL, int senderOverride = -1, int cutAtMs = -1, int cutLenMs = 0) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 311 + (uint32_t)rate);
    const int sender = senderOverride >= 0 ? senderOverride : (loader == 0 ? 1 : 2);
    uint8_t ev[160] = {0};
    int n = 0;
    MpMsg m;
    for (int t = 0; t < loadMs; t += 1000) {
        if (cutAtMs >= 0 && t == cutAtMs) hub.setDown((uint32_t)(loader + 1), true);   // the loader's link is cut (a Wi-Fi dropout during the load)
        if (cutAtMs >= 0 && t == cutAtMs + cutLenMs) hub.setDown((uint32_t)(loader + 1), false);
        for (int i = 0; i < rate; i++) { ev[0] = (uint8_t)n; if (all[sender]->send(type, ev, (uint16_t)payload, true)) n++; StepDrain(all, 1000 / rate, loader); }
    }
    bool stillIn = all[loader]->active() && all[0]->slotPresent(1) && all[0]->slotPresent(2);
    int got = 0;
    int tDone = 0;
    for (int t = 0; t < 120000 && got < n; t += 20) {
        StepDrain(all, 20, loader);
        while (all[loader]->poll(&m)) if (m.type == type) got++;
        tDone = t;
    }
    *queued = tDone;   // ms after the load until the last message arrived
    *arrived = got;
    *sent = n;
    stillIn = stillIn && all[loader]->active() && all[0]->slotPresent(1) && all[0]->slotPresent(2);
    Free(all);
    return stillIn;
}
static void TestLongLoad() {
    // a 40 s load (a Vita's worst): nobody is removed and every message arrives once the console reads again, whichever console loads and however fast the
    // other one sends (2/s is the host's 1 s world check; a star-bit storm reaches 40/s for seconds). Before MP_ESTAB_NOACK_MS (15 s) the loading GUEST was removed
    // by the host as soon as 160 messages of one relayed origin filled its queue and its gate stopped acknowledging (8/s from the other guest: 320 sent, 33 arrived).
    for (int loader = 0; loader < 2; loader++)
        for (int rate : {2, 4, 8, 20}) {
            g_log.clear();
            int q = 0, got = 0, sent = 0;
            const bool in = LongLoadRun(loader, rate, 40000, &q, &got, &sent);
            CHECK(in && got == sent && LogCount("receive queue") == 0 && LogCount("no acknowledgement") == 0);
            printf("long load 40 s: loader=%d rate=%d/s sent=%d arrived=%d stayed=%d\n", loader, rate, sent, got, (int)in);
        }
    // 138-byte payloads (a host OBJMAP chunk; the largest real message is the 568-byte SWITCHMAP; both use the reliable layer's 16 big hold slots and are sent in bursts on a join or
    // repair, never as a stream). 2/s for 40 s (11 KB) must pass. 8/s for 40 s is 44 KB: it hits the sender's 32 KiB backlog limit first (mp_core.cpp "send backlog full"):
    // the console is then removed, a bound that no real stream reaches.
    {
        g_log.clear();
        int q = 0, got = 0, sent = 0;
        const bool in = LongLoadRun(1, 2, 40000, &q, &got, &sent, 138, MP_MSG_OBJMAP, 0);
        CHECK(in && sent == 80 && got == sent && LogCount("backlog full") == 0 && LogCount("no acknowledgement") == 0);
        printf("long load 40 s, 138-byte OBJMAP from the host: loader=1 rate=2/s sent=%d arrived=%d stayed=%d\n", sent, got, (int)in);
        g_log.clear();
        got = sent = 0;
        LongLoadRun(1, 8, 40000, &q, &got, &sent, 138, MP_MSG_OBJMAP, 0);
        CHECK(LogCount("backlog full") >= 1 && got < sent);
        printf("long load 40 s, 138-byte OBJMAP from the host: loader=1 rate=8/s sent=%d arrived=%d (32 KiB send backlog bound hit first, as expected)\n", sent, got);
    }
    // the realistic big burst: two full states (2 x 568-byte SWITCHMAP + 18 x 138-byte OBJMAP chunks) sent at once into a console that then loads for 40 s: all arrive after the
    // load (the 16 big hold slots and the core's 16 big queue blocks give back-pressure, not loss) and nobody is removed
    {
        g_log.clear();
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 377);
        static uint8_t big[568];
        int sentMap = 0, sentObj = 0, gotMap = 0, gotObj = 0;
        for (int i = 0; i < 20; i++) {
            const bool map = i == 0 || i == 10;
            big[0] = (uint8_t)i;
            if (all[0]->send(map ? MP_MSG_SWITCHMAP : MP_MSG_OBJMAP, big, map ? 568 : 138, true)) (map ? sentMap : sentObj)++;
        }
        StepDrain(all, 40000, 1);
        MpMsg m;
        for (int t = 0; t < 30000 && gotMap + gotObj < sentMap + sentObj; t += 20) {
            StepDrain(all, 20, 1);
            while (all[1]->poll(&m)) { if (m.type == MP_MSG_SWITCHMAP) gotMap++; else if (m.type == MP_MSG_OBJMAP) gotObj++; }
        }
        CHECK(sentMap == 2 && sentObj == 18 && gotMap == 2 && gotObj == 18 && all[1]->active() && all[0]->slotPresent(1) && LogCount("backlog full") == 0 && LogCount("no acknowledgement") == 0);
        printf("long load 40 s, burst of 2 SWITCHMAP + 18 OBJMAP chunks: sent=%d+%d arrived=%d+%d stayed=%d\n", sentMap, sentObj, gotMap, gotObj, (int)all[1]->active());
        Free(all);
    }
    // a 3 s link cut at second 25 of a 40 s load (the loader is already past 15 s without acks and "busy"): it must stay (the 60 s bound is measured at its LAST packet, so a
    // quiet spell falls to the 15 s silence rule, which 3 s does not reach; a cut of 5 s or more makes the guest itself declare the host lost: MP_HOST_LOST_MS, not this rule)
    {
        g_log.clear();
        int q = 0, got = 0, sent = 0;
        const bool in = LongLoadRun(1, 20, 40000, &q, &got, &sent, 8, MP_MSG_KILL, -1, 25000, 3000);
        CHECK(in && got == sent && LogCount("no acknowledgement") == 0 && LogCount("left reason") == 0);
        printf("long load 40 s, loader's link cut 3 s at second 25: sent=%d arrived=%d stayed=%d\n", sent, got, (int)in);
    }
    // the hard bound stays: a guest whose game reads nothing for far longer than any load (here 90 s at 20/s, queue full from second 8) is still removed
    {
        g_log.clear();
        int q = 0, got = 0, sent = 0;
        const bool in = LongLoadRun(1, 20, 90000, &q, &got, &sent);
        CHECK(!in || got < sent);   // (removed, then rejoined: what was sent meanwhile is gone)
        printf("long load 90 s: loader=1 rate=20/s sent=%d arrived=%d stayed=%d (a bound, not a load)\n", sent, got, (int)in);
    }
}

// When is a peer that stops acknowledging removed? (a) busy (its gate refuses, MP_HDR_BUSY on every packet): about MP_ESTAB_NOACK_MS after its last ack progress;
// (b) not busy (it sends, but nothing of ours reaches it, so it acknowledges nothing and says nothing of a load): about MP_PEER_LOST_MS.
static bool g_dropToLoaderOn = false;
static bool DropToLoaderFilter(void*, const MpAddr&, const MpAddr& to, const void*, size_t) { return !(g_dropToLoaderOn && to.host == 2); }
static void TestNoAckRemovalTimes() {
    {   // (a) permanent BUSY: the loader never reads, guest 2 sends 20/s through the host
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 391);
        uint8_t ev[8] = {0};
        const uint64_t t0 = g_now;
        int64_t goneAt = -1;
        MpMsg d;
        for (int t = 0; t < 100000 && goneAt < 0; t += 50) {
            for (int i = 0; i < 1; i++) all[2]->send(MP_MSG_KILL, ev, 8, true);
            Step(all, 50);
            for (int i = 0; i < (int)all.size(); i++) if (i != 1) while (all[i]->poll(&d)) {}
            if (!all[0]->slotPresent(1)) goneAt = (int64_t)(g_now - t0);
        }
        CHECK(goneAt >= 50000 && goneAt <= 90000);
        printf("no-ack removal, loader busy for good: removed after %lld ms (expect ~60-75 s)\n", (long long)goneAt);
        Free(all);
    }
    {   // (b) not busy: the host's packets to guest 1 are dropped, guest 1 keeps sending
        g_log.clear();
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 392);
        uint8_t ev[8] = {0};
        hub.setFilter(DropToLoaderFilter, 0);
        g_dropToLoaderOn = true;
        all[0]->send(MP_MSG_KILL, ev, 8, true);
        const uint64_t t0 = g_now;
        int64_t goneAt = -1;
        for (int t = 0; t < 40000 && goneAt < 0; t += 50) {
            Step(all, 50);
            MpMsg d;
            for (int i = 0; i < (int)all.size(); i++) while (all[i]->poll(&d)) {}
            if (!all[0]->slotPresent(1)) goneAt = (int64_t)(g_now - t0);
        }
        g_dropToLoaderOn = false;
        hub.setFilter(0, 0);
        CHECK(goneAt >= 10000 && goneAt <= 22000);
        printf("no-ack removal, peer not busy: removed after %lld ms (expect ~15 s)\n", (long long)goneAt);
        Free(all);
    }
}

// 8. rejoin: suspend + resume takes the same slot back; a console that vanished is replaced by its new session (same name)
static void TestRejoin() {
    g_log.clear();   // (counts below are this test's)
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 71);
    all[0]->uiStart();
    Step(all, 300);
    all[1]->suspend();
    CHECK(Phase(all[1]) == MP_UI_IDLE && !all[1]->active());
    Step(all, 500);
    CHECK(!all[0]->slotPresent(1));
    // a newcomer does not take the reserved slot
    MpCore* d = MakeCore(hub, 4, "Daisy", 74);
    all.push_back(d);
    CHECK(ScanJoin(all, d));
    CHECK(WaitPhase(all, d, MP_UI_PLAYING, 3000));
    CHECK(d->localSlot() == 3);
    d->uiLeave();
    Step(all, 1000);
    CHECK(!all[1]->debugHeld());   // MP3 lodgate: suspended (asleep): nothing is held
    CHECK(all[1]->resume());
    CHECK(all[1]->debugHeld() && !all[1]->active());   // the manual rejoin (no m_reconnect) is in flight: held (m_rejoining)
    for (int i = 0; i < 400 && Phase(all[1]) != MP_UI_PLAYING; i++) {
        Step(all, 10);
        if (Phase(all[1]) != MP_UI_PLAYING && !all[1]->debugHeld()) { CHECK(all[1]->debugHeld()); break; }   // held on every frame until joined
    }
    CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 5000));
    CHECK(all[1]->debugHeld() && all[1]->active());
    CHECK(all[1]->localSlot() == 1);
    CHECK(LogCount("slot 1 joined name=Luigi rejoin=1") == 1);
    // vanish (no goodbye) and come back as a new console with the same name within 15 s
    hub.setDown(3, true);   // Peach, slot 2
    Step(all, 2000);
    MpCore* p2 = MakeCore(hub, 9, "Peach", 79);
    SameConsole(p2, all[2]);   // corefix 1: the same console again (its key is kept in mp.cfg)
    all.push_back(p2);
    CHECK(ScanJoin(all, p2));
    CHECK(WaitPhase(all, p2, MP_UI_PLAYING, 3000));
    CHECK(p2->localSlot() == 2);
    CHECK(all[0]->slotPresent(2));
    Step(all, 1000);
    CHECK(all[1]->slotPresent(2));
    Free(all);
    printf("rejoin: ok\n");
}


// corefix 1: a HELLO with the same name does not take a present guest's slot (the default name is "Player", and Wi-Fi power save
// easily makes a guest silent for a second or two). Only the console that held the slot (same console id) takes it back.
static void TestSameNameNoSteal() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 301);
    MpCore* a = MakeCore(hub, 2, "Player", 302);
    MpCore* b = MakeCore(hub, 3, "Player", 303);
    std::vector<MpCore*> all = {h, a, b};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, a));
    CHECK(WaitPhase(all, a, MP_UI_LOBBY, 3000));
    const int sa = a->localSlot();
    const int backBefore = LogCount("is back, the old session goes");
    // a goes silent for 1.5 s (power save bunching: it is still a live console)
    std::vector<MpCore*> rest = {h};
    Step(rest, 1500);
    CHECK(h->slotPresent(sa));
    // another console called "Player" joins: it gets its own slot, a keeps its own
    CHECK(ScanJoin(all, b));
    CHECK(WaitPhase(all, b, MP_UI_LOBBY, 3000));
    CHECK(b->localSlot() != sa && b->localSlot() > 0);
    Step(all, 500);
    CHECK(Phase(a) == MP_UI_LOBBY && a->active() && a->localSlot() == sa);
    CHECK(h->slotPresent(sa) && h->slotPresent(b->localSlot()));
    CHECK(LogCount("is back, the old session goes") == backBefore);
    // the console that held the slot (same id) takes it back at once, even though its old session is not silent for long
    MpCore* a2 = MakeCore(hub, 4, "Player", 304);
    SameConsole(a2, a);
    all.push_back(a2);
    delete a;
    all[1] = 0;
    CHECK(ScanJoin(all, a2));
    CHECK(WaitPhase(all, a2, MP_UI_LOBBY, 3000));
    CHECK(a2->localSlot() == sa);
    char want[64];
    snprintf(want, sizeof want, "slot %d joined name=Player rejoin=1", sa);
    CHECK(LogCount(want) == 1);
    Free(all);
    printf("same-name join does not take a live guest's slot: ok\n");
}


// corefix 2: ghost joiners. A HELLO from an address that never acknowledges its WELCOME holds a slot only for MP_ACK_STALL_MS, even
// when it keeps sending bare datagrams; and a real guest on a bad link (20% loss, 400 ms power-save bunching) is not dropped by that.
// core2: the ghost does the cookie join (it can receive at its address: a real ghost could), then sends sealed datagrams only
static RawJoiner* g_ghost = 0;
static void GhostHello(MpLoopHub& hub, MpCore* host, uint32_t endpoint, const char* name, uint32_t id, std::vector<MpCore*>& all) {
    delete g_ghost;
    g_ghost = new RawJoiner(endpoint, host->debugLocalAddr());
    g_ghost->fp = host->fingerprint();
    RawJoiner::Hello h;
    h.name = name;
    h.consoleId = id;
    CHECK(g_ghost->join(hub, all, h));
    g_ghost->done(hub);
}
static void GhostBeat(MpLoopHub& hub, MpCore*, uint32_t) {   // a bare heartbeat claiming to carry an ack
    uint8_t body[8] = {0};
    std::vector<uint8_t> pk;
    WritePacket(pk, MP_MSG_HEARTBEAT, 1, MP_HDR_ACK, 0, body, 8);
    g_ghost->send(hub, pk.data(), pk.size());
}
static void TestGhostJoiners() {
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 311);
        MpCore* g = MakeCore(hub, 3, "Real", 312);
        std::vector<MpCore*> all = {h};
        MpHostParams hp = HostParams(MP_MODE_TOGETHER, 2);
        CHECK(h->uiHost(&hp));
        Step(all, 100);
        GhostHello(hub, h, 7, "Ghost", 0xABCDEF01u, all);
        Step(all, 100);
        CHECK(h->slotPresent(1));
        // the ghost keeps the room full with bare datagrams, but never acknowledges
        for (int t = 0; t < 3000; t += 250) {
            GhostBeat(hub, h, 7);
            Step(all, 250);
        }
        CHECK(h->slotPresent(1));   // (still inside the stall limit)
        for (int t = 0; t < 3500; t += 250) {
            GhostBeat(hub, h, 7);
            Step(all, 250);
        }
        CHECK(!h->slotPresent(1));
        CHECK(LogCount("slot 1 left reason=TIMEOUT") + LogCount("no acknowledgement") >= 1);
        // the room is free again for a real player
        all.push_back(g);
        CHECK(ScanJoin(all, g));
        CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
        Free(all);
    }
    {
        // a real guest on a bad link keeps its slot for a minute of traffic
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 321);
        all[0]->uiStart();
        Step(all, 300);
        MpLoopImpairment imp = {0.20f, 0.05f, 10, 400, 0};
        hub.setImpairment(imp, 99);
        Book b;
        const int dropsBefore = LogCount("no acknowledgement");
        for (int turn = 0; turn < 3750; turn++) {   // 60 s at 16 ms
            if (turn % 4 == 0) SendStar(b, all[turn % 8 < 4 ? 0 : 1]);
            Step(all, 16, 8);
            for (MpCore* c : all) Drain(b, c);
            CHECK(all[0]->slotPresent(1) && all[1]->active());
            if (g_fail) break;
        }
        CHECK(LogCount("no acknowledgement") == dropsBefore);
        Free(all);
    }
    printf("ghost joiners are removed after %d ms without acknowledgement; a guest on a bad link is not: ok\n", MP_ACK_STALL_MS);
}


// corefix 3: a forged merge beacon (public session id + fingerprint, "open", more players) does not make a healthy host abandon its
// session. A beacon from an unknown address is ignored; one from a member's address needs MP_MERGE_BEACONS consecutive beacons.
static std::vector<uint8_t> g_hostBeacon;
static void BeaconTap(void*, const MpAddr& from, const MpAddr&, const void* data, size_t len) {
    if (from.host == 1 && len == MP_BEACON_BYTES) g_hostBeacon.assign((const uint8_t*)data, (const uint8_t*)data + len);
}
static void ForgeBeacon(MpLoopHub& hub, MpCore* host, uint32_t fromHost, uint32_t seq) {
    MpBeacon b;
    CHECK(!g_hostBeacon.empty() && mp_beacon_read(g_hostBeacon.data(), g_hostBeacon.size(), &b));
    b.players = 4;
    b.maxPlayers = 4;
    b.flags |= 1;
    b.epoch = 9;
    b.hostSlot = 1;
    b.seq = seq;
    uint8_t raw[MP_BEACON_BYTES];
    CHECK(mp_beacon_write(raw, sizeof raw, b) == MP_BEACON_BYTES);
    hub.inject(MpAddr{fromHost, MP_SESSION_PORT}, MpAddr{host->debugLocalAddr().host, MP_LAN_PORT}, raw, sizeof raw);
}
static void TestForgedMergeBeacon() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    g_hostBeacon.clear();
    hub.setTap(BeaconTap, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 331);
    all[0]->uiStart();
    Step(all, 1200);
    CHECK(!g_hostBeacon.empty());
    uint32_t seq = 5000;
    for (int i = 0; i < 40; i++) {   // 10 s of forged beacons from a stranger (address 9)
        ForgeBeacon(hub, all[0], 9, seq++);
        Step(all, 250);
    }
    CHECK(all[0]->active() && all[0]->isHost() && all[0]->slotPresent(1) && all[1]->active());
    CHECK(LogCount("merge: a room of this session is ahead") == 0);
    // one beacon from a member's address is not enough either
    ForgeBeacon(hub, all[0], 2, seq++);
    Step(all, 1000);
    CHECK(all[0]->active() && all[0]->isHost() && LogCount("merge: a room of this session is ahead") == 0);
    // core2: nor 10 s of them from a member's address (the beacon's tag needs the room key, which only travels sealed)
    const int untagged = LogCount("without the room key's tag");
    for (int i = 0; i < 40; i++) {
        ForgeBeacon(hub, all[0], 2, seq++);
        Step(all, 250);
    }
    CHECK(all[0]->active() && all[0]->isHost() && all[0]->slotPresent(1) && LogCount("merge: a room of this session is ahead") == 0);
    CHECK(LogCount("without the room key's tag") > untagged);
    // (the host's own beacons carry the tag: 52 bytes)
    CHECK(g_hostBeacon.size() == MP_BEACON_BYTES && MP_BEACON_BYTES == 52);
    Free(all);
    hub.setTap(0, 0);
    printf("forged merge beacons do not end a session: ok\n");
}



// corefix 10: randomness and beacon trust. (a) a core without a test seed takes its ids from the strong source (two cores differ);
// (b) a scanner does not move a room to another address on one forged beacon of the same session; (c) a guest that rejoins by session
// does not follow a beacon from an address that was not in the room.
static void TestBeaconTrustAndRandom() {
    {   // (b)
        MpLanScan scan;
        MpBeacon b;
        memset(&b, 0, sizeof b);
        b.proto = MP_PROTO_VERSION;
        b.fingerprint = 123;
        b.sessionId = 0x1234567u;
        b.mode = MP_MODE_TOGETHER;
        b.players = 1;
        b.epoch = 0;
        b.seq = 10;
        MpAddr real{5, MP_SESSION_PORT}, fake{9, MP_SESSION_PORT};
        scan.add(real, b, 1000, 123);
        MpAddr got;
        uint32_t sid;
        MpLocalRoom room;
        for (int i = 0; i < 20; i++) {   // a forger beacons as steadily as the host (higher epoch even): the host that holds the room keeps it
            b.seq = 100 + i;
            b.epoch = 5;
            scan.add(fake, b, 1500 + 500 * i, 123);
            b.epoch = 0;
            b.seq = 20 + i;
            scan.add(real, b, 1500 + 500 * i + 10, 123);
        }
        CHECK(scan.get(0, &room, &got, &sid) && got.host == 5);
    }
    {   // a migrated host (the old one is silent): the new address takes over after 3 beacons over 1 s
        MpLanScan scan;
        MpBeacon b;
        memset(&b, 0, sizeof b);
        b.proto = MP_PROTO_VERSION;
        b.fingerprint = 123;
        b.sessionId = 0x1234567u;
        b.mode = MP_MODE_TOGETHER;
        b.players = 1;
        b.seq = 10;
        MpAddr real{5, MP_SESSION_PORT}, next{6, MP_SESSION_PORT};
        scan.add(real, b, 1000, 123);
        b.epoch = 1;
        MpAddr got;
        uint32_t sid;
        MpLocalRoom room;
        b.seq = 50;
        scan.add(next, b, 2000, 123);
        CHECK(scan.get(0, &room, &got, &sid) && got.host == 5);   // one beacon: not yet
        b.seq = 51;
        scan.add(next, b, 2500, 123);
        CHECK(scan.get(0, &room, &got, &sid) && got.host == 5);
        b.seq = 52;
        scan.add(next, b, 3000, 123);
        CHECK(scan.get(0, &room, &got, &sid) && got.host == 5);   // core2: the first address beaconed less than 2.5 s ago (MP_LAN_MOVE_SILENT_MS): it keeps the room
        for (uint64_t at = 3500; at <= 6000; at += 500) { b.seq++; scan.add(next, b, at, 123); }   // (the new host keeps beaconing)
        b.seq++;
        scan.add(next, b, 6100, 123);
        CHECK(scan.get(0, &room, &got, &sid) && got.host == 6);   // ... the first one silent for 2.5 s: the room moves
    }
    {   // (a)
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* c1 = MakeCore(hub, 1, "A", 0);
        MpCore* c2 = MakeCore(hub, 2, "B", 0);
        uint8_t k1[16], k2[16];
        c1->debugConsoleKey(k1);
        c2->debugConsoleKey(k2);
        CHECK(memcmp(k1, k2, 16) != 0);
        delete c1;
        delete c2;
    }
    {   // (c)
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        g_hostBeacon.clear();
        hub.setTap(BeaconTap, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 361);
        all[0]->uiStart();
        Step(all, 1200);
        CHECK(!g_hostBeacon.empty());
        all[1]->suspend();
        Step(all, 300);
        hub.setDown(1, true);   // the real host does not answer / beacon
        CHECK(all[1]->resume());
        uint32_t seq = 9000;
        for (int i = 0; i < 12; i++) {
            ForgeBeacon(hub, all[1], 9, seq++);
            Step(all, 250);
        }
        CHECK(Phase(all[1]) == MP_UI_JOINING);
        CHECK(LogCount("rejoin: a beacon of the session from an unknown address") >= 1);
        hub.setDown(1, false);
        CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 8000));   // the real host's beacon is followed
        hub.setTap(0, 0);
        Free(all);
    }
    printf("beacon trust and randomness: ok\n");
}

// corefix 6: privacy. The persistent console secret never leaves the console: HELLO / RESUME carry a keyed hash of it and the session
// id, so the same console shows another id to every session (and no fixed id), and the same id again when it rejoins the same session.
static std::vector<std::vector<uint8_t>> g_guestWire;   // everything endpoint 2 sent
static void WireTap(void*, const MpAddr& from, const MpAddr&, const void* data, size_t len) {
    if (from.host == 2) g_guestWire.emplace_back((const uint8_t*)data, (const uint8_t*)data + len);
}
static std::vector<uint32_t> HelloIds() {
    std::vector<uint32_t> ids;
    for (auto& d : g_guestWire) {
        size_t pl = 0;
        const uint8_t* h = FindPacket(d.data(), d.size(), MP_MSG_HELLO, &pl);   // (a datagram may be a bundle)
        if (h && pl >= 44 && (h[5] & MP_HDR_RELIABLE)) {   // (core2: the reliable HELLO; the first one carries no id)
            uint32_t v;
            memcpy(&v, h + 40, 4);
            ids.push_back(v);
        }
    }
    return ids;
}
static void TestSessionIds() {
    {   // SipHash-2-4: the published test vector (key 00..0f, message 00..0e)
        uint8_t key[16], msg[15];
        for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
        for (int i = 0; i < 15; i++) msg[i] = (uint8_t)i;
        CHECK(mp_siphash24(key, msg, 15) == 0xa129ca6149be45e5ull);
        CHECK(mp_session_wire_id(key, 1) != mp_session_wire_id(key, 2) && mp_session_wire_id(key, 1) != 0);
    }
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    g_guestWire.clear();
    hub.setTap(WireTap, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 371);
    MpCore* g = MakeCore(hub, 2, "Guest", 372);
    std::vector<MpCore*> all = {h, g};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, g));
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
    const uint32_t first = g->debugWireId();
    g->uiLeave();
    Step(all, 1000);
    h->uiLeave();
    Step(all, 1000);
    CHECK(h->uiHost(&hp));   // a new session (new session id)
    Step(all, 100);
    CHECK(ScanJoin(all, g));
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
    const uint32_t second = g->debugWireId();
    const int slot = g->localSlot();
    CHECK(first != second && first != g->debugConsoleId() && second != g->debugConsoleId());
    // a rejoin of the same session (suspend / resume) shows the same id and gets the same slot
    g->suspend();
    Step(all, 500);
    CHECK(g->resume());
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 5000));
    CHECK(g->debugWireId() == second && g->localSlot() == slot);
    std::vector<uint32_t> ids = HelloIds();
    bool sawFirst = false, sawSecond = false, onlyOurs = true;
    for (uint32_t v : ids) {
        sawFirst |= v == first;
        sawSecond |= v == second;
        onlyOurs &= v == first || v == second;
    }
    CHECK(ids.size() >= 3 && sawFirst && sawSecond && onlyOurs && ids[0] == first && ids.back() == second);
    // the persistent secret's handle is on no datagram this console ever sent
    const uint32_t handle = g->debugConsoleId();
    int leaks = 0;
    for (auto& d : g_guestWire)
        for (size_t i = 0; i + 4 <= d.size(); i++)
            if (!memcmp(d.data() + i, &handle, 4)) leaks++;
    CHECK(leaks == 0);
    hub.setTap(0, 0);
    Free(all);
    printf("session ids: per session, stable over a rejoin, the persistent secret never sent: ok\n");
}


// corefix 7: names follow the chat character rules at every entry point, and the host numbers a duplicate ("Player 2").
static bool NameClean(const char* n) {   // what a name may look like after the cleaner
    const size_t len = strlen(n);
    if (len == 0) return true;   // core2: nothing usable left (the character's name is shown)
    if (len > MP_NAME_BYTES || n[0] == ' ' || n[len - 1] == ' ') return false;
    size_t i = 0;
    bool prevSpace = false;
    while (i < len) {
        const uint32_t c = mp_chat_utf8_next((const uint8_t*)n, len, &i);
        if (c == 0xFFFFFFFFu || !mp_chat_char_ok(c)) return false;
        if (c == ' ' && prevSpace) return false;
        prevSpace = c == ' ';
    }
    return true;
}
static void TestNames() {
    // fuzz the cleaner: random bytes, and random mixes of the characters that matter
    static const uint32_t kNasty[] = {0x202E, 0x202D, 0x200B, 0x200D, 0xFEFF, 0xE000, 0xE123, 0xF8FF, 0x1F600, 0x10000, 0x2066, 0x2028, 0xAD,
                                      0xFFFD, 0xFFFF, 0xA0, 0x3000, 0x7F, 0x85, 0x01, ' ', ' ', 'a', 'Z', 0xE9, 0x3042, 0x20AC};
    uint32_t rng = 12345;
    auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    int bad = 0;
    for (int it = 0; it < 200000; it++) {
        uint8_t buf[40];
        size_t n = 0;
        if (it % 2) {
            n = rnd() % sizeof buf;
            for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)rnd();
        } else {
            const int chars = (int)(rnd() % 12);
            for (int k = 0; k < chars && n + 4 < sizeof buf; k++) {
                const uint32_t c = kNasty[rnd() % (sizeof kNasty / sizeof kNasty[0])];
                if (c < 0x80) buf[n++] = (uint8_t)c;
                else if (c < 0x800) { buf[n++] = (uint8_t)(0xC0 | (c >> 6)); buf[n++] = (uint8_t)(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { buf[n++] = (uint8_t)(0xE0 | (c >> 12)); buf[n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (uint8_t)(0x80 | (c & 0x3F)); }
                else { buf[n++] = (uint8_t)(0xF0 | (c >> 18)); buf[n++] = (uint8_t)(0x80 | ((c >> 12) & 0x3F)); buf[n++] = (uint8_t)(0x80 | ((c >> 6) & 0x3F)); buf[n++] = (uint8_t)(0x80 | (c & 0x3F)); }
            }
        }
        char out[MP_NAME_BYTES + 1], again[MP_NAME_BYTES + 1];
        mp_name_clean((const char*)buf, n, out);
        mp_name_clean(out, strlen(out), again);
        if (!NameClean(out) || strcmp(out, again) != 0) {
            if (bad++ < 3) {
                fprintf(stderr, "  name fuzz: bad output for input of %zu bytes:", n);
                for (size_t k = 0; k < n; k++) fprintf(stderr, " %02x", buf[k]);
                fprintf(stderr, " -> [%s]\n", out);
            }
        }
    }
    CHECK(bad == 0);
    char o[MP_NAME_BYTES + 1];
    mp_name_clean("Ma\xE2\x80\xAEri\xEE\x80\x80o\xE2\x80\x8B", 14, o);   // bidi override, private use, zero width
    CHECK(!strcmp(o, "Mari?o"));
    // a HELLO with such a name: the host's roster shows the clean name
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 381);
        std::vector<MpCore*> all = {h};
        MpHostParams hp = HostParams();
        CHECK(h->uiHost(&hp));
        Step(all, 100);
        RawJoiner j(7, h->debugLocalAddr());
        j.fp = h->fingerprint();
        RawJoiner::Hello hl;
        const uint8_t nm[MP_NAME_BYTES] = {'B', 'o', 0xE2, 0x80, 0xAE, 'b', 0xEE, 0x80, 0x80, 'y', 0xE2, 0x80, 0x8B};
        memcpy(hl.rawName, nm, sizeof nm);
        hl.useRaw = true;
        hl.consoleId = 0x5555u;
        CHECK(j.join(hub, all, hl));
        j.done(hub);
        Step(all, 100);
        char nameBuf[MP_NAME_BYTES + 1];
        h->slotName(1, nameBuf);
        CHECK(h->slotPresent(1) && !strcmp(nameBuf, "Bob?y"));
        Free(all);
    }
    // duplicates: three consoles called "Player" and one with a 16-byte name twice
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Player", 382);
        MpCore* a = MakeCore(hub, 2, "player", 383);   // (case does not make a name different)
        MpCore* b = MakeCore(hub, 3, "Player", 384);
        MpCore* c = MakeCore(hub, 4, "Player 2", 385);   // (and "Player 2" is taken by then: the next number)
        std::vector<MpCore*> all = {h, a, b, c};
        MpHostParams hp = HostParams();
        CHECK(h->uiHost(&hp));
        Step(all, 100);
        for (MpCore* g : {a, b, c}) {
            CHECK(ScanJoin(all, g));
            CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
        }
        Step(all, 600);
        for (MpCore* v : all) {
            char names[MP_MAX_PLAYERS][MP_NAME_BYTES + 1];
            int cnt = 0;
            for (int s = 0; s < MP_MAX_PLAYERS; s++)
                if (v->slotPresent(s)) v->slotName(s, names[cnt++]);
            CHECK(cnt == 4);
            for (int x = 0; x < cnt; x++)
                for (int y = x + 1; y < cnt; y++) CHECK(strcasecmp(names[x], names[y]) != 0);
        }
        char n1[MP_NAME_BYTES + 1], n2[MP_NAME_BYTES + 1], n3[MP_NAME_BYTES + 1];
        h->slotName(1, n1); h->slotName(2, n2); h->slotName(3, n3);
        CHECK(!strcmp(n1, "player 2") && !strcmp(n2, "Player 3") && !strcmp(n3, "Player 2 2"));
        Free(all);
    }
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "ABCDEFGHIJKLMNOP", 386);
        MpCore* g = MakeCore(hub, 2, "ABCDEFGHIJKLMNOP", 387);
        MpCore* g2 = MakeCore(hub, 3, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 388);   // 8 x e-acute = 16 bytes
        MpCore* g3 = MakeCore(hub, 4, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 389);
        std::vector<MpCore*> all = {h, g, g2, g3};
        MpHostParams hp = HostParams();
        CHECK(h->uiHost(&hp));
        Step(all, 100);
        for (MpCore* x : {g, g2, g3}) {
            CHECK(ScanJoin(all, x));
            CHECK(WaitPhase(all, x, MP_UI_LOBBY, 3000));
        }
        char nm[MP_NAME_BYTES + 1];
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            h->slotName(s, nm);
            CHECK(strlen(nm) <= MP_NAME_BYTES && NameClean(nm));
        }
        h->slotName(1, nm);
        CHECK(!strcmp(nm, "ABCDEFGHIJKLMN 2"));
        h->slotName(3, nm);
        CHECK(!strcmp(nm, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9 2"));   // (cut at a character: 7 + " 2" = 16 bytes)
        Free(all);
    }
    printf("names: chat character rules, fuzz, duplicates numbered within the length limit: ok\n");
}

// ---------------------------------------------------------------------------------------------------------------------------
// Host migration (DESIGN.md section 8)
static std::vector<MpCore*> Survivors(std::vector<MpCore*>& all, std::vector<int> gone) {
    std::vector<MpCore*> v;
    for (size_t i = 0; i < all.size(); i++) {
        bool g = false;
        for (int x : gone) if ((int)i == x) g = true;
        if (!g) v.push_back(all[i]);
    }
    return v;
}

static int CheckStreams(Book& b, std::vector<MpCore*>& alive, const char* what) {
    int bad = 0;
    for (MpCore* r : alive)
        for (MpCore* s : alive) {
            if (r == s) continue;
            const std::vector<uint32_t>& v = b.got[std::make_pair(r, b.slotOf[s])];
            if (!(v.size() == b.sent[s] && InOrder(v) && (v.empty() || v[0] == 0))) {
                fprintf(stderr, "  %s: slot %d from slot %d: got %zu of %u inorder=%d\n", what, b.slotOf[r], b.slotOf[s], v.size(), b.sent[s],
                        InOrder(v));
                bad++;
            }
        }
    return bad;
}

static void Traffic(Book& b, std::vector<MpCore*>& senders, std::vector<MpCore*>& all, int turns) {
    for (int turn = 0; turn < turns; turn++) {
        for (MpCore* c : senders)
            if ((turn + b.slotOf[c]) % 4 == 0 && c->active()) SendStar(b, c);
        Step(all, 16, 8);
        for (MpCore* c : all) Drain(b, c);
    }
}

static void TestMigrationDrop() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 81);
    all[0]->uiStart();
    Step(all, 300);
    Book b;
    for (MpCore* c : all) b.slotOf[c] = c->localSlot();
    MpLoopImpairment imp = {0.10f, 0.05f, 10, 100, 0};
    hub.setImpairment(imp, 4242);
    std::vector<MpCore*> guests = Survivors(all, {0});
    Traffic(b, all, all, 300);
    hub.setDown(1, true);   // the host vanishes mid-traffic
    Traffic(b, guests, all, 900);   // ~15 s: migration happens while the guests keep sending
    hub.setImpairment(MpLoopImpairment{0, 0, 0, 0, 0}, 1);
    Traffic(b, guests, all, 0);
    Step(all, 6000);
    for (MpCore* c : all) Drain(b, c);
    int hosts = 0;
    for (MpCore* c : guests) {
        CHECK(c->active() && Phase(c) == MP_UI_PLAYING);
        if (c->isHost()) hosts++;
        CHECK(c->hostSlot() == 1 && c->hostEpoch() == 1);
        CHECK(!c->slotPresent(0));
    }
    CHECK(hosts == 1 && guests[0]->isHost());
    CHECK(CheckStreams(b, guests, "drop") == 0);
    for (MpCore* c : guests) CHECK(c->debugGaps() == 0);
    // the new host does not save; nobody does after a migration
    for (MpCore* c : guests) CHECK(c->saveSuppressed() && !c->saveAutoconfirm());
    // events
    MpCoreEvent ev;
    int became = 0, changed = 0;
    for (MpCore* c : guests)
        while (c->pollEvent(&ev)) {
            if (ev.kind == MP_EV_BECAME_HOST) became++;
            if (ev.kind == MP_EV_HOST_CHANGED) changed++;
        }
    CHECK(became == 1 && changed == 2);
    printf("migration (host drop, 3 guests, 10%% loss): ok, %u events per guest\n", b.sent[guests[0]]);
    // second migration in a row: the new host drops too
    hub.setDown(2, true);
    std::vector<MpCore*> last = Survivors(all, {0, 1});
    Traffic(b, last, all, 900);
    Step(all, 6000);
    for (MpCore* c : all) Drain(b, c);
    for (MpCore* c : last) {
        CHECK(c->active() && c->hostSlot() == 2 && c->hostEpoch() == 2);
    }
    CHECK(last[0]->isHost() && !last[1]->isHost());
    CHECK(CheckStreams(b, last, "second") == 0);
    // and the last host leaving with one player left ends the session for that one ("everyone left")
    last[0]->uiLeave();
    Step(all, 2000);
    CHECK(Phase(last[1]) == MP_UI_IDLE && Error(last[1]) == MP_ERR_HOST_LEFT);
    Free(all);
    printf("migration twice in a row: ok\n");
}

static void TestMigrationGraceful() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 91);
    all[0]->uiStart();
    Step(all, 300);
    Book b;
    for (MpCore* c : all) b.slotOf[c] = c->localSlot();
    Traffic(b, all, all, 100);
    uint8_t where[8] = {3, 1, 1, 0, 0, 0, 7, 0};   // galaxy 3, scenario 1, serial 7
    CHECK(all[0]->send(MP_MSG_WHERE, where, 8, true));
    Step(all, 200);
    CHECK(all[1]->sceneSerial() == 7 && all[2]->sceneSerial() == 7);
    const uint64_t t0 = g_now;
    all[0]->uiLeave();
    std::vector<MpCore*> guests = Survivors(all, {0});
    while (!(guests[0]->isHost() && guests[1]->hostSlot() == 1) && g_now - t0 < 5000) Traffic(b, guests, all, 1);
    CHECK(g_now - t0 < 1500);   // no 5 s wait: the host said it is leaving
    Traffic(b, guests, all, 200);
    Step(all, 2000);
    for (MpCore* c : all) Drain(b, c);
    CHECK(Phase(all[0]) == MP_UI_IDLE && Error(all[0]) == MP_ERR_NONE);
    CHECK(CheckStreams(b, guests, "graceful") == 0);
    // the stored WHERE came again from the new host (same serial: no reload)
    CHECK(guests[1]->sceneSerial() == 7);
    CHECK(LogCount("slot 2 resumed epoch=1") == 1);
    MpCoreEvent ev;
    bool resumed = false;
    while (guests[0]->pollEvent(&ev))
        if (ev.kind == MP_EV_RESUMED && ev.slot == 2) resumed = true;
    CHECK(resumed);
    // the old host can come back as a guest (it takes its slot back)
    CHECK(ScanJoin(all, all[0]));
    CHECK(WaitPhase(all, all[0], MP_UI_PLAYING, 3000));
    CHECK(all[0]->localSlot() == 0 && !all[0]->isHost() && all[0]->hostSlot() == 1);
    CHECK(all[0]->saveSuppressed());
    Free(all);
    printf("migration (graceful host leave): ok\n");
}

static int g_chunkBudget = -1;
static uint32_t g_blockEp = 0;
static bool ChunkFilter(void*, const MpAddr&, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl;
    if (to.host == g_blockEp && FindPacket(d, len, MP_MSG_SNAPSHOT_CHUNK, &pl) && g_chunkBudget >= 0) {
        if (g_chunkBudget == 0) return false;
        g_chunkBudget--;
    }
    return true;
}

static void TestMigrationDuringSnapshot() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 101);
    all[0]->uiStart();
    static uint8_t img[7000], got[MP_SNAPSHOT_MAX];
    for (int i = 0; i < 7000; i++) img[i] = (uint8_t)(i * 7 + 1);
    all[0]->snapshotOffer(img, sizeof img);
    Step(all, 1000);
    CHECK(all[1]->snapshotReady() == sizeof img && all[2]->snapshotReady() == sizeof img);
    // a fourth console joins; its snapshot stalls after 3 chunks
    MpCore* d = MakeCore(hub, 4, "Daisy", 104);
    all.push_back(d);
    g_blockEp = 4;
    g_chunkBudget = 3;
    hub.setFilter(ChunkFilter, 0);
    CHECK(ScanJoin(all, d));
    CHECK(WaitPhase(all, d, MP_UI_PLAYING, 3000));
    Step(all, 500);
    MpUiStatus st;
    d->uiStatus(&st);
    CHECK(d->snapshotReady() == 0 && st.progress > 0 && st.progress < 100);
    CHECK(all[1]->slotPresent(3));
    hub.setDown(1, true);   // the host goes mid-transfer
    g_chunkBudget = -1;
    Step(all, 12000);
    CHECK(d->active() && d->hostSlot() == 1 && d->hostEpoch() == 1);
    CHECK(d->snapshotReady() == sizeof img);
    CHECK(d->snapshotGet(got, sizeof got) == sizeof img && !memcmp(got, img, sizeof img));
    hub.setFilter(0, 0);
    Free(all);
    printf("migration during a snapshot transfer: ok\n");
}

// The old host relayed some events to one guest but not to the successor before it died: after the migration the
// successor gets them again from their senders and relays them, and the guest that had them must not see them twice.
static bool g_starToN = true;
static bool StarFilter(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl;
    if (!g_starToN && from.host == 1 && to.host == 2 && FindPacket(d, len, MP_MSG_STAR, &pl)) return false;
    return true;
}
static void TestMigrationDedupe() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 111);
    all[0]->uiStart();
    Step(all, 300);
    Book b;
    for (MpCore* c : all) b.slotOf[c] = c->localSlot();
    hub.setFilter(StarFilter, 0);
    Traffic(b, all, all, 100);
    g_starToN = false;   // from now on the host's relays to slot 1 (the successor) are lost
    std::vector<MpCore*> guests = Survivors(all, {0});
    Traffic(b, guests, all, 60);
    hub.setDown(1, true);
    g_starToN = true;
    Traffic(b, guests, all, 700);
    Step(all, 4000);
    for (MpCore* c : all) Drain(b, c);
    CHECK(guests[0]->isHost() && guests[1]->hostEpoch() == 1);
    CHECK(CheckStreams(b, guests, "dedupe") == 0);
    hub.setFilter(0, 0);
    Free(all);
    printf("migration with events only some consoles had: ok\n");
}

// corefix 4: the host sends events (origin = the host) and dies after they reached only one of two guests. The new host and the
// other guest must still get them, exactly once (the dead host's history is kept until the next scene, and re-sent to the new host).
static uint32_t g_dropToEp = 0;
static int g_dropFromCounter = 1000;
static bool HostStarFilter(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl = 0;
    const uint8_t* st = g_dropToEp && from.host == 1 && to.host == g_dropToEp ? FindPacket(d, len, MP_MSG_STAR, &pl) : nullptr;
    if (st && pl >= 12 + MP_GAME_PREFIX + 8) {   // (a datagram may be a bundle: the packet is found inside)
        uint32_t v;
        memcpy(&v, st + 12 + MP_GAME_PREFIX, 4);   // (origin sequence u16 and generation u16, then the counter)
        if ((int)v >= g_dropFromCounter) return false;
    }
    return true;
}
static void TestMigrationDeadHostEvents() {
    for (int variant = 0; variant < 2; variant++) {   // 0: the successor (slot 1) missed them; 1: the other guest (slot 2) missed them
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 341 + (uint32_t)variant);
        all[0]->uiStart();
        Step(all, 300);
        Book b;
        hub.setFilter(HostStarFilter, 0);
        g_dropToEp = variant == 0 ? 2 : 3;
        g_dropFromCounter = 3;
        for (int i = 0; i < 6; i++) {
            SendStar(b, all[0]);
            Step(all, 30);
            for (MpCore* c : all) Drain(b, c);
        }
        // (the host's last sends reached one guest only) the host dies
        hub.setDown(1, true);
        g_dropToEp = 0;
        Step(all, 9000);
        for (MpCore* c : all) Drain(b, c);
        std::vector<MpCore*> guests = Survivors(all, {0});
        CHECK(guests[0]->isHost() && guests[1]->hostEpoch() == 1);
        for (MpCore* g : guests) {
            const std::vector<uint32_t>& v = b.got[{g, 0}];
            const bool exact = v.size() == 6 && InOrder(v) && v[0] == 0;
            if (!exact) fprintf(stderr, "  variant %d slot %d got %zu of 6 events of the dead host\n", variant, g->localSlot(), v.size());
            CHECK(exact);
        }
        hub.setFilter(0, 0);
        Free(all);
    }
    printf("migration: events a dead host relayed to only some guests reach everyone once: ok\n");
}

// ---------------------------------------------------------------------------------------------------------------------------
// netcore: exactly-once delivery of tracked game events per origin MEMBERSHIP across host migration, rejoin and slot reuse.
// Xo is a ledger: every event carries (sender index, sender membership, counter); a receiver's membership changes when it is
// welcomed again. Per (receiver membership, origin membership) the counters delivered must be consecutive, each once; at the end
// every pair that is still together must have everything (no tail lost). A receiver that was re-admitted (resync) starts over.
struct Xo {
    std::vector<MpCore*> cs;
    std::vector<uint32_t> memb, cnt;
    std::map<std::tuple<int, uint32_t, int, uint32_t>, uint32_t> last;
    int dups = 0, gaps = 0, stale = 0;
    explicit Xo(std::vector<MpCore*>& all) : cs(all), memb(all.size(), 1), cnt(all.size(), 0) {
        MpCoreEvent ev;   // (the WELCOMEs that made the current memberships are behind us)
        for (MpCore* c : cs) while (c && c->pollEvent(&ev)) {}
    }
    int idx(MpCore* c) { for (size_t i = 0; i < cs.size(); i++) if (cs[i] == c) return (int)i; return -1; }
    void rejoined(int i) { memb[i]++; cnt[i] = 0; }
    void send(int i) {
        uint8_t p[9];
        p[0] = (uint8_t)i;
        memcpy(p + 1, &memb[i], 4);
        memcpy(p + 5, &cnt[i], 4);
        if (cs[i]->active() && cs[i]->send(MP_MSG_KILL, p, 9, true)) cnt[i]++;
    }
    void drain(int i) {
        MpCoreEvent ev;
        while (cs[i]->pollEvent(&ev))
            if (ev.kind == MP_EV_JOINED && ev.slot == cs[i]->localSlot() && !cs[i]->isHost()) rejoined(i);
        MpMsg m;
        while (cs[i]->poll(&m)) {
            if (m.type != MP_MSG_KILL || m.len != 9) continue;
            uint32_t om, oc;
            memcpy(&om, m.data + 1, 4);
            memcpy(&oc, m.data + 5, 4);
            const int o = m.data[0];
            if (om < memb[o]) stale++;
            auto key = std::make_tuple(i, memb[i], o, om);
            auto it = last.find(key);
            if (it != last.end()) {
                if (oc <= it->second) { dups++; if (dups <= 3) fprintf(stderr, "  dup: receiver %d origin %d memb %u counter %u (last %u)\n", i, o, om, oc, it->second); }
                else if (oc != it->second + 1) { gaps++; if (gaps <= 3) fprintf(stderr, "  gap: receiver %d origin %d memb %u counter %u (last %u)\n", i, o, om, oc, it->second); }
            }
            if (it == last.end() || oc > it->second) last[key] = oc;
        }
    }
    void drainAll() { for (size_t i = 0; i < cs.size(); i++) if (cs[i]) drain((int)i); }
    void sendAll(std::vector<int> who) { for (int i : who) send(i); }
    // everything sent in the current memberships of `who` reached the others among `who`
    int tailMissing(std::vector<int> who) {
        int n = 0;
        for (int r : who)
            for (int o : who) {
                if (r == o || !cnt[o]) continue;
                auto it = last.find(std::make_tuple(r, memb[r], o, memb[o]));
                if (it == last.end() || it->second + 1 != cnt[o]) {
                    n++;
                    fprintf(stderr, "  tail: receiver %d has %d of origin %d's %u events\n", r, it == last.end() ? 0 : (int)it->second + 1, o, cnt[o]);
                }
            }
        return n;
    }
};
static void XoStep(Xo& x, std::vector<MpCore*>& all, int ms, std::vector<int> senders, int every = 50) {
    for (int t = 0; t < ms; t += every) {
        x.sendAll(senders);
        Step(all, every);
        x.drainAll();
    }
}
// (filters) links cut between two endpoints, both ways; and the BYE of endpoint 1 to one endpoint
static uint32_t g_cutA = 0, g_cutB = 0, g_byeFrom = 0, g_byeTo = 0;
static bool CutFilter(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    if (g_cutA && ((from.host == g_cutA && to.host == g_cutB) || (from.host == g_cutB && to.host == g_cutA))) return false;
    size_t pl = 0;
    if (g_byeFrom && from.host == g_byeFrom && to.host == g_byeTo && FindPacket(data, len, MP_MSG_BYE, &pl)) return false;
    return true;
}

// (C) the host leaves with a goodbye that slot 1 never hears: slot 2 claims 3 s later (the BYE told the others), slot 1 claims at 5 s
// of silence, two claims for one epoch, slot 2 steps down. Slot 3 switched from slot 2 to slot 1: slot 2's events must not come twice.
static void TestExactlyOnceDoubleClaim() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(CutFilter, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 971);
    all[0]->uiStart();
    Step(all, 300);
    Xo x(all);
    XoStep(x, all, 1500, {0, 1, 2, 3});
    g_byeFrom = 1; g_byeTo = 2;   // slot 1 misses the goodbye (and everything after it from the old host)
    g_cutA = 1; g_cutB = 2;
    all[0]->uiLeave();
    XoStep(x, all, 12000, {1, 2, 3});
    g_byeFrom = g_cutA = 0;
    XoStep(x, all, 3000, {});
    std::vector<MpCore*> g = Survivors(all, {0});
    int hosts = 0;
    for (MpCore* c : g) hosts += c->isHost() ? 1 : 0;
    CHECK(hosts == 1 && g[0]->isHost());
    const int tail = x.tailMissing({1, 2, 3});
    if (x.dups || x.gaps || tail) fprintf(stderr, "  double claim: dups=%d gaps=%d tail=%d stale=%d\n", x.dups, x.gaps, tail, x.stale);
    CHECK(x.dups == 0 && x.gaps == 0 && tail == 0);
    hub.setFilter(0, 0);
    Free(all);
    printf("exactly once: two claims for one epoch, a follower switches hosts: ok\n");
}

// (B) the successor misses a rejoin (cut off from the host for 3 s while slot 3 leaves and comes back, a new membership with fewer
// events than its first one), then the host dies: the new host's history knows only slot 3's first membership. Nobody may get the
// old membership's events again, and slot 3's new events must reach everyone.
// (A) the same with a follower (slot 2) cut off instead of the successor: the new host's catch-up of slot 3's new membership comes
// before the ROSTER that tells the follower about the rejoin.
static void ExactlyOnceMissedRejoin(int cutEp, uint32_t seed) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(CutFilter, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, seed);
    all[0]->uiStart();
    Step(all, 300);
    Xo x(all);
    XoStep(x, all, 4000, {0, 1, 2, 3});   // slot 3's first membership: 80 events
    g_cutA = 1; g_cutB = (uint32_t)cutEp;
    all[3]->uiLeave();
    XoStep(x, all, 800, {0, 1, 2});
    CHECK(ScanJoin(all, all[3]));
    CHECK(WaitPhase(all, all[3], MP_UI_PLAYING, 3000) || WaitPhase(all, all[3], MP_UI_LOBBY, 10));
    x.drainAll();
    XoStep(x, all, 800, {0, 1, 2, 3});   // its second membership: a few events
    hub.setDown(1, true);                // the host dies; the cut heals (nothing to carry any more)
    g_cutA = 0;
    std::vector<MpCore*> g = Survivors(all, {0});
    XoStep(x, all, 12000, {1, 2, 3});
    XoStep(x, all, 3000, {});
    CHECK(g[0]->isHost() && g[1]->hostSlot() == 1 && g[2]->hostSlot() == 1);
    const int tail = x.tailMissing({1, 2, 3});
    if (x.dups || x.gaps || tail) fprintf(stderr, "  missed rejoin (cut ep %d): dups=%d gaps=%d tail=%d stale=%d\n", cutEp, x.dups, x.gaps, tail, x.stale);
    CHECK(x.dups == 0 && x.gaps == 0 && tail == 0);
    hub.setFilter(0, 0);
    Free(all);
}
static void TestExactlyOnceMissedRejoin() {
    ExactlyOnceMissedRejoin(2, 981);   // (B) the successor is the one that missed it: a stale new host
    ExactlyOnceMissedRejoin(3, 983);   // (A) a follower missed it
    printf("exactly once: a rejoin some consoles missed, then the host dies: ok\n");
}

// (D) a follower the new host cannot catch up: slot 2 is cut off from the host for 3.5 s while slot 1 leaves and comes back (its history
// starts again at its new WELCOME), then the host dies and slot 1 takes over. What slot 2 missed of slots 0 and 3 is in nobody's
// history any more: slot 2 must be admitted again (a new membership) instead of carrying on with a silent hole.
static void TestExactlyOnceUnservable() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(CutFilter, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 991);
    all[0]->uiStart();
    Step(all, 300);
    Xo x(all);
    XoStep(x, all, 1000, {0, 1, 2, 3});
    g_cutA = 1; g_cutB = 3;
    XoStep(x, all, 300, {0, 1, 2, 3});
    all[1]->uiLeave();
    XoStep(x, all, 800, {0, 2, 3});
    CHECK(ScanJoin(all, all[1]));
    CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 3000));
    x.drainAll();
    XoStep(x, all, 1500, {0, 1, 2, 3});
    hub.setDown(1, true);
    g_cutA = 0;
    std::vector<MpCore*> g = Survivors(all, {0});
    XoStep(x, all, 20000, {1, 2, 3});
    XoStep(x, all, 3000, {});
    CHECK(g[0]->isHost() && g[1]->active() && g[1]->hostSlot() == 1 && g[2]->hostSlot() == 1);
    const int tail = x.tailMissing({1, 2, 3});
    if (x.dups || x.gaps || tail) fprintf(stderr, "  unservable catch-up: dups=%d gaps=%d tail=%d\n", x.dups, x.gaps, tail);
    CHECK(x.dups == 0 && x.gaps == 0 && tail == 0);
    CHECK(x.memb[2] >= 2);               // it was admitted again
    CHECK(LogCount("cannot be caught up") >= 1);
    hub.setFilter(0, 0);
    Free(all);
    printf("exactly once: a follower the new host cannot catch up is admitted again: ok\n");
}

// netcore: a guest that learns from a ROSTER that a member was removed must remember its address too: if it hosts a room later, that
// member's room of the same session is the one to merge into (corefix 3 vouches only for remembered addresses). Slot 1 is cut off
// from the host for 5.5 s: the host removes it, slot 1 claims, the host merges into slot 1's room (it removed slot 1 itself, so it
// vouches); slots 2 and 3 then make a room of their own, which must merge into slot 1's (it was "unknown" to them: a split for good).
static void TestSplitHealRosterRemoval() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(CutFilter, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 1001);
    all[0]->uiStart();
    Step(all, 500);
    Xo x(all);
    g_cutA = 1; g_cutB = 2;
    XoStep(x, all, 5500, {0});   // (the host's events wait for slot 1's acknowledgement: it is removed when it claims)
    g_cutA = 0;
    for (int t = 0; t < 40000; t += 100) {
        for (MpCore* c : all) {
            if (Phase(c) != MP_UI_IDLE) continue;
            const int cnt = c->uiLocalScan(MP_CONN_LOOPBACK);
            for (int k = 0; k < cnt; k++) {
                MpLocalRoom room;
                if (c->uiLocalGet(k, &room) && room.compatible && c->uiJoinLocal(MP_CONN_LOOPBACK, k)) break;
            }
        }
        Step(all, 100);
        int hosts = 0, act = 0;
        bool agree = true;
        for (MpCore* c : all) {
            hosts += c->isHost() ? 1 : 0;
            act += c->active() ? 1 : 0;
            for (int k = 0; k < 4; k++) agree = agree && c->slotPresent(k);
        }
        if (hosts == 1 && act == 4 && agree) break;
    }
    Step(all, 2000);
    int hosts = 0, act = 0;
    for (MpCore* c : all) { hosts += c->isHost() ? 1 : 0; act += c->active() ? 1 : 0; }
    if (hosts != 1 || act != 4) fprintf(stderr, "  split heal: hosts=%d active=%d merge refusals=%d\n", hosts, act, LogCount("from an unknown address"));
    CHECK(hosts == 1 && act == 4);
    for (MpCore* a : all) {
        bool same = true;
        for (int s = 0; s < 4; s++) same = same && a->slotPresent(s) == all[0]->slotPresent(s);
        if (!same) fprintf(stderr, "  split heal: slot %d sees %d%d%d%d\n", a->localSlot(), a->slotPresent(0), a->slotPresent(1), a->slotPresent(2), a->slotPresent(3));
        CHECK(same);
    }
    hub.setFilter(0, 0);
    Free(all);
    printf("split heal: a member a guest saw removed in a ROSTER vouches for a merge when that guest hosts: ok\n");
}

// corefix 5: host usurping. A guest accepts a host claim only for the next epoch, from the successor in line (or, once the ones before
// it had their turn, the next one), at its roster address. Claims from a present non-successor, with an epoch jump, or from another
// address are refused; the legitimate claim is accepted.
static uint32_t g_sidTap = 0;
static void SidTap(void*, const MpAddr&, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl = 0;
    const uint8_t* w = g_sidTap ? nullptr : FindPacket(d, len, MP_MSG_WELCOME, &pl);
    if (w && pl > 17) memcpy(&g_sidTap, w + 13, 4);
}
// core2: sealer = the member whose claim it is (sealed with its key, from its address); null = a plain claim (an outsider's: dropped)
static void InjectClaim(MpLoopHub& hub, MpCore* to, uint32_t fromEp, int slot, uint16_t epoch, uint16_t seq, MpCore* sealer = nullptr) {
    uint8_t body[25];
    MpWriter w(body, sizeof body);
    w.u16(epoch); w.u8((uint8_t)slot); w.u16(0); w.u32(g_sidTap);
    for (int i = 0; i < MP_MAX_PLAYERS; i++) w.u16(0);
    for (int i = 0; i < MP_MAX_PLAYERS; i++) w.u16(0);   // (netcore: per-origin generations, u16 since core2)
    std::vector<uint8_t> pk;
    WritePacket(pk, MP_MSG_HOST_CHANGE, (uint8_t)slot, MP_HDR_RELIABLE, seq, body, (uint16_t)w.n);
    if (sealer) InjectAs(hub, sealer, to, pk.data(), pk.size());
    else hub.inject(MpAddr{fromEp, MP_SESSION_PORT}, to->debugLocalAddr(), pk.data(), pk.size());
}
static void TestHostUsurping() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    g_sidTap = 0;
    hub.setTap(SidTap, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 351);   // host 0 (ep 1), guests 1..3 (ep 2..4)
    all[0]->uiStart();
    Step(all, 500);
    CHECK(g_sidTap != 0);
    MpCore* g3 = all[3];
    std::vector<MpCore*> only = {g3};
    Step(only, 3000);   // the host (and everyone else) is silent for 3 s: "gone" for a claim, but the 5 s of its own timer are not over
    CHECK(g3->hostSlot() == 0 && g3->hostEpoch() == 0);
    InjectClaim(hub, g3, 3, 2, 1, 100, all[2]);    // slot 2 is not the successor (slot 1 is in line before it)
    Step(only, 50);
    CHECK(g3->hostSlot() == 0 && g3->hostEpoch() == 0);
    InjectClaim(hub, g3, 2, 1, 5, 101, all[1]);    // the successor, but an epoch jump
    Step(only, 50);
    CHECK(g3->hostSlot() == 0 && g3->hostEpoch() == 0);
    InjectClaim(hub, g3, 77, 1, 1, 102);   // the successor's claim from another address, plain (core2: sealed, the key would prove it)
    Step(only, 50);
    CHECK(g3->hostSlot() == 0 && g3->hostEpoch() == 0);
    InjectClaim(hub, g3, 2, 1, 1, 103, all[1]);    // the legitimate claim
    Step(only, 50);
    CHECK(g3->hostSlot() == 1 && g3->hostEpoch() == 1);
    hub.setTap(0, 0);
    Free(all);
    {   // a present guest cannot take the host role from a host that is running (same epoch, or the next one, long after a migration)
        MpLoopHub hub2;
        hub2.setClock(VNow, 0);
        g_sidTap = 0;
        hub2.setTap(SidTap, 0);
        std::vector<MpCore*> all2 = StartSession(hub2, 4, 361);
        all2[0]->uiStart();
        Step(all2, 500);
        all2[0]->uiLeave();
        for (int t = 0; t < 9000 && !(all2[1]->isHost() && all2[2]->hostSlot() == 1 && all2[3]->hostSlot() == 1); t += 50) Step(all2, 50);
        CHECK(all2[1]->isHost() && all2[3]->hostSlot() == 1 && all2[3]->hostEpoch() == 1);
        Step(all2, 15000);
        InjectClaim(hub2, all2[3], 3, 2, 1, 200, all2[2]);   // a guest claims the epoch the host already has
        InjectClaim(hub2, all2[3], 3, 2, 2, 201, all2[2]);   // ... or the next one, with the host alive
        InjectClaim(hub2, all2[1], 3, 2, 1, 202, all2[2]);   // ... and to the host itself
        Step(all2, 100);
        CHECK(all2[3]->hostSlot() == 1 && all2[3]->hostEpoch() == 1 && all2[1]->isHost() && all2[1]->hostEpoch() == 1);
        hub2.setTap(0, 0);
        Free(all2);
    }
    printf("host usurping: claims from a non-successor, an epoch jump and another address are refused: ok\n");
}


// Reconnect: the link goes (Wi-Fi off / sleep as the transport or the Vita watch reports it): "Reconnecting..." (JOINING +
// NO_WIFI), then back in the same slot; it gives up after 30 s. STATE of an older scene serial is dropped.
// A new host that claimed the room (migration) and then gets a joiner is not the isolated one: it must not end itself MP_CLAIM_MS later
// ("nobody followed the new host") while the joiner is in. Regression (soak): two consoles whose host was gone ping-ponged between
// hosting a room that ended 4 s after the claim and joining the other one's, for ever.
static void TestClaimWithJoiner() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 91);
    all[0]->uiStart();
    Step(all, 300);
    MpCore* d = MakeCore(hub, 4, "Newcomer", 4242);
    all.push_back(d);
    hub.setDown(1, true);   // the host and the other guest vanish silently: only Luigi (slot 1) is left to claim
    hub.setDown(3, true);
    for (int t = 0; t < 9000 && !all[1]->isHost(); t += 50) Step(all, 50);
    CHECK(all[1]->isHost());
    CHECK(ScanJoin(all, d));
    CHECK(WaitPhase(all, d, MP_UI_PLAYING, 3000));
    Step(all, MP_CLAIM_MS + 3000);
    CHECK(all[1]->active() && all[1]->isHost());
    CHECK(d->active() && d->hostSlot() == all[1]->localSlot());
    Free(all);
    printf("claim with a joiner: ok\n");
}

// A retransmitted WELCOME of an earlier join attempt must not be adopted by a guest that has started over (soak, seed 8): the host had
// reset the channel at the second HELLO, the guest stayed pinned to the first WELCOME's channel: heartbeats flowed (the guest looked
// alive) but no ROSTER or event ever arrived again. Same for a stray REJECT(CLOSED) (the host's answer to packets of a session the
// guest had just left): it is not the answer to the new HELLO and must not abort the attempt.
static std::vector<uint8_t> g_welcome1;
static void WelcomeTap(void*, const MpAddr&, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl;
    const uint8_t* w = FindPacket(d, len, MP_MSG_WELCOME, &pl);
    if (to.host == 2 && w && pl > 12 && g_welcome1.empty()) g_welcome1.assign(w, w + pl);
}
static void TestStaleWelcome() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setTap(WelcomeTap, 0);
    g_welcome1.clear();
    std::vector<MpCore*> all;
    all.push_back(MakeCore(hub, 1, "Alex", 61));
    all.push_back(MakeCore(hub, 2, "Luigi", 62));
    MpHostParams hp = HostParams();
    CHECK(all[0]->uiHost(&hp));
    CHECK(WaitPhase(all, all[0], MP_UI_LOBBY, 2000));
    CHECK(ScanJoin(all, all[1]));
    CHECK(WaitPhase(all, all[1], MP_UI_LOBBY, 3000));
    CHECK(!g_welcome1.empty());
    all[1]->uiLeave();
    Step(all, 1500);
    CHECK(Phase(all[1]) == MP_UI_IDLE);
    // the guest starts over; the old WELCOME (a retransmission that was still in the net) arrives first
    CHECK(ScanJoin(all, all[1]));
    CHECK(Phase(all[1]) == MP_UI_CONNECTING);
    all[1]->debugReceive(all[0]->debugLocalAddr(), g_welcome1.data(), g_welcome1.size());
    CHECK(Phase(all[1]) == MP_UI_CONNECTING);
    // a stray REJECT(CLOSED) with sequence 0 (the host's reply to a stray packet) does not end the attempt either
    uint8_t rej[13] = {'G', 'M', MP_PROTO_VERSION, MP_MSG_REJECT, 0, 0, 0, 0, 0, 0, 0, 0, MP_REJECT_CLOSED};
    all[1]->debugReceive(all[0]->debugLocalAddr(), rej, sizeof rej);
    CHECK(Phase(all[1]) == MP_UI_CONNECTING);
    CHECK(WaitPhase(all, all[1], MP_UI_LOBBY, 3000));
    // the channel is the live one: events from the host reach the guest, and the host's later roster changes do too
    MpMsg m;
    while (all[1]->poll(&m)) {}
    uint8_t ev[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    CHECK(all[0]->send(MP_MSG_STAR, ev, 8, true));
    Step(all, 500);
    int got = 0;
    while (all[1]->poll(&m)) if (m.type == MP_MSG_STAR) got++;
    CHECK(got == 1);
    MpCore* third = MakeCore(hub, 3, "Peach", 63);
    all.push_back(third);
    CHECK(ScanJoin(all, third));
    CHECK(WaitPhase(all, third, MP_UI_LOBBY, 3000));
    Step(all, 3000);
    CHECK(all[1]->slotPresent(third->localSlot()));   // the guest heard about the newcomer: its ROSTER channel works
    Free(all);
    printf("stale welcome: ok\n");
}

// Split sessions heal (soak): after a partition some guests migrated while the old host was alive: two rooms of one session. The
// hosts hear each other's beacons; the one that is behind (fewer players, then the earlier epoch, then the higher slot) joins the other.
static int g_partitionA = 0;   // endpoints 1..g_partitionA are on one side; 0 = no partition
static bool PartitionFilter(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    if (!g_partitionA) return true;
    if (to.host == MP_ADDR_BROADCAST) return false;   // (beacons do not cross either)
    return ((int)from.host <= g_partitionA) == ((int)to.host <= g_partitionA);
}
static bool OneSession(std::vector<MpCore*>& cs, int n) {
    int hosts = 0, active = 0;
    uint16_t epoch = 0;
    int hostSlot = -1;
    for (MpCore* c : cs) {
        if (!c->active()) continue;
        active++;
        if (c->isHost()) hosts++;
        if (active == 1) { epoch = c->hostEpoch(); hostSlot = c->hostSlot(); }
        else if (c->hostEpoch() != epoch || c->hostSlot() != hostSlot) return false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            bool any = false;
            for (MpCore* o : cs) if (o->active() && o->slotPresent(s)) any = true;
            if (any && !c->slotPresent(s)) return false;
        }
    }
    return active == n && hosts == 1;
}
static void TestSplitHeal() {
    {   // the old host is cut off for longer than MP_PEER_LOST_MS: its guests migrate, it is left alone in a room of one
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 101);
        all[0]->uiStart();
        Step(all, 500);
        hub.setDown(1, true);
        Step(all, MP_PEER_LOST_MS + 5000);
        CHECK(all[0]->isHost() && all[0]->active());          // alone in its own room
        CHECK(all[1]->isHost() || all[2]->isHost());          // the guests run on under a new host
        hub.setDown(1, false);
        for (int t = 0; t < 40000 && !OneSession(all, 3); t += 100) Step(all, 100);
        CHECK(OneSession(all, 3));
        CHECK(!all[0]->isHost() && all[0]->localSlot() == 0);   // the old host is a guest again, in its old slot
        CHECK(LogCount("merge: a room of this session is ahead") >= 1);
        Free(all);
    }
    {   // 2 + 2: both halves keep a host, same player count; the later epoch wins
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setFilter(PartitionFilter, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 111);
        all[0]->uiStart();
        Step(all, 500);
        g_partitionA = 2;   // {1,2} (the host and Luigi) | {3,4}
        Step(all, MP_PEER_LOST_MS + 8000);
        int hosts = 0;
        for (MpCore* c : all) if (c->active() && c->isHost()) hosts++;
        CHECK(hosts == 2);
        g_partitionA = 0;
        // (a guest left alone by its host ends with HOST_LEFT and a player joins again: done here for every console that is idle)
        for (int t = 0; t < 60000 && !OneSession(all, 4); t += 100) {
            Step(all, 100);
            for (MpCore* c : all) {
                if (Phase(c) != MP_UI_IDLE) continue;
                const int n = c->uiLocalScan(MP_CONN_LOOPBACK);
                for (int i = 0; i < n; i++) { MpLocalRoom r; if (c->uiLocalGet(i, &r) && r.compatible && c->uiJoinLocal(MP_CONN_LOOPBACK, i)) break; }
            }
        }
        CHECK(OneSession(all, 4));
        Free(all);
    }
    printf("split sessions heal: ok\n");
}

// Reconnect: the link goes (Wi-Fi off / sleep as the transport or the Vita watch reports it): "Reconnecting..." (RECONNECTING +
// NO_WIFI, never IDLE meanwhile: the menus must not end the session), then back in the same slot; it gives up after 30 s (IDLE).
// STATE of an older scene serial is dropped.
static void TestReconnect() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 121);
    all[0]->uiStart();
    Step(all, 300);
    hub.setLost(2, true);   // Luigi's link
    Step(all, 200);
    CHECK(Phase(all[1]) == MP_UI_RECONNECTING && Error(all[1]) == MP_ERR_NO_WIFI && !all[1]->active());
    CHECK(all[1]->debugPhase() == MP_UI_JOINING);
    CHECK(all[0]->debugHeld() && all[1]->debugHeld() && all[2]->debugHeld());   // MP3 lodgate: held while joined and, for the one that lost its link, while it rejoins (active() is false)
    // fix2: every frame of the outage reads RECONNECTING (a single IDLE frame would show "session over" and drop the progress)
    for (int i = 0; i < 300; i++) {
        Step(all, 10);
        if (Phase(all[1]) != MP_UI_RECONNECTING) { CHECK(Phase(all[1]) == MP_UI_RECONNECTING); break; }
        if (!all[1]->debugHeld()) { CHECK(all[1]->debugHeld()); break; }   // (held on every frame of the outage: a gate must not open for one frame)
    }
    hub.setLost(2, false);
    CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 10000));
    CHECK(all[1]->debugHeld() && all[1]->active());
    CHECK(all[1]->localSlot() == 1 && Error(all[1]) == MP_ERR_NONE);
    // resume from sleep reported by the watch
    all[2]->debugWatch(MP_WATCH_RESUMED);
    CHECK(Phase(all[2]) == MP_UI_RECONNECTING);
    CHECK(WaitPhase(all, all[2], MP_UI_PLAYING, 10000));
    CHECK(all[2]->localSlot() == 2);
    // the host loses its link: the others migrate, the old host comes back as a guest in its old slot
    hub.setLost(1, true);
    Step(all, 8000);
    CHECK(all[1]->isHost() && all[2]->hostSlot() == 1);
    hub.setLost(1, false);
    CHECK(WaitPhase(all, all[0], MP_UI_PLAYING, 10000));
    CHECK(all[0]->localSlot() == 0 && !all[0]->isHost());
    // gives up after 30 s
    hub.setLost(3, true);
    Step(all, MP_RECONNECT_MS + 3000);
    CHECK(Phase(all[2]) == MP_UI_IDLE && Error(all[2]) == MP_ERR_NO_WIFI);
    CHECK(!all[2]->debugHeld());   // MP3 lodgate: gave up: the session is over, the speed features are back
    CHECK(LogCount("reconnect: gave up") == 1);
    hub.setLost(3, false);
    // scene serial: STATE from an older scene is dropped
    uint8_t pose[68] = {0};
    pose[4] = 5;   // serial 5
    all[1]->setSceneSerial(6);
    CHECK(all[0]->send(MP_MSG_STATE, pose, 68, false));   // (68 bytes: pose + tail)
    pose[4] = 6;
    CHECK(all[0]->send(MP_MSG_STATE, pose, 68, false));
    Step(all, 100);
    MpMsg m;
    int n = 0;
    while (all[1]->poll(&m))
        if (m.type == MP_MSG_STATE) { n++; CHECK(m.data[4] == 6 && m.len == 68); }
    CHECK(n == 1);
    Free(all);
    printf("reconnect: ok\n");
}

// corefix D1 / S5: a guest whose link goes silent for 5..70 s (the console keeps running, the OS reports nothing) is not dropped:
// it shows RECONNECTING, keeps its token, rejoins its own slot, and the others hold the slot for the same window. After 90 s it ends
// with the honest reason "connection lost" (NO_WIFI), never "host left". Reviewer's outage.cpp / outage2.cpp.
static void LinkOutageOnce(int players, int who, int seconds, bool expectBack) {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, players, 300 + (uint32_t)seconds);
    all[0]->uiStart();
    Step(all, 1000);
    hub.setDown((uint32_t)who + 1, true);
    bool sawReconnecting = false;
    for (int t = 0; t < seconds * 1000; t += 50) {
        Step(all, 50);
        if (Phase(all[who]) == MP_UI_RECONNECTING) sawReconnecting = true;
        // never IDLE meanwhile while the window runs (the menus would end the session): checked until 70 s
        if (t < 70000 && t > 17000) CHECK(Phase(all[who]) == MP_UI_RECONNECTING);
    }
    hub.setDown((uint32_t)who + 1, false);
    if (seconds >= 20) CHECK(sawReconnecting);
    if (expectBack) {
        CHECK(WaitPhase(all, all[who], MP_UI_PLAYING, 20000));
        CHECK(all[who]->localSlot() == who && Error(all[who]) == MP_ERR_NONE && !all[who]->isHost());
        Step(all, 2000);
        int present = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) present += all[0]->slotPresent(s) ? 1 : 0;
        CHECK(present == players);
    } else {
        Step(all, 3000);
        CHECK(Phase(all[who]) == MP_UI_IDLE);
        CHECK(Error(all[who]) == MP_ERR_NO_WIFI);   // "connection lost", not "host left"
        CHECK(Error(all[who]) != MP_ERR_HOST_LEFT);
        CHECK(all[0]->isHost() && all[0]->active());
    }
    Free(all);
}
static void TestLinkOutage() {
    const int ok[] = {5, 6, 12, 25, 40, 70};
    for (int s : ok) {
        LinkOutageOnce(2, 1, s, true);   // the lone guest of a 2-player room (nobody else to ask)
        LinkOutageOnce(4, 2, s, true);   // a guest with others around (it claims the host role first, nobody answers)
    }
    LinkOutageOnce(2, 1, 90, false);
    LinkOutageOnce(4, 3, 90, false);
    printf("link outage: ok\n");
}
// S5: a console frozen (not pumped: a suspend nothing reported, a debugger pause) 45..90 s wakes, and rejoins its own slot
static void TestFrozenConsole() {
    const int secs[] = {31, 45, 60, 90};
    for (int s : secs) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 400 + (uint32_t)s);
        all[0]->uiStart();
        Step(all, 1000);
        std::vector<MpCore*> rest = {all[0], all[1], all[3]};   // all[2] is frozen: its clock stops, the others' go on
        Step(rest, s * 1000);
        Step(all, 100);
        Step(all, 45000);   // (it first suspects the others, claims the host role, finds nobody, and rejoins its own slot)
        CHECK(Phase(all[2]) == MP_UI_PLAYING);
        CHECK(all[2]->active() && all[2]->localSlot() == 2 && Error(all[2]) == MP_ERR_NONE);
        Step(all, 3000);
        int present = 0;
        for (int p = 0; p < MP_MAX_PLAYERS; p++) present += all[0]->slotPresent(p) ? 1 : 0;
        if (present != 4) printf("frozen %d s: present=%d slots %d%d%d%d phase2=%d host2=%d\n", s, present, all[0]->slotPresent(0), all[0]->slotPresent(1), all[0]->slotPresent(2), all[0]->slotPresent(3), (int)Phase(all[2]), (int)all[2]->localSlot());
        CHECK(present == 4);
        Free(all);
    }
    printf("frozen console: ok\n");
}

// corefix D2: the automatic bad-message kick is a ban for the session (before: the flooder was back in the same slot within a second).
// corefix S4: a ban is never keyed on the clear-text console id of a HELLO: a hostile joiner that borrows a victim's id, and is kicked,
// must not lock the victim out.
static void TestBadKickBanAndIdSpoof() {
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 700);   // Player (host), Luigi (the flooder), Peach
        MpCore* mal = all[1];
        const int slot = mal->localSlot();
        for (int k = 0; k < 60; k++) all[0]->reportBad(slot, "flood");
        Step(all, 500);
        CHECK(!mal->active() && Phase(mal) == MP_UI_IDLE && Error(mal) == MP_ERR_KICKED);
        CHECK(ScanJoin(all, mal));
        Step(all, 2500);
        CHECK(!mal->active() && Phase(mal) == MP_UI_IDLE && Error(mal) == MP_ERR_KICKED);   // refused: banned for the session
        CHECK(LogCount("hello rejected reason=kicked") >= 1);
        CHECK(!all[0]->slotPresent(slot));
        CHECK(all[2]->active());   // the others are not touched
        Free(all);
    }
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 710);
        MpCore* v = MakeCore(hub, 2, "Victim", 711);
        std::vector<MpCore*> all = {h, v};
        MpHostParams hp = HostParams();
        CHECK(h->uiHost(&hp));
        Step(all, 200);
        CHECK(ScanJoin(all, v));
        CHECK(WaitPhase(all, v, MP_UI_LOBBY, 3000));
        // Mallory joins by hand under the victim's id, then the host kicks Mallory
        RawJoiner j(7, h->debugLocalAddr());
        j.fp = h->fingerprint();
        RawJoiner::Hello hl;
        hl.name = "Mallory";
        hl.consoleId = v->debugWireId();
        CHECK(j.join(hub, all, hl));
        j.done(hub);
        Step(all, 200);
        int ms = -1;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            char nb[MP_NAME_BYTES + 1];
            h->slotName(s, nb);
            if (h->slotPresent(s) && !strcmp(nb, "Mallory")) ms = s;
        }
        CHECK(ms >= 0);
        if (ms >= 0) h->uiKick(ms);
        Step(all, 300);
        // the victim leaves and comes back: not banned
        v->uiLeave();
        Step(all, 500);
        CHECK(ScanJoin(all, v));
        CHECK(WaitPhase(all, v, MP_UI_LOBBY, 5000));
        CHECK(v->active() && Error(v) == MP_ERR_NONE);
        Free(all);
    }
    printf("bad-message ban and id spoof: ok\n");
}

// corefix D3: HELLO2 proof failures are counted per source address: one stranger replaying a wrong-proof HELLO2 (a cookie costs nothing)
// no longer holds the room-wide allowance and keeps honest joins out; the total work of many strangers is still bounded.
static void TestProofFailsPerSource() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 720);
    MpCore* g = MakeCore(hub, 3, "Honest", 721);
    std::vector<MpCore*> all = {h, g};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 300);
    // a stranger: one cookie, one wrong-proof HELLO2, replayed 12 times at once
    RawJoiner j(9, h->debugLocalAddr());
    j.fp = h->fingerprint();
    RawJoiner::Hello hl;
    hl.name = "Eve";
    hl.badProof = true;
    const uint32_t keys0 = h->debugPairKeys();
    CHECK(j.join(hub, all, hl));
    j.done(hub);
    std::vector<uint8_t> bad = j.hello2(hl, 0x4321);
    for (int i = 0; i < 12; i++) hub.inject(j.me, j.host, bad.data(), bad.size());
    Step(all, 5);
    CHECK(h->debugPairKeys() - keys0 <= (uint32_t)MP_PROOF_FAILS_PER_SEC);   // (the stranger's own allowance)
    // an honest console joins right now: it is not held back by the stranger's failures (before: refused until the second was over)
    CHECK(ScanJoin(all, g));
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 700));
    CHECK(g->active() && g->localSlot() == 1);
    // many strangers together: the work stays bounded
    const uint32_t keys1 = h->debugPairKeys();
    std::vector<RawJoiner*> eves;
    for (uint32_t e = 0; e < 12; e++) {
        RawJoiner* x = new RawJoiner(20 + e, h->debugLocalAddr());
        x->fp = h->fingerprint();
        RawJoiner::Hello xh;
        xh.badProof = true;
        CHECK(x->join(hub, all, xh));   // (one failure each)
        x->done(hub);
        std::vector<uint8_t> b = x->hello2(xh, 0x4000 + (uint16_t)e);
        for (int i = 0; i < 10; i++) hub.inject(x->me, x->host, b.data(), b.size());
        eves.push_back(x);
    }
    Step(all, 5);
    CHECK(h->debugPairKeys() - keys1 <= (uint32_t)MP_PROOF_FAILS_ALL_PER_SEC + 2);
    for (RawJoiner* x : eves) delete x;
    Free(all);
    printf("proof failures per source: ok\n");
}

// corefix D4: replayed valid HELLO2s cost no X25519 when the room is full / the member is already in, and the key-exchange work of strangers
// is metered. S3 (the game thread's per-turn snapshotReady read) takes no lock.
struct HelloCap { std::vector<std::vector<uint8_t>> v; uint32_t from; MpAddr fromAddr{0, 0}; };
static void HelloCapTap(void* ctx, const MpAddr& from, const MpAddr&, const void* data, size_t len) {
    HelloCap* h = (HelloCap*)ctx;
    const uint8_t* d = (const uint8_t*)data;
    if (len > 100 && from.host == h->from && d[0] == 'G' && d[1] == 'M' && d[3] == MP_MSG_HELLO && (d[5] & MP_HDR_RELIABLE)) { h->v.push_back(std::vector<uint8_t>(d, d + len)); h->fromAddr = from; }
}
static void TestHelloReplayCost() {
    {   // a full room: a stranger holding a valid HELLO2 replays it 40 times
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 730);
        MpCore* g = MakeCore(hub, 2, "Guest", 731);
        std::vector<MpCore*> all = {h, g};
        MpHostParams hp = HostParams(MP_MODE_TOGETHER, 2);
        CHECK(h->uiHost(&hp));
        Step(all, 200);
        CHECK(ScanJoin(all, g));
        CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
        Step(all, 300);
        RawJoiner j(9, h->debugLocalAddr());
        j.fp = h->fingerprint();
        RawJoiner::Hello hl;
        hl.name = "Eve";
        const uint32_t k0 = h->debugPairKeys();
        CHECK(j.join(hub, all, hl));
        std::vector<uint8_t> ok = j.hello2(hl, 0x4321);
        for (int i = 0; i < 40; i++) hub.inject(j.me, j.host, ok.data(), ok.size());
        Step(all, 20);
        j.done(hub);
        CHECK(h->debugPairKeys() == k0);   // (before: one X25519 for each of the 41)
        CHECK(h->active() && g->active() && h->slotPresent(1));
        Free(all);
    }
    {   // (not a regression of the old behaviour) a member's own HELLO2 replayed from its address is dropped before any key exchange
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 732);
        MpCore* g = MakeCore(hub, 2, "Guest", 733);
        std::vector<MpCore*> all = {h, g};
        MpHostParams hp = HostParams();
        CHECK(h->uiHost(&hp));
        Step(all, 200);
        HelloCap cap;
        cap.from = 2;
        hub.setTap(HelloCapTap, &cap);
        CHECK(ScanJoin(all, g));
        CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
        Step(all, 1500);
        hub.setTap(0, 0);
        CHECK(!cap.v.empty());
        const uint32_t k0 = h->debugPairKeys();
        const int back0 = LogCount("is back, the old session goes");
        const MpAddr ga = cap.fromAddr;
        if (!cap.v.empty())
            for (int i = 0; i < 40; i++) hub.inject(ga, h->debugLocalAddr(), cap.v.back().data(), cap.v.back().size());
        Step(all, 300);
        CHECK(h->debugPairKeys() == k0);
        CHECK(LogCount("is back, the old session goes") == back0);
        CHECK(g->active() && h->slotPresent(1));
        Free(all);
    }
    {   // 40 strangers with valid proofs but a wrong mode (each costs the host a key exchange and takes no slot): metered
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 734);
        std::vector<MpCore*> all = {h};
        MpHostParams hp = HostParams(MP_MODE_TOGETHER, 4);
        CHECK(h->uiHost(&hp));
        Step(all, 200);
        std::vector<RawJoiner*> xs;
        std::vector<std::vector<uint8_t>> hellos;
        for (uint32_t e = 0; e < 40; e++) {   // (cookies first, then every HELLO2 at once: a burst)
            RawJoiner* x = new RawJoiner(30 + e, h->debugLocalAddr());
            x->fp = h->fingerprint();
            RawJoiner::Hello xh;
            xh.mode = MP_MODE_RACE;
            hub.setTap(RawJoiner::Tap, x);
            x->hello1(hub);
            for (int i = 0; i < 20 && !x->haveCookie; i++) Step(all, 5);
            hub.setTap(0, 0);
            xs.push_back(x);
            if (x->haveCookie) hellos.push_back(x->hello2(xh, 0x4000 + (uint16_t)e));   // (the host's cookie answers are rate-limited: a few get none)
            else hellos.push_back(std::vector<uint8_t>());
        }
        const uint32_t k0 = h->debugPairKeys();
        const uint64_t t0 = g_now;
        int sent = 0;
        for (uint32_t e = 0; e < 40; e++)
            if (!hellos[e].empty()) { hub.inject(xs[e]->me, xs[e]->host, hellos[e].data(), hellos[e].size()); sent++; }
        Step(all, 5);
        for (RawJoiner* x : xs) delete x;
        const uint32_t used = h->debugPairKeys() - k0;
        const uint32_t bound = MP_PAIRKEYS_PER_SEC + (uint32_t)((g_now - t0) * MP_PAIRKEYS_PER_SEC / 1000) + 2;   // burst + refill meanwhile
        printf("  strangers: %d valid HELLO2s, %u key exchanges (bound %u)\n", sent, used, bound);
        CHECK(sent >= 25);
        CHECK(used <= bound);
        CHECK(used < (uint32_t)sent);   // metered: not every one cost an X25519
        CHECK(used >= 10);              // (and the first ones still get theirs)
        Free(all);
    }
    printf("hello replay cost: ok\n");
}

// corefix S3: the game thread's per-turn snapshotReady() does not wait for m_mutex. The helper thread holds it through a whole pump (log
// lines go to the SD card, X25519, ...): here a log sink that stalls while the lock is held stands for that.
#include <atomic>
#include <chrono>
#include <thread>
static std::atomic<int> g_s3Armed{0}, g_s3Entered{0};
static void S3Sink(const char* line) {
    LogSink(line);
    if (g_s3Armed.load() && strstr(line, "bad message from slot")) {
        g_s3Entered = 1;
        for (int i = 0; i < 600 && g_s3Armed.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void TestSnapshotReadNoLock() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 740);
    mp_log_set_sink(S3Sink, true);
    g_s3Armed = 1;
    MpCore* h = all[0];
    std::thread th([&] { h->reportBad(1, "stall"); });   // logs (and stalls) with the lock held
    for (int i = 0; i < 2000 && !g_s3Entered.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(g_s3Entered.load() == 1);
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t r = h->snapshotReady();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r == 0);
    CHECK(ms < 150);   // (before: it waited for the stalled lock, about 600 ms)
    g_s3Armed = 0;
    th.join();
    mp_log_set_sink(LogSink, true);
    Free(all);
    printf("snapshotReady without the lock: ok\n");
}

// netopt item 5: heartbeat piggyback. A link where other datagrams leave needs no HEARTBEAT datagram of its own (the ping's clock echo
// rides inside a datagram that leaves anyway, twice a second); a link with nothing else leaving still gets a datagram every
// MP_KEEPAWAKE_MS (the old rate); and a player who stands still (only its STATE flows) is never timed out, with or without loss.
static uint64_t g_hbDg[8];
static void HbTap(void*, const MpAddr& from, const MpAddr&, const void*, size_t) { if (from.host < 8) g_hbDg[from.host]++; }
static void HbPose(MpCore* c, uint8_t n) { uint8_t pb[68]; memset(pb, 0x11, sizeof pb); pb[0] = n; c->send(MP_MSG_STATE, pb, 68, false); }
static void TestHeartbeatPiggyback() {
    const int dropsBefore = LogCount("no acknowledgement") + LogCount("left reason") + LogCount("lost");
    {   // (1) busy link: both consoles send a STATE every 33 ms: about 30 datagrams a second each (was ~40: + 10 heartbeats)
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 811);
        all[0]->uiStart();
        Step(all, 1000);
        hub.setTap(HbTap, 0);
        memset(g_hbDg, 0, sizeof g_hbDg);
        MpMsg m;
        uint8_t n = 0;
        for (int t = 0; t < 20000; t += 33) {
            for (MpCore* c : all) HbPose(c, n++);
            Step(all, 33);
            for (MpCore* c : all) while (c->poll(&m)) {}
        }
        hub.setTap(0, 0);
        const double rate0 = g_hbDg[1] / 20.0, rate1 = g_hbDg[2] / 20.0;
        printf("heartbeat piggyback: busy link: host %.1f, guest %.1f datagrams/s (STATE 30.3/s)\n", rate0, rate1);
        CHECK(rate0 < 33.5 && rate1 < 33.5 && rate0 > 28 && rate1 > 28);
        CHECK(all[0]->slotPresent(1) && all[1]->active());
        CHECK(all[0]->slotPing(1) >= 0 && all[0]->slotPing(1) < 1000);   // (the ping still measures, from the piggybacked echoes)
        CHECK(all[1]->slotPing(0) >= 0 && all[1]->slotPing(0) < 1000);
        Free(all);
    }
    {   // (2) a silent link: the old rate (a datagram every 50 ms), and nobody is dropped over a minute
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 812);
        all[0]->uiStart();
        Step(all, 1000);
        hub.setTap(HbTap, 0);
        memset(g_hbDg, 0, sizeof g_hbDg);
        Step(all, 20000);
        hub.setTap(0, 0);
        const double rate0 = g_hbDg[1] / 20.0, rate1 = g_hbDg[2] / 20.0;
        printf("heartbeat piggyback: silent link: host %.1f, guest %.1f datagrams/s (old rate 20)\n", rate0, rate1);
        CHECK(rate0 > 17 && rate0 < 23 && rate1 > 17 && rate1 < 23);
        Step(all, 40000);
        CHECK(all[0]->slotPresent(1) && all[1]->active());
        Free(all);
    }
    for (int variant = 0; variant < 3; variant++) {   // (3) a still player: only STATE flows, 150 s: 0 both send, 1 only the guest sends, 2 both, 15 % loss
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 820 + (uint32_t)variant);
        all[0]->uiStart();
        Step(all, 1000);
        if (variant == 2) hub.setImpairment(MpLoopImpairment{0.15f, 0.0f, 5, 30, 0}, 821);
        MpMsg m;
        uint8_t n = 0;
        for (int t = 0; t < 150000; t += 33) {
            for (size_t i = 0; i < all.size(); i++)
                if (variant != 1 || i != 0) HbPose(all[i], n++);
            Step(all, 33);
            for (MpCore* c : all) while (c->poll(&m)) {}
            if (!all[0]->slotPresent(1) || !all[0]->slotPresent(2) || !all[1]->active() || !all[2]->active()) break;
        }
        CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2) && all[1]->active() && all[2]->active());
        CHECK(Error(all[1]) == MP_ERR_NONE && Error(all[2]) == MP_ERR_NONE);
        Free(all);
    }
    CHECK(LogCount("no acknowledgement") + LogCount("left reason") + LogCount("lost") == dropsBefore);
    printf("heartbeat piggyback: ok\n");
}

// netopt item 13: pollMany takes up to `max` messages under one lock, in the same order as poll().
static void TestPollMany() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 830);
    all[0]->uiStart();
    Step(all, 500);
    Book b;
    for (int i = 0; i < 40; i++) SendStar(b, all[0]);
    Step(all, 300);
    MpMsg buf[8];
    CHECK(all[1]->pollMany(nullptr, 8) == 0 && all[1]->pollMany(buf, 0) == 0);
    std::vector<uint32_t> got;
    int calls = 0, biggest = 0;
    for (;;) {
        const uint32_t n = all[1]->pollMany(buf, 8);
        if (!n) break;
        calls++;
        CHECK(n <= 8);
        biggest = n > (uint32_t)biggest ? (int)n : biggest;
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i].type != MP_MSG_STAR) continue;
            CHECK(buf[i].len == 8 && buf[i].fromSlot == 0);
            uint32_t v;
            memcpy(&v, buf[i].data, 4);
            got.push_back(v);
        }
    }
    CHECK(got.size() == 40 && InOrder(got) && got[0] == 0);
    CHECK(biggest == 8 && calls >= 5 && calls <= 7);
    CHECK(all[1]->pollMany(buf, 8) == 0);
    MpMsg one;
    CHECK(!all[1]->poll(&one));
    for (int i = 0; i < 3; i++) SendStar(b, all[0]);   // poll() is pollMany(1): still works after
    Step(all, 200);
    int k = 0;
    while (all[1]->poll(&one)) k += one.type == MP_MSG_STAR;
    CHECK(k == 3);
    Free(all);
    printf("pollMany: 40 messages in %d calls of up to 8, in order: ok\n", calls);
}

// netopt item 11: a guest holds one reliable window (99 KB), the successor in line two, a host three. A takeover needs no allocation
// when the successor in line takes over (it holds the spare), a later claimant allocates at that moment, and a heap that has none still
// leaves a takeover that happens (the guest it has no window for rejoins).
static void TestLazyWindows() {
    {   // (1) holdings
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 840);
        all[0]->uiStart();
        Step(all, 3000);
        printf("lazy windows: host %d, guests %d %d %d windows\n", all[0]->debugRelWindows(), all[1]->debugRelWindows(), all[2]->debugRelWindows(), all[3]->debugRelWindows());
        CHECK(all[0]->debugRelWindows() == 3);
        CHECK(all[1]->debugRelWindows() == 2);   // (slot 1: first in line, two guests to serve)
        CHECK(all[2]->debugRelWindows() == 1 && all[3]->debugRelWindows() == 1);
        // a guest that leaves: the successor's duty changes; the spare goes back after 3 s
        hub.setDown(4, true);
        std::vector<MpCore*> rest = {all[0], all[1], all[2]};
        Step(rest, 20000);
        Step(rest, 4000);
        CHECK(all[1]->debugRelWindows() == 1);   // (only one other guest left: the host's window serves the takeover)
        Free(all);
    }
    {   // (2) the successor in line takes over under 10 % loss, then the next one: nothing converges worse than before
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 841);
        all[0]->uiStart();
        Step(all, 3000);
        Book b;
        for (MpCore* c : all) b.slotOf[c] = c->localSlot();
        hub.setImpairment(MpLoopImpairment{0.10f, 0.05f, 10, 100, 0}, 4243);
        std::vector<MpCore*> guests = Survivors(all, {0});
        Traffic(b, all, all, 300);
        hub.setDown(1, true);
        Traffic(b, guests, all, 900);
        hub.setImpairment(MpLoopImpairment{0, 0, 0, 0, 0}, 1);
        Traffic(b, guests, all, 0);
        Step(all, 6000);
        for (MpCore* c : all) Drain(b, c);
        for (MpCore* c : guests) CHECK(c->active() && c->hostSlot() == 1 && c->hostEpoch() == 1);
        CHECK(guests[0]->isHost());
        CHECK(CheckStreams(b, guests, "lazy") == 0);
        hub.setDown(2, true);   // the new host drops too: slot 2 claims (one guest left to serve: the window it holds)
        std::vector<MpCore*> last = Survivors(all, {0, 1});
        Traffic(b, last, all, 900);
        Step(all, 6000);
        for (MpCore* c : all) Drain(b, c);
        for (MpCore* c : last) CHECK(c->active() && c->hostSlot() == 2 && c->hostEpoch() == 2);
        CHECK(CheckStreams(b, last, "lazy2") == 0);
        Free(all);
    }
    {   // (3) the heap has no window at the takeover (the spare could never be had): the takeover happens; the guest left without a window
        //     rejoins once the heap has room again, and the room is whole again
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all;
        const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
        for (int i = 0; i < 4; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], 842 + (uint32_t)i * 7919u));
        MpHostParams hp = HostParams(MP_MODE_TOGETHER);
        CHECK(all[0]->uiHost(&hp));
        CHECK(WaitPhase(all, all[0], MP_UI_LOBBY, 2000));
        MpCore::debugFailRelAlloc(1000000);   // (the host holds its three; no guest can get a second window from now on)
        for (int i = 1; i < 4; i++) {
            CHECK(ScanJoin(all, all[i]));
            CHECK(WaitPhase(all, all[i], MP_UI_LOBBY, 8000));
        }
        all[0]->uiStart();
        Step(all, 3000);
        CHECK(all[1]->debugRelWindows() == 1);
        hub.setDown(1, true);   // (the host's console id is 1 on the hub)
        Step(all, 30000);
        int hosts = 0, alive = 0;
        for (size_t i = 1; i < all.size(); i++) {
            alive += all[i]->active() ? 1 : 0;
            hosts += all[i]->isHost() ? 1 : 0;
        }
        CHECK(alive >= 2);   // (the new host and the guest it has a window for; the third is "reconnecting" until the heap has room)
        printf("lazy windows: takeover with an empty heap: %d host, windows %d %d %d\n", hosts, all[1]->debugRelWindows(), all[2]->debugRelWindows(), all[3]->debugRelWindows());
        CHECK(hosts >= 1);
        MpCore::debugFailRelAlloc(0);
        Step(all, 90000);
        hosts = 0;
        int together = 0;
        for (size_t i = 1; i < all.size(); i++) {
            hosts += all[i]->isHost() ? 1 : 0;
            together += all[i]->active() && all[i]->hostSlot() == all[1]->hostSlot() ? 1 : 0;
        }
        CHECK(hosts == 1 && together == 3);
        Free(all);
    }
    // a stranded successor with the spare dead: a claimant who must allocate at takeover (slot 2 claims first because slot 1 is gone with the host)
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 843);
        all[0]->uiStart();
        Step(all, 3000);
        hub.setDown(1, true);
        hub.setDown(2, true);   // host and the successor in line vanish together: slot 2 claims, serves slot 3 with the window it has
        std::vector<MpCore*> rest = {all[2], all[3]};
        Step(all, 40000);
        CHECK(all[2]->active() && all[3]->active() && all[2]->hostSlot() == all[3]->hostSlot());
        Free(all);
    }
    printf("lazy windows: ok\n");
}

// netopt item 8 (first half): the host does not relay a pose to a console whose last pose carried another scene serial (bytes 4..5), and
// does not send its own either. Anything else is relayed as before: a console it knows nothing of, one that has stopped sending for a
// second (it is loading), a console in the same scene.
static void StatePose(MpCore* c, uint16_t serial, uint8_t n) { uint8_t pb[68]; memset(pb, 0x11, sizeof pb); pb[0] = n; pb[4] = (uint8_t)serial; pb[5] = (uint8_t)(serial >> 8); c->send(MP_MSG_STATE, pb, 68, false); }
static int CountStates(MpCore* c, int fromSlot) {
    MpMsg m;
    int n = 0;
    while (c->poll(&m)) n += m.type == MP_MSG_STATE && m.fromSlot == fromSlot;
    return n;
}
static void TestStateSceneFilter() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 850);
    all[0]->uiStart();
    Step(all, 500);
    for (MpCore* c : all) CountStates(c, 99);
    uint16_t serialv[3] = {5, 5, 9}; const uint16_t* serial = serialv;   // host and guest 1 in scene 5, guest 2 in scene 9
    int got[3][3] = {};
    uint8_t n = 0;
    auto round = [&](int ms, bool silent2) {
        for (int t = 0; t < ms; t += 33) {
            for (int i = 0; i < 3; i++)
                if (!(silent2 && i == 2)) StatePose(all[i], serial[i], n++);
            Step(all, 33);
            for (int i = 0; i < 3; i++) {
                MpMsg m;
                while (all[i]->poll(&m))
                    if (m.type == MP_MSG_STATE && m.fromSlot < 3) got[i][m.fromSlot]++;
            }
        }
    };
    round(3000, false);
    printf("state scene filter: guest 1 got %d from host / %d from guest 2; guest 2 got %d from host / %d from guest 1; host got %d from guest 2; skipped %u\n",
           got[1][0], got[1][2], got[2][0], got[2][1], got[0][2], all[0]->debugStateSkipped());
    CHECK(got[0][1] > 80 && got[0][2] > 80);   // (the host hears every guest)
    CHECK(got[1][0] > 80);                      // same scene: relayed / sent
    CHECK(got[1][2] == 0 || got[1][2] < 5);     // (guest 2's pose reaches guest 1 only before the host has seen both serials)
    CHECK(got[2][0] < 5 && got[2][1] < 5);
    CHECK(all[0]->debugStateSkipped() > 100);
    // guest 2 enters scene 5: everything flows again at once
    memset(got, 0, sizeof got);
    serialv[2] = 5;
    round(2000, false);
    CHECK(got[1][2] > 50 && got[2][0] > 50 && got[2][1] > 50);
    // guest 2 goes to scene 9 and stops sending (a load): after a second the host relays to it again (it cannot tell where it is)
    serialv[2] = 9;
    round(300, false);
    memset(got, 0, sizeof got);
    round(3000, true);
    printf("state scene filter: a console that stopped sending gets poses again: %d from host, %d from guest 1\n", got[2][0], got[2][1]);
    CHECK(got[2][0] > 30 && got[2][1] > 30);
    CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2));
    Free(all);
    printf("state scene filter: ok\n");
}

// fix2: Same Wi-Fi / Nearby rooms are joined by identity (MpLocalRoom::id): a room that expires moves the list's indexes,
// never which room a press joins; a room that is gone is refused (ROOM_NOT_FOUND), never swapped for another.
static void TestJoinByRoomId() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* a = MakeCore(hub, 1, "Alpha", 41);
    MpCore* b = MakeCore(hub, 2, "Beta", 42);
    MpCore* g = MakeCore(hub, 3, "Gamma", 43);
    std::vector<MpCore*> all = {a, b, g};
    MpHostParams hp = HostParams();
    CHECK(a->uiHost(&hp));
    CHECK(b->uiHost(&hp));
    Step(all, 100);
    int n = 0;
    for (int t = 0; t < 3000 && n < 2; t += 50) { n = g->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 2);
    MpLocalRoom r0, r1;
    CHECK(g->uiLocalGet(0, &r0) && g->uiLocalGet(1, &r1));
    CHECK(r0.id != 0 && r1.id != 0 && r0.id != r1.id);
    const bool alphaFirst = strcmp(r0.host, "Alpha") == 0;
    const uint32_t alphaId = alphaFirst ? r0.id : r1.id, betaId = alphaFirst ? r1.id : r0.id;
    // Alpha closes its room: its row expires, Beta's room moves up
    a->uiLeave();
    for (int t = 0; t < MP_LAN_EXPIRE_MS + 3000 && n != 1; t += 50) { n = g->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 1);
    CHECK(!g->uiJoinLocalRoom(MP_CONN_LOOPBACK, alphaId) && Error(g) == MP_ERR_ROOM_NOT_FOUND && Phase(g) == MP_UI_IDLE);
    CHECK(!g->uiJoinLocalRoom(MP_CONN_LOOPBACK, 0));
    CHECK(g->uiJoinLocalRoom(MP_CONN_LOOPBACK, betaId));
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
    CHECK(b->slotPresent(1) && g->localSlot() == 1 && !a->active());
    Free(all);
    printf("join by room id: ok\n");
}

// netx: active discovery. An access point that never forwards broadcasts to wireless clients (the filter drops every broadcast beacon):
// a scanner still lists the room from the host's unicast answers to its probes, at once; a probe flood is answered at a bounded rate.
static int g_probeAnswers = 0;
static bool DropBroadcastBeacons(void*, const MpAddr&, const MpAddr& to, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    if (len == MP_BEACON_BYTES && d[0] == 'S' && d[1] == 'G' && d[2] == 'B') {
        if (to.host == MP_ADDR_BROADCAST) return false;
        g_probeAnswers++;
    }
    return true;
}
static void TestProbeDiscovery() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(DropBroadcastBeacons, 0);
    MpCore* a = MakeCore(hub, 1, "Alpha", 51);
    MpCore* g = MakeCore(hub, 2, "Gamma", 52);
    std::vector<MpCore*> all = {a, g};
    MpHostParams hp = HostParams();
    CHECK(a->uiHost(&hp));
    Step(all, 100);
    int n = 0, waited = 0;
    for (; waited < 3000 && n < 1; waited += 50) { n = g->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 1);
    CHECK(waited <= 200);   // the first probe goes out with the scan, its answer comes straight back
    CHECK(g_probeAnswers >= 1);
    MpLocalRoom r;
    CHECK(g->uiLocalGet(0, &r) && r.compatible && strcmp(r.host, "Alpha") == 0);
    // the room stays listed (probes every second keep it fresh past MP_LAN_EXPIRE_MS)
    for (int t = 0; t < MP_LAN_EXPIRE_MS + 2000; t += 50) { n = g->uiLocalScan(MP_CONN_LOOPBACK); Step(all, 50); }
    CHECK(n == 1);
    CHECK(g->uiJoinLocalRoom(MP_CONN_LOOPBACK, r.id));
    CHECK(WaitPhase(all, g, MP_UI_LOBBY, 3000));
    // a stranger floods probes: the host answers at most MP_PROBE_ANSWERS_PER_SEC a second
    g_probeAnswers = 0;
    uint8_t probe[MP_PROBE_BYTES];
    CHECK(mp_probe_write(probe, sizeof probe, 0) == MP_PROBE_BYTES);
    MpAddr stranger, to;
    stranger.host = 77;
    stranger.port = MP_LAN_PORT;
    to.host = 1;
    to.port = MP_LAN_PORT;
    for (int i = 0; i < 500; i++) {
        hub.inject(stranger, to, probe, sizeof probe);
        if (i % 50 == 49) Step(all, 100);
    }
    CHECK(g_probeAnswers <= MP_PROBE_ANSWERS_PER_SEC * 2);
    CHECK(g_probeAnswers >= MP_PROBE_ANSWERS_PER_SEC);
    Free(all);
    printf("probe discovery: ok\n");
}

// fix2: a kick bans the kicked console (its HELLO's console id) for the session, never a name: kicking one "Player" lets
// another console called "Player" in, and still refuses the kicked one.
static void TestKickByConsole() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 51);
    MpCore* p1 = MakeCore(hub, 2, "Player", 52);
    MpCore* p2 = MakeCore(hub, 3, "Player", 53);
    std::vector<MpCore*> all = {h, p1, p2};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, p1));
    CHECK(WaitPhase(all, p1, MP_UI_LOBBY, 3000));
    const int s1 = p1->localSlot();
    h->uiKick(s1);
    Step(all, 1000);
    CHECK(Phase(p1) == MP_UI_IDLE && Error(p1) == MP_ERR_KICKED);
    // the kicked console stays out (also after the name-based rejoin window)...
    CHECK(ScanJoin(all, p1));
    Step(all, 1000);
    CHECK(Phase(p1) == MP_UI_IDLE && Error(p1) == MP_ERR_KICKED);
    CHECK(LogCount("hello rejected reason=kicked name=Player") >= 1);
    // ...another console with the same name joins
    const int rejected = LogCount("hello rejected reason=kicked");
    CHECK(ScanJoin(all, p2));
    CHECK(WaitPhase(all, p2, MP_UI_LOBBY, 3000));
    CHECK(p2->active() && h->slotPresent(p2->localSlot()) && LogCount("hello rejected reason=kicked") == rejected);
    // a new session of the same host forgets the ban
    p2->uiLeave();
    h->uiLeave();
    Step(all, 1000);
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    CHECK(ScanJoin(all, p1));
    CHECK(WaitPhase(all, p1, MP_UI_LOBBY, 3000));
    Free(all);
    printf("kick by console: ok\n");
}

// corefix 9: the kick list is a ring of 8 (the oldest is dropped and logged), and a kicked address is refused for the session even with
// another console id (the id is per session now, a restarted game must not walk around the ban)
static void TestKickRing() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 61);
    std::vector<MpCore*> all = {h};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    Step(all, 100);
    std::vector<MpCore*> kicked;
    for (int i = 0; i < 10; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "K%d", i);
        MpCore* k = MakeCore(hub, 10 + i, nm, 700 + i);
        all.push_back(k);
        kicked.push_back(k);
        CHECK(ScanJoin(all, k));
        CHECK(WaitPhase(all, k, MP_UI_LOBBY, 3000));
        h->uiKick(k->localSlot());
        Step(all, 600);
        CHECK(Phase(k) == MP_UI_IDLE && Error(k) == MP_ERR_KICKED);
    }
    CHECK(LogCount("kick list full: K0 ") == 1 && LogCount("kick list full: K1 ") == 1 && LogCount("kick list full: K2 ") == 0);
    // the newest is refused by id; K0 and K1 fell off the ring and can join again
    const int before = LogCount("hello rejected reason=kicked");
    CHECK(ScanJoin(all, kicked[9]));
    Step(all, 1500);
    CHECK(LogCount("hello rejected reason=kicked") > before && Phase(kicked[9]) == MP_UI_IDLE);
    CHECK(ScanJoin(all, kicked[0]));
    CHECK(WaitPhase(all, kicked[0], MP_UI_LOBBY, 3000));
    // the same address as K9 with a fresh console (another id) is refused by address
    for (size_t i = 0; i < all.size(); i++) if (all[i] == kicked[9]) { delete all[i]; all.erase(all.begin() + (long)i); break; }
    kicked[9] = 0;   // (its endpoint is free again)
    MpCore* again = MakeCore(hub, 19, "K9", 799);
    all.push_back(again);
    const int b2 = LogCount("(by address)");
    CHECK(ScanJoin(all, again));
    Step(all, 1500);
    CHECK(Phase(again) == MP_UI_IDLE && LogCount("(by address)") > b2);
    Free(all);
    printf("kick ring: ok\n");
}

// fix3: the console id outlives a restart (mp.cfg), and a host that took over at a migration (it adopted its guests without a
// HELLO) still bans by console: the RESUME carries the id. The kicked console comes back from another address with the same id
// (restarted game: a new port / a new mapping) and is refused; another console of the same name joins.
static void TestKickAfterMigration() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 4242);
    all[0]->uiStart();
    Step(all, 300);
    all[0]->uiLeave();
    std::vector<MpCore*> guests = Survivors(all, {0});
    for (int t = 0; t < 5000 && !(guests[0]->isHost() && guests[1]->hostSlot() == guests[0]->localSlot()); t += 5) Step(all, 5);
    CHECK(guests[0]->isHost());
    Step(all, 500);
    const uint32_t id = all[2]->debugConsoleId();       // (the persistent secret's handle: the restarted console has the same key)
    CHECK(id != 0);
    char want[64];
    snprintf(want, sizeof want, "resumed epoch=1 serial=0 console=%08x", (unsigned)all[2]->debugWireId());   // corefix 6: the host sees the session's id
    CHECK(LogCount(want) == 1);
    const int s2 = all[2]->localSlot();
    guests[0]->uiKick(s2);
    Step(all, 1000);
    CHECK(Phase(all[2]) == MP_UI_IDLE && Error(all[2]) == MP_ERR_KICKED);
    // the same console restarted: a new process (new address), the id from its mp.cfg
    MpCore* old2 = all[2];
    all[2] = MakeCore(hub, 9, "Peach", 777);
    SameConsole(all[2], old2);
    delete old2;
    const int rejected = LogCount("hello rejected reason=kicked name=Peach");
    CHECK(ScanJoin(all, all[2]));
    Step(all, 1000);
    CHECK(Phase(all[2]) == MP_UI_IDLE && Error(all[2]) == MP_ERR_KICKED);
    CHECK(LogCount("hello rejected reason=kicked name=Peach") == rejected + 1);
    // another console (another id) with the same name joins
    MpCore* other = MakeCore(hub, 10, "Peach", 778);
    all.push_back(other);
    CHECK(other->debugConsoleId() != id);
    CHECK(ScanJoin(all, other));
    CHECK(WaitPhase(all, other, MP_UI_PLAYING, 3000));
    Free(all);
    printf("kick after migration (console id in RESUME, kept over a restart): ok\n");
}

// fix3: MP_PROTO_VERSION 2 is the 3.0 release's wire: it came with bubble (MpSettings.downed, BUBBLE / RESTART, MP_POSE_BUBBLE)
// and before any v2 build left this machine coop changed EMOTE (fixed 2 B) and added POINTER (0x24), celebrate added TALLY (0x3D).
// The only build that went out is v1 (eboot-21a58de). A v1 console must be refused both ways with "different version":
// (a) its HELLO to a v2 host (header and body proto 1, its v1 fingerprint, no console id tail as v1 sent it) gets REJECT(VERSION)
// with its sequence echoed (the v1 code reads an unreliable REJECT without checking the header's proto: DIFFERENT_VERSION);
// (b) its Same Wi-Fi / Nearby beacon is listed as another version (the list says so; joining is refused up front);
// (c) a v2 guest that reached a v1 host (online: no beacon, the HELLO is broadcast) reads its REJECT (header proto 1).
static uint32_t FingerprintV(uint16_t proto, uint32_t dataHash) {   // core's FingerprintOf with another protocol version
    uint8_t buf[64];
    MpWriter w(buf, sizeof buf);
    w.bytes("SMGMP/1", 7);
    uint32_t crc = mp_crc32(0, buf, w.n);
    crc = mp_crc32(crc, MP_BUILD_ID, strlen(MP_BUILD_ID));
    MpWriter t(buf, sizeof buf);
    t.u16(proto);
    t.u32(dataHash);
    return mp_crc32(crc, buf, t.n);
}
static uint16_t g_helloSeq = 0;
static bool g_dropHello = false;
static uint8_t g_helloNonce[8];
static bool DropHello(void*, const MpAddr&, const MpAddr&, const void* data, size_t len) {
    const uint8_t* d = (const uint8_t*)data;
    size_t pl;
    const uint8_t* h = g_dropHello ? FindPacket(d, len, MP_MSG_HELLO, &pl) : nullptr;
    if (!h) return true;
    g_helloSeq = (uint16_t)(h[6] | (h[7] << 8));
    if (pl >= MP_HEADER_BYTES + 46 && !(h[5] & MP_HDR_RELIABLE)) memcpy(g_helloNonce, h + MP_HEADER_BYTES + 38, 8);   // core2: HELLO1's nonce
    if (!(h[5] & MP_HDR_RELIABLE)) g_helloSeq = 1;
    return false;
}
static void TestVersionRefusal() {
    CHECK(MP_PROTO_VERSION == 5);   // 5: starfix (STAR 12 bytes, STARV 0x3F); 4 was worldsync (OBJMAP 0x46, the REGCRC flag byte)
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpCore* h = MakeCore(hub, 1, "Host", 61);
    std::vector<MpCore*> all = {h};
    MpHostParams hp = HostParams();
    CHECK(h->uiHost(&hp));
    CHECK(WaitPhase(all, h, MP_UI_LOBBY, 2000));
    CHECK(h->fingerprint() == FingerprintV(MP_PROTO_VERSION, 0) && FingerprintV(1, 0) != h->fingerprint());
    // (a)
    MpLoopSocket* sock = hub.open(5, MP_SESSION_PORT);
    CHECK(sock != nullptr);
    const int before = LogCount("hello rejected reason=version");
    for (int variant = 0; variant < 2 && sock; variant++) {   // its own v1 fingerprint; and (a v1 build of other data) ours
        uint8_t d[64];
        MpWriter w(d, sizeof d);
        w.u8('G'); w.u8('M'); w.u8(1); w.u8(MP_MSG_HELLO); w.u8(0xFF); w.u8(MP_HDR_RELIABLE); w.u16(0x1234); w.u16(0); w.u16(0);
        w.u32(variant == 0 ? FingerprintV(1, 0) : h->fingerprint()); w.u16(1);
        uint8_t nm[MP_NAME_BYTES] = {'O', 'l', 'd'};
        w.bytes(nm, sizeof nm); w.u8(0); w.u8(0); w.u32(0);
        hub.inject(MpAddr{5, MP_SESSION_PORT}, h->debugLocalAddr(), d, w.n);
        int reason = -1, rseq = -1, rproto = -1;
        for (int t = 0; t < 500 && reason < 0; t += 10) {
            Step(all, 10);
            MpAddr from;
            uint8_t buf[1200];
            int r;
            while ((r = sock->recv(&from, buf, sizeof buf)) > 0)
                if (r == MP_HEADER_BYTES + 1 && buf[3] == MP_MSG_REJECT) reason = buf[12], rseq = buf[6] | (buf[7] << 8), rproto = buf[2];
        }
        CHECK(reason == MP_REJECT_VERSION && rseq == 0x1234 && rproto == MP_PROTO_VERSION);
    }
    CHECK(LogCount("hello rejected reason=version") == before + 2);
    for (int s = 1; s < MP_MAX_PLAYERS; s++) CHECK(!h->slotPresent(s));
    if (sock) hub.closeSocket(sock);
    // (b)
    {
        MpLanScan scan;
        MpBeacon b;
        memset(&b, 0, sizeof b);
        b.proto = 1; b.mode = MP_MODE_TOGETHER; b.players = 1; b.maxPlayers = 4; b.sessionId = 77; b.fingerprint = FingerprintV(1, 0);
        snprintf(b.name, sizeof b.name, "Old");
        uint8_t raw[MP_BEACON_BYTES];
        CHECK(mp_beacon_write(raw, sizeof raw, b) == MP_BEACON_BYTES);
        MpBeacon back;
        CHECK(mp_beacon_read(raw, sizeof raw, &back) && back.proto == 1);   // (listed: still a valid beacon)
        CHECK(scan.add(MpAddr{6, MP_LAN_PORT}, back, 0, h->fingerprint()));
        MpLocalRoom room;
        CHECK(scan.count() == 1 && scan.get(0, &room, 0, 0) && !room.compatible);
    }
    // (c)
    MpCore* g = MakeCore(hub, 2, "Guest", 62);
    all.push_back(g);
    g_dropHello = true;
    g_helloSeq = 0;
    hub.setFilter(DropHello, 0);
    CHECK(ScanJoin(all, g));
    Step(all, 200);
    CHECK(Phase(g) == MP_UI_CONNECTING && g_helloSeq != 0);
    // core2: a refusal must echo the joiner's nonce (from its first HELLO): one without it (a spoofer off the path) is ignored
    uint8_t rej[MP_HEADER_BYTES + 1] = {'G', 'M', 1, MP_MSG_REJECT, 0, 0, 0, 0, 0, 0, 0, 0, MP_REJECT_VERSION};
    hub.inject(h->debugLocalAddr(), g->debugLocalAddr(), rej, sizeof rej);
    Step(all, 100);
    CHECK(Phase(g) == MP_UI_CONNECTING);
    uint8_t rej2[MP_HEADER_BYTES + 9] = {'G', 'M', MP_PROTO_VERSION, MP_MSG_REJECT, 0, 0, 0, 0, 0, 0, 0, 0, MP_REJECT_VERSION};
    memcpy(rej2 + MP_HEADER_BYTES + 1, g_helloNonce, 8);
    hub.inject(h->debugLocalAddr(), g->debugLocalAddr(), rej2, sizeof rej2);
    Step(all, 200);
    CHECK(Phase(g) == MP_UI_IDLE && Error(g) == MP_ERR_DIFFERENT_VERSION);
    hub.setFilter(0, 0);
    g_dropHello = false;
    Free(all);
    printf("protocol version 3: a v1 console is refused both ways (different version): ok\n");
}

// fix3: the connector reports a drop from the room (online: the service's 403 not_member after a minute of silence, a Vita asleep
// or a long stall): the core rejoins the same session at once (as after a sleep) instead of ending it or sitting in a session
// nobody else is in; when the room is gone by then (the rejoin's join finds none), the session ends at once with "The host
// closed the session.", not 30 s of "Reconnecting..." and a Wi-Fi message.
static void TestDroppedFromRoom() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 515);
    all[0]->uiStart();
    Step(all, 300);
    MpCore* g = all[2];
    const int slot = g->localSlot();
    const int before = LogCount("dropped from the room (loopback): rejoining");
    hub.failConnector(3, MP_ERR_HOST_LEFT, true);
    Step(all, 50);
    CHECK(LogCount("dropped from the room (loopback): rejoining") == before + 1);
    CHECK(Phase(g) == MP_UI_RECONNECTING && !g->active());
    CHECK(WaitPhase(all, g, MP_UI_PLAYING, 8000));
    CHECK(g->active() && g->localSlot() == slot && Error(g) == MP_ERR_NONE);
    CHECK(all[0]->slotPresent(slot) && all[1]->slotPresent(slot));
    // dropped again, and the room is gone by the time the rejoin asks (its join is refused: room_not_found)
    hub.failConnector(3, MP_ERR_HOST_LEFT, true);
    hub.failConnector(3, MP_ERR_ROOM_NOT_FOUND, false);
    Step(all, 3000);
    CHECK(Phase(g) == MP_UI_IDLE && Error(g) == MP_ERR_HOST_LEFT && !g->active());
    // a failure that is not a drop still ends the session as before (no rejoin)
    CHECK(ScanJoin(all, g));
    CHECK(WaitPhase(all, g, MP_UI_PLAYING, 3000));
    hub.failConnector(3, MP_ERR_SERVICE_DOWN, false);
    Step(all, 1000);
    CHECK(Phase(g) == MP_UI_IDLE && Error(g) == MP_ERR_SERVICE_DOWN);
    Free(all);
    printf("dropped from the room: rejoined at once; room gone: ended at once (host left): ok\n");
}

// ---------------------------------------------------------------------------------------------------------------------------
// 9. soak: 4 consoles join / leave / vanish / rejoin at random for 10k turns (16 ms each) with mild impairment
static bool LooksUnique(std::vector<MpCore*>& cs);
static bool LooksAgree(std::vector<MpCore*>& cs);
struct SoakNode {
    MpCore* c;
    uint32_t ep;
    uint32_t run;        // bumps on every (re)join
    uint32_t counter;    // events sent in this run
    uint64_t downUntil;
};
static void TestSoak() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    MpLoopImpairment imp = {0.05f, 0.02f, 5, 40, 0};
    hub.setImpairment(imp, 555 + SeedAdd());
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    std::vector<SoakNode> nodes(4);
    std::vector<MpCore*> all;
    for (int i = 0; i < 4; i++) {
        nodes[i].c = MakeCore(hub, (uint32_t)(i + 1), names[i], 1000u + (uint32_t)i);
        nodes[i].ep = (uint32_t)(i + 1);
        nodes[i].run = 0;
        nodes[i].counter = 0;
        nodes[i].downUntil = 0;
        all.push_back(nodes[i].c);
    }
    // (receiver node, receiver run, origin node, origin run) -> last counter
    std::map<std::tuple<int, uint32_t, int, uint32_t>, uint32_t> last;
    uint32_t rng = 0xC0FFEE ^ (SeedAdd() * 2654435761u);
    auto R = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    int dups = 0, gaps = 0, joins = 0, leaves = 0, vanishes = 0, migrations = 0, delivered = 0;
    std::vector<uint32_t> wasActive(4, 0);
    const int kTurns = 10000;
    for (int turn = 0; turn < kTurns; turn++) {
        const bool settling = turn > kTurns - 1500;
        for (int i = 0; i < 4; i++) {
            SoakNode& n = nodes[i];
            if (n.downUntil && g_now >= n.downUntil) {
                hub.setDown(n.ep, false);
                n.downUntil = 0;
            }
            if (n.downUntil) continue;
            const MpUiPhase ph = Phase(n.c);
            if (n.c->active()) {
                if (!wasActive[i]) { n.run++; n.counter = 0; wasActive[i] = 1; joins++; }
                if (R() % 4 == 0) {
                    uint8_t p[16];
                    p[0] = (uint8_t)i;
                    memcpy(p + 1, &n.run, 4);
                    memcpy(p + 5, &n.counter, 4);
                    if (n.c->send(MP_MSG_KILL, p, 9, true)) n.counter++;
                }
                if (!settling) {
                    const uint32_t r = R() % 10000;
                    if (r < 8) { n.c->uiLeave(); leaves++; }
                    else if (r < 12) { hub.setDown(n.ep, true); n.downUntil = g_now + 2000 + R() % 15000; vanishes++; }
                }
            } else {
                if (wasActive[i]) wasActive[i] = 0;
                if (ph == MP_UI_IDLE) {
                    // join a room if one is visible, else host one (node 0 hosts first; others wait a little)
                    const int cnt = n.c->uiLocalScan(MP_CONN_LOOPBACK);
                    bool joined = false;
                    for (int k = 0; k < cnt && !joined; k++) {
                        MpLocalRoom room;
                        if (n.c->uiLocalGet(k, &room) && room.compatible && room.players < room.maxPlayers)
                            joined = n.c->uiJoinLocal(MP_CONN_LOOPBACK, k);
                    }
                    if (!joined && cnt == 0 && R() % 400 == 0) {
                        MpHostParams hp = HostParams();
                        n.c->uiHost(&hp);
                    }
                }
            }
        }
        Step(all, 16, 8);
        for (int i = 0; i < 4; i++) {
            // netcore: a run is a MEMBERSHIP: it also starts over when the console is welcomed again without going idle (re-admitted
            // after a resync, a split heal); and it starts before the messages of the same pump are looked at
            bool bumped = false;
            if (nodes[i].c->active() && !wasActive[i]) { nodes[i].run++; nodes[i].counter = 0; wasActive[i] = 1; joins++; bumped = true; }
            MpCoreEvent ev;
            while (nodes[i].c->pollEvent(&ev)) {
                if (ev.kind == MP_EV_BECAME_HOST) migrations++;
                if (ev.kind == MP_EV_JOINED && ev.slot == nodes[i].c->localSlot() && !nodes[i].c->isHost() && !bumped) {
                    nodes[i].run++;
                    nodes[i].counter = 0;
                    joins++;
                }
            }
            MpMsg m;
            while (nodes[i].c->poll(&m)) {
                if (m.type != MP_MSG_KILL || m.len != 9) continue;
                uint32_t orun, ocnt;
                memcpy(&orun, m.data + 1, 4);
                memcpy(&ocnt, m.data + 5, 4);
                const int on = m.data[0];
                auto key = std::make_tuple(i, nodes[i].run, on, orun);
                auto it = last.find(key);
                delivered++;
                if (it != last.end()) {
                    if (ocnt <= it->second) {
                        dups++;
                        if (getenv("MP_DUPDEBUG") && dups <= 3) {   // netpack: where a duplicate game event came from
                            fprintf(stderr, "DUP #%d t=%llu receiver=%d(run %u) origin=%d(run %u) counter=%u last=%u; log tail:\n", dups, (unsigned long long)g_now, i,
                                    (unsigned)nodes[i].run, on, (unsigned)orun, (unsigned)ocnt, (unsigned)it->second);
                            for (size_t k = g_log.size() > 40 ? g_log.size() - 40 : 0; k < g_log.size(); k++) fprintf(stderr, "    %s\n", g_log[k].c_str());
                        }
                    }
                    else if (ocnt != it->second + 1) gaps++;
                }
                if (it == last.end() || ocnt > it->second) last[key] = ocnt;
            }
        }
    }
    hub.setImpairment(MpLoopImpairment{0, 0, 0, 0, 0}, 1);
    for (int i = 0; i < 4; i++) { hub.setDown(nodes[i].ep, false); nodes[i].downUntil = 0; }
    // settle: everyone ends in one session
    for (int t = 0; t < 40000; t += 50) {
        int hostsUp = 0;
        for (auto& n : nodes) if (n.c->isHost()) hostsUp++;
        // netcore: two rooms of DIFFERENT sessions are legitimate (two consoles hosted while neither saw a room): as a player would, the
        // host of the smaller one leaves and joins the other. Two hosts of ONE session (a split) must heal by themselves.
        if (hostsUp > 1 && t % 1000 == 0) {
            int big = -1, bigPlayers = 0;
            for (int i = 0; i < 4; i++) {
                MpUiStatus st;
                nodes[i].c->uiStatus(&st);
                if (nodes[i].c->isHost() && (big < 0 || st.players > bigPlayers)) { big = i; bigPlayers = st.players; }
            }
            for (int i = 0; i < 4; i++)
                if (i != big && nodes[i].c->isHost() && nodes[i].c->debugSessionId() != nodes[big].c->debugSessionId()) nodes[i].c->uiLeave();
        }
        for (auto& n : nodes) {
            if (Phase(n.c) != MP_UI_IDLE) continue;
            const int cnt = n.c->uiLocalScan(MP_CONN_LOOPBACK);
            for (int k = 0; k < cnt; k++) {
                MpLocalRoom room;
                if (n.c->uiLocalGet(k, &room) && room.compatible && n.c->uiJoinLocal(MP_CONN_LOOPBACK, k)) break;
            }
            if (cnt == 0 && hostsUp == 0 && &n == &nodes[0]) { MpHostParams hp = HostParams(); n.c->uiHost(&hp); }
        }
        Step(all, 50);
        int act = 0, hosts1 = 0;
        for (auto& n : nodes) { if (n.c->active()) act++; if (n.c->isHost()) hosts1++; }
        if (act == 4 && hosts1 == 1 && t > 5000) break;
    }
    int hosts = 0, act = 0;
    for (auto& n : nodes) { if (n.c->isHost()) hosts++; if (n.c->active()) act++; }
    CHECK(act == 4 && hosts == 1);
    for (auto& a : nodes)
        for (auto& b2 : nodes)
            for (int s = 0; s < 4; s++) {
                CHECK(a.c->slotPresent(s) == b2.c->slotPresent(s));
                char n1[17], n2[17];
                a.c->slotName(s, n1);
                b2.c->slotName(s, n2);
                CHECK(!strcmp(n1, n2));
            }
    CHECK(dups == 0);
    CHECK(gaps == 0);   // netcore: exactly once AND no holes within a membership (DESIGN.md section 3); soak_exact runs the seed matrix
    CHECK(LooksUnique(all) && LooksAgree(all));
    printf("soak 10k turns: joins=%d leaves=%d vanishes=%d migrations=%d events=%d dups=%d gaps=%d hosts=%d active=%d\n", joins,
           leaves, vanishes, migrations, delivered, dups, gaps, hosts, act);
    Free(all);
}

// 11. four unique characters: the host arbitrates at join, on change requests, after a migration and on a rejoin
static bool LooksUnique(std::vector<MpCore*>& cs) {
    for (MpCore* c : cs) {
        if (!c->active()) continue;
        int seen = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (!c->slotPresent(s)) continue;
            const int bit = 1 << c->slotLook(s);
            if (seen & bit) return false;
            seen |= bit;
        }
    }
    return true;
}
static bool LooksAgree(std::vector<MpCore*>& cs) {
    for (MpCore* a : cs)
        for (MpCore* b : cs)
            for (int s = 0; s < MP_MAX_PLAYERS; s++)
                if (a->active() && b->active() && a->slotPresent(s) && a->slotLook(s) != b->slotLook(s)) return false;
    return true;
}
static void TestLooks() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 4, 131);   // everybody wishes Mario (MakeCore)
    Step(all, 500);
    CHECK(all[0]->slotLook(0) == MP_LOOK_MARIO);
    CHECK(LooksUnique(all) && LooksAgree(all));
    CHECK(all[1]->slotLook(1) == MP_LOOK_LUIGI && all[2]->slotLook(2) == MP_LOOK_TOAD && all[3]->slotLook(3) == MP_LOOK_COSMIC);
    // a change to a taken look is refused, to a free one (after Toad leaves) accepted; everybody sees it
    all[3]->setProfile("Toad", MP_LOOK_LUIGI);
    Step(all, 500);
    CHECK(all[3]->slotLook(3) == MP_LOOK_COSMIC && LooksUnique(all));
    CHECK(LogCount("look change refused slot=3 look=1") == 1);
    all[2]->uiLeave();
    Step(all, 500);
    std::vector<MpCore*> rest = Survivors(all, {2});
    all[3]->setProfile("Toad", MP_LOOK_TOAD);
    Step(all, 500);
    CHECK(all[3]->slotLook(3) == MP_LOOK_TOAD && all[0]->slotLook(3) == MP_LOOK_TOAD && all[1]->slotLook(3) == MP_LOOK_TOAD);
    CHECK(LooksUnique(rest) && LooksAgree(rest));
    // the host changes its own
    all[0]->setProfile("Player", MP_LOOK_COSMIC);
    Step(all, 500);
    CHECK(all[1]->slotLook(0) == MP_LOOK_COSMIC && LooksUnique(rest) && LooksAgree(rest));
    // a puppet LOOK with another console's character is corrected by the host
    uint8_t lk[2] = {MP_LOOK_LUIGI, 0};
    CHECK(all[3]->send(MP_MSG_LOOK, lk, 2, true));
    Step(all, 300);
    MpMsg m;
    bool fixedSeen = false;
    while (all[1]->poll(&m))
        if (m.type == MP_MSG_LOOK && m.fromSlot == 3) fixedSeen = m.data[0] == MP_LOOK_TOAD;
    CHECK(fixedSeen);
    // TALLY (the team star's counters): a guest's message reaches the host and the other guests; too long is refused
    {
        const uint8_t tally[8] = {7, 0, 12, 0, 8, 0, 2, 1};   // serial 7, 12 star bits, 8 coins, helped 2, near
        CHECK(all[3]->send(MP_MSG_TALLY, tally, 8, true));
        const uint8_t longer[17] = {0};
        CHECK(!all[3]->send(MP_MSG_TALLY, longer, 17, true));
        Step(all, 300);
        for (int who : {0, 1}) {
            bool seen = false;
            while (all[who]->poll(&m))
                if (m.type == MP_MSG_TALLY && m.fromSlot == 3 && m.len == 8 && m.data[2] == 12) seen = true;
            CHECK(seen);
        }
    }
    // Peach comes back wishing Mario: Mario is free now (the host moved to Cosmic)
    CHECK(ScanJoin(all, all[2]));
    CHECK(WaitPhase(all, all[2], MP_UI_LOBBY, 3000));
    Step(all, 500);
    CHECK(all[2]->slotLook(2) == MP_LOOK_MARIO && LooksUnique(all) && LooksAgree(all));
    // after a host migration the looks stay; the old host rejoins and keeps its look when it is still free
    const MpLook hostLook = all[0]->slotLook(0);
    hub.setDown(1, true);
    Step(all, 9000);
    hub.setDown(1, false);
    std::vector<MpCore*> g = Survivors(all, {0});
    CHECK(g[0]->isHost() && LooksUnique(g) && LooksAgree(g));
    all[0]->uiLeave();
    all[0]->setProfile("Player", MP_LOOK_MARIO);   // (wishes Mario, but keeps Cosmic: a rejoin keeps its old look if free)
    CHECK(ScanJoin(all, all[0]));
    CHECK(WaitPhase(all, all[0], MP_UI_LOBBY, 3000));
    Step(all, 500);
    CHECK(all[0]->slotLook(0) == hostLook && LooksUnique(all) && LooksAgree(all));
    Free(all);
    printf("unique characters: ok\n");
}

// 10. mp.cfg (atomic write, .bak recovery) and name cleaning
static void TestConfig() {
    char dir[] = "/tmp/mp_cfg_testXXXXXX";
    CHECK(mkdtemp(dir) != 0);
    char sub[300];
    snprintf(sub, sizeof sub, "%s/mp", dir);
    mp_config_set_dir(sub);
    MpConfig c = *mp_config();
    snprintf(c.name, sizeof c.name, "Rosa");   // (menus3: ingame's rule: a saved "Player" loads as the character's name, so the round trip uses another name)
    c.look = MP_LOOK_LUIGI;
    c.nameTyped = 0;   // (13 Nov, default name: an untyped name is replaced by the character's, and follows the look)
    mp_config_set(&c);
    CHECK(!strcmp(mp_config()->name, "Luigi") && mp_config()->nameTyped == 0);
    c.nameTyped = 1;   // (13 Nov, default name: a typed name is kept; with nameTyped 0 Clamp makes the name the character's, so "Rosa" would not survive)
    c.raceWaitSec = 30;
    mp_config_set(&c);
    CHECK(mp_config_save());
    CHECK(mp_config_save());   // (second save: the first becomes .bak)
    MpConfig d = c;
    snprintf(d.name, sizeof d.name, "Other");
    mp_config_set(&d);
    CHECK(mp_config_load());
    CHECK(!strcmp(mp_config()->name, "Rosa") && mp_config()->look == MP_LOOK_LUIGI && mp_config()->raceWaitSec == 30);
    char path[400];
    snprintf(path, sizeof path, "%s/mp.cfg", sub);
    FILE* f = fopen(path, "w");
    fputs("garbage", f);
    fclose(f);
    CHECK(mp_config_load());   // recovered from .bak
    CHECK(!strcmp(mp_config()->name, "Rosa"));
    // keepsafe: a damaged mp.cfg makes the run read only (tested with keepsafe's tests); the rest of this test needs a writable file:
    // the damaged one goes (as if the player deleted it), the .bak is read again
    remove(path);
    {
        char unr[400];
        snprintf(unr, sizeof unr, "%s/mp.cfg.unreadable", sub);
        remove(unr);
    }
    CHECK(mp_config_load() && !strcmp(mp_config()->name, "Rosa"));
    // corefix 6: the console key (128-bit secret): none until asked for, then made once, saved at once, read back after a restart, kept by
    // later saves; an old 32-bit console_id line is ignored; "stun" defaults to on and is kept
    {
        const uint8_t zero[16] = {0};
        CHECK(!memcmp(mp_config()->consoleKey, zero, 16) && mp_config()->stun == 1);
        uint8_t key[16], again[16];
        mp_config_console_key(key);
        mp_config_console_key(again);
        CHECK(memcmp(key, zero, 16) != 0 && !memcmp(key, again, 16));
        MpConfig e = *mp_config();
        memset(e.consoleKey, 0, 16);
        mp_config_set(&e);   // (as a fresh process would start)
        CHECK(mp_config_load() && !memcmp(mp_config()->consoleKey, key, 16));
        e = *mp_config();
        e.look = MP_LOOK_MARIO;
        e.stun = 0;
        mp_config_set(&e);
        CHECK(mp_config_save() && mp_config_load() && !memcmp(mp_config()->consoleKey, key, 16) && mp_config()->stun == 0);
        char line[96];
        bool found = false, legacy = false;
        FILE* cf = fopen(path, "r");
        while (cf && fgets(line, sizeof line, cf)) {
            char hex[40];
            if (sscanf(line, "console_key = %39s", hex) == 1 && strlen(hex) == 32) found = true;
            if (strstr(line, "console_id")) legacy = true;
        }
        if (cf) fclose(cf);
        CHECK(found && !legacy);
        cf = fopen(path, "a");   // an old file's id is ignored, a broken key too
        fputs("console_id = 12345679\nconsole_key = nothex\n", cf);
        fclose(cf);
        e = *mp_config();
        memset(e.consoleKey, 0, 16);
        mp_config_set(&e);
        CHECK(mp_config_load() && !memcmp(mp_config()->consoleKey, key, 16));   // (the first valid line stays: the broken one is skipped)
    }
    {   // keepsafe: that file (a key twice) is not understood any more: read only. A clean file again for the rest of the test
        char more[400];
        for (const char* ext : {"", ".bak", ".unreadable"}) {
            snprintf(more, sizeof more, "%s%s", path, ext);
            remove(more);
        }
        mp_config_load();
        CHECK(mp_config_save());
    }
    {   // touches: the host-rules card's memory (config only): "don't show again", the hosts seen (newest first, four kept)
        CHECK(!mp_core_rules_hidden() && mp_core_rules_wanted(111) && !mp_core_rules_wanted(0));
        mp_core_rules_done(111, false);
        CHECK(!mp_core_rules_wanted(111) && mp_core_rules_wanted(222) && !mp_core_rules_hidden());
        for (uint32_t k = 222; k <= 555; k += 111) mp_core_rules_done(k, false);   // 5 hosts: the oldest falls out
        CHECK(mp_core_rules_wanted(111) && !mp_core_rules_wanted(555) && !mp_core_rules_wanted(222));
        mp_core_rules_done(333, false);   // (already known: moves to the front, nothing lost)
        CHECK(!mp_core_rules_wanted(444) && !mp_core_rules_wanted(333));
        mp_config_set(mp_config());
        CHECK(mp_config_load() && !mp_core_rules_wanted(333) && !mp_core_rules_wanted(555));   // saved in mp.cfg, read back
        mp_core_rules_done(0, true);                                                           // "don't show again"
        CHECK(mp_core_rules_hidden() && !mp_core_rules_wanted(999));
        CHECK(mp_config_load() && mp_core_rules_hidden());
    }
    char out[17];
    mp_name_clean("  Mar\x01io\xff\xfe  ", 14, out);
    CHECK(!strcmp(out, "Mar?io??"));
    mp_name_clean("", 0, out);
    CHECK(!strcmp(out, ""));   // core2: nothing left is empty; the shown name is the character's
    mp_name_or_character(out, MP_LOOK_LUIGI);
    CHECK(!strcmp(out, "Luigi"));
    mp_name_clean("\xc3\xa9t\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 20, out);   // never splits a character
    CHECK(strlen(out) <= MP_NAME_BYTES && (out[strlen(out) - 1] & 0xC0) != 0xC0);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
    printf("config: ok\n");
}

// review: two successors claim the host role for the same epoch (the link between them was cut for longer than the 3 s
// step): the higher slot must step down when the lower one's HOST_CHANGE finally arrives, not drop it as an in-channel
// message from a known peer and end its own session ("nobody followed the new host").
static bool g_cut23 = false;
static bool Cut23(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    if (!g_cut23) return true;
    const bool a = from.host == 2 || from.host == 3, b = to.host == 2 || to.host == 3;
    return !(a && b && from.host != to.host);
}
static void TestMigrationDoubleClaim() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 141);
    all[0]->uiStart();
    Step(all, 300);
    hub.setFilter(Cut23, 0);
    g_cut23 = true;
    hub.setDown(1, true);   // the host vanishes while slots 1 and 2 cannot hear each other
    for (int t = 0; t < 12000 && !(all[1]->isHost() && all[2]->isHost()); t += 10) Step(all, 10);
    CHECK(all[1]->isHost() && all[2]->isHost());   // both claimed epoch 1
    g_cut23 = false;
    Step(all, 8000);
    CHECK(all[1]->active() && all[1]->isHost() && all[1]->hostEpoch() == 1);
    CHECK(all[2]->active() && !all[2]->isHost() && all[2]->hostSlot() == 1);
    CHECK(all[1]->slotPresent(2) && all[2]->slotPresent(1));
    hub.setFilter(0, 0);
    Free(all);
    printf("migration with two claimants: ok\n");
}

// 11. coop (greeting emote + pointer ping): EMOTE (reliable) and POINTER (unreliable, latest wins) are relayed by the host to
// every other guest with the original sender's slot; the session layer's length table refuses oversize ones; codecs are strict
static void TestCoopMessages() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 77);
    CHECK(all[0]->uiStart());
    Step(all, 300);
    // codecs
    uint8_t eb[8], pb[32];
    MpWriter ew(eb, sizeof eb);
    mp_write_emote(ew, MpEmoteMsg{2, MP_EMOTE_GESTURE});
    CHECK(ew.ok() && ew.n == 2);
    MpPointerMsg pm = {7, 1, {1.5f, -20.0f, 3000.25f}};
    MpWriter pw(pb, sizeof pb);
    mp_write_pointer(pw, pm);
    CHECK(pw.ok() && pw.n == 15);
    {
        MpEmoteMsg e;
        MpReader r(eb, 2);
        CHECK(mp_read_emote(r, &e) && e.emote == 2 && e.flags == MP_EMOTE_GESTURE);
        const uint8_t badId[2] = {4, 0}, badFlags[2] = {0, 2}, longer[3] = {0, 0, 0};
        MpReader r1(badId, 2), r2(badFlags, 2), r3(longer, 3), r4(eb, 1);
        CHECK(!mp_read_emote(r1, &e) && !mp_read_emote(r2, &e) && !mp_read_emote(r3, &e) && !mp_read_emote(r4, &e));
        MpPointerMsg q;
        MpReader rp(pb, 15);
        CHECK(mp_read_pointer(rp, &q) && q.serial == 7 && q.on == 1 && q.pos[2] == 3000.25f);
        uint8_t nan[15];
        memcpy(nan, pb, 15);
        nan[11] = 0xFF; nan[12] = 0xFF; nan[13] = 0xC0; nan[14] = 0x7F;   // pos[2] = NaN
        MpReader rn(nan, 15), rs(pb, 14);
        CHECK(!mp_read_pointer(rn, &q) && !mp_read_pointer(rs, &q));
        uint8_t on2[15];
        memcpy(on2, pb, 15);
        on2[2] = 2;
        MpReader ro(on2, 15);
        CHECK(!mp_read_pointer(ro, &q));
    }
    // guest 1 greets and points; the host relays both to guest 2 (from slot 1)
    CHECK(all[1]->send(MP_MSG_EMOTE, eb, 2, true));
    for (int i = 0; i < 5; i++) {
        pm.pos[0] = (float)i;
        MpWriter w(pb, sizeof pb);
        mp_write_pointer(w, pm);
        CHECK(all[1]->send(MP_MSG_POINTER, pb, (uint16_t)w.n, false));
    }
    Step(all, 200);
    for (int c = 0; c < 3; c++) {
        if (c == 1) continue;
        MpMsg m;
        int emotes = 0, pointers = 0;
        float last = -1.0f;
        while (all[c]->poll(&m)) {
            if (m.type == MP_MSG_EMOTE) { emotes++; CHECK(m.fromSlot == 1 && m.len == 2 && m.data[0] == 2); }
            if (m.type == MP_MSG_POINTER) {
                pointers++;
                CHECK(m.fromSlot == 1 && m.len == 15);
                MpReader r(m.data, m.len);
                MpPointerMsg q;
                CHECK(mp_read_pointer(r, &q));
                CHECK(q.pos[0] > last);   // never an older one after a newer one
                last = q.pos[0];
            }
        }
        CHECK(emotes == 1);
        CHECK(pointers >= 1 && last == 4.0f);
    }
    // oversize: refused by the sender's own table (and would be dropped + counted by a receiver)
    uint8_t big[32] = {0};
    CHECK(!all[1]->send(MP_MSG_POINTER, big, 17, false));
    CHECK(!all[1]->send(MP_MSG_EMOTE, big, 9, true));
    CHECK(mp_msg_host_relays(MP_MSG_POINTER) && !mp_msg_host_only(MP_MSG_POINTER));
    Free(all);
    printf("coop messages: ok\n");
}

// bubble: BUBBLE from a guest reaches the host and the other guest (relayed); RESTART is host only; the "Falls" setting (downed)
// travels in SETTINGS and an out-of-range one is refused.
static void TestBubble() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 12);
    MpCore* h = all[0];
    CHECK(h->uiStart());
    Step(all, 300);
    uint8_t b[8];
    MpWriter w(b, sizeof b);
    mp_write_bubble(w, MpBubbleMsg{4, 1, 2, MP_BUBBLE_BY_NONE, 0});
    CHECK(all[1]->send(MP_MSG_BUBBLE, b, (uint16_t)w.n, true));
    uint8_t r[2] = {4, 0};
    CHECK(!all[1]->send(MP_MSG_RESTART, r, 2, true));   // a guest cannot restart everyone
    CHECK(h->send(MP_MSG_RESTART, r, 2, true));
    Step(all, 500);
    MpMsg m;
    int bubbleAtHost = 0, bubbleAtOther = 0, restartAtGuests = 0;
    while (h->poll(&m)) if (m.type == MP_MSG_BUBBLE && m.fromSlot == 1 && m.len == 6) ++bubbleAtHost;
    while (all[2]->poll(&m)) {
        if (m.type == MP_MSG_BUBBLE && m.fromSlot == 1 && m.len == 6) ++bubbleAtOther;
        if (m.type == MP_MSG_RESTART && m.fromSlot == 0) ++restartAtGuests;
    }
    while (all[1]->poll(&m)) if (m.type == MP_MSG_RESTART && m.fromSlot == 0) ++restartAtGuests;
    CHECK(bubbleAtHost == 1 && bubbleAtOther == 1 && restartAtGuests == 2);
    // the setting
    MpSettings s;
    h->settings(&s);
    CHECK(s.downed == MP_DOWNED_BUBBLES);
    s.downed = MP_DOWNED_INFINITE;
    CHECK(h->updateSettings(&s));
    Step(all, 300);
    MpSettings g;
    all[2]->settings(&g);
    CHECK(g.downed == MP_DOWNED_INFINITE);
    s.downed = 7;
    CHECK(!h->updateSettings(&s));
    all[1]->settings(&g);
    CHECK(g.downed == MP_DOWNED_INFINITE);
    Free(all);
    printf("bubble: ok\n");
}


// ---------------------------------------------------------------------------------------------------------------------------
// netpack: datagram bundles, keep-awake, resend backoff, flood limits
// ---------------------------------------------------------------------------------------------------------------------------
static void ForEachPacket(const uint8_t* d, size_t len, void (*fn)(void*, const uint8_t*, size_t), void* ctx) {
    uint8_t plain[2048];   // core2: a sealed datagram is opened first (MpCore::debugOpenAny)
    if (len >= 2 && d[0] == 'G' && d[1] == 'S') {
        size_t pl = 0;
        if (!MpCore::debugOpenAny(d, len, plain, sizeof plain, &pl)) return;
        d = plain;
        len = pl;
    }
    if (len >= 4 && d[0] == 'G' && d[1] == 'B') {
        size_t off = 4;
        for (unsigned i = 0; i < d[3] && off + 2 <= len; i++) {
            const size_t n = d[off] | (d[off + 1] << 8);
            off += 2;
            if (n > len - off) return;
            fn(ctx, d + off, n);
            off += n;
        }
    } else if (len >= 12) fn(ctx, d, len);
}

static std::vector<uint8_t> MakeBundle(const std::vector<std::vector<uint8_t>>& pk) {
    std::vector<uint8_t> b = {'G', 'B', MP_PROTO_VERSION, (uint8_t)pk.size()};
    for (auto& p : pk) { b.push_back((uint8_t)p.size()); b.push_back((uint8_t)(p.size() >> 8)); b.insert(b.end(), p.begin(), p.end()); }
    return b;
}

struct NpTap {
    uint64_t datagrams = 0, packets = 0, maxLen = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> last;      // (from,to) -> last send time
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> maxGap;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> count;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> startAt;
    uint64_t lastReliableSeqToGuest = 0;
    bool dropGuestToHost = false;
};
static NpTap g_np;
static void NpTapFn(void*, const MpAddr& from, const MpAddr& to, const void* data, size_t len) {
    if (from.port != MP_SESSION_PORT || to.host == MP_ADDR_BROADCAST) return;   // (beacons are not a peer link)
    g_np.datagrams++;
    if (len > g_np.maxLen) g_np.maxLen = len;
    ForEachPacket((const uint8_t*)data, len, [](void*, const uint8_t* p, size_t n) {
        g_np.packets++;
        if ((p[5] & MP_HDR_RELIABLE) && !(p[5] & MP_HDR_ACK_ONLY) && p[3] == MP_MSG_STAR) g_np.lastReliableSeqToGuest = p[6] | (p[7] << 8);
        (void)n;
    }, 0);
    const auto k = std::make_pair(from.host, to.host);
    if (g_np.last.count(k)) {
        const uint64_t gap = g_now - g_np.last[k];
        if (gap > g_np.maxGap[k]) g_np.maxGap[k] = gap;
    }
    g_np.last[k] = g_now;
    g_np.count[k]++;
}
static bool NpFilterFn(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    (void)to;
    return !(g_np.dropGuestToHost && from.host != 1);
}

static void TestNetpackBundles() {
    // 1. coalescing: a 4-player session with STATE flowing (30 Hz each way): fewer datagrams than packets, none over the limit,
    //    and every STATE still arrives (the receiver unpacks)
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 4, 61);
        all[0]->uiStart();
        Step(all, 500);
        g_np = NpTap();
        hub.setTap(NpTapFn, 0);
        uint8_t pose[51];
        memset(pose, 0, sizeof pose);
        int got = 0;
        for (int ms = 0; ms < 3000; ms += 5) {
            if (ms % 30 == 0)
                for (MpCore* c : all) c->send(MP_MSG_STATE, pose, 51, false);
            Step(all, 5);
            MpMsg m;
            for (MpCore* c : all) while (c->poll(&m)) if (m.type == MP_MSG_STATE) got++;
        }
        hub.setTap(0, 0);
        printf("netpack: 4 players 3 s: %llu datagrams, %llu packets, max datagram %llu bytes, STATE received %d\n",
               (unsigned long long)g_np.datagrams, (unsigned long long)g_np.packets, (unsigned long long)g_np.maxLen, got);
        CHECK(g_np.datagrams * 3 <= g_np.packets * 2);   // (the host's links carry ~3 poses a datagram; a guest sends its one pose at once)
        CHECK(g_np.maxLen <= MP_DATAGRAM_MAX);
        CHECK(got > 100 * 3 * 3 / 2);
        // keep-awake: STATE flows (every 33 ms), so the keep-awake adds nothing: about one datagram per STATE period per link
        const auto k = std::make_pair((uint32_t)1, (uint32_t)2);
        CHECK(g_np.count[k] <= 3000 / 30 + 30);
        for (auto& kv : g_np.maxGap) CHECK(kv.second <= MP_KEEPAWAKE_MS);
        Free(all);
    }
    // 2. idle lobby and an idle started session (a load barrier): no link is silent for longer than MP_KEEPAWAKE_MS, either way
    for (int started = 0; started < 2; started++) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 71 + (uint32_t)started);
        if (started) all[0]->uiStart();
        Step(all, 1000);
        g_np = NpTap();
        hub.setTap(NpTapFn, 0);
        Step(all, 5000);
        hub.setTap(0, 0);
        CHECK(g_np.maxGap.size() >= 4);   // (host <-> 2 guests, both ways)
        for (auto& kv : g_np.maxGap) {
            CHECK(kv.second <= MP_KEEPAWAKE_MS);
            if (kv.second > MP_KEEPAWAKE_MS) fprintf(stderr, "  gap %llu ms on %u->%u\n", (unsigned long long)kv.second, kv.first.first, kv.first.second);
        }
        for (auto& kv : g_np.count) CHECK(kv.second < 5000 / MP_KEEPAWAKE_MS + 40);   // (and no more than needed: no extra sends)
        Free(all);
        printf("netpack: idle %s: max gap %llu ms\n", started ? "started session" : "lobby", (unsigned long long)g_np.maxGap.begin()->second);
    }
    // 2b. corefix's liveness rules with keep-awake heartbeats flowing: an idle but healthy lobby / session is never dropped by the 5 s
    //     ack-stall rule (every peer sends a heartbeat with its ack at least every 50 ms; nothing to acknowledge = progress), and a
    //     ghost that only sends heartbeats is still removed (TestGhostJoiners, with the host's own keep-awake heartbeats going to it)
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 75);
        const int before = LogCount("no acknowledgement");
        Step(all, 2 * MP_ACK_STALL_MS + 2000);
        CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2) && all[1]->active() && all[2]->active());
        CHECK(LogCount("no acknowledgement") == before);
        all[0]->uiStart();
        Step(all, 2 * MP_ACK_STALL_MS + 2000);   // (and idle in a started session)
        CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2) && all[1]->active() && all[2]->active());
        CHECK(LogCount("no acknowledgement") == before);
        Free(all);
    }
    // 3. malformed bundles are refused whole (stray), a good one is unpacked
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 81);
        uint8_t hb[8] = {0};
        std::vector<uint8_t> a, b;
        WritePacket(a, MP_MSG_HEARTBEAT, 1, 0, 1, hb, 8);
        WritePacket(b, MP_MSG_HEARTBEAT, 1, 0, 2, hb, 8);
        const uint32_t bad0 = all[0]->debugBadCount();
        uint32_t stray = all[0]->debugStray();
        std::vector<uint8_t> good = MakeBundle({a, b});
        all[0]->debugReceiveAuthed(1, good.data(), good.size());
        CHECK(all[0]->debugStray() == stray);   // accepted
        auto expectStray = [&](std::vector<uint8_t> d, const char* what) {
            const uint32_t before = all[0]->debugStray();
            all[0]->debugReceiveAuthed(1, d.data(), d.size());
            if (all[0]->debugStray() != before + 1) fprintf(stderr, "  bundle case not refused: %s\n", what);
            CHECK(all[0]->debugStray() == before + 1);
        };
        std::vector<uint8_t> d = good;
        d.resize(d.size() - 3); expectStray(d, "truncated last packet");
        d = good; d.push_back(0); expectStray(d, "trailing byte");
        d = good; d[3] = 3; expectStray(d, "count too large for the data");
        d = good; d[3] = 255; expectStray(d, "count 255");
        d = good; d[3] = 65; expectStray(d, "count over the cap");
        d = good; d[3] = 1; expectStray(d, "count 1 (a bundle has two or more)");
        d = good; d[3] = 0; expectStray(d, "count 0");
        d = good; d[4] = 0; d[5] = 0; expectStray(d, "zero length");
        d = good; d[4] = 0xFF; d[5] = 0xFF; expectStray(d, "length 65535");
        d = good; d[4] = (uint8_t)(d[4] + 1); expectStray(d, "length one too long");
        d = good; d[4] = 11; d[5] = 0; expectStray(d, "length below a header");
        d = good; d[2] = MP_PROTO_VERSION - 1; expectStray(d, "other protocol");
        d = good; d[6] = 'X'; expectStray(d, "bad packet magic");
        d = std::vector<uint8_t>(good.begin(), good.begin() + 5); expectStray(d, "header + 1 byte");
        d = {'G', 'B', MP_PROTO_VERSION}; expectStray(d, "3 bytes");
        d = MakeBundle({good, a}); expectStray(d, "nested bundle");
        d = good; d.resize(MP_DATAGRAM_MAX + 1, 0); expectStray(d, "over the datagram limit");
        CHECK(all[0]->debugBadCount() == bad0);   // (a stray is not a peer's bad message; nothing was delivered or kicked)
        CHECK(all[0]->slotPresent(1));
        Free(all);
    }
    printf("netpack bundles: ok\n");
}

static void TestNetpackBackoff() {
    // 1. MpReliable alone: 64 messages of 1 KB to a peer that never answers: resend bytes stay under budget
    {
        static MpReliable rel;
        rel.reset(1);
        std::vector<uint8_t> payload(1024, 7);
        for (int i = 0; i < 30; i++) CHECK(rel.push(0x11, 0, payload.data(), 1024));   // (the send queue holds 32 KB)
        struct Ctx { uint64_t bytes = 0, msgs = 0; } ctx;
        uint64_t t = 1000, firstBytes = 0;
        uint64_t maxPerSec = 0, secBytes = 0, secStart = t;
        for (; t < 1000 + 20000; t += 5) {
            const uint64_t before = ctx.bytes;
            rel.emitDue(t, 60, 32, [](void* c, const MpRelMsg& m) { Ctx* x = (Ctx*)c; x->bytes += m.len + 12; x->msgs++; return true; }, &ctx);
            if (t == 1000) firstBytes = ctx.bytes;
            secBytes += ctx.bytes - before;
            if (t - secStart >= 1000) { if (t > 3000 && secBytes > maxPerSec) maxPerSec = secBytes; secBytes = 0; secStart = t; }
        }
        const uint64_t resent = ctx.bytes - firstBytes;
        printf("netpack backoff: 30 KB queued, dead peer, 20 s: first %llu B, resent %llu B, worst second after 3 s %llu B\n",
               (unsigned long long)firstBytes, (unsigned long long)resent, (unsigned long long)maxPerSec);
        CHECK(resent < 8 * 1024 + 20 * 24 * 1024 + 4096);   // (the old timer: ~500 KB/s)
        CHECK(maxPerSec <= 28 * 1024);
        // one small message: the gaps double (60, 120, 240, 480, 960, 960 ...): about 12 sends in 20 s, never every 120 ms
        rel.reset(1);
        uint8_t one = 1;
        CHECK(rel.push(0x11, 0, &one, 1));
        int sends = 0;
        for (t = 1000; t < 21000; t += 5) rel.emitDue(t, 60, 32, [](void* c, const MpRelMsg&) { (*(int*)c)++; return true; }, &sends);
        CHECK(sends <= 100 && sends >= 8);   // (the old timer: ~170)
    }
    // 2. a crafted heartbeat echo cannot pull the smoothed RTT down (echo = now: RTT 0), nor a future echo
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 91);
        Step(all, 1500);
        const int before = all[0]->debugSrtt(1);
        for (int i = 0; i < 50; i++) {
            uint8_t hb[8];
            const uint32_t now32 = (uint32_t)g_now, zero = 0;
            memcpy(hb, &now32, 4);
            memcpy(hb + 4, &now32, 4);   // echo = the instant of receipt
            (void)zero;
            std::vector<uint8_t> pk;
            WritePacket(pk, MP_MSG_HEARTBEAT, 1, 0, (uint16_t)(60000 + i), hb, 8);
            all[0]->debugReceiveAuthed(1, pk.data(), pk.size());
        }
        const int after = all[0]->debugSrtt(1);
        printf("netpack echo: srtt %d -> %d after 50 crafted zero-RTT echoes\n", before, after);
        CHECK(after >= before);
        Free(all);
    }
    // 3. the rate cap no longer swallows acks: a guest at its cap still gets its ack counted
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 95);
        Step(all, 500);
        g_np = NpTap();
        g_np.dropGuestToHost = true;      // the host's messages reach the guest, nothing comes back
        hub.setFilter(NpFilterFn, 0);
        hub.setTap(NpTapFn, 0);
        uint8_t star[8] = {0, 0, 0, 0, 1, 0, 0, 0};
        CHECK(all[0]->send(MP_MSG_STAR, star, 8, true));
        Step(all, 100);
        CHECK(all[0]->debugUnacked(1) >= 1);
        const uint16_t seq = (uint16_t)g_np.lastReliableSeqToGuest;
        uint8_t pose[51] = {0};
        std::vector<uint8_t> pk;
        for (int i = 0; i < MP_RATE_CAP + 20; i++) {
            WritePacket(pk, MP_MSG_STATE, 1, 0, (uint16_t)(5000 + i), pose, 51);
            all[0]->debugReceiveAuthed(1, pk.data(), pk.size());
        }
        WritePacket(pk, 0, 1, MP_HDR_ACK | MP_HDR_ACK_ONLY, 0, 0, 0);
        pk[8] = (uint8_t)seq; pk[9] = (uint8_t)(seq >> 8);
        all[0]->debugReceiveAuthed(1, pk.data(), pk.size());
        CHECK(all[0]->debugUnacked(1) == 0);   // (the ack was processed although the guest is over its cap)
        hub.setFilter(0, 0);
        hub.setTap(0, 0);
        Free(all);
    }
    printf("netpack backoff / echo / rate-cap acks: ok\n");
}

static void TestNetpackFlood() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 101);
    Step(all, 200);
    const MpAddr haddr = all[0]->debugLocalAddr();
    // 10,000 HELLOs of another build from strangers: the log stays small
    g_log.clear();
    uint8_t hello[24] = {0};
    hello[0] = 0xEF; hello[1] = 0xBE;   // a fingerprint that is not ours
    std::vector<uint8_t> pk;
    for (int i = 0; i < 10000; i++) {
        WritePacket(pk, MP_MSG_HELLO, 0xFF, MP_HDR_RELIABLE, 1, hello, sizeof hello);
        all[0]->debugReceive(MpAddr{(uint32_t)(5000 + i % 50), (uint16_t)MP_SESSION_PORT}, pk.data(), pk.size());
    }
    const int lines = LogCount("hello rejected");
    printf("netpack flood: 10000 bad HELLOs -> %d log lines\n", lines);
    CHECK(lines <= MP_LOG_BURST + 6);
    // a backlog of 3000 datagrams in the socket: one pump handles at most MP_RECV_BUDGET_DATAGRAMS, the rest is dropped
    uint8_t junk[20] = {1, 2, 3};
    for (int i = 0; i < 3000; i++) hub.inject(MpAddr{7000, (uint16_t)MP_SESSION_PORT}, haddr, junk, sizeof junk);
    uint32_t total = 0, worst = 0;
    int pumps = 0;
    for (; pumps < 20; pumps++) {
        const uint32_t before = all[0]->debugStray();
        g_now += 5;
        all[0]->pump();
        const uint32_t h = all[0]->debugStray() - before;
        total += h;
        if (h > worst) worst = h;
        if (!h) break;
    }
    printf("netpack flood: 3000 queued datagrams -> at most %u handled per pump (%u in all, %u pumps with drops)\n", worst, total, all[0]->debugRecvDropped());
    CHECK(worst <= MP_RECV_BUDGET_DATAGRAMS);
    CHECK(total < 3000 && all[0]->debugRecvDropped() >= 1);
    CHECK(all[0]->slotPresent(1));
    Free(all);
    printf("netpack flood: ok\n");
}

// netpack (memory): the per-session block and the compact receive queue
static void TestNetpackMemory() {
    printf("netpack memory: sizeof(MpCoreMem) = %zu bytes (was 635184 before the compact queue and 3 windows)\n", MpCore::debugMemBytes());
    CHECK(MpCore::debugMemBytes() < 260000);   // core2: + 60 KB for 1024-message backlogs (a 15 s freeze in a star-bit storm); netopt: two of the three windows are separate blocks (a guest holds one)
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 111);
    all[0]->uiStart();
    Step(all, 300);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    // long messages (SWITCHMAP, 568 bytes) interleaved with short ones: contents, order and sender survive the queue
    uint8_t map[568], pose[51] = {0}, ev[8] = {0};
    int sent = 0;
    for (int i = 0; i < MP_QBIG; i++) {
        for (size_t k = 0; k < sizeof map; k++) map[k] = (uint8_t)(i * 7 + k);
        CHECK(all[0]->send(MP_MSG_SWITCHMAP, map, sizeof map, true));
        sent++;
        pose[0] = (uint8_t)i;
        all[0]->send(MP_MSG_STATE, pose, 51, false);
        ev[0] = (uint8_t)i;
        all[0]->send(MP_MSG_STAR, ev, 8, true);
        Step(all, 20);
    }
    Step(all, 500);
    int got = 0, stars = 0;
    for (MpCore* c : {all[1], all[2]}) {
        got = stars = 0;
        while (c->poll(&m)) {
            if (m.type == MP_MSG_SWITCHMAP) {
                CHECK(m.len == 568 && m.fromSlot == 0);
                bool ok = true;
                for (size_t k = 0; k < sizeof map; k++) if (m.data[k] != (uint8_t)(got * 7 + k)) ok = false;
                CHECK(ok);
                got++;
            } else if (m.type == MP_MSG_STAR) {
                CHECK(m.len >= 8);
                stars++;
            }
        }
        CHECK(got == sent && stars == sent);
        CHECK(c->active());
    }
    // a host that leaves: the guests build their peers (3 windows for 4 slots) from the same block, nothing else is allocated
    Free(all);
    {
        MpLoopHub hub2;
        hub2.setClock(VNow, 0);
        std::vector<MpCore*> a4 = StartSession(hub2, 4, 121);
        a4[0]->uiStart();
        Step(a4, 300);
        hub2.setDown(1, true);
        Step(a4, 20000);
        int hosts = 0, active = 0;
        for (int i = 1; i < 4; i++) { if (a4[i]->isHost()) hosts++; if (a4[i]->active()) active++; }
        CHECK(hosts == 1 && active == 3);
        CHECK(LogCount("no free reliable window") == 0);
        Free(a4);
    }
    printf("netpack memory: ok\n");
}

// netpack: with a creeping clock (every read a little later than the last: g_creep 1 and 2) a 4-player session with STATE flowing sends
// no more datagrams per second than with a steady clock (no extra keep-awake heartbeats, no early bundle flushes, no resend storms)
// and nobody is dropped for a false timeout.
static double FlowRate(uint64_t creep, int* badLines) {   // datagrams per link and pump
    g_creep = creep;
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    const size_t log0 = g_log.size();
    std::vector<MpCore*> all = StartSession(hub, 4, 131);
    all[0]->uiStart();
    Step(all, 500);
    g_np = NpTap();
    hub.setTap(NpTapFn, 0);
    const uint64_t t0 = g_now;
    uint8_t pose[51];
    memset(pose, 0, sizeof pose);
    for (int ms = 0; ms < 4000; ms += 5) {
        if (ms % 30 == 0)
            for (MpCore* c : all) c->send(MP_MSG_STATE, pose, 51, false);
        Step(all, 5);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}
    }
    hub.setTap(0, 0);
    // (netcore: per link and pump, not per virtual second: with a creeping clock every read advances it (1-2 ms a read: a pump spans
    // tens of virtual ms), so the same 4000 loop-ms of sends and pumps cover far more virtual time and a per-virtual-second rate fell by
    // itself (the check could never fail). What a storm would show is more than about one datagram per link per pump: the host pumps
    // 800 times with 3 links, each guest 800 times with 1.)
    const double secs = (double)(800 * 3 + 800 * 3);
    (void)t0;
    CHECK(all[0]->slotPresent(1) && all[0]->slotPresent(2) && all[0]->slotPresent(3));
    for (MpCore* c : all) CHECK(c->active());
    *badLines = 0;
    for (size_t i = log0; i < g_log.size(); i++)
        if (g_log[i].find("lost") != std::string::npos || g_log[i].find("no acknowledgement") != std::string::npos ||
            g_log[i].find("no answer") != std::string::npos || g_log[i].find("claim") != std::string::npos) (*badLines)++;
    Free(all);
    g_creep = 0;
    return (double)g_np.datagrams / secs;
}
static void TestNetpackCreep() {
    int bad0 = 0, bad1 = 0, bad2 = 0;
    const double r0 = FlowRate(0, &bad0), r1 = FlowRate(1, &bad1), r2 = FlowRate(2, &bad2);
    printf("netpack creeping clock: datagrams per link and pump, 4 players with STATE flowing: steady %.2f, creep 1 %.2f, creep 2 %.2f\n", r0, r1, r2);
    CHECK(bad0 == 0 && bad1 == 0 && bad2 == 0);
    CHECK(r0 <= 1.0 && r1 <= 1.5 && r2 <= 1.5);   // (a creeping clock ends every pose hold at once: about one per link and pump, no storm)
    printf("netpack creeping clock: ok\n");
}


// ---------------------------------------------------------------------------------------------------------------------------
// netcore: review of netpack (untrusted bundles, keep-awake vs liveness, backoff and the resend byte cap)
static uint64_t g_outageEvery = 0, g_outageLen = 0;   // (filter) every g_outageEvery ms the link of endpoint g_cutB drops all for g_outageLen
static bool OutageFilter(void*, const MpAddr& from, const MpAddr& to, const void*, size_t) {
    if (!g_outageEvery || (from.host != g_cutB && to.host != g_cutB)) return true;
    return g_now % g_outageEvery >= g_outageLen;
}
static void TestNetcoreBundleReview() {
    // 1. the per-guest rate cap counts the packets inside a bundle: 4 bundles of 64 packets inside a second are 256 > MP_RATE_CAP
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 1101);
        all[0]->uiStart();
        Step(all, 300);
        std::vector<std::vector<uint8_t>> pk;
        for (int i = 0; i < MP_BUNDLE_MAX; i++) {
            std::vector<uint8_t> p;
            WritePacket(p, 0, 1, MP_HDR_ACK_ONLY, 0, nullptr, 0);
            pk.push_back(p);
        }
        const std::vector<uint8_t> b = MakeBundle(pk);
        CHECK(b.size() <= MP_DATAGRAM_MAX);
        const uint32_t bad0 = all[0]->debugBadCount();
        for (int i = 0; i < 4; i++) all[0]->debugReceiveAuthed(1, b.data(), b.size());
        const uint32_t bad = all[0]->debugBadCount() - bad0;
        printf("netcore bundles: 4 bundles of %d packets from a guest in one second -> %u over the rate cap\n", MP_BUNDLE_MAX, bad);
        CHECK(bad >= 4 * MP_BUNDLE_MAX - MP_RATE_CAP - 8);
        Step(all, 100);
        CHECK(!all[0]->slotPresent(1));   // (more than MP_BAD_KICK bad messages: kicked, as 256 single datagrams would have been)
        Free(all);
    }
    // 2. the receive budget counts packets: a flood of full bundles from a stranger is handled at most MP_RECV_BUDGET_DATAGRAMS
    //    packets per pump (it used to be datagrams: 64x more work per pump with bundles)
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 1103);
        Step(all, 200);
        const MpAddr haddr = all[0]->debugLocalAddr();
        std::vector<std::vector<uint8_t>> pk;
        for (int i = 0; i < MP_BUNDLE_MAX; i++) {
            std::vector<uint8_t> p;
            WritePacket(p, MP_MSG_STATE, 1, 0, (uint16_t)i, nullptr, 0);
            pk.push_back(p);
        }
        const std::vector<uint8_t> b = MakeBundle(pk);
        for (int i = 0; i < 200; i++) hub.inject(MpAddr{7001, (uint16_t)MP_SESSION_PORT}, haddr, b.data(), b.size());
        uint32_t worst = 0;
        for (int pumps = 0; pumps < 30; pumps++) {
            const uint32_t before = all[0]->debugStray();
            g_now += 5;
            all[0]->pump();
            const uint32_t h = all[0]->debugStray() - before;
            if (h > worst) worst = h;
            if (!h) break;
        }
        printf("netcore bundles: 200 bundles x %d packets queued -> at most %u packets handled per pump\n", MP_BUNDLE_MAX, worst);
        CHECK(worst <= MP_RECV_BUDGET_DATAGRAMS + MP_BUNDLE_MAX);
        CHECK(all[0]->slotPresent(1));
        Free(all);
    }
    // 3. a bundle from an unknown address: each packet as if alone (a HELLO in it is a join attempt; the rest stray); a bundle that
    //    mixes a packet of another protocol: that packet is refused, the others are taken
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 1105);
        all[0]->uiStart();
        Step(all, 300);
        uint8_t hb[8] = {0};
        std::vector<uint8_t> a, b, c;
        WritePacket(a, MP_MSG_HEARTBEAT, 1, MP_HDR_ACK_ONLY, 0, nullptr, 0);
        WritePacket(b, MP_MSG_HEARTBEAT, 1, 0, 7, hb, 8);
        b[2] = MP_PROTO_VERSION - 1;
        WritePacket(c, 0, 1, MP_HDR_ACK_ONLY, 0, nullptr, 0);
        std::vector<uint8_t> mixed = MakeBundle({a, b, c});
        const uint32_t bad0 = all[0]->debugBadCount();
        all[0]->debugReceiveAuthed(1, mixed.data(), mixed.size());
        CHECK(all[0]->debugBadCount() == bad0 + 2);   // (the heartbeat with data in an ack-only packet, and the other protocol)
        const uint32_t stray0 = all[0]->debugStray();
        all[0]->debugReceive(MpAddr{7002, (uint16_t)MP_SESSION_PORT}, mixed.data(), mixed.size());
        CHECK(all[0]->debugStray() == stray0 + 1);   // (core2: a plain datagram from a stranger is dropped whole, unopened)
        CHECK(all[0]->slotPresent(1) && all[1]->active());
        Free(all);
    }
    printf("netcore bundles: ok\n");
}

// keep-awake (netpack) vs corefix's liveness. A ghost that sends keep-awake heartbeats at 50 ms, bundled, with an ack field, is
// still removed (it never acknowledges what waits for it); a real guest whose link loses 20% and goes dark for 350 ms every
// second (Wi-Fi power save / bunching) for a minute is not.
static void GhostBeatBundle(MpLoopHub& hub, MpCore* host, uint32_t endpoint) {
    uint8_t body[8] = {0};
    std::vector<uint8_t> p1, p2;
    WritePacket(p1, MP_MSG_HEARTBEAT, 1, MP_HDR_ACK, 0, body, 8);
    WritePacket(p2, 0, 1, MP_HDR_ACK | MP_HDR_ACK_ONLY, 0, nullptr, 0);
    std::vector<uint8_t> b = MakeBundle({p1, p2});
    (void)host;
    (void)endpoint;
    g_ghost->send(hub, b.data(), b.size());
}
static void TestNetcoreKeepAwakeLiveness() {
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        MpCore* h = MakeCore(hub, 1, "Host", 1111);
        std::vector<MpCore*> all = {h};
        MpHostParams hp = HostParams(MP_MODE_TOGETHER, 2);
        CHECK(h->uiHost(&hp));
        Step(all, 100);
        GhostHello(hub, h, 7, "Ghost", 0xABCDEF02u, all);
        Step(all, 100);
        CHECK(h->slotPresent(1));
        uint64_t goneAt = 0;
        for (int t = 0; t < 9000; t += 50) {
            GhostBeatBundle(hub, h, 7);
            Step(all, 50);
            if (!goneAt && !h->slotPresent(1)) goneAt = (uint64_t)t;
        }
        printf("netcore liveness: a ghost sending bundled 50 ms heartbeats with acks is removed after %llu ms\n", (unsigned long long)goneAt);
        CHECK(goneAt > 0 && goneAt <= MP_ACK_STALL_MS + 1500);
        Free(all);
    }
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setFilter(OutageFilter, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 1113);
        all[0]->uiStart();
        Step(all, 300);
        MpLoopImpairment imp = {0.20f, 0.0f, 10, 40, 0};
        hub.setImpairment(imp, 1115);
        g_cutB = 2; g_outageEvery = 1000; g_outageLen = 350;
        Xo x(all);
        const int dropsBefore = LogCount("no acknowledgement") + LogCount("left reason");
        for (int t = 0; t < 60000; t += 50) {
            x.sendAll({0, 1});
            Step(all, 50);
            x.drainAll();
            if (!all[0]->slotPresent(1) || !all[1]->active()) break;
        }
        g_outageEvery = 0; g_cutB = 0;
        hub.setImpairment(MpLoopImpairment{0, 0, 0, 0, 0}, 1);
        XoStep(x, all, 3000, {});
        CHECK(all[0]->slotPresent(1) && all[1]->active());
        CHECK(LogCount("no acknowledgement") + LogCount("left reason") == dropsBefore);
        CHECK(x.dups == 0 && x.gaps == 0 && x.tailMissing({0, 1}) == 0);
        hub.setFilter(0, 0);
        Free(all);
    }
    printf("netcore liveness: ghost removed, a guest with 20%% loss and a 350 ms outage every second keeps its slot for 60 s: ok\n");
}

// Backoff and the resend byte cap. (1) Nothing starves: the oldest unacknowledged message is always first in line for the budget
// (a later one never takes tokens it waits for) and every message costs at most ~1.1 KB of a 24 KB/s budget, so a due resend waits
// at most ~50 ms for tokens: a stream and a backlog-sized burst on bad links arrive complete and in order, within bounds. (2) A host claim survives a follower whose Wi-Fi is gone
// for 2.0 / 2.5 / 3.0 s from the moment of the claim: the claimant's HOST_CHANGE resends (200 ms, x2, x4: then every 800 ms at the
// default ceiling) reach it inside MP_CLAIM_MS. With MP_BACKOFF_SHIFT=3 the last resend inside the window is at 3.0 s: a 3.0 s gap
// is then lost (DESIGN.md section 3: the ceiling is bounded by MP_CLAIM_MS - gap - RTT).
static void TestNetcoreBackoffBounds() {
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 2, 1121);
        all[0]->uiStart();
        Step(all, 300);
        MpMsg m;
        for (MpCore* c : all) while (c->poll(&m)) {}
        // (a) a stream: 30 messages of 400 B a second for 8 s at 30% loss (first sends 12 KB/s; resends stay under the budget)
        // (b) a burst of 60 x 500 B (30 KB: just under the 32 KB backlog) at 50% loss: resends far over the 24 KB/s budget
        for (int phase = 0; phase < 2; phase++) {
            MpLoopImpairment imp = {phase == 0 ? 0.30f : 0.50f, 0.0f, 10, 20, 0};
            hub.setImpairment(imp, 1123 + (uint32_t)phase);
            const int count = phase == 0 ? 240 : 60, size = phase == 0 ? 400 : 500;
            uint8_t big[500];
            std::vector<uint64_t> sentAt(count, 0);
            int sent = 0, got = 0, inOrder = 1, last = -1;
            uint64_t worst = 0;
            const uint64_t t0 = g_now;
            for (int t = 0; t < 60000 && got < count; t += 10) {
                while (sent < count && (phase == 1 || g_now - t0 >= (uint64_t)sent * 1000 / 30)) {
                    memset(big, 0, sizeof big);
                    memcpy(big, &sent, 4);
                    CHECK(all[0]->sendTo(1, MP_MSG_SWITCHMAP, big, (uint16_t)size, true));
                    sentAt[sent++] = g_now;
                }
                Step(all, 10);
                while (all[1]->poll(&m)) {
                    if (m.type != MP_MSG_SWITCHMAP) continue;
                    int v;
                    memcpy(&v, m.data, 4);
                    if (v != last + 1) inOrder = 0;
                    last = v;
                    if (v >= 0 && v < count && g_now - sentAt[v] > worst) worst = g_now - sentAt[v];
                    got++;
                }
            }
            printf("netcore backoff: %s: %d/%d delivered in order=%d, worst latency %llu ms, total %llu ms\n",
                   phase == 0 ? "stream 30 x 400 B/s at 30% loss" : "burst 60 x 500 B at 50% loss", got, count, inOrder,
                   (unsigned long long)worst, (unsigned long long)(g_now - t0));
            CHECK(got == count && inOrder && all[0]->slotPresent(1));
            CHECK(worst < (phase == 0 ? 6000u : 20000u));
        }
        Free(all);
    }
    const int gaps[4] = {2000, 2500, 3000, 3500};
    for (int gi = 0; gi < 4; gi++) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 1131 + (uint32_t)gi);
        all[0]->uiStart();
        Step(all, 300);
        hub.setDown(1, true);   // the host dies
        std::vector<MpCore*> g = Survivors(all, {0});
        // the only follower's Wi-Fi goes just before the successor claims (so not even the first HOST_CHANGE gets through) and
        // comes back `gap` ms after the claim
        int t = 0;
        for (; t < 4900; t += 5) Step(all, 5);
        hub.setDown(3, true);
        for (; t < 12000 && !g[0]->isHost(); t += 5) Step(all, 5);
        CHECK(g[0]->isHost());
        Step(all, gaps[gi]);
        hub.setDown(3, false);
        Step(all, 6000);
        const bool held = g[0]->isHost() && g[0]->hostEpoch() == 1 && g[1]->hostSlot() == 1 && g[0]->slotPresent(2) && g[1]->active();
        printf("netcore backoff: claim, follower gone for %d ms after it: %s\n", gaps[gi], held ? "held" : "LOST");
        CHECK(held);
        Free(all);
    }
    printf("netcore backoff: nothing starves behind the resend budget; a claim survives a 2-3 s follower gap: ok\n");
}


// netcore (coordinator, scenario mp_silence6): an ESTABLISHED guest whose console freezes (not pumped: suspend, a long Wi-Fi gap) keeps
// its slot up to MP_PEER_LOST_MS and catches up when it comes back; after a 16 s freeze it is removed and can rejoin into the same
// slot (its reservation, by its per-session console id); a ghost joiner is still removed at MP_ACK_STALL_MS (TestGhostJoiners,
// TestNetcoreKeepAwakeLiveness).
static void TestNetcoreFrozenGuest() {
    // core2 (claims audit 1): the guarantee is "a console frozen for up to 14 s keeps its place, catches up, exactly once, at a star-bit
    // storm's event rate": every awake console sends 10 tracked events a second (Star Bit DELTAs are batched to 10/s), the host 10 more
    // (its TOTALs); 3 and 4 players; freezes of 6..14 s in 1 s steps. (The host removes a silent console after MP_PEER_LOST_MS = 15 s.)
    int held = 0, runs = 0;
    for (int players = 3; players <= 4; players++)
        for (int freezeMs = 6000; freezeMs <= 14000; freezeMs += 1000) {
            MpLoopHub hub;
            hub.setClock(VNow, 0);
            std::vector<MpCore*> all = StartSession(hub, players, 1201 + (uint32_t)(players * 100 + freezeMs / 1000));
            all[0]->uiStart();
            Step(all, 300);
            Xo x(all);
            std::vector<int> everyone, awakeIdx;
            for (int i = 0; i < players; i++) everyone.push_back(i);
            for (int i = 0; i < players; i++) if (i != 2) awakeIdx.push_back(i);
            XoStep(x, all, 1000, everyone, 100);
            std::vector<MpCore*> awake;
            for (int i : awakeIdx) awake.push_back(all[i]);
            for (int t = 0; t < freezeMs; t += 50) {   // console 2 frozen: nothing pumps it, what is sent to it waits in its socket
                if (t % 100 == 0) { x.sendAll(awakeIdx); x.send(0); }
                Step(awake, 50);
                for (int i : awakeIdx) x.drain(i);
            }
            const bool presentAfter = all[0]->slotPresent(2);
            for (int t = 0; t < 6000; t += 100) {
                x.sendAll(everyone);
                x.send(0);
                Step(all, 100);
                x.drainAll();
            }
            XoStep(x, all, 4000, {}, 100);
            const int tail = x.tailMissing(everyone);
            const bool ok = presentAfter && all[0]->slotPresent(2) && all[2]->active() && all[2]->hostSlot() == 0 && all[0]->isHost() &&
                            x.dups == 0 && x.gaps == 0 && tail == 0 && x.memb[2] == 1;
            if (!ok)
                fprintf(stderr, "  frozen %d ms, %d players: present=%d active=%d hostSlot=%d dups=%d gaps=%d tail=%d memb=%u\n", freezeMs, players,
                        presentAfter, all[2]->active(), all[2]->hostSlot(), x.dups, x.gaps, tail, x.memb[2]);
            CHECK(ok);
            held += ok ? 1 : 0;
            runs++;
            Free(all);
        }
    // 16 s: removed (silent past MP_PEER_LOST_MS); on waking it must not take the host role from the healthy host; it ends up in the
    // session again (a new membership), the others undisturbed
    {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 1299);
        all[0]->uiStart();
        Step(all, 300);
        std::vector<MpCore*> awake = {all[0], all[1]};
        Step(awake, 16000);
        CHECK(!all[0]->slotPresent(2));
        int hosts = 0, act = 0;
        for (int t = 0; t < 30000; t += 50) {
            Step(all, 50);
            CHECK(all[0]->isHost() && all[0]->active() && all[1]->hostSlot() == 0);   // (its claim, if any, takes nobody along)
            if (!all[2]->active() && Phase(all[2]) == MP_UI_IDLE) ScanJoin(all, all[2], 200);
            hosts = act = 0;
            for (MpCore* c : all) { hosts += c->isHost() ? 1 : 0; act += c->active() ? 1 : 0; }
            if (hosts == 1 && act == 3 && all[0]->slotPresent(2) && all[2]->slotPresent(0)) break;
        }
        CHECK(hosts == 1 && act == 3 && all[0]->isHost() && all[1]->hostSlot() == 0);
        CHECK(LogCount("woke after") >= 1);
        Free(all);
    }
    // a link cut with the console running: 3 s (keeps everything), 8 s (the guest takes the host as lost, cannot take over: the other
    // guest still hears the host; it rejoins): either way it is back in the same session with the same host, nothing doubled
    for (int cut = 3000; cut <= 8000; cut += 5000) {
        MpLoopHub hub;
        hub.setClock(VNow, 0);
        hub.setFilter(CutFilter, 0);
        std::vector<MpCore*> all = StartSession(hub, 3, 1311 + (uint32_t)cut);
        all[0]->uiStart();
        Step(all, 300);
        Xo x(all);
        g_cutA = 1; g_cutB = 3;
        XoStep(x, all, cut, {0, 1, 2}, 100);
        g_cutA = 0;
        bool back = false;
        for (int t = 0; t < 30000 && !back; t += 100) {
            x.sendAll({0, 1, 2});
            Step(all, 100);
            x.drainAll();
            if (!all[2]->active() && Phase(all[2]) == MP_UI_IDLE) ScanJoin(all, all[2], 200);
            back = all[2]->active() && all[2]->hostSlot() == 0 && all[0]->slotPresent(2) && all[0]->isHost();
        }
        XoStep(x, all, 3000, {}, 100);
        if (!back || x.dups || x.gaps) fprintf(stderr, "  link cut %d ms: back=%d dups=%d gaps=%d memb=%u\n", cut, back, x.dups, x.gaps, x.memb[2]);
        CHECK(back && x.dups == 0 && x.gaps == 0 && x.tailMissing({0, 1}) == 0);
        if (cut == 3000) CHECK(x.memb[2] == 1);
        hub.setFilter(0, 0);
        Free(all);
    }
    printf("netcore liveness: frozen 6..14 s at 10 events/s per console: %d of %d keep their place, exactly once; frozen 16 s: removed, no "
           "takeover, back; link cut 3 / 8 s: back: ok\n", held, runs);
}

// A stranger's flood must not starve the members: the receive budget drops what is left over once it is spent, but a member's
// datagrams behind the flood are still handled (a second budget), so nobody is removed as silent and no event is lost.
static void TestFloodDoesNotStarveMembers() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 1301);
    all[0]->uiStart();
    Step(all, 300);
    Xo x(all);
    XoStep(x, all, 500, {0, 1});
    const MpAddr haddr = all[0]->debugLocalAddr();
    const uint32_t dropped0 = all[0]->debugRecvDropped();
    uint8_t junk[20] = {1, 2, 3};
    for (int t = 0; t < 20000; t += 5) {   // 20 s: longer than MP_PEER_LOST_MS
        if (t % 50 == 0) x.sendAll({0, 1});
        g_now += 5;
        all[0]->pump();
        // 400 datagrams from a stranger land in the host's socket AHEAD of what the guest sends in this pump (80,000 a second)
        for (int i = 0; i < 400; i++) hub.inject(MpAddr{7100, (uint16_t)MP_SESSION_PORT}, haddr, junk, sizeof junk);
        all[1]->pump();
        x.drain(0);
        x.drain(1);
    }
    const bool present = all[0]->slotPresent(1) && all[1]->active() && all[1]->hostSlot() == 0;
    XoStep(x, all, 3000, {0, 1});
    const int tail = x.tailMissing({0, 1});
    printf("flood vs members: 20 s of 80,000 stranger datagrams a second: guest present=%d, dups=%d gaps=%d tail=%d, %u pumps over budget\n", present, x.dups,
           x.gaps, tail, all[0]->debugRecvDropped() - dropped0);
    CHECK(all[0]->debugRecvDropped() - dropped0 > 3000);   // (the budget was spent in nearly every pump: the flood was real)
    CHECK(present);
    CHECK(x.dups == 0 && x.gaps == 0 && tail == 0);
    CHECK(x.memb[1] == 1);   // (the same membership throughout)
    Free(all);
    printf("flood vs members: ok\n");
}



// core2 (claims audit): across a rejoin. Guest B does not hear the host for a while; meanwhile guest A sends 10 events, leaves, comes
// back into the same slot (a new membership) and sends 5 more. B then gets A's first membership's 10 (late, in order, once) BEFORE
// any of the second's, then the second's from number 0: a late event of an earlier membership reaches the game only before the
// first event of the next one; after that the generation check drops it (TestExactlyOnceMissedRejoin covers the dropped case).
static void TestLateOldMembership() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    hub.setFilter(CutFilter, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 1401);
    all[0]->uiStart();
    Step(all, 300);
    Xo x(all);
    g_cutA = 1; g_cutB = 3;   // host <-> B (endpoint 3)
    for (int i = 0; i < 10; i++) { x.send(1); Step(all, 20); x.drainAll(); }
    all[1]->uiLeave();
    Step(all, 800);
    CHECK(ScanJoin(all, all[1]));
    CHECK(WaitPhase(all, all[1], MP_UI_PLAYING, 3000));
    x.drainAll();
    for (int i = 0; i < 5; i++) { x.send(1); Step(all, 20); x.drainAll(); }
    g_cutA = 0;
    std::vector<std::pair<uint32_t, uint32_t>> seen;   // (A's membership, counter) as B's game got them
    for (int t = 0; t < 4000; t += 20) {
        Step(all, 20);
        MpMsg m;
        while (all[2]->poll(&m))
            if (m.type == MP_MSG_KILL && m.len == 9 && m.data[0] == 1) {
                uint32_t mb, c;
                memcpy(&mb, m.data + 1, 4);
                memcpy(&c, m.data + 5, 4);
                seen.push_back({mb, c});
                CHECK(m.fromSlot == 1);
            }
        for (int k = 0; k < 2; k++) x.drain(k);
    }
    bool ordered = seen.size() == 15;
    for (size_t i = 0; i < seen.size() && ordered; i++)
        ordered = i < 10 ? (seen[i].first == 1 && seen[i].second == i) : (seen[i].first == 2 && seen[i].second == i - 10);
    if (!ordered) {
        fprintf(stderr, "  late old membership: B got %zu:", seen.size());
        for (auto& p : seen) fprintf(stderr, " %u/%u", p.first, p.second);
        fprintf(stderr, "\n");
    }
    CHECK(ordered);
    hub.setFilter(0, 0);
    Free(all);
    printf("rejoin: a late tail of the old membership comes first, in order, once; the new one from 0: ok\n");
}

// core2 (stage 2, review finding 11): a scanned room keeps the address it was first seen at while that address beacons, however many
// forged beacons of its session come from elsewhere; one address lists at most MP_LAN_ROOMS_PER_ADDR rooms.
static void TestScanHardening() {
    MpLanScan scan;
    MpBeacon b;
    memset(&b, 0, sizeof b);
    b.proto = MP_PROTO_VERSION; b.fingerprint = 123; b.mode = MP_MODE_TOGETHER; b.sessionId = 77; b.players = 1; b.flags = 1;
    const MpAddr real{5, MP_SESSION_PORT}, evil{9, MP_SESSION_PORT};
    uint32_t seq = 1, evilSeq = 1000;
    for (uint64_t t = 1000; t < 20000; t += 100) {
        if (t % 500 == 0) { b.seq = seq++; b.epoch = 0; scan.add(real, b, t, 123); }
        b.seq = evilSeq++; b.epoch = 9; scan.add(evil, b, t, 123);   // forged, faster, a higher epoch
    }
    MpLocalRoom room;
    MpAddr got;
    uint32_t sid;
    CHECK(scan.count() == 1 && scan.get(0, &room, &got, &sid) && got.host == 5);
    for (uint32_t i = 0; i < 10; i++) { b.sessionId = 1000 + i; b.seq = 1; scan.add(evil, b, 30000, 123); }
    int fromEvil = 0;
    for (int i = 0; i < scan.count(); i++)
        if (scan.get(i, &room, &got, &sid) && got.host == 9) fromEvil++;
    CHECK(fromEvil == MP_LAN_ROOMS_PER_ADDR);
    printf("scan: a beaconing room keeps its address against forged beacons; one address lists at most %d rooms: ok\n", MP_LAN_ROOMS_PER_ADDR);
}

// core2: friend codes in the roster. They come from each console's own room view at the service (the hub stands in for it here),
// shown for another present player only when the view lists that slot at the player's address; never for a LAN / ad hoc / by address
// room, never for this console's own slot; they follow the roster (gone when the player leaves; a rejoin is shown once re-listed).
static void TestFriendCodes() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 1301);   // slots 0, 1, 2 on endpoints 1, 2, 3
    Step(all, 300);
    char c[16];
    for (MpCore* x : all)
        for (int s = 0; s < 4; s++) CHECK(!x->slotFriendCode(s, c, sizeof c) && c[0] == 0);   // the service has said nothing yet
    hub.setMemberCode(0, 1, "K7QM-4DXT");
    hub.setMemberCode(1, 2, "PQ2R-9HXA");
    hub.setMemberCode(2, 9, "M4TT-ZE7B");   // the view lists slot 2 at another address than the roster's: not shown
    Step(all, 50);
    CHECK(all[1]->slotFriendCode(0, c, sizeof c) && !strcmp(c, "K7QM-4DXT"));
    CHECK(all[2]->slotFriendCode(1, c, sizeof c) && !strcmp(c, "PQ2R-9HXA"));
    CHECK(all[0]->slotFriendCode(1, c, sizeof c) && !strcmp(c, "PQ2R-9HXA"));
    CHECK(!all[1]->slotFriendCode(1, c, sizeof c));   // own slot
    CHECK(!all[0]->slotFriendCode(2, c, sizeof c) && !all[1]->slotFriendCode(2, c, sizeof c));
    CHECK(!all[0]->slotFriendCode(3, c, sizeof c) && !all[0]->slotFriendCode(-1, c, sizeof c) && !all[0]->slotFriendCode(9, c, sizeof c));
    CHECK(!all[1]->slotFriendCode(0, c, 9) && c[0] == 0);   // (too small a buffer)
    hub.setMemberCode(2, 3, "M4TT-ZE7B");
    hub.setMemberCode(1, 2, "pq2r-9hxa");   // malformed (lower case): never shown
    Step(all, 50);
    CHECK(all[0]->slotFriendCode(2, c, sizeof c) && !strcmp(c, "M4TT-ZE7B"));
    CHECK(!all[0]->slotFriendCode(1, c, sizeof c));
    hub.setMemberCode(1, 2, "PQ2R-9HXA");
    // slot 2 leaves: its code goes with it (also while the view still lists it)
    all[2]->uiLeave();
    Step(all, 1500);
    CHECK(!all[0]->slotPresent(2) && !all[0]->slotFriendCode(2, c, sizeof c) && !all[1]->slotFriendCode(2, c, sizeof c));
    CHECK(all[1]->slotFriendCode(0, c, sizeof c));
    // the codes never appear in the log
    CHECK(LogCount("K7QM-4DXT") == 0 && LogCount("PQ2R-9HXA") == 0 && LogCount("M4TT-ZE7B") == 0);
    Free(all);
    printf("friend codes: from the service's view, bound to slot and address, own slot / gone players / malformed codes never: ok\n");
}

// svcfix3: two players with the same name are told apart by the service's label ("Luma#4DXT"). Another player's name is shown as the label when the
// view lists the slot at the player's address (as for the codes); the own slot, a wrong address, a label with control characters, a view without
// labels (an older service) and a player who left all show the plain name.
static void TestSharedNameLabels() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 1311);
    Step(all, 300);
    char plain0[17], plain1[17], n[17];
    all[1]->slotName(0, plain0);
    all[0]->slotName(1, plain1);
    CHECK(plain0[0] && plain1[0] && !strchr(plain0, '#'));
    hub.setMemberLabel(0, 1, "Luma#4DXT");
    hub.setMemberLabel(1, 2, "Luma#9HXA");
    hub.setMemberLabel(2, 9, "Evil#0000");   // listed at another address than the roster's: not shown
    Step(all, 50);
    all[1]->slotName(0, n); CHECK(!strcmp(n, "Luma#4DXT"));
    all[2]->slotName(0, n); CHECK(!strcmp(n, "Luma#4DXT"));
    all[0]->slotName(1, n); CHECK(!strcmp(n, "Luma#9HXA"));
    all[1]->slotName(1, n); CHECK(!strcmp(n, plain1));   // (the own slot: the name itself)
    all[0]->slotName(2, n); CHECK(strcmp(n, "Evil#0000") != 0 && n[0]);
    hub.setMemberLabel(0, 1, "A\tB#1234");   // a control character: never shown
    Step(all, 50);
    all[1]->slotName(0, n); CHECK(!strcmp(n, plain0));
    hub.setMemberLabel(0, 1, nullptr);         // an older service: no labels
    hub.setMemberLabel(1, 2, nullptr);
    Step(all, 50);
    all[1]->slotName(0, n); CHECK(!strcmp(n, plain0));
    all[0]->slotName(1, n); CHECK(!strcmp(n, plain1));
    hub.setMemberLabel(0, 1, "Luma#4DXT");
    all[2]->uiLeave();   // slot 2 leaves: the labels of the others stay, nobody shows a stale one for it
    Step(all, 1500);
    all[1]->slotName(0, n); CHECK(!strcmp(n, "Luma#4DXT"));
    CHECK(LogCount("Luma#4DXT") == 0);   // (names of players are not logged by the label code)
    Free(all);
    printf("shared names: the service's label for another player, bound to slot and address, never the own slot / bad labels: ok\n");
}

#include "rev_sec_tests.h"   // outside security review

#include "core2_sec_tests.h"
#include "netfix_tests.h"
#include "mute_id_tests.h"   // smallfix2 A

#include "core3_tests.h"   // corefix2

// wave8: MpCore::pollStates() extracts MP_MSG_STATE packets early (pre-update) while preserving exact FIFO order of all
// non-STATE messages for mp_poll_many().
static void TestPollStatesPreservesFifo() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 718);
    all[0]->uiStart();
    Step(all, 300);
    // Drain any join/start messages first
    MpMsg tmp[8];
    while (all[1]->pollMany(tmp, 8) > 0) {}

    // Interleave reliable non-STATE events and unreliable STATE packets from host -> guest
    uint8_t k0[4] = {1, 0, 0, 0};
    uint8_t k1[4] = {2, 0, 0, 0};
    uint8_t k2[4] = {3, 0, 0, 0};
    uint8_t stA[8] = {0xAA, 1, 2, 3, 4, 5, 6, 7};
    uint8_t stB[8] = {0xBB, 8, 9, 10, 11, 12, 13, 14};
    CHECK(all[0]->send(MP_MSG_KILL, k0, 4, true));
    CHECK(all[0]->send(MP_MSG_STATE, stA, 8, false));
    CHECK(all[0]->send(MP_MSG_STAR, k1, 4, true));
    CHECK(all[0]->send(MP_MSG_STATE, stB, 8, false));
    CHECK(all[0]->send(MP_MSG_KILL, k2, 4, true));
    Step(all, 80);

    // 1. pollStates() drains ONLY STATE messages (latest STATE replaces older STATE from same slot, so >=1 STATE)
    MpMsg states[8];
    const uint32_t ns = all[1]->pollStates(states, 8);
    CHECK(ns >= 1);
    for (uint32_t i = 0; i < ns; i++) CHECK(states[i].type == MP_MSG_STATE);
    CHECK(states[ns - 1].data[0] == 0xBB);
    // Second call returns 0
    CHECK(all[1]->pollStates(states, 8) == 0);

    // 2. pollMany() returns all three non-STATE messages in exact FIFO order (KILL 1 -> STAR 2 -> KILL 3)
    MpMsg rest[8];
    const uint32_t nr = all[1]->pollMany(rest, 8);
    CHECK(nr == 3);
    CHECK(rest[0].type == MP_MSG_KILL && rest[0].data[0] == 1);
    CHECK(rest[1].type == MP_MSG_STAR && rest[1].data[0] == 2);
    CHECK(rest[2].type == MP_MSG_KILL && rest[2].data[0] == 3);
    CHECK(all[1]->pollMany(rest, 8) == 0);
    Free(all);
    printf("pollStates extracts STATE early and preserves FIFO of other messages: ok\n");
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    g_echo = argc > 1 && !strcmp(argv[1], "-v");
    const char* only = getenv("MP_ONLY");   // MP_ONLY=Netpack runs the tests whose name contains it
#define RUN(f) do { if (!only || strstr(#f, only)) f(); } while (0)
    if (getenv("MP_BACKOFF_SHIFT")) MpReliable::setMaxBackoffShift(atoi(getenv("MP_BACKOFF_SHIFT")));   // 1 = 2x, 2 = 4x (default), 3 = 8x, 4 = 16x
    rel_test::Run();
    RUN(TestConfig);
    RUN(TestBasic);
    RUN(TestGoodbyeOnQuit);
    RUN(TestClockReadsWithinAPump);
    RUN(TestReliableE2E);
    RUN(TestFailurePaths);
    RUN(TestLinkLostWindowOverride);
    RUN(TestRateCap);
    RUN(TestStallBacklog);
    RUN(TestSnapshot);
    RUN(TestQueueOverflow);
    RUN(TestLongLoad);
    RUN(TestNoAckRemovalTimes);
    RUN(TestRejoin);
    RUN(TestSameNameNoSteal);
    RUN(TestMuteIdStableAcrossRejoin);
    RUN(TestGhostJoiners);
    RUN(TestForgedMergeBeacon);
    RUN(TestSessionIds);
    RUN(TestNames);
    RUN(TestMigrationGraceful);
    RUN(TestMigrationDrop);
    RUN(TestMigrationDuringSnapshot);
    RUN(TestMigrationDedupe);
    RUN(TestMigrationDeadHostEvents);
    RUN(TestExactlyOnceDoubleClaim);
    RUN(TestExactlyOnceMissedRejoin);
    RUN(TestExactlyOnceUnservable);
    RUN(TestSplitHealRosterRemoval);
    RUN(TestHostUsurping);
    RUN(TestMigrationDoubleClaim);
    RUN(TestClaimWithJoiner);
    RUN(TestStaleWelcome);
    RUN(TestSplitHeal);
    RUN(TestReconnect);
    RUN(TestLinkOutage);
    RUN(TestFrozenConsole);
    RUN(TestBadKickBanAndIdSpoof);
    RUN(TestProofFailsPerSource);
    RUN(TestHelloReplayCost);
    RUN(TestSnapshotReadNoLock);
    RUN(TestTakeoverNoWindow);   // corefix2
    RUN(TestBanSurvivesTakeover);
    RUN(TestReservationHeld);
    RUN(TestHello1FloodFair);
    RUN(TestProofCeilingFair);
    RUN(TestDoubleTakeover);
    RUN(TestTakeoverDuringSnapshot);
    RUN(TestHeartbeatPiggyback);
    RUN(TestPollMany);
    RUN(TestLazyWindows);
    RUN(TestStateSceneFilter);
    RUN(TestJoinByRoomId);
    RUN(TestProbeDiscovery);
    RUN(TestKickByConsole);
    RUN(TestKickRing);
    RUN(TestBeaconTrustAndRandom);
    RUN(TestKickAfterMigration);
    RUN(TestVersionRefusal);
    RUN(TestDroppedFromRoom);
    RUN(TestLooks);
    RUN(TestCoopMessages);
    RUN(TestBubble);
    RUN(TestNetpackBundles);
    RUN(TestNetpackBackoff);
    RUN(TestNetpackFlood);
    RUN(TestNetpackMemory);
    RUN(TestNetpackCreep);
    RUN(TestNetcoreBundleReview);
    RUN(TestNetcoreKeepAwakeLiveness);
    RUN(TestNetcoreBackoffBounds);
    RUN(TestNetcoreFrozenGuest);
    RUN(TestFriendCodes);
    RUN(TestSharedNameLabels);
    RUN(TestScanHardening);
    RUN(TestLateOldMembership);
    RUN(TestFloodDoesNotStarveMembers);
    RUN(TestSoak);
    RUN(TestCore2Sec);
    RUN(TestRevSecVLinkIdentity);
    RUN(TestRevSecSpoofedRejectEndsGuest);
    RUN(TestRevSecSpoofedJunkKicksGuest);
    RUN(TestRevSecUnreliableEvents);
    RUN(TestRevSecPoseFreeze);
    RUN(TestRevSecForgedAck);
    RUN(TestRevSecHostQueueFlood);
    RUN(TestNetfixStaleHeldSlot);
    RUN(TestNetfixFragmentSlots);
    RUN(TestNetfixPosesFillQueue);
    RUN(TestNetfixBigBlocks);
    RUN(TestNetfixHostLoad10);
    RUN(TestNetfixHostLoad40);
    RUN(TestPollStatesPreservesFifo);
    if (g_fail) {
        printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
