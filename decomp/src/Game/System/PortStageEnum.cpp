// precomp26 (GXV_STAGE_ENUM, read once by the renderer: vgx_draw.cpp StageEnumMode; 0 off, the default; 1 census; 2 on): the game's half of "enumerate the shader programs a stage can
// need from the materials of the models it has loaded, before they are first drawn". The renderer's half is vgx_draw.cpp (StageEnumDrawMarker) and vgx_backend_gl.cpp (EnumFrame,
// RunEnumWarm); the pure parts are platform/vgx/src/vgx_stage_enum.h.
//
// What a stage's shader lists hold is what automated tours drew in the stage's opening view; the places a player reaches (launch-star destinations, inner planets, later parts of
// a mission, enemies and effects that appear later) are in no list. What can be drawn is in the J3DModelData of every resource archive the stage has loaded.
//
// The walk. Every ResourceHolder the ResourceHolderManager holds, in the order they were added (the stationed archives, then the stage's: the start zone's objects first, as far as
// the load order says), every model of the holder, every material of the model that has a shape and a display list. For each such (material, shape) pair the draw the game would make
// is written without its geometry, inside the renderer's warm-up scope (the one GXV_PREWARM uses for effects):
//     PORT_GX_MARK_WARM_BEGIN
//       the material's display list (J3DDisplayListObj::callDL: for the .bdl models the list the file carries, the same bytes J3DMatPacket::draw calls),
//       the shape's vertex description list (J3DShape::loadPreDrawSetting: GXCallDisplayList(getVcdVatCmd())) and its current-matrix words (J3DCurrentMtx::load, made from the
//       material's texgens as J3DMaterial::calcCurrentMtx does),
//       PORT_GX_MARK_STAGE_ENUM_DRAW (once for each palette class the shape's matrix groups need: a shape that names at most 4 matrices has the small palette, one that names more
//       the short one)
//     PORT_GX_MARK_WARM_END
// and the renderer resolves the state as it resolves a draw (BuildFsKey, BuildVsKey, the render state, the folding of small draws), with a vertex layout planned from the shape's
// description, without uploading a texture and without recording anything; the variant goes to the back end's enumeration.
//
// What this does NOT see (counted in the census line where it can be): materials without a display list in the model data (the .bmd models' lists are made when a J3DModel is created:
// "no list" below), the differed lists (colour, texture number, texture matrix, fog values: they change values, not shader text), the material swaps and changeMaterial calls of
// the game (a model whose material is replaced at run time), the port's own passes (shadow, silhouette, indirect capture, the draw-buffer types' state overrides) which write their own
// state, and particle effects (GXV_PREWARM's job).
//
// When. Only in frames whose screen is closed (port_screen_quiet() == 2), once a frame (the first group the particle manager draws), at most GXV_STAGE_ENUM_PAIRS (24) pairs and
// GXV_STAGE_ENUM_FRAME_MS (4) ms of the game thread a frame. What the closed screen does not give is walked at the next closed screen of the same stage (the cursor stays); never in play.
// What is put back afterwards: the GX library's state (GXData, which a display-list call flushes) and the draw counter, as GXV_PREWARM's scopes do.
#ifdef TARGET_PC
#include "Game/System/ResourceHolder.hpp"
#include "Game/System/ResourceHolderManager.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include <JSystem/J3DGraphBase/J3DTevs.hpp>
#include <revolution.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" int port_stage_enum_mode(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
extern "C" uint64_t port_gx_recording_frame(void) __attribute__((weak));
extern "C" unsigned long long port_par_ptcl_now(void);
extern "C" void port_gx_marker(unsigned int kind);
extern "C" u32 __PortGXDrawSerial;

