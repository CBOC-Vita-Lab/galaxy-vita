// Differential test against the existing real GLES XF vertex shader.
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "cpu_xf.h"
#include "gx_record.h"

static void require(bool valid, const char* message) {
    if (!valid) { std::fprintf(stderr, "XF differential FAILED: %s (GL 0x%x)\n", message, glGetError()); std::exit(1); }
}

static GLuint shader(GLenum kind, const std::string& source) {
    GLuint result = glCreateShader(kind);
    const char* text = source.c_str();
    glShaderSource(result, 1, &text, nullptr);
    glCompileShader(result);
    GLint valid;
    glGetShaderiv(result, GL_COMPILE_STATUS, &valid);
    if (!valid) {
        char log[4096];
        glGetShaderInfoLog(result, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n%s\n", log, text);
    }
    require(valid, "compile reference shader");
    return result;
}

static GLuint buffer(GLenum kind, const void* source, size_t size, GLuint binding) {
    GLuint result;
    glGenBuffers(1, &result);
    glBindBuffer(kind, result);
    glBufferData(kind, size, source, GL_DYNAMIC_DRAW);
    if (kind != GL_ARRAY_BUFFER) glBindBufferBase(kind, binding, result);
    return result;
}

static void context() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    require(eglInitialize(display, nullptr, nullptr), "EGL initialize");
    const EGLint attributes[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count;
    require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    const EGLint version[]{EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    const EGLint dimensions[]{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, dimensions);
    require(ctx != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, ctx), "GLES 3.2 context");
    std::printf("XF reference: %s / %s; not Vita GPU evidence\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
}

static gpu::ResolvedXf state() {
    gpu::ResolvedXf s;
    for (int row = 0; row < 66; ++row) {
        s.pos[row][row % 3] = 1 + float(row / 3) * 0.02f;
        s.pos[row][3] = float(row / 3) * 0.03f;
    }
    for (int row = 0; row < 33; ++row) s.normal[row][row % 3] = 1 + float(row / 3) * 0.05f;
    for (int row = 0; row < 24; ++row) {
        s.post[row][row % 3] = 0.75f + float(row / 3) * 0.03f;
        s.post[row][3] = 0.12f;
    }
    s.projection.proj[0][0] = 0.3f;
    s.projection.proj[1][1] = 0.4f;
    s.projection.proj[2][2] = 0.7f;
    s.projection.proj[2][3] = 0.2f;
    s.projection.proj[3][2] = -0.1f;
    s.projection.proj[3][3] = 1.1f;
    s.projection.depthParams[0] = 8000000;
    s.projection.depthParams[1] = 9000000;
    for (int light = 0; light < 8; ++light) {
        auto& l = s.lights.lights[light];
        for (int i = 0; i < 4; ++i) l[0][i] = 0.03f * float(light + 1);
        l[1][0] = 0.8f; l[1][1] = 0.1f; l[1][2] = 0.05f;
        l[2][0] = 1.2f; l[2][1] = 0.01f; l[2][2] = 0.002f;
        l[3][0] = 4 + light; l[3][1] = 6 - light; l[3][2] = 3 + light;
        l[4][0] = 0.2f; l[4][1] = 0.4f; l[4][2] = 0.8f;
    }
    for (int channel = 0; channel < 2; ++channel) for (int i = 0; i < 4; ++i) {
        s.lights.matColor[channel][i] = 0.3f + float(i) * 0.13f;
        s.lights.ambColor[channel][i] = 0.1f + float(i) * 0.02f;
    }
    s.matrixA = 6;
    for (int i = 0; i < 4; ++i) s.matrixA |= uint32_t(3 * (i + 2)) << (6 * (i + 1));
    for (int i = 0; i < 4; ++i) s.matrixB |= uint32_t(3 * (i + 6)) << (6 * i);
    return s;
}

static std::vector<uint32_t> vertices(uint32_t flags, size_t count) {
    std::vector<uint32_t> words;
    std::mt19937 generator(0x58464);
    auto scalar = [&] { return float(generator() % 2001) / 1000 - 1; };
    auto append = [&](float value) { uint32_t word; std::memcpy(&word, &value, 4); words.push_back(word); };
    for (size_t v = 0; v < count; ++v) {
        for (int i = 0; i < 3; ++i) append(scalar());
        if (flags & gpu::VF_POSMTX) words.push_back(uint32_t(v % 11) * 3);
        if (flags & gpu::VF_NRM) for (int i = 0; i < 3; ++i) append(scalar());
        if (flags & gpu::VF_NBT) for (int i = 0; i < 6; ++i) append(scalar());
        if (flags & gpu::VF_CLR0) words.push_back(generator());
        if (flags & gpu::VF_CLR1) words.push_back(generator());
        for (int i = 0; i < 8; ++i) if (flags & (gpu::VF_TEX0 << i)) { append(scalar()); append(scalar()); }
        if (flags & gpu::VF_TEXMTX) {
            for (int group = 0; group < 2; ++group) {
                uint32_t packed = 0;
                for (int i = 0; i < 4; ++i) packed |= uint32_t(3 * ((v + group * 4 + i) % 22)) << (i * 8);
                words.push_back(packed);
            }
        }
    }
    return words;
}

static size_t checked = 0;
static float maxError = 0;

static void fixture(gpu::ShaderUid uid, const gpu::ResolvedXf& s, size_t count) {
    GLuint vs = shader(GL_VERTEX_SHADER, gpu::genVertexShader(uid));
    GLuint fs = shader(GL_FRAGMENT_SHADER, "#version 320 es\nprecision highp float;\nout vec4 color;\nvoid main(){color=vec4(1.0);}\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs);
    std::vector<std::string> names{"gl_Position", "v_clr0", "v_clr1"};
    for (uint32_t i = 0; i < uid.numTexGens; ++i) names.push_back("v_tex" + std::to_string(i));
    names.push_back("v_viewz"); names.push_back("v_gxz");
    std::vector<const char*> outputs;
    for (const auto& name : names) outputs.push_back(name.c_str());
    glTransformFeedbackVaryings(program, outputs.size(), outputs.data(), GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) { char log[4096]; glGetProgramInfoLog(program, sizeof(log), nullptr, log); std::fprintf(stderr, "%s\n", log); }
    require(linked, "link reference transform feedback");
    glUseProgram(program);
    glUniform1ui(glGetUniformLocation(program, "u_firstOrdinal"), 0);
    uint32_t rec[gpu::kRecWords]{};
    for (int i = 0; i < 22; ++i) rec[gpu::kRecPosSlots + i] = i * 3;
    for (int i = 0; i < 11; ++i) rec[gpu::kRecNrmSlots + i] = 66 + i * 3;
    for (int i = 0; i < 8; ++i) rec[gpu::kRecPost + i] = 99 + i * 3;
    rec[gpu::kRecLights] = 0; rec[gpu::kRecProj] = 44; rec[gpu::kRecPixel] = 49;
    rec[gpu::kRecMtxIdxA] = s.matrixA; rec[gpu::kRecMtxIdxB] = s.matrixB; rec[gpu::kRecFlags] = s.flags;
    std::vector<float> rows;
    for (const auto& row : s.pos) rows.insert(rows.end(), row, row + 4);
    for (const auto& row : s.normal) rows.insert(rows.end(), row, row + 4);
    for (const auto& row : s.post) rows.insert(rows.end(), row, row + 4);
    std::vector<float> states(76 * 4);
    std::memcpy(states.data(), &s.lights, sizeof(s.lights));
    std::memcpy(states.data() + 44 * 4, &s.projection, sizeof(s.projection));
    std::vector<uint32_t> records(count, 0);
    float eye[56]{};
    std::vector<GLuint> buffers;
    buffers.push_back(buffer(GL_SHADER_STORAGE_BUFFER, rows.data(), rows.size() * 4, 0));
    buffers.push_back(buffer(GL_SHADER_STORAGE_BUFFER, rec, sizeof(rec), 1));
    buffers.push_back(buffer(GL_SHADER_STORAGE_BUFFER, states.data(), states.size() * 4, 2));
    buffers.push_back(buffer(GL_SHADER_STORAGE_BUFFER, records.data(), records.size() * 4, 3));
    buffers.push_back(buffer(GL_UNIFORM_BUFFER, eye, sizeof(eye), 2));
    const auto words = vertices(uid.vtxFlags, count);
    buffers.push_back(buffer(GL_ARRAY_BUFFER, words.data(), words.size() * 4, 0));
    GLuint vao;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    size_t offset = 0;
    auto attribute = [&](GLuint location, GLint components, GLenum type, bool normalized, bool integer, size_t bytes) {
        glEnableVertexAttribArray(location);
        const void* pointer = reinterpret_cast<const void*>(offset);
        const GLsizei stride = gpu::vtxWords(uid.vtxFlags) * 4;
        if (integer) glVertexAttribIPointer(location, components, type, stride, pointer);
        else glVertexAttribPointer(location, components, type, normalized, stride, pointer);
        offset += bytes;
    };
    attribute(0, 3, GL_FLOAT, false, false, 12);
    const uint32_t flags = uid.vtxFlags;
    if (flags & gpu::VF_POSMTX) attribute(1, 1, GL_UNSIGNED_INT, false, true, 4);
    if (flags & gpu::VF_NRM) attribute(2, 3, GL_FLOAT, false, false, 12);
    if (flags & gpu::VF_NBT) { attribute(3, 3, GL_FLOAT, false, false, 12); attribute(4, 3, GL_FLOAT, false, false, 12); }
    if (flags & gpu::VF_CLR0) attribute(5, 4, GL_UNSIGNED_BYTE, true, false, 4);
    if (flags & gpu::VF_CLR1) attribute(6, 4, GL_UNSIGNED_BYTE, true, false, 4);
    for (int i = 0; i < 8; ++i) if (flags & (gpu::VF_TEX0 << i)) attribute(7 + i, 2, GL_FLOAT, false, false, 8);
    if (flags & gpu::VF_TEXMTX) attribute(15, 2, GL_UNSIGNED_INT, false, true, 8);
    require(offset == gpu::vtxWords(flags) * 4, "decoded vertex stride");
    const size_t outputWords = 14 + uid.numTexGens * 3;
    buffers.push_back(buffer(GL_TRANSFORM_FEEDBACK_BUFFER, nullptr, count * outputWords * 4, 0));
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS); glDrawArrays(GL_POINTS, 0, count); glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    require(glGetError() == GL_NO_ERROR, "reference feedback draw");
    const float* reference = static_cast<const float*>(glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0, count * outputWords * 4, GL_MAP_READ_BIT));
    require(reference, "map GPU XF values");
    for (size_t vertex = 0; vertex < count; ++vertex) {
        gpu::ClipVertex actual;
        const char* error = nullptr;
        const bool valid = gpu::transformVertexCpu(uid, s, words.data() + vertex * gpu::vtxWords(flags), gpu::vtxWords(flags), actual, &error);
        require(valid, error ? error : "CPU XF rejected valid vertex");
        std::vector<float> values(actual.clip, actual.clip + 4);
        for (const auto& color : actual.color) values.insert(values.end(), color, color + 4);
        for (uint32_t i = 0; i < uid.numTexGens; ++i) values.insert(values.end(), actual.tex[i], actual.tex[i] + 3);
        values.push_back(actual.viewZ); values.push_back(actual.gxZ);
        for (size_t component = 0; component < outputWords; ++component) {
            const float expected = reference[vertex * outputWords + component], got = values[component];
            const float difference = std::abs(expected - got);
            maxError = std::max(maxError, difference);
            if (!std::isfinite(expected) || !std::isfinite(got) || difference > 0.00003f * std::max(1.0f, std::abs(expected))) {
                std::fprintf(stderr, "flags=%x texgens=%u vertex=%zu component=%zu GPU=%g CPU=%g\n", flags, uid.numTexGens, vertex, component, expected, got);
                require(false, "CPU/reference mismatch");
            }
            ++checked;
        }
    }
    glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    glDeleteBuffers(buffers.size(), buffers.data()); glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
}

int main() {
    context();
    gpu::ResolvedXf s = state();
    gpu::ShaderUid u{};
    fixture(u, s, 128);
    u.vtxFlags = gpu::VF_NRM | gpu::VF_CLR0 | gpu::VF_CLR1 | gpu::VF_POSMTX | gpu::VF_TEX0 | (gpu::VF_TEX0 << 1);
    u.numChans = 2; u.numTexGens = 2;
    u.colorCtrl[0] = 1; u.alphaCtrl[1] = 1;
    u.texMtxInfo[0] = 5 << 7; u.texMtxInfo[1] = (6 << 7) | 6;
    fixture(u, s, 128);
    u.vtxFlags = 0x3FFF; u.numTexGens = 8; u.dualTex = 1;
    for (int i = 0; i < 8; ++i) {
        u.texMtxInfo[i] = uint32_t(i + 5) << 7 | (i & 1 ? 6 : 0);
        u.postMtxInfo[i] = i & 1 ? 256 : 0;
    }
    fixture(u, s, 128);
    for (int i = 0; i < 8; ++i) {
        u.texMtxInfo[i] = uint32_t(i % 5) << 7 | 6;
        if (i == 3 || i == 4) u.texMtxInfo[i] = uint32_t(i - 1) << 4;
        if (i == 6) u.texMtxInfo[i] = (1 << 4) | (1 << 12) | (7 << 15);
        if (i == 7) u.texMtxInfo[i] = (1 << 4) | (7 << 12);
    }
    fixture(u, s, 128);
    for (int channel = 0; channel < 2; ++channel) {
        const uint32_t allLights = (15 << 2) | (15 << 11);
        u.colorCtrl[channel] = allLights | 2 | (channel << 7) | (1 << 9) | (channel << 10);
        u.alphaCtrl[channel] = allLights | 3 | 64 | (2 << 7);
    }
    fixture(u, s, 128);
    u.vtxFlags &= ~(gpu::VF_POSMTX | gpu::VF_TEXMTX);
    fixture(u, s, 128);
    auto invalid = vertices(u.vtxFlags, 1);
    gpu::ClipVertex rejected;
    const char* error = nullptr;
    require(!gpu::transformVertexCpu(u, s, invalid.data(), invalid.size() - 1, rejected, &error) && error, "reject truncated input");
    u.numTexGens = 9;
    require(!gpu::transformVertexCpu(u, s, invalid.data(), invalid.size(), rejected, &error), "reject excess texgens");
    std::printf("XF differential passed: 768 vertices, %zu float components, max absolute error %.9g; full lighting/NBT/post/emboss/default matrices\n", checked, maxError);
}
