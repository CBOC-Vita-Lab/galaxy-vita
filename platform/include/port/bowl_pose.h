// bowl31 (17 Nov 2026, perf-inventory D4): GXV_BOWL_POSE, the pose of a Beach Bowl fish left out of the turn whose pose nothing reads. The pure rules
// (no game types: tests/bowl_pose.cpp compiles this header as it is). The glue: decomp/src/Game/LiveActor/LiveActor.cpp (PortBowlPose, the flush, the
// verify), perf.cpp (the turn's phase, the "bowl-pose:" line), JointUtil.cpp / ModelUtil.cpp (the logic's joint reads), the classes' constructors
// (Fish, the penguins), entry.cpp (the default).
//
// WHICH TURN. A picture is drawn at the start of a drawn turn from the state the turn before left (port/pose_phase.h has the whole argument). On a SKIP TURN
// (a drawn turn that follows an undrawn one: Quality's even 30, Performance's pair) the pose this turn's calc-anim makes is made again by the next turn
// before any picture shows it. GXV_POSE_PHASE uses the same turn (port::posephase::Phase); this rule takes its phase from the same tracker.
//
// WHAT. On a skip turn the calc-anim of a marked actor (Fish; the penguins are marked for the census and the verify only) is left out when NOTHING reads
// its pose before the next turn makes it again:
//   - not the logic: its joint matrices were never handed out (MR::getJointMtx & co. mark the model, JointUtil.cpp / ModelUtil.cpp), it has no hit
//     sensor, no effect keeper, no shadow, no star-pointer target, no collision, no camera controller, no joint callback; a read of its base matrix
//     (LiveActor::getBaseMtx) or joints in the window is counted (mode 1: LATE; mode 2: compared);
//   - not the next pose: XanimeCore keeps the last pose's local transforms (_28) and reads them when a freeze (an animation change) is captured at the
//     next computation, when _20 != 1 (smoothing toward the last pose) or when track 0 has no resource. _20 != 1, no track, or a freeze already
//     pending refuse; a freeze started by the next turn's movement is detected there (mode 1: LATE; mode 2: compared). Fish never change animation
//     after init (only MR::setBckRate), so this never happens to them; it is why the penguins are not cut;
//   - not 60 Assist's second picture: a model of the pair set (mPortHrwTag) is posed every turn;
//   - not a picture after a broken pair: the actor is listed for the drawn turn's flush (port_anim_flush_stale), which poses it before the picture
//     from the same state the left-out calc-anim had (the BCK latch kept as the far deferral keeps it). The next turn, drawn or not, poses it whatever
//     the other every-n-th rules say (never two left out in a row). Pose age at draw: 0, always.
// The BCK clock is released as a made calcAnm releases it (XanimePlayer::_88 = false), so movement, nerves, timers and the random numbers are untouched.
//
// GXV_BOWL_POSE=<0|1|2> (default 0): 0 as before (the calc-anim path is not entered: one byte test per call); 1 the cut (fish only); 2 verify: nothing is
// left out; for every actor mode 1 would leave out (fish and penguins) the state hash is taken before and after the calc-anim (it must not change: the
// cut changes nothing but the pose), the pose products the cut would leave (the joint matrices, the base matrix, the core's last local transforms) are
// kept, and every reader in the window is compared against them: a joint read, a base-matrix read, the next computation's freeze / smoothing.
// A difference is a MISMATCH line (24 at most) and a count in the census. One "bowl-pose:" line every 600 turns in modes 1 and 2.
#pragma once

#include <stdint.h>
#include <string.h>

