// Galaxy 3.0 multiplayer (port side, hwaids): aids for the first session on a real console, none of them a taiHEN plugin.
//   1. "[mp] mem <when>: ..." lines: Vita user / cdram / phycont free memory (with the lowest seen), the C heap (newlib's mallinfo, against the
//      heap's fixed size), and on the game thread vitaGL's pools (port_vgx_vita_memory_brief). Desktop: resident memory and the heap. Written
//      at the session's start and end (hook in mp_log.cpp's mp_heap_note), at every scene start of a session (MpPuppets.cpp), 3 s and 15 s
//      after a resume, and every 30 s while a session runs.
//   2. The resume path: after a resume (the real one, mp_core_vita.cpp, or the simulated one) one line says what the network control layer
//      reports (state, IP address) and the memory. SMG_MP_SIM_RESUME=<seconds> (env.txt or the environment; also works on the console)
//      makes the helper believe the console slept that many seconds into every session (SMG_MP_SIM_RESUME_COUNT=<n>, default 1, how many
//      times); what follows is the real code: BYE to the others, "connection lost", the rejoin.
//   3. A UDP mirror of the port log: SMG_LOG_UDP=<ip>:<port> (default off). Each log line is copied into a fixed ring; a small thread of
//      its own sends them in datagrams of up to 1200 bytes, only when the network is up (a socket is opened per batch and closed again, so
//      this never keeps the network library from being shut down). The game never waits: a busy ring lock or a full ring drops lines (the
//      next datagram begins with "[mirror: N lines lost]"). It never starts the network library itself. Receive on the Mac: `nc -ul <port>`.
//   4. SMG_VITA_BUDGET_MIB=<n> (desktop): every memory line says how much of an n MiB C heap the game uses (the Vita's is 96). The game's own
//      banks are capped by PETARI_VITA_LIMITS=1 (os_mem.cpp, already there).
// Every entry point does nothing (one relaxed load) unless its switch is set, and single player is not touched.
#include "mp_hwaids_logic.h"
#include "mp_core_sys.h"

#include "port/mp_bridge.h"
#include "mp_netctl_guard.h"
#include "port/port.h"          // port_thread_helper (the log flusher's core placement)
#include "port/qemu_hooks.h"    // PORT_THREAD_NAME

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <malloc.h>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#pragma weak sceNetSocket
#pragma weak sceNetSocketClose
#pragma weak sceNetSetsockopt
#pragma weak sceNetSendto
#pragma weak sceNetCtlInetGetState
#pragma weak sceNetCtlInetGetInfo
extern "C" unsigned int _newlib_heap_size_user __attribute__((weak));
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

extern "C" int port_vgx_vita_memory_brief(char* out, unsigned size) __attribute__((weak));   // vgx_host_galaxy_vita.cpp (game thread only)

