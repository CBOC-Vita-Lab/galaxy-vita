#include "Game/Player/MarioShadow.hpp"
#include "port/offset32.h"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/J3DModelX.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Screen/ImageEffectLocalUtil.hpp"
#include "Game/Util/DirectDraw.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SchedulerUtil.hpp"
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <cstring>
#include <revolution/gd.h>

void MarioShadow_FORCE_MATCH_SDATA2() {
    (void)1.0f;
    (void)0.0f;
    (void)0.5f;
    (void)-1.0f;
    (void)2.0f;
}

namespace {
    f32 cCheckOffset0 = 5.0f;
    f32 cDrawOffset0 = -5.0f;
}  // namespace

#ifdef TARGET_PC
#include <cstdio>
#include <cstdlib>
#include "port/colshadow_select.h"
#include "port/colshadow_clip.h"
#include "port/stage_knob.h"
#include <algorithm>
extern "C" void port_log_summary(const char* fmt, ...);
// ---- GXV_COLSHADOW_STABLE (shadow25): 0 = the game's polygon selection (default); 1 = stable selection (port/colshadow_select.h: a band on the camera / facing
// thresholds, nearest-to-the-drop-point when over the cap, a dead band on the box radius feedback); 2 = 1 + the shadow polygons lifted 10 units (not 5) for the
// depth test. GXV_COLSHADOW_LOG=1: a census line every 300 pictures (any mode).
// Where the list lives: create() runs per logic TURN, in one call, on the game thread; the draws (draw1/2/3, sendVtx...) run per PICTURE on the same thread and
// write the vertices into the GX stream as they go (GXBegin / GXPosition), so a draw always sees a complete list of the LAST turn and the renderer never reads it
// later: no tearing, no half-built list. A picture with two turns just shows the second one's list. The flicker can only come from the lists of successive turns
// differing, and that is what the stable selection removes.
namespace {
enum { kStableCap = 256 };
int ColShadowStable() {
    static const int v = [] { const char* e = getenv("GXV_COLSHADOW_STABLE"); return e && *e ? atoi(e) : 0; }();
    return v;
}
// ---- GXV_COLSHADOW_CLIP (dreadblend25, stage-settable colshadow_clip): 0 = every ground triangle of the list is drawn whole (default); 1 = each triangle is cut to the shadow texture's
// own extent (port/colshadow_clip.h: the texture is clamped and its edge texels are 0, so what lies outside is thrown away by the alpha test anyway) and a triangle wholly outside is not drawn;
// 2 = census only ("colshadow-clip:" line every 300 pictures; nothing changes). The polygons' fill is what the Dreadnought's 40 draws of fs 1240 cost.
int ColShadowClip() {
    return PORT_STAGE_KNOB(int32_t, std::clamp< long >(port::stageknob::Int("GXV_COLSHADOW_CLIP", 0), 0, 2));
}
port::colshadow::ClipCensus gClipCensus;
// Cuts polygon i of the list (3 vertices a triangle's own, pos / uv of the vertices as the draw would send them); false: not cut (the polygon is drawn as before).
inline int ClipPolygon(const TVec3f* pos[3], const TVec2f* tex[3], port::colshadow::ClipVtx* out) {
    port::colshadow::ClipVtx in[3];
    for (int j = 0; j < 3; j++) {
        in[j].x = pos[j]->x;
        in[j].y = pos[j]->y;
        in[j].z = pos[j]->z;
        in[j].u = tex[j]->x;
        in[j].v = tex[j]->y;
    }
    const int m = port::colshadow::ClipUvBox(in, 3, -port::colshadow::kClipPad, 1.0f + port::colshadow::kClipPad, out);
    port::colshadow::ClipCensus& c = gClipCensus;
    c.polygons++;
    c.areaIn += port::colshadow::UvArea(in, 3);
    if (m == 0) {
        c.outside++;
    } else {
        c.areaOut += port::colshadow::UvArea(out, m);
        c.trisOut += static_cast< uint64_t >(m - 2);
        if (m == 3 && std::memcmp(in, out, sizeof in) == 0) c.inside++; else c.cut++;
    }
    c.trisIn++;
    return m;
}
inline void EmitClipped(const port::colshadow::ClipVtx* v, int m) {
    for (int t = 1; t + 1 < m; t++) {
        const int idx[3] = {0, t, t + 1};
        for (int k = 0; k < 3; k++) {
            GXPosition3f32(v[idx[k]].x, v[idx[k]].y, v[idx[k]].z);
            GXTexCoord2f32(v[idx[k]].u, v[idx[k]].v);
        }
    }
}
bool ColShadowLog() {
    static const bool v = [] { const char* e = getenv("GXV_COLSHADOW_LOG"); return e && *e && atoi(e) != 0; }();
    return v;
}
uint64_t ColShadowKey(Triangle& t) {
    uint64_t h = 1469598103934665603ull;
    if (t.mParts != nullptr) {
        // identity = the collision part and the triangle's index in it: it survives a part that moves or rotates (a platform, the Dreadnought's disc), where the
        // world-space vertices change every turn. (The extras the game appends, _330/_334, are Triangles of a part too.)
        h = port::colshadow::Mix(h, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(t.mParts) >> 4));
        h = port::colshadow::Mix(h, static_cast< uint32_t >(reinterpret_cast< uintptr_t >(t.mParts) >> 36));
        h = port::colshadow::Mix(h, t.mIdx);
        return h | 1;
    }
    for (u32 j = 0; j < 3; j++) {
        const TVec3f& p = *t.getPos(j);
        uint32_t b;
        memcpy(&b, &p.x, 4); h = port::colshadow::Mix(h, b);
        memcpy(&b, &p.y, 4); h = port::colshadow::Mix(h, b);
        memcpy(&b, &p.z, 4); h = port::colshadow::Mix(h, b);
    }
    return h | 1;
}
struct ColShadowState {
    const void* owner;
    port::colshadow::Mem mem[2];
    port::colshadow::RadiusMem radius;
    int cur;
    // census
    uint32_t swings, radMin, radMax, pics, minP, maxP, changed, changedStill, turns, turnChanged, turnChangedStill, capTurns, dropped, empties, maxEdge, radiusChanges;
    uint64_t sumP;
    uint64_t setHash, lastPicSet, lastTurnSet;
    uint32_t setCount, lastPicCount;
    bool picValid, turnValid;
    TVec3f lastPicPos, lastPicCam, lastPicDir, lastTurnPos, lastTurnCam, lastTurnDir;
};
ColShadowState* ColShadowFor(const void* o) {
    static ColShadowState sSt[2];
    for (int i = 0; i < 2; i++) {
        if (sSt[i].owner == o) return &sSt[i];
    }
    for (int i = 0; i < 2; i++) {
        if (sSt[i].owner == nullptr) {
            sSt[i].owner = o;
            sSt[i].minP = 0xFFFFFFFFu;
            return &sSt[i];
        }
    }
    return &sSt[1];
}
bool ColShadowStill(const TVec3f& pos, const TVec3f& pos0, const TVec3f& cam, const TVec3f& cam0, const TVec3f& dir, const TVec3f& dir0) {
    const TVec3f dp(pos - pos0), dc(cam - cam0);
    const f32 dd = MR::abs(dir.x - dir0.x) + MR::abs(dir.y - dir0.y) + MR::abs(dir.z - dir0.z);
    return dp.dot(dp) < 0.25f && dc.dot(dc) < 1.0f && dd < 0.0005f;
}
}  // namespace
#endif

