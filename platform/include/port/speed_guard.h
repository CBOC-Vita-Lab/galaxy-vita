#pragma once
// GXV_SPEED_GUARD (speed30, 13 Nov 2026): a game-speed guard. Pure logic (no clock, no globals but GlobalSpeedGuard): tests/speed_guard_test.cpp runs this code.
//
// WHY. The game runs fixed 1/60 s turns. When the work of a picture's turns does not fit the refreshes the pacing rule gives it (Quality: a pair of turns, one drawn and
// one not, against 33.4 ms; Performance: a picture at least every GXV_SKIP_MAXSTEP turns), nothing in the rules pays the missing time back: the turns simply run back to
// back and the game clock falls behind the display's. The picture stays smooth, so nobody sees it, and the game plays in slow motion (console c90: Quality Honeyhive 83.7 % of
// real speed, Beach Bowl 96.5 %, Dreadnought 98.9 %; fps and frame-time graphs cannot show it). Strikers found the same in its slip rule and fixed it with a leaky bucket.
//
// WHAT. A leaky bucket of game time lost against the display's clock: every counted turn adds (its length - 16.683 ms), at most two refreshes of it, and leaks 2 % of the
// wall time. Short overloads stay in the bucket (the slow-down of a hitch, a brief heavy moment: absorbed as before, nobody sees it). When the bucket is over its budget
// (GXV_SPEED_GUARD_MS, default 30) the pacing rule may leave ONE more turn without a picture for the picture it is about to make (Quality: a three-turn picture, a
// 50 ms hold; Performance: the step cap becomes 3 for that picture). That turn runs without the retrace wait (code 2, as the late catch-up), so it costs about the work of an
// undrawn turn (~12 ms) and gives back (16.683 - 12) ms of the lost time: the extra turns are paid back, one by one, until the bucket is under its budget again.
// What cannot be repaid with one extra turn (a pair that costs 41 ms: a three-turn picture costs ~53 ms for 50 ms of game time) keeps the bucket over its budget: the guard
// then makes every picture a three-turn one (20 pictures a second at ~94 % speed). GXV_SPEED_GUARD_MAXSHARE=<percent> limits the extra turns to that share of the pictures
// (25 = at most one in four, the others stay two-turn pictures: speed in between, rhythm more even); 100 = no limit = exact speed first.
//
// WHAT DOES NOT COUNT (the bucket does not take it, and the census does not count it): a turn the game marks as loading / scene change (allowed false), a covered screen
// (a wipe, a menu: quiet), the first GXV_SPEED_GUARD_GRACE_MS (3000) after any of those (the first seconds of a stage: shader compiles, first-use uploads), and any gap between
// two turn starts over 80 ms (a stall: GXV_STALL_FORGIVE's threshold; whether it is on or off). Leaving such a turn also empties the bucket.
//
// speed31 (13 Nov, the owner's rule: game speed stays at real time, a dropped picture is a fault to be removed by the cuts): ESCALATION (GXV_SPEED_GUARD_ESC, default 1; 0 = speed30's one
// extra turn, bit for bit). The guard learns what a picture costs it: per picture the NATURAL loss (the game time lost over the picture's own turns, the extra turns left out) and
// what one extra turn gives back (eps = refresh - the extra turn's own length, learned: about 4.7 ms). The extra turns of the next picture are then
//     e = floor(carry + ff + fb),  ff = (the smaller of the last two pictures' natural loss - the leak of the picture) / eps,  fb = (bucket - budget/2) / (eps * 3),
// carry = the rest of the last division (error diffusion: a fractional need makes a regular 2-3-2-3 pattern, never a random one). ff is the feed-forward (a pair of 60 ms needs ~5.4
// extra turns a picture: it takes them from the second picture on, not after the bucket has grown for a second); fb the bucket's feedback (it escalates only while the bucket stays above half the budget). The smaller of
// two pictures keeps ONE hitch from planning a long hold. Active from the bucket over its budget until it is under a quarter of it. While active the guard OWNS the repayment: the pacers'
// own late catch-up (debt, code-2 turns) is switched off (GXV_SPEED_GUARD_OWN, default 1), or the two would pay the same debt twice and produce uneven 2-2-4 holds.
// PRESSURE: the same estimate says whether the overload is SUSTAINED (the need of the last two pictures at least half an extra turn, for pressSec): the signal the cut systems read
// (Quality ladder, Performance governors, far field, rate controller) through Pressure(): an isolated hitch has no need after the hitch, so it is no signal.
//
// pace32 (13 Nov 21:30, the owner: "Whatever will feel the smoothest to the player ... try to create some overhead room"): the SLIP BAND (GXV_SPEED_GUARD_SLIP, default 0 = speed31, bit for
// bit). A load that is only a little over (Beach Bowl's measured pair 34.5 ms against 33.37: 3.3 %) makes the guard drop one picture in five or six: holds of 2 and 3 turns, 82 / 18 %, an
// uneven 27.6 pictures a second, to save 3 % of game speed nobody can see. With the band, at the moment the guard would start to drop (the bucket over its budget) and the measured natural loss
// is at most slipBandPermille (4.5 %: the 34.5 ms pair loses 3.4 % by arithmetic, 3.5 % on the console, 4.3 % in the host model, where a retrace wait every eleventh pair adds to it) of the wall
// time, the bucket forgives the loss instead: every picture stays a two-turn picture and the game runs that much slow. NOT perfectly even: a 34.5 ms picture on a 16.7 ms display is held three
// refreshes once in eleven to fifteen pictures whatever is done (about two a second, a steady beat), against one in three to six with dropped pictures. BOUNDED: an allowance of slipMaxNs (10 s) that is spent while slipping and refilled at a third of real time while not (never more than a quarter of the play time, so the long-run
// loss is under 1 %); when it is spent, or the loss leaves the band for three pictures, the guard drops pictures as before. The cut systems see a slip as pressure (Slipping(): perf.cpp
// presses the governors; the Quality ladder's own slip meter reads the turn gaps), so the cuts engage during the slip and remove its cause.
//
// Off (the default): nothing but the census runs; every decision of the pacers is as before (the pacers' own extra hooks are never armed).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf)

