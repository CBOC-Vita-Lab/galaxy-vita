// A change of Settings > Controls > Tilt with makes the hold-the-controller pictures again (the same hook as a button swap); no change, no call.
#include "port/settings_native.h"
#include "port/tilt_gain.h"
#include <cstdio>
#include <cstdlib>
static int gCalls = 0;
extern "C" void port_handheld_pictures_changed(void) { ++gCalls; }
extern "C" int port_tilt_stick_set(int);
namespace port::settings { const NativeHost* NativeHostGet() { return nullptr; } }
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    port_tilt_stick_set(0); gCalls = 0;
    CHECK(port_tilt_stick_set(0) == 0 && gCalls == 0);
    CHECK(port_tilt_stick_set(1) == 1 && gCalls == 1 && port::tilt::UseStick());
    CHECK(port_tilt_stick_set(1) == 1 && gCalls == 1);
    CHECK(port_tilt_stick_set(0) == 0 && gCalls == 2 && !port::tilt::UseStick());
    std::puts("settings_live_tilt: all passed");
}
