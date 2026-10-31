// Structural shader keys and GLSL ES 1.00 generation. Front-end side: no GL.
#pragma once
#include "vgx_cmd.h"

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
        uint8_t texMap = 0, texCoord = 0, pad[2] = {};
    } ind[4];
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
};

// FsKey::fog bit: the (unclamped) fog factor is an affine function of clip w (perspective) or clip z (orthographic), made in
// the vertex shader and interpolated: GXV_FOGVTX. The bit and VsKey::fogVtx are set only with the switch on, so with it off
// every key (and every generated source) is the one that was made before. sizeof(FsKey) / sizeof(VsKey) are unchanged.
constexpr uint8_t kFogVtxBit = 0x10;
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
    bool Eligible(long mode) const;
};
constexpr double kHalfMaxLevels = 1.0, kHalfMaxLevelsStrict = 0.5, kHalfMaxLevelsAlphaTest = 0.15;
FsHalfReport FsHalfAnalyse(const FsKey& key);
// Whether GenerateFragmentShader makes this key half precision under the current GXV_FS_HALF (modes 1 and 3).
bool FsHalfEligible(const FsKey& key);
// The fragment shader of a key with the precision given (for tests; a key whose shape FsHalfAnalyse rejects is always written as float).
void GenerateFragmentShaderPrecision(const FsKey& key, ShaderSource& out, bool half);
// GXV_NODISCARD (read once, default 0): no `discard` in a fragment shader whose alpha test cannot fail.
bool NoDiscard();
// Whether alpha compare (comp0 ref0 op comp1 ref1; GX encodings, op 0 AND 1 OR 2 XOR 3 XNOR) is true for every 8-bit alpha. A
// negative ref means "not known" (only ALWAYS / NEVER and equal comparisons are then used).
bool AlphaTestAlwaysPasses(uint32_t comp0, int ref0, uint32_t op, uint32_t comp1, int ref1);

uint64_t HashKey(const void* key, size_t size);
// Bump when the key structs or what is generated from them change: shader
// warm-up lists written by an older generator are then discarded.
// 2 is the 1.0 cache namespace. Keep it: the wrap modes change shader text, and vitaGL finds compiled
// shaders by a hash of that text, so no stale binary can be reused; a new number only empties every
// player's compiled cache and warm-up list (the 1.1 regression).
constexpr uint32_t kShaderGenVersion = 2;
// Uniform arrays are declared (and their blocks built) only as long as the shader needs
// them: u_tev[9 / 11 / 28], u_lt[4 + 5 per light], u_pal[60 / 96], u_tm per texgen. On the
// console every draw copies the program's whole uniform buffers once more.
//   GXV_UNIPACK=0   the old fixed sizes (what the reference frame hashes were made with)
bool UniformPacking();
// Sizes in vec4 of what a shader with this key declares.
int FsTevCount(const FsKey& k);  // u_tev
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
void GenerateFragmentShader(const FsKey& key, ShaderSource& out);
void GenerateVertexShader(const VsKey& key, ShaderSource& out);

} // namespace vgx
