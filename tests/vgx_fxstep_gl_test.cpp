// The quality steps (GXV_AUTOQ_FXSTEP) on the real GLES back end and Mesa: the runtime state the controller flips between
// frames (backend::DebugSetQualitySteps) against the static switches. Usage: vgx_fxstep_gl_test <fx|fxi|bg> <output prefix> <schedule>
// where the schedule has one letter per frame 2..4: '-' no step, 'f' half-size effects, 'b' cheaper background, 'x' both.
//   fx  the fxscan fixture (additive runs, a depth-tested copy between them): with the schedule "fff" the ordered GL operations and
//       the pictures must equal those of a process run with GXV_FXHALF=2; with "---" those of a process with no switch at all, and
//       a frame after a stepped frame must equal the same frame of the unstepped process (no state left behind).
//   fxi the same scene in every frame, each frame cleared afterwards: a frame's picture depends on its step alone, so any frame must
//       equal the static switch's (step on) or the unstepped (step off) picture, whatever the frames before it did.
//   bg  a 64x64 texture with seven authored mip levels, each a different constant, drawn at 1.5 texels per pixel (LOD 0.58) by two
//       quads of 32x32 pixels: one with an ordinary texture, one whose id the front end's IsCharaTexture names (a character).
//       Trilinear blends levels 0 and 1; the cheaper background takes level 1 alone, for the ordinary texture only.
// Wrappers observe calls but always forward them to Mesa, including readback.
#include "vgx_cmd.h"

