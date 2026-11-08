#include "Game/Enemy/KarikariDirector.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/Player/MarioParts.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Player/ModelHolder.hpp"
#include "Game/Screen/GameSceneLayoutHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EffectUtil.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <revolution/types.h>

void MarioActor::setPlayerMode(u16 playerMode, bool myBool) {
    if (mPlayerMode == playerMode) {
        if (playerMode == 1) {
            _3DC = mConst->getTable()->mMetalHoldTime;
            MR::startSubBGM("BGM_MUTEKI_A", false);
        }

        if (playerMode == 9) {
            _3DC = mConst->getTable()->mTornadoHoldTime;
        }

        if (playerMode == 3) {
            _3DC = mConst->getTable()->mIceModeTime;
            MR::startSubBGM("BGM_ICE_A", false);
        }

        if (playerMode == 2) {
            _3DC = mConst->getTable()->mFireModeTime;
            MR::startSubBGM("BGM_FIRE_A", false);
        }

        if (playerMode == 7) {
            _3DC = 3600;
            MR::startStageBGM("BGM_FLYING_A", false);
        }

        if (playerMode != 0) {
            playEffect("\x83\x41\x83\x43\x83\x65\x83\x80\x8d\xc4\x83\x51\x83\x62\x83\x67");
        }

        return;
    }

    if (mMario->isStatusActive(MarioStatus_Foo)) {
        mMario->closeStatus(nullptr);
    }

    _3DC = 0;
    _A6E = 0;

    switch (mPlayerMode) {
    case 5:
        mMario->endRabbitMode();

        MR::startBtp(this, "ColorChange");
        MR::setBtpFrameAndStop(this, 0.0f);
        break;

    case 6:
        MR::deleteEffect(_9A4, "Glow");

        if (playerMode == 0) {
            MR::offCalcAnim(_9A4);
        } else {
            _9A4->kill();
        }

        _483 = false;

        mMario->resetTeresaMode();
        break;

    case 2:
        MR::startBtp(this, "ColorChange");
        MR::setBtpFrameAndStop(this, 0.0f);

        MR::stopSubBGM(30);
        break;

    case 4:
        stopAnimation("\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86");

        if (!mMario->isSwimming()) {
            MR::getGameSceneLayoutHolder()->changeLifeMeterModeGround();
        }

        _9E4->kill();
        _9E8->kill();
        break;

    case 3:
        stopEffect("\x83\x41\x83\x43\x83\x58\x92\x86");

        changeHandMaterial();

        if (mMario->isStatusActive(MarioStatus_Skate)) {
            mMario->closeStatus(nullptr);
        }

        MR::stopSubBGM(30);
        break;

    case 1:
        stopEffect("\x96\xb3\x93\x47\x92\x86");

        _A6E = 0;

        MR::stopSubBGM(30);

        changeHandMaterial();
        break;

    case 7:
        MR::startCurrentStageBGM();
        break;
    }

    _3D6 = mPlayerMode;
    mPlayerMode = playerMode;

    switch (playerMode) {
    case 9:
        _3DC = mConst->getTable()->mTornadoHoldTime;

        MR::startBtp(this, "TornadoElement");

        if (isJumping()) {
            changeAnimation("\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67");
        } else {
            changeAnimation("\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67\x90\xda\x92\x6e\x92\x86");
        }
        break;

    case 1:
        _3DC = mConst->getTable()->mMetalHoldTime;

        playEffect("\x96\xb3\x93\x47\x92\x86");

        MR::startSubBGM("BGM_MUTEKI_A", false);

        changeHandMaterial();

        MR::stopBrk(_9C8);
        MR::stopBrk(_A50);
        MR::stopBrk(_A54);

        MR::startBrk(_9C8, "InvincibleMario");
        MR::startBrk(_A50, "InvincibleMarioHandL");
        MR::startBrk(_A54, "InvincibleMarioHandR");

        MR::removeAllClingingKarikari();
        break;

    case 0:
        MR::startBtp(this, "ElementEnd");

        if (myBool) {
            switch (_3D6) {
            case 4:
            case 5:
            case 6:
                playSound("\x8e\xf4\x82\xa2\x89\xf0\x8f\x9c", -1);
                break;

            default:
                playSound("\x95\xcf\x90\x67\x89\xf0\x8f\x9c", -1);
                break;
            }
        }

        _3DC = 0;
        mMario->_76C = 0;

        if (mMario->isSwimming()) {
            if (_468 == 0) {
                changeAnimation(nullptr, "\x90\x85\x89\x6a\x8a\xee\x96\x7b");
            }
        } else if (isJumping()) {
            changeAnimation(nullptr, "\x97\x8e\x89\xba");
        } else {
            changeAnimation(nullptr, "\x8a\xee\x96\x7b");
        }

        if (_4A4 != nullptr) {
            _4A4->mHost->kill();
            _4A4 = nullptr;
        }
        break;

    case 5:
        mMario->startRabbitMode();
        rushDropThrowMemoSensor();
        break;

    case 6:
        _9A4->appear();

        MR::onCalcAnim(_9A4);

        runTeresaBaseAnimation();

        MR::startBrk(_9A4, "Erase");
        MR::setBrkFrameAndStop(_9A4, 0.0f);

        _483 = true;

        _9A8 = 0.0f;
        _9AC = 0.0f;

        rushDropThrowMemoSensor();

        mMario->startTeresaMode();

        _9A4->calcAnim();

        MR::emitEffect(_9A4, "Glow");
        MR::emitEffect(_9A4, "Appear");
        break;

    case 2:
        MR::startSubBGM("BGM_FIRE_A", false);

        _3DC = mConst->getTable()->mFireModeTime;
        break;

    case 3:
        playEffect("\x83\x41\x83\x43\x83\x58\x92\x86");

        MR::startSubBGM("BGM_ICE_A", false);

        changeHandMaterial();

        _3DC = mConst->getTable()->mIceModeTime;
        break;

    case 4:
        MR::getGameSceneLayoutHolder()->changeLifeMeterModeBee();

        rushDropThrowMemoSensor();

        _9E4->appear();
        MR::showModel(_9E4);

        mMario->_402 = mConst->getTable()->mAirWalkTime;
        mMario->_42A = 0;

        _9E8->appear();
        _9F0 = 0;

        MR::startBck(_9E8, "Wait");
        MR::startBva(_9E8, "Wait");

        MR::stopBtk(_9E8);
        break;

    case 7:
        MR::startStageBGM("BGM_FLYING_A", false);
        _3DC = 3600;
        break;
    }

    if (mPlayerMode == 0) {
        if (mTransforming) {
            MR::endDemo(this, "\x83\x7d\x83\x8a\x83\x49\x95\xcf\x90\x67");
            mTransforming = false;
        }

        if (myBool) {
            _3D8 = 30;
            mMario->startPadVib("\x83\x7d\x83\x8a\x83\x49[\x95\xcf\x90\x67\x89\xf0\x8f\x9c]");
        } else {
            _3D8 = 2;
        }

        playEffect("\x95\xcf\x90\x67\x89\xf0\x8f\x9c");
    } else {
        if (_3D8 == 0 || _3D6 == 0) {
            mPowerupCollected = true;
        } else {
            _3D8 = 64;

            MR::stopAnimFrame(this);

            playEffect("\x95\xcf\x90\x67");

            mMario->startPadVib("\x83\x7d\x83\x8a\x83\x49[\x95\xcf\x90\x67]");

            mPowerupCollected = false;
        }

        _945 = 0;
        _946 = 0;

        _6D4 = 0.0f;
        _6D8 = 0.0f;

        _94C = 0;
        _94E = 0;

        updateFairyStar();
    }

    updateHandAtMorph();
}

