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
namespace fake { std::set<std::string> cache; std::mutex mu; int probes = 0; std::atomic<int> workerCompiles{0}; std::atomic<int> workerMs{60}; std::atomic<bool> workerOnRender{false}; std::thread::id renderThread; }
extern "C" {
int vgxvita_asyncShaderInCache_v1(const char* joined, int) { std::lock_guard<std::mutex> lk(fake::mu); ++fake::probes; return fake::cache.count(joined) ? 1 : 0; }
// the worker: a thread of this process that takes a while per pair and then "writes the cache files"
int vgxvita_asyncShaderCompile_v1(const char* vs, const char* fs, int, int* compiled) {
    if (std::this_thread::get_id() == fake::renderThread) fake::workerOnRender = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(fake::workerMs.load()));
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
enum DrawKind { Hud, Effect };
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
        if (kinds[i] == Effect) { d.rs.depthTest = 1; d.rs.blend = 1; d.rs.blendSrc = BF_SRC_ALPHA; d.rs.blendDst = BF_INV_SRC_ALPHA; }
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
    setenv("GXV_STAGE_PRECOMPILE", "1", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");   // (the default under the switch: on)
    setenv("GXV_MAXPROGRAMS", "1000", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    for (unsigned i = 0; i < 4; ++i) fake::cache.insert(JoinedVs(i));
    for (unsigned i = 0; i < 64; ++i) fake::cache.insert(JoinedFs(i));
    for (unsigned f : {40u, 41u, 42u, 43u, 44u, 45u}) fake::cache.erase(JoinedFs(f));
    scene = 0xA; loading = 0; quiet = 0;
    uint64_t frame = 1;
    // before play: a frame that links something makes the worker (not at the first miss in play)
    Play(false);
    Execute(Frame(frame++, {{2, 1}}, {Hud}));
    Require(g_async != nullptr, "the worker exists after the first frames outside play, without GXV_ASYNC_SHADERS");
    Play(true);

    // A. an uncached shader and the draw that needs it: the command leaves it, the draw is skipped, the worker compiles, the draw's program links from the cache
    Execute(Frame(frame++, {{2, 40}}, {Effect}, {&g_fsrc[40]}));
    Require(g_pcs.shadersDeferred == 1 && g_fsPending[40] == 1, "A: the compile was not made at the command");
    Require(!Linked(2, 40), "A: the draw waits");
    Require(g_pcs.drawMisses == 1, "A: counted as a miss in play");
    unsigned n = 0;
    while (!Linked(2, 40) && n < 200) { Settle(10); Play(true); Execute(Frame(frame++, {{2, 40}}, {Effect})); ++n; }
    Require(Linked(2, 40), "A: linked once the worker had written the cache");
    Require(fake::workerCompiles >= 1 && !fake::workerOnRender, "A: the compile ran on the worker, not on the render thread");

    // B. while the worker compiles, a cached program that may wait is not linked (the link would wait on the compiler's lock)
    fake::workerMs = 250;
    Execute(Frame(frame++, {{2, 41}}, {Effect}, {&g_fsrc[41]}));   // (queues the worker)
    Settle(40);
    const uint64_t busy0 = g_pcs.busyDeferred;
    Play(true);
    Execute(Frame(frame++, {{2, 20}}, {Effect}));
    Require(g_pcs.busyDeferred > busy0 && !Linked(2, 20), "B: a cached program waited while the worker held the compiler");
    // C. what must not wait links at once even with the worker busy
    Execute(Frame(frame++, {{2, 43}}, {Hud}));
    Require(Linked(2, 43), "C: a HUD draw links at once");
    n = 0;
    while ((!Linked(2, 20) || !Linked(2, 41)) && n < 300) { Settle(10); Play(true); Execute(Frame(frame++, {{2, 20}, {2, 41}}, {Effect, Effect})); ++n; }
    Require(Linked(2, 20) && Linked(2, 41), "B: both linked when the worker was idle");

    // D. GXV_STAGE_PRECOMPILE: queue entries with no cache go to the worker and are linked by the queue when it is done
    fake::workerMs = 30;
    g_pcs = ::vgx::pcs::Census{};
    for (unsigned f : {44u, 45u}) { VariantCmd v; v.vsId = 2; v.fsId = static_cast<uint16_t>(f); g_stageQueue.push_back(v); }
    g_stageQueued = 2;
    n = 0;
    while (!g_stageQueue.empty() && n < 300) { Settle(10); Play(true); Execute(Frame(frame++, {}, {})); ++n; }
    Require(g_stageQueue.empty() && Linked(2, 44) && Linked(2, 45), "D: both entries linked by the queue");
    Require(g_pcs.preCold == 2 && g_pcs.preAsked == 2 && g_pcs.preWorkerReady == 2 && g_pcs.preRotations >= 2 && g_pcs.preDropped == 0, "D: asked, passed over, taken when ready");
    Require(fake::cache.count(CacheKey(true, 44)) && fake::cache.count(CacheKey(true, 45)), "D: the worker wrote both");
    lines.clear();
    PcsStageEnd("StubGalaxy");
    bool found = false;
    for (const auto& l : lines) if (l.find("precompile: stage StubGalaxy:") != std::string::npos) { found = true; std::printf("%s\n", l.c_str()); }
    if (!found) for (const auto& l : lines) std::fprintf(stderr, "line: %s\n", l.c_str());
    Require(found, "D: the census line");
    std::printf("PASS play compile worker\n");
    return 0;
}
