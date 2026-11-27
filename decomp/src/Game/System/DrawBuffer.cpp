#include "Game/System/DrawBuffer.hpp"
#include "Game/LiveActor/ActorLightCtrl.hpp"
#include "Game/System/ShapePacketUserData.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ModelUtil.hpp"
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <cstring>
#ifdef TARGET_PC
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DPortDL.hpp>
#include <JSystem/J3DGraphBase/J3DPortSort.hpp>  // (GXV_STARBATCH: the plan of a drawer's order of packets)
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include "Game/Map/LightFunction.hpp"
#include "port/gt71.h"  // (gt71: exact game-thread overhead cuts, one switch GXV_GT71)
#include "port/stream_census.h"  // (GXV_STREAM_CENSUS: each packet drawn is its model's)
#include "port/compat.h"  // (GXV_HRW, hrw28: PORT_GX_OWNER, port_gx_marker)
#include "port/draw_pipe.h"  // (GXV_DRAWPIPE, drawpipe24: a drawer's packets from the live list or from a frame frozen at the hand-over)
#include "port/gx_fifo.h"  // (GXV_REPLAY25: the stream's write position)
#include "port/array_skip.h"
#include "port/shapecull_math.h"  // (cpu24: the plane test, with a host test against the original)
#include "port/fpbits.h"  // (neonbox40: shapecull::Out and the front end compare floats without vcmp / vmrs)
#include <cmath>
#include "port/hot_knob.h"
#include "port/turn_prof.h"  // (GXV_STARBATCH: port_turn_prof_on)
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include <chrono>
#include <type_traits>
#include <cstdlib>
#include <cstdio>
extern "C" void port_log_summary(const char* fmt, ...);  // (port_log is a no-op in the player build, PORT_RUNTIME_DIAGNOSTICS=0: census lines go to log.txt through port_log_summary)
extern "C" u32 __PortGXDrawSerial;  // (GXDisplayList.c)
#endif

DrawBufferShapeDrawer::DrawBufferShapeDrawer(J3DMaterial* pMaterial, J3DMatPacket* pMatPacket)
    : mMaterial(pMaterial), mMatPacket(pMatPacket), _8(1), mMaxPackets(0), mNumPackets(0), mPackets(nullptr) {
#ifdef TARGET_PC
    mMergedDL = nullptr;
    mMergedCap = 0;
    mMergedSize = 0;
    mMergedMatSize = 0;
    mMergedDiffSize = 0;
    mMergedLastCall = 0;
    mMergedQuietUntil = 0;
    mMergedChanges = 0;
    mMergedGen = 0;
    mPortSerial = 0;
    mPortFamily = nullptr;
#endif
}

void DrawBufferShapeDrawer::init(s32 a1) {
    mMaxPackets = a1 * _8;
    mPackets = new PacketInfo*[mMaxPackets];

    for (s32 idx = 0; idx < mMaxPackets; idx++) {
        PacketInfo* packet = new PacketInfo();
        mPackets[idx] = packet;
        mPackets[idx]->mShapePacket = nullptr;
        mPackets[idx]->mLightLoader = nullptr;
        mPackets[idx]->mLightCtrl = nullptr;
#ifdef TARGET_PC
        mPackets[idx]->mPortReplay = nullptr;
#endif
    }
}

void DrawBufferShapeDrawer::swap(DrawBufferShapeDrawer* pOther) {
    J3DMaterial* material = mMaterial;
    J3DMatPacket* matPacket = mMatPacket;
    s32 __8 = _8;

    mMaterial = pOther->mMaterial;
    mMatPacket = pOther->mMatPacket;
    _8 = pOther->_8;

    pOther->mMaterial = material;
    pOther->mMatPacket = matPacket;
    pOther->_8 = __8;
#ifdef TARGET_PC
    // (made at the table's sorting, before any draw: no merged list yet; dropped, so that none follows the wrong material)
    mMergedDL = pOther->mMergedDL = nullptr;
    mMergedCap = pOther->mMergedCap = 0;
    mMergedSize = pOther->mMergedSize = 0;
    mMergedMatSize = pOther->mMergedMatSize = 0;
    mMergedDiffSize = pOther->mMergedDiffSize = 0;
    ++mMergedGen;
    ++pOther->mMergedGen;
    ++mPortSerial;
    ++pOther->mPortSerial;
#endif
}

#ifdef TARGET_PC
// GXV_DRAWPIPE: moved by every change of a drawer's packet list (add, remove, a light sort: the only writers of the lists the J3D
// passes walk) and of a model's light type. A frame frozen at another epoch is not the live lists any more.
u32 gPortDrawPipeEpoch;
// GXV_HRW (hrw28): see DrawBuffer.hpp
u32 gPortHrwPhase = 0;
const J3DModel* gPortHrwOnly = nullptr;

