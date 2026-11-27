// ondraw31 (18 Nov 2026, plan-cpu31 idea 2): GXV_POSE_ONDRAW, every actor's joints made only on the turn whose pose a picture shows, whenever nothing
// reads them on the turn whose pose no picture shows. bowl31's fish rule (port/bowl_pose.h) generalised to every class of the calc-anim lists that are
// not the player's, with blendfix31's catch-up (XanimeCore::portLeaveOut / portCatchUp) for the animation state. The pure rules (no game types:
// tests/ondraw.cpp compiles this header as it is). The glue: decomp/src/Game/LiveActor/LiveActor.cpp (PortOnDraw, the flush, the read hook, the verify,
// the census), perf.cpp (the turn's start), entry.cpp (the default).
//
// WHICH TURN (port/pose_phase.h has the argument). A picture is drawn at the START of a drawn turn from the state the turn before left. On a SKIP TURN (a
// drawn turn that follows an undrawn one: Quality's even 30, the pair of Performance / 60 Assist) the joints this turn's calc-anim makes are made again by
// the next turn before any picture shows them. Only a reader in that window (the rest of the skip turn, the next turn's movement) could see them.
//
// WHAT IS LEFT OUT on a skip turn: ModelManager::calcAnim (XanimePlayer::calcAnm, the joint tree, envelopes, material animation and lists) and the view
// pass. WHAT IS STILL MADE: the base matrix (setBaseScale + the class's own calcAndSetBaseMtx, exactly as today), so every reader of the base matrix
// (sensors, effects, shadows, star-pointer targets, collision and camera code that follow getBaseMtx) sees this turn's value: they are not readers of
// what is left out. The BCK latch is released as calcAnm releases it and the inputs recorded (blendfix31), so movement, nerves, timers, random numbers
// and the animation clock are untouched.
//
// WHO (per actor, at run time, every skip turn; the first reason that holds is the census's):
//   turn      not a skip turn, a lockstep session, not the frame thread, a demo or no player (PairPoseGate), the switch off
//   list      not one of the lists 4 Environment, 5 MapObj, 6 NPC, 8 Enemy, 0x0B MapObjDecoration, 0x10 Item (the player's, the planets', collision objects',
//             rides', mirrors', layouts' and particles' lists are posed as today; so is every calcAnim called from outside a list: the flush, movement)
//   blendfix  GXV_POSE_BLENDFIX is not 1: an animation change right after a left-out pose would start its blend from an older pose (blendfix31)
//   other     another rule holds the actor (listed for the flush, an every-n-th count, PB_ANIM, MATLOD, half-rate movement, a plant, star bits / coins,
//             the far deferral, a class of the logic LODs (ALOD / far-logic families: their movement can run without their calc-anim), or it
//             was never posed
//   picture   collision parts, a camera controller, 60 Assist's pair set (the second picture draws this turn's pose)
//   read      its joint matrices were handed out at any time since it was made (MR::getJointMtx & co. mark the model for good: a sensor or effect
//             following a joint, a shadow dropped from a joint, an attachment (PartsModel, syncJointAnimation, copyJointAnimation), a camera or pointer
//             target on a joint, a class reading its own joints in control() or in its calcAnim override)
//   callback  a joint callback (it can write game state while the joints are made)
//   shared    its XanimeCore shares its joint list (MarioAnimator's cores): never owed (blendfix31)
//   blend     the next computation reads the last pose every time (_20 != 1, an empty track 0, a total weight of 0, _6 = 1 / 2): a catch-up every turn;
//             or more tracks than blendfix31's record holds (4)
// A FIRST read in the window (a reader that appears only now) is answered by a CATCH-UP: the left-out joints are made at once, as the skip turn would
// have made them: from the same state when it is provably unchanged (a hash of the base matrix, base scale, the BCK clock and latch, the core's inputs,
// taken when the pose was left out: the whole ModelManager::calcAnim, with the material and visibility players' resources and frames), else, when only
// the actor's own update has run since, from blendfix31's record of
// the left-out computation's inputs (XanimeCore::portRemake: the joint tree walked on the recorded frame ratio, interpolation and tracks; the base and
// base scale must be unchanged). The model is then marked and refused for good. If neither holds (an animation change caught the record up, the base
// was rewritten, the read came from inside another model's computation), the read is counted LATE (mode 1) or compared (mode 2): the console verify
// must show 0.
//
// THE PICTURE: every left-out pose is listed for the drawn turn's flush (port_anim_flush_stale), which makes it (joints and materials: the base of the
// skip turn stands) before the picture if the next turn did not; the next calc-anim makes it whatever the every-n-th rules say. Pose age at draw: 0,
// always ("stale at draw" in the census must be 0). When an update ran after the skip turn and the next calc-anim did not (the actor switched its
// calc-anim off in its own movement while shown), the flush makes the pose from the record: the joints (XanimeCore::portRemake), the visibility
// animation (BVA, applied with the joints) at its recorded frame, and the materials as the skip turn makes them (GXV_DL_LAZY=2, the default: from
// the frames at the drawn turn's material flush, as for every model; 0 / 1: at the recorded frames of BTK / BRK / BTP / BPK). The "unchanged" test
// covers those players' resources and frames. One case is not exact: the record was caught up by an animation change (or the base rewritten, or
// under GXV_DL_LAZY 0 / 1 a material resource changed) before such a flush: the pose is then made from the later state ("drift"); counted, and
// mode 2's picture check reports it as a MISMATCH.
//
// GXV_POSE_ONDRAW=<0|1|2> (default 0): 0 as before (one load per calcAnim); 1 the cut; 2 verify: nothing is left out; for every actor mode 1 would leave
// out, the state hash is taken before and after its calc-anim (the calc-anim must change nothing but the pose), the products the cut would not make
// (joint matrices, the core's last local transforms) are kept, and every reader in the window is compared against them: a joint read (a catch-up when
// the state is unchanged, else the kept joints against the made ones), the next computation's freeze / last-pose read (covered by blendfix31's
// catch-up, else compared), and the picture: the flush checks that every pose of the window was made from the same base and state as the skip turn
// left (picture matrices kept and compared when the next computation reads them). "pose-ondraw: MISMATCH" lines (24 at most).
// Census (modes 1 and 2): one "pose-ondraw:" line every 600 turns: asked / left out by list, owed poses made (next turn / flush / read catch-up / not
// drawn), refused by reason, blendfix catch-ups, LATE reads, mismatches, stale at draw, and the estimated saving (sampled pose time x poses left out,
// less the catch-ups and the rule's own time), ms a pair.
#pragma once

