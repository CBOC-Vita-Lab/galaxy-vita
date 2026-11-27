// endwait26 (GXV_ENDWAIT_LOG=1, default 0): the pure parts of "why did the game thread wait at the end of its frame". Header only, standard library only, so the
// front end (platform/src/gx/vgx_galaxy.cpp), the game thread's report (platform/src/port/perf.cpp) and the host test (tests/endwait_model.cpp) share the code.
//
// WHY (console, 22-stage tours c79 K3 / RM, new/handover26): about 8.5 s into a stage 6 of 20 stages show the GAME THREAD's end of frame (GXSetDrawDone: the sleep
// until the front end has parsed the whole picture and raised the finish interrupt) taking 130-363 ms, followed by 1-2 more turns of 130-150 ms. The back end is idle
// meanwhile (its picture starts the moment the wait ends: frame 28777 at +0.402 s = the wait's end), the player waits for the finishing thread 11 ms in all
// (fepar settles) and the front end's texture decode is 12-28 ms for the window, so the time is in the front end's own work for those pictures. The logs cannot
// say which; this file's record can.
//
// WHAT IT HOLDS
//   Pic    one picture as the front end (the "player" thread) built it, from its first parsed byte to the hand-over to the back end: busy time split into
//          texture decode (ms, bytes, count: the upload path of ResolveTextureFull), display-list builds (vgx_dlcache.cpp Build), the join with the finishing thread
//          (FinishFrame: the player's wait for it, and the settle waits of GXV_FE_PARALLEL), the picture's passes (FrontPresent less the gate), the gate wait (the
//          back end's queue: FrameSink), and the rest, "parse" (stream parsing, state resolution, draw emission, everything not listed); idle time (the player asleep,
//          waiting for the game's bytes) is separate.
//   Ring   the last kRing pictures; the player writes one a picture, the game thread reads when a wait was long (a tiny spin lock, once a picture).
//   Text   the one-line report of a long wait: stage and time, the game thread's wait, the pictures that overlap it with their splits, the stream (segment waits),
//          the queue between front and back end, the back end's state and what it finished during the wait.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

