// Galaxy 3.0 multiplayer: MpPuppets (owner: puppet). See MpGame.hpp and DESIGN.md section 4 "Puppets".
//
//  - initScene(): after the scene's actor placement: builds the animation-name map, creates one puppet per remote slot (the
//    character from mp_slot_look) and a self puppet when our own character is not the MarioActor's
//    (dead until a STATE for our scene serial arrives), the drawer and the name tags.
//  - update(): once per logic turn: sends our STATE every 2nd turn (30 Hz, unreliable), drives puppet appear/disappear,
//    demo movement, name tags, our cosmetic squash timer.
//  - handle(): STATE (pose buffers), LOOK (logged; looks apply at the next scene), BUMP (victim side of stomp/spin),
//    EMOTE / POINTER (MpCoop.cpp: greeting emote, pointer ping).
//  - endScene(): forgets every scene object (never kept across scenes).
//  - addArchives(): SceneDataInitializer hook: the other character's model archive when a remote player looks like him.
//  - drop-in (MpDropInLogic.hpp): the engine takes no new actor mid-scene, so initScene also makes one dead "spare" puppet per
//    character nobody uses yet (drop-in allowed, a free seat, memory guard); update() binds a joiner (or a LOOK change) to the
//    puppet of his character, which grows and fades in. No puppet of his character: hidden until the next scene (never a
//    stand-in that looks like somebody else). On the joiner's own console: hidden from the others until it has warped to the
//    host (or a bubble hook took over), so the others see it arrive beside them.
#include "port/mp_diag.h"      // beta diagnostics: the session totals handed to the summary
#include "port/mp_nettune.h"   // netlog: the timing knobs and the statistics interval
#include "port/mp_test_env.h"   // worldsync: desktop-only test switches (nothing on the console)
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/MP/MpBubble.hpp"   // MP3 bubble2: spin / Star Bit pop requests (BUMP kind MP_BUMP_POP)
#include "Game/MP/MpDropInLogic.hpp"
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpCoop.hpp"   // MP3 coop: greeting emote, pointer ping
#include "Game/MP/MpCoopLogic.hpp"   // mppause: pushHeld
#include "Game/MP/MpDemo.hpp"   // MP3 demo: the puppets also run for the puppet demo (mp_active() || MpDemo::live())
#include "Game/MP/MpFormation.hpp"   // MP3 formation: personal space, the star-get photo
#include "Game/MP/MpLod.hpp"   // MP3 lod2: puppet level of detail by distance
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpSelfPuppet.hpp"
#include "Game/MP/MpQuality.hpp"   // MP3 perf3
#include "Game/NameObj/NameObjArchiveListCollector.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Player/MarioState.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/NameObj/NameObjHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/ModelUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <math.h>
#include "Game/System/ResourceHolder.hpp"
#include "Game/System/ResourceInfo.hpp"
#include "Game/Util/CameraUtil.hpp"   // MR::getCamPos (the fly-in diagnostic)
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "port/mp_prof.h"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <cstring>

// netfix B8a: the Vita's C library mishandles 64-bit length modifiers (%llu / %lld): 64-bit counters are logged as %u through this clamp
#ifndef NETFIX_U32C
#define NETFIX_U32C(v) ((unsigned)((uint64_t)(v) > 0xFFFFFFFFull ? 0xFFFFFFFFull : (uint64_t)(v)))
#endif

extern "C" int64_t port_host_time_ns(void);
extern "C" void mp_aids_scene(unsigned serial, unsigned napaBytes, unsigned gddrBytes, unsigned cacheBytes) __attribute__((weak));   // MP3 hwaids: the scene-start memory line (platform/src/mp/mp_hwaids.cpp)

// The player's Xanime tables live in MarioAnimator.cpp's translation unit (MarioAnimatorData.hpp defines them there).
extern XanimeGroupInfo marioAnimeTable[];
extern XanimeAuxInfo marioAnimeAuxTable[];
extern XanimeOfsInfo marioAnimeOfsTable[];
extern XanimeBckTable1 singleAnimeTable[];
extern XanimeBckTable2 doubleAnimeTable[];
extern XanimeBckTable3 tripleAnimeTable[];
extern XanimeBckTable4 quadAnimeTable[];
extern XanimeSwapTable luigiAnimeSwapTable[];

bool gMpHideLocalBody = false;   // MpSelfPuppet.hpp

namespace {
    const uint32_t cStaleTurns = 180;      // 3 s without a STATE: the puppet disappears
    const uint32_t cBumpCooldown = 30;
    const uint32_t cStompRepeatTurns = 180;   // 3 s: repeated stomps from one friend only squash (no reaction animation)
    const f32 cBumpMaxDist = 400.0f;
    const uint16_t cSquashTurns = 30;

    struct SlotState {
        MpPoseBuffer buf;
        bool demoMovement;     // requestMovementOn sent for the current local demo
        uint32_t lastBump;     // turn of the last BUMP we accepted from this slot
        uint32_t lastStomp;    // turn of the last STOMP from this slot that played the reaction (a friend bouncing on our head)
        uint16_t otherSerial;  // last foreign scene serial logged
        bool firstLogged;
        uint32_t loggedHash[4];
    };

    SlotState sSlots[MP_MAX_PLAYERS];

    // netlog: what one remote player's pose stream looked like since the last "[mp] net: slot=" line. Game thread only; plain counters
    // (no allocation, no lock); the line is written once every SMG_MP_NETLOG_SEC seconds (default 5, 0 = off).
    struct NetStat {
        int64_t lastArrNs;                    // wall clock of the previous STATE of this slot (0: none yet in this window's run)
        uint32_t rx, late, dup, stall;        // STATEs taken in; arrived after a newer one (out of order); refused as duplicates / too old; gaps over 100 ms
        float gapSumMs, gapMaxMs;             // the time between two arrivals
        uint32_t smp, interp, extrap, hold;   // samplePose calls that drew the puppet, and how: inside the buffer / guessed ahead / held still
        float depthSumMs, depthMaxMs;         // how far the drawn pose is behind the newest pose received (the buffer delay in use)
        uint32_t snaps0;                      // the buffer's own snap count at the window's start
    };
    NetStat sNet[MP_MAX_PLAYERS];
    // beta diagnostics: the whole session's pose-stream totals over the remote players (counters only; pushed to the port about once a second)
    MpDiagPose sDiag;
    uint32_t sDiagSnap0[MP_MAX_PLAYERS];
    int64_t sDiagPushNs;
    int64_t sNetWinNs;                        // the window's start (0: not started)
    bool sNetWasActive;

    void netStatReset(int s) {
        NetStat& n = sNet[s];
        const uint32_t snaps = sSlots[s].buf.snaps;
        memset(&n, 0, sizeof n);
        n.snaps0 = snaps;
    }
    // MP3 integration: the last pose per slot (remote: last STATE received; local: last STATE sent) for MpPuppets::lastPose.
    // MP3 profile: decoded once when it is stored (a few times a second), not on every lastPose call (the bubble, party strip,
    // flow and drop-in ask several times per slot every turn). 0: none this process, 1: decoded, 2: stored bytes did not decode.
    MpPose sLastPose[MP_MAX_PLAYERS];
    uint8_t sLastPoseState[MP_MAX_PLAYERS];

    void storeLastPose(int slot, const uint8_t* data, size_t len) {
        if (len > MP_POSE_WIRE_BYTES + MP_POSE_TAIL_BYTES) return;   // (as before: a longer payload was not kept)
        MpReader r(data, len);
        MpPose p;
        if (mp_read_pose(r, &p)) {
            sLastPose[slot] = p;
            sLastPoseState[slot] = 1;
        } else {
            sLastPoseState[slot] = 2;
        }
    }
    MpPuppetBase* sPuppets[MP_MAX_PLAYERS];     // scene objects: valid only while sSceneLive (local slot: the self puppet)

    // MP3 netfeel: one "net link" line per remote slot at most every 10 s, from the game thread (the pose buffers are its own: no lock): what the
    // link carried (datagrams in / out a second, bytes in a second: mp_core_link_counts) and what the pose buffer did since the last line (its
    // delay now in ms, STATEs received, dropped, error-correction snaps). With the "[mp] status" and "net ad hoc" lines, this is what tells
    // a choppy remote Mario from log.txt.
    // The marks of every remote slot move on at each line, also for a slot that has no puppet in this scene (a player in another galaxy:
    // its link still carries traffic), and after a break of more than 30 s (no scene, no session: this is not called) the marks are only
    // taken, with no line: a line's rates are always those of the time since the marks were taken.
    struct LinkSeen { uint32_t dgIn, bytesIn, dgOut, received, dropped, snaps; };
    LinkSeen sLinkSeen[MP_MAX_PLAYERS];
    int64_t sLinkAtNs = 0;

