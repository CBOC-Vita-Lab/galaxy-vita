// PS Vita platform layer of vgx: the counterpart of src/vgx_host.cpp and
// src/vgx_host_egl.cpp.
//
//   - a render thread that owns the vitaGL context. In the renderer's default
//     mode it is the "player" (src/vgx_rec.h): it decodes the record stream the
//     game thread writes and executes the resulting command buffers itself. In
//     the two other modes it executes command buffers handed over
//     synchronously by the game thread;
//   - vitaGL start-up with explicit memory pool sizes, shader cache directory
//     ux0:data/mscharged/shadercache and fixed varying semantics;
//   - present: the back end calls PresentDisplayCopy (its present hook) at
//     every display copy, while the finished picture is still in the EFB
//     framebuffer object; it is blitted to the display and the buffers are
//     swapped;
//   - renderer settings a console needs: a texture budget and the shader
//     warm-up list (see EnsureThread);
//   - shader start-up: vitaGL's on-disk cache of compiled shaders lives in a
//     folder whose name carries a key of everything that decides what a
//     compiled shader looks like (ShaderCacheFolder), and while shaders are
//     being compiled the back end draws a progress bar that ShowProgressBar
//     puts on the display;
//   - vitaGL's own threads and pools set up as the one GameCube port known to
//     run on consoles has them (docs/acgc-vita-port-findings.md): a larger
//     shader patcher, the collector thread on core 2, sceGxm's display-queue
//     thread on core 1;
//   - a compiled-shader cache that came inside the package (app0:shadercache)
//     is copied to the writable cache folder once (SeedShaderCache), so that a
//     first start need not compile: both reference ports that run on consoles
//     ship their compiled shaders (ACGC as GXP files, melee-Vita as a warm
//     cache next to a writable one);
//   - the desktop graphics-library entry points the runtime links against
//     (aurora_*, PAD).
//
// Nothing in vgx/src is modified for the Vita. This file and include/GLES2
// adapt vitaGL to what the back end expects; see include/GLES2/gl2.h for the
// two GL calls routed through here.
//
// NOT RUN YET: this file has been compiled and linked, never executed (no
// Vita or emulator in the build environment). Everything it prints is meant
// to make the first run diagnosable: look for "[vgx-vita]" in log.txt.
//
// Debug controls (hold SELECT+START, then press):
//   D-pad down   show the text console (log tail) instead of the picture, and back
//                (it is copied over each presented frame, so it only updates
//                while the game presents frames)
//   D-pad up     flip the presented picture vertically
//   D-pad right  stretch to the whole screen / 4:3 with side bars
// Marker files in ux0:data/mscharged (looked at once, at start-up):
//   vgx_verbose          the renderer's verbose log
//   vgx_widescreen       forces full screen (see DecideWidescreen() below; since round 6 full
//                        screen is the default whenever the game's widescreen setting is on)
//   vgx_play_frame       the game thread decodes each frame at present and
//   vgx_play_immediate   ... after every call (the renderer's GXV_PLAY
//                        modes "frame" and "immediate"; default "thread")
// The renderer's own switches are environment variables (GXV_*); on the
// Vita they are set through ux0:data/mscharged/env.txt (vita_main.cpp), e.g.
//   GXV_TEXBUDGET=48     texture budget in MiB (default here: 32)
//   GXV_SHADERLIST=path  another warm-up list file
//   GXV_PASSISO=1        the renderer's pass isolation
//   GXV_VITA_WIDE=0|1        4:3 with side bars / full screen, whatever the game's setting
//   GXV_VITA_RENDER=WxH      internal picture size (default: the game's own, 640x448 in a
//                            match), scaled to the screen at present. 480x272 or 640x362 if
//                            the GPU ever becomes the limit; 960x544 for a sharper picture
//                            (costs video memory and fill rate: see RenderSize() below)
#if defined(__vita__)

#define VGX_VITA_NO_GL_ROUTING
#include <GLES2/gl2.h>

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>

#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "plat_frame_stats.h"
#include "plat_split.h"
#include "vgx_rec.h"
#include "vgx_shadergen.h"
#include "vgx_recipe_seed.h"
#include "vgx_state.h"
#include "vita_platform.h"
#include "vgx_display_rect.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kScreenW = 960, kScreenH = 544;
constexpr const char* kShaderCacheDir = GXV_VITA_DATA_DEVICE "/shadercache";
// What decides the compiled form of a shader besides its GLSL text (which is vitaGL's own
// key inside the folder): the vitaGL build (its translator, its serialisation, its options)
// and the varying semantics fixed in StartGl. The build system can pass the first as
// MSC_VGL_ID ("<commit> <options>", the line vita/Makefile.vitadeps writes to
// obj/vitagl/options.txt); without it every compile of this file gets a key of its own,
// which is safe (a new package compiles its shaders again once) but invalidates more often
// than necessary. Bump kHostShaderAbi when the semantic bindings in StartGl change.
#ifndef MSC_VGL_ID
#define MSC_VGL_ID "unidentified vitaGL, host built " __DATE__ " " __TIME__
#endif
constexpr const char* kHostShaderAbi = "tex0-7 on TEXCOORD0-7, col0/1 on TEXCOORD8/9, uv on TEXCOORD0; mode GLOBAL; 1";
// vitaGL's collector thread at its default priority, but on the audio core: the game
// thread's core is the one that has no time to spare.
constexpr int kGcPriority = 0x10000100;
// Every shader key the renderer has needed so far; a run that finds the file
// generates, compiles and links all of them in its first frame instead of in
// the middle of play. Derived from playing, so it lives next to vitaGL's
// cache of the compiled shaders and is not part of the package.
constexpr const char* kShaderListFile = GXV_VITA_DATA_DEVICE "/shadercache/vgx_shaderlist.bin";
// Decoded guest textures the renderer keeps (RGBA8 bytes). Its own
// measurement: a match frame's working set is about 20 MiB, and without a
// budget the cache grows to 52-61 MiB over a match; video memory here is
// 109 MiB for everything (display buffers, EFB, copy targets, textures).
constexpr uint64_t kTextureBudget = 32ull * 1024 * 1024;
constexpr const char* kVerboseMarker = GXV_VITA_DATA_DEVICE "/vgx_verbose";
constexpr const char* kWideMarker = GXV_VITA_DATA_DEVICE "/vgx_widescreen";
constexpr size_t kRenderStack = 4u * 1024u * 1024u;   // the Cg compiler runs on this thread

// ---- what the routed GL calls record (render thread only)
bool g_scissorOn = false;
void ParkScissor();     // (defined with vgxvita_glScissor below)
void UnparkScissor();

// ---- render thread hand-over (the renderer's "frame" and "immediate" modes)
std::mutex g_mtx;
std::condition_variable g_cv;
const vgx::CmdBuf* g_jobBuf = nullptr;
bool g_started = false, g_startOk = false, g_threadCreated = false;
char g_startError[512] = "";

// ---- present state. The flags are set by the game thread (debug controls).
std::atomic<bool> g_flipY{false}, g_stretch{false}, g_showConsole{false};
uint64_t g_presented = 0;       // render thread
uint64_t g_lastPresentUs = 0;
vgx::backend::Stats g_lastStats;

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

