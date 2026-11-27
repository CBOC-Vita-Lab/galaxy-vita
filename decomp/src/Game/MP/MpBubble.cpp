// Galaxy 3.0 multiplayer: Play Together "Bubbles" falls, New Super Mario Bros. style (owner: bubble). Rules: MpBubbleLogic.hpp.
//
// The look is the game's own Bubble Breeze bubble: a BigBubble from the scene's BigBubbleHolder (BigBubble.arc textures, its
// wobbling octahedron-Bezier surface, its shadow, its "Break" effect and SE_OJ_BIG_BUBBLE_* sounds), with the player inside in
// BigBubble's own capture pose (startBckPlayer "SpaceWait"). The local player is held the way the game holds Mario in a demo
// (MarioAccess::readyRemoteDemo: no control, no sensors, no damage) and placed every turn with setPlayerBaseMtx; the self puppet
// (Luigi, Toad, ...) follows that pose as always. Remote players: their STATE carries MP_POSE_BUBBLE and every console draws a
// BigBubble around their puppet; no extra traffic while floating.
//
// Messages: BUBBLE (any, relayed by the host) when a player goes in or out; RESTART (host) when everyone is caught: each console
// closes the game's miss wipe and runs its own miss reload (move type 5, local). A pop request (BUMP kind MP_BUMP_POP) from the
// console of a teammate whose spin reached the bubble or whose Star Bit flew through it; the bubbled console checks it and pops.
//
// Sounds: SE_OJ_BIG_BUBBLE_APPEAR / _MARIO_IN / _BREAK need no extra wave bank. The game maps SE waves to scenes with fixed tables
// in AudSceneMgr.cpp (cWaveSetInfo / cSeWaveSet per stage, cSeScenarioWaveSet per scenario), not through the scene archive
// lists; Bubble Breeze and Bubble Blast (CubeBubbleExLv1/Lv2Galaxy) are stage 0 there, which loads no stage SE wave set at all.
// Their bubble sounds therefore come from the static wave arcs (0-6, 8, 11: AudSceneMgr::loadStaticResource, loaded at boot and
// never erased), which every scene has.
#include "Game/MP/MpBubble.hpp"
#include "Game/MP/MpWorld.hpp"   // hostreload: MpWorldSync::dropCarry
#include "Game/MP/MpDemo.hpp"   // MP3 demo
#include "Game/MP/MpSpectate.hpp"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Game/LiveActor/Nerve.hpp"
#include "Game/MP/MpBubbleLogic.hpp"
#include "Game/MP/MpWaitLogic.hpp"   // uxwait: "Waiting for <name>..." while bubbled and a teammate is still loading
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpTeam.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/MapObj/BigBubbleHolder.hpp"
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Map/CollisionCode.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Player/MarioFireDamage.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Player/MarioAccess.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioDamage.hpp"
#include "Game/Ride/BigBubble.hpp"
#include "Game/MP/MpPanelDraw.hpp"   // MP3 ingame: the pop prompt (the panel draws it)
#include "Game/MP/MpPresent.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GameDataFunction.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/MapObj/StarPieceDirector.hpp"
#include "Game/MapObj/StarPiece.hpp"

using namespace MpBubbleLogic;

static_assert(MP_BUMP_POP == kBumpPop, "the pop request's BUMP kind");

namespace {
    const char* const kArchive = "BigBubble";
    const char* const kRiderBck = "SpaceWait";            // BigBubble::exeCapture: the rider's pose
    const char* const kRiderBrk = "BigBubbleCapture";

    struct Local {
        bool on;
        bool out;              // out of lives: only a teammate's touch pops it
        BigBubble* bubble;
        V3 pos;                // the bubble's centre
        V3 vel;
        V3 drift;              // the stick's smoothed offset
        V3 up;
        int age;               // turns in the bubble
        int target;            // the teammate it floats to (-1 none)
        bool hoverLogged;
        int alone;             // turns without any teammate to float to (never strand anyone: a restart after kAloneTurns)
        uint16_t serial;
        bool arrival;          // an arrival bubble (drop-in, warp to a friend): no life lost, pops by itself
        int arriveTarget;      // ...beside this teammate
        int grace;             // turns before a touch / pop request can pop it
        bool canPop;           // MP3 ingame: the bubbled player's own button would pop it now (a grounded teammate beside it)
    };
    // MP3 ingame: the prompt over the local bubble while the player's own button pops it: the button's picture from the game's picture
    // font (the handheld build draws Cross) on a small round plate, as the game marks talkable characters; no words. (The game's own
    // IconAButton layout was tried first: its A texture is not one the handheld build replaces, so it showed the Wii's A.)
    bool sPromptOn = false;
    int sPromptHold = 0;       // turns the prompt stays after the pop stops being possible (a teammate's hop must not blink it)
    float sPromptA = 0.0f, sPromptX = 0.0f, sPromptY = 0.0f;
    struct Remote {
        BigBubble* bubble;
        V3 last;               // its centre this turn (as this console shows it)
        int age;
        uint32_t lastAsk;      // turn of our last pop request for it (sTurn; 0 none)
    };
    // After a pop: the player's state is checked (logged) twice, so a run shows no sink, burn, crush or bind left behind
    struct Check {
        bool active;
        int turns;
        V3 at;                 // where he landed
    };
    struct Restart {
        bool active;
        int phase;             // 0 wipe closing, 1 after the wipe (the miss sequence waits 30 turns too)
        int turns;
    };

    void startRestart(const char* why);

    Local sLocal;
    Check sCheck;
    Remote sRemote[MP_MAX_PLAYERS];
    uint8_t sSeenMask = 0;          // teammates seen in this scene serial (their pose): a newcomer gets our BUBBLE again
    uint16_t sSeenSerial = 0;
    uint16_t sEpoch = 0;            // the host epoch we know (a migration: the new host gets our BUBBLE again)
    bool sEpochKnown = false;
    Restart sRestart;
    Table sTable;
    RestartGate sGate;
    bool sHolder = false;           // this scene has a BigBubbleHolder of ours (or the stage's own)
    bool sSceneAlive = false;
    bool sBypass = false;
    bool sLifeNextScene = false;    // out of lives when the last scene ended: 1 life at the next one
    uint32_t sTurn = 0;
    uint32_t sCamCutTurn = 0;   // sTurn of our last camera cut (catchUpCamera)
    bool sCamCutUseless = false;   // review: a float cut left the camera still far (a fixed or wide game camera): no more float cuts this bubble
    uint32_t sPopTurn = 0;      // sTurn of the last pop (0 none): a death queued before it is stale (interceptDeath)
    LoadingWait sLoadWait;   // glitchfix G8
    LoadingWait sWaitLoad;   // uxwait: the same 40 s window, on every console (sLoadWait is stepped by the host only)
    PoseFreshness sFresh;   // gamefix: when each slot's pose last changed (noteFreshness, every turn)
    bool sWasActive = false;

    bool together() { return (mp_active() && mp_mode() == MP_MODE_TOGETHER) || MpDemo::live(); }   // (MP3 demo: the puppet demo has the bubble holder and the remote bubbles too)
    bool togetherHeld() { return MpGame::heldMode() == MP_MODE_TOGETHER || MpDemo::live(); }
    bool bubblesOn() { return together() && mp_settings()->downed == MP_DOWNED_BUBBLES; }
    uint16_t serialNow() { return mp_scene_serial(); }

    V3 toV(const TVec3f& v) { return v3(v.x, v.y, v.z); }
    TVec3f toT(V3 v) { return TVec3f(v.x, v.y, v.z); }

    MarioActor* player() { return MR::isExistMario() ? MarioAccess::getPlayerActor() : nullptr; }

    V3 playerUp(MarioActor* a) {
        const TVec3f g = a->mGravity;   // (LiveActor gravity: down)
        return normOr(v3(-g.x, -g.y, -g.z), v3(0.0f, 1.0f, 0.0f));
    }

