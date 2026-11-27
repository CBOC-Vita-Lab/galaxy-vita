#pragma once
// GXV_PACE25 (pacer25): the "Up to 60" skip rule's cadence when the work per picture lies between one and two refreshes.
//
// WHY (console tour c69, Toy Time, GXV_PRESENT_SYNC=2): with every picture shown exactly L vblanks after its turn, the hold on screen IS the number of
// turns the picture advanced ("held-vs-step off by 0" for 95 % of the pictures once the pipeline got faster). The turns a picture advances are decided by
// port::pace::LegacySkip: a turn is left out when the back end holds `pending` >= 2 pictures it has not finished (code 1), or the game is half a refresh
// behind the display (code 2). `pending` is a delayed, quantised signal: a picture is invisible to it from its draw until the front end has queued it
// (~1 refresh), and it falls only when the render thread has finished a picture, which with the display callback holding pictures to their slots
// is a vblank-quantised event (the render thread's first scene waits for the buffer of the picture two before it to leave the screen). A bang-bang
// control with that delay and a one-turn sampling step overshoots: it draws 3-4 turns in a row (the queue is empty), then pending reads >= 2 and it
// leaves out TWO turns (GXV_SKIP_MAXSTEP=3 allows two in a row), a hold of 3 vblanks (50 ms), then draws 3-4 in a row again: 1-1-1-3.
// Work between 1 and 2 refreshes cannot be 1-1-1-1; an even rhythm of 1s and 2s exists (1-2, 1-1-2, 1-1-1-2, ...) and costs the same pictures a second.
//
// WHAT: Pace25 replaces the pending-driven choice of WHICH turns draw by a fixed even rhythm (Bresenham spread) whose mean hold p/q is picked from the
// measured cost per picture, so that no hold exceeds ceil(p/q) (never 3 while the work fits 2 refreshes), and keeps pending only as a guard at a higher
// threshold. A scene that fits one refresh (level 0) is not touched at all: every decision is the legacy rule's, strict 60, no added latency.
//
// The decision logic here is pure (no clock, no globals): port/perf.cpp feeds it, tests/pace25_model.cpp replays traces from the console logs.
#include <cstdint>
#include <cstdio>

namespace port::pace {

// Mean hold p/q turns per picture. Level 0: every turn (60 pictures a second, the legacy rule decides). Levels 1..16: 1 + i/16 turns (56.5 .. 30 a second),
// 17..24: i/8 (28 .. 20 a second). The turns a rhythm draws are spread evenly (Bresenham), so its holds are floor(p/q) and ceil(p/q) only, the long ones
// as far apart as they can be.
struct Pace25Level { unsigned p, q; };
inline constexpr unsigned kPace25Levels = 25;
inline constexpr Pace25Level Pace25LevelAt(unsigned i) {
    return i == 0 ? Pace25Level{1, 1} : i <= 16 ? Pace25Level{16 + i, 16} : Pace25Level{i < kPace25Levels ? i : kPace25Levels - 1, 8};
}

struct Pace25Config {
    int mode = 0;               // 0 off (every call answers the legacy rule), 1 act, 2 census (decides and counts, answers the legacy rule)
    double marginPct = 3.0;     // headroom on the slowest stage's cost per picture (the game's own cost is exact; the stages overlap imperfectly)
    double deadPct = 3.0;       // a cost (with the headroom) within this of one refresh stays at level 0 (strict 60, the legacy rule decides)
    unsigned window = 30;       // turns per evidence window (0.5 s)
    unsigned guardDepth = 3;    // pictures the back end holds at which a scheduled picture is left out (the legacy rule's own pipeDepth is 2)
    unsigned contraMax = 2;     // guard hits in a window that push the level up
    unsigned upWindows = 2;     // windows the estimate must ask for a higher level before it is taken (a hit takes it at once)
    unsigned downWindows = 8;   // windows without a guard hit (and an estimate below) before the level comes down one step (4 s)
    unsigned behindForce = 4;   // consecutive turns the display clock is behind before one scheduled picture is left out anyway (game speed)
    unsigned backoffBase = 12, backoffMax = 192;  // windows a lowering is held off after one that failed (doubles)
};

class Pace25 {
public:
    static constexpr int64_t kPeriodUs = 16683;
    struct Turn {
        int64_t nowUs;
        bool allowed;          // false: a load or scene change (the turn draws, nothing is measured)
        bool quiet;            // a pause menu or closed fade: unmeasured, the legacy rule answers
        int legacy;            // what port::pace::LegacySkip decided: 0 draw, 1 leave out and wait for the retrace, 2 leave out late (no wait)
        bool legacyBehind;     // ... because the display clock is half a refresh ahead of the game
        unsigned pending;      // pictures queued to the back end that it has not finished (port_vgx_backend_pending)
        uint32_t pipeUs;       // the slowest stage's time per picture (front end / renderer / GPU, port_autoq_stages); 0 unknown
    };
    struct Stats {
        uint64_t turns = 0, draws = 0, legacyDraws = 0, differ = 0, guardHits = 0, behindForced = 0, levelUps = 0, levelDowns = 0, failedLowers = 0;
        uint64_t hold[5] = {};  // the rhythm's own holds in turns (index 4: 4 or more); in act mode what the screen gets, in census mode what it would get
    };

