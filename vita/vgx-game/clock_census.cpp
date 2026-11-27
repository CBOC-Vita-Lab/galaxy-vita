// dma31 (17 Nov 2026), GXV_CLOCK_CENSUS: how many times a picture the game reads a clock on the console, by thread and by call site.
// Every clock the program can read goes through one of these system calls, so link wrappers around them see every read: the port's own
// (port_host_time_ns, NowMs, the wait wrappers, beprof), newlib's (clock_gettime / steady_clock::now / gettimeofday / time), pthreads' and
// vitaGL's. Each read is a system call on the console (~0.87 us measured by GXV_BE_PROF's "clock 865 ns"; sceKernelGetThreadInfo, the
// thread CPU-time read, more), which is why Strikers counted them (286 a picture in their renderer).
//   sceKernelGetProcessTimeWide   the port's clock, newlib's clock_gettime(CLOCK_MONOTONIC) / steady_clock
//   sceKernelGetProcessTimeLow    pthreads (timed waits), newlib's clock()
//   sceKernelGetThreadInfo        port_thread_cpu_us (a thread's CPU time) and the HEAP FULL report
//   sceKernelLibcGettimeofday     newlib's gettimeofday / time
//   sceRtcGetCurrentTick          the RTC (saves, the online service)
// GXV_CLOCK_CENSUS=1 (default 0): the reads are counted per thread and per call site (the return address: the instruction after the call,
// in the caller; for newlib's own wrappers it is inside clock_gettime / gettimeofday), and every 5 s the render thread writes one basic-log
// line "clock-census:" (reads a picture by kind, by thread, the 12 busiest call sites) - resolve the sites with
//   arm-vita-eabi-addr2line -f -C -e galaxy_native <address>   (the line gives the address of a known function to check the load base).
// Off (0): every wrapper is one relaxed load and a branch in front of the system call; nothing else changes. The census itself reads the
// clock through the real calls, so it does not count itself. A picture is a display-queue entry: wait_wrap.cpp's sceGxmDisplayQueueAddEntry
// wrapper calls vgxvita_clockCensusPicture_v1 (the render thread), which reads GXV_CLOCK_CENSUS at its first call and starts the census
// there; the renderer itself is not changed (its PGO profile stays valid). The cold-trace build (VGX_COLDTRACE_WRAPPERS) has no
// wait_wrap.cpp: there the census never starts.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));

extern "C" {
SceUInt64 __real_sceKernelGetProcessTimeWide(void);
SceUInt32 __real_sceKernelGetProcessTimeLow(void);
int __real_sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo* info);
int __real_sceKernelLibcGettimeofday(SceKernelTimeval* tv, SceKernelTimezone* tz);
int __real_sceRtcGetCurrentTick(SceRtcTick* tick);
}

