/* A GL that does nothing: <GLES2/gl2.h> for the armhf harness with the real GL back end
 * (cmake -DARMHF_VGX_BACKEND=gl-stub, tools/pgo/README.md).
 * The real GL back end (vgx_backend_gl.cpp) is compiled against this header, so everything
 * it does up to the GL call itself runs: state caching, uniform and attribute de-duplication,
 * target pooling, the per-frame count of GL calls by kind. Used for
 *   - instruction counts of the back end's own work on the armhf build under qemu,
 *   - recording a profile of the back end's own code (the GL calls themselves are the driver's, not profiled).
 * It answers like the target driver where the back end asks something: depth24 but no depth
 * textures (vitaGL's renderbuffer path), S3TC present, every shader compiles and links.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* The stub's functions stay calls, as the driver's are: inlined, their bodies (and the loops among them) would change the control
 * flow of the back end's functions that a profile is keyed on, and the back end's code would be optimised for a GL that is not there. */
#define VGX_STUB_FN static __attribute__((noinline, unused))

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat;
typedef char GLchar;
typedef void GLvoid;
typedef ptrdiff_t GLsizeiptr, GLintptr;

enum {
    GL_FALSE = 0, GL_TRUE = 1, GL_NO_ERROR = 0, GL_ZERO = 0, GL_ONE = 1,
    GL_POINTS = 0, GL_LINES = 1, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5,
    GL_DEPTH_BUFFER_BIT = 0x100, GL_COLOR_BUFFER_BIT = 0x4000,
    GL_NEVER = 0x200, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS,
    GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408, GL_CW = 0x900, GL_CCW = 0x901,
    GL_CULL_FACE = 0xB44, GL_DEPTH_TEST = 0xB71, GL_BLEND = 0xBE2, GL_DITHER = 0xBD0, GL_SCISSOR_TEST = 0xC11,
    GL_UNPACK_ALIGNMENT = 0xCF5, GL_PACK_ALIGNMENT = 0xD05, GL_TEXTURE_2D = 0xDE1,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
    GL_DEPTH_COMPONENT = 0x1902, GL_RGBA = 0x1908, GL_LUMINANCE_ALPHA = 0x190A,
    GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
    GL_LINEAR_MIPMAP_LINEAR,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_REPEAT = 0x2901,
    GL_FUNC_ADD = 0x8006, GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_CLAMP_TO_EDGE = 0x812F, GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_DEPTH_COMPONENT24_OES = 0x81A6, GL_MIRRORED_REPEAT = 0x8370, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT = 0x83F1, GL_TEXTURE0 = 0x84C0,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_STREAM_DRAW = 0x88E0, GL_STATIC_DRAW = 0x88E4, GL_DYNAMIC_DRAW = 0x88E8,
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_ACTIVE_UNIFORMS = 0x8B86,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00, GL_FRAMEBUFFER = 0x8D40,
    GL_RENDERBUFFER = 0x8D41,
};

/* The functions that answer with a value are the driver's own symbols too (armhf/glue/vgl_stub.cpp): defined in this file, the compiler
 * would learn things about their results (that they never are undefined, a constant) which it cannot about the driver's, and
 * the back end's branches on them would differ from the console build's. */
extern "C" {
GLuint glCreateShader(GLenum);
GLuint glCreateProgram(void);
GLint glGetUniformLocation(GLuint, const GLchar*);
GLenum glCheckFramebufferStatus(GLenum);
GLenum glGetError(void);
const GLubyte* glGetString(GLenum);
extern unsigned vgx_stub_name_;
}

/* Memory census: what the back end has asked the driver to hold, by kind, in bytes as vitaGL
 * would allocate them (RGBA8 and depth 4 bytes per pixel, a mip chain = 4/3 of level 0 whether
 * it was uploaded or generated, compressed levels at their size, buffers at their size).
 * Read with vgx_stub_mem(). */
