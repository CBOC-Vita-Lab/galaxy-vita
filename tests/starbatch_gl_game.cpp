// GXV_STARBATCH host proof, the game's half (tools/test_starbatch.sh). Compiled with the game's flags (TARGET_PC) and linked with the PRODUCTION
// decomp/src/Game/System/DrawBuffer.cpp (DrawBufferShapeDrawer: add, light sort, draw, drawPort, portSortOrder), the console's GX library (decomp
// RVL_SDK/gx) and, through tests/starbatch_gl_main.cpp, the production vgx front end and GLES back end on Mesa. No game data.
// What this file stands in for: the J3D objects a drawer reads (shape packets, a shape, a material packet with its list objects: raw memory with the
// fields set), the shape's own draw (J3DShapePacket::drawFast: the packet's position / normal matrices, then the shape's list: a lit, indexed
// 40-triangle star prism of 22 vertices), the light loader, and the few MR:: / JKR calls DrawBuffer.cpp makes. The material list and every packet's
// differed list are real display-list bytes (BP / XF writes) in guest memory, as J3D's GD writers make them: the material's list sets the whole
// lit opaque state, a packet's differed list writes the packet's colour (XF 0x100C, MR::initDLMakerMatColor0's) and sends it through the same
// callDL / DIFFSKIP / light-loader code as the game.
// Scenes (SB_SCENE): 9 as 1 without light loaders (packet 0 can be merged with the material list); 1 N star bits of six colours in a random order (one light); 2 two light groups; 3 hidden and culled packets, packet 0 culled;
// 4 a blended material (the planner must leave it); 5 depth write off (likewise); 6 differed lists of two skeletons (likewise); 7 the pieces'
// colours change from picture to picture (lists rewritten); 8 the packet limit (256) and one past it. Every scene ends with a second drawer whose
// material list does not write the material colour (it draws with what the first drawer left: the end state must be the original's).
// SB_NOLEAK=1: no second drawer. SB_NOMERGE=1: the first drawer has no block for its merged list (otherwise one is given: the material list and the first
// packet's differed list are sent as one list from the second picture on).
// SB_NAIVE=1: the packets are ADDED in colour order (the order a naive sort would send them in, last packet not kept last), GXV_STARBATCH off: the
// control that the pictures of the scenes 4 .. 6 and the leak drawer SEE a changed order.
#include "Game/System/DrawBuffer.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Map/LightFunction.hpp"
#include "Game/Util/ModelUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
void* hx_guest_alloc(u32 bytes);
void hx_fifo_init(void);
void hx_fifo_flush(void);
void hx_guest_written(const void* p, unsigned n);  // (tests/starbatch_gl_main.cpp: the front end is told a list was written)
unsigned port_sc_turn = 0;
void port_stream_census_model_begin2(const void*, unsigned) {}
void port_stream_census_model_end(void) {}
}
// The guest arena of this test: the host pointers of the display lists and arrays must be the guest addresses the GX library writes into the FIFO (it
// writes (u32) of the pointer), so the arena is mapped below 1 GiB (the console's addresses) and the guest address IS the host address. These
// definitions come first in the link: they replace the stand-ins of tests/bloom_fast_gl_gx.c (whose arena is in the program's data).
#include <sys/mman.h>
namespace {
uintptr_t kArenaBase = 0x10000000u;
constexpr size_t kArenaBytes = 48u << 20;
unsigned char* sArenaP = nullptr;
size_t sArenaUsed = 0;
}  // namespace
extern "C" {
void* hx_guest_alloc(u32 bytes) {
    if (sArenaP == nullptr) {
        void* m = MAP_FAILED;
        for (uintptr_t base = 0x08000000u; base < 0x3C000000u && m == MAP_FAILED; base += 0x01000000u) {  // (below 1 GiB; wherever the dynamic loader left room)
            m = mmap(reinterpret_cast< void* >(base), kArenaBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m != MAP_FAILED) kArenaBase = base;
        }
        if (m == MAP_FAILED) { std::printf("FAIL: guest arena\n"); std::exit(1); }
        sArenaP = static_cast< unsigned char* >(m);
    }
    sArenaUsed = (sArenaUsed + 63u) & ~static_cast< size_t >(63);
    if (sArenaUsed + bytes + 4096 > kArenaBytes) return nullptr;
    void* p = sArenaP + sArenaUsed;
    sArenaUsed += bytes + 4096u;
    std::memset(p, 0, bytes);
    return p;
}
const uint8_t* hx_resolve(uint32_t a, uint32_t* avail) {
    a &= 0x3FFFFFFFu;
    if (sArenaP == nullptr || a < kArenaBase || a >= kArenaBase + kArenaBytes) return nullptr;
    if (avail) *avail = static_cast< uint32_t >(kArenaBase + kArenaBytes - a);
    return reinterpret_cast< const uint8_t* >(static_cast< uintptr_t >(a));
}
u32 port_host_to_phys(const void* p) { return p == nullptr ? 0u : static_cast< u32 >(reinterpret_cast< uintptr_t >(p)); }
void* port_phys_to_host(u32 a) {
    static u32 sLowMem[0x4000 / 4];
    if (a < 0x4000u) { sLowMem[0xF8 / 4] = 243000000u; return reinterpret_cast< u8* >(sLowMem) + a; }
    return reinterpret_cast< void* >(static_cast< uintptr_t >(a));
}
}
u32 gJ3DPortVer[J3DPortVer_Count];
J3DPortListStats gJ3DPortListStats;
J3DPortSortStats gJ3DPortSortStats;
void DCStoreRange(void* p, u32 n) { hx_guest_written(p, n); }

