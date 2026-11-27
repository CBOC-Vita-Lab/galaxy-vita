// dma31: host test of vita/vgx-game/clock_census.cpp (GXV_CLOCK_CENSUS) with counting stubs for the real system calls.
// Checks: off = no counting and the real calls still answer; on = reads a picture by kind and by thread, the busiest call sites, the
// line every 5 s of process time (the census's own clock reads are not counted), several threads at once (atomics), a full site table.
//   tools/test_dma31.sh
#include "../vita/vgx-game/clock_census.cpp"
#include <cstdarg>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local int t_tid = 0x40010001;
uint64_t g_nowUs = 1000000;
unsigned g_realWide = 0, g_realLow = 0, g_realInfo = 0, g_realTod = 0, g_realRtc = 0;
std::vector<std::string> g_lines;
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
// distinct call sites (noinline: each has its own return address)
__attribute__((noinline)) SceUInt64 SiteA() { return __wrap_sceKernelGetProcessTimeWide(); }
__attribute__((noinline)) SceUInt64 SiteB() { return __wrap_sceKernelGetProcessTimeWide(); }
__attribute__((noinline)) SceUInt32 SiteC() { return __wrap_sceKernelGetProcessTimeLow(); }
__attribute__((noinline)) int SiteD() { SceKernelThreadInfo i; i.size = sizeof i; return __wrap_sceKernelGetThreadInfo(t_tid, &i); }
} // namespace

extern "C" {
SceUID sceKernelGetThreadId(void) { return t_tid; }
SceUInt64 __real_sceKernelGetProcessTimeWide(void) { ++g_realWide; return g_nowUs; }
SceUInt32 __real_sceKernelGetProcessTimeLow(void) { ++g_realLow; return static_cast<SceUInt32>(g_nowUs); }
int __real_sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo* info) {
    ++g_realInfo;
    std::snprintf(info->name, sizeof info->name, "%s", thid == 0x40010001 ? "vgxRender" : (thid == 0x40010002 ? "GameMain" : "Worker"));
    return 0;
}
int __real_sceKernelLibcGettimeofday(SceKernelTimeval*, SceKernelTimezone*) { ++g_realTod; return 0; }
int __real_sceRtcGetCurrentTick(SceRtcTick*) { ++g_realRtc; return 0; }
void port_log_summary(const char* fmt, ...) {
    char text[4096];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    g_lines.emplace_back(text);
}
}

int main() {
    // off: every wrapper passes the call on, nothing is counted
    vgxvita_clockCensusStart_v1(0);
    for (int i = 0; i < 100; ++i) { SiteA(); SiteC(); SiteD(); __wrap_sceKernelLibcGettimeofday(nullptr, nullptr); __wrap_sceRtcGetCurrentTick(nullptr); }
    unsigned total = 0;
    for (auto& k : clockcensus::g_kind) total += k.load();
    Check(total == 0 && clockcensus::g_thread[0].n.load() == 0, "off: nothing counted");
    Check(g_realWide == 101 && g_realLow == 100 && g_realInfo == 100 && g_realTod == 100 && g_realRtc == 100, "off: every real call made once per read (+1 for the start)");
    for (int i = 0; i < 200; ++i) vgxvita_clockCensusPicture_v1();
    Check(g_lines.empty(), "off: no line");

    // on: the render thread reads 3 a picture (2 at SiteA, 1 at SiteB), the game thread 1 low + 1 thread info, a worker 1
    vgxvita_clockCensusStart_v1(1);
    const unsigned realBefore = g_realWide;
    for (int pic = 0; pic < 150; ++pic) {
        SiteA(); SiteA(); SiteB();
        std::thread game([] { t_tid = 0x40010002; SiteC(); SiteD(); });
        game.join();
        std::thread worker([] { t_tid = 0x40010003; SiteB(); });
        worker.join();
        g_nowUs += 33334; // 30 pictures a second
        vgxvita_clockCensusPicture_v1();
    }
    Check(g_realWide - realBefore == 150 * 4 + 150, "on: the reads still reach the real call, and the census's own read is the only extra one a picture");
    Check(g_lines.size() == 1, "on: one line after 5 s");
    if (!g_lines.empty()) {
        const std::string& l = g_lines[0];
        std::printf("    %s\n", l.c_str());
        Check(Has(l, "clock-census: 5.0 s, 150 pictures: clock reads a picture 6.0 (process time 4.0, process time low 1.0, thread info 1.0, gettimeofday 0.0, rtc 0.0)"),
              "on: reads a picture by kind");
        Check(Has(l, "vgxRender (render) 3.0") && Has(l, "GameMain 2.0") && Has(l, "Worker 1.0"), "on: by thread, the render thread marked, names from the real call");
        Check(Has(l, "top sites") && Has(l, " 2.0") && Has(l, " 1.0"), "on: the sites (SiteA twice a picture, the others once)");
    }
    // a second window starts empty
    for (int pic = 0; pic < 150; ++pic) { SiteA(); g_nowUs += 33334; vgxvita_clockCensusPicture_v1(); }
    Check(g_lines.size() == 2 && Has(g_lines[1], "clock reads a picture 1.0 (process time 1.0,") && Has(g_lines[1], "vgxRender (render) 1.0") &&
          !Has(g_lines[1], "GameMain"), "on: the next window counts only its own reads");
    // many threads and sites at once: no crash, nothing lost (unplaced sites counted)
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t) ts.emplace_back([t] { t_tid = 0x40020000 + t; for (int i = 0; i < 20000; ++i) { SiteA(); SiteC(); } });
        for (auto& t : ts) t.join();
        for (uintptr_t a = 0x81000000u; a < 0x81000000u + 4 * 1024; a += 4) clockcensus::Count(clockcensus::kWide, reinterpret_cast<void*>(a));
        g_nowUs += 6000000;
        vgxvita_clockCensusPicture_v1();
        Check(g_lines.size() == 3 && Has(g_lines[2], "clock reads a picture 161024.0") && Has(g_lines[2], "unplaced"),
              "on: 4 threads x 40000 reads + 1024 one-off sites all counted, the sites past the table counted as unplaced");
        if (g_lines.size() == 3) std::printf("    %.300s\n", g_lines[2].c_str());
    }
    std::printf("%s dma31 clock census (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
