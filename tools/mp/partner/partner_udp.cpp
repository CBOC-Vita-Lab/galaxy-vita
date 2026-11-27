// Galaxy 3.0 practice partner: the Play-by-Address UDP socket with the partner's own loss / delay on its sends.
// net/src/online/np_udp_posix.cpp (the unchanged POSIX socket) is compiled into this file under another name; CreatePlatformUdp() here
// wraps it with the fault shim (np_fault.cpp) when --loss / --dup / --delay / --jitter were given. The game's own build never sees this file.
#include "partner_impair.h"

#define CreatePlatformUdp CreatePlatformUdpRaw
#include "../../../net/src/online/np_udp_posix.cpp"
#undef CreatePlatformUdp

#include "../../../net/src/online/np_fault.h"

PartnerImpair g_partnerImpair;

namespace NpNet {
Udp* CreatePlatformUdp();
Udp* CreatePlatformUdp() {
    Udp* raw = CreatePlatformUdpRaw();
    if (!g_partnerImpair.any()) return raw;
    FaultConfig c;
    c.dropPct = g_partnerImpair.lossPct;
    c.dupPct = g_partnerImpair.dupPct;
    c.delayMs = g_partnerImpair.delayMs;
    c.jitterMs = g_partnerImpair.jitterMs;
    return CreateFaultUdp(raw, c, SteadyNowMs, 0, 0x5EED1234u);
}
}  // namespace NpNet
