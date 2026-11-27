// hitch33 (GXV_WIPE_GUARD): port_screen_wipe_moving (vita/runtime/flat_view.cpp) says whether a wipe / fade changed its closed fraction in the last recorded frames.
// Build: clang++ -std=c++20 -O1 -Iplatform/include -include cwchar -include string tests/wipe_moving.cpp vita/runtime/flat_view.cpp -o /tmp/wipe_moving && /tmp/wipe_moving
#include <cstdint>
#include <cstdio>
extern "C" void port_vr_wipe(int, float closed, unsigned int);
extern "C" int port_screen_quiet(void);
extern "C" int port_screen_wipe_moving(void);
static uint64_t gFrame = 10;
extern "C" uint64_t port_gx_recording_frame(void) { return gFrame; }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
    CHECK(port_screen_wipe_moving() == 0);
    // a ring closing: 0.5, 0.8, 0.95, 1.0 then held at 1.0
    const float closing[] = {0.5f, 0.8f, 0.95f, 1.0f};
    for (float c : closing) { ++gFrame; port_vr_wipe(1, c, 0); CHECK(port_screen_wipe_moving() == 1); }
    CHECK(port_screen_quiet() == 2); // (closed, and still counted as moving for 2 frames)
    ++gFrame; port_vr_wipe(1, 1.0f, 0); CHECK(port_screen_wipe_moving() == 1);
    ++gFrame; port_vr_wipe(1, 1.0f, 0); CHECK(port_screen_wipe_moving() == 1);
    ++gFrame; port_vr_wipe(1, 1.0f, 0); CHECK(port_screen_wipe_moving() == 0); // stands still: work may run
    for (int i = 0; i < 20; ++i) { ++gFrame; port_vr_wipe(1, 1.0f, 0); CHECK(port_screen_wipe_moving() == 0); CHECK(port_screen_quiet() == 2); }
    // two reporters in one frame (a fade over the cinema frame's black): the lower first report must not read as movement
    ++gFrame; port_vr_wipe(0, 0.3f, 0); port_vr_wipe(0, 1.0f, 0); CHECK(port_screen_wipe_moving() == 0);
    ++gFrame; port_vr_wipe(0, 1.0f, 0); port_vr_wipe(0, 0.3f, 0); CHECK(port_screen_wipe_moving() == 0);
    // opening: 1.0 -> 0.97 is movement at once, and stays so while it opens
    ++gFrame; port_vr_wipe(1, 0.97f, 0); CHECK(port_screen_wipe_moving() == 1); CHECK(port_screen_quiet() == 2);
    ++gFrame; port_vr_wipe(1, 0.92f, 0); CHECK(port_screen_wipe_moving() == 1);
    ++gFrame; port_vr_wipe(1, 0.5f, 0); CHECK(port_screen_wipe_moving() == 1); CHECK(port_screen_quiet() == 0);
    // no reports for a while (the wipe is gone): not moving
    gFrame += 10; CHECK(port_screen_wipe_moving() == 0);
    // a fade that appears fully closed after a gap is a black screen, not a movement (6a); one that appears part closed is
    ++gFrame; port_vr_wipe(0, 1.0f, 0); CHECK(port_screen_wipe_moving() == 0);
    gFrame += 3; port_vr_wipe(0, 1.0f, 0); CHECK(port_screen_wipe_moving() == 0);
    gFrame += 10; port_vr_wipe(0, 0.95f, 0); CHECK(port_screen_wipe_moving() == 1);
    gFrame += 10;
    // a static partial cover (0.95, not changing) is not movement
    gFrame += 10;
    for (int i = 0; i < 6; ++i) { ++gFrame; port_vr_wipe(0, 0.95f, 0); }
    CHECK(port_screen_wipe_moving() == 0); CHECK(port_screen_quiet() == 2);
    std::printf(fails ? "FAILED %d\n" : "PASS wipe_moving\n", fails);
    return fails ? 1 : 0;
}
