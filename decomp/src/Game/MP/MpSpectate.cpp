// Galaxy 3.0 multiplayer: spectating, game side (owner: spectate). Rules and maths in MpSpectateLogic.hpp (host tests).
//
// A waiting player (a bubble no teammate is near, a bubble with no life left, a Star Race finisher behind the star wipe) watches a
// teammate. The view is the game's own event camera machinery: the event chunk "MpWatch" (declared in CameraDirector::
// closeCreatingCameraChunk, a session only) runs CameraMpWatch (CAM_TYPE_MP_WATCH) on MpWatchTarget, a CameraTargetObj over the
// puppet's pose. Starting and ending go through MR::startEventCamera / MR::endEventCamera with a blend (the game's view interpolator
// glides between the two views, collision included); switching the watched player re-targets and starts a view interpolation.
// The local Mario is not touched: it stays in its own state (held in the bubble, or in the star demo behind the wipe), so nothing it
// does can change while the view is elsewhere.
//
// What follows the local Mario and what follows the view while watching (DESIGN):
//   clipping / draw culling, sound listener ....... the view (ClippingJudge and the microphone read the camera matrix): automatic
//   planet LOD (LodCtrl) .......................... the watched character (hook in LodCtrl::calcDistanceToCamera; it asked for the player)
//   puppet detail (MpQuality) ..................... the watched puppet counts as the nearest (full rate)
//   switches, coins, enemies, kill planes, area triggers, music, fog / lights by area ... the local Mario's own place (a held player
//                                                     triggers nothing; a bubble's sensors are off, the star demo holds the player)
//   the bubble's own float ........................ goes on; the stick no longer steers it (nobody sees it)
#include <stdlib.h>
#include "Game/MP/MpSpectate.hpp"
#include "Game/MP/MpSpectateLogic.hpp"

#include "Game/Camera/CameraDirector.hpp"
#include "Game/Camera/CameraManEvent.hpp"
#include "Game/Camera/CameraMpWatch.hpp"
#include "Game/Camera/CameraParamChunk.hpp"
#include "Game/Camera/CameraTargetArg.hpp"
#include "Game/Camera/CameraTargetObj.hpp"
#include "Game/LiveActor/ActorCameraInfo.hpp"
#include "Game/MP/MpBubble.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpMenuSequence.hpp"
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpPanelDraw.hpp"
#include "Game/MP/MpPanelPlan.hpp"
#include "Game/MP/MpPresent.hpp"   // MP3 ingame: the watch badge
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"

namespace {
    const char* const kEvent = "MpWatch";     // (the same name CameraDirector declares)
    const int kBlendIn = 45, kBlendSwitch = 36, kBlendOut = 45;   // logic turns (the game's own cameras use 30 to 120)
    const int kBubbleBlend = 20;              // wave8: crisp 0.33 s glide onto / off the local bubble-follow camera
    const int kWipeFrames = 40;
    const int kSelfBubbleSlot = -2;           // wave8: MpWatchTarget tracks the local player's bubble

    // The camera's target: a teammate's puppet (mSlot >= 0) or the local bubble (kSelfBubbleSlot), read every frame by CameraDirector::movement.
    class MpWatchTarget : public CameraTargetObj {
    public:
        MpWatchTarget() : CameraTargetObj("MpWatchTarget"), mSlot(-1), mPos(0.0f, 0.0f, 0.0f), mUp(0.0f, 1.0f, 0.0f), mFront(0.0f, 0.0f, 1.0f),
                          mSide(1.0f, 0.0f, 0.0f), mVel(0.0f, 0.0f, 0.0f), mGravity(0.0f, -1.0f, 0.0f) {}
        virtual ~MpWatchTarget() {}

