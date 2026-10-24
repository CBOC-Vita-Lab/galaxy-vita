// PS Vita host of the vgx renderer for the Galaxy port: starts vitaGL on the render thread that
// platform/src/gx/vgx_galaxy.cpp creates, and shows the renderer's picture on the display.
//
// Adapted from the Mario Strikers Charged host (vgx_host_vita.cpp, kept beside this file for
// reference): everything that was that project's runtime API (aurora_*, the record layer, frame
// statistics, the text console, thread plan) is gone. What Galaxy needs from it is here:
//
//   port_vgx_vita_start()       once, on the render thread, before anything else: vitaGL
//                               start-up with explicit pool sizes (the memory plan is in
//                               notes/vita-bringup.md), shader-compiler check, semantic bindings,
//                               shader-cache folder, the vgx back end, the present hook.
//   port_vgx_vita_frame_done()  on the render thread after every executed frame: counters.
//   present hook                the back end calls it at every display copy while the finished
//                               EFB is still bound: the EFB is blitted to the 960x544 display
//                               (the game's 16:9 picture over the whole screen; GXV_VITA_WIDE=0: unstretched) and swapped.
//   the vgxvita_gl* routing     the GL calls vitaGL needs help with (include/GLES2/gl2.h).
//
// Environment: the renderer is configured through GXV_* variables; on the Vita they come from
// ux0:data/smg/env.txt (read by vita/vgx-game/entry.cpp before anything starts). Host-only ones:
//   GXV_VITA_VGL_RAM_MIB=<n>      cap of vitaGL's main-memory pool (default 64)
//   GXV_VITA_RAM_RESERVE_MIB=<n>  main memory left to the system and sceGxm (default 40)
//   GXV_VITA_CDRAM_RESERVE_MIB=<n> video memory left to sceGxm itself (default 18)
//   GXV_VITA_PATCHER_MIB=<n>      shader-patcher pool (default 8: 8 MiB host, 4 + 4 MiB code)
//   GXV_VITA_FBO_SCENES=<1..8>    scenes per frame of a framebuffer object (default 8)
//   GXV_VITA_DISPLAY=WxH          display mode (default 960x544; 720x408, 640x368, 480x272)
//   GXV_VITA_WIDE=0|1             1 (default): the game's 16:9 picture over the whole 960x544 screen
//   GXV_VITA_PRIME=0              leave out the scissored clear that Vita3K's OpenGL renderer needs
//   GXV_VITA_MASK_GUARD=1|2       only for Vita3K: 1 its Vulkan renderer, 2 its OpenGL renderer (see vgxvita_maskGuard)
//   GXV_VITA_UPLOAD_SYNC=0        do not wait for the GPU transfer unit after compressed uploads
//   GXV_TEXBUDGET=<MiB>           texture-cache budget (default 32)
//   GXV_NO_SHADERLIST=1           do not use the shader warm-up list
//   PETARI_SCRIPT_ANCHOR_DRAWS=<n> (test harness) port_vgx_vita_rich_frame() reports the game frame of the first
//                                 picture with at least n draw calls (the title screen has ~1000, a loading screen < 10)
//   GXV_VITA_SEED=app0            vitaGL reads a shader cache packed in the VPK in place;  =off: the packed cache is not used
#if defined(__vita__)

#define VGX_VITA_NO_GL_ROUTING
#include "port/diagnostics.h"
#include <GLES2/gl2.h>

#include <psp2/display.h>
#include <pthread.h>
#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "loading_screen.h"
#include "vgx_display_rect.h"
#include "vgx_shadergen.h"
#include "vgx_coldtrace.h"
#include "port/vgx_coldtrace_control.h"
#include "vgx_state.h"
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
#include "vgx_shader_pack.h"
#include "vgx_shader_reader.h"
#include <new>
#endif

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void vglSetNextSceneDepthModes(GLboolean load, GLboolean store) __attribute__((weak)); // vitaGL built with DEPTH_HINT only
extern "C" uint32_t vglGetMaskSkips(void) __attribute__((weak)); // vitaGL built with MASK_SKIP: full-target mask quads left out so far
extern "C" unsigned long long port_game_frames(void); // vgx_galaxy.cpp: frames the game has finished
extern "C" int port_boot_title_is_visible(void); // perf.cpp: the title screen has appeared (decomp/.../TitleSequenceProduct.cpp)
extern "C" void vglSetAnisotropicFilter(uint32_t sceFilter) __attribute__((weak)); // vitaGL with ANISO_SWITCH (GXV_ANISO)
// The title's data folder (also named in vita/vgx-game/entry.cpp and the notes).
#ifndef GXV_VITA_DATA_DEVICE
#define GXV_VITA_DATA_DEVICE "ux0:data/smg"
#endif
#ifndef GXV_COMMIT
#define GXV_COMMIT "unknown"
#endif
// What decides the compiled form of a shader besides its GLSL text (which is vitaGL's own key
// inside the folder): the vitaGL build (its translator, serialisation, options) and the varying
// semantics fixed in StartGl. The build script passes the first as GXV_VGL_ID; a new key makes a
// new package compile its shaders again once, which is safe. Bump kHostShaderAbi when the
// semantic bindings in StartGl change.
#ifndef GXV_VGL_ID
#define GXV_VGL_ID "vitaGL cdbba42 SHADER_CACHE SHARK_LOG LOG_ERRORS STORE_DEPTH_STENCIL SKIP_SPLASHSCREEN STRICT_DRAW_COMPLIANCE ring8MiB"
#endif

namespace {

// The display mode vitaGL is started with. 960x544 is the panel; GXV_VITA_DISPLAY=WxH takes one of
// the console's smaller modes (720x408, 640x368, 480x272: the display scales them up), which a
// software-rendered emulator needs to get anywhere (the copy to the display and the emulator's
// own copy to its window are its two largest fills).
int kScreenW = 960, kScreenH = 544;
bool g_guardAlways = false; // GXV_VITA_MASK_GUARD_ALWAYS (see vgxvita_maskGuard)
constexpr const char* kShaderCacheDir = GXV_VITA_DATA_DEVICE "/shadercache";
constexpr const char* kShaderListFile = GXV_VITA_DATA_DEVICE "/shadercache/vgx_shaderlist.bin";
constexpr const char* kHostShaderAbi = "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1";
constexpr const char* kSeedDir = "app0:shadercache";
// "v<SHADER_CACHE_MAGIC>" of the vitaGL linked in: vitaGL keeps its compiled shaders in <cache>/v<magic>. Upstream raised the
// magic from 2 to 3 (7635e3c, 16 Oct 2026: the bucket folders are made when a shader is written; the files are unchanged) and our
// libraries follow upstream since thp50, but this said "v2" until 23 Oct: the package's seed and the card's cache went unused and
// every shader was compiled again. The build can pass GXV_VGL_CACHE_DIR; StartGl checks it against vitaGL's own path and logs a
// mismatch; MigrateOldShaderCaches moves the files of an older version folder into this one.
#ifndef GXV_VGL_CACHE_DIR
#define GXV_VGL_CACHE_DIR "v3"
#endif
constexpr const char* kVglCacheVersionDir = GXV_VGL_CACHE_DIR;
extern "C" char vgl_shader_cache_path[256] __attribute__((weak)); // (vitaGL vgl.c, set in vglInit*)
// Decoded guest textures the renderer keeps (RGBA8 bytes). Galaxy's castle frame references
// ~6.5 MiB of distinct images (desktop statistics); a budget of 32 MiB keeps the whole working
// set of a scene plus its neighbours and is what the Strikers port ran with.
constexpr uint64_t kTextureBudget = 32ull * 1024 * 1024;
// vitaGL's collector thread: default priority, on core 2 (audio and I/O are there too); the
// render thread is on core 1 and the guest threads keep core 0.
constexpr int kGcPriority = 0x10000100;
constexpr int kCoreGame = 0x10000, kCoreRender = 0x20000, kCoreAux = 0x40000; // SCE_KERNEL_CPU_MASK_USER_0/1/2

bool g_scissorOn = false;
void ParkScissor();
void UnparkScissor();

std::atomic<bool> g_stretch{false};
uint64_t g_presented = 0;      // render thread only
uint64_t g_lastPresentUs = 0;
vgx::backend::Stats g_lastStats;
uint64_t g_frames = 0, g_fpsT0 = 0, g_fpsFrames = 0;
std::atomic<unsigned long long> g_richFrame{0}; // game frame of the first picture with >= g_richDraws draw calls (0: none yet)
unsigned g_richDraws = 0;

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }
unsigned NowMs() { return static_cast<unsigned>(NowUs() / 1000u); }

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void LogMemory(const char* when) {
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    port_log("[vgx-vita] t=%u ms memory %s: free main %d KiB, cdram %d KiB, phycont %d KiB", NowMs(), when, free.size_user / 1024, free.size_cdram / 1024,
             free.size_phycont / 1024);
}

void LogPools(const char* when) {
    port_log("[vgx-vita] vitaGL pools %s (free/total KiB): vram %u/%u, ram %u/%u, phycont %u/%u, external %u/%u", when,
             static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) / 1024),
             static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) / 1024),
             static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_PHYCONT) / 1024),
             static_cast<unsigned>(vglMemFree(VGL_MEM_EXTERNAL) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_EXTERNAL) / 1024));
}

void PinCurrentThread(int mask) {
    const int res = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    if (res < 0) port_log("[vgx-vita] could not pin the render thread to core mask 0x%X (0x%X)", static_cast<unsigned>(mask), static_cast<unsigned>(res));
}

void EnvLong(const char* name, long& value, long lo, long hi) {
    if (const char* e = std::getenv(name); e && *e) value = std::atol(e);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
}

// ---- shader-compiler self-test: a trivial GLSL pair through GLSL -> Cg -> libshacccg -> link.
// If this fails nothing the renderer generates can work, and the reason is in the returned text.
bool ShaderCompilerSelfTest(char* error, size_t errorSize) {
    static const char* vs = "attribute vec4 a_pos;\nvarying vec2 v_uv;\nvoid main() { v_uv = a_pos.xy; gl_Position = a_pos; }\n";
    static const char* fs = "precision mediump float;\nvarying vec2 v_uv;\nuniform vec4 u_add;\nvoid main() { gl_FragColor = vec4(v_uv, 0.0, 1.0) + u_add; }\n";
    const uint64_t t0 = NowUs();
    char log[1024] = "";
    GLuint shaders[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        // (through the host's entry points, like the back end's shaders: they make the cache's
        // bucket folder for this text if it is not there yet, see vgxvita_glShaderSource)
        shaders[i] = vgxvita_glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* text = i == 0 ? vs : fs;
        vgxvita_glShaderSource(shaders[i], 1, &text, nullptr);
        glCompileShader(shaders[i]);
        GLint ok = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            std::snprintf(error, errorSize, "the %s test shader did not compile:\n%s", i == 0 ? "vertex" : "fragment", log);
            return false;
        }
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, shaders[0]);
    glAttachShader(program, shaders[1]);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::snprintf(error, errorSize, "the test program did not link:\n%s", log);
        return false;
    }
    port_log("[vgx-vita] shader compiler self-test passed in %u ms (u_add at location %d)", static_cast<unsigned>((NowUs() - t0) / 1000u),
             static_cast<int>(glGetUniformLocation(program, "u_add")));
    glDeleteProgram(program);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    return true;
}

void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h);

