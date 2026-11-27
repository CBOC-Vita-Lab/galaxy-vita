#include "Game/Util/FurCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/FurDrawer.hpp"
#include "Game/Util/FurMulti.hpp"
#include "Game/Util/FurShader.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef TARGET_PC
#include "Game/LiveActor/LiveActor.hpp"
extern "C" void port_logic_count(int what, unsigned n);
extern "C" float port_pb_fur_dist(void);                                // (platform/src/port/perf.cpp, GXV_PB_FUR)
extern "C" float port_pb_cam_dist2(float x, float y, float z);          // (decomp/src/Game/LiveActor/LodCtrl.cpp)
extern "C" float port_popin_zoom(void);                                // (popin24: LodCtrl.cpp)
extern "C" float port_pb_scale(void);                                  // (popin25: LodCtrl.cpp; the zoom ratio times GXV_PB_DEMO's widening in a flight)
extern "C" int port_pb_demo_off(void);                                 // (popin25: GXV_PB_DEMO=2 in a flight: the budget is off)
extern "C" float port_pb_lead(float limit);                            // (popin25: GXV_POPIN_LEAD: appear earlier for a fast camera)
extern "C" void port_popin_event(int kind, int appear, const LiveActor* pActor, const TVec3f* pPos, float radius);
#include "port/popin.h"
#include "port/popin_site.h"
#include "port/detail.h"
extern "C" int port_detail_fur_target(float x, float y, float z, int prevTgt, int numLayers);
extern "C" int port_detail_frozen(void);
#include "port/fur25.h"
extern "C" int port_safe_cam_exists(void);                              // (LodCtrl.cpp)
extern "C" float port_pb_player_dist2(float x, float y, float z);       // (LodCtrl.cpp: 0 when no player is known)
extern "C" void port_log_summary(const char* fmt, ...);
#include "Game/Util/CameraUtil.hpp"
#include <chrono>
extern "C" void port_detail_fur_changed(void);
extern "C" float port_detail_dist(int rule, float base);
#endif

FurMulti* FurBank::check(J3DModelData* pModelData, u32 layer) {
    for (u32 i = 0; i < mCount; i++) {
        if (mEntries[i]->mModel->getModelData() == pModelData && ((1 << layer) & mLayerMasks[i])) {
            return mEntries[i];
        }
    }

    return nullptr;
}

void FurDrawManager::add(FurCtrl* pCtrl, u8 index) {
    if (mCapacity != mCounts[index]) {
        mFurCtrls[index][mCounts[index]] = pCtrl;
        mCounts[index]++;
    }
}

#ifdef TARGET_PC
// reflfur25: the picture counter (one FurDrawManager::draw a picture), the parameters of the level (read again when a stage's switches change) and
// the census (GXV_FURCENSUS=1: a line every 5 s)
namespace {
    u32 sF25Pic = 1;
    port::fur25::Params sF25P = port::fur25::ParamsFor(1);
    u32 sF25Gen = 0xFFFFFFFFu;
    int sF25Level = 0;
    port::fur25::Census sF25Census;
    std::chrono::steady_clock::time_point sF25T0;
    bool sF25T0Set = false;
    u32 sFloorCostTick = 0;  // (popin31b: the sample counter of the coats the fur floor kept, timed for the census's price line)
}  // namespace
extern "C" int64_t port_host_time_ns(void);
#endif

void FurDrawManager::draw() const {
#ifdef TARGET_PC
    ++sF25Pic;
    if (sF25Gen != port::stageknob::Gen()) {
        sF25Gen = port::stageknob::Gen();
        sF25Level = port::fur25::Level();
        sF25P = port::fur25::CurrentParams(sF25Level > 0 ? sF25Level : 1);
    }
    sF25Level = port::fur25::Level();
    if (port::fur25::CensusOn()) {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (!sF25T0Set) {
            sF25T0 = now;
            sF25T0Set = true;
        }
        ++sF25Census.pictures;
        const double secs = std::chrono::duration< double >(now - sF25T0).count();
        if (secs >= 5.0) {
            char line[1200];
            port::fur25::Format(line, sizeof line, sF25Census, sF25Level, secs);
            port_log_summary("%s", line);
            sF25Census = port::fur25::Census();
            sF25T0 = now;
        }
    }
#endif
    for (u32 i = 0; i < mCounts[1]; i++) {
        if (!MR::isClipped(mFurCtrls[1][i]->mActor) && !MR::isDead(mFurCtrls[1][i]->mActor) && !MR::isHiddenModel(mFurCtrls[1][i]->mActor)) {
            if (mFurCtrls[1][i]->mVisible == 1) {
                mFurCtrls[1][i]->drawFur();
            }
        }
    }

    GXSetClipMode(GX_CLIP_ENABLE);
}

