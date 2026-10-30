// GXV_DRAWDONE_DEFER (port/drawdone_defer.h; read once, default 0): the game thread's wait for the front end at the end of a drawn
// turn is made later, at the start of the next turn's update() (1) or calcAnim() (2).  The calls are from the game thread only
// (MainLoopFramework::waitDrawDoneAndSetAlarm, GameSystem::update / calcAnim, GXSetDrawDone); the SDK calls stay with them.
#include <stdint.h>
#include <stdlib.h>

#include "port/diagnostics.h"
#include "port/drawdone_defer.h"
#include "port/port.h"

namespace {
port::drawdone::Deferral& DrawDoneDeferral() {
    static port::drawdone::Deferral d(port::drawdone::ParseMode(getenv("GXV_DRAWDONE_DEFER")));
    return d;
}
}  // namespace

extern "C" int port_gx_drawdone_defer_mode(void) { return DrawDoneDeferral().Mode(); }
extern "C" void port_gx_drawdone_armed(void) { DrawDoneDeferral().Arm(); }
extern "C" int port_gx_drawdone_must_wait(int point) { return DrawDoneDeferral().MustWait(point) ? 1 : 0; }
extern "C" void port_gx_drawdone_waited(int64_t ns) {
    port::drawdone::Deferral& d = DrawDoneDeferral();
    d.Waited(ns);
    char line[240];
    if (PORT_RUNTIME_DIAGNOSTICS && d.Report(line, sizeof line, 300)) port_log_summary("%s", line);
}
