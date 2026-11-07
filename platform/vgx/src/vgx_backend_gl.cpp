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
#include "vgx_sanitize.h"
#include "vgx_coldtrace.h"
#include "vgx_autoq.h"
#include "vgx_async_shaders.h"
#include "vgx_copyvar.h"
#include "vgx_resource_audit.h"
#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif
#ifdef __VGX_VITA__
#include <psp2/gxm.h>
// GXV_MIDFLUSH: vitaGL's context, and the GXM state cache of its patch 0011 (weak: a library without them leaves these null)
extern "C" SceGxmContext* gxm_context __attribute__((weak));
extern "C" const SceGxmFragmentProgram* gxm_sc_fprog __attribute__((weak));
extern "C" const SceGxmVertexProgram* gxm_sc_vprog __attribute__((weak));
extern "C" uint32_t gxm_sc_ftex_valid __attribute__((weak));
// GXV_UNI_SHADOW: vitaGL's uniform shadow buffers (vgl.diff of agent be8; weak: a library without them keeps glUniform*)
// (GLboolean / GLuint / GLint spelled as the GL headers, which come later, define them)
extern "C" unsigned char vglGetUniformShadow(unsigned int prog, int location, void** vptr, void** fptr, uint32_t* bytes) __attribute__((weak));
extern "C" void vglUniformShadowWritten(unsigned int prog, uint32_t stages) __attribute__((weak));
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
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
#include <cstdarg>
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
extern "C" int port_skip_movie_playing(void) __attribute__((weak));  // (platform/src/port/cutscene_skip.cpp: a THP movie plays)
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
extern "C" void port_render_size_changed(void) __attribute__((weak)); // (platform/src/port/perf.cpp: PETARI_PAUSE_FREEZE copies again)
extern "C" int port_vgx_vita_splash_active(void) __attribute__((weak));
extern "C" int port_vgx_vita_fixed_cap_supported(void) __attribute__((weak)); // (vgx_host_galaxy_vita.cpp: the fixed 30 cap's driver path; GXV_AUTOQ_HOLD30)
#ifdef __VGX_VITA__
extern "C" unsigned vglGetFreeProgramSlots(void);
extern "C" void vglSetNextSceneDepthModes(GLboolean load, GLboolean store) __attribute__((weak));
extern "C" void vglUseLowPrecisionFragmentOnly(GLboolean val) __attribute__((weak)); // vitaGL patch 0012 (GXV_FS_HALF)
extern "C" GLint vglFramebufferDepthTexture(GLuint fb) __attribute__((weak)); // vitaGL built with DEPTH_SAMPLE (GXV_FXHALF): row width or 0
extern "C" void vgxvita_uploadAuthoredI4Mips(unsigned width, unsigned height, unsigned count, const void* const* levels) __attribute__((weak));
// tex11: the transfer-unit waits of texture uploads (vgx_host_galaxy_vita.cpp, GXV_VITA_UPLOAD_SYNC) and vitaGL patch 0015's chain reserve
// (vglReserveTextureLevels: the next level-0 allocation of that size holds the whole chain). Weak: a host or a library without them.
extern "C" int vgxvita_uploadSyncMode(void) __attribute__((weak));
extern "C" void vgxvita_xferFence(void) __attribute__((weak));
extern "C" void vgxvita_xferStats(uint64_t* waits, uint64_t* us) __attribute__((weak));
extern "C" void vglReserveTextureLevels(GLsizei width, GLsizei height, GLint levels) __attribute__((weak));
// GXV_ASYNC_SHADERS (vgx_async_shaders.h; vgx_host_galaxy_vita.cpp): the cache probe, the compile of a pair into the cache (vitaGL patch 0020), the
// worker thread's start and its priority boost. Weak: a host or a vitaGL without them leaves every shader on the blocking path.
extern "C" int vgxvita_asyncShaderInCache_v1(const char* joined, int fragment) __attribute__((weak));
extern "C" int vgxvita_asyncShaderCompile_v1(const char* vs, const char* fs, int fsHalf, int* compiled) __attribute__((weak));
extern "C" int vgxvita_asyncShaderStart_v1(void (*fn)(void*), void* arg, char* where, size_t whereSize) __attribute__((weak));
extern "C" void vgxvita_asyncShaderBoost_v1(int on) __attribute__((weak));
extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// The first few times something of the gpupass section happens: one line in the console's log (the back end's stderr does not reach it).
#define GPUPASS_NOTE(n, ...) do { static int gp_cnt_ = 0; if (gp_cnt_ < (n)) { ++gp_cnt_; port_log("[vgx] " __VA_ARGS__); } } while (0)
#else
#define GPUPASS_NOTE(n, ...) VGX_LOG_N(n, __VA_ARGS__)
#endif

extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak));  // the basic log, when the platform layer has one
// The stage the game is loading (platform/src/port/perf.cpp, set by GameSystemSceneController::initializeScene on its loading
// thread): its key while it loads and after, 0 before the first load; and 1 while the loading thread runs (GXV_STAGEWARM).
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));
extern "C" int port_scene_loading_active(void) __attribute__((weak));
// The front end's texture decode time so far, in microseconds (vgx_texcache.cpp; GXV_STUTTER_LOG). Weak: absent without the front end.
extern "C" uint64_t vgx_tex_decode_us(void) __attribute__((weak));

// The front end's list of character and key-item textures (vgx_state.h): weak, a build without the front end has none.
namespace vgx { bool IsCharaTexture(uint32_t texId) __attribute__((weak)); }

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
        c.dxt5 = true;  // (GXV_CMPRA: compressed where the GL has DXT5, decoded in Upload otherwise)
        c.tex16 = true; // (GXV_TEX16: glTexImage2D with the 16-bit types, internal format = format)
#ifdef __VGX_VITA__
        // Read once; off until the authored storage/LOD path has native validation.
        c.authoredI4Mips = EnvInt("GXV_AUTHORED_I4_MIPS", 0) != 0 && c.mipClamp && vgxvita_uploadAuthoredI4Mips;
#endif
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
    X(quadPads)       /* copy-program quads drawn over a viewport made even (odd size; GXV_RECT_LEGACY) */                            \
    X(quadIdentity) X(quadRgbA) X(quadConstant) X(quadGeneric) /* every copy-program quad by the form of its transform (vgx_copyvar.h): counted whether or not GXV_COPYVAR draws the dedicated program */ \
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
    X(efbLoadFroms)   /* of those: filled by the next scene's own colour load instead (GXV_VGL_LOADFROM, vitaGL patches/0019) */ \
    X(efbSwapsPartial) /* of those: GXV_EFBSWAP2's, copies with a conversion that the frame's samplers do not see */                  \
    X(rawConverts)    /* GXV_EFBSWAP2: such buffers converted into an ordinary copy after all, for a later frame's draw (RawConvert) */ \
    X(copiesMade)     /* texture copies executed (not dead, not hoisted): with a copy scene unless swapped */                         \
    X(wholeCopies) X(wholeRgb) X(wholeA) X(wholeOk) /* of those: of the whole EFB 1:1; keeping r,g,b only (RGB565); a only (A8); swapOk */ \
    X(wholePredNo) X(swapHolds) /* GXV_EFBSWAP2_PREDICT: whole copies left ordinary for an earlier reader; GXV_EFBSWAP2_HOLD: swaps refused */ \
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
    uint64_t drawSerial = 0;               // last frame a real draw (not a warm-up variant) used it; 0: only ever warmed
    uint32_t blendKey = ~0u, layout = ~0u; // of its last draw (see BlendKey)
    uint32_t layoutNeed = 0;               // vertex-layout bits (VL_*) of the attributes the vertex shader declares
    bool ok = false;
    bool warmCompatible = false; // last use had the EFB format and the warm draw's zero vertex base
    uint32_t recStage = 0, recLB = ~0u; // (GXV_STAGEWARM) stage and layout/blend of its last draw already in that stage's set
    uint8_t failures = 0;        // (GXV_GL_RETRY) failed links so far; the GL program of a failed link is deleted
    uint64_t retryAfter = 0;     // (GXV_GL_RETRY) frame from which a failed link may be tried again
    // (GXV_UNI_SHADOW) per uniform (US_*): where vitaGL keeps its values for each stage; ok = it is written there directly
    struct Shadow { float* v = nullptr; float* f = nullptr; bool ok = false; } sh[7];
    bool shadow = false; // (GXV_UNI_SHADOW) at least one of sh[] is ok
};
enum { US_PROJ, US_MV, US_TM, US_PTM, US_PAL, US_LT, US_TEV, US_COUNT };
struct Tex {
    GLuint id = 0;
    uint16_t w = 0, h = 0;
    bool mips = false;
    uint8_t wrapS = 0xFF, wrapT = 0xFF, minF = 0xFF, magF = 0xFF;
    bool renderTarget = false;
    uint16_t ew = 0, eh = 0; // a render target: its size in EFB pixels (w x h is that at the render scale); 0: unknown
    bool efbBuf = false; // GXV_EFBSWAP: `id` is a colour buffer the EFB had (it goes back to the EFB's spares, it is not deleted)
    int8_t lodBias = 0;    // GXV_RUNG_LODBIAS: the bias last set on it, in vitaGL steps of 1/8 level (0 = never set)
    uint32_t anisoGen = 0; // g_anisoGen when a trilinear minification filter was last set (GXV_AUTOQ_ANISO: set again after a change)
    uint32_t xferEpoch = 0; // (GXV_VITA_UPLOAD_SYNC=2) g_xferEpoch when its upload last left transfers pending
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

// GXV_SANITIZE (vgx_sanitize.h): checks on what reaches the GPU, read once, default 0 (off): nothing is read, counted or skipped.
sanitize::Counts g_san;
inline unsigned SanModeRead() {
    static const unsigned mode = sanitize::ParseMode(EnvInt("GXV_SANITIZE", 0));
    return mode;
}
// GXV_BE_FAST2=1 (default; 0: as before; read in Init): (1) the switches a draw asks about (GXV_SANITIZE two or three times a
// draw, GXV_UNIFORM_REUSE, GXV_STAGEWARM, GXV_RESOURCE_AUDIT and GXV_RUNG_LODBIAS once a texture) are read once in Init into
// plain variables instead of through function-local statics: with clang on ARMv7 every pass over such a static's guard is a
// load-acquire, i.e. a `dmb ish` that waits for the draw's outstanding stores (~8-12 a draw). (2) The uniform comparisons of a
// draw (Draw: need, ShadowBlock) use SameWords (NEON, 64 bytes a step) instead of newlib's memcmp (a word a step, two loads and
// a compare per 4 bytes, called for every candidate block: lights 176 floats, palette up to 384). Both exact: the same values,
// the same answers (bitwise equality, as memcmp == 0).
bool g_beFast2 = false;
struct BeFast2Cache {
    unsigned sanMode = 0;
    bool uniformReuse = true, stageWarm = true, audit = false;
    long lodPct = 0;
} g_f2;
inline unsigned SanMode() { return g_beFast2 ? g_f2.sanMode : SanModeRead(); }
inline bool UniformReuseRead() {
    static const bool on = EnvInt("GXV_UNIFORM_REUSE", 1) != 0;
    return on;
}
// Bitwise equality of n 32-bit words: exactly memcmp(a, b, 4 * n) == 0 (floats compared as bits: -0 / +0 and NaN payloads
// differ, as for memcmp). Both pointers 4-byte aligned (uniform arena floats, vitaGL's uniform buffers).
inline bool SameWords(const void* a, const void* b, size_t n) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint32_t* x = static_cast<const uint32_t*>(a);
    const uint32_t* y = static_cast<const uint32_t*>(b);
    if (n && *x != *y) return false; // (most changed blocks differ in their first word: a matrix row, a light colour)
    while (n >= 16) {
        uint32x4_t d = veorq_u32(vld1q_u32(x), vld1q_u32(y));
        d = vorrq_u32(d, veorq_u32(vld1q_u32(x + 4), vld1q_u32(y + 4)));
        d = vorrq_u32(d, veorq_u32(vld1q_u32(x + 8), vld1q_u32(y + 8)));
        d = vorrq_u32(d, veorq_u32(vld1q_u32(x + 12), vld1q_u32(y + 12)));
        const uint32x2_t h = vorr_u32(vget_low_u32(d), vget_high_u32(d));
        if (vget_lane_u32(vpmax_u32(h, h), 0) != 0) return false;
        x += 16;
        y += 16;
        n -= 16;
    }
    uint32_t acc = 0;
    while (n--) acc |= *x++ ^ *y++;
    return acc == 0;
#else
    return std::memcmp(a, b, n * 4) == 0;
#endif
}
inline bool SameUniforms(const void* a, const void* b, size_t words) { return g_beFast2 ? SameWords(a, b, words) : std::memcmp(a, b, words * 4) == 0; }
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
unsigned g_efbSwapsPartialTotal = 0, g_rawConvertsTotal = 0; // GXV_EFBSWAP2's, and the later conversions (tests)
unsigned g_efbSwapHoldsTotal = 0; // GXV_EFBSWAP2_HOLD: conversion swaps refused (tests: DebugEfbSwapHolds)
unsigned g_efbLoadFromsTotal = 0; // GXV_VGL_LOADFROM: swaps whose drawing back was the next scene's colour load (tests)
ColorLoadHook g_colorLoadHook = nullptr; // (GXV_VGL_LOADFROM: the host's vglColorLoadFrom)
unsigned g_efbAdoptsTotal = 0; // EFB buffers that became a texture's own storage (AdoptEfbBuffer; tests: DebugEfbAdopts)

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
uint64_t g_poolRecycled = 0; // pooled objects given up by RecycleFbo (status line)
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
// Floor protection by degrees (vgx_autoq.h, GXV_AUTOQ_FXSTEP): states the controller (or a test, DebugSetQualitySteps) sets
// between frames; AutoQualityFrame latches them at the start of each frame, so a frame is wholly one or the other.
//   g_stepFx  effects and blur passes at half size, as GXV_FXHALF=2 and GXV_PASSHALF=1 would draw them
//   g_stepBg  the textures of draws that sample no character or key-item texture use the nearest mip level between levels
//             (LINEAR_MIPMAP_LINEAR -> LINEAR_MIPMAP_NEAREST): half the texture fetches of a trilinear surface
// Off, every draw is the one the static switches (or nothing) give.
//   g_stepEdge the present without its edge smoothing (GXV_AUTOQ_EDGESTEP, Performance mode's first step): PresentEdgeAA declines, so
//             the host presents as it does with GXV_EDGE_AA off for that picture (the HUD and Mario's own smoothing as before)
bool g_stepFxWanted = false, g_stepBgWanted = false, g_stepEdgeWanted = false;
bool g_stepFx = false, g_stepBg = false, g_stepEdge = false;
float g_passScaleNext = 1.0f;   // ... for the pass BeginPass begins next (set by its caller, used once)
// Anisotropic filtering at the top rung (vgx_autoq.h, GXV_AUTOQ_ANISO): vitaGL's anisotropic switch (a vitaGL built with ANISO_SWITCH;
// the host hands its function over, SetAnisotropicHook) makes a trilinear texture's minification filter the anisotropic value when
// that filter is SET (glTexParameteri), not at every draw. So a change is made in two parts: the switch is called between frames, and
// g_anisoGen counts the changes; BindTexture sets the minification filter again, once, on the first bind after a change, of every
// texture that is trilinear. Never changed (g_anisoGen 0), no call is made that was not made before.
AnisoHook g_anisoHook = nullptr;
bool g_anisoWanted = false, g_aniso = false; // wanted (the controller, or a test: DebugSetAnisotropic) / latched at the start of a frame
unsigned g_anisoFilter = 2;                  // the value handed to the switch when on (GXV_AUTOQ_ANISO=1|2: 2, =3: 3)
uint32_t g_anisoGen = 0;
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif
// GXV_RUNG_LODBIAS (see BindTexture): the bias for the current rung, recomputed only when the rung's width changes.
long RungLodPctRead() {
    static const long pct = std::clamp<long>(EnvInt("GXV_RUNG_LODBIAS", 0), 0, 200);
    return pct;
}
int8_t RungLodBiasSteps() {
    const long pct = g_beFast2 ? g_f2.lodPct : RungLodPctRead(); // (GXV_BE_FAST2: no guard per texture)
    if (!pct) return 0;
    static float lastSX = -1.0f;
    static int8_t steps = 0;
    if (g_rungSX != lastSX) {
        lastSX = g_rungSX;
        const float levels = g_rungSX > 0.0f ? std::log2(1.5f / g_rungSX) : 0.0f; // (1.5 = 960 / 640, the top rung's width factor)
        steps = static_cast<int8_t>(std::clamp(-std::lround(levels * 8.0f * static_cast<float>(pct) / 100.0f), -32L, 0L));
    }
    return steps;
}

// ---- GXV_BE_PROF (the player build too; default 0): where the back end's own time goes, on the console
// GXV_PROF / GXV_PROF=3 below are compiled out of the player build (kBackendDiagnostics), and the workload probe only gives the
// whole Execute (r27: 8.3 ms a picture on the Observatory route, ~280 draws, 6-8 scenes). GXV_BE_PROF=<n> times one picture in n
// (1: every one) with the process clock at coarse points: the frame's set-up (render-scale controller, stream upload, plans,
// warm-up slice), every command by its type, and four stages inside a draw (program + render state; glUniform*; textures,
// vertex layout and target; the vitaGL draw call itself, i.e. vitaGL + sceGxm). Every 5 s one basic-log line "be-prof:" gives
// the per-picture averages, the counts that drive them (draws, program switches, blend re-patches, texture binds, uniform calls
// and bytes, scenes, copy quads) and, from a vitaGL with patch 0014, its fragment-program memo and uniform-ring counters. A clock
// read costs time itself: the measured cost is in the line ("clock N ns x M reads a picture"), to take off by hand.
// Off: one predictable branch per command and per draw stage.
extern "C" void vglGetDrawCacheStats(uint32_t* stats) __attribute__((weak)); // tools/vitagl/patches/0014: 8 counters, see there
extern "C" void vglSetDrawCaches(uint32_t flags) __attribute__((weak));       // (the same patch; VGL_DRAWCACHE_* bits)
namespace beprof {
enum Bucket : unsigned { kSetup, kDrawProg, kDrawUni, kDrawTex, kDrawCall, kDrawAfter, kCopy, kPass, kDisp, kUpload, kWarmCmd, kOther, kTail, kBuckets };
bool g_on = false;    // GXV_BE_PROF > 0 (read in Init)
unsigned g_every = 1; // one picture in g_every is timed
bool g_frame = false; // the picture being executed is timed
uint64_t g_us[kBuckets] = {};
uint64_t g_pictures = 0, g_seen = 0, g_reads = 0;
uint64_t g_draws = 0, g_progSwitches = 0, g_blendRepatch = 0, g_texBinds = 0, g_uniCalls = 0, g_uniBytes = 0, g_scenes = 0, g_copies = 0;
double g_windowStartMs = 0.0, g_clockNs = 0.0;
uint32_t g_vglBase[8] = {};
uint64_t g_mark = 0;
int64_t g_cpu0 = 0;
uint64_t g_wall0 = 0, g_cpuSum = 0, g_wallSum = 0;
inline uint64_t Now() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
// A stage mark: the time since the previous mark goes to `b`.
inline void Mark(Bucket b) {
    const uint64_t n = Now();
    g_us[b] += n - g_mark;
    g_mark = n;
    ++g_reads;
}
void Configure() {
    const long n = EnvInt("GXV_BE_PROF", 0);
    g_on = n > 0;
    g_every = n > 0 ? static_cast<unsigned>(n) : 1u;
    if (!g_on) return;
    // the clock's own cost: the best of five runs of 200 reads
    double best = 1e9;
    volatile uint64_t sink = 0;
    for (int k = 0; k < 5; ++k) {
        const uint64_t a = Now();
        for (int i = 0; i < 200; ++i) sink = sink + Now();
        const uint64_t b = Now();
        best = std::min(best, static_cast<double>(b - a) * 1000.0 / 200.0);
    }
    g_clockNs = best;
    if (vglGetDrawCacheStats) vglGetDrawCacheStats(g_vglBase);
}
inline void FrameBegin() {
    g_frame = g_on && (g_seen++ % g_every) == 0;
    if (!g_frame) return;
    g_cpu0 = port_thread_cpu_us ? port_thread_cpu_us() : 0;
    g_wall0 = g_mark = Now();
}
void Report(double nowMs) {
    const double n = g_pictures ? static_cast<double>(g_pictures) : 1.0;
    auto ms = [&](uint64_t us) { return static_cast<double>(us) / 1000.0 / n; };
    const uint64_t drawUs = g_us[kDrawProg] + g_us[kDrawUni] + g_us[kDrawTex] + g_us[kDrawCall] + g_us[kDrawAfter];
    char line[1024];
    std::snprintf(line, sizeof line,
                  "be-prof: %.1f s, %llu pictures timed (1 in %u): a picture %.2f ms wall, %.2f ms CPU: set-up %.2f, draws %.2f (program+state %.2f, "
                  "uniforms %.2f, textures+layout+target %.2f, vitaGL call %.2f, after %.2f; %.1f us a draw), copies %.2f, passes %.2f, "
                  "display copy+present %.2f, uploads %.2f, shader cmds %.2f, other %.2f, end %.2f; a picture: %.0f draws, %.0f program switches, "
                  "%.1f blend re-patches, %.0f texture binds, %.0f glUniform (%.1f KB), %.1f scenes, %.1f copy quads; clock %.0f ns x %.0f reads",
                  (nowMs - g_windowStartMs) / 1000.0, static_cast<unsigned long long>(g_pictures), g_every, ms(g_wallSum), ms(g_cpuSum), ms(g_us[kSetup]),
                  ms(drawUs), ms(g_us[kDrawProg]), ms(g_us[kDrawUni]), ms(g_us[kDrawTex]), ms(g_us[kDrawCall]), ms(g_us[kDrawAfter]),
                  g_draws ? static_cast<double>(drawUs) / static_cast<double>(g_draws) : 0.0, ms(g_us[kCopy]), ms(g_us[kPass]), ms(g_us[kDisp]),
                  ms(g_us[kUpload]), ms(g_us[kWarmCmd]), ms(g_us[kOther]), ms(g_us[kTail]), g_draws / n, g_progSwitches / n, g_blendRepatch / n,
                  g_texBinds / n, g_uniCalls / n, g_uniBytes / n / 1024.0, g_scenes / n, g_copies / n, g_clockNs, g_reads / n);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "%s\n", line);
    if (vglGetDrawCacheStats) {
        // (vitaGL counts every draw, timed or not: these are the whole window's)
        uint32_t v[8] = {}, d[8] = {};
        vglGetDrawCacheStats(v);
        for (int i = 0; i < 8; ++i) d[i] = v[i] - g_vglBase[i];
        std::memcpy(g_vglBase, v, sizeof v);
        std::snprintf(line, sizeof line,
                      "be-prof vitaGL (patch 0014, every picture of the window): fragment programs patched %u, memo hits %u; vertex uniform ring "
                      "copies %u (%u KB), re-used %u; fragment copies %u (%u KB), re-used %u",
                      d[0], d[1], d[2], d[6] / 1024u, d[3], d[4], d[7] / 1024u, d[5]);
        if (port_log_summary) port_log_summary("%s", line);
        else std::fprintf(stderr, "%s\n", line);
    }
    for (uint64_t& v : g_us) v = 0;
    g_pictures = g_reads = g_draws = g_progSwitches = g_blendRepatch = g_texBinds = g_uniCalls = g_uniBytes = g_scenes = g_copies = 0;
    g_cpuSum = g_wallSum = 0;
    g_windowStartMs = nowMs;
}
inline void FrameEnd(unsigned scenes, unsigned copyQuads) {
    if (!g_frame) return;
    Mark(kTail);
    ++g_pictures;
    g_scenes += scenes;
    g_copies += copyQuads;
    g_wallSum += g_mark - g_wall0;
    if (port_thread_cpu_us) g_cpuSum += static_cast<uint64_t>(port_thread_cpu_us() - g_cpu0);
    g_frame = false;
    const double now = static_cast<double>(g_mark) / 1000.0;
    if (g_windowStartMs == 0.0) g_windowStartMs = now;
    if (now - g_windowStartMs >= 5000.0) Report(now);
}
} // namespace beprof
#define BEPROF_MARK(b) do { if (beprof::g_frame) beprof::Mark(beprof::b); } while (0)

// ---- GXV_GPU_SCENES (the player build too; default 0): where the GPU's time goes, scene by scene, on the console
// The GPU clock of automatic quality (vitaGL's GPU timers) gives one number a picture: from each scene's sceGxmEndScene to its fragment
// completion. A tile-based GPU does a scene in two phases: the vertex phase (vertex shaders, clipping, binning into the tiles: "TA")
// starts when the scene is handed over (sceGxmEndScene, or a mid-scene flush) and must finish before that scene's pixel phase ("3D":
// hidden-surface removal, fragment shaders, blending, the tiles' load and store) can start. With a vitaGL whose GPU timers can also
// stamp each scene's VERTEX completion (tools/vitagl/gpu_timers.py: vglRtSetupScenes / vglRtTagScene / vglRtReadScenes), each scene's
// time splits into
//   TA = vertex done - max(submitted, the last vertex completion seen before it)
//   3D = fragment done - max(vertex done, submitted, the last fragment completion seen before it)
// (the two phases of different scenes overlap on the GPU, so TA + 3D may exceed the busy time). The back end tags every scene it begins
// (TouchTarget) with what it drew there: kind (EFB / texture or pass target / display or other), draws, indices, indices of lit draws and
// "light-weighted" indices (indices x the light slots their vertex shader evaluates), blended draws and draws whose fragment shader can
// discard. Every 5 s one basic-log line "gpu-scenes:" gives, a picture: TA, 3D and busy, and per kind the CPU-side counts with their TA,
// 3D and the CPU time from the scene's beginning to its sceGxmEndScene ("feed"); also the biggest EFB scene of each picture alone.
// Resolution-independent GPU time that sits in TA points at vertex work; 3D that does not shrink with the picture size points at per-tile
// state, and a display scene with a long 3D at waits for the display. Without such a vitaGL the line still gives the CPU-side counts.
// Scenes the timers could not follow ("untimed": vitaGL's watcher was more than 48 scenes behind, gpu_timers.py RT_SLOTS - 16, which
// happens when the GPU runs about three pictures behind with 17 scenes a picture) leave a hole: the GPU work of the untimed scenes is
// done between the last completion seen and the next timed scene's completion. That span is counted once (busy, and the picture's TA
// and 3D), never from the next scene's submission (which counted the whole queue as work: per-picture sums above the picture interval),
// and it is not given to that next scene's kind: the per-kind and per-scene figures hold only scenes whose previous scene was timed.
//   GXV_GPU_SCENES=2 also every 5 s: the eight scene slots (a scene's place in the picture: its number, kind and target size) with the
// most 3D a picture, each with its draws by class (opaque, blended, discard, depth-only, no effect), the pixels they cover (measured from
// the vertices of stream draws; bounded by viewport and scissor for static ones), vitaGL's scissor-mask passes, clears and copy quads,
// whether the scene loaded and stored its depth and whether its colour load was dead (the scene began with a clear of its whole
// colour), and the fragment shaders with the largest estimated cost (pixels x a per-pixel weight from the shader's text). See the
// second part of this section, after DrawAreaPx. Mode 2 costs some back-end CPU (each stream draw's triangles projected, at most 64).
// Off: one predictable branch per scene and per draw.
struct VgxRtGpuScene { // = vglRtGpuScene (tools/vitagl/gpu_timers.py)
    uint32_t seq, frame, tag, begin_us, submit_us, vdone_us, fdone_us;
    uint8_t timed, vtimed, pad0, pad1;
};
extern "C" uint32_t vglRtSetupScenes(int priority, int affinity) __attribute__((weak));
extern "C" void vglRtTagScene(uint32_t tag) __attribute__((weak));
extern "C" uint32_t vglRtReadScenes(VgxRtGpuScene* out, uint32_t max) __attribute__((weak));
namespace gpuscenes {
enum Kind : uint8_t { kEfb, kTex, kDisp, kKinds };
struct Desc {
    uint32_t serial = 0;
    uint8_t kind = kDisp;
    int8_t dLoad = -1, dStore = -1;   // (mode 2) the depth hint the scene began with; -1: not known
    uint16_t ordinal = 0, w = 0, h = 0; // (mode 2) the scene's number in the back end's picture, its target's size
    uint32_t draws = 0, idx = 0, litIdx = 0, lightIdx = 0, blend = 0, discard = 0;
};
// ---- GXV_GPU_SCENES=2: what each scene drew (filled by the hooks of the second part; one per scene, a ring like Desc's)
enum DrawClass : uint8_t { kOpaque, kBlended, kDiscard, kDepthOnly, kNoEffect, kClasses };
struct ShaderUse { uint16_t fs = 0; uint32_t draws = 0; float px = 0.0f, cost = 0.0f; };
constexpr int kSceneShaders = 12;  // fragment shaders kept apart per scene (the rest are summed as "others")
struct Detail {
    uint32_t serial = 0;
    uint16_t clsDraws[kClasses] = {}, clsBound[kClasses] = {}; // draws of the class; of them, those with a rectangle bound only
    float clsPx[kClasses] = {}, clsBoundPx[kClasses] = {};      // target pixels: measured from the vertices / bounded by rectangles
    ShaderUse sh[kSceneShaders];
    uint8_t nSh = 0;
    uint16_t otherDraws = 0;
    float otherPx = 0.0f, otherCost = 0.0f;
    uint16_t masks = 0, maskFull = 0; // vitaGL's scissor-mask updates (tests.c update_scissor_test) and full-target passes among them
    float maskPx = 0.0f;              // pixels those passes cover (a full-target "clear the mask" pass and the rectangle / target)
    uint16_t clears = 0, quads = 0;   // glClear quads; copy / pass quads (DrawQuad)
    float clearPx = 0.0f, quadPx = 0.0f;
    uint8_t firstClear = 0;           // the scene's first operation cleared its whole target: 1 colour, 2 depth
    bool touched = false;             // something was drawn or cleared in it already
};
constexpr uint32_t kDetailRing = 256; // (the reader is a few pictures behind: at most ~70 scenes)
Detail g_detail[kDetailRing];
struct Cpu { uint64_t scenes = 0, draws = 0, idx = 0, litIdx = 0, lightIdx = 0, blend = 0, discard = 0; };
struct Gpu { uint64_t scenes = 0, taUs = 0, fUs = 0, feedUs = 0; };
bool g_on = false, g_started = false, g_timers = false;
int g_mode = 0; // GXV_GPU_SCENES: 1 the line; 2 the line and the top scenes
uint32_t g_serial = 0;
uint16_t g_ordinal = 0; // scenes begun in the picture being executed
constexpr uint32_t kRing = 1024;
Desc g_ring[kRing];
Desc* g_cur = nullptr;
std::vector<int8_t> g_fsDiscard; // per fragment shader id: -1 not known yet, 0 / 1 its text has no / a `discard`
Cpu g_cpu[kKinds];
Gpu g_gpu[kKinds], g_main;
uint64_t g_cpuPictures = 0, g_gpuPictures = 0, g_busyUs = 0, g_untimed = 0, g_early = 0, g_untagged = 0;
uint64_t g_mainIdx = 0, g_mainDraws = 0;
// scenes timed after an untimed one (their TA / 3D include the untimed scenes' remaining work: counted in the picture's totals only)
uint64_t g_gapScenes = 0, g_gapTaUs = 0, g_gapFUs = 0;
double g_windowStartMs = 0.0;
// record continuity (the previous record the reader saw)
bool g_havePrev = false;
uint32_t g_prevSeq = 0;
bool g_prevVt = false, g_prevFt = false;
// the last completions seen (of any earlier scene: the GPU does the scenes' phases in order)
bool g_haveV = false, g_haveF = false;
uint32_t g_lastV = 0, g_lastF = 0;
// the picture being gathered: its biggest EFB scene
uint32_t g_frame = ~0u;
bool g_frameOpen = false;
struct Best { uint32_t draws = 0, idx = 0, ta = 0, f = 0, feed = 0; bool timed = false; } g_best;
inline uint32_t Since(uint32_t later, uint32_t earlier) { return static_cast<int32_t>(later - earlier) > 0 ? later - earlier : 0u; }
inline uint32_t Later(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0 ? a : b; }

void TakeSlot(const Desc& d, uint32_t ta, uint32_t f, uint32_t feed); // (mode 2, second part)
void Close() {
    if (!g_cur) return;
    if (g_mode >= 2 && !g_timers) TakeSlot(*g_cur, 0, 0, 0); // (no scene timers: the slots get the CPU-side record of every scene)
    Cpu& c = g_cpu[g_cur->kind];
    ++c.scenes;
    c.draws += g_cur->draws; c.idx += g_cur->idx; c.litIdx += g_cur->litIdx; c.lightIdx += g_cur->lightIdx;
    c.blend += g_cur->blend; c.discard += g_cur->discard;
    g_cur = nullptr;
}
void BeginDetail(Desc& d); // (mode 2, second part: the target's size, a clean Detail)
// TouchTarget: a scene begins on the bound target.
void Begin(uint8_t kind) {
    Close();
    if (++g_serial == 0) ++g_serial; // (tag 0: a scene the back end did not begin)
    Desc& d = g_ring[g_serial % kRing];
    d = Desc{};
    d.serial = g_serial;
    d.kind = kind;
    d.ordinal = g_ordinal < 0xFFFF ? g_ordinal++ : g_ordinal;
    g_cur = &d;
    if (g_mode >= 2) BeginDetail(d);
    if (g_timers) vglRtTagScene(g_serial);
}
// GXV_VSOUT census (vertex shaders that leave out what their fragment shader does not read; ShaderSource::outPruned): per window
uint64_t g_voDraws = 0, g_voPruned = 0, g_voIdx = 0, g_voColGone = 0, g_voPartOff = 0, g_voTexOff = 0;
// GXV_FS_TRIM / GXV_VS_TRIM census (ShaderSource::trimOps): draws with a trimmed shader, and the operations left out, summed over draws
uint64_t g_ftDraws = 0, g_ftOps = 0, g_vtDraws = 0, g_vtOps = 0, g_vtIdx = 0;
inline bool FsDiscards(uint16_t fsId) {
    if (fsId >= g_fsDiscard.size()) g_fsDiscard.resize(static_cast<size_t>(fsId) + 64, -1);
    int8_t& dis = g_fsDiscard[fsId];
    if (dis < 0) dis = (fsId < g_fsSrc.size() && g_fsSrc[fsId] && g_fsSrc[fsId]->text.find("discard") != std::string::npos) ? 1 : 0;
    return dis != 0;
}
inline void NoteDraw(const DrawRec& r, const ShaderSource* vs) {
    Desc& d = *g_cur;
    ++d.draws;
    d.idx += r.idxCount;
    ++g_voDraws;
    if (vs && vs->trimOps) { ++g_vtDraws; g_vtOps += vs->trimOps; g_vtIdx += r.idxCount; }
    if (r.fsId < g_fsSrc.size() && g_fsSrc[r.fsId] && g_fsSrc[r.fsId]->trimOps) { ++g_ftDraws; g_ftOps += g_fsSrc[r.fsId]->trimOps; }
    if (vs && vs->outPruned) {
        ++g_voPruned;
        g_voIdx += r.idxCount;
        g_voColGone += (vs->outPruned & 1u) + ((vs->outPruned >> 1) & 1u);
        g_voPartOff += (vs->outPruned >> 2) & 1u;
        g_voTexOff += (vs->outPruned >> 3) & 1u;
    }
    if (vs && vs->usesLights) {
        d.litIdx += r.idxCount;
        d.lightIdx += r.idxCount * static_cast<uint32_t>(vs->nLt > 4 ? (vs->nLt - 4) / 5 : 0);
    }
    if (r.rs.blend) ++d.blend;
    d.discard += FsDiscards(r.fsId) ? 1u : 0u;
}
void EndPicture() {
    if (!g_frameOpen) return;
    g_frameOpen = false;
    ++g_gpuPictures;
    if (g_best.draws) {
        g_mainDraws += g_best.draws;
        g_mainIdx += g_best.idx;
        if (g_best.timed) { ++g_main.scenes; g_main.taUs += g_best.ta; g_main.fUs += g_best.f; g_main.feedUs += g_best.feed; }
    }
    g_best = Best{};
}
void Take(const VgxRtGpuScene& s) {
    if (s.frame != g_frame) { EndPicture(); g_frame = s.frame; g_frameOpen = true; }
    const Desc* d = s.tag && g_ring[s.tag % kRing].serial == s.tag ? &g_ring[s.tag % kRing] : nullptr;
    if (!d) ++g_untagged;
    const uint8_t kind = d ? d->kind : static_cast<uint8_t>(kDisp);
    // (clean: the scene before it, in the GPU's order, was read and had both completions - or it is the first record read; then this
    // scene's phases start at the later of its submission and that scene's completions, and its TA / 3D are its own)
    const bool clean = !g_havePrev || (s.seq == g_prevSeq + 1 && g_prevFt && g_prevVt);
    if (s.timed) {
        // (from the last fragment completion seen, also across untimed scenes: their work lies in this span, counted once)
        const uint32_t start = g_haveF ? Later(g_lastF, s.submit_us) : s.submit_us;
        g_busyUs += Since(s.fdone_us, start);
    }
    if (s.timed && s.vtimed) {
        const uint32_t ta0 = g_haveV ? Later(g_lastV, s.submit_us) : s.submit_us;
        const uint32_t ta = Since(s.vdone_us, ta0);
        if (static_cast<int32_t>(s.vdone_us - s.submit_us) <= 0) ++g_early; // (its vertex work was done before sceGxmEndScene returned)
        uint32_t f0 = Later(s.vdone_us, s.submit_us);
        if (g_haveF) f0 = Later(g_lastF, f0);
        const uint32_t f = Since(s.fdone_us, f0);
        const uint32_t feed = s.begin_us ? Since(s.submit_us, s.begin_us) : 0u;
        if (clean) {
            Gpu& g = g_gpu[kind];
            ++g.scenes; g.taUs += ta; g.fUs += f; g.feedUs += feed;
            if (d && kind == kEfb && d->draws > g_best.draws) g_best = Best{d->draws, d->idx, ta, f, feed, true};
            if (g_mode >= 2) TakeSlot(d ? *d : Desc{}, ta, f, feed);
        } else {
            ++g_gapScenes; g_gapTaUs += ta; g_gapFUs += f;
            if (d && kind == kEfb && d->draws > g_best.draws) g_best = Best{d->draws, d->idx, 0, 0, 0, false};
        }
    } else {
        ++g_untimed;
        if (d && kind == kEfb && d->draws > g_best.draws) g_best = Best{d->draws, d->idx, 0, 0, 0, false};
    }
    g_havePrev = true;
    g_prevSeq = s.seq;
    g_prevVt = s.timed && s.vtimed;
    g_prevFt = s.timed != 0;
    if (s.timed) { g_lastF = g_haveF ? Later(s.fdone_us, g_lastF) : s.fdone_us; g_haveF = true; }
    if (s.timed && s.vtimed) { g_lastV = g_haveV ? Later(s.vdone_us, g_lastV) : s.vdone_us; g_haveV = true; }
}
void ReportTop(double seconds); // (mode 2, second part)
void Report(double nowMs) {
    static const char* const kName[kKinds] = {"EFB", "texture/pass", "display/other"};
    const double cp = g_cpuPictures ? static_cast<double>(g_cpuPictures) : 1.0, gp = g_gpuPictures ? static_cast<double>(g_gpuPictures) : 1.0;
    char line[2048];
    int n = std::snprintf(line, sizeof line, "gpu-scenes: %.1f s, %llu pictures", (nowMs - g_windowStartMs) / 1000.0, static_cast<unsigned long long>(g_cpuPictures));
    // (the picture's totals: every kind's clean scenes and the spans after untimed scenes)
    uint64_t ta = g_gapTaUs, f = g_gapFUs;
    for (const Gpu& g : g_gpu) { ta += g.taUs; f += g.fUs; }
    if (g_timers)
        n += std::snprintf(line + n, sizeof line - n, "; GPU (%llu pictures timed, %llu scenes untimed, %llu untagged, %llu with the vertex work done before EndScene): "
                           "a picture vertex (TA) %.2f ms, pixel (3D) %.2f ms, busy %.2f ms",
                           static_cast<unsigned long long>(g_gpuPictures), static_cast<unsigned long long>(g_untimed), static_cast<unsigned long long>(g_untagged),
                           static_cast<unsigned long long>(g_early), ta / 1000.0 / gp, f / 1000.0 / gp, g_busyUs / 1000.0 / gp);
    else
        n += std::snprintf(line + n, sizeof line - n, "; GPU times: none (a vitaGL without vglRtReadScenes, or its GPU timers are off)");
    for (int k = 0; k < kKinds && n > 0 && n < static_cast<int>(sizeof line); ++k) {
        const Cpu& c = g_cpu[k];
        const Gpu& g = g_gpu[k];
        n += std::snprintf(line + n, sizeof line - n, "; %s: %.1f scenes, %.0f draws, %.1fk indices (lit %.1fk, light-weighted %.1fk), %.0f blended, %.0f discard",
                           kName[k], c.scenes / cp, c.draws / cp, c.idx / cp / 1000.0, c.litIdx / cp / 1000.0, c.lightIdx / cp / 1000.0, c.blend / cp, c.discard / cp);
        if (g_timers && n > 0 && n < static_cast<int>(sizeof line))
            n += std::snprintf(line + n, sizeof line - n, ", TA %.2f, 3D %.2f, feed %.2f ms", g.taUs / 1000.0 / gp, g.fUs / 1000.0 / gp, g.feedUs / 1000.0 / gp);
    }
    if (g_timers && n > 0 && n < static_cast<int>(sizeof line)) {
        const double m = g_main.scenes ? static_cast<double>(g_main.scenes) : 1.0;
        n += std::snprintf(line + n, sizeof line - n, "; biggest EFB scene: %.0f draws, %.1fk indices, TA %.2f, 3D %.2f, feed %.2f ms", g_mainDraws / gp, g_mainIdx / gp / 1000.0,
                      g_main.taUs / 1000.0 / m, g_main.fUs / 1000.0 / m, g_main.feedUs / 1000.0 / m);
    }
    if (VsOutMode() && n > 0 && n < static_cast<int>(sizeof line)) {
        const double dr = g_voDraws ? static_cast<double>(g_voDraws) : 1.0;
        n += std::snprintf(line + n, sizeof line - n, "; GXV_VSOUT: %.0f of %.0f draws a picture with a pruned vertex shader (%.1fk indices; colour varyings left out %.0f, "
                      "a colour or alpha part not computed %.0f, a texgen constant %.0f), %.0f%% of draws", g_voPruned / cp, g_voDraws / cp, g_voIdx / cp / 1000.0,
                      g_voColGone / cp, g_voPartOff / cp, g_voTexOff / cp, 100.0 * g_voPruned / dr);
    }
    if ((FsTrimMode() || VsTrimMode()) && n > 0 && n < static_cast<int>(sizeof line)) {
        const double dr = g_voDraws ? static_cast<double>(g_voDraws) : 1.0;
        n += std::snprintf(line + n, sizeof line - n, "; GXV_FS_TRIM: %.0f of %.0f draws a picture with a trimmed fragment shader (%.0f%%, %.1f clamp sides / floors left out "
                           "a draw); GXV_VS_TRIM: %.0f draws (%.1fk indices, %.0f%%) with a trimmed vertex shader (%.1f light terms / clamp sides left out a draw)",
                           g_ftDraws / cp, g_voDraws / cp, 100.0 * g_ftDraws / dr, g_ftDraws ? static_cast<double>(g_ftOps) / g_ftDraws : 0.0, g_vtDraws / cp,
                           g_vtIdx / cp / 1000.0, 100.0 * g_vtDraws / dr, g_vtDraws ? static_cast<double>(g_vtOps) / g_vtDraws : 0.0);
    }
    g_ftDraws = g_ftOps = g_vtDraws = g_vtOps = g_vtIdx = 0;
    g_voDraws = g_voPruned = g_voIdx = g_voColGone = g_voPartOff = g_voTexOff = 0;
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "%s\n", line);
    if (g_mode >= 2) ReportTop((nowMs - g_windowStartMs) / 1000.0);
    for (Cpu& c : g_cpu) c = Cpu{};
    for (Gpu& g : g_gpu) g = Gpu{};
    g_main = Gpu{};
    g_cpuPictures = g_gpuPictures = g_busyUs = g_untimed = g_early = g_untagged = g_mainIdx = g_mainDraws = 0;
    g_gapScenes = g_gapTaUs = g_gapFUs = 0;
    g_windowStartMs = nowMs;
}
// Execute's start (the render thread, the one that ends vitaGL's scenes): the vertex watcher is started once, as soon as automatic
// quality's GPU clock has started vitaGL's timers (tried for the first 600 pictures; GXV_AUTOQ_LEVEL or GXV_AUTOQ=0 start no clock).
void FrameBegin() {
    if (!g_on || g_started) return;
    static unsigned tries = 0;
    if (vglRtSetupScenes && vglRtTagScene && vglRtReadScenes) g_timers = vglRtSetupScenes(0x60, 0x20000) != 0;
    if (!g_timers && ++tries < 600 && vglRtSetupScenes) return;
    g_started = true;
    if (port_log_summary)
        port_log_summary("gpu-scenes: GXV_GPU_SCENES=%d: %s%s", g_mode,
                         g_timers ? "vertex and fragment completion of every scene (vitaGL scene timers, watchers on core 1)"
                                  : "CPU-side counts only (this vitaGL has no scene timers, or its GPU timers are off)",
                         g_mode >= 2 ? "; every 5 s also the top scene slots by 3D (gpu-scenes top: lines)" : "");
}
// Execute's end: the picture's last scene is closed, the finished scenes' times are taken, every 5 s the line.
void FrameEnd() {
    if (!g_on) return;
    Close();
    g_ordinal = 0;
    ++g_cpuPictures;
    if (g_timers) {
        VgxRtGpuScene recs[64];
        for (int rounds = 0; rounds < 4; ++rounds) {
            const uint32_t got = vglRtReadScenes(recs, 64);
            for (uint32_t i = 0; i < got; ++i) Take(recs[i]);
            if (got < 64) break;
        }
    }
    const double now = static_cast<double>(beprof::Now()) / 1000.0;
    if (g_windowStartMs == 0.0) g_windowStartMs = now;
    if (now - g_windowStartMs >= 5000.0) Report(now);
}
// (mode 2, the second part, after DrawAreaPx: the hooks the back end calls)
void NoteMask(bool began);
void NoteClear(bool colour, bool depth);
void NoteQuad();
void NoteDrawDetail(const CmdBuf& buf, const DrawRec& r);
} // namespace gpuscenes

// ---- GXV_MIDFLUSH=<n> (default 0, off; the console only): sceGxmMidSceneFlush after every n draws of an EFB scene
// sceGxm starts a scene's vertex phase only when the scene is ended (or a ring buffer is full); the back end feeds the Observatory's
// main EFB scene for ~15 ms of CPU, so its vertex phase (all its vertex shading and binning) then runs only after the last draw,
// in series with that scene's pixel phase. A mid-scene flush hands the GPU the vertex work recorded so far: the vertex phase runs
// while the CPU still records, and only the rest is left after sceGxmEndScene. Nothing is drawn differently: the pixel phase still
// begins at the scene's end. The default uniform buffers are kept (SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS); to be safe the
// GXM state cache (vitaGL patch 0011) forgets what the context holds and the next draw re-issues its program, so vitaGL points sceGxm
// at its uniform buffers again (with GXV_VGL_UNIREUSE without copying). A failure is logged once and turns the switch off.
// NOTE: vertex work done before sceGxmEndScene is not in the GPU timers' busy time (gpu_timers.py), so with this switch the "GPU" ms
// of the floor/autoq lines falls by the part that moved before the scene's end: compare frame rate and "ready after" (present: line).
// GXV_GPU_SCENES=1 shows how many scenes had all their vertex work done before EndScene.
uint32_t g_midFlushEvery = 0, g_midFlushDraws = 0, g_midFlushes = 0;
// ---- GXV_GPU_ATTRIB=<n> (default 0, off; a TEST switch: the picture is wrong while it is on): where the GPU time of the EFB goes.
// Every draw keeps its program, uniforms, textures, render state, vertex layout and its own sceGxmDraw (the per-draw costs); what it
// is asked to draw is cut:
//   1  each EFB draw draws only its first triangle: GPU ms = per-draw costs + scene costs (no vertex or pixel work to speak of)
//   2  each EFB draw is drawn into a 1x1 viewport: every vertex is shaded and every triangle set up as before, but almost none
//      covers a pixel: GPU ms = per-draw + vertex (TA) costs, pixel work removed
// With the normal picture (0): full - 2 = pixel work, 2 - 1 = vertex work beyond the per-draw cost, 1 = per-draw + scene costs.
// Read the floor: line's GPU ms (and gpu-scenes: TA / 3D) at the same spot as a run with 0. Copies, passes and the present are untouched.
int g_gpuAttrib = 0;
bool g_beFast = true; // GXV_BE_FAST (read in Init; see DirectFor)
#ifdef __VGX_VITA__
// GXV_UNI_SHADOW=1 (default 0 until a console A/B; read in Init; needs the vitaGL of agent be8's vgl.diff, otherwise off): a
// draw's uniform blocks go into vitaGL's per-program uniform buffers directly (vglGetUniformShadow at link), compared with what
// vitaGL holds and written only where they differ, plus one vglUniformShadowWritten for the dirty flags, instead of a
// glUniform4fv / glUniformMatrix4fv per changed block (~1200 calls a picture in the lit Observatory). See Draw.
bool g_uniShadow = false;
#endif

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
    int8_t blendAlphaZero = -1;
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
// GXV_SCENELOG_MIN=<n> (desktop diagnostic): the scene lines of every frame are kept, and printed after a frame that had at least n
// scenes (the water-surface crossings have 19 to 21 where an ordinary frame in the same place has 16).
bool g_sceneLogKeep = false;
std::string g_sceneLogBuf;
void LogScene() {
    int w = g_rtW, h = g_rtH;
    const char* what = "EFB";
    if (S.fbo != g_efbFbo) {
        what = "?";
        for (const PoolFbo& f : g_pool)
            if (f.fbo == S.fbo) { w = f.w; h = f.h; what = f.attached ? "texture" : "pass buffer"; }
    }
    char line[200];
    std::snprintf(line, sizeof line, "[scene] %2u: %s %dx%d (after %u draws, %u clears, %u copy quads)%s\n", C.scenes, what, w, h, C.draws, C.clears, C.copyQuads,
                  (S.fbo == g_inUseFbo) ? " same object, re-attached" : "");
    if (g_sceneLogKeep) g_sceneLogBuf += line;
    else std::fputs(line, stderr);
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
    if (gpuscenes::g_cur) { gpuscenes::g_cur->dLoad = load ? 1 : 0; gpuscenes::g_cur->dStore = store ? 1 : 0; } // (GXV_GPU_SCENES=2)
#ifdef __VGX_VITA__
    if (mode > 0) vglSetNextSceneDepthModes(load ? GL_TRUE : GL_FALSE, store ? GL_TRUE : GL_FALSE);
#else
    DepthSimScene(efb, certain, load, store);
#endif
}
// ---- texture transfers (tex11; GXV_VITA_UPLOAD_SYNC=2, vgx_host_galaxy_vita.cpp): vitaGL hands a compressed level and the
// generated levels of a texture to the GPU's transfer unit and returns at once. With the host's mode 2 the wait for them is
// made here, once for all the transfers issued before it, at the points where anything could touch their memory: before a call
// that may make vitaGL move a texture's storage (its realloc copies with the CPU), before a texture with pending transfers is
// deleted or given new storage (vitaGL may free the old one at once), when a scene ends (a new target, the present, a
// mid-scene flush: the first moments the GPU may sample an uploaded texture) and at the end of the frame. Modes 0 and 1: no-op.
inline double NowMs();
uint32_t g_xferEpoch = 1;          // bumped by every fence
bool g_xferOpen = false;           // transfers issued since the last fence
unsigned g_xferFrameFences = 0;    // (GXV_STUTTER_LOG) this frame's fences that waited, and their time
double g_xferFrameMs = 0;
inline bool XferLazy() {
#ifdef __VGX_VITA__
    static const bool on = vgxvita_uploadSyncMode && vgxvita_xferFence && vgxvita_uploadSyncMode() == 2;
    return on;
#else
    return false;
#endif
}
inline void XferFence() {
#ifdef __VGX_VITA__
    if (!g_xferOpen) return;
    const double t0 = NowMs();
    vgxvita_xferFence();
    g_xferFrameMs += NowMs() - t0;
    ++g_xferFrameFences;
    g_xferOpen = false;
    ++g_xferEpoch;
#endif
}
// After a call that issued transfers for texture t.
inline void XferIssued(Tex& t) {
    if (!XferLazy()) return;
    g_xferOpen = true;
    t.xferEpoch = g_xferEpoch;
}
// Before t's storage may be freed or moved.
inline void XferFenceFor(const Tex& t) {
    if (g_xferOpen && t.xferEpoch == g_xferEpoch) XferFence();
}
inline void TouchTarget(int need = DN_KEEP) {
    bool began = false; // (GXV_GPU_SCENES=2: this call begins the scene)
    if (S.fbo != g_inUseFbo || g_fbDirty) {
        began = true;
        XferFence(); // (tex11: the scene that ends here may sample a texture just uploaded)
        if (g_sceneLog) LogScene();
        ++C.scenes;
        if (S.fbo == g_efbFbo) ++C.efbScenes;
        // (certainly a new scene for vitaGL: this frame drew to another target last, or the present hook showed the display)
        const bool certain = (g_inUseFbo != ~0u && (g_inUseFbo != S.fbo || g_fbDirty)) || dplan::g_afterHook;
        dplan::g_afterHook = false;
        g_inUseFbo = S.fbo;
        g_fbDirty = false;
        g_midFlushDraws = 0; // (GXV_MIDFLUSH counts a scene's draws)
        if (gpuscenes::g_on) gpuscenes::Begin(S.fbo == g_efbFbo ? gpuscenes::kEfb : S.fbo == 0 ? gpuscenes::kDisp : gpuscenes::kTex); // (GXV_GPU_SCENES)
        SceneDepth(need, certain);
        MarkScissorDirty(); // (vitaGL, STORE_DEPTH_STENCIL: every scene on a framebuffer object rebuilds the mask)
    }
    if (g_scissorDirty) {
        ++C.scissorUpdates;
        g_scissorDirty = false;
        if (gpuscenes::g_mode >= 2 && gpuscenes::g_cur) gpuscenes::NoteMask(began); // (vitaGL rebuilds its scissor mask now)
    }
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
    if (beprof::g_frame) ++beprof::g_progSwitches;
    S.program = id;
    g_uniDirtyV = g_uniDirtyF = true;
}
inline void BindTex(int unit, GLuint name) {
    if (S.tex[unit] == name) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    G(glBindTexture(GL_TEXTURE_2D, name));
    ++C.texBinds;
    if (beprof::g_frame) ++beprof::g_texBinds;
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
// ---- vitaGL's viewport and odd sizes (GXV_RECT_LEGACY, see DrawQuad)
// vitaGL's glViewport halves the size with an integer shift (source/misc.c: x_scale = width >> 1, x_port = x + x_scale, and alike
// for y): a viewport of an odd width or height covers [x, x + w - 1), one column / row short of the GL's [x, x + w), and whatever
// is drawn over it is squeezed into the pixels left. GXV_VGL_VIEWPORT=1 (host tests only) makes the desktop's viewport the same.
#if !defined(__VGX_VITA__)
inline bool VglViewportEmu() {
    static const bool on = EnvInt("GXV_VGL_VIEWPORT", 0) != 0;
    return on;
}
#endif
inline void SetViewport(int x, int y, int w, int h) {
    if (S.vp[0] == x && S.vp[1] == y && S.vp[2] == w && S.vp[3] == h) return;
#if !defined(__VGX_VITA__)
    if (VglViewportEmu()) G(glViewport(x, y, w & ~1, h & ~1));
    else
#endif
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
// alphaMode: 0 the colour factors for alpha too; 1 alpha factors ZERO/ZERO (writes 0); 2 alpha factors ONE/ZERO (RenderState::dstAlphaZero)
inline void SetBlend(GLenum eq, GLenum src, GLenum dst, int alphaMode = 0) {
    if (S.blendOn != 1) { G(glEnable(GL_BLEND)); ++C.stateCalls; S.blendOn = 1; }
    if (S.blendEq != static_cast<int>(eq)) { G(glBlendEquation(eq)); ++C.stateCalls; S.blendEq = static_cast<int>(eq); }
    if (S.blendSrc != static_cast<int>(src) || S.blendDst != static_cast<int>(dst) || S.blendAlphaZero != static_cast<int8_t>(alphaMode)) {
        if (alphaMode == 1) G(glBlendFuncSeparate(src, dst, GL_ZERO, GL_ZERO));
        else if (alphaMode == 2) G(glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO));
        else G(glBlendFunc(src, dst));
        ++C.stateCalls;
        S.blendSrc = static_cast<int>(src); S.blendDst = static_cast<int>(dst);
        S.blendAlphaZero = static_cast<int8_t>(alphaMode);
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
// GXV_RESOURCE_AUDIT=1: bounded basic-log evidence, including quiet player builds.
// Read once, default off. This does not change allocation, retry or draw policy.
bool ResourceAuditOn() {
    return g_beFast2 ? g_f2.audit : resourceaudit::Enabled(); // (GXV_BE_FAST2: no guard per texture)
}
enum class ResourceStage { CreateShader, CompileVertex, CompileFragment, CreateProgram, Link, MissingSource, DrawSkip, WhiteFallback, CopyLink, Count };
struct ResourceRecord {
    uint32_t vs = ~0u, fs = ~0u, texture = 0;
    uint64_t vsHash = 0, fsHash = 0;
    unsigned attempt = 0, slot = 0;
    const char* disposition = "-";
    const char* reason = "-";
};
struct ResourceCategory {
    ResourceRecord records[16]{};
    unsigned emitted = 0;
    uint64_t suppressed = 0;
};
ResourceCategory g_resourceAudit[static_cast<unsigned>(ResourceStage::Count)];
uint64_t ResourceHash(bool fragment, uint32_t id) {
    const auto& sources = fragment ? g_fsSrc : g_vsSrc;
    return id < sources.size() && sources[id] ? sources[id]->keyHash : 0;
}
void ResourceAudit(ResourceStage stage, ResourceRecord record, const char* info = nullptr) {
    if (!ResourceAuditOn()) return;
    static const char* names[] = {"create-shader", "compile-vertex", "compile-fragment", "create-program", "link", "missing-source", "draw-skip", "declared-white", "copy-link"};
    auto& category = g_resourceAudit[static_cast<unsigned>(stage)];
    bool duplicate = false;
    for (unsigned i = 0; i < category.emitted; ++i) {
        const auto& old = category.records[i];
        if (old.vs == record.vs && old.fs == record.fs && old.texture == record.texture &&
            old.vsHash == record.vsHash && old.fsHash == record.fsHash && old.attempt == record.attempt && old.slot == record.slot &&
            !std::strcmp(old.disposition, record.disposition) && !std::strcmp(old.reason, record.reason)) { duplicate = true; break; }
    }
    if (duplicate || category.emitted == 16) {
        // At most 64 summaries per category over its lifetime, even for a persistent bad draw.
        if (category.suppressed != ~uint64_t{0}) ++category.suppressed;
        const auto count = category.suppressed;
        if (count && !(count & (count - 1)))
            autoq::Log("[resource-audit] stage=%s frame=%llu serial=%llu emitted=%u suppressed=%llu",
                       names[static_cast<unsigned>(stage)], static_cast<unsigned long long>(g_progFrame),
                       static_cast<unsigned long long>(g_serial), category.emitted, static_cast<unsigned long long>(count));
        return;
    }
    category.records[category.emitted++] = record;
    char excerpt[97] = {};
    if (info) for (unsigned i = 0; i < sizeof excerpt - 1 && info[i]; ++i) excerpt[i] = info[i] < ' ' ? ' ' : info[i];
    autoq::Log("[resource-audit] stage=%s frame=%llu serial=%llu vs=%u fs=%u vhash=%016llx fhash=%016llx attempt=%u disposition=%s slot=%u texture=%u reason=%s info=%.96s",
               names[static_cast<unsigned>(stage)], static_cast<unsigned long long>(g_progFrame), static_cast<unsigned long long>(g_serial),
               record.vs, record.fs, static_cast<unsigned long long>(record.vsHash), static_cast<unsigned long long>(record.fsHash),
               record.attempt, record.disposition, record.slot, record.texture, record.reason, excerpt);
}
ResourceRecord ResourcePair(uint32_t vs, uint32_t fs, unsigned attempt, const char* disposition, const char* reason) {
    ResourceRecord r;
    r.vs = vs; r.fs = fs; r.vsHash = ResourceHash(false, vs); r.fsHash = ResourceHash(true, fs);
    r.attempt = attempt; r.disposition = disposition; r.reason = reason;
    return r;
}
bool g_compileFailed = false; // the last Compile returned 0 because the source did not compile (GXV_GL_RETRY), not for an allocation
bool BadGlName(GLuint id) {
    // The frozen vitaGL can return UINT_MAX when its program table is full.
#ifdef __VGX_VITA__
    if (id == ~0u) return true;
#endif
    return id == 0;
}

// GXV_FS_HALF=2 (read once through FsHalfMode, default 0; Vita only, needs the vitaGL of patch 0012): EVERY generated fragment shader is
// translated with half precision (vitaGL's float -> half header, texture coordinates and uniforms included), the vertex shaders stay
// float. This is the all-or-nothing form, kept for A/B comparisons. vitaGL translates both shaders of a program inside
// glLinkProgram (its postponed mode), not at glCompileShader, so the low-precision mode is on around the link of a generated
// program only, and only fragment shaders are affected. The translation is not part of vitaGL's compiled-shader cache key (that is a
// hash of the GLSL text handed to glShaderSource), so a fragment shader compiled with the switch on gets a marker line in front of
// its text: its cache entry is its own, and a cache made without the switch is not reused (nor the other way round).
// GXV_FS_HALF=1 / 3 do not come here: they decide per shader in GenerateFragmentShader (the text itself declares half variables for
// the shaders proven to differ by at most one 8-bit level), which needs no vitaGL support and no marker.
bool FsHalf() {
    static const bool on = [] {
#ifdef __VGX_VITA__
        if (FsHalfMode() != 2) return false;
        if (!vglUseLowPrecisionFragmentOnly) { VGX_ERR("GXV_FS_HALF: this vitaGL has no vglUseLowPrecisionFragmentOnly (patch 0012); fragment shaders stay float\n"); return false; }
        return true;
#else
        return false;
#endif
    }();
    return on;
}
constexpr const char kFsHalfMarker[] = "// vgx: fragment precision half (GXV_FS_HALF)\n";

GLuint Compile(GLenum type, const char* text, const char* what, uint32_t logicalId = ~0u, unsigned attempt = 1, bool half = false) {
    g_compileFailed = false;
    if (GlAllocBackoff()) return 0;
#if VGX_COLDTRACE
    coldtrace::Scope trace(coldtrace::g_recorder,coldtrace::Phase::Compile,coldtrace::IoClass::None,{g_progFrame,0x10000u,type,0,0,0},coldtrace::Origin::Utility);
#endif
    const GLuint s = G(glCreateShader(type));
    if (BadGlName(s)) {
        g_glAllocFailSerial = g_serial;
        if (ResourceAuditOn()) ResourceAudit(ResourceStage::CreateShader,
            ResourcePair(type == GL_VERTEX_SHADER ? logicalId : ~0u, type == GL_FRAGMENT_SHADER ? logicalId : ~0u, attempt, "next-serial", what));
        return 0;
    }
    const char* parts[3] = {"#version 100\n", text, nullptr};
    if (half) { parts[1] = kFsHalfMarker; parts[2] = text; }
    G(glShaderSource(s, half ? 3 : 2, parts, nullptr));
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
        if (ResourceAuditOn()) ResourceAudit(type == GL_VERTEX_SHADER ? ResourceStage::CompileVertex : ResourceStage::CompileFragment,
            ResourcePair(type == GL_VERTEX_SHADER ? logicalId : ~0u, type == GL_FRAGMENT_SHADER ? logicalId : ~0u, attempt,
                         logicalId == ~0u ? "caller-policy" : !GlRetry() ? "retained" : attempt >= kGlMaxAttempts ? "exhausted" : "retry", what), log);
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

bool EfbCreateGuard() {
    // GXV_EFB_CREATE_GUARD=1 refuses exhausted EFB names before replacing a valid target; default off.
    static const bool enabled = EnvInt("GXV_EFB_CREATE_GUARD", 0) == 1;
    return enabled;
}
void EfbNameFailure(const char* resource, int w, int h) {
    static unsigned reported = 0;
    if (reported >= 8) return;
    ++reported;
    autoq::Log("[efb-create] no %s name for %dx%d target; previous target retained", resource, w, h);
}

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
        if (EfbCreateGuard() && !color) {
            EfbNameFailure("texture", w, h);
            return false;
        }
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
    GLuint preparedFbo = 0;
    if (EfbCreateGuard() && (!g_efbFbo || KeepRenderTargets())) {
        G(glGenFramebuffers(1, &preparedFbo));
        if (!preparedFbo) {
            RetireTexture(color, w, h);
            EfbNameFailure("framebuffer", w, h);
            return false;
        }
    }
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
    if (!g_efbFbo) {
        if (preparedFbo) g_efbFbo = preparedFbo;
        else G(glGenFramebuffers(1, &g_efbFbo));
    }
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

// ---- area-entry stutter: where the back end's time goes (GXV_STUTTER_LOG, default 1: the basic log; 0 = off)
// The 5-s "fps:" line counts program links, but cannot tell a draw's link in play from the warm-up's in a closed fade, and it does
// not see program deletes (vitaGL's glDeleteProgram starts with sceGxmFinish: a wait for all of the GPU's work) or texture
// uploads. Each Execute adds its costs to g_stFrame; a long frame with a large share of them gets a "stutter:" line, and every 5 s
// one "stutter-5s:" line gives the window's totals, split into play and quiet moments (closed fades and wipes, the pause menu).
// tex11: also texture frees (CMD_TEX_FREE: vitaGL's glDeleteTextures, a stage change's budget evictions), the transfer-unit waits
// (GXV_VITA_UPLOAD_SYNC: inside "texture uploads" in mode 1; mode 2's fences outside them, counted in `xfer`), and of the uploads'
// time the part in glGenTextures (`names`).
struct StutterCosts {
    unsigned frames = 0, demandProgs = 0, warmLinks = 0, stageItems = 0, stageLinks = 0, deletes = 0, texUploads = 0, bufUploads = 0;
    unsigned texFrees = 0, xferFences = 0, xferWaits = 0;
    double demandMs = 0, warmMs = 0, stageMs = 0, deleteMs = 0, texMs = 0, bufMs = 0, execMs = 0, worstMs = 0;
    double texFreeMs = 0, xferMs = 0, xferWaitMs = 0, texNameMs = 0;
    uint64_t texBytes = 0, bufBytes = 0;
    void Add(const StutterCosts& o) {
        frames += o.frames; demandProgs += o.demandProgs; warmLinks += o.warmLinks; stageItems += o.stageItems; stageLinks += o.stageLinks;
        deletes += o.deletes; texUploads += o.texUploads; bufUploads += o.bufUploads;
        texFrees += o.texFrees; xferFences += o.xferFences; xferWaits += o.xferWaits;
        demandMs += o.demandMs; warmMs += o.warmMs; stageMs += o.stageMs; deleteMs += o.deleteMs; texMs += o.texMs; bufMs += o.bufMs;
        texFreeMs += o.texFreeMs; xferMs += o.xferMs; xferWaitMs += o.xferWaitMs; texNameMs += o.texNameMs;
        execMs += o.execMs; worstMs = std::max(worstMs, o.worstMs); texBytes += o.texBytes; bufBytes += o.bufBytes;
    }
    double Known() const { return demandMs + warmMs + stageMs + deleteMs + texMs + bufMs + texFreeMs + xferMs; }
};
StutterCosts g_stFrame, g_stPlay, g_stQuiet;
double g_texNameMs = 0; // (Upload: this frame's glGenTextures time)
bool StutterLogOn() {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    return false;
#else
    static const bool on = EnvInt("GXV_STUTTER_LOG", 1) != 0;
    return on && port_log_summary;
#endif
}
// The screen is live: a stage is being played and no fade, wipe or pause menu hides it (the deferred warm-up's own test).
bool ScreenLive() {
    return port_scene_key && port_screen_quiet && port_scene_key() != 0 && !port_screen_quiet() &&
           (!port_vgx_vita_splash_active || !port_vgx_vita_splash_active());
}
bool g_screenLive = false; // ScreenLive() at the start of the frame being executed
// Every program delete of the eviction paths goes through here (timed: each one can wait for the GPU).
void DeleteGlProgram(GLuint id) {
    const double t0 = NowMs();
    G(glDeleteProgram(id));
    ++g_stFrame.deletes;
    g_stFrame.deleteMs += NowMs() - t0;
}
// GXV_RECLAIM_QUIET (default 1): the housekeeping deletes (the program map's limit, the warm-up's capacity service) wait for a quiet
// moment. In play each of them is a sceGxmFinish in the middle of the frame; the warm-up that wants the room only runs in quiet
// moments anyway, and a draw that finds the slots full still makes room at once (ReclaimForDemand). Only a map more than 64 over its
// limit is trimmed in play.
bool ReclaimQuietOn() {
    static const bool on = EnvInt("GXV_RECLAIM_QUIET", 1) != 0;
    return on;
}
// (GXV_STAGEWARM) the programs of the stage being entered or played, by key: the housekeeping evictions spare them, and a draw's
// emergency reclaim takes them last. g_optionalReserve: while it is set, the free program slots below which an optional link is
// refused (the stage warm-up may use more of the slots than the generic warm-up; 0 = WarmProgramReserve()).
std::unordered_set<uint32_t> g_stageProgs;
unsigned g_optionalReserve = 0;

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
// GXV_DEMAND_RECLAIM (default 1). The ground of the Observatory stayed invisible for a long while after a large warm-up (console,
// 2 Nov; Mario stood on it): the warm-up had linked programs up to its reserve, every one of them stamped as used by its warm-up
// triangle and so protected for 600 frames, and the ~200 programs a newly entered area draws with did not fit in the 124 slots
// left. Each failed glCreateProgram skipped the draw and backed off for the frame, and nothing could be reclaimed until the warm-up
// programs aged out. Now: (1) a warm-up triangle no longer counts as a draw for eviction, so a program only the warm-up has used is
// the first victim, oldest link first, and only then the least recently drawn; (2) a draw that finds the slots full reclaims slots
// at once, in the same frame, and links again (a program the current frame has used is never deleted; vitaGL's glDeleteProgram
// waits for the GPU, so this is a short stall, not a corruption); (3) the warm-up stops at a larger reserve (kWarmProgramReserve).
// GXV_DEMAND_RECLAIM=0: the previous policy.
bool DemandReclaimOn() {
    static const bool on = EnvInt("GXV_DEMAND_RECLAIM", 1) != 0;
    return on;
}
// Victims in eviction order: (rank, key), rank = link frame for warm-up-only programs, (1 << 62) | last draw frame for drawn ones.
// Drawn programs must not have been drawn for minDrawAge frames, warm-up-only ones not linked for minWarmAge frames; neither may be
// used in this frame. (GXV_STAGEWARM) Programs of the current stage's set (g_stageProgs) are left out when spareStage, else ranked
// after all others ((1 << 63) on top).
void CollectProgramVictims(std::vector<std::pair<uint64_t, uint32_t>>& out, uint64_t minDrawAge, uint32_t keepKey, uint64_t minWarmAge = 0,
                           bool spareStage = false) {
    out.clear();
    for (const auto& kv : g_programs) {
        const Program& p = kv.second;
        if (!p.id || !p.ok || kv.first == keepKey || p.usedSerial == g_serial || p.linkSerial == g_serial) continue;
        const uint64_t stage = !g_stageProgs.empty() && g_stageProgs.count(kv.first) ? (uint64_t{1} << 63) : 0;
        if (stage && spareStage) continue;
        if (!p.drawSerial) { if (p.linkSerial + minWarmAge <= g_serial) out.emplace_back(stage | p.linkSerial, kv.first); }
        else if (p.drawSerial + minDrawAge < g_serial) out.emplace_back(stage | (uint64_t{1} << 62) | p.drawSerial, kv.first);
    }
    std::sort(out.begin(), out.end());
}
unsigned DeleteProgramVictims(const std::vector<std::pair<uint64_t, uint32_t>>& victims, unsigned want) {
    unsigned deleted = 0;
    for (const auto& entry : victims) {
        if (deleted == want) break;
        const auto it = g_programs.find(entry.second);
        if (it == g_programs.end()) continue;
        if (it->second.id) {
            if (S.program == it->second.id) S.program = ~0u;
            DeleteGlProgram(it->second.id);
            if (g_stats.programs) --g_stats.programs;
        }
        g_programs.erase(it);
        ++deleted;
    }
    if (deleted) for (ProgramFront& f : g_programFront) f = ProgramFront{};
    return deleted;
}
std::atomic<unsigned> g_demandReclaims{0}, g_demandReclaimed{0}; // (basic log: emergencies and programs they deleted)
// A draw needs a program and vitaGL's slots are (nearly) full: make room now.
unsigned ReclaimForDemand(uint32_t keepKey) {
    if (!DemandReclaimOn()) return 0;
    static std::vector<std::pair<uint64_t, uint32_t>> victims;
    CollectProgramVictims(victims, 2, keepKey);  // (drawn programs: not in this or the previous frame)
    const unsigned n = DeleteProgramVictims(victims, 24);
    const unsigned count = g_demandReclaims.load(std::memory_order_relaxed) + 1;
    if (n) {
        g_demandReclaims.fetch_add(1, std::memory_order_relaxed);
        g_demandReclaimed.fetch_add(n, std::memory_order_relaxed);
    }
    if (count <= 8 || !(count & 63))
        autoq::Log("[programs] draw needed a program with the slots full: %u deleted at once (%u emergencies so far); %u linked",
                   n, count, static_cast<unsigned>(g_programs.size()));
    return n;
}

void EvictProgramMap(bool live) {
#ifdef __VGX_VITA__
    static const size_t limit = static_cast<size_t>(EnvInt("GXV_MAXPROGRAMS", 900));
#else
    static const size_t limit = static_cast<size_t>(EnvInt("GXV_MAXPROGRAMS", 0));
#endif
    if (!limit || g_programs.size() <= limit) return;
    // (GXV_RECLAIM_QUIET: in play only a map far over its limit is trimmed; the rest waits for a quiet moment)
    if (live && ReclaimQuietOn() && g_programs.size() <= limit + 64) return;
    size_t resident = 0;
    for (const auto& kv : g_programs) if (kv.second.id && kv.second.ok) ++resident;
    if (!limit || resident <= limit) return;
    if (live && ReclaimQuietOn() && resident <= limit + 64) return;
    static std::vector<std::pair<uint64_t, uint32_t>> old; // (last use/link, key)
    old.clear();
    if (DemandReclaimOn()) {
        CollectProgramVictims(old, 600, ~0u, 0, true);
        const size_t want = resident - (limit > 100 ? limit - 100 : 0);
        const unsigned n = DeleteProgramVictims(old, static_cast<unsigned>(want));
        if (n) VGX_LOG("%u programs deleted (warm-up only, or unused for 600 frames or more); %u linked now\n", n, static_cast<unsigned>(g_programs.size()));
        return;
    }
    for (const auto& kv : g_programs)
        if (kv.second.id && kv.second.ok && kv.second.usedSerial + 600 < g_serial)
            old.emplace_back(std::max(kv.second.usedSerial, kv.second.linkSerial), kv.first);
    if (old.empty()) return;
    std::sort(old.begin(), old.end());
    const size_t want = resident - (limit > 100 ? limit - 100 : 0);
    size_t n = 0;
    for (; n < old.size() && n < want; ++n) {
        auto it = g_programs.find(old[n].second);
        if (it->second.id) {
            if (S.program == it->second.id) S.program = ~0u;
            DeleteGlProgram(it->second.id);
            if (g_stats.programs) --g_stats.programs;
        }
        g_programs.erase(it);
    }
    for (ProgramFront& f : g_programFront) f = ProgramFront{};
    VGX_LOG("%u programs deleted (unused for 600 frames or more); %u linked now\n", static_cast<unsigned>(n), static_cast<unsigned>(g_programs.size()));
}

constexpr unsigned kWarmProgramReserve = 124;
bool DemandReclaimOn();
// Slots the optional warm-up leaves free for draws: 124 before, 256 with GXV_DEMAND_RECLAIM (an area's own programs, ~200, fit
// without an emergency reclaim and its GPU wait).
inline unsigned WarmProgramReserve() {
    static const unsigned n = DemandReclaimOn() ? static_cast<unsigned>(EnvInt("GXV_WARM_RESERVE", 256)) : kWarmProgramReserve;
    return n;
}
constexpr unsigned kCapacityReclaimBatch = 4;
// GXV_WARM_NOCHURN=1 (default 0): the generic deferred warm-up (RunWarm: every program drawn on this console so far, ~14,000 units
// in r49's "warm N left", far more than vitaGL's 1,024 program slots) no longer makes room for itself. Without it, a link the
// warm-up is refused at its reserve sets g_nativeCapacityPressure, and EvictPrograms then deletes 4 programs unused for 600 frames
// in every quiet frame so that the warm-up can link 4 more: in each pause, fade and dome transition the list goes round, linking
// other galaxies' programs and deleting the ones of the area just left (the hub's while a dome is played; they are linked again on
// the way back). The user's r41 dome round trip: one quiet stretch of 90 frames with 552 warm-up links (162 ms) and 328 program
// deletes (167 ms), each delete a possible wait for the GPU. With the switch the generic warm-up stops at its reserve and waits for
// room that something else frees (the map limit, a draw's or the stage warm-up's own reclaim, which are unchanged); its shaders and
// links up to the reserve are done as before. The stage warm-up (GXV_STAGEWARM, g_optionalReserve set) still asks for room.
bool WarmNoChurn() {
    static const bool on = EnvInt("GXV_WARM_NOCHURN", 0) != 0;
    return on;
}
bool g_nativeCapacityPressure = false;
uint64_t g_capacityServiceSerial = ~uint64_t{0};
uint64_t g_capacityNextScanSerial = 0;
std::atomic<uint64_t> g_warmHeadIdentity{0}, g_warmCapacityHead{0};
uint64_t g_warmNextIdentity = 0, g_warmCapacityBlockSerial = ~uint64_t{0};
bool g_optionalCapacityRefused = false;
uint32_t g_warmCapacityKey = 0;

void WarmDemandProgramReady(uint32_t key) {
    if (g_warmCapacityBlockSerial != ~uint64_t{0} && key == g_warmCapacityKey) {
        g_warmCapacityHead.store(0, std::memory_order_release);
        g_warmCapacityBlockSerial = ~uint64_t{0};
    }
}

// Only Execute's existing retirement boundary may reclaim pressure victims.
void EvictPrograms() {
    EvictProgramMap(g_screenLive);
#ifdef __VGX_VITA__
    if (!g_nativeCapacityPressure || g_capacityServiceSerial == g_serial) return;
    if (g_screenLive && ReclaimQuietOn()) return; // (GXV_RECLAIM_QUIET: room for the warm-up is made in quiet moments, where it runs)
    g_capacityServiceSerial = g_serial;
    unsigned free = vglGetFreeProgramSlots();
    if (free > WarmProgramReserve()) { g_nativeCapacityPressure = false; return; }
    static const int limit = EnvInt("GXV_MAXPROGRAMS", 900);
    if (limit <= 0) return;
    if (g_serial < g_capacityNextScanSerial) return;
    static std::vector<std::pair<uint64_t, uint32_t>> old;
    old.clear();
    if (DemandReclaimOn()) {
        // Room for more warm-up only from programs unused for 600 frames (drawn long ago, or warmed long ago and never drawn),
        // never from recently warmed ones (that only churns) or anything recently drawn.
        CollectProgramVictims(old, 600, ~0u, 601, true);
        if (old.empty()) { g_capacityNextScanSerial = g_serial + 60; return; }
        const unsigned want = free >= WarmProgramReserve() + kCapacityReclaimBatch ? 0u : WarmProgramReserve() + kCapacityReclaimBatch - free;
        free += DeleteProgramVictims(old, std::min(want, kCapacityReclaimBatch));
        if (free > WarmProgramReserve()) g_nativeCapacityPressure = false;
        return;
    }
    uint64_t next = g_serial + 601;
    for (const auto& kv : g_programs) {
        const Program& p = kv.second;
        const uint64_t last = std::max(p.usedSerial, p.linkSerial);
        if (p.id && p.ok) {
            if (last + 600 < g_serial) old.emplace_back(last, kv.first);
            else next = std::min(next, last + 601);
        }
    }
    // Successful insertion/retry stamps linkSerial now; demand use only moves
    // eligibility later. After a zero-candidate scan, neither can beat next.
    // Keep querying native occupancy each serial so utility releases still clear pressure.
    if (old.empty()) { g_capacityNextScanSerial = next; return; }
    g_capacityNextScanSerial = 0;
    std::sort(old.begin(), old.end());
    unsigned deleted = 0;
    for (const auto& entry : old) {
        if (deleted == kCapacityReclaimBatch || free >= WarmProgramReserve() + kCapacityReclaimBatch) break;
        const auto it = g_programs.find(entry.second);
        if (S.program == it->second.id) S.program = ~0u;
        DeleteGlProgram(it->second.id);
        if (g_stats.programs) --g_stats.programs;
        g_programs.erase(it); ++deleted; ++free;
    }
    if (deleted) for (ProgramFront& f : g_programFront) f = ProgramFront{};
    if (free > WarmProgramReserve()) g_nativeCapacityPressure = false;
#endif
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
    const GLuint shader = Compile(fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, s->text.c_str(), fragment ? "fragment" : "vertex", id, attempt ? attempt->failures + 1 : 1, fragment && FsHalf());
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
    if (ResourceAuditOn() && attempt && attempt->failures) ResourceAudit(fragment ? ResourceStage::CompileFragment : ResourceStage::CompileVertex,
        ResourcePair(fragment ? ~0u : id, fragment ? id : ~0u, attempt->failures + 1, "recovered", "compiled"));
    pend[id] = 0;
    if (fragment) ++g_stats.fragmentShaders; else ++g_stats.vertexShaders;
}

Program& UnavailableProgram() {
    static Program p;
    p = Program{};
    return p;
}

// ---- asynchronous shaders (GXV_ASYNC_SHADERS, default 1; 0 = the blocking behaviour of before, nothing below runs)
// A draw whose program is not linked yet and whose shaders are not in vitaGL's compiled-shader cache hands the pair to a worker thread
// (vgx_async_shaders.h) and is skipped, as a draw of a program that failed to link always was, until the worker has written the cache files;
// the ordinary path below then links it with a file read. What must not wait never does (AsyncMustBlock). The announcements (CMD_PROGRAM,
// CMD_VARIANT) are never draws, so they never block: the first draw that needs the program decides. The warm-ups (optional) are behind
// loading screens and keep the old path.
asyncsh::Engine* g_async = nullptr;
bool g_asyncTried = false, g_asyncWaited = false; // (g_asyncWaited: the last GetProgram said Wait; Draw reads it to count its skip)
#ifdef __VGX_VITA__
bool AsyncInCacheHook(void*, const asyncsh::Job& j) {
    return vgxvita_asyncShaderInCache_v1(j.vs.c_str(), 0) && vgxvita_asyncShaderInCache_v1(j.fs.c_str(), 1);
}
int AsyncCompileHook(void*, const asyncsh::Job& j, int* shaders) {
    int n = 0;
    const int rc = vgxvita_asyncShaderCompile_v1(j.vs.c_str(), j.fs.c_str(), j.fsHalf ? 1 : 0, &n);
    if (shaders) *shaders = n;
    return rc;
}
double AsyncNowHook(void*) { return NowMs(); }
void AsyncBoostHook(void*, bool on) { vgxvita_asyncShaderBoost_v1(on ? 1 : 0); }
void AsyncWorkerMain(void* arg) { static_cast<asyncsh::Engine*>(arg)->WorkerLoop(); }
#endif
// The engine, made at the first miss of a draw's program (on the render thread, so vitaGL's lock is created on the thread that owns GL, before
// the worker exists); null when the switch is off or the host cannot do it. Asked once.
asyncsh::Engine* AsyncEngine() {
    if (g_asyncTried) return g_async;
    g_asyncTried = true;
#ifdef __VGX_VITA__
    if (EnvInt("GXV_ASYNC_SHADERS", 0) == 0) return nullptr;  // (off by default in 2.1: the r101 console A/B showed cached links waiting behind the worker's compile lock)
    if (!vgxvita_asyncShaderInCache_v1 || !vgxvita_asyncShaderCompile_v1 || !vgxvita_asyncShaderStart_v1) {
        port_log_summary("async shaders: off (this build has no host support)");
        return nullptr;
    }
    asyncsh::Host host;
    host.inCache = &AsyncInCacheHook;
    host.compile = &AsyncCompileHook;
    host.nowMs = &AsyncNowHook;
    if (vgxvita_asyncShaderBoost_v1) host.boost = &AsyncBoostHook;
    const long maxWait = EnvInt("GXV_ASYNC_SHADERS_MAXWAIT_MS", 15000);
    auto* engine = new asyncsh::Engine(host, static_cast<double>(maxWait < 0 ? 0 : maxWait)); // (never freed: the worker runs for the life of the process)
    char where[128] = "";
    if (!vgxvita_asyncShaderStart_v1(&AsyncWorkerMain, engine, where, sizeof where)) {
        port_log_summary("async shaders: off (%s)", where[0] ? where : "the worker thread or vitaGL's compiler lock could not be made");
        delete engine; // (no thread was made, nothing refers to it)
        return nullptr;
    }
    g_async = engine;
    port_log_summary("async shaders: on, worker %s, a skipped draw waits at most %ld ms (GXV_ASYNC_SHADERS=0 turns it off)", where, maxWait);
#endif
    return g_async;
}
inline bool AsyncPossible() { return g_async || !g_asyncTried; }
// Draws that must never be skipped for want of a program: the player and the main characters (the HD pack's list, the notion GXV_CHARAA_AUTO
// uses for "Mario's own draws"), the HUD and every 2D or screen-space draw (no depth test: layouts, fades, blits, post effects), a draw that
// covers the viewport, anything drawn into a texture (shadow maps, blur chains, copies read later), and opaque cached geometry (a static
// buffer without blending: the map's shapes, GXV_STATICCULL's static runs, and the actors drawn the same way: the back end cannot tell a
// tile from a coin, so GXV_ASYNC_SMALL_VERTS=<n> lets opaque cached draws of at most n vertices wait). What can wait a few frames: blended
// 3D (effects, particles, glass, shadows) and CPU-transformed opaque draws.
bool AsyncMustBlock(const DrawRec& r) {
    static const long smallVerts = EnvInt("GXV_ASYNC_SMALL_VERTS", 0);
    if (S.fbo != g_efbFbo) return true;
    if (!r.rs.depthTest || r.cover) return true;
    if (vgx::IsCharaTexture)
        for (int m = 0; m < 8; ++m)
            if ((r.texMask & (1u << m)) && vgx::IsCharaTexture(r.tex[m].texId)) return true;
    if (!r.rs.blend && r.vbuf) return !(smallVerts > 0 && static_cast<long>(r.vtxCount) <= smallVerts);
    return false;
}
// The shader texts as Compile() hands them to glShaderSource (vitaGL's cache key is the hash of exactly this).
asyncsh::Job AsyncJob(uint16_t vsId, uint16_t fsId) {
    asyncsh::Job j;
    const bool half = FsHalf();
    j.vs = std::string("#version 100\n") + g_vsSrc[vsId]->text;
    j.fs = std::string("#version 100\n") + (half ? kFsHalfMarker : "") + g_fsSrc[fsId]->text;
    j.fsHalf = half;
    return j;
}
// The worker's priority follows the render thread's while the render thread compiles (or waits for the worker's compile) itself.
struct AsyncBoostScope {
    asyncsh::Engine* e; bool on;
    AsyncBoostScope(asyncsh::Engine* engine) : e(engine), on(engine && engine->WorkerBusy()) { if (on) e->Boost(true); }
    ~AsyncBoostScope() { if (on) e->Boost(false); }
};
// The 5 s line (called from StutterFrameEnd's window): shaders compiled in the background, draws skipped, the longest wait.
void AsyncSummary() {
    if (!g_async) return;
    const asyncsh::Stats a = g_async->TakeWindow();
    if (!a.Any()) return;
    port_log_summary("async-shaders-5s: compiled %llu shaders in the background (%llu programs, %llu failed; %.0f ms of compile, longest %.0f ms), draws skipped %llu, "
                     "longest wait %.0f ms, queue %u; blocking path taken %llu (must not wait) + %llu (waited too long), %llu waited for the worker's own compile; "
                     "%llu programs found in the cache (probes %.1f ms)",
                     static_cast<unsigned long long>(a.compiledShaders), static_cast<unsigned long long>(a.compiledPairs), static_cast<unsigned long long>(a.failed),
                     a.compileMs, a.longestCompileMs, static_cast<unsigned long long>(a.skippedDraws), a.longestWaitMs, a.queueDepth,
                     static_cast<unsigned long long>(a.forcedBlocking), static_cast<unsigned long long>(a.forcedByAge), static_cast<unsigned long long>(a.raced),
                     static_cast<unsigned long long>(a.cacheHits), a.probeMs);
}

Program& GetProgram(uint16_t vsId, uint16_t fsId, bool optional = false, const DrawRec* draw = nullptr) {
    const uint32_t key = (static_cast<uint32_t>(vsId) << 16) | fsId;
    auto& fr = g_programFront[((vsId * 7u + fsId) * 2654435761u) >> 22];
    if (fr.p && fr.key == key && (fr.p->ok || !GlRetry())) {
        if (!optional && fr.p->ok) WarmDemandProgramReady(key);
        return *fr.p;
    }
    auto it = g_programs.find(key);
    uint8_t failures = 0;
    if (it != g_programs.end()) {
        const Program& old = it->second;
        // (GXV_GL_RETRY) a failed link is linked again when its delay is over, at most one a frame
        if (old.ok || !GlRetry() || old.failures >= kGlMaxAttempts || g_serial < old.retryAfter || g_programRetryFrame == g_serial ||
            GlAllocBackoff()) {
            fr.key = key;
            fr.p = &it->second;
            if (!optional && it->second.ok) WarmDemandProgramReady(key);
            return it->second;
        }
        failures = old.failures;
    }
    if (GlAllocBackoff()) return UnavailableProgram();
    // (GXV_ASYNC_SHADERS) shaders not in the cache are compiled by the worker; a draw that may wait is skipped meanwhile, one that must not
    // (draw != null and AsyncMustBlock) goes on below as before, and so does anything once the pair is Ready. The announcements pass no draw.
    asyncsh::Engine* const asyncEngine = (!optional && AsyncPossible()) ? AsyncEngine() : nullptr;
    if (asyncEngine && vsId < g_vsSrc.size() && fsId < g_fsSrc.size() && g_vsSrc[vsId] && g_fsSrc[fsId]) {
        if (asyncEngine->Ask(key, draw && AsyncMustBlock(*draw), [&] { return AsyncJob(vsId, fsId); }) == asyncsh::Verdict::Wait) {
            g_asyncWaited = true;
            return UnavailableProgram();
        }
    }
    const AsyncBoostScope asyncBoost(asyncEngine);
#if VGX_COLDTRACE
    coldtrace::Scope trace(coldtrace::g_recorder,coldtrace::Key{g_progFrame,CMD_PROGRAM,vsId,fsId,0,0},coldtrace::Origin::DirectProgram);
#endif
    // (GXV_STUTTER_LOG) a draw's own program creation: its compiles, link and uniform queries, whatever the outcome
    struct DemandTimer {
        bool on; double t0;
        ~DemandTimer() { if (on) { ++g_stFrame.demandProgs; g_stFrame.demandMs += NowMs() - t0; } }
    } demandTimer{!optional, optional ? 0.0 : NowMs()};
    Program p;
    p.failures = failures;
    CompileShader(false, vsId);
    CompileShader(true, fsId);
    if (vsId < g_vs.size() && fsId < g_fs.size() && g_vs[vsId] && g_fs[fsId]) {
#ifdef __VGX_VITA__
        if (optional && vglGetFreeProgramSlots() <= (g_optionalReserve ? g_optionalReserve : WarmProgramReserve())) {
            // (GXV_WARM_NOCHURN: the generic warm-up, g_optionalReserve 0, only waits; the stage warm-up still asks for room)
            if (g_optionalReserve || !WarmNoChurn()) g_nativeCapacityPressure = true;
            g_optionalCapacityRefused = true;
            g_warmCapacityKey = key;
            return UnavailableProgram();
        }
#endif
        if (it != g_programs.end()) g_programRetryFrame = g_serial;
        p.id = G(glCreateProgram());
        // (GXV_DEMAND_RECLAIM) a draw's program with vitaGL's slots full: make room in this frame and create it again
        if (BadGlName(p.id) && !optional && ReclaimForDemand(key)) p.id = G(glCreateProgram());
        if (BadGlName(p.id)) {
            g_glAllocFailSerial = g_serial;
#ifdef __VGX_VITA__
            if (vglGetFreeProgramSlots() == 0) g_nativeCapacityPressure = true;
#endif
            if (ResourceAuditOn()) ResourceAudit(ResourceStage::CreateProgram, ResourcePair(vsId, fsId, failures + 1, "next-serial", "invalid-gl-name"));
            return UnavailableProgram();
        }
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
#ifdef __VGX_VITA__
            const bool half = FsHalf();
            if (half) vglUseLowPrecisionFragmentOnly(GL_TRUE);
#endif
            G(glLinkProgram(p.id));
#ifdef __VGX_VITA__
            if (half) vglUseLowPrecisionFragmentOnly(GL_FALSE);
#endif
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
            if (ResourceAuditOn()) ResourceAudit(ResourceStage::Link,
                ResourcePair(vsId, fsId, failures + 1, !GlRetry() ? "retained" : failures + 1 >= kGlMaxAttempts ? "exhausted" : "retry", "false-link-status"), log);
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
            if (ResourceAuditOn() && failures) ResourceAudit(ResourceStage::Link, ResourcePair(vsId, fsId, failures + 1, "recovered", "linked"));
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
#ifdef __VGX_VITA__
            // (GXV_UNI_SHADOW) where vitaGL keeps each block's values; a block whose upload would not fit what vitaGL says belongs
            // to the uniform, or that is not an F32 vec4 / vec4 array / mat4 (a half precision u_tev), keeps glUniform*.
            if (g_uniShadow && p.id) {
                const GLint locs[US_COUNT] = {p.proj, p.mv, p.tm, p.ptm, p.pal, p.lt, p.tev};
                const GLint vec4s[US_COUNT] = {4, p.nMv, p.nTm, p.nPtm, p.nPal, p.nLt, p.nTev};
                for (int s = 0; s < US_COUNT; ++s) {
                    void *v = nullptr, *f = nullptr;
                    uint32_t room = 0;
                    if (locs[s] == -1 || vec4s[s] <= 0 || !vglGetUniformShadow(p.id, locs[s], &v, &f, &room)) continue;
                    if (static_cast<uint32_t>(vec4s[s]) * 16u > room || (!v && !f)) continue;
                    p.sh[s].v = static_cast<float*>(v);
                    p.sh[s].f = static_cast<float*>(f);
                    p.sh[s].ok = true;
                    p.shadow = true;
                }
            }
#endif
#endif
        }
        if (p.id) ++g_stats.programs; // (GXV_GL_RETRY: a failed link holds no GL program)
    } else {
        if (ResourceAuditOn() && (vsId >= g_vsSrc.size() || !g_vsSrc[vsId] || fsId >= g_fsSrc.size() || !g_fsSrc[fsId]))
            ResourceAudit(ResourceStage::MissingSource, ResourcePair(vsId, fsId, 0, "await-registration", "unregistered"));
        return UnavailableProgram();
    }
    if (it != g_programs.end()) {
        // (a retried link: the entry keeps its address, which g_programFront may hold)
        it->second = p;
        if (!optional && p.ok) WarmDemandProgramReady(key);
        return it->second;
    }
    auto& inserted = g_programs.emplace(key, p).first->second;
    if (!optional && inserted.ok) WarmDemandProgramReady(key);
    return inserted;
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

// ---- GXV_PX_*: cuts of the GPU's pixel work that keep the picture exactly (pixcut). Each switch is read once (Init); all default 0.
//   GXV_PX_NOOP=1       a draw that cannot change colour, alpha or depth (PxNoEffect: nothing written, or blended ZERO / ONE with no
//                       depth write) is not drawn: no fragment work, no program switch, no scene begun for it.
//   GXV_PX_DEADCLEAR=1  a clear whose result the next draw overwrites whole before anything reads it is left out (vitaGL clears by
//                       drawing a quad over the target: a full pass of pixels). An offscreen pass (BeginPass) that begins with a
//                       rectangle over all of its target drops the colour part of its clear, and the depth part too when no draw of the
//                       pass tests depth; an EFB clear (after a copy) followed at once by such a rectangle over the cleared area drops
//                       its colour / alpha part (its depth part stays: the depth plan counts on it).
//   GXV_PX_SCISSOR=1    a scissor change that cannot cut any pixel of the draw is not made: the scissor in place and the draw's own
//                       both hold every pixel the draw can reach (a small screen-space draw whose extent is its vertices' bounds, with
//                       a pixel to spare). vitaGL redraws its scissor mask at every change (tests.c update_scissor_test: one or two
//                       passes over the target).
// "pxcut:" basic-log line every 600 pictures while a switch is on: what was left out.
struct PxCuts { bool noop = false, deadClear = false, scissor = false; } g_px;
struct PxCounts { uint64_t noop = 0, clearColour = 0, clearDepth = 0, clearWhole = 0, scissorKept = 0; } g_pxN, g_pxTotal; // (this window's, the logged windows')
const CmdBuf* g_pxBuf = nullptr; // the buffer being executed (ExecuteFrame)
bool g_pxPassScan = false;       // the next BeginPass is the buffer's own CMD_PASS_BEGIN (dplan::g_cmd): it may look at the pass's commands
// A draw that cannot change a pixel of colour, alpha or depth: no depth written, and colour and alpha either not written or blended with
// source factor ZERO and destination factor ONE (dst * 1 + src * 0, or dst * 1 - src * 0: the target's own value; for alpha only when its
// factors are the colour's: RenderState::dstAlphaZero 0).
inline bool PxNoEffect(const RenderState& rs) {
    if (rs.depthTest && rs.depthWrite) return false;
    const bool keep = rs.blend && rs.blendSrc == BF_ZERO && rs.blendDst == BF_ONE;
    return (!rs.colorWrite || keep) && (!rs.alphaWrite || (keep && rs.dstAlphaZero == 0));
}
// Where an extent [e0, e1] (EFB coordinates) lands in the bound target through a viewport (EFB vp0, vpW) at render scale s, with the
// target's offset o: the same mapping ApplyRenderState's glViewport gives (u_proj maps the viewport onto -1..1 with no offset; Y is
// flipped there so that rows run top to bottom as the EFB's do, vgx_draw.cpp BuildProjC).
struct PxSpan { float lo, hi, v0, v1; };
inline PxSpan PxMap(float e0, float e1, float vp0, float vpW, float s, int o) {
    const float V0 = static_cast<float>(Rnd(vp0 * s) - o), VW = static_cast<float>(Rnd(vpW * s));
    const float k = VW / vpW;
    return PxSpan{V0 + (e0 - vp0) * k, V0 + (e1 - vp0) * k, V0, V0 + VW};
}
// Every pixel the draw can write lies in [x0, x1) x [y0, y1) (target coordinates), with a pixel to spare on each side. Only for a record
// whose extent is its vertices' bounds (vgx_draw.cpp SmallDrawBounds, a small draw that is not depth-tested): one whose extent lies
// strictly inside its scissor on all four sides (an extent the scissor cut, and that of a draw whose vertices are not known - the scissor
// itself - touch it; a merged record's extent is the union of its draws').
bool PxDrawInside(const DrawRec& d, int x0, int y0, int x1, int y1, float sx, float sy, int ox, int oy) {
    const RenderState& rs = d.rs;
    if (d.prim != PRIM_TRIANGLES || d.exW <= 0 || d.exH <= 0 || !(rs.vpW > 0.0f) || !(rs.vpH > 0.0f)) return false;
    if (!(d.exX > rs.scX && d.exY > rs.scY && d.exX + d.exW < rs.scX + rs.scW && d.exY + d.exH < rs.scY + rs.scH)) return false;
    const float e = 0.02f;
    const PxSpan h = PxMap(static_cast<float>(d.exX) - e, static_cast<float>(d.exX + d.exW) + e, rs.vpX, rs.vpW, sx, ox);
    const PxSpan v = PxMap(static_cast<float>(d.exY) - e, static_cast<float>(d.exY + d.exH) + e, rs.vpY, rs.vpH, sy, oy);
    if (!std::isfinite(h.lo) || !std::isfinite(h.hi) || !std::isfinite(v.lo) || !std::isfinite(v.hi) || std::fabs(h.lo) > 16384.0f || std::fabs(h.hi) > 16384.0f ||
        std::fabs(v.lo) > 16384.0f || std::fabs(v.hi) > 16384.0f)
        return false;
    // (pixel i is drawn only when its centre i + 0.5 lies in [lo, hi]: i in floor(lo) .. ceil(hi) - 1; one more on each side)
    const int i0 = static_cast<int>(std::floor(h.lo)) - 1, i1 = static_cast<int>(std::ceil(h.hi)) + 1;
    const int j0 = static_cast<int>(std::floor(v.lo)) - 1, j1 = static_cast<int>(std::ceil(v.hi)) + 1;
    return x0 <= i0 && i1 <= x1 && y0 <= j0 && j1 <= y1;
}
// The draw's quad lies inside the clip volume's depth (|z| <= w, w > 0 at its vertices): nothing of it is clipped away by the near or far
// plane. From its vertices through its own blocks, as DrawAreaPx; false when they are not at hand here.
bool PxQuadInDepth(const CmdBuf& buf, const DrawRec& d) {
    if (d.vbuf || d.idxCount == 0 || d.vsId >= g_vsSrc.size() || !g_vsSrc[d.vsId] || g_vsSrc[d.vsId]->nMv == 0) return false;
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV];
    if (pb == kNoBlock || mb == kNoBlock || pb > buf.uni.size() || buf.uni.size() - pb < 16 || mb > buf.uni.size() || buf.uni.size() - mb < 12) return false;
    if (static_cast<size_t>(d.idxOffset) + d.idxCount > buf.idx.size()) return false;
    size_t base = d.vtxOffset;
    if (!buf.vtxChunks.empty()) {
        if (d.vchunk >= buf.vtxChunks.size()) return false;
        base += buf.vtxChunks[d.vchunk];
    }
    const uint32_t stride = MakeLayout(d.layoutMask).stride;
    const float* P = buf.uni.data() + pb; // (column-major)
    const float* M = buf.uni.data() + mb; // (rows: eye = M[r] . (x, y, z, 1))
    for (uint32_t i = 0; i < d.idxCount; ++i) {
        const size_t off = base + static_cast<size_t>(buf.idx[d.idxOffset + i]) * stride;
        if (off > buf.vtx.size() || buf.vtx.size() - off < 12) return false;
        float p[3];
        std::memcpy(p, buf.vtx.data() + off, 12);
        float eye[4];
        for (int r = 0; r < 3; ++r) eye[r] = M[r * 4 + 0] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2] + M[r * 4 + 3];
        eye[3] = 1.0f;
        const float z = P[2] * eye[0] + P[6] * eye[1] + P[10] * eye[2] + P[14] * eye[3];
        const float w = P[3] * eye[0] + P[7] * eye[1] + P[11] * eye[2] + P[15] * eye[3];
        if (!(w > 1e-6f) || !std::isfinite(z) || !(std::fabs(z) <= 0.999f * w)) return false;
    }
    return true;
}
// The draw's program exists and is linked (a draw whose program is not ready draws nothing: a clear before it must stay).
inline bool PxProgramReady(const DrawRec& d) {
    const auto it = g_programs.find((static_cast<uint32_t>(d.vsId) << 16) | d.fsId);
    return it != g_programs.end() && it->second.ok;
}
// The draw's first primitive writes every pixel of [x0, x1) x [y0, y1) (target coordinates) whatever was there: a record of one axis-aligned
// rectangle (DrawRec::cover: four vertices, not blended, not depth-tested, no alpha test), inside the clip volume's depth, whose own scissor
// leaves the rectangle whole, and whose edges (its extent's whole EFB pixels; the vertices lie within 0.01 of them) are at least an eighth
// of a pixel beyond the rectangle's outer pixel centres; and nothing keeps it from being drawn.
bool PxDrawCovers(const CmdBuf& buf, const DrawRec& d, int x0, int y0, int x1, int y1, float sx, float sy, int ox, int oy) {
    const RenderState& rs = d.rs;
    if (!d.cover || d.vtxCount != 4 || d.prim != PRIM_TRIANGLES || d.vbuf || d.idxCount == 0 || rs.blend || !rs.colorWrite) return false;
    if (rs.depthTest && (rs.depthFunc & 7) != CMP_ALWAYS) return false;
    if (!(rs.vpW > 0.0f) || !(rs.vpH > 0.0f) || x1 <= x0 || y1 <= y0) return false;
    if (g_skipDraws || g_gpuAttrib || SanMode() || d.vchunk >= g_streamVbo.size() || !PxProgramReady(d)) return false;
    // (its own scissor, as ApplyRenderState makes it)
    if (Rnd(rs.scX * sx) - ox > x0 || Rnd(rs.scY * sy) - oy > y0 || Rnd((rs.scX + rs.scW) * sx) - ox < x1 || Rnd((rs.scY + rs.scH) * sy) - oy < y1) return false;
    const float e = 0.01f, m = 0.125f;
    PxSpan h = PxMap(static_cast<float>(d.exX) + e, static_cast<float>(d.exX + d.exW) - e, rs.vpX, rs.vpW, sx, ox);
    PxSpan v = PxMap(static_cast<float>(d.exY) + e, static_cast<float>(d.exY + d.exH) - e, rs.vpY, rs.vpH, sy, oy);
    // (a GL clips at the viewport)
    h.lo = std::max(h.lo, h.v0); h.hi = std::min(h.hi, h.v1);
    v.lo = std::max(v.lo, v.v0); v.hi = std::min(v.hi, v.v1);
    if (!(h.lo <= static_cast<float>(x0) + 0.5f - m && h.hi >= static_cast<float>(x1) - 0.5f + m && v.lo <= static_cast<float>(y0) + 0.5f - m &&
          v.hi >= static_cast<float>(y1) - 0.5f + m))
        return false;
    return PxQuadInDepth(buf, d);
}

// The draw state that was applied last (with the target it was applied for).
RenderState g_rs;
bool g_rsValid = false;

// Returns false when nothing can be drawn (empty scissor).
bool ApplyRenderState(const RenderState& rs, const DrawRec* d = nullptr) {
    // The state of the last draw, applied for this same target (every change of target or
    // of GL state behind this function's back clears g_rsValid; a state whose scissor
    // came out empty is never remembered): nothing to do.
    if (g_rsValid && SameStruct(rs, g_rs)) return true;
    float zNear = rs.zNear, zFar = rs.zFar;
    if (const unsigned sm = SanMode()) { // (GXV_SANITIZE: a viewport of no area or of non-finite numbers is not drawn into; a NaN depth range becomes 0..1)
        if (!sanitize::ViewportUsable(rs.vpX, rs.vpY, rs.vpW, rs.vpH)) {
            ++g_san.badViewport;
            VGX_LOG_N(8, "GXV_SANITIZE: viewport %g,%g %gx%g%s\n", rs.vpX, rs.vpY, rs.vpW, rs.vpH, sanitize::Acts(sm) ? ": draw skipped" : "");
            if (sanitize::Acts(sm)) { ++g_san.skippedDraws; return false; }
        }
        if (!sanitize::Finite(zNear) || !sanitize::Finite(zFar)) {
            ++g_san.badDepthRange;
            VGX_LOG_N(8, "GXV_SANITIZE: depth range %g..%g%s\n", zNear, zFar, sanitize::Acts(sm) ? ": 0..1 used" : "");
            if (sanitize::Acts(sm)) { zNear = sanitize::DepthValue(zNear, 0.0f); zFar = sanitize::DepthValue(zFar, 1.0f); }
        }
    }
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
    const bool off = x0 == 0 && y0 == 0 && x1 == tw && y1 == th;
    bool kept = false;
    if (g_px.scissor && d && S.scissorOn >= 0) {
        // (GXV_PX_SCISSOR: the scissor in place is kept when it and the draw's own both hold every pixel the draw can reach)
        const bool same = off ? S.scissorOn == 0 : (S.scissorOn == 1 && S.sc[0] == x0 && S.sc[1] == y0 && S.sc[2] == x1 - x0 && S.sc[3] == y1 - y0);
        if (!same) {
            const bool on = S.scissorOn == 1;
            const int c0 = on ? S.sc[0] : 0, c1 = on ? S.sc[1] : 0, c2 = on ? S.sc[0] + S.sc[2] : tw, c3 = on ? S.sc[1] + S.sc[3] : th;
            kept = PxDrawInside(*d, c0, c1, c2, c3, sx, sy, ox, oy) && PxDrawInside(*d, x0, y0, x1, y1, sx, sy, ox, oy);
            if (kept) ++g_pxN.scissorKept;
        }
    }
    if (kept) {}
    else if (off) SetScissor(false);
    else SetScissor(true, x0, y0, x1 - x0, y1 - y0);
    SetViewport(Rnd(rs.vpX * sx) - ox, Rnd(rs.vpY * sy) - oy, Rnd(rs.vpW * sx), Rnd(rs.vpH * sy));
    SetDepthRange(zNear, zFar);
    if (rs.blend) SetBlend(rs.blendEq == BE_REVSUB ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend(rs.blendSrc), MapBlend(rs.blendDst), rs.dstAlphaZero == 2 ? 2 : (rs.dstAlphaZero ? 1 : 0));
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
    g_rsValid = !kept; // (a kept scissor is not this state's: the next draw with the same state decides again)
    return true;
}

bool BackgroundPerspective(const CmdBuf& buf, const DrawRec& r) {
    const size_t off = r.ublock[UB_PROJ];
    if (off == kNoBlock || off > buf.uni.size() || buf.uni.size() - off < 16) return false;
    const float* const p = buf.uni.data() + off;
    if (p[3] != 0.0f || p[7] != 0.0f || p[11] != -1.0f || p[15] != 0.0f) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}

void BindTexture(int unit, const TexBind& b, bool cheap = false) {
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
    // (the cheaper background: no blend of two mip levels; EFB copies and other render targets keep theirs)
    if (cheap && !t.renderTarget) {
        if (minF == FILT_LIN_MIP_LIN) minF = FILT_LIN_MIP_NEAR;
        else if (minF == FILT_NEAR_MIP_LIN) minF = FILT_NEAR_MIP_NEAR;
    }
    const bool anisoStale = minF == FILT_LIN_MIP_LIN && t.anisoGen != g_anisoGen; // (trilinear: the switch applies to it)
    // GXV_RUNG_LODBIAS=<percent> (default 0 = off): below the top rung the picture is drawn smaller and stretched, so the GPU picks
    // mip levels for the smaller pixels and textures look softer than at 960x544. A negative LOD bias of percent x log2(960 / width)
    // levels, in vitaGL's 1/8-level steps (-4 levels at most), picks the level a 960-wide picture would have: sharper ground and
    // walls for the same pixels, at the cost of some shimmer and texture reads. Only mip-mapped textures, never render targets.
    const int8_t lodBias = (t.mips && !t.renderTarget) ? RungLodBiasSteps() : 0;
    if (t.wrapS == b.wrapS && t.wrapT == b.wrapT && t.minF == minF && t.magF == b.magFilter && !anisoStale && t.lodBias == lodBias) return;
    if (S.unit != unit) { G(glActiveTexture(GL_TEXTURE0 + unit)); S.unit = unit; }
    if (t.wrapS != b.wrapS) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kWrap[b.wrapS % 3]))); t.wrapS = b.wrapS; ++C.texParams; }
    if (t.wrapT != b.wrapT) { G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kWrap[b.wrapT % 3]))); t.wrapT = b.wrapT; ++C.texParams; }
    if (t.minF != minF || anisoStale) {
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(kMin[minF % 6])));
        t.minF = minF;
        if (minF == FILT_LIN_MIP_LIN) t.anisoGen = g_anisoGen;
        ++C.texParams;
    }
    if (t.lodBias != lodBias) {
        G(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias));
        t.lodBias = lodBias;
        ++C.texParams;
    }
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

#ifdef __VGX_VITA__
// GXV_GXM_DIRECT=1 (read once; default 0 until a console run has compared it): the GXM rebuild's first layer. A scene draw goes to
// vitaGL as ONE call, vglDrawElementsDirect (tools/vitagl/patches/0013-vgx-direct-draw.patch), instead of up to fourteen
// glVertexAttribPointer calls and glDrawElements: every enabled attribute of a decoded vertex comes from the one interleaved buffer,
// so vitaGL sets one vertex stream for all of them (plus one of stride 0 per constant attribute) where it set one per attribute,
// and it keeps the patched vertex program per (program, layout) instead of hashing the whole attribute set on every draw. The
// program, textures, uniforms, render state and indices are the ordinary path's. Lines and points keep the ordinary path; so does
// every draw when the linked vitaGL lacks the call (a weak symbol).
// (the table as tools/vitagl/patches/0013 declares it in vitaGL.h; spelled out here so the back end does not depend on which
// vitaGL.h the dependency folder carries, and declared weak so a vitaGL without the call links and keeps the ordinary path)
enum { VGL_DIRECT_F32 = 0, VGL_DIRECT_U8N = 1, VGL_DIRECT_U8 = 2 };
struct VgxDirectLayout {
    uint32_t key;
    uint32_t enabled;
    uint16_t stride;
    uint8_t size[16];
    uint8_t type[16];
    uint16_t offset[16];
};
extern "C" GLboolean vglDrawElementsDirect(GLenum mode, GLsizei count, uint32_t idxOffset, GLuint vboName, uint32_t base,
                                           const VgxDirectLayout* layout) __attribute__((weak));
bool GxmDirectOnRead() {
    static const bool on = [] {
        const bool want = EnvInt("GXV_GXM_DIRECT", 0) != 0, have = vglDrawElementsDirect != nullptr;
        if (want) autoq::Log("[gxm-direct] %s", have ? "on: scene draws go to vitaGL as one direct call each" : "asked for, but this vitaGL has no vglDrawElementsDirect: ordinary path");
        return want && have;
    }();
    return on;
}
// (read once; then a plain byte, not a function-static guard with its memory barrier, on every draw)
int8_t g_gxmDirect = -1;
inline bool GxmDirectOn() {
    if (g_gxmDirect < 0) g_gxmDirect = GxmDirectOnRead() ? 1 : 0;
    return g_gxmDirect != 0;
}
// The table for a decoded-vertex layout (vgx_cmd.h MakeLayout), built once per layout mask: what SetLayout points the arrays at.
const VgxDirectLayout* DirectLayoutTable(uint32_t layoutMask) {
    static std::unordered_map<uint32_t, VgxDirectLayout> tables;
    auto it = tables.find(layoutMask);
    if (it != tables.end()) return &it->second;
    const VtxLayout L = MakeLayout(layoutMask);
    VgxDirectLayout d{};
    d.key = layoutMask | 0x80000000u;
    d.stride = L.stride;
    auto set = [&](int loc, bool on, uint8_t size, uint8_t type, uint32_t offset) {
        if (!on) return;
        d.enabled |= 1u << loc;
        d.size[loc] = size;
        d.type[loc] = type;
        d.offset[loc] = static_cast<uint16_t>(offset);
    };
    set(AT_POS, true, 4, VGL_DIRECT_F32, 0);
    set(AT_NRM, (L.mask & VL_NRM) != 0, 3, VGL_DIRECT_F32, L.offNrm);
    set(AT_COL0, (L.mask & VL_COL0) != 0, 4, VGL_DIRECT_U8N, L.offCol0);
    set(AT_COL1, (L.mask & VL_COL1) != 0, 4, VGL_DIRECT_U8N, L.offCol1);
    for (int t = 0; t < 8; ++t) set(AT_TEX0 + t, (L.mask & (VL_TEX0 << t)) != 0, 2, VGL_DIRECT_F32, L.offTex[t]);
    set(AT_TMI0, (L.mask & VL_TMI) != 0, 4, VGL_DIRECT_U8, L.offTmi);
    set(AT_TMI1, (L.mask & VL_TMI) != 0, 4, VGL_DIRECT_U8, L.offTmi + 4u);
    return &tables.emplace(layoutMask, d).first->second;
}
// GXV_BE_FAST=1 (default; 0: as before): a draw's layout table and the attribute locations that layout leaves to constants come
// from a small direct-mapped memo (16 entries, a frame uses a handful of layouts) instead of a hash-map lookup (a function-static
// guard, a hash and a modulo) and SetLayoutConstants' rebuilding of the whole layout (MakeLayout) and thirteen tests, per draw.
// (the entries point into the map above, whose values never move; nothing is ever removed from it)
struct DirectMemo { uint32_t mask; uint32_t constBits; const VgxDirectLayout* table; };
DirectMemo g_directMemo[16] = {};
inline const DirectMemo& DirectFor(uint32_t layoutMask) {
    DirectMemo& m = g_directMemo[(layoutMask ^ (layoutMask >> 4) ^ (layoutMask >> 8)) & 15u];
    if (m.table && m.mask == layoutMask) return m;
    m.table = DirectLayoutTable(layoutMask);
    m.mask = layoutMask;
    m.constBits = ((1u << AT_COUNT) - 1u) & ~m.table->enabled; // (every location SetLayoutConstants gives a constant)
    return m;
}
const VgxDirectLayout* DirectLayoutFor(uint32_t layoutMask) { return g_beFast ? DirectFor(layoutMask).table : DirectLayoutTable(layoutMask); }
// The constants of the attributes a layout does not supply, as SetLayout gives them (the arrays it switches on are not touched).
void SetLayoutConstants(uint32_t layoutMask) {
    const VtxLayout L = MakeLayout(layoutMask);
    if (!(L.mask & VL_NRM)) SetAttrib(AT_NRM, false, 3, GL_FLOAT, GL_FALSE, 0, 0, 0, 0, 1, 0);
    if (!(L.mask & VL_COL0)) SetAttrib(AT_COL0, false, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, 0, 1, 1, 1, 1);
    if (!(L.mask & VL_COL1)) SetAttrib(AT_COL1, false, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, 0, 1, 1, 1, 1);
    for (int t = 0; t < 8; ++t)
        if (!(L.mask & (VL_TEX0 << t))) SetAttrib(AT_TEX0 + t, false, 2, GL_FLOAT, GL_FALSE, 0, 0, 0, 0, 0, 1);
    if (!(L.mask & VL_TMI)) {
        SetAttrib(AT_TMI0, false, 4, GL_UNSIGNED_BYTE, GL_FALSE, 0, 0, 60, 60, 60, 60);
        SetAttrib(AT_TMI1, false, 4, GL_UNSIGNED_BYTE, GL_FALSE, 0, 0, 60, 60, 60, 60);
    }
}
std::atomic<uint64_t> g_directDraws{0}, g_directFallbacks{0};
#endif

// Program variants (BlendKey / VariantCmd in vgx_cmd.h). vitaGL compares, per draw, the
// blend function and colour mask with what the program was last drawn with and calls the
// shader patcher when they differ (setup_frag_program in custom_shaders.c), and asks its
// vertex program cache for (vertex shader, attributes, streams) on every draw
// (vglCreateVertexProgram). A combination that was never used before is built there and then.
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
std::unordered_set<uint64_t> g_fragVariants; // legacy folded ID below bit 32; zero-alpha override at bit 32
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
        if (p.blendKey != ~0u && beprof::g_frame) ++beprof::g_blendRepatch; // (GXV_BE_PROF: vitaGL patches the fragment program again)
        p.blendKey = blendKey;
#if !defined(PORT_RUNTIME_DIAGNOSTICS) || PORT_RUNTIME_DIAGNOSTICS
        const uint32_t legacy = (static_cast<uint32_t>(vsId) << 22) ^ (static_cast<uint32_t>(fsId) << 12) ^ (blendKey & 0xFFFu);
        if (g_fragVariants.insert(static_cast<uint64_t>(legacy) | (static_cast<uint64_t>(blendKey & 0x1000u) << 20)).second) ++C.fragVariants;
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
void WarmVariant(const VariantCmd& v, bool optional = false) {
    Program& p = GetProgram(v.vsId, v.fsId, optional);
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
    if (v.blendKey & 1u) SetBlend((v.blendKey & 2u) ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, MapBlend((v.blendKey >> 2) & 15u), MapBlend((v.blendKey >> 6) & 15u), (v.blendKey & (1u << 12)) != 0);
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
    uint64_t identity = 0;
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
    it.identity = ++g_warmNextIdentity;
    if (g_warm.empty()) {
        g_warmCapacityHead.store(0, std::memory_order_release);
        g_warmHeadIdentity.store(it.identity, std::memory_order_release);
        g_warmCapacityBlockSerial = ~uint64_t{0};
    }
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
    if (g_warmCapacityBlockSerial == g_serial) return;
    // Optional replay waits for quiet stages, but cannot hold up the startup overlay's own warm-up.
    if (port_scene_key && port_screen_quiet && port_scene_key() != 0 && !port_screen_quiet() &&
        (!port_vgx_vita_splash_active || !port_vgx_vita_splash_active())) return;
    if (g_warmBackoffLeft > 0) { --g_warmBackoffLeft; return; }
#if VGX_COLDTRACE
    coldtrace::SpanScope sliceTrace(coldtrace::g_recorder,coldtrace::SpanKind::WarmSlice);
#endif
    const double t0 = NowMs();
    unsigned done = 0, compiles0 = g_sw.compiles, links0 = g_sw.links;
    while (!g_warm.empty()) {
        const WarmItem it = g_warm.front();
        g_optionalCapacityRefused = false;
        {
#if VGX_COLDTRACE
        const coldtrace::Key traceKey = it.type==CMD_SHADER ? coldtrace::Key{g_progFrame,it.type,it.src->id,it.src->fragment?1u:0u,0,0} :
            (it.type==CMD_PROGRAM ? coldtrace::Key{g_progFrame,it.type,it.arg>>16,it.arg&0xffffu,0,0} :
             coldtrace::Key{g_progFrame,it.type,it.var.vsId,it.var.fsId,it.var.layoutMask,it.var.blendKey});
        coldtrace::Scope itemTrace(coldtrace::g_recorder,traceKey,coldtrace::Origin::Queued);
#endif
        switch (it.type) {
        case CMD_SHADER: CompileShader(it.src->fragment, it.src->id); break;
        case CMD_PROGRAM: GetProgram(static_cast<uint16_t>(it.arg >> 16), static_cast<uint16_t>(it.arg & 0xFFFFu), true); break;
        default: WarmVariant(it.var, true); break;
        }
        if (g_optionalCapacityRefused) {
            g_warmCapacityBlockSerial = g_serial;
            g_warmCapacityHead.store(it.identity, std::memory_order_release);
            break;
        }
        }
        if (GlAllocBackoff()) {
            // Retain warm work, but give demand its own bounded attempt in this Execute.
            g_warmAllocFailSerial = g_serial;
            g_glAllocFailSerial = ~uint64_t{0};
            break;
        }
        g_warm.pop_front();
        g_warmCapacityBlockSerial = ~uint64_t{0};
        g_warmCapacityHead.store(0, std::memory_order_release);
        g_warmHeadIdentity.store(g_warm.empty() ? 0 : g_warm.front().identity, std::memory_order_release);
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
    g_stFrame.warmMs += took;
    g_stFrame.warmLinks += g_sw.links - links0;
}

// ---- per-stage warm-up (GXV_STAGEWARM, default 1; 0 = off: no recording, no file, no queue)
// The deferred warm-up above replays the whole list (every shader, program and variant drawn on this console so far: 23,000 units in
// the r21 log) in the order they were first met, in quiet moments only. When a stage is entered it links whatever comes next in that
// list (other galaxies' programs, which then take program slots), and the stage's own programs are linked by its draws as they first
// appear: 300-400 links in the 5 s of a stage entry, with hitches of 100-650 ms (r18/r21). So each draw's (vertex shader, fragment
// shader, vertex layout, blend) is also recorded under the stage it is drawn in (port_scene_key), in the order first drawn, by the
// shaders' key hashes (ids differ between runs), and the sets are kept in a file (GXV_STAGEWARM_FILE; appended in quiet moments, at
// most every 120 frames; nothing is written without it). When the game starts loading a stage (port_scene_loading_key) that stage's
// set becomes a queue of its own that goes before the generic warm-up: GXV_STAGEWARM_MS (default 30) ms a frame while the screen is
// not live (the closed wipe and the load behind it), and GXV_STAGEWARM_LIVE_MS (default 0) ms a frame in play. GXV_STAGEWARM_IDLE=1
// (default 0) also links the queue's programs between frames while the game's loading thread runs: links only, no warm draw, no
// delete (see StageWarmIdle). Its optional links may use the program slots down to GXV_STAGEWARM_RESERVE (default 48) free; in a quiet moment it makes
// room by deleting programs that are not the stage's and were not drawn in the last two frames (up to GXV_STAGEWARM_EVICT, default
// 48, at a time). The housekeeping evictions spare the stage's programs, a draw's emergency reclaim takes them last.
// (GXV_STAGEWARM_COMPILE_MS, default 1500; 0 = off) While a stage loads (the screen not live) its queue may run past the usual slice
// (GXV_STAGEWARM_MS), up to GXV_STAGEWARM_COMPILE_SLICE_MS (default 100) a frame, until this much time beyond the slices has gone into
// that load's queue; what is left then is logged and goes on in the usual slices. In frames only: no GPU scene between frames.
long StageCompileMs() {
    static const long ms = std::max(0L, EnvInt("GXV_STAGEWARM_COMPILE_MS", 1500));
    return ms;
}
struct StageRec { uint64_t vs = 0, fs = 0; uint32_t lb = 0; }; // shader key hashes; layout mask << 13 | blend key
struct StageFileRec { uint32_t stage, lb; uint64_t vs, fs; };  // 24 bytes, after an 8-byte magic
static_assert(sizeof(StageFileRec) == 24, "stage warm-up file record");
// (the duplicate check of a set is made only for the stage being recorded into, when its first new draw comes: the sets read from
// the file cost their records alone, 24 bytes each)
struct StageSet { std::vector<StageRec> recs; std::unordered_set<uint64_t> seen; size_t persisted = 0; bool indexed = false; };
std::unordered_map<uint32_t, StageSet> g_stageSets;
std::deque<VariantCmd> g_stageQueue;
std::atomic<unsigned> g_stageLeft{0};
uint32_t g_stageTarget = 0, g_stageRecKey = 0; // stage the queue was made for; stage this frame's draws are recorded under
bool g_stageLoaded = false;
uint64_t g_stageFlushSerial = 0;
size_t g_stageFileRecs = 0;
double g_stageQueueMs = 0;           // spent on the current queue so far
size_t g_stageIdleCursor = 0;        // (GXV_STAGEWARM_IDLE) queue entries from the front whose programs the between-frames path has tried
bool g_stageIdleBlocked = false;     // (GXV_STAGEWARM_IDLE) it met the slot reserve: the rest waits for the frames' slices
unsigned g_stageQueueLinks = 0, g_stageQueued = 0;
double g_stageCompileMs = 0;          // (GXV_STAGEWARM_COMPILE_MS) time beyond the usual slice spent on the queue of this load so far
bool g_stageCompileLogged = false;    // the line "budget used up, N left" was written for this queue
size_t g_stageRecs = 0;               // in memory, all stages
constexpr size_t kStageMaxRecs = 2048, kStageMemMax = 32768, kStageFileMax = 65536; // per stage; in memory (768 KiB); in the file
constexpr char kStageMagic[8] = {'V', 'G', 'X', 'S', 'T', 'W', '0', '1'};
bool StageWarmOnRead() {
    static const bool on = EnvInt("GXV_STAGEWARM", 1) != 0;
    return on;
}
bool StageWarmOn() { return g_beFast2 ? g_f2.stageWarm : StageWarmOnRead(); } // (GXV_BE_FAST2: no guard per draw)
const char* StageFile() {
    static const char* const f = StageWarmOn() ? std::getenv("GXV_STAGEWARM_FILE") : nullptr;
    return f && *f ? f : nullptr;
}
inline uint64_t StageMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline uint64_t StageTuple(const StageRec& r) { return StageMix(r.vs ^ StageMix(r.fs ^ StageMix(r.lb))); }
// fromFile: the file holds no duplicates (only new records are appended), so they are not checked for.
bool StageAdd(uint32_t stage, const StageRec& r, bool fromFile = false) {
    if (g_stageRecs >= kStageMemMax) return false;
    StageSet& set = g_stageSets[stage];
    if (set.recs.size() >= kStageMaxRecs) return false;
    if (!fromFile) {
        if (!set.indexed) {
            set.indexed = true;
            set.seen.reserve(set.recs.size() + 64);
            for (const StageRec& old : set.recs) set.seen.insert(StageTuple(old));
        }
        if (!set.seen.insert(StageTuple(r)).second) return false;
    }
    set.recs.push_back(r);
    ++g_stageRecs;
    return true;
}
void StageLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void StageLog(const char* fmt, ...) {
#if defined(PORT_LOGGING) && !PORT_LOGGING
    (void)fmt;
#else
    if (!port_log_summary) return;
    char text[320];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    port_log_summary("%s", text);
#endif
}
// (GXV_STAGEWARM_PKG, default 1) The list that came with the package (GXV_STAGEWARM_PKG_FILE, set by the host only when the package's
// shader generator key is this build's), read-only. Its records are added after the card's: a record the card has is kept as it is
// (the card wins), the others are new to the card's file and beyond the sets' persisted marks, so the next flush appends them (the
// card's file then holds the union). A file of another format or cut short is ignored from the damaged record on, nothing else.
bool StagePkgOn() { return EnvInt("GXV_STAGEWARM_PKG", 1) != 0; }
void StageLoadPackaged() {
    const char* path = std::getenv("GXV_STAGEWARM_PKG_FILE");
    if (!path || !*path || !StagePkgOn()) return;
    FILE* f = std::fopen(path, "rb");
    if (!f) return;
    char magic[8];
    unsigned read = 0, added = 0;
    if (std::fread(magic, 1, sizeof magic, f) == sizeof magic && std::memcmp(magic, kStageMagic, sizeof magic) == 0) {
        StageFileRec r;
        while (std::fread(&r, 1, sizeof r, f) == sizeof r) {
            ++read;
            if (StageAdd(r.stage, StageRec{r.vs, r.fs, r.lb})) ++added;
        }
        StageLog("stage warm-up: %u draw variants read from the package's %s, %u of them new to the card's list", read, path, added);
    } else {
        StageLog("stage warm-up: the package's %s is not a stage list of this format, ignored", path);
    }
    std::fclose(f);
}
void StageLoad() {
    g_stageLoaded = true;
    const char* path = StageFile();
    if (!path) return;
    bool rewrite = true;
    if (FILE* f = std::fopen(path, "rb")) {
        char magic[8];
        if (std::fread(magic, 1, sizeof magic, f) == sizeof magic && std::memcmp(magic, kStageMagic, sizeof magic) == 0) {
            rewrite = false;
            StageFileRec r;
            size_t got;
            while ((got = std::fread(&r, 1, sizeof r, f)) == sizeof r) {
                ++g_stageFileRecs;
                StageAdd(r.stage, StageRec{r.vs, r.fs, r.lb}, true);
            }
            if (got != 0 || std::ferror(f)) rewrite = true; // (a record cut short: appending after it would shift the rest)
        }
        std::fclose(f);
    }
    if (rewrite) {
        // A new file (missing, another format, or cut short): what was read is written again whole by the next flush.
        g_stageFileRecs = 0;
        FILE* w = std::fopen(path, "wb");
        if (w) {
            const bool ok = std::fwrite(kStageMagic, 1, sizeof kStageMagic, w) == sizeof kStageMagic;
            if (std::fclose(w) != 0 || !ok) g_stageFileRecs = kStageFileMax;
        } else {
            g_stageFileRecs = kStageFileMax; // (no file: recording stays in memory only)
        }
    }
    // What the card's file holds is persisted (or, for a file started anew, nothing is: the next flush writes it whole).
    for (auto& kv : g_stageSets) kv.second.persisted = rewrite ? 0 : kv.second.recs.size();
    size_t recs = 0;
    for (auto& kv : g_stageSets) recs += kv.second.recs.size();
    StageLog("stage warm-up: %u stages, %u draw variants read from %s%s", static_cast<unsigned>(g_stageSets.size()), static_cast<unsigned>(recs), path,
             rewrite ? " (file started anew)" : "");
    StageLoadPackaged();
}
// Appends what was recorded since the last flush (quiet moments only: a few KiB to the memory card).
void StageFlush() {
    const char* path = StageFile();
    if (!path || g_stageFileRecs >= kStageFileMax) return;
    bool any = false;
    for (const auto& kv : g_stageSets) if (kv.second.recs.size() > kv.second.persisted) { any = true; break; }
    if (!any) return;
    FILE* f = std::fopen(path, "ab");
    if (!f) { g_stageFileRecs = kStageFileMax; return; }
    bool ok = true;
    for (auto& kv : g_stageSets) {
        StageSet& set = kv.second;
        for (; ok && set.persisted < set.recs.size() && g_stageFileRecs < kStageFileMax; ++set.persisted, ++g_stageFileRecs) {
            const StageRec& r = set.recs[set.persisted];
            const StageFileRec w{kv.first, r.lb, r.vs, r.fs};
            ok = std::fwrite(&w, sizeof w, 1, f) == 1;
        }
    }
    if (std::fclose(f) != 0 || !ok) g_stageFileRecs = kStageFileMax; // (a failed write: no more appends this run)
}
// The queue for a stage: its recorded variants in the order first drawn, for the shaders this run knows.
void StageBuild(uint32_t target) {
    g_stageTarget = target;
    g_stageQueue.clear();
    g_stageProgs.clear();
    g_stageQueueMs = 0;
    g_stageQueueLinks = 0;
    g_stageQueued = 0;
    g_stageIdleCursor = 0;
    g_stageIdleBlocked = false;
    g_stageCompileMs = 0;
    g_stageCompileLogged = false;
    g_stageLeft.store(0, std::memory_order_relaxed);
    const auto it = g_stageSets.find(target);
    if (!target || it == g_stageSets.end() || it->second.recs.empty()) return;
    std::unordered_map<uint64_t, uint16_t> vsIds, fsIds;
    vsIds.reserve(g_vsSrc.size());
    fsIds.reserve(g_fsSrc.size());
    for (size_t i = 0; i < g_vsSrc.size() && i <= 0xFFFFu; ++i) if (g_vsSrc[i]) vsIds.emplace(g_vsSrc[i]->keyHash, static_cast<uint16_t>(i));
    for (size_t i = 0; i < g_fsSrc.size() && i <= 0xFFFFu; ++i) if (g_fsSrc[i]) fsIds.emplace(g_fsSrc[i]->keyHash, static_cast<uint16_t>(i));
    unsigned unknown = 0;
    for (const StageRec& r : it->second.recs) {
        const auto v = vsIds.find(r.vs), f = fsIds.find(r.fs);
        if (v == vsIds.end() || f == fsIds.end()) { ++unknown; continue; }
        VariantCmd c;
        c.vsId = v->second;
        c.fsId = f->second;
        c.layoutMask = static_cast<uint16_t>((r.lb >> 13) & 0xFFFu);
        c.blendKey = static_cast<uint16_t>(r.lb & 0x1FFFu);
        g_stageQueue.push_back(c);
        g_stageProgs.insert((static_cast<uint32_t>(c.vsId) << 16) | c.fsId);
    }
    g_stageQueued = static_cast<unsigned>(g_stageQueue.size());
    g_stageLeft.store(g_stageQueued, std::memory_order_relaxed);
    StageLog("stage warm-up: stage %08x: %u draw variants of %u programs queued (%u with shaders this run does not know)", target, g_stageQueued,
             static_cast<unsigned>(g_stageProgs.size()), unknown);
    if (StageCompileMs() > 0) {
        unsigned fresh = 0;
        for (const uint32_t k : g_stageProgs) { const auto p = g_programs.find(k); if (p == g_programs.end() || !p->second.ok) ++fresh; }
        StageLog("stage warm-up: stage %08x: %u of its programs are not linked yet (compiled here if the card's cache and the package's seed lack them), up to %ld ms beyond the slices",
                 target, fresh, StageCompileMs());
    }
}
// Room for the stage's programs (quiet moments only): programs that are not the stage's, not drawn in the last two frames.
bool StageEvict() {
    static const unsigned per = static_cast<unsigned>(std::max(1L, EnvInt("GXV_STAGEWARM_EVICT", 48)));
    static std::vector<std::pair<uint64_t, uint32_t>> victims;
    CollectProgramVictims(victims, 2, ~0u, 0, true);
    return DeleteProgramVictims(victims, per) != 0;
}
// A slice of the stage's queue. True when it did work or work is left (the generic warm-up then waits for a later frame).
bool RunStageWarm(bool idle) {
    if (!StageWarmOn() || g_stageQueue.empty()) return false;
    static const double quietMs = static_cast<double>(EnvInt("GXV_STAGEWARM_MS", 30));
    static const double liveMs = static_cast<double>(EnvInt("GXV_STAGEWARM_LIVE_MS", 0));
    static const unsigned reserve = static_cast<unsigned>(std::max(8L, EnvInt("GXV_STAGEWARM_RESERVE", 48)));
    double budgetMs = g_screenLive ? liveMs : quietMs;
    if (budgetMs <= 0.0) return true;
    if (GlAllocBackoff() || g_warmAllocFailSerial == g_serial) return true;
    static const double sliceMax = static_cast<double>(std::max(1L, EnvInt("GXV_STAGEWARM_COMPILE_SLICE_MS", 100)));
    const double compileLeft = static_cast<double>(StageCompileMs()) - g_stageCompileMs;
    if (!g_screenLive && compileLeft > 0.0 && sliceMax > budgetMs) budgetMs += std::min(compileLeft, sliceMax - budgetMs);
    const uint32_t capacityKey = g_warmCapacityKey;
    g_optionalReserve = reserve;
    const double t0 = NowMs();
    const unsigned links0 = g_sw.links;
    unsigned done = 0;
    bool evicted = false;
    while (!g_stageQueue.empty()) {
        const VariantCmd v = g_stageQueue.front();
        g_optionalCapacityRefused = false;
        WarmVariant(v, true);
        if (g_optionalCapacityRefused) {
            // The slots are down to the reserve: make room once (never in play; the delete waits for the GPU), else wait for a frame.
            if (!g_screenLive && !evicted && StageEvict()) { evicted = true; continue; }
            break;
        }
        if (GlAllocBackoff()) {
            g_warmAllocFailSerial = g_serial;
            g_glAllocFailSerial = ~uint64_t{0};
            break;
        }
        g_stageQueue.pop_front();
        if (g_stageIdleCursor) --g_stageIdleCursor;
        ++done;
        if (NowMs() - t0 >= budgetMs) break;
    }
    g_optionalReserve = 0;
    g_optionalCapacityRefused = false;
    g_warmCapacityKey = capacityKey;
    const double took = NowMs() - t0;
    const unsigned links = g_sw.links - links0;
    g_stats.shaderWorkMs += took;
    g_stFrame.stageMs += took;
    g_stFrame.stageItems += done;
    g_stFrame.stageLinks += links;
    g_stageQueueMs += took;
    g_stageQueueLinks += links;
    if (!g_screenLive && StageCompileMs() > 0 && took > quietMs) g_stageCompileMs += took - quietMs;
    g_stageLeft.store(static_cast<unsigned>(g_stageQueue.size()), std::memory_order_relaxed);
    if (!g_stageQueue.empty() && !g_stageCompileLogged && StageCompileMs() > 0 && g_stageCompileMs >= static_cast<double>(StageCompileMs())) {
        g_stageCompileLogged = true;
        unsigned fresh = 0;
        for (const VariantCmd& q : g_stageQueue) { const auto p = g_programs.find((static_cast<uint32_t>(q.vsId) << 16) | q.fsId); if (p == g_programs.end() || !p->second.ok) ++fresh; }
        StageLog("stage warm-up: stage %08x: compile budget of %ld ms used up, %u draw variants left (%u of them without a linked program)", g_stageTarget,
                 StageCompileMs(), static_cast<unsigned>(g_stageQueue.size()), fresh);
    }
    if (g_stageQueue.empty())
        StageLog("stage warm-up: stage %08x ready: %u draw variants, %u links, %.0f ms in all (the last slice %s)", g_stageTarget, g_stageQueued,
                 g_stageQueueLinks, g_stageQueueMs, idle ? "between frames" : (g_screenLive ? "in play" : "in a quiet frame"));
    return true;
}
// Start of a frame: the stage to record under and to prepare for; the file is read the first time, appended in quiet moments.
void StageFrameBegin() {
    if (!StageWarmOn()) return;
    if (!g_stageLoaded) StageLoad();
    g_stageRecKey = port_scene_key ? port_scene_key() : 0;
    const uint32_t loading = port_scene_loading_key ? port_scene_loading_key() : 0;
    const uint32_t target = loading ? loading : g_stageRecKey;
    if (target != g_stageTarget) StageBuild(target);
    if (!g_screenLive && g_serial >= g_stageFlushSerial + 120) {
        g_stageFlushSerial = g_serial;
        StageFlush();
    }
}
// A draw: its variant goes into the set of the stage being played (once per stage; the program remembers its last one).
inline void StageNote(Program& p, const DrawRec& r, uint32_t blendKey) {
    if (!g_stageRecKey) return;
    const uint32_t lb = ((static_cast<uint32_t>(r.layoutMask) & 0xFFFu) << 13) | (blendKey & 0x1FFFu);
    if (p.recStage == g_stageRecKey && p.recLB == lb) return;
    p.recStage = g_stageRecKey;
    p.recLB = lb;
    if (r.vsId >= g_vsSrc.size() || r.fsId >= g_fsSrc.size() || !g_vsSrc[r.vsId] || !g_fsSrc[r.fsId]) return;
    if (StageAdd(g_stageRecKey, StageRec{g_vsSrc[r.vsId]->keyHash, g_fsSrc[r.fsId]->keyHash, lb}) && g_stageRecKey == g_stageTarget)
        g_stageProgs.insert((static_cast<uint32_t>(r.vsId) << 16) | r.fsId);
}

// (GXV_STUTTER_LOG) the end of an Execute: its costs go to the window of play or of quiet moments; a long frame whose time is
// mostly accounted for here (or that took 100 ms or more) gets a line, at most 6 every 5 s; every 5 s a summary of the window.
void StutterFrameEnd(uint64_t frame, double ms) {
    StutterCosts f = g_stFrame;
    g_stFrame = StutterCosts{};
    // (tex11) mode 2's fences (wherever they fell: the uploads' and frees' times leave them out), every transfer-unit wait of the
    // host in any mode (mode 1: inside the uploads' time), and the glGenTextures part of the uploads
    f.xferFences = g_xferFrameFences;
    f.xferMs = g_xferFrameMs;
    g_xferFrameFences = 0;
    g_xferFrameMs = 0;
    f.texNameMs = g_texNameMs;
    g_texNameMs = 0;
#ifdef __VGX_VITA__
    if (vgxvita_xferStats) {
        static uint64_t waits0 = 0, us0 = 0;
        uint64_t waits = 0, us = 0;
        vgxvita_xferStats(&waits, &us);
        f.xferWaits = static_cast<unsigned>(waits - waits0);
        f.xferWaitMs = static_cast<double>(us - us0) / 1000.0;
        waits0 = waits;
        us0 = us;
    }
#endif
    f.frames = 1;
    f.execMs = ms;
    f.worstMs = ms;
    (g_screenLive ? g_stPlay : g_stQuiet).Add(f);
    if (!StutterLogOn()) return;
    static const double minMs = static_cast<double>(EnvInt("GXV_STUTTER_MS", 50));
    static double windowT0 = 0;
    static unsigned lines = 0;
    static uint64_t decode0 = 0;
    const double now = NowMs();
    if (windowT0 == 0) { windowT0 = now; decode0 = vgx_tex_decode_us ? vgx_tex_decode_us() : 0; }
    if (ms >= minMs && (f.Known() >= 10.0 || ms >= 100.0) && lines < 6) {
        ++lines;
        port_log_summary("stutter: frame %llu took %.0f ms in the back end (%s): draws' new programs %u (%.0f ms), warm-up %.0f ms (%u links), stage warm-up %u (%.0f ms, "
                         "%u links), program deletes %u (%.0f ms), texture uploads %u (%.0f KiB, %.0f ms; names %.1f ms, transfer waits %u, %.1f ms), "
                         "texture frees %u (%.0f ms), upload fences %u (%.0f ms), buffer uploads %u (%.0f KiB, %.0f ms), the rest %.0f ms",
                         static_cast<unsigned long long>(frame), ms, g_screenLive ? "in play" : "quiet", f.demandProgs, f.demandMs, f.warmMs, f.warmLinks, f.stageItems,
                         f.stageMs, f.stageLinks, f.deletes, f.deleteMs, f.texUploads, f.texBytes / 1024.0, f.texMs, f.texNameMs, f.xferWaits, f.xferWaitMs,
                         f.texFrees, f.texFreeMs, f.xferFences, f.xferMs, f.bufUploads, f.bufBytes / 1024.0, f.bufMs, std::max(0.0, ms - f.Known()));
    }
    if (now - windowT0 < 5000.0) return;
    const uint64_t decode = vgx_tex_decode_us ? vgx_tex_decode_us() : 0;
    const StutterCosts& a = g_stPlay;
    const StutterCosts& q = g_stQuiet;
    if (a.demandProgs || a.deletes || a.texUploads || a.stageItems || q.demandProgs || q.deletes || q.warmLinks || q.stageItems || q.texUploads ||
        a.worstMs >= minMs) {
        port_log_summary("stutter-5s: in play %u frames (longest %.0f ms): draws' new programs %u (%.0f ms), program deletes %u (%.0f ms), texture uploads %u "
                         "(%.1f MiB, %.0f ms), buffer uploads %.1f MiB (%.0f ms), stage warm-up %u (%.0f ms); quiet %u frames: warm-up %.0f ms (%u links), "
                         "stage warm-up %u (%.0f ms, %u links), draws' new programs %u (%.0f ms), program deletes %u (%.0f ms), texture uploads %u (%.0f ms); "
                         "front-end texture decode %.0f ms; stage %08x queue %u left; %u programs",
                         a.frames, a.worstMs, a.demandProgs, a.demandMs, a.deletes, a.deleteMs, a.texUploads, a.texBytes / 1048576.0, a.texMs,
                         a.bufBytes / 1048576.0, a.bufMs, a.stageItems, a.stageMs, q.frames, q.warmMs, q.warmLinks, q.stageItems, q.stageMs, q.stageLinks,
                         q.demandProgs, q.demandMs, q.deletes, q.deleteMs, q.texUploads, q.texMs, (decode - decode0) / 1000.0, g_stageTarget,
                         g_stageLeft.load(std::memory_order_relaxed), static_cast<unsigned>(g_programs.size()));
        // (tex11) the texture side of the window in detail: the uploads' glGenTextures time, the transfer-unit waits (GXV_VITA_UPLOAD_SYNC),
        // mode 2's fences, the frees (CMD_TEX_FREE)
        if (a.texUploads || q.texUploads || a.texFrees || q.texFrees)
            port_log_summary("tex-upload-5s: in play %u uploads (%.0f ms; names %.1f ms), transfer waits %u (%.1f ms), fences %u (%.1f ms), frees %u (%.1f ms); "
                             "quiet %u uploads (%.0f ms; names %.1f ms), transfer waits %u (%.1f ms), fences %u (%.1f ms), frees %u (%.1f ms); upload sync mode %d, chain reserve %s",
                             a.texUploads, a.texMs, a.texNameMs, a.xferWaits, a.xferWaitMs, a.xferFences, a.xferMs, a.texFrees, a.texFreeMs,
                             q.texUploads, q.texMs, q.texNameMs, q.xferWaits, q.xferWaitMs, q.xferFences, q.xferMs, q.texFrees, q.texFreeMs,
#ifdef __VGX_VITA__
                             vgxvita_uploadSyncMode ? vgxvita_uploadSyncMode() : -1, (vglReserveTextureLevels && EnvInt("GXV_TEX_RESERVE", 1) != 0) ? "on" : "off");
#else
                             -1, "off");
#endif
    }
    AsyncSummary(); // (GXV_ASYNC_SHADERS: its own line, when the worker or a skipped draw had anything to do in the window)
    g_stPlay = StutterCosts{};
    g_stQuiet = StutterCosts{};
    windowT0 = now;
    decode0 = decode;
    lines = 0;
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
    const unsigned clampRow = vs && vs->nPal == kPalRowsShort ? kPalShortMaxRow : vs && vs->nPal == kPalRowsSmall ? kPalSmallMaxRow : 61u;
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

// GXV_SANITIZE: true when the draw must not reach the GPU. Counts what it finds (log-only mode counts and goes on).
// Always checked: non-finite projection or position-matrix words and an index range outside the frame's index data;
// with bit 1 also every streamed vertex the draw names; with bit 3 also non-finite texture / palette / TEV words.
bool SanitizeRefusesDraw(const CmdBuf& buf, const DrawRec& r, const Program& p) {
    const unsigned mode = SanMode();
    bool bad = false;
    const float* u = buf.uni.data();
    const size_t un = buf.uni.size();
    auto block = [&](int b, size_t extra, size_t words, bool always) {
        if (!words || r.ublock[b] == kNoBlock) return;
        const size_t start = static_cast<size_t>(r.ublock[b]) + extra;
        if (start > un || words > un - start) return;
        const uint32_t k = sanitize::CountNonFinite(u + start, words);
        if (!k) return;
        ++g_san.nonFiniteUniform[b];
        VGX_LOG_N(16, "GXV_SANITIZE: frame %llu draw vs=%u fs=%u: %u non-finite words in uniform block %d+%u\n", static_cast<unsigned long long>(buf.frame),
                  static_cast<unsigned>(r.vsId), static_cast<unsigned>(r.fsId), k, b, static_cast<unsigned>(extra));
        if (always || (mode & sanitize::kStrict)) bad = true;
    };
    block(UB_PROJ, 0, p.proj != -1 ? 16 : 0, true);
    block(UB_MV, 0, p.mv != -1 ? 12 : 0, true); // (the position rows; the normal rows' fourth words are unused)
    block(UB_TEXM, 0, p.tm != -1 ? static_cast<size_t>(p.nTm) * 4 : 0, false);
    block(UB_TEXM, 96, p.ptm != -1 ? static_cast<size_t>(p.nPtm) * 4 : 0, false);
    block(UB_PAL, 0, p.pal != -1 ? static_cast<size_t>(p.nPal) * 4 : 0, false);
    block(UB_LIGHT, 0, p.lt != -1 ? static_cast<size_t>(p.nLt) * 4 : 0, false);
    block(UB_TEV, 0, p.tev != -1 ? static_cast<size_t>(p.nTev) * 4 : 0, false);
    if (!r.vbuf) { // (a cached display list's buffers are not kept here: the validator's shadow covers them)
        if (!sanitize::IndexRangeUsable(r.idxOffset, r.idxCount, buf.idx.size())) {
            ++g_san.badIndexRange;
            VGX_LOG_N(16, "GXV_SANITIZE: frame %llu draw vs=%u fs=%u: indices %u+%u outside the %u of the frame\n", static_cast<unsigned long long>(buf.frame),
                      static_cast<unsigned>(r.vsId), static_cast<unsigned>(r.fsId), r.idxOffset, r.idxCount, static_cast<unsigned>(buf.idx.size()));
            bad = true;
        } else if (mode & sanitize::kVertexScan) {
            const uint8_t* vertices = nullptr;
            size_t vertexBytes = 0;
            const size_t chunks = buf.vtxChunks.empty() ? 1 : buf.vtxChunks.size();
            if (r.vchunk < chunks) {
                const size_t first = buf.vtxChunks.empty() ? 0 : buf.vtxChunks[r.vchunk];
                const size_t last = r.vchunk + 1u < chunks ? buf.vtxChunks[r.vchunk + 1] : buf.vtx.size();
                if (first <= last && last <= buf.vtx.size() && !buf.vtx.empty()) { vertices = buf.vtx.data() + first; vertexBytes = last - first; }
            }
            const sanitize::VertexScan sc = sanitize::ScanVertices(r, vertices, vertexBytes, buf.idx.data(), buf.idx.size());
            g_san.badVertexRefs += sc.badRefs;
            g_san.outsideRefs += sc.outside;
            g_san.nonFinitePos += sc.nonFinitePos;
            g_san.extremePos += sc.extremePos;
            g_san.nonFiniteAttr += sc.nonFiniteAttr;
            if (sc.badRefs || sc.nonFinitePos || sc.extremePos || sc.nonFiniteAttr || sc.outside) {
                VGX_LOG_N(16, "GXV_SANITIZE: frame %llu draw vs=%u fs=%u: %u vertices outside the data, %u indices outside the declared range, %u non-finite and %u extreme positions, %u non-finite attributes (first at vertex %u, set 0x%X: bit 0 normal, bit 1+n texture coordinate n); draw: %u vertices, %u indices, layout 0x%X, textures 0x%X (%u %u %u %u), prim %u, %s\n",
                          static_cast<unsigned long long>(buf.frame), static_cast<unsigned>(r.vsId), static_cast<unsigned>(r.fsId), sc.badRefs, sc.outside,
                          sc.nonFinitePos, sc.extremePos, sc.nonFiniteAttr, sc.firstBadVertex, sc.badAttrMask, r.vtxCount, r.idxCount, r.layoutMask, static_cast<unsigned>(r.texMask),
                          r.tex[0].texId, r.tex[1].texId, r.tex[2].texId, r.tex[3].texId, static_cast<unsigned>(r.prim), S.fbo == g_efbFbo ? "into the EFB" : "into a pass or copy target");
            }
            if (sc.badRefs || sc.nonFinitePos || sc.extremePos || sc.nonFiniteAttr) bad = true; // (an index outside the declared range alone is only counted)
        }
    }
    if (bad && sanitize::Acts(mode)) { ++g_san.skippedDraws; return true; }
    return false;
}

// GXV_SANITIZE bit 4 (feedback): the colour and depth textures of the target being drawn into (the EFB's, or the pooled object's
// attachments), and a check that a draw or copy quad does not sample one of them.
void TargetTextures(GLuint& color, GLuint& depth, const char*& what) {
    color = depth = 0;
    what = "?";
    if (S.fbo == g_efbFbo) { color = g_efbColor; depth = g_efbDepthTex; what = "EFB"; return; }
    for (const PoolFbo& f : g_pool)
        if (f.fbo == S.fbo) { color = f.attached; depth = f.depthTex; what = f.attached == f.color ? "pass buffer" : "texture"; return; }
}
void SanitizeFeedbackDraw(const CmdBuf& buf, const DrawRec& r) {
    GLuint color, depth;
    const char* what;
    TargetTextures(color, depth, what);
    for (int m = 0; m < 8; ++m) {
        if (!(r.texMask & (1u << m))) continue;
        if (!sanitize::SamplesOwnTarget(S.tex[m], color, depth)) continue;
        ++g_san.feedbackDraws;
        VGX_LOG_N(32, "GXV_SANITIZE: frame %llu draw vs=%u fs=%u samples %s texture %u (guest texture %u, unit %d) while drawing into it (target: %s%s)\n",
                  static_cast<unsigned long long>(buf.frame), static_cast<unsigned>(r.vsId), static_cast<unsigned>(r.fsId), S.tex[m] == depth ? "the depth" : "the colour", S.tex[m],
                  static_cast<unsigned>(r.tex[m].texId), m, what, g_passDirectTex && g_inPass ? ", pass drawn straight into the copy texture" : "");
    }
}

// GXV_PROF=3: where a draw spends its time (program, state, uniforms, textures, layout, call).
inline double DNow() { return NowMs(); }
bool g_dprofOn = false;
double g_dprof[8] = {};
double g_dprofT = 0;
// (kBackendDiagnostics is a constant: in the player build the stage marks of a draw (seven per draw) compile away instead of testing g_dprofOn each time)
#define DP(i) do { if (kBackendDiagnostics && g_dprofOn) { const double n_ = DNow(); g_dprof[i] += n_ - g_dprofT; g_dprofT = n_; } } while (0)
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
#include "vgx_draw_reject.h"
#endif
#ifdef __VGX_VITA__
// GXV_MIDFLUSH (see g_midFlushEvery): the open EFB scene's vertex work so far goes to the GPU now.
void MidSceneFlush() {
    g_midFlushDraws = 0;
    XferFence(); // (tex11: the flushed part of the scene may sample a texture just uploaded)
    if (&gxm_context == nullptr || gxm_context == nullptr) { g_midFlushEvery = 0; return; }
    const int rc = sceGxmMidSceneFlush(gxm_context, SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS, nullptr, nullptr);
    if (rc < 0) {
        if (port_log_summary) port_log_summary("GXV_MIDFLUSH: sceGxmMidSceneFlush failed (0x%08X) after %u flushes: switched off", static_cast<unsigned>(rc), g_midFlushes);
        g_midFlushEvery = 0;
        return;
    }
    ++g_midFlushes;
    // (the context's programs, textures and default uniform buffers are given again by the next draw)
    if (&gxm_sc_fprog != nullptr) gxm_sc_fprog = nullptr;
    if (&gxm_sc_vprog != nullptr) gxm_sc_vprog = nullptr;
    if (&gxm_sc_ftex_valid != nullptr) gxm_sc_ftex_valid = 0;
    G(glUseProgram(0));
    S.program = ~0u;
}
#endif
void Draw(const CmdBuf& buf, const DrawRec& r) {
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    if(r.idxCount) DrawRejectAdd(g_drawReject.nonempty);
#endif
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    if (g_skipDraws) { DrawRejectNote(RejectSkip,buf,r); return; }
#else
    if (g_skipDraws) return;
#endif
    if (g_px.noop && PxNoEffect(r.rs)) { ++g_pxN.noop; return; } // (GXV_PX_NOOP: it would change nothing)
    if (kBackendDiagnostics && g_dprofOn) g_dprofT = DNow();
    g_asyncWaited = false;
    Program& p = GetProgram(r.vsId, r.fsId, false, &r);
    DP(0);
    if (g_asyncWaited && !p.ok) g_async->NoteSkipped(); // (GXV_ASYNC_SHADERS: this draw waits for the worker's compile)
    if (!p.ok || r.idxCount == 0) {
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
        if(!p.ok) { DrawRejectNote(RejectProgram,buf,r); DrawRejectProgramState(r,p); }
#endif
        if (!p.ok && r.idxCount && ResourceAuditOn()) {
            unsigned failures = p.failures;
            if (r.vsId < g_vsAttempts.size()) failures = std::max(failures, unsigned(g_vsAttempts[r.vsId].failures));
            if (r.fsId < g_fsAttempts.size()) failures = std::max(failures, unsigned(g_fsAttempts[r.fsId].failures));
            ResourceAudit(ResourceStage::DrawSkip, ResourcePair(r.vsId, r.fsId, failures, failures >= kGlMaxAttempts ? "exhausted" : "unavailable", "program-not-ready"));
        }
        return;
    }
    p.drawSerial = g_serial;
    if (g_validateFrame) ValidateDraw(buf, r, p);
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    if (SanMode() && SanitizeRefusesDraw(buf,r,p)) { DrawRejectNote(RejectSanitize,buf,r); return; }
#else
    if (SanMode() && SanitizeRefusesDraw(buf, r, p)) return;
#endif
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
            S.blendAlphaZero = -1;
        }
    }
    GLuint vbo, ibo;
    if (r.vbuf) {
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
        if (r.vbuf >= g_sbuf.size() || !g_sbuf[r.vbuf].vbo) { DrawRejectNote(RejectStatic,buf,r); return; }
#else
        if (r.vbuf >= g_sbuf.size() || !g_sbuf[r.vbuf].vbo) return;
#endif
        vbo = g_sbuf[r.vbuf].vbo;
        ibo = g_sbuf[r.vbuf].ibo;
    } else {
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
        if (r.vchunk >= g_streamVbo.size()) { DrawRejectNote(RejectStream,buf,r); return; }
#else
        if (r.vchunk >= g_streamVbo.size()) return;
#endif
        vbo = g_streamVbo[r.vchunk];
        ibo = g_ibo;
    }
    if (g_gpuAttrib == 2) g_rsValid = false; // (GXV_GPU_ATTRIB=2: the viewport below is not the state's; set it again every draw)
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    if (!ApplyRenderState(r.rs, &r)) { DrawRejectNote(RejectState,buf,r); return; }
#else
    if (!ApplyRenderState(r.rs, &r)) return;
#endif
    if (g_gpuAttrib == 2 && S.fbo == g_efbFbo) SetViewport(S.vp[0], S.vp[1], 1, 1); // (TEST)
    DP(1);
    // The first draw of a scene gives its uniform blocks again when it continues with the
    // program of the last draw before the render-target change: vitaGL would otherwise keep
    // using the buffers it handed to sceGxm in the previous scene (it re-reserves them only
    // after glUseProgram / glUniform*, custom_shaders.c upload_uniforms; gxm.c scene_reset
    // restores the viewport and the region clip, nothing else).
    const bool sceneStart = (S.fbo != g_inUseFbo || g_fbDirty) && S.program == p.id;
    UseProgram(p.id);
    BEPROF_MARK(kDrawProg);
    const bool fresh = p.serial != g_serial || p.uniformBuffer != &buf || sceneStart;
    if (sceneStart) ++C.sceneReuploads;
    const float* u = buf.uni.data();
    const bool uniformReuse = g_beFast2 ? g_f2.uniformReuse : UniformReuseRead(); // (GXV_BE_FAST2: no guard per draw)
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
        const bool different = !SameUniforms(u + old + skip, u + r.ublock[b] + skip, static_cast<size_t>(words)); // (GXV_BE_FAST2: NEON)
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
        if (beprof::g_frame) { ++beprof::g_uniCalls; beprof::g_uniBytes += static_cast<uint32_t>(n) * 16u; }
    };
#ifdef __VGX_VITA__
    // GXV_UNI_SHADOW (see g_uniShadow): the program's uniforms that vitaGL keeps as plain F32 vec4s are compared with what it holds
    // and written there directly, only where they differ, with one call for the dirty flags; the others (a half precision u_tev)
    // keep glUniform*. On the first draw of the program in a buffer or a scene (fresh) the old path gave every block again, which
    // also made vitaGL set its default uniform buffers again; here glUseProgram(0) + glUseProgram(id) does that last part (both
    // stages dirty, the ring copy kept: GXV_VGL_UNIREUSE points sceGxm at it again), and only blocks whose values differ from
    // vitaGL's are written. The same bytes reach vitaGL's buffers as with glUniform4fv / glUniformMatrix4fv.
    const bool viaShadow = g_uniShadow && p.shadow;
    if (viaShadow) {
        if (fresh) {
            G(glUseProgram(0));
            G(glUseProgram(p.id));
            g_uniDirtyV = g_uniDirtyF = true;
        }
        uint32_t wrote = 0;
        auto shadowBlock = [&](int s, int b, GLint words, size_t skip) {
            if (!p.sh[s].ok || r.ublock[b] == kNoBlock || (!fresh && p.applied[b] == r.ublock[b])) return;
            const float* src = u + r.ublock[b] + skip;
            const size_t bytes = static_cast<size_t>(words) * sizeof(float);
            uint32_t w = 0;
            if (float* v = p.sh[s].v; v && !SameUniforms(v, src, static_cast<size_t>(words))) { std::memcpy(v, src, bytes); w |= 1u; }
            if (float* f = p.sh[s].f; f && !SameUniforms(f, src, static_cast<size_t>(words))) { std::memcpy(f, src, bytes); w |= 2u; }
            if (!w) return;
            wrote |= w;
            ++C.uniformCalls;
            C.uniformBytes += static_cast<uint32_t>(bytes);
            if (beprof::g_frame) { ++beprof::g_uniCalls; beprof::g_uniBytes += static_cast<uint32_t>(bytes); } // (counted as glUniform calls)
        };
        shadowBlock(US_PROJ, UB_PROJ, 16, 0);
        shadowBlock(US_MV, UB_MV, p.nMv * 4, 0);
        shadowBlock(US_TM, UB_TEXM, p.nTm * 4, 0);
        shadowBlock(US_PTM, UB_TEXM, p.nPtm * 4, 96);
        shadowBlock(US_PAL, UB_PAL, p.nPal * 4, 0);
        shadowBlock(US_LT, UB_LIGHT, p.nLt * 4, 0);
        shadowBlock(US_TEV, UB_TEV, p.nTev * 4, 0);
        if (wrote) {
            vglUniformShadowWritten(p.id, wrote);
            if (wrote & 1u) g_uniDirtyV = true;
            if (wrote & 2u) g_uniDirtyF = true;
        }
    }
    const auto viaGl = [&](int s) { return !viaShadow || !p.sh[s].ok; };
#else
    const auto viaGl = [](int) { return true; };
#endif
    if (viaGl(US_PROJ) && p.proj != -1 && need(UB_PROJ, 16)) {
        G(glUniformMatrix4fv(p.proj, 1, GL_FALSE, u + r.ublock[UB_PROJ]));
        ++C.uniformCalls;
        C.uniformBytes += 64;
        if (beprof::g_frame) { ++beprof::g_uniCalls; beprof::g_uniBytes += 64; }
        g_uniDirtyV = true;
    }
    if (viaGl(US_MV) && p.mv != -1 && need(UB_MV, p.nMv * 4)) { up4(p.mv, p.nMv, u + r.ublock[UB_MV]); g_uniDirtyV = true; }
    if (viaGl(US_TM) && p.tm != -1 && need(UB_TEXM, p.nTm * 4)) { up4(p.tm, p.nTm, u + r.ublock[UB_TEXM]); g_uniDirtyV = true; }
    if (viaGl(US_PTM) && p.ptm != -1 && need(UB_TEXM, p.nPtm * 4, 96)) { up4(p.ptm, p.nPtm, u + r.ublock[UB_TEXM] + 96); g_uniDirtyV = true; }
    if (viaGl(US_PAL) && p.pal != -1 && need(UB_PAL, p.nPal * 4)) {
        up4(p.pal, p.nPal, u + r.ublock[UB_PAL]);
        ++C.palCalls;
        C.palBytes += static_cast<uint32_t>(p.nPal) * 16u;
        g_uniDirtyV = true;
    }
    if (viaGl(US_LT) && p.lt != -1 && need(UB_LIGHT, p.nLt * 4)) { up4(p.lt, p.nLt, u + r.ublock[UB_LIGHT]); g_uniDirtyV = true; }
    if (viaGl(US_TEV) && p.tev != -1 && need(UB_TEV, p.nTev * 4)) { up4(p.tev, p.nTev, u + r.ublock[UB_TEV]); g_uniDirtyF = true; }
    for (int b = 0; b < UB_COUNT; ++b) {
        if (r.ublock[b] != kNoBlock || fresh) p.applied[b] = r.ublock[b];
    }
    p.serial = g_serial;
    p.uniformBuffer = &buf;
    if (g_uniDirtyV) { C.ringBytes += p.vsBytes; g_uniDirtyV = false; }
    if (g_uniDirtyF) { C.ringBytes += p.fsBytes; g_uniDirtyF = false; }
    BEPROF_MARK(kDrawUni);

    DP(2);
    // Only known depth-tested perspective draws are candidates; ordinary orthographic UI keeps its filters even with AA off.
    // Recognized character/key-item textures protect the whole draw (the pack's list, not a general actor classification).
    bool cheap = g_stepBg && r.rs.depthTest && BackgroundPerspective(buf, r) && p.proj != -1;
    if (cheap && vgx::IsCharaTexture)
        for (int m = 0; m < 8 && cheap; ++m)
            if ((r.texMask & (1u << m)) && vgx::IsCharaTexture(r.tex[m].texId)) cheap = false;
    for (int m = 0; m < 8; ++m)
        if (r.texMask & (1u << m)) {
            if (ResourceAuditOn()) {
                const uint32_t id = r.tex[m].texId;
                const char* reason = id == 0 ? "zero-id" : id >= g_tex.size() ? "out-of-range" : !g_tex[id].id ? "no-gl-name" : nullptr;
                if (reason) {
                    auto record = ResourcePair(r.vsId, r.fsId, 0, "white-fallback", reason);
                    record.slot = m; record.texture = id;
                    ResourceAudit(ResourceStage::WhiteFallback, record);
                }
            }
            BindTexture(m, r.tex[m], cheap);
        }
    DP(3);

#ifdef __VGX_VITA__
    const bool direct = r.prim == PRIM_TRIANGLES && GxmDirectOn();
    if (direct) {
        // (GXV_BE_FAST: nothing to do when every location the layout leaves to a constant has its array off and its constant set,
        // which is exactly when SetLayoutConstants would call nothing)
        if (!g_beFast) SetLayoutConstants(r.layoutMask);
        else if (const uint32_t cb = DirectFor(r.layoutMask).constBits; (cb & S.attrEnabled) || (cb & ~S.attrConstSet)) SetLayoutConstants(r.layoutMask);
    } else
#endif
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
    if (SanMode() & sanitize::kFeedback) SanitizeFeedbackDraw(buf, r);
    TouchTarget();
    DP(5);
    BEPROF_MARK(kDrawTex);
    // (GXV_GPU_ATTRIB=1, TEST: an EFB triangle draw keeps only its first triangle)
    const uint32_t idxCount = g_gpuAttrib == 1 && S.fbo == g_efbFbo && mode == GL_TRIANGLES && r.idxCount > 3 ? 3u : r.idxCount;
#ifdef __VGX_VITA__
    if (direct) {
        // (the arrays SetLayout pointed at are not what vitaGL last drew from any more: the next ordinary draw points them again)
        S.layoutVbo = ~0u;
        if (vglDrawElementsDirect(mode, static_cast<GLsizei>(idxCount), static_cast<uint32_t>(r.idxOffset) * 2u, vbo, r.vtxOffset,
                                  DirectLayoutFor(r.layoutMask))) {
            g_directDraws.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_directFallbacks.fetch_add(1, std::memory_order_relaxed);
            SetLayout(vbo, r.layoutMask, r.vtxOffset);
            G(glDrawElements(mode, static_cast<GLsizei>(idxCount), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(static_cast<size_t>(r.idxOffset) * 2)));
        }
    } else
#endif
    G(glDrawElements(mode, static_cast<GLsizei>(idxCount), GL_UNSIGNED_SHORT, reinterpret_cast<const void*>(static_cast<size_t>(r.idxOffset) * 2)));
    DP(6);
    BEPROF_MARK(kDrawCall);
    if (beprof::g_frame) ++beprof::g_draws;
    if (gpuscenes::g_cur) {
        gpuscenes::NoteDraw(r, r.vsId < g_vsSrc.size() ? g_vsSrc[r.vsId] : nullptr); // (GXV_GPU_SCENES)
        if (gpuscenes::g_mode >= 2) gpuscenes::NoteDrawDetail(buf, r);
    }
#ifdef __VGX_VITA__
    if (g_midFlushEvery && S.fbo == g_efbFbo && ++g_midFlushDraws >= g_midFlushEvery) MidSceneFlush(); // (GXV_MIDFLUSH)
#endif
    ++C.draws;
    C.drawIndices += r.idxCount;
    ++g_stats.drawCallsThisFrame;
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    DrawRejectAdd(g_drawReject.submitted);
#endif
    const uint32_t blendKey = BlendKey(r.rs);
    NoteVariant(p, r.vsId, r.fsId, r.layoutMask, blendKey, S.fbo == g_efbFbo && r.vtxOffset == 0 && r.prim == PRIM_TRIANGLES);
    if (StageWarmOn()) StageNote(p, r, blendKey); // (GXV_STAGEWARM)
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
bool AdoptEfbBuffer(Tex& t, int w, int h); // (GXV_EFBSWAP, below)
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
    XferFenceFor(t); // (tex11, GXV_VITA_UPLOAD_SYNC=2: vitaGL may free the storage its pending transfers write)
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
    if (const unsigned sm = SanMode(); sm && !sanitize::UploadUsable(up.width, up.height, up.levels.size(), up.levels.empty() ? 0 : up.levels[0].size())) {
        ++g_san.badUpload; // (GXV_SANITIZE: a texture of no texels, or with no data for its first level)
        VGX_LOG_N(8, "GXV_SANITIZE: upload to texture %u: %ux%u, %u levels%s\n", up.texId, static_cast<unsigned>(up.width), static_cast<unsigned>(up.height),
                  static_cast<unsigned>(up.levels.size()), sanitize::Acts(sm) ? ": not uploaded" : "");
        if (sanitize::Acts(sm)) { ++g_san.skippedUploads; return; }
    }
    Tex& t = TexSlotFor(up.texId);
    if (t.renderTarget && t.id) DeleteTex(t);
    XferFenceFor(t); // (tex11, GXV_VITA_UPLOAD_SYNC=2: new storage for a texture whose transfers are pending)
    if (!t.id) {
        const double tn = NowMs();
        G(glGenTextures(1, &t.id));
        ++g_stats.textures;
        g_texNameMs += NowMs() - tn; // (GXV_STUTTER_LOG: vitaGL's glGenTextures walks its slots from the first)
    }
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
    // tex11 (GXV_TEX_RESERVE, default 1; vitaGL patch 0015): the level-0 call allocates the whole chain - a compressed chain of
    // these levels, or the levels glGenerateMipmap makes - so that no later call reallocates: no CPU copy of the texture (reads of
    // uncached video memory), no copy racing the level-0 transfer. The final storage, offsets and descriptor are the ones the
    // reallocations end with. Without the patch (weak symbol absent) nothing changes.
    const bool compressedUp = (up.format == TF_DXT1 && g_hasS3tc) || (up.format == TF_DXT5 && g_hasDxt5);
    int reserveLevels = 0;
#if defined(__VGX_VITA__)
    static const bool reserveOn = EnvInt("GXV_TEX_RESERVE", 1) != 0 && vglReserveTextureLevels;
    if (reserveOn) {
        if (compressedUp && !up.genMips && up.levels.size() >= 2 && up.levels.size() < 16 && !(up.width & (up.width - 1u)) && !(up.height & (up.height - 1u))
            && (uint32_t(up.width) >> (up.levels.size() - 1)) >= 4 && (uint32_t(up.height) >> (up.levels.size() - 1)) >= 4)
            reserveLevels = static_cast<int>(up.levels.size());
        else if (!compressedUp && up.genMips && up.levels.size() == 1)
            reserveLevels = -1; // (the full chain glGenerateMipmap makes)
    }
#endif
    std::vector<uint8_t> tmp;
#if defined(__VGX_VITA__)
    const bool preserveI4 = up.preserveI4Mips && GetCaps().authoredI4Mips && up.format == TF_RGBA8 && !up.genMips
        && up.width == up.height && up.width > 1 && up.width <= 4096 && !(up.width & (up.width - 1u)) && up.levels.size() > 1
        && up.levels.size() <= 32u - static_cast<uint32_t>(__builtin_clz(up.width)) && up.levels.size() <= 13;
    if (preserveI4) {
        const void* levels[13];
        for (size_t l = 0; l < up.levels.size(); ++l) levels[l] = up.levels[l].data();
        G(vgxvita_uploadAuthoredI4Mips(up.width, up.height, static_cast<unsigned>(up.levels.size()), levels));
        C.glCalls += static_cast<uint32_t>(up.levels.size()); // helper: base + allocation + count-1 subimages
    }
#else
    const bool preserveI4 = false;
#endif
    for (size_t i = 0; i < up.levels.size(); ++i) {
        const size_t l = !lastSecond || i == 0 ? i : (i == 1 ? up.levels.size() - 1 : i - 1);
        const uint32_t w = l == 0 ? up.width : (l < 32 ? std::max(1u, uint32_t(up.width) >> l) : 1u);
        const uint32_t h = l == 0 ? up.height : (l < 32 ? std::max(1u, uint32_t(up.height) >> l) : 1u);
        const std::vector<uint8_t>& lv = up.levels[l];
#if defined(__VGX_VITA__)
        if (i == 0 && reserveLevels) G(vglReserveTextureLevels(static_cast<GLsizei>(up.width), static_cast<GLsizei>(up.height), reserveLevels));
#endif
        if (compressedUp) {
            // (GXV_VITA_UPLOAD_SYNC=2) a level past the chain allocated so far makes vitaGL reallocate it - a CPU copy of the levels
            // the transfer unit may still be writing: the wait first. With the base first and then the last level (lastSecond)
            // that is the second call only; with the chain reserved, none.
            if (i > 0 && !reserveLevels && (!lastSecond || i == 1)) XferFence();
            G(glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l),
                                     up.format == TF_DXT5 ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, static_cast<GLsizei>(w),
                                     static_cast<GLsizei>(h), 0, static_cast<GLsizei>(lv.size()), lv.data()));
            XferIssued(t);
            g_stats.textureBytes += lv.size();
            C.texUploadBytes += static_cast<uint32_t>(lv.size());
        } else {
            const uint8_t* px = lv.data();
            if (up.format == TF_DXT1) { DecodeDxt1(lv.data(), w, h, tmp); px = tmp.data(); }
            if (up.format == TF_DXT5) { DecodeDxt5(lv.data(), w, h, tmp); px = tmp.data(); }
            // (GXV_TEX16: internal format = format, so that vitaGL keeps the 16-bit words as they are, textures.c fast_store, and picks the
            // GXM format of the same layout: U5U6U5_RGB, U5U5U5U1_RGBA, U4U4U4U4_RGBA)
            const GLenum format = up.format == TF_LA88 ? GL_LUMINANCE_ALPHA : up.format == TF_RGB565 ? GL_RGB : GL_RGBA;
            const GLenum type = up.format == TF_RGB565 ? GL_UNSIGNED_SHORT_5_6_5 : up.format == TF_RGBA5551 ? GL_UNSIGNED_SHORT_5_5_5_1
                              : up.format == TF_RGBA4444 ? GL_UNSIGNED_SHORT_4_4_4_4 : GL_UNSIGNED_BYTE;
            const bool two = up.format == TF_LA88 || type != GL_UNSIGNED_BYTE;
            const uint64_t bytes = static_cast<uint64_t>(w) * h * (two ? 2u : 4u);
            if (!preserveI4) G(glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), format, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, format,
                                           type, px));
            if (!preserveI4 && l > 0) XferIssued(t); // (vitaGL makes a level above 0 itself, with transfer-unit downscales)
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
        XferIssued(t);
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
    static const char* vs = copyvar::kVertex;            // (the sources are vgx_copyvar.h's, byte for byte what was here)
    static const char* fs = copyvar::kFragmentGeneric;
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
    if (!ok) {
        g_glAllocFailSerial = g_serial;
        if (ResourceAuditOn()) ResourceAudit(ResourceStage::CopyLink, ResourcePair(~0u, ~0u, 0, "next-serial", "false-link-status"));
        return false;
    }
    g_copyProg = pendingProgram;
    pendingProgram = 0;
    UseProgram(g_copyProg);
    g_copyRect = G(glGetUniformLocation(g_copyProg, "u_rect"));
    g_copyMat = G(glGetUniformLocation(g_copyProg, "u_mat"));
    g_copyAdd = G(glGetUniformLocation(g_copyProg, "u_add"));
    G(glUniform1i(glGetUniformLocation(g_copyProg, "u_src"), 0));
    return true;
}

// ---- dedicated copy programs (GXV_COPYVAR; vgx_copyvar.h)
// GXV_COPYVAR=1 (default 0): see vgx_copyvar.h. Read once.
bool CopyVarOn() {
    static const bool on = EnvInt("GXV_COPYVAR", 0) == 1;
    return on;
}
struct CopyVariant {
    GLuint prog = 0;
    GLint rect = -1, add = -1;
    bool failed = false; // did not compile or link: the form is drawn by the generic program for good
};
CopyVariant g_copyVar[copyvar::kForms];
// The program of `form`, built at its first use (the generic program is there: EnsureCopyProgram). A failure, or a GL that is
// out of names for now, leaves the generic program in charge (the latter is tried again later).
bool EnsureCopyVariant(copyvar::Form form) {
    CopyVariant& v = g_copyVar[form];
    if (v.prog) return true;
    if (v.failed || GlAllocBackoff()) return false;
    static GLuint sharedVertex = 0; // (one vertex shader for all the variants, as the generic program keeps its own)
    auto fail = [&] {
        v.failed = true;
        autoq::Log("[vgx] GXV_COPYVAR: the %s copy program did not build; the generic one draws those copies", copyvar::Name(form));
        return false;
    };
    if (!sharedVertex) {
        sharedVertex = Compile(GL_VERTEX_SHADER, copyvar::kVertex, "copy variant vertex");
        if (!sharedVertex) return g_compileFailed ? fail() : false; // (no name for now: tried again)
    }
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, copyvar::FragmentSource(form), "copy variant fragment");
    if (!fs) return g_compileFailed ? fail() : false;
    const GLuint prog = G(glCreateProgram());
    if (BadGlName(prog)) { g_glAllocFailSerial = g_serial; return false; } // (tried again)
    G(glAttachShader(prog, sharedVertex));
    G(glAttachShader(prog, fs));
    G(glBindAttribLocation(prog, AT_POS, "a_pos"));
    G(glLinkProgram(prog));
    GLint ok = 0;
    G(glGetProgramiv(prog, GL_LINK_STATUS, &ok));
    if (!ok) {
        G(glDeleteProgram(prog));
        G(glDeleteShader(fs));
        return fail();
    }
    UseProgram(prog);
    v.rect = G(glGetUniformLocation(prog, "u_rect"));
    v.add = G(glGetUniformLocation(prog, "u_add"));
    if (copyvar::UsesTexture(form)) G(glUniform1i(glGetUniformLocation(prog, "u_src"), 0));
    v.prog = prog;
    return true;
}

// GXV_RECT_LEGACY=0 (default): the copy program's quad over a viewport of an odd width or height is drawn over a viewport one pixel
// larger on that side (even: vitaGL then maps it exactly, see SetViewport), with its texture rectangle grown alike so that every
// pixel of the real viewport samples the very point it would in the GL, and a scissor of the real viewport keeps the pixel added
// from being written. Without it a copy into a target of an odd size never wrote its last row (column): at 720x404 the corner
// tile the effects save and put back (BloomEffectSimple / DepthOfFieldBlur: 160x114 EFB pixels, a 180x101 target) came back
// with its last row black, a line across the top-left quarter of the picture (Beach Bowl, the menus). On the Vita (and with
// GXV_VGL_VIEWPORT=1 on the desktop) only: the desktop's GL needs nothing. =1: as before.
inline bool QuadPadOn() {
#if defined(__VGX_VITA__)
    static const bool on = EnvInt("GXV_RECT_LEGACY", 0) == 0;
#else
    static const bool on = VglViewportEmu() && EnvInt("GXV_RECT_LEGACY", 0) == 0;
#endif
    return on;
}
// Draws the copy program's quad over the whole viewport: out = m * texel + add.
// Raster state (target, viewport, scissor, masks) is the caller's.
void DrawQuad(GLuint srcTex, float rx, float ry, float rw, float rh, const float* m16, const float* add4) {
    if (!EnsureCopyProgram()) return;
    // (GXV_RECT_LEGACY: the viewport made even, see QuadPadOn)
    const int vx = S.vp[0], vy = S.vp[1], vw = S.vp[2], vh = S.vp[3];
    const bool pad = QuadPadOn() && vw > 0 && vh > 0 && ((vw | vh) & 1) != 0;
    if (pad) {
        rw = rw * static_cast<float>(vw + (vw & 1)) / static_cast<float>(vw);
        rh = rh * static_cast<float>(vh + (vh & 1)) / static_cast<float>(vh);
    }
    if (SanMode() & sanitize::kFeedback) { // (GXV_SANITIZE bit 4: a copy quad that reads the texture it writes)
        GLuint color, depth;
        const char* what;
        TargetTextures(color, depth, what);
        if (sanitize::SamplesOwnTarget(srcTex, color, depth)) {
            ++g_san.feedbackCopies;
            VGX_LOG_N(32, "GXV_SANITIZE: copy quad reads texture %u (%s) while drawing into it (target: %s)\n", srcTex, srcTex == depth ? "depth" : "colour", what);
        }
    }
    // GXV_COPYVAR (vgx_copyvar.h): a transform that is the identity, the identity with a constant alpha or a constant colour is
    // drawn by a program without the matrix product (and without the uniforms it does not read). Anything else, and a form
    // whose program did not build, goes through the generic program as before.
    copyvar::Form form = copyvar::kGeneric;
    if (kBackendDiagnostics || CopyVarOn()) form = copyvar::Classify(m16, add4);
    if (kBackendDiagnostics) ++(form == copyvar::kIdentity ? C.quadIdentity : form == copyvar::kRgbConstA ? C.quadRgbA : form == copyvar::kConstant ? C.quadConstant : C.quadGeneric);
    const CopyVariant* var = nullptr;
    if (form != copyvar::kGeneric && CopyVarOn() && EnsureCopyVariant(form)) var = &g_copyVar[form];
    if (var) {
        UseProgram(var->prog);
        if (copyvar::UsesTexture(form)) BindTex(0, srcTex);
        uint32_t calls = 0, bytes = 0;
        if (copyvar::UsesRect(form)) { G(glUniform4f(var->rect, rx, ry, rw, rh)); ++calls; bytes += 16; }
        if (copyvar::UsesAdd(form)) { G(glUniform4f(var->add, add4[0], add4[1], add4[2], add4[3])); ++calls; bytes += 16; }
        C.uniformCalls += calls;
        C.uniformBytes += bytes;
        C.ringBytes += (copyvar::UsesRect(form) ? 16 : 0) + (copyvar::UsesAdd(form) ? 16 : 0);
    } else {
        UseProgram(g_copyProg);
        BindTex(0, srcTex);
        G(glUniform4f(g_copyRect, rx, ry, rw, rh));
        G(glUniformMatrix4fv(g_copyMat, 1, GL_FALSE, m16));
        G(glUniform4f(g_copyAdd, add4[0], add4[1], add4[2], add4[3]));
        C.uniformCalls += 3;
        C.uniformBytes += 96;
        C.ringBytes += 16 + 80;
    }
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
    if (gpuscenes::g_mode >= 2) gpuscenes::NoteQuad(); // (GXV_GPU_SCENES=2)
    if (!pad) {
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        return;
    }
    // (the scissor: the real viewport, inside the caller's scissor when one is on; both put back afterwards)
    const int8_t scOn = S.scissorOn;
    const int sc[4] = {S.sc[0], S.sc[1], S.sc[2], S.sc[3]};
    int x0 = vx, y0 = vy, x1 = vx + vw, y1 = vy + vh;
    if (scOn == 1) {
        x0 = std::max(x0, sc[0]); y0 = std::max(y0, sc[1]);
        x1 = std::min(x1, sc[0] + sc[2]); y1 = std::min(y1, sc[1] + sc[3]);
    }
    if (x1 > x0 && y1 > y0) {
        SetScissor(true, x0, y0, x1 - x0, y1 - y0);
        SetViewport(vx, vy, vw + (vw & 1), vh + (vh & 1));
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
        ++C.quadPads;
        SetViewport(vx, vy, vw, vh);
    }
    if (scOn == 1) SetScissor(true, sc[0], sc[1], sc[2], sc[3]);
    else SetScissor(false);
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
inline bool FxCmd(size_t i);       // (GXV_FXHALF, below)
inline bool FxNeutral(uint8_t t);  // (GXV_FXHALF, below)
// GXV_PX_DEADCLEAR (see PxDrawCovers): the colour / alpha part of an EFB clear made after a copy (Clear: the command being executed is the
// copy, dplan::g_cmd) is dead when the next command that draws is a draw into the EFB that covers the cleared rectangle [x0, x1) x [y0, y1)
// (target pixels) and writes every plane the clear writes. Between them only uploads and frees (nothing that draws or shows a picture).
bool PxEfbClearDead(const ClearSpec& c, int x0, int y0, int x1, int y1) {
    if (!g_pxBuf || !dplan::g_inClear || g_inPass) return false; // (an effects run has ended before any copy: ExecuteFrame)
    const CmdBuf& buf = *g_pxBuf;
    size_t j = dplan::g_cmd + 1;
    while (j < buf.cmds.size() && FxNeutral(buf.cmds[j].type)) ++j;
    if (j >= buf.cmds.size() || buf.cmds[j].type != CMD_DRAW || FxCmd(j)) return false;
    const DrawRec& d = buf.draws[buf.cmds[j].arg];
    if ((c.alpha && !d.rs.alphaWrite) || (c.color && !d.rs.colorWrite)) return false;
    return PxDrawCovers(buf, d, x0, y0, x1, y1, ScaleX(), ScaleY(), 0, 0);
}
// GXV_PX_DEADCLEAR for the clear an offscreen pass begins with (BeginPass, the buffer's own CMD_PASS_BEGIN at dplan::g_cmd; the pass
// target is set up: g_passX / Y / W / H, g_passScale): the planes it must still clear. Its colour is dead when the pass's first draw covers
// the whole target writing colour and alpha (uploads and frees may come before it); then its depth too when no draw of the pass tests depth
// (the pass ends at the copy that ends it, as FxDecide reads it, at another pass or at the end of the commands; nothing else reads a pass's
// depth: a depth copy samples the EFB's).
GLbitfield PxPassClearMask() {
    const GLbitfield all = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    if (!g_pxBuf || g_pass < 0) return all;
    const CmdBuf& buf = *g_pxBuf;
    const size_t n = buf.cmds.size();
    size_t j = dplan::g_cmd + 1;
    while (j < n && FxNeutral(buf.cmds[j].type)) ++j;
    if (j >= n || buf.cmds[j].type != CMD_DRAW) return all;
    const DrawRec& first = buf.draws[buf.cmds[j].arg];
    if (!first.rs.alphaWrite) return all;
    const float f = g_passScale;
    if (!PxDrawCovers(buf, first, 0, 0, g_passW, g_passH, ScaleX() * f, ScaleY() * f, g_passX, g_passY)) return all;
    for (size_t k = j; k < n; ++k) {
        const Cmd& c = buf.cmds[k];
        if (c.type == CMD_DRAW) {
            const RenderState& rs = buf.draws[c.arg].rs;
            if (rs.depthTest && (rs.depthFunc & 7) != CMP_ALWAYS) return GL_DEPTH_BUFFER_BIT;
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            if (t.skip ? t.fromPass : !(t.srcTex || (t.passMore && t.fromPass))) break;
        } else if (c.type == CMD_PASS_BEGIN || c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET) {
            break;
        }
    }
    return 0;
}
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
    if (colorByClear && g_px.deadClear && PxEfbClearDead(c, x0, y0, x1, y1)) { // (GXV_PX_DEADCLEAR: the next draw covers it)
        colorByClear = false;
        ++g_pxN.clearColour;
        if (!c.depth) ++g_pxN.clearWhole;
    }
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
        if (gpuscenes::g_mode >= 2) gpuscenes::NoteClear(colorByClear, c.depth); // (GXV_GPU_SCENES=2)
        G(glClear(mask));
        ++C.clears;
    }
}

// GXV_POOL_MAX=<n> (default 16; 0 = no limit, the pool only shrinks by GXV_POOL_IDLE): a pool of n objects or more gives up the one
// unused the longest for a new size instead of growing (RecycleFbo). Around water a copy can change size with every picture; each
// size took an object with its own sceGxm render target, and they were let go only after GXV_POOL_IDLE pictures, so the system ran
// out: sceGxmCreateRenderTarget failed, the scene did not begin and vitaGL wrote through a null uniform buffer (Beach Bowl, on a
// console; from Producdevity's fork, 219837c). Objects used this picture are never given up, so a frame that needs more simply
// grows the pool as before.
size_t PoolMax() {
    static const long n = EnvInt("GXV_POOL_MAX", 16);
    return n > 0 ? static_cast<size_t>(n) : SIZE_MAX;
}
// A full pool: the object unused the longest becomes a new one of w x h, in its slot, so the indexes of open passes stay valid.
// -1: nothing to give up (or GXV_KEEP_RT, the emulator's keeping), the pool grows.
int RecycleFbo(int w, int h) {
    if (KeepRenderTargets() || g_pool.size() < PoolMax()) return -1;
    int oldest = -1;
    for (size_t i = 0; i < g_pool.size(); ++i) {
        const PoolFbo& f = g_pool[i];
        if (f.held || !f.fbo || f.lastUse == g_presents || f.fbo == g_inUseFbo || f.fbo == S.fbo || static_cast<int>(i) == g_pass) continue;
        if (oldest < 0 || f.lastUse < g_pool[oldest].lastUse) oldest = static_cast<int>(i);
    }
    if (oldest < 0) return -1;
    GLuint fresh = 0;
    G(glGenFramebuffers(1, &fresh));
    if (!fresh) return -1; // (no name: nothing is given up)
    PoolFbo& f = g_pool[oldest];
    G(glDeleteFramebuffers(1, &f.fbo));
    if (f.color) { ForgetTex(f.color); G(glDeleteTextures(1, &f.color)); }
    if (f.depthTex) { ForgetTex(f.depthTex); G(glDeleteTextures(1, &f.depthTex)); }
    if (f.depthRb) G(glDeleteRenderbuffers(1, &f.depthRb));
    f = PoolFbo{};
    f.w = w;
    f.h = h;
    f.fbo = fresh;
    ++g_poolRecycled;
    VGX_LOG("framebuffer object %dx%d made in the place of the oldest (%u pooled, %llu given up so far)\n", w, h, static_cast<unsigned>(g_pool.size()),
            static_cast<unsigned long long>(g_poolRecycled));
    return oldest;
}

// A pooled framebuffer object of w x h that has uses left this frame (bound on return).
// tex: the colour texture to draw into (of exactly this size); 0: the object's own colour
// texture. wantDepth: with a depth buffer. An object that already has `tex` attached is
// taken first and not attached to again: on vitaGL attaching to the object in use ends the
// scene (shared.h _glFramebufferTexture2D), so draws that follow each other into one
// texture (the passes of one atlas) stay one scene only this way.
int AcquireFbo(int w, int h, GLuint tex, bool wantDepth, bool guardName = false) {
    // (GXV_KEEP_RT, see DeleteTex: vitaGL gives every framebuffer object a hidden depth buffer
    // at its first scene and frees it when a depth attachment is made (framebuffers.c,
    // glFramebufferRenderbuffer); the emulator then shows that freed buffer's depth, as red, in
    // whatever texture is given its address next. With the attachment made before the first
    // scene nothing is freed.)
    if (KeepRenderTargets()) wantDepth = true;
    int idx = -1;
    if (tex) {
        for (size_t i = 0; i < g_pool.size(); ++i)
            if ((!guardName || g_pool[i].fbo) && g_pool[i].attached == tex && g_pool[i].w == w && g_pool[i].h == h && (g_pool[i].uses < g_fboMaxUses || g_inUseFbo == g_pool[i].fbo)) {
                idx = static_cast<int>(i);
                break;
            }
    }
    if (idx < 0)
        for (size_t i = 0; i < g_pool.size(); ++i)
            if ((!guardName || g_pool[i].fbo) && g_pool[i].w == w && g_pool[i].h == h && g_pool[i].uses < g_fboMaxUses) { idx = static_cast<int>(i); break; }
    if (idx < 0) idx = RecycleFbo(w, h);
    if (idx < 0) {
        PoolFbo f;
        f.w = w;
        f.h = h;
        G(glGenFramebuffers(1, &f.fbo));
        if (guardName && !f.fbo) return -1;
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
    // (GXV_EFBSWAP: written the ordinary way again. A buffer of the size wanted becomes the texture's own storage - no
    // allocation, see AdoptEfbBuffer; another size goes back to the spares and the texture gets storage of its own)
    if (t.efbBuf && !AdoptEfbBuffer(t, w, h)) EfbBufferBack(t);
    // (GXV_KEEP_RT, see DeleteTex: a render target that changes size gets a new texture there
    // instead of new storage, so that the emulator never meets its old memory again)
    if (KeepRenderTargets() && t.id && t.renderTarget && (t.w != w || t.h != h)) { DeleteTex(t); --g_stats.textures; }
    if (!t.id) { G(glGenTextures(1, &t.id)); ++g_stats.textures; }
    if (!t.renderTarget || t.w != w || t.h != h) {
        XferFenceFor(t); // (tex11: new storage; the old may be freed at once)
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

bool CopyAllocationGuard() {
    // GXV_RT_ALLOC_GUARD=1 guards ordinary non-fill copy allocations and keeps the previous target
    // on failure. Experimental, read once, default OFF pending physical-Vita validation.
    static const bool on = EnvInt("GXV_RT_ALLOC_GUARD", 0) != 0;
    return on;
}

// Ordinary non-fill copies only. Allocate and acquire before replacing the last usable picture.
// BeginPass/depth allocations and ApplyRung retain their existing behavior.
int AcquireCopyTarget(uint32_t texId, int w, int h, int ew, int eh, bool guard) {
    if (!guard) return AcquireFbo(w, h, CopyTexture(texId, w, h, ew, eh).id, false);
    Tex& t = TexSlotFor(texId);
    if (t.efbBuf) AdoptEfbBuffer(t, w, h); // (as in CopyTexture: no allocation for a buffer of the right size)
    if (t.id && t.renderTarget && !t.efbBuf && t.w == w && t.h == h) {
        BindTexForEdit(t.id);
        if (BoundTexHasStorage(w, h)) {
            const int target = AcquireFbo(w, h, t.id, false, true);
            if (target >= 0) { t.ew = static_cast<uint16_t>(ew); t.eh = static_cast<uint16_t>(eh); }
            return target;
        }
    }
    GLuint fresh = 0;
    G(glGenTextures(1, &fresh));
    if (!fresh) return -1;
    BindTexForEdit(fresh);
    G(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    if (!BoundTexHasStorage(w, h)) {
        G(glDeleteTextures(1, &fresh));
        ForgetTex(fresh);
        ++C.texDeletes;
        return -1;
    }
    SetupNewTexture(GL_LINEAR);
    const int target = AcquireFbo(w, h, fresh, false, true);
    if (target < 0) {
        G(glDeleteTextures(1, &fresh));
        ForgetTex(fresh);
        ++C.texDeletes;
        return -1;
    }
    if (!t.id) ++g_stats.textures;
    DeleteTex(t);
    t.id = fresh;
    t.w = static_cast<uint16_t>(w); t.h = static_cast<uint16_t>(h);
    t.ew = static_cast<uint16_t>(ew); t.eh = static_cast<uint16_t>(eh);
    t.renderTarget = true;
    t.wrapS = t.wrapT = WRAP_CLAMP;
    t.minF = t.magF = FILT_LINEAR;
    BindFbo(g_pool[target].fbo);
    return target;
}

// CMD_PASS_BEGIN: the draws up to the next copy go to a target of the pass
// rectangle's size that starts cleared to the values the EFB rectangle held.
// That includes passes over the whole EFB rectangle: they may use one plane
// only (the blur chain's last steps write alpha) while another plane of the
// EFB holds the picture.
void BeginPass(const PassCmd& p) {
    const bool pxScan = g_pxPassScan; // (GXV_PX_DEADCLEAR: the buffer's own pass, at dplan::g_cmd)
    g_pxPassScan = false;
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
    GLbitfield clearMask = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    if (pxScan && g_px.deadClear && !p.atlasTex && st == GL_FRAMEBUFFER_COMPLETE) { // (GXV_PX_DEADCLEAR)
        clearMask = PxPassClearMask();
        if (!(clearMask & GL_COLOR_BUFFER_BIT)) ++g_pxN.clearColour;
        if (!(clearMask & GL_DEPTH_BUFFER_BIT)) ++g_pxN.clearDepth;
        if (!clearMask) ++g_pxN.clearWhole;
    }
    if (clearMask) { // (none left: the pass's scene begins with its first draw)
        TouchTarget((clearMask & GL_DEPTH_BUFFER_BIT) ? DN_CLEARED : DN_KEEP);
        if (gpuscenes::g_mode >= 2) gpuscenes::NoteClear((clearMask & GL_COLOR_BUFFER_BIT) != 0, (clearMask & GL_DEPTH_BUFFER_BIT) != 0); // (GXV_GPU_SCENES=2)
        G(glClear(clearMask));
        ++C.clears;
    }
    g_inPass = true;
    g_staticCrowdPassComplete = st == GL_FRAMEBUFFER_COMPLETE;
    g_rsValid = false;
}

// GXV_SANITIZE: true when this copy is reduced to its clear (a copy of no texels, or of non-finite colour numbers).
bool SanitizeCopyOnlyClear(const CopyTexCmd& c) {
    const unsigned sm = SanMode();
    if (!sm || c.skip || sanitize::CopyUsable(c, g_efbW, g_efbH)) return false;
    ++g_san.badCopy;
    VGX_LOG_N(8, "GXV_SANITIZE: copy to texture %u: source %d,%d %dx%d, destination %ux%u (atlas %ux%u)%s\n", c.texId, c.src.x, c.src.y, c.src.w, c.src.h,
              static_cast<unsigned>(c.dstW), static_cast<unsigned>(c.dstH), static_cast<unsigned>(c.dstTexW), static_cast<unsigned>(c.dstTexH),
              sanitize::Acts(sm) ? ": only its clear is made" : "");
    if (!sanitize::Acts(sm)) return false;
    ++g_san.skippedCopies;
    dplan::g_valid = false; // (no copy scene happened: conservative depth hints for the rest of the frame, as for a failed target)
    return true;
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
    if (c.skip || SanitizeCopyOnlyClear(c)) { // only its clear matters (a dead copy, or one that is made later from an atlas cell)
        if (c.fromPass && g_inPass) EndPass();
        Clear(c.clear, c.src);
        return;
    }
    // (census for the efbswap2 line: what the copies of a picture are)
    ++C.copiesMade;
    if (!c.fromPass && !c.srcTex && !c.dstTexW && !c.depth && !c.fill && c.src.x == 0 && c.src.y == 0 && c.src.w == g_efbW && c.src.h == g_efbH &&
        c.dstW == g_efbW && c.dstH == g_efbH) {
        ++C.wholeCopies;
        bool rgb = true, a = true; // (rows r, g, b / row a: that channel unchanged)
        for (int r = 0; r < 4; ++r) {
            bool same = c.add[r] == 0;
            for (int col = 0; col < 4; ++col) same = same && c.mat[r][col] == (r == col ? 1.0f : 0.0f);
            if (r < 3) rgb = rgb && same;
            else a = same;
        }
        if (rgb && !a) ++C.wholeRgb;
        if (a && !rgb) ++C.wholeA;
        if (c.swapOk) ++C.wholeOk;
        if (c.swapPredictedNo) ++C.wholePredNo;
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
    // Fill copies can be elided across frames by the frontend; without a retry signal their failed
    // first allocation cannot recover here. Keep those on the existing path for this experiment.
    const bool guard = CopyAllocationGuard() && !c.fill;
    auto failedTarget = [&] {
        dplan::g_valid = false; // No copy scene happened: use conservative depth hints for the remaining frame.
        static unsigned failureNotes = 0;
        if (failureNotes < 4) {
            ++failureNotes;
            autoq::Log("GXV_RT_ALLOC_GUARD: copy texture %u (%dx%d) skipped: no replacement target; previous capture kept when available\n", c.texId, w, h);
        }
        if (c.srcTex) { BindFbo(g_efbFbo); g_rsValid = false; return; }
        if (c.passMore && c.fromPass && g_pass >= 0) { g_rsValid = false; return; }
        EndPass();
        Clear(c.clear, c.src);
    };
    if (c.dstTexW) {
        // Into a rectangle of an atlas texture.
        const int tw = Rnd(c.dstTexW * sx), th = Rnd(c.dstTexH * sy);
        if (AcquireCopyTarget(c.texId, tw, th, c.dstTexW, c.dstTexH, guard) < 0) { failedTarget(); return; }
        SetViewport(Rnd(c.dstX * sx), Rnd(c.dstY * sy), w, h);
    } else {
        if (AcquireCopyTarget(c.texId, w, h, c.dstW, c.dstH, guard) < 0) { failedTarget(); return; }
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
        if (gpuscenes::g_mode >= 2) gpuscenes::NoteClear(true, false); // (GXV_GPU_SCENES=2)
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
        if (gpuscenes::g_mode >= 2) gpuscenes::NoteClear(true, false); // (GXV_GPU_SCENES=2)
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
        // (GXV_EDGE_AA: the whole picture again, smoothed, over the sharpened one; the character pass only where it did not draw)
        if ((panels == 1 || pass >= 1) && !PresentEdgeAA(fbo, x, y, w, h, 0, dy, pw, g_capPH, amount, false))
            PresentCharaAA(fbo, x, y, w, h, 0, dy, pw, g_capPH, amount, false);
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
            bool sceneTransition = false;
            {
#if VGX_COLDTRACE
            coldtrace::Scope presentTrace(coldtrace::g_recorder,coldtrace::Key{g_progFrame,0x10003u},coldtrace::Origin::Present);
#endif
            EfbForRead();
            XferFence(); // (tex11: the present ends the EFB's scene)
            g_charaDisp = &c; // (PresentCharaAA, called by the hook: this copy's rectangles)
            sceneTransition = g_presentHook(g_efbFbo, x, y, w, h);
            g_charaDisp = nullptr;
            }
            // Whatever the hook did to the context: nothing cached is trusted any more,
            // and the EFB is the target again.
            Invalidate();
            g_rsValid = false;
            g_inUseFbo = ~0u; // (the hook may alter the target; only its result certifies a new scene)
            dplan::g_afterHook = sceneTransition;
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
//   GXV_AUTOQ_LEVEL=<rung>  a fixed rung, 0-8 (testing, no controller): 0 544x388, 1 592x424, 2 640x456, 3 720x480,
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
bool EnsureFxDepthProgram(); // (defined with the effects, below)
// The half-size step's two programs, built before the step first turns on (a program build is milliseconds on the console: not
// in the picture that has just missed its time). Without the effects' depth texture they are not needed (the step then draws as before).
void WarmFxPrograms() {
    static bool done = false;
    if (done) return;
    done = true;
    EnsureFxDepthProgram();
    EnsureCopyProgram();
}
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
    // (the game's copies of whole pictures that it keeps across frames take a new one: the frozen pause picture, GameScene.cpp;
    // told of every attempt, even one refused for memory below: a copy taken again at the same size costs one picture)
    if (port_render_size_changed) port_render_size_changed();
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
    const int targetFps = autoq::TargetFps(); // 30: Quality mode, 60: Performance mode, 0: the GPU-only controller
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
    if (targetFps) {
        cfg.targetFps = targetFps;
        cfg.budgetMs = 1000.0 / targetFps;
        cfg.minRung = autoq::kEmergency;
    }
    cfg.minRung = static_cast<int>(EnvInt("GXV_AUTOQ_MIN", cfg.minRung));
    cfg.maxRung = std::min(static_cast<int>(EnvInt("GXV_AUTOQ_MAX", cfg.maxRung)), TopRung());
    // GXV_AUTOQ_START=<rung>: the rung a run starts at (default 2, 640x456). The Settings' "Locked 30" (GXV_FPS_LIMIT=30) starts at
    // 6, the screen's own 960x544: at 30 pictures a second the GPU has twice the time, so the picture is spent on quality; an overload
    // steps down as in any mode (GXV_AUTOQ_OVERLOAD).
    {
        const bool locked30 = EnvInt("GXV_FPS_LIMIT", 0) == 30;
        // GXV_AUTOQ_LOCKED30_FLOOR=<rung> (default 6) / GXV_AUTOQ_LOCKED30_START (default 7): r57 console A/B, lit Observatory at zoom
        // 1.0, Locked 30: 960x544 with 2x multisampling ran at 27.2 fps, 720x404 at 27.3 - the picture rate there is set by the game
        // thread, and the "GPU load" the controller saw (122 %) counted the GPU's waits for that CPU-paced picture rate. So Locked 30
        // starts at 960x544 2x and never goes below 960x544; an overload can still take the multisampling away. 0 = the old floor.
        if (locked30) {
            const long floor = EnvInt("GXV_AUTOQ_LOCKED30_FLOOR", 6);
            if (floor > 0) cfg.minRung = std::max(cfg.minRung, static_cast<int>(std::min<long>(floor, cfg.maxRung)));
        }
        const long start = EnvInt("GXV_AUTOQ_START", locked30 ? EnvInt("GXV_AUTOQ_LOCKED30_START", 7) : cfg.startRung);
        cfg.startRung = std::clamp(static_cast<int>(start), std::max(cfg.minRung, 0), cfg.maxRung);
    }
    cfg.upTarget = static_cast<double>(EnvInt("GXV_AUTOQ_UP", calm ? 70 : 78)) / 100.0;
    // GXV_AUTOQ_RECOVER=1 (default 0; calm mode): the way back up after an overload without a pause or 120 s of room in a row
    // (vgx_autoq.h, "Recovery"). GXV_AUTOQ_RECOVER_SEC=<5..120> (default 20): the room in a row it needs. Read once, here.
    if (calm && EnvInt("GXV_AUTOQ_RECOVER", 0) == 1) {
        cfg.recover = true;
        cfg.recoverHoldSec = static_cast<double>(std::clamp<long>(EnvInt("GXV_AUTOQ_RECOVER_SEC", 20), 5, 120));
    }
    // (testing, the emulator: at a few frames a second every frame is a "hitch" or a "load" to the controller, which then
    // never decides. GXV_AUTOQ_HITCH_MS / _GAP_MS / _SCENE_MS raise those limits: 100 / 250 / 1500 ms by default.)
    cfg.outlierMs = static_cast<double>(EnvInt("GXV_AUTOQ_HITCH_MS", 100));
    cfg.gapResetMs = static_cast<double>(EnvInt("GXV_AUTOQ_GAP_MS", 250));
    cfg.longGapMs = static_cast<double>(EnvInt("GXV_AUTOQ_SCENE_MS", 1500));
    // GXV_AUTOQ_SPACING=<frames>: two changes at least this many frames apart (default 20; the Strikers port, 160c3f9)
    cfg.minChangeFrames = static_cast<int>(EnvInt("GXV_AUTOQ_SPACING", 20));
    // GXV_AUTOQ_FXSTEP=0|1|2 (default 0; target 30 or 60 only): floor protection by degrees (vgx_autoq.h). 1: at 640x456, a GPU-limited miss
    // halves the effects and blur passes (GXV_FXHALF / GXV_PASSHALF as a runtime state), before any smaller picture; 2: first tries
    // cheaper mip filtering on eligible background draws, then half-size effects. GPU-timed trials; recovery is the reverse.
    // 3: the cheaper background only, never half-size effects (the console, 1 Nov: at the Observatory half-size effects added ten
    // render-target scenes and ten copy quads a picture, the renderer's CPU rose from 15 to 24 ms a picture and the picture rate
    // did not rise; the step costs more than it saves wherever the renderer or the game is the limit).
    if (const long steps = targetFps ? EnvInt("GXV_AUTOQ_FXSTEP", 0) : 0; steps >= 1) {
        const bool fxStatic = EnvInt("GXV_FXHALF", 0) != 0 && EnvInt("GXV_PASSHALF", 0) != 0;
        cfg.fxStep = !fxStatic && steps != 3;
        cfg.bgStep = steps >= 2;
    }
    // GXV_AUTOQ_EDGESTEP=1 (default 0; target 30 or 60, with GXV_EDGE_AA on; the Settings' Performance mode sets it): the present's
    // whole-picture edge smoothing is the first step at 640x456's rung under GPU load (before the cheaper background), the last one back
    // (vgx_autoq.h "Performance mode"). It needs no new render target and no program (the present without it is the start-up one).
    cfg.edgeStep = targetFps && EnvInt("GXV_AUTOQ_EDGESTEP", 0) != 0 && EnvInt("GXV_EDGE_AA", 0) != 0;
    // GXV_AUTOQ_FRONT=-1|0|1 (default -1): the front end's CPU a picture (stamped at each display copy by the player thread) as a stage of the
    // whole-picture work in the decisions: target 60 only / never / in target 30 too. The floor census counts it whenever it is measured.
    cfg.frontStage = static_cast<int>(std::clamp<long>(EnvInt("GXV_AUTOQ_FRONT", -1), -1, 1));
    // GXV_AUTOQ_ANISO=0|1|2|3 (default 0; target 30 or 60 only): anisotropic filtering as the top of the ladder (vgx_autoq.h): on when the GPU
    // has room at the highest rung, the first thing given up under load (before the multisampled rungs), on again last. 1 or 2: the
    // switch's filter value 2; 3: its value 3 (which of the two vitaGL's GXM minification filters is the anisotropic one is
    // to be seen on a console). Needs a vitaGL built with ANISO_SWITCH (the host hands the switch over, SetAnisotropicHook), and is
    // moot when the start-up GXV_ANISO already has it on for good.
    if (const long an = targetFps ? EnvInt("GXV_AUTOQ_ANISO", 0) : 0; an > 0) {
        if (EnvInt("GXV_ANISO", 0) >= 2) autoq::Log("[autoq] GXV_AUTOQ_ANISO ignored: GXV_ANISO already has anisotropic filtering on for good");
        else if (!g_anisoHook) autoq::Log("[autoq] GXV_AUTOQ_ANISO ignored: this vitaGL has no anisotropic switch");
        else {
            cfg.aniso = true;
            g_anisoFilter = an >= 3 ? 3u : 2u;
        }
    }
    // Play-time quality (vgx_autoq.h; target 30 or 60 only, calm mode for the wipe rule; each default off here, on in the Vita's entry.cpp):
    //   GXV_AUTOQ_FREE=<percent>   a step up (rung or anisotropic filtering) keeps the GPU's predicted busy time within this share of the
    //                              picture interval the CPU stages set, and above today's rung a GPU that sets the pace steps back (50-100; 0 off)
    //   GXV_AUTOQ_WIPE=1           fades, wipes and loads: no step up there, a rung above 640x456 back to it while the screen is closed
    //   GXV_AUTOQ_LATEROOM=1       a late picture whose own work fits does not end a stretch of room (the front end's hitches)
    //   GXV_AUTOQ_ANISO_RUNG=<r>   anisotropic filtering (GXV_AUTOQ_ANISO) may be on from rung r up (default: the top rung only)
    if (targetFps) {
        if (const long fr = EnvInt("GXV_AUTOQ_FREE", 0); fr > 0) cfg.freeFrac = static_cast<double>(std::clamp<long>(fr, 50, 100)) / 100.0;
        // (Locked 30, the quality mode, keeps its picture size across fades and loads: GXV_AUTOQ_WIPE_LOCKED30=1 applies the rule there too)
        cfg.newStage = calm && EnvInt("GXV_AUTOQ_NEWSTAGE", 1) != 0;  // (default on: r73, a tour kept Beach Bowl's small picture in every later stage)
        cfg.wipe = calm && EnvInt("GXV_AUTOQ_WIPE", 0) != 0 && (EnvInt("GXV_FPS_LIMIT", 0) != 30 || EnvInt("GXV_AUTOQ_WIPE_LOCKED30", 0) != 0);
        cfg.lateRoom = EnvInt("GXV_AUTOQ_LATEROOM", 0) != 0;
        if (const long ar = EnvInt("GXV_AUTOQ_ANISO_RUNG", -9); cfg.aniso && ar >= 0) cfg.anisoRung = static_cast<int>(std::min<long>(ar, autoq::kRungs - 1));
        // GXV_AUTOQ_OVERLOAD=0|1|2 (default 0; on in the Vita's entry.cpp): a severe GPU overload in play is acted on within about a
        // second, in one change (vgx_autoq.h "Severe overload in play"); slow pictures that are the GPU's work are measured. 2: such an
        // overload may also take half-size effects and blur when GXV_AUTOQ_FXSTEP leaves them out.
        if (const long ov = EnvInt("GXV_AUTOQ_OVERLOAD", 0); ov > 0) {
            cfg.overload = true;
            cfg.overloadFx = ov >= 2 && !(EnvInt("GXV_FXHALF", 0) != 0 && EnvInt("GXV_PASSHALF", 0) != 0);
        }
        // GXV_AUTOQ_HOLD30=1|0 (default 1): Quality mode judged by the pictures (vgx_autoq.h, Config::hold30). Only at target 30 under the
        // fixed 30 cap (GXV_FPS_LIMIT=30, the driver able to hold it) with a floor below 960x544, i.e. the Settings' Quality; Locked 30
        // (floor 960x544) and Performance (target 60) keep their rules. r82: the lit Observatory held 147-151 of 150 pictures at 960x544
        // with the GPU busy 32.5 ms, yet one ~102 % window took it to 784x440 for good. 0: the GPU-busy rules as before.
        cfg.hold30 = targetFps == 30 && EnvInt("GXV_FPS_LIMIT", 0) == 30 && cfg.minRung < autoq::kTopSingle &&
                     (!port_vgx_vita_fixed_cap_supported || port_vgx_vita_fixed_cap_supported() != 0) && EnvInt("GXV_AUTOQ_HOLD30", 1) != 0;
        // GXV_AUTOQ_CADENCE_UP=1|0 (default 1; target 60 only): the way back on Performance mode's steady 30 (vgx_autoq.h,
        // Config::cadenceUp). r98, Beach Bowl: the mode went 720x404 -> 608x348 chasing 60, then the steady 30 engaged (GPU 24-28 ms, none
        // late) and it stayed at 608x348 with the cheaper background for good, the same 30 as Quality at 720x404, because every way back is
        // judged against 16.7 ms. With it a window on the steady 30 climbs (to 720x404 at most) and takes its steps off against 33.3 ms,
        // only where 60 is out of reach at any size: the reach is the steady 30's own release (GXV_STEADY30_ABOVE, 45 a second, read and
        // bounded as port/perf.cpp does, so the two agree). 0: the old decisions.
        cfg.cadenceUp = targetFps == 60 && EnvInt("GXV_AUTOQ_CADENCE_UP", 1) != 0;
        if (cfg.cadenceUp) {
            const long below = std::clamp<long>(EnvInt("GXV_STEADY30_BELOW", static_cast<long>(port::pace::kSteady30BelowFps)), 20, 58);
            const long above = std::clamp<long>(EnvInt("GXV_STEADY30_ABOVE", static_cast<long>(port::pace::kSteady30AboveFps)), 31, 60);
            cfg.cadenceReachFps = std::max(static_cast<double>(above), static_cast<double>(below) + 3.0);
        }
        // GXV_AUTOQ_EDGECHECK=1|0 (default 1; target 60 with GXV_AUTOQ_EDGESTEP): the edge step's trial over 4 s, kept for a 3 % fall of
        // the GPU's time (vgx_autoq.h, Config::edgeCheck). Good Egg, 720x404 pinned: 20.9 -> 18.4 ms of GPU a picture without the present's
        // edge smoothing, yet its 2 s trial in play read only 5 % through the camera's noise and undid it. 0: checkSec and betterFrac as
        // before.
        cfg.edgeCheck = targetFps == 60 && cfg.edgeStep && EnvInt("GXV_AUTOQ_EDGECHECK", 1) != 0;
    }
    if (!autoq::clock::Start()) {
        autoq::Log("[autoq] off: no GPU clock here, the EFB's own size");
        return;
    }
    g_autoq = std::make_unique<autoq::Controller>(cfg);
    const autoq::Config& c = g_autoq->Cfg();
    SetRungFactors(g_autoq->Current());
    if (c.fxStep || c.bgStep || (c.edgeStep && !c.hold30))
        autoq::Log("[autoq] GXV_AUTOQ_FXSTEP%s: at 640x456, a GPU-limited miss first takes %s%s%s%s%s before any smaller picture (GPU-timed trials; the way back is the reverse)",
                   c.edgeStep && !c.hold30 ? "/EDGESTEP" : "", c.edgeStep && !c.hold30 ? "the present's edge smoothing off" : "",
                   c.edgeStep && !c.hold30 && (c.bgStep || c.fxStep) ? ", then " : "",
                   c.bgStep ? "cheaper background mip filtering" : "", c.fxStep && c.bgStep ? ", then " : "", c.fxStep ? "half-size effects and blur" : "");
    if (c.aniso) {
        char top[32];
        RungText(c.maxRung, top, sizeof top);
        if (c.anisoRung == autoq::kNoRung)
            autoq::Log("[autoq] GXV_AUTOQ_ANISO: anisotropic filtering (filter %u) at rung %d (%s) after %.0f s of room, the first thing given up under load, on again last",
                       g_anisoFilter, c.maxRung, top, c.anisoHoldSec);
        else {
            char low[32];
            RungText(g_autoq->AnisoMinRung(), low, sizeof low);
            autoq::Log("[autoq] GXV_AUTOQ_ANISO: anisotropic filtering (filter %u) at rungs %d (%s) to %d (%s) after %.0f s of room (GXV_AUTOQ_ANISO_RUNG), "
                       "the first thing given up under load, off at any change of size", g_anisoFilter, g_autoq->AnisoMinRung(), low, c.maxRung, top, c.anisoHoldSec);
        }
    }
    if (c.freeFrac > 0.0)
        autoq::Log("[autoq] GXV_AUTOQ_FREE: a step up only while the GPU stays within %.0f %% of the picture interval (quality never costs pictures); "
                   "above 640x456 a GPU busy %.0f %% of it for %.0f s steps back", c.freeFrac * 100.0, c.freeDownFrac * 100.0, c.freeDownSec);
    if (c.wipe) autoq::Log("[autoq] GXV_AUTOQ_WIPE: fades, wipes and loads lead into unmeasured areas: no step up there, above 640x456 back to it while the screen is closed");
    if (c.overload)
        autoq::Log("[autoq] GXV_AUTOQ_OVERLOAD: pictures whose GPU time each reaches the budget for %.1f s with a mean of %.0f %% of it take what the GPU "
                   "time says is needed at once (%s%ssmaller sizes to %d); slow pictures that are the GPU's work are measured", c.overloadSec, c.overloadFrac * 100.0,
                   c.bgStep ? "cheaper background, " : "", c.fxStep || c.overloadFx ? "half-size effects, " : "", c.minRung);
    if (c.lateRoom) autoq::Log("[autoq] GXV_AUTOQ_LATEROOM: a late picture whose own work fits (the front end's hitches) does not end a stretch of room");
    if (c.hold30)
        autoq::Log("[autoq] GXV_AUTOQ_HOLD30: Quality judged by the pictures: a smaller size only after %d windows of %.0f s in a row with more than %.0f %% of the pictures "
                   "late for the GPU (late for the game's CPU never costs size; GPU busy %% alone never does), one size at a time; below the start a trial one size up "
                   "after %d windows with every picture on time and the GPU within %.0f %% of the budget, back and blocked if it loses pictures within %.0f s",
                   c.holdMissWindows, c.holdWindowSec, c.holdMissFrac * 100.0, c.holdClimbWindows, c.holdClimbGpuFrac * 100.0, c.holdTrialSec);
    if (c.hold30 && c.edgeStep)
        autoq::Log("[autoq] GXV_AUTOQ_HOLD30/EDGESTEP: such a miss first takes the present's edge smoothing off at the size it happens (before any step or "
                   "smaller size); on again after the size, when every picture came on time and the GPU plus its measured saving fits, as a trial (blocked "
                   "60 s doubling after a failed one)");
    if (c.cadenceUp)
        autoq::Log("[autoq] GXV_AUTOQ_CADENCE_UP: on the steady 30, with 60 out of reach at any size (floor rung's GPU or the other stages "
                   "at %.1f ms or more, under %.0f a second), sizes up to 720x404 and the quality steps come back against 33.3 ms (within "
                   "%.0f %%, the cadence held, none late for the GPU); a climb that then loses pictures to the GPU goes back within %.0f s "
                   "and that size is blocked", 1000.0 / c.cadenceReachFps, c.cadenceReachFps, c.cadenceUpFrac * 100.0, c.holdTrialSec);
    if (c.edgeCheck)
        autoq::Log("[autoq] GXV_AUTOQ_EDGECHECK: the edge step's trial is judged over %.0f s and kept when the GPU's time fell by %.1f %% "
                   "or more",
                   c.edgeCheckSec, 100.0 * std::min(0.5 * (1.0 - 1.0 / std::max(c.edgeRatio, 1.0)), c.edgeBetterFrac));
    if (targetFps == 30) autoq::Log("[autoq] target 30: whole-picture work (two game turns, renderer CPU, GPU busy; overlapping stages use max); emergency floor 480x340 after a sustained GPU miss at 544x388");
    if (targetFps == 60)
        autoq::Log("[autoq] target 60 (Performance mode): as close to 60 pictures a second as the GPU allows; whole-picture work (one game turn, front end CPU, "
                   "renderer CPU, GPU busy; overlapping stages use max) against 16.7 ms; only a GPU-limited miss lowers quality (edge smoothing, cheaper background "
                   "and effects first, then sizes to rung %d), and a series of smaller sizes is kept only when it shows more pictures; a CPU-limited scene keeps its "
                   "size (under the pacing's steady 30 it is judged against 33.3 ms)", cfg.minRung);
    char lo[32], hi[32], st[32];
    RungText(c.minRung, lo, sizeof lo);
    RungText(c.maxRung, hi, sizeof hi);
    RungText(g_rung, st, sizeof st);
    char calmText[224] = "";
    if (c.calm)
        std::snprintf(calmText, sizeof calmText, "; calm mode: changes of size in quiet moments (pause menu, closed fades), in play only for an overload (or after 120 s of room), "
                      "against a %.0f ms frame budget; each stage keeps its rung", c.budgetMs);
    autoq::Log("[autoq] on: rungs %d (%s) to %d (%s), starting at %d (%s); up when the predicted GPU load is <= %.0f %% for %.1f s%s", c.minRung, lo, c.maxRung, hi,
               g_rung, st, c.upTarget * 100.0, c.upHoldSec, calmText);
    if (c.recover) autoq::Log("[autoq] GXV_AUTOQ_RECOVER: a step up in play after %.0f s of room in a row, a single frame ends that stretch only when it missed the budget, blocks' doubling forgotten after %.0f s", c.recoverHoldSec, c.blockForgetSec);
    if (c.calm && (!port_screen_quiet || !port_scene_key))
        autoq::Log("[autoq] calm mode without %s%s from the game", port_screen_quiet ? "" : "quiet moments ", port_scene_key ? "" : "stage keys");
}

// Anisotropic filtering (g_anisoWanted) takes effect at the start of a frame: the switch is called on this thread, between
// frames, and the textures are set again as they are bound (BindTexture). No change, nothing happens.
void AnisoLatch() {
    if (g_aniso == g_anisoWanted) return;
    g_aniso = g_anisoWanted;
    ++g_anisoGen;
    if (g_anisoHook) g_anisoHook(g_aniso ? g_anisoFilter : 0u);
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
    if (g_autoq && g_autoq->Cfg().targetFps != 0)
        autoq::Log("[autoq] target %d work: game %s %.1f ms, renderer CPU %.1f ms, front end CPU %.1f ms, GPU %.1f ms; overlapping maximum %.1f ms against %.3f ms",
                   g_autoq->Cfg().targetFps, g_autoq->Cfg().targetFps == 60 ? "one turn" : "two turns", g_autoq->WindowGameMs(), g_autoq->WindowBackendMs(),
                   g_autoq->WindowFrontMs(), g_autoq->WindowLoad() * g_autoq->Cfg().budgetMs, g_autoq->WindowWorkMs(), g_autoq->Cfg().budgetMs);
    double freeMiB = 0;
    if (FreeVram(freeMiB))
        autoq::Log("[autoq] status: video memory free %.1f MiB (vitaGL's pool); %u retired render textures kept as spares (GXV_KEEP_RT)", freeMiB,
                   static_cast<unsigned>(g_rtSpares.size()));
}

// The floor census (vgx_autoq.h, target 30): the basic log's line every 5 s, so that a run tells exactly why any picture fell under
// 30: the time spent in pictures later than the budget, and which stage was the largest in each: GPU, game CPU (two turns),
// renderer CPU, none of the three (the front end, a wait, the pacing), or not measured.
uint64_t g_floorAt = 0;
const char* RungSizeText(int rung) {
    static char text[32];
    RungText(rung, text, sizeof text);
    return text;
}
void FloorReport() {
    const uint64_t now = autoq::clock::NowUs();
    if (g_floorAt == 0) { g_floorAt = now; g_autoq->TakeFloor(); return; }
    if (now - g_floorAt < 5000000u) return;
    const double window = static_cast<double>(now - g_floorAt) / 1e6;
    g_floorAt = now;
    const autoq::FloorWindow w = g_autoq->TakeFloor();
    char rungText[32];
    RungText(g_rung, rungText, sizeof rungText);
    char steps[160] = "";
    if (g_autoq->Cfg().fxStep || g_autoq->Cfg().bgStep || g_autoq->Cfg().edgeStep)
        std::snprintf(steps, sizeof steps, "; half-size effects %s, cheaper background %s%s", g_autoq->FxStepOn() ? "on" : "off", g_autoq->BgStepOn() ? "on" : "off",
                      g_autoq->Cfg().edgeStep ? (g_autoq->EdgeStepOn() ? ", edge smoothing off" : ", edge smoothing on") : "");
    if (g_autoq->Cfg().aniso) {
        const size_t used = std::strlen(steps);
        std::snprintf(steps + used, sizeof steps - used, "; anisotropic filtering %s", g_autoq->AnisoOn() ? "on" : "off");
    }
    char extra[256] = "";
    if (w.stalls || w.quiet)
        std::snprintf(extra, sizeof extra, "; not counted: %u stalls (%.2f s), %u pause/fade pictures", w.stalls, static_cast<double>(w.stallUs) / 1e6, w.quiet);
    if (w.cadence) {
        // (target 60: pictures the pacing showed on its steady 30, judged against 33.3 ms, GXV_STEADY30)
        const size_t used = std::strlen(extra);
        std::snprintf(extra + used, sizeof extra - used, "; %u on the steady 30", w.cadence);
    }
    if (g_autoq->Windows() != 0) {
        // (the GPU's room in play: its busy time a picture against the picture interval, last decision window; GXV_AUTOQ_FREE reads the same two)
        const size_t used = std::strlen(extra);
        std::snprintf(extra + used, sizeof extra - used, "; last window GPU %.1f ms a picture, a picture every %.1f ms", g_autoq->WindowLoad() * g_autoq->Cfg().budgetMs,
                      g_autoq->WindowIntervalMs());
    }
    if (g_autoq->Cfg().hold30 && g_autoq->HoldWindows() != 0) {
        // (GXV_AUTOQ_HOLD30: the last hold window the decisions read, and the runs of misses / calm windows in a row)
        const autoq::Controller::HoldWindow& h = g_autoq->LastHold();
        const size_t used = std::strlen(extra);
        std::snprintf(extra + used, sizeof extra - used, "; hold30 %u/%u on time, %u late for GPU, runs miss %d calm %d", h.pictures - h.late, h.pictures,
                      h.lateGpu, g_autoq->HoldMissRun(), g_autoq->HoldCalmRun());
    }
    const double late = static_cast<double>(w.lateUs) / 1e6;
    if (w.late == 0) {
        autoq::LogSummary("floor: %.1f s, %u pictures, none under %d; rung %d (%s)%s%s", window, w.pictures, g_autoq->Cfg().targetFps, g_rung, rungText, steps, extra);
        return;
    }
    autoq::LogSummary("floor: %.1f s, %u pictures, %u late (over %.1f ms) = %.2f s below %d, worst %.0f ms; late by GPU %u, game CPU %u, renderer CPU %u, "
                      "front end CPU %u, none of these (waits, pacing) %u, not measured %u; rung %d (%s)%s%s",
                      window, w.pictures, w.late, g_autoq->Cfg().budgetMs * 1.03, late, g_autoq->Cfg().targetFps, static_cast<double>(w.worstUs) / 1000.0, w.gpu, w.game, w.renderer,
                      w.front, w.other, w.unknown, g_rung, rungText, steps, extra);
}

void AutoQualityFrame() {
    g_stepFx = g_stepFxWanted; // (the steps latch here: a frame is wholly one state)
    g_stepBg = g_stepBgWanted;
    g_stepEdge = g_stepEdgeWanted;
    {
        // GXV_QUIET_FXHALF=1 (default 0, a test switch): the pause menu and closed fades draw effects and blur at half size whatever the
        // controller decided (it does not measure those pictures, so it never steps down for them; the pause screen measured GPU-bound
        // at ~10 FPS on 4 Nov). The way back is immediate: the next picture that is not quiet has the controller's state again.
        static const bool quietFx = EnvInt("GXV_QUIET_FXHALF", 0) != 0;
        if (quietFx && port_screen_quiet && port_screen_quiet() != 0) {
            static bool warmed = false;
            if (!warmed) { WarmFxPrograms(); warmed = true; }
            g_stepFx = true;
        }
    }
    AnisoLatch();
    if (g_autoq && (g_autoq->Cfg().fxStep || g_autoq->Cfg().overloadFx)) WarmFxPrograms();
    AutoQualityStatus();
    if (g_forceRung != autoq::kNoRung) {
        if (g_autoq && g_autoq->Cfg().aniso) g_anisoWanted = false; // (the controller that owned the switch is stopped: back to the plain picture)
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
    // (port_screen_quiet: 1 the pause menu, 2 a fade or wipe that has the screen closed)
    const int quietKind = port_screen_quiet ? port_screen_quiet() : 0;
    const bool quiet = quietKind != 0;
    const bool movie = port_skip_movie_playing && port_skip_movie_playing() != 0;  // (forkB: a movie neither measured nor a step)
    const uint32_t scene = port_scene_key ? port_scene_key() : 0;
    while (autoq::clock::NextDone(f)) {
        ++g_autoqTimed;
        f.quiet = quiet;
        f.movie = movie;
        f.wipe = quietKind == 2;
        f.scene = scene;
        if (g_autoq->Add(f)) want = g_autoq->Current();
    }
    if (g_autoq->Cfg().targetFps != 0) FloorReport();
    if (g_autoq->Cfg().aniso && g_autoq->AnisoOn() != g_anisoWanted) {
        const autoq::Change& ac = g_autoq->LastAnisoChange();
        g_anisoWanted = g_autoq->AnisoOn();
        AnisoLatch(); // (now: the frame about to be drawn is wholly the new state)
        autoq::Log("[autoq] anisotropic filtering now %s at rung %d (%s): %s (GPU %.0f %% of the budget, %.1f ms a picture)", g_aniso ? "on" : "off", g_rung,
                   RungSizeText(g_rung), autoq::WhyText(ac.why), ac.load * 100.0, ac.intervalMs);
    }
    if (double before = 0, after = 0; g_autoq->Cfg().aniso && g_autoq->TakeAnisoMeasure(before, after))
        autoq::Log("[autoq] anisotropic filtering measured at rung %d (%s): GPU %.1f ms a picture before, %.1f ms over the %.0f s after (x%.2f; assumed x%.2f)", g_rung,
                   RungSizeText(g_rung), before, after, g_autoq->Cfg().checkSec, before > 0.0 ? after / before : 0.0, g_autoq->Cfg().anisoRatio);
    if (g_autoq->FxStepOn() != g_stepFxWanted || g_autoq->BgStepOn() != g_stepBgWanted || g_autoq->EdgeStepOn() != g_stepEdgeWanted) {
        const autoq::Change& fc = g_autoq->LastFxChange();
        g_stepFxWanted = g_autoq->FxStepOn();
        g_stepBgWanted = g_autoq->BgStepOn();
        g_stepEdgeWanted = g_autoq->EdgeStepOn();
        g_stepFx = g_stepFxWanted;
        g_stepBg = g_stepBgWanted;
        g_stepEdge = g_stepEdgeWanted;
        autoq::Log("[autoq] quality steps now: half-size effects and blur %s, cheaper background %s%s, at rung %d (%s): %s (GPU %.0f %% of the budget, %.1f ms a picture)",
                   g_stepFx ? "on" : "off", g_stepBg ? "on" : "off", g_autoq->Cfg().edgeStep ? (g_stepEdge ? ", edge smoothing off" : ", edge smoothing on") : "",
                   g_rung, RungSizeText(g_rung), autoq::WhyText(fc.why), fc.load * 100.0, fc.intervalMs);
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
        else if (quiet && (ch.why == autoq::Why::Headroom || ch.why == autoq::Why::CadenceUp))
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

void SetAnisotropicHook(AnisoHook hook) { g_anisoHook = hook; }
void DebugSetAnisotropic(bool on, unsigned filter) {
    g_anisoWanted = on;
    g_anisoFilter = filter == 3 ? 3u : 2u;
}
bool AnisotropicOn() { return g_aniso; }
void DebugSetQualitySteps(bool fxHalf, bool cheapBackground) {
    g_stepFxWanted = fxHalf;
    g_stepBgWanted = cheapBackground;
    if (fxHalf) WarmFxPrograms();
}
void DebugSetEdgeStep(bool off) { g_stepEdgeWanted = off; }
void ForceQualityRung(int rung) { g_forceRung = std::clamp(rung, autoq::TargetEnabled() ? autoq::kEmergency : 0, TopRung()); }
void DebugFailAllocation(int nth) { g_simFailAt = nth; g_simChecks = 0; }
void DebugLastChange(unsigned& resampled, unsigned& plain) { resampled = g_lastResampled; plain = g_lastPlain; }
int QualityRung() { return g_rung; }
void QualityRungSize(int& w, int& h, int& samples) { w = g_rtW; h = g_rtH; samples = g_efbSamples; }

int QualityTopRung() { return TopRung(); }
unsigned DebugEfbSwaps() { return g_efbSwapsTotal; }
unsigned DebugEfbSwapsPartial() { return g_efbSwapsPartialTotal; }
unsigned DebugRawConverts() { return g_rawConvertsTotal; }
unsigned DebugEfbSwapHolds() { return g_efbSwapHoldsTotal; }
unsigned DebugEfbLoadFroms() { return g_efbLoadFromsTotal; }
void SetColorLoadHook(ColorLoadHook hook) { g_colorLoadHook = hook; }
unsigned DebugEfbAdopts() { return g_efbAdoptsTotal; }
const sanitize::Counts& DebugSanitizeCounts() { return g_san; } // (GXV_SANITIZE: tests)
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

namespace { void RawConvertTexture(uint32_t texId); } // (GXV_EFBSWAP2, below)
bool DebugReadTexture(uint32_t texId, std::vector<uint8_t>& rgba, int& width, int& height) {
    RawConvertTexture(texId); // (GXV_EFBSWAP2: what the copy holds, not the EFB's buffer it was made with)
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
    static const char* fsHalf =
        "#ifdef VITAGL\n#define RGBV half3\n#else\n#define RGBV vec3\n#endif\n"
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nvarying vec2 v_uv;\nvoid main() {\n"
        "  vec2 uv = clamp(v_uv, u_lim.xy, u_lim.zw);\n"
        "  vec3 c = texture2D(u_src, uv).rgb;\n"
        "  RGBV b = RGBV(texture2D(u_src, clamp(uv + vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)).rgb) + RGBV(texture2D(u_src, clamp(uv - vec2(u_size.z, 0.0), u_lim.xy, u_lim.zw)).rgb)\n"
        "         + RGBV(texture2D(u_src, clamp(uv + vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)).rgb) + RGBV(texture2D(u_src, clamp(uv - vec2(0.0, u_size.w), u_lim.xy, u_lim.zw)).rgb);\n"
        "  vec3 result = vec3(c) + (vec3(c) - vec3(b) * 0.25) * u_p.z;\n"
        "  gl_FragColor = vec4(result, 1.0);\n}\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "sharpen vertex");
    GLint vok = 0;
    if (v) G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    GLuint prog = 0;
    // Half precision is confined to the positive neighbor sum. Keep the original
    // five-tap program as a single fallback before reverting to the existing blit.
    for (unsigned attempt = 0; vok && attempt != 2; ++attempt) {
        const GLuint f = Compile(GL_FRAGMENT_SHADER, attempt ? fs : fsHalf, "sharpen fragment");
        GLint fok = 0, lok = 0;
        if (f) G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
        if (fok) {
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
        if (f) G(glDeleteShader(f));
        if (lok) break;
        if (prog) G(glDeleteProgram(prog));
        prog = 0;
    }
    if (v) G(glDeleteShader(v));
    if (!prog) {
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

namespace { // (GXV_PRESENT_FAST, "the fast present" below)
int FastEnv();
bool PresentFastDraw(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY, bool edge,
                     unsigned* hudDrawn = nullptr);
} // namespace

bool PresentSharpened(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY) {
    if (w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return true;
    if (FastEnv() && PresentFastDraw(fbo, x, y, w, h, dx, dy, dw, dh, amount, flipY, false)) return true; // (GXV_PRESENT_FAST: in its place)
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
    // GXV_CHARAA_AUTO=1|2 (with GXV_CHARAA off; the front end then notes the player's own draws only: Mario and Luigi). =1: the edge
    // filter runs only while the picture is upscaled from a render size below the EFB's (rungs 1, 0 and the emergency rung), so the
    // player stays crisp exactly when the rest of the picture softens; at the EFB's own size and above, nothing is drawn. =2: it runs
    // whenever the picture is shown larger than it was drawn (the render rectangle smaller than the screen's, so at 640x456 and up to
    // the sizes before 960x544 as well) and the EFB is not multisampled (the check above): the 640x456 picture is upscaled to the
    // screen, and its stair steps are visible at the player.
    static const long autoMode = [] {
        if (EnvInt("GXV_CHARAA", 0) != 0) return 0L;
        const long v = EnvInt("GXV_CHARAA_AUTO", 0);
        return v == 0 ? 0L : (v >= 2 ? 2L : 1L);
    }();
    if (autoMode == 1 && g_rtW >= g_efbW && g_rtH >= g_efbH) return 0;
    if (autoMode == 2 && w >= dw && h >= dh) return 0; // (shown 1:1 or smaller: no upscale steps to smooth)
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
void DebugPxCuts(uint64_t& noop, uint64_t& clearColour, uint64_t& clearDepth, uint64_t& clearWhole, uint64_t& scissorKept) {
    // (totals: the "pxcut:" line's window counts plus what earlier windows logged)
    noop = g_pxTotal.noop + g_pxN.noop;
    clearColour = g_pxTotal.clearColour + g_pxN.clearColour;
    clearDepth = g_pxTotal.clearDepth + g_pxN.clearDepth;
    clearWhole = g_pxTotal.clearWhole + g_pxN.clearWhole;
    scissorKept = g_pxTotal.scissorKept + g_pxN.scissorKept;
}
void CharaAACounters(uint64_t& presents, uint64_t& boxes, uint64_t& pixels, uint64_t& msaaSkips) {
    presents = g_charaStats.presents;
    boxes = g_charaStats.boxes;
    pixels = g_charaStats.pixels;
    msaaSkips = g_charaStats.msaaSkips;
}

// ================================================================ whole-picture edge smoothing (GXV_EDGE_AA)
// (the front end's side, and why: vgx_draw.cpp "whole-picture edge smoothing".) PresentEdgeAA replaces the present itself: one quad
// over the whole display rectangle with the edge filter below, then the HUD's rectangles (CopyDispCmd::hud) drawn again with the
// sharpened present's own program, so that every HUD pixel is exactly what PresentSharpened / the blit would have shown. All of it is
// in the display's own scene: no render target, no scene, no EFB store or load is added, and nothing else of the frame changes.
// The filter: T. Lottes' FXAA 3.11 "console" form (the cheapest FXAA that still finds the edge's direction), as on SGX it is cheapest:
//   - the centre and the four diagonal neighbours half a texel away (bilinear: each reads a 2x2 block, so the five cover the 3x3
//     around the pixel) at coordinates the vertex shader computed, varyings TEXCOORD0-4 (v_uv, v_tex1..4: the semantics the Vita host
//     binds), with no clamp: on the SGX these are non-dependent reads the hardware issues before the program runs;
//   - flat pixels (luma contrast of the five below max(GXV_EDGE_AA_MIN, GXV_EDGE_AA_THRESHOLD x the brightest): FXAA's defaults
//     0.0625 / 0.125) leave right there: the centre, plus the sharpened present's unsharp mask at `amount` from the same four reads
//     (scaled by 4/3: each diagonal read holds a quarter of the centre texel), so at 960x544 (amount 0) exactly the blit's pixel;
//   - edge pixels: the direction from the four diagonals, two reads half a texel along it and two up to four texels along it (clamped
//     to the display copy), the wider blend kept unless its luma leaves the neighbourhood's range (FXAA's two-tap fallback).
// Cost per screen pixel: 5 texture reads and about 20 instructions on flat pixels; 4 more reads (dependent) and about 25 more
// instructions on edge pixels (typically 5-15 % of a game picture). Against the blit (1 read): about +4 reads a pixel over the
// 960x544 screen, 522 240 pixels. The Strikers port's SMOOTH (6 computed reads, 89 instructions by the native compiler) runs on
// every picture there; this is about a third of its instructions. Nothing on any CPU but one draw and a few uniforms per HUD rectangle.
// Skipped (the present as before): the switch off, the copy not marked (CopyDispCmd::edge), the EFB multisampled (rungs 7-8, smooth
// already), or a program that did not build. The programs are built at Init (WarmEdgeAA), never in play: this shader is not in the
// package's compiled cache, so the first start ever compiles it once (libshacccg, behind the loading screen); later starts read it
// from vitaGL's cache on the card.
//   GXV_EDGE_AA_THRESHOLD=<per mille>  the relative edge threshold (default 125; 166 FXAA "quality", 250 fewer edges)
//   GXV_EDGE_AA_MIN=<per mille>        the absolute one (default 63; dark edges below it are left alone)
//   GXV_EDGE_AA_SHARPNESS=<n>          FXAA console's edge sharpness (default 8; 2 softens more, wider)
namespace {
struct EdgeProg {
    GLuint prog = 0;
    bool failed = false;
    GLint rect = -1, size = -1, lim = -1, p = -1, q = -1;
} g_edgeProg;
struct EdgeStats { uint64_t presents = 0, hud = 0, msaaSkips = 0, otherSkips = 0, stepSkips = 0, nativeSkips = 0; } g_edgeStats;
int EdgeEnv() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_EDGE_AA", 0), 0, 3));
    return m;
}
// GXV_EDGE_AA_NATIVE=0 (default 1: as before): no edge smoothing for a display copy shown at its own size (the picture rendered at the
// screen's 960x544, Quality's top rung), so the present there is the plain blit (one read a pixel, about 0.9 ms of the display scene
// at 960x544) instead of the five-read edge pass and the HUD redraw (r61 console, GPU_SCENES: display scene 4.5 ms with edge smoothing,
// 2.3 ms with the three-read plain present; h79full at 960x544: display/other 3D 4.6-4.9 ms a picture). NOT the same picture: the
// edges of the 3D scene keep their staircase at 960x544 (the GPU renders them with one sample a pixel). Upscaled pictures (every
// smaller rung) keep the edge smoothing. Read once.
int EdgeNativeEnv() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_EDGE_AA_NATIVE", 1), 0, 1));
    return m;
}

bool EnsureEdgeProgram() {
    if (g_edgeProg.prog) return true;
    if (g_edgeProg.failed || GlAllocBackoff()) return false;
    static const char* vs =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nuniform vec4 u_size;\n"
        "varying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\nvarying vec2 v_tex3;\nvarying vec2 v_tex4;\n"
        "void main() {\n"
        "  v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w);\n"
        "  vec2 h = u_size.zw * 0.5;\n"
        "  v_tex1 = v_uv - h;\n"                  // (NW: -u -v)
        "  v_tex2 = v_uv + vec2(h.x, -h.y);\n"    // (NE)
        "  v_tex3 = v_uv + vec2(-h.x, h.y);\n"    // (SW)
        "  v_tex4 = v_uv + h;\n"                  // (SE)
        "  gl_Position = vec4(a_pos.xy, 0.0, 1.0);\n}\n";
    // (H / H3: half precision for the colour arithmetic where vitaGL's translator has it, as the sharpened present's fsHalf; the
    // coordinates stay float: half a texel of 960 is below half precision's step near 1.0)
    static const char* fsBody =
        "precision highp float;\nuniform sampler2D u_src;\nuniform vec4 u_size;\nuniform vec4 u_lim;\nuniform vec4 u_p;\nuniform vec4 u_q;\n"
        "varying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\nvarying vec2 v_tex3;\nvarying vec2 v_tex4;\n"
        "H L(H3 c) { return dot(c, H3(0.299, 0.587, 0.114)); }\n"
        "H3 F(vec2 uv) { return H3(texture2D(u_src, clamp(uv, u_lim.xy, u_lim.zw)).rgb); }\n"
        "void main() {\n"
        "  H3 m = H3(texture2D(u_src, v_uv).rgb);\n"
        "  H3 cNW = H3(texture2D(u_src, v_tex1).rgb), cNE = H3(texture2D(u_src, v_tex2).rgb);\n"
        "  H3 cSW = H3(texture2D(u_src, v_tex3).rgb), cSE = H3(texture2D(u_src, v_tex4).rgb);\n"
        "  H lM = L(m), lNW = L(cNW), lNE = L(cNE), lSW = L(cSW), lSE = L(cSE);\n"
        "  H lMin = min(min(lNW, lNE), min(lSW, lSE)), lMax = max(max(lNW, lNE), max(lSW, lSE));\n"
        "  H hi = max(lMax, lM);\n"
        "  H3 o;\n"
        "  if (hi - min(lMin, lM) < max(H(u_p.x), hi * H(u_p.y))) {\n"
        "    o = m + (m - (cNW + cNE + cSW + cSE) * H(0.25)) * H(u_p.z);\n" // (flat: the sharpened present's value)
        "  } else {\n"
        "    vec2 dir = vec2(float((lSW + lSE) - (lNW + lNE)), float((lNW + lSW) - (lNE + lSE)));\n"
        "    dir = dir * inversesqrt(max(dot(dir, dir), 1e-8));\n"
        "    vec2 d1 = dir * u_size.zw * 0.5;\n"
        "    vec2 d2 = clamp(dir / max(min(abs(dir.x), abs(dir.y)) * u_q.x, 1e-4), -2.0, 2.0) * u_size.zw * 2.0;\n"
        "    H3 a = F(v_uv - d1) + F(v_uv + d1);\n"
        "    H3 b = (F(v_uv - d2) + F(v_uv + d2)) * H(0.25) + a * H(0.25);\n"
        "    H lb = L(b);\n"
        "    o = (lb < lMin || lb > lMax) ? a * H(0.5) : b;\n"
        "    if (u_p.w > 0.5) o = mix(o, H3(1.0, 0.0, 0.0), H(0.5));\n" // (GXV_EDGE_AA=2: the pixels it changed)
        "  }\n"
        "  gl_FragColor = vec4(vec3(o), 1.0);\n}\n";
    static const std::string fsHalf = std::string("#ifdef VITAGL\n#define H half\n#define H3 half3\n#else\n#define H float\n#define H3 vec3\n#endif\n") + fsBody;
    static const std::string fsFloat = std::string("#define H float\n#define H3 vec3\n") + fsBody;
    const GLuint v = Compile(GL_VERTEX_SHADER, vs, "edge-aa vertex");
    GLint vok = 0;
    if (v) G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    GLuint prog = 0;
    // (the half form first; the float form as the one fallback, as for the sharpened present)
    for (unsigned attempt = 0; vok && attempt != 2; ++attempt) {
        const GLuint f = Compile(GL_FRAGMENT_SHADER, (attempt ? fsFloat : fsHalf).c_str(), "edge-aa fragment");
        GLint fok = 0, lok = 0;
        if (f) G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
        if (fok) {
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
        if (f) G(glDeleteShader(f));
        if (lok) break;
        if (prog) G(glDeleteProgram(prog));
        prog = 0;
    }
    if (v) G(glDeleteShader(v));
    if (!prog) {
        g_edgeProg.failed = true;
        autoq::Log("[edgeaa] the edge-smoothing shader did not build: pictures are presented as before (GXV_EDGE_AA)");
        return false;
    }
    g_edgeProg.prog = prog;
    UseProgram(prog);
    g_edgeProg.rect = G(glGetUniformLocation(prog, "u_rect"));
    g_edgeProg.size = G(glGetUniformLocation(prog, "u_size"));
    g_edgeProg.lim = G(glGetUniformLocation(prog, "u_lim"));
    g_edgeProg.p = G(glGetUniformLocation(prog, "u_p"));
    g_edgeProg.q = G(glGetUniformLocation(prog, "u_q"));
    G(glUniform1i(glGetUniformLocation(prog, "u_src"), 0));
    return true;
}

// The uv rectangle (u_rect) of the screen pixels X0..X1 x Y0..Y1 inside the present's rectangle (dx, dy, dw, dh) of the display copy
// (x, y, w, h of the EFB target): each pixel centre samples where the full-screen present samples it (as PresentCharaAA's boxes).
void PresentUvRect(int x, int y, int w, int h, int dx, int dy, int dw, int dh, bool flipY, int X0, int Y0, int X1, int Y1, float out[4]) {
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    const float u0 = static_cast<float>(x) / tw, du = static_cast<float>(w) / tw;
    const float v0 = static_cast<float>(y) / th, dv = static_cast<float>(h) / th;
    const float ul = u0 + du * static_cast<float>(X0 - dx) / static_cast<float>(dw), uw = du * static_cast<float>(X1 - X0) / static_cast<float>(dw);
    const float fb = static_cast<float>(Y0 - dy) / static_cast<float>(dh), ft = static_cast<float>(Y1 - dy) / static_cast<float>(dh);
    float vb, vt;
    if (flipY) { vb = v0 + dv * (1.0f - fb); vt = v0 + dv * (1.0f - ft); }
    else { vb = v0 + dv * fb; vt = v0 + dv * ft; }
    out[0] = ul; out[1] = vb; out[2] = uw; out[3] = vt - vb;
}
} // namespace

bool WarmPresentFast(); // (GXV_PRESENT_FAST, below)
void WarmEdgeAA() {
    const bool fastOk = WarmPresentFast(); // (GXV_PRESENT_FAST: its programs; the filter below is then only its fallback)
    if (EdgeEnv() == 0 || (FastEnv() && fastOk)) return;
    const double t0 = NowMs();
    const bool ok = EnsureEdgeProgram() && EnsureSharpenProgram();
    autoq::Log("[edgeaa] GXV_EDGE_AA=%d: whole-picture edge smoothing at the present%s; programs %s in %.1f ms (at start-up, not in play)", EdgeEnv(),
               EdgeEnv() == 2 ? " (changed pixels tinted)" : (EdgeEnv() == 3 ? " (left half of the screen only)" : ""), ok ? "built" : "NOT built", NowMs() - t0);
}

bool PresentEdgeAA(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY) {
    const int mode = EdgeEnv();
    const CopyDispCmd* d = g_charaDisp;
    if (mode == 0 || !d || !d->edge || w <= 0 || h <= 0 || dw <= 0 || dh <= 0) return false;
    if (g_efbSamples > 1) { ++g_edgeStats.msaaSkips; return false; } // (rungs 7-8: the whole picture is multisampled)
    if (g_stepEdge) { ++g_edgeStats.stepSkips; return false; }      // (GXV_AUTOQ_EDGESTEP: the controller has it off under GPU load)
    if (EdgeNativeEnv() == 0 && dw == w && dh == h) { ++g_edgeStats.nativeSkips; return false; } // (GXV_EDGE_AA_NATIVE=0: 1:1, the blit)
    if (FastEnv()) { // (GXV_PRESENT_FAST: the one-pass present with its edge term, the HUD drawn again plainly; this filter if it did not build)
        unsigned hud = 0;
        if (PresentFastDraw(fbo, x, y, w, h, dx, dy, dw, dh, amount, flipY, true, &hud)) {
            g_edgeStats.hud += hud;
            ++g_edgeStats.presents;
            return true;
        }
    }
    if (!EnsureEdgeProgram() || !EnsureSharpenProgram()) { ++g_edgeStats.otherSkips; return false; } // (nothing of the GL state touched)
    // Thresholds (read once; see above)
    static const float thr = static_cast<float>(std::clamp<long>(EnvInt("GXV_EDGE_AA_THRESHOLD", 125), 20, 500)) / 1000.0f;
    static const float thrMin = static_cast<float>(std::clamp<long>(EnvInt("GXV_EDGE_AA_MIN", 63), 0, 500)) / 1000.0f;
    static const float sharp = static_cast<float>(std::clamp<long>(EnvInt("GXV_EDGE_AA_SHARPNESS", 8), 1, 16));
    // The display's scene on `fbo`, begun here as PresentSharpened begins it (so it is counted and tagged as the display's: GXV_GPU_SCENES
    // gives this pass's GPU time as the display scene's 3D time). Called in place of PresentSharpened, at the same point of the hook.
    BindFbo(fbo);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    BindTex(0, g_efbColor);
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
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    const float lim[4] = {(static_cast<float>(x) + 0.5f) / tw, (static_cast<float>(y) + 0.5f) / th, (static_cast<float>(x + w) - 0.5f) / tw,
                          (static_cast<float>(y + h) - 0.5f) / th};
    float rect[4];
    // 1. The filter over the picture (=3: its left half; the right half as PresentSharpened draws it, below with the HUD).
    const int splitX = mode == 3 ? dx + dw / 2 : dx + dw;
    UseProgram(g_edgeProg.prog);
    PresentUvRect(x, y, w, h, dx, dy, dw, dh, flipY, dx, dy, splitX, dy + dh, rect);
    SetViewport(dx, dy, splitX - dx, dh);
    G(glUniform4fv(g_edgeProg.rect, 1, rect));
    G(glUniform4f(g_edgeProg.size, tw, th, 1.0f / tw, 1.0f / th));
    G(glUniform4fv(g_edgeProg.lim, 1, lim));
    G(glUniform4f(g_edgeProg.p, thrMin, thr, amount * (4.0f / 3.0f), mode == 2 ? 1.0f : 0.0f));
    G(glUniform4f(g_edgeProg.q, sharp, 0.0f, 0.0f, 0.0f));
    C.uniformCalls += 5;
    C.uniformBytes += 80;
    TouchTarget();
    G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    // 2. The HUD (and =3's right half) as the sharpened present draws it: the same program and uniforms as PresentSharpened, the
    // viewport and uv rectangle of each piece (outwards to whole screen pixels, inside the present's rectangle).
    UseProgram(g_sharp.prog);
    G(glUniform4f(g_sharp.size, tw, th, 1.0f / tw, 1.0f / th));
    G(glUniform4fv(g_sharp.lim, 1, lim));
    G(glUniform4f(g_sharp.p, static_cast<float>(dw) / static_cast<float>(w), static_cast<float>(dh) / static_cast<float>(h), amount, 0.0f));
    C.uniformCalls += 3;
    C.uniformBytes += 48;
    auto plain = [&](int X0, int Y0, int X1, int Y1) {
        X0 = std::max(X0, dx); Y0 = std::max(Y0, dy);
        X1 = std::min(X1, dx + dw); Y1 = std::min(Y1, dy + dh);
        if (X1 <= X0 || Y1 <= Y0) return;
        PresentUvRect(x, y, w, h, dx, dy, dw, dh, flipY, X0, Y0, X1, Y1, rect);
        SetViewport(X0, Y0, X1 - X0, Y1 - Y0);
        G(glUniform4fv(g_sharp.rect, 1, rect));
        ++C.uniformCalls;
        TouchTarget();
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    };
    if (splitX < dx + dw) plain(splitX, dy, dx + dw, dy + dh);
    const float sx = ScaleX(), sy = ScaleY();
    const float kx = static_cast<float>(dw) / static_cast<float>(w), ky = static_cast<float>(dh) / static_cast<float>(h);
    for (int i = 0; i < d->hudN && i < kCharaBoxes; ++i) {
        const EfbRect& b = d->hud[i];
        // EFB pixels -> render-target pixels -> the screen's pixels (outwards)
        const float rx0 = b.x * sx - static_cast<float>(x), rx1 = (b.x + b.w) * sx - static_cast<float>(x);
        const float ry0 = b.y * sy - static_cast<float>(y), ry1 = (b.y + b.h) * sy - static_cast<float>(y);
        const int X0 = dx + static_cast<int>(std::floor(rx0 * kx)), X1 = dx + static_cast<int>(std::ceil(rx1 * kx));
        int Y0, Y1;
        if (flipY) { Y0 = dy + dh - static_cast<int>(std::ceil(ry1 * ky)); Y1 = dy + dh - static_cast<int>(std::floor(ry0 * ky)); }
        else { Y0 = dy + static_cast<int>(std::floor(ry0 * ky)); Y1 = dy + static_cast<int>(std::ceil(ry1 * ky)); }
        plain(X0, Y0, std::min(X1, splitX), Y1); // (=3: what lies in the right half is drawn plainly already)
        ++g_edgeStats.hud;
    }
    g_uniDirtyV = g_uniDirtyF = false;
    ++g_edgeStats.presents;
    return true;
}
void EdgeAACounters(uint64_t& presents, uint64_t& hudRects, uint64_t& msaaSkips, uint64_t& otherSkips) {
    presents = g_edgeStats.presents;
    hudRects = g_edgeStats.hud;
    msaaSkips = g_edgeStats.msaaSkips;
    otherSkips = g_edgeStats.otherSkips + g_edgeStats.stepSkips + g_edgeStats.nativeSkips; // (GXV_AUTOQ_EDGESTEP's and GXV_EDGE_AA_NATIVE's too)
}

// ================================================================ the fast present (GXV_PRESENT_FAST)
// One pass from the display copy to the screen that does the upscale, the unsharp mask and (GXV_EDGE_AA) the edge smoothing together, in
// place of PresentSharpened's program and of PresentEdgeAA's FXAA program. Console (r59, GXV_GPU_SCENES=1, lit Observatory, 720x404 shown
// at 960x544), display scene 3D: blit 0.9 ms, PresentSharpened 5.3 ms, PresentEdgeAA 8.1 ms (7.8 ms even at 960x544 1:1: so not the
// source's size nor its texture cache, but the work per screen pixel). Why, per screen pixel (hand counts from the Cg that vitaGL's
// translator makes of the GLSL: there is no Vita shader compiler here):
//   - the SGX543MP4 at 222 MHz: 4 cores x 4 USSE pipes, one instruction a clock per pipe, so each instruction over the 522 240 screen
//     pixels costs about 0.15 ms; the blit (one read the hardware issues before the program runs, about two instructions) is the 0.9 ms
//     floor of a full-screen pass (rasterising, iterating, the read, the tile store);
//   - PresentSharpened: every read is DEPENDENT: each coordinate is clamped (min + max on float2) and four are offset in the fragment
//     program, so the USSE computes them, issues five sample instructions and waits on them (with float registers few pixels are in flight
//     to hide that); all maths in float32 (vitaGL defines highp / mediump / lowp away: every "float" is 32 bits): about 30 instructions
//     -> 0.9 + 30 x 0.15 = 5.4 ms, as measured;
//   - PresentEdgeAA: its five first reads are non-dependent, but each float result is converted to half (its half form), five luma dot
//     products, eight min / max, the threshold, and a branch whose edge side (direction, rsqrt, two clamped coordinates, four DEPENDENT
//     reads, two more lumas) the USSE runs for a whole group of pixels when any of them takes it: edges are spread over the picture, so
//     most groups pay it; about 45-50 instructions a pixel -> 7.8-8.1 ms, as measured. Not the HUD redraw (tens of thousands of pixels),
//     not a second pass (there is none), not the source's memory (the blit reads the same texture in 0.9 ms).
// The fast present therefore: every read at a coordinate the vertex program computed (non-dependent: no clamp; the rectangle gives up one
// texel on a side that is not the texture's own edge, so no read leaves the display copy), no branch, luma = green (FXAA's
// FXAA_GREEN_AS_LUMA), the colour maths in half (vitaGL's translator keeps Cg's half) with half uniforms (no conversion per pixel).
// Programs (u_k.x = the unsharp amount, as PresentSharpened's):
//   plain3  three reads: the centre c and two half a texel off along one diagonal (each a 2x2 average through the bilinear filter: the
//           Strikers port's MSC_VITA_UPSCALE=fast); o = c + (c - (a + b) / 2) * amount. Along either axis the mask's response is
//           PresentSharpened's (0 at DC, 1/2 at half Nyquist, 1 at Nyquist); along one diagonal it is weaker. About 4 instructions
//           (a+b, c-m, c+d*k, pack). Estimate 0.9 + 0.6 = about 1.5 ms.
//   plain5  five reads: c and its four neighbours one texel away (left, right, up, down): PresentSharpened's own mask, the same value
//           (within half precision), without its clamps and dependent reads. About 6 instructions; estimate about 1.8 ms.
//   edge5   the same five reads and mask, plus the edge term: eh = |l - r|, ev = |u - n| (green); the strength
//           w = saturate((eh + ev - T) / R) x saturate(|ev - eh| x 8 + diag); the direction td = saturate((ev - eh) x 4 + 0.5) (1: a
//           horizontal edge); the pair ALONG the edge m = mix(u + n, l + r, td); the result moves from the plain present's value
//           sp = c + (c - mean4) * amount towards (2c + m) / 4 (a [1 2 1] filter along the edge) by w. Along the edge, never across it:
//           at 720x404 -> 960x544 the bilinear upscale has already softened every edge across it, and the host test shows that a blur
//           across (a 3x3 tent on edge pixels, the first form of this kernel) moves the edges FURTHER from the ideal picture. So an
//           edge whose direction the cross cannot tell (a diagonal: eh = ev) is left alone when upscaled (diag 0) and blended at the
//           screen's own size (diag 1, 960x544, where nothing has softened it). About 18 instructions: sums 3, mask 1, eh / ev / dd 3,
//           w 5, td 1-2, m 2, sp 1, target 2, blend 2, pack 1; no dependent read, no branch. Estimate 0.9 + 18 x 0.15 = about 3.6 ms with
//           the pessimistic model above, about 2-2.5 ms if (as Sony's 28.8 GFLOPS figure implies) a vec3 / vec4 costs one instruction
//           whatever the precision.
// GXV_PRESENT_FAST=1: plain3 where no edge smoothing is asked (GXV_EDGE_AA off; a copy not marked; the HUD), edge5 with GXV_EDGE_AA.
// GXV_PRESENT_FAST=2: plain5 there instead (the picture exactly as today's sharpened present: the A/B that isolates the cost of
//                     dependent reads, clamps and float maths), edge5 with GXV_EDGE_AA.
// The HUD is left as it is (as PresentEdgeAA does): its rectangles (CopyDispCmd::hud) are drawn again with the plain program, so a HUD
// pixel is what the fast present without edge smoothing shows. The programs are built at Init (WarmEdgeAA -> WarmPresentFast) behind the
// loading screen, never in play: they are not in the package's cache, so the first start ever compiles them once (libshacccg) and
// vitaGL's cache keeps them.
//   GXV_PRESENT_FAST=0|1|2             default 0: the present as before (PresentSharpened / PresentEdgeAA / the blit)
//   GXV_PRESENT_FAST_EDGE=<per mille>  T, the edge measure (green levels / 255) where smoothing starts (default 100)
//   GXV_PRESENT_FAST_RAMP=<per mille>  R, from T to full smoothing (default 150)
namespace {
int FastEnv() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_PRESENT_FAST", 0), 0, 2));
    return m;
}
enum FastKind { kFastPlain3, kFastPlain5, kFastEdge5, kFastEdge5Tint, kFastKinds };
struct FastProg {
    GLuint prog = 0;
    bool failed = false, half = false;
    GLint rect = -1, size = -1, p = -1, k = -1;
} g_fastProgs[kFastKinds];
struct FastStats { uint64_t presents = 0, smoothed = 0, hud = 0; } g_fastStats;

std::string FastFragmentSource(int kind, bool half) {
    const bool five = kind != kFastPlain3, edge = kind >= kFastEdge5;
    std::string s = half ? "#ifdef VITAGL\n#define H half\n#define H3 half3\n#define H4 half4\n#define SAT(x) saturate(x)\n#else\n" : "";
    s += "#define H float\n#define H3 vec3\n#define H4 vec4\n#define SAT(x) clamp(x, 0.0, 1.0)\n";
    if (half) s += "#endif\n";
    s += "precision highp float;\nuniform sampler2D u_src;\nuniform H4 u_k;\n";
    if (edge) s += "uniform H4 u_p;\n";
    s += five ? "varying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\nvarying vec2 v_tex3;\nvarying vec2 v_tex4;\n"
              : "varying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\n";
    s += "void main() {\n"
         "  H3 c = H3(texture2D(u_src, v_uv).rgb);\n";
    if (!five) {
        s += "  H3 a = H3(texture2D(u_src, v_tex1).rgb);\n"
             "  H3 b = H3(texture2D(u_src, v_tex2).rgb);\n"
             "  H3 d = c - (a + b) * H(0.5);\n" // (c minus 7 of the 3x3 through the filter)
             "  gl_FragColor = vec4(vec3(c + d * u_k.x), 1.0);\n}\n";
        return s;
    }
    s += "  H3 l = H3(texture2D(u_src, v_tex1).rgb);\n"
         "  H3 r = H3(texture2D(u_src, v_tex2).rgb);\n"
         "  H3 u = H3(texture2D(u_src, v_tex3).rgb);\n"
         "  H3 n = H3(texture2D(u_src, v_tex4).rgb);\n"
         "  H3 sh = l + r;\n"
         "  H3 sv = u + n;\n"
         "  H3 d = c - (sh + sv) * H(0.25);\n"; // (PresentSharpened's mask)
    if (!edge) {
        s += "  gl_FragColor = vec4(vec3(c + d * u_k.x), 1.0);\n}\n";
        return s;
    }
    // w: 0 flat .. 1 edge (above); td: 1 for a horizontal edge (the vertical pair differs), 0 for a vertical one; m: the pair along the edge;
    // the result moves from sp, the plain present's value, to (2c + m) / 4 by w. (No identifier s or t: vitaGL's translator header
    // #defines them as swizzles.)
    s += "  H eh = abs(l.g - r.g);\n"
         "  H ev = abs(u.g - n.g);\n"
         "  H dd = ev - eh;\n"
         "  H w = SAT((eh + ev) * u_p.x + u_p.y) * SAT(abs(dd) * u_p.z + u_p.w);\n"
         "  H td = SAT(dd * H(4.0) + H(0.5));\n"
         "  H3 m = mix(sv, sh, td);\n"
         "  H3 sp = c + d * u_k.x;\n"
         "  H3 o = sp + ((c * H(0.5) + m * H(0.25)) - sp) * w;\n";
    if (kind == kFastEdge5Tint) s += "  o = mix(o, H3(1.0, 0.0, 0.0), w * H(0.5));\n"; // (GXV_EDGE_AA=2: what it changed)
    s += "  gl_FragColor = vec4(vec3(o), 1.0);\n}\n";
    return s;
}

FastProg* EnsureFastProgram(int kind) {
    FastProg& fp = g_fastProgs[kind];
    if (fp.prog) return &fp;
    if (fp.failed || GlAllocBackoff()) return nullptr;
    // (v_tex1..4: TEXCOORD1-4, the names the Vita host binds; u_size.zw: the reads' offset, set per program)
    static const char* vs3 =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nuniform vec4 u_size;\nvarying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\n"
        "void main() {\n"
        "  v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w);\n"
        "  v_tex1 = v_uv + u_size.zw;\n  v_tex2 = v_uv - u_size.zw;\n"
        "  gl_Position = vec4(a_pos.xy, 0.0, 1.0);\n}\n";
    static const char* vs5 =
        "attribute vec4 a_pos;\nuniform vec4 u_rect;\nuniform vec4 u_size;\nvarying vec2 v_uv;\nvarying vec2 v_tex1;\nvarying vec2 v_tex2;\n"
        "varying vec2 v_tex3;\nvarying vec2 v_tex4;\n"
        "void main() {\n"
        "  v_uv = vec2(u_rect.x + (a_pos.x * 0.5 + 0.5) * u_rect.z, u_rect.y + (a_pos.y * 0.5 + 0.5) * u_rect.w);\n"
        "  v_tex1 = v_uv - vec2(u_size.z, 0.0);\n  v_tex2 = v_uv + vec2(u_size.z, 0.0);\n"
        "  v_tex3 = v_uv - vec2(0.0, u_size.w);\n  v_tex4 = v_uv + vec2(0.0, u_size.w);\n"
        "  gl_Position = vec4(a_pos.xy, 0.0, 1.0);\n}\n";
    const GLuint v = Compile(GL_VERTEX_SHADER, kind == kFastPlain3 ? vs3 : vs5, "fast-present vertex");
    GLint vok = 0;
    if (v) G(glGetShaderiv(v, GL_COMPILE_STATUS, &vok));
    GLuint prog = 0;
    bool half = false;
    // (the half form first; the float form as the one fallback, as for the sharpened present)
    for (unsigned attempt = 0; vok && attempt != 2; ++attempt) {
        const std::string text = FastFragmentSource(kind, attempt == 0);
        const GLuint f = Compile(GL_FRAGMENT_SHADER, text.c_str(), "fast-present fragment");
        GLint fok = 0, lok = 0;
        if (f) G(glGetShaderiv(f, GL_COMPILE_STATUS, &fok));
        if (fok) {
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
        if (f) G(glDeleteShader(f));
        if (lok) { half = attempt == 0; break; }
        if (prog) G(glDeleteProgram(prog));
        prog = 0;
    }
    if (v) G(glDeleteShader(v));
    if (!prog) {
        fp.failed = true;
        static const char* const kName[kFastKinds] = {"plain (3 reads)", "plain (5 reads)", "edge", "edge (tinted)"};
        autoq::Log("[present] the fast present's %s program did not build: pictures are presented as before (GXV_PRESENT_FAST)", kName[kind]);
        return nullptr;
    }
    fp.prog = prog;
    fp.half = half;
    UseProgram(prog);
    fp.rect = G(glGetUniformLocation(prog, "u_rect"));
    fp.size = G(glGetUniformLocation(prog, "u_size"));
    fp.p = G(glGetUniformLocation(prog, "u_p"));
    fp.k = G(glGetUniformLocation(prog, "u_k"));
    G(glUniform1i(glGetUniformLocation(prog, "u_src"), 0));
    return &fp;
}
int FastPlainKind() { return FastEnv() == 2 ? kFastPlain5 : kFastPlain3; }
int FastEdgeKind() { return EdgeEnv() == 2 ? kFastEdge5Tint : kFastEdge5; }

// ---- GXV_EDGE_AA_HUDCUT: the edge pass of the fast present leaves the HUD's rectangles out (exact; default 0)
// The fast present with edge smoothing draws the edge program over the whole screen and then the HUD's rectangles again with the plain
// program, so every HUD pixel is shaded twice and the first time for nothing (the plain pass overwrites it). With the switch the edge
// pass covers the screen minus the HUD rectangles instead (the complement cut into horizontal bands, vertically equal neighbours joined),
// and the plain pass draws the rectangles as before: each screen pixel is written by the same program at the same texture coordinates
// as before (PresentUvRect maps every piece as the whole quad), so the picture is the same. Saves the edge program's work (5 reads and
// about 18 instructions a pixel) on the HUD's area: 960x544 with HUD rectangles covering f of the screen, about f x 3.6 ms (f = 5 %:
// ~0.2 ms). Only with GXV_PRESENT_FAST (PresentFastDraw) and GXV_EDGE_AA=1/2; more than kHudPieces pieces: the whole quad, as before.
//   GXV_EDGE_AA_HUDCUT=0|1   default 0 (the integrator's A/B; exact)
bool HudCutEnv() {
    static const bool on = EnvInt("GXV_EDGE_AA_HUDCUT", 0) != 0;
    return on;
}
constexpr int kHudPieces = 48;
struct HudPieces { int n = 0; int r[kHudPieces][4]; long hudPx = 0; };
struct HudCutStats { uint64_t presents = 0, pieces = 0, hudPx = 0; } g_hudCutStats;
// The screen rectangles (X0, Y0, X1, Y1) of the display rectangle (dx, dy, dw, dh) that no HUD rectangle of d covers, each HUD rectangle
// taken exactly as PresentFastDraw's plain pass draws it (outward to whole screen pixels, cut to the display rectangle). False: no HUD
// rectangle, or too many pieces (the caller draws the whole quad).
bool HudComplement(const CopyDispCmd* d, int x, int y, int w, int h, int dx, int dy, int dw, int dh, bool flipY, HudPieces& out) {
    out.n = 0;
    out.hudPx = 0;
    if (!d || d->hudN <= 0 || w <= 0 || h <= 0) return false;
    int hr[kCharaBoxes][4], nh = 0;
    const float sx = ScaleX(), sy = ScaleY();
    const float kx = static_cast<float>(dw) / static_cast<float>(w), ky = static_cast<float>(dh) / static_cast<float>(h);
    for (int i = 0; i < d->hudN && i < kCharaBoxes; ++i) {
        const EfbRect& hb = d->hud[i];
        // (the same arithmetic as the plain pass below)
        const float rx0 = hb.x * sx - static_cast<float>(x), rx1 = (hb.x + hb.w) * sx - static_cast<float>(x);
        const float ry0 = hb.y * sy - static_cast<float>(y), ry1 = (hb.y + hb.h) * sy - static_cast<float>(y);
        int X0 = dx + static_cast<int>(std::floor(rx0 * kx)), X1 = dx + static_cast<int>(std::ceil(rx1 * kx));
        int Y0, Y1;
        if (flipY) { Y0 = dy + dh - static_cast<int>(std::ceil(ry1 * ky)); Y1 = dy + dh - static_cast<int>(std::floor(ry0 * ky)); }
        else { Y0 = dy + static_cast<int>(std::floor(ry0 * ky)); Y1 = dy + static_cast<int>(std::ceil(ry1 * ky)); }
        X0 = std::max(X0, dx); Y0 = std::max(Y0, dy);
        X1 = std::min(X1, dx + dw); Y1 = std::min(Y1, dy + dh);
        if (X1 <= X0 || Y1 <= Y0) continue;
        hr[nh][0] = X0; hr[nh][1] = Y0; hr[nh][2] = X1; hr[nh][3] = Y1;
        ++nh;
    }
    if (nh == 0) return false;
    // Horizontal bands between all the rectangles' top and bottom rows.
    int ys[2 * kCharaBoxes + 2], ny = 0;
    ys[ny++] = dy;
    ys[ny++] = dy + dh;
    for (int i = 0; i < nh; ++i) { ys[ny++] = hr[i][1]; ys[ny++] = hr[i][3]; }
    std::sort(ys, ys + ny);
    ny = static_cast<int>(std::unique(ys, ys + ny) - ys);
    int open[kHudPieces]; // (pieces that end at the current band's top: candidates to grow downwards)
    int nOpen = 0;
    for (int b = 0; b + 1 < ny; ++b) {
        const int ya = ys[b], yb = ys[b + 1];
        if (ya < dy || yb > dy + dh || yb <= ya) continue;
        // The x intervals the HUD covers in this band (rectangles spanning all of it), sorted and merged.
        struct Iv { int x0, x1; } iv[kCharaBoxes];
        int ni = 0;
        for (int i = 0; i < nh; ++i)
            if (hr[i][1] <= ya && hr[i][3] >= yb) iv[ni++] = Iv{hr[i][0], hr[i][2]};
        std::sort(iv, iv + ni, [](const Iv& a, const Iv& c) { return a.x0 < c.x0; });
        int nextOpen[kHudPieces], nNext = 0;
        int cx = dx;
        auto gap = [&](int g0, int g1) {
            if (g1 <= g0) return true;
            for (int k = 0; k < nOpen; ++k) {
                int* p = out.r[open[k]];
                if (p[0] == g0 && p[2] == g1 && p[3] == ya) { p[3] = yb; nextOpen[nNext++] = open[k]; return true; }
            }
            if (out.n >= kHudPieces) return false;
            out.r[out.n][0] = g0; out.r[out.n][1] = ya; out.r[out.n][2] = g1; out.r[out.n][3] = yb;
            nextOpen[nNext++] = out.n++;
            return true;
        };
        for (int k = 0; k < ni; ++k) {
            if (iv[k].x1 <= cx) continue;
            if (iv[k].x0 > cx && !gap(cx, iv[k].x0)) return false;
            out.hudPx += static_cast<long>(iv[k].x1 - std::max(cx, iv[k].x0)) * (yb - ya);
            cx = iv[k].x1;
        }
        if (!gap(cx, dx + dw)) return false;
        std::copy(nextOpen, nextOpen + nNext, open);
        nOpen = nNext;
    }
    return true;
}

// The fast present of the display copy (x, y, w, h of the EFB target) into (dx, dy, dw, dh) of `fbo`, in the display's own scene. edge:
// with the edge term (the caller has checked GXV_EDGE_AA, the copy's mark and the samples), then the HUD rectangles (g_charaDisp->hud)
// drawn again plainly (their number added to *hudDrawn). False: nothing drawn, no GL state touched (a program did not build; the caller
// presents as before).
bool PresentFastDraw(unsigned fbo, int x, int y, int w, int h, int dx, int dy, int dw, int dh, float amount, bool flipY, bool edge, unsigned* hudDrawn) {
    if (FastEnv() == 0 || w < 4 || h < 4 || dw <= 0 || dh <= 0) return false;
    const int edgeMode = edge ? EdgeEnv() : 0;
    FastProg* plain = EnsureFastProgram(FastPlainKind());
    FastProg* smooth = edgeMode ? EnsureFastProgram(FastEdgeKind()) : nullptr;
    if (!plain || (edgeMode && !smooth)) return false; // (nothing of the GL state touched)
    static const float thr = static_cast<float>(std::clamp<long>(EnvInt("GXV_PRESENT_FAST_EDGE", 100), 0, 900)) / 1000.0f;
    static const float ramp = static_cast<float>(std::clamp<long>(EnvInt("GXV_PRESENT_FAST_RAMP", 150), 10, 1000)) / 1000.0f;
    // No read may leave the display copy, and there is no clamp per pixel: on a side where the copy is not at the texture's edge (where
    // the sampler's clamp-to-edge does it) the rectangle gives up one texel (a 0.3 % zoom on that axis; normally no side does).
    const int l = x > 0 ? 1 : 0, r = x + w < g_rtW ? 1 : 0, b = y > 0 ? 1 : 0, t = y + h < g_rtH ? 1 : 0;
    const int xi = x + l, yi = y + b, wi = w - l - r, hi = h - b - t;
    BindFbo(fbo);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    BindTex(0, g_efbColor);
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
    const float tw = static_cast<float>(g_rtW), th = static_cast<float>(g_rtH);
    float rect[4];
    auto setup = [&](FastProg& fp, bool three) {
        UseProgram(fp.prog);
        // (three reads: half a texel along the diagonal; five: one texel along each axis)
        const float o = three ? 0.5f : 1.0f;
        G(glUniform4f(fp.size, tw, th, o / tw, o / th));
        G(glUniform4f(fp.k, amount, 0.0f, 0.0f, 0.0f));
        C.uniformCalls += 2;
        C.uniformBytes += 32;
        if (fp.p >= 0) {
            // w = saturate((eh + ev - thr) / ramp) x saturate(|ev - eh| x 8 + diag): an edge whose direction the cross cannot tell (a
            // diagonal: eh = ev) is smoothed only when the picture is shown at its own size (diag 1); upscaled (diag 0) the bilinear
            // filter has smoothed it already and a blend across it only moves it away from the ideal picture (the host test)
            const float diag = std::fabs(static_cast<float>(dw) / static_cast<float>(w) - 1.0f) < 0.02f ? 1.0f : 0.0f;
            G(glUniform4f(fp.p, 1.0f / ramp, -thr / ramp, 8.0f, diag));
            ++C.uniformCalls;
            C.uniformBytes += 16;
        }
    };
    auto quad = [&](FastProg& fp, int X0, int Y0, int X1, int Y1) {
        X0 = std::max(X0, dx); Y0 = std::max(Y0, dy);
        X1 = std::min(X1, dx + dw); Y1 = std::min(Y1, dy + dh);
        if (X1 <= X0 || Y1 <= Y0) return;
        PresentUvRect(xi, yi, wi, hi, dx, dy, dw, dh, flipY, X0, Y0, X1, Y1, rect);
        SetViewport(X0, Y0, X1 - X0, Y1 - Y0);
        G(glUniform4fv(fp.rect, 1, rect));
        ++C.uniformCalls;
        TouchTarget();
        G(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    };
    // 1. The picture with the edge term (=3: its left half).
    const int splitX = edgeMode == 3 ? dx + dw / 2 : dx + dw;
    if (smooth) {
        setup(*smooth, false);
        // (GXV_EDGE_AA_HUDCUT: only the screen outside the HUD rectangles, which step 2 draws plainly over whatever step 1 left)
        HudPieces cut;
        if (edgeMode != 3 && HudCutEnv() && HudComplement(g_charaDisp, x, y, w, h, dx, dy, dw, dh, flipY, cut)) {
            for (int i = 0; i < cut.n; ++i) quad(*smooth, cut.r[i][0], cut.r[i][1], cut.r[i][2], cut.r[i][3]);
            g_hudCutStats.pieces += static_cast<uint64_t>(cut.n);
            g_hudCutStats.hudPx += static_cast<uint64_t>(cut.hudPx);
            if (++g_hudCutStats.presents % 1800 == 0) // (about once a minute at 30 pictures a second)
                autoq::Log("[present] GXV_EDGE_AA_HUDCUT: %llu presents, %.1f edge pieces and %.1f%% of the screen (HUD) left to the plain pass a present",
                           static_cast<unsigned long long>(g_hudCutStats.presents), static_cast<double>(g_hudCutStats.pieces) / g_hudCutStats.presents,
                           100.0 * static_cast<double>(g_hudCutStats.hudPx) / (static_cast<double>(g_hudCutStats.presents) * dw * dh));
        } else {
            quad(*smooth, dx, dy, splitX, dy + dh);
        }
    }
    // 2. Plainly: the whole picture without edge smoothing; or (=3) the right half, and the HUD rectangles.
    setup(*plain, FastPlainKind() == kFastPlain3);
    if (!smooth) quad(*plain, dx, dy, dx + dw, dy + dh);
    else {
        if (splitX < dx + dw) quad(*plain, splitX, dy, dx + dw, dy + dh);
        const CopyDispCmd* d = g_charaDisp;
        const float sx = ScaleX(), sy = ScaleY();
        const float kx = static_cast<float>(dw) / static_cast<float>(w), ky = static_cast<float>(dh) / static_cast<float>(h);
        for (int i = 0; d && i < d->hudN && i < kCharaBoxes; ++i) {
            const EfbRect& hb = d->hud[i];
            // EFB pixels -> render-target pixels -> the screen's pixels (outwards), as PresentEdgeAA
            const float rx0 = hb.x * sx - static_cast<float>(x), rx1 = (hb.x + hb.w) * sx - static_cast<float>(x);
            const float ry0 = hb.y * sy - static_cast<float>(y), ry1 = (hb.y + hb.h) * sy - static_cast<float>(y);
            const int X0 = dx + static_cast<int>(std::floor(rx0 * kx)), X1 = dx + static_cast<int>(std::ceil(rx1 * kx));
            int Y0, Y1;
            if (flipY) { Y0 = dy + dh - static_cast<int>(std::ceil(ry1 * ky)); Y1 = dy + dh - static_cast<int>(std::floor(ry0 * ky)); }
            else { Y0 = dy + static_cast<int>(std::floor(ry0 * ky)); Y1 = dy + static_cast<int>(std::ceil(ry1 * ky)); }
            quad(*plain, X0, Y0, std::min(X1, splitX), Y1); // (=3: the right half is plain already)
            ++g_fastStats.hud;
            if (hudDrawn) ++*hudDrawn;
        }
        ++g_fastStats.smoothed;
    }
    g_uniDirtyV = g_uniDirtyF = false;
    ++g_fastStats.presents;
    return true;
}
} // namespace

// Builds the fast present's programs at Init (see above). True when GXV_PRESENT_FAST is off or every program it needs built.
bool WarmPresentFast() {
    if (FastEnv() == 0) return true;
    const double t0 = NowMs();
    const FastProg* plain = EnsureFastProgram(FastPlainKind());
    const FastProg* smooth = EdgeEnv() ? EnsureFastProgram(FastEdgeKind()) : nullptr;
    const bool ok = plain && (!EdgeEnv() || smooth);
    autoq::Log("[present] GXV_PRESENT_FAST=%d: one-pass present (non-dependent reads: %d plain, 5 with edge smoothing), edge smoothing %s; "
               "programs %s (%s) in %.1f ms (at start-up, not in play)",
               FastEnv(), FastEnv() == 2 ? 5 : 3, EdgeEnv() ? "on (GXV_EDGE_AA)" : "off", ok ? "built" : "NOT built",
               ok ? (plain->half && (!smooth || smooth->half) ? "half form" : "float form") : "-", NowMs() - t0);
    return ok;
}
void PresentFastCounters(uint64_t& presents, uint64_t& smoothed, uint64_t& hudRects) {
    presents = g_fastStats.presents;
    smoothed = g_fastStats.smoothed;
    hudRects = g_fastStats.hud;
}

bool Init(int renderWidth, int renderHeight) {
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
    g_drawReject=DrawRejectCounters{};
#endif
    g_capacityNextScanSerial = 0;
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
    beprof::Configure(); // (GXV_BE_PROF)
    gpuscenes::g_mode = static_cast<int>(std::clamp<long>(EnvInt("GXV_GPU_SCENES", 0), 0, 2));
    gpuscenes::g_on = gpuscenes::g_mode != 0;
    g_px.noop = EnvInt("GXV_PX_NOOP", 0) != 0;
    g_px.deadClear = EnvInt("GXV_PX_DEADCLEAR", 0) != 0;
    g_px.scissor = EnvInt("GXV_PX_SCISSOR", 0) != 0;
    if ((g_px.noop || g_px.deadClear || g_px.scissor) && port_log_summary)
        port_log_summary("pxcut: GXV_PX_NOOP=%d GXV_PX_DEADCLEAR=%d GXV_PX_SCISSOR=%d (pixel-work cuts that keep the picture)", g_px.noop ? 1 : 0, g_px.deadClear ? 1 : 0,
                         g_px.scissor ? 1 : 0);
#ifdef __VGX_VITA__
    g_midFlushEvery = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_MIDFLUSH", 0), 0, 4096));
    if (g_midFlushEvery && port_log_summary) port_log_summary("GXV_MIDFLUSH=%u: the EFB scene's vertex work goes to the GPU every %u draws", g_midFlushEvery, g_midFlushEvery);
#endif
    g_beFast = EnvInt("GXV_BE_FAST", 1) != 0;
    g_gpuAttrib = static_cast<int>(std::clamp<long>(EnvInt("GXV_GPU_ATTRIB", 0), 0, 2));
    if (g_gpuAttrib && port_log_summary)
        port_log_summary("GXV_GPU_ATTRIB=%d (TEST, the picture is wrong): every EFB draw %s", g_gpuAttrib,
                         g_gpuAttrib == 1 ? "draws only its first triangle (GPU ms = per-draw costs)" : "goes to a 1x1 viewport (GPU ms = per-draw + vertex costs)");
    // GXV_BE_FAST2 (see g_beFast2): the per-draw switches latched here, before the first draw, from the same reads as before
    g_f2.sanMode = SanModeRead();
    g_f2.uniformReuse = UniformReuseRead();
    g_f2.stageWarm = StageWarmOnRead();
    g_f2.audit = resourceaudit::Enabled();
    g_f2.lodPct = RungLodPctRead();
    g_beFast2 = EnvInt("GXV_BE_FAST2", 1) != 0;
#ifdef __VGX_VITA__
    {
        const bool want = EnvInt("GXV_UNI_SHADOW", 0) != 0, have = vglGetUniformShadow && vglUniformShadowWritten;
        g_uniShadow = want && have;
        if (want)
            autoq::Log("[uni-shadow] %s", have ? "on: uniform blocks go into vitaGL's uniform buffers directly, compared first (GXV_UNI_SHADOW)"
                                               : "asked for, but this vitaGL has no vglGetUniformShadow: glUniform* as before");
    }
#endif
    // vitaGL's draw caches (tools/vitagl/patches/0014; a vitaGL without it has no such call and keeps its own behaviour):
    // GXV_VGL_FRAGMEMO=1 (default): each program keeps the fragment programs it was patched for (blend, colour mask, target
    // format, multisampling), so a program drawn alternately with two blends no longer asks the shader patcher on every switch.
    // GXV_VGL_UNIREUSE=1 (default): a program whose uniform values did not change since its last draw points sceGxm at the copy
    // that draw made in the uniform ring instead of copying its whole default uniform buffer again (after every program switch).
    if (vglSetDrawCaches) {
        const uint32_t flags = (EnvInt("GXV_VGL_FRAGMEMO", 1) != 0 ? 1u : 0u) | (EnvInt("GXV_VGL_UNIREUSE", 1) != 0 ? 2u : 0u);
        vglSetDrawCaches(flags);
        autoq::Log("[vgl-cache] fragment-program memo %s, uniform-ring re-use %s (GXV_VGL_FRAGMEMO, GXV_VGL_UNIREUSE)", (flags & 1u) ? "on" : "off",
                (flags & 2u) ? "on" : "off");
    }
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
    if (!CreateTarget() && EfbCreateGuard()) {
        std::fprintf(stderr, "[vgx] back end set-up failed: the EFB render target could not be created\n");
        if (port_log_summary) port_log_summary("[vgx] back end set-up failed: the EFB render target could not be created");
        return false;
    }
    if (!EnsureCopyProgram()) { // compiled here, not at the first copy that needs it
        std::fprintf(stderr, "[vgx] back end set-up failed: the display copy program did not compile or link\n");
        if (port_log_summary) port_log_summary("[vgx] back end set-up failed: the display copy program did not compile or link");
        return false;
    }
    WarmEdgeAA(); // (GXV_EDGE_AA: its shaders built behind the loading screen, never at the first smoothed picture in play)
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
uint64_t g_profSum[80] = {}; // the call census (VGX_GL_COUNTERS) summed over the same frames
static_assert(sizeof(Counts) / sizeof(uint32_t) <= 80, "g_profSum too small");
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
                if (rs.alphaWrite || rs.dstAlphaZero || dstAlpha) g_ev[p0] |= EV_RA;
            }
            // (DrawRec::cover: its first primitive writes every pixel of its box, unblended)
            if (d.cover && d.exX <= 0 && d.exY <= 0 && d.exX + d.exW >= g_efbW && d.exY + d.exH >= g_efbH) {
                if (rs.colorWrite) g_ev[p0] |= EV_FC;
                if (rs.alphaWrite || rs.dstAlphaZero) g_ev[p0] |= EV_FA;
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
            if (rs.alphaWrite || rs.dstAlphaZero || dstAlpha) return false;
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
//   GXV_PASSHALF=2   only the passes INSIDE a chain (the narrow form, below); the quality step (g_stepFx) still takes the form of 1
// ---- GXV_PASSHALF=2: only the blur steps inside a chain at half size
// GameScene's BloomEffect::postDraw (decomp Game/Screen/BloomEffect.cpp; EFB 640x456, render scale s = 1.5 x 1.19 at 960x544), as the
// front end isolates it (tile = a cell of the 4x4 or 8x8 grid of the EFB, ImageEffectLocalUtil::capture's rectangles):
//   EFB -> _28 320x228 (box-halved copy of the glow mask)  [an EFB copy: not a pass]
//   P0 tile 0 160x114: _28 drawn at 1/2 (one bilinear read = the exact 2x2 box)  -> _2C 160x114, _40 I8   reads an EFB copy: KEPT
//   P1 tile 2 160x114: _2C, then x threshold mask _40                            -> _44                     HALVED
//   P2 tile 3 160x114: 6+6-tap ring blur of _44 (r 0.01 / 0.02 of the width)      -> _30, _34 80x57 (box)    HALVED
//   P3 tile 20 80x57:  36-tap ring blur of _34 (r 0.04-0.09)                      -> _38                     HALVED
//   P4 tile 7 160x114: _30 + _38                                                   -> _3C                     its copy is read by the EFB: KEPT
//   EFB: _24 (the saved picture) drawn back, _3C added over the whole screen (full size, as always).
// So the narrow form halves P1-P3 (about 0.11 + 0.42 + 0.32 ms of 0.9 ms of pass fill at 960x544 by the blit-cost model; they become a
// quarter) and keeps P0 (a half-size P0 would read _28 4:1 with one bilinear tap: single glow-mask texels would flicker) and P4 (its
// picture is stretched over the whole screen; at half size the glow would be blockier). BloomEffectSimple has no pass inside its chain
// (its one blur goes straight to the screen): nothing changes there. Why the picture stays nearly the same: every halved pass draws a
// picture that is only ever read by another pass of the chain through bilinear filtering, before more blurring or a 2x-4x upscale to
// the screen; P1 at half size reads _2C / _40 2:1 at texel corners (the exact 2x2 box, no aliasing); the ring blurs' radii (2.4-21 render
// pixels at 960x544) are fixed fractions of the screen, so they cover the same area at either size, and their output carries little above
// the half size's Nyquist frequency; the copies of a halved pass are half size and every reader samples them through the same 0..1
// coordinates. NOT exact (GXV_PASSVERIFY gives the PSNR on the desktop). Rule: a pass is halved when every texture its draws sample is a
// copy this frame makes from a pass (CopyTexCmd::fromPass, or out of an atlas cell), and every copy that ends it (passMore chain) is read,
// before it is written again this frame, only by draws inside passes; plus PASSHALF's own limits (at most a quarter of the EFB, no atlas,
// at least 8x8 pixels after).
bool PassInChain(const CmdBuf& buf, size_t begin) {
    auto fromPassCopy = [&](uint32_t id) { // (the latest copy into id before `begin` was made from a pass)
        int found = -1;
        for (size_t j = 0; j < begin && j < buf.cmds.size(); ++j)
            if (buf.cmds[j].type == CMD_COPY_TEX && buf.copyTex[buf.cmds[j].arg].texId == id) found = static_cast<int>(j);
        if (found < 0) return false;
        const CopyTexCmd& t = buf.copyTex[buf.cmds[static_cast<size_t>(found)].arg];
        return !t.depth && !t.skip && (t.fromPass || t.srcTex != 0);
    };
    size_t end = begin + 1;
    for (; end < buf.cmds.size() && buf.cmds[end].type != CMD_COPY_TEX; ++end) {
        if (buf.cmds[end].type == CMD_PASS_BEGIN) return false;
        if (buf.cmds[end].type != CMD_DRAW) continue;
        const DrawRec& d = buf.draws[buf.cmds[end].arg];
        for (int m = 0; m < 8; ++m)
            if ((d.texMask & (1u << m)) && !fromPassCopy(d.tex[m].texId)) return false;
    }
    if (end >= buf.cmds.size()) return false;
    // The copies that end the pass (the first, and the ones chained to it by passMore) and their readers.
    uint32_t made[8];
    int nMade = 0;
    for (size_t j = end; j < buf.cmds.size() && buf.cmds[j].type == CMD_COPY_TEX; ++j) {
        const CopyTexCmd& t = buf.copyTex[buf.cmds[j].arg];
        if (!t.fromPass || t.depth || t.dstTexW || nMade == 8) return false;
        if (!t.skip) made[nMade++] = t.texId;
        if (!t.passMore) { end = j; break; }
        end = j;
    }
    bool inPass = false;
    for (size_t j = end + 1; j < buf.cmds.size() && nMade > 0; ++j) {
        const Cmd& c = buf.cmds[j];
        if (c.type == CMD_PASS_BEGIN) { inPass = true; continue; }
        if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            for (int k = 0; k < nMade; ++k)
                if (t.srcTex == made[k]) return false; // (an atlas copy out of it: not a pass's draw)
            if (t.fromPass && !t.passMore) inPass = false;
            for (int k = 0; k < nMade; ++k)
                if (t.texId == made[k]) { made[k] = made[--nMade]; break; } // (written again: later readers see another picture)
            continue;
        }
        if (c.type == CMD_TEX_FREE || c.type == CMD_TEX_UPLOAD || c.type == CMD_SET_TARGET || c.type == CMD_COPY_DISP) {
            if (c.type == CMD_SET_TARGET || c.type == CMD_COPY_DISP) inPass = false;
            const uint32_t id = c.type == CMD_TEX_FREE ? c.arg : (c.type == CMD_TEX_UPLOAD ? buf.uploads[c.arg].texId : 0u);
            for (int k = 0; k < nMade; ++k)
                if (id && id == made[k]) { made[k] = made[--nMade]; break; }
            continue;
        }
        if (c.type != CMD_DRAW) continue;
        const DrawRec& d = buf.draws[c.arg];
        for (int m = 0; m < 8; ++m) {
            if (!(d.texMask & (1u << m))) continue;
            for (int k = 0; k < nMade; ++k)
                if (d.tex[m].texId == made[k] && !inPass) return false; // (the EFB reads it: the composite or anything else)
        }
    }
    return true;
}
float PassScaleFor(const CmdBuf& buf, size_t begin, const PassCmd& ps) {
    static const long mode = EnvInt("GXV_PASSHALF", 0);
    const bool on = mode != 0;
    if (!(on || g_stepFx) || GpupassOff(GP_PASSHALF) || ps.atlasTex || ps.crowd.valid()) return 1.0f;
    if (mode == 2 && !g_stepFx && !PassInChain(buf, begin)) return 1.0f; // (GXV_PASSHALF=2: only passes inside a chain)
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
//   GXV_FXHALF_MIN=<vertices>   smallest run that is drawn so (default 400; 0: every run); then the area test (GXV_FXHALF_AREA, below)
int FxDiv() {
    static const int div = [] {
        const long v = EnvInt("GXV_FXHALF", 0);
        return v == 2 || v == 4 ? static_cast<int>(v) : 0;
    }();
    return div ? div : (g_stepFx ? 2 : 0); // (the quality step: half size, when the switch itself is off)
}
bool FxEligible(const DrawRec& d) {
    const RenderState& rs = d.rs;
    return rs.blend && rs.blendEq != BE_REVSUB && rs.blendDst == BF_ONE && (rs.blendSrc == BF_SRC_ALPHA || rs.blendSrc == BF_ONE) && rs.colorWrite &&
           !(rs.depthTest && rs.depthWrite) && !rs.dstAlphaZero && d.idxCount > 0;
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
// ---- GXV_FXHALF_AREA: what a run of effect draws covers (and GXV_FXAREA, the census of it)
// A reduced run is not free: the depth copy (1/div^2 of the picture), the composite (the whole picture, blended), and two more
// scenes of the EFB (its colour and depth stored and loaded again; on the console, 1 Nov, ten such runs a picture took the renderer
// from 15 to 24 ms a picture and the GPU gained nothing). It only pays where the run's own fill is larger than that: a run of
// 400 vertices may be a hundred 4-pixel sparks or a screen-filling glow. So the run's pixels are measured, from its vertices:
// each triangle through the draw's own matrix and projection blocks (u_mv rows and u_proj, as the vertex shader has them),
// clipped against w > 0 and the clip box, scaled by the viewport. Only stream draws with u_mv positions can be measured;
// a big draw is measured on every k-th of its triangles (at most kAreaTris) and scaled. An estimate, for choosing.
//   GXV_FXHALF_AREA=<screens>   a run is drawn at the reduced size only when it covers at least this many screens (default 3;
//                               0: the vertex count alone decides, as before). GXV_FXHALF_MIN=0 ("every run") also turns it off.
//   GXV_FXHALF_RUNS=<n>         with the area test, at most this many runs a picture, the largest (default 2; 0: no limit)
//   GXV_FXAREA=<n>              with GXV_WORKLOAD_PROBE=1: one picture in n is measured for the "workload: fx area" line (default
//                               16; 0: off): the blended draws' coverage, the additive runs' coverage
constexpr uint32_t kAreaTris = 64;
// Polygon clipping in clip space against one plane (d = a plane's signed distance per vertex: dot(plane, v)).
struct ClipV { float x, y, w; };
// axis 0 / 1: the plane w + sign * x (or y) >= 0 (sign -1: x <= w; +1: x >= -w); axis 2: w >= 1e-5 (in front of the eye).
inline int ClipPlane(const ClipV* in, int n, ClipV* out, int axis, float sign) {
    auto dist = [axis, sign](const ClipV& v) { return axis == 2 ? v.w - 1e-5f : v.w + sign * (axis == 0 ? v.x : v.y); };
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const ClipV& a = in[i];
        const ClipV& b = in[(i + 1) % n];
        const float da = dist(a), db = dist(b);
        if (da >= 0.0f) out[m++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            const float t = da / (da - db);
            out[m++] = ClipV{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t};
        }
    }
    return m;
}
// The area of one clip-space triangle inside the clip box, in NDC units (the box is 2 x 2).
inline float ClipTriArea(const ClipV& a, const ClipV& b, const ClipV& c) {
    const auto inside = [](const ClipV& v) { return v.w > 1e-5f && std::fabs(v.x) <= v.w && std::fabs(v.y) <= v.w; };
    ClipV buf0[12], buf1[12];
    int n = 3;
    buf0[0] = a; buf0[1] = b; buf0[2] = c;
    ClipV* poly = buf0;
    if (!(inside(a) && inside(b) && inside(c))) {
        // (all three outside one plane: nothing)
        if (a.w <= 1e-5f && b.w <= 1e-5f && c.w <= 1e-5f) return 0.0f;
        if (a.x > a.w && b.x > b.w && c.x > c.w) return 0.0f;
        if (a.x < -a.w && b.x < -b.w && c.x < -c.w) return 0.0f;
        if (a.y > a.w && b.y > b.w && c.y > c.w) return 0.0f;
        if (a.y < -a.w && b.y < -b.w && c.y < -c.w) return 0.0f;
        ClipV* src = buf0;
        ClipV* dst = buf1;
        static const int axes[5] = {2, 0, 0, 1, 1};
        static const float signs[5] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f};
        for (int k = 0; k < 5 && n >= 3; ++k) {
            n = ClipPlane(src, n, dst, axes[k], signs[k]);
            std::swap(src, dst);
        }
        if (n < 3) return 0.0f;
        poly = src;
    }
    float twice = 0.0f;
    float px = poly[n - 1].x / poly[n - 1].w, py = poly[n - 1].y / poly[n - 1].w;
    for (int i = 0; i < n; ++i) {
        const float x = poly[i].x / poly[i].w, y = poly[i].y / poly[i].w;
        twice += px * y - x * py;
        px = x;
        py = y;
    }
    return std::fabs(twice) * 0.5f;
}
// The pixels (EFB) a stream draw's triangles cover, counted with overlap (fill work); < 0: not known here.
float DrawAreaPx(const CmdBuf& buf, const DrawRec& d) {
    if (d.vbuf || d.prim != PRIM_TRIANGLES || d.idxCount < 3) return -1.0f;
    if (d.vsId >= g_vsSrc.size() || !g_vsSrc[d.vsId] || g_vsSrc[d.vsId]->nMv == 0) return -1.0f; // (positions from a palette, or not known yet)
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV];
    if (pb == kNoBlock || mb == kNoBlock || pb > buf.uni.size() || buf.uni.size() - pb < 16 || mb > buf.uni.size() || buf.uni.size() - mb < 12) return -1.0f;
    if (static_cast<size_t>(d.idxOffset) + d.idxCount > buf.idx.size()) return -1.0f;
    size_t base = d.vtxOffset;
    if (!buf.vtxChunks.empty()) {
        if (d.vchunk >= buf.vtxChunks.size()) return -1.0f;
        base += buf.vtxChunks[d.vchunk];
    }
    const uint32_t stride = MakeLayout(d.layoutMask).stride;
    const float* P = buf.uni.data() + pb; // (column-major)
    const float* M = buf.uni.data() + mb; // (rows: eye = M[r] . (x, y, z, 1))
    // clip = P * (M * p): rows 0 (x), 1 (y), 3 (w) of the product, as a 3x4 matrix.
    float R[3][4];
    static const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; ++i) {
        const int r = rows[i];
        for (int k = 0; k < 4; ++k) R[i][k] = P[0 * 4 + r] * M[0 * 4 + k] + P[1 * 4 + r] * M[1 * 4 + k] + P[2 * 4 + r] * M[2 * 4 + k];
        R[i][3] += P[12 + r];
    }
    const uint8_t* const vtx = buf.vtx.data();
    const size_t vtxSize = buf.vtx.size();
    const uint16_t* const ix = buf.idx.data() + d.idxOffset;
    auto at = [&](uint16_t v, ClipV& o) {
        const size_t off = base + static_cast<size_t>(v) * stride;
        if (off > vtxSize || vtxSize - off < 12) return false;
        float p[3];
        std::memcpy(p, vtx + off, 12);
        o.x = R[0][0] * p[0] + R[0][1] * p[1] + R[0][2] * p[2] + R[0][3];
        o.y = R[1][0] * p[0] + R[1][1] * p[1] + R[1][2] * p[2] + R[1][3];
        o.w = R[2][0] * p[0] + R[2][1] * p[1] + R[2][2] * p[2] + R[2][3];
        return std::isfinite(o.x) && std::isfinite(o.y) && std::isfinite(o.w);
    };
    const uint32_t tris = d.idxCount / 3;
    const uint32_t step = tris > kAreaTris ? (tris + kAreaTris - 1) / kAreaTris : 1;
    float ndc = 0.0f;
    uint32_t measured = 0;
    for (uint32_t t = 0; t < tris; t += step) {
        ClipV a, b, c;
        if (!at(ix[t * 3], a) || !at(ix[t * 3 + 1], b) || !at(ix[t * 3 + 2], c)) return -1.0f;
        ndc += ClipTriArea(a, b, c);
        ++measured;
    }
    if (!measured) return -1.0f;
    // (NDC area 4 is the whole viewport)
    return ndc * (static_cast<float>(tris) / static_cast<float>(measured)) * std::fabs(d.rs.vpW * d.rs.vpH) * 0.25f;
}
// The EFB's pixels: one screen.
inline float ScreenPx() { return static_cast<float>(g_efbW > 0 ? g_efbW : 640) * static_cast<float>(g_efbH > 0 ? g_efbH : 456); }
// ---- GXV_GPU_SCENES=2, the second part: what each scene drew, and the scene slots with the most 3D (see the first part, near the top)
// Per scene (Detail, filled while the back end draws it):
//  - draws by class: depth-only (no colour or alpha written), no effect (nothing written: PxNoEffect), discard (the fragment shader has a
//    `discard`), blended, opaque (the rest); for each the pixels covered, in target pixels: measured (DrawAreaPx: a stream draw's
//    triangles, at most 64 of them, through its own matrices and viewport, overlap counted) or, where the vertices are not at hand
//    (static buffers, palette positions), bounded by the draw's rectangle (the front end's extent, cut to the viewport);
//  - vitaGL's scissor-mask passes: tests.c update_scissor_test runs at the beginning of every scene on a framebuffer object and at every
//    scissor change; with the scissor off it draws the mask over the whole target, with it on a "clear the mask" pass over the whole
//    target (left out when the scene did not load its depth: MASK_SKIP) and the rectangle. Counted as the back end's TouchTarget sees
//    vitaGL's dirty flag (an estimate of what vitaGL does);
//  - glClear quads (vitaGL clears by drawing a quad over the scissor rectangle), copy and pass quads (DrawQuad);
//  - whether its first operation cleared the whole target (then the colour load GXM made at the scene's beginning was dead work) and
//    the depth hint it began with (load / store).
// Per fragment shader, a per-pixel weight from its text, cached by id: statements of its main + 3 x texture reads (a rough count of
// USSE instructions; a `discard` and the TEV stage count are shown with it). A draw's estimated cost is its pixels x that weight.
// The slots: a scene is placed by its number in the back end's picture, its kind and its target size (the picture's structure is the
// same from picture to picture in steady play). Every scene timed cleanly (see Take) adds its TA / 3D / feed and its Detail to its slot;
// every 5 s the eight slots with the most 3D a picture get one line each:
//   gpu-scenes top N: scene <number> <kind> <w>x<h> in P% of pictures: 3D x ms, TA y ms a picture (per scene x' / y'), feed z;
//     D draws (B blended, X discard); <class> n draws m+<=b screens ...; scissor masks ...; clears ...; copy quads ...;
//     depth load L% store S%, colour load dead C%; shaders by est. cost: fs <id> [stages, texture reads, statements, discard]
//     draws, screens, share of the slot's estimated cost ...
// "screens" are the slot's target pixels (w x h): 1.0 = every pixel once. "m+<=b": m measured, plus at most b for the draws known only by
// their rectangles.
namespace gpuscenes {
// (per fragment shader id: bit 31 known, bit 30 discard, bits 16..23 TEV stages, bits 8..15 texture reads (255 = more), 0..7 statements / 2)
uint32_t g_fsInfo[4096];
uint32_t FsInfo(uint16_t fsId) {
    if (fsId < 4096 && (g_fsInfo[fsId] & 0x80000000u)) return g_fsInfo[fsId];
    uint32_t stages = 0, tex = 0, st = 0;
    bool dis = false;
    if (fsId < g_fsSrc.size() && g_fsSrc[fsId]) {
        const std::string& t = g_fsSrc[fsId]->text;
        for (size_t p = t.find("// stage "); p != std::string::npos; p = t.find("// stage ", p + 1)) ++stages;
        for (size_t p = t.find("texture2D("); p != std::string::npos; p = t.find("texture2D(", p + 1)) ++tex;
        const size_t m = t.find("void main");
        for (size_t p = m == std::string::npos ? t.size() : m; p < t.size(); ++p) st += t[p] == ';';
        dis = t.find("discard") != std::string::npos;
    }
    const uint32_t v = 0x80000000u | (dis ? 0x40000000u : 0u) | (std::min<uint32_t>(stages, 255) << 16) | (std::min<uint32_t>(tex, 255) << 8) |
                       std::min<uint32_t>((st + 1) / 2, 255);
    if (fsId < 4096) g_fsInfo[fsId] = v;
    return v;
}
inline float FsWeight(uint32_t info) { return static_cast<float>((info & 0xFF) * 2 + ((info >> 8) & 0xFF) * 3); }
inline Detail* CurDetail() {
    if (!g_cur || g_mode < 2) return nullptr;
    Detail& x = g_detail[g_cur->serial % kDetailRing];
    return x.serial == g_cur->serial ? &x : nullptr;
}
// (Begin) the target's size and a clean record
void BeginDetail(Desc& d) {
    int w = 0, h = 0;
    if (S.fbo == g_efbFbo) { w = g_rtW; h = g_rtH; }
    else if (g_inPass && g_pass >= 0 && static_cast<size_t>(g_pass) < g_pool.size() && g_pool[g_pass].fbo == S.fbo) { w = g_passW; h = g_passH; }
    else if (S.fbo == 0) { w = g_dispW; h = g_dispH; }
    else
        for (const PoolFbo& f : g_pool)
            if (f.fbo == S.fbo) { w = f.w; h = f.h; break; }
    d.w = static_cast<uint16_t>(std::clamp(w, 0, 0xFFFF));
    d.h = static_cast<uint16_t>(std::clamp(h, 0, 0xFFFF));
    Detail& x = g_detail[d.serial % kDetailRing];
    x = Detail{};
    x.serial = d.serial;
}
// The pixels of the bound target inside the GL scissor (target coordinates), or all of them.
inline float ScissoredPx(int x0, int y0, int x1, int y1) {
    const Desc& d = *g_cur;
    if (S.scissorOn == 1) {
        x0 = std::max(x0, S.sc[0]); y0 = std::max(y0, S.sc[1]);
        x1 = std::min(x1, S.sc[0] + S.sc[2]); y1 = std::min(y1, S.sc[1] + S.sc[3]);
    }
    x1 = std::min(x1, static_cast<int>(d.w)); y1 = std::min(y1, static_cast<int>(d.h));
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    return x1 > x0 && y1 > y0 ? static_cast<float>(x1 - x0) * static_cast<float>(y1 - y0) : 0.0f;
}
// (TouchTarget, when vitaGL will rebuild its scissor mask) began: this call began the scene
void NoteMask(bool began) {
    Detail* x = CurDetail();
    if (!x) return;
    const float full = static_cast<float>(g_cur->w) * static_cast<float>(g_cur->h);
    ++x->masks;
    if (S.scissorOn == 1) {
        if (!(began && g_cur->dLoad == 0)) { ++x->maskFull; x->maskPx += full; } // (the "clear the mask" pass; MASK_SKIP leaves it out)
        x->maskPx += ScissoredPx(0, 0, g_cur->w, g_cur->h);
    } else {
        ++x->maskFull;
        x->maskPx += full;
    }
}
// (a clear, after TouchTarget) colour / depth: what it clears
void NoteClear(bool colour, bool depth) {
    Detail* x = CurDetail();
    if (!x) return;
    const float px = ScissoredPx(0, 0, g_cur->w, g_cur->h), full = static_cast<float>(g_cur->w) * static_cast<float>(g_cur->h);
    if (!x->touched && px >= full && full > 0.0f) x->firstClear = static_cast<uint8_t>((colour ? 1 : 0) | (depth ? 2 : 0));
    x->touched = true;
    ++x->clears;
    x->clearPx += px;
}
// (DrawQuad, after TouchTarget) over the viewport
void NoteQuad() {
    Detail* x = CurDetail();
    if (!x) return;
    x->touched = true;
    ++x->quads;
    x->quadPx += ScissoredPx(S.vp[0], S.vp[1], S.vp[0] + S.vp[2], S.vp[1] + S.vp[3]);
}
// (Draw, after its draw call)
void NoteDrawDetail(const CmdBuf& buf, const DrawRec& r) {
    Detail* x = CurDetail();
    if (!x) return;
    x->touched = true;
    const RenderState& rs = r.rs;
    const uint32_t info = FsInfo(r.fsId);
    const uint8_t cls = PxNoEffect(rs) ? kNoEffect : (!rs.colorWrite && !rs.alphaWrite) ? kDepthOnly : (info & 0x40000000u) ? kDiscard : rs.blend ? kBlended : kOpaque;
    const float ps = g_inPass ? g_passScale : 1.0f;
    const float s2 = ScaleX() * ps * ScaleY() * ps;
    float px = DrawAreaPx(buf, r);
    bool bound = false;
    if (px >= 0.0f) {
        px *= s2;
    } else {
        // (the draw's rectangle - the front end's extent, inside its scissor - cut to its viewport)
        const float x0 = std::max(static_cast<float>(r.exX), rs.vpX), y0 = std::max(static_cast<float>(r.exY), rs.vpY);
        const float x1 = std::min(static_cast<float>(r.exX + r.exW), rs.vpX + rs.vpW), y1 = std::min(static_cast<float>(r.exY + r.exH), rs.vpY + rs.vpH);
        px = x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) * s2 : 0.0f;
        bound = true;
    }
    ++x->clsDraws[cls];
    if (bound) { ++x->clsBound[cls]; x->clsBoundPx[cls] += px; }
    else x->clsPx[cls] += px;
    const float cost = px * FsWeight(info);
    int k = 0;
    while (k < x->nSh && x->sh[k].fs != r.fsId) ++k;
    if (k == x->nSh && x->nSh < kSceneShaders) { x->sh[k] = ShaderUse{}; x->sh[k].fs = r.fsId; ++x->nSh; }
    if (k < x->nSh) { ++x->sh[k].draws; x->sh[k].px += px; x->sh[k].cost += cost; }
    else { ++x->otherDraws; x->otherPx += px; x->otherCost += cost; }
}
// ---- the slots of the window
constexpr int kSlotShaders = 16;
struct Slot {
    uint16_t ordinal = 0, w = 0, h = 0;
    uint8_t kind = 0;
    bool tagged = false;
    uint32_t n = 0, withDetail = 0;
    uint64_t taUs = 0, fUs = 0, feedUs = 0, draws = 0, blend = 0, discard = 0;
    uint32_t dLoad = 0, dStore = 0, dKnown = 0, colourDead = 0;
    uint64_t clsDraws[kClasses] = {}, clsBound[kClasses] = {};
    double clsPx[kClasses] = {}, clsBoundPx[kClasses] = {};
    uint64_t masks = 0, maskFull = 0, clears = 0, quads = 0;
    double maskPx = 0, clearPx = 0, quadPx = 0;
    ShaderUse sh[kSlotShaders];
    int nSh = 0;
    uint64_t otherDraws = 0;
    double otherPx = 0, otherCost = 0;
};
constexpr int kSlots = 48; // (the last one takes whatever does not fit)
Slot g_slots[kSlots];
int g_nSlots = 0;
void TakeSlot(const Desc& d, uint32_t ta, uint32_t f, uint32_t feed) {
    const bool tagged = d.serial != 0;
    int i = 0;
    while (i < g_nSlots && !(g_slots[i].tagged == tagged && g_slots[i].ordinal == d.ordinal && g_slots[i].kind == d.kind && g_slots[i].w == d.w && g_slots[i].h == d.h)) ++i;
    if (i == g_nSlots) {
        if (g_nSlots < kSlots) {
            g_slots[i] = Slot{};
            g_slots[i].tagged = tagged; g_slots[i].ordinal = d.ordinal; g_slots[i].kind = d.kind; g_slots[i].w = d.w; g_slots[i].h = d.h;
            ++g_nSlots;
        } else {
            i = kSlots - 1;
        }
    }
    Slot& s = g_slots[i];
    ++s.n;
    s.taUs += ta; s.fUs += f; s.feedUs += feed;
    s.draws += d.draws; s.blend += d.blend; s.discard += d.discard;
    if (d.dLoad >= 0) { ++s.dKnown; s.dLoad += static_cast<uint32_t>(d.dLoad); s.dStore += static_cast<uint32_t>(d.dStore > 0); }
    const Detail& x = g_detail[d.serial % kDetailRing];
    if (!tagged || x.serial != d.serial) return;
    ++s.withDetail;
    s.colourDead += x.firstClear & 1u;
    for (int c = 0; c < kClasses; ++c) {
        s.clsDraws[c] += x.clsDraws[c]; s.clsBound[c] += x.clsBound[c];
        s.clsPx[c] += x.clsPx[c]; s.clsBoundPx[c] += x.clsBoundPx[c];
    }
    s.masks += x.masks; s.maskFull += x.maskFull; s.maskPx += x.maskPx;
    s.clears += x.clears; s.clearPx += x.clearPx; s.quads += x.quads; s.quadPx += x.quadPx;
    for (int k = 0; k < x.nSh; ++k) {
        int j = 0;
        while (j < s.nSh && s.sh[j].fs != x.sh[k].fs) ++j;
        if (j == s.nSh && s.nSh < kSlotShaders) { s.sh[j] = ShaderUse{}; s.sh[j].fs = x.sh[k].fs; ++s.nSh; }
        if (j < s.nSh) { s.sh[j].draws += x.sh[k].draws; s.sh[j].px += x.sh[k].px; s.sh[j].cost += x.sh[k].cost; }
        else { s.otherDraws += x.sh[k].draws; s.otherPx += x.sh[k].px; s.otherCost += x.sh[k].cost; }
    }
    s.otherDraws += x.otherDraws; s.otherPx += x.otherPx; s.otherCost += x.otherCost;
}
void ReportTop(double seconds) {
    static const char* const kName[kKinds] = {"EFB", "texture/pass", "display/other"};
    static const char* const kCls[kClasses] = {"opaque", "blended", "discard", "depth-only", "no effect"};
    const uint64_t pictures = g_timers ? g_gpuPictures : g_cpuPictures; // (no scene timers: every picture executed, no GPU times)
    const double gp = pictures ? static_cast<double>(pictures) : 1.0;
    auto out = [](const char* text) {
        if (port_log_summary) port_log_summary("%s", text);
        else std::fprintf(stderr, "%s\n", text);
    };
    char line[1600];
    double slots3d = 0;
    for (int i = 0; i < g_nSlots; ++i) slots3d += static_cast<double>(g_slots[i].fUs);
    if (g_timers)
        std::snprintf(line, sizeof line, "gpu-scenes top: %.1f s, %llu pictures timed, %d scene slots, 3D %.2f ms a picture in them; scenes after an untimed scene: %.1f a picture, "
                      "TA %.2f, 3D %.2f ms a picture (not in the slots); the slots by 3D a picture follow (pixels in screens of the slot's target, measured from "
                      "vertices + at most the bounded rest; est. cost = pixels x (statements + 3 x texture reads))",
                      seconds, static_cast<unsigned long long>(g_gpuPictures), g_nSlots, slots3d / 1000.0 / gp, g_gapScenes / gp, g_gapTaUs / 1000.0 / gp, g_gapFUs / 1000.0 / gp);
    else
        std::snprintf(line, sizeof line, "gpu-scenes top: %.1f s, %llu pictures, %d scene slots, NO GPU times (this vitaGL has no scene timers): the slots by estimated pixel "
                      "cost follow (pixels in screens of the slot's target, measured from vertices + at most the bounded rest; est. cost = pixels x (statements + 3 x "
                      "texture reads))", seconds, static_cast<unsigned long long>(g_cpuPictures), g_nSlots);
    out(line);
    int order[kSlots];
    double cost[kSlots];
    for (int i = 0; i < g_nSlots; ++i) {
        order[i] = i;
        cost[i] = g_slots[i].otherCost;
        for (int j = 0; j < g_slots[i].nSh; ++j) cost[i] += g_slots[i].sh[j].cost;
    }
    if (g_timers) std::sort(order, order + g_nSlots, [](int a, int b) { return g_slots[a].fUs > g_slots[b].fUs; });
    else std::sort(order, order + g_nSlots, [&cost](int a, int b) { return cost[a] > cost[b]; });
    for (int r = 0; r < g_nSlots && r < 8; ++r) {
        const Slot& s = g_slots[order[r]];
        if (!s.n) continue;
        const double n = static_cast<double>(s.n), nd = s.withDetail ? static_cast<double>(s.withDetail) : 1.0;
        const double scr = s.w && s.h ? static_cast<double>(s.w) * s.h : 1.0;
        int k = 0;
        if (s.tagged && !g_timers)
            k = std::snprintf(line, sizeof line, "gpu-scenes top %d: scene %u %s %ux%u%s in %.0f%% of pictures: %.0f draws (%.0f blended, %.0f discard)", r + 1, s.ordinal,
                              kName[s.kind], s.w, s.h, order[r] == kSlots - 1 && g_nSlots == kSlots ? " (+ others)" : "", 100.0 * n / gp, s.draws / n, s.blend / n, s.discard / n);
        else if (s.tagged)
            k = std::snprintf(line, sizeof line, "gpu-scenes top %d: scene %u %s %ux%u%s in %.0f%% of pictures: 3D %.2f ms, TA %.2f ms a picture (a scene %.2f / %.2f), feed %.2f; "
                              "%.0f draws (%.0f blended, %.0f discard)", r + 1, s.ordinal, kName[s.kind], s.w, s.h, order[r] == kSlots - 1 && g_nSlots == kSlots ? " (+ others)" : "",
                              100.0 * n / gp, s.fUs / 1000.0 / gp, s.taUs / 1000.0 / gp, s.fUs / 1000.0 / n, s.taUs / 1000.0 / n, s.feedUs / 1000.0 / n, s.draws / n, s.blend / n,
                              s.discard / n);
        else
            k = std::snprintf(line, sizeof line, "gpu-scenes top %d: scenes the back end did not begin (the host's) in %.0f%% of pictures: 3D %.2f ms, TA %.2f ms a picture",
                              r + 1, 100.0 * n / gp, s.fUs / 1000.0 / gp, s.taUs / 1000.0 / gp);
        if (s.withDetail) {
            for (int c = 0; c < kClasses && k > 0 && k < static_cast<int>(sizeof line); ++c)
                if (s.clsDraws[c])
                    k += std::snprintf(line + k, sizeof line - k, "; %s %.1f draws %.2f+<=%.2f screens (%.1f bounded)", kCls[c], s.clsDraws[c] / nd, s.clsPx[c] / nd / scr,
                                       s.clsBoundPx[c] / nd / scr, s.clsBound[c] / nd);
            if (k > 0 && k < static_cast<int>(sizeof line))
                k += std::snprintf(line + k, sizeof line - k, "; scissor masks %.1f (%.1f full-target, %.2f screens); clears %.1f (%.2f screens); copy quads %.1f (%.2f screens)",
                                   s.masks / nd, s.maskFull / nd, s.maskPx / nd / scr, s.clears / nd, s.clearPx / nd / scr, s.quads / nd, s.quadPx / nd / scr);
            if (k > 0 && k < static_cast<int>(sizeof line))
                k += std::snprintf(line + k, sizeof line - k, "; depth load %.0f%% store %.0f%%, colour load dead %.0f%%", s.dKnown ? 100.0 * s.dLoad / s.dKnown : 0.0,
                                   s.dKnown ? 100.0 * s.dStore / s.dKnown : 0.0, 100.0 * s.colourDead / nd);
            int top[kSlotShaders];
            for (int j = 0; j < s.nSh; ++j) top[j] = j;
            std::sort(top, top + s.nSh, [&s](int a, int b) { return s.sh[a].cost > s.sh[b].cost; });
            double all = s.otherCost;
            for (int j = 0; j < s.nSh; ++j) all += s.sh[j].cost;
            if (all <= 0) all = 1;
            if (s.nSh && k > 0 && k < static_cast<int>(sizeof line)) k += std::snprintf(line + k, sizeof line - k, "; shaders by est. cost:");
            for (int j = 0; j < s.nSh && j < 5 && k > 0 && k < static_cast<int>(sizeof line); ++j) {
                const ShaderUse& u = s.sh[top[j]];
                const uint32_t info = FsInfo(u.fs);
                k += std::snprintf(line + k, sizeof line - k, " fs %u [%u stages, %u tex, %u st%s] %.1f draws %.2f screens %.0f%%,", u.fs, (info >> 16) & 0xFF, (info >> 8) & 0xFF,
                                   (info & 0xFF) * 2, (info & 0x40000000u) ? ", discard" : "", u.draws / nd, u.px / nd / scr, 100.0 * u.cost / all);
            }
            if (s.otherDraws && k > 0 && k < static_cast<int>(sizeof line))
                k += std::snprintf(line + k, sizeof line - k, " others %.1f draws %.2f screens %.0f%%", s.otherDraws / nd, s.otherPx / nd / scr, 100.0 * s.otherCost / all);
        }
        out(line);
    }
    g_nSlots = 0;
}
} // namespace gpuscenes
// GXV_FXAREA: whether the picture being decided is one the census measures (one in n, with the workload probe on).
bool AreaCensusPicture() {
    if (!workload::Enabled() || g_verify.shadow) return false;
    static const uint32_t every = [] {
        const int savedErrno = errno;
        const long v = EnvInt("GXV_FXAREA", 16);
        errno = savedErrno;
        return v > 0 ? static_cast<uint32_t>(v) : 0u;
    }();
    static uint32_t count = 0;
    return every && (count++ % every) == 0;
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
    // GXV_FXHALF_AREA / GXV_FXHALF_RUNS (above): read once, and only when a run may be reduced (errno as the switches above).
    // GXV_FXHALF_MIN=0 asks for every run: no area test then (the host tests' fixtures are runs of a few pixels).
    static float areaMin = -1.0f;
    static uint32_t maxRuns = 0;
    if (on && areaMin < 0.0f) {
        const int savedErrno = errno;
        const char* e = std::getenv("GXV_FXHALF_AREA");
        const double v = e && *e ? std::strtod(e, nullptr) : 3.0;
        areaMin = minVerts == 0 || !(v > 0.0) ? 0.0f : static_cast<float>(v);
        const long r = EnvInt("GXV_FXHALF_RUNS", 2);
        maxRuns = areaMin > 0.0f && r > 0 ? static_cast<uint32_t>(r) : 0u;
        errno = savedErrno;
    }
    const bool gate = on && areaMin > 0.0f;
    // GXV_FXAREA: whether this picture is measured for the census (decided at its first blended draw, so that a picture without
    // one asks nothing of the workload switch).
    int census = -1;
    uint32_t censusBlend = 0, censusAdd = 0, censusUnknown = 0;
    float runArea = 0.0f;
    bool runAreaKnown = false;
    struct FxRun { size_t start, end; float area; };
    static std::vector<FxRun> runs;
    runs.clear();
    auto close = [&](size_t end) {
        if (runStart != ~static_cast<size_t>(0) && !g_verify.shadow) workload::CandidateRun(runVerts);
        if (runStart != ~static_cast<size_t>(0) && census == 1 && runAreaKnown) workload::AreaRun(static_cast<uint32_t>(runArea / ScreenPx() * 1000.0f));
        if (runStart != ~static_cast<size_t>(0) && on && runVerts >= minVerts) {
            float area = 0.0f;
            if (gate) {
                // (measured here unless the census did already; the measuring stops once the run is surely big enough)
                if (census == 1) {
                    area = runArea;
                } else {
                    const float enough = areaMin * 4.0f * ScreenPx();
                    for (size_t k = runStart; k < end && area < enough; ++k)
                        if (buf.cmds[k].type == CMD_DRAW) {
                            const float a = DrawAreaPx(buf, buf.draws[buf.cmds[k].arg]);
                            if (a > 0.0f) area += a;
                        }
                }
            }
            if (!gate || area >= areaMin * ScreenPx()) runs.push_back(FxRun{runStart, end, area});
        }
        runStart = ~static_cast<size_t>(0);
        runVerts = 0;
        runArea = 0.0f;
        runAreaKnown = false;
    };
    for (size_t i = 0; i < n; ++i) {
        const Cmd& c = buf.cmds[i];
        if (c.type == CMD_DRAW && !inPass && buf.draws[c.arg].rs.blend && buf.draws[c.arg].rs.colorWrite) {
            if (census < 0) census = AreaCensusPicture() ? 1 : 0;
            if (census == 1) {
                const DrawRec& d = buf.draws[c.arg];
                const float a = DrawAreaPx(buf, d);
                if (a < 0.0f) {
                    ++censusUnknown;
                } else {
                    const uint32_t milli = static_cast<uint32_t>(a / ScreenPx() * 1000.0f);
                    censusBlend += milli;
                    if (FxEligible(d)) {
                        censusAdd += milli;
                        runArea += a;
                        runAreaKnown = true;
                    }
                }
            }
        }
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
    if (census == 1 && !g_verify.shadow) workload::AreaPicture(censusBlend, censusAdd, censusUnknown);
    // (GXV_FXHALF_RUNS: the largest runs only; each further run is two more EFB scenes and a full-size composite)
    if (maxRuns && runs.size() > maxRuns) {
        std::stable_sort(runs.begin(), runs.end(), [](const FxRun& a, const FxRun& b) { return a.area > b.area; });
        runs.resize(maxRuns);
    }
    for (const FxRun& r : runs) {
        if (g_fxCmd.empty()) g_fxCmd.assign(n, 0);
        for (size_t k = r.start; k < r.end; ++k)
            if (buf.cmds[k].type == CMD_DRAW) g_fxCmd[k] = 1;
        any = true;
    }
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
// ---- GXV_VGL_LOADFROM=1 (default 0; needs vitaGL with patches/0019 COLOR_LOAD_FROM, the host's SetColorLoadHook): the EFB's
// new colour buffer gets the old picture from the next scene's own tile load (GXM loads a scene's colour tiles from the colour
// surface's background texture; for that one scene it points at the old buffer) instead of a full-screen quad drawn at the
// start of that scene: the quad's texture read and shading (~0.4-0.6 ms at 960x544) are gone, the load is made anyway. All four
// channels are loaded, as GXV_EFBSWAP_FULLMASK's quad writes them. Single-sampled EFB only. A 2x2 corner is still drawn back by
// the quad (the same pixels the load gives) so that the scene begins at once, before anything could read the new buffer.
// Desktop (tests): with no hook the load is emulated by a blit from the old buffer at the swap.
#if !defined(__VGX_VITA__)
bool EmuColorLoad(unsigned tex) {
    static GLuint readFbo = 0;
    if (!readFbo) G(glGenFramebuffers(1, &readFbo));
    if (!readFbo) return false;
    const GLenum kRead = 0x8CA8, kDraw = 0x8CA9; // GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER
    G(glBindFramebuffer(kRead, readFbo));
    G(glFramebufferTexture2D(kRead, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));
    G(glBindFramebuffer(kDraw, g_efbFbo));
    if (S.scissorOn == 1) G(glDisable(GL_SCISSOR_TEST));
    G(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
    glBlitFramebuffer(0, 0, g_rtW, g_rtH, 0, 0, g_rtW, g_rtH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    S.colorMask = -1;
    if (S.scissorOn == 1) G(glEnable(GL_SCISSOR_TEST));
    G(glFramebufferTexture2D(kRead, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0));
    G(glBindFramebuffer(GL_FRAMEBUFFER, S.fbo));
    return true;
}
#endif
bool EfbLoadFrom(GLuint old) {
    static const bool on = EnvInt("GXV_VGL_LOADFROM", 0) != 0;
    if (!on || g_efbSamples > 1 || S.fbo != g_efbFbo) return false;
    ColorLoadHook hook = g_colorLoadHook;
#if !defined(__VGX_VITA__)
    if (!hook && !g_dsim.on) hook = EmuColorLoad;
#endif
    return hook && hook(old);
}
// ---- GXV_EFBSWAP2: the EFB's buffer for a copy with a conversion (the front end's MarkSwapCopies, vgx_draw.cpp)
// The texture holds the EFB's pixels, not the converted copy: right for every sampler of the frame the copy was made in (the
// front end saw them all), not known for later frames. Each such texture is listed here with the conversion it lacks; at the
// start of every frame (RawPrescan, before anything is drawn) a draw that samples one of them before the texture is written
// again, and reads a channel the conversion changes, has it converted first: the copy quad of the ordinary copy, from the
// buffer into a texture of its own, a scene before the frame's first: what the ordinary copy cost, once, in the rare frame that
// needs it (the pause freeze, the camera cover or a wipe drawing an earlier picture's capture with a shader that reads the alpha
// RGB565 sets to 1, say). An entry holds while the texture still has that buffer (Tex::efbBuf, the same name): any other
// write into the texture gives the buffer back and ends it.
struct RawEntry {
    uint32_t texId = 0;
    GLuint gl = 0;       // the buffer the texture was given
    uint32_t valid = 0;  // channels (bit 0 r .. bit 3 a) the buffer already has as the copy would
    uint16_t ew = 0, eh = 0;
    float mat[4][4] = {}, add[4] = {};
};
std::vector<RawEntry> g_raw;
// GXV_EFBSWAP2_HOLD=<pictures> (default 1800, about a minute; 0 = off): a texture whose swapped buffer had to be converted for a
// later frame's reader (RawConvert) gets ordinary copies for that many pictures. A reader of the previous picture that the copy's
// own frame cannot see (MarkSwapCopies' prediction looks only at this frame) then costs one conversion, not one every picture: a
// swap that is converted next frame is a copy scene anyway, plus the renderer CPU of a target change and a buffer leaving the
// rotation (the r84 Good Egg slowdown). Exact either way: it only chooses the ordinary copy.
struct RawHold { uint32_t texId; uint64_t until; };
std::vector<RawHold> g_rawHolds;
long RawHoldFrames() {
    static const long n = std::max(0L, EnvInt("GXV_EFBSWAP2_HOLD", 1800));
    return n;
}
void RawHoldNote(uint32_t texId) {
    if (RawHoldFrames() <= 0) return;
    const uint64_t until = g_progFrame + static_cast<uint64_t>(RawHoldFrames());
    for (RawHold& h : g_rawHolds)
        if (h.texId == texId) { h.until = until; return; }
    if (g_rawHolds.size() < 64) g_rawHolds.push_back(RawHold{texId, until});
}
bool RawHeld(uint32_t texId) {
    for (size_t i = 0; i < g_rawHolds.size(); ++i) {
        if (g_rawHolds[i].texId != texId) continue;
        if (g_progFrame < g_rawHolds[i].until) return true;
        g_rawHolds[i] = g_rawHolds.back();
        g_rawHolds.pop_back();
        return false;
    }
    return false;
}
void RawNote(const CopyTexCmd& c, GLuint gl, uint32_t valid) {
    size_t i = 0;
    while (i < g_raw.size() && g_raw[i].texId != c.texId) ++i;
    if (valid == 0xFu) { // (no conversion: the buffer is the copy)
        if (i < g_raw.size()) { g_raw[i] = g_raw.back(); g_raw.pop_back(); }
        return;
    }
    if (i == g_raw.size()) g_raw.emplace_back();
    RawEntry& e = g_raw[i];
    e.texId = c.texId;
    e.gl = gl;
    e.valid = valid;
    e.ew = c.dstW;
    e.eh = c.dstH;
    std::memcpy(e.mat, c.mat, sizeof e.mat);
    std::memcpy(e.add, c.add, sizeof e.add);
    ++C.efbSwapsPartial;
    ++g_efbSwapsPartialTotal;
}
inline bool RawLive(const RawEntry& e) {
    return e.texId < g_tex.size() && g_tex[e.texId].efbBuf && g_tex[e.texId].id == e.gl;
}
// The buffer of entry e into an ordinary copy texture of the same size (the conversion the copy would have made).
void RawConvert(const RawEntry& e) {
    if (!RawLive(e)) return;
    Tex held = g_tex[e.texId]; // (the buffer: given back to the spares below, after the quad has read it)
    const int w = held.w, h = held.h;
    // (r32 fix: the destination is a spare buffer of this size when there is one - it leaves the rotation as the texture's own
    // storage -, a new texture only with kFxVramMin of video memory free; else the texture stays unconverted for now (its
    // reader sees the EFB's alpha / colours this frame) rather than risk an allocation that waits: see AdoptEfbBuffer)
    GLuint dest = 0;
    for (size_t i = 0; i < g_efbSpares.size() && !KeepRenderTargets(); ++i)
        if (g_efbSpares[i].w == w && g_efbSpares[i].h == h) {
            dest = g_efbSpares[i].id;
            g_efbSpares[i] = g_efbSpares.back();
            g_efbSpares.pop_back();
            --g_efbBufs;
            break;
        }
    if (!dest && VramFree() < kFxVramMin) {
        GPUPASS_NOTE(4, "GXV_EFBSWAP2: frame %llu: texture %u left unconverted (no spare buffer, video memory free %u KiB)\n",
                     static_cast<unsigned long long>(g_progFrame), e.texId, static_cast<unsigned>(std::min<size_t>(VramFree() / 1024u, 0xFFFFFFFFu)));
        return;
    }
    g_tex[e.texId] = Tex{};
    if (dest) {
        Tex& t = g_tex[e.texId];
        t.id = dest;
        t.w = static_cast<uint16_t>(w);
        t.h = static_cast<uint16_t>(h);
        t.renderTarget = true;
        t.wrapS = t.wrapT = t.minF = t.magF = 0xFF;
    }
    if (AcquireCopyTarget(e.texId, w, h, e.ew, e.eh, false) < 0) {
        // (no target: the texture keeps the buffer, unconverted, as before this function)
        Tex& t = g_tex[e.texId];
        DeleteTex(t);
        t = held;
        return;
    }
    SetViewport(0, 0, w, h);
    SetScissor(false);
    SetBlendOff();
    SetDepthTest(false);
    SetCull(CULL_NONE);
    SetDepthMask(false);
    SetColorMask(true, true);
    g_rsValid = false;
    float m[16];
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) m[col * 4 + r] = e.mat[r][col];
    DrawQuad(held.id, 0.0f, 0.0f, 1.0f, 1.0f, m, e.add);
    ++C.copyQuads;
    ++C.rawConverts;
    ++g_rawConvertsTotal;
    RawHoldNote(e.texId); // (GXV_EFBSWAP2_HOLD)
    EfbBufferBack(held);
    BindFbo(g_efbFbo);
    g_rsValid = false;
    GPUPASS_NOTE(3, "GXV_EFBSWAP2: frame %llu: texture %u converted after all (a later frame reads a channel its copy changes)\n",
                 static_cast<unsigned long long>(g_progFrame), e.texId);
}
void RawConvertTexture(uint32_t texId) {
    for (size_t i = 0; i < g_raw.size(); ++i)
        if (g_raw[i].texId == texId) {
            RawConvert(g_raw[i]);
            g_raw[i] = g_raw.back();
            g_raw.pop_back();
            return;
        }
}
// Start of a frame: the listed textures that a draw (or an atlas copy) of this frame reads, before writing them, in a channel the
// buffer does not have as the copy would, are converted now. Entries whose texture has let its buffer go are dropped.
void RawPrescan(const CmdBuf& buf) {
    if (g_raw.empty()) return;
    for (size_t i = 0; i < g_raw.size();) {
        if (RawLive(g_raw[i])) ++i;
        else { g_raw[i] = g_raw.back(); g_raw.pop_back(); }
    }
    if (g_raw.empty()) return;
    // (entries whose old picture may still be read / to convert, one bit each: the first 32 entries - there are one or two)
    uint32_t open = g_raw.size() >= 32 ? 0xFFFFFFFFu : (1u << g_raw.size()) - 1u, convert = 0;
    auto index = [&](uint32_t texId) -> int {
        for (size_t i = 0; i < g_raw.size() && i < 32; ++i)
            if ((open & (1u << i)) && g_raw[i].texId == texId) return static_cast<int>(i);
        return -1;
    };
    for (size_t k = 0; k < buf.cmds.size() && open; ++k) {
        const Cmd& c = buf.cmds[k];
        if (c.type == CMD_DRAW) {
            const DrawRec& d = buf.draws[c.arg];
            for (int m = 0; m < 8; ++m) {
                if (!(d.texMask & (1u << m))) continue;
                const int i = index(d.tex[m].texId);
                if (i < 0) continue;
                const uint32_t chan = d.fsId < g_fsSrc.size() && g_fsSrc[d.fsId] ? g_fsSrc[d.fsId]->texChan : 0xFFFFFFFFu;
                if (((chan >> (4 * m)) & 0xFu) & ~g_raw[i].valid) { convert |= 1u << i; open &= ~(1u << i); }
            }
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = buf.copyTex[c.arg];
            int i = index(t.srcTex);
            if (t.srcTex && i >= 0) { convert |= 1u << i; open &= ~(1u << i); }
            i = index(t.texId);
            if (i >= 0) open &= ~(1u << i); // (written: nothing after it sees the old picture)
        } else if (c.type == CMD_TEX_FREE || c.type == CMD_TEX_UPLOAD) {
            const int i = index(c.type == CMD_TEX_FREE ? c.arg : buf.uploads[c.arg].texId);
            if (i >= 0) open &= ~(1u << i);
        }
    }
    if (!convert) return;
    for (size_t i = 0; i < g_raw.size() && i < 32; ++i)
        if (convert & (1u << i)) RawConvert(g_raw[i]);
    for (size_t i = 0; i < g_raw.size();) {
        if (RawLive(g_raw[i])) ++i;
        else { g_raw[i] = g_raw.back(); g_raw.pop_back(); }
    }
}
void EfbBufferBack(Tex& t) {
    if (t.id) {
        // (r32 fix: a spare is not kept while video memory is short - a stage load's uploads need it, and vitaGL's allocator
        // waits for its collector when it has none; the next swap makes a buffer again only above kFxVramMin)
        if (KeepRenderTargets() || (g_efbSpares.size() < kEfbSparesMax && VramFree() >= kFxVramMin)) g_efbSpares.push_back(EfbSpare{t.id, t.w, t.h});
        else EfbBufLetGo(t.id);
    }
    t = Tex{};
}
// r32 fix (the console froze at a stage load with GXV_EFBSWAP2 on): a texture that holds an EFB buffer and is now written the
// ordinary way (a capture whose readers see its conversion - the system wipe's, the pause freeze's -, or a copy into it of
// another kind) used to give the buffer back and get new storage: a 640x456 allocation in the middle of the frame, right when
// a stage load's texture uploads fill video memory, where vitaGL's allocator can wait for a collector cycle that does not come
// while the scene is open. The buffer is an ordinary RGBA texture of the render size: when the copy wants that size it simply
// becomes the texture's own storage (it leaves the rotation: g_efbBufs counts it no more). No allocation, no new memory.
bool AdoptEfbBuffer(Tex& t, int w, int h) {
    if (!t.efbBuf || !t.id || t.w != w || t.h != h || KeepRenderTargets()) return false;
    t.efbBuf = false;
    t.renderTarget = true;
    t.mips = false;
    t.wrapS = t.wrapT = t.minF = t.magF = 0xFF; // (set at the next bind)
    --g_efbBufs;
    ++g_efbAdoptsTotal;
    GPUPASS_NOTE(3, "GXV_EFBSWAP: frame %llu: an EFB buffer held by a texture becomes its own storage (ordinary copy into it, no allocation; video memory free %u KiB)\n",
                 static_cast<unsigned long long>(g_progFrame), static_cast<unsigned>(std::min<size_t>(VramFree() / 1024u, 0xFFFFFFFFu)));
    return true;
}
bool EfbSwap(const CopyTexCmd& c) {
    if (!EfbSwapEnabled() || GpupassOff(GP_EFBSWAP) || g_inPass || g_skipDraws) return false;
    if (c.skip || c.fromPass || c.depth || c.fill || c.srcTex || c.dstTexW || c.texId == 0 || c.passMore) return false;
    if (c.src.x != 0 || c.src.y != 0 || c.src.w != g_efbW || c.src.h != g_efbH || c.dstW != g_efbW || c.dstH != g_efbH) return false;
    // (the channels the copy's conversion leaves as they are; a conversion at all only with GXV_EFBSWAP2's word that this frame's
    // samplers do not see it: CopyTexCmd::swapOk; later frames' are looked after by RawPrescan)
    uint32_t valid = 0;
    for (int r = 0; r < 4; ++r) {
        bool same = c.add[r] == 0;
        for (int col = 0; col < 4 && same; ++col) same = c.mat[r][col] == (r == col ? 1.0f : 0.0f);
        if (same) valid |= 1u << r;
    }
    if (valid != 0xFu && !c.swapOk) return false;
    if (valid != 0xFu && RawHeld(c.texId)) { ++C.swapHolds; ++g_efbSwapHoldsTotal; return false; } // (GXV_EFBSWAP2_HOLD)
    // (r32 fix: none of GXV_EFBSWAP2's while a stage loads - its loading thread runs: the buffers then stay where they are, and the
    // captures of the closing scene are made the ordinary way into storage the textures already have)
    if (valid != 0xFu && port_scene_loading_active && port_scene_loading_active()) return false;
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
            // Read once, default off: a reused name must be rebound after deletion unbinds it in the driver.
            static const bool rebindGuard = EnvInt("GXV_EFB_REBIND_GUARD", 0) != 0;
            if (rebindGuard) ForgetTex(fresh);
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
    RawNote(c, old, valid); // (GXV_EFBSWAP2: what the buffer still lacks of the copy)
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
    // GXV_EFBSWAP_FULLMASK=1 (default): the drawing back writes all four channels whenever it is made at all. A plane the plan
    // calls dead is overwritten whole before anything reads it, so its old value is as good as the new buffer's garbage: same
    // picture. But a colour-only or alpha-only mask is another blend state for vitaGL, i.e. another patched fragment program of
    // the copy program, made in play the first time (no warm-up knows it) and re-patched whenever the copy program alternates
    // between the copies' full mask and the drawing back's (vitaGL keeps one patched program per program): the GXV_EFBSWAP2
    // in-play links of r84. =0: the planes' own mask, as before.
    static const bool fullMask = EnvInt("GXV_EFBSWAP_FULLMASK", 1) != 0;
    if (fullMask && (keepC || keepA)) keepC = keepA = true;
    const bool loaded = (keepC || keepA) && EfbLoadFrom(old); // (GXV_VGL_LOADFROM: the next scene's load draws it back)
    if (loaded) {
        keepC = keepA = true;
        ++C.efbLoadFroms;
        ++g_efbLoadFromsTotal;
    }
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
        if (loaded) {
            // (the 2x2 corner, even so that the quad is not padded: texel (x, y) to pixel (x, y) as every EFB copy maps them)
            SetViewport(0, 0, 2, 2);
            DrawQuad(old, 0.0f, 0.0f, 2.0f / static_cast<float>(g_rtW), 2.0f / static_cast<float>(g_rtH), identity, zero4);
            SetViewport(0, 0, g_rtW, g_rtH);
        } else {
            DrawQuad(old, 0.0f, 0.0f, 1.0f, 1.0f, identity, zero4);
        }
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
    static const long sceneLogMin = DiagnosticInt("GXV_SCENELOG_MIN", 0);
    g_sceneLogKeep = sceneLogMin > 0;
    g_sceneLog = g_sceneLogKeep || (sceneLogFrame >= 0 && static_cast<int64_t>(buf.frame) == sceneLogFrame);
    g_sceneLogBuf.clear();
    beprof::FrameBegin(); // (GXV_BE_PROF)
    gpuscenes::FrameBegin(); // (GXV_GPU_SCENES)
    g_pxBuf = &buf; // (GXV_PX_DEADCLEAR looks ahead in it)
    ++g_serial;
    // (GXV_STUTTER_LOG) this frame's costs, logged when it ends
    struct StutterScope { uint64_t frame; double t0; ~StutterScope() { StutterFrameEnd(frame, NowMs() - t0); } } stutterScope{buf.frame, NowMs()};
    g_screenLive = ScreenLive();
    StageFrameBegin(); // (GXV_STAGEWARM)
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
    RawPrescan(buf); // (GXV_EFBSWAP2: a later frame's reader of a copy made without its conversion)
    if (cutAt != ~static_cast<size_t>(0) || passTime || drawTime) dplan::g_valid = false; // (diagnostic waits and cuts: no plan)
    // What an earlier buffer left of the deferred warm-up: a slice of it, before this frame's own commands. The stage being entered
    // goes first (GXV_STAGEWARM); the generic list waits for a frame the stage's queue leaves alone.
    if (!RunStageWarm(false)) RunWarm();
    BEPROF_MARK(kSetup);
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
        case CMD_TEX_UPLOAD: {
            ForgetCrowd(buf.uploads[c.arg].texId);
            const double tUp = NowMs(), x0 = g_xferFrameMs;
            Upload(buf.uploads[c.arg]);
            g_stFrame.texMs += NowMs() - tUp - (g_xferFrameMs - x0); // (GXV_STUTTER_LOG; tex11: a mode-2 fence in it is counted as a fence)
            ++g_stFrame.texUploads;
            for (const auto& level : buf.uploads[c.arg].levels) g_stFrame.texBytes += level.size();
            break;
        }
        case CMD_TEX_FREE:
            ForgetCrowd(c.arg);
            if (c.arg < g_tex.size() && g_tex[c.arg].id) {
                const double tf = NowMs(), x0 = g_xferFrameMs; // (GXV_STUTTER_LOG, tex11)
                DeleteTex(g_tex[c.arg]);
                --g_stats.textures;
                g_stFrame.texFreeMs += NowMs() - tf - (g_xferFrameMs - x0);
                ++g_stFrame.texFrees;
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
            const double tUp = NowMs(); // (GXV_STUTTER_LOG)
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
            g_stFrame.bufMs += NowMs() - tUp;
            ++g_stFrame.bufUploads;
            g_stFrame.bufBytes += u.vtx.size() + u.idx.size() * 2;
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
                g_pxPassScan = true; // (GXV_PX_DEADCLEAR: this pass's commands follow this one)
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
        if (beprof::g_frame) { // (GXV_BE_PROF: the command's time, or for a draw what follows its draw call)
            switch (c.type) {
            case CMD_DRAW: beprof::Mark(beprof::kDrawAfter); break;
            case CMD_COPY_TEX: beprof::Mark(beprof::kCopy); break;
            case CMD_PASS_BEGIN: beprof::Mark(beprof::kPass); break;
            case CMD_COPY_DISP: beprof::Mark(beprof::kDisp); break;
            case CMD_TEX_UPLOAD: case CMD_TEX_FREE: case CMD_BUF_UPLOAD: case CMD_BUF_FREE: beprof::Mark(beprof::kUpload); break;
            case CMD_SHADER: case CMD_PROGRAM: case CMD_VARIANT: beprof::Mark(beprof::kWarmCmd); break;
            default: beprof::Mark(beprof::kOther); break;
            }
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
    if (g_sceneLogKeep && C.scenes >= static_cast<unsigned>(EnvInt("GXV_SCENELOG_MIN", 0)))
        std::fprintf(stderr, "[scenes] frame %llu: %u scenes, %u copy quads, %u efb swaps, %u hoists, %u texture deletes\n%s", static_cast<unsigned long long>(buf.frame), C.scenes,
                     C.copyQuads, C.efbSwaps, C.hoists, C.texDeletes, g_sceneLogBuf.c_str());
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
    if (SanMode()) { // GXV_SANITIZE: one line when the totals moved, at most every 120 frames
        static uint64_t reported = 0, lastFrame = 0;
        if (g_san.Total() != reported && buf.frame >= lastFrame + 120) {
            reported = g_san.Total();
            lastFrame = buf.frame;
            std::fprintf(stderr, "[vgx] GXV_SANITIZE: frame %llu: %llu findings (skipped: %llu draws, %llu copies, %llu uploads; feedback draws %llu, copies %llu; viewport %llu, depth range %llu, copy %llu, upload %llu, index range %llu, vertex refs %llu, non-finite positions %llu, extreme positions %llu, non-finite attributes %llu, uniforms proj/mv/texm/pal/light/tev %llu/%llu/%llu/%llu/%llu/%llu)\n",
                         static_cast<unsigned long long>(buf.frame), static_cast<unsigned long long>(reported), static_cast<unsigned long long>(g_san.skippedDraws),
                         static_cast<unsigned long long>(g_san.skippedCopies), static_cast<unsigned long long>(g_san.skippedUploads), static_cast<unsigned long long>(g_san.feedbackDraws),
                         static_cast<unsigned long long>(g_san.feedbackCopies), static_cast<unsigned long long>(g_san.badViewport),
                         static_cast<unsigned long long>(g_san.badDepthRange), static_cast<unsigned long long>(g_san.badCopy), static_cast<unsigned long long>(g_san.badUpload),
                         static_cast<unsigned long long>(g_san.badIndexRange), static_cast<unsigned long long>(g_san.badVertexRefs), static_cast<unsigned long long>(g_san.nonFinitePos),
                         static_cast<unsigned long long>(g_san.extremePos), static_cast<unsigned long long>(g_san.nonFiniteAttr),
                         static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_PROJ]), static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_MV]),
                         static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_TEXM]), static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_PAL]),
                         static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_LIGHT]), static_cast<unsigned long long>(g_san.nonFiniteUniform[UB_TEV]));
            std::fflush(stderr);
        }
    }
    XferFence(); // (tex11, GXV_VITA_UPLOAD_SYNC=2: no texture transfer outlives its frame)
#ifdef VGX_GL_STUB
    ReportGlMem(buf.frame);
#endif
    if (g_verify.read) g_verify.counts = C;
    if (!g_verify.shadow) GpupassMemReport(buf.frame);
    g_lastScenes = C.scenes;
    if (!g_verify.shadow && port_log_summary) {
        // GXV_EFBSWAP2 (basic log, every 600 pictures while it does anything): whole-EFB copies made without a copy scene
        // (identity / with a conversion the samplers do not see), the later conversions, and the scenes, per picture.
        static uint32_t pics = 0, sw = 0, part = 0, conv = 0, scenes = 0, made = 0, whole = 0, wRgb = 0, wA = 0, wOk = 0, wPred = 0, holds = 0;
        ++pics; sw += C.efbSwaps; part += C.efbSwapsPartial; conv += C.rawConverts; scenes += C.scenes;
        made += C.copiesMade; whole += C.wholeCopies; wRgb += C.wholeRgb; wA += C.wholeA; wOk += C.wholeOk;
        wPred += C.wholePredNo; holds += C.swapHolds;
        if (pics >= 600) {
            const double n = static_cast<double>(pics);
            if (whole || conv)
                port_log_summary("efbswap2: %u pictures, a picture: %.2f texture copies, %.2f of the whole EFB (%.2f colour only as RGB565, %.2f alpha only as A8, "
                                 "%.2f whose samplers see no conversion), %.2f made without a copy scene (%.2f of them with a conversion: GXV_EFBSWAP2=%d), "
                                 "converted later %u; scenes %.2f a picture; left ordinary: %.2f for an earlier reader (GXV_EFBSWAP2_PREDICT), %u held "
                                 "(GXV_EFBSWAP2_HOLD)", pics, made / n, whole / n, wRgb / n, wA / n, wOk / n, sw / n, part / n,
                                 static_cast<int>(EnvInt("GXV_EFBSWAP2", 0) != 0), conv, scenes / n, wPred / n, holds);
            pics = sw = part = conv = scenes = made = whole = wRgb = wA = wOk = wPred = holds = 0;
        }
    }
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
    if ((g_px.noop || g_px.deadClear || g_px.scissor) && !g_verify.shadow && port_log_summary) {
        // (GXV_PX_*: basic log every 600 pictures, what was left out)
        static uint32_t pics = 0;
        if (++pics >= 600) {
            const double n = static_cast<double>(pics);
            port_log_summary("pxcut: %u pictures, a picture: GXV_PX_NOOP draws left out %.2f; GXV_PX_DEADCLEAR clears without their colour %.2f, without their depth %.2f, "
                             "left out whole %.2f; GXV_PX_SCISSOR scissor changes not made %.2f", pics, g_pxN.noop / n, g_pxN.clearColour / n, g_pxN.clearDepth / n,
                             g_pxN.clearWhole / n, g_pxN.scissorKept / n);
            pics = 0;
            g_pxTotal.noop += g_pxN.noop; g_pxTotal.clearColour += g_pxN.clearColour; g_pxTotal.clearDepth += g_pxN.clearDepth;
            g_pxTotal.clearWhole += g_pxN.clearWhole; g_pxTotal.scissorKept += g_pxN.scissorKept;
            g_pxN = PxCounts{};
        }
    }
    g_pxBuf = nullptr;
    beprof::FrameEnd(C.scenes, C.copyQuads); // (GXV_BE_PROF)
    gpuscenes::FrameEnd(); // (GXV_GPU_SCENES)
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
bool WarmCapacityYielded() {
    const uint64_t head = g_warmHeadIdentity.load(std::memory_order_acquire);
    return head && g_warmCapacityHead.load(std::memory_order_acquire) == head &&
           g_warmHeadIdentity.load(std::memory_order_acquire) == head && WarmPending() != 0;
}
void WarmIdle() {
    if (g_warm.empty() || g_warmBudgetMs.load(std::memory_order_relaxed) <= 0.0) return;
    // (the state a frame starts from, see the top of Execute)
    S.attrConstSet = 0;
    S.layoutVbo = ~0u;
    BindFbo(g_efbFbo);
    RunWarm();
}
unsigned WarmTotal() { return g_warmAll.load(std::memory_order_relaxed); }
unsigned StageWarmPending() { return StageWarmOn() ? g_stageLeft.load(std::memory_order_relaxed) : 0; }
// GXV_STAGEWARM_IDLE=1 (default 0): between frames, while the game's loading thread runs, the programs of the stage's queue are linked
// (compile + glLinkProgram, as a draw's own link) - and nothing else. Two console runs (r32 15:30, r36 16:52) froze for good right after
// "stage warm-up: stage ... queued" with the first version of this path, which did the queue's warm draws here: an empty triangle into
// the EFB between frames makes vitaGL begin a GXM scene outside any frame (scene_reset: the display copy's scene ended at the swap),
// which then stays open, unsubmitted, for as long as the load hands over no frame; everything that waits for the GPU or for vitaGL's
// collector meanwhile (allocation recovery: sceGxmFinish + collector cycles, a stage load's texture uploads short of video memory)
// waits on work that cannot complete. So: no draw (no scene), no program delete (no sceGxmFinish), no framebuffer binding here; the
// warm draws (vitaGL's patched variants) and any room-making stay in the slices at the start of quiet frames, where the frame's own
// scene is open anyway. A link refused at the slot reserve stops this path for the queue (it never deletes to make room).
bool StageWarmIdleWanted() {
    static const bool idleOn = EnvInt("GXV_STAGEWARM_IDLE", 0) != 0;
    if (!idleOn || !StageWarmOn()) return false;
    // (also when the stage being loaded has no queue yet: the game may hand over no frame for seconds while it loads)
    const uint32_t loading = port_scene_loading_key ? port_scene_loading_key() : 0;
    if (loading && loading != g_stageTarget) return true;
    return !g_stageIdleBlocked && g_stageIdleCursor < g_stageQueue.size();
}
void StageWarmIdleSlice();
void StageWarmIdle() {
    if (!StageWarmIdleWanted()) return;
    if (!g_stageLoaded) StageLoad();
    const uint32_t loading = port_scene_loading_key ? port_scene_loading_key() : 0;
    if (loading && loading != g_stageTarget) StageBuild(loading);
    StageWarmIdleSlice();
}
// (the links themselves; no GL call here or below draws, binds a framebuffer or deletes)
void StageWarmIdleSlice() {
    if (g_stageIdleBlocked || g_stageIdleCursor >= g_stageQueue.size()) return;
    if (GlAllocBackoff() || g_warmAllocFailSerial == g_serial) return;
    static const double budgetMs = static_cast<double>(std::max(1L, EnvInt("GXV_STAGEWARM_MS", 30)));
    static const unsigned reserve = static_cast<unsigned>(std::max(8L, EnvInt("GXV_STAGEWARM_RESERVE", 48)));
    const uint32_t capacityKey = g_warmCapacityKey;
    g_optionalReserve = reserve;
    const double t0 = NowMs();
    const unsigned links0 = g_sw.links;
    while (g_stageIdleCursor < g_stageQueue.size()) {
        const VariantCmd v = g_stageQueue[g_stageIdleCursor];
        g_optionalCapacityRefused = false;
        GetProgram(v.vsId, v.fsId, true); // (optional: refused at the reserve; a failed link is the draw's problem, as always)
        if (g_optionalCapacityRefused) { g_stageIdleBlocked = true; break; }
        if (GlAllocBackoff()) {
            g_warmAllocFailSerial = g_serial;
            g_glAllocFailSerial = ~uint64_t{0};
            break;
        }
        ++g_stageIdleCursor;
        if (NowMs() - t0 >= budgetMs) break;
    }
    g_optionalReserve = 0;
    g_optionalCapacityRefused = false;
    g_warmCapacityKey = capacityKey;
    const double took = NowMs() - t0;
    const unsigned links = g_sw.links - links0;
    g_stats.shaderWorkMs += took;
    g_stFrame.stageMs += took;
    g_stFrame.stageLinks += links;
    g_stageQueueMs += took;
    g_stageQueueLinks += links;
    if (g_stageIdleCursor >= g_stageQueue.size() || g_stageIdleBlocked)
        StageLog("stage warm-up: stage %08x: programs linked between frames (%u links so far, %.0f ms)%s; its %u warm draws follow in quiet frames",
                 g_stageTarget, g_stageQueueLinks, g_stageQueueMs, g_stageIdleBlocked ? ", stopped at the slot reserve" : "",
                 static_cast<unsigned>(g_stageQueue.size()));
}
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
#if defined(GXV_DRAW_REJECT_COUNTERS) && GXV_DRAW_REJECT_COUNTERS
void DrawRejectReport() {
    // PerfFrame calls this on the render owner, after Execute, at its existing log cadence.
    if(!port_log_summary) return;
    const auto c=g_drawReject;
    g_drawReject=DrawRejectCounters{};
    const uint32_t stage=port_scene_key ? port_scene_key() : 0;
    port_log_summary("draw-reject: stage_now=%08x nonempty=%llu submitted_calls=%llu rejected=%llu overflow=%u; Draw entries only, not pixels/actor identity",
        stage,(unsigned long long)c.nonempty,(unsigned long long)c.submitted,(unsigned long long)c.rejected,c.overflow?1u:0u);
    port_log_summary("draw-reject: exclusive skip_behind=%llu program=%llu static_vbo=%llu stream_chunk=%llu sanitize=%llu render_state=%llu",
        (unsigned long long)c.reason[0],(unsigned long long)c.reason[1],(unsigned long long)c.reason[2],
        (unsigned long long)c.reason[3],(unsigned long long)c.reason[4],(unsigned long long)c.reason[5]);
    port_log_summary("draw-reject: program_state_overlap alloc_backoff=%llu compile_failure=%llu link_unavailable=%llu missing_source=%llu shader_pending=%llu exhausted=%llu other=%llu",
        (unsigned long long)c.programState[0],(unsigned long long)c.programState[1],(unsigned long long)c.programState[2],
        (unsigned long long)c.programState[3],(unsigned long long)c.programState[4],(unsigned long long)c.programState[5],(unsigned long long)c.programState[6]);
    static const char* const names[]={"skip_behind","program","static_vbo","stream_chunk","sanitize","render_state"};
    for(unsigned i=0;i<RejectCount;++i) if(c.reason[i]) {
        const auto& e=c.first[i];
        port_log_summary("draw-reject: first reason=%s frame=%llu vs=%u fs=%u vbuf=%u vchunk=%u indices=%u; stage_now is report-time only",
            names[i],(unsigned long long)e.frame,e.vs,e.fs,e.vbuf,e.vchunk,e.indices);
    }
}
#endif
Stats GetStats() { return g_stats; }

} // namespace vgx::backend

#ifdef VGX_GL_STUB
namespace vgx::host {
bool CreateContext() { return true; }
}
#endif
