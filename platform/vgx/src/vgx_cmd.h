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
enum : uint32_t {
    VL_NRM = 1u << 0,
    VL_COL0 = 1u << 1,
    VL_COL1 = 1u << 2,
    VL_TEX0 = 1u << 3, // .. VL_TEX0 << 7
    VL_TMI = 1u << 11,
    VL_ALLTEX = 0xFFu << 3,
};
struct VtxLayout {
    uint32_t mask = 0;
    uint16_t stride = 16;
    uint8_t offNrm = 0, offCol0 = 0, offCol1 = 0, offTmi = 0;
    uint8_t offTex[8] = {};
};
inline VtxLayout MakeLayout(uint32_t mask) {
    VtxLayout l;
    l.mask = mask;
    uint32_t o = 16;
    if (mask & VL_NRM) { l.offNrm = static_cast<uint8_t>(o); o += 12; }
    if (mask & VL_COL0) { l.offCol0 = static_cast<uint8_t>(o); o += 4; }
    if (mask & VL_COL1) { l.offCol1 = static_cast<uint8_t>(o); o += 4; }
    for (int i = 0; i < 8; ++i)
        if (mask & (VL_TEX0 << i)) { l.offTex[i] = static_cast<uint8_t>(o); o += 8; }
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
    uint8_t dstAlphaPass = 0; // second, alpha-only pass with constant alpha (blend + GXSetDstAlpha)
    uint8_t lineWidth6 = 6;   // GX line width / point size in 1/6 pixel
    // Scissor and viewport in EFB pixels, origin top-left. The back end scales
    // them to the render target.
    int16_t scX = 0, scY = 0, scW = 640, scH = 528;
    float vpX = 0, vpY = 0, vpW = 640, vpH = 528, zNear = 0, zFar = 1;
};

// What the target GPU's shader patcher keys a fragment program on besides the shaders:
// the blend function (or none) and the colour mask. 12 bits. (vitaGL: blend_info in
// blending.c, compared per draw in setup_frag_program, custom_shaders.c.)
inline uint32_t BlendKey(const RenderState& rs) {
    return (rs.blend ? 1u | (static_cast<uint32_t>(rs.blendEq & 1u) << 1) | (static_cast<uint32_t>(rs.blendSrc & 15u) << 2) |
                           (static_cast<uint32_t>(rs.blendDst & 15u) << 6)
                     : 0u) |
           (rs.colorWrite ? 1u << 10 : 0u) | (rs.alphaWrite ? 1u << 11 : 0u);
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
};

enum : uint8_t { PRIM_TRIANGLES, PRIM_LINES, PRIM_POINTS };
struct DrawRec {
    uint16_t vsId = 0, fsId = 0;
    uint8_t prim = PRIM_TRIANGLES;
    uint8_t texMask = 0; // which of tex[] the fragment shader samples
    // The record starts with a primitive that writes every pixel of `ex` whatever was there:
    // a rectangle over the whole viewport, no blending, no alpha test, no depth test.
    uint8_t cover = 0;
    uint8_t pad0 = 0;
    // The EFB rectangle the record's pixels are known to stay inside: the scissor, or the
    // scissor cut to the viewport when the vertices are known not to leave it (on the
    // console, as on the Wii, only the scissor cuts; a desktop GL clips to the viewport).
    // For the front end's bookkeeping of what the EFB holds (pass isolation), not for drawing.
    int16_t exX = 0, exY = 0, exW = 0, exH = 0;
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
    uint8_t nTev = 28; // fragment: size of u_tev
    std::string text;
};

