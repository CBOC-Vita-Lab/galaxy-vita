// Exercise production entry gates and the actual CPU far-clip update, independently of a GPU.
#include "port/honey_shadow.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using f32 = float;
struct TVec3f { float x = 0, y = 0, z = 0; };
static float depth, playerD, pct;
static const char* stage;
static bool perf, noDemoBudget, sHoneyStage, sHoneyHoldBatch;
static port::popin::HoneyShadowHolds sHoneyHolds;
namespace MR {
static bool isEqualStageName(const char* name) { return std::strcmp(stage, name) == 0; }
static float calcCameraDistanceZ(const TVec3f&) { return depth; }
}
// popin31: the floor's turn state and the site helper (port/popin_site.h), as the production pair behaves for one shadow: a cut only beyond r * k.
// popin31b: per kind (the shadow's kind is kFlShadow; another kind's floor must not touch it) and the verdict (floor kept / release held / ok)
static struct { float kHeldK[7] = {}; int releasing = 0; } port_pb_floor;
static int sFloorAsked = 0;
namespace port { namespace popin {
enum { kShadow };
enum { kFlShadow = 0, kFlDecor = 1 };
enum { kCutOk = 0, kCutFloor = 1, kCutRelease = 2 };
static float HystPct() { return pct; }
static float Band(float limit, float percent) { return limit * percent * 0.01f; }
static bool LogOn() { return false; }
static bool GuardOn(int fk) { return port_pb_floor.kHeldK[fk] > 0.0f || port_pb_floor.releasing != 0; }
static int CutVerdict(int, bool, float radius, float dist, float, float, float) {
    ++sFloorAsked;
    return dist > radius * port_pb_floor.kHeldK[kFlShadow] ? kCutOk : kCutFloor;
}
} }
static bool port_pb_budget_on() { return perf; }
static bool port_pb_demo_off() { return noDemoBudget; }
static float port_pb_scale() { return 1; }
static float port_pb_lead(float) { return 0; }
static float port_detail_dist(int, float limit) { return limit; }
static float port_pb_player_dist2(float, float, float) { return playerD * playerD; }
static void port_logic_count(int, int) {}
static void port_popin_event(int, int, void*, const TVec3f*, float) {}
struct ShadowController {
    void* mActor = nullptr;
    bool _67 = false, mPbLogicRead = false, mPbShadowFar = false, mHoneyWasDrawn = false;
    unsigned char mPbFloorKept = 0;
    void getDropPos(TVec3f* v) const { *v = {}; }
    float portFloorRadius() { return 100.0f; }
    void updateFarClippingPb(float, float);
};
#include "honey-update.inc"
static void init() {
#include "honey-init.inc"
}
static void batch(float pbShadowClip) {
#include "honey-batch.inc"
}
int main() {
    perf = true; stage = "HoneyBeeKingdomGalaxy"; unsetenv("GXV_HONEY_SHADOW_HOLD");
    init(); batch(2800); assert(sHoneyHoldBatch);
    ShadowController s;
    depth = 2810; playerD = 2000;
    s.updateFarClippingPb(100000,2800); assert(s._67);  // new, not previously drawn
    depth = 2799; s.updateFarClippingPb(100000,2800); assert(!s._67);  // approach is not delayed
    s.mHoneyWasDrawn = true; depth = 2869; s.updateFarClippingPb(100000,2800); assert(!s._67);
    depth = 2870; s.updateFarClippingPb(100000,2800); assert(s._67);
    depth = 5000; playerD = 1199; s.updateFarClippingPb(100000,2800); assert(!s._67);  // guards player
    playerD = 4000; s.mPbLogicRead = true; s.updateFarClippingPb(100000,2800); assert(!s._67);
    s.mPbLogicRead = false; depth = 2810; s.updateFarClippingPb(2700,2800); assert(s._67);  // authored far clip wins
    noDemoBudget = true; batch(2800); assert(!sHoneyHoldBatch);
    s.updateFarClippingPb(100000,2800); assert(!s._67);
    noDemoBudget = false; perf = false; batch(2800); assert(!sHoneyHoldBatch);
    perf = true; pct = 8; batch(2800); assert(!sHoneyHoldBatch);
    pct = 0; batch(0); assert(!sHoneyHoldBatch);
    stage = "AstroGalaxy"; init(); batch(2800); assert(!sHoneyHoldBatch);
    stage = "HoneyBeeKingdomGalaxy"; setenv("GXV_HONEY_SHADOW_HOLD","0",1); init(); batch(2800); assert(!sHoneyHoldBatch);
    // popin31: with GXV_PB_MINPX the floor decides (a 100-unit caster under the floor only beyond 3000 here) and the Honeyhive hold stands aside
    setenv("GXV_HONEY_SHADOW_HOLD","1",1); init(); perf = true; pct = 0; noDemoBudget = false;
    port_pb_floor.kHeldK[port::popin::kFlDecor] = 30.0f; batch(2800); assert(sHoneyHoldBatch);   // (popin31b: another kind's floor leaves the shadows' hold alone)
    port_pb_floor.kHeldK[port::popin::kFlDecor] = 0.0f;
    port_pb_floor.kHeldK[port::popin::kFlShadow] = 30.0f; batch(2800); assert(!sHoneyHoldBatch);
    ShadowController f;
    playerD = 4000; depth = 2900; f.updateFarClippingPb(100000,2800); assert(!f._67 && sFloorAsked == 1);  // beyond the budget's 2800, not under the floor: drawn
    assert(f.mPbFloorKept == 1);                                                                          // (popin31b: kept by the floor: timed for the price line)
    depth = 3100; f.updateFarClippingPb(100000,2800); assert(f._67 && f.mPbFloorKept == 0);               // under the floor: the budget's clip acts
    depth = 2700; f.updateFarClippingPb(100000,2800); assert(!f._67 && sFloorAsked == 2);                 // nearer than the budget: never asked
    port_pb_floor.kHeldK[port::popin::kFlShadow] = 0.0f; batch(2800); assert(sHoneyHoldBatch);            // floor off: the hold as before
    depth = 2900; const int asked = sFloorAsked; f.updateFarClippingPb(100000,2800); assert(f._67 && sFloorAsked == asked);
    std::puts("C183 production shadow clip: approach/departure, player, logic, authored, stage, demo, Quality, rollback PASS");
    std::puts("popin31 shadow floor: a far shadow at or above the floor is drawn, under it clipped; the Honeyhive hold stands aside while the floor is on PASS");
    std::puts("popin31b shadow floor by kind: only the shadows' own floor moves the hold; a kept shadow is marked for the price line PASS");
}