        void setSlot(int slot) {
            mSlot = slot;
            refresh();
        }
        void setLocalBubble() {
            mSlot = kSelfBubbleSlot;
            float pos[3], up[3], vel[3];
            if (MpBubble::localPose(pos, up, vel)) {
                const MpSpectate::V3 u = MpSpectate::normOr(MpSpectate::v3(up[0], up[1], up[2]), MpSpectate::v3(0.0f, 1.0f, 0.0f));
                // Seed heading from the active camera's forward direction so entering the bubble keeps the player's viewing angle.
                const TVec3f cz = MR::getCamZdir();
                MpSpectate::V3 f = MpSpectate::v3(cz.x, cz.y, cz.z);
                f = MpSpectate::sub(f, MpSpectate::mul(u, MpSpectate::dot(f, u)));
                f = MpSpectate::normOr(f, MpSpectate::perpendicular(u));
                mFront.set(f.x, f.y, f.z);
            }
            refresh();
        }
        int slot() const { return mSlot; }

        virtual void movement() { refresh(); }
        virtual const TVec3f& getPosition() const { return mPos; }
        virtual const TVec3f& getUpVec() const { return mUp; }
        virtual const TVec3f& getFrontVec() const { return mFront; }
        virtual const TVec3f& getSideVec() const { return mSide; }
        virtual const TVec3f& getLastMove() const { return mVel; }
        virtual const TVec3f& getGroundPos() const { return mPos; }
        virtual const TVec3f& getGravityVector() const { return mGravity; }

    private:
        void refresh() {
            if (mSlot == kSelfBubbleSlot) {
                float pos[3], up[3], vel[3];
                if (!MpBubble::localPose(pos, up, vel)) {
                    return;
                }
                const MpSpectate::V3 p = MpSpectate::v3(pos[0], pos[1], pos[2]);
                const MpSpectate::V3 u = MpSpectate::normOr(MpSpectate::v3(up[0], up[1], up[2]), MpSpectate::v3(0.0f, 1.0f, 0.0f));
                const MpSpectate::V3 v = MpSpectate::v3(vel[0], vel[1], vel[2]);
                if (!MpSpectate::finite3(p) || !MpSpectate::finite3(u) || !MpSpectate::finite3(v)) {
                    return;
                }
                MpSpectate::V3 curF = MpSpectate::v3(mFront.x, mFront.y, mFront.z);
                curF = MpSpectate::normOr(MpSpectate::sub(curF, MpSpectate::mul(u, MpSpectate::dot(curF, u))), MpSpectate::perpendicular(u));
                // Gently align heading along horizontal travel velocity while floating across the stage.
                const MpSpectate::V3 vh = MpSpectate::sub(v, MpSpectate::mul(u, MpSpectate::dot(v, u)));
                if (MpSpectate::len(vh) > 2.0f) {
                    const MpSpectate::V3 dir = MpSpectate::normOr(vh, curF);
                    curF = MpSpectate::normOr(MpSpectate::add(MpSpectate::mul(curF, 0.85f), MpSpectate::mul(dir, 0.15f)), dir);
                    curF = MpSpectate::normOr(MpSpectate::sub(curF, MpSpectate::mul(u, MpSpectate::dot(curF, u))), dir);
                }
                mPos.set(p.x, p.y, p.z);
                mUp.set(u.x, u.y, u.z);
                mFront.set(curF.x, curF.y, curF.z);
                mSide.cross(mUp, mFront);
                mVel.set(v.x, v.y, v.z);
                mGravity.set(-u.x, -u.y, -u.z);
                return;
            }
            MpPuppetBase* p = mSlot >= 0 ? MpPuppetsInt::puppet(mSlot) : nullptr;
            if (p == nullptr || MR::isDead(p)) {
                return;   // (the camera keeps the last pose; the selection moves on within a turn)
            }
            const TVec3f& pos = p->mPosition;
            TVec3f up, front;
            p->getFrame(&up, &front);
            if (!MpSpectate::finite3(MpSpectate::v3(pos.x, pos.y, pos.z)) || !MpSpectate::finite3(MpSpectate::v3(up.x, up.y, up.z))) {
                return;
            }
            mPos = pos;
            mUp = up;
            mFront = front;
            mSide.cross(up, front);
            mVel = p->mVelocity;
            mGravity.set(-up.x, -up.y, -up.z);
        }

        int mSlot;
        TVec3f mPos, mUp, mFront, mSide, mVel, mGravity;
    };

    MpWatchTarget* sTarget = nullptr;
    bool sSceneUp = false;
    MpSpectate::Selector sSel;
    uint32_t sTurn = 0;

