#pragma once
// GXV_RATE_AUTO (rate30, 13 Nov 2026): the two-way, fast, flap-free rate controller of Performance mode. Two states only: 60 (a picture every turn) and an even 30 (a picture
// every second turn, each held exactly two refreshes; the logic still runs 60 turns a second, game speed 100 %). It never picks a rhythm in between: the uneven 31-59 band is what it
// exists to avoid. Pure header (no clock, no globals, no platform includes): tests/rate_auto_test.cpp runs this code against a model of the pipeline.
//
// ORDER OF DEFENCE (the owner, 13 Nov: "big sacrifices, least noticeable, to get us to 60"; "recover quality as fast as possible"; "invisible transitions"):
//  1. the governors' cuts (cpulod table, far-field levels) go to their maximum FIRST (one request, the governors' own walk fades them in over about 3 s);
//  2. only when 60 still cannot be held does the rate drop to an even 30;
//  3. at 30 the stage has twice the budget: the governors give quality back by their own (recover30) rules (the glue gives them the 30 budgets while Giveback30());
//  4. back to 60 as soon as the PREDICTED 60 fits: each lane's p95 over the last second or two (game thread drawn turn, front end, render thread, GPU busy, per picture), less the
//     governors' learned gains of the cuts that will be in force; the cuts are put back first (fade), the lanes are measured again with them, then the rate goes up.
//
// DOWN (60 -> 30): turn-level evidence, not a one-second sample. A turn "missed" when the pacer left out a picture it would have drawn (the back end was behind, or the game thread was
// behind the display's clock). Fast rule: at least downM of the last downN turns (8 of 30, half a second). Slow rule (a stage that sits at 50-55: one in every eight turns): at least
// slowMissPct of the last slowTurns (180, three seconds). A hitch, a load or a stall does not count: turns that follow a stall (the debt the skip rule pays back) are held off, and
// turns that are not play are not in the ring. The decision needs the governors to be at their maximum or unable (learned gains) to close the gap, and the even 30 to be predicted to fit.
// UP (30 -> 60): fit with margin on every lane (laneLimitMs 14.5 = 87 % of a refresh), held for fitHoldS, cuts re-applied first when they have been given back, proof measured with them.
//  A failed up (back to 30 within failS) doubles the wait before the next try (retryS .. retryMaxS) and halves back after halveS at 60, recover30's pattern.
// A switch is made at a natural break (a camera cut, a closed screen, a pause, a load) when one comes within breakWait; never later than that. The switch costs nothing in itself: it is only
// which turns are offered a picture (SteadyCadence::Force), no resource is made or freed; the glue says how the seam is kept free of 3-refresh holds.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdio.h>  // (unqualified snprintf: game units map it to port_snprintf, so std::snprintf does not exist there)