namespace clockcensus {
enum Kind : unsigned { kWide, kLow, kThreadInfo, kTimeOfDay, kRtc, kKinds };
const char* const kKindName[kKinds] = {"process time", "process time low", "thread info", "gettimeofday", "rtc"};
constexpr unsigned kThreads = 24, kSites = 512, kTop = 12;
struct ThreadSlot {
    std::atomic<int> tid{0};
    std::atomic<uint32_t> n{0};
    char name[32] = {};
};
struct SiteSlot {
    std::atomic<uintptr_t> at{0};
    std::atomic<uint32_t> n{0};
};
std::atomic<int> g_on{0};
std::atomic<uint32_t> g_kind[kKinds];
std::atomic<uint32_t> g_otherThreads{0}, g_otherSites{0};
ThreadSlot g_thread[kThreads];
SiteSlot g_site[kSites];
int g_render = 0;             // the thread that started the census (the back end's render thread)
uint32_t g_pictures = 0;      // (render thread only)
uint64_t g_windowUs = 0;      // (render thread only)
uint64_t g_reportEveryUs = 5000000;

void CountThread() {
    const int tid = sceKernelGetThreadId();
    unsigned h = static_cast<unsigned>(tid) * 2654435761u;
    for (unsigned k = 0; k < kThreads; ++k) {
        ThreadSlot& s = g_thread[(h + k) % kThreads];
        int seen = s.tid.load(std::memory_order_relaxed);
        if (seen == 0) {
            int empty = 0;
            if (s.tid.compare_exchange_strong(empty, tid, std::memory_order_relaxed)) seen = tid;
            else seen = empty;
        }
        if (seen == tid) { s.n.fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_otherThreads.fetch_add(1, std::memory_order_relaxed);
}
void CountSite(uintptr_t at) {
    unsigned h = static_cast<unsigned>(at >> 1) * 2654435761u;
    for (unsigned k = 0; k < 8; ++k) {
        SiteSlot& s = g_site[(h + k) % kSites];
        uintptr_t seen = s.at.load(std::memory_order_relaxed);
        if (seen == 0) {
            uintptr_t empty = 0;
            if (s.at.compare_exchange_strong(empty, at, std::memory_order_relaxed)) seen = at;
            else seen = empty;
        }
        if (seen == at) { s.n.fetch_add(1, std::memory_order_relaxed); return; }
    }
    g_otherSites.fetch_add(1, std::memory_order_relaxed);
}
inline void Count(Kind k, void* at) {
    g_kind[k].fetch_add(1, std::memory_order_relaxed);
    CountThread();
    CountSite(reinterpret_cast<uintptr_t>(at));
}

// One "clock-census:" line (and the window starts again). Render thread.
void Report(double seconds) {
    const double n = g_pictures ? static_cast<double>(g_pictures) : 1.0;
    uint32_t kinds[kKinds], total = 0;
    for (unsigned k = 0; k < kKinds; ++k) { kinds[k] = g_kind[k].exchange(0, std::memory_order_relaxed); total += kinds[k]; }
    char line[2048];
    int at = std::snprintf(line, sizeof line, "clock-census: %.1f s, %u pictures: clock reads a picture %.1f (", seconds, g_pictures, total / n);
    for (unsigned k = 0; k < kKinds && at < static_cast<int>(sizeof line); ++k)
        at += std::snprintf(line + at, sizeof line - at, "%s%s %.1f", k ? ", " : "", kKindName[k], kinds[k] / n);
    if (at < static_cast<int>(sizeof line)) at += std::snprintf(line + at, sizeof line - at, "); by thread:");
    for (unsigned t = 0; t < kThreads; ++t) {
        ThreadSlot& s = g_thread[t];
        const int tid = s.tid.load(std::memory_order_relaxed);
        const uint32_t c = s.n.exchange(0, std::memory_order_relaxed);
        if (!tid || !c) continue;
        if (!s.name[0]) {
            SceKernelThreadInfo info;
            std::memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (__real_sceKernelGetThreadInfo(tid, &info) >= 0) std::snprintf(s.name, sizeof s.name, "%s", info.name);
            else std::snprintf(s.name, sizeof s.name, "0x%x", static_cast<unsigned>(tid));
        }
        if (at < static_cast<int>(sizeof line))
            at += std::snprintf(line + at, sizeof line - at, " %s%s %.1f", s.name, tid == g_render ? " (render)" : "", c / n);
    }
    const uint32_t otherT = g_otherThreads.exchange(0, std::memory_order_relaxed);
    if (otherT && at < static_cast<int>(sizeof line)) at += std::snprintf(line + at, sizeof line - at, " others %.1f", otherT / n);
    // the busiest sites (a partial selection: kTop passes over the table)
    uintptr_t topAt[kTop] = {};
    uint32_t topN[kTop] = {};
    for (unsigned i = 0; i < kSites; ++i) {
        const uint32_t c = g_site[i].n.exchange(0, std::memory_order_relaxed);
        if (!c) continue;
        const uintptr_t a = g_site[i].at.load(std::memory_order_relaxed);
        unsigned j = kTop;
        while (j > 0 && topN[j - 1] < c) --j;
        if (j >= kTop) continue;
        for (unsigned m = kTop - 1; m > j; --m) { topN[m] = topN[m - 1]; topAt[m] = topAt[m - 1]; }
        topN[j] = c;
        topAt[j] = a;
    }
    if (at < static_cast<int>(sizeof line))
        at += std::snprintf(line + at, sizeof line - at, "; top sites (addr2line; Report is at %p):", reinterpret_cast<void*>(&Report));
    for (unsigned j = 0; j < kTop && topN[j] && at < static_cast<int>(sizeof line); ++j)
        at += std::snprintf(line + at, sizeof line - at, " %p %.1f", reinterpret_cast<void*>(topAt[j]), topN[j] / n);
    const uint32_t otherS = g_otherSites.exchange(0, std::memory_order_relaxed);
    if (otherS && at < static_cast<int>(sizeof line)) std::snprintf(line + at, sizeof line - at, " (unplaced %.1f)", otherS / n);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "%s\n", line);
    g_pictures = 0;
}
} // namespace clockcensus

extern "C" {
// GXV_CLOCK_CENSUS: 0 off (the wrappers only pass the calls on), 1 count and report every 5 s. Called on the render thread.
void vgxvita_clockCensusStart_v1(int mode) {
    using namespace clockcensus;
    g_render = sceKernelGetThreadId();
    g_windowUs = __real_sceKernelGetProcessTimeWide();
    g_pictures = 0;
    g_on.store(mode > 0 ? 1 : 0, std::memory_order_relaxed);
}
// The end of a picture (render thread: a display-queue entry). The first call reads GXV_CLOCK_CENSUS and starts the census when it is
// set; then every 5 s a line.
void vgxvita_clockCensusPicture_v1(void) {
    using namespace clockcensus;
    static int asked = 0;
    if (__builtin_expect(!asked, 0)) {
        asked = 1;
        const char* e = std::getenv("GXV_CLOCK_CENSUS");
        if (e && std::atoi(e) > 0 && !g_on.load(std::memory_order_relaxed)) vgxvita_clockCensusStart_v1(1);
    }
    if (!g_on.load(std::memory_order_relaxed)) return;
    ++g_pictures;
    const uint64_t now = __real_sceKernelGetProcessTimeWide();
    if (now - g_windowUs < g_reportEveryUs) return;
    Report(static_cast<double>(now - g_windowUs) / 1e6);
    g_windowUs = now;
}

SceUInt64 __wrap_sceKernelGetProcessTimeWide(void) {
    if (__builtin_expect(clockcensus::g_on.load(std::memory_order_relaxed) != 0, 0)) clockcensus::Count(clockcensus::kWide, __builtin_return_address(0));
    return __real_sceKernelGetProcessTimeWide();
}
SceUInt32 __wrap_sceKernelGetProcessTimeLow(void) {
    if (__builtin_expect(clockcensus::g_on.load(std::memory_order_relaxed) != 0, 0)) clockcensus::Count(clockcensus::kLow, __builtin_return_address(0));
    return __real_sceKernelGetProcessTimeLow();
}
int __wrap_sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo* info) {
    if (__builtin_expect(clockcensus::g_on.load(std::memory_order_relaxed) != 0, 0)) clockcensus::Count(clockcensus::kThreadInfo, __builtin_return_address(0));
    return __real_sceKernelGetThreadInfo(thid, info);
}
int __wrap_sceKernelLibcGettimeofday(SceKernelTimeval* tv, SceKernelTimezone* tz) {
    if (__builtin_expect(clockcensus::g_on.load(std::memory_order_relaxed) != 0, 0)) clockcensus::Count(clockcensus::kTimeOfDay, __builtin_return_address(0));
    return __real_sceKernelLibcGettimeofday(tv, tz);
}
int __wrap_sceRtcGetCurrentTick(SceRtcTick* tick) {
    if (__builtin_expect(clockcensus::g_on.load(std::memory_order_relaxed) != 0, 0)) clockcensus::Count(clockcensus::kRtc, __builtin_return_address(0));
    return __real_sceRtcGetCurrentTick(tick);
}
} // extern "C"
