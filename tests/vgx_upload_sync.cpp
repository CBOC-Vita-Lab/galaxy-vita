// tex11: texture uploads' transfer-unit waits (GXV_VITA_UPLOAD_SYNC, vgx_host_galaxy_vita.cpp) and vitaGL patch 0015's chain
// reserve (GXV_TEX_RESERVE, vglReserveTextureLevels), on the back end built as for the console (__VGX_VITA__, GL stub).
// A model of vitaGL stands in for the texture calls: per texture the levels its storage holds, and whether a transfer that
// writes it is pending. A call that would make vitaGL move or free storage with a pending transfer (realloc's CPU copy, a free)
// is a hazard; so is a frame that ends with a transfer pending. Mode 1 (wait after every call, the default), mode 2 with and
// without the reserve: the same GL calls in the same order with the same data, no hazard, and the number of waits.
//   tools/test_vgx_upload_sync.sh
#include <GLES2/gl2.h>
#include <map>
#include <string>
#include <vector>
namespace model {
struct T { int levels = 0; bool pending = false; bool live = false; };
std::map<unsigned, T> tex;
unsigned bound = 0, hazards = 0, waits = 0, reserveW = 0, reserveH = 0;
int mode = 1, reserveLevels = 0;
bool open = false;
std::vector<std::string> calls;
void Issue(T& t) { t.pending = true; if (mode == 1) { for (auto& kv : tex) kv.second.pending = false; ++waits; } else open = true; }
void Store(int level, unsigned w, unsigned h, int fullLevels, const void* data, size_t size) {
    T& t = tex[bound];
    char line[96];
    std::snprintf(line, sizeof line, "%u L%d %ux%u %zu %02x", bound, level, w, h, size, data && size ? *static_cast<const unsigned char*>(data) : 0);
    calls.push_back(line);
    if (level == 0) {
        if (t.live && t.pending) ++hazards; // (vitaGL frees the old storage)
        const bool hint = reserveLevels && reserveW == w && reserveH == h;
        t.levels = hint ? (reserveLevels < 0 ? fullLevels : reserveLevels) : 1;
        t.live = true;
        reserveLevels = 0;
    } else if (level + 1 > t.levels) {
        if (t.pending) ++hazards; // (realloc: a CPU copy of what the transfer unit may still be writing)
        t.levels = level + 1;
    }
}
}
static void M_glCompressedTexImage2D(GLenum, GLint level, GLenum, GLsizei w, GLsizei h, GLint, GLsizei size, const GLvoid* data) {
    model::Store(level, w, h, 0, data, size);
    model::Issue(model::tex[model::bound]);
}
static void M_glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum, GLenum, const GLvoid* data) {
    int full = 1; for (unsigned a = w, b = h; a > 1 && b > 1; a /= 2, b /= 2) ++full;
    model::Store(level, w, h, full, data, data ? 4 : 0);
}
static void M_glGenerateMipmap(GLenum) {
    model::T& t = model::tex[model::bound];
    model::calls.push_back(std::to_string(model::bound) + " genmips");
    if (t.levels < 2 && t.pending) ++model::hazards;
    t.levels = 99;
    model::Issue(t);
}
static void M_glDeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) { model::T& t = model::tex[ids[i]]; if (t.live && t.pending) ++model::hazards; t = {}; }
}
static void M_glBindTexture(GLenum target, GLuint name) { model::bound = name; glBindTexture(target, name); }
#define glCompressedTexImage2D M_glCompressedTexImage2D
#define glTexImage2D M_glTexImage2D
#define glGenerateMipmap M_glGenerateMipmap
#define glDeleteTextures M_glDeleteTextures
#define glBindTexture M_glBindTexture
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
// The host's side (vgx_host_galaxy_vita.cpp): mode from the model, a fence clears every pending transfer.
int vgxvita_uploadSyncMode(void) { return model::mode; }
void vgxvita_xferFence(void) {
    if (!model::open || model::mode != 2) return;
    for (auto& kv : model::tex) kv.second.pending = false;
    model::open = false;
    ++model::waits;
}
void vgxvita_xferStats(uint64_t* w, uint64_t* us) { *w = model::waits; *us = 0; }
}
static bool g_reserveLib = false;
extern "C" void vglReserveTextureLevelsImpl(GLsizei w, GLsizei h, GLint levels) { model::reserveW = w; model::reserveH = h; model::reserveLevels = levels; }
extern "C" void vglReserveTextureLevels(GLsizei w, GLsizei h, GLint levels) { if (g_reserveLib) vglReserveTextureLevelsImpl(w, h, levels); }
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

