// GXV_FE_PARALLEL ("feparallel", 2.3): the front end's per-picture work split over two cores, stage 1: vertex decoding.
//
// The player (core 2) parses the GX stream, keeps the register file, resolves keys / shaders / textures and decodes the vertices of every
// streamed draw (top-level immediate draws, display lists the cache bypasses, particle lists) into the frame's arena. The finishing thread
// of the front-end split (vgx_fesplit.h; on the fourth core with GXV_CORE3_SPLIT=1, the PS Vita default) already builds the uniform
// blocks and the draw records from the operations the player sends, in the player's order. With this switch the player sends the decode
// of a draw's vertices to the finishing thread as one more operation, instead of doing it itself, wherever nothing on the player reads
// those vertices afterwards:
//   - DrawStreamPlan (vgx_state.cpp) reserves the vertices' room in the arena as before and, for a draw that may go (Want), leaves the
//     room empty and records the decode (Defer): the plan (copied: the player's plans change with the vertex format), the source bytes
//     and the vertex arrays bound now (copied).
//   - EmitDraw (vgx_draw.cpp) runs as before. Every place in it that reads or rewrites the draw's vertices on the player (shadow-fill
//     bounds, character bounds, lines and points widened, texture-matrix baking, the small-draw bounds, register folding) first makes them
//     exist (Place: the player decodes them itself, exactly as it did before). These are the same places GXV_SPLIT_COPY's vertex copies
//     are made at ("place()"), and the same rule: none of them runs after the job is sent.
//   - When the draw reaches its job with the vertices still not made, the decode goes out (Send) as an operation in front of the
//     draw's CPU transform and its job (the only operations that read them), and the finishing thread decodes into the arena.
//   - A draw dropped after the reservation (culled, a primitive with no index) gives the decode up (Cancel), as the arena gives the room up.
// The decode is the same function on the same inputs (DecodeVerticesOff: the player's own decode, with the arrays passed in and none of
// the player's globals written), into the same bytes of the arena: the vertices, the records, the indices and the uniforms the back end
// gets are those of the single-thread decode, bit for bit. A draw's highest position-matrix row (PNMTXIDX), which its own resolution needs
// at once, is read from the stream by the player (PosMtxMaxOf: one byte a vertex), the value the decode would have left.
// What the finishing thread reads: the copied plan and arrays, the source bytes and the vertex arrays (guest memory or the FIFO segment).
// Those must not change before it has decoded them:
//   - guest memory changes only after the "draw done" / token the game waits for: WriteBP's 0x45 / 0x47 / 0x48 settle every decode sent
//     before the hook is called (SettleSources); the frame's end (split::FinishFrame) waits for everything anyway;
//   - the FIFO segment is written again only after the game has moved to the other one and back: the player settles when it sees the
//     game move (vgx_galaxy.cpp PlayerMain).
//
//   GXV_FE_PARALLEL=0 (default): off, exactly as before.  =1: on (needs GXV_SPLIT=1, not 2).
//   =2 (verify): the player decodes every such draw itself, as with 0 (the picture is the single-thread one); the finishing thread decodes
//      it again off the player's thread, into a buffer of its own, from the copied plan and arrays, at the moment it would have with 1,
//      and compares with the bytes the player wrote; the player compares the highest matrix row it reads from the stream with its decode's.
//      "fepar: MISMATCH" lines and counts on the per-picture / summary line.
//   GXV_FE_PARALLEL_MIN=<n> (default 17): only draws of at least n vertices (a draw of 16 or fewer, soft-transformed and not depth-tested,
//      is read on the player for its bounds: it would only be decoded twice over).
//   GXV_FE_PARALLEL_PROF=1: per picture, one summary-level log line: draws and vertices decoded on the finishing thread, made on the
//      player after all, given up, the finishing thread's decode time, the player's waits at the settle points (and the verify counts).
//      Without it: one summary line every 600 pictures while the switch is on.
// The design of the full two-worker front end (state at split points, per-worker side tables) and why this is its first stage: REPORT.txt
// of agents/feparallel.
#pragma once
#include "vgx_state.h"

namespace vgx {

// ---- the decode as the finishing thread runs it (vgx_state.cpp)
// The plan's decode may run on another thread: the specialised decode (GXV_DECODE=1, GXV_POSDEFAULT not 2).
bool DecodeOffThreadOk(const Plan& p);
// The highest PNMTXIDX of `count` vertices of `src` (0 without the attribute): what DecodeVertices leaves in LastDecodedPosMtxMax().
uint32_t PosMtxMaxOf(const Plan& p, const uint8_t* src, uint32_t count);
// DecodeVertices without spans, reading `arrays` instead of g.arrays and writing none of the player's globals (only `out`); out-of-range
// indices are counted in *oob instead of logged. Requires DecodeOffThreadOk(p) and DecodeTablesReady() on the player before.
void DecodeVerticesOff(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, const ArrayRef* arrays, uint32_t* oob);
void DecodeTablesReady(); // (player) builds every table the decode fills on first use (the 8-bit component tables)

namespace fepar {
int Mode(); // GXV_FE_PARALLEL, read once
extern bool g_pending;     // (player) the draw being emitted has its decode waiting
inline bool Pending() { return __builtin_expect(g_pending, 0); }
// (player, DrawStreamPlan) whether this draw's decode may go to the finishing thread: the switch, the split on for this frame and not
// verifying, the plan, the size.
bool Want(const Plan& p, uint32_t count);
// (player) the vertices at `out` (in the frame's arena) are the decode of `count` vertices of `src` with `p` and the arrays bound now.
// With GXV_FE_PARALLEL=2 they are decoded here, as before.
void Defer(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out);
void Place();  // (player) the vertices are needed on the player now: decoded here
void Send();   // (player) the decode goes to the finishing thread (EmitDraw: in front of the transform and the job)
void Cancel(); // (player) the draw is dropped
// (player) every decode sent so far is done: before the game may change guest memory or the FIFO segment the sources lie in.
void SettleSources();
// (the finishing thread's side; fesplit) the picture is finished: counters, the log line
void FrameFinished(uint64_t frame);
// (tests) totals since the start: decodes sent, their vertices, made on the player after all, given up, checked (=2), mismatching (=2),
// settles that waited, out-of-range indices off the player's thread
void Counters(uint64_t out[8]);
// (endwait26) the player's settle waits so far, in ns (cumulative; any thread reads, the player writes)
uint64_t SettleNs();
} // namespace fepar
} // namespace vgx
