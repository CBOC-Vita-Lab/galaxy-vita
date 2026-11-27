// GXV_STAGEWARM (platform/vgx/src/vgx_backend_gl.cpp): a stage's draws are recorded by shader key hash, kept in a file, and when
// the stage is loaded again its programs are linked ahead of its draws, in quiet moments; the housekeeping evictions spare them.
// Host build against the GL stub (no GPU), as the console builds it (__VGX_VITA__: vitaGL's finite program table, simulated here):
//   bash tools/test_vgx_stage_warm.sh [--run]   (fix31: the build line is there)
// (stall23: also GXV_STAGEWARM_OWNFIRST=0 GXV_STUTTER_SPLIT=0 /tmp/stage-warm for the "off" order and the shared stutter lines)
// (without -D__VGX_VITA__=1 add armhf/glue/vgl_stub.cpp: the desktop path, no program table limit)
// (fix31) built and run by tools/test_vgx_stage_warm.sh (tests/asynclink_glshim.h: the GL names the stub lacks); as tests/c185_astro_mask_backend.cpp,
// the stub's const vglGetTexDataPointer is replaced, and the console's sceGxmFinish is a no-op
#define vglGetTexDataPointer stage_warm_unused_const_tex_data_declaration
#ifdef __VGX_VITA__
#include "asynclink_glshim.h"
#else
#include <GLES2/gl2.h>
#endif
#undef vglGetTexDataPointer
#ifdef __VGX_VITA__
inline void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<void*>(1); }
inline void sceGxmFinish(void*) {}
#endif
#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

