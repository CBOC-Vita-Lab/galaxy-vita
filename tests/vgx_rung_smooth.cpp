// smooth23 (Galaxy Vita 2.3): the cheaper change of picture size, host checks against the GL stub (the console's code path, __VGX_VITA__).
//   GXV_RUNG_PREMULTI: one premade texture per texture a change resamples (a size listed more than once), each on its own pooled object;
//   an object held for a premade texture is never taken for another one. GXV_RUNG_RSFREE: a resample without a premade texture draws
//   through the least used object of its size in the frame before, or a new one when all are at the 8-scene limit.
// Build (from the repository root):
//   g++ -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -Iarmhf/glstub -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src
//       -Iplatform/vgx/third_party tests/vgx_rung_smooth.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_autoq.cpp
//       -o /tmp/rung-smooth -lpthread && /tmp/rung-smooth && GXV_RUNG_RSFREE=0 GXV_RUNG_PREMULTI=0 /tmp/rung-smooth off
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <unistd.h>

namespace {
unsigned scene = 0, loading = 0, quiet = 1;
std::vector<std::string> lines;
void Require(bool good, const char* why) {
    if (good) return;
    std::fprintf(stderr, "FAIL: %s\n", why);
    std::exit(1);
}
}
extern "C" {
uint32_t port_scene_key(void) { return scene; }
uint32_t port_scene_loading_key(void) { return loading; }
int port_scene_loading_active(void) { return loading != 0; }
int port_screen_quiet(void) { return quiet; }
void port_log(const char*, ...) {}
void port_log_summary(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
}
}
namespace vgx { bool LogEnabled() { return false; } }
#ifdef __VGX_VITA__
#include <psp2/kernel/threadmgr.h>
// vitaGL's program table (1,024 on the console; tableSize here) and the few entry points the GL stub leaves to the driver.
static unsigned tableSize = 1024;
extern "C" {
unsigned vgx_stub_name_ = 0;
unsigned glCreateShader(unsigned) { return ++vgx_stub_name_; }
unsigned vglGetFreeProgramSlots(void) {
    unsigned live = 0;
    for (const auto& kv : vgx::backend::g_programs) if (kv.second.id) ++live;
    return live >= tableSize ? 0 : tableSize - live;
}
unsigned glCreateProgram(void) { return vglGetFreeProgramSlots() ? ++vgx_stub_name_ : ~0u; }
int glGetUniformLocation(unsigned, const char*) { return 1; }
unsigned glCheckFramebufferStatus(unsigned) { return 0x8CD5; }
unsigned glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned name) {
    return reinterpret_cast<const unsigned char*>(name == 0x1F03 ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : "stub");
}
size_t vglMemFree(int) { return 64u << 20; }
size_t vglMemTotal(int) { return 128u << 20; }
const void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<const void*>(1); }
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) { return 0; }
unsigned char vgxvita_maskGuardOn = 0, vgxvita_maskGuardDue = 0;
void vgxvita_maskGuard(void) {}
std::uint64_t sceKernelGetProcessTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { std::abort(); }
int sceKernelSignalSema(SceUID, int) { std::abort(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(SceUID, SceSize, void*) { std::abort(); }
int sceKernelDelayThread(SceUInt) { std::abort(); }
}
namespace vgx::host { bool CreateContext() { return true; } }
#endif

using namespace vgx;
using namespace vgx::backend;

static int PoolOf(GLuint tex) {
    for (size_t i = 0; i < g_pool.size(); ++i) if (g_pool[i].attached == tex) return static_cast<int>(i);
    return -1;
}
static GLuint NewTex() { GLuint t = 0; glGenTextures(1, &t); return t; }

int main(int argc, char** argv) {
    const bool off = argc > 1 && std::strcmp(argv[1], "off") == 0;
    Require(Init(32, 32), "back end initialised against the GL stub");
    // 1. Two premade textures of one size: each on its own pooled object, both held; the second is not the first taken back.
    Require(RungBuildSize(64, 32) && RungBuildSize(64, 32), "two premade textures of 64x32 made");
    Require(g_rungPre.size() == 2 && g_rungPre[0].tex != g_rungPre[1].tex, "two distinct premade textures");
    const int p0 = PoolOf(g_rungPre[0].tex), p1 = PoolOf(g_rungPre[1].tex);
    if (!off) {
        Require(p0 >= 0 && p1 >= 0 && p0 != p1, "each premade texture attached to its own pooled object");
        Require((g_pool[p0].held & PoolHeld::RUNG) && (g_pool[p1].held & PoolHeld::RUNG), "both objects held");
        // an ordinary copy target of that size does not take a held object
        const GLuint other = NewTex();
        AcquireFbo(64, 32, other, false);
        const int po = PoolOf(other);
        Require(po >= 0 && po != p0 && po != p1, "a copy of the same size gets an object of its own");
        Require(PoolOf(g_rungPre[0].tex) == p0 && PoolOf(g_rungPre[1].tex) == p1, "the premade textures stay attached");
    } else {
        // (the 2.2 way: the second premade texture is attached to the first one's object, which then holds none)
        Require(p1 >= 0 && PoolOf(g_rungPre[0].tex) < 0, "GXV_RUNG_PREMULTI=0: the second premade texture takes the first one's object");
    }
    // the change takes them one after the other
    const GLuint a = NewRenderTexture(64, 32), b = NewRenderTexture(64, 32);
    Require(a != b && g_rungPre.empty(), "a change takes both premade textures");
    // 2. RungPreSizes: the kept picture's size once, then one per resampled texture of a size, at most 3 of a size.
    g_efbW = 640; g_efbH = 456;
    for (int i = 0; i < 4; ++i) {
        Tex& t = TexSlotFor(200 + i);
        t.id = NewTex(); t.renderTarget = true; t.ew = 640; t.eh = 456; t.w = 960; t.h = 544;
    }
    { Tex& t = TexSlotFor(210); t.id = NewTex(); t.renderTarget = true; t.ew = 320; t.eh = 228; t.w = 480; t.h = 272; }
    int nw = 0, nh = 0;
    std::vector<std::pair<int, int>> sizes;
    RungPreSizes(2, nw, nh, sizes);
    size_t full = 0, half = 0;
    for (const auto& sz : sizes) { full += (sz.first == nw && sz.second == nh) ? 1u : 0u; half += (sz.first != nw) ? 1u : 0u; }
    std::printf("  rung 2: %dx%d, sizes listed %u (full size %u, others %u)\n", nw, nh, static_cast<unsigned>(sizes.size()), static_cast<unsigned>(full),
                static_cast<unsigned>(half));
    if (!off) Require(full == 3 && half == 1, "GXV_RUNG_PREMULTI: the full size 3 times (kept picture + 2 more, the cap), the half size once");
    else Require(full == 1 && half == 1, "GXV_RUNG_PREMULTI=0: each size once");
    for (int i = 0; i < 4; ++i) TexSlotFor(200 + i) = Tex{};
    TexSlotFor(210) = Tex{};
    // 3. Resample without a premade texture: the least used object of the size in the frame before, or a new one when all are at the limit.
    const GLuint s1 = NewTex(), s2 = NewTex(), src = NewTex();
    const size_t base = g_pool.size();
    AcquireFbo(100, 50, s1, false);
    AcquireFbo(100, 50, s2, false); // (s1's object is free: the same object takes s2 unless its uses are at the limit)
    // make two objects of 100x50 by hand use counts
    g_fboMaxUses = 1;
    const GLuint s3 = NewTex();
    AcquireFbo(100, 50, s3, false); // (limit 1: a second object)
    g_fboMaxUses = 8;
    int busy = -1, idle = -1;
    for (size_t i = base; i < g_pool.size(); ++i) if (g_pool[i].w == 100) { if (busy < 0) busy = static_cast<int>(i); else idle = static_cast<int>(i); }
    Require(busy >= 0 && idle >= 0, "two objects of 100x50");
    for (PoolFbo& f : g_pool) { f.prevUses = f.uses; f.uses = 0; } // (CMD_PRESENT)
    g_pool[busy].prevUses = 8; g_pool[idle].prevUses = 2;
    const GLuint d1 = NewTex();
    Resample(src, d1, 100, 50);
    if (!off) Require(PoolOf(d1) == idle, "GXV_RUNG_RSFREE: the resample draws through the object used less in the frame before");
    else Require(PoolOf(d1) == busy, "GXV_RUNG_RSFREE=0: the first object of the size, whatever its uses");
    for (PoolFbo& f : g_pool) { f.uses = 0; if (f.w == 100) f.prevUses = 8; }
    const size_t before = g_pool.size();
    const GLuint d2 = NewTex();
    Resample(src, d2, 100, 50);
    if (!off) Require(g_pool.size() == before + 1 && PoolOf(d2) == static_cast<int>(before) && g_rsFreeNew == 1,
                      "GXV_RUNG_RSFREE: every object of the size at the limit: a new one");
    else Require(g_pool.size() == before, "GXV_RUNG_RSFREE=0: no new object");
    std::printf("PASS rung smooth (%s)\n", off ? "switches off: the 2.2 behaviour" : "switches on");
    return 0;
}
