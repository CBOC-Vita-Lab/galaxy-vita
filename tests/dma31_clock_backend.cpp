// dma31 (17 Nov 2026): how many clock reads the back end's render thread makes per picture, on the GL stub, built as the console
// builds it (__VGX_VITA__, PORT_RUNTIME_DIAGNOSTICS=0). Every clock source the console's back end reaches is a counting stub here:
//   sceKernelGetProcessTimeWide (the console's NowMs / beprof::Now / the wait wrappers), std::chrono::steady_clock::now (the same
//   console clock through newlib's clock_gettime), port_host_time_ns (os_alarm.cpp), port_thread_cpu_us (sceKernelGetThreadInfo on the
//   console: a heavier call, counted apart).
// The picture: N draws over P programs, one EFB copy into a texture, the display copy and the present: the shape of an ordinary
// gameplay picture without its numbers. Prints one "clock-census-host:" line per run; tools/test_dma31.sh runs it with the console's
// defaults (entry.cpp's setenv(..., 0) lines) and checks the count against the expected budget.
//   dma31_clock_backend <pictures> <draws> <programs>
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <map>

namespace dma31 {
uint64_t g_process = 0, g_steady = 0, g_hostNs = 0, g_threadCpu = 0;
uint64_t g_fake = 1000000; // microseconds; every read moves it 3 us
std::vector<std::string> g_lines;
bool g_sites = false; // (DMA31_SITES=1: every read's caller is kept, printed as addresses for addr2line)
std::map<void*, uint64_t> g_site;
inline void Site(void* at) { if (g_sites) ++g_site[at]; }
}
extern "C" {
uint32_t port_scene_key(void) { return 0xA; }
uint32_t port_scene_loading_key(void) { return 0; }
int port_scene_loading_active(void) { return 0; }
int port_screen_quiet(void) { return 0; }
void port_log(const char*, ...) {}
void port_log_summary(const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    dma31::g_lines.emplace_back(text);
}
int64_t port_host_time_ns(void) { dma31::Site(__builtin_return_address(0)); ++dma31::g_hostNs; dma31::g_fake += 3; return static_cast<int64_t>(dma31::g_fake) * 1000; }
int64_t port_thread_cpu_us(void) { dma31::Site(__builtin_return_address(0)); ++dma31::g_threadCpu; dma31::g_fake += 3; return static_cast<int64_t>(dma31::g_fake / 2); }
}
std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
    dma31::Site(__builtin_return_address(0));
    ++dma31::g_steady;
    dma31::g_fake += 3;
    return time_point(std::chrono::duration_cast<duration>(std::chrono::microseconds(dma31::g_fake)));
}
namespace vgx { bool LogEnabled() { return false; } }
#include <psp2/kernel/threadmgr.h>
extern "C" {
unsigned vgx_stub_name_ = 0;
unsigned glCreateShader(unsigned) { return ++vgx_stub_name_; }
unsigned vglGetFreeProgramSlots(void) { return 512; }
unsigned glCreateProgram(void) { return ++vgx_stub_name_; }
int glGetUniformLocation(unsigned, const char*) { return 1; }
unsigned glCheckFramebufferStatus(unsigned) { return 0x8CD5; }
unsigned glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned name) {
    return reinterpret_cast<const unsigned char*>(name == 0x1F03 ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : "stub");
}
size_t vglMemFree(int) { return 64u << 20; }
size_t vglMemTotal(int) { return 128u << 20; }
void* (vglGetTexDataPointer)(GLenum) { return reinterpret_cast<void*>(1); }  // (emufix31: the stub declares it as vitaGL does, void* (d73df8c4))
void sceGxmFinish(SceGxmContext*) {}
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) { return 0; }
unsigned char vgxvita_maskGuardOn = 0, vgxvita_maskGuardDue = 0;
void vgxvita_maskGuard(void) {}
std::uint64_t sceKernelGetProcessTimeWide() { dma31::Site(__builtin_return_address(0)); ++dma31::g_process; dma31::g_fake += 3; return dma31::g_fake; }
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { std::abort(); }
int sceKernelSignalSema(SceUID, int) { std::abort(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(SceUID, SceSize, void*) { std::abort(); }
int sceKernelDelayThread(SceUInt) { std::abort(); }
}
namespace vgx::host { bool CreateContext() { return true; } }
namespace vgx {
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t out[7]) { for (int i = 0; i < 7; ++i) out[i] = 0; }
void VtxPackDrawCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
bool PrepassAny() { return false; }
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
// (finish44, as triage43 did for keepdepth31 / halo31: the back end asks vgx_draw.cpp for GXV_HRW and GXV_TAILFOLD; this harness does not link it. Both off.)
int HrwMode() { return 0; }
int TailFoldMode() { return 0; }
}
extern "C" int vgxvita_asyncShaderInCache_v1(const char*, int) { return 1; }