// ============================================================== GXV_SHAPECULL (cpu34d, 14 Nov; default 0)
// A rigid shape whose every vertex is outside one side plane of the view pyramid (or behind the eye) puts no pixel in the picture. The
// front end finds that out (GXV_STATICCULL: 71 of 280 static runs a picture in the Comet Observatory, 2900a) only after the game thread
// has sent the packet's differed list, array bases, matrix index and list call, and after the front end has parsed them, found the record
// and resolved the draw's state. With the switch on the game thread makes the test first and leaves such a packet out as it leaves a
// hidden shape out (the branch GXV_PLANT_DRAWCULL uses for a whole model): its light loader still runs, its differed list and its shape
// are not sent, the packets after it see what they see after a hidden one.
//   Which shapes: one matrix group, a plain J3DShapeMtx ('SMTX': not billboard, not multi-matrix, not concat-view), no position-matrix
//   index per vertex, no LOD / no-matrix flag, a model without CPU skinning or LOD (pipeline 0: J3DShapeMtx::loadMtxIndx_PNGP, the
//   indexed load of row `useMtxIndex` of the draw-matrix array of the buffer's current view), positions read from the model data's own
//   array (not a deformed copy). Everything else is drawn as before.
//   The box: NOT the model file's shape box. A host check of 1200 such shapes in 251 of the game's models found 20 whose vertices lie
//   outside it (it is in model space, the vertices of a shape rigid to a moved joint are joint-local; the joints' boxes are no better:
//   13 wrong, most are zero). The box is made here from the shape's own list: each vertex's position index (the list is the GX stream,
//   big-endian; the array is host data), once per shape, at most GXV_SHAPECULL_BUDGET (4000) vertices a picture; a shape without a box
//   yet is drawn. Kept in a table of 2048 (shape, position array, list, size); emptied when a scene goes (port_shapecull_reset).
//   The test: the box's centre and half extents through the draw matrix, against the four side planes of the projection the GX library
//   holds at this moment (GXGetProjectionv: what the stream last carried, so any pass's own), each moved out by GXV_SHAPECULL_MARGIN
//   (4 %) and by rounding, and against the eye plane. Perspective only. The same or a stricter answer than the front end's test.
//   Off (with one log line) under GXV_OVERSCAN (the back end widens the projection), GXV_J3DNATIVE (its membership version),
//   GXV_PIPE25 / GXV_DRAWPIPE (the draw on another thread); never in the pair's partial draw (GXV_HRW fresh player).
//   0 off; 1 on; 2 verify: nothing is left out; every shape the test would leave out has ALL its vertices put through the same matrix
//   and the plane it failed, without the margin (a vertex must be outside it or behind the eye): "outside-plane failures" must be 0.
//   Line "shapecull:" every 300 pictures. Host test of the arithmetic: new/cpu34d/t_shapecull.cpp (400,000 boxes, 0 left out in view).
extern "C" void port_par_view_touch_out(const void* out);  // (ModelManager.cpp, GXV_PAR_VIEW: the draw matrices are read below)
namespace shapecull {
    struct Slot {
        const J3DShape* shape;
        const void* pos;
        const void* data;  // (the model data: the same shape address under other data is another shape)
        const u8* dl;
        u32 dlSize;
        f32 c[3], h[3];
        f32 ch[3];  // (cpu24: |c| + h, made with c and h)
        u8 state;  // 0 free, 1 box, 2 no box can be made
    };
    enum { kSlots = 2048, kProbe = 12 };
    Slot sSlots[kSlots];
    struct Stats {
        u32 tested, out, notRigid, noBox, ortho, made, madeVerts, unusable, full, verifyShapes, verifyVerts, verifyBad, fileSame;
    };
    Stats sStats;
    u32 sBudgetPicture, sBudgetLeft, sLogPicture;
    inline int EnvI(const char* name, int fallback) {
        const char* e = getenv(name);
        return e != nullptr && *e != 0 ? atoi(e) : fallback;
    }
    int ModeRead() {
        const int m = EnvI("GXV_SHAPECULL", 0);
        if (m != 1 && m != 2) {
            return 0;
        }
        if (EnvI("GXV_OVERSCAN", 0) != 0 || EnvI("GXV_J3DNATIVE", 0) != 0 || EnvI("GXV_PIPE25", 0) != 0 || EnvI("GXV_DRAWPIPE", 0) != 0) {
            port_log_summary("shapecull: GXV_SHAPECULL=%d asked, OFF: not with GXV_OVERSCAN / GXV_J3DNATIVE / GXV_PIPE25 / GXV_DRAWPIPE", m);
            return 0;
        }
        port_log_summary("shapecull: GXV_SHAPECULL=%d (margin %d %%, %d vertices of new boxes a picture)", m, EnvI("GXV_SHAPECULL_MARGIN", 4),
                         EnvI("GXV_SHAPECULL_BUDGET", 4000));
        return m;
    }
    inline int Mode() {
        return PORT_HOT_KNOB(int, ModeRead());
    }
    inline bool RefMode() {  // (cpu24: GXV_SHAPECULL_REF=1: the original plane loop)
        return PORT_HOT_KNOB(int, EnvI("GXV_SHAPECULL_REF", 0)) != 0;
    }
    inline f32 Margin() {
        return PORT_HOT_KNOB(f32, [] {
            const int v = EnvI("GXV_SHAPECULL_MARGIN", 4);
            return 1.0f + 0.01f * static_cast< f32 >(v < 0 ? 0 : v > 50 ? 50 : v);
        }());
    }
    inline u32 Budget() {
        return PORT_HOT_KNOB(u32, [] {
            const int v = EnvI("GXV_SHAPECULL_BUDGET", 4000);
            return static_cast< u32 >(v < 0 ? 0 : v);
        }());
    }
    // The shape's vertices, in the order of its list: f(x, y, z) for each; false: the list or the format is not one this reads.
    template < class F >
    bool ForVerts(const J3DShape* shape, const J3DVertexData& vd, const void* pos, const u8* dl, u32 size, u32* count, F&& f) {
        u32 vsize = 0, posOff = 0, posSz = 0;
        const GXVtxDescList* d = const_cast< J3DShape* >(shape)->getVtxDesc();
        if (d == nullptr) {
            return false;
        }
        for (u32 guard = 0; d->attr != GX_VA_NULL; ++d) {
            if (++guard > 26) {
                return false;
            }
            u32 z;
            if (d->type == GX_NONE) {
                continue;
            } else if (d->type == GX_DIRECT) {
                if (d->attr > GX_VA_TEX7MTXIDX) {
                    return false;  // (a direct component: its size is the format's, not read here)
                }
                z = 1;
            } else if (d->type == GX_INDEX8) {
                z = 1;
            } else if (d->type == GX_INDEX16) {
                z = 2;
            } else {
                return false;
            }
            if (d->attr == GX_VA_POS) {
                if (d->type == GX_DIRECT) {
                    return false;
                }
                posOff = vsize;
                posSz = z;
            }
            vsize += z;
        }
        int type = vd.getVtxPosType();
        u32 frac = vd.getVtxPosFrac();
        const GXVtxAttrFmtList* fl = vd.getVtxAttrFmtList();
        for (u32 guard = 0; fl != nullptr && fl->attr != GX_VA_NULL && guard < 26; ++fl, ++guard) {
            if (fl->attr == GX_VA_POS) {
                if (fl->cnt != GX_POS_XYZ) {
                    return false;
                }
                type = fl->type;
                frac = fl->frac;
            }
        }
        const u32 num = vd.getVtxNum();
        if (posSz == 0 || num == 0 || pos == nullptr || dl == nullptr || (type != GX_F32 && type != GX_S16) || frac > 15) {
            return false;
        }
        const f32 scale = 1.0f / static_cast< f32 >(1u << frac);
        u32 n = 0;
        const u8* q = dl;
        const u8* const end = dl + size;
        while (q < end) {
            const u8 op = *q;
            if (op == 0) {
                ++q;  // (padding)
                continue;
            }
            if (op < 0x80 || op > 0xBF || end - q < 3) {
                return false;
            }
            const u32 c = (static_cast< u32 >(q[1]) << 8) | q[2];
            q += 3;
            if (static_cast< u32 >(end - q) < c * vsize) {
                return false;
            }
            for (u32 i = 0; i < c; ++i, q += vsize) {
                const u32 ix = posSz == 1 ? q[posOff] : ((static_cast< u32 >(q[posOff]) << 8) | q[posOff + 1]);
                if (ix >= num) {
                    return false;
                }
                f32 x, y, z;
                if (type == GX_F32) {
                    f32 v[3];
                    memcpy(v, static_cast< const u8* >(pos) + 12u * ix, 12);
                    x = v[0];
                    y = v[1];
                    z = v[2];
                } else {
                    s16 v[3];
                    memcpy(v, static_cast< const u8* >(pos) + 6u * ix, 6);
                    x = static_cast< f32 >(v[0]) * scale;
                    y = static_cast< f32 >(v[1]) * scale;
                    z = static_cast< f32 >(v[2]) * scale;
                }
                if (!(x == x) || !(y == y) || !(z == z)) {
                    return false;
                }
                f(x, y, z);
                ++n;
            }
        }
        *count = n;
        return n != 0;
    }
    inline u32 Hash(const J3DShape* shape) {
        return (static_cast< u32 >(reinterpret_cast< uintptr_t >(shape) >> 3) * 2654435761u) >> 21;  // (11 bits: kSlots)
    }
    // The shape's slot with this key, made now when the picture's budget allows; null: no box (yet).
    Slot* Find(const J3DShape* shape, const void* data, const J3DVertexData& vd, const void* pos, const u8* dl, u32 size) {
        const u32 h0 = Hash(shape);
        Slot* free = nullptr;
        for (u32 k = 0; k < kProbe; ++k) {
            Slot* s = &sSlots[(h0 + k) & (kSlots - 1)];
            if (s->state == 0) {
                free = s;
                break;
            }
            if (s->shape == shape) {
                if (s->pos == pos && s->data == data && s->dl == dl && s->dlSize == size) {
                    return s;
                }
                free = s;  // (the same shape with other data: its box is made again)
                break;
            }
        }
        if (free == nullptr) {
            ++sStats.full;
            return nullptr;
        }
        const u32 picture = J3DPortPicture();
        if (sBudgetPicture != picture) {
            sBudgetPicture = picture;
            sBudgetLeft = Budget();
        }
        if (sBudgetLeft == 0) {
            return nullptr;
        }
        f32 mn[3] = {3.0e38f, 3.0e38f, 3.0e38f}, mx[3] = {-3.0e38f, -3.0e38f, -3.0e38f};
        u32 n = 0;
        const bool ok = ForVerts(shape, vd, pos, dl, size, &n, [&](f32 x, f32 y, f32 z) {
            mn[0] = x < mn[0] ? x : mn[0];
            mx[0] = x > mx[0] ? x : mx[0];
            mn[1] = y < mn[1] ? y : mn[1];
            mx[1] = y > mx[1] ? y : mx[1];
            mn[2] = z < mn[2] ? z : mn[2];
            mx[2] = z > mx[2] ? z : mx[2];
        });
        sBudgetLeft = sBudgetLeft > n + 16 ? sBudgetLeft - n - 16 : 0;
        free->shape = shape;
        free->pos = pos;
        free->data = data;
        free->dl = dl;
        free->dlSize = size;
        if (!ok) {
            free->state = 2;
            ++sStats.unusable;
            return free;
        }
        for (int i = 0; i < 3; ++i) {
            free->c[i] = 0.5f * (mn[i] + mx[i]);
            const f32 a = mn[i] < 0 ? -mn[i] : mn[i], b = mx[i] < 0 ? -mx[i] : mx[i];
            free->h[i] = 0.5f * (mx[i] - mn[i]) + 1.0e-5f * (a > b ? a : b) + 1.0e-6f;  // (the halving's and the centre's rounding)
        }
        for (int i = 0; i < 3; ++i) {
            free->ch[i] = std::fabs(free->c[i]) + free->h[i];  // (the value Out used to make per plane: same two operations)
        }
        free->state = 1;
        {
            // (a check of the reader, for the log: most shapes' own boxes are the model file's; see the head)
            const Vec* fmn = const_cast< J3DShape* >(shape)->getMin();
            const Vec* fmx = const_cast< J3DShape* >(shape)->getMax();
            const f32 fm[6] = {fmn->x, fmn->y, fmn->z, fmx->x, fmx->y, fmx->z};
            const f32 om[6] = {mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]};
            bool same = true;
            for (int i = 0; i < 6; ++i) {
                const f32 dlt = fm[i] - om[i], mgn = om[i] < 0 ? -om[i] : om[i];
                if (!((dlt < 0 ? -dlt : dlt) <= 0.01f + 0.002f * mgn)) {
                    same = false;
                }
            }
            sStats.fileSame += same ? 1u : 0u;
        }
        ++sStats.made;
        sStats.madeVerts += n;
        return free;
    }
    void Log(int mode) {
        const u32 picture = J3DPortPicture();
        if (picture - sLogPicture < 300) {
            return;
        }
        const double n = static_cast< double >(picture - sLogPicture);
        sLogPicture = picture;
        const Stats& s = sStats;
        port_log_summary("shapecull: GXV_SHAPECULL=%d, a picture: rigid shapes tested %.1f, left out %.1f%s; not tested: not rigid %.1f, no box yet / none "
                         "%.1f, orthographic %.1f; boxes made %u (%u vertices; %u equal to the model file's box), unreadable lists %u, table full %u; verify: %u shapes, %u vertices, %u "
                         "outside-plane failures (must be 0)",
                         mode, s.tested / n, s.out / n, mode == 2 ? " (verify: drawn all the same)" : "", s.notRigid / n, s.noBox / n, s.ortho / n, s.made,
                         s.madeVerts, s.fileSame, s.unusable, s.full, s.verifyShapes, s.verifyVerts, s.verifyBad);
        sStats = Stats();
    }
    // true: the packet's shape is wholly outside the view of the projection in force (see above). Mode 2: always false.
    bool Out(const J3DShapePacket* sp) {
        const int mode = Mode();
        if (__builtin_expect(mode == 0, 1) || gPortHrwOnly != nullptr) {
            return false;
        }
        Log(mode);
        const J3DShape* shape = sp->mpShape;
        J3DModel* model = sp->mpModel;
        J3DMtxBuffer* mb = sp->mpMtxBuffer;
        if (shape == nullptr || model == nullptr || mb == nullptr || shape->mMtxGroupNum != 1 || shape->mHasPNMTXIdx ||
            (shape->mFlags & (J3DShpFlag_EnableLod | J3DShpFlag_NoMtx)) != 0 || shape->mShapeMtx == nullptr || shape->mShapeMtx[0] == nullptr ||
            shape->mShapeDraw == nullptr || shape->mShapeDraw[0] == nullptr || (model->mFlags & 0x1Cu) != 0 ||
            shape->mShapeMtx[0]->getType() != 'SMTX') {
            ++sStats.notRigid;
            return false;
        }
        const J3DVertexData& vd = model->getModelData()->getVertexData();
        const void* pos = model->getVertexBuffer()->mCurrentVtxPos;
        if (pos == nullptr || pos != vd.getVtxPosArray()) {
            ++sStats.notRigid;  // (a deformed copy of the positions)
            return false;
        }
        f32 pj[7];
        GXGetProjectionv(pj);
        if ((fpb::Bits(pj[0]) & 0x7FFFFFFFu) != 0) {  // (pj[0] != 0.0f: a NaN is not zero either; neonbox40: no vcmp)
            ++sStats.ortho;
            return false;
        }
        const Slot* s = Find(shape, model->getModelData(), vd, pos, shape->mShapeDraw[0]->getDisplayList(), shape->mShapeDraw[0]->getDisplayListSize());
        if (s == nullptr || s->state != 1) {
            ++sStats.noBox;
            return false;
        }
        const u32 view = *mb->getCurrentViewNoPtr();
        Mtx** dm = mb->getDrawMtxPtrPtr();
        if (dm == nullptr || dm[view] == nullptr) {
            ++sStats.noBox;
            return false;
        }
        port_par_view_touch_out(dm[view]);  // (GXV_PAR_VIEW: the view pass's job for this buffer is finished first, as the draw's own read does)
        const f32(*m)[4] = dm[view][shape->mShapeMtx[0]->getUseMtxIndex(0)];
        ++sStats.tested;
        // The box in view space: centre v, and for a plane n.p >= 0 the reach r = sum |n . column_i| h_i. Outside: n.v + r < -rounding.
        const f32* c = s->c;
        const f32* h = s->h;
        f32 v[3];
        for (int i = 0; i < 3; ++i) {
            v[i] = m[i][0] * c[0] + m[i][1] * c[1] + m[i][2] * c[2] + m[i][3];
            if ((fpb::Bits(v[i]) & 0x7FFFFFFFu) > 0x7F800000u) {  // (a NaN: !(v == v))
                return false;
            }
        }
        const f32 g = Margin();
        // clip x = pj1 x + pj2 z, clip y = pj3 y + pj4 z, w = -z; inside: -w <= x <= w, -w <= y <= w. With the margin: w' = g w.
        // planes (a, b, cz): a x + b y + cz z >= 0: 0 x + w' >= 0 ... see port/shapecull_math.h (cpu24: the sparse planes without their zero products;
        // GXV_SHAPECULL_REF=1 runs the original loop for an A/B: the same answers).
        const int plane = RefMode() ? shapecull_math::RefPlane(m, c, h, v, pj, g) : shapecull_math::FastPlane(m, c, h, s->ch, v, pj, g);
        if (plane < 0) {
            return false;
        }
        ++sStats.out;
        if (mode != 2) {
            return true;
        }
        // verify: every vertex through the matrix against the plane it failed, without the margin
        ++sStats.verifyShapes;
        const f32 pl[5][3] = {
            {pj[1], 0.0f, pj[2] - g}, {-pj[1], 0.0f, -pj[2] - g}, {0.0f, pj[3], pj[4] - g}, {0.0f, -pj[3], -pj[4] - g}, {0.0f, 0.0f, -1.0f},
        };
        f32 a = pl[plane][0], b = pl[plane][1], cz = pl[plane][2];
        if (plane < 4) {
            cz += g - 1.0f;  // (the plane of the projection itself)
        }
        u32 n = 0, bad = 0;
        ForVerts(shape, vd, pos, s->dl, s->dlSize, &n, [&](f32 x, f32 y, f32 z) {
            const f32 vx = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
            const f32 vy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
            const f32 vz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
            // (behind the eye nothing shows; there the moved plane is not inside the projection's own, and need not be)
            if (!(a * vx + b * vy + cz * vz < 0.0f) && !(vz > 0.0f)) {
                ++bad;
            }
        });
        sStats.verifyVerts += n;
        if (bad != 0) {
            sStats.verifyBad += bad;
            port_log_summary("shapecull: VERIFY FAILED: shape %p of model %p: %u of %u vertices not outside plane %d", static_cast< const void* >(shape),
                             static_cast< void* >(model), bad, n, plane);
        }
        return false;
    }
}  // namespace shapecull
// (a scene is about to go: its shapes' addresses will be other shapes'; GameSystemSceneController, beside port_dl_flush_stale(1))
extern "C" void port_shapecull_reset(void) {
    memset(shapecull::sSlots, 0, sizeof shapecull::sSlots);
}