namespace port {
namespace rate {

struct Params {
    // down
    unsigned downN = 30, downM = 8;        // the fast rule: downM missed turns of the last downN
    unsigned slowTurns = 180;              // the slow rule's window (3 s); at most 180
    double slowMissPct = 13.0;             // ... missed turns in it, in percent (13 % = about 52 pictures a second)
    double settleSec = 3.0;                // no decision this long after a stage's begin (the opening camera, shader builds)
    unsigned holdTurns = 10;               // turns after a stall (a gap of stallMs or more) that are not judged: the skip rule's debt payback is not overload
    double stallMs = 50.0;
    double breakWaitDownMs = 500.0;        // the longest a decided drop waits for a natural break
    // lanes
    double laneLimitMs = 14.5;             // a lane at 60 must fit this (87 % of a refresh: the owner's "responsive" target is under 90 %)
    double lane30LimitMs = 30.0;           // the even 30 (a picture per lane per two refreshes, 90 % of 33.3)
    double laneSpanS = 1.0;                // the window of the p95
    unsigned laneMinN = 12;                // fewer samples than this: the lane is "not known"
    double gpuCredit = 0.4;                // the share of the front end's cut gain credited to the render thread and the GPU (not measured by the governors)
    double predMarginMs = 0.3;             // the predicted lane must fit this much under the limit
    // up
    double fitHoldS = 0.5;                 // the prediction must hold this long (continuously) before the cuts are put back
    double fadeS = 3.0;                    // the governors' walk of a level change (GXV_GOV_FADE_MS)
    double proofS = 1.5;                   // the lanes measured with the cuts in force, this long, before the rate goes up
    double breakWaitUpMs = 1000.0;
    bool sticky = false;                   // (pace32, GXV_RATE_STICKY) the way up needs stickyProofS of fit and a natural break, however long that takes
    double stickyProofS = 20.0;
    double retryS = 4.0, retryMaxS = 80.0; // the wait after a down (and after a failed up, doubling)
    double failS = 10.0;                   // a drop within this of an up is a failed up
    double halveS = 60.0;                  // a minute at 60 halves a doubled wait
    bool giveback30 = true;                // the governors get the 30 budgets while the even 30 stands (they give quality back)
};

struct Obs {              // the turn that just ended, seen at the start of the next one
    bool play = true;     // a turn of play (not a load, a fade, a pause, a movie)
    bool drawn = false;   // it made a picture
    bool missed = false;  // (60 state) it was left out by the pacer for being late or the back end being busy
    bool atBreak = false; // a natural break is at hand now: a camera cut, a closed screen, a pause menu
    float gameMs = -1.f;  // its work in ms (the game thread's CPU time); -1 unknown
    float gapMs = 0.f;    // the gap between its start and the one before
};
struct Gov {              // the governors, as the cuts stand
    bool atMax = true;    // every governor is at its maximum level (or has none)
    float gainLeftGameMs = 0.f, gainLeftFeMs = 0.f;  // the learned (else assumed) gain of the levels still to take, game thread's drawn turn and the front end's picture
};
enum Action : int { kNone = 0, kTo30 = 1, kTo60 = 2 };
enum Phase : int { kRun60 = 0, kPush60 = 1, kWait30 = 2, kRun30 = 3, kPrep30 = 4, kWait60 = 5 };

// A window of timed samples; p95 over the last spanUs.
class Lane {
public:
    static constexpr int kN = 640;   // (20 s of pictures at 30 a second; the peak window of a doubled wait)
    void Add(uint64_t now, float v) { t_[h_] = now; v_[h_] = v; h_ = (h_ + 1) % kN; if (n_ < kN) ++n_; }
    void Clear() { n_ = 0; h_ = 0; }
    // p95 and the count of the samples of the last spanUs (n); 0 when none
    float P95(uint64_t now, uint64_t spanUs, int* n) const {
        float tmp[kN];
        int c = 0;
        for (int i = 0; i < n_; ++i) {
            const int k = (h_ - 1 - i + 2 * kN) % kN;
            if (now >= t_[k] && now - t_[k] > spanUs) break;
            tmp[c++] = v_[k];
        }
        if (n) *n = c;
        if (!c) return 0.f;
        std::sort(tmp, tmp + c);
        int idx = static_cast<int>(std::ceil(0.95 * c)) - 1;
        if (idx < 0) idx = 0;
        return tmp[idx];
    }
private:
    uint64_t t_[kN] = {};
    float v_[kN] = {};
    int h_ = 0, n_ = 0;
};

class Controller {
public:
    void Configure(const Params& p) { p_ = p; if (p_.slowTurns > kRing) p_.slowTurns = kRing; if (p_.downN > 60) p_.downN = 60; }
    const Params& Cfg() const { return p_; }
    // A stage begins (the screen is closed). pin: 0 automatic, 1 the stage stays at 60, 2 the stage stays at an even 30 (a stage row's wish).
    void StageBegin(uint64_t now, int pin) {
        pin_ = pin;
        phase_ = kRun60;
        even30_ = false;
        antUntil_ = 0;
        ClearEvidence();
        ClearLanes();
        begunAt_ = lastNow_ = now;
        wait_ = p_.retryS;
        sinceUp_ = 0;
        upAt_ = 0;
        t60_ = t30_ = 0;
        downs_ = ups_ = failedUps_ = tries_ = cutsHeld_ = cannot30_ = 0;
        sumDownMs_ = sumUpS_ = 0.0;
        raise_ = false;
        active_ = true;
        pending30_ = pendingUp_ = false;
        lastLog_ = now;
        stageLine_ = false;
    }
    void StageEnd() { active_ = false; }
    // anticip32 (GXV_ANTICIPATE): a heavy moment is announced until `until`. No up-switch is prepared or made before it is over (never back to 60 in the middle of the action); with
    // `drop` (the spike map: this event ended in dropped pictures or an even 30 on an earlier visit) the rate goes to the even 30 NOW, at the event's own wind-up / camera cut, instead of
    // after 8 missed turns in the middle of it. True when the switch was made (the glue's cadence follows Even30() at the next turn start). A stage pinned to 60 is left alone.
    bool Anticipate(uint64_t now, uint64_t until, bool drop) {
        if (!active_) return false;
        if (until > antUntil_) antUntil_ = until;
        if (!drop || even30_ || pin_ == 1 || (phase_ != kRun60 && phase_ != kPush60)) return false;
        upAt_ = 0;   // (not a failed up: the wait does not double)
        Enter30(now, now);
        return true;
    }
    bool AnticipateHolds(uint64_t now) const { return now < antUntil_; }
    bool Active() const { return active_; }

