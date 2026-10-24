// Retain the actual time base and sleep functions; substitute only OS clock boundaries.
#include <time.h>
#include <psp2/kernel/processmgr.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
extern "C" int host_clock_gettime_probe(clockid_t, timespec*);
extern "C" int host_nanosleep_probe(const timespec*, timespec*);
#define clock_gettime host_clock_gettime_probe
#define nanosleep host_nanosleep_probe
#if defined(HOST_CLOCK_FIXTURE)
#include "clock-source.cpp"
#else
#include "../platform/src/os/os_alarm.cpp"
#endif
#undef clock_gettime
#undef nanosleep

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint64_t processUs = 0;
unsigned processReads = 0, wrapperReads = 0, virtualReads = 0, polls = 0, sleepCalls = 0;
bool virtualEnabled = false;
int64_t virtualNs = 777000111999ll, virtualSleep = 0;
timespec requestedSleep{}, portableTime{345, 678901234};
clockid_t requestedClock = static_cast<clockid_t>(-1);
std::array<OSAlarm*, 8> fired{};
unsigned firedCount = 0;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void ResetCounters() { processReads = wrapperReads = virtualReads = 0; }
void AlarmFired(OSAlarm* alarm, OSContext*) {
    Require(firedCount < fired.size(), "bounded alarm deliveries");
    fired[firedCount++] = alarm;
}
void SetElapsed(int64_t ns) {
    if (virtualEnabled) virtualNs = sBootNs + ns;
#if defined(__vita__)
    else processUs = static_cast<uint64_t>(sBootNs / 1000 + ns / 1000);
#else
    else {
        const int64_t absolute = sBootNs + ns;
        portableTime.tv_sec = absolute / 1000000000ll;
        portableTime.tv_nsec = absolute % 1000000000ll;
    }
#endif
}
}

extern "C" SceUInt64 sceKernelGetProcessTimeWide(void) { ++processReads; return processUs; }
extern "C" int port_vclock_enabled() { return virtualEnabled; }
extern "C" int64_t port_vclock_now() { ++virtualReads; return virtualNs; }
extern "C" void port_vclock_sleep_ns(int64_t ns) { virtualSleep = ns; }
extern "C" void port_vclock_poke(void*) {}
extern "C" OSContext* OSGetCurrentContext() { return nullptr; }
extern "C" int host_clock_gettime_probe(clockid_t clock, timespec* result) {
    ++wrapperReads;
    requestedClock = clock;
#if defined(__vita__)
    // The actual linked Newlib MONOTONIC branch reads this same SDK clock.
    const uint64_t us = sceKernelGetProcessTimeWide();
    result->tv_sec = static_cast<time_t>(us / 1000000);
    result->tv_nsec = static_cast<long>(us % 1000000 * 1000);
#else
    *result = portableTime;
#endif
    return 0;
}
extern "C" int host_nanosleep_probe(const timespec* request, timespec*) {
    ++sleepCalls;
    requestedSleep = *request;
    return 0;
}
extern "C" void port_irq_poll() { ++polls; }
extern "C" int port_vita_local_time_seconds(int64_t* value) { *value = 1730000000ll; return 1; }

