#include "dsp_mix_kernels.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
int16_t Clamp(int32_t n) { return n < -32768 ? -32768 : n > 32767 ? 32767 : int16_t(n); }
void OldAdd(int16_t* dst, const int16_t* src, int n, uint16_t volume) {
    for (int i = 0; i < n; ++i) dst[i] = Clamp(dst[i] + Clamp((int32_t(src[i]) * int32_t(volume)) >> 15));
}
void OldScale(int16_t* dst, int n, uint16_t volume, int shift) {
    for (int i = 0; i < n; ++i) dst[i] = Clamp((int32_t(dst[i]) * int32_t(volume)) >> shift);
}
constexpr int16_t edges[] = {-32768, -32767, -30000, -16384, -2, -1, 0, 1, 2, 16383, 16384, 30000, 32766, 32767};
uint32_t Random(uint32_t& x) { x = x * 1664525u + 1013904223u; return x; }
}

int main(int argc, char** argv) {
    namespace mix = port::dspmix;
    int expectedMode = std::atoi(std::getenv("GXV_DSPNEON") ? std::getenv("GXV_DSPNEON") : "0");
    if (expectedMode != 1 && expectedMode != 2) expectedMode = 0;
    Require(mix::Mode() == expectedMode, "DSP mode defaults off and recognizes only 1/2");
    setenv("GXV_DSPNEON", expectedMode ? "0" : "2", 1);
    Require(mix::Mode() == expectedMode, "DSP mode is read only once");
    unsigned checks = 0;
    std::array<int16_t, 160> src{}, slow{}, fast{};
    for (uint32_t volume = 0; volume <= 65535; ++volume) {
        for (unsigned i = 0; i < src.size(); ++i) src[i] = edges[i % 14];
        const int count = 17 + int(volume % 80), offset = volume % 4;
        for (unsigned i = 0; i < src.size(); ++i) slow[i] = fast[i] = edges[(i + 5) % 14];
        OldAdd(slow.data() + offset, src.data() + offset, count, uint16_t(volume));
        mix::AddFast(fast.data() + offset, src.data() + offset, count, uint16_t(volume));
        Require(slow == fast, "every unsigned volume, negative saturation, offsets and tails match original add exactly");
        for (unsigned i = 0; i < src.size(); ++i) fast[i] = edges[(i + 5) % 14];
        Require(mix::AddChecked(fast.data() + offset, src.data() + offset, count, uint16_t(volume)), "in-process add oracle agrees");
        Require(slow == fast, "checked add matches expected PCM byte-for-byte");
        ++checks;
        for (int shift : {12, 15}) {
            slow = fast = src;
            OldScale(slow.data() + offset, count, uint16_t(volume), shift);
            mix::ScaleFast(fast.data() + offset, count, uint16_t(volume), shift);
            Require(slow == fast, "every unsigned volume and both production shifts match original scale exactly");
            fast = src;
            Require(mix::ScaleChecked(fast.data() + offset, count, uint16_t(volume), shift), "in-process scale oracle agrees");
            Require(slow == fast, "checked scale matches expected PCM byte-for-byte");
            ++checks;
        }
    }
    uint32_t seed = 0x605b188;
    for (unsigned round = 0; round < 10000; ++round) {
        for (auto& n : src) n = int16_t(Random(seed) >> 16);
        const int count = Random(seed) % 129;
        const uint16_t volume = Random(seed) >> 8;
        const int srcOffset = 16, dstOffset = 8 + int(Random(seed) % 17);
        slow = fast = src;
        OldAdd(slow.data() + dstOffset, slow.data() + srcOffset, count, volume);
        mix::AddFast(fast.data() + dstOffset, fast.data() + srcOffset, count, volume);
        Require(slow == fast, "self and partial overlap preserve scalar forward-update order");
        slow = fast = src;
        const int shift = round % 3 ? 12 : 14;
        OldScale(slow.data(), count, volume, shift);
        mix::ScaleFast(fast.data(), count, volume, shift);
        Require(slow == fast, "random PCM, zero/odd counts and unsupported-shift fallback match scalar");
        checks += 2;
    }
    for (int count : {0, 1, 7, 8, 9, 15, 16, 17, 39, 40, 79, 80, 81, 127}) {
        slow = fast = src;
        OldAdd(slow.data(), slow.data(), count, 0xB820);
        mix::AddFast(fast.data(), fast.data(), count, 0xB820);
        Require(slow == fast, "exact self-alias works through vector groups and scalar tail");
        ++checks;
    }
    std::printf("PASS: DSP exact integer kernels: %u comparisons plus in-process oracles; all 65536 volumes, shifts12/15, saturations, odd tails, self/partial alias; actual_neon=%d mode=%d\n", checks, mix::kHasNeon, expectedMode);
    if (argc > 1 && !std::strcmp(argv[1], "--bench")) {
        constexpr unsigned calls = 1000000;
        for (unsigned i = 0; i < src.size(); ++i) src[i] = int16_t(Random(seed) >> 16);
        for (bool vector : {false, true}) {
            fast = src;
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < calls; ++i) {
                const uint16_t volume = uint16_t(i);
                if (vector) { mix::AddFast(fast.data(), src.data(), 80, volume); mix::ScaleFast(fast.data(), 80, volume, 15); }
                else { OldAdd(fast.data(), src.data(), 80, volume); OldScale(fast.data(), 80, volume, 15); }
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            uint64_t checksum = 0; for (int16_t n : fast) checksum = checksum * 33 + uint16_t(n);
            std::printf("host DSP exercise: vector=%d calls=%u add80+scale80 elapsed_ms=%.3f checksum=%llu (not Vita timing)\n", vector, calls,
                        ms, static_cast<unsigned long long>(checksum));
        }
    }
}
