#include "port/frame_pace.h"
#include "port/hot_knob.h"  // (the extracted perf.cpp section reads its switches with PORT_HOT_KNOB)
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int64_t WallNs = 1000000000;
static unsigned ClockReads, TraceReads, ErrnoAccesses, DrawCallbacks;
static std::vector<std::string> Events;
static std::vector<std::string> Summaries;
static unsigned ExpectedMax;
static bool ExpectedTrace;
static int SummaryErrno = ERANGE;
static int VerboseErrno;
[[noreturn]] static void Fail(const char* why) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    for (const auto& event : Events) std::fprintf(stderr, "  %s\n", event.c_str());
    std::exit(1);
}
static void Require(bool yes, const char* why) { if (!yes) Fail(why); }
static int& TraceErrno() { ++ErrnoAccesses; return errno; }
static void SetErrno(int value) { errno = value; }
static int GetErrno() { return errno; }
static const char* TraceGetenv(const char* name) {
    if (std::strcmp(name, "GXV_SLIP_TRACE") == 0) ++TraceReads;
    return std::getenv(name);
}
static std::string Format(const char* format, va_list args) {
    char line[2048];
    std::vsnprintf(line, sizeof(line), format, args);
    return line;
}
static void port_log_summary(const char* format, ...) {
    va_list args; va_start(args, format);
    Summaries.push_back(Format(format, args)); va_end(args);
    Events.push_back("summary:" + Summaries.back());
    SetErrno(SummaryErrno);
}
static void port_log(const char* format, ...) {
    va_list args; va_start(args, format);
    Events.push_back("verbose:" + Format(format, args)); va_end(args);
    if (VerboseErrno) SetErrno(VerboseErrno);
}
static int64_t port_host_time_ns() { ++ClockReads; return WallNs; }
static int port_handheld() { return 1; }
static unsigned port_vgx_backend_pending() { return 0; }
static int port_vgx_pace_turn(int) { return -1; }
static void port_vgx_present_turn(int) {}
static int port_autoq_stages(uint32_t*, uint32_t*, int*) { return 0; }  // (GXV_STEADY30's stage hint: none here)
static int port_screen_quiet() { return 0; }
namespace vgx::workload {
static void AnimDrawDecision(int decision) {
    Require(decision >= 0 && decision <= 2, "original draw callback receives actual pacing decision");
    ++DrawCallbacks;
}
}
enum class TurnKind { DrawSelected, RequiredDraw, ForcedSkip, LateSkip, PacedSkip, BacklogSkip };
static struct { TurnKind kind; bool bothSkipReasons; } sTurn;
static std::atomic<uint32_t> sSkippedTurns{0};
#include "slip_trace_methods.inc"

static void CheckConfig() {
    SetErrno(E2BIG);
    Require(SlipMax() == ExpectedMax, "original enable/default/max clamp semantics remain unchanged");
    Require(GetErrno() == E2BIG, "startup summary preserves caller errno despite logger mutation");
    Require(port::pace::GlobalSlip().MaxPercent() == ExpectedMax, "actual Slip receives effective configuration");
    Require(Summaries.size() == (ExpectedTrace ? 1u : 0u), "trace=1 emits exactly one startup config, including disabled slip");
    if (ExpectedTrace) {
        const std::string expected = "pace: slip-config enabled=" + std::to_string(ExpectedMax != 0) +
            " max_percent=" + std::to_string(ExpectedMax) + " trace=1";
        Require(Summaries.front() == expected, "startup config attests effective enabled flag and cap");
    }
    const unsigned reads = TraceReads;
    setenv("GXV_SLIP_TRACE", ExpectedTrace ? "0" : "1", 1);
    setenv("GXV_SLIP", ExpectedMax ? "0" : "1", 1);
    setenv("GXV_SLIP_MAX", "7", 1);
    for (unsigned i = 0; i < 4; ++i) Require(SlipMax() == ExpectedMax, "first-use configuration remains frozen");
    Require(port::pace::GlobalSlip().MaxPercent() == ExpectedMax &&
        port::pace::GlobalSlip().Enabled() == (ExpectedMax != 0), "later enable/cap environment changes cannot reconfigure actual Slip");
    Require(Summaries.size() == (ExpectedTrace ? 1u : 0u), "later environment changes cannot duplicate startup config");
    Require(TraceReads == reads, "trace environment is initialized once, not once per call");
#if defined(__vita__) && PORT_LOGGING
    Require(reads == 1, "logging Vita reads trace flag once inside first-use configuration");
#else
    Require(reads == 0, "non-Vita and logging-off never read trace control");
#endif
    Require(ErrnoAccesses == (ExpectedTrace ? 2u : 0u), "trace off never touches errno at startup");
    Require(ClockReads == 0, "configuration adds no clock calls");
}