// ---------------------------------------------------------------- commands
enum : uint8_t {
    TF_RGBA8 = 0, // 4 bytes per texel, R first, top row first
    TF_DXT1 = 1,  // S3TC DXT1 blocks (8 bytes each, row-major), as GL_COMPRESSED_RGBA_S3TC_DXT1_EXT takes them
    TF_LA88 = 2,  // 2 bytes per texel, luminance then alpha, sampling as (L,L,L,A); no mipmaps
    TF_DXT5 = 3,  // S3TC DXT5 blocks (16 bytes each, row-major): only the HD replacements (vgx_hdtex.cpp)
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
};
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
template <typename T> struct NoInitAlloc {
    using value_type = T;
    NoInitAlloc() = default;
    template <typename U> NoInitAlloc(const NoInitAlloc<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
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
bool Init(int renderWidth, int renderHeight); // 0,0 = follow the EFB size
void Execute(const CmdBuf& buf);
// Present hook: the host layer decides what "present" means (swap, PNG dump).
// Returns the last display copy as tightly packed RGBA8, top row first.
const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height);
void RequestCapture(bool enable); // read back the next display copy
// Optional present hook for hosts that show the picture themselves (instead
// of asking for a capture and intercepting the read-back). Called on the back
// end's thread at every display copy, before the copy's clear, with the EFB
// framebuffer object still bound: its name, and the copy rectangle in
// render-target pixels. The EFB is stored top row first: GL row 0 is the top
// row of the picture.
using PresentHook = void (*)(unsigned efbFramebuffer, int x, int y, int width, int height);
void SetPresentHook(PresentHook hook);
// Automatic quality (dynamic resolution, vgx_autoq.h): a host whose present hook swaps calls this right after the
// swap, on the back end's thread. The GPU clock then knows the frame's last scene and times its completion.
void AutoQualitySwapped();
// The quality rung (vgx_autoq.h) to use from the next frame on, whatever the controller says (it is stopped): tests, or a
// host that offers a fixed choice. Back-end thread, between frames.
void ForceQualityRung(int rung);
// The highest rung of the quality ladder this platform can show (8 with 4x multisampling of the EFB, 7 with 2x, 6 without).
int QualityTopRung();
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
// Totals since the start: presents with rectangles drawn, rectangles, screen pixels shaded, presents skipped (multisampled EFB).
void CharaAACounters(uint64_t& presents, uint64_t& boxes, uint64_t& pixels, uint64_t& msaaSkips);
// (tests) Scenes the last executed frame began, as the back end counts them for vitaGL (a new target, or the present).
unsigned DebugLastFrameScenes();
// (tests) The pixels of the back end's texture texId as RGBA8 rows in GL order (bottom row first) and its size; false
// when there is no such texture. Back-end thread, between frames.
bool DebugReadTexture(uint32_t texId, std::vector<uint8_t>& rgba, int& width, int& height);
// (tests) Pooled framebuffer objects now, the render targets' memory (estimate), pooled objects deleted as idle so far.
void DebugPoolStats(unsigned& pooled, uint64_t& renderTargetBytes, uint64_t& evicted);
// (tests) The video-memory check of a step up to `rung` from the current one (`current`): what it needs and what is free
// (both 0 when no figure is known: allowed).
bool DebugRungFits(int rung, double& needMiB, double& freeMiB, int& current, double& projectedMiB); // (and the projected render targets)
// Host-harness fast-forward: while set, draw commands are dropped (uploads,
// shader compiles, copies and clears still run).
void SetSkipDraws(bool skip);
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
// Deferred shader warm-up (the front end's SetShaderWarmupDeferred): the commands of a buffer's `warm` list are not
// executed with the buffer but queued, and done `budgetMs` at a time at the start of the frames that follow (a draw that needs a
// shader that is still queued compiles it on the spot, as it would a shader the list did not know). WarmPending is the weighted
// work left (shader 10, program 2, variant 1) and WarmTotal what was queued in all; either may be read from any thread.
void SetWarmBudget(double budgetMs);
unsigned WarmPending();
// One slice of the warm-up between two frames (the render thread only, with nothing to draw): while the game loads it hands over
// a frame about every 70 ms and the back end is idle in between, so a slice per frame left most of the time unused.
void WarmIdle();
unsigned WarmTotal();
// How the slices went: their number, the longest one and all of them together (ms).
void WarmSliceStats(unsigned& slices, double& worstMs, double& totalMs);
struct Stats {
    uint32_t programs = 0, vertexShaders = 0, fragmentShaders = 0;
    uint32_t programsUsedThisFrame = 0, drawCallsThisFrame = 0, textures = 0;
    uint64_t textureBytes = 0;
    // Shader work since start-up: compiles asked of the GL, of those answered from the simulated
    // cache (GXV_SIM_CACHE; the real one is inside vitaGL and cannot be seen from here),
    // programs linked, time spent in all of it (with the simulated charge), bars shown.
    uint32_t shaderCompiles = 0, shaderSimCacheHits = 0, programLinks = 0, progressFrames = 0;
    double shaderWorkMs = 0;
};
Stats GetStats(); // per-frame members describe the last executed buffer
// What the back end wants from the front end. Fixed at build time / by the
// environment (never asked of the GL), so any thread may read it at any time.
struct Caps {
    bool dxt1 = false;     // takes TF_DXT1 uploads (GXV_DXT1, default 1)
    bool mipGen = false;   // RGBA8 mip chains: send level 0 only, the back end generates the rest (GXV_MIPGEN)
    bool mipClamp = false; // send only the authored levels of a mip chain (GXV_MIPCLAMP)
    bool la88 = false;     // non-mipmapped I/IA uploads (GXV_LA88, default on for Vita)
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
