// fe26 (12 Nov 2026): GXV_FE_GOV, a governor that holds the renderer's FRONT END (the player thread on core 2) under its time budget by raising and
// lowering the CPU level-of-detail level (port/cpulod.h) of the far-detail cuts. Pure logic: no game types, no clocks, no threads (host-tested by
// tests/fegov.cpp). The glue is in platform/src/port/perf.cpp (the atomics, the knobs and the log lines), platform/vgx/src/vgx_autoq.cpp (the 5-second
// floor window carries the front end's CPU time), platform/vgx/src/vgx_backend_gl.cpp (FloorReport hands the window over; a stage load resets),
// decomp ClippingActorHolder.cpp (the accessors of the cuts take the stricter of the static value and the governor's), stage_knob.h (fe_gov ...).
//
// WHY. The front end costs 17.8-21 ms a picture in the six CPU-bound stages; 16.7 ms is needed for 60. The exact items bring a few ms; the rest has
// to come from drawing less far detail. cpulod's level is a fixed choice per stage (a stage that holds 60 pays the visual price anyway). This
// governor makes the cut follow the NEED, the way the owner put it: "some things given up for 60", only when 60 is at risk.
//
// THE RULE (one decision per 5-second window; the measure is the front end's mean CPU ms a picture, which is the limit when it is above the picture
// interval, whatever the GPU does):
//   - HOT: mean front-end ms above the budget (GXV_FE_GOV_MS, default 15.5). `up` hot windows in a row (default 2: 10 s) raise the level one step.
//   - CALM: the mean, plus the ms the level being given back is known to take (measured at the raise, default 0.8), still fits budget - band
//     (GXV_FE_GOV_BAND, default 0.7 ms). `down` calm windows in a row (default 6: 30 s) lower the level one step. A lowering that is followed by a
//     hot window within `backoffSpan` windows doubles the wait for the next one (up to 10 minutes): no limit cycle, specks that come back every 30 s
//     and go again are worse than specks that stay away.
//   - a window with too few pictures, or in which a quarter of the time was a pause, a fade or a load, says nothing (it neither counts nor breaks a run).
//   - the gain of a step is measured: the mean before a raise minus the mean of the second window after it (the first one is mixed: the cuts take
//     hold actor by actor); kept per level as a slow average. It is only used to decide the lowering, never to refuse a raise.
// The level chooses which cuts apply (port/cpulod.h kMembers: the far models' material refresh, far family poses, far star-bit thinning, the
// far-draw cut, the character pose rule, far particle thinning). Mario, the HUD and near objects are outside every one of them (the cuts' own
// distance rules, GXV_CR_NEAR); this header changes none of that. Level 2 of the governor does NOT include cpulod's FARLOGIC=2 (a time dilation).
//
// cpu2-26 (12 Nov): the same class runs a SECOND instance, GXV_GAME_GOV, whose measure is the game thread's own CPU ms a DRAWN turn (platform/src/port/perf.cpp
// GameGovNote / port_gamegov_window: the turn's thread-CPU, summed over the window's drawn turns). The two governors do not fight: the level the cuts read is the LARGER
// of the two (port_fegov_level). GXV_GAME_GOV_MS (15.5), _BAND, _UP, _DOWN, _LOG, _ANY are the same knobs under that name. Level 3 (the strongest look-alikes) exists
// for both; GXV_FE_GOV=3 / GXV_GAME_GOV=3 allow it.
//
//   GXV_FE_GOV=<0..3>        the highest level the governor may use (0 = off, the default; stage-settable as fe_gov). Performance mode only unless GXV_FE_GOV_ANY=1.
//   GXV_FE_GOV_MS=<ms>       the budget (default 15.5)
//   GXV_FE_GOV_BAND=<ms>     headroom the lowering needs (default 0.7)
//   GXV_FE_GOV_UP=<n>        hot windows in a row before a raise (default 2)
//   GXV_FE_GOV_DOWN=<n>      calm windows in a row before a lowering (default 6)
//   GXV_FE_GOV_LOG=<n>       a summary line every n windows besides the one per change (default 6; 0 = changes only)
#pragma once

#include <cstdint>