CollisionShadow::~CollisionShadow() {
}

void CollisionShadow::setMode(u32 mode) {
    if (_C == mode) {
        return;
    }

    switch (mode) {
    case 0:
        _30C = ::cCheckOffset0;
        _310 = ::cDrawOffset0;
        break;
    case 1:
        _30C = 50.0f;
        _310 = -150.0f;
        break;
    case 2:
        _30C = 18.0f;
        _310 = 0.0f;
        break;
    }

    _C = mode;
}

CollisionShadow::CollisionShadow(f32 radius, f32 length) : NameObj("\x93\x8a\x89\x65\x83\x56\x83\x83\x83\x68\x83\x45") {
    _2F0.zero();
    _2FC = 0;
    _2FE = 0;
    _300 = nullptr;
    _305 = false;
    _30C = 0.0f;
    _310 = 0.0f;
    _314.zero();
    _320.zero();
    _32C = nullptr;
    _330 = nullptr;
    _334 = nullptr;
    _338 = 0;
    _33A = 0;
    _33C = 0;
    _340 = nullptr;
    _344 = nullptr;
    _348 = 0;
    _308 = 0;
    _E = 0;
    _14 = radius;
    _18 = 0.0f;
    _1C = 0.0f;
    _24.zero();
    _30.zero();
    _3C.zero();
    _48.zero();
    _10 = radius;
    _20 = length;
    _54 = 0;
    _C = 255;
    setMode(0);
    initCaptureTex();
    _58 = 128;
    _5A = 3;
    _60 = new (32) TVec3f[_58 * _5A];
    _64 = new (32) TVec2f[_58 * _5A];
    _68 = new u16[_58 * _5A];
    _5C = new u8[_58];
    _6C = 0;
#ifdef TARGET_PC
    _32C = new Triangle[ColShadowStable() ? static_cast< u32 >(kStableCap) + 8 : static_cast< u32 >(_58)];  // (stable: room for the 256 the keeper may fill, plus the extras)
#else
    _32C = new Triangle[_58];
#endif
    _330 = new Triangle[2];
    _334 = new Triangle[2];
    _338 = 0;
    _33A = 0;
    _33C = 0;
    _6E = 0;
    _306 = 0;
    _304 = 0;
    _307 = 1;

    for (u32 i = 0; i < 128; i++) {
        _70[i] = 0;
    }

    for (u32 i = 0; i < 128; i++) {
        _F0[i] = 0;
    }

    createDL();
}

