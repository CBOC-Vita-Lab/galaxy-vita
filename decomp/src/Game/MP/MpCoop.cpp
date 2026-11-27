// Galaxy 3.0 multiplayer: coop extras, game side (owner: coop). See MpCoop.hpp / MpCoopLogic.hpp.
//
//  Greeting emote (EMOTE 0x22, reliable): D-pad down (Wii D-pad down: in play the game only beeps "can't" on it) or a stamp in
//    the session menu. The sender's player plays Mario's "ghost win" pose (ゴースト勝利 / WinGhost, the Cosmic Mario race's win)
//    while standing idle; the pose reaches every other console through STATE like any animation (Toad puppets map it to the
//    Kinopio "Joy" clip). Every console plays the character's voice at that player (Mario / Luigi: SE_PV_JUMP_JOY, Toad: the
//    Kinopio "hey"), flashes the picture font's open hand on the player's name tag and shows the stamp text as the toast.
//  Pointer ping (POINTER 0x24, unreliable, <= 15 Hz): while B is held with the pointer on screen, the world point under it
//    (the camera ray's first map hit, else the point at the player's depth) goes out; the others draw the game's 2P pointer
//    (DPDPointerSub, StarPointer panes and their Wait animation) there in the sender's slot colour, kept on the screen edge
//    when off screen. Show-only: nothing reads it but the drawing.
//  Super jump: MpRemotePlayer's head spring sensor (ATYPE_PLAYER_AUTO_JUMP) is live while superJumpArmed(); Mario's own trample
//    code gives the spring bounce for that sensor type (MarioActor::doTrampleJump: mTrampleBegoma + the helicopter spin).
//  Desktop test hooks (no effect unless the env is set; compiled out on the Vita):
//    SMG_MP_TEST_EMOTE="<secs>[:<stamp>[:<slot>]]"   the button path, <secs> after each scene start (retried until idle, 10 s)
//    SMG_MP_TEST_POINTER="<secs>:<dur>[:<slot>]"     B held for <dur> s with the pointer circling the screen centre
//    SMG_MP_TEST_SUPERJUMP="<secs>[:<slot>]"         dropped 320 above the first visible friend: plain stomp, then 8 s
//                                                      later the same drop with the jump button held (logs both apexes)
#include "Game/MP/MpPresent.hpp"   // MP3 ingame
#include "Game/MP/MpCharColor.hpp"   // MP3 touches
#include "Game/MP/MpCoop.hpp"
#include "Game/MP/MpCoStar.hpp"   // MP3 costar: the hold / sweep feed
#include "Game/MP/MpDemo.hpp"   // MP3 demo
#include "Game/MP/MpCoopLogic.hpp"
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
extern "C" int port_input_touched(void);   // vitabtn: platform/src/input/wpad.cpp
#include <JSystem/JAudio2/JAISound.hpp>
#include <JSystem/JAudio2/JASCriticalSection.hpp>
#include <JSystem/JAudio2/JASTrack.hpp>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/pane.h>
#include <revolution/wpad.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern "C" int64_t port_host_time_ns(void);

namespace {
    using namespace MpCoopLogic;

    const char* const cGestureAnim = "\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x9f\x97\x98";   // ゴースト勝利 (WinGhost.bck)

    int64_t nowMs() { return port_host_time_ns() / 1000000; }
    bool validSlot(int s) { return s >= 0 && s < MP_MAX_PLAYERS; }

    // ---- the 2P pointer, as a layout of our own -------------------------------------------------------------------------------
    class MpPointerMark : public LayoutActor {
    public:
        explicit MpPointerMark(int slot) : LayoutActor("MpPointerMark", true), mSlot(slot), mLook(-1) {}

        // touches: the player's character colour (the look can change in the lobby / by a LOOK message): retint when it did
        void setLook(int look) {
            if (look != mLook) {
                mLook = look;
                tint();
            }
        }

        virtual void init(const JMapInfoIter&) {
            initLayoutManager("DPDPointerSub", 2);
            MR::createAndAddPaneCtrl(this, "StarPointer", 1);
            MR::hidePaneRecursive(this, "HandPointer");
            MR::showPaneRecursive(this, "StarPointer");
            MR::startPaneAnim(this, "StarPointer", "Wait", 0);
            tint();
            kill();
        }

