// Full flat-screen XF fallback for variants that exceed Vita vertex uniforms.
#pragma once

#include <stddef.h>

#include "xf_state.h"

namespace gpu {

struct ClipVertex {
    float clip[4]{};
    float color[2][4]{};
    float tex[8][3]{};
    float viewZ = 0, gxZ = 0;
};
static_assert(sizeof(ClipVertex) == 152);

struct CpuXfVertex {
    ClipVertex xf;
    uint32_t pixelState = 0;
};
static_assert(sizeof(CpuXfVertex) == 156);

std::string genCpuXfDiagnosticVertexShader(const ShaderUid& uid);

// Input words are the recorder's decoded, native-endian vertex, not GX FIFO.
bool transformVertexCpu(const ShaderUid& uid, const ResolvedXf& state,
                        const uint32_t* words, size_t wordCount,
                        ClipVertex& output, const char** error = nullptr);

}  // namespace gpu
