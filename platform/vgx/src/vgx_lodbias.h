// The texture LOD bias the back end gives vitaGL, as pure functions (strikersrev25, after the Strikers port's review of 11 Nov 2026).
//
// What reaches glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, n) on the Vita: vitaGL (textures.c) keeps (uint32_t)(n + GL_MAX_TEXTURE_LOD_BIAS) with
// the constant 31, and vglSetTexLodBias (utils/gxm_utils.h) does
//     control_words[0] = control_words[0] & 0xF81FFFFF | (bias << 21)
// with no mask: the bias field is bits 21..26 (6 bits), so the legal inputs are n = -31 .. +32 (field 0 .. 63, 31 = no bias). n = -32 is 0xFFFFFFFF:
// the field reads 63 (+32 steps: a four-level BLUR where a four-level sharpening was asked for) and bits 27..31 of the word, which belong to other
// fields (the gamma mode and more), are set as well; n <= -33 and n >= +33 spill the same way. Nothing of this shows in the desktop GL.
// So every sum that ends in that call is clamped to -31 .. +32 steps (eighths of a mip level) before it. The two sums the back end makes:
//   rung:    -lround(log2(960 / rungWidth) x 8 x GXV_RUNG_LODBIAS / 100) - GXV_RUNG_LODBIAS_EXTRA, kept in -31 .. 0
//   player:  that + GXV_PLAYER_LODBIAS x 8 / 100 (the player's own textures), kept in -31 .. +32 (the player's share is -24 .. +8)
// The shader-side bias of GXV_FIX_LODBIAS (u_tev[V_LODBIAS ..], -4 .. +3.97 levels) is a separate operand of the sampler instruction, not a part
// of this word: it is not summed here (see vgx_state.cpp / vgx_fixes.h LodBiasOf).
// Host test: tests/vgx_lodbias_steps.cpp (tools/test_vgx_lodbias_steps.sh).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vgx {
namespace lodbias {

constexpr long kMinSteps = -31; // vitaGL's glTexParameteri(GL_TEXTURE_LOD_BIAS) range, in eighths of a level: the 6-bit field holds n + 31
constexpr long kMaxSteps = 32;

// Any sum, into the range vitaGL can hold.
inline long ClampSteps(long steps) { return std::clamp(steps, kMinSteps, kMaxSteps); }

// The rung's bias (GXV_RUNG_LODBIAS = pct, GXV_RUNG_LODBIAS_EXTRA = extra eighths more negative at every rung): rungSX is the rung's width over 640
// (1.5 = 960 wide: no share of the rung's). The float product is kept to +-64 before it is rounded (a rung width near zero or not a number would
// otherwise reach lround out of its range), the sum after it in -31 .. 0: the rung only ever sharpens.
inline int8_t RungSteps(float rungSX, long pct, long extra) {
    const float levels = rungSX > 0.0f ? std::log2(1.5f / rungSX) : 0.0f; // (1.5 = 960 / 640, the top rung's width factor)
    float share = levels * 8.0f * static_cast<float>(pct) / 100.0f;
    if (!(share == share)) share = 0.0f;
    share = std::clamp(share, -64.0f, 64.0f);
    return static_cast<int8_t>(std::clamp(-std::lround(share) - extra, kMinSteps, 0L));
}

// GXV_PLAYER_LODBIAS in hundredths of a level (-300 .. +100 accepted) as eighths of a level.
inline long PlayerSteps(long hundredths) {
    const long h = std::clamp(hundredths, -300L, 100L);
    return ClampSteps(std::lround(static_cast<double>(h) * 8.0 / 100.0));
}

// The player's textures: the rung's bias plus the player's own, in vitaGL's range.
inline int8_t PlayerSum(int8_t rungSteps, long playerSteps) { return static_cast<int8_t>(ClampSteps(static_cast<long>(rungSteps) + playerSteps)); }

} // namespace lodbias
} // namespace vgx
