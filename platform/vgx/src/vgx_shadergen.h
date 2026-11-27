// Structural shader keys and GLSL ES 1.00 generation. Front-end side: no GL.
#pragma once
#include "vgx_cmd.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgx {

// Everything that changes the *code* of a shader goes into a key; everything
// that is only a value (colours, matrices, references, fog parameters) is a
// uniform. Keys are plain zero-initialised structs compared with memcmp.
struct FsKey {
    uint8_t numStages = 1;    // 1..16
    uint8_t numTexGens = 0;   // texcoord varyings declared
    uint8_t texProjMask = 0;  // texcoord k is projective (divide by q)
    uint8_t alphaTestOn = 0;
    uint8_t alphaTest = 0;    // comp0 | comp1 << 3 | op << 6
    uint8_t fog = 0;          // fsel | ortho << 3 | kFogVtxBit (0 = off); kFogVtxBit: the factor comes from the vertex shader's v_fog
    uint8_t dstAlpha = 0;     // write the constant destination alpha
    uint8_t numIndStages = 0;
    uint8_t ztex = 0;         // op << 2 | fmt (0 = off)
    // TEV registers 0 and 1 start from the vertex colours instead of u_tev[1] / u_tev[2]
    // (bit 0, bit 1). Set for small draws that do not use the rasterised colours at all:
    // the front end writes the register values into the vertices, so that draws which
    // differ only in them (particles fading out) merge into one. See FoldRegs in vgx_draw.cpp.
    uint8_t regFromVtx = 0;
    // GX logic op emulated by blending (vgx_fixes.h LogicOpPlan): 0 the colour as it is, 1 inverted, 2 white. Set only under GXV_FIX_LOGICOP.
    uint8_t logicSrc = 0;
    // GX reads the TEV inputs A, B and C as the low 8 bits of the (11-bit signed) register value; vgx keeps registers as normalised floats and
    // would clamp an out-of-range one (a J3D material colour of 270 reads as 14). Set only under GXV_FIX_TEVIN, and only while a TEV
    // register is outside 0..255: register reads feeding A, B and C go through vgx_w3 / vgx_w1.
    // Bit 0 (value 1) is that wrap. Bit 1 (value 2, set only under GXV_FS_HALF=1 or 3, which then also track the registers) is "a TEV register is
    // outside 0..255" on its own: no wrap code, but the shader is not made half precision (FsHalfAnalyse assumes registers in 0..1).
    uint8_t inWrap = 0;
    struct Stage {
        uint32_t color = 0; // BP 0xC0+2n bits 0..23
        uint32_t alpha = 0; // BP 0xC1+2n bits 4..23 (swap selects stripped)
        uint32_t ind = 0;   // BP 0x10+n (0 when the stage is direct)
        uint8_t texEnable = 0, texMap = 0, texCoord = 0, rasChan = 7; // 0, 1, 5/6 (bump alpha), 7 = zero
        uint8_t kcsel = 0, kasel = 0;
        uint8_t rasSwap = 0xE4, texSwap = 0xE4; // 2 bits per output component, r in bits 0..1
        // The stage reads the texture colour but its texture is switched off (BP 0x28.. enable bit clear): GX gives white. Set only under GXV_FIX_TEXOFF.
        uint8_t texOff = 0;
        // The stage samples its texture with the texture unit's LOD bias (GX: BP 0x80.. bits 9..16, 1/32 steps): texture2D(..., bias) with the
        // bias from the TEV block (V_LODBIAS, one float per texmap). Set only under GXV_FIX_LODBIAS and only for a mip-mapped, non-zero bias.
        uint8_t lodBias = 0;
        uint8_t pad[2] = {};
    } stage[16];
    struct Ind {
        // pad[0] (GXV_IND_VS, IndVsPair): 1 = the vertex shader writes this lookup's coordinate already scaled (v_texN, a vec2, read by
        // nothing else): the lookup is texture2D(u_texM, v_texN), a non-dependent read. 0 in every key made with the switch off.
        uint8_t texMap = 0, texCoord = 0, pad[2] = {};
    } ind[4];
    uint8_t indVs(int i) const { return ind[i].pad[0]; }
    // GXV_IND_PRECOMP (IndPreMark; ind[3].pad[1]): 1 + the stage whose indirect coordinate is written as uv + M.iv * (1 / tsz) + c, with
    // (1 / tsz, c) per draw in u_tev[V_INDPRE] (IndPreValues). 0 in every key made with the switch off (the text as always).
    uint8_t indPre() const { return ind[3].pad[1]; }
    void setIndPre(uint8_t s) { ind[3].pad[1] = s; }
    // GXV_TEXVEC2 (TexVec2Pair): the texture coordinate varyings of the non-projective texgens are vec2 (v_texN.xy is all such a
    // texgen's coordinate uses); kept in the padding of stage[0], 0 in every key made with the switch off (the text as always).
    uint8_t texVec2() const { return stage[0].pad[0]; }
    void setTexVec2(uint8_t on) { stage[0].pad[0] = on; }
    // GXV_DISCARD_BLEND (vgx_draw.cpp RejectZeroFs): a fragment that fails the alpha test writes vec4(0) instead of being killed. Set
    // only by the front end, only for a draw whose blend makes a zero source a no-op (RejectIsNoop); kept in the padding of stage[0],
    // 0 in every key made with the switch off (the text as always).
    uint8_t rejectZero() const { return stage[0].pad[1]; }
    void setRejectZero(uint8_t on) { stage[0].pad[1] = on; }
    // GXV_TEVIN_CANON=2 (shader23; only in a key with inWrap bit 0): bit r (r, g, b) / bit r + 4 (a) = that part of register r's initial
    // value is read only as A / B / C (never as D), so the front end uploads it already wrapped to its low 8 bits (BuildTevC: (v & 255) /
    // 255) and the text reads it without vgx_w3 / vgx_w1. Kept in the padding of stage[1]; 0 in every key made with the switch off or 1.
    uint8_t preWrap() const { return stage[1].pad[0]; }
    void setPreWrap(uint8_t m) { stage[1].pad[0] = m; }
    // GXV_TEVIN_PREWRAP=1 (pixcuts25; only in a key with inWrap bit 0, under GXV_TEVIN_CANON=2): bit r = register r (0 PREV .. 3 REG2) also has its low byte wrapped
    // (v & 255) / 255 in a row of the TEV block of its own, rows FsTevBase(k) + r (FsTevCount grows by 4), and the A / B / C reads of that register's INITIAL value
    // that keep their wrap (the register is also read as D, which is why GXV_TEVIN_CANON=2's preWrap cannot take it) read that row, `wprev` / `wc0` / `wc1` / `wc2`,
    // instead of calling vgx_w3 / vgx_w1 on the register for every pixel. Kept in the padding of stage[2]; 0 in every key made with the switch off: no shader
    // text, key or hash of an old pack entry changes.
    uint8_t dualWrap() const { return stage[2].pad[1]; }
    void setDualWrap(uint8_t m) { stage[2].pad[1] = m; }
    // GXV_FS_ADEAD (shader23 round 2; vgx_draw.cpp AlphaDeadFs): the draw's render state makes the shader's output alpha unused (no alpha
    // write, no source-alpha blend factor) and the key has no alpha test: the text writes alpha 0 and the compiler drops whatever of the
    // alpha pipeline the colour does not read. Kept in the padding of stage[1]; 0 in every key made with the switch off.
    uint8_t alphaDead() const { return stage[1].pad[1]; }
    void setAlphaDead(uint8_t on) { stage[1].pad[1] = on; }
    // GXV_PIX_ATPASS (pix23; vgx_draw.cpp AtPassFs): the key keeps its alpha test (alphaTestOn / alphaTest, so that the precision,
    // the trims and the final clamps are decided exactly as for the tested key) but the text leaves the test and its `discard` out: the
    // front end gives it only to draws whose alpha test it has proven to pass for every fragment (FsValueEval). Kept in the padding of
    // stage[2]; 0 in every key made with the switch off (the text as always).
    uint8_t atPass() const { return stage[2].pad[0]; }
    void setAtPass(uint8_t on) { stage[2].pad[0] = on; }
    // GXV_ALPHATEST_PREPASS (dread25; vgx_draw.cpp PrepassFs): the text is the tested key's text up to and including its alpha test, then
    // `gl_FragColor = vec4(0.0)`: the depth-only first pass of an alpha-tested draw (the back end draws it with the colour mask off). Kept in the
    // padding of stage[3] (nothing else uses it); 0 in every key made with the switch off (the text as always).
    uint8_t preAlpha() const { return stage[3].pad[0]; }
    void setPreAlpha(uint8_t on) { stage[3].pad[0] = on; }
    // GXV_SILH_FUSE (silhfuse26; vgx_silhfuse.h, vgx_draw.cpp SilhFusePass): the key of the silhouette fill quad fused with the hand-over's draw-back quad. The text is the fill's own
    // up to its alpha test (which no longer discards), then writes the BLEND's result worked out from the texel it sampled (the destination the draw-back would have written) and
    // its own source colour and alpha, and for a fragment that fails the test the texel itself: the render state is then unblended. silhFuse() is vgx::silhfuse::Code (bit 7 set;
    // 0: an ordinary key), silhFuseAlpha() the AlphaMode. Kept in the padding of stage[3] and stage[4]; 0 in every key made with the switch off (the text as always).
    uint8_t silhFuse() const { return stage[3].pad[1]; }
    uint8_t silhFuseAlpha() const { return stage[4].pad[0]; }
    void setSilhFuse(uint8_t code, uint8_t alphaMode) { stage[3].pad[1] = code; stage[4].pad[0] = alphaMode; }
    // GXV_FS_NOCOLOR (fs24; vgx_draw.cpp ColorDeadFs): the draw's render state writes no colour (colour mask off) and no blend factor reads the source colour:
    // the text writes colour 0 and the compiler drops whatever of the colour pipeline (and the fog) the alpha does not read. The alpha pipeline and the alpha test
    // do not read colour (GX's TEV keeps the two apart), so every alpha the draw writes or tests is the same number. Kept in the padding of stage[4]; 0 in every key
    // made with the switch off (the text as always).
    uint8_t colorDead() const { return stage[4].pad[1]; }
    void setColorDead(uint8_t on) { stage[4].pad[1] = on; }
};

