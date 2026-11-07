// GXV_PAR_ANIM host proof (tools/test_par_anim_walk.sh): the speculative joint walk gives the bytes of the game's own.
//
// Compiled here as they stand in the game (clang, the game's dialect), and linked whole:
//   - the original: XanimeCore.cpp (calc, calcSingle, calcBlend, calcScaleBlend*, fixT), J3DJoint.cpp (recursiveCalc and the
//     basic calculators' inits), J3DJointTree.cpp (J3DJointTree::calc), J3DAnimation.cpp (J3DAnmTransformKey::calcTransform),
//     J3DMtxBuffer.cpp (calcWeightEnvelopeMtx), J3DTransform.cpp, JMath.cpp, JMATrigonometric.cpp, and platform/src/sdk/mtx.c;
//   - the copy: decomp/include/Game/Animation/XanimeSpecWalk.hpp (Run, Apply, Compare);
//   - the worker: platform/include/port/par_spec.h (host branch, or with -D__vita__ its console branch over tests/par_stubs).
//
// Synthetic models: random joint trees (depth, siblings, scale compensation, rest transforms), random BCKs (key tables of both
// layouts, constant and empty channels, scales of exactly 1 and not, rotations past the key range), 1 to 4 tracks with weights
// (some zero), the three core types (Basic, SI, Maya without a transform list), fixT (_6 == 3), the three basic initializers
// (Basic, Softimage, Maya), random base matrices and scales (1 and not), envelopes on and off. Several models share one BCK and
// one joint tree (twenty Lumas share "Tico"), and LOD pairs share one matrix buffer.
//
// Each round: the main thread builds the jobs from each model's state (as ModelManager.cpp does at the end of calc-anim) and hands
// them to the worker; then, while the worker runs, it does what the game does in the next turn, model by model in order: set the
// BCK frames on the shared resources (XanimeCore::updateFrame), the root's calculator (XanimePlayer::calcAnm), and either
//   - Claim the job: done -> Apply it (onto the model's buffer and core) / taken or busy -> the original J3DJointTree::calc +
//     calcWeightEnvelopeMtx, then clear the root (clearAnm);
// and in parallel the reference world (a second copy of every buffer and core) always runs the original. After every model, the
// two worlds' buffers, core joint states and J3D statics must be equal byte for byte; and for every job the worker finished, its
// Output must equal what the original wrote (Compare, as GXV_PAR_ANIM_VERIFY does on the console).
// Negative controls (each must FAIL): -DNEG_FRAME (a job built from a frame one ulp off), -DNEG_BASE (base matrix one ulp off),
// -DNEG_PARENTS (the Softimage walk not given J3DSys::mParentS), -DNEG_NOCLAIM (results read without the claim: ThreadSanitizer
// must report the race).
// GXV_PAR_ANIM_BLEND (par2): a third argument "blend" also gives the models the states par-anim refused before: interpolations
// (_1C < 1), calcBlend's _20 != 1, the freeze copy (_29), track 0 without a resource, a total weight of 0, over random per-joint
// core state (_0, _28, _50); the jobs then carry Input::prev (xspec::TakePrev) and are taken only when xspec::SamePrev holds
// (ModelManager.cpp's rule). -DNEG_PREV (the copy of the core's state one ulp off in a frozen pose, not caught by the key) must fail.
// Built with the game's MathUtil.cpp (MR::vecBlend), -DPAR_ANIM_REAL_MATHUTIL.
// GXV_PAR_ANIM_CB (par2): an argument "cb" gives some models the NPC scale joint callback on a random joint: the original runs the
// game's J3DJoint::recursiveCalc -> JointController::staticCallBack -> JointControlDelegator< T > -> the host function, whose body is
// NPCActor::calcJointScale's (MR::preScaleMtx of the joint matrix by a scale vector; NPCActor itself needs the whole game), linked with
// the game's JointController.cpp and MtxUtil.cpp; the job carries Input::cbKind 1 and the scale, and its taker unregisters the callback
// as staticCallBack does at timing 1. -DNEG_CB (the job given the scale one ulp off, not caught by the key) must fail.
// GXV_PAR_FLUSH (par2): an argument "reverse" hands the worker the jobs from the end (job j is item n-1-j, ModelManager.cpp's
// ParFlushItem), while the main thread goes from the start, as the drawn turn's flush batch runs.
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimeSpecWalk.hpp"
#include "port/par_spec.h"
#include "Game/Util/JointController.hpp"
#include "Game/Util/MtxUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DJointTree.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// (J3DModelLoader.cpp defines this constructor inline; the test makes its basic calculators itself)
inline J3DMtxCalcNoAnmBase::J3DMtxCalcNoAnmBase() {
}

