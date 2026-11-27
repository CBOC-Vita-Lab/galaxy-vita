// Galaxy 3.0 multiplayer (port side, owner: core): platform services for the session layer. See mp_core_sys.h.
#include "mp_core_sys.h"
#include "mp_posix_io.h"   // smallfix2
#include "port/mp_prof.h"
#include "port/mp_bridge.h"   // (mp_log)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

uint64_t mp_sys_now_ms(void) {
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide() / 1000u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

uint64_t mp_sys_now_us(void) {
#ifdef __vita__
    return (uint64_t)sceKernelGetProcessTimeWide();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

void mp_sys_sleep_ms(unsigned ms) {
#ifdef __vita__
    sceKernelDelayThread(ms * 1000u);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, 0);
#endif
}

uint32_t mp_sys_random(void) {
    static uint32_t s_state = 0;
    static MpMutex s_mutex;   // function-local: no static-constructor order issue
    MpLock l(s_mutex);
    if (s_state == 0) {
        int local = 0;
        uint64_t t = mp_sys_now_ms();
#ifdef __vita__
        t ^= (uint64_t)sceKernelGetProcessTimeWide() << 7;
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        t ^= (uint64_t)ts.tv_nsec * 2654435761u ^ (uint64_t)getpid() << 16;
#endif
        s_state = (uint32_t)(t ^ (t >> 32) ^ (uint32_t)(uintptr_t)&local) | 1u;
    }
    // xorshift32
    uint32_t x = s_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x ? x : 0x9E3779B9u;
    return s_state;
}

// smallfix: the secure source, tried up to 3 times per word; there is NO fallback to the weak generator (a clock-seeded xorshift is not key
// material) and no sticky state: every call reports its own result. A caller that needs key material refuses to go on when this says false.
bool mp_sys_random_try(uint32_t* out) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t v = 0;
#ifdef __vita__
        const bool ok = sceKernelGetRandomNumber(&v, sizeof v) >= 0;
#else
        const bool ok = mp_io_read("/dev/urandom", &v, sizeof v) == (long)sizeof v;   // smallfix2: POSIX call
#endif
        if (ok) { *out = v; return true; }
    }
    return false;
}

// n bytes (any n), all or nothing: false leaves `out` with partial secure bytes (the caller discards them).
bool mp_sys_random_fill(void* out, size_t n) {
    uint8_t* d = (uint8_t*)out;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v;
        if (!mp_sys_random_try(&v)) return false;
        const size_t k = n - i < 4 ? n - i : 4;
        memcpy(d + i, &v, k);
    }
    return true;
}

bool mp_sys_mkdirs(const char* path) {
    if (!path || !*path) return false;
    char buf[512];
    const size_t n = strlen(path);
    if (n >= sizeof buf) return false;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        const char saved = buf[i];
        buf[i] = 0;
        // skip "ux0:" (a device, not a directory)
        if (!(i > 0 && buf[i - 1] == ':')) {
#ifdef __vita__
            sceIoMkdir(buf, 0777);
#else
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) { /* checked below */ }
#endif
        }
        buf[i] = saved;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t mp_crc32(uint32_t crc, const void* data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;   // (idempotent: a race only recomputes the same table)
    }
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- mutex -------------------------------------------------------------------------------------------------------------------
#ifdef __vita__
static_assert(sizeof(SceKernelLwMutexWork) <= 64, "lw mutex work area");
MpMutex::MpMutex() : m_ok(false) {
    memset(m_storage, 0, sizeof m_storage);
    m_ok = sceKernelCreateLwMutex((SceKernelLwMutexWork*)m_storage, "smg_mp", 0, 0, 0) >= 0;
}
MpMutex::~MpMutex() { if (m_ok) sceKernelDeleteLwMutex((SceKernelLwMutexWork*)m_storage); }
static inline void LockRaw(void* m) { sceKernelLockLwMutex((SceKernelLwMutexWork*)m, 1, 0); }
bool MpMutex::tryLock() { return !m_ok || sceKernelTryLockLwMutex((SceKernelLwMutexWork*)m_storage, 1) >= 0; }
void MpMutex::unlock() { if (m_ok) sceKernelUnlockLwMutex((SceKernelLwMutexWork*)m_storage, 1); }
#else
static_assert(sizeof(pthread_mutex_t) <= 64, "pthread mutex size");
MpMutex::MpMutex() : m_ok(false) { m_ok = pthread_mutex_init((pthread_mutex_t*)m_storage, 0) == 0; }
MpMutex::~MpMutex() { if (m_ok) pthread_mutex_destroy((pthread_mutex_t*)m_storage); }
static inline void LockRaw(void* m) { pthread_mutex_lock((pthread_mutex_t*)m); }
bool MpMutex::tryLock() { return !m_ok || pthread_mutex_trylock((pthread_mutex_t*)m_storage) == 0; }
void MpMutex::unlock() { if (m_ok) pthread_mutex_unlock((pthread_mutex_t*)m_storage); }
#endif
void MpMutex::lock() {
    if (!m_ok) return;
#if defined(SMG_MP_PROF) && SMG_MP_PROF
    // (perf counter: how long the game thread waits for the session lock, and how often it takes it)
    if (mp_prof_is_game_thread()) {
        const uint64_t t0 = mp_prof_now_us();
        LockRaw(m_storage);
        mp_prof_add(MP_PROF_LOCKWAIT, mp_prof_now_us() - t0);
        mp_prof_count(0);
        return;
    }
#endif
    LockRaw(m_storage);
}

