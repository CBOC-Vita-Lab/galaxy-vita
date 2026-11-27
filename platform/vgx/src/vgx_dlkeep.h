// GXV_DL_KEEP_MIB (strk26; vgx_dlcache.cpp Sweep): which of the display-list entries that are due (unused for kRetainFrames) go and which stay.
// Pure logic, so that a host test can check it (tests/vgx_dlkeep_test.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

namespace vgx {
namespace dlkeep {

struct Due {
    uint64_t lastUse;
    uint64_t bytes;
};
// `due` is sorted by lastUse, oldest first; dueBytes is their sum. Returns how many of the oldest go: none when the cap is 0 (the rule is off: the caller lets
// every due entry go as before, it does not ask), none while the due entries fit under the cap, otherwise the oldest until what is left fits, but never more
// than perSweep in one sweep and never all of them (an over-cap sweep that would clear everything lets the newest stay: the next sweep goes on).
inline size_t EvictCount(const Due* due, size_t n, uint64_t dueBytes, uint64_t cap, size_t perSweep) {
    if (cap == 0 || n == 0 || dueBytes <= cap) return 0;
    uint64_t over = dueBytes - cap;
    size_t goes = 0;
    while (goes < n && goes < perSweep && over > 0) {
        over = due[goes].bytes >= over ? 0 : over - due[goes].bytes;
        ++goes;
    }
    if (goes >= n && n > 0) goes = n - 1;
    return goes;
}

} // namespace dlkeep
} // namespace vgx
