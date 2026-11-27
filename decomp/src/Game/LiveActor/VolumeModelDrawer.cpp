#include "Game/LiveActor/VolumeModelDrawer.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <cstdio>
#include "port/glow26.h"
#ifdef TARGET_PC
#include "port/lightvol.h"
#include "port/stage_knob.h"
#include "port/glow26.h"
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <cstdlib>
#include <cstring>
extern "C" void port_log_summary(const char* fmt, ...);
// The box of a drawer's model, for a host test that has no J3D data (null in the game).
extern "C" bool (*port_lightvol_box_hook)(const void* drawer, float* bmin, float* bmax);
bool (*port_lightvol_box_hook)(const void* drawer, float* bmin, float* bmax) = nullptr;
#endif

#ifdef TARGET_PC
static void portVolumeSetup() {
    MR::setupShadowVolumeDraw();
    VolumeModelDrawer::portBegin();
}
#endif

VolumeModelDrawInit::VolumeModelDrawInit() : NameObj("\x83\x7b\x83\x8a\x83\x85\x81\x5b\x83\x80\x83\x82\x83\x66\x83\x8b\x95\x60\x89\xe6\x8f\x89\x8a\xfa\x89\xbb") {
#ifdef TARGET_PC
    MR::registerPreDrawFunction(MR::Functor(&portVolumeSetup), MR::DrawType_VolumeModel);
#else
    MR::registerPreDrawFunction(MR::Functor(&MR::setupShadowVolumeDraw), MR::DrawType_VolumeModel);
#endif
}

VolumeModelDrawer::VolumeModelDrawer(const char* pName, const char* pFileName, MtxPtr mtx)
    : NameObj(pName), mMtx(), mModelData(), mColor(0xFF, 0xA9, 0, 0xFF) {
    MR::createSceneObj(SceneObj_VolumeModelDrawInit);
    mMtx = mtx;
    char buf[0x100];
    snprintf(buf, sizeof(buf), "%s.arc", pFileName);
    mModelData = (J3DModelData*)MR::createAndAddResourceHolder(buf)->mModelResTable->getRes(pFileName);
#ifdef TARGET_PC
    snprintf(mLvName, sizeof(mLvName), "%s", pFileName);
#endif
}

void VolumeModelDrawer::draw() const {
#ifdef TARGET_PC
    if (portRecord()) {  // GXV_LIGHTVOL (lightvol25): drawn with the whole list by flushDeferred
        return;
    }
#endif
    MR::loadProjectionMtx();
    loadModelDrawMtx();
    GXSetCurrentMtx(0);
    GXSetColorUpdate(0);
    GXSetAlphaUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXColor color_1 = mColor;
    GXSetTevColor(GX_TEVREG0, color_1);
    GXSetZMode(0, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXSetDstAlpha(0, 0);
    GXSetZMode(1, GX_GEQUAL, 0);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXSetCullMode(GX_CULL_BACK);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
    MR::drawSimpleModel(mModelData);
    GXColor color_2 = mColor;
    GXSetTevColor(GX_TEVREG0, color_2);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ONE, GX_LO_COPY);
    GXSetColorUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetZMode(0, GX_ALWAYS, 0);
    MR::drawSimpleModel(mModelData);
}

void VolumeModelDrawer::loadModelDrawMtx() const {
    Mtx drawMtx;
    MR::multMtx(drawMtx, mMtx, MR::getCameraViewMtx().mMtx);
    GXLoadPosMtxImm(drawMtx, 0);
}

