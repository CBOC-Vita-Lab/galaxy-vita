// GXV_PAR_DL (gamedraw23) on the host: the drawn turn's material lists made by a worker while the game thread makes the view pass, against
// the serial order (material lists, then view pass), with the PRODUCTION material path: J3DMaterial.cpp, J3DMatBlock.cpp, J3DTevs.cpp,
// J3DGD.cpp, J3DPacket.cpp (differed lists, GXV_DL_GUARD / DIFFKEEP scratch, the guest-call suppression), J3DTransform.cpp, J3DStruct.cpp,
// GDBase.c and mtx.c as they are, built with TARGET_PC (J3D_MSYS: the material path's J3DSys), the production worker and merge
// (port/par_dl.h) and the verify's snapshot / fingerprint (Game/LiveActor/ParDlSnap.hpp). What is modelled here (ModelManager.cpp has
// the originals): updateDL(true) of a model with a display-list maker (calcMaterial: J3D_MSYS model / skin flags / post-texture-matrix
// flag / texture, then per material the animation's writes into the SHARED material and J3DMaterial::calc; DisplayListMaker::diff:
// J3D_MSYS's material packet and J3DMaterial::diff, the guest's interrupts only outside the overlap), port_par_dl_begin / _end /
// _game_enter, and the view pass (j3dSys's view matrix: identity, then the camera; the draw matrices from the joints).
//
// The scene: 3 model data x 3 materials (environment-, projection-, view-projection-mapped and plain texture matrices, TEV / konst
// colours, texture numbers, material colour, fog), 12 instances sharing them (so the material lists keep leftovers in shared state: some
// instances do not animate and take the last writer's values, exactly as the game's), each with its own differed lists. Every turn the
// instances change (animation state, joints), the camera turns, a varying subset is listed in a varying order.
// World S: the serial order. World P: the overlap. After every turn S and P must agree byte for byte: every listed entry's fingerprint
// right after it, every differed list, every material's state, j3dSys, J3DSys::sTexCoordScaleTable, the view pass's draw matrices.
//   par_dl run <turns> <seed> <scenario>   scenario: plain | early (the game thread enters the material path in the middle of its view
//                                          pass on some turns: the overlap is finished first) | foreign (the same from another thread
//                                          while the game thread waits for it: it joins and merges, the game thread ends the overlap) |
//                                          player (round 2: the player's view pass, GD list and j3dSys writes included, in the middle of
//                                          the pass in world S, after the join in world P) | take (the game thread takes the job before
//                                          the worker wakes) | slow (the worker starts late: the join sleeps) | verify (GXV_PAR_DL=2's flow:
//                                          snapshot, overlap, put back, serial again, compare: must find nothing)
//   PDL_NEG=sys     the material path is not given its own J3DSys (the worker writes j3dSys): ThreadSanitizer reports it / S != P
//   PDL_NEG=merge   no merge at the join: j3dSys differs from S
//   PDL_NEG=enter   the game thread's entry into the material path does not finish the overlap (scenario early): race / S != P
//   PDL_NEG=verify  (scenario verify) the overlap with PDL_NEG=sys: the verify flow must report differences
//   PDL_NEG=playerin (scenario player) the player's pass not split off: it runs beside the worker (GD, guest calls): race / S != P
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <vector>

#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DMatBlock.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphBase/J3DShape.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DTexture.hpp"
#include "JSystem/J3DGraphBase/J3DVertex.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/JMath/JMATrigonometric.hpp"
#include "Game/LiveActor/ParDlSnap.hpp"
#include "port/par_dl.h"
#include "port/par_async.h"
#include "revolution/gd/GDBase.h"

// ---- what the production units link against, outside the material path
bool J3DShapeMtx::sLODFlag;
void* J3DShape::sOldVcdVatCmd;
J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6];
J3DTexGenBlock* J3DDifferedTexMtx::sTexGenBlock;
J3DTexMtxObj* J3DDifferedTexMtx::sTexMtxObj;
J3DTexCoordScaleInfo J3DSys::sTexCoordScaleTable[8];
void J3DPacket::draw() {}
void J3DModelData::syncJ3DSysFlags() const {}
void J3DVertexBuffer::setArray() const {}
void J3DShape::loadPreDrawSetting() const {}
J3DSys::J3DSys() {}
J3DSys j3dSys;
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
static uint8_t sFifo[1 << 16];
extern "C" {
void __PortGXFifoOverflow(void) { __PortGXFifoPtr = sFifo; }
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void GXCallDisplayList(const void*, u32) {}
uint32_t port_host_to_phys(const void* p) { return static_cast< uint32_t >(reinterpret_cast< uintptr_t >(p)); }
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
// (par25: the symbols that merges after this test was written made J3DPacket.cpp / GXGeometry.c reference; never reached on the paths the test runs)
int __PortArrKeep = 0;
int __PortJ3nEmitOn = 0;
int port_sc_turn = 0;
int __PortGXJ3nMode(void) { return 0; }
int __PortJ3nEmitTake(void*, int, unsigned*) { return 0; }
void port_stream_census_model_begin2(const void*, unsigned) {}
void port_stream_census_model_end(void) {}
}
void* operator new[](size_t size, int align) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast< size_t >(align < 0 ? -align : align), size) != 0) abort();
    std::memset(p, 0, size);
    return p;
}