static void CheckTransition(unsigned previousChanges) {
    const auto& slip = port::pace::GlobalSlip();
    if (slip.Changes() == previousChanges) return;
    Require(slip.Changes() == previousChanges + 1, "test samples one existing transition boundary at a time");
    if (!ExpectedTrace) return;
    Require(Summaries.size() == slip.Changes() + 1, "one summary per transition, with no duplicates");
    unsigned engaged, changes, max;
    double pair, speed;
    unsigned long long lag;
    int consumed = 0;
    Require(std::sscanf(Summaries.back().c_str(),
        "pace: slip-state engaged=%u changes=%u pair_us=%lf speed_percent=%lf max_percent=%u lag_us=%llu%n",
        &engaged, &changes, &pair, &speed, &max, &lag, &consumed) == 6 &&
        consumed == static_cast<int>(Summaries.back().size()), "state summary provides all six bounded fields");
    Require(engaged == unsigned(slip.Engaged()) && changes == slip.Changes() && max == slip.MaxPercent() &&
        lag == slip.LagUs() && pair == slip.PairUs(), "summary samples existing actual Slip state without modifying it");
    Require(std::abs(speed - slip.SpeedPercent()) <= 0.051, "estimated speed is logged to one decimal place");
    Require(Events.size() >= 2 && Events[Events.size() - 2].find("verbose:pace: slip ") == 0 &&
        Events.back().find("summary:pace: slip-state ") == 0, "existing verbose state log remains first, new basic summary second");
}

static void RunTurns(unsigned count, int64_t gameUs, int64_t drawUs, bool allowed = true) {
    const int64_t vbl = 16683000;
    static int64_t posted = WallNs / vbl, next = posted, message = 0;
    static bool full = false;
    auto post = [&] {
        for (const int64_t end = WallNs / vbl; posted < end;) {
            ++posted;
            if (!full) { full = true; message = posted; }
        }
    };
    for (unsigned i = 0; i < count; ++i) {
        auto& slip = port::pace::GlobalSlip();
        const unsigned previousChanges = slip.Changes();
        const unsigned clocks = ClockReads;
        SetErrno(E2BIG);
        const int decision = port_frameskip_turn(allowed);
        WallNs += (gameUs + (decision == 0 ? drawUs : 0)) * 1000;
        SlipWorkDone();
        Require(GetErrno() == (slip.Changes() != previousChanges && VerboseErrno ? VerboseErrno : E2BIG),
            "summary preserves errno after original verbose logger, or unchanged caller errno");
        Require(ClockReads - clocks == (ExpectedMax ? 3u : 1u), "only existing decision/begin/end clocks run");
        CheckTransition(previousChanges);
        std::printf("sample %lld %d %u %u %.1f %.1f %llu %u\n", static_cast<long long>(WallNs), decision,
            slip.Engaged(), slip.Changes(), slip.PairUs(), slip.SpeedPercent(),
            static_cast<unsigned long long>(slip.LagUs()), sSkippedTurns.load());
        if (decision != 2) {
            post();
            for (;;) {
                if (!full) { WallNs = (posted + 1) * vbl; post(); }
                const int64_t current = message;
                full = false;
                if (current >= next) { next = current + 1; break; }
            }
        } else post();
    }
}