    void netLinkLine(int64_t nowNs) {
        if (sLinkAtNs == 0) sLinkAtNs = nowNs;
        const int64_t span = nowNs - sLinkAtNs;
        if (span < 10000000000LL) {
            return;
        }
        sLinkAtNs = nowNs;
        const bool quiet = span > 30000000000LL;
        const double secs = (double)span / 1e9;
        const int me = mp_local_slot();
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == me) {
                continue;
            }
            LinkSeen& o = sLinkSeen[s];
            uint32_t dgIn = 0, bytesIn = 0, dgOut = 0;
            if (!mp_core_link_counts(s, &dgIn, &bytesIn, &dgOut)) {
                continue;
            }
            const MpPoseBuffer& b = sSlots[s].buf;
            const uint32_t rx = b.received, dr = b.dropped, sn = b.snaps;
            const uint32_t dIn = dgIn - o.dgIn, dBytes = bytesIn - o.bytesIn, dOut = dgOut - o.dgOut;
            // (a buffer that was reset began again at 0)
            const uint32_t dRx = rx >= o.received ? rx - o.received : rx, dDr = dr >= o.dropped ? dr - o.dropped : dr;
            const uint32_t dSn = sn >= o.snaps ? sn - o.snaps : sn;
            o.dgIn = dgIn; o.bytesIn = bytesIn; o.dgOut = dgOut; o.received = rx; o.dropped = dr; o.snaps = sn;
            if (quiet || sPuppets[s] == nullptr || (dIn == 0 && dOut == 0 && dRx == 0)) {
                continue;   // (after a break, a player not in this scene, a slot nothing flows to: no line)
            }
            mp_log("net link slot=%d: in %.1f/s %.0f B/s, out %.1f/s, pose delay %.0f ms, rx %u drop %u snap %u", s, dIn / secs, dBytes / secs,
                   dOut / secs, (double)b.delay() * (1000.0 / 60.0), (unsigned)dRx, (unsigned)dDr, (unsigned)dSn);
        }
    }
    bool sSceneLive;
    uint32_t sTurn;
    uint32_t sSceneStartTurn, sLastDemoTurn;    // mppause: the push-free arrival window
    bool sLastDemoSeen;
    uint8_t sSelfDemoLogs;
    uint16_t sSquash;
    uint16_t sSceneSerial;
    MpAnimNameMap sNames;
    XanimeResourceTable* sOtherTable;           // scene heap
    bool sOtherTableLuigi;
    uint32_t sSent;
    bool sSendLogged;
    // (perf) STATE pacing by wall-clock time (MpSendGate): 30 Hz at 60 turns/s as before, and no flood when turns run faster
    // than real time (a skipped cutscene: by turn count hundreds of STATEs per second, over the host's 200 messages/s/guest cap
    // -> "bad message" -> kicked).
    MpSendGate sSendGate;
    uint32_t sPictures;

    // ---- drop-in (scene objects: valid only while sSceneLive)
    MpPuppetBase* sPool[MP_LOOK_COUNT];        // this scene's remote puppets by character, bound to a slot or spare
    MpDropIn::Binding sBind;
    int8_t sHiddenLogged[MP_MAX_PLAYERS];      // character logged as "no puppet" for a slot (-1 none)
    int sSpares;
    // the joiner's own arrival
    bool sArrivePending;                       // we dropped into a scene the others already play: hidden to them until warped
    uint16_t sArriveHold;                      // turns still hidden after the warp (the others' jitter buffer)
    uint32_t sArriveTurns;                     // action turns spent waiting for a safe point
    uint16_t sLastInitSerial = 0xFFFF;         // a reload of the same serial (a miss) is not an arrival
    MpDropInHooks::ArrivalFn sArrivalHandler;  // the bubble agent's hook (null: warp to the host)
    bool sHeapLogged;

    bool validSlot(int s) { return s >= 0 && s < MP_MAX_PLAYERS; }

    // MP3 netsmooth: SMG_MP_POSELOG=1 (desktop test switch, env.txt on the console): "[mp] poselog ..." lines for the pose
    // smoothing measurements (tools/mp/netsmooth): tx = our own pose every turn (s=1: sent this turn), in = a STATE arrived,
    // rx = what a puppet shows this turn (st = the sender turn it renders). Off (the default): one cached bool test.
    bool poseLogOn() {
        static int sOn = -1;
        if (sOn < 0) {
            const char* e = getenv("SMG_MP_POSELOG");
            sOn = (e != nullptr && *e == '1') ? 1 : 0;
        }
        return sOn == 1;
    }
    unsigned poseLogMs() { return (unsigned)(port_host_time_ns() / 1000000); }   // (netfix B8a: %u, the Vita libc mishandles %llu; wraps after 49 days)
    void poseLog(const char* kind, int slot, uint32_t turn, uint32_t now, float st, int r, const MpPoseF& p) {
        mp_log("poselog %s slot=%d t=%u now=%u ms=%u st=%.2f r=%d p=%.2f,%.2f,%.2f q=%.4f,%.4f,%.4f,%.4f v=%.1f,%.1f,%.1f f=%04x a=%08x fr=%.2f",
               kind, slot, (unsigned)turn, (unsigned)now, poseLogMs(), st, r, p.pos[0], p.pos[1], p.pos[2], p.quat[0], p.quat[1], p.quat[2],
               p.quat[3], p.vel[0], p.vel[1], p.vel[2], (unsigned)p.flags, (unsigned)p.anim, p.frame);
    }

    const char* characterName(int c) { return mp_character_name(c); }

    const char* characterArchive(int c, bool localLuigi) { return mp_character_archive(c, localLuigi); }

    // What the MarioActor itself looks like (the save file's character).
    int actorCharacter(bool localLuigi) { return localLuigi ? MP_LOOK_LUIGI : MP_LOOK_MARIO; }

    MpPuppetBase* newPuppet(int slot, int character, bool self) {   // (slot -1: a drop-in spare)
        MpPuppetBase* p;
        if (mp_character_is_toad(character)) {
            p = new MpToadPuppet(slot, (u8)character, self);
        } else {
            p = new MpRemotePlayer(slot, (u8)character, self);
        }
        p->initWithoutIter();
        return p;
    }

    // ---- drop-in helpers
    struct HeapFree {
        uint32_t napa, gddr, cache;
    };
    uint32_t freeOf(JKRHeap* h) {
        if (h == nullptr) {
            return 0;
        }
        const s32 f = h->getFreeSize();
        return f > 0 ? (uint32_t)f : 0u;
    }
    HeapFree heapFree() {
        HeapFree f;
        f.napa = freeOf(MR::getSceneHeapNapa());
        f.gddr = freeOf(MR::getSceneHeapGDDR3());
        f.cache = freeOf(SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap);
        return f;
    }
    // what a puppet's instance costs in the scene heap beyond its archive, measured on the desktop build (64-bit objects; the
    // Vita's are smaller): see the "puppet heap" lines.
    const uint32_t cPuppetCost = 0x30000;
    // fix2: free places left in the scene's NameObjHolder (capacity fixed at 0x1300, unchecked push_back); -1 = unknown
    int objSlotsLeftNow() {
        GameSystem* sys = SingletonHolder< GameSystem >::get();
        NameObjHolder* h = sys != nullptr && sys->mSceneController != nullptr ? sys->mSceneController->mObjHolder : nullptr;
        return h != nullptr ? h->getObjCapacity() - h->getObjCount() : -1;
    }
    // memfix: a puppet's instance is a game-side `new` (a failure is an OSPanic): a present player's is guarded like a spare's. The
    // scene heap must keep kKeepNapa (3 MiB) after it; else that player has no puppet this scene: his name tag (at his last
    // pose) and the party strip show him, everything that indexes puppets tolerates null, and the next scene tries again.
    // SMG_MP_TEST_NO_PUPPET_MEM=1 (desktop test switch) makes every puppet instance (self, present, spare) fail this check.
    bool testNoPuppetMem() {
        static const bool on = [] {
            const char* e = MP_TEST_GETENV("SMG_MP_TEST_NO_PUPPET_MEM");
            return e != nullptr && *e != 0 && atoi(e) != 0;
        }();
        return on;
    }
    bool puppetFits() {
        if (testNoPuppetMem()) return false;
        if (!MpDropIn::objSlotsOk(objSlotsLeftNow())) return false;   // fix2: the NameObjHolder (fixed 0x1300) is nearly full
        return MpDropIn::spareFits(freeOf(MR::getSceneHeapNapa()), cPuppetCost);
    }

    // The character's model archive is mounted (or needs none). integ (B1): NEVER mounted from here: this runs on the scene init thread,
    // where a synchronous mount (the file loader wakes the main thread's executor, the heap is the scene's) ended in a thread with no
    // entry on one console and a null archive on the other. An archive the archive-request phase did not get mounted (addArchives: the
    // present players' and the spares') means: no puppet of that character this scene (a present player: hidden, name tag and strip
    // still show him; a spare: not made). spare: a puppet nobody uses yet (a present player's puppet is always made, as before).
    bool ensureArchive(int c, bool localLuigi, bool spare, const char** pWhy) {
        const char* arc = characterArchive(c, localLuigi);
        JKRHeap* napa = MR::getSceneHeapNapa();
        const uint32_t napaFree = freeOf(napa);
        const uint32_t cacheFree = freeOf(SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap);
        const bool mounted = arc == nullptr || MR::isLoadedObjectOrLayoutArchive(arc);
        const uint32_t cost = spare ? cPuppetCost : 0u;
        const bool fits = MpDropIn::spareFits(napaFree, cost) && !testNoPuppetMem() && MpDropIn::objSlotsOk(objSlotsLeftNow());
        if (spare || !mounted) {
            mp_log("puppet archive decide character=%s arc=%s mounted=%d cost=%u napa=%u cache=%u fits=%d", characterName(c),
                   arc != nullptr ? arc : "-", mounted ? 1 : 0, (unsigned)cost, (unsigned)napaFree, (unsigned)cacheFree, fits ? 1 : 0);
        }
        if (!mounted) {
            *pWhy = "notrequested";
            return false;
        }
        if (spare && !fits) {
            *pWhy = "memory";
            return false;
        }
        return true;
    }

    MpDropIn::SceneRoster sHeldRoster = {};
    bool sHeldRosterValid = false;

    MpDropIn::SceneRoster sceneRoster() {
        if (!mp_active() && !MpDemo::live() && MpGame::sessionHeld() && sHeldRosterValid) {
            return sHeldRoster;
        }
        MpDropIn::SceneRoster r;
        for (int s = 0; s < MP_MAX_PLAYERS && s < MP_DROPIN_SLOTS; s++) {
            r.present[s] = mp_slot_present(s);
            r.look[s] = (uint8_t)mp_slot_look(s);
        }
        r.localSlot = mp_local_slot();
        r.dropInAllowed = (mp_settings()->flags & 1) != 0;
        r.maxPlayers = mp_settings()->maxPlayers;
        if (mp_active()) {
            sHeldRoster = r;
            sHeldRosterValid = true;
        }
        return r;
    }

    // ---- the joiner's own arrival: beside the host (or the first player found), like the session menu's "Warp to"
    void arrivalStep() {
        if (!sArrivePending) {
            if (sArriveHold != 0) {
                sArriveHold--;
            }
            return;
        }
        if (!mp_active() || mp_mode() != MP_MODE_TOGETHER) {
            sArrivePending = false;
            return;
        }
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION || MR::isDemoActive() || MR::isPlayerDead() || MR::isPlayerInBind()) {
            return;   // the scenario opening, a demo: still hidden to the others
        }
        const char* why = nullptr;
        if (++sArriveTurns > 600) {
            why = "timeout";
        }
        const int me = mp_local_slot();
        int target = -1;
        MpPose them;
        const int host = mp_host_slot();
        for (int k = -1; k < MP_MAX_PLAYERS && target < 0 && why == nullptr; k++) {
            const int s = k < 0 ? host : k;
            if (!validSlot(s) || s == me || !mp_slot_present(s)) {
                continue;
            }
            if (MpPuppets::lastPose(s, &them) && them.sceneSerial == sSceneSerial && (them.flags & MP_POSE_VISIBLE) != 0 &&
                (them.flags & (MP_POSE_DEAD | MP_POSE_IN_DEMO)) == 0) {
                target = s;
            }
        }
        if (why == nullptr && target < 0) {
            return;   // nobody to go to yet (in a demo, dead): wait
        }
        if (why == nullptr && sArrivalHandler != nullptr && sArrivalHandler(target)) {
            mp_log("puppet dropin arrive handler to=%d", target);
            sArrivePending = false;
            sArriveHold = 0;
            return;
        }
        // (positions are global in a scene: every zone is placed in one world, the STATE zone is 0xFF; no zone test)
        float at[3];
        if (why == nullptr && !MpDropIn::arrivalPoint(them.pos, them.quat, at)) {
            why = "pose";
        }
        if (why != nullptr) {
            mp_log("puppet dropin arrive stay reason=%s", why);
        } else {
            MR::setPlayerPos(TVec3f(at[0], at[1], at[2]));   // (as MpFlow's "Warp to": a teleport; he drops onto the ground)
            mp_log("puppet dropin arrive warp to=%d pos=%.0f,%.0f,%.0f", target, at[0], at[1], at[2]);
        }
        sArrivePending = false;
        sArriveHold = why != nullptr ? 0 : 12;   // the others render 100..200 ms behind: no glimpse of the start point
    }

    const char* bumpName(uint8_t k) {
        switch (k) {
        case MP_BUMP_STOMP: return "stomp";
        case MP_BUMP_SPIN: return "spin";
        case MP_BUMP_PUSH: return "push";
        default: return "?";
        }
    }

    void resetSlot(int s) {
        sSlots[s].buf.reset();
        netStatReset(s);
        sSlots[s].demoMovement = false;
        sSlots[s].lastBump = 0;
        sSlots[s].lastStomp = 0;
        sSlots[s].otherSerial = 0xFFFF;
        sSlots[s].firstLogged = false;
        for (int i = 0; i < 4; i++) {
            sSlots[s].loggedHash[i] = 0;
        }
    }

    void buildNameMap(MarioActor* a) {
        sNames.clear();
        XanimeResourceTable* t = a->getResourceTable();
        if (t == nullptr || t->mResourceHolder == nullptr || t->mResourceHolder->mMotionResTable == nullptr) {
            sNames.finish();
            return;
        }
        ResTable* motions = t->mResourceHolder->mMotionResTable;
        for (u32 i = 0; i < motions->mCount; i++) {
            const char* name = motions->getResName(i);
            sNames.add(MpPoseMath::animHash(name), name);
        }
        sNames.finish();
    }

    // netlog: SMG_MP_NET_* (port/mp_nettune.h, read and logged when the session began): a key that was not given leaves the constant.
    void applyNetTune() {
        const MpNetTune& t = MpNetTune::cur();
        MpPoseBuffer::resetTune();
        MpSendGate::tMinNs = MpSendGate::kMinNs;
        if (!mp_active()) return;   // (a session that ended: back to the constants)
        if (t.set & MpNetTune::DELAY_MIN) MpPoseBuffer::tMinDelay = MpNetTune::msToTurns(t.delayMinMs);
        if (t.set & MpNetTune::DELAY_MAX) MpPoseBuffer::tMaxDelay = MpNetTune::msToTurns(t.delayMaxMs);
        if (t.set & MpNetTune::EXTRAP) MpPoseBuffer::tMaxExtrap = MpNetTune::msToTurns(t.extrapMs);
        if (t.set & MpNetTune::DELAY_EXTRA) MpPoseBuffer::tDelayExtra = MpNetTune::msToTurns(t.delayExtraMs);
        if (t.set & MpNetTune::DELAY_PCT) MpPoseBuffer::tLateQuantile = (float)(100 - t.delayPct) * 0.01f;
        if (t.set & MpNetTune::SEND) MpSendGate::tMinNs = (int64_t)t.sendMs * 1000000;
        if (MpPoseBuffer::tMaxDelay < MpPoseBuffer::tMinDelay) MpPoseBuffer::tMaxDelay = MpPoseBuffer::tMinDelay;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {   // (the prior target was made with the constants)
            sSlots[s].buf.target = sSlots[s].buf.updateTarget();
        }
    }

    // netlog: one line per present remote player every SMG_MP_NETLOG_SEC seconds, then the window starts again. Not for the turn's cost:
    // one clock read a turn, the rest only when the line is due.
    void netLogTick() {
        const int sec = MpNetTune::cur().logSec;
        if (sec <= 0 || !mp_active()) return;
        const int64_t nowNs = port_host_time_ns();
        if (sNetWinNs == 0) { sNetWinNs = nowNs; return; }
        if (nowNs - sNetWinNs < (int64_t)sec * 1000000000ll) return;
        const double span = (double)(nowNs - sNetWinNs) * 1e-9;
        sNetWinNs = nowNs;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == mp_local_slot() || !mp_slot_present(s)) { netStatReset(s); continue; }
            const NetStat& n = sNet[s];
            const MpPoseBuffer& b = sSlots[s].buf;
            const uint32_t gaps = n.rx > 1 ? n.rx - 1 : 1;
            mp_log("net: slot=%d rx %.1f/s gap %.1f/%.0f ms (mean/max) buf %.0f/%.0f ms (mean/max, target %.0f) late=%u dup=%u stall100=%u interp=%u extrap=%u hold=%u snap=%u",
                   s, n.rx / span, n.rx > 1 ? n.gapSumMs / gaps : 0.0f, n.gapMaxMs, n.smp ? n.depthSumMs / n.smp : 0.0f, n.depthMaxMs,
                   b.target * (1000.0f / 60.0f), (unsigned)n.late, (unsigned)n.dup, (unsigned)n.stall, (unsigned)n.interp, (unsigned)n.extrap,
                   (unsigned)n.hold, (unsigned)(b.snaps - n.snaps0));
            const int64_t keepLast = n.lastArrNs;
            netStatReset(s);
            sNet[s].lastArrNs = keepLast;   // (the next window's first gap still counts from the last arrival)
        }
    }

    // beta diagnostics: the session totals to the port (replace, not add), at most once a second, and once more when the session ends.
    void diagPush(bool force) {
        if (!mp_diag_note_pose) return;
        const int64_t nowNs = port_host_time_ns();
        if (!force && nowNs - sDiagPushNs < 1000000000ll) return;
        sDiagPushNs = nowNs;
        MpDiagPose t = sDiag;
        uint32_t snaps = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) snaps += sSlots[s].buf.snaps - sDiagSnap0[s];
        t.snap = snaps;
        mp_diag_note_pose(&t);
    }

    void handleState(const MpMsg& m) {
        int from = m.fromSlot;
        if (!validSlot(from) || from == mp_local_slot()) {
            return;
        }
        MpPoseF p;
        if (!MpPoseMath::readState(m.data, m.len, &p)) {
            MpPuppetsInt::logOnce(from, "drop state reason=parse len", m.len);
            return;
        }
        SlotState& s = sSlots[from];
        if (p.sceneSerial != mp_scene_serial()) {
            if (s.otherSerial != p.sceneSerial) {
                s.otherSerial = p.sceneSerial;
                mp_log("puppet slot=%d other scene serial=%u ours=%u", from, (unsigned)p.sceneSerial, (unsigned)mp_scene_serial());
            }
            return;
        }
        if (s.buf.count > 0 && s.buf.s[s.buf.count - 1].sceneSerial != p.sceneSerial) {
            s.buf.reset();
        }
        {   // netlog: arrival statistics (counters only)
            NetStat& n = sNet[from];
            const int64_t nowNs = port_host_time_ns();
            if (n.lastArrNs != 0 && nowNs > n.lastArrNs) {
                const float gap = (float)(nowNs - n.lastArrNs) * 1e-6f;
                n.gapSumMs += gap;
                if (gap > n.gapMaxMs) n.gapMaxMs = gap;
                if (gap > 100.0f) n.stall++;
                sDiag.gaps++;
                sDiag.gapSumMs += gap;
                if (gap > sDiag.gapMaxMs) sDiag.gapMaxMs = gap;
                if (gap > 100.0f) sDiag.stall100++;
            }
            n.lastArrNs = nowNs;
            if (s.buf.count > 0 && MpPoseMath::turnDiff(p.turn, s.buf.s[s.buf.count - 1].turn) < 0) { n.late++; sDiag.late++; }
            if (s.buf.push(p, sTurn)) { n.rx++; sDiag.arrivals++; } else n.dup++;
        }
        if (poseLogOn()) {
            poseLog("in", from, p.turn, sTurn, 0.0f, 0, p);   // MP3 netsmooth (test log)
        }
        storeLastPose(from, m.data, m.len);
        if (!s.firstLogged) {
            s.firstLogged = true;
            mp_log("puppet slot=%d state first serial=%u", from, (unsigned)p.sceneSerial);
        }
    }

    void handleBump(const MpMsg& m) {
        int from = m.fromSlot;
        if (!validSlot(from) || from == mp_local_slot()) {
            return;
        }
        MpReader r(m.data, m.len);
        uint8_t target = r.u8();
        uint8_t kind = r.u8();
        int8_t dir[3];
        for (int i = 0; i < 3; i++) {
            dir[i] = r.s8();
        }
        (void)dir;   // the direction is recomputed from our own view of both players
        if (!r.done() || kind < MP_BUMP_STOMP || kind > MP_BUMP_PUSH) {
            MpPuppetsInt::logOnce(from, "drop bump reason=parse len", m.len);
            return;
        }
        if ((int)target != mp_local_slot()) {
            return;   // aimed at someone else (the host relays to everyone)
        }
        const char* why = nullptr;
        MarioActor* a = MpPuppetsInt::localActor();
        MpPuppetBase* attacker = sPuppets[from];
        if (!MpPuppetsInt::interactionOn()) {
            why = "off";
        } else if (!sSceneLive || a == nullptr) {
            why = "noscene";
        } else if (attacker == nullptr || MR::isDead(attacker)) {
            why = "nopuppet";
        } else if (sTurn - sSlots[from].lastBump < cBumpCooldown && sSlots[from].lastBump != 0) {
            why = "cooldown";
        } else if (MR::isPlayerDead() || MR::isDemoActive() || MpPuppetsInt::pushHeld(attacker->poseFlags())) {
            why = "busy";
        }
        TVec3f away(0.0f, 0.0f, 0.0f);
        if (why == nullptr) {
            away = a->mPosition;
            away -= attacker->mPosition;
            if (away.length() > cBumpMaxDist) {
                why = "far";
            }
        }
        if (why != nullptr) {
            mp_log("puppet bump recv from=%d kind=%s result=reject reason=%s", from, bumpName(kind), why);
            return;
        }
        sSlots[from].lastBump = sTurn;
        if (sSlots[from].lastBump == 0) {
            sSlots[from].lastBump = 1;
        }

        const char* result = "none";
        const bool onGround = a->getMovementStates()._1;
        if (kind == MP_BUMP_STOMP) {
            // MarioActor::tryAttackMsg(ACTMES_ENEMY_ATTACK_FLIP_VERYWEAK): the no-damage reaction, plus a cosmetic squash for the
            // others (STATE flag)
            // A friend bouncing on our head again and again (one bounce every ~0.7 s): the reaction plays once, the repeats are
            // only the cosmetic squash, so the player underneath is never animation-locked.
            const bool repeat = sSlots[from].lastStomp != 0 && sTurn - sSlots[from].lastStomp < cStompRepeatTurns;
            if (repeat) {
                result = "squash-repeat";
            } else if (onGround && !a->isAnimationRun(nullptr)) {
                sSlots[from].lastStomp = sTurn ? sTurn : 1;
                a->changeAnimation("\x83\x6d\x81\x5b\x83\x5f\x83\x81\x81\x5b\x83\x57");   // "ノーダメージ"
                result = "nodamage";
            } else {
                result = "squash";
            }
            sSquash = cSquashTurns;
        } else if (kind == MP_BUMP_SPIN) {
            // a small knockback, never damage: doFlipWeak (Friendly) / doFlipRot (Rough). resetPlayerModeOnNoDamage inside the
            // flip would take Spring away: skip Spring and Boo, while damaged, and in the air.
            const u16 mode = a->mPlayerMode;
            if (mode == 5 || mode == 6) {
                result = "skip-form";
            } else if (a->mMario->isDamaging()) {
                result = "skip-damaging";
            } else if (!onGround || a->isJumping()) {
                result = "skip-air";
            } else {
                TVec3f v(away);
                if (v.length() < 1.0f) {
                    a->getFrontVec(&v);
                    v = -v;
                }
                v.setLength(30.0f);
                bool ok = mp_settings()->interaction == 2 ? a->mMario->doFlipRot(v) : a->mMario->doFlipWeak(v);
                result = ok ? (mp_settings()->interaction == 2 ? "fliprot" : "flipweak") : "refused";
            }
        }
        mp_log("puppet bump recv from=%d kind=%s result=%s", from, bumpName(kind), result);
    }
}  // namespace