void CollisionShadow::create(const TVec3f& rPosition, const TVec3f& rDirection, const TVec3f& rUp) {
    TVec3f corners[8];
    TVec3f blended;
    TVec3f vertical(rUp);
    bool success = MR::vecBlendSphere(_24, rDirection, &blended, 0.1f);
    if (MR::isNearZero(_24) || !success || _305) {
        blended = rDirection;
    }

    _305 = false;
    if (!MR::isNearZero(blended)) {
        _24 = blended;
        MR::vecKillElement(vertical, blended, &vertical);
        MR::normalizeOrZero(&vertical);
        if (!MR::isNearZero(vertical)) {
            _30 = vertical;
        }
    }

    _2F0 = rPosition;
    if (_C == 3) {
        return;
    }

    _E++;
    TVec3f side;
    side.cross(vertical, blended);
    if (MR::isNearZero(side)) {
        return;
    }

    MR::normalize(&side);
    for (u32 i = 0; i < 8; i++) {
        corners[i] = rPosition;
        if (i & 1) {
            corners[i] += vertical * _10;
        } else {
            corners[i] -= vertical * _10;
        }

        if (i & 2) {
            corners[i] += side * _10;
        } else {
            corners[i] -= side * _10;
        }

        if (i & 4) {
            corners[i] -= blended * _30C;
        } else {
            corners[i] += blended * _20;
        }
    }

    TVec3f minimum;
    TVec3f maximum;
    MR::createBoundingBox(corners, 8, &minimum, &maximum);
    _314 = minimum;
    _320 = maximum;
    if (MR::isInitializeStatePlacementSomething()) {
        return;
    }

#ifdef TARGET_PC
    const bool stable = ColShadowStable() != 0;
    u32 count = MR::createAreaPolygonListArray(_32C, stable ? static_cast< u32 >(kStableCap) : static_cast< u32 >(_58), corners, 8);
#else
    u32 count = MR::createAreaPolygonListArray(_32C, _58, corners, 8);
#endif
    for (u32 i = 0; i < _338; i++) {
        _32C[count] = _330[i];
        count++;
    }

    _338 = 0;
    for (u32 i = 0; i < _33A; i++) {
        _32C[count] = _334[i];
        count++;
    }

    _33A = 0;
    _33C = count;
    _54 = 0;
    if (!count) {
#ifdef TARGET_PC
        if (stable || ColShadowLog()) {
            ColShadowState* st = ColShadowFor(this);
            st->mem[st->cur].Clear();
            st->setCount = 0;
            st->setHash = 0;
            st->lastTurnSet = 0;
            st->turnValid = true;
        }
#endif
        _10 = _14;
        return;
    }

    u32 polygonCount = 0;
    _6E = 0;
#ifdef TARGET_PC
    if (stable) {
        ColShadowState* st = ColShadowFor(this);
        const TVec3f camZ = MR::getCamZdir();
        static port::colshadow::Cand cands[kStableCap + 8];
        uint8_t cls[kStableCap + 8];
        for (u32 i = 0; i < count; i++) {
            Triangle& tr = _32C[i];
            const TVec3f normal(*tr.getNormal(0));
            port::colshadow::Cand& c = cands[i];
            c.facing = normal.dot(_24);
            c.camDot = normal.dot(camZ);
            c.floorSkip = false;
            if (c.facing <= port::colshadow::kFaceStay && c.camDot <= port::colshadow::kCamStay && -c.facing >= port::colshadow::kShadowToEdge) {
                const char* code = MR::getFloorCodeString(&tr);
                c.floorSkip = code != nullptr && (strcmp(code, "PullBack") == 0 || strcmp(code, "Glass") == 0);
            }
            TVec3f centre(*tr.getPos(0));
            centre += *tr.getPos(1);
            centre += *tr.getPos(2);
            centre.scale(0.33333334f);
            centre.sub(_48);
            c.dist = centre.length();
            c.key = ColShadowKey(tr);
        }
        const unsigned dropped = port::colshadow::SelectStable(cands, count, st->mem[st->cur], st->mem[st->cur ^ 1], _58, 128, cls);
        st->cur ^= 1;
        if (dropped) {
            st->capTurns++;
            st->dropped += dropped;
        }
        for (u32 i = 0; i < count; i++) {
            if (cls[i] == 2) {
                _70[_6E] = i;
                _6E++;
            } else if (cls[i] == 1) {
                _5C[polygonCount] = i;
                polygonCount++;
            }
        }
    } else
#endif
    for (u32 i = 0; i < count; i++) {
        TVec3f normal(*_32C[i].getNormal(0));
        f32 facing = normal.dot(_24);
        if (facing > 0.0f) {
            continue;
        }

        if (normal.dot(MR::getCamZdir()) > 0.707f) {
            continue;
        }

        if (MR::isNearZero(facing, 0.15f)) {
            _70[_6E] = i;
            _6E++;
        } else {
            const char* code = MR::getFloorCodeString(&_32C[i]);
            if (code == nullptr || (strcmp(code, "PullBack") != 0 && strcmp(code, "Glass") != 0)) {
                _5C[polygonCount] = i;
                polygonCount++;
            }
        }
    }

    _54 = polygonCount;
#ifdef TARGET_PC
    if (ColShadowLog()) {
        ColShadowState* st = ColShadowFor(this);
        uint64_t set = 0;
        for (u32 i = 0; i < _54; i++) set += port::colshadow::SetMix(ColShadowKey(_32C[_5C[i]]));
        for (u32 i = 0; i < _6E; i++) set += port::colshadow::SetMix(ColShadowKey(_32C[_70[i]]) ^ 0x9e3779b97f4a7c15ull);
        const TVec3f cam = MR::getCamPos();
        const TVec3f dir = MR::getCamZdir();
        st->turns++;
        if (st->turnValid && set != st->lastTurnSet) {
            st->turnChanged++;
            if (ColShadowStill(rPosition, st->lastTurnPos, cam, st->lastTurnCam, dir, st->lastTurnDir)) st->turnChangedStill++;
        }
        st->turnValid = true;
        st->lastTurnSet = set;
        st->lastTurnPos = rPosition;
        st->lastTurnCam = cam;
        st->lastTurnDir = dir;
        st->setHash = set;
        st->setCount = _54;
        if (_6E > st->maxEdge) st->maxEdge = _6E;
        if (_33C >= _58) st->capTurns += stable ? 0 : 1;  // (stable counts its own drops above; this is the game's keeper cut at the cap)
    }
#endif
    f32 tolerance = 2.0f + _310;
    _6C = 0;
    s32 index = 0;
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = *_32C[_5C[i]].getPos(j);
            u32 k;
            for (k = 0; k < _6C; k++) {
                if (MR::abs(_60[k].x - point.x) < tolerance && MR::abs(_60[k].y - point.y) < tolerance && MR::abs(_60[k].z - point.z) < tolerance) {
                    break;
                }
            }

            if (k == _6C) {
                const TVec3f& normal = *_32C[_5C[i]].getNormal(0);
                _60[_6C] = point + normal * _310;
                _6C++;
            }

            _68[index] = k;
            index++;
        }
    }

    // TODO is this legit?
    register f32 texScale = 0.0033333334f;

    for (u32 i = 0; i < _6C; i++) {
        TVec3f relative(_60[i] - rPosition);
        f32 s = relative.dot(side);
        f32 t = relative.dot(_30);

        _64[i].x = 0.5f + s * texScale;
        _64[i].y = 0.5f + t * texScale;
    }

