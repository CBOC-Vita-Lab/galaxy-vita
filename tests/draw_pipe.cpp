// GXV_DRAWPIPE (drawpipe24) on the host: the J3D passes of the draw buffers drawn from the lists frozen at the hand-over, in the
// serial order and on a second thread beside the next turn's update, against today's draw from the live lists; and the census /
// verify run. PRODUCTION code: Game/System/DrawBuffer.cpp, DrawBufferGroup.cpp, DrawBufferExecuter.cpp, DrawBufferHolder.cpp (the
// lists, the passes, portPipeFreeze / portPipeDraw / portPipeWalk), Game/LiveActor/ActorLightCtrl.cpp (the light sort and blend that
// movement runs), J3DPacket.cpp (J3DDisplayListObj's double buffer and callDL, J3DShapePacket::drawFast / prepareDraw) with the
// material path it links (J3DMaterial / J3DMatBlock / J3DTevs / J3DGD / J3DTransform / J3DStruct, GDBase.c, mtx.c) and
// port/draw_pipe.h's driver. Modelled here: the scene (model data, actors, light areas), the turn's phases as GameSystem::frameLoop
// orders them, and the GX library below the draw (every call the passes make is recorded, with the bytes it would send: that is
// the "GX stream" whose hash must not depend on the mode).
//
// A turn: [flushes: joints, the view pass into the other matrix half and the swap, some material / differed lists into their
// other buffer and the swap, hide flags, cull stamps] [hand-over] [draw: every buffer type's opaque pass, then every type's
// translucent pass] [update: actors shown / hidden / clipped, actors moving through light areas (ActorLightCtrl::update: a light
// sort, a blend), flag bytes] [calc-anim: joints]. What update writes here is what the code review found movement writes of the
// passes' read set (DESIGN.md section 1); what the flushes write is what only they may write.
//   draw_pipe <mode> <turns> <seed>     mode: 0 today's draw | 1 the frozen lists and the draw side's own J3D context, serial
//                                       (GXV_DRAWPIPE=1) | 2 the census / verify (GXV_DRAWPIPE=2) | 3 the whole draw on the draw
//                                       thread while the game thread waits (GXV_DRAWPIPE=3) | 4 the draw recorded on the game
//                                       thread and executed by the draw thread beside update, joined before calc-anim (GXV_DRAWPIPE=4)
//   prints one hash of the recorded GX stream per turn (the modes' outputs must be equal), then a line of counters on stderr
// dpengine (11 Nov): the draw is now GameScene::draw's shape: J3D passes between "open" draw lists that run on the game thread
// and read live game state (a shadow list reading the actors' positions; a model drawn by hand, which writes j3dSys and reads the
// J3D statics a pass leaves; the camera's view matrix into j3dSys; the player's light; the star pointer), and the update writes
// everything the hand-over freezes: the lists and lights as before, j3dSys (J3DModel::calc in movement), the camera, the
// player's and the point light, the scene / fog serial, the star pointer, the draw's branch flags. Modes 3 and 4 run the
// production engine (port/draw_thread.h: the thread, the hand-over, the join, the watchdog) over a recorder that stands for the
// GX capture layer (port/gx_capture.h's interface: every call the draw makes below the passes is recorded and executed later).
//   DP_NEG=live     (mode 4) the draw thread draws each pass from the live lists: ThreadSanitizer reports the race
//   DP_NEG=open     (mode 4) the whole draw, open lists included, runs on the draw thread beside update: the race that keeps the
//                   open lists on the game thread
//   DP_NEG=sys      (mode 4) the passes read j3dSys itself, not the draw side's J3DSys
//   DP_NEG=light    (mode 4) a pass loads the player's light from the live light, not the copy made at the hand-over
//   DP_NEG=cam      (mode 4) a pass's light and user-list loads read the live camera
//   DP_NEG=ext      (mode 4) a pass's brackets carry the live scene / fog serial
//   DP_NEG=guess    (modes 1, 3, 4) the token's guess of the J3D statics a pass leaves is not made: the model drawn by hand after
//                   the pass sends another stream, and the miss is counted
//   DP_NEG=midlist  (modes 1, 2, 4) a list changes between the hand-over and its pass: mode 1 must fall back to the live lists
//                   (epoch) and still equal mode 0; mode 2 must count the pass as differing; mode 4 must end the recording there
//                   (executed in order on the game thread) and still equal mode 0
//   DP_NEG=fur      (mode 4) the fur draw does not hand its shape's matrix-index words to the pipeline: a pass that stood before it
//                   is drawn with fur's words, and the stream differs
//   DP_NEG=furlent  (mode 4, with DP_ABANDON inside a fur draw) the fur draw does not say that it has pointed the packet at a
//                   layer model: a pass executed there draws the packet with the layer model
//   DP_NEG=keep     (mode 4) every token claims that its pass leaves the draw serial alone: the recorder must count wrong claims
//   DP_NEG=capall   (mode 4) the draw's begin and end inside the recording: every recording must end at once (no turn is handed over)
//   DP_NEG=endearly (mode 4) the draw's end made at the hand-over, not at the join: the stream's order must differ from mode 0's
//   DP_NEG=midsmall (mode 2) the draw itself changes the camera and the star pointer after the hand-over: the census must count it
//   DP_ABANDON=n    (mode 4) the recorder abandons the capture at its n-th call on every third turn (what it recorded is executed at
//                   once, the passes through the token handler): the stream must still equal mode 0's
//   DP_NOOPEN=1     (mode 4) the engine does not ask the recorder whether the capture is still open: after an abandon the
//                   recorder executes each further token itself, through the handler
//   DP_BUSY=1       (mode 4) the recorder refuses every fourth capture
//   DP_STALL=turn   (mode 4) the draw thread stalls for DP_STALL_MS (default 60) inside a pass of that turn: the join must wait,
//                   the watchdog (DP_WATCHDOG_MS, default 20 here) must turn the pipeline off, the stream must equal mode 0's
//   DP_UPLATE=us    (modes 3, 4) the draw thread reports in that late (after the starter has stopped waiting, as on the console's
//                   first run): it must still be used; until it runs, its jobs are taken back
//   DP_LATE=us      (mode 4) the draw thread starts every job that late: the game thread reaches the join first and takes the
//                   job back (after DP_MAXTAKEN in a row the pipeline goes off)
//   DP_OTHER=1      (mode 4) every third turn another thread asks to write while the draw thread has the job: it waits, the join is the game thread's
//   DP_NESTED=1     (mode 4) every fifth turn the draw thread is slow and, at the join's safe point, an interrupt handler uses GX:
//                   it joins the draw from inside the join (the outer join must notice and return, not wait for ever)
//   DP_LOGICGX=1    the update uses GX on every seventh turn (a write into the stream): in mode 4 it joins the draw first
//   DP_SERIAL=k     (modes 3, 4) every k-th turn does not qualify (a serial turn among the pipelined ones)
//   DP_DIFFSKIP=0/1, DP_MATMERGE=0/1: GXV_J3D_DIFFSKIP / GXV_J3D_MATMERGE as the passes see them (default 1 / 0)
//   DP_NATIVE=0/1: GXV_J3DNATIVE's brackets at the lists' calls (the drawer's list version and the scene serial are part of them)
//   -DDP_BASE: the same fixture over the base revision's DrawBuffer*.cpp (mode 0 only): the patched mode 0 must equal it
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/System/DrawBufferGroup.hpp"
#include "Game/System/DrawBufferHolder.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include "JSystem/J3DGraphAnimator/J3DJointTree.hpp"
#include "JSystem/J3DGraphAnimator/J3DModel.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphBase/J3DShape.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/J3DGraphBase/J3DSys.hpp"
#include "JSystem/J3DGraphBase/J3DTexture.hpp"
#include "JSystem/J3DGraphBase/J3DVertex.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "Game/System/PortDrawPipe.hpp"
#include "port/draw_pipe.h"
#include "port/draw_thread.h"

static const char* sNeg = "";
static bool Neg(const char* n) { return std::strcmp(sNeg, n) == 0; }

