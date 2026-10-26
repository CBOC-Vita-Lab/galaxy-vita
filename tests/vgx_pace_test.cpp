#include "port/frame_pace.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

static void Run(port::pace::Controller& c, uint64_t& now, unsigned turns, uint64_t backend,
                bool quiet = false, bool wholeKnown = true) {
    for (unsigned i = 0; i < turns; ++i, now += 16683)
        c.Turn(now, true, quiet, 0, backend, wholeKnown ? backend : 0);
}

int main() {
    using port::pace::Policy;
    Require(port::pace::ResolveMode(30, 2, true) == 3, "fixed cap overrides adaptive verification");
    Require(port::pace::ResolveMode(30, 1, false) == 1, "unsupported cap retains the old adaptive path");
    Require(port::pace::ResolveMode(20, 0, true) == 0, "unsupported saved value does not enable pacing");
    for (bool required : {false, true}) {
        port::pace::Controller fixed(true, Policy::Fixed30);
        unsigned pictures = 0, eligible = 0;
        for (unsigned i = 0; i < 600; ++i) {
            const int d = fixed.Turn(1000000 + i * 16683ull, !required, false, 0, 22000);
            if (d == 0) { ++pictures; eligible += fixed.PresentAllowed(); }
            Require(fixed.Divisor() == 2, "fixed policy never selects 20");
        }
        Require(pictures == (required ? 600u : 300u) && eligible == 300,
                "600 turns retain required work with exactly 300 eligible pictures");
    }
    for (unsigned pattern = 0; pattern < 16; ++pattern) {
        port::pace::Controller fixed(true, Policy::Fixed30);
        bool previousHidden = false;
        uint64_t clock = 1000000;
        for (unsigned i = 0; i < 2400; ++i) {
            const bool allowed = ((i + pattern) % 3) != 0;
            const unsigned pending = (i + pattern) % 4;
            const bool gap = i % 97 == 0;
            clock += gap ? 300000 : (i % 7 == 0 ? 50000 : 16683);
            const int d = fixed.Turn(clock, allowed, i % 11 == 0, pending, 90000, 90000);
            Require(fixed.Divisor() == 2 && fixed.Changes() == 0, "quiet overload cannot select a slower fixed mode");
            if (d != 0) continue;
            const bool hidden = !fixed.PresentAllowed();
            Require(!hidden || !previousHidden, "no consecutive hidden emitted pictures across backlog/catchup");
            Require(!allowed || !hidden, "ordinary drawn turns are not gated a second time");
            Require(!gap || !hidden, "first drawn picture after gap is eligible");
            previousHidden = hidden;
        }
        fixed.Reset();
        Require(fixed.Turn(clock + 1, false, false, 0, 0) == 0 && fixed.PresentAllowed(), "reset promotes next draw");
    }
    port::pace::PresentationSlots resetSlots;
    resetSlots.Next(10, 2);
    resetSlots.Reset();
    Require(resetSlots.Next(11, 2) == 11, "slot reset discards stale debt");
    port::pace::Controller c(true);
    unsigned draws = 0, skips = 0;
    for (unsigned i = 0; i < 600; ++i) {
        const int decision = c.Turn(1000000 + i * 16683ull, true, false, 0, 22000);
        Require(decision != 2, "steady 60 Hz simulation never catches up unnecessarily");
        if (decision == 0) ++draws; else ++skips;
    }
    Require(draws == 300 && skips == 300, "steady state selects exactly 30 of 60 pictures");
    Require(c.Mismatches() == 0, "independent cadence oracle agrees in steady state");

    port::pace::Controller recovered(true);
    uint64_t now = 1000000;
    for (unsigned i = 0; i < 241; ++i, now += 16683) recovered.Turn(now, true, false, 0, 42000);
    for (unsigned i = 0; i < 180; ++i, now += 16683) recovered.Turn(now, true, false, 0, 22000);
    recovered.Turn(now, true, true, 0, 22000);
    Require(recovered.Divisor() == 2, "recovery cancels a stale overload before the next quiet moment");

    port::pace::PresentationSlots slots;
    Require(slots.Next(101, 2) == 101, "first ready picture becomes the display reference without extra latency");
    Require(slots.Next(102, 2) == 103, "early picture waits for two vblanks");
    Require(slots.Next(105, 2) == 105, "on-time picture needs no extra wait");
    Require(slots.Next(108, 2) == 108, "late picture shows at its earliest vblank, not one full cadence later");
    Require(slots.Next(109, 2) == 110, "late picture rebases the cadence without catch-up bursts");
    Require(slots.Next(111, 3) == 111, "quiet mode change resets the display reference");
    Require(slots.Next(112, 3) == 114, "20-picture mode holds each image for three vblanks");

    port::pace::Controller adaptive(true);
    now = 1000000;
    Run(adaptive, now, 360, 42000);
    Require(adaptive.Divisor() == 2, "sustained overload cannot switch visible gameplay");
    adaptive.Turn(now, true, true, 0, 42000); now += 16683;
    Require(adaptive.Divisor() == 3 && adaptive.Changes() == 1, "overload takes 20-picture fallback only at a quiet gate");
    Run(adaptive, now, 240, 22000);
    adaptive.Turn(now, true, true, 0, 22000, 22000); now += 16683;
    Require(adaptive.Divisor() == 3, "less than five seconds cannot reverse a mode change");
    Run(adaptive, now, 240, 22000);
    Require(adaptive.Divisor() == 3, "recovered gameplay waits for a quiet gate");
    adaptive.Turn(now, true, true, 0, 22000, 22000); now += 16683;
    Require(adaptive.Divisor() == 2 && adaptive.Changes() == 2, "sustained recovery restores 30 only while quiet");
    Require(adaptive.Mismatches() == 0, "cadence oracle agrees through both mode changes");

    port::pace::Controller unknown;
    now = 1000000;
    Run(unknown, now, 360, 42000);
    unknown.Turn(now, true, true, 0, 42000); now += 16683;
    Require(unknown.Divisor() == 3, "unknown-signal fixture reaches the emergency cadence");
    Run(unknown, now, 720, 12000, false, false);
    unknown.Turn(now, true, true, 0, 12000); now += 16683;
    Require(unknown.Divisor() == 3 && unknown.Changes() == 1,
            "low render CPU without GPU-inclusive work never permits recovery to 30");
    Run(unknown, now, 420, 22000);
    unknown.Turn(now, true, true, 0, 12000); now += 16683;
    Require(unknown.Divisor() == 3, "a newly unknown whole-picture signal vetoes an otherwise pending recovery");
    unknown.Turn(now, true, true, 0, 12000, 22000); now += 16683;
    Require(unknown.Divisor() == 3, "one valid quiet sample cannot resurrect recovery cancelled by unknown work");
    Run(unknown, now, 420, 22000);
    unknown.Turn(now, true, true, 0, 12000, 22000); now += 16683;
    Require(unknown.Divisor() == 2 && unknown.Changes() == 2,
            "recovery becomes eligible only with sustained GPU-inclusive headroom and a current valid signal");

    for (const uint64_t interruptedWork : {uint64_t(0), uint64_t(40000)}) {
        port::pace::Controller partial;
        now = 1000000;
        Run(partial, now, 360, 42000);
        partial.Turn(now, true, true, 0, 42000); now += 16683;
        Run(partial, now, 420, 22000);
        partial.Turn(now, true, false, 0, 12000, interruptedWork); now += 16683;
        partial.Turn(now, true, true, 0, 12000, 22000); now += 16683;
        Require(partial.Divisor() == 3, "bad or unknown work in the current partial window cancels pending recovery");
    }

    port::pace::Controller slowGpu;
    now = 1000000;
    Run(slowGpu, now, 360, 42000);
    slowGpu.Turn(now, true, true, 0, 42000); now += 16683;
    for (unsigned i = 0; i < 720; ++i, now += 16683) slowGpu.Turn(now, true, false, 0, 12000, 40000);
    slowGpu.Turn(now, true, true, 0, 12000, 40000);
    Require(slowGpu.Divisor() == 3, "a 40 ms GPU picture cannot recover based on a 12 ms CPU submission");

    port::pace::Controller spike(true);
    now = 1000000;
    Run(spike, now, 120, 22000);
    Run(spike, now, 20, 90000);
    Run(spike, now, 300, 22000, true);
    Require(spike.Divisor() == 2 && spike.Changes() == 0, "one short load spike never chooses 20");

    port::pace::Controller noise;
    now = 1000000;
    for (unsigned i = 0; i < 30; ++i) {
        Run(noise, now, 60, i % 2 ? 22000 : 42000);
        noise.Turn(now, true, true, 0, 22000); now += 16683;
    }
    Require(noise.Divisor() == 2 && noise.Changes() == 0, "alternating noisy load does not oscillate modes");

    port::pace::Controller load;
    now = 1000000;
    Run(load, now, 360, 42000);
    now += 2000000;
    Require(load.Turn(now, true, true, 0, 22000) == 0, "long load resets catch-up debt and starts with a picture");
    Require(load.Divisor() == 2, "a load invalidates stale overload from the previous scene");

    port::pace::Controller catchup(true);
    now = 1000000;
    Run(catchup, now, 60, 22000);
    now += 50000;
    Require(catchup.Turn(now, true, false, 0, 22000) == 2, "a late turn uses no-wait catch-up, not slowed game time");
    now += 10000;
    Require(catchup.Turn(now, false, false, 0, 22000) == 0, "required transition draws even while late");
    now += 16683;
    Require(catchup.Turn(now, true, false, 2, 22000) == 1, "a full pipeline suppresses drawing without advancing simulation early");
    Require(catchup.Mismatches() == 0, "oracle agrees across catch-up, required drawing and backlog");

    port::pace::PresentationSlots wrap;
    Require(wrap.Next(0xfffffffeu, 2) == 0xfffffffeu, "display reference may be near counter wrap");
    Require(wrap.Next(0xffffffffu, 2) == 0, "two-vblank cadence crosses unsigned counter wrap");
    wrap.Shown(1);
    Require(wrap.Next(2, 2) == 3, "a wait overshoot rebases on the actual shown vblank");
    std::puts("PASS: simulated clocks: steady 30, spike, recovery, quiet gates, no oscillation, catch-up, backlog, counter wrap, cadence oracle");
}
