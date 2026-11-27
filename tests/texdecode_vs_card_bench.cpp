// cook24: is a decoded-texture file cache worth it? Decode throughput of platform/vgx/src/vgx_texdecode.cpp per GX format on this host,
// against the memory card's measured sequential read speed (7.5 MB/s: the RAM shader pack's preload, 2.49 MB in 334 ms, console log).
// The Vita's Cortex-A9 is assumed ~10-15x slower than this host per core (weak estimate; the real number needs a console run: "load-split:" lines).
// A cache of decoded RGBA8 reads 4 bytes per texel from the card; the cooked install already stores the Wii-format bytes (0.5-4 bytes per texel).
#include "../platform/vgx/src/vgx_texdecode.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
    struct F { const char* name; uint32_t fmt; double srcBytesPerTexel; } formats[] = {
        {"I4", 0, 0.5}, {"I8", 1, 1}, {"IA4", 2, 1}, {"IA8", 3, 2}, {"RGB565", 4, 2}, {"RGB5A3", 5, 2}, {"RGBA8", 6, 4}, {"C8+TLUT", 9, 1}, {"CMPR", 14, 0.5}};
    const uint32_t w = 256, h = 256;
    std::vector<uint8_t> src(w * h * 4 + 4096), dst(w * h * 4), tlut(512);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i * 131u + (i >> 7));
    for (size_t i = 0; i < tlut.size(); ++i) tlut[i] = static_cast<uint8_t>(i * 17u);
    const double card = 7.5e6;
    std::printf("%-8s %12s %14s %16s %18s\n", "format", "host ns/texel", "Vita ns/texel*", "card ns/texel (RGBA8 file)", "card ns/texel (Wii bytes)");
    for (const F& f : formats) {
        const int reps = 40;
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) vgx::DecodeTexture(src.data(), w, h, f.fmt, tlut.data(), 2, 256, dst.data());
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (reps * double(w) * h);
        std::printf("%-8s %12.2f %14.1f %16.0f %18.0f\n", f.name, ns, ns * 12, 4.0 / card * 1e9, f.srcBytesPerTexel / card * 1e9);
    }
    std::printf("* host x12 (weak estimate).\n");
}