    int sWatch = -1;                 // slot whose view is up (-1 none)
    bool sCamUp = false;             // the event camera was started and not ended
    bool sBubbleCam = false;         // wave8: "MpWatch" is following our own local bubble (watching() stays false)
    bool sLabelOn = false;
    bool sHintShown = false;         // once per scene
    bool sZPrev = false;
    int sPickSlot = -1;              // selectSlot(), consumed by the next turn
    int sRetry = 0;                  // turns until a lost camera is started again
    bool sNoChunk = false;           // the scene has no "MpWatch" camera chunk: logged once, no more attempts (review2 #12)
    bool sBlockedLogged = false;     // "start deferred" logged once per deferral

    // Star Race hold (behind the closed star wipe)
    enum RacePhase { RP_CLOSED = 0, RP_OPENING, RP_OPEN, RP_CLOSING };
    int sRacePhase = RP_CLOSED;
    int sRaceTimer = 0;
    bool sRaceLatch = false;         // holdAfterClear ticked and the results are not in
    bool sRaceCloseWanted = false;

    // the per-scene / per-session leftovers (review2 #11): nothing of one scene's input or timers may reach the next
    void clearTransient() {
        sRaceCloseWanted = false;
        sPickSlot = -1;
        sZPrev = false;
        sRetry = 0;
        sBlockedLogged = false;
    }

    bool raceNow() { return mp_active() && mp_mode() == MP_MODE_RACE; }

    CameraDirector* director() { return MR::getCameraDirector(); }

    CameraParamChunkEvent* chunk() {
        CameraDirector* d = director();
        return d != nullptr ? d->getEventParameter(0, kEvent) : nullptr;
    }

    // Is our event the one the camera runs now?
    bool cameraIsOurs() {
        CameraDirector* d = director();
        CameraParamChunkEvent* c = chunk();
        return d != nullptr && c != nullptr && d->isEventCameraActive() && d->mCameraManEvent != nullptr && d->mCameraManEvent->mChunk == c;
    }

    // MP3 ingame: no "Watching <name>" line: the watched player's head in a ring in the corner (badgeModel), the L and R pictures
    // beside it when there is someone else to switch to; it fades in and out
    int sLabelSlot = -1;
    float sBadgeA = 0.0f;
    bool sCanSwitch = false;
    // A game event camera or a demo that is not ours is running (a boss intro, a talk camera): our chunk shares its priority slot, so
    // starting ours would cut it short (review2 #7). Checked by reading the camera director's own "is an event camera active" answer.
    bool gameEventRunning() {
        CameraDirector* d = director();
        if (d != nullptr && d->isEventCameraActive() && !cameraIsOurs()) return true;
        return MR::isDemoActive();
    }

    void labelOff() {
        sLabelOn = false;
    }

    void labelOn(int slot) {
        sLabelSlot = slot;
        sLabelOn = true;
    }

    bool startCamera(int slot, int frames) {
        CameraDirector* d = director();
        if (d == nullptr || sTarget == nullptr) {
            return false;
        }
        if (chunk() == nullptr) {
            if (!sNoChunk) mp_log("spectate: no watch camera in this scene (no more attempts)");
            sNoChunk = true;   // (review2 #12: the scene's camera table has no \"MpWatch\": waiting is pointless, watching is off here)
            return false;
        }
        sTarget->setSlot(slot);
        CameraMpWatch::sRestartToken++;
        ActorCameraInfo info = ActorCameraInfo();
        MR::startEventCamera(&info, kEvent, CameraTargetArg(sTarget, nullptr, nullptr, nullptr), frames);
        sCamUp = true;
        sBubbleCam = false;
        return true;
    }

    bool startBubbleCamera(int frames) {
        CameraDirector* d = director();
        if (d == nullptr || sTarget == nullptr) {
            return false;
        }
        if (chunk() == nullptr) {
            if (!sNoChunk) mp_log("spectate: no watch camera in this scene (no more attempts)");
            sNoChunk = true;
            return false;
        }
        sTarget->setLocalBubble();
        CameraMpWatch::sRestartToken++;
        ActorCameraInfo info = ActorCameraInfo();
        MR::startEventCamera(&info, kEvent, CameraTargetArg(sTarget, nullptr, nullptr, nullptr), frames);
        sCamUp = true;
        sBubbleCam = true;
        return true;
    }