// ---- the recorded GX stream (one writer at a time: the thread that draws), and the recorder that stands for the GX capture
// layer (port/gx_capture.h): while a capture is open the draw's calls go into it instead of the stream; a pass is a token; the
// replay puts the calls into the stream in order and calls back at each token. An abandon executes what was recorded at once (the
// tokens through the registered handler) and from then on a token is executed where it stands.
static std::vector< unsigned char > gStream;
static uint32_t gDrawSerial = 0;  // (the GX library's draw serial: list calls and primitives; moved by the thread that draws)
namespace cap {
    struct Item {
        bool token;
        uint32_t id;
        std::vector< unsigned char > bytes;
        uint32_t keep = 0;
    };
    static uint32_t sKeepClaims = 0, sKeepWrong = 0;  // tokens that claimed to leave the draw serial alone; those whose pass moved it
    struct Capture {
        std::vector< Item > items;
    };
    static Capture* sOpen = nullptr;   // the open capture (the recording thread's)
    static bool sAbandoned = false;    // abandoned: tokens run at once until the end
    static void (*sHandler)(uint32_t, void*) = nullptr;
    static void* sHandlerCtx = nullptr;
    static uint32_t sCalls = 0, sBegun = 0, sAbandons = 0, sRefused = 0;
    static int sAbandonAt = 0, sTurn = 0;
    static bool sBusy = false;
    static void Run(Capture* c, void (*token)(uint32_t, void*), void* ctx) {
        for (Item& it : c->items) {
            if (it.token) {
                const uint32_t before = gDrawSerial;
                token(it.id, ctx);
                if (it.keep & port::drawpipe::kKeepSerial) {
                    ++sKeepClaims;
                    if (gDrawSerial != before) ++sKeepWrong;
                }
            } else {
                gStream.insert(gStream.end(), it.bytes.begin(), it.bytes.end());
            }
        }
        delete c;
    }
    static void Abandon(const char*) {
        if (sOpen == nullptr) return;
        Capture* c = sOpen;
        sOpen = nullptr;
        sAbandoned = true;
        ++sAbandons;
        Run(c, sHandler, sHandlerCtx);
    }
    static bool Begin() {
        ++sBegun;
        if (sBusy && sBegun % 4 == 0) {
            ++sRefused;
            return false;
        }
        sOpen = new Capture;
        sAbandoned = false;
        sCalls = 0;
        return true;
    }
    static void TokenKeep(uint32_t id, uint32_t keep) {
        if (sOpen == nullptr) {
            sHandler(id, sHandlerCtx);  // (abandoned earlier: the pass runs here, where it runs serially)
            return;
        }
        sOpen->items.push_back(Item{true, id, {}, keep});
    }
    static void Token(uint32_t id) { TokenKeep(id, 0); }
    static void* End() {
        Capture* c = sOpen;
        sOpen = nullptr;
        sAbandoned = false;
        return c;
    }
    static void Replay(void* c, void (*token)(uint32_t, void*), void* ctx) { Run(static_cast< Capture* >(c), token, ctx); }
    static void SetHandler(void (*token)(uint32_t, void*), void* ctx) { sHandler = token, sHandlerCtx = ctx; }
    static bool Open() { return sOpen != nullptr; }
}  // namespace cap
static void Rec(char tag, const void* p, size_t n) {
    if (cap::sOpen != nullptr) {
        // (DP_ABANDON: a call the layer cannot record ends the capture: what was recorded is executed first, then the call)
        if (cap::sAbandonAt != 0 && cap::sTurn % 3 == 0 && ++cap::sCalls == static_cast< uint32_t >(cap::sAbandonAt)) {
            cap::Abandon("a call that cannot be recorded");
        }
    }
    std::vector< unsigned char >* to = &gStream;
    if (cap::sOpen != nullptr) {
        if (cap::sOpen->items.empty() || cap::sOpen->items.back().token) cap::sOpen->items.push_back(cap::Item{false, 0, {}, 0});
        to = &cap::sOpen->items.back().bytes;
    }
    static const bool trace = getenv("DP_TRACE") != nullptr;  // (debugging: every record's place, tag and size)
    if (trace) fprintf(stderr, "@%zu %c %zu\n", to->size(), tag, n);
    to->push_back(static_cast< unsigned char >(tag));
    const unsigned char* b = static_cast< const unsigned char* >(p);
    to->insert(to->end(), b, b + n);
}
template < class T > static void RecV(char tag, const T& v) { Rec(tag, &v, sizeof v); }
// A GX call no recording can hold (the capture layer's abandon list: GXSetDispCopyYScale in endRender's copy, the draw-done calls):
// made while a capture is open it ends the capture, as the layer does; then it goes into the stream.
static uint32_t sHardInCapture = 0;
template < class T > static void RecHard(char tag, const T& v) {
    if (cap::sOpen != nullptr) {
        ++sHardInCapture;
        cap::Abandon("a call that cannot be recorded");
    }
    Rec(tag, &v, sizeof v);
}

static int sMode = 0;       // port_drawpipe_mode()
static bool sDiffSkip = true, sMatMerge = false;

// ---- what the production units link against below and beside the passes
bool J3DShapeMtx::sLODFlag;
// (J3DShapeMtx.cpp is not linked: the rest of the draw path's scratch statics, which the draw buffers' holder saves around a token)
u16 J3DShapeMtx::sMtxLoadCache[10];
MtxPtr J3DShapeMtxConcatView::sMtxPtrTbl[2];
u32 J3DShapeMtx::sCurrentPipeline;
u8* J3DShapeMtx::sCurrentScaleFlag;
bool J3DShapeMtx::sNBTFlag;
u32 J3DShapeMtx::sTexMtxLoadType;
void* J3DShape::sOldVcdVatCmd;
bool J3DShape::sEnvelopeFlag;
J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6];
J3DTexGenBlock* J3DDifferedTexMtx::sTexGenBlock;
J3DTexMtxObj* J3DDifferedTexMtx::sTexMtxObj;
J3DTexCoordScaleInfo J3DSys::sTexCoordScaleTable[8];
Mtx J3DMtxBuffer::sNoUseDrawMtx;
Mtx33 J3DMtxBuffer::sNoUseNrmMtx;
Mtx* J3DMtxBuffer::sNoUseDrawMtxPtr = &J3DMtxBuffer::sNoUseDrawMtx;
Mtx33* J3DMtxBuffer::sNoUseNrmMtxPtr = &J3DMtxBuffer::sNoUseNrmMtx;
void J3DPacket::draw() {}
void J3DModelData::syncJ3DSysFlags() const {}
void J3DVertexBuffer::setArray() const { J3D_DSYS.setVtxPos(const_cast< J3DVertexBuffer* >(this)); }  // (as the real one: into the draw path's J3DSys)
J3DSys::J3DSys() {}
J3DSys j3dSys;
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
static uint8_t sFifo[1 << 16];
struct Scene;
static Scene* gScene;
static int ShapeId(const J3DShape* shape);
static int ModelId(const J3DModel* model);

// ---- the live small state the update writes (the game's camera, light director, fog, star pointer, draw branches)
struct Live {
    TPos3f view, invView;
    Mtx44 projection;
    f32 pose[8];
    ActorLightInfo playerLight;
    PointLightInfo pointLight;
    u32 pointer[4];
    u32 branch;
    u32 extPlain;  // (a plain copy of the scene serial, for DP_NEG=ext's reader)
};
static Live gLive;
static const TPos3f& CamView() { return gPortPipeScene != nullptr && !Neg("cam") ? gPortPipeScene->mView : gLive.view; }
static const TPos3f& CamInvView() { return gPortPipeScene != nullptr && !Neg("cam") ? gPortPipeScene->mInvView : gLive.invView; }

// (the first shape of a drawer: its vertex-format list, when it is not the last one sent, and its matrix index: J3DShape.cpp)
void J3DShape::loadPreDrawSetting() const {
    if (sOldVcdVatCmd != mVcdVatCmd) {
        RecV('V', ShapeId(this));
        sOldVcdVatCmd = mVcdVatCmd;
    }
    RecV('P', ShapeId(this));
}
// (a shape: J3DShape::drawFast's use of the two statics, what it reads of the packet's set-up and of the draw path's J3DSys
// (the model, the flags, the view matrix through the packet's base matrix), prepareDraw's writes into the shape, and the
// matrices the front end would fetch through the array pointers it sends)
void J3DShape::drawFast() const {
    ++gDrawSerial;  // (the shape's own list call)
    if (sOldVcdVatCmd != mVcdVatCmd) {
        RecV('V', ShapeId(this));
        sOldVcdVatCmd = mVcdVatCmd;
    }
    if (sEnvelopeFlag && !mHasPNMTXIdx) {
        RecV('M', ShapeId(this));
    }
    sEnvelopeFlag = mHasPNMTXIdx;
    J3DSys& sys = Neg("sys") ? j3dSys : J3D_DSYS;
    RecV('S', ShapeId(this));
    RecV('m', ModelId(sys.getModel()));
    RecV('f', mFlags);
    RecV('F', sys.mFlags);
    Rec('w', sys.mViewMtx, sizeof(Mtx));
    if (J3DShapePacket* packet = sys.getShapePacket()) {
        if (Mtx* base = packet->getBaseMtxPtr()) {
            Rec('B', Neg("sys") ? *base : *PortBaseMtx(base), sizeof(Mtx));
        }
    }
    const u32 view = *mCurrentViewNo;
    sys.mModelDrawMtx = mDrawMtx[view];  // (J3DShape::setArrayAndBindPipeline: the two words a shape leaves in its J3DSys)
    sys.mModelNrmMtx = mNrmMtx[view];
    Rec('d', mDrawMtx[view], sizeof(Mtx));
    Rec('n', mNrmMtx[view], sizeof(Mtx33));
    Rec('c', &mCurrentMtx, sizeof mCurrentMtx);
}
// (J3DShape.cpp is not linked: its other virtual functions, for the shapes' vtable, and the constructor's set-up)
void J3DShape::draw() const {}
void J3DShape::simpleDraw() const {}
void J3DShape::simpleDrawCache() const {}
void J3DShape::initialize() {
    mMaterial = nullptr;
    mIndex = 0xFFFF;
    mFlags = 0;
    mHasNBT = false;
    mCurrentMtx.mMtxIdxRegA = 0;
    mCurrentMtx.mMtxIdxRegB = 0;
}
extern "C" {
void __PortGXFifoOverflow(void) { __PortGXFifoPtr = sFifo; }
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void GXCallDisplayList(const void* list, u32 size) {
    ++gDrawSerial;
    RecV('C', size);
    Rec('b', list, size);
}
void port_gx_call_prologue(void) { gStream.push_back('p'); }
void port_gx_call_bracketed(const void* list, u32 nbytes, u32, u32 verA, u32 verB, u32 ext, u32 flags) {
    ++gDrawSerial;
    RecV('B', nbytes);
    Rec('b', list, nbytes);
    RecV('v', verA);
    RecV('w', verB);
    RecV('x', Neg("ext") && gPortPipeScene != nullptr ? gLive.extPlain : ext);  // (the scene / fog / fade serial)
    RecV('g', flags);
}
int __PortGXJ3nMode(void) { return getenv("DP_NATIVE") ? atoi(getenv("DP_NATIVE")) : 0; }
// (GXDisplayList.c's census of the unbracketed list calls, which J3DPacket.cpp logs: none here)
int __PortJ3nEmitOn;
int __PortJ3nEmitTake(void*, int, u32* lost) {
    *lost = 0;
    return 0;
}
uint32_t port_host_to_phys(const void* p) { return static_cast< uint32_t >(reinterpret_cast< uintptr_t >(p)); }
int64_t port_host_time_ns(void) {
    return std::chrono::duration_cast< std::chrono::nanoseconds >(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void port_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
void port_mtx_profile_begin(void) {}
void port_mtx_profile_end(void) {}
int port_drawpipe_mode(void) { return sMode; }
// (the light-dedup scopes: GX-library state, so calls of the stream like any other; the capture layer records them)
int port_light_scope_enter(int level) {
    RecV('E', level);
    return 1;
}
void port_light_scope_exit(int opened) { RecV('X', opened); }
BOOL OSDisableInterrupts() { return 1; }
BOOL OSRestoreInterrupts(BOOL) { return 1; }
}
void* operator new[](size_t size, int align) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast< size_t >(align < 0 ? -align : align), size) != 0) abort();
    std::memset(p, 0, size);
    return p;
}
// (GXV_J3D_MATMERGE's block comes from the drawer's guest heap: none here, so a drawer that wants one stays unmerged)
JKRHeap* JKRHeap::findFromRoot(void*) { return nullptr; }
void* JKRHeap::alloc(u32, int, JKRHeap*) { return nullptr; }

