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
#include "Game/MapObj/StarPiece.hpp"  // (GXV_STARBATCH_DL: the star bit's colour)
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/CameraUtil.hpp"  // (GXV_MATCALM: the camera position)
#include <JSystem/J3DGraphBase/J3DPacket.hpp>  // (GXV_MATCALM: gJ3DPortListStats)
#include "Game/Util/ObjUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <cstdio>

#ifdef TARGET_PC
#include "port/diagnostics.h"
#include "port/logic_prof.h"  // (update25: GXV_LOGIC_PROF=3's per-model flush rows)
#include "port/gt71.h"  // (gt71)
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
extern "C" void port_drawpipe_join_before_material(void);  // (fx66: SceneFunction.cpp; GXV_PIPE25_JOINLATE: the helper is joined before calcAnim's material work)
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
#include "port/hot_knob.h"
#include <cstdlib>
// GXV_DL_LAZY_KEY (dlcrash, 8 Nov; default 1, vita/vgx-game/entry.cpp). The Bee Mushroom crash (2.0 .. 2.3, Honeyhive's crystal, two
// c3 dumps and the shipped 2.2's): a listed refresh is "updateDL(true) of this model manager", but what that call computes depends on
// where the manager points when it runs. MarioActor has one model manager and one display-list maker for his six bodies (normal, bone,
// bee, ice, rainbow, spring): MarioActor::changeDisplayMode points the animator's XanimePlayer (so getJ3DModel()) and
// mDisplayListMaker->mModel at mModels[n], and MarioActor::calcAnim asks for MR::updateModelDiffDL(this) only while the normal body
// (mCurrModel 0) is the one pointed at (the other bodies refresh through their own ModelHolders). Listed on the turn Mario touched the
// mushroom (still the normal body) and flushed after the next turn had switched to the bee body, the refresh ran Mario's maker over
// BeeMario's materials with Mario's flags: material 0 with fog + 2 texture numbers (0x10020000, 0xC0 bytes) into BeeMario's differed
// list sized for fog only (0x40), 0x60 bytes over the next J3DDisplayListObj, and the next material's endDiffDL copied to a pointer made
// of GX bytes. Electrified Mario (bone body) after a listed turn does the same.
// The fix: each listing notes the models it is for (the latest one wins: it is the refresh a synchronous call would have made last);
// the flush computes the entry only while the manager still points at them. Otherwise the refresh is left out: it was for a body that
// the manager no longer draws (the one it draws now was not asked for, exactly as with the synchronous call), and the next request
// for that body, made whenever it is shown again, lists it again. 0 = as before (A/B only: it crashes at the crystal).
namespace {
inline bool PortDlLazyKeyOn() {
    return PORT_HOT_KNOB(bool, [] {
        const char* e = getenv("GXV_DL_LAZY_KEY");
        return !(e != nullptr && *e == '0');
    }());
}
inline J3DModel* PortDlDiffModel(const ModelManager* mm) {
    return mm->mDisplayListMaker != nullptr ? mm->mDisplayListMaker->mModel : nullptr;
}
// (at every request, listed now or already listed)
inline void PortDlNoteTarget(ModelManager* mm) {
    mm->mStaleDLModel = mm->getJ3DModel();
    mm->mStaleDLDiffModel = PortDlDiffModel(mm);
}
inline bool PortDlTargetSame(const ModelManager* mm) {
    return mm->getJ3DModel() == mm->mStaleDLModel && PortDlDiffModel(mm) == mm->mStaleDLDiffModel;
}
unsigned sDlRetargeted = 0;
}  // namespace
// (GXV_DL_GUARD's log line, J3DPacket.cpp: the actor whose lists are being made, when known)
const LiveActor* gPortDLOwner = nullptr;
extern "C" const char* port_dl_owner_name(void) {
    const LiveActor* a = gPortDLOwner != nullptr ? gPortDLOwner : gPortCalcAnimOwner;
    return a != nullptr ? a->getName() : nullptr;
}
extern "C" int port_flush_prefetch_on(void);   // (GXV_FLUSH_PREFETCH: perf.cpp; the prefetches: LiveActor.cpp)
extern "C" void port_prefetch_actor(const void* actor);
extern "C" void port_prefetch_model(const void* actor);
extern "C" int port_dl_static_on(void);        // (GXV_DL_STATIC: perf.cpp)
extern "C" int port_dl_static_flagfix_on(void); // (GXV_DL_STATIC_FLAGFIX: perf.cpp, cpu60)
extern "C" int port_dl_static_tm_on(void);      // (GXV_DL_STATIC_TM: perf.cpp, game24)
extern "C" int port_draw_prof_on(void);
extern "C" int64_t port_host_time_ns(void);  // (dead26: PortDlCensus)
extern "C" void port_draw_prof_count(int what, unsigned n);
namespace {
// GXV_DL_STATIC (flushcut, 7 Nov). updateDL(true) of a model with no display-list maker and no material animation player is
// calcMaterial: j3dSys.setModel / flags / setTexture, then for each material: its animation (none), J3DMaterial::calc (the texture
// matrices of its texgen block, none in a material without any; then the current-matrix register words, set from the texgen's own
// matrix numbers, into the material and its shape). Nothing in a material without animation, without a texture matrix, in a model
// that nothing else feeds, can differ from one call to the next, except the two register words, which are constants of the material's
// data. A model that meets that test and has computed its materials in full once (with the same j3dSys flag 0x40000000, which picks the
// register words' source) leaves the repeats out. The flush still does what is global in calcMaterial: j3dSys's model, flags, texture and
// last material packet, in the same order. Where the test fails (a player started later, a texture matrix) the model is computed in
// full as before, and the memo starts again.
// Verify (GXV_DL_STATIC=2): the repeat is computed in full, and the register words of each material and its shape are compared with
// what they were before; "dl-static verify" in the draw-prof line (GXV_DRAW_PROF=2) counts the models compared and the ones that differed.
// cpu60-test-begin (tests/dl_static_why.cpp compiles the text between these markers)
// gamecpu25 (12 Nov): GXV_DL_STATIC_XANIM. The repeat test below took the model from ModelManager::mModel, which is set only for a model WITHOUT a
// BCK: with a Xanime player (every model that has a BCK) the model lives in mXanimePlayer->mModel and mModel stays null, so DlStaticQualifies
// answered "no data" for every animated model and never left one out (the console's "no data/shape 64-77" of ~97 lists a drawn turn in Honeyhive,
// "0 repeats compared" in every log). With XANIM on the model is getJ3DModel() (what updateDL's calcMaterial works on). The memo then also
// remembers WHICH model it was made for (mixed into mDlTmHash: mDlStatic alone says "computed in full with flag f", not for which model; the
// Xanime player's model can be re-pointed, MarioActor::changeDisplayMode). XANIM 0: the old behaviour bit for bit (null model for animated).
extern "C" int port_dl_static_xanim_on(void);  // (GXV_DL_STATIC_XANIM: perf.cpp)
// cpu60 (10 Nov): why a model was not left out. Counted only while GXV_DRAW_PROF=2 and GXV_DL_STATIC are on (port_draw_prof_count 8..15, the
// "dl-static why" part of the draw-prof line); the classification runs on the refusal path and never changes an answer.
// 0 left out, 1 computed in full: first time, or the post-texture-matrix flag changed (the model qualifies), 2 computed in full: a material's
// words differ from its shape's, 3 display-list maker, 4 a Btk/Brk/Btp/Bpk player, 5 a material animation on a material, 6 a plain
// (mode 0) texture matrix is the only thing in the way (GXV_DL_STATIC would take the model if it were allowed), 7 a texture matrix of another
// mode (environment, projection, effect matrix: it reads the view or the model matrix), 8 no model data / a material without a shape.
// game24 (GXV_DL_STATIC_TM): 9 computed in full: a plain texture matrix's content (or its texgen words) is not what it was after the last
// full computation; 10 (a subset of 0, not a reason of its own) left out although the model has plain texture matrices.
// With GXV_DL_STATIC_TM on, a plain (mode 0) texture matrix no longer disqualifies a model: J3DTexGenBlockPatched::calc / calcPostTexMtx run
// J3DTexMtx::calc(j3dDefaultMtx) for it, whose result mMtx is a function of the matrix's own info alone (default branch of calcTexMtx /
// calcPostTexMtx), and the texgen's register is a function of the data's post flag. So a model whose plain texture matrices (info, result,
// texgen fields, pointer) are bit-for-bit what the last full computation left, with the same flag, would compute the same bytes again.
// DlStaticTmHash is that content; the stamp that would skip even the hash (a generation bumped by every writer of a texture matrix pointer
// or info) is NOT built: its writers (btk calc, attach, MaterialAnmBuffer, TexMtxCtrl, setTexMtx) change every frame wherever a btk plays,
// and a missed writer would be silent, while the content hash cannot miss one.
enum DlWhy { kWhyLeftOut, kWhyFirst, kWhyWords, kWhyMaker, kWhyPlayer, kWhyMatAnm, kWhyPlainTexMtx, kWhyOtherTexMtx, kWhyNoData, kWhyTmChanged, kWhyLeftOutTm, kWhyCount };
bool DlStaticQualifiesImpl(ModelManager* mm, J3DModel* model, int* why, bool* hasTex = nullptr) {
    const bool tmOk = port_dl_static_tm_on() != 0;
    if (hasTex) *hasTex = false;
    if (model == nullptr) { if (why) *why = kWhyNoData; return false; }
    if (mm->mDisplayListMaker != nullptr) { if (why) *why = kWhyMaker; return false; }
    if (mm->mBtkPlayer != nullptr || mm->mBrkPlayer != nullptr || mm->mBtpPlayer != nullptr || mm->mBpkPlayer != nullptr) {
        if (why) *why = kWhyPlayer;
        return false;
    }
    J3DModelData* data = model->getModelData();
    if (data == nullptr) { if (why) *why = kWhyNoData; return false; }
    const u16 matNum = data->getMaterialNum();
    bool plain = false, other = false, anm = false, noShape = false;
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        if (material == nullptr || material->getShape() == nullptr) {
            if (!why) return false;
            noShape = true;
            if (material == nullptr) break;
            continue;
        }
        if (material->getMaterialAnm() != nullptr) {
            if (!why) return false;
            anm = true;
        }
        for (u32 j = 0; j < 8; j++) {
            J3DTexMtx* t = material->getTexMtx(j);
            if (t != nullptr) {
                const bool isPlain = (t->getTexMtxInfo().mInfo & 0x3F) == 0;
                if (!(isPlain && tmOk) && !why) return false;
                if (isPlain) { plain = true; if (hasTex) *hasTex = true; } else other = true;
            }
        }
    }
    if (why) {
        if (noShape) *why = kWhyNoData;
        else if (anm) *why = kWhyMatAnm;
        else if (other) *why = kWhyOtherTexMtx;
        else if (plain) *why = kWhyPlainTexMtx;
        else *why = kWhyFirst;
    }
    return !noShape && !anm && (!plain || tmOk) && !other;
}
bool DlStaticQualifies(ModelManager* mm, J3DModel* model, bool* hasTex = nullptr) {
    return DlStaticQualifiesImpl(mm, model, nullptr, hasTex);
}
// What the full computation leaves in the plain texture matrices of this model's materials (see the comment at DlWhy).
inline u64 DlTmMix(u64 h, u32 v) { return (h ^ v) * 0x100000001b3ull; }
u64 DlStaticTmHash(J3DModel* model, u8 flag) {
    u64 h = 0xcbf29ce484222325ull ^ flag;
    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* m = data->getMaterialNodePointer(i);
        for (u32 j = 0; j < 8; j++) {
            J3DTexMtx* t = m->getTexMtx(j);
            if (t == nullptr) continue;
            h = DlTmMix(h, (static_cast< u32 >(i) << 3) | j);
            h = DlTmMix(h, static_cast< u32 >(reinterpret_cast< uintptr_t >(t) >> 2));
            u32 w[(sizeof(J3DTexMtxInfo) + 3) / 4] = {};
            std::memcpy(w, &t->getTexMtxInfo(), sizeof(J3DTexMtxInfo));
            for (u32 k = 0; k < sizeof w / 4; k++) h = DlTmMix(h, w[k]);
            u32 r[(sizeof(t->getMtx()) + 3) / 4] = {};
            std::memcpy(r, &t->getMtx(), sizeof(t->getMtx()));
            for (u32 k = 0; k < sizeof r / 4; k++) h = DlTmMix(h, r[k]);
            J3DTexCoord* c = m->getTexCoord(j);
            if (c != nullptr) {
                h = DlTmMix(h, c->getTexGenType() | (c->getTexGenSrc() << 8) | (c->getTexGenMtx() << 16));
                h = DlTmMix(h, c->getTexMtxReg());
            }
        }
    }
    return h;
}
// The post-texture-matrix state calcMaterial will use for this model: J3DModelData::syncJ3DSysFlags (flag 0x20 of the data) sets j3dSys's
// 0x40000000 from it before the first material is calculated. cpu60: the first version asked j3dSys's flag as it stood at the flush, i.e.
// what the PREVIOUS listed model left, so in a list of models of different kinds a model asked "is the flag what I computed with" against
// another model's flag and was computed in full again and again (mDlStatic alternated 1 / 2 / 1). The data's own flag is what the full
// computation uses, so it is the right thing to compare.
inline u8 DlStaticFlagOf(J3DModel* model) {
    return model->getModelData()->checkFlag(0x20) ? 2 : 1;
}
// The outcome test: every material's register words already equal its shape's. What the full computation writes into the shape is the
// material's own words (J3DMaterial::setCurrentMtx), so where they agree the shape would be left as it is; where something else set the
// shape's words since (FurCtrl's draw does, to its fur layers' value), the model is computed in full as before.
bool DlStaticWordsAgree(J3DModel* model) {
    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* m = data->getMaterialNodePointer(i);
        if (m->getShape()->mCurrentMtx.mMtxIdxRegA != m->mCurrentMtx.mMtxIdxRegA || m->getShape()->mCurrentMtx.mMtxIdxRegB != m->mCurrentMtx.mMtxIdxRegB) {
            return false;
        }
    }
    return true;
}
// What J3DModel::calcMaterial does besides its materials, in its order (model, the two skinning flags, flags from the data, texture,
// and the packet of the last material).
void DlStaticSysState(J3DModel* model) {
    J3D_MSYS.setModel(model);
    if (model->checkFlag(J3DMdlFlag_SkinPosCpu)) {
        J3D_MSYS.onFlag(4);
    } else {
        J3D_MSYS.offFlag(4);
    }
    if (model->checkFlag(J3DMdlFlag_SkinNrmCpu)) {
        J3D_MSYS.onFlag(8);
    } else {
        J3D_MSYS.offFlag(8);
    }
    J3DModelData* data = model->getModelData();
    data->syncJ3DSysFlags();
    J3D_MSYS.setTexture(data->getTexture());
    const u16 matNum = data->getMaterialNum();
    if (matNum != 0) {
        J3D_MSYS.setMatPacket(model->getMatPacket(matNum - 1));
    }
}
// true: taken care of (left out, or in verify mode computed and compared); false: the caller computes in full.
inline J3DModel* DlStaticModelOf(ModelManager* mm) {
    return port_dl_static_xanim_on() != 0 ? mm->getJ3DModel() : mm->mModel;
}
// which model the memo is for: 0 with XANIM off (nothing mixed in), else a non-zero mix of the pointer
inline u64 DlStaticIdent(J3DModel* model) {
    return port_dl_static_xanim_on() != 0 ? ((static_cast< u64 >(reinterpret_cast< uintptr_t >(model)) * 0x9E3779B97F4A7C15ull) | 1ull) : 0ull;
}
bool DlStaticTry(ModelManager* mm, const StaleDL& e, int mode, unsigned* left, unsigned* verified, unsigned* bad, unsigned* why9) {
    J3DModel* model = DlStaticModelOf(mm);
    bool hasTex = false;
    if (!DlStaticQualifies(mm, model, &hasTex)) {
        mm->mDlStatic = 0;
        if (port_draw_prof_on() == 2) {
            int why = kWhyNoData;
            DlStaticQualifiesImpl(mm, model, &why);
            ++why9[why];
        }
        return false;
    }
    const u8 want = port_dl_static_flagfix_on() ? DlStaticFlagOf(model) : (J3D_MSYS.checkFlag(0x40000000) ? 2 : 1);
    if (mm->mDlStatic != want) {
        ++why9[kWhyFirst];
        return false;  // (computed in full by the caller; DlStaticDone below then notes it)
    }
    if (!DlStaticWordsAgree(model)) {
        ++why9[kWhyWords];
        return false;  // (see above: computed in full)
    }
    u64 tmNow = 0;
    const u64 ident = DlStaticIdent(model);
    if (hasTex || ident != 0) {
        tmNow = (hasTex ? DlStaticTmHash(model, want) : 0ull) ^ ident;
        if (tmNow != mm->mDlTmHash) {
            ++why9[hasTex ? kWhyTmChanged : kWhyFirst];  // (without texture matrices: the memo was made for another model)
            return false;  // (computed in full by the caller)
        }
    }
    if (mode == 2) {
        const J3DModelData* data = model->getModelData();
        const u16 matNum = data->getMaterialNum();
        u32 before[64][4];
        const bool fits = matNum <= 64;
        if (fits) {
            for (u16 i = 0; i < matNum; i++) {
                J3DMaterial* m = data->getMaterialNodePointer(i);
                before[i][0] = m->mCurrentMtx.mMtxIdxRegA;
                before[i][1] = m->mCurrentMtx.mMtxIdxRegB;
                before[i][2] = m->getShape()->mCurrentMtx.mMtxIdxRegA;
                before[i][3] = m->getShape()->mCurrentMtx.mMtxIdxRegB;
            }
        }
        mm->updateDL(true);
        if (hasTex && (DlStaticTmHash(model, want) ^ ident) != tmNow) {
            ++*bad;
            static unsigned sTmLogged = 0;
            if (sTmLogged < 8) {
                ++sTmLogged;
                port_log_summary("dl-static: verify MISMATCH \"%s\": the full computation changed a plain texture matrix (or its texgen) that GXV_DL_STATIC_TM would have left", e.owner->getName());
            }
        }
        if (fits) {
            ++*verified;
            for (u16 i = 0; i < matNum; i++) {
                J3DMaterial* m = data->getMaterialNodePointer(i);
                if (before[i][0] != m->mCurrentMtx.mMtxIdxRegA || before[i][1] != m->mCurrentMtx.mMtxIdxRegB ||
                    before[i][2] != m->getShape()->mCurrentMtx.mMtxIdxRegA || before[i][3] != m->getShape()->mCurrentMtx.mMtxIdxRegB) {
                    ++*bad;
                    static unsigned sLogged = 0;
                    if (sLogged < 8) {
                        ++sLogged;
                        port_log_summary("dl-static: verify MISMATCH \"%s\" material %u: material words %08x %08x -> %08x %08x, shape words %08x %08x -> %08x %08x",
                                         e.owner->getName(), static_cast< unsigned >(i), before[i][0], before[i][1], m->mCurrentMtx.mMtxIdxRegA,
                                         m->mCurrentMtx.mMtxIdxRegB, before[i][2], before[i][3], m->getShape()->mCurrentMtx.mMtxIdxRegA,
                                         m->getShape()->mCurrentMtx.mMtxIdxRegB);
                    }
                    break;
                }
            }
        }
        return true;
    }
    DlStaticSysState(model);
    ++*left;
    ++why9[kWhyLeftOut];
    if (hasTex) ++why9[kWhyLeftOutTm];
    return true;
}
// After a full computation: a model that qualifies has now computed its materials in full.
void DlStaticDone(ModelManager* mm) {
    J3DModel* model = DlStaticModelOf(mm);
    bool hasTex = false;
    mm->mDlStatic = DlStaticQualifies(mm, model, &hasTex) ? (port_dl_static_flagfix_on() ? DlStaticFlagOf(model) : (J3D_MSYS.checkFlag(0x40000000) ? 2 : 1)) : 0;
    mm->mDlTmHash = mm->mDlStatic != 0 ? ((hasTex ? DlStaticTmHash(model, mm->mDlStatic) : 0ull) ^ DlStaticIdent(model)) : 0;
}
// cpu60-test-end
}  // namespace
// ================================================================================================================================
// GXV_STARBATCH_DL (starbatch26; default 0; needs GXV_DL_STATIC on). A star bit's material list refresh (updateDL(true)) is the colour control's
// update (MatColorCtrl: the actor's mColor into the shared material's colour block), calcMaterial, and DisplayListMaker::diff (the instance's own
// differed list, made from the shared material). The model has a display-list maker, so GXV_DL_STATIC (kWhyMaker) always computes it in full,
// every picture it is posed: ~100 of them at Honeyhive. Its differed list is a function of the colour alone (the maker holds one colour control,
// no animation player runs, no material has an animation or a texture matrix), and the colour of a star bit changes only when it is placed
// (StarPiece::setColor / init). So a star bit whose colour is what it was after its last full refresh (the memo: mDlStatic = 9 and mDlTmHash = the
// colour mixed with the data's post-texture-matrix flag) and whose materials' register words still agree with their shapes' (DlStaticWordsAgree)
// leaves the refresh out: DlStaticSysState does what calcMaterial leaves in j3dSys, and the last flagged material's packet is set as diff()'s last
// loop turn sets it. Its differed list keeps the bytes (and the buffer) of the last refresh, which are the ones a refresh would write. The shared
// material keeps the colour the last refreshed instance wrote: nothing reads it between refreshes (every refresh sets the colour first).
// GXV_STARBATCH_DL=2 verifies: the refresh runs in full and the bytes of each material's differed list, and the register words, are compared with
// what they were before ("starbatch: dl ... differed" counts the models whose list changed: must be 0).
// Not for a model whose owner is not a star bit (mFlFam 1), with an animation player (a gift star bit's Btk), a material with an animation or a
// texture matrix, a maker with more than the one control (counted), or after any change of the colour or the model.
namespace {
std::atomic< unsigned > sSbDl[8];  // 0 considered, 1 left out, 2 computed (first / colour changed), 3 not eligible, 4 verified, 5 verified and differed, 6 words disagree
inline int SbDlMode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_STARBATCH_DL");
        const int v = (e != nullptr && *e != '\0') ? atoi(e) : 0;
        return v >= 1 && v <= 2 ? v : 0;
    }());
}
inline u32 SbDlColor(const LiveActor* o) {
    u32 c;
    std::memcpy(&c, &static_cast< const StarPiece* >(o)->mColor, 4);
    return c;
}
bool SbDlEligible(ModelManager* mm, const LiveActor* o) {
    if (o == nullptr || o->mFlFam != 1 || o->mModelManager != mm || mm->mDisplayListMaker == nullptr) {
        return false;
    }
    J3DModel* model = mm->getJ3DModel();
    if (model == nullptr || mm->mBtkPlayer != nullptr || mm->mBrkPlayer != nullptr || mm->mBtpPlayer != nullptr || mm->mBpkPlayer != nullptr ||
        mm->mDisplayListMaker->mFogCtrl != nullptr || mm->mDisplayListMaker->mMaterialCtrl.size() != 1) {
        return false;
    }
    J3DModelData* data = model->getModelData();
    if (data == nullptr) {
        return false;
    }
    const u16 matNum = data->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* m = data->getMaterialNodePointer(i);
        if (m == nullptr || m->getShape() == nullptr || m->getMaterialAnm() != nullptr || model->getMatPacket(i) == nullptr ||
            model->getMatPacket(i)->getShapePacket() == nullptr) {
            return false;
        }
        for (u32 j = 0; j < 8; j++) {
            if (m->getTexMtx(j) != nullptr) {
                return false;
            }
        }
    }
    return true;
}
inline u64 SbDlKey(J3DModel* model, const LiveActor* o) {
    return ((static_cast< u64 >(SbDlColor(o)) | (static_cast< u64 >(DlStaticFlagOf(model)) << 32) | (1ull << 40)) * 0x9E3779B97F4A7C15ull) | 1ull;
}
u32 SbDlListHash(J3DModel* model) {
    u32 h = 2166136261u;
    const u16 n = model->getModelData()->getMaterialNum();
    for (u16 i = 0; i < n; i++) {
        J3DDisplayListObj* dl = model->getMatPacket(i)->getShapePacket()->mpDisplayListObj;
        if (dl == nullptr) {
            h = (h ^ 0xFFu) * 16777619u;
            continue;
        }
        const u8* p = dl->getDisplayList(0);
        const u32 size = dl->getDisplayListSize();
        h = (h ^ size) * 16777619u;
        for (u32 k = 0; p != nullptr && k < size; k++) {
            h = (h ^ p[k]) * 16777619u;
        }
    }
    return h;
}
// The refresh of one eligible star bit: left out, or computed (and memoed), or (mode 2) computed and compared.
void SbDlRefresh(ModelManager* mm, const LiveActor* o, int mode) {
    J3DModel* model = mm->getJ3DModel();
    ++sSbDl[0];
    const u64 key = SbDlKey(model, o);
    if (mm->mDlStatic == 9 && mm->mDlTmHash == key) {
        if (!DlStaticWordsAgree(model)) {
            ++sSbDl[6];
        } else if (mode == 2) {
            const u32 before = SbDlListHash(model);
            mm->updateDL(true);
            ++sSbDl[4];
            if (SbDlListHash(model) != before || !DlStaticWordsAgree(model)) {
                ++sSbDl[5];
                static unsigned sLogged = 0;
                if (sLogged < 8) {
                    ++sLogged;
                    port_log_summary("starbatch: dl VERIFY MISMATCH \"%s\": the refresh changed a differed list (or a register word) that GXV_STARBATCH_DL would have left", o->getName());
                }
            }
            return;
        } else {
            DlStaticSysState(model);
            const DisplayListMaker* maker = mm->mDisplayListMaker;
            for (u16 i = model->getModelData()->getMaterialNum(); i-- > 0;) {
                if (maker->mCurFlag[i]) {
                    J3D_MSYS.setMatPacket(model->getMatPacket(i));  // (diff()'s last turn)
                    break;
                }
            }
            ++sSbDl[1];
            return;
        }
    }
    mm->updateDL(true);
    ++sSbDl[2];
    mm->mDlStatic = 9;
    mm->mDlTmHash = SbDlKey(model, o);
}
}  // namespace
extern "C" void port_starbatch_dl_counts(unsigned* out) {  // (J3DPacket.cpp's "starbatch:" line)
    for (int i = 0; i < 8; i++) {
        out[i] = sSbDl[i].load(std::memory_order_relaxed);
    }
}
extern "C" int port_starbatch_dl_mode(void) {
    return SbDlMode();
}
namespace {
struct DlCounts {
    unsigned updated = 0, leftOut = 0, notModel = 0, verified = 0, bad = 0;
    unsigned why[11] = {};  // (cpu60: DlStaticTry's reasons, see DlWhy)
};
// GXV_MATCALM=<n> (cpu34d, 14 Nov; 2..4; default 0 = off). A drawn turn's flush refreshes every listed model's material lists
// (updateDL(true): the material controls, calcMaterial, J3DMaterial::diff for each flagged material). In the Comet Observatory 235
// differed lists are made a drawn turn and 204 of them are, byte for byte, the list already stored (the "j3d lists:" line, console run
// 2900a): the 4.25 ms of the flush mostly rebuild lists that do not change. A model whose last GXV_MATCALM_RUNS (6) refreshes in a row
// stored no changed list (the count J3DDisplayListObj::endDiffDL keeps: made minus kept) is "calm": its refresh is made every n-th
// flush only (every 2nd within GXV_MATCALM_NEAR, 1500 units, of the camera), until a refresh stores a change (then every flush again,
// and the count starts over). The start of a Btk / Brk / Btp / Bpk on the model ends the calm at once (ModelManager::startBtk ..),
// so an animation that starts is shown on its own turn. What can be late: a change made another way (a colour control's value, a
// texture matrix that follows the camera when the camera starts to move, a frame set directly) first shows up to n-1 drawn turns
// (33-67 ms at n = 3) later on that model; from then on the model is refreshed every flush. A MINOR CHANGE, not exact.
// Never: the player (a camera controller), a model never refreshed, the GXV_PAR_DL worker's flush. A model left out keeps j3dSys as
// the full computation leaves it (DlStaticSysState), as GXV_DL_STATIC does. After GXV_DL_STATIC / GXV_STARBATCH_DL had their say.
// GXV_MATCALM_DRY=1: nothing is left out; the refreshes the rule would leave out are timed and those that stored a change counted.
// Line "matcalm:" every 5 s.
// fix1 (review34 M1, M2, L3):
//  - A stored list is not the refresh's only output: calcMaterial also writes the materials' texture matrices, and a shape packet with a
//    texture-matrix object has them read at DRAW time (J3DDifferedTexMtx::loadExecute), not through a list. So never calm: a model with
//    a shape packet that has a texture-matrix object, or with a texture matrix of any mode but the plain one (environment, projection,
//    view-projection maps: they follow the camera, the model or an effect matrix every turn), or while one of its Btk / Brk / Btp / Bpk
//    players runs. CalmExcluded says which; such a model's count never reaches the calm ("never calm" in the line, per flush).
//  - A refresh left out is OWED: the entry goes back on the list (mStaleDL stays set), so the following flushes see the model whether
//    or not its calcAnim still runs (a stop, a pause, a clipped actor) and the n-th makes the refresh.
//  - The models left out are no longer added to GXV_DL_STATIC's "left out".
extern "C" int port_matcalm_n(void);  // (the switch, below)
extern "C" int port_safe_cam_exists(void);  // (LodCtrl.cpp)
namespace {
struct CalmCounts {
    unsigned asked = 0, left = 0, made = 0, changed = 0, dryLeft = 0, dryChanged = 0, flushes = 0, exTex = 0, exPlayer = 0, owed = 0;
    int64_t dryNs = 0, t0 = 0;
};
CalmCounts sCalm;
inline int CalmEnv(const char* name, int fallback) {
    const char* e = getenv(name);
    return e != nullptr && *e != 0 ? atoi(e) : fallback;
}
inline int CalmRuns() {
    return PORT_HOT_KNOB(int, [] { const int v = CalmEnv("GXV_MATCALM_RUNS", 6); return v < 2 ? 2 : v > 200 ? 200 : v; }());
}
inline float CalmNear2() {
    return PORT_HOT_KNOB(float, [] { const float v = static_cast< float >(CalmEnv("GXV_MATCALM_NEAR", 1500)); return v * v; }());
}
inline bool CalmDry() {
    return PORT_HOT_KNOB(bool, CalmEnv("GXV_MATCALM_DRY", 0) != 0);
}
inline unsigned CalmStored() {
    return gJ3DPortListStats.diffMade - gJ3DPortListStats.diffKept;
}
// (fix1) a material animation player of the model is running: its frames reach the materials every turn
inline bool CalmPlayerRuns(const ModelManager* mm) {
    return (mm->mBtkPlayer != nullptr && !mm->mBtkPlayer->isStop()) || (mm->mBrkPlayer != nullptr && !mm->mBrkPlayer->isStop()) ||
           (mm->mBtpPlayer != nullptr && !mm->mBtpPlayer->isStop()) || (mm->mBpkPlayer != nullptr && !mm->mBpkPlayer->isStop());
}
// (fix1) the refresh has an output that is not a stored list, or an input that moves every turn: a shape packet with a texture-matrix
// object (read at draw time), a texture matrix of a mode other than the plain one, a texture SRT player on the model. Walks the
// materials: asked only when a model's count is about to reach the calm, and at each refresh a calm model makes.
bool CalmTexDriven(ModelManager* mm) {
    if (mm->mBtkPlayer != nullptr) {
        return true;
    }
    J3DModel* model = mm->getJ3DModel();
    if (model == nullptr || model->getModelData() == nullptr) {
        return true;
    }
    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    for (u16 i = 0; i < matNum; i++) {
        J3DMaterial* material = data->getMaterialNodePointer(i);
        if (material == nullptr) {
            return true;
        }
        for (u32 j = 0; j < 8; j++) {
            J3DTexMtx* t = material->getTexMtx(j);
            if (t != nullptr && (t->getTexMtxInfo().mInfo & 0x3F) != 0) {
                return true;
            }
        }
    }
    const u16 shapeNum = data->getShapeNum();
    for (u16 i = 0; i < shapeNum; i++) {
        J3DShapePacket* sp = model->getShapePacket(i);
        if (sp != nullptr && sp->getTexMtxObj() != nullptr) {
            return true;
        }
    }
    return false;
}
// true: this flush's refresh of the model is one the rule leaves out
bool CalmWouldSkip(const StaleDL& e, int n) {
    ModelManager* mm = e.model;
    if (mm->mCalmRun >= CalmRuns() && CalmPlayerRuns(mm)) {
        mm->mCalmRun = 0;  // (fix1: a player runs: every flush, and the count starts over)
        mm->mCalmSkip = 0;
        ++sCalm.exPlayer;
        return false;
    }
    if (mm->mCalmRun < CalmRuns() || mm->mCpDlMade == 0 || e.owner->mCameraCtrl != nullptr || !port_safe_cam_exists()) {
        return false;
    }
    int every = n;
    const TVec3f cam = MR::getCamPos();
    const TVec3f& p = e.owner->mPosition;
    const float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    if (dx * dx + dy * dy + dz * dz < CalmNear2()) {
        every = 2;
    }
    return static_cast< int >(mm->mCalmSkip) + 1 < every;
}
void CalmLog(int n) {
    CalmCounts& k = sCalm;
    ++k.flushes;
    const int64_t now = port_host_time_ns();
    if (k.t0 == 0) {
        k.t0 = now;
        return;
    }
    if (now - k.t0 < 5000000000LL) {
        return;
    }
    const double f = k.flushes != 0 ? static_cast< double >(k.flushes) : 1.0;
    port_log_summary("matcalm: GXV_MATCALM=%d (calm after %d unchanged refreshes; every 2nd within %d units), %.1f s, %u flushes; a flush: refreshes "
                     "asked %.1f, left out %.1f (owed, listed again %.1f), made %.1f (%.1f of them stored a changed list); never calm: %.2f for a texture "
                     "matrix read at the draw or following the camera / an effect, %.2f for a running Btk/Brk/Btp/Bpk; dry (GXV_MATCALM_DRY=%d): would "
                     "leave out %.1f, %.2f of them stored a change (a change shown late), their time %.3f ms a flush",
                     n, CalmRuns(), CalmEnv("GXV_MATCALM_NEAR", 1500), static_cast< double >(now - k.t0) / 1e9, k.flushes, k.asked / f, k.left / f, k.owed / f, k.made / f,
                     k.changed / f, k.exTex / f, k.exPlayer / f, CalmDry() ? 1 : 0, k.dryLeft / f, k.dryChanged / f, static_cast< double >(k.dryNs) / 1e6 / f);
    k = CalmCounts();
    k.t0 = now;
}
}  // namespace
// port_dl_flush_stale's work for one listed entry (not dropping) whose actor is still one of the scene's and still owns the manager (the
// caller has checked both). GXV_PAR_DL runs the same function on its worker, with the knobs read on the game thread.
void PortDlRefreshOne(const StaleDL& e, int staticMode, bool lazyKey, DlCounts& c) {
    bool calmOwed = false;  // (GXV_MATCALM fix1: the refresh was left out and the entry listed again)
    if (e.model->mStaleDL && lazyKey && !PortDlTargetSame(e.model)) {
        // (GXV_DL_LAZY_KEY: the manager points at another model since the request; see above)
        ++sDlRetargeted;
        if (sDlRetargeted <= 8 || (sDlRetargeted & 1023) == 0) {
            port_log_summary("dl-lazy: stale entry \"%s\" left out (%u so far): its model manager now points at model %p / maker %p, "
                             "the refresh was asked for %p / %p",
                             e.owner->getName(), sDlRetargeted, static_cast< void* >(e.model->getJ3DModel()),
                             static_cast< void* >(PortDlDiffModel(e.model)), static_cast< void* >(e.model->mStaleDLModel),
                             static_cast< void* >(e.model->mStaleDLDiffModel));
        }
        ++c.notModel;
    } else if (e.model->mStaleDL) {
        gPortDLOwner = e.owner;
        if (staticMode != 0 && SbDlMode() != 0 && SbDlEligible(e.model, e.owner)) {
            const unsigned left0 = sSbDl[1].load(std::memory_order_relaxed);
            SbDlRefresh(e.model, e.owner, SbDlMode());
            if (sSbDl[1].load(std::memory_order_relaxed) != left0) {
                ++c.leftOut;
            } else {
                ++c.updated;
            }
        } else if (staticMode != 0 && DlStaticTry(e.model, e, staticMode, &c.leftOut, &c.verified, &c.bad, c.why)) {
            // (left out, or computed and compared)
        } else {
            // (GXV_MATCALM, above)
            const int calmN = gPortParDlRun ? 0 : port_matcalm_n();
            bool calmSkip = false, calmDry = false;
            if (__builtin_expect(calmN >= 2, 0)) {
                ++sCalm.asked;
                calmSkip = CalmWouldSkip(e, calmN);
                if (calmSkip && CalmDry()) {
                    calmSkip = false;
                    calmDry = true;
                }
            }
            if (calmSkip) {
                ++e.model->mCalmSkip;
                ++sCalm.left;
                // (fix1: owed. The entry is listed again and mStaleDL stays set, so the next flushes come back to this model even if
                // its calcAnim no longer runs; port_dl_flush_stale took its work list out before this loop, so this is the new list.)
                sStaleDL.push_back(e);
                calmOwed = true;
                ++sCalm.owed;
                J3DModel* calmModel = e.model->getJ3DModel();
                if (calmModel != nullptr) {
                    DlStaticSysState(calmModel);
                }
            } else {
                const unsigned stored0 = calmN >= 2 ? CalmStored() : 0;
                const int64_t calmT0 = calmDry ? port_host_time_ns() : 0;
                e.model->updateDL(true);
                ++c.updated;
                if (staticMode != 0) {
                    DlStaticDone(e.model);
                }
                if (calmN >= 2) {
                    const bool changed = CalmStored() != stored0;
                    ++sCalm.made;
                    sCalm.changed += changed ? 1u : 0u;
                    if (calmDry) {
                        ++sCalm.dryLeft;
                        sCalm.dryChanged += changed ? 1u : 0u;
                        sCalm.dryNs += port_host_time_ns() - calmT0;
                        ++e.model->mCalmSkip;  // (the phase the rule would have: the n-th asks for a real refresh)
                    } else {
                        e.model->mCalmSkip = 0;
                    }
                    if (changed) {
                        e.model->mCalmRun = 0;
                        e.model->mCalmSkip = 0;
                    } else if (static_cast< int >(e.model->mCalmRun) + 1 >= CalmRuns() && CalmTexDriven(e.model)) {
                        // (fix1: about to be, or already, calm, but its refresh feeds the draw outside the stored lists: never calm)
                        e.model->mCalmRun = 0;
                        e.model->mCalmSkip = 0;
                        ++sCalm.exTex;
                    } else if (e.model->mCalmRun < 255) {
                        ++e.model->mCalmRun;
                    }
                }
            }
        }
        gPortDLOwner = nullptr;
    }
    e.model->mStaleDL = calmOwed;
}
void PortDlProfCounts(const DlCounts& c) {
    for (unsigned k = 0; k < 11; ++k) {
        port_draw_prof_count(8 + k, c.why[k]);
    }
    port_draw_prof_count(2, c.updated + c.verified);
    port_draw_prof_count(3, c.leftOut);
    port_draw_prof_count(4, c.notModel);
    port_draw_prof_count(5, c.verified);
    port_draw_prof_count(6, c.bad);
}
// dead26 (12 Nov): the "dl-static:" census line, every 5 s, with GXV_DL_STATIC=1 or 2 and no other switch. The old verify counts were only in the
// "draw-prof:" line (GXV_DRAW_PROF=2); the mismatch lines (port_log_summary) printed only on a failure, so a clean verify run showed nothing.
// Game thread only (port_dl_flush_stale, and the GXV_PAR_DL join).
void PortDlCensus(const DlCounts& c, int staticMode) {
    static DlCounts sum;
    static int64_t t0 = 0;
    static unsigned flushes = 0;
    ++flushes;
    sum.updated += c.updated;
    sum.leftOut += c.leftOut;
    sum.notModel += c.notModel;
    sum.verified += c.verified;
    sum.bad += c.bad;
    for (unsigned k = 0; k < 11; ++k) {
        sum.why[k] += c.why[k];
    }
    const int64_t now = port_host_time_ns();
    if (t0 == 0) {
        t0 = now;
        port_log_summary("dl-static: GXV_DL_STATIC=%d GXV_DL_STATIC_XANIM=%d GXV_DL_STATIC_TM=%d GXV_DL_STATIC_FLAGFIX=%d: census every 5 s", staticMode,
                         port_dl_static_xanim_on(), port_dl_static_tm_on(), port_dl_static_flagfix_on());
        return;
    }
    if (now - t0 < 5000000000LL) {
        return;
    }
    const double secs = static_cast< double >(now - t0) / 1e9;
    const unsigned all = sum.updated + sum.leftOut + sum.verified;
    port_log_summary("dl-static: %.1f s, %u flushes, %u material lists asked: %u left out (%.0f %%), %u computed, %u computed and compared (verify, GXV_DL_STATIC=2); "
                     "verify: %u repeats compared, %u differed%s; not an actor's model %u; refused: first or flag changed %u, words differ %u, maker %u, player %u, "
                     "material animation %u, plain texture matrix only %u, other texture matrix %u, no data/shape %u",
                     secs, flushes, all, sum.leftOut, all != 0 ? 100.0 * sum.leftOut / all : 0.0, sum.updated, sum.verified, sum.verified, sum.bad,
                     staticMode == 2 ? (sum.bad == 0 && sum.verified != 0 ? " (clean)" : sum.bad != 0 ? " (MISMATCHES: see the dl-static: verify MISMATCH lines)" : " (nothing compared yet)") : " (verify is off)",
                     sum.notModel, sum.why[kWhyFirst], sum.why[kWhyWords], sum.why[kWhyMaker], sum.why[kWhyPlayer], sum.why[kWhyMatAnm], sum.why[kWhyPlainTexMtx],
                     sum.why[kWhyOtherTexMtx], sum.why[kWhyNoData]);
    sum = DlCounts();
    flushes = 0;
    t0 = now;
}
}  // namespace
// GXV_MATCALM (see PortDlRefreshOne): 0 off, 2..4 = a calm model's refresh every n-th flush
extern "C" int port_matcalm_n(void) {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_MATCALM");
        const int v = e != nullptr && *e != 0 ? atoi(e) : 0;
        return v < 2 ? 0 : v > 4 ? 4 : v;
    }());
}
extern "C" void port_dl_flush_stale(int drop) {
    if (sStaleDL.empty()) {
        return;
    }
    port_live_actor_snapshot();
    // (taken out first: updateDL can list models while this runs, and a reallocated list under the loop crashed on 3 Nov)
    PortHostVector< StaleDL > work;
    work.swap(sStaleDL);
    const bool prefetch = !drop && port_flush_prefetch_on() != 0;
    const int staticMode = drop ? 0 : port_dl_static_on();
    const bool lazyKey = PortDlLazyKeyOn();
    DlCounts c;
    const size_t count = work.size();
    if (prefetch) {
        for (size_t k = 0; k < 6 && k < count; ++k) {
            port_prefetch_actor(work[k].owner);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (prefetch) {
            if (i + 6 < count) {
                port_prefetch_actor(work[i + 6].owner);
            }
            if (i + 3 < count && port_live_actor_valid(work[i + 3].owner)) {
                port_prefetch_model(work[i + 3].owner);
            }
        }
        const StaleDL e = work[i];
        if (!port_live_actor_valid(e.owner) || e.owner->mModelManager != e.model) {
            ++c.notModel;
            continue;
        }
        if (drop) {
            e.model->mStaleDL = false;  // (the scene is about to go; a model that outlives it is listed again)
            continue;
        }
        if (__builtin_expect(::port::LogicProfOn(2), 0)) {
            // (update25, GXV_LOGIC_PROF=3, a sampled turn: this model's refresh timed under its owner's class)
            const unsigned leftBefore = c.leftOut + c.notModel;
            port_logic_prof_flush_begin(0, e.owner, e.owner->getName());
            PortDlRefreshOne(e, staticMode, lazyKey, c);
            port_logic_prof_flush_end(c.leftOut + c.notModel != leftBefore ? 1 : 0);
            continue;
        }
        PortDlRefreshOne(e, staticMode, lazyKey, c);
    }
    if (!drop && port_draw_prof_on()) {
        PortDlProfCounts(c);
    }
    if (!drop && staticMode != 0) {
        PortDlCensus(c, staticMode);  // (dead26)
    }
    if (!drop && port_matcalm_n() >= 2) {
        CalmLog(port_matcalm_n());  // (GXV_MATCALM)
    }
    work.clear();
    if (sStaleDL.empty() && sStaleDL.capacity() < work.capacity()) {
        sStaleDL.swap(work);  // (keep the larger buffer when nothing was listed meanwhile)
    }
}

// ================================================================================================================================
// GXV_PAR_DL (gamedraw23, 8 Nov; default 0; platform/src/port/perf.cpp, platform/include/port/par_dl.h, GameSystem::frameLoop).
// The drawn turn runs three flushes on the game thread, one after the other: the far models' joints (anim), the material lists
// (this, port_dl_flush_stale: ~2.5 ms a drawn turn in heavy Honeyhive) and the deferred view pass (port_view_flush: ~0.9 ms). With
// GXV_PAR_DL on, the material lists are made by a worker on the fourth core WHILE the game thread makes the view pass; the game thread
// joins the worker before the draw. The game thread saves the shorter of the two (the view pass, as long as the lists take longer).
//
// Why the pictures and the game are exactly as before (the serial order is: material lists, then view pass):
//  1. The worker runs this file's own loop (PortDlRefreshOne: updateDL / GXV_DL_STATIC / GXV_DL_LAZY_KEY as port_dl_flush_stale) over the
//     same list in the same order: one thread makes every material list, as before, so whatever the material path keeps in shared
//     state (the materials of a model data shared by instances, the animation resources' frames, J3DDisplayListObj::sGDLObj, GD's
//     current list, the differed-list scratch buffer, J3DSys::sTexCoordScaleTable) is written in the same order with the same values.
//  2. The view pass touches nothing the material path reads or writes, and the reverse, except j3dSys (one global J3DSys): the view
//     pass writes its view matrix (identity for the 2D groups, then the camera's) and reads it; the material path reads the view
//     matrix (environment and view-projection texture matrices) and writes the model, flags, texture and material packet. The view
//     pass (executeCalcViewAndEntryListExceptPlayer -> DrawBufferExecuter -> LiveActor::calcViewAndEntry -> ModelManager::calcView:
//     GXV_VIEW_KEEP, GXV_PAR_VIEW, J3DModel::viewCalc; the overrides MirrorActor, PartsModel, RailBlock, WoodBox, MirrorReflectionModel
//     end there too) computes draw / normal / bump matrices from the joints, which the material path only reads, and the shape packets'
//     matrix pointers, which it does not touch. So the material path gets a J3DSys of its own for the overlap (J3D_MSYS, J3DSys.hpp:
//     a copy of j3dSys made at the dispatch = the j3dSys the serial flush would have seen, view matrix included), and at the join every
//     word the worker changed is written into j3dSys unless the view pass changed it too (port::par::MergeWords; then the view pass's
//     value stands, as it comes later in the serial order). j3dSys is then what the serial flush + view pass left.
//  3. Only the player's view pass uses the material path's shared state (MarioActor::calcFogLighting -> updateLightDL: GD's current list
//     and the guest scheduler's prohibit; the search light and its hand models' J3DModel::calc write j3dSys). The overlap therefore never
//     runs it: either it was made in calc-anim (PETARI_VIEW_PLAYER, mode <= 2 or the front end done), or (round 2, GXV_PAR_DL_PLAYER) the
//     pending pass is split and the player's part runs after the join, on the merged j3dSys (GameScene.cpp port_view_split_player_last:
//     why its place among the groups changes nothing). Its j3dSys and GD writes are then the last ones, as in the serial pass.
//  4. Should the game thread reach the material path while the worker runs (an entry into J3DModel::calcMaterial / makeDL / entry /
//     calcDiffTexMtx, J3DMaterial::calc / diff / makeDisplayList / patch, DisplayListMaker::update / diff, a differed list's beginDL,
//     a material animation's beginDiff, updateDL: PortDlGameEnter), it first finishes the overlap (port_par_dl_game_enter: waits for the
//     worker, merges j3dSys). In the serial order that call came after every material list too, so it sees exactly what it saw. None is
//     known; the log counts them ("finished early").
//  5. Guest OS: the worker is no guest thread. The game thread takes the material path's mutex (MR::MutexHolder<0>, as updateDL does per
//     model) and turns the guest's interrupts off for the whole overlap, so no other guest thread (the loading thread: model set-up,
//     updateDL) can run material code meanwhile; the worker makes none of the guest calls the serial flush makes per list (interrupts,
//     the scheduler's prohibit and yield, the mutex: J3DPacket.cpp, DisplayListMaker.cpp, updateDL), nor the front-end waits (the game
//     thread made the draw-done wait before the flushes and is inside the flush scope) nor the logging builds' workload counters.
//  6. Worker identity: the actors' validity (the live-actor snapshot) and the knobs are read on the game thread before the dispatch.
//     If the worker has not started when the game thread joins, the game thread takes the job and runs it itself (same function, same
//     private J3DSys: after the view pass, which does not read what it writes).
// Verify (GXV_PAR_DL=2): the material state the lists can write (per material of the listed models' model data: TEV / konst colours,
// texture numbers, material / ambient colours, fog, texture-matrix pointers and contents, texture coordinates, current-matrix words,
// the shape's) is saved before the dispatch; after the join the overlapped result is fingerprinted (per entry, right after it: the
// differed lists' bytes, the texture matrices and current-matrix words) and saved, the saved state is put back with j3dSys as it was at
// the dispatch, and the serial flush runs again on the game thread (the original guest calls included) and is compared entry by entry,
// material by material and word by word (j3dSys, the view pass's words overlaid). The game goes on with the serial result.
// Log (every 5 s, basic log): "par-dl: ..." (perf.cpp).
extern "C" int port_par_dl_mode(void);                                    // (perf.cpp: GXV_PAR_DL 0 / 1 / 2)
extern "C" int port_par_dl_start(void);                                   // (the worker, started once; 0: none)
extern "C" void port_par_dl_dispatch(void (*fn)(void*), void* ctx);
extern "C" int port_par_dl_take(void);
extern "C" void port_par_dl_wait(unsigned* waitUs);
extern "C" void port_par_dl_reset(void);
extern "C" void port_par_dl_note(const unsigned* counts, unsigned n);
extern "C" int port_view_pending_except_player(void);                     // (GameScene.cpp)
extern "C" int port_view_pending_state(void);                             // (GameScene.cpp: 0 none, 1 with the player's, 2 without, 3 splittable)
extern "C" int port_view_split_player_last(void);                         // (GameScene.cpp)
extern "C" int port_par_dl_player_on(void);                               // (perf.cpp: GXV_PAR_DL_PLAYER)
extern "C" int port_par_dl_defer_view(void);                              // (perf.cpp: GXV_PAR_DL_VIEW)
extern "C" int port_gx_drawdone_defer_mode(void);
extern "C" int port_on_frame_thread(void);                                // (perf.cpp)
extern "C" unsigned port_par_dl_self(void);                               // (perf.cpp: this thread's id, never 0)
extern "C" int port_par_dl_done(void);                                    // (perf.cpp: the worker has finished the job)
extern "C" void port_par_dl_stack(unsigned long* lo, unsigned long* hi);  // (perf.cpp: the worker's stack)
extern "C" int port_par_dl_help_on(void);                                 // (perf.cpp: GXV_PAR_DL_HELP)
extern "C" int port_par_view_help_one(void);                              // (below)
extern "C" int64_t port_host_time_ns(void);
#include "port/par_dl.h"
#include "Game/LiveActor/ParDlSnap.hpp"
#include <atomic>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
namespace {
// (GXV_PAR_DL=2) one entry's outputs, right after its refresh (ParDlSnap.hpp)
u32 ParDlFingerprint(ModelManager* mm) {
    J3DModel* model = mm->getJ3DModel();
    if (model == nullptr || model->getModelData() == nullptr) return 2166136261u;
    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    J3DMaterial* mats[256];
    J3DMatPacket* packets[256];
    const u16 n = matNum < 256 ? matNum : 256;
    for (u16 i = 0; i < n; ++i) {
        mats[i] = data->getMaterialNodePointer(i);
        packets[i] = model->getMatPacket(i);
    }
    return ParDlFingerprint(mats, packets, n);
}
void ParDlFingerprintParts(ModelManager* mm, u32 out[5]) {
    for (u32 k = 0; k < 5; ++k) out[k] = 2166136261u;
    J3DModel* model = mm->getJ3DModel();
    if (model == nullptr || model->getModelData() == nullptr) return;
    J3DModelData* data = model->getModelData();
    const u16 matNum = data->getMaterialNum();
    J3DMaterial* mats[256];
    J3DMatPacket* packets[256];
    const u16 n = matNum < 256 ? matNum : 256;
    for (u16 i = 0; i < n; ++i) {
        mats[i] = data->getMaterialNodePointer(i);
        packets[i] = model->getMatPacket(i);
    }
    ParDlFingerprintParts(mats, packets, n, out);
}
struct ParDl {
    PortHostVector< StaleDL > work;
    PortHostVector< u8 > valid, stale0, static0, firstOfData;
    PortHostVector< u64 > tm0;  // (game24: the model's plain texture matrix hash, saved and restored with static0)  // (per entry; the last three for the verify)
    PortHostVector< u32 > fpPar, fpSer;
    PortHostVector< u32 > partsPar;  // (fedl25: 5 words an entry, ParDlFingerprintParts after the overlapped refresh)
    PortHostVector< u8 > firstUse;   // (fedl25: the entry's model data was never refreshed before this turn)
    PortHostVector< ParDlMatSnap > snap0, snap1, snap2;
    int staticMode = 0;
    bool lazyKey = true, prefetch = false, verify = false;
    bool open = false, joined = false;
    bool tookJob = false, playerLast = false;
    unsigned waitUs = 0, taken = 0, conflicts = 0, helped = 0;
    BOOL ints = FALSE;
    DlCounts c;
    int64_t t0 = 0;
    unsigned earlyFinish = 0;
    alignas(8) u8 sys[sizeof(J3DSys)];
    alignas(8) u8 pre[sizeof(J3DSys)];
    alignas(8) u8 gameSys[sizeof(J3DSys)];
    // (defaults48, GXV_PAR_DL=2) J3DSys::sTexCoordScaleTable as the flush found it. A material's diff can read a slot it does not write in the same pass
    // (J3DTevBlock::diff: diffTexCoordScale without diffTexNo, or a TEV order naming a texture map whose number is 0xFFFF), so an entry's list bytes
    // depend on what the last writer BEFORE the flush left there. The serial redo started from what the overlapped pass's last entry left instead:
    // the one "MISMATCH" of the Vita run of 22 Nov (night35 row 16, Beach Bowl, 1 entry of 444,200: the Low penguin, first of its model data that turn;
    // list bytes differed, list sizes, texture matrices, current-matrix words and every material state the same) is that case, not a difference
    // between the worker and the serial order. The redo now starts from the saved table, and the tables after both passes are compared.
    J3DTexCoordScaleInfo scale0[8];
};
ParDl sParDl;
// The job: port_dl_flush_stale's loop over the entries the game thread found valid, with the knobs it read.
std::atomic< unsigned > sParDlRunner{0};  // (the thread running the job: the worker, or the game thread that took it; 0 between jobs)
void ParDlRun(void* ctx) {
    ParDl& d = *static_cast< ParDl* >(ctx);
    sParDlRunner.store(port_par_dl_self(), std::memory_order_relaxed);
    const size_t count = d.work.size();
    if (d.prefetch) {
        for (size_t k = 0; k < 6 && k < count; ++k) {
            port_prefetch_actor(d.work[k].owner);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (d.prefetch) {
            if (i + 6 < count) {
                port_prefetch_actor(d.work[i + 6].owner);
            }
            if (i + 3 < count && d.valid[i + 3]) {
                port_prefetch_model(d.work[i + 3].owner);
            }
        }
        if (!d.valid[i]) {
            continue;  // (counted by the game thread)
        }
        PortDlRefreshOne(d.work[i], d.staticMode, d.lazyKey, d.c);
        if (d.verify) {
            d.fpPar[i] = ParDlFingerprint(d.work[i].model);
            ParDlFingerprintParts(d.work[i].model, &d.partsPar[i * 5]);
        }
    }
    sParDlRunner.store(0, std::memory_order_relaxed);
}
// (GXV_PAR_DL=2) the distinct materials of the listed entries' model data, in list order
void ParDlSnapAll(ParDl& d, PortHostVector< ParDlMatSnap >& out, bool first) {
    out.clear();
    PortHostVector< J3DModelData* > seen;
    for (size_t i = 0; i < d.work.size(); ++i) {
        if (first) d.firstOfData[i] = 0;
        if (!d.valid[i]) continue;
        J3DModel* model = d.work[i].model->getJ3DModel();
        J3DModelData* data = model != nullptr ? model->getModelData() : nullptr;
        if (data == nullptr) continue;
        bool dup = false;
        for (size_t k = 0; k < seen.size() && !dup; ++k) dup = seen[k] == data;
        if (dup) continue;
        seen.push_back(data);
        if (first) d.firstOfData[i] = 1;
        for (u16 m = 0; m < data->getMaterialNum(); ++m) {
            out.push_back(ParDlMatSnap());
            ParDlSnap(out.back(), data->getMaterialNodePointer(m));
        }
    }
}
// (GXV_PAR_DL=2) after the join: the serial flush again from the saved state, compared with the overlapped one; the game keeps the serial.
void ParDlVerify(ParDl& d, unsigned* checked, unsigned* differed, unsigned* differedFirst, unsigned* sysDiffer, unsigned* matDiffer) {
    ParDlSnapAll(d, d.snap1, false);
    u8 overlapped[sizeof(J3DSys)];
    std::memcpy(overlapped, &j3dSys, sizeof(J3DSys));
    for (size_t k = 0; k < d.snap0.size(); ++k) ParDlRestore(d.snap0[k]);
    std::memcpy(&j3dSys, d.pre, sizeof(J3DSys));
    J3DTexCoordScaleInfo scaleOverlapped[8];  // (defaults48: see ParDl::scale0)
    std::memcpy(scaleOverlapped, J3DSys::sTexCoordScaleTable, sizeof(scaleOverlapped));
    std::memcpy(J3DSys::sTexCoordScaleTable, d.scale0, sizeof(d.scale0));
    for (size_t i = 0; i < d.work.size(); ++i) {
        if (!d.valid[i]) continue;
        d.work[i].model->mStaleDL = d.stale0[i] != 0;
        d.work[i].model->mDlStatic = d.static0[i];
        d.work[i].model->mDlTmHash = d.tm0[i];
    }
    // (fedl25) The serial redo is exact only for a model data the game has refreshed before: the first refresh of a model data builds state that
    // ParDlRestore does not take back (the 6 entries of the 12 Nov console run that differed, of 283,174, were all in the first drawn turn of their
    // model data: the first Honeyhive turn, then the first turn the Low bees came: and every entry of one data shows the same two hashes, which a race
    // would not). Entries of a model data not seen before this turn are compared and counted apart ("first use"): they cannot be part of "differed",
    // and the line names which part of the fingerprint differs; a data seen in an earlier turn that differs is a real difference.
    static PortHostVector< const J3DModelData* > sSeen;
    static unsigned sFirstUseDiffers = 0, sFirstUseChecked = 0;
    for (size_t i = 0; i < d.work.size(); ++i) {
        d.firstUse[i] = 0;
        if (!d.valid[i]) continue;
        J3DModel* model = d.work[i].model->getJ3DModel();
        const J3DModelData* data = model != nullptr ? model->getModelData() : nullptr;
        bool seen = data == nullptr;
        for (size_t k = 0; k < sSeen.size() && !seen; ++k) seen = sSeen[k] == data;
        d.firstUse[i] = seen ? 0 : 1;
    }
    DlCounts c;
    for (size_t i = 0; i < d.work.size(); ++i) {
        if (!d.valid[i]) continue;
        PortDlRefreshOne(d.work[i], d.staticMode, d.lazyKey, c);
        d.fpSer[i] = ParDlFingerprint(d.work[i].model);
        ++*checked;
        if (d.firstUse[i]) ++sFirstUseChecked;
        if (d.fpSer[i] != d.fpPar[i]) {
            u32 parts[5];
            ParDlFingerprintParts(d.work[i].model, parts);
            if (d.firstUse[i]) {
                ++sFirstUseDiffers;
                --*checked;  // (not compared: counted in "first use" below)
            } else {
                ++*differed;
                if (d.firstOfData[i]) ++*differedFirst;
            }
            static unsigned sLogged = 0;
            if (sLogged < 16) {
                ++sLogged;
                const u32* pp = &d.partsPar[i * 5];
                port_log_summary("par-dl: verify %s entry %u \"%s\"%s: overlapped %08x, serial %08x; parts (current-matrix words, texture matrices, "
                                 "list sizes, list bytes, other buffer's bytes) overlapped %08x %08x %08x %08x %08x, serial %08x %08x %08x %08x %08x; "
                                 "first-use entries so far: %u checked, %u differ",
                                 d.firstUse[i] ? "first-use difference (the serial redo is not exact for a model data's first refresh; not counted)" : "MISMATCH",
                                 static_cast< unsigned >(i), d.work[i].owner->getName(), d.firstOfData[i] ? " (first of its model data)" : "", d.fpPar[i], d.fpSer[i],
                                 pp[0], pp[1], pp[2], pp[3], pp[4], parts[0], parts[1], parts[2], parts[3], parts[4], sFirstUseChecked, sFirstUseDiffers);
            }
        }
    }
    for (size_t i = 0; i < d.work.size(); ++i) {
        if (!d.valid[i]) continue;
        J3DModel* model = d.work[i].model->getJ3DModel();
        const J3DModelData* data = model != nullptr ? model->getModelData() : nullptr;
        bool seen = data == nullptr;
        for (size_t k = 0; k < sSeen.size() && !seen; ++k) seen = sSeen[k] == data;
        if (!seen) sSeen.push_back(data);
    }
    // j3dSys: the serial flush's words, then the view pass's (it came later), against the merged one
    u8 expected[sizeof(J3DSys)];
    std::memcpy(expected, &j3dSys, sizeof(J3DSys));
    for (size_t b = 0; b < sizeof(J3DSys); ++b) {
        if (d.gameSys[b] != d.pre[b]) expected[b] = d.gameSys[b];
    }
    for (size_t w = 0; w + 4 <= sizeof(J3DSys); w += 4) {
        if (std::memcmp(expected + w, overlapped + w, 4) != 0) ++*sysDiffer;
    }
    std::memcpy(&j3dSys, expected, sizeof(J3DSys));  // (the serial result)
    // (defaults48) the texture-size table both passes leave must be the same too (counted with the j3dSys words: it is J3DSys's)
    for (size_t w = 0; w < 8; ++w) {
        if (std::memcmp(&scaleOverlapped[w], &J3DSys::sTexCoordScaleTable[w], sizeof(J3DTexCoordScaleInfo)) != 0) ++*sysDiffer;
    }
    ParDlSnapAll(d, d.snap2, false);
    for (size_t k = 0; k < d.snap1.size() && k < d.snap2.size(); ++k) {
        if (!ParDlSnapSame(d.snap1[k], d.snap2[k])) ++*matDiffer;
    }
}
}  // namespace

// The per-picture record port_par_dl_note (perf.cpp) sums: 0 overlapped, 1 serial (no view pass to overlap), 2 entries, 3 the game thread
// took the job (the worker had not started), 4 finished early (port_par_dl_game_enter), 5 the game thread's wait at the join (us), 6 the
// merge and verify (us), 7 the game thread's section from the dispatch to the end of the join (us; the view pass included), 8 lists
// updated, 9 the listing and dispatch (us), 10 j3dSys words taken from the worker, 11 conflicts, 12 verify: entries checked, 13 differed,
// 14 of them the first entry of their model data, 15 j3dSys words differed, 16 material states differed, 17 view jobs the game thread ran
// while it waited (GXV_PAR_DL_HELP), 18 serial: no view pass pending, 19 serial: the player's included and not split, 20 overlapped with the
// player's part split off to after the join (round 2).
enum { kParDlRecord = 21 };
namespace {
unsigned sParDlDispatchUs = 0;
bool sParDlEarly = false;
}  // namespace

// GameSystem::frameLoop, in place of port_dl_flush_stale(0) on a drawn turn: 1 = the material lists are the worker's now (the caller makes
// the view pass and then calls port_par_dl_end); 0 = nothing done (the caller flushes serially).
extern "C" int port_par_dl_begin(void) {
    const int mode = port_par_dl_mode();
    if (mode == 0 || sStaleDL.empty() || sParDl.open || !port_on_frame_thread()) {
        return 0;
    }
    // The view pass the material lists overlap: pending "except the player" (made in calc-anim), or pending with the player's included
    // (draw-done modes 3 / 4 with the front end behind at calc-anim: the default Settings "Logic overlap: Max" at 60 pictures a second),
    // which is then split: the rest overlapped, the player's part after the join (round 2: GameScene.cpp port_view_split_player_last).
    const int viewState = port_view_pending_state();
    const bool playerLast = viewState == 3 && port_par_dl_player_on();
    if (viewState != 2 && !playerLast) {
        unsigned rec[kParDlRecord] = {};
        rec[1] = 1;  // (no view pass to overlap: serial)
        rec[18 + (viewState == 0 ? 0 : 1)] = 1;  // (why: none pending / the player's included and not split)
        port_par_dl_note(rec, kParDlRecord);
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            port_log_summary("par-dl: a drawn turn with nothing to overlap: %s (draw-done mode %d, GXV_PAR_DL_VIEW %d, GXV_PAR_DL_PLAYER %d)",
                             viewState == 0 ? "no view pass pending at the flush" : "the pending pass includes the player's and is not split (PETARI_VIEW_PLAYER=0 or GXV_PAR_DL_PLAYER=0)",
                             port_gx_drawdone_defer_mode(), port_par_dl_defer_view(), port_par_dl_player_on());
        }
        return 0;
    }
    if (!port_par_dl_start()) {
        return 0;
    }
    ParDl& d = sParDl;
    const int64_t t0 = port_host_time_ns();
    port_live_actor_snapshot();
    d.work.clear();
    d.work.swap(sStaleDL);  // (as port_dl_flush_stale: anything listed meanwhile goes into a new list)
    const size_t n = d.work.size();
    d.valid.resize(n);
    d.c = DlCounts();
    for (size_t i = 0; i < n; ++i) {
        const StaleDL& e = d.work[i];
        d.valid[i] = port_live_actor_valid(e.owner) && e.owner->mModelManager == e.model ? 1 : 0;
        if (!d.valid[i]) {
            ++d.c.notModel;
        }
    }
    d.staticMode = port_dl_static_on();
    d.lazyKey = PortDlLazyKeyOn();
    d.prefetch = port_flush_prefetch_on() != 0;
    d.verify = mode == 2;
    if (d.verify) {
        d.fpPar.assign(n, 0);
        d.fpSer.assign(n, 0);
        d.partsPar.assign(n * 5, 0);
        d.firstUse.assign(n, 0);
        d.stale0.resize(n);
        d.static0.resize(n);
        d.tm0.resize(n);
        d.firstOfData.resize(n);
        for (size_t i = 0; i < n; ++i) {
            d.stale0[i] = d.valid[i] && d.work[i].model->mStaleDL ? 1 : 0;
            d.static0[i] = d.valid[i] ? d.work[i].model->mDlStatic : 0;
            d.tm0[i] = d.valid[i] ? d.work[i].model->mDlTmHash : 0;
        }
        ParDlSnapAll(d, d.snap0, true);
        std::memcpy(d.scale0, J3DSys::sTexCoordScaleTable, sizeof(d.scale0));  // (defaults48: the redo starts from it too)
    }
    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);  // (the material path's mutex, as updateDL takes it; held for the worker)
    d.ints = OSDisableInterrupts();               // (no other guest thread runs until the join)
    std::memcpy(d.pre, &j3dSys, sizeof(J3DSys));  // (j3dSys as the serial material lists would find it)
    std::memcpy(d.sys, &j3dSys, sizeof(J3DSys));
    d.playerLast = playerLast && port_view_split_player_last() != 0;  // (round 2: the player's part after the join, GameSystem::frameLoop)
    gPortJ3DSysMat = reinterpret_cast< J3DSys* >(d.sys);
    port_par_dl_stack(&gPortParDlStackLo, &gPortParDlStackHi);  // (J3DSys.hpp PortDlGameEnter: the worker's frames go on at once)
    gPortParDlRun = true;
    d.joined = false;
    d.open = true;
    d.t0 = t0;
    sParDlEarly = false;
    port_par_dl_dispatch(&ParDlRun, &d);
    sParDlDispatchUs = static_cast< unsigned >((port_host_time_ns() - t0) / 1000);
    return 1;
}

