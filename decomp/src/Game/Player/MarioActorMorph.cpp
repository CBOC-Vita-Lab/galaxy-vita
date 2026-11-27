#include "Game/Enemy/KarikariDirector.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/MP/MpSelfPuppet.hpp"
#include "Game/Util/FurMulti.hpp"
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
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <revolution/os.h>
extern "C" void port_log_summary(const char* fmt, ...);
#include <revolution/types.h>
#ifdef TARGET_PC
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "port/player_assets.h"
#include "port/powerup_hook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// luigi24 (10 Nov): the optional player assets (see platform/include/port/player_assets.h) as the plain struct the mode check reads.
static PortPlayerAssets portPlayerAssetsOf(const MarioActor* a) {
    PortPlayerAssets t;
    for (int i = 0; i < 6; i++) {
        t.body[i] = a->mModels[i];
    }
    t.bee = a->_9E4;
    t.beeWing = a->_9E8;
    t.beeFur = a->_9EC;
    t.ice = a->_9C0;
    t.iceHandL = a->_A40;
    t.iceHandR = a->_A44;
    t.iceStep = a->_B4C;
    t.rainbow = a->_9C8;
    t.rainbowHandL = a->_A50;
    t.rainbowHandR = a->_A54;
    t.spring = a->_A00;
    t.boo = a->_9A4;
    t.booAnim = a->_9B8;
    for (int i = 0; i < 3; i++) {
        t.fireBall[i] = a->_B54[i];  // crash24
    }
    t.foo = a->mMario ? a->mMario->mFoo : nullptr;
    return t;
}

namespace {
    // One archive group of the start-of-stage line: "ok" (all loaded), "-" (none loaded: the stage has no such item), "PART[a,b]" (some
    // loaded: the ones named are not) or "NOFILE[a]" (not loaded and not on the card at all: the converted data lacks it).
    void groupState(char* out, size_t size, const char* const* names, int n) {
        char miss[160];
        miss[0] = 0;
        char nofile[160];
        nofile[0] = 0;
        int loaded = 0;
        for (int i = 0; i < n; i++) {
            if (MR::isLoadedObjectOrLayoutArchive(names[i])) {
                loaded++;
                continue;
            }
            char file[64];
            snprintf(file, sizeof(file), "%s.arc", names[i]);
            char path[256];
            const bool onCard = MR::makeObjectArchiveFileName(path, sizeof(path), file);
            strncat(miss, miss[0] ? "," : "", sizeof(miss) - strlen(miss) - 1);
            strncat(miss, names[i], sizeof(miss) - strlen(miss) - 1);
            if (!onCard) {
                strncat(nofile, nofile[0] ? "," : "", sizeof(nofile) - strlen(nofile) - 1);
                strncat(nofile, names[i], sizeof(nofile) - strlen(nofile) - 1);
            }
        }
        if (loaded == n) {
            snprintf(out, size, "ok");
        } else if (nofile[0]) {
            snprintf(out, size, "NOFILE[%s]", nofile);
        } else if (loaded == 0) {
            snprintf(out, size, "-");
        } else {
            snprintf(out, size, "PART[%s]", miss);
        }
    }
}  // namespace

