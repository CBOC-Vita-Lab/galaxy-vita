// Real mixer dispatch, environment selection and logging; only external DSP/log boundaries are fixtures.
#include "../platform/src/audio/dsp_mixer.cpp"
#include <cstdarg>

static unsigned summaries, failures;
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    ++summaries;
    if (std::strstr(format, "mismatch:")) ++failures;
}
static void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
int main() {
    const int mode = port::dspmix::Mode();
    int16_t source[80], actual[80], expected[80];
    for (unsigned i = 0; i < 80; ++i) source[i] = actual[i] = expected[i] = int16_t(i * 1023 - 32768);
    for (unsigned volume = 0; volume <= 65535; ++volume) {
        port::dspmix::AddScalar(expected, source, 40, uint16_t(volume));
        addWithVolume(actual, source, 40, uint16_t(volume));
        Require(!std::memcmp(actual, expected, sizeof actual), "real add dispatch produces exact PCM");
        port::dspmix::ScaleScalar(expected, 80, uint16_t(volume), 12);
        scaleInPlace(actual, uint16_t(volume), 12);
        Require(!std::memcmp(actual, expected, sizeof actual), "real scale dispatch produces exact PCM");
    }
    Require(failures == 0, "real verify mode reports no mismatches");
    Require(summaries == (mode == 2 ? 2u : 0u), "verify mode is live and reaches the basic log, quiet/off modes do not log");
    std::printf("PASS: real DSP mixer dispatch mode=%d: 131072 exact PCM kernel calls, summary_lines=%u, mismatches=0\n", mode, summaries);
    for (unsigned i = 0; i < 1024; ++i) noteDspKernelVerify(false, "injected-report-test");
    Require(failures == 15, "mismatch detail is bounded to the first eight and later powers of two");
    std::puts("PASS: DSP oracle mismatch reporting is rate-limited; PCM oracle fallback tested separately");
}
