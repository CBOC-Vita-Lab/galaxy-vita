// The front-end / back-end seam.
//
// The front end (vgx_api.cpp, vgx_state.cpp, vgx_draw.cpp, vgx_texcache.cpp,
// vgx_shadergen.cpp) turns GX API calls and command streams into a CmdBuf: a
// list of self-contained commands plus the arenas they point into. Nothing in
// a CmdBuf refers to guest memory or to mutable front-end state, so a CmdBuf
// can be handed to another thread and executed later. The back end
// (vgx_backend_gl.cpp) only ever sees CmdBufs.
#pragma once
#include "vgx_static_crowd.h"
#include "vgx_common.h"
#include "vgx_hrw_pair_depth.h"
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace vgx {

// ---------------------------------------------------------------- vertices
// Decoded vertices are interleaved, tightly packed in this order:
//   pos    4 x f32  (x, y, z, position-matrix row index)      always
//   nrm    3 x f32                                            VL_NRM
//   col0   4 x u8 (normalised)                                VL_COL0
//   col1   4 x u8                                             VL_COL1
//   texN   2 x f32 each                                       VL_TEX0 << N
//   tmi    8 x u8 (texture-matrix row index per texcoord)     VL_TMI
//
// GXV_VTXPACK (vgx_dlcache.cpp "packed static runs"): a static run of a cached display list may keep the GX source's compact
// integer form instead (bits 12..15, only ever on static runs; every dynamic / arena vertex has them clear):
//   VL_QPOS   pos  4 x s16  (x, y, z as whole numbers q = value * 2^k, w = the matrix row as it is)   8 bytes instead of 16
//   VL_QNRM   nrm  3 x s16 + 2 bytes of padding                                                 8 bytes instead of 12
//   VL_QNRM8  nrm  3 x s8 + 1 byte of padding                                                   4 bytes instead of 12
//   VL_QTEX   tex0 and tex1 (those the run has) 2 x s16 each                                     4 bytes instead of 8
// The GPU converts each whole number to the float of the same value; the 2^-k is folded into the matrices that multiply the attribute
// (vgx_draw.cpp VqScaleBlocks), so the vertex shader's text is unchanged. Every attribute is still 4-byte aligned.
enum : uint32_t {
    VL_NRM = 1u << 0,
    VL_COL0 = 1u << 1,
    VL_COL1 = 1u << 2,
    VL_TEX0 = 1u << 3, // .. VL_TEX0 << 7
    VL_TMI = 1u << 11,
    VL_ALLTEX = 0xFFu << 3,
    VL_QPOS = 1u << 12,
    VL_QNRM = 1u << 13,
    VL_QTEX = 1u << 14,
    VL_QNRM8 = 1u << 15,
    VL_QALL = VL_QPOS | VL_QNRM | VL_QTEX | VL_QNRM8,
};
// (vgx_dlcache.cpp / vgx_draw.cpp, also declared in vgx_state.h) the switch and its totals, for the back end's "vtxfeed:" line
bool VtxPackOn();
void VtxPackCounters(uint64_t out[7]);
void VtxPackDrawCounters(uint64_t out[4]);
struct VtxLayout {
    uint32_t mask = 0;
    uint16_t stride = 16;
    uint8_t offNrm = 0, offCol0 = 0, offCol1 = 0, offTmi = 0;
    uint8_t offTex[8] = {};
};
inline VtxLayout MakeLayout(uint32_t mask) {
    VtxLayout l;
    l.mask = mask;
    uint32_t o = (mask & VL_QPOS) ? 8 : 16;
    if (mask & VL_NRM) { l.offNrm = static_cast<uint8_t>(o); o += (mask & VL_QNRM8) ? 4 : (mask & VL_QNRM) ? 8 : 12; }
    if (mask & VL_COL0) { l.offCol0 = static_cast<uint8_t>(o); o += 4; }
    if (mask & VL_COL1) { l.offCol1 = static_cast<uint8_t>(o); o += 4; }
    for (int i = 0; i < 8; ++i)
        if (mask & (VL_TEX0 << i)) { l.offTex[i] = static_cast<uint8_t>(o); o += (i < 2 && (mask & VL_QTEX)) ? 4 : 8; }
    if (mask & VL_TMI) { l.offTmi = static_cast<uint8_t>(o); o += 8; }
    l.stride = static_cast<uint16_t>(o);
    return l;
}
// Fixed attribute locations (bound by name before linking).
enum : int { AT_POS = 0, AT_NRM = 1, AT_COL0 = 2, AT_COL1 = 3, AT_TEX0 = 4, AT_TMI0 = 12, AT_TMI1 = 13, AT_COUNT = 14 };

// ---------------------------------------------------------- uniform blocks
// Uniform values travel as blocks of floats in CmdBuf::uni. A draw names one
// offset per block; the back end skips blocks whose offset it already applied
// to the program. Sizes are in floats.
enum : int {
    UB_PROJ = 0, // mat4 (column-major, Y flip and GX->GL depth remap folded in)
    UB_MV,       // posMtx 3 x vec4, nrmMtx 3 x vec4(xyz used)
    UB_TEXM,     // texMtx 8 x 3 x vec4, postMtx 8 x 3 x vec4
    UB_PAL,      // posMtx memory 64 x vec4, nrmMtx memory 32 x vec4(xyz used)
    UB_LIGHT,    // amb 2 x vec4, mat 2 x vec4, lights 8 x {pos, dir, col, cosatt, distatt}
    UB_TEV,      // see TevBlock in vgx_draw.cpp
    UB_COUNT
};
constexpr uint32_t kUbSize[UB_COUNT] = {16, 24, 192, 384, 176, 112};
constexpr uint32_t kNoBlock = 0xFFFFFFFFu;
// Layout of UB_TEV (float indices)
enum : int {
    TEV_REG = 0,      // 4 x vec4: PREV, REG0, REG1, REG2 (0..1 scale, may be outside for S10)
    TEV_KONST = 16,   // 4 x vec4
    TEV_MISC = 32,    // alphaRef0, alphaRef1 (0..255), dstAlpha (0..1), zTexBias
    TEV_FOG = 36,     // A', B', C, unused
    TEV_FOGCOL = 40,  // rgb
    TEV_INDMTX = 44,  // 3 x {vec4 (m00,m01,m02,scale), vec4 (m10,m11,m12,0)}
    TEV_INDSCALE = 68, // 4 x vec4: (coordScaleS, coordScaleT, 1/texW, 1/texH) per indirect stage
    TEV_TEXSIZE = 84,  // 4 x vec4: (w, h) of texmap 2n in xy, of texmap 2n+1 in zw
    TEV_LODBIAS = 100, // 2 x vec4: the LOD bias of texmap 0..7 (GXV_FIX_LODBIAS; only a shader with FsKey::Stage::lodBias reads them)
    TEV_INDPRE = 108,  // vec4: (1/w, 1/h, c.s, c.t) of the stage FsKey::indPre marks (GXV_IND_PRECOMP; vgx_shadergen.cpp IndPreValues)
};

// ------------------------------------------------------------ render state
enum : uint8_t { BF_ZERO, BF_ONE, BF_SRC_COLOR, BF_INV_SRC_COLOR, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_DST_ALPHA, BF_INV_DST_ALPHA, BF_DST_COLOR, BF_INV_DST_COLOR };
enum : uint8_t { BE_ADD, BE_REVSUB };
enum : uint8_t { CULL_NONE, CULL_BACK, CULL_FRONT, CULL_ALL }; // GX "front" = clockwise on screen
enum : uint8_t { CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NEQUAL, CMP_GEQUAL, CMP_ALWAYS };
struct RenderState {
    uint8_t blend = 0, blendSrc = BF_ONE, blendDst = BF_ZERO, blendEq = BE_ADD;
    uint8_t depthTest = 0, depthFunc = CMP_LEQUAL, depthWrite = 0, cull = CULL_NONE;
    uint8_t colorWrite = 1, alphaWrite = 1;
    uint8_t dstAlphaZero = 0; // blended RGB with independent alpha factors: 1 ZERO/ZERO, 2 ONE/ZERO (GXV_FIX_DSTA: the shader writes the constant)
    uint8_t lineWidth6 = 6;   // GX line width / point size in 1/6 pixel
    // Scissor and viewport in EFB pixels, origin top-left. The back end scales
    // them to the render target.
    int16_t scX = 0, scY = 0, scW = 640, scH = 528;
    float vpX = 0, vpY = 0, vpW = 640, vpH = 528, zNear = 0, zFar = 1;
};

// What the target GPU's shader patcher keys a fragment program on besides the shaders:
// the blend function (or none), colour mask and zero-alpha override. 13 bits. (vitaGL: blend_info in
// blending.c, compared per draw in setup_frag_program, custom_shaders.c.)
inline uint32_t BlendKey(const RenderState& rs) {
    return (rs.blend ? 1u | (static_cast<uint32_t>(rs.blendEq & 1u) << 1) | (static_cast<uint32_t>(rs.blendSrc & 15u) << 2) |
                           (static_cast<uint32_t>(rs.blendDst & 15u) << 6)
                     : 0u) |
           (rs.colorWrite ? 1u << 10 : 0u) | (rs.alphaWrite ? 1u << 11 : 0u) | (rs.dstAlphaZero ? 1u << 12 : 0u);
}
// GXV_DISCARD_BLEND (vgx_draw.cpp RejectZeroFs): whether a fragment whose output is vec4(0) leaves every value this render state lets
// a draw write as it was, as a killed (`discard`) fragment does: no depth write (as the back end sets the mask: only with the test on);
// colour and alpha each either masked, or blended by an equation that adds or reverse-subtracts the source term with a destination
// factor that is 1 for a zero source (ONE, ONE_MINUS_SRC_ALPHA, ONE_MINUS_SRC_COLOR), alpha with the colour's factors (dstAlphaZero 0,
// not the ZERO/ZERO or ONE/ZERO override). dst * 1 + 0 * sf = dst and dst * 1 - 0 * sf = dst exactly for any factor sf in 0..1.
// GXV_FS_ADEAD (shader23): whether the source alpha of a draw with this render state reaches nothing: alpha not written (the colour mask),
// and the colour either not written, not blended, or blended with factors that do not read the source alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
// (The destination-alpha override, dstAlphaZero, only acts on a written alpha.)
inline bool AlphaOutputUnused(const RenderState& rs) {
    if (rs.alphaWrite) return false;
    if (!rs.colorWrite || !rs.blend) return true;
    auto srcA = [](uint8_t f) { return f == BF_SRC_ALPHA || f == BF_INV_SRC_ALPHA; };
    return !srcA(rs.blendSrc) && !srcA(rs.blendDst);
}
inline bool RejectIsNoop(const RenderState& rs) {
    if (rs.depthTest && rs.depthWrite) return false;
    if (!rs.colorWrite && !rs.alphaWrite) return true;
    if (!rs.blend) return false;
    const bool dstOne = rs.blendDst == BF_ONE || rs.blendDst == BF_INV_SRC_ALPHA || rs.blendDst == BF_INV_SRC_COLOR;
    if (!dstOne || (rs.blendEq != BE_ADD && rs.blendEq != BE_REVSUB)) return false;
    return !rs.alphaWrite || rs.dstAlphaZero == 0;
}
// CMD_VARIANT: this program will be drawn with this vertex layout, blend function and
// colour mask. On the console a patched vertex program exists per (vertex shader, layout)
// and a patched fragment program per (program, blend function, colour mask), and each is
// built at the first draw that needs it (milliseconds). The back end draws one empty
// triangle now so that it is built here instead (shader warm-up list).
struct VariantCmd {
    uint16_t vsId = 0, fsId = 0;
    uint16_t layoutMask = 0, blendKey = 0;
};

enum : uint8_t { WRAP_CLAMP, WRAP_REPEAT, WRAP_MIRROR };
enum : uint8_t { FILT_NEAR, FILT_LINEAR, FILT_NEAR_MIP_NEAR, FILT_LIN_MIP_NEAR, FILT_NEAR_MIP_LIN, FILT_LIN_MIP_LIN };
struct TexBind {
    uint32_t texId = 0; // 0 = none (back end binds a 1x1 white texture)
    uint8_t wrapS = WRAP_CLAMP, wrapT = WRAP_CLAMP, minFilter = FILT_LINEAR, magFilter = FILT_LINEAR;
    // GXV_LODBIAS_SAMPLER (vgx_lodsampler.h): the texture unit's GX LOD bias in 1/8 levels when the shader does not apply it (the back end adds it to the
    // sampler bias of the texture it binds); lodWhy: the census's class of the unit (lodsampler::Why; 0 = no bias). Both 0 with the switch off.
    int8_t lodSteps = 0;
    uint8_t lodWhy = 0;
    uint8_t lodPad[2] = {};
};

enum : uint8_t { PRIM_TRIANGLES, PRIM_LINES, PRIM_POINTS };
// GXV_SHADOW_STENCIL: DrawRec::stOp. The stencil counts the shadow volumes, so that the silhouette fill is drawn where it is not 0 in place of
// where the captured alpha is 2 or more: INCR on a volume's back faces (alpha += a), DECR on its front faces (alpha -= a), ZERO where an alpha 0
// is written after the first volume, SET where the player's silhouette writes its alpha, TEST on the fill (stencil != 0).
enum : uint8_t { ST_NONE = 0, ST_ZERO = 1, ST_INCR = 2, ST_DECR = 3, ST_SET = 4, ST_TEST = 5, ST_OPMASK = 0x7F, ST_WINBEGIN = 0x80 };

struct DrawRec {
    uint16_t vsId = 0, fsId = 0;
    uint8_t prim = PRIM_TRIANGLES;
    uint8_t texMask = 0; // which of tex[] the fragment shader samples
    // The record starts with a primitive that writes every pixel of `ex` whatever was there:
    // a rectangle over the whole viewport, no blending, no alpha test, no depth test.
    uint8_t cover = 0;
    // GXV_SHADOWFILL_BOUNDS (vgx_draw.cpp, "shadow fill bounds"): 1 when abX..abH hold the EFB pixels the record can write,
    // worked out from its vertices (or its list's bounds) when it was made because it writes alpha other than the constant 0.
    // 0: not known (the extent below is all that is known). For the front end's bookkeeping only, not for drawing.
    uint8_t ab = 0;
    uint8_t joined = 0; // ss24: how many later draws the front end merged into this record (saturating); a merged volume record is several volumes for GXV_SS_TILEBOUND
    uint8_t playerMark = 0; // g30: 1 when drawn inside PORT_GX_MARK_PLAYER_BEGIN..END (Mario/Luigi/MP puppets: never deferred/skipped by asynclink; fits in padding before exX)
    // The EFB rectangle the record's pixels are known to stay inside: the scissor, or the
    // scissor cut to the viewport when the vertices are known not to leave it (on the
    // console, as on the Wii, only the scissor cuts; a desktop GL clips to the viewport).
    // For the front end's bookkeeping of what the EFB holds (pass isolation), not for drawing.
    int16_t exX = 0, exY = 0, exW = 0, exH = 0;
    int16_t abX = 0, abY = 0, abW = 0, abH = 0; // (see ab)
    uint32_t layoutMask = 0;
    uint32_t vbuf = 0;      // 0: vertices / indices are in the frame's arenas; else a static buffer (BufUpload)
    // Where index value 0 of this draw is: bytes into the static vertex buffer, or into the
    // draw's chunk of CmdBuf::vtx (chunk start = CmdBuf::vtxChunks[vchunk], or 0 when the
    // frame has no chunk list). With absolute indices (the default, see "vertex addressing"
    // in vgx_draw.cpp) this is always 0 and the draw's first vertex is number `vtxFirst`;
    // in the old scheme (GXV_ABSIDX=0) every draw has its own offset and vtxFirst is 0.
    uint32_t vtxOffset = 0;
    uint32_t vtxCount = 0;
    uint32_t idxOffset = 0; // elements into CmdBuf::idx (or the static index buffer)
    uint32_t idxCount = 0;
    uint32_t vtxFirst = 0;  // index value of the draw's first vertex (what was added to its 0-based indices)
    uint16_t vchunk = 0;    // chunk of CmdBuf::vtx (dynamic draws)
    // GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT (gpuab23; set only while either is on, GXV_SHADOW_CULLCOVER=1): 1 when the record would be
    // `cover` but for its cull mode, and its rectangle faces the side that mode draws (back culling, wound clockwise on the screen as GX
    // draws front faces; front culling, counter-clockwise): it writes every pixel of `ex` all the same. The game's own EFB clear is such
    // a quad (MainLoopFramework::clearEfb: GX_CULL_BACK). Read by the front end's alpha bookkeeping only (ShadowFillPass), never by the
    // back end, so nothing else changes. (In the padding after vchunk: DrawRec's size is unchanged.)
    uint8_t coverCull = 0;
    // GXV_HUD_NATIVE (hud24): 1 when the draw was made between the game's HUD markers (PORT_GX_MARK_HUD_BEGIN / _END, or the 2D parts of the
    // star-select and timer scenes, PORT_GX_MARK_HUDN_*); always 0 with the switch off (the markers are not written then). The back end draws
    // the tail of a picture made only of such draws at the screen's own resolution (vgx_backend_gl.cpp, "GXV_HUD_NATIVE"). In the padding after
    // coverCull: DrawRec's size is unchanged.
    uint8_t hud = 0;
    // GXV_SHADOW_STENCIL (pass24; ShadowStencilPass in vgx_draw.cpp, Draw in vgx_backend_gl.cpp): the stencil operation the back end applies to this
    // draw on the EFB (ST_* below, plus ST_WINBEGIN on the window's first draw); 0 (always, with the switch off) leaves the stencil alone.
    // (Not in padding: DrawRec grew by 4 bytes, nothing depends on its size; RenderState is untouched.)
    uint8_t stOp = 0;
    // GXV_SILH_OCC (across26; SilhGatePass in vgx_draw.cpp, vgx_silhocc_gl.h): 1 on the player's silhouette draws of a picture whose silhouette is being measured: the
    // back end wraps a run of such draws in one GL_SAMPLES_PASSED query and polls it a few pictures later. 0 (always, with the switch off) draws as before. The draw is
    // drawn exactly as without the flag; merges and reorders treat a flagged draw as a barrier (the count must hold only its pixels).
    uint8_t occ = 0;
    // GXV_CLEARQ (pixcuts25; vgx_draw.cpp ClearQMark): the record is the game's EFB clear quad as an exact constant clear: bit 0 set, bits 1..4 = the
    // colour channels (r, g, b, a) that are 255 (the others 0). The depth is the render state's constant (zNear == zFar, 0 or 1). 0 for every record made with
    // the switch off. The back end (ClearQTry) draws a glClear of colour, alpha and depth for it instead of the quad when it also finds the shape intact.
    uint8_t clearQ = 0; // (bit 0: a clear; bit 1: depth only; bit 2: no depth, colour and alpha only; the colour is in clearRgba)
    // hrw41 (GXV_HRW41_DEPTH / GXV_HRW41_BOXKEY / the plane gate's mask): 1 on every draw the game made after PORT_GX_MARK_SCENE_DEPTH ("the 3D scene's depth is
    // complete": GameScene::draw3D, before the image effects and its own depth clear): the back end makes the pair's depth mask just before the first such record. 0 for every
    // record made with the switches off (the marker is not written then). In the padding after clearQ: DrawRec's size is unchanged.
    // halo31 (18 Nov): bit 0 is that mark. Bit 1 (2): the record is one rectangle that writes DEPTH ONLY over its whole extent (drawn under its cull mode, depth ALWAYS +
    // write, no colour, no alpha, no blend, no alpha test): MR::clearZBuffer's quad, which is no `cover` because it writes no colour. Read by the GXV_HRW_KEEPDEPTH plan only.
    uint8_t depthMark = 0;
    uint32_t clearRgba = 0; // r | g << 8 | b << 16 | a << 24
    // GXV_SILH_FUSE (silhfuse26; SilhFusePass in vgx_draw.cpp, vgx_backend_gl.cpp "silh fuse"): not 0 on the silhouette fill quad that follows the alpha capture CopyTexCmd::fuse
    // marks: fuseFs - 1 is the id of the fragment shader of its FUSED key (fsId with FsKey::setSilhFuse), the one the back end draws, unblended, in place of the hand-over's
    // draw-back quad AND this fill when the capture really was handed over; fsId and rs stay the ordinary fill's (the fallback, and what every other pass of the front end saw).
    uint16_t fuseFs = 0;
    // GXV_HRW (hrw27, vgx_hrw.h OwnerKey): the owner of the draw, as the game's PORT_GX_MARK_OWNER marker named it when the draw was made (id << 8 | phase; 0 = nobody's). The half-rate-world
    // replay removes the retained draws of an owner and puts the fresh ones of a partial draw in their place. Never read by the back end. (In the padding after fuseFs: DrawRec's size is unchanged.)
    uint16_t owner = 0;
    uint32_t ublock[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    RenderState rs;
    TexBind tex[8];
};

// ----------------------------------------------------------------- shaders
// Generated by the front end (vgx_shadergen.cpp), compiled by the back end.
// Immutable once created; ids are dense and never reused.
struct ShaderSource {
    bool fragment = false;
    uint16_t id = 0;
    uint64_t keyHash = 0; // stable hash of the structural key (for an on-disk binary cache)
    uint8_t texMask = 0;  // fragment: samplers used
    uint32_t attrMask = 0; // vertex: bit per AT_* location used
    bool usesPalette = false, usesLights = false;
    uint8_t numTexGens = 0;
    // Vertex: sizes (in vec4) of the uniform arrays the text declares; 0 = not declared.
    // The back end uploads at most this much (and at most what the GL reports as active).
    uint8_t nMv = 0, nTm = 0, nPtm = 0, nPal = 0, nLt = 0;
    uint8_t nTev = 28; // fragment: size of u_tev declared by this shader text
    // Fragment (GXV_FS_REGS, fsregs25): canonical UB_TEV row count of the draw's block, and when tevGather is true,
    // which canonical row (0..31) feeds each compacted u_tev[0..nTev-1] slot (gathered in the back end at upload).
    uint8_t nTevCanon = 28;
    bool tevGather = false;
    uint8_t tevSrc[32] = {};
    // Vertex (GXV_VSOUT, VsKey::setOut): colour varyings left out (bits 0, 1), a colour or alpha part not computed (bit 2), a texgen
    // written as a constant (bit 3). 0 for every key made with the switch off. (Census only: GXV_GPU_SCENES' line.)
    uint8_t outPruned = 0;
    // GXV_FS_TRIM (fragment: clamp sides and alpha-test floors left out) / GXV_VS_TRIM (vertex: light terms and clamp sides left out): how
    // many, at most 255. 0 for every shader made with the switches off. (Census only: GXV_GPU_SCENES' line.)
    uint8_t trimOps = 0;
    // Vertex (GXV_VTX_CENSUS, vertex25): the lights the text evaluates per channel (0: COLOR0 / ALPHA0, 1: COLOR1 / ALPHA1) and loop kind (0 colour part only,
    // 1 alpha part only, 2 both in one merged loop): ltLoop[channel * 3 + kind] has bit l set when light l has such a loop. Census only.
    uint8_t ltLoop[6] = {};
    // Fragment (GXV_FS_TEXREUSE, gpucut23): texture reads left out because `tex` already held that read. 0 with the switch off. (Census
    // only: the "gpucut23:" line.)
    uint8_t texReused = 0;
    // asyncsh31 (GXV_UBERSHADER=1, census only; vgx_ubershader.h): fragment, set when the text is made: 1 the ubershader takes the key, 2 it does not (uberWhy: the first
    // reason, a string literal), 0 not asked (the switch off, or a vertex shader). uberIface: the interface word (UberIface::Word) of a taken key.
    uint8_t uber = 0;
    const char* uberWhy = "";
    uint32_t uberIface = 0;
    // precomp26 (GXV_STAGE_ENUM): the text was first made by the enumeration's dry draw, not by a draw: the back end does not compile it at its command (a draw that needs it compiles it on demand).
    bool enumOnly = false;
    // Fragment (GXV_EFBSWAP2): the texture channels the shader reads, bit 4 * texmap + channel (0 r, 1 g, 2 b, 3 a), after the
    // TEV swap tables; an indirect lookup counts as all four. All bits set: not known (every channel read).
    uint32_t texChan = 0xFFFFFFFFu;
    std::string text;
};

// ---------------------------------------------------------------- commands
enum : uint8_t {
    TF_RGBA8 = 0, // 4 bytes per texel, R first, top row first
    TF_DXT1 = 1,  // S3TC DXT1 blocks (8 bytes each, row-major), as GL_COMPRESSED_RGBA_S3TC_DXT1_EXT takes them
    TF_LA88 = 2,  // 2 bytes per texel, luminance then alpha, sampling as (L,L,L,A); no mipmaps
    TF_DXT5 = 3,  // S3TC DXT5 blocks (16 bytes each, row-major): the HD replacements (vgx_hdtex.cpp), and CMPR with transparent texels (GXV_CMPRA)
    // GXV_TEX16 (from the Strikers port's 1.3 MSC_VGX_TEX16): 2 bytes per texel, little-endian 16-bit words, uploaded with internal format =
    // format so that vitaGL keeps the words as they are (textures.c fast_store) and picks the GXM 16-bit format
    TF_RGB565 = 4,   // GL_UNSIGNED_SHORT_5_6_5 (red in the top bits): SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB
    TF_RGBA5551 = 5, // GL_UNSIGNED_SHORT_5_5_5_1: U5U5U5U1_RGBA; never with genMips (vitaGL has no transfer format for its mips)
    TF_RGBA4444 = 6, // GL_UNSIGNED_SHORT_4_4_4_4: U4U4U4U4_RGBA
    // g30 Wave 5 (ported from Strikers 2.0 MSC_VGX_LA88=2): 1 byte per texel, sampling as (I,I,I,I) (Vita GL_INTENSITY / U8_RRRR)
    // for non-mipmapped GX I4 and I8 textures (halves VRAM + upload/fetch bandwidth vs TF_LA88; vitaGL patch 0042 fast_store).
    TF_I8 = 7,
};
struct TexUpload {
    uint32_t texId = 0;
    uint16_t width = 0, height = 0;
    uint8_t format = TF_RGBA8;
    // Level 0 first. Unless backend::Caps::mipClamp, a mipmapped texture carries the
    // full chain down to 1x1 (GLES2 has no GL_TEXTURE_MAX_LEVEL); with mipClamp only
    // the levels the game authored. `authored` says how many of `levels` those are.
    // genMips: only level 0 is given and the back end is to generate the rest
    // (backend::Caps::mipGen; RGBA8 only).
    uint8_t authored = 1;
    bool genMips = false;
    bool preserveI4Mips = false; // opt-in Vita lossless authored I4 chain, never generated
    bool player = false;         // mario24: a texture of Mario's or Luigi's (the HD pack's list, vgx::IsPlayerTexture), for GXV_PLAYER_LODBIAS / _ANISO
    bool autoMip = false;        // automip25 (GXV_AUTOMIP): the game declared this texture without levels; the chain here is made by the port, so it is sampled with the mip filter and bias of GXV_AUTOMIP_FILTER / _BIAS
    bool mirrorWrap = false;     // g30 Wave 7: MIRROR wrap active at upload; SGX543 requires SCE_GXM_TEXTURE_SWIZZLED for MIRROR (never leave linear via GXV_TEXUP_TINY_BYTES)
    std::vector<std::vector<uint8_t>> levels;
};
// Geometry that outlives the frame (a cached display list): created or
// replaced by CMD_BUF_UPLOAD, referenced by DrawRec::vbuf, released by
// CMD_BUF_FREE. Ids are small and reused.
struct BufUpload {
    uint32_t bufId = 0;
    std::vector<uint8_t> vtx;
    std::vector<uint16_t> idx;
};
struct EfbRect { int16_t x = 0, y = 0, w = 0, h = 0; };
struct ClearSpec {
    bool enable = false;
    bool color = true, alpha = true, depth = true;
    float rgba[4] = {0, 0, 0, 1};
    float z = 1.0f;
};
// How the EFB colour is converted when copied into a texture:
// out = M * vec4(rgb, a) + add, rows in mat[].
struct CopyTexCmd {
    staticcrowd::Tag crowd;
    uint64_t crowdDestination = 0;
    uint32_t crowdFormat = 0;
    uint32_t texId = 0;
    uint8_t gxFmt = 0xFF;        // (gpu27, GXV_SCENES25 census only) the GX copy format of the game's GXSetTexCopyDst (0x04 RGB565, 0x06 RGBA8, 0x27 A8 ...)
    uint32_t gxDest = 0;         // (gpu27, census only) the low 32 bits of the guest destination address: names the game's buffer in the trace
    EfbRect src;
    uint16_t dstW = 0, dstH = 0; // in guest texels (the back end may scale)
    bool depth = false;          // copy the depth buffer instead of colour
    bool skip = false;           // only the clear is needed (and, with fromPass, the end of the pass): nothing samples this copy before the
                                 // texture is copied to again, or the copy itself is made later from an atlas cell
    bool fromPass = false;       // the source is the offscreen pass that this copy ends (see PassCmd), not the EFB
    bool passMore = false;       // with fromPass: the next command is another copy of the same pass, which stays open for it
    bool fill = false;           // the source rectangle is known to hold the constant colour fillRgba: no read of the EFB is needed
    float fillRgba[4] = {};
    bool clearElided = false;    // clear.enable was switched off by the front end: the rectangle already held these values
    // GXV_EFBSWAP2 (MarkSwapCopies in vgx_draw.cpp): every draw (and atlas copy) of this frame that samples the copy before the
    // texture is written again reads only channels the conversion leaves as they are (mat row = that channel, add 0), so the
    // EFB's own colour buffer can stand for the converted copy. The back end converts it after all (RawConvert) before a later
    // frame's draw that reads another channel.
    bool swapOk = false;
    // GXV_EFBSWAP2_PREDICT (MarkSwapCopies): the copy has the swap's shape and its own frame's readers would allow it, but a draw
    // EARLIER in this frame read the texture's previous picture in a channel the conversion changes: the next frame will most
    // likely do the same and the back end would have to convert the buffer then (RawConvert). swapOk is left false (census only).
    bool swapPredictedNo = false;
    // GXV_INDCAP_SKIP (MarkTransientCopies in vgx_draw.cpp): the game marked this copy PORT_GX_MARK_TRANSIENT_COPY (its texture is
    // read only by this picture's later draws); with no such reader it is left out (skip) and the EFB is not split for it.
    bool transient = false;
    // GXV_SHADOWFETCH (gpu23b; ShadowFetchPass in vgx_draw.cpp): this is the silhouette fill's whole-EFB alpha capture and draw
    // fetchDraw - 1 (an index into CmdBuf::draws) its only reader, the fill quad. fetchMode 1: the back end checks the fetch fill on
    // this pair first (a calibration: the copy is made as usual); 2: the copy is left out (skip) and the back end draws that fill
    // with a program that reads the EFB's own alpha in its shader (no EFB split). fetchKey: the fill's program, TEV block, blend
    // and the copy's conversion (what a calibration holds for); fetchRect: the fill quad's corners in clip space (x0, y0, x1, y1).
    uint8_t fetchMode = 0;
    uint32_t fetchDraw = 0;
    uint64_t fetchKey = 0;
    float fetchRect[4] = {};
    // GXV_SHADOW_RECT (shadowpx23; ShadowFillPass in vgx_draw.cpp, CopyTex in vgx_backend_gl.cpp): 1 when this is a whole-EFB 1:1
    // copy that reads only the EFB's alpha, adds nothing, and the EFB's alpha is known to be exactly 0 outside the EFB rectangle nz
    // (EFB pixels, inside src): every texel of the result whose bilinear read stays outside nz is then exactly 0. The back end copies
    // only nz (plus that read's margin) united with what the texture may still hold other than 0 from its own last such copy, and
    // leaves the rest of the texture as it is (0 there already): the texture ends up equal to the whole copy, bit for bit.
    uint8_t nzOn = 0;
    EfbRect nz;
    // GXV_SHADOW_STENCIL: 1 when this is the silhouette fill's whole-EFB alpha capture that the stencil replaces: skip is set and the fill (the
    // next draw, stOp ST_TEST, its texture 0) is drawn where the stencil is not 0. The back end made the copy after all, and the fill
    // draws from it (texId), when it cannot use the stencil (multisampled EFB, a surface without stencil, an effects pass in the window).
    uint8_t stWin = 0;
    // GXV_SILH_FUSE (silhfuse26; SilhFusePass in vgx_draw.cpp): 1 on the whole-EFB alpha capture whose swap (GXV_EFBSWAP2 hand-over) may leave out the draw-back quad because the next
    // draw, fuseDraw - 1 (an index into CmdBuf::draws), is the silhouette fill with a fused key (DrawRec::fuseFs). The back end defers the draw-back to that draw and draws the fused
    // key in its place; any other command first, or a draw it cannot make, draws the quad as before. 0 (always, with the switch off) changes nothing.
    uint8_t fuse = 0;
    uint32_t fuseDraw = 0;
    // Not 0: the source is rectangle `src` of this texture (srcTexW x srcTexH, same units as
    // `src`), not the EFB: a picture of a copy atlas (PassCmd::atlasTex). Such a copy ends
    // no pass and has no clear.
    uint32_t srcTex = 0;
    uint16_t srcTexW = 0, srcTexH = 0;
    // Not 0: the destination is the rectangle (dstX, dstY, dstW, dstH) of texture texId,
    // which is dstTexW x dstTexH (a cell of a copy atlas), not a texture of dstW x dstH.
    uint16_t dstTexW = 0, dstTexH = 0;
    int16_t dstX = 0, dstY = 0;
    float mat[4][4] = {};
    float add[4] = {};
    ClearSpec clear;
    // GXV_SCENES25_DRY (scenes25; MarkTransientCopies in vgx_draw.cpp): this copy was marked like the game's "Indirect" capture by its
    // SHAPE (CopyShapeClass), not by the game: with no reader in the picture it is left out under GXV_INDCAP_SKIP's rules (transient is
    // set too). Never taken by GXV_INDCAP_HALFRATE or GXV_PREVCAP, which are for the Indirect capture alone. false: nothing changes.
    bool dry = false;
    // GXV_SILH_GATE (shadow25; SilhGatePass in vgx_draw.cpp): the game's hint written just before this copy (PORT_GX_MARK_SILH_HINT): 0 none, 1 the player's
    // silhouette cannot show in this picture, 2 it may. 0 for every copy made with the switch off.
    uint8_t silhHint = 0;
    uint8_t depthMark = 0; // (hrw41: as DrawRec::depthMark, when a copy and not a draw is the first thing after the game's PORT_GX_MARK_SCENE_DEPTH)
    uint8_t bracket = 0;   // (bloom37, GXV_BLOOM_BRACKET; BloomBracketPass in vgx_draw.cpp) not 0: the game marked this copy as the normal bloom's scene capture; the value is the bracket's id in the picture (1..63)    // GXV_CORNERPASS (imp45; ShadowFillPass in vgx_draw.cpp, PlanCornerPass in vgx_backend_gl.cpp): not 0 when this is a small copy that reads only the EFB's alpha and the
    // front end's alpha bookkeeping (N) shows the alpha over its source and a margin of 3 EFB pixels to be exactly 0 just before draw cornerFrom - 1 (an index into
    // CmdBuf::draws), with only draws between that draw and the copy able to change it. The back end may then make the copy from a pass of its own at the picture's
    // start (those draws again, on alpha 0) and not cut the EFB's scene for it. 0 (always, with the switch off) changes nothing.
    uint8_t cornerPass = 0;
    uint32_t cornerFrom = 0;
    // (fix47) 1: the "0 before cornerFrom" rests on a GXV_SHADOW_STENCIL window's fill (the fill wrote 0 wherever the window's volumes had left alpha: vgx_draw.cpp
    // "the fill's certificate"). The back end then makes the pass only when that window's fill is drawn (not left out by its fallback).
    uint8_t cornerFill = 0;
};
// scenes25: the capture classes, told from a copy's shape (what the game's code makes of them is in agents/scenes25/REPORT.txt).
enum CopyClass : uint8_t {
    CC_INDIRECT = 0, // the game's "Indirect" screen capture (marked by the game: transient)
    CC_SHADOWA,      // whole-EFB alpha-only copy 1:1: the silhouette / shadow-volume alpha capture (ScreenAlphaCapture)
    CC_CORNER,       // alpha-only tile at the top of the EFB, not at x 0: the player's drop-shadow texture (CollisionShadow, 160x160 at 448,0)
    CC_CLIPMASK,     // whole-EFB alpha-only copy, box-halved: the clip area's hard mask (FallOutFieldDraw tex2)
    CC_ATILE,        // other alpha-only tiles: the clip area's soft-edge chain (tex3, tex4)
    CC_COLWHOLE,     // whole-EFB colour copy 1:1 (the normal bloom's scene copy, filters, DoF)
    CC_COLHALF,      // whole-EFB colour copy, box-halved (bloom's half copy, DoF)
    CC_COLTILE,      // colour tiles (the bloom / blur chains' small captures)
    CC_PASS,         // taken from an offscreen pass (ends the pass, not the EFB's scene)
    CC_ATLAS,        // into or out of a copy atlas
    CC_DEPTH,        // depth copy
    CC_COUNT
};
inline CopyClass CopyShapeClass(const CopyTexCmd& c, int efbW, int efbH) {
    if (c.srcTex || c.dstTexW) return CC_ATLAS;
    if (c.depth) return CC_DEPTH;
    if (c.fromPass) return CC_PASS;
    if (c.transient && !c.dry) return CC_INDIRECT;
    const bool whole = c.src.x == 0 && c.src.y == 0 && c.src.w == efbW && c.src.h == efbH;
    const bool oneToOne = c.dstW == c.src.w && c.dstH == c.src.h;
    const bool alphaOnly = c.mat[3][3] == 1.0f && c.mat[0][3] == 1.0f && c.mat[0][0] == 0.0f; // (A8: every channel = the EFB's alpha)
    if (alphaOnly) {
        if (whole) return oneToOne ? CC_SHADOWA : CC_CLIPMASK;
        if (c.src.y == 0 && c.src.x > 0 && oneToOne) return CC_CORNER;
        return CC_ATILE;
    }
    if (whole) return oneToOne ? CC_COLWHOLE : CC_COLHALF;
    return CC_COLTILE;
}
constexpr int kCharaBoxes = 16;
struct CopyDispCmd {
    EfbRect src;
    ClearSpec clear;
    bool clearElided = false; // as in CopyTexCmd
    // GXV_CHARAA (vgx_draw.cpp, "character edge smoothing"): the EFB rectangles (EFB pixels, inside `src`) where the 3D
    // scene's characters and key items were drawn, the HUD's draws cut out. The present smooths the edges there only
    // (backend::PresentCharaAA). 0: nothing to smooth, no pass.
    uint8_t charaN = 0;
    EfbRect chara[kCharaBoxes];
    // GXV_EDGE_AA (vgx_draw.cpp, "whole-picture edge smoothing"): the present smooths the whole picture (backend::PresentEdgeAA)
    // and then draws these rectangles (EFB pixels, the HUD's draws, padded and merged) again plainly over it, so no HUD pixel is
    // ever filtered. edge false: no whole-picture pass for this copy (switch off, or the HUD covers too much of it).
    bool edge = false;
    uint8_t hudN = 0;
    EfbRect hud[kCharaBoxes];
    uint8_t hudBigN = 0;  // (warp32 PATCH13) with hudState 2: hud[0..hudBigN) hold the rectangles all the same and hudBigPct their share of the copy (hudN stays 0: the present's passes do not use them)
    uint8_t hudBigPct = 0;
    uint8_t hudState = 0; // (hrw31, GXV_HRW=5) 0: the rectangles were not collected, 1: hud[0..hudN) are the HUD's (none: no HUD draw), 2: the HUD covers too much of the copy (a menu): no warp
    // mario24 (GXV_EDGE_AA_PLAYER, GXV_PLAYER_SHARPEN): the EFB rectangles of the player's draws (the HUD cut out, padded), where the present
    // draws its own pass over the edge-smoothed picture (backend::PresentFastDraw). 0: none (the present is as before).
    uint8_t playerN = 0;
    EfbRect player[kCharaBoxes];
    // GXV_PREVCAP (prevcap25; PrevCapPass in vgx_draw.cpp): not 0 = the front end has pointed readers of the NEXT picture at this texture id:
    // the back end gives it the picture just shown (the EFB's own colour buffer handed to the texture when the swap can be made, an ordinary
    // whole-EFB copy otherwise), after the present and before the clear. 0 (default): nothing happens.
    uint32_t keepTex = 0;
};
// An offscreen pass, found by the front end (IsolatePasses in vgx_draw.cpp).
// The draws between CMD_PASS_BEGIN and the next CMD_COPY_TEX (which has
// `fromPass` set and ends the pass) only touch `rect` of the EFB; every plane
// of that rectangle which they or the copy read held the constant value given
// here when they began; and nothing ever looks at what they leave in the EFB.
// A back end may therefore draw them into a separate target of the
// rectangle's size that starts cleared to these values, take the copy from
// that target, and leave the EFB alone (the copy's own clear still applies to
// the EFB rectangle). On a tile-based GPU that turns "store the EFB, copy,
// reload the EFB" into one small scene that needs no previous contents.
struct PassCmd {
    staticcrowd::Tag crowd;
    EfbRect rect;
    float rgba[4] = {0, 0, 0, 0};
    float z = 1.0f;
    uint8_t planes = 7; // 1 colour, 2 alpha, 4 depth: the planes the pass uses at all (the others may hold anything)
    uint8_t known = 7;  // of those: the planes whose value at the start matters (and is given above)
    // Not 0: the copy that ends the pass is 1:1 with no conversion into this
    // texture, and no draw of the pass samples it, so the pass may be drawn
    // straight into the texture and the copy needs no work at all.
    uint32_t directTex = 0;
    // Not 0 (only with directTex): the picture does not get a texture of its own. It is drawn
    // into a cell of this atlas texture (atlasW x atlasH; the cell's picture starts at
    // atlasX, atlasY and has the pass rectangle's size; all in EFB pixels), which is cleared
    // to the pass's values, together with a border of atlasGutter pixels around it. The front
    // end has pointed every draw that samples the picture at the atlas (AtlasPasses in
    // vgx_draw.cpp). Many small pictures then share one render target, and passes that follow
    // each other into the same atlas are one scene for the GPU instead of one each.
    // atlasGutter: the border to clear with the cell (0: it holds the clear colour already).
    // Without directTex the cell is only the pass's scratch: the copy that ends the pass has
    // `skip` set and a later copy (CopyTexCmd::srcTex) reads the cell.
    uint32_t atlasTex = 0;
    uint16_t atlasW = 0, atlasH = 0, atlasX = 0, atlasY = 0;
    uint8_t atlasGutter = 0;
};
struct SetTargetCmd {
    uint16_t efbW = 640, efbH = 528;
};

enum CmdType : uint8_t { CMD_DRAW, CMD_TEX_UPLOAD, CMD_TEX_FREE, CMD_SHADER, CMD_COPY_TEX, CMD_COPY_DISP, CMD_PRESENT, CMD_SET_TARGET,
                         CMD_BUF_UPLOAD, CMD_BUF_FREE, CMD_PASS_BEGIN,
                         CMD_PROGRAM, // arg = vsId << 16 | fsId: this pair will be drawn with; link it now (shader warm-up list)
                         CMD_VARIANT, // arg = index into variants (shader warm-up list)
                         CMD_TYPE_COUNT };
struct Cmd {
    CmdType type;
    uint32_t arg; // index into the matching vector below (texId for CMD_TEX_FREE, bufId for CMD_BUF_FREE)
};

// For the frame arenas: resize() leaves new elements uninitialised (a std::vector<uint8_t>
// zero-fills them, and the vertex arena is grown and then written by the decoder for every
// draw: half a megabyte of memset per frame for nothing).
// strk26 (GXV_ARENA_SOFT, vgx_arena.h; after the Strikers port's MSC_VGX_ARENA_SOFT): a block the calling code got beforehand, by a request that may fail without
// ending the program, is what the next allocate() of exactly that size returns; the vector's reserve() then copies into it as always. One writer (the front end
// builds a frame on one thread); not thread_local (the console's emulated TLS costs a call).
struct NoInitPending { void* block = nullptr; size_t bytes = 0; };
inline NoInitPending& NoInitPendingBlock() { static NoInitPending p; return p; }
template <typename T> struct NoInitAlloc {
    using value_type = T;
    NoInitAlloc() = default;
    template <typename U> NoInitAlloc(const NoInitAlloc<U>&) {}
    T* allocate(size_t n) {
        NoInitPending& p = NoInitPendingBlock();
        if (p.block && p.bytes == n * sizeof(T)) { T* r = static_cast<T*>(p.block); p.block = nullptr; p.bytes = 0; return r; }
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, size_t) { ::operator delete(p); }
    template <typename U> void construct(U* p) { ::new (static_cast<void*>(p)) U; }
    template <typename U, typename A0, typename... A> void construct(U* p, A0&& a0, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A0>(a0), std::forward<A>(a)...);
    }
    template <typename U> bool operator==(const NoInitAlloc<U>&) const { return true; }
    template <typename U> bool operator!=(const NoInitAlloc<U>&) const { return false; }
};

// The uniform arena: grown by PushBlock and written right after, so not zero-filled (also the front-end split's arenas).
using UniArena = std::vector<float, NoInitAlloc<float>>;

struct CmdBuf {
    std::vector<Cmd> cmds;
    std::vector<DrawRec> draws;
    std::vector<TexUpload> uploads;
    std::vector<const ShaderSource*> shaders;
    std::vector<CopyTexCmd> copyTex;
    std::vector<CopyDispCmd> copyDisp;
    std::vector<SetTargetCmd> targets;
    std::vector<BufUpload> bufUploads;
    std::vector<PassCmd> passes;
    std::vector<uint8_t, NoInitAlloc<uint8_t>> vtx;
    std::vector<uint16_t, NoInitAlloc<uint16_t>> idx;
    UniArena uni;
    // Start (byte offset in vtx) of each chunk of the vertex arena, ascending, first 0.
    // A chunk goes into a vertex buffer of its own, and the dynamic draws of a chunk
    // address their vertices from the chunk's start (DrawRec::vchunk / vtxFirst).
    // Empty: one chunk, draws carry byte offsets (the old scheme).
    std::vector<uint32_t> vtxChunks;
    std::vector<VariantCmd> variants;
    uint64_t frame = 0; // presented-frame number this buffer belongs to (diagnostics)
    bool presentAllowed = true; // immutable while this buffer is owned by the backend
    uint32_t presentStamp = 0;  // GXV_PRESENT_SYNC: 0 none, else 1 << 31 | the vblank (low 16 bits) of the turn that drew it
    // GXV_HRW (hrw26, vgx_hrw.h): hrwId != 0 on a picture the game opened a "half-rate world" pair with (PORT_GX_MARK_HRW, the pair's number): the render thread keeps this
    // buffer after executing it, for the replay. hrwReplayId != 0 on a tiny job (no commands) that asks the render thread to execute the retained buffer of that pair again
    // with the camera change hrwD (3x4, row major) and presentStamp as its stamp.
    uint32_t hrwId = 0;
    bool hrwNoFresh = false;
    void setHrwPair(uint32_t token) {
        hrwId = token & 0xffffu;
        hrwNoFresh = hrwId && (token & hrwpair::kPlainToken);
    }
    uint32_t hrwReplayId = 0;
    // GXV_HRW (hrw27): on a replay job that also carries the pair's partial draw (the fresh draws of Mario and the significant actors, made for the pair's second picture with its own camera): the job's
    // draws replace the retained buffer's draws of the same owners (vgx_hrw.h Compose); its resource commands (uploads, shaders ...) run whether or not the replay does.
    bool hrwFresh = false;
    float hrwD[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // The shader warm-up list's replay (CMD_SHADER / CMD_PROGRAM / CMD_VARIANT only) when the host has asked for it to be done in
    // slices over the frames that follow instead of in this one (SetShaderWarmupDeferred). They are kept apart from `cmds` so that
    // the passes that insert and remove commands there cannot move them.
    std::vector<Cmd> warm;
    bool crowdActive = false, crowdInvalid = false, crowdReset = false;
    uint32_t crowdObservations = 0, crowdTags = 0;

    bool empty() const { return cmds.empty(); }
    // Host memory this buffer holds (capacities: a cleared buffer keeps what its largest frame needed).
    size_t HostBytes() const {
        size_t n = cmds.capacity() * sizeof(Cmd) + draws.capacity() * sizeof(DrawRec) + uploads.capacity() * sizeof(TexUpload) +
                   shaders.capacity() * sizeof(void*) + copyTex.capacity() * sizeof(CopyTexCmd) + copyDisp.capacity() * sizeof(CopyDispCmd) +
                   targets.capacity() * sizeof(SetTargetCmd) + bufUploads.capacity() * sizeof(BufUpload) + passes.capacity() * sizeof(PassCmd) +
                   vtx.capacity() + idx.capacity() * 2 + uni.capacity() * 4 + vtxChunks.capacity() * 4 + variants.capacity() * sizeof(VariantCmd);
        for (const TexUpload& u : uploads) for (const auto& l : u.levels) n += l.capacity();
        for (const BufUpload& b : bufUploads) n += b.vtx.capacity() + b.idx.capacity() * 2;
        return n;
    }
    void clear() {
        presentAllowed = true;
        presentStamp = 0;
        setHrwPair(0);
        hrwReplayId = 0;
        hrwFresh = false;
        cmds.clear(); draws.clear(); uploads.clear(); shaders.clear(); copyTex.clear();
        copyDisp.clear(); targets.clear(); bufUploads.clear(); passes.clear(); vtx.clear(); idx.clear(); uni.clear();
        vtxChunks.clear(); variants.clear();
        crowdActive = crowdInvalid = crowdReset = false;
        crowdObservations = crowdTags = 0;
        warm.clear();
    }
};

// ---------------------------------------------------------------- back end
// Implemented by vgx_backend_gl.cpp; called on the thread that owns the GL
// context (see vgx_host_egl.cpp).
namespace backend {
int ExportCountLog(); // Paused consumer; no GL calls or mutable render-state access.
uint64_t Cache24Hints(); // (GXV_CACHE24) the cache hints the back end's draw loop has issued since the start
bool Init(int renderWidth, int renderHeight); // 0,0 = follow the EFB size
void Execute(const CmdBuf& buf);
// GXV_HRW (hrw26, vgx_hrw.h): the render thread's three calls. HrwRetainRun(true) before Execute of a buffer that will be kept: its CMD_TEX_FREE / CMD_BUF_FREE are not carried
// out but listed; HrwFlushDeferred() carries the list out (before the next ordinary buffer, and after a replay); HrwReplay(buf) is Execute(buf) for a buffer that was
// executed before: its resource commands (uploads, frees, shaders, programs, variants) and its warm-up list are skipped, everything else runs as it did.
void HrwRetainRun(bool on);
void HrwFlushDeferred();
unsigned HrwDeferredCount();
void HrwReplay(const CmdBuf& buf);
// GXV_HRW (hrw27): the resource commands of a partial draw's buffer (uploads, shaders, programs, variants, buffer uploads), nothing else; its frees are listed for HrwFlushDeferred.
void HrwRunResources(const CmdBuf& buf);
// GXV_HRW=5 (hrw31, "warp"): the second picture of a pair is the first one's finished image, re-projected to the second picture's camera in one full-screen pass.
// WarpArm(on, id): the render thread, before an ordinary buffer: this picture (pair `id`) leaves its image (colour, and the depth as a log code in alpha) in a texture at its display copy.
// WarpPrepare(d, id, truthAvailable, truthRun): the render thread, for the pair's ask (d = V_B * V_A^-1, 3x4 row major): the small buffer (one display copy) to execute with HrwReplay, whose copy draws the warp
// into the EFB first; null = no warp picture (counted by reason: the first picture stays on the screen). A pair chosen for the stills (GXV_HRW_WARP_DUMP, and the kept first buffer is there: truthAvailable) sets *truthRun and
// returns null: the caller shows the true redraw, and the back end writes A, B, B0 (without depth) and T after its present.
void WarpArm(bool on, uint32_t id, bool noFresh);
// WarpFresh(ask): the render thread, after a successful WarpPrepare for an ask that carries the pair's partial draw (GXV_HRW_WARP_FRESH): the buffer to execute
// (the ask itself with the display copy appended, or the plain warp buffer).
const CmdBuf* WarpFresh(CmdBuf* ask);
// mariolag31 (GXV_HRW_MARIOTIME, port/mario_time.h): after WarpPrepare for this ask, is the player in the picture (the box's centre in front of the camera)?
// WarpDrop(): the buffer WarpPrepare (and WarpFresh) gave is NOT executed after all: no second picture for this pair (the first stays on the screen).
bool WarpHasPlayer();
void WarpDrop();
// WarpPlayer(id, ok, a, b): the game thread, before the pair's ask is queued: the player's centre in the first (a) and the second (b) picture's view space (ok false: none).
void WarpPlayer(uint32_t id, bool ok, const float a[3], const float b[3]);
// WarpPlayer36(id, bits, feet, up, ground, normal): the game thread, right after WarpPlayer for the same pair (warp36, port/hrw36.h): the player's feet, his up vector, the
// ground point under him and the ground polygon's normal, all in the SECOND picture's view space; bits as port_hrw_player36 returns them (1 player, 2 ground, 4 normal, 8 the ground is the one remembered through the air: hold36, 16 his shadow's point out of range).
void WarpPlayer36(uint32_t id, int bits, const float feet[3], const float up[3], const float ground[3], const float normal[3]);
const CmdBuf* WarpPrepare(const float d[12], uint32_t id, bool truthAvailable, bool* truthRun);
struct WarpStats {
    uint64_t snaps = 0, snapsDepth = 0, asked = 0, warped = 0, fell[16] = {},  /* (warp36: 12 -> 16, a reason more) */ edgeHist[6] = {}, edgeN = 0, dumps = 0;
    double edgeSum = 0.0, edgeMax = 0.0, snapMs = 0.0, drawMs = 0.0;
    int w = 0, h = 0, depthMode = 0;
};
void WarpTakeStats(WarpStats& out, bool reset);
const char* WarpReasonName(int r);

// Present hook: the host layer decides what "present" means (swap, PNG dump).
// Returns the last display copy as tightly packed RGBA8, top row first.
const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height);
void RequestCapture(bool enable); // read back the next display copy
// Optional present hook for hosts that show the picture themselves (instead
// of asking for a capture and intercepting the read-back). Called on the back
// end's thread at every display copy, before the copy's clear, with the EFB
// framebuffer object still bound: its name, and the copy rectangle in
// render-target pixels. The EFB is stored top row first: GL row 0 is the top
// row of the picture. Return true only when the hook transitioned away from the
// EFB scene, so the next EFB use starts a new scene. Suppressed presents return
// false. This is a scene-boundary certificate, not GPU completion.
using PresentHook = bool (*)(unsigned efbFramebuffer, int x, int y, int width, int height);
void SetPresentHook(PresentHook hook);
// GXV_HUD_NATIVE (hud24, vgx_backend_gl.cpp "GXV_HUD_NATIVE"): the 2D layer drawn at the screen's own resolution over the upscaled picture.
// A host whose present hook can be called in two halves says so (screen size in pixels; 0, 0 = it cannot, the default). The back end then
// calls the hook TWICE for a picture it takes natively, the first time in the middle of the frame, at the first draw of the picture's 2D tail:
//   PresentPhase() == 1: draw the picture into the display (the upscale, the characters' edge smoothing: everything the hook does but the
//     swap), then call SetHudNativeTarget (where it went), and return true. Return false having done NOTHING (no draw, no state change, no counter) when the picture is not
//     to be shown: the back end then goes on as before and calls the hook as a whole (phase 0) at the display copy. Leave `fbo` bound.
//     No swap, no bookkeeping that belongs to the swap.
//   PresentPhase() == 2 (the display copy at the end of the frame): the swap and what follows it; nothing is drawn.
// Phase 0 is the ordinary whole present (every picture with the switch off, and every picture the back end falls back for).
// The back end draws the 2D draws into `fbo` between the two calls (fbo 0 on the console: the display's own scene, so no scene is added).
// (x, y, w, h): the rectangle of the display `fbo` the picture was drawn into (bottom row first when flipY, the console's display), (screenW,
// screenH) the whole target (draws outside the rectangle are scissored to the screen).
void SetHudNativeHost(int screenW, int screenH);
int PresentPhase();
// The display `fbo` of SetHudNativeTarget has a depth buffer that the tail may test: draws of the 2D layer that test depth (3D models inside the
// layouts) then go into the display too, after a clear of its depth at the tail's start. Not called (the default): they end the tail as before.
void SetHudNativeDepth(bool displayHasDepth);
void SetHudNativeTarget(unsigned fbo, int x, int y, int w, int h, bool flipY);
// GXV_HUD_NATIVE census since the start (tests, the host's log): out[0] pictures seen, [1] taken natively, [2] taken with part of the 2D left in
// the EFB, [3..] fell back by reason (HudNativeReasonName(i)), see the back end.
constexpr int kHudNativeReasons = 16;
// GXV_TAILFOLD=3 (tailfold39) census since the start (tests): out[0] pictures seen, [1] foldable pictures (armed), [2] folded (the present was made from the put-back's source), [3] armed but declined at run time.
void TailFoldCounters(uint64_t out[4]);
void HudNativeCounters(uint64_t out[kHudNativeReasons + 4]); // [0] pictures, [1] taken, [2] partial, [3] tail draws, [4..] fallbacks by reason
const char* HudNativeReasonName(int reason);
// Automatic quality (dynamic resolution, vgx_autoq.h): a host whose present hook swaps calls this right after the
// swap, on the back end's thread. The GPU clock then knows the frame's last scene and times its completion.
void AutoQualitySwapped();
void NoteSwapUs(unsigned swapUs); // (seam68: the host's vglSwapBuffers took this long)
// The quality rung (vgx_autoq.h) to use from the next frame on, whatever the controller says (it is stopped): tests, or a
// host that offers a fixed choice. Back-end thread, between frames.
void ForceQualityRung(int rung);
// The highest rung of the quality ladder this platform can show (8 with 4x multisampling of the EFB, 7 with 2x, 6 without).
int QualityTopRung();
// (tests) Floor protection by degrees (vgx_autoq.h) set by hand for the next frame: effects and blur passes at half size, the
// cheaper background. The controller stops being the one to set them only if it is off (tests run without one).
void DebugSetQualitySteps(bool fxHalf, bool cheapBackground);
// (tests) The present without its edge smoothing (GXV_AUTOQ_EDGESTEP's step), as the controller would set it; latched at the next frame.
void DebugSetEdgeStep(bool off);
// Anisotropic filtering as the top step of the quality ladder (vgx_autoq.h, GXV_AUTOQ_ANISO). The host that has vitaGL's anisotropic
// switch (a build with ANISO_SWITCH: vglSetAnisotropicFilter) hands it over before the first frame; the back end calls it on its own
// thread between frames with the minification filter value to use for trilinear textures (2 or 3), or 0 for none, and sets every
// trilinear texture's minification filter again when it is next bound (the switch acts when a filter is set, not at a draw).
// Without a hook GXV_AUTOQ_ANISO is ignored.
using AnisoHook = void (*)(unsigned filter);
void SetAnisotropicHook(AnisoHook hook);
// GXV_VGL_LOADFROM (vitaGL patches/0019, COLOR_LOAD_FROM): asks the GL to make the next scene begun on the framebuffer bound for
// drawing load its colour from texture `tex` (same size and format) instead of its own buffer; true when armed. The host sets it
// (vglColorLoadFrom); without one the back end draws the EFB's old picture back with a quad as before.
using ColorLoadHook = bool (*)(unsigned tex);
void SetColorLoadHook(ColorLoadHook hook);
// GXV_COLOR_LOADSKIP (vitaGL patches/0021, COLOR_LOAD_FROM): asks the GL to make the next scene begun on the framebuffer bound for
// drawing (arm = true) load its colour from a tiny texture instead of from its own 2.1 MB / 1.2 MB buffer, or takes the request back
// (arm = false, called right after the clear that began the scene); true when armed. Set by the host (vglColorLoadSkip); without one
// nothing is asked and the scenes load as before.
using ColorSkipHook = bool (*)(bool arm);
void SetColorSkipHook(ColorSkipHook hook);
// GXV_VGL_VALIDREGION (vitaGL patches/0028, VP26_VALID_REGION; scenes23): asks the GL to begin the next scene on the framebuffer bound for
// drawing with a GXM valid region (x0, y0) - (x1, y1), render pixels of that target, inclusive: only the tiles it touches are processed
// (loaded, shaded, stored). The back end asks it only for a scene none of whose pixels outside the region is ever read (the GXV_HOIST_GRID
// pass). True when armed. Set by the host (vglSetNextSceneValidRegion); without one nothing is asked and the whole target is processed.
// GXV_COLOR_LOADSKIP bit 16 (vitaGL patches/0028, VP26_SKIP_COLOR; scenes23): as ColorSkipHook, but the cheap load is a texture of one colour
// (RGBA8 bytes, byte 0 red; each byte 0 or 255): asked for a scene that begins with a clear of its whole target to that colour, which then
// starts with exactly what the clear writes. True when armed (the host's vglColorLoadSkipColor); disarmed with ColorSkipHook(false).
using ColorSkipColorHook = bool (*)(bool arm, uint32_t rgba);
void SetColorSkipColorHook(ColorSkipColorHook hook);
using ValidRegionHook = bool (*)(int x0, int y0, int x1, int y1);
void SetValidRegionHook(ValidRegionHook hook);
// (tests) GXV_HOIST_GRID: corner copies made from a target on the EFB's own pixel grid so far.
unsigned DebugHoistGrids();
unsigned DebugCornerPasses(); // (imp45, GXV_CORNERPASS: corner copies made from a pass at the picture's start so far)
// (tests) GXV_SHADOW_RECT: alpha captures copied in part / whole (the record kept or started) so far.
void DebugShadowRect(uint64_t out[2]);
// (tests, lowres25) GXV_FXLOWRES=3, the census since the start: [0] pictures measured; thousandths of a screen summed over them:
// [1] far 3D effect candidates, [2] near large-sprite, [3] near, [4] flat, [5] sorted; [6] far draws, [7] static candidates,
// [8] pictures with a net saving at a quarter of the pixels.
void DebugFxLowres(uint64_t out[9]);
// (tests, lowres25) GXV_FXLOWRES=1: [0] pictures decided, [1] runs drawn at half size, [2] their draws, [3] of them static, [4] runs left at full size.
void DebugFxLowresRender(uint64_t out[5]);
// (tests, lowres25) GXV_FXLOWRES_CEILING (a diagnostic that leaves a class of blended draws out): draws left out since the start.
uint64_t DebugFxLowresCeiling();
void DebugFxAuto(uint64_t out[8]);  // compositor25: the automatic choice's counters (see the back end)
void DebugFxAutoReset();       // a stage load, for the tests
void DebugFxStage(uint64_t out[8]);  // compositor25 round 3: the stage line's counters: lines logged, pictures, unable, without a candidate, with a run, windows
void DebugFxDefer(uint64_t out[8]);  // compositor25 round 2: the deferral's counters (see the back end)
// (tests, gpucut23 GXV_FIRSTCLEAR) since the start: clear quads taken as a whole-depth overwrite, EFB depth loads that left out
void DebugFirstClear(uint64_t out[2]);
// (tests) The last frame's scenes by kind: EFB, copy, pass, GXV_HOIST_GRID pass, display (as the back end began them).
void DebugLastFrameSceneKinds(unsigned kinds[5]);
// (tests) GXV_VGL_LOADFROM: GXV_EFBSWAP buffers handed to the next scene's colour load instead of a quad.
unsigned DebugEfbLoadFroms();
bool AnisotropicOn(); // in force for the frame being drawn
// (tests) Anisotropic filtering set by hand for the next frame (filter 2 or 3): as the controller's, through the same hook.
void DebugSetAnisotropic(bool on, unsigned filter = 2);
// The rung of the quality ladder now.
int QualityRung();
void QualityRungSize(int& w, int& h, int& samples); // the internal picture now (render target size, EFB samples)
// (tests) The nth check from now that a new render target or texture got its memory reports none (a simulated console
// allocation failure; 0: none).
void DebugFailAllocation(int nth);
// (tests) Render textures of the last change of size: kept by resampling, and resized without their picture (no memory).
void DebugLastChange(unsigned& resampled, unsigned& plain);
// Samples a pixel of the EFB target now (1: not multisampled).
int EfbSamples();
// (tests) A triangle of one colour over the lower left half of the EFB target, drawn now (an edge across every row).
void DebugDrawEfbTriangle(const float rgba[4]);
// (tests) GXV_EFBSWAP copies made so far.
unsigned DebugEfbSwaps();
// (tests) Of those, GXV_EFBSWAP2's (a conversion the samplers do not see), and the ones converted later (RawConvert).
unsigned DebugEfbSwapsPartial();
unsigned DebugRawConverts();
// (tests) GXV_EFBSWAP2_A1: swapped buffers given the alpha-1 sampler format, and the conversions that avoided (a reader of the alpha met one).
unsigned DebugA1Swaps();
unsigned DebugA1Avoided();
// (tests) GXV_EFBSWAP2_HOLD: conversion swaps refused because their texture needed a later conversion recently.
unsigned DebugEfbSwapHolds();
// (tests) EFB buffers that became a texture's own storage at an ordinary copy into it (no allocation: AdoptEfbBuffer).
unsigned DebugEfbAdopts();
unsigned DebugDisplayHeldByPlainTextures(); // (pace25) display textures that plain textures hold as their own storage
unsigned DebugDynReturned();                // (pace25) display textures AdoptEfbBuffer gave back to the spares (GXV_DD_DYN)
// (tests) GXV_RT_DEFER: GL names of textures the GPU drew into that wait to be deleted, those handed over and deleted so far,
// the frees of storage drawn into within 4 presents (immediate deletes), and the GL name the back end's texture texId has now.
void DebugRtDefer(unsigned& waiting, uint64_t& handed, uint64_t& deleted, uint64_t& earlyFrees);
unsigned DebugTexName(uint32_t texId);
// The display copy's picture (rectangle x, y, w, h of the EFB target, render pixels) drawn into framebuffer `fbo` at
// (dx, dy, dw, dh) with bilinear filtering and an unsharp mask of `amount` (after the Strikers port's PIC_SMOOTH); flipY:
// the quad's bottom edge shows the picture's last row. False when the shader did not build (no GL state touched: the
// host blits instead). For the host's present hook; nothing cached is trusted afterwards (as after the hook).
bool PresentSharpened(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY);
// The amount for a picture of w x h shown at dw x dh (GXV_SHARPEN=auto|off|<amount>; auto follows the upscale).
float SharpenAmount(int w, int h, int dw, int dh);
// GXV_CHARAA: right after PresentSharpened (or the host's blit, with amount 0) into the same framebuffer, with the same
// arguments, inside the present hook: the edges of the characters and key items of the display copy being presented
// (CopyDispCmd::chara) are smoothed, in the display's own scene. Returns the rectangles drawn (0: nothing was drawn, nothing
// of the GL state touched: no rectangles, a multisampled EFB, or the shader did not build).
int PresentCharaAA(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY);
// GXV_EDGE_AA: called by the present hook INSTEAD of PresentSharpened / the host's blit and PresentCharaAA, with the same
// arguments: when this display copy is to be smoothed whole (CopyDispCmd::edge, the EFB not multisampled, the shaders built) the
// picture is drawn with the edge filter (FXAA 3.11 "console": five fetches at coordinates the vertex shader computed, four more on an
// edge; on flat pixels the sharpened upscale's unsharp mask at `amount`, from the same fetches) and the HUD rectangles are drawn again
// as PresentSharpened draws them. True: the picture is complete (the caller draws nothing more). False: nothing was drawn and no GL
// state was touched (the caller presents as before).
bool PresentEdgeAA(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY);
// (gpucut23) whether PresentEdgeAA would draw a present of w x h shown at dw x dh (the Vita host's GXV_PRESENT_PLAIN leaves those alone)
bool EdgeAAWanted(int w, int h, int dw, int dh);
// GXV_DIRECT_DISPLAY (direct24; vgx_directdisplay.h): see the definitions in vgx_backend_gl.cpp. The host's whole-present phase asks, shows the picture by
// vglSetNextFlip + the swap when it got a buffer, and says how it was shown (DirectDisplayResult) before the hook returns.
unsigned DirectDisplayOffer(int x, int y, int w, int h, int dx0, int dx1, int screenW, int screenH, bool hostOk);
void DirectDisplayResult(bool flipped, int why);
int DirectDisplayMode();
bool DirectDisplayDrained();   // (dd24, GXV_DD_DYN) the game thread may open a 60 Assist pair: nothing of the direct display is in flight to the screen
bool DirectDisplayDynOn();
void DebugDirectStarts(uint64_t& dead, uint64_t& restored); // (tests: after a flip whose copy did not clear, the next picture's starts found the colour dead / drew it back)
unsigned DebugDirectSparesExtra(); // (tests: the spares kept besides the ordinary ones for the direct display's flight: 0 when the switch is off or the machine is)
uint64_t DebugVramFreeCalls(); // (tests: how often video memory was asked about so far; direct24 r2: a machine that is off must add none)
// Builds the edge filter's programs now (Init, while the loading screen is up) when GXV_EDGE_AA is on: the shader is not in the
// package's compiled cache, so its first build ever (libshacccg) must not fall into play.
void WarmEdgeAA();
// GXV_EDGE_AA totals since the start: presents smoothed whole, HUD rectangles drawn again, presents skipped (multisampled EFB),
// presents skipped for another reason (shader not built).
void EdgeAACounters(uint64_t& presents, uint64_t& hudRects, uint64_t& msaaSkips, uint64_t& otherSkips);
// GXV_PRESENT_FAST=1|2 (vgx_backend_gl.cpp, "the fast present"): PresentSharpened and PresentEdgeAA draw the one-pass present (upscale,
// unsharp mask and, for PresentEdgeAA, the edge term, all with non-dependent reads) in place of their own programs. Totals since the start:
// fast presents, of those with the edge term, HUD rectangles drawn again plainly.
void PresentFastCounters(uint64_t& presents, uint64_t& smoothed, uint64_t& hudRects);
// GXV_UPSCALE=1|2 (vgx_backend_gl.cpp, "the upscaler"): PresentEdgeAA draws a picture shown larger than rendered through the upscaler
// (Catmull-Rom from five non-dependent bilinear reads) in place of the edge smoothing and the sharpened present. Totals since the start:
// upscaled presents, of those over the left half only (GXV_UPSCALE_SPLIT), presents left to the plain present by the controller's edge step.
void UpscaleCounters(uint64_t& presents, uint64_t& split, uint64_t& stepSkips);
// mario24: presents that carried the pass over the player's rectangles (GXV_EDGE_AA_PLAYER, GXV_PLAYER_SHARPEN), their rectangles and screen pixels.
void PlayerPassCounters(uint64_t& presents, uint64_t& rects, uint64_t& pixels);
// mario24: one clause of text for the "player-look:" line: the render target, the texture LOD bias and anisotropy the player's textures get, the present's
// passes. Returns the length written (snprintf's); any thread.
int PlayerLookText(char* buf, size_t n);
// Totals since the start: presents with rectangles drawn, rectangles, screen pixels shaded, presents skipped (multisampled EFB).
void CharaAACounters(uint64_t& presents, uint64_t& boxes, uint64_t& pixels, uint64_t& msaaSkips);
// (tests) Scenes the last executed frame began, as the back end counts them for vitaGL (a new target, or the present).
unsigned DebugLastFrameScenes();
void ClearQBackCounters(uint64_t out[8]); // (tests, "clearq:" line; GXV_CLEARQ) marked records met, made as a clear, refused by reason (count-only, shape, stencil, pass, extent, planes)
// (tests/vgx_hsrorder_gl.cpp, GXV_HSR_ORDER / GXV_DROP_*) pictures planned, pictures executed in the reordered order, alpha-tested draws moved later, draws dropped (since the start)
void DebugHsr(uint64_t out[4]);
void DebugHsrGeo(uint64_t out[8]); // (ties25) the proofs of GXV_HSR_ORDER_TIES=4: ranges ready, bad, pairs done, big, queued, jobs pending, work units, keep on
void DebugAtpp(uint64_t out[5]); // (tests, GXV_ALPHATEST_PREPASS) [0] pairs drawn, [1] prepasses hoisted, [2] tested-key fallbacks, [3] hoists planned, [4] pictures planned
// (tests) GXV_PX_* totals since the start: draws left out (GXV_PX_NOOP), clears without their colour, without their depth, left out
// whole (GXV_PX_DEADCLEAR), scissor changes not made (GXV_PX_SCISSOR).
void DebugPxCuts(uint64_t& noop, uint64_t& clearColour, uint64_t& clearDepth, uint64_t& clearWhole, uint64_t& scissorKept);
// (tests) The pixels of the back end's texture texId as RGBA8 rows in GL order (bottom row first) and its size; false
// when there is no such texture. Back-end thread, between frames.
bool DebugReadTexture(uint32_t texId, std::vector<uint8_t>& rgba, int& width, int& height);
bool DebugReadEfbStencil(std::vector<uint8_t>& st, int& width, int& height); // (mf26, tests)
// (tests) Pooled framebuffer objects now, the render targets' memory (estimate), pooled objects deleted as idle so far.
void DebugPoolStats(unsigned& pooled, uint64_t& renderTargetBytes, uint64_t& evicted);
unsigned TextureSlots(); // (grow24) slots of the back end's id-indexed texture table (g_tex), any thread
void NoteSceneGone(); // (bugs26) a scene was destroyed (any thread): the next size change distrusts its skip-resample evidence for a while
void DebugObjectCounts(unsigned& programs, unsigned& textureSlots, unsigned& liveTextures, unsigned& pooledTargets); // (bowser24 test)
// (tests) The video-memory check of a step up to `rung` from the current one (`current`): what it needs and what is free
// (both 0 when no figure is known: allowed).
bool DebugRungFits(int rung, double& needMiB, double& freeMiB, int& current, double& projectedMiB); // (and the projected render targets)
// Host-harness fast-forward: while set, draw commands are dropped (uploads,
// shader compiles, copies and clears still run).
void SetSkipDraws(bool skip);
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
void DrawRejectReport();
#endif
// Progress display while shaders are being compiled. On the console a shader takes
// 0.3 to 1 s the first time (GLSL -> Cg -> the system's compiler); a frame that needs
// several (the first frame of a start with the warm-up list: all of them) would leave
// the last picture frozen for that long. The host names a framebuffer the back end may
// draw a bar into in the middle of a frame (0 = the display itself), its size, and a
// function that shows it (swaps). After 150 ms of shader work within one command buffer
// the back end draws the bar and calls `show` after every further shader / program /
// variant, at most every 40 ms. `show` may do anything with the context; the back end
// trusts none of its cached state afterwards. Without this call nothing is drawn.
//   GXV_PROGRESS=0            never draw the bar
//   GXV_SIM_COMPILE_MS=<n>    (testing, any GL) every shader compile is charged n ms on
//                                 top of what it took, as the console's compiler would cost;
//   GXV_SIM_CACHE=<dir>       ... unless <dir> holds a file named after the hash of the
//                                 shader text, which is created by the first compile: the
//                                 behaviour of vitaGL's on-disk shader cache;
//   GXV_SIM_COMPILE_SLEEP=1   ... and the thread really sleeps that long.
using ProgressShowHook = void (*)(uint32_t done, uint32_t total);
void SetProgressDisplay(unsigned framebuffer, int width, int height, ProgressShowHook show);
// When the bar is shown (default: whenever the rule above says): during the first `startupFrames`
// frames of the run (the warm-up list's work), and later only once a burst of shader work (work
// that goes on with gaps of less than 1.5 s) has lasted `afterMs`. A new shader met in the middle
// of play, which costs a few hundred milliseconds, is then a hitch and not a flash of the bar.
//   GXV_PROGRESS_AFTER_MS=<n>   overrides afterMs;  GXV_PROGRESS_STARTUP=<n>  overrides startupFrames
void SetProgressPolicy(uint64_t startupFrames, unsigned afterMs);
// A host that has a picture of its own for the wait (the Vita's loading screen) draws it with these:
// SetPicture copies a w x h RGBA picture (R first in memory, top row first) into a texture of the back
// end's, DrawPicture draws that texture over the whole display target (framebuffer 0, no swap; the host
// swaps). With SetProgressHostDraws(true) the back end does not draw its own bar at a progress call: it
// only calls the host's `show` function, which draws the picture. Context state is trusted by nobody
// afterwards (as after the present hook). ReleasePicture gives the textures back.
bool SetPicture(const uint32_t* rgba, int width, int height);
bool DrawPicture();
void ReleasePicture();
void SetProgressHostDraws(bool hostDraws);
// bootscr26: PlayStarted is set at the first frame whose picture is live in a real stage (not the title, not the file select) and is never cleared. From then on the port makes
// no boot, loading, dialog or progress picture over a live picture (vgx_boot_screen.h). PlayStage: the stage key it was set at. PlayScreenLive: this frame's picture is live (render thread).
bool PlayStarted();
uint32_t PlayStage();
bool PlayScreenLive();
// Deferred shader warm-up (the front end's SetShaderWarmupDeferred): the commands of a buffer's `warm` list are not
// executed with the buffer but queued, and done `budgetMs` at a time at the start of the frames that follow (a draw that needs a
// shader that is still queued compiles it on the spot, as it would a shader the list did not know). WarmPending is the weighted
// work left (shader 10, program 2, variant 1) and WarmTotal what was queued in all; either may be read from any thread.
void SetWarmBudget(double budgetMs);
unsigned WarmPending();
// Pending optional work yielded to native program capacity, not completed work.
bool WarmCapacityYielded();
// One slice of the warm-up between two frames (the render thread only, with nothing to draw): while the game loads it hands over
// a frame about every 70 ms and the back end is idle in between, so a slice per frame left most of the time unused.
void WarmIdle();
// (GXV_STAGEWARM, vgx_backend_gl.cpp) the stage warm-up: the draw variants left in the queue of the stage being loaded or played
// (any thread); whether a slice between frames has work (programs of the queue not linked yet, or a stage being loaded that has no
// queue yet); and that slice, which only links (no draw, no delete; both render thread only, called while the game's loading thread runs).
unsigned StageWarmPending();
bool StageWarmIdleWanted();
void StageWarmIdle();
unsigned WarmTotal();
// How the slices went: their number, the longest one and all of them together (ms).
void WarmSliceStats(unsigned& slices, double& worstMs, double& totalMs);
// (endwait26, GXV_ENDWAIT_LOG) what the back end has finished since start-up, summed over its pictures (render thread writes, any thread reads without a lock: a
// log line may see a picture half added), for the front end's report of a long game-thread wait; and the picture whose own back-end line is wanted.
struct EndWaitCum {
    unsigned frames = 0, targets = 0, texN = 0;
    double execMs = 0, targetMs = 0, texMs = 0, sceneMs = 0, endMs = 0, finishMs = 0, dispMs = 0, presentMs = 0, drawMs = 0, reserveMs = 0;
    uint64_t texBytes = 0;
};
void EndWaitCumulative(EndWaitCum& out);
void EndWaitTag(uint64_t frame);
struct Stats {
    uint32_t programs = 0, vertexShaders = 0, fragmentShaders = 0;
    uint32_t programsUsedThisFrame = 0, drawCallsThisFrame = 0, textures = 0;
    uint64_t textureBytes = 0; // texel bytes uploaded since start-up (every upload, re-uploads too; nothing subtracted)
    uint64_t textureBytesLive = 0; // (heap23b) texel bytes of the textures alive now: each texture's last upload, less the deleted ones
    // Shader work since start-up: compiles asked of the GL, of those answered from the simulated
    // cache (GXV_SIM_CACHE; the real one is inside vitaGL and cannot be seen from here),
    // programs linked, time spent in all of it (with the simulated charge), bars shown.
    uint32_t shaderCompiles = 0, shaderSimCacheHits = 0, programLinks = 0, progressFrames = 0;
    double shaderWorkMs = 0;
    // (GXV_SHADER_RECLAIM, shader23) shader objects the back end holds now (g_vs / g_fs: what takes vitaGL's 2,048 slots, besides a
    // handful of utility shaders) and their peak; given back so far, in how many passes, programs deleted for shader room, and the
    // times glCreateShader had no slot. Counted whatever the switch says (with 0 nothing is given back).
    uint32_t shadersHeld = 0, shadersHeldPeak = 0, shadersReclaimed = 0, shaderReclaimPasses = 0, shaderReclaimProgramDeletes = 0;
    uint32_t shaderCreateFails = 0;
};
Stats GetStats(); // per-frame members describe the last executed buffer
// What the back end wants from the front end. Fixed at build time / by the
// environment (never asked of the GL), so any thread may read it at any time.
struct Caps {
    bool dxt1 = false;     // takes TF_DXT1 uploads (GXV_DXT1, default 1)
    bool mipGen = false;   // RGBA8 mip chains: send level 0 only, the back end generates the rest (GXV_MIPGEN)
    bool mipClamp = false; // send only the authored levels of a mip chain (GXV_MIPCLAMP)
    bool la88 = false;     // non-mipmapped I/IA uploads (GXV_LA88, default on for Vita)
    bool i8 = false;       // non-mipmapped I4/I8 1-byte uploads as TF_I8 (GXV_LA88=2, Vita default; GL_INTENSITY / U8_RRRR)
    bool authoredI4Mips = false; // default-off, requires the Vita host's synchronized authored upload
    bool dxt5 = false;     // takes TF_DXT5 uploads of CMPR textures (GXV_CMPRA; the GL back end decodes them where the GL has no DXT5)
    bool tex16 = false;    // takes TF_RGB565 / TF_RGBA5551 / TF_RGBA4444 uploads (GXV_TEX16)
};
const Caps& GetCaps();
}

// The host layer's submission hook: runs `buf` on the render thread and
// returns when it has been executed (synchronous for now).
void SubmitToBackend(CmdBuf& buf);

namespace host {
// Makes whatever context the back end needs current on the calling thread
// (the render thread). vgx_host_egl.cpp for the GL back end; the null back
// end has nothing to create.
bool CreateContext();
}

} // namespace vgx
