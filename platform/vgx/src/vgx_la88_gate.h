#pragma once
#include <cstdint>

namespace vgx::la88 {
// Only replace a generated RGBA8 chain. Avoid the authored I4 path, 16-bit
// experiments, tiny/non-square transfer shapes and unknown driver formats.
inline bool Candidate(unsigned format, unsigned w, unsigned h, bool generate, bool tex16Mips) {
    return format >= 1 && format <= 3 && generate && !tex16Mips &&
           w >= 8 && w <= 1024 && w == h && (w & (w - 1)) == 0;
}
inline bool CanProbeInFrame(int mode, bool live) { return mode == 2 || !live; }
}
