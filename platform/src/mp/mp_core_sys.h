// Galaxy 3.0 multiplayer (port side, owner: core): the few platform services the session layer needs: a monotonic millisecond
// clock, sleep, one mutex, one helper thread, CRC32, a small random source and directory creation. Desktop: POSIX; Vita: SceKernel.
#ifndef PORT_MP_CORE_SYS_H
#define PORT_MP_CORE_SYS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

uint64_t mp_sys_now_ms(void);             // monotonic, arbitrary origin
uint64_t mp_sys_now_us(void);             // the same in microseconds (MP3 perf3: the helper thread's busy time, SMG_MP_PERFLOG)
void mp_sys_sleep_ms(unsigned ms);
uint32_t mp_sys_random(void);             // seeded from the clock and the address of a stack variable: ids and sequence starts
bool mp_sys_random_try(uint32_t* out);    // smallfix: one word from the system's secure random source (Vita RNG, /dev/urandom), up to 3 tries; false = it failed (nothing written). NO weak fallback
bool mp_sys_random_fill(void* out, size_t n);   // smallfix: n secure bytes, all or nothing (false: discard `out`)
bool mp_sys_mkdirs(const char* path);     // creates every missing directory of `path` (the path itself is a directory)

uint32_t mp_crc32(uint32_t crc, const void* data, size_t len);   // standard CRC-32 (zlib); start with crc = 0

// A non-recursive mutex. Desktop: pthread; Vita: a light-weight mutex.
class MpMutex {
public:
    MpMutex();
    ~MpMutex();
    void lock();
    bool tryLock();   // (perf) true when taken without waiting
    void unlock();
private:
    MpMutex(const MpMutex&);
    MpMutex& operator=(const MpMutex&);
    alignas(8) unsigned char m_storage[64];
    bool m_ok;
};

class MpLock {
public:
    explicit MpLock(MpMutex& m) : m_m(m) { m_m.lock(); }
    ~MpLock() { m_m.unlock(); }
private:
    MpMutex& m_m;
};

// MP3 nocap: where the session's helper thread runs (pure: tests/mp/nocap_test.cpp). Defaults: the core asked for (core 2, the aux
// core, in MpCore::ensureThread) at priority 0x70, below every audio thread (0x40 output / AI clock, 0x4F JAudio, 0x50 DSP mixer) and
// above the front end's player and the default-priority threads (the ad hoc receive worker, np_https, the loading ticker) on core 2.
// The same place with and without the Vita's fourth core: the helper never asks for the system core (mask 0x80000), which an
// application has only with the owner's CapUnlocker plug-in and only the port's probe may decide about (vita/runtime/memory.cpp
// port_vita_core3_available); a core asked for outside -1..2 is core 2. SMG_MP_NET_CORE=-1..2 / SMG_MP_NET_PRIO=0x40..0xBF (env.txt)
// override, a value that is not entirely a number in range is ignored. retryMask: what start() asks for when the kernel refuses
// `mask` (cores 1-2, never core 0, the game's); -1: no second try (the game thread then pumps alone, as before).
struct MpHelperPlace {
    int core;        // -1 (any user core) or 0..2
    int prio;        // 0x40..0xBF
    int mask;        // SCE_KERNEL_CPU_MASK_USER_0 << core, or 0 (the process default) for -1
    int retryMask;   // 0x60000 or -1
};
static inline bool mp_helper_env_int(const char* e, long lo, long hi, long* out) {
    if (!e || !*e) return false;
    char* end = 0;
    const long v = strtol(e, &end, 0);
    if (!end || *end != 0 || v < lo || v > hi) return false;
    *out = v;
    return true;
}
static inline MpHelperPlace mp_helper_place(int asked, const char* envCore, const char* envPrio) {
    MpHelperPlace p;
    p.core = asked >= -1 && asked <= 2 ? asked : 2;
    p.prio = 0x70;
    long v = 0;
    if (mp_helper_env_int(envPrio, 0x40, 0xBF, &v)) p.prio = (int)v;
    if (mp_helper_env_int(envCore, -1, 2, &v)) p.core = (int)v;
    p.mask = p.core >= 0 ? (0x10000 << p.core) : 0;
    p.retryMask = p.mask != 0 && p.mask != 0x60000 ? 0x60000 : -1;
    return p;
}

