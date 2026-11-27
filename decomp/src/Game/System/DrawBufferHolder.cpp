#include "Game/System/DrawBufferHolder.hpp"
#include "Game/System/DrawBufferGroup.hpp"
#ifdef TARGET_PC
#include "Game/Scene/SceneFunction.hpp"
#endif
#include "Game/Util/MemoryUtil.hpp"
#include <algorithm>
#ifdef TARGET_PC
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DJointTree.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include "Game/System/PortDrawPipe.hpp"
#include "port/draw_pipe.h"
extern "C" int64_t port_host_time_ns(void);
#endif

namespace {
    // static const char* sDrawTypeCameraName = ;
    // static const char* sPermitMultiCategoryArciveName = ;

    bool isExcludedCheckMultiRegistCategory(s32);
};  // namespace

DrawBufferHolder::DrawBufferHolder() : mTableInitialized() {
}

bool DrawBufferHolder::isBufferGroupEmpty(s32 drawBufferType) const {
    return mBufferGroups[drawBufferType].mActiveExecutors.size() == 0;
}

#pragma push
#pragma opt_loop_invariants off
void DrawBufferHolder::initTable(const DrawBufferInitialTable* pInitialTable, s32 numGroups) {
    mBufferGroups.init(numGroups);

    s32 cameraTypeCounts[3];
    MR::zeroMemory(cameraTypeCounts, 3 * sizeof(s32));

    const DrawBufferInitialTable* entry;
    for (s32 idx = 0; entry = &pInitialTable[idx], entry->mDrawBufferType != -1; idx++) {
        u32 drawType = entry->mDrawBufferType;
        cameraTypeCounts[entry->mDrawCameraType]++;

        mBufferGroups[drawType].init(entry->mCapacity);
        mBufferGroups[drawType].setDrawCameraType(entry->mDrawCameraType);
        mBufferGroups[drawType].setLightType(entry->mLightType);
    }

    s32* pCounts = cameraTypeCounts;

    for (s32 idx = 0; idx < 3; idx++) {
        if (pCounts[idx] > 0) {
            mExecuteLists[idx].init(pCounts[idx]);
        }
    }

    mTableInitialized = true;
}
#pragma pop

void DrawBufferHolder::allocateActorListBuffer() {
    mTableInitialized = false;
    // NOTE: this symbol does not appear in the debug map, instead it likely uses std::for_each. However the current std::for_each does not support
    // flat array iteration
    std::for_each_array(mBufferGroups.begin(), mBufferGroups.end(), std::mem_func(&DrawBufferGroup::allocateActorListBuffer));
}

s32 DrawBufferHolder::registerDrawBuffer(LiveActor* pActor, s32 drawBufferType) {
    return mBufferGroups[drawBufferType].registerDrawBuffer(pActor);
}

void DrawBufferHolder::active(LiveActor* pActor, s32 drawBufferType, s32 executorIndex) {
    bool isEmpty = isBufferGroupEmpty(drawBufferType);
    mBufferGroups[drawBufferType].active(pActor, executorIndex);

    if (isEmpty) {
        ExecutorList& executeList = getExecuteList(drawBufferType);
        executeList.mArray[executeList.mCount++] = &mBufferGroups[drawBufferType];
    }
}

void DrawBufferHolder::deactive(LiveActor* pActor, s32 drawBufferType, s32 executorIndex) {
    mBufferGroups[drawBufferType].deactive(pActor, executorIndex);

    if (isBufferGroupEmpty(drawBufferType)) {
        ExecutorList& executeList = getExecuteList(drawBufferType);
        executeList[std::find(executeList.begin(), executeList.end(), &mBufferGroups[drawBufferType]) - executeList.begin()] =
            executeList[executeList.mCount - 1];
        executeList.mCount--;
    }
}

void DrawBufferHolder::findLightInfo(LiveActor* pActor, s32 drawBufferType, s32 executorIndex) {
    mBufferGroups[drawBufferType].findLightInfo(pActor, executorIndex);
}

void DrawBufferHolder::entry(s32 drawBufferType) {
    std::for_each(mExecuteLists[drawBufferType].begin(), mExecuteLists[drawBufferType].end(), std::mem_func(&DrawBufferGroup::entry));
}

#ifdef TARGET_PC
// PETARI_VIEW_PLAYER (forkB; split from Producdevity's fork, ee487a3): the view pass of the 3D list in two parts, the player's group
// alone (MarioActor::calcViewAndEntry also carries per-turn state: decideShadowMode, the lighting, footprints) and the rest. A group
// is visited only if it is in the list, as entry() does.
void DrawBufferHolder::entryPlayer() {
    for (DrawBufferGroup** ppGroup = mExecuteLists[MR::CameraType_3D].begin(); ppGroup != mExecuteLists[MR::CameraType_3D].end(); ppGroup++) {
        if (*ppGroup == &mBufferGroups[MR::DrawBufferType_Player]) {
            (*ppGroup)->entry();
        }
    }
}

void DrawBufferHolder::entryExceptPlayer(s32 cameraType) {
    for (DrawBufferGroup** ppGroup = mExecuteLists[cameraType].begin(); ppGroup != mExecuteLists[cameraType].end(); ppGroup++) {
        if (*ppGroup != &mBufferGroups[MR::DrawBufferType_Player]) {
            (*ppGroup)->entry();
        }
    }
}
#endif

#ifdef TARGET_PC
extern "C" unsigned port_hrw_set_phase(int bufferType, int xlu);  // (GXV_HRW, hrw28: decomp/src/Game/Scene/PortHrwSet.cpp)
#endif

void DrawBufferHolder::drawOpa(s32 drawBufferType) const {
#ifdef TARGET_PC
    if (portPipeDraw(drawBufferType, false)) {  // (GXV_DRAWPIPE=1: the pass from the frame frozen at the hand-over)
        return;
    }
#endif
    if (isBufferGroupEmpty(drawBufferType)) {
        return;
    }

#ifdef TARGET_PC
    gPortHrwPhase = port_hrw_set_phase(drawBufferType, 0);  // (GXV_HRW, hrw28: the owner phase of this pass for the models of a pair's set, 0 when none is tagging)
    mBufferGroups[drawBufferType].drawOpa();
    gPortHrwPhase = 0;
#else
    mBufferGroups[drawBufferType].drawOpa();
#endif
}

void DrawBufferHolder::drawXlu(s32 drawBufferType) const {
#ifdef TARGET_PC
    if (portPipeDraw(drawBufferType, true)) {  // (GXV_DRAWPIPE=1)
        return;
    }
#endif
    if (isBufferGroupEmpty(drawBufferType)) {
        return;
    }

#ifdef TARGET_PC
    gPortHrwPhase = port_hrw_set_phase(drawBufferType, 1);
    mBufferGroups[drawBufferType].drawXlu();
    gPortHrwPhase = 0;
#else
    mBufferGroups[drawBufferType].drawXlu();
#endif
}

void DrawBufferHolder::dummy(s32 drawBufferType) {
    // TODO: This SHOULD NOT be here, this is only here because for_each and Vector<>.end are emitted in this file for DrawBufferGroups,
    // indicating some stripped function uses them. (Check Debug symbols for candidates)
    std::for_each(mExecuteLists[drawBufferType].begin(), mExecuteLists[drawBufferType].end(), std::mem_func(&DrawBufferGroup::entry));
}

ExecutorList& DrawBufferHolder::getExecuteList(s32 drawBufferType) {
    s32 listId = mBufferGroups[drawBufferType].mDrawCameraType;
    return mExecuteLists[listId];
}

DrawBufferGroup* DrawBufferHolder::getDrawBufferGroup(s32 drawBufferType) {
    return &mBufferGroups[drawBufferType];
}

#ifdef TARGET_PC
// ================================================================================================ GXV_DRAWPIPE (drawpipe24, 10 Nov 2026)
// The J3D passes' side of a pipelined draw (port/draw_pipe.h has the switch's modes; DESIGN.md of drawpipe24 the whole plan).
// A pass (drawOpa / drawXlu of one buffer type) walks: the group's active models in order; per model its light type and its
// DrawBuffer's drawers (the opaque ones, or the others); per drawer its packets in order, each with its shape packet and, for the
// first packet of a run of one light, the actor's light loader. show / hide / clipping (DrawBufferHolder::active / deactive, in
// movement) and a light sort (ActorLightCtrl::update -> DrawBuffer::resetLightSort, in movement) rewrite those lists in place, and
// ActorLightCtrl::update rewrites what a loader loads. That is all the next update writes of what a pass walks itself; what it
// reaches through the packets (the lists' bytes, the matrix halves, the shapes' and models' words) is the flushes' (kMatDL ..
// kJoints in the census, which is how a console run checks this paragraph on the real stages).
//   portPipeFreeze copies the lists and the lights into a frame; portPipeDraw draws a pass from the frame exactly as
// DrawBufferGroup::drawOpa / DrawBufferExecuter::drawOpa / DrawBuffer::drawOpa draw it from the live lists.
extern "C" int port_drawpipe_mode(void);        // (platform/src/port/perf.cpp)
extern "C" int port_light_scope_enter(int level);  // (GXV_LIGHT_DEDUP: decomp/src/Game/Map/LightFunction.cpp)
extern "C" void port_light_scope_exit(int opened);

// (dpengine) the scene state a frozen pass reads: Game/System/PortDrawPipe.hpp
const PortPipeScene* gPortPipeScene = nullptr;

