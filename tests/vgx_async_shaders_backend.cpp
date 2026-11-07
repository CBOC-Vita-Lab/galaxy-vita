// GXV_ASYNC_SHADERS in the back end (vgx_backend_gl.cpp GetProgram / Draw / AsyncMustBlock) on the GL stub, built as the console builds it
// (__VGX_VITA__), with a fake host: a cache probe, a "compiler" that sleeps 60 ms on another thread, a worker thread made from std::thread.
//   vgx_async_shaders_backend on    GXV_ASYNC_SHADERS unset (default 1)
//   vgx_async_shaders_backend off   GXV_ASYNC_SHADERS=0: the old behaviour, none of the host's functions is ever called
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
#include <thread>

// ---- the fake host (the functions vgx_host_galaxy_vita.cpp defines on the console)
namespace fake {
std::mutex m;
std::set<std::string> cache;
int probes = 0, compiles = 0, boosts = 0, starts = 0;
std::thread::id mainThread, compileThread;
std::set<std::string> compiled;
double compileMs = 60;
}
extern "C" {
int vgxvita_asyncShaderInCache_v1(const char* joined, int) {
    std::lock_guard<std::mutex> lk(fake::m);
    ++fake::probes;
    return fake::cache.count(joined) ? 1 : 0;
}
int vgxvita_asyncShaderCompile_v1(const char* vs, const char* fs, int, int* n) {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(fake::compileMs * 1000)));
    std::lock_guard<std::mutex> lk(fake::m);
    ++fake::compiles;
    fake::compileThread = std::this_thread::get_id();
    int made = 0;
    if (fake::cache.insert(vs).second) ++made;
    if (fake::cache.insert(fs).second) ++made;
    fake::compiled.insert(std::string(vs) + "|" + fs);
    if (n) *n = made;
    return 0;
}
int vgxvita_asyncShaderStart_v1(void (*fn)(void*), void* arg, char* where, size_t n) {
    ++fake::starts;
    std::snprintf(where, n, "on a test thread");
    std::thread(fn, arg).detach();
    return 1;
}
void vgxvita_asyncShaderBoost_v1(int) { ++fake::boosts; }
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
        RegisterShader(&g_fsrc[i]);
    }
}
static std::string JoinedVs(unsigned i) { return "#version 100\n" + g_vsrc[i].text; }
static std::string JoinedFs(unsigned i) { return "#version 100\n" + g_fsrc[i].text; }
enum DrawKind { Hud, Effect, CachedProgram, Map };
// A frame with the given draws: (vs, fs, kind). Hud: no depth test. Effect: depth test, blended. Map: opaque with a static buffer (only classified here).
static CmdBuf Frame(uint64_t frame, const std::vector<std::pair<unsigned, unsigned>>& draws, const std::vector<DrawKind>& kinds,
                    const std::vector<uint32_t>& announce = {}) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2};
    b.uni = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (uint32_t a : announce) b.cmds.push_back({CMD_PROGRAM, a});
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
static double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool Linked(unsigned vs, unsigned fs) {
    const auto it = g_programs.find((vs << 16) | fs);
    return it != g_programs.end() && it->second.ok;
}

