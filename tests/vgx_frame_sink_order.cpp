#include <condition_variable>
#include <mutex>
void frame_sink_observe_ready(bool);
#include "vgx_frame_sink_fixture.cpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
std::mutex probeMutex;
std::condition_variable probeCv;
bool sawWait = false, streamReturned = false;
unsigned finishes = 0, tokens = 0, tokenValue = 0;
size_t consumed = 0;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void WaitFor(bool& flag) {
    std::unique_lock<std::mutex> lock(probeMutex);
    Require(probeCv.wait_for(lock, std::chrono::seconds(5), [&] { return flag; }), "fixture reaches the expected ordering boundary");
}
void Run(unsigned limit, unsigned order) {
    vgx::CmdBuf previous, spare;
    {
        std::lock_guard<std::mutex> lock(sMtx);
        sQueueLimit = limit;
        sAdaptDepth = false;
        sPerfSeconds = 0;
        sJobs = {&previous}; // Active backend ownership is retained in sJobs until completion.
        sFree = {&spare};
    }
    sawWait = streamReturned = false;
    finishes = tokens = tokenValue = 0;
    std::array<uint8_t, 15> stream{};
    const std::array<uint8_t, 5> copy = {0x61, 0x52, 0, 0x40, 0};
    const std::array<uint8_t, 5> finish = {0x61, 0x45, 0, 0, 2};
    const std::array<uint8_t, 5> token = {0x61, 0x47, 0, 0x12, 0x34};
    const auto& first = order == 1 ? finish : copy;
    const auto& second = order == 1 ? copy : (order == 2 ? token : finish);
    const auto& third = order == 2 ? finish : token;
    std::copy(first.begin(), first.end(), stream.begin());
    std::copy(second.begin(), second.end(), stream.begin() + 5);
    std::copy(third.begin(), third.end(), stream.begin() + 10);
    std::thread player([&] {
        const size_t read = vgx::ProcessStream(stream.data(), stream.size(), false);
        std::lock_guard<std::mutex> lock(probeMutex);
        consumed = read;
        streamReturned = true;
        probeCv.notify_all();
    });
    if (limit == 1) {
        WaitFor(sawWait);
        {
            std::lock_guard<std::mutex> lock(probeMutex);
            Require(!streamReturned && finishes == (order == 1 ? 1u : 0u) && tokens == 0,
                "copy blocks later finish/token but not a finish preceding it");
        }
        {
            std::lock_guard<std::mutex> lock(sMtx);
            Require(sJobs.size() == 1 && sJobs.front() == &previous && sFree.size() == 1,
                "one-job limit counts the active backend job, even with a free buffer");
            sJobs.erase(sJobs.begin());
            sFree.push_back(&previous);
        }
        sCv.notify_all();
    }
    WaitFor(streamReturned);
    player.join();
    Require(consumed == stream.size() && finishes == 1 && tokens == 1 && tokenValue == 0x1234,
        "actual parser consumes all commands and delivers ordered PE notifications");
    {
        std::lock_guard<std::mutex> lock(sMtx);
        Require(sJobs.size() == limit, "queued ownership matches the configured limit");
        if (limit == 2) Require(sJobs.front() == &previous && !sawWait,
            "two-job limit allows copy and later finish while the prior backend job remains active");
        const vgx::CmdBuf* submitted = sJobs.back();
        Require(submitted->cmds.size() == 2 && submitted->cmds[0].type == vgx::CMD_COPY_DISP &&
            submitted->cmds[1].type == vgx::CMD_PRESENT, "the real frontend submits the copy/present in original order");
        sJobs.clear();
        sFree.clear();
    }
}
}

void frame_sink_observe_ready(bool ready) {
    if (!ready) {
        std::lock_guard<std::mutex> lock(probeMutex);
        sawWait = true;
        probeCv.notify_all();
    }
}
extern "C" {
volatile uint32_t __PortGXPERegs[0x400] = {};
int64_t port_host_time_ns() { return 0; }
int64_t port_thread_cpu_us() { return 0; }
void port_fatal(const char*, ...) { Require(false, "unexpected fatal path"); std::abort(); }
void port_log(const char*, ...) {}
void port_irq_raise(int irq) {
    std::lock_guard<std::mutex> lock(probeMutex);
    if (irq == PORT_IRQ_PE_FINISH) ++finishes;
    else if (irq == PORT_IRQ_PE_TOKEN) {
        ++tokens;
        tokenValue = reinterpret_cast<volatile uint16_t*>(__PortGXPERegs)[7];
    }
    else Require(false, "only expected PE interrupts are raised");
}
}

int main() {
    vgx::GuestHooks hooks;
    hooks.peFinish = PeFinish;
    hooks.peToken = PeToken;
    hooks.displayCopy = DisplayCopy;
    vgx::SetGuestHooks(hooks);
    vgx::SetFrameSink(FrameSink);
    Require(!sThreadMade && !sStarted, "fixture must not create an implicit rendering consumer");
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        for (unsigned limit : {1u, 2u})
            for (unsigned order = 0; order < 3; ++order) Run(limit, order);
    Require(!sThreadMade && !sStarted, "actual sink and parser never start an unobserved render consumer");
    std::printf("PASS: 120 actual parser/frontend/FrameSink orderings; finish/token barriers at queue limits 1/2, diagnostics=%d; not FPS\n",
        PORT_RUNTIME_DIAGNOSTICS);
}
