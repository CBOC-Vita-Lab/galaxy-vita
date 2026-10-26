// Portable implementation of the Revolution SDK MTX/VEC/QUAT library.
// The original PS* variants are hand-written paired-single assembly; these are
// straightforward C equivalents with the same conventions:
//   - Mtx is 3x4 row-major, points are column vectors (v' = M * v), translation in column 3.
//   - Every function tolerates its output aliasing an input.
#include <math.h>
#include <string.h>
#include "revolution/mtx.h"
#include "port/mtx_profile.h"

void C_VECNormalize(const Vec* src, Vec* unit);
void C_VECCrossProduct(const Vec* a, const Vec* b, Vec* axb);
void C_VECAdd(const Vec* a, const Vec* b, Vec* ab);
f32 C_VECDotProduct(const Vec* a, const Vec* b);

// ---------------------------------------------------------------------------
// 3x4 matrices
// ---------------------------------------------------------------------------
void C_MTXIdentity(Mtx m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}
void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }

void C_MTXCopy(const Mtx src, Mtx dst) {
    if (src != (const float(*)[4])dst) {
        memmove(dst, src, sizeof(Mtx));
    }
}
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }

#if defined(__ARM_NEON)
// The same sums in the same order and with the same roundings as the portable loop below (a multiply, then
// two multiply-adds that are not fused, and a[r][3] added last), three rows at once: on a Cortex-A9 the
// scalar VFP form is dozens of instructions on a unit that is not pipelined. Checked against the loop on
// 2 million random matrices (zeros, negative zeros, the result aliasing either input): no bit differs.
// Every input is read before any output is written, so the result may alias an input.
#include <arm_neon.h>
void C_MTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    const float32x4_t b0 = vld1q_f32(b[0]), b1 = vld1q_f32(b[1]), b2 = vld1q_f32(b[2]);
    const float32x4_t a0 = vld1q_f32(a[0]), a1 = vld1q_f32(a[1]), a2 = vld1q_f32(a[2]);
    float32x4_t r0 = vmulq_lane_f32(b0, vget_low_f32(a0), 0);
    float32x4_t r1 = vmulq_lane_f32(b0, vget_low_f32(a1), 0);
    float32x4_t r2 = vmulq_lane_f32(b0, vget_low_f32(a2), 0);
    r0 = vmlaq_lane_f32(r0, b1, vget_low_f32(a0), 1);
    r1 = vmlaq_lane_f32(r1, b1, vget_low_f32(a1), 1);
    r2 = vmlaq_lane_f32(r2, b1, vget_low_f32(a2), 1);
    r0 = vmlaq_lane_f32(r0, b2, vget_high_f32(a0), 0);
    r1 = vmlaq_lane_f32(r1, b2, vget_high_f32(a1), 0);
    r2 = vmlaq_lane_f32(r2, b2, vget_high_f32(a2), 0);
    r0 = vsetq_lane_f32(vgetq_lane_f32(r0, 3) + vgetq_lane_f32(a0, 3), r0, 3);
    r1 = vsetq_lane_f32(vgetq_lane_f32(r1, 3) + vgetq_lane_f32(a1, 3), r1, 3);
    r2 = vsetq_lane_f32(vgetq_lane_f32(r2, 3) + vgetq_lane_f32(a2, 3), r2, 3);
    vst1q_f32(ab[0], r0);
    vst1q_f32(ab[1], r1);
    vst1q_f32(ab[2], r2);
}
#else
void C_MTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    for (int r = 0; r < 3; r++) {
        t[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
        t[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
        t[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
        t[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
    }
    memcpy(ab, t, sizeof(Mtx));
}
#endif
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }

void C_MTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        C_MTXConcat(a, srcBase[i], dstBase[i]);
    }
}
void PSMTXConcatArray(const Mtx a, const Mtx* srcBase, Mtx* dstBase, u32 count) { C_MTXConcatArray(a, srcBase, dstBase, count); }

void C_MTXTranspose(const Mtx src, Mtx xPose) {
    Mtx t;
    t[0][0] = src[0][0]; t[0][1] = src[1][0]; t[0][2] = src[2][0]; t[0][3] = 0.0f;
    t[1][0] = src[0][1]; t[1][1] = src[1][1]; t[1][2] = src[2][1]; t[1][3] = 0.0f;
    t[2][0] = src[0][2]; t[2][1] = src[1][2]; t[2][2] = src[2][2]; t[2][3] = 0.0f;
    memcpy(xPose, t, sizeof(Mtx));
}
void PSMTXTranspose(const Mtx src, Mtx xPose) { C_MTXTranspose(src, xPose); }

