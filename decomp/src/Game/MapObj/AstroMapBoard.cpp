#include "Game/MapObj/AstroMapBoard.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/MapObj/AstroDemoFunction.hpp"
#include "Game/MapObj/MapObjActorInitInfo.hpp"
#include "Game/Screen/GalaxyMapController.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"

namespace {
    static const char* cDummyTexName = "MapDummy";
};  // namespace

namespace NrvAstroMapBoard {
    NEW_NERVE(AstroMapBoardNrvWait, AstroMapBoard, Wait);
};  // namespace NrvAstroMapBoard

AstroMapBoard::AstroMapBoard(const char* pName) : MapObjActor(pName) {
}

void AstroMapBoard::init(const JMapInfoIter& rIter) {
    MapObjActor::init(rIter);
    MapObjActorInitInfo info;
    MapObjActorUtil::setupInitInfoSimpleMapObj(&info);
    info.setupPrepareChangeDummyTexture(::cDummyTexName);
    info.setupNerve(GET_NERVE(AstroMapBoard, AstroMapBoardNrvWait));
    info.setupFarClipping(-1.0f);
    info.setupNoAppearRiddleSE();
    initialize(rIter, info);
    AstroDemoFunction::tryRegisterGrandStarReturnAndSimpleCast(this, rIter);
    AstroDemoFunction::tryRegisterDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\xe0\x96\xbe\x83\x66\x83\x82", rIter);
    AstroDemoFunction::tryRegisterDemo(this, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\x69\x92\xbb\x83\x66\x83\x82", rIter);

    if (MR::isButlerMapAppear()) {
        makeActorAppeared();
    } else {
        makeActorDead();
    }
}

void AstroMapBoard::connectToScene(const MapObjActorInitInfo& rInfo) {
    MR::connectToSceneIndirectMapObj(this);
}

void AstroMapBoard::initAfterPlacement() {
    if (MR::isButlerMapAppear()) {
        MR::changeModelDataTexAll(this, ::cDummyTexName, *MR::getGalaxyMapResTIMG());
    }
}

void AstroMapBoard::exeWait() {
}

AstroMapBoard::~AstroMapBoard() {
}
