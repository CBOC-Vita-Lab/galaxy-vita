// GXV_UNI_SHADOW and GXV_BE_FAST2 (platform/vgx/src/vgx_backend_gl.cpp, agent be8): the real Draw over the GL stub, with a model of
// vitaGL's uniform handling in place of glUseProgram / glUniform4fv / glUniformMatrix4fv / vglDrawElementsDirect and of the two
// calls of be8's vgl.diff (vglGetUniformShadow, vglUniformShadowWritten). The model keeps, as custom_shaders.c does, a vertex and
// a fragment buffer per program (the "shadows"), the dirty flags that glUseProgram and every uniform write set, the ring copy each
// upload makes (re-used while the program's values are unchanged: GXV_VGL_UNIREUSE), and the buffer the GPU context points at
// (lost at a scene change). At every draw it checks that the bytes the GPU would read for each of the draw's uniform blocks are
// the draw's own (the arena's), and prints a digest of what the GPU reads, so that two runs (glUniform*, shadow) can be compared.
// A third of the programs say their u_tev is not F32 (a half precision shader), so that block keeps glUniform4fv in shadow mode.
// Build (host, g++):
//   g++ -std=c++17 -O1 -DNDEBUG -D__VGX_VITA__=1 -Iarmhf/glstub -Itests/autoq_clock_stubs -Iplatform/include -Iplatform/vgx/src
//       -Iplatform/vgx/third_party tests/vgx_uni_shadow.cpp platform/vgx/src/vgx_shadergen.cpp platform/vgx/src/vgx_autoq.cpp
//       -o /tmp/uni-shadow -lpthread
//   /tmp/uni-shadow 0 0 > a; /tmp/uni-shadow 1 1 > b; /tmp/uni-shadow 0 1 > c; /tmp/uni-shadow 1 0 > d; cmp a b && cmp a c && cmp a d
//   (arguments: GXV_UNI_SHADOW, GXV_BE_FAST2; the last line of each run gives its counts and differs between runs)
#include <GLES2/gl2.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace model {
struct Uni { unsigned prog; std::string name; bool frag; uint32_t off; uint32_t vec4s; };
struct Prog {
    std::vector<float> v, f;          // vitaGL's shadow buffers (unif_vbuffer / unif_fbuffer)
    std::vector<float> ringV, ringF;  // the ring copy of the last upload
    bool ringVOk = false, ringFOk = false;
    uint32_t vUsed = 0, fUsed = 0;    // floats handed out
};
std::map<unsigned, Prog> progs;
std::vector<Uni> unis;                // location = index + 1
unsigned cur = 0;
bool dirtyV = true, dirtyF = true, sceneDue = false;
const std::vector<float>*ctxV = nullptr, *ctxF = nullptr; // what the GPU context points at
unsigned fbo = 0;
uint64_t copiesV = 0, copiesF = 0, uniCalls = 0, shadowCalls = 0;
bool halfTev(unsigned prog) { return prog % 3 == 0; }
void Fail(const char* why) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    std::exit(1);
}
void InvalidateAll() {
    for (auto& kv : progs) kv.second.ringVOk = kv.second.ringFOk = false;
}
void Write(int loc, int n, const float* data) {
    if (loc <= 0 || static_cast<size_t>(loc) > unis.size()) Fail("bad location");
    const Uni& u = unis[loc - 1];
    Prog& p = progs[u.prog];
    if (static_cast<uint32_t>(n) > u.vec4s) Fail("upload beyond the uniform");
    std::vector<float>& b = u.frag ? p.f : p.v;
    std::memcpy(b.data() + u.off, data, static_cast<size_t>(n) * 16u);
    (u.frag ? dirtyF : dirtyV) = true;
    if (u.prog == cur) (u.frag ? p.ringFOk : p.ringVOk) = false;
    else InvalidateAll();
    ++uniCalls;
}
} // namespace model

