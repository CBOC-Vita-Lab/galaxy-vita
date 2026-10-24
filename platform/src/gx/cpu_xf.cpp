#include "cpu_xf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gx_record.h"

namespace gpu {
namespace {

float dot3(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

float dot4(const float* a, const float* b) {
    return dot3(a, b) + a[3] * b[3];
}

void normalize(float* v) {
    const float length = std::sqrt(dot3(v, v));
    if (length > 0) for (int i = 0; i < 3; ++i) v[i] /= length;
}

void multiply(const float (*rows)[4], const float* v, float* result) {
    for (int i = 0; i < 3; ++i) result[i] = dot4(rows[i], v);
}

struct Decoded {
    float position[4]{0, 0, 0, 1};
    float normal[3]{0, 0, 1}, binormal[3]{}, tangent[3]{};
    float color[2][4]{{1, 1, 1, 1}, {1, 1, 1, 1}};
    float tex[8][2]{};
    uint32_t positionMatrix = 0;
    uint8_t textureMatrices[8]{};
};

Decoded decode(const uint32_t* words, uint32_t flags) {
    Decoded d;
    size_t at = 0;
    auto floats = [&](float* destination, size_t count) {
        std::memcpy(destination, words + at, count * sizeof(float));
        at += count;
    };
    floats(d.position, 3);
    if (flags & VF_POSMTX) d.positionMatrix = words[at++];
    if (flags & VF_NRM) floats(d.normal, 3);
    if (flags & VF_NBT) {
        floats(d.binormal, 3);
        floats(d.tangent, 3);
    }
    for (int channel = 0; channel < 2; ++channel) {
        if (flags & (VF_CLR0 << channel)) {
            const uint32_t rgba = words[at++];
            for (int component = 0; component < 4; ++component)
                d.color[channel][component] = float((rgba >> (component * 8)) & 255) / 255.0f;
        }
    }
    for (int i = 0; i < 8; ++i) if (flags & (VF_TEX0 << i)) floats(d.tex[i], 2);
    if (flags & VF_TEXMTX) {
        for (int i = 0; i < 8; ++i) d.textureMatrices[i] = uint8_t(words[at + i / 4] >> ((i % 4) * 8));
    }
    return d;
}

bool fail(const char** error, const char* message) {
    if (error) *error = message;
    return false;
}

}  // namespace

std::string genCpuXfDiagnosticVertexShader(const ShaderUid& uid) {
    std::string source = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
        "layout(location=0) in vec4 a_clip;\nlayout(location=1) in vec4 a_clr0;\n"
        "layout(location=2) in vec4 a_clr1;\nlayout(location=11) in vec2 a_depth;\n"
        "layout(location=12) in uint a_pixel;\nflat out uint v_pix;\n"
        "out vec4 v_clr0;\nout vec4 v_clr1;\nout float v_viewz;\nout float v_gxz;\n";
    for (uint32_t i = 0; i < uid.numTexGens; ++i) {
        source += "layout(location=" + std::to_string(i + 3) + ") in vec3 a_tex" + std::to_string(i) + ";\n";
        source += "out vec3 v_tex" + std::to_string(i) + ";\n";
    }
    source += "void main(){gl_Position=a_clip;v_clr0=a_clr0;v_clr1=a_clr1;"
        "v_viewz=a_depth.x;v_gxz=a_depth.y;v_pix=a_pixel;\n";
    for (uint32_t i = 0; i < uid.numTexGens; ++i)
        source += "v_tex" + std::to_string(i) + "=a_tex" + std::to_string(i) + ";\n";
    return source + "}\n";
}

bool transformVertexCpu(const ShaderUid& uid, const ResolvedXf& state,
                        const uint32_t* words, size_t wordCount,
                        ClipVertex& output, const char** error) {
    output = {};
    if (error) *error = nullptr;
    if (uid.numTexGens > 8 || uid.numChans > 2)
        return fail(error, "XF channel or texgen count exceeds GX limits");
    if (uid.cutaway) return fail(error, "VR cutaway is not a flat-screen XF mode");
    const uint32_t flags = uid.vtxFlags;
    if (!words || wordCount < vtxWords(flags)) return fail(error, "truncated decoded vertex");
    const Decoded d = decode(words, flags);
    const uint32_t positionMatrix = (flags & VF_POSMTX) ? d.positionMatrix : (state.matrixA & 63);
    if (positionMatrix > 63) return fail(error, "position matrix rows exceed XF palette");
    const uint32_t normalMatrix = positionMatrix & 31;
    if ((flags & (VF_NRM | VF_NBT)) && normalMatrix > 30)
        return fail(error, "normal matrix rows exceed XF palette");
    uint32_t textureMatrix[8]{};
    for (uint32_t i = 0; i < uid.numTexGens; ++i) {
        textureMatrix[i] = (flags & VF_TEXMTX) ? d.textureMatrices[i] :
            (i < 4 ? (state.matrixA >> (6 * (i + 1))) & 63 : (state.matrixB >> (6 * (i - 4))) & 63);
        const uint32_t type = (uid.texMtxInfo[i] >> 4) & 7;
        if (type != 1 && type != 2 && type != 3 && textureMatrix[i] > 63)
            return fail(error, "texture matrix rows exceed XF palette");
    }
    ClipVertex result;
    float position[3], normal[3]{0, 0, 1}, binormal[3]{}, tangent[3]{};
    multiply(state.pos + positionMatrix, d.position, position);
    if (flags & VF_NRM) {
        for (int i = 0; i < 3; ++i) normal[i] = dot3(state.normal[normalMatrix + i], d.normal);
        normalize(normal);
    }
    if (flags & VF_NBT) {
        for (int i = 0; i < 3; ++i) {
            binormal[i] = dot3(state.normal[normalMatrix + i], d.binormal);
            tangent[i] = dot3(state.normal[normalMatrix + i], d.tangent);
        }
    }
    const float view[4]{position[0], position[1], position[2], 1};
    float clip[4];
    for (int i = 0; i < 4; ++i) clip[i] = dot4(state.projection.proj[i], view);
    const float depth = (clip[2] * state.projection.depthParams[0] +
                         clip[3] * state.projection.depthParams[1]) / 16777215.0f;
    result.gxZ = clip[3] != 0 ? depth / clip[3] : depth;
    result.viewZ = position[2];
    result.clip[0] = clip[0];
    result.clip[1] = clip[1];
    result.clip[2] = depth * 2 - clip[3];
    result.clip[3] = clip[3];

    for (uint32_t channel = 0; channel < 2; ++channel) {
        if (channel >= uid.numChans) {
            std::memcpy(result.color[channel], d.color[channel], sizeof(d.color[channel]));
            continue;
        }
        for (int alpha = 0; alpha < 2; ++alpha) {
            const uint32_t control = alpha ? uid.alphaCtrl[channel] : uid.colorCtrl[channel];
            const float* material = (control & 1) ? d.color[channel] : state.lights.matColor[channel];
            const float* ambient = (control & 64) ? d.color[channel] : state.lights.ambColor[channel];
            const int first = alpha ? 3 : 0, count = alpha ? 1 : 3;
            float illumination[4];
            std::memcpy(illumination, ambient, sizeof(illumination));
            if ((control >> 1) & 1) {
                const uint32_t mask = ((control >> 2) & 15) | (((control >> 11) & 15) << 4);
                const uint32_t diffuse = (control >> 7) & 3;
                const bool attenuation = (control >> 9) & 1, spot = (control >> 10) & 1;
                for (int light = 0; light < 8; ++light) {
                    if (!(mask & (1u << light))) continue;
                    const auto& values = state.lights.lights[light];
                    float direction[3];
                    float att = 1;
                    if (!attenuation || spot) {
                        for (int i = 0; i < 3; ++i) direction[i] = values[3][i] - position[i];
                        const float distanceSquared = dot3(direction, direction);
                        const float distance = std::sqrt(distanceSquared);
                        normalize(direction);
                        if (attenuation) {
                            const float a = std::max(0.0f, dot3(direction, values[4]));
                            const float angle[3]{1, a, a * a}, dist[3]{1, distance, distanceSquared};
                            att = std::max(0.0f, dot3(values[1], angle)) / dot3(values[2], dist);
                        }
                    } else {
                        std::memcpy(direction, values[3], sizeof(direction));
                        normalize(direction);
                        const float a = dot3(normal, direction) >= 0 ? std::max(0.0f, dot3(normal, values[4])) : 0;
                        const float angle[3]{1, a, a * a};
                        att = std::max(0.0f, dot3(values[1], angle)) / dot3(values[2], angle);
                    }
                    const float amount = diffuse == 0 ? 1 : diffuse == 1 ? dot3(direction, normal) : std::max(0.0f, dot3(direction, normal));
                    for (int i = first; i < first + count; ++i) illumination[i] += att * amount * values[0][i];
                }
                for (int i = first; i < first + count; ++i)
                    result.color[channel][i] = material[i] * std::clamp(illumination[i], 0.0f, 1.0f);
            } else {
                for (int i = first; i < first + count; ++i) result.color[channel][i] = material[i];
            }
        }
    }

    for (uint32_t i = 0; i < uid.numTexGens; ++i) {
        const uint32_t info = uid.texMtxInfo[i], type = (info >> 4) & 7, source = (info >> 7) & 31;
        float* target = result.tex[i];
        if (type == 2 || type == 3) {
            target[0] = result.color[type - 2][0];
            target[1] = result.color[type - 2][1];
            target[2] = 1;
            continue;
        }
        if (type == 1) {
            const uint32_t prior = (info >> 12) & 7, light = (info >> 15) & 7;
            if (prior < i) {
                float direction[3];
                for (int j = 0; j < 3; ++j) direction[j] = state.lights.lights[light][3][j] - position[j];
                normalize(direction);
                target[0] = result.tex[prior][0] + dot3(direction, tangent);
                target[1] = result.tex[prior][1] + dot3(direction, binormal);
                target[2] = result.tex[prior][2];
            } else target[2] = 1;
            continue;
        }
        float input[4]{0, 0, 1, 1};
        const float* original = nullptr;
        if (source == 0) original = d.position;
        else if (source == 1 && (flags & VF_NRM)) original = d.normal;
        else if (source == 3 && (flags & VF_NBT)) original = d.tangent;
        else if (source == 4 && (flags & VF_NBT)) original = d.binormal;
        if (original) std::memcpy(input, original, 3 * sizeof(float));
        else if (source >= 5 && source <= 12 && (flags & (VF_TEX0 << (source - 5)))) {
            input[0] = d.tex[source - 5][0];
            input[1] = d.tex[source - 5][1];
        }
        if (!((info >> 2) & 1)) input[2] = 1;
        multiply(state.pos + textureMatrix[i], input, target);
        if (!((info >> 1) & 1)) target[2] = 1;
        if (uid.dualTex) {
            if ((uid.postMtxInfo[i] >> 8) & 1) normalize(target);
            const float postInput[4]{target[0], target[1], target[2], 1};
            multiply(state.post + i * 3, postInput, target);
        }
    }
    output = result;
    return true;
}

}  // namespace gpu
