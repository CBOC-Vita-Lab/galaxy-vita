// seamless23 (Galaxy Vita 2.3): quality transitions without a jump or a wrong picture, host checks against the GL stub (the console's code
// path, __VGX_VITA__). tools/test_vgx_seamless.sh builds and runs it (twice: switches on, and "off" = GXV_SEAMLESS=0, the 2.3 behaviour).
//   1. GXV_SEAMLESS_PREP: a step UP with nothing made ahead is held (SeamRequest) while its pieces are made one a picture (SeamPendStep), then
//      applied: the change itself makes no pooled object (no render target in play) and takes its premade textures.
//   2. GXV_SEAMLESS_KEEPOLD: the objects of the size left are kept (held OLD); the step back makes no pooled object either.
//   3. GXV_SEAMLESS_LAZY: a texture rewritten whole every picture keeps its texture and size at the change (its picture stays readable); the
//      next copy gives it the new size from a premade texture. Off: it gets a blank texture, and a read before the copy is a STALE READ.
//   4. The watch: a change logs "[seamless] size: ..." after 4 presents with its 4 picture intervals.
//   5. GXV_SEAMLESS_RAMP: the edge step fades the present's edge smoothing over the ramp, then takes the plain present; back at once from 0.
#include <GLES2/gl2.h>
#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <unistd.h>

namespace {
unsigned scene = 0, loading = 0, quiet = 0;
std::vector<std::string> lines;
int failures = 0;
void Check(bool good, const char* why) {
    std::printf("  [%s] %s\n", good ? "ok" : "FAIL", why);
    if (!good) ++failures;
}
}
extern "C" {
uint32_t port_scene_key(void) { return scene; }
uint32_t port_scene_loading_key(void) { return loading; }
int port_scene_loading_active(void) { return loading != 0; }
int port_screen_quiet(void) { return quiet; }
void port_log(const char* fmt, ...) { // (autoq::Log: the verbose log; the player build sends it to the basic log)
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
    if (std::getenv("SEAM_VERBOSE")) std::printf("    v %s\n", text);
}
void port_log_summary(const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
    if (std::getenv("SEAM_VERBOSE")) std::printf("    | %s\n", text);
}
}
namespace vgx {
bool LogEnabled() { return false; }
// (the vertex packer's counters live in vgx_draw.cpp, not linked here)
bool VtxPackOn() { return false; }
void VtxPackCounters(unsigned long*) {}
void VtxPackDrawCounters(unsigned long*) {}
}
#ifdef __VGX_VITA__
#include <psp2/kernel/threadmgr.h>
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
size_t vglMemFree(int) { return 96u << 20; }
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

static GLuint NewTex() { GLuint t = 0; glGenTextures(1, &t); return t; }
// One picture: what CMD_PRESENT does (the pool's counts, EvictIdlePool: g_presents, the watch), with a fixed picture interval.
static void Present() {
    for (PoolFbo& f : g_pool) { f.prevUses = f.uses; f.uses = 0; }
    EvictIdlePool();
}
static unsigned Count(const char* needle) {
    unsigned n = 0;
    for (const std::string& l : lines) n += l.find(needle) != std::string::npos ? 1u : 0u;
    return n;
}
// A copy texture of the game: slot `slot`, ew x eh EFB pixels, at the current scale, written whole (RtMarkWrite) in this picture.
static void GameCopy(uint32_t slot, int ew, int eh) {
    const int w = Rnd(ew * ScaleX()), h = Rnd(eh * ScaleY());
    AcquireCopyTarget(slot, w, h, ew, eh, false);
    RtMarkWrite(slot);
    g_inUseFbo = ~0u;
    BindFbo(g_efbFbo);
}