enum { VGX_STUB_TEX_RGBA = 1, VGX_STUB_TEX_DXT, VGX_STUB_RT_COLOR, VGX_STUB_RT_DEPTH, VGX_STUB_RB, VGX_STUB_BUF_STATIC, VGX_STUB_BUF_STREAM, VGX_STUB_BUF_DYNAMIC, VGX_STUB_TEX_LA, VGX_STUB_KINDS };
#define VGX_STUB_MAXN (1u << 18)
static uint32_t vgx_stub_bytes_[VGX_STUB_MAXN], vgx_stub_base_[VGX_STUB_MAXN];
static uint8_t vgx_stub_kind_[VGX_STUB_MAXN];
static uint64_t vgx_stub_total_[VGX_STUB_KINDS];
static uint32_t vgx_stub_count_[VGX_STUB_KINDS];
static unsigned vgx_stub_curTex_ = 0, vgx_stub_curRb_ = 0, vgx_stub_curBuf_[2] = {0, 0};
VGX_STUB_FN void vgx_stub_set(unsigned name, int kind, uint64_t bytes) {
    if (name == 0 || name >= VGX_STUB_MAXN) return;
    if (vgx_stub_kind_[name]) { vgx_stub_total_[vgx_stub_kind_[name]] -= vgx_stub_bytes_[name]; --vgx_stub_count_[vgx_stub_kind_[name]]; }
    vgx_stub_kind_[name] = (uint8_t)kind;
    vgx_stub_bytes_[name] = (uint32_t)bytes;
    if (kind) { vgx_stub_total_[kind] += bytes; ++vgx_stub_count_[kind]; }
}
VGX_STUB_FN void vgx_stub_mem(uint64_t bytes[VGX_STUB_KINDS], uint32_t count[VGX_STUB_KINDS]) {
    for (int i = 0; i < VGX_STUB_KINDS; ++i) { bytes[i] = vgx_stub_total_[i]; count[i] = vgx_stub_count_[i]; }
}
#define VGX_STUB_GEN(fn) VGX_STUB_FN void fn(GLsizei n, GLuint* ids) { for (GLsizei i = 0; i < n; ++i) ids[i] = ++vgx_stub_name_; }
VGX_STUB_GEN(glGenFramebuffers) VGX_STUB_GEN(glGenTextures) VGX_STUB_GEN(glGenRenderbuffers) VGX_STUB_GEN(glGenBuffers)
#undef VGX_STUB_GEN
/* What the back end asks a linked program about itself: its uniforms (name and array size), taken from the shaders'
 * "uniform <type> <name>[<n>];" lines, the way a driver reports them; the stub has no other use for the source. */
