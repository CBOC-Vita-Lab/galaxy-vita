// GXV_REPLAY25 on the host (replay25, 12 Nov): the production draw buffers (Game/System/DrawBuffer.cpp: DrawBufferShapeDrawer::draw and the
// replay engine in it), the production J3DShape / J3DShapeMtx / J3DShapeDraw / J3DShapePacket / J3DPacket (the draw path), over synthetic models.
// Only the GX library is a stand-in (GXSetArray, GXCallDisplayList, port_gx_call_retained, the dirty-state flush): each writes the bytes the
// production one writes (CP array registers, the 0x40 list call, the 0x59 handle + call), into a FIFO buffer the engine reads and writes like
// the real one (__PortGXFifoPtr / __PortGXFifoLimit, __PortGXFifoOverflow = a publish step).
//
// One process draws one scenario in one mode (GXV_REPLAY25=0 / 1 / 2) and prints, per frame, the FNV hash of the bytes the frame put into the
// stream and, after every drawer, a hash of everything the draw leaves in j3dSys / the J3D statics / the shapes' bound fields / the GX state.
// Memory comes from one arena at a fixed offset (pointers go into the stream as offsets from its base), so the output of two processes can be
// compared byte for byte: tools/test_replay25.sh does, mode 1 against mode 0 and mode 2 against mode 0, for every seed and setting, and
// checks the census of mode 2 (verify) says 0 differed. T_MUT=<name> mutates the scene during the run (see Mutate): "idx" changes a word the
// replay's key does not hold (a stale replay: mode 1 must differ from mode 0, and verify must count it), the others change words the key
// holds (all three modes must agree).
//   draw_replay25 <frames> <seed>          environment: GXV_REPLAY25, T_FIFO (bytes), T_ARRSKIP, T_MUT, T_BATCH, T_SIMPLE (no J3D_DIFFSKIP/MATMERGE)
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/System/DrawBuffer.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include "JSystem/J3DGraphAnimator/J3DJointTree.hpp"
#include "JSystem/J3DGraphAnimator/J3DModel.hpp"
#include "JSystem/J3DGraphAnimator/J3DModelData.hpp"
#include "JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DFifo.hpp"
#include "JSystem/J3DGraphBase/J3DMaterial.hpp"
#include "JSystem/J3DGraphBase/J3DPacket.hpp"
#include "JSystem/J3DGraphBase/J3DPortDL.hpp"
#include "JSystem/J3DGraphBase/J3DShape.hpp"
#include "JSystem/J3DGraphBase/J3DShapeDraw.hpp"
#include "JSystem/J3DGraphBase/J3DShapeMtx.hpp"
#include "JSystem/J3DGraphBase/J3DSys.hpp"
#include "JSystem/J3DGraphBase/J3DTexture.hpp"
#include "JSystem/J3DGraphBase/J3DVertex.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include "port/array_skip.h"
#include "port/gx_fifo.h"

// ---------------------------------------------------------------- the arena (fixed layout: pointers are offsets from its base)
static const size_t kArena = 24u << 20;
alignas(4096) static uint8_t gArena[kArena];
static size_t gArenaUsed = 0;
static bool gArenaOn = false;
static void* ArenaAlloc(size_t n, size_t align) {
    gArenaUsed = (gArenaUsed + align - 1) & ~(align - 1);
    if (gArenaUsed + n > kArena) abort();
    void* p = gArena + gArenaUsed;
    gArenaUsed += n;
    std::memset(p, 0, n);
    return p;
}
static inline bool InArena(const void* p) { return static_cast< const uint8_t* >(p) >= gArena && static_cast< const uint8_t* >(p) < gArena + kArena; }
static inline uint32_t Rel(const void* p) { return p == nullptr ? 0u : InArena(p) ? static_cast< uint32_t >(static_cast< const uint8_t* >(p) - gArena) + 0x1000u : 0xDEAD0000u; }
void* operator new(size_t n) { return gArenaOn ? ArenaAlloc(n, 16) : std::calloc(1, n); }
void* operator new[](size_t n) { return gArenaOn ? ArenaAlloc(n, 16) : std::calloc(1, n); }
void* operator new[](size_t n, int align) { return gArenaOn ? ArenaAlloc(n, static_cast< size_t >(align < 0 ? -align : align)) : std::calloc(1, n); }
void operator delete(void* p) noexcept { if (!InArena(p)) std::free(p); }
void operator delete[](void* p) noexcept { if (!InArena(p)) std::free(p); }
void operator delete(void* p, size_t) noexcept { if (!InArena(p)) std::free(p); }
void operator delete[](void* p, size_t) noexcept { if (!InArena(p)) std::free(p); }
template < class T > static T* Z(size_t n = 1) { return static_cast< T* >(ArenaAlloc(sizeof(T) * n, 32)); }