#ifdef TARGET_PC
    const f32 radiusBefore = _10;
    if (stable) {
        _10 = port::colshadow::RadiusNext(_10, _33C, ColShadowFor(this)->radius, true);  // (never shrinks below the radius it last grew to; see port/colshadow_select.h)
    } else
#endif
    if (_33C > 40) {
        _10 -= 10.0f;
    } else if (_33C > 10) {
        _10 -= 1.0f;
    } else if (_33C > 20) {
        _10 -= 4.0f;
    } else {
        _10 += 1.0f;
    }

    MR::clampBoth(&_10, 1.0f, _14);
#ifdef TARGET_PC
    if (ColShadowLog()) {
        ColShadowState* st = ColShadowFor(this);
        if (_10 != radiusBefore) st->radiusChanges++;
        const uint32_t r = static_cast< uint32_t >(_10);
        if (!st->radMax && !st->radMin) st->radMin = r;
        if (r < st->radMin) st->radMin = r;
        if (r > st->radMax) st->radMax = r;
    }
#endif
}

void CollisionShadow::draw1() const {
    if (_308 & 8) {
        return;
    }

    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetDstAlpha(GX_TRUE, 64);
    sendVtx();
    GXSetDstAlpha(GX_FALSE, 0);
}

void CollisionShadow::sendVtx() const {
    if (!_54) {
        return;
    }

    u16 vertexCount = _54 * _5A;
    if (_5A == 4) {
        GXBegin(GX_QUADS, GX_VTXFMT0, vertexCount);
    } else {
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, vertexCount);
    }

    const TVec2f* pTex;
    u32 index = 0;
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(index);
            pTex = &_64[_68[index]];
            index++;
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(pTex->x, pTex->y);
        }
    }
}

