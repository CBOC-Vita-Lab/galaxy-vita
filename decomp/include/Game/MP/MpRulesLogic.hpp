// Galaxy 3.0 multiplayer: when the host-rules card shows (touches). Pure logic, no game headers (tests/mp/rules_test.cpp).
//
//  - The first time a player joins a given host's room: a card in the game's own window (in the lobby), or, for a late joiner who
//    arrives during play, a short toast that does not block anything.
//  - Whenever the host changes a rule (mode, falls, bumping) mid-lobby or mid-game: the card again (lobby) / a toast (play), once the
//    host stopped fiddling for a moment (1.5 s), so a run of changes is one card.
//  - "Don't show again" (mp.cfg) silences the card and the join toast for every host; a changed rule still gets its small toast.
#pragma once

#include <stdint.h>

namespace MpRulesLogic {

constexpr int kSettleFrames = 90;
enum Action : uint8_t { NONE = 0, CARD, TOAST_JOIN, TOAST_CHANGE };

// FNV-1a of the host's name, never 0.
static inline uint32_t hostKey(const char* name) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name ? name : ""); *p; ++p) h = (h ^ *p) * 16777619u;
    return h ? h : 1u;
}
static inline uint32_t sigOf(int mode, int falls, bool bump) { return static_cast<uint32_t>((mode & 3) | ((falls & 3) << 2) | (bump ? 16 : 0)) + 1u; }

struct Watch {
    uint32_t key = 0, sig = 0, pendingSig = 0;
    int settle = 0;
    bool announced = false;
    void reset() { *this = Watch(); }
    // Once a frame while we are a guest in a session. inLobby: our lobby screen is up and can take a window (nothing else open).
    // wanted: this host's card is due (mp.cfg); hidden: "don't show again".
    Action step(uint32_t hostKeyNow, uint32_t sigNow, bool inLobby, bool wanted, bool hidden) {
        if (hostKeyNow != key) {   // a new host (the first one, or a migration): its own first-time card
            key = hostKeyNow;
            sig = pendingSig = sigNow;
            settle = 0;
            announced = false;
        }
        if (!announced) {
            if (hidden || !wanted) { announced = true; return NONE; }
            if (inLobby) { announced = true; return CARD; }
            announced = true;
            return TOAST_JOIN;
        }
        if (sigNow == sig) { pendingSig = sig; settle = 0; return NONE; }
        if (sigNow != pendingSig) { pendingSig = sigNow; settle = 0; return NONE; }
        if (++settle < kSettleFrames) return NONE;
        sig = sigNow;
        settle = 0;
        return inLobby && !hidden ? CARD : TOAST_CHANGE;
    }
};

}  // namespace MpRulesLogic
