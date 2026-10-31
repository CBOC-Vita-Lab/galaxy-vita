// The frame arenas' hold on the C heap (platform/vgx/src/vgx_arena.h), on the host.
//
// Four frame buffers circulate as on the console (the ring's three and the front end's own) through a scripted
// session like the console's tour of stages: each stage starts with a few frames of 3 to 5.85 MiB of vertices (and up
// to 570 KiB of indices), as the desktop tour measured, and goes on with ordinary frames of about 1 MiB; one stage
// has a frame of 7 MiB, past the floor. The vertex arena grows the way the front end grows it (VtxMakeRoom, then
// resize). Every allocation is seen by a replacement operator new and mirrored into a capped first-fit heap, in which
// the game's small blocks come and go between the stages (what cut the console's heap into pieces). Checked, with
// the floors on (the default) against the old behaviour (GXV_ARENA_KIB=0, doubling):
//   - once the floors are reserved, only the frame past the floor asks for a large block, within its size + 2 MiB;
//   - the buffers hold no more than the old way at the end;
//   - a capped heap the old way runs out of (82 MiB here: a doubling request found no room; ARENA_TIGHT_MIB=<n> to try
//     others: the old way ran out at 90 MiB and less, the floors at 78 and less) still meets every request, and so does
//     the console's 96 MiB.
// (The giving back of excess capacity at quiet moments was tried in this model too: it ran out at 84 MiB, sooner
// than doubling.)
#include "vgx_arena.h"
#include <cstdio>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete" // (the replacement operator new below is malloc's)
#endif
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <new>
#include <vector>

namespace {
// ---- every allocation of the test passes here
size_t g_largest = 0, g_bigRequests = 0, g_bigInPlay = 0;
bool g_inPlay = false, g_measure = false; // (only the frame buffers' requests are measured, not the stand-in for the game's)
struct ToyHeap; ToyHeap* g_toy = nullptr;
void ToyNote(void* p, size_t n, bool alloc);
}

