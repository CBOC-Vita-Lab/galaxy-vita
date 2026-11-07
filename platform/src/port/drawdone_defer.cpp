// GXV_DRAWDONE_DEFER (port/drawdone_defer.h; read once, default 0): the game thread's wait for the front end at the end of a drawn
// turn is made later, at the start of the next turn's update() (1) or calcAnim() (2).  The calls are from the game thread only
// (MainLoopFramework::waitDrawDoneAndSetAlarm, GameSystem::update / calcAnim, GXSetDrawDone); the SDK calls stay with them.
#include <stdint.h>
#include <stdlib.h>

#include "port/diagnostics.h"
#include "port/drawdone_defer.h"
#include "port/hot_knob.h"
#include "port/port.h"

namespace {
port::drawdone::Deferral& DrawDoneDeferralOnce() {
    // Modes 3 and 4 need the material lists made once per picture (GXV_DL_LAZY=2, the default): with another value they act as 2.
    static port::drawdone::Deferral d([] {
        int m = port::drawdone::ParseMode(getenv("GXV_DRAWDONE_DEFER"));
        const char* lazy = getenv("GXV_DL_LAZY");
        if (m >= 3 && lazy && *lazy && atoi(lazy) != 2) m = 2;
        return m;
    }());
    return d;
}
// (GXV_HOT_KNOBS: its address read without a barrier, port/hot_knob.h; the object is still made once, by the guarded static above)
port::drawdone::Deferral& DrawDoneDeferral() { return *PORT_HOT_KNOB(port::drawdone::Deferral*, &DrawDoneDeferralOnce()); }
}  // namespace

extern "C" int port_gx_drawdone_defer_mode(void) { return DrawDoneDeferral().Mode(); }
extern "C" int port_gx_drawdone_pending(void) { return DrawDoneDeferral().Pending() ? 1 : 0; }
extern "C" void port_gx_drawdone_armed(void) { DrawDoneDeferral().Arm(); }
extern "C" int port_gx_drawdone_must_wait(int point) { return DrawDoneDeferral().MustWait(point) ? 1 : 0; }
extern "C" void port_gx_drawdone_waited(int64_t ns) {
    port::drawdone::Deferral& d = DrawDoneDeferral();
    d.Waited(ns);
    char line[240];
    if (PORT_RUNTIME_DIAGNOSTICS && d.Report(line, sizeof line, 300)) port_log_summary("%s", line);
}
