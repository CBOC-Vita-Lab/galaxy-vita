// GXV_LODBIAS_SAMPLER: the GX texture unit's LOD bias given to the sampler instead of the fragment shader, as pure functions (lodmask37, idea 1 of
// FABLE-IDEAS.md). Host test: tests/vgx_lodsampler_test.cpp (tools/test_vgx_lodsampler.sh).
//
// Today (GXV_FIX_LODBIAS=1) every stage whose unit mip-maps with a non-zero bias reads texture2D(u_tex, uv, u_tev[V_LODBIAS ..]): on the SGX a read with a
// bias given by the shader is a dependent read. GXM keeps a bias in the texture's control word, in 1/8 levels, and the back end already drives that field
// per texture (vgx_lodbias.h: GL_TEXTURE_LOD_BIAS, -31 .. +32 eighths after the rung's, the player's and automip's terms). A GX bias is in 1/32 levels.
//
// A unit's bias moves to the sampler only when the picture cannot tell. Every condition below is a function of the texture units' mode0 / mode1 registers and
// of the texmaps the key reads (both are part of the key hash), never of a texture's identity (which the key hash does not carry):
//   (1) 1/8:      the bias in 1/32 levels is a multiple of 4 (else not representable: "not 1/8");
//   (2) range:    bias + every term the back end adds, at its worst, stays inside vitaGL's -31 .. +32 (else the back end would clamp it: "range");
//   (3) direct:   the draw reads no texmap indirectly (an indirect read has no bias in the shader, and the unit's texture may be the same object: "indirect");
//   (4) one bias: every texmap the draw reads is read with the same bias (a texture bound to two texmaps has one sampler bias, so two reads of a texture
//                 with different biases cannot both be exact; the key cannot see which texmaps share a texture, so a draw with different biases keeps the
//                 shader path whether or not they do: "mixed bias (shared texture possible)").
// A stage that is not moved keeps today's shader bias. Whatever is not moved is counted by the reason that kept it.
#pragma once
#include "vgx_lodbias.h"
#include <algorithm>
#include <cstdint>

namespace vgx {
namespace lodsampler {

// The class of a unit that has a bias (TexBind::lodWhy; 0 = the unit has none).
enum Why : uint8_t { kNone = 0, kMoved = 1, kNotEighth = 2, kRange = 3, kIndirect = 4, kMixed = 5, kWhyCount = 6 };
// (mode 2 = count only: kMoved then means "would move")

// The back end's terms (vgx_backend_gl.cpp BindTexture) at their worst, from the switches it reads once. rungWorstSX is the narrowest rung's width over 640
// (the ladder's emergency size is 480 wide = 0.75; 0.5 leaves room).
struct Cfg {
    long rungPct = 0, rungExtra = 0;   // GXV_RUNG_LODBIAS, the larger of GXV_RUNG_LODBIAS_EXTRA and _EXTRA_AF
    long playerHundredths = 0;         // GXV_PLAYER_LODBIAS
    int automipMode = 0, automipBias = 0; // GXV_AUTOMIP (0 off), GXV_AUTOMIP_BIAS in eighths
};
constexpr float kWorstRungSX = 0.5f;

inline long RungWorstLow(const Cfg& c) { return lodbias::RungSteps(kWorstRungSX, c.rungPct, c.rungExtra); } // (-31 .. 0)
// The sum's lowest and highest value over every rung, the player's textures or not, automip's textures or not (the terms that are zero or one-sided are
// left out of the bound they do not touch).
inline long WorstLow(long steps, const Cfg& c) {
    long lo = steps + RungWorstLow(c);
    const long pl = lodbias::PlayerSteps(c.playerHundredths);
    if (pl < 0) lo += pl;
    if (c.automipMode != 0 && c.automipBias < 0) lo += c.automipBias;
    return lo;
}
inline long WorstHigh(long steps, const Cfg& c) {
    long hi = steps; // (the rung's term is never above 0)
    const long pl = lodbias::PlayerSteps(c.playerHundredths);
    if (pl > 0) hi += pl;
    if (c.automipMode != 0 && c.automipBias > 0) hi += c.automipBias;
    return hi;
}
inline bool RangeOk(long steps, const Cfg& c) { return WorstLow(steps, c) >= lodbias::kMinSteps && WorstHigh(steps, c) <= lodbias::kMaxSteps; }

// A GX bias in 1/32 levels (the signed byte of mode0 bits 9..16 when the unit mip-maps, else 0: vgx_fixes.h LodBiasOf x 32) as eighths, or false.
inline bool ToEighths(int bias32, int& steps) {
    if (bias32 % 4 != 0) return false;
    steps = bias32 / 4;
    return true;
}

// One draw's units. bias32[m]: the unit's bias in 1/32 levels (0: none or the unit does not mip-map). stageMask: bit m = a stage reads texmap m;
// indMask: bit m = an indirect stage reads texmap m. Result for unit m: lodWhy, and steps (eighths) when it is kMoved. A unit with no bias is kNone.
struct Unit { uint8_t why = kNone; int8_t steps = 0; };
inline Unit Classify(unsigned m, const int bias32[8], uint8_t stageMask, uint8_t indMask, const Cfg& cfg) {
    Unit u;
    if (!(stageMask & (1u << m)) || bias32[m] == 0) return u;
    int steps = 0;
    if (!ToEighths(bias32[m], steps)) { u.why = kNotEighth; return u; }
    if (!RangeOk(steps, cfg)) { u.why = kRange; return u; }
    if (indMask) { u.why = kIndirect; return u; }
    for (unsigned n = 0; n < 8; ++n)
        if ((stageMask & (1u << n)) && bias32[n] != bias32[m]) { u.why = kMixed; return u; }
    u.why = kMoved;
    u.steps = static_cast<int8_t>(steps);
    return u;
}

// The back end's sum, as BindTexture makes it, and whether it stayed in range without a clamp: the check that a moved bias reached the sampler exactly.
inline int8_t Sum(int8_t base, int8_t steps, bool& exact) {
    const long raw = static_cast<long>(base) + steps;
    exact = raw >= lodbias::kMinSteps && raw <= lodbias::kMaxSteps;
    return static_cast<int8_t>(lodbias::ClampSteps(raw));
}

// The key hash's share of the units: the bytes of the 8 biases (0 for no bias), so that any change of a bias value (which can change a decision above)
// changes the hash. Packed as the two halves of a 64-bit word.
inline uint64_t PackBiases(const int bias32[8]) {
    uint64_t d = 0;
    for (unsigned m = 0; m < 8; ++m) d |= static_cast<uint64_t>(static_cast<uint8_t>(static_cast<int8_t>(bias32[m]))) << (8 * m);
    return d;
}

} // namespace lodsampler
} // namespace vgx