// ---- the scene
enum { kTypes = 5, kData = 7, kMaxMat = 4, kActors = 56, kAreas = 4, kJointN = 3 };
struct Data {
    J3DModelData* data;
    J3DMaterial* mat[kMaxMat];
    J3DShape* shape[kMaxMat];
    const char* matName[kMaxMat];
    int numMat, type;
    char name[16];
};
struct Actor {
    LiveActor* actor;  // (raw storage of a LiveActor's size: the passes and the light controller read its flags and position only)
    J3DModel* model;
    ActorLightCtrl* light;
    int data, exec;
    bool shown, user;
    ShapePacketUserData userData;
    Mtx base;
};
struct Scene {
    Data data[kData];
    Actor actor[kActors];
    AreaLightInfo area[kAreas];
    DrawBufferHolder holder;
    u32 rng;
};
static int ShapeId(const J3DShape* shape) {
    for (int d = 0; d < kData; ++d)
        for (int m = 0; m < gScene->data[d].numMat; ++m)
            if (gScene->data[d].shape[m] == shape) return d * 16 + m;
    return -1;
}
enum { kFurLayers = 2 };
static J3DModel* sFurLayer[kActors][kFurLayers];  // (FurCtrl's layer models: J3DModel2, sharing the actor's model's packets and matrix buffer)
static int ModelId(const J3DModel* model) {
    for (int a = 0; a < kActors; ++a) {
        if (gScene->actor[a].model == model) return a;
        for (int l = 0; l < kFurLayers; ++l)
            if (sFurLayer[a][l] != nullptr && sFurLayer[a][l] == model) return 500 + a * 4 + l;
    }
    return -1;
}
// (a pointer the draw leaves in j3dSys or in a J3D static, as a number that is the same in every run)
static int PtrId(const void* p) {
    if (p == nullptr) return -1;
    for (int a = 0; a < kActors; ++a) {
        const Actor& A = gScene->actor[a];
        if (p == &A.model->mVertexBuffer) return 1000 + a;
        for (int l = 0; l < kFurLayers; ++l) {
            if (sFurLayer[a][l] != nullptr && p == &sFurLayer[a][l]->mVertexBuffer) return 1500 + a * 4 + l;
        }
        for (int m = 0; m < kMaxMat; ++m) {
            if (p == &A.model->mShapePacket[m]) return 2000 + a * 8 + m;
        }
        for (int h = 0; h < 2; ++h) {
            if (p == A.model->mMtxBuffer->mpDrawMtxArr[h][0]) return 4000 + a * 2 + h;
        }
    }
    for (int d = 0; d < kData; ++d) {
        for (int m = 0; m < gScene->data[d].numMat; ++m) {
            if (p == gScene->data[d].shape[m]->mVcdVatCmd) return 6000 + d * 8 + m;
        }
    }
    return -2;
}
static Actor* ActorOf(const LiveActor* p) {
    for (int a = 0; a < kActors; ++a)
        if (gScene->actor[a].actor == p) return &gScene->actor[a];
    abort();
}
static u32 Rnd(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// ---- the game's utilities the units call (ModelUtil / LiveActorUtil / LightUtil / StringUtil are not linked)
namespace MR {
    J3DModel* getJ3DModel(const LiveActor* pActor) { return ActorOf(pActor)->model; }
    ActorLightCtrl* getLightCtrl(const LiveActor* pActor) { return ActorOf(pActor)->light; }
    const char* getModelResName(const LiveActor* pActor) { return gScene->data[ActorOf(pActor)->data].name; }
    bool isEqualString(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
    s32 getMaterialNum(J3DModel* pModel) { return pModel->mModelData->mMaterialTable.mMaterialNum; }
    J3DMaterial* getMaterial(J3DModel* pModel, int idx) { return pModel->mModelData->mMaterialTable.getMaterialNodePointer(static_cast< u16 >(idx)); }
    const char* getMaterialName(const J3DModelData* pData, int idx) {
        for (int d = 0; d < kData; ++d)
            if (gScene->data[d].data == pData) return gScene->data[d].matName[idx];
        abort();
    }
    bool isHiddenModel(const LiveActor* pActor) { return pActor->mFlag.mIsHiddenModel; }
    void initActorLightInfoLightType(LiveActor* pActor, s32 type) { ActorOf(pActor)->light->_4 = type; }
    void initActorLightInfoDrawBuffer(LiveActor* pActor, DrawBuffer* pBuffer) { ActorOf(pActor)->light->_8 = pBuffer; }
    void findActorLightInfo(const LiveActor* pActor) {
        Actor* a = ActorOf(pActor);
        gScene->holder.findLightInfo(a->actor, gScene->data[a->data].type, a->exec);
    }
    // (MR::loadLight: the player's type loads the player's light and the point light, LightUtil.cpp: in a frozen pass from the
    // copies made at the hand-over; every type's light positions go through the camera's view matrix, LightFunction.cpp)
    void loadLight(s32 type) {
        RecV('T', type);
        if (gPortPipeScene != nullptr && !Neg("light")) {
            Rec('l', &gPortPipeScene->mPlayerLightInfo, sizeof(ActorLightInfo));
            Rec('q', &gPortPipeScene->mPointLight, sizeof(PointLightInfo));
        } else {
            Rec('l', &gLive.playerLight, sizeof(ActorLightInfo));
            Rec('q', &gLive.pointLight, sizeof(PointLightInfo));
        }
        Rec('k', &CamView(), sizeof(TPos3f));
    }
    ShapePacketUserData* getJ3DShapePacketUserData(const J3DShapePacket* pPacket) { return static_cast< ShapePacketUserData* >(pPacket->mpUserArea); }
    void zeroMemory(void* p, u32 n) { std::memset(p, 0, n); }
}  // namespace MR
ZoneLightID::ZoneLightID() { _0 = -1, mLightID = -1; }
void ZoneLightID::clear() { _0 = -1, mLightID = -1; }
s32 JKRHeap::getFreeSize() { return 0; }
extern "C" {
int port_flush_prefetch_on(void) { return 0; }
void port_prefetch_actor(const void*) {}
void port_prefetch_model(const void*) {}
}
ShapePacketUserData::ShapePacketUserData() {}
// (the user list, and its texture-matrix loads' use of the camera's inverse view matrix: ShapePacketUserData::loadTexMtx)
void ShapePacketUserData::callDL() const {
    Rec('U', mDisplayList, mDisplayListSize);
    Rec('i', &CamInvView(), sizeof(TPos3f));
}

// (light areas: bands of x, 500 wide; the actor's light changes when it crosses into another band)
bool LightFunction::tryFindNewAreaLightID(const TVec3f& rPos, ZoneLightID* pID) {
    int band = static_cast< int >(std::floor(rPos.x / 500.0f));
    band = ((band % kAreas) + kAreas) % kAreas;
    if (pID->_0 == 0 && pID->mLightID == band) return false;
    pID->_0 = 0;
    pID->mLightID = band;
    return true;
}
AreaLightInfo* LightFunction::getAreaLightInfo(const ZoneLightID& rID) { return &gScene->area[((rID.mLightID % kAreas) + kAreas) % kAreas]; }
void LightFunction::blendActorLightInfo(ActorLightInfo* pOut, const ActorLightInfo& rA, const ActorLightInfo& rB, f32 t) {
    *pOut = rB;
    pOut->mInfo0.mColor.r = static_cast< u8 >(rA.mInfo0.mColor.r + (rB.mInfo0.mColor.r - rA.mInfo0.mColor.r) * t);
    pOut->mInfo1.mColor.g = static_cast< u8 >(rA.mInfo1.mColor.g + (rB.mInfo1.mColor.g - rA.mInfo1.mColor.g) * t);
    pOut->mAlpha2 = static_cast< u8 >(rA.mAlpha2 + (rB.mAlpha2 - rA.mAlpha2) * t);
}
void LightFunction::loadActorLightInfo(const ActorLightInfo* pInfo) {
    Rec('L', pInfo, sizeof(ActorLightInfo));
    Rec('k', &CamView(), sizeof(TPos3f));  // (the light's position in view space)
}
s32 LightFunction::getDefaultStepInterpolate() { return 6; }

// ---- building it
// A list's bytes: plain BP register writes (TEV colour registers) and NOPs, 32 bytes a block, so that the game's own scan
// (J3DPortScanDL) finds them pure and GXV_J3D_DIFFSKIP / MATMERGE treat them as they treat the game's lists. `variant` picks the
// values: instances of a model often hold equal differed lists (a stopped animation), which is what DIFFSKIP leaves out.
static void FillList(u8* b, u32 size, u32 variant) {
    for (u32 at = 0; at + 32 <= size; at += 32) {
        for (u32 k = 0; k < 6; ++k) {
            u8* c = b + at + 5 * k;
            c[0] = 0x61;
            c[1] = static_cast< u8 >(0xE0 + k);
            c[2] = static_cast< u8 >(variant);
            c[3] = static_cast< u8 >(variant * 7u + k);
            c[4] = static_cast< u8 >(at >> 5);
        }
        b[at + 30] = b[at + 31] = 0;
    }
}
static J3DDisplayListObj* NewList(u32& rng, u32 size) {
    J3DDisplayListObj* dl = static_cast< J3DDisplayListObj* >(calloc(1, sizeof(J3DDisplayListObj)));
    const u32 variant = Rnd(rng) % 3;
    for (int i = 0; i < 2; ++i) {
        void* p = nullptr;
        if (posix_memalign(&p, 32, size) != 0) abort();
        dl->mpDisplayList[i] = p;
        FillList(static_cast< u8* >(p), size, variant);
    }
    dl->mSize = size;
    dl->mMaxSize = size;
    return dl;
}
static void Build(Scene& s, u32 seed) {
    gScene = &s;
    s.rng = seed * 2654435761u + 12345u;
    // (one buffer type more than the models use: GameScene::draw asks for many passes whose type has no model in the stage)
    static const DrawBufferInitialTable table[kTypes + 2] = {{0, 8, 0, 0}, {1, 8, 1, 0}, {2, 8, 2, 0}, {3, 8, 3, 0}, {4, 8, 0, 0}, {5, 8, 0, 0}, {0xFFFFFFFFu, 0, 0, 0}};
    s.holder.initTable(table, kTypes + 1);
    for (int i = 0; i < kAreas; ++i) {
        AreaLightInfo& a = s.area[i];
        std::memset(&a, 0, sizeof a);
        a.mInterpolate = i == 1 ? 0 : 5;
        ActorLightInfo* l[4] = {&a.mPlayerLight, &a.mStrongLight, &a.mWeakLight, &a.mPlanetLight};
        for (int k = 0; k < 4; ++k) {
            l[k]->mInfo0.mColor.r = static_cast< u8 >(40 * i + 10 * k);
            l[k]->mInfo1.mColor.g = static_cast< u8 >(30 * i + 7 * k);
            l[k]->mAlpha2 = static_cast< u8 >(50 * i + k);
        }
    }
    for (int d = 0; d < kData; ++d) {
        Data& D = s.data[d];
        snprintf(D.name, sizeof D.name, "Model%d", d);
        D.type = d % kTypes;
        D.numMat = 2 + d % 3;
        D.data = static_cast< J3DModelData* >(calloc(1, sizeof(J3DModelData)));
        D.data->mFlags = (d % 2) ? 0x20u : 0u;  // (the post-texture-matrix flag prepareDraw copies into the draw path's J3DSys)
        J3DMaterialTable& t = D.data->mMaterialTable;
        t.mMaterialNum = static_cast< u16 >(D.numMat);
        t.mUniqueMatNum = static_cast< u16 >(D.numMat);
        t.mMaterialNodePointer = static_cast< J3DMaterial** >(calloc(kMaxMat, sizeof(J3DMaterial*)));
        static const char* const names[kMaxMat] = {"mat_c", "Mat_a", "mat_d", "mat_b"};  // (sorted by name at the table's set-up)
        for (int m = 0; m < D.numMat; ++m) {
            J3DMaterial* mat = static_cast< J3DMaterial* >(calloc(1, sizeof(J3DMaterial)));
            J3DShape* shape = new (calloc(1, sizeof(J3DShape))) J3DShape();
            shape->mIndex = static_cast< u16 >(m);
            shape->mMaterial = mat;
            shape->mVcdVatCmd = static_cast< u8* >(calloc(1, 32));  // (its vertex-format list: an identity for the "last one sent" static)
            shape->mHasPNMTXIdx = (d + m) % 3 == 0;                  // (a matrix index per vertex: the "envelope" static)
            mat->mShape = shape;
            mat->mIndex = static_cast< u16 >(m);
            mat->mMaterialMode = (m == D.numMat - 1 && d % 2) ? 0 : 1;  // (the last material of the odd data is translucent)
            mat->mDiffFlag = 0x1000u * static_cast< u32 >(m + 1) + static_cast< u32 >(d);
            t.mMaterialNodePointer[m] = mat;
            D.mat[m] = mat;
            D.shape[m] = shape;
            D.matName[m] = names[m];
        }
        J3DJointTree& jt = D.data->mJointTree;
        jt.mJointNum = kJointN;
        jt.mWEvlpMtxNum = 0;
        jt.mDrawMtxData.mEntryNum = kJointN;
    }
    for (int a = 0; a < kActors; ++a) {
        Actor& A = s.actor[a];
        A.data = a % kData;
        const Data& D = s.data[A.data];
        A.actor = static_cast< LiveActor* >(calloc(1, sizeof(LiveActor)));
        A.actor->mPosition.x = static_cast< f32 >(Rnd(s.rng) % 2000);
        A.model = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
        A.model->mModelData = D.data;
        J3DMtxBuffer* mb = static_cast< J3DMtxBuffer* >(calloc(1, sizeof(J3DMtxBuffer)));
        mb->mJointTree = &D.data->mJointTree;
        mb->mpAnmMtx = static_cast< Mtx* >(calloc(kJointN, sizeof(Mtx)));
        mb->mpScaleFlagArr = static_cast< u8* >(calloc(kJointN, 1));
        for (int h = 0; h < 2; ++h) {
            mb->mpDrawMtxArr[h] = static_cast< Mtx** >(calloc(1, sizeof(Mtx*)));
            mb->mpDrawMtxArr[h][0] = static_cast< Mtx* >(calloc(kJointN, sizeof(Mtx)));
            mb->mpNrmMtxArr[h] = static_cast< Mtx33** >(calloc(1, sizeof(Mtx33*)));
            mb->mpNrmMtxArr[h][0] = static_cast< Mtx33* >(calloc(kJointN, sizeof(Mtx33)));
        }
        A.model->mMtxBuffer = mb;
        A.model->mMatPacket = new J3DMatPacket[kMaxMat];
        A.model->mShapePacket = new J3DShapePacket[kMaxMat];
        A.user = a % 9 == 4;
        for (int m = 0; m < D.numMat; ++m) {
            A.model->mMatPacket[m].mpDisplayListObj = NewList(s.rng, 64);
            J3DShapePacket& sp = A.model->mShapePacket[m];
            sp.mpShape = D.shape[m];
            sp.mpMtxBuffer = mb;
            sp.mpModel = A.model;
            // (the base matrix: the model's own, or the view matrix INSIDE j3dSys, as J3DModel::prepareShapePackets sets it)
            sp.mpBaseMtxPtr = (a % 3 == 0) ? reinterpret_cast< Mtx* >(j3dSys.mViewMtx) : &A.base;
            sp.mpDisplayListObj = (a + m) % 5 == 3 ? nullptr : NewList(s.rng, 32 + 32 * static_cast< u32 >((a + m) % 2));
            if (A.user && m == 0) {
                A.userData.mDisplayListSize = 16;
                A.userData.mDisplayList = static_cast< u8* >(calloc(1, 16));
                A.userData.mDisplayList[3] = static_cast< u8 >(a);
                sp.mpUserArea = &A.userData;
            }
        }
        A.light = new ActorLightCtrl(A.actor);
        A.exec = s.holder.registerDrawBuffer(A.actor, D.type);
    }
    s.holder.allocateActorListBuffer();
    for (int a = 0; a < kActors; ++a) {
        Actor& A = s.actor[a];
        A.light->init(a % 7 == 2 ? 2 : -1, false);  // (some actors keep a fixed light: _C, never sorted)
        A.shown = false;
    }
}

// ---- the turn's phases
static void Show(Scene& s, int a, bool on) {
    Actor& A = s.actor[a];
    if (A.shown == on) return;
    A.shown = on;
    if (on) {
        s.holder.active(A.actor, s.data[A.data].type, A.exec);
    } else {
        s.holder.deactive(A.actor, s.data[A.data].type, A.exec);
    }
}
// update(): what movement writes of the passes' read set, and of the small state the hand-over freezes.
static bool sLogicGx = false;
static void (*sGameEnter)() = nullptr;  // (mode 4: the logic's GX use joins the draw thread first)
static void Update(Scene& s, int turn) {
    const int n = turn == 0 ? kActors : 3 + static_cast< int >(Rnd(s.rng) % 4);
    for (int i = 0; i < n; ++i) {
        const int a = turn == 0 ? i : static_cast< int >(Rnd(s.rng) % kActors);
        Show(s, a, turn == 0 ? (a % 4 != 3) : (Rnd(s.rng) % 3 != 0));  // (show / hide / clipping: DrawBufferHolder::active / deactive)
    }
    for (int a = 0; a < kActors; ++a) {
        Actor& A = s.actor[a];
        if (a % 3 == turn % 3) A.actor->mPosition.x += static_cast< f32 >(Rnd(s.rng) % 180);  // (moving through the light areas)
        A.actor->mFlag.mIsClipped = !A.shown;
        A.actor->mFlag.mIsHiddenModel = (Rnd(s.rng) % 23) == 0;
        if (A.shown) A.light->update(false);  // (ActorLightCtrl::update: tryFindNewAreaLight -> resetLightSort, updateLightBlend)
        if (a % 11 == turn % 11) {
            // (an actor that calcs its model in movement: J3DModel::calc writes j3dSys's model and skin flags)
            j3dSys.setModel(A.model);
            if (a & 1) j3dSys.onFlag(J3DSysFlag_SkinPosCpu); else j3dSys.offFlag(J3DSysFlag_SkinPosCpu);
        }
    }
    // the camera (CameraDirector::movement), the player's light and the point light (ActorLightCtrl::update, LightPointCtrl::update),
    // the fog (FogCtrl::update bumps the scene serial when it changes), the star pointer (the pad read), the draw's branches
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            gLive.view.mMtx[r][c] = static_cast< f32 >(turn * 3 + r * 4 + c) * 0.25f;
            gLive.invView.mMtx[r][c] = -gLive.view.mMtx[r][c];
        }
    }
    gLive.projection[0][0] = 1.0f + static_cast< f32 >(turn % 5);
    gLive.pose[turn % 8] = static_cast< f32 >(turn);
    gLive.playerLight.mInfo0.mColor.r = static_cast< u8 >(turn * 7);
    gLive.playerLight.mAlpha2 = static_cast< u8 >(turn);
    gLive.pointLight.mRefDistance = static_cast< f32 >(turn % 13);
    if (turn % 3 == 1) {
        J3DPortVerBump(J3DPortVer_Fog);
    }
    gLive.extPlain = J3DPortExtSerial();
    // (what the passes ask of a packet's model and shape, written in movement: the zoom cull's stamp for the picture AFTER the one
    // being drawn (ClippingActorHolder.cpp), and a material hidden or shown)
    for (int a = 0; a < kActors; a += 2) {
        s.actor[a].model->mPortCullPicture = (Rnd(s.rng) % 7) == 0 ? J3DPortPicture() + 1 : 0;
    }
    if (turn > 0 && (Rnd(s.rng) % 3) == 0) {
        // (MR::hideMaterial / showMaterial in movement: the packet's own hide flag)
        Actor& A = s.actor[Rnd(s.rng) % kActors];
        A.model->mShapePacket[Rnd(s.rng) % static_cast< u32 >(s.data[A.data].numMat)].mFlags ^= 0x10;
    }
    gLive.pointer[0] = static_cast< u32 >(turn % 3);
    gLive.pointer[1] = Rnd(s.rng);
    gLive.branch = static_cast< u32 >(turn % 2);
    if (sLogicGx && turn % 7 == 3) {
        // (the logic uses GX: a draw still out on the draw thread is joined first; the write then lands after the draw's stream)
        if (sGameEnter) sGameEnter();
        RecV('G', turn);
    }
}
static void CalcAnim(Scene& s) {
    for (int a = 0; a < kActors; ++a) {
        J3DMtxBuffer* mb = s.actor[a].model->mMtxBuffer;
        for (int j = 0; j < kJointN; ++j) mb->mpAnmMtx[j][0][3] += 1.0f + static_cast< f32 >(a);
    }
}
// The drawn turn's flushes: the only writers of the double buffers (and, here, of the shapes' hide bits and the cull stamps).
static void Flushes(Scene& s, int turn) {
    for (int a = 0; a < kActors; ++a) {
        Actor& A = s.actor[a];
        J3DMtxBuffer* mb = A.model->mMtxBuffer;
        mb->swapDrawMtx();
        mb->swapNrmMtx();
        for (int j = 0; j < kJointN; ++j) {
            std::memcpy(mb->getDrawMtxPtr()[j], mb->mpAnmMtx[j], sizeof(Mtx));
            mb->getDrawMtxPtr()[j][1][3] = static_cast< f32 >(turn);
            mb->getNrmMtxPtr()[j][0][0] = mb->mpAnmMtx[j][0][3] * 0.5f;
        }
        A.base[0][3] = static_cast< f32 >(turn + a);
        if (a % 2) {
            // (the flush's cull stamp, GXV_PLANT_DRAWCULL: the coming picture's number; the other half keep the update's, below)
            A.model->mPortCullPicture = (Rnd(s.rng) % 11) == 0 ? J3DPortPicture() + 1 : 0;
        }
        for (int m = 0; m < s.data[A.data].numMat; ++m) {
            J3DDisplayListObj* lists[2] = {A.model->mMatPacket[m].mpDisplayListObj, A.model->mShapePacket[m].mpDisplayListObj};
            for (int k = 0; k < 2; ++k) {
                J3DDisplayListObj* dl = lists[k];
                if (dl == nullptr || (Rnd(s.rng) % 4) != 0) continue;
                // (an animated material: the other buffer written, then the swap, as J3DMaterial::diff does)
                FillList(static_cast< u8* >(dl->mpDisplayList[1]), dl->mSize, Rnd(s.rng) % 3);
                dl->swapBuffer();
            }
        }
    }
    for (int d = 0; d < kData; ++d) {
        for (int m = 0; m < s.data[d].numMat; ++m) {
            J3DShape* shape = s.data[d].shape[m];
            if ((Rnd(s.rng) % 13) == 0) shape->mFlags ^= 1;  // (hidden / shown)
            // (the material's calc sets its shape's matrix-index words when its list is made again: some turns, not all, so
            // that a fur draw finds its own values of the last picture in some shapes and the material's in others)
            if ((Rnd(s.rng) % 3) != 0) shape->mCurrentMtx.mMtxIdxRegA = Rnd(s.rng);
        }
    }
}

