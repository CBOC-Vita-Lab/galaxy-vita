// logic23 host test (tools/test_logic23_grids.sh): GXV_GRAV_GRID (PlanetGravityManager.cpp), GXV_AREA_GRID (AreaObj.cpp: find_in) and
// its manager-name memo (AreaObjContainer.cpp: getManager), with the REAL units, against the original paths.
//   part "cells": port/logic_grid.h alone: random boxes (also degenerate, huge, touching), points on and around their faces (exact
//     bounds and +- 1..3 ulps): every box that holds a point must be among the point's candidates, in order.
//   part "gravity": 40-120 gravities (point, disk, segment; with and without a reach sphere; random priorities and types), followers
//     moving some of them every turn (marked and bumped as GraviryFollower::update does), switches toggling validity, late
//     registrations and priority changes (the array unsorted: the grid must step aside); ~1M queries. Every answer (result, vector
//     bits, GravityInfo) goes into a hash.
//   part "area": managers of 6-300 areas (cubes in every orientation, follow cubes whose matrix moves every turn, spheres, cylinders,
//     bowls), validity / wake toggles, sorts (the array reordered, with the bump), late entries; points inside, on and near the cubes'
//     faces, random, far, NaN / infinite. Every find_in answer (the area's index) goes into the hash. Then a container of managers with
//     prefix-sharing names asked by literal pointers and by one buffer whose text changes (getManager).
// The script runs the binary with each switch at 0, 1 and 2: the hashes must be equal, mode 2 must count 0 mismatches and the grids
// must actually answer (counts printed). The negative control (PORT_LGRID_NEG: boxes filed 25 % too small, and the memo not checking
// the text) must FAIL.
#include "Game/Gravity.hpp"
#include "Game/AreaObj/AreaForm.hpp"
#include "Game/AreaObj/AreaObj.hpp"
#define private public
#include "Game/AreaObj/AreaObjContainer.hpp"
#undef private
#include "port/grav_memo.h"
#include "port/logic_grid.h"
#include "port/obj_cut.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <pthread.h>