    void Configure(const Pace25Config& c) {
        cfg_ = c;
        level_ = 0;
        upRun_ = quiet_ = probation_ = failed_ = backoff_ = 0;
        windowsSinceDown_ = 0;
        Reset();
        stats_ = Stats();
    }
    bool Enabled() const { return cfg_.mode != 0; }
    bool Acting() const { return cfg_.mode == 1; }
    unsigned Level() const { return level_; }
    double MeanHold() const { return static_cast<double>(Pace25LevelAt(level_).p) / Pace25LevelAt(level_).q; }
    unsigned MaxHold() const { const Pace25Level l = Pace25LevelAt(level_); return (l.p + l.q - 1) / l.q; }
    double Target() const { return target_; }
    double DrawnUs() const { return dUs_; }
    double UndrawnUs() const { return nUs_; }
    const Stats& GetStats() const { return stats_; }
    void ClearStats() { stats_ = Stats(); }

    // The decision for one turn: 0 draw, 1 / 2 as the legacy codes.
    int Decide(const Turn& t) {
        if (!cfg_.mode) return t.legacy;
        lastPipeUs_ = t.pipeUs;
        const bool gap = started_ && (t.nowUs < lastUs_ || t.nowUs - lastUs_ > 250000);
        lastUs_ = t.nowUs;
        if (!t.allowed || gap || !started_) {
            started_ = true;
            Reset();
            return t.legacy;
        }
        if (t.quiet) { open_ = false; return t.legacy; }
        open_ = true;
        ++turns_;
        ++stats_.turns;
        behindRun_ = t.legacyBehind ? behindRun_ + 1 : 0;
        int code = t.legacy;
        bool draw = t.legacy == 0;
        if (level_ != 0) {
            const Pace25Level l = Pace25LevelAt(level_);
            acc_ += l.q;
            draw = acc_ >= l.p;
            if (draw) acc_ -= l.p;
            bool guard = false, forced = false;
            if (draw) {
                // (a skip now makes this hold skipRun_ + 2 turns: not over the rhythm's own largest)
                const bool room = skipRun_ + 2 <= MaxHold();
                if (t.pending >= cfg_.guardDepth && room) guard = true;
                else if (t.legacyBehind && behindRun_ >= cfg_.behindForce) forced = true;  // (the game thread cannot keep 60 turns a second at this rhythm)
                if (guard || forced) { draw = false; acc_ += l.p; }
            }
            if (guard) { ++contra_; ++stats_.guardHits; }
            if (forced) { ++contra_; ++stats_.behindForced; behindRun_ = 0; }
            code = draw ? 0 : (t.legacyBehind || forced ? 2 : 1);
        }
        if (t.legacy == 0) ++stats_.legacyDraws;
        if ((code == 0) != (t.legacy == 0)) ++stats_.differ;
        Note(draw);
        return cfg_.mode == 1 ? code : t.legacy;
    }

