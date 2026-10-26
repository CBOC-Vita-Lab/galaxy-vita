// Real GLES backend dispatch. The runner compares modes in separate processes;
// wrappers observe calls but always forward them to Mesa, including readback.
// A lost draw, changed uniform, or changed copy/target operation must fail even
// when both modes would otherwise produce the same empty picture.
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
namespace {
FILE* traceFile = nullptr;
unsigned draws = 0, uniforms = 0, targets = 0, copies = 0, reads = 0, finishes = 0;
int effectDivisor = 0;
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
    const float quad[16] = {-1, -1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1, 1, 0, 0};
    b.vtx.resize(sizeof quad);
    std::memcpy(b.vtx.data(), quad, sizeof quad);
    b.idx = {0, 1, 2, 2, 1, 3};
    b.uni.assign(16, 0);
    for (unsigned i = 0; i < 4; ++i) b.uni[i * 5] = 1;
}
void AddDraw(CmdBuf& b, bool right, float r, float g, float blue, bool additive = true) {
    DrawRec d;
    d.vsId = d.fsId = 1;
    d.vtxCount = 4;
    d.idxCount = 6;
    d.ublock[UB_PROJ] = 0;
    d.ublock[UB_TEV] = static_cast<uint32_t>(b.uni.size());
    b.uni.insert(b.uni.end(), {r, g, blue, 0});
    d.rs.blend = additive;
    d.rs.blendSrc = BF_ONE;
    d.rs.blendDst = BF_ONE;
    d.rs.depthTest = 1;
    d.rs.depthFunc = CMP_ALWAYS;
    d.rs.depthWrite = 0;
    d.rs.scX = right ? 320 : 0;
    d.rs.scW = 320;
    d.rs.scH = 456;
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
void CheckRegions(const std::vector<uint8_t>& pixels, int w, int h, int green) {
    Require(w == 64 && h == 64, "requested render dimensions");
    for (int y : {h / 4, h * 3 / 4}) {
        const uint8_t* left = &pixels[(static_cast<size_t>(y) * w + w / 4) * 4];
        const uint8_t* right = &pixels[(static_cast<size_t>(y) * w + w * 3 / 4) * 4];
        Require(left[0] >= 126 && left[0] <= 130 && std::abs(int(left[1]) - green) <= 2 && left[2] == 0,
                "left region contains independently expected additive red and green");
        Require(right[0] == 0 && right[1] == 0 && right[2] >= 126 && right[2] <= 130,
                "right region contains independently expected additive blue");
    }
}
} // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "output prefix argument");
    effectDivisor = std::getenv("GXV_FXHALF") ? std::atoi(std::getenv("GXV_FXHALF")) : 0;
    Require(effectDivisor == 0 || effectDivisor == 2 || effectDivisor == 4, "supported fixture effect mode");
    Require(host::CreateContext(), "Mesa EGL context");
    Require(backend::Init(64, 64), "real backend initialization");
    const std::string prefix = argv[1];
    traceFile = std::fopen((prefix + ".trace").c_str(), "wb");
    FILE* pixelsFile = std::fopen((prefix + ".rgba").c_str(), "wb");
    Require(traceFile && pixelsFile, "open comparison artifacts");

    ShaderSource vs, fs;
    vs.id = fs.id = 1;
    vs.attrMask = 1u << AT_POS;
    vs.text = "attribute vec4 a_pos; uniform mat4 u_proj; void main() { gl_Position = u_proj * vec4(a_pos.xyz, 1.0); }";
    fs.fragment = true;
    fs.nTev = 1;
    fs.text = "precision mediump float; uniform vec4 u_tev[1]; void main() { gl_FragColor = u_tev[0]; }";
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &fs};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Present(setup, true);
    backend::Execute(setup);

    for (unsigned frame = 2; frame <= 4; ++frame) {
        CmdBuf b;
        b.frame = frame;
        Geometry(b);
        if (frame == 2) {
            AddDraw(b, false, 0.25f, 0, 0);
            TexUpload upload;
            upload.texId = 10;
            upload.width = upload.height = 1;
            upload.levels.push_back({255, 255, 255, 255});
            b.uploads.push_back(upload);
            Push(b, CMD_TEX_UPLOAD);
            AddDraw(b, false, 0.25f, 0, 0);
            Push(b, CMD_TEX_FREE, 10);
            Copy(b, 20); // A real copy terminates the first eligible run.
            AddDraw(b, true, 0, 0, 0.25f);
            AddDraw(b, true, 0, 0, 0.25f);
        } else if (frame == 3) {
            // The previous frame's result must survive; only the left green channel changes.
            AddDraw(b, false, 0, 0.125f, 0);
            AddDraw(b, false, 0, 0.125f, 0);
        } else {
            // No eligible draws follow the previous frame's nonempty FX mask.
            AddDraw(b, false, 0.5f, 0.25f, 0, false);
        }
        Copy(b, 21);
        Present(b);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        Require(backend::GetStats().drawCallsThisFrame == (frame == 2 ? 4u : (frame == 3 ? 2u : 1u)), "actual DrawRec dispatch count");
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        CheckRegions(pixels, w, h, frame == 2 ? 0 : 64);
        Save(pixelsFile, pixels, w, h);
        std::vector<uint8_t> copy;
        Require(backend::DebugReadTexture(21, copy, w, h), "real copied texture readback");
        CheckRegions(copy, w, h, frame == 2 ? 0 : 64);
        Save(pixelsFile, copy, w, h);
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
    }
    glFinish();
    Require(draws == 7 && uniforms >= 6 && targets >= 4 && copies >= 3 && reads >= 6 && finishes >= 1,
            "nonempty real draw, uniform, target, copy, read and finish operations");
    Require(!effectDivisor || reducedViewports >= 3, "enabled FX really draws its runs into reduced targets");
    std::printf("PASS: indexed draws=%u uniforms=%u targets=%u copy operations=%u reads=%u finishes=%u reduced viewports=%u\n",
                draws, uniforms, targets, copies, reads, finishes, reducedViewports);
    const int traceClose = std::fclose(traceFile);
    const int pixelsClose = std::fclose(pixelsFile);
    Require(traceClose == 0 && pixelsClose == 0, "close comparison artifacts");
    traceFile = nullptr;
    return 0;
}
