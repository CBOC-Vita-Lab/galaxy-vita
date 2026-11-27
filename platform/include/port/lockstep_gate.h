#pragma once
// MP3 (checks83): pure rules for "no clock-driven logic decision in a lockstep session". Header only, no game types, no globals: tests/mp/lockstep_gate_test.cpp runs
// this code. The callers read the flag themselves: each file declares
//     extern "C" int port_net_session_lockstep(void) __attribute__((weak));
// (the strong definition is the session layer's; a build without it counts as 0) and passes `port_net_session_lockstep && port_net_session_lockstep()` here.
//
// PRINCIPLE (Fable review, 14 Nov). Multiplayer is host-authoritative and every console runs in real time. Pacing that keeps game time real-time is NOT gated:
// undrawn turns (full logic, only the picture is skipped), late catch-up turns (port_frameskip_turn code 2) and the speed guard's repayment. Gating them would make a
// slow console play in slow motion and would not make consoles agree. What IS gated is anything that changes WHICH logic runs, or how much, from this console's
// wall clock: a far-actor level swept in from measured frame times (Quality ladder, far logic).
// Drawing-only budgets are not touched.
#include <stdint.h>

namespace port {
namespace lockstep {

// The far-actor logic level that is acted on (port::farlogic::LevelInSession adds the plain multiplayer gate; this adds the lockstep one): 0 in either.
inline int FarLogicLevel(int level, bool mpActive, bool lockstep) { return (mpActive || lockstep) ? 0 : level; }

// The Quality ladder's far-logic distance (step 3l, a function of measured frame times): 0 = off in a lockstep session.
inline float QualityLogicLimit(float limit, bool lockstep) { return lockstep ? 0.0f : limit; }

}  // namespace lockstep
}  // namespace port