// ---- stubs: what the units reach outside the game ----
// (NameObj's constructor registers the object with the scene's NameObjRegister; the test has no scene)
NameObj::NameObj(const char* pName) : mName(pName), mFlag(), mExecutorIdx(-1) {}
// (THREADS=1: a second thread looks areas up beside the game thread; only the game thread is the "frame thread", as on the console)
static pthread_t sMainThread = pthread_self();
extern "C" int port_on_frame_thread(void) { return pthread_equal(pthread_self(), sMainThread) ? 1 : 0; }
extern "C" unsigned port_mtx_profile_begin(const void*) { return 0; }
extern "C" void port_mtx_profile_end(unsigned, unsigned) {}
#undef vsnprintf
extern "C" int port_vsnprintf(char* dst, size_t n, const char* fmt, va_list args) { return ::vsnprintf(dst, n, fmt, args); }
extern "C" int port_snprintf(char* dst, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = ::vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}
void JMAVECScaleAdd(const Vec* vec1, const Vec* vec2, Vec* dst, f32 scale) {
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
int port_logic_prof_flag = 0;
extern "C" void port_logic_prof_sys_begin(int) {}
extern "C" void port_logic_prof_sys_end(int) {}
extern "C" void port_logic_prof_count(int, unsigned) {}
static int64_t sFakeNs = 0;
extern "C" int64_t port_host_time_ns(void) { return sFakeNs += 50000000; }  // (5 s every 100 reads: the stats lines come out)
extern "C" int port_hl_areacube_on(void) { return getenv("GXV_HL_AREACUBE") ? atoi(getenv("GXV_HL_AREACUBE")) : 1; }
extern "C" void port_hl_verify_area(int, int, int, const char*, const float*, const float*) {}
uint32_t port_area_gen = 1;
static unsigned sMismatchLines;
static char sLastGrav[512], sLastArea[512];
static unsigned long long sGravWalks, sGravThroughGrid, sGravVisited, sGravOfAll, sAreaLookups, sAreaThroughGrid, sAreaTested, sAreaOfAll,
    sMemoAsked, sMemoHits;
extern "C" void port_log_summary(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    ::port_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strstr(buf, "MISMATCH")) ++sMismatchLines;
    unsigned a, b, c, d, e, f;
    if (!strncmp(buf, "grav-grid:", 10)) {
        snprintf(sLastGrav, sizeof sLastGrav, "%s", buf);
        if (sscanf(buf, "grav-grid: %u walks, %u through the grid (%u of %u", &a, &b, &c, &d) == 4) {
            sGravWalks += a, sGravThroughGrid += b, sGravVisited += c, sGravOfAll += d;
        }
    }
    if (!strncmp(buf, "area-grid:", 10) && !strstr(buf, "MISMATCH")) {
        snprintf(sLastArea, sizeof sLastArea, "%s", buf);
        const char* m = strstr(buf, "manager memo ");
        if (sscanf(buf, "area-grid: %u lookups, %u through the grid (%u of %u", &a, &b, &c, &d) == 4 && m &&
            sscanf(m, "manager memo %u of %u", &e, &f) == 2) {
            sAreaLookups += a, sAreaThroughGrid += b, sAreaTested += c, sAreaOfAll += d, sMemoHits += e, sMemoAsked += f;
        }
    }
    if (getenv("VERBOSE")) printf("%s\n", buf);
}
extern "C" uint32_t gPortAreaGridStats[10];

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
static f32 ulps(f32 v, int k) {
    for (int i = 0; i < k; ++i) v = std::nextafter(v, k > 0 ? HUGE_VALF : -HUGE_VALF);
    return v;
}
static f32 jiggle(f32 v) {
    const int k = static_cast< int >(rnd() % 7) - 3;
    f32 r = v;
    for (int i = 0; i < (k < 0 ? -k : k); ++i) r = std::nextafter(r, k > 0 ? HUGE_VALF : -HUGE_VALF);
    return r;
}

// ---- part "cells": the grid alone ----
static int CellsTest() {
    long checks = 0, missing = 0, orderBad = 0;
    for (int scen = 0; scen < 3000; ++scen) {
        const int n = 1 + rnd() % 200;
        std::vector< port::lgrid::Box > boxes(n);
        std::vector< uint8_t > st(n);
        const f32 far = (scen % 4 == 0) ? 50.0f : (scen % 4 == 1) ? 5000.0f : (scen % 4 == 2) ? 2e5f : 3e7f;
        for (int i = 0; i < n; ++i) {
            st[i] = (rnd() % 5) != 0;
            for (int a = 0; a < 3; ++a) {
                const f32 c = frand(-far, far);
                f32 h = (rnd() % 9 == 0) ? 0.0f : frand(0.0f, far * ((rnd() & 3) ? 0.05f : 0.6f));
                boxes[i].lo[a] = c - h;
                boxes[i].hi[a] = (rnd() % 13 == 0) ? boxes[i].lo[a] : c + h;
            }
        }
        port::lgrid::Grid g;
        if (!g.Build(boxes.data(), st.data(), n, 1 + rnd() % 24, 4096 + rnd() % 60000)) {
            continue;
        }
        for (int q = 0; q < 300; ++q) {
            f32 p[3];
            const int bi = rnd() % n;
            for (int a = 0; a < 3; ++a) {
                const int mode = rnd() % 4;
                p[a] = mode == 0 ? jiggle(boxes[bi].lo[a]) : mode == 1 ? jiggle(boxes[bi].hi[a]) : mode == 2 ? frand(boxes[bi].lo[a], boxes[bi].hi[a])
                                                                                                             : frand(-2 * far, 2 * far);
            }
            const uint16_t* sb;
            const uint16_t* se;
            g.Cell(p[0], p[1], p[2], &sb, &se);
            std::vector< int > got;
            port::lgrid::ForEachAscending(g, sb, se, [&](int i) {
                got.push_back(i);
                return false;
            });
            for (size_t k = 1; k < got.size(); ++k) orderBad += got[k] <= got[k - 1];
            std::vector< int > down;
            port::lgrid::ForEachDescending(g, sb, se, [&](int i) {
                down.push_back(i);
                return false;
            });
            if (down.size() != got.size()) ++orderBad;
            for (size_t k = 0; k < down.size() && k < got.size(); ++k) orderBad += down[k] != got[got.size() - 1 - k];
            for (int i = 0; i < n; ++i) {
                bool inside = true;
                for (int a = 0; a < 3; ++a) inside &= boxes[i].lo[a] <= p[a] && p[a] <= boxes[i].hi[a];
                if (!st[i]) inside = true;  // (dynamic: always a candidate)
                if (!inside) continue;
                ++checks;
                bool found = false;
                for (int k : got) found |= k == i;
                missing += !found;
            }
        }
    }
    printf("cells: %ld box/point pairs that must be candidates, %ld missing, %ld order errors\n", checks, missing, orderBad);
    return missing == 0 && orderBad == 0 && checks > 100000 ? 0 : 1;
}

