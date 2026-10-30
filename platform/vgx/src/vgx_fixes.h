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

} // namespace fixes
} // namespace vgx
