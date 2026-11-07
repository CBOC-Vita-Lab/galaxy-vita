// GXV_LC_KEYSEARCH host proof (logiccut, 6 Nov): port/key_search.h against the original key search of J3DGetKeyFrameInterpolation
// (decomp/src/JSystem/J3DGraphAnimator/J3DAnimation.cpp), copied below as the reference. Both must pick the same branch (before the
// first key, after the last key, or the same key interval) for every frame and table; the integer search may give up (NaN frame or
// NaN key time met), in which case the game runs the original.
// Build and run: bash tools/test_lc_keysearch.sh
#include "port/key_search.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using port::keysearch::Result;

// The original (J3DAnimation.cpp, J3DGetKeyFrameInterpolation), with the Hermite call replaced by "the interval starts at pData".
template <typename T>
static Result Reference(float frame, uint16_t maxFrame, uint16_t type, const T* pData, const T** ppKey) {
    if (frame < pData[0]) {
        return port::keysearch::kBeforeFirst;
    }
    if (type == 0) {
        uint32_t idx = (maxFrame - 1) * 3;
        if (pData[idx] <= frame) {
            return port::keysearch::kAfterLast;
        }
        uint32_t uVar7 = maxFrame;
        while (uVar7 > 1) {
            uint32_t uVar2 = uVar7 >> 1;
            uint32_t tmp = uVar2 * 3;
            if (frame >= pData[tmp]) {
                pData += tmp;
                uVar7 = uVar7 - uVar2;
            } else {
                uVar7 = uVar2;
            }
        }
        *ppKey = pData;
        return port::keysearch::kBetween;
    } else {
        uint32_t idx = maxFrame - 1;
        if (pData[idx * 4] <= frame) {
            return port::keysearch::kAfterLast;
        }
        uint32_t var_r27 = maxFrame;
        while (var_r27 > 1) {
            uint32_t var_r25 = var_r27 >> 1;
            uint32_t var_r23 = var_r25 * 4;
            if (frame >= pData[var_r23]) {
                pData += var_r23;
                var_r27 = var_r27 - var_r25;
            } else {
                var_r27 = var_r25;
            }
        }
        *ppKey = pData;
        return port::keysearch::kBetween;
    }
}