// ---- part "gravity" ----
static void GravityTest() {
    PlanetGravityManager* mgr = new PlanetGravityManager("grav");
    std::vector< PlanetGravity* > all;
    std::vector< PlanetGravity* > movers;
    auto addOne = [&](bool late) {
        PlanetGravity* g = nullptr;
        const int kind = rnd() % 3;
        const f32 W = 30000.0f;
        if (kind == 0) {
            PointGravity* p = new PointGravity();
            p->setOrig(TVec3f(frand(-W, W), frand(-W, W), frand(-W, W)));
            g = p;
        } else if (kind == 1) {
            DiskGravity* d = new DiskGravity();
            d->setLocalPosition(TVec3f(frand(-W, W), frand(-W, W), frand(-W, W)));
            d->setLocalDirection(TVec3f(0, 1, 0));
            d->setSideDirection(TVec3f(1, 0, 0));
            d->setRadius(frand(200, 3000));
            d->setValidDegee(360.0f);
            d->setBothSide((rnd() & 1) != 0);
            d->setEnableEdgeGravity(true);
            g = d;
        } else {
            SegmentGravity* s = new SegmentGravity();
            const TVec3f c(frand(-W, W), frand(-W, W), frand(-W, W));
            s->setGravityPoint(0, c);
            s->setGravityPoint(1, c + TVec3f(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000)));
            s->setSideVector(TVec3f(0, 0, 1));
            s->setValidSideDegree(360.0f);
            s->setEdgeValid(0, true);
            s->setEdgeValid(1, true);
            g = s;
        }
        g->mRange = (rnd() % 10 == 0) ? -1.0f : frand(500, 9000);  // (-1: no reach sphere, a candidate everywhere)
        g->mGravityType = (rnd() % 4 == 0) ? GRAVITY_TYPE_SHADOW : (rnd() % 3 == 0) ? (GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW) : GRAVITY_TYPE_NORMAL;
        g->mHost = (rnd() % 6 == 0) ? reinterpret_cast< const void* >(static_cast< uintptr_t >(0x1000 + 16 * (rnd() % 4))) : nullptr;
        g->setPriority(static_cast< s32 >(rnd() % 4));
        g->updateIdentityMtx();
        mgr->registerGravity(g);
        all.push_back(g);
        if (!late && rnd() % 8 == 0) {
            movers.push_back(g);
        }
    };
    const int n0 = 40 + rnd() % 60;
    for (int i = 0; i < n0; ++i) addOne(false);
    unsigned queries = 0;
    for (int turn = 0; turn < 12000; ++turn) {
        // followers: marked moving once (with the bump), then a new matrix, the memo's bump and the sphere, every turn (GraviryFollower::update)
        for (PlanetGravity* g : movers) {
            if (rnd() % 4 == 0) continue;
            if (!g->mPortMoving) {
                g->mPortMoving = true;
                PortGravGeoBump();
            }
            TPos3f m;
            m.identity();
            m.setTrans(TVec3f(frand(-800, 800), frand(-800, 800), frand(-800, 800)));
            g->updateMtx(m);
            PortGravBump();
            g->updateReach();
        }
        const uint32_t r = rnd() % 1000;
        if (r < 30) {
            PortGravSet(all[rnd() % all.size()]->mActivated, (rnd() & 3) != 0);
        } else if (r < 50) {
            PortGravSet(all[rnd() % all.size()]->mAppeared, (rnd() & 3) != 0);
        } else if (r < 51) {
            all[rnd() % all.size()]->setPriority(static_cast< s32 >(rnd() % 4));  // (may leave the array unsorted: the grid steps aside)
        } else if (r < 53 && all.size() < 127) {
            addOne(true);  // (a late registration)
        } else if (r < 56) {
            // (sorted again, as a new scene would have it: a fresh manager with the same gravities)
            mgr = new PlanetGravityManager("grav2");
            for (PlanetGravity* g : all) mgr->registerGravity(g);
        }
        const int nq = 40 + rnd() % 80;
        for (int q = 0; q < nq; ++q) {
            TVec3f pos;
            const int pm = rnd() % 8;
            if (pm < 5) {
                PlanetGravity* g = all[rnd() % all.size()];
                const f32 rad = g->mReachSquared >= 0.0f ? std::sqrt(g->mReachSquared) : 3000.0f;
                pos = g->mReachCentre + TVec3f(frand(-1.1f, 1.1f) * rad, frand(-1.1f, 1.1f) * rad, frand(-1.1f, 1.1f) * rad);
                if (pm == 4) {  // (on the sphere's box faces, to the ulp)
                    pos.x = jiggle(g->mReachCentre.x + ((rnd() & 1) ? rad : -rad));
                }
            } else if (pm == 5) {
                pos.set(frand(-40000, 40000), frand(-40000, 40000), frand(-40000, 40000));
            } else if (pm == 6) {
                pos.set(frand(-1e7f, 1e7f), frand(-1e7f, 1e7f), frand(-1e7f, 1e7f));
            } else {
                const f32 sp[4] = {__builtin_nanf(""), HUGE_VALF, -HUGE_VALF, 3e19f};
                pos.set(frand(-100, 100), sp[rnd() % 4], 0.0f);
            }
            const u32 type = (rnd() & 3) == 0 ? GRAVITY_TYPE_SHADOW : (rnd() & 7) == 0 ? (GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW) : GRAVITY_TYPE_NORMAL;
            const u32 host = (rnd() & 7) == 0 ? static_cast< u32 >(0x1000 + 16 * (rnd() % 4)) : 0u;
            GravityInfo info;
            TVec3f out(7.0f, 7.0f, 7.0f);
            const int form = rnd() % 3;
            bool ret;
            if (form == 0) {
                ret = mgr->calcTotalGravityVector(&out, &info, pos, type, host);
            } else if (form == 1) {
                ret = mgr->calcTotalGravityVector(&out, nullptr, pos, type, host);
            } else {
                ret = mgr->calcTotalGravityVector(nullptr, &info, pos, type, host);
            }
            mix(&ret, sizeof ret);
            mix(&out, sizeof out);
            mix(&info.mGravityVector, sizeof info.mGravityVector);
            mix(&info.mLargestPriority, sizeof info.mLargestPriority);
            int which = -1;
            for (size_t i = 0; i < all.size(); ++i)
                if (info.mGravityInstance == all[i]) which = static_cast< int >(i);
            mix(&which, sizeof which);
            ++queries;
        }
    }
    printf("gravity: %zu gravities (%zu moving), %u queries\n", all.size(), movers.size(), queries);
}

