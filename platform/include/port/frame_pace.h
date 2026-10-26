#pragma once
#include <cstdint>

namespace port::pace {
enum class Policy { Adaptive, Fixed30 };
// Mode 3 is the fixed presentation policy, never the adaptive 20-picture fallback.
inline int ResolveMode(int fpsLimit, int adaptive, bool fixedSupported) {
    if (fpsLimit == 30 && fixedSupported) return 3;
    return adaptive == 1 || adaptive == 2 ? adaptive : 0;
}
class Controller {
public:
    explicit Controller(bool verify = false, Policy policy = Policy::Adaptive) : verify_(verify), policy_(policy) {}
    void Reset() { *this = Controller(verify_, policy_); }
    bool PresentAllowed() const { return presentAllowed_; }

    // One call per simulation turn; 1 keeps its retrace, 2 catches up without one.
    int Turn(uint64_t now, bool allowed, bool quiet, unsigned pending, uint64_t backendUs, uint64_t wholeUs = 0) {
        if (policy_ == Policy::Fixed30) return FixedTurn(now, allowed, pending);
        const uint64_t workUs = wholeUs > backendUs ? wholeUs : backendUs;
        const bool gap = started_ && (now < last_ || now - last_ > 250000);
        if (!started_ || gap || !allowed) {
            debt_ = 0;
            catchup_ = 0;
            phase_ = oracleRemaining_ = 0;
            windowAt_ = now;
            samples_ = bad_ = 0;
            roomKnown_ = true;
            if (gap || !allowed) {
                overUs_ = roomUs_ = 0;
                want_ = divisor_;
            }
        } else {
            const int64_t delta = static_cast<int64_t>(now - last_) - 16683;
            debt_ += delta;
            if (debt_ < 0) debt_ = 0;
            if (debt_ > 66732) debt_ = 66732;
        }
        if (!started_) changedAt_ = now;
        started_ = true;
        last_ = now;

        // Render-thread CPU alone cannot establish GPU headroom, especially while paced at 20.
        const bool recoveryKnown = wholeUs != 0 && workUs < 28000;
        // Do not carry completed good windows across bad evidence in the current partial window.
        if (!recoveryKnown || pending >= 2 || debt_ >= 8341) {
            roomUs_ = 0;
            if (want_ == 2) want_ = divisor_;
        }
        if (quiet && want_ != divisor_ && (want_ != 2 || recoveryKnown) && now >= changedAt_ && now - changedAt_ >= 5000000) {
            divisor_ = want_;
            changedAt_ = now;
            phase_ = oracleRemaining_ = 0;
            overUs_ = roomUs_ = 0;
            ++changes_;
        }
        const bool late = debt_ >= 8341 && catchup_ < 3;
        catchup_ = late ? catchup_ + 1 : 0;
        const bool due = phase_ == 0;
        phase_ = (phase_ + 1) % divisor_;
        const int decision = !allowed ? 0 : late ? 2 : (!due || pending >= 2) ? 1 : 0;

        if (verify_) {
            // Independent countdown oracle for picture selection, not game logic.
            const bool oracleDue = oracleRemaining_ == 0;
            oracleRemaining_ = oracleDue ? divisor_ - 1 : oracleRemaining_ - 1;
            const int expected = !allowed ? 0 : late ? 2 : (!oracleDue || pending >= 2) ? 1 : 0;
            if (decision != expected) ++mismatches_;
        }

        if (quiet || !allowed || gap) {
            windowAt_ = now;
            samples_ = bad_ = 0;
            roomKnown_ = true;
        } else {
            ++samples_;
            roomKnown_ = roomKnown_ && recoveryKnown;
            // A single missed deadline is not evidence for a slower mode.
            if (late || pending >= 2 || workUs > 34500) ++bad_;
            if (now >= windowAt_ && now - windowAt_ >= 1000000) {
                const uint64_t span = now - windowAt_;
                if (samples_ && bad_ * 5 >= samples_) {
                    overUs_ += span;
                    roomUs_ = 0;
                    want_ = divisor_;
                } else if (bad_ == 0 && roomKnown_) {
                    roomUs_ += span;
                    overUs_ = 0;
                    want_ = divisor_;
                } else {
                    overUs_ = roomUs_ = 0;
                    want_ = divisor_;
                }
                if (overUs_ >= 3000000) want_ = 3;
                if (roomUs_ >= 6000000) want_ = 2;
                windowAt_ = now;
                samples_ = bad_ = 0;
                roomKnown_ = true;
            }
        }
        return decision;
    }

    unsigned Divisor() const { return divisor_; }
    unsigned Changes() const { return changes_; }
    uint64_t Mismatches() const { return mismatches_; }

private:
    int FixedTurn(uint64_t now, bool allowed, unsigned pending) {
        const bool gap = started_ && (now < last_ || now - last_ > 250000);
        if (!started_ || gap) {
            phase_ = 0;
            forcePresent_ = true;
        }
        if (!started_ || gap || !allowed) {
            debt_ = 0;
            catchup_ = 0;
        } else {
            debt_ += static_cast<int64_t>(now - last_) - 16683;
            if (debt_ < 0) debt_ = 0;
            if (debt_ > 66732) debt_ = 66732;
        }
        started_ = true;
        last_ = now;
        const bool late = debt_ >= 8341 && catchup_ < 3;
        catchup_ = late ? catchup_ + 1 : 0;
        const bool due = phase_ == 0;
        phase_ = (phase_ + 1) % 2;
        const int decision = !allowed ? 0 : late ? 2 : (!due || pending >= 2) ? 1 : 0;
        if (decision == 0) {
            // Count emitted pictures, not nominal slots: backlog may skip every due turn.
            presentAllowed_ = forcePresent_ || allowed || due || previousHidden_;
            previousHidden_ = !presentAllowed_;
            forcePresent_ = false;
        }
        return decision;
    }
    uint64_t last_ = 0, changedAt_ = 0, windowAt_ = 0, overUs_ = 0, roomUs_ = 0, mismatches_ = 0;
    int64_t debt_ = 0;
    unsigned divisor_ = 2, want_ = 2, phase_ = 0, oracleRemaining_ = 0;
    unsigned catchup_ = 0, samples_ = 0, bad_ = 0, changes_ = 0;
    bool started_ = false, verify_ = false, roomKnown_ = true;
    Policy policy_ = Policy::Adaptive;
    bool presentAllowed_ = true, previousHidden_ = false, forcePresent_ = true;
};

class PresentationSlots {
public:
    void Reset() { started_ = false; }
    // earliest is the next scanout vblank (GetVcount + 1), after GPU completion.
    uint32_t Next(uint32_t earliest, unsigned divisor) {
        if (!started_ || divisor != divisor_) {
            started_ = true;
            divisor_ = divisor;
            target_ = earliest;
            return target_;
        }
        target_ += divisor_;
        if (static_cast<int32_t>(earliest - target_) > 0) target_ = earliest;
        return target_;
    }
    void Shown(uint32_t actual) { target_ = actual; }

private:
    uint32_t target_ = 0;
    unsigned divisor_ = 2;
    bool started_ = false;
};
}
