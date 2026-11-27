// Galaxy 3.0 practice partner: what the partner leaves out of the session layer.
//  - the router port mapping (UPnP / NAT-PMP, mp_portmap.cpp): only a host by address asks for it, and the partner never hosts by address.
//    The five calls do nothing (state OFF), so the by-address connector links without np_upnp / np_natpmp / np_https_sys.
//  - the net glue's per-frame call (online play through the service: not in the partner).
#include "../../../platform/src/mp/mp_portmap.h"

#include <stdint.h>
#include <string.h>

extern "C" {
void mp_portmap_request(uint16_t, uint32_t, const char*) {}
void mp_portmap_release(void) {}
void mp_portmap_info(MpPortmapInfo* out) { memset(out, 0, sizeof *out); }
void mp_portmap_pump(void) {}
bool mp_portmap_busy(void) { return false; }
void mp_portmap_finish(unsigned) {}
void mp_netglue_frame(void) {}
// mp_boot.cpp times the boot data read (memfix); the partner has no game clock.
int64_t port_host_time_ns(void) { return 0; }
}
