// stall27: GXV_RES_ADOPT in the back end on the GL stub (preamble copied from noprog26's tests/vgx_play_compile_backend.cpp): a copy's own texture adopts an exact-size premade
// object (AcquireFbo), a premade pass object is counted when taken, a size mismatch makes a new object and leaves the premade alone, the premade texture of a size change and of the
// guard path is taken, port_res_request queues and the closed-screen step makes the target. tools/test_res_adopt.sh
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
#ifdef C185_ASTRO_MASK
static size_t c185VramFree = 64u << 20, c185VramTotal = 128u << 20;
#endif
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
size_t vglMemFree(int) {
#ifdef C185_ASTRO_MASK
    return c185VramFree;
#else
    return 64u << 20;
#endif
}
size_t vglMemTotal(int) {
#ifdef C185_ASTRO_MASK
    return c185VramTotal;
#else
    return 128u << 20;
#endif
}
#ifndef C185_ASTRO_MASK
const void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<const void*>(1); }
#endif
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) {
#ifdef C185_ASTRO_MASK
    return vgx::backend::TgtW();
#else
    return 0;
#endif
}
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



#include <set>
// (symbols of other translation units this base's back end links against; none is used by the test's draws)
namespace vgx {
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t out[7]) { for (int i = 0; i < 7; ++i) out[i] = 0; }
void VtxPackDrawCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
bool PrepassAny() { return false; }
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
}
namespace fake { std::set<std::string> cache; int probes = 0; }
extern "C" {
int vgxvita_asyncShaderInCache_v1(const char* joined, int) { ++fake::probes; return fake::cache.count(joined) ? 1 : 0; }
}
using namespace vgx;
using namespace vgx::backend;
extern "C" int port_res_request(int w, int h, int kind, int depth);
static void PreMade(int w, int h, GLuint tex, bool depth) {
    PoolFbo f; f.w = w; f.h = h; f.fbo = 100 + static_cast<GLuint>(g_pool.size()); f.attached = tex; f.held = PoolHeld::PRIME; f.keepUntil = ~0ull;
    if (depth) f.depthTex = 900 + tex;
    g_pool.push_back(f);
    if (tex) g_resPre.push_back(ResPre{w, h, tex, ~0ull});
}
int main() {
    setenv("GXV_PREWARM_RES", "1", 1);
    setenv("GXV_STAGEWARM", "0", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    Require(PrewarmResMode() == 1 && ResAdoptOn(), "GXV_PREWARM_RES=1 and GXV_RES_ADOPT default on");
    g_pool.clear(); g_resPre.clear();
    // A. exact size: the copy's own texture 200 adopts the premade 240x191 object (texture 100); the premade texture goes
    PreMade(240, 191, 100, false);
    const unsigned madeBefore = g_poolMade;
    int idx = AcquireFbo(240, 191, 200, false);
    Require(idx == 0 && g_pool[0].attached == 200, "A: the premade object now carries the copy's texture");
    Require(g_poolMade == madeBefore, "A: no object made");
    Require(g_resPre.empty() && g_resAdoptSession == 1 && g_rc.adopted == 1, "A: premade entry consumed, census counted");
    Require(!(g_pool[0].held & PoolHeld::PRIME), "A: the object is in ordinary use now");
    // B. a size that does not match: a new object, the premade stays untouched
    g_pool.clear(); g_resPre.clear();
    PreMade(240, 191, 101, false);
    idx = AcquireFbo(240, 136, 201, false);
    Require(idx == 1 && g_pool.size() == 2 && g_resPre.size() == 1 && g_pool[0].attached == 101 && (g_pool[0].held & PoolHeld::PRIME), "B: size mismatch makes a new object, premade kept");
    // C. depth: a premade with depth is preferred for a depth wish, one without is still adopted for it (the depth is made in the call), never the other way round by mistake
    g_pool.clear(); g_resPre.clear();
    PreMade(480, 272, 102, false); PreMade(480, 272, 103, true);
    idx = AcquireFbo(480, 272, 202, true);
    Require(idx == 1 && g_pool[1].attached == 202 && g_resPre.size() == 1 && g_resPre[0].tex == 102, "C: the premade with depth is taken for a depth wish");
    // D. an object in use (uses at the limit) or a rung-held one is not adopted
    g_pool.clear(); g_resPre.clear();
    PreMade(120, 68, 104, false);
    g_pool[0].held |= PoolHeld::RUNG;
    const size_t n0 = g_pool.size();
    idx = AcquireFbo(120, 68, 204, false);
    Require(g_pool.size() == n0 + 1 && idx == static_cast<int>(n0), "D: a rung-held premade object is not adopted");
    // E. a pass object premade by ResBuildObj (no texture of its own in the premade list) is taken by a pass and counted
    g_pool.clear(); g_resPre.clear();
    PreMade(120, 68, 0, true);
    g_pool[0].color = 555; g_pool[0].attached = 555;
    const unsigned passBefore = g_resPassSession;
    idx = AcquireFbo(120, 68, 0, true);
    Require(idx == 0 && g_resPassSession == passBefore + 1 && g_rc.passTaken >= 1, "E: a premade pass object is taken and counted");
    // F. size change: TakeResPre gives the premade texture
    g_resPre.clear();
    g_resPre.push_back(ResPre{480, 272, 300, ~0ull});
    Require(TakeResPre(480, 272) == 300 && g_resPre.empty(), "F: TakeResPre takes the exact size");
    Require(TakeResPre(480, 272) == 0, "F: and only once");
    // G. requests: queued, deduplicated, bounded; refused with the feature off sizes
    Require(port_res_request(240, 191, 0, 0) == 1 && port_res_request(240, 191, 0, 0) == 1 && g_resReq.size() == 1, "G: a request is queued once");
    Require(port_res_request(4, 4, 0, 0) == 0 && port_res_request(100, 100, 7, 0) == 0, "G: nonsense refused");
    for (int i = 0; i < 20; ++i) port_res_request(64 + i, 64, 1, 1);
    Require(g_resReq.size() == 8, "G: at most 8 wait");
    // H. a request is made by the closed-screen step
    g_resReq.clear(); g_pool.clear(); g_resPre.clear();
    port_res_request(212, 169, 0, 0);
    ResReqStep(900);
    Require(g_resReqMade + g_resReqFailed >= 1, "H: the step handled the request");
    std::puts("res_adopt backend test: ok");
    return 0;
}