namespace {
constexpr int kW = 640, kH = 456;
GXRenderModeObj sMode;
u8* sReadback = nullptr;
u8* sXfb = nullptr;
u32 sPicture = 1;
int sScene = 1, sCount = 110, sNaive = 0, sFrame = 0;
unsigned sSeed = 1;

int Env(const char* n, int d) {
    const char* e = std::getenv(n);
    return (e != nullptr && *e != '\0') ? std::atoi(e) : d;
}
unsigned Lcg() { sSeed = sSeed * 1664525u + 1013904223u; return sSeed >> 8; }
float Rnd(float lo, float hi) { return lo + (hi - lo) * ((Lcg() & 0xFFFF) / 65535.0f); }

// ---- display-list bytes
struct Dl {
    std::vector< u8 > b;
    void be32(u32 v) { for (int i = 3; i >= 0; --i) b.push_back(static_cast< u8 >(v >> (8 * i))); }
    void bp(unsigned reg, unsigned v) { b.push_back(0x61); b.push_back(static_cast< u8 >(reg)); b.push_back(static_cast< u8 >(v >> 16)); b.push_back(static_cast< u8 >(v >> 8)); b.push_back(static_cast< u8 >(v)); }
    void xf(unsigned addr, std::initializer_list< u32 > w) {
        const unsigned n = static_cast< unsigned >(w.size());
        b.push_back(0x10); b.push_back(static_cast< u8 >((n - 1) >> 8)); b.push_back(static_cast< u8 >(n - 1)); b.push_back(static_cast< u8 >(addr >> 8)); b.push_back(static_cast< u8 >(addr));
        for (u32 x : w) be32(x);
    }
    void pad() { while (b.size() & 31) b.push_back(0); if (b.empty()) b.resize(32, 0); }
};
J3DDisplayListObj* MakeList(const Dl& d0) {
    Dl d = d0;
    d.pad();
    u8* g = static_cast< u8* >(hx_guest_alloc(static_cast< u32 >(d.b.size())));
    std::memcpy(g, d.b.data(), d.b.size());
    J3DDisplayListObj* o = static_cast< J3DDisplayListObj* >(std::calloc(1, sizeof(J3DDisplayListObj)));
    o->mpDisplayList[0] = g;
    o->mpDisplayList[1] = g;
    o->mSize = static_cast< u32 >(d.b.size());
    o->mMaxSize = o->mSize;
    return o;
}
void Rewrite(J3DDisplayListObj* o, const Dl& d0) {  // (same size: the bytes of a differed list made again)
    Dl d = d0;
    d.pad();
    std::memcpy(o->mpDisplayList[0], d.b.data(), d.b.size());
    hx_guest_written(o->mpDisplayList[0], static_cast< unsigned >(d.b.size()));
}

// material lists: the lit opaque state (the encodings of tests/vgx_dllearn_gl.cpp's Material('A')). kind: 0 opaque depth write; 1 blended; 2 depth
// write off; 3 the "leak" drawer's (no material colour: it uses what the drawer before left).
Dl MatList(int kind) {
    Dl d;
    d.bp(0, 1u << 4);
    d.xf(0x1009, {1});
    d.xf(0x103F, {0});
    d.xf(0x1040, {1u << 7});
    d.xf(0x100E, {(1u << 1) | (1u << 2) | (2u << 7)});
    d.xf(0x1010, {0});
    d.bp(0x28, 0);
    d.bp(0xC0, (10u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    d.bp(0xC1, (5u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    d.bp(0xF3, (7u << 16) | (7u << 19));
    d.bp(0x40, kind == 2 ? (1u | (3u << 1)) : (1u | (3u << 1) | (1u << 4)));
    d.bp(0x41, kind == 1 ? (1u | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5)) : ((1u << 3) | (1u << 4)));
    d.bp(0x42, 0);
    if (kind == 3) {  // (no colour of its own; its own light: none: the light the first drawer left)
    }
    return d;
}
u32 Rgba(u8 r, u8 g, u8 b, u8 a) { return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a; }
Dl DiffList(u32 colour, int skeleton) {
    Dl d;
    d.xf(0x100C, {colour});
    if (skeleton == 1) d.xf(0x100A, {((colour >> 1) & 0x7F7F7F00u) | 0xFFu});  // (a second register: another skeleton; its value follows the colour)
    return d;
}
const u32 kColours[6] = {Rgba(0x80, 0x00, 0x99, 255), Rgba(0xE6, 0xA0, 0x00, 255), Rgba(0x46, 0xA1, 0x08, 255), Rgba(0x37, 0x5A, 0xA0, 255), Rgba(0xBE, 0x33, 0x0B, 255), Rgba(0x80, 0x80, 0x80, 255)};

u32 ColourOf(int i) { return sScene == 4 ? ((kColours[i] & ~0xFFu) | 140u) : kColours[i]; }  // (scene 4: translucent pieces: the blend shows)

// ---- the shape: a 5-point star prism, indexed position and normal arrays in guest memory, a triangle list of 120 vertices
u8 *sPos, *sNrm, *sShapeList;
u32 sShapeListBytes;
void Put32f(u8* p, float f) { u32 v; std::memcpy(&v, &f, 4); p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v); }
void MakeShape() {
    const float pi = 3.14159265f;
    float px[22][3];
    for (int i = 0; i < 10; ++i) {
        const float a = 2 * pi * i / 10 + pi / 2, r = (i & 1) ? 0.45f : 1.0f;
        px[i][0] = r * std::cos(a); px[i][1] = r * std::sin(a); px[i][2] = 0.3f;
        px[10 + i][0] = px[i][0]; px[10 + i][1] = px[i][1]; px[10 + i][2] = -0.3f;
    }
    px[20][0] = px[20][1] = 0; px[20][2] = 0.3f;
    px[21][0] = px[21][1] = 0; px[21][2] = -0.3f;
    sPos = static_cast< u8* >(hx_guest_alloc(22 * 12));
    for (int i = 0; i < 22; ++i) for (int k = 0; k < 3; ++k) Put32f(sPos + i * 12 + k * 4, px[i][k]);
    // normals: 0 front, 1 back, 2..11 sides (outward, per outer segment)
    sNrm = static_cast< u8* >(hx_guest_alloc(12 * 12));
    Put32f(sNrm, 0); Put32f(sNrm + 4, 0); Put32f(sNrm + 8, 1);
    Put32f(sNrm + 12, 0); Put32f(sNrm + 16, 0); Put32f(sNrm + 20, -1);
    for (int i = 0; i < 10; ++i) {
        const float a = 2 * pi * (i + 0.5f) / 10 + pi / 2;
        Put32f(sNrm + (2 + i) * 12, std::cos(a)); Put32f(sNrm + (2 + i) * 12 + 4, std::sin(a)); Put32f(sNrm + (2 + i) * 12 + 8, 0);
    }
    std::vector< u8 > l;
    int n = 0;
    std::vector< u8 > v;
    auto vert = [&](int p, int nr) { v.push_back(u8(p >> 8)); v.push_back(u8(p)); v.push_back(u8(nr >> 8)); v.push_back(u8(nr)); ++n; };
    for (int i = 0; i < 10; ++i) { const int j = (i + 1) % 10; vert(20, 0); vert(i, 0); vert(j, 0); }          // front fan
    for (int i = 0; i < 10; ++i) { const int j = (i + 1) % 10; vert(21, 1); vert(10 + j, 1); vert(10 + i, 1); }  // back fan
    for (int i = 0; i < 10; ++i) {  // sides: two triangles a segment
        const int j = (i + 1) % 10;
        vert(i, 2 + i); vert(10 + i, 2 + i); vert(10 + j, 2 + i);
        vert(i, 2 + i); vert(10 + j, 2 + i); vert(j, 2 + i);
    }
    l.push_back(0x90); l.push_back(u8(n >> 8)); l.push_back(u8(n));
    l.insert(l.end(), v.begin(), v.end());
    while (l.size() & 31) l.push_back(0);
    sShapeListBytes = static_cast< u32 >(l.size());
    sShapeList = static_cast< u8* >(hx_guest_alloc(sShapeListBytes));
    std::memcpy(sShapeList, l.data(), l.size());
}

// ---- packets
struct Piece {
    J3DShapePacket* pk;
    J3DDisplayListObj* diff;
    ActorLightCtrl* light;
    int colour;
    int id;
    Mtx mv;
    Mtx nrm;
};
std::vector< Piece* > sPieces;  // by id
std::map< const void*, int > sLightId;
std::map< const void*, Piece* > sPieceOf;
J3DShape* sShape;
ActorLightCtrl* sDefaultLight;
J3DModel* sCulledModel;
struct Drawer {
    DrawBufferShapeDrawer* d;
    std::vector< Piece* > pieces;
};
std::vector< Drawer > sDrawers;

ActorLightCtrl* NewLight(int id) {
    ActorLightCtrl* l = static_cast< ActorLightCtrl* >(std::calloc(1, sizeof(ActorLightCtrl)));
    sLightId[l] = id;
    return l;
}
void Transform(Piece* p, float x, float y, float z, float s, float yaw, float pitch) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float r[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) { p->mv[i][j] = r[i][j] * s; p->nrm[i][j] = r[i][j]; }
        p->nrm[i][3] = 0;
    }
    p->mv[0][3] = x; p->mv[1][3] = y; p->mv[2][3] = z;
}
Drawer* NewDrawer(int matKind, int packets) {
    sDrawers.push_back(Drawer{});
    Drawer& dr = sDrawers.back();
    J3DMatPacket* mp = static_cast< J3DMatPacket* >(std::calloc(1, sizeof(J3DMatPacket)));
    mp->mpDisplayListObj = MakeList(MatList(matKind));
    dr.d = new DrawBufferShapeDrawer(nullptr, mp);
    dr.d->init(packets + 4);
    return &dr;
}
Piece* NewPiece(Drawer* dr, ActorLightCtrl* light, int colour, int skeleton, bool noDiff = false) {
    Piece* p = new Piece();
    p->id = static_cast< int >(sPieces.size());
    sPieces.push_back(p);
    p->colour = colour;
    p->light = light;
    J3DShapePacket* pk = static_cast< J3DShapePacket* >(std::calloc(1, sizeof(J3DShapePacket)));
    pk->mpShape = sShape;
    pk->mpDisplayListObj = noDiff ? nullptr : (p->diff = MakeList(DiffList(ColourOf(colour), skeleton)));
    std::memcpy(pk->unk_0x14, &p->id, sizeof p->id);
    p->pk = pk;
    sPieceOf[pk] = p;
    dr->pieces.push_back(p);
    return p;
}
}  // namespace