#include <stdint.h>
#include <string.h>

namespace port {
namespace ondraw {

inline int ParseMode(const char* e) {
    if (e == nullptr || e[0] == 0 || e[1] != 0) return 0;
    return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0;
}

// The lists the rule acts in (SceneFunction's CalcAnimType), and the census's kind per list.
enum Kind : int { kEnv = 0, kMapObj = 1, kNpc = 2, kEnemy = 3, kDeco = 4, kItem = 5, kKinds = 6 };
inline int KindOfList(int raw) {
    switch (raw) {
    case 0x04: return kEnv;
    case 0x05: return kMapObj;
    case 0x06: return kNpc;
    case 0x08: return kEnemy;
    case 0x0B: return kDeco;
    case 0x10: return kItem;
    default: return -1;
    }
}
inline const char* KindName(int k) {
    static const char* const n[kKinds] = {"env", "mapobj", "npc", "enemy", "deco", "item"};
    return k >= 0 && k < kKinds ? n[k] : "?";
}

// LiveActor::mBowlPose bits (bowl31 owns 0x03 class, 0x20 / 0x40 / 0x80 window; both rules share the window bits, 0x10 says the window is this rule's).
enum : uint8_t {
    kMade = 0x08,     // mode 1: a first reader in the window had the joints made (the view pass, and after a remake the materials, still owed)
    kWin = 0x10,      // the window (0x80 / 0x40 / 0x08) is this rule's
    kPending = 0x80,  // mode 1: left out, listed for the flush
    kVerify = 0x40,   // mode 2: products kept, readers compared
    kVerifyD = 0x20,  // mode 2: the skip turn's own calc-anim is running
    kWinMask = 0xF8
};

enum Refusal : int {
    kRefTurn = 0,
    kRefList = 1,
    kRefBlendFix = 2,
    kRefOther = 3,
    kRefPicture = 4,
    kRefRead = 5,
    kRefCallback = 6,
    kRefShared = 7,
    kRefBlend = 8,
    kRefusals = 9
};
inline const char* RefusalName(int r) {
    static const char* const k[kRefusals] = {"turn", "list", "blendfix", "other", "picture", "read", "callback", "shared", "blend"};
    return r >= 0 && r < kRefusals ? k[r] : "?";
}

struct Facts {
    bool skipTurn;     // the turn's phase (a drawn turn after an undrawn one), the switch on, not a lockstep session
    bool gate;         // the frame thread, a game scene with a player, no demo
    bool list;         // a calc-anim list of KindOfList (not the flush, not a call from movement)
    bool blendFixOn;   // GXV_POSE_BLENDFIX == 1
    bool posedOnce;    // mCrPosed, a model
    bool otherRule;    // mStaleAnim, mCrSkip != 0xFF, mPbHalf, MATLOD (mCpDlFar / mCpDlSkip), mFlFam, mAlodFamily, mAlodRate > 1, mLogicFreeJoints,
                       // mPairSkip, the far deferral
    bool collision, camera, hrwTag;
    bool jointsRead;   // J3DModel::mPortBowl & 1 (sticky)
    bool callback;     // a J3DJoint callback on the model data
    bool sharedCore;   // XanimeCore::mPortShared
    bool readsLast;    // XanimeCore::portReadsLast() now (the next computation reads the last pose whatever happens)
};
inline int Refuse(const Facts& f) {
    if (!f.skipTurn || !f.gate) return kRefTurn;
    if (!f.list) return kRefList;
    if (!f.blendFixOn) return kRefBlendFix;
    if (!f.posedOnce || f.otherRule) return kRefOther;
    if (f.collision || f.camera || f.hrwTag) return kRefPicture;
    if (f.jointsRead) return kRefRead;
    if (f.callback) return kRefCallback;
    if (f.sharedCore) return kRefShared;
    if (f.readsLast) return kRefBlend;
    return -1;
}

// Word-wise FNV-1a (the state hashes: the catch-up's "unchanged" test and mode 2's calc-anim check).
struct Hash {
    uint32_t h = 2166136261u;
    void U32(uint32_t v) { h = (h ^ v) * 16777619u; }
    void F32(float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        U32(v);
    }
    void Words(const void* p, unsigned bytes) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (unsigned i = 0; i + 4 <= bytes; i += 4) {
            uint32_t v;
            memcpy(&v, b + i, 4);
            U32(v);
        }
        for (unsigned i = bytes & ~3u; i < bytes; ++i) U32(b[i]);
    }
};

// What the census line prints (one window of 600 turns).
struct Census {
    uint32_t turns, skipTurns, broken;
    uint32_t asked[kKinds];        // calc-anims in the rule's lists on a skip turn
    uint32_t leftOut[kKinds];      // mode 1: left out; mode 2: would be
    uint32_t refused[kRefusals];   // all kinds (turn and list are counted for calls that reach the rule)
    uint32_t posedNext, posedFlush, posedRead, notPosed;  // who made the owed pose
    uint32_t remade;               // of posedRead: made from blendfix31's record (the actor's own update had run since the skip turn)
    uint32_t drift;                // an owed pose shown from a later state (an update ran, the next calc-anim did not, and the record was caught up by an
                                   // animation change, the base rewritten or, under GXV_DL_LAZY 0 / 1, a material resource changed)
    uint32_t flushRecord;          // of posedFlush: made by the flush from the record (joints, visibility, materials: exact) after an update
    uint32_t late;                 // mode 1: a first joint read in the window whose state had changed (the read saw the older pose)
    uint32_t mismatch[4];          // mode 2: 0 state hash changed by the calc-anim, 1 joint read, 2 next pose, 3 picture
    uint32_t checks;               // mode 2: windows checked
    uint32_t baseSplit;            // mode 2: of them, the state compared from after the base (emufix31: mode 1 makes the base too; the rest held by another rule)
    uint32_t baseWrote;            // mode 2 (fable31fix): of the checks, those where the class's base (calcAndSetBaseMtx) had written logic state (state after the base differs from before it); NOT a mismatch
    uint32_t blendCatch;           // blendfix31 catch-ups made in the window (all rules; counted from its census)
    uint64_t staleAtDraw;          // pictures drawn while a pose was owed (must be 0)
    uint64_t poseNs;               // sampled pose time (mode 1: the next turn's make; mode 2: the skip turn's own calc-anim)
    uint32_t poseN;
    uint64_t ruleNs;               // sampled time of the rule's decision on a skip turn
    uint32_t ruleN, ruleCalls;
    void Clear() { memset(this, 0, sizeof *this); }
    uint32_t AskedAll() const {
        uint32_t s = 0;
        for (int k = 0; k < kKinds; ++k) s += asked[k];
        return s;
    }
    uint32_t LeftAll() const {
        uint32_t s = 0;
        for (int k = 0; k < kKinds; ++k) s += leftOut[k];
        return s;
    }
    // ms a pair: the poses left out at their sampled price, less the read catch-ups (a full pose each), blendfix's catch-ups (about half a pose
    // each: blendfix31 measured 40-53 %) and the rule's own time on the skip turns. The view pass of each left-out pose is not in it (a bonus).
    double SavedMsPerPair() const {
        if (skipTurns == 0 || poseN == 0) return 0.0;
        const double pose = static_cast<double>(poseNs) / poseN;
        const double rule = ruleN ? static_cast<double>(ruleNs) / ruleN : 0.0;
        const double ns = LeftAll() * pose - posedRead * pose - blendCatch * 0.5 * pose - ruleCalls * rule;
        return ns / skipTurns / 1e6;
    }
};

}  // namespace ondraw
}  // namespace port
