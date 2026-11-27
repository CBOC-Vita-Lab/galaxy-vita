// Galaxy 3.0 multiplayer: when the "multiplayer was entered in this boot" flag (port/mp_bridge.h mp_entered) may be cleared again (optfix;
// pure logic, host tested in tests/mp/entered_test.cpp; the game side is MpGame.cpp retireEntered).
//
// After the player opened the multiplayer menu once, the per-frame hooks (MpSaveGame::tick, mp_tick_game, drainEvents, afterSessionStep, the
// UI's input reads) ran every frame for the rest of the boot. Once the session is over and nothing of multiplayer is on screen or pending
// for kQuietTurns turns in a row, and the core is idle, the flag is cleared: the game runs the early-outs of a boot that never entered. The
// next opening of a menu / a session / a scan sets it again (mp_mark_entered).
#pragma once

#include <stdint.h>

namespace MpEnteredLogic {
    enum { kQuietTurns = 180, kRetryTurns = 60 };

    struct Inputs {
        bool sessionActive;   // mp_active()
        bool uiSettled;       // MpUi::settledQuiet(): no menu, toast, card, strip, window; not rejoining
        bool saveSettled;     // MpSaveGame::settled(): no leave decision pending, no progress held, no write in flight
        bool returnPending;   // the back-to-title step after a session still waits
        bool demoOrAutopilot; // the puppet demo or the desktop autopilot lives on the flag
    };

    class Retire {
    public:
        // One game turn. Returns true on the turn the flag should be cleared. coreIdle() is asked only when everything else has held
        // for kQuietTurns turns (it may take a lock): false keeps the flag and asks again after kRetryTurns more.
        template < class CoreIdle >
        bool turn(const Inputs& in, CoreIdle coreIdle) {
            if (in.sessionActive || !in.uiSettled || !in.saveSettled || in.returnPending || in.demoOrAutopilot) {
                mQuiet = 0;
                return false;
            }
            if (++mQuiet < kQuietTurns) return false;
            if (!coreIdle()) {
                mQuiet = kQuietTurns - kRetryTurns;
                return false;
            }
            mQuiet = 0;
            return true;
        }
        uint32_t quietTurns() const { return mQuiet; }

    private:
        uint32_t mQuiet = 0;
    };
}  // namespace MpEnteredLogic
