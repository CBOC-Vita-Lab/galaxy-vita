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
        // pad[0] (GXV_IND_VS, IndVsPair): 1 = the vertex shader writes this lookup's coordinate already scaled (v_texN, a vec2, read by
        // nothing else): the lookup is texture2D(u_texM, v_texN), a non-dependent read. 0 in every key made with the switch off.
        uint8_t texMap = 0, texCoord = 0, pad[2] = {};
    } ind[4];
    uint8_t indVs(int i) const { return ind[i].pad[0]; }
    // GXV_TEXVEC2 (TexVec2Pair): the texture coordinate varyings of the non-projective texgens are vec2 (v_texN.xy is all such a
    // texgen's coordinate uses); kept in the padding of stage[0], 0 in every key made with the switch off (the text as always).
    uint8_t texVec2() const { return stage[0].pad[0]; }
    void setTexVec2(uint8_t on) { stage[0].pad[0] = on; }
    // GXV_DISCARD_BLEND (vgx_draw.cpp RejectZeroFs): a fragment that fails the alpha test writes vec4(0) instead of being killed. Set
    // only by the front end, only for a draw whose blend makes a zero source a no-op (RejectIsNoop); kept in the padding of stage[0],
    // 0 in every key made with the switch off (the text as always).
    uint8_t rejectZero() const { return stage[0].pad[1]; }
    void setRejectZero(uint8_t on) { stage[0].pad[1] = on; }
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
// GXV_DISCARD_BLEND (read once, default 0): the alpha test of a draw whose blend function, colour mask and depth mask make a source of
// vec4(0) leave the target as it is (no depth write; colour and alpha masked or blended with a destination factor that is 1 for a zero
// source) is written without `discard`: a failing fragment outputs vec4(0) (FsKey::rejectZero). Exact; keeps hidden-surface removal.
bool DiscardBlend();
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
void GenerateFragmentShader(const FsKey& key, ShaderSource& out);
void GenerateVertexShader(const VsKey& key, ShaderSource& out);

} // namespace vgx
