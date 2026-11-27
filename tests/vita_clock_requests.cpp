#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace {
struct Call { char device; int frequency; };
std::vector<Call> calls;
std::vector<int> armResults;
unsigned armAt, reads[4], measured, failures;
int busResult, gpuResult, xbarResult, reportedCpu;
std::vector<std::string> basicLogs;

void Check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "ok" : "FAIL", message);
    failures += !ok;
}
void Reset(std::vector<int> arm, int cpu = 500) {
    calls.clear(); basicLogs.clear(); armResults = std::move(arm); armAt = measured = 0;
    std::memset(reads, 0, sizeof(reads));
    busResult = gpuResult = xbarResult = 0; reportedCpu = cpu;
    unsetenv("GXV_VITA_CLOCKS"); unsetenv("GXV_VITA_XBAR");
}
bool ArmCalls(std::initializer_list<int> want) {
    std::vector<int> actual;
    for (const auto& c : calls) if (c.device == 'A') actual.push_back(c.frequency);
    return actual == std::vector<int>(want);
}
bool OtherCalls(int xbar = 166) {
    std::vector<Call> actual;
    for (const auto& c : calls) if (c.device != 'A') actual.push_back(c);
    return actual.size() == 3 && actual[0].device == 'B' && actual[0].frequency == 222 &&
        actual[1].device == 'G' && actual[1].frequency == 222 && actual[2].device == 'X' && actual[2].frequency == xbar;
}
bool Logged(const char* text) {
    for (const auto& line : basicLogs) if (line.find(text) != std::string::npos) return true;
    return false;
}
}

int scePowerSetArmClockFrequency(int value) {
    calls.push_back({'A', value});
    if (armAt >= armResults.size()) { Check(false, "no unexpected/repeated ARM setter"); return -99; }
    return armResults[armAt++];
}
int scePowerSetBusClockFrequency(int value) { calls.push_back({'B', value}); return busResult; }
int scePowerSetGpuClockFrequency(int value) { calls.push_back({'G', value}); return gpuResult; }
int scePowerSetGpuXbarClockFrequency(int value) { calls.push_back({'X', value}); return xbarResult; }
int scePowerGetArmClockFrequency() { ++reads[0]; return reportedCpu; }
int scePowerGetBusClockFrequency() { ++reads[1]; return 222; }
int scePowerGetGpuClockFrequency() { ++reads[2]; return 222; }
int scePowerGetGpuXbarClockFrequency() { ++reads[3]; return 166; }
unsigned MeasureArmMHz() { ++measured; return 497; }
void port_log(const char*, ...) {}
void port_log_summary(const char* format, ...) {
    char text[2048]; va_list ap;
    va_start(ap, format); std::vsnprintf(text, sizeof(text), format, ap); va_end(ap);
    basicLogs.emplace_back(text);
}
void RequestActualClocks() {
#include "vita_clock_block.inc"
}
void CheckReads() {
    Check(reads[0] == 1 + PORT_RUNTIME_DIAGNOSTICS && reads[1] == 1 && reads[2] == 1 && reads[3] == 1,
          "basic readbacks occur once; only the existing diagnostics line adds its CPU read");
    Check(measured == PORT_RUNTIME_DIAGNOSTICS, "the existing calibration stays diagnostics-only");
}

int main() {
    for (int rc : {0, 7}) {
        Reset({rc}); RequestActualClocks();
        Check(ArmCalls({500}) && armAt == 1, "successful nonnegative request500 has no fallback or retry");
        Check(OtherCalls(), "GPU222/bus222/xbar166 requests and ordering stay unchanged");
        Check(Logged("CPU request=500") && Logged(rc ? "arm_rc=0x00000007" : "arm_rc=0x00000000") && Logged("reported CPU=500"),
              "basic log reports the requested value, exact ARM rc and API readback");
        Check(Logged("fallback_tried=0") && Logged("plugin-dependent") && Logged("not calibrated"),
              "basic log distinguishes no fallback and plugin-dependent uncalibrated reports");
        CheckReads();
    }
    Reset({0}, 444); RequestActualClocks();
    Check(ArmCalls({500}), "reported444 after success does not trigger a fallback or override a plugin lock");
    Check(Logged("reported CPU=444") && !Logged("effective CPU=500"), "reported444 is preserved without a false effective500 claim");
    CheckReads();

    Reset({-1, 0}, 444); RequestActualClocks();
    Check(ArmCalls({500, 444}) && armAt == 2 && OtherCalls(), "negative500 request falls back to444 exactly once, other clocks still requested");
    Check(Logged("arm_rc=0xFFFFFFFF") && Logged("fallback_tried=1") && Logged("fallback_rc=0x00000000"),
          "basic log keeps original failure and successful fallback separately");
    CheckReads();

    Reset({-1, -5}, 444); busResult = -2; gpuResult = -3; xbarResult = -4; RequestActualClocks();
    Check(ArmCalls({500, 444}) && armAt == 2 && OtherCalls(), "failed fallback is nonfatal and is not repeated");
    Check(Logged("fallback_rc=0xFFFFFFFB") && Logged("bus_rc=0xFFFFFFFE") && Logged("gpu_rc=0xFFFFFFFD") && Logged("xbar_rc=0xFFFFFFFC"),
          "all failed power-call return codes remain observable");
    CheckReads();

    Reset({0}); busResult = 2; gpuResult = 3; xbarResult = 4; RequestActualClocks();
    Check(Logged("bus_rc=0x00000002") && Logged("gpu_rc=0x00000003") && Logged("xbar_rc=0x00000004"),
          "nonnegative nonzero power-call return codes are not discarded");

    Reset({}); setenv("GXV_VITA_CLOCKS", "0", 1); RequestActualClocks();
    Check(calls.empty(), "existing clocks0 skips every setter, including fallback");
    Check(Logged("requests disabled") && Logged("reported CPU=500"), "disabled requests are not mislabeled as successful requests");
    CheckReads();

    Reset({0}); setenv("GXV_VITA_XBAR", "111", 1); RequestActualClocks();
    Check(ArmCalls({500}) && OtherCalls(111), "existing crossbar override is retained without altering other defaults");
    Check(Logged("xbar request=111") && Logged("reported xbar=166"), "crossbar request and API report remain distinct");
    CheckReads();
    unsetenv("GXV_VITA_CLOCKS"); unsetenv("GXV_VITA_XBAR");
    std::printf("%s (%u failed), diagnostics=%d\n", failures ? "FAILED" : "all checks passed", failures, PORT_RUNTIME_DIAGNOSTICS);
    return failures ? 1 : 0;
}
