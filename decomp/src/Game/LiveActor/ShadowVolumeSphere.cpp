#include "Game/LiveActor/ShadowVolumeSphere.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include <revolution/gx.h>
#ifdef TARGET_PC
#include "Game/LiveActor/ShadowController.hpp"
#include "Game/Util/MathUtil.hpp"
#include <cmath>
extern "C" int port_boss_pshadow_on(void);        // (decomp/src/Game/Scene/GameScene.cpp, boss23)
extern "C" void port_boss_pshadow_count(int disc);
#endif

namespace {
    const f32 sModelScale = 100.0f;
}

void ShadowVolumeSphere_FORCE_MATCH_SDATA2() {
    0.0f;
}

ShadowVolumeSphere::~ShadowVolumeSphere() {
}

ShadowVolumeSphere::ShadowVolumeSphere() : ShadowVolumeModel("\x89\x65\x95\x60\x89\xe6[\x83\x7b\x83\x8a\x83\x85\x81\x5b\x83\x80\x8b\x85]"), mRadius(100.0f) {
    initVolumeModel("ShadowVolumeSphere");
}

void ShadowVolumeSphere::setRadius(f32 radius) {
    mRadius = radius;
}

bool ShadowVolumeSphere::isDraw() const {
    ShadowController* controller = getController();

    return controller->isProjected() && controller->isDraw();
}

void ShadowVolumeSphere::loadModelDrawMtx() const {
    ShadowController* controller = getController();
    f32 radius = mRadius / ::sModelScale;
    if (controller->isFollowHostScale()) {
        radius *= controller->getHost()->mScale.x;
    }

    TPos3f mtx;
    mtx.identity();
    mtx.mMtx[0][0] = radius;
    mtx.mMtx[0][1] = 0.0f;
    mtx.mMtx[0][2] = 0.0f;
    mtx.mMtx[1][0] = 0.0f;
    mtx.mMtx[1][1] = radius;
    mtx.mMtx[1][2] = 0.0f;
    mtx.mMtx[2][0] = 0.0f;
    mtx.mMtx[2][1] = 0.0f;
    mtx.mMtx[2][2] = radius;
    TVec3f position;
    controller->getProjectionPos(&position);
    mtx.setTrans(position);

    PSMTXConcat(MR::getCameraViewMtx(), mtx, mtx);
    GXLoadPosMtxImm(mtx, 0);
}

#ifdef TARGET_PC
// ---- boss23: PETARI_BOSS_PSHADOW, a projectile's sphere shadow volume drawn as one flat disc (a MINOR CHANGE, see below) ----
// The sphere volume (ShadowVolumeDrawer::draw) is two passes of a sphere mesh of radius r centred on the ground point under the
// projectile (the projection position), against the scene's depth with GEQUAL (a face counts where it lies behind the scene surface):
// GX_CULL_FRONT, the far shell, adds 4 to the EFB alpha; GX_CULL_BACK, the near shell, subtracts 4. Net: +4 on every visible pixel
// of scene surface INSIDE the sphere (far shell behind it, near shell in front), 0 elsewhere (a sphere wholly in front of the scene
// counts nothing, one wholly behind it counts +4 - 4); the opaque scene's alpha is 0 (GXSetDstAlpha(1, 0) before the pass), so the
// mark can only be the +4. The alpha-shadow pass then darkens the marked pixels.
// The disc: one fan of radius r on the ground plane at the projection position (lifted 2 units along the ground normal), drawn with
// LEQUAL against the scene's depth and the same +4 (additive, the same TEV colour, colour writes off). On flat ground the marked
// pixels are those of the sphere: the ground inside a sphere centred on the ground is that disc. Differences: on a curved planet of
// radius R the disc's rim sits up to r^2 / 2R above or below the surface (Bowser's fire, r 200 on a planet of R ~2000: 10 units, ~1-3
// screen pixels at the rim); scene surfaces other than the ground inside the sphere (a step, a wall, the boss's foot within r of the
// point) are not darkened. Cost: one draw of 18 triangles instead of two of the sphere mesh, and the pixels of the disc as seen
// (an ellipse: r^2 pi sin(elevation)) instead of the sphere's whole outline twice.
void ShadowVolumeSphere::draw() const {
    if (!mPortProjectile) {
        ShadowVolumeDrawer::draw();
        return;
    }
    if (!port_boss_pshadow_on()) {
        if (isDraw()) {
            port_boss_pshadow_count(0);
        }
        ShadowVolumeDrawer::draw();
        return;
    }
    if (!isDraw()) {
        return;
    }
    port_boss_pshadow_count(1);
    portDrawDisc();
}

void ShadowVolumeSphere::portDrawDisc() const {
    ShadowController* controller = getController();
    f32 radius = mRadius;
    if (controller->isFollowHostScale()) {
        radius *= controller->getHost()->mScale.x;
    }
    TVec3f center, normal;
    controller->getProjectionPos(&center);
    controller->getProjectionNormal(&normal);
    f32 len = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len < 0.001f) {
        return;
    }
    normal.x /= len;
    normal.y /= len;
    normal.z /= len;
    center.x += normal.x * 2.0f;
    center.y += normal.y * 2.0f;
    center.z += normal.z * 2.0f;
    // two axes across the normal
    TVec3f a = (normal.x > 0.9f || normal.x < -0.9f) ? TVec3f(0.0f, 1.0f, 0.0f) : TVec3f(1.0f, 0.0f, 0.0f);
    TVec3f u(normal.y * a.z - normal.z * a.y, normal.z * a.x - normal.x * a.z, normal.x * a.y - normal.y * a.x);
    len = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
    u.x /= len;
    u.y /= len;
    u.z /= len;
    TVec3f v(normal.y * u.z - normal.z * u.y, normal.z * u.x - normal.x * u.z, normal.x * u.y - normal.y * u.x);

    // the volume pass's own state (ShadowVolumeDrawer::draw), one pass: add where the disc is seen (the far shell's +4)
    GXLoadPosMtxImm(MR::getCameraViewMtx(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetTevColor(GX_TEVREG0, GXColor{0xC0, 0xC0, 0, 4});  // (ShadowVolumeDrawer.cpp sShapeColor)
    GXSetColorUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    const s32 segments = 18;
    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, segments + 2);
    GXPosition3f32(center.x, center.y, center.z);
    for (s32 i = 0; i <= segments; i++) {
        const f32 ang = (6.2831855f * i) / segments;
        const f32 c = cosf(ang) * radius, s = sinf(ang) * radius;
        GXPosition3f32(center.x + u.x * c + v.x * s, center.y + u.y * c + v.y * s, center.z + u.z * c + v.z * s);
    }
    GXEnd();
    // back to what the pass's set-up and the sphere path leave
    GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);  // (MR::setupShadowVolumeDraw)
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);  // (as the sphere's last pass leaves it)
    GXSetCullMode(GX_CULL_NONE);
    GXSetColorUpdate(GX_TRUE);
    GXSetDstAlpha(GX_TRUE, 0);
}
#endif
