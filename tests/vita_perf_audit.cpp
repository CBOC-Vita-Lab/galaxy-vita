// Execute the production accounting and skip controller with deterministic clocks.
#ifndef VITA_PERF_SOURCE
#define VITA_PERF_SOURCE "../platform/src/port/perf.cpp"
#endif
#include VITA_PERF_SOURCE
#include <cstdarg>
#include <cmath>
#include <string>
#undef vsnprintf

static int64_t wallNs = 1000000000, cpuUs = 100000, publishedPlayerCpuUs;
static unsigned pendingFrames;
static std::string lastSlowLog;
static unsigned checks;
static unsigned wallReads, cpuReads, playerReads, logCalls;
static std::string turnCostLogs;
static void require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
extern "C" int64_t port_host_time_ns() { ++wallReads; return wallNs; }
extern "C" int64_t port_thread_cpu_us() { ++cpuReads; return cpuUs; }
extern "C" int64_t port_vgx_player_cpu_us() { ++playerReads; return publishedPlayerCpuUs; }
extern "C" unsigned port_vgx_backend_pending() { return pendingFrames; }
extern "C" int port_handheld() { return 1; }
extern "C" void port_log(const char* format, ...) {
    ++logCalls;
    char line[2048]; va_list args; va_start(args, format);
    ::vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (std::strstr(line, "slow turn:")) lastSlowLog = line;
    if (!std::strncmp(line, "turn-cost:", 10)) { turnCostLogs += line; turnCostLogs += '\n'; }
}
struct Totals { int64_t wall, cpu, max; uint32_t turns; };
static Totals totals() {
    Totals t{}; port_perf_game_totals(&t.wall, &t.turns, &t.cpu, &t.max); return t;
}
static void work(int64_t elapsedNs, int64_t usedCpuUs) {
    port_perf_frame_begin(); wallNs += elapsedNs; cpuUs += usedCpuUs; port_perf_frame_work_done();
}
static void cpuScope() {
    work(10000000, 6000);
    const auto a = totals();
    wallNs += 6000000; cpuUs += 4000; // CPU work between frame-work spans (e.g. retrace IRQ handling).
    work(10000000, 7000);
    const auto b = totals();
    require(b.wall - a.wall == 10000000 && b.turns - a.turns == 1, "work wall/count span is exactly the second work interval");
    std::printf("cpu-scope: work_wall_us=%lld, actual_work_cpu_us=7000, reported_cpu_us=%lld\n",
                (long long)((b.wall - a.wall) / 1000), (long long)(b.cpu - a.cpu));
    require(b.cpu - a.cpu == 7000, "CPU comparison must exclude CPU outside the matching work span");
}
static void cpuSampleFailure() {
    struct Samples { int64_t begin, end; };
    for (const auto sample : {Samples{0, 1007000}, Samples{1000000, 0}, Samples{0, 0}, Samples{1000000, 999000}}) {
        const auto before = totals();
        cpuUs = sample.begin;
        port_perf_frame_begin();
        wallNs += 10000000;
        cpuUs = sample.end;
        port_perf_frame_work_done();
        const auto after = totals();
        require(after.wall - before.wall == 10000000 && after.turns - before.turns == 1,
                "failed CPU reads do not discard work wall time or simulation turns");
        require(after.cpu == before.cpu, "zero/failing or backwards CPU samples must not add lifetime CPU or negative time");
    }
    const auto before = totals();
    cpuUs = 2000000;
    work(10000000, 7000);
    require(totals().cpu - before.cpu == 7000, "valid CPU accounting resumes after a failed sample");
}
static void stalePlayer() {
    publishedPlayerCpuUs = 1000;
    port_perf_frame_begin();
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_GX_FINISH_WAIT);
    wallNs += 50000000;
    // Counter is only published at a front-end frame boundary. Earlier work can arrive late.
    publishedPlayerCpuUs = 61000;
    cpuUs += 1000;
    port_perf_frame_work_done();
    const auto at = lastSlowLog.find("front-end frame-boundary CPU counter advanced ");
    require(at != std::string::npos, "real slow-turn reporter emitted a front-end CPU diagnostic");
    long long reported = -1;
    require(std::sscanf(lastSlowLog.c_str() + at, "front-end frame-boundary CPU counter advanced %lld", &reported) == 1,
            "front-end CPU diagnostic contains a numeric duration");
    std::printf("stale-player: turn_wall_ms=50, reported_player_cpu_ms=%lld\n", reported);
    require(reported == 60, "a delayed frame-boundary publication is reported honestly, not clamped to turn wall");
    require(lastSlowLog.find("not turn-scoped; includes idle spin") != std::string::npos &&
            lastSlowLog.find("CPU in this turn") == std::string::npos,
            "delayed player counter must not claim matched turn CPU or infer blocked/preempted time");
}
static void skipSweep(int64_t costNs, int64_t skippedCostNs = 0) {
    setenv("PETARI_PIPE_DEPTH", "0", 1);
    if (!skippedCostNs) skippedCostNs = costNs;
    unsigned drawn = 0, skipped = 0, run = 0, longest = 0;
    constexpr unsigned turns = 2000;
    for (unsigned i = 0; i < turns; ++i) {
        const int decision = port_frameskip_turn(1);
        if (decision) { ++skipped; ++run; longest = std::max(longest, run); }
        else { ++drawn; run = 0; }
        require(decision == 0 || decision == 2, "no GPU backlog cannot request GPU-only skip");
        require(run <= 3, "default late skip run is bounded to three");
        wallNs += decision ? skippedCostNs : costNs;
    }
    require(port_frameskip_count() == skipped, "reported skip count matches actual policy decisions");
    if (costNs <= 16683350) require(skipped == 0, "on-time work does not accumulate late skip debt");
    if (costNs == 18100000 && skippedCostNs == costNs)
        require(skipped > 1400, "persistent 18.1 ms turns reproduce quarter-rate drawing policy");
    if (costNs == 18100000 && skippedCostNs == 10000000)
        require(drawn > 1500, "skipping recovers debt when it makes the turn materially cheaper");
    std::printf("skip-policy: drawn_turn_ms=%.4f skipped_turn_ms=%.4f turns=%u drawn=%u skipped=%u longest_run=%u drawn_fraction=%.4f\n",
                costNs / 1e6, skippedCostNs / 1e6, turns, drawn, skipped, longest, double(drawn) / turns);
    wallNs += 300000000;
    require(port_frameskip_turn(1) == 0, "a long load clears catch-up debt");
    wallNs += 20000000;
    require(port_frameskip_turn(0) == 0, "scene transition disables skipping and resets debt");
}
static void backlogBound() {
    pendingFrames = 2;
    unsigned run = 0, skipped = 0;
    for (unsigned i = 0; i < 700; ++i) {
        const int decision = port_frameskip_turn(1);
        require(decision == 0 || decision == 1, "on-time backlog never requests a late/no-retrace skip");
        if (decision) { ++run; ++skipped; }
        else run = 0;
        require(run <= 6, "persistent backlog still allows a picture at the configured bound");
        wallNs += 16683350;
    }
    require(skipped == 600 && port_frameskip_count() == 600, "default backlog skips exactly six of seven pictures");
    pendingFrames = 0;
    require(port_frameskip_turn(1) == 0, "clearing backlog immediately permits a picture");
    std::printf("backlog-policy: turns=700 drawn=100 skipped=600; backlog clear resumes drawing\n");
}
static void forcedCount() {
    setenv("PETARI_FORCE_SKIP", "3", 1);
    unsigned skipped = 0;
    for (unsigned i = 0; i < 8; ++i) { skipped += port_frameskip_turn(1) != 0; wallNs += 10000000; }
    std::printf("forced-count: actual_skips=%u reported_skips=%u\n", skipped, port_frameskip_count());
    require(skipped == 6, "forced skip fixture omits exactly three of every four pictures");
    require(port_frameskip_count() == skipped, "forced skips must also appear in the public skip total");
}
static void skipCostModel(int64_t drawnNs, int64_t skippedNs) {
    require(drawnNs >= 16683350 && skippedNs > 0, "model uses explicit positive turn intervals, with drawn turns at least one refresh");
    setenv("PETARI_PIPE_DEPTH", "0", 1);
    const char* setting = getenv("PETARI_FRAMESKIP");
    const unsigned limit = setting ? static_cast<unsigned>(std::max(0, std::min(8, atoi(setting)))) : 3;
    constexpr unsigned turns = 20000;
    unsigned drawn = 0, skipped = 0, run = 0, longest = 0;
    const int64_t start = wallNs;
    for (unsigned i = 0; i < turns; ++i) {
        const int decision = port_frameskip_turn(1);
        require(decision == 0 || decision == 2, "model has no GPU backlog");
        if (decision) {
            ++skipped;
            longest = std::max(longest, ++run);
        } else {
            ++drawn;
            run = 0;
        }
        require(run <= limit, "configured consecutive skip limit is preserved");
        // Complete call-to-call intervals are injected, not measured or inferred from the Vita log.
        wallNs += decision ? skippedNs : drawnNs;
    }
    require(drawn + skipped == turns && port_frameskip_count() == skipped, "model decisions and public counts agree");
    if (limit == 0) require(skipped == 0, "disabling skipping does not add simulation work");
    if (drawnNs == 50000000 && skippedNs == 12000000 && limit == 3)
        require(drawn == 5000 && skipped == 15000, "default bound reaches one picture per four turns in this workload");
    if (drawnNs == 50000000 && skippedNs == 12000000 && limit == 1)
        require(drawn == 10000 && skipped == 10000, "one-skip bound trades simulation turns for pictures in the same workload");
    const double seconds = (wallNs - start) / 1e9;
    std::printf("cost-model: limit=%u drawn_ms=%.3f skipped_ms=%.3f drawn=%u skipped=%u "
                "pictures_per_s=%.3f turns_per_s=%.3f longest_run=%u; synthetic intervals, NOT physical Vita FPS\n",
                limit, drawnNs / 1e6, skippedNs / 1e6, drawn, skipped, drawn / seconds, turns / seconds, longest);
}
static void quietDiagnostics() {
    port_perf_frame_begin();
    port_game_progress_checkpoint(PORT_GAME_PHASE_DRAW);
    port_game_progress_irq_checkpoint(0, 1);
    port_game_progress_gpu_checkpoint(1);
    port_game_progress_cpu_checkpoint(12);
    wallNs += 50000000;
    port_perf_frame_work_done();
    const auto t = totals();
    require(wallReads == 0 && cpuReads == 0 && playerReads == 0,
            "quiet frame accounting must not sample wall/thread/player clocks or time phases");
    require(logCalls == 0, "quiet frames must not format or emit diagnostic reports");
    require(t.turns == 1, "quiet mode retains the simulation-turn count");
    require(t.wall == 0 && t.cpu == 0 && t.max == 0, "quiet mode must not publish unmeasured timing values");
    PortGameProgress sample{};
    port_game_progress_poll(0, &sample);
    require(port_game_progress_poll(10000000000ll, &sample) == 0, "quiet mode does not publish diagnostic checkpoints");
}
struct CostRow {
    unsigned turns = 0, cpuSamples = 0, bothReasons = 0, phaseTurns = 0;
    unsigned long long work = 0, sampledWork = 0, cpu = 0, draw = 0, anim = 0, audio = 0, finish = 0, record = 0;
};
static CostRow costRow(const char* kind) {
    const std::string prefix = std::string("turn-cost: kind=") + kind + " ";
    const auto at = turnCostLogs.find(prefix);
    require(at != std::string::npos, "cost report retains each observed turn policy separately");
    require(turnCostLogs.find(prefix, at + 1) == std::string::npos, "a report contains one row per policy");
    CostRow row;
    require(std::sscanf(turnCostLogs.c_str() + at + prefix.size(),
        "turns=%u cpu_samples=%u both_reasons=%u phase_turns=%u work_us=%llu sampled_work_us=%llu cpu_us=%llu draw_us=%llu anim_us=%llu audio_objects_us=%llu finish_wait_us=%llu record_us=%llu",
        &row.turns, &row.cpuSamples, &row.bothReasons, &row.phaseTurns, &row.work, &row.sampledWork, &row.cpu, &row.draw, &row.anim, &row.audio, &row.finish, &row.record) == 12,
        "report exposes totals with matching CPU sample counts and explicit units");
    return row;
}
static void turnCosts() {
    const unsigned firstWall = wallReads, firstCpu = cpuReads;
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 0, "first on-time turn permits drawing");
    wallNs += 2000000;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_DRAW);
    wallNs += 3000000;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
    wallNs += 4000000;
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_OBJECTS);
    wallNs += 2000000; cpuUs += 6000;
    port_perf_record(700000);
    port_perf_frame_work_done();
    require(wallReads - firstWall == 6 && cpuReads - firstCpu == 2,
            "policy cost accounting reuses existing wall and CPU samples");

    wallNs += 29000000; cpuUs += 9000; // Outside work: must not enter any policy's CPU total.
    pendingFrames = 2;
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 2, "late wins when late and pipeline skip conditions both hold");
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_GX_FINISH_WAIT);
    wallNs += 5000000; cpuUs += 3000;
    port_perf_frame_work_done();

    pendingFrames = 0;
    port_perf_frame_begin();
    require(port_frameskip_turn(0) == 0, "scene transition requires a drawn turn and resets debt");
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_CALC_ANIM);
    wallNs += 2000000; cpuUs += 1000;
    port_perf_frame_work_done();

    wallNs += 14683350;
    pendingFrames = 2;
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 1, "on-time pipeline pressure selects a retrace-taking skip");
    PORT_GAME_PROGRESS(PORT_GAME_PHASE_AUDIO_OBJECTS);
    wallNs += 4000000; cpuUs += 2000;
    port_perf_frame_work_done();

    // New frame without a skip-controller call must not inherit the prior policy.
    port_perf_frame_begin();
    wallNs += 3000000; cpuUs += 1500;
    port_perf_frame_work_done();
    wallNs += 11000000000ll;
    port_perf_frame_begin();
    wallNs += 1000000; cpuUs = 0; // Failed CPU sample remains explicitly unmeasured.
    port_perf_frame_work_done();

    const auto drawn = costRow("draw-selected"), late = costRow("late-skip");
    const auto required = costRow("required-draw"), backlog = costRow("backlog-skip"), unknown = costRow("unclassified");
    require(drawn.turns == 1 && drawn.cpuSamples == 1 && drawn.work == 11000 && drawn.cpu == 6000 && drawn.bothReasons == 0,
            "selected draw totals exclude between-turn CPU and preserve matched spans");
    require(drawn.draw == 3000 && drawn.anim == 4000 && drawn.audio == 2000 && drawn.finish == 0 && drawn.record == 700,
            "selected draw reports real checkpoint durations without conflating recording");
    require(late.turns == 1 && late.cpuSamples == 1 && late.bothReasons == 1 && late.work == 5000 && late.cpu == 3000 && late.finish == 5000,
            "late skip retains its work and overlapping pipeline condition");
    require(required.turns == 1 && required.work == 2000 && required.cpu == 1000 && required.anim == 2000 && required.bothReasons == 0,
            "required drawing is not mixed with ordinary selected-draw work");
    require(backlog.turns == 1 && backlog.cpuSamples == 1 && backlog.work == 4000 && backlog.cpu == 2000 && backlog.audio == 4000 && backlog.bothReasons == 0,
            "pipeline-only skip is measured independently from late catch-up");
    require(unknown.turns == 2 && unknown.cpuSamples == 1 && unknown.work == 4000 && unknown.cpu == 1500 && unknown.anim == 0 && unknown.audio == 0,
            "unclassified turns reset policy and phases, and do not fabricate failed CPU samples");
    require(unknown.sampledWork == 3000 && unknown.sampledWork - unknown.cpu == 1500 && unknown.phaseTurns == 0,
            "off-CPU gap uses only matching sampled wall spans, with unmeasured phases identified");
    require(drawn.sampledWork == drawn.work && late.sampledWork == late.work && required.sampledWork == required.work && backlog.sampledWork == backlog.work &&
            drawn.phaseTurns == 1 && late.phaseTurns == 1 && required.phaseTurns == 1 && backlog.phaseTurns == 1,
            "valid CPU and checkpoint sample coverage follows the corresponding policy");
    require(turnCostLogs.find("kind=forced-skip") == std::string::npos, "unseen policies do not emit invented samples");
    const auto all = totals();
    require(all.turns == 6 && all.wall == 26000000 && all.cpu == 13500,
            "policy totals conserve the existing work and valid CPU accounting");

    turnCostLogs.clear();
    pendingFrames = 0;
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 0, "long report gap resets catch-up debt");
    wallNs += 11000000000ll; cpuUs = 1000;
    port_perf_frame_work_done();
    const auto next = costRow("draw-selected");
    require(next.turns == 1 && next.cpuSamples == 0 && next.work == 11000000 && next.cpu == 0 && next.draw == 0,
            "next report contains only fresh samples, including an invalid begin CPU read");
    require(next.sampledWork == 0 && next.phaseTurns == 0, "failed begin CPU sample cannot fabricate sampled work or checkpoint coverage");
    require(turnCostLogs.find("kind=late-skip") == std::string::npos, "reported policy costs reset at the window boundary");
    std::printf("%s", turnCostLogs.c_str());
}
static void forcedCosts() {
    setenv("PETARI_FORCE_SKIP", "1", 1);
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 2, "forced skip retains existing return code");
    wallNs += 3000000; cpuUs += 1000;
    port_perf_frame_work_done();
    wallNs += 11000000000ll;
    port_perf_frame_begin();
    require(port_frameskip_turn(0) == 0, "forced override still respects required-draw turns");
    wallNs += 1000000; cpuUs += 500;
    port_perf_frame_work_done();
    const auto forced = costRow("forced-skip"), required = costRow("required-draw");
    require(forced.turns == 1 && forced.work == 3000 && forced.cpu == 1000 && required.turns == 1 && required.work == 1000,
            "forced skips are never misreported as natural lateness");
    require(turnCostLogs.find("kind=late-skip") == std::string::npos, "forced override creates no late-skip samples");
    turnCostLogs.clear();
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 0, "forced sequence still includes its scheduled draw turn");
    wallNs += 11000000000ll; cpuUs += 1000;
    port_perf_frame_work_done();
    require(costRow("draw-selected").turns == 1 && turnCostLogs.find("kind=forced-skip") == std::string::npos,
            "forced draw turn is not labelled as a skipped turn");
}
static void fastSkipCosts() {
    for (unsigned i = 0; i < 620; ++i) {
        port_perf_frame_begin();
        port_frameskip_turn(1);
        wallNs += 1000000; cpuUs += 500;
        port_perf_frame_work_done();
        wallNs += 15700000;
    }
    const auto drawn = costRow("draw-selected"), late = costRow("late-skip");
    require(drawn.turns + late.turns == 600 && late.turns > 0 && logCalls == 2,
            "near-60-Hz simulation with late skips retains policy rows without enabling unrelated verbose summaries");
    require(drawn.sampledWork == drawn.work && late.sampledWork == late.work,
            "skip-heavy high-rate report preserves matched CPU spans");
}
static void doubleDone() {
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 0, "double-completion fixture starts with a real selected draw");
    wallNs += 1000000; cpuUs += 500;
    port_perf_frame_work_done();
    wallNs += 2000000; cpuUs += 1000;
    port_perf_frame_work_done();
    const auto result = totals();
    require(result.turns == 1 && result.wall == 1000000 && result.cpu == 500,
            "completing a closed diagnostic work span cannot double-count work or reuse policy");
}
static void disabledCosts() {
    setenv("PETARI_FRAMESKIP", "0", 1);
    port_perf_frame_begin();
    require(port_frameskip_turn(1) == 0, "disabled skipper retains ordinary draw selection");
    wallNs += 1000000; cpuUs += 500;
    port_perf_frame_work_done();
    port_perf_frame_begin();
    require(port_frameskip_turn(0) == 0, "disabled skipper still distinguishes mandatory drawing");
    wallNs += 11000000000ll; cpuUs += 500;
    port_perf_frame_work_done();
    require(costRow("draw-selected").turns == 1 && costRow("required-draw").turns == 1,
            "disabled and transition early returns remain classified without inventing skips");
}
int main(int argc, char** argv) {
    require(argc >= 2, "scenario supplied");
    if (!std::strcmp(argv[1], "quiet")) quietDiagnostics();
    else if (!std::strcmp(argv[1], "cpu-scope")) cpuScope();
    else if (!std::strcmp(argv[1], "cpu-failure")) cpuSampleFailure();
    else if (!std::strcmp(argv[1], "stale-player")) stalePlayer();
    else if (!std::strcmp(argv[1], "forced-count")) forcedCount();
    else if (!std::strcmp(argv[1], "cheap-skips")) skipSweep(18100000, 10000000);
    else if (!std::strcmp(argv[1], "backlog")) backlogBound();
    else if (!std::strcmp(argv[1], "turn-costs")) turnCosts();
    else if (!std::strcmp(argv[1], "forced-costs")) forcedCosts();
    else if (!std::strcmp(argv[1], "fast-skip-costs")) fastSkipCosts();
    else if (!std::strcmp(argv[1], "double-done")) doubleDone();
    else if (!std::strcmp(argv[1], "disabled-costs")) disabledCosts();
    else if (!std::strcmp(argv[1], "cost-model")) {
        require(argc == 4, "cost model requires drawn and skipped complete-turn intervals in nanoseconds");
        skipCostModel(std::strtoll(argv[2], nullptr, 10), std::strtoll(argv[3], nullptr, 10));
    }
    else skipSweep(std::strtoll(argv[1], nullptr, 10));
    std::printf("PASS: production perf characterization (%u checks; synthetic clocks, not Vita FPS)\n", checks);
}
