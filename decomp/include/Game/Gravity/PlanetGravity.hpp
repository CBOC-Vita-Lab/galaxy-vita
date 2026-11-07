#pragma once

#include <JSystem/JGeometry.hpp>

class NameObj;

// Gravity vector calculation flags
#define GRAVITY_TYPE_NORMAL 1
#define GRAVITY_TYPE_SHADOW 2
#define GRAVITY_TYPE_MAGNET 4
#define GRAVITY_TYPE_MARIO_LAUNCHER 8

// Gravity power types
#define GRAVITY_POWER_LIGHT 0
#define GRAVITY_POWER_NORMAL 1
#define GRAVITY_POWER_HEAVY 2

class PlanetGravity {
public:
    PlanetGravity();

    virtual void updateMtx(const TPos3f& rMtx);

    virtual bool calcOwnGravityVector(TVec3f* pDest, f32* pScalar, const TVec3f& rPosition) const {
        return false;
    }

    bool calcGravity(TVec3f* pDest, const TVec3f& rPosition) const;
    bool calcGravityFromMassPosition(TVec3f* pDirection, f32* pScalar, const TVec3f& rPosition, const TVec3f& rMassPosition) const;

    bool isInRangeSquare(f32 radius) const;
    bool isInRangeDistance(f32 radius) const;

    f32 getDistant() const {
        return mDistant;
    }

    void setPriority(s32 priority);
    void updateIdentityMtx();

#ifdef TARGET_PC
    // forkB (6 Nov; ported from Producdevity's fork, 1403ec9): the sphere around mReachCentre that holds every position the field
    // accepts (calcReach, widened in updateReach). PlanetGravityManager rejects a position outside it before the virtual call
    // (PETARI_GRAVITY_CULL). Worked out eagerly where the fields calcOwnGravityVector reads are written (registerGravity,
    // updateIdentityMtx, GraviryFollower::update: the writers port/grav_memo.h lists), never lazily, so readers on other threads
    // only ever read it.
    bool isOutOfSphere(const TVec3f& rPosition) const {
        if (!(mReachSquared >= 0.0f)) {
            return false;  // (no sphere: a cone or wire field, a field without a range, or not worked out yet)
        }
        const f32 x = rPosition.x - mReachCentre.x, y = rPosition.y - mReachCentre.y, z = rPosition.z - mReachCentre.z;
        const f32 distanceSquared = x * x + y * y + z * z;
        return distanceSquared > mReachSquared && distanceSquared <= __FLT_MAX__;
    }

    // The radius of that sphere around *pCentre; negative when the field gives none.
    virtual f32 calcReach(TVec3f* pCentre) const {
        return -1.0f;
    }
    void updateReach();
#endif

    /* 0x4 */ f32 mRange;
    /* 0x8 */ f32 mDistant;
    /* 0xC */ s32 mPriority;
    /* 0x10 */ s32 mGravityId;     // read from the stage files but never used
    /* 0x14 */ const void* mHost;  // host object pointer, can be any pointer
    /* 0x18 */ u32 mGravityType;   // gravity type bits
    /* 0x1C */ s32 mGravityPower;  // gravity power type
    /* 0x20 */ bool mActivated;    // corresponding GlobalGravityObj has switch activated?
    /* 0x21 */ bool mIsInverse;    // invert gravity vector?
    /* 0x22 */ bool mValidFollower;
    /* 0x23 */ bool mIsRegistered;  // is registered by PlanetGravityManager?
    /* 0x24 */ bool mAppeared;      // corresponding GlobalGravityObj has appeared?
#ifdef TARGET_PC
    f32 mReachSquared;    // (isOutOfSphere; negative: no sphere)
    TVec3f mReachCentre;
#endif
};
