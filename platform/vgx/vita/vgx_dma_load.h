// dma31 (17 Nov 2026), GXV_DMA_LOAD: the one large CPU copy of a texture upload done by the Vita's DMA engine (sceDmacMemcpy) instead of
// the CPU. Where: vgxvita_uploadSwizzled (vgx_host_galaxy_vita.cpp, GXV_TEX_SWIZZLE=1, the Vita default) copies every uncompressed game
// texture's level 0 (the decoded texels, in the back end's cached heap) into a GPU-mapped linear scratch before the transfer unit makes the
// mip levels and the swizzled chain: width x height x 2 or 4 bytes (a 128x128 RGBA8 texture is 64 KiB, 256x256 is 256 KiB), on the render
// thread, mostly while a stage loads (Observatory: 259 uploads, ~7 MiB, 691 ms of upload time in the 2.3 trace). The scratch is
// GPU-mapped (not cached for the CPU), where CPU stores are slow: Strikers measured 339-363 MB/s for sceClibMemcpy into such memory and
// 866-935 MB/s for sceDmacMemcpy (17 Nov console), and ~400 MB/s for the DMA engine at 768 bytes, so only large copies are given to it.
// sceDmacMemcpy is synchronous: the render thread waits for it as it waited for its own copy; the gain is only the copy's speed.
// Modes (0 = off, the default; read once):
//   0  memcpy, nothing counted (as before)
//   1  copies of GXV_DMA_LOAD_MIN bytes or more (default 65536) by sceDmacMemcpy; smaller ones, and any the call refuses, by memcpy
//   2  verify: as 1, then the copy is compared with its source byte for byte; a difference is counted (and logged once) and the copy is
//      made again by memcpy, so the texture is right either way. This is the check that the DMA engine saw the CPU's latest bytes (the
//      source is in cached memory; the SDK has no user call to write the CPU cache back, so the kernel's DMA call must do it).
//   3  census: memcpy as 0, but counted and timed - the "before" of a console A/B
// One basic-log line "dma-load:" every 100 copies (totals since the start).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vgx::dmaload {
using DmaFn = int (*)(void* dst, const void* src, unsigned size); // sceDmacMemcpy: < 0 on error
using ClockFn = uint64_t (*)();                                    // microseconds
struct Config {
    int mode = 0;
    uint32_t minBytes = 65536;
};
struct Census {
    uint64_t copies = 0, bytes = 0, us = 0;          // every copy made by this path (any mode but 0)
    uint64_t dmaCopies = 0, dmaBytes = 0, dmaUs = 0; // those the DMA engine made
    uint64_t refused = 0, mismatches = 0, verifyUs = 0;
};
// The copy (dst and src do not overlap). Returns true when the DMA engine made it.
inline bool Copy(const Config& c, Census& s, void* dst, const void* src, uint32_t n, DmaFn dma, ClockFn clock, bool* mismatchOut = nullptr) {
    if (c.mode <= 0 || !clock) {
        std::memcpy(dst, src, n);
        return false;
    }
    const bool big = (c.mode == 1 || c.mode == 2) && dma && n >= c.minBytes && n > 0;
    const uint64_t t0 = clock();
    bool byDma = false;
    if (big) {
        byDma = dma(dst, src, n) >= 0;
        if (!byDma) ++s.refused;
    }
    if (!byDma) std::memcpy(dst, src, n);
    const uint64_t t1 = clock();
    ++s.copies;
    s.bytes += n;
    s.us += t1 - t0;
    if (byDma) {
        ++s.dmaCopies;
        s.dmaBytes += n;
        s.dmaUs += t1 - t0;
        if (c.mode == 2) {
            const bool same = std::memcmp(dst, src, n) == 0;
            if (!same) {
                ++s.mismatches;
                std::memcpy(dst, src, n);
                if (mismatchOut) *mismatchOut = true;
            }
            s.verifyUs += clock() - t1;
        }
    }
    return byDma;
}
// The census line's text (out: at least 320 bytes).
inline int Line(char* out, unsigned size, const Config& c, const Census& s) {
    const double mb = static_cast<double>(s.bytes) / 1048576.0, dmaMb = static_cast<double>(s.dmaBytes) / 1048576.0;
    const uint64_t cpuBytes = s.bytes - s.dmaBytes, cpuUs = s.us - s.dmaUs;
    return std::snprintf(out, size,
                         "dma-load: GXV_DMA_LOAD=%d (min %u bytes): %llu texture level-0 copies, %.2f MB in %.1f ms; by the DMA engine %llu (%.2f MB, %.1f ms, "
                         "%.0f MB/s), by the CPU %llu (%.0f MB/s); refused %llu; verify: %llu differed, %.1f ms comparing",
                         c.mode, c.minBytes, static_cast<unsigned long long>(s.copies), mb, static_cast<double>(s.us) / 1000.0,
                         static_cast<unsigned long long>(s.dmaCopies), dmaMb, static_cast<double>(s.dmaUs) / 1000.0,
                         s.dmaUs ? static_cast<double>(s.dmaBytes) / static_cast<double>(s.dmaUs) : 0.0, static_cast<unsigned long long>(s.copies - s.dmaCopies),
                         cpuUs ? static_cast<double>(cpuBytes) / static_cast<double>(cpuUs) : 0.0, static_cast<unsigned long long>(s.refused),
                         static_cast<unsigned long long>(s.mismatches), static_cast<double>(s.verifyUs) / 1000.0);
}
} // namespace vgx::dmaload