static float Bits(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

static std::mt19937 rng(12345);
static uint32_t R() { return rng(); }

static float SpecialFrame() {
    static const float s[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.999999f, -0.999999f, 1.0e-30f, -1.0e-30f, Bits(1), Bits(0x80000001),
                              32767.0f, 32767.5f, 32768.0f, -32768.0f, -32768.5f, -32769.0f, 39999.996f, 40000.0f, 40000.004f, -39999.996f,
                              -40000.0f, -40000.004f, 1.0e9f, -1.0e9f, std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(), 8388607.5f, 8388608.0f, 16777216.0f, -8388607.5f};
    return s[R() % (sizeof s / sizeof s[0])];
}

static float RandomFrame(int range) {
    switch (R() % 8) {
    case 0: return SpecialFrame();
    case 1: return (float)((int)(R() % (2 * range + 1)) - range);  // a whole number
    case 2: {  // a whole number plus or minus one ulp
        float w = (float)((int)(R() % (2 * range + 1)) - range);
        return std::nextafter(w, (R() & 1) ? 1.0e9f : -1.0e9f);
    }
    case 3: return Bits(R());  // any bits (NaN included)
    default: return ((float)(R() % 2000001) / 1000000.0f - 1.0f) * (float)range;
    }
}

struct Stats { long cases = 0, gaveUp = 0, before = 0, after = 0, between = 0, bad = 0; };

template <typename T>
static void Check(Stats& st, float frame, uint16_t maxFrame, uint16_t type, const T* pData) {
    const uint32_t stride = type == 0 ? 3 : 4;
    const T* a = nullptr;
    const T* b = nullptr;
    const Result fast = port::keysearch::Find<T>(frame, maxFrame, stride, pData, &a);
    st.cases++;
    if (fast == port::keysearch::kGaveUp) {
        st.gaveUp++;
        // it may give up only for a NaN frame or a NaN key time
        uint32_t fb;
        std::memcpy(&fb, &frame, 4);
        bool nan = port::keysearch::IsNaNBits(fb);
        if (!nan && sizeof(T) == 4) {
            for (uint32_t i = 0; i < maxFrame * stride && !nan; i += stride) {
                uint32_t kb;
                std::memcpy(&kb, &pData[i], 4);
                nan = port::keysearch::IsNaNBits(kb);
            }
        }
        if (!nan) {
            if (st.bad++ < 10) std::printf("MISMATCH: gave up without a NaN, frame %a\n", (double)frame);
        }
        return;
    }
    const Result ref = Reference<T>(frame, maxFrame, type, pData, &b);
    if (fast != ref || (fast == port::keysearch::kBetween && a != b)) {
        if (st.bad++ < 10)
            std::printf("MISMATCH: frame %a (%g) keys %u type %u: fast %d at %ld, original %d at %ld\n", (double)frame, (double)frame,
                        (unsigned)maxFrame, (unsigned)type, (int)fast, a ? (long)(a - pData) : -1L, (int)ref, b ? (long)(b - pData) : -1L);
        return;
    }
    if (fast == port::keysearch::kBeforeFirst) st.before++;
    else if (fast == port::keysearch::kAfterLast) st.after++;
    else st.between++;
}

template <typename T> static T KeyTime(int range, bool wild);
template <> int16_t KeyTime<int16_t>(int range, bool wild) {
    if (wild) return (int16_t)R();
    return (int16_t)((int)(R() % (2 * range + 1)) - range);
}
template <> float KeyTime<float>(int range, bool wild) {
    if (wild) {
        switch (R() % 6) {
        case 0: return Bits(R());  // NaN included
        case 1: return SpecialFrame();
        default: break;
        }
    }
    switch (R() % 4) {
    case 0: return (float)((int)(R() % (2 * range + 1)) - range);
    case 1: return (R() & 1) ? 0.0f : -0.0f;
    default: return ((float)(R() % 2000001) / 1000000.0f - 1.0f) * (float)range;
    }
}

template <typename T>
static void Run(const char* name, Stats& st, int tables) {
    for (int t = 0; t < tables; t++) {
        const uint16_t type = (uint16_t)(R() & 1);
        const uint32_t stride = type == 0 ? 3 : 4;
        const uint16_t keys = (uint16_t)(2 + (R() % ((R() & 7) ? 40 : 600)));
        const int range = (R() & 3) == 0 ? 33000 : (int)(1 + R() % 2000);
        const bool sorted = (R() % 5) != 0;  // the original's walk must match also on unsorted tables
        const bool wild = (R() % 7) == 0;
        std::vector<T> data(keys * stride + 3);
        std::vector<T> times(keys);
        for (auto& v : times) v = KeyTime<T>(range, wild);
        if (sorted) {
            for (size_t i = 1; i < times.size(); i++)
                if (!(times[i] >= times[i - 1])) {
                    for (size_t j = i; j > 0 && !(times[j] >= times[j - 1]); j--) std::swap(times[j], times[j - 1]);
                }
        }
        for (uint32_t i = 0; i < keys; i++) {
            data[i * stride] = times[i];
            for (uint32_t c = 1; c < stride; c++) data[i * stride + c] = (T)(R() % 100);
        }
        for (int f = 0; f < 300; f++) Check<T>(st, RandomFrame(range), keys, type, data.data());
        // every key time, and its neighbours
        for (uint32_t i = 0; i < keys; i++) {
            const float k = (float)data[i * stride];
            Check<T>(st, k, keys, type, data.data());
            Check<T>(st, std::nextafter(k, 1.0e30f), keys, type, data.data());
            Check<T>(st, std::nextafter(k, -1.0e30f), keys, type, data.data());
            Check<T>(st, k + 0.5f, keys, type, data.data());
            Check<T>(st, k - 0.5f, keys, type, data.data());
        }
    }
    std::printf("%s: %ld cases: %ld before the first key, %ld after the last, %ld between; %ld gave up (NaN); %ld mismatches\n", name,
                st.cases, st.before, st.after, st.between, st.gaveUp, st.bad);
}

int main(int argc, char** argv) {
    const int tables = argc > 1 ? std::atoi(argv[1]) : 20000;
    Stats s16, f32;
    Run<int16_t>("s16 key times", s16, tables);
    Run<float>("f32 key times", f32, tables);
    // every s16 key time against frames around it: exhaustive over the s16 range for a one-key-pair table
    Stats ex;
    for (int k = -32768; k <= 32767; k++) {
        int16_t d[8] = {(int16_t)k, 1, 2, (int16_t)(k < 32767 ? k + 1 : k), 3, 4, 0, 0};
        const float fk = (float)k;
        const float frames[] = {fk, std::nextafter(fk, 1.0e30f), std::nextafter(fk, -1.0e30f), fk + 0.5f, fk - 0.5f, fk + 0.25f, fk - 0.75f};
        for (float fr : frames) Check<int16_t>(ex, fr, 2, 0, d);
    }
    std::printf("s16 exhaustive key times: %ld cases, %ld mismatches\n", ex.cases, ex.bad);
    const long bad = s16.bad + f32.bad + ex.bad;
    std::printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
