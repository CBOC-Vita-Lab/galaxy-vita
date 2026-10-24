#include "Game/Player/MarioSukekiyo.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Util/MtxUtil.hpp"

MarioSukekiyo::MarioSukekiyo(MarioActor* pActor) : MarioState(pActor, MarioStatus_Sukekiyo) {
    _44 = new Triangle();
    _14.zero();
    _20.zero();
    _2C.zero();
    _38.zero();
    _48 = 0;
    _4A = 0;
}

MarioBury::MarioBury(MarioActor* pActor) : MarioSukekiyo(pActor) {
    mStatusId = MarioStatus_Bury;
}

bool MarioSukekiyo::notice() {
    return false;
}

bool MarioSukekiyo::postureCtrl(MtxPtr pMtx) {
    MR::makeMtxUpSide(reinterpret_cast< TPos3f* >(pMtx), _14, _20);
    return true;
}

bool MarioSukekiyo::start() {
    playSound("\x83\x58\x83\x50\x83\x4c\x83\x88\x8a\x4a\x8e\x6e");
    Mario* player = getPlayer();
    playEffectRT("\x91\xae\x90\xab\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76", player->_368, getTrans());
    startPadVib("\x8d\xc5\x8b\xad");
    startCamVib(3);
    mActor->_F44 = 0;
    _14 = getPlayer()->_368;
    _20 = getPlayer()->mSideVec;
    getPlayer()->forceSetHeadVecKeepSide(_14);
    _48 = 0;
    _4A = 0;
    getPlayer()->stopJump();
    getPlayer()->stopWalk();

    if (mStatusId == MarioStatus_Sukekiyo) {
        changeAnimation("\x83\x58\x83\x50\x83\x4c\x83\x88");
    } else {
        playSound("\x90\xba\x91\xab\x96\x84\x82\xdc\x82\xe8\x8a\x4a\x8e\x6e");
        changeAnimation("\x96\x84\x82\xdc\x82\xe8");
    }

    return true;
}

bool MarioSukekiyo::update() {
    if (!getPlayer()->isCurrentFloorSand()) {
        return false;
    }

    if (_4A) {
        return isAnimationRun(nullptr) != false;
    }

    if (mActor->isRequestRush()) {
        _4A = 1;
    }

    if (checkTrgA()) {
        _4A = 1;
    }

    if (_4A) {
        if (mStatusId == MarioStatus_Sukekiyo) {
            changeAnimation("\x83\x58\x83\x50\x83\x4c\x83\x88\x92\x45\x8f\x6f", "\x8a\xee\x96\x7b");
            playSound("\x90\xba\x83\x58\x83\x50\x83\x4c\x83\x88\x8f\x49\x97\xb9");
        } else {
            changeAnimation("\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f", "\x8a\xee\x96\x7b");
            playSound("\x90\xba\x91\xab\x96\x84\x82\xdc\x82\xe8\x8f\x49\x97\xb9");
        }

        playSound("\x83\x58\x83\x50\x83\x4c\x83\x88\x8f\x49\x97\xb9");
    }

    return true;
}

bool MarioSukekiyo::close() {
    getPlayer()->stopWalk();
    mActor->_F44 = 1;
    stopAnimation(nullptr, "\x8a\xee\x96\x7b");
    return true;
}