#ifdef TARGET_PC
// ---------------------------------------------------------------------------------------------------------------------------------------
// GXV_LIGHTVOL (lightvol25). Default 0: draw() above runs unchanged. Bits: 1 exact cull (a volume whose padded box is wholly off the screen or behind the eye draws no
// pixel), 2 pass-major drawing of groups of volumes whose screen rectangles are disjoint (the same picture: whole cycles commute, tests/lightvol_gl_test), 4 the LOD
// (size / nearest-K rank, smooth fade of the tint, port/lightvol.h), 8 the census line. Each volume's four passes (see port/lightvol.h) are the original's statements.
// A volume that is cut (f = 0) keeps only its alpha-clear pass (GXV_LIGHTVOL_CLEAR=1, the default): the alpha plane is then what the original leaves.
extern "C" int port_q30lock_step_on(int step);   // (q30fin26, perf.cpp: the Quality ladder holds this step)
namespace {
using namespace port::lightvol;

struct Rec {
    const VolumeModelDrawer* d;
    Mtx mv;  // model -> view
    GXColor col;
    J3DModelData* md;
};
Rec gRec[kMax];
int gN = 0;
unsigned gPic = 0;

int KnobMode() {
    int m = PORT_STAGE_KNOB(int, port::glow26::LightVolFloor(port::glow26::ClampLevel(port::stageknob::Int("GXV_GLOW26", 0)), static_cast<int>(port::stageknob::Int("GXV_LIGHTVOL", 0)) & 15)); // (glow26: level 1 adds exact cull + grouping, level 2 also the far fade)
    if (port_q30lock_step_on(5)) m |= 5;   /* (step 4 = bit 5 of the ladder's mask) */   // (q30fin26: the Quality ladder's step 4: far small light beams fade (the LOD bit; each volume slews 1/8 a picture, no jump) with the exact cull)
    return m;
}
int KnobKeep() { return PORT_STAGE_KNOB(int, port::stageknob::Int("GXV_LIGHTVOL_KEEP", 12)); }
float KnobPx() { return PORT_STAGE_KNOB(float, port::stageknob::Num("GXV_LIGHTVOL_PX", 2500.0f)); }
float KnobNear() { return PORT_STAGE_KNOB(float, port::stageknob::Num("GXV_LIGHTVOL_NEAR", 1500.0f)); }
int KnobClear() { return PORT_STAGE_KNOB(int, port::stageknob::Int("GXV_LIGHTVOL_CLEAR", 1)); }

// the four passes' state, exactly the original's statements
void State1(GXColor c) {
    GXSetCurrentMtx(0);
    GXSetColorUpdate(0);
    GXSetAlphaUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetTevColor(GX_TEVREG0, c);
    GXSetZMode(0, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
}
void State2(GXColor c) {
    GXSetTevColor(GX_TEVREG0, c);
    GXSetDstAlpha(0, 0);
    GXSetZMode(1, GX_GEQUAL, 0);
    GXSetCullMode(GX_CULL_FRONT);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ZERO, GX_LO_COPY);
}
void State3(GXColor c) {
    GXSetTevColor(GX_TEVREG0, c);
    GXSetDstAlpha(0, 0);
    GXSetZMode(1, GX_GEQUAL, 0);
    GXSetCullMode(GX_CULL_BACK);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY);
}
void State4(GXColor c) {
    GXSetTevColor(GX_TEVREG0, c);
    GXSetCullMode(GX_CULL_NONE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ONE, GX_LO_COPY);
    GXSetColorUpdate(1);
    GXSetDstAlpha(1, 0);
    GXSetZMode(0, GX_ALWAYS, 0);
}
void Pos(const Rec& r) { GXLoadPosMtxImm(const_cast< Mtx& >(r.mv), 0); }
void Draw(const Rec& r) { MR::drawSimpleModel(r.md); }
GXColor Scaled(GXColor c, float f) {
    if (f >= 1.0f) return c;
    c.r = static_cast< u8 >(c.r * f + 0.5f);
    c.g = static_cast< u8 >(c.g * f + 0.5f);
    c.b = static_cast< u8 >(c.b * f + 0.5f);
    return c;
}

// census
struct ModelStat { char name[28]; unsigned n, culled, cut; double area, dist; };
struct Census {
    unsigned pics, sumN, minN, maxN, culled, cutSize, cutRank, faded, full, drawsBefore, drawsAfter, groups;
    double px;
    ModelStat m[8];
    int nm;
};
Census gC = {};
void CensusAdd(const char* name, bool culled, bool cut, float area, float dist) {
    ModelStat* s = nullptr;
    for (int i = 0; i < gC.nm; ++i)
        if (std::strcmp(gC.m[i].name, name) == 0) { s = &gC.m[i]; break; }
    if (!s && gC.nm < 8) {
        s = &gC.m[gC.nm++];
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->n = s->culled = s->cut = 0;
        s->area = s->dist = 0;
    }
    if (!s) return;
    ++s->n;
    if (culled) ++s->culled;
    if (cut) ++s->cut;
    s->area += area < 0 ? 0 : area;
    s->dist += dist;
}
void CensusPrint() {
    if (gC.pics == 0) return;
    const double p = gC.pics;
    port_log_summary("lightvol: %u pictures, volumes/picture min %u max %u mean %.1f; (planned, all options on) culled exact %.1f/pic, cut by size %.1f, cut by rank %.1f, faded %.1f, full %.1f; "
                     "draws %.1f -> %.1f per picture, %.1f pass groups, pixel estimate %.0f px/pic (4 passes, 960x544)",
                     gC.pics, gC.minN, gC.maxN, gC.sumN / p, gC.culled / p, gC.cutSize / p, gC.cutRank / p, gC.faded / p, gC.full / p, gC.drawsBefore / p, gC.drawsAfter / p,
                     gC.groups / p, gC.px / p);
    for (int i = 0; i < gC.nm; ++i) {
        const ModelStat& s = gC.m[i];
        port_log_summary("lightvol:   model %s: %.1f/pic, mean size %.4f of the screen (%.0f px), mean distance %.0f, culled %.0f%%, cut %.0f%%", s.name, s.n / p,
                         s.n ? s.area / s.n : 0.0, s.n ? s.area / s.n * 522240.0 : 0.0, s.n ? s.dist / s.n : 0.0, s.n ? 100.0 * s.culled / s.n : 0.0,
                         s.n ? 100.0 * s.cut / s.n : 0.0);
    }
    std::memset(&gC, 0, sizeof(gC));
}
}  // namespace