// GXV_DRAWPIPE (drawpipe24; port/draw_pipe.h): where a drawer's packets come from. The live source is the drawer's own list, read
// as draw() always read it. The frozen source is the copy made at the hand-over (DrawBufferHolder::portPipeFreeze): the packets in
// the order they had then, and for each light loader the bytes it would have loaded then (ActorLightCtrl::loadLight: its blended
// light while it blends, else its area light's; nothing without an area light). The draw functions below are written once, over
// either source, so the two cannot drift apart; with the live source they are the functions they were.
namespace {
    struct PortLivePackets {
        const DrawBufferShapeDrawer* mDrawer;
        s32 num() const { return mDrawer->mNumPackets; }
        u32 serial() const { return mDrawer->mPortSerial; }
        u32 ext() const { return J3DPortExtSerial(); }
        J3DShapePacket* shape(s32 idx) const { return mDrawer->mPackets[idx]->mShapePacket; }
        bool hasLight(s32 idx) const { return mDrawer->mPackets[idx]->mLightLoader != nullptr; }
        void loadLight(s32 idx) const { mDrawer->mPackets[idx]->mLightLoader->loadLight(); }
        // (the packet's shape is hidden; its model is left out of this picture: GXV_PLANT_DRAWCULL, see drawPort)
        bool hidden(s32 idx) const { return (shape(idx)->mpShape->mFlags & 1) != 0; }
        bool culled(s32 idx, u32 cullPicture) const {
            const J3DShapePacket* packet = shape(idx);
            return packet->getModel() != nullptr && packet->getModel()->mPortCullPicture == cullPicture;
        }
        void draw(s32 idx) const { shape(idx)->drawFast(); }
        bool shapeOut(s32 idx) const { return shapecull::Out(shape(idx)); }  // (GXV_SHAPECULL)
    };
    struct PortFrozenPackets {
        const port::drawpipe::Frame* mFrame;
        const port::drawpipe::FrozenPacket* mPackets;
        s32 mNum;
        u32 mSerial;
        s32 num() const { return mNum; }
        u32 serial() const { return mSerial; }
        u32 ext() const { return mFrame->extSerial; }  // (the scene / fog / fade serial as the hand-over saw it: the next update moves it)
        J3DShapePacket* shape(s32 idx) const { return static_cast< J3DShapePacket* >(mPackets[idx].shapePacket); }
        bool hasLight(s32 idx) const { return mPackets[idx].light != -2; }
        // (as the hand-over saw them: the next update may hide a shape, and writes the cull stamp of the picture after this one)
        bool hidden(s32 idx) const { return (mPackets[idx].flags & port::drawpipe::kFrozenHidden) != 0; }
        bool culled(s32 idx, u32) const { return (mPackets[idx].flags & port::drawpipe::kFrozenCulled) != 0; }
        bool shapeOut(s32) const { return false; }  // (GXV_SHAPECULL: off with GXV_DRAWPIPE)
        // J3DShapePacket::drawFast, with the packet's own hide flag (MR::hideMaterial) as the hand-over saw it
        void draw(s32 idx) const {
            J3DShapePacket* packet = shape(idx);
            if ((mPackets[idx].flags & port::drawpipe::kFrozenPacketHidden) != 0 || packet->mpShape == nullptr) {
                return;
            }
            packet->prepareDraw();
            if (packet->getTexMtxObj() != nullptr) {
                J3DDifferedTexMtx::sTexGenBlock = packet->mpShape->getMaterial()->getTexGenBlock();
                J3DDifferedTexMtx::sTexMtxObj = packet->getTexMtxObj();
            } else {
                J3DDifferedTexMtx::sTexGenBlock = nullptr;
            }
            packet->mpShape->drawFast();
        }
        void loadLight(s32 idx) const {
            if (mPackets[idx].light >= 0) {
                LightFunction::loadActorLightInfo(reinterpret_cast< const ActorLightInfo* >(&mFrame->lights[static_cast< u32 >(mPackets[idx].light)]));
            }
        }
    };

    // (GXV_STREAM_CENSUS, port/stream_census.h: in a counted picture, what a packet sends is its model's; one load otherwise)
    inline void PortCensusModelBegin(const J3DShapePacket* pPacket) {
        if (__builtin_expect(port_sc_turn != 0, 0)) {
            const J3DModel* pModel = pPacket->getModel();
            port_stream_census_model_begin2(pModel != nullptr ? static_cast< const void* >(pModel->mModelData) : nullptr, pModel != nullptr ? pModel->mPortCensusStamp : 0u);
        }
    }
    inline void PortCensusModelEnd() {
        if (__builtin_expect(port_sc_turn != 0, 0)) {
            port_stream_census_model_end();
        }
    }

    // GXV_HRW (hrw28): the draws of a model carrying a unit's id are made under the owner marker (id << 8 | the pass's phase), so that the pair's second picture can take them out of the retained picture
    // and put the model's fresh draws in their place. One load of a global when no pair is tagging (the only case without HRW).
    struct PortHrwTagScope {
        bool on;
        explicit PortHrwTagScope(const J3DShapePacket* pPacket) : on(false) {
            if (__builtin_expect(gPortHrwPhase != 0, 0)) {
                const J3DModel* pModel = pPacket->getModel();
                if (pModel != nullptr && pModel->mPortHrwTag != 0) {
                    port_gx_marker(PORT_GX_OWNER(pModel->mPortHrwTag & 0xFFu, gPortHrwPhase));
                    on = true;
                }
            }
        }
        ~PortHrwTagScope() {
            if (on) {
                port_gx_marker(PORT_GX_OWNER(0, 0));
            }
        }
    };

    template < class Packets >
    void portDrawPlain(const DrawBufferShapeDrawer* pDrawer, const Packets& rPackets) {
        pDrawer->mMatPacket->mpDisplayListObj->callDL();

        J3DShapePacket* firstPacket = rPackets.shape(0);
        firstPacket->mpShape->loadPreDrawSetting();
        if (MR::getJ3DShapePacketUserData(firstPacket) != nullptr) {
            MR::getJ3DShapePacketUserData(firstPacket)->callDL();
        }

        const u32 cullPicture = J3DPortPicture();  // (GXV_PLANT_DRAWCULL: see drawPort)
        const s32 num = rPackets.num();
        for (s32 idx = 0; idx < num; idx++) {
            J3DShapePacket* packet = rPackets.shape(idx);

            if (rPackets.hasLight(idx)) {
                rPackets.loadLight(idx);
            }

            if (!rPackets.hidden(idx) && !rPackets.culled(idx, cullPicture) && !rPackets.shapeOut(idx)) {
                PortCensusModelBegin(packet);
                PortHrwTagScope hrwTag(packet);
                if (packet->mpDisplayListObj != nullptr) {
                    packet->mpDisplayListObj->callDL();
                }
                rPackets.draw(idx);
                PortCensusModelEnd();
            }
        }
    }

    template < class Packets >
    void portDrawPort(const DrawBufferShapeDrawer* pDrawer, const Packets& rPackets);
}  // namespace
#endif

