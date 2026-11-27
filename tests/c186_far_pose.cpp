#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port/hot_knob.h"
#include "port/pose_phase.h"   // (c186merge: integ-3.1's PbAnimSkips has ratecut31's GXV_POSE_PHASE path; Alternate is the real rule)

using u32 = uint32_t;
struct TVec3f { float x, y, z; };
struct LiveActor {
    uint8_t mPbHalf = 1, mCrSkip = 0xFF;  // (u8 as in LiveActor.hpp)
    int mFlFam = 0, mAlodRate = 0, mStaleAnim = 0;
    void* mCollisionParts = nullptr;
    void* mModelManager = nullptr;
    TVec3f mPosition{4000, 0, 0};
};
static int level = 3, cpN, cpCalls, famResult = -1, farResult = -1, skipped, notes;
static bool lodCategory, locked;
static TVec3f origin{};
namespace MR {
TVec3f getCamPos() { return origin; }
const char* getModelResName(const LiveActor*) { return "test"; }
}
namespace port {
namespace lockstep { bool On() { return locked; } }
namespace mpgate { enum { kPose }; }
}
static void port_mpgate_note(int) {}
static int port_protect_sprites() { return 0; }
static float port_pb_scale() { return 1; }
static int port_farlogic_level() { return level; }
static int port_farlogic_pose_on() { return level == 3; }
static int port_anim_lod_category() { return lodCategory; }
static int port_farlogic_pose_category() { return 1; }
static float port_farlogic_pose_dist2() { return 2500.0f * 2500; }
static int port_cp_n_for(uint32_t) { return cpN; }
static float port_cp_dist2() { return 2500.0f * 2500; }
static float port_pb_anim_dist2() { return 1; }
static float port_pb_anim_pdist2() { return 1; }
static float port_detail_dist(int, float x) { return x; }
static float port_pb_lead(float) { return 0; }
static bool port_pb_demo_off() { return false; }
static int port_cpulod_fampose_n_for(uint32_t) { return famResult >= 0 ? 2 : 0; }
static bool port_on_frame_thread() { return true; }
static int CpulodFamPose(LiveActor*, int, float) { return famResult; }
static bool port_safe_cam_exists() { return true; }
static const TVec3f* port_safe_player_pos() { return &origin; }
static bool CpCharPose(LiveActor*, int, float) { ++cpCalls; return true; }
static int port_cr_faranim_n() { return farResult >= 0 ? 2 : 0; }
static int CrFarAnim(LiveActor*, int, float) { return farResult; }
// ratecut31 (GXV_POSE_PHASE, default 0) glue, as LiveActor.cpp declares it; the census flag stands in for the mode-0 census path
static int ppMode, port_pose_phase_skip, port_pose_phase_census_flag, phaseNotes;
static int port_pose_phase_mode() { return ppMode; }
static bool PosePhaseTagged(const LiveActor*) { return false; }
static void PosePhaseNote(const LiveActor*, int, int) { ++phaseNotes; }
static void port_logic_count(int k, int n) { assert(k == 11); skipped += n; }
static void port_log_summary(const char*, const char*) { ++notes; }
#define PORT_LOGGING 1
#include "skip.inc"

int main(int argc, char** argv) {
    assert(argc == 2);
    const bool on = std::strcmp(argv[1], "on") == 0;
    LiveActor a;
    cpN = 2;  // Must not enable the independently configured CHARPOSE fallback.
    assert(PbAnimSkips(&a) == !on && a.mPbHalf == 2);
    assert(!PbAnimSkips(&a) && a.mPbHalf == 1);
    assert(PbAnimSkips(&a) == !on && cpCalls == 0);
    assert(skipped == (on ? 0 : 2) && notes == int(on));
    a.mPbHalf = 1; level = 2;
    assert(PbAnimSkips(&a) && cpCalls == 1);
    level = 3; famResult = 1;
    assert(PbAnimSkips(&a) && a.mPbHalf == 0);
    famResult = -1; farResult = 1; a.mPbHalf = 1;
    assert(PbAnimSkips(&a) && a.mPbHalf == 1);
    farResult = -1; lodCategory = true;
    assert(PbAnimSkips(&a) && a.mPbHalf == 2);
    // (c186merge) GXV_POSE_PHASE on, or its census: the pose-phase rule decides before the mode-0 switch, so the diagnostic does not act there
    lodCategory = false; level = 3; a.mPbHalf = 1; port_pose_phase_census_flag = 1;
    const int before = skipped;
    assert(PbAnimSkips(&a) && a.mPbHalf == 2 && skipped == before + 1 && phaseNotes == 1);
    port_pose_phase_census_flag = 0;
    locked = true; a.mPbHalf = 1;
    assert(!PbAnimSkips(&a) && a.mPbHalf == 0);
    std::printf("C186 far-pose-only diagnostic %s: phase, movement-independent pose policies, MP and one-time notice OK\n", argv[1]);
}
