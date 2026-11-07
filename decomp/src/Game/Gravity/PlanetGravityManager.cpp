#include "Game/Gravity.hpp"
#include "Game/Util/MathUtil.hpp"
#ifdef TARGET_PC
extern "C" void port_logic_count(int what, unsigned n);  // (logic12: logging builds' per-turn counts, platform/src/port/perf.cpp)
#include "port/move_prof.h"  // (hivelogic: GXV_ANIM_PROF=1/3 times the gravity queries, "move-sys:" in the basic log)
#include "port/grav_memo.h"
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include <cstring>
extern "C" int port_on_frame_thread(void);

// GXV_MO_GRAVMEMO (mapobjcut, 6 Nov; default 1, exact; port/grav_memo.h): a query asked again with the same position (bit for bit),
// gravity types, requester and manager while the gravity world has not changed (port_grav_gen) returns the vector, the result and
// the GravityInfo the query computed then. Static objects that ask every turn (a standing NPC, an object's drop shadow, a coin)
// pay a hash and a compare instead of a walk over every gravity. Game thread only (other threads take the original path).
//   0  off: every query walks the gravities, as before.
//   1  on.
//   2  verify: every hit is also computed the original way and compared bit for bit; a difference is counted and logged, and the
//      computed answer is used.
// Every 65536 queries (about 10 s in Honeyhive's heavy zone), logging builds write one line to log.txt:
//   grav-memo: queries Q hits H (P%) misses M world changes W mismatches X
uint32_t port_grav_gen = 1;

namespace {
int GravMemoMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_MO_GRAVMEMO");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 0;
    }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

struct GravMemoEntry {
    uint32_t gen;  // 0: empty (port_grav_gen starts at 1 and is never 0 while an entry of it lives: a wrap needs 4e9 changes)
    const void* mgr;
    uint32_t px, py, pz, type, host;
    uint32_t ret;
    TVec3f gravity;
    TVec3f infoVector;
    s32 infoPriority;
    PlanetGravity* infoInstance;
};
GravMemoEntry sGravMemo[256];
uint32_t sGravQueries, sGravHits, sGravMisses, sGravMismatches, sGravGenSeen, sGravGenChanges;

inline uint32_t Bits(f32 v) {
    uint32_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}
inline uint32_t GravMemoSlot(uint32_t x, uint32_t y, uint32_t z, uint32_t type, uint32_t host) {
    uint32_t h = x * 0x9E3779B1u;
    h = (h ^ (h >> 15) ^ y) * 0x85EBCA77u;
    h = (h ^ (h >> 13) ^ z) * 0xC2B2AE3Du;
    h = (h ^ (h >> 16) ^ type ^ (host >> 2)) * 0x27D4EB2Fu;
    return (h >> 24) & 255u;
}
inline bool SameVec(const TVec3f& a, const TVec3f& b) {
    return Bits(a.x) == Bits(b.x) && Bits(a.y) == Bits(b.y) && Bits(a.z) == Bits(b.z);
}
void GravMemoCount() {
#if PORT_LOGGING
    if (port_grav_gen != sGravGenSeen) {
        sGravGenSeen = port_grav_gen;
        ++sGravGenChanges;
    }
    if (++sGravQueries < 65536u) return;
    port_log_summary("grav-memo: queries %u hits %u (%.1f%%) misses %u world changes %u mismatches %u", sGravQueries, sGravHits,
                     100.0 * sGravHits / sGravQueries, sGravMisses, sGravGenChanges, sGravMismatches);
    sGravQueries = sGravHits = sGravMisses = sGravGenChanges = 0;  // (mismatches stay: a total since boot)
#endif
}

// PETARI_GRAVITY_CULL (forkB, 6 Nov; ported from Producdevity's fork, 1403ec9; default 1, exact): every query walked all of a
// galaxy's fields, a virtual call each, and nearly all reject the position (Toy Time: 124 fields, under 1 % accepted). Each field
// keeps a sphere that holds every position it accepts (PlanetGravity::isOutOfSphere / calcReach: point, disk, torus, cube, segment
// and parallel fields; cone and wire give none), and the walk below rejects a position outside it before the call. A position inside
// is evaluated as before, so only a sphere too small could change a result.
//   0  off: every field is evaluated.
//   1  on.
//   2  verify: a field the sphere rejects is evaluated anyway; one that accepts the position is a mismatch (the sphere's answer is
//      still used). Logging builds write every 65536 rejections:
//      grav-cull: rejected R mismatches X
int GravityCullMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("PETARI_GRAVITY_CULL");
        return e && *e ? static_cast< int >(strtol(e, nullptr, 0)) : 1;
    }());  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}
