// Optional host microbenchmark; not a Vita or whole-game speed measurement.
#include "../platform/src/audio/dsp_mixer.cpp"
#include <chrono>
extern "C" u32 port_dsp_varam_base(void) { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char*, ...) {}
int main(int argc, char** argv) {
    const int path = argc > 1 ? atoi(argv[1]) : 0;
    const int blocks = argc > 2 ? atoi(argv[2]) : 1000000;
    for (int ramp = 0; ramp < 2; ++ramp) {
        s16 source[80], destination[80];
        for (int i = 0; i < 80; ++i) { source[i] = s16(i * 977 - 32768); destination[i] = s16(i * 311); }
        s32 volume = 0x40000000;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i) {
            const s32 step = ramp ? 0x00160001 : 0;
            volume = path ? addWithRamp(destination, source, volume, step) : addWithRampScalar(destination, source, volume, step);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        uint64_t checksum = uint32_t(volume);
        for (s16 sample : destination) checksum = checksum * 31 + uint16_t(sample);
        printf("ramp path=%d varying=%d blocks=%d ms=%.3f checksum=%llu\n", path, ramp, blocks, ms,
               static_cast<unsigned long long>(checksum));
    }
}
