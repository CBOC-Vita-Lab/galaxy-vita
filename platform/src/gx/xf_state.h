// Resolved per-draw XF values, independent of any graphics API or buffer ABI.
#pragma once

#include "gl_shadergen.h"

namespace gpu {

struct ResolvedXf {
    float pos[66][4]{};
    float normal[33][4]{};
    float post[24][4]{};
    LightState lights{};
    ProjState projection{};
    uint32_t matrixA = 0, matrixB = 0, flags = 0;
};

}  // namespace gpu
