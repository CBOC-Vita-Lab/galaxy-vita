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
//   GXV_VITA_RTMEM_LOG=1          log what sceGxm asks for one render target's driver memory at 1 and at 8 scenes per frame (default 0)
//   GXV_VITA_DISPLAY=WxH          display mode (default 960x544; 720x408, 640x368, 480x272)
//   GXV_VITA_WIDE=0|1             1 (default): the game's 16:9 picture over the whole 960x544 screen
//   GXV_VITA_PRIME=0              leave out the scissored clear that Vita3K's OpenGL renderer needs
//   GXV_VITA_MASK_GUARD=1|2       only for Vita3K: 1 its Vulkan renderer, 2 its OpenGL renderer (see vgxvita_maskGuard)
//   GXV_VITA_UPLOAD_SYNC=0        do not wait for the GPU transfer unit after compressed uploads
//   GXV_TEXBUDGET=<MiB>           texture-cache budget (default 32)
//   GXV_NO_SHADERLIST=1           do not use the shader warm-up list
//   PETARI_SCRIPT_ANCHOR_DRAWS=<n> (test harness) port_vgx_vita_rich_frame() reports the game frame of the first
//                                 picture with at least n draw calls (the title screen has ~1000, a loading screen < 10)
//   GXV_VITA_SEED=copy            copy the shader cache packed in the VPK to the card once (before 1.2; default: read it in place
//                                 behind the card's own cache);  =app0: vitaGL uses only the packed cache (nothing it compiles is
//                                 kept);  =off: the packed cache is not used
#if defined(__vita__)

#define VGX_VITA_NO_GL_ROUTING
#include "port/boot_dialog.h"
#include "port/keep_awake.h"
#include "port/load_split.h"
#include "port/diagnostics.h"
#include "port/hrw_stage.h"  // (pace33 PATCH9: GXV_HRW_STAGE_AUTO, the "even:" window's score)
#include <GLES2/gl2.h>

#include <psp2/display.h>
#include <pthread.h>
#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "loading_screen.h"
#include "vgx_boot_screen.h"
#include "vgx_boot_watch.h"
#include "vgx_common.h"
#include "vgx_dma_load.h" // (dma31: GXV_DMA_LOAD)
#include "vgx_resource_audit.h"
#include "vgx_display_rect.h"
#include "vgx_directdisplay.h"
#include "vgx_shadergen.h"
#include "vgx_coldtrace.h"
#include "port/vgx_coldtrace_control.h"
#include "port/core_layout.h"
#include "port/frame_pace.h"
#include "port/drain33.h"  // (pace33: GXV_DRAIN33)
#include "port/even26.h"
#include "port/q30cushion.h"  // (q30cush31: GXV_Q30_CUSHION, the "q30sep:" line)
#include "vgx_state.h"
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
#include "vgx_shader_pack.h"
#include "vgx_compact_seed.h"
#include "vgx_pack_misslog.h"  // (packrefill25: GXV_PACK_MISSLOG)
#include "vgx_cook_pack.h"
#include "vgx_shader_reader.h"
#include "vgx_swizzle_layout.h"
#include "vgx_texup_scratch.h" // (texup31: GXV_TEXUP_CENSUS / _LAZYFENCE / _SCRATCH / _VERIFY)
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
#include <malloc.h>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void vglSetNextSceneDepthModes(GLboolean load, GLboolean store) __attribute__((weak)); // vitaGL built with DEPTH_HINT only
extern "C" GLboolean vglFinishUniformPoolScene(void) __attribute__((weak));
extern "C" void vglResumeUniformPoolScene(GLboolean resume) __attribute__((weak));
extern "C" void vglSubmitWithoutSwap(void) __attribute__((weak));
extern "C" void vglUseVramForUSSE(GLboolean usage) __attribute__((weak)); // (GXV_VITA_USSE_CDRAM; upstream vitaGL, in the reader archive)
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2))); // (platform/src/os/os_alarm.cpp: the basic log in ux0:data/smg/log.txt)
extern "C" void port_log_summary_cb(const char* fmt, ...) __attribute__((format(printf, 1, 2))); // (stackgate31: the same line for the display-queue callback thread: a static buffer, not 1100 bytes of its 4 KiB stack; os_alarm.cpp)
extern "C" void port_log_flush(void); // (2.3: flushed after a vitaGL fail-stop message)
extern "C" int port_perf_mode_60(void) __attribute__((weak)); // (platform/src/port/perf.cpp: Performance mode, GXV_AUTOQ_TARGET=60; gpucut23's GXV_PRESENT_PLAIN=perf)
extern "C" unsigned port_vgx_pace_divisor(void);
extern "C" void port_stage_warm_progress(unsigned* queued, unsigned* left, unsigned* links, double* ms) __attribute__((weak)); // (vgx_backend_gl.cpp, pace24)
extern "C" int port_pace_steady30_engaged(void) __attribute__((weak));     // (perf.cpp: the steady 30)
extern "C" void port_q30lock_shown(unsigned hold) __attribute__((weak));  // (perf.cpp: the Quality lock ladder, port/q30lock.h: the vblanks a shown picture was held)
extern "C" int port_pose_phase_window_take(unsigned* seenAll, unsigned* seenOld, unsigned* skipTurns, unsigned* broken, unsigned* turns, int* mode) __attribute__((weak));  // (perf.cpp: the pose-phase census since the last call; q30cush31)
extern "C" int port_pace_even_info(char* out, unsigned cap) __attribute__((weak));  // (perf.cpp: which rule chose the pictures; the "even:" line)
extern "C" int port_vgx_even_take(uint32_t* steps, uint32_t* vcount) __attribute__((weak));  // (vgx_galaxy.cpp: the fixed 30's turns a picture, in order)
extern "C" uint32_t port_pace_steady30_changes(void) __attribute__((weak)); // (perf.cpp: how often it changed; GXV_PACE24's "smooth:" line)
extern "C" void port_vgx_pending_allowance_set(unsigned n);  // vgx_galaxy.cpp (pace25: PresentSync::HoldAllowance for the game thread's skip rule)
extern "C" int port_vgx_present_sync_mode(void);  // vgx_galaxy.cpp: GXV_PRESENT_SYNC, 0 when off or when GXV_PACE owns the display callback
extern "C" void port_vgx_fif_retire(void);  // vgx_galaxy.cpp (GXV_FIF)
extern "C" int port_hrw_still(void) __attribute__((weak));  // vgx_galaxy.cpp (pace33: the camera between the last pair's turns, for GXV_DRAIN33)
extern "C" unsigned port_vgx_queue_max_take(void) __attribute__((weak));  // vgx_galaxy.cpp: the most frames the back end's queue held (GXV_LATENCY24's line)
extern "C" void port_input_age_take(unsigned* reads, double* meanMs, double* maxMs, unsigned* resamples) __attribute__((weak));  // wpad.cpp (GXV_LATENCY24's line)
extern "C" int port_vita_core0_alone(void) __attribute__((weak)); // GXV_CORE0_ALONE (vita/runtime/memory.cpp)
extern "C" void port_vgx_pace_waited(uint64_t us);
extern "C" int port_vgx_gate_wait_take(uint64_t* n, uint64_t* totalUs, uint64_t* maxUs);
extern "C" uint32_t vglGetMaskSkips(void) __attribute__((weak)); // vitaGL built with MASK_SKIP: full-target mask quads left out so far
extern "C" void vglSetMaskKeep(GLboolean on) __attribute__((weak));    // vitaGL with patches/0018 built with MASK_KEEP (GXV_VGL_MASKKEEP)
extern "C" uint32_t vglGetMaskKeeps(void) __attribute__((weak));      // (the same: full-target mask quads left out by it so far)
extern "C" void vglSetMaskKeepDisplay(GLboolean on) __attribute__((weak)); // (patches/0022: MASK_KEEP for the display's own scene, GXV_VGL_MASKKEEP_DISPLAY)
extern "C" GLboolean vglColorLoadSkip(GLboolean arm) __attribute__((weak)); // vitaGL with patches/0021 (GXV_COLOR_LOADSKIP)
extern "C" GLboolean vglColorLoadFrom(GLuint tex) __attribute__((weak)); // vitaGL with patches/0019 built with COLOR_LOAD_FROM (GXV_VGL_LOADFROM)
extern "C" uint32_t vglGetColorLoadsFrom(void) __attribute__((weak));   // (the same: scenes that loaded their colour from another buffer)
// vitaGL patch 0028 (vp26, scenes23): VP26_VALID_REGION, VP26_SKIP_COLOR, VP26_LOADFROM_INIT
extern "C" GLboolean vglSetNextSceneValidRegion(GLint xMax, GLint yMax) __attribute__((weak)); // (GXV_VGL_VALIDREGION)
extern "C" uint32_t vglGetValidRegionScenes(void) __attribute__((weak));
extern "C" uint32_t vglGetValidRegionRefusals(void) __attribute__((weak));
extern "C" GLboolean vglColorLoadSkipColor(GLboolean arm, uint32_t rgba) __attribute__((weak)); // (GXV_COLOR_LOADSKIP bit 16)
extern "C" uint32_t vglGetColorLoadFromFailures(void) __attribute__((weak));
extern "C" uint32_t vglGetColorLoadSkips(void) __attribute__((weak));
extern "C" uint32_t vglGetColorSurfaceRefusals(void) __attribute__((weak)); // (VP26_SURFACE_RETRY)
extern "C" uint32_t vglGetSceneBeginFailures(void) __attribute__((weak));
// GXV_DIRECT_DISPLAY (direct24; vitaGL patch 0030): the picture shown by queueing the EFB's own colour buffer. Weak: a vitaGL without it never flips.
extern "C" int vglSetNextFlip(GLuint name) __attribute__((weak));
extern "C" void* vglForceAllocAligned(uint32_t alignment, uint32_t size, GLboolean for_gpu) __attribute__((weak));
extern "C" uint32_t vglGetDisplayFailure(uint32_t* out) __attribute__((weak)); // (patch 0032)
#include "vgx_core3_probe_vita.h"
extern "C" unsigned long long port_game_frames(void); // vgx_galaxy.cpp: frames the game has finished
extern "C" int port_boot_title_is_visible(void); // perf.cpp: the title screen has appeared (decomp/.../TitleSequenceProduct.cpp)
extern "C" int port_boot_play_has_begun(void);  // perf.cpp: PETARI_BOOT went straight into a stage and play began (no title on that path)
extern "C" void vglSetAnisotropicFilter(uint32_t sceFilter) __attribute__((weak)); // vitaGL with ANISO_SWITCH (GXV_ANISO)
// vitaGL vp34 patches (0040, 0041, 0043): all weak so older libvitaGL.a archives link and run unchanged.
extern "C" void vglSetFramebufferDetachKeep(GLboolean on) __attribute__((weak));
extern "C" void vglSetWriteCopy(int mode) __attribute__((weak));
extern "C" void vglSetWriteCopyFallback(int on) __attribute__((weak));
extern "C" void vglSetTextureCopyDma(int mode) __attribute__((weak));
extern "C" void vglSetRenderTargetPool(uint32_t count, uint32_t kib) __attribute__((weak));
// Start-up watchdog inputs: platform/src/dvd/dvd.cpp
extern "C" uint64_t port_dvd_bytes_read(void) __attribute__((weak));
extern "C" unsigned port_dvd_failures(void) __attribute__((weak));
extern "C" int port_dvd_first_failure(char* out, size_t size) __attribute__((weak));
extern "C" const char* port_boot_title_audio_problem(void) __attribute__((weak));
// The shader compiler's loader (vitaShaRK, inside vitaGL) and vitaGL's own flag for its outcome at start-up.
extern "C" int shark_init(const char* path) __attribute__((weak));
extern "C" unsigned char is_shark_online __attribute__((weak));
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

// (cook24) whether the game is loading a scene (the card is the game's then): the user pack's writer waits for it. Weak: absent in a host build.
extern "C" int port_scene_loading_active(void) __attribute__((weak));
extern "C" int port_screen_quiet(void) __attribute__((weak));  // (smooth25: PaceGate's "in play")
extern "C" uint32_t port_scene_loading_key(void) __attribute__((weak));        // (packrefill25; platform/src/port/perf.cpp: the stage being loaded, or the last one: its key as the logs print it)
extern "C" int port_load_split_on(void) __attribute__((weak));                      // (platform/src/port/perf.cpp)
extern "C" void port_load_split_disc(PortDiscSplit* out) __attribute__((weak));     // (platform/src/dvd/dvd.cpp)

namespace {

// The display mode vitaGL is started with. 960x544 is the panel; GXV_VITA_DISPLAY=WxH takes one of
// the console's smaller modes (720x408, 640x368, 480x272: the display scales them up), which a
// software-rendered emulator needs to get anywhere (the copy to the display and the emulator's
// own copy to its window are its two largest fills).
int kScreenW = 960, kScreenH = 544;
bool g_guardAlways = false; // GXV_VITA_MASK_GUARD_ALWAYS (see vgxvita_maskGuard)
constexpr const char* kShaderCacheDir = GXV_VITA_DATA_DEVICE "/shadercache";
constexpr const char* kShaderListFile = GXV_VITA_DATA_DEVICE "/shadercache/vgx_shaderlist.bin";
constexpr const char* kStageWarmFile = GXV_VITA_DATA_DEVICE "/shadercache/vgx_stagewarm.bin";
constexpr const char* kHostShaderAbi = "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1";
// (Not bumped for the 2.3 fog bindings "v_fogc" / "v_fogt<n>" in StartGl: they are NEW names, which only the fog shaders' texts use, so
// no text that existed before compiles differently; the old "v_fog" is still unbound. A binding that changes how an EXISTING name
// compiles still needs a bump.)
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
} // namespace (an extern "C" variable belongs at global scope: declared inside the anonymous namespace, GCC finds it ambiguous with later global declarations)
extern "C" char vgl_shader_cache_path[256] __attribute__((weak)); // (vitaGL vgl.c, set in vglInit*)
// MP3: multiplayer's system dialogs (net/src/vlink_vita_radio.cpp: the ad hoc NetCheck dialog, and the keyboard of np_online_ime_vita.cpp) are
// drawn by vitaGL's swap, which must be told that a CommonDialog is on screen (Strikers renderer/vita/vgx_host_vita.cpp, SwapDisplay). Weak: a
// build without net/ has none, and then every swap is the plain one. The dialog step is two atomic loads when nothing is asked for.
extern "C" void np_vita_radio_dialog_render(void) __attribute__((weak));
extern "C" bool np_vita_radio_dialog_active(void) __attribute__((weak));
extern "C" int np_vita_radio_dialog_prepare(bool) __attribute__((weak));
// MP3: runs the pending dialog operation (render thread, before the swap) and says how to swap: GL_TRUE while a dialog is on screen.
static inline GLboolean MpDialogSwapMode() {
    if (!np_vita_radio_dialog_render) return GL_FALSE;
    np_vita_radio_dialog_render();
    return np_vita_radio_dialog_active && np_vita_radio_dialog_active() ? GL_TRUE : GL_FALSE;
}
namespace {
// A file name in vitaGL's private shader cache: <root>/{v,f}/<HH>/<hash>.gxp, spelled exactly as vitaGL spells it
// (source/custom_shaders.c: "%s/%s/%02X/%llX.gxp", the hash in upper-case hex without leading zeros, <HH> its top byte).
// Returns the length of `root` when `path` is such a name under it (kind 'v' or 'f', bucket = HH), else 0. Nothing is read or
// written: shared by the write preparation and the read-only seed lookup below.
int CacheHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
size_t CacheFileUnder(const char* root, const char* path, char* kind, unsigned* bucketOut) {
    if (!root || !root[0] || !path) return 0;
    const size_t rootSize = strnlen(root, 256);
    const size_t pathSize = strnlen(path, 320);
    if (rootSize == 256 || pathSize < rootSize + 11 || pathSize > rootSize + 26 || std::strncmp(path, root, rootSize) != 0) return 0;
    const char* suffix = path + rootSize;
    if (suffix[0] != '/' || (suffix[1] != 'v' && suffix[1] != 'f') || suffix[2] != '/' || suffix[5] != '/') return 0;
    const int hi = CacheHex(suffix[3]), lo = CacheHex(suffix[4]);
    if (hi < 0 || lo < 0) return 0;
    const unsigned bucket = static_cast<unsigned>((hi << 4) | lo);
    const size_t digits = pathSize - rootSize - 10;
    if (std::memcmp(suffix + 6 + digits, ".gxp", 4) != 0) return 0;
    uint64_t hash = 0;
    for (size_t i = 0; i < digits; ++i) {
        const int digit = CacheHex(suffix[6 + i]);
        if (digit < 0) return 0;
        hash = (hash << 4) | static_cast<unsigned>(digit);
    }
    if (static_cast<unsigned>(hash >> 56) != bucket) return 0;
    if (kind) *kind = suffix[1];
    if (bucketOut) *bucketOut = bucket;
    return rootSize;
}

// ---- the package's compiled shaders as a read-only second level of vitaGL's cache
// vitaGL looks a shader up with sceIoOpen(<cache>/{v,f}/<HH>/<hash>.gxp, SCE_O_RDONLY, 0777) when it is compiled or linked (never per
// draw), and writes a new compile into the same writable <cache> on the card. The game is linked with --wrap=sceIoOpen
// (vita/vgx-game/coldtrace_wrap.cpp): when such a lookup fails, vgxvita_openShaderCacheFallback_v1 opens the same name under the
// package's seed (app0:shadercache/<key>/v<n>) instead, read-only. So the card's own compiles win (a program the card compiled again,
// e.g. because the packaged file was refused, replaces the seed's for good), the seed answers everything else, and nothing has to be
// copied on the first start (a copy of ~2,100 files into ~470 new folders took ~100 s on a memory card). The cost is one failed open
// on the card per program that comes from the seed, at its compile/link only.
// g_seedFallback is set once by SeedShaderCache on the game thread before vitaGL's thread is created; g_seedFallbackOn is cleared
// (on the GL thread) when the compiler self-test fails with no file of the card's own to blame (RemoveSelfTestCacheFiles).
char g_seedFallback[200] = "";
std::atomic<bool> g_seedFallbackOn{false};
std::atomic<unsigned> g_seedFallbackHits{0}, g_seedFallbackMisses{0};
bool g_shaderPackFromSeed = false;            // (GXV_SHADERPACK_SEED) the pack is the package's: it follows g_seedFallbackOn
std::atomic<bool> g_compactRejected{false};
std::atomic<unsigned> g_shaderPackHits{0};    // lookups answered from the pack (the loading screen's summary line)
std::atomic<unsigned> g_userPackHits{0};      // (cook24) lookups answered from the user pack (GXV_USERPACK)
std::atomic<unsigned> g_packMisses{0};        // (cook24) lookups neither pack answered: the file on the card is read, or the shader compiled
// (packrefill25, GXV_PACK_MISSLOG; default 0 = the set is never made and every function below returns at its first test)
vgx::vita::PackMissLog g_missLog;                     // first-miss set (one "pack-miss:" line per distinct key)
std::atomic<unsigned> g_missOrdinal{0};
std::atomic<unsigned> g_compilesAll{0}, g_compilesInPlay{0};   // shader compiler runs (any thread); the ones made while a stage played
// Decoded guest textures the renderer keeps (RGBA8 bytes). Galaxy's castle frame references
// ~6.5 MiB of distinct images (desktop statistics); a budget of 32 MiB keeps the whole working
// set of a scene plus its neighbours and is what the Strikers port ran with.
constexpr uint64_t kTextureBudget = 32ull * 1024 * 1024;
// vitaGL's collector thread: default priority, on core 2 (audio and I/O are there too); the
// render thread is on core 1 and the guest threads keep core 0.
constexpr int kGcPriority = 0x10000100;
constexpr int kCoreGame = 0x10000, kCoreRender = 0x20000, kCoreAux = 0x40000; // SCE_KERNEL_CPU_MASK_USER_0/1/2
// GXV_GC_CORE=1|2 (read once; default 1 in 1.3): vitaGL's collector next to the render thread rather than on core 2, where the
// front end's player decodes the stream: the player is the stage nearest its limit (~28-34 ms of CPU a picture at the
// Observatory, the whole of a 30 FPS budget), the render thread has room (~15 ms). 2: as in 1.2.
int GcCoreMask() {
    static const int mask = vgx::EnvInt("GXV_GC_CORE", 1) == 2 ? kCoreAux : kCoreRender;
    return mask;
}

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
bool g_presentAllowed = true, g_jobSwapped = false, g_wasSplash = true, g_fixedJob = false;
bool g_fifJobSwapped = false;   // (PATCH3; render thread only)
GLboolean g_settingsSceneResume = GL_FALSE;
std::atomic<unsigned long long> g_gameSwaps{0};
std::atomic<unsigned> g_presentSlotEpoch{0};
// GXV_PRESENT_SYNC (frame_pace.h PresentSync, SyncDisplay below): the stamp of the job the render thread is executing (vgx_galaxy.cpp
// ExecuteJob; 0 none, else 1 << 31 | its turn's vblank), and the stamps of the swaps the display queue has not called back for yet,
// in swap order: every vglSwapBuffers of this file notes one (NoteSwap, the game's pictures their stamp, the loading screen and the
// progress bar none), and the display callback takes one per call, so each picture meets its own. One producer (the render thread),
// one consumer (the display queue's thread). The queue holds at most two pictures (three display buffers): eight is never full.
uint32_t g_presentStamp = 0;
bool g_syncOn = false;  // (set once, before the first swap, when SyncDisplay is the display callback)
struct StampRing {
    static constexpr unsigned kSize = 8;
    std::atomic<unsigned> w{0}, r{0};
    uint32_t v[kSize] = {};
    void Push(uint32_t stamp) {
        const unsigned wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= kSize) return;  // (cannot happen; a lost stamp only costs that picture its slot)
        v[wi % kSize] = stamp;
        w.store(wi + 1, std::memory_order_release);
    }
    unsigned Size() const { return w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed); }  // (consumer side)
    bool Pop(uint32_t& stamp) {
        const unsigned ri = r.load(std::memory_order_relaxed);
        if (ri == w.load(std::memory_order_acquire)) return false;
        stamp = v[ri % kSize];
        r.store(ri + 1, std::memory_order_release);
        return true;
    }
} g_stampRing;
inline void NoteSwap(uint32_t stamp) {
    if (g_syncOn) g_stampRing.Push(stamp);
}

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }
unsigned NowMs() { return static_cast<unsigned>(NowUs() / 1000u); }

// GXV_WAITLOG=1 (default 0): where the render thread blocks, per 5 s window ("waits:" line next to "pace:"): the display swap (the
// scene end and the wait for a free display buffer) and the hidden frame's GPU finish.
struct WaitCensus {
    std::atomic<uint64_t> n{0}, totalUs{0}, maxUs{0};
    void Add(uint64_t us) {
        n.fetch_add(1, std::memory_order_relaxed);
        totalUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t seen = maxUs.load(std::memory_order_relaxed);
        while (us > seen && !maxUs.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {}
    }
};
WaitCensus g_swapWait, g_hiddenFinishWait;
bool WaitLogOn() { static const bool on = vgx::EnvInt("GXV_WAITLOG", 0) != 0; return on; }

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

bool g_vglPoolsUp = false; // ram33: vitaGL is initialised (its pools can be asked)
void LogMemory(const char* when) {
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    // ram33: the lowest free main memory seen at any of these lines (the status block calls this every 10 s), and vitaGL's RAM pool at the
    // same moment, so one log says how much of the extended memory is never used and whether the pool ran full (GXV_VITA_VGL_RAM_MIB).
    static unsigned minUserKiB = 0xFFFFFFFFu, minCdramKiB = 0xFFFFFFFFu;
    const unsigned userKiB = static_cast<unsigned>(free.size_user / 1024), cdramKiB = static_cast<unsigned>(free.size_cdram / 1024);
    if (userKiB < minUserKiB) minUserKiB = userKiB;
    if (cdramKiB < minCdramKiB) minCdramKiB = cdramKiB;
    port_log("[vgx-vita] t=%u ms memory %s: free main %d KiB, cdram %d KiB, phycont %d KiB; lowest so far main %u KiB, cdram %u KiB; vitaGL ram pool free %u of %u KiB",
             NowMs(), when, free.size_user / 1024, free.size_cdram / 1024, free.size_phycont / 1024, minUserKiB, minCdramKiB,
             g_vglPoolsUp ? static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024) : 0u, g_vglPoolsUp ? static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) / 1024) : 0u);
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

// The basic log (ux0:data/smg/log.txt) in every build; port_log only reaches it in a diagnostic build.
// (stackgate31: the display-queue callback thread's own: port_log_summary_cb; every log line written on that thread goes through it, tools/check_display_stack.py)
// (fable31fix: no port_log fallback here: its consoleLine has a 1128-byte frame, which must never run on that thread. port_log_summary_cb is defined in every build, a no-op when logging is compiled out.)
#define GXV_CB_LOG(...) port_log_summary_cb(__VA_ARGS__)
#define GXV_BASIC_LOG(...) do { if (port_log_summary) port_log_summary(__VA_ARGS__); else port_log(__VA_ARGS__); } while (0)

// A whole number of seconds from the environment, `fallback` when unset; 0 is a value (the limit is off).
long ReadSeconds(const char* name, long fallback) {
    long value = fallback;
    EnvLong(name, value, 0, 86400);
    return value;
}

// What a status call and a read-only open say about a file (the shader compiler module is loaded by the system from there).
vgx::bootwatch::FileProbe ProbeFile(const char* path) {
    vgx::bootwatch::FileProbe probe;
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    probe.stat = sceIoGetstat(path, &st);
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    probe.open = f;
    if (f >= 0) sceIoClose(f);
    return probe;
}

// ---- shader-compiler self-test: a trivial GLSL pair through GLSL -> Cg -> libshacccg -> link.
// If this fails nothing the renderer generates can work, and the reason is in the returned text.
const char* const kSelfTestVs = "attribute vec4 a_pos;\nvarying vec2 v_uv;\nvoid main() { v_uv = a_pos.xy; gl_Position = a_pos; }\n";
const char* const kSelfTestFs = "precision mediump float;\nvarying vec2 v_uv;\nuniform vec4 u_add;\nvoid main() { gl_FragColor = vec4(v_uv, 0.0, 1.0) + u_add; }\n";
bool ShaderCompilerSelfTest(char* error, size_t errorSize) {
    const char* const sources[2] = {kSelfTestVs, kSelfTestFs};
    const uint64_t t0 = NowUs();
    char log[1024] = "";
    GLuint shaders[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        // (through the host's entry points, like the back end's shaders: they make the cache's
        // bucket folder for this text if it is not there yet, see vgxvita_glShaderSource)
        shaders[i] = vgxvita_glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* text = sources[i];
        vgxvita_glShaderSource(shaders[i], 1, &text, nullptr);
        glCompileShader(shaders[i]);
        GLint ok = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            const unsigned glError = static_cast<unsigned>(glGetError());
            glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            std::snprintf(error, errorSize, "the %s test shader did not compile (GL error 0x%X, compiler %s):\n%s", i == 0 ? "vertex" : "fragment", glError,
                          (&is_shark_online != nullptr && is_shark_online) ? "loaded" : "not loaded", log);
            for (int j = 0; j <= i; ++j) glDeleteShader(shaders[j]);
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
        const unsigned glError = static_cast<unsigned>(glGetError());
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::snprintf(error, errorSize, "the test program did not link (GL error 0x%X):\n%s", glError, log);
        glDeleteProgram(program);
        glDeleteShader(shaders[0]);
        glDeleteShader(shaders[1]);
        return false;
    }
    port_log("[vgx-vita] shader compiler self-test passed in %u ms (u_add at location %d)", static_cast<unsigned>((NowUs() - t0) / 1000u),
             static_cast<int>(glGetUniformLocation(program, "u_add")));
    glDeleteProgram(program);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    return true;
}

// GXV_FOG_SELFTEST=1 (default 0; also run when GXV_FOG_BIND=1): does the console's libshacccg take a float varying on the FOG semantic
// (vitaGL FOUT/FIN: "varying out float x : FOGC" / "varying in float x : FOGC") and link the pair? The one thing about GXV_FOG_BIND=1
// that no host test can show. Logs "fog semantic self-test: passed" or the compiler's message; changes nothing (the mode is decided by
// the environment before any shader is generated). The same name as the generator's mode 1, so it uses the same binding.
void FogSemanticSelfTest() {
    if (vgx::EnvInt("GXV_FOG_SELFTEST", 0) == 0 && vgx::FogBindMode() != 1) return;
    static const char* const kVs = "attribute vec4 a_pos;\nvarying float v_fogc;\nvoid main() { v_fogc = a_pos.z; gl_Position = a_pos; }\n";
    static const char* const kFs = "precision mediump float;\nvarying float v_fogc;\nvoid main() { gl_FragColor = vec4(v_fogc, 0.0, 0.0, 1.0); }\n";
    const char* const sources[2] = {kVs, kFs};
    const uint64_t t0 = NowUs();
    char log[512] = "";
    GLuint shaders[2] = {0, 0};
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        shaders[i] = vgxvita_glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        const char* text = sources[i];
        vgxvita_glShaderSource(shaders[i], 1, &text, nullptr);
        glCompileShader(shaders[i]);
        GLint compiled = 0;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
            GXV_BASIC_LOG("[vgx-vita] fog semantic self-test: the %s shader did not compile (FOGC refused?): %s", i == 0 ? "vertex" : "fragment", log);
            ok = false;
        }
    }
    if (ok) {
        const GLuint program = glCreateProgram();
        glAttachShader(program, shaders[0]);
        glAttachShader(program, shaders[1]);
        glBindAttribLocation(program, 0, "a_pos");
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            GXV_BASIC_LOG("[vgx-vita] fog semantic self-test: the pair did not link: %s", log);
            ok = false;
        }
        glDeleteProgram(program);
    }
    for (GLuint s : shaders)
        if (s) glDeleteShader(s);
    GXV_BASIC_LOG("[vgx-vita] fog semantic self-test: %s in %u ms (GXV_FOG_BIND=%ld)%s", ok ? "passed" : "FAILED",
                  static_cast<unsigned>((NowUs() - t0) / 1000u), vgx::FogBindMode(),
                  !ok && vgx::FogBindMode() == 1 ? ": fogged draws will not compile, use GXV_FOG_BIND=2" : "");
}

// GXV_GXM_PROBE=1 (default 0; diagnostic, scenes23): what GXM really does with a scene's colour load, read back from the GPU once at start-up
// (about 10 ms, behind the loading screen; nothing of the game's state is touched: its own 64x64 targets, deleted afterwards). Each test:
// a 64x64 RGBA target B filled with blue by a scene of its own; then the mechanism is armed and B's next scene begins with a 2x2 clear in
// its corner (scissored), and B's pixel (40,40) - which no draw of that scene touches - is read back: it shows what the scene's tile
// load brought.
//   skip0   GXV_COLOR_LOADSKIP's zero texture (patch 0021)       expect 0,0,0,0     (blue: the background texture is not the load)
//   skipc   the colour texture of bit 16 (patch 0028), red       expect 255,0,0,255
//   from    GXV_VGL_LOADFROM from a yellow target A (0019/0028)  expect 255,255,0,255 (blue: the background was not changed - vp25's
//           sceGxmTextureSetData; black / other: the load read something else - the black screen)
//   vr      a valid region (0,0)-(31,31) (patch 0028), then a full green clear of B: (40,40) blue = only the region's tiles were processed,
//           green = the region was ignored (harmless), refused = sceGxmBeginScene said no
// One line: "gxm-probe: skip0 r,g,b,a (...) skipc ... from ... vr ...". Tests whose vitaGL function is missing say "n/a".
void GxmProbe() {
    if (vgx::EnvInt("GXV_GXM_PROBE", 0) == 0) return;
    const uint64_t t0 = NowUs();
    GLuint tex[2] = {0, 0}, fbo[2] = {0, 0};
    glGenTextures(2, tex);
    glGenFramebuffers(2, fbo);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
    }
    uint8_t base[4] = {0, 0, 0, 0};
    auto fill = [&](int i, float r, float g, float b) { // (a scene of its own: a clear of the whole target, then (40,40) read back: the scene ends)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, 64, 64);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(r, g, b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, base);
    };
    auto away = [&]() { // (the read-back begins a new scene on the target it read: a 1-pixel clear of A at (63,63) moves vitaGL off B, so that
                        // the scene armed next on B is certainly a new one)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
        glEnable(GL_SCISSOR_TEST);
        glScissor(63, 63, 1, 1);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    };
    auto corner = [&](uint8_t out[4]) { // (B's next scene: a 2x2 clear in its corner, then (40,40) read back)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 2, 2);
        glClearColor(0.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    };
    char line[400];
    size_t at = 0;
    auto put = [&](const char* name, bool ran, const uint8_t px[4], const char* note) {
        if (at >= sizeof line) return;
        if (ran) at += std::snprintf(line + at, sizeof line - at, " %s %u,%u,%u,%u%s", name, px[0], px[1], px[2], px[3], note);
        else at += std::snprintf(line + at, sizeof line - at, " %s n/a", name);
    };
    uint8_t px[4] = {0, 0, 0, 0};
    // base: what a scene that begins with a full glClear leaves at (40,40) (blue; anything else: vitaGL's clear did not reach it - then the
    // colour of a clear-begun scene is the LOADED colour where no draw covers, and GXV_COLOR_LOADSKIP's zero texture shows there)
    fill(1, 0, 0, 1);
    put("base", true, base, " (expect 0,0,255,255)");
    // skip0
    away();
    bool ran = false;
    if (vglColorLoadSkip) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        ran = vglColorLoadSkip(GL_TRUE) == GL_TRUE;
        corner(px);
        vglColorLoadSkip(GL_FALSE);
    }
    put("skip0", ran, px, " (expect 0,0,0,0)");
    // skipc
    fill(1, 0, 0, 1);
    away();
    ran = false;
    if (vglColorLoadSkipColor) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        ran = vglColorLoadSkipColor(GL_TRUE, 0xFF0000FFu) == GL_TRUE;
        corner(px);
        vglColorLoadSkip(GL_FALSE);
    }
    put("skipc", ran, px, " (expect 255,0,0,255)");
    // from
    fill(0, 1, 1, 0);
    fill(1, 0, 0, 1);
    away();
    ran = false;
    if (vglColorLoadFrom) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        ran = vglColorLoadFrom(tex[0]) == GL_TRUE;
        corner(px);
    }
    put("from", ran, px, vglGetColorLoadFromFailures ? " (expect 255,255,0,255; vp26)" : " (expect 255,255,0,255; vp25's form)");
    // vr
    fill(1, 0, 0, 1);
    away();
    ran = false;
    uint32_t refusedBefore = vglGetValidRegionRefusals ? vglGetValidRegionRefusals() : 0;
    if (vglSetNextSceneValidRegion) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
        ran = vglSetNextSceneValidRegion(31, 31) == GL_TRUE;
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    const bool refused = vglGetValidRegionRefusals && vglGetValidRegionRefusals() != refusedBefore;
    put("vr", ran, px, refused ? " (REFUSED by sceGxmBeginScene)" : " (blue: honoured, green: ignored)");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbo);
    glDeleteTextures(2, tex);
    GXV_BASIC_LOG("[vgx-vita] gxm-probe:%s; refused changed surfaces %u, scene begin failures %u in %u ms", line,
                  vglGetColorSurfaceRefusals ? vglGetColorSurfaceRefusals() : 0u, vglGetSceneBeginFailures ? vglGetSceneBeginFailures() : 0u,
                  static_cast<unsigned>((NowUs() - t0) / 1000u));
}

