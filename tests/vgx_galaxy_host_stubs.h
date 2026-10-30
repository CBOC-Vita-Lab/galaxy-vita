// Host stand-ins for the renderer and Vita host hooks that platform/src/gx/vgx_galaxy.cpp calls and a test that includes it
// does not provide itself (diagnostic counters, frame pacing swap counter, settings hold hooks, quality rung size, character AA counters).
// Include after vgx_galaxy.cpp. Every one reports "nothing happening".
#pragma once
#include <cstdint>

extern "C" unsigned long long port_vgx_vita_game_swaps(void) { return 0; }
extern "C" void port_vgx_vita_set_present_allowed(int) {}
extern "C" void port_vgx_vita_finish_unpresented_frame(void) {}
extern "C" int port_vgx_vita_settings_finish(unsigned) { return 1; }
extern "C" void port_vgx_vita_settings_resume(void) {}
namespace vgx {
int CharaAAMode() { return 0; }
void StateListCounters(uint64_t out[5]) { for (int i = 0; i < 5; ++i) out[i] = 0; }
void RegShadowCounters(uint64_t out[2]) { out[0] = out[1] = 0; }
void RigidMergeCounters(uint64_t out[2]) { out[0] = out[1] = 0; }
void ResolveCacheCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
void TexMemoCounters(uint64_t out[3]) { for (int i = 0; i < 3; ++i) out[i] = 0; }
namespace hdtex { void GetCounters(Counters& c) { c = Counters{}; } }
namespace split {
void Counters(int64_t& finisherCpuUs, int64_t& playerWaitNs, uint32_t& frames) { finisherCpuUs = playerWaitNs = 0; frames = 0; }
uint32_t StolenOps() { return 0; }
}
void CharaCounters(uint64_t out[11]) { for (int i = 0; i < 11; ++i) out[i] = 0; }
namespace backend {
void QualityRungSize(int& w, int& h, int& samples) { w = h = samples = 0; }
void CharaAACounters(uint64_t& presents, uint64_t& boxes, uint64_t& pixels, uint64_t& msaaSkips) {
    presents = boxes = pixels = msaaSkips = 0;
}
}
}