FurCtrl::FurCtrl(LiveActor* pActor, FurParam* pParam, bool addToManager, u8 drawOrder) : mParam(pParam), mVisible(), mFurUVScale(1.0f) {
    if (addToManager) {
        MR::getFurDrawManager()->add(this, drawOrder);
    }

    mActor = pActor;
    mModel = nullptr;
    mShapeIndex = -1;
    mNumLayers = 0;
    mDrawer = nullptr;
    mShader = nullptr;
    mLayerModels = nullptr;
    mLengthMap = nullptr;
    mBodyMap = nullptr;
    mIndirectMap = nullptr;
    mIsClone = 0;
    mDynamicParam.mFogCtrl = nullptr;
    mDynamicParam.mLightParam = nullptr;
}

void FurCtrl::calcLayerForm() {
    f32 rate = 1.0f;

    mDrawer->mFurUVScale = mParam->mFurUVScale;
    mDrawer->mBodyUVScale = mParam->mBodyUVScale;
    mDrawer->mLength.mTip = mParam->mLength;
    mDrawer->mLength.mRoot = 0.0f;
    mDrawer->mLength.mCurve = mParam->mLengthCurve;
    mDrawer->mNumLayers = mParam->mNumLayers;

    if (mDrawer->mNumLayers > mNumLayers) {
        mDrawer->mNumLayers = mNumLayers;
    }

    for (s32 i = 0; i < mDrawer->mNumLayers; i++) {
        const FurDrawer* pDrawer = mDrawer;
        f32 length = pDrawer->mLength.calcValue(i, pDrawer->mNumLayers);
        mShader->_8 = pow(rate * i / mDrawer->mNumLayers, 4.0);
        mLayerModels[i]->mVertexBuffer.frameInit();
        mShader->_1C = length;
        mShader->calc(mLayerModels[i]);
    }
}