namespace {

u32 gSeed = 1;
u32 Rand() {
    gSeed ^= gSeed << 13;
    gSeed ^= gSeed >> 17;
    gSeed ^= gSeed << 5;
    return gSeed;
}
f32 RandF(f32 lo, f32 hi) {
    return lo + (hi - lo) * static_cast< f32 >(Rand() & 0xFFFFFF) / static_cast< f32 >(0x1000000);
}
bool Chance(u32 percent) {
    return Rand() % 100 < percent;
}

// ---- a BCK (J3DAnmTransformKey) with random key tables ----
struct Bck {
    J3DAnmTransformKey anm;
    std::vector< f32 > scale, trans;
    std::vector< s16 > rot;
    std::vector< J3DAnmTransformKeyTable > table;
};
template < typename T >
void MakeChannel(J3DAnmKeyTableBase& info, std::vector< T >& data, f32 lo, f32 hi, s16 frameMax, bool isScale) {
    const u32 r = Rand() % 10;
    info.mOffset = static_cast< u16 >(data.size());
    if (r == 0) {
        info.mMaxFrame = 0;
        info.mType = 0;
        return;
    }
    if (r <= 2) {
        info.mMaxFrame = 1;
        info.mType = 0;
        data.push_back(isScale && Chance(50) ? static_cast< T >(1) : static_cast< T >(RandF(lo, hi)));
        return;
    }
    const u16 keys = static_cast< u16 >(2 + Rand() % 5);
    const u16 type = static_cast< u16 >(Rand() % 2);
    info.mMaxFrame = keys;
    info.mType = type;
    f32 t = static_cast< f32 >(Rand() % 3);
    for (u16 k = 0; k < keys; k++) {
        data.push_back(static_cast< T >(t));
        data.push_back(isScale && Chance(30) ? static_cast< T >(1) : static_cast< T >(RandF(lo, hi)));
        data.push_back(static_cast< T >(RandF(-0.5f, 0.5f) * (hi - lo)));
        if (type == 1) {
            data.push_back(static_cast< T >(RandF(-0.5f, 0.5f) * (hi - lo)));
        }
        t += static_cast< f32 >(1 + Rand() % (frameMax / keys + 1));
    }
}
Bck* MakeBck(u16 joints) {
    Bck* b = new Bck;
    const s16 frameMax = static_cast< s16 >(10 + Rand() % 80);
    b->anm.mFrameMax = frameMax;
    b->anm.mDecShift = static_cast< int >(Rand() % 2);
    b->table.resize(joints * 3);
    for (u32 i = 0; i < b->table.size(); i++) {
        MakeChannel(b->table[i].mScaleInfo, b->scale, 0.5f, 1.8f, frameMax, true);
        MakeChannel(b->table[i].mRotationInfo, b->rot, -16000.0f, 16000.0f, frameMax, false);
        MakeChannel(b->table[i].mTranslateInfo, b->trans, -200.0f, 200.0f, frameMax, false);
    }
    b->scale.push_back(1.0f);  // (never empty)
    b->rot.push_back(0);
    b->trans.push_back(0.0f);
    b->anm.mScaleData = b->scale.data();
    b->anm.mRotData = b->rot.data();
    b->anm.mTransData = b->trans.data();
    b->anm.mAnmTable = b->table.data();
    return b;
}

// ---- a model data: a joint tree with envelopes and a basic calculator ----
struct Data {
    J3DModelData* data;  // (raw storage: only its joint tree is used)
    std::vector< J3DJoint* > joints;
    std::vector< J3DJoint* > nodePtr;
    std::vector< u8 > mixNum;
    std::vector< u16 > mixIdx;
    std::vector< f32 > mixW;
    struct M {
        Mtx m;
    };
    std::vector< M > inv;
    int kind;
    std::vector< Bck* > bcks;
    J3DJointTree& tree() { return data->mJointTree; }
};
Data* MakeData() {
    Data* d = new Data;
    void* raw = calloc(1, sizeof(J3DModelData));
    d->data = static_cast< J3DModelData* >(raw);
    new (&d->data->mJointTree) J3DJointTree();
    const u16 n = static_cast< u16 >(1 + Rand() % 40);
    for (u16 i = 0; i < n; i++) {
        J3DJoint* j = new J3DJoint();
        j->mJntNo = i;
        j->mScaleCompensate = Chance(30) ? 1 : 0;
        J3DTransformInfo& t = j->mTransformInfo;
        t.mScale.x = Chance(60) ? 1.0f : RandF(0.5f, 1.5f);
        t.mScale.y = Chance(60) ? 1.0f : RandF(0.5f, 1.5f);
        t.mScale.z = Chance(60) ? 1.0f : RandF(0.5f, 1.5f);
        t.mRotation.x = static_cast< s16 >(Rand());
        t.mRotation.y = static_cast< s16 >(Rand());
        t.mRotation.z = static_cast< s16 >(Rand());
        t.mTranslate.x = RandF(-100.0f, 100.0f);
        t.mTranslate.y = RandF(-100.0f, 100.0f);
        t.mTranslate.z = RandF(-100.0f, 100.0f);
        d->joints.push_back(j);
        if (i > 0) {
            // a parent among the earlier joints (deep chains and wide fans both happen)
            const u16 p = Chance(50) ? static_cast< u16 >(i - 1) : static_cast< u16 >(Rand() % i);
            d->joints[p]->appendChild(j);
        }
    }
    // joint numbers in the node table may come in another order than the tree's (the walk goes by the tree)
    d->nodePtr = d->joints;
    J3DJointTree& tr = d->tree();
    tr.mRootNode = d->joints[0];
    tr.mJointNodePointer = d->nodePtr.data();
    tr.mJointNum = n;
    // envelopes
    const u16 env = Chance(40) ? 0 : static_cast< u16 >(1 + Rand() % 24);
    for (u16 e = 0; e < env; e++) {
        const u8 k = static_cast< u8 >(1 + Rand() % 4);
        d->mixNum.push_back(k);
        f32 left = 1.0f;
        for (u8 m = 0; m < k; m++) {
            d->mixIdx.push_back(static_cast< u16 >(Rand() % n));
            const f32 w = m + 1 == k ? left : RandF(0.0f, left);
            left -= w;
            d->mixW.push_back(w);
        }
    }
    d->inv.resize(n);
    for (u16 i = 0; i < n; i++) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                d->inv[i].m[r][c] = RandF(-1.0f, 1.0f) + (r == c ? 1.0f : 0.0f);
            }
        }
    }
    tr.mWEvlpMtxNum = env;
    tr.mWEvlpMixMtxNum = d->mixNum.empty() ? nullptr : d->mixNum.data();
    tr.mWEvlpMixMtxIndex = d->mixIdx.empty() ? nullptr : d->mixIdx.data();
    tr.mWEvlpMixWeight = d->mixW.empty() ? nullptr : d->mixW.data();
    tr.mInvJointMtx = reinterpret_cast< Mtx* >(d->inv.data());
    d->kind = static_cast< int >(Rand() % 3);
    J3DMtxCalc* basic = nullptr;
    switch (d->kind) {
    case 0:
        basic = new J3DMtxCalcNoAnm< J3DMtxCalcCalcTransformBasic, J3DMtxCalcJ3DSysInitBasic >();
        break;
    case 1:
        basic = new J3DMtxCalcNoAnm< J3DMtxCalcCalcTransformSoftimage, J3DMtxCalcJ3DSysInitSoftimage >();
        break;
    default:
        basic = new J3DMtxCalcNoAnm< J3DMtxCalcCalcTransformMaya, J3DMtxCalcJ3DSysInitMaya >();
        break;
    }
    tr.mBasicMtxCalc = basic;
    const u32 nb = 1 + Rand() % 3;
    for (u32 i = 0; i < nb; i++) {
        d->bcks.push_back(MakeBck(n));
    }
    return d;
}

