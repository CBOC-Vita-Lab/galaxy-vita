// Galaxy 3.0 multiplayer: the queue of game messages that arrive before the scene is live, as a byte pool (optfix item 15; host tested in
// tests/mp/held_pool_test.cpp). It used to be 24 slots of a whole MpMsg (1028 bytes each, 24.7 KB of bss); most held messages are a few
// bytes (a switch edge is 4), so the entries are stored with their real length in an 8 KB pool: the same 24 messages when they are small,
// fewer only when they are big. First in, first out. No heap. A message that does not fit is refused (the caller counts and logs it,
// exactly as it did for a full queue).
#pragma once

#include <stdint.h>
#include <string.h>

#include "port/mp_proto.h"

namespace MpHeld {
    enum { POOL_BYTES = 8192, MAX_ENTRIES = 24, HEADER = 4 };

    class Pool {
    public:
        int count() const { return mCount; }
        uint32_t usedBytes() const { return mUsed; }
        void clear() { mCount = 0; mUsed = 0; mReadAt = 0; }

        // Stores a copy. False when the pool or the entry count is full (nothing stored).
        bool push(const MpMsg& m) {
            const uint32_t len = m.len <= MP_MAX_PAYLOAD ? m.len : MP_MAX_PAYLOAD;
            const uint32_t need = HEADER + len;
            if (mCount >= MAX_ENTRIES || mUsed + need > POOL_BYTES) return false;
            uint8_t* d = mPool + mUsed;
            d[0] = m.type;
            d[1] = m.fromSlot;
            d[2] = (uint8_t)(len & 0xFF);
            d[3] = (uint8_t)(len >> 8);
            memcpy(d + HEADER, m.data, len);
            mUsed += need;
            mCount++;
            return true;
        }

        // Removes (compacts: the bytes and entry slots are free again at once) the held entries whose type is in [lo, hi]; returns how many. A
        // restart of the scene: the world messages held from before it must not be applied after the fresh reply. Resets the read position.
        int dropRange(uint8_t lo, uint8_t hi) {
            int n = 0;
            uint32_t w = 0;
            for (uint32_t at = 0; at < mUsed;) {
                const uint32_t sz = HEADER + (uint32_t)(mPool[at + 2] | (mPool[at + 3] << 8));
                if (mPool[at] >= lo && mPool[at] <= hi) n++;
                else { if (w != at) memmove(mPool + w, mPool + at, sz); w += sz; }
                at += sz;
            }
            mUsed = w;
            mCount -= n;
            mReadAt = 0;
            return n;
        }

        // Reads the entries oldest first into `out` (a whole MpMsg the caller owns): rewind(), then while (next(&m)) ...; clear() after.
        void rewind() { mReadAt = 0; }
        bool next(MpMsg* out) {
            if (mReadAt >= mUsed) return false;
            const uint8_t* s = mPool + mReadAt;
            out->type = s[0];
            out->fromSlot = s[1];
            out->len = (uint16_t)(s[2] | (s[3] << 8));
            memcpy(out->data, s + HEADER, out->len);
            mReadAt += HEADER + out->len;
            return true;
        }

    private:
        uint8_t mPool[POOL_BYTES];
        uint32_t mUsed = 0, mReadAt = 0;
        int mCount = 0;
    };
}  // namespace MpHeld
