// Kernel/API boundary fixture for the actual Vita adapter; scheduling policy is tested separately.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using SceSize = uint32_t;
using SceUID = int;
using SceUInt = unsigned;
struct SceKernelThreadInfo { uint32_t size; uint64_t runClocks; int currentCpuAffinityMask; };
static int creates, starts, deletes, summaries, diagnosticLogs, affinityCalls, infoCalls;
static int createResult = 11, startResult, pinResult, infoResult, readbackMask = 0x80000;
static int createdMask;
static uint64_t clockUs = 1000;
static bool stuckClock;
using Entry = int (*)(SceSize, void*);
static Entry entry;
static int sceKernelGetThreadId() { return 11; }
static int sceKernelChangeThreadCpuAffinityMask(int id, int mask) {
    ++affinityCalls;
    if (id != 11 || mask != 0x80000) std::abort();
    return pinResult;
}
static int sceKernelGetThreadInfo(int, SceKernelThreadInfo* info) {
    ++infoCalls;
    if (info->size != sizeof(*info)) std::abort();
    info->runClocks = clockUs;
    info->currentCpuAffinityMask = readbackMask;
    return infoResult;
}
static uint64_t sceKernelGetProcessTimeWide() { return stuckClock ? clockUs : ++clockUs; }
static int sceKernelDelayThread(unsigned) { return 0; }
static int sceKernelCreateThread(const char*, Entry callback, int, unsigned, int, int mask, void*) {
    ++creates; entry = callback; createdMask = mask; return createResult;
}
static int sceKernelStartThread(int, unsigned, void*) { ++starts; return startResult; }
static int sceKernelDeleteThread(int) { ++deletes; return 0; }
static int sceKernelExitDeleteThread(int) { ++deletes; return 0; }
extern "C" void port_log_summary(const char*, ...) { ++summaries; }
extern "C" void port_log(const char*, ...) { ++diagnosticLogs; }
#include "vgx_core3_probe_vita.h"

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
int main(int argc, char** argv) {
    Require(argc == 2, "one adapter scenario requested");
    const char* scenario = argv[1];
    setenv("GXV_CORE3_PROBE", "1", 1);
    if (!std::strcmp(scenario, "off")) unsetenv("GXV_CORE3_PROBE");
    if (!std::strcmp(scenario, "invalid")) setenv("GXV_CORE3_PROBE", "100", 1);
    if (!std::strcmp(scenario, "create-fail")) createResult = -4;
    if (!std::strcmp(scenario, "start-fail")) startResult = -5;
    vgx::core3probe::Start();
    setenv("GXV_CORE3_PROBE", "1", 1);
    vgx::core3probe::Start();
    if (!std::strcmp(scenario, "off") || !std::strcmp(scenario, "invalid")) {
        Require(creates == 0 && summaries == 0, "default-off/invalid settings create no helper, read once");
    } else {
        Require(creates == 1 && createdMask == 0x80000, "helper is created directly on system core, exactly once");
        if (createResult < 0) Require(starts == 0 && summaries == 1, "creation refusal fails closed to basic log");
        else if (startResult < 0) Require(deletes == 1 && summaries == 1, "failed start deletes unstarted thread and logs once");
        else Require(starts == 1 && entry != nullptr, "enabled adapter starts one real probe entry");
    }
    if (!std::strcmp(scenario, "pin-fail")) {
        pinResult = -6; entry(0, nullptr);
        Require(affinityCalls == 1 && infoCalls == 0 && deletes == 1 && summaries == 1, "refused affinity deletes helper before any spin");
    }
    if (!std::strcmp(scenario, "info-fail")) {
        infoResult = -7; entry(0, nullptr);
        Require(infoCalls == 1 && deletes == 1 && summaries == 1, "usage failure deletes helper before any spin");
    }
    if (!std::strcmp(scenario, "wrong-mask")) {
        readbackMask = 0x40000; entry(0, nullptr);
        Require(infoCalls == 1 && deletes == 1 && summaries == 1, "successful affinity call with wrong readback cannot run on user core");
    }
    if (!std::strcmp(scenario, "spin-clock")) {
        vgx::core3probe::VitaKernel kernel;
        stuckClock = true;
        uint64_t iterations = 0;
        Require(kernel.Spin(250, iterations) < 0 && iterations <= 64 * 4097, "stuck spin clock fails closed after a bounded instruction count");
    }
    Require(diagnosticLogs == 0, "probe reporting uses basic log even when runtime diagnostics are off");
    std::printf("PASS: real Vita adapter API fixture: %s\n", scenario);
}