// ---- internal interface ------------------------------------------------------------------------------------------------------
namespace MpPuppetsInt {
    uint32_t turn() { return sTurn; }
    uint32_t pictures() { return sPictures; }
    void notePicture() { sPictures++; }

    MarioActor* localActor() {
        if (!MR::isExistSceneObj(SceneObj_MarioHolder)) {
            return nullptr;
        }
        MarioHolder* h = MR::getMarioHolder();
        return h != nullptr ? h->getMarioActor() : nullptr;
    }

    MpPuppetBase* puppet(int slot) {
        if (!sSceneLive || !validSlot(slot)) {
            return nullptr;
        }
        return sPuppets[slot];
    }

    MpSampleResult samplePose(int slot, MpPoseF* pOut) {
        if (!validSlot(slot) || (!mp_active() && !MpDemo::live())) {
            return MP_SAMPLE_NONE;
        }
        if (slot == mp_local_slot()) {
            // the self puppet: the local player's pose of this very turn, no delay
            if (!sSceneLive || !MpLocalCapture::capture(pOut, sTurn)) {
                return MP_SAMPLE_NONE;
            }
            MarioActor* a = localActor();
            if (a != nullptr && a->mMario != nullptr && a->mMario->isStatusActive(MarioStatus_FpView)) {
                pOut->flags &= (uint16_t)~MP_POSE_VISIBLE;   // first-person view: nothing of the body is drawn
            }
            return MP_SAMPLE_INTERP;
        }
        const MpSampleResult r = sSlots[slot].buf.sample(sTurn, pOut);
        if (r != MP_SAMPLE_NONE) {   // netlog: how the puppet was drawn, and how far behind the newest pose (counters only)
            NetStat& n = sNet[slot];
            const float behind = -sSlots[slot].buf.renderRel(sTurn) * (1000.0f / 60.0f);
            const float d = behind > 0.0f ? behind : 0.0f;
            n.smp++;
            n.depthSumMs += d;
            if (d > n.depthMaxMs) n.depthMaxMs = d;
            if (r == MP_SAMPLE_INTERP) n.interp++; else if (r == MP_SAMPLE_EXTRAP) n.extrap++; else n.hold++;
            sDiag.samples++;
            sDiag.depthSumMs += d;
            if (d > sDiag.depthMaxMs) sDiag.depthMaxMs = d;
            if (r == MP_SAMPLE_EXTRAP) sDiag.extrap++; else if (r != MP_SAMPLE_INTERP) sDiag.hold++;
        }
        if (r != MP_SAMPLE_NONE && poseLogOn()) {   // MP3 netsmooth (test log)
            const MpPoseBuffer& b = sSlots[slot].buf;
            poseLog("rx", slot, b.s[b.count - 1].turn, sTurn, b.renderRel(sTurn), (int)r, *pOut);   // (st: relative to t)
        }
        return r;
    }

