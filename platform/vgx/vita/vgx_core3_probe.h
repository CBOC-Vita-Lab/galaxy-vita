// Scheduling-only probe policy. Kernel boundaries are supplied by the Vita host or a simulated-clock test.
#pragma once
#include <cstdint>

namespace vgx::core3probe {
constexpr int kSystemMask = 0x80000; // SCE_KERNEL_CPU_MASK_SYSTEM
constexpr uint32_t kWindowUs = 100000, kBudgetUs = 45000, kChunkUs = 250;
constexpr uint32_t kControlReserveUs = 250; // sampling/control work remains inside the guarded quota
constexpr uint64_t kReportUs = 10000000;
struct Sample { uint64_t wallUs, cpuUs; int mask; };
struct Totals { uint64_t wallUs, cpuUs, spinWallUs, iterations; };

template <class Kernel> void Run(Kernel& kernel) {
    const int pin = kernel.PinSystem();
    if (pin < 0) { kernel.Failed("affinity", pin); return; }
    Sample last{};
    bool haveSample = false;
    const auto read = [&](Sample& sample) {
        const int result = kernel.Read(sample);
        if (result < 0) { kernel.Failed("thread sample", result); return false; }
        if (sample.mask != kSystemMask) { kernel.Failed("affinity readback", -1); return false; }
        if (haveSample && (sample.wallUs < last.wallUs || sample.cpuUs < last.cpuUs)) {
            kernel.Failed("non-monotonic clocks", -1); return false;
        }
        last = sample;
        haveSample = true;
        return true;
    };
    Sample begin{};
    if (!read(begin)) return;
    kernel.Started();
    uint64_t windowWall = begin.wallUs, windowCpu = begin.cpuUs, activeWall = 0;
    uint64_t reportWall = begin.wallUs, reportCpu = begin.cpuUs, spinWall = 0, iterations = 0;
    for (;;) {
        Sample now{};
        if (!read(now)) return;
        if (now.wallUs - reportWall >= kReportUs) {
            kernel.Report({now.wallUs - reportWall, now.cpuUs - reportCpu, spinWall, iterations});
            reportWall = now.wallUs; reportCpu = now.cpuUs; spinWall = iterations = 0;
            continue; // sample again: reporting cost is charged too
        }
        if (now.wallUs - windowWall >= kWindowUs) {
            // No accumulated credit or catch-up burst after a delayed/preempted window.
            windowWall = now.wallUs; windowCpu = now.cpuUs; activeWall = 0;
        }
        const uint64_t elapsed = now.wallUs - windowWall, cpu = now.cpuUs - windowCpu;
        const uint64_t charged = cpu > activeWall ? cpu : activeWall;
        if (charged + kChunkUs + kControlReserveUs > kBudgetUs || elapsed + kChunkUs >= kWindowUs) {
            const int result = kernel.Sleep(static_cast<uint32_t>(kWindowUs - elapsed));
            if (result < 0) { kernel.Failed("quota sleep", result); return; }
            continue;
        }
        uint64_t count = 0;
        const int result = kernel.Spin(kChunkUs, count);
        if (result < 0) { kernel.Failed("spin clock", result); return; }
        Sample end{};
        if (!read(end)) return;
        const uint64_t wall = end.wallUs - now.wallUs;
        activeWall += wall;
        spinWall += wall;
        iterations += count;
    }
}
} // namespace vgx::core3probe