namespace {
    const port::drawpipe::Frame* sPortPipeFrame;  // (the frame the passes draw from: between a hand-over and the end of its draw)
    port::drawpipe::PassStats sPortPipeNoStats;
    port::drawpipe::PassStats* sPortPipeStats = &sPortPipeNoStats;
    const PortPipeScene* sPortPipeSceneIn = nullptr;  // (the scene state that goes with the frame)

    // What a light loader loads now (ActorLightCtrl::loadLight), or nullptr when it loads nothing.
    const ActorLightInfo* portPipeLightOf(const ActorLightCtrl* pLoader) {
        if (pLoader->mAreaLightInf == nullptr) {
            return nullptr;
        }
        return pLoader->_1C != nullptr ? &pLoader->mLightInfo : pLoader->getTargetActorLight(pLoader->mAreaLightInf);
    }

    // ---- the J3D draw context (dpengine)
    // What the J3D draw path keeps between two shapes besides the GX library's state: j3dSys, and the two statics a shape reads
    // before it writes them (the vertex-format list last sent, J3DShape::sOldVcdVatCmd; whether the last shape drawn had a
    // matrix index per vertex, J3DShape::sEnvelopeFlag). The serial draw interleaves two writers of it: the game thread's draw
    // lists (the camera's view matrix into j3dSys, Mario's and other models drawn by hand) and the J3D passes. A pass drawn
    // later, on the draw thread, must find the context the serial draw would have shown it; so the passes get a context of their
    // own (sPortPipeDraw), and at each pass the words the game thread's side changed since the pass before (the snapshot the
    // pass's token carries against the one before it: port::drawpipe::ApplyChanged) are carried over into it first.
    //   In the other direction the game thread's side must go on from the context the pass LEAVES, without the pass having run:
    // a later write of its own is only seen as a change against the right value, and Mario's shapes read the two statics. So the
    // token's maker works out what the pass leaves from the frozen lists (portPipeLeave: the pass's last drawn packet; its
    // prepareDraw is run for real, which writes j3dSys and the shape exactly as the pass's last shape will; the draw mode, the two
    // matrix-array words and the two statics are set by hand) and the game thread goes on from that. The token carries both
    // snapshots. Where the pass really leaves something else it is counted (PassStats::predictMiss for the statics, sysMiss for
    // j3dSys words: the "drawpipe:" line) and put right when the draw is over, unless the game thread wrote the word since
    // (portPipeUse(nullptr): MergeChanged). The matrix-array words of a shape with several matrix groups are the known miss:
    // nothing reads them before the next shape sets them.
    // GXV_DRAWPIPE=1 runs exactly this on the game thread, pass by pass: a serial rehearsal of what mode 4 does across threads.
    struct PortPipeCtx {
        alignas(8) unsigned char sys[sizeof(J3DSys)];  // (bytes: a J3DSys member would run its constructor)
        void* oldVcdVat;
        u32 envelope;
        u32 pad;
    };
    struct PortPipeSnap {
        PortPipeCtx pre;   // the game thread's context where the pass stands
        PortPipeCtx post;  // and after it: what the pass is expected to leave (the game thread goes on from this)
    };
    // The rest of the J3D draw path's statics: scratch a shape sets before it uses it (the matrix-load pipeline and cache, the
    // differed texture matrices). Saved before a token is executed and put back after it, so that a pass executed in the middle
    // of a model the game thread draws by hand (a recorded draw ended there) leaves that model's draw as it found it.
    struct PortPipeScratch {
        u16 cache[10];
        MtxPtr tbl[2];
        u32 pipeline, texMtxLoadType;
        u8* scaleFlag;
        J3DTexGenBlock* texGenBlock;
        J3DTexMtxObj* texMtxObj;
        bool nbt, lod;
        void save() {
            memcpy(cache, J3DShapeMtx::sMtxLoadCache, sizeof cache);
            tbl[0] = J3DShapeMtxConcatView::sMtxPtrTbl[0], tbl[1] = J3DShapeMtxConcatView::sMtxPtrTbl[1];
            pipeline = J3DShapeMtx::sCurrentPipeline, texMtxLoadType = J3DShapeMtx::sTexMtxLoadType;
            scaleFlag = J3DShapeMtx::sCurrentScaleFlag;
            texGenBlock = J3DDifferedTexMtx::sTexGenBlock, texMtxObj = J3DDifferedTexMtx::sTexMtxObj;
            nbt = J3DShapeMtx::sNBTFlag, lod = J3DShapeMtx::sLODFlag;
        }
        void load() const {
            memcpy(J3DShapeMtx::sMtxLoadCache, cache, sizeof cache);
            J3DShapeMtxConcatView::sMtxPtrTbl[0] = tbl[0], J3DShapeMtxConcatView::sMtxPtrTbl[1] = tbl[1];
            J3DShapeMtx::sCurrentPipeline = pipeline, J3DShapeMtx::sTexMtxLoadType = texMtxLoadType;
            J3DShapeMtx::sCurrentScaleFlag = scaleFlag;
            J3DDifferedTexMtx::sTexGenBlock = texGenBlock, J3DDifferedTexMtx::sTexMtxObj = texMtxObj;
            J3DShapeMtx::sNBTFlag = nbt, J3DShapeMtx::sLODFlag = lod;
        }
    };
    port::drawpipe::Tokens sPortPipeTok;  // this picture's tokens (written by the game thread; read where they are executed)
    PortPipeCtx sPortPipeDraw;            // the draw side's context
    // The game thread's context at the last pass executed (what the next snapshot is compared with): the `post` of that pass's
    // token, kept as its place in the tokens' bytes (they move when the store grows); before the first pass, sPortPipeBase0.
    PortPipeCtx sPortPipeBase0;
    u32 sPortPipeBaseAt = 0xFFFFFFFFu;
    const PortPipeCtx* portPipeBase() {
        return sPortPipeBaseAt == 0xFFFFFFFFu ? &sPortPipeBase0 : reinterpret_cast< const PortPipeCtx* >(&sPortPipeTok.bytes[sPortPipeBaseAt]);
    }
    // ---- what an open draw list changes under the passes (dpengine 2: the fur draw, FurCtrl.cpp)
    // A fur draw writes its shape's matrix-index words and leaves them, and for its length it points the model's shape packet
    // at its layer models. In the serial order a pass that comes BEFORE the fur draw sees neither. A pass recorded as a token is
    // executed later (by the draw thread, or at once when the recording is ended), after the game thread's fur draw has run:
    //   - the words as they were before the fur draw are kept here with the number of tokens made so far; a token with a
    //     smaller number is executed with those words in the shape and the shape's own put back after it (nothing else reads
    //     them meanwhile: the game thread writes them in the dl flush and in the draw only);
    //   - a token executed in the middle of a fur draw (the recording ended there) is given the packet's real model for its length.
    struct PortPipeWords {
        J3DShape* shape;
        u32 a, b;      // the words before the write
        u32 tokenAt;   // tokens made before the write: the passes that must not see it
    };
    enum { kPortPipeWordsMax = 48 };
    PortPipeWords sPortPipeWords[kPortPipeWordsMax];
    u32 sPortPipeWordsNum = 0;
    J3DShapePacket* sPortPipeLentPacket = nullptr;
    J3DModel* sPortPipeLentModel = nullptr;
    bool sPortPipeOpen = false;           // between a hand-over with a frame and the end of its draw
    bool (*sPortPipeTokenOut)(u32, bool) = nullptr;  // set while the draw's GX calls are recorded: a token is handed out, not executed
    void (*sPortPipeAbandon)(int) = nullptr;
    // (pipe27, GXV_PIPE25) called after a pass that was executed where it stands (its token was not handed out): verify's in-place walk;
    // and whether a fur write ends the deferral (the late draw keeps no "words before the write": the passes made so far run first)
    void (*sPortPipeDone)(u32) = nullptr;
    bool sPortPipeLate = false;

