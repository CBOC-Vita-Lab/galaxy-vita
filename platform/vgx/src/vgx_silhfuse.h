// GXV_SILH_FUSE (silhfuse26, 12 Nov 2026): the pure parts. No GL, no front-end state: vgx_draw.cpp SilhFusePass hands it the fill's render state and the capture's
// neighbours, vgx_shadergen.cpp writes the shader text from the same code byte, vgx_backend_gl.cpp defers and draws; tests/vgx_silhfuse_test.cpp checks it.
//
// What is fused. Every game picture with Mario on screen has this pair in the EFB's second scene (notes: new/silhfuse26/REPORT.txt):
//   1. the hand-over's draw-back quad (EfbSwap, GXV_EFBSWAP2): the old EFB colour buffer, which now holds the alpha capture (ScreenAlphaCapture), is copied whole into the
//      new buffer, nearest, blend off, all four channels written;
//   2. MR::fillSilhouetteColor's fill quad: one full-screen quad that samples that same texture, tests TEXA > K, and where it passes blends black at alpha 0x80 over the pixel
//      (SRC_ALPHA / INV_SRC_ALPHA, alpha channel written 0).
// Both read the same texel of the same texture at the same pixel (the fill's texture coordinate is the pixel's place in the whole EFB, SfeFillGeom), and the second reads the
// first one's output only through the blender. One fragment shader can therefore write the second pass's result straight into the new buffer: a pixel that fails the fill's
// test gets the texel (what pass 1 wrote); a pixel that passes gets the blend's result worked out from the texel (the destination pass 1 would have written) and the fill's
// own source colour and alpha. One full-screen pass less, same pixels.
//
// Exactness. The blender is not ours (the GPU's), so the fusion is allowed only for pairs whose result does not depend on how the GPU orders its roundings. With the colour
// bytes s (source, the shader's output quantised to 8 bits), d (destination) and the alpha byte a, a blend term is X * F / 255 with F one of 0, 255, a, 255 - a, s, 255 - s.
//   - A term is EXACT (an integer, no rounding) when F is 0 or 255, or when its X is 0 (the source colour is proven 0 for every fragment of the draw: the game's fill is
//     black, its TEV colour is register C0 = 0).
//   - A term that is not exact has x = X * F with 0 <= x <= 65025 an integer and the term is x / 255, whose fractional part is a multiple of 1/255: never 1/2 (255 is odd), so
//     "round to nearest" is unambiguous: floor((x + 127) / 255).
//   - The result of ADD is the sum of the two terms (REVSUB: dst term - source term), clamped to 0..255. With AT MOST ONE non-exact term the sum of an integer and one rounded
//     term is the same whether the GPU rounds each term and adds them or adds and rounds: the fusion is exact against every round-to-nearest blender. With two non-exact terms
//     those two orders can differ by one level: such a pair is NEVER fused (RJ_TWO_ROUNDINGS, counted).
// What stays unproven is only whether the console's blender rounds to nearest at all; GXV_SILH_FUSE=2 measures it (mean / max difference of the finished pictures).
//
// The shader writes (r + 0.25) / 255 for the result byte r: a converter that rounds to nearest and one that truncates both give r (the margin is 0.25 of a level, far above
// float and half-precision error at 0..1), so the output stage's own rounding cannot move a byte.
#pragma once
#include <cstdint>