// The start-of-stage line (called once from MarioActor::init2 after the models are made): which optional player assets this stage loaded
// and which not, and from the members themselves which modes can be entered. "-" for an asset is normal in a stage without that item.
void portPlayerRetryReset();
void portPlayerAssetsLog(MarioActor* a, bool isLuigi) {
    portPlayerRetryReset();  // a new stage, a new actor: the Bee retries start again
    const char* beeN[] = {isLuigi ? "BeeLuigi" : "BeeMario", "BeeMarioWing"};
    const char* iceN[] = {isLuigi ? "IceLuigi" : "IceMario", "IceMarioHandL", "IceMarioHandR"};
    const char* rainN[] = {isLuigi ? "InvincibleLuigi" : "InvincibleMario", "InvincibleMarioHandL", "InvincibleMarioHandR"};
    const char* springN[] = {isLuigi ? "HopperLuigi" : "HopperMario"};
    const char* booN[] = {isLuigi ? "TeresaLuigi" : "TeresaMario"};
    const char* tornN[] = {isLuigi ? "LuigiTornado" : "MarioTornado"};
    char bee[200], ice[200], rain[200], spring[200], boo[200], torn[200];
    groupState(bee, sizeof(bee), beeN, 2);
    groupState(ice, sizeof(ice), iceN, 3);
    groupState(rain, sizeof(rain), rainN, 3);
    groupState(spring, sizeof(spring), springN, 1);
    groupState(boo, sizeof(boo), booN, 1);
    groupState(torn, sizeof(torn), tornN, 1);

    const PortPlayerAssets t = portPlayerAssetsOf(a);
    char ok[48];
    char refused[160];
    ok[0] = 0;
    refused[0] = 0;
    static const char* const modeName[] = {"normal", "rainbow", "fire", "ice", "bee", "spring", "boo", "flying"};
    for (unsigned m = 0; m <= 7; m++) {
        const char* why = portPlayerModeMissing(m, t);
        char one[96];
        if (why) {
            snprintf(one, sizeof(one), "%s%s(%s)", refused[0] ? " " : "", modeName[m], why);
            strncat(refused, one, sizeof(refused) - strlen(refused) - 1);
        } else {
            snprintf(one, sizeof(one), "%s%s", ok[0] ? "," : "", modeName[m]);
            strncat(ok, one, sizeof(ok) - strlen(ok) - 1);
        }
    }
    port_log_summary("player-assets: %s:%d %s: bee=%s ice=%s rainbow=%s spring=%s boo=%s tornado=%s | modes ok: %s | modes REFUSED: %s",
                     MR::getCurrentStageName(), (int)MR::getCurrentScenarioNo(), isLuigi ? "Luigi" : "Mario", bee, ice, rain, spring, boo, torn, ok,
                     refused[0] ? refused : "none");
}
#endif

#ifdef TARGET_PC
// relfix24 (10 Nov): GXV_POWERUP_RETRY (default 1, entry.cpp): when a Bee pickup finds the Bee models missing, try the model initialisation again
// before refusing, because the archives may have finished loading after MarioActor::init2 made the models (initBeeMario returns early, and
// nothing else ever calls it again). At most 2 tries per stage, only when both archives now report loaded, so a stage that really lacks the
// bee archives costs two cheap checks and one log line. initBeeMario creates nothing before its two archive checks, so a try that finds them
// missing leaves no half-made state. Plain globals, not function statics (no guard barriers).
static int sPortBeeRetryEnv = -1;
static int sPortBeeRetriesLeft = 2;
void portPlayerRetryReset() {
    sPortBeeRetriesLeft = 2;
}
#endif

// Would setPlayerMode(mode) be accepted? No player state is touched (the Bee retry only fills members that were null). The item objects ask this
// before they use themselves up, so a refused change leaves the item in the world.
bool MarioActor::canEnterPlayerMode(u16 playerMode) {
#ifdef TARGET_PC
    if (mPlayerMode == playerMode) {
        return true;  // setPlayerMode only refreshes the timer and plays the effect
    }
    if (playerMode == 4 && !_9E4 && !_9E8 && !mModels[2] && sPortBeeRetriesLeft > 0) {
        if (sPortBeeRetryEnv < 0) {
            const char* e = getenv("GXV_POWERUP_RETRY");
            sPortBeeRetryEnv = (!e || atoi(e) != 0) ? 1 : 0;
        }
        // (beefix25: the decision is portBeeRetryWanted, tests/bee_late_init.cpp)
        const PortPlayerAssets before = portPlayerAssetsOf(this);
        if (sPortBeeRetryEnv && portBeeRetryWanted(before, MR::isLoadedObjectOrLayoutArchive(portBeeArchiveName(gIsLuigi)),
                                                   MR::isLoadedObjectOrLayoutArchive("BeeMarioWing"), sPortBeeRetriesLeft, true)) {
            sPortBeeRetriesLeft--;
            // the models of a stage are made in its scene heap (initDrawAndModel); this runs from a sensor message, so say so explicitly
            {
                MR::CurrentHeapRestorer restorer(reinterpret_cast< JKRHeap* >(MR::getSceneHeapNapa()));
                // (a late call also makes the Bee eyelid texture slots and hides the Bee body's hand joints: initBeeMario, beefix25)
                initBeeMario();
            }
            port_log_summary("player-assets: Bee models were missing at stage start, retried: _9E4=%p _9E8=%p (%s)", (void*)_9E4, (void*)_9E8,
                             gIsLuigi ? "Luigi" : "Mario");
        }
    }
    const PortPlayerAssets assets = portPlayerAssetsOf(this);
    return portPlayerModeMissing(playerMode, assets) == nullptr;
#else
    return playerMode != 4 || (_9E4 && _9E8);
#endif
}

