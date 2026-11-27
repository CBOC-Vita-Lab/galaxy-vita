// keepdepth31 (17 Nov 2026): GXV_HRW_KEEPDEPTH in the real back end, on the GL stub, built as the console builds it (__VGX_VITA__,
// PORT_RUNTIME_DIAGNOSTICS=0; the harness of tests/dma31_clock_backend.cpp). A picture shaped like the game's: MainLoopFramework::clearEfb's covering
// quad, the 3D (depth-tested, depth-writing), MR::clearZBuffer's quad (depth ALWAYS + write, no colour, no alpha, one constant depth), the 2D (no depth
// test), the display copy and the present. Executed as an armed pair's first picture (WarpArm), then the picture after it.
//   keepdepth31_backend <case>      (one case a process: the switches are read once)
//     kept        GXV_HRW_KEEPDEPTH=1: the clear is left out (one draw fewer than with the switch off), no violation, the next picture overwrites
//                 its depth first (not cleared for)
//     off         GXV_HRW_KEEPDEPTH=0: nothing changes, nothing is counted
//     census      =2: decided and counted, nothing left out
//     read-after  a 2D draw after the clear tests depth: refused, the clear is drawn
//     write-after a draw after the clear writes depth (ALWAYS + write over part of the picture): refused
//     colour      the last covering quad writes colour too (not a depth-only clear): refused
//     next-live   the picture after the kept pair tests depth before overwriting it: the back end clears its depth first
//     unarmed     a picture that is not a pair's first: nothing planned
//     watch       the run-time check itself: forced on, it counts the picture's depth-tested EFB draws (a negative control: it can see a violation)
//     mirror      a clear inside the 3D (GameScene::drawMirror), then the 3D, then the real clear: only the last one is left out
//   emufix31 (GXV_HRW_KEEPDEPTH_2D, and the refusal census):
//     write-after (above) also: the refusal line names the deciding draw (a 3D draw, depth ALWAYS + write) and counts it "would be kept with _2D=1"
//     write2d     GXV_HRW_KEEPDEPTH_2D=1, a 2D draw (HUD markers) writes depth (ALWAYS) after the clear: kept, its write counted apart (no violation),
//                 the next picture overwrites first (nothing inexact)
//     write2d-off the same picture with the switch off: refused by "a 2D draw", counted as kept-with-_2D
//     write2d-read GXV_HRW_KEEPDEPTH_2D=1 and a 2D draw that tests depth after the write: refused (read after it)
//     write2d-next GXV_HRW_KEEPDEPTH_2D=1 and the next picture tests depth first: cleared for it, counted inexact
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
// (d851336a / 5aa0ea4d, 21 Nov: the back end now asks vgx_draw.cpp for GXV_HRW and GXV_TAILFOLD; the harness does not link vgx_draw.cpp. Both off, as the back end's own pair flow
// is not run here: the keepdepth/halo plans are made by the harness's own calls.)
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

enum Kind { kClearEfb, k3D, kZClear, k2D, k2DTest, kPartWrite, kFill, kAlwaysNoWrite, kHudWrite, kHudReader };
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
    case kHudReader: rs.depthTest = 1; rs.depthWrite = 1; rs.depthFunc = CMP_LEQUAL; rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_INV_SRC_ALPHA; d.hud = 1; break;  // (halo32: the HUD markers' draw: LEQUAL + write)
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

