// honey25 (12 Nov 2026): GXV_CPUGOV, a governor for the far-draw cut (GXV_FARDRAW and its kinds / hold / flush skip, cpucuts25). Pure logic: no
// game types, no clocks, no threads (host-tested by tests/cpugov.cpp). The glue is in platform/src/port/perf.cpp (the atomics and the log line),
// platform/vgx/src/vgx_backend_gl.cpp (FloorReport feeds a window, a stage preset's load resets), ClippingActorHolder.cpp (the accessors take the
// larger of the static value and the governor's), stage_knob.h (cpu_gov), entry.cpp (setenv defaults: all off).
//
// WHY. cpucuts25's far-draw cut is a fixed threshold per stage: a stage that holds 60 without it pays the visual price anyway, a stage that
// needs more than its row has gets nothing. The governor makes the cut follow the NEED: it reads the back end's 5-second floor window (the
// late pictures and the stage that made each one late: GPU, game thread CPU, renderer CPU, front end CPU, none) and
//   - raises the level (a larger projected-size limit in lines of 456, more classes of far actor, a hold on the stamps) when a window is
//     late AND the lateness is the CPU's (game thread, front end, renderer): a draw cut gives the GPU little, and the picture-size controller
//     (autoq) is the lever for a GPU-late window;
//   - lowers it only after a long calm run, and when a lowering is followed by lateness again within a few windows it waits twice as long the
//     next time (no limit cycle: specks that come back every 30 s and go again are worse than specks that stay away).
// A level only chooses WHICH far models the existing, hysteretic, per-actor-jittered far-draw rule may take (its own ramp spreads every
// actor's threshold over +50 %, its hold keeps a stamp 20 turns): one level step makes a few models leave one by one, never a crowd at once.
// Mario, collectibles, items, switches, bosses, anything with collision and anything near the camera or the player are outside the rule's reach
// at every level (cpucuts.h ClassOfSensorType, GXV_CR_NEAR); this header does not change that.
//
//   GXV_CPUGOV=<0..5>       the highest level (0 = off, the default; stage-settable as cpu_gov). In Performance mode only unless GXV_CPUGOV_ANY=1.
//   GXV_CPUGOV_LATE=<pct>   a window is HOT when at least this % of its pictures are late (default 15)
//   GXV_CPUGOV_CPU=<pct>    ... and at least this % of its late pictures are the game thread's, the renderer's or the front end's (default 50)
//   GXV_CPUGOV_CALM=<pct>   a window is CALM when at most this % of its pictures are late (default 3)
//   GXV_CPUGOV_UP=<n>       hot windows in a row before a level up (default 1: 5 s a step)
//   GXV_CPUGOV_DOWN=<n>     calm windows in a row before a level down (default 6: 30 s)
//   GXV_CPUGOV_LOG=<n>      a summary line every n windows besides the one per change (default 6; 0 = changes only)
#pragma once

#include <cstdint>