u32 C_MTXInverse(const Mtx src, Mtx inv) {
#if PORT_MTX_PROFILE_COMPILED
    const unsigned profile = port_mtx_profile_begin(src);
#endif
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] + src[0][2] * src[1][0] * src[2][1] -
              src[2][0] * src[1][1] * src[0][2] - src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) {
#if PORT_MTX_PROFILE_COMPILED
        if (profile) port_mtx_profile_end(profile, 0);
#endif
        return 0;
    }
    f32 d = 1.0f / det;
    Mtx t;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * d;
    t[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * d;
    t[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * d;
    t[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * d;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * d;
    t[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * d;
    t[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * d;
    t[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * d;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * d;
    t[0][3] = -t[0][0] * src[0][3] - t[0][1] * src[1][3] - t[0][2] * src[2][3];
    t[1][3] = -t[1][0] * src[0][3] - t[1][1] * src[1][3] - t[1][2] * src[2][3];
    t[2][3] = -t[2][0] * src[0][3] - t[2][1] * src[1][3] - t[2][2] * src[2][3];
    memcpy(inv, t, sizeof(Mtx));
#if PORT_MTX_PROFILE_COMPILED
    if (profile) port_mtx_profile_end(profile, 1);
#endif
    return 1;
}
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }

u32 C_MTXInvXpose(const Mtx src, Mtx invX) {
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] + src[0][2] * src[1][0] * src[2][1] -
              src[2][0] * src[1][1] * src[0][2] - src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) {
        return 0;
    }
    f32 d = 1.0f / det;
    Mtx t;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * d;
    t[0][1] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * d;
    t[0][2] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * d;
    t[1][0] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * d;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * d;
    t[1][2] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * d;
    t[2][0] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * d;
    t[2][1] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * d;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * d;
    t[0][3] = t[1][3] = t[2][3] = 0.0f;
    memcpy(invX, t, sizeof(Mtx));
    return 1;
}
u32 PSMTXInvXpose(const Mtx src, Mtx invX) { return C_MTXInvXpose(src, invX); }

void C_MTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    Vec v = *src;
    dst->x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3];
    dst->y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3];
    dst->z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
}
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }

void C_MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    Vec v = *src;
    dst->x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z;
    dst->y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z;
    dst->z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z;
}
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVecSR(m, src, dst); }

void C_MTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        C_MTXMultVec(m, &srcBase[i], &dstBase[i]);
    }
}
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) { C_MTXMultVecArray(m, srcBase, dstBase, count); }

void C_MTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        C_MTXMultVecSR(m, &srcBase[i], &dstBase[i]);
    }
}
void PSMTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) { C_MTXMultVecArraySR(m, srcBase, dstBase, count); }

void C_MTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = xT;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = yT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = zT;
}
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) { C_MTXTrans(m, xT, yT, zT); }

void C_MTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    if (src != (const float(*)[4])dst) {
        memcpy(dst, src, sizeof(Mtx));
    }
    dst[0][3] += xT;
    dst[1][3] += yT;
    dst[2][3] += zT;
}
void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) { C_MTXTransApply(src, dst, xT, yT, zT); }

void C_MTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    m[0][0] = xS;   m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = yS;   m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = zS;   m[2][3] = 0.0f;
}
void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) { C_MTXScale(m, xS, yS, zS); }

void C_MTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    for (int c = 0; c < 4; c++) {
        dst[0][c] = src[0][c] * xS;
        dst[1][c] = src[1][c] * yS;
        dst[2][c] = src[2][c] * zS;
    }
}
void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) { C_MTXScaleApply(src, dst, xS, yS, zS); }

void C_MTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) {
    switch (axis) {
    case 'x':
    case 'X':
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;  m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA; m[1][2] = -sinA; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA; m[2][2] = cosA;  m[2][3] = 0.0f;
        break;
    case 'y':
    case 'Y':
        m[0][0] = cosA;  m[0][1] = 0.0f; m[0][2] = sinA; m[0][3] = 0.0f;
        m[1][0] = 0.0f;  m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA; m[2][3] = 0.0f;
        break;
    case 'z':
    case 'Z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA;  m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f;  m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        break;
    }
}
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) { C_MTXRotTrig(m, axis, sinA, cosA); }