    const char* groupBckName(uint32_t hash) {
        XanimeResourceTable* t = MR::getPlayerXanimeResource();
        const XanimeGroupInfo* g = t != nullptr ? t->getGroupInfoFromHash(hash) : nullptr;
        return g != nullptr ? g->mBckName : nullptr;
    }

    const MpAnimNameMap& names() { return sNames; }

    // The local character's puppets share the player's table. The other character gets its own table built on a COPY of
    // marioAnimeTable: XanimeResourceTable::initGroupInfo writes the motions into the group entries it is given.
    XanimeResourceTable* xanimeTable(bool luigi) {
        bool localLuigi = MR::isPlayerLuigi();
        if (luigi == localLuigi) {
            return MR::getPlayerXanimeResource();
        }
        if (sOtherTable != nullptr && sOtherTableLuigi == luigi) {
            return sOtherTable;
        }
        u32 count = 0;
        while (marioAnimeTable[count].mParent.mAnimationName[0] != '\0') {
            count++;
        }
        XanimeGroupInfo* copy = new XanimeGroupInfo[count + 1];
        memcpy(copy, marioAnimeTable, sizeof(XanimeGroupInfo) * (count + 1));
        // (the MarioAnime holder: the same for every character)
        sOtherTable = new XanimeResourceTable(MR::getPlayerXanimeResource()->mResourceHolder, copy, marioAnimeAuxTable, marioAnimeOfsTable,
                                              reinterpret_cast< XanimeBckTable* >(singleAnimeTable), doubleAnimeTable, tripleAnimeTable,
                                              quadAnimeTable, luigi ? luigiAnimeSwapTable : nullptr);
        sOtherTableLuigi = luigi;
        mp_log("puppet xanime table swap=%s groups=%u", luigi ? "Luigi" : "none", (unsigned)count);
        return sOtherTable;
    }

