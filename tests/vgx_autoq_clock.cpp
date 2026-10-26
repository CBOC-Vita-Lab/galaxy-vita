// Execute the real Vita timer adapter and controller, mocking only OS/vitaGL boundaries.
// Regressions: partial GPU coverage becoming headroom; replaying rejected publications.
#include "../platform/vgx/src/vgx_autoq.cpp"

#include <cinttypes>
#include <string>

namespace {
std::uint64_t nowUs = 1000000;
std::int64_t cpuUs = 100000, gameCpuUs = 200000, backendCpuUs = 100000;
unsigned cpuReads = 0, timerReads = 0, divisor = 0;
std::uint32_t publication = 0;
VgxRtGpuFrame gpu{};
bool target30 = false;

void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
[[noreturn]] int UnexpectedKernelCall() {
    Require(false, "GPU timer adapter must not start or poll the fallback watcher");
    std::abort();
}

vgx::autoq::FrameTimes Present(std::uint32_t head, std::uint16_t scenes,
                             std::uint16_t timed, std::uint32_t busy, bool quiet = false) {
    using namespace vgx::autoq;
    const unsigned before = cpuReads;
    for (unsigned turn = 0; turn < 2; ++turn) {
        cpuUs = ++gameCpuUs;
        port_autoq_game_begin();
        gameCpuUs += 4000;
        cpuUs = gameCpuUs;
        port_autoq_game_work_done();
    }
    publication = head;
    gpu = {busy, 31000, 2000, scenes, timed};
    nowUs += 32333;
    clock::FrameBegin();
    nowUs += 1000;
    backendCpuUs += 3000;
    cpuUs = backendCpuUs;
    clock::FramePresented();
    FrameTimes f;
    Require(clock::NextDone(f), "published timer entry can be consumed without a GPU wait");
    Require(f.startUs == nowUs - 1000 && f.presentUs == nowUs && f.gpuDoneUs == 0,
            "timer entry retains its real adapter timestamps, not a fabricated fence completion");
    Require(f.timers && f.backendRunUs == static_cast<std::uint64_t>(backendCpuUs) &&
            f.paced == (divisor != 0), "timer, CPU and pacing metadata survive the ring");
    Require(f.gameKnown == target30 && (target30 || (f.gameRunUs == 0 && f.gameTurns == 0)),
            "target-only game work is not sampled in legacy mode");
    Require(cpuReads - before == (target30 ? 5u : 1u), "disabled game hooks add no CPU-clock reads");
    FrameTimes empty;
    Require(!clock::NextDone(empty), "each published entry is consumed once");
    f.quiet = quiet;
    f.scene = 1;
    return f;
}

vgx::autoq::Config RecoveryConfig() {
    vgx::autoq::Config c;
    c.targetFps = target30 ? 30 : 0;
    c.budgetMs = 1000.0 / 30.0;
    c.windowFrames = 2;
    c.minChangeFrames = 0;
    c.upQuietHoldSec = 0.05;
    c.maxUpStep = 1;
    c.maxRung = 3;
    return c;
}
}

extern "C" {
SceGxmNotification query_fence{nullptr, 0};
std::uint64_t sceKernelGetProcessTimeWide(void) { return nowUs; }
std::int64_t port_thread_cpu_us(void) { ++cpuReads; return cpuUs; }
unsigned port_vgx_pace_divisor(void) { return divisor; }
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
void vglRtSetup(std::uint32_t flags, int priority, int affinity) {
    Require(flags == 8 && priority == 0x60 && affinity == 0x20000,
            "production timer setup retains its watcher configuration");
}
std::uint32_t vglRtLastGpuFrame(VgxRtGpuFrame* out) {
    ++timerReads;
    *out = gpu;
    return publication;
}
void vglRtGpuCounters(std::uint32_t* timed, std::uint32_t* untimed, std::uint32_t* late, std::uint32_t* pending) {
    *timed = *untimed = *late = *pending = 0;
}
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return UnexpectedKernelCall(); }
int sceKernelWaitSema(SceUID, int, SceUInt*) { return UnexpectedKernelCall(); }
int sceKernelSignalSema(SceUID, int) { return UnexpectedKernelCall(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) {
    return UnexpectedKernelCall();
}
int sceKernelStartThread(SceUID, SceSize, void*) { return UnexpectedKernelCall(); }
int sceKernelDelayThread(SceUInt) { return UnexpectedKernelCall(); }
}

