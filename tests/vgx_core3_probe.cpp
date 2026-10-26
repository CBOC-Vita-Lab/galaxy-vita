#include "vgx_core3_probe.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vgx::core3probe;
namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
struct Kernel {
    uint64_t wall = 1000000, cpu = 700, stop = wall + 21000000;
    int pin = 0, mask = kSystemMask, failRead = 0, backward = 0, delayFail = 0, spinFail = 0;
    unsigned reads = 0, spins = 0, sleeps = 0, starts = 0, reports = 0;
    unsigned share = 100, preemptUs = 0;
    int error = 0;
    const char* operation = "";
    std::vector<uint64_t> chunks;
    int PinSystem() { return pin; }
    int Read(Sample& sample) {
        ++reads;
        if (failRead && reads == unsigned(failRead)) return -5;
        if (wall >= stop) return -999;
        wall += 2; cpu += 2;
        sample = {wall, cpu, mask};
        if (backward && reads == unsigned(backward)) sample.cpuUs = 0;
        return 0;
    }
    int Spin(uint32_t us, uint64_t& iterations) {
        ++spins;
        Require(us > 0 && us <= kChunkUs, "runtime never requests a spin larger than 250 us");
        if (spinFail) return -7;
        // Integrate actual thread CPU (not spin throughput) over the requested wall slice.
        wall += us + preemptUs;
        cpu += us * share / 100;
        iterations = us * share;
        chunks.push_back(wall);
        return 0;
    }
    int Sleep(uint32_t us) {
        ++sleeps;
        Require(us > 0 && us <= kWindowUs, "sleep is bounded by this quota window, never catch-up credit");
        if (delayFail) return -8;
        wall += us;
        return 0;
    }
    void Started() { ++starts; }
    void Report(const Totals& totals) {
        ++reports;
        Require(totals.wallUs >= kReportUs && totals.cpuUs > 0, "telemetry reports measured CPU and wall time");
        Require(totals.cpuUs * 100 < totals.wallUs * 50, "even with kernel sampling overhead measured use stays below 50 percent");
        Require(totals.spinWallUs > 0 && totals.iterations > 0, "telemetry includes real spin counts and active wall time");
        if (share == 100 && !preemptUs)
            Require(totals.cpuUs * 100 > totals.wallUs * 40, "unrestricted model exercises the full guarded quota");
        if (share == 20)
            Require(totals.cpuUs * 100 < totals.wallUs * 12, "restricted core is not misreported as a free core");
    }
    void Failed(const char* op, int result) { operation = op; error = result; }
};
}

int main() {
    {
        Kernel k; Run(k);
        Require(k.error == -999 && k.starts == 1 && k.reports == 2, "steady probe runs and reports every ten seconds");
        Require(k.sleeps >= 200 && k.spins > 30000, "quota sleeps every window while admitting short chunks");
        // No adjacent 100 ms interval can contain a 50 ms continuous spin burst.
        for (size_t i = 200; i < k.chunks.size(); ++i)
            Require(k.chunks[i] - k.chunks[i - 200] > kWindowUs, "no 200-chunk (50 ms) burst fits within a quota window");
    }
    {
        Kernel k; k.share = 20; Run(k);
        Require(k.reports == 2 && k.error == -999, "throttled-core telemetry remains useful");
    }
    {
        Kernel k; k.preemptUs = 1000; Run(k);
        Require(k.reports == 2 && k.error == -999, "preemption charges active wall time rather than borrowing future quota");
    }
    for (const char* scenario : {"pin", "mask", "read", "later-read", "clock", "delay", "spin"}) {
        Kernel k;
        if (!std::strcmp(scenario, "pin")) k.pin = -4;
        if (!std::strcmp(scenario, "mask")) k.mask = 0x40000;
        if (!std::strcmp(scenario, "read")) k.failRead = 1;
        if (!std::strcmp(scenario, "later-read")) k.failRead = 3;
        if (!std::strcmp(scenario, "clock")) k.backward = 3;
        if (!std::strcmp(scenario, "delay")) k.delayFail = 1;
        if (!std::strcmp(scenario, "spin")) k.spinFail = 1;
        Run(k);
        Require(k.error != 0 && k.error != -999, "failed kernel operation permanently stops the helper");
        if (k.pin || k.mask != kSystemMask || k.failRead == 1)
            Require(k.spins == 0 && k.starts == 0, "no calibration or game-core fallback before affinity and usage validation");
        Require(k.wall < k.stop, "failure does not run until the measurement timeout");
    }
    std::puts("PASS: core3 probe: steady, restricted, preempted, quota bursts and seven fail-closed paths (simulated kernel; not hardware share)");
}
