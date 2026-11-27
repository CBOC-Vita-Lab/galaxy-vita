#pragma once

namespace port { namespace popin {
// Two departing shadow controllers can spend a little longer near the cut. Existing
// owners keep priority on a reversal; an absent owner's slot expires after the batch.
struct HoneyShadowHolds {
    const void* owner[2] = {};
    unsigned seen = 0;

    void begin() { seen = 0; }
    bool keep(const void* who, bool previouslyDrawn, bool budgetFar, float depth, float limit) {
        const float band = limit * 0.025f < 80.0f ? limit * 0.025f : 80.0f;
        if (!previouslyDrawn || !(depth >= limit - band && depth < limit + band)) return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (owner[i] == who) {
                seen |= 1u << i;
                return budgetFar;
            }
        }
        if (budgetFar) {
            for (unsigned i = 0; i < 2; ++i) {
                if (!owner[i]) {
                    owner[i] = who;
                    seen |= 1u << i;
                    return true;
                }
            }
        }
        return false;
    }
    void end() {
        for (unsigned i = 0; i < 2; ++i) if (!(seen & (1u << i))) owner[i] = nullptr;
    }
};
} }