// ---- the draw, in GameScene::draw's shape: J3D passes between open draw lists. The open lists run on the thread that makes the
// draw's calls (the game thread, also while the calls are recorded) and read LIVE game state.
static volatile f32 sOpenSink;
static int sCurType = 0;       // (the pass the draw is asking for: DP_NEG=live's side table)
static bool sCurXlu = false;
static int sDrawTurn = 0;
static void OpenView() {
    // (MR::loadViewMtx: the camera's view matrix into j3dSys, for the passes and the particles after it)
    PSMTXCopy(*gLive.view.toCMtx(), j3dSys.mViewMtx);
    Rec('c', &gLive.projection, sizeof gLive.projection);  // (MR::loadProjectionMtx)
}
static void OpenShadows(Scene& s) {
    // (a shadow list: each controller's own state)
    f32 sum = 0.0f;
    for (int a = 0; a < kActors; ++a) sum += s.actor[a].actor->mPosition.x;
    sOpenSink = sum;
    RecV('H', sum);
}
static void OpenHandModel(Scene& s, int which) {
    // (a model drawn by hand, as Mario's or a SimpleJ3DModelDrawer's: its own view matrix and model into j3dSys, its shape through
    // the J3D statics the pass before it left, the camera's view matrix back afterwards)
    Actor& A = s.actor[which % kActors];
    Mtx shadowView;
    PSMTXCopy(*gLive.view.toCMtx(), shadowView);
    shadowView[2][3] += 100.0f;
    PSMTXCopy(shadowView, j3dSys.mViewMtx);
    J3DShapePacket& packet = A.model->mShapePacket[0];
    J3DShape* shape = packet.mpShape;
    packet.prepareDraw();  // (as J3DModelX::shapePacketDrawFast and FurCtrl::drawFur do: the model, the packet, the flags into j3dSys)
    shape->loadPreDrawSetting();
    shape->drawFast();
    PSMTXCopy(*gLive.view.toCMtx(), j3dSys.mViewMtx);
}
// A fur draw (FurCtrl::drawFur): setters, then the model's shape drawn once per layer through layer models that share the
// model's shape packet (the packet is pointed at each layer model and back at the end), the shape's matrix-index words written
// to fur's own values and LEFT there, the vertex-format static cleared. The actor's model is in the draw buffers too: a pass
// before this draw must see the words and the packet as they were, a pass after it the words as fur left them.
#ifdef DP_BASE
extern "C" void port_drawpipe_shape_words(void*, u32, u32) {}
extern "C" void port_drawpipe_packet_lent(void*, void*) {}
#else
extern "C" void port_drawpipe_shape_words(void* pShape, u32 a, u32 b);
extern "C" void port_drawpipe_packet_lent(void* pPacket, void* pRealModel);
#endif
static void OpenFur(Scene& s, int which) {
    const int a = which % kActors;
    Actor& A = s.actor[a];
    const int m = which % s.data[A.data].numMat;
    J3DShapePacket& packet = A.model->mShapePacket[m];
    J3DShape* shape = packet.mpShape;
    for (int l = 0; l < kFurLayers; ++l) {
        if (sFurLayer[a][l] == nullptr) {
            J3DModel* layer = static_cast< J3DModel* >(calloc(1, sizeof(J3DModel)));
            std::memcpy(static_cast< void* >(layer), static_cast< const void* >(A.model), sizeof(J3DModel));  // (J3DModel2: the same data, packets, matrix buffer)
            layer->mFlags ^= J3DMdlFlag_SkinPosCpu;  // (another model: prepareDraw sets the shape up differently for it)
            sFurLayer[a][l] = layer;
        }
    }
    Rec('l', &gLive.playerLight, sizeof gLive.playerLight);  // (its light, by hand: live)
    RecV('u', which);                                        // (FurDrawer::setupMaterial)
    RecV('u', a);
    if (!Neg("fur")) port_drawpipe_shape_words(shape, 0x3C921780, 0x00F3CF3C);
    if (!Neg("furlent")) port_drawpipe_packet_lent(&packet, A.model);
    shape->mCurrentMtx.mMtxIdxRegA = 0x3C921780;
    shape->mCurrentMtx.mMtxIdxRegB = 0x00F3CF3C;
    J3DShape::sOldVcdVatCmd = nullptr;
    for (int l = 0; l < kFurLayers; ++l) {
        RecV('y', l);  // (setupLayerMaterial)
        packet.setModel(sFurLayer[a][l]);
        packet.prepareDraw();
        RecV('y', l + 16);
        shape->drawFast();
    }
    packet.setModel(A.model);
    port_drawpipe_packet_lent(nullptr, nullptr);
    RecV('z', which);  // (GXSetFog, GXSetClipMode)
    J3DShape::sOldVcdVatCmd = nullptr;
}
static void OpenTail() {
    // (the player's light by hand, the star pointer, the 2D: live state)
    Rec('l', &gLive.playerLight, sizeof gLive.playerLight);
    Rec('2', gLive.pointer, sizeof gLive.pointer);
    RecV('b', gLive.branch);
}
extern "C" void port_j3d_picture_begin(void);  // (J3DPacket.cpp: the drawn picture's number)
static void DrawAll(Scene& s) {
    port_j3d_picture_begin();  // (GameSystem::portDrawMiddle: after beginRender, before draw(); the cull stamps name this picture)
    const int lightScope = port_light_scope_enter(2);  // (GameScene::draw's)
    OpenView();
    for (int t = 0; t < kTypes + 1; ++t) {
        sCurType = t, sCurXlu = false;
        s.holder.drawOpa(t);
        if (t == 0) OpenShadows(s);
        if (t == 1) OpenHandModel(s, sDrawTurn);
        if (t == 2) OpenFur(s, sDrawTurn * 7 + 3);  // (after some opaque passes, before the others and before every translucent one)
        if (t == 2 && Neg("midsmall")) {
            gLive.pose[0] += 1.0f;  // (the draw itself moves the camera and the pointer after the hand-over)
            gLive.pointer[2] += 1;
        }
        if (t == 3) OpenHandModel(s, sDrawTurn * 5 + 2);
    }
    OpenView();
    for (int t = 0; t < kTypes + 1; ++t) {
        sCurType = t, sCurXlu = true;
        s.holder.drawXlu(t);
        if (t == 2) OpenHandModel(s, sDrawTurn * 3 + 1);
        if (t == 1) OpenFur(s, sDrawTurn * 11 + 5);  // (a second fur between translucent passes)
    }
    OpenTail();
    port_light_scope_exit(lightScope);
}

