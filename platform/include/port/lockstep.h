// fmt32 (13 Nov 2026): the multiplayer session's lockstep flag, for the performance features that change the game's LOGIC, its turn timing or its speed.
//
// A lockstep session runs the same game turns on two consoles from the same inputs. Every feature that decides something about the logic from what only ONE
// console knows (its camera, its picture mode, its governors' level, its clock, how late its pictures are) makes the two games differ. Each such feature asks
// port::lockstep::On() at its own decision point and behaves exactly as if it were switched off while it is true (the list: new/fmt32/REPORT.txt).
//
// The multiplayer side says when: it defines
//     extern "C" int port_net_session_lockstep(void);   // 1 while a synchronised session is running (best: from before the stage's actors are made), else 0
// as an ordinary (strong) function in a unit that is linked into the game. The default (platform/src/port/perf.cpp) is a weak function that returns 0: single
// player. Do NOT declare the hook weak in a unit that defines it. The value is read ONCE at the start of each game turn (port_perf_frame_begin, game thread)
// and kept in gTurn, so every decision of one turn sees the same answer and a feature pays one load of a word when no session runs.
#pragma once

namespace port {
namespace lockstep {
inline int gTurn = 0;  // 1 during the turns of a lockstep session (written by the game thread at the start of a turn; read by anyone)
inline bool On() { return __builtin_expect(__atomic_load_n(&gTurn, __ATOMIC_RELAXED) != 0, 0); }
}  // namespace lockstep
}  // namespace port