uint32_t sCullRejected, sCullMismatches;  // (verify mode; racy counts are fine: a log line)
void GravityCullVerified(bool accepted) {
    if (accepted) {
        ++sCullMismatches;
    }
#if PORT_LOGGING
    if (++sCullRejected < 65536u) return;
    port_log_summary("grav-cull: rejected %u mismatches %u", sCullRejected, sCullMismatches);
    sCullRejected = 0;  // (mismatches stay: a total since boot)
#endif
}
}  // namespace

bool PlanetGravityManager::calcTotalGravityVector(TVec3f* pGravity, GravityInfo* pInfo, const TVec3f& rPosition, u32 gravityType, u32 host) const {
    const int mode = GravMemoMode();
    if (mode == 0 || !port_on_frame_thread()) {
        return calcTotalGravityVectorRaw(pGravity, pInfo, rPosition, gravityType, host);
    }
    PORT_MOVE_SYS(0);  // (the memo's own time counts as gravity time)
    GravMemoCount();
    const uint32_t px = Bits(rPosition.x), py = Bits(rPosition.y), pz = Bits(rPosition.z);
    const uint32_t gen = port_grav_gen;
    GravMemoEntry& e = sGravMemo[GravMemoSlot(px, py, pz, gravityType, host)];
    const bool hit = e.gen == gen && e.mgr == this && e.px == px && e.py == py && e.pz == pz && e.type == gravityType && e.host == host;
    if (hit && mode == 1) {
        ++sGravHits;
        if (pInfo) {
            pInfo->mGravityVector = e.infoVector;
            pInfo->mLargestPriority = e.infoPriority;
            pInfo->mGravityInstance = e.infoInstance;
        }
        if (pGravity) {
            *pGravity = e.gravity;
        }
        return e.ret != 0;
    }
    // The original query, with a GravityInfo of its own (it only writes the info, so the vector and the result are those of a
    // query without one) and the vector always asked for (the normalisation writes nothing else).
    const TVec3f pos(rPosition);  // (rPosition may alias *pGravity or a field the caller's info points into)
    GravityInfo info;
    TVec3f gravity;
    const bool ret = calcTotalGravityVectorRaw(&gravity, &info, pos, gravityType, host);
    if (hit) {  // (mode 2)
        ++sGravHits;
        if (e.ret != static_cast< uint32_t >(ret) || !SameVec(e.gravity, gravity) || !SameVec(e.infoVector, info.mGravityVector) ||
            e.infoPriority != info.mLargestPriority || e.infoInstance != info.mGravityInstance) {
#if PORT_LOGGING
            if (sGravMismatches < 16) {
                port_log_summary("grav-memo: MISMATCH at (%g %g %g) type %u: memo %d (%g %g %g) query %d (%g %g %g)", pos.x, pos.y, pos.z,
                                 gravityType, static_cast< int >(e.ret), e.gravity.x, e.gravity.y, e.gravity.z, ret ? 1 : 0, gravity.x,
                                 gravity.y, gravity.z);
            }
#endif
            ++sGravMismatches;
        }
    } else {
        ++sGravMisses;
    }
    if (port_grav_gen == gen) {  // (the query changes nothing; checked anyway, so an entry never outlives its world)
        e.gen = gen;
        e.mgr = this;
        e.px = px;
        e.py = py;
        e.pz = pz;
        e.type = gravityType;
        e.host = host;
        e.ret = ret ? 1u : 0u;
        e.gravity = gravity;
        e.infoVector = info.mGravityVector;
        e.infoPriority = info.mLargestPriority;
        e.infoInstance = info.mGravityInstance;
    }
    if (pInfo) {
        *pInfo = info;
    }
    if (pGravity) {
        *pGravity = gravity;
    }
    return ret;
}
#endif

PlanetGravityManager::PlanetGravityManager(const char* pName) : NameObj(pName) {
    mNumGravities = 0;
#ifdef TARGET_PC
    PortGravBump();  // (GXV_MO_GRAVMEMO: a new scene's gravity world)
#endif
}

void PlanetGravityManager::init(const JMapInfoIter& rIter) {
}

