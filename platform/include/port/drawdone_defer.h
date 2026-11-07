// GXV_DRAWDONE_DEFER: where the game thread waits for the front end at the end of a drawn turn.
//
// MainLoopFramework::endFrame ends a drawn turn with GXDrawDone: it writes the "draw done" command into the stream and sleeps until
// the front end (the player thread) has parsed everything up to it and raised the finish interrupt.  On the Wii that wait is what
// lets the game write to memory the GP was reading (matrix and vertex arrays, display lists, textures) in the next turn.  The
// game's turn is: draw (the stream is written, the front end follows behind it), then its logic (update, calcAnim), then this wait.
// When the front end is the slow part the game thread sleeps here for the rest of the front end's turn (finish_wait_us in the
// "turn-cost:" lines, 4-6 ms per drawn turn in the plazas) although the next turn begins with work that writes none of that memory
// (the frame-skip decision, the retrace wait, beginRender and draw, which only read the game's state).
//
//   GXV_DRAWDONE_DEFER=0   wait in endFrame (the default)
//   GXV_DRAWDONE_DEFER=1   write the command and go on; wait at the start of the next turn's update() (draw of the next turn overlaps)
//   GXV_DRAWDONE_DEFER=2   the same, but wait at the start of the next calcAnim(): update(), the game's logic, overlaps as well.  Riskier:
//                          update() writes more of the memory the front end reads (display-list patches, textures made by the CPU)
//   GXV_DRAWDONE_DEFER=3   (1.3) the wait only at the start of the next turn that draws (kDrawnTurn): the turns without a picture in
//                          between do not wait at all.  They leave out the view pass (the draw matrices the front end reads by index,
//                          GameScene::calcAnim) and the material lists (GXV_DL_LAZY=2), and the drawn turn makes both, from the same
//                          state, after the wait and before its picture.  What update() and the joints write is the same as mode 2.
//   GXV_DRAWDONE_DEFER=4   (1.3) pipelined: no wait before a picture either.  The drawn turn's own view pass is left out too, so each
//                          model's view matrices and material lists are made once per picture, in the flush before it, into the half of
//                          their double buffer the last picture does not read; the front end finishes picture N while the game draws
//                          N+1, and the wait is made only before the next GXSetDrawDone.  A view pass or list made anywhere else waits
//                          (port_gx_view_write).
// Any other value is 0.  The wait is also made, whatever the mode, before the next GXSetDrawDone (the finish interrupts of two
// commands must never be outstanding together: the flag GXWaitDrawDone watches is one bit).
//
// The state machine below is the only thing that decides; the SDK calls (GXSetDrawDone, GXWaitDrawDone) stay with the callers
// (decomp/src/Game/System/MainLoopFramework.cpp; the counters and the log line are in platform/src/port/perf.cpp).
// tests/drawdone_defer.cpp checks it with a model of the front end.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace port {
namespace drawdone {

// Points at which a deferred wait may have to be made, in the order a turn reaches them.
enum Point : int { kSetDrawDone = 0, kUpdate = 1, kCalcAnim = 2, kDrawnTurn = 3 };

// "0", "1", "2" give that mode; anything else (unset, empty, garbage, out of range) is 0.
inline int ParseMode(const char* text) {
    if (!text || !*text) return 0;
    char* end = nullptr;
    const long v = strtol(text, &end, 0);
    return (end && end != text && !*end && (v >= 1 && v <= 4)) ? static_cast<int>(v) : 0;
}

class Deferral {
public:
    explicit Deferral(int mode) : mode_(mode) {}
    int Mode() const { return mode_; }
    bool Enabled() const { return mode_ != 0; }
    bool Pending() const { return pending_; }

    // The command was written and the caller goes on without waiting.  Only valid with the mode on and nothing outstanding.
    void Arm() {
        if (pending_) ++overlaps_;  // (a caller bug: Settle(kSetDrawDone) comes first)
        pending_ = true;
        ++deferred_;
    }
    // The game reached `point`; true: the finish is outstanding and the caller must wait for it now (and call Waited).
    bool MustWait(int point) {
        if (!pending_) return false;
        if (point != kSetDrawDone && point < mode_) return false;
        pending_ = false;
        ++waits_;
        return true;
    }
    // The wait that MustWait asked for took `ns`.
    void Waited(int64_t ns) {
        totalNs_ += ns;
        if (ns > maxNs_) maxNs_ = ns;
        if (ns >= 1000000) ++over1ms_;
    }
    // Every `every` waits: a line for the log (false: nothing to report yet); the counters of the window restart.
    bool Report(char* line, size_t size, uint32_t every) {
        if (waits_ - reported_ < every) return false;
        const uint32_t n = waits_ - reported_;
        snprintf(line, size, "drawdone-defer: mode %d, %u deferred waits in the window, %.2f ms each on average (%u over 1 ms), longest %.2f ms", mode_, n,
                 static_cast<double>(totalNs_) / 1e6 / n, over1ms_, static_cast<double>(maxNs_) / 1e6);
        reported_ = waits_;
        totalNs_ = maxNs_ = 0;
        over1ms_ = 0;
        return true;
    }
    uint32_t Deferred() const { return deferred_; }
    uint32_t Waits() const { return waits_; }
    uint32_t Overlaps() const { return overlaps_; }  // must stay 0

private:
    int mode_;
    bool pending_ = false;
    uint32_t deferred_ = 0, waits_ = 0, reported_ = 0, over1ms_ = 0, overlaps_ = 0;
    int64_t totalNs_ = 0, maxNs_ = 0;
};

}  // namespace drawdone
}  // namespace port