// ---- a matrix buffer's arrays and a core, one per world ----
struct State {
    J3DMtxBuffer* buf;
    XanimeCore* core;
};
J3DMtxBuffer* MakeBuffer(Data* d) {
    J3DMtxBuffer* b = new J3DMtxBuffer();
    const u16 n = d->tree().getJointNum(), e = d->tree().getWEvlpMtxNum();
    b->mJointTree = &d->tree();
    b->mpAnmMtx = new Mtx[n];
    b->mpScaleFlagArr = new u8[n];
    b->mpWeightEvlpMtx = e ? new Mtx[e] : nullptr;
    b->mpEvlpScaleFlagArr = e ? new u8[e] : nullptr;
    for (u16 i = 0; i < n; i++) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                b->mpAnmMtx[i][r][c] = RandF(-9.0f, 9.0f);  // (garbage: nothing may survive from before)
            }
        }
        b->mpScaleFlagArr[i] = static_cast< u8 >(Rand() % 3);
    }
    return b;
}

// The NPC scale callback's host (NPCActor::calcJointScale's body).
struct CbHost {
    TVec3f scale;
    bool calcJointScale(TPos3f* pPos, const JointControllerInfo&) {
        MR::preScaleMtx(pPos->toMtxPtr(), scale);
        return true;
    }
};

struct Model {
    Data* d;
    J3DModel* model;  // (raw storage: j3dSys.getModel()->getModelData() for fixT)
    State st[2];      // 0: the speculative world, 1: the reference world
    u8 type4, mode6, trackCount;
    u16 fixJoint;
    u32 flags;        // J3DMdlFlag_EnableLOD or not
    Vec scale;
    Mtx base;
    u32 bck[4];
    f32 weight[4];
    f32 frame[4];
    Model* lodHigh;   // shares that model's buffers (syncJointAnimation)
    f32 b1C, b20;     // (blend runs) XanimeCore::_1C, _20
    u8 freeze;        // (blend runs) XanimeCore::_29
    u8 noTrack0;      // (blend runs) track 0 without a resource
    u16 cbJoint;      // (cb runs) the joint of its scale callback
    bool hasCb;
    CbHost cbHost[2];                            // (one per world)
    JointControlDelegator< CbHost >* cbCtl[2];
};
bool gBlend = false;    // (an argument "blend")
bool gCb = false;       // (an argument "cb")
bool gReverse = false;  // (an argument "reverse")
u32 gJobCount = 0;

XanimeCore* MakeCore(const Model& m) {
    XanimeCore* c = new XanimeCore(m.trackCount, m.d->tree().getJointNum(), m.type4);
    c->_6 = m.mode6;
    c->_C = m.fixJoint;
    for (u32 j = 0; j < c->mJointCount; j++) {
        // (garbage: the qualifying walk never reads it, and must write it all)
        c->mJointList[j]._28._0.set(RandF(-5, 5), RandF(-5, 5), RandF(-5, 5));
        c->mJointList[j]._28._C.set(RandF(-5, 5), RandF(-5, 5), RandF(-5, 5));
        c->mJointList[j]._50.set(RandF(-5, 5), RandF(-5, 5), RandF(-5, 5));
        // (blend runs read the frozen pose _0 and the last pose's rotation: unit-ish quaternions, as the game's are)
        c->mJointList[j]._0._0.set(RandF(0.5f, 2), RandF(0.5f, 2), RandF(0.5f, 2));
        c->mJointList[j]._0._C.set(RandF(-50, 50), RandF(-50, 50), RandF(-50, 50));
        Quaternion q = {RandF(-1, 1), RandF(-1, 1), RandF(-1, 1), RandF(-1, 1)};
        const f32 l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) + 1e-3f;
        q.x /= l, q.y /= l, q.z /= l, q.w /= l;
        c->mJointList[j]._0.mRotation = q;
        c->mJointList[j]._28.mRotation = q;
    }
    return c;
}