// ---------------------------------------------------------------- the stream and the GX stand-ins
static const char* EnvStr(const char* n) { const char* e = getenv(n); return e != nullptr && *e != '\0' ? e : nullptr; }
static int EnvInt(const char* n, int d) { const char* e = EnvStr(n); return e != nullptr ? atoi(e) : d; }
static std::vector< uint8_t > gOut;        // (what was published since the frame began)
static uint8_t* gFifoBase = nullptr;
static size_t gFifoSize = 0;
static bool gDirty = false;                // (GX-library deferred state: set by a list marked 0x77, flushed by the next call that sends something)
static uint32_t gDirtyFlushes = 0;
uint32_t __PortGXDrawSerial = 0;
extern "C" {
uint8_t* __PortGXFifoPtr;
uint8_t* __PortGXFifoLimit;
void __PortGXFifoOverflow(void) {
    ++__PortGXFifoOverflowSerial;
    gOut.insert(gOut.end(), gFifoBase, __PortGXFifoPtr);
    __PortGXFifoPtr = gFifoBase;
}
uint32_t __PortArrValid = 0;
uint32_t __PortArrShadow[3];
int __PortArrKeep = 0;
int port_arrayskip_mode(void) { return EnvInt("T_ARRSKIP", 0); }
void port_arrayskip_note(int) {}
void port_arrayskip_verify_mark(uint32_t, uint32_t) {}
uint32_t port_host_to_phys(const void* p) { return Rel(p); }
int __PortGXReplayOk(void) { return EnvInt("T_NOSTREAM", 0) == 0; }
int port_gx_replay_clean(void) { return !gDirty; }
static void FlushDirty() {
    if (gDirty) {
        gDirty = false;
        ++gDirtyFlushes;
        __PortGXFifo_u8(0x61);
        __PortGXFifo_u32(0x4D000000u + gDirtyFlushes);
    }
}
void GXSetArray(GXAttr attr, const void* base, u8 stride) {
    __PortArrValid = 0;
    const u32 cp = static_cast< u32 >((attr == GX_VA_NBT ? GX_VA_NRM : attr) - GX_VA_POS);
    __PortGXFifo_u8(0x08);
    __PortGXFifo_u8(static_cast< u8 >(0xA0 + cp));
    __PortGXFifo_u32(Rel(base));
    __PortGXFifo_u8(0x08);
    __PortGXFifo_u8(static_cast< u8 >(0xB0 + cp));
    __PortGXFifo_u32(stride);
}
static void SendList(const void* list, u32 n) {
    __PortGXFifo_u8(0x40);
    __PortGXFifo_u32(Rel(list));
    __PortGXFifo_u32(n);
    if (list != nullptr && n != 0 && static_cast< const u8* >(list)[0] == 0x77) gDirty = true;  // (a list that leaves deferred state behind)
}
void GXCallDisplayList(const void* list, u32 n) {
    __PortArrValid = __PortArrKeep ? __PortArrValid : 0;
    ++__PortGXDrawSerial;
    FlushDirty();
    SendList(list, n);
}
void port_gx_call_prologue(void) { FlushDirty(); }
void port_gx_call_retained(const void* list, u32 n, u32 handle) {
    ++__PortGXDrawSerial;
    FlushDirty();
    __PortGXFifo_u8(0x59);
    __PortGXFifo_u32(handle);
    SendList(list, n);
}
void port_gx_call_bracketed(const void* list, u32 n, u32, u32, u32, u32, u32) { GXCallDisplayList(list, n); }
int __PortGXJ3nMode(void) { return 0; }
int __PortJ3nEmitOn;
int __PortJ3nEmitTake(void*, int, u32* lost) { *lost = 0; return 0; }
void GXLoadTexMtxImm(const f32 (*)[4], u32, GXTexMtxType) { __PortGXFifo_u8(0x10); }
void GDSetArray(GXAttr, const void*, u8) {}
void GDSetArrayRaw(GXAttr, u32, u8) {}
void GDSetVtxDescv(const GXVtxDescList*) {}
void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
BOOL OSDisableInterrupts() { return 1; }
BOOL OSRestoreInterrupts(BOOL) { return 1; }
s32 OSDisableScheduler(void) { return 0; }
s32 OSEnableScheduler(void) { return 0; }
void port_fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort(); }
static void LogV(const char* fmt, va_list ap) { char b[2048]; vsnprintf(b, sizeof b, fmt, ap); fprintf(stderr, "%s\n", b); }
void port_log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }
void port_log_summary(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(fmt, ap); va_end(ap); }
int port_turn_prof_on(void) { return 0; }  // (dead26: DrawBuffer.cpp asks since starbatch26; the host fixture times nothing)
int port_starbatch_dl_mode(void) { return 0; }
int port_starbatch_far_on(void) { return 0; }
void port_starbatch_dl_counts(unsigned* o) { for (int i = 0; i < 8; i++) o[i] = 0; }
void port_starbatch_far_counts(unsigned* o) { for (int i = 0; i < 8; i++) o[i] = 0; }
void port_mtx_profile_begin(void) {}
void port_mtx_profile_end(void) {}
void port_par_view_touch_out(const void*) {}
int port_sc_turn = 0;
int __PortGXCapFifo = 0;
int __PortGXFifoInStream(void) { return 1; }
void port_stream_census_model_begin2(const void*, u32) {}
void port_stream_census_model_end(void) {}
int port_flush_prefetch_on(void) { return 0; }
void port_prefetch_actor(const void*) {}
void port_prefetch_model(const void*) {}
void port_j3d_picture_begin(void);
void port_replay25_report(void);
int port_draw_prefetch_on(void) { return 0; }
int port_draw_prof_on(void) { return 0; }
void port_draw_prof_count(int, unsigned) {}
}
bool gPortParViewAsync = false;
u32 j3dDefaultViewNo;
J3DDrawBuffer::sortFunc J3DDrawBuffer::sortFuncTable[6];
J3DTexCoordScaleInfo J3DSys::sTexCoordScaleTable[8];
Mtx J3DMtxBuffer::sNoUseDrawMtx;
Mtx33 J3DMtxBuffer::sNoUseNrmMtx;
Mtx* J3DMtxBuffer::sNoUseDrawMtxPtr = &J3DMtxBuffer::sNoUseDrawMtx;
Mtx33* J3DMtxBuffer::sNoUseNrmMtxPtr = &J3DMtxBuffer::sNoUseNrmMtx;
void J3DPacket::draw() {}
J3DSys::J3DSys() {}
J3DSys j3dSys;
void J3DSys::loadPosMtxIndx(int addr, u16 indx) const { J3DFifoLoadIndx(0x20, indx, 0xB000 | ((u16)(addr * 0x0C))); }
void J3DSys::loadNrmMtxIndx(int addr, u16 indx) const { J3DFifoLoadNrmMtxIndx3x3(indx, addr * 3); }
void J3DVertexBuffer::setArray() const {
    J3D_DSYS.setVtxPos(mCurrentVtxPos);
    J3D_DSYS.setVtxNrm(mCurrentVtxNrm);
    J3D_DSYS.setVtxCol(mCurrentVtxCol);
}
namespace MR {
ShapePacketUserData* getJ3DShapePacketUserData(const J3DShapePacket* p) { return static_cast< ShapePacketUserData* >(p->mpUserArea); }
J3DModel* getJ3DModel(const LiveActor*) { abort(); }
ActorLightCtrl* getLightCtrl(const LiveActor*) { abort(); }
s32 getMaterialNum(J3DModel*) { abort(); }
J3DMaterial* getMaterial(J3DModel*, int) { abort(); }
const char* getMaterialName(const J3DModelData*, int) { abort(); }
bool isHiddenModel(const LiveActor*) { return false; }
void findActorLightInfo(const LiveActor*) {}
}  // namespace MR
void J3DShapeMtx::loadMtxIndx_PNGP(int slot, u16 index) const {  // (Game/System/Overwrite.cpp's)
    J3DFifoLoadIndx(0x20, index, 0xB000 | static_cast< u16 >(slot * 12));
    J3DFifoLoadNrmMtxIndx3x3(index, slot * 3);
    ShapePacketUserData* pData = MR::getJ3DShapePacketUserData(J3D_DSYS.getShapePacket());
    if (pData != nullptr) pData->loadTexMtx(J3D_DSYS.getShapePacket()->getShape()->getMaterial(), slot, index);
}
void ShapePacketUserData::callDL() const {}
void ShapePacketUserData::loadTexMtx(J3DMaterial*, int, u16) const { __PortGXFifo_u8(0x10); }
ShapePacketUserData::ShapePacketUserData() {}
ZoneLightID::ZoneLightID() { _0 = -1, mLightID = -1; }
void ZoneLightID::clear() { _0 = -1, mLightID = -1; }
bool LightFunction::tryFindNewAreaLightID(const TVec3f&, ZoneLightID*) { return false; }
AreaLightInfo* LightFunction::getAreaLightInfo(const ZoneLightID&) { return nullptr; }
void LightFunction::blendActorLightInfo(ActorLightInfo*, const ActorLightInfo&, const ActorLightInfo&, f32) {}
void LightFunction::loadActorLightInfo(const ActorLightInfo*) {}
s32 LightFunction::getDefaultStepInterpolate() { return 6; }
s32 JKRHeap::getFreeSize() { return 0; }
JKRHeap* JKRHeap::findFromRoot(void*) { return nullptr; }
void* JKRHeap::alloc(u32, int, JKRHeap*) { return nullptr; }

