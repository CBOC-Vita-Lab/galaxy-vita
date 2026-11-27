#include "Game/LiveActor/SensorHitChecker.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#ifdef TARGET_PC
#include <cstdint>
#include <cstdlib>
#include "Game/LiveActor/LiveActor.hpp"
extern "C" void port_logic_count(int what, unsigned n);  // (logic12: logging builds' per-turn counts, platform/src/port/perf.cpp)
extern "C" int port_hl_sensor_on(void);                  // GXV_HL_SENSOR (hivelogic), platform/src/port/perf.cpp: 0 off, 1 on, 2 verify
extern "C" int port_sensor_bp_on(void);                  // GXV_SENSOR_BP (update23), platform/src/port/perf.cpp: 0 off, 1 on, 2 verify
extern "C" int port_upd_prefetch_on(void);               // GXV_UPD_PREFETCH (update23), platform/src/port/perf.cpp: 0 off, 1 on
#include "port/sensor_bp.h"
#include "port/obj_cut.h"  // (PortOcDue: the "sensor-bp:" line)
#include "port/diagnostics.h"
// GXV_HL_SENSOR=2 (perf.cpp): counts a turn's compared sensors, and logs the first mismatches with the host's name.
extern "C" void port_hl_verify_sensor(unsigned compared, unsigned mismatched, const char* host, unsigned type, unsigned fastCount,
                                      unsigned origCount);

// GXV_HL_SENSOR (hivelogic, 6 Nov; default 1, exact): the sensor pairs of movement() are looked at from compact copies.
// The original tests every pair's two sensors for validity and their hosts for clipping, and reads both sensors through their
// pointers, inside the pair loops: a group of n sensors against one of m costs n * m validity and clipping tests (a call each).
// Here each group's sensors that pass those tests (valid by host and by system, host not clipped) are copied once per movement(),
// in their order, with position, radius and host, and the same pair loops run over the copies. Nothing a pair test does
// (HitSensor::addHitSensor: the hit list) changes a sensor's validity, position or radius, or a host's clipping, so the copies
// equal what the original reads at each pair; the pairs are tested in the original order, with the same arithmetic and the same
// comparison, so every hit list gets the same sensors in the same order. 0: the original loops.
namespace {
    struct HlSensor {
        f32 x, y, z, r;
        HitSensor* sensor;
        const LiveActor* host;
    };
    HlSensor* sHlBuf = nullptr;  // (host heap, grows as needed; never freed)
    s32 sHlCap = 0;
    // GXV_HL_SENSOR=2: every sensor's hit list after the copies' loops (its count, then its entries), group by group
    HitSensor** sHlSnap = nullptr;  // (host heap, grows as needed; never freed)
    s32 sHlSnapCap = 0;

    // The hit lists of the six groups' sensors, in group order, into sHlSnap: false if there was no room.
    bool hlSnapshot(SensorGroup* const* pGroups) {
        s32 need = 0;
        for (s32 g = 0; g < 6; g++) {
            for (s32 i = 0; i < pGroups[g]->mSensorCount; i++) {
                need += 1 + pGroups[g]->mSensors[i]->mSensorCount;
            }
        }
        if (need > sHlSnapCap) {
            HitSensor** pGrown = static_cast< HitSensor** >(std::realloc(sHlSnap, sizeof(HitSensor*) * static_cast< size_t >(need + 256)));
            if (pGrown == nullptr) {
                return false;
            }
            sHlSnap = pGrown;
            sHlSnapCap = need + 256;
        }
        HitSensor** pOut = sHlSnap;
        for (s32 g = 0; g < 6; g++) {
            for (s32 i = 0; i < pGroups[g]->mSensorCount; i++) {
                const HitSensor* pSensor = pGroups[g]->mSensors[i];
                *pOut++ = reinterpret_cast< HitSensor* >(static_cast< uintptr_t >(pSensor->mSensorCount));
                for (s32 k = 0; k < pSensor->mSensorCount; k++) {
                    *pOut++ = pSensor->mSensors[k];
                }
            }
        }
        return true;
    }