// =============================================================================================
// The loading screen (loading_screen.h draws it). This part knows what the start-up is doing.
//
// From the first moments of the process until the title screen has appeared the display shows a
// space scene with a progress bar. A thread of its own (core 2, low priority) draws the picture into
// memory about 30 times a second; who puts it on the display depends on how far the start-up is:
//   kDirect  no graphics library yet (the shader copy, vitaGL's own start-up take seconds): the thread
//            writes it to a block of video memory and tells the display to scan it;
//   kGl      from the moment the renderer is up: the render thread draws the latest picture and swaps
//            (at every present the game makes, at every progress call of the shader warm-up and, when
//            nothing happens, every 20 ms from the wait of the render loop), so the game's own black
//            first pictures never reach the display;
//   kDone    the picture has faded out after the title appeared; the game's pictures take over.
// The bar follows the stages of the start-up, weighted by how long each took on the last start
// (ux0:data/smg/loadtimes.txt); a stage that knows its progress (unpacking the shaders, the warm-up
// list) reports it, the others advance with time.
//   GXV_VITA_SPLASH=0   no loading screen
namespace splash {

enum Stage { kStart, kUnpack, kGraphics, kGame, kShaders, kTitle, kStageCount };
enum Phase { kOff = 0, kDirect, kGl, kDone };

const char* const kCaption[kStageCount] = {"Starting up", "Unpacking shaders", "Starting the graphics", "Loading the game", "Preparing shaders",
                                           "Almost there"};
// Seconds the stages took on the first measured console start; a start replaces them with its own.
const double kDefaultSeconds[kStageCount] = {0.7, 30.0, 1.9, 1.0, 4.5, 5.5};
constexpr const char* kTimesFile = GXV_VITA_DATA_DEVICE "/loadtimes.txt";

std::atomic<int> g_phase{kOff};
std::atomic<int> g_stage{kStart};
std::atomic<uint64_t> g_stageStartUs{0};
std::atomic<int> g_stagePermille{-1};      // progress inside the stage when it reports it (0..1000), else -1
std::atomic<bool> g_unpackNeeded{false};
std::atomic<int> g_unpackDone{0}, g_unpackTotal{0};
std::atomic<int> g_taskDone{0}, g_taskTotal{0};  // a job of the game's own (moving its files on the card), whatever the stage
std::atomic<const char*> g_taskLabel{""};
std::atomic<bool> g_titleSeen{false};
std::atomic<uint64_t> g_titleSeenUs{0};
std::atomic<bool> g_faded{false};          // the picture has gone dark
std::atomic<bool> g_directBusy{false};
std::atomic<bool> g_failed{false};
char g_failText[200] = "";
double g_expect[kStageCount];
uint64_t g_durUs[kStageCount] = {};
uint64_t g_beginUs = 0;
SceUID g_thread = -1;
uint32_t* g_pic[2] = {nullptr, nullptr};   // the finished picture is g_pic[g_front]
int g_front = 0;
std::mutex g_picLock;                      // guards g_front and the reading of the finished picture
std::atomic<uint32_t> g_frameId{0};
uint32_t g_uploadedId = 0;                 // render thread only
int g_blackPresents = 0;
uint64_t g_freeBlockAtUs = 0;
uint64_t g_lastPresentUs = 0;              // render thread only
unsigned g_frames = 0;                     // ticker only, for the closing line
uint64_t g_renderUsSum = 0, g_renderUsWorst = 0;

inline bool Active() { return g_phase.load(std::memory_order_acquire) == kGl; }

void LoadTimes() {
    for (int i = 0; i < kStageCount; ++i) g_expect[i] = kDefaultSeconds[i];
    const SceUID f = sceIoOpen(kTimesFile, SCE_O_RDONLY, 0);
    if (f < 0) return;
    char text[160];
    const int n = sceIoRead(f, text, sizeof(text) - 1);
    sceIoClose(f);
    if (n <= 0) return;
    text[n] = 0;
    unsigned ms[kStageCount] = {};
    if (std::sscanf(text, "SMGL1 %u %u %u %u %u %u", &ms[0], &ms[1], &ms[2], &ms[3], &ms[4], &ms[5]) != kStageCount) return;
    for (int i = 0; i < kStageCount; ++i)
        if (ms[i] >= 50 && ms[i] <= 300000) g_expect[i] = ms[i] / 1000.0;
}
void SaveTimes() {
    char text[160];
    int len = std::snprintf(text, sizeof(text), "SMGL1");
    for (int i = 0; i < kStageCount && len > 0 && len < static_cast<int>(sizeof(text)) - 12; ++i) {
        const unsigned ms = g_durUs[i] ? static_cast<unsigned>(g_durUs[i] / 1000u) : static_cast<unsigned>(g_expect[i] * 1000.0);
        len += std::snprintf(text + len, sizeof(text) - len, " %u", ms);
    }
    const SceUID f = sceIoOpen(kTimesFile, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f < 0) return;
    sceIoWrite(f, text, static_cast<SceSize>(std::strlen(text)));
    sceIoClose(f);
}

// A later stage begins (never an earlier one: a stage that was skipped stays at zero).
void Enter(Stage s) {
    if (g_phase.load() == kOff || g_phase.load() == kDone) return;
    const int cur = g_stage.load();
    if (static_cast<int>(s) <= cur) return;
    const uint64_t now = NowUs();
    g_durUs[cur] = now - g_stageStartUs.load();
    g_stagePermille.store(-1);
    g_stageStartUs.store(now);
    g_stage.store(s);
}
void UnpackBegin(int total) {
    if (g_phase.load() == kOff) return;
    g_unpackTotal.store(total);
    g_unpackDone.store(0);
    g_unpackNeeded.store(true);
    Enter(kUnpack);
}
void UnpackProgress(int done, int total) {
    g_unpackDone.store(done);
    g_stagePermille.store(total > 0 ? static_cast<int>(static_cast<int64_t>(done) * 1000 / total) : 0);
}
// total <= 0: the job is over.  `label` is a string literal; `counts`: the caption says "x of y".
std::atomic<bool> g_taskCounts{true};
void TaskProgress(const char* label, int done, int total, bool counts = true) {
    if (g_phase.load() == kOff) return;
    if (total <= 0) {
        g_taskTotal.store(0);
        g_stagePermille.store(-1);
        return;
    }
    g_taskLabel.store(label);
    g_taskCounts.store(counts);
    g_taskDone.store(done);
    g_taskTotal.store(total);
    g_stagePermille.store(static_cast<int>(static_cast<int64_t>(done) * 1000 / total));
}
bool g_heldForShaders = false;
bool g_warmAfterTitle = false;  // the warm-up's slices have not begun: they wait for the title
void NoteTitleVisible(const char* how) {
    if (g_phase.load() == kOff || g_titleSeen.load()) return;
    // The shader warm-up goes on behind the loading of the game (SetShaderWarmupDeferred); the screen stays up until it is done.
    const unsigned left = vgx::backend::WarmPending(), all = vgx::backend::WarmTotal();
    // The warm-up's compiles read the shader cache from the card, the same card the game loads from, so it is still a third done
    // when the title is up, and holding the screen for it cost 5 s of a 26 s start. By default the title is shown and the rest of
    // the warm-up goes on behind it at GXV_WARM_TITLE_MS (default 10) ms a frame; GXV_WARM_HOLD=1 holds the screen as before.
    static const bool hold = std::getenv("GXV_WARM_HOLD") && std::getenv("GXV_WARM_HOLD")[0] == '1';
    if (left > 0 && all > 0) {
        if (hold) {
            if (!g_heldForShaders) port_log("[vgx-vita] t=%u ms the title is up, but the shader warm-up has %u of %u units left: the loading screen waits for it", NowMs(), left, all);
            g_heldForShaders = true;
            TaskProgress("Preparing shaders", static_cast<int>(all - left), static_cast<int>(all), false);
            return;
        }
        static bool goesOn = false;
        if (!goesOn) {
            goesOn = true;
            long titleMs = 10;
            EnvLong("GXV_WARM_TITLE_MS", titleMs, 1, 1000);
            vgx::backend::SetWarmBudget(static_cast<double>(titleMs));
            port_log("[vgx-vita] t=%u ms the title is up with %u of %u units of the shader warm-up left: it %s behind the title, %ld ms a frame", NowMs(), left, all,
                     g_warmAfterTitle ? "begins" : "goes on", titleMs);
        }
    }
    if (g_heldForShaders) {
        g_heldForShaders = false;
        TaskProgress("", 0, 0);
    }
    if (g_titleSeen.exchange(true)) return;
    g_titleSeenUs.store(NowUs());
    port_log("[vgx-vita] t=%u ms loading screen: the title has appeared (%s), the screen fades out", NowMs(), how);
}
void Fail(const char* what) {
    if (g_phase.load() == kOff) return;
    std::snprintf(g_failText, sizeof(g_failText), "%s", what);
    g_failed.store(true);
}

// The bar: the stages before the current one count in full, the current one by its own report or by
// time (it slows towards 99 % of its stage rather than stopping), weighted by the expected seconds.
float Progress(uint64_t now, float last) {
    const int stage = g_stage.load();
    double w[kStageCount], total = 0.0, before = 0.0;
    for (int i = 0; i < kStageCount; ++i) {
        w[i] = (i == kUnpack && !g_unpackNeeded.load()) ? 0.0 : (g_expect[i] > 0.05 ? g_expect[i] : 0.05);
        total += w[i];
        if (i < stage) before += w[i];
    }
    double frac;
    const int permille = g_stagePermille.load();
    if (permille >= 0) {
        frac = permille / 1000.0;
    } else {
        const double x = (now - g_stageStartUs.load()) / 1e6 / (w[stage] > 0.0 ? w[stage] : 1.0);
        frac = x < 0.9 ? x : 0.9 + 0.09 * (1.0 - std::exp(-(x - 0.9) * 3.0));
    }
    if (frac > 0.995) frac = 0.995;
    float p = static_cast<float>((before + w[stage] * frac) / total);
    if (p > 0.995f) p = 0.995f;
    return p > last ? p : last;
}

int TickerMain(SceSize, void*) {
    loading::Init();
    const uint64_t t0 = NowUs();
    float progress = 0.0f, progressAtTitle = 0.0f;
    char caption[128], footer[200];
    for (;;) {
        const uint64_t f0 = NowUs();
        if (g_phase.load() == kDone) break;
        if (!g_titleSeen.load() && port_boot_title_is_visible()) NoteTitleVisible("title logo");
        loading::State st;
        const int stage = g_stage.load();
        st.tip = static_cast<int>((f0 - t0) / 4500000u);
        if (g_failed.load()) {
            st.caption = "The game could not start";
            std::snprintf(footer, sizeof(footer), "%.150s", g_failText);
            st.footer = footer;
            st.alert = true;
            progress = Progress(f0, progress);
        } else if (g_taskTotal.load() > 0) {
            if (g_taskCounts.load()) {
                std::snprintf(caption, sizeof(caption), "%s: %d of %d", g_taskLabel.load(), g_taskDone.load(), g_taskTotal.load());
                st.footer = "This only happens once: the game's files are put into smaller folders so that they open quickly.";
            } else {
                std::snprintf(caption, sizeof(caption), "%s", g_taskLabel.load());
            }
            st.caption = caption;
            progress = Progress(f0, progress);
        } else if (stage == kUnpack && g_unpackTotal.load() > 0) {
            std::snprintf(caption, sizeof(caption), "%s: %d of %d", kCaption[stage], g_unpackDone.load(), g_unpackTotal.load());
            st.caption = caption;
            st.footer = "This only happens on the first start after an update; the next starts are much shorter.";
            progress = Progress(f0, progress);
        } else {
            st.caption = kCaption[stage];
            progress = Progress(f0, progress);
        }
        if (g_titleSeen.load()) {
            const double since = (f0 - g_titleSeenUs.load()) / 1e6;
            if (progressAtTitle == 0.0f) progressAtTitle = progress;
            const float k = since >= 0.20 ? 1.0f : static_cast<float>(since / 0.20);
            progress = progressAtTitle + (1.0f - progressAtTitle) * k;
            st.caption = "Ready";
            const double fade = (since - 0.20) / 0.30;
            st.fade = fade < 0.0 ? 0.0f : (fade > 1.0 ? 1.0f : static_cast<float>(fade));
            if (st.fade >= 1.0f) g_faded.store(true);
        }
        st.progress = progress;
        const int back = 1 - g_front;
        const uint64_t r0 = NowUs();
        loading::Render(g_pic[back], (f0 - t0) / 1e6, st);
        const uint64_t spent = NowUs() - r0;
        g_renderUsSum += spent;
        if (spent > g_renderUsWorst) g_renderUsWorst = spent;
        ++g_frames;
        {
            std::lock_guard<std::mutex> lk(g_picLock);
            g_front = back;
        }
        g_frameId.fetch_add(1);
        if (g_phase.load() == kDirect) {
            g_directBusy.store(true);
            if (g_phase.load() == kDirect) loading::direct::Show(g_pic[back]);
            g_directBusy.store(false);
        }
        // 30 pictures a second while it goes straight to the display; once the renderer shows it every picture costs the
        // render thread about 35 ms (a 2 MiB texture copied into video memory and a swap), and at the start of the game that
        // thread has other things to do (the shader warm-up above all: the first console run with a picture every 33 ms made
        // it take 15 s instead of 6): 7 a second, 4 during the warm-up, 30 again for the fade at the end.
        uint32_t periodUs = 33000u;
        if (g_phase.load() == kGl && !g_titleSeen.load()) periodUs = g_stage.load() == kShaders ? 250000u : 150000u;
        const uint64_t used = NowUs() - f0;
        sceKernelDelayThread(used < periodUs ? static_cast<SceUInt>(periodUs - used) : 2000u);
    }
    return 0;
}

// Render thread: puts the latest finished picture on the display (a swap) when it is a new one.
uint64_t g_upUs = 0, g_drawUs = 0, g_swapUs = 0, g_presented = 0;  // (render thread only)
bool PresentFrame(bool force) {
    const uint64_t a = NowUs();
    {
        std::lock_guard<std::mutex> lk(g_picLock);
        const uint32_t id = g_frameId.load();
        if (id == g_uploadedId && !force) return false;
        if (!vgx::backend::SetPicture(g_pic[g_front], loading::W, loading::H)) return false;
        g_uploadedId = id;
    }
    const uint64_t b = NowUs();
    if (!vgx::backend::DrawPicture()) return false;
    const uint64_t c = NowUs();
    vglSwapBuffers(GL_FALSE);
    const uint64_t d = NowUs();
    g_upUs += b - a;
    g_drawUs += c - b;
    g_swapUs += d - c;
    ++g_presented;
    return true;
}

void Finish() {
    g_phase.store(kDone);
    if (g_thread >= 0) {
        SceUInt timeout = 2000000u;
        int status = 0;
        sceKernelWaitThreadEnd(g_thread, &status, &timeout);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    vgx::backend::SetProgressHostDraws(false);
    vgx::backend::ReleasePicture();
    if (g_freeBlockAtUs || loading::direct::g_base) loading::direct::Close();
    g_freeBlockAtUs = 0;
    for (uint32_t*& p : g_pic) { free(p); p = nullptr; }
    loading::Release();
    const uint64_t title = g_titleSeenUs.load();
    g_durUs[kTitle] = title > g_stageStartUs.load() ? title - g_stageStartUs.load() : 0;
    if (g_stage.load() != kTitle) g_durUs[kTitle] = 0;  // (the stages are not complete; keep the old figures)
    if (g_stage.load() == kTitle && !g_failed.load()) SaveTimes();
    if (g_presented)
        port_log("[vgx-vita] loading screen on the renderer: %llu pictures shown, per picture %.1f ms copying it in, %.1f ms drawing, %.1f ms swapping",
                 static_cast<unsigned long long>(g_presented), g_upUs / 1000.0 / g_presented, g_drawUs / 1000.0 / g_presented, g_swapUs / 1000.0 / g_presented);
    port_log("[vgx-vita] t=%u ms loading screen finished: %u pictures, drawing %.1f ms on average (worst %.1f ms); stages (ms): start %u, unpack %u, graphics %u, game %u, shaders %u, title %u",
             NowMs(), g_frames, g_frames ? g_renderUsSum / 1000.0 / g_frames : 0.0, g_renderUsWorst / 1000.0, static_cast<unsigned>(g_durUs[kStart] / 1000u),
             static_cast<unsigned>(g_durUs[kUnpack] / 1000u), static_cast<unsigned>(g_durUs[kGraphics] / 1000u), static_cast<unsigned>(g_durUs[kGame] / 1000u),
             static_cast<unsigned>(g_durUs[kShaders] / 1000u), static_cast<unsigned>(g_durUs[kTitle] / 1000u));
}

// Render thread, wherever it is at a moment when the display can be refreshed. `minGapUs`: not sooner than that after the last
// picture (the shader warm-up asks for 80 ms: showing a picture costs the render thread about 10 ms, and the warm-up is its work).
void Service(uint64_t minGapUs = 0) {
    if (!Active()) return;
    const uint64_t now = NowUs();
    if (minGapUs && now - g_lastPresentUs < minGapUs) return;
    const bool shown = PresentFrame(false);
    if (shown) g_lastPresentUs = now;
    if (g_freeBlockAtUs && NowUs() >= g_freeBlockAtUs) {
        loading::direct::Close();
        g_freeBlockAtUs = 0;
    }
    if (shown && g_faded.load() && ++g_blackPresents >= 3) Finish();  // (every buffer of the swap chain has been shown dark)
}

} // namespace splash

// ux0:data/smg/shadercache/<8 hex digits>: the folder vitaGL keeps compiled shaders in. A
// package whose key differs starts with an empty folder (the old one is left behind).
const char* ShaderCacheFolder() {
    static char path[160] = "";
    if (path[0]) return path;
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", GXV_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* c = id; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    std::snprintf(path, sizeof(path), "%s/%08x", kShaderCacheDir, static_cast<unsigned>(h));
    port_log("[vgx-vita] compiled-shader cache: %s (key of: %s)", path, id);
    return path;
}

#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
// The renderer can still be alive during static teardown. Leave the immutable
// cache to process reclamation instead of freeing bytes a late compile can read.
alignas(vgx::vita::ShaderPack) unsigned char g_shaderPackStorage[sizeof(vgx::vita::ShaderPack)];
vgx::vita::ShaderPack& g_shaderPack = *::new (g_shaderPackStorage) vgx::vita::ShaderPack;
void ShaderPackIdentity(char* out, size_t size) {
    std::snprintf(out, size, "%s|%s|gen %u", GXV_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
}
uint64_t ShaderPackAbi() {
    char identity[512];
    ShaderPackIdentity(identity, sizeof identity);
    return XXH3_64bits(identity, std::strlen(identity));
}
void PrepareShaderPack(const char* folder) {
    static bool prepared = false;
    if (prepared) return;
    prepared = true;
    port_vgx_shader_cache_reader_driver_v1();
    long enabled = 0;
    EnvLong("GXV_SHADERPACK", enabled, 0, 1);
    if (!enabled) return;
    char root[256], pack[256];
    const int r = std::snprintf(root, sizeof root, "%s/%s", folder, kVglCacheVersionDir);
    const int p = std::snprintf(pack, sizeof pack, "%s/stages.pack", folder);
    if (r < 0 || p < 0 || r >= static_cast<int>(sizeof root) || p >= static_cast<int>(sizeof pack)) return;
    const uint64_t start = (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? NowUs() : 0;
    const bool loaded = g_shaderPack.LoadFile(pack, root, ShaderPackAbi());
    port_log("[vgx-vita] shader RAM pack: %s, %u records, %u bytes, %u ms preload, ABI %016llx (ordinary files remain fallback)",
        loaded ? "loaded" : "unavailable", static_cast<unsigned>(g_shaderPack.Entries()), static_cast<unsigned>(g_shaderPack.Bytes()),
        static_cast<unsigned>((PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? (NowUs() - start) / 1000u : 0),
        static_cast<unsigned long long>(ShaderPackAbi()));
}
#endif

// The back end's progress hook: a bar has been drawn into the display's back buffer in the
// middle of a frame while shaders compile; show it.
void ShowProgressBar(uint32_t done, uint32_t total) {
    static uint32_t shown = 0;
    if (shown++ == 0) port_log("[vgx-vita] shaders are being compiled: progress bar on screen (%u of %u units)", done, total);
    if (splash::Active()) {  // (the loading screen shows the warm-up as one of its stages)
        splash::Enter(splash::kShaders);
        splash::g_stagePermille.store(total ? static_cast<int>(static_cast<uint64_t>(done < total ? done : total) * 1000u / total) : 0);
        splash::Service(80000u);
        return;
    }
    vglSwapBuffers(GL_FALSE);
}

// ---- a shader cache that came with the package (app0:shadercache/<key>/v<n>/{v,f}/<HH>/<hash>.gxp
// and vgx_shaderlist.bin), copied once into the writable cache folder. The seed is only found by
// a package whose <key> (above) is the same.
constexpr unsigned kSeedMaxFiles = 4096;
constexpr size_t kSeedMaxListBytes = 4u * 1024u * 1024u;
constexpr uint32_t kIoNotFound = 0x80010002u;
uint8_t g_seedBuffer[64u * 1024u]; // startup owner only; shared by fingerprinting and the later copy
enum class SeedRead { Ok, Missing, Error, TooLarge };
SeedRead ReadSeedBytes(const char* path, std::vector<uint8_t>& bytes, size_t limit) {
    bytes.clear();
    SceIoStat st{};
    const int result = sceIoGetstat(path, &st);
    if (result < 0) return static_cast<uint32_t>(result) == kIoNotFound ? SeedRead::Missing : SeedRead::Error;
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > limit) return SeedRead::TooLarge;
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return SeedRead::Error;
    bytes.resize(static_cast<size_t>(st.st_size));
    size_t offset = 0;
    bool ok = true;
    while (offset < bytes.size()) {
        const int n = sceIoRead(f, bytes.data() + offset, static_cast<SceSize>(bytes.size() - offset));
        if (n <= 0 || static_cast<size_t>(n) > bytes.size() - offset) { ok = false; break; }
        offset += static_cast<size_t>(n);
    }
    uint8_t extra;
    if (ok && sceIoRead(f, &extra, 1) != 0) ok = false;
    const bool closed = sceIoClose(f) == 0;
    return ok && closed ? SeedRead::Ok : SeedRead::Error;
}
bool WriteSeedBytes(const char* path, const std::vector<uint8_t>& bytes) {
    const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f < 0) return false;
    const bool written = bytes.empty() || sceIoWrite(f, bytes.data(), static_cast<SceSize>(bytes.size())) == static_cast<int>(bytes.size());
    const bool closed = sceIoClose(f) == 0;
    return written && closed;
}
struct SeedCopyStats {
    unsigned files = 0, bytes = 0, failed = 0, folders = 0, kept = 0;
    uint64_t folderUs = 0, fileUs = 0;
    unsigned total = 0; // files the copy will go through (the loading screen's count; 0: not counted)
};
bool CopyFile(const char* from, const char* to, bool mayExist, SeedCopyStats& st, int64_t expected = -1) {
    if (expected < 0) {
        SceIoStat info{};
        if (sceIoGetstat(from, &info) < 0) return false;
        expected = info.st_size;
    }
    if (expected <= 0) return false;
    if (mayExist) {
        SceIoStat info{};
        const int result = sceIoGetstat(to, &info);
        if (result >= 0 && info.st_size > 0) { ++st.kept; return true; }
        if (result < 0 && static_cast<uint32_t>(result) != kIoNotFound) return false;
    }
    auto& buffer = g_seedBuffer;
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); return false; }
    bool ok = true;
    uint64_t total = 0;
    for (;;) {
        const int n = sceIoRead(in, buffer, sizeof(buffer));
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (static_cast<uint64_t>(n) > static_cast<uint64_t>(expected) - total) { ok = false; break; }
        if (sceIoWrite(out, buffer, static_cast<SceSize>(n)) != n) { ok = false; break; }
        st.bytes += static_cast<unsigned>(n);
        total += static_cast<unsigned>(n);
    }
    const bool closedIn = sceIoClose(in) == 0;
    const bool closedOut = sceIoClose(out) == 0;
    ok = ok && total == static_cast<uint64_t>(expected) && closedIn && closedOut;
    if (!ok) sceIoRemove(to); // never leave half a program behind: vitaGL would load it
    return ok;
}
void CopyTree(const char* from, const char* to, int depth, bool fresh, SeedCopyStats& st) {
    const SceUID dir = sceIoDopen(from);
    if (dir < 0) { ++st.failed; return; }
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        const int next = sceIoDread(dir, &entry);
        if (next <= 0) { if (next < 0) ++st.failed; break; }
        if (entry.d_name[0] == '.' || std::strcmp(entry.d_name, "seeded") == 0) continue;
        if (st.files + st.failed >= kSeedMaxFiles) { ++st.failed; break; }
        // (a package's seed is laid out in the folder of the vitaGL it was made with, v<magic>: copied into the folder of the
        // vitaGL linked in, so that no folder has to be moved on the card afterwards - a few hundred renames there take a minute)
        const char* name = entry.d_name;
        if (depth == 0 && SCE_S_ISDIR(entry.d_stat.st_mode) && name[0] == 'v' && name[1] >= '0' && name[1] <= '9' &&
            std::strspn(name + 1, "0123456789") == std::strlen(name + 1))
            name = kVglCacheVersionDir;
        char a[320], b[320];
        if (std::snprintf(a, sizeof(a), "%s/%s", from, entry.d_name) >= static_cast<int>(sizeof(a)) ||
            std::snprintf(b, sizeof(b), "%s/%s", to, name) >= static_cast<int>(sizeof(b)))
            { ++st.failed; continue; }
        const uint64_t t0 = NowUs();
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            if (depth >= 3) { ++st.failed; continue; }
            const bool made = sceIoMkdir(b, 0777) >= 0;
            if (made) ++st.folders;
            st.folderUs += NowUs() - t0;
            CopyTree(a, b, depth + 1, made, st);
        } else {
            if (CopyFile(a, b, !fresh, st, entry.d_stat.st_size)) ++st.files; else ++st.failed;
            st.fileUs += NowUs() - t0;
            if (st.total) splash::UnpackProgress(static_cast<int>(st.files + st.failed), static_cast<int>(st.total));
        }
    }
    if (sceIoDclose(dir) < 0) ++st.failed;
}
// How many files CopyTree will go through under `from` (it stops at the same depth and name rules).
unsigned CountTree(const char* from, int depth) {
    const SceUID dir = sceIoDopen(from);
    if (dir < 0) return 0;
    unsigned n = 0;
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        if (sceIoDread(dir, &entry) <= 0 || n >= kSeedMaxFiles) break;
        if (entry.d_name[0] == '.' || std::strcmp(entry.d_name, "seeded") == 0) continue;
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            if (depth >= 3) continue;
            char a[320];
            if (std::snprintf(a, sizeof(a), "%s/%s", from, entry.d_name) < static_cast<int>(sizeof(a))) n += CountTree(a, depth + 1);
        } else {
            ++n;
        }
    }
    sceIoDclose(dir);
    return n;
}
// <cache>/v2/v/00 (and f/00): with the first of them present vitaGL does not create its 512
// bucket folders at start-up; a missing bucket is made only when vitaGL writes a cache miss.
void MakeFirstBuckets(const char* folder, SeedCopyStats& st) {
    static const char* const kParts[] = {"", "/v", "/v/00", "/f", "/f/00"};
    const uint64_t t0 = NowUs();
    for (const char* part : kParts) {
        char path[320];
        if (std::snprintf(path, sizeof(path), "%s/%s%s", folder, kVglCacheVersionDir, part) >= static_cast<int>(sizeof(path))) return;
        if (sceIoMkdir(path, 0777) >= 0) ++st.folders;
    }
    st.folderUs += NowUs() - t0;
}
// Fingerprint the list once at startup; equal-size changes must not conceal newly packaged GXPs.
bool PackageStamp(char* out, size_t n, uint64_t* listSize = nullptr) {
    if (!out || !n) return false;
    out[0] = 0;
    SceIoStat st{};
    char list[220];
    std::snprintf(list, sizeof(list), "%s/vgx_shaderlist.bin", kSeedDir);
    if (sceIoGetstat(list, &st) < 0 || st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > kSeedMaxListBytes) return false;
    const SceUID f = sceIoOpen(list, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    XXH64_state_t hash{};
    XXH64_reset(&hash, 0);
    auto& buffer = g_seedBuffer;
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        const int got = sceIoRead(f, buffer, sizeof buffer);
        if (got < 0) { ok = false; break; }
        if (!got) break;
        if (static_cast<uint64_t>(got) > static_cast<uint64_t>(st.st_size) - total) { ok = false; break; }
        XXH64_update(&hash, buffer, static_cast<size_t>(got));
        total += static_cast<unsigned>(got);
    }
    const bool closed = sceIoClose(f) == 0;
    if (!ok || !closed || total != static_cast<uint64_t>(st.st_size)) return false;
    const int count = std::snprintf(out, n, "list2 %llu %016llx", static_cast<unsigned long long>(total),
                                    static_cast<unsigned long long>(XXH64_digest(&hash)));
    if (count < 0 || static_cast<size_t>(count) >= n) return false;
    if (listSize) *listSize = total;
    return true;
}
struct SeedMarker { bool matches = false, readError = false, replaceList = false; };
SeedMarker CheckSeedMarker(const char* marker, const char* stamp, uint64_t size) {
    std::vector<uint8_t> bytes;
    const SeedRead result = ReadSeedBytes(marker, bytes, 80);
    if (result != SeedRead::Ok) return {false, result == SeedRead::Error, false};
    const std::string text(bytes.begin(), bytes.end());
    if (text == stamp) return {true, false, false};
    unsigned long long oldSize = 0, digest = 0;
    int used = 0;
    const bool fingerprint = std::sscanf(text.c_str(), "list2 %llu %16llx%n", &oldSize, &digest, &used) == 2 &&
                             used == static_cast<int>(text.size()) && oldSize <= kSeedMaxListBytes;
    used = 0;
    const bool legacy = std::sscanf(text.c_str(), "list %llu%n", &oldSize, &used) == 1 &&
                        used == static_cast<int>(text.size()) && oldSize <= kSeedMaxListBytes;
    // Legacy size matches still require one GXP validation pass, but not a learned-list reset.
    return {false, false, fingerprint || (legacy && oldSize != size)};
}
// The compiled shaders of an older vitaGL version folder (<cache>/v1, v2: the same files, only the folder name changed) are moved
// into the one in use (`cur`). On the memory card every rename and directory operation is slow (the first console run of thp52
// spent tens of seconds on the loading screen renaming ~850 files one by one), so whole folders are moved wherever possible:
// the version folder in one rename when the new one does not exist; else each stage folder (v, f) in one rename when the new
// one is missing or empty (vitaGL makes them empty at start-up); else each bucket folder; only a bucket both sides have is
// merged file by file (a file the new folder already has is a duplicate and is removed). A time budget of 1.5 s stops a
// file-by-file merge; what is left is moved at the next start (vitaGL compiles a missing shader meanwhile). Emptied folders are
// removed. Runs before vitaGL starts. Nothing outside <cache>/v1|v2 is touched.
bool DirEmpty(const char* path) {
    const SceUID d = sceIoDopen(path);
    if (d < 0) return true;
    SceIoDirent e{};
    bool empty = true;
    while (empty && sceIoDread(d, &e) > 0) empty = e.d_name[0] == '.';
    sceIoDclose(d);
    return empty;
}
std::vector<std::string> ListDir(const char* path, bool dirs) {
    std::vector<std::string> out;
    const SceUID d = sceIoDopen(path);
    if (d < 0) return out;
    SceIoDirent e{};
    while (sceIoDread(d, &e) > 0)
        if (e.d_name[0] != '.' && (SCE_S_ISDIR(e.d_stat.st_mode) != 0) == dirs) out.emplace_back(e.d_name);
    sceIoDclose(d);
    return out;
}
void MigrateOldShaderCaches(const char* folder, const char* cur) {
    static const char* const kOld[] = {"v1", "v2"};
    for (const char* old : kOld) {
        if (std::strcmp(old, cur) == 0) continue;
        char from[300], to[300];
        if (std::snprintf(from, sizeof from, "%s/%s", folder, old) >= static_cast<int>(sizeof from)) continue;
        if (std::snprintf(to, sizeof to, "%s/%s", folder, cur) >= static_cast<int>(sizeof to)) continue;
        if (!Exists(from)) continue;
        const uint64_t t0 = NowUs();
        if (!Exists(to)) {
            const int r = sceIoRename(from, to);
            port_log("[vgx-vita] compiled-shader cache: %s renamed to %s (%s)", from, to, r >= 0 ? "done" : "FAILED, files are compiled again");
            continue;
        }
        unsigned stages = 0, buckets = 0, moved = 0, dup = 0, failed = 0;
        bool outOfTime = false;
        for (const char* stage : {"v", "f"}) {
            char sfrom[320], sto[320];
            std::snprintf(sfrom, sizeof sfrom, "%s/%s", from, stage);
            std::snprintf(sto, sizeof sto, "%s/%s", to, stage);
            if (!Exists(sfrom)) continue;
            if (!Exists(sto) || DirEmpty(sto)) {
                if (Exists(sto)) sceIoRmdir(sto);
                if (sceIoRename(sfrom, sto) >= 0) { ++stages; continue; }
                sceIoMkdir(sto, 0777);
            }
            for (const std::string& bucket : ListDir(sfrom, true)) {
                if (outOfTime || NowUs() - t0 > 1500000u) { outOfTime = true; break; } // (a bucket rename costs 0.1-0.3 s on the card)
                char bfrom[360], bto[360];
                if (std::snprintf(bfrom, sizeof bfrom, "%s/%s", sfrom, bucket.c_str()) >= static_cast<int>(sizeof bfrom)) continue;
                if (std::snprintf(bto, sizeof bto, "%s/%s", sto, bucket.c_str()) >= static_cast<int>(sizeof bto)) continue;
                if (!Exists(bto)) {
                    if (sceIoRename(bfrom, bto) >= 0) { ++buckets; continue; }
                    sceIoMkdir(bto, 0777);
                }
                for (const std::string& f : ListDir(bfrom, false)) {
                    if (NowUs() - t0 > 1500000u) { outOfTime = true; break; }
                    char ffrom[420], fto[420];
                    if (std::snprintf(ffrom, sizeof ffrom, "%s/%s", bfrom, f.c_str()) >= static_cast<int>(sizeof ffrom)) continue;
                    if (std::snprintf(fto, sizeof fto, "%s/%s", bto, f.c_str()) >= static_cast<int>(sizeof fto)) continue;
                    if (Exists(fto)) { if (sceIoRemove(ffrom) >= 0) ++dup; else ++failed; continue; }
                    if (sceIoRename(ffrom, fto) >= 0) ++moved; else ++failed;
                }
                sceIoRmdir(bfrom); // (fails harmlessly if something is left)
            }
            if (!outOfTime) sceIoRmdir(sfrom);
        }
        const bool gone = sceIoRmdir(from) >= 0;
        port_log("[vgx-vita] compiled-shader cache: %s into %s: %u stage folders and %u bucket folders moved whole, %u files moved, %u duplicates "
                 "removed, %u failed%s; old folder %s; %u ms", from, to, stages, buckets, moved, dup, failed, outOfTime ? ", time budget used up (the rest next start)" : "",
                 gone ? "removed" : "kept", static_cast<unsigned>((NowUs() - t0) / 1000u));
    }
}
bool g_seedIsNew = false;
char g_seedPendingStamp[80] = {}, g_seedPendingMarker[200] = {};
// Returns the folder vitaGL is to use as its cache.
const char* SeedShaderCache(const char* folder) {
    g_seedIsNew = false;
    g_seedPendingStamp[0] = g_seedPendingMarker[0] = 0;
    const char* key = std::strrchr(folder, '/');
    static char seed[200];
    char marker[200];
    std::snprintf(seed, sizeof(seed), "%s%s", kSeedDir, key ? key : "/none");
    std::snprintf(marker, sizeof(marker), "%s/seeded", folder);
    SeedCopyStats st;
    if (!Exists(seed)) {
        MakeFirstBuckets(folder, st);
        port_log("[vgx-vita] no compiled shaders in the package for this key (%s): they are compiled on this console as needed", seed);
        return folder;
    }
    if (const char* e = std::getenv("GXV_VITA_SEED"); e && std::strcmp(e, "off") == 0) {
        MakeFirstBuckets(folder, st);
        port_log("[vgx-vita] GXV_VITA_SEED=off: the package's compiled shaders (%s) are not used", seed);
        return folder;
    }
    if (const char* e = std::getenv("GXV_VITA_SEED"); e && std::strcmp(e, "app0") == 0) {
        port_log("[vgx-vita] GXV_VITA_SEED=app0: vitaGL reads the package's compiled shaders in place (%s)", seed);
        return seed;
    }
    char stamp[80];
    uint64_t listSize = 0;
    const bool stamped = PackageStamp(stamp, sizeof(stamp), &listSize);
    const SeedMarker previous = stamped ? CheckSeedMarker(marker, stamp, listSize) : SeedMarker{};
    if (previous.matches) return folder;
    const uint64_t t0 = NowUs();
    st.total = CountTree(seed, 0);
    splash::UnpackBegin(static_cast<int>(st.total));
    CopyTree(seed, folder, 0, false, st);
    MakeFirstBuckets(folder, st);
    port_log("[vgx-vita] compiled shaders from the package copied to %s: %u files (%u KiB), %u already there, %u failed; %u folders; %u ms in all", folder,
             st.files - st.kept, st.bytes / 1024u, st.kept, st.failed, st.folders, static_cast<unsigned>((NowUs() - t0) / 1000u));
    if (st.failed == 0 && stamped && !previous.readError) {
        if (previous.replaceList && !std::getenv("GXV_SHADERLIST") && !std::getenv("GXV_NO_SHADERLIST")) {
            // Keep the old fingerprint until list adoption succeeds, including across process restarts.
            std::snprintf(g_seedPendingStamp, sizeof g_seedPendingStamp, "%s", stamp);
            std::snprintf(g_seedPendingMarker, sizeof g_seedPendingMarker, "%s", marker);
            g_seedIsNew = true;
        } else {
            const std::vector<uint8_t> bytes(stamp, stamp + std::strlen(stamp));
            if (!WriteSeedBytes(marker, bytes)) sceIoRemove(marker);
        }
    }
    return folder;
}
void SeedShaderList() {
    char seed[200];
    std::snprintf(seed, sizeof(seed), "%s/vgx_shaderlist.bin", kSeedDir);
    if (const char* e = std::getenv("GXV_VITA_SEED"); e && std::strcmp(e, "off") == 0) return;
    // The card's own list is kept unless the package is new to this card: then the package's
    // list (made from a tour of the game) replaces it. Shaders only the card's list knew are
    // still in the compiled-shader cache and are added to the list again when they are met.
    SceIoStat info{};
    const int status = sceIoGetstat(kShaderListFile, &info);
    if (!g_seedIsNew && (status >= 0 || static_cast<uint32_t>(status) != kIoNotFound)) return;
    std::vector<uint8_t> packaged, old;
    bool ok = ReadSeedBytes(seed, packaged, kSeedMaxListBytes) == SeedRead::Ok && !packaged.empty();
    const SeedRead previous = ok ? ReadSeedBytes(kShaderListFile, old, kSeedMaxListBytes) : SeedRead::Error;
    // An oversized personal list is kept; committing the cache avoids an unbounded retry walk.
    const bool keepLarge = g_seedIsNew && previous == SeedRead::TooLarge;
    ok = ok && (previous == SeedRead::Ok || previous == SeedRead::Missing || keepLarge);
    if (ok && !keepLarge && !WriteSeedBytes(kShaderListFile, packaged)) {
        ok = false;
        if (previous == SeedRead::Ok) {
            if (!WriteSeedBytes(kShaderListFile, old)) port_log("[vgx-vita] warm-up list restore failed; cache learning will recover a prefix");
        } else sceIoRemove(kShaderListFile);
    }
    if (g_seedIsNew && ok) {
        const std::vector<uint8_t> bytes(g_seedPendingStamp, g_seedPendingStamp + std::strlen(g_seedPendingStamp));
        if (!WriteSeedBytes(g_seedPendingMarker, bytes)) sceIoRemove(g_seedPendingMarker);
    }
    g_seedIsNew = false;
    g_seedPendingStamp[0] = g_seedPendingMarker[0] = 0;
    port_log("[vgx-vita] warm-up list from the package: %s (%u bytes)", ok ? "copied" : "copy FAILED", static_cast<unsigned>(packaged.size()));
}

