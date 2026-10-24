// Guest RAM classification and write generations (see vgx_guestmem.cpp).
#pragma once
#include <cstdint>

namespace vgx {
constexpr uint64_t kNotGuest = ~0ull;
// Sum of the write-generation counters under [p, p+n); kNotGuest when the
// range is not (entirely) inside registered guest RAM.
uint64_t GuestWriteGeneration(const void* p, uint32_t n);
bool IsGuestRam(const void* p, uint32_t n);
bool HaveGuestRam(); // has the runtime registered its memory banks?
// Changes whenever any write notification arrives (to detect a write that
// raced with a decode).
uint64_t GuestWriteEpoch();
// Whether a write notification numbered [from, to) (epochs, as GuestWriteEpoch counts them) touched any of
// the byte ranges, or cannot be told apart from one that did (the log of recent notifications is short).
// A decode that ran while unrelated data was flushed is not "raced"; only a write over what it read is.
struct HostRange { const void* p; uint32_t n; };
bool GuestWritesTouch(uint64_t from, uint64_t to, const HostRange* ranges, int count);
}