    bool localInFlyInOrDemo() {
        const int st = MpGameInternal::sceneState();
        return st == MP_SCENE_OPENING || st == MP_SCENE_STARTER || MR::isDemoActive() || MR::isPlayerInBind();
    }

    bool pushHeld(uint16_t remoteFlags) {
        return MpCoopLogic::pushHeld(sTurn, sSceneStartTurn, sLastDemoTurn, sLastDemoSeen, localInFlyInOrDemo(),
                                     (remoteFlags & (MP_POSE_IN_DEMO | MP_POSE_IN_BIND)) != 0);
    }

    bool interactionOn() {
        return mp_active() && mp_mode() == MP_MODE_TOGETHER && mp_settings()->interaction != 0;
    }

    int style() {
        if (mp_mode() != MP_MODE_RACE) {
            return MP_PUPPET_SOLID;
        }
        switch (mp_settings()->rivals) {
        case 0: return MP_PUPPET_SOLID;
        case 2: return MP_PUPPET_HIDDEN;
        default: return MP_PUPPET_GHOST;
        }
    }

    void sendBump(int targetSlot, uint8_t kind, const TVec3f& rDir) {
        if (!interactionOn() || !validSlot(targetSlot)) {
            return;
        }
        TVec3f d(rDir);
        if (d.length() > 0.001f) {
            d.setLength(127.0f);
        }
        uint8_t buf[8];
        MpWriter w(buf, sizeof buf);
        w.u8((uint8_t)targetSlot);
        w.u8(kind);
        w.s8((int8_t)MpPoseMath::clampf(d.x, -127.0f, 127.0f));
        w.s8((int8_t)MpPoseMath::clampf(d.y, -127.0f, 127.0f));
        w.s8((int8_t)MpPoseMath::clampf(d.z, -127.0f, 127.0f));
        bool ok = w.ok() && mp_send(MP_MSG_BUMP, buf, (uint16_t)w.n, true);
        mp_log("puppet bump sent target=%d kind=%s ok=%d", targetSlot, bumpName(kind), ok ? 1 : 0);
    }

    uint16_t localSquashTurns() { return sSquash; }

    int spareCount() { return sSceneLive ? sSpares : 0; }

    bool arrivalHidden() { return sArrivePending || sArriveHold != 0; }

    // MP3 puppetprep: MpWarm's tiny-scale in-frustum draw of every puppet (pool spares, already-alive remote puppets, and self puppet),
    // solid or with the see-through list. Each warmDraw saves and restores state; engine-drawn Toads have no warm draw of their own.
    void warmPuppets(int variant) {
        if (!sSceneLive || !MpGame::sessionHeld()) {
            return;
        }
        int n = 0;
        auto warmOne = [&](MpPuppetBase* p) {
            if (p == nullptr || !p->usesDrawer()) {
                return;
            }
            if (n == 0) {
#ifdef TARGET_PC
                port_gx_marker(PORT_GX_MARK_PLAYER_BEGIN);   // mark warm draws (incl. blended ghost) for kSyncMustChara
#endif
            }
            p->warmDraw(variant);
            n++;
        };
        for (int c = 0; c < MP_LOOK_COUNT; c++) {
            warmOne(sPool[c]);
        }
        const int me = mp_active() ? mp_local_slot() : (sHeldRosterValid ? sHeldRoster.localSlot : -1);
        MpPuppetBase* selfP = validSlot(me) ? sPuppets[me] : nullptr;
        if (selfP != nullptr && selfP->isSelf()) {
            warmOne(selfP);
        }
        if (n != 0) {
#ifdef TARGET_PC
            port_gx_marker(PORT_GX_MARK_PLAYER_END);
#endif
            MR::loadLightPlayer();
        }
        mp_log("warm puppets variant=%d drawn=%d", variant, n);
    }

    void logOnce(int slot, const char* what, uint32_t value) {
        if (!validSlot(slot)) {
            return;
        }
        uint32_t* seen = sSlots[slot].loggedHash;
        for (int i = 0; i < 4; i++) {
            if (seen[i] == value + 1u) {
                return;
            }
        }
        for (int i = 3; i > 0; i--) {
            seen[i] = seen[i - 1];
        }
        seen[0] = value + 1u;
        mp_log("puppet slot=%d %s=%08x", slot, what, (unsigned)value);
    }
}  // namespace MpPuppetsInt

namespace MpDropInHooks {
    void setArrivalHandler(ArrivalFn fn) { sArrivalHandler = fn; }
}

// ---- contract (MpGame.hpp) ---------------------------------------------------------------------------------------------------
namespace MpPuppets {
    // MP3 integration (flow request): the last pose of a slot as the wire decodes it; false when none this session.
    bool lastPose(int slot, MpPose* out) {
        if (slot < 0 || slot >= MP_MAX_PLAYERS || out == nullptr || sLastPoseState[slot] != 1 || !(mp_active() || MpDemo::live())) return false;   // MP3 demo: the mock roster's poses too
        *out = sLastPose[slot];
        return true;
    }