int main(int argc, char** argv) {
    Require(argc == 2, "usage: vgx_async_shaders_backend on|off");
    const bool on = std::strcmp(argv[1], "on") == 0;
    if (on) unsetenv("GXV_ASYNC_SHADERS"); else setenv("GXV_ASYNC_SHADERS", "0", 1);
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_MAXPROGRAMS", "1000", 1);
    fake::mainThread = std::this_thread::get_id();
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    // the cache holds vertex shader 0, 1 and fragment shaders 0..3 (programs among them are "in the cache"); everything else is not
    for (unsigned i = 0; i < 2; ++i) fake::cache.insert(JoinedVs(i));
    for (unsigned i = 0; i < 4; ++i) fake::cache.insert(JoinedFs(i));
    scene = 0xA; loading = 0; quiet = 0;
    uint64_t frame = 1;

    // ---- the classes
    {
        DrawRec d;
        Require(AsyncMustBlock(d), "no depth test: HUD / 2D must not be skipped");
        d.rs.depthTest = 1;
        Require(!AsyncMustBlock(d) || d.cover, "depth-tested blended 3D may wait");
        d.rs.blend = 1;
        Require(!AsyncMustBlock(d), "an effect may wait");
        d.rs.blend = 0;
        d.vbuf = 5;
        d.vtxCount = 40;
        Require(AsyncMustBlock(d), "opaque cached geometry (the map) must not be skipped");
        d.vbuf = 0;
        Require(!AsyncMustBlock(d), "opaque CPU-transformed geometry may wait");
        d.cover = 1;
        Require(AsyncMustBlock(d), "a draw that covers the viewport must not be skipped");
    }

    // ---- frame 1: an effect with a missing program, a HUD draw with a missing program, an effect whose program is cached, an announcement
    const std::vector<std::pair<unsigned, unsigned>> draws = {{2, 10}, {2, 11}, {1, 2}, {2, 12}};
    const std::vector<DrawKind> kinds = {Hud, Effect, Effect, Effect};
    const double t0 = Ms();
    Execute(Frame(frame++, draws, kinds, {(2u << 16) | 13u})); // (announcement of a missing program: never compiled on the render thread)
    const double firstMs = Ms() - t0;
    if (on) {
        Require(Linked(2, 10), "the HUD draw's program was linked at once (the old blocking path)");
        Require(Linked(1, 2), "the program in the cache was linked at once");
        Require(!Linked(2, 11) && !Linked(2, 12) && !Linked(2, 13), "the effects' programs that are not in the cache were skipped, not compiled");
        Require(g_async != nullptr && fake::starts == 1, "one worker");
        Require(firstMs < fake::compileMs, "the frame did not wait for a compile");
        const asyncsh::Stats s = g_async->Totals();
        Require(s.skippedDraws == 2, "two draws skipped (the announcement is not a draw)");
        Require(s.queued == 3, "three pairs queued: two draws' and the announcement's");
        // frames go on while the worker compiles: none of them waits, the effects stay skipped
        double worst = 0;
        unsigned frames = 0;
        while (!(Linked(2, 11) && Linked(2, 12) && Linked(2, 13)) && Ms() - t0 < 4000.0) {
            const double f0 = Ms();
            Execute(Frame(frame++, draws, kinds, {(2u << 16) | 13u}));
            worst = std::max(worst, Ms() - f0);
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        Require(Linked(2, 11) && Linked(2, 12), "once the worker is done the draws' programs link (a read of the cache, here the stub)");
        Require(Linked(2, 13), "the announcement's program links the same way");
        Require(frames >= 5, "several frames were drawn while the worker compiled");
        Require(worst < fake::compileMs / 2, "no frame waited for the worker (the slowest took a fraction of one compile)");
        Require(fake::compileThread != fake::mainThread, "the compiles ran on the worker's thread");
        Require(fake::compiles == 3, "three pairs compiled once each");
        const asyncsh::Stats t = g_async->Totals();
        Require(t.compiledPairs == 3 && t.failed == 0, "the engine counted them");
        Require(t.skippedDraws > 2, "draws were skipped for as long as it took");
        // a draw that must not be skipped, whose pair a worker job has queued: the job leaves the queue, the draw links at once
        fake::compileMs = 200;
        Execute(Frame(frame++, {{3, 20}}, {Effect}, {}));
        Require(!Linked(3, 20), "an effect with a new program is skipped");
        Execute(Frame(frame++, {{3, 20}}, {Hud}, {}));
        Require(Linked(3, 20), "the same program wanted by a HUD draw links at once, behind the worker's job");
        // the line for the log
        lines.clear();
        AsyncSummary();
        bool found = false;
        for (const auto& l : lines) if (l.find("async-shaders-5s: compiled") != std::string::npos && l.find("draws skipped") != std::string::npos) found = true;
        Require(found, "the 5 s line is written");
        for (const auto& l : lines) std::printf("%s\n", l.c_str());
        std::printf("PASS async shaders (on): %u frames while compiling, slowest %.1f ms (a compile is %.0f ms), first frame %.1f ms\n", frames, worst, 60.0, firstMs);
    } else {
        Require(g_async == nullptr && fake::starts == 0 && fake::probes == 0 && fake::compiles == 0, "off: no worker, no probe, no compile");
        Require(Linked(2, 10) && Linked(2, 11) && Linked(2, 12) && Linked(1, 2), "off: every draw's program was linked at once, as before");
        Require(Linked(2, 13), "off: the announcement's program was linked at once");
        Execute(Frame(frame++, {{3, 20}}, {Effect}, {}));
        Require(Linked(3, 20), "off: a new effect program links in its draw's frame");
        Require(fake::probes == 0 && fake::boosts == 0, "off: the host was never asked");
        std::printf("PASS async shaders (off): identical to the blocking behaviour\n");
    }
    return 0;
}
