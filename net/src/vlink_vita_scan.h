// Galaxy (mp74): finding a console that could only bind "port 0".
//
// Console log (c48, home Wi-Fi 192.168.2.71): every bind of a CHOSEN UDP port (41100..41107, any address, any option, any order) is refused with
// 0x8041010D (EACCES); a bind of port 0 works and the stack answers with a port in the 629xx range (62950, 62953, 62954 in three runs). A console
// that cannot bind a known port cannot be found by a broadcast to a known port. What it CAN do: send. So a console that ended up on a system-chosen
// port also sends its broadcasts (hello probe, room beacon) to a rotating slice of ports of the system's ephemeral range, nearest to its own port
// first (the stack hands out ports of one range, so a neighbour's port is near ours: 62950..62982 in all logs), the whole range in the end. Whoever's
// slice contains the other console's port is heard; the answer is unicast to the sender's port, as it always was. Consoles that did bind 41100.. are
// found by the fixed targets as before and never scan.
//
// Pure functions over a small state, so a host test can run two or three consoles against each other (tests/mp/vlink_scan_test.cpp).
#pragma once
#include <cstdint>

namespace VlinkScan {

constexpr uint16_t kLo = 49152, kHi = 65535;   // the ephemeral range of the stack (the logged ports are inside it)
constexpr int kSlicePorts = 32;                // ports a slice carries (one broadcast datagram each)
constexpr uint64_t kSliceMs = 250;             // at most one slice per this many ms: <= 128 small datagrams a second

struct Cursor {
    bool on = false;
    uint16_t own = 0;
    uint32_t idx = 0;          // position in the spiral
    uint64_t lastMs = 0;
    bool started = false;
    void Start(uint16_t ownPort)
    {
        on = ownPort != 0;
        own = ownPort < kLo ? kLo : ownPort;   // (a port below the range: the spiral starts at its edge)
        idx = 0;
        lastMs = 0;
        started = false;
    }
};

// position i of the spiral around own: own+1, own-1, own+2, own-2, ...; 0 when outside the range
inline uint16_t SpiralAt(uint16_t own, uint32_t i)
{
    const int32_t d = int32_t(i / 2) + 1;
    const int32_t p = (i & 1) ? int32_t(own) - d : int32_t(own) + d;
    return (p < kLo || p > kHi) ? uint16_t(0) : uint16_t(p);
}

// The ports of the next slice (nothing when the last slice was less than kSliceMs ago, or the cursor is off). Returns how many were written.
inline int NextSlice(Cursor& c, uint64_t nowMs, uint16_t* out, int max = kSlicePorts)
{
    if (!c.on) return 0;
    if (c.started && nowMs - c.lastMs < kSliceMs) return 0;
    c.started = true;
    c.lastMs = nowMs;
    const uint32_t total = 2u * uint32_t(kHi - kLo);   // (a bound: the spiral is longer than the range on both sides)
    int n = 0;
    for (uint32_t guard = 0; n < max && guard < total; ++guard) {
        const uint16_t p = SpiralAt(c.own, c.idx);
        c.idx = c.idx + 1 >= total ? 0 : c.idx + 1;
        if (p && p != c.own) out[n++] = p;
    }
    return n;
}

}  // namespace VlinkScan