    // ---- teammates: present, in this scene, playing, not in a bubble --------------------------------------------------------------
    void teammates(Teammate out[MP_MAX_PLAYERS]) {
        const int self = mp_local_slot();
        const uint16_t serial = serialNow();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            out[s] = Teammate{false, v3(0, 0, 0), v3(0, 1, 0), false};
            if (s == self || !mp_slot_present(s)) continue;
            MpPose p;
            if (!MpPuppets::lastPose(s, &p) || p.sceneSerial != serial) continue;
            if (p.flags & (MP_POSE_DEAD | MP_POSE_IN_DEMO | MP_POSE_BUBBLE)) continue;
            MpPuppetBase* pup = MpPuppetsInt::puppet(s);
            if (pup == nullptr || MR::isDead(pup)) continue;
            const V3 feet = toV(pup->mPosition);
            if (!finite3(feet)) continue;
            out[s].valid = true;
            out[s].feet = feet;
            out[s].up = upOfQuat(p.quat);
            out[s].onGround = (p.flags & MP_POSE_ON_GROUND) != 0;
        }
    }

    // Teammates still in this scene and not in a bubble (in a demo, mid-jump, dead for a moment: still "around"). None for
    // kAloneTurns = nobody will come: restart (never stranded).
    // gamefix: every turn, before anything reads a pose: note which slots' poses are still changing.
    void noteFreshness() {
        const int self = mp_local_slot();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            MpPose p;
            if (s != self && mp_slot_present(s) && MpPuppets::lastPose(s, &p)) sFresh.note(s, p.turn, sTurn);
        }
    }
    bool anyoneAround() {
        const int self = mp_local_slot();
        const uint16_t serial = serialNow();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == self || !mp_slot_present(s)) continue;
            MpPose p;
            if (MpPuppets::lastPose(s, &p) && p.sceneSerial == serial && !(p.flags & MP_POSE_BUBBLE) && sFresh.fresh(s, sTurn)) return true;   // (gamefix: not a stalled peer's old pose)
        }
        return false;
    }

    // ---- messages -----------------------------------------------------------------------------------------------------------
    void sendBubble(bool on, int by) {
        MpBubbleMsg m;
        m.serial = sLocal.serial;
        m.on = on ? 1 : 0;
        const int lives = GameDataFunction::getPlayerLeft();
        m.lives = (uint8_t)(lives < 0 ? 0 : (lives > 99 ? 99 : lives));
        m.by = (uint8_t)(by >= 0 && by < MP_MAX_PLAYERS ? by : MP_BUBBLE_BY_NONE);
        m.flags = !on ? 0 : (sLocal.out ? MP_BUBBLE_OUT_OF_LIVES : (sLocal.arrival ? MP_BUBBLE_ARRIVAL : 0));
        uint8_t b[8];
        MpWriter w(b, sizeof b);
        mp_write_bubble(w, m);
        mp_send(MP_MSG_BUBBLE, b, (uint16_t)w.n, true);
        sTable.note(mp_local_slot(), m);
    }

    // ---- the bubble actors --------------------------------------------------------------------------------------------------------
    BigBubble* makeBubble(V3 centre, V3 up) {
        if (!sHolder || !MR::isExistSceneObj(SceneObj_BigBubbleHolder)) return nullptr;
        // generate() grows it from rPos along rUp to its full radius (BigBubble::exeAppear): start one radius below the centre
        const V3 spawn = sub(centre, mul(up, kRadius));
        BigBubble* b = MR::generateBigBubble(nullptr, toT(spawn), toT(up), 1.0f, true, 0, BigBubble::ShapeType_Sphere, -1);
        if (b == nullptr) return nullptr;
        MR::offBind(b);                 // no map collision: it follows a player, not the wind
        MR::invalidateHitSensors(b);    // nobody rides it, pushes it or bursts it with a hip drop: only our rules pop it
        return b;
    }
    // every turn: the appear end validates its sensors again (BigBubble::exeAppear)
    void keepQuiet(BigBubble* b) { MR::invalidateHitSensors(b); }
    void moveBubble(BigBubble* b, V3 centre, V3 vel, int age) {
        if (age < 32) return;   // still growing (BigBubble::sAppearTime = 30): it places itself
        b->mPosition.set(centre.x, centre.y, centre.z);
        // BigBubble::updatePose's own velocity wobble (its velocity stays 0 in the obstruct mode we use)
        V3 d = mul(vel, -1.0f);
        const float l = len(d);
        if (l > 10.0f) d = mul(d, 10.0f / l);
        b->addDeformVelocityOuter(toT(mul(d, 0.1f)), false);
    }
    void popBubble(BigBubble*& b, bool withEffect) {
        if (b != nullptr && !MR::isDead(b)) {
            if (withEffect) b->requestCancelBind();   // the game's own burst: "Break" effect + SE_OJ_BIG_BUBBLE_BREAK, then dead
            else b->kill();
        }
        b = nullptr;
    }

    // ---- the local player ------------------------------------------------------------------------------------------------------------
    void holdPlayer(MarioActor* a) {
        if (!a->_EA4) MarioAccess::readyRemoteDemo();   // (a demo that ended meanwhile gave it back)
        MarioAccess::offControl();
        const char* bck = MarioAccess::getCurrentBckName();
        if (bck == nullptr || strcasecmp(bck, kRiderBck) != 0) {   // (a demo or the game changed it: the capture pose again)
            static int sLogged = 0;
            if (sLogged++ < 3) mp_log("bubble pose restart (was %s)", bck ? bck : "-");
            MR::startBckPlayer(kRiderBck, kRiderBrk);
        }
        if (a->mHealth == 0) a->mHealth = a->mMaxHealth;
        a->_390 = 0;   // (a crush in progress: no "uncrush" countdown inside the bubble)
    }
    // The bubble and the pop teleport the player (an abyss fall: to the teammate, across the stage). The game's own miss would restart the
    // scene, and an abyss fall starts the "fall" event camera, which the intercepted death never reaches: the camera stayed where he
    // fell (7600 units away in the owner's run) and followed nothing. Cut it to him, as after a launch star, when it is lost.
    void catchUpCamera(V3 p, const char* why) {
        // (review: resetCameraMan restarts whichever camera is current: never an event or demo camera, a cutscene, the star-get demo)
        if (MR::isEventCameraActive() || MR::isDemoActive() || MpGameInternal::sceneState() != MP_SCENE_ACTION) return;
        const float d = len(sub(toV(MR::getCamPos()), p));
        const bool isFloat = why[0] == 'f' && why[1] == 'l';   // "float" (not "fall")
        if (isFloat && sCamCutUseless) return;
        if (isFloat && sCamCutTurn != 0 && d > kCameraLost && sTurn - sCamCutTurn >= (uint32_t)kCameraResetGap) {
            // the last cut did not bring the camera to him: the game's camera sits far on purpose; leave it
            sCamCutUseless = true;
            mp_log("bubble camera stays (dist=%.0f after a cut)", d);
            return;
        }
        if (!cameraNeedsCut(d, sTurn - sCamCutTurn)) return;
        sCamCutTurn = sTurn ? sTurn : 1;
        MR::resetCameraMan();
        mp_log("bubble camera cut why=%s dist=%.0f", why, d);
    }
    void syncMarioCameraState(MarioActor* a, V3 feet, V3 up, bool updateGround) {
        if (a == nullptr) return;
        const TVec3f prevFeet = a->mPosition;
        const TVec3f feetVec = toT(feet);
        const TVec3f upVec = toT(up);
        const TVec3f gravVec(-up.x, -up.y, -up.z);
        a->mPosition = feetVec;
        a->mCameraTrans = feetVec;
        a->mLastMove = feetVec - prevFeet;
        a->mUpVec = upVec;
        a->mGravity = gravVec;
        if (a->mMario != nullptr) {
            a->mMario->mPosition = feetVec;
            a->mMario->mShadowPos = feetVec;
            a->mMario->mGroundPos = feetVec;
            a->mMario->mHeadVec = upVec;
            a->mMario->mAirGravityVec = gravVec;
            a->mMario->mMovementStates._2 = 1;   // wave8: clear isLongDrop() so camera never locks downward
            a->mMario->mMovementStates._B = 0;   // wave8: clear isFastDrop()
            a->mMario->mVerticalSpeed = 0.0f;
            if (a->mMario->mMovementStates._37 || a->mMario->_10._15) {
                a->mMario->_688 = feetVec;       // wave8: 2D / circular-2D plane anchor tracks the bubble
            }
            a->mMario->updateCubeCode();
            if (updateGround) {
                a->mMario->updateGroundInfo();
            }
        }
    }
    void placePlayer(V3 centre, V3 up) {
        const V3 feet = sub(centre, mul(up, kRiderDrop));
        const V3 front = flatten(mul(toV(MR::getCamZdir()), -1.0f), up, v3(0.0f, 0.0f, 1.0f));   // facing the camera
        const V3 side = normOr(v3(up.y * front.z - up.z * front.y, up.z * front.x - up.x * front.z, up.x * front.y - up.y * front.x),
                               v3(1.0f, 0.0f, 0.0f));
        Mtx m;
        m[0][0] = side.x; m[0][1] = up.x; m[0][2] = front.x; m[0][3] = feet.x;
        m[1][0] = side.y; m[1][1] = up.y; m[1][2] = front.y; m[1][3] = feet.y;
        m[2][0] = side.z; m[2][1] = up.z; m[2][2] = front.z; m[2][3] = feet.z;
        MR::setPlayerBaseMtx(m);
        // wave8: MR::setPlayerBaseMtx only writes _EA8/mCameraTrans; Mario::update() still runs under readyRemoteDemo and keeps
        // mMario->mPosition, CubeCameraArea, mShadowPos, and mGroundPos frozen at the death spot unless synced every turn.
        syncMarioCameraState(player(), feet, up, false);
    }

    // ---- the player's inner state: what a death leaves behind ------------------------------------------------------------------
    // A miss normally ends in a scene reload (a fresh Mario). The bubble keeps the same Mario, so whatever the hazard put on him
    // goes here, at the bubble's start and again at the pop: the hazard's status (crush, fire dance / burn, abyss, paralysis,
    // freeze, stun, damage, flip, bury, dark damage), the quicksand / poison sink (mSinkTimer draws him up to 120 units into the
    // floor and keeps the "buried walk" on the lower-body track), the burn after-timer, a press (crush between walls), the crush
    // scale, a black hole's hiding. Swimming, hanging and the forms' own statuses are left to the game.
    bool hazardStatus(u32 id) {
        switch (id) {
        case MarioStatus_Damage: case MarioStatus_Faint: case MarioStatus_Blown: case MarioStatus_FireDamage: case MarioStatus_FireDance:
        case MarioStatus_FireRun: case MarioStatus_Paralyze: case MarioStatus_AbyssDamage: case MarioStatus_Freeze: case MarioStatus_Stun:
        case MarioStatus_Crush: case MarioStatus_Flip: case MarioStatus_Bury: case MarioStatus_Bump: case MarioStatus_DarkDamage:
            return true;
        default:
            return false;
        }
    }
    void cleanPlayer(MarioActor* a, const char* when) {
        Mario* m = a->mMario;
        if (m == nullptr) return;
        const float sy = a->mScale.y;   // (before the statuses close: MarioCrush::close sets the scale back itself)
        char closed[48];
        int n = 0;
        closed[0] = 0;
        MarioState* st = m->_97C;
        for (int guard = 0; st != nullptr && guard < 16; guard++) {
            MarioState* next = st->_8;
            if (hazardStatus(st->mStatusId)) {
                if (n < 40) n += snprintf(closed + n, sizeof closed - n, "%s%x", n ? "," : "", (unsigned)st->mStatusId);
                m->closeStatus(st);
            }
            st = next;
        }
        const unsigned sink = m->mSinkTimer, poison = m->mPoisonTimer, press = a->_390;
        const unsigned burn = m->mFireDamage != nullptr ? m->mFireDamage->_12 : 0;
        const bool hidden = a->_482;
        const bool wasSwim = m->isSwimming();
        if (wasSwim) {
            m->forceExitSwim();   // wave8: water death must exit swim mode so CameraManGame leaves ECategory_Swim
        }
        m->mMovementStates._2 = 1;
        m->mMovementStates._B = 0;
        m->mVerticalSpeed = 0.0f;
        if (sink != 0) {
            m->mSinkTimer = 0;
            // (MarioWalk: leaving the sand puts the walk back on track 1; the timer at 0 would never do it)
            if (a->mMarioAnim != nullptr && a->mMarioAnim->mXanimePlayer != nullptr) {
                a->mMarioAnim->mXanimePlayer->changeTrackAnimation(1, "\x95\xe0\x8d\x73");   // "歩行"
            }
        }
        m->mPoisonTimer = 0;
        if (m->mFireDamage != nullptr) m->mFireDamage->_12 = 0;
        a->_390 = 0;
        a->_39D = 0;
        a->mScale.set(1.0f, 1.0f, 1.0f);
        if (hidden) {
            a->_482 = false;
            MR::showPlayer();
        }
        if (n != 0 || sink != 0 || poison != 0 || burn != 0 || press != 0 || sy != 1.0f || hidden) {
            mp_log("bubble clean at=%s statuses=%s sink=%u poison=%u burn=%u press=%u scale=%.2f hidden=%d", when, n ? closed : "-", sink, poison,
                   burn, press, sy, hidden ? 1 : 0);
        }
    }

    bool isGameOverNerve(MarioActor* a) {
        return a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOver)) || a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss)) ||
               a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss2)) || a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverBlackHole)) ||
               a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverSink)) || a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverFire)) ||
               a->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverNonStop));
    }

    // `loseLife`: a fall (the real decrement); false: an arrival (drop-in, warp) in a bubble beside `wantTarget` (-1 nearest)
    void enterBubble(MarioActor* a, bool farFall, bool loseLife, int wantTarget) {
        // the life: the real decrement (keepLives is bypassed for this one call); none left = out of lives
        const int before = GameDataFunction::getPlayerLeft();
        sLocal.out = loseLife && fallIsOutOfLives(before);
        if (loseLife && !sLocal.out) {
            sBypass = true;
            MR::decPlayerLeft();
            sBypass = false;
        }
        // a fall loses the Spring / Boo form, as the miss's reload would (their own holds would fight the bubble's)
        if (loseLife && (a->mPlayerMode == PlayerMode_Hopper || a->mPlayerMode == PlayerMode_Teresa)) {
            mp_log("bubble form reset mode=%u", (unsigned)a->mPlayerMode);
            a->setPlayerMode(PlayerMode_Normal, true);
        }
        cleanPlayer(a, "enter");
        a->setNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
        a->mHealth = a->mMaxHealth;
        MarioAccess::readyRemoteDemo();
        MarioAccess::offControl();
        MR::startBckPlayer(kRiderBck, kRiderBrk);

        const V3 up = playerUp(a);
        V3 centre = add(toV(a->mPosition), mul(up, kRiderDrop));
        Teammate t[MP_MAX_PLAYERS];
        teammates(t);
        const int near = wantTarget >= 0 && wantTarget < MP_MAX_PLAYERS && t[wantTarget].valid ? wantTarget : nearest(t, centre);
        if (farFall && near >= 0) {
            // an abyss, a black hole, quicksand, an arrival: the bubble appears by the teammate (as in NSMB), not down there
            centre = hoverPoint(t[near], centre, toV(MR::getCamXdir()));
        }
        sLocal.on = true;
        sLocal.arrival = !loseLife;
        sLocal.arriveTarget = loseLife ? -1 : near;
        sLocal.grace = loseLife ? kPopGraceTurns : kArrivalGraceTurns;
        sLocal.bubble = makeBubble(centre, near >= 0 && farFall ? t[near].up : up);
        sLocal.pos = centre;
        sLocal.vel = v3(0, 0, 0);
        sLocal.drift = v3(0, 0, 0);
        sLocal.up = near >= 0 && farFall ? t[near].up : up;
        sLocal.age = 0;
        sLocal.target = near;
        sLocal.alone = 0;
        sLocal.hoverLogged = false;
        sLocal.serial = serialNow();
        sCheck.active = false;
        sCamCutUseless = false;
        placePlayer(centre, sLocal.up);
        // wave8: when MpSpectate's smooth bubble-follow camera ("MpWatch") is available, let it take over cleanly on this turn;
        // otherwise (e.g. SMG_MP_SPECTATE=0) fall back to resetting CameraManGame now that Mario's position/CubeCamera are synced.
        if (!MpSpectate::bubbleCamAvailable()) {
            if (sLocal.arrival) {
                MR::resetCameraMan();
                sCamCutTurn = sTurn ? sTurn : 1;
            } else if (farFall) {
                sCamCutTurn = 0;   // (an abyss fall: always cut when the camera is lost, whatever the last cut)
                catchUpCamera(centre, "fall");
            }
        }
        if (sLocal.bubble != nullptr) MR::startSound(sLocal.bubble, "SE_OJ_BIG_BUBBLE_MARIO_IN");
        sendBubble(true, -1);
        if (loseLife) MpUi::toast(sLocal.out ? MP_T_OUT_OF_LIVES : MP_T_BUBBLED, mp_local_slot(), 0);
        mp_log("bubble on slot=%d lives=%d->%d out=%d far=%d target=%d serial=%d%s", mp_local_slot(), before, GameDataFunction::getPlayerLeft(),
               sLocal.out ? 1 : 0, farFall ? 1 : 0, near, sLocal.serial, sLocal.arrival ? " arrival=1" : "");
    }

    // `by`: the teammate whose touch popped it (-1 own A / other); `landBy`: the teammate to land beside (-1: where it is)
    void leaveBubble(int by, int landBy, bool effect, const char* why) {
        if (!sLocal.on) return;
        MarioActor* a = player();
        const bool wasOut = sLocal.out;
        V3 land = sub(sLocal.pos, mul(sLocal.up, kRiderDrop));
        V3 up = sLocal.up;
        if (landBy >= 0) {
            Teammate t[MP_MAX_PLAYERS];
            teammates(t);
            if (t[landBy].valid) {
                land = landingPoint(t[landBy], sLocal.pos, toV(MR::getCamXdir()));
                up = t[landBy].up;
            }
        }
        popBubble(sLocal.bubble, effect);
        sLocal.on = false;
        const bool wasArrival = sLocal.arrival;
        if (a != nullptr) {
            cleanPlayer(a, "pop");
            MarioAccess::endRemoteDemo(nullptr);   // control, sensors and the normal animation back, as after a demo
            MarioAccess::onControl(false);
            MR::setPlayerPos(toT(land));
            syncMarioCameraState(a, land, up, true);
            a->mHealth = a->mMaxHealth;
            a->mMario->mDamage->_16 = kInvincibleTurns;   // the damage blink: 2 s untouchable
            if (effect) MarioAccess::freeJump(toT(mul(up, 18.0f)), 0);   // a little bounce out of the pop
            sCheck = Check{true, 0, land};
            // a death the game queued this turn (DeathArea under the bubble) must not fire at the landing place
            if (isGameOverNerve(a)) {
                a->setNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
                cleanPlayer(a, "pop-stale");
            }
            sPopTurn = sTurn ? sTurn : 1;
            sCamCutTurn = 0;
            sCamCutUseless = false;
            const bool hadBubbleCam = MpSpectate::onLocalBubbleLeave();
            if (!hadBubbleCam) {
                catchUpCamera(land, "pop");
            }
        }
        if (wasOut) {
            const int lives = GameDataFunction::getPlayerLeft();
            if (livesAfterPop(lives, true) != lives) GameDataFunction::addPlayerLeft(livesAfterPop(lives, true) - lives);
        }
        sLocal.out = false;
        sLocal.arrival = false;
        sLocal.arriveTarget = -1;
        if (mp_active()) sendBubble(false, by);
        if (by >= 0) MpUi::toast(MP_T_POPPED, by, (uint32_t)mp_local_slot());
        mp_log("bubble off slot=%d by=%d land=%d why=%s lives=%d%s", mp_local_slot(), by, landBy, why, GameDataFunction::getPlayerLeft(),
               wasArrival ? " arrival=1" : "");
    }

    // The player's state after a pop, twice (0.5 s and 2.5 s): the status, the hold, the sink / burn / press / scale left behind,
    // the animation, the camera's distance and how far he moved (the run's stick input walks him). "ok=1": nothing left behind.
    void checkStep() {
        if (!sCheck.active) return;
        MarioActor* a = player();
        if (a == nullptr || sLocal.on) { sCheck.active = false; return; }
        ++sCheck.turns;
        if (sCheck.turns != 30 && sCheck.turns != 150) return;
        Mario* m = a->mMario;
        const u32 status = m->getCurrentStatus();
        const bool wait = a->isNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
        const bool ctrl = !MarioAccess::isOffControl() && !a->_EA4;
        const bool bind = MR::isPlayerInBind();
        const bool hidden = MR::isPlayerHidden();
        const unsigned burn = m->mFireDamage != nullptr ? m->mFireDamage->_12 : 0;
        const char* bck = MarioAccess::getCurrentBckName();
        const float cam = len(sub(toV(MR::getCamPos()), toV(a->mPosition)));
        const float moved = len(sub(toV(a->mPosition), sCheck.at));
        const bool clean = !hazardStatus(status) && wait && ctrl && !bind && !hidden && m->mSinkTimer == 0 && m->mPoisonTimer == 0 && burn == 0 &&
                           a->_390 == 0 && a->mScale.y == 1.0f && (bck == nullptr || strcasecmp(bck, kRiderBck) != 0) && cam < 3000.0f;
        mp_log("bubble check t=%d ok=%d status=%x wait=%d ctrl=%d bind=%d hidden=%d sink=%u poison=%u burn=%u press=%u scale=%.2f bck=%s "
               "cam=%.0f moved=%.0f ground=%d health=%d", sCheck.turns, clean ? 1 : 0, (unsigned)status, wait ? 1 : 0, ctrl ? 1 : 0, bind ? 1 : 0,
               hidden ? 1 : 0, (unsigned)m->mSinkTimer, (unsigned)m->mPoisonTimer, burn, (unsigned)a->_390, a->mScale.y, bck ? bck : "-", cam,
               moved, MR::isOnGroundPlayer() ? 1 : 0, (int)a->mHealth);
        if (sCheck.turns >= 150) sCheck.active = false;
    }

    // ---- pop requests (spin, Star Bit): the popper's side --------------------------------------------------------------------------
    const char* causeName(uint8_t c) { return c == kPopSpin ? "spin" : (c == kPopShot ? "starbit" : "?"); }
    bool sendPopRequest(int target, uint8_t cause) {
        Remote& r = sRemote[target];
        if (r.lastAsk != 0 && sTurn - r.lastAsk < (uint32_t)kPopRequestCooldown) return false;
        r.lastAsk = sTurn ? sTurn : 1;
        uint8_t b[8];
        MpWriter w(b, sizeof b);
        const bool ok = writePopRequest(w, (uint8_t)target, cause) && mp_send(MP_MSG_BUMP, b, (uint16_t)w.n, true);
        mp_log("bubble pop request to=%d cause=%s ok=%d", target, causeName(cause), ok ? 1 : 0);
        return ok;
    }
    // Our spin within reach of a teammate's bubble (as this console shows it).
    void spinStep() {
        if (sLocal.on || MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isPlayerDead() || MR::isDemoActive()) return;
        if (!MR::isPlayerSwingAction()) return;
        MarioActor* a = player();
        if (a == nullptr) return;
        const V3 body = add(toV(a->mPosition), mul(playerUp(a), kBodyUp));
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            Remote& r = sRemote[s];
            if (r.bubble == nullptr || MR::isDead(r.bubble)) continue;
            if (spinReaches(body, r.last, r.bubble->getSize())) sendPopRequest(s, kPopSpin);
        }
    }

    // The local player's turn in the bubble.
    void localStep() {
        MarioActor* a = player();
        if (a == nullptr) { sLocal = Local{}; return; }
        ++sLocal.age;
        if (sLocal.bubble != nullptr && MR::isDead(sLocal.bubble)) sLocal.bubble = nullptr;   // (taken back by the stage's own use)
        if (sLocal.bubble == nullptr && sLocal.age > 1 && sHolder) {
            sLocal.bubble = makeBubble(sLocal.pos, sLocal.up);
        }
        if (sLocal.bubble != nullptr) keepQuiet(sLocal.bubble);
        holdPlayer(a);

        Teammate t[MP_MAX_PLAYERS];
        teammates(t);
        const bool keepTarget = sLocal.arrival && sLocal.arriveTarget >= 0 && sLocal.arriveTarget < MP_MAX_PLAYERS && t[sLocal.arriveTarget].valid;
        const int near = keepTarget ? sLocal.arriveTarget : nearest(t, sLocal.pos);
        sLocal.target = near;
        sLocal.alone = anyoneAround() ? 0 : sLocal.alone + 1;
        if (near >= 0 && !sLocal.hoverLogged && len(sub(sLocal.pos, t[near].feet)) < 450.0f) {
            sLocal.hoverLogged = true;
            mp_log("bubble hover slot=%d by=%d dist=%.0f age=%d", mp_local_slot(), near, len(sub(sLocal.pos, t[near].feet)), sLocal.age);
        }
        if (sLocal.arrival) {
            // an arrival pops by itself (beside its friend), at once when the friend is gone; never a restart
            if (arrivalDone(sLocal.age) || (near < 0 && sLocal.age > 30)) {
                leaveBubble(-1, near, true, near >= 0 ? "arrival" : "arrival alone");
                return;
            }
        } else if (sLocal.alone >= kAloneTurns && !sRestart.active) {
            startRestart("alone");   // nobody to float to (the others gone, elsewhere or all caught without a host here)
        }

        // pops: a teammate's touch; our own A next to a teammate standing on the ground
        if (sLocal.age >= sLocal.grace && !sRestart.active) {
            const float r = sLocal.bubble != nullptr ? sLocal.bubble->getSize() : kRadius;
            const int toucher = touchPops(sLocal.pos, r, t);
            if (toucher >= 0) {
                leaveBubble(sLocal.arrival ? -1 : toucher, toucher, true, "touch");
                return;
            }
            sLocal.canPop = !MpUi::isMenuOpen() && !mp_dialog_active() && !MpSpectate::watching() && !sLocal.arrival &&
                            mayPopWithA(true, sLocal.out, sLocal.age, sLocal.pos, t) >= 0;   // MP3 ingame: the prompt
            const bool a_ = !MpUi::isMenuOpen() && !mp_dialog_active() && MR::testCorePadTriggerA(WPAD_CHAN0);
            const int beside = mayPopWithA(a_, sLocal.out, sLocal.age, sLocal.pos, t);
            if (beside >= 0) {
                leaveBubble(-1, beside, true, "button");
                return;
            }
        }

        // the float: to the teammate, the stick drifts it, a gentle bob
        V3 goal = sLocal.pos;
        if (near >= 0) {
            goal = hoverPoint(t[near], sLocal.pos, toV(MR::getCamXdir()));
            sLocal.up = normOr(add(mul(sLocal.up, 0.9f), mul(t[near].up, 0.1f)), t[near].up);   // turn with his gravity, smoothly
        }
#if defined(TARGET_PC) && !defined(__vita__)
        static int sNear = -1;   // (desktop test: SMG_MP_TEST_BUBBLE_NEAR=1 parks the bubble on the teammate: the touch pop runs)
        if (sNear < 0) { const char* e = getenv("SMG_MP_TEST_BUBBLE_NEAR"); sNear = (e && *e == '1') ? 1 : 0; }
        if (sNear && near >= 0 && sLocal.age > 90) goal = add(t[near].feet, mul(t[near].up, kBodyUp));
#endif
        const bool input = !MpUi::isMenuOpen() && !mp_dialog_active() && !MpSpectate::watching();   // (spectate: the stick does not steer a bubble nobody sees)
        const V3 want = input ? driftOffset(MR::getSubPadStickX(WPAD_CHAN0), MR::getSubPadStickY(WPAD_CHAN0), toV(MR::getCamXdir()),
                                            toV(MR::getCamZdir()), sLocal.up)
                              : v3(0, 0, 0);
        sLocal.drift = add(mul(sLocal.drift, 0.92f), mul(want, 0.08f));
#if defined(TARGET_PC) && !defined(__vita__)
        if (!(sNear && near >= 0 && sLocal.age > 90)) goal = add(goal, sLocal.drift);   // (the parked test bubble ignores the stick)
#else
        goal = add(goal, sLocal.drift);
#endif
        if (sLocal.age < 32 && sLocal.bubble != nullptr) {
            sLocal.pos = toV(sLocal.bubble->mPosition);   // growing: it rises by itself
        } else {
            sLocal.pos = stepFloat(sLocal.pos, &sLocal.vel, goal, sLocal.up, sTurn);
        }
        if (sLocal.bubble != nullptr) moveBubble(sLocal.bubble, sLocal.pos, sLocal.vel, sLocal.age);
        placePlayer(sLocal.pos, sLocal.up);
        catchUpCamera(sLocal.pos, "float");   // (the camera lost behind the bubble's long way back)
    }

    // ---- remote players' bubbles (from their STATE flag) --------------------------------------------------------------------------
    void remoteStep() {
        const int self = mp_local_slot();
        const uint16_t serial = serialNow();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            Remote& r = sRemote[s];
            if (r.bubble != nullptr && MR::isDead(r.bubble)) r.bubble = nullptr;
            bool want = false, popped = false;
            MpPose p;
            MpPuppetBase* pup = nullptr;
            if (s != self && mp_slot_present(s) && MpPuppets::lastPose(s, &p) && p.sceneSerial == serial) {
                pup = MpPuppetsInt::puppet(s);
                if (pup != nullptr && !MR::isDead(pup)) {
                    want = (p.flags & MP_POSE_BUBBLE) != 0;
                    popped = !want;
                }
            }
            if (!want) {
                if (r.bubble != nullptr) {
                    mp_log("bubble remote off slot=%d popped=%d", s, popped ? 1 : 0);
                    popBubble(r.bubble, popped);
                }
                r.age = 0;
                continue;
            }
            const V3 up = upOfQuat(p.quat);
            const V3 centre = add(toV(pup->mPosition), mul(up, kRiderDrop));
            if (r.bubble == nullptr) {
                r.bubble = makeBubble(centre, up);
                r.age = 0;
                r.last = centre;
                if (r.bubble != nullptr) mp_log("bubble remote on slot=%d", s);
                if (r.bubble == nullptr) continue;
            }
            ++r.age;
            keepQuiet(r.bubble);
            moveBubble(r.bubble, centre, sub(centre, r.last), r.age);
            r.last = centre;
        }
    }

    // ---- everyone caught: the host decides, every console restarts -------------------------------------------------------------
    void startRestart(const char* why) {
        if (sRestart.active) return;
        sRestart = Restart{true, 0, 0};
        if (!sLocal.on) MR::offPlayerControl();
        MR::startDownWipe();   // the miss sequence's own wipe (EventSequence::doCloseWipe)
        MpUi::toast(MP_T_ALL_CAUGHT, -1, 0);
        MpWorldSync::onRestartMessage();   // hostreload: everyone restarts from the checkpoint: drop carry, pending, early and held world messages on both host and guest
        mp_log("bubble restart start serial=%d why=%s", serialNow(), why);
    }
    void restartStep() {
        if (!sRestart.active) return;
        ++sRestart.turns;
        if (sRestart.phase == 0) {
            if ((sRestart.turns > 10 && !MR::isWipeActive()) || sRestart.turns > 300) {
                sRestart.phase = 1;
                sRestart.turns = 0;
            }
            return;
        }
        if (sRestart.turns == 30) {   // EventSequence::doWaitAfterWipe
            mp_log("bubble restart reload serial=%d", serialNow());
            MR::requestEndMissDemo();   // the game's own reload after a miss (local move type 5; GameScene: comet retry or reload)
        }
    }
    // ---- late joiners and a new host: they get our BUBBLE again ---------------------------------------------------------------
    // The host relays every BUBBLE to everyone present, so a new host after a migration already has the table; a console that was
    // not there (a late joiner, a host that joined after our fall) has only our pose. While we float, a teammate newly seen in this
    // scene and a host change get our BUBBLE again (the everyone-caught test needs message + pose), and the new host's restart
    // debounce starts from zero.
    void announceStep() {
        const uint16_t serial = serialNow();
        if (sSeenSerial != serial) {
            sSeenSerial = serial;
            sSeenMask = 0;
        }
        const int self = mp_local_slot();
        uint8_t mask = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            MpPose p;
            if (s != self && mp_slot_present(s) && MpPuppets::lastPose(s, &p) && p.sceneSerial == serial) mask |= (uint8_t)(1u << s);
        }
        const uint8_t fresh = (uint8_t)(mask & ~sSeenMask);
        sSeenMask = mask;
        const uint16_t epoch = mp_host_epoch();
        const bool hostChanged = sEpochKnown && epoch != sEpoch;
        sEpoch = epoch;
        sEpochKnown = true;
        if (hostChanged) {
            sGate.reset();
            mp_log("bubble host now slot=%d epoch=%d me=%d bubbled=%d", mp_host_slot(), (int)epoch, self, sLocal.on ? 1 : 0);
        }
        if (sLocal.on && sLocal.serial == serial && (hostChanged || fresh != 0)) {
            sendBubble(true, -1);
            mp_log("bubble reannounce why=%s mask=%x", hostChanged ? "host" : "seen", (unsigned)fresh);
        }
    }

    // uxwait: bubbled, and a teammate's galaxy is still loading (the group is not restarted under them, up to 40 s): after 2 s a small
    // banner names the first one. It goes when the bubble pops, the restart starts, or nobody is loading any more.
    MpWaitLogic::Tracker sWaitTracker;
    bool sWaitUp = false;
    void waitBannerStep(bool clear) {
        int who = -1;
        if (!clear && sLocal.on && !sRestart.active && mp_active()) {
            const int self = mp_local_slot();
            const uint16_t serial = serialNow();
            bool loading[MP_MAX_PLAYERS] = {};
            bool inSceneFree[MP_MAX_PLAYERS] = {};
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                if (s == self || !mp_slot_present(s)) { sWaitLoad.step(s, false, sTurn); continue; }
                MpPose lp;
                const bool have = MpPuppets::lastPose(s, &lp);
                const bool stalled = have && !sFresh.fresh(s, sTurn);
                loading[s] = sWaitLoad.step(s, !stalled && (!have || lp.sceneSerial != serial), sTurn);
                inSceneFree[s] = have && !stalled && lp.sceneSerial == serial && !(lp.flags & MP_POSE_BUBBLE);
            }
            who = MpWaitLogic::bubbleWaitSlot(loading, inSceneFree, MP_MAX_PLAYERS);
        } else {
            sWaitLoad.reset();
        }
        const MpWaitLogic::Out o = sWaitTracker.step(who >= 0, who, static_cast<int64_t>(sTurn) * 1000 / 60);
        if (o.show || sWaitUp) MpUi::waitBanner(o.show ? o.slot : -1);
        sWaitUp = o.show;
    }

    void hostStep() {
        if (!mp_is_host() || sRestart.active) { sGate.reset(); return; }
        const int self = mp_local_slot();
        const uint16_t serial = serialNow();
        SlotView v[MP_MAX_PLAYERS];
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            v[s] = SlotView{mp_slot_present(s) || s == self, false, false};
            if (s == self) {
                v[s].inScene = true;
                v[s].poseBubble = sLocal.on;
                continue;
            }
            MpPose p;
            if (v[s].present && MpPuppets::lastPose(s, &p) && p.sceneSerial == serial && sFresh.fresh(s, sTurn)) {   // (gamefix: a stalled peer's old pose is not in the scene)
                v[s].inScene = true;
                v[s].poseBubble = (p.flags & MP_POSE_BUBBLE) != 0;
            }
            if (v[s].present && !v[s].inScene) {   // glitchfix G8: still loading (no pose yet, or a pose of another serial that is not stalled)
                MpPose lp;
                const bool have = MpPuppets::lastPose(s, &lp);
                const bool stalled = have && !sFresh.fresh(s, sTurn);
                v[s].loading = sLoadWait.step(s, !stalled && (!have || lp.sceneSerial != serial), sTurn);
            } else {
                sLoadWait.step(s, false, sTurn);
            }
        }
        if (sGate.step(everyoneCaught(sTable, v))) {
            uint8_t b[2];
            MpWriter w(b, sizeof b);
            w.u16(serial);
            mp_send(MP_MSG_RESTART, b, (uint16_t)w.n, true);
            mp_log("bubble restart sent serial=%d host=%d epoch=%d", serial, self, (int)mp_host_epoch());
            sTable.reset(serial);
            startRestart("host");
        }
    }

    // ---- desktop tests -------------------------------------------------------------------------------------------------------
    // SMG_MP_TEST_DIE="<secs>[:<slot>][:<kind>][,...]": <secs> after the start of each scene serial (game clock), the local player
    // of that slot (any when no slot) dies through the game's own entry point of a hazard (kind); up to 8 entries, each once per
    // scene serial (one at a time: an entry waits while he floats in a bubble):
    //   damage     forceKill 3 (a damage death)          abyss  forceKill 0 = DeathArea's forceKillPlayerByAbyss (doAbyssDamage)
    //   fire       1 health, doFireDanceWithInitialDamage(1) = a lava / fire floor (Mario::damageFloorCheck): fire dance, NonStop
    //   crush      1 health, requestCrush = ACTMES_ENEMY_ATTACK_CRUSH (a crusher): MarioCrush (flattened), game over
    //   press      setPress(1) = squeezed by walls (Mario::checkPressDamage): the press hold, game over 60 turns later
    //   sink       quicksand as MarioWalk leaves it at the end (sink timer 255, the buried walk on track 1, floor code Sink) and
    //              forceGameOverSink (a real sand floor needs a sand stage; the state on Mario is the same)
    //   blackhole  ACTMES_INHALE_BLACK_HOLE's handling (mBlackHolePosition below him, forceGameOverBlackHole); only when the
    //              bubble will take it (else the black hole's demo would need a real BlackHole)
    // SMG_MP_TEST_LIVES=<n> sets his lives just before (0: out of lives). SMG_MP_TEST_SHOOT=<secs>: from <secs> after each scene
    // start, a real Star Bit (StarPieceShooter's throw) is thrown at a teammate's bubble every 2 s while one floats here.
    // SMG_MP_TEST_WARP=<secs>[:<slot>]: the session menu's "Warp to <slot>" (default 0) once per scene serial.
    // Desktop only; nothing without the env.