static std::unordered_map<GLuint, std::string> vgx_stub_src_;
static std::unordered_map<GLuint, std::vector<GLuint>> vgx_stub_att_;
struct VgxStubUniform { std::string name; GLint size; };
static std::unordered_map<GLuint, std::vector<VgxStubUniform>> vgx_stub_uni_;
VGX_STUB_FN void glShaderSource(GLuint s, GLsizei n, const GLchar* const* str, const GLint* len) {
    std::string& d = vgx_stub_src_[s];
    d.clear();
    for (GLsizei i = 0; i < n; ++i) d.append(str[i], len && len[i] >= 0 ? static_cast<size_t>(len[i]) : std::strlen(str[i]));
}
VGX_STUB_FN void glCompileShader(GLuint) {}
VGX_STUB_FN void glGetShaderiv(GLuint, GLenum, GLint* p) { *p = 1; }
VGX_STUB_FN void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar* log) { log[0] = 0; }
VGX_STUB_FN void glAttachShader(GLuint p, GLuint s) { vgx_stub_att_[p].push_back(s); }
VGX_STUB_FN void glBindAttribLocation(GLuint, GLuint, const GLchar*) {}
VGX_STUB_FN void glDeleteProgram(GLuint) {}
VGX_STUB_FN void glDeleteShader(GLuint) {}
VGX_STUB_FN void glLinkProgram(GLuint) {}
VGX_STUB_FN void glGetProgramiv(GLuint prog, GLenum pname, GLint* p) {
    if (pname != GL_ACTIVE_UNIFORMS) { *p = 1; return; }
    std::vector<VgxStubUniform>& u = vgx_stub_uni_[prog];
    u.clear();
    for (GLuint s : vgx_stub_att_[prog]) {
        const std::string& src = vgx_stub_src_[s];
        for (size_t at = src.find("uniform "); at != std::string::npos; at = src.find("uniform ", at + 8)) {
            const size_t end = src.find(';', at), sp = src.find(' ', at + 8);
            if (end == std::string::npos || sp == std::string::npos || sp > end) break;
            std::string name = src.substr(sp + 1, end - sp - 1);
            GLint size = 1;
            const size_t br = name.find('[');
            if (br != std::string::npos) { size = std::atoi(name.c_str() + br + 1); name.resize(br); }
            u.push_back({name, size});
        }
    }
    *p = static_cast<GLint>(u.size());
}
VGX_STUB_FN void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar* log) { log[0] = 0; }
VGX_STUB_FN void glGetActiveUniform(GLuint prog, GLuint i, GLsizei cap, GLsizei* len, GLint* size, GLenum* type, GLchar* name) {
    const VgxStubUniform& u = vgx_stub_uni_[prog][i];
    std::snprintf(name, static_cast<size_t>(cap), "%s%s", u.name.c_str(), u.size > 1 ? "[0]" : "");
    *size = u.size;
    *type = 0x8B52;
    if (len) *len = static_cast<GLsizei>(std::strlen(name));
}
VGX_STUB_FN void glUseProgram(GLuint) {}
VGX_STUB_FN void glUniform1i(GLint, GLint) {}
VGX_STUB_FN void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat) {}
VGX_STUB_FN void glUniform4fv(GLint, GLsizei, const GLfloat*) {}
VGX_STUB_FN void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*) {}
VGX_STUB_FN void glBindTexture(GLenum, GLuint name) { vgx_stub_curTex_ = name; }
VGX_STUB_FN void glActiveTexture(GLenum) {}
VGX_STUB_FN void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum, const GLvoid* data) {
    const unsigned t = vgx_stub_curTex_;
    if (t == 0 || t >= VGX_STUB_MAXN) return;
    if (level == 0) {
        vgx_stub_base_[t] = (uint32_t)w * (uint32_t)h * (format == GL_LUMINANCE_ALPHA ? 2u : 4u);
        vgx_stub_set(t, format == GL_DEPTH_COMPONENT ? VGX_STUB_RT_DEPTH : data ? (format == GL_LUMINANCE_ALPHA ? VGX_STUB_TEX_LA : VGX_STUB_TEX_RGBA) : VGX_STUB_RT_COLOR, vgx_stub_base_[t]);
    } else {
        vgx_stub_set(t, vgx_stub_kind_[t], (uint64_t)vgx_stub_base_[t] * 4u / 3u);
    }
}
VGX_STUB_FN void glCompressedTexImage2D(GLenum, GLint level, GLenum, GLsizei, GLsizei, GLint, GLsizei size, const GLvoid*) {
    const unsigned t = vgx_stub_curTex_;
    if (t == 0 || t >= VGX_STUB_MAXN) return;
    vgx_stub_set(t, VGX_STUB_TEX_DXT, (level == 0 ? 0u : vgx_stub_bytes_[t]) + (uint64_t)size);
}
VGX_STUB_FN void glGenerateMipmap(GLenum) {
    const unsigned t = vgx_stub_curTex_;
    if (t && t < VGX_STUB_MAXN && vgx_stub_kind_[t]) vgx_stub_set(t, vgx_stub_kind_[t], (uint64_t)vgx_stub_base_[t] * 4u / 3u);
}
VGX_STUB_FN void glTexParameteri(GLenum, GLenum, GLint) {}
VGX_STUB_FN void glDeleteTextures(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) vgx_stub_set(ids[i], 0, 0); }
static GLint vgx_stub_unpack_ = 4;
VGX_STUB_FN void glPixelStorei(GLenum name, GLint value) { if (name == GL_UNPACK_ALIGNMENT) vgx_stub_unpack_ = value; }
VGX_STUB_FN void glGetIntegerv(GLenum name, GLint* value) { *value = name == GL_UNPACK_ALIGNMENT ? vgx_stub_unpack_ : 0; }
VGX_STUB_FN void glBindFramebuffer(GLenum, GLuint) {}
VGX_STUB_FN void glDeleteFramebuffers(GLsizei, const GLuint*) {}
VGX_STUB_FN void glDeleteRenderbuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) vgx_stub_set(ids[i], 0, 0); }
VGX_STUB_FN void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const GLvoid*) {}
VGX_STUB_FN void glGetRenderbufferParameteriv(GLenum, GLenum, GLint* p) { *p = 1; }
VGX_STUB_FN void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint) {}
VGX_STUB_FN void glBindRenderbuffer(GLenum, GLuint name) { vgx_stub_curRb_ = name; }
VGX_STUB_FN void glRenderbufferStorage(GLenum, GLenum, GLsizei w, GLsizei h) { vgx_stub_set(vgx_stub_curRb_, VGX_STUB_RB, (uint64_t)w * (uint64_t)h * 4u); }
VGX_STUB_FN void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
VGX_STUB_FN void glViewport(GLint, GLint, GLsizei, GLsizei) {}
VGX_STUB_FN void glScissor(GLint, GLint, GLsizei, GLsizei) {}
VGX_STUB_FN void glDepthRangef(GLfloat, GLfloat) {}
VGX_STUB_FN void glEnable(GLenum) {}
VGX_STUB_FN void glDisable(GLenum) {}
VGX_STUB_FN void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean) {}
VGX_STUB_FN void glDepthMask(GLboolean) {}
VGX_STUB_FN void glDepthFunc(GLenum) {}
VGX_STUB_FN void glBlendEquation(GLenum) {}
VGX_STUB_FN void glBlendFunc(GLenum, GLenum) {}
VGX_STUB_FN void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum) {}
VGX_STUB_FN void glCullFace(GLenum) {}
VGX_STUB_FN void glFrontFace(GLenum) {}
VGX_STUB_FN void glLineWidth(GLfloat) {}
VGX_STUB_FN void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat) {}
VGX_STUB_FN void glClearDepthf(GLfloat) {}
VGX_STUB_FN void glClear(GLbitfield) {}
VGX_STUB_FN void glBindBuffer(GLenum target, GLuint name) { vgx_stub_curBuf_[target == GL_ELEMENT_ARRAY_BUFFER] = name; }
VGX_STUB_FN void glBufferData(GLenum target, GLsizeiptr size, const GLvoid*, GLenum usage) {
    vgx_stub_set(vgx_stub_curBuf_[target == GL_ELEMENT_ARRAY_BUFFER], usage == GL_STREAM_DRAW ? VGX_STUB_BUF_STREAM : usage == GL_DYNAMIC_DRAW ? VGX_STUB_BUF_DYNAMIC : VGX_STUB_BUF_STATIC, (uint64_t)size);
}
VGX_STUB_FN void glDeleteBuffers(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) vgx_stub_set(ids[i], 0, 0); }
VGX_STUB_FN void glEnableVertexAttribArray(GLuint) {}
VGX_STUB_FN void glDisableVertexAttribArray(GLuint) {}
VGX_STUB_FN void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const GLvoid*) {}
VGX_STUB_FN void glVertexAttrib4f(GLuint, GLfloat, GLfloat, GLfloat, GLfloat) {}
VGX_STUB_FN void glDrawElements(GLenum, GLsizei, GLenum, const GLvoid*) {}
VGX_STUB_FN void glDrawArrays(GLenum, GLint, GLsizei) {}
VGX_STUB_FN void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*) {}
VGX_STUB_FN void glFinish(void) {}