    void endCamera(int frames) {
        sBubbleCam = false;
        if (!sCamUp) {
            return;
        }
        sCamUp = false;
        if (director() != nullptr && chunk() != nullptr) {
            ActorCameraInfo info = ActorCameraInfo();
            MR::endEventCamera(&info, kEvent, true, frames);
        }
    }

    // ---- who can be watched ------------------------------------------------------------------------------------------------
    bool canWatch(int s, bool race) {
        if (s == mp_local_slot() || !mp_slot_present(s)) return false;
        MpPose p;
        if (!MpPuppets::lastPose(s, &p) || p.sceneSerial != mp_scene_serial()) return false;
        if (p.flags & (MP_POSE_DEAD | MP_POSE_BUBBLE)) return false;
        MpPuppetBase* pup = MpPuppetsInt::puppet(s);
        if (pup == nullptr || MR::isDead(pup) || !pup->isDrawable()) return false;
        if (race && (MpRace::slotFinished(s) || MpPuppetsInt::style() == MP_PUPPET_HIDDEN)) return false;
        return true;
    }

    // a touch on a party strip head (the heads sit in slot order, the local player left out)
    int stripPick() {
        if (!MR::testCorePadTriggerA(WPAD_CHAN0) || !MR::isStarPointerInScreen(0)) return -1;
        const TVec2f* p = MR::getStarPointerScreenPosition(0);
        if (p == nullptr) return -1;
        const MpPanel::Screen scr = MpPanelDraw::screen();
        const float px = p->x * (scr.width / static_cast<float>(MR::getScreenWidth())), py = p->y;
        int i = 0;
        for (int s = 0; s < MP_MAX_PLAYERS && i < MpPanel::kMaxStrip; s++) {
            if (s == mp_local_slot() || !mp_slot_present(s)) continue;
            const float hx = MpPanel::layoutToSquareX(scr, MpPanel::kStripX + i * MpPanel::kStripStep);
            const float hy = MpPanel::layoutToSquareY(scr, MpPanel::kStripY);
            const float r = MpPanel::kStripScale * 100.0f * 0.6f;
            if (px > hx - r && px < hx + r && py > hy - r && py < hy + r) return s;
            i++;
        }
        return -1;
    }

    // ---- the race hold's wipe --------------------------------------------------------------------------------------------
    void raceOpen() {
        MR::openWipeCircle(kWipeFrames);
        sRacePhase = RP_OPENING;
        sRaceTimer = kWipeFrames + 5;
        MpOverlay::hideWaiting(true);
    }
    void raceClose() {
        MR::closeWipeCircle(kWipeFrames);
        sRacePhase = RP_CLOSING;
        sRaceTimer = kWipeFrames + 5;
    }
    void raceStep() {
        if (sRacePhase == RP_OPENING && --sRaceTimer <= 0) {
            sRacePhase = RP_OPEN;
        } else if (sRacePhase == RP_CLOSING && --sRaceTimer <= 0 && !MR::isWipeActive()) {
            sRacePhase = RP_CLOSED;
            MpOverlay::hideWaiting(false);
            endCamera(0);   // (behind the closed wipe: a cut)
            sWatch = -1;
            mp_log("spectate: race view closed");
        }
    }

    // everything back to the player's own view at once (the session ended, the scene is going)
    void dropAll(const char* why) {
        const bool wasBubble = sBubbleCam;
        if (sRacePhase != RP_CLOSED) {
            MR::forceCloseWipeCircle();
            sRacePhase = RP_CLOSED;
        }
        MpOverlay::hideWaiting(false);
        if (sCamUp) {
            endCamera(0);
        }
        if (sWatch >= 0 || sLabelOn || wasBubble) mp_log("spectate: view dropped (%s)", why);
        labelOff();
        sWatch = -1;
        sBubbleCam = false;
        sRaceLatch = false;
        clearTransient();
        sSel.reset();
    }
}  // namespace

namespace MpSpectate {