    void portPipeCtxNow(PortPipeCtx* pCtx) {
        memcpy(pCtx->sys, &j3dSys, sizeof(J3DSys));
        // (the bytes between the J3DSys and the next member, if any: contexts are compared word by word)
        if (offsetof(PortPipeCtx, oldVcdVat) > sizeof(J3DSys)) {
            memset(pCtx->sys + sizeof(J3DSys), 0, offsetof(PortPipeCtx, oldVcdVat) - sizeof(J3DSys));
        }
        pCtx->oldVcdVat = J3DShape::sOldVcdVatCmd;
        pCtx->envelope = J3DShape::sEnvelopeFlag ? 1u : 0u;
        pCtx->pad = 0;
        if (sizeof(PortPipeCtx) > offsetof(PortPipeCtx, pad) + sizeof(u32)) {
            memset(reinterpret_cast< unsigned char* >(pCtx) + offsetof(PortPipeCtx, pad) + sizeof(u32), 0,
                   sizeof(PortPipeCtx) - offsetof(PortPipeCtx, pad) - sizeof(u32));
        }
    }
    // Will the pass draw this packet's shape? (DrawBufferShapeDrawer's tests over the frozen packet.)
    bool portPipeDrawn(const port::drawpipe::FrozenPacket& rPacket) {
        return rPacket.flags == 0 && static_cast< const J3DShapePacket* >(rPacket.shapePacket)->mpShape != nullptr;
    }
    // What the pass leaves, from the frozen lists: the two statics (*ppVcd / *pEnv come in as they are before it), and the last
    // packet whose shape it draws (nullptr: none).
    // J3DShape::sOldVcdVatCmd: each model with actors clears it, each drawer's first packet sets its shape's list
    // (loadPreDrawSetting), each shape drawn sets its own: the last of these in the pass. sEnvelopeFlag: the last shape drawn sets it.
    J3DShapePacket* portPipeLeave(const port::drawpipe::Frame& rFrame, const port::drawpipe::FrozenGroup& rGroup, bool xlu, void** ppVcd, u32* pEnv) {
        using namespace port::drawpipe;
        bool vcdKnown = false;
        for (u32 e = rGroup.numExec; e-- > 0;) {
            const FrozenExec& fe = rFrame.execs[rGroup.firstExec + e];
            if (fe.numActors <= 0) {
                continue;
            }
            const u32 first = fe.firstDrawer + (xlu ? fe.numOpa : 0);
            const u32 end = fe.firstDrawer + (xlu ? fe.numDrawers : fe.numOpa);
            for (u32 d = end; d-- > first;) {
                const FrozenDrawer& fd = rFrame.drawers[d];
                const FrozenPacket* packets = &rFrame.packets[fd.first];
                for (u32 i = fd.count; i-- > 0;) {
                    J3DShapePacket* packet = static_cast< J3DShapePacket* >(packets[i].shapePacket);
                    if (portPipeDrawn(packets[i])) {
                        if (!vcdKnown) {
                            *ppVcd = packet->mpShape->mVcdVatCmd;
                        }
                        *pEnv = packet->mpShape->mHasPNMTXIdx ? 1u : 0u;
                        return packet;
                    }
                }
                if (!vcdKnown && fd.count != 0) {
                    // (nothing of the pass's last drawer is drawn: its first packet's shape still sent its list)
                    *ppVcd = static_cast< const J3DShapePacket* >(packets[0].shapePacket)->mpShape->mVcdVatCmd;
                    vcdKnown = true;
                }
            }
            if (!vcdKnown) {
                *ppVcd = nullptr;  // (the model has actors and no drawer in this pass: cleared, nothing sent)
                vcdKnown = true;
            }
        }
        return nullptr;
    }
    // Does the pass leave the GX library's draw serial alone (no primitive, no list call: nothing that can write the frame
    // buffer)? Only a drawer sends those: a pass whose models have no drawer in this half (opaque / translucent) loads lights
    // at most. The capture layer is told with the token, so that what it answers about the serial stays good across the pass.
    bool portPipeNoDraw(const port::drawpipe::Frame& rFrame, const port::drawpipe::FrozenGroup& rGroup, bool xlu) {
        for (u32 e = 0; e < rGroup.numExec; ++e) {
            const port::drawpipe::FrozenExec& fe = rFrame.execs[rGroup.firstExec + e];
            if (fe.numActors > 0 && (xlu ? fe.numDrawers - fe.numOpa : fe.numOpa) != 0) {
                return false;
            }
        }
        return true;
    }
    // A token of this picture: its index, or -1 without memory.
    s32 portPipeToken(u32 kind, s32 a, s32 b, bool snap) {
        using namespace port::drawpipe;
        u32 at = 0xFFFFFFFFu;
        if (snap) {
            at = sPortPipeTok.bytes.Count();
            if (sPortPipeTok.bytes.Push(static_cast< u32 >(sizeof(PortPipeSnap))) == nullptr) {
                return -1;
            }
        }
        TokenRec* rec = sPortPipeTok.recs.Push();
        if (rec == nullptr) {
            return -1;
        }
        rec->kind = kind;
        rec->a = a;
        rec->b = b;
        rec->ctx = at;
        return static_cast< s32 >(sPortPipeTok.recs.Count() - 1);
    }
}  // namespace

// The frame the passes draw from, with the scene state that goes with it, from now until the next call (nullptr: none; the draw
// of the last one is over: what its passes wrote into their own J3DSys goes back into j3dSys).
void DrawBufferHolder::portPipeUse(const port::drawpipe::Frame* pFrame, port::drawpipe::PassStats* pStats, const PortPipeScene* pScene) {
    if (sPortPipeOpen) {
        sPortPipeOpen = false;
        u32 taken = 0, conflicts = 0;
        const PortPipeCtx& base = *portPipeBase();
        if (memcmp(&base, &sPortPipeDraw, sizeof base) != 0) {  // (nearly always equal: every pass left what its token expected)
            port::drawpipe::MergeChanged(&j3dSys, base.sys, sPortPipeDraw.sys, sizeof(J3DSys), &taken, &conflicts);
            // (what the last pass left where its token expected something else, unless the game thread wrote the word since)
            if (sPortPipeDraw.oldVcdVat != base.oldVcdVat && J3DShape::sOldVcdVatCmd == base.oldVcdVat) {
                J3DShape::sOldVcdVatCmd = sPortPipeDraw.oldVcdVat;
            }
            if (sPortPipeDraw.envelope != base.envelope && (J3DShape::sEnvelopeFlag ? 1u : 0u) == base.envelope) {
                J3DShape::sEnvelopeFlag = sPortPipeDraw.envelope != 0;
            }
        }
        sPortPipeStats->sysTaken += taken;
        sPortPipeStats->sysConflicts += conflicts;
    }
    sPortPipeTokenOut = nullptr;
    sPortPipeAbandon = nullptr;
    sPortPipeFrame = pFrame;
    sPortPipeSceneIn = pScene;
    sPortPipeStats = pStats != nullptr ? pStats : &sPortPipeNoStats;
    gPortPipePrepared = nullptr;  // (no packet of an earlier draw is "the one the game thread is drawing")
    sPortPipeWordsNum = 0;
    sPortPipeLentPacket = nullptr;
    sPortPipeLentModel = nullptr;
    if (pFrame != nullptr) {
        portPipeCtxNow(&sPortPipeDraw);
        sPortPipeBase0 = sPortPipeDraw;
        sPortPipeBaseAt = 0xFFFFFFFFu;
        sPortPipeTok.Clear();
        sPortPipeOpen = true;
    }
}

// FurCtrl::drawFur, before it writes its shape's matrix-index words (a, b: the values it is about to write). Only while passes
// wait as tokens (the draw is being recorded) is there anything to keep: a pass executed where it stands has drawn already.
extern "C" void port_drawpipe_shape_words(void* pShape, u32 a, u32 b) {
    if (!sPortPipeOpen || sPortPipeTokenOut == nullptr || sPortPipeTok.recs.Count() == 0) {
        return;
    }
    J3DShape* shape = static_cast< J3DShape* >(pShape);
    if (shape->mCurrentMtx.mMtxIdxRegA == a && shape->mCurrentMtx.mMtxIdxRegB == b) {
        return;  // (they hold these values already: the last fur draw left them and no flush has written them since)
    }
    if (sPortPipeLate) {
        // (GXV_PIPE25: the passes made so far are run now, in order, before this write; the rest of the draw is serial. Nothing to keep.)
        if (sPortPipeAbandon != nullptr) {
            sPortPipeAbandon(port::drawpipe::kWhyReview);
        }
        return;
    }
    if (sPortPipeWordsNum == kPortPipeWordsMax) {
        // (no room: the recording ends here, the tokens made so far are executed now, before the write)
        if (sPortPipeAbandon != nullptr) {
            sPortPipeAbandon(port::drawpipe::kWhyNoMemory);
        }
        return;
    }
    PortPipeWords& w = sPortPipeWords[sPortPipeWordsNum++];
    w.shape = shape;
    w.a = shape->mCurrentMtx.mMtxIdxRegA;
    w.b = shape->mCurrentMtx.mMtxIdxRegB;
    w.tokenAt = sPortPipeTok.recs.Count();
}
// FurCtrl::drawFur around its layers: pPacket is pointed at the layer models until the second call (nullptr, nullptr).
extern "C" void port_drawpipe_packet_lent(void* pPacket, void* pRealModel) {
    sPortPipeLentPacket = static_cast< J3DShapePacket* >(pPacket);
    sPortPipeLentModel = static_cast< J3DModel* >(pRealModel);
}

// GXV_DRAWPIPE=4: from now on a pass of the draw leaves a token with pTokenOut(id, the pass makes no GX call) instead of being
// executed (false from it: not recorded after all, execute); pAbandon ends the recording when a pass has to be drawn from the
// live lists.
// (nullptr, nullptr): the recording is over; tokens are executed where they stand again.
// (The light-dedup scopes need no token: the capture layer records the light shadow's drop and the scope's depth with each
// light load and each token, gx_capture.cpp.)
void DrawBufferHolder::portPipeRecord(bool (*pTokenOut)(u32, bool), void (*pAbandon)(int)) {
    sPortPipeTokenOut = pTokenOut;
    sPortPipeAbandon = pAbandon;
}

// GXV_PIPE25 (pipe27): the late draw's extras. pDone(id): a pass the token function did not take was executed where it stands (verify).
// late: a fur write ends the deferral (above). (nullptr, false): off. portPipeRewind: the draw side's J3D context back to where the
// picture's first pass found it, so that the passes can be executed a second time in order (verify's re-run on the helper).
// portPipeTokenInfo: a token's buffer type and translucency; portPipeGroupName: the first model of a buffer type's group (verify's naming).
void DrawBufferHolder::portPipeLateHooks(void (*pDone)(u32), bool late) {
    sPortPipeDone = pDone;
    sPortPipeLate = late;
}
void DrawBufferHolder::portPipeRewind() {
    sPortPipeDraw = sPortPipeBase0;
    sPortPipeBaseAt = 0xFFFFFFFFu;
}
bool DrawBufferHolder::portPipeTokenInfo(u32 id, s32* pType, bool* pXlu) {
    if (id >= sPortPipeTok.recs.Count()) {
        return false;
    }
    *pType = sPortPipeTok.recs[id].a;
    *pXlu = sPortPipeTok.recs[id].b != 0;
    return true;
}
const char* DrawBufferHolder::portPipeGroupName(s32 type) {
    const port::drawpipe::Frame* frame = sPortPipeFrame;
    if (frame == nullptr || frame->owner == nullptr || type < 0) {
        return "?";
    }
    const DrawBufferHolder* holder = static_cast< const DrawBufferHolder* >(frame->owner);
    if (holder->mBufferGroups.mArr == nullptr || type >= holder->mBufferGroups.size()) {
        return "?";
    }
    const DrawBufferGroup& group = holder->mBufferGroups[type];
    return group.mActiveExecutors.size() != 0 ? group.mActiveExecutors[0]->mName : "-";
}