// sceGxm's display-queue thread and vitaGL's collector are created inside the libraries with
// affinity "any". They are found by name in the process's thread ids and moved: the display
// queue next to the render thread, the collector to the auxiliary core (so neither takes time
// from the game thread's core). Threads can appear late, so this runs again at a few presents.
// (Vita3K does not implement sceKernelGetThreadmgrUIDClass: there the scan sees nothing.)
void PinLibraryThreads(bool log, bool census = false) {
    SceKernelThreadInfo info;
    int missesInRow = 0, moved = 0, seen = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        // (the thread census: threads made by the C++ library start at the lowest user priority, 0xBF)
        if (PORT_RUNTIME_DIAGNOSTICS && census) port_log("[vgx-vita] thread census: \"%s\" priority 0x%X (made with 0x%X), core mask 0x%X (made with 0x%X), ran %llu us", info.name,
                             static_cast<unsigned>(info.currentPriority), static_cast<unsigned>(info.initPriority), static_cast<unsigned>(info.currentCpuAffinityMask),
                             static_cast<unsigned>(info.initCpuAffinityMask), static_cast<unsigned long long>(info.runClocks));
        int target = 0;
        if (std::strstr(info.name, "Garbage Collector")) target = kCoreAux;
        else if (!std::strcmp(info.name, "SceGxmDisplayQueue")) target = kCoreRender;
        if (target && info.currentCpuAffinityMask != static_cast<SceUInt32>(target)) {
            const int res = sceKernelChangeThreadCpuAffinityMask(uid, target);
            ++moved;
            port_log("[vgx-vita] thread \"%s\" (priority 0x%X) moved to core mask 0x%X: %s", info.name, static_cast<unsigned>(info.currentPriority),
                     static_cast<unsigned>(target), res >= 0 ? "ok" : "refused");
        }
    }
    if (log) port_log("[vgx-vita] library threads: %d threads seen, %d moved%s", seen, moved, seen == 0 ? " (none seen: expected in the emulator)" : "");
}