    // One picture's lanes in ms (-1: not measured). play false: not a picture of play.
    void Picture(uint64_t now, float feMs, float renderMs, float gpuMs, bool play) {
        if (!active_ || !play) return;
        if (feMs >= 0.f) fe_.Add(now, feMs);
        if (renderMs >= 0.f) rd_.Add(now, renderMs);
        if (gpuMs >= 0.f) gpu_.Add(now, gpuMs);
        if (feMs >= 0.f) lastFe_ = feMs;
        if (renderMs >= 0.f) lastRd_ = renderMs;
        if (gpuMs >= 0.f) lastGpu_ = gpuMs;
    }

    // The start of a turn. o describes the turn that ended, g the governors. Returns the switch to make for THIS turn (kNone / kTo30 / kTo60).
    Action Step(uint64_t now, const Obs& o, const Gov& g) {
        if (!active_) return kNone;
        const uint64_t dt = now > lastNow_ ? now - lastNow_ : 0;
        lastNow_ = now;
        (even30_ ? t30_ : t60_) += dt;
        if (pin_ == 2 && !even30_) { Enter30(now, now); return kTo30; }
        if (pin_ == 1 || pin_ == 2) return kNone;   // (anticip32: a stage pinned to the even 30 stays there. It used to run the up rules: lanes that fit gave ONE turn at 60 and a "failed up", again and again: tests/anticipate_test.cpp)
        const uint64_t span = static_cast<uint64_t>(p_.laneSpanS * 1e6);
        const bool stalled = o.gapMs >= p_.stallMs;
        if (stalled) hold_ = p_.holdTurns;
        // the evidence of the turn
        if (o.play && !even30_) {
            if (hold_ > 0 && !stalled) { --hold_; AddTurn(now, false, false); }  // (held off: counted as a clean turn, no miss)
            else if (!stalled) AddTurn(now, o.missed, true);
        } else if (hold_ > 0 && !stalled) --hold_;
        if (o.play && o.gameMs >= 0.f) {
            if (o.drawn) game_.Add(now, o.gameMs);
            else undrawn_ = undrawn_ < 0.f ? o.gameMs : 0.9f * undrawn_ + 0.1f * o.gameMs;
        }
        const bool brk = o.atBreak || !o.play;
        switch (phase_) {
            case kRun60: {
                // a minute at 60 forgives a doubled wait
                if (upAt_ && now - upAt_ >= static_cast<uint64_t>(p_.halveS * 1e6)) { wait_ = std::max(p_.retryS, wait_ * 0.5); upAt_ = now; }
                if (now < begunAt_ + static_cast<uint64_t>(p_.settleSec * 1e6) || !o.play) return kNone;
                if (!Triggered()) return kNone;
                return OnTrigger(now, g, span);
            }
            case kPush60: {
                if (now < fadeEnd_) return kNone;
                if (!proofBegun_) { proofBegun_ = true; ClearEvidence(); game_.Clear(); fe_.Clear(); rd_.Clear(); gpu_.Clear(); proofAt_ = now; return kNone; }
                if (now < proofAt_ + static_cast<uint64_t>(p_.proofS * 1e6)) {
                    // still missing at the fast rule: it did not close, no need to wait for the end of the proof
                    if (o.play && FastTriggered()) return DropOrStay(now, g, span, "the cuts at their maximum did not close it");
                    return kNone;
                }
                NotePost(now, span);
                if (Over60(now, span, nullptr) || Misses(60) >= 4) return DropOrStay(now, g, span, "the cuts at their maximum did not close it");
                phase_ = kRun60;
                ++cutsHeld_;
                ClearEvidence();
                overSince_ = 0;
                return kNone;
            }
            case kWait30: {
                if (brk || now >= breakUntil_) { Enter30(now, overStart_); return kTo30; }
                return kNone;
            }
            case kRun30: {
                if (!o.play) return kNone;
                if (now < antUntil_) { fitSince_ = 0; return kNone; }   // (anticip32: the fit is proved after the announced moment, not inside it)
                if (now < since30_ + static_cast<uint64_t>(wait_ * 1e6)) { fitSince_ = 0; return kNone; }
                if (now < nextEval_) return kNone;
                nextEval_ = now + 250000u;
                // the window of the p95 grows with the wait (a failed up doubles the wait AND the margin: the prediction looks back further, recover30's peak test)
                const double peakS = std::min(20.0, std::max(p_.laneSpanS, p_.laneSpanS + (wait_ - p_.retryS) * 0.5));
                const uint64_t pspan = static_cast<uint64_t>(peakS * 1e6);
                if (!Fits60(now, pspan, g)) { fitSince_ = 0; return kNone; }
                if (!fitSince_) fitSince_ = now;
                if (now - fitSince_ < static_cast<uint64_t>((p_.sticky ? std::max(p_.fitHoldS, p_.stickyProofS) : p_.fitHoldS) * 1e6)) return kNone;
                tries_++;
                wantSince_ = fitSince_;
                {   // the lanes fit as they are (no cut counted): the window of the last second is the proof, the rate goes up at the next break
                    Gov none;
                    none.atMax = true;
                    if (Fits60(now, pspan, none)) { SnapshotLanes(now, span); phase_ = kWait60; breakUntil_ = now + static_cast<uint64_t>(p_.breakWaitUpMs * 1e3); return kNone; }
                }
                // predicted to fit only with the cuts: they go back first (they have been given back), then the proof with them
                if (!g.atMax) { NotePre(now, pspan); raise_ = true; phase_ = kPrep30; fadeEnd_ = now + static_cast<uint64_t>(p_.fadeS * 1e6); proofBegun_ = false; return kNone; }
                phase_ = kPrep30;  // (already at the maximum: the lanes in the window are the proof, measured with the cuts in force)
                fadeEnd_ = now;
                proofBegun_ = true;
                proofAt_ = now - static_cast<uint64_t>(p_.proofS * 0.5e6);
                return kNone;
            }
            case kPrep30: {
                if (!o.play) return kNone;
                if (now < antUntil_) { phase_ = kRun30; since30_ = now; fitSince_ = 0; return kNone; }   // (anticip32)
                if (now < fadeEnd_) return kNone;
                if (!proofBegun_) { proofBegun_ = true; game_.Clear(); fe_.Clear(); rd_.Clear(); gpu_.Clear(); proofAt_ = now; return kNone; }
                if (now < proofAt_ + static_cast<uint64_t>(p_.proofS * 1e6)) return kNone;
                NotePost(now, span);
                if (Over60(now, span, nullptr, 20)) {  // not enough room with the cuts in: a failed try (no switch was made), the wait doubles
                    ++failedTries_;
                    wait_ = std::min(p_.retryMaxS, wait_ * 2.0);
                    phase_ = kRun30;
                    since30_ = now;
                    fitSince_ = 0;
                    return kNone;
                }
                phase_ = kWait60;
                breakUntil_ = now + static_cast<uint64_t>(p_.breakWaitUpMs * 1e3);
                return kNone;
            }
            case kWait60: {
                if (now < antUntil_) { phase_ = kRun30; since30_ = now; fitSince_ = 0; return kNone; }   // (anticip32: no up-switch inside an announced heavy moment)
                if (!brk && (p_.sticky || now < breakUntil_)) {   // (pace32: sticky = only at a break, and only while the lanes still fit: the proof is not allowed to go stale while it waits)
                    if (p_.sticky && o.play && now >= nextEval_) {
                        nextEval_ = now + 250000u;
                        Gov none;
                        none.atMax = true;
                        if (!Fits60(now, span, none)) { phase_ = kRun30; fitSince_ = 0; }
                    }
                    return kNone;
                }
                even30_ = false;
                phase_ = kRun60;
                ++ups_;
                sumUpS_ += static_cast<double>(now - wantSince_) / 1e6;
                upAt_ = now;
                ClearEvidence();
                overSince_ = 0;
                hold_ = 0;
                // lanes of the 30 state stay (they are what the 60 state is judged against), the game lane is per drawn turn at both rhythms
                SnapshotLanes(now, span);
                return kTo60;
            }
        }
        return kNone;
    }