#ifdef TARGET_PC
// GXV_DRAWPIPE (dpengine 2; DrawBufferHolder.cpp): a fur draw is a draw list like any other (it reads the actor, its light and
// the camera on the game thread and sends ordinary GX calls: recorded when the draw is recorded), with two things a J3D pass
// that comes BEFORE it in the serial order must not see once the pass is executed later, on the draw thread:
//   - it writes its shape's matrix-index words (below) and does not put them back: the words as they were are handed to the
//     pipeline first, which gives them to the passes that were asked for before this draw;
//   - for the length of its layers it points the model's own shape packet at its layer models (they share the packets): the
//     pipeline is told, for a pass executed in the middle of it (a recording ended there).
// Nothing happens in either call while no pass waits (GXV_DRAWPIPE 0, or a draw that is not being recorded).
// (The first version ended the recording at every fur draw and kept the next 60 turns serial: Honeyhive has fur in every picture.)
extern "C" void port_drawpipe_shape_words(void* pShape, u32 a, u32 b);
extern "C" void port_drawpipe_packet_lent(void* pPacket, void* pRealModel);
#endif
void FurCtrl::drawFur() {
    J3DModelData* pModelData = mModel->getModelData();

    if (MR::getJ3DModel(mActor)->getShapePacket(mShapeIndex)->checkFlag(0x10)) {
        return;
    }

    if (pModelData->getShapeNodePointer(mShapeIndex)->checkFlag(1)) {
        return;
    }

    if (mDynamicParam.mLightParam->mLightType == -1) {
        if (MR::getLightCtrl(mActor) != nullptr) {
            MR::loadActorLight(mActor);
        }
    } else {
        MR::loadLight(mDynamicParam.mLightParam->mLightType);
    }

    mDrawer->mFurUVScale = mParam->mFurUVScale * mFurUVScale;
    mDrawer->mBodyUVScale = mParam->mBodyUVScale;
    mDrawer->mColor = mParam->mColor;
    mDrawer->mIndirect.mTip = mParam->mIndirect;
    mDrawer->mIndirect.mRoot = 0.0f;
    mDrawer->mIndirect.mCurve = mParam->mIndirectCurve;
    mDrawer->mBrightness.mTip = mParam->mBrightnessTip;
    mDrawer->mBrightness.mRoot = mParam->mBrightnessRoot;
    mDrawer->mBrightness.mCurve = mParam->mBrightnessCurve;
    mDrawer->mAlpha.mTip = mParam->mAlphaTip;
    mDrawer->mAlpha.mRoot = mParam->mAlphaRoot;
    mDrawer->mAlpha.mCurve = mParam->mAlphaCurve;
    mDrawer->mColorBlend.mTip = mParam->mSkinAlphaTip;
    mDrawer->mColorBlend.mRoot = mParam->mSkinAlphaRoot;
    mDrawer->mColorBlend.mCurve = mParam->mSkinAlphaCurve;
    mDrawer->update();
    mDrawer->setupMaterial(&mDynamicParam);

    if (mDrawer->mMixFog) {
        mDrawer->mMixFog--;
    }

    J3DShape* pShape = pModelData->getShapeNodePointer(mShapeIndex);
#ifdef TARGET_PC
    port_drawpipe_shape_words(pShape, 0x3C921780, 0x00F3CF3C);
    port_drawpipe_packet_lent(&mModel->mShapePacket[mShapeIndex], mModel);
#endif
    pShape->mCurrentMtx.mMtxIdxRegA = 0x3C921780;
    pShape->mCurrentMtx.mMtxIdxRegB = 0x00F3CF3C;
    J3DShape::sOldVcdVatCmd = nullptr;
    J3DShapePacket* pPacket;

#ifdef TARGET_PC
    // GXV_PB_FUR (Performance-mode visual budget; 2500 units in Performance mode, 0 = off): fur that is farther than the distance from
    // the camera (planets and the like; never the player's, light type 0) is drawn with every second layer, the tip layer always: half
    // the layer draws (each a full draw of the shape) for a slightly thinner, paler pelt at a distance where a layer is a pixel or two.
    bool pbThin = false;
    int64_t floorT0 = 0;  // (popin31b: a coat the fur floor kept, timed: the shell loop's start)
    // GXV_FUR25 (reflfur25, port/fur25.h): the shells follow the fur's size on the screen, the dropped ones' alpha merged into the kept ones; replaces
    // GXV_DETAIL_FUR and GXV_PB_FUR for the objects it handles (the player's own fur, light type 0, never). The census (GXV_FURCENSUS=1) measures the same.
    bool f25Own = false;   // GXV_FUR25 decides this object's shells this picture (also when it keeps them all)
    bool f25Thin = false;  // ... and it keeps fewer than all of them
    u8 f25Flags[port::fur25::kMaxShells];
    bool f25Metrics = false;
    f32 f25Nearest = 0.0f, f25Px = 0.0f;
    int f25Drawn = 0;
    {
        const s32 nl = mDrawer->mNumLayers;
        const bool want25 = sF25Level > 0 && nl > 2 && nl <= port::fur25::kMaxShells && mDynamicParam.mLightParam->mLightType != 0 && !port_pb_demo_off();
        if ((want25 || port::fur25::CensusOn()) && nl >= 1 && nl <= port::fur25::kMaxShells && port_safe_cam_exists()) {
            const TVec3f& fp = mActor->mPosition;
            f32 scale = __builtin_fabsf(mActor->mScale.x);
            if (__builtin_fabsf(mActor->mScale.y) > scale) {
                scale = __builtin_fabsf(mActor->mScale.y);
            }
            if (__builtin_fabsf(mActor->mScale.z) > scale) {
                scale = __builtin_fabsf(mActor->mScale.z);
            }
            if (mF25Radius < 0.0f) {  // (once: the fur shape's box as a sphere around the model's origin; 0 = no box known)
                const Vec* mn = pShape->getMin();
                const Vec* mx = pShape->getMax();
                const f32 cx = (mn->x + mx->x) * 0.5f, cy = (mn->y + mx->y) * 0.5f, cz = (mn->z + mx->z) * 0.5f;
                const f32 hx = (mx->x - mn->x) * 0.5f, hy = (mx->y - mn->y) * 0.5f, hz = (mx->z - mn->z) * 0.5f;
                mF25Radius = (__builtin_sqrtf(cx * cx + cy * cy + cz * cz) + __builtin_sqrtf(hx * hx + hy * hy + hz * hz)) * scale;
            }
            if (mF25Radius > 0.0f) {
                const f32 camD = __builtin_sqrtf(port_pb_cam_dist2(fp.x, fp.y, fp.z));
                const f32 plD = __builtin_sqrtf(port_pb_player_dist2(fp.x, fp.y, fp.z));
                f25Nearest = port::fur25::NearestDist(camD, mF25Radius);
                const f32 nearestPlayer = port::fur25::NearestDist(plD, mF25Radius);
                const f32 tip = mDrawer->mLength.calcValue(nl - 1, nl) * scale;
                f25Px = tip * port::fur25::PixelsPerUnit(port::fur25::Lines(), MR::getFovy(), f25Nearest);
                f25Metrics = true;
                if (want25) {
                    port::fur25::State st;
                    st.keep = mF25Keep;
                    st.wait = mF25Wait;
                    st.lastPic = mF25LastPic;
                    const int keep = port::fur25::Step(st, sF25Pic, nl, port::fur25::FullRule(f25Nearest, nearestPlayer, sF25P),
                                                       port::fur25::Supported(f25Px, sF25P.gapPx), sF25P);
                    mF25Keep = st.keep;
                    mF25Wait = st.wait;
                    mF25LastPic = st.lastPic;
                    f25Own = true;
                    if (keep < nl) {
                        f25Thin = true;
                        port::fur25::KeepFlags(nl, keep, f25Flags);
                    }
                }
            }
        }
    }
    // GXV_DETAIL_FUR (detail25): layers by distance and the detail level, with a fade (below); replaces the rule that follows while it is on
    bool dFur = false;
    int dFrom = 0, dTo = 0;
    f32 dT = 1.0f;
    if (!f25Own && port::detail::FurMode() != 0 && mDrawer->mNumLayers > 2 && mDynamicParam.mLightParam->mLightType != 0 && !port_pb_demo_off()) {
        const TVec3f& dPos = mActor->mPosition;
        const int tgt = port_detail_fur_target(dPos.x, dPos.y, dPos.z, mDetailTgt, mDrawer->mNumLayers);
        if (port::detail::FurMode() == 1) {
            mDetailTgt = static_cast< u8 >(tgt);
            if (!mDetailInit) {
                mDetailInit = 1;
                mDetailFrom = mDetailTo = static_cast< u8 >(tgt);  // (first drawn thin: nothing to fade from)
                mDetailT = 30;
            } else if (mDetailT >= 30 && tgt != mDetailTo && !port_detail_frozen()) {
                mDetailFrom = mDetailTo;
                mDetailTo = static_cast< u8 >(tgt);
                mDetailT = 0;
                port_detail_fur_changed();
            }
            if (mDetailT < 30) {
                ++mDetailT;
            }
            dFur = true;
            dFrom = mDetailFrom;
            dTo = mDetailTo;
            dT = static_cast< f32 >(mDetailT) / 30.0f;
        }
    }
    if (!dFur && !f25Own) {
        const f32 pbFurBase = port_pb_fur_dist();
        const f32 pbFur = pbFurBase > 0.0f ? port_detail_dist(2, pbFurBase) : 0.0f;
        if (pbFur > 0.0f && mDrawer->mNumLayers > 2 && mDynamicParam.mLightParam->mLightType != 0 && !port_pb_demo_off()) {
            const TVec3f& pbPos = mActor->mPosition;
            // (popin24) GXV_POPIN_ZOOM scales the distance with the zoom ratio; GXV_POPIN_HYST: thinned fur returns to full popin-pct nearer
            const bool was = mPbThin != 0;
            // fur31: the distance to the fur's surface (the shape's bound: radius * scale added to the limit), a band wider than a governor step on the way
            // back, and the base distance latched while a governor step would visibly flip this coat (port/fur25.h PbThin)
            const bool surf = port::fur25::PbSurfaceOn() != 0;
            const f32 pct = port::popin::HystPct();
            const TVec3f& sc3 = mActor->mScale;
            const f32 rad = surf ? pShape->mRadius : 0.0f;
            const f32 pbD2 = port_pb_cam_dist2(pbPos.x, pbPos.y, pbPos.z);
            const f32 centreD = __builtin_sqrtf(pbD2);
            auto thinFor = [&](f32 base) {
                const f32 lim = base * port_pb_scale() + port_pb_lead(base * port_pb_scale());  // (popin25: the flight's widening, GXV_POPIN_LEAD's margin)
                if (!surf) {
                    f32 r = lim;
                    if (was) r -= port::popin::Band(lim, pct);
                    return pbD2 > r * r;
                }
                return port::fur25::PbThinAt(was, centreD, lim, pct, rad, sc3.x, sc3.y, sc3.z);
            };
            if (surf) {
                // free to change at once: under the pop-in floor (GXV_PB_MINPX: too small to see), else only after it was not drawn for a few pictures
                bool freeNow = false;
                // (popin31b: the furs' own floor, GXV_PB_MINPX_FUR)
                if (__builtin_expect(port::popin::GuardOn(port::popin::kFlFur), 0) && port_pb_floor.kHeldK[port::popin::kFlFur] > 0.0f && rad > 0.0f) {
                    f32 sc = __builtin_fabsf(sc3.x);
                    if (__builtin_fabsf(sc3.y) > sc) sc = __builtin_fabsf(sc3.y);
                    if (__builtin_fabsf(sc3.z) > sc) sc = __builtin_fabsf(sc3.z);
                    freeNow = port::popin::FloorMayCut2K(port_pb_floor, port::popin::kFlFur, rad * sc, pbD2, was);
                }
                port::fur25::PbLatch latch;
                latch.base = mPbBase;
                latch.pic = mPbBasePic;
                pbThin = port::fur25::PbThin(latch, sF25Pic, pbFur, freeNow, thinFor);
                mPbBase = latch.base;
                mPbBasePic = latch.pic;
            } else {
                pbThin = thinFor(pbFur);
            }
            if (pbThin && __builtin_expect(port::popin::GuardOn(port::popin::kFlFur), 0)) {
                // popin31: GXV_PB_MINPX: the pelt is thinned only while the furred thing (its fur shape's box as a sphere round the model's origin, at its scale) is
                // under the floor on the picture; GXV_PB_DEMO=2's release after a flight: a pelt in the picture is not thinned
                if (mPbFloorR0 < 0.0f) {
                    const Vec* mn = pShape->getMin();
                    const Vec* mx = pShape->getMax();
                    const f32 cx = (mn->x + mx->x) * 0.5f, cy = (mn->y + mx->y) * 0.5f, cz = (mn->z + mx->z) * 0.5f;
                    const f32 hx = (mx->x - mn->x) * 0.5f, hy = (mx->y - mn->y) * 0.5f, hz = (mx->z - mn->z) * 0.5f;
                    mPbFloorR0 = __builtin_sqrtf(cx * cx + cy * cy + cz * cz) + __builtin_sqrtf(hx * hx + hy * hy + hz * hz);
                }
                const TVec3f& s = mActor->mScale;
                f32 sc = __builtin_fabsf(s.x);
                if (__builtin_fabsf(s.y) > sc) sc = __builtin_fabsf(s.y);
                if (__builtin_fabsf(s.z) > sc) sc = __builtin_fabsf(s.z);
                const f32 r = mPbFloorR0 > 0.0f ? mPbFloorR0 * sc : 100000.0f;   // (no box known: never under the floor)
                // (popin31b: the furs' own floor, GXV_PB_MINPX_FUR; a coat the floor kept has its shell loop timed for the census's price line)
                const int v = port::popin::CutVerdict2(port::popin::kFur, was, r, pbD2, pbPos.x, pbPos.y, pbPos.z);
                pbThin = v == port::popin::kCutOk;
                if (v == port::popin::kCutFloor && port::popin::CostSample(sFloorCostTick)) {
                    floorT0 = port_host_time_ns();
                }
            }
            if (pbThin != was) {
                mPbThin = pbThin ? 1 : 0;
                if (port::popin::LogOn()) {
                    port_popin_event(port::popin::kFur, pbThin ? 0 : 1, mActor, &pbPos, -1.0f);
                }
            }
            if (pbThin) {
                port_logic_count(15, 1);
                port_logic_count(16, static_cast< unsigned >((mDrawer->mNumLayers - 1) / 2));
            }
        }
    }
#endif
    for (s32 i = 0; i < mDrawer->mNumLayers; i++) {
#ifdef TARGET_PC
        if (pbThin && (i & 1) && i != mDrawer->mNumLayers - 1) {
            continue;
        }
        if (f25Thin) {
            // GXV_FUR25: only the kept shells, each with the merged constants of the group it heads (a group of one: the game's own setup)
            if (!f25Flags[i]) {
                continue;
            }
            const s32 lo = port::fur25::GroupLo(f25Flags, i);
            if (lo == i) {
                mDrawer->setupLayerMaterial(i);
            } else {
                port::fur25::Konst group[port::fur25::kMaxShells];
                for (s32 m = lo; m <= i; m++) {
                    mDrawer->calcLayerKonst(m, &group[m].r, &group[m].g, &group[m].b);
                }
                const port::fur25::Konst merged = port::fur25::Merge(group, lo, i);
                mDrawer->setupLayerMaterialKonst(i, merged.r, merged.g, merged.b);
            }
            pPacket = &mLayerModels[i]->mShapePacket[mShapeIndex];
            pPacket->setModel(mLayerModels[i]);
            pPacket->prepareDraw();
            pPacket->getShape()->drawFast();
            ++f25Drawn;
            continue;
        }
#endif
#ifdef TARGET_PC
        if (dFur) {
            const f32 w = port::detail::FurWeight(dFrom, dTo, dT, i, mDrawer->mNumLayers, port::detail::FurFarLayers(), port::detail::FurGainExp());
            if (w < 0.03f) {
                continue;
            }
            mDrawer->setupLayerMaterial(i, w);
        } else {
            mDrawer->setupLayerMaterial(i);
        }
#else
        mDrawer->setupLayerMaterial(i);
#endif
        pPacket = &mLayerModels[i]->mShapePacket[mShapeIndex];
        pPacket->setModel(mLayerModels[i]);
        pPacket->prepareDraw();
        pPacket->getShape()->drawFast();
#ifdef TARGET_PC
        ++f25Drawn;
#endif
    }
#ifdef TARGET_PC
    if (floorT0 != 0 && mDrawer->mNumLayers > 0) {
        // popin31b: the census's price line: the shells the cut would have left out (every second one, the tip kept: (n - 1) / 2 of n) as their share of the
        // timed loop (the front end's part of a shell draw; the GPU's fill is priced by the one-kind console arm)
        const s32 nl = mDrawer->mNumLayers;
        const int64_t dt = port_host_time_ns() - floorT0;
        port_pb_floor_cost(port::popin::kFlFur, static_cast< u32 >(dt * ((nl - 1) / 2) / nl));
    }
    if (f25Metrics && port::fur25::CensusOn()) {
        sF25Census.Add(f25Nearest, f25Px, mDrawer->mNumLayers, f25Drawn, f25Thin ? mDrawer->mNumLayers - f25Drawn : 0);
    }
#endif

    mModel->mShapePacket[mShapeIndex].setModel(mModel);
#ifdef TARGET_PC
    port_drawpipe_packet_lent(nullptr, nullptr);
#endif
    GXColor fogColor = {0, 0, 0, 0};
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, fogColor);
    J3DShape::sOldVcdVatCmd = nullptr;
    GXSetClipMode(GX_CLIP_ENABLE);
}