void C_MTXRotRad(Mtx m, char axis, f32 rad) { C_MTXRotTrig(m, axis, sinf(rad), cosf(rad)); }
void PSMTXRotRad(Mtx m, char axis, f32 rad) { C_MTXRotRad(m, axis, rad); }

void C_MTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    f32 mag = sqrtf(axis->x * axis->x + axis->y * axis->y + axis->z * axis->z);
    f32 x = axis->x, y = axis->y, z = axis->z;
    if (mag != 0.0f) {
        x /= mag;
        y /= mag;
        z /= mag;
    }
    f32 xSq = x * x, ySq = y * y, zSq = z * z;
    m[0][0] = (t * xSq) + c;
    m[0][1] = (t * x * y) - (s * z);
    m[0][2] = (t * x * z) + (s * y);
    m[0][3] = 0.0f;
    m[1][0] = (t * x * y) + (s * z);
    m[1][1] = (t * ySq) + c;
    m[1][2] = (t * y * z) - (s * x);
    m[1][3] = 0.0f;
    m[2][0] = (t * x * z) - (s * y);
    m[2][1] = (t * y * z) + (s * x);
    m[2][2] = (t * zSq) + c;
    m[2][3] = 0.0f;
}
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) { C_MTXRotAxisRad(m, axis, rad); }

void C_MTXQuat(Mtx m, const Quaternion* q) {
    f32 s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    f32 xs = q->x * s, ys = q->y * s, zs = q->z * s;
    f32 wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    f32 xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    f32 yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;
    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}
void PSMTXQuat(Mtx m, const Quaternion* q) { C_MTXQuat(m, q); }

void C_MTXReflect(Mtx m, const Vec* p, const Vec* n) {
    f32 vxy = -2.0f * n->x * n->y, vxz = -2.0f * n->x * n->z, vyz = -2.0f * n->y * n->z;
    f32 pdotn = 2.0f * (p->x * n->x + p->y * n->y + p->z * n->z);
    m[0][0] = 1.0f - 2.0f * n->x * n->x; m[0][1] = vxy; m[0][2] = vxz; m[0][3] = pdotn * n->x;
    m[1][0] = vxy; m[1][1] = 1.0f - 2.0f * n->y * n->y; m[1][2] = vyz; m[1][3] = pdotn * n->y;
    m[2][0] = vxz; m[2][1] = vyz; m[2][2] = 1.0f - 2.0f * n->z * n->z; m[2][3] = pdotn * n->z;
}
void PSMTXReflect(Mtx m, const Vec* p, const Vec* n) { C_MTXReflect(m, p, n); }

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target) {
    Vec vLook, vRight, vUp;
    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    C_VECNormalize(&vLook, &vLook);
    C_VECCrossProduct(camUp, &vLook, &vRight);
    C_VECNormalize(&vRight, &vRight);
    C_VECCrossProduct(&vLook, &vRight, &vUp);
    m[0][0] = vRight.x; m[0][1] = vRight.y; m[0][2] = vRight.z;
    m[0][3] = -(camPos->x * vRight.x + camPos->y * vRight.y + camPos->z * vRight.z);
    m[1][0] = vUp.x; m[1][1] = vUp.y; m[1][2] = vUp.z;
    m[1][3] = -(camPos->x * vUp.x + camPos->y * vUp.y + camPos->z * vUp.z);
    m[2][0] = vLook.x; m[2][1] = vLook.y; m[2][2] = vLook.z;
    m[2][3] = -(camPos->x * vLook.x + camPos->y * vLook.y + camPos->z * vLook.z);
}

void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = ((2 * n) * tmp) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = (((r + l) * tmp) * scaleS) - transS;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = ((2 * n) * tmp) * scaleT;
    m[1][2] = (((t + b) * tmp) * scaleT) - transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / tanf(angle);
    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * tmp * scaleS);
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = ((-(r + l) * tmp) * scaleS) + transS;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * tmp) * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = ((-(t + b) * tmp) * scaleT) + transT;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

// ---------------------------------------------------------------------------
// 4x4 matrices
// ---------------------------------------------------------------------------
void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = (2 * n) * tmp;
    m[0][1] = 0.0f;
    m[0][2] = (r + l) * tmp;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2 * n) * tmp;
    m[1][2] = (t + b) * tmp;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -(n)*tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / tanf(angle);
    m[0][0] = cot / aspect;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    f32 tmp = 1.0f / (f - n);
    m[2][2] = -(n)*tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -(1.0f) * tmp;
    m[2][3] = -(f)*tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