// ---- stand-ins for what DrawBuffer.cpp links against
bool J3DPortMatMergeOn() { return Env("GXV_J3D_MATMERGE", 1) != 0; }
bool J3DPortDiffSkipOn() { return Env("GXV_J3D_DIFFSKIP", 1) != 0; }
u32 J3DPortPicture() { return sPicture; }
int J3DPortNativeMode() { return 0; }
int J3DPortSortMode() { return Env("GXV_STARBATCH", 0); }
u32 J3DPortSortMin() { return static_cast< u32 >(Env("GXV_STARBATCH_MIN", 4)); }
void LightFunction::loadActorLightInfo(const ActorLightInfo*) {}
bool ActorLightCtrl::isSameLight(const ActorLightCtrl* o) const { return sLightId[this] == sLightId[o]; }
void ActorLightCtrl::loadLight() const {
    // (the light group's light: direction and colour by the group)
    const int id = sLightId[this];
    GXLightObj lo;
    GXInitLightPos(&lo, 200.0f * (id + 1), 300.0f, 100.0f);
    GXInitLightColor(&lo, id == 0 ? GXColor{255, 240, 220, 255} : GXColor{150, 200, 255, 255});
    GXInitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    GXLoadLightObjImm(&lo, GX_LIGHT0);
}
void J3DDisplayListObj::callDL() const { GXCallDisplayList(mpDisplayList[0], mSize); }
void ShapePacketUserData::callDL() const {}
void J3DShape::loadPreDrawSetting() const {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetArray(GX_VA_POS, sPos, 12);
    GXSetArray(GX_VA_NRM, sNrm, 12);
    GXSetCurrentMtx(GX_PNMTX0);
}
void* J3DShape::sOldVcdVatCmd;
bool J3DShape::sEnvelopeFlag;
void J3DShapePacket::prepareDraw() const {}
void J3DShapePacket::drawFast() {
    Piece* p = sPieceOf[this];
    GXLoadPosMtxImm(p->mv, GX_PNMTX0);
    GXLoadNrmMtxImm(p->nrm, GX_PNMTX0);
    GXCallDisplayList(sShapeList, sShapeListBytes);
}
J3DTexGenBlock* J3DDifferedTexMtx::sTexGenBlock;
J3DTexMtxObj* J3DDifferedTexMtx::sTexMtxObj;
namespace MR {
J3DModel* getJ3DModel(const LiveActor*) { return nullptr; }
J3DMaterial* getMaterial(J3DModel*, int) { return nullptr; }
ActorLightCtrl* getLightCtrl(const LiveActor*) { return nullptr; }
int getMaterialNum(J3DModel*) { return 0; }
const char* getMaterialName(const J3DModelData*, int) { return ""; }
ShapePacketUserData* getJ3DShapePacketUserData(const J3DShapePacket*) { return nullptr; }
}  // namespace MR
s32 JKRHeap::getFreeSize() { return 0; }
JKRHeap* JKRHeap::findFromRoot(void*) { return nullptr; }
void* JKRHeap::alloc(u32, int, JKRHeap*) { return nullptr; }

