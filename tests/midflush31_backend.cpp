// midflush31 (18 Nov 2026): GXV_MIDFLUSH's modes in the real back end on the GL stub, built as the console builds it (__VGX_VITA__; tools/test_midflush31_backend.sh,
// the harness of tests/keepdepth31_backend.cpp). One picture shaped like the Observatory's heavy EFB scene (240 draws, programs changing in runs of 1-9, uniforms
// changing, a few big draws), executed three times (the programs link) and then twice measured. Every GL call that carries state is hashed in order, the uniforms
// as the value each (program, location) holds when a draw is made, the program as the one bound at the draw: the DIGEST must be the same with the switch off and
// in every mode (a flush may only add glUseProgram(0) and the program given again). The sceGxm flush is the stub's: counted, and its hook sees which program was bound.
//   midflush31_backend <GXV_MIDFLUSH value> [fail]
#include <cstdint>
#include <cstring>
#include <map>
#include <type_traits>
#include <GLES2/gl2.h>
namespace mft {
uint64_t g_state = 1469598103934665603ull;      // every state call, in order (not the program or the uniforms)
uint64_t g_digest = 1469598103934665603ull;     // per draw: the call hash so far, the program, that program's uniforms, the draw's own arguments
unsigned g_draws = 0, g_curProg = 0, g_prevProg = ~0u, g_afterFlushDraws = 0, g_sameAfterFlush = 0, g_diffAfterFlush = 0, g_uniCalls = 0, g_useCalls = 0;
bool g_justFlushed = false;
std::map<std::pair<unsigned, int>, uint64_t> g_uni;   // (program, location) -> hash of what it holds
inline void Mix(uint64_t& h, uint64_t v) { h = (h ^ v) * 1099511628211ull; }
template <class T> inline uint64_t Bits(T v) {
    if constexpr (std::is_pointer<T>::value) return reinterpret_cast<uintptr_t>(v);
    else if constexpr (std::is_floating_point<T>::value) { double d = v; uint64_t u; std::memcpy(&u, &d, 8); return u; }
    else return static_cast<uint64_t>(v);
}
template <class... A> inline void Rec(unsigned id, A... a) { Mix(g_state, id); (Mix(g_state, Bits(a)), ...); }
inline void Uni(int loc, const float* v, unsigned n) {
    uint64_t h = 7;
    for (unsigned i = 0; i < n; ++i) { uint32_t u; std::memcpy(&u, v + i, 4); Mix(h, u); }
    g_uni[{g_curProg, loc}] = h;
    ++g_uniCalls;
}
inline void Draw(unsigned mode, int count, uintptr_t idx) {
    Mix(g_digest, g_state); Mix(g_digest, g_curProg ? 1 : 0); Mix(g_digest, mode); Mix(g_digest, count); Mix(g_digest, idx);
    for (auto& e : g_uni) if (e.first.first == g_curProg) { Mix(g_digest, e.first.second); Mix(g_digest, e.second); }
    ++g_draws;
    if (g_justFlushed) { g_justFlushed = false; ++g_afterFlushDraws; if (g_curProg == g_prevProg) ++g_sameAfterFlush; else ++g_diffAfterFlush; }
}
}
#define MFT_R(name, id, ...) (mft::Rec(id, __VA_ARGS__), name(__VA_ARGS__))
#undef glUseProgram
#undef glUniform4fv
#undef glUniformMatrix4fv
#undef glDrawElements
#undef glEnable
#undef glDisable
#undef glBlendFunc
#undef glDepthFunc
#undef glDepthMask
#undef glColorMask
#undef glViewport
#undef glScissor
#undef glBindTexture
#undef glCullFace
#undef glBindBuffer
#undef glActiveTexture
#define glUseProgram(p) (++mft::g_useCalls, mft::g_curProg = (p), glUseProgram(p))
#define glUniform4fv(loc, n, v) (mft::Uni(loc, v, 4 * (n)), glUniform4fv(loc, n, v))
#define glUniformMatrix4fv(loc, n, t, v) (mft::Uni(loc, v, 16 * (n)), glUniformMatrix4fv(loc, n, t, v))
#define glDrawElements(mode, count, type, idx) (mft::Draw(mode, count, reinterpret_cast<uintptr_t>(idx)), glDrawElements(mode, count, type, idx))
#define glEnable(a) MFT_R(glEnable, 1, a)
#define glDisable(a) MFT_R(glDisable, 2, a)
#define glBlendFunc(a, b) MFT_R(glBlendFunc, 3, a, b)
#define glDepthFunc(a) MFT_R(glDepthFunc, 4, a)
#define glDepthMask(a) MFT_R(glDepthMask, 5, a)
#define glColorMask(a, b, c, d) MFT_R(glColorMask, 6, a, b, c, d)
#define glViewport(a, b, c, d) MFT_R(glViewport, 7, a, b, c, d)
#define glScissor(a, b, c, d) MFT_R(glScissor, 8, a, b, c, d)
#define glBindTexture(a, b) MFT_R(glBindTexture, 9, a, b)
#define glCullFace(a) MFT_R(glCullFace, 10, a)
#define glBindBuffer(a, b) MFT_R(glBindBuffer, 11, a, b)
#define glActiveTexture(a) MFT_R(glActiveTexture, 12, a)

