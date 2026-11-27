// Galaxy 3.0 multiplayer: a lightweight cost counter for the multiplayer code on the game thread (owner: perf).
//
// Compiled in only with -DSMG_MP_PROF=1 (cmake -DSMG_MP_PROF=ON). Without it every macro below is empty and nothing is linked.
// With it, MP_PROF_SCOPE(zone) times a block on the game thread and MP_PROF_TURN() closes a logic turn; every N turns
// (env SMG_MP_PROF=N, default 600 = 10 s) one line goes to the log:
//   [mp] prof turns=600 active=600 total avg=0.123ms max=0.456ms | net 0.010/0.050 | puppets ... | lockwait ...
// (per zone: average / maximum milliseconds per turn; zones nest, each is inclusive). Single-player runs report too, so the
// cost of the inactive hooks can be read off the same line.
#ifndef PORT_MP_PROF_H
#define PORT_MP_PROF_H

#include <stdint.h>

enum MpProfZone {
    MP_PROF_TOTAL = 0,      // every game-side hook (MpGame::onGameSceneUpdate, pumpSystem, MpUi update/draw)
    MP_PROF_NET,            // bridge calls into the session layer from the game thread (tick, poll, send)
    MP_PROF_PUPPET_UPDATE,  // MpPuppets::update + STATE handling (send, lifecycle, name tags)
    MP_PROF_PUPPET_CALC,    // puppet movement/calcAnim (pose sample, Xanime, joints)
    MP_PROF_PUPPET_DRAW,    // MpPuppetDrawer::draw
    MP_PROF_WORLD,          // MpWorldSync::update + handle
    MP_PROF_FLOW,           // MpFlow::update + handle
    MP_PROF_UI_UPDATE,      // MpUi::updateSystem
    MP_PROF_UI_DRAW,        // MpUi::drawSystem
    MP_PROF_LOCKWAIT,       // the game thread waiting for the session lock
    MP_PROF_ZONES
};

#if defined(SMG_MP_PROF) && SMG_MP_PROF
extern "C" {
uint64_t mp_prof_now_us(void);
void mp_prof_add(int zone, uint64_t us);
void mp_prof_turn(void);              // a logic turn ended (the report every N turns)
void mp_prof_count(int counter);      // 0: game-thread lock acquisitions, 1: STATE sent, 2: puppet calcAnim run, 3: skipped
int mp_prof_is_game_thread(void);     // the thread that closes the turns (MP_PROF_TURN)
}
struct MpProfScope {
    int zone;
    uint64_t t0;
    explicit MpProfScope(int z) : zone(z), t0(mp_prof_now_us()) {}
    ~MpProfScope() { mp_prof_add(zone, mp_prof_now_us() - t0); }
};
#define MP_PROF_CAT2(a, b) a##b
#define MP_PROF_CAT(a, b) MP_PROF_CAT2(a, b)
#define MP_PROF_SCOPE(z) MpProfScope MP_PROF_CAT(mpProfScope_, __LINE__)(z)
#define MP_PROF_TURN() mp_prof_turn()
#define MP_PROF_COUNT(c) mp_prof_count(c)
#else
// MP3 perf3: without SMG_MP_PROF, three zones (total, puppet calc, puppet draw) are still timed when the env switch
// SMG_MP_PERFLOG=1 is set (env.txt on the console): MpQuality.cpp prints "[mp] perf ..." every ~10 s. Switch off (the default):
// a scope is one load of mp_perf_on and a branch; the other zones compile to nothing (no symbol: the host tests link without
// mp_prof.cpp).
extern "C" {
extern int mp_perf_on;
uint64_t mp_prof_now_us(void);
void mp_perf_add(int zone, uint32_t us);
}
template <int Z>
struct MpPerfScope {
    static constexpr bool kTimed = Z == MP_PROF_TOTAL || Z == MP_PROF_PUPPET_CALC || Z == MP_PROF_PUPPET_DRAW;
    uint64_t t0 = 0;
    MpPerfScope() {
        if constexpr (kTimed) {
            if (mp_perf_on) t0 = mp_prof_now_us();
        }
    }
    ~MpPerfScope() {
        if constexpr (kTimed) {
            if (t0) mp_perf_add(Z, (uint32_t)(mp_prof_now_us() - t0));
        }
    }
};
#define MP_PROF_CAT2(a, b) a##b
#define MP_PROF_CAT(a, b) MP_PROF_CAT2(a, b)
#define MP_PROF_SCOPE(z) MpPerfScope<(int)(z)> MP_PROF_CAT(mpPerfScope_, __LINE__)
#define MP_PROF_TURN() ((void)0)
#define MP_PROF_COUNT(c) ((void)0)
#endif

// MP3 perf3 (always compiled; platform/src/mp/mp_prof.cpp): the lite counters, the frame target and the test switches for the
// quality governor (decomp/src/Game/MP/MpQuality.cpp).
extern "C" {
void mp_perf_take(uint32_t out[3]);     // the zones' microseconds since the last call: [0] total, [1] puppet calc, [2] puppet draw
int mp_perflog_env(void);               // SMG_MP_PERFLOG=1 (read once); also sets mp_perf_on
uint32_t mp_frame_target_us(void);      // 16667 (60 fps), or 33333 only when a real 30 fps cap is engaged (GXV_FPS_LIMIT=30, steady 30 pacing)
uint32_t mp_frame_work_us(void);        // the renderer's work estimate a picture (Vita, dynamic quality's target mode), 0 unknown
int mp_quality_auto_max(void);          // SMG_MP_QUALITY_AUTO=1..4: the deepest level the governor may step to (testing); 0 default: it never steps
int mp_quality_forced(void);            // SMG_MP_QUALITY=0..4: that level, governor off (a test switch); -1 automatic
int mp_test_slow_ms(int* belowLevel);   // desktop only: SMG_MP_TEST_SLOW_MS=<ms>[:<level>] (0 on the console)
void mp_test_sleep_ms(int ms);          // desktop only
// platform/src/mp/mp_core.cpp: the helper thread's busy time since the last call (the first call starts the measurement)
void mp_core_helper_take(uint32_t* busyUs, uint32_t* wakes, uint32_t* maxUs);
// MP3 nocap (platform/src/mp/mp_core.cpp): the game thread's queued unreliable sends (STATE, POINTER) since the last call: sent by the
// helper thread, refused by the full ring, and their wait from queue to send (sum, max).
void mp_core_send_take(uint32_t* sent, uint32_t* refused, uint32_t* lagSumUs, uint32_t* lagMaxUs);
// MP3 netfeel (platform/src/mp/mp_core.cpp): what came in from / went out to a remote slot's link since the process began (datagrams in,
// bytes in, datagrams out; relaxed counters, wrap at 2^32: take differences). 0 for a slot out of range.
int mp_core_link_counts(int slot, uint32_t* dgIn, uint32_t* bytesIn, uint32_t* dgOut);
}

#endif  // PORT_MP_PROF_H