// A cached compiled form of a self-test shader that vitaGL cannot use (a file from another library version, cut short by a full
// card) fails the test with nothing wrong in the compiler. The two test shaders' cache files are removed so that the retry compiles
// them afresh; returns how many files were removed. When the card had none of them, the files the test read can only have come from
// the package's seed (read in place, it cannot be removed): the seed is then not used for the rest of this session.
unsigned RemoveSelfTestCacheFiles() {
    if (&vgl_shader_cache_path[0] == nullptr || !vgl_shader_cache_path[0]) return 0;
    unsigned removed = 0;
    for (int i = 0; i < 2; ++i) {
        const char* text = i == 0 ? kSelfTestVs : kSelfTestFs;
        char path[320];
        if (!vgx::bootwatch::ShaderCachePath(path, sizeof(path), vgl_shader_cache_path, i == 1, XXH3_64bits(text, std::strlen(text)))) continue;
        const int r = sceIoRemove(path);
        GXV_BASIC_LOG("[vgx-vita] self-test shader cache file %s: %s", path, r >= 0 ? "removed" : (static_cast<uint32_t>(r) == 0x80010002u ? "not there" : "could not be removed"));
        if (r >= 0) ++removed;
    }
    if (removed == 0 && g_seedFallbackOn.exchange(false)) GXV_BASIC_LOG("[vgx-vita] the package's compiled shaders (%s) are not used for the rest of this start", g_seedFallback);
    if (!removed) g_compactRejected.store(true, std::memory_order_release);
    return removed;
}

bool PresentDisplayCopy(unsigned efb, int x, int y, int w, int h);
void PaceDisplay(void*);
void SyncDisplay(void*);