    void initScene() {
        if (!MpGame::sessionHeld() && !MpDemo::live()) {   // MP3 demo: live() = the puppet demo's scene (mock roster, no session)
            return;
        }
        if (sSceneLive) {
            endScene();   // defensive: the previous scene never told us it ended
        }
        MarioActor* a = MpPuppetsInt::localActor();
        if (a == nullptr) {
            mp_log("puppet init skipped reason=nomario");
            return;
        }
        const MpDropIn::SceneRoster roster = sceneRoster();
        sSceneSerial = mp_active() ? mp_scene_serial() : MpGameInternal::serial();
        sSceneStartTurn = sTurn;   // mppause
        sLastDemoSeen = false;
        sSelfDemoLogs = 0;
        sSquash = 0;
        sOtherTable = nullptr;
        MpLocalCapture::resetScene();
        buildNameMap(a);

        const bool localLuigi = MR::isPlayerLuigi();
        const int actorChar = actorCharacter(localLuigi);
        const int me = roster.localSlot;
        int created = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            resetSlot(s);
            sPuppets[s] = nullptr;
            sBind.bound[s] = -1;
            sBind.standIn[s] = false;
            sHiddenLogged[s] = -1;
        }
        for (int c = 0; c < MP_LOOK_COUNT; c++) {
            sPool[c] = nullptr;
            sBind.pool[c] = -2;
        }
        sSpares = 0;
        sHeapLogged = false;
        const HeapFree h0 = heapFree();

        // the self puppet: our character when it is not the MarioActor's
        if (validSlot(me)) {
            const int c = MpDropIn::clampLook(roster.look[me]);
            const char* arc = characterArchive(c, localLuigi);
            if (c == actorChar) {
                // the MarioActor itself already looks right
            } else if (arc != nullptr && !MR::isLoadedObjectOrLayoutArchive(arc)) {
                mp_log("puppet slot=%d model fallback want=%s missing=%s", me, characterName(c), arc);   // keep drawing the MarioActor
            } else if (!puppetFits()) {
                mp_log("puppet slot=%d self NOT created reason=memory napa=%u (the MarioActor keeps drawing)", me, (unsigned)freeOf(MR::getSceneHeapNapa()));
            } else {
                sPuppets[me] = newPuppet(me, c, true);
                created++;
                mp_log("puppet slot=%d character=%s", me, characterName(c));
                sPuppets[me]->appear();
                mp_log("puppet self on character=%s", characterName(c));
            }
        }

