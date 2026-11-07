#include "Game/Map/KCollision.hpp"
#include "Game/Camera/CameraPolygonCodeUtil.hpp"
#include "Game/Map/CollisionCode.hpp"
#include "Game/Map/CollisionDirector.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/MathUtil.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

// GXV_KCL_MEMO (kcl, 6 Nov; default 1, vita/vgx-game/entry.cpp; exact): one query's octree walk (checkSphere,
// checkSphereWithThickness, checkArea3D, checkArrow) meets the same prism again in every leaf cell the prism crosses. The original
// tests it again each time and the answer cannot change (same prism, same query), so the walk remembers the prisms it has already
// looked at in a small direct-mapped table on the stack (KclSeen) and goes past them:
//   - sphere / area walks: a prism met again either missed (it misses again), or was stored (the std::find skipped it), or hit with
//     the output full (foundCount only grows, so it is dropped again). Every prism met again is passed.
//   - line walk: only prisms that MISSED are remembered. A prism that hit is tested and reported again in each cell, as before
//     (the collect mode reports it once per cell, and that is kept).
// A slot holds one prism index; a prism pushed out by another one is simply tested again, as before. Hits, their order, distances
// and flags are the same bits. 0: every prism is tested in every cell, as before.
// 2 (verify, kcl2): every query runs the memo walk into scratch arrays and then the original walk into the caller's arrays (the
// caller gets the original's answer), and the two are compared bit for bit: count, prisms in order, distances, flags, the best
// prism. "kcl-verify:" lines (port_log_summary): the first 8 mismatches in full (kind, KCL, local query as hex floats), then one
// line per 65536 checks with the totals. The walks themselves are checkSphereWalk & co. (KCollision.hpp), with the memo as a
// parameter, so the two runs share no state.
#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/hot_knob.h"
#include <cstdlib>
namespace {
    // 0 off, 1 on, 2 verify (unset or anything else: 1)
    inline int kclMemoMode() {
        return PORT_HOT_KNOB(int, [] { const char* e = std::getenv("GXV_KCL_MEMO"); return e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1; }());
    }
#if PORT_LOGGING
    // (logging builds: one line per 2^18 queries, about every 1-3 minutes of play: prisms tested, prisms passed by the memo)
    u32 sKclQueries = 0, sKclTested = 0, sKclPassed = 0;
    inline void kclCount(u32 tested, u32 passed) {
        sKclTested += tested;
        sKclPassed += passed;
        if ((++sKclQueries & 0x3FFFF) == 0) {
            port_log_summary("kcl: last 262144 map queries: %u prism tests, %u repeats passed by the memo (GXV_KCL_MEMO=%d)", sKclTested,
                             sKclPassed, kclMemoMode());
            sKclTested = sKclPassed = 0;
        }
    }
#define KCL_COUNT(tested, passed) kclCount((tested), (passed))
#else
#define KCL_COUNT(tested, passed) ((void)(tested), (void)(passed))
#endif
}  // namespace
#else
#define KCL_COUNT(tested, passed) ((void)(tested), (void)(passed))
namespace {
    inline int kclMemoMode() {
        return 0;
    }
}  // namespace
#endif

#ifndef KCL_SEEN_SLOTS
#define KCL_SEEN_SLOTS 256
#endif
namespace {
    // The prisms one walk has looked at, by index (prism lists hold u16 indices, never 0; 0 marks a free slot). 512 bytes cleared
    // per query, only when the memo is on. mPushedOut: some prism lost its slot in this walk. Until then the table holds every prism
    // the walk has looked at, so a prism it does not hold was never looked at, and so is not among the walk's results either: the
    // walk can leave out its std::find over them (the results only ever hold prisms looked at earlier in the same walk).
    struct KclSeen {
        u16 mSlot[KCL_SEEN_SLOTS];
        bool mPushedOut;

        void clear() {
            std::memset(mSlot, 0, sizeof(mSlot));
            mPushedOut = false;
        }
        // true: idx was already put in; otherwise puts it in (pushing out whichever prism had its slot)
        bool testAndPut(u16 idx) {
            u16& rSlot = mSlot[idx & (KCL_SEEN_SLOTS - 1)];
            if (rSlot == idx) {
                return true;
            }
            if (rSlot != 0) {
                mPushedOut = true;
            }
            rSlot = idx;
            return false;
        }
        bool has(u16 idx) const {
            return mSlot[idx & (KCL_SEEN_SLOTS - 1)] == idx;
        }
        void put(u16 idx) {
            mSlot[idx & (KCL_SEEN_SLOTS - 1)] = idx;
        }
    };
}  // namespace

void KCollision_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.000003814697265625f;
    (void)0.001f;
    (void)1000000000.0f;
    (void)0.01f;
}
f32 KCollision_FORCE_MATCH_CONVERSION(s32 value) {
    return value;
}

// trying to get both operator== and operator/ to emit
void DUMMY_KCollision() {
    TVec3f a, b, c;

    if (a == b) {
        c = a / b.x;
    }
}

void DUMMY_KCollision2() {
    TVec3f a, b, c;

    if (a == b) {
        c = a / b.x;
    }
}

KCollisionServer::KCollisionServer() : mFile(), mapInfo(new JMapInfo()), mMaxVertexDistance(1.0f) {
}

void KCollisionServer::init(void* pData, const void* pMapData) {
    setData(pData);

    if (pMapData != nullptr) {
        mapInfo->attach(pMapData);
    }
}

void KCollisionServer::setData(void* pData) {
    mFile = reinterpret_cast< KCLFile* >(pData);

    if (!isBinaryInitialized(pData)) {
        mFile->mPos = reinterpret_cast< TVec3f* >(reinterpret_cast< u8* >(mFile) + mFile->mPosOffset);
        mFile->mNorms = reinterpret_cast< TVec3f* >(reinterpret_cast< u8* >(mFile) + mFile->mNormOffset);
        mFile->mPrisms = reinterpret_cast< KC_PrismData* >(reinterpret_cast< u8* >(mFile) + mFile->mPrismOffset);
        mFile->mOctree = reinterpret_cast< void* >(reinterpret_cast< u8* >(mFile) + mFile->mOctreeOffset);
    }
}

bool KCollisionServer::calcFarthestVertexDistance() {
    s32 triCount = getTriangleNum();
    f32 maxDistance = 0.0f;
    KC_PrismData* prism;
    bool result = true;

    for (u32 i = 0; i < (u32)triCount; i++) {
        prism = &mFile->mPrisms[i + 1];
        JMapInfoIter iter = getAttributes(i);

        if (!iter.isValid()) {
            result = false;
        } else {
            MR::registerCameraCode(MR::getCollisionDirector()->mCode->getCameraID(iter));
        }

        if (isNearParallelNormal(prism)) {
            prism->mHeight = -MR::abs(prism->mHeight);
        } else {
            for (s32 j = 0; j < 3; j++) {
                TVec3f pos = getPos(prism, j);
                f32 distSq = pos.squared();

                if (maxDistance < distSq) {
                    maxDistance = distSq;
                }
            }
        }
    }

    mMaxVertexDistance = MR::sqrt(maxDistance);
    return result;
}

bool KCollisionServer::isBinaryInitialized(const void* pData) NO_INLINE {
#if TARGET_PC
    const KCLFile* file = static_cast< const KCLFile* >(pData);
    return port_is_native_range(file->mPos.get(), sizeof(TVec3f));
#else
    return reinterpret_cast< const s32* >(pData)[0] < 0;
#endif
}

