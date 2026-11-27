// hitch24b (GXV_HITCH_LOG, default 1): the always-on "hitch:" ledger. Header only, no dependencies beyond the standard library, so the game thread, the
// render thread, the platform layer and the host tests all use the same code.
//
//  1. The JOB JOURNAL. Every periodic or occasional job that can hold a hot thread (the stage queue's build, the stage list's file append, the C heap's
//     30 s census, the shader reclaim, the render-thread housekeeping ...) notes itself here when it ran: name, start, end. A note is two clock reads and
//     four relaxed stores, made only when the job runs (never per draw). The hitch line asks "which journaled job overlapped this picture?".
//  2. The CLASSIFIER. One long picture (interval over GXV_HITCH_MS, default 40) gets a cause: the largest of the back end's measured parts, the front end,
//     the game thread's longest turn, the game thread's waits for a full stream segment, the disc worker's busy time, a journaled job. Pure function of its
//     inputs (tests/hitch24_model.cpp).
//  3. The STAGE SUMMARY: count, total ms, worst, and the same by cause, printed when the stage is left.
//
// Nothing here changes any picture, any timing of the game or any memory use beyond ~1 KiB of statics.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" int64_t port_host_time_ns(void) __attribute__((weak));

namespace port::hitch24 {

inline int64_t NowNs() {
    if (port_host_time_ns) return port_host_time_ns();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the job journal --------------------------------------------------------------------------------------------------------------------------------
constexpr unsigned kJobRing = 32;
struct JobSlot {
    std::atomic<const char*> name{nullptr};
    std::atomic<int64_t> t0{0}, t1{0};
};
struct JobJournal {
    std::atomic<uint32_t> head{0};
    JobSlot slot[kJobRing];
};
inline JobJournal g_jobs;  // (an inline variable, not a function-local static: no guard barrier on every access, ARMv7)

// A job ran from t0 to t1 (ns, the clock of NowNs). `name` must outlive the program (a string literal).
inline void JobNote(const char* name, int64_t t0, int64_t t1) {
    JobSlot& s = g_jobs.slot[g_jobs.head.fetch_add(1, std::memory_order_relaxed) % kJobRing];
    s.t1.store(0, std::memory_order_relaxed);  // (a reader that meets the slot half written sees no end and skips it)
    s.name.store(name, std::memory_order_relaxed);
    s.t0.store(t0, std::memory_order_relaxed);
    s.t1.store(t1, std::memory_order_release);
}
struct JobScope {
    const char* name;
    int64_t t0;
    explicit JobScope(const char* n) : name(n), t0(NowNs()) {}
    ~JobScope() { JobNote(name, t0, NowNs()); }
};
// The longest journaled job (at least minNs long) that overlaps [from, to]; false when none. `ns` is its whole length, not only the overlap: a job that
// held a thread for 60 ms and ended inside this picture is the cause of the picture even if it began in the one before.
inline bool JobLongest(int64_t from, int64_t to, int64_t minNs, const char** name, int64_t* ns) {
    bool found = false;
    int64_t best = 0;
    const char* bestName = nullptr;
    for (unsigned i = 0; i < kJobRing; ++i) {
        const JobSlot& s = g_jobs.slot[i];
        const int64_t t1 = s.t1.load(std::memory_order_acquire);
        if (!t1) continue;
        const int64_t t0 = s.t0.load(std::memory_order_relaxed);
        const char* n = s.name.load(std::memory_order_relaxed);
        if (!n || t1 < from || t0 > to) continue;
        const int64_t len = t1 - t0;
        if (len >= minNs && len > best) { best = len; bestName = n; found = true; }
    }
    if (found) { *name = bestName; *ns = best; }
    return found;
}

// ---- the classifier ---------------------------------------------------------------------------------------------------------------------------------
enum Cause : uint8_t {
    kLink, kUpload, kTarget, kSize, kFrameStart, kDelete, kWarm, kGpuWait, kBackOther, kFront, kGame, kSegWait, kJob, kIo, kGate, kUpstream, kPresent, kUnknown, kCauseN
};
inline const char* CauseName(Cause c) {
    static const char* const n[kCauseN] = {"program links", "texture uploads", "render targets made", "size change", "frame start", "program deletes", "warm-up",
                                           "GPU waits", "back end other", "front end", "game thread", "stream segment full (game waits for the front end)",
                                           "periodic job", "disc reads",
                                           "game thread waits for the back end's queue",
                                           "no picture arrived: front end or pacing (game turns were on time)", "present path", "no part explains it"};
    return c < kCauseN ? n[c] : "?";
}
inline void FormatDisplayQueue(char* out, size_t cap, unsigned calls, double ms) {
    if (calls) std::snprintf(out, cap, "; GXM display queue (whole frame): %.2f ms (%u calls)", ms, calls);
    else std::snprintf(out, cap, "; GXM display queue (whole frame): n/a (no sampled calls)");
}
// One picture's numbers, all in ms. The back end's parts are its own (StutterCosts); the rest are from the platform layer, where known (0 = not known).
struct Picture {
    double intervalMs = 0;     // since the previous picture finished in the back end
    double beMs = 0;           // the back end's own time in this picture
    double linkMs = 0, uploadMs = 0, deleteMs = 0, warmMs = 0, frameStartMs = 0, sizeMs = 0, gpuWaitMs = 0, presentMs = 0, otherMs = 0;
    unsigned targetsMade = 0;  // pooled render targets made in this picture (their time is inside `otherMs`)
    double frontMs = 0;        // the front end's CPU for the pictures since the last one, per picture
    double gameMs = 0;         // the longest game-thread turn finished since the last picture
    const char* gameTop = "";  // ... and its largest part
    double segWaitMs = 0;      // the game thread's waits for a full stream segment since the last picture
    double ioMs = 0;           // the disc worker's busy time since the last picture
    // smooth25: the game thread's own clock, always on (platform/src/port/perf.cpp port_turn_gap_take: one clock read a turn). 0 = not known.
    double turnGapMs = 0;      // the longest time between two turns' starts since the last picture (a turn and its retrace wait: 16.7 when nothing is wrong)
    double gateMs = 0;         // the longest wait of the game thread for room in the back end's queue (vgx_galaxy.cpp FrameSink) since the last picture
    unsigned turns = 0;        // game turns started since the last picture (60 a second: a 600 ms gap with 2 turns says the game thread was stuck, with 36 it ran on)
    double jobMs = 0;          // the longest journaled job overlapping this picture ...
    const char* jobName = "";  // ... and its name
};
struct Verdict {
    Cause cause = kUnknown, second = kUnknown;
    double ms = 0, secondMs = 0;
};
// The picture's time not in the back end is waiting for something upstream: the back end sat idle for `idle` ms.
inline Verdict Classify(const Picture& p, double minShare = 0.25) {
    double v[kCauseN] = {};
    v[kLink] = p.linkMs;
    v[kUpload] = p.uploadMs;
    v[kDelete] = p.deleteMs;
    v[kWarm] = p.warmMs;
    v[kFrameStart] = p.frameStartMs;
    v[kSize] = p.sizeMs;
    v[kGpuWait] = p.gpuWaitMs;
    // Two ordinary vblanks can be intentional at 30 Hz. Only name a longer
    // elapsed present; it includes the display copy and swap, not just GPU wait.
    if (p.presentMs > 34.0) v[kPresent] = p.presentMs;
    // the unexplained rest of the back end's time: a pooled render target made in it is what it is (the console: 24-72 ms a target)
    (p.targetsMade ? v[kTarget] : v[kBackOther]) = p.otherMs;
    const double idle = p.intervalMs - p.beMs;
    if (idle > 8.0) {  // the back end waited for its next picture: the front end or the game thread was the slow one
        v[kFront] = p.frontMs > 0 ? (p.frontMs < idle + p.beMs ? p.frontMs : idle + p.beMs) : 0;
        v[kGame] = p.gameMs;
        v[kSegWait] = p.segWaitMs;
        // smooth25: the 501 ms "no part explains it" of the first picture of Honeyhive. The game thread's turn gaps and its gate wait say whose time it was:
        //  - a turn gap over half of the idle time: the game thread (its own turn, or a wait inside it the other counters do not name);
        //  - a gate wait over half of it: the game thread waited for the back end's queue (the back end was the slow side upstream of this picture);
        //  - turns kept coming (gap small) and no picture arrived: the front end was behind, or the pacer left the pictures out.
        if (p.turnGapMs > 0) {
            if (p.gateMs >= 0.5 * idle) v[kGate] = p.gateMs;
            else if (p.turnGapMs >= 0.5 * idle && p.turnGapMs > 25.0) v[kGame] = p.turnGapMs > v[kGame] ? p.turnGapMs : v[kGame];  // (a gap of 17-25 ms is an ordinary turn)
            else if (v[kFront] <= 0 && v[kGame] <= 0 && v[kSegWait] <= 0) v[kUpstream] = idle;
        }
    }
    v[kIo] = p.ioMs > 8.0 ? p.ioMs : 0;
    v[kJob] = p.jobMs;
    Verdict r;
    unsigned top = kUnknown, sec = kUnknown;
    for (unsigned c = 0; c < kUnknown; ++c) {
        if (v[c] <= 0) continue;
        if (top == kUnknown || v[c] > v[top]) { sec = top; top = c; }
        else if (sec == kUnknown || v[c] > v[sec]) sec = c;
    }
    // a journaled job that is most of the part it ran inside is named as the cause (the stage queue's build is a "frame start" of 72 ms)
    if (top != kUnknown && p.jobMs > 0 && top != kJob && (top == kFrameStart || top == kBackOther || top == kDelete || top == kWarm) && p.jobMs >= 0.6 * v[top]) { sec = top; top = kJob; }
    if (top == kUnknown || v[top] < minShare * p.intervalMs) {
        r.cause = kUnknown;
        r.ms = p.intervalMs - p.beMs > 0 ? p.intervalMs - p.beMs : 0;
        r.second = top == kUnknown ? kUnknown : static_cast<Cause>(top);
        r.secondMs = top == kUnknown ? 0 : v[top];
        return r;
    }
    r.cause = static_cast<Cause>(top);
    r.ms = v[top];
    r.second = static_cast<Cause>(sec);
    r.secondMs = sec == kUnknown ? 0 : v[sec];
    return r;
}

// ---- the stage summary ------------------------------------------------------------------------------------------------------------------------------
struct Summary {
    unsigned n = 0;
    double totalMs = 0, worstMs = 0;
    unsigned byN[kCauseN] = {};
    double byMs[kCauseN] = {};
    const char* worstJob = "";
    void Add(Cause c, double intervalMs, const char* job) {
        ++n;
        totalMs += intervalMs;
        if (intervalMs > worstMs) { worstMs = intervalMs; worstJob = job ? job : ""; }
        ++byN[c];
        byMs[c] += intervalMs;
    }
    // "5 pictures, 312 ms in all, worst 152 ms; by cause: render targets made 2 (180 ms), ..." (causes with pictures only, most time first)
    size_t Format(char* out, size_t cap) const {
        int len = std::snprintf(out, cap, "%u pictures over the limit, %.0f ms in all, worst %.0f ms", n, totalMs, worstMs);
        if (len < 0 || static_cast<size_t>(len) >= cap) return cap ? cap - 1 : 0;
        unsigned order[kCauseN];
        unsigned m = 0;
        for (unsigned c = 0; c < kCauseN; ++c) if (byN[c]) order[m++] = c;
        for (unsigned i = 1; i < m; ++i)
            for (unsigned j = i; j > 0 && byMs[order[j]] > byMs[order[j - 1]]; --j) { const unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
        for (unsigned i = 0; i < m; ++i) {
            const int k = std::snprintf(out + len, cap - static_cast<size_t>(len), "%s %s %u (%.0f ms)", i ? "," : "; by cause:", CauseName(static_cast<Cause>(order[i])), byN[order[i]],
                                        byMs[order[i]]);
            if (k < 0 || static_cast<size_t>(len + k) >= cap) break;
            len += k;
        }
        return static_cast<size_t>(len);
    }
};

}  // namespace port::hitch24