namespace port::pace {

class SpeedGuard {
public:
    static constexpr int64_t kPeriodNs = 16683350;           // one refresh (1001/60000 s)
    static constexpr int64_t kStallNs = 80000000;            // a gap between two turn starts over this is a stall: not counted
    static constexpr int64_t kMaxLostPerTurnNs = 2 * kPeriodNs;  // one turn adds at most this much lost time (a 70 ms hitch adds 33 ms, not 53)
    struct Config {
        bool on = false;
        int64_t budgetNs = 30000000;   // GXV_SPEED_GUARD_MS
        unsigned refillPermille = 20;  // the bucket leaks this share of the wall time (2 %)
        unsigned maxSharePct = 100;    // GXV_SPEED_GUARD_MAXSHARE: extra turns per 100 pictures at most (100 = no limit)
        unsigned maxExtra = 1;         // extra undrawn turns per picture (ESC off); with ESC the cap is maxExtraEsc
        int64_t graceNs = 3000000000ll;
        bool escalate = true;          // GXV_SPEED_GUARD_ESC: the extra turns follow the learned need (0 = speed30's one extra turn)
        unsigned maxExtraEsc = 8;      // GXV_SPEED_GUARD_EXTRA_MAX: extra turns a picture at most (8 = a ten-turn picture, 167 ms: a pair of 70 ms)
        bool own = true;               // GXV_SPEED_GUARD_OWN: while active the pacers' own late catch-up is off (Owns())
        int64_t pressNs = 500000000ll; // GXV_SPEED_GUARD_PRESS_MS: the need must last this long to be a signal
        double fbPictures = 8.0;       // the feedback's time constant, in pictures
        bool snap = true;              // GXV_SPEED_GUARD_SNAP: a nearly whole number of extra turns a picture is made exactly that (an even 20 / 15 / 12 a second)
        double snapNear = 0.2, snapFar = 0.45;   // enter within snapNear of a whole number, leave beyond snapFar
        unsigned snapPictures = 6;     // plans in a row within snapNear before the rhythm is locked
        // pace32: the slip band (see the header). All inert with slip false.
        bool slip = false;                 // GXV_SPEED_GUARD_SLIP
        unsigned slipBandPermille = 45;    // GXV_SPEED_GUARD_SLIP_PM: a natural loss up to this (per mille of the wall time, over the last 32 pictures) is slipped; over it pictures are dropped, as before
        int64_t slipMaxNs = 10000000000ll; // GXV_SPEED_GUARD_SLIP_S: the allowance (spent while slipping)
        unsigned slipRefillDiv = 3;        // the allowance comes back at 1/this of real time while not slipping (GXV_SPEED_GUARD_SLIP_REFILL_DIV)
        int64_t slipMinNs = 5000000000ll;  // a slip starts only with this much allowance (Configure: half of slipMaxNs)
    };
    // The census: what the counted turns did. lostNs = counted wall time - counted turns * 16.683 ms (negative when the turns ran faster than 60 a second).
    struct Census {
        uint64_t turns = 0, extras = 0, pictures = 0;
        uint64_t droppedPics = 0, longestRun = 0, cuts = 0;   // (speed31) pictures that took an extra turn; the longest run of them in a row; cuts the systems took because of the drops
        double worst1s = 100.0;                                // the lowest game speed (percent) over any counted second inside it
        int64_t wallNs = 0, exclNs = 0;
        int64_t slipNs = 0, slipLostNs = 0;                    // (pace32) wall time spent in the slip band, and the game time it forgave beyond the base leak
        uint32_t slipStarts = 0, slipSpent = 0, slipHeavy = 0; // slips begun; ended because the allowance was spent / because the loss left the band
        int64_t lostNs() const { return wallNs - static_cast<int64_t>(turns) * kPeriodNs; }
        double speedPct() const { return wallNs > 0 ? 100.0 * static_cast<double>(turns) * kPeriodNs / static_cast<double>(wallNs) : 100.0; }
    };