KC_PrismData* KCollisionServer::checkPoint(Fxyz* pPoint, f32 param, f32* pDist) {
    f32 maxDist = mFile->mThickness * param;
    u32 x = (s32)(pPoint->x - mFile->mMin.x);

    if (x & mFile->mXMask) {
        return nullptr;
    }

    u32 y = (s32)(pPoint->y - mFile->mMin.y);

    if (y & mFile->mYMask) {
        return nullptr;
    }

    u32 z = (s32)(pPoint->z - mFile->mMin.z);

    if (z & mFile->mZMask) {
        return nullptr;
    }

    s32 shift;
    u16* prismList = (u16*)searchBlock(&shift, x, y, z);

    while (*++prismList != 0) {
        KC_PrismData* prism = &mFile->mPrisms[*prismList];
        f32 height = prism->mHeight;

        if (height <= 0.0f) {
            continue;
        }

        TVec3f* vtx = &mFile->mPos[prism->mPositionIndex];

        Fxyz dir;
        dir.x = pPoint->x - vtx->x;
        dir.y = pPoint->y - vtx->y;
        dir.z = pPoint->z - vtx->z;

        TVec3f& edgeNormal0 = mFile->mNorms[prism->mEdgeIndices[0]];

        if (dir.x * edgeNormal0.x + dir.y * edgeNormal0.y + dir.z * edgeNormal0.z > 0.0f) {
            continue;
        }

        TVec3f& edgeNormal1 = mFile->mNorms[prism->mEdgeIndices[1]];

        if (dir.x * edgeNormal1.x + dir.y * edgeNormal1.y + dir.z * edgeNormal1.z > 0.0f) {
            continue;
        }

        TVec3f& edgeNormal2 = mFile->mNorms[prism->mEdgeIndices[2]];

        if (dir.x * edgeNormal2.x + dir.y * edgeNormal2.y + dir.z * edgeNormal2.z > height) {
            continue;
        }

        TVec3f& faceNormal = mFile->mNorms[prism->mNormalIndex];
        f32 dist = -dir.x * faceNormal.x - dir.y * faceNormal.y - dir.z * faceNormal.z;

        if (dist < 0.0f) {
            continue;
        }

        if (maxDist < dist) {
            continue;
        }

        *pDist = dist;
        return prism;
    }

    return nullptr;
}

u32 KCollisionServer::checkArea3DWalk(Fxyz* pMin, Fxyz* pMax, KC_PrismData** pOut, u32 maxCount, bool memo) {
    s32 remX;
    s32 yStep;
    s32 zStep;
    s32 bestRemY;
    u16* list;
    u16* nextSkipList = nullptr;
    u16* skipList = nullptr;
    u32 foundCount = 0;

    Fxyz box[2];
    box[0] = *pMin;
    box[1] = *pMax;

    V3u pointMin;
    V3u pointMax;
    TVec3f queryMax;
    TVec3f queryMin;
    MR::createBoundingBox((TVec3f*)box, 2, &queryMin, &queryMax);

    if (queryMin.x == queryMax.x) {
        queryMin.x -= 1.0f;
        queryMax.x += 1.0f;
    }

    if (queryMin.y == queryMax.y) {
        queryMin.y -= 1.0f;
        queryMax.y += 1.0f;
    }

    if (queryMin.z == queryMax.z) {
        queryMin.z -= 1.0f;
        queryMax.z += 1.0f;
    }

    if (!outCheck(&queryMin, &queryMax, &pointMin, &pointMax)) {
        return 0;
    }

    // GXV_KCL_MEMO (top of file): the prisms this walk has looked at (memo: the caller's choice, see the wrappers)
    KclSeen seen;
    u32 portTested = 0, portPassed = 0;
    if (memo) {
        seen.clear();
    }

    u32 x;
    u32 y;
    u32 z;
    KC_PrismData* prism;
    s32 shift;
    z = pointMin.z;

    do {
        y = pointMin.y;
        zStep = 1000000;

        do {
            x = pointMin.x;
            yStep = 1000000;
            bestRemY = 0;

            do {
                list = reinterpret_cast< u16* >(searchBlock(&shift, x, y, z));
                shift = 1 << shift;
                s32 blockSize = shift;
                s32 remZ;
                s32 mask = blockSize - 1;
                remX = blockSize - (x & mask);
                s32 remY = blockSize - (y & mask);
                remZ = blockSize - (z & mask);

                if (remZ < zStep) {
                    zStep = remZ;
                }

                if (remY < yStep) {
                    yStep = remY;
                }

                if (remY > bestRemY && list[1] != 0) {
                    bestRemY = remY;
                    nextSkipList = list;
                }

                if (skipList == nullptr || list != skipList) {
                    while (*++list != 0) {
                        if (memo && seen.testAndPut(*list)) {
                            portPassed++;
                            continue;
                        }

                        portTested++;
                        prism = &mFile->mPrisms[*list];

                        if (prism->mHeight <= 0.0f) {
                            continue;
                        }

                        KC_PrismData** end = pOut + foundCount;

                        // (GXV_KCL_MEMO: a prism the table never held is not among the results; see KclSeen)
                        if ((!memo || seen.mPushedOut) && std::find(pOut, end, (KC_PrismData* const&)prism) != end) {
                            continue;
                        }

                        TVec3f verts[3];
                        verts[0] = getPos(prism, 0);
                        verts[1] = getPos(prism, 1);
                        verts[2] = getPos(prism, 2);

                        TVec3f prismMin;
                        TVec3f prismMax;
                        MR::createBoundingBox(verts, 3, &prismMin, &prismMax);

                        if (prismMax.x < queryMin.x) {
                            continue;
                        }

                        if (prismMax.y < queryMin.y) {
                            continue;
                        }

                        if (prismMax.z < queryMin.z) {
                            continue;
                        }

                        if (queryMax.x < prismMin.x) {
                            continue;
                        }

                        if (queryMax.y < prismMin.y) {
                            continue;
                        }

                        if (queryMax.z < prismMin.z) {
                            continue;
                        }

                        *end = prism;
                        foundCount++;

                        if (foundCount == maxCount) {
                            KCL_COUNT(portTested, portPassed);
                            return maxCount;
                        }
                    }
                }

                x += remX;
            } while (x <= pointMax.x);

            skipList = nextSkipList;
            y += yStep;
        } while (y <= pointMax.y);

        z += zStep;
    } while (z <= pointMax.z);

    KCL_COUNT(portTested, portPassed);
    return foundCount;
}