struct VsKey {
    uint8_t numTexGens = 0;
    uint8_t numChans = 0;   // 0..2 lit colour channels produced
    uint8_t perVtxPos = 0;  // position/normal matrix row comes from a_pos.w (2: no row above 27 is used, the palette is the short one;
                            // 3: none above 9, the small palette of 4 matrices: GXV_PALROWS)
    // (was padding: 0 unless GXV_FOGVTX is on) bit 0: write the varying v_fog for the fragment shader's vertex fog
    // (FsKey::fog & kFogVtxBit), bit 1: from clip z (orthographic fog) instead of clip w (perspective fog)
    uint8_t fogVtx = 0;
    // Per channel control: [0] colour0, [1] colour1, [2] alpha0, [3] alpha1.
    struct Chan {
        uint8_t enable = 0, matSrcVtx = 0, ambSrcVtx = 0;
        uint8_t diffuse = 0; // 0 none, 1 sign, 2 clamp
        uint8_t attn = 0;    // 0 none, 1 spec, 2 spot
        uint8_t lightMask = 0;
        uint8_t pad[2] = {};
    } chan[4];
    struct TexGen {
        uint8_t type = 0;      // 0 regular, 1 emboss, 2 colour0, 3 colour1
        uint8_t src = 5;       // XF source row: 0 pos, 1 nrm, 2 colours, 3 binrm, 4 tangent, 5.. tex0..
        uint8_t proj = 0;      // 1 = STQ (3x4 matrix, projective)
        uint8_t inputAbc1 = 0; // input form (a,b,c,1) instead of (a,b,1,1)
        uint8_t post = 0;      // 0 none, 1 post matrix, 2 normalise + post matrix
        uint8_t perVtxMtx = 0; // matrix row from the per-vertex TEXnMTXIDX
        uint8_t pad[2] = {};
    } tg[8];
    // GXV_FIX_LINES: the vertex shader widens a GX line (lpMode 1) or point (lpMode 2) into a quad, as GX rasterises it. The expanded
    // vertices carry, in texture coordinate slots lpBase.. that nothing else reads (3 slots for a line: the other end point's a_pos
    // as two vec2, then the corner; 1 for a point: the corner), the corner's NDC offset (+-width / viewport size per axis, a quad
    // corner or the side of the line). lpAmount is the GX_TO_* code of GXSetLineWidth / GXSetPointSize (texture coordinate offsets,
    // 0 = none) and lpTexMask the texgens whose GXEnableTexOffsets bit is set. Kept in the padding of chan[0] and chan[1], so
    // sizeof(VsKey) and every key with lpMode 0 are unchanged.
    uint8_t lpMode() const { return chan[0].pad[0]; }
    uint8_t lpBase() const { return chan[0].pad[1]; }
    uint8_t lpAmount() const { return chan[1].pad[0]; }
    uint8_t lpTexMask() const { return chan[1].pad[1]; }
    void setLp(uint8_t mode, uint8_t base, uint8_t amount, uint8_t texMask) {
        chan[0].pad[0] = mode; chan[0].pad[1] = base; chan[1].pad[0] = amount; chan[1].pad[1] = texMask;
    }
    // GXV_VSOUT (VsOutPrune): what the fragment shader the draw is paired with never reads, so that the vertex shader neither
    // computes nor writes it. Kept in the padding of chan[2] and chan[3]; all zero (every key made with the switch off) means
    // "everything is read" and gives the text the generator always made.
    //   outCol(ch): kOutRgb = v_colN.rgb not read, kOutA = v_colN.a not read, kOutGone = v_colN not declared at all (the varying is left out)
    //   outTexOff(): bit i = v_texI not read (the varying is written as a constant: the fragment shader still declares it)
    static constexpr uint8_t kOutRgb = 1, kOutA = 2, kOutGone = 4;
    uint8_t outCol(int ch) const { return chan[2].pad[ch & 1]; }
    uint8_t outTexOff() const { return chan[3].pad[0]; }
    // GXV_TEXVEC2: bit i = v_texI is declared and written as a vec2 (its fragment shader's key has texVec2 and texgen i is not projective)
    uint8_t texVec2Mask() const { return chan[3].pad[1]; }
    void setTexVec2Mask(uint8_t m) { chan[3].pad[1] = m; }
    void setOut(uint8_t col0, uint8_t col1, uint8_t texOff) { chan[2].pad[0] = col0; chan[2].pad[1] = col1; chan[3].pad[0] = texOff; }
    // GXV_VS_TRIM (from the Strikers port's 1.3 MSC_VGX_VS_TRIM; BuildVsKey): facts about the lights of the draw, kept in the padding of
    // tg[6] and tg[7] (0 in every key made with the switch off, and in every key without a lit channel: the text as always).
    //   ltCos1(): bit l = light l's cos attenuation is exactly (1, 0, 0) (XF light words 4..6), so its spot polynomial is 1
    //   ltDist1(): bit l = light l's distance attenuation is exactly (1, 0, 0) (words 7..9), so its distance divisor is 1
    //   ltTrim(): the switch is on for this lit key: a light sum that provably stays >= 0 is clamped by min(x, 1) alone
    uint8_t ltCos1() const { return tg[7].pad[0]; }
    uint8_t ltDist1() const { return tg[7].pad[1]; }
    uint8_t ltTrim() const { return tg[6].pad[0]; }
    void setLtTrim(uint8_t cos1, uint8_t dist1) { tg[7].pad[0] = cos1; tg[7].pad[1] = dist1; tg[6].pad[0] = 1; }
    // GXV_VS_LTSKIP (vertex25; kept in tg[6].pad[1], 0 in every key made with the switch off): a light whose colour (rgb for a colour part, a for an alpha part,
    // all four for a merged loop) is exactly 0 adds exactly 0 to the sum, so its loop body is skipped by a branch on that uniform.
    bool ltSkip() const { return tg[6].pad[1] != 0; }
    void setLtSkip() { tg[6].pad[1] = 1; }
    // GXV_IND_VS (IndVsPair): texgen i (i < 6) feeds only indirect lookups and is written as a vec2 times the lookups' coordinate
    // scale 2^-s, 2^-t (GXSetIndTexCoordScale, BP 0x25 / 0x26). Kept in the padding of tg[0..5]: pad[0] = 0x10 | s, pad[1] = t.
    // 0 in every key made with the switch off (the text as always).
    bool indScaled(int i) const { return i < 6 && (tg[i].pad[0] & 0x10u) != 0; }
    uint8_t indScaleS(int i) const { return static_cast<uint8_t>(tg[i].pad[0] & 15u); }
    uint8_t indScaleT(int i) const { return static_cast<uint8_t>(tg[i].pad[1] & 15u); }
    void setIndScale(int i, uint8_t s, uint8_t t) { tg[i].pad[0] = static_cast<uint8_t>(0x10u | (s & 15u)); tg[i].pad[1] = static_cast<uint8_t>(t & 15u); }
};

