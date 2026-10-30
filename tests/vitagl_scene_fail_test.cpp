// Fault injection for vitaGL's checked scene entry (patch 0009) and checked depth/stencil creation (patch 0010).
// The functions under test are the real text of the patched vitaGL driver (tools/prepare_vitagl_scene_fixture.cjs); only the
// GXM, allocator and GL state around them is stubbed. Run through tools/test_vitagl_scene_fail.sh.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "vitagl_scene_functions_config.h"

using GLboolean = int;
using GLenum = unsigned;
using GLuint = uintptr_t; // the driver passes object pointers as GLuint (32 bit on the Vita)
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned;
using GLfloat = float;
constexpr int GL_TRUE = 1, GL_FALSE = 0;
constexpr unsigned GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_STENCIL_BUFFER_BIT = 0x400;
constexpr unsigned GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_LINEAR = 0x2601, GL_BACK = 0x405, GL_NEAREST = 0x2600;
constexpr unsigned GL_TRIANGLES = 4, GL_LINES = 1;
#define THREAD_SAFE()
#define VGL_ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))
#define SCE_GXM_TILE_SIZEX 16
#define SCE_GXM_TILE_SIZEY 16
#define SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT 4096
enum SceGxmMultisampleMode { SCE_GXM_MULTISAMPLE_NONE, SCE_GXM_MULTISAMPLE_2X, SCE_GXM_MULTISAMPLE_4X };
enum SceGxmDepthStencilFormat { SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M = 0x1000000, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8 = 0x2000000 };
enum SceGxmDepthStencilSurfaceType { SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR = 0x1000 };
enum { SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED = 1, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED = 2 };
enum { SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_PRIMITIVE_LINES };
enum { SCE_GXM_CULL_NONE, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL, SCE_GXM_DEPTH_WRITE_ENABLED, SCE_GXM_DEPTH_WRITE_DISABLED,
       SCE_GXM_FRAGMENT_PROGRAM_ENABLED, SCE_GXM_FRAGMENT_PROGRAM_DISABLED, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_FUNC_NEVER,
       SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_REPLACE, SCE_GXM_REGION_CLIP_OUTSIDE, SCE_GXM_INDEX_FORMAT_U16,
       SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_PARAMETER_TYPE_F32 };
constexpr int kErrWithinScene = (int)0x80021001, kErrNotWithinScene = (int)0x80021002;
struct SceGxmDepthStencilSurface { uint32_t w[8]; };
struct SceGxmTexture { int dummy; };
struct SceGxmColorSurface { void* data; SceGxmTexture backgroundTex; };
struct SceGxmSyncObject { int dummy; };
struct SceGxmRenderTarget { int dummy; };
struct vector4f { float x, y, z, w; };
struct texture { void* data; SceGxmTexture gxm_tex; unsigned mag_filter, min_filter; bool use_mips; int mip_count; };
struct framebuffer {
    GLboolean is_float;
    uint8_t depthbuffer_state;
    SceGxmRenderTarget* target;
    SceGxmColorSurface colorbuffer;
    SceGxmDepthStencilSurface depthbuffer;
    SceGxmDepthStencilSurface* depthbuffer_ptr;
    int width, height;
    texture* tex;
#if FIXTURE_HAS_MS
    SceGxmMultisampleMode msaa;
#endif
};
enum { DEPTHBUFFER_MISSING, DEPTHBUFFER_WANTS_STENCIL, DEPTHBUFFER_READY };
struct SceSharedFbInfo { int index; int vsync; };
struct SceGxmNotification { volatile unsigned* address; unsigned value; };
struct SceGxmPrecomputedDraw { int d; };
using SceGxmPrimitiveType = int;