bool VolumeModelDrawer::portBox(float* pMin, float* pMax) const {
    if (mLvBox == 0) {
        mLvBox = -1;
        float lo[3], hi[3];
        bool ok = false;
        if (port_lightvol_box_hook) {
            ok = port_lightvol_box_hook(this, lo, hi);
        } else if (mModelData && mModelData->getJointNum() > 0) {
            J3DJoint* j = mModelData->getJointNodePointer(0);
            if (j) {
                const Vec* a = j->getMin();
                const Vec* b = j->getMax();
                lo[0] = a->x; lo[1] = a->y; lo[2] = a->z;
                hi[0] = b->x; hi[1] = b->y; hi[2] = b->z;
                ok = true;
            }
        }
        // (a box that is not a box, or all zero, is no claim)
        if (ok && hi[0] > lo[0] && hi[1] > lo[1] && hi[2] > lo[2] && std::isfinite(lo[0] + lo[1] + lo[2] + hi[0] + hi[1] + hi[2])) {
            for (int k = 0; k < 3; ++k) { mLvMin[k] = lo[k]; mLvMax[k] = hi[k]; }
            mLvBox = 1;
        }
    }
    if (mLvBox != 1) return false;
    for (int k = 0; k < 3; ++k) { pMin[k] = mLvMin[k]; pMax[k] = mLvMax[k]; }
    return true;
}

bool VolumeModelDrawer::portRecord() const {
    if (KnobMode() == 0 || gN >= kMax || mMtx == nullptr || mModelData == nullptr) return false;
    Rec& r = gRec[gN++];
    r.d = this;
    MR::multMtx(r.mv, mMtx, MR::getCameraViewMtx().mMtx);
    r.col = mColor;
    r.md = mModelData;
    return true;
}

void VolumeModelDrawer::portBegin() { gN = 0; }