    // ---- state
    bool Even30() const { return even30_; }
    int PhaseNow() const { return static_cast<int>(phase_); }
    // the governors get the budgets of the even 30 (they may give quality back)
    bool Giveback30() const { return active_ && even30_ && phase_ == kRun30 && p_.giveback30; }
    // true once: the governors are asked to go to their maximum levels now (their own walk fades the cuts in)
    bool TakeRaise() { const bool r = raise_; raise_ = false; return r; }
    // After a push (cuts taken to their maximum, then measured): what they gained, front end's p95 and the game thread's, from the lanes before and after. True once.
    bool TakeLearned(float* feMs, float* gameMs) {
        if (!learnReady_) return false;
        learnReady_ = false;
        if (feMs) *feMs = learnFe_;
        if (gameMs) *gameMs = learnGame_;
        return true;
    }
    uint32_t Downs() const { return downs_; }
    uint32_t Ups() const { return ups_; }
    uint32_t FailedUps() const { return failedUps_ + failedTries_; }
    uint32_t FailedUpsReal() const { return failedUps_; }
    uint32_t CutsHeld() const { return cutsHeld_; }
    double Wait() const { return wait_; }
    double Sec60() const { return static_cast<double>(t60_) / 1e6; }
    double Sec30() const { return static_cast<double>(t30_) / 1e6; }
    double LastDecisionMsDown() const { return lastDownMs_; }
    void Lanes(float* game, float* fe, float* render, float* gpu) const { *game = decG_; *fe = decFe_; *render = decRd_; *gpu = decGpu_; }

