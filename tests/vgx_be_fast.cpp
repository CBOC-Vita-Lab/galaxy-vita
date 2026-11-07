// GXV_BE_FAST (platform/vgx/src/vgx_backend_gl.cpp): the direct draw's layout memo (DirectFor) and its skip of
// SetLayoutConstants. For every layout mask the memo must give the table DirectLayoutTable builds, and for random states of the
// attribute arrays (enabled, constant set) the skip may only happen when SetLayoutConstants would change nothing and make no GL
// call. Also: GXV_BE_PROF's per-picture line comes out with the stage times and counts; GXV_GPU_SCENES splits scene times and reports.
// Host build against the GL stub (no GPU), as the console builds it:
//   g++ -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -Iarmhf/glstub -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src
//       -Iplatform/vgx/third_party tests/vgx_be_fast.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_autoq.cpp
//       -o /tmp/be-fast -lpthread && /tmp/be-fast
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <random>

namespace {
std::vector<std::string> lines;
void Require(bool good, const char* why) {
    if (good) return;
    std::fprintf(stderr, "FAIL: %s\n", why);
    std::exit(1);
}
}
extern "C" {
void port_log(const char*, ...) {}
void port_log_summary(const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
}
}
namespace vgx { bool LogEnabled() { return false; } }
#include <psp2/kernel/threadmgr.h>
extern "C" {
std::uint64_t sceKernelGetProcessTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
unsigned vglGetFreeProgramSlots(void) { return 512; }
unsigned vgx_stub_name_ = 0;
unsigned glCreateShader(unsigned) { return ++vgx_stub_name_; }
unsigned glCreateProgram(void) { return ++vgx_stub_name_; }
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
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { std::abort(); }
int sceKernelSignalSema(SceUID, int) { std::abort(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(SceUID, SceSize, void*) { std::abort(); }
int sceKernelDelayThread(SceUInt) { std::abort(); }
unsigned char vgxvita_maskGuardOn = 0, vgxvita_maskGuardDue = 0;
void vgxvita_maskGuard(void) {}
}

using namespace vgx;
using namespace vgx::backend;

int main() {
    setenv("GXV_PASSHALF", "2", 1); // (case 8; read once at the first PassScaleFor, which nothing before case 8 reaches)
    // 1. the memo against the map, every mask a decoded vertex can have (VL_* bits below 1 << 12), twice (cold and warm)
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t mask = 0; mask < (1u << 12); ++mask) {
            const DirectMemo& m = DirectFor(mask);
            const VgxDirectLayout* t = DirectLayoutTable(mask);
            Require(m.table == t && m.mask == mask, "memo gives another layout table");
            Require(m.constBits == (((1u << AT_COUNT) - 1u) & ~t->enabled), "memo's constant locations");
        }
    // 2. the skip against SetLayoutConstants itself
    std::mt19937 rng(5);
    unsigned skipped = 0, worked = 0;
    for (int i = 0; i < 200000; ++i) {
        const uint32_t mask = rng() & 0xFFFu;
        S.attrEnabled = rng() & ((1u << AT_COUNT) - 1u);
        S.attrConstSet = rng() % 3 == 0 ? (rng() & ((1u << AT_COUNT) - 1u)) : ((1u << AT_COUNT) - 1u) & ~S.attrEnabled;
        if (rng() % 2) S.attrEnabled &= ~DirectFor(mask).constBits; // (the common case: nothing to do)
        const uint32_t cb = DirectFor(mask).constBits;
        const bool skip = !((cb & S.attrEnabled) || (cb & ~S.attrConstSet));
        const uint32_t en0 = S.attrEnabled, cs0 = S.attrConstSet, calls0 = C.glCalls;
        SetLayoutConstants(mask);
        const bool changed = S.attrEnabled != en0 || S.attrConstSet != cs0 || C.glCalls != calls0;
        Require(!(skip && changed), "a skipped SetLayoutConstants would have done something");
        Require(skip || changed, "a needed SetLayoutConstants found nothing to do");
        skip ? ++skipped : ++worked;
    }
    std::printf("layout constants: %u skipped, %u needed work, all as SetLayoutConstants says\n", skipped, worked);
    // 3. GXV_BE_PROF's report (a fake window: the numbers only have to come out)
    beprof::g_on = true;
    beprof::g_every = 1;
    beprof::g_windowStartMs = 1.0; // (process-clock milliseconds: more than 5 s before now)
    beprof::FrameBegin();
    for (int k = 0; k < 10; ++k) { BEPROF_MARK(kDrawProg); ++beprof::g_draws; }
    BEPROF_MARK(kCopy);
    beprof::FrameEnd(6, 2); // (the window began 'long ago': the line is written at this picture's end)
    Require(!lines.empty() && lines.back().find("be-prof:") == 0 && lines.back().find("10 draws") != std::string::npos &&
                lines.back().find("6.0 scenes") != std::string::npos,
            "be-prof line");
    std::printf("%s\n", lines.back().c_str());
    // 4. GXV_GPU_SCENES: the CPU-side descriptors and the split of each scene's GPU time (records as vglRtReadScenes gives them)
    gpuscenes::g_on = true;
    gpuscenes::Begin(gpuscenes::kEfb); // tag 1
    DrawRec d{};
    d.idxCount = 300;
    gpuscenes::NoteDraw(d, nullptr);
    gpuscenes::NoteDraw(d, nullptr);
    gpuscenes::Begin(gpuscenes::kTex); // tag 2
    gpuscenes::NoteDraw(d, nullptr);
    gpuscenes::Close();
    ++gpuscenes::g_cpuPictures;
    gpuscenes::g_timers = true;
    // frame 5: EFB (tag 1) TA 1000..4000, 3D 4000..9000; texture (tag 2) TA 4000..4500, 3D 9000..9500; display (tag 0) 3D 9600..10000
    gpuscenes::Take(VgxRtGpuScene{10, 5, 1, 500, 1000, 4000, 9000, 1, 1, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{11, 5, 2, 1100, 1500, 4500, 9500, 1, 1, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{12, 5, 0, 0, 9550, 9600, 10000, 1, 1, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{13, 6, 0, 0, 20000, 20100, 20200, 1, 1, 0, 0}); // (the next picture: frame 5 is complete)
    Require(gpuscenes::g_gpuPictures == 1, "one picture gathered");
    Require(gpuscenes::g_gpu[gpuscenes::kEfb].taUs == 3000 && gpuscenes::g_gpu[gpuscenes::kEfb].fUs == 5000 && gpuscenes::g_gpu[gpuscenes::kEfb].feedUs == 500,
            "EFB scene: TA 3 ms, 3D 5 ms, feed 0.5 ms");
    Require(gpuscenes::g_gpu[gpuscenes::kTex].taUs == 500 && gpuscenes::g_gpu[gpuscenes::kTex].fUs == 500, "texture scene: TA and 3D after the EFB's");
    Require(gpuscenes::g_gpu[gpuscenes::kDisp].fUs == 400 + 100 && gpuscenes::g_untagged == 2, "display scenes untagged");
    Require(gpuscenes::g_main.scenes == 1 && gpuscenes::g_mainDraws == 2 && gpuscenes::g_main.taUs == 3000, "biggest EFB scene");
    gpuscenes::Report(1e9);
    Require(lines.back().find("gpu-scenes:") == 0 && lines.back().find("EFB: 1.0 scenes, 2 draws, 0.6k indices") != std::string::npos, "gpu-scenes line");
    std::printf("%s\n", lines.back().c_str());
    // 5. GXV_GPU_SCENES across untimed scenes (vitaGL's watcher more than 48 scenes behind): the GPU does scenes 14..17 back to back, 10 ms
    // each (2 ms vertex, 8 ms pixel), from 30 ms on, while they are submitted 1 ms apart: it runs far behind the submissions. 15 and 16 are
    // untimed. Scene 17's busy time starts at 14's completion (not at its own submission, which would count 7 ms of 14's work twice); its
    // TA / 3D go to the picture's totals, not to its kind; busy, TA and 3D summed are never more than the time they span.
    gpuscenes::Take(VgxRtGpuScene{14, 7, 0, 0, 30000, 32000, 40000, 1, 1, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{15, 7, 0, 0, 31000, 0, 0, 0, 0, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{16, 7, 0, 0, 32000, 0, 0, 0, 0, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{17, 7, 0, 0, 33000, 62000, 70000, 1, 1, 0, 0});
    Require(gpuscenes::g_busyUs == 10000 + 30000, "busy across untimed scenes: from the last completion seen (the old way: 10 + 37 ms)");
    Require(gpuscenes::g_untimed == 2 && gpuscenes::g_gapScenes == 1 && gpuscenes::g_gapFUs == 8000, "the scene after the untimed ones: its own 3D, in the gap");
    Require(gpuscenes::g_gpu[gpuscenes::kDisp].scenes == 1 && gpuscenes::g_gpu[gpuscenes::kDisp].fUs == 8000, "per kind: only the clean scene 14");
    {
        uint64_t ta = gpuscenes::g_gapTaUs, f = gpuscenes::g_gapFUs;
        for (const auto& g : gpuscenes::g_gpu) { ta += g.taUs; f += g.fUs; }
        Require(gpuscenes::g_busyUs <= 70000 - 20000 && ta <= 62000 - 20000 && f <= 70000 - 20000, "sums within the span");
    }
    // 6. GXV_GPU_SCENES=2: one EFB scene (720x404 target, EFB 640x456): a mask pass, a full clear, one blended static draw over a quarter of
    // the EFB with a discarding shader; its slot line.
    gpuscenes::g_mode = 2;
    g_efbW = 640; g_efbH = 456; g_rtW = 720; g_rtH = 404; g_efbFbo = 7; S.fbo = 7; S.scissorOn = 0;
    ShaderSource fsrc;
    fsrc.fragment = true;
    fsrc.text = "uniform vec4 u_tev[4];\nvoid main() {\n  // stage 0\n  vec4 tex = texture2D(u_tex0, v_tex0.xy);\n  vec4 c = tex * 2.0;\n  if (c.a < 0.5) discard;\n  gl_FragColor = c;\n}\n";
    g_fsSrc.resize(4, nullptr);
    g_fsSrc[3] = &fsrc;
    gpuscenes::g_timers = false; // (no vglRtTagScene in this test)
    gpuscenes::g_ordinal = 0;    // (a new picture: FrameEnd's)
    gpuscenes::Begin(gpuscenes::kEfb);
    gpuscenes::g_timers = true;
    const uint32_t tag = gpuscenes::g_serial;
    Require(gpuscenes::g_cur->w == 720 && gpuscenes::g_cur->h == 404 && gpuscenes::g_cur->ordinal == 0, "the scene's target and number");
    gpuscenes::g_cur->dLoad = 0;
    gpuscenes::g_cur->dStore = 1;
    gpuscenes::NoteMask(true);
    gpuscenes::NoteClear(true, true);
    {
        CmdBuf buf;
        DrawRec r{};
        r.vbuf = 1; // (static: bounded by its rectangle)
        r.idxCount = 6;
        r.fsId = 3;
        r.exX = 0; r.exY = 0; r.exW = 320; r.exH = 228;
        r.rs.vpX = 0; r.rs.vpY = 0; r.rs.vpW = 640; r.rs.vpH = 456;
        r.rs.blend = 1;
        gpuscenes::NoteDraw(r, nullptr);
        gpuscenes::NoteDrawDetail(buf, r);
    }
    gpuscenes::Close();
    gpuscenes::g_ordinal = 0;
    ++gpuscenes::g_cpuPictures;
    gpuscenes::Take(VgxRtGpuScene{18, 8, tag, 71000, 72000, 73000, 79000, 1, 1, 0, 0});
    gpuscenes::Take(VgxRtGpuScene{19, 9, 0, 0, 80000, 80100, 80200, 1, 1, 0, 0});
    gpuscenes::Report(2e9);
    bool head = false, top = false;
    for (const std::string& l : lines) {
        if (l.find("gpu-scenes top: ") == 0) head = true;
        if (l.find("gpu-scenes top 1: scene 0 EFB 720x404") == 0) {
            top = l.find("(a scene 6.00 / 1.00)") != std::string::npos && l.find("discard 1.0 draws 0.00+<=0.25 screens (1.0 bounded)") != std::string::npos &&
                  l.find("scissor masks 1.0 (1.0 full-target, 1.00 screens)") != std::string::npos && l.find("clears 1.0 (1.00 screens)") != std::string::npos &&
                  l.find("depth load 0% store 100%, colour load dead 100%") != std::string::npos && l.find("fs 3 [1 stages, 1 tex,") != std::string::npos &&
                  l.find(", discard] 1.0 draws 0.25 screens 100%") != std::string::npos;
            std::printf("%s\n", l.c_str());
        }
    }
    Require(head && top, "gpu-scenes top lines (GXV_GPU_SCENES=2)");
    // 7. GXV_PX_NOOP's test: what cannot change a pixel
    {
        RenderState rs;
        rs.colorWrite = 0; rs.alphaWrite = 0;
        Require(PxNoEffect(rs), "nothing written");
        rs.depthTest = 1; rs.depthWrite = 1;
        Require(!PxNoEffect(rs), "depth written");
        rs = RenderState{};
        rs.blend = 1; rs.blendSrc = BF_ZERO; rs.blendDst = BF_ONE;
        Require(PxNoEffect(rs), "blended ZERO / ONE");
        rs.blendEq = BE_REVSUB;
        Require(PxNoEffect(rs), "reverse-subtracted ZERO / ONE");
        rs.dstAlphaZero = 1;
        Require(!PxNoEffect(rs), "alpha factors ZERO / ZERO write alpha 0");
        rs.alphaWrite = 0;
        Require(PxNoEffect(rs), "... unless alpha is not written");
        rs = RenderState{};
        rs.blend = 1; rs.blendSrc = BF_SRC_ALPHA; rs.blendDst = BF_ONE;
        Require(!PxNoEffect(rs), "additive");
    }
    // 8. GXV_PASSHALF=2 (PassInChain): the bloom chain of BloomEffect::postDraw as the front end isolates it (EFB 640x456 at 960x544):
    // only the passes inside the chain (P1 threshold, P2 and P3 blurs) are halved; P0 (reads the EFB's copy) and P4 (its copy is added
    // to the EFB) keep their size. Then the same chain with an EFB draw reading P1's copy: P1 keeps its size.
    {
        g_efbW = 640; g_efbH = 456; g_rtW = 960; g_rtH = 544;
        enum : uint32_t { T28 = 10, T2C, T40, T44, T30, T34, T38, T3C, T24 };
        auto build = [](CmdBuf& b, bool efbReadsP1) {
            auto copy = [&](uint32_t tex, EfbRect src, int dw, int dh, bool fromPass, bool more) {
                CopyTexCmd c;
                c.texId = tex; c.src = src; c.dstW = static_cast<uint16_t>(dw); c.dstH = static_cast<uint16_t>(dh);
                c.fromPass = fromPass; c.passMore = more;
                b.copyTex.push_back(c);
                b.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)});
            };
            auto pass = [&](EfbRect r) {
                PassCmd p;
                p.rect = r;
                b.passes.push_back(p);
                b.cmds.push_back(Cmd{CMD_PASS_BEGIN, static_cast<uint32_t>(b.passes.size() - 1)});
            };
            auto draw = [&](uint32_t tex) {
                DrawRec d{};
                d.texMask = 1;
                d.tex[0].texId = tex;
                b.draws.push_back(d);
                b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
            };
            copy(T28, EfbRect{0, 0, 640, 456}, 320, 228, false, false); // (the EFB's glow mask, box-halved)
            pass(EfbRect{0, 0, 160, 114}); draw(T28); copy(T2C, EfbRect{0, 0, 160, 114}, 160, 114, true, true); copy(T40, EfbRect{0, 0, 160, 114}, 160, 114, true, false);
            pass(EfbRect{320, 0, 160, 114}); draw(T2C); draw(T40); copy(T44, EfbRect{320, 0, 160, 114}, 160, 114, true, false);
            pass(EfbRect{480, 0, 160, 114}); draw(T44); draw(T44);
            copy(T30, EfbRect{480, 0, 160, 114}, 160, 114, true, true); copy(T34, EfbRect{480, 0, 160, 114}, 80, 57, true, false);
            pass(EfbRect{320, 114, 80, 57}); draw(T34); draw(T34); draw(T34); copy(T38, EfbRect{320, 114, 80, 57}, 80, 57, true, false);
            pass(EfbRect{480, 114, 160, 114}); draw(T30); draw(T38); copy(T3C, EfbRect{480, 114, 160, 114}, 160, 114, true, false);
            draw(T24); draw(T3C); // (the EFB: the saved picture back, the bloom added)
            if (efbReadsP1) draw(T44);
        };
        for (int variant = 0; variant < 2; ++variant) {
            CmdBuf b;
            build(b, variant == 1);
            std::string got;
            for (size_t i = 0; i < b.cmds.size(); ++i)
                if (b.cmds[i].type == CMD_PASS_BEGIN) got += PassScaleFor(b, i, b.passes[b.cmds[i].arg]) == 0.5f ? 'h' : 'F';
            std::printf("GXV_PASSHALF=2 bloom chain%s: passes P0..P4 %s\n", variant ? " (an EFB draw reads P1's copy)" : "", got.c_str());
            Require(got == (variant ? "FFhhF" : "FhhhF"), "GXV_PASSHALF=2 halves exactly the passes inside the chain");
        }
    }
    std::printf("PASS GXV_BE_FAST layout memo and constants skip; GXV_BE_PROF line; GXV_GPU_SCENES split, gaps, line and top lines; GXV_PX_NOOP's test; "
                "GXV_PASSHALF=2's chain rule\n");
    return 0;
}
