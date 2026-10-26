// Inject bad PCM/final-volume results only at the fixture's kernel boundary.
#include "../platform/src/audio/dsp_ramp_kernel.h"
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRAMP_NO_NEON)
static bool volumeFault;
namespace port::dspramp {
inline int32_t FaultyAddNeon(int16_t* dst, const int16_t* src, int32_t volume, int32_t step) {
    const int32_t result = AddNeon(dst, src, volume, step);
    if (volumeFault) return result ^ 1;
    dst[0] ^= 1;
    return result;
}
}
#define AddNeon FaultyAddNeon
#include "../platform/src/audio/dsp_mixer.cpp"
#undef AddNeon
static unsigned details, summaries;
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    if (strstr(format, "mismatch:")) ++details;
    else ++summaries;
}
int main() {
    for (int i = 0; i < 1024; ++i) {
        s16 source[80], actual[80], expected[80];
        for (int j = 0; j < 80; ++j) { source[j] = s16(j * 1031); actual[j] = expected[j] = s16(j * 393); }
        volumeFault = (i & 1) != 0;
        const s32 reference = addWithRampScalar(expected, source, 0x7fffffff, 13107);
        const s32 result = addWithRamp(actual, source, 0x7fffffff, 13107);
        if (result != reference || memcmp(actual, expected, sizeof actual)) abort();
    }
    if (details != 4 || summaries != 1) abort();
    puts("PASS: 1024 injected PCM/volume faults restore scalar results; only 4 mismatch details");
}
#else
int main() { return 0; }
#endif
