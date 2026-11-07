// Host test of vitaGL patch 0014 (tools/vitagl/patches/0014-vgx-draw-caches.patch): the real helper text of custom_shaders.c
// (the block from "---- vgx draw caches" to the end of vgl_upload_frag_unifs, extracted by tools/test_vgl_drawcache.sh) against
// models of the uniform ring, the shader patcher and the GPU.
//
// Uniform-ring re-use (VGL_DRAWCACHE_UNIREUSE): a random script of program switches (glUseProgram's dirty flags), uniform writes
// to the current program and to other programs, draws (upload_uniforms) and frame ends (all dirty) runs over a small two-arena
// ring that rolls over as UNIFORM_PINGPONG does and scribbles over an arena when it is entered again. At every draw the buffers
// sceGxm would read must hold the program's values, and every draw's buffers must still hold them when their arena is entered
// again (the GPU reads them up to then). The same script with the re-use off must give the same values and use at least as
// many copies.
// Fragment-program memo (VGL_DRAWCACHE_FRAGMEMO): for a random script of blend / target-mode changes, the program's fragment
// program after setup_frag_program must be what the patcher would return for (program, blend, mode), with and without the memo.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <tuple>
#include <vector>

typedef unsigned char GLboolean;
typedef unsigned int GLuint;
#define GL_TRUE 1
#define GL_FALSE 0
typedef int SceGxmShaderPatcherId;
struct SceGxmFragmentProgram { int id; };
struct SceGxmProgram { int id; };
struct SceGxmContext { int dummy; };
enum SceGxmOutputRegisterFormat { SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 = 0, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 = 1 };
enum SceGxmMultisampleMode { SCE_GXM_MULTISAMPLE_NONE = 0, SCE_GXM_MULTISAMPLE_2X = 1, SCE_GXM_MULTISAMPLE_4X = 2 };
#define VGL_DRAWCACHE_FRAGMEMO 1
#define VGL_DRAWCACHE_UNIREUSE 2
#define VGL_FRAG_MEMO 4
#define MAX_CUSTOM_PROGRAMS 8
#define FBO_MSAA 1

struct shader { SceGxmShaderPatcherId id; const SceGxmProgram* prog; uint32_t unif_buf_size; };
typedef union { uint32_t info; uint32_t raw; } blend_config;
struct program {
    shader *vshader, *fshader;
    SceGxmFragmentProgram* fprog;
    void *unif_vbuffer, *unif_fbuffer;
    uint32_t frag_memo_blend[VGL_FRAG_MEMO];
    uint8_t frag_memo_mode[VGL_FRAG_MEMO];
    SceGxmFragmentProgram* frag_memo_prog[VGL_FRAG_MEMO];
    uint8_t frag_memo_next;
    void *ring_v, *ring_f;
    uint32_t ring_gen_v, ring_gen_f, ring_epoch_v, ring_epoch_f;
};

// ---- models of what the helpers call
static program progs[MAX_CUSTOM_PROGRAMS];
static GLuint cur_program = 0;
static blend_config blend_info;
static SceGxmMultisampleMode cur_msaa = SCE_GXM_MULTISAMPLE_NONE, msaa_mode = SCE_GXM_MULTISAMPLE_NONE;
static SceGxmContext ctx_storage, *gxm_context = &ctx_storage;
static void *vgl_def_vert_buf = nullptr, *vgl_def_frag_buf = nullptr;
static uint32_t vgl_def_vert_buf_size = 0, vgl_def_frag_buf_size = 0;
uint32_t vgl_unif_pool_gen = 1;
static void* gxm_vert = nullptr; // what sceGxm would read at a draw
static void* gxm_frag = nullptr;
static void sceGxmSetVertexDefaultUniformBuffer(SceGxmContext*, void* p) { gxm_vert = p; }
static void sceGxmSetFragmentDefaultUniformBuffer(SceGxmContext*, void* p) { gxm_frag = p; }
#define vgl_fast_memcpy std::memcpy
#define vgl_memset std::memset