// A token executed: on the thread that executes the recording (the draw thread; the game thread when it took the job back or
// the recording was abandoned), or where it was made (GXV_DRAWPIPE=1 / 3, and after an abandon). `displaced`: on the game
// thread, a token recorded earlier, executed now because the recording ended in the middle of the game thread's own draw.
void DrawBufferHolder::portPipeRunToken(u32 id, bool displaced) {
    using namespace port::drawpipe;
    if (id >= sPortPipeTok.recs.Count()) {
        return;
    }
    const TokenRec rec = sPortPipeTok.recs[id];
    const Frame* frame = sPortPipeFrame;
    if (rec.kind != kTokPass || frame == nullptr) {
        return;
    }
    // (the snapshots are read where they lie: nothing is added to the tokens while one is executed)
    const PortPipeSnap& snap = *reinterpret_cast< const PortPipeSnap* >(&sPortPipeTok.bytes[rec.ctx]);
    // (the words the game thread's side changed since the last pass, into the draw side's context; none, between two passes
    // with no draw list of the game thread's in between)
    const PortPipeCtx* base = portPipeBase();
    if (memcmp(base, &snap.pre, sizeof(PortPipeCtx)) != 0) {
        ApplyChanged(&sPortPipeDraw, base, &snap.pre, sizeof(PortPipeCtx));
    }
    void* const gameVcd = J3DShape::sOldVcdVatCmd;
    const bool gameEnv = J3DShape::sEnvelopeFlag;
    const J3DShapePacket* const gamePrepared = gPortPipePrepared;
    PortPipeScratch scratch;
    scratch.save();
    // (what a fur draw AFTER this pass has written into shapes since the token was made: the pass sees what it saw serially)
    u32 wordsNow[kPortPipeWordsMax][2];
    const u32 wordsNum = sPortPipeWordsNum;
    for (u32 w = wordsNum; w-- > 0;) {
        const PortPipeWords& e = sPortPipeWords[w];
        if (e.tokenAt > id) {
            wordsNow[w][0] = e.shape->mCurrentMtx.mMtxIdxRegA;
            wordsNow[w][1] = e.shape->mCurrentMtx.mMtxIdxRegB;
            e.shape->mCurrentMtx.mMtxIdxRegA = e.a;
            e.shape->mCurrentMtx.mMtxIdxRegB = e.b;
        }
    }
    // (executed in the middle of a fur draw: the packet it has pointed at a layer model is the real model's for the pass)
    J3DShapePacket* const lent = displaced ? sPortPipeLentPacket : nullptr;
    J3DModel* lentNow = nullptr;
    if (lent != nullptr) {
        lentNow = lent->getModel();
        lent->setModel(sPortPipeLentModel);
    }
    J3DShape::sOldVcdVatCmd = sPortPipeDraw.oldVcdVat;
    J3DShape::sEnvelopeFlag = sPortPipeDraw.envelope != 0;
    gPortJ3DSysDraw = reinterpret_cast< J3DSys* >(sPortPipeDraw.sys);
    gPortPipeScene = sPortPipeSceneIn;
    // (GXV_DRAWPIPE's log: what the passes themselves cost, timed on some turns: the part of the draw that can leave the game thread)
    const bool timed = sPortPipeStats->timeThis;
    const int64_t t0 = timed ? port_host_time_ns() : 0;
    static_cast< const DrawBufferHolder* >(frame->owner)->portPipeDrawFrozen(*frame, rec.a, rec.b != 0);
    if (timed) {
        sPortPipeStats->passNs += static_cast< u64 >(port_host_time_ns() - t0);
    }
    gPortPipeScene = nullptr;
    gPortJ3DSysDraw = &j3dSys;
    sPortPipeDraw.oldVcdVat = J3DShape::sOldVcdVatCmd;
    sPortPipeDraw.envelope = J3DShape::sEnvelopeFlag ? 1u : 0u;
    J3DShape::sOldVcdVatCmd = gameVcd;
    J3DShape::sEnvelopeFlag = gameEnv;
    if (lent != nullptr) {
        lent->setModel(lentNow);
    }
    for (u32 w = 0; w < wordsNum; ++w) {
        const PortPipeWords& e = sPortPipeWords[w];
        if (e.tokenAt > id) {
            e.shape->mCurrentMtx.mMtxIdxRegA = wordsNow[w][0];
            e.shape->mCurrentMtx.mMtxIdxRegB = wordsNow[w][1];
        }
    }
    scratch.load();
    gPortPipePrepared = gamePrepared;
    if (displaced && gamePrepared != nullptr) {
        // (the pass set shapes up for its own packets, maybe the shape of the model the game thread was in the middle of: that
        // packet's set-up again, as its own prepareDraw left it. J3D_DSYS is j3dSys here.)
        gamePrepared->prepareDraw();
    }
    // (what the pass left against what its token expected)
    if (memcmp(&snap.post, &sPortPipeDraw, sizeof(PortPipeCtx)) != 0) {
        if (snap.post.oldVcdVat != sPortPipeDraw.oldVcdVat || snap.post.envelope != sPortPipeDraw.envelope) {
            ++sPortPipeStats->predictMiss;
        }
        // (the two matrix-array words are counted apart: a shape with several matrix groups sets them again as it draws)
        const size_t mtxLo = offsetof(J3DSys, mModelDrawMtx), mtxHi = offsetof(J3DSys, mModelNrmMtx) + sizeof(void*);
        for (size_t at = 0; at + 4 <= sizeof(J3DSys); at += 4) {
            if (memcmp(snap.post.sys + at, sPortPipeDraw.sys + at, 4) != 0) {
                ++(at >= mtxLo && at < mtxHi ? sPortPipeStats->mtxMiss : sPortPipeStats->sysMiss);
            }
        }
    }
    sPortPipeBaseAt = rec.ctx + static_cast< u32 >(offsetof(PortPipeSnap, post));
}