// Who used the CPU since the last call: the threads that ran for at least 40 ms, with their core masks, and the sum for each core.
// The game thread's turn takes longer by the clock than it computes (the Observatory on 18 Oct: 16.6 ms a turn, 12.8 ms of CPU); this
// says who else is on its core (mask 0x10000) while it waits. Called every 10 s by the status block in vita/vgx-game/entry.cpp.
void ThreadCpuReport() {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    struct Seen { SceUID uid; unsigned long long run; };
    static Seen before[160];
    static int beforeCount = 0;
    static uint64_t lastUs = 0;
    Seen after[160];
    int afterCount = 0;
    struct Row { SceUID uid; char name[32]; unsigned prio, mask; unsigned long long ms; };
    Row rows[160];
    int rowCount = 0;
    unsigned long long core[4] = {0, 0, 0, 0}; // core 0, 1, 2, "any or another"
    const uint64_t nowUs = NowUs();
    SceKernelThreadInfo info;
    int missesInRow = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500 && afterCount < 160; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        const unsigned long long run = static_cast<unsigned long long>(info.runClocks);
        after[afterCount++] = {uid, run};
        unsigned long long was = 0;
        for (int i = 0; i < beforeCount; ++i) {
            if (before[i].uid == uid) { was = before[i].run; break; }
        }
        if (lastUs == 0 || run < was) continue; // (the first call only takes the starting values)
        const unsigned long long ms = (run - was) / 1000ull;
        const unsigned mask = static_cast<unsigned>(info.currentCpuAffinityMask);
        core[mask == 0x10000u ? 0 : mask == 0x20000u ? 1 : mask == 0x40000u ? 2 : 3] += ms;
        if (ms >= 40 && rowCount < 160) {
            Row& r = rows[rowCount++];
            r.uid = uid;
            std::snprintf(r.name, sizeof(r.name), "%s", info.name);
            r.prio = static_cast<unsigned>(info.currentPriority);
            r.mask = mask;
            r.ms = ms;
        }
    }
    if (lastUs != 0 && afterCount > 0) {
        port_log("threads: CPU used in the last %.1f s (ms): core 0 %llu, core 1 %llu, core 2 %llu, any core %llu", (nowUs - lastUs) / 1e6, core[0], core[1], core[2], core[3]);
        for (int i = 0; i < rowCount; ++i) {
            port_log("threads:   \"%s\" id %08X priority 0x%X core mask 0x%X: %llu ms", rows[i].name, static_cast<unsigned>(rows[i].uid), rows[i].prio, rows[i].mask, rows[i].ms);
        }
    }
    std::memcpy(before, after, sizeof(Seen) * static_cast<size_t>(afterCount));
    beforeCount = afterCount;
    lastUs = nowUs;
}

