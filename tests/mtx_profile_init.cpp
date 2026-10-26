#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../platform/vgx/src/vgx_common.h"

static std::atomic<unsigned> parses{0}, entered{0}, returned{0};
static std::atomic<bool> releaseParser{false};
namespace vgx {
static long HeldEnvInt(const char* name, long fallback) {
    if (std::strcmp(name, "GXV_MTXPROFILE") != 0) return EnvInt(name, fallback);
    parses.fetch_add(1, std::memory_order_release);
    while (!releaseParser.load(std::memory_order_acquire)) std::this_thread::yield();
    return EnvInt(name, fallback);
}
}
// Redirect this fixture's EnvInt tokens, but hold/count only the matrix control.
#define EnvInt HeldEnvInt
#include "../platform/src/port/perf.cpp"
#undef EnvInt
#include "revolution/mtx.h"

extern "C" u32 C_MTXInverse(const Mtx, Mtx);
extern "C" u32 mtx_profile_reference(const Mtx, Mtx);
static std::vector<std::string> logs;
extern "C" int port_is_game_thread() { return 1; }
extern "C" int64_t port_host_time_ns() { return 1000000000; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* fmt, ...) {
    char line[1024]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    logs.emplace_back(line);
}
extern "C" int port_handheld() { return 0; }
extern "C" unsigned port_vgx_backend_pending() { return 0; }
extern "C" int64_t port_vgx_player_cpu_us() { return 0; }
extern "C" void port_vgx_diagnostic_csv_pause(int) {}
extern "C" void port_autoq_game_begin() {}
extern "C" void port_autoq_game_work_done() {}
extern "C" void port_settings_panel_turn(int) {}
namespace MR { const char* getCurrentStageName() { return ""; } }

static void require(bool yes, const char* why) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static void await(const std::atomic<unsigned>& count, unsigned expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (count.load(std::memory_order_acquire) != expected) {
        require(std::chrono::steady_clock::now() < deadline, "first-use fixture reached its bounded rendezvous");
        std::this_thread::yield();
    }
}

int main(int argc, char** argv) {
    require(argc == 3 && PORT_MTX_PROFILE_COMPILED, "expected setting and enabled expectation in diagnostic build");
    setenv("GXV_MTXPROFILE", argv[1], 1);
    setenv("GXV_MTXPROFILE_TEST_OTHER", "0x7suffix", 1);
    require(vgx::HeldEnvInt("GXV_MTXPROFILE_TEST_OTHER", 0) == 7 && parses.load() == 0,
            "unrelated environment reads bypass the held matrix lookup and retain real parsing");
    const bool on = atoi(argv[2]) != 0;
    constexpr unsigned kThreads = 16;
    std::array<bool, kThreads> valid{};
    auto call = [&](unsigned id) {
        const Mtx source = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}};
        Mtx reference{}, actual{};
        const u32 expected = mtx_profile_reference(source, reference);
        const int incoming = 1200 + static_cast<int>(id);
        errno = incoming;
        entered.fetch_add(1, std::memory_order_release);
        const u32 result = C_MTXInverse(source, actual);
        valid[id] = result == expected && memcmp(actual, reference, sizeof(Mtx)) == 0 && errno == incoming;
        const u32 second = C_MTXInverse(source, actual);
        valid[id] = valid[id] && second == expected && memcmp(actual, reference, sizeof(Mtx)) == 0 && errno == incoming;
        returned.fetch_add(1, std::memory_order_release);
    };
    std::vector<std::thread> workers;
    workers.emplace_back(call, 0);
    await(parses, 1); // Hold the initializing thread before the actual environment parse.
    for (unsigned i = 1; i < kThreads; ++i) workers.emplace_back(call, i);
    await(entered, kThreads);
    std::this_thread::yield();
    require(parses.load() == 1 && returned.load() == 0, "followers cannot parse or finish before initializer publication");
    releaseParser.store(true, std::memory_order_release);
    await(returned, kThreads);
    for (auto& worker : workers) worker.join();
    require(parses.load() == 1 && returned.load() == kThreads, "exactly one real environment parse serves every first-use caller");
    for (bool result : valid) require(result, "every thread preserves inverse output and its own incoming errno");
    for (unsigned i = 0; i < 256; ++i) { port_perf_frame_begin(); port_perf_frame_work_done(); }
    unsigned headers = 0, rows = 0;
    for (const std::string& line : logs) {
        if (line.find("mtx-profile cumulative turns=") == 0) {
            unsigned long long turns, overflow; unsigned threads;
            require(sscanf(line.c_str(), "mtx-profile cumulative turns=%llu threads=%u overflow-threads=%llu", &turns, &threads, &overflow) == 3 &&
                    turns == 256 && threads == kThreads && overflow == 0, "all initializing and waiting callers own one slot");
            ++headers;
        } else if (line.find("mtx-profile cumulative thread=") == 0) {
            unsigned id; unsigned long long calls, repeats, singular, work, workRepeats, workSingular, guest;
            require(sscanf(line.c_str(), "mtx-profile cumulative thread=%u calls=%llu repeats=%llu singular=%llu work=%llu work-repeats=%llu work-singular=%llu guest=%llu",
                           &id, &calls, &repeats, &singular, &work, &workRepeats, &workSingular, &guest) == 8 &&
                    id >= 1 && id <= kThreads && calls == 2 && repeats == 1 && singular == 0 && work == 0 && workRepeats == 0 && workSingular == 0 && guest == 2,
                    "every enabled caller counts both calls and the exact per-thread repeat");
            ++rows;
        }
    }
    require(on ? headers == 1 && rows == kThreads : headers == 0 && rows == 0, "all first-use callers observe the same enabled or disabled result");
    require(parses.load() == 1, "frame reporting does not re-read the environment");
    printf("PASS: concurrent first use setting=%s enabled=%d; 16 threads; one real parse; errno/output/counts preserved\n", argv[1], on);
}