// FsKey::fog bit: the (unclamped) fog factor is an affine function of clip w (perspective) or clip z (orthographic), made in
// the vertex shader and interpolated: GXV_FOGVTX. The bit and VsKey::fogVtx are set only with the switch on, so with it off
// every key (and every generated source) is the one that was made before. sizeof(FsKey) / sizeof(VsKey) are unchanged.
constexpr uint8_t kFogVtxBit = 0x10;
// FsKey::fog bit (GXV_FS_HALF_FOG, fs24; only together with kFogVtxBit): the fog factor is made from float parameters that travel in half storage as exact
// half values (FogHalfLanes), so the shader's colour variables can be half precision (FsHalfAnalyse takes the key). Set by the front end only with the switch
// on, for a key the half analysis accepts with it; 0 in every key made with the switch off (the text as always). The key then has one more TEV row (row 11).
constexpr uint8_t kFogHalfBit = 0x20;
// GXV_FS_HALF_FOG (read once, default 0; needs GXV_FOGVTX and GXV_FS_HALF = 1 or 3): see FogHalfLanes.
bool FsHalfFogMode();
// GXV_FS_NOCOLOR (read once, default 0): see FsKey::colorDead.
bool FsNoColorMode();
// The five numbers a kFogHalfBit key's fragment shader reads, from c1 and c0 of FogVtxCoeffs (the unclamped factor is c1 * v + c0, v the vertex fog varying):
//   fg = clamp((v * sInv - bs) * c1s + k, 0, 1)    in float, v and the arithmetic float, sInv / c1s / bs / k read from half storage
// Every lane is an exact half value (rounded here), so the conversion on the console cannot change it. sInv = 2^e (a power of two: v * sInv exact),
// c1s = c1 / sInv in [0.5, 1) up to its own half rounding (relative 2^-11: a slope error, the factor is exactly 0 at the ramp's start), bs = hi + lo = -c0 / c1s
// to 22 bits (hi, lo each a half value): bs is small (|c0| to 2 |c0|), so nothing large cancels in half and the float difference v * sInv - bs keeps
// the float shader's accuracy. A flat factor (c1 == 0, or too small for the lanes) is k = c0 clamped. wide: |bs| > 4096 (the float error then
// leaves the analysis's allowance: counted by the "fogfs24:" line, never seen so far).
struct FogHalfLane { float sInv, c1s, bHi, bLo, k; bool wide; };
FogHalfLane FogHalfLanes(float c1, float c0);
float RoundToHalf(float x); // the nearest half precision value (ties to even; subnormals; |x| > 65504 gives +-65504)
// GXV_FOGVTX (read once, default 0): 0 off; 1 linear fog with the orthographic fog flag (exact: see FogVtxCoeffs);
// 2 also linear fog with the perspective flag (a straight line through the factor's 0 and 1 points: NOT exact, see below)
long FogVtxMode();
// Whether a draw whose BP 0xF1 gave this fog value (fsel | ortho << 3) takes the vertex fog.
bool FogVtxEligible(uint8_t fog);
// The two numbers the vertex-fog fragment shader needs, from what the per-pixel one reads. Inputs are the fog block's
// A' (a * 2^b_s for the perspective form, a for the orthographic), B' and C as BuildTevC writes them, the projection's
// GX parameters p4 and p5 (XF 0x24, 0x25), the depth range glDepthRange gets (zn, zf), and the orthographic flag.
// The fragment shader then computes clamp(c1 * v_fog + c0, 0, 1).
//   ortho: gl_FragCoord.z = zn + (zf - zn) / 2 * (z_clip + 1) is affine in the interpolated clip z, so the factor is the
//          same number: exact (equal up to the order of float operations).
//   persp: the per-pixel factor is A' / (B' - z) - C with z = a + b / w: affine in w only if B' equals a constant the
//          projection fixes. GX's own fog set-up (GXSetFog) multiplies B by 8388638 / 2^23, so it never does, and the
//          factor is slightly curved in w. c1 and c0 are the line through the points where the factor is 0 and 1 (so a
//          fog ramp has its right start and end); between them the line is off by a small bow (see tests/vgx_fogvtx_gl.cpp
//          for its measured size).
struct FogVtxCoef { float c1, c0; };
FogVtxCoef FogVtxCoeffs(double a, double b, double c, double p4, double p5, double zn, double zf, bool ortho);
// GXV_FOG_BIND (read once, default 0 here; the Galaxy Vita build sets 2 in entry.cpp): the name of the vertex-fog varying, which
// the Vita host binds to a fixed semantic (vgx_host_galaxy_vita.cpp StartGl). Issue 6: "v_fog" was in no semantic binding, so
// vitaGL's GLOBAL mode gave it the first TEXCOORD free in the first shader that met it, in that run; a vertex and a fragment
// shader compiled in different runs (card cache, package seed, the farm) could disagree: fog lost, or a texture coordinate read
// from the fog's register. vitaGL finds compiled shaders by a hash of the GLSL text, so a new NAME is what keeps the old
// (unpinned) compiled files of the fog shaders from being reused: only the ~210 fog texts change (2.2 package list: 125 vertex,
// 85 fragment of ~2,430), every other text, the cache folder key, the package's pack and the warm-up lists stay valid.
//   0 "v_fog"         unbound, as 2.0-2.2 (exact old texts, old compiled files)
//   1 "v_fogc"        bound to the FOG semantic (FOGC; vitaGL FOUT/FIN): collision-free, needs libshacccg to accept FOGC
//   2 "v_fogt<slot>"  bound to TEXCOORD<slot>, GXV_FOG_SLOT (0..9, default 7). Every fog text of the 2.2 list uses only
//                     TEXCOORD0-3, 8, 9 (tex0-3, col0, col1): 4-7 are free in all of them. A key with fog AND a varying on the same
//                     slot (8 texgens for slot 7) would collide; the generator logs that once ("fog slot collision").
// Other values are 0. The texts differ from mode 0 only in the varying's name.
long FogBindMode();
int FogBindSlot();             // the TEXCOORD index of mode 2 (meaningless otherwise)
const char* FogVaryingName();  // "v_fog", "v_fogc" or "v_fogt<slot>"
// GXV_FS_HALF (read once, default 0): half precision (fp16) fragment shaders.
//   0 off (every fragment shader float, as always)
//   1 only the shaders FsHalfAnalyse proves to differ from the float shader by at most one 8-bit level in every colour channel
//     (kHalfMaxLevels) and, with an alpha test, by well under one level in alpha (kHalfMaxLevelsAlphaTest): the shader text declares
//     its colour variables as hvec3 / hvec4 / hfloat (half3 / half4 / half under vitaGL, plain floats on a PC); texture coordinates,
//     the alpha-test arithmetic and gl_FragColor stay float. No vitaGL patch is needed for this mode.
//   2 every fragment shader (vitaGL patch 0012: the translator #defines float -> half for the whole shader, texture coordinates too;
//     the back end sets it around the link, the text of the shaders is unchanged). For A/B comparisons.
//   3 like 1 with the stricter bound kHalfMaxLevelsStrict (colour within 0.5 of a level)
long FsHalfMode();
// What the analysis found for a key (all numbers are worst-case bounds, in 8-bit levels, of |half result - float result| before the final
// 8-bit rounding; they are meaningful only when `eligibleShape` is true).
struct FsHalfReport {
    bool eligibleShape = false;   // no indirect stage, fog, compare-mode stage, wrapped final value, out-of-range register, logic-op output
    const char* why = "";         // the first reason it is not (a string literal), "" if it is
    double colorLevels = 0.0;     // bound for the colour channels of the final value
    double alphaLevels = 0.0;     // bound for the alpha channel of the final value
    bool alphaTested = false;     // the generated shader has a `discard`
    // The bound of the alpha the alpha test reads (the TEV alpha before a destination-alpha constant or GXV_FS_ADEAD replaces the output's;
    // alphaLevels is the output's). Used by GXV_FS_HALF_MINOR only (mode 1 keeps its decisions, and so its texts, as they were).
    double testedAlphaLevels = 0.0;
    bool Eligible(long mode) const;
    // GXV_FS_HALF_MINOR: colour and untested alpha below kHalfMaxLevelsMinor; a tested alpha (testedAlphaLevels) within kHalfMaxLevelsAlphaTest.
    bool EligibleMinor() const;
};
constexpr double kHalfMaxLevels = 1.0, kHalfMaxLevelsStrict = 0.5, kHalfMaxLevelsAlphaTest = 0.15;
// GXV_FS_HALF_MINOR (shadowpx23; read once, default 0; with GXV_FS_HALF=1 or 3): a MINOR CHANGE. Keys mode 1 / 3 leave float but whose
// half error FsHalfAnalyse bounds below kHalfMaxLevelsMinor 8-bit levels in colour (and in an untested alpha), with an alpha test's
// own alpha within kHalfMaxLevelsAlphaTest, are made half too: at most 2 levels off in a colour channel (the bound is < 2 before the
// 8-bit rounding), never a different alpha-test result beyond what mode 1 allows. Only those keys' texts change (pack refill).
//   0 off; 1 on in every picture mode; "perf" (or "p") on in Performance mode only (GXV_AUTOQ_TARGET=60 without GXV_FPS_LIMIT=30).
constexpr double kHalfMaxLevelsMinor = 2.0;
long FsHalfMinorMode();
void FsHalfMinorModeForTest(long m); // (tests: -1 = the environment's)
FsHalfReport FsHalfAnalyse(const FsKey& key);
// Whether GenerateFragmentShader makes this key half precision under the current GXV_FS_HALF (modes 1 and 3).
bool FsHalfEligible(const FsKey& key);
// The fragment shader of a key with the precision given (for tests; a key whose shape FsHalfAnalyse rejects is always written as float).
void GenerateFragmentShaderPrecision(const FsKey& key, ShaderSource& out, bool half);
// GXV_NODISCARD (read once, default 0): no `discard` in a fragment shader whose alpha test cannot fail.
bool NoDiscard();
// GXV_DISCARD_BLEND (read once, default 0): the alpha test of a draw whose blend function, colour mask and depth mask make a source of
// vec4(0) leave the target as it is (no depth write; colour and alpha masked or blended with a destination factor that is 1 for a zero
// source) is written without `discard`: a failing fragment outputs vec4(0) (FsKey::rejectZero). Exact; keeps hidden-surface removal.
bool DiscardBlend();
// Whether alpha compare (comp0 ref0 op comp1 ref1; GX encodings, op 0 AND 1 OR 2 XOR 3 XNOR) is true for every 8-bit alpha. A
// negative ref means "not known" (only ALWAYS / NEVER and equal comparisons are then used).
bool AlphaTestAlwaysPasses(uint32_t comp0, int ref0, uint32_t op, uint32_t comp1, int ref1);
// Whether the generated text of this key has an alpha test (a `discard`, or the rejectZero select): alphaTestOn, not one that
// GXV_NODISCARD drops, not an atPass key.
bool FsHasAlphaTestText(const FsKey& k);