void CollisionShadow::sendZsortedVtx(bool offset) const {
    u16 vertexCount = _54 * _5A;
    if (_5A == 4) {
        GXBegin(GX_QUADS, GX_VTXFMT0, vertexCount);
    } else {
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, vertexCount);
    }

#ifdef TARGET_PC
    TVec3f shift(-_24 * (ColShadowStable() >= 2 ? 10.0f : 5.0f));  // (2: lifted further off the floor for the depth test)
#else
    TVec3f shift(-_24 * 5.0f);
#endif
    for (u32 i = 0; i < _54; i++) {
        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            if (offset) {
                GXPosition3f32(point.x + shift.x, point.y + shift.y, point.z + shift.z);
            } else {
                GXPosition3f32(point.x, point.y, point.z);
            }

            GXTexCoord2f32(tex.x, tex.y);
        }
    }
}

void CollisionShadow::initCaptureTex() {
    _2FC = 160;
    _2FE = 160;
    {
        MR::CurrentHeapRestorer restorer(MR::getSceneHeapGDDR3());
        _300 = new JUTTexture(_2FC, _2FE, GX_TF_I8);
        MR::zeroMemory(_300->mImage, _2FC * _2FE);
        MR::setMarioShadowTex(_300);
    }

    _300->mWrapS = GX_CLAMP;
    _300->mWrapT = GX_CLAMP;
    _300->mMinType = GX_LINEAR;
    _300->mMagType = GX_LINEAR;
    _300->init();
}

void CollisionShadow::setViewMtx(const TVec3f& rDirection) {
    TVec3f normal = rDirection;
    MR::normalize(&normal);
    TVec3f eye = _2F0 - normal * 10000.0f;

    TPos3f matrix;
    matrix.identity();
    matrix.setPositionFromLookAt(eye, _30, _2F0);
    PSMTXCopy(matrix, j3dSys.mViewMtx);
    TDDraw::setViewMtx(matrix);
    MR::setMarioShadowVec(rDirection);
}

void CollisionShadow::setUpdateFlag() {
    if (!MR::isNearZero(_24)) {
        _304 = 1;
    }
}

void CollisionShadow::calcView(J3DModelX* pModel, u32 view, J3DModelX* pReference) {
    if (!_304) {
        return;
    }

    const TVec3f& rDirection = _24;
    _304 = 0;
    _306 = 1;
    if (_C == 3) {
        Mtx matrix;
        PSMTXCopy(pReference->getBaseTRMtx(), matrix);
        if (pReference != nullptr) {
            pModel->viewCalcRefPos(view, pReference, _48, _24);
        }
    } else {
        setViewMtx(rDirection);
        if (pReference != nullptr) {
            pModel->viewCalcRef(view, pReference);
        } else {
            pModel->viewCalc3(view, nullptr);
        }
    }

    if (_C == 2) {
        doSortPolygons();
    }
}