    // The hit lists now (the original loops') against sHlSnap (the copies'): reported to perf.cpp, the first mismatches logged there.
    void hlCompare(SensorGroup* const* pGroups) {
        unsigned compared = 0, mismatched = 0;
        HitSensor* const* pSnap = sHlSnap;
        for (s32 g = 0; g < 6; g++) {
            for (s32 i = 0; i < pGroups[g]->mSensorCount; i++) {
                const HitSensor* pSensor = pGroups[g]->mSensors[i];
                const s32 fastCount = static_cast< s32 >(reinterpret_cast< uintptr_t >(*pSnap++));
                bool same = fastCount == pSensor->mSensorCount;
                for (s32 k = 0; same && k < fastCount; k++) {
                    same = pSnap[k] == pSensor->mSensors[k];
                }
                pSnap += fastCount;
                ++compared;
                if (!same) {
                    ++mismatched;
                    const char* host = pSensor->mHost != nullptr ? pSensor->mHost->mName : "?";
                    port_hl_verify_sensor(0, 1, host, pSensor->mType, static_cast< unsigned >(fastCount),
                                          static_cast< unsigned >(pSensor->mSensorCount));
                }
            }
        }
        port_hl_verify_sensor(compared, 0, nullptr, 0, 0, 0);
    }

    s32 hlCollect(const SensorGroup* pGroup, HlSensor* pOut) {
        s32 n = 0;
        const s32 count = pGroup->mSensorCount;
        const bool prefetch = port_upd_prefetch_on() != 0;
        for (s32 i = 0; i < count; i++) {
            HitSensor* pSensor = pGroup->mSensors[i];
            if (prefetch) {
                // GXV_UPD_PREFETCH (update23; exact: hints only, nothing is read for the result): the sensors are scattered heap
                // objects and each test also reads its host's flags; ask for the lines of the sensor 4 ahead (type .. radius,
                // validity .. host) and the host flags of the one 2 ahead (its sensor was asked for 2 steps ago).
                if (i + 4 < count) {
                    const char* q = reinterpret_cast< const char* >(pGroup->mSensors[i + 4]);
                    __builtin_prefetch(q);
                    __builtin_prefetch(q + 0x20);
                }
                if (i + 2 < count) {
                    const LiveActor* pHost = pGroup->mSensors[i + 2]->mHost;
                    if (pHost != nullptr) {
                        __builtin_prefetch(&pHost->mFlag);
                    }
                }
            }
            if (pSensor->mValidByHost && pSensor->mValidBySystem && !MR::isClipped(pSensor->mHost)) {
                HlSensor& rOut = pOut[n++];
                rOut.x = pSensor->mPosition.x;
                rOut.y = pSensor->mPosition.y;
                rOut.z = pSensor->mPosition.z;
                rOut.r = pSensor->getRadius();
                rOut.sensor = pSensor;
                rOut.host = pSensor->mHost;
            }
        }
        return n;
    }

    // SensorHitChecker::checkAttack on the copies: the same operations in the same order.
    inline void hlAttack(const HlSensor& rSensor1, const HlSensor& rSensor2) {
        if (rSensor1.host != rSensor2.host) {
            f32 xPos = rSensor1.x - rSensor2.x;
            f32 yPos = rSensor1.y - rSensor2.y;
            f32 zPos = rSensor1.z - rSensor2.z;
            f32 totalSize = rSensor1.r + rSensor2.r;

            if (!((((xPos * xPos) + (yPos * yPos)) + (zPos * zPos)) >= (totalSize * totalSize))) {
                if (!rSensor2.sensor->isType(ATYPE_EYE)) {
                    rSensor1.sensor->addHitSensor(rSensor2.sensor);
                }

                if (!rSensor1.sensor->isType(ATYPE_EYE)) {
                    rSensor2.sensor->addHitSensor(rSensor1.sensor);
                }
            }
        }
    }

    // doObjColGroup and doObjColInSameGroup on the copies (the same pair order)
    void hlGroups(const HlSensor* pList1, s32 count1, const HlSensor* pList2, s32 count2) {
        for (s32 i = 0; i < count1; i++) {
#pragma clang loop vectorize(disable)
            for (s32 x = 0; x < count2; x++) {
                hlAttack(pList1[i], pList2[x]);
            }
        }
    }

    void hlSameGroup(const HlSensor* pList, s32 count) {
        for (s32 i = 0; i < count; i++) {
#pragma clang loop vectorize(disable)
            for (s32 x = i; x < count; x++) {
                hlAttack(pList[i], pList[x]);
            }
        }
    }

