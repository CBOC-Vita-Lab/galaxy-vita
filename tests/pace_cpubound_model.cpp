// A model, not a console measurement: what the picture-rate logic does to a CPU-bound stretch, with the real pacing controller
// (port::pace::Controller, Fixed30 policy: frame_pace.h) and a copy of the legacy frame-skip rule (port/perf.cpp
// port_frameskip_turn, PETARI_FRAMESKIP default 3 on a handheld). The game thread's turn costs G ms (update + calcAnim, paid by every turn
// whether it draws or not) plus D ms more on a turn that draws; the turn then waits for the next retrace unless it was a catch-up
// skip (decision 2). Retraces come every 16.683 ms. The GPU and the render threads are taken as keeping up (no pending pictures),
// which is the CPU-bound case: the renderer's work shows only as D. For each (G, D) the table gives, over 30 s of model time, the
// game's speed (turns done against real time; 100 % = 60 turns a second), the pictures a second, and the turns that drew, skipped
// to catch up (no retrace wait) and skipped without lateness.
//   tools/test_pace_cpubound_model.sh
#include "port/frame_pace.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int64_t kVbl = 16683;      // us
constexpr int64_t kEnd = 30000000;   // 30 s

struct Result { double speed, pictures; unsigned drawn, late, notLate; };

// The legacy rule of port_frameskip_turn with maxRun 3 and no pending pictures (the GPU not behind).
struct Legacy {
    int64_t last = 0, debt = 0;
    int run = 0;
    int Turn(int64_t now) {
        const int64_t dt = last ? now - last : kVbl;
        last = now;
        if (dt > 15 * kVbl) { debt = 0; run = 0; return 0; }
        debt += dt - kVbl;
        if (debt < 0) debt = 0;
        if (debt > 4 * kVbl) debt = 4 * kVbl;
        const bool behind = debt >= kVbl / 2 && run < 3;
        if (behind) { ++run; return 2; }
        run = 0;
        return 0;
    }
};

template <class Pacer> Result Run(Pacer&& decide, int64_t gUs, int64_t dUs) {
    int64_t t = 1000000;  // (after the start-up)
    const int64_t t0 = t;
    unsigned turns = 0, drawn = 0, late = 0, notLate = 0;
    while (t - t0 < kEnd) {
        const int d = decide(t);
        ++turns;
        t += gUs;
        if (d == 0) { ++drawn; t += dUs; } else if (d == 2) ++late; else ++notLate;
        if (d != 2) t = (t / kVbl + 1) * kVbl;  // the retrace wait
    }
    const double secs = static_cast<double>(t - t0) / 1e6;
    return Result{turns * kVbl / 1e6 / secs, drawn / secs, drawn, late, notLate};
}
}

int main() {
    int bad = 0;
    std::printf("%-24s %-22s %9s %10s %7s %6s %9s\n", "turn cost (ms)", "mode", "game speed", "pictures/s", "drawn", "late", "skip(wait)");
    const int64_t costs[][2] = {{6000, 3000}, {9000, 4000}, {12000, 4000}, {14000, 6000}, {16000, 6000}, {20000, 6000}, {26000, 8000}};
    for (const auto& c : costs) {
        Legacy legacy;
        port::pace::Controller fixed(false, port::pace::Policy::Fixed30);
        const Result a = Run([&](int64_t now) { return legacy.Turn(now); }, c[0], c[1]);
        const Result b = Run([&](int64_t now) { return fixed.Turn(static_cast<uint64_t>(now), true, false, 0, 0, 0); }, c[0], c[1]);
        char label[48];
        std::snprintf(label, sizeof label, "G %.0f + D %.0f (pair %.0f)", c[0] / 1000.0, c[1] / 1000.0, (2 * c[0] + c[1]) / 1000.0);
        std::printf("%-24s %-22s %8.0f %% %10.1f %7u %6u %9u\n", label, "Up to 60 (legacy rule)", a.speed * 100, a.pictures, a.drawn, a.late, a.notLate);
        std::printf("%-24s %-22s %8.0f %% %10.1f %7u %6u %9u\n", "", "30 / Steady 30 (fixed)", b.speed * 100, b.pictures, b.drawn, b.late, b.notLate);
        // A turn that costs less than a retrace for the game thread alone keeps the game at real time in both rules.
        if (c[0] + c[1] < kVbl) {
            if (a.speed < 0.995 || b.speed < 0.995) { std::printf("  FAIL: a light stretch slowed the game\n"); ++bad; }
        }
        // The fixed rule never presents more than 30 pictures a second.
        if (b.pictures > 30.5) { std::printf("  FAIL: fixed presented %.1f pictures a second\n", b.pictures); ++bad; }
    }
    std::printf("%s (%d failed)\n", bad ? "SOME CHECKS FAILED" : "all checks passed", bad);
    return bad ? 1 : 0;
}
