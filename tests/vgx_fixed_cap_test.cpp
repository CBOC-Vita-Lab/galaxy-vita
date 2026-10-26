#include <condition_variable>
#include <mutex>
void frame_sink_observe_ready(bool) {}
#include "vgx_frame_sink_fixture.cpp"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
namespace vgx { unsigned FixedCapHudDepth(); }

namespace {
std::vector<int> calls;
std::array<uint8_t, 128> fifo{};
uint64_t clockUs = 1000000;
unsigned latencyDone = 0;
unsigned long long swaps = 0;
std::string summary;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void Enqueue(bool eligible, bool marker = true) {
    if (marker) vgx::WriteBP(0xFF, eligible ? vgx::kPresentAllowedMarker : vgx::kPresentSuppressedMarker);
    // Existing HUD markers nest exactly as before, and unknown marker values are benign.
    vgx::WriteBP(0xFF, 1); vgx::WriteBP(0xFF, 1);
    Require(vgx::FixedCapHudDepth() == 2, "HUD nesting remains unchanged by presentation metadata");
    vgx::WriteBP(0xFF, 0x7F);
    Require(vgx::FixedCapHudDepth() == 2, "unknown marker does not change HUD nesting");
    vgx::WriteBP(0xFF, 2); vgx::WriteBP(0xFF, 2);
    Require(vgx::FixedCapHudDepth() == 0, "HUD end markers still balance nesting");
    vgx::FrontPresent(false, nullptr);
}
}
namespace vgx::backend {
void Execute(const CmdBuf& b) {
    calls.push_back(2);
    for (const auto& c : b.cmds) calls.push_back(100 + c.type);
    calls.push_back(3);
}
}
extern "C" {
volatile uint32_t __PortGXPERegs[0x400] = {};
int64_t port_host_time_ns() { return clockUs * 1000; }
int64_t port_thread_cpu_us() { return 0; }
void port_input_latency_frame_done(unsigned long long) { ++latencyDone; }
void port_fatal(const char*, ...) { std::abort(); }
void port_irq_raise(int) {}
int port_vgx_vita_fixed_cap_supported() { return 1; }
void port_vgx_vita_set_present_allowed(int allowed) { calls.push_back(allowed ? 10 : 11); }
void port_vgx_vita_finish_unpresented_frame() { calls.push_back(4); }
unsigned long long port_vgx_vita_game_swaps() { return swaps; }
void port_log_summary(const char* format, ...) {
    char buf[1024];
    va_list args; va_start(args, format); vsnprintf(buf, sizeof buf, format, args); va_end(args);
    summary = buf;
}
void port_log(const char*, ...) {}
void port_perf_game_totals(int64_t* work, uint32_t* turns, int64_t* cpu, int64_t* max) {
    *work = *cpu = *max = 0; *turns = clockUs / 16683;
}
uint32_t port_frameskip_count() { return 0; }
uint8_t* __PortGXFifoPtr = fifo.data();
uint8_t* __PortGXFifoLimit = fifo.data() + fifo.size();
void __PortGXFifoOverflow() { std::abort(); }
}
int main(int argc, char**) {
    const bool fixed = argc == 1;
    setenv("GXV_FPS_LIMIT", fixed ? "30" : "0", 1);
    setenv("GXV_PACE", "0", 1);
    for (unsigned i = 0; i < 4; ++i, clockUs += 16683) {
        Require(port_vgx_pace_turn(0) == (fixed ? 0 : -1), "actual producer option selects fixed required draws or old path");
    }
    Require(__PortGXFifoPtr - fifo.data() == (fixed ? 20 : 0), "disabled producer writes no extra FIFO bytes");
    if (fixed) for (unsigned i = 0; i < 4; ++i)
        Require(fifo[5 * i] == 0x61 && fifo[5 * i + 1] == 0xFF && fifo[5 * i + 4] ==
                (i % 2 ? vgx::kPresentSuppressedMarker : vgx::kPresentAllowedMarker), "actual producer emits ordered marker bytes without CharaAA");
    vgx::CmdBuf a, b, c;
    sQueueLimit = 3;
    sFree = {&a, &b, &c};
    vgx::SetFrameSink(FrameSink);
    Enqueue(false);
    Enqueue(true);
    Enqueue(false);
    Require(sJobs.size() == 3 && !sJobs[0]->presentAllowed && sJobs[1]->presentAllowed && !sJobs[2]->presentAllowed,
            "actual WriteBP/FrontPresent/sink preserves metadata despite producer lead");
    for (unsigned i = 0; i < 3; ++i) {
        auto* job = sJobs.front();
        job->cmds.push_back({vgx::CMD_TEX_FREE, 0}); // retirement must follow any commands after the display copy
        calls.clear();
        ExecuteJob(*job, fixed);
        CompleteJob();
        Require(calls.front() == (fixed ? (i % 2 ? 10 : 11) : 2), "setter precedes backend ownership only when fixed");
        Require(calls[calls.size() - (fixed ? 3 : 2)] == 100 + vgx::CMD_TEX_FREE,
                "all backend commands including copy tail execute");
        Require(calls.back() == (fixed ? 4 : 3), "finish follows the complete Execute tail for both picture kinds");
        sJobs.erase(sJobs.begin());
        job->clear();
        Require(job->presentAllowed, "buffer clear restores default eligibility");
        sFree.push_back(job);
    }
    Require(sFramesShown == 3 && latencyDone == 3, "completed jobs retain queue and input anchors, including hidden pictures");
#if !PORT_RUNTIME_DIAGNOSTICS
    sPerfSeconds = 1;
    PerfFrame(0, 0);
    clockUs += 1000000;
    swaps = 2;
    PerfFrame(0, 0);
    Require(summary.find(fixed ? "swap FPS 2.0" : "shown 0.0 fps") != std::string::npos,
            "basic logs count actual game swaps rather than completed jobs and preserve off-path wording");
#endif
    Enqueue(true, false);
    Require(sJobs.front()->presentAllowed, "reused buffer cannot inherit a suppressed marker");
    std::puts("PASS: production fixed producer, real BP/frontend/sink metadata, complete-job host adapter; synthetic, not gameplay");
}