#ifdef TARGET_PC
// ============================================================== (replay25) GXV_REPLAY25: a shape's draw replayed as the bytes it wrote
// What the game thread does for each packet of a drawer is: its differed list's call, then J3DShapePacket::drawFast = prepareDraw (the
// packet's, model's and matrix buffer's state into the draw path's J3DSys and into the SHARED J3DShape), J3DShape::drawFast (the
// vertex-format list when it is not the last one sent, the three vertex array bases, the two matrix array bases, the matrix index loads,
// the shape's retained list call). For a rigid, single-joint shape (J3DShapeMtx 'SMTX', no CPU skinning, no normal-bump, no LOD, no
// user list, no texture matrices) every byte of that is a function of a handful of words: the shape, the model, its vertex arrays, the
// two matrix arrays of the view pass (the view pass swaps them: two values alternate; the matrices themselves are NOT in the stream, the
// front end reads them through the array bases, which is why a MOVING rigid actor repeats its bytes as well as a still one), the
// shape's flags and whether the vertex-format list is due. The engine keeps those words and the bytes (<= 112) per packet, two variants
// (the two matrix halves); when the words are the same it copies the bytes into the stream and leaves out the rest.
// What a skipped drawFast would also have done is not in the stream: the statics J3DShape::sOldVcdVatCmd (set here, it is read by the
// next packet's test), __PortGXDrawSerial (added), the view pass's touch of the two arrays (made), and the J3D state the packet leaves in
// J3D_DSYS / the shape / J3DShapeMtx / J3DDifferedTexMtx (nothing before the end of the drawer reads it: the next packet's drawFast
// sets all of it, and a hidden / culled / light-loading packet between reads none of it). At the end of the drawer, when its last
// packet was replayed, that packet's drawFast is run once with its bytes thrown away (the stream pointer and the draw serial put back),
// so the state is the one the real draw leaves, whatever it is: no list of "what drawFast writes" to keep in step.
// Not replayed (the real draw runs, and says why in the census): a packet hidden or culled (as before: not drawn at all), a shape of
// another kind (multi-matrix / concat-view / skinned / bump / LOD / user list / texture matrices: counted "kind"), the GX library holding
// state to send (a light load just before: "dirty"), array shadows (GXV_ARRAYSKIP) or the envelope flag set, less than 640 bytes of
// room in the stream, a capture open, the draw pipe's frozen passes.
// GXV_REPLAY25=0 off (default); 1 on; 2 verify: the real draw always runs and its bytes (and the post state) are compared with the
// replay's; the "replay25:" census line says how many were compared and how many differed, per family (it must say 0).
namespace {
namespace replay25 {
enum { kKey = 15, kMaxSpan = 112, kRoom = 640, kFam = 64 };
enum Why { W_Struct, W_Skin, W_Dirty, W_Env, W_Arr, W_Room, W_Big, W_Post, W_Count };
struct Variant {
    uintptr_t key[kKey];
    u32 drawSerial;
    u16 bytes;
    u16 used;
    u8 data[kMaxSpan];
};
struct Slot {
    const J3DShapePacket* owner;
    const J3DShape* clsShape;
    u8 cls;  // 0 not looked at, 1 the shape qualifies, 2 it does not
    u8 next;
    u16 pad;
    Variant v[2];
};
struct Fam {
    const char* name;
    u32 cand, hit, rec, mism, post;
    u32 bytes;
    u32 why[W_Count];
    u32 key[kKey];
};
struct Ctx {
    Fam* fam;
    const J3DShapePacket* pending;
    int mode;
};
struct Totals {
    u32 pictures0, cand, hit, rec, mism, compared, mat, bytes, slotsMade, poolFull;
    u32 why[W_Count], key[kKey];
    u64 realNs, replayNs;
    u32 realN, replayN;
};
Fam sFam[kFam];
u32 sFamNum = 0;
Totals sT;
u32 sTick = 0, sSamp = 0;
std::chrono::steady_clock::time_point sT0;
bool sT0Set = false;
u32 sSlots = 0, sPoolUsed = 0, sPoolCap = 0;
u8* sPool = nullptr;
u32 sPoolLeft = 0;
u32 sLogged = 0;

inline int Mode() {
    return PORT_HOT_KNOB(int, [] {
        const char* e = getenv("GXV_REPLAY25");
        const int v = (e != nullptr && *e != '\0') ? atoi(e) : 0;
        return v >= 1 && v <= 2 ? v : 0;
    }());
}
inline u32 PoolKiB() {
    return PORT_HOT_KNOB(u32, [] {
        const char* e = getenv("GXV_REPLAY25_KB");
        const int v = (e != nullptr && *e != '\0') ? atoi(e) : 0;
        return static_cast< u32 >(v > 0 ? v : 768);
    }());
}
inline u64 NowNs() {
    return static_cast< u64 >(std::chrono::duration_cast< std::chrono::nanoseconds >(std::chrono::steady_clock::now().time_since_epoch()).count());
}
Fam* FamFor(const char* name) {
    for (u32 i = 0; i < sFamNum; ++i) {
        if (sFam[i].name == name) {
            return &sFam[i];
        }
    }
    if (sFamNum < kFam) {
        Fam* f = &sFam[sFamNum++];
        memset(f, 0, sizeof(*f));
        f->name = name;
        return f;
    }
    return &sFam[kFam - 1];  // (the last row takes the rest)
}
Slot* NewSlot() {
    const u32 need = (sizeof(Slot) + 7u) & ~7u;
    if (sPoolLeft < need) {
        if (sPoolUsed + 65536u > PoolKiB() * 1024u) {
            if (sT.poolFull == 0) {
                port_log_summary("replay25: the slot pool (GXV_REPLAY25_KB, %u KiB) is full: packets without a slot are drawn as before", PoolKiB());
            }
            ++sT.poolFull;
            return nullptr;
        }
        u8* p = static_cast< u8* >(malloc(65536));
        if (p == nullptr) {
            ++sT.poolFull;
            return nullptr;
        }
        sPool = p;
        sPoolLeft = 65536;
        sPoolUsed += 65536;
    }
    Slot* s = reinterpret_cast< Slot* >(sPool);
    sPool += need;
    sPoolLeft -= need;
    memset(s, 0, sizeof(*s));
    ++sSlots;
    ++sT.slotsMade;
    return s;
}
// The shape's own kind: what is fixed for the model data, looked at once per slot.
bool Qualifies(const J3DShape* shape) {
    if (shape->mHasNBT || shape->mHasPNMTXIdx || (shape->mFlags & (J3DShpFlag_EnableLod | J3DShpFlag_NoMtx)) != 0) {
        return false;
    }
    const u32 groups = shape->mMtxGroupNum;
    if (groups < 1 || groups > 4 || shape->mShapeMtx == nullptr || shape->mShapeDraw == nullptr) {
        return false;
    }
    for (u32 i = 0; i < groups; ++i) {
        if (shape->mShapeMtx[i] == nullptr || shape->mShapeDraw[i] == nullptr || shape->mShapeMtx[i]->getType() != 'SMTX') {
            return false;
        }
    }
    return true;
}
// The words the bytes depend on (see the head of this block). false: the packet cannot be keyed now.
inline bool BuildKey(const J3DShapePacket* sp, const J3DShape* shape, uintptr_t* k) {
    J3DModel* model = sp->mpModel;
    J3DMtxBuffer* mb = sp->mpMtxBuffer;
    if (model == nullptr || mb == nullptr) {
        return false;
    }
    const u32 view = *mb->getCurrentViewNoPtr();
    Mtx** dm = mb->getDrawMtxPtrPtr();
    Mtx33** nm = mb->getNrmMtxPtrPtr();
    if (dm == nullptr || nm == nullptr) {
        return false;
    }
    const J3DVertexBuffer* vb = model->getVertexBuffer();
    k[0] = reinterpret_cast< uintptr_t >(shape);
    k[1] = reinterpret_cast< uintptr_t >(model);
    k[2] = reinterpret_cast< uintptr_t >(mb);
    k[3] = model->mFlags & 0x1Cu;  // (CPU skinning, LOD)
    k[4] = shape->mFlags & ~0x1Fu;  // (the bits prepareDraw writes and the hide bit are not the shape's own)
    k[5] = reinterpret_cast< uintptr_t >(vb->mCurrentVtxPos);
    k[6] = reinterpret_cast< uintptr_t >(vb->mCurrentVtxNrm);
    k[7] = reinterpret_cast< uintptr_t >(vb->mCurrentVtxCol);
    k[8] = reinterpret_cast< uintptr_t >(dm[view]);
    k[9] = reinterpret_cast< uintptr_t >(nm[view]);
    k[10] = reinterpret_cast< uintptr_t >(mb->getScaleFlagArray());
    k[11] = reinterpret_cast< uintptr_t >(sp->getUserArea()) | reinterpret_cast< uintptr_t >(sp->mpTexMtxObj);
    k[12] = (J3DShape::sOldVcdVatCmd != shape->mVcdVatCmd ? 1u : 0u) | (view << 1);
    k[13] = reinterpret_cast< uintptr_t >(shape->mShapeDraw[0]->getDisplayList());
    k[14] = shape->mShapeDraw[0]->getDisplayListSize();
    return true;
}
inline bool Same(const uintptr_t* a, const uintptr_t* b) {
    uintptr_t d = 0;
    for (int i = 0; i < kKey; ++i) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}
inline void CountMiss(Ctx& c, const Variant& v, const uintptr_t* k) {
    for (int i = 0; i < kKey; ++i) {
        if (v.key[i] != k[i]) {
            ++c.fam->key[i];
            return;
        }
    }
}
inline bool PostOk(const J3DShape* shape) {
    return J3DShape::sOldVcdVatCmd == shape->mVcdVatCmd && !J3DShape::sEnvelopeFlag && __PortArrValid == 0 && port_gx_replay_clean != nullptr &&
           port_gx_replay_clean() != 0;
}
// The real draw, nothing else (and what a replay left to do first is not pending any more: the draw sets all of it).
inline void Real(Ctx& c, J3DShapePacket* sp) {
    c.pending = nullptr;
    sp->drawFast();
}
// The real draw with its bytes kept in `v` (key `k`, taken before it), when it is a record that can be replayed.
inline bool RealRecord(Ctx& c, J3DShapePacket* sp, const J3DShape* shape, Variant& v, const uintptr_t* k) {
    uint8_t* const p0 = __PortGXFifoPtr;
    const u32 ov0 = __PortGXFifoOverflowSerial;
    const u32 ds0 = __PortGXDrawSerial;
    Real(c, sp);
    uint8_t* const p1 = __PortGXFifoPtr;
    if (ov0 != __PortGXFifoOverflowSerial || p1 <= p0) {
        return false;
    }
    if (p1 - p0 > kMaxSpan) {
        ++c.fam->why[W_Big];
        return false;
    }
    if (!PostOk(shape)) {
        ++c.fam->why[W_Post];
        return false;
    }
    memcpy(v.data, p0, static_cast< size_t >(p1 - p0));
    memcpy(v.key, k, sizeof(v.key));
    v.bytes = static_cast< u16 >(p1 - p0);
    v.drawSerial = __PortGXDrawSerial - ds0;
    v.used = 1;
    return true;
}
// One packet's shape draw: replayed, or drawn (and recorded).
inline void Draw(Ctx& c, PacketInfo* info, J3DShapePacket* sp) {
    J3DShape* const shape = sp->mpShape;
    if (shape == nullptr || sp->checkFlag(J3DShpFlag_Hidden)) {
        sp->drawFast();  // (draws nothing, and leaves nothing: a replayed packet before it is still the last one drawn)
        return;
    }
    Fam* const f = c.fam;
    ++f->cand;
    Slot* slot = static_cast< Slot* >(info->mPortReplay);
    if (slot == nullptr) {
        slot = NewSlot();
        info->mPortReplay = slot;
        if (slot == nullptr) {
            Real(c, sp);
            return;
        }
    }
    if (slot->owner != sp) {
        slot->owner = sp;
        slot->cls = 0;
        slot->v[0].used = slot->v[1].used = 0;
    }
    if (slot->cls == 0 || slot->clsShape != shape) {
        slot->clsShape = shape;
        slot->cls = Qualifies(shape) ? 1 : 2;
        slot->v[0].used = slot->v[1].used = 0;
    }
    if (slot->cls != 1) {
        ++f->why[W_Struct];
        Real(c, sp);
        return;
    }
    uintptr_t k[kKey];
    if (!BuildKey(sp, shape, k)) {
        ++f->why[W_Struct];
        Real(c, sp);
        return;
    }
    if (k[3] != 0 || k[11] != 0) {
        ++f->why[k[3] != 0 ? W_Skin : W_Struct];
        Real(c, sp);
        return;
    }
    if (J3DShape::sEnvelopeFlag) {
        ++f->why[W_Env];
        Real(c, sp);
        return;
    }
    if (__PortArrValid != 0) {
        ++f->why[W_Arr];
        Real(c, sp);
        return;
    }
    if (port_gx_replay_clean == nullptr || port_gx_replay_clean() == 0) {
        ++f->why[W_Dirty];
        Real(c, sp);
        return;
    }
    Variant* hit = nullptr;
    for (int i = 0; i < 2; ++i) {
        if (slot->v[i].used != 0 && Same(slot->v[i].key, k)) {
            hit = &slot->v[i];
            slot->next = static_cast< u8 >(i ^ 1);
            break;
        }
    }
    const bool sample = ((++sSamp) & 15u) == 0 && ::port::gt71::StatClockOn();  // (gt71 CLOCK: replayNs / realNs only feed the census line)
    if (hit != nullptr && c.mode == 1) {
        if (static_cast< intptr_t >(reinterpret_cast< uintptr_t >(__PortGXFifoLimit) - reinterpret_cast< uintptr_t >(__PortGXFifoPtr)) <
            static_cast< intptr_t >(hit->bytes + kRoom)) {
            ++f->why[W_Room];
            Real(c, sp);
            return;
        }
        const u64 t0 = sample ? NowNs() : 0;
        memcpy(__PortGXFifoPtr, hit->data, hit->bytes);
        __PortGXFifoPtr += hit->bytes;
        __PortGXDrawSerial += hit->drawSerial;
        if (gPortParViewAsync) {
            port_par_view_touch_out(reinterpret_cast< const void* >(k[8]));
            port_par_view_touch_out(reinterpret_cast< const void* >(k[9]));
        }
        J3DShape::sOldVcdVatCmd = shape->mVcdVatCmd;
        c.pending = sp;
        ++f->hit;
        f->bytes += hit->bytes;
        if (sample) {
            sT.replayNs += NowNs() - t0;
            ++sT.replayN;
        }
        return;
    }
    if (hit != nullptr) {  // (verify: the real draw, compared with what the replay would have written)
        uint8_t* const p0 = __PortGXFifoPtr;
        const u32 ov0 = __PortGXFifoOverflowSerial;
        const u32 ds0 = __PortGXDrawSerial;
        Real(c, sp);
        uint8_t* const p1 = __PortGXFifoPtr;
        if (ov0 == __PortGXFifoOverflowSerial && p1 > p0) {
            ++sT.compared;
            const u32 n = static_cast< u32 >(p1 - p0);
            const bool ok = n == hit->bytes && memcmp(p0, hit->data, n) == 0 && __PortGXDrawSerial - ds0 == hit->drawSerial;
            const bool post = PostOk(shape);
            if (!ok || !post) {
                if (!ok) {
                    ++f->mism;
                } else {
                    ++f->post;
                }
                if (sLogged < 12) {
                    ++sLogged;
                    u32 at = 0;
                    while (at < n && at < hit->bytes && p0[at] == hit->data[at]) {
                        ++at;
                    }
                    port_log_summary("replay25: VERIFY MISMATCH family \"%s\": the real draw wrote %u bytes, the replay %u; first difference at byte %u; "
                             "draw serial %u against %u; post state %s",
                             f->name != nullptr ? f->name : "?", n, static_cast< unsigned >(hit->bytes), at,
                             static_cast< unsigned >(__PortGXDrawSerial - ds0), static_cast< unsigned >(hit->drawSerial), post ? "ok" : "WRONG");
                }
                if (n <= kMaxSpan && post) {  // (what the real draw wrote is the record from now on)
                    memcpy(hit->data, p0, n);
                    hit->bytes = static_cast< u16 >(n);
                    hit->drawSerial = __PortGXDrawSerial - ds0;
                }
            }
        }
        return;
    }
    // no record for these words: the real draw, recorded
    CountMiss(c, slot->v[slot->next], k);
    const u64 t0 = sample ? NowNs() : 0;
    if (RealRecord(c, sp, shape, slot->v[slot->next], k)) {
        slot->next ^= 1;
        ++f->rec;
    }
    if (sample) {
        sT.realNs += NowNs() - t0;
        ++sT.realN;
    }
}
// The end of a drawer: the J3D state the last replayed packet would have left, made by running its draw with the bytes thrown away.
inline void End(Ctx& c) {
    const J3DShapePacket* p = c.pending;
    if (p == nullptr) {
        return;
    }
    c.pending = nullptr;
    uint8_t* const save = __PortGXFifoPtr;
    const u32 ds = __PortGXDrawSerial;
    const u32 ov = __PortGXFifoOverflowSerial;
    const_cast< J3DShapePacket* >(p)->drawFast();
    if (ov != __PortGXFifoOverflowSerial) {
        port_log_summary("replay25: BUG: the state-only draw overflowed the stream");  // (kRoom keeps this from happening)
    }
    __PortGXFifoPtr = save;
    __PortGXDrawSerial = ds;
    ++sT.mat;
}
void Report() {
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration< double >(now - sT0).count();
    const u32 pics = J3DPortPicture() - sT.pictures0;
    sT0 = now;
    sT.pictures0 = J3DPortPicture();
    Totals t = sT;
    // (sum the families)
    t.cand = t.hit = t.rec = t.mism = 0;
    memset(t.why, 0, sizeof(t.why));
    memset(t.key, 0, sizeof(t.key));
    u32 post = 0;
    u64 bytes = 0;
    for (u32 i = 0; i < sFamNum; ++i) {
        const Fam& f = sFam[i];
        t.cand += f.cand;
        t.hit += f.hit;
        t.rec += f.rec;
        t.mism += f.mism;
        post += f.post;
        bytes += f.bytes;
        for (int w = 0; w < W_Count; ++w) {
            t.why[w] += f.why[w];
        }
        for (int w = 0; w < kKey; ++w) {
            t.key[w] += f.key[w];
        }
    }
    const double n = pics > 0 ? static_cast< double >(pics) : 1.0;
    const double realUs = sT.realN != 0 ? sT.realNs / 1000.0 / sT.realN : 0.0;
    const double repUs = sT.replayN != 0 ? sT.replayNs / 1000.0 / sT.replayN : 0.0;
    const double savedMs = realUs > repUs ? t.hit * (realUs - repUs) / 1000.0 / n : 0.0;
    static const char* const kWhy[W_Count] = {"kind", "skin/LOD", "dirty", "envelope", "arrays", "room", "big", "post"};
    static const char* const kKeyName[kKey] = {"shape", "model", "mtxbuf", "flags", "shapeflags", "vtxpos", "vtxnrm", "vtxcol", "drawmtx",
                                               "nrmmtx", "scaleflags", "user/texmtx", "vcd/view", "list", "listsize"};
    char why[200], key[300];
    int wl = 0, kl = 0;
    why[0] = key[0] = '\0';
    for (int w = 0; w < W_Count && wl < 170; ++w) {
        wl += snprintf(why + wl, sizeof(why) - wl, "%s%s %.1f", w ? ", " : "", kWhy[w], t.why[w] / n);
    }
    for (int w = 0; w < kKey && kl < 260; ++w) {
        if (t.key[w] != 0) {
            kl += snprintf(key + kl, sizeof(key) - kl, "%s%s %.1f", kl ? ", " : "", kKeyName[w], t.key[w] / n);
        }
    }
    port_log_summary("replay25: GXV_REPLAY25=%d, %.1f s, %u pictures; a picture: %.1f packets given to the shape draw (%.1f replayed = %.0f %%, %.1f "
             "recorded because no record had these words, rest drawn: %s), %.0f bytes replayed (%.0f B a replay); words that differed: %s; "
             "state-only draws at drawer ends %.1f; sampled cost: real draw %.2f us (%u samples), replay %.2f us (%u); estimated saved %.2f ms "
             "a picture; verify: %u compared, %u differed, %u post-state wrong; slots %u (%u KiB of %u KiB%s)",
             Mode(), secs, pics, t.cand / n, t.hit / n, t.cand != 0 ? 100.0 * t.hit / t.cand : 0.0, t.rec / n, why, bytes / n,
             t.hit != 0 ? static_cast< double >(bytes) / t.hit : 0.0, kl != 0 ? key : "none", sT.mat / n, realUs, sT.realN, repUs, sT.replayN,
             savedMs, sT.compared, t.mism, post, sSlots, sPoolUsed / 1024u, PoolKiB(), sT.poolFull != 0 ? ", FULL" : "");
    // families, the largest first
    u32 order[kFam];
    for (u32 i = 0; i < sFamNum; ++i) {
        order[i] = i;
    }
    for (u32 i = 1; i < sFamNum; ++i) {
        for (u32 j = i; j > 0 && sFam[order[j]].cand > sFam[order[j - 1]].cand; --j) {
            const u32 x = order[j];
            order[j] = order[j - 1];
            order[j - 1] = x;
        }
    }
    char fam[600];
    int fl = 0;
    fam[0] = '\0';
    for (u32 i = 0; i < sFamNum && i < 8 && fl < 520; ++i) {
        const Fam& f = sFam[order[i]];
        fl += snprintf(fam + fl, sizeof(fam) - fl, "%s\"%s\" %.1f packets, %.1f replayed, %u differed", i ? "; " : "", f.name != nullptr ? f.name : "?",
                       f.cand / n, f.hit / n, f.mism + f.post);
    }
    port_log_summary("replay25: families by packets a picture: %s", fam);
    for (u32 i = 0; i < sFamNum; ++i) {
        if (sFam[i].mism + sFam[i].post != 0) {
            port_log_summary("replay25: VERIFY family \"%s\": %u bytes differed, %u post states wrong", sFam[i].name != nullptr ? sFam[i].name : "?", sFam[i].mism,
                     sFam[i].post);
        }
    }
    // (the window's counters go to zero; the verify totals and the slots stay)
    for (u32 i = 0; i < sFamNum; ++i) {
        Fam& f = sFam[i];
        f.cand = f.hit = f.rec = 0;
        f.bytes = 0;
        memset(f.why, 0, sizeof(f.why));
        memset(f.key, 0, sizeof(f.key));
    }
    sT.mat = sT.compared = 0;
    sT.realNs = sT.replayNs = 0;
    sT.realN = sT.replayN = 0;
}
// A drawer's context, or null when the replay is off or cannot be used now.
inline bool Begin(Ctx& c, const char* family) {
    const int mode = Mode();
    if (mode == 0) {
        return false;
    }
    if (__PortGXReplayOk == nullptr || __PortGXReplayOk() == 0 || gPortJ3DSysDraw != &j3dSys || gPortPipeOverlap) {
        // (dead26) said once per reason, so a console run can tell "never engaged" from "engaged but silent": the first refusals of each kind
        static u32 sRefused[4];
        const u32 why = __PortGXReplayOk == nullptr ? 0u : __PortGXReplayOk() == 0 ? 1u : gPortJ3DSysDraw != &j3dSys ? 2u : 3u;
        if (++sRefused[why] == 1) {
            static const char* const kReason[4] = {"__PortGXReplayOk is not linked (weak, null)", "the FIFO is not writing into the front end's stream (or a capture / particle gathering is open)",
                                                   "the draw path's J3DSys is not j3dSys (GXV_DRAWPIPE)", "a late draw (GXV_PIPE25) is overlapping"};
            port_log_summary("replay25: GXV_REPLAY25=%d: a drawer was refused: %s (said once for this reason; the census line follows when a drawer is accepted)", mode,
                             kReason[why]);
        }
        return false;
    }
    c.mode = mode;
    c.pending = nullptr;
    c.fam = FamFor(family);
    if (!sT0Set) {
        sT0Set = true;
        sT0 = std::chrono::steady_clock::now();
        sT.pictures0 = J3DPortPicture();
        port_log_summary("replay25: GXV_REPLAY25=%d (%s), slot pool %u KiB", mode, mode == 2 ? "verify: the real draw runs and is compared" : "on",
                 PoolKiB());
    }
    return true;
}
inline void Tick() {
    if ((++sTick & 255u) == 0 && sT0Set && std::chrono::steady_clock::now() - sT0 >= std::chrono::seconds(5)) {
        Report();
    }
}
}  // namespace replay25
}  // namespace