namespace {
void BuildScene() {
    sPieces.clear();
    sPieceOf.clear();
    sLightId.clear();
    sDrawers.clear();
    sSeed = 7u + static_cast< unsigned >(sScene) * 101u + static_cast< unsigned >(Env("SB_SEED", 0));
    sShape = static_cast< J3DShape* >(std::calloc(1, sizeof(J3DShape)));
    sShape->mMtxGroupNum = 1;
    sCulledModel = static_cast< J3DModel* >(std::calloc(1, sizeof(J3DModel)));
    int n = sCount;
    if (sScene == 8) n = Env("SB_BIG", 256);
    const int matKind = sScene == 4 ? 1 : (sScene == 5 ? 2 : 0);
    Drawer* stars = NewDrawer(matKind, n);
    ActorLightCtrl* lights[2] = {NewLight(0), NewLight(1)};
    sDefaultLight = lights[0];
    struct Spec { int colour; int light; float x, y, z, s, yaw, pitch; };
    std::vector< Spec > specs;
    for (int i = 0; i < n; ++i) {
        Spec sp;
        sp.colour = static_cast< int >(Lcg() % 6);
        sp.light = sScene == 2 ? (i % 5 < 3 ? 0 : 1) : 0;
        // overlapping pieces at distinct depths: the order of the draws would show where depth write is off or blending is on
        sp.x = Rnd(-260, 260); sp.y = Rnd(-150, 150); sp.z = -Rnd(300, 1000) - 0.37f * i; sp.s = Rnd(14, 44); sp.yaw = Rnd(0, 6.28f); sp.pitch = Rnd(0, 6.28f);
        specs.push_back(sp);
    }
    if (sNaive) {  // (the same pieces added in colour order, the last no longer last: the order a naive sort would send them in)
        std::vector< Spec > s2;
        for (int c = 0; c < 6; ++c) for (const Spec& s : specs) if (s.colour == c) s2.push_back(s);
        specs = s2;
    }
    std::vector< Piece* > mine;
    for (int i = 0; i < n; ++i) {
        const Spec& sp = specs[i];
        ActorLightCtrl* lc = sScene == 9 ? nullptr : lights[sp.light];
        Piece* p = NewPiece(stars, lc, sp.colour, sScene == 6 && (i % 3 == 0) ? 1 : 0);
        Transform(p, sp.x, sp.y, sp.z, sp.s, sp.yaw, sp.pitch);
        stars->d->add(p->pk, lc);
        mine.push_back(p);
    }
    if (sScene == 3) {
        // culled (the model's cull stamp = this picture's; the shape's hidden flag is the shape's, shared by every packet): packet 0 and every 7th
        sCulledModel->mPortCullPicture = 0;
        for (size_t i = 0; i < mine.size(); ++i) {
            if (i % 7 == 3 || i == 0) mine[i]->pk->mpModel = sCulledModel;  // (culled while the stamp equals the picture)
        }
    }
    // the first drawer's merged-list block (the heap the game takes it from is not here): the material list and the first packet's differed list
    // are one call from the second picture on (GXV_J3D_MATMERGE), so that the planner's rule "the first packet stays first" is under test
    if (!Env("SB_NOMERGE", 0)) {
        stars->d->mMergedDL = static_cast< u8* >(hx_guest_alloc(0x2000));
        stars->d->mMergedCap = 0x2000;
    }
    if (Env("SB_NOLEAK", 0)) return;
    // the second drawer: the "leak" material (draws with the colour and light the first drawer left)
    Drawer* leak = NewDrawer(3, 12);
    for (int i = 0; i < 12; ++i) {
        Piece* p = NewPiece(leak, lights[0], 0, 0, true);
        Transform(p, -240 + 44.0f * i, -170, -600 - 7.0f * i, 20, 0.3f * i, 0.2f * i);
        leak->d->add(p->pk, nullptr);
    }
}
void SetColours(int f) {
    // scene 7: colours change from picture to picture (a few pieces a picture: the lists rewritten, the front end told)
    for (size_t i = 0; i < sPieces.size(); ++i) {
        Piece* p = sPieces[i];
        if (p->diff == nullptr || (static_cast< int >(i) + f) % 9 != 0) continue;
        p->colour = (p->colour + 1) % 6;
        Rewrite(p->diff, DiffList(ColourOf(p->colour), 0));
    }
}
void SceneState() {
    GXSetViewport(0.0f, 0.0f, kW, kH, 0.0f, 1.0f);
    GXSetScissor(0, 0, kW, kH);
    Mtx44 proj;
    C_MTXPerspective(proj, 45.0f, static_cast< float >(kW) / kH, 1.0f, 5000.0f);
    GXSetProjection(proj, GX_PERSPECTIVE);
    GXSetCullMode(GX_CULL_BACK);
    GXSetChanAmbColor(GX_COLOR0A0, GXColor{40, 40, 60, 255});
}
}  // namespace

