// Exercise the actual synchronous mixer function, including its environment and oracle dispatch.
#include "../platform/src/audio/dsp_mixer.cpp"
#include <cstdarg>
#include <cstdint>

static unsigned summaries, failures;
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    ++summaries;
    if (strstr(format, "mismatch:")) ++failures;
}
static void Require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); abort(); }
}
static uint32_t state = 0x94812abdu;
static uint32_t Random() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
static s16 ClampReference(int64_t value) { return value < -32768 ? -32768 : value > 32767 ? 32767 : s16(value); }
static void Reference(Voice* voice, s16* buffer, int taps) {
    if (taps > 24) taps = 24;
    s16 coefficients[24], input[100];
    memcpy(coefficients, reinterpret_cast<const unsigned char*>(voice) + offsetof(Voice, fir_filter_params), sizeof coefficients);
    memcpy(input, voice->_80, 40);
    memcpy(input + 20, buffer, 160);
    for (int i = 0; i < 80; ++i) {
        int64_t sum = 0;
        for (int k = 0; k < taps && k <= 20 + i; ++k) sum += int64_t(coefficients[k]) * input[20 + i - k];
        buffer[i] = ClampReference(sum >> 15);
    }
    memcpy(voice->_80, input + 80, 40);
}
int main() {
    const char* setting = getenv("GXV_DSPFIR");
    char* end = nullptr;
    const long value = setting && *setting ? strtol(setting, &end, 0) : 0;
    const int requested = end && end != setting && !*end && (value == 1 || value == 2) ? int(value) : 0;
    unsigned cases = 0;
    for (int taps = -1; taps <= 31; ++taps) for (int run = 0; run < 300; ++run) {
        Voice actual{}, expected{};
        unsigned char* bytes = reinterpret_cast<unsigned char*>(&actual);
        for (size_t j = 0; j < sizeof actual; ++j) bytes[j] = static_cast<unsigned char>(Random());
        s16 buffer[80], reference[80];
        for (int j = 0; j < 80; ++j) buffer[j] = s16(Random());
        if (run < 4) {
            s16 coefficients[24];
            for (int j = 0; j < 24; ++j) coefficients[j] = run & 1 ? -32768 : 32767;
            memcpy(bytes + offsetof(Voice, fir_filter_params), coefficients, sizeof coefficients);
            for (int j = 0; j < 20; ++j) actual._80[j] = run & 2 ? -32768 : 32767;
            for (int j = 0; j < 80; ++j) buffer[j] = run & 2 ? -32768 : 32767;
        }
        expected = actual;
        memcpy(reference, buffer, sizeof buffer);
        Reference(&expected, reference, taps);
        fir(&actual, buffer, taps);
        Require(!memcmp(buffer, reference, sizeof buffer), "PCM is exact for every tap count and signed accumulator edge");
        Require(!memcmp(&actual, &expected, sizeof actual), "history and all unrelated voice bytes are exact");
        ++cases;
    }
    Require(failures == 0, "oracle found no mismatches");
    Require(summaries == (requested == 2 ? 1u : 0u), "verify mode emits a basic-log summary; off and invalid settings stay silent");
    setenv("GXV_DSPFIR", requested == 2 ? "0" : "2", 1);
    Require(port::dspfir::Mode() == (requested == 1 || requested == 2 ? requested : 0), "switch is read once");
    printf("PASS: actual FIR dispatch mode=%d cases=%u, PCM/history exact, summaries=%u, mismatches=0\n", requested, cases, summaries);
}
