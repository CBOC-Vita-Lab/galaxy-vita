// GXV_GPU_CENSUS (gpucensus31; default 0, log only, nothing drawn differently): two numbers per picture that rank GPU-bound scenes, read beside the
// "gpu-scenes:" line. Needs the per-draw detail of GXV_GPU_SCENES=2 (it uses the pixels that mode measures); GXV_GPU_CENSUS=1 asks for it.
//   * textures per fragment: sum over draws of (pixels x textures the fragment shader samples) / sum of pixels, apart for opaque, alpha-tested
//     (discard) and blended draws, and for the EFB scenes only. (Strikers gpuscene20: Dump 3.19 against 1.40 for another scene: texture reads, not
//     pixels, were what the slow scene paid.) Pixels are the "px" of mode 2 (measured from the vertices of stream draws, bounded by rectangles for
//     static ones: "bounded" is counted apart so a reader knows how much is a bound).
//   * parameter buffer estimate (Strikers PBEST): per picture the sum over draws of vertices x (16 + 8 per texture coordinate set + 4 colour) bytes
//     + 12 bytes a triangle. An ESTIMATE that ranks pictures; whether the GPU really runs short of its parameter buffer is decided by the console
//     (partial renders), not by this number. Printed as the mean and the max a picture and as MiB times `flight` pictures in flight against `limitMiB`.
// Pure: no GL, no globals (the back end owns one Census). Tests: tests/vgx_gpucensus.cpp, tools/test_vgx_gpucensus.sh.
#pragma once
#include <cstdint>
#include <cstdio>

namespace gpucensus {
enum Cls : int { kOpaque, kDiscard, kBlended, kOther, kCls };   // kOther: depth-only, no effect (not counted in the texture figure)
struct Census {
    // the window
    uint64_t pics = 0;
    double px[kCls] = {}, texPx[kCls] = {};     // pixels, pixels x textures sampled
    double boundPx[kCls] = {};                  // of px, the part that is only bounded by a rectangle
    uint64_t draws[kCls] = {};
    double efbOpaquePx = 0, efbOpaqueTexPx = 0; // opaque draws of the EFB scenes only
    double pbSumB = 0, pbMaxB = 0;              // per picture bytes
    uint64_t pbPicsOver = 0;                    // pictures whose estimate times `flight` is over limitMiB
    // the picture under way
    double curPb = 0;
    // settings
    double limitMiB = 16.0;
    int flight = 2;

    void Draw(Cls c, double pixels, bool bounded, unsigned nTex, bool efbScene, uint32_t vtx, uint32_t tris, unsigned nTexGens) {
        ++draws[c];
        px[c] += pixels;
        texPx[c] += pixels * nTex;
        if (bounded) boundPx[c] += pixels;
        if (c == kOpaque && efbScene) { efbOpaquePx += pixels; efbOpaqueTexPx += pixels * nTex; }
        curPb += static_cast<double>(vtx) * (16.0 + 8.0 * nTexGens + 4.0) + 12.0 * tris;
    }
    void EndPicture() {
        ++pics;
        pbSumB += curPb;
        if (curPb > pbMaxB) pbMaxB = curPb;
        if (curPb * flight > limitMiB * 1048576.0) ++pbPicsOver;
        curPb = 0;
    }
    static double Ratio(double a, double b) { return b > 0.0 ? a / b : 0.0; }
    // one line for the window; clears the window
    int Report(char* out, size_t cap) {
        const double n = pics ? static_cast<double>(pics) : 1.0;
        const double mib = 1048576.0;
        int w = std::snprintf(out, cap,
            "gpucensus31: %llu pictures; textures per fragment opaque %.2f (EFB scenes %.2f) discard %.2f blended %.2f; pixels a picture (Mpx) opaque %.2f discard %.2f blended %.2f "
            "(bounded-only share %.0f%% %.0f%% %.0f%%); draws a picture %.1f %.1f %.1f; pbest %.2f MiB mean %.2f max, x%d in flight %.2f MiB against %.0f: %llu pictures over",
            static_cast<unsigned long long>(pics), Ratio(texPx[kOpaque], px[kOpaque]), Ratio(efbOpaqueTexPx, efbOpaquePx), Ratio(texPx[kDiscard], px[kDiscard]), Ratio(texPx[kBlended], px[kBlended]),
            px[kOpaque] / n / 1e6, px[kDiscard] / n / 1e6, px[kBlended] / n / 1e6,
            100.0 * Ratio(boundPx[kOpaque], px[kOpaque]), 100.0 * Ratio(boundPx[kDiscard], px[kDiscard]), 100.0 * Ratio(boundPx[kBlended], px[kBlended]),
            draws[kOpaque] / n, draws[kDiscard] / n, draws[kBlended] / n,
            pbSumB / n / mib, pbMaxB / mib, flight, pbMaxB * flight / mib, limitMiB, static_cast<unsigned long long>(pbPicsOver));
        const double lim = limitMiB; const int fl = flight;
        *this = Census{};
        limitMiB = lim; flight = fl;
        return w;
    }
};
}  // namespace gpucensus