#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
// The front end's list of character and key-item textures (the HD pack's selection): texture 31 in this fixture.
bool IsCharaTexture(uint32_t id) { return id == 31; }
}
namespace {
FILE* traceFile = nullptr;
unsigned draws = 0, uniforms = 0, targets = 0, copies = 0, reads = 0, finishes = 0;
const int effectDivisor = 2;
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
    if (traceFile && w == 64 / effectDivisor && h == 64 / effectDivisor) ++reducedViewports;
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

struct Frame { bool fx, bg; };
Frame StepOf(char c) {
    Require(c == '-' || c == 'f' || c == 'b' || c == 'x', "schedule letters are - f b x");
    return Frame{c == 'f' || c == 'x', c == 'b' || c == 'x'};
}
FILE* Open(const std::string& name) {
    FILE* f = std::fopen(name.c_str(), "wb");
    Require(f != nullptr, "open comparison artifact");
    return f;
}
void Setup(const ShaderSource& vs, const ShaderSource& fs) {
    CmdBuf setup;
    setup.frame = 1;
    setup.targets.push_back(SetTargetCmd{640, 456});
    Push(setup, CMD_SET_TARGET);
    setup.shaders = {&vs, &fs};
    Push(setup, CMD_SHADER, 0);
    Push(setup, CMD_SHADER, 1);
    Present(setup, true);
    backend::Execute(setup);
}

// independent: every frame draws the first frame's scene and clears afterwards, so a frame's picture depends on its step alone and
// not on what the frames before it left in the target; otherwise the fxscan frames, each building on the one before.
void RunFx(const std::string& prefix, const std::string& schedule, bool independent) {
    ShaderSource vs, fs;
    vs.id = fs.id = 1;
    vs.attrMask = 1u << AT_POS;
    vs.text = "attribute vec4 a_pos; uniform mat4 u_proj; void main() { gl_Position = u_proj * vec4(a_pos.xyz, 1.0); }";
    fs.fragment = true;
    fs.nTev = 1;
    fs.text = "precision mediump float; uniform vec4 u_tev[1]; void main() { gl_FragColor = u_tev[0]; }";
    Setup(vs, fs);
    const bool staticFx = std::getenv("GXV_FXHALF") && std::atoi(std::getenv("GXV_FXHALF")) == 2;
    for (unsigned frame = 2; frame <= 4; ++frame) {
        const Frame step = StepOf(schedule[frame - 2]);
        const unsigned scene = independent ? 2 : frame;
        backend::DebugSetQualitySteps(step.fx, step.bg);
        if (frame == 2) Record("first scheduled frame", {}); // (what a step builds ahead of itself comes before the marker)
        const unsigned reducedBefore = reducedViewports;
        CmdBuf b;
        b.frame = frame;
        Geometry(b);
        if (scene == 2) {
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
        } else if (scene == 3) {
            AddDraw(b, false, 0, 0.125f, 0);
            AddDraw(b, false, 0, 0.125f, 0);
        } else {
            AddDraw(b, false, 0.5f, 0.25f, 0, false);
        }
        Copy(b, 21);
        Present(b, independent);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        Require(backend::GetStats().drawCallsThisFrame == (scene == 2 ? 4u : (scene == 3 ? 2u : 1u)), "actual DrawRec dispatch count");
        // Frames 2 and 3 carry eligible runs; frame 4 has none (an opaque draw).
        const unsigned reduced = reducedViewports - reducedBefore;
        const bool half = staticFx || step.fx;
        Require(half ? (scene == 4 ? reduced == 0 : reduced > 0) : reduced == 0,
                half ? "a half-size frame draws its eligible runs into reduced targets" : "a full-size frame draws no reduced target");
        int w = 0, h = 0;
        const auto& pixels = backend::LastDisplayCopy(w, h);
        CheckRegions(pixels, w, h, scene == 2 ? 0 : 64);
        std::vector<uint8_t> copy;
        Require(backend::DebugReadTexture(21, copy, w, h), "real copied texture readback");
        CheckRegions(copy, w, h, scene == 2 ? 0 : 64);
        FILE* f = Open(prefix + ".f" + std::to_string(frame) + ".rgba");
        Save(f, pixels, w, h);
        Save(f, copy, w, h);
        Require(std::fclose(f) == 0, "close frame artifact");
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
        std::printf("frame %u: step %c, reduced viewports %u\n", frame, schedule[frame - 2], reduced);
    }
}

// ---- the cheaper background
constexpr unsigned kLevelValue[7] = {20, 60, 100, 140, 180, 220, 250};
void AddTextured(CmdBuf& b, bool right, uint32_t tex) {
    // 4 vertices of 24 bytes (position, texture coordinate) per quad: 32x32 pixels, texels 0..0.75 of the 64x64 texture.
    const float x0 = right ? 0.0f : -1.0f, x1 = x0 + 1.0f;
    const float v[] = {x0, -1, 0, 0, 0, 0,  x1, -1, 0, 0, 0.75f, 0,  x0, 0, 0, 0, 0, 0.75f,  x1, 0, 0, 0, 0.75f, 0.75f};
    const uint32_t first = static_cast<uint32_t>(b.vtx.size() / 24);
    const size_t at = b.vtx.size();
    b.vtx.resize(at + sizeof v);
    std::memcpy(b.vtx.data() + at, v, sizeof v);
    DrawRec d;
    d.vsId = 2;
    d.fsId = 2;
    d.layoutMask = VL_TEX0;
    d.texMask = 1;
    d.tex[0].texId = tex;
    d.tex[0].minFilter = FILT_LIN_MIP_LIN;
    d.tex[0].magFilter = FILT_LINEAR;
    d.vtxOffset = 0;
    d.vtxFirst = 0;
    d.vtxCount = 4;
    d.idxOffset = static_cast<uint32_t>(b.idx.size());
    d.idxCount = 6;
    for (uint16_t i : {0, 1, 2, 2, 1, 3}) b.idx.push_back(static_cast<uint16_t>(first + i));
    d.ublock[UB_PROJ] = 0;
    d.rs.scX = 0;
    d.rs.scW = 640;
    d.rs.scH = 456;
    d.rs.vpH = 456;
    b.draws.push_back(d);
    Push(b, CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1));
}
unsigned Probe(const std::vector<uint8_t>& px, int w, int x) { // the textured quad is the bottom or the top half of its column
    unsigned best = 0;
    for (int y : {16, 48}) best = std::max<unsigned>(best, px[(static_cast<size_t>(y) * w + x) * 4]);
    return best;
}
bool Near(double got, double want, double tol) { return got >= want - tol && got <= want + tol; }
void RunBg(const std::string& prefix, const std::string& schedule) {
    ShaderSource vs, fs;
    vs.id = fs.id = 2;
    vs.attrMask = (1u << AT_POS) | (1u << AT_TEX0);
    vs.text = "attribute vec4 a_pos; attribute vec2 a_tex0; uniform mat4 u_proj; varying vec2 v_t;"
              " void main() { gl_Position = u_proj * vec4(a_pos.xyz, 1.0); v_t = a_tex0; }";
    fs.fragment = true;
    fs.texMask = 1;
    fs.text = "precision mediump float; uniform sampler2D u_tex0; varying vec2 v_t; void main() { gl_FragColor = texture2D(u_tex0, v_t); }";
    Setup(vs, fs);
    for (unsigned frame = 2; frame <= 4; ++frame) {
        const Frame step = StepOf(schedule[frame - 2]);
        backend::DebugSetQualitySteps(step.fx, step.bg);
        CmdBuf b;
        b.frame = frame;
        b.uni.assign(16, 0);
        for (unsigned i = 0; i < 4; ++i) b.uni[i * 5] = 1;
        if (frame == 2) // (the textures are uploaded once, in the first frame)
            for (uint32_t id : {30u, 31u}) {
                TexUpload up;
                up.texId = id;
                up.width = up.height = 64;
                up.authored = 7;
                for (unsigned l = 0, size = 64; l < 7; ++l, size /= 2) {
                    std::vector<uint8_t> level(static_cast<size_t>(size) * size * 4, static_cast<uint8_t>(kLevelValue[l]));
                    for (size_t a = 3; a < level.size(); a += 4) level[a] = 255;
                    up.levels.push_back(std::move(level));
                }
                b.uploads.push_back(up);
                Push(b, CMD_TEX_UPLOAD, static_cast<uint32_t>(b.uploads.size() - 1));
            }
        AddTextured(b, false, 30); // an ordinary texture
        AddTextured(b, true, 31);  // a character's
        Present(b);
        backend::RequestCapture(true);
        backend::Execute(b);
        backend::RequestCapture(false);
        Require(backend::GetStats().drawCallsThisFrame == 2u, "actual DrawRec dispatch count");
        int w = 0, h = 0;
        const auto& px = backend::LastDisplayCopy(w, h);
        Require(w == 64 && h == 64, "requested render dimensions");
        const unsigned left = Probe(px, w, 16), right = Probe(px, w, 48);
        // 1.5 texels per pixel: LOD 0.585. Trilinear: level 0 + 0.585 of the step to level 1; nearest level: level 1.
        const double trilinear = kLevelValue[0] + (kLevelValue[1] - kLevelValue[0]) * 0.585, nearest = kLevelValue[1];
        std::printf("frame %u: step %c, ordinary %u, character %u (trilinear %.1f, nearest level %.0f)\n", frame, schedule[frame - 2], left, right, trilinear, nearest);
        Require(Near(right, trilinear, 6), "the character's texture keeps its trilinear filter at every step");
        if (step.bg) Require(Near(left, nearest, 3), "the cheaper background samples the nearest mip level");
        else Require(Near(left, trilinear, 6), "no step: the ordinary texture is trilinear");
        FILE* f = Open(prefix + ".f" + std::to_string(frame) + ".rgba");
        Save(f, px, w, h);
        Require(std::fclose(f) == 0, "close frame artifact");
        Require(glGetError() == GL_NO_ERROR, "no GL errors");
    }
}
} // namespace

int main(int argc, char** argv) {
    Require(argc == 4, "usage: <fx|fxi|bg> <output prefix> <schedule of three letters>");
    const std::string what = argv[1], prefix = argv[2], schedule = argv[3];
    Require((what == "fx" || what == "fxi" || what == "bg") && schedule.size() == 3, "mode and schedule");
    Require(host::CreateContext(), "Mesa EGL context");
    Require(backend::Init(64, 64), "real backend initialization");
    traceFile = Open(prefix + ".trace");
    if (what == "bg") RunBg(prefix, schedule); else RunFx(prefix, schedule, what == "fxi");
    glFinish();
    std::printf("PASS: %s %s: indexed draws=%u uniforms=%u targets=%u copy operations=%u reads=%u finishes=%u\n", what.c_str(), schedule.c_str(),
                draws, uniforms, targets, copies, reads, finishes);
    Require(std::fclose(traceFile) == 0, "close the trace");
    traceFile = nullptr;
    return 0;
}
