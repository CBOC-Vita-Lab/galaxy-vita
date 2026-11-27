// revA (review of the 12 Nov patches): three interactions of noprog26 / nohitch26 with a worker, on the GL stub: a worker that FAILED hands its programs back to the spaced gate, a busy worker
// cannot hold a draw for ever (GXV_NOPROG_BUSY_MAXWAIT), a stencil/occlusion (player silhouette) draw is never held. Same harness as tests/vgx_play_compile_worker.cpp.
// (copy of that test's header follows)
// noprog26: GXV_NO_PLAY_COMPILE_STOP in the back end on the GL stub (the gate without a worker: a CMD_SHADER in play, spaced compiles, the progress picture). tools/test_play_compile.sh
// (the stub and fake probe are copied from nohitch26's tests/vgx_async_link_backend.cpp; its header follows)
// nohitch26: GXV_ASYNC_LINK in the back end (vgx_backend_gl.cpp GetProgram / DrawBody / AlStandIn / AlStageEnd) on the GL stub, built as the console builds it (__VGX_VITA__),
// with a fake cache probe (no worker: GXV_ASYNC_SHADERS stays off). Run as   vgx_async_link_backend 0|1|2   (the mode).
//   0: every draw's program links in its own picture, nothing is counted (the old behaviour).
//   1: in play a program a draw may wait for links one a picture; the draws that wait are skipped; Mario / HUD / map draws link at once and are counted.
//   2: the same, but a draw that waits is drawn with a linked program of the same vertex shader.
// "on" checks: a draw whose program is not in the cache is skipped and the frame does not wait for the compile (every frame is quick); the
// draws that must not be skipped (no depth test, i.e. the HUD and 2D; the opaque cached geometry; a draw into a texture) link at once; a
// program in the cache links at once; the announcements (CMD_PROGRAM / CMD_VARIANT) never compile on the render thread; the skipped
// program is linked by a later frame once the worker is done, and drawn; the compile ran on another thread; one compile per pair; the 5 s line.
//   tools/test_vgx_async_shaders.sh
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
#undef vglGetTexDataPointer  // (tests31: tests/play_compile_glshim.h wraps calls; the definition is the stub's own)
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
#include <mutex>
#include <thread>
#include <chrono>
namespace fake { std::set<std::string> cache; std::mutex mu; int probes = 0; std::atomic<int> workerCompiles{0}; std::atomic<int> workerMs{60}; std::atomic<bool> workerOnRender{false}; std::atomic<bool> failCompile{false}; std::thread::id renderThread; }
extern "C" {
int vgxvita_asyncShaderInCache_v1(const char* joined, int) { std::lock_guard<std::mutex> lk(fake::mu); ++fake::probes; return fake::cache.count(joined) ? 1 : 0; }
// the worker: a thread of this process that takes a while per pair and then "writes the cache files"
int vgxvita_asyncShaderCompile_v1(const char* vs, const char* fs, int, int* compiled) {
    if (std::this_thread::get_id() == fake::renderThread) fake::workerOnRender = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(fake::workerMs.load()));
    if (fake::failCompile.load()) return -2;   // (the heap guard of vgxvita_asyncShaderCompile_v1, or a compiler error: nothing is written)
    std::lock_guard<std::mutex> lk(fake::mu);
    fake::cache.insert(vs);
    fake::cache.insert(fs);
    ++fake::workerCompiles;
    if (compiled) *compiled = 2;
    return 0;
}
int vgxvita_asyncShaderStart_v1(void (*fn)(void*), void* arg, char* where, size_t n) {
    std::snprintf(where, n, "a test thread");
    std::thread(fn, arg).detach();
    return 1;
}
void vgxvita_asyncShaderBoost_v1(int) {}
}
using namespace vgx;
using namespace vgx::backend;

static ShaderSource g_vsrc[4], g_fsrc[64];
static void RegisterAll() {
    for (uint16_t i = 0; i < 4; ++i) {
        g_vsrc[i].id = i; g_vsrc[i].keyHash = 0x1000 + i;
        g_vsrc[i].text = "attribute vec4 a_pos; uniform mat4 u_proj; void main(){gl_Position=u_proj*a_pos;} // vs " + std::to_string(i);
        RegisterShader(&g_vsrc[i]);
    }
    for (uint16_t i = 0; i < 64; ++i) {
        g_fsrc[i].id = i; g_fsrc[i].fragment = true; g_fsrc[i].keyHash = 0x2000 + i;
        g_fsrc[i].text = "precision mediump float; void main(){gl_FragColor=vec4(1.0);} // fs " + std::to_string(i);
        g_fsrc[i].nTev = 28;
        RegisterShader(&g_fsrc[i]);
    }
}
static std::string JoinedVs(unsigned i) { return "#version 100\n" + g_vsrc[i].text; }
static std::string JoinedFs(unsigned i) { return "#version 100\n" + g_fsrc[i].text; }
enum DrawKind { Hud, Effect, Occ };
static CmdBuf Frame(uint64_t frame, const std::vector<std::pair<unsigned, unsigned>>& draws, const std::vector<DrawKind>& kinds, const std::vector<const ShaderSource*>& shaders = {}) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2};
    b.uni = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (size_t i = 0; i < shaders.size(); ++i) { b.shaders.push_back(shaders[i]); b.cmds.push_back({CMD_SHADER, static_cast<uint32_t>(i)}); }
    for (size_t i = 0; i < draws.size(); ++i) {
        DrawRec d;
        d.vsId = static_cast<uint16_t>(draws[i].first);
        d.fsId = static_cast<uint16_t>(draws[i].second);
        d.idxCount = d.vtxCount = 3;
        d.ublock[UB_PROJ] = 0;
        d.layoutMask = 0;
        if (kinds[i] == Effect || kinds[i] == Occ) { d.rs.depthTest = 1; d.rs.blend = 1; d.rs.blendSrc = BF_SRC_ALPHA; d.rs.blendDst = BF_INV_SRC_ALPHA; }
        if (kinds[i] == Occ) d.occ = 1;
        b.draws.push_back(d);
        b.cmds.push_back({CMD_DRAW, static_cast<uint32_t>(i)});
    }
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
static bool Linked(unsigned vs, unsigned fs) {
    const auto it = g_programs.find((vs << 16) | fs);
    return it != g_programs.end() && it->second.ok;
}
static void Play(bool on) { g_screenLive = true; g_stageTarget = g_stageRecKey = on ? 0xA : 0; g_menuRec = !on; }
static unsigned g_hookCalls = 0;
static void CountHook(uint32_t, uint32_t) { ++g_hookCalls; }

