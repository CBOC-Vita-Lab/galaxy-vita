// heap23b (9 Nov 2026): GXV_SL_SCENEDROP, the state-list store at a scene's destruction (vgx_state.cpp StateListsSceneGone), on the
// host with the real front end (tools/test_vgx_sl_scenedrop.sh). A session of 7 "galaxies" visited LOOPS times in order. Each galaxy has
// its own material lists (J3D-like: TEV / blend / z / indirect BP writes, CP matrix indices, XF colour / texgen registers and texture
// matrix rows; 32-byte padded), laid out in a "scene heap" region of guest RAM at the start of every visit. The layout of a galaxy is the
// same on every visit except that a share of its lists lands elsewhere (the console's scene heaps are filled by asynchronous loads, so
// the same galaxy's lists are at partly different addresses each time). A persistent region holds lists every scene calls (Mario's,
// the HUD's). Each visit draws FRAMES pictures: every list of the galaxy and of the persistent region is called (CallList, the path the
// front end takes for a material list), some are written again with the same bytes, some with a register changed; then the scene is
// destroyed: StateListsSceneGone(scene heap range, GXV_SL_SCENEDROP) as vgx_galaxy.cpp's CopyDropService calls it, and the region
// is overwritten (the next scene's heap).
// Printed per visit: a digest of the register state after every call (vgx::g, the whole State), the store's entries / bytes after the
// destroy, and the host bytes held through `new` (this file counts every operator new / delete). The script runs modes 0, 1 and 2:
//   - the digests must be the same in all three (a freed entry is parsed again: the same register state);
//   - mode 0 keeps the entries of every layout seen (the growth of the c24 soak); modes 1 and 2 hold only the persistent lists' entries
//     after a destroy, and the bytes held through `new` at the destroy do not grow from loop to loop.
#include "vgx_hle.h"
#include "vgx_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <vector>

