// GXV_PTCL_THIN (gamecpu25, 12 Nov; default 0): direction D, the particle draw recording thinned at the source by distance. Pure parts (no game types).
// A particle emitter whose origin is farther than D0 from the camera draws only a FRACTION of its particles, the fraction falling linearly with the
// distance from 1 at D0 to FMIN at D1 (and staying FMIN beyond). Which particles stay is decided by a hash of the particle's own address, so a
// particle is in or out for its whole life at a given fraction, and as the camera moves the set shrinks or grows one particle at a time (no pop of a
// whole emitter; no flicker from list order). Nothing is thinned within D0 of the camera, in an orthographic (2D) group, or with FMIN >= 1.
#pragma once
#include <cmath>
#include <cstdint>

namespace port {
namespace ptclthin {
struct Cfg {
    float fmin;      // fraction kept at and beyond D1; <= 0 or >= 1: off
    float d0, d1;    // distances (world units) from the camera
};
inline bool On(const Cfg& c) { return c.fmin > 0.0f && c.fmin < 1.0f && c.d1 > c.d0 && c.d0 >= 0.0f; }
// The camera's world position from a GX view matrix (3 rows of 4): -R^T t.
inline void CamPos(const float m[3][4], float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = -(m[0][k] * m[0][3] + m[1][k] * m[1][3] + m[2][k] * m[2][3]);
}
inline float KeepFraction(float dist2, const Cfg& c) {
    if (!On(c) || dist2 <= c.d0 * c.d0) return 1.0f;
    if (dist2 >= c.d1 * c.d1) return c.fmin;
    const float t = (std::sqrt(dist2) - c.d0) / (c.d1 - c.d0);
    return 1.0f - (1.0f - c.fmin) * t;
}
// A well-mixed value in [0, 1) from a particle's identity.
inline float Hash01(uintptr_t id) {
    uint32_t h = static_cast<uint32_t>(id >> 3) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}
inline bool Keep(uintptr_t id, float frac) { return Hash01(id) < frac; }
} // namespace ptclthin
} // namespace port