// (replay25) the census line now (a scene's end, a host test's end); the window's counters are those since the last line
extern "C" void port_replay25_report(void) {
    if (replay25::sT0Set) {
        replay25::Report();
    }
}
// (replay25) a packet's record is dropped when the packet moves (DrawBufferShapeDrawer::add / remove / resetLightSort)
static inline void PortReplayDrop(PacketInfo* info) {
    if (info->mPortReplay != nullptr) {
        static_cast< replay25::Slot* >(info->mPortReplay)->owner = nullptr;
    }
}
#endif

void DrawBufferShapeDrawer::draw() const {
#ifdef TARGET_PC
    if (__builtin_expect(gPortHrwOnly != nullptr, 0)) {
        portHrwDrawOnly(gPortHrwOnly);  // (GXV_HRW, hrw28: the pair's partial draw: one model's packets)
        return;
    }
    if (J3DPortMatMergeOn() || J3DPortDiffSkipOn()) {
        drawPort();
        return;
    }
    portDrawPlain(this, PortLivePackets{this});
#else
    mMatPacket->mpDisplayListObj->callDL();

    J3DShapePacket* firstPacket = mPackets[0]->mShapePacket;
    firstPacket->mpShape->loadPreDrawSetting();
    if (MR::getJ3DShapePacketUserData(firstPacket) != nullptr) {
        MR::getJ3DShapePacketUserData(firstPacket)->callDL();
    }

    for (s32 idx = 0; idx < mNumPackets; idx++) {
        J3DShapePacket* packet = mPackets[idx]->mShapePacket;

        if (getLightLoader(idx) != nullptr) {
            getLightLoader(idx)->loadLight();
        }

        if ((packet->mpShape->mFlags & 1) == 0) {
            if (packet->mpDisplayListObj != nullptr) {
                packet->mpDisplayListObj->callDL();
            }
            packet->drawFast();
        }
    }
#endif
}

#ifdef TARGET_PC
// (GXV_DRAWPIPE) the drawer's packets as they were frozen at the hand-over: draw() over the frozen source.
void DrawBufferShapeDrawer::drawFrozen(const port::drawpipe::Frame& rFrame, const port::drawpipe::FrozenPacket* pPackets, s32 num, u32 serial) const {
    const PortFrozenPackets packets = {&rFrame, pPackets, num, serial};
    if (J3DPortMatMergeOn() || J3DPortDiffSkipOn()) {
        portDrawPort(this, packets);
        return;
    }
    portDrawPlain(this, packets);
}