// ---------------------------------------------------------------- the scene
enum { kData = 7, kMaxMat = 3, kActors = 44, kJointN = 3 };
// shape kinds: 0 rigid plain, 1 rigid plain with two matrix groups, 2 concat-view (matrices in the stream), 3 per-vertex matrix index (envelope),
// 4 no-matrix shape (base matrix in the stream), 5 rigid plain again (a second eligible drawer after the odd ones)
static const int kKind[kData][kMaxMat] = {{0, 0, 1}, {0, 1, 0}, {2, 0, 0}, {0, 3, 0}, {4, 0, 5}, {0, 0, 0}, {1, 5, 3}};
struct MData {
    J3DModelData* data;
    J3DShape* shape[kMaxMat];
    J3DMaterial* mat[kMaxMat];
    DrawBufferShapeDrawer* drawer[kMaxMat];
    J3DMatPacket* matPacket[kMaxMat];
    int nmat;
    char name[16];
};
struct MActor {
    J3DModel* model;
    J3DMtxBuffer* mb;
    J3DShapePacket* sp;
    int d;
    int moves;     // 0 still, 1 view pass swaps its matrix halves every frame
    int vtx;       // 0 fixed arrays, 1 two arrays alternating, 2 three arrays cycling, 3 a new array now and then
    bool shown;
    u32 pos[4];
    void* arr[3][3];
    J3DDisplayListObj* diff[3];
    Mtx base;
};
static MData gD[kData];
static MActor gA[kActors];
static u32 gRng;
static u32 Rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5; return gRng; }
static J3DDisplayListObj* NewList(u32 size, u32 variant, bool dirty) {
    J3DDisplayListObj* dl = new J3DDisplayListObj();
    for (int i = 0; i < 2; ++i) {
        u8* b = static_cast< u8* >(ArenaAlloc(size, 32));
        for (u32 at = 0; at + 32 <= size; at += 32) {
            for (u32 k = 0; k < 6; ++k) {
                u8* c = b + at + 5 * k;
                c[0] = 0x61; c[1] = static_cast< u8 >(0xE0 + k); c[2] = static_cast< u8 >(variant); c[3] = static_cast< u8 >(variant * 7u + k); c[4] = static_cast< u8 >(at >> 5);
            }
        }
        if (dirty) b[0] = 0x77;
        dl->mpDisplayList[i] = b;
    }
    dl->mSize = size;
    dl->mMaxSize = size;
    return dl;
}
static void BuildActor(int a, int d, MData& D) {
    MActor& A = gA[a];
    A.d = d;
    A.moves = a % 3 != 0;
    A.vtx = a % 5 == 1 ? 1 : a % 7 == 3 ? 2 : a % 11 == 5 ? 3 : 0;
    A.model = Z< J3DModel >();
    A.model->mModelData = D.data;
    J3DMtxBuffer* mb = A.mb = Z< J3DMtxBuffer >();
    mb->mJointTree = &D.data->mJointTree;
    mb->mpAnmMtx = Z< Mtx >(kJointN);
    mb->mpUserAnmMtx = Z< Mtx >(kJointN);
    mb->mpWeightEvlpMtx = Z< Mtx >(kJointN);
    mb->mpScaleFlagArr = Z< u8 >(kJointN);
    for (int h = 0; h < 2; ++h) {
        mb->mpDrawMtxArr[h] = Z< Mtx* >(1);
        mb->mpDrawMtxArr[h][0] = Z< Mtx >(kJointN);
        mb->mpNrmMtxArr[h] = Z< Mtx33* >(1);
        mb->mpNrmMtxArr[h][0] = Z< Mtx33 >(kJointN);
    }
    A.model->mMtxBuffer = mb;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) A.arr[i][j] = ArenaAlloc(64, 32);
    A.model->mVertexBuffer.mCurrentVtxPos = A.arr[0][0];
    A.model->mVertexBuffer.mCurrentVtxNrm = A.arr[1][0];
    A.model->mVertexBuffer.mCurrentVtxCol = static_cast< GXColor* >(A.arr[2][0]);
    A.sp = new J3DShapePacket[kMaxMat];
    for (int m = 0; m < D.nmat; ++m) {
        J3DShapePacket& sp = A.sp[m];
        sp.mpShape = D.shape[m];
        sp.mpMtxBuffer = mb;
        sp.mpModel = A.model;
        sp.mpBaseMtxPtr = &A.base;
        const u32 r = Rnd() % 6;
        A.diff[m] = r == 5 ? nullptr : NewList(32 + 32 * (Rnd() % 2), Rnd() % 3, r == 4);
        sp.mpDisplayListObj = A.diff[m];
        D.drawer[m]->add(&sp, nullptr);
    }
    A.shown = true;
}
static void Build(u32 seed) {
    gRng = seed * 2654435761u + 12345u;
    std::memset(gD, 0, sizeof gD);
    std::memset(gA, 0, sizeof gA);
    for (int d = 0; d < kData; ++d) {
        MData& D = gD[d];
        snprintf(D.name, sizeof D.name, "Model%d", d);
        D.nmat = 2 + (d % 2);
        D.data = Z< J3DModelData >();
        D.data->mFlags = (d % 2) ? 0x20u : 0u;
        D.data->mJointTree.mJointNum = kJointN;
        D.data->mJointTree.mDrawMtxData.mEntryNum = kJointN;
        D.data->mJointTree.mDrawMtxData.mDrawMtxFlag = Z< u8 >(kJointN);
        D.data->mJointTree.mDrawMtxData.mDrawMtxIndex = Z< u16 >(kJointN);
        for (int m = 0; m < D.nmat; ++m) {
            J3DMaterial* mat = Z< J3DMaterial >();
            J3DShape* shape = new (Z< J3DShape >()) J3DShape();
            const int kind = kKind[d][m];
            shape->mIndex = static_cast< u16 >(m);
            shape->mMaterial = mat;
            shape->mVcdVatCmd = Z< u8 >(32);
            if (m > 0 && d % 3 == 1) shape->mVcdVatCmd = D.shape[0]->mVcdVatCmd;  // (two shapes with one vertex-format list: "not due" cases)
            shape->mHasPNMTXIdx = kind == 3;
            shape->mMtxGroupNum = kind == 1 ? 2 : 1;
            shape->mFlags |= kind == 4 ? J3DShpFlag_NoMtx : 0;
            shape->mShapeMtx = Z< J3DShapeMtx* >(2);
            shape->mShapeDraw = Z< J3DShapeDraw* >(2);
            for (u32 g = 0; g < shape->mMtxGroupNum; ++g) {
                shape->mShapeMtx[g] = kind == 2 ? new J3DShapeMtxConcatView(static_cast< u16 >(g)) : new J3DShapeMtx(static_cast< u16 >(g + 1));
                u8* list = Z< u8 >(64);
                list[0] = 0x90;
                shape->mShapeDraw[g] = new J3DShapeDraw(list, 64);
            }
            mat->mShape = shape;
            mat->mIndex = static_cast< u16 >(m);
            D.mat[m] = mat;
            D.shape[m] = shape;
            D.matPacket[m] = new J3DMatPacket();
            D.matPacket[m]->mpDisplayListObj = NewList(64, Rnd() % 3, false);
            D.drawer[m] = new DrawBufferShapeDrawer(mat, D.matPacket[m]);
            D.drawer[m]->init(kActors);
            D.drawer[m]->mPortFamily = D.name;
        }
    }
    for (int a = 0; a < kActors; ++a) BuildActor(a, a % kData, gD[a % kData]);
}
static void Mutate(int frame) {
    const char* mut = EnvStr("T_MUT");
    // (the view pass, every frame: swapDrawMtx / swapNrmMtx of a model that is posed, and what the matrices hold)
    for (int a = 0; a < kActors; ++a) {
        MActor& A = gA[a];
        if (A.moves) {
            std::swap(A.mb->mpDrawMtxArr[0][0], A.mb->mpDrawMtxArr[1][0]);
            std::swap(A.mb->mpNrmMtxArr[0][0], A.mb->mpNrmMtxArr[1][0]);
        }
        Mtx* dm = A.mb->mpDrawMtxArr[1][0];
        for (int i = 0; i < 12; ++i) (*dm)[i / 4][i % 4] = static_cast< f32 >(frame * 0.125 + a + i);
        for (int i = 0; i < 12; ++i) A.base[i / 4][i % 4] = static_cast< f32 >(frame * 0.25 + a * 2 + i);
        if (A.vtx == 1) {
            A.model->mVertexBuffer.mCurrentVtxPos = A.arr[0][frame & 1];
        } else if (A.vtx == 2) {
            A.model->mVertexBuffer.mCurrentVtxPos = A.arr[0][frame % 3];
        } else if (A.vtx == 3 && (Rnd() % 9) == 0) {
            A.model->mVertexBuffer.mCurrentVtxNrm = A.arr[1][Rnd() % 3];
        }
        // (a differed list rewritten into its other buffer: the call's address moves, as J3DMaterial::diff does)
        if (a % 4 == 2 && A.diff[0] != nullptr) {
            std::swap(A.diff[0]->mpDisplayList[0], A.diff[0]->mpDisplayList[1]);
        }
        // show / hide: the packet leaves the drawer and comes back (DrawBufferShapeDrawer::remove / add); hide flags of the packet and of its model
        if (a % 6 == 1 && frame % 9 == a % 9) {
            for (int m = 0; m < gD[A.d].nmat; ++m) {
                if (A.shown) gD[A.d].drawer[m]->remove(&A.sp[m]);
                else gD[A.d].drawer[m]->add(&A.sp[m], nullptr);
            }
            A.shown = !A.shown;
        }
        for (int m = 0; m < gD[A.d].nmat; ++m) {
            if (a % 5 == 0) {
                if ((frame + a) % 7 == 0) A.sp[m].mFlags |= J3DShpFlag_Hidden; else A.sp[m].mFlags &= ~static_cast< u32 >(J3DShpFlag_Hidden);
            }
        }
        A.model->mPortCullPicture = a % 8 == 3 && frame % 5 == 0 ? J3DPortPicture() : 0;
    }
    if (mut == nullptr || frame != 12) return;
    MActor& A = gA[2];  // (an eligible, moving, shown actor of model data 2... see the checks in tools/test_replay25.sh: the mutated shape is data 0, shape 0)
    (void)A;
    J3DShape* sh = gD[0].shape[0];
    if (!std::strcmp(mut, "idx")) {          // a matrix index the key does not hold: the stream changes, the replay does not know
        sh->mShapeMtx[0] = new J3DShapeMtx(5);
    } else if (!std::strcmp(mut, "list")) {  // the shape's geometry list: in the key
        u8* list = Z< u8 >(64);
        list[0] = 0x90;
        sh->mShapeDraw[0] = new J3DShapeDraw(list, 64);
    } else if (!std::strcmp(mut, "skin")) {  // CPU skinning on a model: out of the replay's kind
        gA[0].model->mFlags |= J3DMdlFlag_SkinPosCpu;
    } else if (!std::strcmp(mut, "flags")) { // the shape's own flags: in the key
        sh->mFlags |= 0x40;
    } else if (!std::strcmp(mut, "user")) {  // a user list appears on a packet: in the key
        static ShapePacketUserData ud;
        ud.mDisplayList = Z< u8 >(16);
        ud.mDisplayListSize = 16;
        gA[0].sp[0].mpUserArea = &ud;
    }
}

static uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast< const uint8_t* >(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template < class T > static uint64_t FnvV(uint64_t h, const T& v) { return Fnv(h, &v, sizeof v); }
static uint64_t StateHash() {
    uint64_t h = 1469598103934665603ull;
    h = FnvV(h, Rel(J3DShape::sOldVcdVatCmd));
    h = FnvV(h, J3DShape::sEnvelopeFlag);
    h = FnvV(h, J3DShapeMtx::sCurrentPipeline);
    h = FnvV(h, Rel(J3DShapeMtx::sCurrentScaleFlag));
    h = FnvV(h, J3DShapeMtx::sNBTFlag);
    h = FnvV(h, J3DShapeMtx::sTexMtxLoadType);
    h = FnvV(h, J3DShapeMtx::sLODFlag);
    h = FnvV(h, Rel(J3DDifferedTexMtx::sTexGenBlock));
    h = FnvV(h, Rel(J3DDifferedTexMtx::sTexMtxObj));
    h = FnvV(h, Rel(j3dSys.getModel()));
    h = FnvV(h, Rel(j3dSys.getShapePacket()));
    h = FnvV(h, Rel(j3dSys.mModelDrawMtx));
    h = FnvV(h, Rel(j3dSys.mModelNrmMtx));
    h = FnvV(h, Rel(j3dSys.getVtxPos()));
    h = FnvV(h, Rel(j3dSys.getVtxNrm()));
    h = FnvV(h, Rel(j3dSys.getVtxCol()));
    h = FnvV(h, j3dSys.mFlags);
    h = FnvV(h, Rel(gPortPipePrepared));
    h = FnvV(h, __PortArrValid);
    h = FnvV(h, __PortGXDrawSerial);
    h = FnvV(h, gDirty);
    h = Fnv(h, J3DShapeMtx::sMtxLoadCache, sizeof(J3DShapeMtx::sMtxLoadCache));
    for (int d = 0; d < kData; ++d) {
        for (int m = 0; m < gD[d].nmat; ++m) {
            const J3DShape* s = gD[d].shape[m];
            h = FnvV(h, s->mFlags);
            h = FnvV(h, Rel(s->mDrawMtx));
            h = FnvV(h, Rel(s->mNrmMtx));
            h = FnvV(h, Rel(s->mScaleFlagArray));
            h = FnvV(h, Rel(s->mCurrentViewNo));
        }
    }
    return h;
}

int main(int argc, char** argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 60;
    const unsigned seed = argc > 2 ? static_cast< unsigned >(atoi(argv[2])) : 1u;
    gFifoSize = static_cast< size_t >(EnvInt("T_FIFO", 16384));
    gFifoBase = static_cast< uint8_t* >(std::calloc(1, gFifoSize + 64));
    __PortGXFifoPtr = gFifoBase;
    __PortGXFifoLimit = gFifoBase + gFifoSize - 16;
    __PortGXFifoBatch = EnvInt("T_BATCH", 0);
    gArenaOn = true;
    Build(seed);
    uint64_t all = 1469598103934665603ull;
    for (int frame = 0; frame < frames; ++frame) {
        port_j3d_picture_begin();
        Mutate(frame);
        gOut.clear();
        __PortGXFifoPtr = gFifoBase;
        J3DShape::sOldVcdVatCmd = nullptr;
        uint64_t states = 1469598103934665603ull;
        for (int d = 0; d < kData; ++d) {
            for (int m = 0; m < gD[d].nmat; ++m) {
                DrawBufferShapeDrawer* dr = gD[d].drawer[m];
                if (dr->mNumPackets == 0) continue;
                if (frame % 4 == 3 && d == 5 && m == 1) gDirty = true;  // (a light load before a drawer)
                dr->draw();
                states = FnvV(states, StateHash());
            }
        }
        gOut.insert(gOut.end(), gFifoBase, __PortGXFifoPtr);
        const uint64_t sh = Fnv(1469598103934665603ull, gOut.data(), gOut.size());
        all = FnvV(FnvV(all, sh), states);
        printf("F %d bytes %zu stream %016llx state %016llx\n", frame, gOut.size(), static_cast< unsigned long long >(sh), static_cast< unsigned long long >(states));
    }
    printf("ALL %016llx\n", static_cast< unsigned long long >(all));
    port_replay25_report();
    return 0;
}