// GXV_VITA_PRIME=0 leaves out the scissored clear after the first swap (see StartGl).
bool PrimeEnabled() {
    const char* e = std::getenv("GXV_VITA_PRIME");
    return !(e && *e && std::atoi(e) == 0);
}

// What the renderer's front end and the shader cache need from the host, which is all file work and settings with no GL in it.
// It runs on the game's thread before the render thread is started (port_vgx_vita_prepare), so that the thread that starts vitaGL
// (2.3 s) can do so while the game boots, and the front end finds its warm-up list and texture budget set when the first draw comes.
const char* g_shaderCachePath = "";
bool g_prepared = false;
void PrepareFrontEnd() {
    if (g_prepared) return;
    g_prepared = true;
    sceIoMkdir(GXV_VITA_DATA_DEVICE, 0777);
    sceIoMkdir(kShaderCacheDir, 0777);
    sceIoMkdir(ShaderCacheFolder(), 0777);
    g_shaderCachePath = SeedShaderCache(ShaderCacheFolder());
    // (before vitaGL starts: it then finds the files where it looks, and makes no empty stage folders in the way)
    if (g_shaderCachePath == ShaderCacheFolder()) MigrateOldShaderCaches(g_shaderCachePath, kVglCacheVersionDir);
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
    // port_gx_select_backend completes this before EnsureThread/pthread_create.
    // StartGl's fallback runs on the sole GL owner before backend initialization.
    PrepareShaderPack(g_shaderCachePath);
#endif
    // Settings a console needs, unless env.txt chose otherwise (the renderer reads GXV_TEXBUDGET
    // and GXV_SHADERLIST itself).
    if (!std::getenv("GXV_SHADERLIST") && !std::getenv("GXV_NO_SHADERLIST")) {
        SeedShaderList();
        vgx::SetShaderListFile(kShaderListFile);
    }
    if (!std::getenv("GXV_TEXBUDGET")) vgx::SetTextureBudget(kTextureBudget);
    {
        // The warm-up list's 7 s of compiling and linking used to hold up the game's first frame, and with it everything the game does
        // after it (the loading of the title above all); now they are done in slices behind it (GXV_WARM_DEFER=0: all in the first frame).
        // The warm-up's compiles read the shader cache from the card, which the game is loading from at the same time: while they run
        // together the game's loading takes up to twice as long (console: 22 s against 12 s). So by default the warm-up waits for the
        // title (afterTitle: its slices begin when NoteTitleVisible sees the title) and the loading screen goes away as soon as the
        // title is up; GXV_WARM_AFTER_TITLE=0 starts the slices with the first frames, as before.
        long defer = 1, budgetMs = 25, afterTitle = (std::getenv("GXV_WARM_HOLD") && std::getenv("GXV_WARM_HOLD")[0] == '1') ? 0 : 1;
        EnvLong("GXV_WARM_DEFER", defer, 0, 1);
        EnvLong("GXV_WARM_MS", budgetMs, 1, 1000);
        EnvLong("GXV_WARM_AFTER_TITLE", afterTitle, 0, 1);
        if (defer && splash::g_phase.load() != splash::kOff && !std::getenv("GXV_NO_SHADERLIST")) {
            vgx::SetShaderWarmupDeferred(true);
            splash::g_warmAfterTitle = afterTitle != 0;
            vgx::backend::SetWarmBudget(afterTitle ? 0.0 : static_cast<double>(budgetMs));
            if (afterTitle) port_log("[vgx-vita] the shader warm-up waits for the title, then goes on behind it (GXV_WARM_AFTER_TITLE=0: slices of %ld ms behind the game's first frames)", budgetMs);
            else port_log("[vgx-vita] the shader warm-up is done in slices of %ld ms behind the game's first frames (GXV_WARM_DEFER=0: all in the first frame)", budgetMs);
        }
    }
    port_log("[vgx-vita] shader warm-up list: %s (%s); texture budget: %s",
             std::getenv("GXV_NO_SHADERLIST") ? "off (GXV_NO_SHADERLIST)" : (std::getenv("GXV_SHADERLIST") ? std::getenv("GXV_SHADERLIST") : kShaderListFile),
             Exists(kShaderListFile) ? "found: all known shaders are compiled during the first frame" : "not there yet: shaders are compiled as the game needs them",
             std::getenv("GXV_TEXBUDGET") ? "GXV_TEXBUDGET (MiB)" : "32 MiB");

}

bool StartGl(char* error, size_t errorSize) {
    // The shader compiler module is the user's own copy of a system file; all of the renderer's
    // shaders are compiled with it on first use.
    const bool compilerA = Exists("ur0:data/libshacccg.suprx"), compilerB = Exists("ur0:data/external/libshacccg.suprx");
    port_log("[vgx-vita] shader compiler module: ur0:data/libshacccg.suprx %s, ur0:data/external/libshacccg.suprx %s", compilerA ? "present" : "absent",
             compilerB ? "present" : "absent");
    if (!compilerA && !compilerB) {
        std::snprintf(error, errorSize, "The shader compiler module (libshacccg.suprx) is missing from ur0:data/.");
        return false;
    }

    LogMemory("before vitaGL (game arenas allocated)");
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    // vitaGL's general pool (its rings and uniform ring, vertex and index data, everything it
    // allocates "for the CPU"): free main memory minus what is left to the system and sceGxm's
    // own buffers, capped. The plan and its reasons are in notes/vita-bringup.md (memory table).
    long ramLimitMib = 64, ramReserveMib = 40;
    EnvLong("GXV_VITA_VGL_RAM_MIB", ramLimitMib, 16, 192); // (vitaGL caps an uncached pool at 200 MiB)
    EnvLong("GXV_VITA_RAM_RESERVE_MIB", ramReserveMib, 4, 200);
    int64_t ram = static_cast<int64_t>(free.size_user) - ramReserveMib * 1024 * 1024;
    if (ram > ramLimitMib * 1024 * 1024) ram = ramLimitMib * 1024 * 1024;
    if (ram < 16ll * 1024 * 1024) ram = 16ll * 1024 * 1024;
    // Video memory: what is free MINUS what sceGxm takes for itself when vitaGL initialises it
    // (16 MiB parameter buffer + 2). On a console the first run of the Strikers port died
    // because the pool request ate the parameter buffer's share. Vita3K does not limit memory.
    long cdramReserveMib = 18;
    EnvLong("GXV_VITA_CDRAM_RESERVE_MIB", cdramReserveMib, 1, 100);
    int64_t cdram = static_cast<int64_t>(free.size_cdram) - cdramReserveMib * 1024 * 1024;
    cdram &= ~static_cast<int64_t>(256 * 1024 - 1);
    if (cdram < 0) cdram = 0;
    const int64_t phycont = free.size_phycont > 0 ? free.size_phycont : 0;
    port_log("[vgx-vita] vitaGL pools requested: ram %u KiB, cdram %u KiB, phycont %u KiB", static_cast<unsigned>(ram / 1024),
             static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(phycont / 1024));

    // Varyings get fixed semantics: vgx compiles vertex and fragment shaders independently and
    // links them in many combinations, so the translator's name-order assignment cannot be used.
    vglSetSemanticBindingMode(VGL_MODE_GLOBAL);
    static const char* const kTex[8] = {"v_tex0", "v_tex1", "v_tex2", "v_tex3", "v_tex4", "v_tex5", "v_tex6", "v_tex7"};
    for (int i = 0; i < 8; ++i) vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>(kTex[i]), i, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col0"), 8, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col1"), 9, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_uv"), 0, VGL_TYPE_TEXCOORD);

    // sceGxm sizes a render target for a number of scenes per frame (1 to 8); the renderer
    // returns to the EFB after every EFB copy (a castle frame has 13), so its framebuffer
    // objects get the maximum.
    long fboScenes = 8;
    EnvLong("GXV_VITA_FBO_SCENES", fboScenes, 1, 8);
    // The display: one scene per swap in play (PresentDisplayCopy: the blit of the finished EFB, then vglSwapBuffers; the
    // final composite and the HUD are drawn into the EFB, not here), and one per swap on the loading screen (PresentFrame)
    // and with the shader progress bar outside it (ShowProgress, then ShowProgressBar's swap). The one case with two
    // scenes before a swap: the progress bar drawn while the loading screen is up and the loading screen then has no new
    // picture to show (ShowProgressBar -> splash::Service): the bar's scene is left without a swap and the next scene on
    // the display waits for it (a scene of a quad or two, at start-up only). So 1 is right (gpupass, 23 Oct).
    // GXV_VITA_DISPLAY_SCENES=<1..8> changes it (for A/B on the console).
    long displayScenes = 1;
    EnvLong("GXV_VITA_DISPLAY_SCENES", displayScenes, 1, 8);
    vglSetupRenderTargetScenesNum(static_cast<uint8_t>(displayScenes), static_cast<uint8_t>(fboScenes));

    // The shader patcher holds every linked program and every patched variant of one (blend
    // state, vertex layout). One scene of Galaxy has 130-160 programs; the warm-up list a
    // package is seeded with (a tour of all 44 galaxies) has 205 vertex and 640 fragment
    // shaders in 736 programs, all made in the first frame: 8 MiB of host memory and 4 MiB
    // each of vertex and fragment code (vitaGL's own default is 1 MiB each).
    long patcherMib = 8;
    EnvLong("GXV_VITA_PATCHER_MIB", patcherMib, 1, 16);
    vglSetupShaderPatcher(static_cast<unsigned>(patcherMib) * 1024u * 1024u, static_cast<unsigned>((patcherMib + 1) / 2) * 1024u * 1024u,
                          static_cast<unsigned>((patcherMib + 1) / 2) * 1024u * 1024u);
    vglSetupGarbageCollector(kGcPriority, kCoreAux);
    // sceGxm buffers, all at vitaGL's defaults unless env.txt asks: triple buffering lets a frame
    // go on while the last one waits for the display (a frame never stalls on the swap); the
    // parameter and VDM ring buffers bound how much of a dense scene can be in flight (a scene
    // that does not fit is drawn in several passes). KiB values; 0 = the default.
    long tripleBuffering = 0, paramKib = 0, vdmKib = 0;
    EnvLong("GXV_VITA_TRIPLE", tripleBuffering, 0, 1);
    EnvLong("GXV_VITA_PARAM_KIB", paramKib, 0, 65536);
    EnvLong("GXV_VITA_VDM_KIB", vdmKib, 0, 65536);
    if (tripleBuffering) vglUseTripleBuffering(GL_TRUE);
    if (paramKib) vglSetParamBufferSize(static_cast<uint32_t>(paramKib) * 1024u);
    if (vdmKib) vglSetVDMBufferSize(static_cast<uint32_t>(vdmKib) * 1024u);
    // A vitaGL built with HAVE_SCRATCH_MEMORY puts GL_DYNAMIC_DRAW and GL_STREAM_DRAW buffers in the
    // per-frame circular pool by default. The static geometry is given the DYNAMIC hint (see below) and
    // must outlive the frame, so only the stream buffers (the frame's vertex chunks and indices) may be
    // scratch. Without that build option the call does nothing.
    vglSetupScratchMemory(GL_FALSE, GL_TRUE);
    port_log("[vgx-vita] sceGxm buffers: %s buffering, parameter buffer %ld KiB, VDM ring %ld KiB (0 = vitaGL's default)",
             tripleBuffering ? "triple" : "double", paramKib, vdmKib);
    port_log("[vgx-vita] render targets: %ld scene(s) per frame; shader patcher pools %ld / %ld / %ld MiB; collector on core mask 0x%X", fboScenes, patcherMib,
             (patcherMib + 1) / 2, (patcherMib + 1) / 2, static_cast<unsigned>(kCoreAux));

    PrepareFrontEnd();  // (normally done already, on the game's thread, before this one was started)
    vglSetShaderCachePath(g_shaderCachePath);
    splash::Enter(splash::kGraphics);
    // The game paces itself to its own 60 Hz retrace clock; a second wait for vertical blank in
    // the swap would make the two beat against each other.
    vglWaitVblankStart(GL_FALSE);

    // vglInitWithCustomSizes returns GL_TRUE only when the display could not take the requested
    // size and vitaGL fell back to a smaller one (source/vgl.c: `return res_fallback`): GL_FALSE
    // is the normal result.
    if (const char* e = std::getenv("GXV_VITA_DISPLAY")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && ((w == 960 && h == 544) || (w == 720 && h == 408) || (w == 640 && h == 368) || (w == 480 && h == 272))) {
            kScreenW = w;
            kScreenH = h;
        } else {
            port_log("[vgx-vita] GXV_VITA_DISPLAY=%s is not one of 960x544, 720x408, 640x368, 480x272: ignored", e);
        }
    }
    // GXV_MSAA=0|2|4 (default 0): multisampling for EVERY render target of the session, chosen at start-up. vitaGL has one
    // multisample mode for the whole context (vgl.c msaa_mode): the display, every framebuffer object's render target and
    // colour surface (resolved on store, so colour memory does not grow), every hidden depth buffer (2 or 4 times the
    // memory), and every patched fragment program (built for that mode). It cannot be changed while running without
    // re-making all of those, so the automatic quality controller (vgx_autoq.h) works with resolution only and this
    // stays a start-up choice, to measure what MSAA costs on the console before a per-target vitaGL patch is worth it.
    long msaa = 0;
    EnvLong("GXV_MSAA", msaa, 0, 4);
    const SceGxmMultisampleMode msaaMode = msaa >= 4 ? SCE_GXM_MULTISAMPLE_4X : (msaa >= 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE);
    port_log("[vgx-vita] t=%u ms vitaGL start-up, display mode %dx%d, multisampling %s", NowMs(), kScreenW, kScreenH,
             msaaMode == SCE_GXM_MULTISAMPLE_4X ? "4x (GXV_MSAA)" : (msaaMode == SCE_GXM_MULTISAMPLE_2X ? "2x (GXV_MSAA)" : "off"));
    const GLboolean fellBack = vglInitWithCustomSizes(0, kScreenW, kScreenH, static_cast<int>(ram), static_cast<int>(cdram), static_cast<int>(phycont), 0,
                                                      msaaMode);
    if (fellBack) port_log("[vgx-vita] vitaGL could not set %dx%d and fell back to a smaller display mode", kScreenW, kScreenH);
    // GXV_ANISO=0|2|3 (default 0, off): the Strikers port's experimental anisotropic switch, in a vitaGL built with ANISO_SWITCH
    // (tools/vitagl/build-gpu-timers.sh): trilinear textures get minification filter 2 or 3, believed to be the hardware's
    // anisotropic modes. Untested on a console (also in Strikers): not part of the automatic quality.
    {
        long aniso = 0;
        EnvLong("GXV_ANISO", aniso, 0, 3);
        if (aniso >= 2) {
            if (vglSetAnisotropicFilter) {
                vglSetAnisotropicFilter(static_cast<uint32_t>(aniso));
                port_log("[vgx-vita] GXV_ANISO=%ld: trilinear textures use minification filter %ld (experimental)", aniso, aniso);
            } else {
                port_log("[vgx-vita] GXV_ANISO=%ld ignored: this vitaGL has no anisotropic switch", aniso);
            }
        }
    }
    if (vglMemTotal(VGL_MEM_VRAM) == 0 && vglMemTotal(VGL_MEM_RAM) == 0) {
        std::snprintf(error, errorSize, "vitaGL started without any memory pool (see the [vitaGL] lines in the log).");
        return false;
    }
    LogMemory("after vitaGL");
    if (&vgl_shader_cache_path != nullptr && vgl_shader_cache_path[0]) {
        const char* tail = std::strrchr(vgl_shader_cache_path, '/');
        if (!tail || std::strcmp(tail + 1, kVglCacheVersionDir) != 0)
            port_log("[vgx-vita] WARNING: vitaGL keeps compiled shaders in %s, but the package seed and the cache migration use \"%s\" "
                     "(set GXV_VGL_CACHE_DIR to this vitaGL's v<SHADER_CACHE_MAGIC>)", vgl_shader_cache_path, kVglCacheVersionDir);
    }
    {
        // A pool that vitaGL could not allocate leaves the budget untouched; say so instead of
        // crashing at its first use. (An emulator reports more video memory than any console has
        // and may not count allocations at all: no verdict there.)
        SceKernelFreeMemorySizeInfo after;
        std::memset(&after, 0, sizeof(after));
        after.size = sizeof(after);
        sceKernelGetFreeMemorySize(&after);
        const bool realBudget = free.size_cdram > 0 && free.size_cdram <= 160u * 1024u * 1024u;
        const int64_t taken = static_cast<int64_t>(free.size_cdram) - static_cast<int64_t>(after.size_cdram);
        if (realBudget && cdram > 0 && taken < cdram / 2) {
            std::snprintf(error, errorSize, "vitaGL could not allocate its video memory pool (%u KiB asked, %u KiB of video memory still free); raise GXV_VITA_CDRAM_RESERVE_MIB.",
                          static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(after.size_cdram / 1024));
            return false;
        }
    }
    LogPools("after start-up");
    port_log("[vgx-vita] GL: %s / %s / %s", reinterpret_cast<const char*>(glGetString(GL_VENDOR)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
             reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // Every buffer of the swap chain starts black (the side bars of the 4:3 picture are never
    // drawn again). The display is cleared and swapped three times; after the FIRST of these,
    // one clear with the scissor on (GXV_VITA_PRIME=0 leaves it out). The scissored clear and its
    // place are there for Vita3K's OpenGL renderer (README "Traps" 2 and 3): its mask value
    // starts at 0 and only a scissored draw sets it to 1, and it must not be the session's
    // very first draw or the emulator caches a broken clear shader. A console needs neither.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    static const bool prime = PrimeEnabled();
    if (splash::g_phase.load() == splash::kDirect) {
        // (the loading screen is on the display: the swap chain is filled with it, and later with black, by splash::Ready / Finish)
        port_log("[vgx-vita] the loading screen stays on the display until the renderer is ready");
    } else {
        for (int i = 0; i < 3; ++i) {
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_FALSE);
            if (i == 0 && prime) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(0, 0, 16, 16);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
        }
        port_log("[vgx-vita] display cleared and swapped three times (%s)", prime ? "with one scissored clear after the first" : "GXV_VITA_PRIME=0: no scissored clear");
    }
    if (const char* e = std::getenv("GXV_VITA_MASK_GUARD"); e && std::atoi(e) != 0) {
        vgxvita_maskGuardOn = std::atoi(e) == 2 ? 2 : 1;
        if (const char* a = std::getenv("GXV_VITA_MASK_GUARD_ALWAYS"); a && std::atoi(a) != 0) g_guardAlways = true;
        port_log(vgxvita_maskGuardOn == 2
                     ? "[vgx-vita] GXV_VITA_MASK_GUARD=2: every draw and clear is preceded by a mask update and the depth-write state is given again (Vita3K OpenGL only)"
                     : "[vgx-vita] GXV_VITA_MASK_GUARD=1: every draw and clear is preceded by a mask update under depth function NEVER (Vita3K Vulkan only)");
    }

    char testError[400] = "";
    if (!ShaderCompilerSelfTest(testError, sizeof(testError))) {
        std::snprintf(error, errorSize, "Run-time shader compilation does not work:\n%s", testError);
        return false;
    }
    // Static geometry goes to vitaGL's MAIN-memory pool (hint GL_DYNAMIC_DRAW): less video memory.
    setenv("GXV_STATIC_USAGE", "dynamic", 0);

    // GXV_RENDER_SIZE (read by the glue) sets the back end's picture size; here the back end
    // is initialised by vgx_galaxy.cpp after this returns.
    vgx::backend::SetPresentHook(PresentDisplayCopy);
    vgx::backend::SetProgressDisplay(0, kScreenW, kScreenH, ShowProgressBar);
    // The bar is for start-up (the warm-up list's work in the first frames) and for a long wait; a
    // shader met in the middle of play costs a moment, not a flash of the bar.
    vgx::backend::SetProgressPolicy(90, 1500);
    PinLibraryThreads(true);
    port_log("[vgx-vita] t=%u ms vitaGL ready; renderer sources: %s", NowMs(), GXV_COMMIT);
    splash::Enter(splash::kGame);
    return true;
}

