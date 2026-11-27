// Galaxy 3.0 multiplayer: the "away" state of the local player, the pure part (touches). No game headers: tests/mp/away_test.cpp drives it.
//
// Away = "Be right back" (the quick phrase) or 30 seconds without input while playing. It shows to the others as a dimmed head on the
// party strip and "zzz" on the name tag (the MP_POSE_AWAY bit of STATE, MpAway.cpp). Any stick movement or button press brings the
// player back, except while the menu is open (and for a moment after) so the very press that chose the phrase does not undo it.
// It is cosmetic only: nothing waits for it (the load barrier, the star delivery and the race never look at it).
#pragma once

namespace MpAwayLogic {

constexpr int kIdleTurns = 30 * 60;   // 30 s at 60 turns/s
constexpr int kGraceTurns = 60;       // input ignored for 1 s after the phrase / after a menu closes

struct State {
    bool away = false;
    bool byPhrase = false;
    int idle = 0;
    int grace = 0;
    void reset() { *this = State(); }
    // "Be right back" was sent
    void brb() {
        away = true;
        byPhrase = true;
        idle = 0;
        grace = kGraceTurns;
    }
    // once a turn. input: a button or the stick this turn. playing: the player can be steered (action, no demo, alive).
    // menuOpen: a menu / window of ours is up. Returns away.
    bool step(bool input, bool playing, bool menuOpen) {
        if (menuOpen) {
            grace = kGraceTurns;
            idle = 0;
            return away;
        }
        if (grace > 0) --grace;
        if (input) {
            idle = 0;
            if (away && grace == 0) {
                away = false;
                byPhrase = false;
            }
            return away;
        }
        if (!away && playing && ++idle >= kIdleTurns) {
            away = true;
            byPhrase = false;
        }
        return away;
    }
};

}  // namespace MpAwayLogic
