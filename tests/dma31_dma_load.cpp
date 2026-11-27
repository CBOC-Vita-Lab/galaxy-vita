// dma31: host test of platform/vgx/vita/vgx_dma_load.h (GXV_DMA_LOAD) with a stub DMA engine and a stub clock.
// Checks every mode: 0 = memcpy and nothing counted; 1 = the engine for copies >= the minimum, memcpy below it and when the engine refuses;
// 2 = verify finds a stale copy (an engine that copies an old version of the source, as one that missed the CPU cache's write-back would)
// and repairs it; 3 = census only; the bytes are always the source's; the census line.
//   tools/test_dma31.sh
#include "../platform/vgx/vita/vgx_dma_load.h"
#include <string>
#include <vector>

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
uint64_t g_us = 0;
uint64_t Clock() { return g_us += 10; }
unsigned g_dmaCalls = 0;
int g_dmaResult = 0;
bool g_stale = false; // the engine copies what memory held before the CPU's last write (all bytes 0x5A)
int Engine(void* dst, const void* src, unsigned n) {
    ++g_dmaCalls;
    if (g_dmaResult < 0) return g_dmaResult;
    if (g_stale) std::memset(dst, 0x5A, n);
    else std::memcpy(dst, src, n);
    return 0;
}
std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 9));
    return v;
}
} // namespace

int main() {
    using namespace vgx::dmaload;
    const auto big = Pattern(256 * 256 * 4, 3), small = Pattern(32 * 32 * 4, 9);
    std::vector<uint8_t> dst(big.size());
    {
        Config c; Census s;
        Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock);
        Check(dst == big && g_dmaCalls == 0 && s.copies == 0 && g_us == 0, "mode 0: memcpy, no clock read, nothing counted");
    }
    {
        Config c; c.mode = 1; Census s;
        std::fill(dst.begin(), dst.end(), 0);
        const bool d1 = Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock);
        Check(d1 && dst == big && g_dmaCalls == 1, "mode 1: a 256 KiB copy goes to the engine, the bytes are the source's");
        std::vector<uint8_t> d2(small.size());
        const bool s1 = Copy(c, s, d2.data(), small.data(), static_cast<uint32_t>(small.size()), &Engine, &Clock);
        Check(!s1 && d2 == small && g_dmaCalls == 1, "mode 1: a 4 KiB copy (under 64 KiB) stays with the CPU");
        g_dmaResult = -1;
        std::fill(dst.begin(), dst.end(), 0);
        const bool r = Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock);
        g_dmaResult = 0;
        Check(!r && dst == big && s.refused == 1, "mode 1: an engine error falls back to memcpy and is counted");
        Check(s.copies == 3 && s.dmaCopies == 1 && s.dmaBytes == big.size() && s.bytes == 2 * big.size() + small.size(), "mode 1: census counts");
        char line[512];
        Line(line, sizeof line, c, s);
        std::printf("    %s\n", line);
        Check(std::string(line).find("3 texture level-0 copies") != std::string::npos && std::string(line).find("by the DMA engine 1") != std::string::npos,
              "mode 1: the census line");
    }
    {
        Config c; c.mode = 2; Census s;
        g_stale = true;
        bool differed = false;
        std::fill(dst.begin(), dst.end(), 0);
        Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock, &differed);
        g_stale = false;
        Check(differed && s.mismatches == 1 && dst == big, "mode 2: a stale engine copy is found and repaired by the CPU");
        differed = false;
        Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock, &differed);
        Check(!differed && s.mismatches == 1 && s.dmaCopies == 2, "mode 2: a good engine copy passes the check");
    }
    {
        Config c; c.mode = 3; Census s;
        const unsigned calls = g_dmaCalls;
        Copy(c, s, dst.data(), big.data(), static_cast<uint32_t>(big.size()), &Engine, &Clock);
        Check(g_dmaCalls == calls && s.copies == 1 && s.dmaCopies == 0 && s.us > 0 && dst == big, "mode 3: census only, the CPU copies");
    }
    {
        Config c; c.mode = 1; c.minBytes = 8192; Census s;
        std::vector<uint8_t> d(8192), src = Pattern(8192, 1);
        Copy(c, s, d.data(), src.data(), 8192, &Engine, &Clock);
        Check(s.dmaCopies == 1 && d == src, "mode 1: the minimum itself goes to the engine");
    }
    std::printf("%s dma31 dma load (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