// GXV_HRW (hrw28): the pair's partial draw of one unit's model. What portDrawPlain sends for a drawer, for the packets of `pOnly` alone: the material's list, the first shape's pre-draw
// setting and user list (drawer-wide, as the full draw sends them), then for each packet of the model its OWN light (not the previous packet's: a packet that is not the first of a run of
// equal lights draws with the run's, which is this actor's own by isSameLight; a packet with no light control would inherit a neighbour's and never gets here: PortHrwSet.cpp refuses such actors),
// its differed list and its shape. The plain path, never the merged / skipped / sorted / replayed ones: every list is sent in full, the record carries the whole state.
void DrawBufferShapeDrawer::portHrwDrawOnly(const J3DModel* pOnly) const {
    const PortLivePackets packets{this};
    const s32 num = packets.num();
    bool any = false;
    for (s32 i = 0; i < num; ++i) {
        if (packets.shape(i)->getModel() == pOnly) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    mMatPacket->mpDisplayListObj->callDL();
    J3DShapePacket* firstPacket = packets.shape(0);
    firstPacket->mpShape->loadPreDrawSetting();
    if (MR::getJ3DShapePacketUserData(firstPacket) != nullptr) {
        MR::getJ3DShapePacketUserData(firstPacket)->callDL();
    }
    const u32 cullPicture = J3DPortPicture();
    for (s32 idx = 0; idx < num; idx++) {
        J3DShapePacket* packet = packets.shape(idx);
        if (packet->getModel() != pOnly) {
            continue;
        }
        if (mPackets[idx]->mLightCtrl != nullptr) {
            mPackets[idx]->mLightCtrl->loadLight();
        }
        if (!packets.hidden(idx) && !packets.culled(idx, cullPicture)) {
            if (packet->mpDisplayListObj != nullptr) {
                packet->mpDisplayListObj->callDL();
            }
            packets.draw(idx);
        }
    }
}
#endif

#ifdef TARGET_PC
extern "C" void port_gx_call_prologue(void);  // RVL_SDK/gx/GXDisplayList.c
// ============================================================== (1.3 port) GXV_J3D_MATMERGE, GXV_J3D_DIFFSKIP
// What draw() sends, per drawer (one material of one model, for every actor of the model in the buffer):
//   the material's list; the first shape's vertex-format list (when it is not the last one sent) and matrix index
//   (J3DShape::loadPreDrawSetting); the first packet's user list (environment maps); then per packet: its light (the first
//   of a light group), its differed list (J3DShapePacket's: the instance's animated TEV colours, texture matrices, fog, ...)
//   and its shape (J3DShape::drawFast: array bases, matrix index, position / normal matrices, primitives).
// The front end's cost is mostly per list call. Two cuts, both leaving the front end's registers as draw() leaves them at every
// draw and at the end (see J3DPortDL.hpp for what a "pure" list is, and for the classes of what lists write):
//   GXV_J3D_MATMERGE=1 (default): when the first packet has a differed list, no light and no user list, the material's list and
//     that differed list are one call of a list holding both, one after the other (callMergedList). The differed list then comes
//     before loadPreDrawSetting's writes (CP registers, XF 0x1008 / 0x1018 / 0x1019) instead of after them: it must write none of
//     them, and nothing between reads a register (no draw). The merged list is in the drawer's heap (guest memory, as the front
//     end's list cache needs), made when the two lists' bytes change, and called only from the picture after it was made, when
//     its bytes are still those of the two lists (compared on every call). It is never written in a picture that follows one that
//     called it: the front end may still be reading the picture before (the guarantee J3D's own double buffers rely on).
//     Lists that keep changing (animated materials) are left alone for 300 pictures after three changes in a row.
//   GXV_J3D_DIFFSKIP=1 (default): a differed list with the bytes of the last one sent (instances of a model in the same state) is not
//     sent when between them there were only shapes and no light: a pure list that writes no CP register, no XF 0x1008 / 0x1018 /
//     0x1019 and no position or normal matrix (what a shape writes) is then writing every register with the value it holds. A light,
//     or a shape that may also load texture matrices (a differed texture matrix, user data, a normal matrix in texture memory), ends
//     the run. A hidden shape sends nothing and does not. The call's own work (the GX library's deferred state) is still done there.
//   With GXV_J3D_MATMERGE likewise, at the place of the first packet's differed list.
// Either switch at 0: the same calls as before for that part; both at 0: draw() exactly as before. Counters: the "j3d lists:" line.
void DrawBufferShapeDrawer::drawPort() const {
    portDrawPort(this, PortLivePackets{this});
}

namespace {
// GXV_STARBATCH=2 (starbatch26): what each pass wrote for each packet: the hash of the stream bytes its shape draw wrote, their number, the hash of its
// differed list; state 0 not drawn, 1 recorded, 2 not comparable (the stream changed segment).
struct SbSig {
    u32 span[J3DPortSort_MaxPackets];
    u32 len[J3DPortSort_MaxPackets];
    u32 list[J3DPortSort_MaxPackets];
    u8 state[J3DPortSort_MaxPackets];
};
inline u64 sbNowNs() {
    return static_cast< u64 >(std::chrono::duration_cast< std::chrono::nanoseconds >(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Compares the two passes of a verified drawer, packet by packet. The first packet that drew in the drawer's own order is left out of the shape
// comparison: it may carry the vertex-format list the later ones do not (the planned pass comes after it).
void portSortCompare(const char* family, const SbSig& a, const SbSig& b, s32 n) {
    J3DPortSortStats& ss = gJ3DPortSortStats;
    bool skippedFirst = false;
    for (s32 i = 0; i < n; ++i) {
        if (a.state[i] == 0 && b.state[i] == 0) {
            continue;
        }
        if (a.state[i] == 2 || b.state[i] == 2) {
            ++ss.verifyUncomparable;
            continue;
        }
        if (a.state[i] != b.state[i]) {
            ++ss.verifyBad;
            ++ss.verifyInstBad;
            if (ss.verifyLogged < 8) {
                ++ss.verifyLogged;
                port_log_summary("starbatch: VERIFY MISMATCH family \"%s\": packet %d was drawn in one pass and not the other", family != nullptr ? family : "?", static_cast< int >(i));
            }
            continue;
        }
        ++ss.verifyInst;
        const bool listBad = a.list[i] != b.list[i];
        bool shapeBad = false;
        if (!skippedFirst) {
            skippedFirst = true;  // (this one carries the vertex-format list in the first pass)
        } else {
            shapeBad = a.span[i] != b.span[i] || a.len[i] != b.len[i];
        }
        if (listBad || shapeBad) {
            ++ss.verifyInstBad;
            if (ss.verifyLogged < 8) {
                ++ss.verifyLogged;
                port_log_summary("starbatch: VERIFY MISMATCH family \"%s\": packet %d: %s differs (own order %u bytes %08x, planned order %u bytes %08x)", family != nullptr ? family : "?",
                         static_cast< int >(i), listBad ? "its differed list" : "the shape draw's stream", a.len[i], a.span[i], b.len[i], b.span[i]);
            }
        }
    }
}
// GXV_STARBATCH (starbatch25; the argument is in JSystem/J3DGraphBase/J3DPortSort.hpp). The order a drawer sends its packets in: inside each
// stretch of packets between two light loads, the packets with the same differed list bytes are made adjacent (one run a colour: the
// differed list is sent once, the front end's state stays, the back end joins the draws), the first packet of the stretch stays first and
// the last of it last. Writes sortOrder[0..n) and returns true when a stretch was reordered (GXV_STARBATCH=1); GXV_STARBATCH=2 plans, checks
// and counts the same but leaves the order as it is (returns false). Only for an opaque depth-writing material list; a stretch whose packets
// are not all rigid, one-shape, without differed texture matrix or user area, or whose differed lists are not of one skeleton, is left alone.
template < class Packets >
bool portSortOrder(const DrawBufferShapeDrawer* pDrawer, const Packets& rPackets, u32 cullPicture, u16* sortOrder) {
    J3DPortSortStats& ss = gJ3DPortSortStats;
    const s32 n = rPackets.num();
    ++ss.drawers;
    J3DDisplayListObj* matDL = pDrawer->mMatPacket->mpDisplayListObj;
    if (matDL == nullptr || matDL->getDisplayList(0) == nullptr || !j3dportsort::MatListOpaqueDepth(matDL->getDisplayList(0), matDL->getDisplayListSize())) {
        ++ss.why[J3DPortSortWhy_MatList];
        ss.whyPackets[J3DPortSortWhy_MatList] += static_cast< u32 >(n);
        return false;
    }
    for (s32 i = 0; i < n; ++i) sortOrder[i] = static_cast< u16 >(i);
    bool any = false;
    for (s32 s = 0; s < n;) {
        s32 e = s + 1;
        while (e < n && !rPackets.hasLight(e)) ++e;
        // the active packets of the stretch [s, e)
        s32 act[J3DPortSort_MaxPackets];
        j3dportsort::In in[J3DPortSort_MaxPackets];
        u32 m = 0;
        int why = -1;
        const J3DShape* shape0 = nullptr;
        for (s32 i = s; i < e; ++i) {
            if (rPackets.hidden(i) || rPackets.culled(i, cullPicture)) continue;
            J3DShapePacket* pk = rPackets.shape(i);
            const J3DShape* sh = pk->mpShape;
            if (sh == nullptr || (shape0 != nullptr && sh != shape0) || sh->mMtxGroupNum != 1 || sh->mHasPNMTXIdx) {
                why = J3DPortSortWhy_Shape;
            } else if (pk->getTexMtxObj() != nullptr || sh->getTexMtxLoadType() == 0x2000) {
                why = J3DPortSortWhy_TexMtx;
            } else if (pk->getUserArea() != nullptr) {
                why = J3DPortSortWhy_User;
            }
            shape0 = sh;
            J3DDisplayListObj* dl = pk->mpDisplayListObj;
            in[m].dl = dl != nullptr ? dl->getDisplayList(0) : nullptr;
            in[m].size = dl != nullptr ? dl->getDisplayListSize() : 0;
            act[m++] = i;
        }
        ++ss.stretches;
        ss.packets += m;
        u16 perm[J3DPortSort_MaxPackets];
        j3dportsort::Info info = {};
        int r = why >= 0 ? why : j3dportsort::Plan(in, m, m > 0 && act[0] == s, perm, &info, 10, J3DPortSortMin());
        if (r == J3DPortSortWhy_Ok) {
            ++ss.sorted;
            ss.sortedPackets += m;
            ss.runsBefore += info.runsBefore;
            ss.runsAfter += info.runsAfter;
            ss.recBefore += info.recBefore;
            ss.recAfter += info.recAfter;
            if (!j3dportsort::CheckPlan(in, m, act[0] == s, perm)) {
                ++ss.verifyBad;
            } else {  // (mode 1 sends it; mode 2 sends the own order, then this one, and compares)
                for (u32 k = 0; k < m; ++k) sortOrder[act[k]] = static_cast< u16 >(act[perm[k]]);
                any = true;
            }
        } else if (r == J3DPortSortWhy_Same) {
            ++ss.same;
        } else {
            ++ss.why[r];
            ss.whyPackets[r] += m;
        }
        s = e;
    }
    return any;
}


template < class Packets >
void portDrawPort(const DrawBufferShapeDrawer* pDrawer, const Packets& rPackets) {
    J3DMatPacket* const mMatPacket = pDrawer->mMatPacket;
    const s32 mNumPackets = rPackets.num();
    J3DPortListStats& st = gJ3DPortListStats;
    ++st.drawers;
    J3DShapePacket* firstPacket = rPackets.shape(0);
    ShapePacketUserData* firstUser = MR::getJ3DShapePacketUserData(firstPacket);
    J3DDisplayListObj* mergedDiff = nullptr;
    // (GXV_J3DNATIVE) the drawer's membership version: its serial and which packets are visible (show / hide, a culled model)
    const bool nativeOn = J3DPortNativeMode() != 0;
    u32 vB = rPackets.serial();
    if (nativeOn) {
        for (s32 i = 0; i < mNumPackets; ++i) {
            if (rPackets.hidden(i)) vB = vB * 31u + static_cast< u32 >(i) + 1u;
        }
    }
    if (J3DPortMatMergeOn() && mNumPackets > 0 && firstUser == nullptr && !rPackets.hasLight(0) && !rPackets.hidden(0) &&
        firstPacket->mpDisplayListObj != nullptr && pDrawer->callMergedList(mMatPacket->mpDisplayListObj, firstPacket->mpDisplayListObj, rPackets.ext())) {
        mergedDiff = firstPacket->mpDisplayListObj;
        ++st.matMerged;
    } else if (nativeOn) {
        J3DDisplayListObj* ml = mMatPacket->mpDisplayListObj;
        port_gx_call_bracketed(ml->getDisplayList(0), ml->getDisplayListSize(), static_cast< u32 >(reinterpret_cast< uintptr_t >(ml->getDisplayList(0))),
                               ml->mPortVer, vB, rPackets.ext(), ml->mPortKinds << 8);
    } else {
        mMatPacket->mpDisplayListObj->callDL();
    }

    firstPacket->mpShape->loadPreDrawSetting();
    if (firstUser != nullptr) {
        firstUser->callDL();
    }

    // (the last differed list sent and its flags, J3DPortScanDL, worked out when another list matches its bytes)
    const bool skipOn = J3DPortDiffSkipOn();
    const u8* last = nullptr;
    u32 lastSize = 0;
    u32 lastFlags = 0;
    if (mergedDiff != nullptr) {
        last = mergedDiff->getDisplayList(0);
        lastSize = mergedDiff->getDisplayListSize();
    }

    // GXV_PLANT_DRAWCULL (gamepar, 8 Nov; LiveActor.cpp port_anim_flush_stale): a model whose whole bounding sphere is outside this
    // picture's view pyramid (stamped with this picture's number by the flush; only the flower groups' plants) is left out like a hidden
    // shape: its light loader still runs (the light group's state is the next packets'), its differed list and its shape are not sent.
    // What a hidden shape does in the original (the same branch), so the state the packets after it see is the one they see after a
    // hidden one; and a model outside the view pyramid puts no pixel in the picture.
    const u32 cullPicture = J3DPortPicture();
    // (GXV_REPLAY25, above: the live drawers' shape draws, replayed from the bytes the last picture wrote when the words they depend on repeat)
    replay25::Ctx replayCtx;
    bool replayOn = false;
    if constexpr (std::is_same< Packets, PortLivePackets >::value) {
        replayOn = replay25::Begin(replayCtx, pDrawer->mPortFamily);
    }
    // GXV_STARBATCH (starbatch25, starbatch26): the order the packets are sent in (portSortOrder: packets with the same differed list adjacent,
    // inside the stretch between two light loads; the identity when the drawer is not planned). Mode 1 sends the planned order. Mode 2 sends the
    // drawer's own order as always (the recording is the original's), then the planned order once more into the same recording (the picture is the
    // same: opaque, depth-written draws of the same pieces), and compares, per packet, the stream bytes the shape draw wrote and the differed list
    // in force when it was written, between the two passes (a mismatch is logged and counted: "starbatch: VERIFY").
    u16 sortOrder[J3DPortSort_MaxPackets];
    bool sorted = false, verifyPlan = false;
    const int sbMode = J3DPortSortMode();
    if (__builtin_expect(sbMode != 0, 0) && mNumPackets >= static_cast< s32 >(J3DPortSortMin()) && mNumPackets <= J3DPortSort_MaxPackets &&
        firstUser == nullptr) {
        const u64 planT0 = port_turn_prof_on() != 0 ? sbNowNs() : 0;
        const bool planned = portSortOrder(pDrawer, rPackets, cullPicture, sortOrder);
        if (planT0 != 0) {
            gJ3DPortSortStats.planNs += sbNowNs() - planT0;
        }
        sorted = planned && sbMode == 1;
        verifyPlan = planned && sbMode == 2;
    }
    SbSig sigA, sigB;  // (mode 2 only: written by the passes below)
    const bool sbTimed = (sorted || verifyPlan) && port_turn_prof_on() != 0;
    bool firstPass = true;
    auto sendPass = [&](bool useOrder, SbSig* sig) {
        if (sig != nullptr) {
            memset(sig->state, 0, static_cast< size_t >(mNumPackets));
            memset(sig->list, 0, sizeof(u32) * static_cast< size_t >(mNumPackets));
            memset(sig->span, 0, sizeof(u32) * static_cast< size_t >(mNumPackets));
            memset(sig->len, 0, sizeof(u32) * static_cast< size_t >(mNumPackets));
        }
        for (s32 k = 0; k < mNumPackets; k++) {
            const s32 idx = useOrder ? static_cast< s32 >(sortOrder[k]) : k;
            J3DShapePacket* packet = rPackets.shape(idx);

            if (rPackets.hasLight(idx)) {
                rPackets.loadLight(idx);
                last = nullptr;
            }

            if (!rPackets.hidden(idx) && !rPackets.culled(idx, cullPicture) && !rPackets.shapeOut(idx)) {
                PortCensusModelBegin(packet);
                PortHrwTagScope hrwTag(packet);  // (GXV_HRW, hrw28)
                J3DDisplayListObj* dl = packet->mpDisplayListObj;
                if (dl != nullptr) {
                    const u8* bytes = dl->getDisplayList(0);
                    const u32 size = dl->getDisplayListSize();
                    if (firstPass && idx == 0 && mergedDiff != nullptr) {
                        port_gx_call_prologue();  // (sent in the merged list; the call's own work, as it would have been done here)
                        ++st.diffMerged;
                    } else if (skipOn && last != nullptr && size == lastSize && size != 0 && (bytes == last || memcmp(bytes, last, size) == 0) &&
                               J3DPortDiffSkippable(lastFlags != 0 ? lastFlags : (lastFlags = J3DPortScanDL(last, lastSize)))) {
                        port_gx_call_prologue();  // (the call's own work: state set since is sent here, as the call would have sent it)
                        ++st.diffSkipped;
                    } else {
                        if (nativeOn && size != 0) {
                            // (GXV_J3DNATIVE) a bracket: the differed list's version and the kinds of the animation that made it
                            port_gx_call_bracketed(bytes, size, static_cast< u32 >(reinterpret_cast< uintptr_t >(bytes)), dl->mPortVer, vB, rPackets.ext(),
                                                   1u | (dl->mPortKinds << 8));
                        } else {
                            dl->callDL();
                        }
                        ++st.diffSent;
                        if (bytes != last || size != lastSize) {
                            lastFlags = 0;
                        }
                        last = bytes;
                        lastSize = size;
                    }
                    if (sig != nullptr) {
                        // the list in force now must be this packet's own (that is what skipping a list says); its hash goes to the comparison
                        const u32 own = j3dportsort::HashBytes(bytes, size);
                        const u32 force = (last != nullptr) ? j3dportsort::HashBytes(last, lastSize) : own;
                        if (force != own) {
                            ++gJ3DPortSortStats.verifyListBad;
                        }
                        sig->list[idx] = own;
                    }
                }
                uint8_t* const sp0 = __PortGXFifoPtr;
                const u32 sov0 = __PortGXFifoOverflowSerial;
                if constexpr (std::is_same< Packets, PortLivePackets >::value) {
                    if (firstPass && replayOn) {
                        replay25::Draw(replayCtx, pDrawer->mPackets[idx], packet);
                    } else {
                        rPackets.draw(idx);
                    }
                } else {
                    rPackets.draw(idx);
                }
                if (sig != nullptr) {
                    uint8_t* const sp1 = __PortGXFifoPtr;
                    if (sov0 == __PortGXFifoOverflowSerial && sp1 >= sp0) {
                        sig->span[idx] = j3dportsort::HashBytes(sp0, static_cast< u32 >(sp1 - sp0));
                        sig->len[idx] = static_cast< u32 >(sp1 - sp0);
                        sig->state[idx] = 1;
                    } else {
                        sig->state[idx] = 2;  // (the stream moved to another segment: not comparable)
                    }
                }
                PortCensusModelEnd();
                if (last != nullptr && (packet->getUserArea() != nullptr || packet->getTexMtxObj() != nullptr || packet->mpShape->getTexMtxLoadType() == 0x2000)) {
                    last = nullptr;  // (the shape may have loaded texture matrices)
                }
            }
        }
    };
    const u64 sbT0 = sbTimed ? sbNowNs() : 0;
    sendPass(sorted, verifyPlan ? &sigA : nullptr);
    if (sbTimed) {
        (verifyPlan ? gJ3DPortSortStats.passOldNs : gJ3DPortSortStats.passNewNs) += sbNowNs() - sbT0;
        (verifyPlan ? gJ3DPortSortStats.passOldN : gJ3DPortSortStats.passNewN) += static_cast< u32 >(mNumPackets);
    }
    if constexpr (std::is_same< Packets, PortLivePackets >::value) {
        if (replayOn) {
            replay25::End(replayCtx);
            replay25::Tick();
        }
    }
    if (verifyPlan) {
        firstPass = false;
        last = nullptr;
        lastSize = 0;
        lastFlags = 0;
        const u64 t1 = sbTimed ? sbNowNs() : 0;
        sendPass(true, &sigB);
        if (sbTimed) {
            gJ3DPortSortStats.passNewNs += sbNowNs() - t1;
            gJ3DPortSortStats.passNewN += static_cast< u32 >(mNumPackets);
        }
        portSortCompare(pDrawer->mPortFamily, sigA, sigB, mNumPackets);
    }
}
}  // namespace

// The merged list's call (true), or nothing (false: the caller sends the material's list itself). See above.
// GXV_DRAWPIPE=4 (dpengine): a pass drawn on the draw thread beside the next update may not enter the guest OS, and the merged
// list's block comes from the drawer's guest heap; nor does it tell the front end's list cache of new bytes from a second thread.
// So while gPortPipeOverlap is set, a drawer whose merged list has to be made (again) is noted instead, and the game thread makes
// it after the join (portMergedFinish: the same bytes, the two lists do not change before the next flush; the drawer's counters
// are where the serial draw leaves them). The pass sends the two lists one by one, as the serial draw does in a picture that
// makes the merged list.
bool gPortPipeOverlap = false;
namespace {
    struct PortMergedLater {
        const DrawBufferShapeDrawer* mDrawer;
        J3DDisplayListObj* mMat;
        J3DDisplayListObj* mDiff;
    };
    enum { kPortMergedLaterMax = 64 };
    PortMergedLater sPortMergedLater[kPortMergedLaterMax];
    u32 sPortMergedLaterNum = 0;
}  // namespace

u32 DrawBufferShapeDrawer::portMergedFinish() {
    const u32 num = sPortMergedLaterNum;
    sPortMergedLaterNum = 0;
    for (u32 i = 0; i < num; ++i) {
        const PortMergedLater& later = sPortMergedLater[i];
        later.mDrawer->makeMergedList(later.mMat, later.mDiff, J3DPortPicture());
    }
    return num;
}

bool DrawBufferShapeDrawer::callMergedList(J3DDisplayListObj* pMatDL, J3DDisplayListObj* pDiffDL, u32 ext) const {
    const u32 picture = J3DPortPicture();
    if (picture < mMergedQuietUntil) {
        return false;
    }
    const u8* mat = pMatDL->getDisplayList(0);
    const u8* diff = pDiffDL->getDisplayList(0);
    const u32 matSize = pMatDL->getDisplayListSize();
    const u32 diffSize = pDiffDL->getDisplayListSize();
    if (mat == nullptr || diff == nullptr || matSize == 0 || diffSize == 0 || ((matSize | diffSize) & 0x1F) != 0) {
        return false;
    }
    // (the block: the merged list's room, matSize + diffSize + 0x80, then the copy of the two lists)
    const u32 room = matSize + diffSize + 0x80;
    if (mMergedMatSize == matSize && mMergedDiffSize == diffSize && memcmp(mMergedDL + room, mat, matSize) == 0 &&
        memcmp(mMergedDL + room + matSize, diff, diffSize) == 0) {
        // (made in an earlier picture from these bytes)
        mMergedChanges = 0;
        mMergedLastCall = picture;
        const int nat = J3DPortNativeMode();
        if (nat != 0) {
            // (GXV_J3DNATIVE) a bracket: the merged block's own version and the two lists' (the block's bytes are those of the two)
            const u32 vA = mMergedGen * 0x9E3779B1u + pMatDL->mPortVer * 65599u + pDiffDL->mPortVer;
            port_gx_call_bracketed(mMergedDL, mMergedSize, static_cast< u32 >(reinterpret_cast< uintptr_t >(mMergedDL)), vA, mPortSerial, ext,
                                   2u | (pDiffDL->mPortKinds << 8));
        } else {
            GXCallDisplayList(mMergedDL, mMergedSize);
        }
        return true;
    }
    // Changed (or new): made again, for the next picture, unless the front end may still read it.
    if (mMergedLastCall != 0 && mMergedLastCall + 1 >= picture) {
        return false;
    }
    if (++mMergedChanges >= 3) {
        mMergedChanges = 0;
        mMergedQuietUntil = picture + 300;  // (an animated material: its differed list changes on every picture)
        return false;
    }
    if (!J3DPortMergeable(J3DPortScanDL(mat, matSize), J3DPortScanDL(diff, diffSize))) {
        mMergedQuietUntil = picture + 600;
        return false;
    }
    if (gPortPipeOverlap) {
        // (the draw thread, beside the next update: made by the game thread after the join; one that does not fit in the note
        // is made by a later picture, as when the heap has no room)
        if (sPortMergedLaterNum < kPortMergedLaterMax) {
            sPortMergedLater[sPortMergedLaterNum].mDrawer = this;
            sPortMergedLater[sPortMergedLaterNum].mMat = pMatDL;
            sPortMergedLater[sPortMergedLaterNum].mDiff = pDiffDL;
            ++sPortMergedLaterNum;
        }
        return false;
    }
    makeMergedList(pMatDL, pDiffDL, picture);
    return false;
}

// The merged list of the two lists, made for the pictures after this one (callMergedList decided that it is to be made).
void DrawBufferShapeDrawer::makeMergedList(J3DDisplayListObj* pMatDL, J3DDisplayListObj* pDiffDL, u32 picture) const {
    const u8* mat = pMatDL->getDisplayList(0);
    const u8* diff = pDiffDL->getDisplayList(0);
    const u32 matSize = pMatDL->getDisplayListSize();
    const u32 diffSize = pDiffDL->getDisplayListSize();
    const u32 room = matSize + diffSize + 0x80;
    if (mMergedDL == nullptr || mMergedCap < room + matSize + diffSize) {
        // In the heap that holds the drawer (the scene's), and only with room to spare there; not freed (the old one may be read by
        // the front end still), the heap goes with the scene.
        static const u32 kRoomLeft = 0x40000;
        const u32 cap = (room + matSize + diffSize + 0x7F) & ~0x1Fu;
        JKRHeap* heap = JKRHeap::findFromRoot(const_cast< DrawBufferShapeDrawer* >(this));
        void* p = nullptr;
        if (heap != nullptr && heap->getHeapType() == 'EXPH' && heap->getFreeSize() >= static_cast< s32 >(cap + kRoomLeft)) {
            p = JKRHeap::alloc(cap, 0x20, heap);
        }
        if (p == nullptr) {
            mMergedQuietUntil = picture + 3600;
            return;
        }
        mMergedDL = static_cast< u8* >(p);
        mMergedCap = cap;
    }
    mMergedMatSize = 0;  // (until it is made)
    const u32 size = J3DPortBuildMerged(mat, matSize, diff, diffSize, mMergedDL, room);
    if (size == 0) {
        mMergedQuietUntil = picture + 600;  // (a register the two lists write with different masks: not merged)
        return;
    }
    memcpy(mMergedDL + room, mat, matSize);
    memcpy(mMergedDL + room + matSize, diff, diffSize);
    mMergedSize = size;
    mMergedMatSize = matSize;
    mMergedDiffSize = diffSize;
    DCStoreRange(mMergedDL, size);  // (the front end's list cache learns the new bytes)
    ++mMergedGen;
    ++gJ3DPortListStats.mergeWrites;
}
#endif

void DrawBufferShapeDrawer::add(const J3DShapePacket* pShapePacket, const ActorLightCtrl* pLightCtrl) {
    bool setLightLoader = false;
    s32 index = findLightSortIndex(pLightCtrl, &setLightLoader);

    PacketInfo* packet = mPackets[mNumPackets];
    packet->mShapePacket = const_cast< J3DShapePacket* >(pShapePacket);
    packet->mLightCtrl = const_cast< ActorLightCtrl* >(pLightCtrl);
#ifdef TARGET_PC
    PortReplayDrop(packet);  // (GXV_REPLAY25: the record belongs to the packet that was here)
#endif

    if (setLightLoader) {
        packet->mLightLoader = const_cast< ActorLightCtrl* >(pLightCtrl);
    } else {
        packet->mLightLoader = nullptr;
    }

    for (s32 idx = mNumPackets - 1; idx >= index; idx--) {
        mPackets[idx + 1] = mPackets[idx];
    }

    mPackets[index] = packet;
    mNumPackets++;
#ifdef TARGET_PC
    ++mPortSerial;  // (GXV_J3DNATIVE: the membership changed)
    ++gPortDrawPipeEpoch;  // (GXV_DRAWPIPE: a frame frozen before this no longer holds the lists)
#endif
}

void DrawBufferShapeDrawer::remove(const J3DShapePacket* pShapePacket) {
    PacketInfo* packet;
    for (s32 removeIdx = 0; removeIdx < mNumPackets; removeIdx++) {
        packet = mPackets[removeIdx];
        if (packet->mShapePacket == pShapePacket) {
            for (s32 idx = removeIdx; idx < mNumPackets - 1; idx++) {
                mPackets[idx] = mPackets[idx + 1];
            }
            if (packet->mLightLoader != nullptr && removeIdx < mNumPackets - 1) {
                mPackets[removeIdx]->mLightLoader = mPackets[removeIdx]->mLightCtrl;
            }
            mNumPackets--;
            mPackets[mNumPackets] = packet;
#ifdef TARGET_PC
            PortReplayDrop(packet);  // (GXV_REPLAY25)
            ++mPortSerial;  // (GXV_J3DNATIVE)
            ++gPortDrawPipeEpoch;  // (GXV_DRAWPIPE)
#endif
            return;
        }
    }
}

void DrawBufferShapeDrawer::resetLightSort(const ActorLightCtrl* pLightCtrl) {
    PacketInfo* packet;
    for (s32 idx = 0; idx < mNumPackets; idx++) {
        packet = mPackets[idx];
        if (packet->mLightCtrl == pLightCtrl) {
            J3DShapePacket* shapePacket = packet->mShapePacket;
            remove(shapePacket);
            add(shapePacket, pLightCtrl);
            return;
        }
    }
}

s32 DrawBufferShapeDrawer::findLightSortIndex(const ActorLightCtrl* pLightCtrl, bool* pSetLightLoader) const {
    *pSetLightLoader = false;

    if (pLightCtrl == nullptr) {
        return mNumPackets;
    }

    if (pLightCtrl->_1C != 0) {
        *pSetLightLoader = true;
        return mNumPackets;
    }

    bool foundLight = false;
    for (s32 idx = 0; idx < mNumPackets; idx++) {
        if (mPackets[idx]->mLightLoader != nullptr) {
            if (foundLight) {
                return idx;
            }

            if (pLightCtrl->isSameLight(mPackets[idx]->mLightLoader)) {
                foundLight = true;
            }
        }
    }

    if (!foundLight) {
        *pSetLightLoader = true;
    }
    return mNumPackets;
}

DrawBuffer::DrawBuffer(J3DModel* pModel)
    : mModelData(pModel->mModelData), mModel(pModel), _8(0), mNumActors(0), mNumMaterials(0), mMaterialNos(nullptr), mNumShapeDrawers(0),
      mNumOpaShapeDrawers(0), mShapeDrawers(nullptr) {
}

void DrawBuffer::init(int a1) {
    _8 = a1;
    initTable();
}

void DrawBuffer::add(const LiveActor* pActor) {
    J3DModel* model = MR::getJ3DModel(pActor);
    ActorLightCtrl* lightCtrl = MR::getLightCtrl(pActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->add(model->getShapePacket(mModelData->mMaterialTable.getMaterialNodePointer(idx)->getShape()->mIndex), lightCtrl);
    }
    mNumActors++;
}

void DrawBuffer::remove(const LiveActor* pActor) {
    J3DModel* model = MR::getJ3DModel(pActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->remove(model->getShapePacket(mModelData->mMaterialTable.getMaterialNodePointer(idx)->getShape()->mIndex));
    }
    mNumActors--;
}

void DrawBuffer::resetLightSort(const ActorLightCtrl* pLightCtrl) {
    J3DModel* model = MR::getJ3DModel(pLightCtrl->mActor);

    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        getShapeDrawerByMatNo(idx)->resetLightSort(pLightCtrl);
    }
}

void DrawBuffer::drawOpa() const {
    if (mNumActors > 0) {
        J3DShape::sOldVcdVatCmd = nullptr;
        for (s32 idx = 0; idx < mNumOpaShapeDrawers; idx++) {
            getShapeDrawerByIndex(idx)->draw();
        }
    }
}

void DrawBuffer::drawXlu() const {
    if (mNumActors > 0) {
        J3DShape::sOldVcdVatCmd = nullptr;
        for (s32 idx = mNumOpaShapeDrawers; idx < mNumShapeDrawers; idx++) {
            getShapeDrawerByIndex(idx)->draw();
        }
    }
}

namespace {
    s32 getSortedMaterialIndex(int materialIndex, J3DModel* pModel) {
        J3DMaterial* material;
        material = MR::getMaterial(pModel, materialIndex);
        for (s32 idx = 0; idx < materialIndex; idx++) {
            if (MR::getMaterial(pModel, idx)->getDiffFlag() == material->getDiffFlag()) {
                return idx;
            }
        }
        return -1;
    }
};  // namespace

void DrawBuffer::initTable() {
    mNumMaterials = MR::getMaterialNum(mModel);
    mNumShapeDrawers = mModelData->mMaterialTable.mUniqueMatNum;

    J3DModel* model = mModel;
    s32 idx, numOpaMaterials;
    numOpaMaterials = 0;
    for (idx = 0; idx < MR::getMaterialNum(model); idx++) {
        if (!MR::getMaterial(model, idx)->isDrawModeOpaTexEdge()) {
            if (::getSortedMaterialIndex(idx, model) < 0) {
                numOpaMaterials++;
            }
        }
    }
    mNumOpaShapeDrawers = numOpaMaterials;

    mMaterialNos = new s32[mNumMaterials];
    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        mMaterialNos[idx] = -1;
    }

    mShapeDrawers = new DrawBufferShapeDrawer*[mNumShapeDrawers];

    for (s32 idx = 0; idx < mNumShapeDrawers; idx++) {
        mShapeDrawers[idx] = nullptr;
    }

    s32 opaIndex = 0;
    s32 xluIndex = 0;
    for (s32 idx = 0; idx < mNumMaterials; idx++) {
        J3DMaterial* material = MR::getMaterial(mModel, idx);
        s32 index = ::getSortedMaterialIndex(idx, mModel);

        if (index >= 0) {
            s32 matNo = mMaterialNos[index];
            mMaterialNos[idx] = matNo;
            mShapeDrawers[matNo]->_8++;
            continue;
        }

        if (!material->isDrawModeOpaTexEdge()) {
            mMaterialNos[idx] = opaIndex;
            mShapeDrawers[opaIndex] = new DrawBufferShapeDrawer(material, mModel->getMatPacket(idx));
            opaIndex++;
        } else {
            mMaterialNos[idx] = xluIndex + mNumOpaShapeDrawers;
            mShapeDrawers[xluIndex + mNumOpaShapeDrawers] = new DrawBufferShapeDrawer(material, mModel->getMatPacket(idx));
            xluIndex++;
        }
    }

    // sort Opa
    for (s32 idx = 0; idx < mNumOpaShapeDrawers - 1; idx++) {
        for (s32 idx2 = idx + 1; idx2 < mNumOpaShapeDrawers; idx2++) {
            sortShapeDrawer(idx, idx2);
        }
    }

    // sort Xlu
    for (s32 idx = mNumOpaShapeDrawers; idx < mNumShapeDrawers - 1; idx++) {
        for (s32 idx2 = idx + 1; idx2 < mNumShapeDrawers; idx2++) {
            sortShapeDrawer(idx, idx2);
        }
    }

    for (s32 idx = 0; idx < mNumShapeDrawers; idx++) {
        mShapeDrawers[idx]->init(_8);
#ifdef TARGET_PC
        mShapeDrawers[idx]->mPortFamily = mPortName;  // (GXV_REPLAY25's census)
#endif
    }
}

void DrawBuffer::sortShapeDrawer(s32 matNo1, s32 matNo2) {
    DrawBufferShapeDrawer* drawerA = getShapeDrawerByIndex(matNo1);
    DrawBufferShapeDrawer* drawerB = getShapeDrawerByIndex(matNo2);
    const char* nameA = MR::getMaterialName(mModelData, drawerA->mMaterial->mIndex);
    const char* nameB = MR::getMaterialName(mModelData, drawerB->mMaterial->mIndex);

    if (strcasecmp(nameA, nameB) > 0) {
        drawerA->swap(drawerB);

        for (s32 idx = 0; idx < mNumMaterials; idx++) {
            if (matNo1 == mMaterialNos[idx]) {
                mMaterialNos[idx] = matNo2;
            } else if (matNo2 == mMaterialNos[idx]) {
                mMaterialNos[idx] = matNo1;
            }
        }
    }
}