namespace port {
namespace bowlpose {

inline int ParseMode(const char* e) {
    if (e == nullptr || e[0] == 0 || e[1] != 0) return 0;
    return e[0] == '1' ? 1 : e[0] == '2' ? 2 : 0;
}

// LiveActor::mBowlPose: the class in the low bits, the window's state in the high ones.
enum : uint8_t {
    kNone = 0,
    kFish = 1,     // Fish (FishGroup's members): cut in mode 1
    kPenguin = 2,  // Penguin, PenguinStudent, PenguinCoach, PenguinRacer(Leader), PenguinSkater, PenguinMaster: census and verify only
    kClassMask = 3,
    kPending = 0x80,   // mode 1: left out on the skip turn, listed for the flush; the next calc-anim or the flush poses it
    kVerify = 0x40,    // mode 2: the pose products before the skip turn's calc-anim are kept; readers in the window are compared
    kVerifyD = 0x20,   // mode 2: the skip turn's own calc-anim is running (the hash after it is compared)
};
// J3DModel::mPortBowl
enum : uint8_t { kModelRead = 1, kModelPend = 2 };

inline int ClassOf(uint8_t b) { return b & kClassMask; }
inline bool ClassCut(int cls) { return cls == kFish; }

// Why an actor's pose is not left out (Census::refused index), -1 = leave out.
enum Refusal : int {
    kRefTurn = 0,      // not a skip turn, a lockstep session, the loading thread, a demo / no player (the turn's gate)
    kRefOther = 1,     // another rule holds it (listed for the flush, an every-n-th count, a family, half-rate movement, a plant), never posed yet
    kRefPicture = 2,   // collision, a camera controller, 60 Assist's pair set
    kRefRead = 3,      // its joint matrices were handed out to the logic
    kRefCallback = 4,  // a joint callback
    kRefParts = 5,     // a hit sensor, effect keeper, shadow or star-pointer target (each reads the pose or the base matrix)
    kRefBlend = 6,     // the next computation reads the last pose (_20 != 1, track 0 without a resource, a freeze pending)
    kRefClass = 7,     // a census-only class (mode 1 does not cut it)
    kRefusals = 8
};
inline const char* RefusalName(int r) {
    static const char* const k[kRefusals] = {"turn", "other-rule", "picture", "joints-read", "callback", "parts", "blend", "class"};
    return r >= 0 && r < kRefusals ? k[r] : "?";
}

struct Facts {
    int cls;            // kFish / kPenguin
    bool skipTurn;      // the turn's phase (drawn after undrawn), the mode on, not a lockstep session
    bool gate;          // on the frame thread, a game scene with a player, no demo
    bool posedOnce;     // mCrPosed
    bool otherRule;     // mStaleAnim, mCrSkip != 0xFF, mFlFam, mAlodRate > 1, mLogicFreeJoints
    bool collision, camera, hrwTag;
    bool jointsRead;    // J3DModel::mPortBowl & kModelRead
    bool callback;      // a J3DJoint callback on the model data
    bool parts;         // sensors, effects, shadows, star-pointer target
    bool blendReadsLast;  // XanimeCore _20 != 1, track 0 null with a player, or its freeze flag set
};
// The order is the census's: the first reason that holds is counted. -1: mode 1 leaves the pose out (mode 2 verifies it, any class).
inline int Refuse(const Facts& f, bool forCut) {
    if (!f.skipTurn || !f.gate) return kRefTurn;
    if (!f.posedOnce || f.otherRule) return kRefOther;
    if (f.collision || f.camera || f.hrwTag) return kRefPicture;
    if (f.jointsRead) return kRefRead;
    if (f.callback) return kRefCallback;
    if (f.parts) return kRefParts;
    if (f.blendReadsLast) return kRefBlend;
    if (forCut && !ClassCut(f.cls)) return kRefClass;
    return -1;
}

// FNV-1a over 32-bit words (the state hash of mode 2).
struct Hash {
    uint64_t h = 14695981039346656037ull;
    void Byte(uint8_t b) { h = (h ^ b) * 1099511628211ull; }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; ++i) Byte(static_cast<uint8_t>(v >> (8 * i)));
    }
    void F32(float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        U32(v);
    }
    void Bytes(const void* p, unsigned n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (unsigned i = 0; i < n; ++i) Byte(b[i]);
    }
};

// The census of one "bowl-pose:" line (per class: index 0 fish, 1 penguins).
struct Census {
    uint32_t turns, skipTurns, broken;
    uint32_t asked[2];            // calc-anims of the class on a skip turn
    uint32_t leftOut[2];          // mode 1: left out; mode 2: would be
    uint32_t posedNext[2];        // the owed pose made by the next turn's calc-anim
    uint32_t posedFlush[2];       // ... by the drawn turn's flush (a broken pair, or the next turn deferred it)
    uint32_t notPosed[2];         // ... by nobody before a picture: hidden, dead, clipped (not drawn)
    uint32_t refused[2][kRefusals];
    uint32_t late[2];             // mode 1: a reader in the window (joints, base matrix, a freeze); mode 2: readers compared
    uint32_t mismatch[2][4];      // mode 2: 0 state hash changed by the calc-anim, 1 joint read, 2 base-matrix read, 3 the next pose (freeze / smoothing)
    uint32_t checks[2];           // mode 2: actors checked
    uint64_t staleAtDraw;         // pictures drawn while an owed pose was still owed (must be 0)
    void Clear() { memset(this, 0, sizeof *this); }
};

}  // namespace bowlpose
}  // namespace port
