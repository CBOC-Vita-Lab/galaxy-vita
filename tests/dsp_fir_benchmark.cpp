// Optional host microbenchmark only; timings are not Vita game performance measurements.
#include "../platform/src/audio/dsp_mixer.cpp"
#include <chrono>
#include <initializer_list>
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char*, ...) {}
int main(int argc, char** argv) {
    const bool fast = argc > 1 && atoi(argv[1]) != 0;
    const int count = argc > 2 ? atoi(argv[2]) : 200000;
    for (int taps : {8, 20, 24}) {
        Voice voice{};
        Buffer buffer;
        for (int i = 0; i < 80; ++i) buffer[i] = s16(i * 997 - 32768);
        for (int i = 0; i < 20; ++i) voice._80[i] = s16(i * 1293 - 32768);
        s16 coefficients[24];
        for (int i = 0; i < 24; ++i) coefficients[i] = s16(i * 1313 - 16384);
        memcpy(reinterpret_cast<unsigned char*>(&voice) + offsetof(Voice, fir_filter_params), coefficients, sizeof coefficients);
        const auto begin = std::chrono::steady_clock::now();
        for (int n = 0; n < count; ++n) {
            if (fast) fir(&voice, buffer, taps);
            else firScalar(&voice, buffer, taps);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        uint64_t checksum = 0;
        for (s16 sample : buffer) checksum = checksum * 31 + uint16_t(sample);
        for (s16 sample : voice._80) checksum = checksum * 31 + uint16_t(sample);
        printf("FIR fast=%d taps=%d blocks=%d ms=%.3f checksum=%llu\n", fast, taps, count, ms, static_cast<unsigned long long>(checksum));
    }
}