// The draw's begin and end (MainLoopFramework::beginRender / endRender): the frame buffer exchange and the clear before the
// picture, the flush and the copy to the display buffer after it. Each makes a call that cannot be recorded, so the engine must
// make both on the game thread outside the recording, the end only when the whole middle is in the stream.
static uint32_t sBegins = 0, sEnds = 0;
static void DrawBeginFix() {
    ++sBegins;
    RecHard('<', sDrawTurn);
    RecV('e', sDrawTurn * 3);  // (clearEfb, preGX)
}
static void DrawEndFix() {
    ++sEnds;
    RecV('f', sDrawTurn);      // (endGX's flush)
    RecHard('>', sDrawTurn);   // (prepareCopyDisp: GXSetDispCopyYScale)
    RecV('x', sDrawTurn * 7);  // (GXCopyDisp)
}

// ---- the driver's host
static std::vector< std::string >* sLog;
#ifdef DP_BASE
const PortPipeScene* gPortPipeScene = nullptr;  // (the base revision's draw buffers have none: the stubs above read the live state)
#endif
static PortPipeScene sSceneStore;
static void SceneNow(PortPipeScene* out) {
    std::memset(out, 0, sizeof *out);
    out->mView = gLive.view;
    out->mInvView = gLive.invView;
    std::memcpy(out->mProjection, gLive.projection, sizeof out->mProjection);
    std::memcpy(out->mPose, gLive.pose, sizeof gLive.pose);
    out->mPoseBytes = sizeof gLive.pose;
    out->mLights = true;
    out->mPlayerLight = true;
    out->mPlayerLightInfo = gLive.playerLight;
    out->mPointLight = gLive.pointLight;
    out->mExtSerial = J3DPortExtSerial();
    std::memcpy(out->mPointer, gLive.pointer, sizeof gLive.pointer);
    out->mBranch = gLive.branch;
}
static uint64_t SceneHash(const PortPipeScene& sc, int cls) {
    using namespace port::drawpipe;
    switch (cls) {
    case kCamera:
        return HashBytes(sc.mPose, sizeof sc.mPose, HashBytes(sc.mProjection, sizeof sc.mProjection, HashBytes(&sc.mInvView, sizeof sc.mInvView, HashBytes(&sc.mView, sizeof sc.mView))));
    case kSceneLight:
        return HashBytes(&sc.mPointLight, sizeof sc.mPointLight, HashBytes(&sc.mPlayerLightInfo, sizeof sc.mPlayerLightInfo));
    case kExt:
        return HashValue(13, sc.mExtSerial);
    case kPointer:
        return HashBytes(sc.mPointer, sizeof sc.mPointer, 14);
    default:
        return HashValue(15, sc.mBranch);
    }
}
static port::drawpipe::Pipe* sPipe = nullptr;
struct FixHost : port::drawpipe::Host {
    Scene* s = nullptr;
    port::drawpipe::Driver* driver = nullptr;
    int64_t clock = 0;
#ifdef DP_BASE
    // (built against the base revision's draw buffers, which have none of this: mode 0 only; tools/test_draw_pipe.sh compares
    // its stream with the patched code's mode 0)
    void Walk(port::drawpipe::Set&, port::drawpipe::Names&) override {}
    bool Freeze(port::drawpipe::Frame&) override { return false; }
    void UseFrozen(const port::drawpipe::Frame*) override {}
#else
    void Walk(port::drawpipe::Set& set, port::drawpipe::Names& names) override {
        using namespace port::drawpipe;
        s->holder.portPipeWalk(set, names);
        PortPipeScene now;
        SceneNow(&now);
        for (int c = kFirstSmall; c < kClasses; ++c) {
            set.Add(c, reinterpret_cast< const unsigned char* >(&sSceneStore) + (c - kFirstSmall), SceneHash(now, c), 0, 16);
        }
    }
    bool Freeze(port::drawpipe::Frame& frame) override { return s->holder.portPipeFreeze(frame); }
    void FreezeSmall() override { SceneNow(&sSceneStore); }
    uint32_t SmallDiffer() override {
        using namespace port::drawpipe;
        PortPipeScene now;
        SceneNow(&now);
        uint32_t differ = 0;
        for (int c = kFirstSmall; c < kClasses; ++c) {
            if (SceneHash(now, c) != SceneHash(sSceneStore, c)) differ |= 1u << (c - kFirstSmall);
        }
        return differ;
    }
    bool DrawOutstanding() override { return sPipe != nullptr && sPipe->Overlapped(); }
    void UseFrozen(const port::drawpipe::Frame* frame) override {
        DrawBufferHolder::portPipeUse(frame, frame ? &driver->pass : nullptr, frame ? &sSceneStore : nullptr);
    }
#endif
    int64_t NowNs() override { return clock += 1000; }
    const unsigned char* StreamPos() override { return sFifo + (gStream.size() & 0xFFFF); }
    void OpenLists(uint32_t* lists, uint32_t* objects) override { *lists += 5, *objects += 60; }
    void Log(const char* line) override {
        if (sLog) sLog->push_back(line);
    }
};

