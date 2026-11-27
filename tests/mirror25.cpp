// reflfur25: host test of platform/include/port/mirror25.h (the reflection's skip decision and census).
#include <cstdio>
#include <cstring>
#include "port/mirror25.h"
using namespace port::mirror25;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // exhaustive over the four inputs: the decision table
    int skips = 0, n = 0;
    for (int m = 0; m < 16; ++m) {
        Input in;
        in.hidden = m & 1; in.fading = (m >> 1) & 1; in.outside = (m >> 2) & 1; in.cameraNear = (m >> 3) & 1;
        const Reason r = Decide(in);
        ++n;
        // the game's own returns come first and are never "skipped" by this switch
        if (in.hidden) CHECK(r == kHidden && !Skips(r), "hidden wins");
        else if (in.fading) CHECK(r == kFading && !Skips(r), "fading wins over outside");
        else if (in.outside && !in.cameraNear) CHECK(r == kOffscreen && Skips(r), "outside, camera far: skipped");
        else CHECK(r == kDrawn && !Skips(r), "otherwise drawn (inside, or the camera near the origin)");
        skips += Skips(r) ? 1 : 0;
    }
    CHECK(n == 16 && skips == 1, "exactly one row (not hidden, not fading, outside, camera far) skips: %d", skips);
    // the bound: carrying makes it larger, never smaller than a figure 160 units tall around the origin plus its arms (>= 2 x 160 + 100)
    CHECK(Radius(false) >= 420.0f && Radius(true) > Radius(false), "bound");
    CHECK(NearGuard() >= 500.0f, "the judge's near plane is at 500 units");
    // census: 300 pictures a line, reset by the caller, and the line carries every count
    Census c;
    int lines = 0;
    for (int i = 0; i < 900; ++i) {
        c.reason[i % 5 == 0 ? kOffscreen : kDrawn]++;
        c.mode[(i & 1) ? 2 : 3]++;
        if (Tick(c)) { ++lines; char b[512]; const int w = Format(b, sizeof b, c, 2); CHECK(w > 0 && w < 511, "format fits"); c = Census(); }
    }
    CHECK(lines == 3, "three lines in 900 pictures: %d", lines);
    Census d; d.reason[kDrawn] = 240; d.reason[kOffscreen] = 60; d.mode[2] = 300; d.single = 240; d.pictures = 300;
    char b[512]; Format(b, sizeof b, d, 2);
    CHECK(std::strstr(b, "drawn 240") && std::strstr(b, "off screen 60") && std::strstr(b, "glass 300") && std::strstr(b, "one-pass 240") && std::strstr(b, "300 pictures"), "line: %s", b);
    std::printf("mirror25: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