// ---- pix23: what a draw's fragment shader can output, from the values it reads (GXV_PIX_ZERO / GXV_PIX_ATPASS, vgx_draw.cpp) ---------
// FsValueEval follows exactly the arithmetic GenerateFragmentShader writes for the key (the TEV stages with their clamps, the final wrap
// and clamp, the alpha test, fog, the destination-alpha constant, the alpha-dead variant, the logic-op output), with every value an
// interval plus a bound on how far the shader's own (float or half) value can be from the real one (the half-precision analysis' Ab
// arithmetic: every operation rounded, an operand exactly 0 / 1 exact). Inputs are the draw's real values: TEV registers and konst
// colours as the uniform block carries them, the alpha-test references, the texture channels' ranges as decoded at upload (any filter
// gives a value between the smallest and the largest texel; plus one level), and the rasterised colours as the vertex key and the
// material colours make them. A value is "exactly 0" only where the shader's arithmetic cannot make anything else: a product with an
// exact 0, a sum of exact 0s, mix(a, b, c) with a = b = 0, or c exactly 0 and a = 0, or c exactly 1 and b = 0 (both code forms), a
// clamp of 0; a texture channel whose every texel is 0; a rasterised colour whose every vertex has 0 (interpolation of 0 is 0).
struct FsValueIn {
    int reg[4][4] = {};        // TEV registers PREV, REG0..2 as State::tevReg (signed 11-bit; over 255 the uniform is v / 255)
    int konst[4][4] = {};      // konst colours (the low byte)
    uint8_t texLo[8][4] = {}, texHi[8][4] = {}; // per texmap, the sampled R, G, B, A channels' smallest / largest byte
    bool texKnown[8] = {};     // false: 0..255 (an EFB copy, a replacement, a texture not decoded on this side)
    float rasLo[2][4] = {}, rasHi[2][4] = {}; // v_col0 / v_col1 components (r, g, b, a) per vertex: in this range
    bool rasZero[2][4] = {};   // the component is 0 at every vertex (so exactly 0.0 at every pixel)
    int ref0 = 0, ref1 = 0;    // alpha-test references (BP 0xF3)
    int dstAlpha = 0;          // the destination-alpha constant (BP 0x42 low byte)
    uint8_t fogColor[3] = {};  // fog colour (BP 0xF2)
};
struct FsValueOut {
    bool ok = false;           // the key was analysed (false: nothing below holds)
    int test = -1;             // FsHasAlphaTestText keys: 1 the test passes for every fragment, 0 it fails for every fragment, -1 either
    bool colorZero = false;    // gl_FragColor.rgb is exactly 0.0 for every fragment drawn
    bool alphaZero = false;    // gl_FragColor.a is exactly 0.0 for every fragment drawn
    float alphaLo = 0.0f, alphaHi = 1.0f; // bounds of the alpha the test compares (diagnostics)
    // gpu960 (GXV_PIX_BLENDOFF / GXV_PIX_CENSUS):
    int testRaw = -1;          // as `test`, but the alpha taken without the one-level widening of the texture channels' filtering (census: how near the test is)
    bool alphaOne = false;     // the alpha the blend uses (the shader's gl_FragColor.a) is exactly 1.0 for every fragment drawn: its value, error bound included, is >= 1
                               // before the final clamp (registers, konst, constants; a textured or rasterised alpha only when a register part lifts it over 1)
    float alphaLoOut = 0.0f;   // a lower bound of that alpha (0..1, error bound included)
};
void FsValueEval(const FsKey& k, const FsValueIn& in, FsValueOut& out);

uint64_t HashKey(const void* key, size_t size);
// Bump when the key structs or what is generated from them change: shader
// warm-up lists written by an older generator are then discarded.
// 2 is the 1.0 cache namespace. Keep it: the wrap modes change shader text, and vitaGL finds compiled
// shaders by a hash of that text, so no stale binary can be reused; a new number only empties every
// player's compiled cache and warm-up list (the 1.1 regression).
constexpr uint32_t kShaderGenVersion = 2;

