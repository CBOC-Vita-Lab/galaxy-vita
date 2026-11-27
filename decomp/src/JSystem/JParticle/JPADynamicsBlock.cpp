#include "JSystem/JParticle/JPADynamicsBlock.hpp"
#include "JSystem/JMath/JMATrigonometric.hpp"
#include "JSystem/JParticle/JPAEmitter.hpp"
#ifdef TARGET_PC
#include "JSystem/JParticle/JPAParticle.hpp"  // (boss23 round 2: the new particles' size)
#endif

namespace {
    inline f32 JPASinShort(s16 v) {
        return JMath::sSinCosTable.table[static_cast< u16 >(v) >> 2].a1;
    }

    inline f32 JPACosShort(s16 v) {
        return reinterpret_cast< const f32* >(&JMath::sSinCosTable.table[static_cast< u16 >(v) >> 2])[1];
    }
}

#ifdef TARGET_PC
// GXV_PB_PTCL (Performance-mode visual budget, decomp/src/Game/LiveActor/LodCtrl.cpp): the emission of an emitter far from the camera
// and the player is thinned by a factor (the fraction stays in mEmitCount, so a rate of 1 a turn emits every second turn).
// (flow23: the factor of THIS calc, decided at JPAEmitterManager::calcNormal's start: 1 outside the stage's own particle calc)
extern "C" float port_pb_ptcl_turn_factor(void);
extern "C" float port_pb_ptcl_scale_at(float x, float y, float z);
extern "C" float g_port_boss_ptcl_grow;   // (boss23 round 2, PETARI_BOSS_PTCL_GROW: > 1 while thinned emitters make larger particles)
extern "C" float g_port_boss_ptcl_scale;  // (boss23, PETARI_BOSS_PTCL: < 1 while a boss storm is thinned; decomp/src/Game/Scene/GameScene.cpp)
extern "C" void port_logic_count(int what, unsigned n);
#endif

void JPAVolumePoint(JPAEmitterWorkData* work) {
    work->mVolumeCalcData.mVolumePos.zero();
    f32 z = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    f32 y = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    f32 x = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    work->mVolumeCalcData.mVelOmni.set< f32 >(x, y, z);
    work->mVolumeCalcData.mVelAxis.set< f32 >(work->mVolumeCalcData.mVelOmni.x, 0.0f, work->mVolumeCalcData.mVelOmni.z);
}

void JPAVolumeLine(JPAEmitterWorkData* work) {
    if (work->mpEmtr->checkFlag(JPADynFlag_FixedInterval)) {
        work->mVolumeCalcData.mVolumePos.set< f32 >(0.0f, 0.0f, work->mVolumeSize * ((work->mVolumeEmitIdx / (work->mEmitCount - 1.0f) - 0.5f)));
        work->mVolumeEmitIdx++;
    } else {
        work->mVolumeCalcData.mVolumePos.set< f32 >(0.0f, 0.0f, work->mVolumeSize * (work->mpEmtr->mRndm.get_rndm_f() - 0.5f));
    }

    work->mVolumeCalcData.mVelOmni.set< f32 >(0.0f, 0.0f, work->mVolumeCalcData.mVolumePos.z * work->mGlobalScl.z);
    work->mVolumeCalcData.mVelAxis.set< f32 >(0.0f, 0.0f, work->mVolumeCalcData.mVolumePos.z);
}

void JPAVolumeCircle(JPAEmitterWorkData* work) {
    s16 theta;
    f32 distance;

    if (work->mpEmtr->checkFlag(JPADynFlag_FixedInterval)) {
        theta = (s16)((work->mVolumeEmitIdx << 16) / work->mEmitCount);
        theta *= work->mVolumeSweep;
        work->mVolumeEmitIdx++;
    } else {
        theta = work->mVolumeSweep * work->mpEmtr->get_r_ss();
    }

    distance = work->mpEmtr->get_r_f();
    if (work->mpEmtr->checkFlag(JPADynFlag_FixedDensity)) {
        distance = 1.0f - (distance * distance);
    }

    distance = work->mVolumeSize * (work->mVolumeMinRad + distance * (1.0f - work->mVolumeMinRad));
    work->mVolumeCalcData.mVolumePos.set< f32 >(distance * JPASinShort(theta), 0.0f, distance * JPACosShort(theta));
    work->mVolumeCalcData.mVelOmni.mul(work->mVolumeCalcData.mVolumePos, work->mGlobalScl);
    work->mVolumeCalcData.mVelAxis.set< f32 >(work->mVolumeCalcData.mVolumePos.x, 0.0f, work->mVolumeCalcData.mVolumePos.z);
}

void JPAVolumeCube(JPAEmitterWorkData* work) {
    f32 z = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    f32 y = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    f32 x = work->mpEmtr->mRndm.get_rndm_f() - 0.5f;
    work->mVolumeCalcData.mVolumePos.set< f32 >(work->mVolumeSize * x, work->mVolumeSize * y, work->mVolumeSize * z);
    work->mVolumeCalcData.mVelOmni.mul(work->mVolumeCalcData.mVolumePos, work->mGlobalScl);
    work->mVolumeCalcData.mVelAxis.set< f32 >(work->mVolumeCalcData.mVolumePos.x, 0.0f, work->mVolumeCalcData.mVolumePos.z);
}