static TexUpload Dxt(uint32_t id, unsigned w, unsigned h, unsigned levels) {
    TexUpload up; up.texId = id; up.width = w; up.height = h; up.format = TF_DXT1;
    for (unsigned l = 0; l < levels; ++l) { up.levels.emplace_back(((w + 3) / 4) * ((h + 3) / 4) * 8, uint8_t(id * 7 + l)); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    up.authored = levels; return up;
}
static TexUpload Rgba(uint32_t id, unsigned w, unsigned h, bool mips) {
    TexUpload up; up.texId = id; up.width = w; up.height = h; up.format = TF_RGBA8; up.genMips = mips;
    up.levels.emplace_back(size_t(w) * h * 4, uint8_t(id)); return up;
}
// A stage arrival's frame: uploads of many kinds, a re-upload, frees, a scene change in between; then a frame with the frees of
// textures uploaded the frame before and new uploads.
static CmdBuf Frame(uint64_t frame, uint32_t base, bool frees) {
    CmdBuf b; b.frame = frame;
    auto up = [&](TexUpload u) { b.uploads.push_back(std::move(u)); b.cmds.push_back({CMD_TEX_UPLOAD, uint32_t(b.uploads.size() - 1)}); };
    for (uint32_t i = 0; i < 6; ++i) up(Dxt(base + i, 64 << (i % 3), 64, 1 + i % 5));      // chains of 1..5 levels
    up(Dxt(base + 10, 256, 256, 2));
    up(Rgba(base + 20, 64, 32, true));
    up(Rgba(base + 21, 16, 16, false));
    up(Dxt(base + 0, 64, 64, 4));                                                             // the same texture again
    if (frees) for (uint32_t i = 1; i < 4; ++i) b.cmds.push_back({CMD_TEX_FREE, base - 100 + i});
    up(Dxt(base + 30, 128, 128, 5));
    b.copyDisp.push_back({{0, 0, 32, 32}, {}, false});
    b.cmds.push_back({CMD_COPY_DISP, 0});
    b.cmds.push_back({CMD_PRESENT, 0});
    return b;
}
int main(int argc, char** argv) {
    Require(argc == 3, "usage: vgx_upload_sync <mode 1|2> <reserve 0|1>");
    model::mode = std::atoi(argv[1]);
    g_reserveLib = std::atoi(argv[2]) != 0;
    setenv("GXV_TEX_RESERVE", g_reserveLib ? "1" : "0", 1); // (the symbol is always here: a library without the patch = the switch off)
    setenv("GXV_STAGEWARM", "0", 1);
    Require(Init(32, 32), "back end initialised against the GL stub");
    Execute(Frame(1, 100, false));
    for (auto& kv : model::tex) Require(!kv.second.pending, "no transfer outlives its frame");
    Execute(Frame(2, 200, true));
    for (auto& kv : model::tex) Require(!kv.second.pending, "no transfer outlives its frame");
    // The calls the driver saw (order, sizes, data) go to a file to compare between modes.
    FILE* f = std::fopen((std::string(argv[0]) + ".calls").c_str(), "w");
    for (const auto& c : model::calls) std::fprintf(f, "%s\n", c.c_str());
    std::fclose(f);
    std::printf("mode %d reserve %d: %zu texture calls, %u transfer waits, %u hazards\n", model::mode, g_reserveLib ? 1 : 0, model::calls.size(), model::waits, model::hazards);
    for (const auto& l : lines) if (l.rfind("tex-upload", 0) == 0) std::printf("  %s\n", l.c_str());
    Require(model::hazards == 0, "no storage moved or freed under a pending transfer");
    std::printf("PASS\n");
    return 0;
}
