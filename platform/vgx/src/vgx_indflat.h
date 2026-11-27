// GXV_IND_FLAT (freeze25; stage key "ind_flat", default 0 = the path is unchanged): the dependent (indirect) texture read of a SIMPLE warp is
// left out of the fragment shader, so the stage reads its texture at the plain coordinate (one fetch, no indirect-texture read, no coordinate
// arithmetic that waits for a fetch). It is a visible simplification (the heat haze / glow layer no longer wobbles); the layer stays.
//   1: only draws that blend (haze, glow, additive layers); 2: every draw (the opaque warped surfaces too).
// A "simple" warp has no wrapping, no add-previous and no bump alpha: the matrix and bias only offset the coordinate. Anything else is kept.
// Pure logic (tests/vgx_indflat_test.cpp). The keys it makes are ordinary direct keys: a shader pack made for them has them, a pack without them
// falls back to the runtime compile (no pack refill is needed for correctness; a refill removes the first-use hitch).
#pragma once
#include <cstdint>
namespace vgx::indflat {
inline constexpr uint32_t kMatrixMask = 0xFu << 9, kBiasMask = 0x7u << 4, kWrapMask = 0x3Fu << 13, kAddPrev = 1u << 20, kBumpAlpha = 0x3u << 7;
// ind = BP 0x10+stage (21 bits). True when the stage's indirect part may be dropped under `mode` for a draw that does / does not blend.
constexpr bool Eligible(int mode, uint32_t ind, bool blended) {
    if (mode <= 0) return false;
    if (mode == 1 && !blended) return false;
    if ((ind & (kMatrixMask | kBiasMask)) == 0) return false; // already direct
    return (ind & (kWrapMask | kAddPrev | kBumpAlpha)) == 0;
}
// Mixed into the shader-key memo's hash so a mode change can never return a shader id found under another mode (0 for mode 0: nothing changes).
constexpr uint64_t Salt(int mode) { return mode <= 0 ? 0 : 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(mode > 2 ? 2 : mode); }
} // namespace vgx::indflat