int main(int argc, char** argv) {
    const std::string test = argc > 1 ? argv[1] : "kept";
    g_zNoMark = test == "zclear-nomark";
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_ASYNC_LINK", "0", 1);
    unsetenv("GXV_ASYNC_SHADERS");
    setenv("GXV_HRW_KEEPDEPTH", test == "off" ? "0" : test == "census" ? "2" : "1", 1);
    const bool w2d = test == "write2d" || test == "write2d-read" || test == "write2d-next";
    setenv("GXV_HRW_KEEPDEPTH_2D", w2d ? "1" : "0", 1);
    setenv("GXV_HRW_KEEPDEPTH_HUD", test == "hud-reader-off" ? "0" : "1", 1);
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
    for (int i = 0; i < 6; ++i) { WarpArm(false, 0, false); Execute(Picture(frame++, GameKinds())); }  // (warm-up: programs link)
    std::vector<Kind> a = GameKinds();
    if (test == "read-after") a.insert(a.end() - 2, k2DTest), a.insert(a.end() - 1, k2DTest);  // (halo32: two readers; ONE is kept through the pre-pass, test hud-reader)
    if (test == "write-after") a.insert(a.end() - 2, kPartWrite);
    if (w2d || test == "write2d-off") a.insert(a.end() - 2, kHudWrite), a.insert(a.end() - 1, kHudWrite);
    if (test == "write2d-read") a.insert(a.end() - 1, k2DTest);
    if (test == "always-hud") a.insert(a.end() - 2, kAlwaysNoWrite), a.insert(a.end() - 1, kAlwaysNoWrite);
    if (test == "hud-reader" || test == "hud-reader-off" || test == "hud-reader-next") a.insert(a.end() - 2, kHudReader);
    if (test == "hud-reader-two") a.insert(a.end() - 2, kHudReader), a.insert(a.end() - 1, kHudReader);
    if (test == "hud-reader-write") a.insert(a.end() - 3, kHudWrite), a.insert(a.end() - 1, kHudReader);  // (a write before the reader would be wiped by the pre-pass: refused)
    if (test == "colour") { a.push_back(kFill); a.push_back(k2D); }
    size_t mirrorFirst = 0;
    if (test == "mirror") {
        std::vector<Kind> m{kClearEfb, k3D, k3D, kZClear};
        mirrorFirst = m.size() - 1;
        for (int i = 0; i < 6; ++i) m.push_back(k3D);
        m.push_back(kZClear);
        for (int i = 0; i < 4; ++i) m.push_back(k2D);
        a = m;
    }
    if (test == "watch") {  // (the run-time check itself: with the watch forced on, a picture's depth-tested EFB draws are counted)
        keepd::g_c = keepd::Counts{};
        WarpArm(false, 0, false);
        keepd::g_watch = true;
        Execute(Picture(frame++, a));
        const uint64_t v = keepd::g_c.violations;
        std::printf("watch: depth-tested EFB draws counted with the watch forced on: %llu (the picture has 20)\n", static_cast<unsigned long long>(v));
        Check(v >= 20 && !keepd::g_watch, "the violation counter counts depth-tested EFB draws, and the watch ends with the picture");
        {   // the counter mirrors the plan's refusal rule (fixnow31): every depth state, counted or not
            RenderState r{};
            const int funcs[] = {CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL, CMP_GREATER, CMP_NEQUAL, CMP_GEQUAL, CMP_ALWAYS};
            for (int dt = 0; dt < 2; ++dt) for (int w = 0; w < 2; ++w) for (int f : funcs) {
                r.depthTest = dt; r.depthWrite = w; r.depthFunc = f;
                const bool want = dt && (f != CMP_ALWAYS || w);
                const uint64_t before = keepd::g_c.violations;
                keepd::Watch(r, true);
                Check((keepd::g_c.violations - before == 1) == want, "Watch counts exactly the depth states the plan refuses (test, func not ALWAYS or write)");
                keepd::Watch(r, false);
                Check(keepd::g_c.violations - before == (want ? 1u : 0u), "an off-EFB draw is never counted");
            }
        }
        return g_fail ? 1 : 0;
    }
    // the baseline draw count of this very picture, not armed (nothing is planned for it)
    WarpArm(false, 0, false);
    Execute(Picture(frame++, a));
    const uint64_t drawsPlain = g_lastDraws;
    keepd::g_c = keepd::Counts{};
    // the pair's first picture
    WarpArm(test != "unarmed", 7, false);
    Execute(Picture(frame++, a));
    const uint64_t drawsArmed = g_lastDraws;
    const keepd::Counts c = keepd::g_c;
    const bool pending = keepd::g_pending;
    // the picture after it (an ordinary one): it begins with clearEfb, or (next-live) tests depth first
    std::vector<Kind> next = GameKinds();
    if (test == "next-live" || test == "write2d-next") next.insert(next.begin(), k2DTest);
    WarpArm(false, 0, false);
    Execute(Picture(frame++, next));
    const keepd::Counts n = keepd::g_c;
    std::printf("%s: draws executed plain %llu, armed %llu | planned %llu, kept %llu, census %llu, refused: no plan %llu, no clear %llu, read after %llu, "
                "write after %llu, other %llu | violations %llu | next picture: overwrote first %llu, cleared for %llu\n", test.c_str(),
                static_cast<unsigned long long>(drawsPlain), static_cast<unsigned long long>(drawsArmed), static_cast<unsigned long long>(c.pics),
                static_cast<unsigned long long>(c.kept), static_cast<unsigned long long>(c.census), static_cast<unsigned long long>(c.noPlan),
                static_cast<unsigned long long>(c.noClear), static_cast<unsigned long long>(c.readAfter), static_cast<unsigned long long>(c.writeAfter),
                static_cast<unsigned long long>(c.other), static_cast<unsigned long long>(n.violations), static_cast<unsigned long long>(n.nextDead),
                static_cast<unsigned long long>(n.nextCleared));
    // (emufix31: the refusal line and the census of what refused; every keepdepth31 line fits autoq::LogSummary's 511 characters)
    keepd::Report();
    bool refusalLine = false, fits = true;
    std::string refusal;
    for (const std::string& l : kdt::g_lines) {
        if (l.rfind("hrw: keepdepth31", 0) == 0 && l.size() >= 511) fits = false;
        if (l.rfind("hrw: keepdepth31 refusal 1 |", 0) == 0) { refusalLine = true; refusal = l; }
        if (l.rfind("hrw: keepdepth31 refusal 1 event |", 0) == 0) refusal += " || " + l;
    }
    {   // (the same lines with every count at a 7-digit value: still under the limit)
        const keepd::Counts keep = keepd::g_c;
        uint64_t* w = reinterpret_cast<uint64_t*>(&keepd::g_c);
        for (size_t k = 0; k < sizeof(keepd::Counts) / sizeof(uint64_t); ++k) w[k] = 9999999;
        const size_t before = kdt::g_lines.size();
        keepd::Report();
        for (size_t k = before; k < kdt::g_lines.size(); ++k) if (kdt::g_lines[k].size() >= 511) { fits = false; std::printf("too long (%u): %s\n", static_cast<unsigned>(kdt::g_lines[k].size()), kdt::g_lines[k].c_str()); }
        keepd::g_c = keep;
    }
    if (refusalLine) std::printf("%s\n", refusal.c_str());
    Check(fits, "every keepdepth31 line is shorter than autoq::LogSummary's 511 characters (the one line was cut before)");
    const bool w2dKept = test == "write2d" || test == "write2d-next";
    if (w2dKept) {
        Check(c.pics == 1 && c.kept == 1 && c.keptWrites == 1, "GXV_HRW_KEEPDEPTH_2D=1: kept through the 2D draws' partial writes");
        Check(drawsArmed + 1 == drawsPlain, "exactly one draw fewer than the same picture unarmed");
        std::printf("%s: allowed 2D writes executed %llu\n", test.c_str(), static_cast<unsigned long long>(n.writesRun));
        Check(n.violations == 0 && n.writesRun == 2, "no violation; the two allowed 2D writes are counted apart");
        Check(refusalLine && refusal.find("kept, partial draw writes passed over") != std::string::npos && refusal.find("2 partial writes (2 by draws)") != std::string::npos,
              "the line says the picture was kept with 2 draw writes passed over");
        if (test == "write2d") Check(n.nextDead == 1 && n.nextCleared == 0 && n.inexact == 0, "the next picture begins with clearEfb: nothing inexact");
        else Check(n.nextCleared == 1 && n.inexact == 1, "the next picture tests depth first: cleared for it, and counted inexact (the 2D's depth is not restored)");
        return g_fail ? 1 : 0;
    }
    if (test == "write2d-off" || test == "write2d-read") {
        Check(c.pics == 1 && c.kept == 0 && drawsArmed == drawsPlain && !pending, "refused: the clear is drawn");
        if (test == "write2d-off")
            Check(c.writeAfter == 1 && c.by[1][keepd::kBy2D] == 1 && c.writesOnly == 1 && refusalLine && refusal.find("a 2D draw (HUD markers)") != std::string::npos &&
                      refusal.find("depth test 1 always write 1") != std::string::npos,
                  "switch off: refused by a 2D draw's write, named in the refusal line, and counted as kept with GXV_HRW_KEEPDEPTH_2D=1");
        else Check(c.readAfter == 1 && c.by[0][keepd::kBy2D] == 1 && c.writesOnly == 0, "a 2D depth test after the write: refused as a read (the switch cannot keep it)");
        return g_fail ? 1 : 0;
    }
    if (test == "hud-reader" || test == "hud-reader-next") {
        Check(c.pics == 1 && c.kept == 1 && c.preKept == 1 && c.readAfter == 0, "halo32: the one HUD reader after the clear: kept through its pre-pass");
        Check(c.preDraws == 1, "the pre-pass was drawn once, in front of the reader");
        Check(drawsArmed == drawsPlain, "one draw fewer (the clear), one more (the pre-pass): the same count as unarmed");
        Check(n.violations == 0 && pending, "no violation (the reader is the allowed one), the next picture is owed the check");
        Check(keepd::g_keptWrites, "the kept depth is marked as holding the 2D's writes (BOXDEPTH does not key on it)");
        return g_fail ? 1 : 0;
    }
    if (test == "hud-reader-off" || test == "hud-reader-two" || test == "hud-reader-write") {
        Check(c.pics == 1 && c.kept == 0 && c.preKept == 0 && c.preDraws == 0 && drawsArmed == drawsPlain && !pending, "refused: the clear is drawn, no pre-pass");
        Check(c.readAfter + c.writeAfter == 1, "refused by the depth event after the clear");
        return g_fail ? 1 : 0;
    }
    const bool keptCase = test == "kept" || test == "always-hud" || test == "next-live" || test == "mirror";
    if (keptCase) {
        Check(c.pics == 1 && c.kept == 1, "the armed picture's last depth clear is left out");
        Check(drawsArmed + 1 == drawsPlain, "exactly one draw fewer than the same picture unarmed");
        Check(n.violations == 0, "no depth-tested EFB draw ran after the left-out clear");
        Check(pending, "the next ordinary picture is owed the check");
        if (test == "always-hud") Check(c.violations == 0 && c.readAfter == 0 && c.writeAfter == 0, "depth-test ALWAYS draws without a write after the clear: kept, no violation");
        if (test == "next-live") Check(n.nextCleared == 1 && n.nextDead == 0, "the next picture tests depth first: its depth is cleared for it");
        else Check(n.nextDead == 1 && n.nextCleared == 0, "the next picture begins with clearEfb: nothing to do");
        if (test == "mirror") Check(mirrorFirst > 0 && c.kept == 1, "a mirror's clear inside the 3D: the picture still keeps only its last clear (one draw fewer)");
    } else if (test == "off") {
        Check(c.pics == 0 && c.kept == 0 && drawsArmed == drawsPlain && !pending, "switch off: nothing planned, nothing left out");
    } else if (test == "census") {
        Check(c.pics == 1 && c.census == 1 && c.kept == 0 && drawsArmed == drawsPlain && !pending, "census: decided and counted, nothing left out");
    } else if (test == "unarmed") {
        Check(c.pics == 0 && c.kept == 0 && drawsArmed == drawsPlain && !pending, "a picture that is not a pair's first: nothing planned");
    } else {
        Check(c.pics == 1 && c.kept == 0 && drawsArmed == drawsPlain && !pending, "refused: the clear is drawn");
        if (test == "read-after") Check(c.readAfter == 1, "refused because a depth test follows the clear");
        if (test == "write-after")
            Check(c.writeAfter == 1 && c.by[1][keepd::kBy3D] == 1 && c.writesOnly == 1 && refusalLine && refusal.find("a 3D draw") != std::string::npos,
                  "refused because a depth write follows the clear; the refusal line names the draw");
        if (test == "zclear-nomark") Check(c.writeAfter == 1 && c.kept == 0, "the depth clear without the front end's depth-rectangle mark is not found (the console's refusal before 18 Nov)");
        if (test == "colour") Check(c.writeAfter == 1, "a covering quad that writes colour is not a depth-only clear (refused: a write after the real clear)");
    }
    return g_fail ? 1 : 0;
}