    private:
        // the materials' "white" colour (TEV register 1: the texture's bright end) becomes the slot colour; the Wait animation
        // only animates vertex colours and pane SRT, so this holds
        void tint() {
            static const char* const cPanes[] = {"PicPointer", "PicFrame", "PicRingEffect"};
            const MpCharColor::C c = MpCharColor::fill(mLook < 0 ? static_cast<int>(mp_slot_look(mSlot)) : mLook);
            for (u32 i = 0; i < sizeof cPanes / sizeof cPanes[0]; i++) {
                nw4r::lyt::Pane* pane = MR::getPane(this, cPanes[i]);
                nw4r::lyt::Material* m = pane != nullptr ? pane->GetMaterial() : nullptr;
                if (m == nullptr) {
                    continue;
                }
                GXColorS10 w = m->GetTevColor(1);
                w.r = c.r;
                w.g = c.g;
                w.b = c.b;
                w.a = 255;   // (the 2P star is drawn over a blur trail in the game; alone it needs its full body to be seen)
                m->SetTevColor(1, w);
            }
        }

        int mSlot;
        int mLook;
    };

    // ---- state ----------------------------------------------------------------------------------------------------------------
    bool sSceneLive;
    uint32_t sSceneGen;                       // initScene count (the test hooks' scene key)
    uint32_t sTurn;
    RateGate sSendGate;
    RateGate sRecvGate[MP_MAX_PLAYERS];
    int32_t sFlashFrom[MP_MAX_PLAYERS];   // sTurn of the slot's last greeting; -1 none
    bool sGesture;
    int sGestureTurns;
    PointerSender sPtrSend;
    PointerView sPtrView[MP_MAX_PLAYERS];
    float sLocalPt[3];                        // MP3 costar: the world point of this console's own last ping
    int64_t sLocalPtMs;
    bool sPtrLogged[MP_MAX_PLAYERS];
    int sPtrTransitions;                      // show / hide lines logged this scene (at most 20)
    MpPointerMark* sMarks[MP_MAX_PLAYERS];   // scene objects: only while sSceneLive
    bool sTestJumpHeld;                       // (desktop test hook) the jump button counts as held
    // super jump apex (the height reached above the bounce point, along the player's up)
    int sApexTurns;
    float sApexStart[3];
    float sApexUp[3];
    float sApexMax;
    bool sApexSuper;

    MarioActor* localActor() { return MpPuppetsInt::localActor(); }

    int localCharacter() {
        const int me = mp_local_slot();
        int c = validSlot(me) ? (int)mp_slot_look(me) : MP_LOOK_MARIO;
        return c < MP_LOOK_COUNT ? c : MP_LOOK_MARIO;
    }

    // ---- voice -----------------------------------------------------------------------------------------------------------------
    void playVoice(LiveActor* at, int character) {
        if (at == nullptr || at->mSoundObject == nullptr) {
            return;
        }
        const Voice v = voiceFor(character);
        JAISoundHandle* h = MR::startSound(at, v.se, -1, -1);
        if (v.port12 >= 0 && h != nullptr && h->isSoundAttached()) {
            // AudSystem::switchToLuigi's port, for this one sound: the voice of the character shown, whatever the local save's
            JASCriticalSection crit;
            JASTrack* track = h->mSound->getTrack();
            if (track != nullptr) {
                track->writePort(12, (u16)v.port12);
            }
        }
    }

    // where the local player's voice comes from: the self puppet when it draws us, else the MarioActor
    LiveActor* localVoiceSource() {
        MpPuppetBase* self = MpPuppetsInt::puppet(mp_local_slot());
        if (self != nullptr && !MR::isDead(self)) {
            return self;
        }
        return localActor();
    }

    // ---- gesture ---------------------------------------------------------------------------------------------------------------
    bool stickOn() {
        const f32 x = MR::getPlayerStickX(), y = MR::getPlayerStickY();
        return x * x + y * y > 0.01f;
    }