    // The end of the turn's work (before the retrace wait). drew: the turn drew a picture (what Decide returned).
    void WorkDone(int64_t nowUs, bool drew) {
        if (!cfg_.mode || !open_) return;
        open_ = false;
        int64_t w = nowUs >= lastUs_ ? nowUs - lastUs_ : 0;
        if (w > 4 * kPeriodUs) w = 4 * kPeriodUs;
        if (drew) { sumD_ += w; ++nD_; } else { sumN_ += w; ++nN_; }
        if (turns_ >= cfg_.window) Evaluate();
    }

    static int Format(char* out, size_t n, const Pace25& p) {
        const Stats& s = p.stats_;
        const Pace25Level l = Pace25LevelAt(p.level_);
        return std::snprintf(out, n,
            "pace25: %s, level %u (a picture every %u/%u turns = %.1f a second, no hold over %u turns), cost estimate %.2f turns (drawn turn %.1f ms, other %.1f ms, slowest stage %.1f ms), "
            "turns %u, pictures %u (legacy rule %u), turns decided differently %u, guard hits %u, game-speed forced %u, level up/down %u/%u (failed lowers %u), "
            "holds in turns 1/2/3/4+ %u/%u/%u/%u",
            p.cfg_.mode == 2 ? "census (decides, the legacy rule acts)" : "acting", p.level_, l.p, l.q, 60.0 * l.q / l.p, p.MaxHold(), p.target_, p.dUs_ / 1000.0, p.nUs_ / 1000.0,
            p.lastPipeUs_ / 1000.0, static_cast<unsigned>(s.turns), static_cast<unsigned>(s.draws), static_cast<unsigned>(s.legacyDraws), static_cast<unsigned>(s.differ),
            static_cast<unsigned>(s.guardHits), static_cast<unsigned>(s.behindForced), static_cast<unsigned>(s.levelUps), static_cast<unsigned>(s.levelDowns),
            static_cast<unsigned>(s.failedLowers), static_cast<unsigned>(s.hold[1]), static_cast<unsigned>(s.hold[2]), static_cast<unsigned>(s.hold[3]), static_cast<unsigned>(s.hold[4]));
    }

private:
    void Reset() {
        const Pace25Level l = Pace25LevelAt(level_);
        acc_ = l.p - l.q;  // (the next turn draws)
        skipRun_ = 0;
        behindRun_ = 0;
        turns_ = 0;
        contra_ = 0;
        sumD_ = sumN_ = 0;
        nD_ = nN_ = 0;
        open_ = false;
        haveDraw_ = false;
    }
    void Note(bool draw) {
        if (draw) {
            ++stats_.draws;
            if (haveDraw_) { const unsigned h = skipRun_ + 1; ++stats_.hold[h < 4 ? h : 4]; }
            haveDraw_ = true;
            skipRun_ = 0;
        } else {
            ++skipRun_;
        }
    }
    unsigned LevelFor(double target) const {
        if (target <= 1.0 + cfg_.deadPct / 100.0) return 0;
        unsigned l = 1;
        while (l + 1 < kPace25Levels && static_cast<double>(Pace25LevelAt(l).p) / Pace25LevelAt(l).q < target) ++l;
        return l;
    }
    void Evaluate() {
        const double r = static_cast<double>(kPeriodUs);
        double gameM = 0.0;
        // (the cost of a drawn turn d and of the others n, smoothed over windows: a window at a long rhythm holds only one or two turns of the second kind)
        if (nD_ >= 2) { const double d = static_cast<double>(sumD_) / nD_; dUs_ = dUs_ > 0.0 ? 0.6 * dUs_ + 0.4 * d : d; }
        if (nN_ >= 1) { const double n = static_cast<double>(sumN_) / nN_; nUs_ = nUs_ > 0.0 ? 0.6 * nUs_ + 0.4 * n : n; }
        // (to keep 60 turns a second the drawn share s satisfies s d + (1 - s) n <= R: a mean hold of at least (d - n) / (R - n) turns)
        if (dUs_ > r) gameM = (nUs_ > 0.0 && nUs_ < r) ? (dUs_ - nUs_) / (r - nUs_) : 3.0;
        const double pipeM = lastPipeUs_ ? static_cast<double>(lastPipeUs_) / r : 0.0;
        const double pipeT = pipeM * (1.0 + cfg_.marginPct / 100.0);
        target_ = gameM > pipeT ? gameM : pipeT;
        // (the pending-driven skips, and with them the 1-1-1-3, belong to a pipeline that is the slowest part: when the game thread itself needs the skips
        //  (gameM above the stages' cost) the legacy rule's debt rule makes a clean 1 / 2 mix with no added latency, and it is left alone)
        const unsigned est = pipeT > gameM ? LevelFor(target_) : 0u;
        if (windowsSinceDown_ < 1000000u) ++windowsSinceDown_;
        if (backoff_) --backoff_;
        const bool hit = contra_ >= cfg_.contraMax;
        quiet_ = contra_ == 0 ? quiet_ + 1 : 0;
        if (hit) {
            // (the pictures the rhythm asks for do not fit: the next step up at once; a lowering that failed backs off)
            if (probation_) {
                ++stats_.failedLowers;
                failed_ = failed_ < 8 ? failed_ + 1 : 8;
                unsigned b = cfg_.backoffBase << failed_;
                backoff_ = b > cfg_.backoffMax ? cfg_.backoffMax : b;
                probation_ = 0;
            }
            SetLevel(est > level_ + 1 ? est : level_ + 2);
            upRun_ = 0;
        } else if (est > level_) {
            if (++upRun_ >= cfg_.upWindows) { SetLevel(est); upRun_ = 0; }
        } else {
            upRun_ = 0;
            if ((est == 0 || est + 1 < level_) && quiet_ >= cfg_.downWindows && backoff_ == 0) {  // (a dead band of one step: the estimate's jitter does not move it)
                SetLevel(est == 0 ? 0 : level_ - 1);
                probation_ = cfg_.downWindows;
                quiet_ = 0;
            }
        }
        if (probation_ && !hit) --probation_;
        if (probation_ == 0 && windowsSinceDown_ > 1200) failed_ = 0;  // (a long quiet at one level forgives the failed lowerings)
        turns_ = 0;
        contra_ = 0;
        sumD_ = sumN_ = 0;
        nD_ = nN_ = 0;
    }
    void SetLevel(unsigned l) {
        if (l >= kPace25Levels) l = kPace25Levels - 1;
        if (l == level_) return;
        if (l > level_) ++stats_.levelUps; else { ++stats_.levelDowns; windowsSinceDown_ = 0; }
        level_ = l;
        const Pace25Level p = Pace25LevelAt(level_);
        if (acc_ >= p.p) acc_ = p.p - p.q;
    }

    Pace25Config cfg_;
    Stats stats_;
    unsigned level_ = 0, skipRun_ = 0, behindRun_ = 0, turns_ = 0, contra_ = 0, upRun_ = 0, quiet_ = 0, probation_ = 0, failed_ = 0, backoff_ = 0;
    unsigned windowsSinceDown_ = 0;
    unsigned acc_ = 0;
    int64_t lastUs_ = 0, sumD_ = 0, sumN_ = 0;
    unsigned nD_ = 0, nN_ = 0;
    double dUs_ = 0, nUs_ = 0, target_ = 1.0;
    uint32_t lastPipeUs_ = 0;
    bool started_ = false, open_ = false, haveDraw_ = false;
};

}  // namespace port::pace
