// PORT_HOT_KNOB (platform/include/port/hot_knob.h, GXV_HOT_KNOBS): the value of a switch read through the knob is the value of its
// expression, for every type the port uses (bool, int, unsigned, long, size_t, float, pointers), with GXV_HOT_KNOBS unset, 1 and 0;
// the expression is worked out once (and on every call only when its bits are the "unset" pattern); threads that race on the first
// read all get the same value (ThreadSanitizer build: no data race).  Usage: hot_knob <expected Fenced(): 0|1>
#include "port/hot_knob.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static int sFailures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++sFailures; } } while (0)

static std::atomic<int> sEvalInt{0}, sEvalFloat{0}, sEvalPtr{0}, sEvalSentinel{0}, sEvalRace{0}, sEvalOnce{0};
static int sTarget = 7;

static int KnobInt() { return PORT_HOT_KNOB(int, (sEvalInt.fetch_add(1), static_cast<int>(std::strtol(std::getenv("HK_INT"), nullptr, 0)))); }
static unsigned KnobUnsigned() { return PORT_HOT_KNOB(unsigned, std::strtoul(std::getenv("HK_UNSIGNED"), nullptr, 0)); }
static bool KnobBool() { return PORT_HOT_KNOB(bool, std::getenv("HK_BOOL") != nullptr); }
static bool KnobBoolOff() { return PORT_HOT_KNOB(bool, std::getenv("HK_NOT_SET_ANYWHERE") != nullptr); }
static long KnobLong() { return PORT_HOT_KNOB(long, std::strtol(std::getenv("HK_LONG"), nullptr, 0)); }
static size_t KnobSize() { return PORT_HOT_KNOB(size_t, static_cast<size_t>(std::strtol(std::getenv("HK_INT"), nullptr, 0)) << 10); }
static float KnobFloat() { return PORT_HOT_KNOB(float, (sEvalFloat.fetch_add(1), static_cast<float>(std::atof(std::getenv("HK_FLOAT"))))); }
static float KnobNegZero() { return PORT_HOT_KNOB(float, -0.0f); }
static int* KnobPtr() { return PORT_HOT_KNOB(int*, (sEvalPtr.fetch_add(1), &sTarget)); }
static const void* KnobNull() { return PORT_HOT_KNOB(const void*, nullptr); }
// A value equal to the "unset" pattern: right every time, worked out every time.
static int KnobSentinel() { return PORT_HOT_KNOB(int, (sEvalSentinel.fetch_add(1), 0x7FBADBAD)); }
static int KnobNegative() { return PORT_HOT_KNOB(int, -1); }
static int KnobMin() { return PORT_HOT_KNOB(int, INT32_MIN); }
// The "Once" pattern for initialisers with side effects: the guarded static runs once, the knob keeps its result.
static int OnceSlow() { static const int v = (sEvalOnce.fetch_add(1), 42); return v; }
static int KnobOnce() { return PORT_HOT_KNOB(int, OnceSlow()); }
static int KnobRace() { return PORT_HOT_KNOB(int, (sEvalRace.fetch_add(1), static_cast<int>(std::strtol(std::getenv("HK_INT"), nullptr, 0)) * 3)); }

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: hot_knob <expected fenced 0|1>\n"); return 2; }
    setenv("HK_INT", "0x30", 1);
    setenv("HK_UNSIGNED", "4000000000", 1);
    setenv("HK_BOOL", "", 1);
    setenv("HK_LONG", "-123456", 1);
    setenv("HK_FLOAT", "3000.5", 1);

    // Racing first reads (before anything else touches KnobRace): every thread gets the same value.
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] { for (int i = 0; i < 20000; ++i) if (KnobRace() != 0x30 * 3) wrong.fetch_add(1); });
    for (auto& t : threads) t.join();
    CHECK(wrong.load() == 0);
    CHECK(sEvalRace.load() >= 1 && sEvalRace.load() <= 8);

    for (int i = 0; i < 1000; ++i) {
        CHECK(KnobInt() == 0x30);
        CHECK(KnobUnsigned() == 4000000000u);
        CHECK(KnobBool() == true);
        CHECK(KnobBoolOff() == false);
        CHECK(KnobLong() == -123456);
        CHECK(KnobSize() == (size_t{0x30} << 10));
        CHECK(KnobFloat() == 3000.5f);
        const float nz = KnobNegZero();
        CHECK(nz == 0.0f && std::signbit(nz));
        CHECK(KnobPtr() == &sTarget);
        CHECK(KnobNull() == nullptr);
        CHECK(KnobSentinel() == 0x7FBADBAD);
        CHECK(KnobNegative() == -1);
        CHECK(KnobMin() == INT32_MIN);
        CHECK(KnobOnce() == 42);
    }
    // Read once: the environment changing later is not seen (as with the guarded statics).
    setenv("HK_INT", "5", 1);
    CHECK(KnobInt() == 0x30);
    CHECK(sEvalInt.load() == 1);
    CHECK(sEvalFloat.load() == 1);
    CHECK(sEvalPtr.load() == 1);
    CHECK(sEvalOnce.load() == 1);
    CHECK(sEvalSentinel.load() == 1000);
    CHECK(port::hotknob::Fenced() == (argv[1][0] == '1'));

    if (sFailures) { std::fprintf(stderr, "hot knob: %d failures\n", sFailures); return 1; }
    std::printf("hot knob: values, read-once, sentinel, Once pattern and %d racing first reads correct (GXV_HOT_KNOBS fenced=%s)\n",
                8, argv[1]);
    return 0;
}
