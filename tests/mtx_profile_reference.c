#include <string.h>
#include "revolution/mtx.h"

// Uninstrumented formula from the pinned port, compiled with the same C flags.
u32 mtx_profile_reference(const Mtx src, Mtx inv) {
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] + src[0][2] * src[1][0] * src[2][1] -
              src[2][0] * src[1][1] * src[0][2] - src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];
    if (det == 0.0f) return 0;
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
    return 1;
}