// One helper thread. start() runs fn(arg) on a new thread; join() waits for it to return.
class MpThread {
public:
    MpThread();
    ~MpThread();
    // cpuCore: Vita core for the thread (-1 = any, 0..2; anything else is core 2: see mp_helper_place); ignored on the desktop.
    bool start(void (*fn)(void*), void* arg, const char* name, int cpuCore = -1);
    void join();
    bool running() const { return m_started; }
private:
    MpThread(const MpThread&);
    MpThread& operator=(const MpThread&);
    bool m_started;
    void (*m_fn)(void*);
    void* m_arg;
    uintptr_t m_handle;
#ifdef __vita__
    static int entry(unsigned int argc, void* argv);
#else
    static void* entry(void* self);
#endif
};

// ---- Vita network watch (mp_core_vita.cpp; desktop: no-ops) ------------------------------------------------------------------
// Runs on the network thread. begin/end bracket the thread's life (power callback, net-control callback).
void mp_watch_begin(void);
void mp_watch_end(void);
enum { MP_WATCH_RESUMED = 1, MP_WATCH_WIFI_LOST = 2 };
// One call per network tick, outside the core lock. inSession = a lobby or session exists (the console is kept awake:
// sceKernelPowerTick(DEFAULT) at 1 Hz: the screen neither dims nor goes off and the console does not auto-suspend; "[mp] watch: keep-awake on" /
// "off" once each, desktop too (a stub that only logs); the statistics line every 10 s). Returns MP_WATCH_* bits for what happened since the last call:
// a resume from sleep (power callback resume bits, or the process clock stood still while the real-time clock ran on), or
// the Wi-Fi went (net-control state not "connected" twice in a row at 1 Hz, or the IP address / access point changed).
int mp_watch_tick(bool inSession, uint64_t nowMs);
// Nearby (ad hoc) play: the console is in an ad hoc cell and its infrastructure Wi-Fi is NOT connected (the two cannot run together), so
// the Wi-Fi / IP checks must not run (they would end every Nearby session after two seconds). The cell itself is watched by net/
// (vlink_vita_radio.cpp: two looks a second apart). The power tick and the sleep / resume detection stay on. Set by the network thread.
void mp_watch_set_ad_hoc(bool adHoc);
bool mp_watch_wifi_up(void);          // connected with an IP address (desktop: always true)
void mp_watch_sleep_ms(unsigned ms);  // the network thread's wait (Vita: callbacks run inside it)
// netlog: Nearby (ad hoc): what the radio did with the sends since the last call (the counters restart): sends it took, sends it turned away with
// "would block" / "time-out" (the send is NOT retried: it is dropped, and the reliable layer resends what mattered), and any other error. False
// when there is no radio (desktop, or a build without it): the three stay 0.
bool mp_sys_radio_send_take(uint32_t* ok, uint32_t* blocked, uint32_t* err);
// MP3 nocap: one "[mp] threads: ..." line per thread of the session and the ones it shares cores with (names smg_*, np_*, galaxy_*,
// Sce* audio/net), with priority and core mask, and one "[mp] cores: ..." line (fourth core proven or not). The player build has no
// thread census of its own (the renderer's is a diagnostics build's); called once per session by the helper thread when the
// SMG_MP_PERFLOG line is on. Desktop: nothing.
void mp_sys_thread_census(void);

extern "C" bool mp_wifi_turbo_installed(void);   // read-only: tai config lists wifiturbo_kernel.skprx (no plugin calls)

#endif  // PORT_MP_CORE_SYS_H
