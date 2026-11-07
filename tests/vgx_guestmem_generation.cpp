// vgx_guestmem.cpp's write generations against a model: a range's generation moves exactly when a write notification
// touched one of the 4 KiB pages under it (never for writes elsewhere), for ranges from one byte to the whole bank,
// and starting on or off page boundaries. tools/test_vgx_guestmem_generation.sh.
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr uint32_t kBank = 2u << 20, kPage = 4096;
alignas(4096) unsigned char ram[kBank];
uint64_t state = 0x243f6a8885a308d3ull;
uint32_t Next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 33);
}
void Require(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::abort();
    }
}
struct Range {
    uint32_t off, n;
};
// Ranges of every shape: short, page-sized and longer, starting on and off page and 64 KiB boundaries.
Range RandomRange() {
    static const uint32_t kLens[] = {1, 3, 4095, 4096, 4097, 8192, 60000, 65536, 65537, 131072, 200000, 1u << 20, kBank};
    uint32_t n = (Next() & 3) ? kLens[Next() % (sizeof kLens / sizeof *kLens)] : 1 + Next() % kBank;
    if (n > kBank) n = kBank;
    uint32_t off;
    switch (Next() % 4) {
    case 0: off = (Next() % (kBank / 65536)) * 65536; break;
    case 1: off = (Next() % (kBank / kPage)) * kPage; break;
    default: off = Next() % kBank; break;
    }
    if (off + n > kBank) off = kBank - n;
    return {off, n};
}
bool Overlap(Range a, Range b) {  // in pages
    return a.off / kPage <= (b.off + b.n - 1) / kPage && b.off / kPage <= (a.off + a.n - 1) / kPage;
}
}  // namespace

int main() {
    vgx::hle::RegisterGuestRam(ram, kBank);
    Require(vgx::GuestWriteGeneration(ram, 0) == vgx::kNotGuest, "an empty range is not tracked");
    Require(vgx::GuestWriteGeneration(ram + kBank - 4, 8) == vgx::kNotGuest, "a range leaving the bank is not tracked");
    struct Probe {
        Range r;
        uint64_t gen;
        bool written;
    };
    std::vector<Probe> probes;
    for (int i = 0; i < 400; ++i) {
        const Range r = RandomRange();
        probes.push_back({r, vgx::GuestWriteGeneration(ram + r.off, r.n), false});
    }
    unsigned moved = 0, still = 0;
    for (int step = 0; step < 120000; ++step) {
        if (Next() % 6 == 0) {  // (mostly small writes, as material display lists are; now and then a long one)
            const Range w = (Next() % 32) ? Range{Next() % (kBank - 64), 1 + Next() % 64} : RandomRange();
            vgx::hle::NotifyGuestWrite(ram + w.off, w.n);
            for (Probe& p : probes)
                if (Overlap(p.r, w)) p.written = true;
        } else {
            Probe& p = probes[Next() % probes.size()];
            const uint64_t gen = vgx::GuestWriteGeneration(ram + p.r.off, p.r.n);
            Require(gen != vgx::kNotGuest, "a range inside the bank is tracked");
            Require((gen != p.gen) == p.written, p.written ? "a write under the range moves its generation"
                                                           : "writes elsewhere leave the generation alone");
            (p.written ? moved : still)++;
            p.gen = gen;
            p.written = false;
        }
    }
    Require(moved > 1000 && still > 1000, "both outcomes were exercised");
    std::printf("PASS: generations move exactly with writes under the range (%u moved, %u unchanged)\n", moved, still);
}