// ---- the simulated GPU, allocator and counters
struct AbortCalled {};
struct Sim {
    bool scene_open;
    int begins, ends, bad_ends, bad_begins, draws, draws_outside_scene, state_calls, finishes, poly_restores, logs, aborts;
    int begin_fail_budget;                      // the next N sceGxmBeginScene calls fail
    int alloc_calls; uint64_t alloc_fail_mask;  // bit i set: the i-th gpu allocation since the last reset fails
    int rt_fail;                                // 1: render target creation fails
    int frees, dirty_marks;
    std::map<void*, size_t> live;
};
static Sim sim;
static void ResetSim() { sim = Sim(); }
static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
#define abort() (++sim.aborts, throw AbortCalled())
#define LOG_ERRORS
#define SKIP_ERROR_HANDLING
#define SKIP_SPLASHSCREEN
#define STORE_DEPTH_STENCIL
#define SET_GL_ERROR(x)
#define SET_GL_ERROR_WITH_VALUE(x, y)

static int gxm_context = 7;
static SceGxmNotification query_fence;
static bool system_app_mode = false;
static unsigned vsync_interval = 1;
static GLboolean needs_scene_reset = GL_TRUE, needs_end_scene, scene_not_flushed, dirty_framebuffer, dirty_query;
static uint32_t dirty_frag_unifs, dirty_vert_unifs;
static GLboolean dirty_shader_frag_unifs, dirty_shader_vert_unifs;
static uint32_t vgl_framecount;
static framebuffer *in_use_framebuffer, *active_write_fb, *old_framebuffer;
static GLboolean is_fbo_float, is_rendering_display, dirty_scissor_state, scissor_test_state, skip_scissor_invalidation;
static SceGxmMultisampleMode msaa_mode = SCE_GXM_MULTISAMPLE_NONE, cur_msaa = SCE_GXM_MULTISAMPLE_NONE;
static SceGxmRenderTarget gxm_render_target_obj, *gxm_render_target = &gxm_render_target_obj;
static SceGxmSyncObject sync_objs[3], *gxm_sync_objects[3] = {&sync_objs[0], &sync_objs[1], &sync_objs[2]};
static SceGxmColorSurface gxm_color_surfaces[3];
static SceGxmDepthStencilSurface gxm_depth_stencil_surface;
static int gxm_back_buffer_index, gxm_front_buffer_index, gxm_fbo_rt_size = 2;
static SceSharedFbInfo shared_fb_info;
static int shared_fb;
static uint32_t legacy_pool_size = 0;
static float *legacy_pool, *legacy_pool_ptr, *legacy_pool_end;
struct { int x, y, w, h; } gl_viewport, region;
static float x_port, x_scale, y_port, y_scale, z_port, z_scale;
static int DISPLAY_WIDTH = 960, DISPLAY_HEIGHT = 544;
static float DISPLAY_WIDTH_FLOAT = 960.f, DISPLAY_HEIGHT_FLOAT = 544.f;
static int display_read_mode = GL_BACK;

// clear / blit state
static vector4f clear_vertices_obj, *clear_vertices = &clear_vertices_obj;
static struct { float r, g, b, a; } clear_rgba_val;
static float depth_value, scissor_depth;
static GLboolean orig_depth_test, depth_test_state, depth_mask_state;
static uint32_t stencil_mask_front_write, stencil_mask_back_write, stencil_value;
static int polygon_mode_front, polygon_mode_back;
static int clear_vertex_program_patched, clear_fragment_program_patched, clear_fragment_program_float_patched;
static int blit_vertex_program_patched, blit_fragment_program_patched, blit_fragment_program_float_patched;
static int clear_position, clear_depth, clear_color, clear_position_offset, clear_depth_offset, blit_position_offset, blit_texcoord_offset;
static SceGxmPrecomputedDraw clear_draw_state, blit_draw_state;

// draw state
static uint16_t *default_idx_ptr, *default_quads_idx_ptr, *default_line_strips_idx_ptr;
static int cur_program = 1;
static uint16_t ffp_vertex_attrib_state = 1;