void Randomize(Model& m) {
    // the base and the scale move or not; the frames advance; tracks and weights change now and then
    if (Chance(40)) {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                m.base[r][c] = RandF(-2.0f, 2.0f) * (c == 3 ? 300.0f : 1.0f);
            }
        }
    }
    if (Chance(20)) {
        m.scale.x = Chance(60) ? 1.0f : RandF(0.5f, 2.0f);
        m.scale.y = Chance(60) ? 1.0f : RandF(0.5f, 2.0f);
        m.scale.z = Chance(60) ? 1.0f : RandF(0.5f, 2.0f);
    }
    if (m.hasCb && Chance(30)) {
        m.cbHost[0].scale.set(Chance(30) ? 1.0f : RandF(0.7f, 1.3f), RandF(0.7f, 1.3f), Chance(30) ? 1.0f : RandF(0.7f, 1.3f));
        m.cbHost[1].scale = m.cbHost[0].scale;
    }
    if (gBlend) {
        // (an interpolation now and then, as after a change of animation; the freeze copy at its first computation)
        if (Chance(25)) {
            m.b1C = Chance(30) ? 0.0f : RandF(0.0f, 1.0f);
            m.freeze = Chance(30) ? 1 : 0;
        } else if (Chance(30)) {
            m.b1C = 1.0f;
            m.freeze = 0;
        }
        if (Chance(10)) {
            m.b20 = Chance(50) ? 1.0f : RandF(0.05f, 1.0f);
        }
        m.noTrack0 = m.trackCount == 1 && Chance(5) ? 1 : 0;
        if (m.trackCount > 1 && Chance(4)) {
            for (u32 t = 0; t < m.trackCount; t++) {
                m.weight[t] = 0.0f;  // (calcBlend's zero-weight branch: the last pose)
            }
        }
    }
    for (u32 t = 0; t < m.trackCount; t++) {
        const J3DAnmTransformKey& a = m.d->bcks[m.bck[t]]->anm;
        m.frame[t] = Chance(10) ? RandF(-5.0f, 0.0f) : RandF(0.0f, static_cast< f32 >(a.mFrameMax) + 8.0f);
        if (Chance(5)) {
            m.bck[t] = Rand() % m.d->bcks.size();
        }
        if (Chance(5)) {
            m.weight[t] = Chance(20) ? 0.0f : RandF(0.1f, 1.0f);
        }
    }
}

