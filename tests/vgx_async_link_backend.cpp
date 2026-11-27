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
static CmdBuf Frame(uint64_t frame, const std::vector<std::pair<unsigned, unsigned>>& draws, const std::vector<DrawKind>& kinds) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2};
    b.uni = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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

int main(int argc, char** argv) {
    Require(argc == 2, "usage: vgx_async_link_backend 0|1|2");
    const int mode = std::atoi(argv[1]);
    setenv("GXV_ASYNC_LINK", argv[1], 1);
    setenv("GXV_NO_PLAY_COMPILE_STOP", "0", 1); // (noprog26: this test is of the gate alone)
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_MAXPROGRAMS", "1000", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    for (unsigned i = 0; i < 4; ++i) fake::cache.insert(JoinedVs(i));      // everything is in the shader cache, except what a test removes
    for (unsigned i = 0; i < 64; ++i) fake::cache.insert(JoinedFs(i));
    scene = 0xA; loading = 0; quiet = 0;
    uint64_t frame = 1;
    Play(true);
    // a program of vertex shader 2 is linked (by a HUD draw: it must not wait, so it is a synchronous link) and will stand in for the others
    Execute(Frame(frame++, {{2, 1}}, {Hud}));
    Require(Linked(2, 1), "a 2D draw's program links at once in every mode");
    Play(true);
    // six effects with new programs of the same vertex shader, in every picture: one new link a picture (modes 1, 2), all at once (mode 0)
    std::vector<std::pair<unsigned, unsigned>> draws;
    std::vector<DrawKind> kinds;
    for (unsigned f = 10; f < 16; ++f) { draws.push_back({2, f}); kinds.push_back(Effect); }
    unsigned pictures = 0;
    auto linkedCount = [&] { unsigned n = 0; for (unsigned f = 10; f < 16; ++f) n += Linked(2, f) ? 1 : 0; return n; };
    unsigned prev = 0;
    while (linkedCount() < 6 && pictures < 20) {
        Play(true);
        Execute(Frame(frame++, draws, kinds));
        ++pictures;
        const unsigned now = linkedCount();
        if (mode == 0) Require(now == 6, "mode 0: all six link in the first picture");
        else Require(now <= prev + 1, "one new link a picture");
        prev = now;
    }
    Require(linkedCount() == 6, "all six linked in the end");
    Require(mode == 0 ? pictures == 1 : pictures == 6, "pictures to link them all");
    Play(true);
    Execute(Frame(frame++, {{2, 1}}, {Hud})); // (closes the previous picture's run)
    if (mode == 0) {
        Require(g_al.c.linksInPlay == 0 && g_al.c.deferredAsks == 0 && fake::probes == 0, "mode 0: nothing counted, the cache never probed");
    } else {
        Require(g_al.c.slicedLinks == 6, "six sliced links");
        Require(g_al.c.sync[::vgx::alink::kSyncMustFlat] == 1 && g_al.c.linksInPlay == 7, "the HUD draw's link was synchronous and counted with its reason");
        Require(g_al.c.deferredKeys == 5, "five programs waited");
        const uint64_t drawsTurnedAway = 5 + 4 + 3 + 2 + 1;
        // (tests31: hitch33 made GXV_MISS_STANDIN default 1, so mode 1 stands the draws in by (2,1) like mode 2; GXV_MISS_STANDIN=0 is nohitch26's "skipped": both are checked,
        //  tools/test_async_link.sh runs mode 1 with and without it)
        const bool missStandInOff = std::getenv("GXV_MISS_STANDIN") && std::atoi(std::getenv("GXV_MISS_STANDIN")) == 0;
        if (mode == 1 && missStandInOff) Require(g_al.c.drawsSkipped == drawsTurnedAway && g_al.c.drawsStoodIn == 0, "mode 1, GXV_MISS_STANDIN=0: 15 draws skipped");
        else if (mode == 1) Require(g_al.c.drawsStoodIn == drawsTurnedAway && g_al.c.drawsSkipped == 0, "mode 1 (GXV_MISS_STANDIN default 1): 15 draws stood in by (2,1)");
        else Require(g_al.c.drawsStoodIn == drawsTurnedAway && g_al.c.drawsSkipped == 0, "mode 2: 15 draws stood in by (2,1)");
        Require(g_al.c.picturesAffected == 5 && g_al.c.runMax == 5, "five pictures had a draw turned away");
        // the stand-in must not leak into the variant records
        Require(!Linked(3, 10), "nothing else was linked");
    }
    // not in play (a menu is up): linked at once, not counted
    if (mode) {
        const uint64_t before = g_al.c.linksInPlay;
        Play(false);
        Execute(Frame(frame++, {{2, 20}, {2, 21}, {2, 22}}, {Effect, Effect, Effect}));
        Require(Linked(2, 20) && Linked(2, 21) && Linked(2, 22), "outside play every draw links at once");
        Require(g_al.c.linksInPlay == before, "and is not counted");
        Play(true);
    }
    // not in the cache and no worker: the first links (counted), the second waits missGap pictures
    if (mode) {
        fake::cache.erase(JoinedFs(30)); fake::cache.erase(JoinedFs(31));
        Play(true);
        Execute(Frame(frame++, {{2, 30}, {2, 31}}, {Effect, Effect}));
        Require(Linked(2, 30) && !Linked(2, 31), "a program whose shaders are not cached links, the next miss waits");
        Require(g_al.c.sync[::vgx::alink::kSyncMissNoWorker] == 1, "counted as a synchronous link with its reason");
        for (int i = 0; i < 25; ++i) { Play(true); Execute(Frame(frame++, {{2, 31}}, {Effect})); }
        Require(!Linked(2, 31), "still waiting inside the gap");
        for (int i = 0; i < 8; ++i) { Play(true); Execute(Frame(frame++, {{2, 31}}, {Effect})); }
        Require(Linked(2, 31), "linked when the gap is over");
    }
    // the census line
    lines.clear();
    AlStageEnd("StubGalaxy");
    if (mode) {
        bool found = false;
        for (const auto& l : lines) if (l.find("asynclink: stage StubGalaxy: mode") != std::string::npos && l.find("synchronous links that still happened:") != std::string::npos) { found = true; std::printf("%s\n", l.c_str()); }
        Require(found, "the stage line is written");
        Require(g_al.c.linksInPlay == 0, "and the census starts again");
    } else {
        Require(lines.empty(), "mode 0 writes no line");
    }
    std::printf("PASS async link backend (mode %d)\n", mode);
    return 0;
}

// (tests42: vgx_draw.cpp, not linked here, owns these; the back end calls them)
namespace vgx { int TailFoldMode() { return 0; } int HrwMode() { return 0; } }