static std::string CacheKey(bool fs, unsigned i) { return fs ? JoinedFs(i) : JoinedVs(i); }
static void Settle(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int main() {
    fake::renderThread = std::this_thread::get_id();
    setenv("GXV_NO_PLAY_COMPILE_STOP", "1", 1);
    setenv("GXV_STAGE_PRECOMPILE", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    setenv("GXV_NOPROG_BUSY_MAXWAIT", "20", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_MAXPROGRAMS", "1000", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    for (unsigned i = 0; i < 4; ++i) fake::cache.insert(JoinedVs(i));
    for (unsigned i = 0; i < 40; ++i) fake::cache.insert(JoinedFs(i));
    scene = 0xA; loading = 0; quiet = 0;
    uint64_t frame = 1;
    Play(false);
    Execute(Frame(frame++, {{2, 1}}, {Hud}));
    Require(g_async != nullptr, "the worker exists");
    Play(true);

    // X. the worker FAILED two programs (compile error, or the heap guard's -2): the render thread must not compile them one a picture; the spaced gate (one a GXV_ASYNC_LINK_MISSGAP) owns them
    fake::failCompile = true; fake::workerMs = 20;
    Execute(Frame(frame++, {{2, 46}, {2, 47}}, {Effect, Effect}, {&g_fsrc[46], &g_fsrc[47]}));
    unsigned n = 0;
    while ((g_async->StateOf((2u << 16) | 46) != asyncsh::State::Failed || g_async->StateOf((2u << 16) | 47) != asyncsh::State::Failed) && n < 300) {
        Settle(10); Play(true); Execute(Frame(frame++, {{2, 46}, {2, 47}}, {Effect, Effect})); ++n;
    }
    Require(g_async->StateOf((2u << 16) | 46) == asyncsh::State::Failed && g_async->StateOf((2u << 16) | 47) == asyncsh::State::Failed, "X: both jobs failed");
    n = 0;
    while (!Linked(2, 46) && !Linked(2, 47) && n < 5) { Play(true); Execute(Frame(frame++, {{2, 46}, {2, 47}}, {Effect, Effect})); ++n; }
    Require(Linked(2, 46) || Linked(2, 47), "X: the first failed program is compiled by the render thread (one slot)");
    for (int i = 0; i < 10; ++i) { Play(true); Execute(Frame(frame++, {{2, 46}, {2, 47}}, {Effect, Effect})); }
    Require(Linked(2, 46) != Linked(2, 47), "X: the second failed program waits for the next slot (it was linked a picture later before the fix)");
    n = 0;
    while (!(Linked(2, 46) && Linked(2, 47)) && n < 120) { Play(true); Execute(Frame(frame++, {{2, 46}, {2, 47}}, {Effect, Effect})); ++n; }
    Require(Linked(2, 46) && Linked(2, 47), "X: and is linked at that slot (not skipped for ever)");

    // Y. a worker that stays busy cannot hold a draw for ever
    fake::failCompile = false; fake::workerMs = 1500;
    Execute(Frame(frame++, {{2, 48}}, {Effect}, {&g_fsrc[48]}));   // (queues a 1.5 s job)
    Settle(60);
    Require(g_async->WorkerBusy(), "Y: the worker is busy");
    const int compiles0 = fake::workerCompiles.load();
    n = 0;
    while (!Linked(2, 21) && n < 60) { Play(true); Execute(Frame(frame++, {{2, 21}}, {Effect})); ++n; }
    Require(Linked(2, 21), "Y: a cached program linked although the worker was still busy (aged)");
    Require(n >= 15 && g_async->WorkerBusy() && fake::workerCompiles.load() == compiles0, "Y: it did wait for the cap first, and the worker was still in the same compile");
    Require(g_pcs.busyAged >= 1, "Y: counted");

    // Z. the player's silhouette draw (occlusion / stencil flag) is not held while the worker is busy
    Execute(Frame(frame++, {{2, 22}}, {Occ}));
    Require(Linked(2, 22), "Z: an occ-flagged draw links at once with the worker busy");
    std::printf("PASS review A worker\n");
    std::fflush(stdout);
    _exit(0);   // (the fake worker thread is detached and may still be sleeping)
}
