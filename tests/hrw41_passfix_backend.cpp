// finish44 (21 Nov 2026): GXV_HRW41_MASK_PASSFIX in the real back end, on the GL stub, built as the console builds it (the harness of
// tests/keepdepth31_backend.cpp). A pair's first picture whose first record after the game's scene-depth marker (DrawRec::depthMark bit 0) is INSIDE an
// offscreen pass: before the switch such a picture got no depth mask (its pair the c181 warp). With the switch (default 1) the mask is made just before that
// pass begins. The picture is executed for real (PlanFrame + the command loop + MaskMid + MaskSnap); the stub's depth-texture hook records the command
// the back end was at when the mask asked for the EFB's depth.
//   hrw41_passfix_backend <case>      (one case a process: the switches are read once)
//     fix            default: the mask is made, at the pass's begin command, outside the pass; counted "before the pass's begin"
//     off            GXV_HRW41_MASK_PASSFIX=0: no mask (as before), nothing counted for the switch
//     depth-touched  a copy inside the pass clears depth before the marked record: refused and counted, no mask
//     outside        control: the marked record is outside any pass: the mask at the marker as always, the switch counts nothing
//     no-detail      GXV_HRW41_DEPTH=0 (no detail mask wanted): no mask pass at all, with the switch on
//     second-pass    two passes; the marked record is in the second: the mask before the SECOND pass's begin (the latest begin)
//   tools/test_hrw41_passfix.sh
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <string>

namespace pft {
std::vector<std::string> g_lines;
unsigned g_depthModes = 0, g_depthStores = 0;
long g_maskAtCmd = -1; bool g_maskInPass = false; unsigned g_maskAsks = 0;
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
    pft::g_lines.emplace_back(text);
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
void vglSetNextSceneDepthModes(unsigned char, unsigned char store) { ++pft::g_depthModes; pft::g_depthStores += store ? 1u : 0u; }
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) {   // (the EFB's depth as a texture: asked by MaskSnap only here. Where the picture was when it asked:)
    pft::g_maskAtCmd = static_cast<long>(vgx::backend::dplan::g_cmd); pft::g_maskInPass = vgx::backend::g_inPass; ++pft::g_maskAsks;
    return vgx::backend::TgtW();
}
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
// (d851336a / 5aa0ea4d, 21 Nov: the back end now asks vgx_draw.cpp for GXV_HRW and GXV_TAILFOLD; the harness does not link vgx_draw.cpp. Both off, as the back end's own pair flow
// is not run here: the keepdepth/halo plans are made by the harness's own calls.)
int HrwMode() { return 0; }
int TailFoldMode() { return 0; }
}
extern "C" int vgxvita_asyncShaderInCache_v1(const char*, int) { return 1; }

using namespace vgx;
using namespace vgx::backend;

using namespace vgx;
using namespace vgx::backend;

static ShaderSource g_vsrc[4], g_fsrc[10];
static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