// One group into the frame (pFrame == nullptr: only its hash, from the live lists: the verify's compare at the pass).
u64 DrawBufferHolder::portPipeFreezeGroup(port::drawpipe::Frame* pFrame, s32 drawBufferType) const {
    using namespace port::drawpipe;
    // (GXV_DRAWPIPE=2, dpengine: the verify's hash also covers what a pass reads THROUGH its lists and nothing but a flush should
    // write between the hand-over and the pass: which shapes are hidden or culled, which lists the packets and the drawers call.
    // A game-thread draw list that writes one of these (a fur draw, a model drawn by hand) would be seen by a pass that came
    // before it in the serial order once the pass is drawn later: "differed" must stay 0 on the console for the overlap to be exact.)
    const bool deep = port_drawpipe_mode() == 2;
    // (dpengine 2: the hash is compared in mode 2 only, a frozen group's against the live lists' at the pass; in the other
    // modes the freeze does not compute it: on the console the 64-bit mixes were a good part of the freeze's time)
    const bool hashOn = deep || pFrame == nullptr;
    const DrawBufferGroup& group = mBufferGroups[drawBufferType];
    const s32 numExec = group.mActiveExecutors.size();
    u64 h = HashValue(0x51ED270B0F2Dull, numExec);
    h = HashValue(h, group.mLightLoadType);
    FrozenGroup* fg = nullptr;
    if (pFrame != nullptr) {
        fg = &pFrame->groups[static_cast< u32 >(drawBufferType)];
        fg->firstExec = pFrame->execs.Count();
        fg->numExec = static_cast< u32 >(numExec);
        fg->lightLoadType = group.mLightLoadType;
    }
    for (s32 e = 0; e < numExec; ++e) {
        const DrawBufferExecuter* exec = group.mActiveExecutors[e];
        const DrawBuffer* buffer = exec->mDrawBuffer;
        const bool any = buffer->mNumActors > 0;
        h = HashValue(h, reinterpret_cast< uintptr_t >(exec));
        h = HashValue(h, exec->mLightType);
        h = HashValue(h, any);
        FrozenExec* fe = pFrame != nullptr ? pFrame->execs.Push() : nullptr;
        if (pFrame != nullptr && fe == nullptr) {
            pFrame->valid = false;
            return 0;
        }
        if (fe != nullptr) {
            fe->exec = exec;
            fe->lightType = exec->mLightType;
            fe->numActors = buffer->mNumActors;
            fe->firstDrawer = pFrame->drawers.Count();
            fe->numOpa = any ? static_cast< u32 >(buffer->mNumOpaShapeDrawers) : 0;
            fe->numDrawers = any ? static_cast< u32 >(buffer->mNumShapeDrawers) : 0;
        }
        if (!any) {
            continue;  // (DrawBuffer::drawOpa / drawXlu draw nothing of it)
        }
        for (s32 d = 0; d < buffer->mNumShapeDrawers; ++d) {
            const DrawBufferShapeDrawer* drawer = buffer->mShapeDrawers[d];
            const s32 num = drawer->mNumPackets;
            if (hashOn) {
                h = HashValue(h, num);
            }
            FrozenDrawer* fd = pFrame != nullptr ? pFrame->drawers.Push() : nullptr;
            FrozenPacket* fp = pFrame != nullptr && num > 0 ? pFrame->packets.Push(static_cast< u32 >(num)) : nullptr;
            if (pFrame != nullptr && (fd == nullptr || fp == nullptr)) {
                pFrame->valid = false;  // (no memory, or a drawer without packets in a model that has actors: draw() would read packet 0)
                return 0;
            }
            if (fd != nullptr) {
                fd->drawer = drawer;
                fd->first = pFrame->packets.Count() - static_cast< u32 >(num);
                fd->count = static_cast< u32 >(num);
                fd->serial = drawer->mPortSerial;
            }
            if (deep) {
                if (const J3DDisplayListObj* dl = drawer->mMatPacket->mpDisplayListObj) {
                    h = HashValue(HashValue(h, reinterpret_cast< uintptr_t >(dl->mpDisplayList[0])), dl->mSize);
                }
            }
            for (s32 i = 0; i < num; ++i) {
                const PacketInfo* info = drawer->mPackets[i];
                if (hashOn) {
                    h = HashValue(h, reinterpret_cast< uintptr_t >(info->mShapePacket));
                }
                if (deep) {
                    const J3DShapePacket* sp = info->mShapePacket;
                    h = HashValue(HashValue(h, sp->mFlags), reinterpret_cast< uintptr_t >(sp->mpShape));
                    h = HashValue(HashValue(h, reinterpret_cast< uintptr_t >(sp->mpTexMtxObj)), reinterpret_cast< uintptr_t >(sp->mpUserArea));
                    if (sp->mpShape != nullptr) {
                        h = HashValue(h, sp->mpShape->mFlags & 1u);
                    }
                    if (sp->mpModel != nullptr) {
                        h = HashValue(h, sp->mpModel->mPortCullPicture);
                    }
                    if (const J3DDisplayListObj* dl = sp->mpDisplayListObj) {
                        h = HashValue(HashValue(h, reinterpret_cast< uintptr_t >(dl->mpDisplayList[0])), dl->mSize);
                    }
                }
                s32 light = -2;
                if (info->mLightLoader != nullptr) {
                    const ActorLightInfo* loaded = portPipeLightOf(info->mLightLoader);
                    light = -1;
                    if (loaded != nullptr) {
                        if (hashOn) {
                            h = HashBytes(loaded, sizeof(ActorLightInfo), h);
                        }
                        if (pFrame != nullptr) {
                            // (the copy starts on an 8-byte step of a malloc block: aligned as an ActorLightInfo wants)
                            const u32 room = (static_cast< u32 >(sizeof(ActorLightInfo)) + 7u) & ~7u;
                            light = static_cast< s32 >(pFrame->lights.Count());
                            unsigned char* at = pFrame->lights.Push(room);
                            if (at == nullptr) {
                                pFrame->valid = false;
                                return 0;
                            }
                            memcpy(at, loaded, sizeof(ActorLightInfo));
                        } else {
                            light = 0;
                        }
                    }
                }
                if (hashOn) {
                    h = HashValue(h, light < 0 ? light : 0);
                }
                if (fp != nullptr) {
                    fp[i].shapePacket = info->mShapePacket;
                    fp[i].light = light;
                    // (the picture this hand-over draws is the next one: port_j3d_picture_begin comes after the hand-over)
                    const J3DShapePacket* sp = info->mShapePacket;
                    u32 flags = (sp->mpShape != nullptr && (sp->mpShape->mFlags & 1) != 0) ? kFrozenHidden : 0u;
                    if (sp->mpModel != nullptr && sp->mpModel->mPortCullPicture == J3DPortPicture() + 1) {
                        flags |= kFrozenCulled;
                    }
                    if (sp->checkFlag(J3DShpFlag_Hidden)) {
                        flags |= kFrozenPacketHidden;
                    }
                    fp[i].flags = flags;
                }
            }
        }
    }
    if (fg != nullptr) {
        fg->hash = h;
    }
    return h;
}

bool DrawBufferHolder::portPipeFreeze(port::drawpipe::Frame& rFrame) const {
    using namespace port::drawpipe;
    rFrame.Clear();
    rFrame.owner = this;
    rFrame.epoch = gPortDrawPipeEpoch;
    rFrame.extSerial = J3DPortExtSerial();
    const s32 numGroups = mBufferGroups.size();
    if (numGroups <= 0 || mBufferGroups.mArr == nullptr) {
        return false;
    }
    FrozenGroup* groups = rFrame.groups.Push(static_cast< u32 >(numGroups));
    if (groups == nullptr) {
        return false;
    }
    for (s32 t = 0; t < numGroups; ++t) {
        groups[t] = FrozenGroup();
    }
    rFrame.valid = true;
    for (s32 t = 0; t < numGroups && rFrame.valid; ++t) {
        if (mBufferGroups[t].mActiveExecutors.size() != 0) {
            portPipeFreezeGroup(&rFrame, t);
        }
    }
    return rFrame.valid;
}

// A pass asked for by the draw (drawOpa / drawXlu) while a frame is in use. true: taken care of (drawn from the frame here, or
// left as a token for the thread that executes the recorded draw). false: the caller draws it from the live lists, as always (no
// frame, another holder's frame, the census's mode, or the lists moved since the freeze).
bool DrawBufferHolder::portPipeDraw(s32 drawBufferType, bool xlu) const {
    using namespace port::drawpipe;
    const Frame* frame = sPortPipeFrame;
    if (frame == nullptr) {
        return false;
    }
    PassStats& st = *sPortPipeStats;
    if (frame->owner != this || !frame->valid || drawBufferType < 0 || static_cast< u32 >(drawBufferType) >= frame->groups.Count()) {
        if (sPortPipeAbandon != nullptr) {
            sPortPipeAbandon(kWhyListMoved);  // (a recorded draw cannot hold a pass from the live lists: executed up to here, serial from here)
        }
        ++st.livePasses;
        return false;
    }
    const FrozenGroup& fg = frame->groups[static_cast< u32 >(drawBufferType)];
    if (port_drawpipe_mode() == 2) {
        // (GXV_DRAWPIPE=2) the pass is drawn live; would the frozen lists have drawn the same?
        ++st.verifyPasses;
        const u64 live = mBufferGroups[drawBufferType].mActiveExecutors.size() != 0 ? portPipeFreezeGroup(nullptr, drawBufferType) : 0;
        if (live != fg.hash) {
            if (st.verifyDiffer++ == 0) {
                const DrawBufferGroup& group = mBufferGroups[drawBufferType];
                snprintf(st.firstDiffer, sizeof st.firstDiffer, "type 0x%X %.12s", static_cast< unsigned >(drawBufferType),
                         group.mActiveExecutors.size() != 0 ? group.mActiveExecutors[0]->mName : "-");
            }
        }
        ++st.livePasses;
        return false;
    }
    if (frame->guardEpoch && gPortDrawPipeEpoch != frame->epoch) {
        if (sPortPipeAbandon != nullptr) {
            sPortPipeAbandon(kWhyListMoved);
        }
        ++st.epochFallbacks;
        ++st.livePasses;
        return false;
    }
    if (fg.numExec == 0) {
        // (the buffer type had no model at the hand-over: the pass does nothing at all, as DrawBufferGroup's own test has it.
        // No token: there is nothing to execute and nothing it could leave.)
        ++st.frozenPasses;
        ++st.emptyPasses;
        return true;
    }
    // The pass's token: the game thread's J3D context where the pass stands, and the one the pass leaves. Both are written
    // straight into the tokens' bytes.
    const s32 id = portPipeToken(kTokPass, drawBufferType, xlu ? 1 : 0, true);
    if (id < 0) {
        if (sPortPipeAbandon != nullptr) {
            sPortPipeAbandon(kWhyNoMemory);
        }
        ++st.livePasses;
        return false;
    }
    PortPipeSnap& snap = *reinterpret_cast< PortPipeSnap* >(&sPortPipeTok.bytes[sPortPipeTok.recs[static_cast< u32 >(id)].ctx]);
    portPipeCtxNow(&snap.pre);
#if !defined(PORT_TEST_NO_GUESS)  // (tests/draw_pipe.cpp's negative control is built without this)
    {
        // The game thread's side goes on from what the pass leaves (see the J3D context's story above): the draw mode; the last
        // drawn packet's prepareDraw, for real (J3D_DSYS is j3dSys here: its model, packet, vertex arrays and flag, and the
        // shape's set-up, as the pass's last shape will leave them); the two matrix arrays J3DShape::setArrayAndBindPipeline
        // sets; the two statics.
        void* vcd = snap.pre.oldVcdVat;
        u32 env = snap.pre.envelope;
        J3DShapePacket* last = portPipeLeave(*frame, fg, xlu, &vcd, &env);
        j3dSys.mDrawMode = xlu ? 4 : 3;
        if (last != nullptr) {
            last->prepareDraw();
            const J3DShape* shape = last->mpShape;
            j3dSys.mModelDrawMtx = shape->mDrawMtx[*shape->mCurrentViewNo];
            j3dSys.mModelNrmMtx = shape->mNrmMtx[*shape->mCurrentViewNo];
        }
        J3DShape::sOldVcdVatCmd = vcd;
        J3DShape::sEnvelopeFlag = env != 0;
    }
#endif
    portPipeCtxNow(&snap.post);
    // (the second argument: the pass draws nothing, portPipeNoDraw)
    if (sPortPipeTokenOut == nullptr || !sPortPipeTokenOut(static_cast< u32 >(id), portPipeNoDraw(*frame, fg, xlu))) {
        portPipeRunToken(static_cast< u32 >(id), false);
        if (sPortPipeDone != nullptr) {
            sPortPipeDone(static_cast< u32 >(id));  // (GXV_PIPE25=2: the pass as the game thread made it in place)
        }
    }
    return true;
}

