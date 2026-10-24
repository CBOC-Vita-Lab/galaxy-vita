// Exercise the real render loop with a deterministic GPU boundary and clock.
#include "../platform/src/gx/vgx_galaxy.cpp"

namespace {
struct Finished {};
int64_t nowNs = 1000000000;
unsigned clockReads = 0, executed = 0, frameDone = 0, inputDone = 0, logCalls = 0;
constexpr unsigned kPictures = 144;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int64_t Cost(unsigned picture) {
    if (picture < 24) return 30000000;
    if (picture < 48) return 22000000;
    if (picture < 72) return 19000000;
    if (picture == 72) return 1000000000; // A shader stall must still be capped at 60 ms by adaptation.
    if (picture < 96) return 18000000;
    if (picture < 120) return 26000000;
    return 20000000;
}
unsigned ExpectedLimit(unsigned picture) {
    return (picture >= 24 && picture < 72) || (picture >= 120 && picture < 144) ? 1 : 3;
}
}

extern "C" int64_t port_host_time_ns() { ++clockReads; return nowNs; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" uint32_t port_frameskip_count() { return 0; }
extern "C" void port_perf_game_totals(int64_t* wall, uint32_t* turns, int64_t* cpu, int64_t* max) {
    *wall = *cpu = *max = 0; *turns = 0;
}
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" void port_log(const char*, ...) { ++logCalls; }
extern "C" void port_log_summary(const char*, ...) { ++logCalls; }
extern "C" void port_input_latency_frame_done(unsigned long long shown) {
    Require(shown == ++inputDone, "one input-latency completion per executed picture, in order");
}
extern "C" void port_headless_write_png(const char*, const unsigned char*, int, int) {
    Require(false, "no capture requested");
}
extern "C" int port_vgx_vita_start() { return 1; }
extern "C" void port_vgx_vita_ready() {}
extern "C" int port_vgx_vita_splash_active() { return 0; }
extern "C" void port_vgx_vita_idle() {}
extern "C" void port_vgx_vita_idle_gap(unsigned) {}
extern "C" void port_vgx_vita_frame_done() { ++frameDone; }

namespace vgx::backend {
bool Init(int, int) { return true; }
void SetSkipDraws(bool skip) { Require(!skip, "ordinary-play queue must not skip backend draws"); }
void RequestCapture(bool cap) { Require(!cap, "no unrequested capture"); }
unsigned WarmPending() { return 0; }
void WarmIdle() {}
Stats GetStats() { return {}; }
const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height) {
    static std::vector<uint8_t> empty;
    width = height = 0;
    return empty;
}
void Execute(const CmdBuf&) {
    Require(sQueueLimit == ExpectedLimit(executed), "queue adaptation follows identical 24-picture costs in both builds");
    if (!PORT_RUNTIME_DIAGNOSTICS) {
        Require(sBackStage.load() == -7 && sBackSince.load() == -11,
                "quiet render loop must not sample/store diagnostic stage timestamps");
    } else {
        Require(sBackStage.load() == 1 && sBackSince.load() == nowNs,
                "diagnostic build retains its executing-stage report");
    }
    if (executed == kPictures) throw Finished{};
    Require(clockReads == executed * (PORT_RUNTIME_DIAGNOSTICS ? 5u : 2u) + (PORT_RUNTIME_DIAGNOSTICS ? 2u : 1u),
            "only adaptive timing remains in quiet mode; all three diagnostic timestamps stay gated");
    nowNs += Cost(executed++);
}
}

int main() {
    std::vector<vgx::CmdBuf> jobs(kPictures + 1);
    for (auto& job : jobs) sJobs.push_back(&job);
    sMaxInFlight = 2;
    sQueueLimit = 3;
    sAdaptDepth = true;
    sPerfSeconds = 0;
    sBackStage.store(-7);
    sBackSince.store(-11);
    try { RenderMain(); Require(false, "fixture must finish"); }
    catch (const Finished&) {}
    Require(executed == kPictures && frameDone == kPictures && inputDone == kPictures && sFramesShown == kPictures,
            "diagnostic gating never drops presentation, input completion or frame bookkeeping");
    Require(sQueueLimit == 3 && sJobs.size() == 1 && sFree.size() == kPictures,
            "queue opens at the inclusive 20 ms threshold; completed buffers are returned");
    for (unsigned i = 0; i != kPictures; ++i)
        Require(sFree[i] == &jobs[i], "completed jobs retain FIFO order");
    if (!PORT_RUNTIME_DIAGNOSTICS) {
        clockReads = logCalls = 0;
        port_vgx_debug_dump();
        Require(clockReads == 0 && logCalls == 0, "disabled stall diagnostics must not invent a zero-age stage or read clocks");
    }
    sJobs.clear();
    sFree.clear();
    std::printf("PASS: 144 real render-loop jobs, adaptive thresholds/hysteresis/stall cap, FIFO and diagnostic gating (diagnostics=%d)\n",
                PORT_RUNTIME_DIAGNOSTICS);
}