namespace {

uint64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

long EnvLong(const char* name, long fallback, long lo, long hi) {
    const char* e = getenv(name);
    if (!e || !*e) return fallback;
    long v = atol(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// ---- memory line -------------------------------------------------------------------------------------------------------------------------
std::atomic<int> g_minUser{0x7FFFFFFF}, g_minCdram{0x7FFFFFFF}, g_minPhy{0x7FFFFFFF};
void NoteMin(std::atomic<int>& m, int v) {
    int cur = m.load(std::memory_order_relaxed);
    while (v < cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

struct HeapNums { unsigned usedKiB = 0, arenaKiB = 0, freeKiB = 0, pieces = 0; };
HeapNums ReadHeap() {
    HeapNums h;
#if defined(__GLIBC__) && !defined(__vita__)
    const struct mallinfo2 mi = mallinfo2();
    h.arenaKiB = (unsigned)((mi.arena + mi.hblkhd) / 1024u);
    h.usedKiB = (unsigned)((mi.uordblks + mi.hblkhd) / 1024u);
    h.freeKiB = (unsigned)(mi.fordblks / 1024u);
    h.pieces = (unsigned)mi.ordblks;
#else
    const struct mallinfo mi = mallinfo();
    h.arenaKiB = (unsigned)((unsigned)mi.arena / 1024u);
    h.usedKiB = (unsigned)((unsigned)mi.uordblks / 1024u);
    h.freeKiB = (unsigned)((unsigned)mi.fordblks / 1024u);
    h.pieces = (unsigned)mi.ordblks;
#endif
    return h;
}

#ifndef __vita__
// VmRSS / VmHWM of this process, KiB (0 when /proc is not there)
void ReadRss(unsigned* rss, unsigned* peak) {
    *rss = *peak = 0;
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        unsigned v = 0;
        if (sscanf(line, "VmRSS: %u kB", &v) == 1) *rss = v;
        else if (sscanf(line, "VmHWM: %u kB", &v) == 1) *peak = v;
    }
    fclose(f);
}
#endif

void MemoryLine(const char* when, bool gameThread) {
    const HeapNums h = ReadHeap();
#ifdef __vita__
    SceKernelFreeMemorySizeInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.size = sizeof fi;
    if (sceKernelGetFreeMemorySize(&fi) < 0) {
        mp_log("mem %s: free memory unreadable; C heap used %u KiB (arena %u, free %u in %u pieces)", when, h.usedKiB, h.arenaKiB, h.freeKiB, h.pieces);
    } else {
        NoteMin(g_minUser, fi.size_user);
        NoteMin(g_minCdram, fi.size_cdram);
        NoteMin(g_minPhy, fi.size_phycont);
        const unsigned cap = &_newlib_heap_size_user ? _newlib_heap_size_user / 1024u : 0;
        mp_log("mem %s: free user %d cdram %d phycont %d KiB (lowest seen %d / %d / %d); C heap used %u of %u KiB, arena %u, free %u KiB in %u pieces", when,
               fi.size_user / 1024, fi.size_cdram / 1024, fi.size_phycont / 1024, g_minUser.load(std::memory_order_relaxed) / 1024,
               g_minCdram.load(std::memory_order_relaxed) / 1024, g_minPhy.load(std::memory_order_relaxed) / 1024, h.usedKiB, cap, h.arenaKiB, h.freeKiB,
               h.pieces);
    }
#else
    unsigned rss = 0, peak = 0;
    ReadRss(&rss, &peak);
    static const long budget = EnvLong("SMG_VITA_BUDGET_MIB", 0, 0, 4096);
    if (budget > 0) {
        const unsigned pct = (unsigned)((uint64_t)h.usedKiB * 100u / ((uint64_t)budget * 1024u));
        mp_log("mem %s: resident %u KiB (peak %u); C heap used %u KiB (arena %u, free %u in %u pieces); Vita budget %ld MiB: %u%%%s (64-bit objects are larger "
               "than the console's)", when, rss, peak, h.usedKiB, h.arenaKiB, h.freeKiB, h.pieces, budget, pct, pct >= 100 ? " OVER" : "");
    } else {
        mp_log("mem %s: resident %u KiB (peak %u); C heap used %u KiB (arena %u, free %u in %u pieces)", when, rss, peak, h.usedKiB, h.arenaKiB, h.freeKiB,
               h.pieces);
    }
#endif
    if (gameThread && port_vgx_vita_memory_brief) {   // (vitaGL's pool walk is the game thread's: its scene-destroy line does the same)
        char gl[640];
        gl[0] = 0;
        port_vgx_vita_memory_brief(gl, sizeof gl);
        // (mp_log cuts a line at 512 bytes: the renderer's brief is longer, so it goes out in two)
        const size_t glen = strlen(gl);
        if (glen > 380) {
            size_t cut = 380;
            while (cut > 200 && gl[cut] != ';') --cut;
            const char keep = gl[cut];
            gl[cut] = 0;
            mp_log("mem %s gl: %s", when, gl);
            gl[cut] = keep;
            mp_log("mem %s gl (cont.): %s", when, gl + cut + (keep == ';' ? 2 : 0));
        } else if (gl[0]) {
            mp_log("mem %s gl: %s", when, gl);
        }
    }
}

// ---- the helper thread's stack (Vita) ---------------------------------------------------------------------------------------------------
// The SDK gives no high-water mark, so the thread paints the unused part of its own stack with a pattern the first time it is seen
// (mp_aids_tick runs on it) and a later look finds how far the pattern was overwritten. A new thread (a new session) is painted anew.
#ifdef __vita__
constexpr uint32_t kPaint = 0xA5C3A5C3u;
SceUID g_paintedThread = -1;
uint32_t* g_paintedLo = nullptr;
bool CanaryOn() {
    static const bool on = [] { const char* e = getenv("SMG_MP_STACK_CANARY"); return e && *e && *e != '0'; }();   // default off
    return on;
}
bool StackBounds(uint32_t** lo, uint32_t* words) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0 || !info.stack || info.stackSize < 8192) return false;
    *lo = static_cast<uint32_t*>(info.stack);
    *words = (uint32_t)info.stackSize / 4u;
    return true;
}
__attribute__((noinline)) void StackPaint() {
    if (!CanaryOn()) return;
    const SceUID me = sceKernelGetThreadId();
    uint32_t* lo;
    uint32_t words;
    if (!StackBounds(&lo, &words)) return;
    if (me == g_paintedThread && lo == g_paintedLo) return;   // (a reused thread id with another stack is painted anew)
    uint32_t* sp = (uint32_t*)__builtin_frame_address(0);
    if (sp <= lo + 512 || sp >= lo + words) return;           // not inside the stack: nothing is touched
    for (uint32_t* w = lo; w < sp - 256; ++w) *w = kPaint;   // (a KiB of margin below this frame is left alone)
    g_paintedThread = me;
    g_paintedLo = lo;
}
void StackLine(const char* when) {
    if (!CanaryOn() || sceKernelGetThreadId() != g_paintedThread) return;
    uint32_t* lo;
    uint32_t words;
    if (!StackBounds(&lo, &words) || lo != g_paintedLo) return;
    uint32_t untouched = 0;
    while (untouched < words && lo[untouched] == kPaint) ++untouched;
    const unsigned peakKiB = (words - untouched) * 4u / 1024u, sizeKiB = words * 4u / 1024u;
    mp_log("mem %s: helper thread stack peak %u of %u KiB%s", when, peakKiB, sizeKiB, peakKiB * 4u > sizeKiB * 3u ? " (OVER 75%)" : "");
}
#else
void StackPaint() {}
void StackLine(const char*) {}
#endif

// ---- resume ------------------------------------------------------------------------------------------------------------------------------
void NetLine(const char* when) {
#ifdef __vita__
    int state = -1;
    int rc = -1;
    if (mp_netctl_live() && sceNetCtlInetGetState) rc = sceNetCtlInetGetState(&state);
    char ip[16] = {0};
    if (rc >= 0 && mp_netctl_live() && sceNetCtlInetGetInfo) {
        SceNetCtlInfo info;
        memset(&info, 0, sizeof info);
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) {
            memcpy(ip, info.ip_address, 15);
        }
    }
    mp_log("resume %s: netctl call 0x%08X state %d (%s) ip %s", when, (unsigned)rc, state, rc < 0 ? "unreadable" : state == SCE_NETCTL_STATE_CONNECTED ? "connected" : "not connected",
           ip[0] ? ip : "none");
#else
    mp_log("resume %s: (desktop: no network control layer)", when);
#endif
}

std::atomic<uint64_t> g_resumeAt{0};      // NowMs() of the last resume, 0 = none pending
std::atomic<int> g_resumeStage{0};        // 0 none, 1 waiting for +3 s, 2 waiting for +15 s

// ---- simulated resume --------------------------------------------------------------------------------------------------------------------
long g_simSeconds = -1;   // -1: not read yet
long g_simCount = 1;
long g_simFired = 0;
uint64_t g_simSince = 0;  // when the current session was first seen (0 = no session)

// ---- the UDP mirror ----------------------------------------------------------------------------------------------------------------------
std::atomic<bool> g_mirrorOn{false};
std::atomic_flag g_ringLock;
mpaids::LineRing<16384> g_ring;
std::atomic<unsigned> g_lockMisses{0};
uint8_t g_ip[4];
uint16_t g_port = 0;
bool g_threadStarted = false;

// Is the network usable right now? Checked BEFORE a batch is taken from the ring, so nothing is lost while it is not.
bool NetReady() {
#ifdef __vita__
    if (!mp_netctl_live() || !sceNetCtlInetGetState || !sceNetSocket) return false;
    int state = 0;
    return sceNetCtlInetGetState(&state) >= 0 && state == SCE_NETCTL_STATE_CONNECTED;   // (an error: the library is not started yet)
#else
    return true;
#endif
}

bool SendBatch(const char* data, size_t len) {
#ifdef __vita__
    if (!mp_netctl_live() || !sceNetSocket || !sceNetSendto || !sceNetSocketClose || !sceNetSetsockopt) return false;   // rev32 F4: the early-return guard the checker asks for
    const int s = sceNetSocket("smg_logmirror", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (s < 0) return false;
    int one = 1;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
    SceNetSockaddrIn sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = (uint16_t)((g_port >> 8) | (g_port << 8));   // network order
    memcpy(&sa.sin_addr.s_addr, g_ip, 4);                       // the bytes as written: a.b.c.d
    const int n = sceNetSendto(s, data, (unsigned)len, 0, reinterpret_cast<SceNetSockaddr*>(&sa), sizeof sa);
    sceNetSocketClose(s);
    return n >= 0;
#else
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(g_port);
    memcpy(&sa.sin_addr.s_addr, g_ip, 4);
    const ssize_t n = sendto(s, data, len, MSG_DONTWAIT, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa);
    close(s);
    return n >= 0;
#endif
}

void* MirrorMain(void*) {
    PORT_THREAD_NAME("log-mirror");
    static char batch[1280];
    size_t pending = 0;       // a batch taken from the ring that no send has taken yet: kept and retried, never thrown away
    uint64_t holdUntil = 0;   // after a failed send or a network that is not ready: no new try before this
    int placed = 0, tries = 0;
    for (;;) {
        if (!placed && tries < 50) { placed = port_thread_helper(); ++tries; }   // (the same core placement as the log flusher)
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, nullptr);
        if (NowMs() < holdUntil) continue;
        if (!NetReady()) {
            holdUntil = NowMs() + 2000;   // the lines stay in the ring (the oldest go, and are counted, when it is full)
            continue;
        }
        for (int round = 0; round < 3; round++) {
            if (pending == 0) {
                while (g_ringLock.test_and_set(std::memory_order_acquire)) {   // (only this thread may wait for the lock; the game never does)
                    struct timespec y = {0, 200 * 1000};
                    nanosleep(&y, nullptr);
                }
                if (!g_ring.Empty()) {
                    const unsigned lost = g_ring.TakeDropped() + g_lockMisses.exchange(0, std::memory_order_relaxed);
                    int head = 0;
                    if (lost) head = snprintf(batch, sizeof batch, "[mirror: %u lines lost]\n", lost);
                    pending = (size_t)head + g_ring.PopBatch(batch + head, sizeof batch - (size_t)head);
                }
                g_ringLock.clear(std::memory_order_release);
            }
            if (pending == 0) break;
            if (!SendBatch(batch, pending)) {
                holdUntil = NowMs() + 2000;   // the batch stays in `pending` for the next try
                break;
            }
            pending = 0;
        }
    }
    return nullptr;
}

}  // namespace

