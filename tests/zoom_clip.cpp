// zoomclip23 (GXV_ZOOM_LOGIC_VIEW): host check of the game's object clipping under the handheld zoom. Links the REAL
// decomp/src/Game/LiveActor/ClippingJudge.cpp and the REAL MR::getFovyForLogic (cut out of decomp/src/Game/Util/CameraUtil.cpp by
// tools/test_zoom_clip.sh) against a stub camera: the camera context holds the picture's field of view (the game's, narrowed by the
// zoom as CameraContext.cpp's zoomedFovy does: tan(fovy/2) / Z) and the game's own (mPortFovyGame).
// Random cameras, fields of view (30..60), zooms (1.0..2.0) and spheres; each sphere is sampled (centre, 26 surface directions, 64
// interior points) against the two pictures (GX view space: d = -z, |x| <= d tan(f/2) aspect, |y| <= d tan(f/2), 500 <= d <= far).
//  1. logic (ClippingJudge::isJudgedToClipFrustum, every far level): a sphere with a point in the ORIGINAL picture is never clipped.
//     (2.2.1 failed this: its frustum was the zoomed one.)
//  2. picture frustum (isJudgedToClipPictureFrustum, GXV_ZOOM_DRAWCULL / GXV_ANIM_LOD_OFFSCREEN): a sphere with a point in the ZOOMED
//     picture is never called outside; and it is 2.2.1's clipping frustum, plane for plane (calcViewingVolumeFovy with the picture's fovy).
//  3. some spheres are in the original picture and wholly outside the zoomed one (kept running, not drawn): the case of the report.
//  ZC_MODE=old (GXV_ZOOM_LOGIC_VIEW=0 set by the script): the logic frustum is 2.2.1's again (the zoomed one), plane for plane.
//  ZC_NEG=1: check 1 is made against a frustum of the picture's fovy (2.2.1): it must FAIL.
#include "Game/Camera/CameraContext.hpp"
#include "Game/LiveActor/ClippingDirector.hpp"
#include "Game/LiveActor/ClippingJudge.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "port/hot_knob.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

// ---- stubs of what ClippingJudge.cpp calls
NameObj::NameObj(const char* pName) : mName(pName), mFlag(0), mExecutorIdx(-1) {}
NameObj::~NameObj() {}
void NameObj::init(const JMapInfoIter&) {}
void NameObj::initAfterPlacement() {}
void NameObj::movement() {}
void NameObj::draw() const {}
void NameObj::calcAnim() {}
void NameObj::calcViewAndEntry() {}
int port_vr_cull_view(float*, float*, float*) { return 0; }
extern "C" float port_popin_clip_pad(float) { return 1.0f; }  // (popin25, GXV_POPIN_LEAD: the pad of a turning camera; none here)
extern "C" float port_hrw_clip_pad() { return 1.0f; }  // This fixture exercises the original, unwarped camera.
// (the port's C versions: platform/src/sdk/mtx.c, decomp/src/JSystem/JMath/JMath.cpp)
extern "C" f32 PSVECMag(const Vec* v) { return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }
extern "C" void PSVECNormalize(const Vec* src, Vec* unit) {
    const f32 inv = 1.0f / sqrtf(src->x * src->x + src->y * src->y + src->z * src->z);
    unit->x = src->x * inv; unit->y = src->y * inv; unit->z = src->z * inv;
}
void JMAVECScaleAdd(const Vec* vec1, const Vec* vec2, Vec* dst, f32 scale) {
    const f32 x = vec1->x * scale + vec2->x, y = vec1->y * scale + vec2->y, z = vec1->z * scale + vec2->z;
    dst->x = x; dst->y = y; dst->z = z;
}

