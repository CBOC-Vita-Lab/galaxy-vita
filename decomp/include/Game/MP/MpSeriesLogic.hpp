// Galaxy 3.0 multiplayer: the Star Race series, the catch-up head start and the results card's next step (owner: race).
// Pure logic, no game headers and no global state (the host unit test tests/mp/series_test.cpp drives it directly; MpFlowLogic.hpp
// carries it in GO and RESULTS; decomp/src/Game/MP/MpRace.cpp owns the one instance).
//
//  - Series: a running score across the races of one room. Points per place (n = the players of that race): n - place, so 4 players
//    3-2-1-0, 3 players 2-1-0, 2 players 1-0; ties share a place and its points; a player who did not finish gets 0. The score is
//    shown as points (a Power Star and a number) on the results card: "wins" would show nothing for a player who is always second.
//    A player who joins mid-series starts at 0; one who leaves keeps their line (greyed) until the series ends; the host's state is
//    the authority: it is carried whole in every GO and RESULTS, so every console holds it and a migrated host continues from it.
//  - Head start (catch-up): the sole winner of the previous race starts the next countdown late: 3 s after one win in a row, 6 s after
//    two, 9 s after three or more; it ends when anyone else wins (or the winner leaves, or the race had no sole winner).
//  - The results card's next step, chosen by the host: Race Again / Next Star / Leave Race (RESULTS tail: choice, next scenario).
//  - Timing: a console counts its own turns from ITS OWN start (StartGate), the leaderboard gets exactly those turns; the room's
//    standings add the head start (ranking by who crossed the line first).
#pragma once

#include "port/mp_proto.h"

namespace MpSeriesLogic {

enum : int { kMaxPoints = 99, kMaxStreak = 3, kStepSec = 3, kFps = 60, kBlockBytes = 6, kTailBytes = 8 };
enum : uint8_t { kLinesMask = 0x0F };

static inline int popc4(unsigned m) { return (int)(m & 1) + (int)((m >> 1) & 1) + (int)((m >> 2) & 1) + (int)((m >> 3) & 1); }

// Points for a place (1-based, ties share the place) in a race of `players` players; 0 for no place (did not finish).
static inline int pointsForPlace(int place, int players) {
    if (place < 1 || players < 2 || place > players) return 0;
    return players - place;
}

// ---- the series block (6 bytes on the wire): pts[4] u8 (<= 99), lines u8 (low nibble: slots with a line; high nibble: of those, the
// slots that left: greyed), streak u8 (low 2 bits: the streak holder's slot; bits 2..3: its wins in a row 0..3; bits 4..7 zero) ------
struct Block {
    uint8_t pts[MP_MAX_PLAYERS] = {0, 0, 0, 0};
    uint8_t in = 0;           // slots with a line
    uint8_t left = 0;         // subset of `in`: the player left, the line is greyed
    uint8_t streakSlot = 0;   // the last race's sole winner (valid when streakLen > 0)
    uint8_t streakLen = 0;    // 0..3 wins in a row
};
static inline bool operator==(const Block& a, const Block& b) {
    for (int i = 0; i < MP_MAX_PLAYERS; i++) if (a.pts[i] != b.pts[i]) return false;
    return a.in == b.in && a.left == b.left && a.streakLen == b.streakLen && (a.streakLen == 0 || a.streakSlot == b.streakSlot);
}
static inline bool blockValid(const Block& b) {
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        if (b.pts[i] > kMaxPoints) return false;
        if (b.pts[i] != 0 && !(b.in & (1u << i))) return false;
    }
    if ((b.in & ~kLinesMask) || (b.left & ~b.in)) return false;
    if (b.streakLen > kMaxStreak) return false;
    if (b.streakLen > 0) {
        if (b.streakSlot >= MP_MAX_PLAYERS) return false;
        if (!(b.in & (1u << b.streakSlot)) || (b.left & (1u << b.streakSlot))) return false;
    } else if (b.streakSlot != 0) {
        return false;
    }
    return true;
}
static inline void writeBlock(MpWriter& w, const Block& b) {
    for (int i = 0; i < MP_MAX_PLAYERS; i++) w.u8(b.pts[i]);
    w.u8((uint8_t)((b.in & kLinesMask) | ((b.left & kLinesMask) << 4)));
    w.u8((uint8_t)((b.streakLen ? b.streakSlot : 0) | (b.streakLen << 2)));
}
// Bounds-checked: false = the bytes are not a series block (nothing is kept).
static inline bool readBlock(MpReader& r, Block* out) {
    Block b;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) b.pts[i] = r.u8();
    const uint8_t lines = r.u8(), st = r.u8();
    if (!r.ok()) return false;
    b.in = lines & 0x0F;
    b.left = (uint8_t)(lines >> 4);
    if (st & 0xF0) return false;
    b.streakSlot = (uint8_t)(st & 3);
    b.streakLen = (uint8_t)((st >> 2) & 3);
    if (b.streakLen == 0 && b.streakSlot != 0) return false;
    if (!blockValid(b)) return false;
    *out = b;
    return true;
}

