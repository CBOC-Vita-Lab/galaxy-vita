// GXV_FXLOWRES=1 (lowres25): pictures of the reduced-size effect path against the full-size ones. Real GLES back end on Mesa; the
// runner (tools/test_vgx_fxlowres_gl.sh) runs this with the switch off and on and compares the RGBA readbacks (PSNR, the frames that
// must be identical) and writes PNGs. Three pictures at 960x544: (1) a far wall, a nearer wall over the right half and a near pillar,
// with far smoke ("over") and glows (additive) partly behind the nearer wall and near sparkles between them; (2) every effect behind a
// wall that covers the picture; (3) near effects only. The wrappers are those of tests/vgx_fxlowres_gl_test.cpp.
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace vgx { bool LogEnabled() { return false; } }
#ifdef FXLOWRES_BASE_TREE // (the runner's reference build: the same fixture against a tree without the switch; FXLOWRES_REF=<tree>)
namespace vgx::backend { void DebugFxLowres(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; } uint64_t DebugFxLowresCeiling() { return 0; }
void DebugFxLowresRender(uint64_t out[5]) { for (int i = 0; i < 5; ++i) out[i] = 0; } }
#endif
// The basic log: the census line is printed (the runner looks for it); every other line is dropped.
#include <cstdarg>
namespace { unsigned censusLines = 0; }
extern "C" void port_log_summary(const char* fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!std::strncmp(line, "fxlowres:", 9)) { ++censusLines; std::printf("%s\n", line); }
}
// (the back end linked alone: the front end's vertex-packing switch is off and counts nothing)
namespace vgx {
bool VtxPackOn() { return false; }
void VtxPackCounters(uint64_t out[7]) { for (int i = 0; i < 7; ++i) out[i] = 0; }
void VtxPackDrawCounters(uint64_t out[4]) { for (int i = 0; i < 4; ++i) out[i] = 0; }
// (the back end's alpha-test prepass hooks, dread25, merged after these tests were written: no pairs exist here)
bool PrepassPair(uint16_t, uint16_t&, uint16_t&) { return false; }
bool PrepassAny() { return false; }
void PrepassCounters(uint64_t out[9]) { for (int i = 0; i < 9; ++i) out[i] = 0; }
} // namespace vgx
namespace {
FILE* traceFile = nullptr;
unsigned draws = 0, uniforms = 0, targets = 0, copies = 0, reads = 0, finishes = 0;
int effectDivisor = 0;
[[maybe_unused]] const int unusedFixtureKnob = 0;
unsigned reducedViewports = 0;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint32_t Bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}
void Record(const char* name, std::initializer_list<uint64_t> values) {
    if (!traceFile) return;
    const uint32_t count = static_cast<uint32_t>(values.size());
    Require(std::fwrite(name, std::strlen(name) + 1, 1, traceFile) == 1, "write operation name");
    Require(std::fwrite(&count, sizeof count, 1, traceFile) == 1, "write argument count");
    for (uint64_t value : values)
        Require(std::fwrite(&value, sizeof value, 1, traceFile) == 1, "write operation argument");
}
void RecordFloats(const GLfloat* values, size_t count) {
    if (!traceFile) return;
    Require(std::fwrite(values, sizeof *values, count, traceFile) == count, "write uniform bytes");
}
} // namespace

#define WRAP(name, params, args, ...) \
    extern "C" void __real_##name params; \
    extern "C" void __wrap_##name params { Record(#name, {__VA_ARGS__}); __real_##name args; }
WRAP(glUseProgram, (GLuint p), (p), p)
WRAP(glEnable, (GLenum cap), (cap), cap)
WRAP(glDisable, (GLenum cap), (cap), cap)
WRAP(glScissor, (GLint x, GLint y, GLsizei w, GLsizei h), (x, y, w, h), uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h))
WRAP(glClear, (GLbitfield mask), (mask), mask)
WRAP(glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a), (r, g, b, a), Bits(r), Bits(g), Bits(b), Bits(a))
WRAP(glClearDepthf, (GLfloat depth), (depth), Bits(depth))
WRAP(glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a), (r, g, b, a), r, g, b, a)
WRAP(glDepthMask, (GLboolean mask), (mask), mask)
WRAP(glDepthFunc, (GLenum func), (func), func)
WRAP(glBlendFunc, (GLenum src, GLenum dst), (src, dst), src, dst)
WRAP(glBlendEquation, (GLenum mode), (mode), mode)
WRAP(glActiveTexture, (GLenum unit), (unit), unit)
WRAP(glBindTexture, (GLenum target, GLuint tex), (target, tex), target, tex)
WRAP(glTexParameteri, (GLenum target, GLenum name, GLint value), (target, name, value), target, name, uint64_t(value))
WRAP(glUniform1i, (GLint loc, GLint value), (loc, value), uint64_t(loc), uint64_t(value))
WRAP(glUniform1f, (GLint loc, GLfloat value), (loc, value), uint64_t(loc), Bits(value))
WRAP(glUniform2f, (GLint loc, GLfloat x, GLfloat y), (loc, x, y), uint64_t(loc), Bits(x), Bits(y))
WRAP(glUniform4f, (GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w), (loc, x, y, z, w), uint64_t(loc), Bits(x), Bits(y), Bits(z), Bits(w))
WRAP(glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum texTarget, GLuint tex, GLint level),
     (target, attachment, texTarget, tex, level), target, attachment, texTarget, tex, uint64_t(level))