namespace {
// The join: every material list made (by the worker, or here if it had not started), j3dSys merged, the material path's own J3DSys and the
// overlap flag gone. Thread-agnostic: the game thread's port_par_dl_end makes it, or whichever thread reaches the material path first
// (port_par_dl_game_enter). Afterwards nothing of the job is touched by the worker.
void ParDlJoin() {
    ParDl& d = sParDl;
    if (d.joined) {
        return;
    }
    d.joined = true;
    d.waitUs = 0;
    d.helped = 0;
    d.tookJob = false;
    if (port_par_dl_take()) {
        d.tookJob = true;  // (the worker had not started: run it here, still with the private J3DSys)
        ParDlRun(&d);
        port_par_dl_reset();
    } else {
        // (GXV_PAR_DL_HELP, default 1: the wait is idle time on the game thread; it takes the view jobs the view worker, on the same
        // fourth core, has not reached, so the draw's touches find them done. Only on the game thread, the only one that draws.)
        const int64_t th = port_host_time_ns();
        if (port_par_dl_help_on() && port_on_frame_thread()) {
            while (!port_par_dl_done() && port_par_view_help_one()) {
                ++d.helped;
            }
        }
        port_par_dl_wait(&d.waitUs);
        d.waitUs = static_cast< unsigned >((port_host_time_ns() - th) / 1000);  // (the game thread's time at the join, the help included)
    }
    std::memcpy(d.gameSys, &j3dSys, sizeof(J3DSys));
    d.taken = d.conflicts = 0;
    port::par::MergeWords(&j3dSys, d.pre, d.sys, sizeof(J3DSys), &d.taken, &d.conflicts);
    gPortJ3DSysMat = &j3dSys;
    gPortParDlRun = false;
}
}  // namespace