alignas(CameraContext) static unsigned char sCtxStore[sizeof(CameraContext)];
static CameraContext* sCtx = reinterpret_cast< CameraContext* >(sCtxStore);
static TPos3f sView;
static const float kAspect = 16.0f / 9.0f, kFar = 800000.0f;
static CameraContext* getCameraContext() { return sCtx; }
namespace MR {
f32 getFovy() { return sCtx->mFovy; }
f32 getAspect() { return kAspect; }
f32 getFarZ() { return kFar; }
const TPos3f& getCameraViewMtx() { return sView; }
bool isSubjectiveCameraOnForObjClipping() { return false; }
ClippingDirector* getClippingDirector() { return nullptr; }
#include "zoom_clip_logic.inc"  // (the real getFovyForLogic / isPictureZoomedIn, cut out of CameraUtil.cpp)
}  // namespace MR

// ---- the test
static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }
static void lookAt(float m[3][4], const float eye[3], const float at[3], const float up0[3]) {
    float f[3] = {eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]};
    float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++) f[i] /= l;
    float x[3] = {up0[1] * f[2] - up0[2] * f[1], up0[2] * f[0] - up0[0] * f[2], up0[0] * f[1] - up0[1] * f[0]};
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    float y[3] = {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
    const float* rows[3] = {x, y, f};
    for (int r = 0; r < 3; r++) {
        m[r][0] = rows[r][0]; m[r][1] = rows[r][1]; m[r][2] = rows[r][2];
        m[r][3] = -(rows[r][0] * eye[0] + rows[r][1] * eye[1] + rows[r][2] * eye[2]);
    }
}
// a point well inside the picture of field of view fovyDeg (a margin of 0.5 % so float noise at the very edge does not count)
static bool inPicture(const float m[3][4], float fovyDeg, float farD, const float p[3]) {
    const float t = tanf(fovyDeg * 3.14159265f / 360.0f) * 0.995f;
    const float vx = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
    const float vy = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
    const float vz = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
    const float d = -vz;
    return d > 505.0f && d < farD * 0.995f && fabsf(vx) <= d * t * kAspect && fabsf(vy) <= d * t;
}
static bool anyIn(const float m[3][4], float fovyDeg, float farD, const float c[3], float r) {
    float p[3] = {c[0], c[1], c[2]};
    if (inPicture(m, fovyDeg, farD, p)) return true;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                if (!dx && !dy && !dz) continue;
                const float l = sqrtf((float)(dx * dx + dy * dy + dz * dz));
                p[0] = c[0] + r * dx / l; p[1] = c[1] + r * dy / l; p[2] = c[2] + r * dz / l;
                if (inPicture(m, fovyDeg, farD, p)) return true;
            }
    for (int k = 0; k < 64; k++) {
        float u[3], l2;
        do { u[0] = frand(-1, 1); u[1] = frand(-1, 1); u[2] = frand(-1, 1); l2 = u[0] * u[0] + u[1] * u[1] + u[2] * u[2]; } while (l2 > 1.0f || l2 < 1e-6f);
        p[0] = c[0] + r * u[0]; p[1] = c[1] + r * u[1]; p[2] = c[2] + r * u[2];
        if (inPicture(m, fovyDeg, farD, p)) return true;
    }
    return false;
}
static bool sameHex(const THex3f& a, const THex3f& b) { return memcmp(&a, &b, sizeof a) == 0; }