u32 KCollisionServer::checkSphereWalk(Fxyz* pCenter, f32 radius, f32 param, u32 maxCount, KC_PrismData** pOut, f32* pDists, u8* pFlags,
                                       bool memo) {
    KC_PrismData** end;
    s32 remX;
    s32 yStep;
    s32 zStep;
    s32 bestRemY;
    u16* list;
    u16* nextSkipList = nullptr;
    u16* skipList = nullptr;
    u32 foundCount = 0;

    V3u pointMin;
    V3u pointMax;
    TVec3f boxMax;
    TVec3f boxMin;
    u32 x;
    u32 y;
    u32 z;
    KC_PrismData* prism;
    f32 dist = 0.0f;
    s32 shift;
    u8 flag;
    boxMax.x = pCenter->x + radius;
    boxMax.y = pCenter->y + radius;
    boxMax.z = pCenter->z + radius;

    boxMin.x = pCenter->x - radius;
    boxMin.y = pCenter->y - radius;
    boxMin.z = pCenter->z - radius;

    if (!outCheck(&boxMin, &boxMax, &pointMin, &pointMax)) {
        return 0;
    }

    // GXV_KCL_MEMO (top of file): the prisms this walk has looked at (memo: the caller's choice, see the wrappers)
    KclSeen seen;
    u32 portTested = 0, portPassed = 0;
    if (memo) {
        seen.clear();
    }

    z = pointMin.z;

    do {
        y = pointMin.y;
        zStep = 1000000;

        do {
            x = pointMin.x;
            yStep = 1000000;
            bestRemY = 0;

            do {
                list = reinterpret_cast< u16* >(searchBlock(&shift, x, y, z));
                s32 remZ;
                s32 blockSize = 1 << shift;
                s32 mask = blockSize - 1;
                remX = blockSize - (x & mask);
                s32 remY = blockSize - (y & mask);
                remZ = blockSize - (z & mask);

                if (remZ < zStep) {
                    zStep = remZ;
                }

                if (remY < yStep) {
                    yStep = remY;
                }

                if (remY > bestRemY && list[1] != 0) {
                    bestRemY = remY;
                    nextSkipList = list;
                }

                if (skipList == nullptr || list != skipList) {
                    while (*++list != 0) {
                        if (memo && seen.testAndPut(*list)) {
                            portPassed++;
                            continue;
                        }

                        portTested++;
                        prism = &mFile->mPrisms[*list];

                        if (prism->mHeight <= 0.0f) {
                            continue;
                        }

                        end = pOut + foundCount;

                        // (GXV_KCL_MEMO: a prism the table never held is not among the results; see KclSeen)
                        if ((!memo || seen.mPushedOut) && std::find(pOut, end, (KC_PrismData* const&)prism) != end) {
                            continue;
                        }

                        if (!KCHitSphere(prism, pCenter, radius, param, &dist, &flag)) {
                            continue;
                        }

                        if (foundCount >= maxCount) {
                            continue;
                        }

                        // (GXV_KCL_MEMO: the same search as the one above, over the same results; it can only fail again)
                        if (!memo && std::find(pOut, end, (KC_PrismData* const&)prism) != end) {
                            continue;
                        }

                        *end = prism;
                        pDists[foundCount] = dist;
                        pFlags[foundCount] = flag;
                        foundCount++;
                    }
                }

                x += remX;
            } while (x <= pointMax.x);

            skipList = nextSkipList;
            y += yStep;
        } while (y <= pointMax.y);

        z += zStep;
    } while (z <= pointMax.z);

    KCL_COUNT(portTested, portPassed);
    return foundCount;
}

u32 KCollisionServer::checkSphereWithThicknessWalk(Fxyz* pCenter, f32 radius, f32 param, u32 maxCount, KC_PrismData** pOut, f32* pDists,
                                                   u8* pFlags, f32 thickness, bool memo) {
    KC_PrismData** end;
    s32 remX;
    s32 yStep;
    s32 zStep;
    s32 bestRemY;
    u16* list;
    u16* nextSkipList = nullptr;
    u16* skipList = nullptr;
    u32 foundCount = 0;

    V3u pointMin;
    V3u pointMax;
    TVec3f boxMax;
    TVec3f boxMin;
    u32 x;
    u32 y;
    u32 z;
    KC_PrismData* prism;
    f32 dist = 0.0f;
    s32 shift;
    u8 flag;
    boxMax.x = pCenter->x + radius;
    boxMax.y = pCenter->y + radius;
    boxMax.z = pCenter->z + radius;

    boxMin.x = pCenter->x - radius;
    boxMin.y = pCenter->y - radius;
    boxMin.z = pCenter->z - radius;

    if (!outCheck(&boxMin, &boxMax, &pointMin, &pointMax)) {
        return 0;
    }

    // GXV_KCL_MEMO (top of file): the prisms this walk has looked at (memo: the caller's choice, see the wrappers)
    KclSeen seen;
    u32 portTested = 0, portPassed = 0;
    if (memo) {
        seen.clear();
    }

    z = pointMin.z;

    do {
        y = pointMin.y;
        zStep = 1000000;

        do {
            x = pointMin.x;
            yStep = 1000000;
            bestRemY = 0;

            do {
                list = reinterpret_cast< u16* >(searchBlock(&shift, x, y, z));
                s32 remZ;
                s32 blockSize = 1 << shift;
                s32 mask = blockSize - 1;
                remX = blockSize - (x & mask);
                s32 remY = blockSize - (y & mask);
                remZ = blockSize - (z & mask);

                if (remZ < zStep) {
                    zStep = remZ;
                }

                if (remY < yStep) {
                    yStep = remY;
                }

                if (remY > bestRemY && list[1] != 0) {
                    bestRemY = remY;
                    nextSkipList = list;
                }

                if (skipList == nullptr || list != skipList) {
                    while (*++list != 0) {
                        if (memo && seen.testAndPut(*list)) {
                            portPassed++;
                            continue;
                        }

                        portTested++;
                        prism = &mFile->mPrisms[*list];

                        if (prism->mHeight <= 0.0f) {
                            continue;
                        }

                        end = pOut + foundCount;

                        // (GXV_KCL_MEMO: a prism the table never held is not among the results; see KclSeen)
                        if ((!memo || seen.mPushedOut) && std::find(pOut, end, (KC_PrismData* const&)prism) != end) {
                            continue;
                        }

                        if (!KCHitSphereWithThickness(prism, pCenter, radius, param, &dist, &flag, thickness)) {
                            continue;
                        }

                        if (foundCount >= maxCount) {
                            continue;
                        }

                        // (GXV_KCL_MEMO: the same search as the one above, over the same results; it can only fail again)
                        if (!memo && std::find(pOut, end, (KC_PrismData* const&)prism) != end) {
                            continue;
                        }

                        *end = prism;
                        pDists[foundCount] = dist;
                        pFlags[foundCount] = flag;
                        foundCount++;
                    }
                }

                x += remX;
            } while (x <= pointMax.x);

            skipList = nextSkipList;
            y += yStep;
        } while (y <= pointMax.y);

        z += zStep;
    } while (z <= pointMax.z);

    KCL_COUNT(portTested, portPassed);
    return foundCount;
}

