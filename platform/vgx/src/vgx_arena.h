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
#pragma once
#include "vgx_cmd.h"
#include <cstddef>

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