// ---- seedgate31 (A0): the profile stamp ------------------------------------------------------------------------------------------
// ONE list of every switch that changes a shader KEY or a shader TEXT (and so the name of a compiled file), with the default the code
// uses when the variable is unset. Three readers: ProfileWord() below (the game logs it once: "profile: word=..."), the host test
// tests/vgx_profile_word.cpp, and tools/pack24/refill_pack.py (TEXT_SWITCHES is parsed from THIS list: do not copy it anywhere).
// A seed (stage list, key list, compiled files) harvested under another word is dead weight that passes every format check (the C184
// recipes were made with GXV_VSOUT=0, the release runs GXV_VSOUT=1): the merge tool refuses it.
// Add a switch here in the same change that adds the getenv; the line format is parsed by the tools, keep it: X("NAME", "default").
#define VGX_TEXT_SWITCHES(X) \
    X("GXV_TEVIN_CANON", "0") \
    X("GXV_TEVIN_PREWRAP", "0") \
    X("GXV_FIX_TEVIN", "0") \
    X("GXV_FIX_INDPREV", "0") \
    X("GXV_FIX_LODBIAS", "0") \
    X("GXV_FS_FINCLAMP", "0") \
    X("GXV_FS_ADEAD", "0") \
    X("GXV_FS_TEXREUSE", "0") \
    X("GXV_FS_PROJ", "0") \
    X("GXV_FS_HALF", "0") \
    X("GXV_FS_HALF_MINOR", "0") \
    X("GXV_FS_TRIM", "0") \
    X("GXV_FS_TRIM_PARTS", "1") \
    X("GXV_VS_TRIM", "0") \
    X("GXV_VS_LTSKIP", "0") \
    X("GXV_VSOUT", "0") \
    X("GXV_TEXVEC2", "0") \
    X("GXV_IND_VS", "0") \
    X("GXV_IND_PRECOMP", "0") \
    X("GXV_FOG_BIND", "0") \
    X("GXV_FOG_SLOT", "7") \
    X("GXV_FOGVTX", "0") \
    X("GXV_DISCARD_BLEND", "0") \
    X("GXV_NODISCARD", "0") \
    X("GXV_TEVWRAP", "0") \
    X("GXV_TEVWRAP_ELIDE", "0") \
    X("GXV_UNIPACK", "1") \
    X("GXV_PALROWS", "0") \
    X("GXV_PALIDX", "1") \
    X("GXV_LTMERGE", "0")
// Switches that join the word ONLY when set to something other than their default, so that the word (and a seed stamped with it) of a build
// with them off is the word it was before they existed. tools/pack24/refill_pack.py reads this list too.
#define VGX_TEXT_SWITCHES_OPT(X) \
    X("GXV_FS_ATFAST", "0") \
    X("GXV_VS_NOBRANCH", "0") \
    X("GXV_VS_NOBRANCH_LIGHT", "0") \
    X("GXV_FS_HALF_FOG", "0") \
    X("GXV_FS_NOCOLOR", "0") \
    X("GXV_FS_REGS", "0") \
    X("GXV_FS_ATEARLY", "0")