// =============================================================================================
// bootscr26: every full-screen picture the PORT makes (the boot screen, the branded in-play progress picture, the back end's plain bar, the startup dialog's yield) asks
// portpic::Allowed before it is composed or swapped, and says portpic::Shown when it was on the display. The decision is vgx::bootscreen::PortPicture (vgx_boot_screen.h),
// the same function the back end's ShowProgress asks; PlayStarted is the back end's latch (vgx::backend::PlayStarted: the first live picture of a real stage after the title,
// never cleared). A refusal writes ONE line per kind to the basic log (then every 256th):
//   loading-guard: suppressed <which> in play at <stage> t=<s> (reason ...)
// and every picture that WAS shown writes (basic log, every build):
//   port-picture: <which> shown for <ms> ms (phase ..., reason ...)
namespace portpic {
std::atomic<unsigned> g_refused[vgx::bootscreen::kPicKinds] = {};
bool Allowed(vgx::bootscreen::Pic kind, int phase, const char* where) {
    const vgx::bootscreen::Verdict v = vgx::bootscreen::PortPicture(kind, phase, vgx::backend::PlayStarted(), vgx::backend::PlayScreenLive(), false);
    if (v.ok) return true;
    const unsigned n = g_refused[static_cast<unsigned>(kind)].fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (n + 1) % 256 == 0)
        GXV_BASIC_LOG("loading-guard: suppressed %s in play at %08x t=%.1f s (reason %s; asked by %s; %u refused so far)", vgx::bootscreen::PicName(kind), vgx::backend::PlayStage(),
                      NowMs() / 1000.0, v.reason, where, n + 1);
    return false;
}
void Shown(const char* which, uint64_t sinceUs, int phase, const char* reason) {
    GXV_BASIC_LOG("port-picture: %s shown for %u ms (phase %s%s, reason %s)", which, static_cast<unsigned>((NowUs() - sinceUs) / 1000u), vgx::bootscreen::PhaseName(phase),
                  vgx::backend::PlayStarted() ? ", play started" : "", reason);
}
} // namespace portpic

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
//   kYielded an actionable startup dialog was drawn: game pictures remain visible until the real title arrives.
// The bar follows the stages of the start-up, weighted by how long each took on the last start
// (ux0:data/smg/loadtimes.txt); a stage that knows its progress (unpacking the shaders, the warm-up
// list) reports it, the others advance with time.
//   GXV_VITA_SPLASH=0   no loading screen
namespace splash {

enum Stage { kStart, kUnpack, kGraphics, kGame, kShaders, kTitle, kStageCount };
enum Phase { kOff = 0, kDirect, kGl, kDone, kYielded };

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
std::atomic<bool> g_stalled{false};          // keepawake33: true while GXV_BOOT_STALL_SEC alert is shown (lets OLED sleep on a hung boot)
std::mutex g_failLock;
char g_failText[200] = "";
double g_expect[kStageCount];
uint64_t g_durUs[kStageCount] = {};
uint64_t g_beginUs = 0;
SceUID g_thread = -1;
uint64_t g_shownSinceUs = 0;               // bootscr26: when the boot screen first reached the display (the direct block at the first moments), for the "port-picture:" line
uint64_t g_yieldedSinceUs = 0;             // bootscr26: when an actionable startup dialog took the display from the boot screen
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
std::atomic<uint64_t> g_contentTick{0};    // what the renderer has been given so far (written by the render thread; moves while the game loads)

inline bool Active() { return g_phase.load(std::memory_order_acquire) == kGl; }

void ConfigureDialog() {
    // GXV_BOOTDIALOG=1 yields to a drawn, actionable startup dialog; read once, default off.
    static const bool enabled = vgx::EnvInt("GXV_BOOTDIALOG", 0) != 0;
    port::bootdialog::Enable(enabled);
}

bool HandleDialog();  // Render thread only: owns the handoff back to game presentation.
bool EndIfDue(const char* where);  // bootscr26: ends the boot screen for good when vgx::bootscreen::EndDue says so (render thread; defined after Finish)

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
uint64_t g_holdSinceUs = 0;  // when a GXV_WARM_HOLD hold began
bool g_warmAfterTitle = false;  // the warm-up's slices have not begun: they wait for the title
void NoteTitleVisible(const char* how, bool genuineTitle = false) {
    if (g_phase.load() == kOff || g_failed.load() || g_titleSeen.load()) return;
    // The shader warm-up goes on behind the loading of the game (SetShaderWarmupDeferred); the screen stays up until it is done.
    const unsigned left = vgx::backend::WarmPending(), all = vgx::backend::WarmTotal();
    // The warm-up's compiles read the shader cache from the card, the same card the game loads from, so it is still a third done
    // when the title is up, and holding the screen for it cost 5 s of a 26 s start. By default the title is shown and the rest of
    // the warm-up goes on behind it at GXV_WARM_TITLE_MS (default 10) ms a frame; GXV_WARM_HOLD=1 holds the screen as before.
    static const bool hold = std::getenv("GXV_WARM_HOLD") && std::getenv("GXV_WARM_HOLD")[0] == '1';
    if (left > 0 && all > 0) {
        const bool capacityYield = genuineTitle && vgx::backend::WarmCapacityYielded();
        bool held = hold && !capacityYield;
        if (hold && capacityYield)
            GXV_BASIC_LOG("[vgx-vita] title warm hold released for native program capacity with %u of %u units still pending", left, all);
        if (held) {
            // GXV_WARM_HOLD_MAX_SEC (default 90, 0 = no limit): a hold ends after that long, whatever the warm-up says, so that a
            // warm-up that never finishes cannot keep the screen up for good (the hold itself is off unless GXV_WARM_HOLD=1).
            static const long holdLimit = ReadSeconds("GXV_WARM_HOLD_MAX_SEC", 90);
            if (!g_heldForShaders) {
                g_holdSinceUs = NowUs();
                port_log("[vgx-vita] t=%u ms the title is up, but the shader warm-up has %u of %u units left: the loading screen waits for it", NowMs(), left, all);
            } else if (vgx::bootwatch::HoldExpired(g_holdSinceUs, NowUs(), static_cast<double>(holdLimit))) {
                held = false;
                GXV_BASIC_LOG("loading: the shader warm-up hold ended after %ld s with %u of %u units left (GXV_WARM_HOLD_MAX_SEC)", holdLimit, left, all);
            }
            g_heldForShaders = true;
        }
        if (held) {
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
    uint64_t expectedUs = 0;
    const uint64_t nowUs = NowUs();
    g_titleSeenUs.compare_exchange_strong(expectedUs, nowUs ? nowUs : 1u, std::memory_order_release);
    if (g_titleSeen.exchange(true, std::memory_order_acq_rel)) return;
    port_log("[vgx-vita] t=%u ms loading screen: the title has appeared (%s), the screen fades out", NowMs(), how);
    GXV_BASIC_LOG("loading screen ends: the title has appeared (%s); the boot screen fades out and is taken down at the first game picture after the fade (bootscr26)", how);
}
void Fail(const char* what) {
    if (g_phase.load() == kOff) return;
    std::lock_guard<std::mutex> lock(g_failLock);
    if (g_failed.load()) return;
    std::snprintf(g_failText, sizeof(g_failText), "%s", what);
    g_faded.store(false);
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

extern "C" int port_vgx_shader_compiling(void) __attribute__((weak));
extern "C" uint64_t port_vgx_shader_compile_seq(void) __attribute__((weak));

int TickerMain(SceSize, void*) {
    loading::Init();
    const uint64_t t0 = NowUs();
    // keepawake33 (ported from Strikers 2.0 int33): keep VitaOS from auto-suspending (~15.2 s unattended)
    // during early disc unpack, vitaGL init, and shader warm-up before entry.cpp's pad loop starts.
    // OLED-safe: stops immediately if a fatal boot error is shown or once the splash yields/ends.
    static const bool compileAwake = [] {
        const char* e = std::getenv("GXV_COMPILE_AWAKE");
        return !(e && *e && std::atoi(e) == 0);
    }();
    uint64_t lastPowerTickUs = 0;
    {
        const int phaseNow = g_phase.load(std::memory_order_acquire);
        if (port::keepawake::ShouldSplashTick(compileAwake, g_failed.load(std::memory_order_acquire), phaseNow == kDirect || phaseNow == kGl)) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            lastPowerTickUs = t0;
        }
    }
    float progress = 0.0f, progressAtTitle = 0.0f;
    char caption[128], footer[100], footer2[100];
    int loggedStage = -1;
    bool loggedStall = false, loggedTitle = false, loggedTitleFallback = false;
    // Sign of life: GXV_BOOT_STALL_SEC (default 60, 0 = off): this long without any change of the start-up's counters, of what the
    // renderer has been given and of the bytes read from the card, and the screen says so (it goes away if the start-up goes on).
    static const long stallLimit = ReadSeconds("GXV_BOOT_STALL_SEC", 60);
    // GXV_BOOT_TITLE_FALLBACK=<s> (default 0 = off): this long in the last stage with the game thread running pictures, and the
    // title is taken as seen without its logo (the screen fades out; the game's own picture, whatever it shows, is then visible).
    static const long titleFallback = ReadSeconds("GXV_BOOT_TITLE_FALLBACK", 0);
    vgx::bootwatch::StallWatch watch(static_cast<double>(stallLimit));
    unsigned long long framesSample = 0;
    uint64_t framesSampleUs = t0;
    double advancingSeconds = 0.0;
    uint64_t lastCompileSeq = 0, lastCompileUs = 0, compileStartUs = 0, prevTickUs = t0;
    int prevStageForTimer = -1;
    double nonCompileStageSec = 0.0;
    char audioProblem[48] = "";  // the title audio file's check, made once per stall (it opens the file)
    for (;;) {
        const uint64_t f0 = NowUs();
        const int phaseNow = g_phase.load(std::memory_order_acquire);
        if (phaseNow == kDone) break;
        const uint64_t compileSeq = port_vgx_shader_compile_seq ? port_vgx_shader_compile_seq() : 0u;
        const bool compilingNow = port_vgx_shader_compiling && port_vgx_shader_compiling() != 0;
        if (compilingNow || compileSeq != lastCompileSeq) {
            lastCompileSeq = compileSeq;
            lastCompileUs = f0;
            if (compileStartUs == 0) compileStartUs = f0;
        }
        const bool compileRecent = compilingNow || (lastCompileUs != 0 && f0 >= lastCompileUs && (f0 - lastCompileUs) < 5000000u);
        if (!compileRecent) compileStartUs = 0;
        const int curStage = g_stage.load();
        if (curStage != prevStageForTimer) {
            prevStageForTimer = curStage;
            nonCompileStageSec = 0.0;
        } else if (!compileRecent && f0 > prevTickUs) {
            nonCompileStageSec += static_cast<double>(f0 - prevTickUs) / 1e6;
        }
        prevTickUs = f0;
        const bool stopped = g_failed.load(std::memory_order_acquire) || g_stalled.load(std::memory_order_acquire);
        if (port::keepawake::ShouldSplashTick(compileAwake, stopped, phaseNow == kDirect || phaseNow == kGl) &&
            f0 - lastPowerTickUs >= 1000000u) {
            lastPowerTickUs = f0;
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        }
        if (!g_titleSeen.load() && port_boot_title_is_visible()) NoteTitleVisible("title logo", true);
        // The game thread's pictures per second, sampled about once a second: part of the stall text and of the opt-in fallback.
        if (f0 - framesSampleUs >= 1000000u) {
            const unsigned long long frames = port_game_frames();
            const double dt = (f0 - framesSampleUs) / 1e6;
            advancingSeconds = vgx::bootwatch::FramesAdvancing(framesSample, frames, dt, 20.0) ? advancingSeconds + dt : 0.0;
            framesSample = frames;
            framesSampleUs = f0;
        }
        if (!g_titleSeen.load() && !g_failed.load() && g_stage.load() == kTitle &&
            vgx::bootwatch::TitleFallbackDue(static_cast<double>(titleFallback), (f0 - g_stageStartUs.load()) / 1e6, advancingSeconds, false)) {
            if (!loggedTitleFallback && port_log_summary) {
                loggedTitleFallback = true;
                port_log_summary("loading: no title logo after %.0f s of running pictures (GXV_BOOT_TITLE_FALLBACK): the screen is taken down", (f0 - g_stageStartUs.load()) / 1e6);
            }
            NoteTitleVisible("fallback after running pictures without the title logo");
        }
        // The basic log (log.txt) gets the loading milestones, so that a start that never reaches the title can be read.
        if (port_log_summary) {
            const int now = g_stage.load();
            if (now != loggedStage) {
                loggedStage = now;
                port_log_summary("loading: %s (%.1f s since start)", kCaption[now], (f0 - t0) / 1e6);
            }
            if (g_titleSeen.load() && !loggedTitle) {
                loggedTitle = true;
                port_log_summary("loading: title reached after %.1f s", (f0 - t0) / 1e6);
                if (port_load_split_on && port_load_split_disc && port_load_split_on()) {  // (cook24) what the start spent on the disc, by folder, and what its shaders came from
                    PortDiscSplit now;
                    PortDiscSplit zero;
                    std::memset(&zero, 0, sizeof zero);
                    port_load_split_disc(&now);
                    static char text[900];
                    port::loadsplit::DiscText(text, sizeof text, zero, now);
                    const unsigned ram = g_shaderPackHits.load(std::memory_order_relaxed), user = g_userPackHits.load(std::memory_order_relaxed),
                                   miss = g_packMisses.load(std::memory_order_relaxed);
                    port_log_summary("load-split: boot to the title %.1f s: %s shader lookups so far: %u from the package/card RAM pack, %u from the user pack, %u neither (loose file on the card or a compile; ~18-22 ms a link each, ~4 ms from a pack)",
                                     (f0 - t0) / 1e6, text, ram, user, miss);
                }
                if (port_stage_warm_progress) {  // (pace24: what the loading screen waited for; GXV_BOOT24_MENU_N / _MS cut that queue)
                    unsigned queued = 0, left = 0, links = 0;
                    double ms = 0;
                    port_stage_warm_progress(&queued, &left, &links, &ms);
                    port_log_summary("loading: boot24: at the title the current stage-warm queue had %u of %u draw variants done (%u left), %u links, %.0f ms spent on it", queued - std::min(queued, left),
                                     queued, left, links, ms);
                }
            }
        }
        loading::State st;
        const int stage = g_stage.load();
        st.tip = static_cast<int>((f0 - t0) / 4500000u);
        if (g_failed.load()) {
            st.caption = "The game could not start";
            // (two lines of about 90 characters: a longer text used to be cut off at the end of the first)
            vgx::bootwatch::WrapTwoLines(g_failText, footer, sizeof(footer), footer2, sizeof(footer2));
            st.footer = footer;
            if (footer2[0]) st.footer2 = footer2;
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
            // Active first-boot shader compilation: inform the player instead of showing a false game-files stall warning.
            if (!g_titleSeen.load() && compileRecent && compileStartUs != 0 && f0 >= compileStartUs && (f0 - compileStartUs) >= 15000000u) {
                st.footer = "Compiling graphics shaders for first launch...";
                st.footer2 = "This only happens once; next starts are much faster.";
            } else if (!g_titleSeen.load() && nonCompileStageSec > 90.0 && stage != kUnpack) {
                // A stage that has gone on for far longer than it ever takes (excluding active shader compiles): say so, and say what to check.
                st.footer = "Taking longer than usual. Check that the game files are in ux0:data/smg/game.";
                st.footer2 = "See Known Issues on the project page.";
                if (!loggedStall && port_log_summary) {
                    loggedStall = true;
                    port_log_summary("loading: still in \"%s\" after %.0f s without reaching the title (see KNOWN-ISSUES.md)", kCaption[stage], nonCompileStageSec);
                }
            }
        }
        if (!g_failed.load() && !g_titleSeen.load()) {
            const uint64_t sign[8] = {static_cast<uint64_t>(stage), static_cast<uint64_t>(g_stagePermille.load() + 1), static_cast<uint64_t>(g_unpackDone.load()),
                                      static_cast<uint64_t>(g_taskDone.load()), static_cast<uint64_t>(vgx::backend::WarmPending()), g_contentTick.load(),
                                      port_dvd_bytes_read ? port_dvd_bytes_read() : 0u, compileSeq};
            const vgx::bootwatch::StallWatch::Result quiet = watch.Update(f0, sign, 8);
            g_stalled.store(quiet.stalled, std::memory_order_release);
            if (quiet.stalled) {
                char problem[200] = "";
                const bool diskFailure = port_dvd_first_failure && port_dvd_first_failure(problem, sizeof(problem));
                if (quiet.becameStalled) std::snprintf(audioProblem, sizeof(audioProblem), "%s", port_boot_title_audio_problem ? port_boot_title_audio_problem() : "");
                const char* audio = audioProblem;
                if (quiet.becameStalled && port_log_summary)
                    port_log_summary("loading: no sign of life for %.0f s in \"%s\": game pictures %llu (%s), disc bytes read %llu, failed disc operations %u, first failure: %s, "
                                     "title audio file: %s, shader warm-up units left %u; the screen says so and goes on waiting",
                                     quiet.quietSeconds, kCaption[stage], static_cast<unsigned long long>(port_game_frames()), advancingSeconds >= 5.0 ? "still running" : "not running",
                                     static_cast<unsigned long long>(port_dvd_bytes_read ? port_dvd_bytes_read() : 0u), port_dvd_failures ? port_dvd_failures() : 0u,
                                     diskFailure ? problem : "none", audio && *audio ? audio : "no problem found", vgx::backend::WarmPending());
                if (advancingSeconds >= 5.0)
                    std::snprintf(footer, sizeof(footer), "The game is running but has not shown its title for %.0f s.", quiet.quietSeconds);
                else
                    std::snprintf(footer, sizeof(footer), "Nothing has loaded for %.0f s.", quiet.quietSeconds);
                if (diskFailure) {
                    char tail[100];
                    vgx::bootwatch::PathTail(tail, sizeof(tail), problem, 84);
                    std::snprintf(footer2, sizeof(footer2), "File problem: %.84s", tail);
                } else if (audio && *audio) {
                    std::snprintf(footer2, sizeof(footer2), "Title audio file (SMG_title_strm.ast): %.40s. Restore the game files.", audio);
                } else {
                    std::snprintf(footer2, sizeof(footer2), "Close the game and start it again. If this repeats, send log.txt.");
                }
                st.footer = footer;
                st.footer2 = footer2;
                st.alert = true;
            } else if (quiet.resumed && port_log_summary) {
                port_log_summary("loading: the start-up went on after a pause (\"%s\")", kCaption[stage]);
            }
        }
        if (g_titleSeen.load(std::memory_order_acquire) && !g_failed.load()) {
            g_stalled.store(false, std::memory_order_release);
            st.titleReady = true;
            const uint64_t seenUs = g_titleSeenUs.load(std::memory_order_acquire);
            const double since = (seenUs != 0 && f0 > seenUs) ? (f0 - seenUs) / 1e6 : 0.0;
            if (progressAtTitle == 0.0f) progressAtTitle = progress;
            const float k = since >= 0.08 ? 1.0f : static_cast<float>(since / 0.08);
            progress = progressAtTitle + (1.0f - progressAtTitle) * k;
            st.caption = "Ready";
            const double fade = (since - 0.08) / 0.12;
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
            if (g_phase.load() == kDirect && !vgx::backend::PlayStarted()) loading::direct::Show(g_pic[back]);  // (bootscr26: never over play)
            g_directBusy.store(false);
        }
        // 30 pictures a second while it goes straight to the display; once the renderer shows it every picture costs the
        // render thread about 35 ms (a 2 MiB texture copied into video memory and a swap), and at the start of the game that
        // thread has other things to do (the shader warm-up above all: the first console run with a picture every 33 ms made
        // it take 15 s instead of 6): 7 a second, 4 during the warm-up, 30 again for the fade at the end.
        uint32_t periodUs = 33000u;
        const int phase = g_phase.load();
        if ((phase == kGl || phase == kYielded) && !g_titleSeen.load()) periodUs = g_stage.load() == kShaders ? 250000u : 150000u;
        const uint64_t used = NowUs() - f0;
        sceKernelDelayThread(used < periodUs ? static_cast<SceUInt>(periodUs - used) : 2000u);
    }
    return 0;
}

// Render thread: puts the latest finished picture on the display (a swap) when it is a new one.
uint64_t g_upUs = 0, g_drawUs = 0, g_swapUs = 0, g_presented = 0;  // (render thread only)
bool PresentFrame(bool force) {
    if (HandleDialog()) return false;
    // bootscr26: the one place the boot screen reaches the renderer's display. Refused after the screen has ended (its buffers are gone) and once play has started.
    if (!g_pic[0] || !g_pic[1] || !portpic::Allowed(vgx::bootscreen::Pic::Boot, g_phase.load(), "PresentFrame")) return false;
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
    NoteSwap(0);
    vglSwapBuffers(MpDialogSwapMode());  // MP3: GL_TRUE while a multiplayer system dialog is up
    const uint64_t d = NowUs();
    g_upUs += b - a;
    g_drawUs += c - b;
    g_swapUs += d - c;
    ++g_presented;
    return true;
}

// bootscr26: the end of the boot screen, for good: the phase is kDone (nothing ever sets another: Active() is false, PresentFrame refuses, playprogress may start), the thread
// is stopped and joined, the pictures are freed. `why`: what ended it (vgx::bootscreen::EndName), for the log.
// If the thread does not end within 2 s (it is in a render) its buffers and the loading assets are LEFT (a leak of two 2 MiB pictures, not a write into freed memory);
// the thread leaves its loop at the next pass because the phase is kDone.
void Finish(const char* why = "the title appeared and the screen faded out") {
    if (g_phase.load() == kDone) return;
    const int phaseBefore = g_phase.load();
    g_phase.store(kDone);
    port::bootdialog::Enable(false);
    bool joined = true;
    if (g_thread >= 0) {
        SceUInt timeout = 2000000u;
        int status = 0;
        joined = sceKernelWaitThreadEnd(g_thread, &status, &timeout) >= 0;
        if (joined) sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (!joined) GXV_BASIC_LOG("loading screen: its thread did not end within 2 s; its pictures are left in memory (bootscr26)");
    vgx::backend::SetProgressHostDraws(false);
    vgx::backend::ReleasePicture();
    if (g_freeBlockAtUs || loading::direct::g_base) loading::direct::Close();
    g_freeBlockAtUs = 0;
    if (joined) {
        for (uint32_t*& p : g_pic) { free(p); p = nullptr; }
        loading::Release();
    }
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
    // (bootscr26) the log.txt equivalents of Strikers' "loading screen done, the game's pictures show": the screen is over, and how long it was on the display.
    GXV_BASIC_LOG("loading screen done, the game's pictures show (%s; was in phase %s; %u pictures)", why, vgx::bootscreen::PhaseName(phaseBefore), g_frames);
    portpic::Shown("boot", g_shownSinceUs ? g_shownSinceUs : g_beginUs, phaseBefore, g_failed.load() ? "start-up failure text, then the game" : why);
    if (g_seedFallback[0])
        GXV_BASIC_LOG("[vgx-vita] compiled shaders read from the package in place so far: %u (%u looked for there and not found), from the RAM pack %u%s",
                 g_seedFallbackHits.load(std::memory_order_relaxed), g_seedFallbackMisses.load(std::memory_order_relaxed),
                 g_shaderPackHits.load(std::memory_order_relaxed),
                 g_seedFallbackOn.load(std::memory_order_relaxed) ? "" : "; switched off after the compiler self-test");
}

bool HandleDialog() {
    if (!port::bootdialog::Enabled()) return false;
    const int phase = g_phase.load();
    if (phase != kGl && phase != kYielded) return false;
    if (g_failed.load()) {
        if (phase == kYielded) g_phase.store(kGl);
        return false;
    }
    if (phase == kGl) {
        if (!port::bootdialog::Requested() || g_titleSeen.load()) return false;
        g_phase.store(kYielded);
        g_yieldedSinceUs = NowUs();
        GXV_BASIC_LOG("loading screen: an actionable startup dialog is up; the game's pictures show until the real title (GXV_BOOTDIALOG)");
    }
    // Stay out of the way across modal transitions; only the real title completes startup.
    if (g_freeBlockAtUs && NowUs() >= g_freeBlockAtUs) {
        loading::direct::Close();
        g_freeBlockAtUs = 0;
    }
    if (g_titleSeen.load()) {
        if (g_yieldedSinceUs) portpic::Shown("dialog", g_yieldedSinceUs, kYielded, "the game's own startup dialog had the display until the real title");
        g_yieldedSinceUs = 0;
        Finish("the real title arrived after a startup dialog");
    } else {
        EndIfDue("dialog");
    }
    return true;
}

// Render thread, wherever it is at a moment when the display can be refreshed. `minGapUs`: not sooner than that after the last
// picture (the shader warm-up asks for 80 ms: showing a picture costs the render thread about 10 ms, and the warm-up is its work).
// bootscr26: the single place that decides the boot screen is over (vgx::bootscreen::EndDue): called at the top and the bottom of Service, from HandleDialog, and from every path a
// game picture arrives by (NoteGamePresent), so that no path is the only one that completes the fade-out. Returns true when the screen ended.
bool EndIfDue(const char* where) {
    const int phase = g_phase.load();
    if (phase != kGl && phase != kYielded) return false;
    static const long finishLimit = ReadSeconds("GXV_BOOT_FINISH_SEC", 5);
    vgx::bootscreen::EndInput in;
    in.phase = phase;
    in.failed = g_failed.load();
    in.titleSeen = g_titleSeen.load();
    in.faded = g_faded.load();
    in.darkSwaps = g_blackPresents;
    in.sinceTitleS = in.titleSeen ? (NowUs() - g_titleSeenUs.load()) / 1e6 : 0.0;
    in.limitS = static_cast<double>(finishLimit);
    in.playStarted = vgx::backend::PlayStarted();
    const vgx::bootscreen::EndWhy why = vgx::bootscreen::EndDue(in);
    if (why == vgx::bootscreen::EndWhy::None) return false;
    if (why == vgx::bootscreen::EndWhy::Deadline)
        GXV_BASIC_LOG("loading: the screen did not fade out within %.0f s of the title (faded %d, pictures shown dark %d): ending it now (%s)", in.sinceTitleS, g_faded.load() ? 1 : 0, g_blackPresents, where);
    Finish(vgx::bootscreen::EndName(why));
    return true;
}
// A game picture is being presented (PresentDisplayCopy, whichever half: 0 the whole present, 1 the picture half of GXV_HUD_NATIVE, 2 its swap half; direct display and the
// ordinary copy alike). The boot screen is asked whether it is over; it never needs a particular path to complete.
void NoteGamePresent() { EndIfDue("a game present"); }

// Render thread, wherever it is at a moment when the display can be refreshed. `minGapUs`: not sooner than that after the last
// picture (the shader warm-up asks for 80 ms: showing a picture costs the render thread about 10 ms, and the warm-up is its work).
void Service(uint64_t minGapUs = 0) {
    if (HandleDialog()) return;
    if (!Active()) return;
    const uint64_t now = NowUs();
    // The title appeared but the screen has not faded out and gone (a picture that could not be shown, a swap that never came):
    // end it after GXV_BOOT_FINISH_SEC (default 5; 0 or more than 10 = 10) so that the game underneath is not hidden for good.
    if (EndIfDue("service")) return;
    if (minGapUs && now - g_lastPresentUs < minGapUs) return;
    const bool shown = PresentFrame(false);
    if (shown) g_lastPresentUs = now;
    if (g_freeBlockAtUs && NowUs() >= g_freeBlockAtUs) {
        loading::direct::Close();
        g_freeBlockAtUs = 0;
    }
    if (shown && !g_failed.load() && g_faded.load()) ++g_blackPresents;  // (every buffer of the swap chain has been shown dark after three)
    EndIfDue("service, after a swap");
}

} // namespace splash

// ux0:data/smg/shadercache/<8 hex digits>: the folder vitaGL keeps compiled shaders in. A
// package whose key differs starts with an empty folder (the old one is left behind).
char g_shaderCacheKey[16] = "";
const char* ShaderCacheFolder() {
    static char path[160] = "";
    if (path[0]) return path;
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", GXV_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* c = id; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    std::snprintf(g_shaderCacheKey, sizeof(g_shaderCacheKey), "/%08x", static_cast<unsigned>(h));
    // hitch33 10 (GXV_SHADER_CACHE_TAG=<1-12 letters or digits>, unset = as before): the card's folder of compiled shaders for this run is <key>-<tag>, a folder of its own
    // that starts empty; the owner's <key> folder is neither read nor written. The package's seed and RAM pack are still found by the plain key (ShaderCacheKey). For tests
    // that must meet shader compiles (the compile worker's console test) without setting the owner's cache aside. Give GXV_STAGEWARM_FILE and GXV_SHADERLIST a path of
    // their own in the same env, or the run's programs are recorded in the owner's lists.
    char tag[16] = "";
    if (const char* e = std::getenv("GXV_SHADER_CACHE_TAG"); e && *e) {
        size_t n = 0;
        for (const char* c = e; *c && n < 12; ++c)
            if ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) tag[n++] = *c;
        tag[n] = 0;
    }
    if (tag[0]) std::snprintf(path, sizeof(path), "%s%s-%s", kShaderCacheDir, g_shaderCacheKey, tag);
    else std::snprintf(path, sizeof(path), "%s%s", kShaderCacheDir, g_shaderCacheKey);
    port_log("[vgx-vita] compiled-shader cache: %s (key of: %s)", path, id);
    if (tag[0]) GXV_BASIC_LOG("shader cache: GXV_SHADER_CACHE_TAG=%s: this run's compiled shaders go to %s (its own folder; the card's %s%s is not touched)", tag, path, kShaderCacheDir,
                              g_shaderCacheKey);
    return path;
}
// "/<8 hex digits>": the key alone, as the package's seed folder is named (the card's folder may carry a GXV_SHADER_CACHE_TAG suffix)
const char* ShaderCacheKey() {
    ShaderCacheFolder();
    return g_shaderCacheKey;
}

#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
// The renderer can still be alive during static teardown. Leave the immutable
// cache to process reclamation instead of freeing bytes a late compile can read.
alignas(vgx::vita::ShaderPack) unsigned char g_shaderPackStorage[sizeof(vgx::vita::ShaderPack)];
vgx::vita::ShaderPack& g_shaderPack = *::new (g_shaderPackStorage) vgx::vita::ShaderPack;
alignas(vgx::vita::CompactSeed) unsigned char g_compactStorage[sizeof(vgx::vita::CompactSeed)];
vgx::vita::CompactSeed& g_compact = *::new (g_compactStorage) vgx::vita::CompactSeed;
void ShaderPackIdentity(char* out, size_t size) {
    std::snprintf(out, size, "%s|%s|gen %u", GXV_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
}
uint64_t ShaderPackAbi() {
    char identity[512];
    ShaderPackIdentity(identity, sizeof identity);
    return XXH3_64bits(identity, std::strlen(identity));
}
std::mutex g_compactOpenMu;
std::atomic<bool> g_compactOpened{false};
std::atomic<bool> g_compactConfigured{false};
char g_compactFile[256] = "", g_compactRoot[256] = "";
void EnsureCompactSeedOpen() {
    if (g_compactOpened.load(std::memory_order_acquire) || !g_compactConfigured.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_compactOpenMu);
    if (g_compactOpened.load(std::memory_order_relaxed)) return;
    if (g_compact.Open(g_compactFile, g_compactRoot, ShaderPackAbi()))
        port_log_summary("shader seed: C182 indexed backing ready: %u package binaries, no loose-file extraction", g_compact.Entries());
    g_compactOpened.store(true, std::memory_order_release);
}
// ---- (cook24, GXV_USERPACK) the user shader pack: what this card's own play compiled, in one read at the next start.
// vgx_cook_pack.h has the file format and the merge. Lookups (any thread) go package pack -> user pack -> the loose file as before; the
// lookups neither pack answers are remembered, and a low-priority thread reads those files from the card once the title is up and the game is
// not loading a scene, merges them into the pack (LRU, <= GXV_USERPACK_MAX_KB) and writes the other slot. The pack in use is never changed in
// this run (lookups stay lock-free); the new one is read at the next start.
alignas(vgx::vita::ShaderPack) unsigned char g_userPackStorage[sizeof(vgx::vita::ShaderPack)];
vgx::vita::ShaderPack& g_userPack = *::new (g_userPackStorage) vgx::vita::ShaderPack;
vgx::vita::cook::Loaded g_userLoaded;
std::atomic<uint8_t> g_userUsed[vgx::vita::ShaderPack::kMaxEntries];   // records that answered a lookup this run (zero at start)
bool g_userEnabled = false;                                             // GXV_USERPACK=1 (set at start, read-only after)
char g_userFolder[200] = "", g_userRoot[256] = "";
size_t g_userCapBytes = 0;
long g_userDelayS = 30;
std::mutex g_userMissMutex;
std::unordered_set<std::string> g_userMisses;                          // paths of the lookups nobody answered (bounded)
std::atomic<unsigned> g_userMissesDropped{0};
constexpr size_t kUserMissLimit = 4096;
// Estimates for the log (weak): a link from a loose file on the card ~18-22 ms, from a RAM pack record ~4 ms (agents/pace24 and smooth23 logs).
constexpr unsigned kLooseLinkMs = 18, kPackLinkMs = 4;

bool UserPackUsable() { return !g_seedFallback[0] || g_seedFallbackOn.load(std::memory_order_acquire); }
bool UserPackFind(const char* path, const void** bytes, size_t* size) {
    const int i = g_userPack.FindIndex(path, bytes, size);
    if (i < 0) return false;
    if (static_cast<unsigned>(i) < vgx::vita::ShaderPack::kMaxEntries) g_userUsed[i].store(1, std::memory_order_relaxed);
    return true;
}
void UserPackNoteMiss(const char* path) {
    if (!g_userEnabled || !path) return;
    std::lock_guard<std::mutex> lock(g_userMissMutex);
    if (g_userMisses.size() >= kUserMissLimit) { g_userMissesDropped.fetch_add(1, std::memory_order_relaxed); return; }
    g_userMisses.emplace(path);
}
bool UserPackBetween(void*) {
    // the card is the game's while a scene loads: wait (the slot stays unfinished, which the next start ignores, if the game quits meanwhile)
    while (port_scene_loading_active && port_scene_loading_active()) sceKernelDelayThread(200 * 1000);
    sceKernelDelayThread(4 * 1000);
    return true;
}
int UserPackThread(SceSize, void*) {
    using namespace vgx::vita;
    while (!splash::g_titleSeen.load(std::memory_order_acquire)) sceKernelDelayThread(500 * 1000);
    for (long i = 0; i < g_userDelayS * 2; ++i) sceKernelDelayThread(500 * 1000);
    UserPackBetween(nullptr);
    const uint64_t t0 = NowUs();
    std::vector<std::string> paths;
    { std::lock_guard<std::mutex> lock(g_userMissMutex); paths.assign(g_userMisses.begin(), g_userMisses.end()); }
    std::sort(paths.begin(), paths.end());
    std::vector<cook::NewRecord> fresh;
    unsigned unreadable = 0;
    const size_t rootSize = std::strlen(g_userRoot);
    for (const std::string& path : paths) {
        UserPackBetween(nullptr);
        cook::NewRecord rec{};
        if (!ShaderPack::ParsePath(g_userRoot, rootSize, path.c_str(), rec.stage, rec.hash)) { ++unreadable; continue; }
        const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0777);   // the card's file, or the package's behind it (the same open vitaGL does)
        if (fd < 0) { ++unreadable; continue; }   // never compiled (the shader was rejected) or never written
        const SceOff end = sceIoLseek(fd, 0, SCE_SEEK_END);
        sceIoLseek(fd, 0, SCE_SEEK_SET);
        bool ok = end >= 8 && end <= static_cast<SceOff>(ShaderPack::kMaxRecordBytes);
        if (ok) {
            rec.data.resize(static_cast<size_t>(end));
            ok = sceIoRead(fd, rec.data.data(), static_cast<SceSize>(end)) == static_cast<int>(end);
        }
        sceIoClose(fd);
        if (!ok || !ShaderPack::ValidRecord(rec.data.data(), static_cast<uint32_t>(rec.data.size()), rec.stage)) { ++unreadable; continue; }
        fresh.push_back(std::move(rec));
    }
    const unsigned readMs = static_cast<unsigned>((NowUs() - t0) / 1000u);
    const unsigned hits = g_userPackHits.load(std::memory_order_relaxed);
    if (fresh.empty()) {
        port_log_summary("cook: user pack: nothing new to write (%u missed lookups, %u unreadable or not compiled, %u dropped); %u lookups answered from it so far (est. %u ms saved)",
                         static_cast<unsigned>(paths.size()), unreadable, g_userMissesDropped.load(), hits, hits * (kLooseLinkMs - kPackLinkMs));
        return 0;
    }
    std::vector<uint8_t> used(g_userPack.Entries());
    for (size_t i = 0; i < used.size(); ++i) used[i] = g_userUsed[i].load(std::memory_order_relaxed);
    std::vector<uint8_t> file;
    cook::MergeStats st;
    if (!cook::Merge(g_userPack, g_userLoaded, used, fresh, ShaderPackAbi(), g_userCapBytes, file, st)) {
        port_log_summary("cook: user pack: merge produced no file (%" VGX_PRIZU " candidates)", fresh.size());
        return 0;
    }
    int slot = -1;
    const uint64_t w0 = NowUs();
    const bool saved = cook::Save(g_userFolder, g_userLoaded.slot, file, 64 * 1024, &UserPackBetween, nullptr, &slot);
    port_log_summary("cook: user pack %s: slot=%c gen=%llu records=%u (+%u new, %u already known, %u evicted, %u unreadable) bytes=%" VGX_PRIZU " cap=%" VGX_PRIZU "; read %u files %u ms, wrote %u ms; "
                     "next start answers these from RAM (est. %u ms of links saved per start if all are linked again)",
                     saved ? "written" : "NOT written", slot ? 'b' : 'a', static_cast<unsigned long long>(g_userLoaded.gen + 1), st.kept, st.added, st.duplicate, st.evicted,
                     unreadable, st.bytes, g_userCapBytes, static_cast<unsigned>(fresh.size()), readMs, static_cast<unsigned>((NowUs() - w0) / 1000u),
                     st.added * (kLooseLinkMs - kPackLinkMs));
    return 0;
}
void PrepareUserPack(const char* root, const char* cardFolder) {
    long enabled = 0;
    EnvLong("GXV_USERPACK", enabled, 0, 1);
    if (!enabled) return;
    long maxKb = 3072;
    EnvLong("GXV_USERPACK_MAX_KB", maxKb, 64, static_cast<long>(vgx::vita::ShaderPack::kMaxBytes >> 10));
    EnvLong("GXV_USERPACK_DELAY_S", g_userDelayS, 0, 3600);
    g_userCapBytes = static_cast<size_t>(maxKb) << 10;
    const int a = std::snprintf(g_userFolder, sizeof g_userFolder, "%s", cardFolder);
    const int b = std::snprintf(g_userRoot, sizeof g_userRoot, "%s", root);
    if (a < 0 || b < 0 || a >= static_cast<int>(sizeof g_userFolder) || b >= static_cast<int>(sizeof g_userRoot)) {
        port_log_summary("cook: user pack: enabled=1 loaded=0 (path too long)");
        return;
    }
    const uint64_t t0 = NowUs();
    const bool loaded = vgx::vita::cook::Load(g_userFolder, g_userRoot, ShaderPackAbi(), g_userPack, g_userLoaded);
    g_userEnabled = true;
    port_log_summary("cook: user pack: enabled=1 loaded=%u records=%u bytes=%u slot=%d gen=%llu slots_on_card=%u refused=%u load_ms=%u cap_kb=%ld write_delay_s=%ld",
                     static_cast<unsigned>(loaded), static_cast<unsigned>(g_userPack.Entries()), static_cast<unsigned>(g_userPack.Bytes()), g_userLoaded.slot,
                     static_cast<unsigned long long>(g_userLoaded.gen), g_userLoaded.slotsSeen, g_userLoaded.slotsRejected,
                     static_cast<unsigned>((NowUs() - t0) / 1000u), maxKb, g_userDelayS);
    const SceUID tid = sceKernelCreateThread("smg_cook", &UserPackThread, 0xBF, 128 * 1024, 0, port::cores::kHelpers, nullptr);
    if (tid < 0 || sceKernelStartThread(tid, 0, nullptr) < 0) {
        port_log_summary("cook: user pack: the writer thread did not start (0x%08X): lookups only", static_cast<unsigned>(tid));
        g_userEnabled = false;
    }
}
void PrepareShaderPack(const char* folder) {
    static bool prepared = false;
    if (prepared) return;
    prepared = true;
    port_vgx_shader_cache_reader_driver_v1();
    // This is the package's backing file, not a RAM-pack optimization. Configure its paths now and
    // open lazily on the first RAM-pack miss (or eagerly below if the RAM pack is disabled/unavailable)
    // so warm boots do not pay ~220 ms to parse & CRC seed.c182 before the first frame.
    const char* seedMode = std::getenv("GXV_VITA_SEED");
    if (!seedMode || std::strcmp(seedMode, "off") != 0) {
        const int f = std::snprintf(g_compactFile, sizeof g_compactFile, "%s%s/%s/seed.c182", kSeedDir, ShaderCacheKey(), kVglCacheVersionDir);
        const int r = std::snprintf(g_compactRoot, sizeof g_compactRoot, "%s/%s", folder, kVglCacheVersionDir);
        if (f > 0 && r > 0 && f < int(sizeof g_compactFile) && r < int(sizeof g_compactRoot))
            g_compactConfigured.store(true, std::memory_order_release);
    }
    long enabled = 0;
    EnvLong("GXV_SHADERPACK", enabled, 0, 1);
    if (!enabled) {
        EnsureCompactSeedOpen();
        port_log_summary("[vgx-vita] shader RAM pack: enabled=0 loaded=0 records=0 bytes=0 fallback=disabled");
        return;
    }
    char root[256], pack[256];
    const int r = std::snprintf(root, sizeof root, "%s/%s", folder, kVglCacheVersionDir);
    const int p = std::snprintf(pack, sizeof pack, "%s/stages.pack", folder);
    if (r < 0 || p < 0 || r >= static_cast<int>(sizeof root) || p >= static_cast<int>(sizeof pack)) {
        EnsureCompactSeedOpen();
        port_log_summary("[vgx-vita] shader RAM pack: enabled=1 loaded=0 records=0 bytes=0 fallback=path-too-long");
        return;
    }
    const uint64_t start = (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? NowUs() : 0;
    bool loaded = g_shaderPack.LoadFile(pack, root, ShaderPackAbi());
    // (GXV_SHADERPACK_SEED, default 1; smooth23) Since 2.1 the package's compiled shaders are read in place (g_seedFallback) and nothing is
    // copied to the card, so the card's folder has no stages.pack and the package's own (app0:shadercache/<key>/stages.pack, shipped in
    // every package since 2.0) was never read: every link of a stage warm-up opened two files (a failed open on the card, then the
    // package's), 12-25 ms a program behind the stage's own archive reads (Honeyhive / Observatory / title: 270-290 links, 3-7 s of loading
    // screen in today's logs). The package's pack is the same files in one read; lookups still name the card's cache folder (`root`):
    // the pack is keyed by the shader's hash, and a record vitaGL refuses falls back to the files as before. Never used once the compiler
    // self-test has switched the package's shaders off (vgxvita_shaderCacheRead_v1).
    const char* source = "card";
    if (!loaded && g_seedFallback[0]) {
        long seedOn = 1;
        EnvLong("GXV_SHADERPACK_SEED", seedOn, 0, 1);
        char seedPack[256];
        const char* slash = std::strrchr(g_seedFallback, '/'); // (g_seedFallback is <seed>/v<n>; the pack sits in <seed>)
        const int n = slash ? std::snprintf(seedPack, sizeof seedPack, "%.*s/stages.pack", static_cast<int>(slash - g_seedFallback), g_seedFallback) : -1;
        if (seedOn && n > 0 && n < static_cast<int>(sizeof seedPack)) {
            loaded = g_shaderPack.LoadFile(seedPack, root, ShaderPackAbi(), true);
            if (loaded) { g_shaderPackFromSeed = true; source = "package"; }
        }
    }
    if (!loaded) EnsureCompactSeedOpen();
    port_log_summary("[vgx-vita] shader RAM pack: enabled=1 loaded=%u records=%u bytes=%u preload_ms=%u ABI=%016llx fallback=%s source=%s",
        static_cast<unsigned>(loaded), static_cast<unsigned>(g_shaderPack.Entries()), static_cast<unsigned>(g_shaderPack.Bytes()),
        static_cast<unsigned>((PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? (NowUs() - start) / 1000u : 0),
        static_cast<unsigned long long>(ShaderPackAbi()), loaded ? "loose-on-miss-or-rejection" : "unavailable-or-invalid", loaded ? source : "none");
    PrepareUserPack(root, ShaderCacheFolder());  // (cook24: off unless GXV_USERPACK=1; it writes into the card's folder, never the package's)
}
#endif

// The back end's progress hook: a bar has been drawn into the display's back buffer in the
// middle of a frame while shaders compile; show it.
//
// In play (the loading screen is gone, or never was) the same screen is shown in the loading screen's look: the player's title
// picture, the same bar, the caption "Preparing graphics". The background is read once into loading::detail::g_bg (host heap, malloc)
// and one finished frame is kept in g_playFrame (host heap); an update restores and redraws only the bar's rows, then the whole
// picture goes through SetPicture/DrawPicture like the loading screen's. All of it runs on the render thread (this hook, and the
// present hook below), so there is no lock. GXV_PROGRESS_BRANDED=0 gives the back end's plain bar back.
namespace playprogress {
uint32_t* g_frame = nullptr;
bool g_failed = false;         // the buffers could not be made: the plain bar stays
bool g_lit = false;            // a branded picture is on the display and no game picture has replaced it yet
uint64_t g_firstUs = 0, g_lastUs = 0;  // first and latest picture of this appearance
uint32_t g_done = 0, g_total = 0;
uint64_t g_plainUs = 0;        // bootscr26: the back end's plain bar went to the display at this time and no game picture has replaced it yet

bool Enabled() {
    static const bool on = [] { long v = 1; EnvLong("GXV_PROGRESS_BRANDED", v, 0, 1); return v != 0; }();
    return on && !g_failed;
}
bool Ready() {
    if (g_frame) return true;
    if (!loading::Init()) { g_failed = true; return false; }
    g_frame = static_cast<uint32_t*>(std::malloc(static_cast<size_t>(loading::W) * loading::H * 4));
    if (!g_frame) { g_failed = true; return false; }
    loading::Render(g_frame, 0.0, loading::State());  // (a complete first frame; later updates redraw the band only)
    return true;
}
// fade: 0 = full picture, towards 1 = black. A fade needs a full render (it touches every pixel); the rest redraw the band.
void Draw(uint32_t done, uint32_t total, float fade) {
    loading::State st;
    st.progress = total ? static_cast<float>(done < total ? done : total) / static_cast<float>(total) : 0.0f;
    st.caption = "Preparing graphics";
    st.titleReady = true;  // (no boot-time clamp at 99%)
    st.fade = fade;
    if (fade > 0.0f) loading::Render(g_frame, 0.0, st); else loading::RenderBand(g_frame, st);
    if (!vgx::backend::SetPicture(g_frame, loading::W, loading::H) || !vgx::backend::DrawPicture()) return;
    NoteSwap(0);
    vglSwapBuffers(MpDialogSwapMode());  // MP3: GL_TRUE while a multiplayer system dialog is up
    g_lastUs = NowUs();
    g_lit = true;
}
bool Allowed() {
    const int phase = splash::g_phase.load();
    return (phase == splash::kOff || phase == splash::kDone) && Enabled();
}
void Show(uint32_t done, uint32_t total) {
    // (bootscr26) the same question the back end asked: a branded picture over a live picture after play started is refused here too.
    if (!portpic::Allowed(vgx::bootscreen::Pic::Progress, splash::g_phase.load(), "playprogress::Show")) return;
    g_done = done;
    g_total = total;
    const uint64_t now = NowUs();
    if (!g_lit) {
        // First picture of an appearance: fade up from black in three steps (about 100 ms), so it does not snap in.
        g_firstUs = now;
        Draw(done, total, 0.75f);
        sceKernelDelayThread(25000u);
        Draw(done, total, 0.5f);
        sceKernelDelayThread(25000u);
        Draw(done, total, 0.25f);
        sceKernelDelayThread(25000u);
        Draw(done, total, 0.0f);
        port_log("[vgx-vita] in-play shader progress: branded screen up (%u of %u units)", done, total);
        return;
    }
    // (a picture costs the render thread a 2 MiB texture copy; the compiles are its work: not more than 8 a second)
    if (now - g_lastUs < 120000u) return;
    Draw(done, total, 0.0f);
}
// Called first thing in every game present. If a branded picture is still up, the game's picture is held back until the screen has
// been readable for a while (at least 900 ms in all, 400 ms at the finished bar), then it fades out in four steps. Without this the
// screen would be up for the time of the last compile only, which can be a few frames: a flash.
void Leave() {
    if (g_plainUs) {  // (bootscr26) the plain bar's picture ends with the game's next present
        portpic::Shown("plain-bar", g_plainUs, splash::g_phase.load(), "shader work met by a picture (the back end's bar, until the game's next picture)");
        g_plainUs = 0;
    }
    if (!g_lit) return;
    if (!portpic::Allowed(vgx::bootscreen::Pic::Progress, splash::g_phase.load(), "playprogress::Leave")) {  // (bootscr26) the game's picture is live: no hold, no fade; the game's picture replaces it
        g_lit = false;
        portpic::Shown("progress", g_firstUs, splash::g_phase.load(), "branded picture raised by shader work, cut at the game's first live picture");
        return;
    }
    const uint64_t now = NowUs();
    if (g_total) Draw(g_total, g_total, 0.0f);  // (the work is done: show the full bar)
    const uint64_t until1 = g_firstUs + 900000u, until2 = g_lastUs + 400000u;
    const uint64_t until = until1 > until2 ? until1 : until2;
    if (until > now && until - now < 2000000u) sceKernelDelayThread(static_cast<SceUInt>(until - now));
    for (int i = 1; i <= 4; ++i) {
        Draw(g_total, g_total, 0.25f * static_cast<float>(i));
        sceKernelDelayThread(20000u);
    }
    g_lit = false;
    portpic::Shown("progress", g_firstUs, splash::g_phase.load(), "branded 'Preparing graphics' picture raised by shader work (units done/total at the end shown as the full bar)");
}
}  // namespace playprogress

void ShowProgressBar(uint32_t done, uint32_t total) {
    if (splash::HandleDialog()) return;
    // (bootscr26) the boot screen's own warm-up stage is the boot picture; anything else here is the progress picture or the plain bar.
    if (!portpic::Allowed(splash::Active() ? vgx::bootscreen::Pic::Boot : (playprogress::Allowed() && playprogress::Ready() ? vgx::bootscreen::Pic::Progress : vgx::bootscreen::Pic::PlainBar),
                          splash::g_phase.load(), "ShowProgressBar")) return;
    if (playprogress::Allowed()) {
        // (the back end skips its own plain bar from the next call on; this call's plain bar is covered by the picture)
        vgx::backend::SetProgressHostDraws(playprogress::Ready());
        if (playprogress::Ready()) {
            playprogress::Show(done, total);
            return;
        }
    }
    static uint32_t shown = 0;
    if (shown++ == 0) port_log("[vgx-vita] shaders are being compiled: progress bar on screen (%u of %u units)", done, total);
    if (splash::Active()) {  // (the loading screen shows the warm-up as one of its stages)
        splash::Enter(splash::kShaders);
        splash::g_stagePermille.store(total ? static_cast<int>(static_cast<uint64_t>(done < total ? done : total) * 1000u / total) : 0);
        splash::Service(80000u);
        return;
    }
    NoteSwap(0);
    vglSwapBuffers(MpDialogSwapMode());  // MP3: GL_TRUE while a multiplayer system dialog is up
    if (!playprogress::g_plainUs) playprogress::g_plainUs = NowUs();  // (bootscr26)
}

// ---- a shader cache that came with the package (app0:shadercache/<key>/v<n>/{v,f}/<HH>/<hash>.gxp
// and vgx_shaderlist.bin). The seed is only found by a package whose <key> (above) is the same. By default it is read in place as the
// second level of vitaGL's cache (g_seedFallback above) and only the warm-up list is copied; GXV_VITA_SEED=copy copies the compiled
// shaders once into the writable cache folder instead (what every package before 1.2 did: ~100 s on the first start of 1.2).
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
// <cache>/v<n>, its stage folders v and f, and for vitaGL's v1/v2 layout also v/00 and f/00: with the first of them present those
// versions did not create their 512 bucket folders at start-up. vitaGL v3 never does (it makes a bucket when it writes a miss into it,
// and v, f only together with a new v3), so v/00 and f/00 are left out there: two folder creations fewer on the first start.
void MakeFirstBuckets(const char* folder, SeedCopyStats& st) {
    static const char* const kParts[] = {"", "/v", "/v/00", "/f", "/f/00"};
    const bool legacyLayout = std::strcmp(kVglCacheVersionDir, "v1") == 0 || std::strcmp(kVglCacheVersionDir, "v2") == 0;
    const uint64_t t0 = NowUs();
    for (const char* part : kParts) {
        if (!legacyLayout && std::strstr(part, "/00")) continue;
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
// The marker <cache>/seeded holds the package list's fingerprint (PackageStamp) once this card has taken what it needs from that
// package: "<stamp>" after a copy of the compiled shaders (GXV_VITA_SEED=copy), "<stamp> lookup" when they are read in place. A copy
// marker also satisfies the lookup (the card then has the files itself); a lookup marker does not satisfy the copy, so switching to
// GXV_VITA_SEED=copy copies once. Either way a changed fingerprint means a new package: its warm-up list replaces the card's.
constexpr const char* kSeedLookupSuffix = " lookup";
struct SeedMarker { bool matches = false, readError = false, replaceList = false; };
SeedMarker CheckSeedMarker(const char* marker, const char* stamp, uint64_t size, bool lookup = false) {
    std::vector<uint8_t> bytes;
    const SeedRead result = ReadSeedBytes(marker, bytes, 80);
    if (result != SeedRead::Ok) return {false, result == SeedRead::Error, false};
    std::string text(bytes.begin(), bytes.end());
    if (text == stamp) return {true, false, false};
    const size_t suffix = std::strlen(kSeedLookupSuffix);
    if (text.size() > suffix && text.compare(text.size() - suffix, suffix, kSeedLookupSuffix) == 0) {
        text.resize(text.size() - suffix);
        if (text == stamp) return {lookup, false, false};
    }
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
bool g_seedComplete = false; // the package's compiled shaders are all there for vitaGL (copied, or read in place): SeedShaderList
char g_seedPendingStamp[80] = {}, g_seedPendingMarker[200] = {};
// The marker is written now, or (a new package whose warm-up list is to replace the card's) by SeedShaderList once the list is in
// place: the old fingerprint is kept until then, so that an adoption cut short is done again at the next start.
void CommitSeedMarker(const char* marker, const char* stamp, const char* suffix, bool replaceList) {
    char text[sizeof g_seedPendingStamp];
    const int n = std::snprintf(text, sizeof text, "%s%s", stamp, suffix);
    if (n < 0 || n >= static_cast<int>(sizeof text)) return;
    if (replaceList && !std::getenv("GXV_SHADERLIST") && !std::getenv("GXV_NO_SHADERLIST")) {
        std::snprintf(g_seedPendingStamp, sizeof g_seedPendingStamp, "%s", text);
        std::snprintf(g_seedPendingMarker, sizeof g_seedPendingMarker, "%s", marker);
        g_seedIsNew = true;
    } else {
        const std::vector<uint8_t> bytes(text, text + n);
        if (!WriteSeedBytes(marker, bytes)) sceIoRemove(marker);
    }
}
// The package's version folder under app0:shadercache/<key>: the one of the vitaGL linked in (v3), else any v<n> (the files are the
// same, only vitaGL's folder name changed; CopyTree renames it the same way). One stat at a normal start.
bool SeedVersionFolder(const char* seed, char* out, size_t size) {
    if (std::snprintf(out, size, "%s/%s", seed, kVglCacheVersionDir) < static_cast<int>(size) && Exists(out)) return true;
    out[0] = 0;
    const SceUID dir = sceIoDopen(seed);
    if (dir < 0) return false;
    SceIoDirent entry;
    bool found = false;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        if (sceIoDread(dir, &entry) <= 0) break;
        const char* name = entry.d_name;
        if (SCE_S_ISDIR(entry.d_stat.st_mode) && name[0] == 'v' && name[1] >= '0' && name[1] <= '9' && std::strspn(name + 1, "0123456789") == std::strlen(name + 1) &&
            std::snprintf(out, size, "%s/%s", seed, name) < static_cast<int>(size)) { found = true; break; }
    }
    sceIoDclose(dir);
    if (!found) out[0] = 0;
    return found;
}
// Returns the folder vitaGL is to use as its cache.
const char* SeedShaderCache(const char* folder) {
    g_seedIsNew = false;
    g_seedComplete = false;
    g_seedPendingStamp[0] = g_seedPendingMarker[0] = 0;
    const char* key = ShaderCacheKey(); // (hitch33 10: not the folder's last part, which may carry a tag)
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
        g_seedComplete = true;
        return seed;
    }
    const char* mode = std::getenv("GXV_VITA_SEED");
    const bool copy = mode && std::strcmp(mode, "copy") == 0;
    char stamp[80];
    uint64_t listSize = 0;
    const bool stamped = PackageStamp(stamp, sizeof(stamp), &listSize);
    const SeedMarker previous = stamped ? CheckSeedMarker(marker, stamp, listSize, !copy) : SeedMarker{};
    if (!copy) {
        // Read in place, behind the card's own cache (g_seedFallback): nothing is copied. What is left to do once per package is the
        // warm-up list (SeedShaderList) and vitaGL's top folders.
        const bool found = SeedVersionFolder(seed, g_seedFallback, sizeof g_seedFallback);
        g_seedFallbackOn.store(found, std::memory_order_release);
        g_seedComplete = found;
        if (previous.matches) return folder;
        MakeFirstBuckets(folder, st);
        GXV_BASIC_LOG("[vgx-vita] compiled shaders of the package %s: %s (read in place after %s; nothing copied, %u folders made)", seed,
                 found ? "found" : "NOT FOUND (no v<n> folder), shaders are compiled on this console", folder, st.folders);
        if (found && stamped && !previous.readError) CommitSeedMarker(marker, stamp, kSeedLookupSuffix, previous.replaceList);
        return folder;
    }
    if (previous.matches) {
        g_seedComplete = true;
        return folder;
    }
    const uint64_t t0 = NowUs();
    st.total = CountTree(seed, 0);
    splash::UnpackBegin(static_cast<int>(st.total));
    CopyTree(seed, folder, 0, false, st);
    MakeFirstBuckets(folder, st);
    port_log("[vgx-vita] compiled shaders from the package copied to %s: %u files (%u KiB), %u already there, %u failed; %u folders; %u ms in all", folder,
             st.files - st.kept, st.bytes / 1024u, st.kept, st.failed, st.folders, static_cast<unsigned>((NowUs() - t0) / 1000u));
    if (st.failed == 0 && stamped && !previous.readError) {
        g_seedComplete = true;
        CommitSeedMarker(marker, stamp, "", previous.replaceList);
    }
    return folder;
}
// A warm-up list begins with vgx_draw.cpp's ListHeader: magic, key sizes and the shader generator's version.
constexpr size_t kListHeaderBytes = 24;
// The bytes of the header read (fewer: a shorter file), or -1 when the file cannot be read.
int ReadListHeader(const char* path, uint8_t (&out)[kListHeaderBytes]) {
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return -1;
    size_t got = 0;
    bool ok = true;
    while (got < kListHeaderBytes) {
        const int n = sceIoRead(f, out + got, static_cast<SceSize>(kListHeaderBytes - got));
        if (n < 0 || static_cast<size_t>(n) > kListHeaderBytes - got) { ok = false; break; }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    const bool closed = sceIoClose(f) == 0;
    return ok && closed ? static_cast<int>(got) : -1;
}
// The card's list was written by another shader generator (or with other key sizes) than the package's: the renderer would start
// it over empty (EnsureShaderList). A header that cannot be read keeps the card's list. (From Producdevity's fork, 9ed7f64.)
bool ListFromOtherGenerator(const char* packaged) {
    uint8_t card[kListHeaderBytes], package[kListHeaderBytes];
    if (ReadListHeader(packaged, package) != static_cast<int>(kListHeaderBytes)) return false;
    const int got = ReadListHeader(kShaderListFile, card);
    return got >= 0 && (got < static_cast<int>(kListHeaderBytes) || std::memcmp(card, package, kListHeaderBytes) != 0);
}
void SeedShaderList() {
    char seed[200];
    std::snprintf(seed, sizeof(seed), "%s/vgx_shaderlist.bin", kSeedDir);
    if (const char* e = std::getenv("GXV_VITA_SEED"); e && std::strcmp(e, "off") == 0) return;
    // The card's own list is kept unless the package is new to this card, or the list is another generator's and the package's
    // compiled shaders are all there: then the package's list (made from a tour of the game) replaces it. Shaders only the card's
    // list knew are still in the compiled-shader cache and are added to the list again when they are met. (A new generator also means
    // a new cache folder, ShaderCacheFolder: no marker there says the package is new, so the card kept a list the renderer then
    // started over empty, and an updated console had no warm-up.)
    SceIoStat info{};
    const int status = sceIoGetstat(kShaderListFile, &info);
    const bool missing = status < 0 && static_cast<uint32_t>(status) == kIoNotFound;
    if (!g_seedIsNew && !missing && (status < 0 || !g_seedComplete || !ListFromOtherGenerator(seed))) return;
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
//
// GXV_CORE0_ALONE=1 (default 0): the game thread has core 0 to itself, so every other thread of the process that is allowed to run
// there is moved to cores 1-2 as well (port::cores::SweepTarget: not the threads pinned to core 0 alone, those are the game's), and
// the basic log says which ones and how many threads stay pinned on core 0.
// stut600: the scan of 0x10000 thread ids (a syscall each, ~10 ms in all on the console) ran whole inside the present of every 600th picture and
// showed as "present 8-12 ms" in the "hitch:" line every 20 s at 30 fps (a 10 ms present is only 26 ms at 60 fps, under GXV_HITCH_MS 40). The
// repeat scans now take kPinSliceIds (32) ids per present (PinLibraryThreadsStep): the same threads found, the same moves and lines, a few presents later.
struct PinScan {
    SceUID next = 0; // 0: no scan running
    int missesInRow = 0, moved = 0, seen = 0, sweptOff = 0, pinnedCore0 = 0;
    bool log = false, census = false;
};
// (review: the scan stops 500 misses after the last thread, so a whole scan is only ~500-1,000 ids: ~10 ms means ~10-20 us an id. 32 ids a
// present keep a slice under ~1 ms; a scan takes ~20-40 presents.)
constexpr int kPinSliceIds = 32;
int g_pinSweepLines = 0;

// Scans up to `budget` ids (0: to the end). True when the scan is finished (the state is then reset).
bool PinScanRun(PinScan& st, int budget) {
    SceKernelThreadInfo info;
    port::cores::Switches layout;
    layout.core0Alone = port_vita_core0_alone && port_vita_core0_alone() != 0;
    if (!st.next) st.next = 0x40010001;
    int left = budget;
    for (; st.next < 0x40020000 && st.missesInRow < 500; ++st.next) {
        if (budget > 0 && left-- == 0) return false;
        const SceUID uid = st.next;
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++st.missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++st.missesInRow; continue; }
        st.missesInRow = 0;
        ++st.seen;
        // (the thread census: threads made by the C++ library start at the lowest user priority, 0xBF)
        if (PORT_RUNTIME_DIAGNOSTICS && st.census) port_log("[vgx-vita] thread census: \"%s\" priority 0x%X (made with 0x%X), core mask 0x%X (made with 0x%X), ran %llu us", info.name,
                             static_cast<unsigned>(info.currentPriority), static_cast<unsigned>(info.initPriority), static_cast<unsigned>(info.currentCpuAffinityMask),
                             static_cast<unsigned>(info.initCpuAffinityMask), static_cast<unsigned long long>(info.runClocks));
        int target = 0;
        bool swept = false;
        if (std::strstr(info.name, "Garbage Collector")) target = GcCoreMask();
        else if (!std::strcmp(info.name, "SceGxmDisplayQueue")) target = kCoreRender;
        else if (layout.core0Alone) swept = (target = port::cores::SweepTarget(layout, static_cast<unsigned>(info.currentCpuAffinityMask))) != 0;
        if (layout.core0Alone && info.currentCpuAffinityMask == static_cast<SceUInt32>(kCoreGame)) ++st.pinnedCore0;
        if (target && info.currentCpuAffinityMask != static_cast<SceUInt32>(target)) {
            const int res = sceKernelChangeThreadCpuAffinityMask(uid, target);
            ++st.moved;
            if (swept && res >= 0) ++st.sweptOff;
            if (swept && g_pinSweepLines < 24) {
                ++g_pinSweepLines;
                GXV_BASIC_LOG("threads: core 0 alone: \"%s\" (priority 0x%X, core mask 0x%X) moved to 0x%X: %s", info.name, static_cast<unsigned>(info.currentPriority),
                              static_cast<unsigned>(info.currentCpuAffinityMask), static_cast<unsigned>(target), res >= 0 ? "ok" : "refused");
            }
            port_log("[vgx-vita] thread \"%s\" (priority 0x%X) moved to core mask 0x%X: %s", info.name, static_cast<unsigned>(info.currentPriority),
                     static_cast<unsigned>(target), res >= 0 ? "ok" : "refused");
        }
    }
    if (layout.core0Alone && (st.log || st.sweptOff))
        GXV_BASIC_LOG("threads: core 0 alone: %d threads seen, %d moved off core 0 now, %d left pinned to core 0 alone (the game's)", st.seen, st.sweptOff, st.pinnedCore0);
    if (st.log) port_log("[vgx-vita] library threads: %d threads seen, %d moved%s", st.seen, st.moved, st.seen == 0 ? " (none seen: expected in the emulator)" : "");
    st = PinScan();
    return true;
}

void PinLibraryThreads(bool log, bool census = false) {
    PinScan st;
    st.log = log;
    st.census = census;
    PinScanRun(st, 0);
}

// The repeat scans after present 120: started at presents 600, 1200, ..., a slice at every present until done (render thread).
void PinLibraryThreadsStep(uint64_t presented) {
    static PinScan st;
    if (!st.next) {
        if (presented % 600 != 0) return;
        st.census = presented == 9000;
    }
    PinScanRun(st, kPinSliceIds);
}

// Who used the CPU since the last call: the threads that ran for at least 40 ms, with their core masks, and the sum for each core.
// The game thread's turn takes longer by the clock than it computes (the Observatory on 18 Oct: 16.6 ms a turn, 12.8 ms of CPU); this
// says who else is on its core (mask 0x10000) while it waits. Called every 10 s by the status block in vita/vgx-game/entry.cpp.
// loglines44 (test only): GXV_THREADS_LOG=1 (unset by default: nothing changes) makes the report in the player build too and writes its lines to the basic
// log (ux0:data/smg/log.txt), where port_log lines never arrive: the compile worker's console arms read the "threads:" lines for its core mask.
bool ThreadsBasicLog() { static const bool on = [] { const char* e = std::getenv("GXV_THREADS_LOG"); return e && *e && std::atoi(e) == 1; }(); return on; }
void ThreadCpuReport() {
    const bool basic = ThreadsBasicLog();
    if (!PORT_RUNTIME_DIAGNOSTICS && !basic) return;
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
        if (basic) GXV_BASIC_LOG("threads: CPU used in the last %.1f s (ms): core 0 %llu, core 1 %llu, core 2 %llu, any core %llu", (nowUs - lastUs) / 1e6, core[0], core[1], core[2], core[3]);
        else port_log("threads: CPU used in the last %.1f s (ms): core 0 %llu, core 1 %llu, core 2 %llu, any core %llu", (nowUs - lastUs) / 1e6, core[0], core[1], core[2], core[3]);
        for (int i = 0; i < rowCount; ++i) {
            if (basic) GXV_BASIC_LOG("threads:   \"%s\" id %08X priority 0x%X core mask 0x%X: %llu ms", rows[i].name, static_cast<unsigned>(rows[i].uid), rows[i].prio, rows[i].mask, rows[i].ms);
            else port_log("threads:   \"%s\" id %08X priority 0x%X core mask 0x%X: %llu ms", rows[i].name, static_cast<unsigned>(rows[i].uid), rows[i].prio, rows[i].mask, rows[i].ms);
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
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
std::mutex g_shaderPackMu;
std::atomic<bool> g_shaderPackDone{false};
void EnsureShaderPackLoaded() {
    if (g_shaderPackDone.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_shaderPackMu);
    if (g_shaderPackDone.load(std::memory_order_relaxed)) return;
    PrepareShaderPack(g_shaderCachePath);
    g_shaderPackDone.store(true, std::memory_order_release);
}
#else
void EnsureShaderPackLoaded() {}
#endif
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
    // Register the RAM-pack driver hook and miss-log table before EnsureThread so the render thread
    // sees immutable state; heavy stages.pack/user.pack loading overlaps behind vglInitWithCustomSizes.
    port_vgx_shader_cache_reader_driver_v1();
    {   // (packrefill25) GXV_PACK_MISSLOG=1: one "pack-miss:" line per distinct key the packs did not answer, a per-minute "pack:" summary. Also with the pack off.
        long missLog = 0;
        EnvLong("GXV_PACK_MISSLOG", missLog, 0, 1);
        if (missLog && g_missLog.Init())
            port_log_summary("pack: GXV_PACK_MISSLOG=1: one line per first pack miss (\"pack-miss: <v|f>/<HH>/<HASH>.gxp stage=.. play=.. t=.. n=..\"), a summary every 60 s");
    }
#endif
    // Settings a console needs, unless env.txt chose otherwise (the renderer reads GXV_TEXBUDGET
    // and GXV_SHADERLIST itself).
    if (!std::getenv("GXV_SHADERLIST") && !std::getenv("GXV_NO_SHADERLIST")) {
        SeedShaderList();
        vgx::SetShaderListFile(kShaderListFile);
    }
    if (!std::getenv("GXV_TEXBUDGET")) vgx::SetTextureBudget(kTextureBudget);
    // The renderer's record of each stage's programs (GXV_STAGEWARM, vgx_backend_gl.cpp): read at its first frame, appended in quiet
    // moments, used to link a stage's programs while it loads. env.txt may name another file; GXV_STAGEWARM=0 turns it all off.
    setenv("GXV_STAGEWARM_FILE", kStageWarmFile, 0);
    {
        // The stage list that came with the package (app0:shadercache/vgx_stagewarm.bin, read-only; the renderer merges it with the card's:
        // GXV_STAGEWARM_PKG=0 = card only). It names shaders by the generator's key hashes, so it is offered only to a build whose key is
        // the package's: the package's seed folder for this key (as SeedShaderCache needs it) must be there, else the list is stale.
        const char* const folderKey = ShaderCacheKey(); // (hitch33 10)
        char keyDir[200], pkgList[200];
        std::snprintf(keyDir, sizeof keyDir, "%s%s", kSeedDir, folderKey ? folderKey : "/none");
        std::snprintf(pkgList, sizeof pkgList, "%s/vgx_stagewarm.bin", kSeedDir);
        const char* const seedMode = std::getenv("GXV_VITA_SEED");
        if (!(seedMode && std::strcmp(seedMode, "off") == 0) && Exists(keyDir) && Exists(pkgList)) {
            setenv("GXV_STAGEWARM_PKG_FILE", pkgList, 0);
            port_log("[vgx-vita] stage warm-up list in the package (%s) for this key: merged with the card's", pkgList);
        } else {
            port_log("[vgx-vita] no stage warm-up list in the package for this key: the card's own is used");
        }
    }
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
        // HOLD cannot wait for a warm-up whose zero budget is only released after that same wait.
        if (vgx::bootguard::Enabled() && std::getenv("GXV_WARM_HOLD") && std::getenv("GXV_WARM_HOLD")[0] == '1') afterTitle = 0;
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
    // Both calls are made and both results logged (the basic log too): a status call that fails is not proof that the file is
    // missing, and the codes say which of the two was refused and why. Only when neither call reaches either file is it "missing".
    const vgx::bootwatch::FileProbe compilerA = ProbeFile("ur0:data/libshacccg.suprx"), compilerB = ProbeFile("ur0:data/external/libshacccg.suprx");
    GXV_BASIC_LOG("[vgx-vita] shader compiler module: ur0:data/libshacccg.suprx stat 0x%08X open 0x%08X, ur0:data/external/libshacccg.suprx stat 0x%08X open 0x%08X",
                  static_cast<unsigned>(compilerA.stat), static_cast<unsigned>(compilerA.open), static_cast<unsigned>(compilerB.stat),
                  static_cast<unsigned>(compilerB.open));
    if (vgx::bootwatch::CompilerMissing(compilerA, compilerB)) {
        vgx::bootwatch::FormatCompilerMissing(error, errorSize, compilerA, compilerB);
        return false;
    }
    if ((compilerA.stat >= 0) != (compilerA.open >= 0) || (compilerB.stat >= 0) != (compilerB.open >= 0))
        GXV_BASIC_LOG("[vgx-vita] the status call and the open disagree about a shader compiler module: going on (the load decides)");

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
    // A larger parameter buffer (GXV_VITA_PARAM_KIB, below: the GPU-time test of 5 Nov for partial renders in the lit Observatory) moves the
    // reserve with it (buffer + 2 MiB) unless GXV_VITA_CDRAM_RESERVE_MIB says otherwise.
    long cdramReserveMib = 18;
    {
        long pbKib = 0;
        EnvLong("GXV_VITA_PARAM_KIB", pbKib, 0, 65536);
        if (pbKib > 16 * 1024) cdramReserveMib = (pbKib + 1023) / 1024 + 2;
    }
    EnvLong("GXV_VITA_CDRAM_RESERVE_MIB", cdramReserveMib, 1, 100);
    int64_t cdram = static_cast<int64_t>(free.size_cdram) - cdramReserveMib * 1024 * 1024;
    cdram &= ~static_cast<int64_t>(256 * 1024 - 1);
    if (cdram < 0) cdram = 0;
    int64_t phycont = free.size_phycont > 0 ? free.size_phycont : 0;
    {
        // The hardware movie decoder (GXV_THPHW=1) needs physically contiguous memory of its own (up to 3 blocks of 2 MiB, allocated when the first
        // movie starts). vitaGL is given ALL the free phycont, so the allocation failed with 0x80024302 (SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE).
        // With GXV_THPHW=1, leave GXV_THPHW_PHYCONT_MIB (default 8, 0 = none) out of vitaGL's request. Default (GXV_THPHW unset/0): as before.
        const char* hw = std::getenv("GXV_THPHW");
        if (hw && std::atoi(hw) != 0) {
            long keepMib = 8;
            EnvLong("GXV_THPHW_PHYCONT_MIB", keepMib, 0, 64);
            const int64_t keep = keepMib * 1024 * 1024;
            phycont = phycont > keep ? phycont - keep : 0;
            if (port_log_summary) port_log_summary("[vgx-vita] GXV_THPHW=1: %ld MiB of physically contiguous memory left out of vitaGL's pools for the movie decoder (free %u KiB, vitaGL asked %u KiB)",
                             keepMib, static_cast<unsigned>(free.size_phycont / 1024), static_cast<unsigned>(phycont / 1024));
        }
    }
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
    // GXV_FOG_BIND (issue 6; vgx_shadergen.h FogBindMode): the vertex-fog varying on a fixed semantic. Its NAME changes with the mode
    // ("v_fog" unbound / "v_fogc" FOG / "v_fogt<n>" TEXCOORD n), so the GLSL texts of the fog shaders (and only those) change, and no
    // compiled file made with the old history-dependent slot can be found by vitaGL's text-hash lookup: the cache folder key
    // (kHostShaderAbi) and every other shader of the package stay valid. Both bound names are registered whatever the mode (a name
    // no shader declares is never looked up), so the binding table does not depend on the switch.
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_fogc"), 0, VGL_TYPE_FOG);
    if (vgx::FogBindMode() == 2)
        vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>(vgx::FogVaryingName()), vgx::FogBindSlot(), VGL_TYPE_TEXCOORD);
    if (vgx::FogBindMode() == 2)
        GXV_BASIC_LOG("[vgx-vita] fog varying (GXV_FOG_BIND=2): %s on TEXCOORD%d", vgx::FogVaryingName(), vgx::FogBindSlot());
    else
        GXV_BASIC_LOG("[vgx-vita] fog varying (GXV_FOG_BIND=%ld): %s on %s", vgx::FogBindMode(), vgx::FogVaryingName(),
                      vgx::FogBindMode() == 1 ? "FOG" : "the first free TEXCOORD (unbound, as 2.0-2.2)");

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
    // GXV_VITA_USSE_CDRAM=1 (default 0, console A/B; from the Strikers port's 1.3 MSC_VITA_USSE_CDRAM): the code of every shader (the
    // patcher's vertex and fragment code pools and the fragment USSE ring) in video memory, the GPU's own memory, instead of main memory:
    // the one thing the GPU reads for every vertex and every pixel that vitaGL places in main memory by default. The code pools
    // ((GXV_VITA_PATCHER_MIB + 1) / 2 MiB each, 4 + 4 at the default) then come out of the video memory textures and render targets
    // use. The picture is the same. Only before vglInit (vitaGL maps the pools there).
    {
        long usseVram = 0;
        EnvLong("GXV_VITA_USSE_CDRAM", usseVram, 0, 1);
        if (usseVram && vglUseVramForUSSE) {
            vglUseVramForUSSE(GL_TRUE);
            port_log("[vgx-vita] GXV_VITA_USSE_CDRAM=1: shader code pools and the fragment USSE ring in video memory");
        } else if (usseVram) {
            port_log("[vgx-vita] GXV_VITA_USSE_CDRAM=1 ignored: this vitaGL has no vglUseVramForUSSE");
        }
    }
    // GXV_VGL_MASKKEEP=1 (default 0, console A/B; vitaGL tools/vitagl/patches/0018 built with MASK_KEEP): a framebuffer-object scene
    // that does not load its depth (GXV_DEPTH_SKIP / the depth plan) and begins with the scissor test off starts with its scissor mask set
    // by the depth surface's background value, so vitaGL leaves out the full-target mask quad it draws at the start of such a scene
    // (every texture / pass / copy scene, the EFB scenes the depth plan begins unloaded). Same picture if the console treats the
    // background mask as the splash screen relies on; "vitaGL MASK_KEEP" in the log (diagnostic builds) counts the quads left out.
    {
        long maskKeep = 0;
        EnvLong("GXV_VGL_MASKKEEP", maskKeep, 0, 1);
        if (maskKeep && vglSetMaskKeep) {
            vglSetMaskKeep(GL_TRUE);
            port_log("[vgx-vita] GXV_VGL_MASKKEEP=1: unloaded framebuffer-object scenes start with their scissor mask set (no full-target mask quad)");
            // GXV_VGL_MASKKEEP_DISPLAY=1 (default 1, with GXV_VGL_MASKKEEP=1; vitaGL patches/0022): the same for the present's own scene (the
            // blit of the finished picture: PresentDisplayCopy hints depth load = false, scissor test off), which otherwise draws the
            // full-target mask quad (960x544, depth/stencil only) before the picture. Exact under the same condition as MASKKEEP.
            long maskKeepDisplay = 1;
            EnvLong("GXV_VGL_MASKKEEP_DISPLAY", maskKeepDisplay, 0, 1);
            if (maskKeepDisplay && vglSetMaskKeepDisplay) {
                vglSetMaskKeepDisplay(GL_TRUE);
                port_log("[vgx-vita] GXV_VGL_MASKKEEP_DISPLAY=1: the display's scene starts with its scissor mask set (no full-target mask quad)");
            }
        } else if (maskKeep) {
            port_log("[vgx-vita] GXV_VGL_MASKKEEP=1 ignored: this vitaGL has no vglSetMaskKeep (patches/0018, MASK_KEEP)");
        }
    }
    // GXV_VGL_LOADFROM=1 (default 0, console A/B; vitaGL tools/vitagl/patches/0019 built with COLOR_LOAD_FROM): after GXV_EFBSWAP(2)
    // hands the EFB's colour buffer to a copy texture, the EFB's next scene loads its colour tiles from that buffer (the colour
    // surface's background texture) instead of the back end drawing it back with a full-screen quad. Same picture if the console
    // loads a scene's colour from the background texture as GXM's surface layout says; the back end's "efbswap2:" line and
    // "vitaGL COLOR_LOAD_FROM" (diagnostic builds) count them.
    {
        long loadFrom = 0;
        EnvLong("GXV_VGL_LOADFROM", loadFrom, 0, 1);
        if (loadFrom && vglColorLoadFrom) {
            vgx::backend::SetColorLoadHook([](unsigned tex) { return vglColorLoadFrom(static_cast<GLuint>(tex)) == GL_TRUE; });
            port_log("[vgx-vita] GXV_VGL_LOADFROM=1: a swapped EFB colour buffer is loaded by the next EFB scene (no drawing-back quad)");
        } else if (loadFrom) {
            port_log("[vgx-vita] GXV_VGL_LOADFROM=1 ignored: this vitaGL has no vglColorLoadFrom (patches/0019, COLOR_LOAD_FROM)");
        }
    }
    // GXV_COLOR_LOADSKIP=<bits> (default 0, console A/B; vitaGL tools/vitagl/patches/0021): a scene that begins with a clear of all its
    // colour loads its colour tiles from a tiny texture instead of 2.1 MB / 1.2 MB of its own old content (vgx_backend_gl.cpp
    // ColorSkipMode: 1 = the EFB's scenes, 2 = pass / effect targets, 4 = the display's scene, this file's PresentDisplayCopy, 8 = copy scenes).
    // Bit 16 (scenes23; vitaGL patch 0028, VP26_SKIP_COLOR): the scenes of bits 1 and 2 (they begin with a clear) start with the clear's own
    // colour as their tile load instead of zeros: exactly the clear's picture whatever its quad reaches. Without vp26 bits 1 and 2 are then
    // left alone (no cheap load for them), the other bits as before.
    {
        long skip = 0;
        EnvLong("GXV_COLOR_LOADSKIP", skip, 0, 31);
        if (skip && vglColorLoadSkip) {
            vgx::backend::SetColorSkipHook([](bool arm) { return vglColorLoadSkip(arm ? GL_TRUE : GL_FALSE) == GL_TRUE; });
            if ((skip & 16) && vglColorLoadSkipColor)
                vgx::backend::SetColorSkipColorHook([](bool arm, uint32_t rgba) { return vglColorLoadSkipColor(arm ? GL_TRUE : GL_FALSE, rgba) == GL_TRUE; });
            port_log("[vgx-vita] GXV_COLOR_LOADSKIP=%ld: scenes that begin with a full clear load a tiny colour texture (bits 1 EFB, 2 passes, 4 display, 8 copy "
                     "scenes; 16: bits 1 and 2 with the clear's own colour%s)", skip, (skip & 16) && !vglColorLoadSkipColor ? " - NOT in this vitaGL (vp26): bits 1, 2 off" : "");
        } else if (skip) {
            port_log("[vgx-vita] GXV_COLOR_LOADSKIP=%ld ignored: this vitaGL has no vglColorLoadSkip (patches/0021, COLOR_LOAD_FROM)", skip);
        }
    }
    // GXV_VGL_VALIDREGION=1 (default 0; vitaGL patch 0028, VP26_VALID_REGION; scenes23): the GXV_HOIST_GRID pass's scene is begun with a GXM valid
    // region (0,0)-(xMax,yMax) around the rectangle it draws: only those tiles are loaded, shaded and stored. A region GXM refuses is logged
    // by vitaGL once and the whole target is used from then on.
    {
        long vr = 0;
        EnvLong("GXV_VGL_VALIDREGION", vr, 0, 1);
        if (vr && vglSetNextSceneValidRegion) {
            vgx::backend::SetValidRegionHook([](int, int, int x1, int y1) { return vglSetNextSceneValidRegion(x1, y1) == GL_TRUE; });
            port_log("[vgx-vita] GXV_VGL_VALIDREGION=1: the grid hoist's scene is begun with a valid region");
        } else if (vr) {
            port_log("[vgx-vita] GXV_VGL_VALIDREGION=1 ignored: this vitaGL has no vglSetNextSceneValidRegion (patch 0028, vp26)");
        }
    }
    vglSetupShaderPatcher(static_cast<unsigned>(patcherMib) * 1024u * 1024u, static_cast<unsigned>((patcherMib + 1) / 2) * 1024u * 1024u,
                          static_cast<unsigned>((patcherMib + 1) / 2) * 1024u * 1024u);
    vglSetupGarbageCollector(kGcPriority, GcCoreMask());
    // sceGxm buffers, all at vitaGL's defaults unless env.txt asks: triple buffering lets a frame
    // go on while the last one waits for the display (a frame never stalls on the swap); the
    // parameter and VDM ring buffers bound how much of a dense scene can be in flight (a scene
    // that does not fit is drawn in several passes). KiB values; 0 = the default.
    long tripleBuffering = 0, paramKib = 0, vdmKib = 0;
    EnvLong("GXV_VITA_TRIPLE", tripleBuffering, 0, 1);
    EnvLong("GXV_VITA_PARAM_KIB", paramKib, 0, 65536);
    EnvLong("GXV_VITA_VDM_KIB", vdmKib, 0, 65536);
    if (tripleBuffering) vglUseTripleBuffering(GL_TRUE);
    // GXV_VITA_DISPLAY_BUFFERS=<2-4> (default 0 = vitaGL's three): more display buffers for GXV_PRESENT_SYNC=2. The display callback holds a
    // finished picture until its slot (the latency L = 3-6 vblanks after its turn began), and while it holds, the buffer is neither shown
    // nor free: with three buffers (displayQueueMaxPendingCount 2) the third picture's sceGxmDisplayQueueAddEntry blocks the render thread
    // and its first scene waits on the buffer's sync object, so the hold, not the work, can set the picture's "ready after" (Good Egg r107:
    // 4 vblanks natural against the lowest latency 5, a 16 ms wait in the callback for 262 of 274 pictures). A fourth buffer costs 2 MiB of
    // CDRAM and takes a quarter off each frame's slice of vitaGL's circular pool (the pool is divided by the buffer count): on the console
    // watch the "present:" line (ready after, waits) and the back end's wall against its CPU in "workload: backend-execute".
    long displayBuffers = 0;
    EnvLong("GXV_VITA_DISPLAY_BUFFERS", displayBuffers, 0, 4);
    if (displayBuffers >= 2) {
        vglSetDisplayBufferCount(static_cast<int>(displayBuffers));
        GXV_BASIC_LOG("[vgx-vita] GXV_VITA_DISPLAY_BUFFERS=%ld: %ld display buffers", displayBuffers, displayBuffers);
    }
    if (paramKib) {
        vglSetParamBufferSize(static_cast<uint32_t>(paramKib) * 1024u);
        GXV_BASIC_LOG("[vgx-vita] GXV_VITA_PARAM_KIB=%ld: sceGxm parameter buffer %ld KiB (vitaGL's default 16384)", paramKib, paramKib);
    }
    if (vdmKib) vglSetVDMBufferSize(static_cast<uint32_t>(vdmKib) * 1024u);
    // bowser24 (GXV_VITA_VERTEX_KIB / _FRAGMENT_KIB / _USSE_KIB; 0 = vitaGL's defaults 2048 / 512 / 16): the other command rings of the sceGxm context
    // (the outside review's "command-ring stalls": THUG2 enlarges all of them). A ring that is too small makes sceGxmReserve*/Draw wait for the GPU
    // to drain it; the back end's be-split lines ("reserve", "draw", "scene begin/end") show whether any wait is there before these are touched.
    long vertexKib = 0, fragmentKib = 0, usseKib = 0;
    EnvLong("GXV_VITA_VERTEX_KIB", vertexKib, 0, 16384);
    EnvLong("GXV_VITA_FRAGMENT_KIB", fragmentKib, 0, 8192);
    EnvLong("GXV_VITA_USSE_KIB", usseKib, 0, 1024);
    if (vertexKib) vglSetVertexBufferSize(static_cast<uint32_t>(vertexKib) * 1024u);
    if (fragmentKib) vglSetFragmentBufferSize(static_cast<uint32_t>(fragmentKib) * 1024u);
    if (usseKib) vglSetUSSEBufferSize(static_cast<uint32_t>(usseKib) * 1024u);
    if (vertexKib || fragmentKib || usseKib)
        port_log("[vgx-vita] sceGxm rings: vertex %ld KiB, fragment %ld KiB, USSE %ld KiB (0 = vitaGL's default)", vertexKib, fragmentKib, usseKib);
    // A vitaGL built with HAVE_SCRATCH_MEMORY puts GL_DYNAMIC_DRAW and GL_STREAM_DRAW buffers in the
    // per-picture circular pool by default. The static geometry is given the DYNAMIC hint (see below) and
    // must outlive the frame, so only the stream buffers (the frame's vertex chunks and indices) may be
    // scratch. Without that build option the call does nothing.
    // GXV_VITA_SCRATCH=1 (default 0): the stream buffers come from that pool. Each is given its data again in every back-end
    // frame before its draws, and the pool of a picture is written again only after the display queue has released the
    // picture three swaps before (tools/vitagl/README.md); the gain is two heap allocations and two collector frees per frame.
    long scratchStream = 0;
    EnvLong("GXV_VITA_SCRATCH", scratchStream, 0, 1);
    vglSetupScratchMemory(GL_FALSE, scratchStream ? GL_TRUE : GL_FALSE);
    port_log("[vgx-vita] stream buffers: %s", scratchStream ? "in vitaGL's per-picture pool (library built with HAVE_SCRATCH_MEMORY only)" : "ordinary allocations");
    // (vitaGL's own default is three display buffers; GXV_VITA_TRIPLE=0 leaves that alone, it does not make two)
    port_log("[vgx-vita] sceGxm buffers: %s, parameter buffer %ld KiB, VDM ring %ld KiB (0 = vitaGL's default)",
             tripleBuffering ? "triple buffering asked for" : "vitaGL's default display buffers", paramKib, vdmKib);
    port_log("[vgx-vita] render targets: %ld scene(s) per frame; shader patcher pools %ld / %ld / %ld MiB; collector on core mask 0x%X", fboScenes, patcherMib,
             (patcherMib + 1) / 2, (patcherMib + 1) / 2, static_cast<unsigned>(GcCoreMask()));

    PrepareFrontEnd();  // (normally done already, on the game's thread, before this one was started)
    vglSetShaderCachePath(g_shaderCachePath);
    splash::Enter(splash::kGraphics);
    // The game paces itself to its own 60 Hz retrace clock; a second wait for vertical blank in
    // the swap would make the two beat against each other.
    vglWaitVblankStart(GL_FALSE);
    if (port_vgx_pace_divisor()) vglSetDisplayCallback(PaceDisplay);
    else if (port_vgx_present_sync_mode()) {
        g_syncOn = true;
        vglSetDisplayCallback(SyncDisplay);
        port_log("[vgx-vita] GXV_PRESENT_SYNC=%d: %s", port_vgx_present_sync_mode(),
                 port_vgx_present_sync_mode() == 2 ? "pictures held to the vblank of their game turn (adaptive latency)" : "picture timing measured only");
    }

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
    // g30 Wave 7: temporarily lift the render thread above Core 1's 0x4F audio worker during vglInitWithCustomSizes
    // (~2 s GPU/GXM bring-up) so boot audio setup does not preempt vitaGL init for +1.2-1.36 s.
    GLboolean fellBack = GL_FALSE;
    {
        struct ScopedThreadPrio {
            int prev = -1;
            explicit ScopedThreadPrio(int prio) {
                const int cur = sceKernelGetThreadCurrentPriority();
                if (cur > prio && cur >= 0 && sceKernelChangeThreadPriority(0, prio) >= 0) prev = cur;
            }
            ~ScopedThreadPrio() {
                if (prev >= 0) sceKernelChangeThreadPriority(0, prev);
            }
        } prioBoost(0x4E);
        fellBack = vglInitWithCustomSizes(0, kScreenW, kScreenH, static_cast<int>(ram), static_cast<int>(cdram), static_cast<int>(phycont), 0,
                                          msaaMode);
    }
    if (fellBack) port_log("[vgx-vita] vitaGL could not set %dx%d and fell back to a smaller display mode", kScreenW, kScreenH);
    // (strikersrev25) The back end reads GXV_MSAA itself (vgx_backend_gl.cpp MsaaProbe: whether the per-framebuffer rungs exist; GlobalSamples: the
    // depth-memory estimates). What vitaGL was started with is the truth: written back when it is not what the environment says (a later start-up guard
    // that lowers `msaa` above then reaches them too). A no-op today: nothing lowers it.
    {
        const char* const want = msaaMode == SCE_GXM_MULTISAMPLE_4X ? "4" : (msaaMode == SCE_GXM_MULTISAMPLE_2X ? "2" : "0");
        const char* const have = std::getenv("GXV_MSAA");
        if (!have || std::strcmp(have, want) != 0) setenv("GXV_MSAA", want, 1);
    }
    // GXV_ANISO=0|2|3 (default 0, off): the Strikers port's experimental anisotropic switch, in a vitaGL built with ANISO_SWITCH
    // (tools/vitagl/build-gpu-timers.sh): trilinear textures get minification filter 2 or 3, believed to be the hardware's
    // anisotropic modes. Untested on a console (also in Strikers). For good from start-up; the automatic quality's own top step is
    // GXV_AUTOQ_ANISO (below, it switches the same filter at run time).
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
        // The automatic quality's top step (GXV_AUTOQ_ANISO, vgx_autoq.h) calls the same switch at run time, through the back end's hook.
        if (vglSetAnisotropicFilter) vgx::backend::SetAnisotropicHook(&vglSetAnisotropicFilter);
    }
    // vitaGL vp34 wins (patches 0040/0041/0043, ported from Strikers real-Vita proven defaults; inert on older libvitaGL.a via weak symbols):
    //   GXV_VGL_FBO_KEEP=0|1      (default 1): detaching colour tex (id 0) keeps FBO render target + depth surface
    //   GXV_VGL_RT_POOL=<0..48>   (default 16): LPDDR render-target pool slot cap (0 = off); _KIB (default 8192) caps driver KiB
    //   GXV_VGL_WRCOPY=0..3       (default 3): write-combining copies into uncached stream/uniform pools (3 = DMA >=8KiB + NEON fallback)
    //   GXV_VGL_WRCOPY_FB=0|1     (default 1): mode 3 fallback for <8KiB or >=0x81000000 (1 = NEON 64B, 0 = sceClibMemcpy)
    //   GXV_VGL_TEXCOPY_DMA=0..2  (default 1): DMA engine for contiguous >=8KiB level-0 texture uploads (2 = verify)
    {
        long fboKeep = 1;
        EnvLong("GXV_VGL_FBO_KEEP", fboKeep, 0, 1);
        if (vglSetFramebufferDetachKeep) vglSetFramebufferDetachKeep(fboKeep ? GL_TRUE : GL_FALSE);

        long rtPool = 16, rtPoolKib = 8192;
        EnvLong("GXV_VGL_RT_POOL", rtPool, 0, 48);
        EnvLong("GXV_VGL_RT_POOL_KIB", rtPoolKib, 0, 32768);
        if (vglSetRenderTargetPool) vglSetRenderTargetPool(static_cast<uint32_t>(rtPool), static_cast<uint32_t>(rtPoolKib));

        long wrCopy = 3, wrCopyFb = 1;
        EnvLong("GXV_VGL_WRCOPY", wrCopy, 0, 3);
        EnvLong("GXV_VGL_WRCOPY_FB", wrCopyFb, 0, 1);
        if (vglSetWriteCopyFallback) vglSetWriteCopyFallback(static_cast<int>(wrCopyFb));
        if (vglSetWriteCopy) vglSetWriteCopy(static_cast<int>(wrCopy));

        long texCopyDma = 1;
        EnvLong("GXV_VGL_TEXCOPY_DMA", texCopyDma, 0, 2);
        if (vglSetTextureCopyDma) vglSetTextureCopyDma(static_cast<int>(texCopyDma));

        if (vglSetFramebufferDetachKeep || vglSetRenderTargetPool || vglSetWriteCopy || vglSetTextureCopyDma) {
            port_log("[vgx-vita] vp34 vitaGL: fboKeep=%ld(%s) rtPool=%ld/%ldKiB(%s) wrcopy=%ld/fb=%ld(%s) texDma=%ld(%s)",
                     fboKeep, vglSetFramebufferDetachKeep ? (fboKeep ? "on" : "off") : "absent",
                     rtPool, rtPoolKib, vglSetRenderTargetPool ? (rtPool ? "on" : "off") : "absent",
                     wrCopy, wrCopyFb, vglSetWriteCopy ? (wrCopy ? "on" : "off") : "absent",
                     texCopyDma, vglSetTextureCopyDma ? (texCopyDma ? "on" : "off") : "absent");
        }
    }
    if (vglMemTotal(VGL_MEM_VRAM) == 0 && vglMemTotal(VGL_MEM_RAM) == 0) {
        std::snprintf(error, errorSize, "vitaGL started without any memory pool (vglInit gave none).\nRestart the Vita. If this repeats, send log.txt (the [vitaGL] lines).");
        return false;
    }
    g_vglPoolsUp = true;
    LogMemory("after vitaGL");
    // GXV_VITA_RTMEM_LOG=1 (default 0, log only): the driver memory sceGxm asks for one render target of a few sizes at 1 and at
    // 8 scenes per frame (a pure query; vitaGL passes driverMemBlock -1, so sceGxm allocates it itself, outside vitaGL's pools).
    // The difference times the number of render targets (display + EFB + pooled objects) is what GXV_VITA_FBO_SCENES=8 costs.
    {
        long rtMemLog = 0;
        EnvLong("GXV_VITA_RTMEM_LOG", rtMemLog, 0, 1);
        if (rtMemLog) {
            static const int kSizes[][2] = {{kScreenW, kScreenH}, {640, 456}, {320, 228}, {160, 160}};
            for (const auto& sz : kSizes) {
                SceGxmRenderTargetParams rtp;
                std::memset(&rtp, 0, sizeof(rtp));
                rtp.width = static_cast<uint16_t>(sz[0]);
                rtp.height = static_cast<uint16_t>(sz[1]);
                rtp.multisampleMode = msaaMode;
                rtp.driverMemBlock = -1;
                unsigned one = 0, eight = 0;
                rtp.scenesPerFrame = 1;
                const int r1 = sceGxmGetRenderTargetMemSize(&rtp, &one);
                rtp.scenesPerFrame = 8;
                const int r8 = sceGxmGetRenderTargetMemSize(&rtp, &eight);
                port_log("[vgx-vita] render target %dx%d driver memory: %u bytes at 1 scene per frame (0x%X), %u bytes at 8 (0x%X)", sz[0], sz[1], one,
                         static_cast<unsigned>(r1), eight, static_cast<unsigned>(r8));
            }
        }
    }
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
            std::snprintf(error, errorSize, "vitaGL could not allocate its video memory pool (%u KiB asked, %u KiB free).\nRestart the Vita; close other applications. See GXV_VITA_CDRAM_RESERVE_MIB.",
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
            NoteSwap(0);
            vglSwapBuffers(MpDialogSwapMode());  // MP3: GL_TRUE while a multiplayer system dialog is up
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

    // vitaGL loads the shader compiler module during its own start-up and says nothing when that fails (a log line at best, and not the
    // result code): ask again here, name both results, and stop with a message that names the step. A load that works now (the first
    // had failed) is kept: vitaGL asks vitaShaRK again at its first compile and finds it ready.
    if (&is_shark_online != nullptr && !is_shark_online && shark_init) {
        const int viaData = shark_init(nullptr);  // vitaGL's own first path: ur0:/data/libshacccg.suprx
        const int viaExternal = viaData >= 0 ? 0 : shark_init("ur0:data/external/libshacccg.suprx");
        GXV_BASIC_LOG("[vgx-vita] shader compiler load (vitaGL's attempt during its start-up had not worked): ur0:/data/libshacccg.suprx 0x%08X, "
                      "ur0:data/external/libshacccg.suprx 0x%08X (0 = loaded)", static_cast<unsigned>(viaData), static_cast<unsigned>(viaExternal));
        if (viaData < 0 && viaExternal < 0) {
            vgx::bootwatch::FormatCompilerLoadFailure(error, errorSize, viaData, viaExternal);
            return false;
        }
    }
    // Ensure overlapped stages.pack / user.pack loading has completed before the first shader compile/lookup.
    EnsureShaderPackLoaded();
    char testError[400] = "";
    bool testOk = ShaderCompilerSelfTest(testError, sizeof(testError));
    if (!testOk) {
        GXV_BASIC_LOG("[vgx-vita] shader compiler self-test failed: %s", testError);
        // GXV_SELFTEST_RETRY=0 stops at the first failure. Failure path only: a start that passes the test never gets here.
        if (vgx::EnvInt("GXV_SELFTEST_RETRY", 1) != 0) {
            const unsigned removed = RemoveSelfTestCacheFiles();
            testError[0] = 0;
            testOk = ShaderCompilerSelfTest(testError, sizeof(testError));
            GXV_BASIC_LOG("[vgx-vita] shader compiler self-test again after removing %u cached file(s): %s", removed, testOk ? "passed" : testError);
        }
    }
    if (!testOk) {
        char reason[120];
        vgx::bootwatch::FirstLine(reason, sizeof(reason), testError, 56);
        std::snprintf(error, errorSize, "Shader compile test failed: %.56s\nDelete ux0:data/smg/shadercache and restart; if it repeats, send log.txt.", reason);
        return false;
    }
    FogSemanticSelfTest();
    GxmProbe(); // (GXV_GXM_PROBE=1, diagnostic: what GXM's colour load really reads)
    // Static geometry goes to vitaGL's MAIN-memory pool (hint GL_DYNAMIC_DRAW): less video memory.
    setenv("GXV_STATIC_USAGE", "dynamic", 0);

    // GXV_RENDER_SIZE (read by the glue) sets the back end's picture size; here the back end
    // is initialised by vgx_galaxy.cpp after this returns.
    vgx::backend::SetPresentHook(PresentDisplayCopy);
    vgx::backend::SetProgressDisplay(0, kScreenW, kScreenH, ShowProgressBar);
    vgx::backend::SetHudNativeHost(kScreenW, kScreenH); // (GXV_HUD_NATIVE: this host's present hook has the two halves)
    vgx::backend::SetHudNativeDepth(true);              // (the display's own depth/stencil surface of vitaGL's default framebuffer: no allocation here)
    // The bar is for start-up (the warm-up list's work in the first frames) and for a long wait; a
    // shader met in the middle of play costs a moment, not a flash of the bar.
    vgx::backend::SetProgressPolicy(90, 1500);
    PinLibraryThreads(true);
    port_log("[vgx-vita] t=%u ms vitaGL ready; renderer sources: %s", NowMs(), GXV_COMMIT);
    // MP3: from here the swap can show a CommonDialog (multiplayer's ad hoc NetCheck, the keyboard): let net/ ask for one
    if (np_vita_radio_dialog_prepare && np_vita_radio_dialog_prepare(true) != 0)
        port_log("[vgx-vita] CommonDialog set-up failed: the multiplayer dialogs (Nearby, keyboard) are unavailable");
    splash::Enter(splash::kGame);
    return true;
}

// GXV_PACE24 / GXV_SMOOTH_LOG (pace24): the vblank each picture was put on, in the display callback (on its thread). It feeds the display's
// holds to the steady-30 rule (frame_pace.h HoldMeter, GXV_PACE24) and prints the "smooth:" line every GXV_SMOOTH_LOG_S seconds (default 10, 0 = no
// line; GXV_SMOOTH_LOG=0 turns the census off): pictures, late (held longer than the rhythm in force expects), longest hold, holds 16/33/50/67+ ms,
// breaks (a hold different from the one before), the pacer's state changes. `fixed30`: the fixed 30 modes (every picture expects 2 vblanks).
void SmoothShown(uint32_t shown, bool fixed30) {
    static const bool meterOn = vgx::EnvInt("GXV_PACE24", 0) != 0;
    static const bool logOn = vgx::EnvInt("GXV_SMOOTH_LOG", 1) != 0;
    if (!meterOn && !logOn) return;
    static uint32_t last = 0;
    static bool have = false;
    static port::pace::SmoothCensus census;
    static const uint64_t periodUs = static_cast<uint64_t>(std::max<long>(0, vgx::EnvInt("GXV_SMOOTH_LOG_S", 10))) * 1000000u;
    if (meterOn) port::pace::GlobalHoldMeter().Shown(shown);
    const uint32_t hold = have ? shown - last : 0;
    last = shown;
    have = true;
    if (!logOn || periodUs == 0) return;
    const bool steady = fixed30 || (port_pace_steady30_engaged && port_pace_steady30_engaged() != 0);
    census.Picture(hold, steady ? 2u : 1u, steady);
    const uint64_t now = NowUs();
    if (!census.Due(now, periodUs)) return;
    const uint32_t changes = port_pace_steady30_changes ? port_pace_steady30_changes() : 0;
    const port::pace::SmoothCensus::Window w = census.Take(now, changes);
    static char line[320];   // (stackgate31: static, not on the 4 KiB display-callback stack; SmoothShown runs only there, one callback at a time)
    port::pace::SmoothCensus::Format(line, sizeof line, w, steady, vgx::backend::QualityRung());
    if (port_log_summary && w.pictures) port_log_summary_cb("%s", line);
}

// GXV_EVEN_LOG (pace26; default 1, 0 = off) / GXV_EVEN_LOG_S (10): the "even:" line, the owner-facing smoothness score of a run (frame_pace.h's neighbour
// platform/include/port/even26.h EvenMeter): the share of pictures on the ideal cadence, judder (a hold that does not match the turns the picture advanced), latency (the
// picture's turn to the vblank it was shown on) and missed moments (held 3+ vblanks, or a jump of 3+ turns), with one 0..100 score. One call per picture shown, on the display
// thread, in both display callbacks; only pictures in play count (menus, loads and fades break the pair and are left out). step 0 / latency < 0: not known.
port::pace::EvenMeter g_evenMeter;  // (the display thread only)
void EvenShown(uint32_t shown, uint32_t step, int latency, int L) {
    static const bool on = vgx::EnvInt("GXV_EVEN_LOG", 1) != 0;
    static const bool policy = vgx::EnvInt("GXV_HRW", 0) == 5 &&
        (vgx::EnvInt("GXV_HRW_STAGE_AUTO", 1) != 0 || vgx::EnvInt("GXV_HRW41_BUDGET", 0) != 0);
    if (!on && !policy) return;
    static const uint64_t periodUs = static_cast<uint64_t>(std::max<long>(2, vgx::EnvInt("GXV_EVEN_LOG_S", 10))) * 1000000u;
    static uint32_t prevShown = 0;
    static bool have = false, prevPlay = false;
    static uint64_t windowAt = 0, lastAt = 0, playUs = 0;
    const uint64_t now = NowUs();
    const bool play = (!port_screen_quiet || port_screen_quiet() == 0) && !(port_scene_loading_active && port_scene_loading_active());
    const uint32_t hold = have ? shown - prevShown : 0;
    prevShown = shown;
    have = true;
    if (play && prevPlay) {
        g_evenMeter.Picture(hold, step, -1, latency);
        if (lastAt && now - lastAt < 250000) playUs += now - lastAt;
    } else {
        g_evenMeter.Break();
    }
    prevPlay = play;
    lastAt = now;
    if (!windowAt) windowAt = now;
    if (now - windowAt < periodUs) return;
    const port::pace::EvenMeter::Window w = g_evenMeter.Take();
    windowAt = now;
    const uint64_t played = playUs;
    playUs = 0;
    if (w.pictures < 30 || played < 1000000) return;  // (a window of menus and loads)
    const port::pace::EvenMeter::Score sc = port::pace::EvenMeter::Compute(w, played / 1e6);
    port::hrwstage::PublishWindow(sc.score);   // (pace33 PATCH9, GXV_HRW_STAGE_AUTO: the game thread reads it; an atomic store, nothing else on this thread)
    if (!on || !port_log_summary) return;
    static char tail[160], line[640];  // (q30sepfix: static, not on the 4 KiB display-callback stack; only the display callback (PaceDisplay or SyncDisplay, one at a time) calls this)
    tail[0] = 0;
    if (port_pace_even_info) port_pace_even_info(tail, sizeof tail);
    port::pace::EvenMeter::Format(line, sizeof line, sc, played / 1e6, tail, L);
    port_log_summary_cb("%s", line);
}

// smooth25: the release gate's present-cadence metric (frame_pace.h PaceMetric), fed with every picture's vblank in both display callbacks (the locked / steady 30
// path, PaceDisplay, and the "as soon as ready" path, SyncDisplay: Performance mode's 60 and the steady 30 below it). target: the rhythm's own hold in vblanks.
// GXV_PACE_GATE=0 turns it off. PaceDisplay appends the window's tail to its "pace:" line; the other path prints the whole line ("pace: 1-vblank mode, ...") every 5 s.
port::pace::PaceMetric g_paceMetric;  // (the display thread only)
void PaceGate(uint32_t shown, unsigned target, bool ownLine) {
    static const bool on = vgx::EnvInt("GXV_PACE_GATE", 1) != 0;
    if (!on) return;
    static uint32_t last = 0;
    static bool have = false, prevPlay = false;
    static uint64_t windowAt = 0;
    const bool play = (!port_screen_quiet || port_screen_quiet() == 0) && !(port_scene_loading_active && port_scene_loading_active());
    const uint32_t hold = have ? shown - last : 0;
    last = shown;
    have = true;
    g_paceMetric.Picture(hold, target, play && prevPlay);
    prevPlay = play;
    if (!ownLine) return;
    const uint64_t now = NowUs();
    if (!windowAt) windowAt = now;
    if (now - windowAt < 5000000) return;
    const port::pace::PaceMetric::Window w = g_paceMetric.Take();
    if (port_log_summary && w.pictures) {
        static char tail[360];  // (q30sepfix: static: the display callback thread only)
        g_paceMetric.Format(tail, sizeof tail, w);
        port_log_summary_cb("pace: %u-vblank mode, %u pictures in %.2f s; holds 0/1/2/3/4+ vblanks 0/%u/%u/%u/%u; %s", target, w.pictures, (now - windowAt) / 1e6, w.hold[1], w.hold[2],
                         w.hold[3], w.hold[4], tail);
    }
    windowAt = now;
}

// Runs after the GPU fence, just before vitaGL's SetFrameBuf(NEXTFRAME), like the Strikers gate.
void PaceDisplay(void*) {
    struct FifRetire { ~FifRetire() { port_vgx_fif_retire(); } } fifRetire;   // (strk27 GXV_FIF: the picture's flip follows this callback)
    if (splash::Active()) return;
    const unsigned interval = port_vgx_pace_divisor();
    if (!interval) return;
    // GXV_EVEN26_QSLACK (pace26; default 0 = as before, 1..3): the rhythm's first picture is put that many vblanks after the first it could take, so a back-end or GPU time that runs
    // long finds room in the slot instead of missing it (a 3-vblank hold and a rhythm moved later for good). Costs that many vblanks of latency, once.
    static const unsigned slack = static_cast<unsigned>(std::max<long>(0, std::min<long>(3, vgx::EnvInt("GXV_EVEN26_QSLACK", 0))));
    static port::pace::PresentationSlots slots = [] { port::pace::PresentationSlots s; s.SetSlack(slack); return s; }();
    static uint32_t lastShown = 0;
    static uint64_t windowAt = 0, pictures = 0, held[5] = {};
    static unsigned slotEpoch = 0;
    const unsigned requestedEpoch = g_presentSlotEpoch.load(std::memory_order_acquire);
    if (slotEpoch != requestedEpoch) {
        slots = {};
        slots.SetSlack(slack);
        lastShown = 0;
        windowAt = pictures = 0;
        for (uint64_t& n : held) n = 0;
        slotEpoch = requestedEpoch;
    }
    // GXV_PACE_VBLANK_COUNT=1 (default 0): a vblank wait that returned counts as a vblank that passed, even if the counter has not
    // moved yet (frame_pace.h WaitForSlot); off = the counter is trusted after every wait, as before.
    static const bool countWaits = vgx::EnvInt("GXV_PACE_VBLANK_COUNT", 0) != 0;
    static uint64_t lastWaitUs = 0;   // (q30cush31: the time the last vblank wait returned = that vblank's start, to a wake-up's latency)
    struct VitaDisplay {
        uint32_t Count() const { return static_cast<uint32_t>(sceDisplayGetVcount()); }
        int WaitVblank() const { const int r = sceDisplayWaitVblankStart(); lastWaitUs = NowUs(); return r; }
    } display;
    const uint64_t readyUs = NowUs();   // (q30cush31: the moment the picture is ready: this callback runs after its GPU fence)
    uint32_t shown = display.Count() + 1;
    uint32_t due = slots.Next(shown, interval);
    // q30cush31b: how far Next() pulled the slot forward because the picture was ready after it (0: on time). The margin is measured against the slot the rhythm asked for (due - clampAdv).
    const uint32_t clampAdv = due - slots.Nominal();
    // q30-28 GXV_Q30_DRAIN (default 1; the Quality lock's side of the latency): with the producer (game turn) and the consumer (GPU) both at exactly 30 Hz a backlog made once (a stage load, a hitch)
    // never shrinks: Dreadnought sat at 12 vblanks (200 ms) turn to picture with every picture ready 5+ vblanks BEFORE its slot. `early` = vblanks between the picture being ready and its slot. When the
    // last 16 pictures were all ready >= 2 vblanks early, ONE picture takes its slot a vblank sooner (a 1-vblank hold, one 1/60 s step of game time): in play at most one per second,
    // behind a cover (a loading screen, a closed wipe) one per picture. It stops at 1 vblank of room (the slot is still met with ~16 ms to spare), and gives up for the stage when a drain is
    // followed by a missed picture within 1 s (the backlog was the GPU's own queue). Census: the "latency:" line.
    // (pace33: GXV_DRAIN33=1 switches it on for every fixed 30 on this callback, not only the Quality lock's: the c111 controls sat at 5 and 6 vblanks turn to picture)
    static const bool drainOn = (vgx::EnvInt("GXV_Q30_DRAIN", 1) != 0 && vgx::EnvInt("GXV_Q30_LOCK", 0) != 0 && vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30) || vgx::EnvInt("GXV_DRAIN33", 0) != 0;
    // q30cush31: GXV_Q30_CUSHION (default 0; port/q30cushion.h has the rule and its price: one vblank of latency while it is on, never more) and GXV_Q30_SEP (default 1, log only: the "q30sep:" line
    // that keeps the three candidate causes of Quality's odd stutter apart: late pictures, the drain, the pose rhythm). Both only for the fixed 30's 2-vblank rhythm.
    static const bool fixed30 = vgx::EnvInt("GXV_FPS_LIMIT", 0) == 30;
    static const bool cushionOn = fixed30 && vgx::EnvInt("GXV_Q30_CUSHION", 0) != 0;
    static const bool sepOn = fixed30 && vgx::EnvInt("GXV_Q30_SEP", 1) != 0;
    static port::q30cush::Cushion cushion;
    static port::q30cush::SepWindow sep;
    static uint32_t anchorIdx = 0;
    static uint64_t anchorUs = 0;
    static bool anchorOk = false;
    static uint64_t cshEng = 0, cshEngCover = 0, cshRel = 0, cshGave = 0, cshAvoid = 0, cshDespite = 0;   // (the cushion's census at the last window's end)
    static int32_t earlyRing[16];
    static unsigned earlyN = 0, sinceDrain = 0, drainsWin = 0, drainsTot = 0, missAfterDrain = 0;
    static bool drainGiveUp = false;
    static float latEw = -1.0f;
    static uint64_t earlySum = 0, earlyPics = 0;
    static int32_t earlyMaxWin = 0;
    static uint32_t latMinWin = 999, latMaxWin = 0;
    const int32_t early = static_cast<int32_t>(due - shown);
    static unsigned drainEpoch = ~0u;
    if (drainEpoch != requestedEpoch) { drainEpoch = requestedEpoch; earlyN = 0; drainGiveUp = false; drainsTot = 0; missAfterDrain = 0; cushion.Reset(); anchorOk = false; }
    earlyRing[earlyN++ & 15u] = early;
    ++earlyPics; earlySum += early > 0 ? static_cast<uint64_t>(early) : 0u; if (early > earlyMaxWin) earlyMaxWin = early;
    ++sinceDrain;
    const bool playNow = (!port_screen_quiet || port_screen_quiet() == 0) && !(port_scene_loading_active && port_scene_loading_active());
    const int32_t drainRoom = cushionOn ? static_cast<int32_t>(cushion.DrainFloor()) : 2;   // (q30cush31: with the cushion on the drain leaves its vblank alone)
    if (drainOn && !drainGiveUp && interval == 2 && earlyN >= 16 && early >= drainRoom) {
        int32_t lo = early;
        for (int i = 0; i < 16; ++i) lo = std::min(lo, earlyRing[i]);
        if (lo >= drainRoom && (latEw < 0.0f || latEw > 3.0f) && sinceDrain >= (playNow ? 30u : 2u)) {
            due -= 1;
            sinceDrain = 0; earlyN = 0; ++drainsWin; ++drainsTot; missAfterDrain = 0;
            if (sepOn) sep.DrainNext();
        }
    }
    // q30cush31: the picture's margin (ready to the start of its slot's vblank) and the cushion's decision. The anchor is the last vblank a wait returned on (+ whole vblanks of 16 683 us).
    int64_t marginUs = 1000000;   // (unknown: no recent anchor; counted in the top bin)
    if (anchorOk && readyUs - anchorUs < 400000u) marginUs = port::q30cush::MarginUsNominal(due, clampAdv, anchorIdx, anchorUs, readyUs);   // (q30cush31b: against the nominal slot: a late picture has a negative margin)
    if (cushionOn && interval == 2 && marginUs != 1000000) {
        const int d = cushion.Picture(marginUs, playNow);
        if (d) { due = static_cast<uint32_t>(static_cast<int32_t>(due) + d); marginUs += static_cast<int64_t>(d) * port::q30cush::kVblankUs; earlyN = 0; }
    }
    const uint64_t before = NowUs();
    const port::pace::SlotWaitResult slot = port::pace::WaitForSlot(display, due, countWaits);
    shown = slot.shown;
    if (slot.waits) { port_vgx_pace_waited(NowUs() - before); anchorIdx = slot.shown - 1u; anchorUs = lastWaitUs; anchorOk = true; }
    slots.Shown(shown);
    SmoothShown(shown, true);
    PaceGate(shown, interval, false);
    {   // (pace26: the fixed 30's turns a picture advanced and its turn's vblank come from the game thread's ring; none: not known)
        uint32_t steps = 0, vc = 0;
        int lat = -1;
        if (port_vgx_even_take && port_vgx_even_take(&steps, &vc) && vc) {
            const int32_t d = static_cast<int32_t>(shown - vc);
            if (d >= 0 && d < 120) lat = d;
        }
        if (lat >= 0) {   // (q30-28)
            latEw = latEw < 0.0f ? static_cast<float>(lat) : latEw * 0.9f + static_cast<float>(lat) * 0.1f;
            latMinWin = std::min<uint32_t>(latMinWin, static_cast<uint32_t>(lat)); latMaxWin = std::max<uint32_t>(latMaxWin, static_cast<uint32_t>(lat));
        }
        EvenShown(shown, steps, lat, -1);
    }
    if (lastShown) {
        const uint32_t n = shown - lastShown;
        ++held[n < 4 ? n : 4];
        if (sepOn && interval == 2) sep.Picture(n < 1000u ? n : 1000u, marginUs, port::q30cush::Config().guardUs);
        if (n >= 3 && n <= 5 && sinceDrain < 30u && drainsTot > 0 && !drainGiveUp) {   // (q30-28) a miss within 1 s of a drain: the queue was the GPU's own; no more drains in this stage
            if (++missAfterDrain >= 2) { drainGiveUp = true; if (port_log_summary) port_log_summary_cb("latency: drain given up for this stage: 2 missed pictures followed a drain (turn to picture %.1f vblanks)", static_cast<double>(latEw)); }
        }
        if (port_q30lock_shown) port_q30lock_shown(n);   // (q30lock26)
        ++pictures;
    }
    lastShown = shown;
    const uint64_t now = NowUs();
    if (!windowAt) windowAt = now;
    if (now - windowAt >= 5000000) {
        static char gateTail[360];  // (smooth25: the longest hold, the late counts, the in-play set and the run's p99) (q30sepfix: static, not on the stack: PaceDisplay runs only on the display queue's callback thread, whose 4 KiB stack has no room for it; one thread, so one buffer)
        gateTail[0] = 0;
        g_paceMetric.Format(gateTail, sizeof gateTail, g_paceMetric.Take());
        if (port_log_summary)
            port_log_summary_cb("pace: %u-vblank mode, %llu pictures in %.2f s; holds 0/1/2/3/4+ vblanks %llu/%llu/%llu/%llu/%llu; %s",
                             interval, static_cast<unsigned long long>(pictures), (now - windowAt) / 1e6,
                             static_cast<unsigned long long>(held[0]), static_cast<unsigned long long>(held[1]),
                             static_cast<unsigned long long>(held[2]), static_cast<unsigned long long>(held[3]),
                             static_cast<unsigned long long>(held[4]), gateTail);
        if (sepOn && interval == 2 && port_log_summary && sep.pictures) {   // (q30cush31)
            // q30sepfix (console crash 18 Nov, Quality, Observatory, 7 s into play: SceGxmDisplayQueue overflowed its stack in logToFile under this block): the three buffers are static
            // (thread-confined: only the display callback runs PaceDisplay), so this block adds nothing to the callback's stack. Same rule as PATCH2 (c116).
            static char tail[320], pose[200], line[1024];
            tail[0] = 0; pose[0] = 0; line[0] = 0;
            if (cushionOn) {
                std::snprintf(tail, sizeof tail, " (cushion on: engaged %llu (%llu behind a cover), let go %llu, gave up %llu, holds avoided %llu, late in spite of it %llu; +1 vblank of latency while on)",
                              static_cast<unsigned long long>(cushion.engages - cshEng), static_cast<unsigned long long>(cushion.engagesCover - cshEngCover), static_cast<unsigned long long>(cushion.releases - cshRel),
                              static_cast<unsigned long long>(cushion.gaveUpN - cshGave), static_cast<unsigned long long>(cushion.avoided - cshAvoid), static_cast<unsigned long long>(cushion.lateDespite - cshDespite));
                cshEng = cushion.engages; cshEngCover = cushion.engagesCover; cshRel = cushion.releases; cshGave = cushion.gaveUpN; cshAvoid = cushion.avoided; cshDespite = cushion.lateDespite;
            } else {
                std::snprintf(tail, sizeof tail, " (cushion off)");
            }
            unsigned pAll = 0, pOld = 0, pSkip = 0, pBroken = 0, pTurns = 0; int pMode = 0;
            if (port_pose_phase_window_take && port_pose_phase_window_take(&pAll, &pOld, &pSkip, &pBroken, &pTurns, &pMode))
                std::snprintf(pose, sizeof pose, "; (b) pose: GXV_POSE_PHASE=%d, %u turns, %u skip turns, %u broken pairs, %u decisions shown, %u of them at pose age 1+ (%.1f %%)", pMode, pTurns, pSkip, pBroken, pAll, pOld,
                              pAll ? 100.0 * pOld / pAll : 0.0);
            else
                std::snprintf(pose, sizeof pose, "; (b) pose: census off (GXV_POSE_PHASE_CENSUS=1 turns it on)");
            sep.Format(line, sizeof line - 220, (now - windowAt) / 1e6, cushion.Level(), tail);
            const size_t at = std::strlen(line);
            std::snprintf(line + at, sizeof line - at, "%s", pose);
            port_log_summary_cb("%s", line);
            sep.Clear();
        }
        if (WaitLogOn() && port_log_summary) {
            uint64_t gn, gt, gm;
            port_vgx_gate_wait_take(&gn, &gt, &gm);
            const uint64_t sn = g_swapWait.n.exchange(0), st = g_swapWait.totalUs.exchange(0), sm = g_swapWait.maxUs.exchange(0);
            const uint64_t fn = g_hiddenFinishWait.n.exchange(0), ft = g_hiddenFinishWait.totalUs.exchange(0), fm = g_hiddenFinishWait.maxUs.exchange(0);
            port_log_summary_cb("waits: game thread gate %llu x avg %.1f max %.1f ms; render swap %llu x avg %.1f max %.1f ms; hidden-frame finish %llu x avg %.1f max %.1f ms",
                             static_cast<unsigned long long>(gn), gn ? gt / 1000.0 / gn : 0.0, gm / 1000.0,
                             static_cast<unsigned long long>(sn), sn ? st / 1000.0 / sn : 0.0, sm / 1000.0,
                             static_cast<unsigned long long>(fn), fn ? ft / 1000.0 / fn : 0.0, fm / 1000.0);
        }
        if (drainOn && port_log_summary && earlyPics) {   // (q30-28) the Quality lock's latency census
            port_log_summary_cb("latency: Quality 30: turn to picture %.1f vblanks now (%u..%u in the window, %.0f ms); pictures ready before their slot by %.1f vblanks mean, %d max (the backlog); drains %u in the window, %u in the stage%s; "
                             "queue max %u (GXV_LATENCY24_DEPTH %ld)", static_cast<double>(latEw < 0 ? 0.0f : latEw), latMinWin == 999 ? 0u : latMinWin, latMaxWin, static_cast<double>(latEw < 0 ? 0.0f : latEw) * 16.7,
                             static_cast<double>(earlySum) / static_cast<double>(earlyPics), early > earlyMaxWin ? early : earlyMaxWin, drainsWin, drainsTot, drainGiveUp ? " (given up)" : "",
                             port_vgx_queue_max_take ? port_vgx_queue_max_take() : 0u, vgx::EnvInt("GXV_LATENCY24_DEPTH", 0));
            earlySum = earlyPics = 0; earlyMaxWin = 0; drainsWin = 0; latMinWin = 999; latMaxWin = 0;
        }
        windowAt = now;
        pictures = 0;
        for (uint64_t& n : held) n = 0;
    }
}

// GXV_LATENCY24 (latency24; the "latency:" line every GXV_LATENCY24_LOG_S seconds, 10; GXV_LATENCY24_LOG=0 turns it off): how long a pad sample takes to reach
// the screen, and how many pictures are in the pipeline. The game thread notes every drawn turn (port_vgx_vita_note_turn, below: the turn's vblank stamp, the display's
// counter and the time at its start, and when the pad state the game last read was sampled), the display callback finds the note by the picture's stamp (the same
// 16 bits the picture carries through the GX stream) and, since the picture appears at the vblank `shown`, has
//   pad sample to flip = (turn start - pad sample) + (shown - the counter at the turn's start) vblanks.
// The turn's start is the vblank (the game thread is woken by it), so that is exact to a fraction of a vblank. The picture drawn in a turn shows what the PREVIOUS
// turn's update did, and the update reads the pad after the turn's draw: pad sample to turn start is about 9 ms at 60 pictures a second (a turn is 16.7 ms, the pad is
// read about 8 ms into it), 17+ ms when the previous turn was left out (steady 30: its update is at the start of its turn).
struct TurnNote {
    std::atomic<uint32_t> tag{0};   // 0x10000 | the stamp's low 16 bits, once the fields are written
    std::atomic<uint8_t> shown{0};
    uint32_t vcount = 0;
    int64_t turnNs = 0, padNs = 0;
};
TurnNote g_turnNotes[64];
std::atomic<int> g_latOn{-1};          // GXV_LATENCY24_LOG (read on the first turn)
std::atomic<unsigned> g_inFlightMax{0};
uint64_t g_cbReturnUs = 0;             // the display callback's last return (display thread): an entry right after it is "censored" (frame_pace.h Latency24)
uint32_t g_censorUs = 1200;
struct LatencyCensus {
    uint64_t stamped = 0, padPictures = 0;
    double turnToFlipMs = 0, padToTurnMs = 0, padToFlipMs = 0, padToFlipMax = 0;
    uint32_t hist[128] = {};            // pad sample to flip, 4 ms a bin
    unsigned stampRingMax = 0;
    // sums of PresentSync::Stats over the window's 5 s blocks (the statistics are cleared every block)
    uint64_t pictures = 0, late = 0, lateTighter = 0, lowers = 0, raises = 0, probeFails = 0, censored = 0, replaced = 0, calmReleases = 0;
    unsigned latMin = 99, latMax = 0;
    uint64_t windowAt = 0;
} g_lat;
int LatencyLogOn() {
    int on = g_latOn.load(std::memory_order_relaxed);
    if (on < 0) {
        long v = 1;
        EnvLong("GXV_LATENCY24_LOG", v, 0, 1);
        on = v != 0 ? 1 : 0;
        g_latOn.store(on, std::memory_order_relaxed);
    }
    return on;
}
void LatencyShown(uint32_t stamp16, uint32_t shown) {
    TurnNote& n = g_turnNotes[stamp16 & 63];
    const uint32_t want = 0x10000u | stamp16;
    if (n.tag.load(std::memory_order_acquire) != want) return;
    const int64_t turnNs = n.turnNs, padNs = n.padNs;
    const uint32_t vc = n.vcount;
    if (n.tag.load(std::memory_order_acquire) != want) return;  // (rewritten meanwhile: not this picture's)
    n.shown.store(1, std::memory_order_relaxed);
    const int32_t vb = static_cast<int32_t>(shown - vc);
    if (vb < 0 || vb > 120) return;
    ++g_lat.stamped;
    const double flip = vb * 16.68335;
    g_lat.turnToFlipMs += flip;
    if (padNs > 0 && turnNs >= padNs && turnNs - padNs < 2000000000ll) {
        const double pad = (turnNs - padNs) / 1e6, all = pad + flip;
        ++g_lat.padPictures;
        g_lat.padToTurnMs += pad;
        g_lat.padToFlipMs += all;
        if (all > g_lat.padToFlipMax) g_lat.padToFlipMax = all;
        const unsigned bin = static_cast<unsigned>(all / 4.0);
        ++g_lat.hist[bin < 127 ? bin : 127];
    }
}
// Called every 5 s with the display statistics of that block (before they are cleared): adds them up, and prints the line when 10 s are over.
void LatencyBlock(const port::pace::PresentSync::Stats& st, const port::pace::PresentSync& sync, uint64_t now) {
    if (!LatencyLogOn()) return;
    g_lat.pictures += st.pictures; g_lat.late += st.misses; g_lat.lateTighter += st.lateTighter; g_lat.lowers += st.lowers; g_lat.raises += st.raises;
    g_lat.probeFails += st.probeFails; g_lat.censored += st.censored; g_lat.replaced += st.replaced; g_lat.calmReleases += st.calmReleases;
    if (st.latMin < g_lat.latMin) g_lat.latMin = st.latMin;
    if (st.latMax > g_lat.latMax) g_lat.latMax = st.latMax;
    if (!g_lat.windowAt) g_lat.windowAt = now;
    static const long everyS = [] { long v = 10; EnvLong("GXV_LATENCY24_LOG_S", v, 5, 3600); return v; }();
    if (now - g_lat.windowAt < static_cast<uint64_t>(everyS) * 1000000u - 500000u) return;
    long mode = 0;
    EnvLong("GXV_LATENCY24", mode, 0, 2);
    unsigned reads = 0, resamples = 0;
    double ageMean = 0, ageMax = 0;
    if (port_input_age_take) port_input_age_take(&reads, &ageMean, &ageMax, &resamples);
    const unsigned jobsMax = port_vgx_queue_max_take ? port_vgx_queue_max_take() : 0;
    const unsigned inFlight = g_inFlightMax.exchange(0, std::memory_order_relaxed);
    if (port_log_summary) {
        double p95 = 0;
        if (g_lat.padPictures) {
            const uint64_t want = (g_lat.padPictures * 95 + 99) / 100;
            uint64_t acc = 0;
            for (unsigned b = 0; b < 128; ++b) {
                acc += g_lat.hist[b];
                if (acc >= want) { p95 = (b + 1) * 4.0; break; }
            }
        }
        const double np = g_lat.padPictures ? static_cast<double>(g_lat.padPictures) : 1.0, ns = g_lat.stamped ? static_cast<double>(g_lat.stamped) : 1.0;
        port_log_summary_cb("latency: %.1f s, %llu pictures; pad sample to flip %.1f ms mean, %.0f ms 95th, %.0f ms max (pad sample to turn start %.1f, turn start to flip %.1f); "
                         "pad age at the game's read %.1f ms mean, %.1f max (%u reads, %u resampled); present latency L %u now (%u..%u in the window), GXV_LATENCY24 %ld; "
                         "in flight max %u pictures (display queue max %u, back-end queue max %u); late at L %llu (%llu of them on time at the largest latency), "
                         "lowered %llu, raised %llu, lowerings failed %llu, entries behind the previous callback %llu, pictures replaced %llu, hold-offs ended by a clean run %llu",
                         (now - g_lat.windowAt) / 1e6, static_cast<unsigned long long>(g_lat.pictures), g_lat.padToFlipMs / np, p95, g_lat.padToFlipMax,
                         g_lat.padToTurnMs / np, g_lat.turnToFlipMs / ns, ageMean, ageMax, reads, resamples, sync.Latency(), g_lat.latMin == 99 ? sync.Latency() : g_lat.latMin,
                         g_lat.latMax ? g_lat.latMax : sync.Latency(), mode, inFlight, g_lat.stampRingMax, jobsMax, static_cast<unsigned long long>(g_lat.late),
                         static_cast<unsigned long long>(g_lat.lateTighter), static_cast<unsigned long long>(g_lat.lowers), static_cast<unsigned long long>(g_lat.raises),
                         static_cast<unsigned long long>(g_lat.probeFails), static_cast<unsigned long long>(g_lat.censored), static_cast<unsigned long long>(g_lat.replaced),
                         static_cast<unsigned long long>(g_lat.calmReleases));
    }
    g_lat = LatencyCensus();
    g_lat.windowAt = now;
}

// pace33 (14 Nov 2026): GXV_DRAIN33=1 (default 0 = off), port/drain33.h: the standing display queue of a fixed cadence (the re-projected 60, an even 30) is drained one picture at a time.
// With GXV_PRESENT_SYNC=2 it needs GXV_LATENCY24 >= 1 (entry.cpp sets it): the latency rule brings L down on evidence, and a picture that is behind its slot for GXV_DRAIN33_CONFIRM (30)
// pictures takes the vblank of the one before it, at a still moment of the camera when one comes within GXV_DRAIN33_STILL_MS (3000), not twice within GXV_DRAIN33_GAP_MS (1000); behind a
// cover at once. With the fixed 30's own callback (PaceDisplay) it switches the Quality lock's drain (GXV_Q30_DRAIN) on without the lock. Line: "drain:" every 10 s.
port::drain33::Drain g_drain33;
std::atomic<int> g_drain33StartLine{0};
struct Drain33Snap {
    std::atomic<int> ready{0};
    port::drain33::Stats stats;
    uint32_t total = 0;
    unsigned latency = 0;
    double seconds = 0, shownAfter = 0;
} g_drain33Snap;
bool Drain33On() {
    static const bool on = [] {
        port::drain33::Config c;
        long v = 0;
        EnvLong("GXV_DRAIN33", v, 0, 1);
        c.on = v != 0;
        v = c.confirmPics; EnvLong("GXV_DRAIN33_CONFIRM", v, 1, 600); c.confirmPics = static_cast<unsigned>(v);
        v = c.gapUs / 1000; EnvLong("GXV_DRAIN33_GAP_MS", v, 100, 60000); c.gapUs = static_cast<uint32_t>(v) * 1000u;
        v = c.stillWaitUs / 1000; EnvLong("GXV_DRAIN33_STILL_MS", v, 0, 60000); c.stillWaitUs = static_cast<uint32_t>(v) * 1000u;
        g_drain33.Configure(c);
        if (c.on) g_drain33StartLine.store(1, std::memory_order_release);   // (PATCH2: no log from here: the first caller is the display queue's callback)
        return c.on;
    }();
    return on;
}
// PATCH2 (c116 smoke: a stack overflow on the display queue's callback thread; its deepest path, SyncDisplay -> LatencyBlock -> the log's 1100-byte line, had under 640 bytes to spare and the drain's
// line buffer took them): the drain's lines are written by the render thread. The callback only hands over a copy of its counters (g_drain33Snap, single writer; ready 0 -> 1 by the callback,
// 1 -> 0 by the render thread) and never formats or logs for the drain.
void Drain33LogPump() {
    if (!port_log_summary) return;
    if (g_drain33StartLine.exchange(0, std::memory_order_acq_rel)) {
        const port::drain33::Config& c = g_drain33.Cfg();   // (set once, before the flag)
        port_log_summary("drain: GXV_DRAIN33=1: a picture that is behind its slot for %u pictures in a row is given up once (the next comes a refresh early: one 1/60 s step) so the standing queue "
                         "shortens; at a still moment of the camera, or after %.1f s without one; at most one every %.1f s in play, at once behind a cover",
                         c.confirmPics, c.stillWaitUs / 1e6, c.gapUs / 1e6);
    }
    if (g_drain33Snap.ready.load(std::memory_order_acquire) != 1) return;
    static char line[640];   // (render thread only; not on a stack)
    port::drain33::FormatLine(line, sizeof line, g_drain33Snap.stats, g_drain33.Cfg(), g_drain33Snap.total, g_drain33Snap.seconds, g_drain33Snap.shownAfter, g_drain33Snap.latency);
    g_drain33Snap.ready.store(0, std::memory_order_release);
    port_log_summary("%s", line);
}
// GXV_PRESENT_SYNC (frame_pace.h PresentSync): the display queue's callback, after the GPU finished the picture and just before
// vitaGL's SetFrameBuf(NEXTFRAME). =2 holds the picture until the vblank `latency` after its turn began (never more than the
// largest latency + 1 vblanks, never two pictures in one vblank); =1 only measures. Every 5 s a "present:" line in the basic log.
void SyncDisplay(void*) {
    struct FifRetire { bool second = false; ~FifRetire() { if (!second) port_vgx_fif_retire(); } } fifRetire;   // (strk27 GXV_FIF; pace33: a pair's second picture (GXV_HRW) is not one of the game thread's hand-overs)
    static port::pace::PresentSync sync = [] {
        port::pace::PresentSync p;
        long lo = 1, hi = 5, init = 3, raise = 4;
        EnvLong("GXV_PRESENT_SYNC_MIN", lo, 1, 8);   // the smallest latency it may take (vblanks after the turn began)
        EnvLong("GXV_PRESENT_SYNC_MAX", hi, 1, 8);   // the largest: a picture later than this is shown when ready
        EnvLong("GXV_PRESENT_SYNC_LAT", init, 1, 8); // where it starts
        EnvLong("GXV_PRESENT_SYNC_RAISE", raise, 1, 32);  // late pictures of the last 32 that raise it (more: lower latency, more judder)
        p.Configure(port_vgx_present_sync_mode() == 2, static_cast<unsigned>(lo), static_cast<unsigned>(hi), static_cast<unsigned>(init),
                    static_cast<unsigned>(raise));
        // GXV_LATENCY24=1 (default 0): the latency rule that sees the display queue's backlog (frame_pace.h Latency24): L comes down on evidence, 90 pictures
        // at a time, even while the pictures are held; 2: also GXV_LATENCY24_CATCHUP=1 (a picture whose slot the one before holds replaces it: one lost picture
        // per vblank of latency, at most one in GXV_LATENCY24_DROPGAP = 24). Sub-switches: _WINDOW (90), _TOL (per mille of the evidence that may be over, 5),
        // _FLOOR (the lowest L, 2), _HOLDOFF (pictures after a raise, 150), _CENSOR_US (1200). Only with GXV_PRESENT_SYNC=2.
        long mode = 0;
        EnvLong("GXV_LATENCY24", mode, 0, 2);
        if (mode >= 1 && p.Aligned()) {
            port::pace::PresentSync::Latency24 l;
            l.on = true;
            long v = l.window;
            EnvLong("GXV_LATENCY24_WINDOW", v, 8, 2000);
            l.window = static_cast<unsigned>(v);
            l.minKnown = l.window / 2;
            v = l.tolPerMille;
            EnvLong("GXV_LATENCY24_TOL", v, 0, 200);
            l.tolPerMille = static_cast<unsigned>(v);
            v = l.lowFloor;
            EnvLong("GXV_LATENCY24_FLOOR", v, 1, 8);
            l.lowFloor = static_cast<unsigned>(v);
            v = l.holdoff;
            EnvLong("GXV_LATENCY24_HOLDOFF", v, 0, 5000);
            l.holdoff = static_cast<unsigned>(v);
            v = mode >= 2 ? 1 : 0;
            EnvLong("GXV_LATENCY24_CATCHUP", v, 0, 1);
            l.catchUp = v != 0;
            if (Drain33On()) l.catchUp = true;   // (pace33: the drain owns the catch-up: when a picture is given up is port/drain33.h's word, not the drop gap's)
            v = l.dropGap;
            EnvLong("GXV_LATENCY24_DROPGAP", v, 1, 1000);
            l.dropGap = static_cast<unsigned>(v);
            v = l.censorUs;
            EnvLong("GXV_LATENCY24_CENSOR_US", v, 100, 10000);
            l.censorUs = static_cast<uint32_t>(v);
            g_censorUs = l.censorUs;
            if (Drain33On()) {   // (PATCH3: a raise within GXV_DRAIN33_REGRET pictures (1200 = 20 s at 60) of a lowering backs the next lowering off; 0 = off)
                v = 1200;
                EnvLong("GXV_DRAIN33_REGRET", v, 0, 100000);
                l.regretPics = static_cast<unsigned>(v);
                if (v) l.probeMax = 14400;   // (the longest wait before the next lowering: 4 minutes at 60 pictures a second)
            }
            v = 450;   // (pace33 PATCH10: a hold-off after a failed lowering ends after 900 / 1800 / 2400 clean pictures in a row (15 / 30 / 40 s at 60); 0 = off)
            EnvLong("GXV_LATENCY24_CALM", v, 0, 100000);
            l.calmBase = static_cast<unsigned>(v);
            v = 0;   // (thr24 GXV_LATENCY24_FAST=1, default 0: hold-off after a failed lowering capped at _CAP pictures (1200), a regret's multiplier _MUL (2) and a release after _CLEAN clean pictures (180); frame_pace.h Latency24)
            EnvLong("GXV_LATENCY24_FAST", v, 0, 1);
            if (v) {
                v = 1200; EnvLong("GXV_LATENCY24_FAST_CAP", v, 150, 14400); l.holdCap = static_cast<unsigned>(v);
                v = 2; EnvLong("GXV_LATENCY24_FAST_MUL", v, 1, 4); l.regretMul = static_cast<unsigned>(v);
                v = 180; EnvLong("GXV_LATENCY24_FAST_CLEAN", v, 45, 2400); l.cleanPics = static_cast<unsigned>(v);
            }
            p.ConfigureLatency24(l);
            p.SetExternalDrop(Drain33On());
            GXV_CB_LOG("latency: GXV_LATENCY24=%ld: present latency rule with the display queue's backlog seen (window %u pictures, %u per mille over, floor L%u, "
                          "hold-off %u, censor %u us, catch-up %s, drop gap %u; GXV_LATENCY24_FAST %s: hold-off cap %u, regret x%u, release after %u clean)", mode, l.window, l.tolPerMille, l.lowFloor, l.holdoff, l.censorUs,
                          l.catchUp ? "on" : "off", l.dropGap, l.cleanPics ? "on" : "off", l.holdCap, l.regretMul ? l.regretMul : 4u, l.cleanPics);
        } else if (mode >= 1) {
            GXV_CB_LOG("latency: GXV_LATENCY24=%ld asked for, but pictures are not held to their turn's slot (GXV_PRESENT_SYNC is not 2): the rule is off", mode);
        }
        return p;
    }();
    static uint64_t windowAt = 0, waitUs = 0, resyncs = 0;
    const uint64_t entryUs = NowUs();   // (GXV_LATENCY24: how soon after the previous callback returned this one began)
    if (g_stampRing.Size() > g_lat.stampRingMax) g_lat.stampRingMax = g_stampRing.Size();
    uint32_t stamp = 0;
    bool have = g_stampRing.Pop(stamp);
    // (a swap that put nothing in the display queue left its stamp behind: the queue holds at most two pictures after this one)
    while (have && g_stampRing.Size() > 2) {
        if ((stamp & 0xC0000000u) != 0xC0000000u) port_vgx_fif_retire();   // (PATCH3: that swap has no callback of its own: GXV_FIF's count must not wait for one)
        g_stampRing.Pop(stamp);
        ++resyncs;
    }
    if (splash::Active()) {
        sync.Reset();
        return;
    }
    static uint64_t vblankAtUs = 0;   // (pace33: when a vblank last began, as this callback's own waits saw it)
    struct VitaDisplay {
        uint32_t Count() const { return static_cast<uint32_t>(sceDisplayGetVcount()); }
        int WaitVblank() const { const int r = sceDisplayWaitVblankStart(); vblankAtUs = NowUs(); return r; }
    } display;
    const uint32_t earliest = display.Count() + 1;
    fifRetire.second = have && (stamp & 0xC0000000u) == 0xC0000000u;
    const bool drain33 = Drain33On() && sync.Latency24On();
    if (drain33) {   // (pace33 GXV_DRAIN33: may this picture take the vblank of the one before it? The standing queue gets one picture shorter)
        const bool quiet = (port_screen_quiet && port_screen_quiet() != 0) || (port_scene_loading_active && port_scene_loading_active());
        sync.AllowDrop(g_drain33.Allow(entryUs, sync.Behind(earliest, have && (stamp & 0x80000000u) != 0, static_cast<uint16_t>(stamp & 0xFFFFu)), port_hrw_still ? port_hrw_still() : -1, quiet));
    }
    const uint32_t target = sync.Target(earliest, have && (stamp & 0x80000000u) != 0, static_cast<uint16_t>(stamp & 0xFFFFu));
    if (drain33 && sync.Dropped()) g_drain33.Dropped(entryUs);
    if (sync.Latency24On()) sync.NoteBacklog(g_cbReturnUs != 0 && entryUs - g_cbReturnUs < g_censorUs);
    if (drain33) {   // (pace33: a picture that came less than GXV_DRAIN33_MARGIN_US (4000) before the next vblank is no evidence for a lower latency)
        static const uint32_t marginUs = [] { long v = 4000; EnvLong("GXV_DRAIN33_MARGIN_US", v, 0, 12000); return static_cast<uint32_t>(v); }();
        bool tight = false;
        if (vblankAtUs && entryUs >= vblankAtUs && entryUs - vblankAtUs < 200000u) tight = 16683u - static_cast<uint32_t>((entryUs - vblankAtUs) % 16683u) < marginUs;
        sync.NoteTight(tight);
    }
    uint32_t shown = earliest;
    bool waited = false;
    if (target != earliest) {
        const uint64_t before = NowUs();
        const port::pace::SlotWaitResult slot = port::pace::WaitForSlot(display, target, true, 10);
        shown = slot.shown;
        waited = slot.waits != 0;
        waitUs += NowUs() - before;
    }
    sync.Shown(shown, waited);
    {   // (pace26: GXV_PENDING_HOLD25=2 = the guarded allowance; low byte the allowance, the rest the guard engaged count)
        static const bool guarded25 = [] { long v = 0; EnvLong("GXV_PENDING_HOLD25", v, 0, 9); return v == 2; }();
        port_vgx_pending_allowance_set((sync.HoldAllowance(guarded25) & 0xFFu) | (sync.GuardEngaged() << 8));
    }
    // (pace25: a scalar store; perf.cpp reads it only with GXV_PENDING_HOLD25=1)
    {   // (pace26: this picture's turns and latency from its stamp: the low 16 bits of its turn's vblank number)
        static uint16_t prevStamp = 0;
        static bool havePrevStamp = false;
        uint32_t step = 0;
        int lat = -1;
        if (have && (stamp & 0x80000000u) != 0) {
            const uint16_t s16 = static_cast<uint16_t>(stamp & 0xFFFFu);
            const uint32_t l = static_cast<uint16_t>(static_cast<uint16_t>(shown) - s16);
            if (l < 120) lat = static_cast<int>(l);
            if (havePrevStamp) { const uint32_t d = static_cast<uint16_t>(s16 - prevStamp); if (d > 0 && d < 60) step = d; }
            prevStamp = s16;
            havePrevStamp = true;
        } else {
            havePrevStamp = false;
        }
        EvenShown(shown, step, lat, static_cast<int>(sync.Latency()));
    }
    SmoothShown(shown, false);
    PaceGate(shown, (port_pace_steady30_engaged && port_pace_steady30_engaged() != 0) ? 2u : 1u, true);
    const uint64_t now = NowUs();
    g_cbReturnUs = now;
    if (have && (stamp & 0x80000000u) != 0 && LatencyLogOn()) LatencyShown(stamp & 0xFFFFu, shown);
    if (!windowAt) windowAt = now;
    if (now - windowAt < 5000000) return;
    const port::pace::PresentSync::Stats& st = sync.GetStats();
    LatencyBlock(st, sync, now);
    if (drain33) {   // (pace33: every second 5 s block)
        static unsigned blocks = 0;
        static uint64_t since = 0;
        if (!since) since = windowAt;
        if (++blocks >= 2 && g_drain33Snap.ready.load(std::memory_order_acquire) == 0) {   // (PATCH2: handed to the render thread; if it has not taken the last one, the counters keep adding up)
            g_drain33Snap.stats = g_drain33.GetStats();
            g_drain33Snap.total = g_drain33.Total();
            g_drain33Snap.latency = sync.Latency();
            g_drain33Snap.seconds = (now - since) / 1e6;
            g_drain33Snap.shownAfter = st.stamped ? static_cast<double>(st.latSum) / static_cast<double>(st.stamped) : 0.0;
            g_drain33Snap.ready.store(1, std::memory_order_release);
            g_drain33.ClearStats();
            blocks = 0;
            since = now;
        }
    }
    if (port_log_summary && st.pictures) {
        const double pairs = st.pairs ? static_cast<double>(st.pairs) : 1.0, n = st.stamped ? static_cast<double>(st.stamped) : 1.0;
        const double latMean = st.latSum / n, latSd = std::sqrt(std::fmax(0.0, st.latSq / n - latMean * latMean));
        port_log_summary_cb("present: %s, %.1f s, %llu pictures (%llu of game turns); latency %u vblanks (+%llu -%llu); ready after 0/1/2/3/4/5/6/7+ vblanks "
                         "%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; late %llu (%llu beyond the largest); held 0/1/2/3/4+ vblanks %llu/%llu/%llu/%llu/%llu; "
                         "game steps 1/2/3/4+ turns %llu/%llu/%llu/%llu; held-vs-step off by 0/1/2/3+ %llu/%llu/%llu/%llu, judder %.2f vblanks a picture "
                         "(%.1f ms); shown-minus-turn %.2f +- %.2f vblanks; waits %llu (%.1f ms avg); resyncs %llu",
                         sync.Aligned() ? "aligned" : "measured", (now - windowAt) / 1e6, static_cast<unsigned long long>(st.pictures),
                         static_cast<unsigned long long>(st.stamped), sync.Latency(), static_cast<unsigned long long>(st.raises),
                         static_cast<unsigned long long>(st.lowers), static_cast<unsigned long long>(st.natural[0]), static_cast<unsigned long long>(st.natural[1]),
                         static_cast<unsigned long long>(st.natural[2]), static_cast<unsigned long long>(st.natural[3]), static_cast<unsigned long long>(st.natural[4]),
                         static_cast<unsigned long long>(st.natural[5]), static_cast<unsigned long long>(st.natural[6]), static_cast<unsigned long long>(st.natural[7]),
                         static_cast<unsigned long long>(st.misses), static_cast<unsigned long long>(st.overload), static_cast<unsigned long long>(st.hold[0]),
                         static_cast<unsigned long long>(st.hold[1]), static_cast<unsigned long long>(st.hold[2]), static_cast<unsigned long long>(st.hold[3]),
                         static_cast<unsigned long long>(st.hold[4]), static_cast<unsigned long long>(st.step[1]), static_cast<unsigned long long>(st.step[2]),
                         static_cast<unsigned long long>(st.step[3]), static_cast<unsigned long long>(st.step[4]), static_cast<unsigned long long>(st.err[0]),
                         static_cast<unsigned long long>(st.err[1]), static_cast<unsigned long long>(st.err[2]), static_cast<unsigned long long>(st.err[3]),
                         st.errSum / pairs, st.errSum / pairs * 16.683, latMean, latSd, static_cast<unsigned long long>(st.waits),
                         st.waits ? waitUs / 1000.0 / st.waits : 0.0, static_cast<unsigned long long>(resyncs));
    }
    sync.ClearStats();
    windowAt = now;
    waitUs = 0;
    resyncs = 0;
}

// The back end's present hook: a display copy, on the render thread, with the EFB framebuffer
// object `efb` bound and the finished picture in it. (x, y, w, h) is the copy rectangle in
// framebuffer pixels; the EFB is stored top row first (its projection flips Y), so the
// picture's top row is the framebuffer's y. The back end binds the EFB again and re-applies
// its render state after this returns.
bool GamePresentAllowed(unsigned efb, int w, int h) {
    return efb != 0 && w > 0 && h > 0 && (g_presentAllowed || (g_fixedJob && g_wasSplash));
}
// The picture into the display's scene: the upscale of the EFB rectangle (x, y, w, h) into the display rectangle [dx0, dx1) x [0, kScreenH),
// bottom row first, with the sharpening / edge smoothing the settings ask for. Leaves READ = the EFB, DRAW = the display bound and the scissor
// test off (the caller puts it back, or the back end sets its own).
void PresentPicture(unsigned efb, int x, int y, int w, int h, int& outDx0, int& outDx1) {
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
    const int dx0 = display.x * kScreenW / 960, dx1 = (display.x + display.width) * kScreenW / 960; // (the rectangle is in panel pixels)
    // Source rows are given bottom-up so the picture comes out upright.
    const int sy0 = y + h, sy1 = y;
    if (g_scissorOn) { glDisable(GL_SCISSOR_TEST); ParkScissor(); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // GXV_COLOR_LOADSKIP bit 4 (vitaGL patches/0021): the display's scene draws opaque quads over the whole screen (the sharpen /
    // edge pass or the blit, scissor off, no blending), so what its colour tiles hold at the start is never seen: they are filled
    // from a tiny texture. Only for the picture on the whole screen (GXV_VITA_WIDE=1): side bars would be left unwritten.
    static const bool colorSkip = [] { long v = 0; EnvLong("GXV_COLOR_LOADSKIP", v, 0, 31); return (v & 4) != 0 && vglColorLoadSkip != nullptr; }(); // (pix23: 0..15, as StartGl reads it: 8 clamped to 7 turned bit 4 on)
    const bool colorSkipArmed = colorSkip && dx0 == 0 && dx1 == kScreenW && vglColorLoadSkip(GL_TRUE) == GL_TRUE;
    if (vgxvita_maskGuardOn) vgxvita_maskGuard(); // (the display's scene starts with a mask update too)
    // GXV_DEPTH_SKIP=2 (a vitaGL with DEPTH_HINT): the display's scene is a blit; it neither tests nor keeps depth.
    static const bool depthSkip = [] { long v = 2; EnvLong("GXV_DEPTH_SKIP", v, 0, 2); return v >= 2 && vglSetNextSceneDepthModes != nullptr; }();
    if (depthSkip) vglSetNextSceneDepthModes(GL_FALSE, GL_FALSE);
    // Sharpened upscale (vgx_backend_gl.cpp PresentSharpened; GXV_SHARPEN, default auto: the amount follows the upscale,
    // none at the screen's own size). A shader that does not build leaves the plain blit for good.
    static bool sharpenOk = true;
    float sharpen = sharpenOk ? vgx::backend::SharpenAmount(w, h, dx1 - dx0, kScreenH) : 0.0f;
    // GXV_PRESENT_PLAIN (gpucut23; a MINOR CHANGE): 0 = as before (default); 1 = every mode; perf = Performance mode only. A picture drawn
    // smaller than the screen is presented by the plain bilinear blit, without the unsharp mask (PresentSharpened / GXV_PRESENT_FAST's
    // three-read "plain3"): the picture is a little softer. Console (r59): the display scene's 3D with the blit 0.9 ms; c25 Honeyhive with
    // plain3 + Mario's edge smoothing 1.75 ms. Only where no edge smoothing is asked (with GXV_EDGE_AA's step on, PresentEdgeAA draws as
    // before). "present-plain:" basic-log line every 600 presents while it is on.
    {
        static const int plainMode = [] {
            const char* e = std::getenv("GXV_PRESENT_PLAIN");
            if (!e || !*e) return 0;
            if (*e == 'p' || *e == 'P') return port_perf_mode_60 && port_perf_mode_60() ? 1 : 0;
            return std::atoi(e) == 1 ? 1 : 0;
        }();
        static uint32_t presents = 0, plain = 0;
        if (plainMode && sharpen >= 0.05f && !vgx::backend::EdgeAAWanted(w, h, dx1 - dx0, kScreenH)) {
            sharpen = 0.0f;
            ++plain;
        }
        if (plainMode && ++presents >= 600) {
            GXV_BASIC_LOG("present-plain: GXV_PRESENT_PLAIN: %u of the last %u presents drawn with the plain blit (unsharp mask left out)", plain, presents);
            presents = plain = 0;
        }
    }
    // GXV_EDGE_AA (vgx_draw.cpp, "whole-picture edge smoothing"): the whole picture through the edge filter, the HUD drawn again
    // plainly, in place of everything below (false: nothing drawn, the present as before).
    if (!vgx::backend::PresentEdgeAA(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, sharpen < 0.05f ? 0.0f : sharpen, true)) {
        bool sharpened = false;
        if (sharpen < 0.05f || !(sharpenOk = sharpened = vgx::backend::PresentSharpened(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, sharpen, true)))
            glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        // GXV_CHARAA (vgx_draw.cpp, "character edge smoothing"): the characters' and key items' edges, over what was just drawn,
        // in this same scene (nothing is drawn in a frame without them, nor on a multisampled EFB).
        vgx::backend::PresentCharaAA(0, x, y, w, h, dx0, 0, dx1 - dx0, kScreenH, sharpened ? sharpen : 0.0f, true);
    }
    if (colorSkipArmed) vglColorLoadSkip(GL_FALSE); // (taken back: only the scene the draws above began may use it)
    outDx0 = dx0;
    outDx1 = dx1;
}

// GXV_DIRECT_DISPLAY=2 (console probe, changes no picture): what one 960x544 A8B8G8R8 sceGxmTransferCopy costs while the game runs, for the
// "1:1 transfer to the display buffer" route that was not built. Two scratch buffers (2 MiB each) are made on the first use; every 300th present
// one copy is issued and waited for (sceGxmTransferFinish), its wall time noted; "direct-display-probe:" every 600 presents (min/avg/max us). The
// wait includes whatever the transfer unit was queued behind (a scene's tile work shares the SGX), which is the number that matters.
void DirectDisplayProbe(uint64_t presented) {
    static void* src = nullptr;
    static void* dst = nullptr;
    static bool tried = false;
    static uint32_t n = 0, lo = ~0u, hi = 0, failed = 0;
    static uint64_t sum = 0;
    if (presented % 300 != 150 && !(n && presented % 600 == 0)) return;
    if (!tried) {
        tried = true;
        if (vglForceAllocAligned) {
            src = vglForceAllocAligned(4096, 2u << 20, GL_TRUE);
            dst = vglForceAllocAligned(4096, 2u << 20, GL_TRUE);
        }
        port_log("[vgx-vita] direct-display-probe: scratch buffers %s", src && dst ? "made (2 x 2 MiB)" : "NOT made: no probe");
    }
    if (!src || !dst) return;
    if (presented % 300 == 150) {
        const uint64_t t0 = NowUs();
        const int r = sceGxmTransferCopy(960, 544, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, src, 0, 0, 3840,
                                         SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, dst, 0, 0, 3840, nullptr, 0, nullptr);
        if (r < 0) { ++failed; return; }
        sceGxmTransferFinish();
        const uint32_t us = static_cast<uint32_t>(NowUs() - t0);
        ++n;
        sum += us;
        if (us < lo) lo = us;
        if (us > hi) hi = us;
    }
    if (presented % 600 == 0 && (n || failed)) {
        GXV_BASIC_LOG("direct-display-probe: sceGxmTransferCopy 960x544 ABGR linear 1:1, %u samples: min %u us, avg %u us, max %u us (%u failed); the pictures are shown as with GXV_DIRECT_DISPLAY=0",
                      n, n ? lo : 0u, n ? static_cast<unsigned>(sum / n) : 0u, hi, failed);
        n = 0; lo = ~0u; hi = 0; sum = 0; failed = 0;
    }
}

bool PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {
    const int phase = vgx::backend::PresentPhase(); // (GXV_HUD_NATIVE: 0 the whole present, 1 the picture only, 2 the swap only; vgx_cmd.h)
    splash::NoteGamePresent(); // (bootscr26: whichever half of the present this is, the boot screen is asked whether it is over; no path is the only one that ends it)
    if (phase == 1) {
        // The picture so far goes into the display at the start of the picture's 2D tail, which the back end then draws into it. Nothing is
        // done here that the whole present would not do, and nothing at all when it would not show the picture (false: the whole present
        // comes at the display copy, as before).
        if (splash::Active() || !GamePresentAllowed(efb, w, h)) return false;
        if (g_fixedJob && g_wasSplash) {
            g_presentAllowed = true;
            g_presentSlotEpoch.fetch_add(1, std::memory_order_release);
        }
        int px0 = 0, px1 = 0;
        PresentPicture(efb, x, y, w, h, px0, px1);
        vgx::backend::SetHudNativeTarget(0, px0, 0, px1 - px0, kScreenH, true);
        return true; // (the display stays bound and its scissor is the back end's to set: no restore here)
    }
    playprogress::Leave();
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
        if (vglGetMaskKeeps) port_log("[vgx-vita] vitaGL MASK_KEEP: %u full-target scissor-mask quads left out so far (GXV_VGL_MASKKEEP)", vglGetMaskKeeps());
        if (vglGetColorLoadsFrom) port_log("[vgx-vita] vitaGL COLOR_LOAD_FROM: %u scenes loaded their colour from another buffer (GXV_VGL_LOADFROM)", vglGetColorLoadsFrom());
        if (vglGetValidRegionScenes && vglGetValidRegionRefusals)
            port_log("[vgx-vita] vitaGL patch 0028: %u scenes begun with a valid region, %u refused (GXV_VGL_VALIDREGION); %u colour loads from another buffer fell back",
                     vglGetValidRegionScenes(), vglGetValidRegionRefusals(), vglGetColorLoadFromFailures ? vglGetColorLoadFromFailures() : 0u);
        unsigned made = 0, ms = 0;
        vgxvita_cacheFolderStats(&made, &ms);
        if (made != 0) port_log("[vgx-vita] shader cache: %u bucket folders made as shaders needed them, %u ms in all", made, ms);
        if (g_seedFallback[0]) port_log("[vgx-vita] shader cache: %u compiled shaders read from the package in place, %u not there either", g_seedFallbackHits.load(std::memory_order_relaxed),
                                        g_seedFallbackMisses.load(std::memory_order_relaxed));
        if (g_presented % 600 == 0 || g_presented == 1) {
            LogPools("now");
            LogMemory("now");
        }
    }
    // scenes23 (basic log, every 600 presents while GXV_COLOR_LOADSKIP / GXV_VGL_LOADFROM / GXV_VGL_VALIDREGION is set): what vitaGL did with
    // the changed scene begins. "begin failures" must stay 0 (each one drops the draw that began a scene, three in a frame drop the rest
    // of the frame); "refused" > 0 means GXM said no to a changed colour surface (patch 0028 then begins it as it is and stops asking).
    static const bool beginLog = [] { long a = 0, b = 0, c = 0; EnvLong("GXV_COLOR_LOADSKIP", a, 0, 31); EnvLong("GXV_VGL_LOADFROM", b, 0, 1);
                                      EnvLong("GXV_VGL_VALIDREGION", c, 0, 1); return a != 0 || b != 0 || c != 0; }();
    if (beginLog && g_presented % 600 == 0)
        GXV_BASIC_LOG("[vgx-vita] vgl-scenes: %llu presents; cheap colour loads %u, loads from another buffer %u (fell back %u), valid-region scenes %u (refused %u); "
                      "changed colour surfaces refused by sceGxmBeginScene %u; scene begin failures %u%s",
                      static_cast<unsigned long long>(g_presented), vglGetColorLoadSkips ? vglGetColorLoadSkips() : 0u, vglGetColorLoadsFrom ? vglGetColorLoadsFrom() : 0u,
                      vglGetColorLoadFromFailures ? vglGetColorLoadFromFailures() : 0u, vglGetValidRegionScenes ? vglGetValidRegionScenes() : 0u,
                      vglGetValidRegionRefusals ? vglGetValidRegionRefusals() : 0u, vglGetColorSurfaceRefusals ? vglGetColorSurfaceRefusals() : 0u,
                      vglGetSceneBeginFailures ? vglGetSceneBeginFailures() : 0u, vglGetSceneBeginFailures ? "" : " (not counted: vitaGL before vp26)");
    if (g_presented == 5 || g_presented == 30 || g_presented == 120) PinLibraryThreads(false, g_presented == 120);
    else PinLibraryThreadsStep(g_presented); // (stut600: the 600-present rescans in slices, not whole in one present)
    if (splash::Active()) {
        // What the renderer has been given so far moves while the game loads: one of the loading screen's signs of life.
        splash::g_contentTick.store(static_cast<uint64_t>(stats.textures) * 7u + static_cast<uint64_t>(stats.programs) * 13u +
                                    static_cast<uint64_t>(stats.shaderCompiles) * 17u + static_cast<uint64_t>(stats.programLinks) * 19u +
                                    static_cast<uint64_t>(stats.textureBytes / 4096u), std::memory_order_relaxed);
    }
    g_lastPresentUs = now;
    g_lastStats = stats;

    splash::HandleDialog();
    if (splash::Active()) {
        // The loading screen is up: the game's pictures stay out of sight. (A game that never shows its title, e.g. started
        // straight into a stage by PETARI_STAGE, ends the screen after 300 pictures of real content in a row.)
        // (farlod32: GXV_BOOT_RICH_DRAWS, default 150, and GXV_BOOT_RICH_PICS, default 300, read once: the two numbers of that rule. A test run
        // that leaves draws out on purpose - GXV_FACECULL=3 - or a stage whose pictures dip under 150 draws never gets 300 such pictures in a
        // row, and the boot screen then stays over a game that is being played: emulator run c102-fcshow.)
        static const unsigned richDraws = [] { const char* e = std::getenv("GXV_BOOT_RICH_DRAWS"); const long v = e && *e ? std::atol(e) : 150; return static_cast<unsigned>(v < 1 ? 1 : v > 100000 ? 100000 : v); }();
        static const unsigned richPics = [] { const char* e = std::getenv("GXV_BOOT_RICH_PICS"); const long v = e && *e ? std::atol(e) : 300; return static_cast<unsigned>(v < 1 ? 1 : v > 100000 ? 100000 : v); }();
        static unsigned richInARow = 0;
        richInARow = stats.drawCallsThisFrame >= richDraws ? richInARow + 1 : 0;
        if (port_boot_play_has_begun()) splash::NoteTitleVisible("PETARI_BOOT: play began");
        else if (richInARow >= richPics)
            splash::NoteTitleVisible(richDraws == 150 && richPics == 300 ? "300 pictures with 150 or more draw calls" : "the pictures in a row with the draw calls GXV_BOOT_RICH_PICS / GXV_BOOT_RICH_DRAWS ask for");
        splash::Service();
        if (g_fixedJob) g_wasSplash = true;
        return false;
    }
    if (!GamePresentAllowed(efb, w, h)) return false;
    if (g_fixedJob && g_wasSplash) {
        // The first real picture is eligible even if its producing turn began
        // while the loading overlay still owned presentation.
        g_presentAllowed = true;
        g_presentSlotEpoch.fetch_add(1, std::memory_order_release);
    }
    int dx0 = 0, dx1 = 0;
    // GXV_DIRECT_DISPLAY=1: the picture is the EFB's own colour buffer when the present would be a plain copy of the whole of it (the back end decides:
    // vgx_directdisplay.h); the buffer is queued by the swap below and the display scene is not made. Anything else: the present as before.
    bool directFlip = false, directOffered = false;
    // (MP3 merge fix 4: the dialog's step runs HERE, once for this present, before the direct-display offer, and its answer goes to the offer and to
    // the swap below: the dialog is composed onto the swapped buffer (GL_TRUE), so while one is up the present stays the ordinary copy. Read before the
    // step, as it was, the answer was the last frame's: on the frame a dialog opened or closed the picture and the dialog composition disagreed.)
    const GLboolean mpDialog = MpDialogSwapMode();
    const bool mpDialogUp = mpDialog == GL_TRUE;
    if (phase == 0 && vgx::backend::DirectDisplayMode() == 1 && !mpDialogUp) {
        const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
        const unsigned id = vgx::backend::DirectDisplayOffer(x, y, w, h, display.x * kScreenW / 960, (display.x + display.width) * kScreenW / 960, kScreenW, kScreenH, true);
        directOffered = id != 0;
        directFlip = directOffered && vglSetNextFlip != nullptr && vglSetNextFlip(id) != 0;
        if (directOffered && !directFlip) { // (r2: say why vitaGL refused the flip, three times at most)
            static int refusedLogged = 0;
            if (refusedLogged < 3) {
                ++refusedLogged;
                uint32_t d[10] = {};
                const uint32_t n = vglGetDisplayFailure ? vglGetDisplayFailure(d) : 0u;
                port_log("[vgx-vita] direct-display: vglSetNextFlip(%u) refused: %u refusals, last reason %u texture %u status %u data 0x%08x format 0x%08x type 0x%08x stride %u size %ux%u, display textures %u",
                         id, n, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
            }
        }
        static bool firstLogged = false, startLogged = false;
        if (!startLogged) {
            startLogged = true;
            port_log("[vgx-vita] direct-display: GXV_DIRECT_DISPLAY=1, vitaGL %s", vglSetNextFlip ? "has patch 0030 (display textures)" : "has no patch 0030 (before vp28): every picture is presented as before");
        }
        if (directFlip && !firstLogged) {
            firstLogged = true;
            port_log("[vgx-vita] direct-display: t=%u ms first picture shown by flipping the EFB's own colour buffer (texture %u): no present scene", NowMs(), id);
        }
    }
    if (phase != 2 && !directFlip) PresentPicture(efb, x, y, w, h, dx0, dx1); // (phase 2, GXV_HUD_NATIVE: it was drawn at the start of the 2D tail)
    const uint64_t swapStart = WaitLogOn() ? NowUs() : 0;
    NoteSwap(g_presentStamp);  // (GXV_PRESENT_SYNC: the display callback holds the picture to its turn's slot)
    g_presentStamp = 0;        // (a second display copy in the same job is shown when ready)
    g_fifJobSwapped = true;    // (PATCH3: GXV_FIF: this job's picture is retired by its display callback)
    const uint64_t seam63Swap0 = NowUs();
    vglSwapBuffers(mpDialog);  // MP3: GL_TRUE while a multiplayer system dialog is up (the step ran above, once for this present); GL_FALSE otherwise, as c181
    {   // seam63: a swap that takes long (vitaGL's collector frees, the display queue's wait) is named in the log with the picture: the owner's 200 ms present after the first ladder step
        const uint64_t swapUs = NowUs() - seam63Swap0;
        vgx::backend::NoteSwapUs(static_cast<unsigned>(std::min<uint64_t>(swapUs, 4000000u))); // (seam68: tied to the present's frees in the back end's log)
        static unsigned swapLogged = 0;
        if (swapUs >= 40000u && swapLogged < 12) {
            ++swapLogged;
            GXV_BASIC_LOG("[seam63] vglSwapBuffers took %u ms at present %llu (the swap itself: vitaGL's collector and the display queue's wait)", static_cast<unsigned>(swapUs / 1000u),
                          static_cast<unsigned long long>(g_presented));
        }
    }
    if (directOffered) vgx::backend::DirectDisplayResult(directFlip, directFlip ? vgx::dd::kDirect : vgx::dd::kRefused); // (the back end rotates the EFB when the hook returns)
    if (vgx::backend::DirectDisplayMode() == 2 && phase != 2) DirectDisplayProbe(g_presented);
    if (swapStart) g_swapWait.Add(NowUs() - swapStart);
    if (g_fixedJob) {
        g_wasSplash = false;
        g_jobSwapped = true;
        g_gameSwaps.fetch_add(1, std::memory_order_relaxed);
    }
    vgx::backend::AutoQualitySwapped(); // (automatic quality: the GPU clock times this frame's completion)
    // Back to what the back end had: it clears the EFB next.
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    if (phase != 2 && g_scissorOn && !directFlip) { UnparkScissor(); glEnable(GL_SCISSOR_TEST); } // (phase 2: the scissor is the tail's, set through the back end's own calls)
    return true;
}

} // namespace

// ---------------------------------------------------------------- what vgx_galaxy.cpp calls
extern "C" void port_vgx_vita_reset_present_slots(void) {
    g_presentSlotEpoch.fetch_add(1, std::memory_order_release);
}
extern "C" int port_vgx_vita_fixed_cap_supported(void) { return vglSubmitWithoutSwap != nullptr; }
// GXV_PRESENT_SYNC: the display's vblank counter (game thread, vgx_galaxy.cpp port_vgx_present_turn) and the stamp of the job the
// render thread executes next (vgx_galaxy.cpp ExecuteJob; PresentDisplayCopy hands it to the display callback with the swap).
extern "C" uint32_t port_vgx_vita_vcount(void) { return static_cast<uint32_t>(sceDisplayGetVcount()); }
extern "C" void port_vgx_vita_set_present_stamp(uint32_t stamp) {
    g_presentStamp = stamp;
    if (g_drain33StartLine.load(std::memory_order_relaxed) || g_drain33Snap.ready.load(std::memory_order_relaxed)) Drain33LogPump();   // (PATCH2: the render thread, once per job: the drain's lines)
}
// GXV_LATENCY24: a turn that draws begins (game thread, vgx_galaxy.cpp port_vgx_present_turn): its stamp, the display's counter and the time, the time the pad
// state the game last read was sampled. Also the pictures in flight: the drawn turns of the last 15 vblanks whose pictures have not been shown yet, and this one.
extern "C" void port_vgx_vita_note_turn(uint32_t stamp, uint32_t vcount, int64_t turnNs, int64_t padNs) {
    if (!LatencyLogOn() || !g_syncOn) return;
    unsigned inFlight = 1;
    for (uint32_t i = 1; i <= 15; ++i) {
        const uint32_t s = stamp - i;
        const TurnNote& e = g_turnNotes[s & 63];
        if (e.tag.load(std::memory_order_acquire) == (0x10000u | (s & 0xFFFFu)) && e.shown.load(std::memory_order_relaxed) == 0) ++inFlight;
    }
    unsigned seen = g_inFlightMax.load(std::memory_order_relaxed);
    while (inFlight > seen && !g_inFlightMax.compare_exchange_weak(seen, inFlight, std::memory_order_relaxed)) {}
    TurnNote& n = g_turnNotes[stamp & 63];
    n.tag.store(0, std::memory_order_relaxed);
    n.shown.store(0, std::memory_order_relaxed);
    n.vcount = vcount;
    n.turnNs = turnNs;
    n.padNs = padNs;
    n.tag.store(0x10000u | (stamp & 0xFFFFu), std::memory_order_release);
}
extern "C" void port_vgx_vita_set_present_allowed(int allowed) {
    g_fixedJob = true;
    g_presentAllowed = allowed != 0;
    g_jobSwapped = false;
}
// PATCH3 (console c118: ~60 ms game-thread gaps in the file select of every GXV_FIF run): GXV_FIF counts a hand-over as over when its picture is flipped, or, for a job that never swaps, here.
// That second half ran only for the fixed 30's jobs (port_vgx_vita_finish_unpresented_frame), so in every other mode each job without a display copy left the count one short for good, the
// gate saw "too many in flight" at every hand-over and made the game thread wait its full 50 ms, thirty times, before it switched itself off. Now every job ends here (render thread).
// second: a pair's second picture (GXV_HRW), never counted.
extern "C" void port_vgx_vita_job_end(int second) {
    if (!g_fifJobSwapped && !second) port_vgx_fif_retire();
    g_fifJobSwapped = false;
}
extern "C" void port_vgx_vita_finish_unpresented_frame(void) {
    if (!g_jobSwapped && vglSubmitWithoutSwap) {
        const uint64_t start = WaitLogOn() ? NowUs() : 0;
        vglSubmitWithoutSwap();
        if (start) g_hiddenFinishWait.Add(NowUs() - start);
    }
}
extern "C" unsigned long long port_vgx_vita_game_swaps(void) {
    return g_gameSwaps.load(std::memory_order_relaxed);
}
extern "C" int port_vgx_vita_settings_finish(unsigned) {
    if (splash::Active() || !vglFinishUniformPoolScene || !vglResumeUniformPoolScene) return 0;
    g_settingsSceneResume = vglFinishUniformPoolScene();
    if (sceGxmTransferFinish() < 0 || sceGxmDisplayQueueFinish() < 0) {
        vglResumeUniformPoolScene(g_settingsSceneResume);
        return 0;
    }
    return 1;
}
extern "C" void port_vgx_vita_settings_resume(void) {
    port_vgx_vita_reset_present_slots();
    g_wasSplash = true;
    vglResumeUniformPoolScene(g_settingsSceneResume);
}
// (on the render thread, which has a 4 MiB stack: the shader compiler runs on it)
extern "C" void port_vgx_vita_prepare(void) { PrepareFrontEnd(); }
extern "C" void port_vgx_vita_load_packs(void) { EnsureShaderPackLoaded(); }
extern "C" int port_vgx_vita_start(void) {
    PinCurrentThread(kCoreRender);
    vgx::core3probe::Start();
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
extern "C" int port_boot_data_enabled(void) __attribute__((weak));
// A message put on the loading screen can be seen: the screen is up and something is drawing it.
static bool CanShowError() {
    using namespace splash;
    const int phase = g_phase.load();
    return !(phase == kOff || phase == kDone || g_thread < 0 || !g_pic[0] || !g_pic[1] || (phase == kDirect && !loading::direct::g_base));
}
extern "C" int port_boot_data_error(const char* message) {
    using namespace splash;
    if (!port_boot_data_enabled || !port_boot_data_enabled()) return 0;
    const int phase = g_phase.load();
    if (phase == kOff || phase == kDone || g_thread < 0 || !g_pic[0] || !g_pic[1] ||
        (phase == kDirect && !loading::direct::g_base)) return 0;
    Fail(message);
    return 1;
}
// vgx_galaxy.cpp: a start-up failure that is already on the loading screen stays there (the game's thread parks) instead of aborting.
extern "C" int port_vgx_vita_error_shown(void) { return splash::g_failed.load() && CanShowError() ? 1 : 0; }
// vgx_galaxy.cpp: a start-up failure with a text for the screen ("line one\nline two"). 1 when the screen can show it.
extern "C" int port_vgx_vita_fail(const char* message) {
    if (!CanShowError()) return 0;
    splash::Fail(message);
    return 1;
}
// vgx_galaxy.cpp: the renderer's own set-up (its picture buffer, its copy shader) failed after the graphics had started.
extern "C" void port_vgx_vita_backend_failed(void) {
    GXV_BASIC_LOG("[vgx-vita] the renderer's set-up failed after the graphics had started (picture buffer or copy shader; see the lines above)");
    port_vgx_vita_fail("The renderer could not set up its picture buffer or copy shader.\nRestart the Vita and start the game again. If this repeats, send log.txt.");
}

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
    if (direct) g_shownSinceUs = NowUs();
    g_thread = sceKernelCreateThread("smg_loading", &TickerMain, 0x10000100, 128 * 1024, 0, kCoreAux, nullptr);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        port_log("[vgx-vita] loading screen: its thread could not start (0x%X)", static_cast<unsigned>(g_thread));
        g_phase.store(kDone);
        for (uint32_t*& p : g_pic) { std::free(p); p = nullptr; }
        loading::direct::Close();
        return;
    }
    ConfigureDialog();
    port_log("[vgx-vita] t=%u ms loading screen up (%s); expected stage times (s): start %.1f, unpack %.1f, graphics %.1f, game %.1f, shaders %.1f, title %.1f", NowMs(),
             direct ? "on the display directly" : "no video memory block: it waits for the renderer", g_expect[0], g_expect[1], g_expect[2], g_expect[3], g_expect[4],
             g_expect[5]);
}

// vgx_galaxy.cpp's render thread: the back end exists. The loading screen moves from the direct display to the renderer's.
extern "C" void port_vgx_vita_ready(void) {
    using namespace splash;
    if (g_phase.load() != kDirect) return;
    g_phase.store(kGl, std::memory_order_release);  // (the thread stops writing the display)
    if (!g_shownSinceUs) g_shownSinceUs = NowUs();
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
extern "C" int port_vgx_vita_splash_active(void) {
    // Keep idle lifecycle service alive after yielding, for real title completion or a later boot failure.
    return splash::Active() || splash::g_phase.load() == splash::kYielded ? 1 : 0;
}
// keepawake33: 1 while the loading screen is actively loading (kDirect/kGl, !failed, !stalled),
// -1 while showing the GXV_BOOT_STALL_SEC hung-boot alert (so entry.cpp stops BootOrCompile ticks even before frame 2),
// 0 on kYielded, kDone, kOff, or fatal failure.
extern "C" int port_vgx_vita_boot_loading(void) {
    const int p = splash::g_phase.load(std::memory_order_acquire);
    if ((p != splash::kDirect && p != splash::kGl) || splash::g_failed.load(std::memory_order_acquire)) return 0;
    return splash::g_stalled.load(std::memory_order_acquire) ? -1 : 1;
}
extern "C" void port_vgx_vita_idle(void) { splash::Service(); }
// (the same while the shader warm-up is being worked off between frames: a picture costs the render thread about 10 ms, so not sooner than `gapUs` after the last)
extern "C" void port_vgx_vita_idle_gap(unsigned gapUs) { splash::Service(gapUs); }
// dvd.cpp: a long job at start-up (moving the game's files into bucket folders) reports its progress here.
extern "C" void port_boot_progress(const char* what, int done, int total) { splash::TaskProgress(what, done, total); }

// The game's frame clock: the console's video retrace, here the display's own vertical blank
// (GXV_VITA_VBLANK_CLOCK=0: the platform layer's free-running 59.94 Hz timer instead, which
// drifts against the display and drops or repeats a picture every few seconds).
extern "C" void port_vi_retrace(void);
extern "C" int port_vita_thread_class(int cls) __attribute__((weak)); // vita/runtime/memory.cpp
extern "C" int port_vita_thread_helper(void) __attribute__((weak));
extern "C" void port_thread_register(const char* name) __attribute__((weak));  // (the "cores-5s:" lines' thread names, vita/runtime/memory.cpp)
namespace {
void* VblankMain(void*) {
    if (port_thread_register) port_thread_register("vblank-clock");
    // GXV_VBLANK_PRIO=1: the frame clock runs at the clocks' priority (0x50, cores 1-2) like the timer and retrace threads. Left to the
    // thread library it starts at the lowest user priority, 0xBF, on any core, and can be held up by the game thread (0x9F) or the
    // back end when it shares a core with them; it does a few microseconds of work per vertical blank. Default 0: as before.
    static const bool raise = std::getenv("GXV_VBLANK_PRIO") && std::atoi(std::getenv("GXV_VBLANK_PRIO")) == 1;
    if (raise && port_vita_thread_class) port_vita_thread_class(1);
    else if (port_vita_thread_helper) port_vita_thread_helper();  // GXV_CORE0_ALONE=1: off the game's core even when not raised
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

// heap23: the renderer's part of the scene-destroy heap line (os_thread.cpp port_heap_scene_destroyed, game thread): free main memory,
// vitaGL's pools (when its main-memory pool is full, vitaGL puts GPU allocations in the C heap: "use_extra_mem"), and the back end's
// object counts (vitaGL's shader and program objects live in the C heap). The counts are the back end's own (read without a lock:
// for a log line). shader23: "vertex + fragment shaders" are compiled so far (cumulative); "shaders held" is what takes vitaGL's
// 2,048 shader slots now (GXV_SHADER_RECLAIM gives back the ones no linked program uses at scene changes), with what was given back.
extern "C" int port_vgx_vita_memory_brief(char* out, unsigned size) {
    if (!out || !size) return 0;
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    const vgx::backend::Stats stats = vgx::backend::GetStats();
    // (heap23b: + the phycont pool, where vitaGL puts what does not fit in its main pool (gpu_utils.c: main, phycont, video, then the C
    // heap); the textures' KiB are the texels of the textures alive now, then all uploaded since start-up - the only figure before)
    return std::snprintf(out, size,
                         "free main memory %d KiB; vitaGL pools free: main %u of %u KiB, video %u of %u KiB, phycont %u of %u KiB; renderer: %u vertex + "
                         "%u fragment shaders, %u programs, %u textures (%u KiB of texels now; %u KiB uploaded since start-up); shaders held %u of 2048 "
                         "slots (peak %u; %u given back in %u passes, %u programs deleted for room; %u creates without a slot)",
                         free.size_user / 1024, static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) / 1024),
                         static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) / 1024),
                         static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_PHYCONT) / 1024),
                         stats.vertexShaders, stats.fragmentShaders, stats.programs, stats.textures, static_cast<unsigned>(stats.textureBytesLive / 1024u),
                         static_cast<unsigned>(stats.textureBytes / 1024u), stats.shadersHeld, stats.shadersHeldPeak, stats.shadersReclaimed,
                         stats.shaderReclaimPasses, stats.shaderReclaimProgramDeletes, stats.shaderCreateFails);
}

// The status block's per-thread CPU report (see ThreadCpuReport). GXV_THREAD_REPORT=0 turns it off.
extern "C" void port_vgx_vita_thread_report(void) {
    if (!PORT_RUNTIME_DIAGNOSTICS && !ThreadsBasicLog()) return;   // (loglines44: GXV_THREADS_LOG=1, the player build's basic log)
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
//
// GXV_VITA_UPLOAD_SYNC=2 (default 1 = the wait after every call; tex11): the wait is kept only where something could touch the
// memory a pending transfer reads or writes, and made there once for all the transfers before it. The calls note that transfers
// are pending; the back end (vgx_backend_gl.cpp, XferFence) asks for the wait before
//   - a call that may move a texture's memory (vitaGL's realloc of a compressed chain or of a texture getting generated levels:
//     the copy is made by the CPU, so the data the transfer writes must be there first) - none when vitaGL reserves the chain at
//     the first level (vglReserveTextureLevels, vgl patch 0015),
//   - the deletion or a new upload of a texture whose transfers are pending (vitaGL may free its memory at once),
//   - the end of a scene (a new render target, the present, a mid-scene flush), the first moment the GPU can sample what was
//     uploaded, and the end of the frame.
// A stage arrival's frame (r49 frame 2640: 263 textures, 143 ms) waited for the transfer unit hundreds of times; in play each new
// texture cost ~1 ms (r49 "in play ... texture uploads 6 (0.0 MiB, 6 ms)"). Waits and their time are counted in every mode
// (vgxvita_xferStats; the back end's "tex-upload:" line).
static int UploadSyncMode() {
    static const int mode = [] {
        const char* e = std::getenv("GXV_VITA_UPLOAD_SYNC");
        const int v = (e && *e) ? std::atoi(e) : 1;
        return v == 0 ? 0 : v == 2 ? 2 : 1;
    }();
    return mode;
}
static bool g_xferPending = false;        // (render thread only) transfers issued since the last wait
static uint64_t g_xferWaits = 0, g_xferWaitUs = 0;
static vgx::texupx::LinPool g_linPool;    // (texup31) the swizzled upload's linear scratch; Flush() is called by every wait below
static void XferWait() {
    const uint64_t t0 = NowUs();
    sceGxmTransferFinish();
    g_xferWaitUs += NowUs() - t0;
    ++g_xferWaits;
    g_xferPending = false;
    g_linPool.Flush(); // (texup31: scratch left pending by GXV_TEXUP_LAZYFENCE goes back now; nothing pending: a no-op)
}
static void XferIssued() {
    if (UploadSyncMode() == 1) XferWait();
    else g_xferPending = true; // (mode 0: never waited for; diagnostic only)
}
extern "C" int vgxvita_uploadSyncMode(void) { return UploadSyncMode(); }
extern "C" void vgxvita_xferFence(void) {
    if (g_xferPending && UploadSyncMode() == 2) XferWait();
}
extern "C" void vgxvita_xferStats(uint64_t* waits, uint64_t* us) {
    *waits = g_xferWaits;
    *us = g_xferWaitUs;
}
extern "C" void vgxvita_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border,
                                               GLsizei imageSize, const void* data) {
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    XferIssued();
}
extern "C" void vgxvita_glGenerateMipmap(GLenum target) {
    glGenerateMipmap(target);
    XferIssued();
}

extern "C" void vgxvita_uploadAuthoredI4Mips(unsigned width, unsigned height, unsigned count, const void* const* levels) {
    // The backend admits only square POT RGBA8 I4 chains. Asking for the last level
    // allocates the chain once; ordinary higher-level TexImage discards its pixels.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, levels[0]);
    const unsigned last = count - 1;
    glTexImage2D(GL_TEXTURE_2D, last, GL_RGBA, width >> last, height >> last, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // Mandatory even when the diagnostic upload-sync switch is off: CPU writes
    // below must not race the temporary downscales issued by the allocation call.
    XferWait();
    for (unsigned l = 1; l < count; ++l)
        glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, width >> l, height >> l, GL_RGBA, GL_UNSIGNED_BYTE, levels[l]);
}

// ---- gxm23, GXV_TEX_SWIZZLE (vgx_backend_gl.cpp Upload): an uncompressed game texture stored SWIZZLED.
// vitaGL keeps every uncompressed texture LINEAR (utils/gpu_utils.c gpu_alloc_texture -> vglInitLinearTexture; only its compressed
// uploads are swizzled, by the transfer unit). The SGX's texture cache fetches lines of the texture's memory: in a linear texture a
// 2x2 filter footprint spans two rows (two lines), a minified or rotated read walks down columns, so most of a fetched line is never
// used; in the swizzled (Morton) layout a line holds a square block of texels. Sony's own validation (sceGxmTextureSetUAddrMode,
// mirrored by Vita3K verify_texture_mode) refuses the MIRROR addressing mode for anything but swizzled textures: vitaGL writes it
// into linear ones anyway, so GX mirrored textures that are not DXT1 are sampled in a mode the hardware is not specified for today.
// Here: the same texels and the same mip levels as vitaGL's path (the level-0 rows, the generated levels made by the same
// sceGxmTransferDownscale calls on the same linear layout, vgx_swizzle_layout.h), then each level copied unchanged (RAW16 / RAW32)
// by the transfer unit into a swizzled chain, a wait for the transfer unit (the default GXV_VITA_UPLOAD_SYNC=1 waits after every
// texture with transfers anyway), the linear scratch freed, and the texture bound on the active unit given the swizzled storage
// (vglSetupTexture: vitaGL frees the old storage as on a new upload, keeps the sampler state, and owns the new block from then on).
// kind: 0 RGBA8 (GL_RGBA/UNSIGNED_BYTE), 1 RGB565, 2 RGBA5551, 3 RGBA4444, 4 LA88 (GL_LUMINANCE_ALPHA) - the GXM formats vitaGL's
// glTexImage2D picks for the back end's calls (textures.c). Returns the level count of the texture made, 0 when it made nothing (not
// eligible, a missing vitaGL entry, out of memory, a transfer refused): then nothing was changed and the caller uploads as before.
extern "C" void vglSetupTexture(GLenum target, void* data, SceGxmTextureFormat format, SceGxmTextureType type, GLsizei width, GLsizei height,
                                GLsizei mip_count) __attribute__((weak));
extern "C" void* vglForceAllocAligned(uint32_t alignment, uint32_t size, GLboolean for_gpu) __attribute__((weak));
// (fix25, vitaGL patch 0036; weak: a library without it files nothing) the swizzled block is a texture's storage: the spill counter files it as a texture, and GXV_VRAM_STRICT's request is made for it
extern "C" uint32_t vglSpillSetKind(uint32_t kind) __attribute__((weak));
struct SpillTexScope {
    uint32_t old = 0;
    SpillTexScope() { if (vglSpillSetKind) old = vglSpillSetKind(1); }
    ~SpillTexScope() { if (vglSpillSetKind) vglSpillSetKind(old); }
};
extern "C" void vglFree(void* addr) __attribute__((weak));
static uint64_t g_swzMade = 0, g_swzBytes = 0, g_swzFallbacks = 0;
// (dma31) GXV_DMA_LOAD: see vgx_dma_load.h. Render thread only.
static uint64_t DmaLoadClockUs() { return sceKernelGetProcessTimeWide(); }
static int DmaLoadEngine(void* dst, const void* src, unsigned size) { return sceDmacMemcpy(dst, src, size); }
static void DmaLoadCopy(void* dst, const void* src, uint32_t n, unsigned width, unsigned height) {
    static const vgx::dmaload::Config cfg = [] {
        vgx::dmaload::Config c;
        c.mode = static_cast<int>(vgx::EnvInt("GXV_DMA_LOAD", 0));
        if (c.mode < 0 || c.mode > 3) c.mode = 0;
        const long m = vgx::EnvInt("GXV_DMA_LOAD_MIN", 65536);
        c.minBytes = static_cast<uint32_t>(m < 8192 ? 8192 : (m > (64L << 20) ? (64L << 20) : m)); // (below ~8 KiB the engine is slower than the CPU)
        if (c.mode) GXV_BASIC_LOG("dma-load: GXV_DMA_LOAD=%d: the level-0 copy of a swizzled texture upload of %u bytes or more %s", c.mode, c.minBytes,
                                  c.mode == 3 ? "stays with the CPU and is counted (census)" : (c.mode == 2 ? "goes to the DMA engine and is verified" : "goes to the DMA engine"));
        return c;
    }();
    static vgx::dmaload::Census census;
    if (cfg.mode == 0) { std::memcpy(dst, src, n); return; }
    bool differed = false;
    vgx::dmaload::Copy(cfg, census, dst, src, n, &DmaLoadEngine, &DmaLoadClockUs, &differed);
    if (differed) {
        static bool told = false;
        if (!told) { told = true; GXV_BASIC_LOG("dma-load: VERIFY: the DMA engine's copy of a %ux%u texture (%u bytes) differed from its source; copied again by the CPU (logged once)", width, height, n); }
    }
    if (census.copies % 100 == 0) {
        char line[512];
        vgx::dmaload::Line(line, sizeof line, cfg, census);
        GXV_BASIC_LOG("%s", line);
    }
}
extern "C" void vgxvita_swizzleStats(uint64_t* made, uint64_t* bytes, uint64_t* fallbacks) {
    *made = g_swzMade;
    *bytes = g_swzBytes;
    *fallbacks = g_swzFallbacks;
}
// ---- texup31 (17 Nov 2026): the fixed cost of a swizzled upload. See vgx_texup_scratch.h for the four switches (all default 0).
static const vgx::texupx::Config& TexupCfg() {
    static const vgx::texupx::Config cfg = [] {
        vgx::texupx::Config c;
        c.census = vgx::EnvInt("GXV_TEXUP_CENSUS", 0) != 0;
        c.lazy = static_cast<int>(vgx::EnvInt("GXV_TEXUP_LAZYFENCE", 0)) != 0 && UploadSyncMode() == 2; // (the back end fences only in mode 2)
        c.scratch = vgx::EnvInt("GXV_TEXUP_SCRATCH", 0) != 0;
        c.verify = vgx::EnvInt("GXV_TEXUP_VERIFY", 0) != 0;
        const long kib = vgx::EnvInt("GXV_TEXUP_POOL_KIB", 2048);
        c.poolBytes = static_cast<uint32_t>((kib < 64 ? 64 : (kib > 16384 ? 16384 : kib)) << 10);
        if (c.census || c.lazy || c.scratch || c.verify)
            GXV_BASIC_LOG("texup31: census=%d lazy-fence=%d scratch-pool=%d (%u KiB) verify=%d (GXV_TEXUP_*; sync mode %d)", c.census, c.lazy, c.scratch, c.poolBytes >> 10, c.verify,
                          UploadSyncMode());
        return c;
    }();
    return cfg;
}
static void* TexupAlloc(uint32_t size) { return vglForceAllocAligned(16, size, GL_TRUE); }
static void TexupFree(void* p) { vglFree(p); }
static void TexupWaitAll() { XferWait(); }
static vgx::texupx::Census g_texupCensus;
static uint64_t g_texupVerifyFails = 0, g_texupVerified = 0;
static bool g_texupLastPending = false;
static void TexupVerifyBlock(const vgx::texupx::LinPool::Block& b, const void* lin) {
    uint32_t level = 0, texel = 0;
    ++g_texupVerified;
    if (!vgx::texupx::VerifyChain(b.meta.plan, b.meta.bpp, static_cast<const uint8_t*>(lin), b.meta.swz, &level, &texel)) {
        ++g_texupVerifyFails;
        static bool told = false;
        if (!told) { told = true; GXV_BASIC_LOG("texup31: VERIFY: a swizzled chain differs from the linear one it was made from (level %u texel %u, %u levels, bpp %u; logged once)", level, texel, b.meta.plan.levels, b.meta.bpp); }
    }
}
static void TexupInit() {
    static bool done = false;
    if (done) return;
    done = true;
    const vgx::texupx::Config& c = TexupCfg();
    vgx::texupx::Hooks h;
    h.alloc = &TexupAlloc; h.free = &TexupFree; h.waitAll = &TexupWaitAll;
    g_linPool.Setup(h, c);
    if (c.verify) g_linPool.SetVerify(&TexupVerifyBlock);
}
extern "C" int vgxvita_swizzleLeftPending(void) { return g_texupLastPending ? 1 : 0; }
// The census line's pieces for the back end's window line: cumulative microseconds of the swizzled path's phases, uploads, and the pool counters.
extern "C" unsigned vgxvita_texupCensusLine(char* out, unsigned cap) {
    if (!out || !cap || !TexupCfg().census) return 0;
    vgx::texupx::Stats st = g_linPool.St();
    st.verified = g_texupVerified;
    st.verifyFails = g_texupVerifyFails;
    const int n = vgx::texupx::Line(out, cap, g_texupCensus, &st);
    return n < 0 ? 0u : (static_cast<unsigned>(n) >= cap ? cap - 1 : static_cast<unsigned>(n));
}
extern "C" unsigned vgxvita_uploadSwizzled(int kind, unsigned width, unsigned height, const void* level0, int genMips, int logEach) {
    if (!vglSetupTexture || !vglForceAllocAligned || !vglFree || !level0 || kind < 0 || kind > 5) return 0;
    g_texupLastPending = false;
    TexupInit();
    const vgx::texupx::Config& cfg = TexupCfg();
    uint64_t ph[vgx::texupx::kPhases] = {}; // (census only)
    uint64_t tc = cfg.census ? NowUs() : 0;
#define TEXUP_LAP(i) do { if (cfg.census) { const uint64_t tn_ = NowUs(); ph[i] += tn_ - tc; tc = tn_; } } while (0)
    if (kind == 5) {
        // g30 Wave 7: 1-byte TF_I8 (U8_RRRR) CPU Morton swizzle into SCE_GXM_TEXTURE_SWIZZLED.
        // libSceGxm has no RAW8 transfer format, and SGX543 requires SWIZZLED for MIRROR wrap (BlackHole.bdl IntTex_QuarterHalo).
        if (genMips) return 0;
        const vgx::swz::Plan p = vgx::swz::MakePlan(width, height, 1u, false);
        if (!p.levels) return 0;
        uint8_t* swz;
        { SpillTexScope spillTex; swz = static_cast<uint8_t*>(vglForceAllocAligned(16, p.swzBytes, GL_TRUE)); }
        if (!swz) { ++g_swzFallbacks; return 0; }
        TEXUP_LAP(vgx::texupx::kAlloc);
        alignas(16) uint8_t stackBuf[4096];
        std::unique_ptr<uint8_t[]> heapBuf;
        uint8_t* tmp = stackBuf;
        if (p.swzBytes > sizeof(stackBuf)) {
            heapBuf.reset(new (std::nothrow) uint8_t[p.swzBytes]);
            if (!heapBuf) { vglFree(swz); ++g_swzFallbacks; return 0; }
            tmp = heapBuf.get();
        }
        vgx::swz::SwizzleU8(static_cast<const uint8_t*>(level0), tmp, width, height);
        DmaLoadCopy(swz, tmp, p.swzBytes, width, height);
        TEXUP_LAP(vgx::texupx::kCopy);
        if (cfg.verify) {
            uint32_t badL = 0, badT = 0;
            ++g_texupVerified;
            if (!vgx::texupx::VerifyChain(p, 1u, static_cast<const uint8_t*>(level0), swz, &badL, &badT)) {
                ++g_texupVerifyFails;
                port_log("[vgx-vita] GXV_TEXUP_VERIFY: I8 %ux%u mismatch at level %u texel %u", width, height, badL, badT);
            }
            if (cfg.census) tc = NowUs(); // do not charge optional verify walk to kSetup
        }
        SceGxmTexture probe;
        if (sceGxmTextureInitSwizzled(&probe, swz, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, width, height, 1) < 0) {
            vglFree(swz);
            ++g_swzFallbacks;
            static bool toldI8 = false;
            if (!toldI8) { toldI8 = true; port_log("[vgx-vita] GXV_TEX_SWIZZLE: sceGxmTextureInitSwizzled refused U8_RRRR %ux%u: linear upload instead (logged once)", width, height); }
            return 0;
        }
        vglSetupTexture(GL_TEXTURE_2D, swz, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, SCE_GXM_TEXTURE_SWIZZLED, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
        ++g_swzMade;
        g_swzBytes += p.swzBytes;
        TEXUP_LAP(vgx::texupx::kSetup);
        if (cfg.census) {
            g_texupCensus.Note(kind, p.swzBytes, false, ph);
            if (g_texupCensus.n % 100 == 0) {
                char line[900];
                if (vgxvita_texupCensusLine(line, sizeof line)) GXV_BASIC_LOG("%s", line);
            }
        }
        if (logEach) port_log("[vgx-vita] GXV_TEX_SWIZZLE: %ux%u kind 5 (I8), 1 level, %u bytes CPU-swizzled", width, height, p.swzBytes);
        return 1u;
    }
    static const SceGxmTextureFormat kFmt[5] = {SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB,
                                                SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA, SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA, SCE_GXM_TEXTURE_FORMAT_A8L8};
    const SceGxmTextureFormat fmt = kFmt[kind];
    const uint32_t bpp = kind == 0 ? 4u : 2u;
    // The downscale format: vitaGL's tex_format_to_transfer (utils/gpu_utils.h), by base format.
    SceGxmTransferFormat down = static_cast<SceGxmTransferFormat>(0xFFFFFFFFu);
    switch (static_cast<uint32_t>(fmt) & 0x9F000000u) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5: down = SCE_GXM_TRANSFER_FORMAT_U1U5U5U5_ABGR; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5: down = SCE_GXM_TRANSFER_FORMAT_U5U6U5_BGR; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4: down = SCE_GXM_TRANSFER_FORMAT_U4U4U4U4_ABGR; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8: down = SCE_GXM_TRANSFER_FORMAT_U8U8_GR; break;
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8: down = SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR; break;
    default: break;
    }
    if (genMips && down == static_cast<SceGxmTransferFormat>(0xFFFFFFFFu)) return 0;
    const vgx::swz::Plan p = vgx::swz::MakePlan(width, height, bpp, genMips != 0);
    if (!p.levels) return 0;
    const SceGxmTransferFormat raw = bpp == 4 ? SCE_GXM_TRANSFER_FORMAT_RAW32 : SCE_GXM_TRANSFER_FORMAT_RAW16;
    // (texup31) the scratch comes from the pool when GXV_TEXUP_SCRATCH is on (and waits for the unit, then retries, when nothing is free), else as before.
    const bool pooled = cfg.scratch || cfg.lazy || cfg.verify; // (lazy and verify need the block's bookkeeping; with scratch off the pool only allocates plainly)
    uint8_t* lin = pooled ? static_cast<uint8_t*>(g_linPool.Acquire(p.linBytes)) : static_cast<uint8_t*>(vglForceAllocAligned(16, p.linBytes, GL_TRUE));
    if (!lin) { ++g_swzFallbacks; return 0; }
    uint8_t* swz;
    { SpillTexScope spillTex; swz = static_cast<uint8_t*>(vglForceAllocAligned(16, p.swzBytes, GL_TRUE)); } // (fix25: filed as a texture)
    if (!swz) { if (pooled) g_linPool.Release(lin, false); else vglFree(lin); ++g_swzFallbacks; return 0; }
    TEXUP_LAP(vgx::texupx::kAlloc);
    // Level 0 as gpu_alloc_texture stores it: width >= 8, so its rows are packed (VGL_ALIGN(w, 8) == w).
    // (dma31, GXV_DMA_LOAD, default 0: a large one by the DMA engine; vgx_dma_load.h)
    DmaLoadCopy(lin, level0, static_cast<uint32_t>(width) * height * bpp, width, height);
    TEXUP_LAP(vgx::texupx::kCopy);
    bool ok = true;
    // gpu_alloc_mipmaps' chain: level j + 1 from level j, same formats, sizes, strides and offsets.
    for (unsigned j = 0; ok && j + 1 < p.levels; ++j)
        ok = sceGxmTransferDownscale(down, lin + p.linOff[j], 0, 0, p.w[j], p.h[j], static_cast<int>(p.linStride[j]), down, lin + p.linOff[j + 1], 0, 0,
                                     static_cast<int>(p.linStride[j + 1]), nullptr, 0, nullptr) >= 0;
    for (unsigned l = 0; ok && l < p.levels; ++l)
        ok = sceGxmTransferCopy(p.w[l], p.h[l], 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE, raw, SCE_GXM_TRANSFER_LINEAR, lin + p.linOff[l], 0, 0,
                                static_cast<int32_t>(p.linStride[l]), raw, SCE_GXM_TRANSFER_SWIZZLED, swz + p.swzOff[l], 0, 0,
                                static_cast<int32_t>(p.w[l] * bpp), nullptr, 0, nullptr) >= 0;
    TEXUP_LAP(vgx::texupx::kXfer);
    // (texup31, GXV_TEXUP_LAZYFENCE) not a refused transfer, and the back end fences in mode 2: leave the transfers pending, the scratch goes back at the next wait.
    const bool lazy = cfg.lazy && ok;
    vgx::texupx::VerifyMeta meta;
    if (cfg.verify && ok) { meta.set = true; meta.plan = p; meta.bpp = bpp; meta.swz = swz; }
    if (lazy) {
        g_xferPending = true;
        g_texupLastPending = true;
        if (g_linPool.Release(lin, true, &meta)) XferWait(); // (over the pending cap: wait now)
    } else {
        XferWait(); // (everything issued above, and any earlier pending transfer, is done: the scratch can go, the texture is complete)
        if (pooled) g_linPool.Release(lin, false, &meta);
        else vglFree(lin);
    }
    TEXUP_LAP(vgx::texupx::kWait);
    if (ok) {
        // vglSetupTexture ignores what sceGxmTextureInitSwizzled returns: ask it first, on a scratch descriptor (size, level count, alignment)
        SceGxmTexture probe;
        ok = sceGxmTextureInitSwizzled(&probe, swz, fmt, width, height, p.levels) >= 0;
    }
    if (!ok) {
        // (a swizzled block a lazy transfer may still be writing: the wait first; lazy is only taken with ok, but the probe may refuse)
        if (g_texupLastPending) { XferWait(); g_texupLastPending = false; }
        vglFree(swz);
        ++g_swzFallbacks;
        static bool told = false;
        if (!told) { told = true; port_log("[vgx-vita] GXV_TEX_SWIZZLE: the transfer unit or sceGxmTextureInitSwizzled refused a %ux%u kind %d texture: linear upload instead (logged once)", width, height, kind); }
        return 0;
    }
    vglSetupTexture(GL_TEXTURE_2D, swz, fmt, SCE_GXM_TEXTURE_SWIZZLED, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                    static_cast<GLsizei>(p.levels));
    ++g_swzMade;
    g_swzBytes += p.swzBytes;
    TEXUP_LAP(vgx::texupx::kSetup);
    if (cfg.census) {
        g_texupCensus.Note(kind, width * height * bpp, genMips != 0, ph);
        if (g_texupCensus.n % 100 == 0) {
            char line[900];
            if (vgxvita_texupCensusLine(line, sizeof line)) GXV_BASIC_LOG("%s", line);
        }
    }
    if (logEach) port_log("[vgx-vita] GXV_TEX_SWIZZLE: %ux%u kind %d, %u level(s), %u bytes swizzled (linear chain %u)", width, height, kind, p.levels,
                          p.swzBytes, p.linBytes);
    return p.levels;
#undef TEXUP_LAP
}

// ---- GXV_LA88_MIPS (revisit23; vgx_texcache.cpp, vgx_backend_gl.cpp La88MipsExactHere): is the transfer unit's U8U8_GR downscale, which
// vitaGL's glGenerateMipmap (gpu_alloc_mipmaps) and the swizzled upload above use for an A8L8 texture, the same per channel as its
// U8U8U8U8_ABGR downscale, which they use for the RGBA8 texture (L, L, L, A) the old path made? A 64x64 level of random (L, A) bytes and
// its RGBA8 twin are each downscaled to the driver's last 2x2 level with its actual padded strides. Every texel of every level is
// compared. 1: exact (mipmapped I/IA textures may go as LA88), 0: not (or a call refused, or out of memory): RGBA8 as before. Once,
// on the render thread (the back end's first such upload); about 1 ms.
extern "C" int vgxvita_la88MipsExact(void) {
    if (!vglForceAllocAligned || !vglFree) return 0;
    const unsigned kW = 64;
    const auto p2 = vgx::swz::MakePlan(kW, kW, 2, true), p4 = vgx::swz::MakePlan(kW, kW, 4, true);
    const unsigned kLevels = p2.levels;
    uint8_t* la = static_cast<uint8_t*>(vglForceAllocAligned(16, p2.linBytes, GL_TRUE));
    uint8_t* rgba = la ? static_cast<uint8_t*>(vglForceAllocAligned(16, p4.linBytes, GL_TRUE)) : nullptr;
    if (!rgba) {
        if (la) vglFree(la);
        port_log("[vgx-vita] GXV_LA88_MIPS: no memory for the downscale check: RGBA8 as before");
        return 0;
    }
    std::memset(la, 0x35, p2.linBytes);
    std::memset(rgba, 0xCE, p4.linBytes);
    uint32_t x = 0x9E3779B9u;
    for (unsigned i = 0; i < kW * kW; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const bool witness = i % kW < kW / 2 && i / kW < kW / 2;
        const uint8_t l = witness ? 37 : static_cast<uint8_t>(x), a = witness ? 213 : static_cast<uint8_t>(x >> 8);
        la[2 * i] = l;
        la[2 * i + 1] = a;
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = l;
        rgba[4 * i + 3] = a;
    }
    bool ok = true;
    for (unsigned j = 0; ok && j + 1 < kLevels; ++j) {
        const unsigned w = kW >> j;
        ok = sceGxmTransferDownscale(SCE_GXM_TRANSFER_FORMAT_U8U8_GR, la + p2.linOff[j], 0, 0, w, w, p2.linStride[j], SCE_GXM_TRANSFER_FORMAT_U8U8_GR,
                                     la + p2.linOff[j + 1], 0, 0, p2.linStride[j + 1], nullptr, 0, nullptr) >= 0 &&
             sceGxmTransferDownscale(SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, rgba + p4.linOff[j], 0, 0, w, w, p4.linStride[j],
                                     SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, rgba + p4.linOff[j + 1], 0, 0, p4.linStride[j + 1], nullptr, 0, nullptr) >= 0;
    }
    XferWait();
    unsigned diff = 0, first = 0xFFFFFFFFu;
    for (unsigned l = 1; ok && l < kLevels; ++l) {
        const unsigned n = (kW >> l) * (kW >> l);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned row = i / p2.w[l], col = i % p2.w[l];
            const auto* a2 = la + p2.linOff[l] + row * p2.linStride[l] + col * 2;
            const auto* a4 = rgba + p4.linOff[l] + row * p4.linStride[l] + col * 4;
            const bool same = a2[0] == a4[0] && a2[0] == a4[1] && a2[0] == a4[2] && a2[1] == a4[3] &&
                              (i != 0 || (a2[0] == 37 && a2[1] == 213));
            if (!same) {
                if (first == 0xFFFFFFFFu) first = l << 16 | i;
                ++diff;
            }
        }
    }
    vglFree(rgba);
    vglFree(la);
    if (!ok) port_log("[vgx-vita] GXV_LA88_MIPS: the transfer unit refused a U8U8 or U8U8U8U8 downscale: RGBA8 as before");
    else if (diff) port_log("[vgx-vita] GXV_LA88_MIPS: U8U8 downscale differs from U8U8U8U8 in %u texels (first: level %u texel %u): RGBA8 as before", diff,
                            first >> 16, first & 0xFFFFu);
    else port_log("[vgx-vita] GXV_LA88_MIPS: exact from 64x64 through 2x2 including padded rows and transfer witness");
    return ok && !diff ? 1 : 0;
}

// ---- vitaGL's cache folders, made when they are needed
// vitaGL stores a compiled shader as <cache>/v3/{v,f}/<HH>/<hash>.gxp, hash = XXH3_64bits of the
// source text as handed to glShaderSource, <HH> = its top byte. The frozen library expects
// all 512 folders at startup. Prepare just the bucket in its actual cache-write path instead:
// cached reads need no mkdir, and normal shader sources need no second hash or joined copy.
extern "C" { extern char vgl_shader_cache_path[256] __attribute__((weak)); }
// (packrefill25) "in play": a stage is loaded, no scene is loading and the screen is not a pause menu or a closed fade
static int PackInPlay() {
    if (!port_scene_loading_key || port_scene_loading_key() == 0) return 0;
    if (port_scene_loading_active && port_scene_loading_active()) return 0;
    return port_screen_quiet && port_screen_quiet() != 0 ? 0 : 1;
}
// (packrefill25, GXV_PACK_MISSLOG) the first miss of a key: one log line (any thread; the set is locked inside)
static void NoteMissLog(const char* path) {
    uint32_t kind = 0;
    uint64_t hash = 0;
    if (!vgx::vita::PackMissLog::ParseName(path, kind, hash) || !g_missLog.First(kind, hash)) return;
    char line[160];
    const unsigned n = g_missOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
    if (vgx::vita::PackMissLog::FormatMiss(line, sizeof line, kind, hash, port_scene_loading_key ? port_scene_loading_key() : 0, PackInPlay(), NowUs() / 1000u, n))
        GXV_BASIC_LOG("%s", line);
}
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
extern "C" int vgxvita_shaderCacheRead_v1(const char* path, const void** bytes, size_t* size) {
    if (!g_shaderPackDone.load(std::memory_order_acquire)) EnsureShaderPackLoaded();
    // (GXV_SHADERPACK_SEED) the package's shaders were switched off after the compiler self-test: so is its pack
    if (g_shaderPackFromSeed && !g_seedFallbackOn.load(std::memory_order_acquire)) { if (bytes) *bytes = nullptr; if (size) *size = 0; return 0; }
    if (!g_shaderPack.Find(path, bytes, size)) {
        if (g_userPack.Entries() && UserPackUsable() && UserPackFind(path, bytes, size)) {   // (cook24, GXV_USERPACK)
            g_userPackHits.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        if (!g_compactRejected.load(std::memory_order_acquire)) {
            EnsureCompactSeedOpen();
            const int i = g_compact.Index(path);
            // The card's loose binary has the same priority as before: leave
            // it to vitaGL, including its ordinary validation and recompile.
            if (i >= 0 && !Exists(path) && g_compact.Read(i, bytes, size)) return 1;
        }
        g_packMisses.fetch_add(1, std::memory_order_relaxed);
        if (g_missLog.Enabled()) NoteMissLog(path);   // (packrefill25)
        if (g_userEnabled && UserPackUsable()) UserPackNoteMiss(path);
        return 0;
    }
    g_shaderPackHits.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> noted{false};
    if (!noted.load(std::memory_order_relaxed) && !noted.exchange(true, std::memory_order_relaxed))
        port_log_summary("[vgx-vita] shader RAM pack lookup: first=1 bytes=%u ABI=%016llx (lookup only; hardened driver validates before use)",
                         static_cast<unsigned>(*size), static_cast<unsigned long long>(ShaderPackAbi()));
    return 1;
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
} // namespace
extern "C" void vgxvita_prepareShaderCacheWrite_v1(const char* path, int flags, SceMode mode) {
    if (flags != (SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC) || mode != 0777 || !path) return;
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return;
    char kind = 0;
    unsigned bucket = 0;
    const size_t rootSize = CacheFileUnder(vgl_shader_cache_path, path, &kind, &bucket);
    if (!rootSize) return;

    // vitaGL changes the root only while its GL owner is quiescent. Serialize our own
    // memoized state in case another SDK caller writes a matching private cache file.
    std::lock_guard<std::mutex> lock(g_bucketMutex);
    if (std::strcmp(g_bucketRoot, vgl_shader_cache_path) != 0) {
        std::memcpy(g_bucketRoot, vgl_shader_cache_path, rootSize + 1);
        std::memset(g_bucketSeen, 0, sizeof(g_bucketSeen));
    }
    uint8_t& bits = g_bucketSeen[kind == 'f'][bucket >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (bucket & 7u));
    if (bits & bit) return;
    char bucketPath[320];
    const int n = std::snprintf(bucketPath, sizeof(bucketPath), "%s/%c/%02X", g_bucketRoot, kind, bucket);
    if (n < 0 || n >= static_cast<int>(sizeof(bucketPath))) return;
    const uint64_t t0 = (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) ? NowUs() : 0;
    const int result = sceIoMkdir(bucketPath, 0777);
    if (result >= 0) ++g_bucketsMade;
    // Vita filesystem EEXIST: a transient mkdir failure must not poison future misses.
    if (result >= 0 || result == static_cast<int>(0x80010011u)) bits |= bit;
    if (PORT_LOGGING || PORT_RUNTIME_DIAGNOSTICS) g_bucketUs += NowUs() - t0;
}
// The read-only second level of the shader cache (see g_seedFallback). Called by __wrap_sceIoOpen only after an open with vitaGL's
// cache-lookup shape (SCE_O_RDONLY, mode 0777) has failed with `failed`: when `path` is a cache file name under vitaGL's cache folder,
// the same name under the package's seed is opened (read-only, mode 0, so this open does not come back here). Returns that handle,
// or `failed` unchanged.
extern "C" SceUID vgxvita_openShaderCacheFallback_v1(const char* path, SceUID failed) {
    if (!g_seedFallbackOn.load(std::memory_order_acquire)) return failed;
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return failed;
    const size_t rootSize = CacheFileUnder(vgl_shader_cache_path, path, nullptr, nullptr);
    if (!rootSize) return failed;
    char seeded[256];
    const int n = std::snprintf(seeded, sizeof(seeded), "%s%s", g_seedFallback, path + rootSize);
    if (n < 0 || n >= static_cast<int>(sizeof(seeded))) return failed;
    const SceUID fd = sceIoOpen(seeded, SCE_O_RDONLY, 0);
    (fd >= 0 ? g_seedFallbackHits : g_seedFallbackMisses).fetch_add(1, std::memory_order_relaxed);
    return fd >= 0 ? fd : failed;
}
// ---- GXV_ASYNC_SHADERS (platform/vgx/src/vgx_async_shaders.h, vgx_backend_gl.cpp AsyncEngine): the Vita's side of the shader worker
// vitaGL with tools/vitagl/patches/0020 compiles a GLSL pair into its cache files from a thread that owns no GL object
// (vglCompileShaderPairToCache) and takes a recursive lock around its translator and the runtime compiler (vglEnableAsyncCompiler) so that
// the render thread's own links and this worker never run the compiler together (the compiler keeps one output, the translator file-level
// state). Nothing here calls GL, the shader patcher or any GXM function.
extern "C" int vglEnableAsyncCompiler(void) __attribute__((weak));
extern "C" int vglCompileShaderPairToCache(const char* vs, const char* fs, GLboolean fsLowPrecision, int* compiled) __attribute__((weak));
extern "C" int port_vita_thread_class(int cls) __attribute__((weak));
extern "C" int port_vita_thread_core3(int part, int planned) __attribute__((weak));
namespace {
std::unordered_set<uint64_t> g_asyncKnownHashes[2]; // (vertex / fragment texts known to be in the cache; only the render thread asks)
struct AsyncStart { void (*fn)(void*); void* arg; };
std::atomic<int> g_asyncMask{-1};                   // the affinity mask the worker ended up with (-1: not placed yet)
std::atomic<SceUID> g_asyncTid{-1};
} // namespace
// asyncsh31: the worker's thread id, set by the worker itself before its first job (-1 before): vita/vgx-game/alloc_wrap.cpp gives this thread newlib's heap only.
extern "C" volatile int vgxvita_asyncWorkerTid = -1;
namespace {
std::atomic<int> g_asyncBasePriority{0};
int AsyncWorkerEntry(SceSize, void* argp) {
    const AsyncStart start = *static_cast<AsyncStart*>(argp);
    // The lowest priority on cores 1-2 first (port_thread_class 4: background work), then the system core when the fourth core is proven and
    // GXV_CORE3_SHADERS allows (default); a refused core leaves it on cores 1-2 (port_vita_thread_core3 puts it back to its planned mask).
    if (port_vita_thread_class) port_vita_thread_class(4);
    else sceKernelChangeThreadPriority(0, 0xBF);
    // hitch33 11: never the system core. port_vita_thread_core3 is not asked any more (core_layout.h answers no anyway); and a hard guard: whatever mask this thread has
    // now, it is set to cores 1-2 and read back; if the system core's bit is still in it, the worker does not run at all (the renderer then has no worker: the gate).
    int mask = port::cores::kHelpers;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), port::cores::kHelpers);
    const int have = sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
    if (have > 0 && (have & port::cores::kSystem) != 0) {
        GXV_BASIC_LOG("async shaders: the worker's core mask is 0x%X after asking for cores 1-2 (the system core is in it): the worker does not run", static_cast<unsigned>(have));
        g_asyncBasePriority.store(sceKernelGetThreadCurrentPriority(), std::memory_order_relaxed);
        g_asyncMask.store(port::cores::kSystem, std::memory_order_release);
        return 0;
    }
    if (have > 0) mask = have;
    g_asyncBasePriority.store(sceKernelGetThreadCurrentPriority(), std::memory_order_relaxed);
    vgxvita_asyncWorkerTid = sceKernelGetThreadId(); // (asyncsh31: from here on this thread's vitaGL allocations are newlib's only)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_asyncMask.store(mask, std::memory_order_release);
    start.fn(start.arg);
    return 0;
}
} // namespace
extern "C" void port_load_split_shader_files(uint64_t* ramHits, uint64_t* userHits, uint64_t* misses) {   // (cook24) the "load-split:" lines
    if (ramHits) *ramHits = g_shaderPackHits.load(std::memory_order_relaxed);
    if (userHits) *userHits = g_userPackHits.load(std::memory_order_relaxed);
    if (misses) *misses = g_packMisses.load(std::memory_order_relaxed);
}
// (packrefill25, GXV_PACK_MISSLOG) called by the link wrapper of shark_compile_shader_extended (vita/vgx-game/wait_wrap.cpp) after each compiler run, any thread:
// the run's microseconds. "In play" = PackInPlay() at that moment (a compile behind a loading screen or at the title is the warm-up's, not the player's).
extern "C" void vgxvita_packNoteCompile_v1(uint64_t us) {
    if (!g_missLog.Enabled()) return;
    g_compilesAll.fetch_add(1, std::memory_order_relaxed);
    const int play = PackInPlay();
    if (play) g_compilesInPlay.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<unsigned> lines{0};
    if (play && lines.fetch_add(1, std::memory_order_relaxed) < 64)   // (the first 64 in-play compiles say where: the stage and how long)
        GXV_BASIC_LOG("pack: a shader compile ran in play: stage=%08x %llu ms (n=%u in play)", port_scene_loading_key ? port_scene_loading_key() : 0u,
                      static_cast<unsigned long long>(us / 1000u), g_compilesInPlay.load(std::memory_order_relaxed));
}
// (packrefill25) called every picture by the back end (StutterFrameEnd); one summary line a minute while the log is on.
extern "C" void vgxvita_packMissTick_v1(void) {
    if (!g_missLog.Enabled()) return;
    static vgx::vita::PackSummary summary(60000);
    vgx::vita::PackSummary::Totals now, delta;
    now.hits = g_shaderPackHits.load(std::memory_order_relaxed);
    now.misses = g_packMisses.load(std::memory_order_relaxed);
    now.userHits = g_userPackHits.load(std::memory_order_relaxed);
    now.compiles = g_compilesAll.load(std::memory_order_relaxed);
    now.compilesInPlay = g_compilesInPlay.load(std::memory_order_relaxed);
    now.distinct = g_missLog.Distinct();
    now.overflow = g_missLog.Overflow();
    uint64_t windowMs = 0;
    char line[320];
    if (summary.Tick(NowUs() / 1000u, now, delta, windowMs) && vgx::vita::PackSummary::FormatSummary(line, sizeof line, delta, windowMs, now)) GXV_BASIC_LOG("%s", line);
}
// hitch33 (revA item 6): the worker writes a compiled shader's cache file AFTER it has released vitaGL's compiler lock (patch 0020: async_compile_to_cache), and not atomically.
// The texts of the pair it is working on are "in flight" from before the call until it has returned (files written and closed): the probe answers "not there yet" for them, and
// vgxvita_asyncShaderSettle_v1 lets the render thread wait before it links / compiles a program that shares one. A folded 32-bit value (no 64-bit atomics here); a collision only
// costs a wait or one more probe. [0] vertex, [1] fragment; 0 = none.
static std::atomic<uint32_t> g_asyncInflight[2];
static inline uint32_t AsyncFold(uint64_t h) { return (static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32)) | 1u; }
extern "C" unsigned vgxvita_asyncShaderSettle_v1(const char* vsJoined, const char* fsJoined, unsigned maxMs) {
    const uint32_t want[2] = {vsJoined ? AsyncFold(XXH3_64bits(vsJoined, std::strlen(vsJoined))) : 0u, fsJoined ? AsyncFold(XXH3_64bits(fsJoined, std::strlen(fsJoined))) : 0u};
    unsigned waited = 0;
    while (waited < maxMs && ((want[0] && g_asyncInflight[0].load(std::memory_order_acquire) == want[0]) ||
                              (want[1] && g_asyncInflight[1].load(std::memory_order_acquire) == want[1]))) {
        sceKernelDelayThread(1000);
        ++waited;
    }
    return waited;
}
extern "C" int vgxvita_asyncShaderInCache_v1(const char* joined, int fragment) {
    if (!joined || &vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return 0;
    const uint64_t hash = XXH3_64bits(joined, std::strlen(joined));
    if (g_asyncInflight[fragment ? 1 : 0].load(std::memory_order_acquire) == AsyncFold(hash)) return 0; // (hitch33: its file is being written now)
    auto& known = g_asyncKnownHashes[fragment ? 1 : 0];
    if (!g_compactRejected.load(std::memory_order_acquire) && known.count(hash)) return 1;
    char path[320];
    if (!vgx::bootwatch::ShaderCachePath(path, sizeof(path), vgl_shader_cache_path, fragment != 0, hash)) return 0;
#if defined(GXV_SHADER_CACHE_PACK) && GXV_SHADER_CACHE_PACK
    {
        if (!g_shaderPackDone.load(std::memory_order_acquire)) EnsureShaderPackLoaded();
        const void* bytes = nullptr;
        size_t size = 0;
        if ((!g_shaderPackFromSeed || g_seedFallbackOn.load(std::memory_order_acquire)) &&
            g_shaderPack.Find(path, &bytes, &size)) { known.insert(hash); return 1; }
        if (g_userPack.Entries() && UserPackUsable() && g_userPack.Find(path, &bytes, &size)) { known.insert(hash); return 1; }   // (cook24)
        if (!g_compactRejected.load(std::memory_order_acquire)) {
            EnsureCompactSeedOpen();
            if (g_compact.Read(g_compact.Index(path), &bytes, &size)) {
                known.insert(hash); return 1;
            }
        }
    }
#endif
    // The same open vitaGL does at a link: the card's cache, then (coldtrace_wrap.cpp's __wrap_sceIoOpen) the package's seed behind it.
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0777);
    if (fd < 0) return 0;
    sceIoClose(fd);
    known.insert(hash);
    return 1;
}
extern "C" unsigned int _newlib_heap_size_user; // (vita/vgx-game/main.cpp: the C heap's size)
// strikersrev25 (the Strikers port's review of r110, hardening): the worker compiles only while the C heap has this much room
// (GXV_ASYNC_SHADERS_HEAP_MIB, default 8, 0 = no check). vitaGL's translator and compiler allocate with vglMalloc, which tries the C heap and,
// when it has no block, falls back to vitaGL's RAM / phycont / VRAM pools; those pools are not locked against the render thread (textures, vertex
// data, uniform rings), the compiler's own lock covers the compiler only. A refused job fails (the engine marks it Failed) and the draw takes
// the render thread's own compile, as for any failed job. Only matters with GXV_ASYNC_SHADERS=1 (off by default since 2.1). mallinfo walks the
// heap (tens of thousands of pieces after a few stages): the answer is kept for half a second; only the worker thread asks.
static bool AsyncHeapRoom() {
    static const long needMib = [] { const char* e = std::getenv("GXV_ASYNC_SHADERS_HEAP_MIB"); return (e && *e) ? std::atol(e) : 8L; }();
    if (needMib <= 0) return true;
    static unsigned stampMs = 0;
    static bool lastOk = true, haveLast = false;
    const unsigned now = NowMs();
    if (haveLast && static_cast<unsigned>(now - stampMs) < 500u) return lastOk;
    const struct mallinfo mi = mallinfo();
    const unsigned long total = _newlib_heap_size_user, used = static_cast<unsigned long>(mi.uordblks);
    lastOk = total > used && (total - used) >= static_cast<unsigned long>(needMib) * 1048576ul;
    stampMs = now;
    haveLast = true;
    return lastOk;
}
extern "C" int vgxvita_asyncShaderCompile_v1(const char* vs, const char* fs, int fsHalf, int* compiled) {
    if (!vglCompileShaderPairToCache) return -1;
    if (!AsyncHeapRoom()) return -2;
    // (hitch33) in flight until the call has returned: see g_asyncInflight
    g_asyncInflight[0].store(vs ? AsyncFold(XXH3_64bits(vs, std::strlen(vs))) : 0u, std::memory_order_release);
    g_asyncInflight[1].store(fs ? AsyncFold(XXH3_64bits(fs, std::strlen(fs))) : 0u, std::memory_order_release);
    const int rc = vglCompileShaderPairToCache(vs, fs, fsHalf ? GL_TRUE : GL_FALSE, compiled);
    g_asyncInflight[0].store(0u, std::memory_order_release);
    g_asyncInflight[1].store(0u, std::memory_order_release);
    return rc;
}
// Makes vitaGL's lock and the worker thread (on the calling thread, which owns GL). 1 on success, with `where` saying where the worker runs;
// 0 with the reason in `where` (the renderer then keeps every shader on the blocking path).
extern "C" int vgxvita_asyncShaderStart_v1(void (*fn)(void*), void* arg, char* where, size_t whereSize) {
    static bool done = false;
    if (done || !fn) return 0;
    done = true;
    if (!vglEnableAsyncCompiler || !vglCompileShaderPairToCache) {
        std::snprintf(where, whereSize, "this vitaGL has no patch 0020 (vglCompileShaderPairToCache)");
        return 0;
    }
    if (&is_shark_online == nullptr || !is_shark_online) {
        std::snprintf(where, whereSize, "the shader compiler is not online");
        return 0;
    }
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) {
        std::snprintf(where, whereSize, "vitaGL has no shader cache path");
        return 0;
    }
    const int lock = vglEnableAsyncCompiler();
    if (lock < 0) {
        std::snprintf(where, whereSize, "vitaGL's compiler lock failed: 0x%08X", static_cast<unsigned>(lock));
        return 0;
    }
    static AsyncStart start;
    start = AsyncStart{fn, arg};
    const long stackKb = vgx::EnvInt("GXV_ASYNC_SHADERS_STACK_KB", 1024);
    const SceUID tid = sceKernelCreateThread("smg_shaders", &AsyncWorkerEntry, 0xBF, static_cast<SceSize>((stackKb < 128 ? 128 : stackKb) * 1024), 0,
                                             port::cores::kHelpers, nullptr);
    if (tid < 0) {
        std::snprintf(where, whereSize, "thread create failed: 0x%08X", static_cast<unsigned>(tid));
        return 0;
    }
    g_asyncTid.store(tid, std::memory_order_relaxed);
    const int rc = sceKernelStartThread(tid, sizeof(start), &start);
    if (rc < 0) {
        std::snprintf(where, whereSize, "thread start failed: 0x%08X", static_cast<unsigned>(rc));
        sceKernelDeleteThread(tid);
        return 0;
    }
    for (int i = 0; i < 300 && g_asyncMask.load(std::memory_order_acquire) < 0; ++i) sceKernelDelayThread(1000); // (placed within a few ms)
    const int mask = g_asyncMask.load(std::memory_order_acquire);
    if (mask >= 0 && (mask & port::cores::kSystem) != 0) { // (hitch33 11: the guard refused: the worker returns without running; no worker)
        std::snprintf(where, whereSize, "the worker could not be kept off the system core (mask 0x%X): no worker", static_cast<unsigned>(mask));
        return 0;
    }
    if (mask < 0) { // (review 14 Nov: the thread exists and will run the loop when scheduled; returning 0 here let the caller free the engine under it)
        std::snprintf(where, whereSize, "started; its core mask not reported within 300 ms (it checks itself and stays off the system core)");
        return 1;
    }
    std::snprintf(where, whereSize, "on cores 1-2 (mask 0x%X) at the lowest priority (0x%X), never the system core", static_cast<unsigned>(mask),
                  static_cast<unsigned>(g_asyncBasePriority.load(std::memory_order_relaxed)));
    return 1;
}
// The render thread is about to compile (or wait for the worker's compile) itself while the worker may hold the compiler's lock: the worker's
// priority becomes the render thread's, then goes back. (The lock has no priority inheritance; the worker runs at the lowest priority.)
extern "C" void vgxvita_asyncShaderBoost_v1(int on) {
    const SceUID tid = g_asyncTid.load(std::memory_order_relaxed);
    if (tid < 0) return;
    sceKernelChangeThreadPriority(tid, on ? sceKernelGetThreadCurrentPriority() : g_asyncBasePriority.load(std::memory_order_relaxed));
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
extern "C" __attribute__((format(printf, 1, 2))) int msc_vgl_printf(const char* format, ...) {
    if (!PORT_LOGGING) return 0;
    // (GXV_ASYNC_SHADERS: the compiler's messages now also come from the shader worker thread: the two counters are atomic)
    static std::atomic<uint32_t> countAtomic{0}, compilerWarningsAtomic{0};
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    vgx::resourceaudit::DriverMessage(text); // Separate bounded basic-log path before ordinary warning/rate limits.
    // (2.3: vitaGL's fail-stop messages - it aborts right after them - reach the player's basic log)
    if (std::strstr(text, "fatal") || std::strstr(text, "stopping") || std::strstr(text, "could not be allocated")) {
        if (port_log_summary) port_log_summary("FATAL: [vitaGL] %s", text);
        port_log_flush();
    }
    if (std::strstr(text, "Shader Compiler: W]")) {
        const uint32_t compilerWarnings = compilerWarningsAtomic.fetch_add(1, std::memory_order_relaxed) + 1;
        if (compilerWarnings > 6) return 0;
        if (compilerWarnings == 6) port_log("[vitaGL] (further shader compiler warnings are not logged)");
    }
    const uint32_t count = countAtomic.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 400 && count % 500 != 0) return 0;
    size_t length = std::strlen(text);
    while (length != 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) text[--length] = '\0';
    if (count > 400) port_log("[vitaGL] (message %u) %s", count, text);
    else port_log("[vitaGL] %s", text);
    return static_cast<int>(length);
}

#endif // __vita__