WRAP(glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderTarget, GLuint renderbuffer),
     (target, attachment, renderTarget, renderbuffer), target, attachment, renderTarget, renderbuffer)
WRAP(glRenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h),
     (target, format, w, h), target, format, uint64_t(w), uint64_t(h))
WRAP(glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* data),
     (target, level, internal, w, h, border, format, type, data), target, uint64_t(level), uint64_t(internal), uint64_t(w), uint64_t(h), uint64_t(border), format, type, data != nullptr)
WRAP(glCopyTexImage2D, (GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border),
     (target, level, internal, x, y, w, h, border), target, uint64_t(level), internal, uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h), uint64_t(border))
WRAP(glFlush, (), ())
#undef WRAP

extern "C" void __real_glViewport(GLint, GLint, GLsizei, GLsizei);
extern "C" void __wrap_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    if (traceFile && effectDivisor && w == 64 / effectDivisor && h == 64 / effectDivisor) ++reducedViewports;
    Record("glViewport", {uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h)});
    __real_glViewport(x, y, w, h);
}
extern "C" void __real_glDrawElements(GLenum, GLsizei, GLenum, const void*);
extern "C" void __wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* offset) {
    if (traceFile) ++draws;
    Record("glDrawElements", {mode, uint64_t(count), type, reinterpret_cast<uintptr_t>(offset)});
    __real_glDrawElements(mode, count, type, offset);
}
extern "C" void __real_glDrawArrays(GLenum, GLint, GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (traceFile) ++copies;
    Record("glDrawArrays", {mode, uint64_t(first), uint64_t(count)});
    __real_glDrawArrays(mode, first, count);
}
extern "C" void __real_glUniform4fv(GLint, GLsizei, const GLfloat*);
extern "C" void __wrap_glUniform4fv(GLint loc, GLsizei count, const GLfloat* values) {
    if (traceFile) ++uniforms;
    Record("glUniform4fv", {uint64_t(loc), uint64_t(count)});
    RecordFloats(values, static_cast<size_t>(count) * 4);
    __real_glUniform4fv(loc, count, values);
}
extern "C" void __real_glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
extern "C" void __wrap_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat* values) {
    if (traceFile) ++uniforms;
    Record("glUniformMatrix4fv", {uint64_t(loc), uint64_t(count), transpose});
    RecordFloats(values, static_cast<size_t>(count) * 16);
    __real_glUniformMatrix4fv(loc, count, transpose, values);
}
extern "C" void __real_glBindFramebuffer(GLenum, GLuint);
extern "C" void __wrap_glBindFramebuffer(GLenum target, GLuint fbo) {
    if (traceFile) ++targets;
    Record("glBindFramebuffer", {target, fbo});
    __real_glBindFramebuffer(target, fbo);
}
extern "C" void __real_glCopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
extern "C" void __wrap_glCopyTexSubImage2D(GLenum target, GLint level, GLint dx, GLint dy, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (traceFile) ++copies;
    Record("glCopyTexSubImage2D", {target, uint64_t(level), uint64_t(dx), uint64_t(dy), uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h)});
    __real_glCopyTexSubImage2D(target, level, dx, dy, x, y, w, h);
}
extern "C" void __real_glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern "C" void __wrap_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    if (traceFile) ++reads;
    Record("glReadPixels", {uint64_t(x), uint64_t(y), uint64_t(w), uint64_t(h), format, type});
    __real_glReadPixels(x, y, w, h, format, type, pixels);
}
extern "C" void __real_glFinish();
extern "C" void __wrap_glFinish() {
    if (traceFile) ++finishes;
    Record("glFinish", {});
    __real_glFinish();
}