// After the view pass (GameSystem::frameLoop), or earlier from port_par_dl_game_enter on the game thread: the join (if no one made it yet),
// then the guest's interrupts and the mutex given back, the verify, the counts. j3dSys is what the serial flush and view pass left.
extern "C" void port_par_dl_end(void) {
    ParDl& d = sParDl;
    if (!d.open) {
        return;
    }
    d.open = false;
    ParDlJoin();
    const int64_t tm = port_host_time_ns();
    unsigned rec[kParDlRecord] = {};
    rec[3] = d.tookJob ? 1 : 0;
    if (d.conflicts != 0) {
        static unsigned sLogged = 0;
        if (sLogged < 4) {
            ++sLogged;
            port_log_summary("par-dl: %u j3dSys words written by both the material lists and the view pass (the view pass's kept)", d.conflicts);
        }
    }
    OSRestoreInterrupts(d.ints);
    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
    if (d.verify) {
        ParDlVerify(d, &rec[12], &rec[13], &rec[14], &rec[15], &rec[16]);  // (in the serial flush's own surroundings)
    }
    if (port_draw_prof_on()) {
        PortDlProfCounts(d.c);
    }
    if (port_dl_static_on() != 0) {
        PortDlCensus(d.c, port_dl_static_on());  // (dead26)
    }
    rec[2] = static_cast< unsigned >(d.work.size());
    d.work.clear();
    if (sStaleDL.empty() && sStaleDL.capacity() < d.work.capacity()) {
        sStaleDL.swap(d.work);
    }
    const int64_t te = port_host_time_ns();
    rec[0] = 1;
    rec[4] = sParDlEarly ? 1 : 0;
    rec[5] = d.waitUs;
    rec[6] = static_cast< unsigned >((te - tm) / 1000);
    rec[7] = static_cast< unsigned >((te - d.t0) / 1000);
    rec[8] = d.c.updated + d.c.verified;
    rec[9] = sParDlDispatchUs;
    rec[10] = d.taken;
    rec[11] = d.conflicts;
    rec[17] = d.helped;
    rec[20] = d.playerLast ? 1 : 0;
    port_par_dl_note(rec, kParDlRecord);
}

