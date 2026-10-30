// A model, not a console measurement: what the picture-rate logic does to a CPU-bound stretch, with the real pacing controller
// (port::pace::Controller, Fixed30 policy: frame_pace.h) and a copy of the legacy frame-skip rule (port/perf.cpp
// port_frameskip_turn, PETARI_FRAMESKIP default 3 on a handheld). The game thread's turn costs G ms (update + calcAnim, paid by every turn
// whether it draws or not) plus D ms more on a turn that draws; the turn then waits for the next retrace unless it was a catch-up
// skip (decision 2). Retraces come every 16.683 ms. The GPU and the render threads are taken as keeping up (no pending pictures),
// which is the CPU-bound case: the renderer's work shows only as D. For each (G, D) the table gives, over 30 s of model time, the
// game's speed (turns done against real time; 100 % = 60 turns a second), the pictures a second, and the turns that drew, skipped
// to catch up (no retrace wait) and skipped without lateness.
//
// GXV_SLIP (second table, below the first): the same for the bounded slip (frame_pace.h Slip, with the real LegacySkip rule for "Up to 60" and
// the real Controller for the fixed 30), with the retrace wait of the port. The first table waits for the next retrace after every
// turn; the port's wait (MainLoopFramework waitForTick on JUTVideo's one-message queue) does not wait for a retrace that has already
// passed, so the second table waits that way, and repeats one row with the first table's wait for comparison.
//   tools/test_pace_cpubound_model.sh
#include "port/frame_pace.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
constexpr int64_t kVbl = 16683;      // us
constexpr int64_t kEnd = 30000000;   // 30 s

struct Result { double speed, pictures; unsigned drawn, late, notLate; };