// ---- generic GXM state stubs: any state call counts, nothing else
#define GXM_STATE_STUBS(X) \
    X(sceGxmSetCullMode) X(sceGxmSetFrontDepthBias) X(sceGxmSetBackDepthBias) X(sceGxmSetFrontPolygonMode) X(sceGxmSetBackPolygonMode) \
    X(sceGxmSetVertexProgram) X(sceGxmSetFragmentProgram) X(sceGxmReserveVertexDefaultUniformBuffer) X(sceGxmReserveFragmentDefaultUniformBuffer) \
    X(sceGxmSetUniformDataF) X(sceGxmSetFrontFragmentProgramEnable) X(sceGxmSetBackFragmentProgramEnable) X(sceGxmSetFrontStencilFunc) \
    X(sceGxmSetBackStencilFunc) X(sceGxmSetFrontStencilRef) X(sceGxmSetBackStencilRef) X(sceGxmSetFragmentTexture) X(vglSetUniformData) \
    X(invalidate_viewport) X(validate_viewport) X(invalidate_depth_test) X(validate_depth_test) X(change_depth_write) X(refresh_stencil_settings) \
    X(update_polygon_offset) X(vglRestoreVertexUniformBuffer) X(vglRestoreFragmentUniformBuffer) X(change_cull_mode) X(vector4f_convert_to_local_space) \
    X(vglSetTexMagFilter) X(vglSetTexMinFilter) X(vglSetTexMipmapCount) X(vglSetViewport)
#define X(name) template <class... A> static int name(A...) { ++sim.state_calls; return 0; }
GXM_STATE_STUBS(X)
#undef X
#define vgl_msaa_fp(a, b) 0
static void vgl_log(const char*, ...) { ++sim.logs; }
static const char* get_gxm_error_literal(int) { return "error"; }
static void glViewport(int, int, int, int) { ++sim.state_calls; }
static void sceSharedFbBegin(int, SceSharedFbInfo*) {}
static void update_scissor_test() { dirty_scissor_state = GL_FALSE; ++sim.state_calls; }
static void sceGxmSetRegionClip(int, int, int, int, int, int) { ++sim.state_calls; }

// ---- memory
static void* gpu_alloc_mapped_aligned_for_gpu(size_t, size_t size) {
    int idx = sim.alloc_calls++;
    if ((sim.alloc_fail_mask >> idx) & 1) return nullptr;
    void* p = std::malloc(size);
    sim.live[p] = size;
    return p;
}
struct AnyPtr { void* p; template <class T> operator T*() const { return (T*)p; } }; // the driver is C: it converts void* freely
static AnyPtr gpu_alloc_mapped_temp(size_t size) { return AnyPtr{std::calloc(1, size)}; }
static void vgl_free(void* p) {
    if (!p) return;
    auto it = sim.live.find(p);
    Require(it != sim.live.end(), "freed a pointer the driver does not own");
    sim.live.erase(it);
    std::free(p);
    ++sim.frees;
}
static void vgl_memset(void* p, int v, size_t n) { std::memset(p, v, n); }
static void mark_as_dirty(void* p) { // handed to the garbage collector (what the driver does for a surface that was in use)
    auto it = sim.live.find(p);
    Require(it != sim.live.end(), "garbage-collected a pointer the driver does not own");
    sim.live.erase(it);
    std::free(p);
    ++sim.dirty_marks;
}

// ---- depth surface helpers (same words the real vglDepthStencilSurfaceInit writes)
static int vglDepthStencilSurfaceInit(SceGxmDepthStencilSurface* surface, SceGxmDepthStencilFormat fmt, SceGxmDepthStencilSurfaceType type,
                                      unsigned stride, void* depth, void* stencil) {
    uint32_t* s = surface->w;
    s[0] = type & 0x11000 | 0x100000 | fmt & 0x7EEE000 | 8 * ((stride >> 5) - 1) & 0x7F8;
    s[1] = (uint32_t)(uintptr_t)depth;
    s[2] = (uint32_t)(uintptr_t)stencil;
    s[3] = 0x3F800000;
    s[4] = 0x200;
    return 0;
}
static void sceGxmDepthStencilSurfaceSetForceLoadMode(SceGxmDepthStencilSurface* s, int m) { s->w[5] |= 0x10 * m; }
static void sceGxmDepthStencilSurfaceSetForceStoreMode(SceGxmDepthStencilSurface* s, int m) { s->w[5] |= 0x100 * m; }

