// dome31 host test: see tools/test_dome31_nocut.sh. The extracted LiveActor::calcAnim runs against stub cuts that always say "leave out".
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
typedef uint8_t u8; typedef uint32_t u32;
#define PORT_ANIM_PROF_NAME(x) ((void)0)
#define PORT_ANIM_PROF_COUNT(x) ((void)0)
#define PORT_LOGIC_SYS(x) ((void)0)
#define PORT_LS_CALCANIM 0
#define PORT_HOT_KNOB(T, ...) (__VA_ARGS__)
static int gPortAlodTurn = 5; static int port_cc_flag = 0, port_logic_prof_flag = 0, port_pair_skip_turn = 1;
static void port_cc_mark(void*, int) {}
static void PortLogicProfName(void*) {}
static int port_pairpose_mode() { return 1; }
static void port_pair_count(int, int) {}
static int port_starbatch_far_on() { return 1; }
namespace port { namespace lockstep { static bool On() { return false; } } }
struct Xanime { bool _88 = false; };
struct ModelManager { Xanime* mXanimePlayer = nullptr; };
struct Sensors { int mSensorCount = 0; };
struct Flag { bool mIsNoCalcAnim = false; };
struct Coll {};
struct LiveActor;
static std::vector<LiveActor*> sStaleAnim;
static void PortPoseLeftOut(Xanime*) {}
struct LiveActor {
    Flag mFlag; ModelManager* mModelManager = nullptr; Coll* mCollisionParts = nullptr; Sensors* mSensorKeeper = nullptr;
    bool mStaleAnim = false, mStaleXLatch = false, mPortNoCut = false; u8 mPairSkip = 0, mPbHalf = 0, mCrSkip = 0xFF, mBowlPose = 0, mCrPosed = 0;
    u8 mFlFam = 1; u32 mAlodSkipTurn = 0; float mLodRadius = 1;
    int posed = 0; void calcAnim(); void calcAnmMtx() { ++posed; }
};
struct PortAlodCalcTimer { PortAlodCalcTimer(LiveActor*) {} bool on() { return false; } };
struct PortFlPoseTimer { PortFlPoseTimer(LiveActor*) {} };
static int PortBowlPose(LiveActor*) { return 1; }
// (ondraw31, GXV_POSE_ONDRAW: on, and its rule says "leave out" too; the exemption must come first)
static int gPortOnDraw = 1;
static const LiveActor* gPortOdTimed = nullptr;
static int PortOnDraw(LiveActor*) { return 1; }
static void PortOnDrawAfter(LiveActor*) {}
static void PortBowlVerifyAfter(LiveActor*) {}
static bool PairPoseLeavesOut(LiveActor*, int) { return true; }
static bool PbAnimSkips(LiveActor*) { return true; }
static int port_starbatch_far_skip(LiveActor*, int, int) { return 1; }
static bool AnimLodDefers(LiveActor*) { return true; }
static void AnimLodTakeRadius(LiveActor*) {}
namespace MR { static int collisionSet = 0; static void setCollisionMtx(LiveActor*) { ++collisionSet; } }
#include "calcanim.inc"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    Xanime x; ModelManager mm; mm.mXanimePlayer = &x;
    LiveActor map, other; map.mModelManager = other.mModelManager = &mm; map.mPortNoCut = true;
    for (int turn = 0; turn < 8; ++turn) { gPortAlodTurn = turn; map.mBowlPose = other.mBowlPose = 1; map.calcAnim(); other.calcAnim(); }
    std::printf("map posed %d of 8, ordinary actor posed %d of 8\n", map.posed, other.posed);
    CHECK(map.posed == 8);   // fails without the exemption: every stub cut leaves it out
    CHECK(other.posed == 0); // control: the stubs do cut an ordinary actor
    CHECK(!map.mStaleAnim);
    LiveActor st; st.mModelManager = &mm; st.mPortNoCut = true; st.mStaleAnim = true; st.calcAnim();
    CHECK(st.posed == 1 && !st.mStaleAnim);
    std::printf(fails ? "dome31 nocut: FAILED (%d)\n" : "dome31 nocut: ok\n", fails);
    return fails ? 1 : 0;
}