    // "rate: <stage>: 60 for X s, even 30 for Y s, switches down K (mean decision time T1 ms), up K (mean T2 s), failed ups K, lanes at the last decision: game G ms, front end F, render R, GPU B; turn to picture now L refreshes"
    int Format(char* out, size_t n, const char* stage, const char* tag, double turnToPicture) const {
        return snprintf(out, n, "rate: %s%s: 60 for %.1f s, even 30 for %.1f s, switches down %u (mean decision time %.0f ms), up %u (mean %.1f s), failed ups %u (+%u tries stopped before the switch), "
                                "cuts at the maximum closed it %u, 30 not predicted to hold (dropped all the same) %u, next try after %.0f s, lanes at the last decision: game %.1f ms, front end %.1f, render %.1f, GPU %.1f; turn to picture now %.1f refreshes",
                        stage ? stage : "?", tag ? tag : "", Sec60(), Sec30(), downs_, downs_ ? sumDownMs_ / downs_ : 0.0, ups_, ups_ ? sumUpS_ / ups_ : 0.0, failedUps_, failedTries_, cutsHeld_, cannot30_, wait_,
                        static_cast<double>(decG_), static_cast<double>(decFe_), static_cast<double>(decRd_), static_cast<double>(decGpu_), turnToPicture);
    }

private:
    static constexpr unsigned kRing = 180;
    void ClearEvidence() { for (unsigned i = 0; i < kRing; ++i) miss_[i] = 0; pos_ = 0; filled_ = 0; overSince_ = 0; }
    void ClearLanes() { game_.Clear(); fe_.Clear(); rd_.Clear(); gpu_.Clear(); undrawn_ = -1.f; hold_ = 0; }
    void AddTurn(uint64_t now, bool missed, bool counts) {
        (void)counts;
        miss_[pos_] = missed ? 1 : 0;
        pos_ = (pos_ + 1) % kRing;
        if (filled_ < kRing) ++filled_;
        if (missed && overSince_ == 0) overSince_ = now;
        if (!missed && Misses(30) == 0 && overSince_ != 0 && filled_ >= 30) overSince_ = 0;
    }
    unsigned Misses(unsigned last) const {
        unsigned c = 0;
        const unsigned n = last < filled_ ? last : filled_;
        for (unsigned i = 0; i < n; ++i) c += miss_[(pos_ + kRing - 1 - i) % kRing];
        return c;
    }
    bool FastTriggered() const { return filled_ >= p_.downN && Misses(p_.downN) >= p_.downM; }
    bool Triggered() const {
        if (FastTriggered()) return true;
        if (filled_ >= p_.slowTurns && p_.slowTurns > 0 && static_cast<double>(Misses(p_.slowTurns)) * 100.0 >= p_.slowMissPct * p_.slowTurns) return true;
        return false;
    }
    // any lane over the limit at 60 (known lanes only); worst: filled with the first lane found over
    bool Over60(uint64_t now, uint64_t span, float* worst, unsigned minN = 0) const {
        int n;
        bool over = false;
        const unsigned mn = minN ? minN : p_.laneMinN;
        float v = game_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= mn && v > p_.laneLimitMs) over = true;
        float w = v;
        v = fe_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= mn && v > p_.laneLimitMs) over = true;
        w = std::max(w, v);
        v = rd_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= mn && v > p_.laneLimitMs) over = true;
        w = std::max(w, v);
        v = gpu_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= mn && v > p_.laneLimitMs) over = true;
        w = std::max(w, v);
        if (worst) *worst = w;
        return over;
    }
    void NotePre(uint64_t now, uint64_t span) {
        int n1, n2;
        preFe_ = fe_.P95(now, span, &n1);
        preGame_ = game_.P95(now, span, &n2);
        preValid_ = static_cast<unsigned>(n1) >= p_.laneMinN / 2 && static_cast<unsigned>(n2) >= p_.laneMinN / 2;
    }
    void NotePost(uint64_t now, uint64_t span) {
        if (!preValid_) return;
        int n1, n2;
        const float fe = fe_.P95(now, span, &n1), gm = game_.P95(now, span, &n2);
        if (static_cast<unsigned>(n1) < p_.laneMinN / 2 || static_cast<unsigned>(n2) < p_.laneMinN / 2) return;
        learnFe_ = std::max(0.f, preFe_ - fe);
        learnGame_ = std::max(0.f, preGame_ - gm);
        learnReady_ = true;
        preValid_ = false;
    }
    void SnapshotLanes(uint64_t now, uint64_t span) {
        int n;
        decG_ = game_.P95(now, span, &n);
        decFe_ = fe_.P95(now, span, &n);
        decRd_ = rd_.P95(now, span, &n);
        decGpu_ = gpu_.P95(now, span, &n);
    }
    // the predicted 60 state from the 30 state's lanes, less what the cuts still to take will give
    bool Fits60(uint64_t now, uint64_t span, const Gov& g) const {
        int n;
        const float gl = g.atMax ? 0.f : g.gainLeftGameMs, fl = g.atMax ? 0.f : g.gainLeftFeMs;
        float v = game_.P95(now, span, &n);
        if (static_cast<unsigned>(n) < p_.laneMinN / 2) return false;   // (the game lane is per drawn turn: at 30 half as many)
        if (v - gl > p_.laneLimitMs - p_.predMarginMs) return false;
        v = fe_.P95(now, span, &n);
        if (static_cast<unsigned>(n) < p_.laneMinN / 2) return false;
        if (v - fl > p_.laneLimitMs - p_.predMarginMs) return false;
        v = rd_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN / 2 && v - static_cast<float>(p_.gpuCredit) * fl > p_.laneLimitMs - p_.predMarginMs) return false;
        v = gpu_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN / 2 && v - static_cast<float>(p_.gpuCredit) * fl > p_.laneLimitMs - p_.predMarginMs) return false;
        return true;
    }
    // the even 30 is predicted to hold: a picture's lanes fit two refreshes, the drawn turn and the undrawn turn together fit them too
    bool Fits30(uint64_t now, uint64_t span) const {
        int n;
        const float lim = static_cast<float>(p_.lane30LimitMs);
        const float g95 = game_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN) {
            const float und = undrawn_ >= 0.f ? undrawn_ : 0.4f * g95;
            if (g95 + und > lim) return false;
        }
        float v = fe_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN && v > lim) return false;
        v = rd_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN && v > lim) return false;
        v = gpu_.P95(now, span, &n);
        if (static_cast<unsigned>(n) >= p_.laneMinN && v > lim) return false;
        return true;
    }
    // the cuts could close the gap: every lane over the limit is within the (learned) gain of the levels still to take
    bool Closable(uint64_t now, uint64_t span, const Gov& g) const {
        if (g.atMax) return false;
        int n;
        const float lim = static_cast<float>(p_.laneLimitMs);
        const float cr = static_cast<float>(p_.gpuCredit);
        struct L { float v; float help; } l[4];
        l[0] = {game_.P95(now, span, &n), g.gainLeftGameMs};
        if (static_cast<unsigned>(n) < p_.laneMinN) l[0].v = 0.f;
        l[1] = {fe_.P95(now, span, &n), g.gainLeftFeMs};
        if (static_cast<unsigned>(n) < p_.laneMinN) l[1].v = 0.f;
        l[2] = {rd_.P95(now, span, &n), cr * g.gainLeftFeMs};
        if (static_cast<unsigned>(n) < p_.laneMinN) l[2].v = 0.f;
        l[3] = {gpu_.P95(now, span, &n), cr * g.gainLeftFeMs};
        if (static_cast<unsigned>(n) < p_.laneMinN) l[3].v = 0.f;
        for (const L& x : l) {
            if (x.v > lim && x.v - lim > 0.9f * x.help) return false;
        }
        return true;   // (no lane known to be over: the cuts are tried first all the same, they cost no decision)
    }
    Action OnTrigger(uint64_t now, const Gov& g, uint64_t span) {
        overStart_ = overSince_ ? overSince_ : now;
        SnapshotLanes(now, span);
        if (Closable(now, span, g)) {  // the cheapest sacrifice first
            NotePre(now, span);
            raise_ = true;
            phase_ = kPush60;
            fadeEnd_ = now + static_cast<uint64_t>(p_.fadeS * 1e6);
            proofBegun_ = false;
            return kNone;
        }
        return DropOrStay(now, g, span, "the cuts cannot close it");
    }
    // The drop. When the even 30 is not predicted to hold either (a lane over lane30LimitMs: the console of the emulator at a third of its speed, a GPU of 32 ms) it is made all the same: the pictures
    // are no fewer than the 60 state's uneven 20-30, and an even rhythm is still the better of the two; it is only counted (weak30_), and no way back is tried until the 60 state is predicted to fit.
    Action DropOrStay(uint64_t now, const Gov& g, uint64_t span, const char*) {
        (void)g;
        if (!Fits30(now, span)) ++cannot30_;
        phase_ = kWait30;
        breakUntil_ = now + static_cast<uint64_t>(p_.breakWaitDownMs * 1e3);
        pending30_ = true;
        return kNone;
    }
    void Enter30(uint64_t now, uint64_t overStart) {
        const bool failed = upAt_ != 0 && now - upAt_ < static_cast<uint64_t>(p_.failS * 1e6);
        if (failed) { ++failedUps_; wait_ = std::min(p_.retryMaxS, wait_ * 2.0); }
        ++downs_;
        sumDownMs_ += static_cast<double>(now > overStart ? now - overStart : 0) / 1e3;
        lastDownMs_ = static_cast<double>(now > overStart ? now - overStart : 0) / 1e3;
        even30_ = true;
        phase_ = kRun30;
        since30_ = now;
        nextEval_ = now;
        fitSince_ = 0;
        upAt_ = 0;
        proofBegun_ = false;
        game_.Clear(); fe_.Clear(); rd_.Clear(); gpu_.Clear();
        pending30_ = false;
    }

    Params p_;
    bool learnReady_ = false;
    float learnFe_ = 0, learnGame_ = 0, preFe_ = 0, preGame_ = 0;
    bool preValid_ = false;
    bool active_ = false, even30_ = false, raise_ = false, proofBegun_ = false, pending30_ = false, pendingUp_ = false, stageLine_ = false;
    int pin_ = 0;
    Phase phase_ = kRun60;
    uint8_t miss_[kRing] = {};
    unsigned pos_ = 0, filled_ = 0, hold_ = 0;
    uint64_t lastNow_ = 0, begunAt_ = 0, overSince_ = 0, overStart_ = 0, fadeEnd_ = 0, proofAt_ = 0, breakUntil_ = 0, since30_ = 0, nextEval_ = 0, fitSince_ = 0, wantSince_ = 0, upAt_ = 0, blockUntil_ = 0, lastLog_ = 0;
    uint64_t t60_ = 0, t30_ = 0, sinceUp_ = 0, antUntil_ = 0;
    double wait_ = 4.0, sumDownMs_ = 0, sumUpS_ = 0, lastDownMs_ = 0;
    uint32_t downs_ = 0, ups_ = 0, failedUps_ = 0, failedTries_ = 0, tries_ = 0, cutsHeld_ = 0, cannot30_ = 0;
    float undrawn_ = -1.f, lastFe_ = 0, lastRd_ = 0, lastGpu_ = 0, decG_ = 0, decFe_ = 0, decRd_ = 0, decGpu_ = 0;
    Lane game_, fe_, rd_, gpu_;
};

}  // namespace rate
}  // namespace port
