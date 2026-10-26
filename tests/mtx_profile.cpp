#include <array>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../platform/src/port/perf.cpp"
#include "revolution/mtx.h"

extern "C" u32 mtx_profile_reference(const Mtx, Mtx);
extern "C" u32 C_MTXInverse(const Mtx, Mtx);
static std::vector<std::string> logs;
static std::mutex logMutex;
static std::atomic<unsigned> clockReads{0};
static thread_local bool guest = true;
extern "C" int port_is_game_thread() { return guest; }
extern "C" int64_t port_host_time_ns() { ++clockReads; return 1000000000; }
#if defined(__vita__)
extern "C" int64_t port_thread_cpu_us() { return 1000; }
#endif
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* fmt, ...) {
    char line[1024]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> lock(logMutex); logs.emplace_back(line);
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
struct Expected {
    uint64_t calls = 0, repeats = 0, singular = 0, work = 0, workRepeats = 0, workSingular = 0, guests = 0;
    bool have = false, active = false;
    std::array<unsigned char, 48> previous{};
};
using Bits = std::array<uint32_t, 12>;
static Bits identity() { return {0x3f800000,0,0,0, 0,0x3f800000,0,0, 0,0,0x3f800000,0}; }
static void invoke(const Bits& bits, int destination, Expected& e) {
    alignas(16) std::array<uint32_t, 48> actual, reference;
    actual.fill(0x42ac0000); memcpy(actual.data() + 12, bits.data(), 48); reference = actual;
    std::array<unsigned char, 48> key{}; memcpy(key.data(), actual.data() + 12, 48);
    const bool repeat = e.have && key == e.previous;
    ++e.calls; e.repeats += repeat; e.workRepeats += e.active && repeat; e.previous = key; e.have = true;
    e.work += e.active; e.guests += guest;
    feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO);
    errno = EDOM;
    const u32 expected = mtx_profile_reference(reinterpret_cast<const float(*)[4]>(reference.data() + 12),
                                                reinterpret_cast<float(*)[4]>(reference.data() + destination));
    const int expectedFlags = fetestexcept(FE_ALL_EXCEPT), expectedErrno = errno, expectedRound = fegetround();
    feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO); errno = EDOM;
    const unsigned clocks = clockReads.load();
    const auto source = reinterpret_cast<const float(*)[4]>(actual.data() + 12);
    const auto output = reinterpret_cast<float(*)[4]>(actual.data() + destination);
    const u32 result = (e.calls & 1) ? C_MTXInverse(source, output) : PSMTXInverse(source, output);
    const int flags = fetestexcept(FE_ALL_EXCEPT), actualErrno = errno, rounding = fegetround();
    require(result == expected && actual == reference, "production inverse preserves return, every byte, aliases and singular no-write");
    require(flags == expectedFlags && rounding == expectedRound, "inverse preserves floating exceptions and rounding");
    require(actualErrno == expectedErrno, "first-use environment parsing and inverse preserve errno");
    require(clockReads.load() == clocks, "inverse observer does not query a clock");
    e.singular += result == 0;
    e.workSingular += e.active && result == 0;
}

