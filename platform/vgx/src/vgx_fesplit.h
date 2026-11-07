// Front-end split ("item 2"): part of the front end's per-picture work runs on a second thread.
//
// The front end (the "player" thread) parses the GX stream, keeps the register state, resolves keys,
// shaders and textures, and decodes vertices into the frame's arena. What it then did for every draw
// with values it already had is moved to a finishing thread (by default on the back end's core, which
// has idle CPU in every scene measured):
//   - the uniform blocks: converting the registers into the float blocks (projection, matrices, light,
//     TEV) and deduplicating them in the uniform arena (BuildProj/BuildMtx/BuildLight/BuildTev,
//     PushBlock in vgx_draw.cpp);
//   - the draw records: merging a primitive into the previous record, making new records, building
//     the index arena (the tail of EmitDraw, EmitStaticDraw, EmitStaticRun);
//   - the CPU transform of small primitives' vertices (SoftTransform), done in place in the arena.
// The player sends these as an ordered stream of operations (with copies of the register values the
// blocks are built from); its command list holds a placeholder for every draw. At the end of the frame
// it waits for the finishing thread to catch up, replaces the placeholders by the records that thread
// made, takes its draws / indices / uniforms into the frame's CmdBuf and goes on as before (pass
// isolation, atlases, hand-over to the back end). The CmdBuf is the one the single-thread path makes.
//
//   GXV_SPLIT=0   everything on the player thread (the old path)
//   GXV_SPLIT=1   the split (the default on the PS Vita)
//   GXV_SPLIT=2   the split, and the player also runs the single-thread code for the moved work on shadow
//                 buffers and compares the results with the finishing thread's at every frame end
//                 (records, indices, uniforms, command order, transformed vertices); mismatches are logged
//                 ("fesplit: MISMATCH"), a summary every 600 frames. The split's output is what is drawn.
//   GXV_SPLIT_CORE=<n>  core of the finishing thread (Vita; default 1, the back end's); -1: not pinned
//   GXV_SPLIT_PRIO=<n>  its priority (Vita; default one step above the back end's: see vgx_galaxy.cpp, SplitThreadInit)
//   GXV_SPLIT_COPY=1   (from Producdevity's fork; default 0) the display-list cache's vertex copies into the frame's arena are
//                      made by the finishing thread (the player reserves the room and sends OP_COPY); the player still copies
//                      them itself where it reads them (edge/character bounds, shadow-fill bounds, lines, texture baking,
//                      register folding, small-draw bounds). A cache entry is not rebuilt, promoted or freed before its last
//                      copy is done (Settle). Off with GXV_SPLIT=2.
//   GXV_SPLIT_POLL_US=<n>  (from Producdevity's fork; default 0 = off) while the player builds a frame, the finishing thread
//                      looks for work every n microseconds (0..2000) instead of being woken by the player every 16 jobs:
//                      a wake-up is a kernel call on the player. Same operations, same order.
// The split is off while a diagnostic that inspects the moved work per draw is on (GXV_STATS detail,
// GXV_TRACE, GXV_TRACE_DRAWS, GXV_MERGEFAST=2, GXV_SOFTNEON=2).
#pragma once
#include "vgx_state.h"

