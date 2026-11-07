#include "Game/MapObj/MarioLauncherAttractor.hpp"
#include "Game/Gravity/GravityCreator.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util.hpp"
#ifdef TARGET_PC
#include "port/grav_memo.h"  // (GXV_MO_GRAVMEMO, mapobjcut: writers of a gravity's fields move port_grav_gen)
#endif

namespace NrvMarioLauncherAttractor {
    NEW_NERVE(MarioLauncherAttractorNrvWait, MarioLauncherAttractor, Wait);
};  // namespace NrvMarioLauncherAttractor

MarioLauncherAttractor::MarioLauncherAttractor(const char* pName) : LiveActor(pName), mGravityCreator() {
}

void MarioLauncherAttractor::init(const JMapInfoIter& rIter) {
    MR::initDefaultPos(this, rIter);
    initModelManagerWithAnm("MarioLauncherAttractor", 0, 0);

    // Create and initialize gravity
    mGravityCreator = new PointGravityCreator();
    mGravityCreator->createFromJMap(rIter);
    PlanetGravity* pGravity = mGravityCreator->getGravity();
    pGravity->mGravityType = GRAVITY_TYPE_MARIO_LAUNCHER;
#ifdef TARGET_PC
    PortGravBump();  // (after registration)
#endif

    // Set clipping & scale
    f32 scale = pGravity->mRange / 1000.0f;
    MR::setClippingTypeSphere(this, pGravity->mRange);
    MR::setClippingFar200m(this);

    mScale.set(TVec3f(scale));

    // Connect to executor, init nerve and start animation
    MR::connectToSceneMapObj(this);
    initNerve(GET_NERVE(MarioLauncherAttractor, MarioLauncherAttractorNrvWait));
    MR::startBtk(this, "MarioLauncherAttractor");

    makeActorAppeared();
}

void MarioLauncherAttractor::exeWait() {
}