#if defined(TARGET_PC) && !defined(__vita__)
    enum TestKind { kDieDamage, kDieAbyss, kDieFire, kDieCrush, kDiePress, kDieSink, kDieBlackHole };
    const char* const kTestKindName[] = {"damage", "abyss", "fire", "crush", "press", "sink", "blackhole"};
    bool testWillBubble() {
        bool others = false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) others = others || (s != mp_local_slot() && mp_slot_present(s));
        return bubblesOn() && sSceneAlive && sHolder && others && !sRestart.active;
    }
    void testKill(MarioActor* a, int kind) {
        Mario* m = a->mMario;
        switch (kind) {
        case kDieAbyss: MarioAccess::forceKill(0, 0); break;
        case kDieFire:
            a->mHealth = 1;
            if (!m->doFireDanceWithInitialDamage(1)) mp_log("bubble test die: fire refused");
            break;
        case kDieCrush:
            a->mHealth = 1;
            m->requestCrush();   // (Mario::checkDamage runs tryCrush on his next turn)
            break;
        case kDiePress:
            a->setPress(1, 0);
            break;
        case kDieSink:
            m->mSinkTimer = 255;
            m->_960 = CollisionFloorCode_Sink;
            if (a->mMarioAnim != nullptr && a->mMarioAnim->mXanimePlayer != nullptr) {
                a->mMarioAnim->mXanimePlayer->changeTrackAnimation(1, "\x96\x84\x82\xdc\x82\xe8\x95\xe0\x8d\x73");   // "埋まり歩行"
            }
            a->forceGameOverSink();
            break;
        case kDieBlackHole: {
            if (!testWillBubble()) { mp_log("bubble test die: blackhole skipped (no bubble would take it)"); break; }
            const V3 hole = sub(toV(a->mPosition), mul(playerUp(a), 1500.0f));
            a->mBlackHolePosition.set(hole.x, hole.y, hole.z);
            a->mBlackHole = nullptr;
            a->forceGameOverBlackHole();
            break;
        }
        default: MarioAccess::forceKill(3, 0); break;
        }
    }
    void testShootStep(int64_t sinceSceneMs) {
        static int sOn = -1;
        static int64_t sFromMs = 0;
        static uint32_t sLast = 0;
        if (sOn < 0) {
            const char* e = getenv("SMG_MP_TEST_SHOOT");
            sOn = (e && *e) ? 1 : 0;
            if (sOn) sFromMs = (int64_t)(atof(e) * 1000.0);
        }
        if (!sOn || sinceSceneMs < sFromMs || sLocal.on || (sLast != 0 && sTurn - sLast < 120)) return;
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isPlayerDead() || MR::isDemoActive()) return;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            Remote& r = sRemote[s];
            if (r.bubble == nullptr || MR::isDead(r.bubble) || r.age < kPopGraceTurns + 10) continue;
            StarPiece* piece = MR::getDeadStarPiece();
            if (piece == nullptr) { mp_log("bubble test shoot: no star bit actor"); sLast = sTurn ? sTurn : 1; return; }
            // StarPieceShooter::shoot: from the camera-side shooting point to the target, the player's gravity
            const TVec3f from = MR::getStarPieceDirector()->calcPosCameraShoot(0);
            TVec3f grav(*MR::getPlayerGravity());
            piece->throwToTargetCore(toT(r.last), from, grav, 0.0f, true);
            sLast = sTurn ? sTurn : 1;
            mp_log("bubble test shoot at=%d from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f", s, from.x, from.y, from.z, r.last.x, r.last.y, r.last.z);
            return;
        }
    }