// The job the game thread builds at the end of calc-anim (ModelManager.cpp ParAnimBuild), from a model's state.
struct Job {
    xspec::Input in;
    xspec::Output out;
    bool ok;
    Model* m;
    xspec::JointPrev* prev;
};
bool InBlendState(const Model& m) {
    if (m.b1C < 1.0f || m.b20 != 1.0f || m.freeze != 0 || m.noTrack0 != 0) {
        return true;
    }
    if (m.trackCount > 1) {
        f32 total = 0.0f;
        for (u32 t = 0; t < m.trackCount; t++) {
            total += m.weight[t];
        }
        return total == 0.0f;
    }
    return false;
}
bool Qualifies(const Model& m) {
    if (gBlend) {
        return true;  // (GXV_PAR_ANIM_BLEND: every state, the blend ones through Input::prev)
    }
    if (m.trackCount == 1) {
        return true;  // (track 0 always has a resource here)
    }
    f32 total = 0.0f;
    for (u32 t = 0; t < m.trackCount; t++) {
        total += m.weight[t];
    }
    return total != 0.0f;  // (calcBlend's zero-weight branch reads the core's _28: such models are computed as before)
}
// The frame a track's resource holds when the original computes: XanimeCore::updateFrame sets every track's resource in turn, so
// two tracks of one resource both read the later track's frame (ModelManager.cpp's prediction does the same).
f32 EffectiveFrame(const Model& m, u32 t) {
    f32 f = m.frame[t];
    for (u32 u = t + 1; u < m.trackCount; u++) {
        if (m.bck[u] == m.bck[t]) {
            f = m.frame[u];
        }
    }
    return f;
}
void BuildJob(Job& j, Model& m) {
    xspec::Input& in = j.in;
    memset(&in, 0, sizeof(in));
    J3DMtxBuffer* buf = m.st[0].buf;
    in.dataTree = &m.d->tree();
    in.bufTree = buf->mJointTree;
    in.root = m.d->tree().getRootNode();
    for (u32 t = 0; t < m.trackCount; t++) {
        in.track[t].res = t == 0 && m.noTrack0 ? nullptr : &m.d->bcks[m.bck[t]]->anm;
        in.track[t].weight = m.weight[t];
        in.track[t].frame = EffectiveFrame(m, t);
#ifdef NEG_FRAME
        in.track[t].frame = nextafterf(in.track[t].frame, 1e9f);
#endif
    }
    in.trackCount = m.trackCount;
    in.type4 = m.type4;
    in.mode6 = m.mode6;
    in.initKind = static_cast< u8 >(m.d->kind);
    in.fixJoint = m.fixJoint;
    in.jointNum = m.d->tree().getJointNum();
    in.doEnv = m.d->tree().getWEvlpMtxNum() != 0 && !(m.flags & J3DMdlFlag_EnableLOD);
    if (m.hasCb) {
        in.cbKind = 1;
        in.cbJoint = m.cbJoint;
        in.cbScale = m.cbHost[0].scale;
#ifdef NEG_CB
        in.cbScale.y = nextafterf(in.cbScale.y, 1e9f);
#endif
    }
    in.blend1C = gBlend ? m.b1C : 1.0f;
    in.blend20 = gBlend ? m.b20 : 1.0f;
    in.freeze29 = gBlend ? m.freeze : 0;
    in.scale = m.scale;
    PSMTXCopy(m.base, in.base);
#ifdef NEG_BASE
    in.base[1][3] = nextafterf(in.base[1][3], 1e9f);
#endif
    in.parentS = J3DSys::mParentS;
#ifdef NEG_PARENTS
    in.parentS.x = 1.0f;
    in.parentS.y = 1.0f;
    in.parentS.z = 1.0f;
#endif
    const u16 n = in.jointNum, e = buf->mJointTree->getWEvlpMtxNum();
    j.out.anm = new Mtx[n];
    j.out.scaleFlag = new u8[n];
    const bool prev = gBlend && InBlendState(m);
    j.out.joint = prev ? nullptr : new xspec::JointState[n];
    j.out.full = prev ? new xspec::JointPrev[n] : nullptr;
    j.prev = prev ? new xspec::JointPrev[n] : nullptr;
    if (prev) {
        xspec::TakePrev(m.st[0].core, n, j.prev);
#ifdef NEG_PREV
        j.prev[n / 2].t0[3] = nextafterf(j.prev[n / 2].t0[3], 1e9f);  // (a frozen pose's x one ulp off)
#endif
        in.prev = j.prev;
    }
    j.out.env = e ? new Mtx[e] : nullptr;
    j.out.envScale = e ? new u8[e] : nullptr;
    j.ok = false;
    j.m = &m;
}
void FreeJob(Job& j) {
    delete[] j.out.anm;
    delete[] j.out.scaleFlag;
    delete[] j.out.joint;
    delete[] j.out.full;
    delete[] j.prev;
    delete[] j.out.env;
    delete[] j.out.envScale;
}
void JobFn(void* ctx, u32 i) {
    Job& j = static_cast< Job* >(ctx)[i];
    j.ok = xspec::Run(j.in, j.out);
}
void JobFnReverse(void* ctx, u32 i) {
    JobFn(ctx, gJobCount - 1u - i);  // (GXV_PAR_FLUSH's order)
}
u32 ItemOf(u32 job) {
    return gReverse ? gJobCount - 1u - job : job;
}

// The key check (ModelManager.cpp: MemoKey's words and, for a Softimage init, J3DSys::mParentS): the job's inputs against the
// model's at its calc. The NEG_* controls leave out the field they perturb, as a key without it would.
bool KeyMatch(const Job& j, const Model& m) {
    if (j.in.trackCount != m.trackCount) {
        return false;
    }
#ifndef NEG_CB
    if ((j.in.cbKind != 0) != m.hasCb || (m.hasCb && (j.in.cbJoint != m.cbJoint || memcmp(&j.in.cbScale, &m.cbHost[0].scale, sizeof(Vec)) != 0))) {
        return false;  // (ModelManager.cpp: the callback's joint, controller and scale are key words)
    }
#endif
    if (gBlend) {
        // (ModelManager.cpp: _1C, _20 and _29 are key words, a track's resource is; the core's joint state is compared with the copy)
        if (memcmp(&j.in.blend1C, &m.b1C, 4) != 0 || memcmp(&j.in.blend20, &m.b20, 4) != 0 || j.in.freeze29 != m.freeze ||
            (j.in.track[0].res == nullptr) != (m.noTrack0 != 0) || (j.in.prev != nullptr) != InBlendState(m)) {
            return false;
        }
#ifndef NEG_PREV
        if (j.in.prev != nullptr && !xspec::SamePrev(m.st[0].core, m.d->tree().getJointNum(), j.in.prev)) {
            return false;
        }
#endif
    }
    for (u32 t = 0; t < m.trackCount; t++) {
        if ((j.in.track[t].res != &m.d->bcks[m.bck[t]]->anm && !(t == 0 && m.noTrack0)) || memcmp(&j.in.track[t].weight, &m.weight[t], 4) != 0) {
            return false;
        }
#ifndef NEG_FRAME
        const f32 f = EffectiveFrame(m, t);
        if (memcmp(&j.in.track[t].frame, &f, 4) != 0) {
            return false;
        }
#endif
    }
    if (memcmp(&j.in.scale, &m.scale, sizeof(Vec)) != 0) {
        return false;
    }
#ifndef NEG_BASE
    if (memcmp(j.in.base, m.base, sizeof(Mtx)) != 0) {
        return false;
    }
#endif
#ifndef NEG_PARENTS
    if (j.in.initKind == xspec::kInitSoftimage && memcmp(&j.in.parentS, &J3DSys::mParentS, sizeof(Vec)) != 0) {
        return false;
    }
#endif
    return true;
}