#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <string>

namespace mft2 {
std::vector<std::string> g_lines;
}
extern "C" {
uint32_t port_scene_key(void) { return 0xA; }
uint32_t port_scene_loading_key(void) { return 0; }
int port_scene_loading_active(void) { return 0; }
int port_screen_quiet(void) { return 0; }
void port_log(const char*, ...) {}
void port_log_summary(const char* fmt, ...) {
    char text[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    mft2::g_lines.emplace_back(text);
}
int64_t port_host_time_ns(void) { static int64_t t = 0; return t += 1000; }
int64_t port_thread_cpu_us(void) { static int64_t t = 0; return t += 1; }
SceGxmContext* gxm_context = reinterpret_cast<SceGxmContext*>(0x1000);
const SceGxmFragmentProgram* gxm_sc_fprog = nullptr;
const SceGxmVertexProgram* gxm_sc_vprog = nullptr;
uint32_t gxm_sc_ftex_valid = 0;
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
void* (vglGetTexDataPointer)(GLenum) { return reinterpret_cast<void*>(1); }
void sceGxmFinish(SceGxmContext*) {}
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) { return 0; }
unsigned char vgxvita_maskGuardOn = 0, vgxvita_maskGuardDue = 0;
void vgxvita_maskGuard(void) {}
std::uint64_t sceKernelGetProcessTimeWide() { static std::uint64_t t = 0; return t += 3; }
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

static ShaderSource g_vsrc[4], g_fsrc[10];
static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
static uint32_t g_rng = 12345;
static uint32_t Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

// one EFB scene of 240 draws: the programs change in runs of 1-9 draws, every draw has its own uniform block (16 floats), a few are big
static CmdBuf Picture(uint64_t frame, unsigned nDraws) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}, {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2, 3, 4, 5};
    for (unsigned i = 0; i < nDraws * 16; ++i) b.uni.push_back(static_cast<float>(i % 97) * 0.25f + (i / 16));
    g_rng = 777;
    unsigned run = 0, fs = 0, vs = 0;
    for (unsigned i = 0; i < nDraws; ++i) {
        if (run == 0) { run = 1 + Rnd() % 9; fs = Rnd() % 10; vs = Rnd() % 4; }
        --run;
        DrawRec d;
        d.vsId = static_cast<uint16_t>(vs);
        d.fsId = static_cast<uint16_t>(fs);
        d.idxCount = d.vtxCount = (i % 37 == 5) ? 900 : 3;
        d.ublock[UB_PROJ] = i * 16;
        d.layoutMask = 0;
        RenderState& rs = d.rs;
        rs.scX = 0; rs.scY = 0; rs.scW = 4096; rs.scH = 4096;
        rs.depthTest = 1; rs.depthWrite = (i % 3) != 0; rs.depthFunc = CMP_LEQUAL;
        if (i % 5 == 0) { rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; }
        d.exX = 8; d.exY = 8; d.exW = 16; d.exH = 16;
        b.cmds.push_back({CMD_DRAW, static_cast<uint32_t>(b.draws.size())});
        b.draws.push_back(d);
    }
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
static void Hook() { mft::g_justFlushed = true; mft::g_prevProg = mft::g_curProg; }

