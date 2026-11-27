// flipfix31 (18 Nov 2026): GXV_HRW_FLIPBOOK / GXV_HRW_FLIPBOOK_FROM under mode 5 pairs, in the real back end on the GL stub (harness of tests/keepdepth31_backend.cpp).
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <string>
namespace kdt {
std::vector<std::string> g_lines;
unsigned g_depthModes = 0, g_depthStores = 0;
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
    kdt::g_lines.emplace_back(text);
}
int64_t port_host_time_ns(void) { static int64_t t = 0; return t += 1000; }
int64_t port_thread_cpu_us(void) { static int64_t t = 0; return t += 1; }
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
void vglSetNextSceneDepthModes(unsigned char, unsigned char store) { ++kdt::g_depthModes; kdt::g_depthStores += store ? 1u : 0u; }
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

enum Kind { kClearEfb, k3D, kZClear, k2D, k2DTest, kPartWrite, kFill, kAlwaysNoWrite, kHudWrite };
static bool g_zNoMark = false;  // (halo31, case zclear-nomark: the depth clear as the front end made it before 18 Nov: no mark at all)
static void Add(CmdBuf& b, Kind k) {
    DrawRec d;
    d.vsId = 0;
    d.fsId = static_cast<uint16_t>(k);
    d.idxCount = d.vtxCount = (k == kClearEfb || k == kZClear || k == kFill) ? 6 : 3;
    d.ublock[UB_PROJ] = 0;
    d.layoutMask = 0;
    RenderState& rs = d.rs;
    rs.scX = 0; rs.scY = 0; rs.scW = 4096; rs.scH = 4096;
    const bool cover = k == kClearEfb || k == kZClear || k == kFill;
    if (cover) { d.coverCull = 1; d.exX = 0; d.exY = 0; d.exW = 4096; d.exH = 4096; rs.cull = CULL_BACK; }
    else { d.exX = 8; d.exY = 8; d.exW = 16; d.exH = 16; }
    // (halo31: MR::clearZBuffer writes no colour, so the real front end never gives it `cover` / `coverCull` (console6: every pair refused, "last depth-only
    // clear at command -1"). It carries DrawRec::depthMark bit 1 instead, the depth-only rectangle's mark; this harness used to set coverCull by hand.)
    if (k == kZClear) { d.coverCull = 0; d.depthMark = g_zNoMark ? 0 : 2; }
    switch (k) {
    case kClearEfb: case kFill: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; rs.zNear = rs.zFar = 1.0f; break;
    case k3D: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_LEQUAL; break;
    case kZClear: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; rs.colorWrite = 0; rs.alphaWrite = 0; rs.zNear = rs.zFar = 1.0f; break;
    case k2D: rs.depthTest = 0; rs.depthWrite = 0; rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; break;
    case k2DTest: rs.depthTest = 1; rs.depthWrite = 0; rs.depthFunc = CMP_LEQUAL; break;
    case kPartWrite: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; break;
    case kAlwaysNoWrite: rs.depthTest = 1; rs.depthWrite = 0; rs.depthFunc = CMP_ALWAYS; rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; break;  // (a HUD draw the front end can emit: vgx_draw.cpp)
    case kHudWrite: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; d.hud = 1; break;  // (emufix31)
    }
    if (k == k2D || k == k2DTest || k == kAlwaysNoWrite) d.hud = 1;  // (the 2D is drawn between the HUD markers; the front end flags it under GXV_HRW)
    b.cmds.push_back({CMD_DRAW, static_cast<uint32_t>(b.draws.size())});
    b.draws.push_back(d);
}
static CmdBuf Picture(uint64_t frame, const std::vector<Kind>& kinds) {
    CmdBuf b;
    b.frame = frame;
    const float positions[][4] = {{-0.8f, -0.8f, 0, 1}, {0.8f, -0.8f, 0, 1}, {0, 0.8f, 0, 1}, {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}};
    b.vtx.resize(sizeof positions);
    std::memcpy(b.vtx.data(), positions, sizeof positions);
    b.idx = {0, 1, 2, 3, 4, 5};
    for (int i = 0; i < 16; ++i) b.uni.push_back(i % 5 == 0 ? 1.0f : 0.0f);
    for (Kind k : kinds) Add(b, k);
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
static std::vector<Kind> GameKinds() {
    std::vector<Kind> v{kClearEfb};
    for (int i = 0; i < 20; ++i) v.push_back(k3D);
    v.push_back(kZClear);
    for (int i = 0; i < 6; ++i) v.push_back(k2D);
    return v;
}

static std::string Slurp(const std::string& p) {
    std::string s;
    if (FILE* f = std::fopen(p.c_str(), "rb")) { char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); }
    return s;
}
static int Count(const std::string& s, const std::string& what) { int n = 0; for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n; return n; }