static void JPAVolumeSphere(JPAEmitterWorkData* work) {
    s16 phi, theta;
    if (work->mpEmtr->checkFlag(JPADynFlag_FixedInterval)) {
#if defined(TARGET_PC) && !defined(__arm__)   // (merge of 3.0 on c181: not on the Vita (ARM): its division by zero already gives 0 as the Wii's, and c181's code, with its PGO profile, stays there)
        // PORT: the first ring of a fixed-interval sphere has mVolumeAngleMax == 1 (and a one-division sphere mDivNumber == 1):
        // an integer division by zero. The Wii's divw and the Vita's sdiv give 0 there; x86's idiv traps (SIGFPE on the desktop
        // build, seen when a spin hit a multiplayer puppet). Same result as the consoles: a zero divisor gives 0.
        const s32 divPhi = work->mDivNumber - 1, divAngle = work->mVolumeAngleMax - 1;
        phi = (u16)((divPhi != 0 ? work->mVolumeX * 0x8000 / divPhi : 0) + 0x4000);
        u16 angle = (u16)(divAngle != 0 ? work->mVolumeAngleNum * 0x10000 / divAngle : 0);
#else
        phi = (u16)(work->mVolumeX * 0x8000 / (work->mDivNumber - 1) + 0x4000);
        u16 angle = (u16)(work->mVolumeAngleNum * 0x10000 / (work->mVolumeAngleMax - 1));
#endif
        theta = (f32)angle * work->mVolumeSweep + 0x8000;
        work->mVolumeAngleNum++;
        if (work->mVolumeAngleNum == work->mVolumeAngleMax) {
            work->mVolumeAngleNum = 0;
            work->mVolumeX++;
            if (work->mVolumeX * 2 < work->mDivNumber) {
                work->mVolumeAngleMax = work->mVolumeAngleMax != 1 ? work->mVolumeAngleMax + 4 : work->mVolumeAngleMax + 3;
            } else {
                work->mVolumeAngleMax = work->mVolumeAngleMax != 4 ? work->mVolumeAngleMax - 4 : 1;
            }
        }
    } else {
        phi = work->mpEmtr->get_r_ss() >> 1;
        theta = work->mVolumeSweep * work->mpEmtr->get_r_ss();
    }

    f32 rnd = work->mpEmtr->get_r_f();
    if (work->mpEmtr->checkFlag(JPADynFlag_FixedDensity)) {
        rnd = 1.0f - rnd * rnd * rnd;
    }
    rnd = work->mVolumeSize * (work->mVolumeMinRad + rnd * (1.0f - work->mVolumeMinRad));
    work->mVolumeCalcData.mVolumePos.set< f32 >(rnd * JPACosShort(phi) * JPASinShort(theta), -rnd * JPASinShort(phi), rnd * JPACosShort(phi) * JPACosShort(theta));
    work->mVolumeCalcData.mVelOmni.mul(work->mVolumeCalcData.mVolumePos, work->mGlobalScl);
    work->mVolumeCalcData.mVelAxis.set< f32 >(work->mVolumeCalcData.mVolumePos.x, 0.0f, work->mVolumeCalcData.mVolumePos.z);
}

static void JPAVolumeCylinder(JPAEmitterWorkData* work) {
    s16 theta = work->mVolumeSweep * work->mpEmtr->get_r_ss();
    f32 rnd = work->mpEmtr->get_r_f();
    if (work->mpEmtr->checkFlag(JPADynFlag_FixedDensity)) {
        rnd = 1.0f - rnd * rnd;
    }
    f32 rad = work->mVolumeSize * (work->mVolumeMinRad + rnd * (1.0f - work->mVolumeMinRad));
    work->mVolumeCalcData.mVolumePos.set< f32 >(rad * JPASinShort(theta), work->mVolumeSize * work->mpEmtr->get_r_zp(), rad * JPACosShort(theta));
    work->mVolumeCalcData.mVelOmni.mul(work->mVolumeCalcData.mVolumePos, work->mGlobalScl);
    work->mVolumeCalcData.mVelAxis.set< f32 >(work->mVolumeCalcData.mVolumePos.x, 0.0f, work->mVolumeCalcData.mVolumePos.z);
}

static void JPAVolumeTorus(JPAEmitterWorkData* work) {
    s16 theta = work->mVolumeSweep * work->mpEmtr->get_r_ss();
    s16 phi = work->mpEmtr->get_r_ss();
    f32 rad = work->mVolumeSize * work->mVolumeMinRad;
    work->mVolumeCalcData.mVelAxis.set< f32 >(rad * JPASinShort(theta) * JPACosShort(phi), rad * JPASinShort(phi), rad * JPACosShort(theta) * JPACosShort(phi));
    work->mVolumeCalcData.mVolumePos.set< f32 >(work->mVolumeCalcData.mVelAxis.x + work->mVolumeSize * JPASinShort(theta),
                                                work->mVolumeCalcData.mVelAxis.y,
                                                work->mVolumeCalcData.mVelAxis.z + work->mVolumeSize * JPACosShort(theta));
    work->mVolumeCalcData.mVelOmni.mul(work->mVolumeCalcData.mVolumePos, work->mGlobalScl);
}

