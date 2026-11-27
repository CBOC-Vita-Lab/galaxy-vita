// OS time base, alarms (the decrementer), and miscellaneous OS services.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(__vita__)
#include <pthread.h>
#include <psp2/kernel/processmgr.h>
#endif

#include "port/port.h"
#include "port/qemu_hooks.h"
#include "port/vclock.h"
#include "revolution/os.h"
#include "revolution/os/OSError.h"
#include "revolution/os/OSReset.h"
#include "revolution/os/OSResetSW.h"

#if defined(__vita__)
#include "clock.h"
#endif

// ---------------------------------------------------------------------------
// Time base.  OSTime counts timer ticks (bus clock / 4 = 60.75 MHz) since
// 2000-01-01 00:00:00, like the console after the IPL loads the RTC.
// ---------------------------------------------------------------------------
static constexpr int64_t kTimerClock = 243000000 / 4;
static int64_t sBootNs;
static int64_t sBootTicks;
#if defined(__vita__)
static int64_t sBootUs;
#endif

extern "C" int64_t port_host_time_ns(void) {
    if (port_vclock_enabled()) return port_vclock_now();  // armhf/qemu harness: virtual time (port/vclock.h)
#if defined(__vita__)
    // Newlib's MONOTONIC clock uses this source, then divides it into a timespec.
    return static_cast<int64_t>(sceKernelGetProcessTimeWide() * 1000ull);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
}

extern "C" int port_vclock_guest_wait(void);  // (os_thread.cpp)

extern "C" void port_host_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        return;
    }
    if (port_vclock_enabled()) {
        // (armhf/qemu harness only: a guest thread sleeping while it holds the guest CPU takes the idle step itself, or virtual time
        // never passes and a stage load hangs; from Producdevity's fork, 2a1d164)
        if (!port_vclock_guest_wait()) {
            port_vclock_sleep_ns(ns);
        }
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ll);
    ts.tv_nsec = (long)(ns % 1000000000ll);
    nanosleep(&ts, nullptr);
}

static inline int64_t nsToTicks(int64_t ns) { return (ns / 4000) * 243 + ((ns % 4000) * 243) / 4000; }
static inline int64_t ticksToNs(int64_t ticks) { return (ticks / 243) * 4000 + ((ticks % 243) * 4000) / 243; }

static void initTimeBase() {
    sBootNs = port_host_time_ns();
#if defined(__vita__)
    sBootUs = sBootNs / 1000;
#endif
    // Seconds from 2000-01-01 to now, in local time (the Wii RTC is local).
#if defined(__vita__)
    int64_t localNow;
    if (!port_vita_local_time_seconds(&localNow)) port_fatal("Vita local RTC read failed");
#else
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    int64_t localNow = (int64_t)now + lt.tm_gmtoff;
#endif
    const int64_t kEpoch2000 = 946684800;  // 2000-01-01 in Unix time
    sBootTicks = (localNow - kEpoch2000) * kTimerClock;
}

static inline OSTime unpausedTime() {
#if defined(__vita__)
    if (!port_vclock_enabled()) {
        const int64_t elapsedUs = static_cast<int64_t>(sceKernelGetProcessTimeWide()) - sBootUs;
        // Exactly nsToTicks(elapsedUs * 1000), including signed truncation, without a 64-bit divide.
        return sBootTicks + elapsedUs * 243 / 4;
    }
#endif
    return sBootTicks + nsToTicks(port_host_time_ns() - sBootNs);
}

// Never activated on the default path. Atomic snapshot fields avoid a mutex in
// guest time reads after the first settings visit, including alarm-thread reads.
static std::atomic<bool> sSettingsClockUsed{false}, sSettingsClockFrozen{false};
static std::atomic<unsigned> sSettingsClockVersion{0};
static std::atomic<int64_t> sSettingsClockOffset{0}, sSettingsClockAt{0};
static std::mutex sSettingsClockMutex;
static inline OSTime rawTime() {
    if (!sSettingsClockUsed.load(std::memory_order_acquire)) return unpausedTime();
    for (;;) {
        const unsigned before = sSettingsClockVersion.load();
        if (before & 1) continue;
        const bool frozen = sSettingsClockFrozen.load();
        const auto at = sSettingsClockAt.load();
        const auto offset = sSettingsClockOffset.load();
        const auto now = frozen ? at : unpausedTime();
        if (before == sSettingsClockVersion.load()) return now - offset;
    }
}

