// mapobjcut host test (tools/test_mo_grav_memo.sh): GXV_MO_GRAVMEMO against the original gravity query, with the real
// PlanetGravityManager.cpp, PlanetGravity.cpp, the Point / Disk / Segment gravities, GravityInfo.cpp and MathUtil.cpp.
// One deterministic run of queries (positions from a small pool, so many repeat) and gravity changes made the way the game's
// writers make them (PortGravSet / PortGravBump). It prints a hash of every answer (vector bits, result, GravityInfo) and the
// verify counts. The script runs it with GXV_MO_GRAVMEMO=0, 1 and 2: the hashes must be equal, and mode 2 must count no mismatch.
// MUTATE=1 changes a gravity without moving the generation: mode 2 must then count mismatches (the check works).
#include "Game/Gravity.hpp"
#include "port/grav_memo.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// (NameObj's constructor registers the object with the scene's NameObjRegister; the test has no scene)
NameObj::NameObj(const char* pName) : mName(pName), mFlag(), mExecutorIdx(-1) {}
extern "C" int port_on_frame_thread(void) { return 1; }
extern "C" unsigned port_mtx_profile_begin(const void*) { return 0; }  // (the port's matrix-call profiler, off)
extern "C" void port_mtx_profile_end(unsigned, unsigned) {}
#undef vsnprintf
extern "C" int port_vsnprintf(char* dst, size_t n, const char* fmt, va_list args) { return ::vsnprintf(dst, n, fmt, args); }
void JMAVECScaleAdd(const Vec* vec1, const Vec* vec2, Vec* dst, f32 scale) {  // (JMath.cpp's plain-C path)
    const f32 x = vec1->x * scale + vec2->x, y = vec1->y * scale + vec2->y, z = vec1->z * scale + vec2->z;
    dst->x = x;
    dst->y = y;
    dst->z = z;
}
extern "C" void port_logic_count(int, unsigned) {}
extern "C" int port_move_sys_flag;
int port_move_sys_flag = 0;
extern "C" void port_move_sys_begin(int) {}
extern "C" void port_move_sys_end(int) {}
static unsigned sMismatchLines, sLastMismatches;
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    ::port_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strstr(buf, "MISMATCH")) ++sMismatchLines;
    const char* m = strstr(buf, "mismatches ");
    if (m) sLastMismatches = static_cast< unsigned >(strtoul(m + 11, nullptr, 10));
    if (getenv("VERBOSE")) printf("%s\n", buf);
}

static uint64_t sRng = 88172645463325252ull;
static uint32_t rnd() {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 7;
    sRng ^= sRng << 17;
    return static_cast< uint32_t >(sRng);
}
static f32 frand(f32 a, f32 b) { return a + (b - a) * (rnd() & 0xFFFFFF) / 16777216.0f; }
static uint64_t sHash = 1469598103934665603ull;
static void mix(const void* p, size_t n) {
    const unsigned char* c = static_cast< const unsigned char* >(p);
    for (size_t i = 0; i < n; ++i) sHash = (sHash ^ c[i]) * 1099511628211ull;
}

