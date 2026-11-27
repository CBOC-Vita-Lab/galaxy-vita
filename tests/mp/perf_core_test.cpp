// Galaxy 3.0 multiplayer: host tests for the session layer's game-thread costs (owner: perf).
//   - the bridge's queries (mode, slots, names, settings, ui status, save policy) never take the session lock: they return at once
//     while another thread holds it (as the helper thread does during a pump);
//   - poll() returns at once when nothing is queued or the lock is busy (no wait), and delivers in order once it is free;
//   - while the helper thread pumps, an unreliable send (STATE) is queued without the lock and goes out at the next pump; tick()
//     does no network work; the queue holds MP_UOUT_SLOTS sends and refuses the next one;
//   - the published view follows the session (a guest leaving, settings).
// Build + run: tests/mp/perf_build.sh (clang++/g++ -std=c++20 -fsanitize=address,undefined).
#define private public   // (test access to the lock and the helper-thread flag)
#include "core_test_util.h"
#undef private

#include <atomic>
#include <chrono>
#include <thread>

static std::vector<MpCore*> StartSession(MpLoopHub& hub, int n, uint32_t seed) {
    std::vector<MpCore*> all;
    const char* names[4] = {"Player", "Luigi", "Peach", "Toad"};
    for (int i = 0; i < n; i++) all.push_back(MakeCore(hub, (uint32_t)(i + 1), names[i], seed + (uint32_t)i * 7919u));
    MpHostParams hp = HostParams(MP_MODE_TOGETHER);
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

static double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Holds a core's session lock on another thread for `ms` (a slow pump on the helper thread).
struct LockHolder {
    std::atomic<bool> held{false};
    std::thread t;
    LockHolder(MpCore* c, int ms) {
        t = std::thread([this, c, ms] {
            c->m_mutex.lock();
            held = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            c->m_mutex.unlock();
        });
        while (!held) std::this_thread::yield();
    }
    ~LockHolder() { t.join(); }
};

// The helper thread "runs" (the tests pump by hand instead): the game-thread paths take the threaded branch.
static void FakeThread(MpCore* c, bool on) {
    c->m_mutex.lock();
    c->m_threadAlive = on;
    c->m_threadExiting = false;
    c->publishView();
    c->m_mutex.unlock();
}

static void SendPose(MpCore* c, float x, bool* ok) {
    MpPose pose;
    memset(&pose, 0, sizeof pose);
    pose.pos[0] = x;
    uint8_t pb[64];
    MpWriter w(pb, sizeof pb);
    mp_write_pose(w, pose);
    *ok = c->send(MP_MSG_STATE, pb, (uint16_t)w.n, false);
}

// 1. queries never wait for the lock
static void TestQueriesLockFree() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 31);
    MpCore* h = all[0];
    MpCore* g = all[1];
    CHECK(h->uiStart());
    Step(all, 300);
    {
        LockHolder hold(g, 300);
        const auto t0 = std::chrono::steady_clock::now();
        int sum = 0;
        for (int i = 0; i < 1000; i++) {
            sum += (int)g->mode() + (g->isHost() ? 1 : 0) + g->localSlot() + g->hostSlot() + g->hostEpoch() + (g->slotPresent(0) ? 1 : 0);
            char name[MP_NAME_BYTES + 1];
            g->slotName(0, name);
            sum += name[0] + (int)g->slotLook(0) + g->slotPing(0) + (g->slotQuiet(0) ? 1 : 0) + g->sceneSerial();
            MpSettings s;
            g->settings(&s);
            MpUiStatus st;
            g->uiStatus(&st);
            sum += s.maxPlayers + st.players + (g->saveSuppressed() ? 1 : 0) + (g->saveAutoconfirm() ? 1 : 0);
        }
        const double ms = MsSince(t0);
        CHECK(ms < 100.0);   // 1000 rounds of every query while the lock is held for 300 ms
        CHECK(sum != 0);
        printf("  queries: 1000 rounds in %.2f ms with the lock held elsewhere\n", ms);
    }
    // the values are those of the session
    CHECK(g->mode() == MP_MODE_TOGETHER && !g->isHost() && g->localSlot() == 1 && g->hostSlot() == 0);
    char name[MP_NAME_BYTES + 1];
    g->slotName(0, name);
    CHECK(!strcmp(name, "Player"));
    MpUiStatus st;
    g->uiStatus(&st);
    CHECK(st.phase == MP_UI_PLAYING && st.players == 2);
    CHECK(g->saveSuppressed() && h->saveAutoconfirm());
    // the view follows a guest leaving
    CHECK(h->slotPresent(1));
    g->uiLeave();
    Step(all, 1000);
    CHECK(!h->slotPresent(1) && h->active());
    h->uiStatus(&st);
    CHECK(st.players == 1);
    CHECK(!g->active() && g->localSlot() == -1 && g->mode() == MP_MODE_NONE);
    Free(all);
    printf("queries lock-free: ok\n");
}

