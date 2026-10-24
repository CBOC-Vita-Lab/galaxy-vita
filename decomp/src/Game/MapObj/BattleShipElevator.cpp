#include "Game/MapObj/BattleShipElevator.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Map/CollisionParts.hpp"
#include "Game/MapObj/MapObjActorInitInfo.hpp"
#include "Game/Util.hpp"

namespace NrvBattleShipElevator {
    NEW_NERVE(BattleShipElevatorNrvWait, BattleShipElevator, Wait);
    NEW_NERVE(BattleShipElevatorNrvMove, BattleShipElevator, Move);
    NEW_NERVE(BattleShipElevatorNrvEnd, BattleShipElevator, End);
};  // namespace NrvBattleShipElevator

BattleShipElevator::BattleShipElevator(const char* pName) : MapObjActor(pName) {
}

BattleShipElevator::~BattleShipElevator() {
}

void BattleShipElevator::init(const JMapInfoIter& rIter) {
    MapObjActor::init(rIter);
    MapObjActorInitInfo info;
    info.setupHioNode("\x92\x6e\x8c\x60\x83\x49\x83\x75\x83\x57\x83\x46");
    info.setupDefaultPos();
    info.setupConnectToScene();
    info.setupEffect(nullptr);
    info.setupSound(4);
    info.setupRailMover();
    info.setupNerve(GET_NERVE(BattleShipElevator, BattleShipElevatorNrvWait));
    MapObjActor::initialize(rIter, info);
}

void BattleShipElevator::exeWait() {
}

void BattleShipElevator::exeMove() {
    if (MR::isFirstStep(this)) {
        MR::startSound(this, "SE_OJ_B_SHIP_ELEV_START");
        MapObjActor::startMapPartsFunctions();
    }
    MR::startLevelSound(this, "SE_OJ_LV_B_SHIP_ELEV_MOVE");
    if (!MapObjActorUtil::isRailMoverWorking(this)) {
        MR::startSound(this, "SE_OJ_B_SHIP_ELEV_STOP");
        setNerve(GET_NERVE(BattleShipElevator, BattleShipElevatorNrvEnd));
    }
}

void BattleShipElevator::exeEnd() {
}

void BattleShipElevator::control() {
    if (!isNerve(GET_NERVE(BattleShipElevator, BattleShipElevatorNrvWait))) {
        MapObjActor::control();
    }
}

bool BattleShipElevator::receiveOtherMsg(u32 msg, HitSensor* pSender, HitSensor* pReceiver) {
    if (!isNerve(GET_NERVE(BattleShipElevator, BattleShipElevatorNrvWait))) {
        return false;
    }

    if (MR::isMsgFloorTouch(msg)) {
        if (MR::isOnPlayer(getSensor("body"))) {
            MR::requestStartTimeKeepDemoMarioPuppetable(this, "\x83\x47\x83\x8c\x83\x78\x81\x5b\x83\x5e\x81\x5b\x8f\xe3\x8f\xb8", GET_NERVE(BattleShipElevator, BattleShipElevatorNrvMove), nullptr,
                                                        nullptr);
        }
    }

    return false;
}