    bool readIdle(MarioActor* a, IdleIn* in) {
        Mario* m = a->mMario;
        if (m == nullptr) {
            return false;
        }
        const u32 status = m->getCurrentStatus();
        in->onGround = a->getMovementStates()._1;
        in->jumping = a->getMovementStates().jumping;
        in->plainStatus = status == MarioStatus_None || status == MarioStatus_Wait;
        in->defaultAnim = status == MarioStatus_Wait || !a->isAnimationRun(nullptr);
        in->stickOn = stickOn();
        in->walkSpeed = m->mWalkSpeed;
        in->busy = MR::isPlayerDead() || MR::isDemoActive() || MR::isPlayerDamaging() || MR::isPlayerSquat() || MR::isPlayerCarryAny() ||
                   MR::isPlayerInBind() || MR::isPlayerSkating() || MR::isPlayerSwimming() || MR::isPlayerInWaterMode() ||
                   a->isJumping();
        const u16 mode = a->mPlayerMode;
        in->formOk = a->mCurrModel != 1 && mode != 5 && mode != 6 && mode != 7;   // not bone, Spring, Boo, Flying
        return true;
    }

    bool tryStartGesture() {
        MarioActor* a = localActor();
        IdleIn in;
        if (!sSceneLive || a == nullptr || !readIdle(a, &in) || !idleForGesture(in)) {
            return false;
        }
        a->changeAnimation(cGestureAnim, nullptr);
        if (!a->isAnimationRun(cGestureAnim)) {
            return false;
        }
        sGesture = true;
        sGestureTurns = 0;
        return true;
    }

    void updateGesture() {
        if (!sGesture) {
            return;
        }
        sGestureTurns++;
        MarioActor* a = localActor();
        if (a == nullptr || !a->isAnimationRun(cGestureAnim)) {
            sGesture = false;   // ended, or the game changed it (a jump, a hit, ...)
            return;
        }
        const bool onGround = a->getMovementStates()._1;
        if (gestureShouldStop(stickOn(), MR::testCorePadTriggerA(WPAD_CHAN0), MR::testSubPadButtonZ(WPAD_CHAN0), onGround, sGestureTurns)) {
            a->stopAnimation(cGestureAnim);
            sGesture = false;
            mp_log("coop gesture stop turns=%d", sGestureTurns);
        }
    }

    // ---- pointer ---------------------------------------------------------------------------------------------------------------
    bool sPtrDiag;   // the first ping of a scene logs how its point was found

    bool pointerWorld(const TVec2f& screen, TVec3f* out) {
        const TVec3f cam = MR::getCamPos();
        TVec3f dir;
        MR::calcWorldPositionFromScreen(&dir, screen, -1.0f);
        dir -= cam;
        const char* how = "none";
        bool ok = false;
        if (!MR::normalizeOrZero(&dir)) {
            TVec3f hit;
            if (MR::getFirstPolyOnLineToMap(&hit, nullptr, cam, dir * 30000.0f)) {
                *out = hit;
                how = "map";
                ok = true;
            }
        }
        const TVec3f* player = MR::getPlayerPos();
        if (!ok && player != nullptr) {
            // nothing under it: the point on the same ray at the player's distance from the camera
            MR::calcWorldPositionFromScreen(out, screen, MR::calcCameraDistanceZ(*player));
            how = "depth";
            ok = true;
        }
        if (!sPtrDiag) {
            sPtrDiag = true;
            mp_log("coop pointer first screen=%.0f,%.0f cam=%.0f,%.0f,%.0f dir=%.2f,%.2f,%.2f how=%s pos=%.0f,%.0f,%.0f", screen.x, screen.y,
                   cam.x, cam.y, cam.z, dir.x, dir.y, dir.z, how, ok ? out->x : 0.0f, ok ? out->y : 0.0f, ok ? out->z : 0.0f);
        }
        return ok;
    }

    void sendPointer(bool on, const TVec3f& p) {
        MpPointerMsg m;
        m.serial = mp_scene_serial();
        m.on = on ? 1 : 0;
        m.pos[0] = p.x;
        m.pos[1] = p.y;
        m.pos[2] = p.z;
        if (on) {   // MP3 costar: the sender's own console holds under its pointer too
            sLocalPt[0] = p.x; sLocalPt[1] = p.y; sLocalPt[2] = p.z;
            sLocalPtMs = nowMs();
        }
        uint8_t buf[16];
        MpWriter w(buf, sizeof buf);
        mp_write_pointer(w, m);
        if (w.ok()) {
            mp_send(MP_MSG_POINTER, buf, (uint16_t)w.n, false);
        }
    }

