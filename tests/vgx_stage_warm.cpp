// GXV_STAGEWARM (platform/vgx/src/vgx_backend_gl.cpp): a stage's draws are recorded by shader key hash, kept in a file, and when
// the stage is loaded again its programs are linked ahead of its draws, in quiet moments; the housekeeping evictions spare them.
// Host build against the GL stub (no GPU), as the console builds it (__VGX_VITA__: vitaGL's finite program table, simulated here):
//   g++ -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -Iarmhf/glstub -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src
//       -Iplatform/vgx/third_party tests/vgx_stage_warm.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_autoq.cpp
//       -o /tmp/stage-warm -lpthread && /tmp/stage-warm
// (without -D__VGX_VITA__=1 add armhf/glue/vgl_stub.cpp: the desktop path, no program table limit)
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
static size_t FileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

int main() {
    const char* path = "/tmp/vgx_stage_warm_test.bin";
    std::remove(path);
    setenv("GXV_STAGEWARM_FILE", path, 1);
    setenv("GXV_MAXPROGRAMS", "40", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    uint64_t frame = 1;
    // Stage A is played (live): 20 programs are drawn and recorded; nothing is written while the screen is live.
    scene = 0xA; loading = 0xA; quiet = 0;
    for (int i = 0; i < 3; ++i) Execute(Frame(frame++, 0, 20));
    Require(g_stageSets[0xA].recs.size() == 20, "each program/layout/blend of stage A recorded once");
    Require(FileSize(path) == 8, "file started with its magic, nothing appended in play");
    // A fade closes: the records are appended.
    quiet = 1;
    g_serial += 200;
    Execute(Frame(frame++, 0, 0));
    Require(FileSize(path) == 8 + 24 * 20, "quiet moment appends the 20 records");
    // Stage B is loaded and played; its 30 programs push the map over its limit (40): the housekeeping evicts stage A's.
    loading = 0xB;
    Execute(Frame(frame++, 0, 0));
    Require(g_stageTarget == 0xB && g_stageQueue.empty(), "unknown stage B: empty queue");
    scene = 0xB; quiet = 0;
    for (int i = 0; i < 3; ++i) Execute(Frame(frame++, 20, 30));
    quiet = 1;
    g_serial += 700;
    for (int i = 0; i < 3; ++i) Execute(Frame(frame++, 0, 0));
    unsigned aLinked = 0;
    for (unsigned i = 0; i < 20; ++i) if (g_programs.count((1u << 16) | i)) ++aLinked;
    Require(g_programs.size() <= 40 + 1 && aLinked < 20, "stage A's programs were evicted while B was played");
    Require(FileSize(path) == 8 + 24 * 50, "stage B's 30 records appended too");
    // Stage A is loaded again: its queue links all of its programs before its first draw, while the screen is closed.
    loading = 0xA;
    Execute(Frame(frame++, 0, 0));
    Require(g_stageTarget == 0xA, "target follows the loading key");
    Require(g_stageQueue.empty(), "the quiet frame's slice linked the whole queue (budget 30 ms)");
    for (unsigned i = 0; i < 20; ++i) Require(g_programs.count((1u << 16) | i) && g_programs.at((1u << 16) | i).ok, "stage A program linked ahead");
    // ... and the map's housekeeping (over its limit with B's programs) spares them.
    g_serial += 700;
    Execute(Frame(frame++, 0, 0));
    for (unsigned i = 0; i < 20; ++i) Require(g_programs.count((1u << 16) | i), "stage A program spared by the housekeeping");
    // Its draws then link nothing.
    scene = 0xA; quiet = 0;
    const unsigned links0 = g_stats.programLinks;
    Execute(Frame(frame++, 0, 20));
    Require(g_stats.programLinks == links0, "stage A's draws find their programs linked");
    // The file reads back into the same sets.
    const size_t a = g_stageSets[0xA].recs.size(), b = g_stageSets[0xB].recs.size();
    g_stageSets.clear();
    g_stageRecs = 0;
    g_stageLoaded = false;
    StageLoad();
    Require(g_stageSets[0xA].recs.size() == a && g_stageSets[0xB].recs.size() == b, "file reads back");
    // A record cut short: the file is started anew and written whole at the next flush.
    {
        FILE* f = std::fopen(path, "ab");
        std::fwrite("xyz", 1, 3, f);
        std::fclose(f);
    }
    g_stageSets.clear();
    g_stageRecs = 0;
    g_stageLoaded = false;
    g_stageFileRecs = 0;
    StageLoad();
    Require(FileSize(path) == 8, "damaged file restarted");
    quiet = 1;
    g_serial += 200;
    Execute(Frame(frame++, 0, 0));
    Require(FileSize(path) == 8 + 24 * (a + b), "restarted file rewritten whole");
#ifdef __VGX_VITA__
    {
        // A small program table (112 slots): stage C's 40 programs do not fit beside the 50 of A and B that the map holds above
        // the stage warm-up's reserve (GXV_STAGEWARM_RESERVE, default 48 free). In a quiet moment it deletes programs that are not
        // C's and were not drawn in the last two frames, and goes on; in play it never deletes.
        tableSize = 112;
        for (unsigned i = 0; i < 40; ++i) StageAdd(0xC, StageRec{g_vsrc[2].keyHash, g_fsrc[i].keyHash, 0});
        g_serial += 10;
        loading = 0xC; scene = 0xA; quiet = 0;
        Execute(Frame(frame++, 0, 0));
        Require(g_stageTarget == 0xC && g_stageQueue.size() == 40, "stage C queued, nothing done in play (GXV_STAGEWARM_LIVE_MS=0)");
        const unsigned deletes0 = g_stFrame.deletes;
        Require(deletes0 == 0, "no deletes in play");
        quiet = 1;
        for (int i = 0; i < 6 && !g_stageQueue.empty(); ++i) { g_serial += 3; Execute(Frame(frame++, 0, 0)); }
        unsigned cLinked = 0;
        for (unsigned i = 0; i < 40; ++i) if (g_programs.count((2u << 16) | i) && g_programs.at((2u << 16) | i).id) ++cLinked;
        Require(g_stageQueue.empty() && cLinked == 40, "stage C's programs all linked in quiet frames after making room");
        Require(vglGetFreeProgramSlots() >= 48, "the stage warm-up keeps its reserve free");
        tableSize = 1024;
    }
#endif
    // A stage loaded with no frame handed over (the game's loading thread busy, its main loop waiting): with GXV_STAGEWARM_IDLE=1 the
    // render thread's idle slices build the stage's queue and link its programs - links only: no warm draw (no GXM scene outside a
    // frame, r32/r36 freezes), no delete. The warm draws follow in the next quiet frame's slice, which links nothing more.
    for (unsigned i = 40; i < 64; ++i) StageAdd(0xD, StageRec{g_vsrc[3].keyHash, g_fsrc[i].keyHash, 0});
    quiet = 1;
    loading = 0xD;
    Require(!StageWarmIdleWanted(), "GXV_STAGEWARM_IDLE is off by default");
    {
        const unsigned draws0 = C.warmDraws, deletes0 = g_stFrame.deletes, links0 = g_stats.programLinks;
        // (the switch is read once and was 0 above: the slice that StageWarmIdle runs when it is 1 is exercised directly)
        StageBuild(0xD);
        Require(g_stageTarget == 0xD && g_stageQueue.size() == 24 && g_stageIdleCursor == 0, "stage D queued");
        // (what StageWarmIdle does after its switch test)
        const bool wantedBefore = !g_stageIdleBlocked && g_stageIdleCursor < g_stageQueue.size();
        Require(wantedBefore, "programs left to link between frames");
        StageWarmIdleSlice();
        Require(g_stageIdleCursor == 24 && !g_stageIdleBlocked && g_stageQueue.size() == 24, "all of D's programs tried, the queue kept for the draws");
        for (unsigned i = 40; i < 64; ++i) Require(g_programs.count((3u << 16) | i) && g_programs.at((3u << 16) | i).ok, "stage D program linked between frames");
        Require(C.warmDraws == draws0, "no warm draw between frames");
        Require(g_stFrame.deletes == deletes0, "no program delete between frames");
        Require(g_stats.programLinks == links0 + 24, "24 links between frames");
        g_serial += 3;
        Execute(Frame(frame++, 0, 0));
        Require(g_stageQueue.empty() && g_stats.programLinks == links0 + 24, "the quiet frame's slice made the warm draws, no further links");
    }
    // GXV_STUTTER_LOG: a long frame with its costs accounted gets a line.
    g_stFrame.demandProgs = 5; g_stFrame.demandMs = 60; g_stFrame.texUploads = 2; g_stFrame.texBytes = 4096; g_stFrame.texMs = 3;
    StutterFrameEnd(99, 80.0);
    bool frameLine = false;
    for (const auto& l : lines) if (l.rfind("stutter: frame 99 took 80 ms", 0) == 0 && l.find("draws' new programs 5 (60 ms)") != std::string::npos) frameLine = true;
    Require(frameLine, "stutter line for a long frame");
    // ... and 5 s later the window's summary (play and quiet split).
    usleep(5100000);
    g_screenLive = true;
    g_stFrame.deletes = 3; g_stFrame.deleteMs = 12;
    StutterFrameEnd(100, 20.0);
    bool windowLine = false;
    for (const auto& l : lines)
        if (l.rfind("stutter-5s: in play ", 0) == 0 && l.find("program deletes 3 (12 ms)") != std::string::npos &&
            l.find("; quiet ") != std::string::npos)
            windowLine = true;
    Require(windowLine, "stutter-5s summary");
    // GXV_STUTTER_LOG: a 5-s summary appears.
    bool summary = false;
    for (const auto& l : lines) if (l.rfind("stutter-5s:", 0) == 0 || l.rfind("stage warm-up:", 0) == 0) summary = true;
    Require(summary, "stage warm-up lines in the basic log");
    for (const auto& l : lines) std::printf("  log: %s\n", l.c_str());
    std::printf("PASS stage warm-up: record, append, evict, prelink, spare, reload, repair\n");
    std::remove(path);
    return 0;
}