void MarioActor::resetPlayerModeOnDamage() {
    if (isActionOk("\x83\x5f\x83\x81\x81\x5b\x83\x57\x89\xf0\x8f\x9c")) {
        setPlayerMode(0, true);
    }
}

void MarioActor::resetPlayerModeOnNoDamage() {
    if (isActionOk("\x83\x6d\x81\x5b\x83\x5f\x83\x81\x81\x5b\x83\x57\x89\xf0\x8f\x9c")) {
        setPlayerMode(0, true);
    }
}

void MarioActor::updatePlayerMode() {
    if (_3DC == 0) {
        return;
    }

    _3DC--;

    if (_3DC != 0) {
        return;
    }

    setPlayerMode(0, true);
}

struct myStruct {
    const char* _0[9];
    u32 _24;
};

static myStruct cMorphStringTable[] = {{"DieBlackHole", 0, 0, 0, "DieBlackHoleBee", 0, 0, 0, "DieBlackHoleLuigi", 0}, {}};

void MarioActor::touchSensor(HitSensor* pSensor) {
    switch (pSensor->mType) {
    case ATYPE_WATER_PRESSURE_BULLET_BIND:
        if (MR::isExistMapCollision(_2A0, pSensor->mPosition - _2A0)) {
            return;
        }

        mMario->touchWater();
    }

    if (MR::isSensorEnemy(pSensor) && mMario->isStatusActive(MarioStatus_Bury)) {
        mMario->closeStatus(nullptr);
    }
}

void MarioActor::initMorphStringTable() {
    for (myStruct* item = cMorphStringTable;; item++) {
        if (item->_0[0] == nullptr) {
            break;
        }

        item->_24 = MR::getHashCode(item->_0[0]);
    }
}

const char* MarioActor::changeMorphString(const char* name) const {
    u32 hash = MR::getHashCode(name);
    for (myStruct* item = cMorphStringTable;; item++) {
        if (item->_0[0] == nullptr) {
            break;
        }
        if (item->_24 == hash) {
            const char* morph = item->_0[mPlayerMode];
            if (morph == nullptr || mPlayerMode == PlayerMode_Normal) {
                if (gIsLuigi) {
                    if (item->_0[8] != nullptr) {
                        return item->_0[8];
                    }
                }
                return item->_0[0];
            }
            return morph;
        }
    }
    return name;
}
