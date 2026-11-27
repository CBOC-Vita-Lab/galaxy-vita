// GXV_FE_TWOCORE, the player's side: see vgx_twocore.h for the idea, vgx_twocore_b.cpp for the second worker. Included at the end of
// vgx_fesplit.cpp (the player's compilation only), so that every build and host test that links the split links this; the second
// worker's instance (vgx_twocore_b*.cpp) is linked by the console build and tools/test_vgx_twocore.sh, and without it the switch
// stays off (its entry points are weak here).
//
// ---- the protocol (one segment at a time)
//   B, ahead of the player in the top-level stream by at least vgx2_lead_bytes, at a CALL_DL (or a retained call) it can do, with no
//   segment outstanding: makes its state "cold" (every dirty flag up, no resolved state, no uniform block known: its first draw resolves
//   everything and sends every block, which the finishing thread deduplicates against the blocks current there, PushBlock), notes
//   the shared tables' epochs, sets vgx2_seg_state = kSegDecoding and then vgx2_stop_at = the command. It decodes command after command.
//   Before each it looks at the state: kSegClaimed (the player is at the segment's start and waits) ends the segment there; a command
//   it cannot do ends it there as well (kSegReady at once; B goes on scanning); the player past the segment's start (vgx2_apos) drops it.
//   At the end it writes its state there into vgx2_seg[slot].snap, its operations' chain and its logs, and sets kSegReady.
//   The player, at every top-level command (ProcessStream), compares the command's address with vgx2_stop_at; at the segment's start it
//   claims it (kSegDecoding -> kSegClaimed), waits for kSegReady (at most kWaitNs; then it declines: kSegClaimed -> kSegDropped, a
//   compare-and-swap that B's own kSegClaimed -> kSegReady races with) and, if the epochs have not moved since B began:
//     - splices B's operations into its own (split::TwoSplice), so the finishing thread makes B's records after its own and before
//       its next ones, and appends B's placeholders (job numbers moved by the player's own jobs so far);
//     - applies B's logs in order: palette loads (BpTlutLoad on the player's palette memory, from the same guest bytes), the display-list
//       cache entries' and textures' last-use marks, the character rectangles (ChaAddBox), the cache's age;
//     - takes B's per-stream state at the segment's end (the register file but the palette slots, the resolved state, the key memo's
//       current key, the block flags, the split's sent state) and marks what the player derives from it as stale (vertex plans,
//       texture slots, the GXV_DIRECT_J3D key copies, the character classification);
//   and goes on at B's end. Otherwise it goes on at the segment's start (B's work is lost; the picture is the player's).
//   FrontPresent ends the picture: the player cancels B, waits until B has let go of the stream, gives B its state for the next picture
//   (register file, frame, character state) and B starts again at the next picture's first command.
//
// ---- why the result is the single-thread one
//   - B runs the same code on its own copy of every per-stream object (its compilation's globals), from the player's state at the
//     picture's start and the same stream bytes: its register state at any command is the single-thread one (the scan applies every
//     register write, material list, indexed load, palette load and copy set-up; draws only change derived and resolved state).
//   - In a segment its resolved state starts cold. Everything the resolution computes is a function of the registers (keys, ids,
//     render state, texture bindings, blocks), so the records are the same; what depends on the history is (a) which blocks are sent
//     (cold: all; PushBlock skips a block equal to the current one, which is what not sending it means), (b) the state serial (only
//     compared for equality, between consecutive draws: B's numbers come from a range of their own, so no false equality with the
//     player's), (c) merging with the record before (static runs never merge; B only does static runs), (d) memos (key memo, texture
//     slots, soft flags, plans), whose misses recompute the same values.
//   - B reads the shared tables (display-list cache, texture cache, shader registry, warm-up list) only, under the lock; anything that
//     would change them ends or abandons its segment, and so does a display-list cache sweep that is due. Its last-use marks are applied
//     by the player in order. The player bumps an epoch whenever it changes a table in a way that could change what B read (cache
//     builds, promotions, bypasses, sweeps; EFB-copy textures made or destroyed; texture erasures); a segment whose epochs moved is not
//     used. New textures or shaders made by the player meanwhile need no epoch: B either found them (made before, as in stream order)
//     or ended its segment.
//   - B's operations are the operations the player would have sent from a cold state; the finishing thread runs them in stream order.
//   - Host proof: tools/test_vgx_twocore.sh (command buffers equal to GXV_FE_TWOCORE=0 frame for frame, several workloads and settings,
//     ThreadSanitizer and AddressSanitizer builds); console check: GXV_FE_TWOCORE=2 compares B's state with the player's at every
//     segment's start and end.
//
//   GXV_FE_TWOCORE=0|1|2       off (default) / on / verify (B's segments are made and checked, never used: the picture is the player's)
//   GXV_FE_TWOCORE_LEAD=<KiB>  how far ahead of the player B has to be to begin a segment (default 6)
//   GXV_FE_TWOCORE_LOG=<n>     the "fetwo:" line every n pictures (default 300; 1: every picture)
//   GXV_FE_TWOCORE_PRIO=<0xNN> B's priority on the console (default 0x93: below the finishing thread's and the other fourth-core workers')
//   GXV_FE_TWOCORE_CORE=<n>    B's core (default 3, the fourth core; when it is not proven, the switch stays off unless this says 1 or 2)
//
// ---- twocore24 (10 Nov): nothing of this can stop the game (agents/twocore24/REPORT.txt)
//   The console froze 20 s after launch with GXV_FE_TWOCORE=1 (c42, job 854): std::this_thread::yield() is EMPTY in the console's C++
//   library (vitasdk's libstdc++ is built without _GLIBCXX_USE_SCHED_YIELD), so B's "spin, then yield" wait for the next published bytes
//   was a pure spin that never gave the fourth core away while a picture was open. B ran at 0x93, above the par-view worker (0x9E, one
//   step above the game class) on the same core; the game thread's draw waits without a bound for a view job that worker is in the middle
//   of (par_async.h Ensure), so the worker never ran again, the game never published the rest of the picture, and B waited for it for
//   ever. Now:
//   - every wait of either side spins a little and then sleeps (Nap); none uses yield;
//   - B runs below every thread whose work someone waits for (default priority 0xA0: below the par workers, 0x9E, the finishing thread,
//     0x8F, the back end, 0x9F, and the audio threads); while it holds the shared lock the player may raise it (port_vgx2_boost);
//   - every wait of the player for B has a bound: a segment's start (GXV_FE_TWOCORE_WAIT_US, then the player does the bytes itself), the
//     picture's end (GXV_FE_TWOCORE_END_US, then the picture is let go of and B gets no picture until it is idle again), the lock
//     (B's reads are a few microseconds and block on nothing; past GXV_FE_TWOCORE_LOCK_US it counts as a hard timeout);
//   - B's waits for the player's lock end after GXV_FE_TWOCORE_BLOCK_US: B gives up its picture;
//   - GXV_FE_TWOCORE_TRIP hard timeouts within 600 pictures, or more than GXV_FE_TWOCORE_TRIP_LATE segments a picture that B was late
//     for over 300 pictures, turn the switch off for the session ("fetwo: ... turns itself off ..." once);
//   - B's state for a picture is handed over at the picture's first command, and only while B is idle (before: at the end of the last
//     one, even when the game had moved to its other FIFO segment and B had not let go yet).
//   GXV_FE_TWOCORE_WAIT_US=400 GXV_FE_TWOCORE_END_US=2000 GXV_FE_TWOCORE_LOCK_US=2000 GXV_FE_TWOCORE_BLOCK_US=3000 GXV_FE_TWOCORE_TRIP=4
//   GXV_FE_TWOCORE_TRIP_LATE=3 (segments a picture); GXV_FE_TWOCORE_INJECT=1|2|3 (host tests: B stalls in a claimed segment / holds the
//   lock GXV_FE_TWOCORE_INJECT_MS / sleeps at random at every hand-over)
//
// ---- twocore25 (11 Nov): the console's c49 runs explained (agents/twocore25/REPORT.txt)
//   - "verify: ... ends n (75 % differing) ... resolved state fields 0x6": bits 1 and 2 of vgx2_a_draw_compare, the fragment and vertex keys'
//     bytes, with the ids equal. GXV_DIRECT_J3D's templated resolution leaves the player's key copies undone (cur.fs / cur.vs hold earlier
//     keys, dj::CurFs gives the real ones); B's retained calls take the normal path and hold the copies. The comparison now reads the keys
//     as they are (dj::CurFs / CurVs); B makes its deferred copies (none today) before the hand-over. Not a difference of state.
//   - "empty 4.4, ended by tiny 4.3": J3D's small differed lists (a material colour or a texture matrix, 32 / 64 bytes, starting with an XF
//     write) before nearly every shape, which the player parses inline, ended B's segment; a segment began at a list call, so most ended at
//     their first command, empty, and the ring of 4 slots filled with them. B now parses such a list when it only writes registers
//     (GXV_FE_TWOCORE_TINY=1), and publishes a segment only once it has resolved a draw (an earlier end costs no slot).
//   - the hand-over copied the register file whole, 32 KiB of palette slots included, and Adopt copied the player's slots aside and back:
//     ~100 KiB of copies a segment. The slots are no longer in it (~7.5 KiB).
//   - loading / title: B gets pictures only in play (a stage, no loading, no closed screen or menu: port_scene_key, port_screen_quiet,
//     port_scene_loading_active), GXV_FE_TWOCORE_SETTLE (60) pictures after play began; while B makes less than GXV_FE_TWOCORE_MINSHARE
//     (5) % of the bytes of a window of GXV_FE_TWOCORE_USEWIN (120) pictures it rests GXV_FE_TWOCORE_REST (600, doubling) pictures, asleep.
//   - the picture's end no longer waits for B (GXV_FE_TWOCORE_END_US=0; B's wait for bytes wakes on the end); the next picture waits
//     GXV_FE_TWOCORE_START_US (50) at most, else goes without B ("let go late"; 30 in a row still turn the switch off). The 2 ms wait was
//     0.2-0.4 ms of the player a picture and its timeouts turned the switch off on the title screens.
//   - GXV_FE_TWOCORE_SLOTS (8, at most 32): the ring of segments; GXV_FE_TWOCORE_INJECT=4 (host measurement): the player waits at each
//     picture's start until B has gone through it (B infinitely early); GXV_FE_TWOCORE_TEST_QUIET=<n> (host test): n pictures not in play.
#pragma once
#include "vgx_twocore.h"
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_texdecode.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <time.h>
#include <thread>

