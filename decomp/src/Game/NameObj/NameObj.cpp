#include "Game/NameObj/NameObj.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Scene/SceneNameObjMovementController.hpp"
#include "Game/Util/SingletonHolder.hpp"

#define FLAG_MOVEMENT_OFF 1u
#define FLAG_SUSPEND 2u
#define FLAG_RESUME 4u

NameObj::NameObj(const char* pName) : mName(pName), mFlag(), mExecutorIdx(-1) {
    SingletonHolder< NameObjRegister >::get()->add(this);
}

NameObj::~NameObj() {
}

void NameObj::init(const JMapInfoIter& rIter) {
}

void NameObj::initAfterPlacement() {
}

void NameObj::movement() {
}

void NameObj::draw() const {
}

void NameObj::calcAnim() {
}

void NameObj::calcViewAndEntry() {
}

void NameObj::initWithoutIter() {
    init(JMapInfoIter());
}

void NameObj::setName(const char* pName) {
    mName = pName;
}

#ifdef TARGET_PC
#include "port/actor_lod.h"
// GXV_ALOD (actorlod23; decomp/src/Game/LiveActor/ActorTurnLod.cpp): a LiveActor of a marked family decides here whether this turn's
// movement is left out (and owed) or made (after the owed ones are replayed). Unmarked objects, and every object with the switch
// off: one test of a bit of the flags this function already loads.
void port_alod_movement(NameObj* pObj);
#endif

void NameObj::executeMovement() {
    if ((mFlag & FLAG_MOVEMENT_OFF) == FLAG_MOVEMENT_OFF) {
        return;
    }

#ifdef TARGET_PC
    if ((mFlag & PORT_NAMEOBJ_FLAG_ALOD) != 0) {
        port_alod_movement(this);
        return;
    }
#endif
    movement();
}

void NameObj::requestSuspend() {
    if ((getFlag() & FLAG_RESUME) == FLAG_RESUME) {
        mFlag &= ~FLAG_RESUME;
    }

    mFlag |= FLAG_SUSPEND;
}

void NameObj::requestResume() {
    if ((getFlag() & FLAG_SUSPEND) == FLAG_SUSPEND) {
        mFlag &= ~FLAG_SUSPEND;
    }

    mFlag |= FLAG_RESUME;
}

void NameObj::syncWithFlags() {
    if ((getFlag() & FLAG_SUSPEND) == FLAG_SUSPEND) {
        mFlag &= ~FLAG_SUSPEND;
        mFlag |= FLAG_MOVEMENT_OFF;
    }

    if ((getFlag() & FLAG_RESUME) == FLAG_RESUME) {
        mFlag &= ~FLAG_RESUME;
        mFlag &= ~FLAG_MOVEMENT_OFF;
    }
}

void NameObjFunction::requestMovementOn(NameObj* pObj) {
    pObj->requestResume();
    MR::notifyRequestNameObjMovementOnOff();
}

void NameObjFunction::requestMovementOff(NameObj* pObj) {
    pObj->requestSuspend();
    MR::notifyRequestNameObjMovementOnOff();
}