KC_PrismData* KCollisionServer::checkArrowWalk(const TVec3f& rOrigin, const TVec3f& rDir, f32* pDists, u8* pFlags, u32* pCount,
                                               KC_PrismData** pOut, u32 maxCount, bool memo, bool nearestOnly) const {
    if (rDir.x == 0.0f && rDir.y == 0.0f && rDir.z == 0.0f) {
        return nullptr;
    }

    f32 length;
    TVec3f dir(rDir);
    f32 startT = 0.0f;
    MR::separateScalarAndDirection(&length, &dir, dir);

    if (MR::isNearZero(dir)) {
        return nullptr;
    }

    TVec3f hitPoint;
    V3u cell;
    TVec3f start(rOrigin);
    start.x -= mFile->mMin.x;
    start.y -= mFile->mMin.y;
    start.z -= mFile->mMin.z;

    cell.setUsingCast(start);

    if (isInsideMinMaxInLocalSpace(cell)) {
        hitPoint.set(start);
    } else {
        TVec3f boxMax;
        boxMax.x = (f32)(u32)~mFile->mXMask;
        boxMax.y = (f32)(u32)~mFile->mYMask;
        boxMax.z = (f32)(u32)~mFile->mZMask;

        if (dir.x != 0.0f) {
            f32 edge = 0.0f < dir.x ? 0.0f : boxMax.x;
            startT = (edge - start.x) / dir.x;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        if (dir.y != 0.0f) {
            f32 edge = 0.0f < dir.y ? 0.0f : boxMax.y;
            startT = (edge - start.y) / dir.y;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        if (dir.z != 0.0f) {
            f32 edge = 0.0f < dir.z ? 0.0f : boxMax.z;
            startT = (edge - start.z) / dir.z;

            if (0.0f <= startT && startT <= length) {
                TVec3f step(dir);
                step.scale(startT);
                hitPoint.set(step);
                hitPoint += start;
                cell.setUsingCast(hitPoint);
                if (isInsideMinMaxInLocalSpace(cell)) {
                    goto searchStart;
                }
            }
        }

        return nullptr;
    }

searchStart:
    u16* prismList;
    u32 foundCount;
    foundCount = 0;

    s32 shift;
    s32 deltaPosX;
    s32 deltaPosY;
    s32 deltaPosZ;
    s32 deltaNegX;
    s32 deltaNegY;
    s32 deltaNegZ;
    s32* pDeltaX;
    s32* pDeltaY;
    s32* pDeltaZ;
    s32 stepX;
    s32 stepY;
    s32 stepZ;
    if (dir.x < 0.0f) {
        pDeltaX = &deltaNegX;
        stepX = -1;
    } else {
        pDeltaX = &deltaPosX;
        stepX = 1;
    }

    if (dir.y < 0.0f) {
        pDeltaY = &deltaNegY;
        stepY = -1;
    } else {
        pDeltaY = &deltaPosY;
        stepY = 1;
    }

    if (dir.z < 0.0f) {
        pDeltaZ = &deltaNegZ;
        stepZ = -1;
    } else {
        pDeltaZ = &deltaPosZ;
        stepZ = 1;
    }

    KC_PrismData* bestPrism;
    f32 accumT = startT;

    bestPrism = nullptr;

    // GXV_KCL_MEMO (top of file): the prisms this walk has seen MISS (a prism that hit is tested and reported again in each cell)
    KclSeen missed;
    u32 portTested = 0, portPassed = 0;
    if (memo) {
        missed.clear();
    }
    // PETARI_RAY_NEAREST (forkB; ported from Producdevity's fork, 1807939): the nearest hit found so far, as a fraction of rDir
    f32 nearestHit = 1.0f;

    do {
        prismList = reinterpret_cast< u16* >(searchBlock(&shift, cell.x, cell.y, cell.z));
        u32 blockSize = 1 << shift;
        u32 mask = blockSize - 1;

        deltaPosX = blockSize - (cell.x & mask);
        deltaPosY = blockSize - (cell.y & mask);
        deltaPosZ = blockSize - (cell.z & mask);
        deltaNegX = -(s32)(cell.x & mask);
        deltaNegY = -(s32)(cell.y & mask);
        deltaNegZ = -(s32)(cell.z & mask);

        if (*pDeltaX == 0) {
            *pDeltaX = stepX;
        }

        if (*pDeltaY == 0) {
            *pDeltaY = stepY;
        }

        if (*pDeltaZ == 0) {
            *pDeltaZ = stepZ;
        }

        f32 bestFraction = 1.0f;
        f32 dist = 1.0f;

        while (*++prismList != 0) {
            if (memo && missed.has(*prismList)) {
                portPassed++;
                continue;
            }

            portTested++;
            KC_PrismData* prism = &mFile->mPrisms[*prismList];

            if (prism->mHeight <= 0.0f) {
                continue;
            }

            u8 flag = 0;

            if (!KCHitArrow(prism, rOrigin, rDir, &dist, &flag)) {
                if (memo) {
                    missed.put(*prismList);
                }
                continue;
            }

            if (pOut != nullptr) {
                pDists[foundCount] = dist;
                pOut[foundCount] = prism;
                foundCount++;
                if (dist < nearestHit) {
                    nearestHit = dist;
                }

                if (dist < bestFraction) {
                    bestFraction = dist;
                    bestPrism = prism;
                }

                if (foundCount == maxCount) {
                    if (pCount != nullptr) {
                        *pCount = foundCount;
                    }

                    KCL_COUNT(portTested, portPassed);
                    return bestPrism;
                }
            } else {
                if (!(dist < bestFraction)) {
                    continue;
                }

                bestFraction = dist;
                *pDists = dist;
                bestPrism = prism;
                *pFlags = flag;
            }
        }

        if (pOut == nullptr && bestPrism != nullptr) {
            break;
        }

        f32 tX = MR::isNearZero(dir.x) ? 1.0e9f : (f32)*pDeltaX / dir.x;
        f32 tY = MR::isNearZero(dir.y) ? 1.0e9f : (f32)*pDeltaY / dir.y;
        f32 tZ = MR::isNearZero(dir.z) ? 1.0e9f : (f32)*pDeltaZ / dir.z;

        f32 tMin = tX;

        if (tY < tMin) {
            tMin = tY;
        }

        if (tZ < tMin) {
            tMin = tZ;
        }

        if (length - accumT <= tMin) {
            break;
        }

        TVec3f step(dir);
        step.scale(tMin);
        hitPoint += step;
        accumT += tMin;
        // PETARI_RAY_NEAREST: for a caller that keeps only the nearest hit (MapUtil's first polygon on a line, no triangle filter),
        // a cell that starts beyond the nearest hit found holds no nearer one: a prism with a nearer hit has that hit point in a
        // cell the walk has already passed, and was tested there. One unit (a cell is at least one) of margin for the rounding.
        if (nearestOnly && foundCount != 0 && nearestHit * length + 1.0f < accumT) {
            break;
        }

        cell.setUsingCast(hitPoint);

        if (!isInsideMinMaxInLocalSpace(cell)) {
            break;
        }
    } while (accumT < length);

    if (pCount != nullptr) {
        *pCount = foundCount;
    }

    KCL_COUNT(portTested, portPassed);
    return bestPrism;
}

#ifdef TARGET_PC
// GXV_KCL_MEMO=2 (verify): scratch for the memo walk's answer, and the comparison. Used by one query at a time (sKvBusy): a query
// that came in while another was being verified (another thread, or a re-entry, which the walks never make) and a query asking for
// more than cKvMax results are answered by the original walk alone and counted as "not checked".
namespace {
    const u32 cKvMax = 512;
    KC_PrismData* sKvOut[cKvMax];
    f32 sKvDists[cKvMax];
    u8 sKvFlags[cKvMax];
    bool sKvBusy = false;
    u32 sKvChecks = 0, sKvMismatches = 0, sKvUnchecked = 0, sKvLogged = 0;

    bool kvSameBits(const void* a, const void* b, u32 n) {
        return n == 0 || std::memcmp(a, b, n) == 0;
    }
    void kvPeriodic() {
        if ((++sKvChecks & 0xFFFF) == 0) {
            port_log_summary("kcl-verify: %u checks, %u mismatches, %u not checked since the start (GXV_KCL_MEMO=2; the game used the original walk)",
                             sKvChecks, sKvMismatches, sKvUnchecked);
        }
    }
    // one mismatch: counted; the first 8 written out in full. q: the query's floats in the KCL's own (local) space.
    void kvMismatch(const KCollisionServer* pSrv, const char* pKind, const char* pWhat, u32 nOrig, u32 nMemo, u32 at, const f32* q, int nq,
                    u32 maxCount) {
        sKvMismatches++;
        if (sKvLogged >= 8) {
            return;
        }
        sKvLogged++;
        const KCLFile* f = pSrv->mFile;
        char qs[160];
        int len = 0;
        for (int i = 0; i < nq && len < (int)sizeof(qs) - 20; i++) {
            len += snprintf(qs + len, sizeof(qs) - len, "%s%a", i ? "," : "", (double)q[i]);
        }
        port_log_summary("kcl-verify: MISMATCH #%u %s (%s at %u): original %u results, memo %u; KCL %p, %d prisms, min (%a,%a,%a), masks %x/%x/%x "
                         "shift %d; query (%s) max %u; check #%u",
                         sKvMismatches, pKind, pWhat, at, nOrig, nMemo, (const void*)f, (int)pSrv->getTriangleNum(), (double)f->mMin.x,
                         (double)f->mMin.y, (double)f->mMin.z, (unsigned)f->mXMask, (unsigned)f->mYMask, (unsigned)f->mZMask,
                         (int)f->mBlockWidthShift, qs, maxCount, sKvChecks);
    }
    // the hit lists: count, then prisms in order, then distances (bits), then flags (pFlagsOrig null: not compared)
    bool kvCompareLists(const KCollisionServer* pSrv, const char* pKind, u32 nOrig, u32 nMemo, KC_PrismData* const* pOutOrig, const f32* pDistsOrig,
                        const u8* pFlagsOrig, const f32* q, int nq, u32 maxCount) {
        if (nOrig != nMemo) {
            kvMismatch(pSrv, pKind, "count", nOrig, nMemo, 0, q, nq, maxCount);
            return false;
        }
        for (u32 i = 0; i < nOrig; i++) {
            if (pOutOrig[i] != sKvOut[i]) {
                kvMismatch(pSrv, pKind, "prism", nOrig, nMemo, i, q, nq, maxCount);
                return false;
            }
            if (pDistsOrig != nullptr && !kvSameBits(&pDistsOrig[i], &sKvDists[i], sizeof(f32))) {
                kvMismatch(pSrv, pKind, "distance", nOrig, nMemo, i, q, nq, maxCount);
                return false;
            }
            if (pFlagsOrig != nullptr && pFlagsOrig[i] != sKvFlags[i]) {
                kvMismatch(pSrv, pKind, "flag", nOrig, nMemo, i, q, nq, maxCount);
                return false;
            }
        }
        return true;
    }
}  // namespace
#endif

// GXV_KCL_MEMO: the public queries. 0 / 1: the walk with the memo off / on. 2: both, compared; the caller gets the original's answer.
u32 KCollisionServer::checkArea3D(Fxyz* pMin, Fxyz* pMax, KC_PrismData** pOut, u32 maxCount) {
#ifdef TARGET_PC
    const int mode = kclMemoMode();
    if (mode != 2) {
        return checkArea3DWalk(pMin, pMax, pOut, maxCount, mode == 1);
    }
    if (sKvBusy || maxCount > cKvMax) {
        sKvUnchecked++;
        return checkArea3DWalk(pMin, pMax, pOut, maxCount, false);
    }
    sKvBusy = true;
    const f32 q[6] = {pMin->x, pMin->y, pMin->z, pMax->x, pMax->y, pMax->z};
    const u32 nMemo = checkArea3DWalk(pMin, pMax, sKvOut, maxCount, true);
    const u32 n = checkArea3DWalk(pMin, pMax, pOut, maxCount, false);
    kvCompareLists(this, "area", n, nMemo, pOut, nullptr, nullptr, q, 6, maxCount);
    kvPeriodic();
    sKvBusy = false;
    return n;
#else
    return checkArea3DWalk(pMin, pMax, pOut, maxCount, false);
#endif
}

u32 KCollisionServer::checkSphere(Fxyz* pCenter, f32 radius, f32 param, u32 maxCount, KC_PrismData** pOut, f32* pDists, u8* pFlags) {
#ifdef TARGET_PC
    const int mode = kclMemoMode();
    if (mode != 2) {
        return checkSphereWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, mode == 1);
    }
    if (sKvBusy || maxCount > cKvMax) {
        sKvUnchecked++;
        return checkSphereWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, false);
    }
    sKvBusy = true;
    const f32 q[5] = {pCenter->x, pCenter->y, pCenter->z, radius, param};
    const u32 nMemo = checkSphereWalk(pCenter, radius, param, maxCount, sKvOut, sKvDists, sKvFlags, true);
    const u32 n = checkSphereWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, false);
    kvCompareLists(this, "sphere", n, nMemo, pOut, pDists, pFlags, q, 5, maxCount);
    kvPeriodic();
    sKvBusy = false;
    return n;