namespace vgx {
namespace split {

// ---- the code both paths run (vgx_draw.cpp)
// Where a uniform block goes: the arena, the offsets / lengths of the blocks current draws refer to,
// and whether the arena counts as empty for deduplication (the first resolve of a frame).
struct BlockCtx {
    UniArena* uni;
    uint32_t* blockOff;
    uint32_t* blockLen;
    bool stale;
    bool census;    // GXV_LIGHTNORM=2 counting (only one thread may do it)
    uint64_t frame; // (census messages)
};
void BuildProjC(BlockCtx& c, const State& s);
uint64_t BuildMtxC(BlockCtx& c, const State& s, const VsKey& vs, bool soft); // returns the matrix rows used (as BuildMtx)
void BuildLightC(BlockCtx& c, const State& s, const VsKey& vs);
void BuildTevC(BlockCtx& c, const State& s, const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold);
// The rows of matrix memory BuildMtxC reads for this key (position: 64 bits, normal: 32, post-transform: 64),
// and the value it returns. Must follow BuildMtxC exactly (GXV_SPLIT=2 compares the returned value).
void MtxReads(const State& s, const VsKey& vs, bool soft, uint64_t& pos, uint32_t& nrm, uint64_t& post, uint64_t& used);
void SoftTransformWith(uint8_t* v, uint32_t count, const VtxLayout& L, const float* m /* 12 */, const float* n /* 9 */);

// What a draw record is made from besides the geometry.
struct DrawStateRef {
    uint16_t vsId, useFs;
    uint8_t texMask;
    uint32_t serial; // Resolved::stateSerial
    const RenderState* rs;
    const TexBind* tex; // [8]
};
enum : uint8_t { JOB_DYNAMIC = 0, JOB_STATIC = 1 };
struct DrawJob {
    uint8_t kind = JOB_DYNAMIC;
    uint8_t gxPrim = 0, prim = 0, cover = 0;
    uint8_t prevIsDraw = 0; // (split) the command before this draw's placeholder is a draw's
    uint8_t copyable = 0;   // static, GXV_RIGIDMERGE_DLLEARN: the list's vertices are also kept on the CPU (a copy in the arena could join a palette run)
    uint8_t ab = 0;         // GXV_SHADOWFILL_BOUNDS: abX..abH are the draw's pixels (DrawRec::ab)
    uint8_t pad[1] = {};
    uint32_t layoutMask = 0, count = 0, idxCount = 0;
    // dynamic: where the vertices are (absolute indices: chunk and first vertex; else the byte offset) and their stride
    uint32_t chunk = 0, first = 0, vtxStart = 0, stride = 0;
    int16_t exX = 0, exY = 0, exW = 0, exH = 0;
    int16_t abX = 0, abY = 0, abW = 0, abH = 0; // (see ab)
    // static: the record's geometry
    uint32_t vbuf = 0, vtxOffset = 0, idxOffset = 0, vtxFirst = 0;
    // dynamic, GXV_RIGIDMERGE: the vertex shader of the draw's key with the matrix palette (0xFFFF: the draw may not join a palette run), and
    // where its vertices are in memory (the arena at the moment: the finishing thread writes the palette slot into their w)
    uint16_t vsTwin = 0xFFFF, padTwin = 0;
    uint32_t vtxPtrLo = 0, vtxPtrHi = 0;
};
// The palette run the last record may be (GXV_RIGIDMERGE; see FinishDraw): rec is the record's index (-1: none), slots the matrices in its
// palette block (at uni offset pal).
struct RigidRun {
    int rec = -1;
    uint32_t slots = 0, pal = 0;
    bool prevOk = false; // (GXV_RIGIDMERGE_DLLEARN) the last job was a rigid draw, or a static one whose list can be copied into the arena
};
// (GXV_SPLIT=2) a palette slot written into the w of `count` vertices at arena offset `off`: recorded instead of written.
struct VtxWrite {
    size_t off;
    uint32_t count, stride;
    float w;
};
// Where draw records go. cmds != null: the single-thread path (a new record's CMD_DRAW is pushed, and whether the
// previous command is a draw is read from cmds); cmds == null: the caller places the records (job.prevIsDraw is used).
struct FinishCtx {
    std::vector<DrawRec>* draws;
    std::vector<uint16_t, NoInitAlloc<uint16_t>>* idx;
    std::vector<Cmd>* cmds;
    const uint32_t* blockOff;
    int* lastDraw;
    uint32_t* lastRecSerial;
    uint32_t* records;   // incremented per new record
    bool mainThread;     // the player's own path: statistics detail, shader warm-up list
    UniArena* uni = nullptr;            // (GXV_RIGIDMERGE) the uniform arena the palette blocks are made in
    RigidRun* rig = nullptr;
    std::vector<VtxWrite>* vtxLog = nullptr; // non-null: the palette indices are logged, not written (the shadow of GXV_SPLIT=2)
    // non-null (GXV_RIGIDMERGE_DLLEARN): one byte per job, in the order of the jobs: 1 = the draw joined the previous record (a static draw: it
    // would have, had its list been copied into the arena), else 0. FinishDraw / FinishStatic append it.
    std::vector<uint8_t>* joinLog = nullptr;
};
// Merges the primitive into the previous record or makes a new one. Returns the new record's index, or ~0u (merged).
uint32_t FinishDraw(FinishCtx& c, const DrawJob& j, const DrawStateRef& st);
uint32_t FinishStatic(FinishCtx& c, const DrawJob& j, const DrawStateRef& st);
// The shader warm-up list's program / variant bookkeeping of a new record (NewDrawRec), for the split path,
// where the player does it for every draw (a merged draw has its record's variant: no difference).
void NoteVariant(const DrawStateRef& st, uint32_t layoutMask);

// ---- the split (vgx_fesplit.cpp), called on the player thread
extern bool g_on; // this frame runs split (decided at the frame's start)
void FrameBegin();                       // after a frame was handed over (and before the first)
// Uniform blocks: instead of building them, send what they are built from. Returns the matrix rows used.
void Proj(bool stale);
uint64_t Mtx(uint16_t vsId, const VsKey& vs, bool soft, bool stale);
void Light(uint16_t vsId, const VsKey& vs, bool stale);
void Tev(uint16_t fsId, const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold, bool stale);
// GXV_SPLIT_COPY: whether the display-list cache's vertices are copied into the arena by the finishing thread this frame (Copy).
bool CopiesVertices();
// The pending vertices at byte `offset` are a copy of `bytes` bytes at `src`, which must stay as they are until Settle(Issued())
// (called after the draw was sent) returns.
void Copy(size_t offset, const uint8_t* src, uint32_t bytes);
uint32_t Issued();
void Settle(uint32_t ticket);
// The pending vertices at byte `offset` (count, layout) are to be transformed with the current position matrix.
void Xform(size_t offset, uint32_t count, uint32_t layoutMask);
// A draw: returns the index its placeholder (Cmd{CMD_DRAW, index}) carries.
uint32_t Job(const DrawJob& j, const DrawStateRef& st);
// The vertex arena is about to grow past its capacity (its memory moves): the finishing thread must be idle.
void BeforeArenaGrow(const std::vector<uint8_t, NoInitAlloc<uint8_t>>& vtx, size_t newSize);
// End of the frame's drawing: waits for the finishing thread, puts its records, indices and uniforms into `buf`
// and replaces the placeholders. Adds the records to the frame's statistics.
void FinishFrame(CmdBuf& buf);
// (host) Called on the finishing thread when it starts: pinning, priority.
void SetThreadInit(void (*fn)(int core));
// (host) Called on the finishing thread about every 500 ms while it is idle (it holds no reader then): the host's check of where the
// thread runs (vgx_galaxy.cpp: the fourth core's guard). Not called while the thread has work.
void SetThreadTick(void (*fn)());
// (any thread, for the perf line) The finishing thread's CPU time (sampled when it goes idle), the player's time spent
// waiting for it (at frame ends and arena growth), and the frames finished split; all since the start.
void Counters(int64_t& finisherCpuUs, int64_t& playerWaitNs, uint32_t& frames);
// (player) GXV_FXJOIN: jobs that went out in runs of two or more (OP_JOBS), and those runs; since the start.
void FxJoinCounters(uint64_t out[2]);
// (GXV_FECUT_KEYID) shader keys handed to the finishing thread: fragment keys sent whole, by id; vertex keys sent whole, by id
void KeyIdCounters(uint64_t out[4]);
// (any thread) Operations the player ran itself at frame ends because the finishing thread was behind (since the start).
uint32_t StolenOps();
// (player, GXV_RIGIDMERGE_DLLEARN) the join log of the frame FinishFrame finished (see FinishCtx::joinLog).
const std::vector<uint8_t>& FinishedJoinLog();

} // namespace split
} // namespace vgx