void* operator new(size_t n) {
    if (g_measure) {
        if (n > g_largest) g_largest = n;
        if (n >= (1u << 20)) ++g_bigRequests;
        if (n >= (1u << 20) && g_inPlay) ++g_bigInPlay;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    ToyNote(p, n, true);
    return p;
}
void operator delete(void* p) noexcept { ToyNote(p, 0, false); std::free(p); }
void operator delete(void* p, size_t) noexcept { ToyNote(p, 0, false); std::free(p); }

namespace {
// ---- a first-fit heap of fixed size (address bookkeeping only), standing for newlib's 96 MiB arena on the console. The test's
// blocks of 4 KiB and more are mirrored into it; the game's long-lived small blocks are placed in it between the stages, so
// the free space is cut up as the console's was (thousands of pieces).
struct ToyHeap {
    size_t size;
    std::map<size_t, size_t> freeList; // start -> length
    std::map<void*, std::pair<size_t, size_t>> placed; // real pointer -> (start, length)
    bool failed = false;
    size_t failSize = 0;
    explicit ToyHeap(size_t s) : size(s) { freeList[0] = s; }
    bool Alloc(size_t n, size_t& at) {
        n = (n + 15) & ~size_t(15);
        for (auto it = freeList.begin(); it != freeList.end(); ++it) {
            if (it->second < n) continue;
            at = it->first;
            const size_t rest = it->second - n;
            freeList.erase(it);
            if (rest) freeList[at + n] = rest;
            return true;
        }
        return false;
    }
    void Free(size_t at, size_t n) {
        n = (n + 15) & ~size_t(15);
        auto next = freeList.lower_bound(at);
        if (next != freeList.end() && at + n == next->first) { n += next->second; next = freeList.erase(next); }
        if (next != freeList.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == at) { prev->second += n; return; }
        }
        freeList[at] = n;
    }
};
bool g_toyBusy = false;
void ToyNote(void* p, size_t n, bool alloc) {
    if (!g_toy || g_toyBusy) return;
    g_toyBusy = true; // (the map's own nodes come through operator new too)
    if (alloc) {
        if (n >= 4096) {
            size_t at = 0;
            if (g_toy->Alloc(n, at)) g_toy->placed[p] = {at, n};
            else if (!g_toy->failed) { g_toy->failed = true; g_toy->failSize = n; }
        }
    } else {
        auto it = g_toy->placed.find(p);
        if (it != g_toy->placed.end()) { g_toy->Free(it->second.first, it->second.second); g_toy->placed.erase(it); }
    }
    g_toyBusy = false;
}

using namespace vgx;

uint32_t g_rng = 12345;
uint32_t Rand() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

struct Result {
    size_t largest = 0, bigRequests = 0, bigInPlay = 0, held = 0, maxHeld = 0;
    bool toyFailed = false;
    size_t toyFailSize = 0, toyPieces = 0;
};

size_t Held(const std::vector<CmdBuf*>& bufs) {
    size_t n = 0;
    for (CmdBuf* b : bufs) n += arena::CapBytes(b->vtx) + arena::CapBytes(b->idx) + arena::CapBytes(b->uni);
    return n;
}

// One frame into `b`: vertices in pieces of a few KiB as BeginVertices asks for them, indices and uniforms alike.
void Fill(CmdBuf& b, size_t vtxBytes, size_t idxBytes, size_t uniBytes, const arena::Floors& f) {
    for (size_t at = 0; at < vtxBytes;) {
        const size_t piece = 1024 + Rand() % 8192;
        const size_t n = at + piece < vtxBytes ? at + piece : vtxBytes;
        arena::VtxMakeRoom(b.vtx, n, f);
        b.vtx.resize(n);
        b.vtx[n - 1] = 1;
        at = n;
    }
    for (size_t i = 0; i < idxBytes / 2; ++i) b.idx.push_back(static_cast<uint16_t>(i));
    for (size_t i = 0; i < uniBytes / 4; ++i) b.uni.push_back(1.0f);
}

Result Session(const arena::Floors& f, int stages, size_t heapBytes) {
    Result r;
    ToyHeap toy(heapBytes);
    g_toy = &toy;
    std::vector<void*> game; // the game's long-lived small blocks
    std::vector<CmdBuf*> bufs;
    // The game's own heap use at the start (in the toy heap: 16 MiB in one block), then the renderer's start.
    void* boot = ::operator new(16u << 20);
    for (int i = 0; i < 4; ++i) {
        bufs.push_back(new CmdBuf());
        arena::Reserve(*bufs.back(), f);
    }
    g_largest = g_bigRequests = g_bigInPlay = 0;
    unsigned frame = 0;
    const size_t burst[] = {3u << 20, 4515u << 10, 5990u << 10, 4u << 20, 5u << 20, 3500u << 10};
    for (int s = 0; s < stages; ++s) {
        // Between stages: the game frees half of what it had and takes 500 new blocks of 4-48 KiB, in between whatever is free
        // now (what cuts the heap up; about 26 MiB of them live at a time).
        for (size_t i = 0; i < game.size(); ++i)
            if (Rand() % 2 == 0) { ::operator delete(game[i]); game[i] = game.back(); game.pop_back(); --i; }
        for (int i = 0; i < 500; ++i) game.push_back(::operator new(4096 + (Rand() % 44) * 1024));
        g_inPlay = false;
        g_measure = true;
        for (int k = 0; k < 8; ++k, ++frame) { // the stage's first frames: the big ones
            CmdBuf& b = *bufs[frame % 4];
            size_t v = k < 6 ? burst[(k + s) % 6] : (300u << 10);
            if (s == stages - 3 && k == 2) v = 7u << 20; // (one frame past the floor)
            Fill(b, v, k < 6 ? (v / 11) : (30u << 10), 180u << 10, f);
            if (Held(bufs) > r.maxHeld) r.maxHeld = Held(bufs);
            b.clear();
        }
        g_inPlay = true;
        for (int k = 0; k < 600; ++k, ++frame) { // ordinary play: 0.6 to 1.25 MiB of vertices
            CmdBuf& b = *bufs[frame % 4];
            const size_t v = (600u << 10) + Rand() % (650u << 10);
            Fill(b, v, v / 10, 100u << 10, f);
            b.clear();
        }
        g_inPlay = false;
        g_measure = false;
    }
    r.held = Held(bufs);
    r.largest = g_largest;
    r.bigRequests = g_bigRequests;
    r.bigInPlay = g_bigInPlay;
    r.toyFailed = toy.failed;
    r.toyFailSize = toy.failSize;
    r.toyPieces = toy.freeList.size();
    for (void* p : game) ::operator delete(p);
    ::operator delete(boot);
    for (CmdBuf* b : bufs) delete b;
    g_toy = nullptr;
    return r;
}

int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail = 1;
}
} // namespace