void CollisionShadow::drawAndCaptureTex(J3DModelX* pModel, const TVec3f& rPosition) {
    if (!_306) {
        return;
    }

    if (_C == 3) {
        _340 = pModel;
        return;
    }

#ifdef TARGET_PC
    // PETARI_SHADOWTEX_HALFRATE (gpu23, ImageEffectLocalUtil.cpp): on the second of two pictures the texture keeps the previous
    // picture's shadow: no corner draw, no capture (no EFB split); the state the full path leaves is set below as always.
    const bool portKeepTex = ImageEffectLocalUtil::portShadowTexReuse(this);
#else
    const bool portKeepTex = false;
#endif

    if (!(_308 & 2) && !portKeepTex) {
        setViewMtx(_24);
        TDDraw::setup(0, 0, 1);
        Mtx44 projection;
        C_MTXOrtho(projection, -150.0f, 150.0f, 150.0f, -150.0f, 10.0f, 100000.0f);
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        if (_307) {
            GXSetViewport(608 - (_2FC + 32), -32.0f, 64.0f + _2FC, 64.0f + _2FE, 0.0f, 1.0f);
            s32 left = 608 - _2FC;
            GXSetScissor(left + 1, 1, _2FC - 2, _2FE - 2);
            pModel->setDrawView(2);
            pModel->mFlags.clear();
            pModel->mFlags._11 = true;
            pModel->mFlags._1E = true;
            pModel->directDraw(nullptr);
        }

        GXSetViewport(608 - _2FC, 0.0f, _2FC, _2FE, 0.0f, 1.0f);
        s32 left = 608 - _2FC;
        GXSetScissor(left + 1, 1, _2FC - 2, _2FE - 2);
        pModel->setDrawView(2);
        pModel->mFlags.clear();
        pModel->mFlags._1E = true;
        pModel->directDraw(nullptr);
        pModel->mFlags._1E = false;
    }

    if (!(_308 & 4) && !portKeepTex) {
        TDDraw::setup(0, 1, 2);
        GXSetColorUpdate(GX_FALSE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_ZERO, GX_LO_NOOP);
        GXSetDstAlpha(GX_FALSE, 0);
        f32 y = _2FE / 2;
        f32 x = 608 - _2FC / 2;
        TVec3f center(x, y, 0.0f);
        TDDraw::fix2Dpos(&center);
        TDDraw::drawFillCircle(center, _2FC / 2, 0xFFFFFFC0, 0, 16);
        const ResTIMG* image = _300->getTexInfo();
        void* destination = const_cast< ResTIMG* >(port_offset32(image, image->mImageDataOffset));
        GXRenderModeObj* mode = JUTVideo::getManager()->getRenderMode();
        GXSetCopyFilter(GX_FALSE, mode->sample_pattern, GX_FALSE, mode->vfilter);
        JUTTexture::captureDolTexture(destination, _2FC, _2FE, 608 - _2FC, 0, false, GX_CTF_A8);
        GXSetCopyFilter(GX_FALSE, mode->sample_pattern, GX_TRUE, mode->vfilter);
        GXInvalidateTexAll();
    }

    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    J3DShape::resetVcdVatCache();
    MR::setDefaultViewportAndScissor();
    TDDraw::cameraInit3D();
    GXSetDstAlpha(GX_FALSE, 0);
}

void CollisionShadow::clearAlphaBuffer() {
    TVec2f position;
    TVec2f size(static_cast< f32 >(_2FC), static_cast< f32 >(_2FE));
    position.x = 608.0f - _2FC;
    position.y = 0.0f;
    MR::clearAlphaBuffer(0, position, size);
}

