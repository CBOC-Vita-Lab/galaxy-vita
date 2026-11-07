#include "Game/LiveActor/ModelManager.hpp"
#include "Game/Animation/BckCtrl.hpp"
#include "Game/Animation/BpkPlayer.hpp"
#include "Game/Animation/BrkPlayer.hpp"
#include "Game/Animation/BtkPlayer.hpp"
#include "Game/Animation/BtpPlayer.hpp"
#include "Game/Animation/BvaPlayer.hpp"
#include "Game/Animation/XanimeCore.hpp"
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Animation/XanimeResource.hpp"
#include "Game/LiveActor/DisplayListMaker.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <cstdio>

#ifdef TARGET_PC
#include "port/diagnostics.h"
#if defined(__vita__) && PORT_LOGGING
extern "C" u32 port_workload_anim_family_begin(unsigned family);
extern "C" void port_workload_anim_family_end(u32 token);
extern "C" void port_workload_anim_material_checkpoint(u32 token, unsigned phase);
#endif
#endif

ModelManager::ModelManager()
    : mBtkPlayer(), mBrkPlayer(), mBtpPlayer(), mBpkPlayer(), mBvaPlayer(), mXanimeResourceTable(), mXanimePlayer(), mModel(), mModelResourceHolder(),
      mDisplayListMaker() {
}

void ModelManager::update() {
    XanimePlayer* pXanimePlayer = mXanimePlayer;

    if (pXanimePlayer != nullptr) {
        pXanimePlayer->updateBeforeMovement();
        pXanimePlayer->updateAfterMovement();
    }

    if (mBtkPlayer != nullptr) {
        mBtkPlayer->update();
    }

    if (mBrkPlayer != nullptr) {
        mBrkPlayer->update();
    }

    if (mBtpPlayer != nullptr) {
        mBtpPlayer->update();
    }

    if (mBpkPlayer != nullptr) {
        mBpkPlayer->update();
    }

    if (mBvaPlayer != nullptr) {
        mBvaPlayer->update();
    }
}

#ifdef TARGET_PC
// GXV_DL_LAZY (default 1 in the Vita player build, platform/src/port/perf.cpp): a turn that will draw nothing (frame skipping)
// still computes every model's joints (collision, attachments and the game's logic read them), but not its materials and
// material display lists (calcMaterial + DisplayListMaker::diff, ~40 % of the animation phase at the Observatory, 2 Nov): only
// a picture reads those, and the turn that draws next computes them again from the same state. A model that skipped them is
// listed, and GameSystem::frameLoop brings every listed model up to date at the start of the next turn that draws, before
// draw(): what that picture shows is exactly what it showed before (the materials of the latest turn's animation frames).
// The list holds models of the current scene only: it is dropped when a scene is destroyed (GameSystemSceneController).
extern "C" int port_turn_skipped_raw(void);
extern "C" int port_dl_lazy_on(void);
extern "C" void port_gx_drawdone_settle(int point);
extern "C" void port_gx_view_write_from(void*);
#include <vector>
#include "port/host_vector.h"  // (PortHostVector: lists that outlive a scene keep host memory)
#include "Game/LiveActor/LiveActor.hpp"
extern LiveActor* gPortCalcAnimOwner;          // (LiveActor.cpp: the actor whose model calcAnim is running, else null)
void port_live_actor_snapshot();
bool port_live_actor_valid(const LiveActor*);
namespace {
// Only the models of live actors are listed (by their actor), and a listed one is used only if its actor is still one of the
// scene's (a model freed inside a scene, the file select's for one, crashed the first version of this list on 3 Nov).
struct StaleDL { LiveActor* owner; ModelManager* model; };
PortHostVector< StaleDL > sStaleDL;  // (host heap: it outlives the scene; port/host_vector.h)
PORT_HEAP_GUARD(sStaleDL);
}  // namespace
extern "C" void port_dl_flush_stale(int drop) {
    if (sStaleDL.empty()) {
        return;
    }
    port_live_actor_snapshot();
    // (taken out first: updateDL can list models while this runs, and a reallocated list under the loop crashed on 3 Nov)
    PortHostVector< StaleDL > work;
    work.swap(sStaleDL);
    for (size_t i = 0; i < work.size(); ++i) {
        const StaleDL e = work[i];
        if (!port_live_actor_valid(e.owner) || e.owner->mModelManager != e.model) {
            continue;
        }
        if (!drop && e.model->mStaleDL) {
            e.model->updateDL(true);
        }
        e.model->mStaleDL = false;  // (drop: the scene is about to go; a model that outlives it is listed again)
    }
    work.clear();
    if (sStaleDL.empty() && sStaleDL.capacity() < work.capacity()) {
        sStaleDL.swap(work);  // (keep the larger buffer when nothing was listed meanwhile)
    }
}
extern "C" int port_gx_in_flush_scope(void);
// A list refresh asked for directly (MR::updateModelDiffDL), outside the drawn turn's flush: listed like ModelManager::calcAnim's.
extern "C" int port_on_frame_thread(void);  // platform/src/port/perf.cpp: only the main loop lists deferred work
bool port_dl_defer(LiveActor* owner, ModelManager* model) {
    if (port_dl_lazy_on() != 2 || port_gx_in_flush_scope() || owner == nullptr || model == nullptr || owner->mModelManager != model ||
        !port_on_frame_thread()) {
        return false;
    }
    if (!model->mStaleDL) {
        model->mStaleDL = true;
        sStaleDL.push_back(StaleDL{owner, model});
    }
    return true;
}
extern "C" unsigned port_dl_stale_count(void) { return static_cast< unsigned >(sStaleDL.size()); }

