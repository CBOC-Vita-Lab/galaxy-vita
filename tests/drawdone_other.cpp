// GXV_DRAWDONE_OTHER (platform/include/port/drawdone_other.h): a draw-done wait asked for by the LOADING thread.
//
// A deterministic model of the guest CPU (one thread at a time, by priority; interrupts delivered at the running thread's safe points
// or at once when the CPU idles), of GXMisc.c's one-bit DrawDone flag with its FinishQueue (a finish wakes every sleeper), of the
// game thread's turns (the real port::drawdone::Deferral, its settle points 1, 2, 3 and 0, GXSetDrawDone then Arm at the end of a
// drawn turn, the retrace sleep) and of the loading thread (priority below the game thread's) building a stage: every model it makes
// writes its view matrices once, and such a write waits for the front end while the game thread's draw-done is pending.
//
// The front end's finishes come (a) in step with the game's turns, during the game thread's next turn (60 pictures a second, every
// turn drawn: the console's closed screen of a load in Performance mode), (b) only after the game thread has started waiting for them
// (the front end a picture behind), (c) while the game thread sleeps (an idle front end), or (d) at random among these, with 0-2 turns
// of lag. The checks:
//   - the 2.2 / r110 behaviour (the loading thread takes the game thread's wait and sleeps for the flag) starves the loading thread
//     for good under (a) and (b) at 60, and gets through under the fixed 30 cap (turns without a picture): the console's issue 3;
//   - the fix (the loading thread waits for the outstanding finish by the count of GXSetDrawDone calls and leaves the game thread's
//     deferral alone) gets through under every pattern, mode and cadence, never returns before the finish that was outstanding when it
//     asked, and the game thread never issues a GXSetDrawDone with an earlier finish still outstanding (the old way did).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <deque>
#include <random>

#include "port/drawdone_defer.h"
#include "port/drawdone_other.h"

using port::drawdone::Deferral;

static void Require(bool ok, const char* why) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", why);
        abort();
    }
}

enum Pattern { kEarly, kAtWait, kIdle, kRandom };
enum Kind { kDuringTurn, kWhenWaited, kWhileAsleep };

struct Due {
    uint64_t vblank;
    Kind kind;
};

struct Sim {
    Sim(int mode, bool fixedOther, Pattern pattern, bool cap30, uint32_t seed) : d(mode), fix(fixedOther), pat(pattern), cap(cap30), rng(seed) {}
    Deferral d;
    bool fix;
    Pattern pat;
    bool cap;
    std::mt19937 rng;
    // GXMisc.c
    bool flag = true;
    uint32_t sets = 0, raised = 0;
    std::deque<Due> due;  // the front end's outstanding finishes, in order
    unsigned twoOutstanding = 0;
    // the loading thread
    static constexpr unsigned kWrites = 40;
    unsigned written = 0;
    bool started = false, waiting = false, asleep = false;
    uint32_t ticket = 0, mustSee = 0;
    uint64_t now = 0;

    void Raise() {  // the finish interrupt: the flag, then every FinishQueue sleeper made ready
        Require(!due.empty(), "a finish with none outstanding");
        due.pop_front();
        ++raised;
        flag = true;
        asleep = false;  // (OSWakeupThread: every sleeper of the queue made ready)
    }
    bool HeadDue(Kind kind) const {
        if (due.empty()) return false;
        const Due& h = due.front();
        if (h.kind == kWhenWaited) return false;  // (only a wait of the game thread brings it: the front end a picture behind)
        return h.vblank < now || (h.vblank == now && h.kind == kind);
    }
    void Deliver(Kind kind) { while (HeadDue(kind)) Raise(); }

