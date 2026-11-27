// GXV_RT_DEFER (the 2.2 Comet Observatory crash; agents/obscrash/REPORT.txt): the back end built as for the console (__VGX_VITA__, GL
// stub) against a model of what vitaGL and the GPU do with a texture's storage:
//   - glTexImage2D gives a texture storage of its own (malloc here); glTexImage2D over it again, or glDeleteTextures, frees the
//     old storage AT ONCE when the texture was not sampled by a draw in its last 4 frames (never sampled included), else 3 frames
//     later (vitaGL's collector) - gpu_utils.h gpu_free_texture_data; a texture attached to a framebuffer object is freed when it
//     is detached or the object deleted (framebuffers.c), by the same rule;
//   - every clear or draw into a framebuffer object is a GPU write into the storage its colour texture has at that moment, carried
//     out one frame later (the console's GPU runs a frame or more behind the back end).
// A GPU write into storage already freed is the crash's mechanism (it overwrote a free chunk's header in vitaGL's CDRAM mspace).
// Built with -fsanitize=address and run with OBS_GPU_WRITE=<frame> the writes (of that frame's work on) are made for real (memset into the old pointer): ASan reports
// the heap-use-after-free; without it the model counts the hazard and skips the write.
// Sequences: (A) textures filled by a copy in every picture and never sampled, then a change of rung - GXV_RUNG_SKIPRS retires them
// unsampled (the log's "5 not resampled (rewritten every picture)" before the crash); (B) a copy texture whose copy size changes
// (CopyTexture's glTexImage2D, the crash's own call: 83x47 -> 42x24 in the dump); (C) pooled objects let go by the change.
//   tools/test_vgx_rt_defer.sh   (GXV_RT_DEFER=0 must show hazards - the 2.2 behaviour -, the default must show none)
// (fix32: the GL stub now declares a non-const vglGetTexDataPointer, as vitaGL.h does, and the backend calls the console's sceGxmFinish at a present:
//  the stub's declaration is renamed here and this file defines both, as tests/vgx_stage_warm.cpp does.)
#define vglGetTexDataPointer rt_defer_unused_tex_data_declaration
#include <GLES2/gl2.h>
#undef vglGetTexDataPointer
inline void sceGxmFinish(void*) {}
extern "C" void* vglGetTexDataPointer(unsigned);
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace model {
struct Tex {
    unsigned char* data = nullptr;
    size_t size = 0;
    int ref = 0;            // framebuffer objects it is attached to (vitaGL's ref_counter)
    bool dirty = false;     // deleted while attached
    bool slot = false;      // name in use
    long lastSampled = -1;  // frame of the last draw that had it bound (-1: never: OBJ_NOT_USED)
    unsigned long gen = 0;  // which allocation `data` is (an address the allocator hands out again is another one)
};
struct Job { unsigned name; unsigned char* data; size_t size; long frame; unsigned long gen; };
struct Late { unsigned char* data; long due; };
std::map<unsigned, Tex> tex;
std::map<unsigned, unsigned> fboColor;
std::vector<Job> jobs;
std::vector<Late> late;
std::map<unsigned char*, unsigned long> live; // storage alive now -> its allocation
unsigned long gens = 0;
unsigned unit = 0, unitTex[16] = {}, fbo = 0;
long frame = 0;
unsigned hazards = 0, immediateFrees = 0, deferredFrees = 0, gpuWrites = 0;
long realWriteFrom = 0; // OBS_GPU_WRITE=<frame>: from that frame's GPU work on, writes into freed storage are made for real
std::vector<std::string> notes;
void Free(unsigned name, Tex& t) {
    if (!t.data) return;
    if (t.lastSampled < 0 || frame - t.lastSampled > 4) { // vitaGL: vgl_free at once
        live.erase(t.data);
        std::free(t.data);
        ++immediateFrees;
    } else {
        late.push_back(Late{t.data, frame + 3}); // mark_as_dirty: the collector, 3 swaps later
        ++deferredFrees;
    }
    (void)name;
    t.data = nullptr;
    t.size = 0;
}
void Detach(unsigned name) {
    if (!name) return;
    Tex& t = tex[name];
    if (t.ref > 0) --t.ref;
    if (t.dirty && t.ref == 0) { Free(name, t); t = Tex{}; }
}
void Write() {
    if (!fbo || !fboColor.count(fbo) || !fboColor[fbo]) return;
    const unsigned name = fboColor[fbo];
    Tex& t = tex[name];
    if (!t.data) return;
    jobs.push_back(Job{name, t.data, t.size, frame, t.gen});
}
void Sample() {
    for (unsigned u = 0; u < 16; ++u)
        if (unitTex[u] && tex.count(unitTex[u]) && tex[unitTex[u]].data) tex[unitTex[u]].lastSampled = frame;
}
// The end of a frame: vglSwapBuffers. The GPU finishes the frame before the last; then the collector.
void Present() {
    ++frame;
    std::vector<Job> keep;
    for (const Job& j : jobs) {
        if (j.frame > frame - 2) { keep.push_back(j); continue; }
        ++gpuWrites;
        const auto it = live.find(j.data);
        if (it == live.end() || it->second != j.gen) { // (freed, or freed and handed out again: the crash's case)
            ++hazards;
            if (notes.size() < 8) notes.push_back("GPU write of frame " + std::to_string(j.frame) + " into texture " + std::to_string(j.name) +
                                                  "'s old storage, freed before frame " + std::to_string(frame) + " ended");
            if (realWriteFrom > 0 && j.frame >= realWriteFrom && it == live.end()) std::memset(j.data, 0xFF, j.size); // (ASan: heap-use-after-free)
        } else {
            std::memset(j.data, 0x00, j.size); // (opaque black in the dump; any value here)
        }
    }
    jobs.swap(keep);
    std::vector<Late> stay;
    for (const Late& l : late) {
        if (l.due > frame) { stay.push_back(l); continue; }
        live.erase(l.data);
        std::free(l.data);
    }
    late.swap(stay);
}
} // namespace model