// ---- scene machinery
static void* sceGxmColorSurfaceGetData(SceGxmColorSurface* c) { return c->data; }
static void _glFramebufferTexture2D(framebuffer* fb, unsigned, texture* t, int) { fb->colorbuffer.data = t->data; }
static int setup_render_target_ms(SceGxmRenderTarget** rt, int, int, int, SceGxmMultisampleMode) {
    static SceGxmRenderTarget made[8]; static int next;
    if (sim.rt_fail) return kErrNotWithinScene; // any nonzero error
    *rt = &made[next++ & 7];
    return 0;
}
static int setup_render_target(SceGxmRenderTarget** rt, int w, int h, int n) { return setup_render_target_ms(rt, w, h, n, msaa_mode); }
#define FB_MSAA(fb) ((fb)->msaa)
static int sceGxmBeginScene(int c, int, SceGxmRenderTarget* rt, void*, void*, SceGxmSyncObject*, SceGxmColorSurface* color, SceGxmDepthStencilSurface* ds) {
    Require(c == 7, "correct context");
    if (sim.begin_fail_budget > 0) { --sim.begin_fail_budget; return kErrWithinScene; }
    if (sim.scene_open) { ++sim.bad_begins; return kErrWithinScene; }
    Require(rt != nullptr, "a scene is never begun on a null render target");
    Require(color != nullptr && ds != nullptr, "a scene is never begun without colour and depth surfaces");
    Require(ds->w[1] != 0, "a scene is never begun on a depth surface without depth memory");
    sim.scene_open = true;
    ++sim.begins;
    return 0;
}
static int sceGxmEndScene(int c, void*, SceGxmNotification*) {
    Require(c == 7, "correct context");
    if (!sim.scene_open) { ++sim.bad_ends; return kErrNotWithinScene; }
    sim.scene_open = false;
    ++sim.ends;
    return 0;
}
static void sceGxmFinish(int) { ++sim.finishes; }
static void sceDisplayWaitVblankStartMulti(unsigned) {}
static int sceGxmDraw(int c, ...) { Require(c == 7, "correct context"); ++sim.draws; if (!sim.scene_open) ++sim.draws_outside_scene; return 0; }
static int sceGxmDrawInstanced(int c, ...) { return sceGxmDraw(c); }
static int sceGxmDrawPrecomputed(int c, SceGxmPrecomputedDraw*) { return sceGxmDraw(c); }
#ifdef GPU_TIMERS
static GLboolean vgl_rt_gpu_on;
static void vgl_rt_end_scene(int c) { sceGxmEndScene(c, nullptr, nullptr); }
#endif

// ---- draw callers
#define gl_primitive_to_gxm(m, p, c) (p = (m) == GL_LINES ? SCE_GXM_PRIMITIVE_LINES : SCE_GXM_PRIMITIVE_TRIANGLES, ++sim.state_calls)
#define restore_polygon_mode(p) ++sim.poly_restores
static GLboolean _glDrawArrays_CustomShadersIMPL(int, int, GLboolean) { ++sim.state_calls; return GL_TRUE; }
static void _glDrawArrays_FixedFunctionIMPL(int, int) { ++sim.state_calls; }
static void _glMultiDrawArrays_CustomShadersIMPL(int, uint16_t*, const GLint*, const GLsizei*, GLint, GLsizei, GLsizei) { ++sim.state_calls; sceGxmDraw(7); }
static void _glMultiDrawArrays_FixedFunctionIMPL(int, uint16_t*, const GLint*, const GLsizei*, GLint, GLsizei, GLsizei) { ++sim.state_calls; sceGxmDraw(7); }
static void* vgl_fast_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
constexpr unsigned GL_QUADS = 7, GL_LINE_STRIP = 3, GL_LINE_LOOP = 2;
static int MAX_IDX_NUMBER = 1 << 20;

#include "vitagl_scene_functions.inc"