int main() {
    const char* mode = getenv("ZC_MODE");
    const bool oldMode = mode && !strcmp(mode, "old");
    const bool neg = getenv("ZC_NEG") && *getenv("ZC_NEG") == '1';
    srand(12345);
    ClippingJudge judge("judge");
    unsigned long spheres = 0, logicBad = 0, pictureBad = 0, keptRunning = 0, planesBad = 0, logicChecked = 0, zoomedCams = 0;
    for (int cam = 0; cam < 3000; cam++) {
        const float gameFovy = frand(30.0f, 60.0f);
        const float zoom = (cam % 7 == 0) ? 1.0f : frand(1.05f, 2.0f);
        const float picture = (float)(2.0 * atan(tan(gameFovy * (M_PI / 360.0)) / zoom) * (180.0 / M_PI));
        sCtx->mFovy = zoom > 1.0f ? picture : gameFovy;
        sCtx->mPortFovyGame = gameFovy;
        const float eye[3] = {frand(-20000, 20000), frand(-20000, 20000), frand(-20000, 20000)};
        const float at[3] = {eye[0] + frand(-1, 1), eye[1] + frand(-1, 1), eye[2] + frand(-1, 1)};
        const float up[3] = {frand(-1, 1), frand(-1, 1), frand(-1, 1)};
        float m[3][4];
        lookAt(m, eye, at, up);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++) sView.mMtx[r][k] = m[r][k];
        judge.movement();
        const bool zoomed = zoom > 1.0f;
        zoomedCams += zoomed;
        // the frustums, plane for plane
        THex3f expectLogic, expectPicture;
        judge.calcViewingVolumeFovy(&expectLogic, kFar, oldMode ? sCtx->mFovy : gameFovy);
        judge.calcViewingVolumeFovy(&expectPicture, kFar, sCtx->mFovy);
        if (!sameHex(judge.mFrustum, expectLogic)) ++planesBad;
        if (judge.mPortPictureZoomed != (zoomed && !oldMode)) ++planesBad;
        if (judge.mPortPictureZoomed && !sameHex(judge.mPortPictureFrustum, expectPicture)) ++planesBad;
        if (neg) {
            judge.mFrustum = expectPicture;  // (2.2.1's logic frustum)
            for (int lv = 1; lv < 8; lv++) judge.calcViewingVolumeFovy(&judge.mClipFrustums[lv], judge.mClipDistances[lv], sCtx->mFovy);
        }
        for (int s = 0; s < 40; s++) {
            // a sphere near the picture: a direction within ~1.6x the original half-angle, at 600..60000
            const float t = tanf(gameFovy * 3.14159265f / 360.0f);
            const float d = frand(600.0f, 60000.0f);
            const float vx = frand(-1.6f, 1.6f) * t * kAspect * d, vy = frand(-1.6f, 1.6f) * t * d, vz = -d;
            float c[3];
            for (int i = 0; i < 3; i++) c[i] = m[0][i] * (vx - m[0][3]) + m[1][i] * (vy - m[1][3]) + m[2][i] * (vz - m[2][3]);
            const float r = frand(10.0f, 0.05f * d);
            const TVec3f pos(c[0], c[1], c[2]);
            ++spheres;
            // 1. logic, every far level whose distance covers the sphere
            for (int lv = 0; lv < 8; lv++) {
                const float farD = lv == 0 ? kFar : judge.mClipDistances[lv];
                if (lv != 0 && farD <= 0.0f) continue;
                if (anyIn(m, oldMode ? sCtx->mFovy : gameFovy, farD, c, r)) {
                    ++logicChecked;
                    if (judge.isJudgedToClipFrustum(pos, r, lv)) ++logicBad;
                }
            }
            // 2. picture
            const bool inZoomed = anyIn(m, sCtx->mFovy, kFar, c, r);
            if (inZoomed && judge.isJudgedToClipPictureFrustum(pos, r)) ++pictureBad;
            // 3. the report's case
            if (!judge.isJudgedToClipFrustum(pos, r, 0) && judge.isJudgedToClipPictureFrustum(pos, r)) ++keptRunning;
        }
    }
    printf("zoom_clip (%s%s): %lu cameras (%lu zoomed), %lu spheres, %lu logic checks: logic clipped a sphere of the original picture %lu, "
           "picture frustum called a visible sphere outside %lu, frustum mismatches %lu, kept running and not drawn %lu\n",
           oldMode ? "GXV_ZOOM_LOGIC_VIEW=0" : "GXV_ZOOM_LOGIC_VIEW=1", neg ? ", NEGATIVE CONTROL" : "", 3000ul, zoomedCams, spheres,
           logicChecked, logicBad, pictureBad, planesBad, keptRunning);
    bool ok = logicBad == 0 && pictureBad == 0 && planesBad == 0 && logicChecked > 10000;
    if (!oldMode && !neg) ok = ok && keptRunning > 1000;
    if (oldMode) ok = ok && keptRunning == 0;
    puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