// ---- part "area" ----
static AreaObj* MakeArea(int form, f32 W, TPos3f* pFollow) {
    AreaObj* a = new AreaObj(form, "area");
    if (form == AreaForm::Type_Cube1 || form == AreaForm::Type_Cube2) {
        AreaFormCube* c = static_cast< AreaFormCube* >(a->mForm);
        c->mTranslation.set(frand(-W, W), frand(-W, W), frand(-W, W));
        c->mRotation.set(0, 0, 0);
        // (angles kept within the s16 range MR::makeMtxRotate converts them to: beyond it the conversion is undefined in C++)
        if (rnd() % 3 == 0) c->mRotation.set(frand(-179, 179), frand(-179, 179), frand(-179, 179));
        else if (rnd() % 2 == 0) c->mRotation.set(0, 90.0f * (static_cast< int >(rnd() % 4) - 2), 0);
        const f32 sc = (rnd() % 5 == 0) ? 0.02f : (rnd() % 5 == 1) ? 30.0f : 3.0f;
        c->mScale.set(frand(0.0f, sc), frand(0.0f, sc), frand(0.0f, sc));
        c->updateBoxParam();
        if (pFollow != nullptr && rnd() % 6 == 0) a->setFollowMtx(pFollow);
    } else if (form == AreaForm::Type_Sphere) {
        AreaFormSphere* s = static_cast< AreaFormSphere* >(a->mForm);
        s->mTranslation.set(frand(-W, W), frand(-W, W), frand(-W, W));
        s->mRadius = frand(100, 3000);
        s->mUp.set(0, 1, 0);
    } else if (form == AreaForm::Type_Cylinder) {
        AreaFormCylinder* c = static_cast< AreaFormCylinder* >(a->mForm);
        c->mTranslation.set(frand(-W, W), frand(-W, W), frand(-W, W));
        c->mRotation.set(0, 1, 0);
        c->mRadius = frand(100, 3000);
        c->mHeight = frand(100, 3000);
    } else {
        AreaFormBowl* b = static_cast< AreaFormBowl* >(a->mForm);
        b->mTranslation.set(frand(-W, W), frand(-W, W), frand(-W, W));
        b->mUp.set(0, 1, 0);
        b->_20 = frand(100, 3000);
    }
    return a;
}

