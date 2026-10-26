// Exercise the production decision path without adding a test API to the backend.
#include "vgx_common.h"
#include <cerrno>
#include <cfenv>

namespace vgx {
static unsigned ScanReads = 0, MinReads = 0, WorkloadReads = 0;
static long CountedEnvInt(const char* name, long fallback) {
    if (!std::strcmp(name, "GXV_FXSCANFAST")) ++ScanReads;
    if (!std::strcmp(name, "GXV_FXHALF_MIN")) ++MinReads;
    if (!std::strcmp(name, "GXV_WORKLOAD_PROBE")) ++WorkloadReads;
    return EnvInt(name, fallback);
}
bool LogEnabled() { return false; }
}
#define EnvInt CountedEnvInt
#include "vgx_workload.h"
namespace vgx::workload { static void FaultCandidateRun(uint32_t vertices); }
#define CandidateRun FaultCandidateRun
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#undef CandidateRun
#undef EnvInt

static bool InjectMask = false;
namespace vgx::workload {
static void FaultCandidateRun(uint32_t vertices) {
    CandidateRun(vertices);
    // Simulate a future slow-scan change; the oracle must retain its actual result, not the proposed empty mask.
    if (InjectMask) backend::g_fxCmd.assign(1, 1);
}
}

extern "C" int64_t port_host_time_ns() { return 1; }
extern "C" int64_t port_thread_cpu_us() { return 1; }
extern "C" void port_log_summary(const char*, ...) {}

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
vgx::CmdBuf Candidates() {
    using namespace vgx;
    CmdBuf b;
    DrawRec a;
    a.rs.blend = 1; a.rs.blendDst = BF_ONE;
    a.idxCount = 3; a.vtxCount = 200;
    b.draws.push_back(a);
    a.vtxCount = 300;
    b.draws.push_back(a);
    a.rs.blend = 0;
    b.draws.push_back(a);
    b.cmds = {{CMD_DRAW, 0}, {CMD_BUF_FREE, 0}, {CMD_DRAW, 1}, {CMD_DRAW, 2}, {CMD_DRAW, 0}};
    return b;
}
void Decide(const vgx::CmdBuf& b, unsigned candidates) {
    using namespace vgx::backend;
    C = Counts{};
    g_fxCmd.assign(8, 1);
    FxDecide(b);
    Check(C.fxCandidates == candidates, "real scan dispatch preserves observed counts or bypasses unobserved candidates");
    Check(g_fxCmd.empty(), "inactive FX clears a previous frame's reduced-draw mask");
    Check(C.glCalls == 0, "inactive decision adds no GL operation");
}
}

int main(int argc, char** argv) {
    Check(argc == 3 || argc == 4, "expected fast and workload expectations");
    const bool fast = std::atoi(argv[1]) != 0, observed = std::atoi(argv[2]) != 0;
    const bool empty = argc == 4 && !std::strcmp(argv[3], "empty");
    using namespace vgx;
    using namespace vgx::backend;
    // Force capability rejection for active-FX gate cases without making GL calls.
    g_fxDepthFailed = true;
    CmdBuf b = Candidates();
    if (argc == 4 && !std::strcmp(argv[3], "oracle")) {
        InjectMask = true;
        for (unsigned i = 0; i < 5; ++i) {
            C = Counts{};
            FxDecide(b);
            Check(g_fxCmd == std::vector<uint8_t>{1}, "oracle mismatch retains the actual slow mask");
            Check(C.fxCandidates == 3 && C.glCalls == 0, "oracle scans once and adds no GPU operation");
        }
        std::puts("PASS: injected oracle mismatch keeps slow result on five calls");
        return 0;
    }
    if (empty) for (auto& d : b.draws) d.rs.blend = 0;
    if (argc == 4 && !std::strcmp(argv[3], "initialized")) {
        Check(workload::Enabled(), "observer initializes enabled before environment changes");
        setenv("GXV_WORKLOAD_PROBE", "0", 1);
    }
    const char* raw = std::getenv("GXV_WORKLOAD_PROBE");
    const bool overflow = raw && std::strlen(raw) > 30;
    errno = EDOM;
    std::fesetround(FE_DOWNWARD);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    Decide(b, fast || empty ? 0 : 3);
    Check(errno == (overflow && !empty ? ERANGE : EDOM), "new control preserves errno and legacy workload overflow remains conditional");
    Check(std::fegetround() == FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
          "no-pass decision preserves FP rounding and exception flags");
    Check(ScanReads == 1 && MinReads == 1, "new control and existing minimum initialize once");
    if (empty && overflow) Check(WorkloadReads == 0, "zero eligible runs do not introduce a workload parse or its overflow");
    Check(workload::Pictures.candidateRuns == (observed ? 2u : 0u) &&
          workload::Pictures.candidateVerts == (observed ? 700u : 0u) &&
          workload::Pictures.runs400 == (observed ? 1u : 0u) && workload::Pictures.runs800 == 0,
          "workload histogram retains two runs of 500 and 200 vertices");
    setenv("GXV_FXSCANFAST", fast ? "0" : "1", 1);
    setenv("GXV_FXHALF_MIN", "1", 1);
    Decide(b, fast || empty ? 0 : 3);
    Check(ScanReads == 1 && MinReads == 1, "environment changes do not change initialized decisions");
    for (bool* flag : {&g_verify.read, &g_verify.shadow, &g_verify.refRun}) {
        *flag = true;
        Decide(b, empty ? 0 : 3);
        *flag = false;
    }
    b.passes.emplace_back(); // Even unused pass records conservatively prohibit bypass.
    Decide(b, empty ? 0 : 3);
    b.passes.clear();
    // A real pass must still update eligibility, not just the no-pass decision.
    PassCmd p;
    p.rect = {0, 0, 32, 32};
    b.passes.push_back(p);
    b.cmds = {{CMD_DRAW, 0}, {CMD_PASS_BEGIN, 0}, {CMD_DRAW, 1}, {CMD_COPY_TEX, 0}, {CMD_DRAW, 0}};
    b.copyTex.emplace_back();
    g_rtW = g_efbW = 640; g_rtH = g_efbH = 456;
    Decide(b, empty ? 0 : 2);
    Check(ScanReads == 1 && MinReads == 1, "gated frames do not re-read options");
    std::printf("PASS: FX scan dispatch fast=%d observed=%d diagnostics=%d; mask, gates, counters, errno, FP\n",
                fast, observed, kBackendDiagnostics);
}