void CollisionShadow::drawVolumeBox(const TVec3f& rPosition, const TVec3f& rDepth) const {
    TVec3f up(_3C);
    TVec3f side;
    side.cross(_24, up);
    MR::normalizeOrZero(&side);
    TDDraw::setup(0, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_FALSE);
    GXSetDstAlpha(GX_FALSE, 0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXColorS10 color = {255, 255, 255, 255};
    GXSetTevColorS10(GX_TEVREG0, color);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetCullMode(GX_CULL_BACK);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
    GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetCullMode(GX_CULL_FRONT);
    TDDraw::drawFillBox3D(rPosition, rDepth * 2.0f, side * _1C, up * _18, 0xFFFFFF01);
}

void CollisionShadow::draw() const {
#ifdef TARGET_PC
    if (ColShadowLog()) {
        ColShadowState* st = ColShadowFor(this);
        const TVec3f cam = MR::getCamPos();
        const TVec3f dir = MR::getCamZdir();
        st->pics++;
        st->sumP += st->setCount;
        if (st->setCount < st->minP) st->minP = st->setCount;
        if (st->setCount > st->maxP) st->maxP = st->setCount;
        if (!st->setCount) st->empties++;
        if (st->picValid && (st->setCount > st->lastPicCount ? st->setCount - st->lastPicCount : st->lastPicCount - st->setCount) >= 5) st->swings++;
        if (st->picValid && (st->setHash != st->lastPicSet || st->setCount != st->lastPicCount)) {
            st->changed++;
            if (ColShadowStill(_2F0, st->lastPicPos, cam, st->lastPicCam, dir, st->lastPicDir)) st->changedStill++;
        }
        st->picValid = true;
        st->lastPicSet = st->setHash;
        st->lastPicCount = st->setCount;
        st->lastPicPos = _2F0;
        st->lastPicCam = cam;
        st->lastPicDir = dir;
        if (st->pics >= 300) {
            port_log_summary("colshadow: mode %d, %u pictures: polygons min/max/mean %u/%u/%.1f, set changed %u (while Mario and camera still: %u), turns %u (set changed %u, still %u), "
                             "cap hit %u turns (dropped %u), empty pictures %u, edge list max %u, radius changes %u (box half-size %u..%u), picture-to-picture polygon count swings >= 5: %u; "
                             "verdict (bugs26): %s",
                             ColShadowStable(), st->pics, st->minP, st->maxP, static_cast< double >(st->sumP) / st->pics, st->changed, st->changedStill, st->turns, st->turnChanged,
                             st->turnChangedStill, st->capTurns, st->dropped, st->empties, st->maxEdge, st->radiusChanges, st->radMin, st->radMax, st->swings,
                             (st->changedStill + st->turnChangedStill) == 0 ? "STABLE (no set change while Mario and the camera were still)"
                             : (st->changedStill + st->turnChangedStill) <= 3 ? "STABLE enough (a rare change while still; a moving part can cause one)"
                                                                               : (ColShadowStable() ? "UNSTABLE even with the stable selection: send this line" : "FLICKER signature (GXV_COLSHADOW_STABLE=1 removes it)"));
            st->pics = st->changed = st->changedStill = st->turns = st->turnChanged = st->turnChangedStill = st->capTurns = st->dropped = st->empties = st->maxEdge = st->radiusChanges = st->swings = 0;
            st->radMin = st->radMax = 0;
            st->minP = 0xFFFFFFFFu;
            st->maxP = 0;
            st->sumP = 0;
        }
    }
#endif
    MR::loadViewMtx();
    MR::loadProjectionMtx();
    switch (_C) {
    case 0:
        draw1();
        break;
    case 1:
        draw2();
        break;
    case 2:
        draw3();
        break;
    case 3:
        drawVolume();
        break;
    }

    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
}

void CollisionShadow::draw2() const {
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_GREATER, 1, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetDstAlpha(GX_TRUE, 0);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    sendVtx();
    GXSetDstAlpha(GX_FALSE, 0);
}

void CollisionShadow::draw3() const {
    if (_308 & 8) {
        return;
    }

    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    TDDraw::setup(1, 1, 0);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_NOOP);
    GXSetAlphaCompare(GX_EQUAL, 0, GX_AOP_AND, GX_EQUAL, 0);
    GXSetDstAlpha(GX_TRUE, 0);
    _300->load(GX_TEXMAP0);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    sendZsortedVtx(true);
    if (_6E) {
        GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, _6E * 3);
        for (u32 i = 0; i < _6E; i++) {
            for (u32 j = 0; j < 3; j++) {
                const TVec3f& point = *_32C[_70[i]].getPos(j);
                GXPosition3f32(point.x, point.y, point.z);
                GXTexCoord2f32(-1.0f, -1.0f);
            }
        }
    }

    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetColorUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_NEQUAL, 0, GX_AOP_AND, GX_NEQUAL, 0);
    GXSetMisc(GX_MT_XF_FLUSH, 8);
#ifdef TARGET_PC
    const int clipMode = _5A == 3 ? ColShadowClip() : 0;
#endif
    for (u32 i = 0; i < _54; i++) {
#ifdef TARGET_PC
        if (clipMode != 0) {  // (GXV_COLSHADOW_CLIP: both passes of this triangle use the same cut polygon; a triangle wholly outside has no fragment that passes the alpha test: neither pass is drawn)
            const TVec3f* pos[3] = {&getDrawPos(i, 0), &getDrawPos(i, 1), &getDrawPos(i, 2)};
            const TVec2f* tex[3] = {&getDrawTx(i, 0), &getDrawTx(i, 1), &getDrawTx(i, 2)};
            port::colshadow::ClipVtx cv[port::colshadow::kClipMax];
            const int m = ClipPolygon(pos, tex, cv);
            if (clipMode == 1) {
                if (m < 3) {
                    continue;
                }
                GXSetColorUpdate(GX_FALSE);
                GXSetDstAlpha(GX_FALSE, 0);
                GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast< u16 >(3 * (m - 2)));
                EmitClipped(cv, m);
                GXSetColorUpdate(GX_TRUE);
                GXSetDstAlpha(GX_TRUE, 0);
                GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast< u16 >(3 * (m - 2)));
                EmitClipped(cv, m);
                continue;
            }
        }