void VolumeModelDrawer::flushDeferred() {
    if (gN == 0) return;
    const int n = gN;
    gN = 0;
    const int mode = KnobMode();
    ++gPic;
    const bool doCull = (mode & 1) != 0, doBatch = (mode & 2) != 0, doLod = (mode & 4) != 0, doCensus = (mode & 8) != 0;

    Params prm;
    prm.keep = (doLod || doCensus) ? KnobKeep() : 0;
    prm.nearGuard = KnobNear();
    prm.areaCut = KnobPx() / 522240.0f;

    const TProj3f& pj = MR::getCameraProjectionMtx();
    Proj pr;
    pr.persp = pj.mMtx[3][2] == -1.0f;
    pr.p00 = pj.mMtx[0][0]; pr.p02 = pj.mMtx[0][2]; pr.p11 = pj.mMtx[1][1]; pr.p12 = pj.mMtx[1][2];
    const bool wantView = doCull || doBatch || doLod || doCensus;

    BoxView bv[kMax];
    Item it[kMax];
    Out out[kMax];
    Rect foot[kMax];
    bool active[kMax];
    int groupOf[kMax];
    float px = 0, py = 0, pz = 0;  // the player in view space
    if (wantView) {
        const TVec3f* pp = MR::getPlayerPos();
        if (pp) {
            const TPos3f& v = MR::getCameraViewMtx();
            px = v.mMtx[0][0] * pp->x + v.mMtx[0][1] * pp->y + v.mMtx[0][2] * pp->z + v.mMtx[0][3];
            py = v.mMtx[1][0] * pp->x + v.mMtx[1][1] * pp->y + v.mMtx[1][2] * pp->z + v.mMtx[1][3];
            pz = v.mMtx[2][0] * pp->x + v.mMtx[2][1] * pp->y + v.mMtx[2][2] * pp->z + v.mMtx[2][3];
        }
    }
    for (int i = 0; i < n; ++i) {
        const Rec& r = gRec[i];
        const VolumeModelDrawer* d = r.d;
        foot[i] = Rect();
        active[i] = true;
        it[i] = Item();
        if (!wantView) continue;
        float lo[3], hi[3];
        if (d->portBox(lo, hi)) {
            bv[i] = ProjectBox(r.mv, lo, hi, pr, 0.1f, 0.01f);
            foot[i] = bv[i].foot;
            const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f;
            const float vx = r.mv[0][0] * cx + r.mv[0][1] * cy + r.mv[0][2] * cz + r.mv[0][3];
            const float vy = r.mv[1][0] * cx + r.mv[1][1] * cy + r.mv[1][2] * cz + r.mv[1][3];
            const float vz = r.mv[2][0] * cx + r.mv[2][1] * cy + r.mv[2][2] * cz + r.mv[2][3];
            const float sc = std::sqrt(r.mv[0][0] * r.mv[0][0] + r.mv[1][0] * r.mv[1][0] + r.mv[2][0] * r.mv[2][0]);
            const float rad = 0.5f * sc * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
            float dist = std::sqrt((vx - px) * (vx - px) + (vy - py) * (vy - py) + (vz - pz) * (vz - pz)) - rad;
            it[i].dist = dist < 0.0f ? 0.0f : dist;
            it[i].area = bv[i].known ? bv[i].area : -1.0f;
            it[i].culled = bv[i].culled;
        } else {
            bv[i] = BoxView();  // not known: never culled, never size-cut, grouped alone (the whole screen)
            it[i].dist = 0.0f;  // (no box, no claim: a guarded volume)
        }
        const bool fresh = !(d->mLvHave && d->mLvPic + 1 == gPic);
        it[i].fPrev = fresh ? -1.0f : d->mLvF;
        it[i].keepPrev = fresh ? false : d->mLvKeep;
    }
    const bool plan = doLod || doCensus;  // (the census plans what the options would do, and applies only the ones that are on)
    if (plan) {
        Plan(prm, it, n, out);
    } else {
        for (int i = 0; i < n; ++i) { out[i].f = out[i].target = 1.0f; out[i].keep = true; }
    }
    const bool clearCut = KnobClear() != 0;
    float fOf[kMax];
    bool drawAny[kMax];   // the full four passes
    bool drawClear[kMax]; // the clear only
    int nAfter = 0, nFaded = 0, nFull = 0, nCulled = 0, nCutSize = 0, nCutRank = 0;  // (planned: what all the options on would draw)
    int aCulled = 0, aCut = 0, aFaded = 0;  // (compfix26: what is APPLIED with the options that are on)
    for (int i = 0; i < n; ++i) {
        const Rec& r = gRec[i];
        const VolumeModelDrawer* d = r.d;
        fOf[i] = 1.0f;
        drawAny[i] = drawClear[i] = false;
        const float f = out[i].f;
        bool cutHere = false;
        if (it[i].culled) {
            ++nCulled;
        } else {
            if (plan) {
                d->mLvHave = true;
                d->mLvPic = gPic;
                d->mLvF = f;
                d->mLvKeep = out[i].keep;
            }
            if (f <= 0.0f) {
                cutHere = true;
                if (SizeFactor(it[i].area, prm, it[i].keepPrev) <= 0.0f) ++nCutSize; else ++nCutRank;
                nAfter += clearCut ? 1 : 0;
            } else {
                nAfter += 4;
                if (f < 1.0f) ++nFaded; else ++nFull;
            }
        }
        if (doCensus) CensusAdd(d->mLvName, it[i].culled, cutHere, it[i].area, it[i].dist);
        // what is drawn
        active[i] = false;
        if (doCull && it[i].culled) ++aCulled;
        else if (doLod && cutHere) ++aCut;
        else if (doLod && f < 1.0f) ++aFaded;
        if (doCull && it[i].culled) continue;
        if (doLod && cutHere) {
            drawClear[i] = clearCut;
        } else {
            drawAny[i] = true;
            active[i] = true;
            fOf[i] = doLod ? f : 1.0f;
        }
    }
    if (mode & 7) {   // compfix26: the applied counts (GXV_LIGHTVOL / GXV_GLOW26): a line every 600 pictures with volumes, so that a console run proves the option acted (the census above prints the planned ones)
        port::glow26::g_count.lightvolPics.fetch_add(1, std::memory_order_relaxed);
        port::glow26::g_count.lightvolVolumes.fetch_add(static_cast< unsigned >(n), std::memory_order_relaxed);
        port::glow26::g_count.lightvolCulled.fetch_add(static_cast< unsigned >(aCulled), std::memory_order_relaxed);
        port::glow26::g_count.lightvolCut.fetch_add(static_cast< unsigned >(aCut), std::memory_order_relaxed);
        port::glow26::g_count.lightvolFaded.fetch_add(static_cast< unsigned >(aFaded), std::memory_order_relaxed);
        static unsigned sPics = 0, sVol = 0, sCul = 0, sCut = 0, sFad = 0;
        ++sPics; sVol += static_cast< unsigned >(n); sCul += static_cast< unsigned >(aCulled); sCut += static_cast< unsigned >(aCut); sFad += static_cast< unsigned >(aFaded);
        if (sPics >= 600 && !doCensus) {
            port_log_summary("lightvol: applied (GXV_LIGHTVOL=%d): %u pictures with volumes, a picture %.1f volumes, culled %.2f, cut to the clear pass %.2f, faded %.2f (batching %s)", mode, sPics,
                             static_cast< double >(sVol) / sPics, static_cast< double >(sCul) / sPics, static_cast< double >(sCut) / sPics, static_cast< double >(sFad) / sPics, doBatch ? "on" : "off");
            sPics = sVol = sCul = sCut = sFad = 0;
        }
    }
    int ng = 0;
    if (doBatch) ng = Group(foot, active, n, groupOf);
    else for (int i = 0; i < n; ++i) groupOf[i] = -1;
    int ngPlanned = ng;  // (the census: the groups the planned set would make with the batching on)
    if (doCensus && !(doBatch && doCull && doLod)) {
        bool pa[kMax];
        int go[kMax];
        for (int i = 0; i < n; ++i) pa[i] = !it[i].culled && out[i].f > 0.0f;
        ngPlanned = Group(foot, pa, n, go);
    }

    // ---- draw
    MR::loadProjectionMtx();
    if (!doBatch) {
        for (int i = 0; i < n; ++i) {
            const Rec& r = gRec[i];
            if (drawAny[i]) {
                Pos(r);
                State1(r.col); Draw(r);
                State2(r.col); Draw(r);
                State3(r.col); Draw(r);
                State4(Scaled(r.col, fOf[i])); Draw(r);
            } else if (drawClear[i]) {
                Pos(r);
                State1(r.col); Draw(r);
            }
        }
    } else {
        // the clears of the cut volumes first (they commute with everything: alpha 0 over a footprint nothing else of this picture writes except through its own clear)
        // then, group by group, pass by pass. Volumes that are in no group (inactive) were handled above or are culled.
        for (int g = 0; g < ng; ++g) {
            for (int pass = 1; pass <= 4; ++pass) {
                for (int i = 0; i < n; ++i) {
                    if (groupOf[i] != g) continue;
                    const Rec& r = gRec[i];
                    Pos(r);
                    if (pass == 1) State1(r.col);
                    else if (pass == 2) State2(r.col);
                    else if (pass == 3) State3(r.col);
                    else State4(Scaled(r.col, fOf[i]));
                    Draw(r);
                }
            }
        }
        for (int i = 0; i < n; ++i)
            if (drawClear[i]) { Pos(gRec[i]); State1(gRec[i].col); Draw(gRec[i]); }
    }
    // the state the original leaves (its last volume's last pass), whatever was drawn last
    Pos(gRec[n - 1]);
    State4(gRec[n - 1].col);

    if (doCensus) {
        if (gC.pics == 0 || static_cast< unsigned >(n) < gC.minN) gC.minN = n;
        if (static_cast< unsigned >(n) > gC.maxN) gC.maxN = n;
        ++gC.pics;
        gC.sumN += n; gC.culled += nCulled; gC.cutSize += nCutSize; gC.cutRank += nCutRank; gC.faded += nFaded; gC.full += nFull;
        gC.drawsBefore += 4 * n;
        gC.drawsAfter += nAfter;
        gC.groups += ngPlanned;
        for (int i = 0; i < n; ++i)
            if (!it[i].culled && it[i].area >= 0.0f) gC.px += it[i].area * 522240.0 * 4;
        static const unsigned period = [] {  // (a diagnostic: the census line's period in pictures; a host test sets it small)
            const char* e = getenv("GXV_LIGHTVOL_CENSUS");
            const long v = (e && *e) ? strtol(e, nullptr, 0) : 300;
            return static_cast< unsigned >(v < 1 ? 300 : v);
        }();
        if (gC.pics >= period) CensusPrint();
    }
}
#endif