int main() {
    const size_t MiB = 1u << 20;
    // The growth rule itself.
    {
        const arena::Floors f = arena::MakeFloors(6144);
        Check(arena::VtxGrowTo(f, 0, 100) == 6 * MiB, "an empty arena grows to its floor");
        Check(arena::VtxGrowTo(f, 2 * MiB, 2 * MiB + 1) == 6 * MiB, "an arena below its floor grows to it");
        Check(arena::VtxGrowTo(f, 6 * MiB, 6 * MiB + 1) == 8 * MiB, "past the floor: at most 2 MiB at a time");
        Check(arena::VtxGrowTo(arena::MakeFloors(512), 2 * MiB, 2 * MiB + 1) == 3 * MiB, "below 4 MiB: half again");
        Check(arena::VtxGrowTo(f, 6 * MiB, 11 * MiB + 5) == 11 * MiB + 0x10000, "a need beyond the step is met (rounded to 64 KiB)");
        const arena::Floors off = arena::MakeFloors(0);
        Check(!off.on && arena::VtxGrowTo(off, 2 * MiB, 3 * MiB) == 3 * MiB, "GXV_ARENA_KIB=0: off");
        CmdBuf b;
        arena::Reserve(b, f);
        Check(arena::CapBytes(b.vtx) == 6 * MiB && arena::CapBytes(b.idx) == 640u << 10 && arena::CapBytes(b.uni) == 256u << 10 &&
                  b.draws.capacity() == 1024 && b.cmds.capacity() == 2048,
              "floors reserved");
        const uint8_t* const base = b.vtx.data();
        arena::VtxMakeRoom(b.vtx, 5 * MiB, f);
        b.vtx.resize(5 * MiB);
        b.clear();
        Check(b.vtx.data() == base && arena::CapBytes(b.vtx) == 6 * MiB, "a frame within the floor does not move the arena, and clear() keeps it");
    }
    // The session, both ways, in the console's heap and in a tighter one.
    const arena::Floors on = arena::MakeFloors(6144), off = arena::MakeFloors(0);
    const int stages = 14;
    for (size_t heapMiB : {static_cast<size_t>(96), static_cast<size_t>(std::getenv("ARENA_TIGHT_MIB") ? std::atoi(std::getenv("ARENA_TIGHT_MIB")) : 82)}) {
        const Result a = Session(off, stages, heapMiB * MiB);
        const Result b = Session(on, stages, heapMiB * MiB);
        std::printf("%zu MiB heap, old (doubling): largest request %zu KiB, %zu requests of 1 MiB or more (%zu in ordinary play), held at the end %zu KiB; "
                    "capped heap: %s%zu KiB, %zu free pieces\n",
                    heapMiB, a.largest >> 10, a.bigRequests, a.bigInPlay, a.held >> 10, a.toyFailed ? "FAILED a request of " : "ok ", a.toyFailSize >> 10,
                    a.toyPieces);
        std::printf("%zu MiB heap, new (floors, steps): largest request %zu KiB, %zu requests of 1 MiB or more (%zu in ordinary play), held at the end %zu KiB; "
                    "capped heap: %s%zu KiB, %zu free pieces\n",
                    heapMiB, b.largest >> 10, b.bigRequests, b.bigInPlay, b.held >> 10, b.toyFailed ? "FAILED a request of " : "ok ", b.toyFailSize >> 10,
                    b.toyPieces);
        if (heapMiB == 96) {
            Check(b.bigRequests <= 2 && b.largest == 8 * MiB,
                  "after the floors only the frame past them asks for large blocks (its vertices: 7 MiB + at most 2 MiB; its indices)");
            Check(b.largest < a.largest, "smaller than doubling's largest");
            Check(b.bigInPlay == 0, "ordinary play asks for nothing of 1 MiB or more");
            Check(b.held <= a.held, "no more held at the end than the old way");
            // (the old way's outcome here depends on the library's growth rule: libc++ doubles the capacity and ran out, libstdc++
            // doubles the size and did not)
            Check(!b.toyFailed, "with the floors the console's 96 MiB meets every request");
        } else {
            Check(a.toyFailed, "the old way runs out of the tighter heap (the console's pattern)");
            Check(!b.toyFailed, "the floors do not");
        }
    }
    return g_fail;
}