void MarioActor::setPlayerMode(u16 playerMode, bool myBool) {
#ifdef TARGET_PC
    if (mPlayerMode != playerMode) port_log_summary("mario: mode %u -> %u", (unsigned)mPlayerMode, (unsigned)playerMode); // (2.3: power-up tests)
    if (mPlayerMode != playerMode) port_anticipate_event(5, 0, 1500);  // (anticip32: the transformation's effects and model change are announced to the scaling engine; drawing only)
#endif
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

    if (playerMode == 4 && (!_9E4 || !_9E8)) {
        // port (2.3): the Bee model or its wings were not loaded (initBeeMario returned early). The check further down came after
        // mPlayerMode had already become 4, so Mario stayed "Bee" with null models and a later update (updateModelDiffDL, the wing
        // animations, hideBeeFur/showBeeFur) wrote through the null pointer. Refuse the change before any state is touched.
        OSReport("beecrash: bee model missing (_9E4=%p _9E8=%p), mode change refused, staying %d\n", _9E4, _9E8, (int)mPlayerMode);
        port_log_summary("beecrash: bee model missing (_9E4=%p _9E8=%p), mode change refused, staying %d", (void*)_9E4, (void*)_9E8, (int)mPlayerMode);
        return;
    }

#ifdef TARGET_PC
    {
        // luigi24 (10 Nov): the same refusal for every mode that needs an optional asset (rainbow: model, body, both hands; ice: model, body,
        // both hands, the steps; spring: model, body; boo: model, animation player; bee again with its fur). Their members are nullptr when the
        // stage did not load the archives, and calcAnim, updateHand/drawHand, the Boo/Teresa code and createIceFloor use them unguarded.
        // relfix24: the check is canEnterPlayerMode, which the item pickup asks first (MR::canChangePlayerItemStatus) so a refused item stays.
        if (!canEnterPlayerMode(playerMode)) {
            const PortPlayerAssets assets = portPlayerAssetsOf(this);
            const char* missing = portPlayerModeMissing(playerMode, assets);
            OSReport("player-assets: mode %u refused, %s missing, staying %d\n", (unsigned)playerMode, missing, (int)mPlayerMode);
            port_log_summary("player-assets: mode %u refused, %s missing (%s), staying %d", (unsigned)playerMode, missing, gIsLuigi ? "Luigi" : "Mario",
                             (int)mPlayerMode);
            return;
        }
    }
#endif

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

        if (_9E4) {
            _9E4->kill();
        }
        if (_9E8) {
            _9E8->kill();
        }
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
        if (MpSelfPuppet::hideLocalBody()) {
            MR::hideModelAndOnCalcAnim(_9A4);
        } else {
            MR::showModel(_9A4);
        }

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
        if (!_9E4 || !_9E8) {
            // port: BeeMario/BeeMarioWing archives were not loaded (initBeeMario returned early);
            // fall back to Normal rather than dereferencing null. Do not change life meter.
            OSReport("beecrash: bee model missing (_9E4=%p _9E8=%p), staying Normal\n", _9E4, _9E8);
            port_log_summary("beecrash: bee model missing (_9E4=%p _9E8=%p), staying Normal", (void*)_9E4, (void*)_9E8);
            break;
        }
        port_log_summary("bee: Mario turns into Bee Mario (models %p %p)", (void*)_9E4, (void*)_9E8); // (2.3: one line, to place a crash in time)
        MR::getGameSceneLayoutHolder()->changeLifeMeterModeBee();

        rushDropThrowMemoSensor();

        _9E4->appear();
        MR::showModel(_9E4);

        mMario->_402 = mConst->getTable()->mAirWalkTime;
        mMario->_42A = 0;

        _9E8->appear();
        if (MpSelfPuppet::hideLocalBody()) {
            MR::hideModelAndOnCalcAnim(_9E8);
            if (_9EC != nullptr) {
                _9EC->offDraw(-1);
            }
        } else {
            MR::showModel(_9E8);
            if (_9EC != nullptr) {
                _9EC->onDraw(-1);
            }
        }
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

#ifdef TARGET_PC
// luigi24 (10 Nov) debug hook: PETARI_POWERUP="4,0,3,0" makes the player take those modes in turn through setPlayerMode, the first one
// PETARI_POWERUP_AT turns of play after the actor was made (default 240 = 4 s; only turns in which the player can move count) and each next
// one PETARI_POWERUP_EVERY turns later (default 360 = 6 s). Mode numbers as PlayerMode_* (1 rainbow, 2 fire, 3 ice, 4 bee, 5 spring, 6 boo,
// 7 flying, 9 tornado; 0 cures).
//
// crash24 (10 Nov): what the hook now does that the first version did not (dumps pw1/pw2/pw3: the first transformation of a form in a save read
// _4A4, the touched item's sensor, which only a pickup sets; no pickup was involved):
//  - it offers a form only when a real pickup could happen (portPowerupBlocked: alive, no transformation running, no morph reset timer, state
//    changes enabled, MarioActor::canEnterPlayerMode). A form that cannot wait (assets missing) is dropped with one "powerup-test: ... not offered"
//    line instead of calling setPlayerMode into its refusal; a transient block (a transformation still running) holds the entry and retries
//    every turn for at most 600 turns. PETARI_POWERUP_FORCE=1 restores the old call-it-anyway behaviour (to test setPlayerMode's own refusals).
//  - every new MarioActor (each stage) restarts the list from its first entry (that was always so: init2 calls portPlayerPowerupReset); the list
//    is now bound to the actor it was restarted for, and PETARI_POWERUP_SCENES=n limits it to the first n stages of the run (0 = all, default).
//    A stage change drops the schedule of the old actor: the hook keeps no pointer to any actor but its owner and never acts for another.
//  - the first-transformation camera no longer needs the item sensor (MarioActor::calcAnimInMovement), so a form taken without an item is safe.
// Unset: one cached getenv.
static unsigned sPortPowerupTurn;
static unsigned sPortPowerupNext;
static const char* sPortPowerupList;  // (plain globals, not function statics: no guard barriers on this per-turn path)
static unsigned sPortPowerupAt;
static unsigned sPortPowerupEvery;
static unsigned sPortPowerupScenesMax;
static bool sPortPowerupForce;
static bool sPortPowerupRead;
static unsigned sPortPowerupScene;              // how many player actors this run has made
static const MarioActor* sPortPowerupOwner;     // the actor the schedule runs for (set by the restart in init2; compared only, never dereferenced)
static bool sPortPowerupHeld;                   // an entry is due and waits for a pickup to be possible
static unsigned sPortPowerupHeldTurns;
static void portPowerupReadEnv() {
    if (sPortPowerupRead) {
        return;
    }
    sPortPowerupRead = true;
    sPortPowerupList = getenv("PETARI_POWERUP");
    sPortPowerupAt = getenv("PETARI_POWERUP_AT") ? (unsigned)atoi(getenv("PETARI_POWERUP_AT")) : 240u;
    sPortPowerupEvery = getenv("PETARI_POWERUP_EVERY") && atoi(getenv("PETARI_POWERUP_EVERY")) > 0 ? (unsigned)atoi(getenv("PETARI_POWERUP_EVERY")) : 360u;
    sPortPowerupScenesMax = getenv("PETARI_POWERUP_SCENES") && atoi(getenv("PETARI_POWERUP_SCENES")) > 0 ? (unsigned)atoi(getenv("PETARI_POWERUP_SCENES")) : 0u;
    sPortPowerupForce = getenv("PETARI_POWERUP_FORCE") && atoi(getenv("PETARI_POWERUP_FORCE")) != 0;
}
// A new player actor (stage): the schedule of the previous one is dropped and starts again for this one.
void portPlayerPowerupReset(MarioActor* a) {
    portPowerupReadEnv();
    sPortPowerupTurn = 0;
    sPortPowerupNext = 0;
    sPortPowerupHeld = false;
    sPortPowerupHeldTurns = 0;
    sPortPowerupOwner = a;
    sPortPowerupScene++;
    if (sPortPowerupList && *sPortPowerupList) {
        if (portPowerupSceneAllowed(sPortPowerupScenesMax, sPortPowerupScene)) {
            port_log_summary("powerup-test: player actor #%u (%s): PETARI_POWERUP=%s restarts, first entry at turn %u, then every %u, scenes limit %u%s", sPortPowerupScene,
                             gIsLuigi ? "Luigi" : "Mario", sPortPowerupList, sPortPowerupAt, sPortPowerupEvery, sPortPowerupScenesMax, sPortPowerupForce ? ", FORCE" : "");
        } else {
            port_log_summary("powerup-test: player actor #%u: PETARI_POWERUP is not applied (PETARI_POWERUP_SCENES=%u)", sPortPowerupScene, sPortPowerupScenesMax);
        }
    }
}
extern "C" void port_log_flush(void);  // (os_alarm.cpp: the log is written by a helper thread every 100 ms; a data abort loses what is still in memory)
static void portPlayerPowerupHook(MarioActor* a) {
    portPowerupReadEnv();
    const char* const list = sPortPowerupList;
    if (!list || !*list) {
        return;
    }
    if (a != sPortPowerupOwner || !portPowerupSceneAllowed(sPortPowerupScenesMax, sPortPowerupScene)) {
        return;  // (not the actor the schedule was restarted for, or a stage the list is not meant for)
    }
    const unsigned turn = ++sPortPowerupTurn;
    unsigned mode = 0;
    if (!sPortPowerupHeld) {
        if (!portPowerupDue(turn, sPortPowerupAt, sPortPowerupEvery)) {
            return;
        }
        if (!portPowerupEntry(list, sPortPowerupNext, &mode)) {
            return;  // the list is used up
        }
        sPortPowerupNext++;
        sPortPowerupHeldTurns = 0;
        port_log_summary("powerup-test: turn %u: PETARI_POWERUP entry %u = mode %u (%s)", turn, sPortPowerupNext, mode, gIsLuigi ? "Luigi" : "Mario");
    } else if (!portPowerupEntry(list, sPortPowerupNext - 1, &mode)) {
        sPortPowerupHeld = false;
        return;
    }
    if (!sPortPowerupForce) {
        PortPowerupState st;
        st.currentMode = a->mPlayerMode;
        st.health = a->mHealth;
        st.transformTimer = a->_3D8;
        st.morphResetTimer = a->mMario ? a->mMario->mMorphResetTimer : 0;
        st.nerveChangeEnabled = a->isEnableNerveChange();
        st.formAccepted = mode == 0 || a->canEnterPlayerMode((u16)mode);
        const char* why = "";
        const PortPowerupVerdict v = portPowerupBlocked(mode, st, &why);
        if (v == PORT_POWERUP_DROP) {
            sPortPowerupHeld = false;
            port_log_summary("powerup-test: entry %u (mode %u) not offered: %s; a pickup would leave the item", sPortPowerupNext, mode, why);
            return;
        }
        if (v == PORT_POWERUP_WAIT) {
            if (!sPortPowerupHeld) {
                port_log_summary("powerup-test: entry %u (mode %u) held: %s", sPortPowerupNext, mode, why);
            }
            sPortPowerupHeld = ++sPortPowerupHeldTurns <= 600;
            if (!sPortPowerupHeld) {
                port_log_summary("powerup-test: entry %u (mode %u) dropped after 600 turns held: %s", sPortPowerupNext, mode, why);
            }
            return;
        }
        if (sPortPowerupHeld) {
            port_log_summary("powerup-test: entry %u (mode %u) offered after %u turns held", sPortPowerupNext, mode, sPortPowerupHeldTurns);
        }
    }
    sPortPowerupHeld = false;
    a->setPlayerMode((u16)mode, true);
    port_log_flush();  // (crash24: in dumps pw2/pw3 the hook's own lines were the ones lost: the fault came one turn after this call)
}
#endif

void MarioActor::updatePlayerMode() {
#ifdef TARGET_PC
    portPlayerPowerupHook(this);
#endif
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