int main(int argc, char** argv) {
    require(argc == 3, "expected setting and enabled expectation");
    if (!strcmp(argv[1], "unset")) unsetenv("GXV_MTXPROFILE"); else setenv("GXV_MTXPROFILE", argv[1], 1);
    const bool on = atoi(argv[2]) != 0 && PORT_MTX_PROFILE_COMPILED;
    Expected main;
    invoke(identity(), 28, main); // First-use parsing happens inside the production inverse.
    setenv("GXV_MTXPROFILE", on ? "0" : "1", 1);
    port_perf_frame_begin(); main.active = true;
    invoke(identity(), 12, main);
    Bits negativeZero = identity(); negativeZero[1] = 0x80000000u;
    invoke(negativeZero, 12, main); invoke(negativeZero, 12, main);
    Bits singular{};
    invoke(singular, 12, main); invoke(singular, 28, main);
    uint32_t random = 0x72c845a3;
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        require(fesetround(rounding) == 0, "test rounding mode is supported");
        for (unsigned i = 0; i < 128; ++i) {
            Bits b;
            for (uint32_t& x : b) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; x = random; }
            if (i == 0) b = {1,0,0,0, 0,1,0,0, 0,0,1,0}; // Subnormals.
            if (i == 1) { b = identity(); b[0] = 0x7f800001u; } // Signaling NaN.
            if (i == 2) { b = identity(); b[0] = 0x7fc01234u; } // Quiet NaN payload.
            if (i == 3) { b = identity(); b[0] = 0x7f800000u; }
            for (int destination : {0, 8, 12, 16, 28}) invoke(b, destination, main);
        }
    }
    require(fesetround(FE_TONEAREST) == 0, "restore rounding");
    std::array<Expected, 40> expectedWorkers;
    std::atomic<unsigned> ready{0}; std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < expectedWorkers.size(); ++i) workers.emplace_back([&, i] {
        guest = (i & 1) == 0; ++ready;
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        Bits b = identity(); b[3] = 0x3f800000u + i;
        invoke(b, 12, expectedWorkers[i]); invoke(b, 28, expectedWorkers[i]);
        invoke(Bits{}, 8, expectedWorkers[i]); invoke(Bits{}, 16, expectedWorkers[i]);
    });
    while (ready.load() != workers.size()) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& thread : workers) thread.join();
    port_perf_frame_work_done(); main.active = false;
    for (unsigned i = 1; i < 256; ++i) { port_perf_frame_begin(); port_perf_frame_work_done(); }
    uint64_t calls = main.calls, repeats = main.repeats, zeros = main.singular, guests = main.guests;
    for (const auto& e : expectedWorkers) { calls += e.calls; repeats += e.repeats; zeros += e.singular; guests += e.guests; }
    uint64_t gotCalls = 0, gotRepeats = 0, gotZeros = 0, gotWork = 0, gotWorkRepeats = 0, gotWorkZeros = 0, gotGuests = 0;
    unsigned rows = 0, overflowRows = 0, workRows = 0, headers = 0;
    for (const std::string& line : logs) {
        if (line.find("mtx-profile cumulative turns=") == 0) {
            unsigned long long turns, overflow; unsigned threads;
            require(sscanf(line.c_str(), "mtx-profile cumulative turns=%llu threads=%u overflow-threads=%llu", &turns, &threads, &overflow) == 3,
                    "bounded report header is machine-readable");
            require(turns == 256 && threads == 32 && overflow == 9, "slot exhaustion is explicit and counts every extra thread");
            ++headers;
        } else if (line.find("mtx-profile cumulative thread=") == 0) {
            char id[32]; unsigned long long c, r, z, w, wr, wz, g;
            require(sscanf(line.c_str(), "mtx-profile cumulative thread=%31s calls=%llu repeats=%llu singular=%llu work=%llu work-repeats=%llu work-singular=%llu guest=%llu",
                           id, &c, &r, &z, &w, &wr, &wz, &g) == 8, "per-thread totals are machine-readable");
            ++rows; overflowRows += !strcmp(id, "overflow");
            if (w) { ++workRows; require(w == main.work && c == main.calls, "work scope identifies only the actual main caller"); }
            gotCalls += c; gotRepeats += r; gotZeros += z; gotWork += w; gotWorkRepeats += wr; gotWorkZeros += wz; gotGuests += g;
        }
    }
    if (on) {
        require(headers == 1 && rows == 33 && overflowRows == 1 && workRows == 1, "enabled observer emits one bounded aggregate report");
        require(gotCalls == calls && gotRepeats == repeats && gotZeros == zeros && gotWork == main.work &&
                gotWorkRepeats == main.workRepeats && gotWorkZeros == main.workSingular && gotGuests == guests,
                "all per-thread and overflow counts match the independent expected stream");
    } else require(headers == 0 && rows == 0, "off/invalid/quiet observer emits nothing");
    printf("PASS: matrix profile setting=%s enabled=%d calls=%llu exact outputs/aliases/FP/errno; 41 threads; counts/read-once/log bounds\n",
           argv[1], on, static_cast<unsigned long long>(calls));
}