// 2. poll: no wait when empty or busy; in-order delivery afterwards
static void TestPollNoWait() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 41);
    MpCore* h = all[0];
    MpCore* g = all[1];
    CHECK(h->uiStart());
    Step(all, 300);
    MpMsg m;
    while (h->poll(&m)) {}
    {
        LockHolder hold(h, 200);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!h->poll(&m));   // empty: no lock taken
        CHECK(MsSince(t0) < 50.0);
    }
    for (int i = 0; i < 5; i++) {
        uint8_t b[4] = {(uint8_t)i, 0, 0, 0};
        CHECK(g->send(MP_MSG_STAR, b, 4, true));
    }
    Step(all, 200);
    {
        LockHolder hold(h, 200);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!h->poll(&m));   // queued, but the lock is busy: try next turn
        CHECK(MsSince(t0) < 50.0);
    }
    int got = 0;
    while (h->poll(&m))
        if (m.type == MP_MSG_STAR && m.fromSlot == 1) CHECK(m.data[0] == got++);
    CHECK(got == 5);
    Free(all);
    printf("poll no-wait: ok\n");
}

// 3. helper thread running: unreliable sends are queued (no lock), tick does no network work
static void TestUnreliableRing() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 3, 51);
    MpCore* h = all[0];
    MpCore* g = all[1];
    CHECK(h->uiStart());
    Step(all, 300);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    // pretend both run their helper thread (the tests pump by hand instead)
    for (MpCore* c : all) FakeThread(c, true);
    {
        LockHolder hold(g, 300);
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        SendPose(g, 1.0f, &ok);
        CHECK(ok);
        g->tick(1);   // returns at once: the helper pumps
        CHECK(MsSince(t0) < 50.0);
    }
    // nothing went out yet: the host sees it after the guest's next pump
    h->pump();
    CHECK(!h->poll(&m));
    Step(all, 50);
    bool got = false;
    while (h->poll(&m))
        if (m.type == MP_MSG_STATE && m.fromSlot == 1) got = true;
    CHECK(got);
    // and the host relays it to the third player
    got = false;
    for (MpCore* c3 = all[2]; c3->poll(&m);)
        if (m.type == MP_MSG_STATE && m.fromSlot == 1) got = true;
    CHECK(got);
    // the host's own unreliable sends reach both guests
    bool ok = false;
    SendPose(h, 2.0f, &ok);
    CHECK(ok);
    Step(all, 50);
    for (int k = 1; k <= 2; k++) {
        got = false;
        while (all[k]->poll(&m))
            if (m.type == MP_MSG_STATE && m.fromSlot == 0) got = true;
        CHECK(got);
    }
    // a full queue refuses (the helper thread stalled): MP_UOUT_SLOTS fit
    for (int i = 0; i < MP_UOUT_SLOTS; i++) {
        SendPose(g, 3.0f + i, &ok);
        CHECK(ok);
    }
    SendPose(g, 99.0f, &ok);
    CHECK(!ok);
    Step(all, 50);
    int n = 0;
    while (h->poll(&m))
        if (m.type == MP_MSG_STATE && m.fromSlot == 1) n++;
    CHECK(n == MP_UOUT_SLOTS);
    // validation without the lock: unknown types and host-only types from a guest are refused
    uint8_t b[4] = {0};
    CHECK(!g->send(0x5E, b, 4, false));
    CHECK(!g->send(MP_MSG_WHERE, b, 4, false));
    // a session that ends drops what was queued
    SendPose(g, 5.0f, &ok);
    CHECK(ok);
    g->uiLeave();
    Step(all, 1000);
    for (MpCore* c : all) FakeThread(c, false);
    CHECK(g->m_uHead == g->m_uTail);
    SendPose(g, 6.0f, &ok);
    CHECK(!ok);   // inactive
    Free(all);
    printf("unreliable ring: ok\n");
}