    // test hook: B held with a pointer circling the screen centre
    bool testPointer(TVec2f* pScreen);
    bool testPointerEnemy(TVec3f* pWorld);
    bool testPointerStarBit(TVec3f* pWorld);

    void updatePointerSend() {
        TVec2f screen(0.0f, 0.0f);
        bool holding = false;
        TVec3f testP(0.0f, 0.0f, 0.0f);
        bool testEnemy = false;
        if (sSceneLive && MpGame::isGameSceneLive() && !MpUi::isMenuOpen() && !MR::isPlayerDead() && !MR::isDemoActive()) {
            if (testPointerEnemy(&testP) || testPointerStarBit(&testP)) {
                holding = testEnemy = true;
            } else if (testPointer(&screen)) {
                holding = true;
            } else if ((MR::testCorePadButtonB(WPAD_CHAN0) || port_input_touched()) && MR::isStarPointerInScreen(WPAD_CHAN0)) {   // vitabtn: Circle held, or a finger on the front screen
                screen = *MR::getStarPointerScreenPosition(WPAD_CHAN0);
                holding = true;
            }
        }
        const PointerSend what = sPtrSend.step(holding, port_host_time_ns());
        if (what == POINTER_ON) {
            TVec3f p;
            if (testEnemy) {
                sendPointer(true, testP);
            } else if (pointerWorld(screen, &p)) {
                sendPointer(true, p);
            }
        } else if (what == POINTER_OFF) {
            sendPointer(false, TVec3f(0.0f, 0.0f, 0.0f));
            mp_log("coop pointer off");
        }
    }

    void updatePointerViews() {
        const int64_t t = nowMs();
        const int me = mp_local_slot();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            MpPointerMark* mark = sMarks[s];
            if (mark == nullptr) {
                continue;
            }
            const bool on = s != me && mp_slot_present(s) && sPtrView[s].step(t);
            if (!on) {
                if (!MR::isDead(mark)) {
                    mark->kill();
                    if (sPtrTransitions < 20) {
                        sPtrTransitions++;
                        mp_log("coop pointer hide slot=%d", s);
                    }
                }
                continue;
            }
            TVec3f w(sPtrView[s].pos[0], sPtrView[s].pos[1], sPtrView[s].pos[2]);
            TVec3f view;
            MR::getCameraViewMtx().mult(w, view);
            const bool behind = view.z > 0.0f;   // the camera looks down -z
            TVec3f screen;
            MR::calcScreenPosition(&screen, w);
            f32 x = screen.x, y = screen.y;
            mp_edge_clamp(screen.x, screen.y, behind, (f32)MR::getScreenWidth(), (f32)MR::getScreenHeight(), 24.0f, &x, &y);
            mark->setLook(static_cast<int>(mp_slot_look(s)));   // MP3 touches: the character's colour
            if (MR::isDead(mark)) {
                mark->appear();
                MR::startPaneAnim(mark, "StarPointer", "Wait", 0);
                if (sPtrTransitions < 20) {
                    sPtrTransitions++;
                    mp_log("coop pointer show slot=%d at=%.0f,%.0f edge=%d", s, x, y, (x != screen.x || y != screen.y) ? 1 : 0);
                }
            }
            mark->setTrans(TVec2f(x, y));
        }
    }

    void handlePointer(const MpMsg& m) {
        const int from = m.fromSlot;
        if (!validSlot(from) || from == mp_local_slot()) {
            return;
        }
        MpReader r(m.data, m.len);
        MpPointerMsg p;
        if (!mp_read_pointer(r, &p)) {
            MpPuppetsInt::logOnce(from, "coop drop pointer reason=parse len", m.len);
            return;
        }
        if (p.serial != mp_scene_serial()) {
            return;   // another scene's point means nothing here
        }
        sPtrView[from].receive(p, nowMs());
        if (p.on && !sPtrLogged[from]) {
            sPtrLogged[from] = true;
            mp_log("coop pointer recv from=%d pos=%.0f,%.0f,%.0f", from, p.pos[0], p.pos[1], p.pos[2]);
        }
    }

    // ---- emote -----------------------------------------------------------------------------------------------------------------
    void handleEmote(const MpMsg& m) {
        const int from = m.fromSlot;
        if (!validSlot(from) || from == mp_local_slot()) {
            return;
        }
        MpReader r(m.data, m.len);
        MpEmoteMsg e;
        if (!mp_read_emote(r, &e)) {
            MpPuppetsInt::logOnce(from, "coop drop emote reason=parse len", m.len);
            return;
        }
        if (!sRecvGate[from].allow(nowMs(), kEmoteRecvMinMs)) {
            mp_log("coop emote recv from=%d dropped reason=rate", from);
            return;
        }
        MpUi::toast((uint16_t)(MP_T_STAMP_0 + e.emote), from, 0);   // (MP3 ingame: not shown in play; the balloon below says it)
        MpChat::greetingBalloon(from, (int)e.emote);                 // MP3 ingame
        bool voice = false;
        MpPuppetBase* p = sSceneLive ? MpPuppetsInt::puppet(from) : nullptr;
        if (p != nullptr && !MR::isDead(p) && p->isDrawable()) {
            playVoice(p, p->getCharacter());
            voice = true;
            sFlashFrom[from] = (int32_t)sTurn;
        }
        mp_log("coop emote recv from=%d stamp=%d gesture=%d voice=%d", from, (int)e.emote, (e.flags & MP_EMOTE_GESTURE) ? 1 : 0,
               voice ? 1 : 0);
    }

    // ---- desktop test hooks ----------------------------------------------------------------------------------------------------
