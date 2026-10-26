#pragma once

#include "port/input.h"

#ifdef __cplusplus
extern "C" {
#endif

int port_vita_input_init(void);
void port_vita_input_recenter(void);
void port_vita_input_poll(float elapsedSeconds, PortPadState* pad);
void port_vita_input_shutdown(void);
// Only requests a reset. Consumed first by the next NORMAL poll, never by UI raw sampling.
void port_vita_input_settings_rearm(void);
int port_vita_input_raw_init(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "port/control_bindings.h"
struct PortVitaRawInput {
    uint64_t held = 0, pressed = 0, released = 0;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    port::controls::ControllerCaps caps;
    int source = -1; // selected SDK port; never combined with another controller
    unsigned controllerType = 0;
    uint64_t timestamp = 0;
};
// One owner at a time: startup UI, or running UI while host input HOLD is acknowledged.
bool port_vita_input_raw_poll(PortVitaRawInput&);
bool port_vita_input_mapping_available(void);
#endif