struct TextSwitch { const char* name; const char* dflt; };
constexpr TextSwitch kTextSwitches[] = {
#define VGX_TEXT_SWITCH_ROW(n, d) {n, d},
    VGX_TEXT_SWITCHES(VGX_TEXT_SWITCH_ROW)
#undef VGX_TEXT_SWITCH_ROW
};
constexpr size_t kTextSwitchCount = sizeof kTextSwitches / sizeof kTextSwitches[0];
constexpr TextSwitch kTextSwitchesOpt[] = {
#define VGX_TEXT_SWITCH_ROW(n, d) {n, d},
    VGX_TEXT_SWITCHES_OPT(VGX_TEXT_SWITCH_ROW)
#undef VGX_TEXT_SWITCH_ROW
};
constexpr size_t kTextSwitchOptCount = sizeof kTextSwitchesOpt / sizeof kTextSwitchesOpt[0];
// The value of switch i as `get` (std::getenv by default) answers it; unset or empty = the list's default. Raw text, not normalised:
// "01" and "1" are two profiles (the stricter reading; the tools use the same rule).
inline const char* TextSwitchValue(size_t i, const char* (*get)(const char*) = nullptr) {
    const char* v = get ? get(kTextSwitches[i].name) : std::getenv(kTextSwitches[i].name);
    return (v && *v) ? v : kTextSwitches[i].dflt;
}
// The 64-bit profile word: FNV-1a over "NAME=value\n" for every switch of the list in order, then "gen=<kShaderGenVersion>\n".
// (The vitaGL cache key, which also names the cache folder, is logged on the "compiled-shader cache:" line and checked by the tools.)
inline uint64_t ProfileWord(const char* (*get)(const char*) = nullptr) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](const char* s) { for (; *s; ++s) { h ^= static_cast<unsigned char>(*s); h *= 1099511628211ull; } };
    for (size_t i = 0; i < kTextSwitchCount; ++i) { mix(kTextSwitches[i].name); mix("="); mix(TextSwitchValue(i, get)); mix("\n"); }
    for (size_t i = 0; i < kTextSwitchOptCount; ++i) { // (only when not at the default: the off-state word is the word without these rows)
        const char* v = get ? get(kTextSwitchesOpt[i].name) : std::getenv(kTextSwitchesOpt[i].name);
        if (v && *v && std::strcmp(v, kTextSwitchesOpt[i].dflt) != 0) { mix(kTextSwitchesOpt[i].name); mix("="); mix(v); mix("\n"); }
    }
    char gen[32];
    std::snprintf(gen, sizeof gen, "gen=%u\n", static_cast<unsigned>(kShaderGenVersion));
    mix(gen);
    return h;
}
// The log lines of the stamp, one emit(const char*) call each (the game's vgx_draw.cpp LogProfileStamp and tests/vgx_profile_word.cpp share this, so the tools' parser
// and the game cannot drift): "profile: word=<16 hex> gen=<n> switches=<n>" then "profile-sw: NAME=value ..." in lines of at most ~180 characters.
template <typename Emit>
inline void EmitProfileLines(Emit&& emit, const char* (*get)(const char*) = nullptr) {
    char line[256];
    std::snprintf(line, sizeof line, "profile: word=%016llx gen=%u switches=%u", static_cast<unsigned long long>(ProfileWord(get)),
                  static_cast<unsigned>(kShaderGenVersion), static_cast<unsigned>(kTextSwitchCount));
    emit(static_cast<const char*>(line));
    size_t n = static_cast<size_t>(std::snprintf(line, sizeof line, "profile-sw:"));
    for (size_t i = 0; i < kTextSwitchCount; ++i) {
        char item[96];
        const int len = std::snprintf(item, sizeof item, " %s=%s", kTextSwitches[i].name, TextSwitchValue(i, get));
        if (len <= 0) continue;
        if (n + static_cast<size_t>(len) >= 180) { emit(static_cast<const char*>(line)); n = static_cast<size_t>(std::snprintf(line, sizeof line, "profile-sw:")); }
        n += static_cast<size_t>(std::snprintf(line + n, sizeof line - n, "%s", item));
    }
    for (size_t i = 0; i < kTextSwitchOptCount; ++i) {
        const char* v = get ? get(kTextSwitchesOpt[i].name) : std::getenv(kTextSwitchesOpt[i].name);
        if (!v || !*v || std::strcmp(v, kTextSwitchesOpt[i].dflt) == 0) continue;
        char item[96];
        const int len = std::snprintf(item, sizeof item, " %s=%s", kTextSwitchesOpt[i].name, v);
        if (len <= 0) continue;
        if (n + static_cast<size_t>(len) >= 180) { emit(static_cast<const char*>(line)); n = static_cast<size_t>(std::snprintf(line, sizeof line, "profile-sw:")); }
        n += static_cast<size_t>(std::snprintf(line + n, sizeof line - n, "%s", item));
    }
    emit(static_cast<const char*>(line));
}
// Uniform arrays are declared (and their blocks built) only as long as the shader needs
// them: u_tev[9 / 11 / 28], u_lt[4 + 5 per light], u_pal[60 / 96], u_tm per texgen. On the
// console every draw copies the program's whole uniform buffers once more.
//   GXV_UNIPACK=0   the old fixed sizes (what the reference frame hashes were made with)
bool UniformPacking();
// Sizes in vec4 of what a shader with this key declares.
int FsTevCount(const FsKey& k);  // u_tev
uint32_t FsTexChannels(const FsKey& k); // texture channels read (ShaderSource::texChan, GXV_EFBSWAP2)
int VsLightCount(const VsKey& k); // u_lt
constexpr int kPalRowsFull = 96, kPalRowsShort = 60, kPalShortMaxRow = 27;
// The small palette (GXV_PALROWS): position rows 0..11, then normal rows 0..11. J3D loads the matrices of a shape into
// slots 0, 3, 6, ... so a shape with at most four matrices never names a row above 9.
constexpr int kPalSmallK = 12, kPalRowsSmall = 24, kPalSmallMaxRow = 9;
// Rows of position matrix memory a palette of this perVtxPos value holds (the normal rows follow them in the block).
constexpr int PalRowsOfKey(int perVtxPos) { return perVtxPos == 3 ? kPalSmallK : 30; }
// GXV_PALROWS (default 0): 1 = a draw whose vertices use no matrix row above 9 gets the small palette (24 vec4 instead of
// 60 in the shader, the block, the upload and the console's uniform buffer); 2 = also count the rows draws use (census).
long PaletteRowsMode();
// GXV_VSOUT (read once, default 0): vertex shaders write only what their fragment shader reads.
//   0 off: every vertex shader writes v_col0, v_col1 (vec4 each) and every texgen, whatever the fragment shader reads (as always).
//   1 the front end marks in the vertex key (VsKey::setOut) the colour varyings, colour / alpha parts and texgens the paired fragment
//     shader never reads (VsOutPrune), and the generator leaves them out: no output (or a constant), no lighting loop for them, no
//     attribute read for them (a_col1 of a one-channel key is a constant vertex stream of its own on vitaGL's direct path).
// Exact: the fragment shader's result depends only on the varyings and components it reads, and those are computed by the same text.
long VsOutMode();
// Marks in `vs` what `fs` does not read (vs and fs as BuildVsKey / BuildFsKey made them, for the same draw state). keepColours: the
// draw may be drawn with the register-folding variant of `fs` (FsKey::regFromVtx = 3, which reads v_col0 and v_col1 whole): colours kept.
// A pure function of the two keys, so the key memo stays valid. With nothing to leave out the key is not changed.
void VsOutPrune(VsKey& vs, const FsKey& fs, bool keepColours);
// GXV_TEXVEC2 (read once, default 0): 1 = a fragment key with a non-projective texgen is marked (FsKey::setTexVec2) and its texture
// coordinate varyings v_texN of the non-projective texgens become vec2 in both shaders of the pair (VsKey::setTexVec2Mask): the
// fragment shader reads only .xy of them, so their third component (always 1.0, or a post matrix's third row) is neither written,
// stored, nor interpolated; a texture2D on such a coordinate takes the varying as it is. Exact (the same x and y everywhere).
// Every fragment shader with a texgen changes text: the shipped compiled cache does not have them (compiled on first use).
long TexVec2Mode();
// Marks the pair (fs and vs as made for the same draw state; fs first): returns whether fs was changed.
bool TexVec2Pair(FsKey& fs, VsKey& vs);
// GXV_IND_VS (read once, default 0; vita/vgx-game/entry.cpp sets 1): the coordinate scale of an indirect lookup moves into the vertex
// shader. Per pixel the fragment shader read texture2D(u_texM, uvN * u_tev[V_INDSCALE + i].xy): a dependent read (the coordinate is
// computed), which the console's GPU cannot prefetch. When texgen N is non-projective, below 6, and read by nothing but indirect
// lookups that all have the same scale, the vertex shader writes v_texN = coordinate * vec2(2^-s, 2^-t) (a vec2) and the lookup reads
// the varying as it is. Exact: the scale is a power of two, and every step from the vertex value to the interpolated coordinate (the
// products and sums of attribute set-up, perspective-correct interpolation, the divide) is a binary floating-point operation, which
// commutes with a power-of-two factor bit for bit (barring overflow / values below 2^-126, which texture coordinates are not). Other
// texgens and keys keep their text. bp25 / bp26: BP 0x25 / 0x26 (part of the key registers: vgx_state.cpp IsKeyBP).
long IndVsMode();
// GXV_IND_PRECOMP (gpuwater's item 2.1; a MINOR CHANGE: near-exact, see vgx_shadergen.cpp): read once, default 0.
long IndPreMode();
// Marks the first stage of the key whose indirect coordinate can be precomputed (an 8-bit matrix offset, no wrap, no add-previous in
// the key): returns whether one was marked.
bool IndPreMark(FsKey& fs);
// The per-draw values of the marked stage: (1 / w, 1 / h, c.s, c.t) for its texture of w x h guest texels (0: 1) and the indirect
// matrices as the TEV block holds them (TEV_INDMTX: 3 x 8 floats).
void IndPreValues(const FsKey& fs, const float* indMtx, float texW, float texH, float out[4]);
// Marks the pair (after TexVec2Pair, before VsOutPrune): returns whether anything was marked.
bool IndVsPair(FsKey& fs, VsKey& vs, uint32_t bp25, uint32_t bp26);
// GXV_FS_TRIM (read once, default 0; from the Strikers port's 1.3 MSC_VGX_FS_TRIM): fragment shaders without the clamps that cannot act.
// The Vita's shader compiler keeps every clamp of the text, each a VMIN and a VMAX (about a fifth of a typical TEV shader). FsTrimAnalyse
// follows the range of every TEV value through a key (texels, konst colours and the rasterised colours are 0..1; the TEV registers are
// 0..1 unless FsKey::inWrap says one is outside 0..255, then -1024..1023 / 255; a register read through the TEVIN wrap is 0..1 again)
// and says which side of which clamp can act; the generator leaves out the sides that cannot (one side: a max or a min; none: nothing),
// the final clamp the same way, and the floor of an ordered alpha compare (floor(x) < n is x < n for a whole n). Every fragment
// shader's text can change: adopt with a re-seeded shader pack. Near-exact: the bounds are real numbers, so a float rounding may leave a
// value an ulp past one (where the clamp would have cut it), and with multisampling the colours interpolated at an edge pixel's
// centre can lie outside the triangle's (outside 0..1). A half precision key (GXV_FS_HALF) is analysed for its error bound with the
// clamps the trimmed text keeps, so its proof covers the trimmed text. -- Superseded (r88 console, Beach Bowl "static"): the trim
// applies to FLOAT shaders only and takes no part in the precision decision; a key GXV_FS_HALF makes half (FsHalfEligible, the analysis
// of the full text) keeps the full text byte for byte (see GenerateFragmentShader in vgx_shadergen.cpp for why).
//   GXV_FS_TRIM=1 also makes the front end track TEV registers outside 0..255 (FsKey::inWrap bit 1, as GXV_FS_HALF does).
long FsTrimMode();
// GXV_FS_TRIM_PARTS=<bits> (read once, only with GXV_FS_TRIM=1; logged once, "fstrim:" basic-log line): which parts of the trim are made,
// for bisecting on the console (r90: Beach Bowl speckle with every part on, with or without GXV_FS_HALF / GXV_VS_TRIM). A part left out
// keeps the full text's operation; 0 is the full text byte for byte.
//   1  colour: idle clamp sides of TEV colour stages (not in a key with a compare-mode stage unless 32 is set too)
//   2  the floor of an ordered alpha compare (LESS, LEQUAL, GREATER, GEQUAL)
//   4  the final wrap (vgx_wrap_final, GXV_TEVWRAP) left out where the final value is proven in 0..1
//   8  alpha: idle clamp sides of TEV alpha stages
//   16 idle sides of the final clamp (one vec4 operation: colour and alpha together)
//   32 with 1: colour clamps also in a key with a compare-mode stage (whose R8 / GR16 / BGR24 compares read colour inputs and scale a
//      difference by up to 2^24, so a rounding at 0 or 1 that a clamp would remove can decide them)
// Default 1 (colour only: nothing that the alpha test, the depth / alpha writes or the blend's source alpha read); 63 = r76-r90 exactly.
constexpr long kFsTrimColourClamps = 1, kFsTrimFloor = 2, kFsTrimFinalWrap = 4, kFsTrimAlphaClamps = 8, kFsTrimFinalClamp = 16,
               kFsTrimCompareKeys = 32, kFsTrimAll = 63;
