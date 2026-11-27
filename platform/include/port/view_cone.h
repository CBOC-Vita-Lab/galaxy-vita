// GXV_FLUSH_OFFSCREEN (gamepar, 8 Nov): is a sphere certainly outside a perspective view? Used by decomp/src/Game/LiveActor/LiveActor.cpp
// port_anim_flush_stale with the picture's camera (j3dSys's view matrix, CameraContext's fovy and aspect: the projection the game sets,
// CameraContext::updateProjectionMtx). GX view space: the camera looks down -z; a point is in the picture when -z > 0 and |x| <= -z * tan(fovy/2)
// * aspect and |y| <= -z * tan(fovy/2). The test widens both tangents (kWiden) and has no near or far plane, so it can only answer "outside"
// for spheres that are beyond a side plane, or behind the camera's plane, by more than their radius. Host test: tools/test_view_cone.sh.
#pragma once
#include <math.h>
namespace port {
namespace viewcone {
constexpr float kWiden = 1.15f;
struct Cone {
    float m[3][4];
    float tx, ty, nx, ny;
    bool ok;
};
inline void Make(Cone* c, const float view[3][4], float fovyDeg, float aspect) {
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 4; ++k) {
            c->m[r][k] = view[r][k];
        }
    }
    c->ok = fovyDeg > 1.0f && fovyDeg < 179.0f && aspect > 0.1f && aspect < 10.0f;
    if (!c->ok) {
        return;
    }
    c->ty = tanf(fovyDeg * (3.14159265f / 360.0f)) * kWiden;
    c->tx = c->ty * aspect;
    c->nx = 1.0f / sqrtf(1.0f + c->tx * c->tx);
    c->ny = 1.0f / sqrtf(1.0f + c->ty * c->ty);
}
// true: the sphere (world x, y, z, radius r) is certainly outside; NaN anywhere: false
inline bool Outside(const Cone& c, float x, float y, float z, float r) {
    if (!c.ok || !(r >= 0.0f)) {
        return false;
    }
    const float vx = c.m[0][0] * x + c.m[0][1] * y + c.m[0][2] * z + c.m[0][3];
    const float vy = c.m[1][0] * x + c.m[1][1] * y + c.m[1][2] * z + c.m[1][3];
    const float vz = c.m[2][0] * x + c.m[2][1] * y + c.m[2][2] * z + c.m[2][3];
    if (!(vx == vx) || !(vy == vy) || !(vz == vz)) {
        return false;
    }
    if (vz > r) {
        return true;  // (wholly behind the camera's plane)
    }
    const float d = -vz;
    // signed distances to the four side planes through the eye (outward normals (+-1, 0, -t) and (0, +-1, -t), normalised)
    if ((vx - c.tx * d) * c.nx > r || (-vx - c.tx * d) * c.nx > r) {
        return true;
    }
    if ((vy - c.ty * d) * c.ny > r || (-vy - c.ty * d) * c.ny > r) {
        return true;
    }
    return false;
}
}  // namespace viewcone
}  // namespace port