bool Exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// FULL SCREEN (the owner's requirement after the first console runs, which showed the picture
// in a 4:3 box). With `widescreen = true` in Config.toml (the default) the game runs in its own
// 16:9 mode: it draws an ANAMORPHIC 16:9 picture into its 640-pixel-wide frame buffer, lays out
// 2D for a 16:9 screen, and the runtime fits the 3D projections to the shape of the surface we
// report (dynamic_aspect.cpp). That picture is only right when it is stretched over a 16:9
// screen: in a 4:3 box the HUD and menus are squeezed to three quarters of their width. So:
//   game's widescreen on  -> surface reported as 960x544, picture stretched over the whole
//                            screen (the Vita's 960x544 is 0.7 % taller than 16:9; the runtime
//                            gives the 3D that much more vertical field of view, 2D is
//                            stretched by it, which nobody can see);
//   game's widescreen off -> surface 640x480, the game's 4:3 picture in a 725x544 box with bars.
// The game's setting is taken from the window the runtime asks for in aurora_initialize (854x480
// with widescreen or "force 16:9" on, 640x480 without: runtime/src/main.cpp), so this file needs
// nothing from the runtime's configuration headers. GXV_VITA_WIDE=0|1 in env.txt overrides it
// (1 with the game's setting off stretches a 4:3 picture: everything too wide); the marker file
// vgx_widescreen forces it on, as before round 6. Checked on the PC build with
// GXV_SURFACE=960x544 (vgx/NOTES.md, round 6, "FULL SCREEN").
std::atomic<bool> g_wide{false};
bool Widescreen() { return g_wide.load(std::memory_order_relaxed); }
void DecideWidescreen(const AuroraConfig* config) {
    bool wide = config && config->windowHeight != 0 && config->windowWidth * 3u > config->windowHeight * 4u;
    const char* why = wide ? "the game's widescreen setting is on" : "the game's widescreen setting is off";
    if (const char* e = std::getenv("GXV_VITA_WIDE"); e && *e) {
        wide = std::atoi(e) != 0;
        why = "GXV_VITA_WIDE in env.txt";
    } else if (!wide && Exists(kWideMarker)) {
        wide = true;
        why = "marker file vgx_widescreen";
    }
    g_wide.store(wide);
    std::fprintf(stderr, "[vgx-vita] full screen: %s (%s; the runtime asked for a %ux%u window)\n", wide ? "ON, 960x544 surface, picture stretched" : "off, 640x480 surface in a 4:3 box",
                 why, config ? static_cast<unsigned>(config->windowWidth) : 0u, config ? static_cast<unsigned>(config->windowHeight) : 0u);
}

// GXV_VITA_RENDER=WxH (env.txt): the size of the picture the renderer draws into, instead of
// the game's own frame-buffer size (640x448 in a match, 640x480 / 640x528 in some screens).
// Every pass target and copy scales with it (the back end's fixed-size mode, the PC harness's
// GXV_RENDER_SIZE). SMALLER (480x272 = a quarter of the screen's pixels, 640x362) is the lever
// if a console turns out to be limited by the GPU: 480x272 draws 43 % of the default's pixels;
// the picture is visibly soft at 480x272 and close to the default at 640x362 (pictures:
// build/host-gl/checks/r6/wide_lowres_sheet.png). LARGER (960x544) is sharper and costs 1.8
// times the pixels and about 3 MiB more video memory for the main target alone.
void RenderSize(int& w, int& h) {
    w = h = 0;
    const char* e = std::getenv("GXV_VITA_RENDER");
    if (!e || !*e) return;
    int rw = 0, rh = 0;
    if (std::sscanf(e, "%dx%d", &rw, &rh) == 2 && rw >= 160 && rh >= 120 && rw <= 960 && rh <= 544) {
        w = rw;
        h = rh;
        std::fprintf(stderr, "[vgx-vita] GXV_VITA_RENDER: internal picture %dx%d, scaled to the screen at present\n", w, h);
    } else {
        std::fprintf(stderr, "[vgx-vita] GXV_VITA_RENDER=%s ignored (WxH from 160x120 to 960x544)\n", e);
    }
}

void LogMemory(const char* when) {
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    std::fprintf(stderr, "[vgx-vita] memory %s: system free main %d KiB, cdram %d KiB, phycont %d KiB\n", when, free.size_user / 1024,
                 free.size_cdram / 1024, free.size_phycont / 1024);
}

void LogPools(const char* when) {
    std::fprintf(stderr, "[vgx-vita] vitaGL pools %s (free/total KiB): vram %u/%u, ram %u/%u, phycont %u/%u, newlib heap %u/%u\n", when,
                 static_cast<unsigned>(vglMemFree(VGL_MEM_VRAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_VRAM) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_RAM) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_RAM) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_PHYCONT) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_PHYCONT) / 1024),
                 static_cast<unsigned>(vglMemFree(VGL_MEM_EXTERNAL) / 1024), static_cast<unsigned>(vglMemTotal(VGL_MEM_EXTERNAL) / 1024));
}

// Called by vitaGL on sceGxm's display-queue thread with the buffer that is
// about to be shown (960 x 544, A8B8G8R8, 960 pixels per row): the place to
// put the text console on screen while the renderer keeps the display.
void DisplayCallback(void* framebuffer) {
    if (!g_showConsole.load(std::memory_order_relaxed)) return;
    const uint32_t* console = VitaConsole::Pixels();
    if (console) std::memcpy(framebuffer, console, static_cast<size_t>(kScreenW) * kScreenH * 4u);
}

// A trivial GLSL pair through the whole chain (GLSL -> Cg translation, the
// shader compiler module, program link). If this fails nothing the renderer
// generates can work, and the reason is in the returned text.
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
    const GLint location = glGetUniformLocation(program, "u_add");
    std::fprintf(stderr, "[vgx-vita] shader compiler self-test passed in %u ms (u_add at location %d)\n",
                 static_cast<unsigned>((NowUs() - t0) / 1000u), static_cast<int>(location));
    glDeleteProgram(program);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    return true;
}

void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h);

// ux0:data/mscharged/shadercache/<8 hex digits>: the folder vitaGL keeps compiled shaders in.
// A package whose key differs starts with an empty folder (the old one is left behind; it is
// some hundred small files and can be deleted by hand).
const char* ShaderCacheFolder() {
    static char path[160] = "";
    if (path[0]) return path;
    char id[512];
    std::snprintf(id, sizeof(id), "%s|%s|gen %u", MSC_VGL_ID, kHostShaderAbi, static_cast<unsigned>(vgx::kShaderGenVersion));
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* c = id; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    std::snprintf(path, sizeof(path), "%s/%08x", kShaderCacheDir, static_cast<unsigned>(h));
    std::fprintf(stderr, "[vgx-vita] compiled-shader cache: %s (key of: %s)\n", path, id);
    return path;
}

// The back end's progress hook (vgx::backend::SetProgressDisplay): a bar has been drawn into
// the display's back buffer in the middle of a frame; show it.
void ShowProgressBar(uint32_t done, uint32_t total) {
    static uint32_t shown = 0;
    if (shown++ == 0) std::fprintf(stderr, "[vgx-vita] shaders are being compiled: progress bar on screen (%u of %u units)\n", done, total);
    if (VitaConsole::OwnsDisplay()) return;
    vglSwapBuffers(GL_FALSE);
}

// ---- a cache that came with the package
// Layout inside the package (the build adds it when it has one; none of today's packages do):
//   app0:shadercache/<key>/v<n>/{v,f}/<HH>/<hash>.gxp   vitaGL's cache folder of a finished session,
//                                                        copied from ux0:data/mscharged/shadercache/<key>
//                                                        (empty bucket folders need not be packed)
//   app0:shadercache/vgx_shaderlist.bin             that session's warm-up list
// <key> is the folder name ShaderCacheFolder computes, so a seed made with another vitaGL
// build, other bindings or another shader generator is simply not found. Files that already
// exist in the writable folder are kept (they are this console's own compiles). The copy
// happens once: the marker file "seeded" is written when it has run to the end.
constexpr const char* kSeedDir = "app0:shadercache";
constexpr unsigned kSeedMaxFiles = 4096;
// The folder name vitaGL cdbba42 puts under the cache path ("v<SHADER_CACHE_MAGIC>", source/
// shared.h). Only used to make <cache>/v2/v/00 before vitaGL starts, which stops it creating
// all 512 bucket folders; with another vitaGL the only effect of a wrong value is that it
// creates them on the first start, as before round 6.
constexpr const char* kVglCacheVersionDir = "v2";