#else
    return checkSphereWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, false);
#endif
}

u32 KCollisionServer::checkSphereWithThickness(Fxyz* pCenter, f32 radius, f32 param, u32 maxCount, KC_PrismData** pOut, f32* pDists, u8* pFlags,
                                               f32 thickness) {
#ifdef TARGET_PC
    const int mode = kclMemoMode();
    if (mode != 2) {
        return checkSphereWithThicknessWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, thickness, mode == 1);
    }
    if (sKvBusy || maxCount > cKvMax) {
        sKvUnchecked++;
        return checkSphereWithThicknessWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, thickness, false);
    }
    sKvBusy = true;
    const f32 q[6] = {pCenter->x, pCenter->y, pCenter->z, radius, param, thickness};
    const u32 nMemo = checkSphereWithThicknessWalk(pCenter, radius, param, maxCount, sKvOut, sKvDists, sKvFlags, thickness, true);
    const u32 n = checkSphereWithThicknessWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, thickness, false);
    kvCompareLists(this, "sphere-thick", n, nMemo, pOut, pDists, pFlags, q, 6, maxCount);
    kvPeriodic();
    sKvBusy = false;
    return n;
#else
    return checkSphereWithThicknessWalk(pCenter, radius, param, maxCount, pOut, pDists, pFlags, thickness, false);
#endif
}