extern "C" {
void m_glUseProgram(GLuint prog) {
    if (prog == model::cur) return;
    model::cur = prog;
    model::dirtyV = model::dirtyF = true;
}
void m_glUniform4fv(GLint loc, GLsizei n, const GLfloat* v) { model::Write(loc, n, v); }
void m_glUniformMatrix4fv(GLint loc, GLsizei n, GLboolean transpose, const GLfloat* v) {
    if (transpose || n != 1) model::Fail("matrix upload");
    model::Write(loc, 4, v);
}
void m_glBindFramebuffer(GLenum, GLuint f) {
    if (f != model::fbo) model::sceneDue = true;
    model::fbo = f;
}
}
#define glUseProgram m_glUseProgram
#define glUniform4fv m_glUniform4fv
#define glUniformMatrix4fv m_glUniformMatrix4fv
#define glBindFramebuffer m_glBindFramebuffer

#include "../platform/vgx/src/vgx_backend_gl.cpp"

namespace {
std::vector<std::string> lines;
const vgx::CmdBuf* g_buf = nullptr;
const vgx::DrawRec* g_rec = nullptr;
uint64_t g_digest = 1469598103934665603ull, g_checked = 0;
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull;
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
// vitaGL's link: every data uniform gets its place in its stage's buffer (u_tev in the fragment one), as large as the GL reports
int glGetUniformLocation(unsigned prog, const char* name) {
    for (size_t i = 0; i < model::unis.size(); ++i)
        if (model::unis[i].prog == prog && model::unis[i].name == name) return static_cast<int>(i + 1);
    if (!std::strncmp(name, "u_tex", 5)) return -1;
    uint32_t n = 1;
    for (const VgxStubUniform& u : vgx_stub_uni_[prog])
        if (u.name == name) n = static_cast<uint32_t>(u.size);
    if (!std::strcmp(name, "u_proj")) n = 4;
    model::Prog& p = model::progs[prog];
    const bool frag = !std::strcmp(name, "u_tev");
    uint32_t& used = frag ? p.fUsed : p.vUsed;
    model::unis.push_back({prog, name, frag, used, n});
    used += n * 4;
    (frag ? p.f : p.v).assign(used, -7.0f); // (never-written values: the check below would see them)
    return static_cast<int>(model::unis.size());
}
unsigned char vglGetUniformShadow(unsigned int prog, int loc, void** vptr, void** fptr, uint32_t* bytes) {
    ++model::shadowCalls;
    if (loc <= 0 || static_cast<size_t>(loc) > model::unis.size()) return 0;
    const model::Uni& u = model::unis[loc - 1];
    if (u.prog != prog) return 0;
    if (u.frag && model::halfTev(prog)) return 0; // (an F16 u_tev: glUniform4fv converts it)
    model::Prog& p = model::progs[prog];
    *vptr = u.frag ? nullptr : p.v.data() + u.off;
    *fptr = u.frag ? p.f.data() + u.off : nullptr;
    *bytes = u.vec4s * 16u;
    return 1;
}
void vglUniformShadowWritten(unsigned int prog, uint32_t stages) {
    ++model::shadowCalls;
    if (stages & 1) model::dirtyV = true;
    if (stages & 2) model::dirtyF = true;
    if (prog == model::cur) {
        if (stages & 1) model::progs[prog].ringVOk = false;
        if (stages & 2) model::progs[prog].ringFOk = false;
    } else {
        model::InvalidateAll();
    }
}
// vitaGL's draw: a scene begun since the last draw has lost the context's uniform buffers; dirty stages are uploaded (or re-pointed)
unsigned char vglDrawElementsDirect(unsigned, int, uint32_t, unsigned, uint32_t, const void*) {
    using namespace model;
    if (sceneDue) { ctxV = ctxF = nullptr; sceneDue = false; }
    Prog& p = progs[cur];
    if (dirtyV) {
        if (!p.ringVOk) { p.ringV = p.v; p.ringVOk = true; ++copiesV; }
        ctxV = &p.ringV;
        dirtyV = false;
    }
    if (dirtyF) {
        if (!p.ringFOk) { p.ringF = p.f; p.ringFOk = true; ++copiesF; }
        ctxF = &p.ringF;
        dirtyF = false;
    }
    // what the GPU reads for each block of the draw must be the draw's own values
    using namespace vgx;
    const DrawRec& r = *g_rec;
    const float* arena = g_buf->uni.data();
    auto check = [&](const char* name, int b, size_t skip, const char* what) {
        if (r.ublock[b] == kNoBlock) return;
        for (const Uni& u : unis) {
            if (u.prog != cur || u.name != name) continue;
            const std::vector<float>* ctx = u.frag ? ctxF : ctxV;
            if (!ctx) Fail("the GPU context has no uniform buffer for this scene");
            if (std::memcmp(ctx->data() + u.off, arena + r.ublock[b] + skip, u.vec4s * 16u) != 0) Fail(what);
            Mix(ctx->data() + u.off, u.vec4s * 16u);
            ++g_checked;
        }
    };
    check("u_proj", UB_PROJ, 0, "u_proj is not the draw's");
    check("u_mv", UB_MV, 0, "u_mv is not the draw's");
    check("u_tm", UB_TEXM, 0, "u_tm is not the draw's");
    check("u_ptm", UB_TEXM, 96, "u_ptm is not the draw's");
    check("u_pal", UB_PAL, 0, "u_pal is not the draw's");
    check("u_lt", UB_LIGHT, 0, "u_lt is not the draw's");
    check("u_tev", UB_TEV, 0, "u_tev is not the draw's");
    return 1;
}
const void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<const void*>(1); }
void vglSetNextSceneDepthModes(unsigned char, unsigned char) {}
void vglUseLowPrecisionFragmentOnly(unsigned char) {}
int vglFramebufferDepthTexture(unsigned) { return 0; }
size_t vglMemFree(int) { return 64u << 20; }
size_t vglMemTotal(int) { return 128u << 20; }
unsigned glCheckFramebufferStatus(unsigned) { return 0x8CD5; }
unsigned glGetError(void) { return 0; }
const unsigned char* glGetString(unsigned name) {
    return reinterpret_cast<const unsigned char*>(name == 0x1F03 ? "GL_OES_depth24 GL_EXT_texture_compression_s3tc" : "stub");
}
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

