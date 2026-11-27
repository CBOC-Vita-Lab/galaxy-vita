// farfix35 (21 Nov 2026): GXV_FARANIM_FIX (default 1; 0 = the game's own numbers). Pure parts, host-tested by tests/farfix.cpp (tools/test_farfix.sh).
//
// WHY. Beach Bowl, owner's report: a waterfall near the cave tunnel "stops" while Mario swims and starts again abruptly as he nears. A waterfall is a
// SimpleEffectObj (WaterfallS / WaterfallL = EffectObjR1000F50: a 10 m clipping sphere, far clip level 50 m = 5000 units). The GAME's own clipping
// (ClippingActorHolder -> LiveActor::startClipped -> EffectKeeper::stopEmitterOnClipped -> MultiEmitter: StopCalc + StopDraw) freezes and hides its emitters
// when the sphere leaves the view frustum or lies beyond 50 m; endClipped clears the flags and the particles go on from the frozen state. The port does
// not shorten that (GXV_CLIPSCALE only takes pure decoration, GXV_POPIN_CLIPPAD is 0, the budget's particle cut thins emission and never freezes). Every
// port pose / material cut keeps its clock (BCK released as a made pose releases it; MATLOD: frame counters advance in movement; FARANIM / PB_ANIM / FAMPOSE:
// at most n-1 turns in a row). So the freeze is the original's, and the fix is to give scenery waterfalls a larger sphere and one far-clip level more.
//
// WHAT. A waterfall's clipping sphere radius x kRadiusMul (1.5: the visible fall is longer than the 10 m sphere) and its far clip 50 m -> 100 m (the next
// level of the game's table, 10000 units).
#pragma once
#include <cstdlib>
#include <cstring>

namespace port {
namespace farfix {

constexpr float kRadiusMul = 1.5f;
constexpr int kFarMetersFrom = 50, kFarMetersTo = 100;

inline bool ParseOn(const char* e) { return !(e && e[0] == '0' && e[1] == 0); }
inline bool On() {
    static const bool v = ParseOn(std::getenv("GXV_FARANIM_FIX"));
    return v;
}
// an effect object's name that is a scenery waterfall (WaterfallS / L, ForestWaterfallS / L, TwinFallLakeWaterFall, ...)
inline bool IsWaterfall(const char* name) { return name != nullptr && (std::strstr(name, "Waterfall") != nullptr || std::strstr(name, "WaterFall") != nullptr); }
inline float Radius(float base, const char* name, bool on) { return on && IsWaterfall(name) ? base * kRadiusMul : base; }
inline int FarMeters(int base, const char* name, bool on) { return on && IsWaterfall(name) && base == kFarMetersFrom ? kFarMetersTo : base; }

}  // namespace farfix
}  // namespace port