#if defined(TARGET_PC) && !defined(__vita__)
    struct TestTimer {
        int on = -1;
        double secs = 0.0, dur = 0.0;
        int arg = -1, slot = -1;
        int serial = -1;
        int64_t startMs = 0;
        void parse(const char* name, bool hasDur) {
            const char* e = getenv(name);
            on = (e && *e) ? 1 : 0;
            if (!on) return;
            secs = atof(e);
            const char* c = strchr(e, ':');
            if (hasDur && c) { dur = atof(c + 1); c = strchr(c + 1, ':'); }
            if (!hasDur && c) { arg = atoi(c + 1); c = strchr(c + 1, ':'); }
            if (c) slot = atoi(c + 1);
            mp_log("coop test %s: %.1f s after each scene start dur=%.1f arg=%d slot=%d", name, secs, dur, arg, slot);
        }
        // seconds since the player got control in this scene (the first live turn without a demo; -1: not yet / not this console)
        double since() {
            if (!on || (slot >= 0 && mp_local_slot() != slot)) return -1.0;
            const int s = (int)sSceneGen;
            if (s != serial) {
                if (!MpGame::isGameSceneLive() || MR::isDemoActive()) return -1.0;
                serial = s;
                startMs = nowMs();
            }
            return (double)(nowMs() - startMs) / 1000.0;
        }
    };
    TestTimer sTestEmote, sTestPointer, sTestJump, sTestPtrEnemy, sTestPtrBit;
    int sTestEmoteSerial = -1;
    int sTestJumpStage = 0;   // 0 waiting, 1 plain drop done, 2 super drop done
    int sTestJumpSerial = -1;

    bool testPointer(TVec2f* pScreen) {
        if (sTestPointer.on < 0) sTestPointer.parse("SMG_MP_TEST_POINTER", true);
        const double t = sTestPointer.since();
        if (t < sTestPointer.secs || t > sTestPointer.secs + sTestPointer.dur) return false;
        const f32 a = (f32)(t - sTestPointer.secs) * 1.5f;
        const f32 w = (f32)MR::getScreenWidth(), h = (f32)MR::getScreenHeight();
        pScreen->set(w * 0.5f + cosf(a) * w * 0.12f, h * 0.62f + sinf(a) * h * 0.08f);
        return true;
    }

    // MP3 costar: the pointer held on the nearest enemy that asks the hold question (SMG_MP_TEST_POINTER_ENEMY="<secs>:<dur>[:<slot>]")
    bool testPointerEnemy(TVec3f* pWorld) {
        if (sTestPtrEnemy.on < 0) sTestPtrEnemy.parse("SMG_MP_TEST_POINTER_ENEMY", true);
        const double t = sTestPtrEnemy.since();
        if (t < sTestPtrEnemy.secs || t > sTestPtrEnemy.secs + sTestPtrEnemy.dur) return false;
        float p[3];
        if (!MpCoStar::testNearestEnemy(p)) return false;
        pWorld->set(p[0], p[1], p[2]);
        return true;
    }

    // ... and on the nearest Star Bit (SMG_MP_TEST_POINTER_STARBIT, same format): the others' copies of it are swept
    bool testPointerStarBit(TVec3f* pWorld) {
        if (sTestPtrBit.on < 0) sTestPtrBit.parse("SMG_MP_TEST_POINTER_STARBIT", true);
        const double t = sTestPtrBit.since();
        if (t < sTestPtrBit.secs || t > sTestPtrBit.secs + sTestPtrBit.dur) return false;
        float p[3];
        if (!MpCoStar::testNearestStarBit(p)) return false;
        pWorld->set(p[0], p[1], p[2]);
        return true;
    }

    void testEmoteStep() {
        if (sTestEmote.on < 0) sTestEmote.parse("SMG_MP_TEST_EMOTE", false);
        const double t = sTestEmote.since();
        if (t < sTestEmote.secs || sTestEmoteSerial == sTestEmote.serial) return;
        const int stamp = sTestEmote.arg >= 0 && sTestEmote.arg < MP_EMOTE_COUNT ? sTestEmote.arg : 0;
        if (MpCoop::requestEmote(stamp, false)) {
            sTestEmoteSerial = sTestEmote.serial;
            mp_log("coop test emote: sent stamp=%d after %.1f s", stamp, t);
        } else if (t > sTestEmote.secs + 10.0) {
            sTestEmoteSerial = sTestEmote.serial;
            IdleIn in;
            MarioActor* a = localActor();
            if (a != nullptr && readIdle(a, &in)) {
                mp_log("coop test emote: gave up (never idle) ground=%d jump=%d anim=%d stick=%d walk=%.2f status=%d busy=%d form=%d",
                       in.onGround, in.jumping, in.defaultAnim, in.stickOn, in.walkSpeed, in.plainStatus, in.busy, in.formOk);
            } else {
                mp_log("coop test emote: gave up (no player)");
            }
        }
    }

    void testDrop(bool super) {
        MarioActor* a = localActor();
        const int me = mp_local_slot();
        for (int s = 0; s < MP_MAX_PLAYERS && a != nullptr; s++) {
            MpPuppetBase* p = s != me ? MpPuppetsInt::puppet(s) : nullptr;
            if (p == nullptr || MR::isDead(p) || !p->isDrawable()) continue;
            TVec3f up(-p->mGravity.x, -p->mGravity.y, -p->mGravity.z);
            TVec3f at(p->mPosition);
            at += up * 320.0f;
            MR::setPlayerPos(at);
            sTestJumpHeld = super;
            mp_log("coop test superjump: dropped onto slot=%d held=%d", s, super ? 1 : 0);
            return;
        }
        mp_log("coop test superjump: no friend to drop onto");
    }

    void testJumpStep() {
        if (sTestJump.on < 0) sTestJump.parse("SMG_MP_TEST_SUPERJUMP", false);
        const double t = sTestJump.since();
        if (t < 0.0) return;
        if (sTestJump.serial != sTestJumpSerial) { sTestJumpSerial = sTestJump.serial; sTestJumpStage = 0; }
        if (sTestJumpStage == 0 && t >= sTestJump.secs) { sTestJumpStage = 1; testDrop(false); }
        else if (sTestJumpStage == 1 && t >= sTestJump.secs + 8.0) { sTestJumpStage = 2; testDrop(true); }
        else if (sTestJumpStage == 2 && t >= sTestJump.secs + 12.0) { sTestJumpStage = 3; sTestJumpHeld = false; }
    }
    void testSteps() { testEmoteStep(); testJumpStep(); }
