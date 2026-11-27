// getenv25: port/getenv25.h's switches are read from the environment once, and answer what vgx::EnvInt answered (same parsing, same defaults).
// Counts the getenv calls with an interposed getenv (the executable's own definition wins over libc's for every call in this file).
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned g_calls = 0;
static const char* g_val = nullptr;   // the value every name has in this run (nullptr: not set)
extern "C" char* getenv(const char* name) {
    if (std::strcmp(name, "GXV_HOT_KNOBS") == 0) return nullptr;   // (the knob's own switch: not counted)
    ++g_calls;
    return const_cast<char*>(g_val);
}
#include "port/getenv25.h"

// the old code, as it was in the call sites: vgx::EnvInt (vgx_common.h) read at every call
static long OldEnvInt(const char* name, long dflt) { const char* v = getenv(name); return (v && *v) ? std::strtol(v, nullptr, 0) : dflt; }

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    g_val = std::strcmp(argv[1], "-") == 0 ? nullptr : argv[1];
    // what the old reads answered (the call sites' own expressions)
    const bool wantPass = OldEnvInt("GXV_PASSDIRECT", 1) != 0;
    const long tinyOld = std::max<long>(0, OldEnvInt("GXV_TEXUP_TINY_BYTES", 0));
    const int wantPtr = static_cast<int>(OldEnvInt("GXV_HRW_POINTER", 0));
    const bool wantOv = OldEnvInt("GXV_OVERSCAN", 0) != 0, wantPrev = OldEnvInt("GXV_PREVCAP", 0) != 0;
    const int N = 1000;   // calls of each in a picture-sized loop
    // before: one getenv per call
    g_calls = 0;
    for (int i = 0; i < N; ++i) {
        volatile bool a = OldEnvInt("GXV_PASSDIRECT", 1) != 0; (void)a;
        volatile long b = std::max<long>(0, OldEnvInt("GXV_TEXUP_TINY_BYTES", 0)); (void)b;
        volatile long c = OldEnvInt("GXV_HRW_POINTER", 0); (void)c;
        volatile bool d = OldEnvInt("GXV_OVERSCAN", 0) != 0; (void)d;
        volatile bool e = OldEnvInt("GXV_PREVCAP", 0) != 0; (void)e;
    }
    const unsigned before = g_calls;
    g_calls = 0;
    errno = 1234;
    for (int i = 0; i < N; ++i) {
        assert(port::getenv25::PassDirectOn() == wantPass);
        // (the tiny-bytes word is an int: a value past INT_MAX answers INT_MAX, which no texture's byte count reaches)
        assert(static_cast<unsigned long long>(port::getenv25::TexupTinyBytes()) == static_cast<unsigned long long>(tinyOld > INT_MAX ? INT_MAX : tinyOld));
        assert(port::getenv25::HrwPointer() == wantPtr);
        assert(port::getenv25::OverscanOn() == wantOv);
        assert(port::getenv25::PrevCapOn() == wantPrev);
    }
    const unsigned after = g_calls;
    assert(errno == 1234);
    std::printf("getenv25: value=%s: %u getenv calls for %d rounds of 5 reads before, %u after\n", argv[1], before, N, after);
    assert(before == 5u * N);
    assert(after == 5u);   // one read each, ever
    return 0;
}