// The ring: two arenas used in turn (UNIFORM_PINGPONG); an arena entered again is scribbled over (as later reservations would).
constexpr uint32_t kArena = 1024;
static uint8_t g_arena[2][kArena];
static int g_cur = 0;
static uint32_t g_idx = 0;
struct DrawRecord { const uint8_t* v; std::vector<uint8_t> vv; const uint8_t* f; std::vector<uint8_t> fv; int arenaV, arenaF; };
static std::vector<DrawRecord> g_inFlight;
static int ArenaOf(const void* p) {
    for (int a = 0; a < 2; ++a)
        if (static_cast<const uint8_t*>(p) >= g_arena[a] && static_cast<const uint8_t*>(p) < g_arena[a] + kArena) return a;
    return -1;
}
static unsigned g_rollovers = 0;
static void* Reserve(uint32_t size) {
    const uint32_t r = (size + 15u) & ~15u;
    if (r > kArena - g_idx) {
        // the GPU must be done with every draw that reads the arena about to be entered: check that those draws' data survived
        const int next = 1 - g_cur;
        for (const DrawRecord& d : g_inFlight) {
            if (d.arenaV == next && d.v && std::memcmp(d.v, d.vv.data(), d.vv.size()) != 0) { std::fprintf(stderr, "FAIL: a draw's vertex uniforms changed before the GPU read them\n"); std::exit(1); }
            if (d.arenaF == next && d.f && std::memcmp(d.f, d.fv.data(), d.fv.size()) != 0) { std::fprintf(stderr, "FAIL: a draw's fragment uniforms changed before the GPU read them\n"); std::exit(1); }
        }
        std::vector<DrawRecord> keep;
        for (DrawRecord& d : g_inFlight)
            if (d.arenaV != next && d.arenaF != next) keep.push_back(std::move(d));
        g_inFlight.swap(keep);
        std::memset(g_arena[next], 0xCD, kArena);
        // (as the pingpong rollover: the current defaults are copied to the start of the arena entered)
        uint32_t idx = 0;
        if (vgl_def_vert_buf) { std::memmove(g_arena[next], vgl_def_vert_buf, vgl_def_vert_buf_size); vgl_def_vert_buf = g_arena[next]; idx = (vgl_def_vert_buf_size + 15u) & ~15u; }
        if (vgl_def_frag_buf) { std::memmove(g_arena[next] + idx, vgl_def_frag_buf, vgl_def_frag_buf_size); vgl_def_frag_buf = g_arena[next] + idx; idx += (vgl_def_frag_buf_size + 15u) & ~15u; }
        gxm_vert = vgl_def_vert_buf;
        gxm_frag = vgl_def_frag_buf;
        g_cur = next;
        g_idx = idx;
        ++g_rollovers;
        vgl_unif_pool_gen++; // (the patched vglReserveUniformCircularPoolBuffer)
    }
    void* p = g_arena[g_cur] + g_idx;
    g_idx += r;
    return p;
}
static void* vglReserveVertexUniformBuffer(uint32_t size) {
    vgl_def_vert_buf = Reserve(size);
    vgl_def_vert_buf_size = size;
    gxm_vert = vgl_def_vert_buf;
    return vgl_def_vert_buf;
}
static void* vglReserveFragmentUniformBuffer(uint32_t size) {
    vgl_def_frag_buf = Reserve(size);
    vgl_def_frag_buf_size = size;
    gxm_frag = vgl_def_frag_buf;
    return vgl_def_frag_buf;
}
// The shader patcher: one object per (program id, format, msaa, blend, vertex program), counted calls.
static std::map<std::tuple<int, int, int, uint32_t, const void*>, SceGxmFragmentProgram*> g_patched;
static std::deque<SceGxmFragmentProgram> g_patchedStore; // (stable addresses, freed at exit)
static unsigned g_patcherCalls = 0;
static void Patch(SceGxmShaderPatcherId id, SceGxmOutputRegisterFormat fmt, SceGxmMultisampleMode msaa, const uint32_t* blend, const SceGxmProgram* link, SceGxmFragmentProgram** out) {
    ++g_patcherCalls;
    auto key = std::make_tuple(id, static_cast<int>(fmt), static_cast<int>(msaa), *blend, static_cast<const void*>(link));
    auto it = g_patched.find(key);
    if (it == g_patched.end()) {
        g_patchedStore.push_back(SceGxmFragmentProgram{static_cast<int>(g_patched.size())});
        it = g_patched.emplace(key, &g_patchedStore.back()).first;
    }
    *out = it->second;
}
#define rebuild_frag_shader(x, y, z, w) Patch(x, w, cur_msaa, &blend_info.info, z, y)

#include "vgl_drawcache_functions.inc"

