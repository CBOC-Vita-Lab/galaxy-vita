#include "gl_shadergen.h"

#include <stdio.h>

int main() {
    uint32_t bp[256]{}, xf[256]{};
    bp[0] = 4u << 16;
    xf[0x3F] = 1;
    bp[0x25] = 0x4321;
    bp[0x26] = 0x8765;
    auto initial = gpu::makeShaderUid(bp, xf, 0, false);
    bool passed = initial.indScale[0] == 0x4321 && initial.indScale[1] == 0x8765;
    // Odd stages read the high byte of each packed pair of scale registers.
    for (uint32_t reg : {0x25u, 0x26u}) {
        bp[reg] ^= 0x0100;
        auto changed = gpu::makeShaderUid(bp, xf, 0, false);
        passed &= !(initial == changed);
        passed &= gpu::genFragmentShader(initial) != gpu::genFragmentShader(changed);
        bp[reg] ^= 0x0100;
    }
    for (uint32_t first : {0x80u, 0xA0u}) {
        for (uint32_t slot = 0; slot < 4; ++slot)
            passed &= gpu::pixelStateDependsOnBpRegister(first + slot);
    }
    passed &= gpu::pixelStateDependsOnBpRegister(0x88);
    passed &= gpu::pixelStateDependsOnBpRegister(0xF3);
    passed &= !gpu::pixelStateDependsOnBpRegister(0xC0);
    fprintf(stderr, "GX indirect scale / LOD snapshot regression: %s\n", passed ? "passed" : "FAILED");
    return passed ? 0 : 1;
}