#ifdef __VGX_VITA__
/* The harness compiles the back end as the console build does (-D__VGX_VITA__, so that a profile recorded here describes the same
 * control flow): what that code asks of vitaGL itself beyond GLES 2. The few vitaGL entry points the back end reaches
 * through weak declarations are in armhf/glue/vgl_stub.cpp. */
enum { VGL_MEM_VRAM = 0 };
extern "C" {
size_t vglMemFree(int);
size_t vglMemTotal(int);
const void* vglGetTexDataPointer(GLenum);
}
/* The console's GLES2/gl2.h wraps glDrawElements, glDrawArrays and glClear in the mask guard's test (platform/vgx/vita/include/GLES2/gl2.h):
 * two branches the back end's control flow contains, so they are here too.  The guard is off (GXV_VITA_MASK_GUARD), as by default. */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char vgxvita_maskGuardOn, vgxvita_maskGuardDue;
void vgxvita_maskGuard(void);
#ifdef __cplusplus
}
#endif
#define glDrawElements(mode, count, type, indices) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glDrawElements(mode, count, type, indices))
#define glDrawArrays(mode, first, count) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glDrawArrays(mode, first, count))
#define glClear(bits) (((vgxvita_maskGuardOn && vgxvita_maskGuardDue) ? vgxvita_maskGuard() : (void)0), glClear(bits))
#endif