// What the first console run showed (hw/run2-gl): "123 files, 133 KiB, 0 failed, 55272 ms" for
// the seed copy, i.e. 55 s of black screen on the first start of a package. The copy made, on
// the memory card: 123 small files, their ~110 bucket folders, and then ALL 512 bucket folders
// (EnsureBuckets, because vitaGL only creates them when <cache>/v2/v/00 does not exist yet),
// with one more stat per file. That is about 750 directory operations for 133 KiB of data; a
// card creates a directory entry in tens of milliseconds. (Not measurable in the emulator,
// where the whole copy takes 24 ms.) Since round 6:
//   - only the bucket folders that hold seeded files are made here, plus v/00 and f/00;
//   - every other bucket is made when a shader that hashes into it is first handed to vitaGL
//     (vgxvita_glShaderSource below computes vitaGL's file name), i.e. just before the one
//     compile whose result it stores; a seeded start makes none;
//   - no stat for files that go into a folder this copy has just made; one read per file;
//   - the log line gives the time spent making folders and copying files separately.
// Expected on the first start: ~130 folders + ~145 files instead of ~750 + 123 operations.
// GXV_VITA_SEED=app0 (env.txt) copies nothing: vitaGL reads the packaged folder in place
// (read-only: what this console compiles itself is then not kept for the next start).
struct SeedCopyStats {
    unsigned files = 0, bytes = 0, failed = 0, folders = 0, kept = 0;
    uint64_t folderUs = 0, fileUs = 0;
};
bool CopyFile(const char* from, const char* to, bool mayExist, SeedCopyStats& st) {
    if (mayExist && Exists(to)) { ++st.kept; return true; }
    static char buffer[64 * 1024];
    const SceUID in = sceIoOpen(from, SCE_O_RDONLY, 0);
    if (in < 0) return false;
    const SceUID out = sceIoOpen(to, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) { sceIoClose(in); return false; }
    bool ok = true;
    for (;;) {
        const int n = sceIoRead(in, buffer, sizeof(buffer));
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (sceIoWrite(out, buffer, static_cast<SceSize>(n)) != n) { ok = false; break; }
        st.bytes += static_cast<unsigned>(n);
        if (n < static_cast<int>(sizeof(buffer))) break;   // (a short read is the end of the file)
    }
    sceIoClose(in);
    sceIoClose(out);
    if (!ok) sceIoRemove(to); // never leave half a program behind: vitaGL would load it
    return ok;
}
// `fresh`: the destination folder was made by this copy, so nothing in it can exist yet.
void CopyTree(const char* from, const char* to, int depth, bool fresh, SeedCopyStats& st) {
    const SceUID dir = sceIoDopen(from);
    if (dir < 0) return;
    SceIoDirent entry;
    for (;;) {
        std::memset(&entry, 0, sizeof(entry));
        if (sceIoDread(dir, &entry) <= 0 || st.files + st.failed >= kSeedMaxFiles) break;
        if (entry.d_name[0] == '.') continue;
        // (a seed taken from a finished session may hold that session's own marker)
        if (std::strcmp(entry.d_name, "seeded") == 0) continue;
        char a[320], b[320];
        if (std::snprintf(a, sizeof(a), "%s/%s", from, entry.d_name) >= static_cast<int>(sizeof(a)) ||
            std::snprintf(b, sizeof(b), "%s/%s", to, entry.d_name) >= static_cast<int>(sizeof(b)))
            continue;
        const uint64_t t0 = NowUs();
        if (SCE_S_ISDIR(entry.d_stat.st_mode)) {
            if (depth >= 3) continue;
            const bool made = sceIoMkdir(b, 0777) >= 0;
            if (made) ++st.folders;
            st.folderUs += NowUs() - t0;
            CopyTree(a, b, depth + 1, made, st);
        } else {
            if (CopyFile(a, b, !fresh, st)) ++st.files; else ++st.failed;
            st.fileUs += NowUs() - t0;
        }
    }
    sceIoDclose(dir);
}
// <cache>/v2/v/00 (and f/00): with the first of them present vitaGL does not create its 512
// bucket folders at start-up (source/vgl.c: a failed mkdir of ".../v/00" means "they are all
// there"). Every bucket a shader needs is made by vgxvita_glShaderSource instead.
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
// The marker file <cache>/seeded holds the size and date of the package's eboot.bin: a newly
// installed package (which may bring a larger seed under the same key, as the one of round 6
// did: 35 vertex shaders changed their text) is looked through once more, and only the files
// this console does not have yet are written. (Markers written before round 6 are empty.)
void PackageStamp(char* out, size_t n) {
    SceIoStat st;
    std::memset(&st, 0, sizeof(st));
    if (sceIoGetstat("app0:eboot.bin", &st) < 0) { std::snprintf(out, n, "no eboot"); return; }
    std::snprintf(out, n, "%lld %04d-%02d-%02d %02d:%02d:%02d", static_cast<long long>(st.st_size), static_cast<int>(st.st_mtime.year),
                  static_cast<int>(st.st_mtime.month), static_cast<int>(st.st_mtime.day), static_cast<int>(st.st_mtime.hour),
                  static_cast<int>(st.st_mtime.minute), static_cast<int>(st.st_mtime.second));
}
bool MarkerHas(const char* marker, const char* stamp) {
    const SceUID f = sceIoOpen(marker, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    char text[80];
    const int n = sceIoRead(f, text, sizeof(text) - 1);
    sceIoClose(f);
    if (n < 0) return false;
    text[n] = 0;
    return std::strcmp(text, stamp) == 0;
}
// Returns the folder vitaGL is to use as its cache.
const char* SeedShaderCache(const char* folder) {
    const char* key = std::strrchr(folder, '/');
    static char seed[200];
    char marker[200];
    std::snprintf(seed, sizeof(seed), "%s%s", kSeedDir, key ? key : "/none");
    std::snprintf(marker, sizeof(marker), "%s/seeded", folder);
    SeedCopyStats st;
    if (!Exists(seed)) {
        MakeFirstBuckets(folder, st);
        std::fprintf(stderr, "[vgx-vita] no compiled shaders in the package for this key (%s): they are compiled on this console as needed\n", seed);
        return folder;
    }
    if (const char* e = std::getenv("GXV_VITA_SEED"); e && std::strcmp(e, "app0") == 0) {
        std::fprintf(stderr, "[vgx-vita] GXV_VITA_SEED=app0: vitaGL reads the package's compiled shaders in place (%s); nothing is copied, and shaders "
                             "compiled on this console are not kept\n", seed);
        return seed;
    }
    char stamp[80];
    PackageStamp(stamp, sizeof(stamp));
    if (MarkerHas(marker, stamp)) return folder;
    const uint64_t t0 = NowUs();
    CopyTree(seed, folder, 0, false, st);
    MakeFirstBuckets(folder, st);
    std::fprintf(stderr, "[vgx-vita] compiled shaders from the package copied to %s: %u files (%u KiB, %u ms), %u already there, %u failed; %u folders (%u ms); %u ms in all\n",
                 folder, st.files - st.kept, st.bytes / 1024u, static_cast<unsigned>(st.fileUs / 1000u), st.kept, st.failed, st.folders,
                 static_cast<unsigned>(st.folderUs / 1000u), static_cast<unsigned>((NowUs() - t0) / 1000u));
    if (st.failed == 0) {
        const SceUID f = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (f >= 0) {
            sceIoWrite(f, stamp, static_cast<SceSize>(std::strlen(stamp)));
            sceIoClose(f);
        }
    }
    return folder;
}
// The warm-up list of the session the packaged cache came from, unless this console has
// a list of its own already.
void SeedShaderList() {
    char seed[200];
    std::snprintf(seed, sizeof(seed), "%s/vgx_shaderlist.bin", kSeedDir);
    if (Exists(kShaderListFile) || !Exists(seed)) return;
    unsigned bytes = 0;
    SeedCopyStats st;
    const bool ok = CopyFile(seed, kShaderListFile, false, st);
    bytes = st.bytes;
    std::fprintf(stderr, "[vgx-vita] warm-up list from the package: %s (%u bytes)\n", ok ? "copied" : "copy FAILED", bytes);
}

// sceGxm's display-queue thread and vitaGL's collector are created inside the libraries with
// affinity "any", at priorities that can take core 0 from the game thread. They are found by
// name in the process's thread ids (the walk the ACGC port does, vita_platform.c
// vita_pin_hidden_threads) and moved: the display queue to the render thread's core, the
// collector to the audio core (it is created there already, see StartGl; this covers a
// vitaGL that ignores the request). Threads can appear late, so this runs again at a few
// presents (PresentDisplayCopy).
void PinLibraryThreads(bool log) {
    SceKernelThreadInfo info;
    int missesInRow = 0, moved = 0, seen = 0;
    for (SceUID uid = 0x40010001; uid < 0x40020000 && missesInRow < 500; ++uid) {
        if (sceKernelGetThreadmgrUIDClass(uid) != SCE_KERNEL_TMID_Thread) { ++missesInRow; continue; }
        std::memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(uid, &info) != 0) { ++missesInRow; continue; }
        missesInRow = 0;
        ++seen;
        int target = 0;
        if (std::strstr(info.name, "Garbage Collector")) target = VitaThreads::CoreMask(VitaThreads::kCollector);
        else if (!std::strcmp(info.name, "SceGxmDisplayQueue")) target = VitaThreads::CoreMask(VitaThreads::kDisplayQueue);
        if (target && info.currentCpuAffinityMask != static_cast<SceUInt32>(target)) {
            const int res = sceKernelChangeThreadCpuAffinityMask(uid, target);
            ++moved;
            std::fprintf(stderr, "[vgx-vita] thread \"%s\" (priority 0x%X) moved to core mask 0x%X: %s\n", info.name, static_cast<unsigned>(info.currentPriority),
                         static_cast<unsigned>(target), res >= 0 ? "ok" : "refused");
        }
    }
    // (Vita3K does not implement sceKernelGetThreadmgrUIDClass: there the scan sees nothing,
    // and only a console exercises it. Found by the emulator workstream.)
    if (log) std::fprintf(stderr, "[vgx-vita] library threads: %d threads seen, %d moved%s\n", seen, moved,
                          seen == 0 ? " (none seen: expected in the emulator, a fault on a console)" : "");
}