#endif
    struct TestDie {
        int64_t atMs;
        int slot;
        int kind;
        bool done;
    };
    void testDieStep() {
#if defined(TARGET_PC) && !defined(__vita__)
        static int sCount = -1;
        static TestDie sDie[8];
        static int64_t sWarpMs = -1;
        static int sWarpSlot = 0;
        static bool sWarpDone = false;
        if (sCount < 0) {
            sCount = 0;
            const char* e = getenv("SMG_MP_TEST_DIE");
            for (const char* p = e; p != nullptr && *p && sCount < 8;) {   // "<secs>[:<slot>][:<kind>]" entries, ',' between
                TestDie d{(int64_t)(atof(p) * 1000.0), -1, kDieDamage, false};
                const char* stop = strchr(p, ',');
                const char* c = strchr(p, ':');
                if (c != nullptr && (stop == nullptr || c < stop) && c[1] >= '0' && c[1] <= '9') {
                    d.slot = atoi(c + 1);
                    c = strchr(c + 1, ':');
                }
                if (c != nullptr && (stop == nullptr || c < stop)) {
                    for (int k = 0; k <= kDieBlackHole; k++) {
                        const size_t n = strlen(kTestKindName[k]);
                        if (strncmp(c + 1, kTestKindName[k], n) == 0 && (c[1 + n] == 0 || c[1 + n] == ',')) d.kind = k;
                    }
                }
                sDie[sCount++] = d;
                mp_log("bubble test die: %ld ms after each scene start, slot %d, %s", (long)d.atMs, d.slot, kTestKindName[d.kind]);
                p = stop != nullptr ? stop + 1 : nullptr;
            }
            if (const char* w = getenv("SMG_MP_TEST_WARP")) {   // "<secs>[:<slot>]": the session menu's "Warp to" (MpFlowHooks)
                if (*w) {
                    sWarpMs = (int64_t)(atof(w) * 1000.0);
                    const char* c = strchr(w, ':');
                    sWarpSlot = c != nullptr ? atoi(c + 1) : 0;
                }
            }
        }
        static int sSerial = -1;
        static int64_t sStartMs = 0;
        if (!MpGameInternal::sceneExists() || MR::isStageFileSelect()) return;
        const int64_t t = MpGameInternal::nowMs();
        const int serial = serialNow();
        if (serial != sSerial) {
            sSerial = serial;
            sStartMs = t;
            for (int i = 0; i < sCount; i++) sDie[i].done = false;
            sWarpDone = false;
        }
        if (sSceneAlive && together()) testShootStep(t - sStartMs);
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || !MR::isExistMario() || MR::isPlayerDead() || MR::isDemoActive() ||
            sLocal.on)
            return;
        if (sWarpMs >= 0 && !sWarpDone && t - sStartMs >= sWarpMs && sSceneAlive && together()) {
            sWarpDone = true;
            if (sWarpSlot != mp_local_slot()) {
                mp_log("bubble test warp to=%d", sWarpSlot);
                MpFlowHooks::requestWarpTo(sWarpSlot);
            }
        }
        MarioActor* a = player();
        if (a == nullptr) return;
        for (int i = 0; i < sCount; i++) {
            TestDie& d = sDie[i];
            if (d.done || t - sStartMs < d.atMs) continue;
            if (d.kind == kDiePress && !MR::isOnGroundPlayer()) return;   // (a press needs the floor under him)
            d.done = true;
            if (d.slot >= 0 && mp_local_slot() != d.slot) continue;
            if (const char* l = getenv("SMG_MP_TEST_LIVES")) {   // (desktop test: the lives just before the fall, e.g. 0 = out of lives)
                const int want = atoi(l);
                if (*l && want >= 0 && want <= 99) GameDataFunction::addPlayerLeft(want - GameDataFunction::getPlayerLeft());
            }
            mp_log("bubble test die: slot %d serial=%d %s lives=%d", mp_local_slot(), serial, kTestKindName[d.kind], GameDataFunction::getPlayerLeft());
            testKill(a, d.kind);
            return;
        }
