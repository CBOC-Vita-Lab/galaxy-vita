#include "Game/Gravity.hpp"
#include "Game/Util.hpp"
#ifdef TARGET_PC
#include "port/grav_memo.h"  // (GXV_MO_GRAVMEMO, mapobjcut: writers of a gravity's fields move port_grav_gen)
#endif

void PlanetGravity_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
}

PlanetGravity::PlanetGravity() {
    mRange = -1.0f;
    mDistant = 0.0f;
    mPriority = 0;
    mGravityId = -1;
    mHost = this;
    mGravityType = GRAVITY_TYPE_NORMAL;
    mGravityPower = GRAVITY_POWER_NORMAL;
    mActivated = true;
    mIsInverse = false;
    mValidFollower = true;
    mIsRegistered = false;
    mAppeared = true;
#ifdef TARGET_PC
    mReachSquared = -1.0f;
    mReachCentre.x = mReachCentre.y = mReachCentre.z = 0.0f;
#endif
}

void PlanetGravity::setPriority(s32 priority) {
    mPriority = priority;
#ifdef TARGET_PC
    PortGravBump();
#endif
}

bool PlanetGravity::calcGravity(TVec3f* pDest, const TVec3f& rPosition) const {
    // Calculate raw gravity vector
    f32 radius = 0.0f;
    TVec3f gravity;
    gravity.x = 0.0f;
    gravity.y = 0.0f;
    gravity.z = 0.0f;

    if (!calcOwnGravityVector(&gravity, &radius, rPosition))
        return false;

    // Adjust radius
    radius -= mDistant;

    if (radius < 1.0f) {
        radius = 1.0f;
    }

    // Apply gravity speed
    f32 scalar = 4000000.0f / (radius * radius);
    gravity.x *= scalar;
    gravity.y *= scalar;
    gravity.z *= scalar;

    // Invert vector if necessary
    if (mIsInverse) {
        gravity = -gravity;
    }

    // Set result vector
    pDest->x = gravity.x;
    pDest->y = gravity.y;
    pDest->z = gravity.z;

    return true;
}

bool PlanetGravity::isInRangeSquare(f32 radius) const {
    f32 range = mRange;

    if (range < 0.0f) {
        return true;
    } else {
        f32 distance = range + mDistant;
        return radius < distance * distance;
    }
}

bool PlanetGravity::isInRangeDistance(f32 radius) const {
    f32 range = mRange;

    if (range < 0.0f) {
        return true;
    } else {
        f32 distance = range + mDistant;
        return radius < distance;
    }
}

bool PlanetGravity::calcGravityFromMassPosition(TVec3f* pDirection, f32* pScalar, const TVec3f& rPosition, const TVec3f& rMassPosition) const {
    TVec3f direction = rMassPosition - rPosition;
    f32 scalar;

    MR::separateScalarAndDirection(&scalar, &direction, direction);

    if (!isInRangeDistance(scalar))
        return false;

    if (pDirection) {
        *pDirection = direction;
    }
    if (pScalar) {
        *pScalar = scalar;
    }

    return true;
}

void PlanetGravity::updateMtx(const TPos3f& rMtx) {
}

void PlanetGravity::updateIdentityMtx() {
    TPos3f mtx;
    mtx.identity();
    updateMtx(mtx);
#ifdef TARGET_PC
    PortGravBump();
    updateReach();
#endif
}

#ifdef TARGET_PC
void PlanetGravity::updateReach() {
    mReachSquared = -1.0f;  // (no sphere while the centre is rewritten)
    TVec3f centre(0.0f, 0.0f, 0.0f);
    const f32 reach = calcReach(&centre);
    if (!(reach >= 0.0f) || !(reach <= 1e18f)) {
        return;
    }
    // (widened far beyond the rounding of the shapes' arithmetic, including that of a centre far from the origin; as the fork's)
    const f32 limit = reach * 1.001f + (__builtin_fabsf(centre.x) + __builtin_fabsf(centre.y) + __builtin_fabsf(centre.z)) * 1e-5f + 1.0f;
    mReachCentre = centre;
    mReachSquared = limit * limit;
}
#endif