// ---- the results card's next step ---------------------------------------------------------------------------------------------------
enum Choice : uint8_t { CH_NONE = 0, CH_AGAIN = 1, CH_NEXT = 2, CH_LEAVE = 3, CH_COUNT = 4 };
enum : int { kMaxNextScenario = 16 };
// lobby: a guest cannot choose, but can ASK for a rematch. The ask rides the existing quick phrase 3 ("Let's go!", MP_MSG_CHAT, no wire change:
// every build already sends, relays and reads it); a leader whose results card is up reads it from a guest as "<name> wants to race again".
// A guest asks once per card. Nothing changes for anyone who does not know the ask: to them it is just "Let's go!".
enum : uint8_t { kAskPhrase = 3 };
static inline bool isAgainAsk(uint8_t chatKind, uint8_t phrase) { return chatKind == 0 /* MP_CHAT_PHRASE */ && phrase == kAskPhrase; }
// menus4: nothing on the wire tells the ask from a "Let's go!" typed by hand, so the two are made one thing: on a console whose results card is
// up, phrase 3 IS the ask (a guest sending it by hand marks the card "Asked!"; MpOverlay::noteAskSentByHand), and is shown as "Race Again?" in
// the bubble and the toast. Without a card on that console it is the plain phrase.
static inline bool asksAsQuestion(uint8_t chatKind, uint8_t phrase, bool resultsCardUp) { return resultsCardUp && isAgainAsk(chatKind, phrase); }
// lobby: the card moves on by itself when nobody is at the leader's console (kSeriesIdle seconds). Race Again twice; if nobody has pressed
// or asked anything in between, the third time it ends the series (Leave Race: back to the galaxy map) instead of racing for nobody.
enum : int { kMaxAutoRuns = 2 };
// B6: how long the results card waits for the leader. The leader's own card chooses by itself after kCardIdleHostMs without a press; a guest whose card
// still has no choice after kCardIdleGuestMs leaves on its own (the Leave Race row's leave), so a leader who is connected but away never holds anyone for good.
enum : int { kCardIdleHostMs = 90000, kCardIdleGuestMs = 120000 };
static inline uint8_t autoChoice(int autoRuns) { return autoRuns >= kMaxAutoRuns ? (uint8_t)CH_LEAVE : (uint8_t)CH_AGAIN; }
// RESULTS tail (8 bytes): the series after this race, the host's choice (CH_NONE while it has not chosen) and the scenario "Next
// Star" would go to (0 = there is none: the row is not shown). Only the host sends it; a guest's copy is a display.
struct Tail {
    Block series;
    uint8_t choice = CH_NONE;
    uint8_t next = 0;
};
static inline bool tailValid(const Tail& t) {
    if (!blockValid(t.series) || t.choice >= CH_COUNT || t.next > kMaxNextScenario) return false;
    if (t.choice == CH_NEXT && t.next == 0) return false;
    return true;
}
static inline void writeTail(MpWriter& w, const Tail& t) { writeBlock(w, t.series); w.u8(t.choice); w.u8(t.next); }
static inline bool readTail(MpReader& r, Tail* out) {
    Tail t;
    if (!readBlock(r, &t.series)) return false;
    t.choice = r.u8();
    t.next = r.u8();
    if (!r.ok() || !tailValid(t)) return false;
    *out = t;
    return true;
}

// The next star of the galaxy the room can play: the first scenario after `current` that the star select would offer, i.e. a
// normal (not hidden) scenario <= normalCount whose predecessors are all collected (`collectedMask` bit k-1 = scenario k collected;
// `current` counts as collected: it was just played). 0 = none.
static inline int nextScenario(int current, int normalCount, unsigned collectedMask) {
    if (current < 1 || normalCount < 1 || current >= normalCount || normalCount > 32) return 0;
    const int s = current + 1;
    for (int k = 1; k < s; k++)
        if (k != current && !(collectedMask & (1u << (k - 1)))) return 0;
    return s;
}

// ---- the series ------------------------------------------------------------------------------------------------------------------------
struct Placing { uint8_t slot, place; };   // place 1.. (ties share one), 0 = did not finish