#endif
    }

    // the drop-in arrival (MpDropInHooks): arrive in a bubble beside the teammate the drop-in picked
    bool dropInArrival(int targetSlot) { return MpBubble::arriveInBubble("dropin", targetSlot); }
}  // namespace

namespace {
    // MP3 ingame: the button picture over the bubble while the player's own press would pop it
    void promptStep() {
        bool want = sLocal.on && sLocal.bubble != nullptr && MpGameInternal::sceneState() == MP_SCENE_ACTION;
        if (want && sLocal.canPop) sPromptHold = 30;
        else if (sPromptHold > 0) --sPromptHold;
        want = want && sPromptHold > 0;
        bool placed = false;
        if (want) {
            TVec3f top(sLocal.pos.x + sLocal.up.x * kRadius, sLocal.pos.y + sLocal.up.y * kRadius, sLocal.pos.z + sLocal.up.z * kRadius);
            TVec3f view;
            MR::getCameraViewMtx().mult(top, view);
            if (view.z < 0.0f) {
                TVec3f scr;
                MR::calcScreenPosition(&scr, top);
                const MpPanel::Screen ps = MpPanelDraw::screen();
                const f32 sw = (f32)MR::getScreenWidth();
                sPromptX = sw > 0.0f ? scr.x * (ps.width / sw) : scr.x;
                sPromptY = scr.y - 16.0f;
                placed = true;
            }
        }
        const bool on = want && placed;
        sPromptA += on ? 0.2f : -0.2f;
        sPromptA = sPromptA < 0.0f ? 0.0f : (sPromptA > 1.0f ? 1.0f : sPromptA);
        if (on != sPromptOn) {
            sPromptOn = on;
            mp_log("bubble prompt %s", on ? "on" : "off");
        }
    }
}  // namespace