// ---- helpers
static framebuffer MakeFbo(uint8_t depth_state, texture* t) {
    framebuffer fb;
    std::memset(&fb, 0, sizeof fb);
    static char texture_memory[16];
    t->data = texture_memory;
    fb.tex = t;
    fb.width = fb.height = 64;
    fb.colorbuffer.data = t->data;
    fb.depthbuffer_state = depth_state;
    std::memset(&fb.depthbuffer, 0xA5, sizeof fb.depthbuffer); // a recognisable "old" surface
    return fb;
}
static void ResetDriver() {
    ResetSim();
    needs_scene_reset = GL_TRUE; needs_end_scene = GL_FALSE; scene_not_flushed = GL_FALSE;
    dirty_framebuffer = dirty_query = GL_FALSE;
    in_use_framebuffer = active_write_fb = old_framebuffer = nullptr;
    vgl_framecount = 100;
    scene_fail_frame = 0; scene_fail_tries = 0; scene_fail_streak = 0;
    query_fence.value = 10;
}
static void EndFrame() { // what vglSubmitWithoutSwap/vglSwapBuffers do to the scene flags
    vglSubmitWithoutSwap();
    ++vgl_framecount;
}
static bool SameBytes(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }
static void ReleaseLive() { for (auto& kv : sim.live) std::free(kv.first); sim.live.clear(); }

static void TestDepthCreation() {
    SceGxmDepthStencilSurface old_surface, surface;
    std::memset(&old_surface, 0x5A, sizeof old_surface);
    const unsigned samples = VGL_ALIGN(64u, 16u) * VGL_ALIGN(64u, 16u);

    // success: depth then stencil, published together, same words as ever
    ResetDriver(); surface = old_surface;
    Require(init_depth_stencil_buffer(64, 64, &surface, GL_TRUE) == GL_TRUE, "creation succeeds with memory");
    Require(sim.alloc_calls == 2 && sim.live.size() == 2, "depth and stencil allocated, nothing else");
    uint32_t depth_ptr = surface.w[1], stencil_ptr = surface.w[2];
    Require(depth_ptr && stencil_ptr && surface.w[3] == 0x3F800000 && surface.w[4] == 0x200, "surface describes both buffers");
    bool found_depth = false, found_stencil = false;
    for (auto& kv : sim.live) {
        if ((uint32_t)(uintptr_t)kv.first == depth_ptr) { found_depth = kv.second == 4 * samples; for (size_t i = 0; i < kv.second; ++i) Require(((uint8_t*)kv.first)[i] == 0x80, "mask update bit initialised"); }
        if ((uint32_t)(uintptr_t)kv.first == stencil_ptr) found_stencil = kv.second == samples;
    }
    Require(found_depth && found_stencil, "depth is 4 bytes a sample, stencil 1");
    Require((surface.w[5] & 0x210) == 0x210, "force load/store set (STORE_DEPTH_STENCIL)");
    ReleaseLive();

    // success without stencil
    ResetDriver(); surface = old_surface;
    Require(init_depth_stencil_buffer(64, 64, &surface, GL_FALSE) && sim.live.size() == 1 && surface.w[2] == 0, "no stencil requested, none allocated");
    ReleaseLive();

    // depth allocation fails: nothing allocated, old surface untouched, nothing written through a null pointer
    ResetDriver(); surface = old_surface; sim.alloc_fail_mask = 1;
    Require(init_depth_stencil_buffer(64, 64, &surface, GL_TRUE) == GL_FALSE, "depth failure is reported");
    Require(sim.alloc_calls == 1 && sim.live.empty() && sim.frees == 0, "depth failure leaves nothing allocated");
    Require(SameBytes(&surface, &old_surface, sizeof surface), "depth failure keeps the old surface");

    // depth ok, stencil fails: the depth buffer is freed, the old surface untouched
    ResetDriver(); surface = old_surface; sim.alloc_fail_mask = 2;
    Require(init_depth_stencil_buffer(64, 64, &surface, GL_TRUE) == GL_FALSE, "stencil failure is reported");
    Require(sim.alloc_calls == 2 && sim.live.empty() && sim.frees == 1, "stencil failure frees the depth buffer, leaks nothing");
    Require(SameBytes(&surface, &old_surface, sizeof surface), "stencil failure keeps the old surface");

    // depth fails although no stencil is wanted
    ResetDriver(); surface = old_surface; sim.alloc_fail_mask = 1;
    Require(init_depth_stencil_buffer(64, 64, &surface, GL_FALSE) == GL_FALSE && sim.live.empty() && SameBytes(&surface, &old_surface, sizeof surface), "depth failure without stencil");

#if FIXTURE_HAS_MS
    for (SceGxmMultisampleMode mode : {SCE_GXM_MULTISAMPLE_2X, SCE_GXM_MULTISAMPLE_4X}) {
        ResetDriver(); surface = old_surface;
        Require(init_depth_stencil_buffer_ms(64, 64, &surface, GL_TRUE, mode) && sim.live.size() == 2, "multisampled creation");
        size_t depth_size = 0;
        for (auto& kv : sim.live) if ((uint32_t)(uintptr_t)kv.first == surface.w[1]) depth_size = kv.second;
        Require(depth_size == 4 * samples * (mode == SCE_GXM_MULTISAMPLE_2X ? 2 : 4), "multisample factor in the size");
        ReleaseLive();
        ResetDriver(); surface = old_surface; sim.alloc_fail_mask = 2;
        Require(!init_depth_stencil_buffer_ms(64, 64, &surface, GL_TRUE, mode) && sim.live.empty() && SameBytes(&surface, &old_surface, sizeof surface), "multisampled stencil failure");
    }
#endif

    // the display surface has no fallback: failure is a fail-stop, not a null fill
    ResetDriver(); sim.alloc_fail_mask = 1;
    bool stopped = false;
    try { init_display_depth_stencil_surfaces(); } catch (const AbortCalled&) { stopped = true; }
    Require(stopped && sim.live.empty(), "display depth failure stops instead of continuing");
    ResetDriver();
    init_display_depth_stencil_surfaces();
    Require(sim.aborts == 0 && sim.live.size() == 2, "display depth success");
    ReleaseLive();
}

