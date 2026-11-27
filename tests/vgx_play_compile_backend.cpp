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
void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<void*>(1); }
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
namespace fake { std::set<std::string> cache; int probes = 0; }
extern "C" {
int vgxvita_asyncShaderInCache_v1(const char* joined, int) { ++fake::probes; return fake::cache.count(joined) ? 1 : 0; }
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
static void Play(bool on) { g_screenLive = on; g_stageTarget = g_stageRecKey = on ? 0xA : 0; g_menuRec = !on; } // (bootscr26: outside play the screen is closed / quiet)
static unsigned g_hookCalls = 0;
static void CountHook(uint32_t, uint32_t) { ++g_hookCalls; }

int main() {
    setenv("GXV_NO_PLAY_COMPILE_STOP", "1", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");   // (no worker in this build: the host functions are absent, so the gate's fallback is what is tested)
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_MAXPROGRAMS", "1000", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    for (unsigned i = 0; i < 4; ++i) fake::cache.insert(JoinedVs(i));
    for (unsigned i = 0; i < 64; ++i) fake::cache.insert(JoinedFs(i));
    fake::cache.erase(JoinedVs(3));
    for (unsigned f : {40u, 41u, 42u, 43u, 44u}) fake::cache.erase(JoinedFs(f));
    {   // bootscr26: the title (no stage) and the file select are not play: PlayStarted stays down for them
        uint32_t h = 2166136261u; for (const char* c = "FileSelect"; *c; ++c) h = (h ^ static_cast<unsigned char>(*c)) * 16777619u;
        loading = 0; quiet = 0;
        scene = 0; Execute(Frame(0, {}, {}));
        Require(!PlayStarted(), "PlayStarted: not for the title (no stage key)");
        scene = h; Execute(Frame(0, {}, {}));
        Require(!PlayStarted(), "PlayStarted: not for the file select");
    }
    scene = 0xA; loading = 0; quiet = 0;
    uint64_t frame = 1;
    Require(PcsOn() && AlMode() == 1 && AlExplicit() == 0, "the switch is on: the gate runs in mode 1, missOnly");

    // A. cached programs link at once, as without the switch (no slicing)
    Play(true);
    Execute(Frame(frame++, {{2, 1}}, {Hud}));
    std::vector<std::pair<unsigned, unsigned>> draws;
    std::vector<DrawKind> kinds;
    for (unsigned f = 10; f < 16; ++f) { draws.push_back({2, f}); kinds.push_back(Effect); }
    Play(true);
    Execute(Frame(frame++, draws, kinds));
    for (unsigned f = 10; f < 16; ++f) Require(Linked(2, f), "A: six cached programs link in one picture (cached links are not gated)");
    Require(g_al.c.deferredAsks == 0, "A: nothing deferred");

    // B. a CMD_SHADER in play: a text no cache has is left alone; a cached one is compiled
    Play(true);
    Execute(Frame(frame++, {}, {}, {&g_fsrc[40], &g_fsrc[50]}));
    Require(g_pcs.shadersDeferred == 1 && g_pcs.shadersCompiledInPlay == 1, "B: one deferred, one answered by a cache");
    Require(g_fsPending[40] == 1 && g_fsPending[50] == 0, "B: the uncached shader is still pending, the cached one compiled");

    // C. a draw's program with one uncached shader: the first miss compiles and links; the next miss waits for the gap
    const unsigned compiles0 = g_stats.shaderCompiles;
    Play(true);
    Execute(Frame(frame++, {{2, 40}, {2, 41}}, {Effect, Effect}));
    Require(Linked(2, 40) && !Linked(2, 41), "C: the first miss links, the second waits");
    Require(g_stats.shaderCompiles - compiles0 == 1, "C: one compile in that picture");
    unsigned n = 0;
    while (!Linked(2, 41) && n < 60) { Play(true); Execute(Frame(frame++, {{2, 41}}, {Effect})); ++n; }
    Require(Linked(2, 41) && n >= 25 && n <= 32, "C: linked when the gap (30 pictures) is over");

    // D. a program whose vertex AND fragment shader both need the compiler: one compile now, the other at the next slot
    for (int i = 0; i < 31; ++i) { Play(true); Execute(Frame(frame++, {{2, 1}}, {Hud})); } // (the gap of C's last miss is over)
    Play(true);
    const unsigned c1 = g_stats.shaderCompiles;
    Execute(Frame(frame++, {{3, 42}}, {Effect}));
    Require(!Linked(3, 42) && g_stats.shaderCompiles - c1 == 1, "D: the vertex shader alone was compiled, the program waits");
    Require(g_vsPending[3] == 0 && g_fsPending[42] == 1 && g_pcs.firstOnly == 1, "D: vs done, fs left, counted");
    n = 0;
    while (!Linked(3, 42) && n < 60) { Play(true); const unsigned c = g_stats.shaderCompiles; Execute(Frame(frame++, {{3, 42}}, {Effect})); Require(g_stats.shaderCompiles - c <= 1, "D: never more than one compile a picture"); ++n; }
    Require(Linked(3, 42) && n >= 25, "D: linked at the next slot");

    // E. what must not wait links at once even when uncached (counted)
    Play(true);
    Execute(Frame(frame++, {{2, 43}}, {Hud}));
    Require(Linked(2, 43), "E: a HUD draw's uncached program links at once");

    // F. the progress picture is never shown in play, and is shown outside it
    g_showHook = &CountHook;
    g_dispW = 32; g_dispH = 32;
    g_hostDrawsProgress = true;
    g_sw.total = 100; g_sw.done = 10;
    Require(PlayStarted() && PlayStage() == 0xA, "F: bootscr26: PlayStarted was latched by the first live frame of the real stage 0xA (not the title, not the file select)");
    Play(true);
    ShowProgress();
    Require(g_hookCalls == 0 && g_pcs.progressSuppressed == 1, "F: suppressed in play");
    Play(false);
    ShowProgress();
    Require(g_hookCalls == 1, "F: outside play (a closed or quiet screen: a stage load, a menu) the hook is called as before");
    // bootscr26: the hard rule needs no switch: with the switchable rule not applying (AlInPlay false because the queue's target is another stage) the live picture is still protected
    Play(true);
    g_stageTarget = 0xB; // (a stage load in flight with the old screen still live: AlInPlay is false)
    Require(!AlInPlay() && g_screenLive, "F: the stand-in for a zone change with a live screen");
    ShowProgress();
    Require(g_hookCalls == 1 && g_pcs.progressSuppressed == 2, "F: bootscr26: a live picture after play started is never covered, with or without noprog26's rule");
    g_stageTarget = 0xA;
    Play(true);

    // G. outside play a CMD_SHADER compiles as before
    Play(false);
    Execute(Frame(frame++, {}, {}, {&g_fsrc[44]}));
    Require(g_fsPending[44] == 0, "G: outside play the uncached text is compiled at its command");

    // H. the census line
    lines.clear();
    PcsStageEnd("StubGalaxy");
    bool found = false;
    for (const auto& l : lines) if (l.find("noprog: stage StubGalaxy:") != std::string::npos && l.find("shaders left to the worker/gate 1") != std::string::npos) { found = true; std::printf("%s\n", l.c_str()); }
    Require(found, "H: the stage line is written");
    Require(!g_pcs.Any(), "H: and the census starts again");
    std::printf("PASS play compile stop backend\n");
    return 0;
}

// (tests42: vgx_draw.cpp, not linked here, owns these; the back end calls them)
namespace vgx { int TailFoldMode() { return 0; } int HrwMode() { return 0; } }
