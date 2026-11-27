// par25 (12 Nov 2026): GXV_PAR25, the net-benefit ledger and guard of the game thread's parallel workers (par-anim, par-ptcl, par-dl; the
// par-view / par-plant / par-flush lines are reported). Pure logic: no game types, no clocks, no threads of its own (host-tested by
// tests/par25.cpp, ThreadSanitizer included). The glue is platform/src/port/perf.cpp (the postings, the tick, the accessors, the log line).
//
// WHY. Every worker that takes work off the game thread costs the game thread something too: the making of its jobs (par-anim's dispatch),
// the join's wait and the merge (par-dl), the wait for a descheduled worker (par-ptcl). The workers' own 5 s lines show both halves, but
// they are four lines in four formats and nobody adds them up. From the Honeyhive logs (c76-tourK2, GXV_PAR_ANIM=1 default build):
// par-anim "saved 0.142 ms a batch (walks 0.174 - copies 0.032), dispatch 0.230" is a NET LOSS of 0.088 ms a turn; par-ptcl's worker is busy
// 0.04-0.17 ms a turn and its joins wait up to 0.48 ms on the parallel turns; par-dl's verify run (c77-hL) shows "saved -5.7 ms" with the
// verify's own second computation inside the merge, so its real net is unknown until a run without verify. A worker that loses should be
// off, and one that wins should be on, without a console iteration per stage.
//
// WHAT. Each worker posts its 5-second window as a Summary (game-thread ms that the jobs replaced = gross; game-thread ms the offload cost =
// cost; both over the window). net = gross - cost, in ms per second of game time (60 turns a second: 1 ms/s = 0.017 ms a turn). The Judge
//   - latches OFF for the session when a window reports verify mismatches (the serial result is what the worker's own verify keeps; the
//     latch only stops asking the worker again). Not exact-by-construction when no verify runs: this is the one place the guard is a safety
//     net and not a scheduler;
//   - goes OFF after GXV_PAR25_NEG_WINDOWS (2) judged windows in a row whose net is below -GXV_PAR25_NEG_MS (3 ms/s = 0.05 ms a turn), stays
//     off GXV_PAR25_PROBE (12) ticks (5 s each: 60 s), then is ON again on probation: one more negative window sends it off again for TWICE
//     as long (12, 24, 48, 96, 120 ticks: no limit cycle, as cpugov.h); GXV_PAR25_GOOD (3) good windows in a row on probation clear the backoff.
// A summary can be an UPPER BOUND (bound = true: the gross is the worker's own time, an upper bound of what the game thread would have
// spent; the cost is what could be measured). A bounded net that is already negative is negative for certain, so the Judge uses it; a bounded
// net is never counted as a proven saving in the total (the line prints it apart).
//
//   GXV_PAR25=0|1|2   0 off (default: nothing is posted, nothing is gated, no line); 1 guard (the Judge's verdicts gate the workers);
//                     2 observe (the line and the verdicts are logged as "would", nothing is gated: the first console run)
//   GXV_PAR25_NEG_MS (3) GXV_PAR25_NEG_WINDOWS (2) GXV_PAR25_PROBE (12) GXV_PAR25_GOOD (3) GXV_PAR25_LOG (1: a line every tick; n: every n)
// Gating: platform/src/port/perf.cpp port_par_anim_switch / port_par_anim_on, port_par_dl_mode, port_par_ptcl_mode AND the verdict. The
// verdicts are applied at the start of a turn (port_perf_frame_begin), never between a worker's two halves of one turn.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace port {
namespace par25 {

enum Worker { kAnim = 0, kPtcl, kDl, kView, kPlant, kFlush, kWorkers };
inline const char* Name(int w) {
    static const char* const n[kWorkers] = {"anim", "ptcl", "dl", "view", "plant", "flush"};
    return w >= 0 && w < kWorkers ? n[w] : "?";
}
// the workers the Judge may switch (the others are reported only: a switch inside a picture's view pass is not a scheduling choice)
inline bool Gated(int w) { return w == kAnim || w == kPtcl || w == kDl; }

struct Summary {
    double secs = 0;        // the window's length
    double turns = 0;       // game turns (batches, pictures) it holds
    double jobs = 0;        // jobs the worker ran
    double workerMs = 0;    // the worker's busy time over the window
    double grossMs = 0;     // game-thread ms the jobs replaced over the window
    double costMs = 0;      // game-thread ms the offload cost over the window (making, copies, waits, merges)
    unsigned mismatches = 0;  // verify mismatches in the window
    bool bound = false;     // grossMs is an upper bound
    bool judged = true;     // false: report only (a verify run's merge holds the verify's own second computation)
};
inline double NetMs(const Summary& s) { return s.grossMs - s.costMs; }
inline double NetPerSec(const Summary& s) { return s.secs > 0.0 ? NetMs(s) / s.secs : 0.0; }

struct Params {
    double negMsPerSec = 3.0;
    int negWindows = 2;
    int probeTicks = 12;
    int goodWindows = 3;
    int maxProbeTicks = 120;
};

enum Verdict { kOn = 0, kOff, kLatched };
struct Judge {
    Verdict st = kOn;
    int negRun = 0, goodRun = 0, offLeft = 0, backoff = 0;
    bool probation = false;
    unsigned lowerings = 0;  // times it went off (not counting the latch)
    // One posted window. Returns true if the verdict changed.
    bool Window(const Summary& s, const Params& p) {
        if (st == kLatched) return false;
        if (s.mismatches != 0) {
            st = kLatched;
            return true;
        }
        if (st == kOff || !s.judged) return false;
        const bool neg = NetPerSec(s) < -p.negMsPerSec;
        if (neg) {
            goodRun = 0;
            ++negRun;
            if (negRun >= p.negWindows || probation) {
                Lower(p);
                return true;
            }
        } else {
            negRun = 0;
            if (probation && ++goodRun >= p.goodWindows) {
                probation = false;
                backoff = 0;
            }
        }
        return false;
    }
    // One tick (every 5 s of wall clock). Returns true if the verdict changed.
    bool Tick(const Params& p) {
        (void)p;
        if (st != kOff) return false;
        if (--offLeft > 0) return false;
        st = kOn;
        probation = true;
        goodRun = negRun = 0;
        return true;
    }
    bool Allowed() const { return st == kOn; }

private:
    void Lower(const Params& p) {
        st = kOff;
        ++lowerings;
        long ticks = static_cast<long>(p.probeTicks) << (backoff < 8 ? backoff : 8);
        if (ticks > p.maxProbeTicks) ticks = p.maxProbeTicks;
        offLeft = static_cast<int>(ticks);
        if (backoff < 8) ++backoff;
        negRun = goodRun = 0;
        probation = false;
    }
};

// The one-line report. Totals: the proven part (summaries that are not bounds) and the bounded part, in ms per second and per turn.
struct Totals {
    double jobs = 0, workerMsPerSec = 0, provenPerSec = 0, boundedPerSec = 0, mismatches = 0;
};
inline Totals Sum(const Summary* s, const Verdict* v, const bool* fresh, double enabledMask) {
    (void)enabledMask;
    Totals t;
    for (int w = 0; w < kWorkers; ++w) {
        if (!fresh[w]) continue;
        t.jobs += s[w].jobs;
        t.mismatches += s[w].mismatches;
        if (s[w].secs > 0.0) t.workerMsPerSec += s[w].workerMs / s[w].secs;
        if (v[w] == kOn) (s[w].bound ? t.boundedPerSec : t.provenPerSec) += NetPerSec(s[w]);
    }
    return t;
}
// "par25: 5.0 s, 300 turns; jobs 3260, worker 2.9 ms/s; game thread saved est +4.1 ms/s (0.068 ms a turn) proven, up to +2.0 ms/s bounded (ptcl); mismatches 0; ..."
inline int Format(char* out, unsigned cap, double secs, double turns, int mode, const Summary* s, const Judge* j, const bool* fresh) {
    Verdict v[kWorkers];
    for (int w = 0; w < kWorkers; ++w) v[w] = j[w].st;
    const Totals t = Sum(s, v, fresh, 0.0);
    const double tps = secs > 0.0 ? turns / secs : 0.0;
    int n = snprintf(out, cap, "par25: %.1f s, %.0f turns (mode %d: %s); jobs %.0f, worker %.2f ms/s; game thread saved est %+.2f ms/s (%+.3f ms a turn) proven, "
                     "up to %+.2f ms/s more from bounded estimates; mismatches %.0f; per worker (net ms/s, a turn):",
                     secs, turns, mode, mode == 1 ? "guard" : mode == 2 ? "observe" : "off", t.jobs, t.workerMsPerSec, t.provenPerSec,
                     tps > 0.0 ? t.provenPerSec / tps : 0.0, t.boundedPerSec, t.mismatches);
    for (int w = 0; w < kWorkers && n > 0 && static_cast<unsigned>(n) < cap; ++w) {
        if (!fresh[w]) {
            n += snprintf(out + n, cap - n, " %s idle;", Name(w));
            continue;
        }
        const double np = NetPerSec(s[w]);
        n += snprintf(out + n, cap - n, " %s %s %+.2f%s (%+.3f)%s%s;", Name(w), j[w].st == kOn ? "on" : j[w].st == kOff ? "OFF" : "LATCHED", np,
                      s[w].bound ? "<=" : "", tps > 0.0 ? np / tps : 0.0, s[w].judged ? "" : " (not judged)",
                      j[w].st == kOff ? "" : (j[w].probation ? " (probation)" : ""));
    }
    return n;
}

// The verdicts the workers read: one relaxed byte each, written by the game thread at the start of a turn.
struct Gate {
    std::atomic<uint8_t> allow[kWorkers];
    Gate() {
        for (auto& a : allow) a.store(1, std::memory_order_relaxed);
    }
    bool Allowed(int w) const { return allow[w].load(std::memory_order_relaxed) != 0; }
    void Set(int w, bool on) { allow[w].store(on ? 1 : 0, std::memory_order_relaxed); }
};

}  // namespace par25
}  // namespace port