JPADynamicsBlock::JPADynamicsBlock(u8 const* data) {
    mpData = (const JPADynamicsBlockData*)data;
    init();
}

enum {
    VOL_Cube = 0x00,
    VOL_Sphere = 0x01,
    VOL_Cylinder = 0x02,
    VOL_Torus = 0x03,
    VOL_Point = 0x04,
    VOL_Circle = 0x05,
    VOL_Line = 0x06,
};

void JPADynamicsBlock::init() {
    switch (getVolumeType()) {
    case VOL_Cube:
        mpCalcVolumeFunc = &JPAVolumeCube;
        break;
    case VOL_Sphere:
        mpCalcVolumeFunc = &JPAVolumeSphere;
        break;
    case VOL_Cylinder:
        mpCalcVolumeFunc = &JPAVolumeCylinder;
        break;
    case VOL_Torus:
        mpCalcVolumeFunc = &JPAVolumeTorus;
        break;
    case VOL_Point:
        mpCalcVolumeFunc = &JPAVolumePoint;
        break;
    case VOL_Circle:
        mpCalcVolumeFunc = &JPAVolumeCircle;
        break;
    case VOL_Line:
        mpCalcVolumeFunc = &JPAVolumeLine;
        break;
    }
}

void JPADynamicsBlock::create(JPAEmitterWorkData* work) {
    if (work->mpEmtr->checkStatus(JPAEmtrStts_RateStepEmit)) {
        s32 emitCount;
        s32 createCount;
#ifdef TARGET_PC
        f32 portGrow = 1.0f;  // (boss23 round 2: the size factor of this turn's new particles, PETARI_BOSS_PTCL_GROW)
#endif

        // Probably an inlined function.
        if (work->mpEmtr->checkFlag(JPADynFlag_FixedInterval)) {
            s32 count;
            if (getVolumeType() == VOL_Sphere) {
                count = 4 * getDivNumber() * getDivNumber() + 2;
            } else {
                count = getDivNumber();
            }
            emitCount = count;

            work->mVolumeEmitIdx = 0;
        } else {
            f32 rateRndm = getRateRndm();
            JPABaseEmitter* emtr = work->mpEmtr;
            f32 newPtclCount = emtr->mRate * (rateRndm * emtr->get_r_zp() + 1.0f);
#ifdef TARGET_PC
            if (port_pb_ptcl_turn_factor() < 1.0f) {
                const f32 pbScale = port_pb_ptcl_scale_at(emtr->mGlobalTrs.x, emtr->mGlobalTrs.y, emtr->mGlobalTrs.z);
                if (pbScale < 1.0f) {
                    newPtclCount *= pbScale;
                    port_logic_count(13, 1);
                }
            }
            if (g_port_boss_ptcl_scale < 1.0f) {
                newPtclCount *= g_port_boss_ptcl_scale;
                portGrow = g_port_boss_ptcl_grow;
            }
#endif
            f32 newEmitCount = emtr->mEmitCount + newPtclCount;
            emtr->mEmitCount = newEmitCount;
            emitCount = (s32)newEmitCount;
            work->mpEmtr->mEmitCount -= emitCount;

            if (work->mpEmtr->checkStatus(JPAEmtrStts_FirstEmit) && 0.0f < newPtclCount && newPtclCount < 1.0f)
                emitCount = 1;
        }

        work->mEmitCount = emitCount;
        if (work->mpEmtr->checkStatus(JPAEmtrStts_StopEmit)) {
            emitCount = 0;
        }

        // Probably an inlined function.
        createCount = emitCount;
        while (createCount > 0) {
            JPABaseParticle* ptcl = work->mpEmtr->createParticle();
            if (ptcl == NULL)
                break;
#ifdef TARGET_PC
            if (portGrow != 1.0f) {
                // fewer, larger particles (the base size: the scale animations and the children start from it)
                ptcl->mScaleOut *= portGrow;
                ptcl->mParticleScaleX *= portGrow;
                ptcl->mParticleScaleY *= portGrow;
            }
#endif
            createCount--;
        }
    }

    JPABaseEmitter* emtr = work->mpEmtr;
    if (++emtr->mRateStepTimer >= (emtr->mRateStep + 1)) {
        work->mpEmtr->mRateStepTimer -= (work->mpEmtr->mRateStep + 1);
        work->mpEmtr->setStatus(JPAEmtrStts_RateStepEmit);
    } else {
        work->mpEmtr->clearStatus(JPAEmtrStts_RateStepEmit);
    }

    work->mpEmtr->clearStatus(JPAEmtrStts_FirstEmit);
}