// 3b. MP3 nocap: the SMG_MP_PERFLOG line's send counters (mp_core_send_take): what the helper put on the wire, how long each send
// waited in the ring (queue to drain) and how many the full ring refused. With the fourth core or without, the 30 Hz STATE pacing
// is the game thread's (MpSendGate); what the core layout can change is this wait, so it is what the console line reports.
extern "C" void mp_core_helper_take(uint32_t* busyUs, uint32_t* wakes, uint32_t* maxUs);
extern "C" void mp_core_send_take(uint32_t* sent, uint32_t* refused, uint32_t* lagSumUs, uint32_t* lagMaxUs);
static void TestSendCounters() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 61);
    MpCore* g = all[1];
    CHECK(all[0]->uiStart());
    Step(all, 300);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    for (MpCore* c : all) FakeThread(c, true);
    uint32_t a = 0, b = 0, c3 = 0, d = 0;
    mp_core_helper_take(&a, &b, &c3);   // (the measurement starts here, as at the first perf line)
    mp_core_send_take(&a, &b, &c3, &d);  // (and what came before it is dropped)
    bool ok = false;
    for (int i = 0; i < MP_UOUT_SLOTS; i++) {
        SendPose(g, 10.0f + i, &ok);
        CHECK(ok);
    }
    SendPose(g, 99.0f, &ok);   // the ring is full: refused, and counted
    CHECK(!ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the helper is late by 20 ms
    g->pump();
    uint32_t sent = 0, refused = 0, lagSum = 0, lagMax = 0;
    mp_core_send_take(&sent, &refused, &lagSum, &lagMax);
    CHECK(sent == MP_UOUT_SLOTS);
    CHECK(refused == 1);
    CHECK(lagMax >= 15000 && lagMax < 5000000);
    CHECK(lagSum >= (uint32_t)MP_UOUT_SLOTS * 15000u);
    // taken: the next window starts at zero
    mp_core_send_take(&sent, &refused, &lagSum, &lagMax);
    CHECK(sent == 0 && refused == 0 && lagSum == 0 && lagMax == 0);
    for (MpCore* c : all) FakeThread(c, false);
    Free(all);
    printf("send counters: ok\n");
}

// 4. (tsan) real threads: the game thread sends STATE and runs every lock-free query while another thread pumps both cores (the
// helper thread's job). Under ThreadSanitizer (tests/mp/tsan_build.sh) this exercises the view seqlock and the STATE ring.
static void TestThreadedStress() {
    MpLoopHub hub;
    hub.setClock(VNow, 0);
    std::vector<MpCore*> all = StartSession(hub, 2, 77);
    MpCore* h = all[0];
    MpCore* g = all[1];
    CHECK(h->uiStart());
    Step(all, 300);
    MpMsg m;
    for (MpCore* c : all) while (c->poll(&m)) {}
    FakeThread(g, true);
    FakeThread(h, true);
    std::atomic<bool> stop{false};
    std::thread pumper([&] {   // (the clock is only read here: g_now does not move)
        while (!stop.load()) {
            g->pump();
            h->pump();
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    long sent = 0, refused = 0;
    while (MsSince(t0) < 1500.0) {
        bool ok = false;
        if (sent < 30) {   // (the clock stands still: the host's per-peer rate limit would kick a faster sender)
            SendPose(g, (float)sent, &ok);
            if (ok) sent++; else refused++;
        }
        g->tick((uint32_t)sent);
        MpSettings s;
        g->settings(&s);
        MpUiStatus st;
        g->uiStatus(&st);
        char name[MP_NAME_BYTES + 1];
        g->slotName(0, name);
        CHECK(g->active());
        (void)g->mode(); (void)g->slotLook(0); (void)g->slotPing(1); (void)g->sceneSerial(); (void)g->saveSuppressed();
        while (g->poll(&m)) {}
    }
    stop.store(true);
    pumper.join();
    CHECK(sent > 0);
    for (MpCore* c : all) FakeThread(c, false);
    Free(all);
    printf("threaded stress: ok (%ld sends, %ld refused)\n", sent, refused);
}

int main(int argc, char** argv) {
    mp_log_set_sink(LogSink, true);
    g_echo = argc > 1 && !strcmp(argv[1], "-v");
    TestQueriesLockFree();
    TestPollNoWait();
    TestUnreliableRing();
    TestSendCounters();
    TestThreadedStress();
    if (g_fail) {
        printf("FAILED: %d checks\n", g_fail);
        return 1;
    }
    printf("all perf core tests passed\n");
    return 0;
}