int main(int argc, char** argv) {
    const bool shadow = argc > 1 && argv[1][0] == '1';
    const bool fast2 = argc > 2 && argv[2][0] == '1';
    // 0. SameWords against memcmp, every length to 80 words, differences at every place, unaligned starts
    {
        std::mt19937 rng(9);
        std::vector<uint32_t> a(96), b(96);
        for (int t = 0; t < 20000; ++t) {
            const size_t n = rng() % 81, o = rng() % 8;
            for (size_t i = 0; i < n; ++i) a[o + i] = b[o + i] = rng() % 4;
            if (n && rng() % 2) b[o + rng() % n] ^= 1u << (rng() % 32);
            if (SameWords(a.data() + o, b.data() + o, n) != (std::memcmp(a.data() + o, b.data() + o, n * 4) == 0)) model::Fail("SameWords");
        }
    }
    g_beFast2 = fast2;
    g_uniShadow = shadow;
    g_gxmDirect = 1;
    g_rtW = 640;
    g_rtH = 528;
    g_streamVbo.assign(1, 77);
    // shaders: vertex ones with different uniform sets, fragment ones with u_tev of different sizes
    std::vector<ShaderSource> vs(6), fs(4);
    for (int i = 0; i < 6; ++i) {
        ShaderSource& s = vs[i];
        s.id = static_cast<uint16_t>(i + 1);
        s.nMv = 6;
        s.nLt = (i % 2) ? 24 : 9;
        s.nTm = (i % 3 == 1) ? 6 : 0;
        s.nPtm = (i == 4) ? 6 : 0;
        s.nPal = (i == 5) ? 30 : 0;
        s.text = "uniform mat4 u_proj;\nuniform vec4 u_mv[6];\nuniform vec4 u_lt[" + std::to_string(s.nLt) + "];\n";
        if (s.nTm) s.text += "uniform vec4 u_tm[" + std::to_string(s.nTm) + "];\n";
        if (s.nPtm) s.text += "uniform vec4 u_ptm[" + std::to_string(s.nPtm) + "];\n";
        if (s.nPal) s.text += "uniform vec4 u_pal[" + std::to_string(s.nPal) + "];\n";
        s.text += "attribute vec4 a_pos;\n";
        RegisterShader(&s);
    }
    for (int i = 0; i < 4; ++i) {
        ShaderSource& s = fs[i];
        s.fragment = true;
        s.id = static_cast<uint16_t>(i + 1);
        s.nTev = static_cast<uint8_t>(4 + 2 * i);
        s.text = "uniform vec4 u_tev[" + std::to_string(s.nTev) + "];\n";
        RegisterShader(&s);
    }
    // a pool of block contents; draws take them at new arena offsets (same values, other offset), at the same offset, or new values
    std::mt19937 rng(1234);
    CmdBuf bufs[2];
    uint64_t draws = 0;
    for (int frame = 0; frame < 60; ++frame) {
        ++g_serial;
        CmdBuf& buf = bufs[frame % 2];
        buf.uni.clear();
        std::vector<uint32_t> last[UB_COUNT];
        auto block = [&](int b) -> uint32_t {
            const uint32_t size = kUbSize[b];
            const int pick = static_cast<int>(rng() % 10);
            if (pick < 3 && !last[b].empty()) return last[b][rng() % last[b].size()]; // the same arena block again
            const uint32_t off = static_cast<uint32_t>(buf.uni.size());
            buf.uni.resize(off + size);
            if (pick < 6 && !last[b].empty()) { // same values, new offset
                const uint32_t from = last[b][rng() % last[b].size()];
                for (uint32_t i = 0; i < size; ++i) buf.uni[off + i] = buf.uni[from + i];
            } else {
                for (uint32_t i = 0; i < size; ++i) buf.uni[off + i] = static_cast<float>(rng() % 5) * 0.5f; // (few values: repeats happen)
            }
            last[b].push_back(off);
            return off;
        };
        // the blocks first (the arena must not move under the draws' offsets), then the draws
        std::vector<DrawRec> recs;
        for (int d = 0; d < 120; ++d) {
            DrawRec r{};
            r.vsId = static_cast<uint16_t>(1 + rng() % 6);
            r.fsId = static_cast<uint16_t>(1 + rng() % 4);
            r.idxCount = 3;
            r.prim = PRIM_TRIANGLES;
            r.ublock[UB_PROJ] = block(UB_PROJ);
            r.ublock[UB_MV] = block(UB_MV);
            r.ublock[UB_LIGHT] = block(UB_LIGHT);
            r.ublock[UB_TEV] = block(UB_TEV);
            if (rng() % 2) r.ublock[UB_TEXM] = block(UB_TEXM);
            if (rng() % 2) r.ublock[UB_PAL] = block(UB_PAL);
            recs.push_back(r);
        }
        for (const DrawRec& r : recs) {
            if (rng() % 25 == 0) BindFbo(static_cast<GLuint>(1 + rng() % 3)); // (a target change: a new scene for vitaGL)
            g_buf = &buf;
            g_rec = &r;
            Draw(buf, r);
            ++draws;
        }
    }
    std::printf("digest %016llx checks %llu\n", static_cast<unsigned long long>(g_digest), static_cast<unsigned long long>(g_checked));
    std::fprintf(stdout, "# shadow=%d fast2=%d draws %llu: glUniform calls %llu, shadow calls %llu, ring copies vertex %llu fragment %llu\n", shadow, fast2,
                 static_cast<unsigned long long>(draws), static_cast<unsigned long long>(model::uniCalls), static_cast<unsigned long long>(model::shadowCalls),
                 static_cast<unsigned long long>(model::copiesV), static_cast<unsigned long long>(model::copiesF));
    if (!g_checked) model::Fail("nothing checked");
    return 0;
}