    void Configure(const Config& c) {
        c_ = c;
        if (c_.budgetNs < 5000000) c_.budgetNs = 5000000;
        if (c_.maxSharePct < 1) c_.maxSharePct = 1;
        if (c_.maxSharePct > 100) c_.maxSharePct = 100;
        if (c_.maxExtra < 1) c_.maxExtra = 1;
        if (c_.maxExtra > 2) c_.maxExtra = 2;
        capNs_ = c_.budgetNs * 4;
        credit_ = 1.0;
        if (c_.maxExtraEsc < 1) c_.maxExtraEsc = 1;
        if (c_.maxExtraEsc > 12) c_.maxExtraEsc = 12;
        if (c_.fbPictures < 1.0) c_.fbPictures = 1.0;
        if (c_.slipBandPermille > 60) c_.slipBandPermille = 60;
        if (c_.slipRefillDiv < 1) c_.slipRefillDiv = 1;
        if (c_.slipMaxNs < 0) c_.slipMaxNs = 0;
        c_.slipMinNs = c_.slipMaxNs / 2;
        slipping_ = false; slipAllowNs_ = c_.slipMaxNs; slipOver_ = 0; rateSlow_ = 0; rateN_ = 0; rateH_ = 0;
        epsNs_ = 4.7e6; need0_ = need1_ = 0; needN_ = 0; needSlow_ = 0; carry_ = 0; planned_ = -1; took_ = 0; active_ = false; snapM_ = -1; snapRun_ = 0; turnsIn_ = 0; lastK_ = 0;
        cycleLost_ = 0; cycleWall_ = 0; cycleExtras_ = 0; prevExtra_ = false; pressSince_ = 0; runNow_ = 0; ringN_ = 0; ringH_ = 0;
    }
    bool Enabled() const { return c_.on; }
    const Config& Cfg() const { return c_; }