enum Kind { kClearEfb, k3D, kZClear, k2D, kPassBegin, kPassDraw, kPassDrawMarked, k3DMarked, kPassCopy, kPassClearCopy };
static void Add(CmdBuf& b, Kind k) {
    if (k == kPassBegin) {
        PassCmd p;
        p.rect = {0, 0, 16, 16};
        b.cmds.push_back({CMD_PASS_BEGIN, static_cast<uint32_t>(b.passes.size())});
        b.passes.push_back(p);
        return;
    }
    if (k == kPassCopy || k == kPassClearCopy) {
        CopyTexCmd t;
        t.texId = 900;
        t.src = {0, 0, 16, 16};
        t.dstW = 16; t.dstH = 16;
        if (k == kPassCopy) t.fromPass = true;   // (the copy that ends the pass)
        else {                                   // (a copy inside the pass of which only the clear is needed: the plan books a whole-depth event for the EFB)
            t.skip = true;
            t.src = {0, 0, 4096, 4096};
            t.clear.enable = true; t.clear.depth = true;
        }
        b.cmds.push_back({CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size())});
        b.copyTex.push_back(t);
        return;
    }
    DrawRec d;
    d.vsId = 0;
    d.fsId = static_cast<uint16_t>(k);
    d.idxCount = d.vtxCount = (k == kClearEfb || k == kZClear) ? 6 : 3;
    d.ublock[UB_PROJ] = 0;
    d.layoutMask = 0;
    RenderState& rs = d.rs;
    rs.scX = 0; rs.scY = 0; rs.scW = 4096; rs.scH = 4096;
    const bool cover = k == kClearEfb || k == kZClear;
    if (cover) { d.coverCull = 1; d.exX = 0; d.exY = 0; d.exW = 4096; d.exH = 4096; rs.cull = CULL_BACK; }
    else { d.exX = 8; d.exY = 8; d.exW = 16; d.exH = 16; }
    if (k == kZClear) { d.coverCull = 0; d.depthMark = 2; }
    if (k == kPassDrawMarked || k == k3DMarked) d.depthMark = 1;   // (the first record after PORT_GX_MARK_SCENE_DEPTH)
    switch (k) {
    case kClearEfb: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; rs.zNear = rs.zFar = 1.0f; break;
    case k3D: case k3DMarked: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_LEQUAL; break;
    case kZClear: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_ALWAYS; rs.colorWrite = 0; rs.alphaWrite = 0; rs.zNear = rs.zFar = 1.0f; break;
    default: rs.depthTest = 0; rs.depthWrite = 0; break;
    }
    if (k == k2D) { rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; d.hud = 1; }
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
    b.uni.assign(16, 0.0f);   // (a perspective projection, as the game's 3D has: MaskSnap finds it for the mask's depth code)
    b.uni[0] = b.uni[5] = 1; b.uni[10] = -1.01f; b.uni[11] = -1; b.uni[14] = -64;
    for (Kind k : kinds) Add(b, k);
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
static size_t IndexOf(const std::vector<Kind>& v, Kind k, int nth = 0) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == k && nth-- == 0) return i;
    return SIZE_MAX;
}