bool StartGl() {
    // The shader compiler module is the user's own copy of a system file; all
    // of the renderer's shaders are compiled with it on first use.
    const bool compilerA = Exists("ur0:data/libshacccg.suprx"), compilerB = Exists("ur0:data/external/libshacccg.suprx");
    std::fprintf(stderr, "[vgx-vita] shader compiler module: ur0:data/libshacccg.suprx %s, ur0:data/external/libshacccg.suprx %s\n",
                 compilerA ? "present" : "absent", compilerB ? "present" : "absent");
    if (!compilerA && !compilerB) {
        std::snprintf(g_startError, sizeof(g_startError),
                      "The shader compiler module is missing.\n"
                      "Put libshacccg.suprx into ur0:data/ (on a console: extract it with\n"
                      "ShaRKBR33D or the PSM runtime guide; in Vita3K: copy the same file into\n"
                      "the emulator's ur0/data folder).");
        return false;
    }

    LogMemory("before vitaGL");
    SceKernelFreeMemorySizeInfo free;
    std::memset(&free, 0, sizeof(free));
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    // vitaGL's general pool (its rings and uniform ring, vertex and index data, everything it
    // allocates "for the CPU"). Leave 40 MiB of main memory to the system for sceGxm's own
    // buffers (16 MiB parameter buffer), thread stacks and the shader compiler; take at most
    // 96 MiB (GXV_VITA_VGL_RAM_MIB in env.txt changes the limit).
    // Measured in Vita3K over a whole match and the start of the next (v3k/NOTES.md session 4,
    // run c1full, with the limit at 64 MiB as it was until then): the pool was FULL (65.3 MiB
    // at its peak, 35 allocations of 3-5 MiB refused) and vitaGL put the overflow, up to
    // 19.2 MiB at once, into the physically contiguous pool. A console in the 365 MiB mode may
    // have no such memory to give, and the overflow would then go to video memory, which was
    // at 91.6 of 109 MiB itself. Main memory is the budget with room: about 85 MiB unassigned.
    const int64_t reserve = 40ll * 1024 * 1024;
    int64_t ramLimitMib = 96;
    if (const char* e = std::getenv("GXV_VITA_VGL_RAM_MIB"); e && *e) ramLimitMib = std::atoi(e);
    if (ramLimitMib < 16) ramLimitMib = 16;
    if (ramLimitMib > 192) ramLimitMib = 192;   // (vitaGL caps an uncached pool at 200 MiB)
    int64_t ram = static_cast<int64_t>(free.size_user) - reserve;
    if (ram > ramLimitMib * 1024 * 1024) ram = ramLimitMib * 1024 * 1024;
    if (ram < 16ll * 1024 * 1024) ram = 16ll * 1024 * 1024;
    // Video memory (display buffers, render targets, textures): what is free MINUS what sceGxm
    // takes for itself when vitaGL initialises it, and whatever physically contiguous memory
    // the system offers. FIRST CONSOLE RUN (5 Oct, hw/run2-gl): this asked for all free video
    // memory but 1 MiB (111.6 of 112.6 MiB); sceGxmInitialize had taken its 16 MiB parameter
    // buffer from the same budget by the time vitaGL allocated its pool, the allocation failed,
    // vitaGL carried on without a video-memory pool, and the first query of that pool
    // dereferenced a null mspace in SceLibKernel (data abort at 0x1BC). Vita3K does not limit
    // memory, so the emulator never showed it. GXV_VITA_CDRAM_RESERVE_MIB in env.txt changes
    // the reserve (default 18 = parameter buffer 16 + 2).
    int64_t cdramReserveMib = 18;
    if (const char* e = std::getenv("GXV_VITA_CDRAM_RESERVE_MIB"); e && *e) cdramReserveMib = std::atoi(e);
    if (cdramReserveMib < 1) cdramReserveMib = 1;
    int64_t cdram = static_cast<int64_t>(free.size_cdram) - cdramReserveMib * 1024 * 1024;
    cdram &= ~static_cast<int64_t>(256 * 1024 - 1);
    if (cdram < 0) cdram = 0;
    const int64_t phycont = free.size_phycont > 0 ? free.size_phycont : 0;
    std::fprintf(stderr, "[vgx-vita] vitaGL pools requested: ram %u KiB, cdram %u KiB, phycont %u KiB\n",
                 static_cast<unsigned>(ram / 1024), static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(phycont / 1024));

    // Varyings get fixed semantics. vgx compiles vertex and fragment shaders
    // independently and links them in many combinations, so the translator's
    // name-order assignment (which depends on which shader of a pair it sees
    // first) cannot be used. v_uv is the back end's EFB-copy program, which
    // never shares a shader with the generated ones.
    vglSetSemanticBindingMode(VGL_MODE_GLOBAL);
    static const char* const kTex[8] = {"v_tex0", "v_tex1", "v_tex2", "v_tex3", "v_tex4", "v_tex5", "v_tex6", "v_tex7"};
    for (int i = 0; i < 8; ++i) vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>(kTex[i]), i, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col0"), 8, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_col1"), 9, VGL_TYPE_TEXCOORD);
    vglAddSemanticBinding(reinterpret_cast<const GLchar* const*>("v_uv"), 0, VGL_TYPE_TEXCOORD);

    // sceGxm sizes a render target for a number of scenes per frame (1 to 8);
    // a further scene on the same target in the same frame waits for the GPU
    // to finish an earlier one. vitaGL's default is 1 for framebuffer objects.
    // The renderer returns to the EFB after every EFB copy (43 copies in an
    // open-play frame), so its framebuffer objects get the maximum; the
    // display itself is drawn to once per frame.
    int fboScenes = 8;
    if (const char* e = std::getenv("GXV_VITA_FBO_SCENES")) fboScenes = std::atoi(e);
    if (fboScenes < 1) fboScenes = 1;
    if (fboScenes > 8) fboScenes = 8;
    vglSetupRenderTargetScenesNum(1, static_cast<uint8_t>(fboScenes));
    std::fprintf(stderr, "[vgx-vita] render targets: %d scene(s) per frame for framebuffer objects (GXV_VITA_FBO_SCENES in env.txt changes it)\n", fboScenes);

    // The shader patcher holds every linked program and every patched variant of one (blend
    // state, vertex layout). vitaGL's default is 1 MiB for each of its three pools; the ACGC
    // port needed 4 / 2 / 2 MiB for some 240 small programs, and ours are 78 programs with
    // 155 variants of much larger shaders. When a pool is exhausted vitaGL logs "shader
    // patching failed" and draws with a stale program.
    unsigned patcherMib = 4;
    if (const char* e = std::getenv("GXV_VITA_PATCHER_MIB")) patcherMib = static_cast<unsigned>(std::atoi(e));
    if (patcherMib < 1) patcherMib = 1;
    if (patcherMib > 16) patcherMib = 16;
    vglSetupShaderPatcher(patcherMib * 1024u * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u, (patcherMib + 1) / 2 * 1024u * 1024u);
    vglSetupGarbageCollector(kGcPriority, VitaThreads::CoreMask(VitaThreads::kCollector));
    std::fprintf(stderr, "[vgx-vita] shader patcher pools %u / %u / %u MiB (GXV_VITA_PATCHER_MIB in env.txt changes the first), collector thread core mask 0x%X\n",
                 patcherMib, (patcherMib + 1) / 2, (patcherMib + 1) / 2, static_cast<unsigned>(VitaThreads::CoreMask(VitaThreads::kCollector)));

    sceIoMkdir(kShaderCacheDir, 0777);
    sceIoMkdir(ShaderCacheFolder(), 0777);
    vglSetShaderCachePath(SeedShaderCache(ShaderCacheFolder()));
    // The game paces itself to the display rate; a second wait for vertical
    // blank inside the swap would make the two beat against each other.
    vglWaitVblankStart(GL_FALSE);
    vglSetDisplayCallback(DisplayCallback);

    VitaPlatform::BootStage("vitaGL start-up (takes over the display; first run creates the shader cache folders)");
    VitaConsole::SetDisplayOwner(false);
    // The return value is NOT "initialised": vitaGL returns GL_TRUE only when the display could
    // not take the requested size and it fell back to a smaller one (source/vgl.c: `return
    // res_fallback`), i.e. GL_FALSE is the normal result. (Until the first run in Vita3K, 5 Oct,
    // this file treated GL_FALSE as failure: every start ended here with "vglInitWithCustomSizes
    // failed", on a console as in the emulator.) vitaGL reports nothing else from its start-up;
    // what can go wrong in it shows in the lines below (pools, GL strings) and in the shader
    // compiler self-test.
    const GLboolean fellBack = vglInitWithCustomSizes(0, kScreenW, kScreenH, static_cast<int>(ram), static_cast<int>(cdram),
                                                      static_cast<int>(phycont), 0, SCE_GXM_MULTISAMPLE_NONE);
    if (fellBack)
        std::fprintf(stderr, "[vgx-vita] vitaGL could not set %dx%d and fell back to the display's largest mode; the picture's placement will be off\n",
                     kScreenW, kScreenH);
    if (vglMemTotal(VGL_MEM_VRAM) == 0 && vglMemTotal(VGL_MEM_RAM) == 0) {
        std::snprintf(g_startError, sizeof(g_startError), "vitaGL started without any memory pool (see the [vitaGL] lines in log.txt).");
        return false;
    }
    LogMemory("after vitaGL");
    {
        // A pool that vitaGL could not allocate leaves the budget untouched; say so instead of
        // crashing at its first use (see the note at the pool sizes above).
        SceKernelFreeMemorySizeInfo after;
        std::memset(&after, 0, sizeof(after));
        after.size = sizeof(after);
        sceKernelGetFreeMemorySize(&after);
        // (An emulator reports more video memory than any console has and may not count
        // allocations at all: no verdict there.)
        const bool realBudget = free.size_cdram > 0 && free.size_cdram <= 160u * 1024u * 1024u;
        const int64_t taken = static_cast<int64_t>(free.size_cdram) - static_cast<int64_t>(after.size_cdram);
        if (realBudget && cdram > 0 && taken < cdram / 2) {
            std::snprintf(g_startError, sizeof(g_startError),
                          "vitaGL could not allocate its video memory pool (%u KiB asked, %u KiB of video memory still free).\n"
                          "Set GXV_VITA_CDRAM_RESERVE_MIB in env.txt to a larger value than %d.",
                          static_cast<unsigned>(cdram / 1024), static_cast<unsigned>(after.size_cdram / 1024),
                          static_cast<int>(cdramReserveMib));
            return false;
        }
    }
    LogPools("after start-up");
    std::fprintf(stderr, "[vgx-vita] GL: %s / %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)), reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    // Every buffer of the swap chain starts black (the side bars of the 4:3
    // picture are never drawn again), and the first swap is a sign of life.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // The display is cleared and swapped three times; after the FIRST of these, one clear with
    // the scissor on (GXV_VITA_PRIME=0 leaves it out). Both the scissored clear and its place
    // are there for Vita3K (v0.2.1-4120); a console needs neither and loses nothing by them.
    //
    // (1) Why a scissored clear at all. vitaGL implements the scissor with sceGxm's mask bit
    // and writes that mask at every scene start. The emulator's OpenGL renderer takes the
    // value such a mask draw writes from the last stencil function it was SENT while the mask
    // program was current (renderer/src/state_set.cpp), starts with 0
    // (renderer/include/renderer/types.h, writing_mask) and is not sent a function that
    // repeats the previous one (modules/SceGxm/SceGxm.cpp sceGxmSetFrontStencilFunc). With
    // the scissor off vitaGL's mask draw repeats "always", so until the first scissored draw
    // of a session every mask is written as 0 and every later draw is discarded: the four
    // boot screens were black (v3k/NOTES.md, session 3, V2). A scissored draw sends "never"
    // then "always", after which the emulator's value stays 1.
    //
    // (2) Why NOT as the very first draw of the session. vitaGL draws its mask quads with the
    // same vertex program as its clear quad. The emulator translates a guest shader when it
    // is first used and keeps the translation on disk under the shader's hash; when that
    // first use is a mask draw, it writes the mask-update body into the VERTEX shader as well
    // ("out_color = vec4(screen_width)", no gl_Position: build/v3k/emucache-poisoned against
    // emucache-good). From then on no clear and no mask draw of any session reaches a pixel:
    // the OpenGL renderer shows nothing at all (runs gl17, gl19, gl20a-d, gl21a, gl22a), and
    // on the Vulkan renderer nothing is ever cleared (menus with piled-up text, the match on
    // black: every Vulkan run from gl13V to gl19V). So the first draw is a plain clear.
    static const bool prime = [] { const char* e = std::getenv("GXV_VITA_PRIME"); return !(e && *e && std::atoi(e) == 0); }();
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
    std::fprintf(stderr, "[vgx-vita] display cleared and swapped three times (%s)\n", prime ? "with one scissored clear after the first" : "GXV_VITA_PRIME=0: no scissored clear");
    if (const char* e = std::getenv("GXV_VITA_MASK_GUARD"); e && std::atoi(e) != 0) {
        vgxvita_maskGuardOn = 1;
        std::fprintf(stderr, "[vgx-vita] GXV_VITA_MASK_GUARD=1: every draw and clear is preceded by a mask update under depth function NEVER "
                             "(for Vita3K's Vulkan renderer, which draws mask updates into the picture; never needed on a console)\n");
    }

    char error[400] = "";
    if (!ShaderCompilerSelfTest(error, sizeof(error))) {
        std::snprintf(g_startError, sizeof(g_startError), "Run-time shader compilation does not work:\n%s", error);
        return false;
    }
    // Static geometry goes to vitaGL's MAIN-memory pool (the hint GL_DYNAMIC_DRAW; see
    // g_staticUsage in vgx_backend_gl.cpp): 18.5 MiB less video memory in a match.
    // GXV_STATIC_USAGE=static in env.txt puts it back into video memory.
    setenv("GXV_STATIC_USAGE", "dynamic", 0);
    {
        const char* usage = std::getenv("GXV_STATIC_USAGE");
        std::fprintf(stderr, "[vgx-vita] static vertex buffers: %s\n", usage && std::strcmp(usage, "dynamic") == 0 ? "main-memory pool first (GL_DYNAMIC_DRAW hint)"
                                                                                                                     : "video memory first (GXV_STATIC_USAGE)");
    }
    int renderW = 0, renderH = 0;   // 0 = the game's own frame-buffer size
    RenderSize(renderW, renderH);
    if (!vgx::backend::Init(renderW, renderH)) {
        std::snprintf(g_startError, sizeof(g_startError), "the vgx GL back end failed to initialise");
        return false;
    }
#ifndef GXV_COMMIT
#define GXV_COMMIT "unknown"
#endif
    vgx::backend::SetPresentHook(PresentDisplayCopy);
    vgx::backend::SetProgressDisplay(0, kScreenW, kScreenH, ShowProgressBar);
    PinLibraryThreads(true);
    if (VitaMem::Checkpoint) VitaMem::Checkpoint("after vitaGL start-up and the back end's own allocations");
    std::fprintf(stderr, "[vgx-vita] back end ready; renderer sources: %s\n", GXV_COMMIT);
    return true;
}