namespace {
unsigned scene = 0, loading = 0, quiet = 1;
// (harvprep31) whether the game's loading thread is building a scene now (port_scene_loading_active). The test's play model predated C182/C183, where it was
// "loading != 0"; since then a frame of a load with few draws is the load's black frame (GXV_LOAD_BLACK) and the deferred queue build runs there
// (GXV_STAGEBUILD_DEFER), so the model says when a load runs, as the game does.
unsigned loadingActive = 0;
long long g_testAge = -1; // (firstuse24) ms since the pad was last touched, as platform/src/input/wpad.cpp's port_player_input_age_ms gives it
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
int port_scene_loading_active(void) { return loadingActive != 0; }
int port_screen_quiet(void) { return quiet; }
int64_t port_player_input_age_ms(void) { return g_testAge; }
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
// (stall23, GXV_STUTTER_WAITS) what vita/vgx-game/wait_wrap.cpp gives the console's back end: the test sets the counters
static uint64_t g_testWaits[8] = {};
static int g_testWaitsOn = -1;
extern "C" void vgxvita_waitStats_v1(uint64_t out[8]) { for (int i = 0; i < 8; ++i) out[i] = g_testWaits[i]; }
extern "C" void vgxvita_waitStatsStart_v1(int on) { g_testWaitsOn = on; }
namespace vgx {
bool LogEnabled() { return false; }
// (the vertex packer's counters live in vgx_draw.cpp, not linked here)
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t*) {}
void VtxPackDrawCounters(uint64_t*) {}
// (fix31: the units this test does not link, stubbed as the other stubs here: no prepass, no character textures)
bool PrepassAny() { return false; }
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
bool IsCharaTexture(uint32_t) { return false; }
}
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
static CmdBuf FrameVs(uint64_t frame, uint16_t vs, unsigned firstFs, unsigned count) {
    CmdBuf b = Frame(frame, 0, 0);
    b.cmds.clear();
    b.draws.clear();
    for (unsigned i = 0; i < count; ++i) {
        DrawRec d;
        d.vsId = vs;
        d.fsId = static_cast<uint16_t>(firstFs + i);
        d.idxCount = d.vtxCount = 3;
        d.ublock[UB_PROJ] = 0;
        d.layoutMask = 0;
        b.draws.push_back(d);
        b.cmds.push_back({CMD_DRAW, i});
    }
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

#ifndef __VGX_VITA__
// GXV_STAGEWARM_COMPILE_MS (desktop build: GXV_SIM_COMPILE_MS/SLEEP make each shader compile cost real time). A fresh process, 60 programs
// of a stage nobody has compiled; quiet frames (the loading screen). "off": 30 ms a frame as before; "on": the slices run past it
// while the extra budget lasts, then the rest is logged and goes on in the usual slices.
static int BudgetMain(bool on) {
    setenv("GXV_SIM_COMPILE_MS", "5", 1);
    setenv("GXV_SIM_COMPILE_SLEEP", "1", 1);
    setenv("GXV_STAGEWARM_COMPILE_MS", on ? "150" : "0", 1);
    setenv("GXV_STAGEWARM_FILE", "/tmp/vgx_stage_warm_budget.bin", 1);
    std::remove("/tmp/vgx_stage_warm_budget.bin");
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    for (unsigned i = 0; i < 60; ++i) StageAdd(0x6, StageRec{g_vsrc[0].keyHash, g_fsrc[i].keyHash, 0});
    uint64_t frame = 1;
    quiet = 1; scene = 0; loading = 0x6;
    g_serial += 10;
    Execute(Frame(frame++, 0, 0));
    Require(g_stageTarget == 0x6, "stage queued");
    const size_t afterFirst = 60 - g_stageQueue.size();
    std::printf("  first quiet frame: %u of 60 programs, extra %.0f ms\n", static_cast<unsigned>(afterFirst), g_stageCompileMs);
    if (!on) {
        Require(afterFirst <= 8 && g_stageCompileMs == 0, "COMPILE_MS=0: one 30 ms slice, as before");
        for (int i = 0; i < 20 && !g_stageQueue.empty(); ++i) { g_serial += 3; Execute(Frame(frame++, 0, 0)); }
        Require(g_stageQueue.empty() && g_stageCompileMs == 0, "COMPILE_MS=0: the queue drains in the usual slices");
    } else {
        Require(afterFirst >= 15 && g_stageCompileMs > 0, "COMPILE_MS=150: the first slice ran past 30 ms");
        for (int i = 0; i < 40 && !g_stageQueue.empty(); ++i) { g_serial += 3; Execute(Frame(frame++, 0, 0)); }
        Require(g_stageQueue.empty(), "the queue drains");
        Require(g_stageCompileMs >= 100.0 && g_stageCompileMs <= 150.0 + 30.0, "extra time stays within the budget (one entry's overshoot)");
        bool logged = false;
        for (const auto& l : lines) if (l.find("compile budget of 150 ms used up") != std::string::npos) logged = true;
        Require(logged, "what was left is logged");
        // a new load starts with a new budget
        for (unsigned i = 0; i < 4; ++i) StageAdd(0x7, StageRec{g_vsrc[1].keyHash, g_fsrc[i].keyHash, 0});
        loading = 0x7; g_serial += 3;
        Execute(Frame(frame++, 0, 0));
        Require(g_stageTarget == 0x7 && g_stageCompileMs < 150.0, "the budget is per load");
        // in play nothing is spent beyond the live slice
        g_stageCompileMs = 0;
    }
    std::printf("PASS stage warm-up compile budget (%s)\n", on ? "150 ms" : "off");
    std::remove("/tmp/vgx_stage_warm_budget.bin");
    return 0;
}
#endif

// (harvprep31) A harvest run (GXV_SEEDWALK=1): a stage keeps up to 8,192 own records (a package list that is full at the release's 2,048 still learns), and every
// distinct draw of the run, held records included, is written by the list writer thread to the stage file's path + ".drawn" (VGXSTD01); loads are not draws.
static int SeedWalkMain() {
    setenv("GXV_SEEDWALK", "1", 1);
    setenv("GXV_STAGEJOURNAL_DELAY_MS", "50", 1);
    setenv("GXV_STAGEJOURNAL_GAP_MS", "0", 1);
    const char* path = "/tmp/vgx_stage_warm_sw.bin";
    const std::string drawnPath = std::string(path) + ".drawn";
    for (const char* x : {"", ".drawn", ".drawn.tmp", ".jnl", ".jnl.tmp", ".ply", ".ply.tmp"}) std::remove((std::string(path) + x).c_str());
    setenv("GXV_STAGEWARM_FILE", path, 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    RegisterAll();
    Require(SeedWalkOn() && StageMaxRecs() == 8192, "GXV_SEEDWALK=1: 8,192 own records a stage");
    // a package list whose stage 0x5A is full at the release's cap: 2,048 records of draws that are not the ones played below
    g_stageLoading = true;
    for (unsigned i = 0; i < 2048; ++i) Require(StageAdd(0x5A, StageRec{0x7000u + i, 0x8000u + i, 0}), "package records added");
    g_stageLoading = false;
    Require(g_swDrawn.empty(), "loads are not draws: no evidence");
    uint64_t frame = 1;
    scene = 0x5A; loading = 0x5A; quiet = 1;
    loadingActive = 1; Execute(Frame(frame++, 0, 0)); loadingActive = 0;
    quiet = 0;
    Execute(Frame(frame++, 0, 6));   // vs 1, fs 0..5: six new draws
    Require(g_stageSets[0x5A].recs.size() == 2048 + 6, "the stage learned past the release's cap (2,048 + 6 records)");
    bool capLine = false;
    for (const auto& l : lines) if (l.find("cap-full:") != std::string::npos) capLine = true;
    Require(!capLine, "no cap-full line below 8,192");
    Require(g_swDrawn.size() == 6, "six distinct draws in the evidence");
    const StageRec drawn3 = g_stageSets[0x5A].recs[2048 + 3];
    Execute(Frame(frame++, 0, 6));   // the same draws again: nothing new
    Require(g_swDrawn.size() == 6, "a draw is in the evidence once");
    // a held record: stage 0x5C's list holds the draw of fs 3 already (a package record): drawn, it is in the evidence, though no record is added
    g_stageLoading = true;
    Require(StageAdd(0x5C, drawn3), "stage 0x5C holds fs 3's draw");
    g_stageLoading = false;
    scene = 0x5C; loading = 0x5C; quiet = 1;
    loadingActive = 1; Execute(Frame(frame++, 0, 0)); loadingActive = 0;
    quiet = 0;
    Execute(Frame(frame++, 0, 6));
    Require(g_stageSets[0x5C].recs.size() == 6, "stage 0x5C: one held, five learned");
    Require(g_swDrawn.size() == 12, "stage 0x5C's six draws in the evidence, the held one included");
    bool held = false;
    for (const StageFileRec& r : g_swDrawn) held = held || (r.stage == 0x5C && r.vs == drawn3.vs && r.fs == drawn3.fs && r.lb == (drawn3.lb & vgx::firstvisit::drawMask));
    Require(held, "the held record's draw is in the evidence");
    // the writer thread: the evidence on the card
    std::vector<StageFileRec> got;
    for (int i = 0; i < 100 && !(PlyRead(drawnPath, got, kDrawnMagic, 65536) && got.size() == 12); ++i) { usleep(20000); Execute(Frame(frame++, 0, 6)); }
    Require(got.size() == 12, "the evidence file holds the twelve draws (VGXSTD01, read back with the player file's reader)");
    for (const StageFileRec& r : got) Require((r.stage == 0x5A || r.stage == 0x5C) && (r.lb & ~vgx::firstvisit::drawMask) == 0, "evidence records: the stage and the draw bits only");
    // the per-stage cap of a harvest
    g_stageLoading = true;
    for (unsigned i = 0; g_stageSets[0x5B].recs.size() < 8192; ++i) StageAdd(0x5B, StageRec{0x9000u + i, 0xA000u + i, 0});
    g_stageLoading = false;
    Require(!StageAdd(0x5B, StageRec{0xFFFF1u, 0xFFFF2u, 0}), "the 8,193rd record of a stage is dropped");
    for (const auto& l : lines) if (l.find("cap-full: stage 0000005b holds 8192 records (limit 8192, the harvest's)") != std::string::npos) capLine = true;
    Require(capLine, "and said: cap-full, the harvest's limit");
    for (const char* x : {"", ".drawn", ".drawn.tmp", ".jnl", ".jnl.tmp", ".ply", ".ply.tmp"}) std::remove((std::string(path) + x).c_str());
    std::printf("PASS stage warm-up, harvest (GXV_SEEDWALK=1): 8,192 a stage, draw evidence with held records, written by the writer thread\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "seedwalk") == 0) return SeedWalkMain();
#ifndef __VGX_VITA__
    if (argc > 1) return BudgetMain(std::strcmp(argv[1], "budget-on") == 0);
#else
    (void)argc; (void)argv;
#endif
    // (stall23: the hold's times, small; StageWarmHold reads them once, at its first call below)
    setenv("GXV_STAGEWARM_HOLD_MS", "150", 0);
    setenv("GXV_STAGEWARM_HOLD_OWN_MS", "600", 0);
    setenv("GXV_STAGEWARM_HOLD_STALL_MS", "200", 0);
    const char* path = "/tmp/vgx_stage_warm_test.bin";
    std::remove(path);
    setenv("GXV_STAGEWARM_FILE", path, 1);
    setenv("GXV_MAXPROGRAMS", "40", 1);
    setenv("GXV_PLAYERFILE_DELAY_MS", "200", 0);
    std::remove((std::string(path) + ".ply").c_str());
    std::remove((std::string(path) + ".ply.tmp").c_str());
    std::remove((std::string(path) + ".drawn").c_str());   // (harvprep31: a GXV_SEEDWALK=1 run of this test leaves one)
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
        // (harvprep31, the model of C182/C183) The game announces C while A is still played: the picture is live and full (A's 20 draws), so the queue's
        // build waits (GXV_STAGEBUILD_DEFER): nothing of C is queued, linked or made room for in play. Then the loading thread builds C's scene: its first
        // frame still shows A's last picture, the next ones are the load's black frames (GXV_LOAD_BLACK: at most 8 draws): C's queue is built in the first of
        // them, and that frame is not a visible one (it may link and make room); then the quiet frames (the wipe, the hold) link the rest.
        tableSize = 112;
        for (unsigned i = 0; i < 40; ++i) StageAdd(0xC, StageRec{g_vsrc[2].keyHash, g_fsrc[i].keyHash, 0});
        g_serial += 10;
        scene = 0xA; quiet = 0; loadingActive = 0;
        Execute(Frame(frame++, 0, 20));   // (A in play, its picture full)
        const uint32_t before = g_stageTarget;
        const unsigned linksBefore = g_stats.programLinks;
        loading = 0xC;
        for (int i = 0; i < 3; ++i) Execute(Frame(frame++, 0, 20));
        Require(g_stageTarget == before && g_stageTarget != 0xC && g_h33Pending.load() == 0xC && g_h33PendTarget == 0xC,
                "stage C announced in play: its queue's build waits for a frame that is not live (GXV_STAGEBUILD_DEFER)");
        Require(g_stats.programLinks == linksBefore && g_stFrame.deletes == 0, "nothing done for C in play: no link, no delete");
        loadingActive = 1;   // (the loading thread builds C's scene)
        Execute(Frame(frame++, 0, 20));   // (its first frame: A's last picture, still full: not the black screen)
        Require(g_stageTarget != 0xC && g_stFrame.deletes == 0, "the load's first frame still shows A: still waiting");
        Execute(Frame(frame++, 0, 0));    // (the black screen: the previous picture had 20 draws, this one none)
        Execute(Frame(frame++, 0, 0));    // (a black frame after a black frame: GXV_LOAD_BLACK)
        Require(g_stageTarget == 0xC, "C's queue built in a black frame of the load (GXV_LOAD_BLACK)");
        bool black = false;
        for (const auto& l : lines) if (l.find("queue build waited") != std::string::npos && l.find("in a black frame of the stage load") != std::string::npos) black = true;
        Require(black, "the hitch33 line says the build ran in the load's black frame");
        loadingActive = 0;
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
    // The package's list (GXV_STAGEWARM_PKG_FILE, read-only) is merged with the card's: the card's records stay, new ones are added and
    // written to the card's file at the next flush (the union), a list of another format or switched off (GXV_STAGEWARM_PKG=0) adds nothing.
    {
        const char* pkg = "/tmp/vgx_stage_warm_pkg.bin";
        const auto Write = [&](const char* file, const char* magic, const std::vector<StageFileRec>& recs, const char* tail) {
            FILE* f = std::fopen(file, "wb");
            std::fwrite(magic, 1, 8, f);
            for (const StageFileRec& r : recs) std::fwrite(&r, sizeof r, 1, f);
            if (tail) std::fwrite(tail, 1, std::strlen(tail), f);
            std::fclose(f);
        };
        const auto Reload = [&] {
            g_stageSets.clear();
            g_stageRecs = 0;
            g_stageLoaded = false;
            g_stageFileRecs = 0;
            StageLoad();
        };
        // the card's file as it is now: its records are persisted; remember them
        Reload();
        const size_t cardRecs = g_stageRecs, cardBytes = FileSize(path);
        Require(cardBytes == 8 + 24 * cardRecs, "card file reads back whole");
        const size_t aRecs = g_stageSets[0xA].recs.size();
        const StageRec aFirst = g_stageSets[0xA].recs[0];
        // package: one record the card has (stage A's first), three new (stage E), one more for stage A; 5 bytes of a cut record
        const std::vector<StageFileRec> list = {
            {0xA, aFirst.lb, aFirst.vs, aFirst.fs}, {0xE, 0, g_vsrc[0].keyHash, g_fsrc[0].keyHash}, {0xE, 0, g_vsrc[0].keyHash, g_fsrc[1].keyHash},
            {0xE, 0, g_vsrc[0].keyHash, g_fsrc[2].keyHash}, {0xA, 7, g_vsrc[0].keyHash, g_fsrc[3].keyHash}};
        Write(pkg, "VGXSTW01", list, "abcde");
        setenv("GXV_STAGEWARM_PKG", "0", 1);
        setenv("GXV_STAGEWARM_PKG_FILE", pkg, 1);
        Reload();
        Require(g_stageRecs == cardRecs && !g_stageSets.count(0xE), "GXV_STAGEWARM_PKG=0: the package's list is not read");
        setenv("GXV_STAGEWARM_PKG", "1", 1);
        Reload();
        Require(g_stageRecs == cardRecs + 4, "package merged: 4 new records, the card's duplicate not added twice, the cut tail ignored");
        Require(g_stageSets[0xE].recs.size() == 3 && g_stageSets[0xA].recs.size() == aRecs + 1, "new records in their stages");
        Require(g_stageSets[0xA].recs[0].fs == aFirst.fs && g_stageSets[0xA].persisted == aRecs && g_stageSets[0xE].persisted == 0,
                "the card's records come first and stay persisted; the package's are not");
        Require(FileSize(path) == cardBytes, "reading the package writes nothing yet");
        StageFlush();
        Require(FileSize(path) == cardBytes + 24 * 4, "the next flush appends the union to the card's file");
        // the card now holds the union: a start without the package reads the same sets
        unsetenv("GXV_STAGEWARM_PKG_FILE");
        Reload();
        Require(g_stageRecs == cardRecs + 4 && g_stageSets[0xE].recs.size() == 3, "the card's file keeps what the package gave");
        // a list of another format (or an older magic) is ignored, the card's is untouched
        Write(pkg, "VGXSTW00", list, nullptr);
        setenv("GXV_STAGEWARM_PKG_FILE", pkg, 1);
        Reload();
        Require(g_stageRecs == cardRecs + 4, "package list with another magic ignored");
        setenv("GXV_STAGEWARM_PKG_FILE", "/tmp/vgx_stage_warm_missing.bin", 1);
        Reload();
        Require(g_stageRecs == cardRecs + 4, "missing package list ignored");
        unsetenv("GXV_STAGEWARM_PKG_FILE");
        std::remove(pkg);
        // and the card's file is started anew: the union is written whole, the package's records included
        setenv("GXV_STAGEWARM_PKG_FILE", "/tmp/vgx_stage_warm_pkg2.bin", 1);
        Write("/tmp/vgx_stage_warm_pkg2.bin", "VGXSTW01", list, nullptr);
        std::remove(path);
        Reload();
        Require(g_stageRecs == 5 && FileSize(path) == 8, "no card file: only the package's records");
        StageFlush();
        Require(FileSize(path) == 8 + 24 * 5, "a new card file gets the package's records at the next flush");
        unsetenv("GXV_STAGEWARM_PKG_FILE");
        std::remove("/tmp/vgx_stage_warm_pkg2.bin");
    }
    // (GXV_STAGEWARM_REBUILD: C182 replaced smooth23's queue rebuild by the late registration of a record's shaders, StageLateOn; that path is
    // tested in tests/c182_firstvisit_backend.cpp, "late player". fix31 removed the stale rebuild case that no longer compiled.)
    // (stall23) GXV_STAGEWARM_OWNFIRST (default 1): the stage's own records whose programs are not linked go first, in their order; the count
    // of those programs is what the hold (GXV_STAGEWARM_HOLD_OWN_MS) waits for.
    {
        const bool ownFirst = !std::getenv("GXV_STAGEWARM_OWNFIRST") || std::atoi(std::getenv("GXV_STAGEWARM_OWNFIRST")) != 0;
        for (unsigned i = 50; i < 60; ++i) StageAdd(0x11, StageRec{g_vsrc[2].keyHash, g_fsrc[i].keyHash, 0});
        for (unsigned i = 50; i < 56; i += 2) Require(GetProgram(2, static_cast<uint16_t>(i)).ok, "programs 50, 52, 54 linked before the load");
        quiet = 1; loading = 0x11;
        StageBuild(0x11);
        Require(g_stageOwnLeft.load() == 7 && g_stageOwnOpen.size() == 7, "7 of the stage's 10 own programs not linked when its queue is made");
        std::vector<unsigned> order;
        for (unsigned i = 0; i < 10 && i < g_stageQueue.size(); ++i) order.push_back(g_stageQueue[i].fsId);
        const std::vector<unsigned> first = {51, 53, 55, 56, 57, 58, 59, 50, 52, 54}, plain = {50, 51, 52, 53, 54, 55, 56, 57, 58, 59};
        Require(order == (ownFirst ? first : plain), ownFirst ? "OWNFIRST: the unlinked ones first, each part in its recorded order" : "OWNFIRST=0: the recorded order");
        g_serial += 3;
        Execute(Frame(frame++, 0, 0));
        Require(g_stageOwnLeft.load() == 0 && g_stageOwnOpen.empty(), "the slice linked them: none of the stage's own programs left open");
        Require(g_stageProgressMs.load() > 0.0 && !g_stageSliceBusy.load(), "the slice stamped its progress and is not running");
        std::printf("  OWNFIRST %s: queue order checked, own programs left 7 -> 0\n", ownFirst ? "on" : "off");
    }
    // (stall23) GXV_STAGEWARM_HOLD_OWN_MS: the hold's cap is the longer one while the stage's own programs are not linked, a running slice is
    // never a stall, a slice's progress is a move. (Small times here: GXV_STAGEWARM_HOLD_MS=150, _HOLD_OWN_MS=600, _HOLD_STALL_MS=200.)
    {
        const auto Has = [&](const char* text) { for (const auto& l : lines) if (l.find(text) != std::string::npos) return true; return false; };
        const auto Rearm = [&] { loading = 1; loadingActive = 1; Require(StageWarmHold() == 0, "a load in progress: no hold"); loading = 0; loadingActive = 0; };
        g_stageMenuQueue.store(false);
        g_stageSliceBusy.store(false);
        // A: own programs left past the usual cap: held; once they are linked the usual cap ends it at once.
        Rearm();
        g_stageLeft.store(10); g_stageOwnLeft.store(3);
        Require(StageWarmHold() == 1, "A: held");
        for (int i = 0; i < 5; ++i) { usleep(50000); g_stageProgressMs.store(NowMs()); Require(StageWarmHold() == 1, "A: held past 150 ms while own programs are left"); }
        Require(Has("the hold goes on past 150 ms: 3 of the stage's own programs are not linked yet"), "A: the extension is logged");
        g_stageOwnLeft.store(0);
        Require(StageWarmHold() == 0 && Has("hold over after") && Has("(cap), 10 draw variants left, 0 of the stage's own programs not linked"), "A: own programs linked: the usual cap ends it");
        // B: a slice running (one long compile) is no stall; when it stops and nothing moves for 200 ms, the hold ends.
        Rearm();
        g_stageLeft.store(10); g_stageOwnLeft.store(2);
        Require(StageWarmHold() == 1, "B: held");
        g_stageSliceBusy.store(true);
        for (int i = 0; i < 4; ++i) { usleep(80000); Require(StageWarmHold() == 1, "B: a running slice is not a stall"); }
        g_stageSliceBusy.store(false);
        g_stageProgressMs.store(NowMs());
        g_stageOwnLeft.store(1);
        Require(StageWarmHold() == 1, "B: the slice's end moved the queue");
        usleep(260000);
        Require(StageWarmHold() == 0 && Has("(queue not moving), 10 draw variants left, 1 of the stage's own programs not linked"), "B: a stall ends it");
        // C: the longer cap ends it too.
        Rearm();
        g_stageLeft.store(10); g_stageOwnLeft.store(5);
        Require(StageWarmHold() == 1, "C: held");
        int polls = 0;
        while (StageWarmHold() == 1 && polls < 40) { usleep(50000); g_stageProgressMs.store(NowMs()); ++polls; }
        Require(polls >= 10 && polls <= 14, "C: the hold ends at GXV_STAGEWARM_HOLD_OWN_MS (600 ms)");
        Require(Has("(cap), 10 draw variants left, 5 of the stage's own programs not linked"), "C: logged with what was left");
        // D: the queue done ends it with the summary line.
        Rearm();
        g_stageLeft.store(4); g_stageOwnLeft.store(0);
        Require(StageWarmHold() == 1, "D: held");
        g_stageLeft.store(0);
        Require(StageWarmHold() == 0 && Has("the stage's own programs not linked: 0 at the hold's start, 0 now"), "D: done");
        g_stageLeft.store(0);
        std::printf("  HOLD_OWN: extension, busy slice, stall, own cap, done: checked (%d polls to the own cap)\n", polls);
    }
    // (stall23) a frame's new shaders (CMD_SHADER) are their own part of "the rest"; the window counts them
    {
        static ShaderSource fresh;
        fresh.id = 65; fresh.fragment = true; fresh.keyHash = 0x9100;
        fresh.text = "precision mediump float; void main(){gl_FragColor=vec4(0.25);}";
        CmdBuf b = Frame(frame++, 0, 0);
        b.shaders.push_back(&fresh);
        b.cmds.insert(b.cmds.begin(), Cmd{CMD_SHADER, 0});
        const unsigned before = g_stQuiet.newShaders + g_stPlay.newShaders;
        quiet = 1; loading = 0x11;
        Execute(b);
        Require(g_stQuiet.newShaders + g_stPlay.newShaders == before + 1, "the CMD_SHADER counted as a new shader");
        Require(g_fs.size() > 65 && g_fs[65] != 0, "and compiled as before");
    }
    // (firstuse24) GXV_PLAYERWARM (default 1): the programs a draw links within GXV_PLAYERWARM_WINDOW_MS (6000) of a pad input go into the
    // player list (the pseudo stage 'PLYR'), which every stage's queue warms after its own records; without a recent input nothing is recorded.
    {
        const bool pw = !std::getenv("GXV_PLAYERWARM") || std::atoi(std::getenv("GXV_PLAYERWARM")) != 0;
        const auto Has = [&](const char* text) { for (const auto& l : lines) if (l.find(text) != std::string::npos) return true; return false; };
        const auto PlayerRecs = [&]() -> size_t { const auto it = g_stageSets.find(kPlayerKey); return it == g_stageSets.end() ? 0 : it->second.recs.size(); };
        const unsigned savedScene = scene, savedLoading = loading, savedQuiet = quiet;
        // Stage P is played; the pad was touched 120 ms ago: 4 new programs (vs 3, fs 60..63) are linked by draws
        scene = 0x51; loading = 0x51; quiet = 0; g_testAge = 120;
        g_serial += 3;
        loadingActive = 1;   // (harvprep31: the frame that starts the stage is its load's black frame: the queue is built there, GXV_LOAD_BLACK)
        Execute(Frame(frame++, 0, 0)); // (the frame that starts the stage: its queue is made)
        loadingActive = 0;
        Require(g_stageTarget == 0x51, "the stage's queue built in its load's black frame");
        Execute(Frame(frame++, 0, 0)); // (harvprep31: the stage's first live picture, its opening, before the player's first move: the first-use ledger opens there)
        Execute(FrameVs(frame++, 3, 60, 4));
        for (unsigned i = 60; i < 64; ++i) Require(g_programs.count((3u << 16) | i) && g_programs.at((3u << 16) | i).ok, "player programs linked by their draws");
        Require(PlayerRecs() == (pw ? 4u : 0u), pw ? "PLAYERWARM: 4 programs linked within the window of a pad input recorded in the player list" : "PLAYERWARM=0: nothing recorded");
        Require(g_fu.playerLinks == (pw ? 4u : 0u) && g_fu.playerAdded == (pw ? 4u : 0u), "the ledger counted the 4 links after an input and the 4 records");
        if (pw) Require(Has("first-use: player list: program vs 00001003 fs 0000203c") && Has("4 records now") , "the recording is logged");
        // The same programs drawn again, other draws without an input in the window: nothing new (a link without an input is not the player's)
        g_testAge = 7000;
        Execute(FrameVs(frame++, 3, 56, 4)); // fs 56..59 with vs 3: new programs, input 7 s ago
        Require(PlayerRecs() == (pw ? 4u : 0u), "a link outside the window of a pad input is not recorded");
        g_testAge = -1;
        Execute(FrameVs(frame++, 3, 52, 2));  // no input yet in this session
        Require(PlayerRecs() == (pw ? 4u : 0u), "a link with no pad input at all is not recorded");
        // The ledger: in play the frames that linked programs are counted; leaving the stage logs its totals
        Require(g_fu.progs >= 10 && g_fu.frames >= 3, "the first-use ledger counted the frames that linked programs in play");
        // A new stage is loaded (the screen closed): the player list is queued right after the stage's own records, before the menu list;
        // simulate "programs gone" (a new process: nothing linked) by marking them unlinked
        scene = 0; quiet = 1; loading = 0x52; g_testAge = -1;
        for (unsigned i = 60; i < 64; ++i) g_programs.at((3u << 16) | i).ok = false;
        StageBuild(0x52);
        size_t ownApart = 0;
        std::vector<unsigned> order;
        for (const VariantCmd& v : g_stageQueue) if (v.vsId == 3 && v.fsId >= 60 && v.fsId < 64) order.push_back(v.fsId);
        if (pw) {
            Require(order == (std::vector<unsigned>{60, 61, 62, 63}), "the 4 player-action variants are in the stage 0x52's queue, in their recorded order");
            const bool hold = !std::getenv("GXV_PLAYERWARM_HOLD") || std::atoi(std::getenv("GXV_PLAYERWARM_HOLD")) != 0;
            Require(g_stageOwnLeft.load() == (hold ? 4u : 0u) && g_stageOwnOpen.size() == (hold ? 4u : 0u),
                    hold ? "GXV_PLAYERWARM_HOLD: their 4 unlinked programs count for the hold" : "GXV_PLAYERWARM_HOLD=0: they do not count for the hold");
            Require(Has("first-use: stage 00000052: player-action list: 4 draw variants queued after its own records (4 of their programs not linked yet"),
                    "the queue's player list is logged");
        } else {
            Require(order.empty(), "PLAYERWARM=0: the queue has no player-action variants");
        }
        (void)ownApart;
        for (unsigned i = 60; i < 64; ++i) g_programs.at((3u << 16) | i).ok = true;
        // The list is in the file like a stage's and reads back (card and package alike)
        StageFlush();
        g_stageSets.clear();
        g_stageRecs = 0;
        g_stageLoaded = false;
        g_stageFileRecs = 0;
        StageLoad();
        Require(PlayerRecs() == (pw ? 4u : 0u), "the player list reads back from the stage file");
        std::printf("  PLAYERWARM %s: recorded after a pad input only, queued after the own records, counted for the hold, persisted\n", pw ? "on" : "off");
        scene = savedScene; loading = savedLoading; quiet = savedQuiet; g_testAge = -1;
    }
    // (firstuse24 r2) the player list kept promptly (GXV_PLAYERFILE), the title's file select is not play
    {
        const bool pw = !std::getenv("GXV_PLAYERWARM") || std::atoi(std::getenv("GXV_PLAYERWARM")) != 0;
        const bool pf = pw && (!std::getenv("GXV_PLAYERFILE") || std::atoi(std::getenv("GXV_PLAYERFILE")) != 0);
        const auto Has = [&](const char* text) { for (const auto& l : lines) if (l.find(text) != std::string::npos) return true; return false; };
        const auto PlayerRecs = [&]() -> size_t { const auto it = g_stageSets.find(kPlayerKey); return it == g_stageSets.end() ? 0 : it->second.recs.size(); };
        const unsigned savedScene = scene, savedLoading = loading, savedQuiet = quiet;
        const std::string ply = PlayerFilePath();
        if (pf) {
            Require(ply == std::string(path) + ".ply", "the player file is next to the stage file");
            // format: write / read back, a cut file and a wrong checksum are ignored, a complete temporary file is read first
            std::vector<StageFileRec> w, r;
            for (uint32_t i = 0; i < 5; ++i) w.push_back(StageFileRec{kPlayerKey, 0x16400u + i, 0x1000u + i, 0x2000u + i});
            Require(PlyWrite(ply, w) && PlyRead(ply, r) && r.size() == 5 && r[4].lb == 0x16404u, "player file: written whole, read back");
            Require(FileSize(ply.c_str()) == sizeof(PlyHead) + 5 * sizeof(StageFileRec) && FileSize((ply + ".tmp").c_str()) == 0, "player file: header + 5 records, the temporary file renamed away");
            {
                FILE* f = std::fopen(ply.c_str(), "r+b");
                std::fseek(f, -3, SEEK_END);
                std::fputc(0x55, f);
                std::fclose(f);
            }
            Require(!PlyRead(ply, r) && r.empty(), "player file: a changed byte fails the checksum, ignored");
            Require(PlyWrite(ply, w) && ::truncate(ply.c_str(), static_cast<off_t>(FileSize(ply.c_str()) - 7)) == 0 && !PlyRead(ply, r), "player file: cut short, ignored");
            // a write cut between the remove and the rename leaves the complete temporary file only: it is the one read
            std::vector<StageFileRec> w2(w.begin(), w.begin() + 3);
            Require(PlyWrite(ply + ".tmp", w2), "temporary file written"); // (a valid file under the temporary name: what the cut write leaves)
            std::remove(ply.c_str());
            std::remove(path); // (the stage file's own copy of the player records is out of this part: only the player file is read)
            g_stageSets.clear(); g_stageRecs = 0; g_stageLoaded = false; g_stageFileRecs = 0;
            lines.clear();
            StageLoad();
            Require(PlayerRecs() == 3 && Has("3 records read from") && Has("a temporary file of a write that was cut short"), "a complete temporary file is read at the first frame");
            std::remove((ply + ".tmp").c_str());
            std::remove(ply.c_str());
            // the kill: a child records a program, polls like the frames do, and is killed 600 ms after (the delay is 200 ms): the record is in the file
            std::remove(path);
            g_stageSets.clear(); g_stageRecs = 0; g_stageLoaded = false; g_stageFileRecs = 0;
            g_plyDirty = false;
            Require(!g_plyThread, "no writer thread yet in this process (a child of a fork has none)");
            const pid_t pid = fork();
            if (pid == 0) {
                StageAdd(kPlayerKey, StageRec{0xAAAA0001ull, 0xBBBB0002ull, 0x16400u});
                PlayerFileMark();
                for (;;) { PlayerFilePoll(NowMs()); usleep(10000); }
            }
            Require(pid > 0, "fork");
            usleep(600000);
            kill(pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
            Require(WIFSIGNALED(st), "the child was killed");
            StageLoad();
            Require(PlayerRecs() == 1 && g_stageSets[kPlayerKey].recs[0].vs == 0xAAAA0001ull && g_stageSets[kPlayerKey].recs[0].fs == 0xBBBB0002ull,
                    "a kill after the write window keeps the record (read back in the next 'boot')");
            std::printf("  PLAYERFILE: atomic write, checksum, temporary-file fallback, record survives SIGKILL after the %ld ms window\n", PlayerFileDelayMs());
            // a kill before the window loses it (the delay batches a burst): documented, not required
            std::remove(ply.c_str());
        } else {
            Require(!Has("player list:") || !Has("written to"), "GXV_PLAYERFILE=0: nothing written");
        }
        // (the built-in player seed, GXV_PLAYERSEED, is gone since seedrun31: the old seed is PLYR data that tools/seedrun/stagetags.py legacy puts in the
        // packaged list; tools/test_c182_firstvisit.sh and tests/c182_firstvisit_backend.cpp test it. fix31 removed this stale case and the dead switch.)
        // the title's file select is not play: nothing recorded, nothing in the ledger
        if (pw) {
            uint32_t fsKey = 2166136261u;
            for (const char* c = "FileSelect"; *c; ++c) fsKey = (fsKey ^ static_cast<unsigned char>(*c)) * 16777619u;
            g_stageSets.clear(); g_stageRecs = 0;
            scene = fsKey; loading = fsKey; quiet = 0; g_testAge = 100;
            g_serial += 3;
            loadingActive = 1; Execute(Frame(frame++, 0, 0)); loadingActive = 0;   // (harvprep31: the load's black frame, as above)
            const unsigned framesBefore = g_fu.frames;
            Execute(FrameVs(frame++, 2, 40, 2));
            Require(PlayerRecs() == 0 && g_fu.playerLinks == 0 && g_fu.frames == framesBefore, "FileSelect: links after a pad press are not a player's action and not 'in play'");
            scene = 0x55; loading = 0x55;
            g_serial += 3;
            loadingActive = 1; Execute(Frame(frame++, 0, 0)); loadingActive = 0;
            Execute(FrameVs(frame++, 2, 42, 2));
            Require(PlayerRecs() == 2, "a real stage: the same kind of links are recorded");
            std::printf("  FileSelect is not play; a real stage is\n");
        }
        scene = savedScene; loading = savedLoading; quiet = savedQuiet; g_testAge = -1;
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
    // (stall23) GXV_STUTTER_SPLIT (default 1): 4 quiet lines and 6 lines in play every 5 s; the rest split; the wait counters
    for (int i = 0; i < 6; ++i) {
        g_screenLive = false;
        g_stFrame.stageItems = 1; g_stFrame.stageMs = 150;
        StutterFrameEnd(200 + i, 160.0);
    }
    g_testWaits[0] += 2; g_testWaits[1] += 1200000; g_testWaits[2] += 1; g_testWaits[3] += 4000; g_testWaits[4] += 9000; g_testWaits[5] += 2;
    g_screenLive = true;
    g_stFrame.newShaders = 2; g_stFrame.newShaderMs = 1150; g_stFrame.presentMs = 30; g_stFrame.rungMs = 0; g_stFrame.beginMs = 5;
    g_stFrame.demandProgs = 1; g_stFrame.demandMs = 20;
    StutterFrameEnd(300, 1385.0);
    {
        unsigned quietLines = 0;
        bool playLine = false;
        for (const auto& l : lines) {
            if (l.rfind("stutter: frame 20", 0) == 0) ++quietLines;
            if (l.rfind("stutter: frame 300 took 1385 ms in the back end (in play)", 0) == 0 &&
                l.find("the rest 1365 ms (new shaders 2: 1150 ms, size change 0 ms, frame start 5 ms, present 30 ms, other 180 ms; pooled objects made 0)") != std::string::npos &&
                l.find("in the whole frame: shader compiler 2 runs (1200 ms), GPU waits: finish 1 (4 ms), scene starts 9 ms (2 over 1 ms), fences 0 (0 ms)") != std::string::npos)
                playLine = true;
        }
        const bool split = !std::getenv("GXV_STUTTER_SPLIT") || std::atoi(std::getenv("GXV_STUTTER_SPLIT")) != 0;
        Require(g_testWaitsOn == 1, "Init started the wait counters");
        if (split) {
            Require(quietLines == 4, "SPLIT: 4 quiet lines in the window");
            Require(playLine, "SPLIT: the frame in play still gets its line, with the rest split and the frame's waits");
        }
        std::printf("  stutter lines: %u quiet, frame in play %s\n", quietLines, playLine ? "logged" : "not logged");
    }
    g_screenLive = false;
    // GXV_STUTTER_LOG: a 5-s summary appears.
    bool summary = false;
    for (const auto& l : lines) if (l.rfind("stutter-5s:", 0) == 0 || l.rfind("stage warm-up:", 0) == 0) summary = true;
    Require(summary, "stage warm-up lines in the basic log");
    for (const auto& l : lines) std::printf("  log: %s\n", l.c_str());
    if (!std::getenv("GXV_SEEDWALK")) {   // (harvprep31) the release: no draw evidence, the release's cap
        Require(!SeedWalkOn() && StageMaxRecs() == 2048 && g_swDrawn.empty() && FileSize((std::string(path) + ".drawn").c_str()) == 0, "no GXV_SEEDWALK: 2,048 a stage, no draw evidence");
    }
    std::printf("PASS stage warm-up: record, append, evict, prelink, spare, reload, repair\n");
    std::remove(path);
    return 0;
}