static void TestSceneDepthFailureAndRecovery() {
    for (uint8_t state : {(uint8_t)DEPTHBUFFER_MISSING, (uint8_t)DEPTHBUFFER_WANTS_STENCIL}) {
        for (uint64_t mask : {1ull, 2ull}) {
            if (state == DEPTHBUFFER_MISSING && mask == 2) continue; // no stencil is created for this state
            ResetDriver();
            texture t{}; framebuffer fb = MakeFbo(state, &t); SceGxmDepthStencilSurface before = fb.depthbuffer;
            active_write_fb = &fb; sim.alloc_fail_mask = mask;
            Require(scene_reset() == GL_FALSE, "depth failure fails the scene entry");
            Require(sim.begins == 0 && sim.ends == 0 && !sim.scene_open, "no scene begun, none ended");
            Require(fb.depthbuffer_ptr == nullptr && !(fb.depthbuffer_state & DEPTHBUFFER_READY) && SameBytes(&fb.depthbuffer, &before, sizeof before), "framebuffer keeps its (not ready) depth state");
            Require(sim.live.empty(), "failed creation leaves nothing allocated");
            Require(needs_scene_reset && !needs_end_scene, "entry stays pending, no EndScene owed");
            vglSubmitWithoutSwap();
            Require(sim.ends == 0 && sim.bad_ends == 0, "frame end after a failed entry ends no scene");
            // the next frame has memory again
            ++vgl_framecount; sim.alloc_fail_mask = 0; sim.alloc_calls = 0;
            Require(scene_reset() == GL_TRUE, "next frame recovers");
            Require(sim.begins == 1 && sim.scene_open && fb.depthbuffer_ptr == &fb.depthbuffer && (fb.depthbuffer_state & DEPTHBUFFER_READY), "recovered scene runs on a complete depth surface");
            Require(sim.live.size() == (state == DEPTHBUFFER_WANTS_STENCIL ? 2u : 1u), "recovered scene owns exactly its depth/stencil memory");
            EndFrame();
            Require(sim.ends == 1 && sim.bad_ends == 0, "the recovered scene is ended exactly once");
            ReleaseLive();
        }
    }
}