#ifdef TARGET_PC
KC_PrismData* KCollisionServer::checkArrow(const TVec3f& rOrigin, const TVec3f& rDir, f32* pDists, u8* pFlags, u32* pCount, KC_PrismData** pOut,
                                           u32 maxCount, bool nearestOnly) const {
#else
KC_PrismData* KCollisionServer::checkArrow(const TVec3f& rOrigin, const TVec3f& rDir, f32* pDists, u8* pFlags, u32* pCount, KC_PrismData** pOut,
                                           u32 maxCount) const {
#endif
#ifdef TARGET_PC
    const int mode = kclMemoMode();
    if (mode != 2) {
        return checkArrowWalk(rOrigin, rDir, pDists, pFlags, pCount, pOut, maxCount, mode == 1, nearestOnly);
    }
    if (sKvBusy || (pOut != nullptr && maxCount > cKvMax)) {
        sKvUnchecked++;
        return checkArrowWalk(rOrigin, rDir, pDists, pFlags, pCount, pOut, maxCount, false, nearestOnly);
    }
    sKvBusy = true;
    const f32 q[6] = {rOrigin.x, rOrigin.y, rOrigin.z, rDir.x, rDir.y, rDir.z};
    // The count is written only on some paths: both runs start from the caller's value. A caller with no count gets none written
    // (the original's goes to a local).
    u32 countOrig = pCount != nullptr ? *pCount : 0;
    u32 countMemo = countOrig;
    KC_PrismData* best;
    if (pOut != nullptr) {
        // collect mode: every hit's prism and distance (flags are not written in this mode, by either walk)
        KC_PrismData* bestMemo = checkArrowWalk(rOrigin, rDir, sKvDists, sKvFlags, &countMemo, sKvOut, maxCount, true, nearestOnly);
        best = checkArrowWalk(rOrigin, rDir, pDists, pFlags, pCount != nullptr ? pCount : &countOrig, pOut, maxCount, false, nearestOnly);
        if (pCount != nullptr) {
            countOrig = *pCount;
        }
        if (kvCompareLists(this, "line-collect", countOrig, countMemo, pOut, pDists, nullptr, q, 6, maxCount) && best != bestMemo) {
            kvMismatch(this, "line-collect", "best", countOrig, countMemo, 0, q, 6, maxCount);
        }
    } else {
        // first-hit mode: the best prism, its distance and flag (written only on a hit: both start from the caller's values)
        // (the game's only caller, CollisionParts::checkStrikeLine, uses collect mode; this one is for completeness)
        f32 distMemo = 0.0f;
        u8 flagMemo = pFlags != nullptr ? *pFlags : 0;
        if (pDists != nullptr) {
            std::memcpy(&distMemo, pDists, sizeof(f32));
        }
        KC_PrismData* bestMemo = checkArrowWalk(rOrigin, rDir, &distMemo, &flagMemo, &countMemo, nullptr, maxCount, true, nearestOnly);
        best = checkArrowWalk(rOrigin, rDir, pDists, pFlags, pCount != nullptr ? pCount : &countOrig, nullptr, maxCount, false, nearestOnly);
        if (pCount != nullptr) {
            countOrig = *pCount;
        }
        if (best != bestMemo) {
            kvMismatch(this, "line-first", "best", countOrig, countMemo, 0, q, 6, maxCount);
        } else if (pDists != nullptr && !kvSameBits(pDists, &distMemo, sizeof(f32))) {
            kvMismatch(this, "line-first", "distance", countOrig, countMemo, 0, q, 6, maxCount);
        } else if (pFlags != nullptr && *pFlags != flagMemo) {
            kvMismatch(this, "line-first", "flag", countOrig, countMemo, 0, q, 6, maxCount);
        } else if (countOrig != countMemo) {
            kvMismatch(this, "line-first", "count", countOrig, countMemo, 0, q, 6, maxCount);
        }
    }
    kvPeriodic();
    sKvBusy = false;
    return best;
#else
    return checkArrowWalk(rOrigin, rDir, pDists, pFlags, pCount, pOut, maxCount, false);
#endif
}

bool KCollisionServer::KCHitSphere(KC_PrismData* pPrism, Fxyz* pCenter, f32 radius, f32 param, f32* pDist, u8* pFlag) {
    f32 radiusSq = radius * radius;
    f32 threshold = mFile->mThickness * param;
    *pFlag = 0;

    TVec3f* v0 = &mFile->mPos[pPrism->mPositionIndex];

    Fxyz dir;
    f32 distances[4];
    dir.x = pCenter->x - v0->x;
    dir.y = pCenter->y - v0->y;
    dir.z = pCenter->z - v0->z;

    TVec3f* n0 = &mFile->mNorms[pPrism->mEdgeIndices[0]];
    distances[1] = dir.x * n0->x + dir.y * n0->y + dir.z * n0->z;

    if (distances[1] >= radius) {
        return false;
    }

    TVec3f* n1 = &mFile->mNorms[pPrism->mEdgeIndices[1]];
    distances[2] = dir.x * n1->x + dir.y * n1->y + dir.z * n1->z;

    if (distances[2] >= radius) {
        return false;
    }

    TVec3f* n2 = &mFile->mNorms[pPrism->mEdgeIndices[2]];
    distances[3] = dir.x * n2->x + dir.y * n2->y + dir.z * n2->z - pPrism->mHeight;

    if (distances[3] >= radius) {
        return false;
    }

    TVec3f* faceNormal = &mFile->mNorms[pPrism->mNormalIndex];
    distances[0] = dir.x * faceNormal->x + dir.y * faceNormal->y + dir.z * faceNormal->z;
    *pDist = radius - distances[0];

    if (*pDist < 0.0f) {
        return false;
    }

    f32 nn;

    if (distances[1] > distances[2]) {
        if (!(distances[1] > distances[3])) {
            goto side2;
        }
        goto side0;
    } else {
        if (!(distances[2] > distances[3])) {
            goto side2;
        }
        goto side1;
    }

side0:
    if (distances[1] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[2] > distances[3]) {
        nn = n0->x * n1->x + n0->y * n1->y + n0->z * n1->z;

        if (nn * distances[1] > distances[2]) {
            goto vertex2;
        } else {
            goto region5;
        }
    }

    nn = n0->x * n2->x + n0->y * n2->y + n0->z * n2->z;

    if (nn * distances[1] > distances[3]) {
        goto vertex2;
    } else {
        goto region7;
    }

side1:
    if (distances[2] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[3] > distances[1]) {
        nn = n1->x * n2->x + n1->y * n2->y + n1->z * n2->z;

        if (nn * distances[2] > distances[3]) {
            goto vertex3;
        } else {
            goto region6;
        }
    }

    nn = n1->x * n0->x + n1->y * n0->y + n1->z * n0->z;

    if (nn * distances[2] > distances[1]) {
        goto vertex3;
    } else {
        goto region5;
    }

side2:

    if (distances[3] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[1] > distances[2]) {
        nn = n2->x * n0->x + n2->y * n0->y + n2->z * n0->z;

        if (nn * distances[3] > distances[1]) {
            goto vertex4;
        } else {
            goto region7;
        }
    }

    nn = n2->x * n1->x + n2->y * n1->y + n2->z * n1->z;

    if (nn * distances[3] > distances[2]) {
        goto vertex4;
    } else {
        goto region6;
    }

vertex2:
    if (distances[1] > distances[0]) {
        return false;
    }

    *pDist = radiusSq - distances[1] * distances[1];
    *pFlag = 2;
    goto finish;

vertex3:
    if (distances[2] > distances[0]) {
        return false;
    }

    *pDist = radiusSq - distances[2] * distances[2];
    *pFlag = 3;
    goto finish;

vertex4:
    if (distances[3] > distances[0]) {
        return false;
    }

    *pDist = radiusSq - distances[3] * distances[3];
    *pFlag = 4;
    goto finish;

region5: {
    f32 t = (nn * distances[2] - distances[1]) / (nn * nn - 1.0f);
    f32 s = distances[2] - t * nn;
    dir.x = t * n0->x + s * n1->x;
    dir.y = t * n0->y + s * n1->y;
    dir.z = t * n0->z + s * n1->z;
    *pFlag = 5;
    goto edgeFinish;
}

region6: {
    f32 t = (nn * distances[3] - distances[2]) / (nn * nn - 1.0f);
    f32 s = distances[3] - t * nn;
    dir.x = t * n1->x + s * n2->x;
    dir.y = t * n1->y + s * n2->y;
    dir.z = t * n1->z + s * n2->z;
    *pFlag = 6;
    goto edgeFinish;
}

region7: {
    f32 t = (nn * distances[1] - distances[3]) / (nn * nn - 1.0f);
    f32 s = distances[1] - t * nn;
    dir.x = t * n2->x + s * n0->x;
    dir.y = t * n2->y + s * n0->y;
    dir.z = t * n2->z + s * n0->z;
    *pFlag = 7;
}

edgeFinish: {
    f32 closestSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    f32 dist = MR::sqrt(closestSq);

    if (dist > distances[0] || dist >= radius) {
        *pFlag = 0;
        return false;
    }

    *pDist = radiusSq - closestSq;
}

finish:
    *pDist = MR::sqrt(*pDist) - distances[0];

    if (*pDist < 0.0f || threshold < *pDist) {
        *pFlag = 0;
        return false;
    }

success:
    return true;
}

bool KCollisionServer::KCHitSphereWithThickness(KC_PrismData* pPrism, Fxyz* pCenter, f32 radius, f32 param, f32* pDist, u8* pFlag, f32 thickness) {
    f32 radiusSq = radius * radius;
    f32 threshold = thickness * param;
    *pFlag = 0;

    TVec3f* v0 = &mFile->mPos[pPrism->mPositionIndex];

    Fxyz dir;
    f32 distances[4];
    dir.x = pCenter->x - v0->x;
    dir.y = pCenter->y - v0->y;
    dir.z = pCenter->z - v0->z;

    TVec3f* n0 = &mFile->mNorms[pPrism->mEdgeIndices[0]];
    distances[1] = dir.x * n0->x + dir.y * n0->y + dir.z * n0->z;

    if (distances[1] >= radius) {
        return false;
    }

    TVec3f* n1 = &mFile->mNorms[pPrism->mEdgeIndices[1]];
    distances[2] = dir.x * n1->x + dir.y * n1->y + dir.z * n1->z;

    if (distances[2] >= radius) {
        return false;
    }

    TVec3f* n2 = &mFile->mNorms[pPrism->mEdgeIndices[2]];
    distances[3] = dir.x * n2->x + dir.y * n2->y + dir.z * n2->z - pPrism->mHeight;

    if (distances[3] >= radius) {
        return false;
    }

    TVec3f* faceNormal = &mFile->mNorms[pPrism->mNormalIndex];
    distances[0] = dir.x * faceNormal->x + dir.y * faceNormal->y + dir.z * faceNormal->z;
    *pDist = radius - distances[0];

    if (*pDist < 0.0f) {
        return false;
    }

    f32 nn;

    if (distances[1] > distances[2]) {
        if (!(distances[1] > distances[3])) {
            goto side2;
        }
        goto side0;
    } else {
        if (!(distances[2] > distances[3])) {
            goto side2;
        }
        goto side1;
    }

side0:
    if (distances[1] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[2] > distances[3]) {
        nn = n0->x * n1->x + n0->y * n1->y + n0->z * n1->z;

        if (!(nn * distances[1] > distances[2])) {
            goto region5;
        }

        goto vertex2;
    }

    nn = n0->x * n2->x + n0->y * n2->y + n0->z * n2->z;

    if (!(nn * distances[1] > distances[3])) {
        goto region7;
    }

    goto vertex2;

side1:
    if (distances[2] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[3] > distances[1]) {
        nn = n1->x * n2->x + n1->y * n2->y + n1->z * n2->z;

        if (!(nn * distances[2] > distances[3])) {
            goto region6;
        }

        goto vertex3;
    }

    nn = n1->x * n0->x + n1->y * n0->y + n1->z * n0->z;

    if (!(nn * distances[2] > distances[1])) {
        goto region5;
    }

    goto vertex3;

side2:

    if (distances[3] <= 0.0f) {
        if (threshold < *pDist) {
            return false;
        }

        *pFlag = 1;
        goto success;
    }

    if (distances[1] > distances[2]) {
        nn = n2->x * n0->x + n2->y * n0->y + n2->z * n0->z;

        if (!(nn * distances[3] > distances[1])) {
            goto region7;
        }

        goto vertex4;
    }

    nn = n2->x * n1->x + n2->y * n1->y + n2->z * n1->z;

    if (!(nn * distances[3] > distances[2])) {
        goto region6;
    }

    goto vertex4;

vertex2:
    *pDist = radiusSq - distances[1] * distances[1];
    *pFlag = 2;
    goto finish;

vertex3:
    *pDist = radiusSq - distances[2] * distances[2];
    *pFlag = 3;
    goto finish;

vertex4:
    *pDist = radiusSq - distances[3] * distances[3];
    *pFlag = 4;
    goto finish;

region5: {
    f32 t = (nn * distances[2] - distances[1]) / (nn * nn - 1.0f);
    f32 s = distances[2] - t * nn;
    dir.x = t * n0->x + s * n1->x;
    dir.y = t * n0->y + s * n1->y;
    dir.z = t * n0->z + s * n1->z;
    *pFlag = 5;
    goto edgeFinish;
}

region6: {
    f32 t = (nn * distances[3] - distances[2]) / (nn * nn - 1.0f);
    f32 s = distances[3] - t * nn;
    dir.x = t * n1->x + s * n2->x;
    dir.y = t * n1->y + s * n2->y;
    dir.z = t * n1->z + s * n2->z;
    *pFlag = 6;
    goto edgeFinish;
}

region7: {
    f32 t = (nn * distances[1] - distances[3]) / (nn * nn - 1.0f);
    f32 s = distances[1] - t * nn;
    dir.x = t * n2->x + s * n0->x;
    dir.y = t * n2->y + s * n0->y;
    dir.z = t * n2->z + s * n0->z;
    *pFlag = 7;
}

edgeFinish: {
    f32 closestSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    f32 dist = MR::sqrt(closestSq);

    if (dist >= radius) {
        *pFlag = 0;
        return false;
    }

    *pDist = radiusSq - closestSq;
}

finish:
    f32 distance;
    distance = MR::sqrt(*pDist);

    if (distances[0] + distance < 0.0f) {
        *pFlag = 0;
        return false;
    }

    *pDist = distance - distances[0];

    if (*pDist < 0.0f || threshold < *pDist) {
        *pFlag = 0;
        return false;
    }

success:
    return true;
}

bool KCollisionServer::isNearParallelNormal(const KC_PrismData* pPrism) const {
    TVec3f edge0 = mFile->mNorms[pPrism->mEdgeIndices[0]];
    TVec3f edge1 = mFile->mNorms[pPrism->mEdgeIndices[1]];
    TVec3f edge2 = mFile->mNorms[pPrism->mEdgeIndices[2]];

    bool isNear = false;

    if (MR::isSameDirection(edge0, edge1) || MR::isSameDirection(edge0, edge2) || MR::isSameDirection(edge1, edge2)) {
        isNear = true;
    }

    return isNear;
}

bool KCollisionServer::KCHitArrow(KC_PrismData* pPrism, const TVec3f& rOrigin, const TVec3f& rDir, f32* pDist, u8* pFlag) const {
    TVec3f* v0 = &mFile->mPos[pPrism->mPositionIndex];
    TVec3f* faceNormal = &mFile->mNorms[pPrism->mNormalIndex];

    TVec3f rel;
    PSVECSubtract(rOrigin, v0, rel);

    f32 t = PSVECDotProduct(rel, faceNormal);

    if (t <= 0.0f) {
        *pFlag = 0;
        return false;
    }

    f32 dirDotFace = PSVECDotProduct(faceNormal, rDir);

    if (0.0f < t + dirDotFace) {
        *pFlag = 0;
        return false;
    }

    t = t / -dirDotFace;

    TVec3f hit = rDir * t;
    hit += rel;

    bool onEdge0 = false;
    bool onEdge1 = false;
    bool onEdge2 = false;

    f32 e0 = PSVECDotProduct(hit, mFile->mNorms[pPrism->mEdgeIndices[0]]);

    if (0.01f < e0) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e0 && e0 <= 0.01f) {
        onEdge0 = true;
    }

    f32 e1 = PSVECDotProduct(hit, mFile->mNorms[pPrism->mEdgeIndices[1]]);

    if (0.01f < e1) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e1 && e1 <= 0.01f) {
        onEdge1 = true;
    }

    f32 e2 = PSVECDotProduct(hit, mFile->mNorms[pPrism->mEdgeIndices[2]]);

    if (0.01f + pPrism->mHeight < e2) {
        *pFlag = 0;
        return false;
    }

    if (0.0f <= e2 && e2 <= 0.01f) {
        onEdge2 = true;
    }

    *pDist = t;

    if (onEdge0) {
        if (onEdge1) {
            if (onEdge2) {
                *pFlag = 1;
            } else {
                *pFlag = 5;
            }
        } else {
            if (onEdge2) {
                *pFlag = 7;
            } else {
                *pFlag = 2;
            }
        }
    } else {
        if (onEdge1) {
            if (onEdge2) {
                *pFlag = 6;
            } else {
                *pFlag = 3;
            }
        } else {
            if (onEdge2) {
                *pFlag = 4;
            } else {
                *pFlag = 1;
            }
        }
    }

    return true;
}

