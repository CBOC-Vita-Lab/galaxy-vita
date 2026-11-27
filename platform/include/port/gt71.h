// gt71 (23 Nov 2026): exact game-thread overhead cuts found by the on-console pc sampler (SAMPLER-c46-beach: Beach Bowl, plain 60, 94,775 running samples).
// ONE switch, GXV_GT71 = a bit mask (default all bits on; 0 = every change below off, the code as it was in c46):
//   1  ENV    GameScene::update read getenv("PETARI_MOVIE") every turn (a scan of the whole environment: _findenv_r + strncmp, 1.2 % of the turn);
//             the answer is kept from the first turn. The environment is set before the game starts and nothing changes it.
//   2  CLOCK  clock reads that only feed a log line (the sampled timers of the actor-turn LOD, the draw replay and the animation rules: one in 8 / 16
//             of every object, two reads each, ~0.87 us a read on the console) are not made. The game never reads these numbers; the log lines they fed show 0.
//   4  AREA   the manager-name memo of AreaObjContainer::getManager (GXV_AREA_GRID's memo, exact and verified clean on the console) also with GXV_AREA_GRID=0:
//             the walk over ~60 managers by name becomes a hashed pointer + the text compared (the same compare as the memo's own).
//   8  PROF   GXV_ANIM_PROF's "is it on" asked once a list instead of a call a object, and as an inline read where an object asks it.
//  (MR::getSceneObjHolder stays a call: it is declared NO_INLINE in a header that most of the 1,900 units include; changing that would rebuild them all.)
#pragma once

#include <cstdint>
#include <cstdlib>

#include "port/hot_knob.h"

extern "C" int port_anim_prof_on(void);
namespace port {
namespace gt71 {
enum : unsigned { kEnv = 1, kClock = 2, kArea = 4, kProf = 8, kAll = 15 };
inline unsigned Bits() {
    return PORT_HOT_KNOB(unsigned, [] {
        const char* e = std::getenv("GXV_GT71");
        return e && *e ? static_cast< unsigned >(std::strtoul(e, nullptr, 0)) : static_cast< unsigned >(kAll);
    }());
}
inline bool On(unsigned bit) { return (Bits() & bit) != 0; }
// A sampled timer is made: only when the clock cut is off.
inline bool StatClockOn() { return (Bits() & kClock) == 0; }
// GXV_ANIM_PROF is on: the same answer as port_anim_prof_on (perf.cpp), which reads the same variable the same way; asked inline with PROF, else by the call as before.
inline bool AnimProfOn() {
    if ((Bits() & kProf) == 0) return port_anim_prof_on() != 0;
    return PORT_HOT_KNOB(int, [] {
               const char* e = std::getenv("GXV_ANIM_PROF");
               return e && *e == '1' ? 1 : 0;
           }()) != 0;
}
}  // namespace gt71
}  // namespace port
