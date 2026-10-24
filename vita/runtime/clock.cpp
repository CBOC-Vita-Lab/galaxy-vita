#include "clock.h"

#include <limits.h>
#include <psp2/rtc.h>

extern "C" int port_vita_local_time_seconds(int64_t* seconds) {
    if (!seconds) return 0;
    SceDateTime local{};
    SceUInt64 value;
    if (sceRtcGetCurrentClockLocalTime(&local) < 0 || sceRtcGetTime64_t(&local, &value) < 0 || value > INT64_MAX)
        return 0;
    *seconds = int64_t(value);
    return 1;
}