struct Series : Block {
    void reset() { *static_cast<Block*>(this) = Block(); }
    const Block& block() const { return *this; }
    // Replace the whole state by a block taken from the host (a guest's copy, or a new host's inheritance). Invalid: unchanged.
    bool restore(const Block& b) {
        if (!blockValid(b)) return false;
        *static_cast<Block*>(this) = b;
        return true;
    }
    // Joins and leaves. A present player with no line gets one at 0; a player with a line who is gone is greyed (points kept); a
    // slot that is present again after its player left is a new player: the line starts again at 0.
    void syncPresence(unsigned present) {
        present &= kLinesMask;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            const uint8_t bit = (uint8_t)(1u << i);
            if (present & bit) {
                if (!(in & bit) || (left & bit)) pts[i] = 0;
                in |= bit;
                left = (uint8_t)(left & ~bit);
            } else if (in & bit) {
                left |= bit;
            }
        }
        if (streakLen > 0 && (left & (1u << streakSlot))) clearStreak();
    }
    void clearStreak() { streakLen = 0; streakSlot = 0; }
    // One race is over: n players placed. Points for everyone; the streak goes on when the same sole winner won again (only with
    // catch-up on and at least two players in the race), starts when a new sole winner appears, and ends otherwise.
    void applyRace(const Placing* p, int n, bool catchUp) {
        if (n < 0) n = 0;
        if (n > MP_MAX_PLAYERS) n = MP_MAX_PLAYERS;
        int winners = 0, winner = -1;
        for (int i = 0; i < n; i++) {
            if (p[i].slot >= MP_MAX_PLAYERS) continue;
            const uint8_t bit = (uint8_t)(1u << p[i].slot);
            in |= bit;
            left = (uint8_t)(left & ~bit);
            int v = pts[p[i].slot] + pointsForPlace(p[i].place, n);
            pts[p[i].slot] = (uint8_t)(v > kMaxPoints ? kMaxPoints : v);
            if (p[i].place == 1) { winners++; winner = p[i].slot; }
        }
        if (!catchUp || n < 2 || winners != 1) {
            clearStreak();
            return;
        }
        if (streakLen > 0 && streakSlot == winner) streakLen = (uint8_t)(streakLen < kMaxStreak ? streakLen + 1 : kMaxStreak);
        else { streakSlot = (uint8_t)winner; streakLen = 1; }
    }
    // Seconds the slot starts late in the next countdown. 0 unless catch-up is on, the slot holds the streak, is here, and
    // someone else is here to be given the head start.
    int headStartSec(int slot, bool catchUp, unsigned present) const {
        if (!catchUp || streakLen == 0 || slot < 0 || slot >= MP_MAX_PLAYERS || slot != streakSlot) return 0;
        if (!(present & (1u << slot)) || popc4(present) < 2) return 0;
        return (int)streakLen * kStepSec;
    }
    // The block the host sends in GO: the head start is the streak, valid for this race only when catch-up is on.
    Block goBlock(bool catchUp, unsigned present) const {
        Block b = *this;
        if (headStartSec(streakSlot, catchUp, present) == 0) { b.streakLen = 0; b.streakSlot = 0; }
        return b;
    }
};

// Head start of a slot from a block a console received (GO), as frames at 60/s. 0..540.
static inline uint32_t headStartFrames(const Block& b, int slot) {
    if (b.streakLen == 0 || slot < 0 || slot >= MP_MAX_PLAYERS || b.streakSlot != slot) return 0;
    return (uint32_t)b.streakLen * kStepSec * kFps;
}

// ---- timing --------------------------------------------------------------------------------------------------------------------------
// One console's start: the countdown is the room's; after it the head-start slot stays held for headFrames more turns; then its OWN
// clock runs. `frames` is the player's own time from their own start: what the leaderboard gets, never more and never less than
// the turns actually raced. The room's standings use roomFrames() = frames + the head start (the same moment on everyone's clock).
struct StartGate {
    uint32_t headFrames = 0;   // head start left to wait
    uint32_t headTotal = 0;
    uint32_t frames = 0;       // own turns since the release
    bool racing = false;
    void begin(uint32_t head) { headFrames = headTotal = head; frames = 0; racing = head == 0; }
    // One game turn, every turn from the end of the countdown (the first one included: the room's turn 1 is the end of the
    // countdown, as before). Returns true on the turn the player is released (the first turn counted: frames == 1 then).
    bool tick() {
        bool released = false;
        if (!racing) {
            if (headFrames > 0) { headFrames--; return false; }
            racing = true;
            released = true;
        }
        frames++;
        return released || (frames == 1 && headTotal == 0);
    }
    uint32_t ownFrames() const { return frames; }
    uint32_t roomFrames() const { return frames + headTotal; }
};
// What the leaderboard is given for a finished race: the player's own frames, whatever the head start was.
static inline uint32_t leaderboardFrames(const StartGate& g) { return g.ownFrames(); }

}  // namespace MpSeriesLogic