int main(int argc, char** argv) {
    using namespace vgx::autoq;
    Require(argc == 3, "supply scenario and expected target mode");
    const std::string mode = argv[1];
    target30 = std::string(argv[2]) == "30";
    Require(Target30Enabled() == target30, "only the opt-in target enables new behavior");
    Require(clock::Start(), "real adapter chooses the supplied GPU timers");
    if (mode == "coverage") {
        struct Row { std::uint32_t head; std::uint16_t scenes, timed; bool oldKnown, newKnown; };
        const Row rows[] = {
            {0, 4, 4, false, false}, {1, 4, 4, true, true}, {1, 4, 4, false, false},
            {2, 4, 3, true, false}, {2, 4, 4, false, false}, {3, 4, 0, true, false},
            {4, 0, 0, true, false}, {5, 2, 3, true, false}, {6, 4, 4, true, true},
            {6, 4, 4, false, false}, {10, 4, 4, true, true},
            {0xfffffffeu, 4, 4, true, true}, {0xffffffffu, 4, 4, true, true},
            {0, 4, 4, false, false}, {1, 4, 4, true, true}
        };
        for (const Row& r : rows) {
            const auto f = Present(r.head, r.scenes, r.timed, 5000);
            const bool known = target30 ? r.newKnown : r.oldKnown;
            Require(f.busyKnown == known, "new, repeated and incomplete publications have correct validity");
            Require(f.gpuBusyUs == (known ? 5000u : 0u), "unknown work is not retained as a measured cheap frame");
        }
        setenv("GXV_AUTOQ_TARGET", target30 ? "0" : "30", 1);
        Require(Target30Enabled() == target30, "target switch remains read once");
        divisor = 2;
        const auto f = Present(2, 4, 3, 5000);
        Require(f.busyKnown == !target30, "environment mutation cannot change coverage policy mid-run");
    } else if (mode == "recovery") {
        Controller c(RecoveryConfig());
        for (unsigned i = 1; i <= 4; ++i) c.Add(Present(i, 11, 2, 5000, i == 4));
        Require(c.Current() == (target30 ? 2 : 3), "partial GPU scene coverage cannot authorize target30 recovery");
        Require(!target30 || port_autoq_picture_work_us() == 0,
                "partial GPU coverage cannot publish a pacing recovery hint");
    } else if (mode == "hint" || mode == "hint-play") {
        const bool quiet = mode == "hint";
        Controller c(RecoveryConfig());
        for (unsigned i = 1; i <= 3; ++i) c.Add(Present(i, 11, 11, 5000));
        Require(!target30 || port_autoq_picture_work_us() == 8000, "complete measured window establishes headroom");
        c.Add(Present(4, 11, 2, 5000, quiet));
        Require(c.Current() == (target30 || !quiet ? 2 : 3), "partial sample cannot use previously accumulated headroom");
        Require(!target30 || port_autoq_picture_work_us() == 0, "partial sample invalidates the old hint");
        c.Add(Present(5, 11, 11, 5000));
        Require(!target30 || port_autoq_picture_work_us() == 0, "one new valid sample cannot restore an old window");
        c.Add(Present(6, 11, 11, 5000));
        if (!quiet) {
            Require(!target30 || port_autoq_picture_work_us() == 0, "an incomplete window cannot seed a later clean one");
            c.Add(Present(7, 11, 11, 5000));
        }
        Require(!target30 || port_autoq_picture_work_us() == 8000, "a complete fresh window can recover after rejection");
    } else if (mode == "ring") {
        for (unsigned i = 1; i <= 140; ++i) {
            const bool partial = i % 7 == 0;
            const auto f = Present(i, 7, partial ? 6 : 7, i * 100);
            const bool known = !target30 || !partial;
            Require(f.busyKnown == known && f.gpuBusyUs == (known ? i * 100u : 0u),
                    "ring reuse cannot leak validity or cost from an older entry");
        }
    } else if (mode == "trace") {
        Controller c(RecoveryConfig());
        for (unsigned i = 1; i <= 32; ++i) {
            auto f = Present(i % 5 ? i : i - 1, i % 7 ? 7 : 0, i % 3 ? 7 : 2, i * 500, i % 4 == 0);
            c.Add(f);
            std::printf("%u start=%" PRIu64 " present=%" PRIu64 " done=%" PRIu64
                        " cpu=%" PRIu64 " game=%u/%u/%u known=%d paced=%d timers=%d busy=%d/%u"
                        " quiet=%d scene=%u rung=%d changes=%u hint=%" PRIu64 " reads=%u/%u\n",
                        i, f.startUs, f.presentUs, f.gpuDoneUs, f.backendRunUs,
                        f.gameRunUs, f.gameTurns, f.gameFaults, f.gameKnown, f.paced, f.timers,
                        f.busyKnown, f.gpuBusyUs, f.quiet, f.scene, c.Current(), c.Changes(),
                        port_autoq_picture_work_us(), cpuReads, timerReads);
        }
    } else Require(false, "known scenario");
    std::printf("PASS: autoq clock %s target=%d\n", mode.c_str(), target30 ? 30 : 0);
}
