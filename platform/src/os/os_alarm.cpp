// OS time base, alarms (the decrementer), and miscellaneous OS services.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
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

extern "C" void port_host_sleep_ns(int64_t ns) {
    if (ns <= 0) {
        return;
    }
    if (port_vclock_enabled()) {
        port_vclock_sleep_ns(ns);
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

static inline OSTime rawTime() {
#if defined(__vita__)
    if (!port_vclock_enabled()) {
        const int64_t elapsedUs = static_cast<int64_t>(sceKernelGetProcessTimeWide()) - sBootUs;
        // Exactly nsToTicks(elapsedUs * 1000), including signed truncation, without a 64-bit divide.
        return sBootTicks + elapsedUs * 243 / 4;
    }
#endif
    return sBootTicks + nsToTicks(port_host_time_ns() - sBootNs);
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
        if (deadline == INT64_MAX) {
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
        if (sTimerDeadline == INT64_MAX) {
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
extern "C" int port_log_to_stderr = 0;

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
static char sLogPendBuf[96 << 10];
static size_t sLogPendLen = 0;
static std::atomic<bool> sLogUrgent{false};
static bool sLogFlusherStarted = false;
static std::mutex sLogWriteLock;  // one writer at a time (the helper thread, or a caller that must have it on the card now)
static char sLogWriteBuf[96 << 10];

static void logWritePending() {
    if (PORT_RUNTIME_DIAGNOSTICS) fflush(stderr);  // stderr may be the separately buffered diagnostic file.
    std::lock_guard<std::mutex> wl(sLogWriteLock);
    size_t n;
    {
        std::lock_guard<std::mutex> lock(sLogPendLock);
        n = sLogPendLen;
        memcpy(sLogWriteBuf, sLogPendBuf, n);
        sLogPendLen = 0;
        sLogUrgent.store(false);
    }
    if (n == 0) return;
    std::lock_guard<std::mutex> lock(sLogFileLock);
    if (!sLogFile || sLogFileBytes > kLogFileMax) return;
    fwrite(sLogWriteBuf, 1, n, sLogFile);
    fflush(sLogFile);
    sLogFileBytes += static_cast<long>(n);
}

static void* logFlusherMain(void*) {
    for (;;) {
        for (int i = 0; i < 10 && !sLogUrgent.load(); ++i) {  // (100 ms in ten steps, so an urgent request is seen within 10 ms)
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        logWritePending();
    }
    return nullptr;
}

extern "C" void port_log_flush(void) { logWritePending(); }

extern "C" void port_log_file(const char* path) {
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
}

static void logToFile(const char* fmt, va_list args) {
    if (!sLogFile) return;
    char line[1100];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, 40, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    if (n < 0 || n > 39) n = 0;
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    if (m < 0) m = 0;
    if (m > static_cast<int>(sizeof(line)) - n - 3) m = static_cast<int>(sizeof(line)) - n - 3;  // (cut, as a line of the old code was never longer in practice)
    line[n + m] = '\n';
    const size_t len = static_cast<size_t>(n + m + 1);
    const bool urgent = std::string_view(line, len).find("FATAL") != std::string_view::npos;
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(sLogPendLock);
        if (sLogPendLen + len <= sizeof(sLogPendBuf)) {
            memcpy(sLogPendBuf + sLogPendLen, line, len);
            sLogPendLen += len;
        } else {
            full = true;
        }
        if (urgent) sLogUrgent.store(true);
    }
    if (full) {  // (a burst of more than the buffer holds in 100 ms: write it now rather than lose it)
        logWritePending();
        std::lock_guard<std::mutex> lock(sLogPendLock);
        if (sLogPendLen + len <= sizeof(sLogPendBuf)) { memcpy(sLogPendBuf + sLogPendLen, line, len); sLogPendLen += len; }
    }
    if (urgent) logWritePending();
}
#else
extern "C" void port_log_flush(void) {}

extern "C" void port_log_file(const char* path) {
    // The previous session's log is kept beside it as <path>.prev.
    char prev[512];
    snprintf(prev, sizeof(prev), "%s.prev", path);
    rename(path, prev);
    std::lock_guard<std::mutex> lock(sLogFileLock);
    sLogFile = fopen(path, "w");
    if (sLogFile) {
        setvbuf(sLogFile, nullptr, _IOLBF, 1024);  // each line reaches the file at once, crash or not
    }
}

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
    n += vfprintf(sLogFile, fmt, args);
    fputc('\n', sLogFile);
    sLogFileBytes += n > 0 ? n + 1 : 1;
}
#endif

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
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
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
#else
extern "C" void port_log_file(const char*) {}
extern "C" void port_log_flush(void) {}
extern "C" void port_vlog(const char*, va_list) {}
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char*, ...) {}
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
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
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