#else
    bool testPointer(TVec2f*) { return false; }
    bool testPointerEnemy(TVec3f*) { return false; }
    bool testPointerStarBit(TVec3f*) { return false; }
    void testSteps() {}
#endif

    // ---- super jump apex (logged: the size of the bounce, for the tests) -------------------------------------------------------
    void updateApex() {
        if (sApexTurns <= 0) {
            return;
        }
        const TVec3f* pos = MR::getPlayerPos();
        if (pos != nullptr) {
            const f32 h = (pos->x - sApexStart[0]) * sApexUp[0] + (pos->y - sApexStart[1]) * sApexUp[1] + (pos->z - sApexStart[2]) * sApexUp[2];
            if (h > sApexMax) sApexMax = h;
        }
        if (--sApexTurns == 0) {
            mp_log("coop bounce apex=%.0f kind=%s", sApexMax, sApexSuper ? "super" : "stomp");
        }
    }
}  // namespace

// ==== contract ===============================================================================================================
namespace MpCoop {
    void initScene() {
        if (!MpGame::sessionHeld() && !MpDemo::live()) {   // MP3 demo: the pointer marks exist for the puppet demo too
            return;
        }
        sSceneLive = false;
        sGesture = false;
        sPtrSend.reset();
        sLocalPtMs = 0;
        MpCoStar::initScene();   // MP3 costar
        sApexTurns = 0;
        sPtrDiag = false;
        sPtrTransitions = 0;
        const bool layout = MR::isLoadedObjectOrLayoutArchive("DPDPointerSub");
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            sMarks[s] = nullptr;
            sPtrView[s].reset();
            sPtrLogged[s] = false;
            sFlashFrom[s] = -1;
            if (!layout || s == mp_local_slot()) {
                continue;
            }
            MpPointerMark* mark = new MpPointerMark(s);
            MR::connectToSceneLayout(mark);
            mark->initWithoutIter();
            sMarks[s] = mark;
        }
        sSceneLive = true;
        sSceneGen++;
        mp_log("coop init pointer=%d", layout ? 1 : 0);
    }

    // MP3 puppetprep (MpWarm): the first mark appears far off screen (its layout's textures are used once) and goes again.
    bool warm(bool on) {
        if (!sSceneLive) {
            return false;
        }
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            MpPointerMark* mark = sMarks[s];
            if (mark == nullptr || MR::isDead(mark) == !on) {
                continue;
            }
            if (on) {
                mark->appear();
                MR::startPaneAnim(mark, "StarPointer", "Wait", 0);
                mark->setTrans(TVec2f(-2000.0f, -2000.0f));
            } else {
                mark->kill();
            }
            return true;
        }
        return false;
    }

    void update() {
        if (!sSceneLive) {
            return;
        }
        if (!mp_active() && MpDemo::live()) {   // MP3 demo: only the other players' pointer marks (no emote, no local pointer, no costar)
            sTurn++;
            updatePointerViews();
            return;
        }
        if (!mp_active()) {
            // the session ended in this scene: no pointer stays on screen (MpPuppets::update calls this then too)
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                if (sMarks[s] != nullptr && !MR::isDead(sMarks[s])) sMarks[s]->kill();
            }
            sGesture = false;
            return;
        }
        sTurn++;
        testSteps();
        // the button: D-pad down (the game uses it for nothing in play), in play, no menu
        if (MR::testCorePadTriggerDown(WPAD_CHAN0) && MpGame::isGameSceneLive() && !MpUi::isMenuOpen()) {
            requestEmote(0, false);
        }
        updateGesture();
        updatePointerSend();
        updatePointerViews();
        updateApex();
        MpCoStar::update();   // MP3 costar: holds and sweeps from the pointers
    }

    void endScene() {
        MpCoStar::endScene();   // MP3 costar
        sSceneLive = false;
        sGesture = false;
        sApexTurns = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            sMarks[s] = nullptr;   // scene heap: never touched again
            sPtrView[s].reset();
            sFlashFrom[s] = -1;
        }
        sPtrSend.reset();
    }

    void handle(const MpMsg& m) {
        if (!mp_active() && !(MpDemo::live() && m.type == MP_MSG_POINTER)) {   // MP3 demo: canned POINTER pings only
            return;
        }
        if (m.type == MP_MSG_EMOTE) {
            handleEmote(m);
        } else if (m.type == MP_MSG_POINTER) {
            handlePointer(m);
        }
    }

    bool requestEmote(int stamp, bool fromMenu) {
        if (!mp_active() || stamp < 0 || stamp >= MP_EMOTE_COUNT) {
            return false;
        }
        // the button only greets from a standing player (Wonder's rule); the menu's stamps go out anyway
        bool gesture = false;
        if (!fromMenu) {
            MarioActor* a = localActor();
            IdleIn in;
            if (!sSceneLive || a == nullptr || !readIdle(a, &in) || !idleForGesture(in)) {
                return false;
            }
        }
        if (!sSendGate.allow(nowMs(), kEmoteMinMs)) {
            mp_log("coop emote refused reason=rate");
            return false;
        }
        gesture = tryStartGesture();
        MpEmoteMsg e;
        e.emote = (uint8_t)stamp;
        e.flags = gesture ? MP_EMOTE_GESTURE : 0;
        uint8_t buf[4];
        MpWriter w(buf, sizeof buf);
        mp_write_emote(w, e);
        const bool sent = w.ok() && mp_send(MP_MSG_EMOTE, buf, (uint16_t)w.n, true);
        if (sSceneLive) {
            playVoice(localVoiceSource(), localCharacter());
        }
        MpUi::toast((uint16_t)(MP_T_STAMP_0 + stamp), mp_local_slot(), 0);
        MpChat::greetingBalloon(mp_local_slot(), stamp);   // MP3 ingame
        mp_log("coop emote sent stamp=%d gesture=%d via=%s ok=%d", stamp, gesture ? 1 : 0, fromMenu ? "menu" : "button", sent ? 1 : 0);
        return true;
    }

    bool pointerPoint(int slot, float out[3]) {   // MP3 costar
        if (!(mp_active() || MpDemo::live()) || !sSceneLive || !validSlot(slot) || !mp_slot_present(slot)) {
            return false;
        }
        const int64_t t = nowMs();
        if (slot == mp_local_slot()) {
            if (!sPtrSend.sending || t < sLocalPtMs || t - sLocalPtMs > kPointerStaleMs) {
                return false;
            }
            for (int i = 0; i < 3; i++) out[i] = sLocalPt[i];
            return true;
        }
        const PointerView& v = sPtrView[slot];
        if (!v.on || t < v.lastMs || t - v.lastMs > kPointerStaleMs) {
            return false;
        }
        for (int i = 0; i < 3; i++) out[i] = v.target[i];
        return true;
    }

    bool superJumpArmed() {
        if (!mp_active() || !sSceneLive) {
            return false;
        }
        MarioActor* a = localActor();
        if (a == nullptr) {
            return false;
        }
        const bool held = sTestJumpHeld || MR::testCorePadButtonA(WPAD_CHAN0);
        return MpCoopLogic::superJumpArmed(MpPuppetsInt::interactionOn(), held, a->getMovementStates()._1, MR::isPlayerDead());
    }

    void noteTrample(int slot, bool super) {
        if (!mp_active()) {
            return;
        }
        if (super) {
            mp_log("coop superjump onto slot=%d", slot);
        }
        // the bounce's apex above this point, along the player's up, is logged 2 s later (the tests compare the two kinds)
        if (sApexTurns > 0) {   // a new bounce before the last one's 2 s were up (bouncing on a friend again and again): log that one now
            mp_log("coop bounce apex=%.0f kind=%s", sApexMax, sApexSuper ? "super" : "stomp");
        }
        const TVec3f* pos = MR::getPlayerPos();
        MarioActor* a = localActor();
        if (pos == nullptr || a == nullptr) {
            return;
        }
        const TVec3f& g = a->getGravityVec();
        sApexStart[0] = pos->x;
        sApexStart[1] = pos->y;
        sApexStart[2] = pos->z;
        sApexUp[0] = -g.x;
        sApexUp[1] = -g.y;
        sApexUp[2] = -g.z;
        sApexMax = 0.0f;
        sApexTurns = 120;
        sApexSuper = super;
    }

    bool nameTagIcon(int slot) {
        if (!mp_active() || !validSlot(slot) || sFlashFrom[slot] < 0) {
            return false;
        }
        return flashOn((int)(sTurn - (uint32_t)sFlashFrom[slot]));
    }
}  // namespace MpCoop
