#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Local wall-clock fields interpreted as Unix seconds, like the Wii RTC.
int port_vita_local_time_seconds(int64_t* seconds);

#ifdef __cplusplus
}
#endif