int main(int argc, char** argv) {
    const bool off = argc > 1 && std::strcmp(argv[1], "off") == 0;
    setenv("GXV_SEAMLESS", off ? "0" : "1", 1);
    setenv("GXV_SEAMLESS_STRESS", "0", 1);
    setenv("GXV_WIDE_RUNGS", "1", 1);
    if (!Init(32, 32)) { std::printf("FAIL: Init\n"); return 1; }
    g_fixedW = g_fixedH = 0; // (Init(w, h) fixes the render size, as GXV_RENDER_SIZE: the ladder's sizes here)
    g_efbW = 640;
    g_efbH = 456;
    // rung 2 (720x404) to start, with the EFB target made
    SetRungFactors(2);
    g_rung = 2;
    Check(CreateTarget(), "EFB target at 720x404");
    for (int i = 0; i < 8; ++i) Present();
    // The game's textures: slot 300 a full-EFB capture read before it is written again (history: resampled), slot 301 a quarter copy written
    // whole every picture before any read (GXV_RUNG_SKIPRS's evidence), slot 302 a half copy with history.
    for (int p = 0; p < 6; ++p) {
        GameCopy(300, 640, 456);
        GameCopy(301, 160, 114);
        GameCopy(302, 320, 228);
        // (300 and 302 are read in the next picture before their copy: history reads)
        Present();
        BindTex(0, g_tex[300].id);
        BindTex(0, g_tex[302].id);
    }
    const GLuint lazyName = g_tex[301].id;
    const int lazyW = g_tex[301].w;
    std::printf("%s\n", off ? "GXV_SEAMLESS=0 (the 2.3 behaviour)" : "GXV_SEAMLESS=1");

    // 1. A step up, nothing made ahead.
    {
        const uint64_t made0 = g_poolMade;
        bool refused = false;
        const bool now = SeamRequest(3, "test: a step up", false, refused);
        if (off) {
            Check(now && g_rung == 3, "off: the step up is applied at once");
            Check(g_poolMade > made0, "off: the change itself makes pooled objects (render targets in play)");
        } else {
            Check(!now && g_rung == 2 && g_seamPend == 3, "the step up is held for its pieces");
            unsigned pictures = 0, piecesPictures = 0;
            uint64_t madeInChange = 0;
            while (g_seamPend != autoq::kNoRung && pictures < 60) {
                quiet = 1; // (a quiet moment: a piece every picture without the slack test)
                const uint64_t m0 = g_poolMade;
                const int r0 = g_rung;
                const int to = SeamPendStep(true, refused);
                if (g_rung != r0) madeInChange = g_poolMade - m0;
                else if (g_poolMade != m0) ++piecesPictures;
                (void)to;
                if (g_rung == r0) { // (the game's picture goes on meanwhile; after the change, its first picture is the lazy test's)
                    GameCopy(300, 640, 456);
                    GameCopy(301, 160, 114);
                    GameCopy(302, 320, 228);
                }
                Present();
                if (g_rung == r0) { BindTex(0, g_tex[300].id); BindTex(0, g_tex[302].id); }
                ++pictures;
            }
            quiet = 0;
            std::printf("    applied after %u pictures, %u of them made pieces\n", pictures, piecesPictures);
            Check(g_rung == 3, "the step up is applied once its pieces are made");
            Check(pictures <= 31, "within GXV_SEAMLESS_PREP_FRAMES");
            Check(madeInChange == 0, "the change itself makes no pooled object (its render targets were made ahead, one piece a picture)");
            Check(Count("[autoq-rung] 2 -> 3 parts") == 1 && Count("premade textures taken 0") == 0, "the change took premade textures");
        }
        for (int i = 0; i < 6; ++i) { Present(); BindTex(0, g_tex[300].id); BindTex(0, g_tex[302].id); }
    }
    // 4. The watch line of that change.
    Check(off ? Count("[seamless] size") == 0 : Count("[seamless] size: rung 2 (720x404) -> 3 (784x440)") == 1,
          off ? "off: no [seamless] line" : "the change's watch line with its 4 pictures");
    // 3. The lazy texture.
    if (!off) {
        Check(g_tex[301].id == lazyName && g_tex[301].w == lazyW, "GXV_SEAMLESS_LAZY: the texture rewritten every picture kept its texture and size");
        BindTex(0, g_tex[301].id); // (a read before the rewrite: the last picture, at the old size - never blank)
        Check(g_seamStaleReads == 0, "a read before its rewrite is no stale read (the old picture)");
        GameCopy(301, 160, 114);
        Check(g_tex[301].id != lazyName && g_tex[301].w == Rnd(160 * ScaleX()), "the next copy gives it the new size (a new name)");
    } else {
        Check(g_tex[301].id != lazyName, "off: the texture got a new (blank) texture at the change (GXV_RUNG_SKIPRS)");
    }
    // 2. The step back down: the old size's objects are kept.
    {
        for (int p = 0; p < 6; ++p) {
            GameCopy(300, 640, 456);
            GameCopy(301, 160, 114);
            GameCopy(302, 320, 228);
            Present();
            BindTex(0, g_tex[300].id);
            BindTex(0, g_tex[302].id);
        }
        unsigned oldHeld = 0;
        for (const PoolFbo& f : g_pool) oldHeld += (f.held & PoolHeld::OLD) ? 1u : 0u;
        if (!off) Check(oldHeld > 0, "GXV_SEAMLESS_KEEPOLD: the objects of 720x404's sizes are kept (held)");
        g_rungPre.clear(); // (GXV_RUNG_PREBUILD's below-rung pieces out of the way: the kept objects alone)
        ReleaseRungHolds();
        const uint64_t made0 = g_poolMade;
        bool refused = false;
        SeamRequest(2, "test: back down (urgent)", true, refused);
        Check(g_rung == 2, "back at 720x404");
        if (!off) Check(g_poolMade == made0, "the step back makes no pooled object: the kept ones of the size are taken again");
        else Check(g_poolMade > made0, "off: the step back makes pooled objects again");
        // the first copies at the size: the kept objects again
        const uint64_t made1 = g_poolMade;
        GameCopy(300, 640, 456);
        GameCopy(301, 160, 114);
        GameCopy(302, 320, 228);
        if (!off) Check(g_poolMade == made1, "the first picture after it makes none either");
        for (int i = 0; i < 6; ++i) Present();
    }
    // Stale reads with GXV_SEAMLESS_LAZY=0's behaviour: the blank texture of the skip, read before its copy.
    {
        const uint64_t stale0 = g_seamStaleReads;
        SeamFreshAdd(NewTex(), 1);
        const GLuint fresh = g_seamFresh[g_seamFreshN - 1].name;
        BindTex(1, fresh);
        if (!off) Check(g_seamStaleReads == stale0 + 1 && Count("[seamless] STALE READ") == 1, "a read of a blank texture before its copy is counted and logged");
        AcquireFbo(64, 64, fresh, false);
        BindTex(1, fresh);
        if (!off) Check(g_seamStaleReads == stale0 + 1, "after a scene drew into it, its reads are the game's again");
    }
    // 5. The edge ramp.
    if (!off) {
        g_stepEdgeWanted = true;
        unsigned frames = 0;
        float last = 1.0f;
        bool monotone = true;
        while (!g_stepEdge && frames < 100) {
            SeamEdgeLatch(g_stepEdgeWanted);
            if (!g_stepEdge) { monotone = monotone && g_seamEdgeFade < last; last = g_seamEdgeFade; }
            ++frames;
        }
        std::printf("    edge smoothing off after %u pictures of fading\n", frames);
        Check(frames == static_cast<unsigned>(SeamRamp()) && monotone, "GXV_SEAMLESS_RAMP: the edge smoothing fades out over the ramp, then the plain present");
        g_stepEdgeWanted = false;
        SeamEdgeLatch(false);
        Check(!g_stepEdge && g_seamEdgeFade > 0.0f && g_seamEdgeFade < 0.2f, "back on: from the plain picture, fading in");
        for (int i = 0; i < 30; ++i) SeamEdgeLatch(false);
        Check(g_seamEdgeFade == 1.0f, "faded in to the full edge smoothing");
    }
    // 6. The half-size effects step: the pass targets of a state are learned the first time, made ahead the next.
    if (!off) {
        auto frame = [&](unsigned& madeAfterLatch) {
            const bool was = g_stepFx;
            AutoQualityFrame();
            const uint64_t m0 = g_poolMade;
            if (g_stepFx) AcquireFbo(80, 57, 0, true); else AcquireFbo(160, 114, 0, true); // (the effects' pass target of each state)
            g_inUseFbo = ~0u;
            BindFbo(g_efbFbo);
            if (g_stepFx != was) madeAfterLatch = static_cast<unsigned>(g_poolMade - m0);
            Present();
        };
        unsigned made = 0;
        for (int i = 0; i < 4; ++i) frame(made);
        g_stepFxWanted = true;
        for (int i = 0; i < 6; ++i) frame(made);
        Check(g_stepFx && made == 1, "half-size effects, the first time: its pass target made in play (learned)");
        g_stepFxWanted = false;
        made = 9;
        for (int i = 0; i < 6; ++i) frame(made);
        Check(!g_stepFx && made == 0, "back off: the old state's target was kept (no render target made in play)");
        for (int i = 0; i < 1100; ++i) frame(made); // (the kept target runs out and is let go)
        bool gone = true;
        for (const PoolFbo& f : g_pool) gone = gone && !(f.w == 80 && f.h == 57);
        Check(gone, "the half state's target let go after GXV_SEAMLESS_KEEPOLD + the idle time");
        g_stepFxWanted = true;
        made = 9;
        unsigned waited = 0;
        while (!g_stepFx && waited < 40) { frame(made); ++waited; }
        std::printf("    half-size effects again: latched after %u pictures\n", waited);
        Check(g_stepFx && made == 0 && waited <= static_cast<unsigned>(SeamPrepFrames()) + 1,
              "the second time: its learned target made ahead, the step latched with no render target made in play");
    }
    // The summary.
    SeamSummary(true);
    if (!off) Check(Count("[seamless] summary, in all") == 1, "the summary line");
    std::printf("%s seamless (%s)\n", failures ? "FAIL" : "PASS", off ? "switches off" : "switches on");
    return failures ? 1 : 0;
}