extern "C" void port_log(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" int64_t port_thread_cpu_us(void) __attribute__((weak));
extern "C" int64_t port_host_time_ns(void) __attribute__((weak));
// (twocore24) raises B's priority while it holds the shared lock (1, the player's call) and gives it back (0, B's own call); the host's
// placement (vgx_galaxy.cpp TwoCoreThreadInit) knows B's thread. Absent: nothing is raised (the host tests).
extern "C" void port_vgx2_boost(int on) __attribute__((weak));
// (twocore25) the game's "in play" facts (platform/src/port/perf.cpp, vita/runtime/flat_view.cpp; weak: absent on the host, where every
// picture is in play): the stage's key (0: no stage), a closed screen or a menu, the loading thread at work
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" int port_scene_loading_active(void) __attribute__((weak));

// ---- B's compilation (vgx_twocore_b.cpp; weak: absent from builds that do not link it)
extern "C" void vgx2_b_thread_main(void) __attribute__((weak));
extern "C" void vgx2_b_picture_state(const void* g, uint32_t gBytes, const void* cha, uint32_t chaBytes, uint64_t frame) __attribute__((weak));
extern "C" int vgx2_b_state_objs(Vgx2Obj* out) __attribute__((weak));
extern "C" int vgx2_b_draw_objs(Vgx2Obj* out) __attribute__((weak));
extern "C" int vgx2_b_split_objs(Vgx2Obj* out) __attribute__((weak));
// ---- the player's files
extern "C" void vgx2_a_tlut_replay(uint32_t bp64, uint32_t bp65);          // vgx_bpsync.cpp
extern "C" void vgx2_a_dl_mark(void* e, uint64_t age);                      // vgx_dlcache.cpp
extern "C" void vgx2_a_dl_rehash(void* e, uint64_t gl, const uint64_t* ga);
extern "C" void vgx2_a_dl_drawn(void* e);
extern "C" void vgx2_a_dl_age(uint64_t age, uint64_t frame);
extern "C" uint64_t vgx2_a_dl_age_next(uint64_t frame);
extern "C" void vgx2_a_tex_mark(void* e, uint64_t frame);                   // vgx_texcache.cpp
extern "C" void vgx2_a_cha_add(int16_t x, int16_t y, int16_t w, int16_t h); // vgx_draw.cpp
extern "C" void vgx2_a_draw_after_adopt(uint32_t jobs);
extern "C" uint32_t vgx2_a_draw_compare(const uint8_t* p);
extern "C" void vgx2_a_draw_cha(void** p, uint32_t* n);
extern "C" int vgx2_a_draw_refusal(void);
extern "C" uint32_t vgx2_a_split_splice(void* first, void* last, uint32_t lastPos, uint32_t ops, uint32_t jobs); // vgx_fesplit.cpp
extern "C" void vgx2_a_split_drop(void* first);
extern "C" int vgx2_a_split_on(void);

namespace vgx {
uint64_t CurrentFrame();
} // namespace vgx

// ------------------------------------------------------------------ shared
extern "C" {
int vgx2_mode = 0;
std::atomic<uint32_t> vgx2_epoch[VGX2_EP_COUNT];
alignas(64) std::atomic<const uint8_t*> vgx2_pub_end{nullptr};
// (twocore25: the player writes it before every top-level command; on a line of its own, so that B's reads of the protocol's other words do
// not take the line from the player at every command)
alignas(64) std::atomic<const uint8_t*> vgx2_apos{nullptr};
alignas(64) uint32_t vgx2_nslots = 8;
std::atomic<uint32_t> vgx2_pic{0};
const uint8_t* vgx2_pic_start = nullptr;
std::atomic<int> vgx2_cancel{0};
std::atomic<int> vgx2_b_busy{0};
alignas(64) std::atomic<int> vgx2_seg_state[VGX2_SLOTS];
alignas(64) std::atomic<const uint8_t*> vgx2_stop_at[VGX2_SLOTS];
Vgx2Seg vgx2_seg[VGX2_SLOTS];
uint32_t vgx2_lead_bytes = 6u << 10;
uint64_t vgx2_pic_age = 0;
std::atomic<uint32_t> vgx2_b_gaveup[16];
std::atomic<int> vgx2_b_exit{0};
int vgx2_inject = 0;
uint32_t vgx2_inject_ms = 0;
uint32_t vgx2_block_us = 3000;
int vgx2_tiny = 1;
std::atomic<uint32_t> vgx2_b_atend{0}; // (GXV_FE_TWOCORE_INJECT=4, host measurement) the picture whose published bytes B has gone through
std::atomic<uint32_t> vgx2_b_unpub[8];
std::atomic<uint32_t> vgx2_b_unpub_list[12];
std::atomic<uint32_t> vgx2_b_tiny_n{0};
std::vector<uint64_t> vgx2_dbg_a, vgx2_dbg_b;
int vgx2_dbg = 0;
}

namespace vgx {
namespace two {
void HardTimeout(int what, int64_t ns); // (below)
} // namespace two
} // namespace vgx

namespace {
enum : int { kSegNone = 0, kSegDecoding = 1, kSegClaimed = 2, kSegReady = 3, kSegDropped = 4 };
// The lock: 0 free, 1 the player's (recursive: g_aDepth), 2 B's.
std::atomic<int> g_lockOwner{0};
int g_aDepth = 0; // (the player's only)
std::atomic<int> g_boosted{0}; // (twocore24) the player raised B's priority; B gives it back at its next unlock or idle point
int64_t g_lockUs = 2000;       // GXV_FE_TWOCORE_LOCK_US
uint64_t g_lockWaits = 0;      // (the player's) waits for B's lock, and their time
int64_t g_lockWaitNs = 0, g_lockMaxNs = 0;
int64_t Vgx2Now() {
    if (port_host_time_ns) return port_host_time_ns();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// A real sleep. (std::this_thread::yield() does nothing on the console: see the head of this file)
inline void Nap(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
} // namespace

extern "C" void vgx2_lock_a(void) {
    if (g_aDepth++ > 0) return;
    int f = 0;
    if (__builtin_expect(g_lockOwner.compare_exchange_strong(f, 1, std::memory_order_acquire, std::memory_order_relaxed), 1)) return;
    // (B is in one of its reads: a few microseconds, unless its core preempted it; its priority is raised for the rest of the read)
    const int64_t t0 = Vgx2Now();
    bool boosted = false, said = false;
    for (unsigned spins = 0;; ++spins) {
        f = 0;
        if (g_lockOwner.compare_exchange_weak(f, 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
        if ((spins & 31) != 31) continue;
        const int64_t w = Vgx2Now() - t0;
        if (!boosted && w > 30000) {
            boosted = true;
            if (port_vgx2_boost) {
                port_vgx2_boost(1);
                g_boosted.store(1, std::memory_order_release); // (after the raise: B gives it back at its next unlock)
            }
        }
        if (!said && w > g_lockUs * 1000) {
            said = true;
            vgx::two::HardTimeout(0, w);
        }
        if (w > 60000) Nap(20); // (B may share this core: GXV_FE_TWOCORE_CORE=2)
    }
    const int64_t w = Vgx2Now() - t0;
    ++g_lockWaits;
    g_lockWaitNs += w;
    if (w > g_lockMaxNs) g_lockMaxNs = w;
}
extern "C" void vgx2_unlock_a(void) {
    if (--g_aDepth > 0) return;
    g_lockOwner.store(0, std::memory_order_release);
}
// (B's; 0: the player held the lock longer than GXV_FE_TWOCORE_BLOCK_US, B gives its picture up. B holds nothing while it waits.)
extern "C" int vgx2_lock_b(void) {
    int f = 0;
    if (g_lockOwner.compare_exchange_strong(f, 2, std::memory_order_acquire, std::memory_order_relaxed)) return 1;
    const int64_t t0 = Vgx2Now();
    for (unsigned spins = 0;; ++spins) {
        f = 0;
        if (g_lockOwner.compare_exchange_weak(f, 2, std::memory_order_acquire, std::memory_order_relaxed)) return 1;
        if ((spins & 15) != 15) continue;
        const int64_t w = Vgx2Now() - t0;
        if (w > static_cast<int64_t>(vgx2_block_us) * 1000) return 0;
        if (w > 20000) Nap(20);
    }
}
extern "C" void vgx2_b_unboost(void) {
    if (g_boosted.load(std::memory_order_relaxed) && g_boosted.exchange(0, std::memory_order_acq_rel) && port_vgx2_boost) port_vgx2_boost(0);
}
extern "C" void vgx2_unlock_b(void) {
    g_lockOwner.store(0, std::memory_order_release);
    vgx2_b_unboost();
}

// ---- forwards for B's compilation (the guest-memory tracking is one per process)
extern "C" uint64_t vgx2_guest_gen(const void* p, uint32_t n) { return vgx::GuestWriteGeneration(p, n); }
extern "C" uint64_t vgx2_guest_epoch(void) { return vgx::GuestWriteEpoch(); }
extern "C" void vgx2_guest_gen_prefetch(const void* p, uint32_t n) { vgx::GuestWriteGenerationPrefetch(p, n); }
extern "C" int vgx2_is_guest_ram(const void* p, uint32_t n) { return vgx::IsGuestRam(p, n) ? 1 : 0; }
extern "C" int vgx2_guest_writes_touch(uint64_t e0, uint64_t e1, const void* ranges, int n) {
    return vgx::GuestWritesTouch(e0, e1, static_cast<const vgx::HostRange*>(ranges), n) ? 1 : 0;
}
extern "C" int vgx2_log_enabled(void) { return vgx::LogEnabled() ? 1 : 0; }
extern "C" void* vgx2_resolve_hook(void) { return reinterpret_cast<void*>(vgx::Hooks().resolve); } // (the guest address map, one per process)
extern "C" int vgx2_tex_is_ci(uint32_t fmt) { return vgx::TexFormatIsCI(fmt) ? 1 : 0; }
extern "C" uint32_t vgx2_tex_level_size(uint32_t w, uint32_t h, uint32_t fmt) { return vgx::TexLevelSize(w, h, fmt); }

namespace vgx {
namespace two {
bool g_needStart = true;
const uint8_t* g_verifyAt = nullptr;
uint32_t g_aSlot = 0;
bool g_aActive = false; // this picture: B was given it

namespace {
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (port_log_summary) port_log_summary("%s", line);
    else if (port_log) port_log("%s", line);
    else std::fprintf(stderr, "%s\n", line);
}
int64_t NowNs() {
    if (port_host_time_ns) return port_host_time_ns();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int64_t CpuUs() {
    if (port_thread_cpu_us) return port_thread_cpu_us();
#if !defined(__vita__)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
    return 0;
}

bool g_started = false, g_failed = false;
bool& g_active = g_aActive;     // this picture: B was given it
bool g_bGiven = false;          // (twocore24) B was given a picture it may not have let go of yet: its slots are its until it is idle
bool g_tripped = false;         // (twocore24) the switch turned itself off for the session (B gets no picture; vgx2_mode goes 0 when B is idle)
const char* g_tripWhy = nullptr;
const uint8_t* g_picStart = nullptr;
const uint8_t* g_lastEnd = nullptr; // the end of the top-level bytes handed over in the picture's memory (what B may read up to)
const uint8_t* g_callEnd = nullptr; // (twocore24) the end of the bytes of the ProcessStream call the player is in
const uint8_t* g_upTo = nullptr;    // (twocore24) PublishedUpTo's end for the next picture (hosts that hand pieces over), or null
int (*g_threadInit)(int) = nullptr; // (the host's: 1 placed as asked, 0 not: the switch stays off)
std::atomic<int> g_initResult{-1};
int g_core = 3;
long g_logEvery = 300;
int64_t g_waitNs = 400000;  // GXV_FE_TWOCORE_WAIT_US: the longest the player waits at a segment's start for B to stop (then it declines)
int64_t g_endNs = 0;        // GXV_FE_TWOCORE_END_US: the longest it waits at a picture's end for B to let go of the picture (twocore25: 0)
int64_t g_startNs = 50000;  // (twocore25) GXV_FE_TWOCORE_START_US: the longest it waits at the next picture's first command for B to be idle
long g_tripHard = 4;        // GXV_FE_TWOCORE_TRIP: hard timeouts (lock, picture end) within 600 pictures that turn the switch off
double g_tripLate = 3.0;    // GXV_FE_TWOCORE_TRIP_LATE: segments a picture B was late for (over 300 pictures) that turn it off
uint64_t g_hardWin = 0, g_hardWinPics = 0, g_lateWin = 0, g_lateWinPics = 0;
uint32_t g_busyRun = 0;     // (twocore24) pictures in a row that B was still in an earlier one (30: off for the session)
// (twocore25) B only in play, and only while it is of use:
//   GXV_FE_TWOCORE_SETTLE=<n>     pictures in play in a row before B gets one (default 60): no picture of a loading screen, the title, a
//                                 fade or a menu is given to B, so none of them can count against it (the console's c49 runs turned
//                                 themselves off within seconds, on the title and loading screens)
//   GXV_FE_TWOCORE_MINSHARE=<%>   every GXV_FE_TWOCORE_USEWIN (120) pictures given to B: if B made less than this share of their bytes
//                                 (=1: taken; =2: made and checked), B rests (gets no picture) for GXV_FE_TWOCORE_REST pictures (600),
//                                 twice as long after each rest that did not help (at most 16 times); default 5. 0: never rests.
//   B waits for a picture asleep: its CPU while it gets none is a wake-up every 20 ms.
long g_settle = 60, g_useWin = 120, g_restBase = 600;
long g_testQuiet = 0; // (GXV_FE_TWOCORE_TEST_QUIET=<n>, host tests: the first n pictures are not in play, and the settling applies as on the console)
uint64_t g_picsSeen = 0;
double g_minShare = 5.0;
uint64_t g_playRun = 0, g_restLeft = 0, g_restLen = 0;
uint64_t g_useWinPics = 0, g_useWinMade = 0, g_useWinBytes = 0;
const uint8_t* g_lastGiven = nullptr; // (the picture start of the last picture given to B: its bytes for the window)
std::mutex& g_mtx = *new std::mutex;
std::condition_variable& g_cv = *new std::condition_variable;

// ---- the log line's counters (the player's; B's are in the segments)
struct Totals {
    uint64_t pictures = 0, active = 0, segs = 0, taken = 0, declinedEpoch = 0, declinedWait = 0, lost = 0, empty = 0;
    uint64_t bytesTaken = 0, bytesPic = 0, staticRuns = 0, jobs = 0, lists = 0;
    int64_t aCpuUs = 0, bCpuUs = 0, waitNs = 0, adoptNs = 0, bDecodeNs = 0, picNs = 0;
    uint64_t whyEnd[8] = {}, listWhy[10] = {};
    uint64_t verS = 0, verSBad = 0, verE = 0, verEBad = 0;
    int64_t verANs = 0, verBNs = 0; // (GXV_FE_TWOCORE=2) the player's own time for the segments B made, and B's for them
    // (twocore24) pictures B was not given, by kNoB*; hard timeouts (0 the lock, 1 the picture's end); pictures B gave up (two::Abandon's
    // reasons, from B's counters); pictures cancelled in the middle (the game moved to its other FIFO segment); the lock's waits
    uint64_t noB[16] = {}, hard[2] = {}, gaveUp[16] = {}, cancelled = 0, lockWaits = 0;
    int64_t lockWaitNs = 0, endWaitNs = 0;
    // (twocore25) bytes of the segments B made that reached the player (=1: taken; =2: checked), of the pictures given to B; pictures B let go
    // of late (soft: the next one goes without B); rests begun; B's segments dropped before publishing, tiny register lists it parsed
    uint64_t bytesMade = 0, bytesGiven = 0, endLate = 0, rests = 0, unpub = 0, tiny = 0;
} g_tot, g_last;
// (twocore24) why a picture was not given to B (the log line's "pictures without B")
enum : int { kNoBSplit = 1, kNoBStats, kNoBTrace, kNoBCensus, kNoBStaticInst, kNoBDlLearn, kNoBRsCache, kNoBJ3dNative, kNoBHudNative,
             kNoBBusy, kNoBOff, kNoBQuiet, kNoBSettle, kNoBRest, kNoBCount };
const char* const kNoBName[kNoBCount] = {"", "split off", "statistics detail", "trace", "GXV_FE_CENSUS", "GXV_STATIC_INST",
                                         "GXV_RIGIDMERGE_DLLEARN", "GXV_RSCACHE not 1", "GXV_J3DNATIVE", "GXV_HUD_NATIVE",
                                         "B still in an earlier picture", "switched off for the session",
                                         "not in play (no stage, loading, a closed screen or a menu)", "settling after play began",
                                         "resting (B made too little of the pictures before)"};
int64_t g_verifyT0 = 0, g_verifyBNs = 0;
std::atomic<int64_t> g_bCpu{0}; // (B's thread: its CPU so far, written at its picture ends)
int64_t g_aCpu0 = 0, g_pic0 = 0;
uint32_t g_segsThisPic = 0, g_segBytesThisPic = 0; // (the per-picture line: the segments taken, as offsets in the picture, the first 8)
uint32_t g_segAt[8][2] = {};
size_t g_picBytes = 0;

void Report(bool force) {
    const uint64_t n = g_tot.pictures - g_last.pictures;
    if (!n || (!force && n < static_cast<uint64_t>(g_logEvery))) return;
    const double pn = static_cast<double>(n);
    auto d = [](uint64_t a, uint64_t b) { return static_cast<double>(a - b); };
    const uint64_t segs = g_tot.segs - g_last.segs;
    Log("fetwo: GXV_FE_TWOCORE=%d, %llu pictures (%llu with B): FE cpu per picture core 2 %.2f ms, core 3 (B) %.2f ms (decoding %.2f); "
        "per picture: segments %.2f taken %.2f (bytes %.0f of %.0f, %.1f %%; static runs %.1f, lists %.1f, jobs %.1f), declined %.2f "
        "(tables changed %.2f, B late %.2f), lost %.2f, empty %.2f; player waited %.3f ms, took over %.3f ms; ended by: claim %.1f, "
        "unsupported %.1f, list %.1f [mode %.1f tiny %.1f ram %.1f bypassed %.1f sweep %.1f new %.1f variant %.1f changed %.1f unpromoted %.1f], "
        "shared %.1f, other %.1f; verify: starts %llu (%llu differing), ends %llu (%llu differing); the segments verified: the player %.3f ms, B %.3f ms per picture",
        vgx2_mode, static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_tot.active - g_last.active),
        (g_tot.aCpuUs - g_last.aCpuUs) / 1e3 / pn, (g_tot.bCpuUs - g_last.bCpuUs) / 1e3 / pn, (g_tot.bDecodeNs - g_last.bDecodeNs) / 1e6 / pn,
        d(g_tot.segs, g_last.segs) / pn, d(g_tot.taken, g_last.taken) / pn, d(g_tot.bytesTaken, g_last.bytesTaken) / pn,
        d(g_tot.bytesPic, g_last.bytesPic) / pn,
        g_tot.bytesPic > g_last.bytesPic ? 100.0 * d(g_tot.bytesTaken, g_last.bytesTaken) / d(g_tot.bytesPic, g_last.bytesPic) : 0.0,
        d(g_tot.staticRuns, g_last.staticRuns) / pn, d(g_tot.lists, g_last.lists) / pn, d(g_tot.jobs, g_last.jobs) / pn,
        d(g_tot.declinedEpoch + g_tot.declinedWait, g_last.declinedEpoch + g_last.declinedWait) / pn,
        d(g_tot.declinedEpoch, g_last.declinedEpoch) / pn, d(g_tot.declinedWait, g_last.declinedWait) / pn, d(g_tot.lost, g_last.lost) / pn,
        d(g_tot.empty, g_last.empty) / pn, (g_tot.waitNs - g_last.waitNs) / 1e6 / pn, (g_tot.adoptNs - g_last.adoptNs) / 1e6 / pn,
        d(g_tot.whyEnd[1], g_last.whyEnd[1]) / pn, d(g_tot.whyEnd[2], g_last.whyEnd[2]) / pn, d(g_tot.whyEnd[3], g_last.whyEnd[3]) / pn,
        d(g_tot.listWhy[1], g_last.listWhy[1]) / pn, d(g_tot.listWhy[2], g_last.listWhy[2]) / pn, d(g_tot.listWhy[3], g_last.listWhy[3]) / pn,
        d(g_tot.listWhy[4], g_last.listWhy[4]) / pn, d(g_tot.listWhy[5], g_last.listWhy[5]) / pn, d(g_tot.listWhy[6], g_last.listWhy[6]) / pn,
        d(g_tot.listWhy[7], g_last.listWhy[7]) / pn, d(g_tot.listWhy[8], g_last.listWhy[8]) / pn, d(g_tot.listWhy[9], g_last.listWhy[9]) / pn,
        d(g_tot.whyEnd[4], g_last.whyEnd[4]) / pn, d(g_tot.whyEnd[0] + g_tot.whyEnd[5] + g_tot.whyEnd[6] + g_tot.whyEnd[7], g_last.whyEnd[0] + g_last.whyEnd[5] + g_last.whyEnd[6] + g_last.whyEnd[7]) / pn,
        static_cast<unsigned long long>(g_tot.verS), static_cast<unsigned long long>(g_tot.verSBad), static_cast<unsigned long long>(g_tot.verE),
        static_cast<unsigned long long>(g_tot.verEBad), (g_tot.verANs - g_last.verANs) / 1e6 / pn, (g_tot.verBNs - g_last.verBNs) / 1e6 / pn);
    (void)segs;
    // (twocore24) the pictures B was not given and why, the bounded waits that ran out, the pictures B gave up, the lock
    for (int i = 0; i < 16; ++i) g_tot.gaveUp[i] = vgx2_b_gaveup[i].load(std::memory_order_relaxed);
    g_tot.unpub = 0;
    for (auto& u : vgx2_b_unpub) g_tot.unpub += u.load(std::memory_order_relaxed);
    // (twocore25: B's counters are the session's: the breakdown below is the session's too)
    char unpubWhy[200];
    {
        static const char* const wn[8] = {"other", "claim", "command", "list", "shared", "palette", "passed", "end"};
        static const char* const ln[12] = {"?", "mode", "tiny", "ram", "bypassed", "sweep", "new", "variant", "changed", "unpromoted", "?", "?"};
        size_t q = 0;
        unpubWhy[0] = 0;
        for (int i = 0; i < 8 && q < sizeof unpubWhy - 40; ++i) {
            const uint32_t v = vgx2_b_unpub[i].load(std::memory_order_relaxed);
            if (v) q += static_cast<size_t>(std::snprintf(unpubWhy + q, sizeof unpubWhy - q, "%s%s %u", q ? ", " : "session: ", wn[i], v));
        }
        for (int i = 1; i < 10 && q < sizeof unpubWhy - 40; ++i) {
            const uint32_t v = vgx2_b_unpub_list[i].load(std::memory_order_relaxed);
            if (v) q += static_cast<size_t>(std::snprintf(unpubWhy + q, sizeof unpubWhy - q, ", list %s %u", ln[i], v));
        }
        if (!q) std::snprintf(unpubWhy, sizeof unpubWhy, "none");
    }
    g_tot.tiny = vgx2_b_tiny_n.load(std::memory_order_relaxed);
    char nob[512];
    size_t k = 0;
    nob[0] = 0;
    for (int i = 1; i < kNoBCount && k < sizeof nob - 64; ++i)
        if (g_tot.noB[i] != g_last.noB[i])
            k += static_cast<size_t>(std::snprintf(nob + k, sizeof nob - k, "%s%s %llu", k ? ", " : "", kNoBName[i],
                                                   static_cast<unsigned long long>(g_tot.noB[i] - g_last.noB[i])));
    uint64_t noBAll = 0;
    for (int i = 1; i < kNoBCount; ++i) noBAll += g_tot.noB[i] - g_last.noB[i];
    const uint64_t lw = g_tot.lockWaits - g_last.lockWaits;
    Log("fetwo: GXV_FE_TWOCORE=%d, the same %llu pictures: without B %llu (%s); hard timeouts: lock %llu, picture end %llu (all %llu, %llu in "
        "the session); pictures cancelled in the middle %llu; B gave up pictures: lock %llu, shader key %llu, variant %llu, texture %llu, "
        "palette %llu, HUD %llu, other %llu; the player waited for B's lock %llu times (%.3f ms in all, longest %.3f ms), for B to let go "
        "of a picture %.3f ms per picture; let go late %llu; B made %.1f %% of the bytes of the pictures it was given (%s); B's segments "
        "dropped before publishing %llu (%s), tiny register lists parsed %llu; rests %llu%s%s",
        vgx2_mode, static_cast<unsigned long long>(n), static_cast<unsigned long long>(noBAll), k ? nob : "none",
        static_cast<unsigned long long>(g_tot.hard[0] - g_last.hard[0]), static_cast<unsigned long long>(g_tot.hard[1] - g_last.hard[1]),
        static_cast<unsigned long long>(g_tot.hard[0] + g_tot.hard[1] - g_last.hard[0] - g_last.hard[1]),
        static_cast<unsigned long long>(g_tot.hard[0] + g_tot.hard[1]), static_cast<unsigned long long>(g_tot.cancelled - g_last.cancelled),
        static_cast<unsigned long long>(g_tot.gaveUp[10] - g_last.gaveUp[10]), static_cast<unsigned long long>(g_tot.gaveUp[2] - g_last.gaveUp[2]),
        static_cast<unsigned long long>(g_tot.gaveUp[3] - g_last.gaveUp[3]), static_cast<unsigned long long>(g_tot.gaveUp[4] - g_last.gaveUp[4]),
        static_cast<unsigned long long>(g_tot.gaveUp[5] - g_last.gaveUp[5]), static_cast<unsigned long long>(g_tot.gaveUp[7] - g_last.gaveUp[7]),
        static_cast<unsigned long long>(g_tot.gaveUp[0] + g_tot.gaveUp[1] + g_tot.gaveUp[6] + g_tot.gaveUp[8] + g_tot.gaveUp[9] - g_last.gaveUp[0] -
                                        g_last.gaveUp[1] - g_last.gaveUp[6] - g_last.gaveUp[8] - g_last.gaveUp[9]),
        static_cast<unsigned long long>(lw), (g_tot.lockWaitNs - g_last.lockWaitNs) / 1e6, g_lockMaxNs / 1e6,
        (g_tot.endWaitNs - g_last.endWaitNs) / 1e6 / pn, static_cast<unsigned long long>(g_tot.endLate - g_last.endLate),
        g_tot.bytesPic > g_last.bytesPic ? 100.0 * d(g_tot.bytesMade, g_last.bytesMade) / d(g_tot.bytesPic, g_last.bytesPic) : 0.0,
        vgx2_mode == 2 ? "made and checked: what =1 would take" : "taken",
        static_cast<unsigned long long>(g_tot.unpub - g_last.unpub), unpubWhy, static_cast<unsigned long long>(g_tot.tiny - g_last.tiny),
        static_cast<unsigned long long>(g_tot.rests - g_last.rests), g_tripped ? "; OFF for the session: " : "", g_tripped ? g_tripWhy : "");
    g_last = g_tot;
}

// (twocore24) the switch goes off for the session: B is given no picture from now on; vgx2_mode goes 0 once B is idle (Finish below)
void Trip(const char* why) {
    if (g_tripped) return;
    g_tripped = true;
    g_tripWhy = why;
    Log("fetwo: GXV_FE_TWOCORE=%d turns itself off for the rest of the session: %s (hard timeouts so far: lock %llu, picture end %llu; "
        "segments B was late for %llu in %llu pictures with B). The pictures stay exact: the player decodes everything itself.",
        vgx2_mode, why, static_cast<unsigned long long>(g_tot.hard[0]), static_cast<unsigned long long>(g_tot.hard[1]),
        static_cast<unsigned long long>(g_tot.declinedWait), static_cast<unsigned long long>(g_tot.active));
}
// (B idle and nothing of its outstanding: the switch really goes off; B's thread ends)
void Finish() {
    if (!g_tripped || g_bGiven || !vgx2_mode || vgx2_b_busy.load(std::memory_order_seq_cst)) return;
    Report(true);
    vgx2_b_exit.store(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cv.notify_all();
    }
    vgx2_mode = 0;
}

// 0, or why this picture cannot be B's (kNoB*): the split must be on, and nothing that inspects the moved work per draw or needs the
// cache's verify modes (vgx_draw.cpp vgx2_a_draw_refusal)
int Refusal() {
    const int why = !vgx2_a_split_on() ? static_cast<int>(kNoBSplit) : vgx2_a_draw_refusal();
    static uint32_t said = 0; // (the first time of each reason)
    if (why > 0 && why < kNoBCount && !(said & (1u << why))) {
        said |= 1u << why;
        Log("fetwo: pictures without the second worker: %s%s", kNoBName[why],
            why == kNoBSplit ? " (GXV_SPLIT=1 is needed; not 2)" : " (a switch whose per-draw work B does not hand over)");
    }
    return why > 0 && why < kNoBCount ? why : 0;
}

void* ThreadEntry(void*) {
    const int ok = g_threadInit ? g_threadInit(g_core) : 1;
    g_initResult.store(ok ? 1 : 0, std::memory_order_release);
    if (!ok) return nullptr; // (B's core was not to be had: the player keeps the whole picture)
    vgx2_b_thread_main();
    return nullptr;
}

void Start() {
    g_started = true;
    vgx2_mode = static_cast<int>(EnvInt("GXV_FE_TWOCORE", 0));
    if (vgx2_mode < 0 || vgx2_mode > 2) vgx2_mode = 0;
    if (!vgx2_mode) return;
    const long lead = EnvInt("GXV_FE_TWOCORE_LEAD", 6);
    vgx2_lead_bytes = static_cast<uint32_t>(lead < 1 ? 1 : lead > 4096 ? 4096 : lead) << 10;
    g_logEvery = EnvInt("GXV_FE_TWOCORE_LOG", 300);
    if (g_logEvery < 1) g_logEvery = 1;
    auto knob = [](const char* name, long def, long lo, long hi) {
        const long v = EnvInt(name, def);
        return v < lo ? lo : v > hi ? hi : v;
    };
    g_waitNs = knob("GXV_FE_TWOCORE_WAIT_US", 400, 10, 4000) * 1000;
    g_endNs = knob("GXV_FE_TWOCORE_END_US", 0, 0, 20000) * 1000;
    g_startNs = knob("GXV_FE_TWOCORE_START_US", 50, 0, 2000) * 1000;
    g_lockUs = knob("GXV_FE_TWOCORE_LOCK_US", 2000, 100, 100000);
    vgx2_block_us = static_cast<uint32_t>(knob("GXV_FE_TWOCORE_BLOCK_US", 3000, 50, 100000));
    g_tripHard = knob("GXV_FE_TWOCORE_TRIP", 4, 1, 1000000);
    g_tripLate = static_cast<double>(knob("GXV_FE_TWOCORE_TRIP_LATE", 3, 0, 1000));
    vgx2_inject = static_cast<int>(knob("GXV_FE_TWOCORE_INJECT", 0, 0, 4));
    vgx2_inject_ms = static_cast<uint32_t>(knob("GXV_FE_TWOCORE_INJECT_MS", 0, 0, 100000000));
    vgx2_tiny = static_cast<int>(knob("GXV_FE_TWOCORE_TINY", 1, 0, 1));
    vgx2_nslots = static_cast<uint32_t>(knob("GXV_FE_TWOCORE_SLOTS", 8, 1, VGX2_SLOTS));
    g_settle = knob("GXV_FE_TWOCORE_SETTLE", 60, 0, 100000);
    g_useWin = knob("GXV_FE_TWOCORE_USEWIN", 120, 10, 100000);
    g_restBase = knob("GXV_FE_TWOCORE_REST", 600, 10, 1000000);
    g_minShare = static_cast<double>(knob("GXV_FE_TWOCORE_MINSHARE", 5, 0, 100));
    g_testQuiet = knob("GXV_FE_TWOCORE_TEST_QUIET", 0, 0, 1000000);
    g_core = static_cast<int>(EnvInt("GXV_FE_TWOCORE_CORE", 3));
    vgx2_dbg = static_cast<int>(EnvInt("GXV_FE_TWOCORE_DEBUG", 0));
    if (!vgx2_b_thread_main || !vgx2_b_picture_state || !vgx2_b_state_objs) {
        Log("fetwo: this build has no second instance of the front end (vgx_twocore_b*.cpp): GXV_FE_TWOCORE stays off");
        vgx2_mode = 0;
        return;
    }
    for (auto& e : vgx2_epoch) e.store(1, std::memory_order_relaxed);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512u << 10); // (the front end's resolution keeps keys and blocks on the stack)
    pthread_t th;
    const int rc = pthread_create(&th, &attr, &ThreadEntry, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        Log("fetwo: the second worker could not be started: GXV_FE_TWOCORE stays off");
        vgx2_mode = 0;
        g_failed = true;
        return;
    }
    pthread_detach(th);
    // (once: the worker places itself on its core; the switch is decided before the first picture)
    for (int i = 0; i < 20000 && g_initResult.load(std::memory_order_acquire) < 0; ++i) Nap(100);
    if (g_initResult.load(std::memory_order_acquire) != 1) {
        Log("fetwo: the second worker could not have core %d (GXV_FE_TWOCORE_CORE; 3 needs a proven fourth core): GXV_FE_TWOCORE stays off", g_core);
        vgx2_mode = 0;
        g_failed = true;
        return;
    }
    Log("fetwo: two-core front end on (GXV_FE_TWOCORE=%d): a second instance of the front end decodes segments of the picture ahead of "
        "the player%s; lead %u KiB; bounded waits: segment start %lld us, picture end %lld us, lock %lld us, B for the lock %u us; off after "
        "%ld hard timeouts in 600 pictures or %.1f late segments a picture%s",
        vgx2_mode, vgx2_mode == 2 ? " (verify: made and checked, not used)" : "", vgx2_lead_bytes >> 10, static_cast<long long>(g_waitNs / 1000),
        static_cast<long long>(g_endNs / 1000), static_cast<long long>(g_lockUs), vgx2_block_us, g_tripHard, g_tripLate,
        vgx2_inject ? " [TEST INJECTION]" : "");
}

// The objects of the per-stream lists (both compilations), in order.
int AllObjs(bool b, Vgx2Obj* out) {
    int n = 0;
    n += (b ? vgx2_b_state_objs : vgx2_a_state_objs)(out + n);
    n += (b ? vgx2_b_draw_objs : vgx2_a_draw_objs)(out + n);
    n += (b ? vgx2_b_split_objs : vgx2_a_split_objs)(out + n);
    return n;
}

// Takes B's state at the segment's end (seg.snap: B's lists, serialized in order).
bool Adopt(const Vgx2Seg& s) {
    Vgx2Obj a[32];
    const int n = AllObjs(false, a);
    size_t total = 0;
    for (int i = 0; i < n; ++i) total += a[i].n;
    if (total != s.snap.size()) {
        Log("fetwo: the hand-over's size differs (%u bytes, B gave %u): the switch goes off", static_cast<unsigned>(total), static_cast<unsigned>(s.snap.size()));
        vgx2_mode = 0;
        return false;
    }
    // (the palette slots stay the player's: they point into its own palette memory, which the loads B made were applied to above;
    // every texmap is synchronised again from the registers before its next use. twocore25: they are not in the hand-over at all, see
    // vgx_state.cpp state_objs)
    const uint8_t* q = s.snap.data();
    for (int i = 0; i < n; ++i) {
        std::memcpy(a[i].p, q, a[i].n);
        q += a[i].n;
    }
    g.bpTexStale = 0xFF;
    g.layoutDirty = true; // (the player's vertex plans were made for its own descriptor)
    vgx2_a_draw_after_adopt(s.jobs);
    return true;
}

// GXV_FE_TWOCORE=2: the register file B had at a segment's start (or end), against the player's. Only the registers and what they
// are turned into are compared (not the dirty flags, which the resolutions clear, nor the derived arrays a sync remakes).
bool SameRegisters(const State& b) {
    const State& a = g;
    bool same = std::memcmp(a.bp, b.bp, sizeof a.bp) == 0 && std::memcmp(a.tevReg, b.tevReg, sizeof a.tevReg) == 0 &&
                std::memcmp(a.tevKonst, b.tevKonst, sizeof a.tevKonst) == 0 && std::memcmp(a.xfPos, b.xfPos, sizeof a.xfPos) == 0 &&
                std::memcmp(a.xfNrm, b.xfNrm, sizeof a.xfNrm) == 0 && std::memcmp(a.xfPost, b.xfPost, sizeof a.xfPost) == 0 &&
                std::memcmp(a.xfLight, b.xfLight, sizeof a.xfLight) == 0 && std::memcmp(a.xfReg, b.xfReg, sizeof a.xfReg) == 0 &&
                std::memcmp(a.vcd, b.vcd, sizeof a.vcd) == 0 && std::memcmp(a.vat, b.vat, sizeof a.vat) == 0 &&
                std::memcmp(a.cpBase, b.cpBase, sizeof a.cpBase) == 0 && std::memcmp(a.cpStride, b.cpStride, sizeof a.cpStride) == 0 &&
                a.matIdxA == b.matIdxA && a.matIdxB == b.matIdxB && a.bpMask == b.bpMask && a.keyHash == b.keyHash &&
                std::memcmp(a.tevRaw, b.tevRaw, sizeof a.tevRaw) == 0 && a.tevOor == b.tevOor && a.ltCos1 == b.ltCos1 && a.ltDist1 == b.ltDist1 &&
                std::memcmp(&a.dispCopySrc, &b.dispCopySrc, sizeof a.dispCopySrc) == 0 && std::memcmp(&a.texCopySrc, &b.texCopySrc, sizeof a.texCopySrc) == 0;
    return same;
}
Vgx2Seg g_verify; // (GXV_FE_TWOCORE=2) the segment whose end the player has still to compare at
// (twocore25) the register file of a hand-over (its first two objects: State before and after its palette slots), as a State for the
// comparisons; the slots are left as they are (none is compared)
constexpr size_t kStateA = offsetof(State, tlut), kStateB = offsetof(State, tlut) + sizeof(State::tlut);
const State& SnapState(const uint8_t* snap) {
    static State tmp;
    std::memcpy(static_cast<void*>(&tmp), snap, kStateA);
    std::memcpy(reinterpret_cast<uint8_t*>(&tmp) + kStateB, snap + kStateA, sizeof(State) - kStateB);
    return tmp;
}
// (the first register that differs, for the log)
const char* FirstDiff(const State& b, char* buf, size_t n) {
    const State& a = g;
    for (int i = 0; i < 256; ++i) if (a.bp[i] != b.bp[i]) { std::snprintf(buf, n, "BP %02X: %06X, B %06X", i, a.bp[i], b.bp[i]); return buf; }
    for (int i = 0; i < 0x58; ++i) if (a.xfReg[i] != b.xfReg[i]) { std::snprintf(buf, n, "XF %04X: %08X, B %08X", 0x1000 + i, a.xfReg[i], b.xfReg[i]); return buf; }
    for (int r = 0; r < 64; ++r)
        if (std::memcmp(a.xfPos[r], b.xfPos[r], sizeof a.xfPos[r])) {
            std::snprintf(buf, n, "XF matrix row %d: %g %g %g %g, B %g %g %g %g", r, a.xfPos[r][0], a.xfPos[r][1], a.xfPos[r][2], a.xfPos[r][3], b.xfPos[r][0],
                          b.xfPos[r][1], b.xfPos[r][2], b.xfPos[r][3]);
            return buf;
        }
    if (std::memcmp(a.xfNrm, b.xfNrm, sizeof a.xfNrm)) return "XF normal matrices";
    if (std::memcmp(a.xfPost, b.xfPost, sizeof a.xfPost)) return "XF post matrices";
    if (std::memcmp(a.xfLight, b.xfLight, sizeof a.xfLight)) return "XF lights";
    if (std::memcmp(a.tevReg, b.tevReg, sizeof a.tevReg) || std::memcmp(a.tevKonst, b.tevKonst, sizeof a.tevKonst)) return "TEV colours";
    if (std::memcmp(a.vcd, b.vcd, sizeof a.vcd) || std::memcmp(a.vat, b.vat, sizeof a.vat)) return "vertex descriptor / formats";
    if (std::memcmp(a.cpBase, b.cpBase, sizeof a.cpBase) || std::memcmp(a.cpStride, b.cpStride, sizeof a.cpStride)) return "array bases";
    if (a.matIdxA != b.matIdxA || a.matIdxB != b.matIdxB) return "matrix indices";
    if (a.keyHash != b.keyHash) return "key hash";
    if (std::memcmp(a.tevRaw, b.tevRaw, sizeof a.tevRaw)) return "TEV raw halves";
    return "copy set-up or facts";
}
} // namespace

void SetThreadInit(int (*fn)(int core)) { g_threadInit = fn; }
void Init() { if (!g_started) Start(); }

void Published(const uint8_t* p, const uint8_t* end) {
    g_callEnd = end;
    // (twocore24: not a heuristic any more. It kept the old end when the new one lay up to 16 MiB below it, which is what the console's other
    // FIFO segment is (gx_fifo.cpp: two 2 MiB allocations in a row): after a change to the lower segment B's end stayed in the upper one,
    // and B decoded bytes the game had not written yet; a segment taken from them made the player go on past what was published)
    if (!g_active) return;
    if (p < g_picStart || p > g_lastEnd) {
        // (other memory: the game moved to its other segment in the middle of the picture; B's pointers are into the old one. B keeps
        // its slots until it has let go of the picture: the picture's end waits for that, bounded, g_bGiven)
        vgx2_cancel.store(1, std::memory_order_seq_cst);
        g_active = false;
        ++g_tot.cancelled;
        g_lastEnd = end;
        return;
    }
    if (end > g_lastEnd) g_lastEnd = end;
    vgx2_pub_end.store(g_lastEnd, std::memory_order_release);
}

// (hosts whose player is handed the stream in pieces as it is made, e.g. tests/vgx_fe_work.cpp: the bytes up to `end` are there already,
// as the console's player has everything the game published; the next handing over must be in the same memory)
void PublishedUpTo(const uint8_t* end) {
    if (!vgx2_mode) return;
    if (!g_active) { g_upTo = end; return; } // (taken by the next picture's start when it lies in that picture's memory)
    if (end > g_lastEnd && end - g_picStart < (1 << 26)) g_lastEnd = end;
    vgx2_pub_end.store(g_lastEnd, std::memory_order_release);
}

namespace {
// The slot the player has finished with (taken, declined or missed): its chain back to the pool, free for B.
void Release(uint32_t slot) {
    Vgx2Seg& s = vgx2_seg[slot];
    if (s.firstChunk) vgx2_a_split_drop(s.firstChunk);
    s.firstChunk = s.lastChunk = nullptr;
    vgx2_stop_at[slot].store(nullptr, std::memory_order_relaxed);
    vgx2_seg_state[slot].store(kSegNone, std::memory_order_release);
}
// (twocore24) B has let go of the picture it was given (it is idle; vgx2_cancel is up, so it cannot take it again): its slots are the
// player's again. Called only with B idle.
void TakeBackSlots() {
    for (uint32_t i = 0; i < VGX2_SLOTS; ++i) {
        if (vgx2_seg_state[i].load(std::memory_order_acquire) != kSegNone) ++g_tot.lost;
        Release(i);
    }
    g_bGiven = false;
}
} // namespace

// (twocore25) a picture in play: a stage, no loading, no closed screen or menu. Without the game's facts (host tests) every picture is.
bool InPlay() {
    if (g_testQuiet > 0 && g_picsSeen <= static_cast<uint64_t>(g_testQuiet)) return false;
    if (port_scene_key && port_scene_key() == 0) return false;
    if (port_screen_quiet && port_screen_quiet() != 0) return false;
    if (port_scene_loading_active && port_scene_loading_active() != 0) return false;
    return true;
}
void StartPicture(const uint8_t* first) {
    g_needStart = false;
    g_active = false;
    int why = !vgx2_mode ? -1 : g_tripped ? static_cast<int>(kNoBOff) : Refusal();
    if (vgx2_mode && !g_tripped) {
        // (twocore25: play only, after it has settled; and not while resting)
        ++g_picsSeen;
        if (!InPlay()) { g_playRun = 0; if (why == 0) why = kNoBQuiet; }
        else if (++g_playRun <= static_cast<uint64_t>(port_scene_key || g_testQuiet ? g_settle : 0) && why == 0) why = kNoBSettle; // (host: none)
        if (why == 0 && g_restLeft > 0) { --g_restLeft; why = kNoBRest; }
    }
    if (why == 0 && g_bGiven) {
        // (B has not let go of an earlier picture: the picture's end did not wait for it (twocore25); a short wait here, then the picture goes
        // without B; B's slots stay B's until it is idle)
        bool busy = vgx2_b_busy.load(std::memory_order_seq_cst) != 0;
        if (busy && g_startNs > 0) {
            const int64_t t0 = NowNs();
            for (unsigned spins = 0; busy; ++spins) {
                busy = vgx2_b_busy.load(std::memory_order_seq_cst) != 0;
                if (!busy || (spins & 31) != 31) continue;
                if (NowNs() - t0 > g_startNs) break;
            }
            g_tot.endWaitNs += NowNs() - t0;
        }
        if (busy) why = kNoBBusy;
        else TakeBackSlots();
    }
    if (why == kNoBBusy) {
        if (++g_busyRun == 30)
            Trip("the second worker has not let go of a picture for 30 pictures: it is stuck, or its core never runs it");
    } else if (why == 0) {
        g_busyRun = 0;
    }
    if (why != 0) {
        if (why > 0) ++g_tot.noB[why];
        if (g_tripped && !g_bGiven) Finish();
        return;
    }
    // (B is idle and has let go of every slot: the state it starts from is the player's now, at the picture's first command: the same as at
    // the last picture's end, since no command came in between)
    {
        void* cha = nullptr;
        uint32_t chaN = 0;
        vgx2_a_draw_cha(&cha, &chaN);
        vgx2_b_picture_state(&g, sizeof g, cha, chaN, CurrentFrame());
        vgx2_pic_age = vgx2_a_dl_age_next(CurrentFrame());
    }
    g_active = true;
    g_bGiven = true;
    g_picStart = first;
    // (what B may read: the bytes of the call the player is in, which holds `first`; more with each call, Published; or what a host said is
    // there already, PublishedUpTo, if `first` lies before it in the same piece of memory)
    g_lastEnd = g_callEnd && g_callEnd >= first && g_callEnd - first < (1 << 26) ? g_callEnd : first;
    if (g_upTo && g_upTo > g_lastEnd && g_upTo - first < (1 << 26)) g_lastEnd = g_upTo;
    g_upTo = nullptr;
    vgx2_pub_end.store(g_lastEnd, std::memory_order_release);
    vgx2_pic_start = first;
    vgx2_apos.store(first, std::memory_order_relaxed);
    for (int i = 0; i < VGX2_SLOTS; ++i) { vgx2_seg_state[i].store(kSegNone, std::memory_order_relaxed); vgx2_stop_at[i].store(nullptr, std::memory_order_relaxed); }
    g_aSlot = 0;
    vgx2_cancel.store(0, std::memory_order_relaxed);
    g_aCpu0 = CpuUs();
    g_pic0 = NowNs();
    g_segsThisPic = g_segBytesThisPic = 0;
    const uint32_t pic = vgx2_pic.fetch_add(1, std::memory_order_release) + 1;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cv.notify_all();
    }
    if (__builtin_expect(vgx2_inject == 4, 0)) {
        // (host measurement only: the player waits, asleep, until B has gone through the bytes published so far: B then makes every segment it
        // can, and the player's time is what is left for it with B infinitely early. Bounded: 2 s.)
        const int64_t t0 = NowNs();
        while (vgx2_b_atend.load(std::memory_order_acquire) != pic && NowNs() - t0 < 2000000000ll) Nap(50);
        g_aCpu0 = CpuUs();
    }
}

// The player is past the next segment's start (B published it too late): not used.
void Missed() {
    const uint32_t slot = g_aSlot;
    g_aSlot = (g_aSlot + 1) % vgx2_nslots;
    ++g_tot.lost;
    int st = kSegDecoding;
    if (vgx2_seg_state[slot].compare_exchange_strong(st, kSegDropped, std::memory_order_acq_rel)) {
        vgx2_stop_at[slot].store(nullptr, std::memory_order_relaxed); // (B gives its operations back and frees the slot)
        return;
    }
    if (st == kSegReady) Release(slot);
}

// The player is at a segment's first command.
const uint8_t* Arrive(const uint8_t* cmdStart) {
    const uint32_t slot = g_aSlot;
    g_aSlot = (g_aSlot + 1) % vgx2_nslots;
    std::atomic<int>& state = vgx2_seg_state[slot];
    if (!g_active) return cmdStart;
    const int64_t t0 = NowNs();
    int st = kSegDecoding;
    if (!state.compare_exchange_strong(st, kSegClaimed, std::memory_order_acq_rel)) {
        if (st != kSegReady) return cmdStart; // (none: cannot be, the address was published)
    } else {
        // (B stops at its next command: a few microseconds; a preempted B is not waited for: after GXV_FE_TWOCORE_WAIT_US the player
        // declines and does the bytes itself. Never while the player holds the shared lock, which B may be waiting for.)
        const int64_t limit = g_aDepth > 0 ? 0 : g_waitNs;
        for (unsigned spins = 0;; ++spins) {
            st = state.load(std::memory_order_acquire);
            if (st == kSegReady) break;
            if ((spins & 63) == 63 || limit == 0) {
                const int64_t w = NowNs() - t0;
                if (w >= limit) {
                    int c = kSegClaimed;
                    if (state.compare_exchange_strong(c, kSegDropped, std::memory_order_acq_rel)) {
                        vgx2_stop_at[slot].store(nullptr, std::memory_order_relaxed);
                        ++g_tot.declinedWait;
                        ++g_lateWin;
                        g_tot.waitNs += NowNs() - t0;
                        return cmdStart; // (B frees the slot)
                    }
                    st = c;
                    if (st == kSegReady) break;
                }
                if (w > 100000) Nap(20); // (B may share this core: GXV_FE_TWOCORE_CORE=2)
            }
        }
    }
    const int64_t t1 = NowNs();
    g_tot.waitNs += t1 - t0;
    Vgx2Seg& s = vgx2_seg[slot];
    ++g_tot.segs;
    ++g_tot.whyEnd[s.why & 7];
    if (s.why == 3) ++g_tot.listWhy[s.listWhy < 10 ? s.listWhy : 0];
    const uint8_t* next = cmdStart;
    // (twocore24: a segment never ends past what the player has been handed, nor before it starts; if it did, it is not taken)
    const bool inRange = s.e >= s.s && s.e <= g_lastEnd;
    bool same = s.s == cmdStart && inRange;
    if (!inRange) ++g_tot.lost;
    for (int i = 0; i < VGX2_EP_COUNT && same; ++i) same = s.epochs[i] == vgx2_epoch[i].load(std::memory_order_relaxed);
    if (!same && inRange) ++g_tot.declinedEpoch;
    else if (s.e == s.s) ++g_tot.empty;
    if (same && s.e != s.s && vgx2_mode == 2) {
        // (verify: B's register state at the start, now; at the end, when the player gets there)
        g_tot.bytesMade += static_cast<uint64_t>(s.e - s.s);
        ++g_tot.verS;
        if (s.snapS.size() == sizeof(State) && !SameRegisters(*reinterpret_cast<const State*>(s.snapS.data()))) {
            char d[96];
            if (++g_tot.verSBad <= 16) Log("fetwo: MISMATCH frame %llu: B's registers at a segment's start (offset %u) differ from the player's (%s)",
                                           static_cast<unsigned long long>(CurrentFrame()), static_cast<unsigned>(s.s - g_picStart),
                                           FirstDiff(*reinterpret_cast<const State*>(s.snapS.data()), d, sizeof d));
        }
        g_verify.snap.swap(s.snap);
        g_verify.e = s.e;
        g_verifyAt = s.e;
        g_verifyT0 = NowNs();
        g_verifyBNs = s.decodeNs;
    }
    if (same && s.e != s.s && vgx2_mode == 1) {
        // ---- take the segment
        const uint32_t base = vgx2_a_split_splice(s.firstChunk, s.lastChunk, s.lastPos, s.ops, s.jobs);
        s.firstChunk = s.lastChunk = nullptr;
        CmdBuf& buf = Pending();
        for (uint32_t j : s.cmdJobs) buf.cmds.push_back(Cmd{CMD_DRAW, base + j});
        for (size_t i = 0; i + 1 < s.tlutLoads.size(); i += 2) vgx2_a_tlut_replay(s.tlutLoads[i], s.tlutLoads[i + 1]);
        if (!s.dlUses.empty() || !s.texUses.empty() || !s.dlRehash.empty() || !s.dlDrawn.empty()) {
            vgx2_lock_a();
            for (size_t i = 0; i + 14 <= s.dlRehash.size(); i += 14)
                vgx2_a_dl_rehash(reinterpret_cast<void*>(static_cast<uintptr_t>(s.dlRehash[i])), s.dlRehash[i + 1], &s.dlRehash[i + 2]);
            for (void* e : s.dlUses) vgx2_a_dl_mark(e, vgx2_pic_age);
            for (void* e : s.dlDrawn) vgx2_a_dl_drawn(e);
            for (void* e : s.texUses) vgx2_a_tex_mark(e, CurrentFrame());
            vgx2_unlock_a();
        }
        if (s.usedAge) vgx2_a_dl_age(vgx2_pic_age, CurrentFrame());
        for (size_t i = 0; i + 3 < s.chaBoxes.size(); i += 4) vgx2_a_cha_add(s.chaBoxes[i], s.chaBoxes[i + 1], s.chaBoxes[i + 2], s.chaBoxes[i + 3]);
        if (Adopt(s)) {
            next = s.e;
            ++g_tot.taken;
            g_tot.bytesTaken += static_cast<uint64_t>(s.e - s.s);
            g_tot.bytesMade += static_cast<uint64_t>(s.e - s.s);
            g_tot.staticRuns += s.staticRuns;
            g_tot.jobs += s.jobs;
            g_tot.lists += s.lists;
            g_tot.bDecodeNs += s.decodeNs;
            if (g_segsThisPic < 8) {
                g_segAt[g_segsThisPic][0] = static_cast<uint32_t>(s.s - g_picStart);
                g_segAt[g_segsThisPic][1] = static_cast<uint32_t>(s.e - g_picStart);
            }
            ++g_segsThisPic;
            g_segBytesThisPic += static_cast<uint32_t>(s.e - s.s);
        }
    }
    Release(slot);
    vgx2_apos.store(next, std::memory_order_relaxed);
    g_tot.adoptNs += NowNs() - t1;
    return next;
}

// GXV_FE_TWOCORE=2: the player is where a segment B made ended.
void VerifyAt(const uint8_t* cmdStart) {
    g_verifyAt = nullptr;
    g_tot.verANs += NowNs() - g_verifyT0;
    g_tot.verBNs += g_verifyBNs;
    (void)cmdStart;
    Vgx2Obj a[32];
    const int n = AllObjs(false, a);
    ++g_tot.verE;
    // (the register file is the first two objects, the resolved state the third; the player's dirty flags and memos depend on its history)
    const bool ok = n >= 3 && a[0].n == kStateA && a[1].n == sizeof(State) - kStateB && g_verify.snap.size() >= a[0].n + a[1].n + a[2].n;
    const bool regs = ok && SameRegisters(SnapState(g_verify.snap.data()));
    const uint32_t res = ok ? vgx2_a_draw_compare(g_verify.snap.data() + a[0].n + a[1].n) : ~0u;
    if (!regs || res) {
        if (++g_tot.verEBad <= 16)
            Log("fetwo: MISMATCH frame %llu: at a segment's end B's %s%s differ from the player's (resolved state fields 0x%X)", static_cast<unsigned long long>(CurrentFrame()),
                regs ? "" : "registers", res ? (regs ? "resolved state" : " and resolved state") : "", res);
    }
}

void PictureEnd() {
    if (!g_started || !vgx2_mode) return;
    ++g_tot.pictures;
    if (g_bGiven) {
        // (B lets go of the picture: it looks at vgx2_cancel before every command and in every wait. Bounded: a B its core does not run,
        // or one that is stuck, is left behind; it gets no picture until it is idle, StartPicture)
        // twocore25: the wait is short (GXV_FE_TWOCORE_END_US, now 200 us) and running out of it is no longer a hard timeout: it only means the
        // next picture goes without B unless B is idle by then ("let go late"). A B that stays in a picture is still caught: 30 pictures in a
        // row without B for that reason turn the switch off (StartPicture). On the console, at B's priority, the old 2 ms wait ran out
        // 10-25 times in 300 pictures, cost the player 0.2-0.4 ms a picture, and four of them turned the switch off on the loading screens.
        vgx2_cancel.store(1, std::memory_order_seq_cst);
        const int64_t t0 = NowNs();
        bool idle = !vgx2_b_busy.load(std::memory_order_seq_cst);
        if (!idle) {
            std::lock_guard<std::mutex> lk(g_mtx); // (twocore25: B asleep in its wait for bytes wakes at once, vgx2_b_nap)
            g_cv.notify_all();
        }
        for (unsigned spins = 0; !idle && g_aDepth == 0 && !g_tripped; ++spins) { // (off for the session: B is only looked at, never waited for)
            idle = !vgx2_b_busy.load(std::memory_order_seq_cst);
            if (idle || (spins & 31) != 31) continue;
            const int64_t w = NowNs() - t0;
            if (w > g_endNs) break;
            if (w > 50000) Nap(20);
        }
        const int64_t w = NowNs() - t0;
        g_tot.endWaitNs += w;
        if (idle) TakeBackSlots();
        else ++g_tot.endLate;
    }
    if (g_active) {
        ++g_tot.active;
        g_aSlot = 0;
        g_verifyAt = nullptr;
        g_tot.bytesPic += static_cast<uint64_t>(g_lastEnd - g_picStart);
        // (twocore25: the use window: B rests when it made too little of the pictures it was given)
        static uint64_t madeAt = 0;
        ++g_useWinPics;
        g_useWinBytes += static_cast<uint64_t>(g_lastEnd - g_picStart);
        g_useWinMade += g_tot.bytesMade - madeAt;
        madeAt = g_tot.bytesMade;
        if (g_useWinPics >= static_cast<uint64_t>(g_useWin)) {
            const double share = g_useWinBytes ? 100.0 * static_cast<double>(g_useWinMade) / static_cast<double>(g_useWinBytes) : 0.0;
            if (g_minShare > 0 && share < g_minShare) {
                g_restLen = g_restLen ? std::min<uint64_t>(g_restLen * 2, static_cast<uint64_t>(g_restBase) * 16) : static_cast<uint64_t>(g_restBase);
                g_restLeft = g_restLen;
                if (++g_tot.rests <= 8)
                    Log("fetwo: the second worker rests for %llu pictures: it made %.1f %% of the bytes of the last %llu pictures it was given "
                        "(GXV_FE_TWOCORE_MINSHARE=%.0f); it gets no picture meanwhile (asleep)", static_cast<unsigned long long>(g_restLen), share,
                        static_cast<unsigned long long>(g_useWinPics), g_minShare);
            } else {
                g_restLen = 0;
            }
            g_useWinPics = g_useWinMade = g_useWinBytes = 0;
        }
        if (vgx2_dbg && !g_bGiven) {
            // (debug: the first command where B's scan had other registers than the player; B is idle)
            size_t ib = 0;
            static int said = 0;
            for (size_t ia = 0; ia + 1 < vgx2_dbg_a.size() && said < 20; ia += 2) {
                while (ib + 1 < vgx2_dbg_b.size() && vgx2_dbg_b[ib] < vgx2_dbg_a[ia]) ib += 2;
                if (ib + 1 >= vgx2_dbg_b.size()) break;
                if (vgx2_dbg_b[ib] == vgx2_dbg_a[ia] && vgx2_dbg_b[ib + 1] != vgx2_dbg_a[ia + 1]) {
                    const uint8_t* c = g_picStart + vgx2_dbg_a[ia];
                    const uint64_t prev = ia >= 2 ? vgx2_dbg_a[ia - 2] : 0;
                    const uint8_t* pc = g_picStart + prev;
                    Log("fetwo: DEBUG frame %llu: registers differ (parts %016llx / %016llx: bp xfmtx xfnrm/post light xfreg vcd/vat cp idx/mask) before the command at %llu (%02X %02X %02X %02X %02X); the command before at %llu: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        (unsigned long long)CurrentFrame(), (unsigned long long)vgx2_dbg_a[ia + 1], (unsigned long long)vgx2_dbg_b[ib + 1], (unsigned long long)vgx2_dbg_a[ia], c[0], c[1], c[2], c[3], c[4], (unsigned long long)prev,
                        pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7], pc[8]);
                    if (pc[0] == 0x40) {
                        const uint32_t addr = (uint32_t(pc[1]) << 24) | (uint32_t(pc[2]) << 16) | (uint32_t(pc[3]) << 8) | pc[4];
                        const uint32_t size = (uint32_t(pc[5]) << 24) | (uint32_t(pc[6]) << 16) | (uint32_t(pc[7]) << 8) | pc[8];
                        uint32_t avail = 0;
                        const uint8_t* l = Hooks().resolve ? Hooks().resolve(addr, &avail) : nullptr;
                        char ops[400] = "";
                        size_t o = 0;
                        for (uint32_t i = 0; l && i < size && i < 64 && o + 4 < sizeof ops; ++i) o += std::snprintf(ops + o, sizeof ops - o, "%02X ", l[i]);
                        Log("fetwo: DEBUG   the list %08X (%u bytes) starts: %s", addr, size, ops);
                    }
                    ++said;
                    break;
                }
            }
            vgx2_dbg_a.clear();
            vgx2_dbg_b.clear(); // (B is idle: a picture it does not get leaves nothing behind)
        }
        const int64_t a = CpuUs() - g_aCpu0;
        g_tot.aCpuUs += a;
        const int64_t bNow = g_bCpu.load(std::memory_order_relaxed);
        static int64_t bLast = 0;
        const int64_t bPic = bNow - bLast;
        g_tot.bCpuUs += bPic;
        bLast = bNow;
        if (g_logEvery == 1) {
            char at[8 * 24 + 8];
            size_t k = 0;
            at[0] = 0;
            for (uint32_t i = 0; i < g_segsThisPic && i < 8 && k < sizeof at - 24; ++i)
                k += static_cast<size_t>(std::snprintf(at + k, sizeof at - k, " [%u,%u)", g_segAt[i][0], g_segAt[i][1]));
            if (g_segsThisPic > 8 && k < sizeof at - 4) std::snprintf(at + k, sizeof at - k, " ...");
            Log("fetwo: picture %llu: FE cpu core 2 %.2f ms, core 3 %.2f ms; %u bytes, %u segments taken (%u bytes):%s",
                static_cast<unsigned long long>(CurrentFrame() - 1), a / 1e3, bPic / 1e3, static_cast<unsigned>(g_lastEnd - g_picStart),
                g_segsThisPic, g_segBytesThisPic, at);
        }
        g_active = false;
    }
    g_aSlot = 0;
    g_verifyAt = nullptr;
    // (the state the next picture starts from is given to B at its first command, StartPicture, when B is idle)
    g_needStart = true;
    // (twocore24: the windows of the switch's own off rule)
    if (++g_hardWinPics >= 600) g_hardWinPics = g_hardWin = 0;
    if (++g_lateWinPics >= 300) {
        if (!g_tripped && g_tripLate > 0 && static_cast<double>(g_lateWin) > g_tripLate * static_cast<double>(g_lateWinPics)) {
            static char why[160];
            std::snprintf(why, sizeof why, "B was late for %.1f segments a picture over the last %llu pictures (GXV_FE_TWOCORE_TRIP_LATE=%.1f): its core "
                          "has no room for it", static_cast<double>(g_lateWin) / static_cast<double>(g_lateWinPics),
                          static_cast<unsigned long long>(g_lateWinPics), g_tripLate);
            Trip(why);
        }
        g_lateWinPics = g_lateWin = 0;
    }
    g_tot.lockWaits = g_lockWaits;
    g_tot.lockWaitNs = g_lockWaitNs;
    if (g_tripped) Finish();
    if (vgx2_mode) Report(false);
}
void ReportNow() { if (vgx2_mode) Report(true); }

// (twocore24) a bounded wait for B ran out: 0 the shared lock, 1 B letting go of a picture. GXV_FE_TWOCORE_TRIP of them within 600
// pictures turn the switch off.
void HardTimeout(int what, int64_t ns) {
    ++g_tot.hard[what & 1];
    static int said = 0;
    if (said < 4) {
        ++said;
        Log("fetwo: hard timeout: %s for %.2f ms (GXV_FE_TWOCORE_%s_US)", what == 0 ? "the player waited for the second worker's lock"
                                                                                      : "the second worker did not let go of the picture",
            ns / 1e6, what == 0 ? "LOCK" : "END");
    }
    if (++g_hardWin >= static_cast<uint64_t>(g_tripHard))
        Trip(what == 0 ? "the player had to wait for the second worker's lock too long, too often (GXV_FE_TWOCORE_LOCK_US, GXV_FE_TWOCORE_TRIP)"
                       : "the second worker did not let go of pictures in time, too often (GXV_FE_TWOCORE_END_US, GXV_FE_TWOCORE_TRIP): its "
                         "core does not run it, or it is stuck");
}

} // namespace two
} // namespace vgx

// ---- B's side of the waits (vgx_twocore_b.cpp)
extern "C" int vgx2_wait_picture(uint32_t seen) {
    using namespace vgx::two;
    if (vgx2_pic.load(std::memory_order_acquire) != seen) return 1;
    vgx2_b_unboost();
    // (twocore25: asleep, no spin first: StartPicture notifies; the timeout only bounds a missed notification. B's CPU while it gets no
    // picture: a wake-up every 20 ms)
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait_for(lk, std::chrono::milliseconds(20), [seen] {
        return vgx2_pic.load(std::memory_order_acquire) != seen || vgx2_b_exit.load(std::memory_order_acquire);
    });
    return vgx2_pic.load(std::memory_order_acquire) != seen;
}
// (twocore25) B's wait for more published bytes: asleep until the bytes move, the picture ends (PictureEnd notifies) or `us` pass. Before:
// a plain sleep, so a picture's end waited for the rest of B's nap (the player's "for B to let go" was 0.2-0.4 ms a picture on the console).
extern "C" void vgx2_b_nap(uint32_t us, const uint8_t* avail) {
    using namespace vgx::two;
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait_for(lk, std::chrono::microseconds(us), [avail] {
        return vgx2_cancel.load(std::memory_order_acquire) != 0 || vgx2_pub_end.load(std::memory_order_acquire) != avail;
    });
}
extern "C" void vgx2_b_cpu_note(void) {
    vgx::two::g_bCpu.store(vgx::two::CpuUs(), std::memory_order_relaxed);
}