static void M_glActiveTexture(GLenum u) { model::unit = (u - GL_TEXTURE0) & 15u; glActiveTexture(u); }
static void M_glBindTexture(GLenum target, GLuint name) { model::unitTex[model::unit] = name; glBindTexture(target, name); }
static void M_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const GLvoid* data) {
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
    const unsigned name = model::unitTex[model::unit];
    if (level != 0 || !name) return;
    model::Tex& t = model::tex[name];
    model::Free(name, t); // (gpu_alloc_texture: gpu_free_texture_data of the old storage first)
    t.size = static_cast<size_t>((w + 7) & ~7) * static_cast<size_t>(h < 1 ? 1 : h) * 4u + 16u;
    t.data = static_cast<unsigned char*>(std::malloc(t.size));
    std::memset(t.data, 0, t.size); // (vgl_memset: the padding columns stay 0 in the dump)
    t.gen = ++model::gens;
    model::live[t.data] = t.gen;
    t.slot = true;
    t.lastSampled = -1; // (OBJ_NOT_USED)
}
static void M_glDeleteTextures(GLsizei n, const GLuint* ids) {
    glDeleteTextures(n, ids);
    for (GLsizei i = 0; i < n; ++i) {
        if (!ids[i] || !model::tex.count(ids[i])) continue;
        model::Tex& t = model::tex[ids[i]];
        if (t.ref > 0) {
            // (vitaGL: attached to the bound object: freed now and the object loses it; else freed when detached)
            if (model::fbo && model::fboColor[model::fbo] == ids[i]) {
                model::Free(ids[i], t);
                model::fboColor[model::fbo] = 0;
                t = model::Tex{};
            } else {
                t.dirty = true;
            }
        } else {
            model::Free(ids[i], t);
            t = model::Tex{};
        }
        for (unsigned& u : model::unitTex) if (u == ids[i]) u = 0;
    }
}
static void M_glBindFramebuffer(GLenum target, GLuint f) { model::fbo = f; glBindFramebuffer(target, f); }
static void M_glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint name, GLint level) {
    glFramebufferTexture2D(target, att, textarget, name, level);
    if (att != GL_COLOR_ATTACHMENT0 || !model::fbo) return;
    const unsigned old = model::fboColor[model::fbo];
    model::fboColor[model::fbo] = name;
    if (name) ++model::tex[name].ref;
    model::Detach(old);
}
static void M_glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
    glDeleteFramebuffers(n, ids);
    for (GLsizei i = 0; i < n; ++i) {
        const unsigned old = model::fboColor.count(ids[i]) ? model::fboColor[ids[i]] : 0;
        model::fboColor.erase(ids[i]);
        if (model::fbo == ids[i]) model::fbo = 0;
        model::Detach(old);
    }
}
static void M_glClear(GLbitfield bits) { model::Write(); (glClear)(bits); }
static void M_glDrawArrays(GLenum m, GLint f, GLsizei c) { model::Sample(); model::Write(); (glDrawArrays)(m, f, c); }
static void M_glDrawElements(GLenum m, GLsizei c, GLenum t, const GLvoid* i) { model::Sample(); model::Write(); (glDrawElements)(m, c, t, i); }
#undef glClear
#undef glDrawArrays
#undef glDrawElements
#define glActiveTexture M_glActiveTexture
#define glBindTexture M_glBindTexture
#define glTexImage2D M_glTexImage2D
#define glDeleteTextures M_glDeleteTextures
#define glBindFramebuffer M_glBindFramebuffer
#define glFramebufferTexture2D M_glFramebufferTexture2D
#define glDeleteFramebuffers M_glDeleteFramebuffers
#define glClear(bits) M_glClear(bits)
#define glDrawArrays(m, f, c) M_glDrawArrays(m, f, c)
#define glDrawElements(m, c, t, i) M_glDrawElements(m, c, t, i)
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdarg>
#include <unistd.h>

