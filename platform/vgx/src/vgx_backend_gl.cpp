// OpenGL ES 2.0 back end. Executes CmdBufs; knows nothing about GX registers
// or guest memory. Only ES 2.0 core calls are used (plus OES_depth24 /
// OES_depth_texture / EXT_texture_compression_s3tc when the driver offers
// them) so the same code sits on vitaGL on the console.
//
// The EFB is an FBO whose rows are stored top row first (the projection flips
// Y), so EFB copies come out in texture row order and no flip is needed when
// a copy is sampled or read back.
//
// What vitaGL (commit cdbba42, read 4-5 Oct) makes of GL calls decides how
// this file is written; the rules, each with the place in vitaGL it comes from:
//  - Every GL state call is issued only when the value changes (GlState below).
//    glEnable(GL_SCISSOR_TEST) and glScissor mark the scissor dirty even when
//    nothing changes (misc.c, tests.c glScissor), and a dirty scissor costs two
//    extra full-target draws before the next draw (tests.c update_scissor_test).
//    glUseProgram marks both uniform buffers dirty, and a dirty buffer is
//    copied whole into a 2 MiB ring before the next draw (custom_shaders.c
//    upload_uniforms; utils/gxm_utils.c).
//  - Vertex attribute pointers never carry a per-draw byte offset: sceGxm's
//    attribute offset has 16 bits and vitaGL stores the whole pointer in it
//    (custom_shaders.c handle_packed_vbo_attrib), and every distinct offset is
//    a newly patched vertex program (utils/gxm_utils.c vglCreateVertexProgram).
//    Draws index their vertices from the start of a buffer instead (see
//    DrawRec::vtxFirst and CmdBuf::vtxChunks in vgx_cmd.h).
//  - Every attribute location has had a pointer set with a buffer bound (Init),
//    and the copy quad comes from a buffer: an attribute without a buffer makes
//    vitaGL scan the index list of every draw for its highest index.
//  - The constant an attribute location supplies while its array is off
//    (glVertexAttrib4f) is given again at least once per frame: vitaGL keeps
//    the four floats in a 256 KiB ring (buffers.c DISABLED_ATTRIBS_POOL_SIZE,
//    custom_shaders.c reserve_attrib_pool) that later glVertexAttrib* calls
//    overwrite, so a constant set once and used for minutes would turn into
//    some other location's constant after about 16 000 calls.
//  - Uniform locations are compared with -1 only (a vitaGL location is the
//    negated address of its record and may have either sign), and array
//    uploads never exceed the size the GL reports for the array.
//  - glClear ignores glColorMask (misc.c glClear: the clear program is patched
//    once without a mask), so a clear of colour without alpha or the reverse is
//    drawn as a quad (GXV_QUADCLEAR, default 1 everywhere).
//  - A framebuffer object's render target is re-created when the colour
//    attachment's size changes and is sized for 8 scenes per frame (shared.h
//    _glFramebufferTexture2D, gxm.c): see the pool below.
//  - glTexImage2D with level > 0 ignores its data and regenerates the level
//    from level 0 (textures.c -> gpu_alloc_mipmaps); glCompressedTexImage2D
//    stores each level as given but cannot take levels smaller than 4x4
//    (utils/gpu_utils.c gpu_alloc_compressed_texture: a transfer of w/4 x h/4
//    blocks). See backend::Caps.
//
// GXV_GLSTATS=<csv> writes, per executed frame, how many GL calls of each
// kind were made (VGX_GL_COUNTERS below) plus what they cost on vitaGL as far
// as that can be told from here (scenes, scissor updates, bytes copied into
// the uniform ring).
#include "vgx_cmd.h"
#include "vgx_workload.h"
#include "vgx_diagnostic_csv.h"
#include "vgx_shadergen.h"
#include "vgx_validate.h"
#include "vgx_coldtrace.h"
#include "vgx_autoq.h"
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#if VGX_COLDTRACE && defined(__vita__)
#include "port/vgx_coldtrace_control.h"
#include <psp2/kernel/threadmgr.h>
#endif

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <ctime>
#include <memory>
#include <unordered_set>
#include <unordered_map>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

// The game's state for automatic quality's calm mode (vita/runtime/flat_view.cpp, platform/src/port/perf.cpp; weak: absent in
// the desktop and test builds, where calm mode then runs without quiet moments and stage keys).
extern "C" int port_screen_quiet(void) __attribute__((weak));
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
#ifdef __VGX_VITA__
extern "C" void vglSetNextSceneDepthModes(GLboolean load, GLboolean store) __attribute__((weak));
extern "C" GLint vglFramebufferDepthTexture(GLuint fb) __attribute__((weak)); // vitaGL built with DEPTH_SAMPLE (GXV_FXHALF): row width or 0
extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The first few times something of the gpupass section happens: one line in the console's log (the back end's stderr does not reach it).
#define GPUPASS_NOTE(n, ...) do { static int gp_cnt_ = 0; if (gp_cnt_ < (n)) { ++gp_cnt_; port_log("[vgx] " __VA_ARGS__); } } while (0)
#else
#define GPUPASS_NOTE(n, ...) VGX_LOG_N(n, __VA_ARGS__)
#endif

namespace vgx::backend {

namespace {
// gpupass: free video memory (the console, vitaGL's vglMemFree, in every variant; elsewhere: plenty). A new EFB buffer
// (GXV_EFBSWAP) or the effects' targets (GXV_FXHALF) are refused below kFxVramMin; the ordinary path is taken instead.
inline size_t VramFree() {
#ifdef __VGX_VITA__
    return vglMemFree(VGL_MEM_VRAM);
#else
    return static_cast<size_t>(-1);
#endif
}
constexpr size_t kFxVramMin = 6u << 20;
} // namespace

const Caps& GetCaps() {
    static const Caps caps = [] {
#ifdef __VGX_VITA__
        const long vita = 1;
#else
        const long vita = EnvInt("GXV_VITA", 0); // 1: behave towards the front end as on the console (for tests on the desktop)
#endif
        Caps c;
        c.dxt1 = EnvInt("GXV_DXT1", 1) != 0;
        c.mipGen = EnvInt("GXV_MIPGEN", vita) != 0;
        c.mipClamp = EnvInt("GXV_MIPCLAMP", vita) != 0;
        c.la88 = EnvInt("GXV_LA88", vita) != 0;
        return c;
    }();
    return caps;
}

namespace {

// Silent output alone does not disable profiling waits, readbacks or file I/O.
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
constexpr bool kBackendDiagnostics = false;
#else
constexpr bool kBackendDiagnostics = true;
#endif
const char* DiagnosticEnv(const char* name) { return kBackendDiagnostics ? std::getenv(name) : nullptr; }
long DiagnosticInt(const char* name, long fallback) { return kBackendDiagnostics ? EnvInt(name, fallback) : fallback; }

// ------------------------------------------------------------ call census
#define VGX_GL_COUNTERS(X)                                                                                                            \
    X(glCalls)        /* every GL call made for the frame */                                                                         \
    X(scenes)         /* render-target changes followed by drawing (sceGxm scenes), not counting the host's present */               \
    X(efbScenes)      /* of those: on the EFB */                                                                                     \
    X(fboBinds) X(fboAttach)                                                                                                         \
    X(clears)         /* glClear */                                                                                                  \
    X(quadClears)     /* clears drawn as a quad (partial colour mask) */                                                             \
    X(copyQuads)      /* EFB / pass -> texture copy draws */                                                                         \
    X(draws) X(drawIndices)                                                                                                          \
    X(programUses)    /* glUseProgram */                                                                                             \
    X(uniformCalls) X(uniformBytes) /* glUniform*: all blocks */                                                                     \
    X(uniformCompares) X(uniformCompareHits) X(uniformCompareBytes) /* candidate payload bytes, not early-exit memory traffic */       \
    X(palCalls) X(palBytes)         /* of those: the matrix palette */                                                               \
    X(ringBytes)      /* vitaGL: bytes of whole uniform buffers copied to its ring (one per draw whose program or uniforms changed) */ \
    X(streamUploads) X(streamBytes) /* glBufferData of the frame's vertex chunks and indices */                                      \
    X(staticUploads) X(staticBytes) /* glBufferData of static buffers */                                                             \
    X(texUploads) X(texUploadBytes) X(texBinds) X(texParams) X(texDeletes) X(mipGens)                                                \
    X(vboBinds) X(attrPointers) X(attrToggles) X(attrConsts)                                                                         \
    X(layoutChanges)  /* draws that had to set attribute pointers (buffer or layout changed) */                                      \
    X(scissorCalls)   /* glScissor + glEnable/glDisable(GL_SCISSOR_TEST) */                                                          \
    X(scissorUpdates) /* vitaGL: scissor mask rebuilt (new scene or scissor call before a draw): 2 extra full-target draws each */   \
    X(viewportCalls) X(stateCalls) /* blend, depth, cull, colour mask, depth range, line width */                                    \
    X(shaderCompiles) X(programLinks) X(readPixels)                                                                                   \
    X(fragSwitches)   /* vitaGL: draws whose program was last drawn with another blend function / colour mask (a shader patcher call) */ \
    X(fragVariants)   /* of those: with a combination never used before (the patcher builds a fragment program: milliseconds) */      \
    X(vtxVariants)    /* draws with a (vertex shader, vertex layout) pair never used before (the patcher builds a vertex program) */   \
    X(warmDraws)      /* empty draws that make vitaGL build a program variant ahead of its first use (CMD_VARIANT) */               \
    X(sceneReuploads) /* draws that gave all their uniform blocks again because they open a scene with the previous draw's program */ \
    X(constAttrDraws) /* draws whose vertex shader reads an attribute the vertex layout does not have (a glVertexAttrib4f constant) */ \
    X(efbDepthLoads) X(efbDepthStores) /* EFB scenes whose depth buffer is loaded / stored (GXV_DEPTH_SKIP, GXV_DEPTHPLAN) */           \
    X(depthPlanSaved) /* of the loads and stores GXV_DEPTH_SKIP=2 alone makes: left out by GXV_DEPTHPLAN (EFB size x 4 bytes each) */   \
    X(efbSwaps)       /* GXV_EFBSWAP: full-size copies made by handing the EFB's colour buffer to the texture (no copy scene) */     \
    X(efbRestores)    /* of those: the EFB's new colour buffer filled from the old one by a quad at the start of the next EFB scene */ \
    X(hoists)         /* GXV_PASSHOIST: small copies made from a target of their own instead of interrupting the EFB's scene */ \
    X(fxRuns) X(fxDraws) X(fxVerts) /* GXV_FXHALF: runs of additive sprite draws drawn at the reduced size, their draws and vertices */ \
    X(fxCandidates)   /* draws that could go there (additive, no depth write, outside passes), whether or not their run did */
struct Counts {
#define X(n) uint32_t n = 0;
    VGX_GL_COUNTERS(X)
#undef X
};
Counts C;
unsigned g_lastScenes = 0; // C.scenes of the last frame executed (tests: DebugLastFrameScenes)
#define G(call) (++C.glCalls, call)

struct Program {
    GLuint id = 0;
    GLint proj = -1, mv = -1, tm = -1, ptm = -1, pal = -1, lt = -1, tev = -1;
    GLint nMv = 0, nTm = 0, nPtm = 0, nPal = 0, nLt = 0, nTev = 0; // elements to upload (vec4)
    uint32_t vsBytes = 0, fsBytes = 0;                           // size of the program's uniform buffers (estimate from the declarations)
    uint32_t applied[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
    uint64_t serial = 0;
    const CmdBuf* uniformBuffer = nullptr;
    uint64_t usedSerial = 0;               // last frame a draw used it
    uint64_t linkSerial = 0;               // last linked frame (eviction ordering only)
    uint32_t blendKey = ~0u, layout = ~0u; // of its last draw (see BlendKey)
    uint32_t layoutNeed = 0;               // vertex-layout bits (VL_*) of the attributes the vertex shader declares
    bool ok = false;
    bool warmCompatible = false; // last use had the EFB format and the warm draw's zero vertex base
    uint8_t failures = 0;        // (GXV_GL_RETRY) failed links so far; the GL program of a failed link is deleted
    uint64_t retryAfter = 0;     // (GXV_GL_RETRY) frame from which a failed link may be tried again
};
struct Tex {
    GLuint id = 0;
    uint16_t w = 0, h = 0;
    bool mips = false;
    uint8_t wrapS = 0xFF, wrapT = 0xFF, minF = 0xFF, magF = 0xFF;
    bool renderTarget = false;
    uint16_t ew = 0, eh = 0; // a render target: its size in EFB pixels (w x h is that at the render scale); 0: unknown
    bool efbBuf = false; // GXV_EFBSWAP: `id` is a colour buffer the EFB had (it goes back to the EFB's spares, it is not deleted)
};

int g_fixedW = 0, g_fixedH = 0; // 0 = follow the EFB size
int g_rtW = 0, g_rtH = 0;
int g_efbW = 640, g_efbH = 528;
// ---- render scale (automatic quality): the ONE place EFB coordinates become render-target pixels. The EFB target is
// g_rtW x g_rtH for an EFB of g_efbW x g_efbH; every rectangle, viewport, scissor and copy size the back end derives
// from EFB units is multiplied by ScaleX() / ScaleY() (and rounded with Rnd). The factors change only between frames
// (ApplyRung), never inside a command buffer. g_rungSX/SY are the quality rung's factors against the EFB (vgx_autoq.h:
// rung w / 640, rung h / 456; 1 x 1 = the EFB's own size, every build before automatic quality).
float g_rungSX = 1.0f, g_rungSY = 1.0f;
inline float ScaleX() { return static_cast<float>(g_rtW) / static_cast<float>(g_efbW); }
inline float ScaleY() { return static_cast<float>(g_rtH) / static_cast<float>(g_efbH); }
// The EFB target's size for an EFB of efbW x efbH: GXV_RENDER_SIZE when given, else the EFB at the rung's factors.
void TargetSize(int efbW, int efbH, int& w, int& h) {
    if (g_fixedW > 0 && g_fixedH > 0) { w = g_fixedW; h = g_fixedH; return; }
    w = static_cast<int>(static_cast<float>(efbW) * g_rungSX + 0.5f);
    h = static_cast<int>(static_cast<float>(efbH) * g_rungSY + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
}
GLuint g_efbFbo = 0, g_efbColor = 0, g_efbDepthRb = 0, g_efbDepthTex = 0;
// ---- the EFB's multisampling (automatic anti-aliasing: rungs 7 and 8 of the quality ladder, vgx_autoq.h)
// g_efbSamples: samples a pixel of the EFB target now (1: none; EfbApplySamples). On the Vita (a vitaGL built with FBO_MSAA,
// tools/vitagl/fbo_msaa.py) the EFB's own framebuffer object is multisampled (vglFramebufferMultisample): its render target
// and hidden depth buffer have the samples, and the colour is averaged into the EFB's texture whenever a scene stores it,
// so g_efbColor only ever holds the resolved picture: what copies sample, what the display copy shows, what GXV_EFBSWAP
// hands to a texture. Elsewhere (the desktop's GL, for the tests) the EFB is drawn into a multisampled renderbuffer object
// (g_efbMs) that BindFbo(g_efbFbo) binds instead, and resolved into g_efbFbo's texture (glBlitFramebuffer) before anything
// else is bound and before the EFB is read (EfbForRead) or its texture is handed over (AttachColor).
int g_efbSamples = 1;
#if !defined(__VGX_VITA__)
struct EfbMs {
    GLuint fbo = 0, color = 0, depth = 0;
    bool dirty = false;      // drawn since the last resolve
    bool depthResolve = true; // (off after a depth blit the GL refused: formats that do not match)
} g_efbMs;
void ResolveEfbMs();
#endif
void EfbForRead(); // before the EFB is read back (glReadPixels, the host's present): resolved, and bound as g_efbFbo
GLuint g_ibo = 0, g_whiteTex = 0, g_quadVbo = 0;
std::vector<GLuint> g_streamVbo; // one per chunk of the frame's vertex arena
bool g_hasDepth24 = false, g_hasDepthTex = false, g_hasS3tc = false, g_hasMaxLevel = false;
bool g_hasDxt5 = false; // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT (HD texture replacements; decoded to RGBA8 without it)
bool g_quadClear = true;
GLuint g_copyProg = 0;
GLint g_copyRect = -1, g_copyMat = -1, g_copyAdd = -1;

std::vector<GLuint> g_vs, g_fs;
std::vector<const ShaderSource*> g_vsSrc, g_fsSrc;
std::unordered_map<uint32_t, Program> g_programs;
std::vector<Tex> g_tex;
struct StaticBuf { GLuint vbo = 0, ibo = 0; };
std::vector<StaticBuf> g_sbuf; // by BufUpload::bufId
uint64_t g_serial = 0;
Stats g_stats;
uint32_t g_progsThisFrame = 0;

std::unique_ptr<validate::Session> g_validation;
bool g_validateFrame = false;
struct ValidationFrame {
    uint64_t draws = 0, badDraws = 0, uncheckedDraws = 0, references = 0, vertexBytes = 0, indexBytes = 0, uniformBytes = 0;
    uint64_t hash = validate::kHashStart;
    unsigned extraLines = 0;
};
ValidationFrame g_validationFrame;

bool g_capture = false;
PresentHook g_presentHook = nullptr;
const CopyDispCmd* g_charaDisp = nullptr; // GXV_CHARAA: the display copy being presented (CopyDisp, around the present hook)
int g_charaKernel = -1; // (CapturePresented) the kernel for this draw: -1 as GXV_CHARAA_KERNEL says
bool g_charaFull = false; // (CapturePresented) the whole present instead of the rectangles (the whole-picture variant, for comparison)
bool g_skipDraws = false; // harness fast-forward: geometry of frames nobody looks at is not rasterised
std::vector<uint8_t> g_captured;
int g_capW = 0, g_capH = 0;
unsigned g_efbSwapsTotal = 0; // GXV_EFBSWAP copies made so far (tests: DebugEfbSwaps)

// Framebuffer objects for everything that is not the EFB: copy destinations and
// offscreen passes (PassCmd in vgx_cmd.h). They are pooled by size, and one of
// them is used at most g_fboMaxUses times per frame, because of how the
// target GPU's render targets work (sceGxm, through vitaGL):
//  - a render target belongs to a framebuffer object and is created for a
//    size: attaching a texture of another size destroys and re-creates it,
//    attaching another texture of the same size is cheap;
//  - a render target is sized for a number of scenes per frame (at most 8); a
//    further scene on it in the same frame waits for the GPU;
//  - the number of render targets is limited (a few dozen).
// So: never re-attach across sizes, never draw to one object more than 8
// times a frame, and do not make one object per texture.
//   GXV_FBO_USES=n   uses per frame of one pooled object (default 8)
struct PoolFbo {
    int w = 0, h = 0;
    GLuint fbo = 0;
    GLuint color = 0;                 // own colour texture (passes that are copied from afterwards), made on demand
    GLuint depthTex = 0, depthRb = 0; // made on demand (passes)
    GLuint attached = 0;              // the colour texture attached at the moment
    uint32_t uses = 0;                // this frame
    uint64_t lastUse = 0;             // g_presents when last acquired (EvictIdlePool)
    // Held across commands by the gpupass section (PoolHeld::FX: the effects' target of GXV_FXHALF, from BeginFx to EndFx,
    // always within one Execute). An eviction must leave a held entry alone; between frames none is held (GpupassMemReport
    // says so if one is). Everything else gpupass makes in the pool (the corner copies' and half-size passes' targets) is an
    // ordinary pass target: it may be evicted between frames like any other. The pool owns its colour and depth; gpupass
    // hands none of them to a texture, and the EFB colour buffers of GXV_EFBSWAP are never pool entries.
    uint8_t held = 0;
};
namespace PoolHeld { constexpr uint8_t FX = 1; }
std::vector<PoolFbo> g_pool;
uint64_t g_presents = 0; // CMD_PRESENTs executed (pool ageing)
uint32_t g_fboMaxUses = 8;
// The usage hint of the display-list cache's static vertex and index buffers.
// GXV_STATIC_USAGE=dynamic (the Vita host's default; "static" elsewhere): on vitaGL the
// hint decides which memory pool a buffer comes from (source/buffers.c, glBufferData):
// GL_STATIC_DRAW takes VIDEO memory first, GL_DYNAMIC_DRAW and GL_STREAM_DRAW main memory
// first. In open play the static buffers are 18.5 MiB in 741 buffers (census of the stub-GL
// build, GXV_GLMEM=1), a quarter of what the renderer holds, and the console's video
// memory pool had 6.5 MiB left at that point while its main-memory pool had 44 MiB free
// (hw/run3-gl). Nothing else differs between the hints in the vitaGL build we use (the
// "scratch" pool they also select is compiled out).
GLenum g_staticUsage = GL_STATIC_DRAW;
int g_pass = -1;                // index in g_pool of the target of the open pass; -1: no pass target (the EFB)
bool g_inPass = false;          // draws currently go to g_pool[g_pass]
bool g_staticCrowdPassComplete = false;
uint32_t g_passDirectTex = 0;   // not 0: the open pass is drawn straight into this copy texture (PassCmd::directTex)
int g_passX = 0, g_passY = 0;   // what to subtract from EFB pixel coordinates inside the pass (the pass rectangle's origin, less its place in an atlas)
int g_passW = 0, g_passH = 0;   // size of the pass's render target
float g_passScale = 1.0f;       // GXV_PASSHALF: the open pass is drawn at this fraction of the render scale
float g_passScaleNext = 1.0f;   // ... for the pass BeginPass begins next (set by its caller, used once)

// ------------------------------------------------------------ cached GL state
// Nothing below calls GL unless the value changes. Invalidate() after
// anything outside this file may have touched the context (the present hook).
struct GlState {
    GLuint fbo = ~0u, program = ~0u, arrayBuf = ~0u, elemBuf = ~0u;
    int unit = -1;
    GLuint tex[8];
    int8_t scissorOn = -1, blendOn = -1, depthTest = -1, depthMask = -1, cullOn = -1, colorMask = -1;
    int sc[4] = {-1, -1, -1, -1}, vp[4] = {-1, -1, -1, -1};
    float zn = -1, zf = -1, lineWidth = -1;
    int blendEq = -1, blendSrc = -1, blendDst = -1, depthFunc = -1, cullFace = -1;
    // vertex arrays
    uint32_t attrEnabled = 0, attrConstSet = 0;
    GLuint layoutVbo = ~0u; // the attribute pointers describe this buffer ...
    uint32_t layoutMask = 0, layoutBase = ~0u; // ... with this layout at this byte offset
    GlState() { for (GLuint& t : tex) t = ~0u; }
} S;
// vitaGL's view of the render target, for the scene and scissor counts:
// a scene begins at the first draw or clear after the bound object changed
// (gxm.c scene_reset) or after its attachment was replaced while it was the
// one in use (shared.h _glFramebufferTexture2D -> dirty_framebuffer).
GLuint g_inUseFbo = ~0u;
bool g_fbDirty = false, g_scissorDirty = true;
// vitaGL will redraw its scissor mask inside the next draw or clear. The Vita host's guards for
// Vita3K (vgxvita_maskGuard: emulator only) run just then, not before every draw.
inline void MarkScissorDirty() {
    g_scissorDirty = true;
#ifdef __VGX_VITA__
    vgxvita_maskGuardDue = 1;
#endif
}
bool g_uniDirtyV = true, g_uniDirtyF = true; // vitaGL: dirty_shader_vert_unifs / dirty_shader_frag_unifs

void Invalidate() {
    const uint32_t en = S.attrEnabled;
    S = GlState();
    // Which arrays are enabled is this file's own business (nobody else touches them).
    S.attrEnabled = en;
    MarkScissorDirty();
    g_uniDirtyV = g_uniDirtyF = true;
}
// Called before every draw and clear.
bool g_sceneLog = false; // GXV_SCENELOG=<frame>: one line per scene of that frame
void LogScene() {
    int w = g_rtW, h = g_rtH;
    const char* what = "EFB";
    if (S.fbo != g_efbFbo) {
        what = "?";
        for (const PoolFbo& f : g_pool)
            if (f.fbo == S.fbo) { w = f.w; h = f.h; what = f.attached ? "texture" : "pass buffer"; }
    }
    std::fprintf(stderr, "[scene] %2u: %s %dx%d (after %u draws, %u clears, %u copy quads)%s\n", C.scenes, what, w, h, C.draws, C.clears, C.copyQuads,
                 (S.fbo == g_inUseFbo) ? " same object, re-attached" : "");
}
// What a scene needs of its depth buffer (TouchTarget's argument): whatever it holds, or it starts with a clear of all of it.
enum { DN_KEEP = 0, DN_CLEARED = 1 };

// BEGIN GPUPASS DEPTH PLAN
// ------------------------------------------------------------ EFB depth plan (GXV_DEPTHPLAN)
// Which EFB scenes need their depth buffer loaded at the start and stored at the end, from the
// whole frame's commands (the back end has the finished frame before it draws any of it).
// GXV_DEPTH_SKIP=2 alone loads the depth of every EFB scene that does not begin with a clear of
// all of it and stores it at the end of every EFB scene: on the console a full-size read and
// write (640x456 x 4 bytes = 1.17 MB each) per interruption of the EFB by a copy. The plan:
//  - the frame is a sequence of positions, two per command (2i: the command itself, 2i+1: the
//    EFB clear that follows a copy or a display copy), and at each position the EFB's depth is
//    read (a draw that tests depth with a function other than ALWAYS, a depth copy), written
//    (a draw that tests and writes depth, a clear of part of it), overwritten whole (a clear of
//    all of it) or left (the back end draws to another target: a copy, a pass, the display);
//  - depth is live at a position when a read comes before the next whole overwrite (the end of
//    the frame counts as a read: the next frame is not known);
//  - a scene from b to its end e (the next position that leaves the EFB) stores its depth only
//    when it writes depth and depth is live at e; it loads only when depth is live at b, or
//    when it stores and does not overwrite all of it (the rest must survive); a scene that reads
//    depth before overwriting all of it loads in any case.
// A scene that neither reads nor writes depth (the plaza's small pass between two copies) then
// neither loads nor stores, and the memory keeps what the scene before stored.
// The plan is only used for scene beginnings that are certainly new scenes for vitaGL (the
// previous target in this frame was another one, or the display after the present hook), and
// not in frames with shader work (a progress display could break a scene where none is planned)
// or a change of the EFB's size. Elsewhere GXV_DEPTH_SKIP's rules apply unchanged.
//   GXV_DEPTHPLAN=0   off (GXV_DEPTH_SKIP=2 rules for every EFB scene); 1 (default) on.
//   (needs a vitaGL with DEPTH_HINT on the console; on the desktop only the counts and the
//   verification, GXV_PASSVERIFY, see the end of this file)
// Known limit (as for GXV_DEPTH_SKIP=2's cleared scenes): vitaGL's uniform-arena rollover, when
// the GPU is behind, ends and re-begins the open scene with the modes it began with; a scene
// planned not to store then loses its own depth writes made before that point.
namespace dplan {
enum : uint8_t { EV_READ = 1, EV_WRITE = 2, EV_FULL = 4, EV_LEAVE = 8,
                 EV_RC = 16, EV_RA = 32, EV_FC = 64, EV_FA = 128 }; // (colour / alpha read, overwritten whole: for GXV_EFBSWAP)
bool g_valid = false;              // a plan for the frame being executed
size_t g_cmd = 0;                  // command being executed (Execute's loop)
bool g_inClear = false;            // inside the EFB clear that follows a copy (position 2i+1)
bool g_afterHook = false;          // the present hook ran and no target was used since: the next EFB scene is certainly new
std::vector<uint8_t> g_ev;         // events per position
std::vector<uint8_t> g_live;       // depth live at the position (before its events)
std::vector<uint32_t> g_nextLeave; // first position > p that leaves the EFB (size: positions + 1)
std::vector<uint32_t> g_nextRead, g_nextWrite, g_nextFull; // first position >= p with that event (positions: none)
std::vector<uint8_t> g_liveC, g_liveA; // the EFB's colour / alpha live at the position (GXV_EFBSWAP's restore)
inline size_t Pos() { return 2 * g_cmd + (g_inClear ? 1 : 0); }
bool Enabled() {
    static const bool on = EnvInt("GXV_DEPTHPLAN", 1) != 0;
    return on;
}
} // namespace dplan
bool EfbSwapEnabled(); // (GXV_EFBSWAP, below)
// END GPUPASS DEPTH PLAN

// vitaGL built with DEPTH_HINT (scripts/build-vitagl.sh): a hint for the scene that begins next on the bound object. With
// STORE_DEPTH_STENCIL vitaGL loads and stores the depth buffer of every scene, a full-size read and write even for a copy
// target that never tests depth. GXV_DEPTH_SKIP=1: a scene on any object but the EFB loads and stores nothing (a pass begins
// with a clear of its own; a copy target does not use depth; the depth of these objects never lives across scenes).
// GXV_DEPTH_SKIP=2: also an EFB scene that begins with a clear of all its depth loads nothing (its store stays: the EFB's depth
// is read by later passes and by depth copies). The display copy (vgx_host_galaxy_vita.cpp) has its own GXV_DEPTH_SKIP=2 part.
// (vglSetNextSceneDepthModes is a weak reference, declared before the namespace: a library without DEPTH_HINT leaves it null and
// nothing happens.)
// GXV_DEPTHPLAN (above) lowers the EFB's load and store further where the frame shows them to be unneeded.
// `certain`: the scene is certainly a new one for vitaGL (see dplan); otherwise the hint is GXV_DEPTH_SKIP's (a hint that
// vitaGL does not take at once stays until the next scene begins on that object, so only these safe ones may go unused).
// On the desktop (no DEPTH_HINT) the same decisions are counted, and simulated for GXV_PASSVERIFY.
void DepthSimScene(bool efb, bool certain, bool load, bool store); // GXV_PASSVERIFY's model of the console's depth memory (desktop)
bool EfbDepthPlan(bool& load, bool& store);                         // GXV_DEPTHPLAN for an EFB scene beginning now
inline void SceneDepth(int need, bool certain) {
#ifdef __VGX_VITA__
    static const int mode = vglSetNextSceneDepthModes ? static_cast<int>(EnvInt("GXV_DEPTH_SKIP", 2)) : 0; // (on by default since thp24; =0 off)
#else
    static const int mode = static_cast<int>(EnvInt("GXV_DEPTH_SKIP", 2)); // (the desktop: for the counts and the verification only)
#endif
    const bool efb = S.fbo == g_efbFbo;
    bool load = true, store = true;
    if (mode > 0) {
        if (!efb) load = store = false;
        else if (mode >= 2 && need == DN_CLEARED) load = false;
    }
    if (efb && mode > 0 && certain) {
        bool l = load, s = store;
        if (EfbDepthPlan(l, s)) {
            C.depthPlanSaved += (load && !l ? 1u : 0u) + (store && !s ? 1u : 0u);
            if ((load && !l) || (store && !s)) GPUPASS_NOTE(3, "GXV_DEPTHPLAN: executed buffer %llu: an EFB scene %s its depth\n", static_cast<unsigned long long>(g_serial),
                                                           (load && !l) ? ((store && !s) ? "neither loads nor stores" : "does not load") : "does not store");
            load = l;
            store = s;
        }
    }
    if (efb) {
        C.efbDepthLoads += load ? 1u : 0u;
        C.efbDepthStores += store ? 1u : 0u;
    }
#ifdef __VGX_VITA__
    if (mode > 0) vglSetNextSceneDepthModes(load ? GL_TRUE : GL_FALSE, store ? GL_TRUE : GL_FALSE);
#else
    DepthSimScene(efb, certain, load, store);
#endif
}
inline void TouchTarget(int need = DN_KEEP) {
    if (S.fbo != g_inUseFbo || g_fbDirty) {
        if (g_sceneLog) LogScene();
        ++C.scenes;
        if (S.fbo == g_efbFbo) ++C.efbScenes;
        // (certainly a new scene for vitaGL: this frame drew to another target last, or the present hook showed the display)
        const bool certain = (g_inUseFbo != ~0u && (g_inUseFbo != S.fbo || g_fbDirty)) || dplan::g_afterHook;
        dplan::g_afterHook = false;
        g_inUseFbo = S.fbo;
        g_fbDirty = false;
        SceneDepth(need, certain);
        MarkScissorDirty(); // (vitaGL, STORE_DEPTH_STENCIL: every scene on a framebuffer object rebuilds the mask)
    }
    if (g_scissorDirty) { ++C.scissorUpdates; g_scissorDirty = false; }
}
inline void BindFbo(GLuint fbo) {
    if (S.fbo == fbo) return;
#if !defined(__VGX_VITA__)
    // (the desktop's multisampled EFB: resolved before anything else is bound; drawn into instead of g_efbFbo)
    if (g_efbMs.dirty && fbo != g_efbFbo) ResolveEfbMs();
    const GLuint phys = g_efbMs.fbo && fbo == g_efbFbo ? g_efbMs.fbo : fbo;
    if (phys != fbo) g_efbMs.dirty = true;
    G(glBindFramebuffer(GL_FRAMEBUFFER, phys));
#else
    G(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
#endif
    ++C.fboBinds;
    S.fbo = fbo;
}
inline void AttachColor(GLuint tex) { // to the bound framebuffer object
#if !defined(__VGX_VITA__)
    if (g_efbMs.fbo && S.fbo == g_efbFbo) {
        // (the desktop's multisampled EFB: the texture now attached gets the picture first - GXV_EFBSWAP hands it to a
        // texture - and the new one is attached to g_efbFbo, the resolve target, not to the multisampled object)
        if (g_efbMs.dirty) ResolveEfbMs();
        G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo));
        G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
        G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbMs.fbo));
        g_efbMs.dirty = true;
        ++C.fboAttach;
        if (g_inUseFbo == S.fbo) g_fbDirty = true;
        return;
    }
#endif
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    ++C.fboAttach;
    if (g_inUseFbo == S.fbo) g_fbDirty = true;
}
inline void UseProgram(GLuint id) {
    if (S.program == id) return;
    G(glUseProgram(id));
    ++C.programUses;
    S.program = id;
    g_uniDirtyV = g_uniDirtyF = true;
}
inline void BindTex(int unit, GLuint name) {
    if (S.tex[unit] == name) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    G(glBindTexture(GL_TEXTURE_2D, name));
    ++C.texBinds;
    S.tex[unit] = name;
}
// For glTexImage2D / glTexParameteri on `name`: bound on unit 0 and unit 0 active.
inline void BindTexForEdit(GLuint name) {
    if (S.unit != 0) { G(glActiveTexture(GL_TEXTURE0)); S.unit = 0; }
    if (S.tex[0] != name) { G(glBindTexture(GL_TEXTURE_2D, name)); ++C.texBinds; S.tex[0] = name; }
}
inline void ForgetTex(GLuint name) { // a deleted name may be handed out again
    for (GLuint& t : S.tex)
        if (t == name) t = ~0u;
}
inline void BindArrayBuf(GLuint b) {
    if (S.arrayBuf == b) return;
    G(glBindBuffer(GL_ARRAY_BUFFER, b));
    ++C.vboBinds;
    S.arrayBuf = b;
}
inline void BindElemBuf(GLuint b) {
    if (S.elemBuf == b) return;
    G(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b));
    ++C.vboBinds;
    S.elemBuf = b;
}
inline void SetScissor(bool on, int x = 0, int y = 0, int w = 0, int h = 0) {
    if (S.scissorOn != static_cast<int8_t>(on)) {
        if (on) G(glEnable(GL_SCISSOR_TEST)); else G(glDisable(GL_SCISSOR_TEST));
        ++C.scissorCalls;
        S.scissorOn = on;
        MarkScissorDirty();
    }
    if (on && (S.sc[0] != x || S.sc[1] != y || S.sc[2] != w || S.sc[3] != h)) {
        G(glScissor(x, y, w, h));
        ++C.scissorCalls;
        S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h;
        MarkScissorDirty();
    }
}
inline void SetViewport(int x, int y, int w, int h) {
    if (S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h) return;
    G(glViewport(x, y, w, h));
    ++C.viewportCalls;
    S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h;
}
inline void SetDepthRange(float n, float f) {
    if (S.zn == n && S.zf == f) return;
    G(glDepthRangef(n, f));
    ++C.stateCalls;
    S.zn = n; S.zf = f;
}
inline void SetBlendOff() {
    if (S.blendOn == 0) return;
    G(glDisable(GL_BLEND));
    ++C.stateCalls;
    S.blendOn = 0;
}
inline void SetBlend(GLenum eq, GLenum src, GLenum dst) {
    if (S.blendOn != 1) { G(glEnable(GL_BLEND)); ++C.stateCalls; S.blendOn = 1; }
    if (S.blendEq != static_cast<int>(eq)) { G(glBlendEquation(eq)); ++C.stateCalls; S.blendEq = static_cast<int>(eq); }
    if (S.blendSrc != static_cast<int>(src) || S.blendDst != static_cast<int>(dst)) {
        G(glBlendFunc(src, dst));
        ++C.stateCalls;
        S.blendSrc = static_cast<int>(src); S.blendDst = static_cast<int>(dst);
    }
}
inline void SetDepthTest(bool on, GLenum func = GL_ALWAYS) {
    if (S.depthTest != static_cast<int8_t>(on)) {
        if (on) G(glEnable(GL_DEPTH_TEST)); else G(glDisable(GL_DEPTH_TEST));
        ++C.stateCalls;
        S.depthTest = on;
    }
    if (on && S.depthFunc != static_cast<int>(func)) { G(glDepthFunc(func)); ++C.stateCalls; S.depthFunc = static_cast<int>(func); }
}
inline void SetDepthMask(bool on) {
    if (S.depthMask == static_cast<int8_t>(on)) return;
    G(glDepthMask(on ? GL_TRUE : GL_FALSE));
    ++C.stateCalls;
    S.depthMask = on;
}
inline void SetCull(uint8_t cull) { // CULL_*
    if (cull == CULL_NONE) {
        if (S.cullOn != 0) { G(glDisable(GL_CULL_FACE)); ++C.stateCalls; S.cullOn = 0; }
        return;
    }
    if (S.cullOn != 1) { G(glEnable(GL_CULL_FACE)); ++C.stateCalls; S.cullOn = 1; }
    const GLenum face = cull == CULL_BACK ? GL_BACK : (cull == CULL_FRONT ? GL_FRONT : GL_FRONT_AND_BACK);
    if (S.cullFace != static_cast<int>(face)) { G(glCullFace(face)); ++C.stateCalls; S.cullFace = static_cast<int>(face); }
}
inline void SetColorMask(bool color, bool alpha) {
    const int8_t m = static_cast<int8_t>((color ? 1 : 0) | (alpha ? 2 : 0));
    if (S.colorMask == m) return;
    G(glColorMask(color, color, color, alpha));
    ++C.stateCalls;
    S.colorMask = m;
}

// ---- shader work of the command buffer being executed (see SetProgressDisplay in vgx_cmd.h)
inline double NowMs() {
#ifdef __vita__
    // This toolchain's steady_clock uses gettimeofday; budgets need elapsed time.
    return static_cast<double>(sceKernelGetProcessTimeWide()) / 1000.0;
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
struct ShaderWork {
    uint32_t total = 0, done = 0;       // weighted units: shader 10, program 2, variant 1
    uint32_t compiles = 0, simHits = 0, links = 0, variants = 0, shown = 0;
    double ms = 0;                      // measured + simulated
    double sinceShow = 1e9;
    double simPending = 0;              // simulated charge of the command being executed
} g_sw;
unsigned g_dispFbo = 0;
int g_dispW = 0, g_dispH = 0;
ProgressShowHook g_showHook = nullptr;
bool g_hostDrawsProgress = false;     // SetProgressHostDraws: the host's show function draws the whole picture
uint64_t g_progStartupFrames = ~0ull; // SetProgressPolicy: bars at any time unless the host says otherwise
unsigned g_progAfterMs = 0;
uint64_t g_progFrame = 0;             // the command buffer being executed
double g_progBurstStart = -1e9, g_progLast = -1e9;

// GXV_SIM_COMPILE_MS: what a compile would have cost on the console, with the on-disk
// cache vitaGL keeps (key: a hash of the shader text; a hit costs nothing here).
void SimulateCompile(const char* text) {
    static const long simMs = DiagnosticInt("GXV_SIM_COMPILE_MS", 0);
    if (simMs <= 0) return;
    static const char* const dir = DiagnosticEnv("GXV_SIM_CACHE");
    if (dir && *dir) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/%016llx.bin", dir, static_cast<unsigned long long>(HashKey(text, static_cast<uint32_t>(std::strlen(text)))));
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            ++g_sw.simHits;
            ++g_stats.shaderSimCacheHits;
            return;
        }
        if (FILE* f = std::fopen(path, "wb")) { std::fputc(1, f); std::fclose(f); }
    }
    g_sw.simPending += static_cast<double>(simMs);
#if !defined(__VGX_VITA__)
    static const bool sleep = DiagnosticInt("GXV_SIM_COMPILE_SLEEP", 0) != 0;
    if (sleep) {
        // (a system call with a relative time: the harness's virtual clock does not shorten it)
        struct timespec ts = {static_cast<time_t>(simMs / 1000), static_cast<long>(simMs % 1000) * 1000000L};
        nanosleep(&ts, nullptr);
    }
#endif
}

uint64_t g_glAllocFailSerial = ~uint64_t{0};
bool GlAllocBackoff() { return g_glAllocFailSerial == g_serial; }
// GXV_GL_RETRY (default 1; from the Strikers port, a9c11e0): bounded recovery from a shader that does not compile or a
// program that does not link. A failed compile deletes its shader and a failed link its program (a vitaGL program slot
// was held for ever before), and each is tried again after 60, 120 and 240 frames (at most one such retry per frame for
// shaders and one for programs), four attempts in all; the draws that need them are skipped meanwhile, as before.
// On the console a compile can fail for want of memory in the shader compiler, which a later attempt survives.
// GXV_GL_RETRY=0: the old behaviour (the failed shader keeps its handle, the failed program is kept, never retried).
bool GlRetry() {
    static const bool on = EnvInt("GXV_GL_RETRY", 1) != 0;
    return on;
}
constexpr uint8_t kGlMaxAttempts = 4;
inline uint64_t GlRetryDelay(uint8_t failures) { return 60ull << (failures - 1); }
bool g_compileFailed = false; // the last Compile returned 0 because the source did not compile (GXV_GL_RETRY), not for an allocation
bool BadGlName(GLuint id) {
    // The frozen vitaGL can return UINT_MAX when its program table is full.
#ifdef __VGX_VITA__
    if (id == ~0u) return true;
#endif
    return id == 0;
}

GLuint Compile(GLenum type, const char* text, const char* what) {
    g_compileFailed = false;
    if (GlAllocBackoff()) return 0;
#if VGX_COLDTRACE
    coldtrace::Scope trace(coldtrace::g_recorder,coldtrace::Phase::Compile,coldtrace::IoClass::None,{g_progFrame,0x10000u,type,0,0,0},coldtrace::Origin::Utility);
#endif
    const GLuint s = G(glCreateShader(type));
    if (BadGlName(s)) { g_glAllocFailSerial = g_serial; return 0; }
    const char* parts[2] = {"#version 100\n", text};
    G(glShaderSource(s, 2, parts, nullptr));
    G(glCompileShader(s));
    ++C.shaderCompiles;
    ++g_sw.compiles;
    ++g_stats.shaderCompiles;
    SimulateCompile(text);
    GLint ok = 0;
    G(glGetShaderiv(s, GL_COMPILE_STATUS, &ok));
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        VGX_ERR("%s shader compile failed:\n%s\n---- source ----\n%s\n", what, log, text);
        if (GlRetry()) {
            G(glDeleteShader(s));
            g_compileFailed = true;
            return 0;
        }
    }
    return s;
}

void SetupNewTexture(GLenum minMag) { // the texture being edited: no mips, clamped
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(minMag)));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(minMag)));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    C.texParams += 4;
}

inline bool KeepRenderTargets();
void EfbDropSamples();
void EfbApplySamples(int samples);
int RungSamples(int rung);
int g_rung = autoq::kToday; // automatic quality's rung (AutoQualityInit, ApplyRung)

// The texture just specified on the active unit (BindTexForEdit + glTexImage2D, w x h) got its memory. On the Vita vitaGL
// reports no error when it has none (the Strikers port's console test, 160c3f9: a framebuffer object with such a texture
// attached crashes the next scene), so its data pointer is asked. Simulated failures for the tests:
//   GXV_SIM_NOSTORAGE_PIXELS=<n>   (diagnostic, any platform) a render target or kept picture of more than n pixels gets none
//   DebugFailAllocation(k)         (tests) the k-th check from now fails
int g_simFailAt = 0, g_simChecks = 0;
bool BoundTexHasStorage(int w, int h) {
    static const long simPixels = DiagnosticInt("GXV_SIM_NOSTORAGE_PIXELS", -1);
    if (simPixels >= 0 && static_cast<long>(w) * h > simPixels) return false;
    if (g_simFailAt > 0 && ++g_simChecks == g_simFailAt) { g_simFailAt = 0; return false; }
#ifdef __VGX_VITA__
    return vglGetTexDataPointer(GL_TEXTURE_2D) != nullptr;
#else
    return true;
#endif
}
void RetireTexture(GLuint id, int w = 0, int h = 0);

// GXV_KEEP_RT (Vita3K): a render target is never deleted (see DeleteTex), so every change of rung used to leave the old
// EFB colour and every resampled texture behind for good; a run that changed the rung every 8 s filled vitaGL's video
// memory after about 30 changes (an allocation failed, then the emulator crashed). The textures retired by a change go to
// these spares instead and are taken again by a later one of the same size (the same memory as the same surface: what the
// emulator's cache can follow), at most kRtSparesMax of them; the rest are forgotten as before. So do the own colour
// textures of pooled objects that are let go (ApplyRung, EvictIdlePool; AcquireFbo takes them).
struct RtSpare { GLuint id; int w, h; };
std::vector<RtSpare> g_rtSpares;
constexpr size_t kRtSparesMax = 64; // (they hold nothing a forgotten texture would not: under GXV_KEEP_RT none is freed)
GLuint TakeRtSpare(int w, int h) {
    for (size_t i = 0; i < g_rtSpares.size(); ++i)
        if (g_rtSpares[i].w == w && g_rtSpares[i].h == h) {
            const GLuint id = g_rtSpares[i].id;
            g_rtSpares.erase(g_rtSpares.begin() + static_cast<std::ptrdiff_t>(i));
            BindTexForEdit(id);
            SetupNewTexture(GL_LINEAR);
            return id;
        }
    return 0;
}

// The EFB target at the size TargetSize gives. The new colour texture is made first, under a new name: when it gets no
// memory nothing changes (the old target, its size and picture stay) and false is returned.
bool CreateTarget() {
    int w = 0, h = 0;
    TargetSize(g_efbW, g_efbH, w, h);
    if (w == g_rtW && h == g_rtH && g_efbFbo) return true;
    GLuint color = KeepRenderTargets() ? TakeRtSpare(w, h) : 0;
    if (!color) {
        G(glGenTextures(1, &color));
        BindTexForEdit(color);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    }
    if (!BoundTexHasStorage(w, h)) {
        G(glDeleteTextures(1, &color));
        ForgetTex(color);
        VGX_ERR("no GPU memory for a %dx%d render target: the EFB target stays %dx%d\n", w, h, g_rtW, g_rtH);
        return false;
    }
    SetupNewTexture(GL_LINEAR);
    EfbDropSamples(); // (the desktop's multisampled objects are of the old size)
    const int oldW = g_rtW, oldH = g_rtH;
    g_rtW = w;
    g_rtH = h;
    GLuint oldColor = g_efbColor;
    if (g_efbFbo && KeepRenderTargets()) {
        // (GXV_KEEP_RT, the emulator: an EFB of a new size is made anew and the old objects are forgotten, never given new
        // storage, as with copy textures (see DeleteTex): Vita3K's surface cache met the freed colour memory again at the
        // new size ("two different surfaces have the same base address"), then failed sceGxmCreateRenderTarget and crashed,
        // after about ten changes of the render scale.) The object itself is deleted: it holds a sceGxm render target,
        // of which there are a few dozen.
        RetireTexture(g_efbColor, oldW, oldH); // (a spare for a later change to this size, or forgotten)
        if (g_efbDepthTex) ForgetTex(g_efbDepthTex);
        G(glDeleteFramebuffers(1, &g_efbFbo));
        if (S.fbo == g_efbFbo) S.fbo = ~0u;
        if (g_inUseFbo == g_efbFbo) g_inUseFbo = ~0u;
        g_efbFbo = g_efbColor = g_efbDepthRb = g_efbDepthTex = 0;
        oldColor = 0; // (forgotten above, never deleted)
    }
    if (!g_efbFbo) G(glGenFramebuffers(1, &g_efbFbo));
    g_efbColor = color;
    BindFbo(g_efbFbo);
    AttachColor(g_efbColor);
    if (oldColor) RetireTexture(oldColor, oldW, oldH); // (detached by the attach above)
    if (g_hasDepthTex) {
        // A depth *texture* lets EFB depth copies be sampled.
        if (!g_efbDepthTex) G(glGenTextures(1, &g_efbDepthTex));
        BindTexForEdit(g_efbDepthTex);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
        SetupNewTexture(GL_NEAREST);
        G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_efbDepthTex, 0));
    } else {
        if (!g_efbDepthRb) G(glGenRenderbuffers(1, &g_efbDepthRb));
        G(glBindRenderbuffer(GL_RENDERBUFFER, g_efbDepthRb));
        G(glRenderbufferStorage(GL_RENDERBUFFER, g_hasDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16, w, h));
        G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_efbDepthRb));
    }
    const GLenum st = G(glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (st != GL_FRAMEBUFFER_COMPLETE) VGX_ERR("EFB framebuffer incomplete: 0x%X\n", st);
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(0, 0, 0, 1));
    G(glClearDepthf(1.0f));
    TouchTarget();
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    VGX_LOG("render target %dx%d for EFB %dx%d (depth %s)\n", w, h, g_efbW, g_efbH,
            g_hasDepthTex ? "texture" : (g_hasDepth24 ? "24-bit rb" : "16-bit rb"));
    EfbApplySamples(RungSamples(g_rung));
    return true;
}

// EFB targets of sizes left by automatic quality, kept for a return to that size (the Strikers port, a4b7466): a change of
// size on the console spent ~50 ms making the EFB's new target (its texture, depth and vitaGL's GPU render target), and a
// return to a size used before finds its objects ready. At most three, the Vita only (the emulator keeps no render target
// apart, see KeepRenderTargets; the desktop's multisampled objects belong to the current one). Dropped after 1800 presents
// unused or when less than 24 MiB of video memory is free (EvictIdlePool). GXV_KEEP_TARGETS=0: off.
struct EfbSlot { int w = 0, h = 0; GLuint fbo = 0, color = 0, depthRb = 0, depthTex = 0; uint64_t lastPresent = 0; };
EfbSlot g_efbKept[3];
unsigned g_efbReused = 0;
bool EfbKeepOn() {
#ifdef __VGX_VITA__
    // (GXV_KEEP_TARGETS=2: also under GXV_KEEP_RT, to test the keeping in the emulator; kept targets free nothing there)
    static const long v = EnvInt("GXV_KEEP_TARGETS", 1);
    return v == 2 || (v == 1 && !KeepRenderTargets());
#else
    return false;
#endif
}
void EfbSlotDrop(EfbSlot& k) {
    if (!k.fbo) return;
    if (S.fbo == k.fbo) S.fbo = ~0u;
    if (g_inUseFbo == k.fbo) g_inUseFbo = ~0u;
    if (k.color) { ForgetTex(k.color); G(glDeleteTextures(1, &k.color)); }
    if (k.depthTex) { ForgetTex(k.depthTex); G(glDeleteTextures(1, &k.depthTex)); }
    if (k.depthRb) G(glDeleteRenderbuffers(1, &k.depthRb));
    G(glDeleteFramebuffers(1, &k.fbo));
    k = EfbSlot{};
}
bool EfbSlotTake(int w, int h, EfbSlot& out) {
    for (EfbSlot& k : g_efbKept)
        if (k.fbo && k.w == w && k.h == h) { out = k; k = EfbSlot{}; return true; }
    return false;
}
void EfbSlotPut(const EfbSlot& cur) {
    EfbSlot* dst = nullptr;
    for (EfbSlot& k : g_efbKept) if (!k.fbo) { dst = &k; break; }
    if (!dst) {
        dst = &g_efbKept[0];
        for (EfbSlot& k : g_efbKept) if (k.lastPresent < dst->lastPresent) dst = &k;
        EfbSlotDrop(*dst);
    }
    *dst = cur;
}

// In front of g_programs: the program last asked for in each of 64 classes (entries of an
// unordered_map do not move; EvictPrograms empties this when it deletes any).
struct ProgramFront { uint32_t key; Program* p; };
ProgramFront g_programFront[1024] = {}; // (1024 like the shader and texture fronts: 64 collided for most of a stage's ~200 programs)

// vitaGL has room for 1,024 linked programs and its glCreateProgram does not fail gracefully
// when they are used up (custom_shaders.c: the slot index stays 0xFFFFFFFF). A tour of the
// game's 44 galaxies, first star of each, makes 736; a long session would pass the limit. So
// at the start of a frame, with more than GXV_MAXPROGRAMS linked (default 900 on the console,
// no limit elsewhere), the programs that have gone longest without a draw are deleted, a
// hundred at a time, never one drawn with in the last 600 frames. A deleted program is linked
// again when a draw asks for it (its shaders stay compiled).
void EvictPrograms() {
#ifdef __VGX_VITA__
    static const size_t limit = static_cast<size_t>(EnvInt("GXV_MAXPROGRAMS", 900));
#else
    static const size_t limit = static_cast<size_t>(EnvInt("GXV_MAXPROGRAMS", 0));
#endif
    if (!limit || g_programs.size() <= limit) return;
    static std::vector<std::pair<uint64_t, uint32_t>> old; // (last use/link, key)
    old.clear();
    for (const auto& kv : g_programs)
        if (kv.second.usedSerial + 600 < g_serial)
            old.emplace_back(std::max(kv.second.usedSerial, kv.second.linkSerial), kv.first);
    if (old.empty()) return;
    std::sort(old.begin(), old.end());
    const size_t want = g_programs.size() - (limit > 100 ? limit - 100 : 0);
    size_t n = 0;
    for (; n < old.size() && n < want; ++n) {
        auto it = g_programs.find(old[n].second);
        if (it->second.id) {
            if (S.program == it->second.id) S.program = ~0u;
            G(glDeleteProgram(it->second.id));
            if (g_stats.programs) --g_stats.programs;
        }
        g_programs.erase(it);
    }
    for (ProgramFront& f : g_programFront) f = ProgramFront{};
    VGX_LOG("%u programs deleted (unused for 600 frames or more); %u linked now\n", static_cast<unsigned>(n), static_cast<unsigned>(g_programs.size()));
}

// ---- shaders: registered when the command says so, compiled then or at the first program that needs them
// (deferred warm-up, see SetWarmBudget in vgx_cmd.h: a shader of the warm-up list is registered and queued, and the compile happens in
// a slice of a later frame, or in GetProgram if a draw meets it first)
std::vector<uint8_t> g_vsPending, g_fsPending;  // by id: registered, not compiled yet
struct ShaderAttempt { uint8_t failures = 0; uint64_t retryAfter = 0; }; // (GXV_GL_RETRY) failed compiles of a pending shader
std::vector<ShaderAttempt> g_vsAttempts, g_fsAttempts;
uint64_t g_shaderRetryFrame = ~uint64_t{0}, g_programRetryFrame = ~uint64_t{0}; // (GXV_GL_RETRY) frame of the last retry
void RegisterShader(const ShaderSource* s) {
    auto& ids = s->fragment ? g_fs : g_vs;
    auto& srcs = s->fragment ? g_fsSrc : g_vsSrc;
    auto& pend = s->fragment ? g_fsPending : g_vsPending;
    auto& attempts = s->fragment ? g_fsAttempts : g_vsAttempts;
    if (ids.size() <= s->id) { ids.resize(s->id + 1, 0); srcs.resize(s->id + 1, nullptr); }
    if (pend.size() <= s->id) pend.resize(s->id + 1, 0);
    if (attempts.size() <= s->id) attempts.resize(s->id + 1);
    ids[s->id] = 0;
    srcs[s->id] = s;
    pend[s->id] = 1;
    attempts[s->id] = ShaderAttempt{};
}
void CompileShader(bool fragment, uint32_t id) {
    auto& pend = fragment ? g_fsPending : g_vsPending;
    if (id >= pend.size() || !pend[id]) return;
    ShaderAttempt* attempt = nullptr;
    if (GlRetry()) {
        auto& attempts = fragment ? g_fsAttempts : g_vsAttempts;
        if (attempts.size() <= id) attempts.resize(id + 1);
        attempt = &attempts[id];
        if (attempt->failures) {
            if (attempt->failures >= kGlMaxAttempts || g_serial < attempt->retryAfter || g_shaderRetryFrame == g_serial) return;
            g_shaderRetryFrame = g_serial;
        }
    }
#if VGX_COLDTRACE
    coldtrace::Scope trace(coldtrace::g_recorder,coldtrace::Key{g_progFrame,CMD_SHADER,id,fragment?1u:0u,0,0},coldtrace::Origin::DirectShader);
#endif
    const ShaderSource* s = (fragment ? g_fsSrc : g_vsSrc)[id];
    const GLuint shader = Compile(fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, s->text.c_str(), fragment ? "fragment" : "vertex");
    if (!shader) {
        // An allocation failure is retryable at once; a source error (GXV_GL_RETRY) after a delay, a bounded number of
        // times. (GXV_GL_RETRY=0: a source error still owns a shader handle and does not come here.)
        if (attempt && g_compileFailed) {
            ++attempt->failures;
            attempt->retryAfter = g_serial + GlRetryDelay(attempt->failures);
            if (attempt->failures >= kGlMaxAttempts)
                VGX_ERR("%s shader %u: no compile in %u attempts, its draws stay skipped\n", fragment ? "fragment" : "vertex", id, kGlMaxAttempts);
        }
        return;
    }
    (fragment ? g_fs : g_vs)[id] = shader;
    pend[id] = 0;
    if (fragment) ++g_stats.fragmentShaders; else ++g_stats.vertexShaders;
}

Program& UnavailableProgram() {
    static Program p;
    p = Program{};
    return p;
}

Program& GetProgram(uint16_t vsId, uint16_t fsId) {
    const uint32_t key = (static_cast<uint32_t>(vsId) << 16) | fsId;
    auto& fr = g_programFront[((vsId * 7u + fsId) * 2654435761u) >> 22];
    if (fr.p && fr.key == key && (fr.p->ok || !GlRetry())) return *fr.p;
    auto it = g_programs.find(key);
    uint8_t failures = 0;
    if (it != g_programs.end()) {
        const Program& old = it->second;
        // (GXV_GL_RETRY) a failed link is linked again when its delay is over, at most one a frame
        if (old.ok || !GlRetry() || old.failures >= kGlMaxAttempts || g_serial < old.retryAfter || g_programRetryFrame == g_serial ||
            GlAllocBackoff()) {
            fr.key = key;
            fr.p = &it->second;
            return it->second;
        }
        failures = old.failures;
    }
    if (GlAllocBackoff()) return UnavailableProgram();
#if VGX_COLDTRACE
    coldtrace::Scope trace(coldtrace::g_recorder,coldtrace::Key{g_progFrame,CMD_PROGRAM,vsId,fsId,0,0},coldtrace::Origin::DirectProgram);
#endif
    Program p;
    p.failures = failures;
    CompileShader(false, vsId);
    CompileShader(true, fsId);
    if (vsId < g_vs.size() && fsId < g_fs.size() && g_vs[vsId] && g_fs[fsId]) {
        if (it != g_programs.end()) g_programRetryFrame = g_serial;
        p.id = G(glCreateProgram());
        if (BadGlName(p.id)) { g_glAllocFailSerial = g_serial; return UnavailableProgram(); }
        p.linkSerial = g_serial;
        G(glAttachShader(p.id, g_vs[vsId]));
        G(glAttachShader(p.id, g_fs[fsId]));
        G(glBindAttribLocation(p.id, AT_POS, "a_pos"));
        G(glBindAttribLocation(p.id, AT_NRM, "a_nrm"));
        G(glBindAttribLocation(p.id, AT_COL0, "a_col0"));
        G(glBindAttribLocation(p.id, AT_COL1, "a_col1"));
        for (int t = 0; t < 8; ++t) {
            char n[16];
            std::snprintf(n, sizeof n, "a_tex%d", t);
            G(glBindAttribLocation(p.id, AT_TEX0 + t, n));
        }
        G(glBindAttribLocation(p.id, AT_TMI0, "a_tmi0"));
        G(glBindAttribLocation(p.id, AT_TMI1, "a_tmi1"));
        {
#if VGX_COLDTRACE
            coldtrace::Scope linkTrace(coldtrace::g_recorder,coldtrace::Phase::Link);
#endif
            G(glLinkProgram(p.id));
        }
        ++C.programLinks;
        ++g_sw.links;
        ++g_stats.programLinks;
        GLint ok = 0;
        G(glGetProgramiv(p.id, GL_LINK_STATUS, &ok));
        if (!ok) {
            char log[2048] = {};
            glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
            VGX_ERR("program link failed (vs %u, fs %u):\n%s\n---- vs ----\n%s\n---- fs ----\n%s\n", vsId, fsId, log,
                    g_vsSrc[vsId]->text.c_str(), g_fsSrc[fsId]->text.c_str());
            if (GlRetry()) {
                // (the program slot is given back; the entry remembers the failure and when to try again)
                G(glDeleteProgram(p.id));
                p.id = 0;
                ++p.failures;
                p.retryAfter = g_serial + GlRetryDelay(p.failures);
                if (p.failures >= kGlMaxAttempts)
                    VGX_ERR("program (vs %u, fs %u): no link in %u attempts, its draws stay skipped\n", vsId, fsId, kGlMaxAttempts);
            }
        } else {
            p.ok = true;
            UseProgram(p.id);
            const ShaderSource& vs = *g_vsSrc[vsId];
            // What the shader text declares (vec4 elements per array).
            const GLint dMv = vs.nMv, dTm = vs.nTm, dPtm = vs.nPtm, dPal = vs.nPal, dLt = vs.nLt, dTev = g_fsSrc[fsId]->nTev;
            p.vsBytes = 64u + 16u * static_cast<uint32_t>(dMv + dTm + dPtm + dPal + dLt);
            p.fsBytes = 16u * static_cast<uint32_t>(dTev);
            if (vs.text.find("a_nrm;") != std::string::npos) p.layoutNeed |= VL_NRM;
            if (vs.text.find("a_col0;") != std::string::npos) p.layoutNeed |= VL_COL0;
            if (vs.text.find("a_col1;") != std::string::npos) p.layoutNeed |= VL_COL1;
            if (vs.text.find("a_tmi0;") != std::string::npos) p.layoutNeed |= VL_TMI;
            for (int t = 0; t < 8; ++t) {
                const char name[8] = {'a', '_', 't', 'e', 'x', static_cast<char>('0' + t), ';', 0};
                if (vs.text.find(name) != std::string::npos) p.layoutNeed |= VL_TEX0 << t;
            }
#ifdef VGX_GL_STUB
            // No GL to ask: take the declarations.
            p.proj = 1;
            if (dMv) { p.mv = 1; p.nMv = dMv; }
            if (dTm) { p.tm = 1; p.nTm = dTm; }
            if (dPtm) { p.ptm = 1; p.nPtm = dPtm; }
            if (dPal) { p.pal = 1; p.nPal = dPal; }
            if (dLt) { p.lt = 1; p.nLt = dLt; }
            p.tev = 1; p.nTev = dTev;
#else
            // Array sizes as the GL reports them (a compiler may shorten an array to its
            // highest used element, and on vitaGL an upload beyond the reported size would
            // write past the array in the program's uniform buffer), never more than declared.
            const auto lim = [](GLint reported, GLint declared) { return reported < declared ? reported : declared; };
            GLint n = 0;
            G(glGetProgramiv(p.id, GL_ACTIVE_UNIFORMS, &n));
            for (GLint i = 0; i < n; ++i) {
                char name[64];
                GLint size = 0;
                GLenum type = 0;
                G(glGetActiveUniform(p.id, static_cast<GLuint>(i), sizeof name, nullptr, &size, &type, name));
                if (char* br = std::strchr(name, '[')) *br = 0;
                const GLint loc = G(glGetUniformLocation(p.id, name));
                if (!std::strcmp(name, "u_proj")) p.proj = loc;
                else if (!std::strcmp(name, "u_mv")) { p.mv = loc; p.nMv = lim(size, dMv); }
                else if (!std::strcmp(name, "u_tm")) { p.tm = loc; p.nTm = lim(size, dTm); }
                else if (!std::strcmp(name, "u_ptm")) { p.ptm = loc; p.nPtm = lim(size, dPtm); }
                else if (!std::strcmp(name, "u_pal")) { p.pal = loc; p.nPal = lim(size, dPal); }
                else if (!std::strcmp(name, "u_lt")) { p.lt = loc; p.nLt = lim(size, dLt); }
                else if (!std::strcmp(name, "u_tev")) { p.tev = loc; p.nTev = lim(size, dTev); }
                else if (!std::strncmp(name, "u_tex", 5)) G(glUniform1i(loc, name[5] - '0'));
            }
#endif
        }
        if (p.id) ++g_stats.programs; // (GXV_GL_RETRY: a failed link holds no GL program)
    } else return UnavailableProgram();
    if (it != g_programs.end()) {
        // (a retried link: the entry keeps its address, which g_programFront may hold)
        it->second = p;
        return it->second;
    }
    return g_programs.emplace(key, p).first->second;
}

GLenum MapBlend(uint8_t f) {
    switch (f) {
    case BF_ZERO: return GL_ZERO;
    case BF_ONE: return GL_ONE;
    case BF_SRC_COLOR: return GL_SRC_COLOR;
    case BF_INV_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case BF_SRC_ALPHA: return GL_SRC_ALPHA;
    case BF_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case BF_DST_ALPHA: return GL_DST_ALPHA;
    case BF_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case BF_DST_COLOR: return GL_DST_COLOR;
    default: return GL_ONE_MINUS_DST_COLOR;
    }
}
const GLenum kCmp[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

// floor(v + 0.5) without the call into the maths library (eight per draw).
inline int Rnd(float v) {
    const float w = v + 0.5f;
    const int i = static_cast<int>(w);
    return i - (w < static_cast<float>(i) ? 1 : 0);
}

// The draw state that was applied last (with the target it was applied for).
RenderState g_rs;
bool g_rsValid = false;

// Returns false when nothing can be drawn (empty scissor).
bool ApplyRenderState(const RenderState& rs) {
    // The state of the last draw, applied for this same target (every change of target or
    // of GL state behind this function's back clears g_rsValid; a state whose scissor
    // came out empty is never remembered): nothing to do.
    if (g_rsValid && SameStruct(rs, g_rs)) return true;
    const float ps = g_inPass ? g_passScale : 1.0f; // (GXV_PASSHALF)
    const float sx = ScaleX() * ps, sy = ScaleY() * ps;
    // Inside an offscreen pass the target is the pass rectangle: same pixels, moved to the origin.
    const int ox = g_inPass ? g_passX : 0, oy = g_inPass ? g_passY : 0;
    const int tw = g_inPass ? g_passW : g_rtW, th = g_inPass ? g_passH : g_rtH;
    int x0 = Rnd(rs.scX * sx) - ox, y0 = Rnd(rs.scY * sy) - oy, x1 = Rnd((rs.scX + rs.scW) * sx) - ox, y1 = Rnd((rs.scY + rs.scH) * sy) - oy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 <= x0 || y1 <= y0) return false;
    // A scissor that covers the whole target is no scissor (and on vitaGL an enabled
    // scissor costs one more full-target draw per update than a disabled one).
    if (x0 == 0 && y0 == 0 && x1 == tw && y1 == th) SetScissor(false);
    else SetScissor(true, x0, y0, x1 - x0, y1 - y0);
    SetViewport(Rnd(rs.vpX * sx) - ox, Rnd(rs.vpY * sy) - oy, Rnd(rs.vpW * sx), Rnd(rs.vpH * sy));
    SetDepthRange(rs.zNear, rs.zFar);
    if (rs.blend) SetBlend(rs.blendEq == BE_REVSUB ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend(rs.blendSrc), MapBlend(rs.blendDst));
    else SetBlendOff();
    if (rs.depthTest) {
        SetDepthTest(true, kCmp[rs.depthFunc & 7]);
        SetDepthMask(rs.depthWrite != 0);
    } else {
        SetDepthTest(false);
        SetDepthMask(false);
    }
    SetCull(rs.cull);
    SetColorMask(rs.colorWrite != 0, rs.alphaWrite != 0);
    g_rs = rs;
    g_rsValid = true;
    return true;
}

void BindTexture(int unit, const TexBind& b) {
    if (b.texId == 0 || b.texId >= g_tex.size() || g_tex[b.texId].id == 0) {
        BindTex(unit, g_whiteTex);
        return;
    }
    Tex& t = g_tex[b.texId];
    BindTex(unit, t.id);
    static const GLenum kWrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    static const GLenum kMin[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                   GL_LINEAR_MIPMAP_LINEAR};
    uint8_t minF = b.minFilter;
    if (!t.mips && minF >= FILT_NEAR_MIP_NEAR) minF = (minF == FILT_NEAR_MIP_NEAR || minF == FILT_NEAR_MIP_LIN) ? FILT_NEAR : FILT_LINEAR;
    if (t.wrapS == b.wrapS && t.wrapT == b.wrapT && t.minF == minF && t.magF == b.magFilter) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    if (t.wrapS != b.wrapS) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kWrap[b.wrapS % 3]))); t.wrapS = b.wrapS; ++C.texParams; }
    if (t.wrapT != b.wrapT) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kWrap[b.wrapT % 3]))); t.wrapT = b.wrapT; ++C.texParams; }
    if (t.minF != minF) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(kMin[minF % 6]))); t.minF = minF; ++C.texParams; }
    if (t.magF != b.magFilter) {
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, b.magFilter == FILT_NEAR ? GL_NEAREST : GL_LINEAR));
        t.magF = b.magFilter;
        ++C.texParams;
    }
}

// The constant an attribute location supplies while its array is disabled is
// fixed per location. It is (re)established whenever the array is switched
// off, because some GL implementations leave the current value undefined
// after a draw that used the array, and once per frame (Execute), because
// vitaGL keeps it in a ring; not on every draw.
inline void SetAttrib(int loc, bool on, GLint size, GLenum type, GLboolean norm, GLsizei stride, size_t offset, float d0, float d1, float d2,
                      float d3) {
    const uint32_t bit = 1u << loc;
    if (on) {
        if (!(S.attrEnabled & bit)) { G(glEnableVertexAttribArray(static_cast<GLuint>(loc))); S.attrEnabled |= bit; ++C.attrToggles; }
        G(glVertexAttribPointer(static_cast<GLuint>(loc), size, type, norm, stride, reinterpret_cast<const void*>(offset)));
        ++C.attrPointers;
    } else {
        if (S.attrEnabled & bit) { G(glDisableVertexAttribArray(static_cast<GLuint>(loc))); S.attrEnabled &= ~bit; S.attrConstSet &= ~bit; ++C.attrToggles; }
        if (!(S.attrConstSet & bit)) { G(glVertexAttrib4f(static_cast<GLuint>(loc), d0, d1, d2, d3)); S.attrConstSet |= bit; ++C.attrConsts; }
    }
}

// Points the attribute arrays at `vbo` with the decoded-vertex layout
// `layoutMask` starting `base` bytes into it. Nothing is done when they
// already are (consecutive draws from one buffer with one layout).
void SetLayout(GLuint vbo, uint32_t layoutMask, uint32_t base) {
    if (S.layoutVbo == vbo && S.layoutMask == layoutMask && S.layoutBase == base) return;
    ++C.layoutChanges;
    BindArrayBuf(vbo);
    const VtxLayout L = MakeLayout(layoutMask);
    const GLsizei st = L.stride;
    SetAttrib(AT_POS, true, 4, GL_FLOAT, GL_FALSE, st, base, 0, 0, 0, 0);
    SetAttrib(AT_NRM, (L.mask & VL_NRM) != 0, 3, GL_FLOAT, GL_FALSE, st, base + L.offNrm, 0, 0, 1, 0);
    SetAttrib(AT_COL0, (L.mask & VL_COL0) != 0, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, base + L.offCol0, 1, 1, 1, 1);
    SetAttrib(AT_COL1, (L.mask & VL_COL1) != 0, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, base + L.offCol1, 1, 1, 1, 1);
    for (int t = 0; t < 8; ++t)
        SetAttrib(AT_TEX0 + t, (L.mask & (VL_TEX0 << t)) != 0, 2, GL_FLOAT, GL_FALSE, st, base + L.offTex[t], 0, 0, 0, 1);
    SetAttrib(AT_TMI0, (L.mask & VL_TMI) != 0, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, base + L.offTmi, 60, 60, 60, 60);
    SetAttrib(AT_TMI1, (L.mask & VL_TMI) != 0, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, base + L.offTmi + 4, 60, 60, 60, 60);
    S.layoutVbo = vbo;
    S.layoutMask = layoutMask;
    S.layoutBase = base;
}

// Program variants (BlendKey / VariantCmd in vgx_cmd.h). vitaGL compares, per draw, the
// blend function and colour mask with what the program was last drawn with and calls the
// shader patcher when they differ (setup_frag_program in custom_shaders.c), and asks its
// vertex program cache for (vertex shader, attributes, streams) on every draw
// (vglCreateVertexProgram). A combination that was never used before is built there and then.
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
std::unordered_set<uint32_t> g_fragVariants; // vsId << 22 ^ fsId << 12 ^ blend key (the patcher also keys on the vertex shader the fragment program links to)
std::unordered_set<uint32_t> g_vtxVariants;  // vsId << 12 | layout mask
#endif
inline void NoteVariant(Program& p, uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey, bool warmCompatible) {
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
    (void)vsId; (void)fsId;
#endif
    p.warmCompatible = warmCompatible;
    if (p.usedSerial != g_serial) { p.usedSerial = g_serial; ++g_progsThisFrame; }
    if (p.layoutNeed & ~layoutMask) {
        ++C.constAttrDraws;
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
        static std::unordered_set<uint32_t> seen;
        static const bool dbg = EnvInt("GXV_CONSTATTR_LOG", 0) != 0;
        if (dbg && seen.insert((p.layoutNeed << 12) | layoutMask).second) std::fprintf(stderr, "[constattr] shader needs %03x, layout has %03x, missing %03x\n", p.layoutNeed, layoutMask, p.layoutNeed & ~layoutMask);
#endif
    }
    if (p.blendKey != blendKey) {
        if (p.blendKey != ~0u) ++C.fragSwitches;
        p.blendKey = blendKey;
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
        if (g_fragVariants.insert((static_cast<uint32_t>(vsId) << 22) ^ (static_cast<uint32_t>(fsId) << 12) ^ blendKey).second) ++C.fragVariants;
#endif
    }
    if (p.layout != layoutMask) {
        p.layout = layoutMask;
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
        if (g_vtxVariants.insert((static_cast<uint32_t>(vsId) << 12) | layoutMask).second) ++C.vtxVariants;
#endif
    }
}

// CMD_VARIANT: one empty triangle (three times vertex 0 of a buffer of zeros) with the
// program, vertex layout, blend function and colour mask a later draw will use, so that
// the console builds the patched programs for it now. Draws nothing anywhere.
GLuint g_warmVbo = 0, g_warmIbo = 0;
void WarmVariant(const VariantCmd& v) {
    Program& p = GetProgram(v.vsId, v.fsId);
    if (!p.ok) return;
    static const bool dedup = EnvInt("GXV_WARM_DEDUP", 1) != 0;
    // Only the linked program's last compatible use is trusted, never the lossy statistic sets.
    const bool compatible = S.fbo == g_efbFbo;
    if (dedup && compatible && p.warmCompatible && p.layout == v.layoutMask && p.blendKey == v.blendKey) {
        NoteVariant(p, v.vsId, v.fsId, v.layoutMask, v.blendKey, true);
        return;
    }
    if (!g_warmVbo) {
        static const uint8_t zeros[256] = {};
        G(glGenBuffers(1, &g_warmVbo));
        G(glGenBuffers(1, &g_warmIbo));
        BindArrayBuf(g_warmVbo);
        G(glBufferData(GL_ARRAY_BUFFER, sizeof zeros, zeros, GL_STATIC_DRAW));
        BindElemBuf(g_warmIbo);
        G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6, zeros, GL_STATIC_DRAW));
    }
    UseProgram(p.id);
    if (v.blendKey & 1u) SetBlend((v.blendKey & 2u) ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend((v.blendKey >> 2) & 15u), MapBlend((v.blendKey >> 6) & 15u));
    else SetBlendOff();
    SetColorMask((v.blendKey & (1u << 10)) != 0, (v.blendKey & (1u << 11)) != 0);
    for (int m = 0; m < 8; ++m) BindTex(m, g_whiteTex);
    SetLayout(g_warmVbo, v.layoutMask, 0);
    BindElemBuf(g_warmIbo);
    TouchTarget();
    {
#if VGX_COLDTRACE
        coldtrace::Scope drawTrace(coldtrace::g_recorder,coldtrace::Phase::Draw);
#endif
        G(glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, nullptr));
    }
    ++C.warmDraws;
    g_rsValid = false; // blend and colour mask no longer are what the last draw's render state says
    NoteVariant(p, v.vsId, v.fsId, v.layoutMask, v.blendKey, compatible);
}

// ---- the deferred warm-up: a queue of the list's commands, worked off at the start of frames
struct WarmItem {
    uint32_t type = 0, arg = 0;
    const ShaderSource* src = nullptr;
    VariantCmd var{};
};
std::deque<WarmItem> g_warm;                    // render thread only
std::atomic<unsigned> g_warmLeft{0}, g_warmAll{0};
std::atomic<double> g_warmBudgetMs{25.0};
std::atomic<unsigned> g_warmSlices{0};
std::atomic<double> g_warmWorstMs{0.0}, g_warmTotalMs{0.0};
inline unsigned WarmUnits(uint32_t type) { return type == CMD_SHADER ? 10u : (type == CMD_PROGRAM ? 2u : 1u); }
void DeferWarm(const CmdBuf& buf, const Cmd& c) {
    WarmItem it;
    it.type = c.type;
    it.arg = c.arg;
    if (c.type == CMD_SHADER) {
        it.src = buf.shaders[c.arg];
        RegisterShader(it.src);  // (known to the draws from now on; compiled when its turn comes or a draw needs it)
    } else if (c.type == CMD_VARIANT) {
        it.var = buf.variants[c.arg];
    }
    g_warm.push_back(it);
    g_warmLeft.fetch_add(WarmUnits(c.type));
    g_warmAll.fetch_add(WarmUnits(c.type));
}
// A slice that takes far longer than its budget means something else has the memory card (the game's stage load reads
// 2 MB archives at 10 MB/s and a shader of the warm-up is read from the same card behind them: one item took 190 ms
// a frame for 5 s, with the game thread waiting for each of those frames, console 18 Oct 23:43). The warm-up then
// stays out of the way for a number of frames that doubles with every slice that is heavy again (8 to 128) and
// goes back to 8 after a light one. GXV_WARM_BACKOFF=0 turns it off.
unsigned g_warmBackoffLeft = 0, g_warmBackoffLen = 8, g_warmBackoffs = 0;
uint64_t g_warmAllocFailSerial = ~uint64_t{0};
const bool g_warmBackoffOn = [] { const char* e = getenv("GXV_WARM_BACKOFF"); return !e || e[0] != '0'; }();
const double g_warmBackoffMs = [] { const char* e = getenv("GXV_WARM_BACKOFF_MS"); return e && atof(e) > 0.0 ? atof(e) : 60.0; }();  // (a lower value to try it in the emulator)
void RunWarm() {
    const double budgetMs = g_warmBudgetMs.load(std::memory_order_relaxed);
    if (g_warm.empty() || budgetMs <= 0.0 || GlAllocBackoff() || g_warmAllocFailSerial == g_serial) return;
    if (g_warmBackoffLeft > 0) { --g_warmBackoffLeft; return; }
#if VGX_COLDTRACE
    coldtrace::SpanScope sliceTrace(coldtrace::g_recorder,coldtrace::SpanKind::WarmSlice);
#endif
    const double t0 = NowMs();
    unsigned done = 0, compiles0 = g_sw.compiles, links0 = g_sw.links;
    while (!g_warm.empty()) {
        const WarmItem it = g_warm.front();
        {
#if VGX_COLDTRACE
        const coldtrace::Key traceKey = it.type==CMD_SHADER ? coldtrace::Key{g_progFrame,it.type,it.src->id,it.src->fragment?1u:0u,0,0} :
            (it.type==CMD_PROGRAM ? coldtrace::Key{g_progFrame,it.type,it.arg>>16,it.arg&0xffffu,0,0} :
             coldtrace::Key{g_progFrame,it.type,it.var.vsId,it.var.fsId,it.var.layoutMask,it.var.blendKey});
        coldtrace::Scope itemTrace(coldtrace::g_recorder,traceKey,coldtrace::Origin::Queued);
#endif
        switch (it.type) {
        case CMD_SHADER: CompileShader(it.src->fragment, it.src->id); break;
        case CMD_PROGRAM: GetProgram(static_cast<uint16_t>(it.arg >> 16), static_cast<uint16_t>(it.arg & 0xFFFFu)); break;
        default: WarmVariant(it.var); break;
        }
        }
        if (GlAllocBackoff()) {
            // Retain warm work, but give demand its own bounded attempt in this Execute.
            g_warmAllocFailSerial = g_serial;
            g_glAllocFailSerial = ~uint64_t{0};
            break;
        }
        g_warm.pop_front();
        g_warmLeft.fetch_sub(WarmUnits(it.type));
        ++done;
        if (NowMs() - t0 >= budgetMs) break;
    }
    const double took = NowMs() - t0;
    if (g_warmBackoffOn) {
        if (took > g_warmBackoffMs && took > 4.0 * budgetMs) {
            g_warmBackoffLeft = g_warmBackoffLen;
            if (++g_warmBackoffs <= 5 || g_warmBackoffs % 50 == 0)
                VGX_LOG("deferred warm-up: a slice took %.0f ms (budget %.0f): it waits %u frames (%u times so far)\n", took, budgetMs, g_warmBackoffLen, g_warmBackoffs);
            if (g_warmBackoffLen < 128) g_warmBackoffLen *= 2;
        } else {
            g_warmBackoffLen = 8;
        }
    }
    const unsigned slices = g_warmSlices.fetch_add(1) + 1;
    g_warmTotalMs.store(g_warmTotalMs.load() + took);
    if (took > g_warmWorstMs.load()) g_warmWorstMs.store(took);
    if (slices <= 3 || g_warm.empty() || slices % 100 == 0)
        VGX_LOG("deferred warm-up: slice %u did %u items in %.0f ms (%u compiles, %u links); %u units left of %u\n", slices, done, took, g_sw.compiles - compiles0,
                g_sw.links - links0, g_warmLeft.load(), g_warmAll.load());
    g_stats.shaderWorkMs += took;
}

// GXV_GLCACHE=0 (diagnostic): issue GL state the way this file did before it cached it
// (program, textures and attribute pointers for every draw, the whole raster state whenever
// any of it changes), for before/after call counts with GXV_GLSTATS.
bool g_noCache = false;

void ValidateDraw(const CmdBuf& buf, const DrawRec& r, const Program& p) {
    validate::BufferView view;
    if (r.vbuf) view = g_validation->Find(r.vbuf);
    else {
        view.indices = buf.idx.data(); view.indexCount = buf.idx.size();
        const size_t chunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
        if (r.vchunk < chunks) {
            const size_t first = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[r.vchunk];
            const size_t last = r.vchunk + 1u < chunks ? buf.vtxChunks[r.vchunk + 1] : buf.vtx.size();
            if (first <= last && last <= buf.vtx.size() && !buf.vtx.empty()) {
                view.vertices = buf.vtx.data() + first; view.vertexBytes = last - first;
            }
        }
    }
    const validate::UniformRange ranges[] = {
        {UB_PROJ, 0, p.proj != -1 ? 16u : 0u},
        {UB_MV, 0, p.mv != -1 ? static_cast<uint32_t>(p.nMv) * 4u : 0u},
        {UB_TEXM, 0, p.tm != -1 ? static_cast<uint32_t>(p.nTm) * 4u : 0u},
        {UB_TEXM, 96, p.ptm != -1 ? static_cast<uint32_t>(p.nPtm) * 4u : 0u},
        {UB_PAL, 0, p.pal != -1 ? static_cast<uint32_t>(p.nPal) * 4u : 0u},
        {UB_LIGHT, 0, p.lt != -1 ? static_cast<uint32_t>(p.nLt) * 4u : 0u},
        {UB_TEV, 0, p.tev != -1 ? static_cast<uint32_t>(p.nTev) * 4u : 0u}
    };
    const ShaderSource* vs = r.vsId < g_vsSrc.size() ? g_vsSrc[r.vsId] : nullptr;
    const ShaderSource* fs = r.fsId < g_fsSrc.size() ? g_fsSrc[r.fsId] : nullptr;
    const bool positionPalette = vs && vs->usesPalette && !vs->nMv;
    const unsigned clampRow = vs && vs->nPal == kPalRowsShort ? kPalShortMaxRow : 61u;
    const bool unavailableStatic = r.vbuf && !view.vertices;
    DrawRec inspected = r;
    if (unavailableStatic) { inspected.idxOffset = 0; inspected.idxCount = 0; }
    const validate::Result result = validate::Inspect(inspected, view, buf.uni.data(), buf.uni.size(), ranges, 7, positionPalette, clampRow);
    ValidationFrame& total = g_validationFrame;
    const uint64_t ordinal = total.draws++;
    total.badDraws += result.errors != 0;
    total.uncheckedDraws += unavailableStatic;
    total.references += result.references;
    total.vertexBytes += result.vertexBytes; total.indexBytes += result.indexBytes; total.uniformBytes += result.uniformBytes;
    const uint64_t words[] = {vs ? vs->keyHash : 0, fs ? fs->keyHash : 0, r.layoutMask, r.prim, r.idxCount, r.vtxCount,
                              result.vertexHash, result.indexHash, result.uniformHash, result.errors, unavailableStatic};
    for (uint64_t word : words) total.hash = validate::HashWord(word, total.hash);
    if (ordinal < 16 || ((result.errors || unavailableStatic) && total.extraLines < 8)) {
        if (ordinal >= 16) ++total.extraLines;
        std::fprintf(stderr, "[validate] frame=%llu draw=%llu source=%s:%u vchunk=%u stride=%u base=%u declared=%u+%u indices=%u+%u minmax=%u:%u buffer_bytes=%llu:%llu "
                             "vs=%016llx fs=%016llx vhash=%016llx ihash=%016llx uhash=%016llx errors=%u unavailable_static=%u bad_layout=%u bad_index_range=%u bad_vertex_refs=%u outside_declared_refs=%u bad_uniform_ranges=%u "
                             "nonfinite_pos_refs=%u nonfinite_attr_refs=%u nonfinite_uniforms=%u nonfinite_blocks=%u,%u,%u,%u,%u,%u bad_palette_refs=%u clamped_palette_refs=%u pos0_bits=%08x,%08x,%08x,%08x\n",
                     static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(ordinal), r.vbuf ? "static" : "stream", r.vbuf,
                     r.vchunk, MakeLayout(r.layoutMask).stride, r.vtxOffset, r.vtxFirst, r.vtxCount, r.idxOffset, r.idxCount, result.minIndex, result.maxIndex,
                     static_cast<unsigned long long>(view.vertexBytes), static_cast<unsigned long long>(view.indexCount * sizeof(uint16_t)),
                     static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]), static_cast<unsigned long long>(result.vertexHash),
                     static_cast<unsigned long long>(result.indexHash), static_cast<unsigned long long>(result.uniformHash), result.errors,
                     unavailableStatic ? 1u : 0u, result.badLayout, result.badIndexRange, result.badVertexRefs, result.outsideDeclaredRefs, result.badUniformRanges, result.nonfinitePositionRefs,
                     result.nonfiniteAttributeRefs, result.nonfiniteUniforms, result.nonfiniteByBlock[UB_PROJ], result.nonfiniteByBlock[UB_MV],
                     result.nonfiniteByBlock[UB_TEXM], result.nonfiniteByBlock[UB_PAL], result.nonfiniteByBlock[UB_LIGHT], result.nonfiniteByBlock[UB_TEV],
                     result.badPaletteRefs, result.clampedPaletteRefs,
                     result.firstPosition[0], result.firstPosition[1], result.firstPosition[2], result.firstPosition[3]);
        std::fflush(stderr);
    }
}

// GXV_PROF=3: where a draw spends its time (program, state, uniforms, textures, layout, call).
inline double DNow() { return NowMs(); }
bool g_dprofOn = false;
double g_dprof[8] = {};
double g_dprofT = 0;
#define DP(i) do { if (g_dprofOn) { const double n_ = DNow(); g_dprof[i] += n_ - g_dprofT; g_dprofT = n_; } } while (0)
void Draw(const CmdBuf& buf, const DrawRec& r) {
    if (g_skipDraws) return;
    if (g_dprofOn) g_dprofT = DNow();
    Program& p = GetProgram(r.vsId, r.fsId);
    DP(0);
    if (!p.ok || r.idxCount == 0) return;
    if (g_validateFrame) ValidateDraw(buf, r, p);
    if (g_noCache) {
        S.program = ~0u;
        S.layoutVbo = ~0u;
        S.unit = -1;
        for (GLuint& t : S.tex) t = ~0u;
        if (!(g_rsValid && std::memcmp(&r.rs, &g_rs, sizeof r.rs) == 0)) {
            S.scissorOn = S.blendOn = S.depthTest = S.depthMask = S.cullOn = S.colorMask = -1;
            S.sc[2] = S.vp[2] = -1;
            S.zn = -1;
            S.blendEq = S.blendSrc = S.depthFunc = S.cullFace = -1;
        }
    }
    GLuint vbo, ibo;
    if (r.vbuf) {
        if (r.vbuf >= g_sbuf.size() || !g_sbuf[r.vbuf].vbo) return;
        vbo = g_sbuf[r.vbuf].vbo;
        ibo = g_sbuf[r.vbuf].ibo;
    } else {
        if (r.vchunk >= g_streamVbo.size()) return;
        vbo = g_streamVbo[r.vchunk];
        ibo = g_ibo;
    }
    if (!ApplyRenderState(r.rs)) return;
    DP(1);
    // The first draw of a scene gives its uniform blocks again when it continues with the
    // program of the last draw before the render-target change: vitaGL would otherwise keep
    // using the buffers it handed to sceGxm in the previous scene (it re-reserves them only
    // after glUseProgram / glUniform*, custom_shaders.c upload_uniforms; gxm.c scene_reset
    // restores the viewport and the region clip, nothing else).
    const bool sceneStart = (S.fbo != g_inUseFbo || g_fbDirty) && S.program == p.id;
    UseProgram(p.id);
    const bool fresh = p.serial != g_serial || p.uniformBuffer != &buf || sceneStart;
    if (sceneStart) ++C.sceneReuploads;
    const float* u = buf.uni.data();
    static const bool uniformReuse = EnvInt("GXV_UNIFORM_REUSE", 1) != 0;
    auto need = [&](int b, GLint words, size_t skip = 0) {
        if (r.ublock[b] == kNoBlock) return false;
        if (fresh) return true;
        if (p.applied[b] == r.ublock[b]) return false;
        if (!uniformReuse) return true;
        const size_t old = p.applied[b];
        const auto inRange = [&](size_t offset) {
            return offset <= buf.uni.size() && skip <= buf.uni.size() - offset &&
                static_cast<size_t>(words) <= buf.uni.size() - offset - skip;
        };
        if (old == kNoBlock || words <= 0 || !inRange(old) || !inRange(r.ublock[b])) return true;
        // Offsets identify arena allocations, not values. A non-adjacent repeat
        // can have a new offset while this program still holds identical bytes.
        const bool different = std::memcmp(u + old + skip, u + r.ublock[b] + skip, static_cast<size_t>(words) * sizeof(float)) != 0;
        if (kBackendDiagnostics) {
            ++C.uniformCompares;
            C.uniformCompareHits += !different;
            C.uniformCompareBytes += static_cast<uint32_t>(words) * sizeof(float);
        }
        return different;
    };
    auto up4 = [&](GLint loc, GLint n, const float* v) {
        G(glUniform4fv(loc, n, v));
        ++C.uniformCalls;
        C.uniformBytes += static_cast<uint32_t>(n) * 16u;
    };
    if (p.proj != -1 && need(UB_PROJ, 16)) {
        G(glUniformMatrix4fv(p.proj, 1, GL_FALSE, u + r.ublock[UB_PROJ]));
        ++C.uniformCalls;
        C.uniformBytes += 64;
        g_uniDirtyV = true;
    }
    if (p.mv != -1 && need(UB_MV, p.nMv * 4)) { up4(p.mv, p.nMv, u + r.ublock[UB_MV]); g_uniDirtyV = true; }
    if (p.tm != -1 && need(UB_TEXM, p.nTm * 4)) { up4(p.tm, p.nTm, u + r.ublock[UB_TEXM]); g_uniDirtyV = true; }
    if (p.ptm != -1 && need(UB_TEXM, p.nPtm * 4, 96)) { up4(p.ptm, p.nPtm, u + r.ublock[UB_TEXM] + 96); g_uniDirtyV = true; }
    if (p.pal != -1 && need(UB_PAL, p.nPal * 4)) {
        up4(p.pal, p.nPal, u + r.ublock[UB_PAL]);
        ++C.palCalls;
        C.palBytes += static_cast<uint32_t>(p.nPal) * 16u;
        g_uniDirtyV = true;
    }
    if (p.lt != -1 && need(UB_LIGHT, p.nLt * 4)) { up4(p.lt, p.nLt, u + r.ublock[UB_LIGHT]); g_uniDirtyV = true; }
    if (p.tev != -1 && need(UB_TEV, p.nTev * 4)) { up4(p.tev, p.nTev, u + r.ublock[UB_TEV]); g_uniDirtyF = true; }
    for (int b = 0; b < UB_COUNT; ++b) {
        if (r.ublock[b] != kNoBlock || fresh) p.applied[b] = r.ublock[b];
    }
    p.serial = g_serial;
    p.uniformBuffer = &buf;
    if (g_uniDirtyV) { C.ringBytes += p.vsBytes; g_uniDirtyV = false; }
    if (g_uniDirtyF) { C.ringBytes += p.fsBytes; g_uniDirtyF = false; }

    DP(2);
    for (int m = 0; m < 8; ++m)
        if (r.texMask & (1u << m)) BindTexture(m, r.tex[m]);
    DP(3);

    SetLayout(vbo, r.layoutMask, r.vtxOffset);
    BindElemBuf(ibo);
    DP(4);

    GLenum mode = GL_TRIANGLES;
    if (r.prim == PRIM_LINES) {
        mode = GL_LINES;
        const float lw = r.rs.lineWidth6 / 6.0f < 1.0f ? 1.0f : r.rs.lineWidth6 / 6.0f;
        if (S.lineWidth != lw) { G(glLineWidth(lw)); ++C.stateCalls; S.lineWidth = lw; }
    } else if (r.prim == PRIM_POINTS) {
        mode = GL_POINTS;
    }
    TouchTarget();
    DP(5);
    G(glDrawElements(mode, static_cast<GLsizei>(r.idxCount), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(static_cast<size_t>(r.idxOffset) * 2)));
    DP(6);
    ++C.draws;
    C.drawIndices += r.idxCount;
    ++g_stats.drawCallsThisFrame;
    NoteVariant(p, r.vsId, r.fsId, r.layoutMask, BlendKey(r.rs), S.fbo == g_efbFbo && r.vtxOffset == 0 && r.prim == PRIM_TRIANGLES);
}

Tex& TexSlotFor(uint32_t id) {
    if (id >= g_tex.size()) g_tex.resize(id + 64);
    return g_tex[id];
}

inline bool KeepRenderTargets() {
    static const bool keep = EnvInt("GXV_KEEP_RT", 0) != 0;
    return keep;
}
void EfbBufferBack(Tex& t); // (GXV_EFBSWAP, below)
// A texture about to be deleted is detached from every pooled framebuffer object first (as the Strikers port found on the
// console, 160c3f9): vitaGL frees a texture's storage only when no framebuffer holds it, so one left attached stayed allocated
// until that object went (with idle pool eviction, or never), and a reused name of the same size could be taken by AcquireFbo
// for "already attached" while vitaGL had dropped it. GXV_DETACH_DELETE=0: the old bookkeeping only.
void DetachFromPool(GLuint name) {
    static const bool on = EnvInt("GXV_DETACH_DELETE", 1) != 0;
    const GLuint prev = S.fbo;
    bool moved = false;
    for (PoolFbo& f : g_pool)
        if (f.attached == name) {
            // (not the target of the scene open now: detaching its colour would end that scene's render target under it;
            // the Strikers port's crash fix, cc302e6. glDeleteTextures handles the texture of the bound one itself.)
            if (on && f.fbo != g_inUseFbo) {
                BindFbo(f.fbo);
                AttachColor(0);
                moved = true;
            }
            f.attached = 0;
        }
    if (moved && prev != ~0u) BindFbo(prev);
}
void DeleteTex(Tex& t) {
    if (!t.id) return;
    if (!t.efbBuf && !(KeepRenderTargets() && t.renderTarget)) DetachFromPool(t.id);
    if (t.efbBuf) { EfbBufferBack(t); return; } // (a buffer the EFB had goes back to its spares)
    // GXV_KEEP_RT=1 (FOR VITA3K ONLY, scripts/run-vita.sh sets it): a texture that was drawn
    // into is never deleted, only forgotten. The emulator keeps every render target it has seen
    // in a cache keyed by guest address and samples from that cache whenever a texture's address
    // lies inside a cached target (renderer/src/gl/surface_cache.cpp), also long after the guest
    // has freed the target and given the memory to an ordinary texture; the same for depth
    // buffers, by exact address (see AcquireFbo). Seen in the castle plaza: the star bit
    // counter's 24x24 "x" got the address of a freed depth buffer and showed as a red square
    // (depth in the red channel). A console samples the memory itself.
    if (!(KeepRenderTargets() && t.renderTarget)) {
        G(glDeleteTextures(1, &t.id));
        ++C.texDeletes;
    }
    ForgetTex(t.id);
    // A pooled framebuffer object that had this texture attached has nothing of ours attached
    // any more. The GL may hand the name out again at once (vitaGL takes the first free slot):
    // if the next texture with this name is a copy destination of the same size, AcquireFbo
    // would take the object as "already attached" and the pass would be drawn into the deleted
    // texture's storage - on vitaGL memory that has been freed and may by then hold another
    // texture (seen as a red square in place of the star bit counter's "x": an alpha copy had
    // been drawn over that 24x24 texture).
    for (PoolFbo& f : g_pool)
        if (f.attached == t.id) f.attached = 0;
    t = Tex{};
}

// DXT5 -> RGBA8 (HD texture replacements, vgx_hdtex.cpp) for a GL without DXT5: the colour
// part decoded as DXT1's four-colour blocks, then the interpolated alpha block.
void DecodeDxt1(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
void DecodeDxt5(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> colour(static_cast<size_t>(bw) * bh * 8);
    for (size_t b = 0; b < static_cast<size_t>(bw) * bh; ++b) {
        std::memcpy(&colour[b * 8], src + b * 16 + 8, 8);
        // (a DXT5 colour block is always read in four-colour mode)
        uint16_t c0, c1;
        std::memcpy(&c0, &colour[b * 8], 2);
        std::memcpy(&c1, &colour[b * 8 + 2], 2);
        if (c0 <= c1) {
            // swap the end points and remap the indices so DXT1 decoding picks the same colours
            std::memcpy(&colour[b * 8], &c1, 2);
            std::memcpy(&colour[b * 8 + 2], &c0, 2);
            for (int i = 4; i < 8; ++i) {
                uint8_t v = colour[b * 8 + i], r = 0;
                for (int k = 0; k < 4; ++k) {
                    static const uint8_t kMap[4] = {1, 0, 3, 2};
                    r |= static_cast<uint8_t>(kMap[(v >> (2 * k)) & 3] << (2 * k));
                }
                colour[b * 8 + i] = r;
            }
            if (c0 == c1) for (int i = 4; i < 8; ++i) colour[b * 8 + i] = 0;
        }
    }
    DecodeDxt1(colour.data(), w, h, out);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* a = src + (static_cast<size_t>(by) * bw + bx) * 16;
            uint8_t pal[8] = {a[0], a[1]};
            if (a[0] > a[1]) for (int i = 1; i < 7; ++i) pal[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
            else { for (int i = 1; i < 5; ++i) pal[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5); pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(a[2 + i]) << (8 * i);
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) out[(static_cast<size_t>(py) * w + px) * 4 + 3] = pal[(bits >> (3 * (y * 4 + x))) & 7];
                }
        }
}

// DXT1 -> RGBA8, for a driver without S3TC (the desktop harness's fallback).
void DecodeDxt1(const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx, src += 8) {
            const uint32_t c0 = src[0] | (src[1] << 8), c1 = src[2] | (src[3] << 8);
            uint8_t pal[4][4];
            const auto dec = [](uint32_t v, uint8_t* o) {
                const uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                o[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); o[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
                o[2] = static_cast<uint8_t>((b << 3) | (b >> 2)); o[3] = 255;
            };
            dec(c0, pal[0]);
            dec(c1, pal[1]);
            for (int k = 0; k < 3; ++k) {
                if (c0 > c1) {
                    pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
                    pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
                } else {
                    pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = 0;
                }
            }
            pal[2][3] = 255;
            pal[3][3] = c0 > c1 ? 255 : 0;
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    std::memcpy(&out[(static_cast<size_t>(py) * w + px) * 4], pal[(src[4 + y] >> (x * 2)) & 3], 4);
                }
        }
}

void Upload(const TexUpload& up) {
#if VGX_COLDTRACE
    coldtrace::Scope uploadTrace(coldtrace::g_recorder,
        coldtrace::Key{g_progFrame,CMD_TEX_UPLOAD,up.texId,up.format,uint32_t(up.width)|(uint32_t(up.height)<<16),
            uint32_t(up.levels.size())|(up.genMips?0x80000000u:0u)},coldtrace::Origin::Texture);
#endif
    Tex& t = TexSlotFor(up.texId);
    if (t.renderTarget && t.id) DeleteTex(t);
    if (!t.id) { G(glGenTextures(1, &t.id)); ++g_stats.textures; }
    BindTexForEdit(t.id);
#if !defined(__VGX_VITA__)
    // LA88 has odd-width two-byte rows. Restore the caller's alignment for this opt-in path.
    // vitaGL copies tightly packed input and rejects GL_UNPACK_ALIGNMENT (textures.c).
    GLint unpackAlignment = 1;
    if (up.format == TF_LA88) G(glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment));
    G(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
#endif
#if defined(__VGX_VITA__)
    // Base first, then grow vitaGL's compressed allocation to the full chain once.
    // No sampling occurs in this loop; every level keeps its data and transfer wait.
    const bool lastSecond = (up.format == TF_DXT1 || up.format == TF_DXT5) && g_hasS3tc && !up.genMips && up.levels.size() > 2
        && up.levels.size() < 16 && !(up.width & (up.width - 1u)) && !(up.height & (up.height - 1u))
        && (uint32_t(up.width) >> (up.levels.size() - 1)) >= 4 && (uint32_t(up.height) >> (up.levels.size() - 1)) >= 4;
#else
    const bool lastSecond = false;
#endif
    std::vector<uint8_t> tmp;
    for (size_t i = 0; i < up.levels.size(); ++i) {
        const size_t l = !lastSecond || i == 0 ? i : (i == 1 ? up.levels.size() - 1 : i - 1);
        const uint32_t w = l == 0 ? up.width : (l < 32 ? std::max(1u, uint32_t(up.width) >> l) : 1u);
        const uint32_t h = l == 0 ? up.height : (l < 32 ? std::max(1u, uint32_t(up.height) >> l) : 1u);
        const std::vector<uint8_t>& lv = up.levels[l];
        if ((up.format == TF_DXT1 && g_hasS3tc) || (up.format == TF_DXT5 && g_hasDxt5)) {
            G(glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l),
                                     up.format == TF_DXT5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, static_cast<GLsizei>(w),
                                     static_cast<GLsizei>(h), 0, static_cast<GLsizei>(lv.size()), lv.data()));
            g_stats.textureBytes += lv.size();
            C.texUploadBytes += static_cast<uint32_t>(lv.size());
        } else {
            const uint8_t* px = lv.data();
            if (up.format == TF_DXT1) { DecodeDxt1(lv.data(), w, h, tmp); px = tmp.data(); }
            if (up.format == TF_DXT5) { DecodeDxt5(lv.data(), w, h, tmp); px = tmp.data(); }
            const GLenum format = up.format == TF_LA88 ? GL_LUMINANCE_ALPHA : GL_RGBA;
            const uint64_t bytes = static_cast<uint64_t>(w) * h * (up.format == TF_LA88 ? 2u : 4u);
            G(glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), format, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, format,
                           GL_UNSIGNED_BYTE, px));
            g_stats.textureBytes += bytes;
            C.texUploadBytes += static_cast<uint32_t>(bytes);
        }
        ++C.texUploads;
    }
#if !defined(__VGX_VITA__)
    if (up.format == TF_LA88) G(glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment));
#endif
    if (up.genMips) {
        G(glGenerateMipmap(GL_TEXTURE_2D));
        ++C.mipGens;
    }
#if !defined(__VGX_VITA__) && !defined(VGX_GL_STUB)
    // Only the authored levels were sent (Caps::mipClamp): a desktop GL needs to be told
    // where the chain ends (sceGxm takes the level count with the texture).
    if (!up.genMips && up.levels.size() > 1) {
        uint32_t lw = up.width, lh = up.height;
        for (size_t l = 1; l < up.levels.size(); ++l) { lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1; }
        if (lw > 1 || lh > 1) {
            if (g_hasMaxLevel) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(up.levels.size() - 1))); ++C.texParams; }
            else VGX_LOG_N(4, "texture with an incomplete mip chain and no GL_TEXTURE_MAX_LEVEL on this GL (GXV_MIPCLAMP needs ES 3): it will not sample\n");
        }
    }
#endif
    t.w = up.width;
    t.h = up.height;
    t.mips = up.levels.size() > 1 || up.genMips;
    t.renderTarget = false;
    t.wrapS = t.wrapT = t.minF = t.magF = 0xFF;
}

bool EnsureCopyProgram() {
    if (g_copyProg) return true;
    if (GlAllocBackoff()) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = u_rect.xy + (a_pos.xy * 0.5 + 0.5) * u_rect.zw; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    static const char* fs =
        "precision mediump float;\nuniform sampler2D u_src;\nuniform mat4 u_mat;\nuniform vec4 u_add;\nvarying vec2 v_uv;\n"
        "void main() { gl_FragColor = u_mat * texture2D(u_src, v_uv) + u_add; }\n";
    // Keep successful work across a transient failure; recompiling costs far more than a retry.
    static GLuint v = 0, f = 0, pendingProgram = 0;
    if (!v) v = Compile(GL_VERTEX_SHADER, vs, "copy vertex");
    if (!f) f = Compile(GL_FRAGMENT_SHADER, fs, "copy fragment");
    if (!v || !f) return false;
    if (!pendingProgram) {
        pendingProgram = G(glCreateProgram());
        if (BadGlName(pendingProgram)) { pendingProgram = 0; g_glAllocFailSerial = g_serial; return false; }
        G(glAttachShader(pendingProgram, v));
        G(glAttachShader(pendingProgram, f));
        G(glBindAttribLocation(pendingProgram, AT_POS, "a_pos"));
    }
    G(glLinkProgram(pendingProgram));
    GLint ok = 0;
    G(glGetProgramiv(pendingProgram, GL_LINK_STATUS, &ok));
    if (!ok) { g_glAllocFailSerial = g_serial; return false; }
    g_copyProg = pendingProgram;
    pendingProgram = 0;
    UseProgram(g_copyProg);
    g_copyRect = G(glGetUniformLocation(g_copyProg, "u_rect"));
    g_copyMat = G(glGetUniformLocation(g_copyProg, "u_mat"));
    g_copyAdd = G(glGetUniformLocation(g_copyProg, "u_add"));
    G(glUniform1i(glGetUniformLocation(g_copyProg, "u_src"), 0));
    return true;
}

// Draws the copy program's quad over the whole viewport: out = m * texel + add.
// Raster state (target, viewport, scissor, masks) is the caller's.
void DrawQuad(GLuint srcTex, float rx, float ry, float rw, float rh, const float* m16, const float* add4) {
    if (!EnsureCopyProgram()) return;
    UseProgram(g_copyProg);
    BindTex(0, srcTex);
    G(glUniform4f(g_copyRect, rx, ry, rw, rh));
    G(glUniformMatrix4fv(g_copyMat, 1, GL_FALSE, m16));
    G(glUniform4f(g_copyAdd, add4[0], add4[1], add4[2], add4[3]));
    C.uniformCalls += 3;
    C.uniformBytes += 96;
    C.ringBytes += 16 + 80;
    g_uniDirtyV = g_uniDirtyF = false;
    // Position only, from the quad's own buffer (2 floats per vertex).
    if (S.layoutVbo != g_quadVbo) {
        ++C.layoutChanges;
        BindArrayBuf(g_quadVbo);
        for (int a = 1; a < AT_COUNT; ++a)
            if (S.attrEnabled & (1u << a)) {
                G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
                S.attrEnabled &= ~(1u << a);
                S.attrConstSet &= ~(1u << a);
                ++C.attrToggles;
            }
        if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; ++C.attrToggles; }
        G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        ++C.attrPointers;
        S.layoutVbo = g_quadVbo;
        S.layoutMask = 0;
        S.layoutBase = 0;
    }
    TouchTarget();
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
}

// The progress bar (SetProgressDisplay in vgx_cmd.h): a dark screen with a framed bar in its
// lower third, drawn with the copy program (a quad of one colour is "0 * texel + colour" over
// a viewport of the bar's size), then shown by the host. Whatever target was bound is bound
// again afterwards and no cached state is trusted: for the commands that follow this is the
// same situation as after the present hook.
void ShowProgress() {
    static const bool enabled = EnvInt("GXV_PROGRESS", 1) != 0;
    if (!enabled || !g_showHook || g_dispW <= 0 || g_dispH <= 0 || g_sw.total == 0) return;
    {
        // (a burst of shader work: the calls come with gaps of less than 1.5 s)
        const double now = NowMs();
        if (now - g_progLast > 1500.0) g_progBurstStart = now;
        g_progLast = now;
        static const long envStartup = EnvInt("GXV_PROGRESS_STARTUP", -1), envAfter = EnvInt("GXV_PROGRESS_AFTER_MS", -1);
        const uint64_t startup = envStartup >= 0 ? static_cast<uint64_t>(envStartup) : g_progStartupFrames;
        const double after = envAfter >= 0 ? static_cast<double>(envAfter) : static_cast<double>(g_progAfterMs);
        if (g_progFrame >= startup && now - g_progBurstStart < after) return;
    }
    const GLuint prevFbo = S.fbo;
    const uint32_t done = g_sw.done < g_sw.total ? g_sw.done : g_sw.total;
    if (!g_hostDrawsProgress) {
        static const float zero[16] = {};
        BindFbo(g_dispFbo);
        SetScissor(false);
        SetColorMask(true, true);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        G(glClearColor(0.02f, 0.02f, 0.04f, 1.0f));
        TouchTarget();
        G(glClear(GL_COLOR_BUFFER_BIT));
        ++C.clears;
        const int w = g_dispW / 2, h = g_dispH / 34 > 6 ? g_dispH / 34 : 6, x = (g_dispW - w) / 2, y = g_dispH / 4;
        const int fw = static_cast<int>(static_cast<uint64_t>(w) * done / g_sw.total);
        static const float frame[4] = {0.45f, 0.45f, 0.50f, 1.0f}, track[4] = {0.06f, 0.06f, 0.09f, 1.0f}, fill[4] = {0.95f, 0.78f, 0.20f, 1.0f};
        SetViewport(x - 2, y - 2, w + 4, h + 4);
        DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, frame);
        SetViewport(x, y, w, h);
        DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, track);
        if (fw > 0) {
            SetViewport(x, y, fw, h);
            DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, fill);
        }
    }
    g_showHook(done, g_sw.total);
    ++g_sw.shown;
    ++g_stats.progressFrames;
    Invalidate();
    g_rsValid = false;
    g_inUseFbo = ~0u; // (the host showed the display: a new scene whatever comes next)
    BindFbo(prevFbo);
}

// ---- the host's own picture (SetPicture in vgx_cmd.h). Two textures taken in turn: the GPU may still be
// reading the one drawn last when the next picture is copied in.
void PicRelease();
GLuint g_picTex[2] = {0, 0};
int g_picW = 0, g_picH = 0, g_picCur = -1;

bool PicSet(const uint32_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0 || !g_copyProg) return false;
    if (g_picW != width || g_picH != height) {
        PicRelease();
        g_picW = width;
        g_picH = height;
    }
    const int next = g_picCur == 0 ? 1 : 0;
    const bool fresh = g_picTex[next] == 0;
    if (fresh) G(glGenTextures(1, &g_picTex[next]));
    BindTexForEdit(g_picTex[next]);
    if (fresh) {
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    } else {
        G(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba));
    }
    g_picCur = next;
    return true;
}

bool PicDraw() {
    if (g_picCur < 0 || !g_picTex[g_picCur] || g_dispW <= 0 || g_dispH <= 0) return false;
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero4[4] = {0, 0, 0, 0};
    const GLuint prevFbo = S.fbo;
    BindFbo(g_dispFbo);
    SetScissor(false);
    SetColorMask(true, true);
    SetBlendOff();
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetViewport(0, 0, g_dispW, g_dispH);
    TouchTarget();
    // (The texture's first row is the picture's top row; the quad's texture rectangle runs bottom-up.)
    DrawQuad(g_picTex[g_picCur], 0.0f, 1.0f, 1.0f, -1.0f, identity, zero4);
    Invalidate();
    g_rsValid = false;
    g_inUseFbo = ~0u;
    BindFbo(prevFbo);
    return true;
}

void PicRelease() {
    for (GLuint& t : g_picTex) {
        if (!t) continue;
        ForgetTex(t);
        G(glDeleteTextures(1, &t));
        t = 0;
    }
    g_picCur = -1;
    g_picW = g_picH = 0;
}

// The clear that goes with a copy: it clears the rectangle that was copied
// (EFB coordinates), not the whole frame buffer. The EFB is bound.
void ClearEfb(const ClearSpec& c, const EfbRect& r);
void Clear(const ClearSpec& c, const EfbRect& r) {
    dplan::g_inClear = true; // (GXV_DEPTHPLAN: the position after the command's own)
    ClearEfb(c, r);
    dplan::g_inClear = false;
}
void ClearEfb(const ClearSpec& c, const EfbRect& r) {
    if (!c.enable) return;
    BindFbo(g_efbFbo);
    const float sx = ScaleX(), sy = ScaleY();
    int x0 = Rnd(r.x * sx), y0 = Rnd(r.y * sy), x1 = Rnd((r.x + r.w) * sx), y1 = Rnd((r.y + r.h) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_rtW) x1 = g_rtW;
    if (y1 > g_rtH) y1 = g_rtH;
    if (x1 <= x0 || y1 <= y0) return;
    g_rsValid = false;
    if (x0 == 0 && y0 == 0 && x1 == g_rtW && y1 == g_rtH) SetScissor(false);
    else SetScissor(true, x0, y0, x1 - x0, y1 - y0);
    bool colorByClear = c.color || c.alpha;
    if (colorByClear && c.color != c.alpha && g_quadClear) {
        // Colour without alpha or alpha without colour: glClear cannot be trusted
        // to honour the colour mask (vitaGL ignores it), a draw can.
        static const float zero[16] = {};
        SetViewport(0, 0, g_rtW, g_rtH);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        SetColorMask(c.color, c.alpha);
        DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, c.rgba);
        ++C.quadClears;
        colorByClear = false;
    }
    GLbitfield mask = 0;
    if (colorByClear) {
        SetColorMask(c.color, c.alpha);
        G(glClearColor(c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]));
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (c.depth) {
        SetDepthMask(true);
        G(glClearDepthf(c.z));
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (mask) {
        // (scissor off: the whole target; the colour mask does not matter for the depth plane)
        TouchTarget((c.depth && x0 == 0 && y0 == 0 && x1 == g_rtW && y1 == g_rtH) ? DN_CLEARED : DN_KEEP);
        G(glClear(mask));
        ++C.clears;
    }
}

// A pooled framebuffer object of w x h that has uses left this frame (bound on return).
// tex: the colour texture to draw into (of exactly this size); 0: the object's own colour
// texture. wantDepth: with a depth buffer. An object that already has `tex` attached is
// taken first and not attached to again: on vitaGL attaching to the object in use ends the
// scene (shared.h _glFramebufferTexture2D), so draws that follow each other into one
// texture (the passes of one atlas) stay one scene only this way.
int AcquireFbo(int w, int h, GLuint tex, bool wantDepth) {
    // (GXV_KEEP_RT, see DeleteTex: vitaGL gives every framebuffer object a hidden depth buffer
    // at its first scene and frees it when a depth attachment is made (framebuffers.c,
    // glFramebufferRenderbuffer); the emulator then shows that freed buffer's depth, as red, in
    // whatever texture is given its address next. With the attachment made before the first
    // scene nothing is freed.)
    if (KeepRenderTargets()) wantDepth = true;
    int idx = -1;
    if (tex) {
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].attached == tex && g_pool[i].w == w && g_pool[i].h == h && (g_pool[i].uses < g_fboMaxUses || g_inUseFbo == g_pool[i].fbo)) {
                idx = static_cast<int>(i);
                break;
            }
    }
    if (idx < 0)
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].w == w && g_pool[i].h == h && g_pool[i].uses < g_fboMaxUses) { idx = static_cast<int>(i); break; }
    if (idx < 0) {
        PoolFbo f;
        f.w = w;
        f.h = h;
        G(glGenFramebuffers(1, &f.fbo));
        g_pool.push_back(f);
        idx = static_cast<int>(g_pool.size() - 1);
        VGX_LOG("framebuffer object %dx%d created (%u pooled)\n", w, h, static_cast<unsigned>(g_pool.size()));
    }
    PoolFbo& f = g_pool[idx];
    BindFbo(f.fbo);
    if (!tex) {
        if (!f.color && KeepRenderTargets()) f.color = TakeRtSpare(w, h); // (GXV_KEEP_RT: one of an object let go)
        if (!f.color) {
            G(glGenTextures(1, &f.color));
            BindTexForEdit(f.color);
            G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            SetupNewTexture(GL_LINEAR);
        }
        tex = f.color;
    }
    f.lastUse = g_presents;
    // A use is a scene: going on drawing into what is attached and in use is not a new one.
    if (!(f.attached == tex && g_inUseFbo == f.fbo && !g_fbDirty)) ++f.uses;
    if (f.attached != tex) {
        AttachColor(tex);
        f.attached = tex;
    }
    if (wantDepth && !f.depthTex && !f.depthRb) {
        // Depth with the same storage as the EFB's, so depth tests come out the same.
        if (g_hasDepthTex) {
            G(glGenTextures(1, &f.depthTex));
            BindTexForEdit(f.depthTex);
            G(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
            SetupNewTexture(GL_NEAREST);
            G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, f.depthTex, 0));
        } else {
            G(glGenRenderbuffers(1, &f.depthRb));
            G(glBindRenderbuffer(GL_RENDERBUFFER, f.depthRb));
            G(glRenderbufferStorage(GL_RENDERBUFFER, g_hasDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16, w, h));
            G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f.depthRb));
        }
    }
    return idx;
}

// The texture a copy writes to: a render-target texture of w x h, which is ew x eh EFB pixels at the render scale
// (kept so that a change of scale can resample it to the size the next copy into it will want: ApplyRung).
Tex& CopyTexture(uint32_t texId, int w, int h, int ew, int eh) {
    Tex& t = TexSlotFor(texId);
    if (t.efbBuf) EfbBufferBack(t); // (GXV_EFBSWAP: written the ordinary way again, into a texture of its own)
    // (GXV_KEEP_RT, see DeleteTex: a render target that changes size gets a new texture there
    // instead of new storage, so that the emulator never meets its old memory again)
    if (KeepRenderTargets() && t.id && t.renderTarget && (t.w != w || t.h != h)) { DeleteTex(t); --g_stats.textures; }
    if (!t.id) { G(glGenTextures(1, &t.id)); ++g_stats.textures; }
    if (!t.renderTarget || t.w != w || t.h != h) {
        BindTexForEdit(t.id);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        SetupNewTexture(GL_LINEAR);
        t.w = static_cast<uint16_t>(w);
        t.h = static_cast<uint16_t>(h);
        t.mips = false;
        t.renderTarget = true;
        t.wrapS = t.wrapT = WRAP_CLAMP;
        t.minF = t.magF = FILT_LINEAR;
    }
    t.ew = static_cast<uint16_t>(ew);
    t.eh = static_cast<uint16_t>(eh);
    return t;
}

// CMD_PASS_BEGIN: the draws up to the next copy go to a target of the pass
// rectangle's size that starts cleared to the values the EFB rectangle held.
// That includes passes over the whole EFB rectangle: they may use one plane
// only (the blur chain's last steps write alpha) while another plane of the
// EFB holds the picture.
void BeginPass(const PassCmd& p) {
    g_staticCrowdPassComplete = false;
    // (GXV_PASSHALF: a pass at a fraction of the render scale; its target, its copies' sizes and its draws' viewports follow)
    const float f = g_passScaleNext;
    g_passScaleNext = 1.0f;
    g_passScale = f;
    const float sx = ScaleX() * f, sy = ScaleY() * f;
    const int lw = f == 1.0f ? g_rtW : Rnd(g_efbW * sx), lh = f == 1.0f ? g_rtH : Rnd(g_efbH * sy);
    int x0 = Rnd(p.rect.x * sx), y0 = Rnd(p.rect.y * sy), x1 = Rnd((p.rect.x + p.rect.w) * sx), y1 = Rnd((p.rect.y + p.rect.h) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > lw) x1 = lw;
    if (y1 > lh) y1 = lh;
    g_pass = -1;
    g_inPass = false;
    g_passDirectTex = 0;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return; // (the draws then go to the EFB with an empty scissor: nothing is drawn)
    const int cx = Rnd(p.atlasX * sx), cy = Rnd(p.atlasY * sy);
    if (p.atlasTex) {
        // A cell of an atlas (see PassCmd): the target is the atlas, the pass rectangle's
        // pixels land in the cell, and the cell is cleared with its gutter.
        const int aw = Rnd(p.atlasW * sx), ah = Rnd(p.atlasH * sy);
        const GLuint dest = CopyTexture(p.atlasTex, aw, ah, p.atlasW, p.atlasH).id;
        g_pass = AcquireFbo(aw, ah, dest, true);
        g_passDirectTex = p.directTex;
        g_passW = aw;
        g_passH = ah;
        g_passX = x0 - cx;
        g_passY = y0 - cy;
    } else {
        // The colour buffer: the pooled object's own texture, or (PassCmd::directTex, when the
        // destination has exactly the pass's size at this render scale) the copy's destination.
        static const bool allowDirect = EnvInt("GXV_PASSDIRECT", 1) != 0;
        bool direct = false;
        if (p.directTex && allowDirect) {
            int dw = Rnd(p.rect.w * sx), dh = Rnd(p.rect.h * sy); // what CopyTex would make the destination
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            direct = dw == w && dh == h;
        }
        if (direct) {
            const GLuint dest = CopyTexture(p.directTex, w, h, p.rect.w, p.rect.h).id;
            g_pass = AcquireFbo(w, h, dest, true);
            g_passDirectTex = p.directTex;
        } else {
            g_pass = AcquireFbo(w, h, 0, true);
        }
        g_passW = w;
        g_passH = h;
        g_passX = x0;
        g_passY = y0;
    }
    const GLenum st = G(glCheckFramebufferStatus(GL_FRAMEBUFFER));
    if (st != GL_FRAMEBUFFER_COMPLETE) VGX_LOG_N(8, "pass framebuffer %dx%d incomplete: 0x%X\n", w, h, st);
    if (p.atlasTex) {
        const int gx = Rnd(p.atlasGutter * sx), gy = Rnd(p.atlasGutter * sy);
        SetScissor(true, cx - gx, cy - gy, w + 2 * gx, h + 2 * gy);
    } else {
        SetScissor(false);
    }
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(p.rgba[0], p.rgba[1], p.rgba[2], p.rgba[3]));
    G(glClearDepthf(p.z));
    TouchTarget(DN_CLEARED);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    g_inPass = true;
    g_staticCrowdPassComplete = st == GL_FRAMEBUFFER_COMPLETE;
    g_rsValid = false;
}

void EndPass() { // back to the EFB
    BindFbo(g_efbFbo);
    g_inPass = false;
    g_pass = -1;
    g_passDirectTex = 0;
    g_passScale = 1.0f;
    g_rsValid = false;
}

bool EfbSwap(const CopyTexCmd& c); // (GXV_EFBSWAP, below)
void CopyTex(const CopyTexCmd& c) {
    if (c.skip) { // only its clear matters (a dead copy, or one that is made later from an atlas cell)
        if (c.fromPass && g_inPass) EndPass();
        Clear(c.clear, c.src);
        return;
    }
    if (EfbSwap(c)) return; // (GXV_EFBSWAP)
    if (c.fromPass && g_pass >= 0 && g_passDirectTex == c.texId) {
        // The pass was drawn straight into this texture: nothing to copy. (GXV_PASSDIRECT2: further copies of the pass
        // follow, made from this texture; the pass stays open for them.)
        if (c.passMore) { g_rsValid = false; return; }
        EndPass();
        Clear(c.clear, c.src);
        return;
    }
    const float cs = (c.fromPass && g_pass >= 0) ? g_passScale : 1.0f; // (GXV_PASSHALF: copies of a scaled pass are scaled too)
    const float sx = ScaleX() * cs, sy = ScaleY() * cs;
    int w = Rnd(c.dstW * sx), h = Rnd(c.dstH * sy);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // (The pass that this copy ends keeps its object: g_pass still names it while it is sampled.)
    const GLuint passColor = (c.fromPass && g_pass >= 0)
        ? (g_passDirectTex ? (g_passDirectTex < g_tex.size() ? g_tex[g_passDirectTex].id : 0) : g_pool[g_pass].color) : 0;
    if (c.dstTexW) {
        // Into a rectangle of an atlas texture.
        const int tw = Rnd(c.dstTexW * sx), th = Rnd(c.dstTexH * sy);
        Tex& t = CopyTexture(c.texId, tw, th, c.dstTexW, c.dstTexH);
        AcquireFbo(tw, th, t.id, false);
        SetViewport(Rnd(c.dstX * sx), Rnd(c.dstY * sy), w, h);
    } else {
        Tex& t = CopyTexture(c.texId, w, h, c.dstW, c.dstH);
        AcquireFbo(w, h, t.id, false);
        SetViewport(0, 0, w, h);
    }
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    const bool fromPass = c.fromPass && g_pass >= 0;
    if (c.srcTex) {
        // Out of an atlas: a rectangle of another texture, unchanged.
        float m[16];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) m[col * 4 + r] = c.mat[r][col];
        const GLuint src = (c.srcTex < g_tex.size() && g_tex[c.srcTex].id) ? g_tex[c.srcTex].id : g_whiteTex;
        DrawQuad(src, static_cast<float>(c.src.x) / c.srcTexW, static_cast<float>(c.src.y) / c.srcTexH, static_cast<float>(c.src.w) / c.srcTexW,
                 static_cast<float>(c.src.h) / c.srcTexH, m, c.add);
        ++C.copyQuads;
        BindFbo(g_efbFbo);
        g_rsValid = false;
        return; // (not the end of a pass, and it has no clear)
    }
    if (c.depth && !g_hasDepthTex) {
        G(glClearColor(1, 1, 1, 1));
        TouchTarget();
        G(glClear(GL_COLOR_BUFFER_BIT));
        ++C.clears;
    } else if (c.fill && !c.depth) {
        // The source is one known colour (as stored in the 8-bit EFB): write its converted value.
        float q[4], o[4];
        for (int i = 0; i < 4; ++i) {
            const float v = c.fillRgba[i] < 0 ? 0 : (c.fillRgba[i] > 1 ? 1 : c.fillRgba[i]);
            q[i] = std::floor(v * 255.0f + 0.5f) / 255.0f;
        }
        for (int r = 0; r < 4; ++r) o[r] = c.mat[r][0] * q[0] + c.mat[r][1] * q[1] + c.mat[r][2] * q[2] + c.mat[r][3] * q[3] + c.add[r];
        G(glClearColor(o[0], o[1], o[2], o[3]));
        TouchTarget();
        G(glClear(GL_COLOR_BUFFER_BIT));
        ++C.clears;
    } else {
        float m[16];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) m[col * 4 + r] = c.depth ? (col == 0 ? 1.0f : 0.0f) : c.mat[r][col];
        const GLuint src = c.depth ? g_efbDepthTex : (fromPass ? passColor : g_efbColor);
        if (fromPass) DrawQuad(src, 0.0f, 0.0f, 1.0f, 1.0f, m, c.add);
        else
            DrawQuad(src, static_cast<float>(c.src.x) / g_efbW, static_cast<float>(c.src.y) / g_efbH, static_cast<float>(c.src.w) / g_efbW,
                     static_cast<float>(c.src.h) / g_efbH, m, c.add);
        ++C.copyQuads;
    }
    if (c.passMore && fromPass) {
        // Another copy of this pass follows at once: the pass's object stays named (g_pass)
        // for it. (No clear: the front end only chains copies that have none.)
        g_rsValid = false;
        return;
    }
    EndPass(); // a pass ends with its copy
    Clear(c.clear, c.src);
}

bool VerifyRead(const CopyDispCmd& c); // (GXV_PASSVERIFY, below)
// GXV_CAP_PRESENT=<W>x<H> (desktop diagnostic; the harness's captures, GXV_SHOT / port_vgx_capture): a captured display copy
// is the picture as the console presents it, upscaled to W x H with the sharpened present and the characters' edge
// smoothing (PresentSharpened, PresentCharaAA), not the EFB's own pixels. GXV_CAP_AB=1: the picture is twice as tall, the
// present without the edge smoothing above the one with it (the same frame, for a before / after comparison); =2: three
// tall, the third with the smoothed pixels tinted (as GXV_CHARAA=2 shows them); =3: four tall: none, the FXAA kernel in the
// rectangles, the sub-pixel kernel (GXV_CHARAA_KERNEL=sub) in them, the sub-pixel kernel over the whole picture, HUD included
// (the Strikers port's way; for comparison only). Unset: the EFB's pixels, as before.
int g_capPW = -1, g_capPH = 0;
int g_capAB = 0;
int g_charaTint = -1; // (CapturePresented) the tint of PresentCharaAA for this draw: -1 as GXV_CHARAA says
bool CapturePresentSize() {
    if (g_capPW < 0) {
        g_capPW = 0;
        if (const char* e = std::getenv("GXV_CAP_PRESENT")) {
            int pw = 0, ph = 0;
            if (std::sscanf(e, "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0 && pw <= 4096 && ph <= 2048) { g_capPW = pw; g_capPH = ph; }
        }
        g_capAB = static_cast<int>(std::clamp<long>(EnvInt("GXV_CAP_AB", 0), 0, 3));
    }
    return g_capPW > 0;
}
void CapturePresented(const CopyDispCmd& c, int x, int y, int w, int h) {
    static GLuint fbo = 0, tex = 0;
    const int panels = 1 + g_capAB, pw = g_capPW, ph = g_capPH * panels;
    if (!fbo) {
        G(glGenTextures(1, &tex));
        BindTexForEdit(tex);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        G(glGenFramebuffers(1, &fbo));
        G(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
        G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
        Invalidate();
    }
    const float amount = SharpenAmount(w, h, pw, g_capPH);
    // (flipY false: the target's row 0 shows the picture's top row, so the read-back is top row first, like the EFB's)
    g_charaDisp = &c;
    for (int pass = 0; pass < panels; ++pass) {
        const int dy = pass * g_capPH;
        if (!PresentSharpened(fbo, x, y, w, h, 0, dy, pw, g_capPH, amount, false)) break;
        g_charaTint = g_capAB == 2 && pass == 2 ? 1 : (panels > 1 ? 0 : -1);
        if (g_capAB == 3) { g_charaKernel = pass == 1 ? 0 : 1; g_charaFull = pass == 3; } // (fxaa, sub, sub on the whole picture)
        if (panels == 1 || pass >= 1) PresentCharaAA(fbo, x, y, w, h, 0, dy, pw, g_capPH, amount, false);
    }
    g_charaTint = g_charaKernel = -1;
    g_charaFull = false;
    g_charaDisp = nullptr;
    g_captured.resize(static_cast<size_t>(pw) * ph * 4);
    G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    G(glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, g_captured.data()));
    ++C.readPixels;
    g_capW = pw;
    g_capH = ph;
    Invalidate();
    g_rsValid = false;
    g_inUseFbo = ~0u;
    BindFbo(g_efbFbo);
}
void CopyDisp(const CopyDispCmd& c) {
    if (VerifyRead(c)) { Clear(c.clear, c.src); return; } // (GXV_PASSVERIFY's reference runs: read back, nothing shown)
    if (g_capture || g_presentHook) {
        const float sx = ScaleX(), sy = ScaleY();
        int x = Rnd(c.src.x * sx), y = Rnd(c.src.y * sy), w = Rnd(c.src.w * sx), h = Rnd(c.src.h * sy);
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > g_rtW) w = g_rtW - x;
        if (y + h > g_rtH) h = g_rtH - y;
        BindFbo(g_efbFbo);
        if (w > 0 && h > 0 && g_presentHook) {
            {
#if VGX_COLDTRACE
            coldtrace::Scope presentTrace(coldtrace::g_recorder,coldtrace::Key{g_progFrame,0x10003u},coldtrace::Origin::Present);
#endif
            EfbForRead();
            g_charaDisp = &c; // (PresentCharaAA, called by the hook: this copy's rectangles)
            g_presentHook(g_efbFbo, x, y, w, h);
            g_charaDisp = nullptr;
            }
            // Whatever the hook did to the context: nothing cached is trusted any more,
            // and the EFB is the target again.
            Invalidate();
            g_rsValid = false;
            g_inUseFbo = ~0u; // (the host presented: a new scene whatever comes next)
            dplan::g_afterHook = true;
            BindFbo(g_efbFbo);
        }
        if (w > 0 && h > 0 && g_capture && CapturePresentSize()) {
            EfbForRead();
            CapturePresented(c, x, y, w, h);
        } else if (w > 0 && h > 0 && g_capture) {
            EfbForRead();
            g_captured.resize(static_cast<size_t>(w) * h * 4);
            G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
            G(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_captured.data()));
            ++C.readPixels;
            g_capW = w;
            g_capH = h;
        }
    }
    Clear(c.clear, c.src);
}

// The frame's vertex arena goes into one buffer per chunk (CmdBuf::vtxChunks), its indices into one.
// (gpupass, 23 Oct: how much this copies. vitaGL's glBufferData with GL_STREAM_DRAW allocates the size from its main-memory
// pool and copies the data in (buffers.c glNamedBufferData; the old block goes to the garbage collector). Measured with
// GXV_GLSTATS on the desktop build in the post-attack castle plaza: 2 uploads and 83-91 KB per picture (streamUploads,
// streamBytes): the plaza's geometry is almost all in the display-list cache's static buffers. That is about 0.1 ms of
// the back end's thread (core 1, which has slack) and nothing on the GPU, which reads the same bytes either way. Writing
// the vertices straight into GPU-mapped memory (vglAllocFromScratch / vglBufferData, as mkwiita does) would need the front
// end to fill memory whose lifetime is fenced to the GPU's use up to N pictures later; not worth it for these volumes.)
void UploadStream(const CmdBuf& buf) {
#if VGX_COLDTRACE
    coldtrace::Scope streamTrace(coldtrace::g_recorder,coldtrace::Key{buf.frame,0x10002u},coldtrace::Origin::Stream);
#endif
    const size_t nChunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
    while (g_streamVbo.size() < nChunks) {
        GLuint b = 0;
        G(glGenBuffers(1, &b));
        g_streamVbo.push_back(b);
    }
    if (!buf.vtx.empty()) {
        for (size_t i = 0; i < nChunks; ++i) {
            const size_t a = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[i];
            const size_t b = i + 1 < nChunks ? buf.vtxChunks[i + 1] : buf.vtx.size();
            if (b <= a) continue;
            BindArrayBuf(g_streamVbo[i]);
            G(glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(b - a), buf.vtx.data() + a, GL_STREAM_DRAW));
            ++C.streamUploads;
            C.streamBytes += static_cast<uint32_t>(b - a);
        }
    }
    if (!buf.idx.empty()) {
        BindElemBuf(g_ibo);
        G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(buf.idx.size() * 2), buf.idx.data(), GL_STREAM_DRAW));
        ++C.streamUploads;
        C.streamBytes += static_cast<uint32_t>(buf.idx.size() * 2);
    }
}

#ifdef VGX_GL_STUB
// GXV_GLMEM=1 (stub-GL builds): the memory the driver has been asked to hold, by kind
// (src/glstub/GLES2/gl2.h), printed every 300 frames and whenever the total passes its
// previous peak by 1 MiB. On the Vita all of it comes out of vitaGL's video-memory pool
// first. Not in it: what vitaGL allocates for itself (display buffers, rings, patcher) and
// the previous frames' stream buffers it has not freed yet.
void ReportGlMem(uint64_t frame) {
    static const bool on = DiagnosticInt("GXV_GLMEM", 0) != 0;
    if (!on) return;
    static uint64_t peak = 0;
    uint64_t b[VGX_STUB_KINDS];
    uint32_t n[VGX_STUB_KINDS];
    vgx_stub_mem(b, n);
    uint64_t total = 0;
    for (int i = 1; i < VGX_STUB_KINDS; ++i) total += b[i];
    const bool newPeak = total > peak + (1u << 20);
    if (!newPeak && frame % 300 != 0) return;
    if (total > peak) peak = total;
    const auto M = [](uint64_t v) { return v / 1048576.0; };
    std::fprintf(stderr, "[glmem] f=%llu total %.1f MiB%s: textures RGBA %.1f (%u) + DXT1 %.1f (%u) + LA88 %.1f (%u), render-target colour %.1f (%u) + depth %.1f (%u in textures, %u renderbuffers), "
                         "buffers static %.1f (%u) + static given as dynamic %.1f (%u) + stream %.1f (%u); %u pooled framebuffer objects\n",
                 static_cast<unsigned long long>(frame), M(total), newPeak ? " (NEW PEAK)" : "", M(b[VGX_STUB_TEX_RGBA]), n[VGX_STUB_TEX_RGBA], M(b[VGX_STUB_TEX_DXT]),
                 n[VGX_STUB_TEX_DXT], M(b[VGX_STUB_TEX_LA]), n[VGX_STUB_TEX_LA], M(b[VGX_STUB_RT_COLOR]), n[VGX_STUB_RT_COLOR], M(b[VGX_STUB_RT_DEPTH] + b[VGX_STUB_RB]), n[VGX_STUB_RT_DEPTH], n[VGX_STUB_RB],
                 M(b[VGX_STUB_BUF_STATIC]), n[VGX_STUB_BUF_STATIC], M(b[VGX_STUB_BUF_DYNAMIC]), n[VGX_STUB_BUF_DYNAMIC], M(b[VGX_STUB_BUF_STREAM]), n[VGX_STUB_BUF_STREAM],
                 static_cast<unsigned>(g_pool.size()));
    // GXV_GLMEM=2: the pooled objects too (size, own colour texture, depth buffer, uses this frame)
    if (DiagnosticInt("GXV_GLMEM", 0) >= 2 && frame % 300 == 0) {
        std::fprintf(stderr, "[glmem] EFB object %dx%d; pool:", g_rtW, g_rtH);
        for (const PoolFbo& f : g_pool) std::fprintf(stderr, " %dx%d%s%s/%u", f.w, f.h, f.color ? "+c" : "", (f.depthTex || f.depthRb) ? "+d" : "", f.uses);
        std::fprintf(stderr, "\n");
    }
}
#endif

diagnosticcsv::Log g_glStats;
constexpr size_t kCsvColumns = 1
#define X(n) +1
    VGX_GL_COUNTERS(X)
#undef X
    ;
static_assert(sizeof(diagnosticcsv::Row::data) > kCsvColumns * 21u, "CSV row fits worst-case integer fields");
void WriteCounts(uint64_t frame) {
    if (!kBackendDiagnostics) return;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (const char* p = DiagnosticEnv("GXV_GLSTATS")) {
            g_glStats.Open(p);
            if (!g_glStats.Enabled()) VGX_ERR("backend CSV disabled: unable to initialize capture\n");
            if (g_glStats.Enabled()) {
                diagnosticcsv::Row header;
                header.Add("frame");
#define X(n) header.Add("," #n);
                VGX_GL_COUNTERS(X)
#undef X
                header.Add("\n");
                g_glStats.Append(header);
            }
        }
    }
    if (!g_glStats.Enabled()) return;
    diagnosticcsv::Row row;
    row.Add("%llu", static_cast<unsigned long long>(frame));
#define X(n) row.Add(",%u", C.n);
    VGX_GL_COUNTERS(X)
#undef X
    row.Add("\n");
    g_glStats.Append(row);
    if (frame % 64 == 0) g_glStats.FlushLive();
}

} // namespace

#if !defined(__VGX_VITA__)
namespace {
// GXV_PROGRESS_DUMP=<dir> (testing): a host without a display gets one made here, a
// 480x272 texture, and every bar shown is written to <dir>/progress_<n>.ppm (the first 6 and
// then every 25th).
std::string g_progDumpDir;
uint64_t g_progFrameNow = 0;
void DumpProgress(uint32_t done, uint32_t total) {
    static uint32_t n = 0;
    ++n;
    if (n > 6 && n % 25 != 0) return;
    std::vector<uint8_t> px(static_cast<size_t>(g_dispW) * g_dispH * 4);
    G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    G(glReadPixels(0, 0, g_dispW, g_dispH, GL_RGBA, GL_UNSIGNED_BYTE, px.data()));
    char path[600];
    std::snprintf(path, sizeof path, "%s/progress_%04u_f%llu_%u_of_%u.ppm", g_progDumpDir.c_str(), n, static_cast<unsigned long long>(g_progFrameNow), done, total);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", g_dispW, g_dispH);
        for (int y = g_dispH - 1; y >= 0; --y) // GL row 0 is the bottom row
            for (int x = 0; x < g_dispW; ++x) std::fwrite(&px[(static_cast<size_t>(y) * g_dispW + x) * 4], 1, 3, f);
        std::fclose(f);
    }
}
void MakeProgressDumpDisplay() {
    const char* d = DiagnosticEnv("GXV_PROGRESS_DUMP");
    if (!d || !*d) return;
    g_progDumpDir = d;
    GLuint fbo = 0, tex = 0;
    G(glGenFramebuffers(1, &fbo));
    G(glGenTextures(1, &tex));
    BindTexForEdit(tex);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    SetupNewTexture(GL_NEAREST);
    BindFbo(fbo);
    G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    BindFbo(g_efbFbo);
    SetProgressDisplay(fbo, 480, 272, &DumpProgress);
}
} // namespace
#endif

void ResetCrowd();

// ================================================================ automatic quality (dynamic resolution)
// The controller (vgx_autoq.h) picks a rung of the render-size ladder from the GPU's load; this part applies it,
// between frames only: the EFB target, every render-target texture (resampled, so that a picture kept across frames -
// an atlas cell, a captured screen - is still there at the new size and the next copy into it finds the size it
// expects) and the pooled framebuffer objects (those of the old sizes are let go).
//   GXV_AUTOQ=1|0           the controller on (default on the Vita, where the GPU clock exists; off elsewhere) / off:
//                           the EFB's own size, every build before this one
//   GXV_AUTOQ_LEVEL=<rung>  a fixed rung, 0-8 (testing, no controller): 0 544x388, 1 592x422, 2 640x456, 3 720x480,
//                           4 800x504, 5 880x520, 6 960x544, 7 960x544 with 2x multisampling, 8 960x544 4x (sizes for
//                           the 640x456 EFB; another EFB scales alike)
//   GXV_AUTOQ_MIN=<rung>, GXV_AUTOQ_MAX=<rung>   the lowest / highest rung the controller may use (default 0 / 8)
//   GXV_AUTOQ_MSAA=0        no multisampling rungs (the ladder ends at 960x544); also without them when the vitaGL has no
//                           per-framebuffer multisampling (FBO_MSAA) or GXV_MSAA multisamples everything from start-up
//   GXV_AUTOQ_UP=<percent>  the GPU load a step up may be predicted to lead to (default 78)
//   GXV_AUTOQ_LOG=1         a status line every 10 s (GPU load, back-end CPU, frame interval, rung)
//   GXV_AUTOQ_CYCLE=<s>     (testing, no controller) the rung walks the ladder, one step every <s> seconds: 2 3 4 5 6 5 4 3
//                           2 1 0 1 2 3 ... (every rung, and every change between neighbours, in one run)
//   GXV_AUTOQ_TIMERS=0      the frame fence clock even with a vitaGL that has GPU timers (vgx_autoq.cpp)
//   GXV_AUTOQ_POLL_US=<n>   the frame fence clock's polling period (default 500)
//   GXV_AUTOQ_HITCH_MS / GXV_AUTOQ_GAP_MS / GXV_AUTOQ_SCENE_MS   (testing, the emulator) the controller's hitch, gap and
//                           new-scene limits (default 100 / 250 / 1500 ms)
//   GXV_SHARPEN=auto|off|<amount>   the sharpened present (below, PresentSharpened; read by SharpenAmount)
//   GXV_AUTOQ_VRAM_KIB / GXV_AUTOQ_VRAM_MARGIN_MIB (16) / GXV_AUTOQ_VRAM_ABOVE_MIB (32) / GXV_AUTOQ_VRAM_LOW_MIB (12)
//                           video memory (FreeVram, RungFits)
//   GXV_POOL_IDLE=<presents>   pooled objects unused this long are deleted (default 60; 0 never; EvictIdlePool); for 3
//                           presents after a change of rung, those unused for 2 (the quick trim)
//   GXV_AUTOQ_SPACING=<frames> two changes at least this many frames apart (default 20)
//   GXV_SIM_NOSTORAGE_PIXELS=<n>  (diagnostic) a new render target of more than n pixels "gets no memory": the change is
//                           refused, the old target kept, the controller put back (BoundTexHasStorage)
// Elsewhere: GXV_MSAA and GXV_ANISO (start-up choices, vgx_host_galaxy_vita.cpp).
// GXV_RENDER_SIZE=WxH (read by the glue: Init's arguments) fixes the size and turns all of this off.
// Every change is logged ("[autoq] rung a (WxH) -> b (WxH) ..."), with the render-target memory before and after.
namespace {
std::unique_ptr<autoq::Controller> g_autoq;
bool g_autoqLog = false;
uint64_t g_autoqStatusUs = 0;
uint64_t g_poolEvicted = 0;   // pooled objects deleted by EvictIdlePool (status line)
unsigned g_autoqTimed = 0;    // frames the GPU clock has timed (status line)
int g_forceRung = autoq::kNoRung; // ForceQualityRung (-1 is the target-only emergency rung)
long g_cycleSec = 0;          // GXV_AUTOQ_CYCLE
uint64_t g_cycleAt = 0;
int g_cycleDir = 1;

void SetRungFactors(int rung) {
    g_rung = rung;
    g_rungSX = static_cast<float>(autoq::RungAt(rung).w) / static_cast<float>(autoq::kBaseW);
    g_rungSY = static_cast<float>(autoq::RungAt(rung).h) / static_cast<float>(autoq::kBaseH);
}

// ---- the EFB's multisampling (see g_efbSamples at the top)
#ifdef __VGX_VITA__
extern "C" GLboolean vglFramebufferMultisample(GLuint framebuffer, GLint samples) __attribute__((weak)); // (FBO_MSAA vitaGL)
#else
extern "C" void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) __attribute__((weak));
extern "C" void glRenderbufferStorageMultisample(GLenum, GLsizei, GLenum, GLsizei, GLsizei) __attribute__((weak));
#endif
int g_msaaTop = 1; // the most samples a pixel the EFB can have here (1: no multisampling rungs; MsaaProbe)

// What this platform allows (AutoQualityInit, before the EFB is made).
int MsaaProbe(const char*& why) {
    why = "";
    if (EnvInt("GXV_AUTOQ_MSAA", 1) == 0) { why = "GXV_AUTOQ_MSAA=0"; return 1; }
#ifdef __VGX_VITA__
    if (!vglFramebufferMultisample) { why = "this vitaGL has no per-framebuffer multisampling (FBO_MSAA)"; return 1; }
    if (EnvInt("GXV_MSAA", 0) >= 2) { why = "GXV_MSAA multisamples every target from start-up"; return 1; }
    return 4;
#else
    if (!glBlitFramebuffer || !glRenderbufferStorageMultisample) { why = "no GLES 3 multisampled renderbuffers"; return 1; }
    if (DiagnosticInt("GXV_PASSVERIFY", 0) != 0) { why = "GXV_PASSVERIFY (its depth model blits the EFB's own objects)"; return 1; }
    while (glGetError() != GL_NO_ERROR) {}
    GLint maxSamples = 0;
    glGetIntegerv(0x8D57, &maxSamples); // GL_MAX_SAMPLES
    if (glGetError() != GL_NO_ERROR) maxSamples = 0;
    if (maxSamples < 2) { why = "GL_MAX_SAMPLES < 2"; return 1; }
    return maxSamples >= 4 ? 4 : 2;
#endif
}

int RungSamples(int rung) {
    const int s = autoq::RungAt(rung).samples;
    return s < g_msaaTop ? s : g_msaaTop;
}
// The highest rung this platform can show (the multisampling rungs only with multisampling).
int TopRung() { return g_msaaTop >= 4 ? autoq::kRungs - 1 : (g_msaaTop >= 2 ? autoq::kTopSingle + 1 : autoq::kTopSingle); }

void RungText(int rung, char* out, size_t n) {
    const int smp = RungSamples(rung);
    if (smp > 1) std::snprintf(out, n, "%ux%u %dx", autoq::RungAt(rung).w, autoq::RungAt(rung).h, smp);
    else std::snprintf(out, n, "%ux%u", autoq::RungAt(rung).w, autoq::RungAt(rung).h);
}

#if !defined(__VGX_VITA__)
void ResolveEfbMs() {
    g_efbMs.dirty = false;
    if (!g_efbMs.fbo) return;
    const GLenum kRead = 0x8CA8, kDraw = 0x8CA9; // GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER
    G(glBindFramebuffer(kRead, g_efbMs.fbo));
    G(glBindFramebuffer(kDraw, g_efbFbo));
    if (S.scissorOn == 1) G(glDisable(GL_SCISSOR_TEST)); // (a blit is scissored)
    glBlitFramebuffer(0, 0, g_rtW, g_rtH, 0, 0, g_rtW, g_rtH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (g_efbMs.depthResolve && g_efbDepthTex) {
        // (the depth texture is what EFB depth copies sample; one sample of each pixel)
        while (glGetError() != GL_NO_ERROR) {}
        glBlitFramebuffer(0, 0, g_rtW, g_rtH, 0, 0, g_rtW, g_rtH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (glGetError() != GL_NO_ERROR) {
            g_efbMs.depthResolve = false;
            VGX_LOG("multisampled EFB: the GL refuses the depth resolve (formats); EFB depth copies keep the last single-sampled depth\n");
        }
    }
    if (S.scissorOn == 1) G(glEnable(GL_SCISSOR_TEST));
    // (the caller binds what comes next)
}
#endif

void EfbForRead() {
#if !defined(__VGX_VITA__)
    if (!g_efbMs.fbo) return;
    if (g_efbMs.dirty) ResolveEfbMs();
    G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo));
    S.fbo = ~0u; // (the next BindFbo(g_efbFbo) binds the multisampled object again)
#endif
}

// The desktop's multisampled objects let go (resolved first); nothing on the Vita (the EFB's object keeps its mode).
void EfbDropSamples() {
#if !defined(__VGX_VITA__)
    if (!g_efbMs.fbo) return;
    if (g_efbMs.dirty) ResolveEfbMs();
    G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo));
    if (S.fbo == g_efbFbo) S.fbo = ~0u;
    G(glDeleteFramebuffers(1, &g_efbMs.fbo));
    G(glDeleteRenderbuffers(1, &g_efbMs.color));
    G(glDeleteRenderbuffers(1, &g_efbMs.depth));
    g_efbMs.fbo = g_efbMs.color = g_efbMs.depth = 0;
    g_efbMs.dirty = false;
#endif
}

// The EFB target with `samples` a pixel, its picture kept; its depth starts cleared (between frames: the display copy has
// cleared it anyway). Called by CreateTarget (a new target) and ApplyRung (a change of multisampling alone).
void EfbApplySamples(int samples) {
    if (g_msaaTop <= 1 || !g_efbFbo) { g_efbSamples = 1; return; } // (GXV_MSAA's start-up choice stays untouched)
    samples = std::clamp(samples, 1, g_msaaTop);
#ifdef __VGX_VITA__
    vglFramebufferMultisample(g_efbFbo, samples);
    g_efbSamples = samples;
#else
    EfbDropSamples();
    g_efbSamples = samples;
    if (samples > 1) {
        G(glGenFramebuffers(1, &g_efbMs.fbo));
        G(glGenRenderbuffers(1, &g_efbMs.color));
        G(glGenRenderbuffers(1, &g_efbMs.depth));
        G(glBindRenderbuffer(GL_RENDERBUFFER, g_efbMs.color));
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, 0x8058, g_rtW, g_rtH); // GL_RGBA8
        GLint got = 0;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, 0x8CAB, &got); // GL_RENDERBUFFER_SAMPLES (a GL may give more)
        VGX_LOG("multisampled EFB %dx%d: %d samples asked, %d given\n", g_rtW, g_rtH, samples, got);
        G(glBindRenderbuffer(GL_RENDERBUFFER, g_efbMs.depth));
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, 0x81A6, g_rtW, g_rtH); // GL_DEPTH_COMPONENT24
        G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbMs.fbo));
        G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_efbMs.color));
        G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_efbMs.depth));
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        S.fbo = ~0u;
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            VGX_ERR("multisampled EFB %dx%d with %d samples incomplete (0x%X): none\n", g_rtW, g_rtH, samples, st);
            G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo));
            G(glDeleteFramebuffers(1, &g_efbMs.fbo));
            G(glDeleteRenderbuffers(1, &g_efbMs.color));
            G(glDeleteRenderbuffers(1, &g_efbMs.depth));
            g_efbMs = EfbMs{};
            g_efbSamples = 1;
            return;
        }
        // The picture so far, drawn in (a blit cannot write a multisampled target).
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
        BindFbo(g_efbFbo);
        SetViewport(0, 0, g_rtW, g_rtH);
        SetScissor(false);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        SetColorMask(true, true);
        DrawQuad(g_efbColor, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero);
    }
#endif
    // The depth of the new target, cleared (the old one's is gone with its samples).
    BindFbo(g_efbFbo);
    SetScissor(false);
    SetDepthMask(true);
    G(glClearDepthf(1.0f));
    TouchTarget();
    G(glClear(GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    g_rsValid = false;
}

GLuint NewRenderTexture(int w, int h) {
    if (KeepRenderTargets())
        if (const GLuint spare = TakeRtSpare(w, h)) return spare;
    GLuint id = 0;
    G(glGenTextures(1, &id));
    BindTexForEdit(id);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    SetupNewTexture(GL_LINEAR);
    return id;
}

// Draws all of `src` over all of `dst` (w x h), bilinear. Through a pooled object of that size whatever its uses this
// frame: a change of scale is rare, and more than 8 scenes on one object only make sceGxm wait for the GPU once.
void Resample(GLuint src, GLuint dst, int w, int h) {
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
    const uint32_t maxUses = g_fboMaxUses;
    g_fboMaxUses = ~0u;
    AcquireFbo(w, h, dst, false);
    g_fboMaxUses = maxUses;
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetColorMask(true, true);
    DrawQuad(src, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero);
    ++C.copyQuads;
}

// A render-target texture that has been replaced (GXV_KEEP_RT, the emulator: forgotten, not deleted; see DeleteTex).
void RetireTexture(GLuint id, int w, int h) {
    if (!id) return;
    if (!KeepRenderTargets()) {
        DetachFromPool(id);
        G(glDeleteTextures(1, &id));
        ++C.texDeletes;
    } else if (w > 0 && h > 0 && g_rtSpares.size() < kRtSparesMax) {
        g_rtSpares.push_back(RtSpare{id, w, h}); // (GXV_KEEP_RT: kept for a later change to this size)
    }
    ForgetTex(id);
    for (PoolFbo& f : g_pool)
        if (f.attached == id) f.attached = 0;
}

// What the render targets hold in video memory (estimate after vitaGL: colour rows of 8-pixel multiples, a depth
// buffer of 4 bytes a sample in 32x32 tiles for the EFB and for every framebuffer object, which vitaGL gives one at its
// first scene whatever is attached; GXV_MSAA (the Vita host's start-up choice) makes that 2 or 4 samples a pixel, the
// multisampling rungs that of the EFB alone: its colour samples live in the GPU's tile memory, only the resolved picture
// is stored).
uint64_t GlobalSamples() {
    static const uint64_t samples = [] {
        const long m = EnvInt("GXV_MSAA", 0);
        return m >= 4 ? 4u : (m >= 2 ? 2u : 1u);
    }();
    return samples;
}
uint64_t DepthBytes(int w, int h, uint64_t samples) {
    return static_cast<uint64_t>((w + 31) & ~31) * static_cast<uint64_t>((h + 31) & ~31) * 4u * samples;
}
uint64_t EfbDepthBytes(int w, int h, int efbSamples) {
    return DepthBytes(w, h, std::max<uint64_t>(GlobalSamples(), static_cast<uint64_t>(efbSamples)));
}
uint64_t RenderTargetBytes() {
    const uint64_t samples = GlobalSamples();
    const auto col = [](int w, int h) { return static_cast<uint64_t>((w + 7) & ~7) * static_cast<uint64_t>(h) * 4u; };
    const auto dep = [samples](int w, int h) { return DepthBytes(w, h, samples); };
    uint64_t b = col(g_rtW, g_rtH) + EfbDepthBytes(g_rtW, g_rtH, g_efbSamples);
    for (const PoolFbo& f : g_pool) b += (f.color ? col(f.w, f.h) : 0u) + dep(f.w, f.h);
    for (const Tex& t : g_tex)
        if (t.id && t.renderTarget) b += col(t.w, t.h);
    return b;
}

void DropEfbSpares(); // (gpupass section: spare EFB colour buffers of the old size are freed at a size change)
uint32_t g_poolFastTrim = 0; // presents left in which pooled objects unused for 2 presents are deleted (after a change)
unsigned g_lastResampled = 0, g_lastPlain = 0; // (tests) render textures of the last size change: resampled / resized plainly
// false: the new EFB target got no memory; the old target, size and rung stay (the caller puts the controller back).
bool ApplyRung(int rung, const char* why) {
    if (rung == g_rung || !g_efbFbo) return true;
    const int fromRung = g_rung, oldW = g_rtW, oldH = g_rtH, oldSamples = g_efbSamples;
    const double t0 = NowMs();
    const uint64_t before = RenderTargetBytes();
    if (g_inPass) EndPass();
    SetRungFactors(rung);
    int nw = 0, nh = 0;
    TargetSize(g_efbW, g_efbH, nw, nh);
    char fromText[32], toText[32];
    RungText(fromRung, fromText, sizeof fromText);
    RungText(rung, toText, sizeof toText);
    if (nw == g_rtW && nh == g_rtH) {
        // Multisampling alone: the EFB's texture (the resolved picture) stays, and so does every other target.
        if (RungSamples(rung) != g_efbSamples) {
            EfbApplySamples(RungSamples(rung));
            g_inUseFbo = ~0u;
            BindFbo(g_efbFbo);
            g_rsValid = false;
            autoq::Log("[autoq] rung %d (%s) -> %d (%s) for EFB %dx%d: %s; EFB samples %d -> %d, its picture kept; render targets %.1f -> %.1f MiB; %.1f ms", fromRung,
                       fromText, rung, toText, g_efbW, g_efbH, why, oldSamples, g_efbSamples, static_cast<double>(before) / 1048576.0,
                       static_cast<double>(RenderTargetBytes()) / 1048576.0, NowMs() - t0);
            g_poolFastTrim = 3;
        }
        return true;
    }
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
    // 1. The EFB: its picture through a texture of the new size, then the target at the new size (CreateTarget clears
    //    colour and depth), then the picture back. (Depth is not kept: the frame's display copy has cleared it.) No
    //    memory for the picture's copy: the new target starts cleared. No memory for the new target: nothing changes.
    GLuint keep = NewRenderTexture(nw, nh);
    if (!BoundTexHasStorage(nw, nh)) {
        G(glDeleteTextures(1, &keep));
        ForgetTex(keep);
        keep = 0;
        autoq::Log("[autoq] no memory to keep the EFB's picture over the change to %dx%d: the new target starts cleared", nw, nh);
    }
    if (keep) Resample(g_efbColor, keep, nw, nh);
    // (the EFB target of the size left is kept, and one of the new size kept earlier is taken back: EfbSlot)
    bool taken = false, stashed = false;
    const EfbSlot cur{g_rtW, g_rtH, g_efbFbo, g_efbColor, g_efbDepthRb, g_efbDepthTex, g_presents};
    if (EfbKeepOn() && g_efbFbo) {
        EfbSlot hit{};
        if (EfbSlotTake(nw, nh, hit)) {
            g_efbFbo = hit.fbo;
            g_efbColor = hit.color;
            g_efbDepthRb = hit.depthRb;
            g_efbDepthTex = hit.depthTex;
            g_rtW = hit.w;
            g_rtH = hit.h;
            EfbSlotPut(cur);
            EfbApplySamples(RungSamples(rung)); // (its mode for this rung; its depth cleared)
            if (!keep) {
                BindFbo(g_efbFbo);
                SetViewport(0, 0, g_rtW, g_rtH);
                SetScissor(false);
                SetColorMask(true, true);
                G(glClearColor(0, 0, 0, 1));
                TouchTarget();
                G(glClear(GL_COLOR_BUFFER_BIT));
                ++C.clears;
            }
            taken = true;
            ++g_efbReused;
        } else {
            EfbSlotPut(cur);
            g_efbFbo = g_efbColor = g_efbDepthRb = g_efbDepthTex = 0; // (CreateTarget makes new objects; the old stay whole)
            stashed = true;
        }
    }
    if (!taken && !CreateTarget()) {
        if (stashed) {
            EfbSlot back{};
            EfbSlotTake(cur.w, cur.h, back);
            g_efbFbo = cur.fbo;
            g_efbColor = cur.color;
            g_efbDepthRb = cur.depthRb;
            g_efbDepthTex = cur.depthTex;
            g_rtW = cur.w;
            g_rtH = cur.h;
        }
        if (keep) RetireTexture(keep, nw, nh);
        SetRungFactors(fromRung);
        g_inUseFbo = ~0u;
        BindFbo(g_efbFbo);
        g_rsValid = false;
        autoq::Log("[autoq] rung %d (%s) -> %d (%s) for EFB %dx%d: %s; NO MEMORY for the %dx%d target: rung %d kept (%dx%d)", fromRung, fromText, rung, toText, g_efbW,
                   g_efbH, why, nw, nh, fromRung, g_rtW, g_rtH);
        return false;
    }
    DropEfbSpares();
    const double tTarget = NowMs(); // (the timing of the parts, for the log: the console's changes took 100-270 ms)
    const size_t oldPool = g_pool.size();
    if (keep) {
        BindFbo(g_efbFbo);
        SetViewport(0, 0, g_rtW, g_rtH);
        SetScissor(false);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        SetColorMask(true, true);
        DrawQuad(keep, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero);
        RetireTexture(keep, nw, nh);
    }
    // 2. Every render-target texture at the size the next copy into it will make (its EFB size at the new scale).
    //    No memory for a second copy (the picture's, while the old one still holds it): the plain resize, the texture
    //    given its new size in place and its picture lost (it starts cleared; a copy into it comes before most reads).
    const float sx = ScaleX(), sy = ScaleY();
    unsigned resampled = 0, plain = 0;
    for (Tex& t : g_tex) {
        // (GXV_EFBSWAP: a texture that holds a colour buffer the EFB had is left as it is: the buffer is the gpupass section's,
        // it goes back to the spares at the next copy into the texture, and a spare of the old size is then deleted)
        if (!t.id || !t.renderTarget || !t.ew || !t.eh || t.efbBuf) continue;
        int w = Rnd(t.ew * sx), h = Rnd(t.eh * sy);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (w == t.w && h == t.h) continue;
        GLuint id = NewRenderTexture(w, h);
        if (!BoundTexHasStorage(w, h)) {
            G(glDeleteTextures(1, &id));
            ForgetTex(id);
            if (KeepRenderTargets()) {
                // (the emulator: a render target is never given new storage, see DeleteTex; a new name, picture lost)
                id = NewRenderTexture(w, h);
                RetireTexture(t.id, t.w, t.h);
                t.id = id;
            } else {
                DetachFromPool(t.id);
                BindTexForEdit(t.id);
                G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
                SetupNewTexture(GL_LINEAR);
                if (!BoundTexHasStorage(w, h)) VGX_ERR("no GPU memory for a %dx%d copy texture even in place\n", w, h);
            }
            ++plain;
        } else {
            Resample(t.id, id, w, h);
            RetireTexture(t.id, t.w, t.h);
            t.id = id;
            ++resampled;
        }
        t.w = static_cast<uint16_t>(w);
        t.h = static_cast<uint16_t>(h);
        t.mips = false;
        t.wrapS = t.wrapT = WRAP_CLAMP;
        t.minF = t.magF = FILT_LINEAR;
    }
    const double tTextures = NowMs();
    // 3. The pooled objects made before the change (their sizes belong to the old scale; each holds a sceGxm render
    //    target, of which the system has a few dozen). Under GXV_KEEP_RT (the emulator) they stay in the pool unless it
    //    has grown past 48: deleting one frees its depth buffer, which the emulator then shows inside whatever texture
    //    gets that memory (see DeleteTex), but a pool that only grows ran out of render targets in a run that changed
    //    the scale every 12 s (sceGxmCreateRenderTarget failed, the emulator crashed).
    //    (Never while a pass is open: a pool index is in use then. ApplyRung runs between frames, where none is.)
    size_t dropped = 0;
    if ((!KeepRenderTargets() || g_pool.size() > 48) && !g_inPass && g_pass < 0) {
        S.fbo = ~0u;
        for (size_t i = 0; i < oldPool; ++i) {
            PoolFbo& f = g_pool[i];
            G(glDeleteFramebuffers(1, &f.fbo));
            if (!KeepRenderTargets()) {
                if (f.color) { ForgetTex(f.color); G(glDeleteTextures(1, &f.color)); }
                if (f.depthTex) { ForgetTex(f.depthTex); G(glDeleteTextures(1, &f.depthTex)); }
                if (f.depthRb) G(glDeleteRenderbuffers(1, &f.depthRb));
            } else if (f.color) {
                RetireTexture(f.color, f.w, f.h); // (GXV_KEEP_RT: a spare for a later object of this size)
            }
        }
        g_pool.erase(g_pool.begin(), g_pool.begin() + static_cast<std::ptrdiff_t>(oldPool));
        dropped = oldPool;
    }
    const double tPool = NowMs();
    g_inUseFbo = ~0u;
    BindFbo(g_efbFbo);
    g_rsValid = false;
    ResetCrowd();
    // 4. A quick trim of the pool for the next presents (the Strikers port, 160c3f9): what the new size does not use goes
    //    after 2 presents instead of GXV_POOL_IDLE.
    g_poolFastTrim = 3;
    g_lastResampled = resampled;
    g_lastPlain = plain;
    const uint64_t after = RenderTargetBytes();
    autoq::Log("[autoq] rung %d (%s, target %dx%d) -> %d (%s, target %dx%d) for EFB %dx%d: %s; %u render textures resampled, %u resized without their picture, "
               "%u framebuffer objects let go; render targets %.1f -> %.1f MiB; %.1f ms (EFB target %.1f%s, render textures %.1f, pool %.1f)",
               fromRung, fromText, oldW, oldH, rung, toText, g_rtW, g_rtH, g_efbW, g_efbH, why, resampled, plain, static_cast<unsigned>(dropped),
               static_cast<double>(before) / 1048576.0, static_cast<double>(after) / 1048576.0, NowMs() - t0, tTarget - t0,
               taken ? ", kept from before" : "", tTextures - tTarget, tPool - tTextures);
    return true;
}

// Video memory (the Strikers port's first console test of its dynamic resolution ran out of it and crashed at
// sceGxmBeginScene). Free VRAM in vitaGL's pool, false when no real figure is known (another platform; the emulator, which
// reports more than it has).
//   GXV_AUTOQ_VRAM_KIB=<n>       (testing, any platform) take n KiB as the free video memory
//   GXV_AUTOQ_VRAM_MARGIN_MIB=<n> what must stay free after a step up, besides the new render targets (default 16)
//   GXV_AUTOQ_VRAM_LOW_MIB=<n>   below this much free, a picture above 640x456 goes back to it (default 12)
bool FreeVram(double& freeMiB) {
    static const long simKib = EnvInt("GXV_AUTOQ_VRAM_KIB", -1);
    if (simKib >= 0) {
        freeMiB = static_cast<double>(simKib) / 1024.0;
        return true;
    }
#ifdef __VGX_VITA__
    const size_t total = vglMemTotal(VGL_MEM_VRAM), free = vglMemFree(VGL_MEM_VRAM);
    if (total == 0 || free > total) return false;
    freeMiB = static_cast<double>(free) / 1048576.0;
    return true;
#else
    return false;
#endif
}

// A step up must not push render targets out of video memory: vitaGL would then put them in main memory (VRAM, then
// RAM, then phycont), where every scene's load and store is slower for the GPU, and when all is used up a target cannot be
// made at all. The new render targets are made while the old ones are still held (vitaGL frees them a few frames later),
// so the step needs the new total free, plus the margin (textures still to be loaded in the scene need room too), and
// never less than the Strikers port's floors: 16 MiB for any step up, 32 MiB for a rung above 640x456.
//   GXV_AUTOQ_VRAM_ABOVE_MIB=<n>   the floor for a rung above 640x456 (default 32); the margin is also the floor below it
// The render targets' memory at rung `want`: everything but the EFB's depth scales with the pixels; the EFB's depth at the
// rung's size and samples.
double ProjectedBytes(int want) {
    const double pix = static_cast<double>(autoq::RungAt(want).w) * autoq::RungAt(want).h /
                       (static_cast<double>(autoq::RungAt(g_rung).w) * autoq::RungAt(g_rung).h);
    int ww = 0, wh = 0;
    {
        const float sx = g_rungSX, sy = g_rungSY;
        g_rungSX = static_cast<float>(autoq::RungAt(want).w) / static_cast<float>(autoq::kBaseW);
        g_rungSY = static_cast<float>(autoq::RungAt(want).h) / static_cast<float>(autoq::kBaseH);
        TargetSize(g_efbW, g_efbH, ww, wh);
        g_rungSX = sx;
        g_rungSY = sy;
    }
    const uint64_t now = RenderTargetBytes(), efbDepthNow = EfbDepthBytes(g_rtW, g_rtH, g_efbSamples);
    return static_cast<double>(now - std::min(now, efbDepthNow)) * pix + static_cast<double>(EfbDepthBytes(ww, wh, RungSamples(want)));
}
bool RungFits(int want, double& needMiB, double& freeMiB) {
    needMiB = freeMiB = 0;
    if (want <= g_rung) return true;
    if (!FreeVram(freeMiB)) return true;
    static const double marginMiB = static_cast<double>(EnvInt("GXV_AUTOQ_VRAM_MARGIN_MIB", 16));
    static const double aboveMiB = static_cast<double>(EnvInt("GXV_AUTOQ_VRAM_ABOVE_MIB", 32));
    needMiB = ProjectedBytes(want) / 1048576.0 + marginMiB;
    needMiB = std::max(needMiB, want > autoq::kToday ? aboveMiB : marginMiB);
    return freeMiB >= needMiB;
}

// Pooled framebuffer objects no frame has used for GXV_POOL_IDLE presents (default 60; 0: never) are deleted, with their
// own colour texture and depth: a pass or copy size the game no longer uses (another scene, another render scale) must not
// hold video memory for the rest of the session (the Strikers port's crash). Attached textures are not touched (they are
// the copies' own; an EFB buffer handed over by GXV_EFBSWAP is never a pooled object's own colour). Under GXV_KEEP_RT (the
// emulator) only while the pool is over 48, and the textures are only forgotten (see DeleteTex).
void EvictIdlePool() {
    ++g_presents;
    static const long idleSetting = EnvInt("GXV_POOL_IDLE", 60);
    if (idleSetting <= 0 || g_inPass || g_pass >= 0) return; // (never while a pass is open: a pool index is in use)
    const long idle = g_poolFastTrim ? 2 : idleSetting;    // (after a change of rung: a quick trim, ApplyRung)
    if (g_poolFastTrim) --g_poolFastTrim;
    if (EfbKeepOn() && (g_presents % 30u) == 0) {
        double freeMiB = 0;
        const bool low = FreeVram(freeMiB) && freeMiB < 24.0;
        for (EfbSlot& k : g_efbKept)
            if (k.fbo && (low || g_presents - k.lastPresent > 1800)) EfbSlotDrop(k);
    }
    if (KeepRenderTargets() && g_pool.size() <= 48) return;
    size_t kept = 0, gone = 0;
    for (size_t i = 0; i < g_pool.size(); ++i) {
        PoolFbo& f = g_pool[i];
        if (f.held || f.fbo == g_inUseFbo || f.fbo == S.fbo || g_presents - f.lastUse <= static_cast<uint64_t>(idle)) { // (a gpupass-held target, or the bound one, is never evicted)
            if (kept != i) g_pool[kept] = f;
            ++kept;
            continue;
        }
        if (S.fbo == f.fbo) S.fbo = ~0u;
        if (g_inUseFbo == f.fbo) g_inUseFbo = ~0u;
        G(glDeleteFramebuffers(1, &f.fbo));
        if (!KeepRenderTargets()) {
            if (f.color) { ForgetTex(f.color); G(glDeleteTextures(1, &f.color)); }
            if (f.depthTex) { ForgetTex(f.depthTex); G(glDeleteTextures(1, &f.depthTex)); }
            if (f.depthRb) G(glDeleteRenderbuffers(1, &f.depthRb));
        } else if (f.color) {
            RetireTexture(f.color, f.w, f.h); // (GXV_KEEP_RT: a spare for a later object of this size)
        }
        ++gone;
    }
    if (!gone) return;
    g_pool.resize(kept);
    g_poolEvicted += gone;
    BindFbo(g_efbFbo);
    g_rsValid = false;
}

// Init, before the EFB target is made: the rung to start with, and the controller when it is on.
void AutoQualityInit() {
#ifdef __VGX_VITA__
    const long onByDefault = 1;
#else
    const long onByDefault = 0; // (no GPU clock: the controller would never move)
#endif
    g_autoqLog = EnvInt("GXV_AUTOQ_LOG", 0) != 0;
    const bool target30 = autoq::Target30Enabled();
    const char* noMsaa = "";
    g_msaaTop = MsaaProbe(noMsaa);
    if (g_msaaTop > 1) autoq::Log("[autoq] the EFB can be multisampled (up to %dx): rungs 7 and 8", g_msaaTop);
    else autoq::Log("[autoq] no multisampling rungs: %s", noMsaa);
    if (g_fixedW > 0 && g_fixedH > 0) {
        autoq::Log("[autoq] off: GXV_RENDER_SIZE fixes the picture at %dx%d", g_fixedW, g_fixedH);
        return;
    }
    g_cycleSec = EnvInt("GXV_AUTOQ_CYCLE", 0); // (a testing switch, also in release builds: the rung changes it makes test the kept targets)
    if (g_cycleSec > 0) {
        autoq::Log("[autoq] GXV_AUTOQ_CYCLE: the rung walks the ladder, a step every %ld s (no controller)", g_cycleSec);
        return;
    }
    const long forced = EnvInt("GXV_AUTOQ_LEVEL", -1);
    if (forced >= 0) {
        const int r = static_cast<int>(std::min<long>(forced, TopRung()));
        SetRungFactors(r);
        char text[32];
        RungText(r, text, sizeof text);
        autoq::Log("[autoq] fixed rung %d by GXV_AUTOQ_LEVEL: %s for a 640x456 EFB (no controller)", r, text);
        return;
    }
    if (EnvInt("GXV_AUTOQ", onByDefault) == 0) {
        autoq::Log("[autoq] off (GXV_AUTOQ=0): the EFB's own size");
        return;
    }
    // GXV_AUTOQ_CALM=1|0: calm mode (default 1: rare changes, steps up in quiet moments, each stage keeps its rung; see
    // vgx_autoq.h) / the rules before it. GXV_AUTOQ_BUDGET_MS=<ms>: calm mode's frame budget (default 33).
    const bool calm = EnvInt("GXV_AUTOQ_CALM", 1) != 0;
    autoq::Config cfg = calm ? autoq::Config() : autoq::Config::Legacy();
    if (const long b = EnvInt("GXV_AUTOQ_BUDGET_MS", 0); calm && b > 0) cfg.budgetMs = static_cast<double>(std::clamp<long>(b, 8, 100));
    if (target30) {
        cfg.targetFps = 30;
        cfg.budgetMs = 1000.0 / 30.0;
        cfg.minRung = autoq::kEmergency;
    }
    cfg.minRung = static_cast<int>(EnvInt("GXV_AUTOQ_MIN", cfg.minRung));
    cfg.maxRung = std::min(static_cast<int>(EnvInt("GXV_AUTOQ_MAX", cfg.maxRung)), TopRung());
    cfg.upTarget = static_cast<double>(EnvInt("GXV_AUTOQ_UP", calm ? 70 : 78)) / 100.0;
    // (testing, the emulator: at a few frames a second every frame is a "hitch" or a "load" to the controller, which then
    // never decides. GXV_AUTOQ_HITCH_MS / _GAP_MS / _SCENE_MS raise those limits: 100 / 250 / 1500 ms by default.)
    cfg.outlierMs = static_cast<double>(EnvInt("GXV_AUTOQ_HITCH_MS", 100));
    cfg.gapResetMs = static_cast<double>(EnvInt("GXV_AUTOQ_GAP_MS", 250));
    cfg.longGapMs = static_cast<double>(EnvInt("GXV_AUTOQ_SCENE_MS", 1500));
    // GXV_AUTOQ_SPACING=<frames>: two changes at least this many frames apart (default 20; the Strikers port, 160c3f9)
    cfg.minChangeFrames = static_cast<int>(EnvInt("GXV_AUTOQ_SPACING", 20));
    if (!autoq::clock::Start()) {
        autoq::Log("[autoq] off: no GPU clock here, the EFB's own size");
        return;
    }
    g_autoq = std::make_unique<autoq::Controller>(cfg);
    const autoq::Config& c = g_autoq->Cfg();
    SetRungFactors(g_autoq->Current());
    if (target30) autoq::Log("[autoq] target 30: whole-picture work (two game turns, renderer CPU, GPU busy; overlapping stages use max); emergency floor 480x340 after a sustained GPU miss at 544x388");
    char lo[32], hi[32], st[32];
    RungText(c.minRung, lo, sizeof lo);
    RungText(c.maxRung, hi, sizeof hi);
    RungText(g_rung, st, sizeof st);
    autoq::Log("[autoq] on: rungs %d (%s) to %d (%s), starting at %d (%s); up when the predicted GPU load is <= %.0f %% for %.1f s%s", c.minRung, lo, c.maxRung, hi,
               g_rung, st, c.upTarget * 100.0, c.upHoldSec,
               c.calm ? "; calm mode: changes of size in quiet moments (pause menu, closed fades), in play only for an overload (or after 120 s of room), against a 33 ms frame budget; each stage keeps its rung" : "");
    if (c.calm && (!port_screen_quiet || !port_scene_key))
        autoq::Log("[autoq] calm mode without %s%s from the game", port_screen_quiet ? "" : "quiet moments ", port_scene_key ? "" : "stage keys");
}

// The start of every frame (Execute): the GPU's finished frames go to the controller, and a new rung is applied now,
// before any of the frame's commands.
// GXV_AUTOQ_LOG=1: every 10 s, the rung, the render targets' memory and (with the controller) its last window.
void AutoQualityStatus() {
    if (!g_autoqLog) return;
    const uint64_t now = autoq::clock::NowUs();
    if (now - g_autoqStatusUs < 10000000u) return;
    g_autoqStatusUs = now;
    char clockCounters[200];
    autoq::clock::Counters(clockCounters, sizeof clockCounters);
    if (g_autoq)
        autoq::Log("[autoq] status: rung %d (%dx%d, %d samples, for EFB %dx%d), render targets %.1f MiB in %u pooled objects (%u let go idle); last window GPU load %.0f %%, back-end CPU %.0f %%, "
                   "%.1f ms a frame; %u frames timed, %u windows, %u changes%s",
                   g_rung, g_rtW, g_rtH, g_efbSamples, g_efbW, g_efbH, static_cast<double>(RenderTargetBytes()) / 1048576.0, static_cast<unsigned>(g_pool.size()), static_cast<unsigned>(g_poolEvicted), g_autoq->WindowLoad() * 100.0,
                   g_autoq->WindowCpu() * 100.0, g_autoq->WindowIntervalMs(), g_autoqTimed, g_autoq->Windows(), g_autoq->Changes(), clockCounters);
    else
        autoq::Log("[autoq] status: rung %d (%dx%d, %d samples, for EFB %dx%d), render targets %.1f MiB in %u pooled objects (no controller)", g_rung, g_rtW, g_rtH,
                   g_efbSamples, g_efbW, g_efbH, static_cast<double>(RenderTargetBytes()) / 1048576.0, static_cast<unsigned>(g_pool.size()));
    if (g_autoq && g_autoq->Cfg().targetFps == 30)
        autoq::Log("[autoq] target 30 work: game two turns %.1f ms, renderer CPU %.1f ms, GPU %.1f ms; overlapping maximum %.1f ms against %.3f ms",
                   g_autoq->WindowGameMs(), g_autoq->WindowBackendMs(), g_autoq->WindowLoad() * g_autoq->Cfg().budgetMs,
                   g_autoq->WindowWorkMs(), g_autoq->Cfg().budgetMs);
    double freeMiB = 0;
    if (FreeVram(freeMiB))
        autoq::Log("[autoq] status: video memory free %.1f MiB (vitaGL's pool); %u retired render textures kept as spares (GXV_KEEP_RT)", freeMiB,
                   static_cast<unsigned>(g_rtSpares.size()));
}

void AutoQualityFrame() {
    AutoQualityStatus();
    if (g_forceRung != autoq::kNoRung) {
        g_autoq.reset();
        g_cycleSec = 0;
        if (!ApplyRung(g_forceRung, "ForceQualityRung")) autoq::Log("[autoq] ForceQualityRung(%d) refused: no memory", g_forceRung);
        g_forceRung = autoq::kNoRung;
        return;
    }
    if (g_cycleSec > 0) {
        const uint64_t now = autoq::clock::NowUs();
        if (g_cycleAt == 0) g_cycleAt = now;
        if (now - g_cycleAt >= static_cast<uint64_t>(g_cycleSec) * 1000000u) {
            g_cycleAt = now;
            if (g_rung + g_cycleDir < 0 || g_rung + g_cycleDir > TopRung()) g_cycleDir = -g_cycleDir;
            if (!ApplyRung(g_rung + g_cycleDir, "GXV_AUTOQ_CYCLE")) g_cycleDir = -g_cycleDir;
        }
        return;
    }
    if (!g_autoq) return;
    autoq::clock::FrameBegin();
    autoq::FrameTimes f;
    int want = autoq::kNoRung;
    // (calm mode: the game's quiet moments and stage, as they are now; the frames handed over are a frame or two older)
    const bool quiet = port_screen_quiet && port_screen_quiet() != 0;
    const uint32_t scene = port_scene_key ? port_scene_key() : 0;
    while (autoq::clock::NextDone(f)) {
        ++g_autoqTimed;
        f.quiet = quiet;
        f.scene = scene;
        if (g_autoq->Add(f)) want = g_autoq->Current();
    }
    double needMiB = 0, freeMiB = 0;
    // Short of video memory above 640x456 (textures of a new scene, other render targets): back to it now.
    static const double lowMiB = static_cast<double>(EnvInt("GXV_AUTOQ_VRAM_LOW_MIB", 12));
    if (g_rung > autoq::kToday && (g_presents % 30u) == 0 && FreeVram(freeMiB) && freeMiB < lowMiB) {
        autoq::Log("[autoq] %.1f MiB of video memory free (under %.0f): back to rung %d", freeMiB, lowMiB, autoq::kToday);
        g_autoq->Drop(autoq::kToday, autoq::clock::NowUs());
        want = g_autoq->Current();
    }
    if (want > g_rung && !RungFits(want, needMiB, freeMiB) && EfbKeepOn()) {
        // (kept EFB targets of other sizes give their memory back before a step up is refused for the lack of it; one of the
        // wanted size stays, it is the step's own target)
        int ww = 0, wh = 0;
        {
            const float sx = g_rungSX, sy = g_rungSY;
            g_rungSX = static_cast<float>(autoq::RungAt(want).w) / static_cast<float>(autoq::kBaseW);
            g_rungSY = static_cast<float>(autoq::RungAt(want).h) / static_cast<float>(autoq::kBaseH);
            TargetSize(g_efbW, g_efbH, ww, wh);
            g_rungSX = sx;
            g_rungSY = sy;
        }
        bool dropped = false;
        for (EfbSlot& k : g_efbKept)
            if (k.fbo && !(k.w == ww && k.h == wh)) { EfbSlotDrop(k); dropped = true; }
        if (dropped) autoq::Log("[autoq] kept render targets of other sizes let go for the step up");
    }
    if (want > g_rung && !RungFits(want, needMiB, freeMiB)) {
        int best = want - 1;
        double bestNeed = 0, bestFree = 0;
        while (best > g_rung && !RungFits(best, bestNeed, bestFree)) --best;
        char wantText[32];
        RungText(want, wantText, sizeof wantText);
        autoq::Log("[autoq] rung %d (%s) would need about %.1f MiB of video memory, %.1f MiB free: rung %d instead", want, wantText, needMiB, freeMiB, best);
        g_autoq->Limit(best, autoq::clock::NowUs());
        want = g_autoq->Current();
    }
    if (want != autoq::kNoRung && want != g_rung) {
        const autoq::Change& ch = g_autoq->LastChange();
        char why[200];
        if (ch.why == autoq::Why::NewScene) std::snprintf(why, sizeof why, "%s", autoq::WhyText(ch.why));
        else if (quiet && ch.why == autoq::Why::Headroom)
            std::snprintf(why, sizeof why, "%s, taken in a quiet moment (GPU load %.0f %%, back-end CPU %.0f %%, %.1f ms a frame in play)", autoq::WhyText(ch.why),
                          ch.load * 100.0, ch.cpu * 100.0, ch.intervalMs);
        else
            std::snprintf(why, sizeof why, "%s (GPU load %.0f %%, back-end CPU %.0f %%, %.1f ms a frame)", autoq::WhyText(ch.why), ch.load * 100.0,
                          ch.cpu * 100.0, ch.intervalMs);
        const int from = g_rung;
        if (!ApplyRung(want, why)) g_autoq->Refused(from, want, autoq::clock::NowUs()); // (no memory: back, that rung blocked)
    }
}
} // namespace

// The host's present has swapped (vgx_cmd.h): the GPU clock takes the frame.
void AutoQualitySwapped() {
    if (g_autoq) autoq::clock::FramePresented();
}

void ForceQualityRung(int rung) { g_forceRung = std::clamp(rung, autoq::Target30Enabled() ? autoq::kEmergency : 0, TopRung()); }
void DebugFailAllocation(int nth) { g_simFailAt = nth; g_simChecks = 0; }
void DebugLastChange(unsigned& resampled, unsigned& plain) { resampled = g_lastResampled; plain = g_lastPlain; }
int QualityRung() { return g_rung; }
void QualityRungSize(int& w, int& h, int& samples) { w = g_rtW; h = g_rtH; samples = g_efbSamples; }

int QualityTopRung() { return TopRung(); }
unsigned DebugEfbSwaps() { return g_efbSwapsTotal; }
int EfbSamples() { return g_efbSamples; }

// (tests) A triangle of one colour over the lower left half of the EFB (corners bottom-left, bottom-right, top-left of the
// target): an edge across every row, for the multisampling checks.
void DebugDrawEfbTriangle(const float rgba[4]) {
    static GLuint prog = 0, vbo = 0;
    static GLint col = -1;
    if (!prog) {
        const char* vs = "attribute vec2 a_pos;\nvoid main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
        const char* fs = "precision mediump float;\nuniform vec4 u_col;\nvoid main() { gl_FragColor = u_col; }\n";
        const GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(v, 1, &vs, nullptr);
        glCompileShader(v);
        glShaderSource(f, 1, &fs, nullptr);
        glCompileShader(f);
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glBindAttribLocation(prog, 0, "a_pos");
        glLinkProgram(prog);
        col = glGetUniformLocation(prog, "u_col");
        static const float tri[6] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f};
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    }
    BindFbo(g_efbFbo);
    TouchTarget();
    G(glViewport(0, 0, g_rtW, g_rtH));
    G(glDisable(GL_SCISSOR_TEST));
    G(glDisable(GL_BLEND));
    G(glDisable(GL_DEPTH_TEST));
    G(glDisable(GL_CULL_FACE));
    G(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    G(glUseProgram(prog));
    G(glUniform4f(col, rgba[0], rgba[1], rgba[2], rgba[3]));
    G(glBindBuffer(GL_ARRAY_BUFFER, vbo));
    G(glEnableVertexAttribArray(0));
    G(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    G(glDrawArrays(GL_TRIANGLES, 0, 3));
    // Back as the back end left it: attribute 0 pointing into the quad buffer, its enable state, nothing cached trusted.
    G(glBindBuffer(GL_ARRAY_BUFFER, g_quadVbo));
    G(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    if (!(S.attrEnabled & 1u)) G(glDisableVertexAttribArray(0));
    const GLuint fbo = S.fbo;
    Invalidate();
    g_rsValid = false;
    S.fbo = fbo; // (still bound)
}

bool DebugRungFits(int rung, double& needMiB, double& freeMiB, int& current, double& projectedMiB) {
    current = g_rung;
    projectedMiB = ProjectedBytes(rung) / 1048576.0;
    return RungFits(rung, needMiB, freeMiB);
}

void DebugPoolStats(unsigned& pooled, uint64_t& renderTargetBytes, uint64_t& evicted) {
    pooled = static_cast<unsigned>(g_pool.size());
    renderTargetBytes = RenderTargetBytes();
    evicted = g_poolEvicted;
}

bool DebugReadTexture(uint32_t texId, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (texId >= g_tex.size() || !g_tex[texId].id) return false;
    const Tex& t = g_tex[texId];
    width = t.w;
    height = t.h;
    AcquireFbo(t.w, t.h, t.id, false);
    rgba.resize(static_cast<size_t>(t.w) * t.h * 4u);
    G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
    G(glReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()));
    BindFbo(g_efbFbo);
    g_rsValid = false;
    return true;
}

// ================================================================ present sharpening (after the Strikers port's PIC_SMOOTH)
// The display copy drawn to the screen with plain bilinear filtering plus a light unsharp mask (five fetches): the
// picture is drawn smaller than the screen (640x456 to 960x544 today; the automatic quality's rungs below 960x544), and
// bilinear upscaling alone softens it. The vertex program is the copy program's position-only quad; u_rect is the uv
// rectangle stretched over the viewport (v at the quad's bottom edge, dv < 0 flips), u_size the EFB texture's
// (w, h, 1/w, 1/h), u_lim the first and last texel centre of the picture (no fetch outside the display copy's
// rectangle), u_p.z the unsharp amount.
namespace {
struct SharpProg {
    GLuint prog = 0;
    bool failed = false; // compile or link failed: the host blits as before, for good
    GLint rect = -1, size = -1, lim = -1, p = -1;
} g_sharp;

bool EnsureSharpenProgram() {
    if (g_sharp.prog) return true;
    if (g_sharp.failed || GlAllocBackoff()) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w); gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    static const char* fs =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\nvoid main() {\n"
        "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n"
        "  vec4 c = texture2D(u_src, uv);\n"
        "  vec4 b = texture2D(u_src, clamp(uv + vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)) + texture2D(u_src, clamp(uv - vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw))\n"
        "         + texture2D(u_src, clamp(uv + vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)) + texture2D(u_src, clamp(uv - vec2(0.0, u_size.w), u_lim.xy, u_lim.zw));\n"
        "  c = c + (c - b * 0.25) * u_p.z;\n"
        "  gl_FragColor = vec4(c.rgb, 1.0);\n}\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "sharpen vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "sharpen fragment");
    GLint vok = 0, fok = 0, lok = 0;
    if (v) G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    if (f) G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
    GLuint prog = 0;
    if (vok && fok) {
        prog = G(glCreateProgram());
        if (BadGlName(prog)) prog = 0;
    }
    if (prog) {
        G(glAttachShader(prog, v));
        G(glAttachShader(prog, f));
        G(glBindAttribLocation(prog, AT_POS, "a_pos"));
        G(glLinkProgram(prog));
        G(glGetProgramiv(prog, GL_LINK_STATUS, &lok));
    }
    if (v) G(glDeleteShader(v));
    if (f) G(glDeleteShader(f));
    if (!lok) {
        if (prog) G(glDeleteProgram(prog));
        g_sharp.failed = true;
        autoq::Log("[autoq] the sharpening shader did not build: the display copy is blitted as before");
        return false;
    }
    g_sharp.prog = prog;
    UseProgram(prog);
    g_sharp.rect = G(glGetUniformLocation(prog, "u_rect"));
    g_sharp.size = G(glGetUniformLocation(prog, "u_size"));
    g_sharp.lim = G(glGetUniformLocation(prog, "u_lim"));
    g_sharp.p = G(glGetUniformLocation(prog, "u_p"));
    G(glUniform1i(glGetUniformLocation(prog, "u_src"), 0));
    return true;
}
} // namespace

bool PresentSharpened(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY) {
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return true;
    if (!EnsureSharpenProgram()) return false; // (nothing of the GL state touched)
    BindFbo(fbo);
    SetViewport(dx, dy, dw, dh);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    UseProgram(g_sharp.prog);
    BindTex(0, g_efbColor);
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    const float u0 = static_cast<float>(x) / tw, du = static_cast<float>(w) / tw;
    const float v0 = static_cast<float>(y) / th, dv = static_cast<float>(h) / th;
    if (flipY) G(glUniform4f(g_sharp.rect, u0, v0 + dv, du, -dv)); // (the quad's bottom edge shows the picture's last row)
    else G(glUniform4f(g_sharp.rect, u0, v0, du, dv));
    G(glUniform4f(g_sharp.size, tw, th, 1.0f / tw, 1.0f / th));
    G(glUniform4f(g_sharp.lim, (static_cast<float>(x) + 0.5f) / tw, (static_cast<float>(y) + 0.5f) / th, (static_cast<float>(x + w) - 0.5f) / tw,
                  (static_cast<float>(y + h) - 0.5f) / th));
    G(glUniform4f(g_sharp.p, static_cast<float>(dw) / static_cast<float>(w), static_cast<float>(dh) / static_cast<float>(h), amount, 0.0f));
    C.uniformCalls += 4;
    C.uniformBytes += 64;
    g_uniDirtyV = g_uniDirtyF = false;
    if (S.layoutVbo != g_quadVbo) { // (DrawQuad's layout: the quad's own position-only buffer)
        ++C.layoutChanges;
        BindArrayBuf(g_quadVbo);
        for (int a = 1; a < AT_COUNT; ++a)
            if (S.attrEnabled & (1u << a)) {
                G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
                S.attrEnabled &= ~(1u << a);
                S.attrConstSet &= ~(1u << a);
                ++C.attrToggles;
            }
        if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; ++C.attrToggles; }
        G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        ++C.attrPointers;
        S.layoutVbo = g_quadVbo;
        S.layoutMask = 0;
        S.layoutBase = 0;
    }
    TouchTarget();
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    return true;
}

// GXV_SHARPEN=auto|off|<amount> (default auto): the unsharp amount of PresentSharpened for a display copy of w x h drawn
// at dw x dh. auto follows the upscale: the larger factor of the two sides, m; amount = (m - 1), at most 0.8: 0.5 at
// today's 640 -> 960 (the Strikers port's Vita default, at the same factor), 0 at the screen's own size (960x544: then
// the plain blit, no extra GPU work), 0.76 at 544x388. A fixed amount applies whatever the size; off = the blit always.
float SharpenAmount(int w, int h, int dw, int dh) {
    static const float fixed = [] {
        const char* e = std::getenv("GXV_SHARPEN");
        if (!e || !*e || std::strcmp(e, "auto") == 0) return -1.0f;
        if (std::strcmp(e, "off") == 0) return 0.0f;
        return std::clamp(static_cast<float>(std::atof(e)), 0.0f, 1.5f);
    }();
    if (fixed >= 0.0f) return fixed;
    if (w <= 0 || h <= 0) return 0.0f;
    const float m = std::max(static_cast<float>(dw) / static_cast<float>(w), static_cast<float>(dh) / static_cast<float>(h));
    return std::clamp(m - 1.0f, 0.0f, 0.8f);
}

// ================================================================ edge smoothing of the characters and key items (GXV_CHARAA)
// The front end's rectangles (vgx_draw.cpp, "character edge smoothing"; CopyDispCmd::chara) are drawn over the present
// that PresentSharpened (or the host's blit) has just made, into the same framebuffer, so into the display's own scene:
// one quad per rectangle, its viewport the rectangle's pixels on the screen. The quad's fragment program reads the EFB
// texture exactly as the sharpened present did (same texel rectangle, same five fetches, same unsharp sum) and, where the
// luma of the 5-texel cross shows no edge, writes that same value: the flat
// inside of a rectangle comes out as it was (contrast below max(1/12, 1/6 of the brightest), FXAA's quality thresholds). On an edge it writes FXAA's result instead (T. Lottes' FXAA 2 / "console"
// form: four diagonal fetches give the edge's direction, two or four fetches along it blend across the staircase).
// Cost: the rectangles' screen pixels a second time (5 fetches, 13 on an edge pixel), nothing else: no render target,
// no scene, no copy. Skipped when the EFB is multisampled (quality rungs 7 and 8: the edges are smooth already).
namespace {
struct CharaProg {
    GLuint prog = 0;
    bool failed = false;
    GLint rect = -1, size = -1, lim = -1, p = -1;
} g_charaProgs[2];
struct CharaStats { uint64_t presents = 0, boxes = 0, pixels = 0, msaaSkips = 0; } g_charaStats;

// Kernel 0 (default, GXV_CHARAA_KERNEL=fxaa): FXAA 2 as above. Kernel 1 (GXV_CHARAA_KERNEL=sub): the Strikers port's 1.1 SMOOTH
// filter (a4b7466, MSC_VITA_EDGE_AA): from the same four neighbours, the pixel is taken a fraction of a texel across the edge
// (FXAA's sub-pixel step without the direction blur), one fetch more, the unsharp mask mostly left out there. Same no-edge test
// shape (its own thresholds), so flat pixels are the sharpened present's either way.
bool EnsureCharaProgram(int kernel) {
    CharaProg& g_charaProg = g_charaProgs[kernel & 1];
    if (g_charaProg.prog) return true;
    if (g_charaProg.failed || GlAllocBackoff()) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w); gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    static const char* fsFxaa =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\n"
        "vec3 F(vec2 uv) { return texture2D(u_src, clamp(uv, u_lim.xy, u_lim.zw)).rgb; }\n"
        "float L(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "void main() {\n"
        "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n"
        "  vec4 c = texture2D(u_src, uv);\n"
        "  vec4 e = texture2D(u_src, clamp(uv + vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)), w = texture2D(u_src, clamp(uv - vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw));\n"
        "  vec4 s = texture2D(u_src, clamp(uv + vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)), n = texture2D(u_src, clamp(uv - vec2(0.0, u_size.w), u_lim.xy, u_lim.zw));\n"
        "  vec4 b = e + w + s + n;\n"
        "  vec3 o = (c + (c - b * 0.25) * u_p.z).rgb;\n" // (PresentSharpened's value)
        "  float lM = L(c.rgb), lE = L(e.rgb), lW = L(w.rgb), lS = L(s.rgb), lN = L(n.rgb);\n"
        "  float mn = min(lM, min(min(lE, lW), min(lS, lN))), mx = max(lM, max(max(lE, lW), max(lS, lN)));\n"
        "  if (mx - mn >= max(0.0833, mx * 0.166)) {\n" // (FXAA's "quality" edge thresholds: texture detail inside a character is left alone)
        "    float lNW = L(F(uv - u_size.zw)), lSE = L(F(uv + u_size.zw));\n"
        "    float lNE = L(F(uv + vec2(u_size.z, -u_size.w))), lSW = L(F(uv + vec2(-u_size.z, u_size.w)));\n"
        "    vec2 dir = vec2((lSW + lSE) - (lNW + lNE), (lNW + lSW) - (lNE + lSE));\n"
        "    float red = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);\n"
        "    dir = clamp(dir * (1.0 / (min(abs(dir.x), abs(dir.y)) + red)), vec2(-8.0), vec2(8.0)) * u_size.zw;\n"
        "    vec3 a = 0.5 * (F(uv - dir * 0.16666667) + F(uv + dir * 0.16666667));\n"
        "    vec3 q = a * 0.5 + 0.25 * (F(uv - dir * 0.5) + F(uv + dir * 0.5));\n"
        "    float lq = L(q);\n"
        "    float lo = min(mn, min(min(lNW, lNE), min(lSW, lSE))), hi = max(mx, max(max(lNW, lNE), max(lSW, lSE)));\n"
        "    o = (lq < lo || lq > hi) ? a : q;\n"
        "    if (u_p.w > 0.5) o = mix(o, vec3(1.0, 0.0, 0.0), 0.5);\n" // (GXV_CHARAA=2: the pixels it changed)
        "  }\n"
        "  gl_FragColor = vec4(o, 1.0);\n}\n";
    static const char* fsSub =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\n"
        "float L(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
        "void main() {\n"
        "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n"
        "  vec4 c = texture2D(u_src, uv);\n"
        "  vec4 e = texture2D(u_src, clamp(uv + vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)), w = texture2D(u_src, clamp(uv - vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw));\n"
        "  vec4 s = texture2D(u_src, clamp(uv + vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)), n = texture2D(u_src, clamp(uv - vec2(0.0, u_size.w), u_lim.xy, u_lim.zw));\n"
        "  vec4 b = e + w + s + n;\n"
        "  vec3 o = (c + (c - b * 0.25) * u_p.z).rgb;\n"
        "  float lC = L(c.rgb), lE = L(e.rgb), lW = L(w.rgb), lS = L(s.rgb), lN = L(n.rgb);\n"
        "  float lMax = max(max(max(lE, lW), max(lS, lN)), lC), lMin = min(min(min(lE, lW), min(lS, lN)), lC);\n"
        "  float range = lMax - lMin;\n"
        "  if (range >= max(0.06, 0.15 * lMax)) {\n"
        "    float sub = clamp(abs((lE + lW + lS + lN) * 0.25 - lC) / max(range, 0.001), 0.0, 1.0);\n"
        "    float sm = sub * sub * (3.0 - 2.0 * sub);\n"
        "    float blend = sm * sm * 0.75;\n"
        "    float horz = step(abs(lE + lW - 2.0 * lC), abs(lN + lS - 2.0 * lC));\n"
        "    float sy = step(abs(lN - lC), abs(lS - lC)) * 2.0 - 1.0, sx = step(abs(lW - lC), abs(lE - lC)) * 2.0 - 1.0;\n"
        "    vec2 nrm = vec2((1.0 - horz) * sx * u_size.z, horz * sy * u_size.w);\n"
        "    vec4 a = texture2D(u_src, clamp(uv + nrm * blend, u_lim.xy, u_lim.zw));\n"
        "    o = a.rgb + (c.rgb - b.rgb * 0.25) * (u_p.z * 0.3);\n"
        "    if (u_p.w > 0.5) o = mix(o, vec3(1.0, 0.0, 0.0), 0.5);\n"
        "  }\n"
        "  gl_FragColor = vec4(o, 1.0);\n}\n";
    const char* fs = (kernel & 1) ? fsSub : fsFxaa;
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "chara-aa vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "chara-aa fragment");
    GLint vok = 0, fok = 0, lok = 0;
    if (v) G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    if (f) G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
    GLuint prog = 0;
    if (vok && fok) {
        prog = G(glCreateProgram());
        if (BadGlName(prog)) prog = 0;
    }
    if (prog) {
        G(glAttachShader(prog, v));
        G(glAttachShader(prog, f));
        G(glBindAttribLocation(prog, AT_POS, "a_pos"));
        G(glLinkProgram(prog));
        G(glGetProgramiv(prog, GL_LINK_STATUS, &lok));
    }
    if (v) G(glDeleteShader(v));
    if (f) G(glDeleteShader(f));
    if (!lok) {
        if (prog) G(glDeleteProgram(prog));
        g_charaProg.failed = true;
        autoq::Log("[charaa] the edge-smoothing shader did not build: characters are not smoothed (GXV_CHARAA)");
        return false;
    }
    g_charaProg.prog = prog;
    UseProgram(prog);
    g_charaProg.rect = G(glGetUniformLocation(prog, "u_rect"));
    g_charaProg.size = G(glGetUniformLocation(prog, "u_size"));
    g_charaProg.lim = G(glGetUniformLocation(prog, "u_lim"));
    g_charaProg.p = G(glGetUniformLocation(prog, "u_p"));
    G(glUniform1i(glGetUniformLocation(prog, "u_src"), 0));
    return true;
}
} // namespace

int PresentCharaAA(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY) {
    const CopyDispCmd* d = g_charaDisp;
    if (!d || (d->charaN == 0 && !g_charaFull) || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return 0;
    if (g_efbSamples > 1) { ++g_charaStats.msaaSkips; return 0; } // (rungs 7-8: the whole picture is multisampled)
    // GXV_CHARAA_KERNEL=fxaa|sub (default fxaa): the edge filter (EnsureCharaProgram)
    static const int kernelEnv = [] { const char* e = std::getenv("GXV_CHARAA_KERNEL"); return e && std::strcmp(e, "sub") == 0 ? 1 : 0; }();
    const int kernel = g_charaKernel >= 0 ? g_charaKernel : kernelEnv;
    if (!EnsureCharaProgram(kernel)) return 0;
    const CharaProg& g_charaProg = g_charaProgs[kernel & 1];
    static const bool tintEnv = EnvInt("GXV_CHARAA", 1) == 2;
    const bool tint = g_charaTint >= 0 ? g_charaTint > 0 : tintEnv;
    // The display's scene is the one the present (PresentSharpened, or the host's blit) has begun on `fbo`: whatever the
    // blit did to the GL state, every cached value is set again here; the framebuffer is the same, so no scene begins.
    Invalidate();
    G(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    S.fbo = fbo;
    g_inUseFbo = fbo;
    g_fbDirty = false;
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    UseProgram(g_charaProg.prog);
    BindTex(0, g_efbColor);
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    const float u0 = static_cast<float>(x) / tw, du = static_cast<float>(w) / tw;
    const float v0 = static_cast<float>(y) / th, dv = static_cast<float>(h) / th;
    G(glUniform4f(g_charaProg.size, tw, th, 1.0f / tw, 1.0f / th));
    G(glUniform4f(g_charaProg.lim, (static_cast<float>(x) + 0.5f) / tw, (static_cast<float>(y) + 0.5f) / th, (static_cast<float>(x + w) - 0.5f) / tw,
                  (static_cast<float>(y + h) - 0.5f) / th));
    G(glUniform4f(g_charaProg.p, 0.0f, 0.0f, amount, tint ? 1.0f : 0.0f));
    C.uniformCalls += 3;
    C.uniformBytes += 48;
    g_uniDirtyV = g_uniDirtyF = false;
    ++C.layoutChanges;
    BindArrayBuf(g_quadVbo);
    for (int a = 1; a < AT_COUNT; ++a)
        if (S.attrEnabled & (1u << a)) {
            G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
            S.attrEnabled &= ~(1u << a);
            S.attrConstSet &= ~(1u << a);
            ++C.attrToggles;
        }
    if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; ++C.attrToggles; }
    G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    ++C.attrPointers;
    S.layoutVbo = g_quadVbo;
    S.layoutMask = 0;
    S.layoutBase = 0;
    const float sx = ScaleX(), sy = ScaleY();
    const float kx = static_cast<float>(dw) / static_cast<float>(w), ky = static_cast<float>(dh) / static_cast<float>(h);
    int drawn = 0;
    const EfbRect whole{static_cast<int16_t>(Rnd(x / sx)), static_cast<int16_t>(Rnd(y / sy)), static_cast<int16_t>(Rnd(w / sx)), static_cast<int16_t>(Rnd(h / sy))};
    const int nBoxes = g_charaFull ? 1 : d->charaN;
    for (int i = 0; i < nBoxes && i < kCharaBoxes; ++i) {
        const EfbRect& b = g_charaFull ? whole : d->chara[i];
        // EFB pixels -> render-target pixels -> the screen's pixels (outwards to whole pixels, inside the present's rectangle)
        const float rx0 = b.x * sx - static_cast<float>(x), rx1 = (b.x + b.w) * sx - static_cast<float>(x);
        const float ry0 = b.y * sy - static_cast<float>(y), ry1 = (b.y + b.h) * sy - static_cast<float>(y);
        int X0 = dx + static_cast<int>(std::floor(rx0 * kx)), X1 = dx + static_cast<int>(std::ceil(rx1 * kx));
        int Y0, Y1;
        if (flipY) { Y0 = dy + dh - static_cast<int>(std::ceil(ry1 * ky)); Y1 = dy + dh - static_cast<int>(std::floor(ry0 * ky)); }
        else { Y0 = dy + static_cast<int>(std::floor(ry0 * ky)); Y1 = dy + static_cast<int>(std::ceil(ry1 * ky)); }
        X0 = std::max(X0, dx); Y0 = std::max(Y0, dy);
        X1 = std::min(X1, dx + dw); Y1 = std::min(Y1, dy + dh);
        if (X1 <= X0 || Y1 <= Y0) continue;
        // The uv rectangle of this viewport, so that each pixel centre samples where the full-screen present sampled it.
        const float ul = u0 + du * static_cast<float>(X0 - dx) / static_cast<float>(dw), uw = du * static_cast<float>(X1 - X0) / static_cast<float>(dw);
        const float fb = static_cast<float>(Y0 - dy) / static_cast<float>(dh), ft = static_cast<float>(Y1 - dy) / static_cast<float>(dh);
        float vb, vt;
        if (flipY) { vb = v0 + dv * (1.0f - fb); vt = v0 + dv * (1.0f - ft); }
        else { vb = v0 + dv * fb; vt = v0 + dv * ft; }
        SetViewport(X0, Y0, X1 - X0, Y1 - Y0);
        G(glUniform4f(g_charaProg.rect, ul, vb, uw, vt - vb));
        ++C.uniformCalls;
        TouchTarget();
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        ++drawn;
        g_charaStats.pixels += static_cast<uint64_t>(X1 - X0) * static_cast<uint64_t>(Y1 - Y0);
    }
    if (drawn) { ++g_charaStats.presents; g_charaStats.boxes += static_cast<uint64_t>(drawn); }
    return drawn;
}
unsigned DebugLastFrameScenes() { return g_lastScenes; }
void CharaAACounters(uint64_t& presents, uint64_t& boxes, uint64_t& pixels, uint64_t& msaaSkips) {
    presents = g_charaStats.presents;
    boxes = g_charaStats.boxes;
    pixels = g_charaStats.pixels;
    msaaSkips = g_charaStats.msaaSkips;
}

bool Init(int renderWidth, int renderHeight) {
#if VGX_COLDTRACE && defined(__vita__)
    static bool captureAttempted=false;
    if(!captureAttempted && EnvInt("GXV_COLDTRACE",0)!=0) {
        captureAttempted=true;
        const auto clock=[]() noexcept -> uint64_t {return sceKernelGetProcessTimeWide();};
        const auto thread=[]() noexcept -> uint32_t {return static_cast<uint32_t>(sceKernelGetThreadId());};
        if(coldtrace::g_recorder.Arm(clock,thread,thread()))port_vgx_coldtrace_activate();
    }
#endif
    ResetCrowd();
    const validate::Window validationWindow = validate::ParseWindow(DiagnosticEnv("GXV_VALIDATE"));
    if (validationWindow.invalid) std::fprintf(stderr, "[validate] invalid GXV_VALIDATE: use <frame> or <first>:<last>, at most four frames; disabled\n");
    if (validationWindow.enabled) {
        g_validation = std::make_unique<validate::Session>(validationWindow);
        std::fprintf(stderr, "[validate] enabled frames=%llu:%llu static_shadow_limit=16777216 detail_limit=16+8 CPU-contract-only; diagnostic timings are not performance evidence\n",
                     static_cast<unsigned long long>(validationWindow.first), static_cast<unsigned long long>(validationWindow.last));
    }
    g_fixedW = renderWidth;
    g_fixedH = renderHeight;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    g_hasDepth24 = ext && std::strstr(ext, "GL_OES_depth24");
    g_hasDepthTex = ext && std::strstr(ext, "GL_OES_depth_texture") && EnvInt("GXV_DEPTH_TEX", 1);
#ifdef __VGX_VITA__
    g_hasS3tc = true; // (vitaGL takes DXT1 whatever its extension string says)
    g_hasDxt5 = true; // (and DXT5: SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR, textures.c)
#else
    g_hasS3tc = ext && (std::strstr(ext, "GL_EXT_texture_compression_s3tc") || std::strstr(ext, "GL_EXT_texture_compression_dxt1")) &&
                EnvInt("GXV_S3TC", 1);
    g_hasDxt5 = g_hasS3tc && std::strstr(ext, "GL_EXT_texture_compression_s3tc");
    g_hasMaxLevel = (ver && std::strstr(ver, "OpenGL ES 3")) || (ext && std::strstr(ext, "GL_APPLE_texture_max_level"));
#endif
    g_quadClear = EnvInt("GXV_QUADCLEAR", 1) != 0;
    g_noCache = EnvInt("GXV_GLCACHE", 1) == 0;
    g_fboMaxUses = static_cast<uint32_t>(EnvInt("GXV_FBO_USES", 8));
    if (const char* e = std::getenv("GXV_STATIC_USAGE"); e && std::strcmp(e, "dynamic") == 0) g_staticUsage = GL_DYNAMIC_DRAW;
    // GXV_EFB0=WxH: the size the EFB object is first made with (default 640x528). The game
    // sets its real size in its first frame; when that differs, the first object's colour and
    // depth memory are given back at once. (Diagnostic for Vita3K, whose surface cache goes on
    // treating the freed addresses as render targets: v3k/NOTES.md, session 3.)
    if (const char* e = std::getenv("GXV_EFB0")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 1024 && h <= 1024) { g_efbW = w; g_efbH = h; }
    }
    if (g_fboMaxUses < 1) g_fboMaxUses = 1;
    G(glGenBuffers(1, &g_ibo));
    G(glGenBuffers(1, &g_quadVbo));
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    BindArrayBuf(g_quadVbo);
    G(glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW));
    // Every attribute location gets a pointer into a buffer once (see the notes at the top).
    for (int a = 0; a < AT_COUNT; ++a) G(glVertexAttribPointer(static_cast<GLuint>(a), 2, GL_FLOAT, GL_FALSE, 0, nullptr));
    G(glGenTextures(1, &g_whiteTex));
    BindTexForEdit(g_whiteTex);
    const uint8_t white[4] = {255, 255, 255, 255};
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    g_tex.resize(256);
    AutoQualityInit();
    CreateTarget();
    if (!EnsureCopyProgram()) return false; // compiled here, not at the first copy that needs it
#if !defined(__VGX_VITA__)
    MakeProgressDumpDisplay();
#endif
    G(glDisable(GL_DITHER));
    G(glFrontFace(GL_CCW)); // GX front faces are clockwise on screen; with the flipped Y that is CCW here
    VGX_LOG("GL back end: %s / %s; DXT1 %s, clears with a partial colour mask %s\n", glGetString(GL_RENDERER), ver ? ver : "?",
            g_hasS3tc ? "native" : "decoded here", g_quadClear ? "drawn" : "by glClear");
    return true;
}

namespace {
// GXV_PROF=1: wall time per command type on the render thread, printed every 100 presents.
double g_prof[CMD_TYPE_COUNT] = {};
uint32_t g_profCount[CMD_TYPE_COUNT] = {};
uint32_t g_profFrames = 0;
double g_profTotal = 0, g_profStream = 0;
uint64_t g_profSum[64] = {}; // the call census (VGX_GL_COUNTERS) summed over the same frames
static_assert(sizeof(Counts) / sizeof(uint32_t) <= 64, "g_profSum too small");
inline double Now() { return NowMs(); }
// Frame selection of the GPU timing diagnostics (GXV_PASSTIME, GXV_DRAWTIME): "7000,7010-7013".
bool FrameInList(const char* list, uint64_t frame) {
    for (const char* q = list; q && *q;) {
        char* end = nullptr;
        const unsigned long long a = std::strtoull(q, &end, 10);
        if (end == q) break;
        unsigned long long b = a;
        if (*end == '-') {
            char* end2 = nullptr;
            const unsigned long long v = std::strtoull(end + 1, &end2, 10);
            if (end2 != end + 1) { b = v; end = end2; }
        }
        if (frame >= a && frame <= b) return true;
        q = *end == ',' ? end + 1 : end;
    }
    return false;
}
// "<seconds>[,<n>]": n consecutive frames (4 by default) once <seconds> have passed since the first call.
struct TimedPick {
    const char* spec;
    bool parsed = false;
    double afterMs = 0, firstMs = -1;
    int left = 0;
    bool Take() {
        if (!spec || !*spec) return false;
        if (!parsed) {
            parsed = true;
            char* e = nullptr;
            afterMs = std::strtod(spec, &e) * 1000.0;
            left = (e && *e == ',') ? std::atoi(e + 1) : 4;
            if (left < 1) left = 1;
        }
        const double now = Now();
        if (firstMs < 0) firstMs = now;
        if (left > 0 && now - firstMs >= afterMs) { --left; return true; }
        return false;
    }
};
}

// BEGIN GPUPASS BACKEND
namespace {
// (GXV_PASSVERIFY's state: see there)
struct PassVerify {
    bool refRun = false;  // the section's changes are off (reference runs)
    bool shadow = false;  // the run is not the one shown: no present hook, no capture, no counts
    bool read = false;    // read the EFB back at the display copy
    std::vector<uint8_t> pix;
    int w = 0, h = 0;
    bool got = false;
    Counts counts; // the run's call census (taken before it is reset)
    uint32_t restorePlanes = 0; // GXV_EFBSWAP: planes drawn back (1 colour, 2 alpha)
} g_verify;
inline bool GpupassOff() { return g_verify.refRun; }
// (GXV_PASSVERIFY's search for the change that makes a frame differ: one feature switched off at a time)
enum : uint32_t { GP_DEPTHPLAN = 1, GP_EFBSWAP = 2, GP_PASSDIRECT2 = 4, GP_PASSHOIST = 8, GP_PASSHALF = 16, GP_FXHALF = 32 };
uint32_t g_gpOffMask = 0, g_gpOnlyMask = 0;
inline bool GpupassOff(uint32_t feature) {
    return g_verify.refRun || (g_gpOffMask & feature) != 0 || (g_gpOnlyMask != 0 && (g_gpOnlyMask & feature) == 0);
}
// The target BeginPass makes for a pass (kept in step with it): false when the pass rectangle is empty at this render
// size (its draws then go nowhere); directTex: the texture the pass is drawn straight into, 0 for the pool's own buffer.
bool PassTargetOf(const PassCmd& p, uint32_t& directTex, float f) {
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW) * f, sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH) * f;
    const int lw = f == 1.0f ? g_rtW : Rnd(g_efbW * sx), lh = f == 1.0f ? g_rtH : Rnd(g_efbH * sy);
    int x0 = Rnd(p.rect.x * sx), y0 = Rnd(p.rect.y * sy), x1 = Rnd((p.rect.x + p.rect.w) * sx), y1 = Rnd((p.rect.y + p.rect.h) * sy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > lw) x1 = lw;
    if (y1 > lh) y1 = lh;
    const int w = x1 - x0, h = y1 - y0;
    directTex = 0;
    if (w <= 0 || h <= 0) return false;
    if (p.atlasTex) {
        directTex = p.directTex;
    } else if (p.directTex && EnvInt("GXV_PASSDIRECT", 1) != 0) {
        int dw = Rnd(p.rect.w * sx), dh = Rnd(p.rect.h * sy);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        if (dw == w && dh == h) directTex = p.directTex;
    }
    return true;
}
// Does an EFB clear of rectangle r (EFB pixels) cover the whole render target? (Clear's rounding)
bool ClearCoversTarget(const EfbRect& r) {
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    const int x0 = Rnd(r.x * sx), y0 = Rnd(r.y * sy), x1 = Rnd((r.x + r.w) * sx), y1 = Rnd((r.y + r.h) * sy);
    return x0 <= 0 && y0 <= 0 && x1 >= g_rtW && y1 >= g_rtH;
}

bool LateDirect(const CmdBuf& buf, size_t begin, PassCmd& ps, float f);
float PassScaleFor(const CmdBuf& buf, size_t begin, const PassCmd& ps);
inline bool GpupassOff();
inline bool Hoisted(size_t i);
inline bool FxCmd(size_t i);
inline bool FxNeutral(uint8_t t);
// GXV_DEPTHPLAN (see dplan near the top): the frame's depth events and what follows from them. Mirrors what Execute and
// its helpers do with the targets: a frame where that cannot be known beforehand gets no plan.
void PlanFrame(const CmdBuf& buf) {
    using namespace dplan;
    g_valid = false;
    g_afterHook = false;
    g_inClear = false;
    g_cmd = 0;
    if (!(Enabled() || EfbSwapEnabled()) || GpupassOff() || buf.crowdActive) return;
    const size_t n = buf.cmds.size(), positions = 2 * n;
    for (const Cmd& c : buf.cmds)
        if (c.type == CMD_SHADER || c.type == CMD_PROGRAM || c.type == CMD_VARIANT || c.type == CMD_SET_TARGET) return;
    g_ev.assign(positions + 1, 0);
    bool inPass = false;
    uint32_t passDirect = 0;
    auto clearEvent = [&](size_t p, const ClearSpec& cs, const EfbRect& r) {
        if (cs.enable && cs.depth) g_ev[p] |= ClearCoversTarget(r) ? EV_FULL : EV_WRITE;
        if (cs.enable && ClearCoversTarget(r)) g_ev[p] |= (cs.color ? EV_FC : 0) | (cs.alpha ? EV_FA : 0);
    };
    bool fxRun = false; // (GXV_FXHALF: inside a run)
    for (size_t i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[i];
        const size_t p0 = 2 * i, p1 = p0 + 1;
        if (fxRun && !(c.type == CMD_DRAW && FxCmd(i)) && !FxNeutral(c.type)) fxRun = false;
        switch (c.type) {
        case CMD_DRAW: {
            if (inPass) break;
            if (FxCmd(i)) {
                // (GXV_FXHALF: drawn into the reduced-size target; the first of a run leaves the EFB and reads its depth, the
                // composite after the run reads its colour - counted here, earlier: conservative)
                if (!fxRun) g_ev[p0] |= EV_LEAVE | EV_READ;
                fxRun = true;
                g_ev[p0] |= EV_RC | EV_RA;
                break;
            }
            const DrawRec& d = buf.draws[c.arg];
            const RenderState& rs = d.rs;
            if (rs.depthTest && (rs.depthFunc & 7) != CMP_ALWAYS) g_ev[p0] |= EV_READ;
            if (rs.depthTest && rs.depthWrite) g_ev[p0] |= EV_WRITE;
            if (rs.blend) {
                const bool dstAlpha = rs.blendSrc == BF_DST_ALPHA || rs.blendSrc == BF_INV_DST_ALPHA || rs.blendDst == BF_DST_ALPHA || rs.blendDst == BF_INV_DST_ALPHA;
                if (rs.colorWrite) g_ev[p0] |= EV_RC;
                if (rs.alphaWrite || rs.dstAlphaPass || dstAlpha) g_ev[p0] |= EV_RA;
            }
            // (DrawRec::cover: its first primitive writes every pixel of its box, unblended)
            if (d.cover && d.exX <= 0 && d.exY <= 0 && d.exX + d.exW >= g_efbW && d.exY + d.exH >= g_efbH) {
                if (rs.colorWrite) g_ev[p0] |= EV_FC;
                if (rs.alphaWrite || rs.dstAlphaPass) g_ev[p0] |= EV_FA;
            }
            break;
        }
        case CMD_PASS_BEGIN: {
            PassCmd ps = buf.passes[c.arg];
            const float f = PassScaleFor(buf, i, ps);
            LateDirect(buf, i, ps, f);
            inPass = PassTargetOf(ps, passDirect, f);
            if (inPass) g_ev[p0] |= EV_LEAVE;
            break;
        }
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            if (Hoisted(i)) { // (GXV_PASSHOIST: made earlier from a target of its own; the EFB is not left here)
                for (int r = 0; r < 4; ++r) {
                    if (t.mat[r][0] != 0 || t.mat[r][1] != 0 || t.mat[r][2] != 0) g_ev[p0] |= EV_RC; // (kept: conservative)
                    if (t.mat[r][3] != 0) g_ev[p0] |= EV_RA;
                }
                clearEvent(p1, t.clear, t.src);
            } else if (t.skip) {
                if (t.fromPass && inPass) inPass = false;
                clearEvent(p1, t.clear, t.src);
            } else if (t.fromPass && inPass && passDirect == t.texId) {
                if (!t.passMore) { // (GXV_PASSDIRECT2: further copies of the pass follow, made from the texture; the pass stays open)
                    inPass = false;
                    clearEvent(p1, t.clear, t.src);
                }
            } else {
                if (!inPass && t.depth && !t.srcTex && g_hasDepthTex) g_ev[p0] |= EV_READ; // (the copy samples the EFB's depth)
                if (!inPass && !t.depth && !t.srcTex && !t.fill) {
                    for (int r = 0; r < 4; ++r) {
                        if (t.mat[r][0] != 0 || t.mat[r][1] != 0 || t.mat[r][2] != 0) g_ev[p0] |= EV_RC;
                        if (t.mat[r][3] != 0) g_ev[p0] |= EV_RA;
                    }
                }
                g_ev[p0] |= EV_LEAVE;
                if (t.srcTex) break; // (out of an atlas: no clear, no end of a pass)
                if (t.passMore && t.fromPass && inPass) break;
                inPass = false;
                clearEvent(p1, t.clear, t.src);
            }
            break;
        }
        case CMD_COPY_DISP:
            g_ev[p0] |= EV_RC; // (the picture)
            // (Not a place where the EFB is left, although the console's present hook usually ends the EFB's scene here:
            // while the loading screen is up it shows nothing, vitaGL goes on with the scene, and a scene planned to end
            // here would not store what the next frame reads. So the scene before it is planned up to the end of the frame.)
            clearEvent(p1, buf.copyDisp[c.arg].clear, buf.copyDisp[c.arg].src);
            break;
        default:
            break;
        }
    }
    const uint32_t none = static_cast<uint32_t>(positions);
    g_live.assign(positions + 1, 0);
    g_nextLeave.assign(positions + 1, none);
    g_nextRead.assign(positions + 1, none);
    g_nextWrite.assign(positions + 1, none);
    g_nextFull.assign(positions + 1, none);
    g_liveC.assign(positions + 1, 1);
    g_liveA.assign(positions + 1, 1);
    g_live[positions] = 1; // (the next frame may read what is left)
    for (size_t p = positions; p-- > 0;) {
        const uint8_t e = g_ev[p];
        const uint32_t here = static_cast<uint32_t>(p);
        g_live[p] = (e & EV_READ) ? 1 : ((e & EV_FULL) ? 0 : g_live[p + 1]);
        g_nextLeave[p] = (g_ev[p + 1] & EV_LEAVE) || p + 1 == positions ? here + 1 : g_nextLeave[p + 1];
        g_nextRead[p] = (e & EV_READ) ? here : g_nextRead[p + 1];
        g_nextWrite[p] = (e & EV_WRITE) ? here : g_nextWrite[p + 1];
        g_nextFull[p] = (e & EV_FULL) ? here : g_nextFull[p + 1];
        g_liveC[p] = (e & EV_RC) ? 1 : ((e & EV_FC) ? 0 : g_liveC[p + 1]);
        g_liveA[p] = (e & EV_RA) ? 1 : ((e & EV_FA) ? 0 : g_liveA[p + 1]);
    }
    g_valid = true;
    // GXV_PLANLOG=<n> (diagnostic): for the first n frames of 300 commands or more, what is live after each copy (depth, colour, alpha).
    static long planLog = DiagnosticInt("GXV_PLANLOG", 0);
    if (planLog > 0 && n >= 300) {
        --planLog;
        for (size_t i = 0; i < n; ++i) {
            if (buf.cmds[i].type != CMD_COPY_TEX) continue;
            const CopyTexCmd& t = buf.copyTex[buf.cmds[i].arg];
            const size_t p1 = 2 * i + 1;
            std::fprintf(stderr, "[planlog] frame %llu cmd %" VGX_PRIZU " copy tex %u (%dx%d -> %ux%u): after it live depth %d colour %d alpha %d\n", static_cast<unsigned long long>(buf.frame), i,
                         t.texId, t.src.w, t.src.h, t.dstW, t.dstH, g_live[p1 + 1], g_liveC[p1 + 1], g_liveA[p1 + 1]);
        }
    }
}

// GXV_DEPTHPLAN for the EFB scene that begins now (a certain beginning): lowers load / store, never raises them.
bool EfbDepthPlan(bool& load, bool& store) {
    using namespace dplan;
    if (!g_valid || !Enabled() || GpupassOff(GP_DEPTHPLAN)) return false;
    const size_t positions = g_ev.size() - 1;
    const size_t b = Pos();
    if (b >= positions) return false;
    const size_t e = g_nextLeave[b];
    const bool rd = g_nextRead[b] < e, wr = g_nextWrite[b] < e, fl = g_nextFull[b] < e;
    const bool readBeforeFull = rd && (!fl || g_nextRead[b] < g_nextFull[b]);
    const bool st = (wr || fl) && g_live[e];
    // (a scene that overwrites all of its depth and reads none before that needs nothing of what was there)
    const bool ld = readBeforeFull || (st && !fl);
    // GXV_GPUPASS_BREAK=1 (diagnostic, desktop): no EFB scene loads its depth - wrong on purpose, to see that GXV_PASSVERIFY notices.
    static const long brk = DiagnosticInt("GXV_GPUPASS_BREAK", 0);
    if (brk & 1) { load = false; return true; }
    load = load && ld;
    store = store && st;
    return true;
}

// ---- GXV_PASSDIRECT2: more passes drawn straight into their destination texture
// The front end draws an isolated pass straight into the texture its copy makes (PassCmd::directTex) only when the copy
// is 1:1 with no conversion at all. The plaza's bloom chain (five passes of 160x114 and 80x57, each one scene for the pass
// and one per copy: 12 scenes a frame) copies into RGB565 textures from an RGBA6 EFB: colour unchanged, alpha set to 1.
// Such a pass may be drawn into the destination as well when its draws neither write nor blend with alpha: the destination
// starts cleared to the pass's colour with the copy's alpha, the draws give it the same colour the pass buffer would get,
// and the alpha stays what the copy would have written. A second copy of the same pass (passMore) is then made from the
// destination texture instead of the pass buffer, when it does not read alpha either. Decided per pass, here, from the
// pass's draws and copies; the front end's records are not changed.
//   GXV_PASSDIRECT2=0   off; 1 (default) on
bool LateDirect(const CmdBuf& buf, size_t begin, PassCmd& ps, float f) {
    static const bool on = EnvInt("GXV_PASSDIRECT2", 1) != 0;
    if (!on || GpupassOff(GP_PASSDIRECT2) || ps.directTex || ps.atlasTex || EnvInt("GXV_PASSDIRECT", 1) == 0) return false;
    size_t end = begin + 1;
    while (end < buf.cmds.size() && buf.cmds[end].type != CMD_COPY_TEX && buf.cmds[end].type != CMD_PASS_BEGIN) ++end;
    if (end >= buf.cmds.size() || buf.cmds[end].type != CMD_COPY_TEX) return false;
    const CopyTexCmd& t = buf.copyTex[buf.cmds[end].arg];
    if (!t.fromPass || t.skip || t.depth || t.fill || t.srcTex || t.dstTexW || t.texId == 0) return false;
    if (t.dstW != ps.rect.w || t.dstH != ps.rect.h || t.src.x != ps.rect.x || t.src.y != ps.rect.y || t.src.w != ps.rect.w || t.src.h != ps.rect.h) return false;
    // Colour rows: identity; alpha row: identity or a constant.
    for (int r = 0; r < 3; ++r) {
        if (t.add[r] != 0) return false;
        for (int c = 0; c < 4; ++c)
            if (t.mat[r][c] != (r == c ? 1.0f : 0.0f)) return false;
    }
    const bool alphaIdentity = t.mat[3][0] == 0 && t.mat[3][1] == 0 && t.mat[3][2] == 0 && t.mat[3][3] == 1.0f && t.add[3] == 0;
    const bool alphaConst = t.mat[3][0] == 0 && t.mat[3][1] == 0 && t.mat[3][2] == 0 && t.mat[3][3] == 0;
    if (!alphaIdentity && !alphaConst) return false;
    for (size_t j = begin + 1; j < end; ++j) {
        if (buf.cmds[j].type != CMD_DRAW) {
            if (buf.cmds[j].type == CMD_TEX_UPLOAD || buf.cmds[j].type == CMD_TEX_FREE || buf.cmds[j].type == CMD_SET_TARGET) return false;
            continue;
        }
        const DrawRec& d = buf.draws[buf.cmds[j].arg];
        for (int m = 0; m < 8; ++m)
            if ((d.texMask & (1u << m)) && d.tex[m].texId == t.texId) return false; // (it samples its own destination)
        if (alphaConst) {
            const RenderState& rs = d.rs;
            const bool dstAlpha = rs.blend && (rs.blendSrc == BF_DST_ALPHA || rs.blendSrc == BF_INV_DST_ALPHA || rs.blendDst == BF_DST_ALPHA || rs.blendDst == BF_INV_DST_ALPHA);
            if (rs.alphaWrite || rs.dstAlphaPass || dstAlpha) return false;
        }
    }
    // Further copies of the pass (made from the destination texture now): they must not read its alpha.
    if (t.passMore && alphaConst) {
        for (size_t j = end + 1; j < buf.cmds.size() && buf.cmds[j].type == CMD_COPY_TEX; ++j) {
            const CopyTexCmd& u = buf.copyTex[buf.cmds[j].arg];
            if (!u.fromPass || u.depth) return false;
            for (int r = 0; r < 4; ++r)
                if (u.mat[r][3] != 0) return false;
            if (!u.passMore) break;
        }
    }
    // The size the destination gets at this render scale must be the pass target's (BeginPass's test).
    uint32_t direct = 0;
    PassCmd probe = ps;
    probe.directTex = t.texId;
    if (!PassTargetOf(probe, direct, f) || direct != t.texId) return false;
    ps.directTex = t.texId;
    if (alphaConst) ps.rgba[3] = t.add[3] < 0 ? 0.0f : (t.add[3] > 1 ? 1.0f : t.add[3]);
    return true;
}

// ---- GXV_PASSHOIST: a small copy taken out of the middle of the EFB's scene
// The plaza draws its picture, copies the EFB's alpha whole (A8, 640x456), draws a full-screen tint that samples that copy
// and two sun spheres into the alpha of one corner, and copies that corner (160x160, A8) for the lens flare; then the
// scene goes on. The corner copy ends the EFB's scene in the middle (store, copy, load: 2 x 1.17 MB of colour, plus depth).
// Everything the corner copy reads is known without the EFB, though: the planes it reads were copied whole a few draws
// before (the first copy reproduces them exactly), and the draws in between that touch the corner neither test depth nor
// sample the corner's texture. So right after the first copy the corner is made in a small target of its own: filled
// from the first copy's texture, the draws in between drawn again into it (the parts inside the corner), then copied as
// the original copy would. The EFB draws everything as before, but the corner copy no longer interrupts it (only its
// clear, if it has one, stays where it was). The EFB scene before the corner copy and the one after become one.
// Conditions (all checked per frame, else the copy stays where it is): the first copy is of the whole EFB, 1:1, without a
// clear, and its texture holds the planes the corner copy reads unchanged (alpha: its alpha row is "alpha"; colour: its
// colour rows are the identity, and then alpha as well); only draws come between the two copies; none of the draws that
// touch the corner reads depth or samples the corner copy's texture; the corner copy is of the EFB (no pass, atlas, fill).
//   GXV_PASSHOIST=0   off; 1 (default) on
std::vector<int32_t> g_hoistAt;  // per command: the corner copy whose pass is made right after this command's copy (-1: none)
std::vector<uint8_t> g_hoisted;  // per command: a corner copy made by such a pass (here only its clear is left)
inline bool Hoisted(size_t i) { return i < g_hoisted.size() && g_hoisted[i]; }
bool DrawTouches(const DrawRec& d, const EfbRect& r) {
    const int ax0 = std::max<int>(d.exX, d.rs.scX), ay0 = std::max<int>(d.exY, d.rs.scY);
    const int ax1 = std::min<int>(d.exX + d.exW, d.rs.scX + d.rs.scW), ay1 = std::min<int>(d.exY + d.exH, d.rs.scY + d.rs.scH);
    return ax0 < r.x + r.w && r.x < ax1 && ay0 < r.y + r.h && r.y < ay1 && ax1 > ax0 && ay1 > ay0;
}
inline bool OnRenderPixels(int efb, float scale) { // (an EFB coordinate that is a whole number of render pixels)
    const float v = static_cast<float>(efb) * scale;
    return std::fabs(v - static_cast<float>(Rnd(v))) < 1e-3f;
}
void PlanHoists(const CmdBuf& buf) {
    static const bool on = EnvInt("GXV_PASSHOIST", 1) != 0;
    g_hoistAt.clear();
    g_hoisted.clear();
    if (!on || GpupassOff(GP_PASSHOIST) || buf.crowdActive) return;
    const size_t n = buf.cmds.size();
    bool inPass = false, any = false;
    for (size_t i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[i];
        if (c.type == CMD_PASS_BEGIN) { inPass = true; continue; }
        if (c.type != CMD_COPY_TEX) continue;
        const CopyTexCmd& x = buf.copyTex[c.arg];
        const bool xInPass = inPass;
        if (!x.srcTex && !(x.passMore && x.fromPass)) inPass = false;
        if (xInPass || x.skip || x.fromPass || x.depth || x.fill || x.srcTex || x.dstTexW || x.passMore || x.clear.enable || x.texId == 0) continue;
        if (x.src.x != 0 || x.src.y != 0 || x.src.w != g_efbW || x.src.h != g_efbH || x.dstW != g_efbW || x.dstH != g_efbH) continue;
        const bool reproA = x.mat[3][0] == 0 && x.mat[3][1] == 0 && x.mat[3][2] == 0 && x.mat[3][3] == 1.0f && x.add[3] == 0;
        bool reproC = true;
        for (int r = 0; r < 3; ++r) {
            if (x.add[r] != 0) reproC = false;
            for (int col = 0; col < 4; ++col)
                if (x.mat[r][col] != (r == col ? 1.0f : 0.0f)) reproC = false;
        }
        if (!reproA) continue; // (colour alone is not enough: blending may weigh it by the alpha underneath)
        size_t j = i + 1;
        while (j < n && buf.cmds[j].type == CMD_DRAW) ++j;
        if (j >= n || buf.cmds[j].type != CMD_COPY_TEX) continue;
        const CopyTexCmd& y = buf.copyTex[buf.cmds[j].arg];
        if (y.skip || y.fromPass || y.depth || y.fill || y.srcTex || y.dstTexW || y.passMore || y.texId == 0 || y.texId == x.texId) continue;
        if (y.src.w <= 0 || y.src.h <= 0 || y.src.x < 0 || y.src.y < 0 || y.src.x + y.src.w > g_efbW || y.src.y + y.src.h > g_efbH) continue;
        if (y.src.w == g_efbW && y.src.h == g_efbH) continue; // (a full-size one gains nothing)
        // At this render scale the corner must lie on whole render pixels: the copy from the EFB samples it at x * ScaleX()
        // (bilinear: at 544x388, 448 is 380.8 and the copy is a resample shifted by a fifth of a pixel), the corner's own
        // target starts on a whole pixel and is copied 1:1. Else the copy stays where it is (equal pictures; GXV_PASSVERIFY).
        if (g_efbSamples > 1) continue; // (a multisampled EFB: the corner's own target is single-sampled, its copy would lose the smoothing)
        if (!OnRenderPixels(y.src.x, ScaleX()) || !OnRenderPixels(y.src.w, ScaleX()) || !OnRenderPixels(y.src.y, ScaleY()) || !OnRenderPixels(y.src.h, ScaleY()))
            continue;
        bool readsC = false;
        for (int r = 0; r < 4; ++r)
            if (y.mat[r][0] != 0 || y.mat[r][1] != 0 || y.mat[r][2] != 0) readsC = true;
        if (readsC && !reproC) continue;
        bool ok = true;
        for (size_t k = i + 1; k < j && ok; ++k) {
            const DrawRec& d = buf.draws[buf.cmds[k].arg];
            for (int m = 0; m < 8; ++m)
                if ((d.texMask & (1u << m)) && d.tex[m].texId == y.texId) ok = false;
            if (DrawTouches(d, y.src) && d.rs.depthTest && (d.rs.depthFunc & 7) != CMP_ALWAYS) ok = false;
        }
        if (!ok) continue;
        if (!any) { g_hoistAt.assign(n, -1); g_hoisted.assign(n, 0); any = true; }
        g_hoistAt[i] = static_cast<int32_t>(j);
        g_hoisted[j] = 1;
        i = j - 1; // (the corner copy cannot be a first copy itself)
    }
}
// Right after the first copy (command i): the corner copy (command j) made from a small target of its own.
void RunHoist(const CmdBuf& buf, size_t i, size_t j) {
    const CopyTexCmd& x = buf.copyTex[buf.cmds[i].arg];
    const CopyTexCmd& y = buf.copyTex[buf.cmds[j].arg];
    PassCmd ps;
    ps.rect = y.src;
    BeginPass(ps);
    if (g_pass < 0) return; // (empty at this render size: CopyTex then copies nothing either)
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero4[4] = {0, 0, 0, 0};
    Tex& xt = TexSlotFor(x.texId);
    BindTexForEdit(xt.id);
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    xt.minF = xt.magF = 0xFF;
    SetViewport(0, 0, g_passW, g_passH);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetColorMask(true, true);
    // (the pass target is the copy rectangle at render scale: the same rounding as BeginPass)
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    const float x0 = static_cast<float>(Rnd(y.src.x * sx)), y0 = static_cast<float>(Rnd(y.src.y * sy));
    DrawQuad(xt.id, x0 / g_rtW, y0 / g_rtH, static_cast<float>(g_passW) / g_rtW, static_cast<float>(g_passH) / g_rtH, identity, zero4);
    ++C.copyQuads;
    g_rsValid = false;
    for (size_t k = i + 1; k < j; ++k) {
        const DrawRec& d = buf.draws[buf.cmds[k].arg];
        if (DrawTouches(d, y.src)) Draw(buf, d);
    }
    CopyTexCmd made = y;
    made.fromPass = true;
    made.clear.enable = false; // (its clear stays at its place in the EFB's commands)
    CopyTex(made);
    ++C.hoists;
    GPUPASS_NOTE(3, "GXV_PASSHOIST: frame %llu: the %dx%d copy into texture %u made from its own target (%" VGX_PRIZU " draws between)\n", static_cast<unsigned long long>(g_progFrame),
                 y.src.w, y.src.h, y.texId, j - i - 1);
}

// ---- GXV_PASSHALF: offscreen passes of blurred pictures at half the render scale (item 5; default off)
// A pass whose draws sample nothing but pictures this frame copied (the bloom chain: 160x114 and 80x57 pictures made from a
// half-size copy of the glow mask, blurred step by step and added to the picture at the end) is drawn into a target of half
// its size at the current render scale, and the textures its copies make get half the size as well; whatever samples them
// later reads them through the same texture coordinates. This changes the picture (a blurrier bloom); GXV_PASSVERIFY gives
// the PSNR against full size. Only passes of at most a quarter of the EFB, not into an atlas, of at least 8x8 pixels after.
//   GXV_PASSHALF=0   off (default); 1 on
float PassScaleFor(const CmdBuf& buf, size_t begin, const PassCmd& ps) {
    static const bool on = EnvInt("GXV_PASSHALF", 0) != 0;
    if (!on || GpupassOff(GP_PASSHALF) || ps.atlasTex || ps.crowd.valid()) return 1.0f;
    if (static_cast<int>(ps.rect.w) * ps.rect.h * 4 > static_cast<int>(g_efbW) * g_efbH) return 1.0f;
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW) * 0.5f, sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH) * 0.5f;
    if (Rnd(ps.rect.w * sx) < 8 || Rnd(ps.rect.h * sy) < 8) return 1.0f;
    // The pictures this frame copies (a texture that is not one of them is a model's own: the pass is not a blur).
    auto copied = [&](uint32_t id) {
        for (const CopyTexCmd& t : buf.copyTex)
            if (t.texId == id && !t.depth) return true;
        return false;
    };
    bool any = false;
    for (size_t j = begin + 1; j < buf.cmds.size() && buf.cmds[j].type != CMD_COPY_TEX; ++j) {
        if (buf.cmds[j].type != CMD_DRAW) continue;
        const DrawRec& d = buf.draws[buf.cmds[j].arg];
        for (int m = 0; m < 8; ++m) {
            if (!(d.texMask & (1u << m))) continue;
            if (!copied(d.tex[m].texId)) return 1.0f;
            any = true;
        }
    }
    return any ? 0.5f : 1.0f;
}

// ---- GXV_FXHALF: additive effect sprites at a reduced size (item 5; after the Strikers port's EXT_FX path)
// The plaza's effects pass (6-14 ms on the console) is mostly overdraw of additive sprites (JPA particles: blend ADD, destination
// factor ONE, no depth write, depth-tested against the scene). Additive light adds up in any order and saturates the same way
// whether it is summed first (min(scene + min(a + b, 1), 1) = min(scene + a + b, 1)), so a run of such draws can be drawn into a
// target of 1/div the size that starts black and holds the scene's depth (each of its pixels the depth of the EFB pixel under its
// centre: a depth-copy pass that writes gl_FragDepth from the EFB's depth sampled as a texture), and then be added to the EFB in
// one full-screen pass (blend ONE, ONE; alpha too when a draw of the run writes alpha). The run ends at the first command that is
// not such a draw (anything that draws otherwise, a copy, a pass, the display copy; uploads and frees may come inside).
// Identified by render state only: blend ADD, destination factor ONE, source factor SRC_ALPHA or ONE, colour written, depth not
// written, no constant-alpha second pass, outside the offscreen passes. A run costs two scene changes and about a screen and a
// quarter of pixels (the depth copy at 1/div, the composite at full size), so only runs of GXV_FXHALF_MIN vertices (default 400)
// or more use it. The EFB's depth as a texture: on the desktop the EFB's depth texture; on the console a vitaGL built with
// DEPTH_SAMPLE (vglFramebufferDepthTexture: the depth buffer as its last scene stored it; the depth plan stores it for that).
//   GXV_FXHALF=0   off (default until shown on the console);  =2  half size (a quarter of the pixels);  =4  a quarter of each side
//   GXV_FXHALF_MIN=<vertices>   smallest run that is drawn so (default 400; 0: every run)
int FxDiv() {
    static const int div = [] {
        const long v = EnvInt("GXV_FXHALF", 0);
        return v == 2 || v == 4 ? static_cast<int>(v) : 0;
    }();
    return div;
}
bool FxEligible(const DrawRec& d) {
    const RenderState& rs = d.rs;
    return rs.blend && rs.blendEq != BE_REVSUB && rs.blendDst == BF_ONE && (rs.blendSrc == BF_SRC_ALPHA || rs.blendSrc == BF_ONE) && rs.colorWrite &&
           !(rs.depthTest && rs.depthWrite) && !rs.dstAlphaPass && d.idxCount > 0;
}
inline bool FxNeutral(uint8_t t) { return t == CMD_TEX_UPLOAD || t == CMD_TEX_FREE || t == CMD_BUF_UPLOAD || t == CMD_BUF_FREE; }
std::vector<uint8_t> g_fxCmd; // per command: a draw of a run drawn at the reduced size
bool g_fxOpen = false, g_fxAlpha = false;
int g_fxPool = -1;
GLuint g_fxDepthProg = 0;
[[maybe_unused]] GLuint g_vitaDepthTex = 0;
GLint g_fxDepthRect = -1;
float g_fxDepthU = 1.0f; // (the part of the depth texture's width that is the EFB's: on the console its rows are 32-sample aligned)
bool g_fxDepthFailed = false;
inline bool FxCmd(size_t i) { return i < g_fxCmd.size() && g_fxCmd[i]; }
bool EnsureFxDepthProgram() {
    if (g_fxDepthProg) return true;
    if (g_fxDepthFailed) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nvarying vec2 v_uv;\n"
        "void main() { v_uv = u_rect.xy + (a_pos.xy * 0.5 + 0.5) * u_rect.zw; gl_Position = vec4(a_pos.xy, 0.0, 1.0); }\n";
    // (GLES2 names the output gl_FragDepthEXT; vitaGL's translator knows gl_FragDepth and drops extension lines)
    static const char* fs =
        "#ifdef GL_EXT_frag_depth\n#extension GL_EXT_frag_depth : enable\n#define VGX_FRAGDEPTH gl_FragDepthEXT\n#else\n#define VGX_FRAGDEPTH gl_FragDepth\n#endif\n"
        "precision highp float;\nuniform sampler2D u_src;\nvarying vec2 v_uv;\n"
        "void main() { VGX_FRAGDEPTH = texture2D(u_src, v_uv).r; gl_FragColor = vec4(0.0); }\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "fx depth vertex"), f = Compile(GL_FRAGMENT_SHADER, fs, "fx depth fragment");
    GLint ok = 0;
    GLuint p = 0;
    if (v && f) {
        p = G(glCreateProgram());
        if (!BadGlName(p)) {
            G(glAttachShader(p, v));
            G(glAttachShader(p, f));
            G(glBindAttribLocation(p, AT_POS, "a_pos"));
            G(glLinkProgram(p));
            G(glGetProgramiv(p, GL_LINK_STATUS, &ok));
        } else {
            p = 0;
        }
    }
    if (!ok) {
        g_fxDepthFailed = true;
        VGX_ERR("GXV_FXHALF: the depth-copy program did not build: effects stay at full size\n");
        GPUPASS_NOTE(1, "GXV_FXHALF: the depth-copy program did not build: effects stay at full size\n");
        return false;
    }
    g_fxDepthProg = p;
    UseProgram(p);
    g_fxDepthRect = G(glGetUniformLocation(p, "u_rect"));
    G(glUniform1i(G(glGetUniformLocation(p, "u_src")), 0));
    return true;
}
// The EFB's depth as a texture (0: not available now).
GLuint FxDepthTexture() {
#ifdef __VGX_VITA__
    if (!vglFramebufferDepthTexture) return 0;
    if (!g_vitaDepthTex) G(glGenTextures(1, &g_vitaDepthTex));
    BindTexForEdit(g_vitaDepthTex);
    // (the surface's rows are VGL_ALIGN(width, 32) samples: the texture is that wide, the EFB is its first g_rtW columns)
    const GLint row = vglFramebufferDepthTexture(g_efbFbo);
    if (row < g_rtW) return 0;
    g_fxDepthU = static_cast<float>(g_rtW) / static_cast<float>(row);
    return g_vitaDepthTex;
#else
    g_fxDepthU = 1.0f;
    return g_hasDepthTex ? g_efbDepthTex : 0;
#endif
}
// Before the frame's plan: which draws go to the reduced-size target.
void FxDecide(const CmdBuf& buf) {
    g_fxCmd.clear();
    const int div = FxDiv();
    bool on = div > 0 && !GpupassOff(GP_FXHALF) && !g_fxDepthFailed && !buf.crowdActive && !g_skipDraws;
    // (decided here, before the frame's depth plan, so that BeginFx does not refuse a run the plan has already cut the scene
    // for: no multisampled EFB (the depth texture is read one sample a pixel), enough video memory, the depth texture and
    // the programs available)
    if (on && (g_efbSamples > 1 || VramFree() < kFxVramMin || !FxDepthTexture() || !EnsureFxDepthProgram() || !EnsureCopyProgram())) on = false;
    static const uint32_t minVerts = static_cast<uint32_t>(EnvInt("GXV_FXHALF_MIN", 400));
    static const int scanFast = [] {
        const int savedErrno = errno;
        // GXV_FXSCANFAST: default 0 keeps the scan, 1 skips unobserved inactive FX, 2 checks its empty-mask decision against the scan.
        const long mode = EnvInt("GXV_FXSCANFAST", 0);
        errno = savedErrno;
        return mode == 1 || mode == 2 ? static_cast<int>(mode) : 0;
    }();
    bool emptyDecision = false;
    if (scanFast && div == 0 && !kBackendDiagnostics && !g_verify.read && !g_verify.shadow && !g_verify.refRun && buf.passes.empty()) {
        static const bool unobserved = [] {
            // Unusual spellings stay on the scan: moving a first overflowing workload parse would change its conditional errno side effect.
            const char* setting = std::getenv("GXV_WORKLOAD_PROBE");
            return (!setting || std::strcmp(setting, "0") == 0) && !workload::Enabled();
        }();
        emptyDecision = unobserved;
        if (emptyDecision && scanFast == 1) return;
    }
    const size_t n = buf.cmds.size();
    bool inPass = false, any = false;
    size_t runStart = ~static_cast<size_t>(0);
    uint32_t runVerts = 0;
    auto close = [&](size_t end) {
        if (runStart != ~static_cast<size_t>(0) && !g_verify.shadow) workload::CandidateRun(runVerts);
        if (runStart != ~static_cast<size_t>(0) && on && runVerts >= minVerts) {
            if (g_fxCmd.empty()) g_fxCmd.assign(n, 0);
            for (size_t k = runStart; k < end; ++k)
                if (buf.cmds[k].type == CMD_DRAW) g_fxCmd[k] = 1;
            any = true;
        }
        runStart = ~static_cast<size_t>(0);
        runVerts = 0;
    };
    for (size_t i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[i];
        if (c.type == CMD_DRAW && !inPass && FxEligible(buf.draws[c.arg])) {
            ++C.fxCandidates;
            if (runStart == ~static_cast<size_t>(0)) runStart = i;
            runVerts += buf.draws[c.arg].vtxCount;
            continue;
        }
        if (runStart != ~static_cast<size_t>(0) && !FxNeutral(c.type)) close(i);
        if (c.type == CMD_PASS_BEGIN) {
            PassCmd ps = buf.passes[c.arg];
            uint32_t direct = 0;
            inPass = PassTargetOf(ps, direct, 1.0f);
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            if (t.skip ? t.fromPass : !(t.srcTex || (t.passMore && t.fromPass))) inPass = false;
        }
    }
    close(n);
    if (any && !FxDepthTexture()) { // (the console: no DEPTH_SAMPLE vitaGL, or no EFB depth buffer yet)
        g_fxCmd.clear();
        GPUPASS_NOTE(2, "GXV_FXHALF: frame %llu: no depth texture of the EFB: effects at full size\n", static_cast<unsigned long long>(g_progFrame));
    }
    if (emptyDecision && !g_fxCmd.empty()) {
        // Verification always keeps the actual slow result, including a nonempty mask if this eligibility rule ever becomes stale.
        GPUPASS_NOTE(3, "GXV_FXSCANFAST: empty-mask mismatch; retaining the scanned mask\n");
    }
}
void BeginFx(const CmdBuf& buf, size_t i) {
    // (FxDecide checked all of this before the plan; should one still fail here, the plan's scene cut at this run does not
    // happen: drop the plan for the rest of the frame so that every later scene loads and stores its depth)
    if (VramFree() < kFxVramMin) { g_fxCmd.clear(); dplan::g_valid = false; return; }
    const GLuint depth = FxDepthTexture();
    if (!depth || !EnsureFxDepthProgram() || !EnsureCopyProgram()) { g_fxCmd.clear(); dplan::g_valid = false; return; }
    const int div = FxDiv();
    const int w = (g_rtW + div - 1) / div, h = (g_rtH + div - 1) / div;
    g_fxPool = AcquireFbo(w, h, 0, true);
    g_pool[static_cast<size_t>(g_fxPool)].held |= PoolHeld::FX;
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetColorMask(true, true);
    SetDepthMask(true);
    G(glClearColor(0, 0, 0, 0));
    G(glClearDepthf(1.0f));
    TouchTarget(DN_CLEARED);
    G(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    ++C.clears;
    // The scene's depth: the EFB depth under each pixel's centre (point sampling).
    SetColorMask(false, false);
    SetBlendOff();
    SetCull(CULL_NONE);
    SetDepthTest(true, GL_ALWAYS);
    SetDepthMask(true);
    UseProgram(g_fxDepthProg);
    BindTex(0, depth);
#if !defined(__VGX_VITA__)
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
#endif
    G(glUniform4f(g_fxDepthRect, 0.0f, 0.0f, g_fxDepthU, 1.0f));
    ++C.uniformCalls;
    g_uniDirtyV = g_uniDirtyF = false;
    if (S.layoutVbo != g_quadVbo) {
        ++C.layoutChanges;
        BindArrayBuf(g_quadVbo);
        for (int a = 1; a < AT_COUNT; ++a)
            if (S.attrEnabled & (1u << a)) {
                G(glDisableVertexAttribArray(static_cast<GLuint>(a)));
                S.attrEnabled &= ~(1u << a);
                S.attrConstSet &= ~(1u << a);
            }
        if (!(S.attrEnabled & 1u)) { G(glEnableVertexAttribArray(AT_POS)); S.attrEnabled |= 1u; }
        G(glVertexAttribPointer(AT_POS, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        S.layoutVbo = g_quadVbo;
        S.layoutMask = 0;
        S.layoutBase = 0;
    }
    TouchTarget();
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    ++C.copyQuads;
    // The run's draws: like a pass over the whole EFB at 1/div of the render scale.
    g_pass = g_fxPool;
    g_inPass = true;
    g_passDirectTex = 0;
    g_passX = g_passY = 0;
    g_passW = w;
    g_passH = h;
    g_passScale = static_cast<float>(w) / static_cast<float>(g_rtW);
    g_rsValid = false;
    g_fxOpen = true;
    g_fxAlpha = false;
    ++C.fxRuns;
    GPUPASS_NOTE(3, "GXV_FXHALF: frame %llu: a run of additive draws into a %dx%d target with the scene's depth\n", static_cast<unsigned long long>(g_progFrame), w, h);
    (void)buf;
    (void)i;
}
void EndFx() {
    if (!g_fxOpen) return;
    g_fxOpen = false;
    PoolFbo& fx = g_pool[static_cast<size_t>(g_fxPool)];
    fx.held &= static_cast<uint8_t>(~PoolHeld::FX);
    const GLuint color = fx.color;
    g_inPass = false;
    g_pass = -1;
    g_passScale = 1.0f;
    BindFbo(g_efbFbo);
    SetViewport(0, 0, g_rtW, g_rtH);
    SetScissor(false);
    SetDepthTest(false);
    SetDepthMask(false);
    SetCull(CULL_NONE);
    SetColorMask(true, g_fxAlpha);
    SetBlend(GL_FUNC_ADD, GL_ONE, GL_ONE);
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero4[4] = {0, 0, 0, 0};
    DrawQuad(color, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero4);
    ++C.copyQuads;
    g_rsValid = false;
}

// ---- GXV_PASSVERIFY=<n> (desktop diagnostic): proof that the frame's picture does not change
// Every n-th frame that has no shader work is executed three times: twice with everything of this section switched off
// (GXV_DEPTHPLAN, GXV_PASSDIRECT2, GXV_EFBSWAP, GXV_PASSHALF: reference runs A and C, which neither present nor count) and
// once as configured (B, the run that is shown). The EFB is read back at each display copy and B is compared with C (A with
// C tells whether the frame depends on what the run before it left behind: then the comparison says nothing). One line per
// frame: pixels that differ, the largest difference, PSNR. In B the console's depth memory is modelled (GXV_PASSVERIFY_SIM,
// default 1, needs a GLES 3 context for glBlitFramebuffer): a scene that does not load its depth starts with it set to 0
// (everything fails a LEQUAL test), one that loads gets what the last storing scene left, one that does not store leaves
// that unchanged. A depth plan that leaves out a load or store that matters then shows up as a different picture.

#if !defined(__VGX_VITA__)
extern "C" void glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) __attribute__((weak));
struct DepthSim {
    bool on = false;          // modelling the depth memory in this run
    bool prevEfb = true, prevStore = true;
    GLuint fbo = 0, tex = 0, rb = 0;
    int w = 0, h = 0;
    uint32_t poisons = 0, loads = 0, stores = 0;
} g_dsim;
void DepthSimBlit(bool toMemory) {
    if (g_dsim.w != g_rtW || g_dsim.h != g_rtH || !g_dsim.fbo) {
        if (!g_dsim.fbo) G(glGenFramebuffers(1, &g_dsim.fbo));
        G(glBindFramebuffer(GL_FRAMEBUFFER, g_dsim.fbo));
        if (g_hasDepthTex) {
            if (!g_dsim.tex) G(glGenTextures(1, &g_dsim.tex));
            BindTexForEdit(g_dsim.tex);
            G(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, g_rtW, g_rtH, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
            SetupNewTexture(GL_NEAREST);
            G(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_dsim.tex, 0));
        } else {
            if (!g_dsim.rb) G(glGenRenderbuffers(1, &g_dsim.rb));
            G(glBindRenderbuffer(GL_RENDERBUFFER, g_dsim.rb));
            G(glRenderbufferStorage(GL_RENDERBUFFER, g_hasDepth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16, g_rtW, g_rtH));
            G(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_dsim.rb));
        }
        g_dsim.w = g_rtW;
        g_dsim.h = g_rtH;
    }
    const GLenum kRead = 0x8CA8, kDraw = 0x8CA9; // GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER
    G(glBindFramebuffer(kRead, toMemory ? g_efbFbo : g_dsim.fbo));
    G(glBindFramebuffer(kDraw, toMemory ? g_dsim.fbo : g_efbFbo));
    if (S.scissorOn == 1) G(glDisable(GL_SCISSOR_TEST));
    glBlitFramebuffer(0, 0, g_rtW, g_rtH, 0, 0, g_rtW, g_rtH, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (S.scissorOn == 1) G(glEnable(GL_SCISSOR_TEST));
    G(glBindFramebuffer(GL_FRAMEBUFFER, S.fbo));
}
void DepthSimScene(bool efb, bool certain, bool load, bool store) {
    if (!g_dsim.on) return;
    if (efb && !certain && g_dsim.prevEfb) return; // (vitaGL goes on with the scene before: nothing ends, nothing begins)
    if (g_dsim.prevEfb && g_dsim.prevStore) { DepthSimBlit(true); ++g_dsim.stores; }
    if (efb) {
        if (load || !certain) {
            DepthSimBlit(false);
            ++g_dsim.loads;
        } else {
            // Not loaded: the tile starts with whatever; 0 makes every LEQUAL test fail.
            const GLuint keep = S.fbo;
            G(glBindFramebuffer(GL_FRAMEBUFFER, g_efbFbo));
            if (S.scissorOn == 1) G(glDisable(GL_SCISSOR_TEST));
            G(glDepthMask(GL_TRUE));
            G(glClearDepthf(0.0f));
            G(glClear(GL_DEPTH_BUFFER_BIT));
            if (S.depthMask == 0) G(glDepthMask(GL_FALSE));
            if (S.scissorOn == 1) G(glEnable(GL_SCISSOR_TEST));
            G(glBindFramebuffer(GL_FRAMEBUFFER, keep));
            ++g_dsim.poisons;
        }
    }
    g_dsim.prevEfb = efb;
    g_dsim.prevStore = store;
}
#else
void DepthSimScene(bool, bool, bool, bool) {}
#endif
// ---- GXV_EFBSWAP: a full-size copy of the EFB without a copy
// A copy of the whole EFB, 1:1, with no conversion (the plaza's RGBA8 copy of the finished picture, kept for the bloom's
// final composite) is a scene of its own on the console: the EFB is stored, a 640x456 target is loaded, drawn with the
// copy quad and stored, and the EFB is loaded again. Instead the EFB's colour buffer becomes the texture (it holds exactly
// the copy's result once the EFB's scene has ended), and the EFB goes on with another colour buffer of the same size: no
// copy scene. What the EFB held is needed again only in the planes that are read before being overwritten whole (the plan
// above; without a plan for the frame: colour and alpha); those are drawn back from the old buffer by one quad at the
// start of the EFB's next scene (efbRestores). In the plaza nothing is: the next draw covers the colour and the alpha is
// never read again in the frame. The buffers rotate: a texture that holds one gives it back when it is written
// otherwise, freed, or copied into this way again.
//   GXV_EFBSWAP=0   off; 1 (default) on
// Ownership (for the pool eviction the resmsaa agent adds to g_pool): the EFB's colour buffers are never pool entries. One
// is the EFB's (g_efbColor), the others are held by textures (Tex::efbBuf) or wait in g_efbSpares; a texture that holds one
// is never attached to a pooled framebuffer object (CopyTexture gives the buffer back before any target is made of it), so
// nothing that evicts pool entries can free them, and they are freed only here. Bounded: a buffer is made only when no spare
// is left, there are never more spares than kEfbSparesMax (the rest are deleted), so the buffers in all are at most the
// textures copied into this way at one time plus 1 + kEfbSparesMax. No new buffer is made with less than kFxVramMin of video
// memory free (the copy is then made the ordinary way).
// Counted exactly: g_efbBufs = buffers this section made (or took over) minus buffers it let go (deleted, or forgotten under
// GXV_KEEP_RT). The EFB always has one buffer (whichever it is; CreateTarget gives it new storage at a change of size, or
// under GXV_KEEP_RT forgets it and makes another: one either way), so g_efbBufs must equal the buffers held by textures plus
// the spares at all times; GpupassMemReport checks that (a leak or a double free shows as a mismatch).
struct EfbSpare { GLuint id; int w, h; };
std::vector<EfbSpare> g_efbSpares;
constexpr size_t kEfbSparesMax = 2;
int64_t g_efbBufs = 0;
void EfbBufLetGo(GLuint id) { // (a buffer of the rotation is deleted, or forgotten under GXV_KEEP_RT)
    if (!KeepRenderTargets()) {
        G(glDeleteTextures(1, &id));
        ++C.texDeletes;
    }
    ForgetTex(id);
    --g_efbBufs;
}
// ApplyRung (a change of the render scale): the spares have the old size. Under GXV_KEEP_RT they stay (EfbSwap lets them go
// one at a time when it meets them), like every render target there.
void DropEfbSpares() {
    if (KeepRenderTargets()) return;
    for (const EfbSpare& sp : g_efbSpares) EfbBufLetGo(sp.id);
    g_efbSpares.clear();
}
bool EfbSwapEnabled() {
    static const bool on = EnvInt("GXV_EFBSWAP", 1) != 0;
    return on;
}
void EfbBufferBack(Tex& t) {
    if (t.id) {
        if (g_efbSpares.size() < kEfbSparesMax || KeepRenderTargets()) g_efbSpares.push_back(EfbSpare{t.id, t.w, t.h});
        else EfbBufLetGo(t.id);
    }
    t = Tex{};
}
bool EfbSwap(const CopyTexCmd& c) {
    if (!EfbSwapEnabled() || GpupassOff(GP_EFBSWAP) || g_inPass || g_skipDraws) return false;
    if (c.skip || c.fromPass || c.depth || c.fill || c.srcTex || c.dstTexW || c.texId == 0 || c.passMore) return false;
    if (c.src.x != 0 || c.src.y != 0 || c.src.w != g_efbW || c.src.h != g_efbH || c.dstW != g_efbW || c.dstH != g_efbH) return false;
    for (int r = 0; r < 4; ++r) {
        if (c.add[r] != 0) return false;
        for (int col = 0; col < 4; ++col)
            if (c.mat[r][col] != (r == col ? 1.0f : 0.0f)) return false;
    }
    Tex& t = TexSlotFor(c.texId);
    if (t.id == g_efbColor) return false; // (cannot happen)
    // (a new buffer is made only when no spare has the current render size - spares of an older size, left by a change of the
    // render scale, are let go below; with too little video memory the copy is made the ordinary way)
    bool spareFits = false;
    for (const EfbSpare& sp : g_efbSpares) spareFits = spareFits || (sp.w == g_rtW && sp.h == g_rtH);
    if (!spareFits && VramFree() < kFxVramMin) return false;
    const GLuint old = g_efbColor;
    if (t.id && !t.efbBuf && KeepRenderTargets() && t.renderTarget && t.w == g_rtW && t.h == g_rtH && g_efbSpares.size() < kEfbSparesMax) {
        // (GXV_KEEP_RT, Vita3K: a render target is never deleted; its own storage of the same size becomes a spare instead of
        // being forgotten, so a texture that goes back and forth between the two ways does not leak one each time)
        DetachFromPool(t.id); // (an EFB buffer is never attached to a pooled object)
        ForgetTex(t.id);
        g_efbSpares.push_back(EfbSpare{t.id, t.w, t.h});
        ++g_efbBufs; // (taken over)
        t = Tex{};
    }
    if (t.id) DeleteTex(t); // (its own storage, or the EFB buffer it had: back to the spares)
    GLuint fresh = 0;
    while (!g_efbSpares.empty() && !fresh) {
        const EfbSpare sp = g_efbSpares.back();
        g_efbSpares.pop_back();
        if (sp.w == g_rtW && sp.h == g_rtH) fresh = sp.id;
        else EfbBufLetGo(sp.id); // (another render size: deleted; under GXV_KEEP_RT forgotten)
    }
    if (!fresh) {
        G(glGenTextures(1, &fresh));
        BindTexForEdit(fresh);
        G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_rtW, g_rtH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
        if (!BoundTexHasStorage(g_rtW, g_rtH)) {
            // (the console reports no error when vitaGL has no memory for it: a framebuffer with such a texture attached crashes
            // the next scene. The EFB keeps its buffer; `t` was emptied above, so the ordinary copy makes it afresh.)
            G(glDeleteTextures(1, &fresh));
            ++C.texDeletes;
            VGX_LOG_N(8, "GXV_EFBSWAP: no memory for a %dx%d colour buffer: ordinary copy\n", g_rtW, g_rtH);
            return false;
        }
        SetupNewTexture(GL_LINEAR);
        ++g_efbBufs;
        VGX_LOG("GXV_EFBSWAP: colour buffer %dx%d made (%u spares)\n", g_rtW, g_rtH, static_cast<unsigned>(g_efbSpares.size()));
    }
    // The EFB's colour texture is sampled by the copies with linear filtering and clamping (a half-size copy averages 2x2
    // through it); a buffer that was a texture since has whatever sampler state its last use left.
    BindTexForEdit(fresh);
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    t.id = old;
    t.w = static_cast<uint16_t>(g_rtW);
    t.h = static_cast<uint16_t>(g_rtH);
    t.mips = false;
    t.renderTarget = true;
    t.efbBuf = true;
    t.wrapS = t.wrapT = t.minF = t.magF = 0xFF; // (whatever the buffer's sampler state is: set at its next use)
    g_efbColor = fresh;
    BindFbo(g_efbFbo);
    AttachColor(fresh);
    g_rsValid = false;
    ++C.efbSwaps;
    ++g_efbSwapsTotal;
    // The planes of the old contents that are read before they are overwritten whole.
    bool keepC = true, keepA = true;
    if (dplan::g_valid) {
        const size_t p = 2 * dplan::g_cmd + 1;
        if (p < dplan::g_liveC.size()) { keepC = dplan::g_liveC[p] != 0; keepA = dplan::g_liveA[p] != 0; }
    }
#if !defined(__VGX_VITA__)
    if (g_dsim.on) {
        // (verification: what a buffer that is not drawn back holds is not known; magenta shows it)
        if (S.scissorOn == 1) G(glDisable(GL_SCISSOR_TEST));
        G(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
        G(glClearColor(1.0f, 0.0f, 1.0f, 0.5f));
        G(glClear(GL_COLOR_BUFFER_BIT));
        S.colorMask = -1;
        if (S.scissorOn == 1) G(glEnable(GL_SCISSOR_TEST));
    }
#endif
    static const long brk = DiagnosticInt("GXV_GPUPASS_BREAK", 0);
    if (brk & 2) keepC = keepA = false; // (=2, diagnostic: never drawn back - wrong on purpose, for GXV_PASSVERIFY)
    if (keepC || keepA) {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, zero4[4] = {0, 0, 0, 0};
        BindTexForEdit(old);
        // (1:1: the nearest texel is the one, whatever the precision of the texture coordinates)
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        SetViewport(0, 0, g_rtW, g_rtH);
        SetScissor(false);
        SetBlendOff();
        SetDepthTest(false);
        SetDepthMask(false);
        SetCull(CULL_NONE);
        SetColorMask(keepC, keepA);
        DrawQuad(old, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero4);
        ++C.copyQuads;
        ++C.efbRestores;
        if (g_verify.read) g_verify.restorePlanes |= (keepC ? 1u : 0u) | (keepA ? 2u : 0u);
    }
    GPUPASS_NOTE(3, "GXV_EFBSWAP: frame %llu: copy into texture %u without a copy scene (drawn back: colour %d, alpha %d)\n", static_cast<unsigned long long>(g_progFrame),
                 c.texId, keepC ? 1 : 0, keepA ? 1 : 0);
    Clear(c.clear, c.src);
    return true;
}

// What the gpupass section holds in video memory, and the pool beside it, every GXV_GPUPASS_MEMLOG frames (default 1800; 0: never):
// pooled framebuffer objects (with their own colour and depth, the effects' and the corner copy's targets among them), the EFB
// colour buffers of GXV_EFBSWAP (in textures and spare), and the console's free video memory. For long runs: the numbers must
// stay flat.
void GpupassMemReport(uint64_t frame) {
    static const long every = EnvInt("GXV_GPUPASS_MEMLOG", 1800);
    if (every <= 0 || frame == 0 || frame % static_cast<uint64_t>(every) != 0) return;
    uint64_t poolBytes = 0;
    unsigned withColor = 0, withDepth = 0;
    for (const PoolFbo& f : g_pool) {
        if (f.color) { ++withColor; poolBytes += static_cast<uint64_t>(f.w) * f.h * 4; }
        if (f.depthTex || f.depthRb) { ++withDepth; poolBytes += static_cast<uint64_t>(f.w) * f.h * 4; }
    }
    unsigned heldPool = 0;
    for (const PoolFbo& f : g_pool) heldPool += f.held ? 1u : 0u;
    unsigned inTex = 0;
    uint64_t efbBytes = 0;
    for (const Tex& t : g_tex)
        if (t.efbBuf) { ++inTex; efbBytes += static_cast<uint64_t>(t.w) * t.h * 4; }
    for (const EfbSpare& sp : g_efbSpares) efbBytes += static_cast<uint64_t>(sp.w) * sp.h * 4;
    const size_t vram = VramFree();
    const int64_t live = static_cast<int64_t>(inTex) + static_cast<int64_t>(g_efbSpares.size());
    char line[400];
    std::snprintf(line, sizeof line, "gpupass memory at frame %llu: render %dx%d; %u pooled objects (%u own colour, %u depth; %.1f MiB; %u held), EFB buffers %u in textures + %u spare = %.1f MiB (accounted %lld: %s), video memory free %s%.1f MiB\n",
                  static_cast<unsigned long long>(frame), g_rtW, g_rtH, static_cast<unsigned>(g_pool.size()), withColor, withDepth, poolBytes / 1048576.0, heldPool, inTex,
                  static_cast<unsigned>(g_efbSpares.size()), efbBytes / 1048576.0, static_cast<long long>(g_efbBufs), g_efbBufs == live ? "ok" : "MISMATCH",
                  vram == static_cast<size_t>(-1) ? "n/a " : "", vram == static_cast<size_t>(-1) ? 0.0 : vram / 1048576.0);
#ifdef __VGX_VITA__
    port_log("[vgx] %s", line);
#else
    std::fprintf(stderr, "[vgx] %s", line);
#endif
}

// The display copy of a verified frame: its picture (the EFB rectangle) is kept. True: a reference run (nothing is shown).
bool VerifyRead(const CopyDispCmd& c) {
    if (!g_verify.read) return false;
    const float sx = static_cast<float>(g_rtW) / static_cast<float>(g_efbW), sy = static_cast<float>(g_rtH) / static_cast<float>(g_efbH);
    int x = Rnd(c.src.x * sx), y = Rnd(c.src.y * sy), w = Rnd(c.src.w * sx), h = Rnd(c.src.h * sy);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > g_rtW) w = g_rtW - x;
    if (y + h > g_rtH) h = g_rtH - y;
    if (w > 0 && h > 0) {
        BindFbo(g_efbFbo);
        EfbForRead();
        g_verify.pix.resize(static_cast<size_t>(w) * h * 4);
        G(glPixelStorei(GL_PACK_ALIGNMENT, 1));
        G(glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_verify.pix.data()));
        g_verify.w = w;
        g_verify.h = h;
        g_verify.got = true;
    }
    if (!g_presentHook) {
        // (as the console's present hook would: the display is drawn, the next EFB scene is certainly a new one)
        g_inUseFbo = ~0u;
        dplan::g_afterHook = true;
    }
    return g_verify.shadow;
}
} // namespace
// END GPUPASS BACKEND

// BEGIN STATIC CROWD BACKEND
struct CrowdResident {
    staticcrowd::Tag tag;
    uint32_t tex = 0;
    GLuint storage = 0;
    int width = 0, height = 0, rtWidth = 0, rtHeight = 0;
};
std::array<CrowdResident, staticcrowd::kMaxTags> g_crowdResidents{};
uint32_t g_crowdResidentCount = 0;
struct CrowdCounters {
    uint64_t frames = 0, observedFrames = 0, tags = 0, invalidFrames = 0;
    uint64_t eligible = 0, captures = 0, reused = 0, invalidations = 0, fallback = 0;
    uint32_t rows = 0;
} g_crowdCounters;
void EraseCrowd(CrowdResident& entry) {
    if (!entry.tex) return;
    entry = {};
    --g_crowdResidentCount;
    ++g_crowdCounters.invalidations;
}
void ForgetCrowd(uint32_t tex) {
    if (!tex || !g_crowdResidentCount) return;
    for (auto& entry : g_crowdResidents) if (entry.tex == tex) EraseCrowd(entry);
}
void ResetCrowd() {
    if (!g_crowdResidentCount) return;
    for (auto& entry : g_crowdResidents) EraseCrowd(entry);
}

// Validate the entire range before skipping anything, including its terminal copy.
const CopyTexCmd* CrowdTerminal(const CmdBuf& buf, size_t begin, const PassCmd& pass, unsigned& draws) {
    draws = 0;
    if (!pass.crowd.valid() || !pass.directTex || pass.atlasTex || g_skipDraws) return nullptr;
    for (size_t i = begin + 1; i < buf.cmds.size(); ++i) {
        const Cmd& cmd = buf.cmds[i];
        switch (cmd.type) {
        case CMD_DRAW:
            if (cmd.arg >= buf.draws.size() || !buf.draws[cmd.arg].idxCount) return nullptr;
            ++draws;
            break;
        case CMD_TEX_UPLOAD:
            if (cmd.arg >= buf.uploads.size() || buf.uploads[cmd.arg].texId == pass.directTex) return nullptr;
            break;
        case CMD_TEX_FREE:
            if (cmd.arg == pass.directTex) return nullptr;
            break;
        case CMD_BUF_UPLOAD: case CMD_BUF_FREE: case CMD_SHADER: case CMD_PROGRAM: case CMD_VARIANT:
            break;
        case CMD_COPY_TEX: {
            if (cmd.arg >= buf.copyTex.size()) return nullptr;
            const auto& copy = buf.copyTex[cmd.arg];
            if (!draws || copy.texId != pass.directTex || copy.crowd != pass.crowd || copy.crowdFormat != 5 ||
                copy.skip || copy.depth || copy.fill || !copy.fromPass || copy.srcTex || copy.dstTexW || copy.dstTexH ||
                copy.dstW != pass.crowd.words[6] || copy.dstH != pass.crowd.words[7] ||
                copy.src.x != pass.rect.x || copy.src.y != pass.rect.y || copy.src.w != pass.rect.w || copy.src.h != pass.rect.h ||
                copy.dstW != copy.src.w || copy.dstH != copy.src.h) return nullptr;
            for (int r = 0; r < 4; ++r) {
                if (copy.add[r] != 0) return nullptr;
                for (int c = 0; c < 4; ++c) if (copy.mat[r][c] != float(r == c)) return nullptr;
            }
            return &copy;
        }
        default: return nullptr;
        }
    }
    return nullptr;
}
bool ResidentCrowd(const CopyTexCmd& copy) {
    if (!copy.texId || !g_crowdResidentCount) return false;
    for (auto& entry : g_crowdResidents) {
        if (entry.tex != copy.texId) continue;
        if (entry.tag == copy.crowd && copy.texId < g_tex.size()) {
            const auto& texture = g_tex[copy.texId];
            if (texture.id && texture.id == entry.storage && texture.renderTarget &&
                texture.w == entry.width && texture.h == entry.height &&
                g_rtW == entry.rtWidth && g_rtH == entry.rtHeight) return true;
        }
        EraseCrowd(entry);
    }
    return false;
}
void RememberCrowd(const CopyTexCmd& copy) {
    if (!copy.texId || copy.texId >= g_tex.size() || g_crowdResidentCount == staticcrowd::kMaxTags) {
        ++g_crowdCounters.fallback;
        return;
    }
    const auto& texture = g_tex[copy.texId];
    if (!texture.id || !texture.renderTarget || !texture.w || !texture.h) return;
    for (auto& entry : g_crowdResidents) {
        if (entry.tex) continue;
        entry = {copy.crowd, copy.texId, texture.id, texture.w, texture.h, g_rtW, g_rtH};
        ++g_crowdResidentCount;
        ++g_crowdCounters.captures;
        return;
    }
}
void ReportCrowd(const CmdBuf& buf) {
    if (!buf.crowdActive) return;
    ++g_crowdCounters.frames;
    g_crowdCounters.observedFrames += buf.crowdObservations != 0;
    g_crowdCounters.tags += buf.crowdTags;
    g_crowdCounters.invalidFrames += buf.crowdInvalid;
    static const bool enabled = [] {
        const char* value = std::getenv("MSC_STATIC_CROWD_DIAG");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled || g_crowdCounters.rows >= 128 || (buf.frame != 1 && buf.frame % 300 != 0)) return;
    ++g_crowdCounters.rows;
    const auto& c = g_crowdCounters;
    std::fprintf(stderr, "[static-crowd] frame=%llu frames=%llu observed_frames=%llu tags=%llu invalid_frames=%llu eligible=%llu captures=%llu reused=%llu invalidations=%llu fallback=%llu resident=%u gpu_complete=0\n",
        static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(c.frames),
        static_cast<unsigned long long>(c.observedFrames), static_cast<unsigned long long>(c.tags),
        static_cast<unsigned long long>(c.invalidFrames), static_cast<unsigned long long>(c.eligible),
        static_cast<unsigned long long>(c.captures), static_cast<unsigned long long>(c.reused),
        static_cast<unsigned long long>(c.invalidations), static_cast<unsigned long long>(c.fallback), g_crowdResidentCount);
}
// END STATIC CROWD BACKEND

int ExportCountLog() {
    if (!kBackendDiagnostics) return 0;
    const bool wasFrozen = g_glStats.Capture().Frozen();
    const int result = g_glStats.ExportPaused();
    if (!wasFrozen && g_glStats.Capture().Frozen())
        VGX_LOG("RAM backend CSV: %u bytes retained, %u rows dropped, export result %d\n",
                static_cast<unsigned>(g_glStats.Capture().ExportedBytes()), g_glStats.Capture().Dropped(), result);
    return result;
}

static void ExecuteFrame(const CmdBuf& buf) {
#if VGX_COLDTRACE
    coldtrace::SpanScope executeTrace(coldtrace::g_recorder,coldtrace::SpanKind::Execute,buf.frame);
#endif
    if (buf.crowdReset || (buf.crowdActive && (buf.crowdInvalid || !buf.crowdObservations))) ResetCrowd();
    g_validateFrame = g_validation && g_validation->Selected(buf.frame);
    if (g_validateFrame) g_validationFrame = ValidationFrame{};
    // GXV_PROF=1: wall time per command type, glFinish after every copy (charges deferred
    // rasterisation to the copy; for the desktop). =2: no glFinish (the CPU side of the GL
    // calls only: what the render thread itself spends, which is the number to compare with
    // its budget on the console). Both print every 100 frames, with the call census.
    static const int profMode = static_cast<int>(DiagnosticInt("GXV_PROF", 0));
    static const bool prof = profMode != 0;
    g_dprofOn = profMode == 3;
    const double tFrame = prof ? Now() : 0.0;
    static const int64_t sceneLogFrame = DiagnosticInt("GXV_SCENELOG", -1);
    g_sceneLog = sceneLogFrame >= 0 && static_cast<int64_t>(buf.frame) == sceneLogFrame;
    ++g_serial;
    EvictPrograms();
    AutoQualityFrame(); // (a new render scale, if the controller wants one, takes effect here: between frames)
    g_stats.drawCallsThisFrame = 0;
    g_progsThisFrame = 0;
    // Attribute constants are given again in every frame (see the notes at the top): the
    // first draw sets its whole layout, constants included.
    S.attrConstSet = 0;
    S.layoutVbo = ~0u;
    BindFbo(g_efbFbo);
    UploadStream(buf);
    if (prof) g_profStream += Now() - tFrame;
    // Shader work of this buffer (see SetProgressDisplay in vgx_cmd.h). Units are weighted by
    // what they cost on the console: a compile is hundreds of milliseconds, a link and a
    // patched variant are a few.
    g_sw = ShaderWork{};
    g_progFrame = buf.frame;
#if !defined(__VGX_VITA__)
    g_progFrameNow = buf.frame;
#endif
    for (const Cmd& c : buf.cmds) g_sw.total += c.type == CMD_SHADER ? 10u : (c.type == CMD_PROGRAM ? 2u : (c.type == CMD_VARIANT ? 1u : 0u));
    for (const Cmd& c : buf.warm) DeferWarm(buf, c);  // (the list's replay: done in slices, see RunWarm)
    const CopyTexCmd* crowdCapture = nullptr;
    bool retainCrowd = false;
    unsigned crowdDraws = 0, crowdDrawStart = 0;
    // GXV_CUT="from,step,count[,hold]" (diagnosis, for hosts whose frame buffer cannot be read
    // back): in frames of at least 250 commands, draws from command number `cut` on are left
    // out, so the picture shows the EFB as it was at that point; `cut` goes from `from` up in
    // `step`s, changing every `hold` frames (default 50) and starting over after `count`
    // values. A white bar at the top left tells which value a picture shows: its length is
    // 6 target pixels per step number (1-based). The first such frame's command list is logged.
    static int cutFrom = -1, cutStep = 1, cutCount = 1, cutHold = 50;
    static bool cutParsed = false, cutListed = false;
    static uint64_t cutFirstFrame = 0;
    if (!cutParsed) {
        cutParsed = true;
        if (const char* e = DiagnosticEnv("GXV_CUT")) {
            if (std::sscanf(e, "%d,%d,%d,%d", &cutFrom, &cutStep, &cutCount, &cutHold) < 3) cutFrom = -1;
            if (cutStep < 1) cutStep = 1;
            if (cutCount < 1) cutCount = 1;
            if (cutHold < 1) cutHold = 1;
        }
    }
    size_t cutAt = ~static_cast<size_t>(0);
    int cutNumber = 0;
    if (cutFrom >= 0 && buf.draws.size() >= 200) {
        if (!cutListed) {
            cutListed = true;
            cutFirstFrame = buf.frame;
            std::fprintf(stderr, "[cut] frame %llu: %u commands\n", static_cast<unsigned long long>(buf.frame), static_cast<unsigned>(buf.cmds.size()));
            for (size_t i = 0; i < buf.cmds.size() && i < 400; ++i) {
                const Cmd& k = buf.cmds[i];
                if (k.type == CMD_DRAW) {
                    const DrawRec& d = buf.draws[k.arg];
                    std::fprintf(stderr, "[cut] %3u draw %u: vs %u fs %u, %u idx, layout 0x%X, blend %u (%u,%u) cw %u aw %u ztest %u zwrite %u, scissor (%d,%d,%d,%d), tex0 %u\n", static_cast<unsigned>(i), k.arg,
                                 d.vsId, d.fsId, d.idxCount, d.layoutMask, d.rs.blend, d.rs.blendSrc, d.rs.blendDst, d.rs.colorWrite, d.rs.alphaWrite, d.rs.depthTest, d.rs.depthWrite,
                                 d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.tex[0].texId);
                } else if (k.type == CMD_COPY_TEX) {
                    const CopyTexCmd& t = buf.copyTex[k.arg];
                    std::fprintf(stderr, "[cut] %3u copy -> tex %u from (%d,%d,%d,%d), clear %d/%d/%d\n", static_cast<unsigned>(i), t.texId, t.src.x, t.src.y, t.src.w, t.src.h, t.clear.color,
                                 t.clear.alpha, t.clear.depth);
                } else {
                    std::fprintf(stderr, "[cut] %3u command type %u arg %u\n", static_cast<unsigned>(i), static_cast<unsigned>(k.type), k.arg);
                }
            }
        }
        cutNumber = static_cast<int>(((buf.frame - cutFirstFrame) / static_cast<uint64_t>(cutHold)) % static_cast<uint64_t>(cutCount));
        cutAt = static_cast<size_t>(cutFrom + cutNumber * cutStep);
    }
    // GXV_PASSTIME=<frame>[,<frame>...] (diagnosis, the console): in these frames the GPU is waited for (glFinish)
    // after every copy and pass change, and one line per stretch of commands says how long it took to get there:
    // the time between two copies, drawing included, and how many draws and triangles that was.
    // (an entry may be a range, "A-B"; GXV_PASSTIME_AT=<seconds>[,<n>] takes n frames, 4 by default, once that many seconds have
    // passed since the first frame: for a scene that is reached by a script and whose frame number is not known beforehand)
    static const char* const passTimeList = DiagnosticEnv("GXV_PASSTIME");
    static TimedPick passAt{DiagnosticEnv("GXV_PASSTIME_AT")};
    bool passTime = FrameInList(passTimeList, buf.frame);
    if (passAt.Take()) passTime = true;
    // GXV_DRAWTIME=<frame>[,<frame>...] (diagnosis, the console): the same wait after every draw. One line per shader
    // pair says how many draws and how much time they took together (minus the cost of an empty wait, measured first),
    // and the 16 slowest single draws are listed.
    static const char* const drawTimeList = DiagnosticEnv("GXV_DRAWTIME");
    static TimedPick drawAt{DiagnosticEnv("GXV_DRAWTIME_AT")};
    bool drawTime = FrameInList(drawTimeList, buf.frame);
    if (drawAt.Take()) drawTime = true;
    struct DrawTimeRow { uint32_t index; double ms; };
    std::vector<DrawTimeRow> drawRows;
    double dtMark = 0, dtEmpty = 0;
    if (drawTime) {
        glFinish();
        double best = 1e9;
        for (int i = 0; i < 8; ++i) {
            const double a = Now();
            glFinish();
            best = std::min(best, Now() - a);
        }
        dtEmpty = best;
        dtMark = Now();
    }
    struct PassTimeRow { uint32_t index, type, tex, w, h, draws, tris; double ms; };
    std::vector<PassTimeRow> passRows;
    uint32_t ptDraws = 0, ptTris = 0;
    double ptMark = 0;
    if (passTime) { glFinish(); ptMark = Now(); }
    FxDecide(buf);   // (GXV_FXHALF)
    PlanHoists(buf); // (GXV_PASSHOIST)
    PlanFrame(buf); // (GXV_DEPTHPLAN, GXV_EFBSWAP)
    if (cutAt != ~static_cast<size_t>(0) || passTime || drawTime) dplan::g_valid = false; // (diagnostic waits and cuts: no plan)
    RunWarm();  // (what an earlier buffer left of the deferred warm-up: a slice of it, before this frame's own commands)
    for (size_t commandIndex = 0; commandIndex < buf.cmds.size(); ++commandIndex) {
        const Cmd& c = buf.cmds[commandIndex];
        dplan::g_cmd = commandIndex;
        if (g_fxOpen && !(c.type == CMD_DRAW && FxCmd(commandIndex)) && !FxNeutral(c.type)) EndFx(); // (GXV_FXHALF: the run is over)
        if (commandIndex >= cutAt) {
            if (commandIndex == cutAt) {
                if (g_inPass) EndPass();
                BindFbo(g_efbFbo);
                g_rsValid = false;
                static const float zero[16] = {}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                SetScissor(false);
                SetBlendOff();
                SetDepthTest(false);
                SetDepthMask(false);
                SetCull(CULL_NONE);
                SetColorMask(true, true);
                SetViewport(0, 0, 6 * (cutNumber + 1), 6);
                DrawQuad(g_whiteTex, 0, 0, 1, 1, zero, white);
            }
            if (c.type == CMD_DRAW || c.type == CMD_PASS_BEGIN || c.type == CMD_COPY_TEX) continue;
        }
        const double t0 = prof ? Now() : 0.0;
        const uint32_t swUnits = g_sw.total ? (c.type == CMD_SHADER ? 10u : (c.type == CMD_PROGRAM ? 2u : (c.type == CMD_VARIANT ? 1u : 0u))) : 0u;
        const double swT0 = swUnits ? NowMs() : 0.0;
        switch (c.type) {
        case CMD_DRAW:
            if (FxCmd(commandIndex)) { // (GXV_FXHALF)
                if (!g_fxOpen) BeginFx(buf, commandIndex);
                if (g_fxOpen) {
                    const DrawRec& d = buf.draws[c.arg];
                    if (d.rs.alphaWrite) g_fxAlpha = true;
                    Draw(buf, d);
                    ++C.fxDraws;
                    C.fxVerts += d.vtxCount;
                    break;
                }
            }
            if (!retainCrowd) Draw(buf, buf.draws[c.arg]);
            break;
        case CMD_TEX_UPLOAD:
            ForgetCrowd(buf.uploads[c.arg].texId);
            Upload(buf.uploads[c.arg]);
            break;
        case CMD_TEX_FREE:
            ForgetCrowd(c.arg);
            if (c.arg < g_tex.size() && g_tex[c.arg].id) {
                DeleteTex(g_tex[c.arg]);
                --g_stats.textures;
            }
            break;
        case CMD_SHADER: {
            const ShaderSource* s = buf.shaders[c.arg];
            RegisterShader(s);
            CompileShader(s->fragment, s->id);
            break;
        }
        case CMD_BUF_UPLOAD: {
            const BufUpload& u = buf.bufUploads[c.arg];
            if (g_validation) g_validation->Upload(u);
            if (g_sbuf.size() <= u.bufId) g_sbuf.resize(u.bufId + 64);
            StaticBuf& sb = g_sbuf[u.bufId];
            if (!sb.vbo) { G(glGenBuffers(1, &sb.vbo)); G(glGenBuffers(1, &sb.ibo)); }
            BindArrayBuf(sb.vbo);
            G(glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(u.vtx.size()), u.vtx.data(), g_staticUsage));
            BindElemBuf(sb.ibo);
            G(glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(u.idx.size() * 2), u.idx.data(), g_staticUsage));
            C.staticUploads += 2;
            C.staticBytes += static_cast<uint32_t>(u.vtx.size() + u.idx.size() * 2);
            if (S.layoutVbo == sb.vbo) S.layoutVbo = ~0u; // new contents: pointers are set again
            break;
        }
        case CMD_BUF_FREE:
            if (g_validation) g_validation->Free(c.arg);
            if (c.arg < g_sbuf.size() && g_sbuf[c.arg].vbo) {
                StaticBuf& sb = g_sbuf[c.arg];
                G(glDeleteBuffers(1, &sb.vbo));
                G(glDeleteBuffers(1, &sb.ibo));
                // The names may be handed out again; a deleted buffer is also unbound by the GL.
                if (S.arrayBuf == sb.vbo) S.arrayBuf = ~0u;
                if (S.elemBuf == sb.ibo) S.elemBuf = ~0u;
                if (S.layoutVbo == sb.vbo) S.layoutVbo = ~0u;
                sb = StaticBuf{};
            }
            break;
        case CMD_PASS_BEGIN:
            crowdCapture = CrowdTerminal(buf, commandIndex, buf.passes[c.arg], crowdDraws);
            if (crowdCapture) ++g_crowdCounters.eligible;
            else if (buf.passes[c.arg].crowd.valid()) ++g_crowdCounters.fallback;
            retainCrowd = crowdCapture && ResidentCrowd(*crowdCapture);
            if (retainCrowd) ++g_crowdCounters.reused;
            crowdDrawStart = g_stats.drawCallsThisFrame;
            if (!retainCrowd) {
                ForgetCrowd(buf.passes[c.arg].directTex);
                ForgetCrowd(buf.passes[c.arg].atlasTex);
                PassCmd ps = buf.passes[c.arg];
                const float f = PassScaleFor(buf, commandIndex, ps); // (GXV_PASSHALF)
                if (LateDirect(buf, commandIndex, ps, f)) // (GXV_PASSDIRECT2: may draw it straight into its destination)
                    GPUPASS_NOTE(3, "GXV_PASSDIRECT2: frame %llu: pass %dx%d drawn straight into texture %u\n", static_cast<unsigned long long>(g_progFrame), ps.rect.w, ps.rect.h, ps.directTex);
                g_passScaleNext = f;
                BeginPass(ps);
            }
            break;
        case CMD_PROGRAM: GetProgram(static_cast<uint16_t>(c.arg >> 16), static_cast<uint16_t>(c.arg & 0xFFFFu)); break;
        case CMD_VARIANT: WarmVariant(buf.variants[c.arg]); break;
        case CMD_COPY_TEX: {
            const auto& copy = buf.copyTex[c.arg];
            if (Hoisted(commandIndex)) { // (GXV_PASSHOIST: made already; its clear is all that is left here)
                Clear(copy.clear, copy.src);
                break;
            }
            if (retainCrowd && crowdCapture == &copy) {
                EndPass();
                Clear(copy.clear, copy.src);
            } else {
                ForgetCrowd(copy.texId);
                const bool validCapture = crowdCapture == &copy && g_inPass && g_pass >= 0 &&
                    g_passDirectTex == copy.texId && g_staticCrowdPassComplete &&
                    g_stats.drawCallsThisFrame - crowdDrawStart == crowdDraws;
                CopyTex(copy);
                if (validCapture) RememberCrowd(copy);
                else if (crowdCapture) ++g_crowdCounters.fallback;
                if (commandIndex < g_hoistAt.size() && g_hoistAt[commandIndex] >= 0) RunHoist(buf, commandIndex, static_cast<size_t>(g_hoistAt[commandIndex]));
            }
            retainCrowd = false;
            crowdCapture = nullptr;
            break;
        }
        case CMD_COPY_DISP: CopyDisp(buf.copyDisp[c.arg]); break;
        case CMD_PRESENT:
            for (PoolFbo& f : g_pool) f.uses = 0;
            EvictIdlePool(); // (render-target memory: objects no frame has used for a while)
            g_stats.programsUsedThisFrame = g_progsThisFrame;
            break;
        case CMD_SET_TARGET:
            ResetCrowd();
            {
                const int oldEfbW = g_efbW, oldEfbH = g_efbH;
                g_efbW = buf.targets[c.arg].efbW;
                g_efbH = buf.targets[c.arg].efbH;
                if (!CreateTarget()) { g_efbW = oldEfbW; g_efbH = oldEfbH; } // (no memory: everything stays as it was)
            }
            BindFbo(g_efbFbo);
            g_rsValid = false;
            break;
        default: break;
        }
        // GXV_PROBE="x,y[,frame[,x2,y2]]" (diagnosis): after every command of the first frame from
        // `frame` on that has at least 250 commands, the EFB pixel(s) at these EFB coordinates
        // are read back and a line is logged whenever one changed, with what the command was.
        // (Desktop and console. In Vita3K a framebuffer object reads back as zeros: GXV_CUT there.)
        {
            static int px[2] = {-1, -1}, py[2] = {-1, -1};
            static long long probeFrom = -1;
            static bool parsed = false, probing = false, done = false;
            static uint8_t last[2][4];
            if (!parsed) {
                parsed = true;
                if (const char* e = DiagnosticEnv("GXV_PROBE")) {
                    long long f = 0;
                    const int n = std::sscanf(e, "%d,%d,%lld,%d,%d", &px[0], &py[0], &f, &px[1], &py[1]);
                    if (n >= 2) probeFrom = n >= 3 ? f : 0;
                }
            }
            if (probeFrom >= 0 && !done) {
                if (commandIndex == 0 && static_cast<long long>(buf.frame) >= probeFrom && buf.cmds.size() >= 250 && !g_skipDraws) {
                    probing = true;
                    std::memset(last, 0xEE, sizeof last);
                    std::fprintf(stderr, "[probe] frame %llu: %u commands, %u draws; pixels (%d,%d) (%d,%d); target %dx%d for EFB %dx%d, skip %d\n", static_cast<unsigned long long>(buf.frame),
                                 static_cast<unsigned>(buf.cmds.size()), static_cast<unsigned>(buf.draws.size()), px[0], py[0], px[1], py[1], g_rtW, g_rtH, g_efbW, g_efbH, g_skipDraws ? 1 : 0);
                }
                if (probing && !g_inPass) {
                    BindFbo(g_efbFbo);
                    EfbForRead();
                    for (int k = 0; k < 2; ++k) {
                        if (px[k] < 0) continue;
                        uint8_t now[4] = {0, 0, 0, 0};
                        G(glReadPixels(px[k] * g_rtW / g_efbW, py[k] * g_rtH / g_efbH, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, now));
                        if (std::memcmp(now, last[k], 4) != 0) {
                            char what[256] = "";
                            if (c.type == CMD_DRAW) {
                                const DrawRec& d = buf.draws[c.arg];
                                std::snprintf(what, sizeof what, "draw %u: vs %u fs %u, %u idx, layout 0x%X, blend %u (%u,%u) cw %u aw %u ztest %u zwrite %u, scissor (%d,%d,%d,%d), tex0 %u", c.arg,
                                              d.vsId, d.fsId, d.idxCount, d.layoutMask, d.rs.blend, d.rs.blendSrc, d.rs.blendDst, d.rs.colorWrite, d.rs.alphaWrite, d.rs.depthTest,
                                              d.rs.depthWrite, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH, d.tex[0].texId);
                            } else if (c.type == CMD_COPY_TEX) {
                                const CopyTexCmd& t = buf.copyTex[c.arg];
                                std::snprintf(what, sizeof what, "copy -> tex %u from (%d,%d,%d,%d), clear %d/%d/%d", t.texId, t.src.x, t.src.y, t.src.w, t.src.h, t.clear.color, t.clear.alpha,
                                              t.clear.depth);
                            }
                            std::fprintf(stderr, "[probe] pixel %d after command %u (type %u) %s: %02X%02X%02X%02X -> %02X%02X%02X%02X\n", k, static_cast<unsigned>(commandIndex),
                                         static_cast<unsigned>(c.type), what, last[k][0], last[k][1], last[k][2], last[k][3], now[0], now[1], now[2], now[3]);
                            std::memcpy(last[k], now, 4);
                        }
                    }
                    if (commandIndex + 1 == buf.cmds.size()) { probing = false; done = true; std::fprintf(stderr, "[probe] end of frame\n"); }
                }
            }
        }
        if (swUnits) {
            const double dt = NowMs() - swT0 + g_sw.simPending;
            g_sw.simPending = 0;
            g_sw.ms += dt;
            g_sw.sinceShow += dt;
            g_sw.done += swUnits;
            if (g_sw.ms >= 150.0 && g_sw.done < g_sw.total && g_sw.sinceShow >= 40.0) {
                ShowProgress();
                g_sw.sinceShow = 0;
            }
        }
        if (drawTime && c.type == CMD_DRAW) {
            glFinish();
            const double t = Now();
            drawRows.push_back({c.arg, t - dtMark});
            dtMark = t;
        } else if (drawTime) {
            dtMark = Now(); // (a copy or another command: not charged to the next draw)
        }
        if (passTime) {
            if (c.type == CMD_DRAW) {
                ++ptDraws;
                ptTris += buf.draws[c.arg].idxCount / 3;
            } else if (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP || c.type == CMD_PASS_BEGIN) {
                glFinish();
                const double t = Now();
                PassTimeRow r{static_cast<uint32_t>(commandIndex), c.type, 0, 0, 0, ptDraws, ptTris, t - ptMark};
                if (c.type == CMD_COPY_TEX) {
                    const CopyTexCmd& k = buf.copyTex[c.arg];
                    r.tex = k.texId;
                    r.w = k.dstW;
                    r.h = k.dstH;
                } else if (c.type == CMD_PASS_BEGIN) {
                    r.w = buf.passes[c.arg].rect.w;
                    r.h = buf.passes[c.arg].rect.h;
                }
                passRows.push_back(r);
                ptDraws = ptTris = 0;
                ptMark = t;
            }
        }
        if (prof) {
            if (profMode == 1 && (c.type == CMD_COPY_TEX || c.type == CMD_COPY_DISP)) glFinish(); // charge deferred rasterisation here
            g_prof[c.type] += Now() - t0;
            ++g_profCount[c.type];
        }
    }
    EndFx(); // (a run at the very end of the commands)
    if (drawTime) {
        struct Group { uint16_t vs, fs; uint32_t n = 0, tris = 0; double ms = 0; uint8_t blend = 0, src = 0, dst = 0, ztest = 0, mask = 0; };
        std::vector<Group> groups;
        for (const DrawTimeRow& r : drawRows) {
            const DrawRec& d = buf.draws[r.index];
            Group* g = nullptr;
            for (Group& x : groups)
                if (x.vs == d.vsId && x.fs == d.fsId && x.blend == d.rs.blend && x.src == d.rs.blendSrc && x.dst == d.rs.blendDst && x.ztest == d.rs.depthTest) { g = &x; break; }
            if (!g) {
                groups.push_back(Group{});
                g = &groups.back();
                g->vs = d.vsId; g->fs = d.fsId; g->blend = d.rs.blend; g->src = d.rs.blendSrc; g->dst = d.rs.blendDst; g->ztest = d.rs.depthTest; g->mask = d.texMask;
            }
            ++g->n;
            g->tris += d.idxCount / 3;
            g->ms += std::max(0.0, r.ms - dtEmpty);
        }
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.ms > b.ms; });
        double all = 0;
        for (const Group& g : groups) all += g.ms;
        std::fprintf(stderr, "[drawtime] frame %llu: %" VGX_PRIZU " draws, %.2f ms in all after taking off %.2f ms for each wait\n", static_cast<unsigned long long>(buf.frame), drawRows.size(), all, dtEmpty);
        for (size_t i = 0; i < groups.size() && i < 14; ++i) {
            const Group& g = groups[i];
            std::fprintf(stderr, "[drawtime]   %7.2f ms  %4u draws %6u tris  vs %u fs %u  blend %u (%u,%u) ztest %u texmask 0x%X\n", g.ms, g.n, g.tris, g.vs, g.fs, g.blend, g.src, g.dst, g.ztest, g.mask);
        }
        std::vector<DrawTimeRow> top = drawRows;
        std::sort(top.begin(), top.end(), [](const DrawTimeRow& a, const DrawTimeRow& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < top.size() && i < 12; ++i) {
            const DrawRec& d = buf.draws[top[i].index];
            std::fprintf(stderr, "[drawtime]   slowest: draw %u %.2f ms: %u tris, vs %u fs %u, blend %u (%u,%u), ztest %u zwrite %u, scissor (%d,%d,%d,%d), tex %u/%u/%u mask 0x%X\n", top[i].index,
                         top[i].ms - dtEmpty, d.idxCount / 3, d.vsId, d.fsId, d.rs.blend, d.rs.blendSrc, d.rs.blendDst, d.rs.depthTest, d.rs.depthWrite, d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH,
                         d.tex[0].texId, d.tex[1].texId, d.tex[2].texId, d.texMask);
        }
    }
    if (passTime) {
        glFinish();
        double all = 0;
        for (const PassTimeRow& r : passRows) all += r.ms;
        std::fprintf(stderr, "[passtime] frame %llu: %.2f ms to the last copy, %u draws after it\n", static_cast<unsigned long long>(buf.frame), all, ptDraws);
        for (const PassTimeRow& r : passRows)
            std::fprintf(stderr, "[passtime]   %8.2f ms  %5u draws %6u tris  then %s%s tex %u %ux%u (command %u)\n", r.ms, r.draws, r.tris,
                         r.type == CMD_COPY_TEX ? "copy" : (r.type == CMD_COPY_DISP ? "display copy" : "pass begin"), "", r.tex, r.w, r.h, r.index);
    }
    g_stats.shaderWorkMs += g_sw.ms;
    // One line for every frame whose shader work would have been seen as a hitch (always
    // printed: on the console this is the record of what start-up and first use cost).
    if (g_sw.ms >= 100.0 || g_sw.compiles >= 8)
        std::fprintf(stderr, "[vgx] frame %llu: %u shader compiles (%u answered by the simulated cache), %u program links, %.0f ms of shader work, progress bar shown %u times\n",
                     static_cast<unsigned long long>(buf.frame), g_sw.compiles, g_sw.simHits, g_sw.links, g_sw.ms, g_sw.shown);
    if (prof) {
        g_profTotal += Now() - tFrame;
#define X(n) g_profSum[k++] += C.n;
        { int k = 0; VGX_GL_COUNTERS(X) }
#undef X
        if (++g_profFrames % 100 == 0) {
            static const char* const nm[CMD_TYPE_COUNT] = {"draw", "upload", "free", "shader", "copytex", "copydisp", "present", "target", "bufupload",
                                                           "buffree", "passbegin", "program", "variant"};
            std::fprintf(stderr, "[vgx] prof (ms/frame over 100 frames%s): all=%.2f stream=%.2f", profMode == 1 ? ", glFinish after copies" : "", g_profTotal / 100.0,
                         g_profStream / 100.0);
            for (int i = 0; i < CMD_TYPE_COUNT; ++i) {
                if (g_profCount[i]) std::fprintf(stderr, " %s=%.2f(x%.1f)", nm[i], g_prof[i] / 100.0, g_profCount[i] / 100.0);
                g_prof[i] = 0;
                g_profCount[i] = 0;
            }
            std::fprintf(stderr, "\n[vgx] per frame:");
            static const char* const cn[] = {
#define X(n) #n,
                VGX_GL_COUNTERS(X)
#undef X
            };
            for (size_t i = 0; i < sizeof cn / sizeof cn[0]; ++i) {
                if (g_profSum[i]) std::fprintf(stderr, " %s=%.1f", cn[i], static_cast<double>(g_profSum[i]) / 100.0);
                g_profSum[i] = 0;
            }
            std::fprintf(stderr, "\n");
            if (g_dprofOn) {
                std::fprintf(stderr, "[vgx] draw stages (ms/frame): program=%.2f state=%.2f uniforms=%.2f textures=%.2f layout=%.2f target=%.2f call=%.2f\n",
                             g_dprof[0] / 100.0, g_dprof[1] / 100.0, g_dprof[2] / 100.0, g_dprof[3] / 100.0, g_dprof[4] / 100.0, g_dprof[5] / 100.0, g_dprof[6] / 100.0);
                for (double& d : g_dprof) d = 0;
            }
            g_profTotal = g_profStream = 0;
        }
    }
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) ResetCrowd();
    ReportCrowd(buf);
    if (err != GL_NO_ERROR) VGX_LOG_N(32, "GL error 0x%X after frame %llu command buffer\n", err, static_cast<unsigned long long>(buf.frame));
    if (!g_verify.shadow) WriteCounts(buf.frame);
    if (g_validation) {
        if (g_validateFrame) {
            const ValidationFrame& total = g_validationFrame;
            std::fprintf(stderr, "[validate] frame=%llu summary draws=%llu bad_draws=%llu unchecked_draws=%llu references=%llu vertex_bytes=%llu index_bytes=%llu uniform_bytes=%llu hash=%016llx shadow_bytes=%llu shadow_drops_total=%u detail_lines=%llu CPU-contract-only\n",
                         static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(total.draws), static_cast<unsigned long long>(total.badDraws),
                         static_cast<unsigned long long>(total.uncheckedDraws), static_cast<unsigned long long>(total.references), static_cast<unsigned long long>(total.vertexBytes), static_cast<unsigned long long>(total.indexBytes),
                         static_cast<unsigned long long>(total.uniformBytes), static_cast<unsigned long long>(total.hash), static_cast<unsigned long long>(g_validation->ShadowBytes()),
                         g_validation->shadowDrops, static_cast<unsigned long long>((total.draws < 16 ? total.draws : 16) + total.extraLines));
            std::fflush(stderr);
        }
        g_validation->Finish(buf.frame);
        if (!g_validation->Enabled()) g_validation.reset();
    }
    g_validateFrame = false;
#ifdef VGX_GL_STUB
    ReportGlMem(buf.frame);
#endif
    if (g_verify.read) g_verify.counts = C;
    if (!g_verify.shadow) GpupassMemReport(buf.frame);
    g_lastScenes = C.scenes;
    if (workload::Enabled() && !g_verify.shadow) {
        uint32_t draws = 0, copies = 0, displays = 0, passes = 0;
        for (const Cmd& c : buf.cmds) {
            draws += c.type == CMD_DRAW;
            copies += c.type == CMD_COPY_TEX;
            displays += c.type == CMD_COPY_DISP;
            passes += c.type == CMD_PASS_BEGIN;
        }
        workload::Picture(draws, copies, displays, passes, C.scenes, C.copyQuads, C.fxCandidates, C.fxRuns);
    }
    C = Counts{};
    // The host presents (and swaps) between frames: whatever is drawn next starts a scene.
    g_inUseFbo = ~0u;
    dplan::g_valid = false;
    dplan::g_afterHook = false;
}

// BEGIN GPUPASS VERIFY
// GXV_PASSVERIFY=<n> (see PassVerify above; desktop only).
static void VerifyFrame(const CmdBuf& buf) {
    struct Pic { std::vector<uint8_t> px; int w = 0, h = 0; bool ok = false; Counts n; uint32_t planes = 0; };
    auto run = [&](bool ref, bool shadow, bool sim, Pic& out) {
        g_verify.refRun = ref;
        g_verify.shadow = shadow;
        g_verify.read = true;
        g_verify.got = false;
#if !defined(__VGX_VITA__)
        static const bool simOn = DiagnosticInt("GXV_PASSVERIFY_SIM", 1) != 0 && glBlitFramebuffer != nullptr &&
                                  glGetString(GL_VERSION) && std::strstr(reinterpret_cast<const char*>(glGetString(GL_VERSION)), "OpenGL ES 3");
        g_dsim.on = sim && simOn;
        g_dsim.prevEfb = true;
        g_dsim.prevStore = true;
        if (g_dsim.on) {
            g_dsim.poisons = g_dsim.loads = g_dsim.stores = 0;
            // (the depth memory starts as what the EFB holds now: the scene that is open goes on into this frame)
            DepthSimBlit(true);
        }
#else
        (void)sim;
#endif
        ExecuteFrame(buf);
#if !defined(__VGX_VITA__)
        g_dsim.on = false;
#endif
        out.px.swap(g_verify.pix);
        out.w = g_verify.w;
        out.h = g_verify.h;
        out.ok = g_verify.got;
        out.n = g_verify.counts;
        out.planes = g_verify.restorePlanes;
        g_verify = PassVerify{};
    };
    Pic a, c, b;
    run(true, true, false, a);
    run(true, true, false, c);
    run(false, false, true, b);
    struct Diff { uint64_t pixels = 0; int maxd = 0; double sse = 0; };
    auto diff = [](const Pic& x, const Pic& y) {
        Diff d;
        const size_t n = std::min(x.px.size(), y.px.size());
        for (size_t i = 0; i < n; i += 4) {
            bool any = false;
            for (int k = 0; k < 3; ++k) { // (colour: the EFB's alpha is not part of the picture)
                const int v = std::abs(static_cast<int>(x.px[i + k]) - static_cast<int>(y.px[i + k]));
                if (v) { any = true; d.maxd = std::max(d.maxd, v); d.sse += static_cast<double>(v) * v; }
            }
            d.pixels += any ? 1 : 0;
        }
        return d;
    };
    static uint64_t frames = 0, equal = 0, differ = 0, unstable = 0;
    ++frames;
    if (!a.ok || !b.ok || !c.ok || a.w != c.w || a.h != c.h || b.w != c.w || b.h != c.h) {
        std::fprintf(stderr, "[passverify] frame %llu: no picture to compare\n", static_cast<unsigned long long>(buf.frame));
        return;
    }
    const Diff ac = diff(a, c), bc = diff(b, c);
    const double n = static_cast<double>(c.w) * c.h * 3;
    const double psnr = bc.sse > 0 ? 10.0 * std::log10(255.0 * 255.0 / (bc.sse / n)) : 99.0;
    if (ac.pixels) ++unstable;
    else if (bc.pixels) ++differ;
    else ++equal;
    static unsigned lines = 0;
    if (lines++ < 400 || bc.pixels)
        std::fprintf(stderr, "[passverify] frame %llu (%ux%u, %" VGX_PRIZU " commands): reference runs %s (%llu px, max %d); configured run vs reference: %llu px differ, max %d, PSNR %.2f dB"
                             " | totals: %llu frames, %llu equal, %llu differ, %llu unstable%s\n",
                     static_cast<unsigned long long>(buf.frame), c.w, c.h, buf.cmds.size(), ac.pixels ? "DIFFER" : "agree", static_cast<unsigned long long>(ac.pixels), ac.maxd,
                     static_cast<unsigned long long>(bc.pixels), bc.maxd, psnr, static_cast<unsigned long long>(frames), static_cast<unsigned long long>(equal),
                     static_cast<unsigned long long>(differ), static_cast<unsigned long long>(unstable),
#if !defined(__VGX_VITA__)
                     g_dsim.poisons ? " (depth model on)" : ""
#else
                     ""
#endif
        );
    // What the configured run left out, in the same frame (EFB size x 4 bytes per depth load or store).
    if (lines <= 400)
        std::fprintf(stderr, "[passverify]   scenes %u -> %u (EFB %u -> %u), copy quads %u -> %u, depth loads %u -> %u, stores %u -> %u; efbSwaps %u (restores %u, planes %u), hoists %u; "
                             "depth model: %u loads, %u stores, %u scenes started with no depth; fx: %u runs, %u draws, %u vertices (%u candidate draws)\n",
                     c.n.scenes, b.n.scenes, c.n.efbScenes, b.n.efbScenes, c.n.copyQuads, b.n.copyQuads, c.n.efbDepthLoads, b.n.efbDepthLoads, c.n.efbDepthStores,
                     b.n.efbDepthStores, b.n.efbSwaps, b.n.efbRestores, b.planes, b.n.hoists,
#if !defined(__VGX_VITA__)
                     g_dsim.loads, g_dsim.stores, g_dsim.poisons,
#else
                     0u, 0u, 0u,
#endif
                     b.n.fxRuns, b.n.fxDraws, b.n.fxVerts, b.n.fxCandidates);
    // A frame that differs: which change does it? The configured run again with one of them off at a time.
    if (bc.pixels && !ac.pixels) {
        static const char* const names[6] = {"GXV_DEPTHPLAN", "GXV_EFBSWAP", "GXV_PASSDIRECT2", "GXV_PASSHOIST", "GXV_PASSHALF", "GXV_FXHALF"};
        std::string found;
        for (int k = 0; k < 6; ++k) {
            Pic x;
            g_gpOffMask = 1u << k;
            run(false, true, true, x);
            g_gpOffMask = 0;
            const Diff xc = diff(x, c);
            if (x.ok && xc.pixels < bc.pixels) {
                char t[96];
                std::snprintf(t, sizeof t, " %s=0 leaves %llu px", names[k], static_cast<unsigned long long>(xc.pixels));
                found += t;
            }
        }
        std::fprintf(stderr, "[passverify]   frame %llu differs; with one change off:%s\n", static_cast<unsigned long long>(buf.frame), found.empty() ? " no single change explains it" : found.c_str());
        // The same as the first configured run after reference runs, with one change only (a frame that differs only
        // right after reference runs depends on what the reference run left behind).
        found.clear();
        for (int k = 0; k < 4; ++k) {
            Pic r, x;
            run(true, true, false, r);
            g_gpOnlyMask = 1u << k;
            run(false, true, true, x);
            g_gpOnlyMask = 0;
            const Diff xc = diff(x, c);
            char t[96];
            std::snprintf(t, sizeof t, " only %s: %llu px", names[k], static_cast<unsigned long long>(xc.pixels));
            found += t;
        }
        std::fprintf(stderr, "[passverify]   after a reference run:%s\n", found.c_str());
        // The frame's passes and copies, for the search.
        static unsigned listed = 0;
        if (listed++ < 3) {
            uint32_t runDraws = 0;
            std::string texs;
            for (size_t i = 0; i < buf.cmds.size(); ++i) {
                const Cmd& k = buf.cmds[i];
                if (k.type == CMD_DRAW) {
                    const DrawRec& d = buf.draws[k.arg];
                    ++runDraws;
                    for (int m = 0; m < 8; ++m)
                        if (d.texMask & (1u << m)) {
                            char t[16];
                            std::snprintf(t, sizeof t, " %u", d.tex[m].texId);
                            if (texs.find(t) == std::string::npos && texs.size() < 200) texs += t;
                        }
                    continue;
                }
                if (runDraws) { std::fprintf(stderr, "[passverify]     %u draws, textures%s\n", runDraws, texs.c_str()); runDraws = 0; texs.clear(); }
                if (k.type == CMD_PASS_BEGIN) {
                    PassCmd ps = buf.passes[k.arg];
                    const bool late = LateDirect(buf, i, ps, 1.0f);
                    std::fprintf(stderr, "[passverify]     %" VGX_PRIZU " PASS (%d,%d %dx%d) direct %u atlas %u late-direct %d known %u planes %u rgba %.2f %.2f %.2f %.2f\n", i, ps.rect.x, ps.rect.y,
                                 ps.rect.w, ps.rect.h, buf.passes[k.arg].directTex, ps.atlasTex, late ? 1 : 0, ps.known, ps.planes, ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3]);
                } else if (k.type == CMD_COPY_TEX) {
                    const CopyTexCmd& t = buf.copyTex[k.arg];
                    std::fprintf(stderr, "[passverify]     %" VGX_PRIZU " COPY tex %u src (%d,%d %dx%d) dst %ux%u%s%s%s%s%s%s clear %d%d%d%d mat a-row %.2f %.2f %.2f %.2f add %.2f %.2f %.2f %.2f%s\n", i, t.texId, t.src.x,
                                 t.src.y, t.src.w, t.src.h, t.dstW, t.dstH, t.fromPass ? " fromPass" : "", t.passMore ? " more" : "", t.skip ? " skip" : "", t.fill ? " fill" : "",
                                 t.depth ? " depth" : "", t.srcTex ? " srcTex" : "", t.clear.enable, t.clear.color, t.clear.alpha, t.clear.depth, t.mat[3][0], t.mat[3][1], t.mat[3][2],
                                 t.mat[3][3], t.add[0], t.add[1], t.add[2], t.add[3], Hoisted(i) ? " HOISTED" : "");
                } else if (k.type == CMD_COPY_DISP) {
                    std::fprintf(stderr, "[passverify]     %" VGX_PRIZU " DISPLAY COPY clear %d\n", i, buf.copyDisp[k.arg].clear.enable);
                } else if (k.type == CMD_TEX_UPLOAD || k.type == CMD_TEX_FREE) {
                    std::fprintf(stderr, "[passverify]     %" VGX_PRIZU " %s %u\n", i, k.type == CMD_TEX_FREE ? "FREE" : "UPLOAD", k.type == CMD_TEX_FREE ? k.arg : buf.uploads[k.arg].texId);
                }
            }
            if (runDraws) std::fprintf(stderr, "[passverify]     %u draws, textures%s\n", runDraws, texs.c_str());
        }
    }
    // GXV_PASSVERIFY_DUMP=<folder>: the reference and configured pictures of frames that differ, as raw RGBA (w x h, top row first)
    if (bc.pixels) {
        static const char* const dir = DiagnosticEnv("GXV_PASSVERIFY_DUMP");
        static unsigned dumps = 0;
        if (dir && *dir && dumps < 8) {
            ++dumps;
            for (int k = 0; k < 2; ++k) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/f%llu_%s_%dx%d.rgba", dir, static_cast<unsigned long long>(buf.frame), k ? "configured" : "reference", c.w, c.h);
                if (FILE* f = std::fopen(path, "wb")) {
                    const Pic& p = k ? b : c;
                    std::fwrite(p.px.data(), 1, p.px.size(), f);
                    std::fclose(f);
                }
            }
        }
    }
}
// A frame that can be executed three times over: no shader work (compiles, the deferred warm-up) and no change of size.
static bool Verifiable(const CmdBuf& buf) {
    if (!buf.warm.empty() || buf.crowdActive) return false;
    bool disp = false;
    for (const Cmd& c : buf.cmds) {
        if (c.type == CMD_SHADER || c.type == CMD_PROGRAM || c.type == CMD_VARIANT || c.type == CMD_SET_TARGET) return false;
        if (c.type == CMD_COPY_DISP) disp = true;
    }
    return disp && buf.draws.size() >= 50;
}
void Execute(const CmdBuf& buf) {
    static const long every = DiagnosticInt("GXV_PASSVERIFY", 0);
    static const long from = DiagnosticInt("GXV_PASSVERIFY_FROM", 0); // (first frame to verify)
    static long counter = 0;
    if (every > 0 && !g_skipDraws && static_cast<long>(buf.frame) >= from && Verifiable(buf) && ++counter % every == 0) {
        VerifyFrame(buf);
        return;
    }
    ExecuteFrame(buf);
}
// END GPUPASS VERIFY

const std::vector<uint8_t>& LastDisplayCopy(int& width, int& height) {
    width = g_capW;
    height = g_capH;
    return g_captured;
}
void RequestCapture(bool enable) { g_capture = enable; }
void SetPresentHook(PresentHook hook) { g_presentHook = hook; }
#if VGX_COLDTRACE
static_assert(VGX_COLDTRACE_DETAIL_VERSION==3);
extern "C" void port_vgx_coldtrace_runtime_link_guard_v3() {}
#endif
bool SetPicture(const uint32_t* rgba, int width, int height) { return PicSet(rgba, width, height); }
bool DrawPicture() { return PicDraw(); }
void ReleasePicture() { PicRelease(); }
void SetProgressHostDraws(bool hostDraws) { g_hostDrawsProgress = hostDraws; }
void SetWarmBudget(double budgetMs) { g_warmBudgetMs.store(budgetMs, std::memory_order_relaxed); }
unsigned WarmPending() { return g_warmLeft.load(std::memory_order_relaxed); }
void WarmIdle() {
    if (g_warm.empty() || g_warmBudgetMs.load(std::memory_order_relaxed) <= 0.0) return;
    // (the state a frame starts from, see the top of Execute)
    S.attrConstSet = 0;
    S.layoutVbo = ~0u;
    BindFbo(g_efbFbo);
    RunWarm();
}
unsigned WarmTotal() { return g_warmAll.load(std::memory_order_relaxed); }
void WarmSliceStats(unsigned& slices, double& worstMs, double& totalMs) {
    slices = g_warmSlices.load();
    worstMs = g_warmWorstMs.load();
    totalMs = g_warmTotalMs.load();
}
void SetProgressDisplay(unsigned framebuffer, int width, int height, ProgressShowHook show) {
    g_dispFbo = framebuffer;
    g_dispW = width;
    g_dispH = height;
    g_showHook = show;
}
void SetProgressPolicy(uint64_t startupFrames, unsigned afterMs) {
    g_progStartupFrames = startupFrames;
    g_progAfterMs = afterMs;
}
void SetSkipDraws(bool skip) { g_skipDraws = skip; }
Stats GetStats() { return g_stats; }

} // namespace vgx::backend

#ifdef VGX_GL_STUB
namespace vgx::host {
bool CreateContext() { return true; }
}
#endif