// ---- the guest OS's interrupt word: a plain int, as os_thread.cpp's sIntEnabled. Only the game thread may touch it (ThreadSanitizer
// reports any other thread that does, e.g. a worker making the per-list guest calls).
static int sGuestInts = 1;
static std::thread::id sMainId;
extern "C" BOOL OSDisableInterrupts() {
    const int p = sGuestInts;
    sGuestInts = 0;
    return p;
}
extern "C" BOOL OSRestoreInterrupts(BOOL l) {
    const int p = sGuestInts;
    sGuestInts = l;
    return p;
}
static bool OnMain() { return std::this_thread::get_id() == sMainId; }

static const char* sNeg = "";
static bool Neg(const char* n) { return std::strcmp(sNeg, n) == 0; }

// ---- the scene
enum { kData = 3, kMat = 3, kModels = 12 };
struct Data {
    J3DMaterial* mat[kMat];
    u32 diffFlag[kMat];
    bool post;
    ResTIMG* timg;  // (4 of them, below 4 GiB: port_offset32 resolves the image offsets from them as the console does)
    J3DTexture* tex;
};
struct Model {
    int data;
    bool playing, skinPos, skinNrm;
    J3DMatPacket mp[kMat];
    J3DShapePacket sp[kMat];
    Mtx anm, draw;
    // (the animation's values, written into the shared material as J3DMaterialAnm::calc writes them)
    J3DGXColorS10 tevColor;
    J3DGXColor kColor, matColor;
    u16 texNo;
    f32 transX, scaleY;
    s16 rot;
    u8 fogR;
};
struct World {
    Data data[kData];
    Model model[kModels];
    J3DSys sys;                           // (this world's j3dSys between turns)
    J3DTexCoordScaleInfo scale[8];        // (this world's J3DSys::sTexCoordScaleTable)
    u32 fp[kModels];                      // (the last turn's entry fingerprints)
    GDLObj* gd;                           // (GD's current list at the end of the turn)
};