    // GXV_SENSOR_BP (update23, 8 Nov; default 1, exact; port/sensor_bp.h has the proof): the 14 loops above, in the same order, but a
    // pair whose sensors are apart by more than both radii (with a margin) along the axis of the widest spread is not looked at; the
    // others are tested by hlAttack in the original order, so every hit list gets the same sensors in the same order. Turns with a
    // NaN, infinite or huge (> 1e9) coordinate or radius, or with no room for the keys, run the plain loops (false returned).
    // 0: the plain loops. 2: verify (as GXV_HL_SENSOR=2: the original loops run after, their lists are used and compared).
    port::sensorbp::Scratch sBp;
    uint32_t sBpTurns, sBpPlain, sBpTick;
    uint64_t sBpSensors, sBpPairs, sBpTested;
    int64_t sBpLast;

    bool hlBroadPhase(HlSensor* const* lists, const s32* counts) {
        using namespace port::sensorbp;
        const int axis = ChooseAxis(lists, counts, 6);
        s32 total = 0, most = 0;
        for (s32 g = 0; g < 6; g++) {
            total += counts[g];
            most = counts[g] > most ? counts[g] : most;
        }
        if (axis < 0 || !Grow(sBp.keys, sBp.keyCap, total) || !Grow(sBp.cand, sBp.candCap, most)) {
            ++sBpPlain;
            return false;
        }
        enum { P, R, E, S, M, C };
        Group grp[6];
        Key* pKeys = sBp.keys;
        grp[P].keys = nullptr;  // (the player group is never the inner one)
        grp[P].n = counts[P];
        grp[P].maxW = 0.0;
        for (s32 g = R; g <= C; g++) {
            grp[g] = BuildGroup(lists[g], counts[g], axis, pKeys);
            pKeys += counts[g];
        }
        const auto attack = [](const HlSensor& a, const HlSensor& b) { hlAttack(a, b); };
        uint32_t tested = 0;
        int32_t* cand = sBp.cand;
        Groups(lists[P], counts[P], lists[C], grp[C], axis, cand, attack, tested);
        Groups(lists[P], counts[P], lists[M], grp[M], axis, cand, attack, tested);
        Groups(lists[P], counts[P], lists[R], grp[R], axis, cand, attack, tested);
        Groups(lists[P], counts[P], lists[S], grp[S], axis, cand, attack, tested);
        Groups(lists[P], counts[P], lists[E], grp[E], axis, cand, attack, tested);
        Groups(lists[R], counts[R], lists[C], grp[C], axis, cand, attack, tested);
        Groups(lists[R], counts[R], lists[M], grp[M], axis, cand, attack, tested);
        Groups(lists[R], counts[R], lists[S], grp[S], axis, cand, attack, tested);
        Groups(lists[R], counts[R], lists[E], grp[E], axis, cand, attack, tested);
        Groups(lists[E], counts[E], lists[C], grp[C], axis, cand, attack, tested);
        Groups(lists[E], counts[E], lists[M], grp[M], axis, cand, attack, tested);
        Groups(lists[E], counts[E], lists[S], grp[S], axis, cand, attack, tested);
        Groups(lists[C], counts[C], lists[M], grp[M], axis, cand, attack, tested);
        SameGroup(lists[C], grp[C], axis, cand, attack, tested);
        ++sBpTurns;
        sBpSensors += static_cast< uint32_t >(total);
        sBpTested += tested;
        sBpPairs += static_cast< uint64_t >(counts[P]) * static_cast< uint64_t >(counts[C] + counts[M] + counts[R] + counts[S] + counts[E]) +
                    static_cast< uint64_t >(counts[R]) * static_cast< uint64_t >(counts[C] + counts[M] + counts[S] + counts[E]) +
                    static_cast< uint64_t >(counts[E]) * static_cast< uint64_t >(counts[C] + counts[M] + counts[S]) +
                    static_cast< uint64_t >(counts[C]) * static_cast< uint64_t >(counts[M]) +
                    static_cast< uint64_t >(counts[C]) * static_cast< uint64_t >(counts[C] + 1) / 2;
#if PORT_LOGGING
        // about every 5 s (logging builds): sensor-bp: N turns: a turn S sensors taking part, P pairs in the loops, T tested (x %); K plain
        if (PortOcDue(sBpLast, sBpTick)) {
            const double t = sBpTurns != 0 ? static_cast< double >(sBpTurns) : 1.0;
            port_log_summary("sensor-bp: %u turns (GXV_SENSOR_BP): a turn %.1f sensors taking part, %.1f pairs in the 14 loops, %.1f tested (%.1f %%); "
                             "%u turns plain (NaN / huge values / no room)",
                             sBpTurns, static_cast< double >(sBpSensors) / t, static_cast< double >(sBpPairs) / t,
                             static_cast< double >(sBpTested) / t,
                             sBpPairs != 0 ? 100.0 * static_cast< double >(sBpTested) / static_cast< double >(sBpPairs) : 0.0, sBpPlain);
            sBpTurns = sBpPlain = 0;
            sBpSensors = sBpPairs = sBpTested = 0;
        }
#endif
        return true;
    }
}  // namespace
#endif

