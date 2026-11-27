// big60: host test of vgx::prebuild::Pick (platform/vgx/src/vgx_prebuild_pick.h). nice g++ -std=c++17 tests/vgx_prebuild_pick_test.cpp -o /tmp/pp && /tmp/pp
#include <cstdio>
#include "../platform/vgx/src/vgx_prebuild_pick.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    auto ok = [](int r) { return r <= 6; };      // rungs 7 and 8 are the multisampled ones
    auto no = [](int r) { return r != 4; };      // a rung that is not single-sampled
    int o[2] = {-1, -1};
    // default (startFirst false): the two rungs under the current one, nearest first, never under the floor
    CHECK(vgx::prebuild::Pick(6, 0, false, false, -1, false, ok, o) == 2 && o[0] == 5 && o[1] == 4);
    CHECK(vgx::prebuild::Pick(6, 5, false, false, -1, false, ok, o) == 1 && o[0] == 5);
    CHECK(vgx::prebuild::Pick(6, 6, false, false, -1, false, ok, o) == 0);
    CHECK(vgx::prebuild::Pick(0, 0, false, false, -1, false, ok, o) == 0);
    // two sizes: only the floor is made, whatever its distance (the rung between is skipped, not a stop)
    CHECK(vgx::prebuild::Pick(6, 4, true, false, -1, false, ok, o) == 1 && o[0] == 4);
    CHECK(vgx::prebuild::Pick(6, 5, true, false, -1, false, ok, o) == 1 && o[0] == 5);
    CHECK(vgx::prebuild::Pick(6, 3, true, false, -1, false, ok, o) == 0);   // floor 3 is 3 away: out of the two-rung reach, as before
    // a rung with samples: the walk stops there
    CHECK(vgx::prebuild::Pick(6, 0, false, false, -1, false, no, o) == 1 && o[0] == 5);
    CHECK(vgx::prebuild::Pick(4, 0, false, false, -1, false, no, o) == 0);  // the current rung itself is not single-sampled
    // default behaviour is the same whatever the preset says while startFirst is off
    CHECK(vgx::prebuild::Pick(6, 2, false, true, 2, false, ok, o) == 2 && o[0] == 5 && o[1] == 4);
    // GXV_RUNG_PREBUILD_START=1: a fixed preset below the current rung is the one target, at any distance
    CHECK(vgx::prebuild::Pick(6, 2, false, true, 2, true, ok, o) == 1 && o[0] == 2);
    CHECK(vgx::prebuild::Pick(6, 4, true, true, 4, true, ok, o) == 1 && o[0] == 4);
    CHECK(vgx::prebuild::Pick(6, 0, false, true, 0, true, ok, o) == 1 && o[0] == 0);
    // not fixed, no preset, or the start is not below: as before
    CHECK(vgx::prebuild::Pick(6, 2, false, false, 2, true, ok, o) == 2 && o[0] == 5);
    CHECK(vgx::prebuild::Pick(6, 2, false, true, -1, true, ok, o) == 2 && o[0] == 5);
    CHECK(vgx::prebuild::Pick(2, 2, false, true, 2, true, ok, o) == 0);      // already there: nothing to make
    CHECK(vgx::prebuild::Pick(3, 2, false, true, 5, true, ok, o) == 1 && o[0] == 2);  // a start above the current rung: the ordinary walk (floor 2 is one below)
    // a start rung that is not single-sampled falls back to the walk
    CHECK(vgx::prebuild::Pick(6, 4, false, true, 4, true, no, o) == 1 && o[0] == 5);
    std::printf(fails ? "FAILED %d\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