extern "C" {

OSTime OSGetTime(void) {
    port_irq_poll();
    return rawTime();
}

OSTick OSGetTick(void) {
    port_irq_poll();
    return (OSTick)rawTime();
}

OSTime __OSGetSystemTime(void) { return rawTime(); }
OSTime __OSTimeToSystemTime(OSTime t) { return t; }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    int64_t totalUs = ticks / 243 * 4 + (ticks % 243) * 4 / 243;  // 60.75 ticks per microsecond
    int64_t secs = totalUs / 1000000;
    int64_t usecs = totalUs % 1000000;
    if (usecs < 0) {
        usecs += 1000000;
        secs -= 1;
    }
    time_t t = (time_t)(secs + 946684800);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    td->sec = tmv.tm_sec;
    td->min = tmv.tm_min;
    td->hour = tmv.tm_hour;
    td->mday = tmv.tm_mday;
    td->mon = tmv.tm_mon;
    td->year = tmv.tm_year + 1900;
    td->wday = tmv.tm_wday;
    td->yday = tmv.tm_yday;
    td->msec = (int)(usecs / 1000);
    td->usec = (int)(usecs % 1000);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Alarms.  The queue is owned by the CPU; a host timer thread raises the
// alarm interrupt when the earliest deadline passes.
// ---------------------------------------------------------------------------
static OSAlarm* sAlarmHead;
static std::mutex sTimerMutex;
static std::condition_variable sTimerCv;
static int64_t sTimerDeadline = INT64_MAX;  // in OSTime ticks, guarded by sTimerMutex

static std::atomic<void*> sTimerSleeper{nullptr};  // virtual clock: the timer thread's sleeper

extern "C" int port_settings_time_pause(int pause) {
    int previous;
    {
        std::lock_guard<std::mutex> lock(sSettingsClockMutex);
        previous = sSettingsClockFrozen.load() ? 1 : 0;
        if (previous == (pause ? 1 : 0)) return previous;
        sSettingsClockVersion.fetch_add(1);
        const auto now = unpausedTime();
        if (pause) sSettingsClockAt.store(now);
        else sSettingsClockOffset.fetch_add(now - sSettingsClockAt.load());
        sSettingsClockFrozen.store(pause != 0);
        sSettingsClockVersion.fetch_add(1);
        sSettingsClockUsed.store(true, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(sTimerMutex);
        sTimerCv.notify_all();
    }
    if (void* sleeper = sTimerSleeper.load()) port_vclock_poke(sleeper);
    return previous;
}

static void rearmTimer() {
    {
        std::lock_guard<std::mutex> lk(sTimerMutex);
        sTimerDeadline = sAlarmHead ? sAlarmHead->fire : INT64_MAX;
        sTimerCv.notify_all();
    }
    if (void* s = sTimerSleeper.load()) port_vclock_poke(s);
}

// The same loop as timerThread() below, sleeping on the virtual clock (a poke replaces the re-arm notification).
static void timerThreadVirtual() {
    sTimerSleeper.store(port_vclock_self());
    for (;;) {
        const uint64_t seq = port_vclock_poke_seq();
        int64_t deadline;
        {
            std::lock_guard<std::mutex> lk(sTimerMutex);
            deadline = sTimerDeadline;
        }
        if (deadline == INT64_MAX || sSettingsClockFrozen.load()) {
            port_vclock_sleep_until(INT64_MAX, seq);
            continue;
        }
        const int64_t now = rawTime();
        if (now >= deadline) {
            {
                std::lock_guard<std::mutex> lk(sTimerMutex);
                sTimerDeadline = INT64_MAX;  // re-armed by the handler
            }
            port_irq_raise(PORT_IRQ_ALARM);
            continue;
        }
        port_vclock_sleep_until(port_vclock_now() + ticksToNs(deadline - now), seq);
    }
}

static void timerThread() {
    PORT_QEMU_THREAD("os-timer");
    port_thread_class(1);
    if (port_vclock_enabled()) timerThreadVirtual();
    std::unique_lock<std::mutex> lk(sTimerMutex);
    for (;;) {
        if (sTimerDeadline == INT64_MAX || sSettingsClockFrozen.load()) {
            sTimerCv.wait(lk);
            continue;
        }
        int64_t now = rawTime();
        if (now >= sTimerDeadline) {
            sTimerDeadline = INT64_MAX;  // re-armed by the handler
            lk.unlock();
            port_irq_raise(PORT_IRQ_ALARM);
            lk.lock();
            continue;
        }
        int64_t waitNs = ticksToNs(sTimerDeadline - now);
        sTimerCv.wait_for(lk, std::chrono::nanoseconds(waitNs));
    }
}

static void insertAlarm(OSAlarm* alarm) {
    OSAlarm* prev = nullptr;
    OSAlarm* next = sAlarmHead;
    while (next && next->fire <= alarm->fire) {
        prev = next;
        next = next->next;
    }
    alarm->prev = prev;
    alarm->next = next;
    if (next) {
        next->prev = alarm;
    }
    if (prev) {
        prev->next = alarm;
    } else {
        sAlarmHead = alarm;
    }
}

static void removeAlarm(OSAlarm* alarm) {
    if (alarm->next) {
        alarm->next->prev = alarm->prev;
    }
    if (alarm->prev) {
        alarm->prev->next = alarm->next;
    } else if (sAlarmHead == alarm) {
        sAlarmHead = alarm->next;
    }
    alarm->prev = alarm->next = nullptr;
}

static void alarmIrq() {
    OSTime now = rawTime();
    while (sAlarmHead && sAlarmHead->fire <= now) {
        OSAlarm* alarm = sAlarmHead;
        removeAlarm(alarm);
        OSAlarmHandler handler = alarm->handler;
        if (alarm->period) {
            alarm->fire += alarm->period;
            if (alarm->fire <= now) {
                // Skip missed periods rather than firing a burst.
                OSTime missed = (now - alarm->fire) / alarm->period + 1;
                alarm->fire += missed * alarm->period;
            }
            insertAlarm(alarm);
        } else {
            alarm->handler = nullptr;
        }
        if (handler) {
            handler(alarm, OSGetCurrentContext());
        }
    }
    rearmTimer();
}

extern "C" {

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = nullptr;
    alarm->tag = 0;
    alarm->prev = alarm->next = nullptr;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = rawTime() + tick;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAbsAlarm(OSAlarm* alarm, OSTime time, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = 0;
    alarm->handler = handler;
    alarm->fire = time;
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
    }
    alarm->period = period;
    alarm->start = start;
    alarm->handler = handler;
    OSTime now = rawTime();
    if (period > 0 && start < now) {
        alarm->fire = start + period * ((now - start) / period + 1);
    } else {
        alarm->fire = start;
    }
    insertAlarm(alarm);
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSCancelAlarm(OSAlarm* alarm) {
    BOOL en = OSDisableInterrupts();
    if (alarm->handler) {
        removeAlarm(alarm);
        alarm->handler = nullptr;
        rearmTimer();
    }
    OSRestoreInterrupts(en);
}

void OSCancelAlarms(u32 tag) {
    BOOL en = OSDisableInterrupts();
    for (OSAlarm* a = sAlarmHead; a;) {
        OSAlarm* n = a->next;
        if (a->tag == tag) {
            removeAlarm(a);
            a->handler = nullptr;
        }
        a = n;
    }
    rearmTimer();
    OSRestoreInterrupts(en);
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { alarm->tag = tag; }
void OSSetAlarmUserData(OSAlarm* alarm, void* data) { alarm->userData = data; }
void* OSGetAlarmUserData(const OSAlarm* alarm) { return alarm->userData; }

}  // extern "C"

// ---------------------------------------------------------------------------
// Reporting / fatal errors
// ---------------------------------------------------------------------------
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "PetariVR"
#endif

// Set by the headless runner so logs also reach the adb shell.
#include "port/diagnostics.h"
#include "port/log_sink.h"
extern "C" int port_log_to_stderr = 0;
// MP3 hwaids: the optional UDP copy of the log (platform/src/mp/mp_hwaids.cpp, SMG_LOG_UDP). Weak: absent in host tests; off (one load) unless the switch is set.
extern "C" void port_log_mirror_line(const char* line, size_t len) __attribute__((weak));
extern "C" int port_log_mirror_enabled(void) __attribute__((weak));

extern "C" void port_card_write_start(void);  // (hitch30: below)
// stdio31 (platform/include/port/console_stdio.h, vita/vgx-game/stdio_safe.cpp): the console streams of the Vita's diagnostic build, written by one
// thread at a time. Weak: absent in the player build (console output discarded) and on other targets, where stderr is flushed as before.
extern "C" void port_console_drain(void) __attribute__((weak));
extern "C" void port_console_attach_writer(void) __attribute__((weak));
// stdio31: a line for stderr in ONE call with its newline (two calls let another thread's text land between them), "prefix" first.
static void consoleLine(const char* prefix, const char* fmt, va_list args) __attribute__((format(printf, 2, 0), unused));
static void consoleLine(const char* prefix, const char* fmt, va_list args) {
    char buf[1100];
    const size_t p = strlen(prefix);
    memcpy(buf, prefix, p);
    va_list again;
    va_copy(again, args);
    const int m = vsnprintf(buf + p, sizeof(buf) - p - 1, fmt, args);
    if (m >= 0 && static_cast<size_t>(m) < sizeof(buf) - p - 1) {
        buf[p + static_cast<size_t>(m)] = '\n';
        fwrite(buf, 1, p + static_cast<size_t>(m) + 1, stderr);
    } else {  // (longer than the buffer: in pieces, as before)
        fputs(prefix, stderr);
        vfprintf(stderr, fmt, again);
        fputc('\n', stderr);
    }
    va_end(again);
}
#if PORT_LOGGING
// The app's own copy of its log (port_log_file): the system log keeps only a
// few minutes of a busy headset, too little to look into a session after it.
static std::mutex sLogFileLock;
static FILE* sLogFile = nullptr;
static long sLogFileBytes = 0;
static const long kLogFileMax = 16 << 20;

#if defined(__vita__)
// On the Vita a line-buffered log file costs 12-15 ms per line: every line is a write to the memory card (the file's size changes,
// so the file system updates its tables as well), done under the log lock. The console logs showed it: the game thread's 5-second
// perf report stalled it 35 ms each time (the "max" of every perf line), and the back end's four lines every 300 pictures made one
// 60-70 ms frame. So a line is only formatted into memory here; a helper thread writes what has gathered every 100 ms in one
// write. A line with FATAL in it, a flush request (port_log_flush: before the process ends on purpose) and the end of the
// process write at once, so a crash loses nothing of what led to it.
// (No heap use in this path: the log is called from everywhere, including while the game's own allocator is busy, so the
// batches live in two fixed buffers and the helper thread sleeps with nanosleep instead of a condition variable.)
static std::mutex sLogPendLock;
// (strikers2audit) two buffers, swapped under the lock (port/log_sink.h): the writer no longer copies up to 96 KiB under the lock the game thread logs under
static char sLogBufA[96 << 10], sLogBufB[96 << 10];
static port::LogSwap sLogSwap(sLogBufA, sLogBufB, sizeof(sLogBufA));
static std::atomic<bool> sLogUrgent{false};
static bool sLogFlusherStarted = false;
static std::mutex sLogWriteLock;  // one writer at a time (the helper thread, or a caller that must have it on the card now)
// hang47: the writer's health, for the hang watch (vita/runtime/memory.cpp, GXV_HANG_WATCH): passes of the writer thread's loop, and since when
// a write to the card is in progress (0: none). Night 35's movie rows ended with a log that simply stopped: a writer stuck in fwrite, a
// writer that never gets a core and a game that stands still all look the same in log.txt.
static std::atomic<unsigned> sLogWriterPasses{0};
static std::atomic<int64_t> sLogWriteSinceNs{0};
static std::atomic<unsigned> sLogWrites{0};
extern "C" void port_log_health(unsigned* passes, unsigned* writes, unsigned* writeMs, unsigned* pendingBytes);

static void logWritePending() {
    // stderr may be the separately buffered diagnostic file. stdio31: on the Vita's diagnostic build this pass also writes what the other
    // threads printed to stdout / stderr (port_console_drain: the only writer of the console FILEs while it runs). It is called BEFORE this
    // function takes any of its locks: no log lock is ever held while the console lock is wanted (console_stdio.h, "Lock order").
    if (PORT_RUNTIME_DIAGNOSTICS) {
        if (port_console_drain) port_console_drain();
        else fflush(stderr);
    }
    std::lock_guard<std::mutex> wl(sLogWriteLock);
    const char* data = nullptr;
    size_t n;
    {
        std::lock_guard<std::mutex> lock(sLogPendLock);  // (a pointer swap: nothing is copied or written under it)
        n = sLogSwap.Take(&data);
        sLogUrgent.store(false);
    }
    if (n == 0) return;
    std::lock_guard<std::mutex> lock(sLogFileLock);
    if (!sLogFile) return;
    if (sLogFileBytes > kLogFileMax) {
        // (strikers2audit) the cap used to end the file with no word: a long tour log looked cut off mid-stage. One line says so.
        static bool capNoted = false;
        if (!capNoted) {
            capNoted = true;
            static const char note[] = "-- log: the 16 MiB limit of this file is reached; later lines are dropped --\n";
            fwrite(note, 1, sizeof(note) - 1, sLogFile);
            fflush(sLogFile);
        }
        return;
    }
    sLogWriteSinceNs.store(port_host_time_ns(), std::memory_order_relaxed);  // (hang47)
    fwrite(data, 1, n, sLogFile);
    fflush(sLogFile);
    sLogWriteSinceNs.store(0, std::memory_order_relaxed);
    sLogWrites.fetch_add(1, std::memory_order_relaxed);
    sLogFileBytes += static_cast<long>(n);
}
// (hang47; no lock taken: the pending length is read as it is, a torn value is only a number in a diagnostic line)
extern "C" void port_log_health(unsigned* passes, unsigned* writes, unsigned* writeMs, unsigned* pendingBytes) {
    const int64_t since = sLogWriteSinceNs.load(std::memory_order_relaxed);
    if (passes) *passes = sLogWriterPasses.load(std::memory_order_relaxed);
    if (writes) *writes = sLogWrites.load(std::memory_order_relaxed);
    if (writeMs) *writeMs = since ? static_cast<unsigned>((port_host_time_ns() - since) / 1000000) : 0;
    if (pendingBytes) *pendingBytes = static_cast<unsigned>(sLogSwap.Length());
}

static void* logFlusherMain(void*) {
    PORT_THREAD_NAME("log-flush");
    if (port_console_attach_writer) port_console_attach_writer();  // (stdio31: from now on console prints are buffered and written here)
    // GXV_CORE0_ALONE=1 (default 0): the writer keeps off the game's core. It starts before env.txt is read, so it asks again at each
    // pass for the first five seconds until the switch has an answer (port.h).
    int placed = 0, tries = 0;
    for (;;) {
        if (!placed && tries < 50) { placed = port_thread_helper(); ++tries; }
        for (int i = 0; i < 10 && !sLogUrgent.load(); ++i) {  // (100 ms in ten steps, so an urgent request is seen within 10 ms)
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        sLogWriterPasses.fetch_add(1, std::memory_order_relaxed);  // (hang47)
        logWritePending();
    }
    return nullptr;
}

extern "C" void port_log_flush(void) { logWritePending(); }

static char sLogFilePath[512];
// beta diagnostics: where this run's log is (the problem report reads it and <path>.prev); null before port_log_file.
extern "C" const char* port_log_current_path(void) { return sLogFilePath[0] ? sLogFilePath : nullptr; }

extern "C" void port_log_file(const char* path) {
    snprintf(sLogFilePath, sizeof(sLogFilePath), "%s", path);
    // The previous session's log is kept beside it as <path>.prev.
    char prev[512];
    snprintf(prev, sizeof(prev), "%s.prev", path);
    rename(path, prev);
    {
        std::lock_guard<std::mutex> lock(sLogFileLock);
        sLogFile = fopen(path, "w");
        if (sLogFile) setvbuf(sLogFile, nullptr, _IOFBF, 16384);  // (written in batches by logWritePending, flushed after each)
    }
    if (sLogFile && !sLogFlusherStarted) {
        sLogFlusherStarted = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 10);
        pthread_t th;
        if (pthread_create(&th, &attr, &logFlusherMain, nullptr) == 0) pthread_detach(th);
        pthread_attr_destroy(&attr);
    }
    port_card_write_start();  // (hitch30: the card-write thread starts here, at boot, so that no caller ever makes it)
}

// hitch33 8 (14 Nov): GXV_LOG_LEVEL (default 2 = every line, as before; 1 = the player's log). The console logs of c106-c120 write about 10 lines and 3 KiB a second in play
// (look1: 1914 lines in 190 s; 2650: 2984 in 279 s), nearly all of them periodic census lines of the test switches, in a card write every 100 ms; the 16 MiB cap of the file
// is reached after about 90 minutes of play. Level 1 does not write the periodic kinds listed here (matched on the line's first word); everything else stays: the start-up and
// settings lines, loading / stage lines, "hitch:" and "hitch-stage:" / "hitch33*:" lines, "first-use:", "stutter:" (a frame of 50 ms or more), errors, FATAL.
// The level is read once, at the first line written 4 s or more after the first one (env.txt has been read by then; the periodic lines start later than that).
static bool logPeriodicDropped(const char* text, size_t n) {
    static const char* const kPeriodic[] = {
        "cores-5s", "spin:", "turn-prof:", "cpulod:", "recover:", "pace:", "speed:", "present:", "audio cpu:", "audio:", "par-ptcl:", "par-anim:", "par-view", "par-plant:",
        "ptcl-thin:", "threads:", "undrawn-lean:", "gpu-scenes:", "scene-probe:", "be-split-5s:", "fps:", "floor:", "oc-lightfind:", "oc-clip:", "farlogic:", "cliprange:", "thinfade:",
        "sensor-bp:", "j3d lists:", "shadow-stencil:", "latency:", "direct-display:", "even:", "smooth:", "stream:", "ta23:", "efbswap2:", "pxcut:", "shadowmemo:",
        "colshadow-clip:", "light-dedup:", "logiccut:", "silhgate:", "silhocc:", "hrw:", "stutter-5s", "tex-census-5s:", "tex-upload-5s:", "texup:", "drain:", "memory:",
        "vgx host memory", "grav-memo:", "bloom-skip:", "bloom-fast:", "indcap:", "indcap-run:", "card:", "async-shaders-5s:", "fepar:", "keys27:", "pack:", "detail25:",
        "popin24:", "popin25:", "perf-budget:", "io:", "io-more:", "drawdone-defer:", "asynclink-5s:", "gamegov:", "fegov:", "dl-keep:", "rehearse-5s:", "fe-lane:"};
    static int level = -1;  // -1: not read yet
    static struct timespec first = {0, 0};
    static unsigned dropped = 0;
    if (level < 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (first.tv_sec == 0 && first.tv_nsec == 0) first = now;
        if (now.tv_sec - first.tv_sec < 4) return false;
        const char* e = getenv("GXV_LOG_LEVEL");
        level = (e && e[0] == '1') ? 1 : 2;
    }
    if (level >= 2) return false;
    {  // (review 14 Nov: a line saying a feature turned itself off is never dropped)
        static const char* const kKeep[] = {"DISABLED", "switched off"};
        for (size_t w = 0; w < 2; ++w) {
            const size_t k = strlen(kKeep[w]);
            for (size_t j = 0; j + k <= n; ++j)
                if (!memcmp(text + j, kKeep[w], k)) return false;
        }
    }
    for (size_t i = 0; i < sizeof(kPeriodic) / sizeof(kPeriodic[0]); ++i) {
        const size_t k = strlen(kPeriodic[i]);
        if (n >= k && !memcmp(text, kPeriodic[i], k)) {
            ++dropped;
            return true;
        }
    }
    return false;
}

// stackgate31: the line buffer is the caller's: logToFile gives it a 1100-byte frame of its own, port_log_summary_cb (the display-queue callback thread, whose system stack is 4 KiB)
// gives it a static one. Same text, same lock, same file.
static void logToFileBuf(char* line, size_t lineCap, bool onDisplayThread, const char* fmt, va_list args) __attribute__((format(printf, 4, 0), noinline));
static void logToFile(const char* fmt, va_list args) __attribute__((format(printf, 1, 0)));   // (fmt1131)
static void logToFile(const char* fmt, va_list args) {
    if (!sLogFile) return;
    char line[1100];
    logToFileBuf(line, sizeof line, false, fmt, args);
}
// fable31fix S1: the display thread's lines that were dropped because the buffer was full (port/log_sink.h DisplayDropRule); reported by the next line of another thread.
static port::DisplayDropRule sDisplayDrops;
static void logReportDisplayDrops() __attribute__((noinline));
static void logReportDisplayDrops() {
    const unsigned k = sDisplayDrops.Take();
    if (!k) return;
    char note[96];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    const int len = snprintf(note, sizeof note, "%02d:%02d:%02d.%03d log: %u display-thread lines dropped (buffer full)\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                             (int)(ts.tv_nsec / 1000000), k);
    bool ok = false;
    if (len > 0 && len < static_cast<int>(sizeof note)) {
        std::lock_guard<std::mutex> lock(sLogPendLock);
        ok = sLogSwap.Append(note, static_cast<size_t>(len));
    }
    if (!ok) sDisplayDrops.Restore(k);
}
static void logToFileBuf(char* line, size_t lineCap, bool onDisplayThread, const char* fmt, va_list args) {
    if (!onDisplayThread && sDisplayDrops.Pending()) logReportDisplayDrops();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, 40, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    if (n < 0 || n > 39) n = 0;
    va_list again;   // (fmt1131: kept for the rare line that does not fit the buffer)
    va_copy(again, args);
    int m = vsnprintf(line + n, lineCap - n - 2, fmt, args);
    if (m < 0) m = 0;
    // fix31: what the file gets, appended under ONE hold of the log lock (one line, or the block of a long line's pieces, which so stay together)
    auto put = [&](const char* data, size_t total, bool urgent) {
        bool full = false;
        {
            std::lock_guard<std::mutex> lock(sLogPendLock);
            if (!sLogSwap.Append(data, total)) full = true;
            if (urgent) sLogUrgent.store(true);
        }
        bool stored = !full;
        if (full && !sDisplayDrops.OnFull(onDisplayThread)) return false;   // (fable31fix: the display thread never writes the card: the line is dropped and counted)
        if (full) {  // (a burst of more than the buffer holds in 100 ms: write it now rather than lose it)
            logWritePending();
            std::lock_guard<std::mutex> lock(sLogPendLock);
            stored = sLogSwap.Append(data, total);
        }
        if (urgent && port::DisplayDropRule::MayWriteNow(onDisplayThread)) logWritePending();   // (display thread: the urgent flag above wakes the writer thread instead)
        return stored;
    };
    // One line of text, as the file gets it (the timestamp is already in line[0..n)). `len` bytes of text sit at line + n.
    auto emit = [&](int len) {
        line[n + len] = '\n';
        const size_t total = static_cast<size_t>(n + len + 1);
        if (port_log_mirror_line) port_log_mirror_line(line, total);   // MP3 hwaids: a copy for the UDP mirror, if it is on
        const bool urgent = std::string_view(line, total).find("FATAL") != std::string_view::npos;
        if (!urgent && logPeriodicDropped(line + n, static_cast<size_t>(len))) return;  // (hitch33 8, GXV_LOG_LEVEL=1)
        put(line, total, urgent);
    };
    const int cap = static_cast<int>(lineCap) - n - 3;
    if (m <= cap) {
        va_end(again);
        emit(m);
        return;
    }
    // fmt1131: the text is longer than the buffer (a list of settings or defaults used to be cut here without a word, so the log hid its tail).
    // It is formatted again in full and written as consecutive lines, each at most about 900 bytes, cut at a space where there is one; every
    // line after the first starts with "+ " so a reader knows it continues the line above.
    // fix31: the line is one unit (port/log_sink.h LongLineBlock): the periodic-line filter is asked once for the whole text (it matched each piece by its
    // prefix: the first piece of a long periodic line was dropped and its "+ " pieces kept), and the pieces go in as one block under one lock, so another
    // thread's line cannot land between a line and its continuation.
    std::string whole(static_cast<size_t>(m) + 1, '\0');
    vsnprintf(&whole[0], whole.size(), fmt, again);
    va_end(again);
    whole.resize(static_cast<size_t>(m));
    const size_t kChunk = 900;
    bool urgent = false;
    const std::string block = port::LongLineBlock(line, static_cast<size_t>(n), whole.data(), whole.size(), kChunk, logPeriodicDropped, &urgent);
    if (port_log_mirror_line) {   // (the mirror gets every line, as it does a short one: before the filter)
        port::SplitLong(whole.data(), whole.size(), kChunk, [&](const char* p, size_t len, bool first) {
            std::string one(line, static_cast<size_t>(n));
            if (!first) one += "+ ";
            one.append(p, len);
            one += '\n';
            port_log_mirror_line(one.data(), one.size());
        });
    }
    if (block.empty()) return;   // (dropped as a whole by GXV_LOG_LEVEL=1)
    if (!put(block.data(), block.size(), urgent)) {
        // (a block larger than the whole buffer, 96 KiB: written line by line, as before fix31, rather than lost)
        for (size_t pos = 0; pos < block.size();) {
            const size_t e = block.find('\n', pos);
            put(block.data() + pos, e - pos + 1, urgent);
            pos = e + 1;
        }
    }
}
#else
extern "C" void port_log_flush(void) {}

// MP3 (harness): PETARI_LOG_FRAMES=1 (desktop only, default off) puts the game's own frame counter into every game.log line, so the harness
// can judge rules in GAME time instead of wall seconds: "HH:MM:SS.mmm @f<frame> text". The frame is port_game_frames() (the clock
// PETARI_GAME_CLOCK scripts run on). Weak, so a target without the renderer links; null = no stamp. Off = the line is byte-identical to before.
extern "C" unsigned long long port_game_frames(void) __attribute__((weak));
static bool sLogFrames = false;

static char sLogFilePath[512];
// beta diagnostics (desktop): where this run's log is (the problem report reads it and <path>.prev); null before port_log_file.
extern "C" const char* port_log_current_path(void) { return sLogFilePath[0] ? sLogFilePath : nullptr; }

extern "C" void port_log_file(const char* path) {
    snprintf(sLogFilePath, sizeof(sLogFilePath), "%s", path);
    // The previous session's log is kept beside it as <path>.prev.
    char prev[512];
    snprintf(prev, sizeof(prev), "%s.prev", path);
    rename(path, prev);
    std::lock_guard<std::mutex> lock(sLogFileLock);
    const char* lf = getenv("PETARI_LOG_FRAMES");
    sLogFrames = lf && atoi(lf) != 0;
    sLogFile = fopen(path, "w");
    if (sLogFile) {
        setvbuf(sLogFile, nullptr, _IOLBF, 1024);  // each line reaches the file at once, crash or not
    }
}

static void logToFile(const char* fmt, va_list args) __attribute__((format(printf, 1, 0)));   // (fmt1131)
static void logToFile(const char* fmt, va_list args) {
    std::lock_guard<std::mutex> lock(sLogFileLock);
    if (!sLogFile || sLogFileBytes > kLogFileMax) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = fprintf(sLogFile, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    if (port_log_mirror_line && port_log_mirror_enabled && port_log_mirror_enabled()) {   // MP3 hwaids: the same line for the UDP mirror (only when it is on)
        char copy[1100];
        va_list again;
        va_copy(again, args);
        int k = snprintf(copy, 40, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
        if (k < 0 || k > 39) k = 0;
        const int m = vsnprintf(copy + k, sizeof(copy) - static_cast<size_t>(k), fmt, again);
        va_end(again);
        port_log_mirror_line(copy, static_cast<size_t>(k + (m < 0 ? 0 : m < static_cast<int>(sizeof(copy)) - k ? m : static_cast<int>(sizeof(copy)) - k - 1)));
    }
    if (sLogFrames && port_game_frames) n += fprintf(sLogFile, "@f%llu ", port_game_frames());   // (PETARI_LOG_FRAMES)
    n += vfprintf(sLogFile, fmt, args);
    fputc('\n', sLogFile);
    sLogFileBytes += n > 0 ? n + 1 : 1;
}
#endif

extern "C" void port_vlog(const char* fmt, va_list args) __attribute__((format(printf, 1, 0)));   // (fmt1131)
extern "C" void port_vlog(const char* fmt, va_list args) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    {
        va_list copy;
        va_copy(copy, args);
        logToFile(fmt, copy);
        va_end(copy);
    }
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        va_list copy;
        va_copy(copy, args);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, args);
#else
    consoleLine("", fmt, args);
#endif
}

extern "C" void port_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    port_vlog(fmt, args);
    va_end(args);
}

extern "C" void port_log_summary(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logToFile(fmt, args);
    va_end(args);
}

// stackgate31: port_log_summary for the display queue's callback thread (PaceDisplay / SyncDisplay and what they call, vgx_host_galaxy_vita.cpp): the same line, but formatted in a static
// buffer instead of logToFile's 1100 bytes of that thread's 4 KiB stack (tools/check_display_stack.py). ONE caller thread only: the display callback (never two at a time).
extern "C" void port_log_summary_cb(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void port_log_summary_cb(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__vita__)
    if (sLogFile) {
        static char line[1100];   // (the display callback thread only)
        logToFileBuf(line, sizeof line, true, fmt, args);
    }
#else
    logToFile(fmt, args);
#endif
    va_end(args);
}
#else
extern "C" void port_log_file(const char*) {}
extern "C" void port_log_flush(void) {}
extern "C" void port_vlog(const char*, va_list) {}
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log_summary_cb(const char*, ...) {}
#endif

extern "C" void port_fatal(const char* fmt, ...) {
#if PORT_LOGGING
    va_list args;
    va_start(args, fmt);
    {
        va_list copy;
        va_copy(copy, args);
        logToFile(fmt, copy);
        va_end(copy);
    }
#ifdef __ANDROID__
    if (port_log_to_stderr) {
        va_list copy;
        va_copy(copy, args);
        fputs("FATAL: ", stderr);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
        va_end(copy);
    }
    __android_log_vprint(ANDROID_LOG_FATAL, LOG_TAG, fmt, args);
#else
    consoleLine("", fmt, args);
#endif
    va_end(args);
    port_log_flush();
#endif
    abort();
}

extern "C" {

void OSVReport(const char* fmt, va_list args) {
    if (!PORT_LOGGING) return;
    // Game messages are Shift-JIS; log them verbatim.
    char buf[1024];
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = 0;
    }
    if (n) {
        port_log("[OS] %s", buf);
    }
}

void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    OSVReport(fmt, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    if (!PORT_LOGGING) abort();
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    port_vsnprintf(buf, sizeof(buf), fmt, args);  // game format strings (MSL semantics)
    va_end(args);
    port_fatal("OSPanic %s:%d: %s", file, line, buf);
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    port_fatal("OSFatal: %s", msg);
}

// Error handlers (exceptions never reach the game on the host).
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return nullptr;
}

void OSRegisterVersion(const char* version) { port_log("OSRegisterVersion: %s", version); }

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) { (void)info; }

OSPowerCallback OSSetPowerCallback(OSPowerCallback cb) {
    (void)cb;
    return nullptr;
}

BOOL OSGetResetButtonState(void) { return FALSE; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
void OSRestart(u32 code) { port_fatal("OSRestart(%u)", code); }
void OSRebootSystem(void) { port_fatal("OSRebootSystem"); }
void OSReturnToMenu(void) { port_fatal("OSReturnToMenu"); }
void OSShutdownSystem(void) { port_fatal("OSShutdownSystem"); }
void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)forceMenu;
    port_fatal("OSResetSystem(%d, %u)", reset, resetCode);
}