SensorGroup::SensorGroup(int maxSensors, const char* pName) {
    mMaxSensors = maxSensors;
    mSensorCount = 0;
    mSensors = nullptr;
    mSensors = new HitSensor*[maxSensors];

    for (s32 i = 0; i < mMaxSensors; i++) {
        mSensors[i] = nullptr;
    }
}

void SensorGroup::add(HitSensor* pSensor) {
    mSensors[mSensorCount] = pSensor;
    mSensorCount++;
    pSensor->mSensorGroup = this;
}

void SensorGroup::remove(HitSensor* pSensor) {
    for (s32 i = 0; i < mSensorCount; i++) {
        if (mSensors[i] == pSensor) {
            u32 count = mSensorCount - 1;
            mSensors[i] = mSensors[count];
            mSensorCount--;
            break;
        }
    }
}

void SensorGroup::clear() const {
    for (s32 i = 0; i < mSensorCount; i++) {
        mSensors[i]->mSensorCount = 0;
    }
}

void SensorHitChecker::init(const JMapInfoIter& rIter) {
    MR::connectToScene(this, MR::MovementType_SensorHitChecker, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
}

void SensorHitChecker::initGroup(HitSensor* pSensor) {
    if (MR::isSensorPlayer(pSensor)) {
        pSensor->mSensorGroup = mPlayerGroup;
    } else if (MR::isSensorRide(pSensor)) {
        pSensor->mSensorGroup = mRideGroup;
    } else if (pSensor->isType(ATYPE_EYE)) {
        pSensor->mSensorGroup = mEyeGroup;
    } else if (pSensor->isType(ATYPE_COIN) || pSensor->isType(ATYPE_STAR_PIECE) || pSensor->isType(ATYPE_ENEMY_SIMPLE) ||
               pSensor->isType(ATYPE_MAP_OBJ_SIMPLE) || pSensor->isType(ATYPE_PLAYER_AUTO_JUMP) || MR::isSensorRush(pSensor) ||
               MR::isSensorAutoRush(pSensor)) {
        pSensor->mSensorGroup = mSimpleGroup;
    } else {
        if (MR::isSensorMapObj(pSensor)) {
            pSensor->mSensorGroup = mMapObjGroup;
        } else {
            pSensor->mSensorGroup = mCharacterGroup;
        }
    }
}

void SensorHitChecker::movement() {
    mPlayerGroup->clear();
    mRideGroup->clear();
    mEyeGroup->clear();
    mSimpleGroup->clear();
    mMapObjGroup->clear();
    mCharacterGroup->clear();
#ifdef TARGET_PC
    {
        const unsigned p = mPlayerGroup->mSensorCount, r = mRideGroup->mSensorCount, e = mEyeGroup->mSensorCount;
        const unsigned s = mSimpleGroup->mSensorCount, m = mMapObjGroup->mSensorCount, c = mCharacterGroup->mSensorCount;
        port_logic_count(10, p * (c + m + r + s + e) + r * (c + m + s + e) + e * (c + m + s) + c * m + c * (c + 1) / 2);
    }
    const int hlMode = port_hl_sensor_on();
    if (hlMode != 0) {
        // GXV_HL_SENSOR (above): the same 14 group loops, in the same order, over each group's sensors that can take part.
        SensorGroup* const groups[6] = {mPlayerGroup, mRideGroup, mEyeGroup, mSimpleGroup, mMapObjGroup, mCharacterGroup};
        s32 need = 0;
        for (s32 g = 0; g < 6; g++) {
            need += groups[g]->mSensorCount;
        }
        if (need > sHlCap) {
            HlSensor* pGrown = static_cast< HlSensor* >(std::realloc(sHlBuf, sizeof(HlSensor) * static_cast< size_t >(need + 64)));
            if (pGrown != nullptr) {
                sHlBuf = pGrown;
                sHlCap = need + 64;
            }
        }
        if (need <= sHlCap && sHlBuf != nullptr) {
            HlSensor* lists[6];
            s32 counts[6];
            HlSensor* pNext = sHlBuf;
            for (s32 g = 0; g < 6; g++) {
                lists[g] = pNext;
                counts[g] = hlCollect(groups[g], pNext);
                pNext += counts[g];
            }
            enum { P, R, E, S, M, C };
            const int bpMode = port_sensor_bp_on();
            // GXV_SENSOR_BP: the same loops through the broad phase (false: a turn it does not take, the plain loops)
            if (bpMode == 0 || !hlBroadPhase(lists, counts)) {
                hlGroups(lists[P], counts[P], lists[C], counts[C]);
                hlGroups(lists[P], counts[P], lists[M], counts[M]);
                hlGroups(lists[P], counts[P], lists[R], counts[R]);
                hlGroups(lists[P], counts[P], lists[S], counts[S]);
                hlGroups(lists[P], counts[P], lists[E], counts[E]);
                hlGroups(lists[R], counts[R], lists[C], counts[C]);
                hlGroups(lists[R], counts[R], lists[M], counts[M]);
                hlGroups(lists[R], counts[R], lists[S], counts[S]);
                hlGroups(lists[R], counts[R], lists[E], counts[E]);
                hlGroups(lists[E], counts[E], lists[C], counts[C]);
                hlGroups(lists[E], counts[E], lists[M], counts[M]);
                hlGroups(lists[E], counts[E], lists[S], counts[S]);
                hlGroups(lists[C], counts[C], lists[M], counts[M]);
                hlSameGroup(lists[C], counts[C]);
            }
            if (hlMode != 2 && bpMode != 2) {
                return;
            }
            // GXV_HL_SENSOR=2 or GXV_SENSOR_BP=2: the lists made above are checked against the original loops' below.
            // GXV_HL_SENSOR=2: keep the copies' hit lists, clear, run the original loops (their lists are the ones used), compare.
            if (hlSnapshot(groups)) {
                for (s32 g = 0; g < 6; g++) {
                    groups[g]->clear();
                }
                doObjColGroup(mPlayerGroup, mCharacterGroup);
                doObjColGroup(mPlayerGroup, mMapObjGroup);
                doObjColGroup(mPlayerGroup, mRideGroup);
                doObjColGroup(mPlayerGroup, mSimpleGroup);
                doObjColGroup(mPlayerGroup, mEyeGroup);
                doObjColGroup(mRideGroup, mCharacterGroup);
                doObjColGroup(mRideGroup, mMapObjGroup);
                doObjColGroup(mRideGroup, mSimpleGroup);
                doObjColGroup(mRideGroup, mEyeGroup);
                doObjColGroup(mEyeGroup, mCharacterGroup);
                doObjColGroup(mEyeGroup, mMapObjGroup);
                doObjColGroup(mEyeGroup, mSimpleGroup);
                doObjColGroup(mCharacterGroup, mMapObjGroup);
                doObjColInSameGroup(mCharacterGroup);
                hlCompare(groups);
                return;
            }
            for (s32 g = 0; g < 6; g++) {  // (no room to keep them: the original loops below, unchecked)
                groups[g]->clear();
            }
        }
        // (no room for the copies: the original loops below)
    }
#endif
    doObjColGroup(mPlayerGroup, mCharacterGroup);
    doObjColGroup(mPlayerGroup, mMapObjGroup);
    doObjColGroup(mPlayerGroup, mRideGroup);
    doObjColGroup(mPlayerGroup, mSimpleGroup);
    doObjColGroup(mPlayerGroup, mEyeGroup);
    doObjColGroup(mRideGroup, mCharacterGroup);
    doObjColGroup(mRideGroup, mMapObjGroup);
    doObjColGroup(mRideGroup, mSimpleGroup);
    doObjColGroup(mRideGroup, mEyeGroup);
    doObjColGroup(mEyeGroup, mCharacterGroup);
    doObjColGroup(mEyeGroup, mMapObjGroup);
    doObjColGroup(mEyeGroup, mSimpleGroup);
    doObjColGroup(mCharacterGroup, mMapObjGroup);
    doObjColInSameGroup(mCharacterGroup);
}

void SensorHitChecker::doObjColGroup(SensorGroup* pGroup1, SensorGroup* pGroup2) const {
    HitSensor* curGroup1Sensor;
    HitSensor* curGroup2Sensor;
    s32 group1SensorCount = pGroup1->mSensorCount;
    for (s32 i = 0; i < group1SensorCount; i++) {
        curGroup1Sensor = pGroup1->mSensors[i];
        bool group1Sensorvalidated = curGroup1Sensor->mValidByHost && curGroup1Sensor->mValidBySystem;

        if (group1Sensorvalidated && !MR::isClipped(curGroup1Sensor->mHost)) {
            s32 group2SensorCount = pGroup2->mSensorCount;
            for (s32 x = 0; x < group2SensorCount; x++) {
                curGroup2Sensor = pGroup2->mSensors[x];
                bool group2Validated = curGroup2Sensor->mValidByHost && curGroup2Sensor->mValidBySystem;

                if (group2Validated && !MR::isClipped(curGroup2Sensor->mHost)) {
                    checkAttack(curGroup1Sensor, curGroup2Sensor);
                }
            }
        }
    }
}

void SensorHitChecker::doObjColInSameGroup(SensorGroup* pSensorGroup) const {
    HitSensor* pFirstSensor;
    HitSensor* pSecondSensor;
    s32 sensorGroupCount = pSensorGroup->mSensorCount;
    for (s32 i = 0; i < sensorGroupCount; i++) {
        pFirstSensor = pSensorGroup->mSensors[i];
        bool isFirstSensorValid = pFirstSensor->mValidByHost && pFirstSensor->mValidBySystem;

        if (isFirstSensorValid && !MR::isClipped(pFirstSensor->mHost)) {
            for (s32 x = i; x < sensorGroupCount; x++) {
                pSecondSensor = pSensorGroup->mSensors[x];
                bool isSecondSensorValid = pSecondSensor->mValidByHost && pSecondSensor->mValidBySystem;

                if (isSecondSensorValid && !MR::isClipped(pSecondSensor->mHost)) {
                    checkAttack(pFirstSensor, pSecondSensor);
                }
            }
        }
    }
}

void SensorHitChecker::checkAttack(HitSensor* pSensor1, HitSensor* pSensor2) const {
    if (pSensor1->mHost != pSensor2->mHost) {
        f32 xPos = pSensor1->mPosition.x - pSensor2->mPosition.x;
        f32 yPos = pSensor1->mPosition.y - pSensor2->mPosition.y;
        f32 zPos = pSensor1->mPosition.z - pSensor2->mPosition.z;
        f32 totalSize = pSensor1->getRadius() + pSensor2->getRadius();

        if (!((((xPos * xPos) + (yPos * yPos)) + (zPos * zPos)) >= (totalSize * totalSize))) {
            if (!pSensor2->isType(ATYPE_EYE)) {
                pSensor1->addHitSensor(pSensor2);
            }

            if (!pSensor1->isType(ATYPE_EYE)) {
                pSensor2->addHitSensor(pSensor1);
            }
        }
    }
}

namespace MR {
    void initHitSensorGroup(HitSensor* pSensor) {
        MR::getSceneObj< SensorHitChecker >(SceneObj_SensorHitChecker)->initGroup(pSensor);
    }
};  // namespace MR

SensorHitChecker::~SensorHitChecker() {
}

SensorHitChecker::SensorHitChecker(const char* pName) : NameObj(pName) {
    mPlayerGroup = nullptr;
    mRideGroup = nullptr;
    mEyeGroup = nullptr;
    mSimpleGroup = nullptr;
    mMapObjGroup = nullptr;
    mCharacterGroup = nullptr;

    mPlayerGroup = new SensorGroup(0x10, "Player");
    mRideGroup = new SensorGroup(0x80, "Ride");
    mEyeGroup = new SensorGroup(0x200, "Eye");
    mSimpleGroup = new SensorGroup(0x800, "Simple");
    mMapObjGroup = new SensorGroup(0x400, "MapObj");
    mCharacterGroup = new SensorGroup(0x400, "Character");
}