// The back end's present hook: a display copy, on the render thread, with the
// EFB framebuffer object `efb` bound and the finished picture in it. (x, y, w,
// h) is the copy rectangle in framebuffer pixels; the EFB is stored top row
// first (its projection flips Y), so the picture's top row is the
// framebuffer's y. The back end binds the EFB again and re-applies its render
// state after this returns.
void PresentDisplayCopy(unsigned efb, int x, int y, int w, int h) {
    ++g_presented;
    const uint64_t now = NowUs();
    const vgx::backend::Stats stats = vgx::backend::GetStats();
    if (g_presented == 1)
        std::fprintf(stderr, "[vgx-vita] first present: display copy %d,%d %dx%d from framebuffer object %u\n", x, y, w, h, efb);
    // A long gap between two presents is almost always shader compilation: say so.
    if (g_lastPresentUs != 0 && now - g_lastPresentUs >= 100000u &&
        (stats.vertexShaders != g_lastStats.vertexShaders || stats.fragmentShaders != g_lastStats.fragmentShaders ||
         stats.programs != g_lastStats.programs))
        std::fprintf(stderr, "[vgx-vita] present %llu came %u ms after the previous one: +%u vertex shaders, +%u fragment shaders, +%u programs, +%u textures\n",
                     static_cast<unsigned long long>(g_presented), static_cast<unsigned>((now - g_lastPresentUs) / 1000u),
                     stats.vertexShaders - g_lastStats.vertexShaders, stats.fragmentShaders - g_lastStats.fragmentShaders,
                     stats.programs - g_lastStats.programs, stats.textures - g_lastStats.textures);
    if (g_presented == 1 || g_presented % 600 == 0) {
        std::fprintf(stderr, "[vgx-vita] present %llu: %u draw calls in the last buffer, %u vertex + %u fragment shaders and %u programs so far, %u textures, %u KiB of texels\n",
                     static_cast<unsigned long long>(g_presented), stats.drawCallsThisFrame, stats.vertexShaders, stats.fragmentShaders,
                     stats.programs, stats.textures, static_cast<unsigned>(stats.textureBytes / 1024u));
        // (A start that found its shaders in vitaGL's cache shows a few hundred milliseconds
        // here; one that compiled them, 0.3 to 1 s per shader.)
        std::fprintf(stderr, "[vgx-vita] shader work so far: %u compiles, %u links, %u ms in all, progress bar shown %u times\n", stats.shaderCompiles,
                     stats.programLinks, static_cast<unsigned>(stats.shaderWorkMs), stats.progressFrames);
        {
            unsigned made = 0, ms = 0;
            vgxvita_cacheFolderStats(&made, &ms);
            if (made != 0) std::fprintf(stderr, "[vgx-vita] shader cache: %u bucket folders made as shaders needed them, %u ms in all\n", made, ms);
        }
        LogPools("now");
    }
    if (g_presented == 5 || g_presented == 30 || g_presented == 120 || g_presented % 600 == 0) PinLibraryThreads(false);
    g_lastPresentUs = now;
    g_lastStats = stats;

    // (With the text console selected the frame is still drawn and swapped,
    // so vitaGL's per-frame bookkeeping goes on; DisplayCallback then puts the
    // console's picture over it.)
    // A fatal-error screen has taken the display back: leave it alone.
    if (VitaConsole::OwnsDisplay()) return;
    if (efb == 0 || w <= 0 || h <= 0) return;
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
    const int dx0 = display.x, dx1 = display.x + display.width;
    // Source rows are given bottom-up so the picture comes out upright.
    int sy0 = y + h, sy1 = y;
    if (g_flipY.load(std::memory_order_relaxed)) std::swap(sy0, sy1);
    if (g_scissorOn) { glDisable(GL_SCISSOR_TEST); ParkScissor(); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, efb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (vgxvita_maskGuardOn) vgxvita_maskGuard();   // (the display's scene starts with a mask update too)
    glBlitFramebuffer(x, sy0, x + w, sy1, dx0, 0, dx1, kScreenH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    vglSwapBuffers(GL_FALSE);
    // Back to what the back end had: it clears the EFB next.
    glBindFramebuffer(GL_FRAMEBUFFER, efb);
    if (g_scissorOn) { UnparkScissor(); glEnable(GL_SCISSOR_TEST); }
}

// GXV_BACKEND_THREAD=1 (env.txt): the renderer's player gets a thread of its own and the
// render thread only executes finished command buffers, i.e. game / decode / GL on three
// threads, which is how both reference ports that run on consoles are laid out (ACGC: main,
// display-list worker on core 1, a third on core 2; melee-Vita: game core 0, submit worker
// core 2, render thread core 1). That thread is created inside the renderer with no
// affinity, so it is pinned here: core 2, next to the audio thread, never the game's core.
// The default stays two threads until a console run shows the render thread is the limit.
// (Cores: vita/src/vita_threads.cpp, GXV_VITA_CORE_PLAYER in env.txt.)
void PinPlayerThread() {
    std::fprintf(stderr, "[vgx-vita] record player on a thread of its own (GXV_BACKEND_THREAD)\n");
    VitaThreads::PinCurrentThread(VitaThreads::kPlayer);
}

void* RenderThread(void*) {
    // The core plan is the platform layer's (vita/src/vita_threads.cpp): core 1 while the game
    // thread is the only heavy thread beside this one, core 2 (with audio) when the two-core
    // split's worker has core 1 (MSC_SPLIT=thread); GXV_VITA_CORE_RENDER in env.txt overrides.
    VitaThreads::PinCurrentThread(VitaThreads::kRender);
    const bool ok = StartGl();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_started = true;
        g_startOk = ok;
    }
    g_cv.notify_all();
    if (!ok) return nullptr;
    // Streaming mode: this thread is the player (front end and back end).
    if (vgx::rec::g_mode == vgx::rec::MODE_THREAD) vgx::rec::PlayerThreadMain();
    for (;;) {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait(lk, [] { return g_jobBuf != nullptr; });
        const vgx::CmdBuf* buf = g_jobBuf;
        lk.unlock();
        vgx::backend::Execute(*buf);
        lk.lock();
        g_jobBuf = nullptr;
        lk.unlock();
        g_cv.notify_all();
    }
    return nullptr;
}

bool EnsureThread() {
    if (!g_threadCreated) {
        g_threadCreated = true;
        // The renderer picks its mode from the environment.
        if (Exists(GXV_VITA_DATA_DEVICE "/vgx_play_frame")) setenv("GXV_PLAY", "frame", 1);
        else if (Exists(GXV_VITA_DATA_DEVICE "/vgx_play_immediate")) setenv("GXV_PLAY", "immediate", 1);
        // Settings a console needs, unless env.txt chose otherwise (the
        // renderer reads those two variables itself).
        sceIoMkdir(kShaderCacheDir, 0777);
        if (!std::getenv("GXV_SHADERLIST")) {
#if GXV_RECIPE_SEED_BATCH
            if (vgx::recipe::Eligible()) vgx::SetShaderRecipeSeedFile("app0:shadercache/vgx_recipe_seed.bin");
            else
#endif
            SeedShaderList();
            vgx::SetShaderListFile(kShaderListFile);
        }
        if (!std::getenv("GXV_TEXBUDGET")) vgx::SetTextureBudget(kTextureBudget);
        std::fprintf(stderr, "[vgx-vita] shader warm-up list: %s (%s); texture budget: %s\n",
                     std::getenv("GXV_SHADERLIST") ? std::getenv("GXV_SHADERLIST") : kShaderListFile,
                     Exists(std::getenv("GXV_SHADERLIST") ? std::getenv("GXV_SHADERLIST") : kShaderListFile)
                         ? "found: all known shaders are compiled during the first frame" : "not there yet: shaders are compiled as the game needs them",
                     std::getenv("GXV_TEXBUDGET") ? "GXV_TEXBUDGET (MiB) from env.txt" : "32 MiB");
        vgx::rec::InitMode();
        vgx::rec::SetPlayerThreadInit(PinPlayerThread);
        static const char* const kModes[3] = {"immediate (game thread, after every call)", "frame (game thread, at present)",
                                              "thread (render thread, streaming)"};
        std::fprintf(stderr, "[vgx-vita] record player mode: %s\n", kModes[vgx::rec::g_mode % 3]);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kRenderStack);
        pthread_t thread;
        const int res = pthread_create(&thread, &attr, RenderThread, nullptr);
        pthread_attr_destroy(&attr);
        if (res != 0) {
            std::snprintf(g_startError, sizeof(g_startError), "could not create the render thread (error %d)", res);
            return false;
        }
        pthread_detach(thread);
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait(lk, [] { return g_started; });
    }
    return g_startOk;
}