#endif
        GXSetColorUpdate(GX_FALSE);
        GXSetDstAlpha(GX_FALSE, 0);
        if (_5A == 4) {
            GXBegin(GX_QUADS, GX_VTXFMT0, _5A);
        } else {
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, _5A);
        }

        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(tex.x, tex.y);
        }

        GXSetColorUpdate(GX_TRUE);
        GXSetDstAlpha(GX_TRUE, 0);
        if (_5A == 4) {
            GXBegin(GX_QUADS, GX_VTXFMT0, _5A);
        } else {
            GXBegin(GX_TRIANGLES, GX_VTXFMT0, _5A);
        }

        for (u32 j = 0; j < _5A; j++) {
            const TVec3f& point = getDrawPos(i, j);
            const TVec2f& tex = getDrawTx(i, j);
            GXPosition3f32(point.x, point.y, point.z);
            GXTexCoord2f32(tex.x, tex.y);
        }
    }

#ifdef TARGET_PC
    if (clipMode != 0) {
        port::colshadow::ClipCensus& c = gClipCensus;
        if (++c.pictures % 300 == 0) {  // (per 300 pictures that drew the list; polygons are counted once per picture)
            port_log_summary("colshadow-clip: mode %d pictures %u polygons/picture %u outside %.1f%% cut %.1f%% unchanged %.1f%% uv-area kept %.3f%% (drawn tris %u of %u)", clipMode, static_cast< u32 >(c.pictures),
                             static_cast< u32 >(c.polygons / c.pictures), c.polygons ? 100.0 * c.outside / c.polygons : 0.0, c.polygons ? 100.0 * c.cut / c.polygons : 0.0,
                             c.polygons ? 100.0 * c.inside / c.polygons : 0.0, c.areaIn > 0 ? 100.0 * c.areaOut / c.areaIn : 0.0, static_cast< u32 >(c.trisOut), static_cast< u32 >(c.trisIn));
        }
    }
#endif
    GXSetMisc(GX_MT_XF_FLUSH, 0);
    GXSetDstAlpha(GX_FALSE, 0);
}

void CollisionShadow::createDL() {
    u8 buffer[512] ATTRIBUTE_ALIGN(32);
    GDLObj displayList;
    MR::ProhibitSchedulerAndInterrupts guard(false);
    GDInitGDLObj(&displayList, buffer, sizeof(buffer));
    GDSetCurrent(&displayList);
    GDSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GDSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GDSetTevDirect(GX_TEVSTAGE0);
    GDSetGenMode(0, 1, 1);
    GDSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_CLAMP, GX_AF_NONE);
    GDSetTevAlphaCalcAndSwap(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV,
                             GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXColor color = {0, 0, 0, 4};
    GDSetTevColor(GX_TEVREG0, color);
    GDPadCurr32();
    u32 size = GDGetGDLObjOffset(&displayList);
    _348 = size;
    size = (size + 31) & ~31;
    _344 = new (32) u8[size];
    MR::copyMemory(_344, buffer, size);
    DCStoreRange(_344, size);
}

void CollisionShadow::drawVolume() const {
    if (_308 & 8) {
        return;
    }

    _340->setDrawView(2);
    TVec3f position(_2F0 - _24 * _30C);
    drawVolumeBox(position, _24 * _20);
    _340->mFlags.clear();
    _340->mFlags._15 = true;
    _340->directDraw(nullptr);
    GXSetDstAlpha(GX_FALSE, 0);
}

void CollisionShadow::doSortPolygons() {
    f32 distances[128];
    f32 inverseVertexCount = 0.33333334f;
    for (u32 i = 0; i < _54; i++) {
        TVec3f center(getDrawPos(0));
        center += getDrawPos(1);
        center += getDrawPos(2);
        center.scale(inverseVertexCount);
        TVec3f relative(center);
        relative.sub(_2F0);
        distances[i] = relative.dot(_24);
    }

    MR::sortSmall(_54, distances, _F0);
}

const TVec3f& CollisionShadow::getDrawPos(u32 index) const {
    return _60[_68[index]];
}

const TVec3f& CollisionShadow::getDrawPos(u32 polygon, u32 vertex) const {
    return _60[_68[vertex + _5A * _F0[polygon]]];
}

const TVec2f& CollisionShadow::getDrawTx(u32 polygon, u32 vertex) const {
    return _64[_68[vertex + _5A * _F0[polygon]]];
}
