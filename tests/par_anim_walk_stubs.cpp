// tests/par_anim_walk.cpp's stand-ins (built like the game units, tools/test_par_anim_walk.sh): j3dSys and the J3DSys statics the
// joint walk uses (J3DSys.cpp itself sets up GX tables in its constructor), and the game utilities XanimeCore.cpp links against on
// branches the test's models never take (they abort if reached). Everything else unresolved belongs to code the test never runs
// (materials, draw buffers, GX) and is left to the linker (--unresolved-symbols=ignore-all).
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JGeometry/TVec.hpp>
#include <cstdio>
#include <cstdlib>

J3DSys::J3DSys() {
}
J3DSys j3dSys;
Mtx J3DSys::mCurrentMtx;
Vec J3DSys::mCurrentS;
Vec J3DSys::mParentS;
J3DTexCoordScaleInfo J3DSys::sTexCoordScaleTable[8];

namespace MR {
static void Unexpected(const char* what) {
    fprintf(stderr, "par_anim_walk: unexpected call of %s (a branch the qualifying models never take)\n", what);
    abort();
}
#ifndef PAR_ANIM_REAL_MATHUTIL  // (GXV_PAR_ANIM_BLEND's runs link the game's own MathUtil.cpp: calcSingle's blend reads MR::vecBlend)
void vecBlend(const TVec3f&, const TVec3f&, TVec3f*, f32) {
    Unexpected("MR::vecBlend");
}
#endif
#ifndef PAR_ANIM_REAL_MTXUTIL  // (GXV_PAR_ANIM_CB's runs link the game's own MtxUtil.cpp: MR::preScaleMtx)
void extractMtxTrans(MtxPtr, TVec3f*) {
    Unexpected("MR::extractMtxTrans");
}
void setMtxTrans(MtxPtr, f32, f32, f32) {
    Unexpected("MR::setMtxTrans");
}
#endif
}  // namespace MR