void FurCtrl::createFurMap() {
    if (mIsClone) {
        return;
    }

    for (u32 i = 0; i < 4; i++) {
        mDrawer->mDensity[i] = mParam->mDensity[i];
        mDrawer->mThickness[i] = mParam->mThickness[i];
        mDrawer->mLengthMap[i] = mParam->mLengthMap[i];
    }

    mDrawer->createFurMap();
}

namespace MR {
    FurDrawManager* getFurDrawManager() {
        return static_cast< FurDrawManager* >(getSceneObjHolder()->getObj(SceneObj_FurDrawManager));
    }
}  // namespace MR

FurDrawManager::FurDrawManager(u8 capacity) : NameObj("\x83\x74\x83\x40\x81\x5b\x95\x60\x89\xe6\x83\x7d\x83\x6c\x81\x5b\x83\x57\x83\x83") {
    mCounts[0] = 0;
    mCounts[1] = 0;
    mFurCtrls[0] = new FurCtrl*[capacity];
    mFurCtrls[1] = new FurCtrl*[capacity];
    mCapacity = capacity;
    mBank = new FurBank;
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_Fur);
}

void FurCtrl::setupFur(J3DModel* pModel, ResTIMG* pLength, ResTIMG* pIndirect, ResTIMG* pBody, u16 shape, u8 numLayers) {
    J3DModelData* pData = pModel->getModelData();
    mModel = pModel;
    mLengthMap = pLength;
    mBodyMap = pBody;
    mIndirectMap = pIndirect;
    mShapeIndex = shape;
    mShader = new CShader(pData, pLength);
    mShader->makeIndexData(pData->getShapeNodePointer(shape));
    mShader->checkBorderVtx(pData, shape);
    mDrawer = new FurDrawer(numLayers, pBody, pIndirect);
    s32 componentSize;

    switch (mShader->_24) {
    case GX_S16:
        componentSize = sizeof(s16);
        break;
    case GX_F32:
        componentSize = sizeof(f32);
        break;
    }

    mNumLayers = numLayers;
    mLayerModels = new J3DModel*[numLayers];

    for (s32 i = 0; i < mNumLayers; i++) {
        char name[16];
        sprintf(name, "\x83\x8c\x83\x43\x83\x84 %d", i);
        J3DModel2* pLayer = new J3DModel2(mModel);
        pLayer->setVtxShader(mShader, componentSize);
        mLayerModels[i] = pLayer;
    }

    mVisible = 1;
}