extern "C" void hg_init(int scene) {
    sScene = scene;
    sCount = Env("SB_N", 110);
    sNaive = Env("SB_NAIVE", 0);
    hx_fifo_init();
    static u8 fifoDummy[64];
    GXInit(fifoDummy, sizeof fifoDummy);
    sMode = GXNtsc480IntDf;
    sMode.fbWidth = kW;
    sMode.efbHeight = kH;
    sMode.xfbHeight = kH;
    GXSetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetDispCopySrc(0, 0, kW, kH);
    GXSetDispCopyDst(kW, kH);
    sReadback = static_cast< u8* >(hx_guest_alloc(GXGetTexBufferSize(kW, kH, GX_TF_RGBA8, GX_FALSE, 1)));
    sXfb = static_cast< u8* >(hx_guest_alloc(kW * kH * 2));
    MakeShape();
    BuildScene();
}

extern "C" void hg_frame(int n) {
    sFrame = n;
    ++sPicture;
    if (sScene == 3) sCulledModel->mPortCullPicture = sPicture;
    if (sScene == 7) SetColours(n);
    SceneState();
    if (sScene == 9) sDefaultLight->loadLight();  // (no light loaders: the light is the scene's)
    // the opaque pass of the draw buffer: every drawer, in its order
    for (Drawer& d : sDrawers) d.d->draw();
    hx_fifo_flush();
    GXSetCopyFilter(GX_FALSE, sMode.sample_pattern, GX_FALSE, sMode.vfilter);
    GXSetTexCopySrc(0, 0, kW, kH);
    GXSetTexCopyDst(kW, kH, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(sReadback, GX_FALSE);
    GXPixModeSync();
    GXSetCopyClear(GXColor{16, 16, 24, 0}, 0xFFFFFF);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXCopyDisp(sXfb, GX_TRUE);
    hx_fifo_flush();
}

extern "C" void hg_report(void) {
    const J3DPortSortStats& t = gJ3DPortSortStats;
    std::printf("sortstats drawers %u stretches %u packets %u sorted %u sortedPackets %u same %u runs %u -> %u records %u -> %u bad %u", t.drawers, t.stretches, t.packets,
                t.sorted, t.sortedPackets, t.same, t.runsBefore, t.runsAfter, t.recBefore, t.recAfter, t.verifyBad);
    std::printf(" vinst %u vbad %u vlist %u vunc %u", t.verifyInst, t.verifyInstBad, t.verifyListBad, t.verifyUncomparable);
    for (int i = 0; i < J3DPortSortWhy_Count; ++i) if (t.why[i]) std::printf(" %s=%u(%u)", J3DPortSortWhyName(i), t.why[i], t.whyPackets[i]);
    std::printf("\n");
    const J3DPortListStats& c = gJ3DPortListStats;
    std::printf("liststats drawers %u diffSent %u diffSkipped %u matMerged %u\n", c.drawers, c.diffSent, c.diffSkipped, c.matMerged);
}