// The pass from the frame, as DrawBufferGroup::drawOpa / DrawBufferExecuter::drawOpa / DrawBuffer::drawOpa draw it from the live
// lists. Reads nothing the next update writes (the lists and lights are the frame's, the J3DSys the draw side's, the scene's
// lights and camera gPortPipeScene's).
void DrawBufferHolder::portPipeDrawFrozen(const port::drawpipe::Frame& rFrame, s32 drawBufferType, bool xlu) const {
    using namespace port::drawpipe;
    const Frame* frame = &rFrame;
    const FrozenGroup& fg = frame->groups[static_cast< u32 >(drawBufferType)];
    ++sPortPipeStats->frozenPasses;
    if (fg.numExec == 0) {
        return;  // (isBufferGroupEmpty at the hand-over)
    }
    const int lightScope = port_light_scope_enter(1);
    J3D_DSYS.mDrawMode = xlu ? 4 : 3;
    if (fg.lightLoadType != -1) {
        MR::loadLight(fg.lightLoadType);
    }
    for (u32 e = 0; e < fg.numExec; ++e) {
        const FrozenExec& fe = frame->execs[fg.firstExec + e];
        if (fe.lightType != -1) {
            MR::loadLight(fe.lightType);
        }
        if (fe.numActors > 0) {
            J3DShape::sOldVcdVatCmd = nullptr;
            const u32 first = fe.firstDrawer + (xlu ? fe.numOpa : 0);
            const u32 end = fe.firstDrawer + (xlu ? fe.numDrawers : fe.numOpa);
            for (u32 d = first; d < end; ++d) {
                const FrozenDrawer& fd = frame->drawers[d];
                static_cast< const DrawBufferShapeDrawer* >(fd.drawer)->drawFrozen(*frame, &frame->packets[fd.first], static_cast< s32 >(fd.count), fd.serial);
            }
        }
    }
    port_light_scope_exit(lightScope);
}

// Everything the J3D passes read, hashed per object (the census; game thread, nothing written). An object reached twice (an actor
// in two groups, a shared list) is hashed once.
void DrawBufferHolder::portPipeWalk(port::drawpipe::Set& rSet, port::drawpipe::Names& rNames) const {
    using namespace port::drawpipe;
    const s32 numGroups = mBufferGroups.size();
    if (mBufferGroups.mArr == nullptr) {
        return;
    }
    for (s32 t = 0; t < numGroups; ++t) {
        const DrawBufferGroup& group = mBufferGroups[t];
        const s32 numExec = group.mActiveExecutors.size();
        if (numExec == 0) {
            continue;
        }
        u64 h = HashValue(HashValue(1, numExec), group.mLightLoadType);
        for (s32 e = 0; e < numExec; ++e) {
            const DrawBufferExecuter* exec = group.mActiveExecutors[e];
            h = HashValue(HashValue(HashValue(h, reinterpret_cast< uintptr_t >(exec)), exec->mLightType), exec->mDrawBuffer->mNumActors > 0);
        }
        rSet.Add(kLists, &group, h, Tag(rNames.Of(group.mActiveExecutors[0], group.mActiveExecutors[0]->mName), static_cast< u32 >(t)),
                 static_cast< u32 >(numExec) * 12u + 8u);
        for (s32 e = 0; e < numExec; ++e) {
            const DrawBufferExecuter* exec = group.mActiveExecutors[e];
            const DrawBuffer* buffer = exec->mDrawBuffer;
            const u32 name = rNames.Of(exec, exec->mName);
            if (buffer->mNumActors <= 0) {
                continue;
            }
            for (s32 d = 0; d < buffer->mNumShapeDrawers; ++d) {
                const DrawBufferShapeDrawer* drawer = buffer->mShapeDrawers[d];
                const s32 num = drawer->mNumPackets;
                u64 hp = HashValue(2, num);
                for (s32 i = 0; i < num; ++i) {
                    const PacketInfo* info = drawer->mPackets[i];
                    hp = HashValue(HashValue(hp, reinterpret_cast< uintptr_t >(info->mShapePacket)), reinterpret_cast< uintptr_t >(info->mLightLoader));
                }
                rSet.Add(kPackets, drawer, hp, Tag(name, static_cast< u32 >(d)), static_cast< u32 >(num) * 8u + 4u);
                if (const J3DDisplayListObj* dl = drawer->mMatPacket->mpDisplayListObj) {
                    const u32 size = dl->mSize;
                    const void* bytes = dl->mpDisplayList[0];
                    const u64 hd = HashValue(HashValue(3, reinterpret_cast< uintptr_t >(bytes)), size);
                    rSet.Add(kMatDL, dl, bytes != nullptr ? HashBytes(bytes, size, hd) : hd, Tag(name, static_cast< u32 >(d)), size + 8u);
                }
                for (s32 i = 0; i < num; ++i) {
                    const PacketInfo* info = drawer->mPackets[i];
                    const J3DShapePacket* sp = info->mShapePacket;
                    const u32 tag = Tag(name, static_cast< u32 >(i));
                    if (const J3DDisplayListObj* dl = sp->mpDisplayListObj) {
                        const u32 size = dl->mSize;
                        const void* bytes = dl->mpDisplayList[0];
                        const u64 hd = HashValue(HashValue(4, reinterpret_cast< uintptr_t >(bytes)), size);
                        rSet.Add(kDiffDL, dl, bytes != nullptr ? HashBytes(bytes, size, hd) : hd, tag, size + 8u);
                    }
                    u64 hs = HashValue(HashValue(5, sp->mFlags), reinterpret_cast< uintptr_t >(sp->mpShape));
                    hs = HashValue(HashValue(hs, reinterpret_cast< uintptr_t >(sp->mpMtxBuffer)), reinterpret_cast< uintptr_t >(sp->mpBaseMtxPtr));
                    hs = HashValue(HashValue(hs, sp->mDiffFlag), reinterpret_cast< uintptr_t >(sp->mpModel));
                    hs = HashValue(HashValue(hs, reinterpret_cast< uintptr_t >(sp->mpTexMtxObj)), reinterpret_cast< uintptr_t >(sp->mpUserArea));
                    if (const J3DShape* shape = sp->mpShape) {
                        hs = HashValue(hs, shape->mFlags);
                        hs = HashBytes(&shape->mCurrentMtx, sizeof shape->mCurrentMtx, hs);
                    }
                    rSet.Add(kShape, sp, hs, tag, 48u);
                    if (const ActorLightCtrl* loader = info->mLightLoader) {
                        u64 hl = HashValue(HashValue(6, reinterpret_cast< uintptr_t >(loader->mAreaLightInf)), reinterpret_cast< uintptr_t >(loader->_1C));
                        if (const ActorLightInfo* loaded = portPipeLightOf(loader)) {
                            hl = HashBytes(loaded, sizeof(ActorLightInfo), hl);
                        }
                        rSet.Add(kLight, loader, hl, tag, static_cast< u32 >(sizeof(ActorLightInfo)) + 8u);
                    }
                }
            }
            for (s32 a = 0; a < exec->mNumActors; ++a) {
                const LiveActor* actor = exec->mActors[a];
                const u32 tag = Tag(name, static_cast< u32 >(a));
                rSet.Add(kActor, actor, HashBytes(&actor->mFlag, sizeof actor->mFlag, 7), tag, static_cast< u32 >(sizeof actor->mFlag));
                J3DModel* model = MR::getJ3DModel(actor);
                if (model == nullptr) {
                    continue;
                }
                u64 hm = HashValue(HashValue(8, model->mFlags), model->mPortCullPicture);
                hm = HashBytes(&model->mVertexBuffer, sizeof model->mVertexBuffer, hm);
                J3DMtxBuffer* mb = model->mMtxBuffer;
                hm = HashValue(hm, reinterpret_cast< uintptr_t >(mb));
                if (mb == nullptr || mb->mJointTree == nullptr) {
                    rSet.Add(kModel, model, hm, tag, 72u);
                    continue;
                }
                const u32 view = mb->mCurrentViewNo;
                Mtx* draw = mb->mpDrawMtxArr[1] != nullptr ? mb->mpDrawMtxArr[1][view] : nullptr;
                Mtx33* nrm = mb->mpNrmMtxArr[1] != nullptr ? mb->mpNrmMtxArr[1][view] : nullptr;
                hm = HashValue(HashValue(HashValue(hm, view), reinterpret_cast< uintptr_t >(draw)), reinterpret_cast< uintptr_t >(nrm));
                rSet.Add(kModel, model, hm, tag, 84u);
                const u32 numDraw = mb->mJointTree->getDrawMtxNum();
                const u32 numJoint = mb->mJointTree->getJointNum();
                const u32 numEnv = mb->mJointTree->getWEvlpMtxNum();
                // (the halves no model owns, J3DMtxBuffer::sNoUseDrawMtx, are one matrix whatever the count says)
                const bool ownDraw = draw != nullptr && draw != J3DMtxBuffer::sNoUseDrawMtxPtr;
                const bool ownNrm = nrm != nullptr && nrm != J3DMtxBuffer::sNoUseNrmMtxPtr;
                u64 hx = 9;
                u32 bytes = 0;
                if (ownDraw) {
                    hx = HashBytes(draw, sizeof(Mtx) * numDraw, hx);
                    bytes += static_cast< u32 >(sizeof(Mtx)) * numDraw;
                }
                if (ownNrm) {
                    hx = HashBytes(nrm, sizeof(Mtx33) * numDraw, hx);
                    bytes += static_cast< u32 >(sizeof(Mtx33)) * numDraw;
                }
                rSet.Add(kDrawMtx, mb, hx, tag, bytes);
                u64 hj = 10;
                bytes = 0;
                if (mb->mpAnmMtx != nullptr) {
                    hj = HashBytes(mb->mpAnmMtx, sizeof(Mtx) * numJoint, hj);
                    bytes += static_cast< u32 >(sizeof(Mtx)) * numJoint;
                }
                if (mb->mpWeightEvlpMtx != nullptr && numEnv != 0) {
                    hj = HashBytes(mb->mpWeightEvlpMtx, sizeof(Mtx) * numEnv, hj);
                    bytes += static_cast< u32 >(sizeof(Mtx)) * numEnv;
                }
                if (mb->mpScaleFlagArr != nullptr) {
                    hj = HashBytes(mb->mpScaleFlagArr, numJoint, hj);
                    bytes += numJoint;
                }
                // (keyed by the joint array: the matrix buffer is kDrawMtx's key)
                rSet.Add(kJoints, mb->mpAnmMtx != nullptr ? static_cast< const void* >(mb->mpAnmMtx) : static_cast< const void* >(&mb->mpAnmMtx), hj, tag, bytes);
            }
        }
    }
    rSet.Add(kSys, &j3dSys, HashBytes(&j3dSys, sizeof j3dSys, 11), 0, static_cast< u32 >(sizeof j3dSys));
}
#endif