#ifndef DP_BASE
// ---- the engine's host (modes 3 and 4): what SceneFunction.cpp's PortPipeGame is in the game
static int sStallTurn = 0, sStallMs = 60;
static std::atomic< bool > sStalled{false};  // (the draw thread has had its stall)
static std::atomic< bool > sNestedSlow{false};  // (DP_NESTED: the draw thread is slow in this turn's first pass)
static std::vector< std::pair< int, bool > > sTokenPass;  // (DP_NEG=live: which pass a token stands for)
static bool FixTokenOut(u32 id, bool noDraw) {
    if (sTokenPass.size() <= id) sTokenPass.resize(id + 1);
    sTokenPass[id] = std::make_pair(sCurType, sCurXlu);
    return sPipe->Token(id, noDraw || Neg("keep"));  // (DP_NEG=keep: every token claims that its pass draws nothing)
}
static void FixAbandon(int why) { sPipe->Abandon(why); }
struct FixGame : port::drawpipe::PipeHost {
    Scene* s = nullptr;
    FixHost* host = nullptr;
    uint32_t polls = 0, ownerEnters = 0, merged = 0;
    bool owner = false;
    std::atomic< uint32_t > afterReplays{0};
    // (DP_NEG=capall: the begin and the end inside the recording, as one serial draw: every recording ends at its first call.
    // DP_NEG=endearly: the end made at the hand-over instead of at the join: the copy before the picture in the stream.)
    bool endMade = false;
    void DrawBegin() override {
        if (!Neg("capall")) DrawBeginFix();
    }
    void DrawMiddle() override {
        if (Neg("capall")) DrawBeginFix();
        DrawAll(*s);
        if (Neg("capall")) DrawEndFix();
    }
    void DrawEnd() override {
        if (!Neg("capall") && !endMade) DrawEndFix();
        endMade = false;
    }
    void AfterReplay() override { afterReplays.fetch_add(1, std::memory_order_relaxed); }
    void OwnerEnter() override {
        owner = true;
        ++ownerEnters;
    }
    void OwnerLeave() override { owner = false; }
    bool CaptureOpen() override {
        DrawBufferHolder::portPipeRecord(&FixTokenOut, &FixAbandon);
        return true;
    }
    void CaptureClose(bool) override { DrawBufferHolder::portPipeRecord(nullptr, nullptr); }
    void RunToken(uint32_t id, bool displaced) override {
        if (sStallTurn != 0 && sDrawTurn == sStallTurn && sPipe->worker.OnThread() && !sStalled.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sStallMs));  // (DP_STALL: the draw thread held up inside a pass)
        }
        if (sNestedSlow.load(std::memory_order_relaxed) && sPipe->worker.OnThread() && sNestedSlow.exchange(false)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));  // (DP_NESTED: slow enough for the game thread to wait at the join)
        }
        if (Neg("live") && id < sTokenPass.size()) {
            // (the pass from the live lists, on whatever thread executes the token)
            if (sTokenPass[id].second) {
                s->holder.getDrawBufferGroup(sTokenPass[id].first)->drawXlu();
            } else {
                s->holder.getDrawBufferGroup(sTokenPass[id].first)->drawOpa();
            }
            return;
        }
        DrawBufferHolder::portPipeRunToken(id, displaced);
    }
    void OverlapBegin() override {
        gPortPipeOverlap = true;
        if (Neg("endearly")) {
            DrawEndFix();
            endMade = true;
        }
    }
    void OverlapEnd() override {
        gPortPipeOverlap = false;
        host->UseFrozen(nullptr);
        merged += DrawBufferShapeDrawer::portMergedFinish();
    }
    uint32_t nested = 0;
    bool nestedWanted = false;
    void Poll() override {
        ++polls;
        if (nestedWanted) {
            // (DP_NESTED: an interrupt taken at this safe point whose handler uses GX: the draw is joined first, from in here)
            nestedWanted = false;
            if (sPipe->GameEnter(*this)) ++nested;
            RecV('I', sDrawTurn);
        }
    }
    int64_t NowNs() override {
        return std::chrono::duration_cast< std::chrono::nanoseconds >(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Log(const char* line) override {
        if (sLog) sLog->push_back(line);
    }
};
static FixGame* sGame = nullptr;
static void FixGameEnter() {
    if (sPipe != nullptr && sGame != nullptr) sPipe->GameEnter(*sGame);
}
#endif

static int EnvInt(const char* name, int fallback) {
    const char* e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: draw_pipe <0|1|2|3|4> <turns> <seed>\n");
        return 2;
    }
    const char mode = argv[1][0];
    const int turns = atoi(argv[2]);
    const u32 seed = static_cast< u32 >(atoi(argv[3]));
    if (const char* e = getenv("DP_NEG")) sNeg = e;
    if (const char* e = getenv("DP_DIFFSKIP")) sDiffSkip = atoi(e) != 0;
    if (const char* e = getenv("DP_MATMERGE")) sMatMerge = atoi(e) != 0;
    setenv("GXV_J3D_DIFFSKIP", sDiffSkip ? "1" : "0", 1);
    setenv("GXV_J3D_MATMERGE", sMatMerge ? "1" : "0", 1);
    sMode = mode - '0';
    if (sMode < 0 || sMode > 4) {
        fprintf(stderr, "draw_pipe: mode 0..4\n");
        return 2;
    }
#ifdef DP_BASE
    if (sMode != 0) {
        fprintf(stderr, "draw_pipe: the base revision's build draws mode 0 only\n");
        return 2;
    }
#endif
    sLogicGx = EnvInt("DP_LOGICGX", 0) != 0;
    __PortGXFifoPtr = sFifo;
    __PortGXFifoLimit = sFifo + sizeof sFifo - 64;

    static Scene scene;
    Build(scene, seed);
    static port::drawpipe::Driver driver;
    FixHost host;
    host.s = &scene;
    host.driver = &driver;
    std::vector< std::string > log;
    sLog = &log;
    using namespace port::drawpipe;
#ifndef DP_BASE
    static Pipe& pipe = *new Pipe;  // (never destroyed: its thread waits on it until the process ends)
    FixGame game;
    game.s = &scene;
    game.host = &host;
    const int serialEvery = EnvInt("DP_SERIAL", 0);
    if (sMode >= 3) {
        sPipe = &pipe;
        sGame = &game;
        sGameEnter = &FixGameEnter;
        pipe.api.begin = &cap::Begin;
        pipe.api.token = &cap::Token;
        pipe.api.tokenKeep = &cap::TokenKeep;
        pipe.api.end = &cap::End;
        pipe.api.replay = &cap::Replay;
        pipe.api.setHandler = &cap::SetHandler;
        pipe.api.abandon = &cap::Abandon;
        pipe.api.open = EnvInt("DP_NOOPEN", 0) ? nullptr : &cap::Open;  // (DP_NOOPEN=1: tokens after an abandon go to the recorder's handler)
        pipe.watchdogUs = static_cast< uint32_t >(EnvInt("DP_WATCHDOG_MS", 20)) * 1000u;
        pipe.sliceUs = 500;
        pipe.maxTaken = static_cast< uint32_t >(EnvInt("DP_MAXTAKEN", 1000000));
        cap::sAbandonAt = EnvInt("DP_ABANDON", 0);
        cap::sBusy = EnvInt("DP_BUSY", 0) != 0;
        sStallTurn = EnvInt("DP_STALL", 0);
        sStallMs = EnvInt("DP_STALL_MS", 60);
        if (const int upLate = EnvInt("DP_UPLATE", 0)) {
            pipe.worker.testUpDelayUs.store(static_cast< uint32_t >(upLate));
            pipe.worker.upWaitUs = 1000;
        }
        if (!pipe.worker.Start(1, 0, 1u << 20)) {
            fprintf(stderr, "draw_pipe: no draw thread\n");
            return 2;
        }
        pipe.worker.testStartDelayUs.store(static_cast< uint32_t >(EnvInt("DP_LATE", 0)));
        if (EnvInt("DP_UPLATE", 0) && (!pipe.worker.Ready() || pipe.worker.Reported() != 0)) {
            fprintf(stderr, "DP_UPLATE: the thread was not created, or had reported in before the starter went on\n");
            return 1;
        }
    }
#endif

    uint64_t all = 0;
    u32 frozen = 0, live = 0, fallbacks = 0, verify = 0, differ = 0, skipped = 0, predictMiss = 0, sysMiss = 0;
    Update(scene, 0);
    CalcAnim(scene);
    for (int turn = 1; turn <= turns; ++turn) {
        Flushes(scene, turn);
        sDrawTurn = turn;
        cap::sTurn = turn;
        // (the turn's decision before the freeze, as the game's glue has it: a serial turn of modes 3 / 4 freezes nothing and
        // draws its passes from the live lists; mode 3 never needs the frame)
        int why = kGo;
#ifndef DP_BASE
        if (sMode >= 3) {
            why = turn <= 2 ? kWhySettling : (serialEvery != 0 && turn % serialEvery == 0) ? kWhyDemo : kGo;
            driver.skipFreeze = sMode == 3 || why != kGo;
            pipe.NoteHeld(why != kGo ? 1u << why : 0u);
        }
#endif
        driver.Point(host, kPtHandOver, sMode, 2, true);
        gStream.clear();
        const PassStats before = driver.pass;  // (the driver's window sums; this turn's passes are the difference)
        const u32 skippedBefore = gJ3DPortListStats.diffSkipped;
        const bool midlist = Neg("midlist") && turn % 5 == 0;
        const int midActor = static_cast< int >((static_cast< u32 >(turn) * 7u) % kActors);
        if (midlist) {
            // (a list changed between the hand-over and its pass, by the draw side itself)
            Show(scene, midActor, !scene.actor[midActor].shown);
        }
        bool overlapped = false;
        const bool nestedTurn = EnvInt("DP_NESTED", 0) != 0 && turn % 5 == 2;
        bool nestedArmed = false, nestedInJoin = false;
#ifndef DP_BASE
        if (sMode >= 3 && Neg("open")) {
            // (the whole draw on the draw thread BESIDE update: what the open lists forbid)
            struct Job {
                static void Run(void* sc) {
                    DrawBeginFix();
                    DrawAll(*static_cast< Scene* >(sc));
                    DrawEndFix();
                }
            };
            pipe.worker.Dispatch(&Job::Run, &scene);
            Update(scene, turn);
            pipe.worker.WaitDone(0);
            overlapped = true;
        } else if (sMode >= 3) {
            if (nestedTurn && sMode == 4) sNestedSlow.store(true);
            pipe.Draw(game, sMode, why);
        } else
#endif
        {
            DrawBeginFix();
            DrawAll(scene);
            DrawEndFix();
        }
        if (midlist) {
            Show(scene, midActor, !scene.actor[midActor].shown);
        }
        driver.Point(host, kPtDrawn, sMode, 2, true);
#ifndef DP_BASE
        if (sMode >= 4 && pipe.Overlapped() && EnvInt("DP_OTHER", 0) && turn % 3 == 1) {
            // (DP_OTHER: another guest thread, a loading thread, reaches a list write while the draw thread has the job; the game
            // thread is parked meanwhile, as the guest's scheduler has it. It waits for the job and leaves the join alone.)
            std::thread other([&] { pipe.WaitOther(game); });
            other.join();
        }
        if (sMode >= 4 && pipe.Overlapped() && turn % 4 != 0 && EnvInt("DP_LATE", 0) == 0) {
            // (the console's timing: the draw thread wakes in a fraction of a millisecond and update() takes several, so the two
            // really run side by side. This update is over in microseconds: three turns in four it starts when the draw thread
            // has the job, so that they overlap here too; the fourth goes straight on and may reach the join first.)
            for (int i = 0; i < 2000 && pipe.worker.State() == Worker::kArmed; ++i) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
#endif
        if (!overlapped) Update(scene, turn);  // (mode 4: beside the draw thread's draw of this hand-over)
#ifndef DP_BASE
        if (sMode >= 3) {
            if (sStallTurn == turn) {
                // (DP_STALL: the draw thread must have the job when the game thread reaches the join, or there is nothing to wait for)
                while (pipe.worker.State() == Worker::kArmed) std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (nestedTurn && sMode == 4 && pipe.Overlapped()) {
                while (pipe.worker.State() == Worker::kArmed) std::this_thread::sleep_for(std::chrono::microseconds(100));
                game.nestedWanted = true;  // (the handler runs at the join's first safe point: FixGame::Poll)
                nestedArmed = true;
            }
            pipe.Join(game);  // (before calc-anim and the next flushes: they write what the passes read through the packets)
            sNestedSlow.store(false);
            if (nestedArmed && !game.nestedWanted) {
                nestedInJoin = true;  // (the handler ran inside the join and sent its bytes there)
            }
            game.nestedWanted = false;
        }
#endif
        if (nestedTurn && !nestedInJoin) {
            RecV('I', turn);  // (the interrupt's GX use after the update: the serial world, and a join that did not have to wait)
        }
        frozen += driver.pass.frozenPasses - before.frozenPasses, live += driver.pass.livePasses - before.livePasses;
        fallbacks += driver.pass.epochFallbacks - before.epochFallbacks;
        verify += driver.pass.verifyPasses - before.verifyPasses, differ += driver.pass.verifyDiffer - before.verifyDiffer;
        predictMiss += driver.pass.predictMiss - before.predictMiss, sysMiss += driver.pass.sysMiss - before.sysMiss;
        skipped += gJ3DPortListStats.diffSkipped - skippedBefore;
        if (const char* dump = getenv("DP_DUMP")) {
            // (debugging: turn DP_DUMP_TURN's stream into the file DP_DUMP)
            if (turn == EnvInt("DP_DUMP_TURN", 1)) {
                if (FILE* f = fopen(dump, "wb")) {
                    fwrite(gStream.data(), 1, gStream.size(), f);
                    fclose(f);
                }
            }
        }
        const uint64_t h = HashBytes(gStream.data(), gStream.size());
        all = Mix(all, h);
        printf("turn %d: %016llx (%zu bytes)\n", turn, static_cast< unsigned long long >(h), gStream.size());
        // (what the draw left behind for the logic: j3dSys and the two J3D statics must be the serial draw's and update's; pointers
        // by what they point at, so that two runs compare)
        uint64_t after = HashBytes(j3dSys.mViewMtx, sizeof j3dSys.mViewMtx);
        after = HashValue(HashValue(HashValue(after, j3dSys.mFlags), j3dSys.mDrawMode), ModelId(j3dSys.mModel));
        after = HashValue(HashValue(after, PtrId(j3dSys.mShapePacket)), PtrId(j3dSys.mModelDrawMtx));
        after = HashValue(HashValue(HashValue(after, PtrId(j3dSys.mVtxPos)), PtrId(J3DShape::sOldVcdVatCmd)), J3DShape::sEnvelopeFlag);
        printf("turn %d: after %016llx\n", turn, static_cast< unsigned long long >(after));
        if (getenv("DP_AFTER")) {
            fprintf(stderr, "after %d: flags %x mode %u model %d packet %d drawmtx %d vtx %d vcd %d env %d\n", turn, j3dSys.mFlags, j3dSys.mDrawMode, ModelId(j3dSys.mModel),
                    PtrId(j3dSys.mShapePacket), PtrId(j3dSys.mModelDrawMtx), PtrId(j3dSys.mVtxPos), PtrId(J3DShape::sOldVcdVatCmd), (int)J3DShape::sEnvelopeFlag);
        }
        driver.Point(host, kPtUpdated, sMode, 2, true);
        CalcAnim(scene);
        driver.Point(host, kPtCalced, sMode, 2, true);
    }
    fprintf(stderr, "mode %c, %d turns, seed %u: stream %016llx; passes from the frozen lists %u, from the live lists %u (epoch fall-backs %u); verify: %u passes, %u differed; differed lists left out by DIFFSKIP %u; statics guessed wrong %u; j3dSys words guessed wrong %u\n",
            mode, turns, seed, static_cast< unsigned long long >(all), frozen, live, fallbacks, verify, differ, skipped, predictMiss, sysMiss);
    int rc = 0;
    if (sDiffSkip && skipped == 0) {
        fprintf(stderr, "DIFFSKIP NOT ENGAGED: no differed list was left out\n");
        rc = 1;
    }
    if (sMode == 2) {
        // The census must see what this fixture's phases write, and nothing else: update writes the lists, the packets (the light
        // sort), the lights, the actors' flags, j3dSys (an actor's calc in movement) and every small field, never a list's bytes, a
        // matrix half or a shape / model word; the flushes write those; the draw writes shape words (prepareDraw) and j3dSys.
        const IntervalStat up = driver.Total(kIvUpdate);
        const IntervalStat dr = driver.Total(kIvDraw);
        const IntervalStat ca = driver.Total(kIvCalc);
        const IntervalStat fl = driver.Total(kIvFlush);
        // (an object that came into or went out of the read set with its actor was not written; a list that did was)
        auto wrote = [](const IntervalStat& st, int c) { return st.written[c] + (c == kLists || c == kPackets ? st.came[c] + st.went[c] : 0) != 0; };
        struct Check { const IntervalStat* st; int cls; bool want; const char* what; };
        const bool midsmall = Neg("midsmall");
        const Check checks[] = {
            {&up, kPackets, true, "update: packets"}, {&up, kLight, true, "update: lights"},
            {&up, kActor, true, "update: actor flags"}, {&up, kMatDL, false, "update: material lists"}, {&up, kDiffDL, false, "update: differed lists"},
            {&up, kDrawMtx, false, "update: draw matrices"}, {&up, kJoints, false, "update: joints"}, {&up, kSys, true, "update: j3dSys"},
            {&up, kCamera, true, "update: camera"}, {&up, kSceneLight, true, "update: scene lights"}, {&up, kExt, true, "update: scene serial"},
            {&up, kPointer, true, "update: star pointer"}, {&up, kBranch, true, "update: draw branches"},
            {&ca, kJoints, true, "calc-anim: joints"}, {&ca, kLists, false, "calc-anim: lists"}, {&ca, kDrawMtx, false, "calc-anim: draw matrices"},
            {&ca, kCamera, false, "calc-anim: camera"}, {&ca, kSceneLight, false, "calc-anim: scene lights"},
            {&fl, kDrawMtx, true, "flushes: draw matrices"}, {&fl, kMatDL, true, "flushes: material lists"}, {&fl, kDiffDL, true, "flushes: differed lists"},
            {&fl, kLists, false, "flushes: lists"}, {&fl, kLight, false, "flushes: lights"}, {&fl, kCamera, false, "flushes: camera"},
            {&dr, kSys, true, "draw: j3dSys"}, {&dr, kLists, false, "draw: lists"}, {&dr, kMatDL, false, "draw: material lists"},
            {&dr, kCamera, midsmall, "draw: camera"}, {&dr, kPointer, midsmall, "draw: star pointer"}, {&dr, kSceneLight, false, "draw: scene lights"},
            {&dr, kExt, false, "draw: scene serial"},
        };
        for (size_t ci = 0; ci < sizeof checks / sizeof checks[0]; ++ci) {
            const auto& c = checks[ci];
            if (wrote(*c.st, c.cls) != c.want) {
                fprintf(stderr, "CENSUS WRONG: %s: %s\n", c.what, c.want ? "not seen" : "seen");
                rc = 1;
            }
        }
        if (up.samples == 0 || fl.samples == 0) {
            fprintf(stderr, "CENSUS WRONG: no samples (update %u, flushes %u)\n", up.samples, fl.samples);
            rc = 1;
        }
        // (the frozen small fields against the draw's own writes after the hand-over: every drawn turn, not only the sampled ones)
        for (int c = kFirstSmall; c < kClasses; ++c) {
            const bool want = midsmall && (c == kCamera || c == kPointer);
            if ((driver.SmallDifferTotal(c) != 0) != want) {
                fprintf(stderr, "SMALL STATE WRONG: %s changed by the draw in %u turns (%s)\n", ClassName(c), driver.SmallDifferTotal(c), want ? "expected some" : "expected none");
                rc = 1;
            }
        }
        driver.Flush(host, sMode);
        for (const std::string& l : log) fprintf(stderr, "%s\n", l.c_str());
        if (Neg("midlist") ? differ == 0 : differ != 0) {
            fprintf(stderr, "VERIFY WRONG: %u passes differed from their frozen lists\n", differ);
            rc = 1;
        }
    } else if (sMode == 1 || sMode >= 3) {
        driver.Flush(host, sMode);
#ifndef DP_BASE
        if (sMode >= 3) pipe.MaybeLog(game, sMode, 1);
#endif
        for (const std::string& l : log) fprintf(stderr, "%s\n", l.c_str());
        if (!Neg("live") && !Neg("open") && frozen == 0 && sMode != 3) {
            fprintf(stderr, "NOT ENGAGED: no pass was drawn from the frozen lists\n");
            rc = 1;
        }
        if (Neg("midlist") && fallbacks == 0) {
            fprintf(stderr, "NO FALL-BACK: a list changed after the hand-over and its pass still drew from the frozen lists\n");
            rc = 1;
        }
        if (!Neg("guess") && (predictMiss != 0 || sysMiss != 0)) {
            fprintf(stderr, "GUESS WRONG: %u passes left other J3D statics than their token said, %u j3dSys words\n", predictMiss, sysMiss);
            rc = 1;
        }
        if (Neg("guess") && predictMiss == 0) {
            fprintf(stderr, "GUESS NOT TESTED: no pass left other statics than it found\n");
            rc = 1;
        }
#ifndef DP_BASE
        if (sMode >= 3 && !Neg("open")) {
            fprintf(stderr, "engine: whole draws on the draw thread %u, recorded and handed over %u, taken back unstarted %u, joined early %u; serial:",
                    pipe.TotalWhole(), pipe.TotalOverlapped(), pipe.TotalTaken(), pipe.TotalEarly());
            for (int r = 1; r < kReasons; ++r) {
                if (pipe.TotalSerial(r)) fprintf(stderr, " %s %u;", ReasonName(r), pipe.TotalSerial(r));
            }
            fprintf(stderr, " recorder: %u captures begun, %u refused, %u abandoned; polls at the join %u; owner hand-overs %u; pipeline %s\n",
                    cap::sBegun, cap::sRefused, cap::sAbandons, game.polls, game.ownerEnters, pipe.Off() ? "OFF" : "on");
            // each scenario must have happened, or the run proves nothing
            const bool stall = EnvInt("DP_STALL", 0) != 0, late = EnvInt("DP_LATE", 0) != 0;
            auto need = [&rc](bool ok, const char* what) {
                if (!ok) {
                    fprintf(stderr, "NOT ENGAGED: %s\n", what);
                    rc = 1;
                }
            };
            if (sMode == 3 && !late) need(pipe.TotalWhole() != 0, "no draw was made on the draw thread");
            if (sMode == 4 && !late) need(pipe.TotalOverlapped() != 0, "no draw was recorded and handed over");
            if (sMode == 4 && cap::sAbandonAt) need(cap::sAbandons != 0 && pipe.TotalSerial(kWhyAbandoned) != 0, "the recorder never abandoned");
            if (sMode == 4 && cap::sBusy) need(pipe.TotalSerial(kWhyBusy) != 0, "the recorder never refused");
            if (sMode == 4 && Neg("midlist")) need(pipe.TotalSerial(kWhyListMoved) != 0, "no recording was ended for a moved list");
            if (sMode == 4 && stall) need(pipe.Off() && pipe.TotalSerial(kWhyOff) != 0 && game.polls != 0, "the stalled draw thread did not trip the watchdog");
            if (late) need(pipe.TotalTaken() != 0, "no job was taken back unstarted");
            if (sMode == 4 && sLogicGx) need(pipe.TotalEarly() != 0, "the logic's GX use never joined a draw early");
            if (serialEvery) need(pipe.TotalSerial(kWhyDemo) != 0, "no serial turn among the pipelined ones");
            if (sMode == 4 && EnvInt("DP_OTHER", 0)) need(pipe.OtherWaits() != 0, "no other thread waited for the draw thread");
            if (sMode == 4 && EnvInt("DP_NESTED", 0)) need(game.nested != 0, "no join was made from inside the join's safe point");
            if (EnvInt("DP_UPLATE", 0)) need(pipe.worker.Reported() == 2 && (sMode == 3 ? pipe.TotalWhole() != 0 : pipe.TotalTaken() != 0 && pipe.TotalOverlapped() > pipe.TotalTaken()),
                                             "the late draw thread was not used once it ran, or no job was taken back before it did");
            // (the claim "this pass leaves the draw serial alone", against what the pass then sent)
            if (cap::sKeepWrong != 0) {
                fprintf(stderr, "WRONG CLAIM: %u tokens said their pass draws nothing, and the pass made a list call or sent a primitive\n", cap::sKeepWrong);
                rc = 1;
            }
            if (sMode == 4 && !late && !stall && cap::sAbandonAt == 0) need(cap::sKeepClaims != 0, "no token claimed that its pass draws nothing");
            // (the draw's begin and end: once a turn each, never inside a recording; the draw thread published the stream after
            // every recording it executed itself)
            need(sBegins == static_cast< uint32_t >(turns) && sEnds == static_cast< uint32_t >(turns), "the draw's begin or end was not made exactly once a turn");
            need(sHardInCapture == 0, "the draw's begin or end was made inside a recording");
            if (sMode == 4) {
                need(game.afterReplays.load() + pipe.TotalTaken() == pipe.TotalOverlapped(), "the draw thread did not publish the stream after each recording it executed");
            }
        }
#endif
    }
    return rc;
}