static u32 Rnd(u32& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static J3DMaterial* MakeMaterial(int d, int m) {
    J3DMaterial* mat = new J3DMaterial();
    J3DTevBlock16* tev = new J3DTevBlock16();
    J3DTexGenBlockPatched* tg = new J3DTexGenBlockPatched();
    J3DColorBlockLightOn* col = new J3DColorBlockLightOn();
    J3DPEBlockFull* pe = new J3DPEBlockFull();
    J3DIndBlockFull* ind = new J3DIndBlockFull();
    mat->mTevBlock = tev;
    mat->mTexGenBlock = tg;
    mat->mColorBlock = col;
    mat->mPEBlock = pe;
    mat->mIndBlock = ind;
    mat->mShape = static_cast< J3DShape* >(calloc(1, sizeof(J3DShape)));  // (only its current-matrix words are touched)
    mat->mIndex = static_cast< u16 >(m);
    tev->mTevStageNum = 2;
    tev->mTexNo[0] = static_cast< u16 >((d + m) & 3);
    tev->mTexNo[1] = static_cast< u16 >((d + m + 1) & 3);
    for (int i = 0; i < 4; ++i) {
        tev->mTevColor[i].r = static_cast< s16 >(10 * d + i);
        tev->mTevKColor[i].g = static_cast< u8 >(20 * m + i);
    }
    tg->mTexGenNum = 3;
    static const u8 modes[4] = {J3DTexMtxMode_Envmap, J3DTexMtxMode_Projmap, J3DTexMtxMode_ViewProjmap, J3DTexMtxMode_EnvmapEffectMtx};
    for (int i = 0; i < 3; ++i) {
        tg->mTexCoord[i].setTexGenMtx(static_cast< u8 >(GX_TEXMTX0 + 3 * i));
        tg->mTexCoord[i].resetTexMtxReg();
        J3DTexMtx* t = new J3DTexMtx();
        t->getTexMtxInfo().mInfo = modes[(d + m + i) & 3];
        t->getTexMtxInfo().mSRT.mScaleX = 1.0f;
        t->getTexMtxInfo().mSRT.mScaleY = 1.0f;
        tg->mTexMtx[i] = t;
    }
    col->mColorChanNum = 1;
    col->mMatColor[0].r = static_cast< u8 >(m);
    pe->mFog.mType = 2;
    pe->mFog.mStartZ = 10.0f;
    pe->mFog.mEndZ = 1000.0f;
    return mat;
}
static void Build(World& w, u32 seed) {
    std::memset(&w.sys, 0, sizeof(w.sys));
    PSMTXIdentity(w.sys.mViewMtx);
    std::memset(w.scale, 0, sizeof(w.scale));
    for (int d = 0; d < kData; ++d) {
        Data& D = w.data[d];
        D.post = d == 2;
        void* low = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (low == MAP_FAILED) abort();
        D.timg = static_cast< ResTIMG* >(low);
        for (int i = 0; i < 4; ++i) {
            std::memset(&D.timg[i], 0, sizeof(ResTIMG));
            D.timg[i].mWidth = static_cast< u16 >(32 << i);
            D.timg[i].mHeight = static_cast< u16 >(16 << (3 - i));
            D.timg[i].mFormat = static_cast< u8 >(i);
            // (the image at the same address in both worlds: the differed lists carry it; the offset wraps modulo 2^32 as cooked ones may)
            D.timg[i].mImageDataOffset = static_cast< u32 >(0x01000000u + 0x1000u * static_cast< u32 >(d * 4 + i)) -
                                         static_cast< u32 >(reinterpret_cast< uintptr_t >(&D.timg[i]));
        }
        D.tex = new J3DTexture(4, D.timg);
        for (int m = 0; m < kMat; ++m) {
            D.mat[m] = MakeMaterial(d, m);
            D.diffFlag[m] = (3u << 8) | J3DDiffFlag_TexGen | (2u << 16) | J3DDiffFlag_TevReg | J3DDiffFlag_KonstColor |
                            J3DDiffFlag_MatColor | (m == 1 ? J3DDiffFlag_Fog : 0u) | J3DDiffFlag_TexCoordScale;
        }
    }
    u32 s = seed * 2654435761u + 1;
    for (int i = 0; i < kModels; ++i) {
        Model& M = w.model[i];
        M.data = i % kData;
        M.playing = (i % 4) != 3;  // (every fourth instance does not animate: it diffs the leftovers of the instance before it)
        M.skinPos = (i % 5) == 1;
        M.skinNrm = (i % 7) == 2;
        for (int m = 0; m < kMat; ++m) {
            J3DDisplayListObj* dl = new J3DDisplayListObj();
            dl->newDisplayList(0x400);
            M.sp[m].mpDisplayListObj = dl;
            M.mp[m].mpInitShapePacket = &M.sp[m];
            M.mp[m].mpMaterial = w.data[M.data].mat[m];
        }
        PSMTXIdentity(M.anm);
        PSMTXIdentity(M.draw);
        (void)Rnd(s);
    }
}
// update(): the turn's animation values and joints (the same in both worlds)
static void Update(World& w, int turn, u32 seed) {
    u32 s = seed * 7919u + static_cast< u32 >(turn) * 104729u + 3;
    for (int i = 0; i < kModels; ++i) {
        Model& M = w.model[i];
        M.tevColor.r = static_cast< s16 >(Rnd(s) & 0x3FF);
        M.tevColor.g = static_cast< s16 >(Rnd(s) & 0x3FF);
        M.kColor.b = static_cast< u8 >(Rnd(s));
        M.matColor.a = static_cast< u8 >(Rnd(s));
        M.texNo = static_cast< u16 >(Rnd(s) & 3);
        M.transX = static_cast< f32 >(Rnd(s) & 0xFFFF) / 4096.0f;
        M.scaleY = 0.5f + static_cast< f32 >(Rnd(s) & 0xFF) / 256.0f;
        M.rot = static_cast< s16 >(Rnd(s));
        M.fogR = static_cast< u8 >(Rnd(s));
        const f32 a = static_cast< f32 >(turn) * 0.1f + static_cast< f32 >(i);
        PSMTXRotRad(M.anm, 'y', a);
        M.anm[0][3] = static_cast< f32 >(i * 10);
        M.anm[1][3] = static_cast< f32 >(turn);
    }
}
static void Camera(Mtx out, int turn) {
    PSMTXRotRad(out, 'x', 0.05f * static_cast< f32 >(turn) + 0.3f);
    out[2][3] = -500.0f - static_cast< f32 >(turn);
}
// The listed entries of the turn, in their order (a varying subset, a varying start)
static void Entries(int turn, u32 seed, std::vector< int >& out) {
    out.clear();
    const int start = static_cast< int >((static_cast< u32 >(turn) * 5u + seed) % kModels);
    for (int k = 0; k < kModels; ++k) {
        const int i = (start + k) % kModels;
        if ((i + turn) % 5 != 0) out.push_back(i);
    }
}

// updateDL(true) of a model with a display-list maker (ModelManager.cpp, J3DModel::calcMaterial, DisplayListMaker::diff)
static void Refresh(World& w, int i) {
    Model& M = w.model[i];
    Data& D = w.data[M.data];
    PortDlGameEnter();
    J3D_MSYS.setModel(reinterpret_cast< J3DModel* >(&M));
    if (M.skinPos) J3D_MSYS.onFlag(4); else J3D_MSYS.offFlag(4);
    if (M.skinNrm) J3D_MSYS.onFlag(8); else J3D_MSYS.offFlag(8);
    if (D.post) J3D_MSYS.onFlag(J3DSysFlag_PostTexMtx); else J3D_MSYS.offFlag(J3DSysFlag_PostTexMtx);
    J3D_MSYS.setTexture(D.tex);
    for (int m = 0; m < kMat; ++m) {
        J3DMaterial* mat = D.mat[m];
        J3D_MSYS.setMatPacket(&M.mp[m]);
        if (M.playing) {
            // (J3DMaterialAnm::calc: the animation's values into the shared material)
            mat->getTevBlock()->setTevColor(0, M.tevColor);
            mat->getTevBlock()->setTevKColor(1, M.kColor);
            mat->getTevBlock()->setTexNo(0, M.texNo);
            mat->getColorBlock()->setMatColor(0, M.matColor);
            J3DTexMtx* t = mat->getTexGenBlock()->getTexMtx(1);
            t->getTexMtxInfo().mSRT.mTranslationX = M.transX;
            t->getTexMtxInfo().mSRT.mScaleY = M.scaleY;
            t->getTexMtxInfo().mSRT.mRotation = M.rot;
            if (J3DFog* f = mat->getPEBlock()->getFog()) f->mColor.r = M.fogR;
        }
        mat->calc(M.anm);
    }
    for (int m = 0; m < kMat; ++m) {
        BOOL ints = 0;
        const bool guest = !gPortParDlRun;  // (DisplayListMaker::diff: the guest's prohibit only outside the overlap)
        if (guest) ints = OSDisableInterrupts();
        J3D_MSYS.setMatPacket(&M.mp[m]);
        D.mat[m]->diff(D.diffFlag[m]);
        if (guest) OSRestoreInterrupts(ints);
    }
}
static u32 Fingerprint(World& w, int i) {
    Model& M = w.model[i];
    J3DMatPacket* packets[kMat];
    for (int m = 0; m < kMat; ++m) packets[m] = &M.mp[m];
    return ParDlFingerprint(w.data[M.data].mat, packets, kMat);
}

// ---- the overlap (ModelManager.cpp port_par_dl_begin / _end / _game_enter)
struct Ctx {
    World* w;
    std::vector< int > entries;
    bool slow;
};
static port::par::OneShot& sWorker = *new port::par::OneShot;  // (never destroyed: its thread waits on it until exit)
static bool sOpen = false, sJoined = false;
static BOOL sInts = 0;
alignas(8) static u8 sSys[sizeof(J3DSys)], sPre[sizeof(J3DSys)], sGame[sizeof(J3DSys)];
static Ctx sCtx;
static unsigned sEarly = 0, sTaken = 0, sConflicts = 0, sOverlapped = 0, sForeign = 0;
static std::atomic< unsigned > sRunner{0};
static unsigned Self() {  // (perf.cpp port_par_dl_self, host branch)
    const pthread_t t = pthread_self();
    unsigned h = 2166136261u;
    const unsigned char* b = reinterpret_cast< const unsigned char* >(&t);
    for (size_t i = 0; i < sizeof(t); ++i) h = (h ^ b[i]) * 16777619u;
    return h | 1u;
}
static void Job(void* p) {
    Ctx& c = *static_cast< Ctx* >(p);
    sRunner.store(Self(), std::memory_order_relaxed);
    if (c.slow && !OnMain()) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    for (int i : c.entries) {
        Refresh(*c.w, i);
        c.w->fp[i] = Fingerprint(*c.w, i);
    }
    sRunner.store(0, std::memory_order_relaxed);
}
// (round 2) the player's view pass when the pending pass includes it (MarioActor::calcViewAndEntry: updateLightDL's GD list and guest
// prohibit, J3DModel::calc of its hand models through j3dSys, the search light's view matrix put back). World S: in its group's place in
// the pass; world P: after the join (port_view_flush_player_last).
static u8 sPlayerList[256] __attribute__((aligned(32)));
static GDLObj sPlayerObj;
static J3DModel* const kPlayerModel = reinterpret_cast< J3DModel* >(static_cast< uintptr_t >(0x7777));
static void PlayerPass(int turn) {
    const BOOL ints = OSDisableInterrupts();
    GDInitGDLObj(&sPlayerObj, sPlayerList, sizeof(sPlayerList));
    __GDCurrentDL = &sPlayerObj;  // (left set, as updateLightDL leaves it)
    for (int k = 0; k < 8; ++k) GDWrite_u32(static_cast< u32 >(turn * 31 + k));
    OSRestoreInterrupts(ints);
    j3dSys.setModel(kPlayerModel);  // (the hand models' J3DModel::calc)
    j3dSys.offFlag(4);
    j3dSys.onFlag(8);
    Mtx light;
    PSMTXRotRad(light, 'z', 0.2f * static_cast< f32 >(turn));
    PSMTXCopy(light, j3dSys.mViewMtx);  // (the search light)
    Mtx cam;
    Camera(cam, turn);
    PSMTXCopy(cam, j3dSys.mViewMtx);  // (MR::loadViewMtx at its end)
}
static void BeginSnap() {  // (port_par_dl_begin: j3dSys as the serial lists would find it, before anything of the view pass)
    std::memcpy(sPre, &j3dSys, sizeof(J3DSys));
    std::memcpy(sSys, &j3dSys, sizeof(J3DSys));
}
static void BeginDispatch() {
    sInts = OSDisableInterrupts();
    if (!Neg("sys")) gPortJ3DSysMat = reinterpret_cast< J3DSys* >(sSys);
    const uintptr_t top = sWorker.StackTop();  // (perf.cpp port_par_dl_stack)
    gPortParDlStackLo = top ? top - port::par::OneShot::kStackSpan : 0;
    gPortParDlStackHi = top;
    gPortParDlRun = true;
    sJoined = false;
    sOpen = true;
    sWorker.Dispatch(&Job, &sCtx);
}
static void Begin() {
    BeginSnap();
    BeginDispatch();
}
static void Join() {  // (ModelManager.cpp ParDlJoin)
    if (sJoined) return;
    sJoined = true;
    if (sWorker.Take()) {
        ++sTaken;
        Job(&sCtx);
        sWorker.Reset();
    } else {
        ++sOverlapped;  // (the worker had the job: it ran beside the game thread's view pass)
        unsigned us = 0;
        sWorker.Wait(1000, &us);
    }
    std::memcpy(sGame, &j3dSys, sizeof(J3DSys));
    unsigned taken = 0, conflicts = 0;
    if (!Neg("merge") && !Neg("sys")) port::par::MergeWords(&j3dSys, sPre, sSys, sizeof(J3DSys), &taken, &conflicts);
    sConflicts += conflicts;
    gPortJ3DSysMat = &j3dSys;
    gPortParDlRun = false;
}
static void End() {  // (ModelManager.cpp port_par_dl_end)
    if (!sOpen) return;
    sOpen = false;
    Join();
    OSRestoreInterrupts(sInts);
}
extern "C" void port_par_dl_game_enter(void) {
    if (sRunner.load(std::memory_order_relaxed) == Self()) return;
    if (!sOpen || sJoined || Neg("enter")) return;
    ++sEarly;
    if (OnMain()) {
        End();
    } else {
        ++sForeign;
        Join();
    }
}

// The view pass (executeCalcViewAndEntryListExceptPlayer + ModelManager::calcView): j3dSys's view matrix, the draw matrices. early: the
// game thread enters the material path in the middle (a material calc of instance 0's first material), on some turns.
static int sViewWork = 1;
static bool sForeignScen = false;  // (scenario foreign)
static bool sPlayerInside = false;  // (scenario player, world S)
static void PlayerPass(int turn);  // (PDL_VIEW_WORK: repeats of each draw-matrix product, for the P world's overlap)
static void ViewPass(World& w, int turn, bool early) {
    PSMTXIdentity(j3dSys.mViewMtx);
    volatile f32 sink = j3dSys.mViewMtx[0][0];  // (the 2D groups read it)
    (void)sink;
    Mtx cam;
    Camera(cam, turn);
    PSMTXCopy(cam, j3dSys.mViewMtx);
    for (int i = 0; i < kModels; ++i) {
        if (sPlayerInside && i == kModels / 3) {
            PlayerPass(turn);
        }
        for (int k = 0; k < sViewWork; ++k) {  // (the view pass is long enough for the worker to run beside it)
            PSMTXConcat(j3dSys.getViewMtx(), w.model[i].anm, w.model[i].draw);
        }
        if (early && i == kModels / 2 && turn % 3 == 0) {
            auto enterMaterialPath = [&w] {
                PortDlGameEnter();
                Model& M = w.model[0];
                J3D_MSYS.setMatPacket(&M.mp[0]);
                w.data[M.data].mat[0]->calc(M.anm);
            };
            if (sForeignScen && turn % 2 == 0) {
                std::thread other(enterMaterialPath);  // (another thread while the game thread waits for it, as a guest switch would)
                other.join();
            } else {
                enterMaterialPath();
            }
        }
    }
}

static void Enter(World& w) {
    std::memcpy(&j3dSys, &w.sys, sizeof(J3DSys));
    std::memcpy(J3DSys::sTexCoordScaleTable, w.scale, sizeof(w.scale));
}
static void Leave(World& w) {
    w.gd = __GDCurrentDL;
    std::memcpy(&w.sys, &j3dSys, sizeof(J3DSys));
    std::memcpy(w.scale, J3DSys::sTexCoordScaleTable, sizeof(w.scale));
}

// (GXV_PAR_DL=2's flow, ModelManager.cpp ParDlVerify) returns the differences found
static unsigned VerifyFlow(World& w, const std::vector< int >& entries, int turn) {
    std::vector< ParDlMatSnap > s0, s1, s2;
    auto snapAll = [&](std::vector< ParDlMatSnap >& out) {
        out.clear();
        for (int d = 0; d < kData; ++d)
            for (int m = 0; m < kMat; ++m) {
                out.push_back(ParDlMatSnap());
                ParDlSnap(out.back(), w.data[d].mat[m]);
            }
    };
    snapAll(s0);
    J3DTexCoordScaleInfo scale0[8], scale1[8];  // (defaults48: as ParDlVerify, the redo starts from the texture-size table the flush found)
    std::memcpy(scale0, J3DSys::sTexCoordScaleTable, sizeof(scale0));
    Begin();
    ViewPass(w, turn, false);
    End();
    u32 fpPar[kModels];
    std::memcpy(fpPar, w.fp, sizeof(fpPar));
    u8 overlapped[sizeof(J3DSys)];
    std::memcpy(overlapped, &j3dSys, sizeof(J3DSys));
    snapAll(s1);
    for (auto& x : s0) ParDlRestore(x);
    std::memcpy(&j3dSys, sPre, sizeof(J3DSys));
    std::memcpy(scale1, J3DSys::sTexCoordScaleTable, sizeof(scale1));
    std::memcpy(J3DSys::sTexCoordScaleTable, scale0, sizeof(scale0));
    unsigned diff = 0;
    for (int i : entries) {
        Refresh(w, i);
        if (Fingerprint(w, i) != fpPar[i]) ++diff;
    }
    u8 expected[sizeof(J3DSys)];
    std::memcpy(expected, &j3dSys, sizeof(J3DSys));
    for (size_t b = 0; b < sizeof(J3DSys); ++b)
        if (sGame[b] != sPre[b]) expected[b] = sGame[b];
    for (size_t b = 0; b + 4 <= sizeof(J3DSys); b += 4)
        if (std::memcmp(expected + b, overlapped + b, 4) != 0) ++diff;
    std::memcpy(&j3dSys, expected, sizeof(J3DSys));
    if (std::memcmp(scale1, J3DSys::sTexCoordScaleTable, sizeof(scale1)) != 0) ++diff;
    snapAll(s2);
    for (size_t k = 0; k < s1.size(); ++k)
        if (!ParDlSnapSame(s1[k], s2[k])) ++diff;
    return diff;
}

static bool Same(World& S, World& P, const std::vector< int >& entries, int turn, const char* what) {
    unsigned bad = 0;
    for (int i : entries)
        if (S.fp[i] != P.fp[i]) {
            if (bad++ < 4) fprintf(stderr, "turn %d: entry %d fingerprint S %08x P %08x\n", turn, i, S.fp[i], P.fp[i]);
        }
    for (int i = 0; i < kModels; ++i) {
        for (int m = 0; m < kMat; ++m) {
            J3DDisplayListObj* a = S.model[i].sp[m].mpDisplayListObj;
            J3DDisplayListObj* b = P.model[i].sp[m].mpDisplayListObj;
            if (a->mSize != b->mSize || std::memcmp(a->mpDisplayList[0], b->mpDisplayList[0], a->mSize) != 0) {
                if (bad++ < 4) fprintf(stderr, "turn %d: model %d material %d differed list\n", turn, i, m);
            }
        }
        if (std::memcmp(S.model[i].draw, P.model[i].draw, sizeof(Mtx)) != 0) {
            if (bad++ < 4) fprintf(stderr, "turn %d: model %d draw matrix\n", turn, i);
        }
    }
    for (int d = 0; d < kData; ++d)
        for (int m = 0; m < kMat; ++m) {
            ParDlMatSnap a, b;
            ParDlSnap(a, S.data[d].mat[m]);
            ParDlSnap(b, P.data[d].mat[m]);
            a.m = b.m = nullptr;  // (different objects: compare the contents; the texture-matrix pointers differ by world too)
            for (int k = 0; k < 8; ++k) a.texMtxPtr[k] = b.texMtxPtr[k] = nullptr;
            if (!ParDlSnapSame(a, b)) {
                if (bad++ < 4) fprintf(stderr, "turn %d: data %d material %d state\n", turn, d, m);
            }
        }
    // j3dSys: the pointer words (model, material packet, texture) point into each world's own objects: compare them as indices
    J3DSys a = S.sys, b = P.sys;
    auto rebase = [](J3DSys& x, World& w) {
        for (int i = 0; i < kModels; ++i) {
            if (x.mModel == reinterpret_cast< J3DModel* >(&w.model[i])) x.mModel = reinterpret_cast< J3DModel* >(static_cast< uintptr_t >(i + 1));
            for (int m = 0; m < kMat; ++m)
                if (x.mMatPacket == &w.model[i].mp[m]) x.mMatPacket = reinterpret_cast< J3DMatPacket* >(static_cast< uintptr_t >(100 + i * kMat + m));
        }
        for (int d = 0; d < kData; ++d)
            if (x.mTexture == w.data[d].tex) x.mTexture = reinterpret_cast< J3DTexture* >(static_cast< uintptr_t >(1000 + d));
    };
    rebase(a, S);
    rebase(b, P);
    if (std::memcmp(&a, &b, sizeof(J3DSys)) != 0) {
        if (bad++ < 4) fprintf(stderr, "turn %d: j3dSys differs\n", turn);
    }
    if (S.gd != P.gd) {
        if (bad++ < 4) fprintf(stderr, "turn %d: GD's current list differs\n", turn);
    }
    if (std::memcmp(S.scale, P.scale, sizeof(S.scale)) != 0) {
        if (bad++ < 4) fprintf(stderr, "turn %d: sTexCoordScaleTable differs\n", turn);
    }
    if (bad != 0) fprintf(stderr, "%s: turn %d: %u differences\n", what, turn, bad);
    return bad == 0;
}

// ---- GXV_PAR_DL_HELP: AsyncBatch::HelpOne (port/par_async.h). A batch of view-like jobs on a SpecWorker that walks from the start while
// the game thread, "waiting", takes jobs from the end; then draw-like touches and the retire. Every job must run exactly once and every
// output be its job's value.
struct HelpJob {
    uint32_t id;
    uint64_t* out;
};
static std::atomic< uint32_t > sHelpRuns[4096];
static void HelpRun(HelpJob& j) {
    sHelpRuns[j.id].fetch_add(1, std::memory_order_relaxed);
    uint64_t v = j.id;
    for (int k = 0; k < 200; ++k) v = v * 6364136223846793005ull + 1442695040888963407ull;  // (some work)
    *j.out = v;
}
static int HelpTest(int rounds) {
    static port::par::AsyncBatch< HelpJob > batch;
    static port::par::SpecWorker& worker = *new port::par::SpecWorker;
    if (!batch.Init(4096, &HelpRun) || !worker.Start(1, -1, "pdl_view")) return 3;
    std::vector< uint64_t > out(4096);
    unsigned helped = 0, bad = 0;
    for (int r = 0; r < rounds; ++r) {
        const uint32_t n = 64 + static_cast< uint32_t >((r * 977) % 1500);
        for (uint32_t i = 0; i < n; ++i) sHelpRuns[i].store(0, std::memory_order_relaxed);
        batch.Begin();
        for (uint32_t i = 0; i < n; ++i) {
            HelpJob* j = batch.Add(&out[i], &out[i], nullptr);
            j->id = i;
            j->out = &out[i];
        }
        batch.Dispatch();
        worker.Dispatch(&port::par::AsyncBatch< HelpJob >::Item, &batch, batch.Items(), batch.Count());
        const uint32_t helpBudget = (r % 3 == 0) ? n : static_cast< uint32_t >(r % 50);  // (the "join" ends at varying points)
        for (uint32_t k = 0; k < helpBudget && batch.HelpOne(); ++k) ++helped;
        for (uint32_t i = 0; i < n; i += 7) batch.TouchOut(&out[i]);  // (the draw's touches)
        batch.Retire();
        worker.Quiesce();
        for (uint32_t i = 0; i < n; ++i) {
            uint64_t v = i;
            for (int k = 0; k < 200; ++k) v = v * 6364136223846793005ull + 1442695040888963407ull;
            if (sHelpRuns[i].load(std::memory_order_relaxed) != 1 || out[i] != v) ++bad;
        }
    }
    printf("par_dl help: %d batches, %u jobs taken by the waiting game thread, %u wrong\n", rounds, helped, bad);
    return bad == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "help") == 0) {
        return HelpTest(atoi(argv[2]));
    }
    if (argc < 5 || std::strcmp(argv[1], "run") != 0) {
        fprintf(stderr, "usage: par_dl run <turns> <seed> plain|early|take|slow|verify\n");
        return 2;
    }
    sMainId = std::this_thread::get_id();
    __PortGXFifoPtr = sFifo;
    __PortGXFifoLimit = sFifo + sizeof(sFifo) - 4096;
    if (const char* n = getenv("PDL_NEG")) sNeg = n;
    if (Neg("verify")) sNeg = "sys";
    if (const char* v = getenv("PDL_VIEW_WORK")) sViewWork = atoi(v) > 0 ? atoi(v) : 1;  // (the verify flow over a broken overlap: it must find the differences)
    const int turns = atoi(argv[2]);
    const u32 seed = static_cast< u32 >(atoi(argv[3]));
    const char* scen = argv[4];
    sForeignScen = std::strcmp(scen, "foreign") == 0;
    const bool player = std::strcmp(scen, "player") == 0;  // (round 2: the player's view pass made first in the flush)
    const bool early = std::strcmp(scen, "early") == 0 || sForeignScen, take = std::strcmp(scen, "take") == 0, slow = std::strcmp(scen, "slow") == 0,
               verify = std::strcmp(scen, "verify") == 0;
    if (!sWorker.Start(1, -1, "pdl")) return 3;
    World* S = new World();
    World* P = new World();
    Build(*S, seed);
    Build(*P, seed);
    std::vector< int > entries;
    unsigned failures = 0, verifyDiff = 0;
    for (int t = 0; t < turns; ++t) {
        Entries(t, seed, entries);
        // world S: the serial order
        Enter(*S);
        Update(*S, t, seed);
        for (int i : entries) {
            Refresh(*S, i);
            S->fp[i] = Fingerprint(*S, i);
        }
        sPlayerInside = player;  // (the serial pass: the player's group in its place, a third of the way)
        ViewPass(*S, t, early);
        sPlayerInside = false;
        Leave(*S);
        // world P: the overlap
        Enter(*P);
        Update(*P, t, seed);
        sCtx.w = P;
        sCtx.entries = entries;
        sCtx.slow = slow;
        if (verify) {
            verifyDiff += VerifyFlow(*P, entries, t);
        } else if (player) {
            Begin();
            sPlayerInside = Neg("playerin");  // (PDL_NEG=playerin: the player's pass left inside the overlapped pass)
            ViewPass(*P, t, early);  // (the rest, overlapped)
            End();
            if (!sPlayerInside) PlayerPass(t);  // (port_view_flush_player_last: after the join)
            sPlayerInside = false;
        } else {
            Begin();
            if (take) {
                End();  // (at once: the worker has not woken)
            }
            ViewPass(*P, t, early);
            End();
        }
        Leave(*P);
        if (!Same(*S, *P, entries, t, scen)) ++failures;
        if (sGuestInts != 1) {
            fprintf(stderr, "turn %d: the guest's interrupts were left off\n", t);
            ++failures;
        }
    }
    printf("par_dl %s seed %u: %d turns, %u failing, worker had the job %u, game thread took it %u, early finishes %u (another thread %u), "
           "conflicts %u%s\n", scen, seed, turns, failures, sOverlapped, sTaken, sEarly, sForeign, sConflicts, verify ? (verifyDiff ? ", VERIFY FOUND DIFFERENCES" : ", verify: 0 differences") : "");
    if (verify && verifyDiff != 0) {
        printf("par_dl verify: %u differences\n", verifyDiff);
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