void FurCtrl::setupFurClone(J3DModel* pModel, FurCtrl* pOther) {
    mModel = pModel;
    mIsClone = 1;
    mLengthMap = pOther->mLengthMap;
    mBodyMap = pOther->mBodyMap;
    mIndirectMap = pOther->mIndirectMap;
    mShapeIndex = pOther->mShapeIndex;
    mShader = nullptr;
    mDrawer = pOther->mDrawer;
    mNumLayers = pOther->mNumLayers;
    mLayerModels = new J3DModel*[mNumLayers];

    for (u32 i = 0; i < mNumLayers; i++) {
        char name[32];
        sprintf(name, "\x83\x8c\x83\x43\x83\x84(\x83\x52\x83\x73\x81\x5b) %d", i);
        J3DModel2* pLayer = new J3DModel2(mModel);
        *pLayer->getVertexBuffer() = *pOther->mLayerModels[i]->getVertexBuffer();
        mLayerModels[i] = pLayer;
    }

    mVisible = 1;
}

namespace MR {
    void initFurParamFromDvd(FurParam* pParam, DynamicFurParam* pDynamic, char* pText, u32 size) {
        char line[256];
        FurLightParam* pLight = pDynamic->mLightParam;
        s32 lightMask = 0;
        s32 matIntensity = 0;
        s32 ambIntensity = 0;
        s32 specularLightMask = 0;
        s32 specularMatIntensity = 0;
        s32 specularAmbIntensity = 0;
        s32 colorSource = 0;
        char* pRead = pText;
        u32 position = 0;

        while (true) {
            if (position >= size) {
                break;
            }

            u32 length = 0;

            while (true) {
                if (*pRead == '\n' || *pRead == '\r') {
                    break;
                }

                position++;
                line[length] = *pRead;
                length++;
                pRead++;

                if (position >= size) {
                    break;
                }
            }

            line[length] = '\0';

            while (true) {
                if (position >= size) {
                    break;
                }

                if (*pRead != '\n' && *pRead != '\r') {
                    break;
                }

                pRead++;
                position++;
            }

            scan32(line, "\x83\x8c\x83\x43\x83\x84\x90\x94", &pParam->mNumLayers);
            scanf32(line, "\x96\xd1\x92\xb7\x82\xb3", &pParam->mLength);
            scanf32(line, "\x92\xb7\x82\xb3\x95\xce\x8d\xb7", &pParam->mLengthCurve);
            scanf32(line, "\x83\x59\x83\x8c(indirect)", &pParam->mIndirect);
            scanf32(line, "\x83\x59\x83\x8c\x95\xce\x8d\xb7", &pParam->mIndirectCurve);
            scanf32(line, "\x96\xbe\x82\xe9\x82\xb3(\x96\xd1\x90\xe6)", &pParam->mBrightnessTip);
            scanf32(line, "\x96\xbe\x82\xe9\x82\xb3(\x96\xd1\x8c\xb3)", &pParam->mBrightnessRoot);
            scanf32(line, "\x96\xbe\x82\xe9\x82\xb3\x95\xce\x8d\xb7", &pParam->mBrightnessCurve);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78(\x96\xd1\x90\xe6)", &pParam->mAlphaTip);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78(\x96\xd1\x8c\xb3)", &pParam->mAlphaRoot);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78\x95\xce\x8d\xb7", &pParam->mAlphaCurve);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78\x81\x45\x92\x6e\x94\xa7(\x96\xd1\x90\xe6)", &pParam->mSkinAlphaTip);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78\x81\x45\x92\x6e\x94\xa7(\x96\xd1\x8c\xb3)", &pParam->mSkinAlphaRoot);
            scanf32(line, "\x93\xa7\x96\xbe\x93\x78\x81\x45\x92\x6e\x94\xa7\x95\xce\x8d\xb7", &pParam->mSkinAlphaCurve);
            scanf32(line, "\x96\xa7\x93\x78\x83\x7d\x83\x62\x83\x76\x83\x58\x83\x50\x81\x5b\x83\x8b", &pParam->mFurUVScale);
            scanf32(line, "\x83\x78\x81\x5b\x83\x58\x83\x7d\x83\x62\x83\x76\x83\x58\x83\x50\x81\x5b\x83\x8b", &pParam->mBodyUVScale);
            scanu8x4(line, "\x8d\xac\x8d\x87\x83\x4a\x83\x89\x81\x5b", &pParam->mColor.r);
            scanf32x4(line, "\x90\x41\x96\xd1\x96\xa7\x93\x78", pParam->mDensity);
            scanf32x4(line, "\x90\x41\x96\xd1\x91\xbe\x82\xb3", pParam->mThickness);
            scanu8x4(line, "\x8d\xac\x8d\x87\x94\xe4", pParam->mLengthMap);
            scan32(line, "\x83\x89\x83\x43\x83\x67""0\x83\x58\x83\x43\x83\x62\x83\x60", &lightMask);
            scan32(line, "\x83\x89\x83\x43\x83\x67""0\x83\x7d\x83\x65\x83\x8a\x83\x41\x83\x8b", &matIntensity);
            scan32(line, "\x83\x89\x83\x43\x83\x67""0\x83\x41\x83\x93\x83\x72\x83\x47\x83\x93\x83\x67", &ambIntensity);
            scan32(line, "\x83\x89\x83\x43\x83\x67""1\x83\x58\x83\x43\x83\x62\x83\x60", &specularLightMask);
            scan32(line, "\x83\x89\x83\x43\x83\x67""1\x83\x7d\x83\x65\x83\x8a\x83\x41\x83\x8b", &specularMatIntensity);
            scan32(line, "\x83\x89\x83\x43\x83\x67""1\x83\x41\x83\x93\x83\x72\x83\x47\x83\x93\x83\x67", &specularAmbIntensity);
            scan32(line, "\x83\x89\x83\x43\x83\x67\x83\x4a\x83\x89\x81\x5b\x83\x5c\x81\x5b\x83\x58", &colorSource);
            pLight->mLightMask = lightMask;
            pLight->mMatIntensity = matIntensity;
            pLight->mAmbIntensity = ambIntensity;
            pLight->mSpecularLightMask = specularLightMask;
            pLight->mSpecularMatIntensity = specularMatIntensity;
            pLight->mSpecularAmbIntensity = specularAmbIntensity;
            pLight->mColorSource = colorSource;
        }
    }
}  // namespace MR

J3DModel2::J3DModel2(J3DModel* pModel) {
    mModelData = pModel->mModelData;
    mMtxBuffer = pModel->mMtxBuffer;
    mShapePacket = pModel->mShapePacket;
    mMatPacket = pModel->mMatPacket;
    mVertexBuffer.setVertexData(&mModelData->mVertexData);
    mFlags |= J3DMdlFlag_UseDefaultJ3D;
}

J3DModel2::~J3DModel2() {
}

FurDrawManager::~FurDrawManager() {
}
