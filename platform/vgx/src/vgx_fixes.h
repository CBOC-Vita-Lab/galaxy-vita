// GX -> GLES2 translation repairs that are pure functions of GX register fields (no GL, no state).
// Each is used behind its own read-once environment switch (default OFF unless noted at the reader).
// Kept in a header so that the host pixel tests (tests/vgx_tevfix_gl.cpp) run exactly the code the
// front end runs.
#pragma once
#include "vgx_cmd.h"

namespace vgx {
namespace fixes {

// ------------------------------------------------------------------ GX logic ops (BP 0x41 bits 12..15)
// GLES2 has no logic op. GX_LO_* are bitwise on 8-bit channels; for a channel that holds 0 or 255 a
// blend of the shader output s and the framebuffer d reproduces them (AND = s*d, OR = s + d - s*d, ...),
// and for values in between the same expressions are the usual fuzzy extension.
//   src: 0 the shader output as it is, 1 the output inverted (1 - s), 2 the output replaced by white
//   sf / df: blend factors for source and destination (BF_* of vgx_cmd.h), equation ADD
//   exact: reproduces the op for every pair of 0/1 channel values (NAND and REVOR need a constant
//          term that one blend cannot add; they use the nearest single blend and say so)
struct LogicPlan {
    uint8_t src, sf, df;
    bool exact;
};
inline LogicPlan LogicOpPlan(unsigned op) {
    switch (op & 15) {
    case 0:  return {0, BF_ZERO, BF_ZERO, true};                 // CLEAR
    case 1:  return {0, BF_DST_COLOR, BF_ZERO, true};            // AND      s & d
    case 2:  return {0, BF_INV_DST_COLOR, BF_ZERO, true};        // REVAND   s & ~d
    case 3:  return {0, BF_ONE, BF_ZERO, true};                  // COPY
    case 4:  return {0, BF_ZERO, BF_INV_SRC_COLOR, true};        // INVAND   ~s & d
    case 5:  return {0, BF_ZERO, BF_ONE, true};                  // NOOP
    case 6:  return {0, BF_INV_DST_COLOR, BF_INV_SRC_COLOR, true}; // XOR
    case 7:  return {0, BF_INV_DST_COLOR, BF_ONE, true};         // OR
    case 8:  return {1, BF_INV_DST_COLOR, BF_ZERO, true};        // NOR      ~s & ~d  = s' & ~d
    case 9:  return {1, BF_INV_DST_COLOR, BF_INV_SRC_COLOR, true}; // EQUIV  ~(s ^ d) = s' ^ d
    case 10: return {2, BF_INV_DST_COLOR, BF_ZERO, true};        // INV      ~d
    case 11: return {0, BF_ONE, BF_ZERO, false};                 // REVOR    s | ~d (nearest: s)
    case 12: return {1, BF_ONE, BF_ZERO, true};                  // INVCOPY  ~s
    case 13: return {1, BF_ONE, BF_INV_SRC_COLOR, true};         // INVOR    ~s | d  = s' | d
    case 14: return {2, BF_INV_DST_COLOR, BF_ZERO, false};       // NAND     ~(s & d) (nearest: ~d)
    default: return {2, BF_ONE, BF_ONE, true};                   // SET      all ones
    }
}
// The table BuildRenderState has always used (Dolphin's table for back ends without logic ops).
inline void LegacyLogicOpFactors(unsigned op, uint8_t& sf, uint8_t& df) {
    static const uint8_t lo[16][2] = {
        {BF_ZERO, BF_ZERO}, {BF_DST_COLOR, BF_ZERO}, {BF_ONE, BF_INV_SRC_COLOR}, {BF_ONE, BF_ZERO},
        {BF_DST_COLOR, BF_ONE}, {BF_ZERO, BF_ONE}, {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_INV_DST_COLOR, BF_ONE},
        {BF_INV_SRC_COLOR, BF_INV_DST_COLOR}, {BF_INV_SRC_COLOR, BF_SRC_COLOR}, {BF_INV_DST_COLOR, BF_INV_DST_COLOR},
        {BF_ONE, BF_INV_DST_ALPHA}, {BF_INV_SRC_COLOR, BF_INV_SRC_COLOR}, {BF_INV_SRC_COLOR, BF_ONE},
        {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_ONE, BF_ONE}};
    sf = lo[op & 15][0];
    df = lo[op & 15][1];
}

// ------------------------------------------------------------------ GX destination alpha (BP 0x42)
// GXSetDstAlpha(TRUE, a): while alpha is updated, the framebuffer alpha becomes the constant a, whatever
// the TEV alpha and the blend factors are (blend factors still read the old destination alpha for the colour).
// Galaxy uses it as a mask plane: VolumeModelDrawer, MarioSearchLight, MarioActorSpecialDraw, TDDraw, with BM_BLEND and
// BM_SUBTRACT, constants 0 and 255.
// A GL blend cannot replace alpha by a constant, so:
//   fsConst  the fragment shader replaces its alpha by the constant (FsKey::dstAlpha)
//   rsMode   0 blend alpha as the colour factors say, 1 alpha factors ZERO/ZERO (writes 0), 2 alpha factors ONE/ZERO
//            (writes the shader's alpha, which is the constant)
//   dropBlend  the draw writes no colour, so its colour blend is moot: blending is switched off
//   unresolved  the colour factors read the TEV alpha (SRC_ALPHA / INV_SRC_ALPHA) while colour is written and the
//            constant is not 0: needs a second, alpha-only pass; nothing is changed
struct DstAlphaPlan {
    bool fsConst = false;
    uint8_t rsMode = 0;
    bool dropBlend = false;
    bool unresolved = false;
};
// dstOn: BP 0x42 bit 8; constA: BP 0x42 bits 0..7; srcF / dstF: colour factors as BF_* (after the no-alpha-target mapping);
// blendBit / subtract: BP 0x41 bits 0 and 11.
inline DstAlphaPlan PlanDstAlpha(bool dstOn, unsigned constA, bool alphaWrite, bool colorWrite, bool blendBit, bool subtract, uint8_t srcF, uint8_t dstF) {
    DstAlphaPlan p;
    if (!dstOn || !alphaWrite) return p;
    const bool glBlend = subtract || (blendBit && !(srcF == BF_ONE && dstF == BF_ZERO));
    if (!glBlend) { p.fsConst = true; return p; }
    if (!colorWrite) { p.fsConst = true; p.dropBlend = true; return p; }
    if (constA == 0) { p.rsMode = 1; return p; }
    const bool readsSrcAlpha = !subtract && (srcF == BF_SRC_ALPHA || srcF == BF_INV_SRC_ALPHA || dstF == BF_SRC_ALPHA || dstF == BF_INV_SRC_ALPHA);
    if (readsSrcAlpha) { p.unresolved = true; return p; }
    p.fsConst = true;
    p.rsMode = 2;
    return p;
}

// ------------------------------------------------------------------ texture LOD bias (BP 0x80.. mode0, 0x84.. mode1)
// A GX texture unit adds a signed bias (mode0 bits 9..16, an 8-bit two's complement number in 1/32 steps, -4 .. +3.97) to the level of
// detail it computes, then clamps it to the unit's range. GLES2 has no per-texture bias; a fragment shader can give one to texture2D.
// Returns the bias a stage must give when the unit mip-maps (mip filter in mode0 bits 5..6 and a maximum LOD of at least 1, the same test
// BpTexSlotSync makes), and 0 when it does not (the bias then changes nothing).
inline float LodBiasOf(uint32_t mode0, uint32_t mode1) {
    const uint32_t hwMin = (mode0 >> 5) & 7;
    const float maxLod = static_cast<float>((mode1 >> 8) & 0xFF) / 16.0f;
    if ((hwMin & 3) == 0 || maxLod < 1.0f) return 0.0f;
    return static_cast<float>(static_cast<int8_t>((mode0 >> 9) & 0xFF)) / 32.0f;
}
// The mode0 / mode1 registers of texmap m.
inline unsigned TexMode0Reg(unsigned m) { return m < 4 ? 0x80 + m : 0xA0 + (m - 4); }
inline unsigned TexMode1Reg(unsigned m) { return TexMode0Reg(m) + 4; }

// ------------------------------------------------------------------ GX lines and points (BP 0x22 SU_LPSIZE, BP 0x30 + 2n SU_SSIZEn)
// GXV_FIX_LINES: GX draws a line as a parallelogram widened along the minor axis of its screen-space direction (an x-major line vertically,
// a y-major one horizontally, a tie x-major) by width/6 EFB pixels in total, and a point as an axis-aligned square of size/6 pixels. vitaGL
// ignores glLineWidth for custom shaders and the generated vertex shader writes no point size, so the front end widens them into quads.
// The first free run of `need` consecutive texture-coordinate slots (bit n of `used` = slot n is read by the layout or by a texgen) for the
// expanded vertices' extra attributes, or -1.
inline int LinePointFreeSlots(unsigned used, int need) {
    for (int b = 0; b + need <= 8; ++b) {
        const unsigned m = ((1u << need) - 1u) << b;
        if (!(used & m)) return b;
    }
    return -1;
}
// SU_LPSIZE fields. size: line (bits 0..7) or point (bits 8..15) size in 1/6 pixel, 0 = nothing to expand; offset code: GX_TO_* (3 bits at
// 16 for lines, 19 for points), 6 and 7 act as 5 (Dolphin's table {0, 1/16, 1/8, 1/4, 1/2, 1, 1, 1} of the texture coordinate offset).
inline unsigned LinePointSize6(unsigned lpsize, bool point) { return point ? (lpsize >> 8) & 0xFF : lpsize & 0xFF; }
inline unsigned LinePointTexCode(unsigned lpsize, bool point) {
    const unsigned c = (lpsize >> (point ? 19 : 16)) & 7u;
    return c > 5 ? 5 : c;
}
// SU_SSIZEn: GXEnableTexOffsets(coord, line, point) bits 18 and 19.
inline bool LinePointTexEnabled(unsigned suSsize, bool point) { return ((suSsize >> (point ? 19 : 18)) & 1u) != 0; }

} // namespace fixes
} // namespace vgx
