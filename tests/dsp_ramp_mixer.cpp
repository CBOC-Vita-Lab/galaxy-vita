// Actual mixer dispatch, with an independent wrapping-integer PCM reference.
#include "../platform/src/audio/dsp_ramp_kernel.h"
static unsigned neonCalls;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(PORT_DSPRAMP_NO_NEON)
static constexpr bool testHasNeon = true;
namespace port::dspramp {
inline int32_t CountedAddNeon(int16_t* dst, const int16_t* src, int32_t volume, int32_t step) {
    ++neonCalls;
    return AddNeon(dst, src, volume, step);
}
}
#define AddNeon CountedAddNeon
#else
static constexpr bool testHasNeon = false;
#endif
#include "../platform/src/audio/dsp_mixer.cpp"
#undef AddNeon
#include <cstdint>

static unsigned summaries, mismatches;
static int expectedMode;
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    ++summaries;
    if (strstr(format, "mismatch:")) ++mismatches;
}
static void Require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); abort(); }
}
static void RequireDispatch(unsigned before, s32 volume, s32 step, int offset) {
    const bool selected = expectedMode == 1 || expectedMode == 2;
    const bool independentSamples = offset == 0 || offset <= -80 || offset >= 80;
    const unsigned expected = testHasNeon && selected && (volume || step) && independentSamples ? 1u : 0u;
    Require(neonCalls - before == expected, "AddNeon invocation count matches initial mode, host, no-op and alias conditions");
}
static uint32_t rng = 0x93ab57edu;
static uint32_t Random() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int32_t Signed(uint32_t bits) { int32_t result; memcpy(&result, &bits, 4); return result; }
static int64_t Shift(int64_t value, unsigned bits) {
    return value >= 0 ? value / (int64_t(1) << bits) : -((-value + (int64_t(1) << bits) - 1) / (int64_t(1) << bits));
}
static s32 Reference(s16* dst, const s16* src, s32 volume, s32 step) {
    uint32_t bits = uint32_t(volume);
    for (int i = 0; i < 80; ++i) {
        const int64_t scaled = Shift(Shift(Signed(bits), 16) * src[i], 16);
        const int64_t value = dst[i] + scaled;
        dst[i] = value < -32768 ? -32768 : value > 32767 ? 32767 : s16(value);
        bits += uint32_t(step);
    }
    return Signed(bits);
}
static unsigned cases;
static void Check(s32 volume, s32 step, int offset) {
    s16 actual[241], expected[241];
    for (int i = 0; i < 241; ++i) actual[i] = s16(Random());
    memcpy(expected, actual, sizeof actual);
    const s32 reference = Reference(expected + 80, expected + 80 + offset, volume, step);
    const unsigned before = neonCalls;
    const s32 result = addWithRamp(actual + 80, actual + 80 + offset, volume, step);
    RequireDispatch(before, volume, step, offset);
    Require(result == reference, "final ramp volume, including signed wrap, is exact");
    Require(!memcmp(actual, expected, sizeof actual), "PCM, alias ordering and guard samples are exact");
    ++cases;
}
int main() {
    const char* setting = getenv("GXV_DSPRAMP");
    char* end = nullptr;
    const long value = setting && *setting ? strtol(setting, &end, 0) : 0;
    const int mode = end && end != setting && !*end && (value == 1 || value == 2) ? int(value) : 0;
    expectedMode = mode;
    {
        s16 actual[80], expected[80], source[80];
        for (int i = 0; i < 80; ++i) { source[i] = -32768; actual[i] = expected[i] = i & 1 ? 32767 : -32768; }
        const s32 volume = Reference(expected, source, INT32_MIN, 0);
        const unsigned before = neonCalls;
        Require(addWithRamp(actual, source, INT32_MIN, 0) == volume && !memcmp(actual, expected, sizeof actual),
                "-32768 times -32768 shifts to 16384 before destination saturation");
        RequireDispatch(before, INT32_MIN, 0, 80);
        ++cases;
    }
    for (unsigned v = 0; v < 65536; ++v) Check(Signed(v << 16), 0, 80);
    const int offsets[] = {-79, -1, 0, 1, 79, 80};
    for (int i = 0; i < 12000; ++i) Check(Signed(Random()), Signed(Random()), offsets[i % 6]);
    for (int offset : offsets) {
        Check(0, 0, offset);
        Check(INT32_MAX, INT32_MAX, offset);
        Check(INT32_MIN, INT32_MIN, offset);
        Check(INT32_MAX, 1, offset);
        Check(INT32_MIN, -1, offset);
    }
    Require(mismatches == 0, "in-process oracle reports no mismatches");
    Require(summaries != 0 || mode != 2, "verify mode emits a basic-log oracle summary");
    Require(summaries == 0 || mode == 2, "off/on/invalid modes have no oracle cost or logging");
    const unsigned before = summaries;
    setenv("GXV_DSPRAMP", mode == 2 ? "0" : "2", 1);
    for (int i = 0; i < 65536; ++i) Check(0x40000000, 13107, 80);
    Require(mode == 2 ? summaries > before : summaries == before, "switch is read once");
    printf("PASS: ramp mode=%d cases=%u exact PCM/volume/aliasing, neon_calls=%u summaries=%u mismatches=0\n",
           mode, cases, neonCalls, summaries);
}