// ---- every allocation through `new`, counted (the front end's own stores are what the soak's "game's `new`" is made of)
namespace {
size_t g_newLive = 0, g_newBlocks = 0;
void* CountedAlloc(size_t n, size_t align) {
    void* p = nullptr;
    if (align <= alignof(std::max_align_t)) p = std::malloc(n ? n : 1);
    else if (posix_memalign(&p, align, n ? n : 1) != 0) p = nullptr;
    if (!p) throw std::bad_alloc();
    g_newLive += malloc_usable_size(p);
    ++g_newBlocks;
    return p;
}
void CountedFree(void* p) {
    if (!p) return;
    g_newLive -= malloc_usable_size(p);
    --g_newBlocks;
    std::free(p);
}
} // namespace
void* operator new(size_t n) { return CountedAlloc(n, 0); }
void* operator new[](size_t n) { return CountedAlloc(n, 0); }
void* operator new(size_t n, std::align_val_t a) { return CountedAlloc(n, static_cast<size_t>(a)); }
void* operator new[](size_t n, std::align_val_t a) { return CountedAlloc(n, static_cast<size_t>(a)); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return CountedAlloc(n, 0); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return CountedAlloc(n, 0); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { CountedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { CountedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { CountedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { CountedFree(p); }

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

namespace {
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }

int EnvI(const char* n, int d) {
    const char* e = std::getenv(n);
    return e && *e ? std::atoi(e) : d;
}

void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
// One J3D-like material list (as tests/vgx_statelist.cpp makes them), at most kMaxList bytes.
constexpr uint32_t kMaxList = 640;
struct List { std::vector<uint8_t> bytes; std::vector<uint32_t> values; };
List MakeList() {
    static const uint8_t kBp[] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
                                  0x28, 0x29, 0x40, 0x41, 0x42, 0xF3, 0xF6, 0xF7, 0x10, 0x11, 0x25, 0x26};
    List l;
    uint8_t buf[kMaxList + 64] = {};
    uint8_t* p = buf;
    const unsigned cmds = 16 + Rn(28);
    for (unsigned c = 0; c < cmds && p - buf < static_cast<long>(kMaxList) - 60; ++c) {
        const uint32_t r = Rn(10);
        if (r < 6) {
            *p++ = 0x61;
            l.values.push_back(static_cast<uint32_t>(p - buf));
            Put32(p, static_cast<uint32_t>(kBp[Rn(sizeof kBp)]) << 24 | (Rnd() & 0xFFFFFF));
            p += 4;
        } else if (r < 7) {
            *p++ = 0x08;
            *p++ = Rn(2) ? 0x30 : 0x40;
            l.values.push_back(static_cast<uint32_t>(p - buf));
            Put32(p, Rnd());
            p += 4;
        } else if (r < 9) {
            const uint32_t addr = 0x1009u + Rn(10);
            *p++ = 0x10;
            *p++ = 0;
            *p++ = 0;
            *p++ = static_cast<uint8_t>(addr >> 8);
            *p++ = static_cast<uint8_t>(addr);
            l.values.push_back(static_cast<uint32_t>(p - buf));
            Put32(p, addr == 0x1009u ? Rn(3) : Rnd());
            p += 4;
        } else {
            const uint32_t row = Rn(20) * 3;
            *p++ = 0x10;
            *p++ = 0;
            *p++ = 11;
            *p++ = static_cast<uint8_t>((row * 4) >> 8);
            *p++ = static_cast<uint8_t>(row * 4);
            for (unsigned k = 0; k < 12; ++k) {
                float f = static_cast<float>(static_cast<int>(Rn(2001)) - 1000) / 250.0f;
                uint32_t u;
                std::memcpy(&u, &f, 4);
                Put32(p, u);
                p += 4;
            }
        }
    }
    while ((p - buf) % 32) *p++ = 0;
    l.bytes.assign(buf, p);
    return l;
}

uint64_t StateHash() {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&vgx::g);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(vgx::State); ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
void Sink(vgx::CmdBuf& b) { vgx::backend::Execute(b); }

constexpr uint32_t kSlot = kMaxList;                   // bytes a list's place takes in a region
constexpr uint32_t kPersistent = 48;                   // lists every scene calls
constexpr uint32_t kSceneSlots = 4096;                 // places in the scene heap
constexpr uint32_t kGalaxies = 7;
alignas(4096) uint8_t g_ram[(kPersistent + kSceneSlots) * kSlot];
uint8_t* const g_persist = g_ram;
uint8_t* const g_scene = g_ram + kPersistent * kSlot;
} // namespace

int main() {
    const int mode = EnvI("GXV_SL_SCENEDROP", 0);
    const int loops = EnvI("SL_LOOPS", 8), frames = EnvI("SL_FRAMES", 3);
    const int moved = EnvI("SL_MOVED_PERMILLE", 150); // lists of a galaxy that land at another place on a visit
    // (pictures without list calls after each drawn one: a visit is ~20 s of play on the console, so the geometry cache's per-list
    // tables - its entries and the bypass marks material lists get at their first call - are swept as there, kRetainFrames = 600)
    const int idle = EnvI("SL_IDLE_FRAMES", 700);
    vgx::ResetState();
    vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    vgx::hle::RegisterGuestRam(g_ram, sizeof g_ram);
    // The session's content: fixed per seed, the same in every mode.
    std::vector<List> persist;
    for (uint32_t i = 0; i < kPersistent; ++i) persist.push_back(MakeList());
    std::vector<std::vector<List>> galaxy(kGalaxies);
    std::vector<std::vector<uint32_t>> home(kGalaxies); // each list's usual place
    for (uint32_t g = 0; g < kGalaxies; ++g) {
        const uint32_t n = 280 + Rn(420);
        for (uint32_t i = 0; i < n; ++i) {
            galaxy[g].push_back(MakeList());
            home[g].push_back(i * 3 + Rn(3)); // (places 0 .. ~2,100: galaxies overlap in the heap, as on the console)
        }
    }
    for (uint32_t i = 0; i < kPersistent; ++i) {
        std::memcpy(g_persist + i * kSlot, persist[i].bytes.data(), persist[i].bytes.size());
        vgx::hle::NotifyGuestWrite(g_persist + i * kSlot, kSlot);
    }
    uint64_t totalCalls = 0;
    size_t firstLoopNew = 0, lastLoopNew = 0;
    for (int loop = 0; loop < loops; ++loop) {
        for (uint32_t g = 0; g < kGalaxies; ++g) {
            // the scene's heap: the galaxy's lists at their places, a share of them elsewhere this time (the layout's own random stream,
            // so that the session is the same in every mode)
            uint64_t layoutRng = 0x51ED2701u + static_cast<uint64_t>(loop) * 7919u + g * 104729u;
            auto lr = [&layoutRng](uint32_t n) {
                layoutRng = layoutRng * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<uint32_t>(layoutRng >> 33) % n;
            };
            std::vector<uint32_t> place(galaxy[g].size());
            std::vector<uint8_t> taken(kSceneSlots, 0);
            for (size_t i = 0; i < place.size(); ++i) {
                uint32_t s = home[g][i];
                if (static_cast<int>(lr(1000)) < moved) s = 2200 + lr(kSceneSlots - 2200);
                while (taken[s]) s = (s + 1) % kSceneSlots;
                taken[s] = 1;
                place[i] = s;
                std::memcpy(g_scene + s * kSlot, galaxy[g][i].bytes.data(), galaxy[g][i].bytes.size());
            }
            vgx::hle::NotifyGuestWrite(g_scene, kSceneSlots * kSlot);
            uint64_t digest = 1469598103934665603ull;
            for (int f = 0; f < frames; ++f) {
                auto call = [&](uint8_t* list, List& l) {
                    const uint32_t r = Rnd() % 100;
                    if (r < 15) {
                        vgx::hle::NotifyGuestWrite(list, static_cast<uint32_t>(l.bytes.size())); // written again with what it holds
                    } else if (r < 19 && !l.values.empty()) {
                        uint8_t* v = list + l.values[Rnd() % l.values.size()]; // a register changed (an animated colour)
                        v[3] = static_cast<uint8_t>(v[3] + 1 + Rnd() % 255);
                        vgx::hle::NotifyGuestWrite(v, 4);
                        std::memcpy(l.bytes.data() + (v - list), v, 4);
                    }
                    vgx::CallList(list, static_cast<uint32_t>(l.bytes.size()));
                    digest = (digest ^ StateHash()) * 1099511628211ull;
                    ++totalCalls;
                };
                for (uint32_t i = 0; i < kPersistent; ++i) call(g_persist + i * kSlot, persist[i]);
                for (size_t i = 0; i < place.size(); ++i) call(g_scene + place[i] * kSlot, galaxy[g][i]);
                vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
                vgx::QueueCopyDisp(true);
                vgx::FrontPresent(false, nullptr);
                for (int k = 0; k < idle; ++k) vgx::FrontPresent(false, nullptr);
            }
            // the scene's destruction (vgx_galaxy.cpp CopyDropService), then its heap is the next scene's
            const void* lo[2] = {g_scene, nullptr};
            const void* hi[2] = {g_scene + kSceneSlots * kSlot, nullptr};
            const vgx::StateListStore sl = vgx::StateListsSceneGone(lo, hi, 2, mode);
            std::memset(g_scene, 0, kSceneSlots * kSlot);
            vgx::hle::NotifyGuestWrite(g_scene, kSceneSlots * kSlot);
            std::printf("visit %d.%u digest %016llx\n", loop, g, static_cast<unsigned long long>(digest));
            std::fprintf(stderr, "visit %d.%u (%zu lists): store %u entries (%llu KiB) after the destroy, %u freed (%llu KiB), %llu clears; "
                                 "`new` holds %zu KiB in %zu blocks\n",
                         loop, g, place.size(), sl.entries, static_cast<unsigned long long>(sl.bytes / 1024), sl.dropped,
                         static_cast<unsigned long long>(sl.droppedBytes / 1024), static_cast<unsigned long long>(sl.clears), g_newLive / 1024, g_newBlocks);
            if (g == kGalaxies - 1) {
                if (loop == 1) firstLoopNew = g_newLive;
                lastLoopNew = g_newLive;
            }
        }
    }
    uint64_t c[5];
    vgx::StateListCounters(c);
    std::fprintf(stderr, "mode %d: %llu calls; state lists %llu replays, %llu parses; `new` at the end of loop 2: %zu KiB, of the last loop: %zu KiB "
                         "(%+.1f KiB a loop)\n",
                 mode, static_cast<unsigned long long>(totalCalls), static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]),
                 firstLoopNew / 1024, lastLoopNew / 1024, loops > 2 ? (static_cast<double>(lastLoopNew) - firstLoopNew) / 1024.0 / (loops - 2) : 0.0);
    std::fprintf(stderr, "growth-kib-per-loop %.1f\n", loops > 2 ? (static_cast<double>(lastLoopNew) - firstLoopNew) / 1024.0 / (loops - 2) : 0.0);
    return 0;
}
