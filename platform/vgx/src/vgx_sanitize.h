#pragma once
// GXV_SANITIZE: cheap checks on what the renderer hands to the GPU, for inputs that cannot come from a valid
// GX program and that a PowerVR core may answer with a hang (NaN / Inf matrices and positions, a zero or
// negative viewport, a copy of nothing, a texture of no texels, an index range outside the command buffer).
// Pure functions here; vgx_backend_gl.cpp decides what to do with a "no" (skip the draw / copy / upload) and
// counts it. Every check is exact for valid input: it never says "no" to anything a real draw uses.
//
//   GXV_SANITIZE=0 (default)  off, nothing is read or counted
//   bit 0 (1)  act: skip a draw, copy or upload that fails a check, count it, log the first few
//   bit 1 (2)  also scan the vertices of streamed draws (non-finite / extreme position, index outside the vertex data)
//   bit 2 (4)  log only: count and log, change nothing (for a desktop run that must stay bit-identical)
//   bit 3 (8)  strict: a non-finite texture-matrix, palette or TEV block also skips the draw (otherwise those are
//              only counted: unused rows of those blocks may hold anything)
#include "vgx_cmd.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vgx::sanitize {

enum : unsigned { kAct = 1, kVertexScan = 2, kLogOnly = 4, kStrict = 8 };
constexpr unsigned kKnownBits = kAct | kVertexScan | kLogOnly | kStrict;

// A mode word from the environment text value: 0 for anything that is not a number or has bits outside kKnownBits.
inline unsigned ParseMode(long v) { return (v > 0 && (v & ~static_cast<long>(kKnownBits)) == 0) ? static_cast<unsigned>(v) : 0u; }
inline bool Acts(unsigned mode) { return (mode & kAct) != 0 && (mode & kLogOnly) == 0; }

// Limits that no valid input comes near (the game's coordinates stay below 1e7; a screen is a few thousand pixels).
constexpr float kExtremeCoord = 1.0e9f;
constexpr float kMaxViewportCoord = 65536.0f;
constexpr int kMaxTextureSide = 4096;

inline bool FiniteBits(uint32_t word) { return (word & 0x7f800000u) != 0x7f800000u; }
inline bool Finite(float f) { uint32_t w; std::memcpy(&w, &f, 4); return FiniteBits(w); }
inline uint32_t CountNonFinite(const float* p, size_t n) {
    uint32_t bad = 0;
    for (size_t i = 0; i < n; ++i) bad += !Finite(p[i]);
    return bad;
}

// The render state's viewport (floats, before the render scale): positive size, finite, not absurd.
inline bool ViewportUsable(float x, float y, float w, float h) {
    return Finite(x) && Finite(y) && Finite(w) && Finite(h) && w > 0.0f && h > 0.0f && std::fabs(x) <= kMaxViewportCoord &&
           std::fabs(y) <= kMaxViewportCoord && w <= kMaxViewportCoord && h <= kMaxViewportCoord;
}
// A depth range value: non-finite becomes `fallback`, the rest is clamped to [0, 1] (what BuildRenderState does for finite values).
inline float DepthValue(float z, float fallback) { return !Finite(z) ? fallback : (z < 0.0f ? 0.0f : (z > 1.0f ? 1.0f : z)); }

// An EFB copy that moves at least one texel: positive source and destination size, a source that touches the EFB, a colour
// transform of finite numbers.
inline bool CopyUsable(const CopyTexCmd& c, int efbW, int efbH) {
    if (c.src.w <= 0 || c.src.h <= 0 || c.dstW <= 0 || c.dstH <= 0 || c.dstW > kMaxTextureSide || c.dstH > kMaxTextureSide) return false;
    // (a copy out of an atlas names a rectangle of that texture, not of the EFB)
    if (!c.srcTex && (c.src.x >= efbW || c.src.y >= efbH || c.src.x + c.src.w <= 0 || c.src.y + c.src.h <= 0)) return false;
    if (c.dstTexW && (c.dstTexW > kMaxTextureSide || c.dstTexH <= 0 || c.dstTexH > kMaxTextureSide)) return false;
    return CountNonFinite(&c.mat[0][0], 16) == 0 && CountNonFinite(c.add, 4) == 0;
}

