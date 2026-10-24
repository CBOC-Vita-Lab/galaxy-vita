// Exercise the actual renderer clock and warm scheduler against controlled OS clocks.
#include <psp2/kernel/processmgr.h>
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <array>
#include <cstdio>
#include <string>

namespace {
std::array<uint64_t, 8> ticks{};
unsigned tickCount = 0, processReads = 0, wallReads = 0;
int64_t wallNs = 9000000000000ll, wallStepNs = 0;
void Require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
void SetTicks(std::initializer_list<uint64_t> values) {
    Require(values.size() && values.size() <= ticks.size(), "bounded synthetic process clock");
    std::copy(values.begin(), values.end(), ticks.begin());
    tickCount = static_cast<unsigned>(values.size());
    processReads = wallReads = 0;
}
void Near(double actual, double expected, const char* what) {
    Require(std::abs(actual - expected) < 0.00001, what);
}
}

namespace vgx { bool LogEnabled() { return false; } }

extern "C" SceUInt64 sceKernelGetProcessTimeWide(void) {
    return ticks[std::min(processReads++, tickCount - 1)];
}

// Interpose only the host library boundary, not renderer timing or queue logic.
std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
    ++wallReads;
    const int64_t value = wallNs;
    wallNs += wallStepNs;
    return time_point(std::chrono::duration_cast<duration>(std::chrono::nanoseconds(value)));
}

int main(int argc, char** argv) {
    Require(argc == 2, "supply clock scenario");
    std::chrono::steady_clock::now();
    Require(wallReads == 1, "host clock interposition is active in this executable");
    using namespace vgx::backend;
    const std::string mode = argv[1];
    if (mode == "clock") {
        SetTicks({123456789});
#ifdef __vita__
        Near(NowMs(), 123456.789, "native shader timing uses elapsed process microseconds");
        wallNs = -3600000000000ll;
        Near(DNow(), 123456.789, "draw profiling ignores a backward RTC jump");
        wallNs = 3600000000000ll;
        Near(Now(), 123456.789, "frame profiling ignores a forward RTC jump");
        Require(processReads == 3 && wallReads == 0, "native renderer bypasses the host wall-clock library");
#else
        Near(NowMs(), 9000000.0, "portable shader timing retains its steady-clock epoch");
        wallNs = 9010000000000ll;
        Near(DNow(), 9010000.0, "portable draw profiling retains steady-clock milliseconds");
        Near(Now(), 9010000.0, "portable frame profiling retains steady-clock milliseconds");
        Require(wallReads == 3 && processReads == 0, "portable renderer does not depend on the Vita SDK clock");
#endif
    } else if (mode == "wrap") {
        SetTicks({4294967000ull, 4294968000ull});
        const double before = NowMs(), after = NowMs();
        Near(before, 4294967.0, "elapsed clock retains high bits before the 32-bit microsecond boundary");
        Near(after, 4294968.0, "elapsed clock retains high bits after the 32-bit microsecond boundary");
        Near(after - before, 1.0, "renderer timing does not wrap after about 71 minutes");
    } else {
        Require(mode == "budget-back" || mode == "budget-forward" || mode == "oversized" || mode == "zero" ||
                mode == "boundary" || mode == "near-boundary" || mode == "budget-wrap" || mode == "large-epoch",
                "known warm-budget scenario");
        Require(g_warmBackoffOn && g_warmBackoffMs == 60.0, "controlled default warm-up backoff policy");
        // Missing shader IDs retire real queue entries without needing a GPU context.
        for (unsigned i = 0; i < 5; ++i) g_warm.push_back({vgx::CMD_PROGRAM, 0x10001u});
        g_warmLeft = g_warmAll = 10;
        SetWarmBudget(mode == "zero" ? 0.0 : 10.0);
        wallStepNs = mode == "budget-forward" ? 1000000000000ll : -1000000000000ll;
        if (mode == "oversized") SetTicks({1000000,1080000,1080000});
        else if (mode == "boundary") SetTicks({1000000,1010000,1010000});
        else if (mode == "near-boundary") SetTicks({1000000,1009999,1014000,1014000});
        else if (mode == "budget-wrap") SetTicks({4294966000ull,4294973000ull,4294980000ull,4294980000ull});
        else if (mode == "large-epoch") SetTicks({1000000000000000ull,1000000000007000ull,1000000000014000ull,1000000000014000ull});
        else SetTicks({1000000,1007000,1014000,1014000});
        RunWarm();
        if (mode == "zero") {
            Require(g_warm.size() == 5 && WarmPending() == 10 && g_warmSlices == 0,
                    "zero budget leaves every deferred warm-up item pending");
            Require(processReads == 0 && wallReads == 0, "disabled warm-up does not read a clock");
        } else if (mode == "oversized") {
            Require(g_warm.size() == 4 && WarmPending() == 8, "one indivisible slow item ends the slice");
            Near(g_warmWorstMs, 80.0, "oversized item reports actual elapsed time");
            Require(g_warmBackoffLeft == 8, "oversized item retains the conservative warm-up backoff");
            Require(wallReads == 0, "slow-item budget never reads the RTC library");
        } else if (mode == "boundary") {
            Require(g_warm.size() == 4 && WarmPending() == 8, "budget equality ends the slice after exactly one item");
            Near(g_warmTotalMs, 10.0, "exact budget-boundary accounting remains unchanged");
            Require(wallReads == 0, "budget-boundary timing does not read the RTC library");
        } else {
            Require(g_warm.size() == 3 && WarmPending() == 6, "10 ms budget retires two 7 ms items despite RTC jumps");
            Near(g_warmTotalMs, 14.0, "warm-up accounting uses actual elapsed time");
            Near(g_warmWorstMs, 14.0, "warm-up worst-slice timing uses the same monotonic source");
            Require(g_warmSlices == 1 && g_warmBackoffLeft == 0, "normal slice retains accounting without false backoff");
            // The missing-shader path has no nested work clocks; only the four budget reads occur.
            Require(processReads == 4 && wallReads == 0, "warm-up budget uses only direct elapsed-clock reads");
        }
    }
    std::printf("PASS: renderer clock %s native=%d diagnostics=%d\n", argv[1],
#ifdef __vita__
                1,
#else
                0,
#endif
                kBackendDiagnostics);
}