// The original, as ModelManager::calc runs it for this model: calcAnm (frames onto the shared BCKs, the core onto the shared
// root), J3DModel::calc's joints (J3DJointTree::calc, J3DMtxBuffer::calcWeightEnvelopeMtx), clearAnm.
void Original(Model& m, int w) {
    State& s = m.st[w];
    for (u32 t = 0; t < m.trackCount; t++) {
        J3DAnmTransformKey* a = &m.d->bcks[m.bck[t]]->anm;
        s.core->mTrackList[t]._0 = t == 0 && m.noTrack0 ? nullptr : a;
        s.core->mTrackList[t].mWeight = m.weight[t];
        a->setFrame(m.frame[t]);
    }
    s.core->_1C = gBlend ? m.b1C : 1.0f;
    s.core->_20 = gBlend ? m.b20 : 1.0f;
    s.core->_29 = gBlend ? m.freeze : 0;
    m.d->tree().getRootNode()->setMtxCalc(s.core);
    m.model->mMtxBuffer = s.buf;  // (the controller reads the joint matrix through its model: JointController::calcJointMatrixAndSetSystem)
    if (m.hasCb) {
        m.cbCtl[w]->registerCallBack();  // (NPCActor::calcAndSetBaseMtx registers it before every computation)
    }
    j3dSys.setModel(m.model);
    m.d->tree().calc(s.buf, m.scale, m.base);
    if (m.d->tree().getWEvlpMtxNum() != 0 && !(m.flags & J3DMdlFlag_EnableLOD)) {
        s.buf->calcWeightEnvelopeMtx();
    }
    m.d->tree().getRootNode()->setMtxCalc(nullptr);
}

