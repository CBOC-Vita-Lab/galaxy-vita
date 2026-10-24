#pragma once

#include "port/input.h"

#ifdef __cplusplus
extern "C" {
#endif

int port_vita_input_init(void);
void port_vita_input_recenter(void);
void port_vita_input_poll(float elapsedSeconds, PortPadState* pad);
void port_vita_input_shutdown(void);

#ifdef __cplusplus
}
#endif