// ---- the script
static uint8_t g_vbuf[MAX_CUSTOM_PROGRAMS][160], g_fbuf[MAX_CUSTOM_PROGRAMS][48];
static shader g_vs[MAX_CUSTOM_PROGRAMS], g_fs[MAX_CUSTOM_PROGRAMS];
static GLboolean dirty_v = GL_TRUE, dirty_f = GL_TRUE;
struct uniform { void *vptr, *fptr; };

static void Setup() {
    std::memset(progs, 0, sizeof progs);
    for (int i = 0; i < MAX_CUSTOM_PROGRAMS; ++i) {
        g_vs[i] = shader{10 + i, reinterpret_cast<const SceGxmProgram*>(0x1000 + i), static_cast<uint32_t>(32 + 16 * (i % 8))};
        g_fs[i] = shader{20 + i % 3, nullptr, static_cast<uint32_t>(16 + 8 * (i % 4))};
        std::memset(g_vbuf[i], 0, sizeof g_vbuf[i]);
        std::memset(g_fbuf[i], 0, sizeof g_fbuf[i]);
        progs[i].vshader = &g_vs[i];
        progs[i].fshader = &g_fs[i];
        progs[i].unif_vbuffer = g_vbuf[i];
        progs[i].unif_fbuffer = g_fbuf[i];
        vgl_reset_draw_caches(&progs[i]);
    }
    std::memset(g_arena, 0, sizeof g_arena);
    g_cur = 0; g_idx = 0; g_inFlight.clear(); g_rollovers = 0;
    vgl_def_vert_buf = vgl_def_frag_buf = nullptr;
    gxm_vert = gxm_frag = nullptr;
    vgl_unif_pool_gen = 1;
    cur_program = 0;
    dirty_v = dirty_f = GL_TRUE;
    for (uint32_t& s : vgl_dc_stats) s = 0;
}
// glUniform*: vgl_fill_uniform_data's write (vertex part, fragment part or an aliased pair)
static void Write(int prog, int which, uint32_t off, uint8_t value) {
    uniform u{nullptr, nullptr};
    program& p = progs[prog];
    if (which & 1) u.vptr = static_cast<uint8_t*>(p.unif_vbuffer) + off % p.vshader->unif_buf_size;
    if (which & 2) u.fptr = static_cast<uint8_t*>(p.unif_fbuffer) + off % p.fshader->unif_buf_size;
    vgl_note_unif_write(u.vptr, u.fptr);
    if (u.vptr) { *static_cast<uint8_t*>(u.vptr) = value; dirty_v = GL_TRUE; }
    if (u.fptr) { *static_cast<uint8_t*>(u.fptr) = value; dirty_f = GL_TRUE; }
}
static void Draw(unsigned& checked) {
    if (!cur_program) return;
    program* p = &progs[cur_program - 1];
    if (dirty_v) { vgl_upload_vert_unifs(p); dirty_v = GL_FALSE; }
    if (dirty_f) { vgl_upload_frag_unifs(p); dirty_f = GL_FALSE; }
    if (std::memcmp(gxm_vert, p->unif_vbuffer, p->vshader->unif_buf_size) != 0) { std::fprintf(stderr, "FAIL: draw reads wrong vertex uniforms\n"); std::exit(1); }
    if (std::memcmp(gxm_frag, p->unif_fbuffer, p->fshader->unif_buf_size) != 0) { std::fprintf(stderr, "FAIL: draw reads wrong fragment uniforms\n"); std::exit(1); }
    DrawRecord d;
    d.v = static_cast<const uint8_t*>(gxm_vert); d.vv.assign(d.v, d.v + p->vshader->unif_buf_size); d.arenaV = ArenaOf(d.v);
    d.f = static_cast<const uint8_t*>(gxm_frag); d.fv.assign(d.f, d.f + p->fshader->unif_buf_size); d.arenaF = ArenaOf(d.f);
    assert(d.arenaV >= 0 && d.arenaF >= 0);
    g_inFlight.push_back(std::move(d));
    ++checked;
}
struct Result { unsigned draws = 0, copies = 0, reuses = 0, rollovers = 0; };
static Result RunUniforms(uint32_t seed, uint32_t flags) {
    Setup();
    vglSetDrawCaches(flags);
    std::mt19937 rng(seed);
    Result r;
    for (int step = 0; step < 20000; ++step) {
        const unsigned op = rng() % 100;
        if (op < 25) { // glUseProgram (vitaGL: both flags dirty on a switch)
            const GLuint np = 1 + rng() % 5;
            if (np != cur_program) { cur_program = np; dirty_v = dirty_f = GL_TRUE; }
        } else if (op < 40) { // the backend's glUniform for the current program (rarer in odd seeds: long-lived ring copies)
            if (cur_program && ((seed & 1) == 0 || rng() % 8 == 0)) Write(static_cast<int>(cur_program - 1), 1 + rng() % 3, rng(), static_cast<uint8_t>(rng()));
        } else if (op < 42) { // a uniform of another program (vitaGL takes any location)
            Write(static_cast<int>(rng() % 5), 1 + rng() % 3, rng(), static_cast<uint8_t>(rng()));
        } else if (op < 43) { // frame end / glFinish: "dirty to be safe"
            dirty_v = dirty_f = GL_TRUE;
        } else if (op < 44) { // a write with no program in use
            const GLuint saved = cur_program;
            cur_program = 0;
            Write(static_cast<int>(rng() % 5), 1, rng(), static_cast<uint8_t>(rng()));
            cur_program = saved;
        } else {
            Draw(r.draws);
        }
    }
    r.copies = vgl_dc_stats[2] + vgl_dc_stats[4];
    r.reuses = vgl_dc_stats[3] + vgl_dc_stats[5];
    r.rollovers = g_rollovers;
    return r;
}

