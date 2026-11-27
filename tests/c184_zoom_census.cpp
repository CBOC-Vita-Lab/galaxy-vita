// The actual zoom-cull and release code. A released stamp is not necessarily an appearance.
#include <cassert>
#include <cstdio>
using u32 = unsigned;
struct TVec3f {};
struct J3DModel { u32 mPortCullPicture = 0; };
struct ModelManager { J3DModel* mModel; };
struct LiveActor { ModelManager* mModelManager; int mLogicFreeJoints = 0; bool dead = false, hidden = false, clipped = false; };
struct ClippingActorInfo { LiveActor* mActor; TVec3f* _4; float _8 = 100; bool mPortZoomCulled = false; };
struct Judge { bool outside = false; bool isJudgedToClipPictureFrustum(const TVec3f&, float) const { return outside; } };
struct ZoomCullTurn { Judge* judge; u32 next; bool zoomed, cull; };
namespace MR {
bool isDead(const LiveActor* a) { return a->dead; }
bool isHiddenModel(const LiveActor* a) { return a->hidden; }
bool isClipped(const LiveActor* a) { return a->clipped; }
}
static bool logging = true;
namespace port { namespace popin { enum { kZoomCull }; bool LogOn() { return logging; } } }
static int appears, disappears, sZoomKept, sZoomCulled;
static u32 J3DPortPicture() { return 41; }
static void port_popin_event(int, int appear, const LiveActor*, const TVec3f*, float) { (appear ? appears : disappears)++; }
#include "zoom-census.inc"
int main() {
    for (int zoom = 0; zoom < 2; ++zoom) for (int mask = 0; mask < 32; ++mask) {
        const bool clipped = mask & 1, hidden = mask & 2, dead = mask & 4, missing = mask & 8;
        logging = !(mask & 16);
        J3DModel m{42}; ModelManager mm{&m}; LiveActor actor{missing ? nullptr : &mm, 0, dead, hidden};
        TVec3f center; ClippingActorInfo info{&actor, &center, 100, true};
        Judge judge; ZoomCullTurn turn{&judge,42,bool(zoom),true};
        appears = disappears = 0;
        ZoomCullJudge(&info, clipped, turn);
        assert(!info.mPortZoomCulled && m.mPortCullPicture == (missing ? 42u : 0u));
        assert(appears == int(logging && !clipped && !hidden && !dead && !missing));
        judge.outside = true;
        ZoomCullJudge(&info, clipped, turn);
        const bool stamped = zoom && !clipped && !missing;
        assert(info.mPortZoomCulled == stamped);
        assert(disappears == int(logging && stamped && !hidden && !dead));
    }
    J3DModel m{42}; ModelManager mm{&m}; LiveActor clippedActor{&mm,0,false,false,true};
    TVec3f c; ClippingActorInfo info{&clippedActor,&c,100,true};
    logging = true; appears = 0;
    ZoomUncull(&info);
    assert(!info.mPortZoomCulled && !m.mPortCullPicture && !appears);
    std::puts("C184 zoom census: 64 combinations + release outside judging; real draw stamps unchanged, clipped/hidden/missing/dead actors not reported");
}