    // The loading thread's state: runnable unless asleep in the FinishQueue (or done).
    bool Runnable() const { return started && written < kWrites && !asleep; }
    // The loading thread gets the CPU (the game thread sleeps or waits): ONE model's view pass a slice (it builds the model between two;
    // the rest of the slice is that work), or the end of the wait it is in.
    void LoaderRun() {
        if (!Runnable()) return;
        if (!waiting) {
            // its next model's view pass (port_gx_view_write_from; ModelManager::updateDL's settle(3) in modes 1-3)
            const bool need = d.Mode() >= 3 ? d.Pending() : d.WouldWait(port::drawdone::kDrawnTurn);
            if (!need) { ++written; return; }
            mustSee = flag ? 0 : sets;
            if (fix) {
                ticket = sets;  // (GXMisc.c port_gx_wait_finish_other; the game thread's deferral is not touched)
            } else {
                Require(d.MustWait(d.Mode() >= 3 ? port::drawdone::kSetDrawDone : port::drawdone::kDrawnTurn), "old path: a wait");
            }
            waiting = true;
        }
        const bool over = fix ? port_drawdone_other_seen(flag, sets, ticket) != 0 : flag;
        if (!over) { asleep = true; return; }
        if (fix && mustSee) Require(raised >= mustSee, "the loading thread returned before the finish outstanding when it asked");
        waiting = false;
        ++written;
    }
    // The game thread at a settle point: a wait it must make sleeps for the flag; meanwhile the CPU is the loading thread's (if it is
    // runnable), then the front end's finish comes; the game thread (higher priority) runs first after it.
    void GameSettle(int point) {
        if (!d.MustWait(point)) return;
        while (!flag) {
            LoaderRun();
            Require(!due.empty(), "the game thread waits for a finish never issued");
            Raise();
        }
    }
    void SetDrawDone() {
        GameSettle(port::drawdone::kSetDrawDone);
        if (!due.empty()) ++twoOutstanding;
        flag = false;
        ++sets;
        uint64_t lag = 1;
        Kind kind = kDuringTurn;
        switch (pat) {
            case kEarly: lag = 1; kind = kDuringTurn; break;
            case kAtWait: lag = 1; kind = kWhenWaited; break;
            case kIdle: lag = 0; kind = kWhileAsleep; break;
            case kRandom:
                lag = rng() % 3;
                kind = static_cast<Kind>(rng() % 3);
                if (lag == 0) kind = kWhileAsleep;  // (this turn: only once the game thread sleeps)
                break;
        }
        due.push_back({now + lag, kind});
    }
    void Turn() {
        const bool drawn = !cap || (now & 1) == 0;
        Deliver(kDuringTurn);  // (raised while the game thread runs: taken at its next safe point, it keeps the CPU)
        GameSettle(port::drawdone::kUpdate);
        GameSettle(port::drawdone::kCalcAnim);
        if (drawn) {
            GameSettle(port::drawdone::kDrawnTurn);
            if (d.Mode() == 0) {
                SetDrawDone();
                GameSettle(port::drawdone::kSetDrawDone);  // (mode 0: GXDrawDone waits at once)
                while (!flag) { LoaderRun(); Raise(); }
            } else {
                SetDrawDone();
                d.Arm();
            }
        }
        // The game thread waits for the next retrace: the CPU idles, finishes come at once, the loading thread runs.
        Deliver(kWhileAsleep);
        LoaderRun();
        Deliver(kWhileAsleep);
        LoaderRun();
    }
    // Returns the vblank the loading thread finished at, or 0 if it had not after `limit` turns.
    uint64_t Run(uint64_t limit) {
        for (now = 1; now <= limit; ++now) {
            if (now == 10) started = true;
            Turn();
            if (written == kWrites) return now;
        }
        return 0;
    }
};

static uint64_t RunOne(int mode, bool fix, Pattern p, bool cap, uint32_t seed, unsigned* two = nullptr, unsigned* overlaps = nullptr) {
    Sim s(mode, fix, p, cap, seed);
    const uint64_t at = s.Run(3000);
    if (two) *two = s.twoOutstanding;
    if (overlaps) *overlaps = s.d.Overlaps();
    return at;
}

int main() {
    // The console's case: Performance mode (draw-done mode 4, "Max"; 2 is the default), every turn drawn, finishes in step with turns.
    for (int mode : {2, 4}) {
        unsigned two = 0;
        Require(RunOne(mode, false, kEarly, false, 1) == 0, "old: finishes during the game's turn at 60 starve the loading thread");
        Require(RunOne(mode, false, kAtWait, false, 1, &two) == 0, "old: finishes the game thread waits for at 60 starve the loading thread");
        Require(two > 0, "old: the game thread issued a GXSetDrawDone with a finish outstanding");
        Require(RunOne(mode, false, kEarly, true, 1) != 0, "old: under the 30 cap the loading thread gets through");
        Require(RunOne(mode, false, kIdle, false, 1) != 0, "old: an idle front end lets the loading thread through");
        printf("old (mode %d): starved at 60 (finishes during the turn / waited for), through at 30 and with an idle front end\n", mode);
    }
    // The fix, every mode, cadence and pattern, and 400 random runs each.
    unsigned runs = 0;
    uint64_t worst = 0;
    for (int mode = 1; mode <= 4; ++mode) {
        for (bool cap : {false, true}) {
            for (Pattern p : {kEarly, kAtWait, kIdle}) {
                unsigned two = 1, ov = 1;
                const uint64_t at = RunOne(mode, true, p, cap, 7, &two, &ov);
                Require(at != 0, "fix: the loading thread finishes");
                Require(two == 0 && ov == 0, "fix: never two finishes outstanding");
                worst = at > worst ? at : worst;
                ++runs;
            }
            for (uint32_t seed = 1; seed <= 400; ++seed) {
                unsigned two = 1, ov = 1;
                const uint64_t at = RunOne(mode, true, kRandom, cap, seed, &two, &ov);
                Require(at != 0, "fix: the loading thread finishes (random finishes)");
                Require(two == 0 && ov == 0, "fix: never two finishes outstanding (random finishes)");
                worst = at > worst ? at : worst;
                ++runs;
            }
        }
    }
    Require(worst < 10 + 3 * Sim::kWrites + 10, "fix: at most a few turns a write");
    // The predicate itself.
    Require(port_drawdone_other_seen(1, 5, 5) && port_drawdone_other_seen(0, 6, 5) && !port_drawdone_other_seen(0, 5, 5), "predicate");
    Require(port_drawdone_other_parse(nullptr) && port_drawdone_other_parse("1") && !port_drawdone_other_parse("0") &&
                port_drawdone_other_parse("01") && port_drawdone_other_parse(""), "switch");
    printf("fix: %u runs (modes 1-4, 60 and 30, every pattern), the loading thread always through (worst at turn %llu of 3000), "
           "never early, never two finishes outstanding\n", runs, static_cast<unsigned long long>(worst));
    return 0;
}
