// How a frame's arenas (CmdBuf::vtx, idx, uni, draws, cmds) hold the C heap.
//
// Every frame buffer keeps the capacity its largest frame needed (a cleared vector keeps its memory), and a
// std::vector grows by doubling. A few frames of a stage need 4 to 6 MiB of vertices while play needs about 1, so
// each of the four frame buffers that circulate (the ring's three and the front end's own) doubled its vertex
// arena on its own, up to 9 or 12 MiB, at whatever moment it first carried such a frame. Each doubling asked the
// C heap (newlib, a fixed 96 MiB on the console) for one block twice the old one while the old one was still
// held, and left the old one as a hole for small blocks to fill. After a dozen stages the heap was in tens of
// thousands of pieces and one such request found no room (abort in BeginVertices). So:
//   - floors: when the renderer starts, while the heap is still in one piece, every frame buffer gets arenas big
//     enough for the biggest frames seen in play (6 MiB of vertices: the largest frame of a tour through eight
//     galaxies was 5.85 MiB, ordinary play 1 MiB or less). They are never given back, so they never move and leave
//     no holes;
//   - past its floor the vertex arena grows in steps of at most 2 MiB, not by doubling: the largest single request
//     is the frame's size plus at most 2 MiB.
// (Giving the excess back at quiet moments was tried in the host model, tests/vgx_arena.cpp: the freed blocks fill
// up with small ones and the next big frame needs fresh room; it ran out sooner than doubling did.)
// None of this changes what a frame holds (sizes, offsets, bytes): only how much room is behind it.
//   GXV_ARENA_KIB=<n>    the vertex arena's floor in KiB (default 6144); 0: the old behaviour (no floors, doubling)
//   GXV_ARENA_SOFT=1     (strk26, after the Strikers port's MSC_VGX_ARENA_SOFT; default 1: a crash guard) growth of the vertex arena past its floor fails soft: when the
//                        C heap and the blocks outside it have no room for the step (or for the bare size of the request), the draws that do not fit that frame are
//                        dropped (one "arena:" line in the basic log, a count in the "arena-soft:" census line) and the program goes on; the next frame tries again.
//                        0: the old way (operator new ends the program with its FATAL line, port_host_alloc_failed).
//   GXV_ARENA_SIM=<KiB>  (test only) a growth to a capacity of this many KiB or more is refused as if the heap had no room
#pragma once
#include "vgx_cmd.h"
#include <cstddef>
#include <cstdio>
#include <new>