// A texture upload with texels: a size, and a first level that holds data.
inline bool UploadUsable(uint32_t width, uint32_t height, size_t levels, size_t firstLevelBytes) {
    return width >= 1 && height >= 1 && width <= static_cast<uint32_t>(kMaxTextureSide) && height <= static_cast<uint32_t>(kMaxTextureSide) && levels >= 1 &&
           firstLevelBytes >= 1;
}

// The index range of a draw lies inside the index data it reads.
inline bool IndexRangeUsable(uint32_t idxOffset, uint32_t idxCount, size_t indexTotal) {
    return idxOffset <= indexTotal && idxCount <= indexTotal - idxOffset;
}

struct VertexScan {
    uint32_t badRefs = 0;     // a vertex that lies outside the vertex data
    uint32_t outside = 0;     // an index outside the draw's declared vertex range
    uint32_t nonFinitePos = 0;
    uint32_t extremePos = 0;  // finite but beyond kExtremeCoord
    uint32_t nonFiniteAttr = 0;
};
// Looks at every vertex the draw's indices name: `vertices` is the draw's vertex chunk, `vtxOffset` the draw's base inside it.
inline VertexScan ScanVertices(const DrawRec& d, const uint8_t* vertices, size_t vertexBytes, const uint16_t* indices, size_t indexTotal) {
    VertexScan s;
    if (!IndexRangeUsable(d.idxOffset, d.idxCount, indexTotal) || !indices) { s.badRefs = d.idxCount ? 1u : 0u; return s; }
    const VtxLayout L = MakeLayout(d.layoutMask);
    for (uint32_t i = 0; i < d.idxCount; ++i) {
        const uint32_t index = indices[static_cast<size_t>(d.idxOffset) + i];
        if (index < d.vtxFirst || index >= static_cast<uint64_t>(d.vtxFirst) + d.vtxCount) ++s.outside;
        const uint64_t start = static_cast<uint64_t>(d.vtxOffset) + static_cast<uint64_t>(index) * L.stride;
        if (!vertices || start > vertexBytes || L.stride > vertexBytes - start) { ++s.badRefs; continue; }
        float f[4];
        std::memcpy(f, vertices + start, sizeof f);
        bool finite = true, extreme = false;
        for (int k = 0; k < 3; ++k) { finite &= Finite(f[k]); extreme |= finite && std::fabs(f[k]) > kExtremeCoord; }
        s.nonFinitePos += !finite;
        s.extremePos += extreme;
        bool attr = true;
        if (d.layoutMask & VL_NRM) {
            float n[3];
            std::memcpy(n, vertices + start + L.offNrm, sizeof n);
            for (float v : n) attr &= Finite(v);
        }
        for (unsigned t = 0; t < 8; ++t)
            if (d.layoutMask & (VL_TEX0 << t)) {
                float uv[2];
                std::memcpy(uv, vertices + start + L.offTex[t], sizeof uv);
                attr &= Finite(uv[0]) && Finite(uv[1]);
            }
        s.nonFiniteAttr += !attr;
    }
    return s;
}

// What was refused, by reason (the backend adds to these; tests read them).
struct Counts {
    uint64_t nonFiniteUniform[UB_COUNT] = {};
    uint64_t badIndexRange = 0, badVertexRefs = 0, outsideRefs = 0, nonFinitePos = 0, extremePos = 0, nonFiniteAttr = 0;
    uint64_t badViewport = 0, badDepthRange = 0, badCopy = 0, badUpload = 0;
    uint64_t skippedDraws = 0, skippedCopies = 0, skippedUploads = 0;
    uint64_t Total() const {
        uint64_t t = badIndexRange + badVertexRefs + outsideRefs + nonFinitePos + extremePos + nonFiniteAttr + badViewport + badDepthRange + badCopy + badUpload;
        for (uint64_t u : nonFiniteUniform) t += u;
        return t;
    }
};

} // namespace vgx::sanitize