// ---- thread ------------------------------------------------------------------------------------------------------------------
MpThread::MpThread() : m_started(false), m_fn(0), m_arg(0), m_handle(0) {}
MpThread::~MpThread() { join(); }

#ifdef __vita__
int MpThread::entry(unsigned int, void* argv) {
    MpThread* self = *(MpThread**)argv;
    self->m_fn(self->m_arg);
    return 0;
}
bool MpThread::start(void (*fn)(void*), void* arg, const char* name, int cpuCore) {
    if (m_started) return false;
    m_fn = fn;
    m_arg = arg;
    // below the game and audio threads; off core 0 (the game's) when a core is given. 64 KiB of stack (as the ad hoc receive thread):
    // the pump nests receive -> reliable send -> VLink send -> backend, each frame with a 1.1-1.6 KiB datagram buffer (-fstack-usage), and
    // the game-side handlers it calls run on it as well; an overflow on the console would be a silent crash.
    // MP3 perf3: SMG_MP_NET_PRIO=0x40..0xBF / SMG_MP_NET_CORE=0..2 (-1 any) in env.txt override the defaults (0x70, the core asked
    // for) for a console A/B against the front end on core 2; anything else keeps them.
    // MP3 nocap: the place is mp_helper_place (mp_core_sys.h): never the fourth core's mask 0x80000 (a core asked for outside -1..2,
    // or a value of SMG_MP_NET_CORE that is not wholly a number, was 0x80000 or core 0 before), and a refused mask is tried once more
    // on cores 1-2 before the game thread is left to pump alone.
    const MpHelperPlace place = mp_helper_place(cpuCore, getenv("SMG_MP_NET_CORE"), getenv("SMG_MP_NET_PRIO"));
    const int prio = place.prio;
    cpuCore = place.core;
    int mask = place.mask;
    SceUID t = sceKernelCreateThread(name ? name : "smg_mp_net", (SceKernelThreadEntry)entry, prio, 64 * 1024, 0, mask, 0);
    int firstRc = 0;
    if (t < 0 && place.retryMask >= 0) {
        firstRc = t;
        mask = place.retryMask;
        t = sceKernelCreateThread(name ? name : "smg_mp_net", (SceKernelThreadEntry)entry, prio, 64 * 1024, 0, mask, 0);
        mp_log("helper thread %s: core mask 0x%X refused (0x%08X); cores 1-2: %s", name ? name : "smg_mp_net", (unsigned)place.mask,
               (unsigned)firstRc, t < 0 ? "refused too" : "ok");
    }
    if (t < 0) return false;
    MpThread* self = this;
    if (sceKernelStartThread(t, sizeof self, &self) < 0) {
        sceKernelDeleteThread(t);
        return false;
    }
    m_handle = (uintptr_t)t;
    m_started = true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        // (the mask as the kernel has it now: what the hardware checklist compares, docs/MULTIPLAYER-HARDWARE-TEST.md)
        mp_log("helper thread %s: core %d priority 0x%X (mask 0x%X asked, 0x%X now)", name ? name : "smg_mp_net", cpuCore, (unsigned)prio,
               (unsigned)mask, (unsigned)sceKernelGetThreadCpuAffinityMask(t));
    }
    return true;
}
void MpThread::join() {
    if (!m_started) return;
    sceKernelWaitThreadEnd((SceUID)m_handle, 0, 0);
    sceKernelDeleteThread((SceUID)m_handle);
    m_started = false;
}
#else
void* MpThread::entry(void* p) {
    MpThread* self = (MpThread*)p;
    self->m_fn(self->m_arg);
    return 0;
}
bool MpThread::start(void (*fn)(void*), void* arg, const char*, int) {
    if (m_started) return false;
    m_fn = fn;
    m_arg = arg;
    pthread_t t;
    if (pthread_create(&t, 0, entry, this) != 0) return false;
    m_handle = (uintptr_t)t;
    m_started = true;
    return true;
}
void MpThread::join() {
    if (!m_started) return;
    pthread_join((pthread_t)m_handle, 0);
    m_started = false;
}
#endif