s32 KCollisionServer::toIndex(const KC_PrismData* pPrism) const {
    return pPrism - (mFile->mPrisms + 1);
}

TVec3f* KCollisionServer::getFaceNormal(const KC_PrismData* pPrism) const {
    return &mFile->mNorms[pPrism->mNormalIndex];
}

TVec3f* KCollisionServer::getEdgeNormal1(const KC_PrismData* pPrism) const {
    return &mFile->mNorms[pPrism->mEdgeIndices[0]];
}

TVec3f* KCollisionServer::getEdgeNormal2(const KC_PrismData* pPrism) const {
    return &mFile->mNorms[pPrism->mEdgeIndices[1]];
}

TVec3f* KCollisionServer::getEdgeNormal3(const KC_PrismData* pPrism) const {
    return &mFile->mNorms[pPrism->mEdgeIndices[2]];
}

TVec3f* KCollisionServer::getNormal(u32 index) const {
    return &mFile->mNorms[index];
}

void KCollisionServer::calXvec(const Fxyz* pVecA, const Fxyz* pVecB, Fxyz* pDst) const {
    pDst->x = pVecA->z * pVecB->y - pVecA->y * pVecB->z;
    pDst->y = pVecA->x * pVecB->z - pVecA->z * pVecB->x;
    pDst->z = pVecA->y * pVecB->x - pVecA->x * pVecB->y;
}