constexpr long kFsTrimPartsDefault = kFsTrimColourClamps;
long FsTrimParts();
struct FsTrimClamp { bool lo = true, hi = true; }; // the side can act somewhere in the key's ranges
struct FsTrimInfo { FsTrimClamp c[16], a[16], finC, finA; };
void FsTrimAnalyse(const FsKey& k, FsTrimInfo& out);
// The fragment shader of a key in the form given (tests: both forms in one process). half as GenerateFragmentShaderPrecision; a key
// written in half is never trimmed (trim is then ignored).
void GenerateFragmentShaderForm(const FsKey& key, ShaderSource& out, bool half, bool trim);
// The same with the trim parts given (kFsTrim* bits; tests: every mask in one process). A half key is never trimmed.
void GenerateFragmentShaderParts(const FsKey& key, ShaderSource& out, bool half, long parts);
// GXV_VS_TRIM (read once, default 0; from the Strikers port's 1.3 MSC_VGX_VS_TRIM mode 1): vertex shaders without the light terms the
// light registers make exactly 1 (VsKey::ltCos1 / ltDist1: a spot attenuation whose polynomial is (1, 0, 0) and whose divisor is
// (1, 0, 0) is 1, the GXInitLightAttn(1, 0, 0, 1, 0, 0) of every actor light of the game), and a light sum that cannot be negative
// clamped by min(x, 1) only. Exact for finite light data (GXV_LIGHTNORM=1, the default, zeroes the direction such a light's spot
// term would multiply by 0). The front end keeps the facts with the light registers (State::ltCos1 / ltDist1, part of the key hash).
long VsTrimMode();
// GXV_VS_LTSKIP (read once, default 0; vertex25): vertex shaders skip a light's loop body when the light's colour is exactly 0 (a uniform branch). Exact for
// finite light data (the term is at * 0 = +-0; the sum is clamped and ambient is >= 0). Changes the text of every lit vertex shader (the key carries the bit):
// the shader pack needs a re-seed before it can ship; with the pack as it is the new texts compile at run time.
long VsLtSkipMode();
// GXV_TEVIN_CANON (shader23, read once, default 0): the TEV-register out-of-range mark (FsKey::inWrap) only where it changes something.
// Today one TEV register outside 0..255 anywhere (State::tevOor, any of PREV / REG0..2, read by the draw or not) marks EVERY key of the
// state: the key is never half precision (FsHalfAnalyse), its clamps are kept for registers of -4..4, and with GXV_FIX_TEVIN every
// register read into A, B or C goes through vgx_w3 / vgx_w1 (two compares, two floors and a divide per component, per pixel), also
// reads of values the shader itself has just clamped to 0..1, and of registers that are in range.
//   1  the front end keeps which registers are out of range (State::tevOor becomes a mask, bit r = register r; part of the key hash as
//      before) and leaves a key unmarked when the mark could not change its result (FsTevInNeedsMark): no stage reads (as A, B, C or D,
//      colour or alpha) the initial value of an out-of-range register, and (GXV_FIX_TEVIN) every A / B / C read of a value a stage made
//      is proven by the range analysis to lie in 0..1, where vgx_w1 returns its argument (no compare-mode stage: those keep the mark
//      as soon as a stage-made value is read). The key is then the key of the same state with every register in range: its text,
//      compiled shader and program already exist (no new shader texts at all with =1), it can be half precision and is trimmed with
//      registers of 0..1. Exact for initial reads (a register of 0..255 is v / 255 in 0..1, which vgx_w1 returns unchanged, bit for
//      bit); near-exact for stage-made values (the range is proven in real numbers: a float rounding a ulp past 0 or 1, which the wrap
//      would have made exactly 0 or 1, passes as it is: a difference of ~1e-7).
//   2  also the text of a key that keeps the mark: an A / B / C read of a stage-made value proven in 0..1 has no wrap, and a register
//      whose initial value the key reads only as A / B / C is uploaded already wrapped (FsKey::preWrap, BuildTevC: (v & 255) / 255, GX's
//      low byte) and read without one; registers also read as D keep the wrap on their A / B / C reads. Same exactness as 1 (the CPU's
//      (v & 255) / 255 is the value vgx_w1 computes, correctly rounded). New keys and texts for marked keys: re-seed the shader pack.
long TevInCanonMode();
// Whether a key built while the registers `oorMask` (bit r: register r, 0 PREV .. 3 REG2, has a component outside 0..255) are out of
// range needs the mark. wrapIn: GXV_FIX_TEVIN (the wrap of A / B / C reads); without it only initial reads of such registers count.
bool FsTevInNeedsMark(const FsKey& k, unsigned oorMask, bool wrapIn);
// GXV_TEVIN_CANON=2: the register parts (bit r: r, g, b; bit r + 4: a) whose initial value a key reads, and reads only as A / B / C
// (FsKey::setPreWrap).
uint8_t FsTevInPreWrapMask(const FsKey& k);
// GXV_TEVIN_PREWRAP (pixcuts25): the registers (bit r) whose initial value a key (inWrap bit 0) reads as A / B / C with a kept wrap AND as D: FsKey::setDualWrap.
uint8_t FsTevInDualMask(const FsKey& k);
void TevInPrewrapModeForTest(long m); // (host tests: both forms in one process)
bool TevInPrewrapMode(); // GXV_TEVIN_PREWRAP read once (needs GXV_TEVIN_CANON=2), default 0
int FsTevBase(const FsKey& k); // the first row of a key's dual-wrap copies (FsTevCount - 4 for a key with dualWrap)
// (canon24b) A warm-up list's fragment key as the front end now makes it (the list may come from a run under another GXV_TEVIN_CANON):
// under =2 a marked key gets its FsTevInPreWrapMask, otherwise the mask is cleared. True when the key changed (vgx_draw.cpp EnsureShaderList).
bool CanonListFsKey(FsKey& k);
// GXV_FS_PROJ (shader23, read once, default 0): a projective texgen's texture read is texture2DProj(u_texM, v_texN) (Cg tex2Dproj: the
// GPU's iterator divides, a non-dependent read the PDS issues before the program runs) instead of texture2D(u_texM, v_texN.xy /
// (v_texN.z == 0.0 ? 1.0 : v_texN.z)) (a divide, a compare and a select per pixel and a dependent read). The q == 0 guard moves to the
// vertex shader (a projective texgen writes q = 1 where it is 0), which gives the same coordinates when q is 0 at all three vertices
// of a triangle (a zero matrix row: the guard's case); near-exact otherwise (a pixel whose interpolated q is exactly 0 is the only
// difference). Not for a stage with a LOD bias or an indirect coordinate (those keep the divide). Changes the text of every shader pair
// with a projective texgen (no key changes): re-seed the shader pack with it on.
long FsProjMode();
// (host tests only: force a mode of the two switches above in this process; -1 = the environment's again)
void TevInCanonModeForTest(long m);
void FsProjModeForTest(long m);
// GXV_FS_FINCLAMP (shader23 round 2, read once, default 0): each side (0, 1) of the final clamp of the TEV result is written only for the
// part (colour, alpha) whose value is not already bounded there bit for bit: the last stage's text bounds the part at 0.0 (clamp() or
// max(x, 0.0)) or at 1.0 (clamp() or min(x, 1.0)) (not a part vgx_wrap_final made: its byte / 255 can be an ulp above 1 on a GPU); an alpha
// replaced afterwards by the destination-alpha constant without an alpha test reading it, or that FsKey::alphaDead makes unused, needs none. clamp(clamp(x, 0, 1), 0, 1) == clamp(x, 0, 1) in float and in half: exact, every key's final line may change
// text (re-seed the shader pack with it on). Unlike GXV_FS_TRIM_PARTS bit 16 (left out on the console: values proven in range only in real
// numbers), nothing here rests on a range proof.
long FsFinClampMode();
// GXV_FS_ADEAD (shader23 round 2, read once, default 0): the alpha-dead variant of a key (FsKey::setAlphaDead) for draws whose render
// state never uses the source alpha: AlphaOutputUnused (vgx_cmd.h). Exact (the colour mask keeps the target's alpha; no blend factor reads
// the source alpha). New keys (one per such shader): re-seed the shader pack with it on.
long FsAlphaDeadMode();
void FsFinClampModeForTest(long m);
void FsAlphaDeadModeForTest(long m);
// GXV_FS_TEXREUSE (gpucut23, read once, default 0): a TEV stage whose texture read is the very statement that made the value `tex` still
// holds (same map, same coordinate variable - uvN / v_texN, written once; never the indirect itc -, same LOD-bias uniform, same swap) does
// not read again. Exact (the same value). Changes the text of keys with such a repeated read (Honeyhive: 11 of 131, its planet ground
// shaders among them): re-seed the shader pack with it on. ShaderSource::texReused counts the reads left out.
long FsTexReuseMode();
void FsTexReuseModeForTest(long m);
// GXV_FS_ATFAST (shader37, read once, default 0): the cut-out (alpha test) of a key without fog is written as one compare against a
// threshold in ALPHA units (no x255, +0.5, floor) or, with both sides, one interval test abs(a - mid) < half. The thresholds are made per
// draw (AtFastValues, BuildTevC) into u_tev row 9 (the fog row, free in a key without fog: FsTevCount grows to 10 for such a key).
// 1 = that; 2 = also the test is made in the LAST stage right after its alpha, before its colour is computed (the `discard` moves ahead of the
// colour arithmetic of that stage; needs the stage's alpha clamped on both sides, else the key keeps the test at the end).
// Eligible tests (AtFastKind): comparisons LESS / LEQUAL (upper bound), GREATER / GEQUAL (lower bound), EQUAL (both) joined by AND, and
// two lower bounds or two upper bounds joined by OR; ALWAYS is no bound. NEVER, NEQUAL, XOR, XNOR and mixed OR keep the old text.
// Exact except for an alpha within ~1e-6 of a half 8-bit level (the old text rounds alpha to a level first; the interval test compares the
// level boundaries themselves), and in a half-precision key the thresholds are half too (<= 0.03 level more).
long FsAtFastMode();
void FsAtFastModeForTest(long m);
int AtFastKind(uint8_t alphaTest);       // 0 not eligible, 1 lower bound only, 2 upper bound only, 3 interval
bool FsAtFastKey(const FsKey& k);        // the key's text uses the thresholds (mode on, kind != 0, test text present, no fog)
bool FsAtEarlyKey(const FsKey& k);       // ... and the test is made in the last stage (mode 2, the stage's alpha clamped, nothing after the test)
// The two numbers for u_tev row 9 from the draw's references (0..255): kind 1: (lo, -), 2: (-, hi), 3: (mid, half width), in alpha units.
void AtFastValues(uint8_t alphaTest, float ref0, float ref1, float out[2]);
// GXV_VS_NOBRANCH (shader37, read once, default 0): the vertex shader's non-light data-dependent `if`s become selects the compiler cannot turn into a
// branch (normalise-if-nonzero as a division by `l + step(l, 0.0)`, the palette index clamps as min() on the float before the int cast, the
// q == 0 guard of GXV_FS_PROJ as an add). Same values bit for bit (a division by exactly 1.0 and an add of exactly 0.0 change nothing).
// GXV_VS_NOBRANCH_LIGHT (default 0, needs GXV_VS_NOBRANCH): the per-light part (light-direction normalise guard and specular spot gate). Kept off by
// default: on multi-light vertex shaders Sony's Cg compiler expands branchless per-light selects to ~85 temporary registers (vs <= 52 with the branch),
// hurting SGX543 thread occupancy and exceeding Vita3K's 80-register limit (matching Strikers 2.0's MSC_VGX_VS_NOBRANCH_LIGHT=0 split).
long VsNoBranchMode();
void VsNoBranchModeForTest(long m);
long VsNoBranchLightMode();
void VsNoBranchLightModeForTest(long m);
// GXV_FS_ATEARLY (v2, read once, default 0): for alpha-tested fragment shaders with a real `discard`, keep ALL
// texture reads (and their indirect lookups and projective coordinates) BEFORE `discard` (so no implicit-LOD
// texture fetch ever runs after a kill on SGX543/GXM), run the entire TEV alpha pipeline + `discard` in Phase 1,
// and defer all TEV colour stages, final colour wrap/clamp, and fog to Phase 2 after `discard`.
// Exact: any alpha register read by a colour stage before a later alpha stage overwrites it is snapshotted in
// Phase 1; keys with colour-reading alpha compares (R8/GR16/BGR24), indirect addPrev, or colorDead stay on the
// standard path. Changes generated text of eligible alpha-tested shaders -> re-harvest shader pack.
long FsAtEarlyMode();
void FsAtEarlyModeForTest(long m); // (tests: -1 = environment)
bool FsAtEarlySplitEligible(const FsKey& k);
struct FsAtEarlyStats {
    uint64_t split = 0, ineligible = 0, colorStagesDeferred = 0, texKeptBeforeDiscard = 0, snapVars = 0;
};
void FsAtEarlyCounters(FsAtEarlyStats& out);
void GenerateFragmentShader(const FsKey& key, ShaderSource& out);
void GenerateVertexShader(const VsKey& key, ShaderSource& out);
// asyncsh31 (GXV_UBERSHADER, vgx_ubershader.h): the decisions GenerateFragmentShader makes for a key, from the generator's own helpers (the ubershader's
// configuration is built from these, never from a second copy of the rules). Untrimmed: the trims GXV_FS_TRIM makes are proven inert, the ubershader keeps the full ops.
struct FsUberPlan {
    uint8_t wrapKeep[16] = {};  // per stage, with wrapIn: the A / B / C register reads that keep their byte wrap (colour a, b, c = 1, 2, 4; alpha = 8, 16, 32)
    bool wrapIn = false;        // FsKey::inWrap bit 0 (GXV_FIX_TEVIN): vgx_w1 / vgx_w3 on the kept register reads
    uint8_t dualRegs = 0;       // GXV_TEVIN_PREWRAP's wrapped register copies (the ubershader does not take such keys)
    bool wrapColor = false, wrapAlpha = false; // the final byte wrap (GXV_TEVWRAP), untrimmed
    bool indPrev = false;       // GXV_FIX_INDPREV acts on this key (an add-previous indirect stage)
    bool alphaTestedText = false; // alphaTestOn and the test can fail (NoDiscard)
    bool testText = false;      // the text has the test (not an atPass key)
    bool rejectZero = false;    // a failing fragment writes vec4(0) (GXV_DISCARD_BLEND) instead of `discard`
    bool preOnly = false;       // GXV_ALPHATEST_PREPASS: vec4(0) after the test
    bool alphaZero = false;     // GXV_FS_ADEAD: the output alpha is written as 0
    bool half = false;          // the specialised text is half precision (GXV_FS_HALF=1/3): the ubershader is float (at most the proven one level apart)
    bool projRead = false;      // GXV_FS_PROJ (texture2DProj reads): not taken by the ubershader
};
void FsUberPlanOf(const FsKey& key, FsUberPlan& out);