struct Statics {
    Mtx cur;
    Vec s, p;
    J3DMtxBuffer* buf;
    J3DJoint* joint;
    J3DMtxCalc* jcalc;
    J3DMtxCalc* syscalc;
};
Statics Snap() {
    Statics x;
    memcpy(x.cur, J3DSys::mCurrentMtx, sizeof(Mtx));
    x.s = J3DSys::mCurrentS;
    x.p = J3DSys::mParentS;
    x.buf = J3DMtxCalc::getMtxBuffer();
    x.joint = J3DMtxCalc::getJoint();
    x.jcalc = J3DJoint::mCurrentMtxCalc;
    x.syscalc = j3dSys.mCurrentMtxCalc;
    return x;
}
// (the two worlds' statics name their own buffer and core: compared by role)
bool SameStatics(const Statics& a, const Statics& b, const Model& m) {
    return memcmp(a.cur, b.cur, sizeof(Mtx)) == 0 && memcmp(&a.s, &b.s, sizeof(Vec)) == 0 && memcmp(&a.p, &b.p, sizeof(Vec)) == 0 &&
           a.buf == m.st[0].buf && b.buf == m.st[1].buf && a.joint == b.joint && a.jcalc == b.jcalc && a.syscalc == m.st[0].core &&
           b.syscalc == m.st[1].core;
}
bool SameWorlds(const Model& m) {
    const J3DMtxBuffer* a = m.st[0].buf;
    const J3DMtxBuffer* b = m.st[1].buf;
    const u16 n = m.d->tree().getJointNum(), e = m.d->tree().getWEvlpMtxNum();
    if (memcmp(a->mpAnmMtx, b->mpAnmMtx, n * sizeof(Mtx)) != 0 || memcmp(a->mpScaleFlagArr, b->mpScaleFlagArr, n) != 0) {
        return false;
    }
    if (e && !(m.flags & J3DMdlFlag_EnableLOD) &&
        (memcmp(a->mpWeightEvlpMtx, b->mpWeightEvlpMtx, e * sizeof(Mtx)) != 0 || memcmp(a->mpEvlpScaleFlagArr, b->mpEvlpScaleFlagArr, e) != 0)) {
        return false;
    }
    for (u16 j = 0; j < n; j++) {
        if (memcmp(&m.st[0].core->mJointList[j]._28, &m.st[1].core->mJointList[j]._28, sizeof(XtransformInfo)) != 0 ||
            memcmp(&m.st[0].core->mJointList[j]._0, &m.st[1].core->mJointList[j]._0, sizeof(XtransformInfo)) != 0 ||
            memcmp(&m.st[0].core->mJointList[j]._50, &m.st[1].core->mJointList[j]._50, sizeof(TVec3f)) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? atoi(argv[1]) : 200;
    gSeed = argc > 2 ? static_cast< u32 >(atoi(argv[2])) : 12345u;
    for (int a = 3; a < argc; a++) {
        gBlend = gBlend || strcmp(argv[a], "blend") == 0;
        gReverse = gReverse || strcmp(argv[a], "reverse") == 0;
        gCb = gCb || strcmp(argv[a], "cb") == 0;
    }
    port::par::SpecWorker& worker = *new port::par::SpecWorker;  // (never destroyed: its thread waits on it)
    if (!worker.Start(1, 0)) {
        printf("worker did not start\n");
        return 2;
    }
    // A scene: model datas, and models of them (some sharing a buffer with an earlier model of the same data: LOD pairs).
    std::vector< Data* > datas;
    for (int i = 0; i < 10; i++) {
        datas.push_back(MakeData());
    }
    std::vector< Model* > models;
    for (int i = 0; i < 48; i++) {
        Model* m = new Model;
        memset(m, 0, sizeof(*m));
        m->d = datas[Rand() % datas.size()];
        void* raw = calloc(1, sizeof(J3DModel));
        m->model = static_cast< J3DModel* >(raw);
        m->model->mModelData = m->d->data;
        m->type4 = static_cast< u8 >(Rand() % 3);
        m->mode6 = Chance(25) ? 3 : 0;
        m->trackCount = static_cast< u8 >(Chance(50) ? 1 : 2 + Rand() % 3);
        m->fixJoint = static_cast< u16 >(Rand() % m->d->tree().getJointNum());
        m->flags = Chance(15) ? J3DMdlFlag_EnableLOD : 0;
        m->scale.x = m->scale.y = m->scale.z = 1.0f;
        m->b1C = m->b20 = 1.0f;
        PSMTXIdentity(m->base);
        for (u32 t = 0; t < m->trackCount; t++) {
            m->bck[t] = Rand() % m->d->bcks.size();
            m->weight[t] = Chance(15) ? 0.0f : RandF(0.1f, 1.0f);
        }
        m->lodHigh = nullptr;
        for (Model* o : models) {
            if (o->d == m->d && o->lodHigh == nullptr && Chance(30)) {
                m->lodHigh = o;
                break;
            }
        }
        for (int w = 0; w < 2; w++) {
            m->st[w].buf = m->lodHigh ? m->lodHigh->st[w].buf : MakeBuffer(m->d);
            m->st[w].core = MakeCore(*m);
        }
        m->hasCb = gCb && Chance(60);
        if (m->hasCb) {
            m->cbJoint = static_cast< u16 >(Rand() % m->d->tree().getJointNum());
            for (int w = 0; w < 2; w++) {
                m->cbHost[w].scale.set(1.0f, 1.0f, 1.0f);
                m->cbCtl[w] = new JointControlDelegator< CbHost >(&m->cbHost[w], &CbHost::calcJointScale, nullptr);
                m->cbCtl[w]->mModel = m->model;
                m->cbCtl[w]->mJoint = m->d->tree().getJointNodePointer(m->cbJoint);
            }
        }
        Randomize(*m);
        models.push_back(m);
    }
    // the reference world's buffers start as the speculative world's
    for (Model* m : models) {
        if (m->lodHigh == nullptr) {
            const u16 n = m->d->tree().getJointNum();
            memcpy(m->st[1].buf->mpAnmMtx, m->st[0].buf->mpAnmMtx, n * sizeof(Mtx));
            memcpy(m->st[1].buf->mpScaleFlagArr, m->st[0].buf->mpScaleFlagArr, n);
        }
        for (u32 j = 0; j < m->st[0].core->mJointCount; j++) {
            m->st[1].core->mJointList[j] = m->st[0].core->mJointList[j];
        }
    }
    J3DSys::mParentS.x = J3DSys::mParentS.y = J3DSys::mParentS.z = 1.0f;

    u32 done = 0, taken = 0, busy = 0, unqual = 0, miss = 0, verifyBad = 0, worldBad = 0, staticBad = 0, walkFalse = 0, blendApplied = 0;
    std::vector< Job > jobs(models.size());
    std::vector< std::atomic< uint8_t > > items(models.size());
    for (int round = 0; round < rounds; round++) {
        // The batch, from each model's state (as the game does at the end of calc-anim). Some models' inputs then change before
        // their calc (a mismatching key is a miss in the game; here such a model is not built, it simply runs the original).
        worker.Quiesce();
        u32 n = 0;
        std::vector< Model* > order;
        std::vector< int > jobOf(models.size(), -1);
        for (size_t i = 0; i < models.size(); i++) {
            Model* m = models[i];
            if (!Qualifies(*m)) {
                ++unqual;
                continue;
            }
            BuildJob(jobs[n], *m);
            items[n].store(port::par::kJobPending, std::memory_order_relaxed);
            jobOf[i] = static_cast< int >(n);
            ++n;
        }
        // (slow the worker down now and then, so that the main thread meets jobs pending, running and done)
        gJobCount = n;
        worker.Dispatch(gReverse ? &JobFnReverse : &JobFn, jobs.data(), items.data(), n);
        for (size_t i = 0; i < models.size(); i++) {
            Model* m = models[i];
            const int ji = jobOf[i];
            // (now and then a model's inputs change after the batch was made: its key no longer matches, a miss)
            if (Chance(8)) {
                Randomize(*m);
            }
            // (now and then the walk starts from a J3DSys::mParentS other than 1: a Softimage init reads it, and so does the key; the
            // game leaves 1 there after every walk, but nothing forbids another value)
            if (Chance(15)) {
                J3DSys::mParentS.x = RandF(0.5f, 2.0f);
                J3DSys::mParentS.y = Chance(50) ? 1.0f : RandF(0.5f, 2.0f);
                J3DSys::mParentS.z = RandF(0.5f, 2.0f);
            }
            // (now and then the batch is ended early, as a turn whose worker is still busy does: the jobs it has not started stay
            // pending and the main thread takes them; the finished ones stay readable)
            if (Chance(3)) {
                (void)worker.TryQuiesce();
            }
            // the speculative world (the J3D statics are shared by the two worlds: both start from the same J3DSys::mParentS)
            const Vec parentAtStart = J3DSys::mParentS;
            bool applied = false;
            if (ji >= 0) {
#ifdef NEG_NOCLAIM
                Job& j = jobs[ji];
                (void)items;
                if (j.ok || true) {
#else
                const port::par::SpecWorker::ClaimResult r = port::par::SpecWorker::Claim(items[ItemOf(static_cast< u32 >(ji))], 2000);
                Job& j = jobs[ji];
                if (r == port::par::SpecWorker::kClaimDone) {
                    ++done;
#endif
                    if (!j.ok) {
                        ++walkFalse;
                    } else if (!KeyMatch(j, *m)) {
                        ++miss;
                    } else {
                        // what calcAnm does around the calc (the frames onto the shared BCKs, the core onto the shared root)
                        for (u32 t = 0; t < m->trackCount; t++) {
                            m->d->bcks[m->bck[t]]->anm.setFrame(m->frame[t]);
                        }
                        const u16 e = m->st[0].buf->mJointTree->getWEvlpMtxNum();
                        m->model->mMtxBuffer = m->st[0].buf;
                        xspec::Apply(j.out, m->d->tree().getJointNum(), e, j.in.doEnv != 0, m->st[0].buf, m->st[0].core,
                                     m->d->tree().getBasicMtxCalc());
                        if (j.in.cbKind != 0) {
                            J3DJoint* cj = m->d->tree().getJointNodePointer(j.in.cbJoint);
                            cj->setCallBack(nullptr);  // (ModelManager.cpp ParTakeJob: staticCallBack's timing 1)
                            cj->mCallBackUserData = nullptr;
                        }
                        applied = true;
                        blendApplied += j.in.prev != nullptr ? 1 : 0;
                    }
#ifndef NEG_NOCLAIM
                } else if (r == port::par::SpecWorker::kClaimTaken) {
                    ++taken;
                } else {
                    ++busy;
#endif
                }
            }
            if (!applied) {
                Original(*m, 0);
            }
            const Statics s0 = Snap();
            // the reference world
            J3DSys::mParentS = parentAtStart;
            Original(*m, 1);
            const Statics s1 = Snap();
            if (!SameWorlds(*m)) {
                ++worldBad;
            }
            if (!SameStatics(s0, s1, *m)) {
                ++staticBad;
            }
            if (m->hasCb && (m->d->tree().getJointNodePointer(m->cbJoint)->getCallBack() != nullptr ||
                             m->d->tree().getJointNodePointer(m->cbJoint)->mCallBackUserData != nullptr)) {
                ++staticBad;  // (both worlds leave the callback unregistered)
            }
            // GXV_PAR_ANIM_VERIFY's check: a finished job's Output against what the original just wrote
            if (applied) {
                const u16 e = m->st[1].buf->mJointTree->getWEvlpMtxNum();
                // (Compare reads the statics: they are the reference world's now, so compare against its buffer and core)
                if (xspec::Compare(jobs[ji].out, m->d->tree().getJointNum(), e, jobs[ji].in.doEnv != 0, m->st[1].buf, m->st[1].core,
                                   m->d->tree().getBasicMtxCalc()) != 0) {
                    if (verifyBad < 8) {
                        printf("verify mismatch: bits 0x%x type %u mode %u tracks %u init %d env %u joints %u lod %d\n",
                               xspec::Compare(jobs[ji].out, m->d->tree().getJointNum(), e, jobs[ji].in.doEnv != 0, m->st[1].buf,
                                              m->st[1].core, m->d->tree().getBasicMtxCalc()),
                               m->type4, m->mode6, m->trackCount, m->d->kind, e, m->d->tree().getJointNum(), m->lodHigh != nullptr);
                        for (u16 q = 0; q < m->d->tree().getJointNum(); q++) {
                            const f32* a = jobs[ji].out.full != nullptr ? jobs[ji].out.full[q].t28 : jobs[ji].out.joint[q].t28;
                            const f32* b = &m->st[1].core->mJointList[q]._28._0.x;
                            if (memcmp(a, b, 40) != 0) {
                                printf("  joint %u: spec %a %a %a | %a %a %a | %a %a %a %a\n  orig %a %a %a | %a %a %a | %a %a %a %a\n", q, a[0], a[1],
                                       a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
                                break;
                            }
                        }
                    }
                    ++verifyBad;
                }
            }
        }
        if (round & 1) {
            worker.Quiesce();
        } else {
            while (!worker.TryQuiesce()) {
            }
        }
        for (u32 k = 0; k < n; k++) {
            FreeJob(jobs[k]);
        }
        for (Model* m : models) {
            Randomize(*m);
        }
        if (Chance(50)) {
            J3DSys::mParentS.x = J3DSys::mParentS.y = J3DSys::mParentS.z = 1.0f;
        }
    }
    printf("par_anim_walk%s%s%s: %d rounds, %zu models: worker done %u, taken by the game thread %u, busy %u, not qualifying %u, key misses %u; "
           "walk refused %u; mismatches: worlds %u, statics %u, verify %u; blend-state jobs applied %u\n",
           gBlend ? " (blend)" : "", gReverse ? " (reverse)" : "", gCb ? " (cb)" : "", rounds, models.size(), done, taken, busy, unqual, miss, walkFalse, worldBad, staticBad, verifyBad, blendApplied);
    const bool pass = worldBad == 0 && staticBad == 0 && verifyBad == 0 && walkFalse == 0 && done > 0 && (!gBlend || blendApplied > 0);
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