    void sceneInit() {
        sTarget = nullptr;
        sSceneUp = false;
        sWatch = -1;
        sCamUp = false;
        sBubbleCam = false;
        sLabelOn = false;
        sHintShown = false;
        sRacePhase = RP_CLOSED;
        sRaceLatch = false;
        sNoChunk = false;
        clearTransient();
        sSel.reset();
        if (!MpGame::sessionHeld()) return;
        // (coordinator) SMG_MP_SPECTATE=0: no watching at all (waiting players keep their own view), for hardware A/B and as a
        // safety valve: the follow camera has not been seen across a planet change yet.
        const char* off = getenv("SMG_MP_SPECTATE");
        if (off != nullptr && off[0] == '0') {
            mp_log("spectate off (SMG_MP_SPECTATE=0)");
            return;
        }
        sTarget = new MpWatchTarget();
        sSceneUp = true;
    }

    void sceneEnd() {
        // (the camera director, the target and the wipe die with the scene: nothing is touched, only forgotten)
        MpOverlay::hideWaiting(false);
        sTarget = nullptr;
        sSceneUp = false;
        sWatch = -1;
        sCamUp = false;
        sBubbleCam = false;
        sLabelOn = false;
        sRacePhase = RP_CLOSED;
        sRaceLatch = false;
        sNoChunk = false;
        clearTransient();
        sSel.reset();
    }

    bool watching() { return sCamUp && sWatch >= 0; }

    bool bubbleCamAvailable() { return sSceneUp && sTarget != nullptr && !sNoChunk && chunk() != nullptr; }

    bool onLocalBubbleLeave() {
        if (sCamUp && sBubbleCam && sWatch < 0) {
            const int frames = (MR::isDemoActive() || !mp_active()) ? 0 : kBubbleBlend;
            endCamera(frames);
            mp_log("spectate: bubble follow camera off (pop)");
            return true;
        }
        return false;
    }

    bool badgeModel(MpPanel::Badge* out) {   // MP3 ingame
        const bool on = sLabelOn && sCamUp && sWatch >= 0 && sLabelSlot >= 0 && sLabelSlot < MP_MAX_PLAYERS && mp_slot_present(sLabelSlot) &&
                        !MpUi::isMenuOpen();
        sBadgeA += on ? 0.125f : -0.125f;
        sBadgeA = sBadgeA < 0.0f ? 0.0f : (sBadgeA > 1.0f ? 1.0f : sBadgeA);
        if (out == nullptr || sBadgeA <= 0.0f || sLabelSlot < 0 || sLabelSlot >= MP_MAX_PLAYERS) return false;
        const int look = mp_slot_look(sLabelSlot);
        out->look = static_cast<uint8_t>(look >= 0 && look < MpPanel::kCharacters ? look : 0);
        out->alpha = sBadgeA;
        out->canSwitch = sCanSwitch;
        return true;
    }
    int watchedSlot() { return sCamUp ? sWatch : -1; }
    bool selectSlot(int slot) {
        if (!sSceneUp || slot < 0 || slot >= MP_MAX_PLAYERS) return false;
        sPickSlot = slot;
        return true;
    }

    bool lodDistance(float x, float y, float z, float* out) {
        if (!sCamUp || (sWatch < 0 && !sBubbleCam) || sTarget == nullptr) return false;
        const TVec3f& p = sTarget->getPosition();
        const float dx = x - p.x, dy = y - p.y, dz = z - p.z;
        *out = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        return true;
    }

    bool raceHoldTick(bool results) {
        if (!raceNow()) return false;
        if (results) {
            if (sRaceLatch) {
                sRaceLatch = false;
                if (sRacePhase == RP_OPEN || sRacePhase == RP_OPENING) sRaceCloseWanted = true;
            }
            return false;
        }
        sRaceLatch = true;
        return sRacePhase == RP_OPEN || sRacePhase == RP_OPENING;
    }

    void raceBackdrop(bool want) {
        if (!raceNow()) return;
        if (want && sRacePhase == RP_CLOSED) raceOpen();
        else if (!want && (sRacePhase == RP_OPEN || sRacePhase == RP_OPENING)) raceClose();
    }

    bool readyToRelease() { return sRacePhase == RP_CLOSED; }

