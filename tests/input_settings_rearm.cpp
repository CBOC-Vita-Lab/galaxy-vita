#include "port/input.h"
#include "revolution/kpad.h"
#include "revolution/wpad.h"
#include <cassert>
#include <cstdint>
#include <cmath>
#include <initializer_list>
extern "C" {
int64_t port_host_time_ns() { return 1000000000; }
int port_skip_inject_a() { return -1; }
int port_handheld() { return 0; }
unsigned long long port_gx_recording_frame() { return 1; }
void port_log(const char*, ...) {}
int port_input_real_a(); int port_title_confirm(); int port_input_take_pause_request();
void port_input_use_tilt(float);
}
void test_tilt_consumers();
int main() {
    PortPadState p{}; p.connected = 1;
    p.buttons = 0x810; p.touched = 1; p.stickX = 1; p.pointerValid = 1;
    port_input_set(0, &p); port_input_set(1, &p);
    KPADStatus k{}; assert(KPADRead(0, &k, 1) == 1 && k.hold == 0x810 && k.trig == 0x810);
    p.buttons = 0; p.touched = 0; port_input_set(0, &p);
    p.buttons = 0x810; port_input_set(0, &p); // latch a parked-guest press
    p.buttons = 0; port_input_set(0, &p); // released before game reads
    port_input_settings_rearm();
    assert(!port_input_take_pause_request() && !port_input_real_a() && !port_title_confirm());
    uint32_t type; assert(WPADProbe(0, &type) == WPAD_ERR_NONE);
    assert(KPADRead(0, &k, 1) == 1 && !k.hold && !k.trig && !k.release && !k.dpd_valid_fg && !k.ex_status.fs.stick.x);
    assert(KPADRead(1, &k, 1) == 1 && k.hold == 0x810 && k.trig == 0x810);
    p = {}; p.connected = 1; p.buttons = 0x810; port_input_set(0, &p);
    assert(port_input_take_pause_request() && port_input_real_a() && port_title_confirm());
    assert(KPADRead(0, &k, 1) == 1 && k.hold == 0x810 && k.trig == 0x810);
    p.buttons = 0; port_input_set(0, &p); KPADRead(0, &k, 1); assert(k.release == 0x810);
    // Exercise the actual remote-gravity transform used by SphereAccelSensorController
    // (neutral pitch 80) and SurfRay (pitch 0), retaining a finite unit vector.
    for (float pitch : {0.0f, 80.0f}) for (int sign : {-1, 1}) {
        p = {}; p.connected = p.tilted = 1; p.downX = sign * .65f; p.downY = -std::sqrt(1-.65f*.65f);
        port_input_use_tilt(pitch); port_input_set(0, &p); KPADRead(0, &k, 1);
        assert(std::fabs(k.acc_value - 1) < 1e-5f && k.acc.x * sign < 0);
        // WPadAcceleration::update negates acc.x; a right stick yields rightward steering.
        assert((-k.acc.x) * sign > 0);
    }
    test_tilt_consumers();
}
