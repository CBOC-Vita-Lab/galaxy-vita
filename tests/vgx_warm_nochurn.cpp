// GXV_WARM_NOCHURN (platform/vgx/src/vgx_backend_gl.cpp WarmNoChurn): the generic deferred warm-up stops at its reserve of vitaGL's
// program slots instead of having the capacity service delete old programs, 4 a quiet frame, to link more of a list that never fits.
// Host build against the GL stub, as the console builds it (__VGX_VITA__: vitaGL's finite program table, simulated here). The switch
// is read once, so each setting is its own run: `vgx_warm_nochurn churn` (unset: the deletes happen) and `vgx_warm_nochurn nochurn`.
//   tools/test_vgx_warm_nochurn.sh
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

static ShaderSource g_vsrc[4], g_fsrc[64];
static void RegisterAll() {
    for (uint16_t i = 0; i < 4; ++i) {
        g_vsrc[i].id = i; g_vsrc[i].keyHash = 0x1000 + i;
        g_vsrc[i].text = "attribute vec4 a_pos; uniform mat4 u_proj; void main(){gl_Position=u_proj*a_pos;}";
        RegisterShader(&g_vsrc[i]);
    }
    for (uint16_t i = 0; i < 64; ++i) {
        g_fsrc[i].id = i; g_fsrc[i].fragment = true; g_fsrc[i].keyHash = 0x2000 + i;
        g_fsrc[i].text = "precision mediump float; void main(){gl_FragColor=vec4(1.0);}";
        RegisterShader(&g_fsrc[i]);
    }
}
static CmdBuf Frame(uint64_t frame, unsigned firstFs, unsigned count) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2};
    b.uni = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (unsigned i = 0; i < count; ++i) {
        DrawRec d;
        d.vsId = 1;
        d.fsId = static_cast<uint16_t>(firstFs + i);
        d.idxCount = d.vtxCount = 3;
        d.ublock[UB_PROJ] = 0;
        d.layoutMask = 0;
        b.draws.push_back(d);
        b.cmds.push_back({CMD_DRAW, i});
    }
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
static CmdBuf WarmList(uint64_t frame, unsigned count) {
    CmdBuf b = Frame(frame, 0, 0);
    for (unsigned i = 0; i < count; ++i) b.warm.push_back({CMD_PROGRAM, (static_cast<uint32_t>(i / 64) << 16) | (i % 64)});
    return b;
}

int main(int argc, char** argv) {
    Require(argc == 2, "usage: vgx_warm_nochurn churn|nochurn");
    const bool nochurn = std::strcmp(argv[1], "nochurn") == 0;
    if (nochurn) setenv("GXV_WARM_NOCHURN", "1", 1); else unsetenv("GXV_WARM_NOCHURN");
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_WARM_RESERVE", "8", 1);
    setenv("GXV_MAXPROGRAMS", "1000", 1);  // (a map limit above the table: only the warm-up's capacity service deletes)
    setenv("GXV_WARM_BACKOFF", "0", 1);
    tableSize = 64;
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    SetWarmBudget(1000.0);
    uint64_t frame = 1;
    scene = 0xA; loading = 0; quiet = 1;
    // A generic warm-up list of 200 programs (4 vertex x 50 fragment shaders), far more than the 64 - 8 slots it may use.
    Execute(WarmList(frame++, 200));
    const unsigned linkedFirst = static_cast<unsigned>(g_programs.size());
    Require(linkedFirst == 64 - 8, "the warm-up links up to its reserve");
    // 800 quiet frames later (the programs are 600 frames old: victims of the capacity service).
    unsigned deletes = 0;
    for (int i = 0; i < 800; ++i) {
        Execute(Frame(frame++, 0, 0));
        deletes += g_stQuiet.deletes;
        g_stQuiet = StutterCosts{};
    }
    std::printf("%s: %u programs linked at first, %u deletes in 800 quiet frames, %u linked now, warm-up units left %u\n", argv[1], linkedFirst,
                deletes, static_cast<unsigned>(g_programs.size()), WarmPending());
    if (nochurn) {
        Require(deletes == 0, "no program deleted for the generic warm-up");
        Require(vglGetFreeProgramSlots() == 8, "the reserve stays free");
    } else {
        Require(deletes > 0, "without the switch the capacity service churns (the behaviour the switch removes)");
    }
    // A draw still gets its program at once (the reserve), and with the table full a draw still reclaims (unchanged).
    quiet = 0;
    Execute(Frame(frame++, 0, 4));
    for (unsigned i = 0; i < 4; ++i) Require(g_programs.count((1u << 16) | i) && g_programs.at((1u << 16) | i).ok, "a draw's program is linked");
    std::printf("PASS warm-up %s\n", argv[1]);
    return 0;
}