#ifdef TARGET_PC
// ============================================================================================================== GXV_BGLAYER (bglayer25, 11 Nov 2026)
// The census of the far-background layer idea (agents/bglayer25/DESIGN.md): which share of a picture's DRAW work (shapes, display list bytes) would sit in a
// layer that could be drawn every second picture, by draw category (sky, air, sun, the astro dome's sky = background by nature; the planet / environment / map
// object lists = background by distance), and what the camera does from one picture to the next (turn and travel), so that the error of showing a stale
// picture of the far layer re-projected for the new camera can be judged. GXV_BGLAYER=2: counted and logged, nothing changes (default 0: nothing runs).
// Walks the live draw lists of GameScene::draw3D's picture every 4th picture (a few hundred shapes: ~20 us), on the game thread. Needs GXV_DRAWPIPE=0 (the shipped default)
// for the front end's by-category index counts (the markers sit where the draws are made); the game side's counts do not depend on it.
#include "Game/Util/SceneUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include "port/diagnostics.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include "port/host_vector.h"  // (heap36: the bglayer census's per-picture lists are host memory)

extern "C" int port_drawpipe_mode(void);
extern "C" const TVec3f* port_safe_player_pos(void);

extern "C" int port_bglayer_mode(void) {
    static const int sMode = [] {
        const char* e = getenv("GXV_BGLAYER");
        const long v = e && *e ? strtol(e, nullptr, 0) : 0;
        return static_cast< int >(v < 0 ? 0 : (v > 2 ? 2 : v));
    }();
    return sMode;
}

// 1 + the MR::DrawBufferType (or 64 + the MR::DrawType) of the draw about to be made (CategoryList): the front end tallies its draws under it. 0 closes it.
extern "C" int port_bglayer_marks_on(void) {
    static const int sOn = port_bglayer_mode() != 0 && port_drawpipe_mode() == 0 ? 1 : 0;
    return sOn;
}

