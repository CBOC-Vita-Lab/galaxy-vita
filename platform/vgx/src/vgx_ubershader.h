// asyncsh31: GXV_UBERSHADER (default 0) -- a fragment "ubershader" for the pictures in which a draw's own (specialised) program is still being compiled.
//
// Why. A shader text that no cache has is compiled by libshacccg: 200-750 ms on the render thread in play (Gusty Garden 748 ms, Star Festival 298 ms),
// or, with the worker (GXV_ASYNC_SHADERS), on cores 1-2 while the draw waits. While it waits, the draw today is skipped (GXV_MISS_STANDIN=0), drawn with a
// look-alike program (GXV_MISS_STANDIN=1: NOT the same pixels), or -- Mario, the HUD, render-to-texture and opaque map draws -- compiled at once (the stall).
// An ubershader is one precompiled program that reads the TEV configuration of the key from uniforms and computes, per pixel, what the key's own text computes.
//
// What this file is (stage 1 of the design in reports/asyncsh31.md):
//   * UberIfaceOf / UberFsCovers: the program family (one text per varying + sampler interface) and which keys it takes under the switches of this run;
//   * GenerateUberFragmentShader: the text (GLSL ES 1.00, the generator's own formulas, operation for operation: the specialised text with every trim
//     GXV_FS_TRIM leaves out kept, which those trims' own proofs make the same values);
//   * UberFsConfig: the per-key uniform block u_ucfg (per key, not per draw: the draw's own u_tev block is uploaded as for the specialised program).
// The back end only counts (GXV_UBERSHADER=1: the "first-use: ubershader census" line); it never draws with it in this stage. The host proof is
// tests/vgx_ubershader_gl.cpp (tools/test_vgx_ubershader_gl.sh): every covered key, ubershader against the specialised text, RGBA8 bytes on Mesa.
//
// Not covered (the draw keeps today's path): more than kUberMaxStages stages, the silhouette fuse (GXV_SILH_FUSE), GXV_TEVIN_PREWRAP's dual-wrap rows,
// GXV_IND_PRECOMP's precomputed indirect stage, an indirect wrap code 7 (no GX meaning), GXV_FS_PROJ's texture2DProj reads, GXV_FS_HALF=2.
// The vertex side is not part of this stage: the family pairs with the draw's own vertex shader (the interface is what that shader writes).
#pragma once
#include "vgx_shadergen.h"
#include "vgx_common.h"
#include <cstdint>
#include <string>

namespace vgx {

constexpr int kUberMaxStages = 8;   // (2,297 of the 2,300 fragment keys of seed 1117 have at most 8)
constexpr int kUberStageRows = 13;  // vec4 rows of u_ucfg per stage
constexpr int kUberGlobalRows = 11; // vec4 rows of u_ucfg before the stages
constexpr int kUberRows = kUberGlobalRows + kUberMaxStages * kUberStageRows;

// The varyings and samplers a key's fragment shader declares: the specialised text and the ubershader of the same interface pair with the same vertex shader.
struct UberIface {
    uint8_t numTexGens = 0;
    uint8_t vec2Mask = 0;   // v_texN declared as vec2 (GXV_TEXVEC2, GXV_IND_VS)
    uint8_t col = 0;        // bit 0 v_col0, bit 1 v_col1
    uint8_t fogVtx = 0;     // the vertex-fog varying (GXV_FOGVTX)
    uint8_t texMask = 0;    // samplers u_tex0..7
    bool operator==(const UberIface& o) const {
        return numTexGens == o.numTexGens && vec2Mask == o.vec2Mask && col == o.col && fogVtx == o.fogVtx && texMask == o.texMask;
    }
    uint32_t Word() const { return numTexGens | (vec2Mask << 4) | (col << 12) | (fogVtx << 14) | (static_cast<uint32_t>(texMask) << 16); }
};
UberIface UberIfaceOf(const FsKey& k);

// Whether the ubershader computes this key under this run's switches; *why (if given) gets the first reason it does not (a string literal).
bool UberFsCovers(const FsKey& k, const char** why = nullptr);

// The ubershader text of an interface (as GenerateFragmentShader writes texts: no #version line). out.texMask / numTexGens / nTev are set as for a key.
void GenerateUberFragmentShader(const UberIface& iface, ShaderSource& out);

// u_ucfg for a covered key: kUberRows vec4 (4 * kUberRows floats). False when the key is not covered.
bool UberFsConfig(const FsKey& k, float* cfg);

// GXV_UBERSHADER (read once, default 0): 0 off (nothing runs); 1 census only (the back end counts first-use draws whose program waits for or runs the
// compiler, by "covered by the ubershader" or not: the "first-use: ubershader census" line). Drawing with it is a later stage (reports/asyncsh31.md).
// (inline: the back end's census needs no link to vgx_ubershader.cpp, so every host test that compiles the back end builds as before)
inline long UberMode() {
    static const long m = [] { const long v = EnvInt("GXV_UBERSHADER", 0); return v < 0 ? 0L : (v > 1 ? 1L : v); }();
    return m;
}

// Census (the back end's line; host-tested): one ask per first-use draw that needs a compile.
struct UberCensus {
    unsigned covered = 0;      // draws whose key the ubershader takes (they could have been drawn with it)
    unsigned blocking = 0;     // ... of which the back end compiled on the render thread (a stall today)
    unsigned notCovered = 0;   // draws whose key it does not take
    unsigned notCoveredBlocking = 0;
    unsigned ifaces = 0;       // distinct interfaces among the covered (the programs the family would need)
    const char* firstWhy = ""; // the first reason a key was not taken
    void Note(bool cov, bool block, const char* why) {
        if (cov) { ++covered; if (block) ++blocking; }
        else { ++notCovered; if (block) ++notCoveredBlocking; if (!*firstWhy && why) firstWhy = why; }
    }
};

} // namespace vgx