static void TestOpenSceneEndedOnceThenFailure() {
    ResetDriver();
    texture t{}; framebuffer fb = MakeFbo(DEPTHBUFFER_MISSING, &t);
    // a good scene on the display first
    Require(scene_reset() == GL_TRUE && sim.begins == 1 && needs_end_scene, "display scene begins");
    active_write_fb = &fb; sim.alloc_calls = 0; sim.alloc_fail_mask = 1;
    Require(scene_reset() == GL_FALSE, "switching to a framebuffer whose depth cannot be made fails");
    Require(sim.ends == 1 && sim.begins == 1 && !sim.scene_open && !needs_end_scene, "the old scene was ended once, no new one is open");
    vglSubmitWithoutSwap();
    Require(sim.ends == 1 && sim.bad_ends == 0, "frame end does not end a scene twice");
    ReleaseLive();
}

static void TestBeginSceneFailure() {
    for (int on_fbo = 0; on_fbo < 2; ++on_fbo) {
        ResetDriver();
        texture t{}; framebuffer fb = MakeFbo(DEPTHBUFFER_MISSING, &t);
        if (on_fbo) active_write_fb = &fb;
        sim.begin_fail_budget = 100; // every attempt of this frame fails
        Require(scene_reset() == GL_FALSE, "sceGxmBeginScene failure fails the scene entry");
        Require(needs_scene_reset && !needs_end_scene && !sim.scene_open, "no scene, no EndScene owed");
        // every submitting caller stops
        int polygon_state_before = sim.state_calls;
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
        GLint first[1] = {0}; GLsizei count[1] = {3};
        glMultiDrawArrays(GL_TRIANGLES, first, count, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        framebuffer* before_blit = active_write_fb;
        glBlitNamedFramebuffer(0, (GLuint)(uintptr_t)&fb, 0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        Require(active_write_fb == before_blit, "blit puts the write framebuffer back");
        Require(sim.draws == 0 && sim.draws_outside_scene == 0, "no draw, clear or blit is submitted without a scene");
        Require(sim.state_calls - polygon_state_before == 2 /* glDrawArrays + instanced convert the primitive */ , "no gxm state is touched beyond the primitive conversion that is put back");
        Require(sim.poly_restores == 2, "polygon mode of the two converting callers is put back");
        glFinish();
        Require(sim.ends == 0 && sim.bad_ends == 0, "finish ends nothing");
        scene_not_flushed = GL_TRUE; glFlush();
        Require(sim.ends == 0 && sim.bad_ends == 0 && !scene_not_flushed, "flush ends nothing");
        vglSubmitWithoutSwap();
        Require(sim.ends == 0 && sim.bad_ends == 0, "frame end ends nothing");
        Require(100 - sim.begin_fail_budget == VGL_SCENE_TRIES_PER_FRAME, "the driver was asked a bounded number of times, the rest of the frame was refused");
        // next frame: the entry works and the draw is submitted inside a scene
        ++vgl_framecount; sim.begin_fail_budget = 0;
        if (on_fbo) active_write_fb = &fb; // blit restored it already; keep explicit
        glDrawArrays(GL_TRIANGLES, 0, 3);
        Require(sim.begins == 1 && sim.draws == 1 && sim.draws_outside_scene == 0, "recovery: one scene, one draw inside it");
        EndFrame();
        Require(sim.ends == 1 && sim.bad_ends == 0 && sim.bad_begins == 0, "recovery: the scene is ended exactly once");
        ReleaseLive();
    }
}

static void TestRenderTargetFailure() {
    ResetDriver();
    texture t{}; framebuffer fb = MakeFbo(DEPTHBUFFER_MISSING, &t);
    active_write_fb = &fb; sim.rt_fail = 1;
    Require(scene_reset() == GL_FALSE && sim.begins == 0 && fb.target == nullptr, "no render target: no scene, nothing dereferenced");
    ++vgl_framecount; sim.rt_fail = 0;
    Require(scene_reset() == GL_TRUE && sim.begins == 1 && fb.target != nullptr, "render target made on the next frame");
    EndFrame();
    ReleaseLive();
}

static void TestBoundedRetryAndFailStop() {
    ResetDriver();
    sim.begin_fail_budget = 1000000;
    // three attempts a frame reach the driver, the rest of the frame is refused without touching anything
    int attempts_frame0 = 0;
    for (int i = 0; i < 10; ++i) { scene_reset(); }
    attempts_frame0 = 1000000 - sim.begin_fail_budget;
    Require(attempts_frame0 == VGL_SCENE_TRIES_PER_FRAME, "a frame makes a bounded number of attempts");
    // fail-stop after the limit, not before
    int stopped_at = -1;
    try {
        for (int frame = 0; frame < 100000; ++frame) {
            ++vgl_framecount;
            for (int i = 0; i < 5; ++i) scene_reset();
        }
    } catch (const AbortCalled&) { stopped_at = 1000000 - sim.begin_fail_budget; }
    Require(stopped_at == VGL_SCENE_FAIL_LIMIT + 1, "fail-stop on the first attempt beyond the limit");
    Require(sim.begins == 0 && sim.draws == 0, "nothing was begun or drawn on the way");

    // a scene that does begin clears the streak
    ResetDriver();
    for (int round = 0; round < 3; ++round) {
        sim.begin_fail_budget = VGL_SCENE_FAIL_LIMIT - 1;
        int guard = 0;
        while (sim.begin_fail_budget > 0 && guard++ < 100000) { ++vgl_framecount; for (int i = 0; i < 4; ++i) scene_reset(); }
        ++vgl_framecount;
        Require(scene_reset() == GL_TRUE, "begins once the failures stop");
        EndFrame();
    }
    Require(sim.aborts == 0, "failures separated by a good scene never add up to a fail-stop");
}

static void TestNothingFails() {
    ResetDriver();
    texture t{}; framebuffer fb = MakeFbo(DEPTHBUFFER_WANTS_STENCIL, &t);
    glDrawArrays(GL_TRIANGLES, 0, 3);                       // display scene begins
    glDrawArrays(GL_TRIANGLES, 0, 3);                       // same scene
    Require(sim.begins == 1 && sim.ends == 0 && sim.draws == 2, "one scene for same-target draws");
    active_write_fb = &fb;
    glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 4);           // framebuffer scene: old ended, new begun
    Require(sim.begins == 2 && sim.ends == 1 && sim.draws == 3 && sim.live.size() == 2, "switching ends one scene and begins one");
    glClear(GL_COLOR_BUFFER_BIT);
    GLint first[1] = {0}; GLsizei count[1] = {3};
    glMultiDrawArrays(GL_TRIANGLES, first, count, 1);
    Require(sim.begins == 2 && sim.ends == 1 && sim.draws == 5, "clear and multi draw stay in the scene");
    active_write_fb = nullptr;
    glBlitNamedFramebuffer((GLuint)(uintptr_t)&fb, 0, 0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    Require(sim.begins == 3 && sim.ends == 2 && sim.draws == 6 && active_write_fb == nullptr, "blit draws on the display scene");
    scene_not_flushed = GL_TRUE; glFinish();
    Require(sim.finishes >= 1 && sim.ends == 3 && sim.begins == 4 && sim.bad_ends == 0 && sim.bad_begins == 0, "finish flushes through a fresh scene as before");
    EndFrame();
    Require(sim.ends == 4 && sim.bad_ends == 0 && sim.aborts == 0 && sim.poly_restores == 4, "frame end ends the last scene; polygon mode restored by the four primitive draws");
    ReleaseLive();
}

int main() {
    TestDepthCreation();
    TestSceneDepthFailureAndRecovery();
    TestOpenSceneEndedOnceThenFailure();
    TestBeginSceneFailure();
    TestRenderTargetFailure();
    TestBoundedRetryAndFailStop();
    TestNothingFails();
    std::puts("PASS: actual driver text for scene entry, depth/stencil creation, flush/finish and draw/clear/blit callers; host stubs only");
}