void OSInit(void) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
extern "C" void port_os_time_init(void) {
    initTimeBase();
    port_irq_set_handler(PORT_IRQ_ALARM, alarmIrq);
    std::thread(timerThread).detach();
}

// ---------------------------------------------------------------------------
// hitch30 (13 Nov 2026): the card-write queue (port/card_queue.h has the pool, the file work and the census text)
//   extern "C" int  port_card_write_async(path, mode 0 replace | 1 append, data, bytes): copies the bytes into a fixed pool and returns 1; a
//        full pool, an oversize payload (> 64 KiB) or a path that is too long returns 0 (counted): the caller goes on, it never waits and
//        never touches the card. The writer ("card-write", class 4 = the lowest priority, the helper cores) does the file work in the
//        order the writes were made.
//   extern "C" void port_card_write_flush(waitMs): 0 = wake the writer now (a stage change, behind the cover); > 0 = the CALLING thread also
//        does what is queued and returns when the queue is empty or after waitMs (the exit paths; a writer starved by a busy core cannot
//        lose a write there). Prints the census line.
// Census (port_log_summary, at most every 5 s while writes happen, and at each flush):
//   card: N async writes, M bytes, longest write T ms, dropped K, on the caller's thread 0 (window longest ..; failed ..; coalesced ..; oversize ..; waiting ..)
// ---------------------------------------------------------------------------
#include "port/card_queue.h"

