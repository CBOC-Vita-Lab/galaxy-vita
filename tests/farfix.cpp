#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include "port/farfix.h"
using namespace port::farfix;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    CHECK(ParseOn(nullptr) && ParseOn("") && ParseOn("1") && !ParseOn("0"));
    for (const char* n : {"WaterfallS", "WaterfallL", "ForestWaterfallL", "TwinFallLakeWaterFall"}) {
        CHECK(IsWaterfall(n));
        CHECK(Radius(1000.0f, n, true) == 1500.0f);          // +50 %: "a touch"
        CHECK(Radius(1000.0f, n, false) == 1000.0f);         // escape: the game's own
        CHECK(FarMeters(50, n, true) == 100 && FarMeters(50, n, false) == 50);
        CHECK(FarMeters(200, n, true) == 200);               // only the 50 m class moves
    }
    for (const char* n : {"Steam", "FountainBig", "DrainPipeBubbleA", ""}) {
        CHECK(!IsWaterfall(n));
        CHECK(Radius(500.0f, n, true) == 500.0f && FarMeters(50, n, true) == 50);
    }
    CHECK(!IsWaterfall(nullptr) && Radius(7.0f, nullptr, true) == 7.0f);
    std::puts(fails ? "FAILED" : "farfix: ok");
    return fails != 0;
}
