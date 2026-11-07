// GXV_TEXDEC_FAST (vgx_texdecode.cpp): the whole-tile decoder against the reference, byte for byte, over every GX format,
// random texels, random palettes of every TLUT format and entry count (also none, and too few), and sizes with and without
// partial tiles; then (argument "bench") the decode throughput of both per format.
//   tools/test_vgx_texdecode_fast.sh
#include "vgx_texdecode.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint32_t g_rand = 0x2545F491u;
uint32_t Rand() { g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5; return g_rand; }
const unsigned kFormats[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x8, 0x9, 0xA, 0xE};
const char* Name(unsigned f) {
    switch (f) {
    case 0x0: return "I4"; case 0x1: return "I8"; case 0x2: return "IA4"; case 0x3: return "IA8"; case 0x4: return "RGB565";
    case 0x5: return "RGB5A3"; case 0x6: return "RGBA8"; case 0x8: return "C4"; case 0x9: return "C8"; case 0xA: return "C14X2";
    case 0xE: return "CMPR"; default: return "?";
    }
}
} // namespace

int main(int argc, char** argv) {
    const unsigned dims[][2] = {{1, 1}, {2, 2}, {4, 4}, {8, 4}, {4, 8}, {8, 8}, {12, 8}, {16, 16}, {24, 40}, {32, 8}, {64, 64},
                                {7, 9}, {128, 32}, {256, 256}, {512, 128}, {9, 4}, {16, 12}};
    unsigned cases = 0, fast = 0;
    for (unsigned fmt : kFormats)
        for (const auto& dm : dims)
            for (unsigned seed = 0; seed < 6; ++seed) {
                const unsigned w = dm[0], h = dm[1];
                std::vector<uint8_t> src(vgx::TexLevelSize(w, h, fmt) + 3);
                for (auto& b : src) b = static_cast<uint8_t>(Rand());
                if (fmt == 0xE && seed & 1) // (many three-colour blocks: c0 <= c1)
                    for (size_t i = 1; i + 8 <= src.size(); i += 8) { src[i] = src[i + 2]; src[i + 1] = static_cast<uint8_t>(src[i + 3] | (seed & 2 ? 0 : 1)); }
                // A palette: none, a short one (indices beyond it read as zero), or a full one; each TLUT format.
                std::vector<uint8_t> tlut(32768 + 2);
                for (auto& b : tlut) b = static_cast<uint8_t>(Rand());
                const uint32_t tlutFmt = seed % 3;
                const uint32_t entries = seed == 0 ? 0 : seed == 1 ? 5 : seed == 2 ? 200 : seed == 3 ? 256 : seed == 4 ? 16384 : 0xFFFF;
                const uint8_t* tl = seed == 0 ? nullptr : tlut.data() + 1;
                std::vector<uint8_t> ref(size_t(w) * h * 4 + 64, 0xCD), got(size_t(w) * h * 4 + 64, 0xCD);
                vgx::detail::DecodeTextureRef(src.data() + 1, w, h, fmt, tl, tlutFmt, entries, ref.data() + 32);
                const bool taken = vgx::detail::DecodeTextureFast(src.data() + 1, w, h, fmt, tl, tlutFmt, entries, got.data() + 32);
                if (!taken) continue;
                ++fast;
                char why[160];
                std::snprintf(why, sizeof why, "fast decode equals the reference (%s %ux%u seed %u)", Name(fmt), w, h, seed);
                Require(ref == got, why);
                ++cases;
            }
    // The dispatcher (environment default: fast on) gives the reference's bytes too.
    for (unsigned fmt : kFormats) {
        const unsigned w = 64, h = 32;
        std::vector<uint8_t> src(vgx::TexLevelSize(w, h, fmt));
        for (auto& b : src) b = static_cast<uint8_t>(Rand());
        std::vector<uint8_t> tlut(512);
        for (auto& b : tlut) b = static_cast<uint8_t>(Rand());
        std::vector<uint8_t> a(size_t(w) * h * 4), b(size_t(w) * h * 4);
        vgx::DecodeTexture(src.data(), w, h, fmt, tlut.data(), 2, 256, a.data());
        vgx::detail::DecodeTextureRef(src.data(), w, h, fmt, tlut.data(), 2, 256, b.data());
        Require(a == b, "DecodeTexture equals the reference");
    }
    std::printf("texdecode fast: %u cases identical to the reference (%u taken by the fast path)\n", cases, fast);
    if (argc > 1 && !std::strcmp(argv[1], "bench")) {
        const unsigned w = 256, h = 256;
        for (unsigned fmt : kFormats) {
            std::vector<uint8_t> src(vgx::TexLevelSize(w, h, fmt));
            for (auto& b : src) b = static_cast<uint8_t>(Rand());
            std::vector<uint8_t> tlut(32768);
            for (auto& b : tlut) b = static_cast<uint8_t>(Rand());
            std::vector<uint8_t> out(size_t(w) * h * 4);
            double ms[2];
            for (int k = 0; k < 2; ++k) {
                const int reps = 200;
                const auto t0 = std::chrono::steady_clock::now();
                for (int r = 0; r < reps; ++r) {
                    if (k == 0) vgx::detail::DecodeTextureRef(src.data(), w, h, fmt, tlut.data(), 2, 256, out.data());
                    else if (!vgx::detail::DecodeTextureFast(src.data(), w, h, fmt, tlut.data(), 2, 256, out.data()))
                        vgx::detail::DecodeTextureRef(src.data(), w, h, fmt, tlut.data(), 2, 256, out.data());
                    __asm__ __volatile__("" : : "r"(out.data()) : "memory");
                }
                ms[k] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
            }
            std::printf("%-7s 256x256: reference %.3f ms, fast %.3f ms (%.1fx); output %.0f / %.0f MB/s\n", Name(fmt), ms[0], ms[1], ms[0] / ms[1],
                        w * h * 4 / 1048576.0 / (ms[0] / 1000), w * h * 4 / 1048576.0 / (ms[1] / 1000));
        }
    }
    return 0;
}
