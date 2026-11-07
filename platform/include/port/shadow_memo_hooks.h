// shadowmemo2: the write-side hooks of GXV_SHADOWMEMO (port/shadow_memo.h; defined in decomp/src/Game/LiveActor/ShadowController.cpp).
// Every write that can change the answer of a shadow's projection cast reports itself here, while the memo is armed:
//   - port_shadowmemo_part:    a collision part's matrix (mBaseMatrix / mInvBaseMatrix), radius (_D8) or on flag (_CC) changed bits;
//                              its sphere (translation, _D8) and _CC before and after.
//   - port_shadowmemo_zone:    a non-zero zone's culling sphere (mCenter, mRadius) changed bits.
//   - port_shadowmemo_reshape: a zone list changed (a part added or removed), a zone was made, or a part (re)loaded its KCL or
//                              sensor: every memo is dropped.
// Writers (6 Nov): CollisionParts::init / resetAllMtxPrivate / updateMtx / updateBoundingSphereRangePrivate,
// CollisionCategorizedKeeper::addToZone / removeFromZone / addToGlobal / removeFromGlobal / getZone, CollisionZone::calcMinMaxAndRadius,
// WaterBazooka::makeActorDead (its direct _CC write). validate/invalidateCollisionParts change _CC only together with the zone list.
// GXV_SHADOWMEMO_VERIFY=1 audits this list on the console: a walk of all parts finds any change no hook reported.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
extern int port_shadowmemo_armed;  // 1 while memos may be kept (the hooks are free otherwise: one load and a branch)
void port_shadowmemo_part(const void* who, const float* wasTrans, float wasRadius, int wasOn, const float* nowTrans, float nowRadius, int nowOn);
void port_shadowmemo_zone(const void* who, const float* wasCenter, float wasRadius, const float* nowCenter, float nowRadius);
void port_shadowmemo_reshape(void);
#ifdef __cplusplus
}
#endif