namespace MpBubble {
    bool promptModel(MpPanel::Prompt* out) {   // MP3 ingame
        if (out == nullptr || sPromptA <= 0.0f || !mp_active()) return false;
        out->x = sPromptX;
        out->y = sPromptY;
        out->alpha = sPromptA;
        return true;
    }

    bool bypassKeepLives() { return sBypass; }
    int localWait(int* target, float* distance) {   // MP3 spectate
        if (target) *target = -1;
        if (distance) *distance = -1.0f;
        if (!sLocal.on) return 0;
        if (sLocal.arrival) return 3;
        if (target) *target = sLocal.target;
        if (distance && sLocal.target >= 0) {
            MpPuppetBase* pup = MpPuppetsInt::puppet(sLocal.target);
            if (pup != nullptr && !MR::isDead(pup)) *distance = len(sub(sLocal.pos, toV(pup->mPosition)));
        }
        return sLocal.out ? 2 : 1;
    }

    bool localBubbled() { return sLocal.on; }
    bool localPose(float pos[3], float up[3], float vel[3]) {
        if (!sLocal.on) return false;
        const V3 feet = sub(sLocal.pos, mul(sLocal.up, kRiderDrop));
        if (!finite3(feet) || !finite3(sLocal.up)) return false;
        if (pos != nullptr) { pos[0] = feet.x; pos[1] = feet.y; pos[2] = feet.z; }
        if (up != nullptr) { up[0] = sLocal.up.x; up[1] = sLocal.up.y; up[2] = sLocal.up.z; }
        if (vel != nullptr) {
            const V3 v = finite3(sLocal.vel) ? sLocal.vel : v3(0.0f, 0.0f, 0.0f);
            vel[0] = v.x; vel[1] = v.y; vel[2] = v.z;
        }
        return true;
    }