namespace port {
namespace cpugov {

constexpr int kMaxLevel = 5;

// What a level asks of the far-draw rule. lines: the projected size (of 456) under which a far model may be left out; kinds: cpucuts.h ClassBit mask
// (1 pure decoration, 2 decoration with effects, 4 enemies, 8 plain map objects, 16 NPCs / animals); hold: minimum stamp age in turns; flush: the pose
// flush also leaves a stamped model out. Level 0 asks nothing.
struct Level {
    float lines;
    uint8_t kinds;
    uint8_t hold;
    uint8_t flush;
};
// Monotone in every field (tests/cpugov.cpp checks it): a higher level never gives a model back that a lower one took. The classes come in the order
// of what a player is least likely to look at: pure decoration, decoration with effects, plain map objects, NPCs and animals, enemies last.
inline Level LevelOf(int level) {
    static const Level k[kMaxLevel + 1] = {
        {0.0f, 0, 0, 0},      // off
        {6.0f, 1, 20, 1},     // pure decoration under 6 lines
        {8.0f, 3, 20, 1},     // + decoration with effects, under 8
        {10.0f, 11, 20, 1},   // + plain map objects, under 10
        {13.0f, 27, 20, 1},   // + NPCs and animals (bees), under 13
        {16.0f, 31, 20, 1},   // + enemies, under 16
    };
    return k[level < 0 ? 0 : (level > kMaxLevel ? kMaxLevel : level)];
}

struct Config {
    int maxLevel = 0;      // 0 = off
    int latePct = 15;
    int cpuPct = 50;
    int calmPct = 3;
    int upWindows = 1;
    int downWindows = 6;
    int downWindowsMax = 120;  // the longest a backed-off wait gets (10 minutes)
    int backoffSpan = 3;       // windows after a lowering in which a hot window counts as "it came back"
    uint32_t minPictures = 60; // a window with fewer pictures says nothing
    int quietPct = 25;         // a window in which this % of the pictures were quiet moments (pause, fades) or stalls (a load) says nothing
};

// One 5-second floor window as the back end counted it (vgx_autoq.h FloorWindow: the fields this reads).
struct Window {
    uint32_t pictures = 0, late = 0;
    uint32_t gpu = 0, game = 0, renderer = 0, front = 0, other = 0, unknown = 0;
    uint32_t stalls = 0, quiet = 0;
};

enum class Verdict : uint8_t { Ignored, Hot, Calm, Middle };

inline Verdict Judge(const Config& c, const Window& w) {
    if (w.pictures < c.minPictures) return Verdict::Ignored;
    // quiet pictures are not in `pictures`; stalls are intervals (not pictures) the window left out: both measure how much of the window was not play
    const uint64_t notPlay = static_cast<uint64_t>(w.quiet) + w.stalls;
    if (notPlay * 100u >= static_cast<uint64_t>(c.quietPct) * (static_cast<uint64_t>(w.pictures) + w.quiet)) return Verdict::Ignored;
    const uint64_t late100 = static_cast<uint64_t>(w.late) * 100u;
    if (late100 >= static_cast<uint64_t>(c.latePct) * w.pictures && w.late != 0) {
        const uint64_t cpu = static_cast<uint64_t>(w.game) + w.renderer + w.front;
        if (cpu * 100u >= static_cast<uint64_t>(c.cpuPct) * w.late) return Verdict::Hot;
        return Verdict::Middle;  // late, but the GPU's (or waits'): not this lever's
    }
    if (late100 <= static_cast<uint64_t>(c.calmPct) * w.pictures) return Verdict::Calm;
    return Verdict::Middle;
}

class Governor {
public:
    Governor() = default;
    explicit Governor(const Config& c) : c_(c), downNeed_(c.downWindows) {}
    void Configure(const Config& c) {
        c_ = c;
        Reset();
    }
    // New thresholds (the knobs are read again every window, so a stage preset's cpu_gov works at once); the state is kept.
    void Update(const Config& c) {
        c_ = c;
        if (downNeed_ < c_.downWindows) downNeed_ = c_.downWindows;
        if (downNeed_ > c_.downWindowsMax) downNeed_ = c_.downWindowsMax < c_.downWindows ? c_.downWindows : c_.downWindowsMax;
    }
    // A new stage (or any new situation): the level and every run start again.
    void Reset() {
        level_ = 0;
        hotRun_ = calmRun_ = 0;
        sinceDown_ = 1000000;
        downNeed_ = c_.downWindows;
        changes_ = 0;
    }
    // Takes a window; returns true when the level changed.
    bool Feed(const Window& w) {
        if (c_.maxLevel <= 0) return false;
        const Verdict v = Judge(c_, w);
        if (sinceDown_ < 1000000) ++sinceDown_;
        last_ = v;
        switch (v) {
            case Verdict::Ignored:
                return false;  // nothing counted: a load or a pause does not make a run of anything
            case Verdict::Hot: {
                calmRun_ = 0;
                if (sinceDown_ <= c_.backoffSpan) {
                    // it came back right after a lowering: wait longer before the next one (a doubling, up to the cap)
                    downNeed_ = downNeed_ * 2 > c_.downWindowsMax ? c_.downWindowsMax : downNeed_ * 2;
                    sinceDown_ = 1000000;
                }
                if (++hotRun_ >= (c_.upWindows < 1 ? 1 : c_.upWindows) && level_ < c_.maxLevel && level_ < kMaxLevel) {
                    ++level_;
                    hotRun_ = 0;
                    ++changes_;
                    return true;
                }
                return false;
            }
            case Verdict::Calm: {
                hotRun_ = 0;
                if (level_ > 0 && ++calmRun_ >= downNeed_) {
                    --level_;
                    calmRun_ = 0;
                    sinceDown_ = 0;
                    ++changes_;
                    return true;
                }
                return false;
            }
            case Verdict::Middle:
                hotRun_ = 0;
                calmRun_ = 0;  // neither: a calm run needs consecutive calm windows
                return false;
        }
        return false;
    }
    int level() const { return level_ < c_.maxLevel ? level_ : c_.maxLevel; }
    int downNeed() const { return downNeed_; }
    int calmRun() const { return calmRun_; }
    int hotRun() const { return hotRun_; }
    uint32_t changes() const { return changes_; }
    Verdict last() const { return last_; }
    const Config& config() const { return c_; }

private:
    Config c_{};
    int level_ = 0, hotRun_ = 0, calmRun_ = 0, sinceDown_ = 1000000, downNeed_ = 6;
    uint32_t changes_ = 0;
    Verdict last_ = Verdict::Ignored;
};

// The far-draw rule's inputs, the static knob values and the governor's level combined: the larger lines, the union of the classes, the longer hold,
// the flush skip if either asks. Level 0 returns the static values as they are (so a stage with the governor off, or at level 0, is exactly what it
// was).
struct Combined {
    float lines;
    int kinds, hold, flush;
};
inline Combined Combine(float staticLines, int staticKinds, int staticHold, int staticFlush, int level) {
    Combined r{staticLines, staticKinds, staticHold, staticFlush};
    if (level <= 0) return r;
    const Level l = LevelOf(level);
    if (l.lines > r.lines) {
        // the governor's lines are what makes the rule act at all when the static one is off (lines 0): its kinds come with them
        r.lines = l.lines;
    }
    r.kinds = staticLines > 0.0f ? (staticKinds | l.kinds) : l.kinds;
    if (l.hold > r.hold) r.hold = l.hold;
    if (l.flush) r.flush = 1;
    return r;
}

}  // namespace cpugov
}  // namespace port