// The back end's present hook: a display copy, on the render thread, with the EFB framebuffer
// object `efb` bound and the finished picture in it. (x, y, w, h) is the copy rectangle in
// framebuffer pixels; the EFB is stored top row first (its projection flips Y), so the
// picture's top row is the framebuffer's y. The back end binds the EFB again and re-applies
// its render state after this returns.
void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {
    ++g_presented;
    const uint64_t now = NowUs();
    const vgx::backend::Stats stats = vgx::backend::GetStats();
    if (g_richDraws && stats.drawCallsThisFrame >= g_richDraws && g_richFrame.load() == 0) {
        g_richFrame.store(port_game_frames() + 1);
        port_log("[vgx-vita] t=%u ms first picture with %u draw calls (>= %u): game frame %llu", NowMs(), stats.drawCallsThisFrame, g_richDraws,
                 static_cast<unsigned long long>(g_richFrame.load()));
    }
    if (g_presented == 1)
        port_log("[vgx-vita] t=%u ms FIRST PRESENT: display copy %d,%d %dx%d from framebuffer object %u", NowMs(), x, y, w, h, efb);
    // A long gap between two presents is almost always shader compilation: say so.
    if (PORT_RUNTIME_DIAGNOSTICS && g_lastPresentUs != 0 && now - g_lastPresentUs >= 100000u &&
        (stats.vertexShaders != g_lastStats.vertexShaders || stats.fragmentShaders != g_lastStats.fragmentShaders || stats.programs != g_lastStats.programs))
        port_log("[vgx-vita] present %llu came %u ms after the previous one: +%u vertex shaders, +%u fragment shaders, +%u programs, +%u textures",
                 static_cast<unsigned long long>(g_presented), static_cast<unsigned>((now - g_lastPresentUs) / 1000u), stats.vertexShaders - g_lastStats.vertexShaders,
                 stats.fragmentShaders - g_lastStats.fragmentShaders, stats.programs - g_lastStats.programs, stats.textures - g_lastStats.textures);
    if (PORT_RUNTIME_DIAGNOSTICS && (g_presented == 1 || g_presented % 300 == 0)) {
        port_log("[vgx-vita] present %llu: %u draw calls in the last buffer, %u vertex + %u fragment shaders and %u programs so far, %u textures, %u KiB of texels",
                 static_cast<unsigned long long>(g_presented), stats.drawCallsThisFrame, stats.vertexShaders, stats.fragmentShaders, stats.programs, stats.textures,
                 static_cast<unsigned>(stats.textureBytes / 1024u));
        port_log("[vgx-vita] shader work so far: %u compiles, %u links, %u ms in all, progress bar shown %u times", stats.shaderCompiles, stats.programLinks,
                 static_cast<unsigned>(stats.shaderWorkMs), stats.progressFrames);
        if (vglGetMaskSkips) port_log("[vgx-vita] vitaGL MASK_SKIP: %u full-target scissor-mask quads left out so far", vglGetMaskSkips());
        unsigned made = 0, ms = 0;
        vgxvita_cacheFolderStats(&made, &ms);
        if (made != 0) port_log("[vgx-vita] shader cache: %u bucket folders made as shaders needed them, %u ms in all", made, ms);
        if (g_presented % 600 == 0 || g_presented == 1) {
            LogPools("now");
            LogMemory("now");
        }
    }
    if (g_presented == 5 || g_presented == 30 || g_presented == 120 || g_presented % 600 == 0) PinLibraryThreads(false, g_presented == 120 || g_presented == 9000);
    g_lastPresentUs = now;
    g_lastStats = stats;

    if (splash::Active()) {
        // The loading screen is up: the game's pictures stay out of sight. (A game that never shows its title, e.g. started
        // straight into a stage by PETARI_STAGE, ends the screen after 300 pictures of real content in a row.)
        static unsigned richInARow = 0;
        richInARow = stats.drawCallsThisFrame >= 150 ? richInARow + 1 : 0;
        if (richInARow >= 300) splash::NoteTitleVisible("300 pictures with 150 or more draw calls");
        splash::Service();
        return;
    }
    if (efb == 0 || w <= 0 || h <= 0) return;
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
    const int dx0 = display.x * kScreenW / 960, dx1 = (display.x + display.width) * kScreenW / 960; // (the rectangle is in panel pixels)
    // Source rows are given bottom-up so the picture comes out upright.
    const int sy0 = y + h, sy1 = y;
    if (g_scissorOn) { glDisable(GL_SCISSOR_TEST); ParkScissor(); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (vgxvita_maskGuardOn) vgxvita_maskGuard(); // (the display's scene starts with a mask update too)
    // GXV_DEPTH_SKIP=2 (a vitaGL with DEPTH_HINT): the display's scene is a blit; it neither tests nor keeps depth.
    static const bool depthSkip = [] { long v = 2; EnvLong("GXV_DEPTH_SKIP", v, 0, 2); return v >= 2 && vglSetNextSceneDepthModes != nullptr; }();
    if (depthSkip) vglSetNextSceneDepthModes(GL_FALSE, GL_FALSE);
    // Sharpened upscale (vgx_backend_gl.cpp PresentSharpened; GXV_SHARPEN, default auto: the amount follows the upscale,
    // none at the screen's own size). A shader that does not build leaves the plain blit for good.
    static bool sharpenOk = true;
    const float sharpen = sharpenOk ? vgx::backend::SharpenAmount(w, h, dx1 - dx0, kScreenH) : 0.0f;
    bool sharpened = false;
    if (sharpen < 0.05f || !(sharpenOk = sharpened = vgx::backend::PresentSharpened(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, sharpen, true)))
        glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    // GXV_CHARAA (vgx_draw.cpp, "character edge smoothing"): the characters' and key items' edges, over what was just drawn,
    // in this same scene (nothing is drawn in a frame without them, nor on a multisampled EFB).
    vgx::backend::PresentCharaAA(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, sharpened ? sharpen : 0.0f, true);
    vglSwapBuffers(GL_FALSE);
    vgx::backend::AutoQualitySwapped(); // (automatic quality: the GPU clock times this frame's completion)
    // Back to what the back end had: it clears the EFB next.
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    if (g_scissorOn) { UnparkScissor(); glEnable(GL_SCISSOR_TEST); }
}

} // namespace