// GXV_FS_REGS (fsregs25, read once, default 0): fragment shader register-pressure and u_tev compaction.
//   * Compacts u_tev[] in the generated text to only the rows this key actually reads, recording the
//     canonical source row for each slot on ShaderSource::tevSrc[0..nTev-1] (tevGather = true).
//   * Front-end UB_TEV blocks stay in the canonical layout (FsTevCount / FsTevBase unchanged; every CPU
//     reader and variant/general block match stay untouched); the back end gathers u_tev[] at upload time.
//   * Elides unread/unwritten TEV register temps (reads u_tev[]/v_col directly for constant regs),
//     sinks projective uvN divides and indirect lookups to first use, and scopes single-stage tex temps.
// Exact (identical arithmetic and values; changes generated fragment shader text -> re-harvest shader pack).
long FsRegsMode();
void FsRegsModeForTest(long m);
struct FsRegsStats {
    uint64_t shaders = 0, rowsOld = 0, rowsNew = 0, maxOld = 0, maxNew = 0;
    uint64_t regsElided = 0, uvsSunk = 0, indSunk = 0, fallbacks = 0;
};
void FsRegsCounters(FsRegsStats& out);
inline const float* FsTevGather(const ShaderSource& src, const float* canon, size_t canonFloats, float scratch[128]) {
    if (!src.tevGather) return canon;
    const int cnt = src.nTev > 32 ? 32 : static_cast<int>(src.nTev);
    for (int i = 0; i < cnt; ++i) {
        const size_t off = static_cast<size_t>(src.tevSrc[i]) * 4u;
        if (off + 4u <= canonFloats) std::memcpy(scratch + i * 4, canon + off, 16u);
        else std::memset(scratch + i * 4, 0, 16u);
    }
    return scratch;
}

} // namespace vgx
