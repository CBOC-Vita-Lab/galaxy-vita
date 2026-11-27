// GXV_FXLOWRES (lowres25): the census of what a reduced-size effect buffer could take must not change a single GL call or pixel,
// and must count what the fixture draws. Real GLES back end on Mesa; the runner (tools/test_vgx_fxlowres_gl.sh) runs the switch
// unset / 0 / 3 / 1 in separate processes and compares the ordered GL traces and the RGBA readbacks byte for byte (the wrappers
// are those of tests/vgx_fxscan_gl_test.cpp: they record every call and forward it to Mesa).
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
// One unit quad at z = -1 (x, y in -1..1) and a perspective projection whose clip w is the distance from the camera plane.
void Geometry(CmdBuf& b) {
    const float quad[16] = {-1, -1, -1, 0, 1, -1, -1, 0, -1, 1, -1, 0, 1, 1, -1, 0};
    b.vtx.resize(sizeof quad);
    std::memcpy(b.vtx.data(), quad, sizeof quad);
    b.idx = {0, 1, 2, 2, 1, 3};
    const float n = 1.0f, f = 20000.0f;
    // (column-major) x' = x, y' = y, z' = a z + c, w' = -z
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -(f + n) / (f - n), -1, 0, 0, -2.0f * f * n / (f - n), 0};
    b.uni.assign(proj, proj + 16);
}
enum Blend { kAdd, kOver };
struct Sprite { float dist, half; Blend blend; bool zTest, zWrite; uint16_t vs; float r, g, bl, a; };
// A camera-facing card `dist` units in front of the camera covering half x half of the picture's half-extent: (2 half)^2 / 4 screens.
void AddSprite(CmdBuf& b, const Sprite& s) {
    DrawRec d;
    d.vsId = s.vs;
    d.fsId = 1;
    d.vtxCount = 4;
    d.idxCount = 6;
    d.ublock[UB_PROJ] = 0;
    d.ublock[UB_MV] = static_cast<uint32_t>(b.uni.size());
    const float k = s.half * s.dist;
    b.uni.insert(b.uni.end(), {k, 0, 0, 0, 0, k, 0, 0, 0, 0, s.dist, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
    d.ublock[UB_TEV] = static_cast<uint32_t>(b.uni.size());
    b.uni.insert(b.uni.end(), {s.r, s.g, s.bl, s.a});
    d.rs.blend = 1;
    d.rs.blendSrc = s.blend == kAdd ? BF_ONE : BF_SRC_ALPHA;
    d.rs.blendDst = s.blend == kAdd ? BF_ONE : BF_INV_SRC_ALPHA;
    d.rs.depthTest = s.zTest;
    d.rs.depthFunc = CMP_LEQUAL;
    d.rs.depthWrite = s.zWrite;
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
void Copy(CmdBuf& b, uint32_t id) {
    CopyTexCmd c;
    c.texId = id;
    c.src = EfbRect{0, 0, 640, 456};
    c.dstW = 640;
    c.dstH = 456;
    for (unsigned i = 0; i < 4; ++i) c.mat[i][i] = 1;
    b.copyTex.push_back(c);
    Push(b, CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1));
}
void Save(FILE* output, const std::vector<uint8_t>& pixels, int w, int h) {
    Require(w > 0 && h > 0 && pixels.size() == static_cast<size_t>(w) * h * 4, "complete RGBA readback");
    const uint32_t dims[] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    Require(std::fwrite(dims, sizeof dims, 1, output) == 1, "write picture dimensions");
    Require(std::fwrite(pixels.data(), pixels.size(), 1, output) == 1, "write exact RGBA bytes");
}
bool Near(uint64_t got, uint64_t want, uint64_t slack) { return got + slack >= want && got <= want + slack; }
} // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "output prefix argument");
    const char* e = std::getenv("GXV_FXLOWRES");
    const int mode = e ? std::atoi(e) : 0;
    Require(host::CreateContext(), "Mesa EGL context");
    Require(backend::Init(64, 64), "real backend initialization");
    const std::string prefix = argv[1];
    traceFile = std::fopen((prefix + ".trace").c_str(), "wb");
    FILE* pixelsFile = std::fopen((prefix + ".rgba").c_str(), "wb");
    Require(traceFile && pixelsFile, "open comparison artifacts");

    ShaderSource vs, vsNoMv, fs;
    vs.id = fs.id = 1;
    vs.attrMask = 1u << AT_POS;
    vs.nMv = 3;
    vs.text = "attribute vec4 a_pos; uniform mat4 u_proj; uniform vec4 u_mv[3]; void main() { vec4 p = vec4(a_pos.xyz, 1.0); "
              "gl_Position = u_proj * vec4(dot(u_mv[0], p), dot(u_mv[1], p), dot(u_mv[2], p), 1.0); }";
    // (positions the census cannot follow: no u_mv block declared, as a palette or static-buffer draw)
    vsNoMv.id = 2;
    vsNoMv.attrMask = 1u << AT_POS;
    vsNoMv.text = "attribute vec4 a_pos; uniform mat4 u_proj; void main() { gl_Position = u_proj * vec4(a_pos.xy * 100.0, -4000.0, 1.0); }";
    fs.fragment = true;
    fs.nTev = 1;
    fs.text = "precision mediump float; uniform vec4 u_tev[1]; void main() { gl_FragColor = u_tev[0]; }";
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &vsNoMv, &fs};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Push(setup, CMD_SHADER, 2);
    Present(setup, true);
    backend::Execute(setup);

    const int ceiling = std::getenv("GXV_FXLOWRES_CEILING") ? std::atoi(std::getenv("GXV_FXLOWRES_CEILING")) : 0;
    const unsigned ceilingSkips = ceiling == 1 || ceiling == 3 ? 6u : ceiling == 4 ? 7u : 0u;
    const unsigned frames = 6;
    for (unsigned frame = 2; frame < 2 + frames; ++frame) {
        CmdBuf b;
        b.frame = frame;
        Geometry(b);
        // First stretch: two far cards with a near one between them (the full-size draw "inside" the stretch), then what never moves.
        AddSprite(b, {5000, 0.5f, kAdd, true, false, 1, 0.25f, 0.0f, 0.0f, 1.0f});    // far additive, 0.250 screens
        AddSprite(b, {1000, 0.05f, kOver, true, false, 1, 0.0f, 1.0f, 0.0f, 0.5f});   // near, small sprite, 0.0025 screens
        AddSprite(b, {3000, 0.25f, kOver, true, false, 1, 0.0f, 0.0f, 1.0f, 0.5f});   // far "over", 0.0625 screens
        AddSprite(b, {1000, 0.3f, kOver, true, false, 1, 0.5f, 0.5f, 0.0f, 0.25f});   // near, large sprite, 0.090 screens
        AddSprite(b, {1000, 0.25f, kOver, false, false, 1, 0.0f, 0.25f, 0.25f, 0.5f}); // flat (no depth test), 0.0625 screens
        AddSprite(b, {5000, 0.1f, kOver, true, true, 1, 1.0f, 1.0f, 1.0f, 0.5f});     // sorted (writes depth), 0.010 screens
        AddSprite(b, {4000, 0.1f, kAdd, true, false, 2, 0.1f, 0.1f, 0.1f, 1.0f});     // a candidate whose vertices cannot be followed
        Copy(b, 20);                                                                  // a scene boundary
        AddSprite(b, {4000, 0.2f, kAdd, true, false, 1, 0.0f, 0.125f, 0.0f, 1.0f});   // second stretch: far additive, 0.040 screens
        Copy(b, 21);
        Present(b, true); // (the display copy clears the EFB: every picture starts black)
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        // (GXV_FXLOWRES_CEILING, the diagnostic: 1 / 3 leave out the six stream candidates, 4 the depth-writing card too, 2 nothing here: no static draw)
        Require(backend::GetStats().drawCallsThisFrame == 8u - ceilingSkips, "every draw of the fixture is dispatched, but the class the diagnostic leaves out");
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        Require(w == 64 && h == 64, "requested render dimensions");
        // (the centre: every card covers it; a corner: only the far additive card's red, then nothing else but the half-size ones do not reach it)
        const uint8_t* centre = &pixels[(static_cast<size_t>(32) * w + 32) * 4];
        const uint8_t* corner = &pixels[(static_cast<size_t>(20) * w + 20) * 4];
        if (!ceilingSkips) {
            Require(centre[0] > 0 && centre[1] > 0 && centre[2] > 0, "the cards are really drawn over the centre");
            Require(corner[0] >= 60 && corner[0] <= 68 && corner[1] == 0 && corner[2] == 0, "only the far additive card reaches (20, 20): red 0.25");
        } else {
            Require(corner[0] == 0 && corner[1] == 0 && corner[2] == 0, "diagnostic: the far additive card is not drawn");
            Require(centre[1] > 0, "diagnostic: the flat card (no depth test) is still drawn");
        }
        Save(pixelsFile, pixels, w, h);
        std::vector<uint8_t> copy;
        Require(backend::DebugReadTexture(21, copy, w, h), "real copied texture readback");
        Save(pixelsFile, copy, w, h);
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
    }
    glFinish();
    uint64_t c[9] = {};
    backend::DebugFxLowres(c);
    if (mode == 0) {
        for (uint64_t v : c) Require(v == 0, "switch off: the census does not run");
    } else {
        const unsigned every = std::getenv("GXV_FXLOWRES_EVERY") ? static_cast<unsigned>(std::atoi(std::getenv("GXV_FXLOWRES_EVERY"))) : 4u;
        const long nearUnits = std::getenv("GXV_FXLOWRES_NEAR") ? std::atol(std::getenv("GXV_FXLOWRES_NEAR")) : 2500;
        // (the set-up frame is scanned too: frames 1..7, every `every`-th from the first)
        const uint64_t all = frames + 1, measured = (all + every - 1) / every, withDraws = every == 1 ? frames : measured - 1;
        Require(c[0] == measured, "pictures measured: one in GXV_FXLOWRES_EVERY");
        if (every == 1 && nearUnits == 2500) {
            Require(Near(c[1], withDraws * 352, withDraws * 3), "far fill: 0.250 + 0.0625 + 0.040 screens a picture");
            Require(Near(c[2], withDraws * 90, withDraws), "near large-sprite fill: 0.090 screens a picture");
            Require(Near(c[3], withDraws * 2, withDraws), "near fill: 0.0025 screens a picture");
            Require(Near(c[4], withDraws * 62, withDraws), "flat fill: 0.0625 screens a picture");
            Require(Near(c[5], withDraws * 10, withDraws), "sorted fill: 0.010 screens a picture");
            Require(c[6] == withDraws * 3, "three far draws a picture");
            Require(c[7] == withDraws, "one candidate a picture whose vertices are not at hand");
        } else if (every == 1 && nearUnits == 6000) {
            Require(c[1] == 0 && c[6] == 0, "near limit 6000: nothing is far");
            Require(Near(c[2], withDraws * (250 + 62 + 90 + 40), withDraws * 4), "near limit 6000: the big cards are near large-sprite ones");
        } else if (every == 1 && nearUnits == 0) {
            Require(c[6] == withDraws * 5 && c[2] == 0 && c[3] == 0, "near limit 0: every measured candidate is far");
        }
    }
    Require(backend::DebugFxLowresCeiling() == static_cast<uint64_t>(frames) * ceilingSkips, "the diagnostic's count of draws left out");
    if (mode != 0 && std::getenv("GXV_FXLOWRES_LOG")) Require(censusLines >= 1, "the census line is written every GXV_FXLOWRES_LOG measured pictures");
    if (mode == 0 && ceiling == 0) Require(censusLines == 0, "switches off: no line");
    std::printf("PASS: GXV_FXLOWRES=%d ceiling=%d indexed draws=%u; census pictures %llu far %llu big %llu near %llu flat %llu sorted %llu (thousandths of a screen), far draws %llu, unmeasured %llu, paying %llu\n",
                mode, ceiling, draws, (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2], (unsigned long long)c[3], (unsigned long long)c[4],
                (unsigned long long)c[5], (unsigned long long)c[6], (unsigned long long)c[7], (unsigned long long)c[8]);
    const int traceClose = std::fclose(traceFile);
    const int pixelsClose = std::fclose(pixelsFile);
    Require(traceClose == 0 && pixelsClose == 0, "close comparison artifacts");
    traceFile = nullptr;
    return 0;
}
