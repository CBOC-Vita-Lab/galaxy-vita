// GXV_J3D_NEON host proof (tools/test_j3d_envelope_neon.sh): the NEON weight-envelope function of J3DMtxBuffer.cpp, its body
// extracted from the production file, against the portable loop it replaces (mtx.c's concat order, then acc += prod * w), on
// random skins (zeros and negative zeros included). On x86 the NEON intrinsics are modelled by tests/neon_shim_f32.h (each
// multiply and add rounded separately, as VMUL/VMLA do on the Cortex-A9); NEON's flush of subnormals is not modelled.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#else
#include "neon_shim_f32.h"
#endif
typedef float f32; typedef uint8_t u8; typedef uint16_t u16; typedef f32 Mtx[3][4];
struct J3DJointTree {
    u16 mWEvlpMtxNum; u8* mMixNum; u16* mIdx; f32* mW; Mtx* mInv;
    u16* getWEvlpMixMtxIndex() const { return mIdx; } f32* getWEvlpMixWeight() const { return mW; }
    u16 getWEvlpMtxNum() const { return mWEvlpMtxNum; } u8 getWEvlpMixMtxNum(u16 i) const { return mMixNum[i]; }
    Mtx& getInvJointMtx(int i) { return mInv[i]; }
};
// --- copy of the production function (J3DMtxBuffer.cpp) ---
static void J3DCalcWeightEnvelopeNeon(J3DJointTree* pTree, Mtx* pAnmMtx, Mtx* pOut, const u8* pScaleFlag, u8* pEnvScaleFlag) {
#include "body.inc"
}
// --- the portable reference: mtx.c's scalar C_MTXConcat order (== its NEON form) + the loop of J3DMtxBuffer.cpp ---
static void concatRef(const Mtx a, const Mtx b, Mtx ab) {
    Mtx t;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        volatile f32 x = a[r][0] * b[0][c]; volatile f32 y = a[r][1] * b[1][c]; volatile f32 z = a[r][2] * b[2][c];
        volatile f32 s = x + y; s = s + z; if (c == 3) s = s + a[r][3]; t[r][c] = s; }
    memcpy(ab, t, sizeof(Mtx));
}
static void ref(J3DJointTree* t, Mtx* anm, Mtx* out, const u8* sf, u8* esf) {
    const u16* pIdx = t->getWEvlpMixMtxIndex(); const f32* pW = t->getWEvlpMixWeight();
    for (int e = 0; e < t->getWEvlpMtxNum(); e++) {
        esf[e] = 1; Mtx acc; for (int r = 0; r < 3; r++) acc[r][0] = acc[r][1] = acc[r][2] = acc[r][3] = 0.0f;
        int n = t->getWEvlpMixMtxNum(e), k = 0;
        do { u16 j = *pIdx++; f32 w = *pW++; Mtx prod; concatRef(anm[j], t->getInvJointMtx(j), prod);
             for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) { volatile f32 m = prod[r][c] * w; acc[r][c] = acc[r][c] + m; }
             esf[e] &= sf[j]; } while (++k < n);
        memcpy(out[e], acc, sizeof(Mtx));
    }
}
static uint32_t rng = 12345; static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng; }
static f32 rf() { uint32_t k = rnd() % 10; if (k == 0) return 0.0f; if (k == 1) return -0.0f; if (k == 2) return 1.0f;
    return ((int32_t)rnd() / 2147483648.0f) * (float)(1 << (rnd() % 12)); }
int main() {
    long checked = 0, bad = 0;
    for (int it = 0; it < 20000; it++) {
        const int J = 1 + rnd() % 40, E = 1 + rnd() % 30;
        static Mtx anm[64], inv[64], o1[64], o2[64]; static u8 sf[64], e1[64], e2[64], mix[64]; static u16 idx[64 * 8]; static f32 w[64 * 8];
        for (int j = 0; j < J; j++) { for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) { anm[j][r][c] = rf(); inv[j][r][c] = rf(); } sf[j] = rnd() & 1; }
        int m = 0; for (int e = 0; e < E; e++) { mix[e] = 1 + rnd() % 4; for (int k = 0; k < mix[e]; k++) { idx[m] = rnd() % J; w[m] = rf(); m++; } }
        J3DJointTree t{(u16)E, mix, idx, w, inv};
        J3DCalcWeightEnvelopeNeon(&t, anm, o1, sf, e1); ref(&t, anm, o2, sf, e2);
        checked++; if (memcmp(o1, o2, E * sizeof(Mtx)) || memcmp(e1, e2, E)) { bad++; if (bad < 5) printf("mismatch at %d\n", it); }
    }
    printf("envelope: %ld cases, %ld mismatches\n", checked, bad); return bad != 0;
}