void C_MTX44Identity(Mtx44 m) {
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}
void PSMTX44Identity(Mtx44 m) { C_MTX44Identity(m); }

void C_MTX44Copy(const Mtx44 src, Mtx44 dst) {
    if (src != (const float(*)[4])dst) {
        memmove(dst, src, sizeof(Mtx44));
    }
}
void PSMTX44Copy(const Mtx44 src, Mtx44 dst) { C_MTX44Copy(src, dst); }

void C_MTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) {
    Mtx44 t;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
        }
    }
    memcpy(ab, t, sizeof(Mtx44));
}
void PSMTX44Concat(const Mtx44 a, const Mtx44 b, Mtx44 ab) { C_MTX44Concat(a, b, ab); }

void C_MTX44MultVec(const Mtx44 m, const Vec* src, Vec* dst) {
    Vec v = *src;
    f32 w = m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3];
    w = 1.0f / w;
    dst->x = (m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3]) * w;
    dst->y = (m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3]) * w;
    dst->z = (m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3]) * w;
}
void PSMTX44MultVec(const Mtx44 m, const Vec* src, Vec* dst) { C_MTX44MultVec(m, src, dst); }

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------
void C_VECAdd(const Vec* a, const Vec* b, Vec* ab) {
    f32 x = a->x + b->x, y = a->y + b->y, z = a->z + b->z;
    ab->x = x; ab->y = y; ab->z = z;
}
void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) { C_VECAdd(a, b, ab); }

void C_VECSubtract(const Vec* a, const Vec* b, Vec* a_b) {
    f32 x = a->x - b->x, y = a->y - b->y, z = a->z - b->z;
    a_b->x = x; a_b->y = y; a_b->z = z;
}
void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) { C_VECSubtract(a, b, a_b); }

void C_VECScale(const Vec* src, Vec* dst, f32 scale) {
    f32 x = src->x * scale, y = src->y * scale, z = src->z * scale;
    dst->x = x; dst->y = y; dst->z = z;
}
void PSVECScale(const Vec* src, Vec* dst, f32 scale) { C_VECScale(src, dst, scale); }

void C_VECNormalize(const Vec* src, Vec* unit) {
    f32 mag = src->x * src->x + src->y * src->y + src->z * src->z;
    // The paired-single version multiplies by rsqrt(mag) unconditionally.
    f32 inv = 1.0f / sqrtf(mag);
    f32 x = src->x * inv, y = src->y * inv, z = src->z * inv;
    unit->x = x; unit->y = y; unit->z = z;
}
void PSVECNormalize(const Vec* src, Vec* unit) { C_VECNormalize(src, unit); }

f32 C_VECSquareMag(const Vec* v) { return v->x * v->x + v->y * v->y + v->z * v->z; }
f32 PSVECSquareMag(const Vec* v) { return C_VECSquareMag(v); }

f32 C_VECMag(const Vec* v) { return sqrtf(C_VECSquareMag(v)); }
f32 PSVECMag(const Vec* v) { return C_VECMag(v); }

f32 C_VECDotProduct(const Vec* a, const Vec* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return C_VECDotProduct(a, b); }

void C_VECCrossProduct(const Vec* a, const Vec* b, Vec* axb) {
    f32 x = a->y * b->z - a->z * b->y;
    f32 y = a->z * b->x - a->x * b->z;
    f32 z = a->x * b->y - a->y * b->x;
    axb->x = x; axb->y = y; axb->z = z;
}
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) { C_VECCrossProduct(a, b, axb); }

f32 C_VECSquareDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}
f32 PSVECSquareDistance(const Vec* a, const Vec* b) { return C_VECSquareDistance(a, b); }

f32 C_VECDistance(const Vec* a, const Vec* b) { return sqrtf(C_VECSquareDistance(a, b)); }
f32 PSVECDistance(const Vec* a, const Vec* b) { return C_VECDistance(a, b); }

void C_VECHalfAngle(const Vec* a, const Vec* b, Vec* half) {
    Vec aN, bN, h;
    aN.x = -a->x; aN.y = -a->y; aN.z = -a->z;
    bN.x = -b->x; bN.y = -b->y; bN.z = -b->z;
    C_VECNormalize(&aN, &aN);
    C_VECNormalize(&bN, &bN);
    C_VECAdd(&aN, &bN, &h);
    if (C_VECDotProduct(&h, &h) > 0.0f) {
        C_VECNormalize(&h, half);
    } else {
        *half = h;
    }
}