    void update() {
        if (!sSceneUp || !mp_active()) {
            // wave8: during a brief reconnect while locally bubbled, keep the local bubble-follow camera alive (only drop teammate spectate).
            if (sSceneUp && MpGame::sessionHeld() && MpBubble::localBubbled() && sCamUp && sBubbleCam && sWatch < 0) {
                return;
            }
            if (sCamUp || sLabelOn || sRacePhase != RP_CLOSED) dropAll(!mp_active() ? "session over" : "no scene");
            return;
        }
        ++sTurn;
        const int local = mp_local_slot();
        const bool race = raceNow();

        // what the local player waits for
        MpSpectate::SelIn in;
        in.local = local;
        in.allowOwn = true;
        int prefer = -1;
        float dist = -1.0f;
        const int wait = race ? 0 : MpBubble::localWait(&prefer, &dist);
        if (wait == 1 || wait == 2) {
            in.reason = wait == 2 ? MpSpectate::REASON_OUT : MpSpectate::REASON_BUBBLE;
            // wave8: with the dedicated bubble-follow camera tracking the player smoothly across planets and fixed-camera zones,
            // keep the camera on your own bubble while you have lives (auto-spectate only when out of lives; L/R still peeks anytime).
            in.startOk = (wait == 2);
            in.prefer = prefer;
        } else if (race && sRaceLatch && MpRace::isRaceScene() && MpRace::finishedLocal() && !MpRace::resultsKnown()) {
            in.reason = MpSpectate::REASON_RACE;
            in.startOk = sRacePhase == RP_CLOSED;   // (a view that is still closing waits)
            in.allowOwn = false;
        }
        if (sNoChunk) in.reason = MpSpectate::REASON_NONE;   // (no watch camera in this scene: nothing to wait for)
        in.eventBlocked = !(sCamUp && sWatch >= 0) && in.reason != MpSpectate::REASON_NONE && gameEventRunning();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) in.canWatch[s] = in.reason != MpSpectate::REASON_NONE && canWatch(s, race);

        // input: L (Z) and R (the spin) step, a touch on a head picks
        const bool z = MR::testSubPadButtonZ(WPAD_CHAN0);
        const bool left = z && !sZPrev;
        sZPrev = z;
        const bool right = MR::isCorePadSwingTrigger(0);
        const bool busy = MpUi::isMenuOpen() || mp_dialog_active();
        if (!busy && in.reason != MpSpectate::REASON_NONE) {
            in.step = right && !left ? 1 : (left && !right ? -1 : 0);
            in.pick = sPickSlot >= 0 ? sPickSlot : stripPick();
        }
        sPickSlot = -1;

        // a race view that was told to close (the results are in)
        if (sRaceCloseWanted) {
            sRaceCloseWanted = false;
            if (sRacePhase == RP_OPEN || sRacePhase == RP_OPENING) raceClose();
        }