int main(int argc, char** argv) {
    const std::string value = argc > 1 ? argv[1] : "0";
    const bool fail = argc > 2 && std::string(argv[2]) == "fail";
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_MIDFLUSH", value.c_str(), 1);
    setenv("GXV_MIDFLUSH_VERIFY", "1", 1);
    setenv("GXV_RING_CENSUS", "1", 1);
    setenv("GXV_MIDFLUSH_CENSUS_MS", getenv("MFT_CENSUS_MS") ? getenv("MFT_CENSUS_MS") : "3600000", 1);  // (MFT_CENSUS_MS=0: a census line every picture, which also clears the window's counts)
    if (!Init(32, 32)) { std::fprintf(stderr, "FAIL: Init\n"); return 1; }
    for (uint16_t i = 0; i < 4; ++i) {
        g_vsrc[i].id = i; g_vsrc[i].keyHash = 0x1000 + i;
        g_vsrc[i].text = "attribute vec4 a_pos; uniform mat4 u_proj; void main(){gl_Position=u_proj*a_pos;} // vs " + std::to_string(i);
        RegisterShader(&g_vsrc[i]);
    }
    for (uint16_t i = 0; i < 10; ++i) {
        g_fsrc[i].id = i; g_fsrc[i].fragment = true; g_fsrc[i].keyHash = 0x2000 + i;
        g_fsrc[i].text = "precision mediump float; void main(){gl_FragColor=vec4(1.0);} // fs " + std::to_string(i);
        g_fsrc[i].nTev = 28;
        RegisterShader(&g_fsrc[i]);
    }
    g_screenLive = true;
    g_stubMidHook = Hook;
    uint64_t frame = 1;
    for (int i = 0; i < 3; ++i) Execute(Picture(frame++, 240));  // (the programs link; mode 3 learns the scene's size)
    g_stubMidCalls = 0;
    if (fail) g_stubMidRc = -1;
    mft::g_digest = 1469598103934665603ull; mft::g_draws = 0; mft::g_afterFlushDraws = mft::g_sameAfterFlush = mft::g_diffAfterFlush = 0; mft::g_justFlushed = false;
    mft::g_state = 1469598103934665603ull;
    g_midStats = MidStats{};
    const unsigned flushesBefore = g_midFlushes;
    Execute(Picture(frame++, 240));
    const uint32_t hashA = g_ring.lastHash;
    Execute(Picture(frame++, 240));
    const uint32_t hashB = g_ring.lastHash;
    const unsigned flushes = g_midFlushes - flushesBefore;
    std::printf("RESULT value=%s digest=%016llx draws=%u flushes=%u stubcalls=%d boundary=%u forced=%u fixed=%u failed=%u reissued=%u after_same=%u after_diff=%u "
                "streamhash=%08x/%08x on=%u uniform_calls=%u\n", value.c_str(), static_cast<unsigned long long>(mft::g_digest), mft::g_draws, flushes, g_stubMidCalls,
                g_midStats.atBoundary, g_midStats.forced, g_midStats.fixed, g_midStats.failed, g_midStats.reissued, mft::g_sameAfterFlush, mft::g_diffAfterFlush, hashA, hashB,
                g_midFlushEvery ? 1u : 0u, mft::g_uniCalls);
    unsigned mid = 0, ring = 0;
    for (const std::string& l : mft2::g_lines) { mid += l.rfind("midflush:", 0) == 0; ring += l.rfind("ring-5s:", 0) == 0; }
    std::printf("CENSUS midflush_lines=%u ring_lines=%u\n", mid, ring);
    for (const std::string& l : mft2::g_lines) if (l.rfind("midflush:", 0) == 0 || l.rfind("ring-5s:", 0) == 0) std::printf("LINE %s\n", l.c_str());
    return g_fail ? 1 : 0;
}