static port::card::Queue sCardQ;
static std::mutex sCardWriteLock;  // one writer at a time (the card thread, or an exit path)
static std::atomic<bool> sCardKick{false}, sCardStarted{false}, sCardThreadOk{false};
static std::atomic<uint32_t> sCardLastMark{0};

static int64_t cardNowMs() { return port_host_time_ns() / 1000000; }
static void cardCensus(bool force) {
    const uint32_t m = sCardQ.Mark();
    if (!force && m == sCardLastMark.load(std::memory_order_relaxed)) return;
    sCardLastMark.store(m, std::memory_order_relaxed);
    char line[360];
    sCardQ.Line(line, sizeof line);
    port_log_summary("%s", line);
}
static void cardRunAll() {
    std::lock_guard<std::mutex> lk(sCardWriteLock);
    sCardQ.RunAll();
}
static void* cardWriterMain(void*) {
    PORT_THREAD_NAME("card-write");
    int placed = 0, tries = 0;
    int64_t lastPrint = cardNowMs();
    for (;;) {
        if (!placed && tries < 50) { placed = port_thread_helper(); ++tries; if (tries == 1) port_thread_class(4); }
        for (int i = 0; i < 4 && !sCardKick.load(std::memory_order_relaxed) && sCardQ.Pending() == 0; ++i) {  // (up to 20 ms in steps of 5, so a kick is seen within 5 ms)
            struct timespec ts = {0, 5 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        sCardKick.store(false, std::memory_order_relaxed);
        if (sCardQ.Pending()) cardRunAll();
        const int64_t now = cardNowMs();
        if (now - lastPrint >= 5000) {
            lastPrint = now;
            cardCensus(false);
        }
    }
    return nullptr;
}
static void cardStart() {
    if (sCardStarted.exchange(true)) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 96u << 10);
    pthread_t th;
    if (pthread_create(&th, &attr, &cardWriterMain, nullptr) == 0) {
        pthread_detach(th);
        sCardThreadOk.store(true);
    }
    pthread_attr_destroy(&attr);
}
extern "C" void port_card_write_start(void) { cardStart(); }
extern "C" int port_card_write_async(const char* path, int mode, const void* data, unsigned bytes) {
    if (!sCardStarted.load(std::memory_order_relaxed)) cardStart();  // (normally started at boot by port_log_file: this is the first call of a build without a log)
    if (!sCardQ.Enqueue(path, mode, data, bytes)) return 0;
    if (!sCardThreadOk.load(std::memory_order_relaxed)) {  // (no writer thread could be made: the caller does it, counted)
        sCardQ.st.callerThread.fetch_add(1, std::memory_order_relaxed);
        cardRunAll();
    }
    return 1;
}
// settingsnative: writes still queued (0 = the queue is empty); the native Settings restart waits for it
extern "C" unsigned port_card_write_pending(void) { return sCardQ.Pending(); }
// setfix31b: finished queued writes (done + failed: it moves while the queue is worked off) and the failed ones, for the native Settings' restart wait
extern "C" unsigned port_card_write_finished(void) { return sCardQ.st.done.load(std::memory_order_relaxed) + sCardQ.st.failed.load(std::memory_order_relaxed); }
extern "C" unsigned port_card_write_failures(void) { return sCardQ.st.failed.load(std::memory_order_relaxed); }
extern "C" void port_card_write_flush(unsigned waitMs) {
    sCardKick.store(true, std::memory_order_relaxed);
    if (waitMs == 0) return;
    const int64_t t0 = cardNowMs();
    while (sCardQ.Pending() && cardNowMs() - t0 < static_cast<int64_t>(waitMs)) {
        cardRunAll();
        if (sCardQ.Pending()) {
            struct timespec ts = {0, 2 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
    }
    cardCensus(false);  // (prints when anything changed since the last line: the exit paths call this twice)
}
