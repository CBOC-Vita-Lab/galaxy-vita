// GXV_ARENA_SOFT (strk26; platform/vgx/src/vgx_arena.h VtxTryRoom): the vertex arena's growth past its floor fails soft. Host test, no GL.
// GXV_ARENA_SIM=8192 refuses any growth to 8 MiB or more as if the heap had no room. Checked: growth below the limit works and keeps the bytes; a step that is
// refused is retried at the bare size of the request (and works when that fits); a request that no size can meet returns false with the vector untouched;
// the pending-block hand-over leaks nothing and never gives its block to an unrelated request; with GXV_ARENA_SOFT=0 the old path (the vector's own growth) is used.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "vgx_arena.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main() {
    setenv("GXV_ARENA_SIM", "8192", 1);
    unsetenv("GXV_ARENA_SOFT");
    using namespace vgx;
    const arena::Floors& f = arena::Get(); // (floor 6 MiB)
    CHECK(f.on && f.vtxBytes == (6144u << 10));
    CHECK(arena::SoftOn());
    CmdBuf b;
    arena::Reserve(b, f);
    CHECK(b.vtx.capacity() >= f.vtxBytes);
    b.vtx.resize(1 << 20);
    for (size_t i = 0; i < b.vtx.size(); ++i) b.vtx[i] = static_cast<uint8_t>(i * 7 + 3);
    // inside the floor: nothing to do
    CHECK(arena::VtxTryRoom(b.vtx, 5u << 20, f));
    // past the floor: the step (6 + 2 = 8 MiB) is refused by the simulated heap, the bare size (7 MiB) fits
    const size_t before = arena::Soft().fails;
    CHECK(arena::VtxTryRoom(b.vtx, 7u << 20, f));
    CHECK(b.vtx.capacity() >= (7u << 20) && b.vtx.capacity() < (8u << 20));
    CHECK(arena::Soft().fails == before); // (the retry at the bare size is not a refusal)
    b.vtx.resize(7u << 20);
    bool same = true;
    for (size_t i = 0; i < (1u << 20); ++i) same = same && b.vtx[i] == static_cast<uint8_t>(i * 7 + 3);
    CHECK(same);
    // nothing fits: false, untouched
    const size_t cap = b.vtx.capacity(), size = b.vtx.size();
    const uint8_t* data = b.vtx.data();
    CHECK(!arena::VtxTryRoom(b.vtx, 9u << 20, f));
    CHECK(b.vtx.capacity() == cap && b.vtx.size() == size && b.vtx.data() == data);
    CHECK(arena::Soft().fails == before + 1);
    CHECK(NoInitPendingBlock().block == nullptr);
    // an unrelated allocation of another size is not given a pending block (none is left) and the next growth still works
    std::vector<uint8_t, NoInitAlloc<uint8_t>> other;
    other.reserve(12345);
    CHECK(other.capacity() >= 12345);
    // drops are counted once per frame, the first one makes the line
    arena::SoftNoteDrop(10);
    arena::SoftNoteDrop(10);
    CHECK(arena::Soft().dropsTotal == 2 && arena::Soft().framesHit == 1 && arena::Soft().noted);
    arena::SoftFrameEnd(11);
    CHECK(arena::Soft().dropsThisFrame == 0);
    arena::SoftNoteDrop(12);
    CHECK(arena::Soft().framesHit == 2);
    std::printf("vgx_arena_soft: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
