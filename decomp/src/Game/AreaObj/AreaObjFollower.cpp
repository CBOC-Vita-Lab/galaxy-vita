#include "Game/AreaObj/AreaObjFollower.hpp"
#include "Game/AreaObj/AreaObj.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Util/LiveActorUtil.hpp"

AreaObjFollower::AreaObjFollower(AreaObj* pObj, const JMapInfoIter& rIter) : BaseMatrixFollower(pObj, rIter), mObj(pObj) {
    mFollowMtx.identity();
    mObj->setFollowMtx(&mFollowMtx);
}

void AreaObjFollower::update() {
    if (MR::isDead(static_cast< LiveActor* >(getFollowTargetActor())) || !isValid()) {
#ifdef TARGET_PC
        PortAreaSet(mObj->_15, false);  // (GXV_OC_LIGHTFIND: a change moves port_area_gen)
#else
        mObj->_15 = false;
#endif
    } else {
        calcFollowMatrix(&mFollowMtx);

#ifdef TARGET_PC
        PortAreaSet(mObj->_15, true);
#else
        mObj->_15 = true;
#endif
    }
}

namespace MR {
    bool addBaseMatrixFollowerAreaObj(AreaObj* pObj, const JMapInfoIter& rIter) {
        if (!MR::isValidFollowID(rIter)) {
            return false;
        }

        AreaObjFollower* follower = new AreaObjFollower(pObj, rIter);

        MR::addBaseMatrixFollower(follower);

        return true;
    }
};  // namespace MR