        const MpSpectate::SelOut o = sSel.step(in);
        {   // MP3 ingame: the badge's L / R pictures only when another teammate can be watched (or the own view chosen)
            int n = 0;
            for (int k = 0; k < MP_MAX_PLAYERS; k++) n += in.canWatch[k] ? 1 : 0;
            sCanSwitch = n > 1 || (n == 1 && in.allowOwn);
        }
        if (o.blocked) {
            if (!sBlockedLogged) {
                sBlockedLogged = true;
                mp_log("spectate: start deferred (a game event camera or demo is running)");
            }
        } else {
            sBlockedLogged = false;
        }
        switch (o.ev) {
        case MpSpectate::EV_START:
            if (sCamUp && sBubbleCam && cameraIsOurs() && sTarget != nullptr && !race) {
                sTarget->setSlot(o.target);
                CameraMpWatch::sRestartToken++;
                MR::startCameraInterpolation(kBlendSwitch);
                sBubbleCam = false;
                sWatch = o.target;
                labelOn(o.target);
                sHintShown = true;
                mp_log("spectate: start watching slot=%d reason=%d (from bubble)", o.target, (int)in.reason);
            } else if (startCamera(o.target, race ? 0 : kBlendIn)) {
                sWatch = o.target;
                labelOn(o.target);
                if (race) raceOpen();
                sHintShown = true;   // (MP3 ingame: no hint sentence; the badge shows the L and R pictures)
                mp_log("spectate: start watching slot=%d reason=%d", o.target, (int)in.reason);
            } else {
                sSel.reset();
            }
            break;
        case MpSpectate::EV_SWITCH:
            if (sCamUp && sTarget != nullptr) {
                sTarget->setSlot(o.target);
                CameraMpWatch::sRestartToken++;
                if (!race || sRacePhase == RP_OPEN) MR::startCameraInterpolation(kBlendSwitch);
                sWatch = o.target;
                labelOn(o.target);
                if (!o.forced) MR::startSystemSE("SE_SY_CAMERA_MOVE", -1, -1);
                mp_log("spectate: switch to slot=%d%s", o.target, o.forced ? " (forced)" : "");
            }
            break;
        case MpSpectate::EV_OWN:
            if (sCamUp && cameraIsOurs() && sTarget != nullptr && !race && MpBubble::localBubbled() &&
                MpGameInternal::sceneState() == MP_SCENE_ACTION && !gameEventRunning()) {
                sTarget->setLocalBubble();
                CameraMpWatch::sRestartToken++;
                MR::startCameraInterpolation(kBlendSwitch);
                sBubbleCam = true;
                sWatch = -1;
                labelOff();
                MR::startSystemSE("SE_SY_CAMERA_MOVE", -1, -1);
                mp_log("spectate: own bubble view (chosen)");
            } else {
                endCamera(kBlendOut);
                sWatch = -1;
                labelOff();
                MR::startSystemSE("SE_SY_CAMERA_MOVE", -1, -1);
                mp_log("spectate: own view (chosen)");
            }
            break;
        case MpSpectate::EV_END:
        case MpSpectate::EV_LOST:
            labelOff();
            if (o.ev == MpSpectate::EV_LOST && !race && MpBubble::localBubbled() && sCamUp && cameraIsOurs() && sTarget != nullptr &&
                MpGameInternal::sceneState() == MP_SCENE_ACTION && !gameEventRunning()) {
                sTarget->setLocalBubble();
                CameraMpWatch::sRestartToken++;
                MR::startCameraInterpolation(kBlendSwitch);
                sBubbleCam = true;
                sWatch = -1;
                mp_log("spectate: nobody left to watch (own bubble)");
            } else if (race && sRacePhase != RP_CLOSED) {
                if (sRacePhase != RP_CLOSING) raceClose();   // the camera ends when the wipe has closed
                sWatch = -1;
            } else {
                endCamera(kBlendOut);
                sWatch = -1;
                mp_log("spectate: %s", o.ev == MpSpectate::EV_END ? "back in play" : "nobody left to watch");
            }
            break;
        default:
            break;
        }

        // wave8: dedicated smooth bubble-follow camera whenever locally bubbled and not spectating a teammate
        if (sWatch < 0) {
            const bool wantBubble = !race && MpBubble::localBubbled() && !sNoChunk && MpGameInternal::sceneState() == MP_SCENE_ACTION &&
                                    !gameEventRunning();
            if (wantBubble && !sBubbleCam) {
                if (startBubbleCamera(kBubbleBlend)) {
                    mp_log("spectate: bubble follow camera on");
                }
            } else if (!wantBubble && sBubbleCam) {
                endCamera(MR::isDemoActive() ? 0 : kBubbleBlend);
                mp_log("spectate: bubble follow camera off");
            }
        }

        // a camera another event took over is started again once it is free (a hint of "Watching" while the view is elsewhere is a lie)
        if (sCamUp && (sWatch >= 0 || sBubbleCam) && sRacePhase != RP_CLOSING) {
            if (!cameraIsOurs()) {
                CameraDirector* d = director();
                if (d != nullptr && !d->isEventCameraActive() && !MR::isDemoActive() && ++sRetry >= 10) {
                    sRetry = 0;
                    sCamUp = false;
                    if (sWatch >= 0) {
                        startCamera(sWatch, kBlendSwitch);
                    } else if (sBubbleCam) {
                        startBubbleCamera(kBubbleBlend);
                    }
                }
            } else {
                sRetry = 0;
            }
        }
        if (race) raceStep();
    }

}  // namespace MpSpectate