    // The start of every turn (the game thread), before the pacing decision. allowed false: loading / scene change. quiet: the screen is covered (wipe) or a menu.
    void BeginTurn(int64_t nowNs, bool allowed, bool quiet) {
        const bool prevExtra = extraNow_;
        extraNow_ = false;
        if (haveLast_ && nowNs >= last_) {
            const int64_t dt = nowNs - last_;
            if (prevCounting_ && dt <= kStallNs) {
                int64_t lost = dt - kPeriodNs;
                if (lost > kMaxLostPerTurnNs) lost = kMaxLostPerTurnNs;
                bucket_ += lost - dt * static_cast<int64_t>(c_.refillPermille) / 1000;
                if (c_.slip) {   // (pace32) the slip band forgives the measured loss beyond the base leak, and its allowance is spent / refilled by the wall time
                    if (slipping_) {   // every loss in the band is forgiven: the bucket is held at half its budget, what is taken off it is counted
                        const int64_t half = c_.budgetNs / 2;
                        if (bucket_ > half) { win_.slipLostNs += bucket_ - half; tot_.slipLostNs += bucket_ - half; bucket_ = half; }
                        slipAllowNs_ -= dt;
                        win_.slipNs += dt; tot_.slipNs += dt;
                        if (slipAllowNs_ <= 0) { slipAllowNs_ = 0; EndSlip(false); }
                    } else if (slipAllowNs_ < c_.slipMaxNs) {
                        slipAllowNs_ = std::min<int64_t>(c_.slipMaxNs, slipAllowNs_ + dt / c_.slipRefillDiv);
                    }
                }
                if (bucket_ < 0) bucket_ = 0;
                if (bucket_ > capNs_) bucket_ = capNs_;
                ++win_.turns; ++tot_.turns;
                win_.wallNs += dt; tot_.wallNs += dt;
                // (speed31) what a picture costs, what an extra turn gives back, the game speed over the last second
                cycleWall_ += dt;
                if (!prevExtra) cycleLost_ += lost;   // (the natural loss: the extra turns' own gain is left out, or the estimate would count what they gave back as need: a ratchet upwards)
                if (prevExtra) {
                    const double eps = static_cast<double>(kPeriodNs - dt);
                    epsNs_ = 0.8 * epsNs_ + 0.2 * std::min(std::max(eps, 1.0e6), 16.0e6);
                }
                Ring1s(dt);
            } else {
                win_.exclNs += dt; tot_.exclNs += dt;
                ringN_ = 0; ringH_ = 0;
            }
        }
        const bool counting = allowed && !quiet;
        if (!counting) {
            graceUntil_ = nowNs + c_.graceNs;
            bucket_ = 0;
            undrawn_ = 0;  // (a scene change starts a picture sequence again)
            ResetCycle();
        }
        nowCounting_ = counting && nowNs >= graceUntil_;
        prevCounting_ = nowCounting_;
        last_ = nowNs;
        haveLast_ = true;
        nowNs_ = nowNs;
        if (c_.escalate) {   // active: from the bucket over its budget until it is under a quarter of it
            if (!nowCounting_) { active_ = false; if (slipping_) EndSlip(false, true); }
            else if (!active_ && bucket_ > c_.budgetNs) {
                // (pace32) the guard would start to drop pictures now: a small, measured loss is slipped instead while the allowance lasts
                if (c_.slip && !slipping_ && slipAllowNs_ >= c_.slipMinNs && rateN_ >= 8 && rateSlow_ > 0 && rateSlow_ <= c_.slipBandPermille) {
                    slipping_ = true; slipOver_ = 0;
                    ++win_.slipStarts; ++tot_.slipStarts;
                    const int64_t half = c_.budgetNs / 2;   // (the bucket's excess is part of the slip: forgiven once, counted)
                    if (bucket_ > half) { win_.slipLostNs += bucket_ - half; tot_.slipLostNs += bucket_ - half; bucket_ = half; }
                } else if (!slipping_) active_ = true;
            }
            else if (active_ && bucket_ < c_.budgetNs / 4 && pressSince_ == 0) { active_ = false; carry_ = 0; }
        }
    }
    // The pacers may take one more undrawn turn for the picture they are about to make.
    // Only for a picture that follows the rule's natural run of one undrawn turn (the pair): a picture that already follows more (the rules' own catch-up after a late turn:
    // up to 3 undrawn turns in a row) is not made later still.
    bool WantExtra() {
        if (!c_.on) return false;
        if (!c_.escalate)
            return nowCounting_ && bucket_ > c_.budgetNs && undrawn_ >= 1 && undrawn_ <= c_.maxExtra && (c_.maxSharePct >= 100 || credit_ >= 1.0);
        // (speed31) the extra turns of this picture: planned at its first due turn, taken one a turn while only the natural undrawn turn and the extras came before
        if (!nowCounting_ || !active_ || undrawn_ < 1 || undrawn_ != 1 + took_) return false;
        if (planned_ < 0) planned_ = Plan();
        return planned_ > 0 && took_ < static_cast<unsigned>(planned_);
    }
    // A pacer took the extra turn (it is this turn's final decision).
    void MarkExtra() {
        extraNow_ = true;
        ++took_;
        ++cycleExtras_;
        if (!c_.escalate && c_.maxSharePct < 100) credit_ -= 1.0;
        ++win_.extras; ++tot_.extras;
    }
    // Performance's post-filter (perf.cpp FrameskipDecide): the rule's final code for the turn (0 draws). A picture that follows at least one undrawn turn and would draw is
    // left out once more (code 2: no retrace wait, the turn is paid back at the speed of an undrawn turn) when the bucket is over its budget.
    int Filter(int code, bool allowed) {
        if (code == 0 && allowed && WantExtra()) {
            MarkExtra();
            return 2;
        }
        return code;
    }
    // The turn's final decision, whoever made it (0 = a picture is drawn).
    void EndDecision(int code) {
        ++turnsIn_;
        if (code == 0) {
            lastK_ = turnsIn_;
            turnsIn_ = 0;
            ++win_.pictures; ++tot_.pictures;
            undrawn_ = 0;
            if (c_.maxSharePct < 100) credit_ = std::min(1.0, credit_ + c_.maxSharePct / 100.0);
            if (c_.escalate && nowCounting_) PictureDone();
            took_ = 0; planned_ = -1;
        } else {
            ++undrawn_;
        }
    }
    // The guard owns the repayment of lost game time while it is active: the pacers' own debt (late catch-up turns) stays zero then (speed31, GXV_SPEED_GUARD_OWN).
    // Only when the guard CAN repay: the pictures come at most every second turn (the last picture followed an undrawn turn). With a picture on every turn (Performance at 60) there is no undrawn
    // turn to add to, and the rules' own skipped turns (their late catch-up) are the only repayment: they stay.
    // (pace32) while the slip band is in force the lost time is forgiven, so nobody repays it: the pacers' catch-up is off too (or THEY would drop the pictures the band is there to keep).
    bool Owns() const { return c_.on && c_.escalate && c_.own && (active_ || slipping_) && nowCounting_ && lastK_ >= 2; }
    // The overload is sustained: the natural loss of the last two pictures is at least half an extra turn over the leak, for pressNs. The signal for the cut systems.
    struct PressureInfo { bool sustained = false; double needMs = 0, heldSec = 0, extrasPerPicture = 0; };
    PressureInfo Pressure() const {
        PressureInfo p;
        if (!c_.on || !c_.escalate || pressSince_ == 0) return p;
        p.needMs = need1_ / 1e6;
        p.heldSec = nowNs_ > pressSince_ ? (nowNs_ - pressSince_) / 1e9 : 0.0;
        p.sustained = nowNs_ - pressSince_ >= c_.pressNs;
        p.extrasPerPicture = epsNs_ > 0 ? need1_ / epsNs_ : 0.0;
        return p;
    }
    // this turn is an extra one the guard took (valid from the pacer's decision to the next BeginTurn): the Quality ladder leaves it out of the game's pair
    bool ExtraNow() const { return extraNow_; }
    // (pace32) the slip band is in force: the game runs a little slow instead of dropping pictures; a pressure signal for the cut systems. AllowSec: what is left of the allowance.
    bool Slipping() const { return slipping_; }
    double SlipAllowSec() const { return slipAllowNs_ / 1e9; }
    double LossPermille() const { return rateSlow_; }
    double EpsMs() const { return epsNs_ / 1e6; }
    bool Active() const { return active_; }
    // A cut system took a step because of the pressure (the census counts it).
    void NoteCut() { ++win_.cuts; ++tot_.cuts; }
    bool CountingNow() const { return nowCounting_; }
    int64_t BucketNs() const { return bucket_; }
    unsigned UndrawnRun() const { return undrawn_; }
    const Census& Window() const { return win_; }
    const Census& Total() const { return tot_; }
    void ClearWindow() { win_ = Census(); }
    void ClearTotal() { tot_ = Census(); }

private:
    // Extra turns for the next picture (error diffusion over the feed-forward need and the bucket's feedback).
    int Plan() {
        const double eps = epsNs_;
        const double need = std::max(0.0, NeedNs());
        double lt = need / eps + (static_cast<double>(bucket_) - 0.5 * c_.budgetNs) / (eps * c_.fbPictures);
        if (lt < 0) lt = 0;
#ifdef SG_DEBUG
        std::fprintf(stderr, "plan need %.2f ms eps %.2f bucket %.1f lt %.2f snap %d\n", need/1e6, eps/1e6, bucket_/1e6, lt, snapM_);
#endif
        int e;
        // A rhythm that is nearly a whole number of extra turns a picture is made EXACTLY that (every picture the same number of turns: an even 20 / 15 / 12 a second), entered after
        // snapPictures plans within snapNear of it and left when the plan is more than snapFar away (the feedback has moved it): hysteresis, not a flutter between k and k + 1.
        if (c_.snap) {
            if (snapM_ >= 0) {
                if (std::fabs(lt - snapM_) > c_.snapFar) { snapM_ = -1; snapRun_ = 0; }
            } else {
                const int m = static_cast<int>(lt + 0.5);
                if (m >= 1 && std::fabs(lt - m) <= c_.snapNear) { if (++snapRun_ >= c_.snapPictures) { snapM_ = m; carry_ = 0; } }
                else snapRun_ = 0;
            }
        }
        if (snapM_ >= 0) e = snapM_;
        else {
            carry_ += lt;
            e = static_cast<int>(carry_);
            carry_ -= e;
        }
        if (e > static_cast<int>(c_.maxExtraEsc)) { e = static_cast<int>(c_.maxExtraEsc); carry_ = 0; }
        if (e > 0 && c_.maxSharePct < 100) { if (credit_ >= 1.0) credit_ -= 1.0; else e = 0; }
        return e;
    }
    int snapM_ = -1;
    unsigned snapRun_ = 0;
    // (pace32) the slip band
    bool slipping_ = false;
    int64_t slipAllowNs_ = 0;
    unsigned slipOver_ = 0;
    double rateSlow_ = 0;   // the natural loss of the last kRateN pictures, per mille of their wall time
    static constexpr unsigned kRateN = 32;
    double rateNat_[kRateN] = {}, rateWall_[kRateN] = {};
    unsigned rateN_ = 0, rateH_ = 0;
    void EndSlip(bool heavy, bool quiet = false) {
        slipping_ = false; slipOver_ = 0;
        // the guard takes over from a clean start: the need it learned while the pacers' catch-up was off (the slip's own rhythm) is not the need of the rhythm it is about to make
        needN_ = 0; needSlow_ = 0; need0_ = need1_ = 0; carry_ = 0; snapM_ = -1; snapRun_ = 0; planned_ = -1; pressSince_ = 0;
        if (quiet) return;
        if (heavy) { ++win_.slipHeavy; ++tot_.slipHeavy; } else { ++win_.slipSpent; ++tot_.slipSpent; }
    }
    // The need of a picture: the mean of the middle three of the last five (one hitch, one odd picture, never moves it; a step in the load is followed in three pictures), smoothed by a
    // slow average (1/12 a picture) that only jumps to it when it is more than 1.5 extra turns away (a step in the load, not the retrace's quantisation noise: a steady need gives a steady plan).
    double TrimNs() const {
        const unsigned n = needN_ < kNeedN ? needN_ : kNeedN;
        if (n == 0) return 0;
        double v[kNeedN];
        for (unsigned i = 0; i < n; ++i) v[i] = needH_[i];
        std::sort(v, v + n);
        if (n < 3) return v[0];   // (too few pictures: the smallest)
        const unsigned lo = n >= 5 ? 1 : 0, hi = n >= 5 ? 4 : n;
        double sum = 0;
        for (unsigned i = lo; i < hi; ++i) sum += v[i];
        return sum / (hi - lo);
    }
    double NeedNs() const { return needSlow_; }
    static constexpr unsigned kNeedN = 5;
    double needH_[kNeedN] = {}, needSlow_ = 0;
    unsigned needN_ = 0;
    void ResetCycle() { rateSlow_ = 0; rateN_ = 0; rateH_ = 0; turnsIn_ = 0; lastK_ = 0; needN_ = 0; needSlow_ = 0; cycleLost_ = 0; cycleWall_ = 0; cycleExtras_ = 0; took_ = 0; planned_ = -1; need0_ = need1_ = 0; pressSince_ = 0; runNow_ = 0; ringN_ = 0; ringH_ = 0; carry_ = 0; snapM_ = -1; snapRun_ = 0; }
    // A picture was drawn: its natural loss (the loss its turns showed plus what its extra turns gave back) less its share of the leak: the need.
    void PictureDone() {
        const double nat = static_cast<double>(cycleLost_);
        const double leak = static_cast<double>(cycleWall_) * c_.refillPermille / 1000.0;
        if (c_.slip) {   // (pace32) the loss rate the band is judged by: the natural loss of the last 32 pictures over their wall time (one picture's rate swings: a pair that runs back to back
                         // for ten pictures and then waits 4 ms for a retrace reads 33, 33, ... 138 per mille). A slip ends when that mean is 15 % over the band (the load is too heavy for it).
            rateNat_[rateH_] = nat; rateWall_[rateH_] = static_cast<double>(cycleWall_);
            rateH_ = (rateH_ + 1) % kRateN;
            if (rateN_ < kRateN) ++rateN_;
            double sn = 0, sw = 0;
            for (unsigned i = 0; i < rateN_; ++i) { sn += rateNat_[i]; sw += rateWall_[i]; }
            rateSlow_ = sw > 0 ? std::max(0.0, 1000.0 * sn / sw) : 0.0;
            if (slipping_ && rateN_ >= 4 && rateSlow_ > c_.slipBandPermille * 1.15) EndSlip(true);
        }
        needH_[needN_++ % kNeedN] = nat - leak;
        need0_ = nat - leak;
        if (needN_ >= 3) {
            const double tr = TrimNs();
            if (needN_ == 3 || std::fabs(tr - needSlow_) > 1.5 * epsNs_) needSlow_ = tr;
            else needSlow_ += (tr - needSlow_) / 12.0;
        }
        need1_ = needSlow_;
        const bool dropped = cycleExtras_ > 0;
        if (dropped) { ++win_.droppedPics; ++tot_.droppedPics; ++runNow_; win_.longestRun = std::max<uint64_t>(win_.longestRun, runNow_); tot_.longestRun = std::max<uint64_t>(tot_.longestRun, runNow_); }
        else runNow_ = 0;
        if (need1_ >= 0.5 * epsNs_) { if (!pressSince_) pressSince_ = nowNs_; }
        else pressSince_ = 0;
        cycleLost_ = 0; cycleWall_ = 0; cycleExtras_ = 0;
    }
    // The game speed over the last counted second (the worst one goes in the census).
    void Ring1s(int64_t dt) {
        ringDt_[ringH_] = dt;
        ringH_ = (ringH_ + 1) % kRingN;
        if (ringN_ < kRingN) ++ringN_;
        int64_t wall = 0;
        unsigned n = 0;
        for (unsigned i = 0; i < ringN_; ++i) {
            wall += ringDt_[(ringH_ + kRingN - 1 - i) % kRingN];
            ++n;
            if (wall >= 1000000000ll) break;
        }
        if (wall < 1000000000ll) return;
        const double sp = 100.0 * static_cast<double>(n) * kPeriodNs / static_cast<double>(wall);
        if (sp < win_.worst1s) win_.worst1s = sp;
        if (sp < tot_.worst1s) tot_.worst1s = sp;
    }
    static constexpr unsigned kRingN = 256;
    int64_t ringDt_[kRingN] = {};
    unsigned ringN_ = 0, ringH_ = 0;
    int64_t nowNs_ = 0, cycleLost_ = 0, cycleWall_ = 0, pressSince_ = 0;
    double epsNs_ = 4.7e6, need0_ = 0, need1_ = 0, carry_ = 0;
    int planned_ = -1;
    unsigned took_ = 0, cycleExtras_ = 0, runNow_ = 0, turnsIn_ = 0, lastK_ = 0;
    bool active_ = false, prevExtra_ = false;
    Config c_;
    int64_t capNs_ = 120000000;
    int64_t last_ = 0, bucket_ = 0, graceUntil_ = 0;
    bool haveLast_ = false, prevCounting_ = false, nowCounting_ = false, extraNow_ = false;
    unsigned undrawn_ = 0;
    double credit_ = 1.0;
    Census win_, tot_;
};

// The one the game's turn loop uses (port/perf.cpp feeds it every turn; the fixed 30 controller's glue in vgx_galaxy.cpp reads it).
inline SpeedGuard& GlobalSpeedGuard() {
    static SpeedGuard g;
    return g;
}

// GXV_SPEED_GUARD_MS (10..200, default 30), _MAXSHARE (1..100, default 100), _EXTRA (1..2, default 1), _GRACE_MS (0..30000, default 3000), _LEAK (percent of the wall time the
// bucket forgives, 1..20, default 2: the guard then aims at 98 % game speed; 5 aims at 95 %, fewer three-turn pictures): the values from the environment text
// (null / empty = default); out of range is held to the nearest end.
inline SpeedGuard::Config MakeSpeedGuardConfigPm(bool on, long ms, long maxSharePct, long extra, long graceMs, long leakPermille) {
    SpeedGuard::Config c;
    c.on = on;
    c.budgetNs = static_cast<int64_t>(ms < 10 ? 10 : ms > 200 ? 200 : ms) * 1000000;
    c.maxSharePct = static_cast<unsigned>(maxSharePct < 1 ? 1 : maxSharePct > 100 ? 100 : maxSharePct);
    c.maxExtra = static_cast<unsigned>(extra < 1 ? 1 : extra > 2 ? 2 : extra);
    c.graceNs = static_cast<int64_t>(graceMs < 0 ? 0 : graceMs > 30000 ? 30000 : graceMs) * 1000000;
    c.refillPermille = static_cast<unsigned>(leakPermille < 1 ? 1 : leakPermille > 200 ? 200 : leakPermille);
    return c;
}
// (speed30's form: the leak in whole percent)
inline SpeedGuard::Config MakeSpeedGuardConfig(bool on, long ms, long maxSharePct, long extra, long graceMs, long leakPct = 2) {
    return MakeSpeedGuardConfigPm(on, ms, maxSharePct, extra, graceMs, (leakPct < 1 ? 1 : leakPct > 20 ? 20 : leakPct) * 10);
}

// The census line (every 5 s, and at a stage's end with the stage's total): "speed: game ran at NN.N % of real time over 5.0 s (lost M ms; K extra undrawn turns by the guard;
// budget B ms)". Seconds are the counted wall time; what was left out is named after it. `what` is "" or a prefix that ends in a space ("stage end 1a2b3c4d: ").
inline int FormatSpeedLine(char* out, unsigned cap, const char* what, const SpeedGuard::Census& c, bool guardOn, int64_t budgetNs) {
    if (c.wallNs < 500000000)
        return snprintf(out, cap, "speed: %sno counted game time (%.1f s left out: loading, covered screen, the first 3 s after those, gaps over 80 ms); %u pictures; guard %s",
                        what, c.exclNs / 1e9, static_cast<unsigned>(c.pictures), guardOn ? "on" : "off");
    return snprintf(out, cap, "speed: %sgame ran at %.1f %% of real time over %.1f s (lost %.0f ms; %u extra undrawn turns by the guard; budget %.0f ms); %u pictures, %.1f a second; left out %.1f s "
                              "(loading, covered screen, the first 3 s after those, gaps over 80 ms); guard %s",
                    what, c.speedPct(), c.wallNs / 1e9, c.lostNs() / 1e6, static_cast<unsigned>(c.extras), budgetNs / 1e6, static_cast<unsigned>(c.pictures), c.pictures * 1e9 / c.wallNs,
                    c.exclNs / 1e9, guardOn ? "on" : "off");
}

// speed31: the acceptance line. One grep ("game speed") judges a console run: per 5 s window and at each stage's end,
//   speed: <stage>: game speed NN.N % (worst 1 s NN.N %), lost M ms, pictures dropped by the guard K (longest run R), cuts taken because of drops C; ...
// PASS for a run = every in-play window at least 98 %, worst 1 s at least 97 % (tools in REPORT.txt: one awk line). `what` is the stage's name with " (stage end)" for a total.
inline int FormatSpeedLine2(char* out, unsigned cap, const char* stage, const char* what, const SpeedGuard::Census& c, bool guardOn, int64_t budgetNs) {
    if (c.wallNs < 500000000)
        return snprintf(out, cap, "speed: %s%s: no counted game time (%.1f s left out: loading, covered screen, the first 3 s after those, gaps over 80 ms); %u pictures; guard %s",
                        stage, what, c.exclNs / 1e9, static_cast<unsigned>(c.pictures), guardOn ? "on" : "off");
    return snprintf(out, cap, "speed: %s%s: game speed %.1f %% (worst 1 s %.1f %%), lost %.0f ms, pictures dropped by the guard %u (longest run %u), cuts taken because of drops %u; "
                              "%u pictures, %.1f a second, %u extra turns, %.1f s counted, budget %.0f ms, left out %.1f s (loading, covered screen, the first 3 s after those, gaps over 80 ms); guard %s",
                    stage, what, c.speedPct(), c.worst1s, c.lostNs() / 1e6, static_cast<unsigned>(c.droppedPics), static_cast<unsigned>(c.longestRun), static_cast<unsigned>(c.cuts),
                    static_cast<unsigned>(c.pictures), c.pictures * 1e9 / c.wallNs, static_cast<unsigned>(c.extras), c.wallNs / 1e9, budgetNs / 1e6, c.exclNs / 1e9, guardOn ? "on" : "off");
}

// pace32: "speed: slip band: ..." (one line per 5 s window in which the band acted, and with the stage's total). Empty (returns 0) when it did not act.
inline int FormatSlipLine(char* out, unsigned cap, const char* stage, const char* what, const SpeedGuard::Census& c, const SpeedGuard& g) {
    if (!c.slipStarts && c.slipNs <= 0) { if (cap) out[0] = 0; return 0; }
    return snprintf(out, cap, "speed: slip band: %s%s: slipped %.1f s of %.1f s counted (%.1f ms of game time forgiven = %.2f %% over that time; no picture dropped for it), begun %u, "
                              "ended with the allowance spent %u, ended because the loss left the band %u; now %s, allowance left %.1f s, measured loss %.1f per mille (band %u)",
                    stage, what, c.slipNs / 1e9, c.wallNs / 1e9, c.slipLostNs / 1e6, c.slipNs > 0 ? 100.0 * c.slipLostNs / static_cast<double>(c.slipNs) : 0.0, static_cast<unsigned>(c.slipStarts),
                    static_cast<unsigned>(c.slipSpent), static_cast<unsigned>(c.slipHeavy), g.Slipping() ? "slipping" : "not slipping", g.SlipAllowSec(), g.LossPermille(), g.Cfg().slipBandPermille);
}

}  // namespace port::pace