namespace vgx::endwait {

constexpr unsigned kRing = 16;
constexpr unsigned kShow = 3;            // pictures printed in a line
constexpr int64_t kMinWaitNs = 40000000; // a game-thread wait shorter than this is not reported (GXV_ENDWAIT_MS overrides, 10..2000)
constexpr unsigned kLinesPerStage = 40;

struct Pic {
    uint64_t frame = 0;                 // the picture's number (the back end's "frame N" in its hitch / stutter lines)
    int64_t startNs = 0, endNs = 0;     // first byte parsed .. handed to the back end (port_host_time_ns clock)
    uint32_t idleUs = 0;                // inside that: the player asleep, waiting for bytes the game had not published
    uint32_t decodeUs = 0, decodeN = 0, decodeKiB = 0;  // texture decode on first upload: time, textures, KiB of source texels
    uint32_t dlUs = 0, dlN = 0;         // display-list builds (new lists decoded): time, count
    uint32_t joinUs = 0;                // FinishFrame: the join with the finishing thread (its drain wait is the first part of it)
    uint32_t drainUs = 0;               // the player's wait for the finishing thread's reader (split::Drain), over the whole picture
    uint32_t settleUs = 0;              // GXV_FE_PARALLEL settle waits over the whole picture
    uint32_t passUs = 0;                // FrontPresent's own work (the passes, the scene counts), less the gate
    uint32_t gateUs = 0;                // the wait for a free place in the back end's queue (FrameSink)
    uint32_t upN = 0, upKiB = 0;        // texture uploads the picture queued for the back end (count, KiB after conversion)
    uint32_t draws = 0;                 // draw records
    bool valid() const { return endNs != 0; }
    uint32_t busyUs() const {
        const int64_t t = (endNs - startNs) / 1000;
        const int64_t b = t - static_cast<int64_t>(idleUs);
        return b > 0 ? static_cast<uint32_t>(b) : 0u;
    }
    // Stream parsing and everything else the parts above do not name (never negative: the parts are timed by different clocks' reads).
    uint32_t parseUs() const {
        const int64_t r = static_cast<int64_t>(busyUs()) - decodeUs - dlUs - joinUs - passUs - gateUs;
        return r > 0 ? static_cast<uint32_t>(r) : 0u;
    }
};

class Ring {
public:
    void Push(const Pic& p) { p_[n_ % kRing] = p; ++n_; }
    unsigned Count() const { return n_ < kRing ? static_cast<unsigned>(n_) : kRing; }
    // i = 0 is the newest.
    const Pic& Back(unsigned i) const { return p_[(n_ - 1 - i) % kRing]; }
    uint64_t Total() const { return n_; }
    // Up to `cap` pictures whose [start, end] touches [t0, t1], oldest first. Returns how many.
    unsigned Overlapping(int64_t t0, int64_t t1, Pic* out, unsigned cap) const {
        Pic tmp[kRing];
        unsigned m = 0;
        for (unsigned i = Count(); i-- > 0;) {  // oldest to newest
            const Pic& p = Back(i);
            if (!p.valid() || p.endNs < t0 || p.startNs > t1) continue;
            tmp[m++] = p;
        }
        const unsigned skip = m > cap ? m - cap : 0;  // keep the newest `cap`
        for (unsigned i = skip; i < m; ++i) out[i - skip] = tmp[i];
        return m - skip;
    }
private:
    Pic p_[kRing];
    uint64_t n_ = 0;
};

// The stage clock: when the stage's scene key was first seen by the front end, and when its screen first showed (not closed by a fade or wipe).
struct StageClock {
    uint32_t key = 0;
    int64_t startNs = 0, openNs = 0;
    unsigned lines = 0;
    // Called once a picture. quiet != 0: the screen is hidden by a wipe / fade / menu.
    void Seen(uint32_t k, int quiet, int64_t nowNs) {
        if (k != key) { key = k; startNs = nowNs; openNs = 0; lines = 0; }
        if (k != 0 && !quiet && openNs == 0) openNs = nowNs;
    }
    bool LineAllowed() { return lines < kLinesPerStage ? (++lines, true) : false; }
};

// What the back end is doing (galaxy.cpp sBackStage: 0 waiting for a frame, 1 executing one, 2 after it), for how long, and the queue.
struct BeState {
    int stage = 0;
    double sinceMs = 0;
    unsigned jobs = 0, limit = 0, freeBufs = 0;
};
// What the back end finished during the wait (differences of the cumulative sums of finished pictures; a picture still running is not in them).
struct BeDone {
    unsigned pictures = 0;
    double execMs = 0, targetMs = 0, texMs = 0, sceneMs = 0, finishMs = 0, dispMs = 0, presentMs = 0, drawMs = 0, endMs = 0, reserveMs = 0;
    unsigned targetsMade = 0, texN = 0;
    double texKiB = 0;
};
struct Stream {  // the game's segment waits (port_vgx_publish "full") over the wait, from the SegCounters
    unsigned full = 0;
    double fullMs = 0;
};

struct Inputs {
    uint32_t stageKey = 0;
    double sinceStartS = 0, sinceOpenS = -1;  // seconds from the stage's first picture / its screen opening (-1: not open yet)
    double waitMs = 0;                        // the game thread's wait at the end of the frame
    int where = 0;                            // 0 end of frame, 1+ a deferred settle point (GXV_DRAWDONE_DEFER)
    int64_t waitStartNs = 0, waitEndNs = 0;
    Pic pics[kShow];
    unsigned nPics = 0;
    Stream stream;
    BeState be;
    BeDone done;
    bool haveBeDone = false;
};

inline void Add(char* out, size_t cap, int& len, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
__attribute__((format(printf, 4, 5))) inline void Add(char* out, size_t cap, int& len, const char* fmt, ...) {
    if (len < 0 || static_cast<size_t>(len) + 1 >= cap) return;
    va_list args;
    va_start(args, fmt);
    const int k = std::vsnprintf(out + len, cap - static_cast<size_t>(len), fmt, args);
    va_end(args);
    if (k > 0) len = static_cast<size_t>(len + k) < cap ? len + k : static_cast<int>(cap - 1);
}

inline const char* BeStageName(int s) { return s == 0 ? "waiting for a picture" : s == 1 ? "executing a picture" : s == 2 ? "finishing a picture" : "?"; }

// Which part of the picture the wait is, in one word (the largest of the parts; "idle" when the front end was asleep for most of the wait: the game's bytes
// were late or the wake-up was; "none" when no picture overlaps).
inline const char* Verdict(const Inputs& in) {
    if (!in.nPics) return "no front-end picture overlaps the wait";
    uint32_t best = 0;
    const char* name = "parse";
    // The picture that released the wait is the last one; the earlier ones may have been waited for too.
    uint64_t sum[7] = {};
    for (unsigned i = 0; i < in.nPics; ++i) {
        const Pic& p = in.pics[i];
        sum[0] += p.parseUs(); sum[1] += p.decodeUs; sum[2] += p.dlUs; sum[3] += p.joinUs; sum[4] += p.passUs; sum[5] += p.gateUs; sum[6] += p.idleUs;
    }
    static const char* const n[7] = {"parse (stream, state, draws)", "texture decode", "display-list builds", "join with the finishing thread", "passes",
                                     "queue gate (back end busy)", "idle (front end waiting for the game's bytes)"};
    for (int i = 0; i < 7; ++i)
        if (sum[i] > best) { best = static_cast<uint32_t>(sum[i] > 0xFFFFFFF0u ? 0xFFFFFFF0u : sum[i]); name = n[i]; }
    return name;
}

// "endwait: stage 2f7c8bbc +8.4 s (screen open +8.3 s): game thread waited 363 ms at the end of the frame; front end: ..."
inline size_t Format(char* out, size_t cap, const Inputs& in) {
    int len = 0;
    out[0] = 0;
    Add(out, cap, len, "endwait: stage %08x +%.1f s", static_cast<unsigned>(in.stageKey), in.sinceStartS);
    if (in.sinceOpenS >= 0) Add(out, cap, len, " (screen open +%.1f s)", in.sinceOpenS);
    else Add(out, cap, len, " (screen closed)");
    Add(out, cap, len, ": game thread waited %.0f ms %s", in.waitMs, in.where == 0 ? "at the end of the frame (GXSetDrawDone)" : "at a deferred draw-done settle");
    if (!in.nPics) {
        Add(out, cap, len, "; front end: no picture record overlaps it (the player had no bytes, or the record is off)");
    }
    for (unsigned i = 0; i < in.nPics; ++i) {
        const Pic& p = in.pics[i];
        const double a = (p.startNs - in.waitStartNs) / 1e6, b = (p.endNs - in.waitStartNs) / 1e6;
        Add(out, cap, len, "; front end picture %llu [%+.0f .. %+.0f ms from the wait's start] busy %.0f ms = parse %.0f + texture decode %.0f (%u tex, %u KiB) + "
                           "display-list builds %.0f (%u) + join %.0f (finisher drain %.0f, fepar settle %.0f) + passes %.0f + queue gate %.0f; idle %.0f; "
                           "queued for the back end: %u uploads %u KiB, %u draws",
            static_cast<unsigned long long>(p.frame), a, b, p.busyUs() / 1000.0, p.parseUs() / 1000.0, p.decodeUs / 1000.0, p.decodeN, p.decodeKiB, p.dlUs / 1000.0, p.dlN,
            p.joinUs / 1000.0, p.drainUs / 1000.0, p.settleUs / 1000.0, p.passUs / 1000.0, p.gateUs / 1000.0, p.idleUs / 1000.0, p.upN, p.upKiB, p.draws);
    }
    if (in.stream.full) Add(out, cap, len, "; stream: %u full-segment waits, %.0f ms", in.stream.full, in.stream.fullMs);
    else Add(out, cap, len, "; stream: no full-segment wait");
    Add(out, cap, len, "; queue: %u job(s) of %u, %u free buffer(s); back end at the wait's end: %s for %.0f ms", in.be.jobs, in.be.limit, in.be.freeBufs,
        BeStageName(in.be.stage), in.be.sinceMs);
    if (in.haveBeDone) {
        const BeDone& d = in.done;
        Add(out, cap, len, "; back end during the wait (finished pictures only): %u picture(s), %.0f ms (target create %.0f ms for %u made, texture uploads %.0f ms for %u "
                           "= %.0f KiB, scene begin %.0f, scene end %.0f, GPU finish wait %.0f, display queue %.0f, present %.0f, draw submit %.0f, reserve %.0f)",
            d.pictures, d.execMs, d.targetMs, d.targetsMade, d.texMs, d.texN, d.texKiB, d.sceneMs, d.endMs, d.finishMs, d.dispMs, d.presentMs, d.drawMs, d.reserveMs);
    }
    Add(out, cap, len, "; verdict: %s (the back end's own phases for the released picture are in its \"endwait-be:\" line for the same frame number)", Verdict(in));
    return static_cast<size_t>(len);
}

// The back end's line for one picture (frame N), written when it finishes executing the picture a long wait was released by. `ms` is the picture's back-end time.
inline size_t FormatBe(char* out, size_t cap, uint64_t frame, double ms, const BeDone& d) {
    int len = 0;
    out[0] = 0;
    Add(out, cap, len, "endwait-be: frame %llu: back end %.0f ms = target create %.0f ms (%u made: render-target texture and depth storage), texture uploads %.0f ms (%u, %.0f KiB), "
                       "scene begin %.0f ms, scene end %.0f, GPU finish wait (sceGxmFinish) %.0f ms, display queue %.0f, present %.0f, draw submit %.0f, reserve %.0f; "
                       "video-memory allocation is not timed apart: it is inside target create, texture uploads and the GPU finish wait",
        static_cast<unsigned long long>(frame), ms, d.targetMs, d.targetsMade, d.texMs, d.texN, d.texKiB, d.sceneMs, d.endMs, d.finishMs, d.dispMs, d.presentMs, d.drawMs,
        d.reserveMs);
    return static_cast<size_t>(len);
}

}  // namespace vgx::endwait