// GXV_ANIM_MEMO (default 1, platform/src/port/perf.cpp port_anim_memo_on; game3, 5 Nov): J3DModel::calc computes the joint matrices
// (and the weighted envelopes from them) from a few inputs only: the model's base matrix and scale, and either the joints' rest
// transforms (no BCK player) or its XanimeCore's tracks (BCK resource, frame, weight). A model whose inputs are those of its last
// computation, and whose matrix buffer still holds exactly what that computation wrote (compared with a copy: nobody else wrote
// into it since, not even a model sharing the buffer), would compute the same bytes again: it keeps them. Exact by construction.
// Static decorations and models whose animation is stopped or held at a frame (most of a hub's furniture, map parts) stop paying
// for their joints; a playing animation never matches and costs one key comparison.
// A model qualifies only when nothing else feeds its joints: no joint callback (JointController, head turning), no calc callback,
// deformers or CPU skinning, no matrix calculator on a joint other than its own player's core on the root, and a core with no
// transform list (outside matrices), no blend in progress (_1C < 1, _20 != 1, a freeze copy _29) and no special mode (_6 1, 2):
// then the core's per-joint state it writes (_28, _50) is a function of the same inputs, written with the same values.
// The copies live in one store (GXV_ANIM_MEMO_KB, default 1024 KiB, allocated at the first use), emptied with the scene; a model
// that does not fit is computed as before.
extern "C" int port_anim_memo_on(void);
extern "C" int port_anim_prof_on(void);
extern "C" void port_anim_prof_count(int what);
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <cstdlib>
#include <cstring>
namespace {
constexpr u32 kMemoKeyMax = 64;
struct AnimMemo {
    u32 keyWords;    // 0: no key
    u32 snapValid;   // the snapshot holds what the last computation with this key wrote
    u32 snapBytes;
    u8* snap;        // anm matrices, envelope matrices, scale flags, envelope scale flags (allocated at the first repeat)
    u32 key[kMemoKeyMax];
};
u8* sMemoStore = nullptr;
u32 sMemoSize = 0, sMemoUsed = 0, sMemoGen = 1;
bool sMemoFull = false;
void* MemoAlloc(u32 bytes) {
    if (sMemoStore == nullptr && sMemoSize == 0) {
        const char* e = getenv("GXV_ANIM_MEMO_KB");
        long kb = e && *e ? atol(e) : 1024;
        if (kb < 64) kb = 64;
        if (kb > 16384) kb = 16384;
        sMemoStore = static_cast< u8* >(malloc(static_cast< size_t >(kb) * 1024));
        sMemoSize = sMemoStore != nullptr ? static_cast< u32 >(kb) * 1024 : 1;  // (1: tried and failed; never again)
    }
    bytes = (bytes + 15u) & ~15u;
    if (sMemoStore == nullptr || sMemoUsed + bytes > sMemoSize) {
        sMemoFull = true;
        return nullptr;
    }
    void* p = sMemoStore + sMemoUsed;
    sMemoUsed += bytes;
    return p;
}
inline void KeyF(u32* key, u32& w, f32 f) {
    u32 v;
    memcpy(&v, &f, sizeof(v));
    key[w++] = v;
}
inline void KeyP(u32* key, u32& w, const void* p) {
    key[w++] = static_cast< u32 >(reinterpret_cast< uintptr_t >(p));  // (equality only; 32-bit on the console)
}
// The inputs of J3DModel::calc's joints, or 0 if the model does not qualify.
// (GXV_PAR_ANIM, joint15) frames != null: the key of the model's NEXT computation as predicted at the end of calc-anim: each track's
// frame is frames[t] instead of what its resource holds now, and the root's calculator may be the core or none (XanimePlayer::clearAnm
// has taken it off since); the rest is read as it is now.
// (GXV_PAR_ANIM_BLEND, par2) blend: a core in an interpolation or a freeze copy qualifies too (par-anim only; GXV_ANIM_MEMO never
// passes it): the key then ends with a word holding _29 (only then, so that a key outside a blend is the plain one, word for word).
// pred: the core's _1C, _20, _29 at the next computation as predicted (else read now). scale: the base scale the next computation
// will have (GXV_PAR_FLUSH: LiveActor::calcAnmMtx sets the actor's scale first), else the model's now.
struct KeyBlend {
    f32 b1C, b20;
    u8 b29;
};
// (GXV_PAR_ANIM_CB, par2) a joint callback par-anim's walk can take: kind 1 = NPCActor::calcJointScale through its
// JointControlDelegator< NPCActor > (MR::preScaleMtx of the joint's matrix by the actor's AnimScaleController::_C, timing 0; nothing
// at timing 1; the callback unregisters itself after the walk, JointController::staticCallBack).
struct ParCb {
    u8 kind;     // 0 none
    u16 joint;
    const void* ctl;  // the JointController
    Vec scale;
};
// (par2) what the key reads instead of the model's state now: GXV_PAR_FLUSH / GXV_PAR_LATE's scale and base matrix as the computation
// will have them; cbMode: a recognised joint callback is let through (registered now: frames == null; or cb: as predicted).
struct ParOverride {
    const Vec* scale;
    const f32 (*base)[4];
    bool cbMode;
    const ParCb* cb;  // (prediction; null with frames == null: read from the joints now)
    ParCb* cbOut;     // (the callback found, when cbMode)
};
bool ParRecogniseCb(J3DModel* model, J3DJoint* joint, const void* ctl, ParCb* out);
u32 MemoKey(ModelManager* mm, J3DModel* model, u32* key, const f32* frames = nullptr, bool blend = false, const KeyBlend* pred = nullptr,
            const ParOverride* ov = nullptr) {
    J3DModelData* data = model->getModelData();
    J3DMtxBuffer* buf = model->getMtxBuffer();
    if (data == nullptr || buf == nullptr || buf->mJointTree == nullptr || model->mCalcCallBack != nullptr || model->mDeformData != nullptr ||
        model->mSkinDeform != nullptr || model->mVtxColorCalc != nullptr || model->mUnkCalc1 != nullptr || model->mUnkCalc2 != nullptr ||
        model->checkFlag(J3DMdlFlag_SkinPosCpu) || model->checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        return 0;
    }
    const u16 jointNum = data->getJointNum();
    if (jointNum == 0 || buf->mJointTree->getJointNum() != jointNum || buf->mJointTree->getWEvlpMtxNum() != data->getWEvlpMtxNum() ||
        buf->mpAnmMtx == nullptr || buf->mpScaleFlagArr == nullptr) {
        return 0;
    }
    XanimeCore* core = nullptr;
    if (mm->mXanimePlayer != nullptr) {
        core = mm->mXanimePlayer->mCore;
        if (core == nullptr || core->mTransformList != nullptr || (core->_6 != 0 && core->_6 != 3) || core->mTrackCount > 8) {
            return 0;
        }
    }
    const f32 k1C = core == nullptr ? 0.0f : (pred != nullptr ? pred->b1C : core->_1C);
    const f32 k20 = core == nullptr ? 0.0f : (pred != nullptr ? pred->b20 : core->_20);
    const u8 k29 = core == nullptr ? 0 : (pred != nullptr ? pred->b29 : core->_29);
    const bool inBlend = core != nullptr && (k29 != 0 || k1C < 1.0f || k20 != 1.0f);
    if (inBlend && !blend) {
        return 0;
    }
    ParCb cbFound;
    cbFound.kind = 0;
    for (u16 i = 0; i < jointNum; i++) {
        J3DJoint* joint = data->getJointNodePointer(i);
        if (joint->getCallBack() != nullptr && ov != nullptr && ov->cbMode && frames == nullptr && cbFound.kind == 0 &&
            joint->getMtxCalc() == (i == 0 ? static_cast< J3DMtxCalc* >(core) : nullptr) && ParRecogniseCb(model, joint, joint->mCallBackUserData, &cbFound)) {
            cbFound.joint = i;
            continue;  // (GXV_PAR_ANIM_CB: registered now, recognised; its inputs go into the key below)
        }
        if (joint->getCallBack() != nullptr || joint->getMtxCalc() != (i == 0 ? static_cast< J3DMtxCalc* >(core) : nullptr)) {
            if (frames == nullptr || i != 0 || joint->getCallBack() != nullptr || joint->getMtxCalc() != nullptr) {
                return 0;
            }
        }
    }
    if (ov != nullptr && ov->cbMode && frames != nullptr && ov->cb != nullptr && ov->cb->kind != 0) {
        cbFound = *ov->cb;  // (the prediction: registered by the computation's own calcAndSetBaseMtx)
    }
    if (ov != nullptr && ov->cbOut != nullptr) {
        *ov->cbOut = cbFound;
    }
    u32 w = 0;
    KeyP(key, w, model);
    KeyP(key, w, buf);
    key[w++] = model->mFlags;
    const Vec& bs = ov != nullptr && ov->scale != nullptr ? *ov->scale : static_cast< const Vec& >(model->mBaseScale);
    const f32(*bm)[4] = ov != nullptr && ov->base != nullptr ? ov->base : model->mBaseTransformMtx;
    KeyF(key, w, bs.x);
    KeyF(key, w, bs.y);
    KeyF(key, w, bs.z);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            KeyF(key, w, bm[r][c]);
        }
    }
    if (core != nullptr) {
        KeyP(key, w, core);
        key[w++] = (static_cast< u32 >(core->_4) << 24) | (static_cast< u32 >(core->_6) << 16) | (static_cast< u32 >(core->mTrackCount) << 8);
        key[w++] = core->_C;
        KeyF(key, w, k1C);
        KeyF(key, w, k20);
        for (u32 t = 0; t < core->mTrackCount; t++) {
            const XanimeTrack& track = core->mTrackList[t];
            KeyP(key, w, track._0);
            KeyF(key, w, track.mWeight);
            KeyF(key, w, track._0 != nullptr ? (frames != nullptr ? frames[t] : track._0->getFrame()) : 0.0f);
        }
        if (inBlend) {
            key[w++] = 0xB1E0D000u | k29;  // (GXV_PAR_ANIM_BLEND)
        }
    } else {
        key[w++] = 0xFFFFFFFFu;
    }
    if (cbFound.kind != 0) {
        // (GXV_PAR_ANIM_CB: which callback, on which joint, through which controller, with which inputs)
        key[w++] = 0xCB000000u | (static_cast< u32 >(cbFound.kind) << 16) | cbFound.joint;
        KeyP(key, w, cbFound.ctl);
        KeyF(key, w, cbFound.scale.x);
        KeyF(key, w, cbFound.scale.y);
        KeyF(key, w, cbFound.scale.z);
    }
    return w;  // (at most 2 + 1 + 3 + 12 + 1 + 4 + 3 * 8 + 1 + 5 = 53 < kMemoKeyMax)
}
// The bytes the joint computation writes into the buffer: anm matrices, envelopes and both scale flag arrays.
u32 MemoSnapBytes(J3DMtxBuffer* buf) {
    const u32 joints = buf->mJointTree->getJointNum(), envelopes = buf->mJointTree->getWEvlpMtxNum();
    u32 bytes = joints * sizeof(Mtx) + joints;
    if (envelopes != 0 && buf->mpWeightEvlpMtx != nullptr && buf->mpEvlpScaleFlagArr != nullptr) {
        bytes += envelopes * sizeof(Mtx) + envelopes;
    }
    return bytes;
}
void MemoCopy(J3DMtxBuffer* buf, u8* out) {
    const u32 joints = buf->mJointTree->getJointNum(), envelopes = buf->mJointTree->getWEvlpMtxNum();
    memcpy(out, buf->mpAnmMtx, joints * sizeof(Mtx));
    out += joints * sizeof(Mtx);
    memcpy(out, buf->mpScaleFlagArr, joints);
    out += joints;
    if (envelopes != 0 && buf->mpWeightEvlpMtx != nullptr && buf->mpEvlpScaleFlagArr != nullptr) {
        memcpy(out, buf->mpWeightEvlpMtx, envelopes * sizeof(Mtx));
        out += envelopes * sizeof(Mtx);
        memcpy(out, buf->mpEvlpScaleFlagArr, envelopes);
    }
}
bool MemoSame(J3DMtxBuffer* buf, const u8* in) {
    const u32 joints = buf->mJointTree->getJointNum(), envelopes = buf->mJointTree->getWEvlpMtxNum();
    if (memcmp(in, buf->mpAnmMtx, joints * sizeof(Mtx)) != 0) {
        return false;
    }
    in += joints * sizeof(Mtx);
    if (memcmp(in, buf->mpScaleFlagArr, joints) != 0) {
        return false;
    }
    in += joints;
    if (envelopes != 0 && buf->mpWeightEvlpMtx != nullptr && buf->mpEvlpScaleFlagArr != nullptr) {
        if (memcmp(in, buf->mpWeightEvlpMtx, envelopes * sizeof(Mtx)) != 0) {
            return false;
        }
        in += envelopes * sizeof(Mtx);
        if (memcmp(in, buf->mpEvlpScaleFlagArr, envelopes) != 0) {
            return false;
        }
    }
    return true;
}
// What J3DModel::calc does before its joints, for a model that qualifies (no CPU skinning, no deformers): the state of j3dSys and
// the vertex buffer it leaves for what follows (calcMaterial, the display lists).
void MemoPrologue(J3DModel* model) {
    j3dSys.setModel(model);
    j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
    j3dSys.offFlag(J3DSysFlag_SkinNrmCpu);
    model->getModelData()->syncJ3DSysFlags();
    model->mVertexBuffer.frameInit();
}
}  // namespace
// The scene's models go with its heap (GameSystemSceneController::exeDestroyScene): the store starts again, and a memo of an
// earlier generation is never read (a model that outlives the scene gets a new one).
extern "C" int port_view_keep_on(void);
extern "C" int port_mtx_notify_on(void);
namespace {
// GXV_VIEW_KEEP (draw7, 5 Nov; default 0, platform/src/port/perf.cpp port_view_keep_on): J3DModel::viewCalc swaps the model's draw
// and normal matrix buffers and computes into the half the last picture did not read: draw = view x joint (or envelope) matrix,
// normal = its inverse transpose (or a copy where the scale is 1), billboards from the draw matrices. In its plain case (no CPU
// skinning, no view-concatenated model data, no bump matrices, one view, a real double buffer) it reads only: the camera's view
// matrix (j3dSys), the model's flags, base scale and base matrix, the joint and envelope matrices and their scale flags, and model
// data that does not change. A model whose inputs are those of its last view pass, and whose buffer nobody swapped, wrote into or
// pointed elsewhere since (J3DMtxBuffer::mPortSerial; the current halves are the ones it wrote), would compute the same bytes into
// the other half: it keeps the half it has (no swap, no computation). The front end reads that half for this picture too, as it did
// for the last one, and nothing writes into it; the next view pass that changes anything swaps as always, into the half the last
// picture did not read. Exact by construction (bitwise compares).
// The inputs are compared in two steps, as GXV_ANIM_MEMO does: the key (view matrix, base matrix and scale, flags, the buffer's
// pointers) every time, a copy of the joint and envelope matrices only when the key repeats (a moving camera never pays for one).
// A model whose joints keep moving under a still camera stops taking copies for 120 view passes after three misses in a row.
constexpr u32 kViewKeyMax = 40;
struct ViewMemo {
    u32 keyWords;
    u32 serial;        // the buffer's write serial after the view pass this memo describes
    const void* draw;  // the current halves it left
    const void* nrm;
    u32 snapValid;
    u32 snapBytes;
    u8* snap;  // joint matrices, joint scale flags, envelope matrices, envelope scale flags (MemoCopy's layout)
    u32 misses;
    u32 quiet;
    u32 key[kViewKeyMax];
};
u8* sViewStore = nullptr;
u32 sViewSize = 0, sViewUsed = 0, sViewGen = 1;
bool sViewFull = false;
u32 sViewKept = 0, sViewMade = 0;  // (the log line)
void* ViewAlloc(u32 bytes) {
    if (sViewStore == nullptr && sViewSize == 0) {
        const char* e = getenv("GXV_VIEW_KEEP_KB");
        long kb = e && *e ? atol(e) : 1024;
        if (kb < 64) kb = 64;
        if (kb > 16384) kb = 16384;
        sViewStore = static_cast< u8* >(malloc(static_cast< size_t >(kb) * 1024));
        sViewSize = sViewStore != nullptr ? static_cast< u32 >(kb) * 1024 : 1;  // (1: tried and failed; never again)
    }
    bytes = (bytes + 15u) & ~15u;
    if (sViewStore == nullptr || sViewUsed + bytes > sViewSize) {
        sViewFull = true;
        return nullptr;
    }
    void* p = sViewStore + sViewUsed;
    sViewUsed += bytes;
    return p;
}
// The view pass's inputs other than the joint matrices, or 0 if the model's view pass is not the plain case.
u32 ViewKey(J3DModel* model, u32* key) {
    J3DModelData* data = model->getModelData();
    J3DMtxBuffer* buf = model->getMtxBuffer();
    if (data == nullptr || buf == nullptr || buf->mJointTree == nullptr || data->checkFlag(0x10) || model->isCpuSkinningOn() ||
        model->checkFlag(J3DMdlFlag_SkinPosCpu) || model->checkFlag(J3DMdlFlag_SkinNrmCpu) || data->checkBumpFlag() || buf->mMtxNum != 1 ||
        buf->mCurrentViewNo != 0 || buf->mpDrawMtxArr[0] == nullptr || buf->mpDrawMtxArr[1] == nullptr || buf->mpNrmMtxArr[0] == nullptr ||
        buf->mpNrmMtxArr[1] == nullptr || buf->mpDrawMtxArr[0] == buf->mpDrawMtxArr[1] || buf->mpNrmMtxArr[0] == buf->mpNrmMtxArr[1] ||
        buf->mpDrawMtxArr[0][0] == buf->mpDrawMtxArr[1][0] || buf->mpNrmMtxArr[0][0] == buf->mpNrmMtxArr[1][0] || buf->mpAnmMtx == nullptr ||
        buf->mpScaleFlagArr == nullptr || buf->mJointTree->getJointNum() == 0 ||
        (buf->mJointTree->getWEvlpMtxNum() != 0 && (buf->mpWeightEvlpMtx == nullptr || buf->mpEvlpScaleFlagArr == nullptr)) ||
        port_mtx_notify_on()) {
        return 0;
    }
    u32 w = 0;
    KeyP(key, w, model);
    KeyP(key, w, buf);
    key[w++] = model->mFlags;
    KeyP(key, w, buf->mpAnmMtx);
    KeyP(key, w, buf->mpWeightEvlpMtx);
    KeyP(key, w, buf->mpScaleFlagArr);
    KeyP(key, w, buf->mpEvlpScaleFlagArr);
    KeyF(key, w, model->mBaseScale.x);
    KeyF(key, w, model->mBaseScale.y);
    KeyF(key, w, model->mBaseScale.z);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            KeyF(key, w, model->mBaseTransformMtx[r][c]);
        }
    }
    const MtxPtr view = j3dSys.getViewMtx();
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            KeyF(key, w, view[r][c]);
        }
    }
    return w;  // (7 + 3 + 12 + 12 = 34 <= kViewKeyMax)
}
}  // namespace
// ModelManager::calcView's view pass (true: kept or computed here); false: the caller computes it as before.
static bool PortViewKeepCalc(ModelManager* mm) {
    if (!port_view_keep_on() || !port_on_frame_thread()) {
        return false;
    }
    J3DModel* model = mm->getJ3DModel();
    if (model == nullptr) {
        return false;
    }
    ViewMemo* memo = mm->mViewMemoGen == sViewGen ? static_cast< ViewMemo* >(mm->mViewMemo) : nullptr;
    u32 key[kViewKeyMax];
    const u32 words = ViewKey(model, key);
    if (words == 0) {
        if (memo != nullptr) {
            memo->keyWords = 0;  // (computed by others' rules this time: the next match starts again)
            memo->snapValid = 0;
        }
        return false;
    }
    if (memo == nullptr) {
        memo = static_cast< ViewMemo* >(ViewAlloc(sizeof(ViewMemo)));
        if (memo == nullptr) {
            return false;
        }
        memset(memo, 0, sizeof(ViewMemo));
        mm->mViewMemo = memo;
        mm->mViewMemoGen = sViewGen;
    }
    J3DMtxBuffer* buf = model->getMtxBuffer();
    const bool sameKey = memo->keyWords == words && memcmp(memo->key, key, words * sizeof(u32)) == 0;
    if (sameKey && memo->snapValid && buf->mPortSerial == memo->serial && buf->mpDrawMtxArr[1][0] == memo->draw &&
        buf->mpNrmMtxArr[1][0] == memo->nrm && MemoSame(buf, memo->snap)) {
        model->prepareShapePackets();  // (what viewCalc does last: the packets' buffer and base matrix pointers)
        memo->misses = 0;
        ++sViewKept;
        return true;
    }
    model->viewCalc();
    ++sViewMade;
    memo->serial = buf->mPortSerial;
    memo->draw = buf->mpDrawMtxArr[1][0];
    memo->nrm = buf->mpNrmMtxArr[1][0];
    if (!sameKey) {
        memcpy(memo->key, key, words * sizeof(u32));
        memo->keyWords = words;
        memo->snapValid = 0;  // (a copy is taken only when the key repeats)
        memo->misses = 0;
        memo->quiet = 0;
    } else if (memo->quiet != 0) {
        --memo->quiet;
        memo->snapValid = 0;
    } else if (memo->snapValid && ++memo->misses >= 3) {
        memo->misses = 0;
        memo->quiet = 120;  // (joints that move every picture under a still camera: no copies for a while)
        memo->snapValid = 0;
    } else {
        const u32 bytes = MemoSnapBytes(buf);
        if (memo->snap == nullptr || memo->snapBytes != bytes) {
            memo->snap = static_cast< u8* >(ViewAlloc(bytes));
            memo->snapBytes = memo->snap != nullptr ? bytes : 0;
        }
        memo->snapValid = 0;
        if (memo->snap != nullptr) {
            MemoCopy(buf, memo->snap);
            memo->snapValid = 1;
        }
    }
#if PORT_LOGGING
    if (sViewKept + sViewMade >= 16384) {
        port_log_summary("view-keep: %u of the last %u view passes kept (GXV_VIEW_KEEP); store %u of %u KiB%s", sViewKept, sViewKept + sViewMade,
                         sViewUsed / 1024, sViewSize / 1024, sViewFull ? " (was full: some models computed as before)" : "");
        sViewKept = sViewMade = 0;
    }
#endif
    return true;
}
extern "C" void port_par_anim_drop(void);
extern "C" void port_par_view_retire(void);  // (GXV_PAR_VIEW=2, below)
extern "C" void port_anim_memo_drop(void) {
    port_par_view_retire();  // (GXV_PAR_VIEW=2: nothing is outstanding outside a picture's draw; made sure of before the models go)
    port_par_anim_drop();  // (GXV_PAR_ANIM: the worker leaves its batch before the scene's memory goes; its records go too)
    sViewUsed = 0;  // (GXV_VIEW_KEEP: its store goes with the scene's models as well)
    ++sViewGen;
    sViewFull = false;
    sMemoUsed = 0;
    ++sMemoGen;
    if (sMemoFull && port_anim_memo_on()) {
        port_log_summary("anim-memo: the store (GXV_ANIM_MEMO_KB, %u KiB) was full in the last scene: some models were computed as before",
                         sMemoSize / 1024);
    }
    sMemoFull = false;
}
// GXV_PAR_ANIM (below): the joints of a qualifying model taken from the worker's speculative computation when its key matches
// (true), else false (the caller computes them as before). key / words: MemoKey's, as read now (after XanimePlayer::calcAnm).
static bool PortParAnimTake(ModelManager* mm, J3DModel* model, const u32* key, u32 words, const ParCb* cb = nullptr);
// true: the joints are as J3DModel::calc would leave them (kept or computed here); false: the caller computes as before.
static bool PortAnimMemoCalc(ModelManager* mm) {
    if (!port_anim_memo_on() || !port_on_frame_thread()) {
        return false;
    }
    J3DModel* model = mm->getJ3DModel();
    AnimMemo* memo = mm->mAnimMemoGen == sMemoGen ? static_cast< AnimMemo* >(mm->mAnimMemo) : nullptr;
    u32 key[kMemoKeyMax];
    const u32 words = MemoKey(mm, model, key);
    if (words == 0) {
        if (memo != nullptr) {
            memo->keyWords = 0;  // (computed by others' rules this time: the next match starts again)
            memo->snapValid = 0;
        }
        return false;
    }
    if (memo == nullptr) {
        memo = static_cast< AnimMemo* >(MemoAlloc(sizeof(AnimMemo)));
        if (memo == nullptr) {
            return false;
        }
        memo->keyWords = 0;
        memo->snapValid = 0;
        memo->snapBytes = 0;
        memo->snap = nullptr;
        mm->mAnimMemo = memo;
        mm->mAnimMemoGen = sMemoGen;
    }
    J3DMtxBuffer* buf = model->getMtxBuffer();
    const bool sameKey = memo->keyWords == words && memcmp(memo->key, key, words * sizeof(u32)) == 0;
    if (sameKey && memo->snapValid && MemoSame(buf, memo->snap)) {
        MemoPrologue(model);
#if PORT_LOGGING
        if (port_anim_prof_on()) {
            port_anim_prof_count(1);
        }
#endif
        return true;
    }
    if (!PortParAnimTake(mm, model, key, words)) {
        model->calc();
    }
    if (!sameKey) {
        memcpy(memo->key, key, words * sizeof(u32));
        memo->keyWords = words;
        memo->snapValid = 0;  // (a copy is taken only when a key repeats: a playing animation never pays for one)
        return true;
    }
    const u32 bytes = MemoSnapBytes(buf);
    if (memo->snap == nullptr || memo->snapBytes != bytes) {
        memo->snap = static_cast< u8* >(MemoAlloc(bytes));
        memo->snapBytes = memo->snap != nullptr ? bytes : 0;
    }
    if (memo->snap != nullptr) {
        MemoCopy(buf, memo->snap);
        memo->snapValid = 1;
    }
    return true;
}