namespace vgx {
namespace silhfuse {

// Blend factors by the front end's BF_* numbers (vgx_cmd.h): ZERO 0, ONE 1, SRC_COLOR 2, INV_SRC_COLOR 3, SRC_ALPHA 4, INV_SRC_ALPHA 5, DST_ALPHA 6, INV_DST_ALPHA 7, DST_COLOR 8,
// INV_DST_COLOR 9. A fusable factor is one of the first six; a "kind" is the same list renumbered 0..5 (K_*).
enum Kind : uint8_t { K_ZERO = 0, K_ONE = 1, K_SA = 2, K_ISA = 3, K_SC = 4, K_ISC = 5, K_COUNT = 6 };
// Alpha channel of the result: how the blend left it. A_KEEP (alpha not written): the texel's; A_ZERO (alpha factors ZERO / ZERO, RenderState::dstAlphaZero 1): 0;
// A_SRC (alpha factors ONE / ZERO, dstAlphaZero 2): the fill's own alpha byte.
enum AlphaMode : uint8_t { A_KEEP = 0, A_ZERO = 1, A_SRC = 2 };

enum Reason : int {
    RJ_OK = 0,
    RJ_NOBLEND,         // the fill is not blended (nothing to compose)
    RJ_NOCOLOUR,        // colour not written
    RJ_DSTFACTOR,       // a factor reads the destination (DST_ALPHA, DST_COLOR, ...): not a function of the texel and the fill alone
    RJ_ALPHAMODE,       // alpha blended with the colour's factors (dstAlphaZero 0 with alpha written): not one of the three modes
    RJ_EQUATION,        // an equation other than ADD / REVSUB
    RJ_TWO_ROUNDINGS,   // two terms that are not exact: the GPU's order of roundings would decide
    RJ_COUNT
};
inline const char* ReasonName(int r) {
    static const char* const n[RJ_COUNT] = {"fused", "not blended", "colour not written", "destination factor", "alpha mode", "equation", "two roundings"};
    return r >= 0 && r < RJ_COUNT ? n[r] : "?";
}

// The pair as the fusion sees it.
struct Pair {
    uint8_t blend = 0, eq = 0 /* BE_ADD 0, BE_REVSUB 1 */, sf = 1, df = 0;
    uint8_t colorWrite = 1, alphaWrite = 1, dstAlphaZero = 0;
    bool srcZero = false; // the fill's source colour is exactly 0 for every fragment (front end: ColourZero)
};

// The code byte (FsKey::silhFuse): bit 7 set (the key is a fused key), bit 6 REVSUB, bits 3..5 the destination factor's kind, bits 0..2 the source factor's kind. The alpha mode is
// FsKey's second byte (silhFuseAlpha). 0 is "not fused".
inline uint8_t Code(bool revsub, uint8_t sKind, uint8_t dKind) { return static_cast<uint8_t>(0x80u | (revsub ? 0x40u : 0u) | ((dKind & 7u) << 3) | (sKind & 7u)); }
inline bool CodeRev(uint8_t c) { return (c & 0x40u) != 0; }
inline uint8_t CodeS(uint8_t c) { return static_cast<uint8_t>(c & 7u); }
inline uint8_t CodeD(uint8_t c) { return static_cast<uint8_t>((c >> 3) & 7u); }
inline bool CodeOn(uint8_t c) { return (c & 0x80u) != 0; }

inline int KindOf(uint8_t bf) { // -1: not fusable
    switch (bf) {
    case 0: return K_ZERO;
    case 1: return K_ONE;
    case 2: return K_SC;
    case 3: return K_ISC;
    case 4: return K_SA;
    case 5: return K_ISA;
    default: return -1;
    }
}
inline bool KindExact(uint8_t k) { return k == K_ZERO || k == K_ONE; }

// Decides one pair. Returns RJ_OK with the code and the alpha mode, else the reason. A source factor of an all-zero source colour is replaced by K_ZERO (the term is exactly 0
// whatever the factor, and the shader then does not even read the source colour).
inline int Classify(const Pair& p, uint8_t& code, uint8_t& alphaMode) {
    code = 0;
    alphaMode = 0;
    if (!p.blend) return RJ_NOBLEND;
    if (!p.colorWrite) return RJ_NOCOLOUR;
    if (p.eq > 1) return RJ_EQUATION;
    const int sk = KindOf(p.sf), dk = KindOf(p.df);
    if (sk < 0 || dk < 0) return RJ_DSTFACTOR;
    if (p.alphaWrite) {
        if (p.dstAlphaZero == 1) alphaMode = A_ZERO;
        else if (p.dstAlphaZero == 2) alphaMode = A_SRC;
        else return RJ_ALPHAMODE;
    } else {
        alphaMode = A_KEEP;
    }
    const uint8_t sEff = p.srcZero ? static_cast<uint8_t>(K_ZERO) : static_cast<uint8_t>(sk);
    // A term is exact when its factor is 0 or 255 (or, for the source, when the source is 0, folded into sEff). A destination term with factor ZERO is 0, with ONE the texel.
    const int rounded = (KindExact(sEff) ? 0 : 1) + (KindExact(static_cast<uint8_t>(dk)) ? 0 : 1);
    if (rounded > 1) return RJ_TWO_ROUNDINGS;
    code = Code(p.eq == 1, sEff, static_cast<uint8_t>(dk));
    return RJ_OK;
}

// ---- the arithmetic, in integers: the reference ("the blend the GPU computes, rounded to nearest, term by term") and the model of the shader's float steps.
inline int FactorOf(uint8_t kind, int s, int a) { // the factor byte for the colour channel whose source byte is s, alpha byte a
    switch (kind) {
    case K_ZERO: return 0;
    case K_ONE: return 255;
    case K_SA: return a;
    case K_ISA: return 255 - a;
    case K_SC: return s;
    default: return 255 - s;
    }
}
inline int RoundDiv255(int x) { return (x + 127) / 255; } // nearest; x >= 0, never a tie
// d: destination byte (the texel), s: the fill's source colour byte (that channel), a: the fill's alpha byte. The GL result for one channel.
inline int BlendRef(uint8_t code, int s, int d, int a) {
    const int ts = RoundDiv255(s * FactorOf(CodeS(code), s, a));
    const int td = RoundDiv255(d * FactorOf(CodeD(code), s, a));
    int r = CodeRev(code) ? td - ts : ts + td;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return r;
}
// The shader's steps in single precision, line for line what vgx_shadergen.cpp writes (FusedTerm): a term is 0 for ZERO, the byte itself for ONE, else
// floor((x + 127) * (1/255) + 0.001) with x = byte * factor.
inline float ShaderTerm(uint8_t kind, float x, float s, float a) {
    if (kind == K_ZERO) return 0.0f;
    if (kind == K_ONE) return x;
    float f = 0.0f;
    switch (kind) {
    case K_SA: f = a; break;
    case K_ISA: f = 255.0f - a; break;
    case K_SC: f = s; break;
    default: f = 255.0f - s; break;
    }
    const float v = (x * f + 127.0f) * (1.0f / 255.0f) + 0.001f;
    return static_cast<float>(static_cast<int>(v)); // floor: v >= 0
}
inline float ShaderChannel(uint8_t code, float s, float d, float a) {
    const float ts = CodeS(code) == K_ZERO ? 0.0f : ShaderTerm(CodeS(code), s, s, a);
    const float td = ShaderTerm(CodeD(code), d, s, a);
    float r = CodeRev(code) ? td - ts : ts + td;
    if (r < 0.0f) r = 0.0f;
    if (r > 255.0f) r = 255.0f;
    return r;
}
// The byte the output stage makes of the value the shader writes for result byte r, for a converter that rounds to nearest (mode 0) and one that truncates (mode 1).
inline int OutputByte(float r, int mode) {
    float v = (r + 0.25f) * (1.0f / 255.0f);
    if (v > 1.0f) v = 1.0f;
    const float x = v * 255.0f;
    return mode == 0 ? static_cast<int>(x + 0.5f) : static_cast<int>(x);
}

// ---- the pairing of a capture with its fill, over a small view of the command list (the front end fills the view from CmdBuf).
enum CmdClass : uint8_t { C_HARMLESS = 0, C_DRAW, C_OTHER };
// Index of the first command after `from` that is not harmless (shader / program / variant / buffer upload / a texture upload of another texture), or n. The fill must be that
// command, and a draw.
template <class Class>
inline long NextDraw(long from, long n, Class classOf) {
    for (long j = from + 1; j < n; ++j) {
        const uint8_t c = classOf(j);
        if (c == C_HARMLESS) continue;
        return c == C_DRAW ? j : -1;
    }
    return -1;
}

// ---- the census a front end keeps (per picture counts folded into a line)
struct Census {
    uint64_t pictures = 0, captures = 0, fused = 0;
    enum Why : int { W_MODE = RJ_COUNT, W_SWAP, W_COPYSHAPE, W_NOFILL, W_FILLSHAPE, W_GEOM, W_STATE, W_KEY, W_FETCH, W_SRCNOTZERO, W_READER, W_COUNT };
    static const char* WhyName(int w) {
        static const char* const n[W_COUNT] = {"fused", "not blended", "colour not written", "destination factor", "alpha mode", "equation", "two roundings", "(mode)",
                                               "no swap", "capture shape", "no fill next", "fill shape", "fill bounded / not the whole EFB", "fill state", "fill key",
                                               "SHADOWFETCH owns it", "source colour unknown", "other reader"};
        return w >= 0 && w < W_COUNT ? n[w] : "?";
    }
    uint64_t why[W_COUNT] = {};
    void Refuse(int w) { if (w >= 0 && w < W_COUNT) ++why[w]; }
};

} // namespace silhfuse
} // namespace vgx