// SELECT+START+D-pad: see the header comment.
void DebugControls() {
    static uint32_t last = 0;
    VitaInput::Raw raw;
    if (!VitaInput::Read(raw)) return;
    const uint32_t chord = VitaInput::kSelect | VitaInput::kStart;
    const uint32_t now = (raw.buttons & chord) == chord ? (raw.buttons & (VitaInput::kUp | VitaInput::kDown | VitaInput::kRight)) : 0;
    const uint32_t pressed = now & ~last;
    last = now;
    if (pressed & VitaInput::kDown) {
        const bool console = !g_showConsole.load();
        g_showConsole.store(console);
        std::fprintf(stderr, "[vgx-vita] display: %s\n", console ? "text console over the picture" : "picture");
    }
    if (pressed & VitaInput::kUp) {
        const bool flip = !g_flipY.load();
        g_flipY.store(flip);
        std::fprintf(stderr, "[vgx-vita] present: vertical flip %s\n", flip ? "on" : "off");
    }
    if (pressed & VitaInput::kRight) {
        const bool stretch = !g_stretch.load();
        g_stretch.store(stretch);
        std::fprintf(stderr, "[vgx-vita] present: %s\n", stretch ? "stretched to 960x544" : "4:3 with side bars");
    }
}

} // namespace

