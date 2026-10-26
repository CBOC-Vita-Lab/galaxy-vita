// Included after the Vita thread/process headers and port log declarations.
#pragma once
#include "vgx_common.h"
#include "vgx_core3_probe.h"

namespace vgx::core3probe {
template <typename... Args> inline void Log(const char* format, Args... args) {
    if (&port_log_summary) port_log_summary(format, args...);
    else port_log(format, args...);
}

struct VitaKernel {
    const SceUID uid = sceKernelGetThreadId();
    int PinSystem() { return sceKernelChangeThreadCpuAffinityMask(uid, kSystemMask); }
    int Read(Sample& sample) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        const int result = sceKernelGetThreadInfo(uid, &info);
        if (result < 0) return result;
        sample = {sceKernelGetProcessTimeWide(), info.runClocks, static_cast<int>(info.currentCpuAffinityMask)};
        return 0;
    }
    int Spin(uint32_t us, uint64_t& iterations) {
        const uint64_t begin = sceKernelGetProcessTimeWide();
        uint64_t previous = begin;
        unsigned stagnant = 0;
        for (;;) {
#if defined(__arm__)
            uint32_t n = 64;
            __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
#else
            for (unsigned n = 0; n < 64; ++n) __asm__ volatile("" : : : "memory");
#endif
            iterations += 64;
            const uint64_t now = sceKernelGetProcessTimeWide();
            if (now < previous || (now == previous && ++stagnant > 4096)) return -1;
            if (now != previous) stagnant = 0;
            if (now - begin >= us) return 0;
            previous = now;
        }
    }
    int Sleep(uint32_t us) { return sceKernelDelayThread(static_cast<SceUInt>(us)); }
    void Started() { Log("[vgx-vita] core3 probe: mask=0x%X, quota <=45 ms/100 ms, spin chunks <=250 us, measured thread CPU/wall; no user-core calibration", kSystemMask); }
    void Report(const Totals& totals) {
        Log("[vgx-vita] core3 probe: cpu_us=%llu wall_us=%llu share=%.2f%% spin_wall_us=%llu iterations=%llu (guarded helper share, not free-core capacity)",
            static_cast<unsigned long long>(totals.cpuUs), static_cast<unsigned long long>(totals.wallUs),
            100.0 * static_cast<double>(totals.cpuUs) / static_cast<double>(totals.wallUs),
            static_cast<unsigned long long>(totals.spinWallUs), static_cast<unsigned long long>(totals.iterations));
    }
    void Failed(const char* operation, int result) { Log("[vgx-vita] core3 probe stopped: %s rc=0x%X; no fallback on a user core", operation, static_cast<unsigned>(result)); }
};

inline int Main(SceSize, void*) {
    VitaKernel kernel;
    Run(kernel);
    return sceKernelExitDeleteThread(0);
}

inline void Start() {
    static const bool attempted = [] {
        // GXV_CORE3_PROBE=1 enables a quota-controlled system-core availability probe; default 0, never a percent/duty setting.
        if (EnvInt("GXV_CORE3_PROBE", 0) != 1) return false;
        const SceUID uid = sceKernelCreateThread("smg_core3_probe", &Main, 0x10000100, 64 * 1024, 0, kSystemMask, nullptr);
        if (uid < 0) { Log("[vgx-vita] core3 probe stopped: create rc=0x%X; no fallback on a user core", static_cast<unsigned>(uid)); return false; }
        const int result = sceKernelStartThread(uid, 0, nullptr);
        if (result < 0) {
            sceKernelDeleteThread(uid);
            Log("[vgx-vita] core3 probe stopped: start rc=0x%X", static_cast<unsigned>(result));
            return false;
        }
        return true;
    }();
    (void)attempted;
}
} // namespace vgx::core3probe