TVec3f KCollisionServer::getPos(const KC_PrismData* pPrism, int vertexIndex) const {
    switch (vertexIndex) {
    case 0: {
        TVec3f* pos = &mFile->mPos[pPrism->mPositionIndex];

        return TVec3f(pos->x, pos->y, pos->z);
    }
    case 1: {
        Fxyz* pos = reinterpret_cast< Fxyz* >(&mFile->mPos[pPrism->mPositionIndex]);
        Fxyz* edge2 = reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mEdgeIndices[2]]);

        Fxyz finalPos;

        calXvec(reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mEdgeIndices[1]]), reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mNormalIndex]),
                &finalPos);

        f32 sideLength = pPrism->mHeight / (finalPos.x * edge2->x + finalPos.y * edge2->y + finalPos.z * edge2->z);

        finalPos.x = pos->x + sideLength * finalPos.x;
        finalPos.y = pos->y + sideLength * finalPos.y;
        finalPos.z = pos->z + sideLength * finalPos.z;

        return TVec3f(finalPos.x, finalPos.y, finalPos.z);
    }
    case 2: {
        Fxyz* pos = reinterpret_cast< Fxyz* >(&mFile->mPos[pPrism->mPositionIndex]);
        Fxyz* edge2 = reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mEdgeIndices[2]]);

        Fxyz finalPos;

        calXvec(reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mNormalIndex]), reinterpret_cast< Fxyz* >(&mFile->mNorms[pPrism->mEdgeIndices[0]]),
                &finalPos);

        f32 sideLength = pPrism->mHeight / (finalPos.x * edge2->x + finalPos.y * edge2->y + finalPos.z * edge2->z);

        finalPos.x = pos->x + sideLength * finalPos.x;
        finalPos.y = pos->y + sideLength * finalPos.y;
        finalPos.z = pos->z + sideLength * finalPos.z;

        return TVec3f(finalPos.x, finalPos.y, finalPos.z);
    }
    default:
        return TVec3f(0.0f, 0.0f, 0.0f);
    }
}

KC_PrismData* KCollisionServer::getPrismData(u32 index) const {
    return &mFile->mPrisms[1 + index];
}

s32 KCollisionServer::getTriangleNum() const {
    return (reinterpret_cast< u8* >(mFile->mOctree.get()) - reinterpret_cast< u8* >(mFile->mPrisms.get() + 1)) / sizeof(KC_PrismData);
}

JMapInfoIter KCollisionServer::getAttributes(u32 index) const {
    KC_PrismData* prism = &mFile->mPrisms[1 + index];

    return JMapInfoIter(mapInfo, prism->mAttribute);
}

s32* KCollisionServer::searchBlock(s32* pShift, const u32& rX, const u32& rY, const u32& rZ) const {
    KCLFile* file = mFile;
    u8* octree = reinterpret_cast< u8* >(file->mOctree.get());
    s32 blockWidthShift = *pShift = file->mBlockWidthShift;

    s32 xyShift = file->mBlockXYShift;
    s32 xShift = file->mBlockXShift;
    s32 offset = (((rZ >> blockWidthShift) << xyShift) | ((rY >> blockWidthShift) << xShift) | (rX >> blockWidthShift)) * 4;

    if (xyShift == -1 && xShift == -1) {
        offset = 0;
    }

    while ((offset = *reinterpret_cast< s32* >(octree + offset)) >= 0) {
        octree += offset;
        (*pShift)--;
        s32 shift = *pShift;

        offset = ((((rZ >> shift) & 1) << 2) | (((rY >> shift) & 1) << 1) | ((rX >> shift) & 1)) * 4;
    }

    return reinterpret_cast< s32* >(octree + (offset & 0x7FFFFFFF));
}

void KCollisionServer::V3u::setUsingCast(const TVec3f& rPos) {
    x = static_cast< s32 >(rPos.x);
    y = static_cast< s32 >(rPos.y);
    z = static_cast< s32 >(rPos.z);
}

Fxyz& Fxyz::operator=(const Fxyz& rOther) {
    x = rOther.x;
    y = rOther.y;
    z = rOther.z;

    return *this;
}
