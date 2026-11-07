// mapobjcut (6 Nov): GXV_MO_GRAVMEMO, the gravity-query memo (decomp/src/Game/Gravity/PlanetGravityManager.cpp).
//
// A gravity query (PlanetGravityManager::calcTotalGravityVector) is a pure function of its inputs (position, gravity types,
// requester) and of the gravity world: the registered gravities, their order, and each one's fields (switch/appear/follower
// state, range, distance, priority, type, host, inverse and the world-space shape its updateMtx derives). port_grav_gen counts the
// changes of that world. Every writer of those fields bumps it at the write:
//   - PlanetGravityManager: constructor (a new scene) and registerGravity;
//   - PlanetGravity::setPriority and updateIdentityMtx (stage-load settings; registration bumps again after them);
//   - GraviryFollower::update (every updateMtx from a follow matrix, and a change of mValidFollower) and setGravityFollowHost;
//   - GlobalGravityObj: mAppeared (appear / dead) and mActivated (stage switches), when the value changes;
//   - MarioLauncherAttractor::init (it sets the type after registration).
// No other code writes a gravity's fields (grep of decomp/src, 6 Nov: the gravity classes are created only by the GravityCreators
// and changed only by the files above and GravityUtil's stage-load settings, which run before registerGravity).
// A memo entry is used only while the generation it was made in is still current, so a hit returns what the query computes.
#pragma once

#include <cstdint>

extern "C" uint32_t port_grav_gen;

static inline void PortGravBump() {
    ++port_grav_gen;
}
// The field takes the value; the generation moves only when the value changes (writing the same value changes nothing).
template < class T > static inline void PortGravSet(T& rField, T value) {
    if (rField != value) {
        rField = value;
        ++port_grav_gen;
    }
}