    bool interceptDeath(MarioActor* actor) {
        if (!MpGame::sessionHeld() || actor == nullptr || !MR::isFirstStep(actor)) return false;
        if (sLocal.on) {
            // already in a bubble (a death area under the spawn point, ...): stay in it, no second life
            actor->setNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
            cleanPlayer(actor, "again");
            holdPlayer(actor);
            return true;
        }
        if (!bubblesOn() || !sSceneAlive || MpGameInternal::sceneState() != MP_SCENE_ACTION || sRestart.active) return false;
        if (MR::isDemoActive() || MR::isStageFileSelect()) return false;
        if (actor->_39D == 1 || actor->_39D == 2 || actor->_39D == 5) return false;   // race defeats (forceKill 1, 2, 5)
        if (staleKillAfterPop(sPopTurn != 0, sTurn - sPopTurn) && sCheck.active) {
            // the game's death queued before the pop (see leaveBubble): the old fall, not a new one; no life, no new bubble
            // (review: after the race-defeat, scene and demo gates, so those still reach the game)
            mp_log("bubble stale death ignored (%u turns after the pop)", (unsigned)(sTurn - sPopTurn));
            actor->setNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
            cleanPlayer(actor, "stale");
            if (actor->mHealth == 0) actor->mHealth = actor->mMaxHealth;   // (a one-shot kill zeroed it: nothing would re-kill him)
            return true;
        }
        if (!sHolder) return false;
        bool others = false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) others = others || (s != mp_local_slot() && mp_slot_present(s));
        if (!others) return false;   // alone: the game's own miss
        const bool far = actor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss)) ||
                         actor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss2)) ||
                         actor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverBlackHole)) ||
                         actor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverSink));
        enterBubble(actor, far, true, -1);
        return true;
    }

    bool arriveInBubble(const char* why, int targetSlot) {
        MarioActor* a = player();
        const char* refuse = nullptr;
        if (!together() || a == nullptr) refuse = "session";
        else if (sLocal.on) refuse = "bubbled";
        else if (!sSceneAlive || !sHolder) refuse = "nobubble";
        else if (sRestart.active) refuse = "restart";
        else if (MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isPlayerDead() || MR::isDemoActive() || MR::isPlayerInBind()) refuse = "busy";
        else if (a->mPlayerMode == PlayerMode_Hopper || a->mPlayerMode == PlayerMode_Teresa) refuse = "form";
        int target = -1;
        if (refuse == nullptr) {
            Teammate t[MP_MAX_PLAYERS];
            teammates(t);
            target = targetSlot >= 0 && targetSlot < MP_MAX_PLAYERS && t[targetSlot].valid ? targetSlot : nearest(t, toV(a->mPosition));
            if (target < 0) refuse = "nobody";
        }
        if (refuse != nullptr) {
            mp_log("bubble arrival why=%s to=%d refused reason=%s", why ? why : "-", targetSlot, refuse);
            return false;
        }
        enterBubble(a, true, false, target);
        mp_log("bubble arrival why=%s to=%d", why ? why : "-", target);
        return true;
    }

    bool shotThrough(const float* from, const float* to) {
        if (!together() || !sSceneAlive || sLocal.on || from == nullptr || to == nullptr) return false;
        const V3 a = v3(from[0], from[1], from[2]), b = v3(to[0], to[1], to[2]);
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            Remote& r = sRemote[s];
            if (r.bubble == nullptr || MR::isDead(r.bubble)) continue;
            if (!segmentHitsSphere(a, b, r.last, r.bubble->getSize())) continue;
            mp_log("bubble starbit hit slot=%d", s);
            sendPopRequest(s, kPopShot);
            return true;   // (the Star Bit hits the bubble even within the request cooldown)
        }
        return false;
    }

    bool handlePopRequest(const MpMsg& m) {
        bool ok = false;
        uint8_t target = 0, cause = 0;
        if (!readPopRequest(m.data, m.len, &ok, &target, &cause)) return false;
        if (!mp_active()) return true;
        const int from = m.fromSlot;
        if (!ok || from < 0 || from >= MP_MAX_PLAYERS) {
            mp_log("bubble drop pop request from=%d len=%d (invalid)", from, m.len);
            return true;
        }
        if ((int)target != mp_local_slot()) return true;   // (the host relays it to everyone)
        const char* why = nullptr;
        float dist = -1.0f;
        bool playing = false;
        if (!together() || !sLocal.on) {
            why = "none";
        } else if (from == mp_local_slot()) {
            why = "self";
        } else {
            Teammate t[MP_MAX_PLAYERS];
            teammates(t);
            playing = t[from].valid;
            if (playing) dist = len(sub(add(t[from].feet, mul(t[from].up, kBodyUp)), sLocal.pos));
            const float r = sLocal.bubble != nullptr ? sLocal.bubble->getSize() : kRadius;
            if (sRestart.active) why = "restart";
            else if (!acceptPop(cause, sLocal.age, sLocal.grace, playing, dist, r))
                why = sLocal.age < sLocal.grace ? "grace" : (!playing ? "notplaying" : "far");
        }
        if (why != nullptr) {
            mp_log("bubble pop request from=%d cause=%s result=reject reason=%s dist=%.0f age=%d", from, causeName(cause), why, dist, sLocal.age);
            return true;
        }
        mp_log("bubble pop request from=%d cause=%s result=pop dist=%.0f age=%d", from, causeName(cause), dist, sLocal.age);
        leaveBubble(sLocal.arrival ? -1 : from, from, true, causeName(cause));
        return true;
    }

    void popInPlace(const char* why) {
        if (sLocal.on) leaveBubble(-1, -1, true, why);
    }

    void addArchives(void* collector) {
        if (!(togetherHeld() || MpDemo::mode() == MP_DEMO_PUPPETS) || collector == nullptr) return;
        static_cast< NameObjArchiveListCollector* >(collector)->addArchive(kArchive);
    }

    // MP3 puppetprep (MpWarm): one bubble of the pool appears cleanly inside the frustum at tiny 0.001 scale behind the closed
    // opening wipe for the first pictures of the scene, so its model, textures and shader variants pass StaticRunCulled and warm up.
    // Nobody sees it, nothing hears of it (no bubble table, no BUBBLE, no pop effect): it is killed again.
    BigBubble* sWarm = nullptr;
    bool warmBegin() {
        if (!sSceneAlive || !sHolder || sWarm != nullptr || !MR::isExistMario()) return false;
        const TVec3f cam = MR::getCamPos();
        const TVec3f fwd = MR::getCamZdir();
        const f32 nearZ = MR::getNearZ();
        const f32 dist = (nearZ > 1.0f ? nearZ : 100.0f) + 120.0f;
        const V3 up = v3(0.0f, 1.0f, 0.0f);
        // makeBubble spawns at centre - up * kRadius; add up * kRadius so the bubble sits dead-center inside the frustum
        const V3 pos = v3(cam.x + fwd.x * dist, cam.y + fwd.y * dist, cam.z + fwd.z * dist);
        sWarm = makeBubble(add(pos, mul(up, kRadius)), up);
        if (sWarm != nullptr) sWarm->setScale(0.001f);
        return sWarm != nullptr;
    }
    bool isWarmBubble(const void* bubble) { return sWarm != nullptr && bubble == static_cast< const void* >(sWarm); }
    void warmTick() {
        if (sWarm != nullptr && !MR::isDead(sWarm)) {
            sWarm->setScale(0.001f);
            MR::stopSound(sWarm, "SE_OJ_BIG_BUBBLE_APPEAR", 0);
        }
    }
    void warmEnd() {
        warmTick();
        if (sWarm != nullptr && !MR::isDead(sWarm)) sWarm->kill();
        sWarm = nullptr;
    }

    void sceneInit() {
        sWarm = nullptr;
        MpDropInHooks::setArrivalHandler(&dropInArrival);   // drop-ins arrive in a bubble (false = the default warp)
        sSceneAlive = false;
        sLocal = Local{};
        sCheck = Check{};
        sRestart = Restart{};
        for (Remote& r : sRemote) r = Remote{};
        sGate.reset();
        waitBannerStep(true);   // uxwait
        sLoadWait.reset();
        sWaitLoad.reset();
        sHolder = false;
        if (!togetherHeld()) return;
        if (MR::isExistSceneObj(SceneObj_BigBubbleHolder)) {
            sHolder = true;   // the stage has its own (Bubble Breeze): its pool of 16 is shared
        } else if (MR::isLoadedObjectOrLayoutArchive(kArchive)) {
            MR::createBigBubbleHolder();
            sHolder = true;
        }
        sSceneAlive = true;
        sPromptOn = false;   // MP3 ingame
        sPromptHold = 0;
        sPromptA = 0.0f;
        mp_log("bubble scene init holder=%d", sHolder ? 1 : 0);
    }

    void sceneEnd() {
        sWarm = nullptr;
        sPromptOn = false;   // MP3 ingame
        sPromptHold = 0;
        sPromptA = 0.0f;
        if (sLocal.on && sLocal.out) sLifeNextScene = true;   // rejoins the next scene with 1 life
        sLocal = Local{};
        sCheck = Check{};
        for (Remote& r : sRemote) r = Remote{};
        sRestart = Restart{};
        sSceneAlive = false;
        sHolder = false;
    }

    void demoStep() {   // MP3 demo
        if (!MpDemo::live() || !sSceneAlive || !MpGameInternal::sceneExists()) return;
        ++sTurn;
        remoteStep();
    }
    bool demoBubbleShown(int slot) {
        return slot >= 0 && slot < MP_MAX_PLAYERS && sRemote[slot].bubble != nullptr && !MR::isDead(sRemote[slot].bubble);
    }

    void onPeerJoined(int slot) {
        if (slot >= 0 && slot < MP_MAX_PLAYERS) sSeenMask &= (uint8_t)~(1u << slot);
    }

    void postCalcAnim() {
        // wave8: MarioActor::calcAndSetBaseMtx() runs after MpBubble::update() and calls updateGravityVec() + updateGroundInfo()
        // (calcShadowPos/checkMap), which overwrites mMovementStates._2=0, mVerticalSpeed=2500, and mShadowPos=pos+2500*g over an
        // abyss. Re-pin the bubble camera invariants here so Turn T+1 CameraDirector::movement() sees clean values.
        if (!sLocal.on) return;
        MarioActor* a = player();
        if (a == nullptr) return;
        const V3 feet = sub(sLocal.pos, mul(sLocal.up, kRiderDrop));
        if (!finite3(feet) || !finite3(sLocal.up)) return;
        syncMarioCameraState(a, feet, sLocal.up, false);
    }

    void update() {
        if (!mp_active()) {
            if (sWasActive) {   // link dropped (ended or reconnecting): clean up remote bubbles and force re-announce on return
                sWasActive = false;
                for (int s = 0; s < MP_MAX_PLAYERS; s++) popBubble(sRemote[s].bubble, false);
                sSeenMask = 0;
                sFresh.reset();
                sLoadWait.reset();
            }
            if (MpGame::sessionHeld()) {
                // Reconnecting: keep sLocal.on safely held in place (never drop the player midair over an abyss).
                if (sLocal.on && sSceneAlive && MpGameInternal::sceneExists()) {
                    MarioActor* a = player();
                    if (a != nullptr) {
                        if (sLocal.bubble != nullptr && MR::isDead(sLocal.bubble)) sLocal.bubble = nullptr;
                        if (sLocal.bubble != nullptr) {
                            keepQuiet(sLocal.bubble);
                            moveBubble(sLocal.bubble, sLocal.pos, v3(0, 0, 0), sLocal.age);
                        }
                        holdPlayer(a);
                        placePlayer(sLocal.pos, sLocal.up);
                    }
                }
                waitBannerStep(true);
                return;
            }
            // Session truly ended: give the player back and reset session bubble state.
            if (sLocal.on) leaveBubble(-1, -1, false, "session end");
            sLocal = Local{};
            sLifeNextScene = false;
            sTable = Table{};
            sFresh.reset();
            sLoadWait.reset();
            waitBannerStep(true);   // uxwait: no session: no banner (also when the scene never had one)
            return;
        }
        if (!sWasActive) sSeenMask = 0;   // active returning: re-broadcast sLocal.on immediately
        sWasActive = true;
        ++sTurn;
        const uint16_t serial = serialNow();
        if (!sTable.serialKnown || sTable.serial != serial) {
            sTable.reset(serial);
            sGate.reset();
        }
        if (!MpGameInternal::sceneExists()) return;
        if (!sSceneAlive) { testDieStep(); return; }
        if (sLifeNextScene && MR::isExistMario()) {
            sLifeNextScene = false;
            if (GameDataFunction::getPlayerLeft() < 1) GameDataFunction::addPlayerLeft(1 - GameDataFunction::getPlayerLeft());
            mp_log("bubble out-of-lives player rejoins with %d", GameDataFunction::getPlayerLeft());
        }
        testDieStep();   // (desktop test hook; Star Race runs it too, line above)
        if (!together()) return;
        const int st = MpGameInternal::sceneState();
        if (sLocal.on && (st == MP_SCENE_STAR_GET || st == MP_SCENE_GRAND_STAR_GET)) leaveBubble(-1, -1, false, "star");
        noteFreshness();
        announceStep();
        waitBannerStep(false);
        remoteStep();
        spinStep();
        if (sLocal.on) localStep();
        promptStep();   // MP3 ingame
        checkStep();
        hostStep();
        restartStep();
    }

    void handle(const MpMsg& m) {
        if (!mp_active()) return;
        MpReader r(m.data, m.len);
        if (m.type == MP_MSG_BUBBLE) {
            MpBubbleMsg b;
            if (!mp_read_bubble(r, &b)) {
                mp_log("bubble drop BUBBLE from=%d len=%d (invalid)", m.fromSlot, m.len);
                return;
            }
            if (!sTable.serialKnown || sTable.serial != serialNow()) sTable.reset(serialNow());
            const bool wasOn = m.fromSlot < MP_MAX_PLAYERS && sTable.e[m.fromSlot].on;   // (a re-announce: no second toast)
            if (!sTable.note(m.fromSlot, b)) {
                mp_log("bubble drop BUBBLE from=%d serial=%d (now %d)", m.fromSlot, b.serial, serialNow());
                return;
            }
            const bool arrival = (b.flags & MP_BUBBLE_ARRIVAL) != 0;
            mp_log("bubble msg from=%d on=%d lives=%d by=%d out=%d%s%s", m.fromSlot, b.on, b.lives, b.by == MP_BUBBLE_BY_NONE ? -1 : b.by,
                   (b.flags & MP_BUBBLE_OUT_OF_LIVES) ? 1 : 0, arrival ? " arrival=1" : "", b.on && wasOn ? " again=1" : "");
            if (b.on && !arrival && !wasOn) MpUi::toast((b.flags & MP_BUBBLE_OUT_OF_LIVES) ? MP_T_OUT_OF_LIVES : MP_T_BUBBLED, m.fromSlot, 0);
            else if (!b.on && b.by != MP_BUBBLE_BY_NONE) MpUi::toast(MP_T_POPPED, b.by, m.fromSlot);
            if (!b.on && b.by != MP_BUBBLE_BY_NONE && (int)b.by == mp_local_slot()) {
                MpTeam::noteHelped();   // MP3 celebrate: we popped him (touch, spin, Star Bit): "helped" on the team star card
                mp_log("bubble helped popped=%d", m.fromSlot);
            }
            return;
        }
        if (m.type == MP_MSG_RESTART) {
            const uint16_t s = r.u16();
            if (!r.done()) { mp_log("bubble drop RESTART from=%d (invalid)", m.fromSlot); return; }
            if (s == serialNow()) MpWorldSync::onRestartMessage();   // hostreload G8: even a console still loading forgets (the others reset their scene)
            if (s != serialNow() || !sSceneAlive || !MpGameInternal::sceneExists()) {
                mp_log("bubble drop RESTART serial=%d (now %d)", s, serialNow());
                return;
            }
            sTable.reset(s);
            startRestart("message");
        }
    }
}  // namespace MpBubble