int main() {
    const bool mutate = getenv("MUTATE") && *getenv("MUTATE") == '1';
    PlanetGravityManager* mgr = new PlanetGravityManager("grav");
    PointGravity* pts[3];
    for (int i = 0; i < 3; ++i) {
        pts[i] = new PointGravity();
        pts[i]->setOrig(TVec3f(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000)));
        pts[i]->mRange = i == 0 ? -1.0f : frand(1000, 4000);
        pts[i]->setPriority(static_cast< s32 >(rnd() % 3));
        pts[i]->updateIdentityMtx();
        mgr->registerGravity(pts[i]);
    }
    DiskGravity* disk = new DiskGravity();
    disk->setLocalPosition(TVec3f(100, 0, 50));
    disk->setLocalDirection(TVec3f(0, 1, 0));
    disk->setSideDirection(TVec3f(1, 0, 0));
    disk->setRadius(1500.0f);
    disk->setValidDegee(360.0f);
    disk->setBothSide(true);
    disk->setEnableEdgeGravity(true);
    disk->mRange = 2500.0f;
    disk->setPriority(1);
    disk->updateIdentityMtx();
    mgr->registerGravity(disk);
    SegmentGravity* seg = new SegmentGravity();
    seg->setGravityPoint(0, TVec3f(-500, 200, 0));
    seg->setGravityPoint(1, TVec3f(800, -100, 300));
    seg->setSideVector(TVec3f(0, 0, 1));
    seg->setValidSideDegree(360.0f);
    seg->setEdgeValid(0, true);
    seg->setEdgeValid(1, true);
    seg->mRange = 3000.0f;
    seg->mGravityType = GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW;
    seg->updateIdentityMtx();
    mgr->registerGravity(seg);
    PlanetGravity* all[5] = {pts[0], pts[1], pts[2], disk, seg};

    TVec3f pool[96];
    for (TVec3f& p : pool) p.set(frand(-4000, 4000), frand(-4000, 4000), frand(-4000, 4000));
    pool[0].set(0.0f, 0.0f, 0.0f);
    pool[1].set(-0.0f, 0.0f, 0.0f);  // (bit-different from pool[0]: a separate key)
    const f32 nan = __builtin_nanf("");
    pool[2].set(nan, 0.0f, 0.0f);
    unsigned queries = 0, changes = 0;
    for (int turn = 0; turn < 20000; ++turn) {
        // gravity changes, as the game's writers make them
        const uint32_t r = rnd() % 100;
        if (r < 4) {
            PortGravSet(all[rnd() % 5]->mActivated, (rnd() & 3) != 0);
            ++changes;
        } else if (r < 7) {
            PortGravSet(all[rnd() % 5]->mAppeared, (rnd() & 3) != 0);
            ++changes;
        } else if (r < 9) {
            PlanetGravity* g = all[rnd() % 5];
            TPos3f m;
            m.identity();
            m.setTrans(TVec3f(frand(-200, 200), frand(-200, 200), frand(-200, 200)));
            g->updateMtx(m);  // (GraviryFollower::update)
            PortGravBump();
            ++changes;
        } else if (r < 10) {
            PortGravSet(all[rnd() % 5]->mValidFollower, (rnd() & 1) != 0);
            ++changes;
        } else if (mutate && r < 12) {
            pts[1]->mRange = frand(500, 4000);  // (a write that does not move the generation: verify must see it)
        }
        const int n = 1 + rnd() % 12;
        for (int q = 0; q < n; ++q) {
            TVec3f pos = pool[rnd() % 96];
            if ((rnd() & 15) == 0) pos.set(frand(-4000, 4000), frand(-4000, 4000), frand(-4000, 4000));
            const u32 type = (rnd() & 3) == 0 ? GRAVITY_TYPE_SHADOW : (rnd() & 7) == 0 ? (GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW) : GRAVITY_TYPE_NORMAL;
            const u32 host = (rnd() & 7) == 0 ? static_cast< u32 >(reinterpret_cast< uintptr_t >(all[rnd() % 5]->mHost)) : 0u;
            GravityInfo info;
            TVec3f out(7.0f, 7.0f, 7.0f);
            const int form = rnd() % 4;
            bool ret;
            if (form == 0) {
                ret = mgr->calcTotalGravityVector(&out, &info, pos, type, host);
            } else if (form == 1) {
                ret = mgr->calcTotalGravityVector(&out, nullptr, pos, type, host);
            } else if (form == 2) {
                ret = mgr->calcTotalGravityVector(nullptr, &info, pos, type, host);
            } else {
                out = pos;  // (the answer written over the position it was asked for)
                ret = mgr->calcTotalGravityVector(&out, &info, out, type, host);
            }
            mix(&ret, sizeof ret);
            mix(&out, sizeof out);
            mix(&info.mGravityVector, sizeof info.mGravityVector);
            mix(&info.mLargestPriority, sizeof info.mLargestPriority);
            int which = -1;
            for (int i = 0; i < 5; ++i)
                if (info.mGravityInstance == all[i]) which = i;
            mix(&which, sizeof which);
            ++queries;
        }
    }
    sLastMismatches = 0;
    for (int i = 0; i < 70000; ++i) {  // (runs the counter to its next line, which carries the mismatch total)
        TVec3f out;
        mgr->calcTotalGravityVector(&out, nullptr, pool[3], GRAVITY_TYPE_NORMAL, 0);
    }
    printf("queries %u changes %u hash %016llx mismatches %u mismatch-lines %u\n", queries, changes, static_cast< unsigned long long >(sHash),
           sLastMismatches, sMismatchLines);
    return 0;
}