int main(int argc, char** argv) {
    Require(argc == 2, "supply clock mode");
    const bool testVirtual = std::strcmp(argv[1], "virtual") == 0;
    if (testVirtual) {
        virtualEnabled = true;
        Require(port_host_time_ns() == virtualNs, "virtual time retains priority over platform clocks");
        Require(virtualReads == 1 && processReads == 0 && wrapperReads == 0, "virtual time never queries a native clock");
        sBootNs = virtualNs - 4000;
        sBootTicks = 91;
        Require(OSGetTime() == 91 + 243, "virtual game ticks retain the existing epoch and rate");
        Require(processReads == 0 && wrapperReads == 0, "virtual game ticks never query a native clock");
        port_host_sleep_ns(1234567890);
        Require(virtualSleep == 1234567890 && sleepCalls == 0, "virtual sleep stays in virtual scheduler");
    } else {
#if defined(__vita__)
        const std::array<uint64_t, 13> values = {0, 1, 999, 999999, 1000000, 1000001,
            4294967295ull, 4294967296ull, 4294967297ull, 86399999999ull, 86400000000ull,
            123456789012345ull, 1000000000000000ull};
        for (uint64_t us : values) {
            processUs = us;
            ResetCounters();
            Require(port_host_time_ns() == static_cast<int64_t>(us * 1000), "native elapsed clock preserves exact 64-bit microseconds");
            Require(processReads == 1 && wrapperReads == 0 && virtualReads == 0,
                "native elapsed clock must bypass the redundant timespec wrapper");
            if (us / 1000000 <= 0x7fffffffull) {
                const int64_t old = static_cast<int64_t>(us / 1000000) * 1000000000ll + us % 1000000 * 1000;
                Require(port_host_time_ns() == old, "same result as linked Newlib within its 32-bit seconds range");
            }
        }
        uint64_t random = 0x12345678;
        for (unsigned i = 0; i < 10000; ++i) {
            random = random * 6364136223846793005ull + 1;
            processUs = random % 2000000000000000ull;
            const int64_t old = static_cast<int64_t>(processUs / 1000000) * 1000000000ll + processUs % 1000000 * 1000;
            Require(port_host_time_ns() == old, "random native readings preserve the old clock arithmetic");
        }
        processUs = 123456789;
        initTimeBase();
        Require(sBootNs == static_cast<int64_t>(processUs * 1000), "time base samples the same process epoch");
        Require(sBootTicks == (1730000000ll - 946684800ll) * kTimerClock, "calendar epoch remains local RTC based");
        processUs += 4000;
        const int64_t expected = sBootTicks + 243000;
        Require(OSGetTime() == expected && OSGetTick() == static_cast<OSTick>(expected),
            "game time and wrapping game tick retain their rate and epoch");
        Require(polls == 2, "OS time queries retain interrupt safe points");
#else
        Require(port_host_time_ns() == 345678901234ll, "portable clock retains nanosecond resolution and epoch");
        Require(wrapperReads == 1 && processReads == 0 && requestedClock == CLOCK_MONOTONIC,
            "portable build continues to use CLOCK_MONOTONIC");
        sBootNs = 345678897234ll;
        sBootTicks = 91;
        Require(OSGetTime() == 91 + 243 && polls == 1, "portable game ticks and safe points are unchanged");
#endif
        port_host_sleep_ns(1234567890);
        Require(sleepCalls == 1 && requestedSleep.tv_sec == 1 && requestedSleep.tv_nsec == 234567890,
            "real sleep retains its original nanosecond conversion");
        port_host_sleep_ns(0);
        port_host_sleep_ns(-1);
        Require(sleepCalls == 1 && virtualSleep == 0, "nonpositive sleeps remain no-ops");
    }
    for (int64_t ns : {-1000000000000ll, -4001ll, -4000ll, -3999ll, -1ll, 0ll, 1ll, 3999ll,
            4000ll, 4001ll, 1000000ll, 1000000000000ll})
        Require(nsToTicks(ns) == ns * 243 / 4000, "unchanged integer tick conversion");
#if defined(__vita__)
    if (!testVirtual) {
        processUs = 4294967296ull;
        initTimeBase();
        const int64_t originTicks = sBootTicks;
        int64_t previous = originTicks;
        for (int64_t elapsed = 0; elapsed < 1000000; ++elapsed) {
            processUs = 4294967296ull + elapsed;
            const int64_t now = rawTime();
            Require(now == originTicks + nsToTicks(elapsed * 1000), "exhaustive native microsecond-to-tick equivalence");
            Require(now >= previous, "game time remains monotonic across every microsecond residue");
            previous = now;
        }
        for (int64_t elapsed : {-1000001ll, -5ll, -4ll, -3ll, -1ll, 0ll, 1ll, 3ll, 4ll, 5ll,
                3999ll, 4000ll, 4001ll, 4294967296ll, 1000000000000ll}) {
            processUs = static_cast<uint64_t>(4294967296ll + elapsed);
            Require(rawTime() == originTicks + nsToTicks(elapsed * 1000), "tick equivalence at signs, residue and large-uptime boundaries");
        }
        constexpr int64_t limitUs = INT64_MAX / 1000;
        processUs = 0;
        initTimeBase();
        processUs = limitUs;
        Require(port_host_time_ns() == limitUs * 1000 && rawTime() == sBootTicks + nsToTicks(limitUs * 1000),
            "maximum supported positive elapsed time remains exact without signed overflow");
        initTimeBase();
        processUs = 0;
        Require(rawTime() == sBootTicks + nsToTicks(-limitUs * 1000),
            "maximum supported negative elapsed time preserves truncation without signed overflow");
    }
#endif
    OSCalendarTime calendar{};
    OSTicksToCalendarTime(0, &calendar);
    Require(calendar.year == 2000 && calendar.mon == 0 && calendar.mday == 1 && calendar.wday == 6 &&
        calendar.hour == 0 && calendar.min == 0 && calendar.sec == 0, "calendar epoch remains January 1 2000");
    OSTicksToCalendarTime(59ll * 86400 * kTimerClock + kTimerClock + 243, &calendar);
    Require(calendar.year == 2000 && calendar.mon == 1 && calendar.mday == 29 && calendar.yday == 59 &&
        calendar.sec == 1 && calendar.msec == 0 && calendar.usec == 4, "leap-day and subsecond calendar conversion is unchanged");
    initTimeBase();
    sBootTicks = 0;
    OSAlarm periodic{}, once{}, tied{};
    periodic.handler = once.handler = tied.handler = AlarmFired;
    periodic.fire = 243; periodic.period = 243;
    once.fire = tied.fire = 486;
    insertAlarm(&periodic); insertAlarm(&once); insertAlarm(&tied);
    SetElapsed(3000);
    alarmIrq();
    Require(firedCount == 0, "alarms never fire before deadline");
    SetElapsed(4000);
    alarmIrq();
    Require(firedCount == 1 && fired[0] == &periodic && periodic.fire == 486,
        "periodic alarm fires once at exact tick deadline");
    SetElapsed(20000);
    alarmIrq();
    Require(firedCount == 4 && fired[1] == &once && fired[2] == &tied && fired[3] == &periodic &&
        periodic.fire == 1458 && sAlarmHead == &periodic,
        "stable equal-deadline ordering and missed-period skipping remain unchanged");
    removeAlarm(&periodic);
    rearmTimer();
    Require(sAlarmHead == nullptr && sTimerDeadline == INT64_MAX, "empty alarm queue remains disarmed");
    std::printf("PASS: host elapsed clock %s native=%d diagnostics=%d; clock domains, 64-bit values, ticks and sleeps\n", argv[1],
#if defined(__vita__)
        1,
#else
        0,
#endif
        PORT_RUNTIME_DIAGNOSTICS);
}
