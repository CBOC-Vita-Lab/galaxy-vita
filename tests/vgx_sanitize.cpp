// Pure checks of platform/vgx/src/vgx_sanitize.h: exact for valid input, a "no" for the inputs it exists for.
#include "vgx_sanitize.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

using namespace vgx;
static unsigned failures = 0;
static void Check(bool ok, const char* name) {
    if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
}
static const float kNan = std::numeric_limits<float>::quiet_NaN(), kInf = std::numeric_limits<float>::infinity();

int main() {
    using namespace sanitize;
    // Mode parsing: unknown bits and non-numbers are off.
    Check(ParseMode(0) == 0 && ParseMode(1) == 1 && ParseMode(3) == 3 && ParseMode(15) == 15, "known modes parse");
    Check(ParseMode(16) == 0 && ParseMode(-1) == 0 && ParseMode(99) == 0, "unknown bits are off");
    Check(Acts(1) && Acts(3) && !Acts(4) && !Acts(5) && !Acts(0) && !Acts(15 | 4), "log-only never acts");
    // Finite.
    Check(Finite(0.0f) && Finite(-0.0f) && Finite(1e38f) && Finite(std::numeric_limits<float>::denorm_min()), "finite values");
    Check(!Finite(kNan) && !Finite(kInf) && !Finite(-kInf), "NaN and Inf");
    const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float bad[16];
    std::memcpy(bad, m, sizeof m);
    bad[5] = kNan; bad[15] = kInf;
    Check(CountNonFinite(m, 16) == 0 && CountNonFinite(bad, 16) == 2 && CountNonFinite(bad, 5) == 0, "non-finite words counted");
    // Viewport.
    Check(ViewportUsable(0, 0, 640, 528) && ViewportUsable(-320, -264, 1280, 1056) && ViewportUsable(0, 0, 0.5f, 0.5f), "ordinary viewports");
    Check(!ViewportUsable(0, 0, 0, 528) && !ViewportUsable(0, 0, 640, 0) && !ViewportUsable(0, 0, -640, 528) && !ViewportUsable(0, 0, 640, -1), "no area");
    Check(!ViewportUsable(kNan, 0, 640, 528) && !ViewportUsable(0, kInf, 640, 528) && !ViewportUsable(0, 0, kNan, 1) && !ViewportUsable(0, 0, 1, kInf), "non-finite");
    Check(!ViewportUsable(1e9f, 0, 640, 528) && !ViewportUsable(0, 0, 1e9f, 528), "absurd");
    // Depth range.
    Check(DepthValue(0.25f, 0) == 0.25f && DepthValue(0, 1) == 0 && DepthValue(1, 0) == 1 && DepthValue(-1, 0) == 0 && DepthValue(2, 0) == 1, "depth values");
    Check(DepthValue(kNan, 0) == 0 && DepthValue(kNan, 1) == 1 && DepthValue(kInf, 0) == 0, "non-finite depth");
    // Copies.
    CopyTexCmd c;
    c.src = {0, 0, 640, 456}; c.dstW = 640; c.dstH = 456;
    for (int i = 0; i < 4; ++i) c.mat[i][i] = 1;
    Check(CopyUsable(c, 640, 456), "full-screen copy");
    c.src = {320, 228, 320, 228}; c.dstW = 160; c.dstH = 114;
    Check(CopyUsable(c, 640, 456), "quarter copy to a half-size texture");
    c.src = {-16, -16, 64, 64}; c.dstW = 64; c.dstH = 64;
    Check(CopyUsable(c, 640, 456), "copy that overhangs the EFB");
    CopyTexCmd z = c;
    z.src.w = 0; Check(!CopyUsable(z, 640, 456), "source of no width");
    z = c; z.src.h = -4; Check(!CopyUsable(z, 640, 456), "source of negative height");
    z = c; z.dstW = 0; Check(!CopyUsable(z, 640, 456), "destination of no width");
    z = c; z.dstH = 9000; Check(!CopyUsable(z, 640, 456), "destination beyond any texture");
    z = c; z.src = {640, 0, 8, 8}; Check(!CopyUsable(z, 640, 456), "source right of the EFB");
    z = c; z.src = {-100, 0, 64, 64}; Check(!CopyUsable(z, 640, 456), "source left of the EFB");
    z = c; z.mat[1][2] = kNan; Check(!CopyUsable(z, 640, 456), "NaN colour transform");
    z = c; z.add[3] = kInf; Check(!CopyUsable(z, 640, 456), "Inf colour offset");
    z = c; z.srcTex = 7; z.src = {700, 900, 64, 64}; Check(CopyUsable(z, 640, 456), "a copy out of an atlas names the atlas, not the EFB");
    z = c; z.dstTexW = 1024; z.dstTexH = 1024; Check(CopyUsable(z, 640, 456), "copy into an atlas cell");
    z = c; z.dstTexW = 1024; z.dstTexH = 0; Check(!CopyUsable(z, 640, 456), "atlas of no height");
    // Uploads and index ranges.
    Check(UploadUsable(1, 1, 1, 4) && UploadUsable(1024, 1024, 11, 4) && UploadUsable(4096, 4096, 1, 1), "ordinary uploads");
    Check(!UploadUsable(0, 8, 1, 4) && !UploadUsable(8, 0, 1, 4) && !UploadUsable(8, 8, 0, 0) && !UploadUsable(8, 8, 1, 0) && !UploadUsable(5000, 8, 1, 4), "empty uploads");
    Check(IndexRangeUsable(0, 0, 0) && IndexRangeUsable(0, 6, 6) && IndexRangeUsable(4, 2, 6) && IndexRangeUsable(6, 0, 6), "index ranges inside");
    Check(!IndexRangeUsable(0, 7, 6) && !IndexRangeUsable(7, 0, 6) && !IndexRangeUsable(5, 2, 6) && !IndexRangeUsable(0xFFFFFFFFu, 2, 6) && !IndexRangeUsable(2, 0xFFFFFFFFu, 6), "index ranges outside");
    // Vertex scan: layout with position (16 bytes) and one texture coordinate.
    const uint32_t layout = VL_TEX0;
    const VtxLayout L = MakeLayout(layout);
    std::vector<uint8_t> vtx(L.stride * 4);
    auto put = [&](int v, float x, float y, float zz, float u, float w) {
        float f[4] = {x, y, zz, 0};
        std::memcpy(&vtx[v * L.stride], f, 16);
        float uv[2] = {u, w};
        std::memcpy(&vtx[v * L.stride + L.offTex[0]], uv, 8);
    };
    put(0, -1, -1, 0, 0, 0); put(1, 1, -1, 0, 1, 0); put(2, 1, 1, 0, 1, 1); put(3, -1, 1, 0, 0, 1);
    const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    DrawRec d;
    d.layoutMask = layout; d.idxOffset = 0; d.idxCount = 6; d.vtxFirst = 0; d.vtxCount = 4; d.vtxOffset = 0;
    VertexScan s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(!s.badRefs && !s.outside && !s.nonFinitePos && !s.extremePos && !s.nonFiniteAttr, "valid quad scans clean");
    put(2, kNan, 1, 0, 1, 1);
    s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(s.nonFinitePos == 2 && !s.badRefs, "NaN position counted once per reference");
    put(2, 1, 1, 0, kInf, 1);
    s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(s.nonFiniteAttr == 2 && !s.nonFinitePos, "Inf texture coordinate");
    put(2, 1e12f, 1, 0, 1, 1);
    s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(s.extremePos == 2 && !s.nonFinitePos, "extreme position");
    put(2, 1, 1, 0, 1, 1);
    d.vtxCount = 3;
    s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(s.outside == 1 && !s.badRefs, "index outside the declared range");
    d.vtxCount = 4;
    s = ScanVertices(d, vtx.data(), L.stride * 3, idx, 6);
    Check(s.badRefs == 1, "vertex outside the data");
    d.idxCount = 9;
    s = ScanVertices(d, vtx.data(), vtx.size(), idx, 6);
    Check(s.badRefs == 1, "index range outside the indices");
    // Property: random valid input is never refused.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> any(-1.0e6f, 1.0e6f), pos(1.0f, 4000.0f);
    unsigned refused = 0;
    for (int i = 0; i < 20000; ++i) {
        float t[16];
        for (float& v : t) v = any(rng);
        refused += CountNonFinite(t, 16) != 0;
        refused += !ViewportUsable(any(rng) * 0.05f, any(rng) * 0.05f, pos(rng), pos(rng));
        const float zn = std::uniform_real_distribution<float>(0, 1)(rng), zf = std::uniform_real_distribution<float>(0, 1)(rng);
        refused += DepthValue(zn, 0) != zn || DepthValue(zf, 1) != zf;
        CopyTexCmd r;
        r.src = {static_cast<int16_t>(rng() % 600), static_cast<int16_t>(rng() % 400), static_cast<int16_t>(1 + rng() % 40), static_cast<int16_t>(1 + rng() % 56)};
        r.dstW = static_cast<uint16_t>(1 + rng() % 1024); r.dstH = static_cast<uint16_t>(1 + rng() % 1024);
        for (int k = 0; k < 4; ++k) { r.add[k] = any(rng); for (int j = 0; j < 4; ++j) r.mat[k][j] = any(rng); }
        refused += !CopyUsable(r, 640, 456);
        refused += !UploadUsable(1 + rng() % 4096, 1 + rng() % 4096, 1 + rng() % 13, 1 + rng() % 100);
        const uint32_t total = rng() % 100000, off = total ? rng() % total : 0;
        refused += !IndexRangeUsable(off, total - off, total);
    }
    Check(refused == 0, "20000 random valid inputs are all accepted");
    if (failures) return 1;
    std::printf("PASS: vgx_sanitize helpers (valid input accepted exactly, degenerate input refused)\n");
    return 0;
}