// ---- GL calls routed here from the back end (include/GLES2/gl2.h)
// GXV_VITA_MASK_GUARD=1 (default 0): FOR VITA3K'S VULKAN RENDERER ONLY, never needed on a
// console. vitaGL implements the scissor with sceGxm's mask bit and redraws the mask (one
// quad over the target, or two when the scissor is on) inside the first draw or clear after
// every scene start and every scissor change. Vita3K's Vulkan renderer has no mask ("Mask
// not implemented in the vulkan renderer!") and draws those quads into the COLOUR surface,
// white or transparent black, at depth 1.0 under whatever depth test is current: with a
// clean emulator shader cache the menus stand on white and the 3D backgrounds are painted
// over (run gl24V; v3k/NOTES.md, session 3). With the guard on, every draw and clear of the
// back end is preceded by a clear of nothing under depth function NEVER: vitaGL does its
// mask update inside that call (scene_reset() is the first statement of its glClear), the
// emulator's quads fail the depth test, and the draw that follows finds the mask clean.
// The clear of nothing itself is a quad with the fragment program disabled and depth
// writes off. The scissor still works there through the region clip vitaGL sets beside
// the mask. Run gl26V with it: boot screens, menus in their dark room, holograms, stadium
// with its crowd, kick-off and open play as in the PC pictures.
extern "C" { unsigned char vgxvita_maskGuardOn = 0; }
namespace {
GLenum g_depthFunc = GL_LESS;   // GL's initial values
bool g_depthTestOn = false;
} // namespace
extern "C" void vgxvita_glDepthFunc(GLenum func) {
    g_depthFunc = func;
    glDepthFunc(func);
}
extern "C" void vgxvita_maskGuard(void) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_NEVER);
    glClear(0);
    glDepthFunc(g_depthFunc);
    if (!g_depthTestOn) glDisable(GL_DEPTH_TEST);
}

// The scissor rectangle while the scissor is OFF. GL ignores it then; vitaGL does not quite:
// update_scissor_test() (source/tests.c) clips the rectangle to the bound target whether the
// test is on or not, and a rectangle that starts right of a narrower target (an atlas cell at
// x = 594, then a 512x256 pass target) leaves a NEGATIVE width in vitaGL's `region`, which
// scene_reset() (source/gxm.c) hands to sceGxmSetRegionClip at the start of the next scene
// with the scissor on, before the update that replaces it. No draw happens in between, so on
// a console that is at worst an odd clip for no pixels. Vita3K's OpenGL renderer turns it
// into glScissor with a negative size (GL_INVALID_VALUE, seen with MESA_DEBUG, 63 times on
// the way to kick-off), and the emulator blames the left-over error on whatever host object
// it creates next: "Failed to initialise color surface texture!", and once a failed
// sceGxmCreateRenderTarget followed by a crash at the post-match scene (run gl14).
// So the rectangle the back end set is kept here, vitaGL's copy is parked at a rectangle
// inside any target while the scissor is off, and put back when it is switched on. Free:
// glEnable/glDisable(GL_SCISSOR_TEST) mark vitaGL's scissor stale anyway.
namespace {
GLint g_scRect[4] = {0, 0, 1, 1};
void ParkScissor() { glScissor(0, 0, 1, 1); }
void UnparkScissor() { glScissor(g_scRect[0], g_scRect[1], g_scRect[2], g_scRect[3]); }
} // namespace
extern "C" void vgxvita_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_scRect[0] = x; g_scRect[1] = y; g_scRect[2] = width; g_scRect[3] = height;
    if (g_scissorOn) glScissor(x, y, width, height);   // (off: it is given when the scissor is switched on)
}

extern "C" void vgxvita_glEnable(GLenum cap) {
    if (cap == GL_DITHER) return;
    if (cap == GL_SCISSOR_TEST) { g_scissorOn = true; UnparkScissor(); }
    if (cap == GL_DEPTH_TEST) g_depthTestOn = true;
    glEnable(cap);
}
extern "C" void vgxvita_glDisable(GLenum cap) {
    if (cap == GL_DITHER) return;   // vitaGL has no dithering to switch off
    if (cap == GL_SCISSOR_TEST) g_scissorOn = false;
    if (cap == GL_DEPTH_TEST) g_depthTestOn = false;
    glDisable(cap);
    if (cap == GL_SCISSOR_TEST) ParkScissor();
}

// The constant a disabled attribute array supplies. In GL the value set with
// glVertexAttrib4f stays until it is set again, and the back end relies on
// that: it sets a location's constant when its array is switched off, not
// before every draw. vitaGL (source/custom_shaders.c, reserve_attrib_pool)
// stores each value in the next slot of a ring of 16,384 four-float slots and
// keeps only a pointer to it per location, so once the ring has gone round, a
// location whose constant was set long ago reads whatever a later call wrote
// into its slot (a texture-matrix index of 60 turning into 0 or 1, say),
// silently and only after some seconds of play. The values are therefore
// shadowed here and all of them are issued again every 2,048 calls, long
// before a slot can come up for reuse.
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