int main(int argc, char** argv) {
    const std::string test = argc > 1 ? argv[1] : "fix";
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_HRW_KEEPDEPTH", "0", 1);
    setenv("GXV_HRW_WARP_DEPTH", "4", 1);
    setenv("GXV_HRW41_DEPTH", test == "no-detail" ? "0" : "1", 1);
    setenv("GXV_HRW41_BOXKEY", "0", 1);
    setenv("GXV_HRW41_BUDGET", "0", 1);
    if (test == "off") setenv("GXV_HRW41_MASK_PASSFIX", "0", 1); else unsetenv("GXV_HRW41_MASK_PASSFIX");   // (the default is the switch on)
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
    // The picture: the EFB's clear, the 3D, an image effect's pass (its own target) holding the first record after the marker, the copy that ends the pass,
    // the game's depth clear, the 2D.
    std::vector<Kind> a{kClearEfb};
    for (int i = 0; i < 12; ++i) a.push_back(k3D);
    if (test == "outside") a.push_back(k3DMarked);
    if (test == "second-pass") { a.push_back(kPassBegin); a.push_back(kPassDraw); a.push_back(kPassCopy); a.push_back(k3D); }
    a.push_back(kPassBegin);
    a.push_back(kPassDraw);
    if (test == "depth-touched") a.push_back(kPassClearCopy);
    a.push_back(test == "outside" ? kPassDraw : kPassDrawMarked);
    a.push_back(kPassCopy);
    a.push_back(kZClear);
    for (int i = 0; i < 4; ++i) a.push_back(k2D);
    const size_t passBegin = IndexOf(a, kPassBegin, test == "second-pass" ? 1 : 0), marked = IndexOf(a, test == "outside" ? k3DMarked : kPassDrawMarked);
    uint64_t frame = 1;
    for (int i = 0; i < 6; ++i) { WarpArm(false, 0, false); Execute(Picture(frame++, a)); }  // (warm-up: programs link; not a pair: nothing asks for a mask)
    Check(pft::g_maskAsks == 0 && g_hrwMaskPlans == 0, "pictures that are not a pair's first plan no mask and make none");
    // The warp's own programs as linked at a stage load (the stub links anything): the detail mask has a consumer.
    if (g_rtW < 64 || g_rtH < 64) { g_rtW = 128; g_rtH = 96; }
    auto& W = warp31::g_w;
    if (!W.progMask) W.progMask = 7001;
    if (test != "no-detail" && !W.prog[warp31::kWarpD41].prog) W.prog[warp31::kWarpD41].prog = 7002;
    WarpArm(true, 7, false);
    Check(warp31::WantsMask() == (test != "no-detail") && warp31::DetailMask() == (test != "no-detail"), "the armed pair wants the detail mask exactly when GXV_HRW41_DEPTH is on");
    const unsigned asks0 = pft::g_maskAsks, masks0 = W.detailMasks;
    Execute(Picture(frame++, a));
    const unsigned asks = pft::g_maskAsks - asks0, masks = W.detailMasks - masks0;
    std::printf("%s: first pictures planned %llu, first marked record inside a pass %llu, before the pass's begin %llu, refused %llu, at the marker %llu | mask asked %u times, made %u, at command %ld (pass begin %zu, marked record %zu), in a pass %d, mask valid for the pair %d\n",
                test.c_str(), (unsigned long long)g_hrwMaskPlans, (unsigned long long)g_hrwMarkInPass, (unsigned long long)g_hrwMaskAtPassBegin, (unsigned long long)g_hrwPassFixRefused,
                (unsigned long long)g_hrwMaskAtMark, asks, masks, pft::g_maskAtCmd, passBegin, marked, pft::g_maskInPass ? 1 : 0, W.maskOk && W.maskId == 7 ? 1 : 0);
    if (test == "fix" || test == "second-pass") {
        Check(g_hrwMaskPlans == 1 && g_hrwMarkInPass == 1, "one first picture planned, its first marked record inside a pass");
        Check(g_hrwMaskAtPassBegin == 1 && g_hrwPassFixRefused == 0 && g_hrwMaskAtMark == 0, "the plan put the mask before the pass's begin (counted there, not as 'at the marker')");
        Check(asks == 1 && masks == 1, "exactly one mask pass ran");
        Check(pft::g_maskAtCmd == static_cast<long>(passBegin) && !pft::g_maskInPass, "it read the EFB's depth at the pass's begin command, outside the pass (before any of the pass's commands ran)");
        Check(W.maskOk && W.maskId == 7, "the pair has a valid mask (its second picture can use the depth warp)");
    } else if (test == "off") {
        Check(g_hrwMaskPlans == 1 && g_hrwMarkInPass == 1, "one first picture planned, its first marked record inside a pass");
        Check(g_hrwMaskAtPassBegin == 0 && g_hrwPassFixRefused == 0, "GXV_HRW41_MASK_PASSFIX=0 counts nothing");
        Check(asks == 0 && masks == 0 && !W.maskOk, "no mask, as before the switch");
    } else if (test == "depth-touched") {
        Check(g_hrwMarkInPass == 1 && g_hrwMaskAtPassBegin == 0 && g_hrwPassFixRefused == 1, "a depth event of the EFB between the pass's begin and the marked record: refused and counted");
        Check(asks == 0 && masks == 0 && !W.maskOk, "no mask (the depth before the pass is not proven to be the marker's)");
    } else if (test == "outside") {
        Check(g_hrwMarkInPass == 0 && g_hrwMaskAtPassBegin == 0 && g_hrwPassFixRefused == 0, "a marked record outside a pass: the switch has nothing to do");
        Check(g_hrwMaskAtMark == 1 && asks == 1 && masks == 1 && pft::g_maskAtCmd == static_cast<long>(marked) && W.maskOk, "the mask at the marker, as always");
    } else if (test == "no-detail") {
        Check(g_hrwMaskPlans == 0 && asks == 0 && masks == 0 && g_hrwMaskAtPassBegin == 0, "no detail mask wanted: no plan, no scene split, no mask pass");
    } else { std::fprintf(stderr, "unknown case %s\n", test.c_str()); return 2; }
    // The pair's second picture and the next ordinary picture still execute (nothing left bound or half open by the early mask).
    WarpArm(false, 0, false);
    Execute(Picture(frame++, a));
    Check(!g_inPass, "the picture after the pair executes and ends outside a pass");
    std::printf("%s: %s\n", test.c_str(), g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