namespace {
using namespace vgx;
void Push(CmdBuf& b, CmdType type, uint32_t arg = 0) { b.cmds.push_back(Cmd{type, arg}); }
void Present(CmdBuf& b, bool clear = false) {
    CopyDispCmd d;
    d.src = EfbRect{0, 0, 640, 456};
    d.clear.enable = clear;
    b.copyDisp.push_back(d);
    Push(b, CMD_COPY_DISP, static_cast<uint32_t>(b.copyDisp.size() - 1));
    Push(b, CMD_PRESENT);
}
void Geometry(CmdBuf& b) {
    const float quad[16] = {-1, -1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, 1, 1, -1, 0};
    b.vtx.resize(sizeof quad);
    std::memcpy(b.vtx.data(), quad, sizeof quad);
    b.idx = {0, 1, 2, 2, 1, 3};
    const float n = 1.0f, f = 20000.0f;
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -(f + n) / (f - n), -1, 0, 0, -2.0f * f * n / (f - n), 0};
    b.uni.assign(proj, proj + 16);
}
// A camera-facing card: centre (cx, cy) in the picture's half-extents, half-size hx x hy, `dist` units from the camera plane.
// kind 0: opaque, writes depth (fs 1, flat colour); 1: "over" (fs 2, a soft round sprite); 2: additive (fs 2).
void Card(CmdBuf& b, int kind, float cx, float cy, float hx, float hy, float dist, float r, float g, float bl, float a) {
    DrawRec d;
    d.vsId = 1;
    d.fsId = kind == 0 ? 1 : 2;
    d.vtxCount = 4;
    d.idxCount = 6;
    d.ublock[UB_PROJ] = 0;
    d.ublock[UB_MV] = static_cast<uint32_t>(b.uni.size());
    // eye = (hx D x + cx D, hy D y + cy D, D z) for the unit quad at z = -1
    b.uni.insert(b.uni.end(), {hx * dist, 0, 0, cx * dist, 0, hy * dist, 0, cy * dist, 0, 0, dist, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    d.ublock[UB_TEV] = static_cast<uint32_t>(b.uni.size());
    b.uni.insert(b.uni.end(), {r, g, bl, a});
    d.rs.blend = kind != 0;
    d.rs.blendSrc = kind == 0 ? BF_ONE : BF_SRC_ALPHA;
    d.rs.blendDst = kind == 0 ? BF_ZERO : kind == 1 ? BF_INV_SRC_ALPHA : BF_ONE;
    d.rs.depthTest = 1;
    d.rs.depthFunc = CMP_LEQUAL;
    d.rs.depthWrite = kind == 0;
    d.rs.colorWrite = 1;
    d.rs.alphaWrite = 0;
    d.rs.scX = d.rs.scY = 0;
    d.rs.scW = 640;
    d.rs.scH = 456;
    d.rs.vpX = d.rs.vpY = 0;
    d.rs.vpW = 640;
    d.rs.vpH = 456;
    b.draws.push_back(d);
    Push(b, CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1));
}
void Save(FILE* output, const std::vector<uint8_t>& pixels, int w, int h) {
    Require(w > 0 && h > 0 && pixels.size() == static_cast<size_t>(w) * h * 4, "complete RGBA readback");
    const uint32_t dims[] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    Require(std::fwrite(dims, sizeof dims, 1, output) == 1, "write picture dimensions");
    Require(std::fwrite(pixels.data(), pixels.size(), 1, output) == 1, "write exact RGBA bytes");
}
// (a fixed pseudo-random sequence: the same scene in every process)
uint32_t g_seed = 12345;
float Rand() { g_seed = g_seed * 1664525u + 1013904223u; return static_cast<float>((g_seed >> 8) & 0xFFFF) / 65535.0f; }
} // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "output prefix argument");
    Require(host::CreateContext(), "Mesa EGL context");
    Require(backend::Init(960, 544), "real backend initialization");
    const std::string prefix = argv[1];
    FILE* pixelsFile = std::fopen((prefix + ".rgba").c_str(), "wb");
    Require(pixelsFile, "open the picture file");

    ShaderSource vs, fsFlat, fsSoft;
    vs.id = 1;
    vs.attrMask = 1u << AT_POS;
    vs.nMv = 3;
    vs.text = "attribute vec4 a_pos; uniform mat4 u_proj; uniform vec4 u_mv[3]; varying vec2 v_uv; void main() { vec4 p = vec4(a_pos.xyz, 1.0); v_uv = a_pos.xy; "
              "gl_Position = u_proj * vec4(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p), 1.0); }";
    fsFlat.id = 1;
    fsFlat.fragment = true;
    fsFlat.nTev = 1;
    fsFlat.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { gl_FragColor = vec4(u_tev[0].rgb * (0.85 + 0.15 * v_uv.y), 1.0); }";
    fsSoft.id = 2;
    fsFlat.keyHash = 0x1234ABCDEF01ull; // (freeze25: the stable content keys the GXV_FXLOWRES_K1..K4 list is matched with)
    fsSoft.keyHash = 0x9A8B7C6D5E4Full;
    fsSoft.fragment = true;
    fsSoft.nTev = 1;
    fsSoft.text = "precision mediump float; uniform vec4 u_tev[1]; varying vec2 v_uv; void main() { float f = clamp(1.0 - dot(v_uv, v_uv), 0.0, 1.0); "
                  "gl_FragColor = vec4(u_tev[0].rgb, u_tev[0].a * f * f); }";
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &fsFlat, &fsSoft};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Push(setup, CMD_SHADER, 2);
    Present(setup, true);
    backend::Execute(setup);

    for (unsigned frame = 2; frame <= 4; ++frame) {
        CmdBuf b;
        b.frame = frame;
        Geometry(b);
        g_seed = 12345;
        Card(b, 0, 0.0f, 0.0f, 1.0f, 1.0f, 9000, 0.10f, 0.14f, 0.30f, 1);          // the far wall
        if (frame == 3) Card(b, 0, 0.0f, 0.0f, 1.0f, 1.0f, 2600, 0.30f, 0.34f, 0.28f, 1); // a wall over the whole picture, in front of every effect
        else Card(b, 0, 0.55f, -0.1f, 0.45f, 0.7f, 4500, 0.30f, 0.34f, 0.28f, 1);  // the nearer wall, right half
        Card(b, 0, -0.35f, -0.2f, 0.06f, 0.8f, 1500, 0.45f, 0.30f, 0.18f, 1);      // a near pillar
        if (frame != 4) {
            for (int i = 0; i < 14; ++i)  // far smoke, behind the nearer wall where it stands
                Card(b, 1, -0.9f + 1.8f * Rand(), -0.6f + 1.0f * Rand(), 0.22f + 0.15f * Rand(), 0.22f + 0.15f * Rand(), 6000, 0.55f, 0.55f, 0.58f, 0.55f);
            for (int i = 0; i < 6; ++i)   // far glows
                Card(b, 2, -0.8f + 1.6f * Rand(), -0.2f + 1.0f * Rand(), 0.18f + 0.12f * Rand(), 0.18f + 0.12f * Rand(), 5000, 1.0f, 0.55f, 0.15f, 0.8f);
        }
        if (frame != 3) {
            for (int i = 0; i < 10; ++i)  // near sparkles (protected: full size)
                Card(b, 2, -0.5f + 1.0f * Rand(), -0.7f + 0.5f * Rand(), 0.02f, 0.03f, 1200, 1.0f, 1.0f, 0.6f, 1.0f);
        }
        if (frame == 2) {
            for (int i = 0; i < 8; ++i)   // smoke in front of the nearer wall, still beyond the near limit
                Card(b, 1, -0.2f + 1.1f * Rand(), -0.8f + 0.9f * Rand(), 0.16f + 0.1f * Rand(), 0.16f + 0.1f * Rand(), 3200, 0.75f, 0.72f, 0.70f, 0.45f);
            for (int i = 0; i < 30; ++i)  // small far sparks
                Card(b, 2, -0.9f + 1.8f * Rand(), -0.9f + 1.8f * Rand(), 0.008f, 0.012f, 5200, 1.0f, 0.9f, 0.5f, 1.0f);
        }
        Present(b, true);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        Require(w == 960 && h == 544, "requested render dimensions");
        Save(pixelsFile, pixels, w, h);
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
    }
    glFinish();
    uint64_t r[5] = {};
    backend::DebugFxLowresRender(r);
    std::printf("PASS: pictures written; GXV_FXLOWRES=1 decided %llu pictures: %llu runs at half size, %llu draws, %llu static, %llu runs left at full size\n",
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3], (unsigned long long)r[4]);
    Require(std::fclose(pixelsFile) == 0, "close the picture file");
    return 0;
}
