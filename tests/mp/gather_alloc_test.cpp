// core2: the crash of flow's round 3 (door_shots-030805: SIGSEGV in MpCore::pumpConnector right after the leader's far-away code
// switch with a friend in the room). Cause: the gathering C API (port/mp_gather.h, called on the GAME thread by the menu) did not open
// the host-allocation scope (MP_HOST_ALLOC) that every other bridge call opens. In the game a game-thread allocation outside that scope
// goes to the game's heap; the by-address connector (and the gathering driver itself) were made there, and a scene's heap reset
// freed them under the network thread. Here: the global core on a loopback hub, the real C API, real time; every allocation made
// inside a C API call outside the scope is counted (the game heap would have got it) and must be 0. The steps are the crash's:
// gather, a friend joins, the leader asks for the code, the room switches, the friend follows, Start.
//   built by bcore.sh like core_tests (target gather_alloc_test); quick
#include "core_test_util.h"
#include "../../platform/src/mp/mp_gather.h"
#include "port/mp_gather.h"

#include <new>
#include <unistd.h>

static thread_local int t_scope = 0;   // port_host_alloc_scope_enter / _leave as the port implements them (os_thread.cpp)
static thread_local bool g_inApi = false;   // this (game) thread is inside a C API call; other threads allocate on the host heap anyway
static int g_gameHeapAllocs = 0;
extern "C" void port_host_alloc_scope_enter(void) { t_scope++; }
extern "C" void port_host_alloc_scope_leave(void) { t_scope--; }
void* operator new(size_t n) {
    if (g_inApi && t_scope == 0) g_gameHeapAllocs++;
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

static uint64_t RealNow(void*) { return mp_sys_now_ms(); }
template <class F> static auto Api(F f) {   // a C API call as the menu makes it, on the "game thread"
    g_inApi = true;
    auto r = f();
    g_inApi = false;
    return r;
}

int main() {
    mp_log_set_sink(LogSink, true);
    // (default: the global core is pumped here, between the calls; GATHER_ALLOC_THREAD=1: its own network thread pumps it as in the game,
    // so the switch happens while the connector is mid-pump: run that under TSan)
    if (!getenv("GATHER_ALLOC_THREAD")) setenv("SMG_MP_NO_THREAD", "1", 1);
    MpLoopHub& hub = *new MpLoopHub();   // (outlives the global core's network thread: never destroyed, the run ends with _exit)
    hub.setClock(RealNow, 0);
    MpCore* leader = mp_core_global();
    leader->setLoopback(&hub, 1);
    leader->setProfile("Ann", MP_LOOK_MARIO);
    MpCoreOptions o;
    memset(&o, 0, sizeof o);
    o.useThread = false;
    o.seed = 77;
    MpCore* friendCore = new MpCore(o);
    friendCore->setLoopback(&hub, 2);
    friendCore->setProfile("Bob", MP_LOOK_LUIGI);
    MpGather friendGather(friendCore);
    auto pumpAll = [&](int ms) {
        for (int t = 0; t < ms; t += 5) {
            leader->pump();
            friendCore->pump();
            friendGather.tick(mp_sys_now_ms());
            MpGatherStatus st;
            Api([&] { mp_gather_status(&st); return 0; });
            usleep(5000);
        }
    };
    CHECK(Api([] { return mp_gather_start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK); }));
    pumpAll(2500);   // the leader's own room
    CHECK(friendGather.start(MP_MODE_TOGETHER, MP_CONN_LOOPBACK, mp_sys_now_ms()));
    bool two = false;
    for (int i = 0; i < 400 && !two; i++) {
        pumpAll(25);
        MpGatherStatus st;
        Api([&] { mp_gather_status(&st); return 0; });
        two = st.state == MP_GATHER_GATHERING && st.headCount == 2 && Api([] { return mp_gather_code_available(); });
    }
    CHECK(two);
    // the code: the room switches to Play by Address while the friend is in it
    char code[MP_DIRECT_CODE_MAX] = {0};
    bool got = Api([&] { return mp_gather_get_code(code, sizeof code); });
    for (int i = 0; i < 400 && !got; i++) {
        pumpAll(25);
        got = Api([&] { return mp_gather_get_code(code, sizeof code); });
    }
    CHECK(got && strlen(code) >= 15);
    bool back = false;
    for (int i = 0; i < 600 && !back; i++) {
        pumpAll(25);
        MpGatherStatus st;
        Api([&] { mp_gather_status(&st); return 0; });
        back = st.headCount == 2 && friendCore->active() && leader->slotPresent(friendCore->localSlot());
    }
    CHECK(back);
    bool canStart = false;
    for (int i = 0; i < 200 && !canStart; i++) {   // (Start shows once the room is quiet: 1 s after it opened, longer if others are heard)
        pumpAll(25);
        MpGatherStatus st;
        Api([&] { mp_gather_status(&st); return 0; });
        canStart = st.canStart;
        if (i == 199) printf("  status: state=%d heads=%d canStart=%d rooms=%d active=%d host=%d\n", (int)st.state, st.headCount, st.canStart, st.roomsSeen, leader->active(), leader->isHost());
    }
    CHECK(canStart);
    CHECK(Api([] { return mp_gather_begin(); }));
    pumpAll(500);
    Api([] { mp_gather_stop(); return 0; });
    pumpAll(200);
    printf("gather alloc: %d allocation(s) inside the gathering C API outside the host-allocation scope (the game heap)\n", g_gameHeapAllocs);
    CHECK(g_gameHeapAllocs == 0);
    friendGather.stop();
    printf("%s\n", g_fail ? "gather alloc test FAILED" : "gather alloc test: ok");
    fflush(stdout);
    fflush(stderr);
    _exit(g_fail ? 1 : 0);   // (the global core lives for the process, as in the game)
}