namespace {
std::vector<std::string> lines;
int g_fail = 0;
void Check(bool good, const std::string& what) {
    std::printf("  [%s] %s\n", good ? "ok" : "FAIL", what.c_str());
    if (!good) ++g_fail;
}
}
extern "C" {
uint32_t port_scene_key(void) { return 1; }
uint32_t port_scene_loading_key(void) { return 0; }
int port_scene_loading_active(void) { return 0; }
int port_screen_quiet(void) { return 0; }
void port_log(const char* fmt, ...) { // (autoq::Log's way in a build with diagnostics)
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
}
void port_log_summary(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    lines.emplace_back(text);
}
int vgxvita_uploadSyncMode(void) { return 1; }
void vgxvita_xferFence(void) {}
void vgxvita_xferStats(uint64_t* w, uint64_t* us) { *w = 0; *us = 0; }
void vglReserveTextureLevels(GLsizei, GLsizei, GLint) {}
}
namespace vgx { bool LogEnabled() { return false; } }
#ifdef __VGX_VITA__
#include <psp2/kernel/threadmgr.h>
extern "C" {
unsigned vgx_stub_name_ = 0;
unsigned glCreateShader(unsigned) { return ++vgx_stub_name_; }
unsigned vglGetFreeProgramSlots(void) { return 1024; }
unsigned glCreateProgram(void) { return ++vgx_stub_name_; }
int glGetUniformLocation(unsigned, const char*) { return 1; }
unsigned glCheckFramebufferStatus(unsigned) { return 0x8CD5; }
unsigned glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned name) {
    return reinterpret_cast<const unsigned char*>(name == 0x1F03 ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : "stub");
}
size_t vglMemFree(int) { return 64u << 20; }
size_t vglMemTotal(int) { return 128u << 20; }
void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<void*>(1); }
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
// (fix32) the backend's later hooks into vgx_draw.cpp, which this test does not link: none of them is in the path under test
namespace vgx {
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t out[7]) { for (int i = 0; i < 7; ++i) out[i] = 0; }
void VtxPackDrawCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
bool PrepassAny() { return false; }
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
}
#endif