// ---------------------------------------------------------------- what vgx_galaxy.cpp calls
// (on the render thread, which has a 4 MiB stack: the shader compiler runs on it)
namespace { void StartCore3Probe(); }
extern "C" void port_vgx_vita_prepare(void) { PrepareFrontEnd(); }
extern "C" int port_vgx_vita_start(void) {
    PinCurrentThread(kCoreRender);
    if (PORT_RUNTIME_DIAGNOSTICS) StartCore3Probe();
    // The game draws for a 16:9 television (the console setting the port reports: SCGetAspectRatio),
    // anamorphically into its 640-wide EFB, so the picture belongs on the whole 960x544 screen.
    // GXV_VITA_WIDE=0 shows the EFB unstretched between side bars (everything a quarter too narrow).
    long wide = 1;
    EnvLong("GXV_VITA_WIDE", wide, 0, 1);
    g_stretch.store(wide != 0);
    if (const char* e = std::getenv("PETARI_SCRIPT_ANCHOR_DRAWS"); e && *e) g_richDraws = static_cast<unsigned>(std::atoi(e));
    port_log("[vgx-vita] t=%u ms render thread started (%s)", NowMs(), wide ? "16:9 picture on the whole 960x544 screen" : "EFB unstretched between side bars");
    {
        // (the back end and the display copy read the same variable; this line is for the test reports)
        long depthSkip = 2;
        EnvLong("GXV_DEPTH_SKIP", depthSkip, 0, 2);
        port_log("[vgx-vita] depth buffer saving (GXV_DEPTH_SKIP): mode %ld%s", vglSetNextSceneDepthModes ? depthSkip : 0L,
                 vglSetNextSceneDepthModes ? "" : " (this vitaGL has no DEPTH_HINT: off)");
    }
    char error[600] = "";
    if (!StartGl(error, sizeof(error))) {
        port_log("[vgx-vita] graphics start-up FAILED: %s", error);
        splash::Fail(error);
        return 0;
    }
    return 1;
}

// ---- the loading screen, as the rest of the program sees it
// vita/vgx-game/entry.cpp, once, first thing after the environment file and the clocks.
extern "C" void port_splash_begin(void) {
    using namespace splash;
    if (const char* e = std::getenv("GXV_VITA_SPLASH"); e && *e && std::atoi(e) == 0) {
        port_log("[vgx-vita] loading screen off (GXV_VITA_SPLASH=0)");
        return;
    }
    LoadTimes();
    g_beginUs = NowUs();
    g_stageStartUs.store(g_beginUs);
    for (uint32_t*& p : g_pic) p = static_cast<uint32_t*>(std::malloc(static_cast<size_t>(loading::W) * loading::H * 4));
    if (!g_pic[0] || !g_pic[1]) {
        for (uint32_t*& p : g_pic) { std::free(p); p = nullptr; }
        port_log("[vgx-vita] loading screen: no memory for its pictures");
        return;
    }
    const bool direct = loading::direct::Open();
    if (direct) {
        // The first thing on the screen, at once: the colour of the scene's sky (the picture follows in a moment).
        const uint32_t sky = 0xFF000000u | (30u << 16) | (8u << 8) | 6u;
        for (size_t i = 0; i < static_cast<size_t>(loading::W) * loading::H; ++i) g_pic[0][i] = sky;
        loading::direct::Show(g_pic[0]);
    }
    g_phase.store(kDirect);
    g_thread = sceKernelCreateThread("smg_loading", &TickerMain, 0x10000100, 128 * 1024, 0, kCoreAux, nullptr);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        port_log("[vgx-vita] loading screen: its thread could not start (0x%X)", static_cast<unsigned>(g_thread));
        g_phase.store(kDone);
        for (uint32_t*& p : g_pic) { std::free(p); p = nullptr; }
        loading::direct::Close();
        return;
    }
    port_log("[vgx-vita] t=%u ms loading screen up (%s); expected stage times (s): start %.1f, unpack %.1f, graphics %.1f, game %.1f, shaders %.1f, title %.1f", NowMs(),
             direct ? "on the display directly" : "no video memory block: it waits for the renderer", g_expect[0], g_expect[1], g_expect[2], g_expect[3], g_expect[4],
             g_expect[5]);
}

// vgx_galaxy.cpp's render thread: the back end exists. The loading screen moves from the direct display to the renderer's.
extern "C" void port_vgx_vita_ready(void) {
    using namespace splash;
    if (g_phase.load() != kDirect) return;
    g_phase.store(kGl, std::memory_order_release);  // (the thread stops writing the display)
    for (int i = 0; i < 100 && g_directBusy.load(); ++i) sceKernelDelayThread(1000);
    vgx::backend::SetProgressHostDraws(true);
    static const bool prime = PrimeEnabled();
    for (int i = 0; i < 3; ++i) {
        PresentFrame(true);
        if (i == 0 && prime) {  // (see StartGl: Vita3K's OpenGL renderer wants one scissored clear after the first swap)
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 16, 16);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
    }
    g_freeBlockAtUs = NowUs() + 500000u;  // (the display needs a moment to move over before the direct block can go)
    port_log("[vgx-vita] t=%u ms loading screen moved to the renderer", NowMs());
}
extern "C" int port_vgx_vita_splash_active(void) { return splash::Active() ? 1 : 0; }
extern "C" void port_vgx_vita_idle(void) { splash::Service(); }
// (the same while the shader warm-up is being worked off between frames: a picture costs the render thread about 10 ms, so not sooner than `gapUs` after the last)
extern "C" void port_vgx_vita_idle_gap(unsigned gapUs) { splash::Service(gapUs); }
// dvd.cpp: a long job at start-up (moving the game's files into bucket folders) reports its progress here.
extern "C" void port_boot_progress(const char* what, int done, int total) { splash::TaskProgress(what, done, total); }

// ---- GXV_CORE3_PROBE=<duty %, 1..100>: how much of the Vita's fourth core a helper thread would get.
// The system core (SCE_KERNEL_CPU_MASK_SYSTEM, 0x80000) is where the OS keeps its own tasks; the
// Strikers port of another author runs a "quota controlled" helper there (at most half the core)
// but publishes no figure. This thread measures what a busy-loop gets on it: iterations per
// microsecond in 20 ms slices, against the same loop on a nearly idle game core measured at
// start-up. Every 10 s one log line: the average and the worst slice (percent of the idle
// rate) and the slice count. The loop is pure ALU (no memory traffic), so it says nothing about
// bus contention, only about scheduling. Nothing else depends on this thread.
namespace {
constexpr int kCoreSystem = 0x80000; // SCE_KERNEL_CPU_MASK_SYSTEM
int g_core3Duty = 0;

// Iterations of a one-cycle loop done in `us` microseconds of wall time, as iterations per microsecond.
double SpinRate(uint32_t us) {
    const uint64_t t0 = NowUs();
    uint64_t n = 0;
    uint64_t t1;
    do {
        uint32_t i = 20000;
        __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(i) : : "cc");
        n += 20000;
        t1 = NowUs();
    } while (t1 - t0 < us);
    return static_cast<double>(n) / static_cast<double>(t1 - t0);
}

int Core3ProbeMain(SceSize, void*) {
    int res = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kCoreAux);
    double ref = 0.0;
    for (int i = 0; i < 3; ++i) { const double r = SpinRate(100000); if (r > ref) ref = r; }
    res = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kCoreSystem);
    if (res < 0) {
        port_log("[vgx-vita] core3 probe: the system core refused this thread (0x%X); nothing to measure", static_cast<unsigned>(res));
        return 0;
    }
    double idle3 = 0.0;
    for (int i = 0; i < 3; ++i) { const double r = SpinRate(100000); if (r > idle3) idle3 = r; }
    port_log("[vgx-vita] core3 probe: a nearly idle game core runs the loop at %.1f M/s, the system core at %.1f M/s (%.0f %%), duty %d %%", ref, idle3,
             ref > 0.0 ? 100.0 * idle3 / ref : 0.0, g_core3Duty);
    const uint32_t busyUs = 20000, periodUs = static_cast<uint32_t>(busyUs * 100u / static_cast<uint32_t>(g_core3Duty));
    double sum = 0.0, worst = 1e30;
    unsigned slices = 0;
    uint64_t lastLog = NowUs();
    for (;;) {
        const uint64_t t0 = NowUs();
        const double r = SpinRate(busyUs);
        const double share = ref > 0.0 ? 100.0 * r / ref : 0.0;
        sum += share;
        if (share < worst) worst = share;
        ++slices;
        const uint64_t now = NowUs();
        if (now - lastLog >= 10000000u) {
            port_log("[vgx-vita] t=%u ms core3 probe: the system core gave a busy loop %.0f %% of a free game core on average, %.0f %% in the worst of %u slices", NowMs(),
                     sum / slices, worst, slices);
            sum = 0.0; worst = 1e30; slices = 0; lastLog = now;
        }
        if (now - t0 < periodUs) sceKernelDelayThread(static_cast<SceUInt>(periodUs - (now - t0)));
    }
    return 0;
}

void StartCore3Probe() {
    const char* e = std::getenv("GXV_CORE3_PROBE");
    if (!e || !*e) return;
    g_core3Duty = std::atoi(e);
    if (g_core3Duty < 1) return;
    if (g_core3Duty > 100) g_core3Duty = 100;
    const SceUID uid = sceKernelCreateThread("smg_core3_probe", &Core3ProbeMain, 0x10000100, 64 * 1024, 0, kCoreAux, nullptr);
    if (uid < 0) { port_log("[vgx-vita] core3 probe: thread not created (0x%X)", static_cast<unsigned>(uid)); return; }
    sceKernelStartThread(uid, 0, nullptr);
}
} // namespace

// The game's frame clock: the console's video retrace, here the display's own vertical blank
// (GXV_VITA_VBLANK_CLOCK=0: the platform layer's free-running 59.94 Hz timer instead, which
// drifts against the display and drops or repeats a picture every few seconds).
extern "C" void port_vi_retrace(void);
namespace {
void* VblankMain(void*) {
    for (;;) {
        sceDisplayWaitVblankStart();
        port_vi_retrace();
    }
    return nullptr;
}
void StartVblankClock() {
    if (const char* e = std::getenv("GXV_VITA_VBLANK_CLOCK"); e && *e && std::atoi(e) == 0) return;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 10);
    const int rc = pthread_create(&thread, &attr, &VblankMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc == 0) pthread_detach(thread);
    port_log("[vgx-vita] frame clock: %s", rc == 0 ? "the display's vertical blank" : "timer (no thread for the vertical-blank clock)");
}
} // namespace

#if VGX_COLDTRACE
static_assert(PORT_VGX_COLDTRACE_CONTROL_VERSION==1 && VGX_COLDTRACE_DETAIL_VERSION==3);
extern "C" void port_vgx_coldtrace_boundary_runtime_link_guard_v3() {}
#endif
extern "C" void port_vgx_vita_frame_done(void) {
    ++g_frames;
    const uint64_t now = PORT_LOGGING ? NowUs() : 0;
    if (g_frames == 1) {
        g_fpsT0 = now;
        g_fpsFrames = 0;
        StartVblankClock();
        port_log("[vgx-vita] t=%u ms first frame executed", NowMs());
        splash::Enter(splash::kTitle);
    }
#if VGX_COLDTRACE
    port_vgx_coldtrace_render_boundary();
#endif
    if (!PORT_LOGGING) return;
    {
        static bool queued = false, finished = false;
        const unsigned all = vgx::backend::WarmTotal(), left = vgx::backend::WarmPending();
        if (all > 0 && !queued) {
            queued = true;
            port_log("[vgx-vita] t=%u ms deferred shader warm-up queued: %u units", NowMs(), all);
        }
        if (queued && left == 0 && !finished) {
            finished = true;
            unsigned slices = 0;
            double worst = 0.0, total = 0.0;
            vgx::backend::WarmSliceStats(slices, worst, total);
            port_log("[vgx-vita] t=%u ms deferred shader warm-up finished (%u units, frame %llu): %u slices, %.1f s in all, longest %.0f ms", NowMs(), all,
                     static_cast<unsigned long long>(g_frames), slices, total / 1000.0, worst);
        }
    }
    ++g_fpsFrames;
    if (now - g_fpsT0 >= 10000000u) { // every 10 s: the frame rate of the executed frames
        const double seconds = static_cast<double>(now - g_fpsT0) / 1e6;
        port_log("[vgx-vita] t=%u ms frames executed: %llu in all, %.1f fps over the last %.1f s", NowMs(), static_cast<unsigned long long>(g_frames),
                 static_cast<double>(g_fpsFrames) / seconds, seconds);
        g_fpsT0 = now;
        g_fpsFrames = 0;
    }
}

// The game frame of the first picture with PETARI_SCRIPT_ANCHOR_DRAWS draw calls, 0 before that.
extern "C" unsigned long long port_vgx_vita_rich_frame(void) { return g_richFrame.load(); }

// Free memory on the calling thread's behalf (vita/vgx-game/entry.cpp logs it at start-up).
extern "C" void port_vgx_census_log(const char* when);
extern "C" void port_vgx_vita_log_memory(const char* when) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    LogMemory(when);
    port_vgx_census_log(when);
}

// The status block's per-thread CPU report (see ThreadCpuReport). GXV_THREAD_REPORT=0 turns it off.
extern "C" void port_vgx_vita_thread_report(void) {
    if (!PORT_RUNTIME_DIAGNOSTICS) return;
    static const bool on = [] { const char* e = std::getenv("GXV_THREAD_REPORT"); return !(e && *e && std::atoi(e) == 0); }();
    if (on) ThreadCpuReport();
}

// vgx_galaxy.cpp's desktop screenshot path (never asked for on the Vita).
extern "C" void port_headless_write_png(const char*, const unsigned char*, int, int) {}