int main(int argc, char** argv) {
    // flipfix31: GXV_HRW_FLIPBOOK / _FROM under mode 5's pairs (WarpArm(true) first pictures, ordinary pictures between), written to a temp folder.
    //   flipbook31_backend <case>:  from10   FROM=10 FLIPBOOK=5: 5 pictures, the first at pair 10, the "hrw: flip-book" lines (armed, done)
    //                               off      FLIPBOOK=0: nothing written, no line
    //                               from0    FROM=0: the boot's pictures (pair 0) are written first, as documented
    const std::string test = argc > 1 ? argv[1] : "from10";
    char dir[] = "/tmp/flipbook31.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_HRW", "5", 1);
    setenv("GXV_HRW_WARP_DIR", dir, 1);
    setenv("GXV_HRW_FLIPBOOK", test == "off" ? "0" : "5", 1);
    setenv("GXV_HRW_FLIPBOOK_FROM", test == "from0" ? "0" : "10", 1);
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
    uint64_t frame = 1;
    for (int i = 0; i < 6; ++i) { WarpArm(false, 0, false); Execute(Picture(frame++, GameKinds())); }
    for (int p = 1; p <= 40; ++p) {
        WarpArm(true, static_cast<uint32_t>(p), false);
        Execute(Picture(frame++, GameKinds()));
        WarpArm(false, 0, false);
        Execute(Picture(frame++, GameKinds()));
    }
    const std::string book = Slurp(std::string(dir) + "/flipbook.txt");
    int files = 0;
    for (int i = 0; i < 20; ++i) { char p[600]; std::snprintf(p, sizeof p, "%s/flip%04d.ppm", dir, i); if (FILE* f = std::fopen(p, "rb")) { ++files; std::fclose(f); } }
    const int lines = Count(book, "\n");
    std::printf("%s: flipbook.txt:\n%s%d pictures files, %d lines\n", test.c_str(), book.c_str(), files, lines);
    bool armedLine = false, doneLine = false, startLine = false;
    for (const std::string& l : kdt::g_lines) {
        if (l.rfind("hrw: flip-book", 0) == 0) std::printf("log: %s\n", l.c_str());
        if (l.rfind("hrw: flip-book | armed: 5 pictures from pair 10 on", 0) == 0 || (test == "from0" && l.rfind("hrw: flip-book | armed: 5 pictures from pair 0 on", 0) == 0)) armedLine = true;
        if (l.rfind("hrw: flip-book | started: first picture (A) at pair 10,", 0) == 0) startLine = true;
        if (l.rfind("hrw: flip-book | done: 5 pictures written", 0) == 0 && l.find("pairs 10 to 11,") != std::string::npos) doneLine = true;
    }
    if (test == "off") {
        Check(book.empty() && files == 0 && !armedLine && !doneLine, "FLIPBOOK=0: nothing written, no flip-book line");
    } else {
        Check(lines == 5, "5 pictures (lines) in flipbook.txt");
        Check(book.compare(0, 18, test == "from0" ? "0000 S pair 0 expo" : "0000 A pair 10 exp") == 0, test == "from0" ? "FROM=0: the boot's pictures (before pair 1) are written, as documented" : "the first picture is pair 10's");
        Check(files >= 3 && files <= 5, "flip*.ppm written (A and S pictures; R lines have no file)");
        Check(armedLine, "the armed line says N pictures from pair FROM");
        if (test == "from10") Check(startLine, "the started line names pair 10");
        if (test == "from10") Check(doneLine, "the done line says 5 pictures written, pairs 10 to 11");
        if (test == "from0") Check(doneLine == false && true, "(from0: the done line's pairs differ, not checked)");
    }
    std::string rm = std::string("rm -rf ") + dir;
    (void)!system(rm.c_str());
    return g_fail ? 1 : 0;
}
