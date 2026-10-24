// Real Vita host bookkeeping, with only kernel and backend boundaries substituted.
#include "../platform/vgx/vita/vgx_host_galaxy_vita.cpp"

namespace {
unsigned clockReads = 0, warmReads = 0, logs = 0, censusLogs = 0, clockStarts = 0, affinityChanges = 0;
unsigned coldtraceBoundaries = 0;
SceUInt64 testUs = 1000000;
int masks[] = {kCoreGame, kCoreAux, kCoreGame};
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
}
extern "C" SceUInt64 sceKernelGetProcessTimeWide() { ++clockReads; return testUs; }
extern "C" void port_log(const char* fmt, ...) {
    ++logs;
    censusLogs += std::strstr(fmt, "thread census") != nullptr;
}
extern "C" int __wrap_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) {
    ++clockStarts;
    return 11; // Test the existing timer-clock fallback without creating a process-lifetime thread.
}
extern "C" int sceDisplayWaitVblankStart() { return 0; }
extern "C" void port_vi_retrace() {}
extern "C" void port_vgx_coldtrace_render_boundary() { ++coldtraceBoundaries; }
extern "C" SceKernelIdListType sceKernelGetThreadmgrUIDClass(SceUID id) {
    return id >= 0x40010001 && id <= 0x40010003 ? SCE_KERNEL_TMID_Thread : static_cast<SceKernelIdListType>(0);
}
extern "C" int sceKernelGetThreadInfo(SceUID id, SceKernelThreadInfo* info) {
    Require(id >= 0x40010001 && id <= 0x40010003, "only thread UIDs are queried for info");
    const unsigned n = id - 0x40010001;
    const char* names[] = {"Garbage Collector", "SceGxmDisplayQueue", "game"};
    std::strcpy(info->name, names[n]);
    info->currentPriority = 0x9f;
    info->currentCpuAffinityMask = masks[n];
    return 0;
}
extern "C" int sceKernelChangeThreadCpuAffinityMask(SceUID id, int mask) {
    const unsigned n = id - 0x40010001;
    Require(n < 2 && mask == (n == 0 ? kCoreAux : kCoreRender), "repair keeps collector/display queue off the game core");
    masks[n] = mask;
    ++affinityChanges;
    return 0;
}
namespace vgx::backend {
unsigned WarmTotal() { ++warmReads; return 1; }
unsigned WarmPending() { ++warmReads; return 0; }
void WarmSliceStats(unsigned& slices, double& worst, double& total) { ++warmReads; slices = 0; worst = total = 0; }
}

int main() {
    PinLibraryThreads(false, true);
    Require(affinityChanges == 2, "quiet mode retains both functional library-thread affinity repairs");
    Require(censusLogs == (PORT_RUNTIME_DIAGNOSTICS ? 3u : 0u), "thread census work follows the diagnostics flag");
    PinLibraryThreads(false, false);
    Require(affinityChanges == 2, "already-correct library threads are not moved again");
    splash::g_phase.store(splash::kGl);
    splash::g_stage.store(splash::kGame);
    splash::g_stageStartUs.store(testUs - 1000);
    port_vgx_vita_frame_done();
    Require(g_frames == 1 && clockStarts == 1 && splash::g_stage == splash::kTitle,
            "first frame still starts the frame clock and advances the real splash to title");
    const uint64_t titleAt = splash::g_stageStartUs.load();
    clockReads = warmReads = logs = 0;
    for (unsigned i = 1; i != 1000; ++i) { testUs += 20000; port_vgx_vita_frame_done(); }
    Require(g_frames == 1000 && clockStarts == 1 && splash::g_stageStartUs == titleAt,
            "silent bookkeeping preserves every frame and never restarts the clock or splash");
    Require(coldtraceBoundaries == (VGX_COLDTRACE ? 1000u : 0u), "probe boundaries run outside Execute even with logging off, and disappear with trace0");
    if (!PORT_LOGGING) Require(clockReads == 0 && warmReads == 0 && logs == 0,
            "silent post-startup frames do no FPS-clock, warm-stat or reporting work");
    else Require(clockReads > 0 && warmReads > 0 && logs > 0, "logging-enabled build retains sparse FPS reports");
    std::printf("PASS: 1000 host frames, real splash/clock startup and thread-affinity repair (diagnostics=%d logging=%d trace=%d)\n",
                PORT_RUNTIME_DIAGNOSTICS, PORT_LOGGING, VGX_COLDTRACE);
}
