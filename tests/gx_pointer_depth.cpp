// Real original controller, inverse projection and portable SDK matrices.
// Only scene/VR/map queries are doubled, not the depth-to-world math.
#include "Game/Screen/StarPointerController.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static Mtx view={{1,0,0,0},{0,1,0,0},{0,0,1,0}};
static unsigned mapReads,vrReads;
namespace MR {
MtxPtr getStarPointerViewMtx() { return view; }
bool isExistSceneObj(int) { return true; }
const TVec3f getCamPos() { return TVec3f(0,0,0); }
bool isScreen16Per9() { return false; }
s32 getScreenWidth() { return 640; }
s32 getFrameBufferWidth() { return 640; }
bool normalizeOrZero(TVec3f* p) { p->set(0,0,-1); return true; }
bool getFirstPolyOnLineToMap(TVec3f* hit,Triangle*,const TVec3f&,const TVec3f&) {
    ++mapReads; hit->set(7,8,9); return true;
}
bool calcVrPointerAimEnd(TVec3f*,f32*,s32) { return false; }
}
extern "C" int port_vr_pointer_ray(float*,float*) { ++vrReads; return 0; }
extern "C" void port_vr_pointer_reach(float) {}
extern "C" void port_vr_pointer_depth(float) {}

int main() {
    const float projection[]={0,1,0,1,0,-1,-1},viewport[]={0,0,640,480,0,1};
    StarPointerController pointer;
    pointer.initAndSetPort(0);
    for (uint32_t depth:{0u,0xFFFFFFu}) {
        pointer.mInfo.mPos.set(320,240); pointer.mInfo.mZDepth=depth;
        pointer.mInfo.mInScreen=true; pointer.mInfo.mDrawReady=true;
        pointer.storeDataFromCallback(projection,viewport);
#ifdef __vita__
        const float expected=depth ? -1.0f : -0.5f;
        if (pointer.mWorldPos.x!=0 || pointer.mWorldPos.y!=0 ||
            std::fabs(pointer.mWorldPos.z-expected)>0.000001f ||
            std::fabs(pointer.mInfo.mViewDistZ+expected)>0.000001f || mapReads || vrReads) {
            std::fputs("native pointer FAILED: original EFB depth is replaced by desktop raycast\n",stderr);
            return 1;
        }
#else
        if (pointer.mWorldPos.x!=7 || pointer.mWorldPos.y!=8 || pointer.mWorldPos.z!=9 || !mapReads || !vrReads)
            return 1;
#endif
        if (pointer.mInfo.mDrawReady || pointer.mOutScreenTime) return 1;
    }
    delete[] pointer.mPastPosition; delete[] pointer.mNextPastPosition;
    std::puts("Original pointer inverse projection: 2 depth cases; native EFB/desktop diagnostic policies, not physical controls");
}