using namespace vgx;
using namespace vgx::backend;

static void Identity(float m[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[r][c] = r == c ? 1.0f : 0.0f;
}
// A copy of a known colour (a fill) of EFB rectangle w x h into texture `tex`: the back end clears the texture's target.
static CopyTexCmd Fill(uint32_t tex, uint16_t w, uint16_t h) {
    CopyTexCmd c;
    c.texId = tex;
    c.src = EfbRect{0, 0, static_cast<int16_t>(w), static_cast<int16_t>(h)};
    c.dstW = w;
    c.dstH = h;
    c.fill = true;
    c.fillRgba[3] = 1;
    Identity(c.mat);
    return c;
}
// One picture: the fills (texture, EFB size), a pass with draws into a pooled object of its own when `pass`, the display copy, the present.
static void Frame(uint64_t n, const std::vector<std::pair<uint32_t, std::pair<uint16_t, uint16_t>>>& fills, bool target) {
    CmdBuf b;
    b.frame = n;
    if (target) {
        b.targets.push_back(SetTargetCmd{640, 456});
        b.cmds.push_back(Cmd{CMD_SET_TARGET, 0});
    }
    for (const auto& f : fills) {
        b.copyTex.push_back(Fill(f.first, f.second.first, f.second.second));
        b.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)});
    }
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    b.copyDisp.push_back(d);
    b.cmds.push_back(Cmd{CMD_COPY_DISP, 0});
    b.cmds.push_back(Cmd{CMD_PRESENT, 0});
    Execute(b);
    model::Present();
}