// GXV_PAR_VIEW (par13, 5 Nov; default 0, platform/src/port/perf.cpp port_par_view_on; executor platform/include/port/par_exec.h):
// the view pass's matrices (draw = view x joint for every draw matrix, the normal matrices, billboards: J3DMtxBuffer's calcDrawMtx,
// calcNrmMtx, calcBBoardMtx) shared between the game thread and a worker on another core. The pass itself (the actor lists, the
// virtual calls, Mario's own view passes, the 2D entries) stays on the game thread in its order; for a model whose viewCalc is the
// plain case it does what viewCalc does in order (the write check, the draw / normal buffer swaps, the serial count, the shape
// packets' pointers) and records the arithmetic as a job: the view matrix, base matrix and scale by value, the buffer's arrays as
// they are now. When the pass is over, the jobs run on both threads, and the game thread waits for all of them before it returns
// (before the picture is drawn). Each job writes only the draw and normal half its own swap chose.
// Exact: the arithmetic is J3DMtxBuffer's own code (the same functions), from the same inputs:
//   - what the job reads: the joint, envelope and scale flag arrays of its buffer. Nothing in the view pass writes them except
//     J3DModel::calc (ModelHolder::updateMtx in Mario's view pass, for one) and a second viewCalc of a model sharing the buffer
//     (an LOD model and its high model): both first run the pending job of that buffer (port_par_view_touch from J3DModel::calc and
//     ::viewCalc; a second record of the same buffer does the same), so the job sees what the serial pass saw;
//   - what it writes: the half its swap chose. Nothing in the view pass reads another model's draw or normal matrices (only Mario's
//     shadow and blur read his own J3DModelX's, which is never recorded); the draw comes after the join;
//   - j3dSys's view matrix is copied when the job is recorded (MarioActor's search light writes j3dSys's view matrix in the pass).
// Only a plain J3DModel qualifies (not J3DModelX or J3DModel2: their own view functions write their buffers), with no view-concatenated
// model data, no CPU skinning, no bump matrices, calc mode 0 or 2 (mode 1's second copy loop runs past its arrays when the envelopes are
// fewer than the full-weight matrices; nothing in the game uses it), a real double buffer and GXV_MTX_NOTIFY off; anything else is
// computed as before.
// Only on the main loop thread, inside the GameScene view pass (port_view_flush, GameScene::calcAnim).
#include "port/par_batch.h"
#include "port/par_async.h"
#include <new>
extern "C" int port_par_view_on(void);
extern "C" unsigned port_par_view_max(void);
extern "C" int port_par_verify_on(void);
extern "C" void port_par_run_batch(void (*fn)(void*, unsigned), void* ctx, unsigned n, unsigned inlineRuns, unsigned overflow);
// GXV_PAR_VIEW=2 (drawpar, 6 Nov; platform/include/port/par_async.h, platform/src/port/perf.cpp): the drawn turn's view pass
// (port_view_flush, armed by GameSystem::frameLoop because draw() follows it at once) records its jobs as in mode 1, but there is no
// join: the jobs go to a worker (GXV_PAR_CORE, default the fourth core when it is proven) and the game thread goes straight on into
// draw(). What reads a job's results first makes sure the job is done (it takes it if the worker has not, or waits for the few
// microseconds the worker still needs):
//   - J3DSys::setModelDrawMtx / setModelNrmMtx (J3DSys.hpp): the one place a draw or normal half is handed to GX (GXSetArray of the
//     position / normal matrix arrays) for the indexed loads the front end makes from memory, and the base the immediate loads of
//     J3DShapeMtx read; the FIFO commands that make the front end read the half are written after it, so the front end (core 2) sees
//     the finished bytes (the job's release, the game thread's acquire, then the FIFO's own publication);
//   - NormalMapBase::drawShape, the one game reader of another model's draw matrices in the draw (immediate loads);
//   - J3DModel::calc and ::viewCalc (a joint computation or a second view pass of the same buffer in the draw: the mirror);
//   - GameSystem::frameLoop right after draw() (port_par_view_retire): every job done, the worker out of the batch, before
//     endRender and the next turn's logic. Nothing of the batch outlives the picture's draw.
// So every byte is the serial pass's (the same code on the same inputs: par13's argument), written later but before any reader.
// Readers of draw matrices that are never recorded (J3DModelX: Mario's shadow and blur) are unchanged.
extern "C" int port_par_view_async_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n);
extern "C" void port_par_view_async_quiesce(void);
extern "C" void port_par_view_async_report(const unsigned* counters, unsigned retireUs);
extern "C" int64_t port_host_time_ns(void);
bool gPortParViewOpen = false;   // (read by J3DModel::calc and ::viewCalc: a plain load while no batch is open or outstanding)
bool gPortParViewAsync = false;  // (GXV_PAR_VIEW=2: jobs outstanding; read by J3DSys.hpp's setModelDrawMtx / setModelNrmMtx)
namespace {
struct ParViewJob {
    J3DJointTree* tree;
    Mtx* anm;
    Mtx* wevlp;
    const u8* scaleFlag;
    const u8* envScaleFlag;
    Mtx* draw;
    Mtx33* nrm;
    u32 mode;
    u32 bboard;
    Vec scale;
    Mtx base;
    Mtx view;
    u32 verifySum;  // (GXV_PAR_VERIFY: the checksum of what the job wrote when computed at its place in the pass; 0: not checked)
};
void ParViewRun(ParViewJob& j) {
    // (J3DModel::viewCalc's plain case: calcDrawMtx, calcNrmMtx, calcBBoardMtx; calcBumpMtx has nothing to do for these models)
    J3DMtxBuffer::portCalcDrawMtxCore(j.tree, j.mode, j.scale, j.base, j.view, j.anm, j.wevlp, j.draw);
    J3DMtxBuffer::portCalcNrmMtxCore(j.tree, j.scaleFlag, j.envScaleFlag, j.draw, j.nrm);
    if (j.bboard) {
        J3DMtxBuffer::portCalcBBoardMtxCore(j.tree, j.draw, j.nrm);
    }
}
port::par::PendingBatch< ParViewJob > sParView;
port::par::AsyncBatch< ParViewJob > sParViewA;  // (GXV_PAR_VIEW=2)
int sParViewDepth = 0;
bool sParRecording = false;     // a view pass is recording jobs (PortParViewRecord)
bool sParAsyncRecording = false;  // ... into sParViewA
bool sParAsyncArmed = false;    // GameSystem::frameLoop: draw() follows this view pass (port_par_view_async_arm)
// GXV_PAR_VERIFY=1 (diagnostic): every job is also computed when it is recorded, into scratch memory that starts as a copy of its
// destination, i.e. at its place in the serial pass; after the join, what the batch wrote must have the same checksum. A mismatch is
// logged (it would mean an input of a pending job changed in the pass, or a race).
constexpr u32 kVerifyMtx = 512;
Mtx sVerifyDraw[kVerifyMtx];
Mtx33 sVerifyNrm[kVerifyMtx];
u32 sVerifyChecked = 0, sVerifyBad = 0, sVerifyLogged = 0;
u32 ParSum(const ParViewJob& j, const Mtx* draw, const Mtx33* nrm) {
    const u32 num = j.tree->getDrawMtxNum();
    u32 h = 2166136261u;
    const u8* p = reinterpret_cast< const u8* >(draw);
    for (u32 i = 0; i < num * sizeof(Mtx); ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    p = reinterpret_cast< const u8* >(nrm);
    for (u32 i = 0; i < num * sizeof(Mtx33); ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h != 0 ? h : 1;
}
void ParVerifyRecord(ParViewJob& j) {
    const u32 num = j.tree->getDrawMtxNum();
    if (num == 0 || num > kVerifyMtx) {
        return;
    }
    memcpy(sVerifyDraw, j.draw, num * sizeof(Mtx));
    memcpy(sVerifyNrm, j.nrm, num * sizeof(Mtx33));
    ParViewJob t = j;
    t.draw = sVerifyDraw;
    t.nrm = sVerifyNrm;
    ParViewRun(t);
    j.verifySum = ParSum(j, sVerifyDraw, sVerifyNrm);
}
template < class Batch >
void ParVerifyAfter(Batch& batch, bool async) {
    for (u32 i = 0; i < batch.Count(); ++i) {
        const ParViewJob& j = batch.At(i);
        if (j.verifySum == 0 || !batch.IsLatest(i) || (async && sParViewA.TouchedAfterDispatch(i))) {
            continue;  // (async: a buffer touched in the draw may have been written again since, as the serial draw would have)
        }
        ++sVerifyChecked;
        if (ParSum(j, j.draw, j.nrm) != j.verifySum) {
            ++sVerifyBad;
            if (sVerifyLogged < 16) {
                ++sVerifyLogged;
                port_log_summary("par-view: verify MISMATCH: job %u of %u, %u draw matrices, mode %u (the parallel result differs from the serial one)%s",
                                 i, batch.Count(), static_cast< unsigned >(j.tree->getDrawMtxNum()), static_cast< unsigned >(j.mode),
                                 async ? " (GXV_PAR_VIEW=2)" : "");
            }
        }
    }
    if (sVerifyChecked >= 20000) {
        port_log_summary("par-view: verify: %u jobs checked, %u mismatches (GXV_PAR_VERIFY)", sVerifyChecked, sVerifyBad);
        sVerifyChecked = 0;
        sVerifyBad = 0;
    }
}
// The vtable of a plain J3DModel: a model of a derived class (J3DModelX, J3DModel2) is never recorded.
const void* PlainJ3DModelVtable() {
    alignas(J3DModel) static unsigned char sProbe[sizeof(J3DModel)];  // (made once, never destroyed: an empty model, nothing allocated)
    static const void* const vt = *reinterpret_cast< const void* const* >(new (sProbe) J3DModel());
    return vt;
}
// true: recorded (or run at once because the batch is full); false: the caller computes it as before.
bool PortParViewRecord(J3DModel* model) {
    if (!sParRecording || model == nullptr || *reinterpret_cast< const void* const* >(model) != PlainJ3DModelVtable()) {
        return false;
    }
    J3DModelData* data = model->getModelData();
    J3DMtxBuffer* buf = model->getMtxBuffer();
    if (data == nullptr || buf == nullptr || buf->mJointTree == nullptr || data->checkFlag(0x10) || model->isCpuSkinningOn() ||
        model->checkFlag(J3DMdlFlag_SkinPosCpu) || model->checkFlag(J3DMdlFlag_SkinNrmCpu) || data->checkBumpFlag() ||
        (model->getMtxCalcMode() != 0 && model->getMtxCalcMode() != 2) || buf->mCurrentViewNo >= buf->mMtxNum || buf->mpDrawMtxArr[0] == nullptr || buf->mpDrawMtxArr[1] == nullptr ||
        buf->mpNrmMtxArr[0] == nullptr || buf->mpNrmMtxArr[1] == nullptr || buf->mpAnmMtx == nullptr || buf->mpScaleFlagArr == nullptr ||
        port_mtx_notify_on()) {
        return false;
    }
    const u32 v = buf->mCurrentViewNo;
    Mtx* const d0 = buf->mpDrawMtxArr[0][v];
    Mtx* const d1 = buf->mpDrawMtxArr[1][v];
    Mtx33* const n0 = buf->mpNrmMtxArr[0][v];
    Mtx33* const n1 = buf->mpNrmMtxArr[1][v];
    if (d0 == nullptr || d1 == nullptr || n0 == nullptr || n1 == nullptr || d0 == d1 || n0 == n1 || d0 == J3DMtxBuffer::sNoUseDrawMtxPtr ||
        d1 == J3DMtxBuffer::sNoUseDrawMtxPtr || n0 == J3DMtxBuffer::sNoUseNrmMtxPtr || n1 == J3DMtxBuffer::sNoUseNrmMtxPtr ||
        (buf->mJointTree->getWEvlpMtxNum() != 0 && (buf->mpWeightEvlpMtx == nullptr || buf->mpEvlpScaleFlagArr == nullptr))) {
        return false;  // (no real double buffer of its own: computed as before)
    }
    // (an earlier job of this buffer runs first; async: the job's outputs are the halves the swap below makes current, d0 and n0)
    ParViewJob* job = sParAsyncRecording ? sParViewA.Add(buf, d0, n0) : sParView.Add(buf);
    // What viewCalc does before its arithmetic, in its order.
    port_gx_view_write_from(__builtin_return_address(0));
    buf->swapDrawMtx();
    buf->swapNrmMtx();
    if (job == nullptr) {
        // The batch is full: computed now, as viewCalc would (the serial count as calcDrawMtx / calcNrmMtx / calcBBoardMtx move it).
        buf->calcDrawMtx(model->getMtxCalcMode(), model->mBaseScale, model->mBaseTransformMtx);
        buf->calcNrmMtx();
        if (data->checkBBoardFlag()) {
            buf->calcBBoardMtx();
        }
        model->prepareShapePackets();
        return true;
    }
    job->tree = buf->mJointTree;
    job->anm = buf->mpAnmMtx;
    job->wevlp = buf->mpWeightEvlpMtx;
    job->scaleFlag = buf->mpScaleFlagArr;
    job->envScaleFlag = buf->mpEvlpScaleFlagArr;
    job->draw = buf->mpDrawMtxArr[1][v];
    job->nrm = buf->mpNrmMtxArr[1][v];
    job->mode = model->getMtxCalcMode();
    job->bboard = data->checkBBoardFlag() ? 1 : 0;
    job->scale = model->mBaseScale;
    PSMTXCopy(model->mBaseTransformMtx, job->base);
    PSMTXCopy(j3dSys.getViewMtx(), job->view);
    buf->portTouch();  // (GXV_VIEW_KEEP's serial moves as calcDrawMtx, calcNrmMtx and calcBBoardMtx would move it)
    buf->portTouch();
    if (job->bboard) {
        buf->portTouch();
    }
    job->verifySum = 0;
    if (port_par_verify_on()) {
        ParVerifyRecord(*job);
    }
    model->prepareShapePackets();  // (pointers only: the buffer and j3dSys's view matrix, as viewCalc sets them)
    return true;
}
}  // namespace
// The GameScene view pass's begin and end (GameScene.cpp). end runs the recorded jobs on both threads and returns when all are done;
// with GXV_PAR_VIEW=2 and the pass armed by GameSystem::frameLoop, it hands them to the worker and returns at once (see above).
extern "C" void port_par_view_async_arm(int on) { sParAsyncArmed = on != 0; }
extern "C" void port_par_view_retire(void);
extern "C" void port_par_view_begin(void) {
    if (sParViewDepth++ != 0) {
        return;
    }
    const int mode = port_par_view_on();
    if (mode == 0 || !port_on_frame_thread()) {
        return;
    }
    port_par_view_retire();  // (none is outstanding here: the last picture's draw retired it)
    if (mode == 2 && sParAsyncArmed) {
        if (!sParViewA.Ready() && !sParViewA.Init(port_par_view_max(), &ParViewRun)) {
            return;
        }
        (void)PlainJ3DModelVtable();
        sParViewA.Begin();
        sParAsyncRecording = true;
        sParRecording = true;
        gPortParViewOpen = true;
        return;
    }
    if (!sParView.Ready() && !sParView.Init(port_par_view_max(), &ParViewRun)) {
        return;
    }
    (void)PlainJ3DModelVtable();
    sParView.Begin();
    sParRecording = true;
    gPortParViewOpen = true;
}
extern "C" void port_par_view_end(void) {
    if (sParViewDepth <= 0 || --sParViewDepth != 0 || !sParRecording) {
        return;
    }
    sParRecording = false;
    if (sParAsyncRecording) {
        sParAsyncRecording = false;
        if (sParViewA.Dispatch() == 0) {
            gPortParViewOpen = false;  // (nothing pending: every job ran in the walk)
            return;
        }
        gPortParViewAsync = true;  // (gPortParViewOpen stays set: J3DModel::calc and ::viewCalc touch until the retire)
        if (!port_par_view_async_run(&port::par::AsyncBatch< ParViewJob >::Item, &sParViewA, sParViewA.Items(), sParViewA.Count())) {
            port_par_view_retire();  // (no worker: every job now, in order, as mode 1 would without its worker)
        }
        return;
    }
    gPortParViewOpen = false;
    sParView.Close();
    port_par_run_batch(&port::par::PendingBatch< ParViewJob >::Item, &sParView, sParView.Count(), sParView.InlineRuns(), sParView.Overflow());
    if (port_par_verify_on()) {
        ParVerifyAfter(sParView, false);
    }
}
// GXV_PAR_VIEW=2: every outstanding job done and the worker out of the batch (GameSystem::frameLoop after draw(); the scene's drop).
extern "C" void port_par_view_retire(void) {
    if (!gPortParViewAsync) {
        return;
    }
    const int64_t t0 = port_host_time_ns();
    sParViewA.Retire();
    port_par_view_async_quiesce();
    gPortParViewAsync = false;
    gPortParViewOpen = false;
    const port::par::AsyncBatch< ParViewJob >::Stats& c = sParViewA.Counters();
    const unsigned counters[8] = {c.jobs, c.inlineRuns, c.overflow, c.dispatched, c.byToucher, c.byRetire, c.waits, c.spins};
    port_par_view_async_report(counters, static_cast< unsigned >((port_host_time_ns() - t0) / 1000));
    if (port_par_verify_on()) {
        ParVerifyAfter(sParViewA, true);
    }
}
// J3DModel::calc and ::viewCalc (J3DModel.cpp), while a batch is open or outstanding: a pending job of this buffer runs first.
extern "C" void port_par_view_touch(void* buf) {
    if (gPortParViewOpen && port_on_frame_thread()) {
        if (gPortParViewAsync || sParAsyncRecording) {
            sParViewA.Touch(buf);
        } else {
            sParView.Touch(buf);
        }
    }
}
// GXV_PAR_VIEW=2: J3DSys::setModelDrawMtx / setModelNrmMtx and NormalMapBase::drawShape, about to hand a draw or normal half to GX (or
// to read it): its job is done when this returns (one acquire load once the worker has finished them all).
extern "C" void port_par_view_touch_out(const void* out) {
    if (gPortParViewAsync) {
        sParViewA.TouchOut(out);
    }
}

// GXV_PAR_ANIM (joint15, 5 Nov; default 0, platform/src/port/perf.cpp port_par_anim_on; worker platform/include/port/par_spec.h; walk
// decomp/include/Game/Animation/XanimeSpecWalk.hpp): the joints of the models that qualify are computed ahead of time, on a worker
// on another core, and the game thread takes them at the model's real ModelManager::calc when its inputs turn out to be the ones the
// worker used. Nothing waits and nothing is read early: the game thread runs calc-anim in its order as always, and at each model
// either copies the finished result in (a hit) or computes as before (a miss: a different key, a job the worker has not reached).
//
// 1. At the end of calc-anim (SceneFunction::executeCalcAnimList, port_par_anim_dispatch) every model that computed its joints
//    lately (a qualifying ModelManager::calc that GXV_ANIM_MEMO did not keep: they register themselves) gets a job: the key of its
//    NEXT computation as it can be predicted now, and that computation's inputs by value. The prediction is what
//    XanimePlayer::calcAnm + XanimeCore::updateFrame will set (they run unchanged): the frame ratio from the frame control's frame
//    (what updateAfterMovement keeps in _84 next turn; _84 itself while the calcAnm latch is held, as for a far model the drawn
//    turn's flush computes: port_anim_flush_stale), each track's frame from it, the later track winning where two tracks share a
//    resource; and the base matrix, scale, tracks and weights as they are (a model that stands still keeps them).
// 2. The worker computes the jobs in order into memory of their own (scratch: joints, scale flags, envelopes, the core's _28 / _50)
//    while the game thread draws the picture, waits for the display and runs the next turn's movement.
// 3. At the model's ModelManager::calc (after calcAnm, as GXV_ANIM_MEMO's key is made) the real key is compared with the job's,
//    word for word: the model, buffer, flags, base scale and matrix, core and its modes, each track's resource, weight and frame
//    (MemoKey), and for a Softimage-init model J3DSys::mParentS. Equal, and the worker done: the result is copied in, with what
//    J3DModel::calc leaves behind (its prologue, as GXV_ANIM_MEMO's MemoPrologue; the core's per-joint state; J3DSys's running
//    matrix and scales; J3DMtxCalc's buffer and joint; the current calculators): every later reader sees the bytes and the state
//    the original would have left, at the same point of the turn. Otherwise the original runs.
// Exact by construction: the joints are a function of the key (the worker's walk is the original's arithmetic over the same
// unchanging model data and BCK tables: see XanimeSpecWalk.hpp), and only a key that matches word for word is taken.
// Qualifying models (GXV_ANIM_MEMO's rules, plus): a plain J3DModel (not J3DModelX / J3DModel2), an XanimePlayer whose core has no
// transform list, no blend in progress (_1C >= 1, _20 == 1, no freeze copy), _6 0 or 3, _4 0-2; track 0 with a resource (one
// track) or a total weight that is not 0 (several); every track the walk reads a J3DAnmTransformKey (a BCK); no joint callbacks,
// deformers, CPU skinning or vertex colour calcs, no calc callback, not UseDefaultJ3D; a joint tree whose root is joint 0, with no
// sibling, reaching every joint once; not the player's categories (as GXV_ANIM_MEMO: Mario's animators share joint state).
// A model whose predictions miss four times in a row is left out for 64 batches; one not computed for 3 batches drops off the list.
// Main loop thread only. A batch ends before the next one is made (if the worker is still in one of its jobs at the end of calc-anim,
// it is told to stop and no batch is made that turn: the game thread never waits for it), and before a scene's memory goes (there
// it does wait for the job the worker is in: port_par_anim_drop).
// GXV_PAR_ANIM_VERIFY=1 (diagnostic): a hit also runs the original, and compares it with the worker's result (xspec::Compare).
#include "Game/Animation/XanimeSpecWalk.hpp"
#include "port/par_spec.h"  // (the job states; NowUs)
#include "Game/NPC/NPCActor.hpp"                // (GXV_PAR_ANIM_CB: NPCActor::calcJointScale)
#include "Game/Enemy/AnimScaleController.hpp"
#include "Game/Util/JointController.hpp"
#include <atomic>
extern "C" int port_par_anim_on(void);
extern "C" int port_par_anim_switch(void);
extern "C" int port_par_anim_verify_on(void);
extern "C" int port_par_anim_lean_on(void);
extern "C" unsigned port_par_anim_max(void);
extern "C" unsigned port_par_anim_kb(void);
extern "C" int port_par_anim_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n);
extern "C" int port_par_anim_try_quiesce(void);
extern "C" void port_par_anim_quiesce(void);
extern "C" int port_par_anim_claim(void* item);
extern "C" void port_par_anim_note(const unsigned* counts, unsigned n);
extern "C" int port_j3d_basic_calc_kind(const J3DMtxCalc* calc);
extern "C" int port_par_anim_blend_on(void);   // (GXV_PAR_ANIM_BLEND, par2: perf.cpp)
extern "C" int port_par_anim_cb_on(void);      // (GXV_PAR_ANIM_CB, par2: perf.cpp)
extern "C" int port_par_late_on(void);         // (GXV_PAR_LATE, par2: perf.cpp)
bool port_predict_default_base(const LiveActor* a, MtxPtr out);  // (LiveActor.cpp: LiveActor::calcAndSetBaseMtx's matrix, not set)
extern "C" int port_par_flush_on(void);        // (GXV_PAR_FLUSH, par2: perf.cpp)
extern "C" int port_par_flush_run(void (*fn)(void*, unsigned), void* ctx, void* items, unsigned n);
extern "C" void port_par_flush_quiesce(void);
extern "C" int port_par_flush_try_quiesce(void);
extern "C" void port_par_flush_note(const unsigned* counts, unsigned n, int late);
namespace {
enum { kParKeyMax = kMemoKeyMax + 4 };
struct ParAnimRec {
    ModelManager* mm;
    LiveActor* owner;
    u32 lastSeen;    // the batch number of its last qualifying computation
    u32 listed;      // on the list of models to speculate on
    u32 jobBatch;    // the batch its job belongs to
    s32 job;         // its job in that batch
    u16 misses;      // predictions in a row that missed
    u16 quiet;       // batches left out after too many misses
    u8 treeOk;       // 0 not checked, 1 the walk reaches every joint once from joint 0, 2 not
    u8 consulted;    // (GXV_PAR_ANIM_LEAN) its job of batch jobBatch was looked at by its model's computation
    u8 unusedRun;    // (GXV_PAR_ANIM_LEAN) jobs in a row that were never looked at
    u8 flushMiss;    // (GXV_PAR_FLUSH) its flush jobs in a row whose key missed
    u8 flushQuiet;   // (GXV_PAR_FLUSH) pictures left out after two misses in a row
    u32 flushGen;    // (GXV_PAR_FLUSH) the flush batch its flushJob belongs to
    s32 flushJob;
    u8 lateBase;     // (GXV_PAR_LATE) the base matrix prediction: 0 the model's now, 1 LiveActor::calcAndSetBaseMtx's arithmetic
    u8 lateMiss;     // (GXV_PAR_LATE) misses in a row
    u8 lateQuiet;    // (GXV_PAR_LATE) turns left out
    u8 cbKind;       // (GXV_PAR_ANIM_CB) the callback its last computation had (0 none)
    u16 cbJoint;
    const void* cbCtl;
};
struct ParAnimJob {
    ParAnimRec* rec;
    J3DMtxCalc* basic;
    xspec::Input in;
    xspec::Output out;
    u32 us;   // the worker's time on it
    u8 ok;    // the walk ran (set by the worker)
    u16 envNum;
    u32 keyWords;
    u32 item;  // (GXV_PAR_FLUSH) its state's index (the worker goes from the end of the flush list)
    u32 key[kParKeyMax];
};
// A scratch store (bump allocated, emptied when its batch is made again).
struct ParArena {
    u8* base;
    u32 size, used;
    bool full;
};
ParAnimRec* sParRecs = nullptr;
u32 sParRecCap = 0, sParRecUsed = 0, sParRecGen = 1;
ParAnimJob* sParJobs = nullptr;
std::atomic< uint8_t >* sParItems = nullptr;
ParArena sParArenaS = {nullptr, 0, 0, false};
u32 sParJobCap = 0, sParJobCount = 0;
u32 sParBatch = 1;  // (the open batch's number; 0 never)
bool sParInitTried = false;
PortHostVector< ParAnimRec* > sParList;  // (host heap: port/host_vector.h)
PORT_HEAP_GUARD(sParList);
// The counts of the last batch, for perf.cpp's 5 s line (the order port_par_anim_note expects).
enum { kPaSpec, kPaHit, kPaMiss, kPaNotReady, kPaBusy, kPaSavedUs, kPaHitUs, kPaDispatchUs, kPaVerified, kPaVerifyBad, kPaSkipped, kPaCount };
unsigned sParCounts[kPaCount];
u32 sParVerifyLogged = 0;

bool ParInit() {
    if (sParJobs != nullptr) {
        return true;
    }
    if (sParInitTried) {
        return false;
    }
    sParInitTried = true;
    sParJobCap = port_par_anim_max();
    sParRecCap = sParJobCap * 4;
    sParArenaS.size = port_par_anim_kb() * 1024u;
    sParJobs = static_cast< ParAnimJob* >(malloc(sizeof(ParAnimJob) * sParJobCap));
    sParItems = static_cast< std::atomic< uint8_t >* >(malloc(sizeof(std::atomic< uint8_t >) * sParJobCap));
    sParRecs = static_cast< ParAnimRec* >(malloc(sizeof(ParAnimRec) * sParRecCap));
    sParArenaS.base = static_cast< u8* >(malloc(sParArenaS.size));
    if (sParJobs == nullptr || sParItems == nullptr || sParRecs == nullptr || sParArenaS.base == nullptr) {
        free(sParJobs);
        free(sParItems);
        free(sParRecs);
        free(sParArenaS.base);
        sParJobs = nullptr;
        port_log_summary("par-anim: no memory for GXV_PAR_ANIM (%u jobs, %u KiB): off", sParJobCap, port_par_anim_kb());
        return false;
    }
    for (u32 i = 0; i < sParJobCap; ++i) {
        new (&sParItems[i]) std::atomic< uint8_t >(port::par::kJobTaken);
    }
    sParList.reserve(sParRecCap);
    return true;
}
void* ParArenaAlloc(ParArena& a, u32 bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (a.used + bytes > a.size) {
        a.full = true;
        return nullptr;
    }
    void* p = a.base + a.used;
    a.used += bytes;
    return p;
}
// The model's record (made at its first qualifying computation in this scene), or null.
ParAnimRec* ParRecord(ModelManager* mm, bool make) {
    if (mm->mParAnimGen == sParRecGen && mm->mParAnim != nullptr) {
        return static_cast< ParAnimRec* >(mm->mParAnim);
    }
    if (!make || sParRecUsed >= sParRecCap) {
        return nullptr;
    }
    ParAnimRec* r = &sParRecs[sParRecUsed++];
    memset(r, 0, sizeof(*r));
    r->mm = mm;
    r->job = -1;
    r->flushJob = -1;
    mm->mParAnim = r;
    mm->mParAnimGen = sParRecGen;
    return r;
}
// The walk reaches every joint exactly once from joint 0, which has no sibling (checked once per model).
bool ParTreeOk(J3DJointTree& tree) {
    const u16 n = tree.getJointNum();
    J3DJoint* root = tree.getRootNode();
    if (n == 0 || n > 1024 || root == nullptr || root != tree.getJointNodePointer(0) || root->getYounger() != nullptr) {
        return false;
    }
    u8 seen[1024 / 8];
    memset(seen, 0, sizeof(seen));
    J3DJoint* stack[1024];
    u32 depth = 0, visited = 0;
    stack[depth++] = root;
    while (depth != 0) {
        J3DJoint* j = stack[--depth];
        for (; j != nullptr; j = j->getYounger()) {
            const u16 no = j->getJntNo();
            if (no >= n || (seen[no >> 3] & (1u << (no & 7))) != 0 || ++visited > n || tree.getJointNodePointer(no) != j) {
                return false;
            }
            seen[no >> 3] |= static_cast< u8 >(1u << (no & 7));
            if (j->getChild() != nullptr) {
                if (depth >= 1024) {
                    return false;
                }
                stack[depth++] = j->getChild();
            }
        }
    }
    return visited == n;
}
// The vtable of J3DAnmTransformKey (a BCK): its getTransform is calcTransform(mFrame, ...), which the walk calls with the frame.
const void* AnmKeyVtable() {
    alignas(J3DAnmTransformKey) static unsigned char sProbe[sizeof(J3DAnmTransformKey)];
    static const void* const vt = *reinterpret_cast< const void* const* >(new (sProbe) J3DAnmTransformKey());
    return vt;
}
// (GXV_PAR_ANIM_CB) the vtable of JointControlDelegator< NPCActor >, as AnmKeyVtable.
const void* NpcDelegatorVtable() {
    alignas(JointControlDelegator< NPCActor >) static unsigned char sProbe[sizeof(JointControlDelegator< NPCActor >)];
    static const void* const vt = *reinterpret_cast< const void* const* >(new (sProbe) JointControlDelegator< NPCActor >(static_cast< NPCActor* >(nullptr),
                                                       static_cast< JointControlDelegator< NPCActor >::func >(nullptr),
                                                       static_cast< JointControlDelegator< NPCActor >::func >(nullptr)));
    return vt;
}
}  // namespace
namespace {
// (GXV_PAR_ANIM_CB) ctl, the user data of a joint whose callback is (or will be) JointController::staticCallBack, is NPCActor's scale
// delegator for this model: the callback the walk performs (kind 1), with its input now. joint: null for a prediction (the joint is
// the record's); else the joint must carry staticCallBack and ctl.
bool ParRecogniseCb(J3DModel* model, J3DJoint* joint, const void* ctl, ParCb* out) {
    if (!port_par_anim_cb_on() || ctl == nullptr || (joint != nullptr && (joint->getCallBack() != &JointController::staticCallBack || joint->mCallBackUserData != ctl)) ||
        *reinterpret_cast< const void* const* >(ctl) != NpcDelegatorVtable()) {
        return false;
    }
    const JointControlDelegator< NPCActor >* d = static_cast< const JointControlDelegator< NPCActor >* >(static_cast< const JointController* >(ctl));
    if (d->mMtxCalcFunc != &NPCActor::calcJointScale || d->mMtxCalcAfterChildFunc != nullptr || d->mModel != model || d->mHost == nullptr ||
        d->mHost->mScaleController == nullptr || (joint != nullptr && d->mJoint != joint)) {
        return false;
    }
    out->kind = 1;
    out->ctl = ctl;
    out->scale = d->mHost->mScaleController->_C;
    return true;
}
// GXV_PAR_ANIM's own conditions beyond MemoKey's (which the caller has checked): the job's Input, or false.
// (GXV_PAR_ANIM_BLEND) blend: pred holds the core's _1C, _20, _29 at the computation (MemoKey has let a blend state through only then);
// *needPrev: the walk reads the core's joint state (a blend state, track 0 without a resource, a total weight of 0): the caller gives
// the job Input::prev. scale: the base scale the computation will have (null: the model's now).
bool ParQualify(ParAnimRec* rec, J3DModel* model, XanimeCore* core, const f32* frames, xspec::Input& in, J3DMtxCalc** basicOut,
                const KeyBlend* pred = nullptr, bool* needPrev = nullptr, const Vec* scale = nullptr) {
    bool prev = pred != nullptr && (pred->b29 != 0 || pred->b1C < 1.0f || pred->b20 != 1.0f);
    J3DModelData* data = model->getModelData();
    J3DMtxBuffer* buf = model->getMtxBuffer();
    if (*reinterpret_cast< const void* const* >(model) != PlainJ3DModelVtable() || core == nullptr || core->mTrackCount == 0 ||
        core->_4 > 2 || model->checkFlag(J3DMdlFlag_UseDefaultJ3D) || core->mJointList == nullptr || core->mJointCount < data->getJointNum()) {
        return false;
    }
    if (rec->treeOk == 0) {
        rec->treeOk = ParTreeOk(data->getJointTree()) ? 1 : 2;
    }
    if (rec->treeOk != 1) {
        return false;
    }
    J3DMtxCalc* basic = data->getJointTree().getBasicMtxCalc();
    const int kind = port_j3d_basic_calc_kind(basic);
    if (kind < 0) {
        return false;
    }
    // (calcSingle reads the core's own state when track 0 has no resource, calcBlend when the total weight is 0)
    const void* keyVt = AnmKeyVtable();
    if (core->mTrackCount == 1) {
        if (core->mTrackList[0]._0 == nullptr && pred != nullptr) {
            prev = true;  // (calcSingle poses the last pose: GXV_PAR_ANIM_BLEND's walk)
        } else if (core->mTrackList[0]._0 == nullptr || *reinterpret_cast< const void* const* >(core->mTrackList[0]._0) != keyVt) {
            return false;
        }
    } else {
        f32 totalWeight = 0.0f;
        for (s32 i = 0; i < core->mTrackCount; i++) {
            if (core->mTrackList[i]._0 != nullptr) {
                totalWeight += core->mTrackList[i].mWeight;
            }
        }
        if (totalWeight == 0.0f) {
            if (pred == nullptr) {
                return false;
            }
            prev = true;  // (calcBlend poses the last pose)
        }
        for (s32 i = 0; i < core->mTrackCount; i++) {
            const XanimeTrack& t = core->mTrackList[i];
            if (t._0 != nullptr && t.mWeight != 0.0f && *reinterpret_cast< const void* const* >(t._0) != keyVt) {
                return false;
            }
        }
    }
    const bool doEnv = data->getWEvlpMtxNum() != 0 && !model->checkFlag(J3DMdlFlag_EnableLOD) && !data->checkFlag(0x100);
    if (doEnv && (buf->mpWeightEvlpMtx == nullptr || buf->mpEvlpScaleFlagArr == nullptr)) {
        return false;
    }
    memset(&in, 0, sizeof(in));
    in.dataTree = &data->getJointTree();
    in.bufTree = buf->mJointTree;
    in.root = data->getJointTree().getRootNode();
    for (u32 t = 0; t < core->mTrackCount; t++) {
        const XanimeTrack& track = core->mTrackList[t];
        in.track[t].res = static_cast< const J3DAnmTransformKey* >(track._0);
        in.track[t].weight = track.mWeight;
        in.track[t].frame = track._0 != nullptr ? frames[t] : 0.0f;
    }
    in.trackCount = core->mTrackCount;
    in.type4 = core->_4;
    in.mode6 = core->_6;
    in.initKind = static_cast< u8 >(kind);
    in.doEnv = doEnv ? 1 : 0;
    in.fixJoint = core->_C;
    in.jointNum = data->getJointNum();
    in.blend1C = pred != nullptr ? pred->b1C : core->_1C;
    in.blend20 = pred != nullptr ? pred->b20 : core->_20;
    in.freeze29 = pred != nullptr ? pred->b29 : 0;
    in.scale = scale != nullptr ? *scale : model->mBaseScale;
    PSMTXCopy(model->mBaseTransformMtx, in.base);
    in.parentS = J3DSys::mParentS;
    in.prev = nullptr;  // (set by ParBuild when *needPrev)
    in.cbKind = 0;      // (set by ParBuild: GXV_PAR_ANIM_CB)
    if (needPrev != nullptr) {
        *needPrev = prev;
    }
    *basicOut = basic;
    return true;
}
// (GXV_PAR_ANIM_BLEND) the core's _1C, _20 and _29 at the model's next computation, as far as they can be told now:
//   - _29: XanimeCore::updateFrame (in calcAnm) sets it from the freeze flag _28, which only a change of animation sets;
//   - a model whose next computation is the drawn turn's flush (GXV_ANIM_LOD / FREE deferred it, or GXV_PAR_FLUSH): no update()
//     runs before it, so _1C and _20 are those of now;
//   - else XanimePlayer::updateAfterMovement sets them: updateInterpoleRatio on _08 (the same arithmetic on a copy), then _1C = _08,
//     _20 = _0C (when _7C; otherwise it returns first and they stay).
// A wrong guess (a change of animation in the movement, for one) only makes the key miss.
void ParPredictBlend(XanimePlayer* player, LiveActor* owner, bool atFlush, KeyBlend& pb) {
    XanimeCore* core = player->mCore;
    pb.b29 = core->_28 != 0 ? 1 : 0;
    pb.b1C = core->_1C;
    pb.b20 = core->_20;
    if (atFlush || (owner != nullptr && owner->mStaleAnim) || !player->_7C || player->_20 == nullptr) {
        return;
    }
    f32 r = player->_08;
    XanimeFrameCtrl* fc = player->_20;
    if (fc->getRate() != 0.0f || fc->getAttribute() == 1) {
        if (fc->_14 != 0) {
            r += (1.0f - r) / fc->_14;
        } else {
            r = 1.0f;
        }
    } else {
        r = 1.0f;
    }
    pb.b1C = r;
    pb.b20 = player->_0C;
}
// The frames XanimePlayer::calcAnm + XanimeCore::updateFrame will set at the model's next computation, as far as they can be told
// now (the same arithmetic, without writing anything); the later track of a shared resource wins, as in updateFrame.
void ParPredictFrames(XanimePlayer* player, LiveActor* owner, f32* frames) {
    XanimeCore* core = player->mCore;
    f32 ratio = core->mFrameRatio;
    if (!player->_80) {
        f32 currentFrame;
        const bool latched = owner != nullptr && owner->mStaleAnim ? owner->mStaleXLatch : player->_88;
        if (player->_20->checkState(1) != 0) {
            currentFrame = player->_20->getEnd();
        } else if (latched) {
            currentFrame = player->_84;
        } else {
            currentFrame = player->_20->getFrame();  // (what updateAfterMovement keeps in _84 before it advances the frame)
        }
        if (player->mCurrentAnimation != nullptr && player->mCurrentAnimation->_20[0] != nullptr) {
            s16 maxFrame = static_cast< J3DAnmTransform* >(player->mCurrentAnimation->_20[0])->getFrameMax();
            if (static_cast< J3DAnmTransform* >(player->mCurrentAnimation->_20[0])->getFrameMax() == 0) {
                maxFrame = 1;
            }
            ratio = currentFrame / maxFrame;
        }
    }
    f32 set[xspec::kMaxTracks];
    for (u32 i = 0; i < core->mTrackCount; i++) {
        set[i] = 0.0f;
        if (core->mTrackList[i]._0 != nullptr) {
            if (core->mTrackList[i]._C != 0) {
                f32 frame = core->mTrackList[i]._8;
                set[i] = frame * core->mTrackList[i]._0->getFrameMax();
            } else {
                set[i] = ratio * core->mTrackList[i]._0->getFrameMax();
            }
        }
    }
    for (u32 i = 0; i < core->mTrackCount; i++) {
        frames[i] = set[i];
        for (u32 j = i + 1; j < core->mTrackCount; j++) {
            if (core->mTrackList[j]._0 == core->mTrackList[i]._0) {
                frames[i] = set[j];
            }
        }
    }
}
// The key's last words beyond MemoKey: J3DSys::mParentS for a Softimage init (its walk reads it; the others set it).
u32 ParKeyTail(int kind, u32* key, u32 w) {
    if (kind == xspec::kInitSoftimage) {
        KeyF(key, w, J3DSys::mParentS.x);
        KeyF(key, w, J3DSys::mParentS.y);
        KeyF(key, w, J3DSys::mParentS.z);
    }
    return w;
}
// One job, from a listed model's state; false: it does not qualify now (or no room).
// (GXV_PAR_FLUSH) flushActor: the job is for the drawn turn's flush of that deferred actor (port_par_flush_prepare): no update() runs
// before it, and LiveActor::calcAnmMtx gives the model the actor's scale first. 2 is returned (as false) when the GXV_ANIM_MEMO record
// of the model says the memo will keep it (no job is worth making).
// (GXV_PAR_LATE) late: the job is for this turn's calc-anim (port_par_late_prepare): update() is over; flushActor is then the actor,
// and base the predicted base matrix (null: the model's now). 3 is returned when par-anim's own job for the model has this very key.
int ParBuild(ParAnimRec* rec, ParAnimJob& job, ParArena& arena, LiveActor* flushActor = nullptr, bool late = false, const f32 (*base)[4] = nullptr) {
    ModelManager* mm = rec->mm;
    J3DModel* model = mm->getJ3DModel();
    XanimePlayer* player = mm->mXanimePlayer;
    if (model == nullptr || player == nullptr || player->mCore == nullptr || player->_20 == nullptr ||
        player->mCore->mTrackCount > xspec::kMaxTracks) {
        return 0;
    }
    f32 frames[xspec::kMaxTracks];
    ParPredictFrames(player, flushActor != nullptr ? flushActor : rec->owner, frames);
    const bool blend = port_par_anim_blend_on() != 0;
    KeyBlend pb;
    if (blend) {
        ParPredictBlend(player, rec->owner, flushActor != nullptr, pb);
    }
    const Vec* scale = flushActor != nullptr ? static_cast< const Vec* >(&flushActor->mScale) : nullptr;
    // (GXV_PAR_ANIM_CB: the callback the record's last computation had, if its controller is still the same kind for this model)
    ParCb cb;
    cb.kind = 0;
    const bool cbMode = port_par_anim_cb_on() != 0;
    if (cbMode && rec->cbKind != 0 && rec->cbJoint < model->getModelData()->getJointNum() &&
        ParRecogniseCb(model, nullptr, rec->cbCtl, &cb) &&
        static_cast< const JointController* >(rec->cbCtl)->mJoint == model->getModelData()->getJointNodePointer(rec->cbJoint) &&
        static_cast< const LiveActor* >(static_cast< const JointControlDelegator< NPCActor >* >(static_cast< const JointController* >(rec->cbCtl))->mHost) == rec->owner) {
        cb.joint = rec->cbJoint;
    } else {
        cb.kind = 0;
    }
    ParOverride ov = {scale, base, cbMode, &cb, nullptr};
    u32 words = MemoKey(mm, model, job.key, frames, blend, blend ? &pb : nullptr, &ov);
    if (words == 0) {
        return 0;
    }
    if (late && rec->jobBatch == sParBatch && rec->job >= 0 && static_cast< u32 >(rec->job) < sParJobCount && sParJobs[rec->job].rec == rec) {
        u32 tail[kParKeyMax];
        memcpy(tail, job.key, words * sizeof(u32));
        const u32 w2 = ParKeyTail(sParJobs[rec->job].in.initKind, tail, words);
        if (sParJobs[rec->job].keyWords == w2 && memcmp(sParJobs[rec->job].key, tail, w2 * sizeof(u32)) == 0) {
            return 3;  // (par-anim's job will hit: nothing to add)
        }
    }
    if (flushActor != nullptr && mm->mAnimMemoGen == sMemoGen && mm->mAnimMemo != nullptr) {
        // (the memo's key is the plain one: equal to this prediction, with its copy valid, the memo keeps the model at the flush)
        const AnimMemo* memo = static_cast< const AnimMemo* >(mm->mAnimMemo);
        if (memo->snapValid && memo->keyWords == words && memcmp(memo->key, job.key, words * sizeof(u32)) == 0) {
            return 2;
        }
    }
    bool needPrev = false;
    if (!ParQualify(rec, model, player->mCore, frames, job.in, &job.basic, blend ? &pb : nullptr, &needPrev, scale)) {
        return 0;
    }
    if (base != nullptr) {
        PSMTXCopy(base, job.in.base);
    }
    if (cb.kind != 0) {
        job.in.cbKind = cb.kind;
        job.in.cbJoint = cb.joint;
        job.in.cbScale = cb.scale;
    }
    words = ParKeyTail(job.in.initKind, job.key, words);
    const u16 n = job.in.jointNum;
    J3DMtxBuffer* buf = model->getMtxBuffer();
    const u16 e = job.in.doEnv ? buf->mJointTree->getWEvlpMtxNum() : 0;
    const u32 save = arena.used;
    job.out.anm = static_cast< Mtx* >(ParArenaAlloc(arena, n * sizeof(Mtx)));
    job.out.scaleFlag = static_cast< u8* >(ParArenaAlloc(arena, n));
    job.out.joint = needPrev ? nullptr : static_cast< xspec::JointState* >(ParArenaAlloc(arena, n * sizeof(xspec::JointState)));
    job.out.full = needPrev ? static_cast< xspec::JointPrev* >(ParArenaAlloc(arena, n * sizeof(xspec::JointPrev))) : nullptr;
    xspec::JointPrev* prev = needPrev ? static_cast< xspec::JointPrev* >(ParArenaAlloc(arena, n * sizeof(xspec::JointPrev))) : nullptr;
    job.out.env = e != 0 ? static_cast< Mtx* >(ParArenaAlloc(arena, e * sizeof(Mtx))) : nullptr;
    job.out.envScale = e != 0 ? static_cast< u8* >(ParArenaAlloc(arena, e)) : nullptr;
    if (job.out.anm == nullptr || job.out.scaleFlag == nullptr || (needPrev ? (job.out.full == nullptr || prev == nullptr) : job.out.joint == nullptr) ||
        (e != 0 && (job.out.env == nullptr || job.out.envScale == nullptr))) {
        arena.used = save;
        return 0;
    }
    if (needPrev) {
        xspec::TakePrev(player->mCore, n, prev);  // (compared again at the real computation: xspec::SamePrev)
        job.in.prev = prev;
    }
    job.rec = rec;
    job.keyWords = words;
    job.envNum = e;
    job.us = 0;
    job.ok = 0;
    return true;
}
void ParAnimItem(void* ctx, unsigned i) {
    ParAnimJob& job = static_cast< ParAnimJob* >(ctx)[i];
    const uint64_t t0 = port::par::NowUs();
    job.ok = xspec::Run(job.in, job.out) ? 1 : 0;
    job.us = static_cast< u32 >(port::par::NowUs() - t0);
}

// GXV_PAR_FLUSH (par2, 6 Nov; DESIGN.md stage 2; default 1 in the Vita player build, perf.cpp port_par_flush_on): the drawn turn's
// far-model flush (port_anim_flush_stale: the models GXV_ANIM_LOD / GXV_ANIM_LOD_FREE deferred since the last picture) computes their
// joints on core 0, one model after the other, from a state no update() changes any more. Before it (GameSystem::frameLoop, ahead of
// the deferred draw-done wait), port_par_flush_prepare makes a job of every deferred model that qualifies as par-anim's do and that has
// no par-anim job of its own (the key of its flush computation: the scale the actor gives it, the frames under its latch, the blend
// state of now; the base matrix as it is: an actor that moved since its last computation misses), and hands them to a worker of their
// own on the fourth core, which takes them from the END of the flush list while the game thread goes through it from the start (and
// first waits for the front end). At each model's computation the job is used only as par-anim's: the real key equal word for word
// (and the core's joint state for a blend job), the worker done: the bytes it wrote are the original's (same walk, same inputs);
// otherwise the game thread computes as before (a job the worker has not started is taken from it). port_par_flush_end after the flush:
// nothing reads a flush job any more. Exact by par-anim's argument; GXV_PAR_ANIM_VERIFY=1 checks flush hits too ("par-flush" line).
// Needs GXV_PAR_ANIM=1 (its records, qualification and walk). 0: the flush as before.
ParAnimJob* sFlJobs = nullptr;
std::atomic< uint8_t >* sFlItems = nullptr;
ParArena sFlArena = {nullptr, 0, 0, false};
u32 sFlCap = 0, sFlCount = 0, sFlGen = 1;
bool sFlOpen = false, sFlInitTried = false;
bool sFlLate = false;  // (GXV_PAR_LATE: the open batch is calc-anim's, not the flush's; the same arrays and worker)
enum { kFlBuilt, kFlHit, kFlMiss, kFlNotReady, kFlBusy, kFlSavedUs, kFlHitUs, kFlBuildUs, kFlCovered, kFlMemo, kFlUnqual, kFlQuiet, kFlVerified,
       kFlVerifyBad, kFlDeferred, kFlCount };
unsigned sFlCounts[kFlCount];
bool FlushInit() {
    if (sFlJobs != nullptr) {
        return true;
    }
    if (sFlInitTried) {
        return false;
    }
    sFlInitTried = true;
    sFlCap = port_par_anim_max();
    sFlArena.size = port_par_anim_kb() * 1024u;
    sFlJobs = static_cast< ParAnimJob* >(malloc(sizeof(ParAnimJob) * sFlCap));
    sFlItems = static_cast< std::atomic< uint8_t >* >(malloc(sizeof(std::atomic< uint8_t >) * sFlCap));
    sFlArena.base = static_cast< u8* >(malloc(sFlArena.size));
    if (sFlJobs == nullptr || sFlItems == nullptr || sFlArena.base == nullptr) {
        free(sFlJobs);
        free(sFlItems);
        free(sFlArena.base);
        sFlJobs = nullptr;
        port_log_summary("par-flush: no memory for GXV_PAR_FLUSH (%u jobs, %u KiB): off", sFlCap, port_par_anim_kb());
        return false;
    }
    for (u32 i = 0; i < sFlCap; ++i) {
        new (&sFlItems[i]) std::atomic< uint8_t >(port::par::kJobTaken);
    }
    return true;
}
void ParFlushItem(void* ctx, unsigned i) {
    // (item i is the (n-1-i)th job: the worker starts at the end of the flush list)
    ParAnimItem(ctx, sFlCount - 1u - i);
}

enum TakeResult { kTakeHit, kTakeMiss, kTakeNotReady, kTakeBusy };
// A job's result at the model's real computation (par-anim's or the flush's): kTakeHit: the joints are in, as J3DModel::calc would
// leave them (or, with GXV_PAR_ANIM_VERIFY, the original ran and was compared); otherwise the caller computes as before.
TakeResult ParTakeJob(ParAnimJob& job, std::atomic< uint8_t >& item, ModelManager* mm, J3DModel* model, const u32* key, u32 words, unsigned* verified,
                      unsigned* verifyBad, const char* what) {
    u32 real[kParKeyMax];
    memcpy(real, key, words * sizeof(u32));
    const u32 realWords = ParKeyTail(job.in.initKind, real, words);
    J3DMtxBuffer* buf = model->getMtxBuffer();
    XanimeCore* core = mm->mXanimePlayer != nullptr ? mm->mXanimePlayer->mCore : nullptr;
    if (realWords != job.keyWords || memcmp(real, job.key, realWords * sizeof(u32)) != 0 || buf->mJointTree != job.in.bufTree || core == nullptr ||
        model->getModelData()->getJointTree().getBasicMtxCalc() != job.basic ||
        (job.in.prev != nullptr && !xspec::SamePrev(core, job.in.jointNum, job.in.prev))) {
        uint8_t v = port::par::kJobPending;
        item.compare_exchange_strong(v, port::par::kJobTaken, std::memory_order_acq_rel);  // (the worker need not compute it any more)
        return kTakeMiss;
    }
    const int claim = port_par_anim_claim(&item);
    if (claim != 0 || !job.ok) {
        return claim == 2 ? kTakeBusy : kTakeNotReady;
    }
    if (port_par_anim_verify_on()) {
        // The original, then the worker's result against what it wrote (and the state it left).
        model->calc();
        const u32 bad = xspec::Compare(job.out, job.in.jointNum, job.envNum, job.in.doEnv != 0, buf, core, job.basic);
        ++*verified;
        if (bad != 0) {
            ++*verifyBad;
            if (sParVerifyLogged < 16) {
                ++sParVerifyLogged;
                port_log_summary("%s: verify MISMATCH (what 0x%x: 1 joints, 2 scale flags, 4 envelopes, 8 envelope flags, 16 core state, "
                                 "32 statics): %u joints, %u envelopes, core type %u mode %u, %u tracks, init %u, blend %s (1C %g, 20 %g, 29 %u)",
                                 what, bad, static_cast< unsigned >(job.in.jointNum), static_cast< unsigned >(job.envNum),
                                 static_cast< unsigned >(job.in.type4), static_cast< unsigned >(job.in.mode6),
                                 static_cast< unsigned >(job.in.trackCount), static_cast< unsigned >(job.in.initKind),
                                 job.in.prev != nullptr ? "yes" : "no", static_cast< double >(job.in.blend1C), static_cast< double >(job.in.blend20),
                                 static_cast< unsigned >(job.in.freeze29));
            }
        }
        return kTakeHit;
    }
    // What J3DModel::calc does for a qualifying model, with the worker's joints.
    if (gPortParViewOpen) {
        port_par_view_touch(buf);
    }
    MemoPrologue(model);
    xspec::Apply(job.out, job.in.jointNum, job.envNum, job.in.doEnv != 0, buf, core, job.basic);
    if (job.in.cbKind != 0) {
        // (GXV_PAR_ANIM_CB: what JointController::staticCallBack does at timing 1, after the walk: the callback unregisters itself)
        J3DJoint* cj = job.in.dataTree->getJointNodePointer(job.in.cbJoint);
        cj->setCallBack(nullptr);
        cj->mCallBackUserData = nullptr;
    }
    return kTakeHit;
}
}  // namespace

static bool PortParAnimTake(ModelManager* mm, J3DModel* model, const u32* key, u32 words, const ParCb* cb) {
    if (!port_par_anim_on() || !port_on_frame_thread() || words == 0 || !ParInit()) {
        return false;
    }
    // (listed for the next batch: only models of live actors, as GXV_DL_LAZY lists them)
    LiveActor* owner = gPortCalcAnimOwner != nullptr && gPortCalcAnimOwner->mModelManager == mm ? gPortCalcAnimOwner : nullptr;
    ParAnimRec* rec = ParRecord(mm, owner != nullptr);
    if (rec == nullptr) {
        return false;
    }
    if (owner != nullptr) {
        rec->owner = owner;
        rec->lastSeen = sParBatch;
        if (!rec->listed) {
            rec->listed = 1;
            sParList.push_back(rec);
        }
    }
    // (GXV_PAR_ANIM_CB: the callback this computation has, for the next predictions)
    rec->cbKind = cb != nullptr ? cb->kind : 0;
    if (rec->cbKind != 0) {
        rec->cbJoint = cb->joint;
        rec->cbCtl = cb->ctl;
    }
    // GXV_PAR_FLUSH / GXV_PAR_LATE: this flush's or this calc-anim's job of the model (port_par_flush_prepare / port_par_late_prepare).
    if (sFlOpen && rec->flushGen == sFlGen && rec->flushJob >= 0 && static_cast< u32 >(rec->flushJob) < sFlCount && sFlJobs[rec->flushJob].rec == rec) {
        ParAnimJob& job = sFlJobs[rec->flushJob];
        rec->flushJob = -1;  // (consulted once)
        const uint64_t t0 = port::par::NowUs();
        const TakeResult r = ParTakeJob(job, sFlItems[job.item], mm, model, key, words, &sFlCounts[kFlVerified], &sFlCounts[kFlVerifyBad], "par-flush");
        if (r == kTakeHit) {
            rec->flushMiss = 0;
            rec->lateMiss = 0;
            ++sFlCounts[kFlHit];
            sFlCounts[kFlHitUs] += static_cast< unsigned >(port::par::NowUs() - t0);
            sFlCounts[kFlSavedUs] += job.us;
            return true;
        }
        if (r == kTakeMiss) {
            ++sFlCounts[kFlMiss];
            if (sFlLate) {
                // (GXV_PAR_LATE: the other base matrix prediction next time; after both missed, left out for a while)
                if (++rec->lateMiss >= 2) {
                    rec->lateMiss = 0;
                    rec->lateQuiet = 16;
                }
                rec->lateBase ^= 1;
            } else if (++rec->flushMiss >= 2) {
                rec->flushMiss = 0;
                rec->flushQuiet = 8;  // (an actor that moves between pictures: its base matrix is never the one of the last flush)
            }
        } else {
            ++sFlCounts[r == kTakeBusy ? kFlBusy : kFlNotReady];
        }
        // (else par-anim's own job, if any, below)
    }
    if (rec->jobBatch != sParBatch || rec->job < 0 || static_cast< u32 >(rec->job) >= sParJobCount) {
        return false;
    }
    ParAnimJob& job = sParJobs[rec->job];
    std::atomic< uint8_t >& item = sParItems[rec->job];
    if (job.rec != rec) {
        return false;
    }
    rec->consulted = 1;  // (GXV_PAR_ANIM_LEAN: hit or miss, the job was looked at)
    const uint64_t t0 = port::par::NowUs();
    const TakeResult r = ParTakeJob(job, item, mm, model, key, words, &sParCounts[kPaVerified], &sParCounts[kPaVerifyBad], "par-anim");
    if (r == kTakeMiss) {
        ++sParCounts[kPaMiss];
        if (++rec->misses >= 4) {
            rec->misses = 0;
            rec->quiet = 64;
        }
        return false;
    }
    if (r != kTakeHit) {
        ++sParCounts[r == kTakeBusy ? kPaBusy : kPaNotReady];
        return false;
    }
    rec->misses = 0;
    ++sParCounts[kPaHit];
    if (!port_par_anim_verify_on()) {
        sParCounts[kPaHitUs] += static_cast< unsigned >(port::par::NowUs() - t0);
        sParCounts[kPaSavedUs] += job.us;  // (what the original would have cost here: the worker's time on the same walk)
    }
    return true;
}
// ModelManager::calc for a model GXV_ANIM_MEMO did not take (off, or its store full): GXV_PAR_ANIM on its own.
static bool PortParAnimCalc(ModelManager* mm) {
    if (!port_par_anim_on() || !port_on_frame_thread()) {
        return false;
    }
    J3DModel* model = mm->getJ3DModel();
    u32 key[kMemoKeyMax];
    // (GXV_PAR_ANIM_BLEND: a core in a blend state qualifies here; GXV_ANIM_MEMO, which sent it here, never keeps one)
    // (GXV_PAR_ANIM_CB: so does a model whose joint callback is one the walk performs; the memo never keeps one either)
    ParCb cb;
    cb.kind = 0;
    ParOverride ov = {nullptr, nullptr, port_par_anim_cb_on() != 0, nullptr, &cb};
    const u32 words = MemoKey(mm, model, key, nullptr, port_par_anim_blend_on() != 0, nullptr, &ov);
    return words != 0 && PortParAnimTake(mm, model, key, words, &cb);
}
// The end of calc-anim (SceneFunction::executeCalcAnimList): the last batch ends, the next one is made from the listed models and
// handed to the worker.
extern "C" void port_par_anim_dispatch(void) {
    if (!port_par_anim_switch() || !port_on_frame_thread() || !ParInit()) {
        return;
    }
    if (!port_par_anim_try_quiesce()) {
        ++sParCounts[kPaSkipped];  // (the worker is still in a job of the last batch, which stays open: no wait, no new batch this turn)
        return;
    }
    // (the worker is out of the last batch: its jobs and scratch can be made again)
    port_par_anim_note(sParCounts, kPaCount);  // (the last batch's counts, for the 5 s line)
    memset(sParCounts, 0, sizeof(sParCounts));
    const uint64_t t0 = port::par::NowUs();
    const u32 lastBatch = sParBatch;  // (GXV_PAR_ANIM_LEAN: the batch whose jobs were looked at or not)
    const bool lean = port_par_anim_lean_on() != 0;
    ++sParBatch;
    if (sParBatch == 0) {
        sParBatch = 1;
    }
    sParArenaS.used = 0;
    sParJobCount = 0;
    port_live_actor_snapshot();
    size_t keep = 0;
    for (size_t i = 0; i < sParList.size(); ++i) {
        ParAnimRec* rec = sParList[i];
        LiveActor* a = rec->owner;
        if (a == nullptr || !port_live_actor_valid(a) || a->mModelManager != rec->mm || rec->mm->mParAnim != rec || rec->mm->mParAnimGen != sParRecGen ||
            sParBatch - rec->lastSeen > 3) {
            rec->listed = 0;  // (no longer an actor of this scene, or not computed lately: off the list until it computes again)
            continue;
        }
        sParList[keep++] = rec;
        if (lean && rec->jobBatch == lastBatch && rec->job >= 0) {
            // GXV_PAR_ANIM_LEAN: a job its model never looked at (kept by GXV_ANIM_MEMO, deferred by GXV_ANIM_LOD, clipped) cost the
            // game thread its making; two in a row, and the model is left out for a while.
            if (rec->consulted) {
                rec->unusedRun = 0;
            } else if (++rec->unusedRun >= 2) {
                rec->unusedRun = 0;
                rec->quiet = 8;
            }
            rec->job = -1;
        }
        if (rec->quiet != 0) {
            --rec->quiet;
            continue;
        }
        if (a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsNoCalcAnim || sParJobCount >= sParJobCap) {
            continue;
        }
        ParAnimJob& job = sParJobs[sParJobCount];
        if (ParBuild(rec, job, sParArenaS) != 1) {
            if (lean) {
                rec->quiet = 16;  // (GXV_PAR_ANIM_LEAN: it does not qualify now; a blend or a change of animation lasts a while)
            }
            continue;
        }
        rec->job = static_cast< s32 >(sParJobCount);
        rec->jobBatch = sParBatch;
        rec->consulted = 0;
        sParItems[sParJobCount].store(port::par::kJobPending, std::memory_order_relaxed);
        ++sParJobCount;
    }
    sParList.resize(keep);
    sParCounts[kPaSpec] = sParJobCount;
    sParCounts[kPaDispatchUs] = static_cast< unsigned >(port::par::NowUs() - t0);
    port_par_anim_run(&ParAnimItem, sParJobs, sParItems, sParJobCount);
}
// GXV_PAR_FLUSH (above): the flush batch, made at the start of a drawn turn before the deferred draw-done wait (GameSystem::frameLoop),
// from the actors port_anim_flush_stale is about to compute, in its order; the worker takes them from the end.
LiveActor* const* port_anim_stale_items(size_t* n);  // (LiveActor.cpp: the deferred actors, in the flush's order)
extern "C" void port_par_flush_prepare(void) {
    sFlOpen = false;
    if (!port_par_flush_on() || !port_par_anim_switch() || !port_on_frame_thread() || !ParInit() || !FlushInit()) {
        return;
    }
    size_t n = 0;
    LiveActor* const* list = port_anim_stale_items(&n);
    if (n == 0) {
        return;
    }
    const uint64_t t0 = port::par::NowUs();
    port_par_flush_quiesce();  // (the last flush batch's worker is out of it: its jobs and scratch can be made again)
    ++sFlGen;
    if (sFlGen == 0) {
        sFlGen = 1;
    }
    sFlArena.used = 0;
    sFlCount = 0;
    port_live_actor_snapshot();
    for (size_t i = 0; i < n && sFlCount < sFlCap; ++i) {
        LiveActor* a = list[i];
        // (port_anim_flush_stale's own tests: the actors it computes)
        if (!port_live_actor_valid(a) || !a->mStaleAnim || a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel ||
            a->mFlag.mIsNoCalcAnim || a->mModelManager == nullptr || a->mModelManager->mXanimePlayer == nullptr) {
            continue;
        }
        ++sFlCounts[kFlDeferred];
        ModelManager* mm = a->mModelManager;
        ParAnimRec* rec = ParRecord(mm, true);
        if (rec == nullptr) {
            break;
        }
        rec->owner = a;
        if (rec->flushQuiet != 0) {
            --rec->flushQuiet;
            ++sFlCounts[kFlQuiet];
            continue;
        }
        if (rec->jobBatch == sParBatch && rec->job >= 0 && static_cast< u32 >(rec->job) < sParJobCount && sParJobs[rec->job].rec == rec) {
            ++sFlCounts[kFlCovered];  // (par-anim's batch has a job for it already)
            continue;
        }
        ParAnimJob& job = sFlJobs[sFlCount];
        const int b = ParBuild(rec, job, sFlArena, a);
        if (b != 1) {
            ++sFlCounts[b == 2 ? kFlMemo : kFlUnqual];
            continue;
        }
        rec->flushGen = sFlGen;
        rec->flushJob = static_cast< s32 >(sFlCount);
        ++sFlCount;
    }
    for (u32 j = 0; j < sFlCount; ++j) {
        sFlJobs[j].item = sFlCount - 1u - j;
        sFlItems[j].store(port::par::kJobPending, std::memory_order_relaxed);
    }
    sFlCounts[kFlBuilt] += sFlCount;
    sFlCounts[kFlBuildUs] += static_cast< unsigned >(port::par::NowUs() - t0);
    if (sFlCount != 0) {
        sFlOpen = true;
        if (!port_par_flush_run(&ParFlushItem, sFlJobs, sFlItems, sFlCount)) {
            // (no worker: the jobs stay pending and the game thread takes each one: the flush as before, plus the jobs' making)
        }
    }
}
// After port_anim_flush_stale: no flush job is read any more; the batch ends (the worker stops after the job it may be in; the next
// prepare, or the scene's drop, waits for it).
extern "C" void port_par_flush_end(void) {
    if (sFlJobs == nullptr) {
        return;
    }
    if (sFlOpen) {
        sFlOpen = false;
        (void)port_par_flush_try_quiesce();
    }
    port_par_flush_note(sFlCounts, kFlCount, sFlLate ? 1 : 0);
    memset(sFlCounts, 0, sizeof(sFlCounts));
    sFlLate = false;
}
// GXV_PAR_LATE (par2, 6 Nov; default 1 in the Vita player build; needs GXV_PAR_ANIM=1): the start of calc-anim
// (SceneFunction::executeCalcAnimList). update() is over: the frames (XanimePlayer's latch), the blend state, the scale the actor will
// give its model and the callback inputs (GXV_PAR_ANIM_CB) are final. Every model par-anim lists (its qualifying models of late) gets a
// job of this turn's computation, unless par-anim's own job already has that very key, the memo will keep it, or it is deferred
// (GXV_ANIM_LOD). The base matrix is predicted: the model's now (an actor that stands still), or after a miss LiveActor::
// calcAndSetBaseMtx's own arithmetic on the actor's position and rotation (an actor that moves and does not override it); after
// two misses in a row the model is left out for 16 turns. The jobs go to GXV_PAR_FLUSH's worker (the fourth core), which takes them
// from the end of the list while the game thread runs calc-anim from the start, unchanged: each model takes its job at its own
// ModelManager::calc on par-anim's word-for-word key rule, else computes as before. A synchronous fork-join in effect, with no
// reader ever seeing a joint early or late. port_par_late_end before par-anim's dispatch: no late job is read after calc-anim.
extern "C" void port_par_late_prepare(void) {
    sFlOpen = false;
    if (!port_par_late_on() || !port_par_anim_switch() || !port_on_frame_thread() || !ParInit() || !FlushInit() || sParList.empty()) {
        return;
    }
    const uint64_t t0 = port::par::NowUs();
    port_par_flush_quiesce();
    ++sFlGen;
    if (sFlGen == 0) {
        sFlGen = 1;
    }
    sFlArena.used = 0;
    sFlCount = 0;
    sFlLate = true;
    port_live_actor_snapshot();
    for (size_t i = 0; i < sParList.size() && sFlCount < sFlCap; ++i) {
        ParAnimRec* rec = sParList[i];
        LiveActor* a = rec->owner;
        if (a == nullptr || !port_live_actor_valid(a) || a->mModelManager != rec->mm || rec->mm->mParAnim != rec || rec->mm->mParAnimGen != sParRecGen ||
            a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsNoCalcAnim || a->mStaleAnim || rec->mm->mXanimePlayer == nullptr) {
            continue;
        }
        ++sFlCounts[kFlDeferred];
        if (rec->lateQuiet != 0) {
            --rec->lateQuiet;
            ++sFlCounts[kFlQuiet];
            continue;
        }
        Mtx bm;
        const f32(*base)[4] = nullptr;
        if (rec->lateBase == 1 && port_predict_default_base(a, bm)) {
            base = bm;
        }
        ParAnimJob& job = sFlJobs[sFlCount];
        const int b = ParBuild(rec, job, sFlArena, a, true, base);
        if (b != 1) {
            ++sFlCounts[b == 3 ? kFlCovered : (b == 2 ? kFlMemo : kFlUnqual)];
            continue;
        }
        rec->flushGen = sFlGen;
        rec->flushJob = static_cast< s32 >(sFlCount);
        ++sFlCount;
    }
    for (u32 j = 0; j < sFlCount; ++j) {
        sFlJobs[j].item = sFlCount - 1u - j;
        sFlItems[j].store(port::par::kJobPending, std::memory_order_relaxed);
    }
    sFlCounts[kFlBuilt] += sFlCount;
    sFlCounts[kFlBuildUs] += static_cast< unsigned >(port::par::NowUs() - t0);
    if (sFlCount != 0) {
        sFlOpen = true;
        (void)port_par_flush_run(&ParFlushItem, sFlJobs, sFlItems, sFlCount);
    }
}
extern "C" void port_par_late_end(void) {
    if (sFlLate) {
        port_par_flush_end();
    }
}
// The scene's models go (GameSystemSceneController::exeDestroyScene, through port_anim_memo_drop): the worker leaves the batch, and
// the records and the list start again.
extern "C" void port_par_anim_drop(void) {
    if (sParJobs == nullptr) {
        return;
    }
    port_par_anim_quiesce();
    for (size_t i = 0; i < sParList.size(); ++i) {
        sParList[i]->listed = 0;
    }
    sParList.clear();
    sParJobCount = 0;
    ++sParBatch;
    if (sParBatch == 0) {
        sParBatch = 1;
    }
    sParRecUsed = 0;
    ++sParRecGen;
    if (sParArenaS.full) {
        port_log_summary("par-anim: the scratch store (GXV_PAR_ANIM_KB, %u KiB) was full in the last scene: some models were not speculated on",
                         sParArenaS.size / 1024);
    }
    sParArenaS.full = false;
    // (GXV_PAR_FLUSH: its worker out of its batch, its records gone with par-anim's)
    if (sFlJobs != nullptr) {
        port_par_flush_quiesce();
        sFlOpen = false;
        sFlCount = 0;
        ++sFlGen;
        if (sFlArena.full) {
            port_log_summary("par-flush: the scratch store (GXV_PAR_ANIM_KB, %u KiB) was full in the last scene: some far models were computed as before",
                             sFlArena.size / 1024);
        }
        sFlArena.full = false;
    }
}
#endif

void ModelManager::calcAnim() {
    calc();
#ifdef TARGET_PC
    const int lazy = port_dl_lazy_on();
    if ((lazy == 2 || (lazy == 1 && port_turn_skipped_raw())) && gPortCalcAnimOwner != nullptr && gPortCalcAnimOwner->mModelManager == this &&
        port_on_frame_thread()) {
        // (listed once however many turns it skips)
        if (!mStaleDL) {
            mStaleDL = true;
            sStaleDL.push_back(StaleDL{gPortCalcAnimOwner, this});
        }
        return;
    }
    mStaleDL = false;  // (computed now; a stale entry still listed is skipped by the flush)
#endif
    updateDL(true);
}

void ModelManager::calcView() {
#ifdef TARGET_PC
    if (PortViewKeepCalc(this)) {
        return;  // (GXV_VIEW_KEEP: kept, or computed there)
    }
    if (PortParViewRecord(getJ3DModel())) {
        return;  // (GXV_PAR_VIEW: its arithmetic runs at the end of the pass, on either thread)
    }
#endif
    getJ3DModel()->viewCalc();
}

void ModelManager::entry() {
    getJ3DModel()->entry();
}

void ModelManager::newDifferedDLBuffer() {
    mDisplayListMaker->newDifferedDisplayList();
}

void ModelManager::updateDL(bool isDiffDL) {
#ifdef TARGET_PC
    // (GXV_DRAWDONE_DEFER=2: the front end may still be reading the last picture's lists while update() runs; a model that writes
    // its display lists outside calcAnim, EarthenPipe::control for one, waits for the deferred draw-done first. No wait is pending
    // in the ordinary case, and then this returns at once.)
    port_gx_drawdone_settle(3);  // (modes 1-3: a list written outside the drawn turn's flush waits for the front end)
    port_gx_view_write_from(__builtin_return_address(0));  // (modes 3, 4: the same, outside the flush, through the one counter)
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 familyToken = port_workload_anim_family_begin(2);
#endif
    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_material_checkpoint(familyToken, 1);
#endif

    bool isUpdateDL = mDisplayListMaker != nullptr && mDisplayListMaker->isValidDiff();

    if (isUpdateDL) {
        mDisplayListMaker->update();

        if (mBtkPlayer != nullptr) {
            mBtkPlayer->beginDiff();
        }

        if (mBrkPlayer != nullptr) {
            mBrkPlayer->beginDiff();
        }

        if (mBtpPlayer != nullptr) {
            mBtpPlayer->beginDiff();
        }

        if (mBpkPlayer != nullptr) {
            mBpkPlayer->beginDiff();
        }
    }

#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_material_checkpoint(familyToken, 2);
#endif
    getJ3DModel()->calcMaterial();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_material_checkpoint(familyToken, 4);
#endif

    if (isUpdateDL) {
        if (isDiffDL) {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
            port_workload_anim_material_checkpoint(familyToken, 3);
#endif
            mDisplayListMaker->diff();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
            port_workload_anim_material_checkpoint(familyToken, 4);
#endif
        }

        if (mBtkPlayer != nullptr) {
            mBtkPlayer->endDiff();
        }

        if (mBrkPlayer != nullptr) {
            mBrkPlayer->endDiff();
        }

        if (mBtpPlayer != nullptr) {
            mBtpPlayer->endDiff();
        }

        if (mBpkPlayer != nullptr) {
            mBpkPlayer->endDiff();
        }
    }

    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_family_end(familyToken);
#endif
}

void ModelManager::startBck(const char* pBckName, const char* pActorAnimName) {
    mXanimePlayer->changeAnimationBck(pBckName);
    mXanimePlayer->changeInterpoleFrame(0);
    changeBckSetting(pBckName, pActorAnimName, nullptr);
}

void ModelManager::startBckWithInterpole(const char* pBckName, s32 interpole) {
    mXanimePlayer->changeAnimationBck(pBckName);
    mXanimePlayer->changeInterpoleFrame(0);
    mXanimePlayer->changeInterpoleFrame(interpole);
}

void ModelManager::startBtk(const char* pBtkName) {
    stopBtk();
    mBtkPlayer->start(pBtkName);
    mDisplayListMaker->onCurFlagBtk(mBtkPlayer->mAnmRes);
}

void ModelManager::startBrk(const char* pBrkName) {
    stopBrk();
    mBrkPlayer->start(pBrkName);
    mDisplayListMaker->onCurFlagBrk(mBrkPlayer->mAnmRes);
}

void ModelManager::startBtp(const char* pBtpName) {
    stopBtp();
    mBtpPlayer->start(pBtpName);
    mDisplayListMaker->onCurFlagBtp(mBtpPlayer->mAnmRes);
}

void ModelManager::startBpk(const char* pBpkName) {
    stopBpk();
    mBpkPlayer->start(pBpkName);
    mDisplayListMaker->onCurFlagBpk(mBpkPlayer->mAnmRes);
}

void ModelManager::startBva(const char* pBvaName) {
    stopBva();
    mBvaPlayer->start(pBvaName);
}

void ModelManager::stopBtk() {
    if (mBtkPlayer->mAnmRes != nullptr) {
        mDisplayListMaker->offCurFlagBtk(mBtkPlayer->mAnmRes);
        mBtkPlayer->stop();
    }
}

void ModelManager::stopBrk() {
    if (mBrkPlayer->mAnmRes != nullptr) {
        mDisplayListMaker->offCurFlagBrk(mBrkPlayer->mAnmRes);
        mBrkPlayer->stop();
    }
}

void ModelManager::stopBtp() {
    if (mBtpPlayer->mAnmRes != nullptr) {
        mDisplayListMaker->offCurFlagBtp(mBtpPlayer->mAnmRes);
        mBtpPlayer->stop();
    }
}

void ModelManager::stopBpk() {
    if (mBpkPlayer->mAnmRes != nullptr) {
        mDisplayListMaker->offCurFlagBpk(mBpkPlayer->mAnmRes);
        mBpkPlayer->stop();
    }
}

void ModelManager::stopBva() {
    if (mBvaPlayer->mAnmRes != nullptr) {
        mBvaPlayer->stop();
    }
}

J3DFrameCtrl* ModelManager::getBckCtrl() const {
    return mXanimePlayer->_20;
}

J3DFrameCtrl* ModelManager::getBtkCtrl() const {
    return &mBtkPlayer->mFrameCtrl;
}

J3DFrameCtrl* ModelManager::getBrkCtrl() const {
    return &mBrkPlayer->mFrameCtrl;
}

J3DFrameCtrl* ModelManager::getBtpCtrl() const {
    return &mBtpPlayer->mFrameCtrl;
}

J3DFrameCtrl* ModelManager::getBpkCtrl() const {
    return &mBpkPlayer->mFrameCtrl;
}

J3DFrameCtrl* ModelManager::getBvaCtrl() const {
    return &mBvaPlayer->mFrameCtrl;
}

bool ModelManager::isBckStopped() const {
    return mXanimePlayer->_24[mXanimePlayer->_54].checkState(1);
}

bool ModelManager::isBtkStopped() const {
    if (mBtkPlayer != nullptr) {
        return mBtkPlayer->isStop();
    }

    return false;
}

bool ModelManager::isBrkStopped() const {
    if (mBrkPlayer != nullptr) {
        return mBrkPlayer->isStop();
    }

    return false;
}

bool ModelManager::isBtpStopped() const {
    if (mBtpPlayer != nullptr) {
        return mBtpPlayer->isStop();
    }

    return false;
}

bool ModelManager::isBpkStopped() const {
    if (mBpkPlayer != nullptr) {
        return mBpkPlayer->isStop();
    }

    return false;
}

bool ModelManager::isBvaStopped() const {
    if (mBvaPlayer != nullptr) {
        return mBvaPlayer->isStop();
    }

    return false;
}

bool ModelManager::isBtkPlaying(const char* pBtkName) const {
    return mBtkPlayer->isPlaying(pBtkName);
}

bool ModelManager::isBrkPlaying(const char* pBrkName) const {
    return mBrkPlayer->isPlaying(pBrkName);
}

bool ModelManager::isBpkPlaying(const char* pBpkName) const {
    return mBpkPlayer->isPlaying(pBpkName);
}

bool ModelManager::isBtpPlaying(const char* pBtpName) const {
    return mBtpPlayer->isPlaying(pBtpName);
}

bool ModelManager::isBvaPlaying(const char* pBvaName) const {
    return mBvaPlayer->isPlaying(pBvaName);
}

void ModelManager::initJointTransform() {
    mXanimePlayer->mCore->enableJointTransform(getJ3DModelData());
}

XjointTransform* ModelManager::getJointTransform(const char* pJointName) {
    s32 index = getJ3DModel()->getModelData()->getJointName()->getIndex(pJointName);
    return mXanimePlayer->mCore->getJointTransform(index);
}

ResourceHolder* ModelManager::getResourceHolder() const {
    if (mXanimeResourceTable == nullptr) {
        return mModelResourceHolder;
    }

    return mXanimeResourceTable->mResourceHolder;
}

ResourceHolder* ModelManager::getModelResourceHolder() const {
    return mModelResourceHolder;
}

J3DModel* ModelManager::getJ3DModel() const {
    if (mXanimePlayer == nullptr) {
        return mModel;
    }

    return mXanimePlayer->mModel;
}

J3DModelData* ModelManager::getJ3DModelData() const {
    return getJ3DModel()->mModelData;
}

const char* ModelManager::getPlayingBckName() const {
    if (mXanimePlayer != nullptr) {
        return mXanimePlayer->getCurrentBckName();
    }

    return nullptr;
}

void ModelManager::initModelAndAnimation(ResourceHolder* pModelHolder, const char* pModelName, ResourceHolder* pAnimationHolder, J3DMdlFlag flags) {
    mModelResourceHolder = pModelHolder;
    if (pAnimationHolder->mMotionResTable->mCount == 0) {
        mModel = MR::newJ3DModel(pModelHolder, pModelName, flags);
    } else {
        mXanimeResourceTable = MR::newXanimeResourceTable(pAnimationHolder);
        mXanimePlayer = MR::newXanimePlayer(pModelHolder, pModelName, pAnimationHolder, flags, mXanimeResourceTable);
    }
}

void ModelManager::initMaterialAnm() {
    ResourceHolder* pResourceHolder = getResourceHolder();
    J3DModelData* pModelData = getJ3DModelData();

    if (pResourceHolder->mMaterialBuf == nullptr) {
        pResourceHolder->newMaterialAnmBuffer(pModelData);
    }

    if (pResourceHolder->mBtkResTable->mCount != 0) {
        mBtkPlayer = new BtkPlayer(pResourceHolder, pModelData);
    }

    if (pResourceHolder->mBrkResTable->mCount != 0) {
        mBrkPlayer = new BrkPlayer(pResourceHolder, pModelData);
    }

    if (pResourceHolder->mBtpResTable->mCount != 0) {
        mBtpPlayer = new BtpPlayer(pResourceHolder, pModelData);
    }

    if (pResourceHolder->mBpkResTable->mCount != 0) {
        mBpkPlayer = new BpkPlayer(pResourceHolder, pModelData);
    }
}

void ModelManager::initVisibilityAnm() {
    ResourceHolder* pResourceHolder = getResourceHolder();

    if (pResourceHolder->mBvaResTable->mCount != 0) {
        mBvaPlayer = new BvaPlayer(pResourceHolder->getBvaResTable(), getJ3DModel());
    }
}

void ModelManager::calc() {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 familyToken = port_workload_anim_family_begin(1);
#endif
    if (mXanimePlayer != nullptr) {
        mXanimePlayer->calcAnm(0);
    }

    if (mBvaPlayer != nullptr) {
        mBvaPlayer->calc();
    }

    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
#ifdef TARGET_PC
    if (!PortAnimMemoCalc(this) && !PortParAnimCalc(this)) {
        getJ3DModel()->calc();  // (GXV_ANIM_MEMO, GXV_PAR_ANIM: a model that does not qualify, as before)
    }
#else
    getJ3DModel()->calc();
#endif
    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);