namespace vgx {
namespace arena {

struct Floors {
    bool on = false;
    size_t vtxBytes = 0, idxBytes = 0, uniBytes = 0; // the arenas' floors
    size_t draws = 0, cmds = 0;                      // records
};

// (The other floors are fixed: the same tour's frames needed at most 570 KiB of indices, 180 KiB of uniforms and
// a few hundred draw records.)
inline Floors MakeFloors(long vtxKiB) {
    Floors f;
    if (vtxKiB <= 0) return f;
    f.on = true;
    f.vtxBytes = static_cast<size_t>(vtxKiB) << 10;
    f.idxBytes = 640u << 10;
    f.uniBytes = 256u << 10;
    f.draws = 1024;
    f.cmds = 2048;
    return f;
}
inline const Floors& Get() {
    static const Floors f = MakeFloors(EnvInt("GXV_ARENA_KIB", 6144)); // read once
    return f;
}

// The capacity to give the vertex arena when it must hold `need` bytes and has `cap`: at least the floor, and
// past it half again its size, but never more than 2 MiB at a time (rounded to 64 KiB).
inline size_t VtxGrowTo(const Floors& f, size_t cap, size_t need) {
    if (!f.on) return need; // (the vector's own doubling)
    size_t step = cap / 2;
    if (step < (256u << 10)) step = 256u << 10;
    if (step > (2u << 20)) step = 2u << 20;
    size_t to = cap + step;
    if (to < need) to = need;
    if (to < f.vtxBytes) to = f.vtxBytes;
    return (to + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu);
}
// Room for `need` bytes in a vertex arena that has less (called before the resize that needs it; without floors the
// resize doubles as before).
template <typename V> inline void VtxMakeRoom(V& vtx, size_t need, const Floors& f) {
    if (f.on && need > vtx.capacity()) vtx.reserve(VtxGrowTo(f, vtx.capacity(), need));
}

template <typename V> inline void ReserveBytes(V& v, size_t bytes) {
    const size_t n = bytes / sizeof(typename V::value_type);
    if (v.capacity() < n) v.reserve(n);
}
template <typename V> inline size_t CapBytes(const V& v) { return v.capacity() * sizeof(typename V::value_type); }

// ---- soft growth (GXV_ARENA_SOFT)
// The game's operator new (decomp/src/JSystem/JKernel/JKRHeap.cpp) never returns null: when port_host_alloc has no block, even outside the C heap (GXV_HEAP_BIG's
// last resort), port_host_alloc_failed ends the program. For the vertex arena, the one frame vector that reaches several MiB at once, a refusal is survivable:
// the draws of that frame that do not fit are not recorded (BeginVertices returns null, which every caller already treats as "draw nothing").
extern "C" void* port_host_alloc(size_t, int) __attribute__((weak));
extern "C" void port_host_free(void*) __attribute__((weak));
// (heap36) A game thread's `new` outside such a scope comes from the game's current heap (the scene's in play): the few places where a game thread grows a renderer
// list that outlives the scene (port_texwarm_cand's inbox, port_res_request's queue) hold one. Weak like the two above: a build without the game does nothing here.
extern "C" void port_host_alloc_scope_enter(void) __attribute__((weak));
extern "C" void port_host_alloc_scope_leave(void) __attribute__((weak));
struct HostAllocScope {
    HostAllocScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~HostAllocScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
    HostAllocScope(const HostAllocScope&) = delete;
    HostAllocScope& operator=(const HostAllocScope&) = delete;
};
extern "C" void port_log_summary(const char*, ...) __attribute__((weak, format(printf, 1, 2)));
struct SoftStats { unsigned long fails = 0, dropsThisFrame = 0, dropsTotal = 0, framesHit = 0, grows = 0; size_t lastNeed = 0, lastCap = 0; bool noted = false; };
inline SoftStats& Soft() { static SoftStats s; return s; }
inline bool SoftOn() {
    static const bool on = EnvInt("GXV_ARENA_SOFT", 1) != 0;
    return on;
}
inline size_t SimLimit() {
    static const size_t kib = static_cast<size_t>(EnvInt("GXV_ARENA_SIM", 0));
    return kib << 10;
}
// A block that operator delete frees correctly (the game's operator new on a host thread is port_host_alloc; its delete sends a pointer outside the
// game's heaps to port_host_free). Null when there is none.
inline void* TryAlloc(size_t bytes) {
    const size_t sim = SimLimit();
    if (sim && bytes >= sim) return nullptr;
    if (port_host_alloc) return port_host_alloc(bytes, 16);
    return ::operator new(bytes, std::nothrow);
}
inline void TryFree(void* p) {
    if (!p) return;
    if (port_host_free) port_host_free(p); else ::operator delete(p);
}
// Room for `need` bytes in a vertex arena without ending the program when the heap cannot give it: true when the vector can hold `need` now. On false the
// vector is untouched. The block is taken first and handed to the vector's own reserve() through the allocator (NoInitPendingBlock), so no other request can take
// its place in between. A growth step that does not fit is tried again at the bare size of the request before giving up.
template <typename V> inline bool VtxTryRoom(V& vtx, size_t need, const Floors& f) {
    if (need <= vtx.capacity()) return true;
    if (!f.on || !SoftOn()) { VtxMakeRoom(vtx, need, f); return true; }
    const size_t step = VtxGrowTo(f, vtx.capacity(), need);
    size_t to = step;
    void* blk = TryAlloc(to);
    if (!blk && step > need) { to = (need + 0xFFFFu) & ~static_cast<size_t>(0xFFFFu); blk = TryAlloc(to); }
    SoftStats& s = Soft();
    if (!blk) {
        ++s.fails;
        s.lastNeed = need;
        s.lastCap = vtx.capacity();
        return false;
    }
    ++s.grows;
    NoInitPending& p = NoInitPendingBlock();
    p.block = blk;
    p.bytes = to;
    vtx.reserve(to);
    if (p.block) { TryFree(p.block); p.block = nullptr; p.bytes = 0; } // (not taken: the vector found its capacity enough)
    return true;
}
// One line the first time a frame loses draws to it (the basic log), the rest counted; the census line is NoteFrame's.
inline void SoftNoteDrop(uint64_t frame) {
    SoftStats& s = Soft();
    if (s.dropsThisFrame == 0) ++s.framesHit;
    ++s.dropsThisFrame;
    ++s.dropsTotal;
    if (s.noted) return;
    s.noted = true;
    char line[220];
    std::snprintf(line, sizeof line, "arena: vertex arena could not grow (frame %u): %u KiB wanted over %u KiB held; the draws that do not fit are dropped until the heap has room (GXV_ARENA_SOFT=0: end the program)",
                  static_cast<unsigned>(frame), static_cast<unsigned>(s.lastNeed >> 10), static_cast<unsigned>(s.lastCap >> 10));
    if (port_log_summary) port_log_summary("%s", line);
    std::fprintf(stderr, "%s\n", line);
}
// At the end of a frame: the per-frame count restarts; every 1800 frames, and at once when something was lost, a census line (only when the switch has acted).
inline void SoftFrameEnd(uint64_t frame) {
    SoftStats& s = Soft();
    static uint64_t last = 0;
    const bool lost = s.dropsThisFrame != 0;
    s.dropsThisFrame = 0;
    if (s.fails == 0 && s.grows == 0) return;
    if (!(lost && frame - last >= 60) && frame - last < 1800) return;
    last = frame;
    if (port_log_summary)
        port_log_summary("arena-soft: GXV_ARENA_SOFT=%d: soft growths %lu, refused %lu, frames that lost draws %lu, draws dropped %lu",
                         SoftOn() ? 1 : 0, s.grows, s.fails, s.framesHit, s.dropsTotal);
}

// A frame buffer's arenas at their floors (at the renderer's start, before the heap is cut up).
inline void Reserve(CmdBuf& b, const Floors& f) {
    if (!f.on) return;
    ReserveBytes(b.vtx, f.vtxBytes);
    ReserveBytes(b.idx, f.idxBytes);
    ReserveBytes(b.uni, f.uniBytes);
    if (b.draws.capacity() < f.draws) b.draws.reserve(f.draws);
    if (b.cmds.capacity() < f.cmds) b.cmds.reserve(f.cmds);
}
// The same for the arenas a front-end split keeps of its own (they are swapped into the frame buffers every frame).
template <typename I, typename U> inline void ReserveSplit(std::vector<DrawRec>& draws, I& idx, U& uni, const Floors& f) {
    if (!f.on) return;
    ReserveBytes(idx, f.idxBytes);
    ReserveBytes(uni, f.uniBytes);
    if (draws.capacity() < f.draws) draws.reserve(f.draws);
}

} // namespace arena
} // namespace vgx