// Texture data that goes through the GPU's transfer unit. vitaGL converts a compressed level
// (and generates mip levels) with sceGxmTransferCopy / sceGxmTransferDownscale and returns
// without waiting for the transfer (utils/gpu_utils.c gpu_alloc_compressed_texture,
// gpu_alloc_mipmaps); it only waits, with sceGxmTransferFinish, where it reads a transfer's
// result itself (framebuffers.c, glReadPixels). But the upload of the NEXT level of the same
// texture reallocates the texture's memory (vgl_realloc in gpu_alloc_compressed_texture):
// if the block moves while the previous level's transfer is still queued, the transfer
// writes into the freed block and the copy made by the reallocation holds whatever was
// there before. In Vita3K, which runs transfers later on its render thread (SceGxm.cpp
// sceGxmTransferCopy -> renderer::transfer_copy, sent without waiting), that happened in
// every session at the title screen, the first scene with mip-mapped DXT1 textures: three
// runs ended in the emulator's allocator (a null link in the freed block) or with the game
// thread reading garbage, the run with GXV_DXT1=0 did not (5 Oct, v3k/NOTES.md). On a
// console the window is a transfer of a few kilobytes against a memcpy, so it probably never
// shows, but nothing orders the two. Waiting for the transfer unit after each such call
// closes it on both; the wait covers our own pending transfers only, not rendering.
// GXV_VITA_UPLOAD_SYNC=0 in env.txt turns the wait off (to measure what it costs).
static bool UploadSync() {
    static const bool on = [] {
        const char* e = std::getenv("GXV_VITA_UPLOAD_SYNC");
        const bool v = !(e && *e && std::atoi(e) == 0);
        std::fprintf(stderr, "[vgx-vita] texture transfers: %s (GXV_VITA_UPLOAD_SYNC)\n",
                     v ? "waited for after every compressed level and mip generation" : "NOT waited for");
        return v;
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

// vitaGL's own diagnostics (the library is built with LOG_ERRORS and with its
// print function renamed to this, see vita/Makefile.vitadeps). The first 400
// messages are logged in full; after that one in 500, so an error raised by
// every draw call cannot flood the log.
// ---- vitaGL's cache folders, made when they are needed
// vitaGL stores a compiled shader as <cache>/v2/{v,f}/<HH>/<hash>.gxp, hash = XXH3_64bits of
// the source text as handed to glShaderSource (all strings joined), <HH> = its top byte
// (source/custom_shaders.c, glCompileShader and glLinkProgram). It never creates <HH> when it
// saves: it expects all 512 of them from its start-up, and a save into a missing folder fails
// without a word (the shader is then compiled again at every start). Those 512 folders are
// what made the first start slow on a console (SeedShaderCache above), so they are made here,
// one at a time, for the shaders that exist. The file names this computes were checked
// against a cache written by vitaGL in the emulator: 141 of 141 generated shaders, the copy
// pair and the self-test pair (vgx/NOTES.md, round 6, "SEED").
extern "C" { extern char vgl_shader_cache_path[256] __attribute__((weak)); }
namespace {
uint8_t g_bucketSeen[2][32];           // [vertex, fragment][bucket / 8]
std::vector<uint8_t> g_shaderKind;     // by shader handle: 1 vertex, 2 fragment
unsigned g_bucketsMade = 0;
uint64_t g_bucketUs = 0;
void EnsureBucket(int kind, unsigned bucket) {
    uint8_t& bits = g_bucketSeen[kind][bucket >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (bucket & 7u));
    if (bits & bit) return;
    bits |= bit;
    if (&vgl_shader_cache_path[0] == nullptr || vgl_shader_cache_path[0] == 0) return;
    char path[320];
    if (std::snprintf(path, sizeof(path), "%s/%s/%02X", vgl_shader_cache_path, kind ? "f" : "v", bucket) >= static_cast<int>(sizeof(path))) return;
    const uint64_t t0 = NowUs();
    if (sceIoMkdir(path, 0777) >= 0) ++g_bucketsMade;   // (an error = it is there already, or the cache is read-only)
    g_bucketUs += NowUs() - t0;
}
} // namespace
extern "C" GLuint vgxvita_glCreateShader(GLenum type) {
    const GLuint shader = glCreateShader(type);
    if (shader < 65536u) {
        if (g_shaderKind.size() <= shader) g_shaderKind.resize(shader + 64u, 0);
        g_shaderKind[shader] = type == GL_VERTEX_SHADER ? 1 : 2;
    }
    return shader;
}
extern "C" void vgxvita_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) {
    if (shader < g_shaderKind.size() && g_shaderKind[shader] != 0 && count > 0 && string) {
        uint64_t hash;
        if (count == 1 && !(length && length[0] >= 0)) {
            hash = XXH3_64bits(string[0], std::strlen(string[0]));
        } else {
            static std::string joined;   // (render thread only)
            joined.clear();
            for (GLsizei i = 0; i < count; ++i) {
                if (length && length[i] >= 0) joined.append(string[i], static_cast<size_t>(length[i]));
                else joined.append(string[i]);
            }
            hash = XXH3_64bits(joined.data(), joined.size());
        }
        EnsureBucket(g_shaderKind[shader] == 1 ? 0 : 1, static_cast<unsigned>(hash >> 56));
    }
    glShaderSource(shader, count, string, length);
}
extern "C" void vgxvita_cacheFolderStats(unsigned* made, unsigned* ms) {
    if (made) *made = g_bucketsMade;
    if (ms) *ms = static_cast<unsigned>(g_bucketUs / 1000u);
}

extern "C" int msc_vgl_printf(const char* format, ...) {
    static uint32_t count = 0, compilerWarnings = 0;
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    // The shader compiler's warnings (vitaGL is built with HAVE_SHARK_LOG): the generated
    // shaders declare locals they do not always use, which gives four or five "local variable
    // ... is unreferenced" lines per compile, some five hundred in a first session. They
    // would use up the 400 full messages before anything that matters is said, so only the
    // first six are logged and the rest are neither logged nor counted. Errors ("E]") and
    // everything else go on as before.
    if (std::strstr(text, "Shader Compiler: W]")) {
        if (++compilerWarnings > 6) return 0;
        if (compilerWarnings == 6) std::fprintf(stderr, "[vitaGL] (further shader compiler warnings are not logged)\n");
    }
    ++count;
    if (count > 400 && count % 500 != 0) return 0;
    size_t length = std::strlen(text);
    while (length != 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) text[--length] = '\0';
    if (count > 400) std::fprintf(stderr, "[vitaGL] (message %u) %s\n", count, text);
    else std::fprintf(stderr, "[vitaGL] %s\n", text);
    return static_cast<int>(length);
}

namespace vgx {

bool LogEnabled() {
    static const bool on = Exists(kVerboseMarker);
    return on;
}

// Used in the "frame" and "immediate" modes only: the game thread decoded
// the records itself and hands the command buffer over; returns when it has
// been executed. (In "thread" mode the player executes buffers directly.)
void SubmitToBackend(CmdBuf& buf) {
    if (!EnsureThread()) return;
    std::unique_lock<std::mutex> lk(g_mtx);
    g_jobBuf = &buf;
    g_cv.notify_all();
    g_cv.wait(lk, [] { return g_jobBuf == nullptr; });
}

} // namespace vgx

using namespace vgx;

extern "C" {

AuroraInfo aurora_initialize(int, char**, const AuroraConfig* config) {
    AuroraInfo info{};
    info.backend = BACKEND_OPENGLES;
    info.userPath = config ? config->userPath : nullptr;
    info.cachePath = config ? config->cachePath : nullptr;
    info.window = nullptr;
    DecideWidescreen(config);
    info.windowSize = Widescreen() ? AuroraWindowSize{960, 544, 960, 544, 960, 544, 1.0f} : AuroraWindowSize{640, 480, 640, 480, 640, 480, 1.0f};
    g_stretch.store(Widescreen());
    const bool ok = EnsureThread();
    if (!ok) {
        // Give the display back to the text console: the runtime reports this
        // as a fatal error there.
        VitaConsole::SetDisplayOwner(true);
        std::fprintf(stderr, "[vgx-vita] graphics start-up failed: %s\n", g_startError);
    }
    info.initializationStatus = ok ? AURORA_INITIALIZATION_SUCCESS : AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE;
    info.initializationError = ok ? nullptr : g_startError;
    rec::BackendFlags(false, false);   // no frame capture, no skipped draws
    return info;
}
void aurora_shutdown() {}
const AuroraEvent* aurora_update() {
    static AuroraEvent none{};
    none.type = AURORA_NONE;
    return &none;
}
bool aurora_begin_frame() { return true; }
void aurora_end_frame() {
    // Two-core split (workstream S): for a forked frame this runs on the split's worker
    // context, and the runtime has already done the game thread's part at the fork. What
    // belongs to the game thread is skipped there (vita/src/plat_split.h): the debug controls
    // read the pad (statics, and the game thread reads it at the same time), the frame
    // accounting drives the console, the harness's script and its exit. Presenting is right
    // on either thread: the recorder has one producer at a time, handed over at the fork and
    // back at the join under the runtime's mutex.
    const bool forked = PlatSplit::OnWorker();
    if (!forked) DebugControls();
    // Returns once the player has read everything this frame refers to in
    // guest memory; the render thread may still be drawing it.
    rec::Present(false);
    if (!forked) PlatFrameStats::OnFramePresented();
}
AuroraDisplayMode aurora_get_display_mode() { return AURORA_DISPLAY_MODE_WINDOWED; }
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback) {}
void aurora_wait_for_frame_worker() {}
void aurora_set_present_schedule(uint64_t, uint64_t) {}
void aurora_report_producer_paced(bool) {}
void aurora_set_preferred_refresh_rate(float) {}
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation, AuroraGuestWriteNotifyCallback) {
    SetGuestWriteGenerationHook(generation);
    rec::SetGuestMemoryProbe(generation);
}
void AuroraSetViewportPolicy(AuroraViewportPolicy) {}
int MscVitaTouchViewport(float* x, float* y, float* width, float* height, int* flipY) {
    if (!x || !y || !width || !height || !flipY ||
        g_showConsole.load(std::memory_order_relaxed) || VitaConsole::OwnsDisplay()) return 0;
    const auto display = vgxvita::GetDisplayRect(g_stretch.load(std::memory_order_relaxed));
    *x = static_cast<float>(display.x);
    *y = static_cast<float>(display.y);
    *width = static_cast<float>(display.width);
    *height = static_cast<float>(display.height);
    *flipY = g_flipY.load(std::memory_order_relaxed) ? 1 : 0;
    return 1;
}
void AuroraGetSurfaceSize(u32* width, u32* height) {
    // The screen's own shape in full-screen mode (the runtime fits its 3D projections to it),
    // 4:3 otherwise: see DecideWidescreen().
    if (width) *width = Widescreen() ? 960 : 640;
    if (height) *height = Widescreen() ? 544 : 480;
}

// One controller on port 0: the Vita's own controls (vita/src/vita_input.cpp).
u32 PADRead(PADStatus* status) {
    if (!status) return 0;
    for (int i = 0; i < PAD_CHANMAX; ++i) {
        std::memset(&status[i], 0, sizeof(PADStatus));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    PlatInput::Fill(status[0]);
    status[0].err = PAD_ERR_NONE;
    return 0x80000000u;
}
s32 PADGetIndexForPort(u32) { return -1; }

} // extern "C"

#endif // __vita__