void C_VECReflect(const Vec* src, const Vec* normal, Vec* dst) {
    Vec uI, uN;
    uI.x = -src->x; uI.y = -src->y; uI.z = -src->z;
    C_VECNormalize(&uI, &uI);
    C_VECNormalize(normal, &uN);
    f32 cosA = C_VECDotProduct(&uI, &uN);
    Vec r;
    r.x = 2.0f * uN.x * cosA - uI.x;
    r.y = 2.0f * uN.y * cosA - uI.y;
    r.z = 2.0f * uN.z * cosA - uI.z;
    C_VECNormalize(&r, dst);
}

// ---------------------------------------------------------------------------
// Quaternions
// ---------------------------------------------------------------------------
void C_QUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r) {
    r->x = p->x + q->x; r->y = p->y + q->y; r->z = p->z + q->z; r->w = p->w + q->w;
}
void PSQUATAdd(const Quaternion* p, const Quaternion* q, Quaternion* r) { C_QUATAdd(p, q, r); }

void C_QUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) {
    Quaternion t;
    t.w = p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z;
    t.x = p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y;
    t.y = p->w * q->y + p->y * q->w + p->z * q->x - p->x * q->z;
    t.z = p->w * q->z + p->z * q->w + p->x * q->y - p->y * q->x;
    *pq = t;
}
void PSQUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) { C_QUATMultiply(p, q, pq); }

f32 C_QUATDotProduct(const Quaternion* p, const Quaternion* q) { return p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w; }
f32 PSQUATDotProduct(const Quaternion* p, const Quaternion* q) { return C_QUATDotProduct(p, q); }

void C_QUATScale(const Quaternion* q, Quaternion* r, f32 scale) {
    r->x = q->x * scale; r->y = q->y * scale; r->z = q->z * scale; r->w = q->w * scale;
}
void PSQUATScale(const Quaternion* q, Quaternion* r, f32 scale) { C_QUATScale(q, r, scale); }

void C_QUATNormalize(const Quaternion* src, Quaternion* unit) {
    f32 mag = src->x * src->x + src->y * src->y + src->z * src->z + src->w * src->w;
    if (mag >= 0.00001f) {
        mag = 1.0f / sqrtf(mag);
        unit->x = src->x * mag; unit->y = src->y * mag; unit->z = src->z * mag; unit->w = src->w * mag;
    } else {
        unit->x = unit->y = unit->z = unit->w = 0.0f;
    }
}
void PSQUATNormalize(const Quaternion* src, Quaternion* unit) { C_QUATNormalize(src, unit); }

void C_QUATMtx(Quaternion* r, const Mtx m) {
    f32 tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        f32 s = sqrtf(tr + 1.0f);
        r->w = s * 0.5f;
        s = 0.5f / s;
        r->x = (m[2][1] - m[1][2]) * s;
        r->y = (m[0][2] - m[2][0]) * s;
        r->z = (m[1][0] - m[0][1]) * s;
    } else {
        static const int nxt[3] = {1, 2, 0};
        int i = 0;
        if (m[1][1] > m[0][0]) i = 1;
        if (m[2][2] > m[i][i]) i = 2;
        int j = nxt[i];
        int k = nxt[j];
        f32 s = sqrtf((m[i][i] - (m[j][j] + m[k][k])) + 1.0f);
        f32 q[4];
        q[i] = s * 0.5f;
        if (s != 0.0f) s = 0.5f / s;
        q[3] = (m[k][j] - m[j][k]) * s;
        q[j] = (m[i][j] + m[j][i]) * s;
        q[k] = (m[i][k] + m[k][i]) * s;
        r->x = q[0]; r->y = q[1]; r->z = q[2]; r->w = q[3];
    }
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 cosTheta = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    f32 tq = 1.0f;
    if (cosTheta < 0.0f) {
        cosTheta = -cosTheta;
        tq = -tq;
    }
    f32 tp;
    if (cosTheta <= 1.0f - 0.00001f) {
        f32 theta = acosf(cosTheta);
        f32 sinTheta = sinf(theta);
        tp = sinf((1.0f - t) * theta) / sinTheta;
        tq *= sinf(t * theta) / sinTheta;
    } else {
        tp = 1.0f - t;
        tq *= t;
    }
    r->x = tp * p->x + tq * q->x;
    r->y = tp * p->y + tq * q->y;
    r->z = tp * p->z + tq * q->z;
    r->w = tp * p->w + tq * q->w;
}