namespace {
    constexpr s32 kBgBufs = 41;  // MR::DrawBufferType_ClippedMapParts .. _0x28
    constexpr int kBgBands = 5;
    const float kBgBand[kBgBands] = {4000.0f, 8000.0f, 10000.0f, 15000.0f, 25000.0f};
    // 0: never in layer A (actors, the player, the HUD, what reads a capture of the picture: indirect, crystal, bloom, glaring light, the 2D lists); 1: background by nature
    // (the skies: modelled round the camera, at infinity; the astro dome's); 2: background when far (Air and Sun too: an atmosphere shell or a sun can stand in front of other things) (by distance from the camera AND the player, nearest point of the shape's bounding sphere)
    const u8 kBgClass[kBgBufs] = {2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    const char* const kBgName[kBgBufs] = {"ClippedMapParts", "Sky", "Air", "Sun", "Planet", "PlanetLow", "Environment", "EnvironmentStrongLight", "MapObj",
                                          "MapObjWeakLight", "MapObjStrongLight", "NoShadowedMapObj", "NoShadowedMapObjStrongLight", "NoSilhouettedMapObj",
                                          "NoSilhouettedMapObjWeakLight", "NoSilhouettedMapObjStrongLight", "NPC", "Ride", "Enemy", "EnemyDecoration", "Player",
                                          "PlayerDecoration", "CrystalBox", "UNK17", "UNK18", "IndirectMapObj", "IndirectMapObjStrongLight", "IndirectNpc",
                                          "IndirectEnemy", "IndirectPlanet", "BloomModel", "TripodBoss", "Crystal", "CrystalItem", "GlaringLight", "AstroDomeSky",
                                          "Model3DFor2D", "UNK25", "UNK26", "MirrorMapObj", "UNK28"};

    struct BgAcc {
        double shapes[kBgBufs] = {};  // drawn shapes (not hidden, not culled)
        double groups[kBgBufs] = {};  // their draw calls (one per matrix group)
        double dlKB[kBgBufs] = {};    // their display list bytes (a proxy for vertices)
        double farShapes[kBgBufs][kBgBands] = {};
        double farDlKB[kBgBufs][kBgBands] = {};
        double models[kBgBufs] = {};  // distinct drawers' packets whose model is null or at the origin: unplaced (counted near)
    };
    BgAcc sBg;
    u32 sBgWalks = 0, sBgPics = 0, sBgDemoPics = 0, sBgSkipped = 0;
    int64_t sBgLast = 0;
    u32 sBgTick = 0;
    // camera
    bool sBgHave = false;
    float sBgPrevR[9], sBgPrevC[3], sBgPrev2C[3];
    bool sBgHave2 = false;
    // (heap36: were std::vector, pushed by the game thread every picture (GXV_BGLAYER=2) and only clear()ed at the stage's line: the storage came from the scene's heap and
    // was written again in the next scene)
    typedef PortHostVector< float > BgVec;
    BgVec sBgRot, sBgMove, sBgMove2;  // per picture: turn in pixels, travel in units (1 and 2 pictures)
    PORT_HEAP_GUARD(sBgRot);
    PORT_HEAP_GUARD(sBgMove);
    PORT_HEAP_GUARD(sBgMove2);
    u32 sBgCuts = 0;
    double sBgFocal = 650.0;

    float bgPct(BgVec& v, float q) {
        if (v.empty()) {
            return 0.0f;
        }
        std::sort(v.begin(), v.end());
        const size_t i = static_cast< size_t >(q * static_cast< float >(v.size() - 1) + 0.5f);
        return v[i];
    }

    void bgWalk(const DrawBufferHolder& rHolder, const float cam[3], const float* player) {
        const u32 cullPicture = J3DPortPicture();
        const s32 nGroups = std::min(rHolder.mBufferGroups.size(), static_cast< s32 >(kBgBufs));
        for (s32 b = 0; b < nGroups; ++b) {
            const DrawBufferGroup& group = rHolder.mBufferGroups[b];
            const s32 numExec = group.mActiveExecutors.size();
            for (s32 e = 0; e < numExec; ++e) {
                const DrawBuffer* buffer = group.mActiveExecutors[e]->mDrawBuffer;
                if (buffer->mNumActors <= 0) {
                    continue;
                }
                for (s32 d = 0; d < buffer->mNumShapeDrawers; ++d) {
                    const DrawBufferShapeDrawer* drawer = buffer->mShapeDrawers[d];
                    for (s32 i = 0; i < drawer->mNumPackets; ++i) {
                        const J3DShapePacket* sp = drawer->mPackets[i]->mShapePacket;
                        const J3DShape* shape = sp->mpShape;
                        if (shape == nullptr || (shape->mFlags & 1) != 0) {
                            continue;
                        }
                        const J3DModel* model = sp->getModel();
                        if (model != nullptr && model->mPortCullPicture == cullPicture) {
                            continue;
                        }
                        u32 dl = 0;
                        const u32 ng = shape->mMtxGroupNum;
                        for (u32 g = 0; g < ng; ++g) {
                            if (const J3DShapeDraw* sd = const_cast< J3DShape* >(shape)->getShapeDraw(static_cast< u16 >(g))) {
                                dl += sd->getDisplayListSize();
                            }
                        }
                        sBg.shapes[b] += 1.0;
                        sBg.groups[b] += ng;
                        sBg.dlKB[b] += dl / 1024.0;
                        if (kBgClass[b] != 2 || model == nullptr) {
                            continue;
                        }
                        // the shape's bounding sphere in the world: its centre through the model's base matrix (the actor's), radius x the largest scale
                        const f32 sx = model->mBaseScale.x, sy = model->mBaseScale.y, sz = model->mBaseScale.z;
                        const f32 lx = 0.5f * (shape->mMin.x + shape->mMax.x) * sx, ly = 0.5f * (shape->mMin.y + shape->mMax.y) * sy,
                                  lz = 0.5f * (shape->mMin.z + shape->mMax.z) * sz;
                        const Mtx& m = model->mBaseTransformMtx;
                        const float wx = m[0][0] * lx + m[0][1] * ly + m[0][2] * lz + m[0][3];
                        const float wy = m[1][0] * lx + m[1][1] * ly + m[1][2] * lz + m[1][3];
                        const float wz = m[2][0] * lx + m[2][1] * ly + m[2][2] * lz + m[2][3];
                        const float rad = shape->mRadius * std::max(std::fabs(sx), std::max(std::fabs(sy), std::fabs(sz)));
                        float dc = std::sqrt((wx - cam[0]) * (wx - cam[0]) + (wy - cam[1]) * (wy - cam[1]) + (wz - cam[2]) * (wz - cam[2])) - rad;
                        if (player != nullptr) {
                            const float dp = std::sqrt((wx - player[0]) * (wx - player[0]) + (wy - player[1]) * (wy - player[1]) + (wz - player[2]) * (wz - player[2])) - rad;
                            dc = std::min(dc, dp);
                        }
                        for (int k = 0; k < kBgBands; ++k) {
                            if (dc >= kBgBand[k]) {
                                sBg.farShapes[b][k] += 1.0;
                                sBg.farDlKB[b][k] += dl / 1024.0;
                            }
                        }
                    }
                }
            }
        }
    }

    void bgLog(const char* pStage) {
        const double n = static_cast< double >(sBgWalks);
        if (sBgWalks == 0) {
            return;
        }
        double tot = 0, totDl = 0, totGroups = 0, nat = 0, natDl = 0, natG = 0;
        double farS[kBgBands] = {}, farDl[kBgBands] = {}, elS = 0, elDl = 0;
        for (s32 b = 0; b < kBgBufs; ++b) {
            tot += sBg.shapes[b];
            totDl += sBg.dlKB[b];
            totGroups += sBg.groups[b];
            if (kBgClass[b] == 1) {
                nat += sBg.shapes[b];
                natDl += sBg.dlKB[b];
                natG += sBg.groups[b];
            } else if (kBgClass[b] == 2) {
                elS += sBg.shapes[b];
                elDl += sBg.dlKB[b];
                for (int k = 0; k < kBgBands; ++k) {
                    farS[k] += sBg.farShapes[b][k];
                    farDl[k] += sBg.farDlKB[b][k];
                }
            }
        }
        const double t = tot > 0 ? tot : 1.0, td = totDl > 0 ? totDl : 1.0;
        char far[420];
        size_t at = 0;
        far[0] = 0;
        for (int k = 0; k < kBgBands; ++k) {
            const int w = snprintf(far + at, sizeof far - at, "%s>=%.0f: +%.1f shapes (+%.1f%% shapes, +%.1f%% dl; layer A would be %.1f%% / %.1f%%)", k ? "; " : "", static_cast< double >(kBgBand[k]),
                                   farS[k] / n, 100.0 * farS[k] / t, 100.0 * farDl[k] / td, 100.0 * (nat + farS[k]) / t, 100.0 * (natDl + farDl[k]) / td);
            if (w < 0 || static_cast< size_t >(w) >= sizeof far - at) {
                break;
            }
            at += static_cast< size_t >(w);
        }
        // the categories that hold the most shapes
        char cats[300];
        size_t ca = 0;
        cats[0] = 0;
        bool used[kBgBufs] = {};
        for (int k = 0; k < 6; ++k) {
            s32 best = -1;
            for (s32 b = 0; b < kBgBufs; ++b) {
                if (!used[b] && sBg.shapes[b] > 0 && (best < 0 || sBg.shapes[b] > sBg.shapes[best])) {
                    best = b;
                }
            }
            if (best < 0) {
                break;
            }
            used[best] = true;
            const int w = snprintf(cats + ca, sizeof cats - ca, "%s%s%s %.1f (dl %.1fK)", k ? ", " : "", kBgName[best], kBgClass[best] == 1 ? "*" : "", sBg.shapes[best] / n,
                                   sBg.dlKB[best] / n);
            if (w < 0 || static_cast< size_t >(w) >= sizeof cats - ca) {
                break;
            }
            ca += static_cast< size_t >(w);
        }
        port_log_summary("bglayer: stage %s: %u pictures (%u walked, %u in demos): per picture %.1f shapes %.1f draw calls %.1f KB display lists; background by nature "
                         "(Sky, AstroDomeSky) %.1f shapes (%.1f%%, dl %.1f%%), %.1f calls; by distance, on top of that, from the camera AND the player (nearest "
                         "point of the shape's sphere), of %.1f eligible shapes (Planet/Environment/MapObj families): %s; shapes by category (* = nature): %s",
                         pStage, sBgPics, sBgWalks, sBgDemoPics, tot / n, totGroups / n, totDl / n, nat / n, 100.0 * nat / t, 100.0 * natDl / td, natG / n, elS / n, far, cats);
        BgVec r(sBgRot), m1(sBgMove), m2(sBgMove2);
        const double nr = static_cast< double >(r.size());
        double rm = 0, mm = 0;
        for (float v : r) {
            rm += v;
        }
        for (float v : m1) {
            mm += v;
        }
        const double f = sBgFocal;
        const float p95m = bgPct(m1, 0.95f), maxm = bgPct(m1, 1.0f), p95m2 = bgPct(m2, 0.95f);
        const double mmean = nr > 0 ? mm / nr : 0.0;
        // error of a stale far picture re-projected for the camera's turn (exact for any depth) but not for its travel: |T| f / D pixels at most (the point at 90 degrees to the travel)
        port_log_summary("bglayer-cam: stage %s: %u pictures, %u cuts (excluded): camera turn px/picture mean %.1f p95 %.1f max %.1f; travel units/picture mean %.1f p95 %.1f max %.1f "
                         "(over 2 pictures p95 %.1f); stale re-projected error (px, upper bound |T| f/D, f %.0f px) at D 5000: mean %.2f p95 %.2f (2 pictures %.2f), D 10000: mean %.2f "
                         "p95 %.2f max %.2f (2 pictures %.2f), D 20000: mean %.2f p95 %.2f",
                         pStage, static_cast< u32 >(r.size()), sBgCuts, nr > 0 ? rm / nr : 0.0, static_cast< double >(bgPct(r, 0.95f)), static_cast< double >(bgPct(r, 1.0f)), mmean,
                         static_cast< double >(p95m), static_cast< double >(maxm), static_cast< double >(p95m2), f, mmean * f / 5000.0, p95m * f / 5000.0, p95m2 * f / 5000.0,
                         mmean * f / 10000.0, p95m * f / 10000.0, maxm * f / 10000.0, p95m2 * f / 10000.0, mmean * f / 20000.0, p95m * f / 20000.0);
        sBg = BgAcc{};
        sBgWalks = sBgPics = sBgDemoPics = sBgCuts = 0;
        sBgRot.clear();
        sBgMove.clear();
        sBgMove2.clear();
    }
}  // namespace

// GameScene::draw3D, once per drawn picture (GXV_BGLAYER on): the view matrix (3x4, world to eye), the camera's position, the player's (nullptr: none), the vertical field of
// view as the game holds it, whether a demo is on. Stats of the camera every picture; the draw lists walked every 4th picture; one line of each every 5 s.
extern "C" void port_bglayer_picture(const void* pHolder, const float* view, const float* cam, const float* player, float fovy, int demo) {
    if (view == nullptr || cam == nullptr) {
        return;
    }
    ++sBgPics;
    if (demo != 0) {
        ++sBgDemoPics;
    }
    const float fov = fovy > 3.2f ? fovy * 0.017453292f : fovy;  // (the game holds degrees; radians would be under pi)
    if (fov > 0.05f && fov < 3.0f) {
        sBgFocal = 272.0 / std::tan(static_cast< double >(fov) * 0.5);
    }
    float R[9];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            R[r * 3 + c] = view[r * 4 + c];
        }
    }
    if (sBgHave) {
        float tr = 0.0f;
        for (int i = 0; i < 9; ++i) {
            tr += R[i] * sBgPrevR[i];
        }
        float cs = (tr - 1.0f) * 0.5f;
        cs = cs > 1.0f ? 1.0f : (cs < -1.0f ? -1.0f : cs);
        const float ang = std::acos(cs);
        const float dx = cam[0] - sBgPrevC[0], dy = cam[1] - sBgPrevC[1], dz = cam[2] - sBgPrevC[2];
        const float mv = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (ang > 0.35f || mv > 2500.0f) {
            ++sBgCuts;  // (a camera cut: not a smooth move; the far layer is re-made at a cut)
            sBgHave2 = false;
        } else {
            sBgRot.push_back(static_cast< float >(ang * sBgFocal));
            sBgMove.push_back(mv);
            if (sBgHave2) {
                const float ex = cam[0] - sBgPrev2C[0], ey = cam[1] - sBgPrev2C[1], ez = cam[2] - sBgPrev2C[2];
                sBgMove2.push_back(std::sqrt(ex * ex + ey * ey + ez * ez));
            }
            sBgHave2 = true;
        }
    }
    for (int i = 0; i < 9; ++i) {
        sBgPrevR[i] = R[i];
    }
    for (int i = 0; i < 3; ++i) {
        sBgPrev2C[i] = sBgPrevC[i];
        sBgPrevC[i] = cam[i];
    }
    sBgHave = true;
    if ((++sBgTick & 3u) == 0 && pHolder != nullptr) {
        ++sBgWalks;
        bgWalk(*static_cast< const DrawBufferHolder* >(pHolder), cam, player);
    }
    if (sBgPics >= 64 && (sBgPics & 15u) == 0) {
        const int64_t now = port_host_time_ns();
        if (sBgLast == 0) {
            sBgLast = now;
        } else if (now - sBgLast >= 5000000000ll) {
            sBgLast = now;
            bgLog(MR::getCurrentStageName());
        }
    }
}
#endif