namespace {
enum { kWarmBegin = 14, kWarmEnd = 15, kEnumDraw = 33 };
constexpr u32 kSeenSize = 16384;  // slots of the duplicate table (a stage has a few thousand distinct pairs)

struct PortEnumWalk {
    int mode = -1;
    unsigned pairsPerFrame = 24;
    unsigned long long frameUs = 4000;
    uint32_t key = 0;             // the stage the cursor belongs to
    u32 holderCount = 0;          // size of the manager's array when the walk began (it grows while a stage loads)
    u32 hi = 0, mi = 0, ci = 0;   // holder, model, material cursor
    bool done = false, logged = false;
    uint64_t frame = ~0ull;
    // counters of this stage's walk
    u32 holders = 0, models = 0, materials = 0, noShape = 0, noList = 0, dup = 0, written = 0, markers = 0, frames = 0, smallShapes = 0, skinned = 0;
    unsigned long long spentUs = 0;
};
PortEnumWalk sEn;
uint64_t sSeen[kSeenSize];  // 0 = free; hashes of the pairs written (open addressing)

uint64_t Fnv(const void* p, size_t n, uint64_t h) {
    const u8* b = static_cast< const u8* >(p);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// true when the hash is new (and now kept)
bool SeenInsert(uint64_t h) {
    if (h == 0)
        h = 1;
    u32 i = static_cast< u32 >(h ^ (h >> 32)) & (kSeenSize - 1);
    for (u32 n = 0; n < kSeenSize; ++n, i = (i + 1) & (kSeenSize - 1)) {
        if (sSeen[i] == h)
            return false;
        if (sSeen[i] == 0) {
            sSeen[i] = h;
            return true;
        }
    }
    return true;  // (full: write the pair again rather than lose it)
}

bool Enabled() {
    if (sEn.mode < 0) {
        sEn.mode = port_stage_enum_mode ? port_stage_enum_mode() : 0;
        const char* p = getenv("GXV_STAGE_ENUM_PAIRS");
        const int np = p != NULL ? atoi(p) : 24;
        sEn.pairsPerFrame = static_cast< unsigned >(np < 1 ? 1 : np > 1000 ? 1000 : np);
        const char* f = getenv("GXV_STAGE_ENUM_FRAME_MS");
        const int ms = f != NULL ? atoi(f) : 4;
        sEn.frameUs = static_cast< unsigned long long >(ms < 1 ? 1 : ms > 200 ? 200 : ms) * 1000ull;
    }
    return sEn.mode > 0;
}

void Restart(uint32_t key) {
    PortEnumWalk keep = sEn;
    sEn = PortEnumWalk();
    sEn.mode = keep.mode;
    sEn.pairsPerFrame = keep.pairsPerFrame;
    sEn.frameUs = keep.frameUs;
    sEn.key = key;
    memset(sSeen, 0, sizeof sSeen);
}

// The first primitive's vertex count of each of the shape's display lists, the smallest of them (0: not known). A display list starts with the primitive command (0x80..0xB8), then
// the 16-bit vertex count, big-endian.
u32 SmallestPrimitive(J3DShape* shape) {
    u32 best = 0;
    for (u32 g = 0; g < shape->getMtxGroupNum() && g < 64; ++g) {
        J3DShapeDraw* d = shape->getShapeDraw(static_cast< u16 >(g));
        if (d == NULL || d->getDisplayList() == NULL || d->getDisplayListSize() < 3)
            continue;
        const u8* dl = d->getDisplayList();
        if (dl[0] < 0x80 || dl[0] > 0xB8)
            continue;
        const u32 n = (static_cast< u32 >(dl[1]) << 8) | dl[2];
        if (n != 0 && (best == 0 || n < best))
            best = n;
    }
    return best > 255 ? 255 : best;
}

// The position-matrix rows the shape's vertices can name: a shape with a per-vertex matrix index loads each group's matrices into rows 0.., three rows a matrix. Out: the highest row
// of the group with the fewest matrices and of the group with the most (equal when one class: the caller sends distinct palette classes only).
// Returns the number of distinct rows written (0: the shape has no per-vertex matrix: send 0xFF once).
u32 PaletteRows(J3DShape* shape, u32 rows[2]) {
    if (!shape->mHasPNMTXIdx)
        return 0;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (u32 g = 0; g < shape->getMtxGroupNum() && g < 64; ++g) {
        J3DShapeMtx* m = shape->getShapeMtx(static_cast< u16 >(g));
        const u32 n = m != NULL ? m->getUseMtxNum() : 1;
        if (n < lo)
            lo = n;
        if (n > hi)
            hi = n;
    }
    if (hi == 0)
        return 0;
    auto row = [](u32 n) { return n > 10 ? 29u : 3u * (n ? n : 1u) - 1u; };
    const u32 rLo = row(lo), rHi = row(hi);
    if ((rLo <= 11) == (rHi <= 11)) {  // (one palette class: the larger row stands for all groups)
        rows[0] = rHi;
        return 1;
    }
    rows[0] = rLo;
    rows[1] = rHi;
    return 2;
}

// One pair, written inside a scope. The GX library's state is put back after it, as GXV_PREWARM's scopes do (a display-list call flushes the library's dirty state into the stream,
// which the renderer's scope end then discards).
void WritePair(J3DModelData* md, J3DMaterial* mat, J3DShape* shape, const J3DCurrentMtx& cm, const u32* rows, u32 nrows, u32 smallCnt) {
    static GXData sGx;
    memcpy(&sGx, gx, sizeof sGx);
    const u32 serial0 = __PortGXDrawSerial;
    port_gx_marker(kWarmBegin);
    mat->getSharedDisplayListObj()->callDL();
    GXCallDisplayList(shape->getVcdVatCmd(), J3DShape::kVcdVatDLSize);
    cm.load();
    if (nrows == 0) {
        port_gx_marker(kEnumDraw | (0xFFu << 8) | (smallCnt << 16));
        ++sEn.markers;
    } else {
        for (u32 i = 0; i < nrows; ++i) {
            port_gx_marker(kEnumDraw | ((rows[i] & 0xFFu) << 8) | (smallCnt << 16));
            ++sEn.markers;
        }
    }
    port_gx_marker(kWarmEnd);
    memcpy(gx, &sGx, sizeof sGx);
    __PortGXDrawSerial = serial0;
    (void)md;
}

// The next pair, or false when the walk is over. Takes at most `budget` cursor steps (materials looked at) so that a stage of skipped materials cannot hold the frame.
bool Step(u32 steps, J3DModelData** outMd, J3DMaterial** outMat, J3DShape** outShape) {
    ResourceHolderManager* mgr = SingletonHolder< ResourceHolderManager >::get();
    if (mgr == NULL) {
        sEn.done = true;
        return false;
    }
    for (u32 s = 0; s < steps; ++s) {
        if (sEn.hi >= mgr->portHolderCount()) {
            sEn.done = true;
            return false;
        }
        ResourceHolder* rh = mgr->portHolderAt(sEn.hi);
        ResTable* t = rh != NULL ? rh->mModelResTable : NULL;
        if (t == NULL || sEn.mi >= t->mCount) {
            ++sEn.hi;
            sEn.mi = 0;
            sEn.ci = 0;
            if (rh != NULL && t != NULL && t->mCount != 0)
                ++sEn.holders;
            continue;
        }
        J3DModelData* md = static_cast< J3DModelData* >(t->getRes(sEn.mi));
        if (md == NULL || sEn.ci >= md->getMaterialNum()) {
            ++sEn.mi;
            sEn.ci = 0;
            if (md != NULL)
                ++sEn.models;
            continue;
        }
        J3DMaterial* mat = md->getMaterialNodePointer(static_cast< u16 >(sEn.ci++));
        ++sEn.materials;
        if (mat == NULL)
            continue;
        J3DShape* shape = mat->getShape();
        if (shape == NULL || shape->getVcdVatCmd() == NULL) {
            ++sEn.noShape;
            continue;
        }
        J3DDisplayListObj* dl = mat->getSharedDisplayListObj();
        if (dl == NULL || dl->getDisplayList(0) == NULL || dl->getDisplayListSize() == 0) {
            ++sEn.noList;
            continue;
        }
        *outMd = md;
        *outMat = mat;
        *outShape = shape;
        return true;
    }
    return false;  // (the step budget ran out; the cursor stays)
}

void CensusLine(const char* why) {
    if (sEn.logged || !port_log_summary)
        return;
    sEn.logged = true;
    port_log_summary("stageenum: game: stage %08x: walk %s: %u holders with models, %u models, %u materials; written as dry draws %u pairs (%u markers; %u skinned shapes, %u with a small "
                     "primitive), skipped: %u duplicates (same list, shape description, matrices), %u without a shape, %u without a display list in the model data; %.0f ms of game thread over "
                     "%u closed frames",
                     sEn.key, why, sEn.holders, sEn.models, sEn.materials, sEn.written, sEn.markers, sEn.skinned, sEn.smallShapes, sEn.dup, sEn.noShape, sEn.noList,
                     sEn.spentUs / 1000.0, sEn.frames);
}
}  // namespace

// (JPAEmitterManager::draw -> PortPrewarmGroup) a slice of the walk, in the 3D scene's draw, with the very GX state the particle groups start from.
void PortStageEnumSlice() {
    if (!Enabled() || !port_screen_quiet || port_screen_quiet() != 2)
        return;
    const uint32_t loading = port_scene_loading_key ? port_scene_loading_key() : 0;
    const uint32_t key = loading ? loading : (port_scene_key ? port_scene_key() : 0);
    if (key == 0)
        return;
    const uint64_t frame = port_gx_recording_frame ? port_gx_recording_frame() : 0;
    if (frame == sEn.frame)
        return;  // (once a frame)
    sEn.frame = frame;
    ResourceHolderManager* mgr = SingletonHolder< ResourceHolderManager >::get();
    if (mgr == NULL)
        return;
    if (key != sEn.key || (sEn.holderCount != 0 && mgr->portHolderCount() < sEn.holderCount))
        Restart(key);
    if (sEn.done) {
        // the manager grew since (another archive): walk again from the first holder; the duplicate table keeps what was written
        if (mgr->portHolderCount() <= sEn.holderCount)
            return;
        sEn.done = false;
        sEn.logged = false;
    }
    sEn.holderCount = mgr->portHolderCount();
    const unsigned long long t0 = port_par_ptcl_now();
    unsigned pairs = 0;
    bool any = false;
    while (pairs < sEn.pairsPerFrame && !sEn.done) {
        J3DModelData* md = NULL;
        J3DMaterial* mat = NULL;
        J3DShape* shape = NULL;
        if (!Step(256, &md, &mat, &shape)) {
            if (sEn.done)
                break;
            if (port_par_ptcl_now() - t0 >= sEn.frameUs)
                break;
            continue;
        }
        any = true;
        J3DCurrentMtx cm;
        {
            u8 idx[8];
            const u32 nTex = mat->getTexGenNum();
            const bool post = md->checkFlag(0x20);
            for (u32 i = 0; i < 8; ++i) {
                J3DTexCoord* tc = i < nTex ? mat->getTexCoord(i) : NULL;
                idx[i] = tc == NULL ? static_cast< u8 >(60) : static_cast< u8 >(post ? tc->getTexMtxReg() : tc->getTexGenMtx());
            }
            cm.setCurrentTexMtx(idx[0], idx[1], idx[2], idx[3], idx[4], idx[5], idx[6], idx[7]);
        }
        u32 rows[2] = {0, 0};
        const u32 nrows = PaletteRows(shape, rows);
        const u32 smallCnt = SmallestPrimitive(shape);
        J3DDisplayListObj* dl = mat->getSharedDisplayListObj();
        uint64_t h = Fnv(dl->getDisplayList(0), dl->getDisplayListSize(), 14695981039346656037ull);
        h = Fnv(shape->getVcdVatCmd(), J3DShape::kVcdVatDLSize, h);
        h = Fnv(&cm.mMtxIdxRegA, 8, h);
        h = Fnv(rows, sizeof rows, h) ^ (static_cast< uint64_t >(smallCnt) << 40) ^ nrows;
        if (!SeenInsert(h)) {
            ++sEn.dup;
        } else {
            WritePair(md, mat, shape, cm, rows, nrows, smallCnt);
            ++sEn.written;
            if (nrows)
                ++sEn.skinned;
            if (smallCnt && smallCnt <= 32)
                ++sEn.smallShapes;
            ++pairs;
        }
        if (port_par_ptcl_now() - t0 >= sEn.frameUs)
            break;
    }
    if (any || sEn.done) {
        sEn.spentUs += port_par_ptcl_now() - t0;
        ++sEn.frames;
    }
    if (sEn.done)
        CensusLine("finished");
}

// (the renderer's census line) [0] pairs written, [1] done (1/0), [2] materials looked at, [3] skipped without a list, [4] frames used
extern "C" void port_stage_enum_game_counters(uint32_t out[5]) {
    out[0] = sEn.written;
    out[1] = sEn.done ? 1u : 0u;
    out[2] = sEn.materials;
    out[3] = sEn.noList;
    out[4] = sEn.frames;
}
#endif
