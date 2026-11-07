#include "Game/Gravity/GraviryFollower.hpp"
#include "Game/Gravity/GlobalGravityObj.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#ifdef TARGET_PC
#include "port/grav_memo.h"  // (GXV_MO_GRAVMEMO, mapobjcut: writers of a gravity's fields move port_grav_gen)
#endif

GraviryFollower::GraviryFollower(GlobalGravityObj* pGravityObj, const JMapInfoIter& rIter)
    : BaseMatrixFollower(pGravityObj, rIter), mGravity(nullptr) {
    mGravity = pGravityObj->getGravity();
}

void GraviryFollower::setGravityFollowHost(const NameObj* pHost) {
    mGravity->mHost = pHost;
#ifdef TARGET_PC
    PortGravBump();
#endif
}

void GraviryFollower::update() {
    GlobalGravityObj* pFollowTarget = static_cast< GlobalGravityObj* >(getFollowTargetActor());

    if (MR::isDead(pFollowTarget) || MR::isClipped(pFollowTarget) || !isValid()) {
#ifdef TARGET_PC
        PortGravSet(mGravity->mValidFollower, false);
#else
        mGravity->mValidFollower = false;
#endif
    } else {
        TPos3f followMtx;
        calcFollowMatrix(&followMtx);

        mGravity->updateMtx(followMtx);
#ifdef TARGET_PC
        PortGravBump();  // (the world shape follows the matrix)
        mGravity->updateReach();  // (PETARI_GRAVITY_CULL's sphere moves with it)
#endif
        mGravity->mValidFollower = true;
    }
}

namespace MR {
    bool addBaseMatrixFollowerGravity(GlobalGravityObj* pGravityObj, const JMapInfoIter& rIter) {
        if (!MR::isValidFollowID(rIter)) {
            return false;
        }

        MR::addBaseMatrixFollower(new GraviryFollower(pGravityObj, rIter));
        return true;
    }
};  // namespace MR