// J3DSys.hpp PortDlGameEnter, on any thread while the overlap is open (gPortParDlRun): the job's own runner goes on; any other thread is
// about to enter the material path while the job may be running, so the job is finished (joined and merged) first. The game thread also
// ends the overlap there (port_par_dl_end); another guest thread (none is known: the game thread holds the guest's interrupts off and
// the material mutex) leaves the interrupts and the mutex to the game thread's own port_par_dl_end.
extern "C" void port_par_dl_game_enter(void) {
    if (sParDlRunner.load(std::memory_order_relaxed) == port_par_dl_self()) {
        return;  // (the worker, or the game thread running the job it took: neither reads the fields below)
    }
    ParDl& d = sParDl;
    if (!d.open || d.joined) {
        return;
    }
    sParDlEarly = true;
    static unsigned sLogged = 0;
    const bool game = port_on_frame_thread() != 0;
    if (sLogged < 8) {
        ++sLogged;
        port_log_summary("par-dl: %s reached the material path during the overlap (from %p): the material lists were finished first",
                         game ? "the game thread" : "ANOTHER THREAD", __builtin_return_address(0));
    }
    if (game) {
        port_par_dl_end();
    } else {
        ParDlJoin();
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
    PortDlNoteTarget(model);  // (GXV_DL_LAZY_KEY)
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
    u8 kind;     // 0 none, 1 NPCActor::calcJointScale, 2 KoteBug::calcJoint (GXV_PAR_ANIM_CB2, gamecpu23)
    u16 joint;
    const void* ctl;  // the JointController
    Vec scale;        // (kind 1)
    f32 quat[4];      // (kind 2) KoteBug::mPosture
    const void* host; // the delegator's host (a LiveActor)
};
// (par2) what the key reads instead of the model's state now: GXV_PAR_FLUSH / GXV_PAR_LATE's scale and base matrix as the computation
// will have them; cbMode: a recognised joint callback is let through (registered now: frames == null; or cb: as predicted).
struct ParOverride {
    const Vec* scale;
    const f32 (*base)[4];
    bool cbMode;
    const ParCb* cb;  // (prediction; null with frames == null: read from the joints now)
    ParCb* cbOut;     // (the callback found, when cbMode)
    bool noScan;      // (GXV_PAR_PLANT: the worker's prediction for a plant: the joint scan of the SHARED model data is left out (another
                      // plant's computation on the game thread writes its joint 0's calc pointer); the real key at the take scans as always)
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
    for (u16 i = 0; i < jointNum && !(ov != nullptr && ov->noScan); i++) {
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
        key[w++] = core->_C | (static_cast< u32 >(core->mPortCatchUps) << 16);  // (blendfix31: a catch-up rewrote the core's joint state since the key was taken)
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
        if (cbFound.kind == 2) {
            // (GXV_PAR_ANIM_CB2: KoteBug::calcJoint's one input, the posture)
            KeyF(key, w, cbFound.quat[0]);
            KeyF(key, w, cbFound.quat[1]);
            KeyF(key, w, cbFound.quat[2]);
            KeyF(key, w, cbFound.quat[3]);
        } else {
            KeyF(key, w, cbFound.scale.x);
            KeyF(key, w, cbFound.scale.y);
            KeyF(key, w, cbFound.scale.z);
        }
    }
    return w;  // (at most 2 + 1 + 3 + 12 + 1 + 4 + 3 * 8 + 1 + 6 = 54 < kMemoKeyMax)
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
        if (::port::gt71::AnimProfOn()) {
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
        if (port_draw_prof_on()) {
            port_draw_prof_count(7, sParViewA.Count());  // (GXV_DRAW_PROF=2: the view jobs of the drawn turn's pass)
        }
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
// GXV_PAR_DL (gamedraw23): the game thread waits at the material lists' join; meanwhile it runs one view job the view worker has not
// started (from the end). 0: none left (or no batch outstanding). The view worker shares the fourth core with the material-list worker.
extern "C" int port_par_view_help_one(void) {
    return gPortParViewAsync && sParViewA.HelpOne() ? 1 : 0;
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
#include "port/par_plant.h"  // (GXV_PAR_PLANT: a plant job's phase)
#include "port/view_cone.h"  // (GXV_PAR_PLANT: the plants the flush leaves out as off-screen get no job)
#include "Game/NPC/NPCActor.hpp"                // (GXV_PAR_ANIM_CB: NPCActor::calcJointScale)
#include "Game/Enemy/KoteBug.hpp"               // (GXV_PAR_ANIM_CB2, gamecpu23: KoteBug::calcJoint)
#include "Game/Enemy/AnimStampController.hpp"
#include "Game/Util/ActorMovementUtil.hpp"
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
extern "C" int port_par_anim_why_on(void);     // (GXV_PAR_ANIM_WHY, game24: perf.cpp)
extern "C" int port_par_anim_blend_on(void);   // (GXV_PAR_ANIM_BLEND, par2: perf.cpp)
extern "C" int port_par_anim_cb_on(void);      // (GXV_PAR_ANIM_CB, par2: perf.cpp)
extern "C" int port_par_late_on(void);         // (GXV_PAR_LATE, par2: perf.cpp; 2 = heavy models only, gamecpu23)
extern "C" unsigned port_par_late_min(void);   // (GXV_PAR_LATE=2: joints + envelopes a model needs, perf.cpp)
extern "C" int port_par_anim_cb2_on(void);     // (GXV_PAR_ANIM_CB2, gamecpu23: perf.cpp)
bool port_predict_default_base(const LiveActor* a, MtxPtr out);  // (LiveActor.cpp: LiveActor::calcAndSetBaseMtx's matrix, not set)
extern "C" int port_par_flush_on(void);        // (GXV_PAR_FLUSH, par2: perf.cpp)
extern "C" int port_par_plant_on(void);        // (GXV_PAR_PLANT, revisit23: perf.cpp)
extern "C" int port_par_plant_early_mode(void);  // (GXV_PAR_PLANT_EARLY, parplant23: perf.cpp)
extern "C" int port_par_plant_take_on(void);     // (GXV_PAR_PLANT_TAKE, parplant23: perf.cpp)
extern "C" int port_turn_skipped_raw(void);      // (perf.cpp: this turn makes no picture)
bool port_flush_plant_cone(port::viewcone::Cone* cone);           // (LiveActor.cpp: GXV_FLUSH_OFFSCREEN's view pyramid of this picture)
bool port_flush_plant_off(const port::viewcone::Cone& cone, LiveActor* a);  // (LiveActor.cpp: the flush will leave it out as off-screen)
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
    u8 plant;  // (GXV_PAR_PLANT) made by the worker from plantActor's live state (port/par_plant.h); built: it was (key and inputs valid)
    u8 built;
    u8 refuse;  // (GXV_PAR_PLANT, parplant23) why the worker made no job of it: kPlRefuse* (0 made, or not tried)
    LiveActor* plantActor;
    u32 key[kParKeyMax];
};
// (parplant23) the worker's refusal codes in ParAnimJob::refuse
enum { kPlRefuseNone, kPlRefuseKey, kPlRefuseMemo, kPlRefuseQualify, kPlRefuseArena, kPlRefuseOther, kPlRefuseSoft };
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
// (GXV_PAR_ANIM_CB2, gamecpu23) the vtable of JointControlDelegator< KoteBug >.
const void* KoteDelegatorVtable() {
    alignas(JointControlDelegator< KoteBug >) static unsigned char sProbe[sizeof(JointControlDelegator< KoteBug >)];
    static const void* const vt = *reinterpret_cast< const void* const* >(new (sProbe) JointControlDelegator< KoteBug >(static_cast< KoteBug* >(nullptr),
                                                       static_cast< JointControlDelegator< KoteBug >::func >(nullptr),
                                                       static_cast< JointControlDelegator< KoteBug >::func >(nullptr)));
    return vt;
}
// (GXV_PAR_ANIM_CB2) ctl is KoteBug's posture delegator for this model (MR::createJointDelegatorWithNullChildFunc(this, &KoteBug::calcJoint,
// "Center")): kind 2, its input the host's posture now.
bool ParRecogniseKoteCb(J3DModel* model, J3DJoint* joint, const void* ctl, ParCb* out) {
    if (!port_par_anim_cb2_on() || *reinterpret_cast< const void* const* >(ctl) != KoteDelegatorVtable()) {
        return false;
    }
    const JointControlDelegator< KoteBug >* d = static_cast< const JointControlDelegator< KoteBug >* >(static_cast< const JointController* >(ctl));
    if (d->mMtxCalcFunc != &KoteBug::calcJoint || d->mMtxCalcAfterChildFunc != nullptr || d->mModel != model || d->mHost == nullptr ||
        (joint != nullptr && d->mJoint != joint)) {
        return false;
    }
    out->kind = 2;
    out->ctl = ctl;
    out->host = static_cast< const LiveActor* >(d->mHost);
    out->quat[0] = d->mHost->mPosture.x;
    out->quat[1] = d->mHost->mPosture.y;
    out->quat[2] = d->mHost->mPosture.z;
    out->quat[3] = d->mHost->mPosture.w;
    return true;
}
bool ParRecogniseCb(J3DModel* model, J3DJoint* joint, const void* ctl, ParCb* out) {
    if (!port_par_anim_cb_on() || ctl == nullptr || (joint != nullptr && (joint->getCallBack() != &JointController::staticCallBack || joint->mCallBackUserData != ctl))) {
        return false;
    }
    if (*reinterpret_cast< const void* const* >(ctl) != NpcDelegatorVtable()) {
        return ParRecogniseKoteCb(model, joint, ctl, out);
    }
    const JointControlDelegator< NPCActor >* d = static_cast< const JointControlDelegator< NPCActor >* >(static_cast< const JointController* >(ctl));
    if (d->mMtxCalcFunc != &NPCActor::calcJointScale || d->mMtxCalcAfterChildFunc != nullptr || d->mModel != model || d->mHost == nullptr ||
        d->mHost->mScaleController == nullptr || (joint != nullptr && d->mJoint != joint)) {
        return false;
    }
    out->kind = 1;
    out->ctl = ctl;
    out->host = static_cast< const LiveActor* >(d->mHost);
    out->scale = d->mHost->mScaleController->_C;
    return true;
}
// GXV_PAR_ANIM's own conditions beyond MemoKey's (which the caller has checked): the job's Input, or false.
// (GXV_PAR_ANIM_BLEND) blend: pred holds the core's _1C, _20, _29 at the computation (MemoKey has let a blend state through only then);
// *needPrev: the walk reads the core's joint state (a blend state, track 0 without a resource, a total weight of 0): the caller gives
// the job Input::prev. scale: the base scale the computation will have (null: the model's now).
// (GXV_PAR_PLANT) workerSide: made on the flush worker: no global is read (a Softimage init, whose walk reads J3DSys::mParentS, which the
// game thread's computations write, does not qualify there).
bool ParQualify(ParAnimRec* rec, J3DModel* model, XanimeCore* core, const f32* frames, xspec::Input& in, J3DMtxCalc** basicOut,
                const KeyBlend* pred = nullptr, bool* needPrev = nullptr, const Vec* scale = nullptr, bool workerSide = false) {
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
    if (kind < 0 || (workerSide && kind == xspec::kInitSoftimage)) {
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
    if (workerSide) {
        in.parentS.x = in.parentS.y = in.parentS.z = 0.0f;  // (not read by the walk of the other inits: they set it)
    } else {
        in.parentS = J3DSys::mParentS;
    }
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
// (GXV_PAR_PLANT) workerSide: made by the flush worker from the plant's live state (port_par_plant_own keeps the game thread off it):
// no joint scan of the shared model data, no callback, no global (ParQualify); the take at the real computation checks as always.
int ParBuild(ParAnimRec* rec, ParAnimJob& job, ParArena& arena, LiveActor* flushActor = nullptr, bool late = false, const f32 (*base)[4] = nullptr,
             const Vec* scaleOverride = nullptr, bool workerSide = false) {
    ModelManager* mm = rec->mm;
    if (workerSide) {
        job.refuse = kPlRefuseOther;  // (parplant23: the reason, for the par-plant line; kPlRefuseNone once made)
    }
    if (workerSide && (rec->cbKind != 0 || late || base != nullptr || scaleOverride != nullptr)) {
        return 0;
    }
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
    const Vec* scale = scaleOverride != nullptr ? scaleOverride : flushActor != nullptr ? static_cast< const Vec* >(&flushActor->mScale) : nullptr;
    // (GXV_PAR_ANIM_CB: the callback the record's last computation had, if its controller is still the same kind for this model)
    ParCb cb;
    cb.kind = 0;
    const bool cbMode = port_par_anim_cb_on() != 0;
    if (cbMode && rec->cbKind != 0 && rec->cbJoint < model->getModelData()->getJointNum() &&
        ParRecogniseCb(model, nullptr, rec->cbCtl, &cb) &&
        static_cast< const JointController* >(rec->cbCtl)->mJoint == model->getModelData()->getJointNodePointer(rec->cbJoint) &&
        static_cast< const LiveActor* >(cb.host) == rec->owner) {
        cb.joint = rec->cbJoint;
    } else {
        cb.kind = 0;
    }
    ParOverride ov = {scale, base, cbMode, &cb, nullptr, workerSide};
    u32 words = MemoKey(mm, model, job.key, frames, blend, blend ? &pb : nullptr, &ov);
    if (words == 0) {
        if (workerSide) {
            job.refuse = kPlRefuseKey;
        }
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
            if (workerSide) {
                job.refuse = kPlRefuseMemo;
            }
            return 2;
        }
    }
    bool needPrev = false;
    if (!ParQualify(rec, model, player->mCore, frames, job.in, &job.basic, blend ? &pb : nullptr, &needPrev, scale, workerSide)) {
        if (workerSide) {
            job.refuse = port_j3d_basic_calc_kind(model->getModelData()->getJointTree().getBasicMtxCalc()) == xspec::kInitSoftimage
                             ? kPlRefuseSoft
                             : kPlRefuseQualify;
        }
        return 0;
    }
    if (base != nullptr) {
        PSMTXCopy(base, job.in.base);
    }
    if (cb.kind != 0) {
        job.in.cbKind = cb.kind;
        job.in.cbJoint = cb.joint;
        job.in.cbScale = cb.scale;
        memcpy(job.in.cbQuat, cb.quat, sizeof(job.in.cbQuat));  // (kind 2, GXV_PAR_ANIM_CB2)
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
        if (workerSide) {
            job.refuse = kPlRefuseArena;
        }
        return 0;
    }
    if (needPrev) {
        xspec::TakePrev(player->mCore, n, prev);  // (compared again at the real computation: xspec::SamePrev)
        job.in.prev = prev;
    }
    if (!workerSide) {
        job.rec = rec;  // (GXV_PAR_PLANT: the prepare set it, and the game thread may read it while the worker is here)
    }
    job.keyWords = words;
    job.envNum = e;
    job.us = 0;
    job.ok = 0;
    if (workerSide) {
        job.refuse = kPlRefuseNone;
    }
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
       kFlVerifyBad, kFlDeferred,
       // (GXV_PAR_PLANT) plant jobs listed, built by the worker, reached first by the game thread (never built), waits for a build in progress,
       // their time (us), plants left out of the batch as off-screen (GXV_FLUSH_OFFSCREEN leaves them out of the flush)
       kFlPlant, kFlPlBuilt, kFlPlGameFirst, kFlPlWaits, kFlPlWaitUs, kFlPlOff,
       // (parplant23) plants the listing saw (computed by the flush); plants not listed: covered by par-anim's batch, quiet; deferred
       // models with no record (the record store full); the worker's refusals: the memo keeps it, it does not qualify (key / walk), a
       // Softimage init (J3DSys::mParentS is not the worker's to read), the scratch store full; early batches (GXV_PAR_PLANT_EARLY): made,
       // ended unused before an update(), jobs built in those; timing: plant batches, the worker's first plant job (us after the listing,
       // batches with one), the game thread's first plant (us after the listing, batches with one)
       kFlPlSeen, kFlPlCovered, kFlPlQuiet, kFlNoRec, kFlPlMemo, kFlPlRefused, kFlPlSoft, kFlPlArena, kFlEarly, kFlEarlyCancel, kFlPlWasted,
       kFlPlBatches, kFlPlWorkerUs, kFlPlWorkerN, kFlPlGameUs, kFlPlGameN, kFlCount };
std::atomic< uint8_t >* sPlPhase = nullptr;  // (GXV_PAR_PLANT) a plant job's phase: port/par_plant.h
bool sPlBatch = false;                       // the open flush batch has plant jobs
u32 sPlPause = 0;                            // pictures left without plant jobs after a long wait for a build
u32 sPlWaitMax = 0;
unsigned sFlCounts[kFlCount];
// (GXV_PAR_PLANT_EARLY, parplant23; port/par_plant.h PlantTurns) the open flush batch was listed at the end of the last turn, for the
// drawn turn that follows; the counts as they were before that listing (an early batch ended unused gives its listing back)
bool sFlEarly = false;
unsigned sFlEarlySnap[kFlCount];
port::par::PlantTurns sPlTurns;
uint64_t sFlListUs = 0;      // (the listing's time: the timing fields of the par-plant line)
u32 sPlWorkerFirstUs = 0;    // (written by the worker in a batch, read by the game thread after the batch: us after the listing + 1)
bool sPlGameSeen = false;
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
    sPlPhase = static_cast< std::atomic< uint8_t >* >(malloc(sizeof(std::atomic< uint8_t >) * sFlCap));
    sFlArena.base = static_cast< u8* >(malloc(sFlArena.size));
    if (sFlJobs == nullptr || sFlItems == nullptr || sPlPhase == nullptr || sFlArena.base == nullptr) {
        free(sFlJobs);
        free(sFlItems);
        free(sPlPhase);
        sPlPhase = nullptr;
        free(sFlArena.base);
        sFlJobs = nullptr;
        port_log_summary("par-flush: no memory for GXV_PAR_FLUSH (%u jobs, %u KiB): off", sFlCap, port_par_anim_kb());
        return false;
    }
    for (u32 i = 0; i < sFlCap; ++i) {
        new (&sFlItems[i]) std::atomic< uint8_t >(port::par::kJobTaken);
        new (&sPlPhase[i]) std::atomic< uint8_t >(port::par::kPlGame);
        sFlJobs[i].plant = 0;
        sFlJobs[i].built = 0;
    }
    return true;
}
void ParFlushItem(void* ctx, unsigned i) {
    // (item i is the (n-1-i)th job: the worker starts at the end of the flush list)
    const u32 j = sFlCount - 1u - i;
    ParAnimJob& job = sFlJobs[j];
    if (job.plant) {
        // (GXV_PAR_PLANT: the job is made here, from the plant's live state, unless the game thread has reached the plant already)
        if (!port::par::PlantWorkerBegin(sPlPhase[j])) {
            return;  // (built stays 0: the game thread computes it)
        }
        if (sPlWorkerFirstUs == 0) {
            sPlWorkerFirstUs = static_cast< u32 >(port::par::NowUs() - sFlListUs) + 1u;  // (read by the game thread after the batch)
        }
        job.ok = 0;
        job.built = ParBuild(job.rec, job, sFlArena, job.plantActor, false, nullptr, nullptr, true) == 1 ? 1 : 0;
        port::par::PlantWorkerBuilt(sPlPhase[j]);  // (from here on only the job is read: the game thread may write the plant again)
        if (!job.built) {
            return;
        }
    }
    ParAnimItem(ctx, j);
}

enum TakeResult { kTakeHit, kTakeMiss, kTakeNotReady, kTakeBusy };
// A job's result at the model's real computation (par-anim's or the flush's): kTakeHit: the joints are in, as J3DModel::calc would
// leave them (or, with GXV_PAR_ANIM_VERIFY, the original ran and was compared); otherwise the caller computes as before.
TakeResult ParTakeJob(ParAnimJob& job, std::atomic< uint8_t >& item, ModelManager* mm, J3DModel* model, const u32* key, u32 words, unsigned* verified,
                      unsigned* verifyBad, const char* what, bool verify) {
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
    if (verify) {
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
        TakeResult r;
        if (job.plant) {
            // (GXV_PAR_PLANT: the flush has taken the plant's state from the worker already, port_par_plant_own; again here for safety)
            uint64_t waited = 0;
            const uint8_t ph = port::par::PlantGameOwn(sPlPhase[static_cast< u32 >(&job - sFlJobs)], &waited);
            if (ph != port::par::kPlBuilt || !job.built) {
                uint8_t v = port::par::kJobPending;
                sFlItems[job.item].compare_exchange_strong(v, port::par::kJobTaken, std::memory_order_acq_rel);
                r = kTakeNotReady;
            } else {
                r = ParTakeJob(job, sFlItems[job.item], mm, model, key, words, &sFlCounts[kFlVerified], &sFlCounts[kFlVerifyBad], "par-plant",
                               port_par_anim_verify_on() || port_par_plant_on() == 2);
            }
        } else {
            r = ParTakeJob(job, sFlItems[job.item], mm, model, key, words, &sFlCounts[kFlVerified], &sFlCounts[kFlVerifyBad], "par-flush",
                           port_par_anim_verify_on() != 0);
        }
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
    const TakeResult r = ParTakeJob(job, item, mm, model, key, words, &sParCounts[kPaVerified], &sParCounts[kPaVerifyBad], "par-anim",
                                    port_par_anim_verify_on() != 0);
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
// GXV_PAR_ANIM_WHY (game24, 10 Nov; default 0; a census, changes nothing): which models the game thread computes in full in calc-anim (neither the memo
// nor GXV_PAR_ANIM took them), why (the first test in MemoKey's / ParQualify's order that refuses), what that costs (the model's calc() timed), and
// the top actors. One line per ~5 s: "par-anim-why". The reason is found by the same tests on the refusal path, so it is an exact classification for
// the first nine kinds and the order of ParQualify's for the rest; "13" is a model both would take that was still computed here (a miss, a limit).
namespace {
enum { kPwAll, kPwNoBuf, kPwCalcCb, kPwDeform, kPwCpuSkin, kPwList, kPwBlend, kPwJointCb, kPwJointCalc, kPwQualVt, kPwTree, kPwTrack, kPwKind, kPwTaken, kPwNoAnm, kPwN };
const char* const kPwName[kPwN] = {"all", "no joint buffer/tree mismatch", "model calc callback", "deformer/skin deform/vtx colour/unk calc", "cpu skin",
                                   "transform list / mode _6 / tracks > 8", "blend (BLEND off)", "joint callback (not recognised / CB off)",
                                   "joint calc other than the core", "model vtable / flag / core fields", "tree not rooted at joint 0",
                                   "non-BCK track / weight 0", "basic calc kind", "qualified, not taken (miss / limit / switch)", "no XanimePlayer (static pose)"};
struct PwRow { char name[28]; u32 n; u64 ns; u8 why; };
struct PwState { u32 n[kPwN]; u64 ns[kPwN]; PwRow top[40]; int nTop; int64_t t0; u32 calls; };
PwState sPw;
int PwClassify(ModelManager* mm, J3DModel* model) {
    J3DModelData* data = model->getModelData();
    J3DMtxBuffer* buf = model->getMtxBuffer();
    if (data == nullptr || buf == nullptr || buf->mJointTree == nullptr) return kPwNoBuf;
    if (model->mCalcCallBack != nullptr) return kPwCalcCb;
    if (model->mDeformData != nullptr || model->mSkinDeform != nullptr || model->mVtxColorCalc != nullptr || model->mUnkCalc1 != nullptr || model->mUnkCalc2 != nullptr) return kPwDeform;
    if (model->checkFlag(J3DMdlFlag_SkinPosCpu) || model->checkFlag(J3DMdlFlag_SkinNrmCpu)) return kPwCpuSkin;
    const u16 jointNum = data->getJointNum();
    if (jointNum == 0 || buf->mJointTree->getJointNum() != jointNum || buf->mJointTree->getWEvlpMtxNum() != data->getWEvlpMtxNum() || buf->mpAnmMtx == nullptr || buf->mpScaleFlagArr == nullptr) return kPwNoBuf;
    XanimeCore* core = nullptr;
    if (mm->mXanimePlayer != nullptr) {
        core = mm->mXanimePlayer->mCore;
        if (core == nullptr || core->mTransformList != nullptr || (core->_6 != 0 && core->_6 != 3) || core->mTrackCount > 8) return kPwList;
        if ((core->_29 != 0 || core->_1C < 1.0f || core->_20 != 1.0f) && port_par_anim_blend_on() == 0) return kPwBlend;
    }
    for (u16 i = 0; i < jointNum; i++) {
        J3DJoint* joint = data->getJointNodePointer(i);
        if (joint->getCallBack() != nullptr) return kPwJointCb;
        if (joint->getMtxCalc() != (i == 0 ? static_cast< J3DMtxCalc* >(core) : nullptr)) return kPwJointCalc;
    }
    if (core == nullptr) return kPwNoAnm;
    if (*reinterpret_cast< const void* const* >(model) != PlainJ3DModelVtable() || core->mTrackCount == 0 || core->_4 > 2 || model->checkFlag(J3DMdlFlag_UseDefaultJ3D) ||
        core->mJointList == nullptr || core->mJointCount < jointNum) return kPwQualVt;
    if (!ParTreeOk(data->getJointTree())) return kPwTree;
    const void* keyVt = AnmKeyVtable();
    for (s32 i = 0; i < core->mTrackCount; i++) {
        const XanimeTrack& t = core->mTrackList[i];
        if (t._0 != nullptr && (core->mTrackCount == 1 || t.mWeight != 0.0f) && *reinterpret_cast< const void* const* >(t._0) != keyVt) return kPwTrack;
    }
    if (port_j3d_basic_calc_kind(data->getJointTree().getBasicMtxCalc()) < 0) return kPwKind;
    return kPwTaken;
}
void PwRecord(ModelManager* mm, J3DModel* model, u64 ns) {
    const int why = PwClassify(mm, model);
    ++sPw.n[kPwAll]; sPw.ns[kPwAll] += ns; ++sPw.n[why]; sPw.ns[why] += ns;
    const LiveActor* a = gPortCalcAnimOwner != nullptr && gPortCalcAnimOwner->mModelManager == mm ? gPortCalcAnimOwner : nullptr;
    const char* nm = a != nullptr ? a->getName() : "?";
    int k = 0;
    for (; k < sPw.nTop; ++k) if (sPw.top[k].why == why && strncmp(sPw.top[k].name, nm, sizeof sPw.top[k].name - 1) == 0) break;
    if (k == sPw.nTop) {
        if (sPw.nTop == 40) return;
        PwRow& r = sPw.top[sPw.nTop++];
        strncpy(r.name, nm, sizeof r.name - 1); r.name[sizeof r.name - 1] = 0; r.n = 0; r.ns = 0; r.why = static_cast< u8 >(why);
    }
    ++sPw.top[k].n; sPw.top[k].ns += ns;
    const int64_t now = port_host_time_ns();
    if (sPw.t0 == 0) sPw.t0 = now;
    if (now - sPw.t0 >= 5000000000ll) {
        const double secs = (now - sPw.t0) * 1e-9;
        char line[900]; int o = snprintf(line, sizeof line, "par-anim-why: %.1f s, game-thread full computations %.1f a second, %.3f ms of a second (timed calc(); no memo, no par-anim):", secs, sPw.n[kPwAll] / secs, sPw.ns[kPwAll] * 1e-6 / secs);
        for (int w = 1; w < kPwN; ++w) if (sPw.n[w] != 0 && o < 700) o += snprintf(line + o, sizeof line - o, " [%s: %.1f/s %.3f ms/s]", kPwName[w], sPw.n[w] / secs, sPw.ns[w] * 1e-6 / secs);
        port_log_summary("%s", line);
        for (int pass = 0; pass < 8; ++pass) {  // the 8 most expensive (actor, reason) rows
            int best = -1;
            for (int q = 0; q < sPw.nTop; ++q) if (sPw.top[q].n != 0 && (best < 0 || sPw.top[q].ns > sPw.top[best].ns)) best = q;
            if (best < 0) break;
            port_log_summary("par-anim-why:   %-26s %6.1f/s %.3f ms/s  (%s)", sPw.top[best].name, sPw.top[best].n / secs, sPw.top[best].ns * 1e-6 / secs, kPwName[sPw.top[best].why]);
            sPw.top[best].n = 0;
        }
        memset(&sPw, 0, sizeof sPw);
        sPw.t0 = now;
    }
}
}  // namespace
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
    // (GXV_PAR_PLANT_TAKE, parplant23: a plant deferred to the next picture's flush gets its job from GXV_PAR_PLANT's worker, made there
    // from its live state, instead of one made here on the game thread. 1: only when this turn's end lists the early batch (the next
    // turn is predicted to draw: else this job is kept, in case the next turn draws after all); 2: always.)
    bool plantTake = false;
    if (port_par_plant_on() != 0 && sPlPause == 0) {
        const int take = port_par_plant_take_on();
        port::par::PlantTurns next = sPlTurns;
        next.Note(port_turn_skipped_raw() == 0);
        plantTake = take >= 2 || (take == 1 && next.NextDraws(port_par_plant_early_mode()));
    }
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
        if (plantTake && a->mStaleAnim && a->mLogicFreeJoints != 0) {
            continue;  // (GXV_PAR_PLANT_TAKE: its next computation is the flush; GXV_PAR_PLANT makes that job)
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
// GXV_PAR_FLUSH (above): the flush batch, from the actors port_anim_flush_stale is about to compute, in its order; the worker takes them
// from the end. Made at the start of a drawn turn before the deferred draw-done wait (GameSystem::frameLoop, port_par_flush_prepare), or,
// with GXV_PAR_PLANT and GXV_PAR_PLANT_EARLY (parplant23), at the end of the turn before it (port_par_plant_turn_end).
// WHY EARLY (parplant23, console 472-verify, Honeyhive: plants listed 18.1 a picture, made by the worker 0.0, reached first by the game
// thread 17.7; the worker 0.002 ms a picture): listed at the drawn turn's start, the batch had no head start at all. The deferred draw-done
// wait (GXV_DRAWDONE_DEFER=2, the default) is taken at the start of calc-anim (point 2), so port_gx_drawdone_settle(3) right after the
// listing never waits, and port_anim_flush_stale begins at once; its list is in calc-anim order, and the plants defer in
// CalcAnimType_MapObj, the second calc-anim list (CutBushModelObj: connectToSceneNoSilhouettedMapObjWeakLightNoMovement), so they are the
// first entries the game thread reaches, within tens of microseconds of the semaphore signal that wakes the worker on the fourth core.
// The worker's priority (one step above the game thread's, above par-anim's) and its wake are not the issue; the window is. At the end
// of the turn before, everything the jobs read is final (no update() runs before the flush; LiveActor::calcViewAndEntry leaves stale
// actors alone), and the worker has the retrace wait and the next turn's start.
LiveActor* const* port_anim_stale_items(size_t* n);  // (LiveActor.cpp: the deferred actors, in the flush's order)
// (parplant23) The plant jobs' tally once the worker is out of the batch: built, refused and why (a plant the memo keeps, or one that
// does not qualify, is left out of the next listings for a while: its job could never be used), the worker's first job's time.
static void PlantBatchTally(bool used) {
    for (u32 j = 0; j < sFlCount; ++j) {
        ParAnimJob& job = sFlJobs[j];
        if (!job.plant) {
            continue;
        }
        if (job.built) {
            ++sFlCounts[used ? kFlPlBuilt : kFlPlWasted];
        } else if (used) {
            ParAnimRec* rec = job.rec;
            switch (job.refuse) {
            case kPlRefuseMemo:
                ++sFlCounts[kFlPlMemo];
                rec->flushQuiet = 8;  // (a still plant: the memo keeps it at the flush, no take ever reads a job)
                break;
            case kPlRefuseKey:
            case kPlRefuseQualify:
            case kPlRefuseOther:
                ++sFlCounts[kFlPlRefused];
                rec->flushQuiet = 16;
                break;
            case kPlRefuseSoft:
                ++sFlCounts[kFlPlSoft];
                rec->flushQuiet = 64;
                break;
            case kPlRefuseArena:
                ++sFlCounts[kFlPlArena];
                break;
            default:
                break;
            }
        }
        job.plant = 0;
    }
    if (used && sPlWorkerFirstUs != 0) {
        sFlCounts[kFlPlWorkerUs] += sPlWorkerFirstUs - 1u;
        ++sFlCounts[kFlPlWorkerN];
    }
    sPlWorkerFirstUs = 0;
}
// The batch (both places): the deferred actors' jobs, listed (plants) or made (GXV_PAR_FLUSH's others), handed to the worker.
static void FlushListAndRun(bool early) {
    sFlOpen = false;
    sFlEarly = false;
    sPlBatch = false;
    // (GXV_PAR_PLANT, revisit23: the plants' jobs are made by the worker; the other deferred models' as before, with GXV_PAR_FLUSH)
    const bool flushOn = port_par_flush_on() != 0;
    bool plantOn = port_par_plant_on() != 0;
    if (plantOn && sPlPause != 0) {
        --sPlPause;
        plantOn = false;
    }
    if (early && !plantOn) {
        return;  // (GXV_PAR_FLUSH alone keeps its old place)
    }
    if ((!flushOn && !plantOn) || !port_par_anim_switch() || !port_on_frame_thread() || !ParInit() || !FlushInit()) {
        return;
    }
    size_t n = 0;
    LiveActor* const* list = port_anim_stale_items(&n);
    if (n == 0) {
        return;
    }
    const uint64_t t0 = port::par::NowUs();
    port_par_flush_quiesce();  // (the last flush batch's worker is out of it: its jobs and scratch can be made again)
    if (early) {
        memcpy(sFlEarlySnap, sFlCounts, sizeof(sFlCounts));  // (given back if the batch ends unused)
    }
    ++sFlGen;
    if (sFlGen == 0) {
        sFlGen = 1;
    }
    sFlArena.used = 0;
    sFlCount = 0;
    port_live_actor_snapshot();
    port::viewcone::Cone plantCone;
    const bool plantCull = plantOn && port_flush_plant_cone(&plantCone);
    for (size_t i = 0; i < n && sFlCount < sFlCap; ++i) {
        LiveActor* a = list[i];
        // (port_anim_flush_stale's own tests: the actors it computes)
        if (!port_live_actor_valid(a) || !a->mStaleAnim || a->mFlag.mIsDead || a->mFlag.mIsClipped || a->mFlag.mIsHiddenModel ||
            a->mFlag.mIsNoCalcAnim || a->mModelManager == nullptr || a->mModelManager->mXanimePlayer == nullptr) {
            continue;
        }
        ++sFlCounts[kFlDeferred];
        const bool isPlant = plantOn && a->mLogicFreeJoints != 0;
        if (isPlant) {
            ++sFlCounts[kFlPlSeen];
        } else if (!flushOn) {
            continue;  // (parplant23: GXV_PAR_PLANT alone: no record is made for a model that gets no job; the record store is not filled)
        }
        ModelManager* mm = a->mModelManager;
        ParAnimRec* rec = ParRecord(mm, true);
        if (rec == nullptr) {
            ++sFlCounts[kFlNoRec];  // (the record store is full: this one is computed as before; the others still get their jobs)
            continue;
        }
        rec->owner = a;
        if (rec->flushQuiet != 0) {
            --rec->flushQuiet;
            ++sFlCounts[isPlant ? kFlPlQuiet : kFlQuiet];
            continue;
        }
        if (rec->jobBatch == sParBatch && rec->job >= 0 && static_cast< u32 >(rec->job) < sParJobCount && sParJobs[rec->job].rec == rec) {
            ++sFlCounts[isPlant ? kFlPlCovered : kFlCovered];  // (par-anim's batch has a job for it already)
            continue;
        }
        ParAnimJob& job = sFlJobs[sFlCount];
        if (isPlant) {
            // GXV_PAR_PLANT: listed only; the worker makes the job from the plant's live state (ParFlushItem), the game thread keeps off that
            // state until the job is made (port_par_plant_own). A plant with a joint callback or a tree the walk cannot take, or one the
            // flush will leave out as off-screen, gets none.
            if (rec->cbKind != 0 || rec->treeOk == 2) {
                ++sFlCounts[kFlUnqual];
                continue;
            }
            if (plantCull && port_flush_plant_off(plantCone, a)) {
                ++sFlCounts[kFlPlOff];
                continue;
            }
            job.rec = rec;
            job.plant = 1;
            job.built = 0;
            job.refuse = kPlRefuseNone;
            job.ok = 0;
            job.plantActor = a;
            sPlPhase[sFlCount].store(port::par::kPlFree, std::memory_order_relaxed);
            rec->flushGen = sFlGen;
            rec->flushJob = static_cast< s32 >(sFlCount);
            ++sFlCount;
            ++sFlCounts[kFlPlant];
            sPlBatch = true;
            continue;
        }
        if (!flushOn) {
            continue;
        }
        job.plant = 0;
        job.built = 1;
        sPlPhase[sFlCount].store(port::par::kPlGame, std::memory_order_relaxed);
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
        sFlEarly = early;
        if (early) {
            ++sFlCounts[kFlEarly];
        }
        if (sPlBatch) {
            ++sFlCounts[kFlPlBatches];
        }
        sPlWorkerFirstUs = 0;
        sPlGameSeen = false;
        sFlListUs = port::par::NowUs();  // (published to the worker by the dispatch's release store)
        if (!port_par_flush_run(&ParFlushItem, sFlJobs, sFlItems, sFlCount)) {
            // (no worker: the jobs stay pending and the game thread takes each one: the flush as before, plus the jobs' making)
        }
    }
}
// The start of a drawn turn (GameSystem::frameLoop, before the deferred draw-done wait): the batch, unless the end of the last turn
// listed it already (GXV_PAR_PLANT_EARLY; nothing has run since but the frame's start).
extern "C" void port_par_flush_prepare(void) {
    if (sFlOpen && sFlEarly && !sFlLate) {
        return;
    }
    FlushListAndRun(false);
}
// (GXV_PAR_PLANT_EARLY, parplant23) An open flush batch when the game's state is about to change (the start of GameSystem::update, the
// start of calc-anim's GXV_PAR_LATE batch, the settings menu): only an early batch whose next turn did not draw (a skipped turn, the
// fast-forward of a cutscene skip) is still open there. The worker may be making a plant's job from its live state: wait for the job it
// is in (at most one), and give the batch's counts back (its work was for nothing; counted as ended unused).
extern "C" void port_par_plant_close_early(void) {
    if (sFlJobs == nullptr || !sFlOpen || sFlLate) {
        return;
    }
    port_par_flush_quiesce();
    const bool early = sFlEarly;
    sFlOpen = false;
    sFlEarly = false;
    if (early) {
        unsigned wasted = 0;
        for (u32 j = 0; j < sFlCount; ++j) {
            wasted += sFlJobs[j].plant && sFlJobs[j].built ? 1u : 0u;
        }
        memcpy(sFlCounts, sFlEarlySnap, sizeof(sFlCounts));
        ++sFlCounts[kFlEarlyCancel];
        sFlCounts[kFlPlWasted] += wasted;
    }
    for (u32 j = 0; j < sFlCount; ++j) {
        sFlJobs[j].plant = 0;
    }
    sPlWorkerFirstUs = 0;
    sPlBatch = false;
}
// (GXV_PAR_PLANT_EARLY) The end of every turn (GameSystem::frameLoop, before the retrace wait; drawn: the turn made a picture): if the
// next turn is predicted to draw (port/par_plant.h PlantTurns), its flush batch is listed now.
extern "C" void port_par_plant_turn_end(int drawn) {
    sPlTurns.Note(drawn != 0);
    if (sFlOpen) {
        return;  // (a batch still open here: none is; the flush or the update's start ended it)
    }
    const int mode = port_par_plant_on() != 0 ? port_par_plant_early_mode() : 0;
    if (!sPlTurns.NextDraws(mode)) {
        return;
    }
    FlushListAndRun(true);
}
// After port_anim_flush_stale: no flush job is read any more; the batch ends (the worker stops after the job it may be in; the next
// prepare, or the scene's drop, waits for it).
extern "C" void port_par_flush_end(void) {
    if (sFlJobs == nullptr) {
        return;
    }
    if (sFlOpen) {
        sFlOpen = false;
        sFlEarly = false;
        if (sPlBatch) {
            // (GXV_PAR_PLANT: the worker may be making a job of a plant from its live state; the next turn's update() writes that state:
            // wait for the job it is in, at most one, before the game goes on)
            port_par_flush_quiesce();
            PlantBatchTally(true);
            sPlBatch = false;
        } else {
            (void)port_par_flush_try_quiesce();
        }
    }
    port_par_flush_note(sFlCounts, kFlCount, sFlLate ? 1 : 0);
    memset(sFlCounts, 0, sizeof(sFlCounts));
    sFlLate = false;
}
extern "C" unsigned port_par_plant_wait_max(void) {
    const unsigned m = sPlWaitMax;
    sPlWaitMax = 0;
    return m;
}
// GXV_PAR_PLANT (revisit23; port/par_plant.h): port_anim_flush_stale calls this for each listed actor BEFORE it writes anything of it
// (its latch, its model's player): a plant whose job the worker has not begun is the game thread's from now on (the worker skips it);
// one the worker is making is waited for (it copies a few words; the two threads meet once a flush). A wait longer than
// GXV_PAR_PLANT_PAUSE_US (default 2000: the worker was put off its core) leaves the plant jobs out for the next 120 pictures.
extern "C" void port_par_plant_own(LiveActor* a) {
    if (!sPlBatch || !sFlOpen) {
        return;
    }
    ModelManager* mm = a->mModelManager;
    if (mm == nullptr || mm->mParAnimGen != sParRecGen || mm->mParAnim == nullptr) {
        return;
    }
    const ParAnimRec* rec = static_cast< const ParAnimRec* >(mm->mParAnim);
    if (rec->flushGen != sFlGen || rec->flushJob < 0 || static_cast< u32 >(rec->flushJob) >= sFlCount) {
        return;
    }
    const u32 j = static_cast< u32 >(rec->flushJob);
    if (!sFlJobs[j].plant || sFlJobs[j].plantActor != a) {
        return;
    }
    if (!sPlGameSeen) {
        sPlGameSeen = true;  // (parplant23: the game thread's first plant, us after the listing)
        sFlCounts[kFlPlGameUs] += static_cast< unsigned >(port::par::NowUs() - sFlListUs);
        ++sFlCounts[kFlPlGameN];
    }
    uint64_t waited = 0;
    if (port::par::PlantGameOwn(sPlPhase[j], &waited) == port::par::kPlGame) {
        ++sFlCounts[kFlPlGameFirst];
        uint8_t v = port::par::kJobPending;
        sFlItems[sFlJobs[j].item].compare_exchange_strong(v, port::par::kJobTaken, std::memory_order_acq_rel);  // (nothing to run)
    }
    if (waited != 0) {
        ++sFlCounts[kFlPlWaits];
        sFlCounts[kFlPlWaitUs] += static_cast< unsigned >(waited);
        static const u32 pauseUs = [] {
            const char* e = getenv("GXV_PAR_PLANT_PAUSE_US");
            const long v = e != nullptr && *e != 0 ? strtol(e, nullptr, 0) : 2000;
            return static_cast< u32 >(v < 100 ? 100 : v);
        }();
        if (waited > sPlWaitMax) {
            sPlWaitMax = static_cast< u32 >(waited);
        }
        if (waited > pauseUs && sPlPause == 0) {
            sPlPause = 120;
            port_log_summary("par-plant: the game thread waited %u us for the worker's job of a plant (more than %u): plant jobs paused for 120 pictures",
                             static_cast< unsigned >(waited), static_cast< unsigned >(pauseUs));
        }
    }
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
// (GXV_PAR_ANIM_CB2, gamecpu23) KoteBug::calcAndSetBaseMtx's base matrix and base scale, as it will set them (the same functions on the
// same members; nothing written). LiveActor::calcAnmMtx sets the actor's scale first; KoteBug's own setBaseScale then replaces it.
void PortPredictKoteBugBase(const KoteBug* k, Mtx out, Vec* scale) {
    TPos3f mtx;
    MR::calcMtxFromGravityAndZAxis(&mtx, k, k->mGravity, k->mFront);
    TPos3f stampMtx(k->mStampController->_14);
    mtx.concat(stampMtx, mtx);
    mtx.setTrans(k->mPosition);
    PSMTXCopy(mtx.toMtxPtr(), out);
    TVec3f s = k->mScaleController->_C * k->mScale;
    scale->x = s.x;
    scale->y = s.y;
    scale->z = s.z;
}
extern "C" void port_par_plant_close_early(void);
extern "C" void port_par_late_prepare(void) {
    port_par_plant_close_early();  // (parplant23: never open here; the update's start ended it)
    sFlOpen = false;
    if (!port_par_late_on() || !port_par_anim_switch() || !port_on_frame_thread() || !ParInit() || !FlushInit() || sParList.empty()) {
        return;
    }
    const uint64_t t0 = port::par::NowUs();
    const int lateMode = port_par_late_on();
    const unsigned lateMin = port_par_late_min();
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
        if (lateMode == 2 && rec->cbKind != 2) {
            // (GXV_PAR_LATE=2, gamecpu23: only the heavy models, whose joints cost far more than a job's making and taking)
            J3DModelData* data = rec->mm->getJ3DModel() != nullptr ? rec->mm->getJ3DModel()->getModelData() : nullptr;
            if (data == nullptr || static_cast< unsigned >(data->getJointNum()) + data->getWEvlpMtxNum() < lateMin) {
                continue;
            }
        }
        Mtx bm;
        Vec bs;
        const f32(*base)[4] = nullptr;
        const Vec* scaleOv = nullptr;
        if (rec->cbKind == 2) {
            // (GXV_PAR_ANIM_CB2: the record's callback is KoteBug's posture delegator whose host is this actor, so the actor is a KoteBug;
            // its own calcAndSetBaseMtx's base matrix and scale, computed now from the final inputs. A wrong one only misses the key.)
            PortPredictKoteBugBase(static_cast< const KoteBug* >(a), bm, &bs);
            base = bm;
            scaleOv = &bs;
        } else if (rec->lateBase == 1 && port_predict_default_base(a, bm)) {
            base = bm;
        }
        ParAnimJob& job = sFlJobs[sFlCount];
        job.plant = 0;  // (GXV_PAR_PLANT: a job made here, on the game thread)
        job.built = 1;
        const int b = ParBuild(rec, job, sFlArena, a, true, base, scaleOv);
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

#ifdef TARGET_PC
// GXV_MATLOD (cpulod26; LiveActor.cpp): true = this turn's material-list refresh of a far model is left out (the last list stays; joints and logic untouched).
bool port_cpulod_mat_skip(LiveActor* pActor, ModelManager* pMm);
extern "C" int port_cpulod_timer_on(void);
extern "C" void port_cpulod_dl_time(int64_t ns);
#endif
void ModelManager::calcAnim() {
    calc();
#ifdef TARGET_PC
    if (gPortCalcAnimOwner != nullptr && gPortCalcAnimOwner->mModelManager == this && port_on_frame_thread() && port_cpulod_mat_skip(gPortCalcAnimOwner, this)) {
        return;  // (GXV_MATLOD: not listed and not refreshed this turn; the flush, if it still has the model listed from before, refreshes it once)
    }
    const int lazy = port_dl_lazy_on();
    if ((lazy == 2 || (lazy == 1 && port_turn_skipped_raw())) && gPortCalcAnimOwner != nullptr && gPortCalcAnimOwner->mModelManager == this &&
        port_on_frame_thread()) {
        // (listed once however many turns it skips)
        if (!mStaleDL) {
            mStaleDL = true;
            sStaleDL.push_back(StaleDL{gPortCalcAnimOwner, this});
        }
        PortDlNoteTarget(this);  // (GXV_DL_LAZY_KEY)
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
    mCpDlMade = 1;  // (GXV_MATLOD: a model is never left out before its first full refresh)
    const int64_t cpT0 = (mCpDlFar != 0 && !gPortParDlRun && port_cpulod_timer_on() != 0) ? port_host_time_ns() : 0;  // (the census times the refreshes of far models)
    PortDlGameEnter();  // (GXV_PAR_DL: on the game thread during the overlap, the worker's material lists are finished first)
    // GXV_PAR_DL: inside the overlap this runs on the worker (or on the game thread that took the job): the game thread has made the
    // draw-done wait (settle 3, before the flushes), is inside the flush scope, holds this mutex and the guest's interrupts for the whole
    // overlap. Nothing of the guest OS, the front-end waits or the game thread's workload counters is touched from here then.
    const bool parDl = gPortParDlRun;
    if (!parDl) {
    // (GXV_DRAWDONE_DEFER=2: the front end may still be reading the last picture's lists while update() runs; a model that writes
    // its display lists outside calcAnim, EarthenPipe::control for one, waits for the deferred draw-done first. No wait is pending
    // in the ordinary case, and then this returns at once.)
    port_gx_drawdone_settle(3);  // (modes 1-3: a list written outside the drawn turn's flush waits for the front end)
    port_gx_view_write_from(__builtin_return_address(0));  // (modes 3, 4: the same, outside the flush, through the one counter)
    port_drawpipe_join_before_material();  // (fx66: in the flush scope the line above does not join; with JOINLATE this does, before the material blocks / texture matrices are written)
    }
#endif
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    const u32 familyToken = parDl ? 0u : port_workload_anim_family_begin(2);
#endif
#ifdef TARGET_PC
    if (!parDl)
#endif
    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    if (!parDl) port_workload_anim_material_checkpoint(familyToken, 1);
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
    if (!parDl) port_workload_anim_material_checkpoint(familyToken, 2);
#endif
    getJ3DModel()->calcMaterial();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    if (!parDl) port_workload_anim_material_checkpoint(familyToken, 4);
#endif

    if (isUpdateDL) {
        if (isDiffDL) {
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
            if (!parDl) port_workload_anim_material_checkpoint(familyToken, 3);
#endif
            mDisplayListMaker->diff();
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
            if (!parDl) port_workload_anim_material_checkpoint(familyToken, 4);
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

#ifdef TARGET_PC
    if (!parDl)
#endif
    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
    if (!parDl) port_workload_anim_family_end(familyToken);
#endif
#ifdef TARGET_PC
    if (cpT0 != 0) {
        port_cpulod_dl_time(port_host_time_ns() - cpT0);
    }
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
#ifdef TARGET_PC
    mCalmRun = 0;  // (GXV_MATCALM: a material animation starts: refreshed every flush again)
    mCalmSkip = 0;
#endif
    stopBtk();
    mBtkPlayer->start(pBtkName);
    mDisplayListMaker->onCurFlagBtk(mBtkPlayer->mAnmRes);
}

void ModelManager::startBrk(const char* pBrkName) {
#ifdef TARGET_PC
    mCalmRun = 0;  // (GXV_MATCALM: a material animation starts: refreshed every flush again)
    mCalmSkip = 0;
#endif
    stopBrk();
    mBrkPlayer->start(pBrkName);
    mDisplayListMaker->onCurFlagBrk(mBrkPlayer->mAnmRes);
}

void ModelManager::startBtp(const char* pBtpName) {
#ifdef TARGET_PC
    mCalmRun = 0;  // (GXV_MATCALM: a material animation starts: refreshed every flush again)
    mCalmSkip = 0;
#endif
    stopBtp();
    mBtpPlayer->start(pBtpName);
    mDisplayListMaker->onCurFlagBtp(mBtpPlayer->mAnmRes);
}

void ModelManager::startBpk(const char* pBpkName) {
#ifdef TARGET_PC
    mCalmRun = 0;  // (GXV_MATCALM: a material animation starts: refreshed every flush again)
    mCalmSkip = 0;
#endif
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

#if defined(TARGET_PC) && defined(__vita__)
extern "C" int port_mutex_lock_gate_fast(OSMutex* mutex);    // (defaults48: decomp/src/RVL_SDK/os/OSMutex.c)
extern "C" int port_mutex_unlock_gate_fast(OSMutex* mutex);
#endif
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

#if defined(TARGET_PC) && defined(__vita__)
    // (defaults48, GXV_MUTEX_GATE_FAST: the lock and the unlock below were the two call sites that shut the second CPU's gate most often, OSMutex.c)
    if (!port_mutex_lock_gate_fast(&MR::MutexHolder< 0 >::sMutex)) OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
#else
    OSLockMutex(&MR::MutexHolder< 0 >::sMutex);
#endif
#ifdef TARGET_PC
    if (!PortAnimMemoCalc(this) && !PortParAnimCalc(this)) {
        if (port_par_anim_why_on() && port_on_frame_thread()) {
            const int64_t t0 = port_host_time_ns();
            getJ3DModel()->calc();
            PwRecord(this, getJ3DModel(), static_cast< u64 >(port_host_time_ns() - t0));
        } else {
            getJ3DModel()->calc();  // (GXV_ANIM_MEMO, GXV_PAR_ANIM: a model that does not qualify, as before)
        }
    }
#else
    getJ3DModel()->calc();
#endif
#if defined(TARGET_PC) && defined(__vita__)
    if (!port_mutex_unlock_gate_fast(&MR::MutexHolder< 0 >::sMutex)) OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
#else
    OSUnlockMutex(&MR::MutexHolder< 0 >::sMutex);
#endif

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