int main(int argc, char** argv) {
    const char* defer = argc > 1 ? argv[1] : "4";
    setenv("GXV_RT_DEFER", defer, 1);
    setenv("GXV_STAGEWARM", "0", 1);
    setenv("GXV_AUTOQ", "0", 1);
    setenv("GXV_RUNG_SKIPRS", "1", 1);
    model::realWriteFrom = std::getenv("OBS_GPU_WRITE") ? std::atol(std::getenv("OBS_GPU_WRITE")) : 0;
    const bool expectHazard = std::atoi(defer) == 0;
    std::printf("GXV_RT_DEFER=%s%s\n", defer, model::realWriteFrom > 0 ? " (GPU writes into freed storage made for real: ASan reports the first)" : "");
    if (!Init(0, 0)) { std::printf("back end Init failed\n"); return 2; }

    // (A) two copy textures filled in every picture, never sampled (an effect whose result this scene draws nothing with), then a
    // change of rung: GXV_RUNG_SKIPRS hands over new textures and retires the old ones.
    using F = std::vector<std::pair<uint32_t, std::pair<uint16_t, uint16_t>>>;
    const F fillsA{{40, {160, 114}}, {41, {80, 57}}};
    Frame(1, fillsA, true);
    ForceQualityRung(2);
    for (uint64_t n = 2; n <= 8; ++n) Frame(n, fillsA, false);
    const unsigned name40 = DebugTexName(40), name41 = DebugTexName(41);
    const unsigned hazards0 = model::hazards;
    ForceQualityRung(1);
    for (uint64_t n = 9; n <= 16; ++n) Frame(n, fillsA, false);
    const unsigned hazardsA = model::hazards - hazards0;
    bool skipped = false;
    for (const auto& l : lines) skipped = skipped || (l.find("2 -> 1 parts") != std::string::npos && l.find(" 2 not resampled") != std::string::npos);
    std::printf("(A) rung change with textures rewritten every picture, never sampled:\n");
    Check(skipped, "the change skipped the resample of both (GXV_RUNG_SKIPRS: the 2.2 crash's precondition)");
    Check(DebugTexName(40) != name40 && DebugTexName(41) != name41, "both have new names after the change");
    Check(expectHazard ? hazardsA > 0 : hazardsA == 0,
          std::string("GPU writes into freed storage: ") + std::to_string(hazardsA) + (expectHazard ? " (the 2.2 behaviour: expected > 0)" : " (expected 0)"));

    // (B) a copy texture whose copy size changes (the dump: an 83x47 render target given 42x24 storage by CopyTexture's glTexImage2D).
    const unsigned hazards1 = model::hazards;
    for (uint64_t n = 17; n <= 22; ++n) Frame(n, F{{42, {80, 57}}}, false);
    const unsigned name42 = DebugTexName(42);
    for (uint64_t n = 23; n <= 28; ++n) Frame(n, F{{42, {40, 29}}}, false);
    const unsigned hazardsB = model::hazards - hazards1;
    std::printf("(B) a copy texture whose copy size changes:\n");
    Check(expectHazard ? DebugTexName(42) == name42 : DebugTexName(42) != name42, "GXV_RT_DEFER: a new name for the new size (0: storage replaced in place)");
    Check(expectHazard ? hazardsB > 0 : hazardsB == 0,
          std::string("GPU writes into freed storage: ") + std::to_string(hazardsB) + (expectHazard ? " (expected > 0)" : " (expected 0)"));

    // (C) back and forth between rungs while all of it goes on, then quiet: everything handed over is deleted in the end.
    const unsigned hazards2 = model::hazards;
    for (int k = 0; k < 6; ++k) {
        ForceQualityRung(k % 2 ? 2 : 0);
        for (uint64_t n = 0; n < 3; ++n) Frame(100 + k * 3 + n, F{{40, {160, 114}}, {41, {80, 57}}, {42, {static_cast<uint16_t>(k % 2 ? 40 : 80), 29}}}, false);
    }
    for (uint64_t n = 0; n < 8; ++n) Frame(200 + n, fillsA, false);
    const unsigned hazardsC = model::hazards - hazards2;
    unsigned waiting = 0;
    uint64_t handed = 0, deleted = 0, early = 0;
    DebugRtDefer(waiting, handed, deleted, early);
    std::printf("(C) rung changes every 3 pictures:\n");
    Check(expectHazard ? hazardsC > 0 : hazardsC == 0,
          std::string("GPU writes into freed storage: ") + std::to_string(hazardsC) + (expectHazard ? " (expected > 0)" : " (expected 0)"));
    if (!expectHazard) {
        Check(handed > 0 && waiting == 0 && deleted == handed,
              "handed over " + std::to_string(handed) + ", deleted " + std::to_string(deleted) + ", waiting " + std::to_string(waiting) + " (all deleted after 4 presents)");
        Check(early == 0, "the back end's own count of frees of storage drawn into within 4 presents: " + std::to_string(early));
    } else {
        Check(early > 0, "the back end's own count of early frees (\"[rt-defer]\" lines) sees them: " + std::to_string(early));
    }
    size_t textures = 0;
    for (const auto& kv : model::tex) textures += kv.second.data ? 1 : 0;
    std::printf("model: %u immediate frees, %u collector frees, %u GPU writes, %u into freed storage; %zu textures hold storage\n", model::immediateFrees,
                model::deferredFrees, model::gpuWrites, model::hazards, textures);
    for (const auto& s : model::notes) std::printf("  hazard: %s\n", s.c_str());
    for (const auto& l : lines)
        if (l.rfind("[rt-defer]", 0) == 0) std::printf("  log: %s\n", l.c_str());
    std::printf(g_fail ? "FAIL\n" : "PASS\n");
    return g_fail ? 1 : 0;
}
