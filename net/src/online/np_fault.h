// Fault shim for the online transport: a Udp wrapper that loses, duplicates, delays and jitters the datagrams THIS console sends. It is the
// ioq3 / Xash style test injector, and it works on the console as well as on the desktop: SMG_NET_DROP=<percent>, SMG_NET_DUP=<percent>,
// SMG_NET_DELAY_MS=<ms>, SMG_NET_JITTER_MS=<ms> (environment on the desktop, np-player.txt keys on the Vita). Off unless one is set.
// Delayed datagrams leave when the transport next runs (every frame, and every received datagram), so on a console the delay is rounded up to
// the frame time. Applies to everything on the socket (STUN, punching, data), as a bad network would.
#ifndef NP_NET_FAULT_H
#define NP_NET_FAULT_H

#include "np_net.h"

namespace NpNet {

struct FaultConfig {
    double dropPct, dupPct;
    uint32_t delayMs, jitterMs;
    FaultConfig() : dropPct(0), dupPct(0), delayMs(0), jitterMs(0) {}
    bool Any() const { return dropPct > 0 || dupPct > 0 || delayMs || jitterMs; }
    static FaultConfig FromEnvironment();
};

// Wraps `inner` (takes ownership). `now`/`nowCtx` is the transport's clock; the random stream is seeded from `seed`.
Udp* CreateFaultUdp(Udp* inner, const FaultConfig& config, uint64_t (*now)(void*), void* nowCtx, uint32_t seed);

}  // namespace NpNet

#endif