// Called once at port boot (mp_boot.cpp): reads the mirror switch (env.txt is loaded by now) and starts the thread.
extern "C" void mp_aids_boot(void) {
    const char* t = getenv("SMG_LOG_UDP");
    if (!t || !*t || g_threadStarted) return;
    if (!mpaids::ParseTarget(t, g_ip, &g_port)) {
        mp_log("log mirror: SMG_LOG_UDP is not <a.b.c.d>:<port>: off");
        return;
    }
    if (!mpaids::IsPrivateTarget(g_ip)) {
        mp_log("log mirror: %u.%u.%u.%u is not a private address (10/8, 172.16/12, 192.168/16, 127/8) or is a broadcast: off", g_ip[0], g_ip[1], g_ip[2], g_ip[3]);
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32u << 10);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, &MirrorMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        mp_log("log mirror: the thread did not start (%d): off", rc);
        return;
    }
    pthread_detach(th);
    g_threadStarted = true;
    g_mirrorOn.store(true, std::memory_order_release);
    mp_log("log mirror: on, to %u.%u.%u.%u:%u", g_ip[0], g_ip[1], g_ip[2], g_ip[3], (unsigned)g_port);
}

// Called by the port log for every line (any thread, possibly with the game's allocator busy: no allocation, no waiting).
extern "C" int port_log_mirror_enabled(void) { return g_mirrorOn.load(std::memory_order_relaxed) ? 1 : 0; }
extern "C" void port_log_mirror_line(const char* line, size_t len) {
    if (!g_mirrorOn.load(std::memory_order_relaxed)) return;
    if (g_ringLock.test_and_set(std::memory_order_acquire)) {
        g_lockMisses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ring.Push(line, len);
    g_ringLock.clear(std::memory_order_release);
}

// "mp session start" / "mp session end" / "mp first tls" (mp_log.cpp's mp_heap_note): any thread, so no vitaGL walk.
extern "C" void mp_aids_memory_line(const char* when) { MemoryLine(when ? when : "?", false); }

// A scene of a session has started its players (MpPuppets.cpp, game thread): the full line with vitaGL's pools.
extern "C" void mp_aids_scene(unsigned serial, unsigned napaBytes, unsigned gddrBytes, unsigned cacheBytes) {
    char when[40];
    snprintf(when, sizeof when, "scene start serial %u", serial);
    MemoryLine(when, true);
    // the game's own heaps (the puppet code reads them): scene Napa, scene GDDR, the file cache (Egg Planet is tight there)
    mp_log("mem %s heaps: scene napa free %u KiB, scene gddr free %u KiB, file cache free %u KiB", when, napaBytes / 1024u, gddrBytes / 1024u, cacheBytes / 1024u);
}

// A resume was detected (real or simulated; the network thread, outside the core lock).
extern "C" void mp_aids_resumed(int bits) {
    (void)bits;
    NetLine("detected");
    MemoryLine("after resume", false);
    g_resumeAt.store(NowMs(), std::memory_order_relaxed);
    g_resumeStage.store(1, std::memory_order_relaxed);
}

// One call per network tick (mp_core.cpp's helper loop, outside the core lock). Returns MP_WATCH_RESUMED bits for a simulated resume.
extern "C" int mp_aids_tick(bool inSession, uint64_t nowMs) {
    (void)nowMs;
    int out = 0;
    static uint64_t lastMem = 0;
    StackPaint();
    const uint64_t now = NowMs();
    if (g_simSeconds < 0) {
        g_simSeconds = EnvLong("SMG_MP_SIM_RESUME", 0, 0, 3600);
        g_simCount = EnvLong("SMG_MP_SIM_RESUME_COUNT", 1, 1, 100);
        if (g_simSeconds > 0) mp_log("sim resume: on, %ld s into a session, %ld time(s)", g_simSeconds, g_simCount);
    }
    if (inSession) {
        if (!lastMem) lastMem = now;
        if (now - lastMem >= 30000) {
            lastMem = now;
            MemoryLine("30 s", false);
            StackLine("30 s");
        }
        if (g_simSeconds > 0 && g_simFired < g_simCount) {
            if (!g_simSince) g_simSince = now;
            else if (now - g_simSince >= (uint64_t)g_simSeconds * 1000u) {
                g_simFired++;
                g_simSince = 0;
                mp_log("sim resume: pretending the console slept (%ld of %ld)", g_simFired, g_simCount);
                out |= MP_WATCH_RESUMED;
            }
        }
    } else {
        lastMem = 0;
        g_simSince = 0;
    }
    const int stage = g_resumeStage.load(std::memory_order_relaxed);
    if (stage) {
        const uint64_t since = now - g_resumeAt.load(std::memory_order_relaxed);
        if (stage == 1 && since >= 3000) {
            NetLine("+3 s");
            MemoryLine("resume +3 s", false);
            StackLine("resume +3 s");
            g_resumeStage.store(2, std::memory_order_relaxed);
        } else if (stage == 2 && since >= 15000) {
            NetLine("+15 s");
            MemoryLine("resume +15 s", false);
            g_resumeStage.store(0, std::memory_order_relaxed);
        }
    }
    return out;
}
