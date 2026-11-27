#pragma once
#include <cstdint>

namespace vgx::hrwpair {
// The FIFO carries only 24 data bits. Use a distinct kind rather than shrinking
// the 16-bit ownership ID used by ordinary, late and replay jobs.
constexpr uint32_t kMaybeFresh = 0x22, kPlain = 0x25, kPlainToken = 1u << 16;
constexpr uint32_t Marker(uint32_t id, bool plain) {
    return ((id & 0xffffu) << 8) | (plain ? kPlain : kMaybeFresh);
}
constexpr bool IsMarker(uint32_t word) {
    return (word & 0xffu) == kMaybeFresh || (word & 0xffu) == kPlain;
}
constexpr uint32_t Token(uint32_t word) {
    return ((word >> 8) & 0xffffu) | ((word & 0xffu) == kPlain ? kPlainToken : 0u);
}
constexpr bool NeedsDepth(bool armed, bool snapshot, bool freshEnabled, bool frozenPlain, bool plane, bool mask) {
    return armed && (snapshot || (freshEnabled && !frozenPlain) || plane || mask);
}
}