namespace port {
namespace fegov {

constexpr int kMaxLevel = 4;  // cpulod's table has rows for 1..3 (port::cpulod::kMaxLevel); far30: 4 = the LAST RESORT (port/farfield.h), level 3's row without the 25 m floor; only GXV_FE_GOV=4 / GXV_GAME_GOV=4 reach it
// q30-26: the Quality (30) mode's budgets. Performance's 15.5 ms is the 16.7 ms picture less 1.2 ms of margin; at the locked 30 the picture is 33.3 ms: the front end's budget is 33.3 less the same
// share of margin (2.4 ms). The game thread's measure is the DRAWN turn's CPU ms only, and at 30 the undrawn turn between two drawn ones is paid on top (perf.cpp GameGovEnd), so its budget is
// lower: 33.3 - 2.4 margin - ~6 ms for the undrawn turn (ESTIMATE, not measured). Both are overridden by GXV_FE_GOV_MS / GXV_GAME_GOV_MS. In Quality the governors reach level 1 only (the q level).
constexpr float kBudget30Ms = 30.9f;
constexpr float kGameBudget30Ms = 25.0f;

struct Config {
    int maxLevel = 0;            // 0 = off
    float budgetMs = 15.5f;
    float bandMs = 0.7f;
    int upWindows = 2;
    int downWindows = 6;
    int downWindowsMax = 120;    // the longest a backed-off wait gets (10 minutes)
    int backoffSpan = 3;         // windows after a lowering in which a hot window counts as "it came back"
    float defaultGainMs = 0.8f;  // what a level step is assumed to take until one has been measured
    uint32_t minPictures = 60;   // a window with fewer front-end pictures says nothing
    int quietPct = 25;           // a window in which this % of the pictures were pauses / fades or stalls (a load) says nothing
    // recover30 (GXV_FE_GOV_RECOVER and _DOWN_MIN / _MARGIN / _MULTI / _HALVE; the same keys under GXV_GAME_GOV_): the give-back follows the room, not a fixed 30 s.
    bool recover = false;        // off = today's rule bit for bit
    int downMinWindows = 2;      // calm windows needed when the room is at fullMarginMs or more (the wait grows to downWindows as the room shrinks to 0)
    float fullMarginMs = 3.0f;
    bool multi = true;           // several levels together when the room covers the sum of their learned gains
    int retakeWindows = 12;      // a hot window within this many windows (60 s) of a lowering is a failed give-back: the wait doubles
    int halveWindows = 60;       // windows after a lowering that stayed (no hot window) after which a doubled wait halves (5 minutes)
    int startLevel = 0;          // far30: the level a stage starts at (the stage table's GXV_FARFIELD_START); the give-back rules then walk it down from there
    int minLevel = 0;            // (14 Nov c161) the level the governor never goes under: the stage's own static cpulod level less one (perf.cpp), under which a step changes nothing
                                 // (the static cuts are already as strict), so the first raise is the one that adds a cut and a give-back never walks through dead steps
};

// One 5-second floor window, as far as the governor reads it.
struct Window {
    uint32_t pictures = 0;      // pictures of the window (play only)
    uint32_t frontPics = 0;     // ... of those, the ones with a measured front-end time
    uint64_t frontSumUs = 0;    // the sum of those times (CPU microseconds)
    uint32_t stalls = 0, quiet = 0;
    bool guard = false;         // speed31: the speed guard was dropping pictures in or just before this window: it is never calm
};

enum class Verdict : uint8_t { Ignored, Hot, Calm, Middle };

inline float MeanMs(const Window& w) { return w.frontPics ? static_cast<float>(static_cast<double>(w.frontSumUs) / (1000.0 * w.frontPics)) : 0.0f; }

class Governor {
public:
    Governor() = default;
    explicit Governor(const Config& c) : c_(c), downNeed_(c.downWindows) { ResetGains(); if (level_ < MinLevel()) level_ = MinLevel(); }   // (c161: the floor)
    void Configure(const Config& c) {
        c_ = c;
        Reset();
    }
    // (c161) the floor in force: never above the cap, never below 0
    int MinLevel() const { const int m = c_.minLevel < c_.maxLevel ? c_.minLevel : c_.maxLevel; return m < 0 ? 0 : (m > kMaxLevel ? kMaxLevel : m); }
    // New thresholds (read again every window, so a stage preset's key acts at once); the state is kept.
    void Update(const Config& c) {
        c_ = c;
        if (level_ < MinLevel()) level_ = MinLevel();   // (c161: a floor that rose, e.g. a stage key: the dead steps are skipped, nothing is cut by it)
        if (downNeed_ < c_.downWindows) downNeed_ = c_.downWindows;
        if (downNeed_ > c_.downWindowsMax) downNeed_ = c_.downWindowsMax < c_.downWindows ? c_.downWindows : c_.downWindowsMax;
    }
    // A new stage (or any new situation): the level, every run and the learned gains start again.
    void Reset() {
        level_ = c_.startLevel < 0 ? 0 : (c_.startLevel > kMaxLevel ? kMaxLevel : c_.startLevel);
        if (level_ < MinLevel()) level_ = MinLevel();   // (c161)
        hotRun_ = calmRun_ = 0;
        sinceDown_ = 1000000;
        downNeed_ = c_.downWindows;
        changes_ = 0;
        pendLevel_ = 0;
        pendAge_ = 0;
        pendDownN_ = 0;
        forcedLo_ = forcedHi_ = 0;
        ResetGains();
    }
    bool Judge(const Window& w, Verdict* v, float* mean) const {
        *v = Verdict::Ignored;
        *mean = MeanMs(w);
        if (w.frontPics < c_.minPictures || w.pictures == 0) return false;
        const uint64_t notPlay = static_cast<uint64_t>(w.quiet) + w.stalls;
        if (notPlay * 100u >= static_cast<uint64_t>(c_.quietPct) * (static_cast<uint64_t>(w.pictures) + w.quiet)) return false;
        if (*mean > c_.budgetMs) *v = Verdict::Hot;
        else if (*mean + GainOf(level_) <= c_.budgetMs - c_.bandMs) *v = w.guard ? Verdict::Middle : Verdict::Calm;
        else *v = Verdict::Middle;
        return true;
    }
    // Takes a window; returns true when the level changed.
    bool Feed(const Window& w) {
        if (c_.maxLevel <= 0) return false;
        Verdict v;
        float mean;
        Judge(w, &v, &mean);
        if (sinceDown_ < 1000000) ++sinceDown_;
        if (c_.recover && downNeed_ > c_.downWindows && sinceDown_ == c_.halveWindows) downNeed_ = downNeed_ / 2 < c_.downWindows ? c_.downWindows : downNeed_ / 2;   // (recover30: a doubling is forgiven slowly)
        last_ = v;
        lastMean_ = mean;
        if (v == Verdict::Ignored) return false;  // a load or a pause does not make a run of anything
        // the gain of the last raise: the second valid window after it against the mean before it
        if (pendLevel_ > 0) {
            if (++pendAge_ >= 2) {
                const float g = pendBefore_ - mean;
                float& slot = gain_[pendLevel_];
                const float clamped = g < 0.0f ? 0.0f : (g > 6.0f ? 6.0f : g);
                slot = gainKnown_[pendLevel_] ? 0.5f * slot + 0.5f * clamped : clamped;
                gainKnown_[pendLevel_] = true;
                pendLevel_ = 0;
            }
        }
        // recover30: the COST of the levels given back: the mean two valid windows after the lowering minus the mean before it, shared by their gains
        if (c_.recover && pendDownN_ > 0 && v != Verdict::Ignored) {
            if (++pendDownAge_ >= 2) {
                float sumG = 0;
                for (int l = pendDownLo_; l <= pendDownHi_; ++l) sumG += GainOf(l);
                const float cost = mean - pendDownBefore_;
                for (int l = pendDownLo_; l <= pendDownHi_; ++l) {
                    const float share = sumG > 0 ? cost * GainOf(l) / sumG : cost / pendDownN_;
                    const float clamped = share < 0.0f ? 0.0f : (share > 6.0f ? 6.0f : share);
                    gain_[l] = gainKnown_[l] ? 0.5f * gain_[l] + 0.5f * clamped : clamped;
                    gainKnown_[l] = true;
                }
                pendDownN_ = 0;
            }
        }
        switch (v) {
            case Verdict::Hot: {
                calmRun_ = 0;
                const int span = c_.recover && c_.retakeWindows > c_.backoffSpan ? c_.retakeWindows : c_.backoffSpan;
                const bool failedProbe = c_.recover && sinceDown_ <= span;   // (recover30: the lowering just failed: no second hot window is needed to take the level again)
                if (sinceDown_ <= span) {
                    downNeed_ = downNeed_ * 2 > c_.downWindowsMax ? c_.downWindowsMax : downNeed_ * 2;
                    sinceDown_ = 1000000;
                }
                if (failedProbe) hotRun_ = c_.upWindows < 1 ? 1 : c_.upWindows;
                else ++hotRun_;
                if (hotRun_ >= (c_.upWindows < 1 ? 1 : c_.upWindows) && level_ < c_.maxLevel && level_ < kMaxLevel) {
                    ++level_;
                    hotRun_ = 0;
                    ++changes_;
                    pendLevel_ = level_;
                    pendAge_ = 0;
                    pendBefore_ = mean;
                    return true;
                }
                return false;
            }
            case Verdict::Calm: {
                hotRun_ = 0;
                if (level_ > MinLevel()) ++calmRun_;   // (c161: not under the floor)
                if (level_ > MinLevel() && calmRun_ >= NeedWindows(mean)) {
                    const int from = level_;
                    --level_;
                    float left = c_.budgetMs - c_.bandMs - (mean + GainOf(from));   // the room that is left after the first level
                    if (c_.recover && c_.multi)
                        while (level_ > MinLevel() && left >= GainOf(level_)) { left -= GainOf(level_); --level_; }
                    calmRun_ = 0;
                    sinceDown_ = 0;
                    ++changes_;
                    pendLevel_ = 0;
                    if (c_.recover) { pendDownLo_ = level_ + 1; pendDownHi_ = from; pendDownN_ = from - level_; pendDownAge_ = 0; pendDownBefore_ = mean; }
                    return true;
                }
                return false;
            }
            case Verdict::Middle:
                hotRun_ = 0;
                calmRun_ = 0;  // a calm run needs consecutive calm windows
                return false;
            case Verdict::Ignored:
                return false;
        }
        return false;
    }
    // recover30: the calm windows a lowering needs now: downNeed_ (today's wait, doubled by a failed lowering) at no room, shrinking to downMinWindows / downWindows of it at fullMarginMs of room
    int NeedWindows(float mean) const {
        if (!c_.recover || c_.fullMarginMs <= 0.0f || c_.downWindows <= c_.downMinWindows) return downNeed_;
        const float room = c_.budgetMs - c_.bandMs - (mean + GainOf(level_));
        const float f = 1.0f - (room <= 0.0f ? 0.0f : (room >= c_.fullMarginMs ? 1.0f : room / c_.fullMarginMs));
        const float base = static_cast<float>(c_.downMinWindows) + static_cast<float>(c_.downWindows - c_.downMinWindows) * f;
        const int n = static_cast<int>(base * static_cast<float>(downNeed_) / static_cast<float>(c_.downWindows) + 0.999f);
        return n < 1 ? 1 : n;
    }
    // rate30 (GXV_RATE_AUTO): the rate controller asks for a level now, without the hot windows (it holds turn-level evidence). Returns true when the level changed. The governor's own rules take over
    // from here: its give-back (recover30: learned cost, peak test, anti-flap) walks the level down when the room allows. The levels taken are ForcedLo()..ForcedHi(); their gain is not
    // learned from the windows (the window before the jump is not the load that caused it): the rate controller measures it (its lanes before and after) and hands it to LearnJump.
    bool ForceLevel(int l) {
        if (c_.maxLevel <= 0) return false;
        if (l > c_.maxLevel) l = c_.maxLevel;
        if (l > kMaxLevel) l = kMaxLevel;
        if (l <= level_) return false;
        forcedLo_ = level_ + 1;
        forcedHi_ = l;
        level_ = l;
        hotRun_ = calmRun_ = 0;
        pendLevel_ = 0;
        pendDownN_ = 0;
        ++changes_;
        return true;
    }
    // speed31 (GXV_GOV_GUARD): the speed guard says its repayment is sustained (pictures are being dropped to hold the game's speed): ONE level now, without waiting for two hot windows (10 s). The anti-flap
    // is the hot window's own: a raise within the retake span of a lowering doubles the next lowering's wait; the level's gain is measured as for a window's raise (the mean the window before it, the
    // second valid window after). `needMs`: how much the guard repays a picture; with a learned gain of the next level under it the next level goes too (at most `maxLevels` in one call).
    // Returns the levels taken (0 = none: off, at the maximum).
    int Press(float needMs, int maxLevels) {
        if (c_.maxLevel <= 0 || level_ >= c_.maxLevel || level_ >= kMaxLevel) return 0;
        const int span = c_.recover && c_.retakeWindows > c_.backoffSpan ? c_.retakeWindows : c_.backoffSpan;
        if (sinceDown_ <= span) {
            downNeed_ = downNeed_ * 2 > c_.downWindowsMax ? c_.downWindowsMax : downNeed_ * 2;
            sinceDown_ = 1000000;
        }
        int taken = 0;
        float sum = 0;
        do {
            ++level_;
            ++taken;
            sum += GainOf(level_);
        } while (taken < maxLevels && sum < needMs && level_ < c_.maxLevel && level_ < kMaxLevel);
        hotRun_ = calmRun_ = 0;
        pendLevel_ = taken == 1 ? level_ : 0;   // (one level: measured as a window's raise; several: not measured, the guesses stay)
        pendAge_ = 0;
        pendBefore_ = lastMean_;
        changes_ += static_cast<uint32_t>(taken);
        return taken;
    }
    // anticip32 (GXV_ANTICIPATE): the cuts were moved from outside (a pre-scale ahead of an announced heavy moment came or went): a gain or a cost being measured across that is not the
    // level's own, so it is not learned (the guesses and the earlier measurements stay).
    void VoidPending() { pendLevel_ = 0; pendDownN_ = 0; }
    int ForcedLo() const { return forcedLo_; }
    int ForcedHi() const { return forcedHi_; }
    // totalMs: the measured gain of the levels ForcedLo()..ForcedHi() together (shared by their assumed / learned gains, as the give-back's cost is)
    void LearnJump(float totalMs) {
        if (forcedHi_ < forcedLo_ || forcedLo_ < 1) return;
        float sumG = 0;
        for (int l = forcedLo_; l <= forcedHi_; ++l) sumG += GainOf(l);
        const int n = forcedHi_ - forcedLo_ + 1;
        for (int l = forcedLo_; l <= forcedHi_; ++l) {
            const float share = sumG > 0 ? totalMs * GainOf(l) / sumG : totalMs / n;
            const float clamped = share < 0.0f ? 0.0f : (share > 6.0f ? 6.0f : share);
            gain_[l] = gainKnown_[l] ? 0.5f * gain_[l] + 0.5f * clamped : clamped;
            gainKnown_[l] = true;
        }
        forcedLo_ = forcedHi_ = 0;
    }
    int level() const { return level_ < c_.maxLevel ? level_ : c_.maxLevel; }
    int downNeed() const { return downNeed_; }
    int calmRun() const { return calmRun_; }
    int hotRun() const { return hotRun_; }
    uint32_t changes() const { return changes_; }
    Verdict last() const { return last_; }
    float lastMean() const { return lastMean_; }
    // the ms that giving level `l` back is expected to cost (measured at the raise to it, else the default)
    float GainOf(int l) const { return l >= 1 && l <= kMaxLevel ? (gainKnown_[l] ? gain_[l] : c_.defaultGainMs) : 0.0f; }
    bool GainKnown(int l) const { return l >= 1 && l <= kMaxLevel && gainKnown_[l]; }
    const Config& config() const { return c_; }

private:
    void ResetGains() {
        for (int i = 0; i <= kMaxLevel; ++i) {
            gain_[i] = 0.0f;
            gainKnown_[i] = false;
        }
    }
    Config c_{};
    int level_ = 0, hotRun_ = 0, calmRun_ = 0, sinceDown_ = 1000000, downNeed_ = 6;
    int pendLevel_ = 0, pendAge_ = 0;
    int pendDownLo_ = 0, pendDownHi_ = 0, pendDownN_ = 0, pendDownAge_ = 0;
    float pendDownBefore_ = 0.0f;
    int forcedLo_ = 0, forcedHi_ = 0;   // (rate30)
    float pendBefore_ = 0.0f;
    float gain_[kMaxLevel + 1] = {};
    bool gainKnown_[kMaxLevel + 1] = {};
    uint32_t changes_ = 0;
    Verdict last_ = Verdict::Ignored;
    float lastMean_ = 0.0f;
};

// ---- revB (12 Nov): the game-thread governor's measure --------------------------------------------------------------------------------------
// GXV_GAME_GOV reads the game thread's CPU time (the Vita's SceKernelThreadInfo::runClocks, vita/runtime/memory.cpp port_thread_cpu_us). That counter is brought up to date ONLY
// when the thread leaves its core. A sample taken at the END of a turn (before the retrace wait) therefore lacks everything since the thread's last wait inside the turn (the
// wait for the front end at its start): "end - begin" of one turn missed most of the turn, and the part it missed fell between the end sample and the next begin sample, which no
// window summed. Samples taken right after a wait are exact, so the turn's cost is the difference of two consecutive turn-BEGIN samples (the retrace wait in between costs no
// CPU): the sums then telescope and hold over a window whatever the counter's laziness. The turn is counted when it was a drawn one (the flag of the turn that ENDED at this
// begin). A turn longer than capUs (a load, a pause, a hitch) is left out of the sum and the count. prev <= 0: no sample before (start, a stage's load): nothing is counted.
inline bool TurnCpuDelta(int64_t prevBeginUs, int64_t nowUs, bool prevWasDrawn, int64_t capUs, int64_t* outUs) {
    if (prevBeginUs <= 0 || nowUs < prevBeginUs || !prevWasDrawn) return false;
    const int64_t d = nowUs - prevBeginUs;
    if (d > capUs) return false;
    *outUs = d;
    return true;
}

// The far-draw cut (cpugov.h levels 0..5) the governor asks for at its level: cpulod's far-draw row (8 / 12 lines, classes 31) is matched by the
// nearest rows of the far-draw governor's table (10 lines, classes 11 / 13 lines, classes 27). Used by port_cpugov_level (the larger of the two governors).
inline int FarDrawLevel(int level) { return level <= 0 ? 0 : (level == 1 ? 3 : (level == 2 ? 4 : 5)); }

// ---- how a cut combines its static value (the stage's, the environment's or the umbrella's) with the governor's -----------------------------
// All of them return the static value unchanged when the governor is at level 0. "Stricter" is the direction that cuts more.
// A count (MATLOD / FAMPOSE / PIECETHIN / CHARPOSE n): the larger, the governor's when the static one is off (0 or 1).
inline int CountMax(int staticN, int govN) { return govN >= 2 && govN > staticN ? govN : staticN; }
// A distance or a threshold that belongs to such a count: where the static count is active the smaller (the cut reaches nearer), else the governor's.
inline float DistFor(int staticN, float staticDist, int govN, float govDist) {
    if (govN < 2) return staticDist;
    if (staticN < 2) return govDist;
    return govDist < staticDist ? govDist : staticDist;
}
// A "more is stricter" size (CHARPOSE_LINES, FAMPOSE_MOVE): the larger where the static count is active, else the governor's.
inline float SizeFor(int staticN, float staticV, int govN, float govV) {
    if (govN < 2) return staticV;
    if (staticN < 2) return govV;
    return govV > staticV ? govV : staticV;
}
// A fraction kept (PTCL_THIN): 0 = off; the smaller of two active ones.
inline float KeepMin(float staticKeep, float govKeep) {
    if (!(govKeep > 0.0f)) return staticKeep;
    if (!(staticKeep > 0.0f)) return govKeep;
    return govKeep < staticKeep ? govKeep : staticKeep;
}

}  // namespace fegov
}  // namespace port