// THREADS=1: another thread (a loading or audio thread on the console) makes lookups and manager searches on a manager and a container
// of its own while the game thread runs the area part: it must take the original paths and touch none of the grids' or the memo's state
// (run under ThreadSanitizer by tools/test_logic23_grids.sh tsan).
static std::atomic< bool > sOtherStop{false};
static std::atomic< unsigned long > sOtherLookups{0};
static AreaObjMgr* sOtherMgr;
static AreaObjContainer* sOtherCtr;
static void* OtherThread(void*) {
    uint64_t r = 12345;
    while (!sOtherStop.load(std::memory_order_relaxed)) {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        const TVec3f pos(static_cast< f32 >((r >> 40) % 40000) - 20000.0f, static_cast< f32 >((r >> 20) % 40000) - 20000.0f, 0.0f);
        AreaObj* a = sOtherMgr->find_in(pos);
        AreaObjMgr* m = sOtherCtr->getManager((r & 1) ? "Water" : "CubeCamera");
        if (a == reinterpret_cast< AreaObj* >(1) || m == nullptr) break;  // (keeps the calls)
        sOtherLookups.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

static void AreaTest() {
    pthread_t other{};
    const bool threads = getenv("THREADS") && *getenv("THREADS") == '1';
    if (threads) {
        TPos3f still;
        still.identity();
        sOtherMgr = new AreaObjMgr(64, "other");
        for (int i = 0; i < 40; ++i) sOtherMgr->entry(MakeArea(i % 4 == 0 ? AreaForm::Type_Sphere : AreaForm::Type_Cube1, 20000.0f, nullptr));
        sOtherCtr = new AreaObjContainer("other container");
        sOtherCtr->mManagerArray[sOtherCtr->mNumManagers++] = new AreaObjMgr(4, "CubeCamera");
        sOtherCtr->mManagerArray[sOtherCtr->mNumManagers++] = new AreaObjMgr(4, "Water");
        pthread_create(&other, nullptr, OtherThread, nullptr);
    }
    TPos3f follow;
    follow.identity();
    std::vector< AreaObjMgr* > mgrs;
    std::vector< int > sizes = {3, 6, 9, 20, 57, 120, 300};
    for (int s : sizes) {
        AreaObjMgr* m = new AreaObjMgr(s + 64, "mgr");
        const f32 W = (s > 100) ? 60000.0f : 20000.0f;
        for (int i = 0; i < s; ++i) {
            const int r = rnd() % 10;
            const int form = r < 7 ? (r & 1 ? AreaForm::Type_Cube1 : AreaForm::Type_Cube2) : r == 7 ? AreaForm::Type_Sphere
                                                                                            : r == 8 ? AreaForm::Type_Cylinder
                                                                                                     : AreaForm::Type_Bowl;
            m->entry(MakeArea(form, W, &follow));
        }
        mgrs.push_back(m);
    }
    unsigned lookups = 0;
    for (int turn = 0; turn < 6000; ++turn) {
        follow.setTrans(TVec3f(frand(-3000, 3000), frand(-3000, 3000), frand(-3000, 3000)));  // (follow cubes move: no bump, they are dynamic)
        const uint32_t r = rnd() % 1000;
        AreaObjMgr* m = mgrs[rnd() % mgrs.size()];
        if (r < 40 && m->mArray.size()) {
            AreaObj* a = m->mArray[rnd() % m->mArray.size()];
            const int k = rnd() % 4;
            if (k == 0) a->validate();
            else if (k == 1) a->invalidate();
            else if (k == 2) a->awake();
            else a->sleep();
        } else if (r < 45 && m->mArray.size() > 1) {
            // (a sort: two areas exchanged, with the bump the sorts make)
            PortAreaGeoBump();
            const int i = rnd() % m->mArray.size(), j = rnd() % m->mArray.size();
            AreaObj* t = m->mArray[i];
            m->mArray[i] = m->mArray[j];
            m->mArray[j] = t;
        } else if (r < 47 && m->mArray.size() < m->_18) {
            m->entry(MakeArea(rnd() % 2 ? AreaForm::Type_Cube1 : AreaForm::Type_Sphere, 20000.0f, &follow));  // (a late entry)
        }
        const int nq = 60 + rnd() % 60;
        for (int q = 0; q < nq; ++q) {
            AreaObjMgr* qm = mgrs[rnd() % mgrs.size()];
            TVec3f pos;
            const int pm = rnd() % 8;
            const int n = qm->mArray.size();
            AreaObj* a = n ? qm->mArray[rnd() % n] : nullptr;
            if (pm < 5 && a != nullptr && (a->mFormType == AreaForm::Type_Cube1 || a->mFormType == AreaForm::Type_Cube2)) {
                AreaFormCube* c = static_cast< AreaFormCube* >(a->mForm);
                f32 l[3] = {frand(c->mBounding.i.x, c->mBounding.f.x), frand(c->mBounding.i.y, c->mBounding.f.y), frand(c->mBounding.i.z, c->mBounding.f.z)};
                const int ax = rnd() % 3;
                if (pm == 1) l[ax] = (rnd() & 1) ? (&c->mBounding.f.x)[ax] : (&c->mBounding.i.x)[ax];
                if (pm == 2) l[ax] = jiggle((rnd() & 1) ? (&c->mBounding.f.x)[ax] : (&c->mBounding.i.x)[ax]);
                if (pm == 3) l[ax] += frand(-200.0f, 200.0f);
                TPos3f w;
                c->calcWorldMtx(&w);
                w.mult(TVec3f(l[0], l[1], l[2]), pos);
                if (pm == 4) {  // (a corner of the cube in the world, to the ulp)
                    TVec3f corner;
                    w.mult((rnd() & 1) ? c->mBounding.f : c->mBounding.i, corner);
                    pos.set(jiggle(corner.x), jiggle(corner.y), jiggle(corner.z));
                }
            } else if (pm < 6) {
                pos.set(frand(-70000, 70000), frand(-70000, 70000), frand(-70000, 70000));
            } else if (pm == 6) {
                pos.set(frand(-1e9f, 1e9f), frand(-100, 100), frand(-1e9f, 1e9f));
            } else {
                const f32 sp[4] = {__builtin_nanf(""), HUGE_VALF, -HUGE_VALF, 3e31f};
                pos.set(sp[rnd() % 4], frand(-100, 100), 0.0f);
            }
            AreaObj* found = qm->find_in(pos);
            int idx = -2;
            for (int i = 0; i < qm->mArray.size(); ++i)
                if (qm->mArray[i] == found) idx = i;
            if (found == nullptr) idx = -1;
            mix(&idx, sizeof idx);
            ++lookups;
        }
    }
    // the manager-name memo: managers with names sharing prefixes, asked by literals and by one buffer whose text changes
    AreaObjContainer* ctr = new AreaObjContainer("container");
    static const char* const kNames[] = {"SwitchArea", "CubeCamera", "Water", "WaterArea", "DeathArea", "LightArea", "SoundEmitterCube",
                                         "SoundEmitterSphere", "BindEndCube", "PlaneModeCube", "Air", "AirBubble"};
    for (const char* nm : kNames) {
        ctr->mManagerArray[ctr->mNumManagers++] = new AreaObjMgr(4, nm);
    }
    static const char* const kAsk[] = {"SwitchArea", "CubeCamera", "WaterArea", "WaterCube", "DeathArea", "LightAreaX", "SoundEmitterCube",
                                       "SoundEmitterSphere", "BindEndCube", "PlaneModeCube", "AirBubble", "AirX"};
    char buf[40];
    unsigned asks = 0;
    for (int i = 0; i < 200000; ++i) {
        const char* ask;
        if (rnd() % 3 == 0) {
            snprintf(buf, sizeof buf, "%s", kAsk[rnd() % 12]);  // (the same pointer, another text)
            ask = buf;
        } else {
            ask = kAsk[rnd() % 12];
        }
        AreaObjMgr* m = ctr->getManager(ask);
        int idx = -1;
        for (u32 k = 0; k < ctr->mNumManagers; ++k)
            if (ctr->mManagerArray[k] == m) idx = static_cast< int >(k);
        mix(&idx, sizeof idx);
        ++asks;
    }
    if (threads) {
        sOtherStop.store(true);
        pthread_join(other, nullptr);
        printf("other thread: %lu lookups beside the game thread\n", sOtherLookups.load());
    }
    printf("area: %zu managers, %u lookups; %u manager lookups\n", mgrs.size(), lookups, asks);
}

int main(int argc, char** argv) {
    const char* part = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (!strcmp(part, "cells") || !strcmp(part, "all")) rc |= CellsTest();
    if (!strcmp(part, "gravity") || !strcmp(part, "all")) GravityTest();
    if (!strcmp(part, "area") || !strcmp(part, "all")) AreaTest();
    // (the stats lines: forced out)
    sFakeNs += 10000000000ll;
    printf("hash %016llx mismatch-lines %u\n", static_cast< unsigned long long >(sHash), sMismatchLines);
    printf("last grav-grid line: %s\n", sLastGrav[0] ? sLastGrav : "(none)");
    printf("last area-grid line: %s\n", sLastArea[0] ? sLastArea : "(none)");
    printf("totals from the stats lines: gravity walks %llu, through the grid %llu (%llu of %llu gravities visited); area lookups %llu, "
           "through the grid %llu (%llu of %llu areas tested); manager memo %llu of %llu\n",
           sGravWalks, sGravThroughGrid, sGravVisited, sGravOfAll, sAreaLookups, sAreaThroughGrid, sAreaTested, sAreaOfAll, sMemoHits, sMemoAsked);
    printf("area-grid mismatches %u memo-mismatches %u; manager memo since the last line: %u hits of %u\n", gPortAreaGridStats[8],
           gPortAreaGridStats[9], gPortAreaGridStats[7], gPortAreaGridStats[6]);
    return rc;
}