int main(int argc, char** argv) {
    Require(argc == 4, "usage: slip_trace <effective max> <effective trace> <config|turns|boundary-inside|boundary-outside|forbidden>");
    ExpectedMax = static_cast<unsigned>(std::atoi(argv[1]));
    ExpectedTrace = std::atoi(argv[2]) != 0;
    CheckConfig();
    const std::string scenario = argv[3];
    if (scenario == "boundary-inside" || scenario == "boundary-outside") {
        Require(ExpectedMax == 20, "shipping-boundary fixture requires actual MAX20 configuration");
        const bool inside = scenario == "boundary-inside";
        // 2R / 0.8 = 41707.5 us: mixed draw/non-draw pair costs straddle MAX20.
        RunTurns(192, inside ? 19706 : 19708, inside ? 2294 : 2292);
        const auto& slip = port::pace::GlobalSlip();
        Require(slip.PairUs() == (inside ? 41706.0 : 41708.0), "actual sampled draw/non-draw classes reach intended shipping boundary");
        Require(slip.Engaged() == inside && slip.Changes() == (inside ? 1u : 0u),
            "MAX20 engages just inside its ceiling and never engages under sustained just-over-cap load");
    } else if (scenario == "forbidden") {
        Require(ExpectedMax == 20, "positive forbidden-reset fixture requires actual MAX20 configuration");
        RunTurns(1800, 14000, 6000);
        const auto& slip = port::pace::GlobalSlip();
        Require(slip.Engaged() && slip.Changes() == 1, "forbidden-reset fixture starts from positive sustained engagement");
        RunTurns(1, 5000, 0, false);
        Require(!slip.Engaged() && slip.Changes() == 2, "active scene/forbidden reset leaves engagement without a pause gap");
        RunTurns(8, 5000, 0, false);
        Require(!slip.Engaged() && slip.Changes() == 2, "repeated forbidden turns cannot duplicate the active reset's leave transition");
    } else {
        Require(scenario == "config" || scenario == "turns", "scenario must execute a known behavioral fixture");
    }
    if (std::strcmp(argv[3], "turns") == 0) {
        RunTurns(1800, 14000, 6000);
        auto& slip = port::pace::GlobalSlip();
        if (ExpectedMax >= 3) Require(slip.Engaged() && slip.Changes() == 1, "sustained positive CPU load really engages actual Slip");
        RunTurns(160, 5000, 0);
        if (ExpectedMax >= 3) Require(!slip.Engaged() && slip.Changes() == 2, "lighter positive load leaves actual Slip once");
        RunTurns(1800, 14000, 6000);
        if (ExpectedMax >= 3) Require(slip.Engaged() && slip.Changes() == 3, "sustained load can engage again after normal leave");
        WallNs += 300000000;
        VerboseErrno = EDOM;
        RunTurns(1, 5000, 0);
        if (ExpectedMax >= 3) Require(!slip.Engaged() && slip.Changes() == 4, "positive pause gap resets engagement at original boundary");
        RunTurns(8, 5000, 0, false);
        if (ExpectedMax >= 3) Require(slip.Changes() == 4, "disallowed turns after gap do not duplicate leave transition");
        Require(Summaries.size() == (ExpectedTrace ? slip.Changes() + 1 : 0), "no periodic or duplicate summary rows");
        Require(ErrnoAccesses == (ExpectedTrace ? 2 * (slip.Changes() + 1) : 0), "only opted-in summaries access errno");
#if defined(__vita__) && PORT_LOGGING
        Require(DrawCallbacks == 3769, "existing Vita draw callback count is unchanged");
#else
        Require(DrawCallbacks == 0, "non-Vita and silent paths remain inert");
#endif
    }
    const auto& slip = port::pace::GlobalSlip();
    Require(Summaries.size() == (ExpectedTrace ? slip.Changes() + 1 : 0), "all scenarios emit only startup plus actual state transitions");
    Require(ErrnoAccesses == (ExpectedTrace ? 2 * (slip.Changes() + 1) : 0), "all scenarios touch errno only around opted-in summary calls");
    std::fprintf(stderr, "PASS slip trace max=%u trace=%u scenario=%s summaries=%zu\n",
        ExpectedMax, ExpectedTrace, argv[3], Summaries.size());
}