// ---- GL calls routed here from the back end (include/GLES2/gl2.h)
// GXV_VITA_MASK_GUARD=1 (default 0): FOR VITA3K'S VULKAN RENDERER ONLY. vitaGL implements the
// scissor with sceGxm's mask bit and redraws the mask inside the first draw or clear after every
// scene start and scissor change; Vita3K's Vulkan renderer has no mask and draws those quads into
// the COLOUR surface. With the guard on, every draw and clear is preceded by a clear of nothing
// under depth function NEVER: vitaGL does its mask update inside that call, the emulator's
// quads fail the depth test and the draw that follows finds the mask clean.
//
// GXV_VITA_MASK_GUARD=2: FOR VITA3K'S OPENGL RENDERER ONLY (scripts/run-vita.sh sets it). That
// renderer draws the mask quads into a mask texture with the depth and stencil tests off and
// then "restores" depth state with glDepthMask(GL_TRUE) (renderer/src/gl/draw.cpp, the
// is_maskupdate branch, calling sync_depth_data): from a mask update on, the emulator writes
// depth whatever sceGxm's depth-write state is, until the guest next CHANGES that state. vitaGL
// only sets it when the GL depth state changes, so a draw with depth writes off that follows a
// scene start or scissor change wrote depth in the emulator. Seen in the castle plaza: the quad
// with which the game clears alpha under its 160x160 glare copy (depth test off, colour writes
// off) left its depth behind, and the sky, drawn later, was missing in that square (the frame's
// clear colour showed). With the guard on, every draw and clear is preceded by a clear of
// nothing, inside which vitaGL does its mask update, and then the depth-write state is given
// again by way of the opposite value. A console needs none of this.
// The guards run when the back end's model of vitaGL says a mask update is about to happen
// (vgxvita_maskGuardDue, set in vgx_backend_gl.cpp's MarkScissorDirty: a new scene, a scissor
// change), not before every draw: that halved the emulator's draw rate.
// GXV_VITA_MASK_GUARD_ALWAYS=1 goes back to every draw (if the model ever misses a case).
extern "C" { unsigned char vgxvita_maskGuardOn = 0; unsigned char vgxvita_maskGuardDue = 1; }
namespace {
GLenum g_depthFunc = GL_LESS; // GL's initial values
bool g_depthTestOn = false;
bool g_depthMaskOn = true;
} // namespace
extern "C" void vgxvita_glDepthFunc(GLenum func) {
    g_depthFunc = func;
    glDepthFunc(func);
}
extern "C" void vgxvita_glDepthMask(GLboolean flag) {
    g_depthMaskOn = flag != GL_FALSE;
    glDepthMask(flag);
}
extern "C" void vgxvita_maskGuard(void) {
    if (!g_guardAlways) vgxvita_maskGuardDue = 0;
    if (vgxvita_maskGuardOn == 2) {
        // sceGxm's depth-write state is switched ON first (vitaGL: "mask and test"), so that every
        // later change is one the emulator sees (it ignores a value it already has): inside
        // glClear vitaGL first does the pending mask update, then switches depth writes off for
        // its clear quad (which therefore writes nothing: no bits were asked for), then on
        // again; the last two lines put back what the back end had set.
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClear(0);
        if (!g_depthMaskOn) glDepthMask(GL_FALSE);
        if (!g_depthTestOn) glDisable(GL_DEPTH_TEST);
        return;
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_NEVER);
    glClear(0);
    glDepthFunc(g_depthFunc);
    if (!g_depthTestOn) glDisable(GL_DEPTH_TEST);
}

// The scissor rectangle while the scissor is OFF. GL ignores it then; vitaGL does not quite:
// update_scissor_test() clips the rectangle to the bound target whether the test is on or not,
// and a rectangle that starts right of a narrower target leaves a NEGATIVE width in vitaGL's
// `region`, which Vita3K's OpenGL renderer turns into a glScissor with a negative size (a GL
// error it then blames on whatever host object it creates next). So the rectangle the back end
// set is kept here, vitaGL's copy is parked at a rectangle inside any target while the scissor
// is off, and put back when it is switched on.
namespace {
GLint g_scRect[4] = {0, 0, 1, 1};
void ParkScissor() { glScissor(0, 0, 1, 1); }
void UnparkScissor() { glScissor(g_scRect[0], g_scRect[1], g_scRect[2], g_scRect[3]); }
} // namespace
extern "C" void vgxvita_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_scRect[0] = x; g_scRect[1] = y; g_scRect[2] = width; g_scRect[3] = height;
    if (g_scissorOn) glScissor(x, y, width, height); // (off: it is given when the scissor is switched on)
}
extern "C" void vgxvita_glEnable(GLenum cap) {
    if (cap == GL_DITHER) return;
    if (cap == GL_SCISSOR_TEST) { g_scissorOn = true; UnparkScissor(); }
    if (cap == GL_DEPTH_TEST) g_depthTestOn = true;
    glEnable(cap);
}
extern "C" void vgxvita_glDisable(GLenum cap) {
    if (cap == GL_DITHER) return; // vitaGL has no dithering to switch off
    if (cap == GL_SCISSOR_TEST) g_scissorOn = false;
    if (cap == GL_DEPTH_TEST) g_depthTestOn = false;
    glDisable(cap);
    if (cap == GL_SCISSOR_TEST) ParkScissor();
}

// The constant a disabled attribute array supplies. In GL the value set with glVertexAttrib4f
// stays until it is set again; vitaGL stores each value in the next slot of a ring of 16,384
// four-float slots, so once the ring has gone round a location whose constant was set long ago
// reads whatever a later call wrote into its slot. The values are therefore shadowed here and
// all of them are issued again every 2,048 calls.
extern "C" void vgxvita_glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    static float shadow[16][4];
    static uint32_t known = 0, sinceRefresh = 0;
    glVertexAttrib4f(index, v0, v1, v2, v3);
    if (index >= 16) return;
    shadow[index][0] = v0;
    shadow[index][1] = v1;
    shadow[index][2] = v2;
    shadow[index][3] = v3;
    known |= 1u << index;
    if (++sinceRefresh < 2048) return;
    sinceRefresh = 0;
    for (GLuint i = 0; i < 16; ++i)
        if (i != index && (known & (1u << i))) glVertexAttrib4f(i, shadow[i][0], shadow[i][1], shadow[i][2], shadow[i][3]);
}

// Texture data that goes through the GPU's transfer unit: vitaGL converts a compressed level (and
// generates mip levels) with sceGxmTransferCopy / sceGxmTransferDownscale and returns without
// waiting, but the upload of the NEXT level reallocates the texture's memory. In Vita3K, which
// runs transfers later on its render thread, that crashed in every session at the first scene
// with mip-mapped DXT1 textures. Waiting for the transfer unit after each such call closes it on
// both. GXV_VITA_UPLOAD_SYNC=0 turns the wait off (to measure what it costs).
static bool UploadSync() {
    static const bool on = [] {
        const char* e = std::getenv("GXV_VITA_UPLOAD_SYNC");
        return !(e && *e && std::atoi(e) == 0);
    }();
    return on;
}
extern "C" void vgxvita_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                               GLsizei imageSize, const void* data) {
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    if (UploadSync()) sceGxmTransferFinish();
}
extern "C" void vgxvita_glGenerateMipmap(GLenum target) {
    glGenerateMipmap(target);
    if (UploadSync()) sceGxmTransferFinish();
}

// ---- vitaGL's cache folders, made when they are needed
// vitaGL stores a compiled shader as <cache>/v2/{v,f}/<HH>/<hash>.gxp, hash = XXH3_64bits of the
// source text as handed to glShaderSource, <HH> = its top byte. The frozen library expects
// all 512 folders at startup. Prepare just the bucket in its actual cache-write path instead:
// cached reads need no mkdir, and normal shader sources need no second hash or joined copy.
extern "C" { extern char vgl_shader_cache_path[256] __attribute__((weak)); }
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
extern "C" int vgxvita_shaderCacheRead_v1(const char* path, const void** bytes, size_t* size) {
    return g_shaderPack.Find(path, bytes, size) ? 1 : 0;
}
#endif
namespace {
std::mutex g_bucketMutex; // Only matched private cache writes; never acquired on cache reads.
uint8_t g_bucketSeen[2][32];
char g_bucketRoot[256];
#if VGX_COLDTRACE
std::vector<uint8_t> g_shaderKind; // Probe-only shader identities, owned by the GL thread.
#endif
unsigned g_bucketsMade = 0;
uint64_t g_bucketUs = 0;
int CacheHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace
extern "C" void vgxvita_prepareShaderCacheWrite_v1(const char* path, int flags, SceMode mode) {
    if (flags != (SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC) || mode != 0777 || !path) return;
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return;
    const size_t rootSize = strnlen(vgl_shader_cache_path, sizeof(vgl_shader_cache_path));
    const size_t pathSize = strnlen(path, 320);
    if (rootSize == sizeof(vgl_shader_cache_path) || pathSize < rootSize + 11 || pathSize > rootSize + 26 ||
        std::strncmp(path, vgl_shader_cache_path, rootSize) != 0) return;
    const char* suffix = path + rootSize;
    if (suffix[0] != '/' || (suffix[1] != 'v' && suffix[1] != 'f') || suffix[2] != '/' || suffix[5] != '/') return;
    const int hi = CacheHex(suffix[3]), lo = CacheHex(suffix[4]);
    if (hi < 0 || lo < 0) return;
    const unsigned bucket = static_cast<unsigned>((hi << 4) | lo);
    const size_t digits = pathSize - rootSize - 10;
    if (std::memcmp(suffix + 6 + digits, ".gxp", 4) != 0) return;
    uint64_t hash = 0;
    for (size_t i = 0; i < digits; ++i) {
        const int digit = CacheHex(suffix[6 + i]);
        if (digit < 0) return;
        hash = (hash << 4) | static_cast<unsigned>(digit);
    }
    if (static_cast<unsigned>(hash >> 56) != bucket) return;

    // vitaGL changes the root only while its GL owner is quiescent. Serialize our own
    // memoized state in case another SDK caller writes a matching private cache file.
    std::lock_guard<std::mutex> lock(g_bucketMutex);
    if (std::strcmp(g_bucketRoot, vgl_shader_cache_path) != 0) {
        std::memcpy(g_bucketRoot, vgl_shader_cache_path, rootSize + 1);
        std::memset(g_bucketSeen, 0, sizeof(g_bucketSeen));
    }
    uint8_t& bits = g_bucketSeen[suffix[1] == 'f'][bucket >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (bucket & 7u));
    if (bits & bit) return;
    char bucketPath[320];
    const int n = std::snprintf(bucketPath, sizeof(bucketPath), "%s/%c/%02X", g_bucketRoot, suffix[1], bucket);
    if (n < 0 || n >= static_cast<int>(sizeof(bucketPath))) return;
    const uint64_t t0 = (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? NowUs() : 0;
    const int result = sceIoMkdir(bucketPath, 0777);
    if (result >= 0) ++g_bucketsMade;
    // Vita filesystem EEXIST: a transient mkdir failure must not poison future misses.
    if (result >= 0 || result == static_cast<int>(0x80010011u)) bits |= bit;
    if (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) g_bucketUs += NowUs() - t0;
}
extern "C" GLuint vgxvita_glCreateShader(GLenum type) {
    const GLuint shader = glCreateShader(type);
#if VGX_COLDTRACE
    if (shader < 65536u) {
        if (g_shaderKind.size() <= shader) g_shaderKind.resize(shader + 64u, 0);
        g_shaderKind[shader] = type == GL_VERTEX_SHADER ? 1 : 2;
    }
#endif
    return shader;
}
extern "C" void vgxvita_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
#if VGX_COLDTRACE
    if (shader < g_shaderKind.size() && g_shaderKind[shader] != 0 && count > 0 && string) {
        uint64_t hash;
        if (count == 1 && !(length && length[0] >= 0)) {
            hash = XXH3_64bits(string[0], std::strlen(string[0]));
        } else {
            static std::string joined; // (render thread only)
            joined.clear();
            for (GLsizei i = 0; i < count; ++i) {
                if (length && length[i] >= 0) joined.append(string[i], static_cast<size_t>(length[i]));
                else joined.append(string[i]);
            }
            hash = XXH3_64bits(joined.data(), joined.size());
        }
        vgx::coldtrace::g_recorder.MarkShader(shader,g_shaderKind[shader],hash);
    }
#endif
    glShaderSource(shader, count, string, length);
}
extern "C" void vgxvita_cacheFolderStats(unsigned* made, unsigned* ms) {
    // Counts only directories created for cache misses, not already cached shader sources.
    std::lock_guard<std::mutex> lock(g_bucketMutex);
    if (made) *made = g_bucketsMade;
    if (ms) *ms = static_cast<unsigned>(g_bucketUs / 1000u);
}

// vitaGL's own diagnostics (the library is built with LOG_ERRORS and with its print function
// renamed to this). The first 400 messages are logged in full; after that one in 500, so an error
// raised by every draw call cannot flood the log. The shader compiler's "unreferenced local"
// warnings (four or five per compile) are logged six times only.
extern "C" int msc_vgl_printf(const char* format, ...) {
    if (!PORT_LOGGING) return 0;
    static uint32_t count = 0, compilerWarnings = 0;
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (std::strstr(text, "Shader Compiler: W]")) {
        if (++compilerWarnings > 6) return 0;
        if (compilerWarnings == 6) port_log("[vitaGL] (further shader compiler warnings are not logged)");
    }
    ++count;
    if (count > 400 && count % 500 != 0) return 0;
    size_t length = std::strlen(text);
    while (length != 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) text[--length] = '\0';
    if (count > 400) port_log("[vitaGL] (message %u) %s", count, text);
    else port_log("[vitaGL] %s", text);
    return static_cast<int>(length);
}

#endif // __vita__
