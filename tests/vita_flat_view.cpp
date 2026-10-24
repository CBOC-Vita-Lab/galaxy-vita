#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "port/compat.h"

extern "C" {
int port_vr_cull_view(float*, float*, float*) __attribute__((weak));
int port_vr_move_axes(float*, float*, float*) __attribute__((weak));
int port_vr_pointer_in_world() __attribute__((weak));
int port_vr_pointer_ray(float*, float*) __attribute__((weak));
int port_vr_diorama() __attribute__((weak));
void port_vr_transit_begin(const void*) __attribute__((weak));
void port_vr_transit_end(const void*) __attribute__((weak));
const void* port_vr_transit_host() __attribute__((weak));
void port_vr_pointer_reach(float) __attribute__((weak));
void port_vr_pointer_depth(float) __attribute__((weak));
void port_vr_pointer_touched(unsigned long long) __attribute__((weak));
void port_vr_wipe(int, float, unsigned int) __attribute__((weak));
void port_vr_pause_menu(int) __attribute__((weak));
}

static unsigned checks;
static void require(bool passed, const char* name) {
    ++checks;
    if (!passed) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main() {
    require(port_vr_cull_view && port_vr_move_axes && port_vr_pointer_in_world && port_vr_pointer_ray &&
            port_vr_diorama && port_vr_transit_begin && port_vr_transit_end && port_vr_transit_host &&
            port_vr_pointer_reach && port_vr_pointer_depth && port_vr_pointer_touched && port_vr_wipe &&
            port_vr_pause_menu, "production handheld game-view adapter exists");
    std::array<float, 21> output;
    for (unsigned i = 0; i < output.size(); ++i) output[i] = float(i) - 9.5f;
    const auto before = output;
    require(port_vr_cull_view(output.data(), &output[12], &output[13]) == 0 &&
            std::memcmp(output.data(), before.data(), sizeof(output)) == 0,
            "original game culling retains its camera and frustum fallback");
    require(port_vr_move_axes(output.data(), &output[3], &output[6]) == 0 &&
            std::memcmp(output.data(), before.data(), sizeof(output)) == 0,
            "original screen-relative movement retains its camera axes fallback");
    require(port_vr_pointer_ray(output.data(), &output[3]) == 0 &&
            std::memcmp(output.data(), before.data(), sizeof(output)) == 0,
            "original pointer hit-testing retains camera-to-cursor ray fallback");
    require(!port_vr_pointer_in_world() && !port_vr_diorama(),
            "handheld keeps original 2D cursor and full-screen game wipes");
    require(!port_vr_cull_view(nullptr, nullptr, nullptr) && !port_vr_move_axes(nullptr, nullptr, nullptr) &&
            !port_vr_pointer_ray(nullptr, nullptr), "unavailable headset views never dereference output pointers");
    int first, second;
    require(port_vr_transit_host() == nullptr, "no stale transit at startup");
    port_vr_transit_begin(&first);
    require(port_vr_transit_host() == &first, "game transit publishes exact owner identity");
    port_vr_transit_end(&second);
    require(port_vr_transit_host() == &first, "unrelated transit cannot end active launch or pipe");
    port_vr_transit_begin(&second);
    port_vr_transit_end(&first);
    require(port_vr_transit_host() == &second, "old owner cannot clear replacement transit");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    port_vr_pointer_reach(nan);
    port_vr_pointer_depth(-100.0f);
    port_vr_pointer_touched(~0ull);
    port_vr_wipe(-1, nan, ~0u);
    port_vr_pause_menu(-1);
    require(port_vr_transit_host() == &second && !port_vr_diorama() && !port_vr_pointer_in_world(),
            "unused headset overlay notifications cannot change handheld game-view state");
    port_vr_transit_end(&second);
    require(port_vr_transit_host() == nullptr, "matching owner clears game transit");
    port_vr_transit_begin(nullptr);
    port_vr_transit_end(nullptr);
    require(port_vr_transit_host() == nullptr, "null owner follows original transit semantics");
    std::printf("Vita flat game view: %u sanitized checks; no native pixels/gameplay proof\n", checks);
}