    if (mXanimePlayer != nullptr) {
        mXanimePlayer->clearAnm(0);
    }
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    port_workload_anim_family_end(familyToken);
#endif
}

void ModelManager::changeBckSetting(const char* pBckName, const char* pActorAnimName, XanimePlayer* pXanimePlayer) {
    if (pXanimePlayer == nullptr) {
        pXanimePlayer = mXanimePlayer;
    }

    BckCtrl* pBckCtrl = getResourceHolder()->mBckCtrl;

    if (pBckCtrl != nullptr) {
        if (pActorAnimName == nullptr) {
            pActorAnimName = pBckName;
        }

        pBckCtrl->changeBckSetting(pActorAnimName, pXanimePlayer);
    }
}

void ModelManager::init(const char* pModelName, const char* pAnimationName, bool deferDisplayList) {
    char modelArchive[64];
    snprintf(modelArchive, sizeof(modelArchive), "%s.arc", pModelName);
    ResourceHolder* modelHolder = MR::createAndAddResourceHolder(modelArchive);
    ResourceHolder* animationHolder;
    if (pAnimationName == nullptr) {
        animationHolder = modelHolder;
    } else {
        char animationArchive[64];
        snprintf(animationArchive, sizeof(animationArchive), "%s.arc", pAnimationName);
        animationHolder = MR::createAndAddResourceHolder(animationArchive);
    }

    bool hasMaterialAnm = animationHolder->isExistMaterialAnm();
    bool hasDiffMaterial = DisplayListMaker::isExistDiffMaterial(static_cast< J3DModelData* >(modelHolder->mModelResTable->getRes(pModelName)));
    J3DMdlFlag flags = J3DMdlFlag_UseSharedDL;
    if (deferDisplayList || hasMaterialAnm || hasDiffMaterial) {
        flags = static_cast< J3DMdlFlag >(flags | J3DMdlFlag_DifferedDLBuffer);
    }

    initModelAndAnimation(modelHolder, pModelName, animationHolder, flags);
    initVisibilityAnm();
    if (hasMaterialAnm) {
        initMaterialAnm();
    }

    if (flags & J3DMdlFlag_DifferedDLBuffer) {
        mDisplayListMaker = new DisplayListMaker(getJ3DModel(), getResourceHolder());
        if (!deferDisplayList) {
            mDisplayListMaker->newDifferedDisplayList();
        }
    }
}