// The legacy rule of port_frameskip_turn with maxRun 3 and no pending pictures (the GPU not behind): port::pace::LegacySkip, which
// perf.cpp runs.
struct Legacy {
    port::pace::LegacySkip rule{3, 2, 6};
    int Turn(int64_t now) { return rule.Turn(now * 1000, true, 0).code; }
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

// ---- The slip (GXV_SLIP), second table ------------------------------------------------------------------------------------
// The port's retrace wait: JUTVideo's message queue holds one retrace (a later one is dropped while it is full), and
// MainLoopFramework waitForTick takes a message whose count is not under the last one taken plus 1, blocking for the next retrace when
// there is none. A turn that ran past a retrace therefore waits for nothing. strict: the first table's wait, the next retrace after the
// turn whatever has passed.
struct Queue {
    int64_t posted = 0, msg = 0, next = 0;
    bool full = false;
    explicit Queue(int64_t t) { posted = next = t / kVbl; }
    void Post(int64_t t) {
        for (const int64_t c = t / kVbl; posted < c;) {
            ++posted;
            if (!full) { full = true; msg = posted; }
        }
    }
    int64_t Wait(int64_t t) {
        Post(t);
        for (;;) {
            if (!full) { t = (posted + 1) * kVbl; Post(t); }
            const int64_t m = msg;
            full = false;
            if (m >= next) { next = m + 1; return t; }
        }
    }
};

// G ms every turn, D ms more on a turn that draws; after sw seconds of model time the costs become G2 and D2.
struct Costs { int64_t g, d, g2, d2; double sw; };

struct SlipResult { double speed, pictures; unsigned drawn; bool engagedAtEnd; unsigned changes; double lagMs; };

// mode 0: Up to 60 (LegacySkip), 1: Locked 30 (Controller Fixed30). Warm-up seconds are not counted. The speed and the pictures are
// taken over the last `tail` seconds.
SlipResult RunSlip(int mode, bool slipOn, unsigned capPct, Costs c, bool strict, double secs = 30.0, double tail = 20.0) {
    port::pace::Slip slip;
    if (slipOn) slip.Configure(capPct);
    port::pace::LegacySkip legacy(3, 2, 6, &slip);
    port::pace::Controller fixed(false, port::pace::Policy::Fixed30);
    fixed.UseSlip(&slip);
    int64_t t = 1000000;
    Queue q(t);
    const int64_t t0 = t, tEnd = t0 + static_cast<int64_t>(secs * 1e6), tMeasure = tEnd - static_cast<int64_t>(tail * 1e6);
    int64_t tStart = 0;
    unsigned turns = 0, drawn = 0;
    while (t < tEnd) {
        const bool second = t - t0 >= static_cast<int64_t>(c.sw * 1e6);
        const int64_t g = second ? c.g2 : c.g, d = second ? c.d2 : c.d;
        slip.Begin(static_cast<uint64_t>(t), true);
        const int dec = mode == 0 ? legacy.Turn(t * 1000, true, 0).code : fixed.Turn(static_cast<uint64_t>(t), true, false, 0, 0, 0);
        if (!tStart && t >= tMeasure) { tStart = t; turns = drawn = 0; }
        ++turns;
        t += g;
        if (dec == 0) { ++drawn; t += d; }
        slip.End(static_cast<uint64_t>(t), dec == 0);
        if (dec != 2) t = strict ? (t / kVbl + 1) * kVbl : q.Wait(t);
        else q.Post(t);
    }
    const double sec = static_cast<double>(t - tStart) / 1e6;
    return SlipResult{turns * kVbl / 1e6 / sec, drawn / sec, drawn, slip.Engaged(), slip.Changes(), slip.LagUs() / 1000.0};
}

int SlipTable() {
    int bad = 0;
    std::printf("\nGXV_SLIP: bounded slip on a CPU-bound stretch (retrace wait of the port; last 20 of 30 s; cap 15 %%)\n");
    std::printf("%-24s %-14s %-9s %9s %10s   %s\n", "turn cost (ms)", "mode", "slip", "game speed", "pictures/s", "");
    const int64_t splits[] = {6000, 3000, 10000};  // D; G follows from the pair
    const int64_t pairs[] = {34000, 36000, 38000, 40000};
    for (const int64_t d : splits) {
        for (const int64_t pair : pairs) {
            const int64_t g = (pair - d) / 2;
            char label[48];
            std::snprintf(label, sizeof label, "G %.1f + D %.0f (pair %.0f)", g / 1000.0, d / 1000.0, pair / 1000.0);
            for (int mode = 0; mode < 2; ++mode) {
                const SlipResult off = RunSlip(mode, false, 15, {g, d, g, d, 1e9}, false);
                const SlipResult on = RunSlip(mode, true, 15, {g, d, g, d, 1e9}, false);
                const char* name = mode == 0 ? "Up to 60" : "Locked 30";
                std::printf("%-24s %-14s %-9s %8.1f %% %10.1f\n", mode == 0 ? label : "", name, "off", off.speed * 100, off.pictures);
                std::printf("%-24s %-14s %-9s %8.1f %% %10.1f   %s\n", "", name, "on", on.speed * 100, on.pictures,
                            on.engagedAtEnd ? "engaged" : "not engaged (the skip rule as before)");
                const double slowdown = 1.0 - 2.0 * kVbl / pair;
                if (slowdown <= 0.15) {
                    // Inside the cap: the speed is what the pair allows (2R / P; a little under when a short turn ends before its retrace and
                    // waits for it), the pictures one per pair, and more than the skip rule gave.
                    if (!on.engagedAtEnd) { std::printf("  FAIL: slip not engaged inside the cap\n"); ++bad; }
                    if (on.speed > 2.0 * kVbl / pair + 0.005 || on.speed < 2.0 * kVbl / pair - 0.03) { std::printf("  FAIL: slip speed %.3f, expected %.3f\n", on.speed, 2.0 * kVbl / pair); ++bad; }
                    if (on.pictures > 60.0 * kVbl / pair + 0.5 || on.pictures < 60.0 * kVbl / pair - 1.5) { std::printf("  FAIL: slip pictures %.1f\n", on.pictures); ++bad; }
                    if (!(on.pictures > off.pictures + 0.5)) { std::printf("  FAIL: slip gave no more pictures than the skip rule\n"); ++bad; }
                } else {
                    // Over the cap: today's skipping, exactly.
                    if (on.engagedAtEnd || on.changes != 0 || on.speed != off.speed || on.drawn != off.drawn) {
                        std::printf("  FAIL: over the cap the slip changed the result\n");
                        ++bad;
                    }
                }
            }
        }
    }

    // The headline: a pair of 34 ms, 29 pictures a second at 98 % speed.
    {
        const SlipResult a = RunSlip(1, true, 15, {14000, 6000, 14000, 6000, 1e9}, false);
        const SlipResult b = RunSlip(0, true, 15, {14000, 6000, 14000, 6000, 1e9}, false);
        std::printf("\n34 ms pair, slip on: Locked 30 %.1f pictures/s at %.1f %%; Up to 60 %.1f pictures/s at %.1f %%\n", a.pictures, a.speed * 100, b.pictures, b.speed * 100);
        for (const SlipResult& r : {a, b}) {
            if (r.pictures < 28.8 || r.pictures > 30.0 || r.speed < 0.975 || r.speed > 0.99) { std::printf("  FAIL: 34 ms pair is not about 29 pictures at 98 %%\n"); ++bad; }
        }
    }

    // The cap: 12 % needed; a cap of 10 falls back to skipping, a cap of 12 or more slips.
    {
        const Costs c{16000, 6000, 16000, 6000, 1e9};
        const SlipResult tight = RunSlip(1, true, 10, c, false), off = RunSlip(1, false, 10, c, false), ok = RunSlip(1, true, 13, c, false);
        std::printf("38 ms pair (12.2 %% slowdown): cap 10 -> %.1f pictures/s at %.1f %% (%s); cap 13 -> %.1f pictures/s at %.1f %%\n", tight.pictures, tight.speed * 100,
                    tight.engagedAtEnd ? "slip" : "skipping", ok.pictures, ok.speed * 100);
        if (tight.engagedAtEnd || tight.speed != off.speed || tight.drawn != off.drawn || !ok.engagedAtEnd) { std::printf("  FAIL: the cap does not bound the slip\n"); ++bad; }
    }

    // A stretch that fits, or that the skip rule serves as well, does not engage the slip and is not touched by it.
    {
        const Costs light[] = {{12000, 4000, 12000, 4000, 1e9}, {14000, 4000, 14000, 4000, 1e9}, {5000, 25000, 5000, 25000, 1e9}, {9000, 4000, 9000, 4000, 1e9}};
        for (const Costs& c : light) {
            for (int mode = 0; mode < 2; ++mode) {
                const SlipResult off = RunSlip(mode, false, 15, c, false), on = RunSlip(mode, true, 15, c, false);
                std::printf("G %lld + D %lld (pair %lld): %s slip %s, %.1f vs %.1f pictures/s, %.1f vs %.1f %%\n", static_cast<long long>(c.g / 1000), static_cast<long long>(c.d / 1000),
                            static_cast<long long>((2 * c.g + c.d) / 1000), mode == 0 ? "Up to 60" : "Locked 30", on.changes ? "ENGAGED" : "not engaged", on.pictures, off.pictures,
                            on.speed * 100, off.speed * 100);
                if (on.changes != 0 || on.speed != off.speed || on.drawn != off.drawn) { std::printf("  FAIL: the slip changed a stretch it should leave alone\n"); ++bad; }
            }
        }
    }

    // It engages for a sustained load and leaves when the load goes: 34 ms pairs for 12 s, then light turns (G 9 + D 4); the game is back at 100 % in the last 5 s.
    for (int mode = 0; mode < 2; ++mode) {
        const SlipResult r = RunSlip(mode, true, 15, {14000, 6000, 9000, 4000, 12.0}, false, 30.0, 5.0);
        std::printf("%s: 34 ms pairs for 12 s, then light: last 5 s %.1f pictures/s at %.1f %%; changes %u, behind real time %.0f ms (not caught up)\n",
                    mode == 0 ? "Up to 60" : "Locked 30", r.pictures, r.speed * 100, r.changes, r.lagMs);
        if (r.engagedAtEnd || r.changes != 2 || r.speed < 0.995 || r.speed > 1.005 || r.pictures < (mode == 0 ? 55.0 : 29.5)) { std::printf("  FAIL: the slip did not engage and leave\n"); ++bad; }
    }
    // One hitch is not a sustained load.
    {
        port::pace::Slip slip;
        slip.Configure(15);
        uint64_t t = 1000000;
        for (int i = 0; i < 400; ++i) {
            slip.Begin(t, true);
            const bool drew = (i & 1) == 0;
            t += drew ? 20000 : 10000;
            if (i == 150) t += 60000;  // one turn of 80 ms
            slip.End(t, drew);
            t += 1000;
        }
        std::printf("a single 80 ms turn among light ones: %u changes\n", slip.Changes());
        if (slip.Changes() != 0) { std::printf("  FAIL: one hitch engaged the slip\n"); ++bad; }
    }

    // The first table's wait (the next retrace after every turn), for one stretch: the slip is not for that wait.
    {
        const Costs c{14000, 6000, 14000, 6000, 1e9};
        for (int mode = 0; mode < 2; ++mode) {
            const SlipResult off = RunSlip(mode, false, 15, c, true), on = RunSlip(mode, true, 15, c, true);
            std::printf("34 ms pair, next-retrace wait of the first table, %s: slip off %.1f pictures/s at %.1f %%, on %.1f at %.1f %% (engaged %u times; not what the port does)\n",
                        mode == 0 ? "Up to 60" : "Locked 30", off.pictures, off.speed * 100, on.pictures, on.speed * 100, on.changes / 2);
            // The measured speed keeps the cap whatever the wait is: the slip leaves when the clock shows the game slower than the cap allows.
            if (on.speed < 0.85) { std::printf("  FAIL: with the other wait the slip took the game under the cap\n"); ++bad; }
        }
    }
    return bad;
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
    bad += SlipTable();
    std::printf("%s (%d failed)\n", bad ? "SOME CHECKS FAILED" : "all checks passed", bad);
    return bad ? 1 : 0;
}