#ifdef TARGET_PC
bool PlanetGravityManager::calcTotalGravityVectorRaw(TVec3f* pGravity, GravityInfo* pInfo, const TVec3f& rPosition, u32 gravityType, u32 host) const {
#else
bool PlanetGravityManager::calcTotalGravityVector(TVec3f* pGravity, GravityInfo* pInfo, const TVec3f& rPosition, u32 gravityType, u32 host) const {
#endif
    TVec3f totalGravity;
    totalGravity.z = 0.0f;
    totalGravity.y = 0.0f;
    totalGravity.x = 0.0f;

    PlanetGravity** pGravities = (PlanetGravity**)&mGravities;
    bool hasCalculated = false;

    if (pInfo) {
        pInfo->init();
    }

    f32 largestScalar = -1.0f;
    s32 largestPriority = -1;
#ifdef TARGET_PC
    const int cullMode = GravityCullMode();
    PORT_MOVE_SYS(0);
    port_logic_count(3, 1);
    port_logic_count(4, static_cast< unsigned >(mNumGravities));  // (an upper bound: the priority cut can stop it earlier)
#endif

    while (pGravities != &mGravities[mNumGravities]) {
        // Is gravity valid/active?
        bool validGravity = false;

        if ((*pGravities)->mActivated && (*pGravities)->mValidFollower && (*pGravities)->mAppeared) {
            validGravity = true;
        }

        // Gravity is valid, matches specified gravity type(s) and does not have same host
        if (validGravity && ((gravityType & (*pGravities)->mGravityType) != 0) && (host != (u32)(*pGravities)->mHost)) {
            // Is priority lower -> end of list reached as it's sorted in descending order
            s32 priority = (*pGravities)->mPriority;

            if (priority < largestPriority) {
                break;
            }

#ifdef TARGET_PC
            // PETARI_GRAVITY_CULL (top of file): a position outside the field's reach sphere is one it rejects.
            if (cullMode != 0 && (*pGravities)->isOutOfSphere(rPosition)) {
                if (cullMode == 2) {
                    TVec3f checkVec;
                    GravityCullVerified((*pGravities)->calcGravity(&checkVec, rPosition));
                }
                pGravities++;
                continue;
            }
#endif

            // Calculate gravity
            TVec3f gravityVec;

            if ((*pGravities)->calcGravity(&gravityVec, rPosition)) {
                bool storeInfo = false;
                f32 scalar = gravityVec.length();

                // If same priority, add gravity vector to total result
                if (priority == largestPriority) {
                    totalGravity += gravityVec;
                    hasCalculated = true;

                    if (largestScalar < scalar) {
                        storeInfo = true;
                    }
                }
                // If larger priority, total gravity gets overriden
                else if (priority > largestPriority) {
                    largestPriority = priority;
                    hasCalculated = true;
                    storeInfo = true;
                    totalGravity = gravityVec;
                }

                // Try to store gravity info if necessary
                if (pInfo && storeInfo) {
                    largestScalar = scalar;
                    pInfo->mGravityVector = gravityVec;
                    pInfo->mLargestPriority = largestPriority;
                    pInfo->mGravityInstance = *pGravities;
                }
            }
        }

        pGravities++;
    }

    // Store normalized vector as result if necessary
    if (pGravity) {
        MR::normalizeOrZero(&totalGravity);
        *pGravity = totalGravity;
    }

    return hasCalculated;
}

void PlanetGravityManager::registerGravity(PlanetGravity* pGravity) {
    pGravity->mIsRegistered = true;
#ifdef TARGET_PC
    PortGravBump();  // (GXV_MO_GRAVMEMO)
    pGravity->updateReach();  // (PETARI_GRAVITY_CULL: the stage-load settings are all in by now)
#endif
    int index = mNumGravities++;
    mGravities[index] = pGravity;

    sortGravities();
}

inline void PlanetGravityManager::sortGravities() {
    for (s32 i = mNumGravities - 1; i >= 1; i--) {
        if (mGravities[i]->mPriority > mGravities[i - 1]->mPriority) {
            bubbleDown(i);
        } else {
            break;
        }
    }
}

inline void PlanetGravityManager::bubbleDown(int i) {
    PlanetGravity* temp = mGravities[i - 1];
    mGravities[i - 1] = mGravities[i];
    mGravities[i] = temp;
}