using namespace vgx;
using namespace vgx::backend;

static ShaderSource g_vsrc[8], g_fsrc[64];
static CmdBuf Picture(uint64_t frame, unsigned draws, unsigned programs) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2};
    // two projection blocks and a model-view block per draw that moves: the uniform arena of a picture
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 16; ++i) b.uni.push_back(i % 5 == 0 ? 1.0f + 0.25f * static_cast<float>(k) : 0.0f);
    for (unsigned i = 0; i < draws; ++i) {
        DrawRec d;
        d.vsId = static_cast<uint16_t>(i % 4);
        d.fsId = static_cast<uint16_t>(i % (programs ? programs : 1));
        d.idxCount = d.vtxCount = 3;
        d.ublock[UB_PROJ] = (i % 7 == 0) ? 16u : 0u;
        d.layoutMask = 0;
        d.rs.depthTest = 1;
        b.draws.push_back(d);
        b.cmds.push_back({CMD_DRAW, static_cast<uint32_t>(i)});
        if (i == draws / 2) { // one EFB copy into a texture in the middle of the picture (a shadow or bloom source)
            CopyTexCmd c;
            c.texId = 77;
            c.src = EfbRect{0, 0, 32, 32};
            c.dstW = 32;
            c.dstH = 32;
            for (int r = 0; r < 4; ++r) for (int q = 0; q < 4; ++q) c.mat[r][q] = r == q ? 1.0f : 0.0f;
            b.copyTex.push_back(c);
            b.cmds.push_back({CMD_COPY_TEX, 0});
        }
    }
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}

int main(int argc, char** argv) {
    const unsigned pictures = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 300;
    const unsigned draws = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 250;
    const unsigned programs = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 24;
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    if (!Init(32, 32)) { std::fprintf(stderr, "FAIL: Init\n"); return 1; }
    for (uint16_t i = 0; i < 8; ++i) {
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
    g_screenLive = true;
    // warm-up pictures: programs link, pools fill (not counted)
    uint64_t frame = 1;
    for (int i = 0; i < 20; ++i) Execute(Picture(frame++, draws, programs));
    const uint64_t p0 = dma31::g_process, s0 = dma31::g_steady, h0 = dma31::g_hostNs, t0 = dma31::g_threadCpu;
    dma31::g_sites = std::getenv("DMA31_SITES") != nullptr;
    uint64_t drawn = 0, scenes = 0;
    for (unsigned i = 0; i < pictures; ++i) {
        Execute(Picture(frame++, draws, programs));
        drawn += g_lastDraws;
        scenes += g_lastScenes;
    }
    const double n = static_cast<double>(pictures);
    const double reads = static_cast<double>((dma31::g_process - p0) + (dma31::g_steady - s0) + (dma31::g_hostNs - h0));
    std::printf("clock-census-host: %u pictures of %u draws over %u fragment programs: render-thread clock reads a picture %.2f "
                "(process time %.2f, steady clock %.2f, port_host_time_ns %.2f) + thread CPU-time reads %.2f; "
                "draws executed a picture %.1f, scenes %.1f\n",
                pictures, draws, programs, reads / n, (dma31::g_process - p0) / n, (dma31::g_steady - s0) / n, (dma31::g_hostNs - h0) / n,
                (dma31::g_threadCpu - t0) / n, static_cast<double>(drawn) / n, static_cast<double>(scenes) / n);
    for (const auto& kv : dma31::g_site) std::printf("site %p %.2f\n", kv.first, static_cast<double>(kv.second) / n);
    return 0;
}