static void RunFragMemo(uint32_t seed) {
    std::mt19937 rng(seed);
    for (int on = 0; on < 2; ++on) {
        Setup();
        vglSetDrawCaches(on ? VGL_DRAWCACHE_FRAGMEMO : 0);
        g_patcherCalls = 0;
        std::mt19937 r2(seed);
        // what the macro keeps per program (blend_info, is_fbo_float, msaa) before it calls the helper
        uint32_t lastBlend[MAX_CUSTOM_PROGRAMS];
        int lastMode[MAX_CUSTOM_PROGRAMS];
        for (int i = 0; i < MAX_CUSTOM_PROGRAMS; ++i) { lastBlend[i] = ~0u; lastMode[i] = -1; }
        for (int step = 0; step < 20000; ++step) {
            const int pi = static_cast<int>(r2() % 6);
            if (r2() % 50 == 0) { vgl_reset_draw_caches(&progs[pi]); lastBlend[pi] = ~0u; lastMode[pi] = -1; } // a re-link
            blend_info.raw = r2() % 5;                                      // a few blends in turn
            const bool half = r2() % 7 == 0;                                // a float target now and then
            cur_msaa = static_cast<SceGxmMultisampleMode>(r2() % 9 == 0 ? 2 : 0);
            const int mode = (half ? 1 : 0) | (static_cast<int>(cur_msaa) << 1);
            program* p = &progs[pi];
            if (lastBlend[pi] != blend_info.raw || lastMode[pi] != mode) {
                lastBlend[pi] = blend_info.raw;
                lastMode[pi] = mode;
                vgl_patch_frag_program(p, half ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
            }
            auto key = std::make_tuple(p->fshader->id, half ? 1 : 0, static_cast<int>(cur_msaa), blend_info.raw, static_cast<const void*>(p->vshader->prog));
            auto it = g_patched.find(key);
            if (it == g_patched.end() || p->fprog != it->second) { std::fprintf(stderr, "FAIL: fragment program differs from the patcher's (memo %d, step %d)\n", on, step); std::exit(1); }
        }
        std::printf("fragment memo %s: %u patcher calls, %u memo hits\n", on ? "on " : "off", g_patcherCalls, vgl_dc_stats[1]);
    }
    (void)rng;
}

int main() {
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        const Result off = RunUniforms(seed, 0);
        const Result on = RunUniforms(seed, VGL_DRAWCACHE_UNIREUSE);
        if (on.draws != off.draws) { std::fprintf(stderr, "FAIL: scripts differ\n"); return 1; }
        if (on.copies > off.copies || off.reuses != 0) { std::fprintf(stderr, "FAIL: re-use made more copies\n"); return 1; }
        if (seed == 1)
            std::printf("uniform re-use: %u draws checked; off: %u copies; on: %u copies, %u re-uses; rollovers off/on %u/%u\n", on.draws, off.copies, on.copies,
                        on.reuses, off.rollovers, on.rollovers);
    }
    RunFragMemo(7);
    std::printf("PASS: vitaGL draw caches (patch 0014)\n");
    return 0;
}
