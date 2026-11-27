// GXV_AUTOQ_SHED (gpuheavy25): the controller's "half-rate captures" level, read by the front end (vgx_draw.cpp IndCapHalfRateNow).
// 1 = the captures may run at half rate (the default, and then the switches GXV_INDCAP_HALFRATE etc. decide as they always did);
// the controller sets 0 at start when it owns the level and 1 only while the level is engaged.
#pragma once
#include <atomic>
namespace vgx::shed {
inline std::atomic<int> g_cutGate{1};
}
