#ifndef MTX_H
#define MTX_H

#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    f32 x, y, z;
} Vec, *VecPtr, Point3d;

typedef struct {
    s16 x;
    s16 y;
    s16 z;
} S16Vec, *S16VecPtr;

typedef struct {
    f32 x, y, z, w;
} Quaternion, *QuaternionPtr;

typedef f32 Mtx23[2][3];
typedef f32 (*Mtx23P)[3];

typedef f32 Mtx[3][4];
typedef f32 (*MtxPtr)[4];

typedef f32 Mtx44[4][4];
typedef f32 (*Mtx44Ptr)[4];

typedef f32 Mtx33[3][3];
typedef f32 (*Mtx3P)[3];

#define MTXDegToRad(a) ((a)*0.01745329252f)
#define MTXRadToDeg(a) ((a)*57.29577951f)

#define MTXRowCol(m, r, c) ((m)[(r)][(c)])

/* C Matrix */
void C_MTXIdentity(Mtx);
void C_MTXCopy(const Mtx, Mtx);
void C_MTXOrtho(Mtx44, f32, f32, f32, f32, f32, f32);
void C_MTXPerspective(Mtx44, f32, f32, f32, f32);

/* Paired Single Matrix */
void PSMTXIdentity(Mtx);
void PSMTXCopy(const Mtx, Mtx);
void PSMTXConcat(const Mtx, const Mtx, Mtx);
void PSMTXMultVecSR(const Mtx, const Vec*, Vec*);
void PSMTXMultVecArraySR(const Mtx, const Vec*, Vec*, u32);

u32 PSMTXInverse(const Mtx, Mtx);

void PSMTXMultVec(const Mtx, const Vec*, Vec*);

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT);
void PSMTXScaleApply(const Mtx, Mtx, f32, f32, f32);
void PSMTXScale(Mtx, f32, f32, f32);
void PSMTXRotRad(Mtx m, char axis, f32 rad) ATTRIBUTE_WEAK;
void PSMTXRotAxisRad(Mtx dst, const Vec*, f32);
void PSMTXRotTrig(Mtx, char, f32, f32);

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT);
void PSMTX44Identity(Mtx44 m);
void PSMTX44Copy(const Mtx44 m, Mtx44 dst);

/* Paired Single Quaternion */
void PSQUATMultiply(const Quaternion*, const Quaternion*, Quaternion*);
void PSMTXQuat(Mtx dst, const Quaternion* quat);
f32 PSQUATDotProduct(const Quaternion*, const Quaternion*);

/* C Quaternion */
void C_QUATMtx(Quaternion* r, const Mtx m);
void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t);

/* C Vector */

f32 C_VECMag(const Vec*);

/* Paired Single Vector */
#if defined(TARGET_PC) && !defined(PORT_MTX_DEFINE)
// (port) The smallest helpers inline: their definitions (platform/src/sdk/mtx.c, PORT_MTX_DEFINE) are outside the game's
// link-time optimisation, where a call costs more than the arithmetic. The same operations in the same order.
static inline void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) {
    f32 x = a->x + b->x, y = a->y + b->y, z = a->z + b->z;
    ab->x = x; ab->y = y; ab->z = z;
}
static inline void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) {
    f32 x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
    a_b->x = x; a_b->y = y; a_b->z = z;
}
static inline void PSVECScale(const Vec* src, Vec* dst, f32 scale) {
    f32 x = src->x * scale, y = src->y * scale, z = src->z * scale;
    dst->x = x; dst->y = y; dst->z = z;
}
static inline f32 PSVECDotProduct(const Vec* a, const Vec* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
static inline void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) {
    f32 x = a->y * b->z - a->z * b->y;
    f32 y = a->z * b->x - a->x * b->z;
    f32 z = a->x * b->y - a->y * b->x;
    axb->x = x; axb->y = y; axb->z = z;
}
#else
void PSVECAdd(const Vec*, const Vec*, Vec*);
void PSVECSubtract(const Vec*, const Vec*, Vec*);
void PSVECScale(const Vec*, Vec*, f32);
f32 PSVECDotProduct(const Vec*, const Vec*);
void PSVECCrossProduct(const Vec*, const Vec*, Vec*);
#endif
void PSVECNormalize(const Vec*, Vec*);

f32 PSVECMag(const Vec*);
f32 PSVECDistance(const Vec*, const Vec*);

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT);
void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT);

/* SMG1 uses the paired single versions of matrix / vector operations */
#define VECNormalize PSVECNormalize
#define VECMag PSVECMag

#ifdef __cplusplus
}
#endif

#endif  // MTX_H