        // present remote players: their own character (characters are unique in a session), never a stand-in
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == me || !roster.present[s]) {
                continue;
            }
            const int c = MpDropIn::clampLook(roster.look[s]);
            const char* why = nullptr;
            if (sPool[c] != nullptr) {
                why = "duplicate";
            } else if (!ensureArchive(c, localLuigi, false, &why)) {
                // (why set)
            } else if (!puppetFits()) {
                why = "memory";   // memfix: the instance itself is guarded too (once per scene: this line)
            }
            if (why != nullptr) {
                mp_log("puppet slot=%d hidden character=%s reason=%s (until the next scene)", s, characterName(c), why);
                sHiddenLogged[s] = (int8_t)c;
                continue;
            }
            MpPuppetBase* p = newPuppet(s, c, false);
            sPool[c] = p;
            sPuppets[s] = p;
            sBind.pool[c] = (int8_t)s;
            sBind.bound[s] = (int8_t)c;
            created++;
            mp_log("puppet slot=%d character=%s", s, characterName(c));
        }
        const HeapFree h1 = heapFree();

        // drop-in spares: one per character nobody uses yet (drop-in allowed, a free seat), within the memory guard
        // memfix: no more spares than free seats (the first characters nobody has): a drop-in whose
        // character has no spare is shown by the stand-in / hidden fallbacks until the next scene (see update: ACT_HIDDEN)
        const MpDropIn::SceneRoster spareRoster = sceneRoster();
        const uint32_t spareMask = MpDropIn::spareLooks(spareRoster);
        mp_log("puppet spares wanted=%d freeSeats=%d present=%d max=%d dropin=%d", __builtin_popcount(spareMask), MpDropIn::freeSeats(spareRoster),
               MpDropIn::presentCount(spareRoster), spareRoster.maxPlayers, spareRoster.dropInAllowed ? 1 : 0);
        for (int c = 0; c < MP_LOOK_COUNT; c++) {
            if ((spareMask & (1u << c)) == 0 || sPool[c] != nullptr) {
                continue;
            }
            const char* why = nullptr;
            if (!ensureArchive(c, localLuigi, true, &why)) {
                mp_log("puppet spare character=%s skipped reason=%s", characterName(c), why);
                continue;
            }
            sPool[c] = newPuppet(-1, c, false);
            sBind.pool[c] = -1;
            sSpares++;
            mp_log("puppet spare character=%s", characterName(c));
        }
        const HeapFree h2 = heapFree();
        // (measurement: what the present players' puppets and the spares took from each heap)
        mp_log("puppet heap init napa=%u gddr=%u cache=%u players=-%d/-%d/-%d spares=%d -%d/-%d/-%d", (unsigned)h2.napa, (unsigned)h2.gddr,
               (unsigned)h2.cache, (int)(h0.napa - h1.napa), (int)(h0.gddr - h1.gddr), (int)(h0.cache - h1.cache), sSpares,
               (int)(h1.napa - h2.napa), (int)(h1.gddr - h2.gddr), (int)(h1.cache - h2.cache));
        if (mp_aids_scene) mp_aids_scene((unsigned)sSceneSerial, (unsigned)h2.napa, (unsigned)h2.gddr, (unsigned)h2.cache);   // MP3 hwaids: free memory / heap / vitaGL pools at this scene start

        // the joiner's own arrival: the others already play this scene (their STATE of this serial came before our init)
        sArrivePending = false;
        sArriveHold = 0;
        sArriveTurns = 0;
        if (mp_active() && mp_mode() == MP_MODE_TOGETHER && sSceneSerial != sLastInitSerial) {
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                MpPose q;
                if (s != me && mp_slot_present(s) && MpPuppets::lastPose(s, &q) && q.sceneSerial == sSceneSerial) {
                    sArrivePending = true;
                }
            }
            if (sArrivePending) {
                mp_log("puppet dropin self serial=%u (the others already play it)", (unsigned)sSceneSerial);
            }
        }
        sLastInitSerial = sSceneSerial;
        sSceneLive = true;   // (puppet() answers from here on)
        MpPuppetDrawerFn::create();
        MpNameTags::create();
        MpCoop::initScene();   // MP3 coop
        MpFormation::initScene();   // MP3 formation
        MpLod::sceneInit();   // MP3 lod2
        mp_log("puppet init serial=%u puppets=%d spares=%d names=%d collisions=%d overflow=%d", (unsigned)sSceneSerial, created, sSpares,
               sNames.n, sNames.collisions, sNames.overflow);
    }

    void onPeerJoined(int slot) {
        if (!validSlot(slot) || slot == mp_local_slot()) return;
        sSlots[slot].buf.reset();
        sLastPoseState[slot] = 0;
        netStatReset(slot);
        mp_log("puppet slot=%d pose reset (peer rejoined)", slot);
    }

    void update() {
        if (mp_active() != sNetWasActive) {   // netlog: a session goes live / ends: the settings file's timing knobs (read when it began), a fresh window
            if (!mp_active()) diagPush(true);   // beta diagnostics: the session's last totals
            sNetWasActive = mp_active();
            applyNetTune();
            sNetWinNs = 0;
            // Reset remote pose buffers on both drop and reconnect edges so post-outage STATE starts a fresh clock immediately.
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                sSlots[s].buf.reset();
                sLastPoseState[s] = 0;
                netStatReset(s);
            }
            if (sNetWasActive) {   // beta diagnostics: a new session: totals from zero (after buf.reset() so sDiagSnap0 matches snaps)
                memset(&sDiag, 0, sizeof sDiag);
                for (int s = 0; s < MP_MAX_PLAYERS; s++) sDiagSnap0[s] = sSlots[s].buf.snaps;
                sDiagPushNs = 0;
            }
        }
        if (!MpGame::sessionHeld()) {
            sHeldRosterValid = false;
        }
        if (!mp_active() && !MpDemo::live()) {
            gMpHideLocalBody = false;
            // review: the session ended in this scene: nobody steps the puppets any more (they would stand frozen, solid and
            // in the way until the scene ends, up to the 10 min the leave dialogs may take; the self puppet next to Mario)
            for (int s = 0; sSceneLive && s < MP_MAX_PLAYERS; s++) {
                if (sPuppets[s] != nullptr && !MR::isDead(sPuppets[s])) sPuppets[s]->kill();
            }
            MpCoop::update();   // MP3 coop: a session that ended in this scene leaves no pointer on screen
            return;
        }
        MP_PROF_SCOPE(MP_PROF_PUPPET_UPDATE);
        sTurn++;
        if (!sSceneLive) {
            return;
        }
        netLogTick();
        diagPush(false);   // beta diagnostics
        if (sSquash != 0) {
            sSquash--;
        }

        // ---- drop-in arrival & link telemetry (local pose capture/send runs right after GameScene::calcAnim in sendLocalPose())
        arrivalStep();
        netLinkLine(port_host_time_ns());

        // ---- drop-in: bind joiners (and LOOK changes) to the puppet of their character; a player who left gives his back
        const int me = mp_local_slot();
        {
            MpDropIn::SlotStep steps[MP_DROPIN_SLOTS];
            MpDropIn::step(sceneRoster(), sBind, steps);
            for (int s = 0; s < MP_MAX_PLAYERS; s++) {
                const MpDropIn::SlotStep& st = steps[s];
                if (st.action == MpDropIn::ACT_UNBIND || st.action == MpDropIn::ACT_REBIND) {
                    MpPuppetBase* old = sPuppets[s];
                    if (old != nullptr) {
                        old->bindSlot(-1);   // (killed: back to the pool as a spare)
                    }
                    sPuppets[s] = nullptr;
                    sSlots[s].demoMovement = false;
                    mp_log("puppet slot=%d unbind character=%s reason=%s", s, characterName(st.oldChar),
                           mp_slot_present(s) ? "look" : "left");
                }
                if (st.action == MpDropIn::ACT_BIND || st.action == MpDropIn::ACT_REBIND) {
                    MpPuppetBase* p = st.newChar >= 0 && st.newChar < MP_LOOK_COUNT ? sPool[st.newChar] : nullptr;
                    if (p != nullptr) {
                        p->bindSlot(s);
                        p->startArrival();
                        sPuppets[s] = p;
                        sHiddenLogged[s] = -1;
                        mp_log("puppet slot=%d bind character=%s (drop-in)", s, characterName(st.newChar));
                    }
                } else if (st.action == MpDropIn::ACT_HIDDEN || (st.action == MpDropIn::ACT_UNBIND && st.newChar >= 0)) {
                    // no puppet of his character: a see-through stand-in of his family (Mario / Cosmic Mario, Luigi / Cosmic
                    // Luigi) when this scene has a free one, else hidden; never somebody else's character
                    const int alt = MpDropIn::standInFor(sceneRoster(), sBind, st.newChar);
                    MpPuppetBase* p = alt >= 0 ? sPool[alt] : nullptr;
                    if (p != nullptr) {
                        MpDropIn::bindStandIn(sBind, s, alt);
                        p->bindSlot(s);
                        p->setStandIn(true);
                        p->startArrival();
                        sPuppets[s] = p;
                        sHiddenLogged[s] = -1;
                        mp_log("puppet slot=%d standin character=%s for=%s (until the next scene)", s, characterName(alt),
                               characterName(st.newChar));
                    } else if (sHiddenLogged[s] != st.newChar) {
                        sHiddenLogged[s] = st.newChar;
                        mp_log("puppet slot=%d hidden character=%s reason=nopuppet (until the next scene)", s, characterName(st.newChar));
                    }
                }
            }
        }

        // ---- puppet lifecycle
        const bool demo = MR::isDemoActive();
        const bool flyOrDemo = demo || MpPuppetsInt::localInFlyInOrDemo();
        // wave8: keep arrival push-free window anchored to the start of playable Action (not opening camera) and refresh across fly-in / Launch Star binds.
        if (MpGameInternal::sceneState() != MP_SCENE_ACTION) {
            sSceneStartTurn = sTurn;
        }
        if (flyOrDemo) {
            sLastDemoTurn = sTurn;   // mppause: the bodies do not push for a while after fly-in / demos / binds
            sLastDemoSeen = true;
        }
        const uint16_t serial = mp_scene_serial();
        bool hideBody = false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            MpPuppetBase* p = sPuppets[s];
            if (p == nullptr) {
                continue;
            }
            SlotState& st = sSlots[s];
            if (s == me) {
                // the self puppet lives for the whole scene; the MarioActor's body is hidden while it does
                if (MR::isDead(p) && p->isSelf()) {
                    // fix2: back from a reconnect (the outage killed every puppet): our own character shows again
                    p->appear();
                    mp_log("puppet self back slot=%d (session resumed)", s);
                }
                hideBody = !MR::isDead(p);
                if (flyOrDemo && sTurn % 30 == 0 && sSelfDemoLogs < 24) {   // mppause / wave8: fly-in body telemetry (covers ScenarioStarter & AstroDomeDemoStarter)
                    const TVec3f cam = MR::getCamPos();
                    TVec3f d(p->mPosition);
                    d -= cam;
                    MarioActor* la = MpPuppetsInt::localActor();
                    XanimeCore* lcore = (la != nullptr && la->mMarioAnim != nullptr && la->mMarioAnim->mXanimePlayer != nullptr) ?
                                            la->mMarioAnim->mXanimePlayer->getCore() : nullptr;
                    XjointTransform* lj0 = lcore != nullptr ? lcore->getJointTransform(0) : nullptr;
                    const int hasJ0_64 = (lj0 != nullptr && lj0->_64 != nullptr) ? 1 : 0;
                    f32 baseDiff = -1.0f;
                    u32 shapeHideMask = 0, diffDlZeroMask = 0;
                    J3DModel* pm = MR::getJ3DModel(p);
                    if (pm != nullptr && la != nullptr && la->getJ3DModel() != nullptr) {
                        MtxPtr pb = pm->getBaseTRMtx();
                        MtxPtr lb = la->getJ3DModel()->getBaseTRMtx();
                        const f32 bx = pb[0][3] - lb[0][3], by = pb[1][3] - lb[1][3], bz = pb[2][3] - lb[2][3];
                        baseDiff = sqrtf(bx * bx + by * by + bz * bz);
                        J3DModelData* pd = pm->getModelData();
                        if (pd != nullptr) {
                            const u16 sn = pd->getShapeNum() < 32 ? pd->getShapeNum() : 32;
                            for (u16 si = 0; si < sn; si++) {
                                J3DShape* shp = pd->getShapeNodePointer(si);
                                if (shp != nullptr && (shp->mFlags & 1u) != 0) shapeHideMask |= (1u << si);
                                J3DShapePacket* pkt = pm->getShapePacket(si);
                                if (pkt != nullptr && pkt->getDisplayListObj() != nullptr && pkt->getDisplayListObj()->mSize == 0) {
                                    diffDlZeroMask |= (1u << si);
                                }
                            }
                        }
                    }
                    mp_log("puppet self demo turn=%u state=%d drawable=%d culled=%d hidden=%d camdist=%.0f baseDiff=%.1f j0_64=%d flag=%u hide=%x dl0=%x",
                           (unsigned)sTurn, MpGameInternal::sceneState(), p->isDrawable() ? 1 : 0, p->isCulled() ? 1 : 0, gMpHideLocalBody ? 1 : 0,
                           d.length(), baseDiff, hasJ0_64, (unsigned)p->getFlag(), (unsigned)shapeHideMask, (unsigned)diffDlZeroMask);
                    sSelfDemoLogs++;
                }
                // NameObj.cpp: bit 0 = FLAG_MOVEMENT_OFF, bit 1 = FLAG_SUSPEND (chained demos / ScenarioStarter / requestStopSceneOverwrite)
                const bool selfSusp = (p->getFlag() & 3u) != 0 || MR::isNoCalcAnim(p);
                if (flyOrDemo && (!st.demoMovement || selfSusp) && hideBody) {
                    MR::requestMovementOn(p);
                    if (MR::isNoCalcAnim(p)) MR::onCalcAnim(p);
                    st.demoMovement = true;
                } else if (!flyOrDemo) {
                    st.demoMovement = false;
                }
                continue;
            }
            if (p->isSelf()) {
                // fix2: a self puppet whose slot is not ours any more (a rejoin into another slot): it would show our character
                // on that slot's pose
                if (!MR::isDead(p)) p->kill();
                continue;
            }
            const bool present = mp_slot_present(s);
            if (!present && st.buf.count > 0) {
                st.buf.reset();
                st.firstLogged = false;
            }
            const MpPoseF* nw = st.buf.newest();
            const char* why = nullptr;
            if (!present) {
                why = "left";
            } else if (nw == nullptr) {
                why = "nodata";
            } else if (nw->sceneSerial != serial) {
                why = "serial";
            } else if (sTurn - st.buf.lastArrival > cStaleTurns) {
                why = "stale";
            }
            if (why == nullptr && MR::isDead(p)) {
                p->appear();
            } else if (why != nullptr && !MR::isDead(p)) {
                p->kill();
                mp_log("puppet slot=%d hide reason=%s", s, why);
            }
            // local demos / ScenarioStarter fly-ins switch the player movement category off: keep remote players moving
            const bool remSusp = (p->getFlag() & 3u) != 0 || MR::isNoCalcAnim(p);
            if (flyOrDemo && (!st.demoMovement || remSusp) && !MR::isDead(p)) {
                MR::requestMovementOn(p);
                if (MR::isNoCalcAnim(p)) MR::onCalcAnim(p);
                st.demoMovement = true;
            } else if (!flyOrDemo) {
                st.demoMovement = false;
            }
        }

        gMpHideLocalBody = hideBody;
        MpFormation::update();   // MP3 formation: the puppets' drawn poses (before the tags: they follow them)
        MpLod::turn();   // MP3 lod2: distance bands, shadow modes, tiny (after the formation: its puppets count as the nearest)
        MpCoop::update();   // MP3 coop (before the tags: it starts their greeting icon)
        MpNameTags::update();

        if (!sHeapLogged && MpGameInternal::sceneState() == MP_SCENE_ACTION) {
            // (measurement: the scene heaps once the whole init and the first turns ran)
            sHeapLogged = true;
            const HeapFree f = heapFree();
            mp_log("puppet heap play serial=%u napa=%u gddr=%u cache=%u spares=%d", (unsigned)sSceneSerial, (unsigned)f.napa, (unsigned)f.gddr,
                   (unsigned)f.cache, sSpares);
        }
        MpQuality::turn();   // MP3 perf3: the picture-rate governor (puppet shadows / animation rates)
    }

    // wave8: capture and transmit local pose right after GameScene::calcAnim() so mPosition, mBaseMtx, joint0, and Xanime are mutually coherent.
    void sendLocalPose() {
        if (!sSceneLive || !mp_active()) {
            return;
        }
        MP_PROF_SCOPE(MP_PROF_PUPPET_UPDATE);
        const bool sendDue = sSendGate.dueTurn(sTurn, port_host_time_ns());
        if (poseLogOn()) {   // MP3 netsmooth (test log): our pose every turn, s=1 on the turns it is sent
            MpPoseF lp;
            if (MpLocalCapture::capture(&lp, sTurn)) {
                poseLog("tx", mp_local_slot(), sTurn, sTurn, 0.0f, sendDue ? 1 : 0, lp);
            }
        }
        if (!sendDue) {
            return;
        }
        MpPoseF p;
        if (!MpLocalCapture::capture(&p, sTurn)) {
            return;
        }
        if (sArrivePending || sArriveHold != 0) {
            p.flags &= (uint16_t)~MP_POSE_VISIBLE;   // drop-in: the others see us only once we stand beside them
        }
        uint8_t buf[MP_POSE_WIRE_BYTES + MP_POSE_TAIL_BYTES];
        const size_t n = MpPoseMath::writeState(buf, sizeof buf, p);
        const int me = mp_local_slot();
        if (n != 0 && me >= 0 && me < MP_MAX_PLAYERS) storeLastPose(me, buf, n);
        if (n != 0 && mp_send(MP_MSG_STATE, buf, (uint16_t)n, false)) {
            MP_PROF_COUNT(1);
            sSent++;
            if (!sSendLogged) {
                sSendLogged = true;
                mp_log("puppet send first serial=%u bytes=%u", (unsigned)p.sceneSerial, (unsigned)n);
            }
        }
    }

    void handle(const MpMsg& m) {
        if (!mp_active() && !MpDemo::live()) {
            return;
        }
        MP_PROF_SCOPE(MP_PROF_PUPPET_UPDATE);
        switch (m.type) {
        case MP_MSG_STATE:
            handleState(m);
            break;
        case MP_MSG_BUMP:
            if (MpBubble::handlePopRequest(m)) break;   // MP3 bubble2: kind MP_BUMP_POP (a spin / Star Bit pop request)
            handleBump(m);
            break;
        case MP_MSG_LOOK:
            if (m.len >= 1 && validSlot(m.fromSlot)) {
                // (the roster carries the look: update() rebinds the slot when this scene has a puppet of that character)
                mp_log("puppet slot=%d look=%u", (int)m.fromSlot, (unsigned)m.data[0]);
            }
            break;
        case MP_MSG_EMOTE:
        case MP_MSG_POINTER:
            MpCoop::handle(m);   // MP3 coop: greeting emote, pointer ping
            break;
        default:
            break;
        }
    }

    void endScene() {
        MpQuality::endScene();   // MP3 perf3
        if (sSceneLive) {
            mp_log("puppet end serial=%u sent=%u", (unsigned)sSceneSerial, (unsigned)sSent);
        }
        sSceneLive = false;
        gMpHideLocalBody = false;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            sPuppets[s] = nullptr;   // scene heap: never touched again
            sSlots[s].buf.reset();
            sSlots[s].demoMovement = false;
            sBind.bound[s] = -1;
        }
        for (int c = 0; c < MP_LOOK_COUNT; c++) {
            sPool[c] = nullptr;      // (spares too: the scene heap takes them; their archives go with the file cache / scene heap)
            sBind.pool[c] = -2;
        }
        sSpares = 0;
        sArrivePending = false;
        sArriveHold = 0;
        sOtherTable = nullptr;
        sNames.clear();
        sSquash = 0;
        MpPuppetsInt::nameTagsEndScene();
        MpCoop::endScene();   // MP3 coop
        MpFormation::endScene();   // MP3 formation
        MpLod::endScene();   // MP3 lod2
    }

    // SceneDataInitializer::startStageFileLoadAfterScenarioSelected: mount every archive a present player's character needs
    // (our own included: the self puppet), beyond the stationed set (which holds only the MarioActor's character): Luigi or
    // Mario, Kinopio (both Toads), GhostMario, GhostLuigi. Animations (MarioAnime) are shared. FileLoader skips what is mounted.
    // memfix: "mem scene <tag> ...": the three heaps' free bytes now (once per tag per scene: MpGame.cpp calls it before and after our objects)
    int nameObjSlotsLeft() { return objSlotsLeftNow(); }
    void logSceneHeap(const char* tag) {
        const HeapFree f = heapFree();
        mp_log("mem scene %s serial=%u napa=%u gddr=%u cache=%u objs-left=%d", tag, (unsigned)mp_scene_serial(), (unsigned)f.napa, (unsigned)f.gddr, (unsigned)f.cache, nameObjSlotsLeft());
    }

    void addArchives(void* collector) {
        if (!MpGame::sessionHeld() || collector == nullptr) {
            return;
        }
        NameObjArchiveListCollector* col = static_cast< NameObjArchiveListCollector* >(collector);
        const bool localLuigi = MR::isPlayerLuigi();
        // integ (B1): everybody present's character and the spares' (drop-in: one per free seat): the only time an archive can be
        // asked for (initScene runs on the init thread and mounts nothing). Two characters may share one archive (both Toads).
        const MpDropIn::SceneRoster roster = sceneRoster();
        const uint32_t looks = MpDropIn::archiveLooks(roster);
        const char* added[MP_LOOK_COUNT];
        int n = 0;
        for (int c = 0; c < MP_LOOK_COUNT; c++) {
            if ((looks & (1u << c)) == 0) {
                continue;
            }
            const char* arc = characterArchive(c, localLuigi);
            if (arc == nullptr) {
                continue;
            }
            bool dup = false;
            for (int i = 0; i < n; i++) {
                dup = dup || strcmp(added[i], arc) == 0;
            }
            for (s32 i = 0; i < col->getArchiveNum() && !dup; i++) {
                dup = strcmp(col->getArchive(i), arc) == 0;   // (the stage's own list already has it)
            }
            if (dup) {
                continue;
            }
            if (col->getArchiveNum() >= 30) {   // (the collector holds 32 names: two stay free for the bubble's)
                mp_log("puppet archive SKIPPED %s: the scene's archive list is full", arc);
                continue;
            }
            added[n++] = arc;
            col->addArchive(arc);
            mp_log("puppet archive add=%s", arc);
            {   // memfix: where the game will mount it (the rule MR::getAproposHeapForSceneArchive applies at mount time: the file cache
                // unless its free ratio is under 3 %, then the scene GDDR): the audit warns Egg Planet with 4 players can overflow the cache
                JKRHeap* chosen = MR::getAproposHeapForSceneArchive(0.03f);
                const HeapFree hf = heapFree();
                mp_log("mem archive %s heap=%s cache-free=%u gddr-free=%u", arc, (MR::getSceneHeapGDDR3() != nullptr && MR::getSceneHeapGDDR3()->getFreeSize() > 0x400000) ? "gddr" : (chosen == (JKRHeap*)SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap ? "cache" : "gddr"),   // (SceneDataInitializer sends ours to the scene GDDR while it has 4 MiB to spare)
                       (unsigned)hf.cache, (unsigned)hf.gddr);
            }
        }
    }
}  // namespace MpPuppets
